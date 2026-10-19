// SPDX-License-Identifier: GPL-2.0-or-later
// The game's Options menu "Graphics" tab: the menu's fifth tab (Language Select, hidden by the
// game in this region) is shown again and opens the port's settings menu instead of the language
// screen. Its label comes from scripts/graphics_tab.py (relabelled menu text in update3.pak).
//
// Changes to the game's code (CTR NF 1.21, image offsets), visual-script nodes:
//  - CGuiNodes::open2_VSC (0x1109200), which opens a menu project: the tab's script asks for a
//    project that does not exist (scripts/graphics_tab.py renames it), which opens the settings
//    menu instead. Every other project opens as before.
// The tab is shown by the root script's own change (its hide names nothing, graphics_tab.py).
// The first calls of each are logged with the object names found ("Graphics tab: ...").
#include "bbport_graphics_tab.h"
#include "bbport_settings.h"
#include "common/types.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c): run.py starts the game again

namespace BbOverlay {
void OpenMenu(); // bbport_overlay.h (Vulkan headers)
}

namespace BbGraphicsTab {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// regionSwitch_VSC: `call 0x1036010` (the region query filling its three output flags).
constexpr u64 RegionQueryCall = 0x10de0c9;
constexpr u8 RegionQueryCode[5] = {0xe8, 0x42, 0x7f, 0xf5, 0xff};

/// open2_VSC's prologue (push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12).
constexpr u64 Open2 = 0x1109200;
constexpr u8 Open2Prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
/// The pin getter both use for their object input: (context, pin, out object), which returns
/// the object with a reference taken; the release (0x150a790) drops it.
constexpr u64 PinGetter = 0x4708a0;
constexpr u64 Release = 0x150a790;

constexpr char Project[] = "Octane_Options_LanguageSelect";
/// hidePlaceable_VSC / showPlaceable_VSC (same prologue: push rbp; mov rbp,rsp; push r15;
/// push r14; push rbx; sub rsp,0x18). The game's menu objects are its .igz data in place: the
/// fifth tab's placeable (ui/Octane_Options_Menu.igz, 1.21 update) has the position (40, 420)
/// at +0x60, which no other placeable there has.
constexpr u64 Hide = 0x26f6b0, Show = 0x26f740;
constexpr u8 PlaceablePrologue[13] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                      0x56, 0x53, 0x48, 0x83, 0xec, 0x18};
constexpr float TabPosition[2] = {40.0f, 420.0f};

using VscFn = u64 PS4_SYSV_ABI (*)(void*, void*, void*, void*, void*, void*);
using GetterFn = void PS4_SYSV_ABI (*)(void*, void*, void**);
using ReleaseFn = void PS4_SYSV_ABI (*)(void*);

u8* image = nullptr;
VscFn open2_original = nullptr, hide_original = nullptr, show_original = nullptr;
std::atomic<int> open_logged{0}, placeable_logged{0};
std::mutex last_mutex;
std::string last_opened;

bool SafeRead(const void* address, void* out, std::size_t size) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        static_cast<const u8*>(address) + size >
            static_cast<const u8*>(mbi.BaseAddress) + mbi.RegionSize) {
        return false;
    }
    std::memcpy(out, address, size);
    return true;
#else
    iovec local{out, size}, remote{const_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
#endif
}

/// The node's object input (its first pin), with the getter's reference dropped again.
void* Input(void* context, void* args) {
    void* object = nullptr;
    void** pins = nullptr;
    void* pin = nullptr;
    if (args && SafeRead(static_cast<u8*>(args) + 0x10, &pins, sizeof(pins)) && pins &&
        SafeRead(pins, &pin, sizeof(pin)) && pin) {
        reinterpret_cast<GetterFn>(image + PinGetter)(context, pin, &object);
        if (object) {
            reinterpret_cast<ReleaseFn>(image + Release)(object);
        }
    }
    return object;
}

/// An identifier-like string at `address` (at least 4 characters: letters, digits, '_').
bool Identifier(u64 address, std::string& out) {
    char text[96] = {};
    if (address < 0x10000 || (address & 0xffff000000000000ull) ||
        !SafeRead(reinterpret_cast<void*>(address), text, 64)) {
        return false;
    }
    std::size_t n = 0;
    while (text[n] && (std::isalnum(static_cast<unsigned char>(text[n])) || text[n] == '_')) {
        ++n;
    }
    if (n < 4 || text[n] != 0) {
        return false;
    }
    out = text;
    return true;
}

/// The object's name (igNamedObject::_name and the like): a string a field among the object's
/// first 32 points to, else one a field of a pointed-to object points to (one level down).
/// `where` gets the field offset (one level down: offset * 0x1000 + inner offset).
std::string Name(void* object, int* where = nullptr) {
    u64 fields[32];
    if (!object || !SafeRead(object, fields, sizeof(fields))) {
        return {};
    }
    std::string text;
    for (int i = 1; i < 32; ++i) {
        if (Identifier(fields[i], text)) {
            if (where) *where = i * 8;
            return text;
        }
    }
    for (int i = 1; i < 32; ++i) {
        u64 inner[16];
        if (fields[i] < 0x10000 || (fields[i] & 0xffff000000000000ull) ||
            !SafeRead(reinterpret_cast<void*>(fields[i]), inner, sizeof(inner))) {
            continue;
        }
        for (int j = 0; j < 16; ++j) {
            if (Identifier(inner[j], text)) {
                if (where) *where = i * 8 * 0x1000 + j * 8;
                return text;
            }
        }
    }
    return {};
}

/// The first calls: the raw object and its first fields, for finding the name field.
void Dump(const char* what, void* args, void* object) {
    static std::atomic<int> dumped{0};
    if (dumped.fetch_add(1) >= 12) {
        return;
    }
    u64 fields[8] = {};
    const bool ok = object && SafeRead(object, fields, sizeof(fields));
    std::printf("Graphics tab: %s args=%p object=%p%s", what, args, object, ok ? " fields" : "");
    for (int i = 0; ok && i < 8; ++i) {
        std::printf(" %llx", static_cast<unsigned long long>(fields[i]));
    }
    std::printf("\n");
}

/// A placeable's state for the log: position (+0x60), flags (+0xc), alpha (+0xe0).
bool IsTab(void* object, const char* what) {
    struct {
        u8 head[0x60];
        float position[2];
    } p{};
    u32 flags = 0;
    float alpha = -1.0f;
    if (!object || !SafeRead(object, &p, sizeof(p))) {
        return false;
    }
    SafeRead(static_cast<u8*>(object) + 0xc, &flags, sizeof(flags));
    SafeRead(static_cast<u8*>(object) + 0xe0, &alpha, sizeof(alpha));
    const bool tab = p.position[0] == TabPosition[0] && p.position[1] == TabPosition[1];
    if (placeable_logged.fetch_add(1) < 80 || tab) {
        std::printf("Graphics tab: %s %p at (%g, %g) flags %x alpha %g%s\n", what, object,
                    p.position[0], p.position[1], flags, alpha, tab ? " <- the tab" : "");
        std::fflush(stdout);
    }
    return tab;
}

/// The language screen's project (ui/Octane_Options_LanguageSelect.igz, loaded in place): its
/// igGuiProject has pointers to its own data at +0x10, +0x18, +0x38 and +0xa0, at these distances.
bool IsLanguageProject(void* object) {
    u64 f[0x15] = {};
    if (!object || !SafeRead(object, f, sizeof(f))) {
        return false;
    }
    const u64 o = reinterpret_cast<u64>(object);
    return f[2] == o + 0xf0 && f[3] == o + 0x210 && f[7] == o + 0x5d0 && f[0x14] == o + 0x610;
}

/// A readable text pointer: printable characters up to a terminator.
bool IsText(u64 address, std::string* out = nullptr) {
    char text[48] = {};
    if (address < 0x10000 || (address >> 48) || !SafeRead(reinterpret_cast<void*>(address), text, 40)) {
        return false;
    }
    std::size_t n = 0;
    while (n < 40 && text[n] >= 0x20 && text[n] < 0x7f) {
        ++n;
    }
    if (n < 3 || text[n] != 0) {
        return false;
    }
    if (out) {
        *out = text;
    }
    return true;
}

/// The navigation item among a placeable's behaviors (+0xe8: igGuiBehaviorList, its data at +0x20,
/// count at +0x10): the one that points back at the placeable's neighbours (+0x110, +0x118).
u64 NavItem(u64 placeable, u64* prev = nullptr) {
    u64 list = 0, count = 0, items = 0;
    if (!SafeRead(reinterpret_cast<void*>(placeable + 0xe8), &list, 8) || !list ||
        !SafeRead(reinterpret_cast<void*>(list + 0x10), &count, 8) ||
        !SafeRead(reinterpret_cast<void*>(list + 0x20), &items, 8) || !items) {
        return 0;
    }
    count &= 0xffffffff;
    for (u64 i = 0; i < count && i < 8; ++i) {
        u64 b = 0, links[2] = {};
        if (SafeRead(reinterpret_cast<void*>(items + 8 * i), &b, 8) && b &&
            SafeRead(reinterpret_cast<void*>(b + 0x110), links, sizeof(links)) && links[0] && links[1]) {
            if (prev) {
                *prev = links[1];
            }
            return b;
        }
    }
    return 0;
}

// The game's strings: interned in its string pool (pointer at 0x2af2600) by 0x1574080 (pool,
// text), which returns a string with a reference taken (count at -8); 0x1518e30 (pool, string -
// 0x10) drops one. A text placeable shows the string at +0x30 (CGuiBehaviorLanguageItem's init,
// 0x1276c70, sets it so and marks the placeable shown: +0xc |= 0x1000).
constexpr u64 StringPool = 0x2af2600, Intern = 0x1574080, Unref = 0x1518e30;
using InternFn = char* PS4_SYSV_ABI (*)(void*, const char*);
using UnrefFn = void PS4_SYSV_ABI (*)(void*, void*);

void* Pool() {
    return *reinterpret_cast<void**>(image + StringPool);
}

char* GameString(const char* text) {
    return reinterpret_cast<InternFn>(image + Intern)(Pool(), text);
}

void DropString(char* string) {
    if (string) {
        reinterpret_cast<UnrefFn>(image + Unref)(Pool(), string - 0x10);
    }
}

/// A string field the game owns: the new text replaces the old (whose reference is dropped).
void SetStringField(u64 address, const char* text) {
    char** field = reinterpret_cast<char**>(address);
    char* old = *field;
    *field = GameString(text);
    DropString(old);
}

void SetText(u64 placeable, const char* text) {
    if (!placeable) {
        return;
    }
    SetStringField(placeable + 0x30, text);
    *reinterpret_cast<u32*>(placeable + 0xc) |= 0x1000;
}

/// The tab's label: its navigation item (CGuiBehaviorNavigationItem: _itemText +0xe8,
/// _textPlaceable +0xf0, from the game's field tables) gets "Graphics" (the text table has none
/// for it in this region, so the button keeps its template's "Crash Cove").
void Relabel(void* tab) {
    const u64 item = NavItem(reinterpret_cast<u64>(tab));
    if (!item) {
        std::printf("Graphics tab: label: no navigation item\n");
        return;
    }
    SetStringField(item + 0xe8, "Graphics");
    SetText(*reinterpret_cast<u64*>(item + 0xf0), "Graphics");
    std::printf("Graphics tab: labelled\n");
    std::fflush(stdout);
}

// ---- The Graphics page: the language screen's rows, relabelled as settings -----------------
// CGuiBehaviorLanguageItem (size 0x38): +0x10 its text placeable, +0x28 its language. Its event
// handler (0x1276c70: this, event, ...) fills the row on the list's init event (event class at
// 0x2a21db0) and, on the input event class (0x2a21d10) with type 8 (+0x18), sets the language
// and opens the "Language Selected" dialog. Here: each row shows a setting; pressing changes it.
constexpr u64 LanguageItemEvent = 0x1276c70;
constexpr u64 InitEventMeta = 0x2a21db0, InputEventMeta = 0x2a21d10;
VscFn language_item_original = nullptr;

bool IsA(void* event, u64 meta_global) {
    using MetaFn = u64 PS4_SYSV_ABI (*)(void*);
    const u64 target = *reinterpret_cast<u64*>(image + meta_global);
    u64 meta = reinterpret_cast<MetaFn>(**reinterpret_cast<u64**>(event))(event);
    for (int depth = 0; meta && depth < 16; ++depth) {
        if (meta == target) {
            return true;
        }
        meta = *reinterpret_cast<u64*>(meta + 0x40);
    }
    return false;
}

std::mutex rows_mutex;
std::vector<u64> rows; ///< the language items, in the order the list filled them

const char* OnOff(bool on) {
    return on ? "On" : "Off";
}

/// Row `index`'s text; with `change`, its setting moves to the next value first.
std::string Row(std::size_t index, bool change) {
    auto& v = BbSettings::Get();
    char text[96];
    switch (index) {
    case 0: {
        int r = v.render_res.load();
        if (change) {
            r = r < 0 ? 2 : (r + 1) % BbSettings::RenderResCount;
            v.render_res = r;
        }
        std::snprintf(text, sizeof(text), "Resolution: %dp", r < 0 ? 1080 : BbSettings::RenderResHeights[r]);
        return text;
    }
    case 1: {
        int t = v.temporal.load();
        if (change) {
            t = (t + 1) % BbSettings::TemporalCount;
            v.temporal = t;
        }
        static const char* names[] = {"Off", "FSR 1", "FSR 3", "DLSS", "FSR 4"};
        std::snprintf(text, sizeof(text), "Upscaling: %s",
                      names[unsigned(t) < std::size(names) ? t : 0]);
        return text;
    }
    case 2: {
        int q = v.dlss_quality.load();
        if (change) {
            q = (q + 1) % BbSettings::DlssQualityCount;
            v.dlss_quality = q;
        }
        static const char* names[] = {"Auto", "DLAA", "Quality", "Balanced", "Performance", "Ultra Perf."};
        std::snprintf(text, sizeof(text), "Upscale Quality: %s", names[q % 6]);
        return text;
    }
    case 3:
    case 4: {
        auto& patch = v.game_patches[index == 3 ? 0 : 1];
        if (change) {
            patch = !patch.load();
        }
        // Row 4 is the game's motion blur: on unless the patch removes it.
        std::snprintf(text, sizeof(text), index == 3 ? "FPS Unlock: %s" : "Motion blur: %s",
                      OnOff(index == 3 ? patch.load() : !patch.load()));
        return text;
    }
    case 5: {
        static const int limits[] = {30, 60, 120, 144, 240, 0};
        int limit = v.fps_limit.load(), k = 0;
        while (k < 6 && limits[k] != limit) {
            ++k;
        }
        if (change) {
            limit = limits[(k + 1) % 6];
            v.fps_limit = limit;
        }
        if (limit) {
            std::snprintf(text, sizeof(text), "FPS limit: %d", limit);
        } else {
            std::snprintf(text, sizeof(text), "FPS limit: Off");
        }
        return text;
    }
    default:
        return "";
    }
}

/// Settings on the page that apply only after a restart (as the settings menu counts them).
bool NeedsRestart() {
    auto& v = BbSettings::Get();
    bool restart = BbSettings::ResolutionNeedsRestart() || BbSettings::RenderSizeNeedsRestart();
    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        restart |= v.game_patches[g] != v.startup_game_patches[g];
    }
    restart |= v.render_res != v.startup_render_res || v.temporal != v.startup_temporal ||
               v.dlss_quality != v.startup_dlss_quality;
    return restart;
}

constexpr std::size_t SettingRows = 6;  ///< rows 0..5: the settings
constexpr std::size_t RestartRow = 6;   ///< "Restart to apply", shown while a change waits

/// A row's visibility: its placeable (CGuiBehaviorLanguageItem::_placeable, +0x30) and text
/// placeable (+0x10), with hidePlaceable/showPlaceable's flags (+0xc: 0x1400 hidden, 0x1000 shown).
void ShowRow(u64 item, bool shown) {
    // The row's own placeable is the text placeable's parent (igGuiPlaceable +0x40): the bar.
    u64 text = 0, parent = 0;
    SafeRead(reinterpret_cast<void*>(item + 0x10), &text, 8);
    if (text) {
        SafeRead(reinterpret_cast<void*>(text + 0x40), &parent, 8);
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        std::printf("Graphics tab: row %llx text %llx parent %llx %s\n",
                    static_cast<unsigned long long>(item), static_cast<unsigned long long>(text),
                    static_cast<unsigned long long>(parent), shown ? "shown" : "hidden");
    }
    for (u64 placeable : {parent, text}) {
        if (!placeable) {
            continue;
        }
        u32& flags = *reinterpret_cast<u32*>(placeable + 0xc);
        flags = shown ? ((flags & ~0x1400u) | 0x1000u) : (flags | 0x1400u);
        *reinterpret_cast<float*>(placeable + 0xe0) = shown ? 1.0f : 0.0f; // _alpha
    }
}

void UpdateRestartRow() {
    u64 item = 0;
    {
        std::scoped_lock lock{rows_mutex};
        if (rows.size() > RestartRow) {
            item = rows[RestartRow];
        }
    }
    if (item) {
        const bool needed = NeedsRestart();
        SetText(*reinterpret_cast<u64*>(item + 0x10), needed ? "Restart to apply" : " ");
        ShowRow(item, needed);
    }
}

u64 PS4_SYSV_ABI LanguageItemHook(void* self, void* event, void* a2, void* a3, void* a4, void* a5) {
    const u64 item = reinterpret_cast<u64>(self);
    if (event && IsA(event, InputEventMeta)) {
        const u32 type = *reinterpret_cast<u32*>(static_cast<u8*>(event) + 0x18);
        std::size_t index = SIZE_MAX;
        {
            std::scoped_lock lock{rows_mutex};
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i] == item) {
                    index = i;
                }
            }
        }
        if (index == RestartRow && type == 8 && NeedsRestart()) {
            BbSettings::Save();
            std::printf("Graphics tab: restarting to apply\n");
            std::fflush(stdout);
            runtime_restart();
            return 0;
        }
        if (index != SIZE_MAX && index >= SettingRows) {
            return 0; // the rows past the settings do nothing
        }
        if (index != SIZE_MAX && type == 8) {
            const std::string text = Row(index, true);
            SetText(*reinterpret_cast<u64*>(item + 0x10), text.c_str());
            BbSettings::Save();
            UpdateRestartRow();
            std::printf("Graphics tab: %s\n", text.c_str());
            std::fflush(stdout);
            return 0; // no language change, no dialog
        }
        if (index != SIZE_MAX && type != 8) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 20) {
                std::printf("Graphics tab: row %zu input %u\n", index, type);
            }
        }
        return language_item_original(self, event, a2, a3, a4, a5);
    }
    const u64 result = language_item_original(self, event, a2, a3, a4, a5);
    if (event && IsA(event, InitEventMeta)) {
        std::size_t index;
        {
            std::scoped_lock lock{rows_mutex};
            index = std::find(rows.begin(), rows.end(), item) - rows.begin();
            if (index == rows.size()) {
                rows.push_back(item);
            }
        }
        const std::string text = Row(index, false);
        SetText(*reinterpret_cast<u64*>(item + 0x10), text.empty() ? " " : text.c_str());
        if (index == RestartRow) {
            UpdateRestartRow();
        } else if (index > RestartRow) {
            ShowRow(item, false); // no setting: the row is hidden
        }
        std::printf("Graphics tab: row %zu \"%s\"\n", index, text.c_str());
        std::fflush(stdout);
    }
    return result;
}

u64 PS4_SYSV_ABI HideHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    void* object = Input(context, args);
    if (IsTab(object, "hide")) {
        Relabel(object);
        return 0; // stays shown
    }
    return hide_original(context, args, a2, a3, a4, a5);
}

u64 PS4_SYSV_ABI ShowHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    IsTab(Input(context, args), "show");
    return show_original(context, args, a2, a3, a4, a5);
}

u64 PS4_SYSV_ABI Open2Hook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    int where = -1;
    void* object = Input(context, args);
    Dump("open", args, object);
    const std::string name = Name(object, &where);
    // The tab's script (relabelled by scripts/graphics_tab.py) names a project that does not
    // exist: its input comes back empty. Every other menu opens with its project.
    const bool ours = !object || name == Project || IsLanguageProject(object);
    if (open_logged.fetch_add(1) < 40) {
        std::printf("Graphics tab: open %s (+0x%x)%s\n", name.empty() ? "(no name)" : name.c_str(),
                    where, ours ? " -> settings menu" : "");
        std::fflush(stdout);
    }
    {
        std::scoped_lock lock{last_mutex};
        last_opened = name;
    }
    if (ours) {
        // The Graphics page: the language screen opens; its rows are filled again.
        std::scoped_lock lock{rows_mutex};
        rows.clear();
    }
    return open2_original(context, args, a2, a3, a4, a5);
}

void* AllocateCode(std::size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

/// movabs rax, target; jmp rax (12 bytes).
void WriteJump(u8* at, const void* target) {
    at[0] = 0x48;
    at[1] = 0xb8;
    const u64 t = reinterpret_cast<u64>(target);
    std::memcpy(at + 2, &t, 8);
    at[10] = 0xff;
    at[11] = 0xe0;
}

/// The function at `offset` jumps to `hook`; the returned stub runs the moved prologue and
/// continues in the function.
VscFn Detour(u8* stub, u64 offset, const u8* prologue, std::size_t size, const void* hook) {
    std::memcpy(stub, prologue, size);
    WriteJump(stub + size, image + offset + size);
    WriteJump(image + offset, hook);
    return reinterpret_cast<VscFn>(stub);
}
} // namespace

void PatchImage(unsigned char* base, std::uint64_t size) {
    if (!BbSettings::Get().graphics_tab) {
        return;
    }
    if (Open2 + sizeof(Open2Prologue) > size ||
        std::memcmp(base + RegionQueryCall, RegionQueryCode, sizeof(RegionQueryCode)) ||
        std::memcmp(base + Open2, Open2Prologue, sizeof(Open2Prologue))) {
        std::printf("Graphics tab: unexpected game code, off\n");
        return;
    }
    u8* stubs = static_cast<u8*>(AllocateCode(4096));
    if (!stubs) {
        std::printf("Graphics tab: no code memory, off\n");
        return;
    }
    image = base;
    open2_original = Detour(stubs, Open2, Open2Prologue, sizeof(Open2Prologue),
                            reinterpret_cast<const void*>(&Open2Hook));
    if (!std::memcmp(base + Hide, PlaceablePrologue, sizeof(PlaceablePrologue)) &&
        !std::memcmp(base + Show, PlaceablePrologue, sizeof(PlaceablePrologue))) {
        hide_original = Detour(stubs + 64, Hide, PlaceablePrologue, sizeof(PlaceablePrologue),
                               reinterpret_cast<const void*>(&HideHook));
        show_original = Detour(stubs + 128, Show, PlaceablePrologue, sizeof(PlaceablePrologue),
                               reinterpret_cast<const void*>(&ShowHook));
    }
    if (!std::memcmp(base + LanguageItemEvent, Open2Prologue, sizeof(Open2Prologue))) {
        language_item_original = Detour(stubs + 192, LanguageItemEvent, Open2Prologue,
                                        sizeof(Open2Prologue),
                                        reinterpret_cast<const void*>(&LanguageItemHook));
    }
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stubs, 256);
#endif
    std::printf("Graphics tab: on (Options menu, fifth tab)\n");
}

} // namespace BbGraphicsTab
