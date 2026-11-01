// SPDX-License-Identifier: GPL-2.0-or-later
// CTR-NitroFueled-PC launcher (Windows). Sets up the game folder (the 1.21 update merged over
// the base game with hard links), edits the settings the game reads (bbport.ini, the same keys
// as the in-game menu) and starts run.py with the bundled Python, showing its output.
// Dear ImGui on Win32 + Direct3D 11, linked statically: one .exe next to run.py and bin\.
//
// Files: launcher.ini (this launcher: game folders, language, display...) and bbport.ini (the
// game's settings) in the port folder; run.py writes last_run.log there.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cwctype>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "discord_link.h"
#include "nat_check.h"
#include "port_update.h"
#include "settings_import.h"
#include "discord_presence.h"
#include "launcher_locales.h"

namespace fs = std::filesystem;

namespace {

// The Americas build (CUSA13795) and the European one (CUSA14876, SacredFire EU): the port runs both.
constexpr const char* kTitleIds[] = {"CUSA13795", "CUSA14876"};
bool IsCtrTitle(const std::string& title) {
    for (const char* id : kTitleIds)
        if (title == id) return true;
    return false;
}
constexpr const char* kUpdateVersion = "01.21";
constexpr const char* kRepoUrl = "https://github.com/GetLostIn/CTR-NitroFueled-PC";
// The 1.21 update alone is about 8.5 GB; base game + update is much larger.
constexpr std::uintmax_t kCompleteGameMinBytes = 10ull << 30;

struct LauncherArtwork {
    ID3D11ShaderResourceView* texture = nullptr;
    UINT width = 0;
    UINT height = 0;
};
LauncherArtwork g_hero_art;
LauncherArtwork g_logo_art;
LauncherArtwork g_menu_background;
ImFont* g_ctr_font = nullptr;
RECT g_titlebar_drag{};
float g_ui_scale = 1.0f;
extern ID3D11Device* g_device;

// ---------------------------------------------------------------------------------------------
// Text helpers

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string PathText(const fs::path& p) { return Narrow(p.wstring()); }
fs::path TextPath(const std::string& s) { return fs::path(Widen(s)); }

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteTextAtomic(const fs::path& p, const std::string& text) {
    fs::path temp = p;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string FormatBytes(std::uintmax_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
    return buf;
}

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kRed{1.0f, 0.45f, 0.40f, 1.0f};
// The game found no Vulkan 1.3 (GPU, driver or loader): UnsupportedVulkanExitCode in vk_platform.h.
constexpr DWORD kUnsupportedGpuCode = 24;
const ImVec4 kYellow{1.0f, 0.80f, 0.35f, 1.0f};
const ImVec4 kGrey{0.65f, 0.65f, 0.68f, 1.0f};
// The Achievements page (TrophiesTab, from GetLostIn's launcher v0.4, GPL-2.0) draws with its
// cards, colours and heading font; its icon-font glyphs are left out (this launcher has none).
const ImVec4 kText{0.92f, 0.93f, 0.96f, 1.0f};
const ImVec4 kBlue{0.30f, 0.64f, 1.0f, 1.0f};
const ImVec4 kCard{0.118f, 0.133f, 0.188f, 1.0f};
const ImVec4 kFrame{0.149f, 0.169f, 0.231f, 1.0f};
const ImVec4 kBorder{0.173f, 0.192f, 0.259f, 1.0f};
#define ICON_INFO ""
#define ICON_CHECK ""
#define ICON_FOLDER ""
#define ICON_UNDO ""
#define ICON_STAR ""
ImFont* g_font_heading = nullptr;
ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
/// A rounded panel that grows with its content.
void BeginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * g_ui_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_ui_scale, 12.0f * g_ui_scale));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------------------------
// Custom Racing UI Themes (Nitro Flame, Oxide Neon, Aku Crimson, Midnight Cobalt)

struct UiTheme {
    const char* name_en;
    const char* name_es;
    ImVec4 win_bg;
    ImVec4 nav_bg;
    ImVec4 content_bg;
    ImVec4 popup_bg;
    ImVec4 titlebar_bg;
    ImVec4 frame_bg;
    ImVec4 frame_hover;
    ImVec4 frame_active;
    ImVec4 button_bg;
    ImVec4 button_hover;
    ImVec4 button_active;
    ImVec4 accent;        // Primary flame/neon accent
    ImVec4 accent_alt;    // Secondary gold/highlight accent
    ImVec4 border;
};

int g_ui_theme = 0;

constexpr UiTheme kThemes[4] = {
    // 0: Nitro Flame (Obsidian Carbon + Nitro Orange & Trophy Gold)
    {"Nitro Flame", "Llama Nitro",
     {0.055f, 0.063f, 0.086f, 1.00f},
     {0.067f, 0.075f, 0.106f, 0.92f},
     {0.082f, 0.094f, 0.129f, 0.90f},
     {0.075f, 0.086f, 0.118f, 0.98f},
     {0.047f, 0.055f, 0.075f, 1.00f},
     {0.125f, 0.141f, 0.192f, 0.92f},
     {0.180f, 0.200f, 0.267f, 1.00f},
     {0.235f, 0.255f, 0.333f, 1.00f},
     {0.165f, 0.180f, 0.243f, 0.96f},
     {0.920f, 0.380f, 0.090f, 1.00f},
     {1.000f, 0.560f, 0.140f, 1.00f},
     {1.000f, 0.420f, 0.100f, 1.00f},
     {1.000f, 0.740f, 0.180f, 1.00f},
     {1.000f, 0.520f, 0.160f, 0.32f}},
    // 1: Oxide Neon (Deep Void + Toxic Emerald & Hyper Teal)
    {"Oxide Neon", "Neón Oxide",
     {0.047f, 0.059f, 0.082f, 1.00f},
     {0.059f, 0.078f, 0.106f, 0.92f},
     {0.071f, 0.098f, 0.129f, 0.90f},
     {0.063f, 0.086f, 0.114f, 0.98f},
     {0.039f, 0.051f, 0.071f, 1.00f},
     {0.106f, 0.149f, 0.192f, 0.92f},
     {0.145f, 0.216f, 0.267f, 1.00f},
     {0.184f, 0.275f, 0.333f, 1.00f},
     {0.129f, 0.184f, 0.235f, 0.96f},
     {0.120f, 0.760f, 0.420f, 1.00f},
     {0.200f, 0.900f, 0.520f, 1.00f},
     {0.180f, 0.900f, 0.450f, 1.00f},
     {0.000f, 0.850f, 0.750f, 1.00f},
     {0.180f, 0.900f, 0.450f, 0.32f}},
    // 2: Aku Crimson (Volcanic Dark + Aku Ruby & Ancient Gold)
    {"Aku Crimson", "Carmesí Aku",
     {0.075f, 0.047f, 0.059f, 1.00f},
     {0.094f, 0.059f, 0.075f, 0.92f},
     {0.114f, 0.075f, 0.094f, 0.90f},
     {0.102f, 0.067f, 0.082f, 0.98f},
     {0.063f, 0.039f, 0.051f, 1.00f},
     {0.176f, 0.114f, 0.141f, 0.92f},
     {0.243f, 0.157f, 0.192f, 1.00f},
     {0.310f, 0.200f, 0.243f, 1.00f},
     {0.212f, 0.137f, 0.169f, 0.96f},
     {0.880f, 0.180f, 0.280f, 1.00f},
     {1.000f, 0.280f, 0.380f, 1.00f},
     {1.000f, 0.220f, 0.320f, 1.00f},
     {1.000f, 0.760f, 0.220f, 1.00f},
     {1.000f, 0.260f, 0.360f, 0.34f}},
    // 3: Midnight Cobalt (Deep Slate Navy + Electric Cyan & Plasma Amber)
    {"Midnight Cobalt", "Cobalto Nocturno",
     {0.047f, 0.071f, 0.118f, 1.00f},
     {0.059f, 0.090f, 0.149f, 0.92f},
     {0.075f, 0.114f, 0.184f, 0.90f},
     {0.067f, 0.102f, 0.165f, 0.98f},
     {0.039f, 0.059f, 0.098f, 1.00f},
     {0.114f, 0.169f, 0.267f, 0.92f},
     {0.157f, 0.231f, 0.357f, 1.00f},
     {0.200f, 0.294f, 0.443f, 1.00f},
     {0.141f, 0.208f, 0.322f, 0.96f},
     {0.180f, 0.560f, 0.960f, 1.00f},
     {0.280f, 0.680f, 1.000f, 1.00f},
     {0.240f, 0.660f, 1.000f, 1.00f},
     {1.000f, 0.680f, 0.200f, 1.00f},
     {0.240f, 0.660f, 1.000f, 0.34f}},
};

const UiTheme& CurrentTheme() {
    return kThemes[std::clamp(g_ui_theme, 0, 3)];
}

void ApplyUiTheme() {
    if (!ImGui::GetCurrentContext()) return;
    const UiTheme& th = CurrentTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.64f, 0.72f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = th.win_bg;
    style.Colors[ImGuiCol_ChildBg] = th.nav_bg;
    style.Colors[ImGuiCol_PopupBg] = th.popup_bg;
    style.Colors[ImGuiCol_Border] = th.border;
    style.Colors[ImGuiCol_FrameBg] = th.frame_bg;
    style.Colors[ImGuiCol_FrameBgHovered] = th.frame_hover;
    style.Colors[ImGuiCol_FrameBgActive] = th.frame_active;
    style.Colors[ImGuiCol_Button] = th.button_bg;
    style.Colors[ImGuiCol_ButtonHovered] = th.button_hover;
    style.Colors[ImGuiCol_ButtonActive] = th.button_active;
    style.Colors[ImGuiCol_CheckMark] = th.accent_alt;
    style.Colors[ImGuiCol_SliderGrab] = th.accent;
    style.Colors[ImGuiCol_SliderGrabActive] = th.accent_alt;
    style.Colors[ImGuiCol_Header] = ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.28f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.48f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.68f);
    style.Colors[ImGuiCol_Separator] = th.border;
    style.Colors[ImGuiCol_PlotHistogram] = th.accent;
}

// ---------------------------------------------------------------------------------------------
// Language: Spanish or English, chosen in the launcher (default: Windows' language).

bool g_spanish = false;
std::string g_language = "auto";
std::string g_active_locale = "en";

std::string DetectWindowsLocale() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return "en";
    std::wstring locale(name);
    std::transform(locale.begin(), locale.end(), locale.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    if (locale.rfind(L"pt", 0) == 0) return "pt_BR";
    if (locale.rfind(L"es", 0) == 0) return "es";
    if (locale.rfind(L"fr", 0) == 0) return "fr";
    if (locale.rfind(L"de", 0) == 0) return "de";
    if (locale.rfind(L"it", 0) == 0) return "it";
    if (locale.rfind(L"ja", 0) == 0) return "ja";
    if (locale.rfind(L"zh", 0) == 0) return "zh_CN";
    if (locale.rfind(L"ru", 0) == 0) return "ru";
    return "en";
}

void ApplyLauncherLanguage() {
    g_active_locale = g_language == "auto" ? DetectWindowsLocale() : g_language;
    g_spanish = g_active_locale == "es";
}

const char* T(const char* es, const char* en) {
    if (g_active_locale == "es") return es;
    if (g_active_locale == "en") return en;
    const auto& all = GeneratedLocales();
    const auto language = all.find(g_active_locale);
    if (language != all.end()) {
        const auto value = language->second.find(en);
        if (value != language->second.end()) return value->second.c_str();
    }
    return en;
}

// ---------------------------------------------------------------------------------------------
// key=value files, edited in place: comments, order and unknown keys are kept.

struct Ini {
    std::vector<std::string> lines;
    std::map<std::string, std::string> values;

    void Load(const fs::path& path) {
        lines.clear();
        values.clear();
        std::stringstream ss(ReadText(path));
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            const auto t = Trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            values[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
        }
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    bool Flag(const std::string& key, bool fallback) const {
        const auto v = Get(key);
        return v.empty() ? fallback : v != "0";
    }
    int Int(const std::string& key, int fallback) const {
        const auto v = Get(key);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end && *end == '\0' ? int(n) : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        values[key] = value;
        erased.erase(key);
    }
    // Removes the key: its line is dropped on Save.
    void Erase(const std::string& key) {
        values.erase(key);
        erased.insert(key);
    }
    std::set<std::string> erased;
    bool Save(const fs::path& path, const char* header) {
        std::vector<std::string> out;
        std::map<std::string, bool> written;
        for (const auto& line : lines) {
            const auto t = Trim(line);
            const auto eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos) {
                const auto key = Trim(t.substr(0, eq));
                if (erased.count(key)) continue;
                const auto it = values.find(key);
                if (it != values.end()) {
                    if (!written[key]) out.push_back(key + "=" + it->second);
                    written[key] = true;
                    continue;
                }
            }
            out.push_back(line);
        }
        if (lines.empty() && header) out.push_back(header);
        for (const auto& [key, value] : values) {
            if (!written[key]) out.push_back(key + "=" + value);
        }
        std::string text;
        for (const auto& line : out) text += line + "\n";
        if (!WriteTextAtomic(path, text)) return false;
        lines = out;
        erased.clear();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// param.sfo: the game's title, version and category (gd = game, gp = patch).

std::map<std::string, std::string> ParseSfo(const std::string& data) {
    std::map<std::string, std::string> out;
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
               uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
    };
    auto u16 = [&](size_t at) -> uint32_t {
        if (at + 2 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8;
    };
    if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) return out;
    const uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= data.size(); ++i) {
        const size_t entry = 20 + size_t(i) * 16;
        const uint32_t key_off = u16(entry), fmt = u16(entry + 2), len = u32(entry + 4), off = u32(entry + 12);
        const size_t key_at = size_t(keys) + key_off;
        if (key_at >= data.size()) continue;
        const size_t key_end = data.find('\0', key_at);
        if (key_end == std::string::npos) continue;
        const std::string key = data.substr(key_at, key_end - key_at);
        const size_t value_at = size_t(values) + off;
        if (value_at + len > data.size()) continue;
        if (fmt == 0x0404) {
            out[key] = std::to_string(u32(value_at));
        } else {
            std::string v = data.substr(value_at, len);
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[key] = v;
        }
    }
    return out;
}

std::map<std::string, std::string> ReadSfo(const fs::path& file) { return ParseSfo(ReadText(file)); }

enum class Kind { Empty, Missing, NotGame, OtherTitle, OtherVersion, Base, Update, Complete };

struct FolderInfo {
    Kind kind = Kind::Empty;
    std::string title, version, category;
    std::uintmax_t bytes = 0;
    bool split = false;  // Image0 + Sc0 (PS4 PKG Tool): needs preparing before it runs
};

std::uintmax_t FolderBytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += it->file_size(e2);
    }
    return total;
}

// Where a dump keeps its files. Most tools give the PS4 layout (eboot.bin and sce_sys\param.sfo
// side by side); PS4 PKG Tool splits it into Image0\ (eboot.bin, sce_module, the data) and Sc0\
// (what goes in sce_sys). Such a folder is "split": preparing the game flattens it.
struct Layout {
    fs::path files, sys;  // the folder that becomes the game root, the one that becomes sce_sys
    bool split = false;
    explicit operator bool() const { return !files.empty(); }
};

Layout FindLayout(const fs::path& dir) {
    std::error_code ec;
    const auto has = [&](const fs::path& p) { return fs::is_regular_file(p, ec); };
    const bool flat = has(dir / L"eboot.bin") && has(dir / L"sce_sys" / L"param.sfo");
    const fs::path image = dir / L"Image0";
    // A folder fixed by hand keeps Image0 next to a copied eboot.bin: it is flat only once
    // sce_module is in the root too.
    if (flat && (!has(image / L"eboot.bin") || fs::is_directory(dir / L"sce_module", ec))) return {dir, dir / L"sce_sys", false};
    if (!has(image / L"eboot.bin")) return {};
    for (const fs::path& sys : {image / L"sce_sys", dir / L"Sc0", dir / L"sce_sys"}) {
        if (has(sys / L"param.sfo")) return {image, sys, true};
    }
    return {};
}

// A folder picked one level too high (the folder that holds CUSA13795) stands for its only game.
fs::path GameRoot(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || FindLayout(dir)) return dir;
    // Image0 itself, picked from a PS4 PKG Tool dump, stands for the dump.
    if (_wcsicmp(dir.filename().c_str(), L"Image0") == 0 && FindLayout(dir.parent_path())) return dir.parent_path();
    fs::path found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && FindLayout(it->path())) {
            if (!found.empty()) return dir;  // more than one: the player chooses
            found = it->path();
        }
    }
    return found.empty() ? dir : found;
}

/// The files of a dump as (source, path in the PS4 layout).
std::vector<std::pair<fs::path, fs::path>> LayoutFiles(const fs::path& dir, std::error_code& ec) {
    std::vector<std::pair<fs::path, fs::path>> files;
    const Layout layout = FindLayout(dir);
    if (!layout) return files;
    auto add = [&](const fs::path& from, const fs::path& prefix) {
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.emplace_back(it->path(), prefix / fs::relative(it->path(), from, e2));
        }
    };
    add(layout.files, {});
    if (!ec && layout.split && layout.sys != layout.files / L"sce_sys") add(layout.sys, L"sce_sys");
    return files;
}

FolderInfo Analyze(const fs::path& dir) {
    FolderInfo info;
    if (dir.empty()) return info;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        info.kind = Kind::Missing;
        return info;
    }
    const Layout layout = FindLayout(dir);
    if (!layout) {
        info.kind = Kind::NotGame;
        return info;
    }
    info.split = layout.split;
    const auto sfo = ReadSfo(layout.sys / L"param.sfo");
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    info.title = get("TITLE_ID");
    info.version = get("APP_VER");
    info.category = get("CATEGORY");
    if (!IsCtrTitle(info.title)) {
        info.kind = Kind::OtherTitle;
    } else if (info.category == "gd") {
        info.kind = Kind::Base;
    } else if (info.version != kUpdateVersion) {
        info.kind = Kind::OtherVersion;
    } else {
        info.bytes = FolderBytes(layout.split ? layout.files : dir);
        info.kind = info.bytes >= kCompleteGameMinBytes ? Kind::Complete : Kind::Update;
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// Processes

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

/// The port folder: the launcher's own (portable package), or the source tree above out\.
fs::path FindRoot() {
    fs::path dir = ExeDir();
    for (int i = 0; i < 3 && !dir.empty(); ++i) {
        std::error_code ec;
        if (fs::is_regular_file(dir / L"run.py", ec)) return dir;
        dir = dir.parent_path();
    }
    return ExeDir();
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

struct Python {
    std::wstring exe;
    std::wstring args;  // before the script, e.g. L"-3 "
    std::string description;
};

std::optional<Python> FindPython(const fs::path& root) {
    std::error_code ec;
    const fs::path bundled = root / L"python" / L"python.exe";
    if (fs::is_regular_file(bundled, ec)) return Python{bundled.wstring(), L"", "python\\ (bundled)"};
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"py.exe", nullptr, DWORD(std::size(found)), found, nullptr)) {
        return Python{found, L"-3 ", Narrow(found)};
    }
    if (SearchPathW(nullptr, L"python.exe", nullptr, DWORD(std::size(found)), found, nullptr)) {
        // Windows' "App execution alias" opens the Microsoft Store instead of running Python.
        if (std::wstring(found).find(L"WindowsApps") == std::wstring::npos) return Python{found, L"", Narrow(found)};
    }
    const fs::path msys = L"C:\\msys64\\clang64\\bin\\python.exe";
    if (fs::is_regular_file(msys, ec)) return Python{msys.wstring(), L"", PathText(msys)};
    return std::nullopt;
}

struct Captured {
    bool started = false;
    DWORD code = 1;
    std::string output;
};

/// Runs a console program without a window and returns its output (stdout + stderr).
Captured RunCaptured(const std::wstring& exe, const std::wstring& command_line, const fs::path& cwd,
                     DWORD timeout_ms) {
    Captured result;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return result;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        return result;
    }
    result.started = true;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) result.output.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    reader.join();
    GetExitCodeProcess(pi.hProcess, &result.code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read);
    return result;
}

void OpenInShell(const fs::path& target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<fs::path> PickFiles(HWND owner, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter) {
    std::vector<fs::path> result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(title);
    const COMDLG_FILTERSPEC spec[] = {{filter_name, filter}};
    dialog->SetFileTypes(1, spec);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result.emplace_back(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<fs::path> PickFolder(HWND owner, const wchar_t* title, const fs::path& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// ---------------------------------------------------------------------------------------------
// The launcher

struct Choice {
    const char* es;
    const char* en;
    const char* value;
};

// PS4 system language codes (BB_LANGUAGE) of the languages CUSA13795 (the Americas build) has:
// English (1, US), Latin American Spanish (20), French (2) and Brazilian Portuguese (17). Other
// codes (Spain's Spanish 3, German, Italian...) are not in this build and fall back to English.
constexpr Choice kGameLanguages[] = {
    {"Inglés", "English", "1"},
    {"Español latinoamericano", "Spanish (Latin America)", "20"},
    {"Francés", "French", "2"},
    {"Portugués (Brasil)", "Portuguese (Brazil)", "17"},
};
// The European build (CUSA14876) has English, Spain's Spanish (3), French, German (4), Italian (5)
// and Arabic (21): the .lng texts in its archives.
constexpr Choice kGameLanguagesEU[] = {
    {"Inglés", "English", "1"},
    {"Español (España)", "Spanish (Spain)", "3"},
    {"Francés", "French", "2"},
    {"Alemán", "German", "4"},
    {"Italiano", "Italian", "5"},
    {"Árabe", "Arabic", "21"},
};
constexpr Choice kPresentModes[] = {
    {"Mailbox (recomendado)", "Mailbox (recommended)", "Mailbox"},
    {"FIFO (VSync)", "FIFO (VSync)", "Fifo"},
    {"FIFO relajado", "FIFO Relaxed", "FifoRelaxed"},
    {"Inmediato (sin VSync)", "Immediate (no VSync)", "Immediate"},
};
constexpr int kFpsLimits[] = {0, 30, 60, 72, 90, 120, 144, 240};

struct SaveParam {  // OrbisSaveDataParam
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};
static_assert(sizeof(SaveParam) == 1328, "OrbisSaveDataParam layout");

struct SaveSlot {
    std::string dir, title, subtitle, detail, modified;
    std::uintmax_t bytes = 0;
};

struct Backup {
    fs::path path;
    std::string name, modified;
    std::uintmax_t bytes = 0;
};

struct PatchInfo {
    std::string mod_name, mod_hash, mod_source, mod_description, mod_versions;
    std::string key, name, author, note, version;
    std::string title;  // the edition its file is for (CUSA13795, CUSA14876), "" for any game
    bool default_on = false;
    const char* ini_key = nullptr;
};

struct DlcInfo {
    fs::path path;
    std::string file, title, label, problem;
    bool ok = false;
};

enum class Job { None, Merge, Check, Play };

struct App {
    HWND hwnd = nullptr;
    int active_page = 0;
    fs::path root, bin;
    std::string version;
    std::optional<Python> python;

    Ini launcher;  // launcher.ini
    Ini game;      // bbport.ini
    char online_server_buf[128] = "octr.ryu7w7.xyz";
    char online_name_buf[32] = "";

    // Game folders.
    fs::path game_dir, base_dir, update_dir;
    FolderInfo game_info, base_info, update_info;
    std::atomic<bool> game_checked{false};
    bool game_ok = false;
    std::string check_message;

    // Background work (one at a time).
    std::atomic<Job> job{Job::None};
    std::thread worker;
    std::mutex mutex;  // guards the strings below and log
    std::string job_error;
    std::atomic<size_t> merge_done{0}, merge_total{0};
    std::atomic<bool> job_finished{false};

    // The running game.
    HANDLE process = nullptr, job_object = nullptr;
    std::deque<std::string> log;
    std::string last_status;
    std::atomic<bool> game_running{false};
    // The native renderer's shaders being built before the game starts (run.py's "@progress"
    // lines): percent, -1 when no build is on; its stage (extract, export, translate, compile).
    std::atomic<int> shader_percent{-1};
    std::string shader_stage;
    DWORD exit_code = 0;
    bool show_exit = false, stopped = false;

    // Monitors and controllers (bb-gpu-capabilities).
    std::vector<std::pair<std::string, std::string>> displays;  // value (number), label
    std::vector<std::pair<std::string, std::string>> gamepads;  // GUID, name
    std::thread probe;
    std::atomic<bool> probe_done{false};

    bool want_close = false;
    bool confirm_clear_cache = false;
    bool ask_nick = false;  // PLAY pressed online without a usable nickname
    // A Discord invite for another server, waiting for the player to confirm the switch.
    bool ask_join_server = false;
    std::string join_server, join_nick;
    bool pending_play = false;  // an accepted invite starts the game once it is ready
    std::string notice;

    // Patches tab.
    std::vector<PatchInfo> patches;
    std::vector<std::string> patch_enabled, patch_disabled;
    // Saves tab.
    std::vector<SaveSlot> saves;
    std::vector<Backup> backups;
    bool saves_dirty = true;
    fs::path pending_path;
    int pending_action = 0, confirm_action = 0;  // 1 restore, 2 delete a backup, 3 remove a DLC
    // DLC tab.
    std::vector<DlcInfo> dlc;
    bool dlc_dirty = true;
};

App g_app;

/// Bringing the settings from another installation (settings_import.h, ImportPopup).
enum class ImportState { Idle, Copying, Done, Failed };
struct {
    std::vector<fs::path> from;  // the installations found, the last played first
    int choice = 0;
    bool ask = false;            // open the question on the next frame
    std::atomic<ImportState> state{ImportState::Idle};
    std::string error;           // written before state turns Failed
} g_import;

bool RestartLauncher(const std::wstring& args);

/// The title ID of the chosen game (the merged folder, else the update or the base game): the
/// saves, the shader cache and the merged folder's name follow it. CUSA13795 when none is known.
std::wstring GameTitle() {
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info})
        if (IsCtrTitle(info->title)) return Widen(info->title);
    return L"CUSA13795";
}

std::string EffectiveRenderRes();

fs::path LauncherIni() { return g_app.root / L"launcher.ini"; }
fs::path GameIni() { return g_app.root / L"bbport.ini"; }

void SaveLauncher() {
    App& a = g_app;
    a.launcher.Set("game_dir", PathText(a.game_dir));
    a.launcher.Set("base_dir", PathText(a.base_dir));
    a.launcher.Set("update_dir", PathText(a.update_dir));
    a.launcher.Set("ui_language", g_language);
    a.launcher.Set("ui_theme", std::to_string(g_ui_theme));
    if (!a.launcher.Save(LauncherIni(), "# CTR-NitroFueled-PC launcher")) {
        a.notice = T("No se pudo guardar launcher.ini.", "Could not save launcher.ini.");
    }
}

void SaveGameIni() {
    if (!g_app.game.Save(GameIni(), "# bbport settings (in-game menu: Insert / L3+R3)")) {
        g_app.notice = T("No se pudo guardar bbport.ini.", "Could not save bbport.ini.");
    }
}

void SyncOnlineBuffers() {
    App& a = g_app;
    std::string srv = a.game.Get("online_server", "octr.ryu7w7.xyz");
    if (srv.empty()) {
        srv = "octr.ryu7w7.xyz";
        a.game.Set("online_server", srv);
    }
    std::string nick = a.game.Get("online_name", a.game.Get("user_name"));
    std::strncpy(a.online_server_buf, srv.c_str(), sizeof(a.online_server_buf) - 1);
    a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
    std::strncpy(a.online_name_buf, nick.c_str(), sizeof(a.online_name_buf) - 1);
    a.online_name_buf[sizeof(a.online_name_buf) - 1] = '\0';
}

std::string CurrentNick() { return Trim(g_app.game.Get("online_name", g_app.game.Get("user_name"))); }

/// The server derives the player's ID from the nickname, so everyone left on the old default
/// (RyuPC) was ONE player online: one wallet, and matchmaking evicting each other. These names
/// are refused; run.py refuses them too.
bool ReservedNick(const std::string& nick) {
    static const char* const kReserved[] = {"ryupc", "ryunet", "player", "jugador", "guest"};
    std::string lower = nick;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* r : kReserved)
        if (lower == r) return true;
    return false;
}

bool NickChar(unsigned c) { return c < 128 && (std::isalnum(int(c)) || c == '_' || c == '-'); }

/// Why this nickname cannot be used online, or nullptr when it can.
const char* NickProblem(const std::string& nick) {
    if (nick.empty()) return T("Falta tu apodo.", "Your nickname is missing.");
    if (nick.size() < 3 || nick.size() > 16)
        return T("El apodo debe tener de 3 a 16 caracteres.", "The nickname must be 3 to 16 characters.");
    for (const char c : nick)
        if (!NickChar(static_cast<unsigned char>(c)))
            return T("Solo letras, números, _ y -.", "Only letters, digits, _ and -.");
    if (ReservedNick(nick))
        return T("Ese es un apodo por defecto que usa mucha gente: elige uno propio.",
                 "That is a default nickname many people share: choose your own.");
    return nullptr;
}

int NickFilter(ImGuiInputTextCallbackData* data) { return NickChar(data->EventChar) ? 0 : 1; }

/// Nickname field (letters, digits, _ and - only, 16 at most); true when it was edited.
bool NickInput(const char* label) {
    App& a = g_app;
    constexpr size_t kMaxNick = 16;
    static_assert(sizeof(a.online_name_buf) > kMaxNick);
    if (!ImGui::InputText(label, a.online_name_buf, kMaxNick + 1, ImGuiInputTextFlags_CallbackCharFilter, NickFilter))
        return false;
    a.game.Set("online_name", a.online_name_buf);
    a.game.Set("user_name", a.online_name_buf);
    return true;
}

/// This installation's secret, shared with run.py (ryunet_online.key next to it): the server
/// lets the first key to use a nickname keep it. Created here when missing, so the nickname
/// check below already speaks for this installation.
/// A second copy of the key outside the game folder (%APPDATA%\CTR-NitroFueled-PC), so that
/// deleting, moving or re-downloading the folder on the same PC keeps the nicknames.
fs::path BackupKeyPath() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    return appdata && *appdata ? fs::path(appdata) / L"CTR-NitroFueled-PC" / L"ryunet_online.key" : fs::path();
}

std::string ReadKeyFile(const fs::path& path) {
    std::string key;
    if (path.empty()) return key;
    std::ifstream(path, std::ios::binary) >> key;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos) key.clear();
    return key;
}

bool WriteKeyFile(const fs::path& path, const std::string& key) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << key << "\n";
    return bool(out);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Every key used on this PC (run.py known_keys), one per line next to the backup: a nickname
/// the current key does not own may belong to another folder's (two installations, or the
/// backup from an older one), and the nickname check switches to it (AskNickStatus).
fs::path KeyringPath() {
    const fs::path backup = BackupKeyPath();
    return backup.empty() ? fs::path() : backup.parent_path() / L"ryunet_online_keys.txt";
}

std::vector<std::string> KnownKeys() {
    std::vector<std::string> keys;
    const fs::path path = KeyringPath();
    if (path.empty()) return keys;
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        line = Lower(Trim(line));
        if (line.size() == 64 && line.find_first_not_of("0123456789abcdef") == std::string::npos &&
            std::find(keys.begin(), keys.end(), line) == keys.end())
            keys.push_back(line);
    }
    return keys;
}

void RememberKey(const std::string& key) {
    const fs::path path = KeyringPath();
    if (path.empty() || key.size() != 64) return;
    const std::vector<std::string> keys = KnownKeys();
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::app) << key << "\n";
}

std::mutex g_online_key_mutex;  // the server checks ask from background threads
std::string g_online_key;

std::string OnlineKey() {
    std::scoped_lock lock{g_online_key_mutex};
    if (!g_online_key.empty()) return g_online_key;
    const fs::path path = g_app.root / L"ryunet_online.key";
    const fs::path backup = BackupKeyPath();
    std::string key = ReadKeyFile(path);
    if (key.empty()) key = ReadKeyFile(backup);  // a new folder on a PC that played before
    if (key.empty()) {
        unsigned char raw[32];
        if (BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
        static const char kHex[] = "0123456789abcdef";
        for (const unsigned char b : raw) {
            key += kHex[b >> 4];
            key += kHex[b & 15];
        }
    }
    if (ReadKeyFile(path) != key && !WriteKeyFile(path, key)) return {};
    if (ReadKeyFile(backup).empty()) WriteKeyFile(backup, key);  // never overwrites another folder's
    RememberKey(ReadKeyFile(backup));
    RememberKey(key);
    g_online_key = key;
    return g_online_key;
}

/// This folder plays with another of this PC's keys from now on (the game reads the file too).
bool UseOnlineKey(const std::string& key) {
    std::scoped_lock lock{g_online_key_mutex};
    if (!WriteKeyFile(g_app.root / L"ryunet_online.key", key)) return false;
    g_online_key = key;
    return true;
}

/// Server availability of a nickname: "free", "yours", "taken", "reserved", "invalid", or
/// "" while unknown or when the server could not be asked (it checks again at login).
struct NickCheck {
    struct Result {
        std::string status;
        ULONGLONG at = 0;  // GetTickCount64: an unreachable server is asked again later
        bool linked = false;          // the nickname is linked to a Discord account
        bool link_available = false;  // the server can link Discord accounts
        std::string discord;          // the linked account's name (only for our own nickname)
        std::string banned;           // the ban's reason when this player or address is banned
        std::string ban_kind;         // "player" or "ip"
        long long ban_until = 0;      // unix time; 0: permanent
        bool key_switched = false;    // it was another installation's on this PC: its key is used now
    };
    std::mutex mutex;
    std::map<std::string, Result> results;  // "server|nick" -> answer
    std::string in_flight;
};
NickCheck g_nick_check;

/// The server setting's host and HTTP port, split exactly as run.py does ("host" or "host:port").
void SplitServer(const std::string& server, std::string& host, INTERNET_PORT& port) {
    host = server;
    port = 8080;  // run.py's online_http_port default
    const auto colon = server.find(':');
    if (colon == std::string::npos || server.front() == '[') return;
    const std::string digits = server.substr(colon + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return;
    host = server.substr(0, colon);
    port = INTERNET_PORT(std::atoi(digits.c_str()));
}

/// What one server sees of the installation's key: HMAC-SHA256(key, host), as run.py sends it.
/// Every server gets a different value, so none can reuse it on another to take a nickname.
/// SHA-256 of msg (HMAC-SHA256 with key when there is one), in lower-case hex; "" on failure.
std::string Sha256Hex(const std::string& msg, const std::string& key = {}) {
    unsigned char out[32];
    bool ok = false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        // No secret at all for a plain hash: a non-null pointer, even with length 0, makes
        // BCryptCreateHash fail (STATUS_INVALID_PARAMETER), and every request went unsigned.
        PUCHAR secret = key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data()));
        if (BCryptCreateHash(alg, &hash, nullptr, 0, secret, ULONG(key.size()), 0) == 0) {
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())), ULONG(msg.size()), 0) == 0 &&
                 BCryptFinishHash(hash, out, sizeof out, 0) == 0;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (const unsigned char b : out) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    return hex;
}

std::string ServerKey(const std::string& host, const std::string& key) {
    if (key.size() != 64) return {};
    std::string raw(32, '\0');
    for (int i = 0; i < 32; ++i) raw[size_t(i)] = static_cast<char>(std::stoi(key.substr(i * 2, 2), nullptr, 16));
    return Sha256Hex(Lower(Trim(host)), raw);
}

/// What the server stores of the server key (its keyHash): the key requests are signed with.
/// key: one of this PC's keys (KnownKeys); the installation's own when empty.
std::string ServerKeyHash(const std::string& host, const std::string& key = {}) {
    const std::string server_key = ServerKey(host, key.empty() ? OnlineKey() : key);
    return server_key.empty() ? std::string() : Sha256Hex(server_key);
}

/// Seconds to add to this PC's clock to get the RyuNET server's (run.py SERVER_CLOCK_OFFSET), from
/// the Date of its answers. Signatures carry the time and the server takes them only within 5
/// minutes of its own clock: with the PC's clock off, every signed request was refused and the
/// player's own nickname read as taken.
std::atomic<long long> g_server_clock_offset{0};

/// An answer of the RyuNET server: its Date header sets g_server_clock_offset.
void NoteServerClock(HINTERNET req) {
    SYSTEMTIME date{};
    DWORD size = sizeof date;
    FILETIME file{};
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME, WINHTTP_HEADER_NAME_BY_INDEX,
                             &date, &size, WINHTTP_NO_HEADER_INDEX) ||
        !SystemTimeToFileTime(&date, &file))
        return;
    const long long ticks = (static_cast<long long>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    const long long server = (ticks - 116444736000000000LL) / 10000000LL;  // 100 ns since 1601 -> Unix
    const long long offset = server - static_cast<long long>(std::time(nullptr));
    // The header has whole seconds: a few seconds either way change nothing.
    g_server_clock_offset = offset > 5 || offset < -5 ? offset : 0;
}

/// X-RyuNET-Proof for one request (the server's nick_proof.go, run.py proof_header): the key
/// never crosses the network, and the signature is good once, for a few minutes of the
/// server's time.
std::string ProofHeader(const std::string& key_hash, const char* method, const std::string& target) {
    unsigned char raw[16];
    if (key_hash.empty() || BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string nonce;
    for (const unsigned char b : raw) {
        nonce += kHex[b >> 4];
        nonce += kHex[b & 15];
    }
    const std::string ts = std::to_string(static_cast<long long>(std::time(nullptr)) + g_server_clock_offset.load());
    const std::string mac = Sha256Hex(ts + "\n" + nonce + "\n" + method + "\n" + target, key_hash);
    return mac.empty() ? std::string() : "1." + ts + "." + nonce + "." + mac;
}

/// A request to the RyuNET server's HTTP port, signed with this installation's key for that
/// server; register also sends the key's hash (a Discord link takes it for a new installation).
/// The body, or "" when the server could not be reached. A request signed with a clock found off
/// by minutes goes again once: the server refuses it before doing anything. Called from
/// background threads. with_key signs with another of this PC's keys (KnownKeys).
std::string HttpRequest(const std::string& server, const wchar_t* method, const std::wstring& path,
                        const std::string& form = {}, int timeout_ms = 3000, bool register_key = false,
                        const std::string& with_key = {}) {
    std::string host;
    INTERNET_PORT port = 0;
    SplitServer(server, host, port);
    const std::string key_hash = ServerKeyHash(host, with_key);
    std::string body;
    HINTERNET session = WinHttpOpen(L"CTR-NitroFueled-PC launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET conn = WinHttpConnect(session, Widen(host).c_str(), port, 0);
    for (int attempt = 1; conn && attempt <= 2; ++attempt) {
        const long long offset = g_server_clock_offset.load();
        const std::string proof = ProofHeader(key_hash, Narrow(method).c_str(), Narrow(path));
        HINTERNET req = WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        std::wstring headers;
        if (!proof.empty()) headers += L"X-RyuNET-Proof: " + Widen(proof) + L"\r\n";
        if (register_key && !key_hash.empty()) headers += L"X-RyuNET-Key-Hash: " + Widen(key_hash) + L"\r\n";
        if (!form.empty()) headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
        body.clear();
        bool answered = false;
        if (req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                      headers.empty() ? 0 : DWORD(-1),
                                      form.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(form.data()),
                                      DWORD(form.size()), DWORD(form.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            answered = true;
            NoteServerClock(req);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 4096) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                body.append(chunk.data(), got);
            }
        }
        if (req) WinHttpCloseHandle(req);
        const long long moved = g_server_clock_offset.load() - offset;
        if (!answered || proof.empty() || (moved <= 240 && moved >= -240)) break;
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

std::string HttpGet(const std::string& server, const std::wstring& path) { return HttpRequest(server, L"GET", path); }

/// A string field of a small flat JSON answer ("" when missing).
std::string JsonField(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":\"";
    const auto at = body.find(field);
    if (at == std::string::npos) return {};
    const auto end = body.find('"', at + field.size());
    return end == std::string::npos ? std::string() : body.substr(at + field.size(), end - at - field.size());
}

/// A number field of a small flat JSON answer.
bool JsonNumber(const std::string& body, const char* key, int& value) {
    const std::string field = std::string("\"") + key + "\":";
    const auto at = body.find(field);
    if (at == std::string::npos) return false;
    char* end = nullptr;
    const long n = std::strtol(body.c_str() + at + field.size(), &end, 10);
    if (end == body.c_str() + at + field.size()) return false;
    value = int(n);
    return true;
}

// Defined with the Discord join secrets below.
std::string JsonText(const std::string& body, const char* key);

/// A true/false field of a small flat JSON answer.
bool JsonBool(const std::string& body, const char* key) {
    return body.find(std::string("\"") + key + "\":true") != std::string::npos;
}

NickCheck::Result AskNickStatus(const std::string& server, const std::string& nick) {
    // {"status":"...","linked":..,"discord_link":..,"discord":".."}; a server without this
    // endpoint answers something else.
    const std::wstring path = L"/ryunet/nick?name=" + Widen(nick);  // [A-Za-z0-9_-]
    std::string body = HttpGet(server, path);
    bool key_switched = false;
    if (JsonField(body, "status") == "taken") {
        // Another installation on this PC may own it (KnownKeys): this folder takes its key.
        const std::string own = OnlineKey();
        for (const std::string& key : KnownKeys()) {
            if (key == own) continue;
            const std::string other = HttpRequest(server, L"GET", path, {}, 3000, false, key);
            if (JsonField(other, "status") == "yours" && UseOnlineKey(key)) {
                body = other;
                key_switched = true;
                break;
            }
        }
    }
    NickCheck::Result r;
    r.key_switched = key_switched;
    r.status = JsonField(body, "status");
    r.linked = JsonBool(body, "linked");
    r.link_available = JsonBool(body, "discord_link");
    r.discord = JsonField(body, "discord");
    r.banned = JsonText(body, "banned");
    r.ban_kind = JsonField(body, "ban_kind");
    if (const auto at = body.find("\"ban_until\":"); at != std::string::npos)
        r.ban_until = std::strtoll(body.c_str() + at + 12, nullptr, 10);
    return r;
}

/// Cached answer for the current nickname, asking the server in the background once per
/// nickname. "?" while the question is out.
std::string NickAvailability() {
    const std::string nick = CurrentNick();
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    if (NickProblem(nick) || server.empty()) return "";
    const std::string id = server + "|" + nick;
    std::scoped_lock lock{g_nick_check.mutex};
    if (const auto it = g_nick_check.results.find(id); it != g_nick_check.results.end()) {
        if (!it->second.status.empty() || GetTickCount64() - it->second.at < 30000) return it->second.status;
    }
    if (!g_nick_check.in_flight.empty()) return "?";
    g_nick_check.in_flight = id;
    std::thread([server, nick, id] {
        NickCheck::Result result = AskNickStatus(server, nick);
        result.at = GetTickCount64();
        std::scoped_lock done{g_nick_check.mutex};
        if (result.key_switched) g_nick_check.results.clear();  // the other answers were for the old key
        g_nick_check.results[id] = result;
        g_nick_check.in_flight.clear();
    }).detach();
    return "?";
}

/// Whether the server can link Discord accounts, asked once in the background without a
/// nickname (a reinstalled launcher has none yet).
bool DiscordLinkAvailable() {
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    if (server.empty()) return false;
    const std::string id = server + "|*";
    std::scoped_lock lock{g_nick_check.mutex};
    if (const auto it = g_nick_check.results.find(id); it != g_nick_check.results.end()) {
        if (it->second.link_available || GetTickCount64() - it->second.at < 30000) return it->second.link_available;
    }
    if (!g_nick_check.in_flight.empty()) return false;
    g_nick_check.in_flight = id;
    std::thread([server, id] {
        NickCheck::Result result = AskNickStatus(server, "");
        result.at = GetTickCount64();
        std::scoped_lock done{g_nick_check.mutex};
        g_nick_check.results[id] = result;
        g_nick_check.in_flight.clear();
    }).detach();
    return false;
}

/// The last answer about the current nickname (status "" when unknown), without asking.
NickCheck::Result NickInfo() {
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    std::scoped_lock lock{g_nick_check.mutex};
    const auto it = g_nick_check.results.find(server + "|" + CurrentNick());
    return it == g_nick_check.results.end() ? NickCheck::Result{} : it->second;
}

/// This player's ban, as the server's nickname check tells it (nothing when not banned).
bool BanNotice() {
    const NickCheck::Result info = NickInfo();
    if (info.banned.empty()) return false;
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", info.ban_kind == "ip" ? T("Tu conexión (IP) está baneada del online.", "Your connection (IP) is banned from online play.") : T("Tu cuenta está baneada del online.", "Your account is banned from online play."));
    ImGui::TextWrapped(T("Motivo: %s", "Reason: %s"), info.banned.c_str());
    if (info.ban_until > 0) {
        char when[32];
        const std::time_t t = std::time_t(info.ban_until);
        std::tm local{};
        localtime_s(&local, &t);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
        ImGui::TextWrapped(T("Hasta: %s", "Until: %s"), when);
    } else {
        ImGui::TextWrapped("%s", T("Duración: permanente", "Duration: permanent"));
    }
    ImGui::PopStyleColor();
    return true;
}

/// One line under a nickname field: what the server says about it.
void NickAvailabilityLine() {
    const std::string s = NickAvailability();
    if (BanNotice()) return;
    if (s == "?") {
        ImGui::TextColored(kGrey, "%s", T("Comprobando el apodo en el servidor...", "Checking the nickname with the server..."));
    } else if (s == "free") {
        ImGui::TextColored(kGreen, "%s", T("Disponible: será tuyo la primera vez que juegues online.",
                                           "Available: it becomes yours the first time you play online."));
    } else if (s == "yours" && NickInfo().key_switched) {
        ImGui::TextColored(kGreen, "%s", T("Este apodo es tuyo (recuperado de otra instalación de este PC).",
                                           "This nickname is yours (recovered from another installation on this PC)."));
    } else if (s == "yours") {
        ImGui::TextColored(kGreen, "%s", T("Este apodo es tuyo.", "This nickname is yours."));
    } else if (s == "taken" && NickInfo().linked) {
        ImGui::TextColored(kRed, "%s", T("Ese apodo está vinculado a una cuenta de Discord. Si es tuyo, recupéralo en la "
                                         "pestaña Online (Recuperar con Discord); si no, elige otro.",
                                         "That nickname is linked to a Discord account. If it is yours, recover it in the "
                                         "Online tab (Recover with Discord); otherwise choose another one."));
    } else if (s == "taken") {
        ImGui::TextColored(kRed, "%s", T("Ese apodo ya lo usa otra persona: elige otro.",
                                         "Someone else already uses that nickname: choose another one."));
    } else if (s == "reserved" || s == "invalid") {
        ImGui::TextColored(kRed, "%s", T("El servidor no acepta ese apodo: elige otro.", "The server does not accept that nickname: choose another one."));
    }
    const long long clock_off = std::llabs(g_server_clock_offset.load());
    if (clock_off > 240) {
        ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
        ImGui::TextWrapped(T("La hora de este PC va %lld min desfasada: el online la corrige, pero activa la fecha y hora "
                             "automáticas en la configuración de Windows.",
                             "This PC's clock is %lld min off: online corrects it, but set the date and time to "
                             "automatic in the Windows settings."),
                           (clock_off + 30) / 60);
        ImGui::PopStyleColor();
    }
}

bool NickTaken() {
    const std::string s = NickAvailability();
    return s == "taken" || s == "reserved" || s == "invalid";
}

/// The lobby this player hosts (their party or private room), from the server's
/// /ryunet/party, refreshed in the background every 5 s while it is asked for: Discord's
/// "1 of 8" follows players joining and leaving. Only changes reach Discord (DiscordPresence
/// sends nothing for an unchanged activity), within its 5 updates per 20 s.
struct PartyPoll {
    std::mutex mutex;
    std::string id;  // "server|nick" of the last answer
    bool known = false;  // the server answered
    int players = 0, max = 0;
    ULONGLONG at = 0;
    bool in_flight = false;
};
PartyPoll g_party;

/// False while unknown (no answer yet, or a server without /ryunet/party).
bool PartyInfo(const std::string& server, const std::string& nick, int& players, int& max) {
    const std::string id = server + "|" + nick;
    std::scoped_lock lock{g_party.mutex};
    if ((g_party.id != id || GetTickCount64() - g_party.at >= 5000) && !g_party.in_flight) {
        g_party.in_flight = true;
        std::thread([server, nick, id] {
            const std::string body = HttpGet(server, L"/ryunet/party?name=" + Widen(nick));
            int p = 0, m = 0;
            const bool known = JsonNumber(body, "players", p) && JsonNumber(body, "max", m);
            std::scoped_lock done{g_party.mutex};
            g_party.id = id;
            g_party.known = known;
            g_party.players = p;
            g_party.max = m;
            g_party.at = GetTickCount64();
            g_party.in_flight = false;
        }).detach();
    }
    if (g_party.id != id || !g_party.known) return false;
    players = g_party.players;
    max = g_party.max;
    return true;
}

/// The NAT type test (nat_check.h), on a worker thread: once per server while the launcher is
/// open (the Play page's status line starts it), again with the Online tab's button.
struct NatState {
    std::mutex mutex;
    bool running = false, done = false;
    std::string server;  // what the last test asked
    NatCheck::Result result;
};
NatState g_nat;

void StartNatCheck(const std::string& server) {
    {
        std::scoped_lock lock{g_nat.mutex};
        if (g_nat.running) return;
        g_nat.running = true;
        g_nat.server = server;
    }
    std::thread([server] {
        const NatCheck::Result r = NatCheck::Check(server);
        std::scoped_lock lock{g_nat.mutex};
        g_nat.result = r;
        g_nat.running = false;
        g_nat.done = true;
    }).detach();
}

/// Short label and colour for the Play page ("NAT: type 2 (moderate)"); starts the first test
/// for this server.
std::string NatLabel(const std::string& server, ImVec4& color) {
    bool running, done;
    std::string checked;
    NatCheck::Result r;
    {
        std::scoped_lock lock{g_nat.mutex};
        running = g_nat.running;
        done = g_nat.done;
        checked = g_nat.server;
        r = g_nat.result;
    }
    if (!running && (!done || checked != server)) {
        StartNatCheck(server);
        running = true;
    }
    color = kGrey;
    if (running) return std::string("NAT: ") + T("comprobando...", "checking...");
    if (r.replies_blocked || r.error == "server") {
        color = kRed;
        return r.replies_blocked ? T("NAT: online bloqueado (¿firewall?)", "NAT: online blocked (firewall?)")
                                 : T("NAT: el servidor no responde", "NAT: no answer from the server");
    }
    switch (r.type) {
    case NatCheck::Type::Open: color = kGreen; return T("NAT: tipo 1 (abierto)", "NAT: type 1 (open)");
    case NatCheck::Type::Moderate: color = kGreen; return T("NAT: tipo 2 (moderado)", "NAT: type 2 (moderate)");
    case NatCheck::Type::Strict: color = kYellow; return T("NAT: tipo 3 (estricto)", "NAT: type 3 (strict)");
    case NatCheck::Type::Unknown: break;
    }
    return T("NAT: desconocido", "NAT: unknown");
}

/// A server setting that can travel inside a Discord join secret (and be trusted coming out
/// of one): a host name or IP with an optional port.
bool ValidServer(const std::string& s) {
    return !s.empty() && s.size() <= 64 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") == std::string::npos;
}

bool ListHas(const std::string& csv, const std::string& name) {
    std::stringstream in(csv);
    for (std::string item; std::getline(in, item, ',');)
        if (Lower(Trim(item)) == Lower(name)) return true;
    return false;
}

/// run.py's "@progress native-shaders ..." lines: the launcher comes back to show the shader build
/// (ShaderProgressPopup) and goes away again when the game starts. Not shown in the log.
bool ShaderProgressLine(const std::string& line) {
    static const std::string prefix = "@progress native-shaders ";
    if (line.compare(0, prefix.size(), prefix) != 0) return line.rfind("@progress ", 0) == 0;
    App& a = g_app;
    const std::string rest = line.substr(prefix.size());
    if (rest.rfind("done", 0) == 0) {
        a.shader_percent = -1;
        if (a.hwnd) ShowWindowAsync(a.hwnd, a.launcher.Flag("hide_on_play", false) ? SW_HIDE : SW_MINIMIZE);
        return true;
    }
    const size_t space = rest.find(' ');
    const int percent = std::clamp(std::atoi(rest.substr(0, space).c_str()), 0, 100);
    {
        std::scoped_lock lock{a.mutex};
        a.shader_stage = space == std::string::npos ? "" : Trim(rest.substr(space + 1));
    }
    if (a.shader_percent.exchange(percent) < 0 && a.hwnd) {
        ShowWindowAsync(a.hwnd, SW_RESTORE);
        SetForegroundWindow(a.hwnd);
    }
    return true;
}

void AddLog(const std::string& line) {
    if (ShaderProgressLine(line)) return;
    std::scoped_lock lock{g_app.mutex};
    g_app.log.push_back(line);
    while (g_app.log.size() > 2000) g_app.log.pop_front();
    if (!line.empty()) g_app.last_status = line;
}

void JoinWorker() {
    if (g_app.worker.joinable()) g_app.worker.join();
}

void StartCheck() {
    App& a = g_app;
    a.game_info = Analyze(a.game_dir);
    a.game_checked = false;
    a.game_ok = false;
    a.check_message.clear();
    if (a.game_info.kind != Kind::Complete || !a.python) return;
    if (a.game_info.split) {  // Image0 + Sc0: PlayTab offers to prepare it
        a.game_checked = true;
        return;
    }
    JoinWorker();
    a.job = Job::Check;
    a.job_finished = false;
    const fs::path dir = a.game_dir;
    a.worker = std::thread([dir] {
        App& app = g_app;
        const std::wstring cmd = Quote(app.python->exe) + L" " + app.python->args + L"-X utf8 " +
                                 Quote((app.root / L"scripts" / L"game_check.py").wstring()) + L" " + Quote(dir.wstring());
        const Captured r = RunCaptured(app.python->exe, cmd, app.root, 120000);
        {
            std::scoped_lock lock{app.mutex};
            app.game_ok = r.started && r.code == 0;
            app.check_message = r.started ? Trim(r.output) : "Python could not be started.";
        }
        app.game_checked = true;
        app.job_finished = true;
    });
}

fs::path MergeTarget(const fs::path& near_dir) {
    App& a = g_app;
    const fs::path parent = near_dir.parent_path();
    fs::path target = parent / (GameTitle() + L"-1.21");
    if (target == near_dir || target == a.base_dir || target == a.update_dir || target == a.game_dir) {
        target = parent / (GameTitle() + L"-1.21-merged");
    }
    return target;
}

/// Base game + 1.21 update (or one already merged dump in the Image0 + Sc0 layout) -> a new
/// folder of hard links in the PS4 layout (copies across drives).
void StartMerge(std::vector<fs::path> sources) {
    App& a = g_app;
    if (sources.empty()) return;
    const fs::path target = MergeTarget(sources.front());
    JoinWorker();
    a.job = Job::Merge;
    a.job_finished = false;
    a.merge_done = 0;
    a.merge_total = 0;
    {
        std::scoped_lock lock{a.mutex};
        a.job_error.clear();
    }
    a.worker = std::thread([sources, target] {
        App& app = g_app;
        std::vector<std::pair<fs::path, fs::path>> files;  // source, relative
        std::error_code ec;
        for (const fs::path& from : sources) {
            auto more = LayoutFiles(from, ec);
            if (ec) break;
            files.insert(files.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        std::string error;
        if (ec) error = "Cannot read the game folders: " + ec.message();
        app.merge_total = files.size();
        size_t copied = 0;
        for (size_t i = 0; error.empty() && i < files.size(); ++i) {
            const fs::path dest = target / files[i].second;
            std::error_code e2;
            fs::create_directories(dest.parent_path(), e2);
            if (fs::exists(dest, e2)) fs::remove(dest, e2);
            if (!CreateHardLinkW(dest.c_str(), files[i].first.c_str(), nullptr)) {
                if (!CopyFileW(files[i].first.c_str(), dest.c_str(), FALSE)) {
                    error = "Cannot create " + PathText(dest) + " (error " + std::to_string(GetLastError()) + ")";
                    break;
                }
                ++copied;
            }
            app.merge_done = i + 1;
        }
        {
            std::scoped_lock lock{app.mutex};
            app.job_error = error;
            if (error.empty()) {
                app.game_dir = target;
                app.log.push_back("Merged " + std::to_string(files.size()) + " files into " + PathText(target) +
                                  (copied ? " (" + std::to_string(copied) + " copied: different drive)" : ""));
            }
        }
        app.job_finished = true;
    });
}

/// The European edition is chosen (its languages differ from the Americas build's).
bool IsEuGame() { return GameTitle() == L"CUSA14876"; }

/// The game language of the launcher's own language, where the chosen edition has it.
const char* DefaultGameLanguage() {
    const bool eu = IsEuGame();
    if (g_active_locale == "es") return eu ? "3" : "20";
    if (g_active_locale == "fr") return "2";
    if (eu && g_active_locale == "de") return "4";
    if (eu && g_active_locale == "it") return "5";
    if (!eu && g_active_locale == "pt_BR") return "17";
    return "1";
}

/// The game language to start with: the saved one when the chosen edition has it, else its
/// counterpart there (Spanish: Latin American in the Americas build, Spain's in the European one),
/// else the default.
std::string GameLanguage() {
    const std::string saved = g_app.launcher.Get("language");
    const bool eu = IsEuGame();
    auto has = [&](const std::string& code) {
        if (eu) {
            for (const auto& c : kGameLanguagesEU)
                if (code == c.value) return true;
        } else {
            for (const auto& c : kGameLanguages)
                if (code == c.value) return true;
        }
        return false;
    };
    if (has(saved)) return saved;
    if (saved == "3" && !eu) return "20";
    if (saved == "20" && eu) return "3";
    return DefaultGameLanguage();
}

void SetEnv(const wchar_t* name, const std::string& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : Widen(value).c_str());
}

bool UpdateBusy();

void StartGame() {
    App& a = g_app;
    if (!a.python || a.game_running || UpdateBusy()) return;
    SaveGameIni();
    SaveLauncher();
    JoinWorker();
    {
        std::scoped_lock lock{a.mutex};
        a.log.clear();
        a.last_status.clear();
    }
    SetEnv(L"BB_GAME_DIR", PathText(a.game_dir));
    SetEnv(L"BB_NEO", EffectiveRenderRes() != "1080" ? "1" : "0");  // the resolution picks the path
    SetEnv(L"BB_LANGUAGE", GameLanguage());
    SetEnv(L"BB_DISPLAY", a.launcher.Get("display"));
    SetEnv(L"BB_PRESENT_MODE", a.launcher.Get("present_mode", "Mailbox"));
    SetEnv(L"BB_GAMEPAD", a.launcher.Get("gamepad"));
    SetEnv(L"BB_MENU_LANGUAGE", g_spanish ? "es" : "en");  // the in-game menu follows the launcher
    SetEnv(L"BB_SAVE_LOG", a.launcher.Flag("save_log", false) ? "1" : "");
    SetEnv(L"BB_DLC", a.launcher.Flag("dlc", true) ? "1" : "0");
    SetEnv(L"BB_DLC_DISABLED", a.launcher.Get("dlc_disabled"));
    SetEnv(L"BB_FULLSCREEN", "");  // bbport.ini's fullscreen= (the menu switch) decides
    SetEnv(L"PYTHONUTF8", "1");
    SetEnv(L"PYTHONIOENCODING", "utf-8");

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " + Quote((a.root / L"run.py").wstring());
    const BOOL ok = CreateProcessW(a.python->exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, a.root.c_str(), &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        AddLog("Could not start Python (error " + std::to_string(GetLastError()) + ")");
        a.exit_code = 1;
        a.show_exit = true;
        return;
    }
    // A job object, so "Stop" ends run.py and the game it started.
    a.job_object = CreateJobObjectW(nullptr, nullptr);
    if (a.job_object) AssignProcessToJobObject(a.job_object, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    a.process = pi.hProcess;
    a.game_running = true;
    a.show_exit = false;
    a.stopped = false;
    a.job = Job::Play;
    a.job_finished = false;
    AddLog("Starting the game...");
    a.worker = std::thread([read] {
        App& app = g_app;
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                AddLog(Trim(pending.substr(0, nl)));
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) AddLog(Trim(pending));
        CloseHandle(read);
        WaitForSingleObject(app.process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(app.process, &code);
        app.exit_code = code;
        app.job_finished = true;
    });
    if (a.hwnd) {
        if (a.launcher.Flag("hide_on_play", false)) ShowWindow(a.hwnd, SW_HIDE);
        else ShowWindow(a.hwnd, SW_MINIMIZE);
    }
}

void StopGame() {
    g_app.stopped = true;
    if (g_app.job_object) TerminateJobObject(g_app.job_object, 1);
    else if (g_app.process) TerminateProcess(g_app.process, 1);
}

/// Discord application whose name and art the presence shows (and whose OAuth2 links
/// nicknames). launcher.ini discord_app_id can name another one; empty or missing is this one.
constexpr const char* kDiscordAppId = "1560408801704943708";

std::string DiscordAppId() {
    const std::string id = g_app.launcher.Get("discord_app_id");
    return id.empty() ? kDiscordAppId : id;
}

/// Shows the game in the player's Discord status while it runs (launcher.ini discord_rpc,
/// on by default). Called every frame; DiscordPresence only talks to Discord on a change.
void UpdateDiscord() {
    App& a = g_app;
    static int64_t started = 0;
    if (!a.game_running) {
        started = 0;
    } else if (started == 0) {
        started = int64_t(std::time(nullptr));
    }
    const bool enabled = a.launcher.Flag("discord_rpc", true);
    const std::string app_id = DiscordAppId();
    if (!a.game_running) {
        DiscordPresence::Update(enabled, app_id, nullptr);
        return;
    }
    DiscordPresence::Activity activity;
    if (a.game.Flag("online", true)) {
        activity.details = T("Jugando online", "Playing online");
        const std::string nick = CurrentNick();
        if (!nick.empty()) activity.state = std::string(T("Apodo: ", "Nickname: ")) + nick;
        // Joinable: Discord's "Join" and the chat's "+ > Invite" hand our server and nickname to
        // whoever accepts; their launcher adds us as a friend and starts their game.
        const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
        if (!NickProblem(nick) && ValidServer(server)) {
            int players = 0, max = 0;
            if (!PartyInfo(server, nick, players, max)) {
                players = 1;  // a server without /ryunet/party: offer it anyway
                max = 8;
            }
            if (players > 0 && max >= players) {
                activity.party_id = "ctr:" + Lower(server) + ":" + Lower(nick);
                activity.party_size = players;
                activity.party_max = max;
                if (players < max) activity.join_secret = "ctr1|" + server + "|" + nick;
            }
        }
    } else {
        activity.details = T("Jugando sin conexión", "Playing offline");
    }
    activity.start = started;
    activity.large_image = "logo";
    activity.large_text = "Crash Team Racing Nitro-Fueled";
    DiscordPresence::Update(enabled, app_id, &activity);
}

/// Lets Discord start this launcher when the player accepts an invite with it closed: the
/// discord-<app id> URL protocol, registered for the current user as Discord's own SDKs do.
/// Written on every start, so a portable folder that moved points at its new place.
void RegisterDiscordLaunch(const std::string& app_id) {
    if (app_id.empty() || app_id.find_first_not_of("0123456789") != std::string::npos) return;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring path(exe, n);
    const std::wstring base = L"Software\\Classes\\discord-" + Widen(app_id);
    const auto set = [](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    };
    set(base, nullptr, L"URL:Run game " + Widen(app_id) + L" protocol");
    set(base, L"URL Protocol", L"");
    set(base + L"\\DefaultIcon", nullptr, path + L",0");
    set(base + L"\\shell\\open\\command", nullptr, L"\"" + path + L"\"");
}

/// What the PLAY button does: online without a usable nickname asks for one first (NickPopup).
void PressPlay() {
    App& a = g_app;
    if (a.game.Flag("online", true) && (NickProblem(CurrentNick()) || NickTaken())) a.ask_nick = true;
    else StartGame();
}

bool GameReady() {
    const App& a = g_app;
    return a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok && a.job == Job::None && a.python;
}

void BringLauncherToFront() {
    ShowWindow(g_app.hwnd, SW_RESTORE);
    SetForegroundWindow(g_app.hwnd);
}

std::string Format(const char* format, const std::string& a, const std::string& b = {}) {
    char buf[512];
    std::snprintf(buf, sizeof buf, format, a.c_str(), b.c_str());
    return buf;
}

/// An invite accepted in Discord, for a server we use (or agreed to switch to): the host
/// becomes a friend (the game lists friends' lobbies as joinable) and the game starts.
void AcceptDiscordJoin(const std::string& server, const std::string& nick) {
    App& a = g_app;
    a.game.Set("online", "1");
    a.game.Set("online_server", server);
    std::string friends = Trim(a.game.Get("online_friends"));
    if (!ListHas(friends, nick)) friends = friends.empty() ? nick : friends + "," + nick;
    a.game.Set("online_friends", friends);
    SaveGameIni();  // a running game reads the new friend from bbport.ini
    SyncOnlineBuffers();
    {
        std::scoped_lock lock{g_nick_check.mutex};  // the server may have changed
        g_nick_check.results.clear();
    }
    // The game takes this as a PS4 invitation accepted in the system UI and joins the host's
    // session by itself (src/runtime_services.c, SESSION_INVITATION). Sessions are named after
    // their host. The file is stamped so a game that starts much later ignores it.
    {
        std::ofstream join(GameIni().parent_path() / L"ryunet_join.txt", std::ios::binary | std::ios::trunc);
        join << "session=cusa13795-session-" << nick << "\nhost=" << nick << "\ntime=" << int64_t(std::time(nullptr))
             << "\n";
    }
    if (a.game_running) {
        a.notice = Format(T("Invitación de %s aceptada: el juego se une a su partida en unos segundos. Si no lo "
                            "hace, abre Amigos en el juego y únete a %s.",
                            "%s's invite accepted: the game joins their match in a few seconds. If it does "
                            "not, open Friends in the game and join %s."),
                          nick, nick);
        return;
    }
    a.notice = Format(T("Invitación de %s aceptada: el juego se abre y se une a su partida (si no, abre Amigos "
                        "y únete a %s).",
                        "%s's invite accepted: the game starts and joins their match (if not, open Friends "
                        "and join %s)."),
                      nick, nick);
    BringLauncherToFront();
    a.pending_play = true;  // PollJobs presses PLAY as soon as the game folder is checked
}

/// A join secret from Discord: "ctr1|<server>|<nickname>", written by UpdateDiscord.
void OnDiscordJoin(const std::string& secret) {
    App& a = g_app;
    const auto p1 = secret.find('|');
    const auto p2 = p1 == std::string::npos ? p1 : secret.find('|', p1 + 1);
    const std::string server = p2 == std::string::npos ? "" : secret.substr(p1 + 1, p2 - p1 - 1);
    const std::string nick = p2 == std::string::npos ? "" : secret.substr(p2 + 1);
    if (secret.compare(0, p1, "ctr1") != 0 || !ValidServer(server) || NickProblem(nick)) {
        a.notice = T("Esa invitación de Discord no es de este launcher.", "That Discord invite is not from this launcher.");
        BringLauncherToFront();
        return;
    }
    if (Lower(nick) == Lower(CurrentNick())) return;  // our own invite
    if (Lower(server) != Lower(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")))) {
        // Never switch servers silently: a crafted invite could send us to any server.
        a.join_server = server;
        a.join_nick = nick;
        a.ask_join_server = true;  // JoinServerPopup
        BringLauncherToFront();
        return;
    }
    AcceptDiscordJoin(server, nick);
}

/// A JSON string field with its escapes decoded (\" \\ \/ \n \t \uXXXX, surrogate pairs), as
/// UTF-8; "" when missing.
std::string JsonText(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":\"";
    size_t i = body.find(field);
    if (i == std::string::npos) return {};
    i += field.size();
    std::string out;
    const auto put = [&out](unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    };
    const auto hex4 = [&body](size_t at, unsigned& v) {
        if (at + 4 > body.size()) return false;
        v = 0;
        for (size_t k = at; k < at + 4; ++k) {
            const char c = body[k];
            v = v * 16 + unsigned(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
            if (v > 0xFFFF) return false;
        }
        return true;
    };
    for (; i < body.size() && body[i] != '"'; ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out += body[i];
            continue;
        }
        const char e = body[++i];
        if (e == 'n') out += ' ';
        else if (e == 't') out += ' ';
        else if (e == 'u') {
            unsigned cp = 0, low = 0;
            if (!hex4(i + 1, cp)) break;
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u' &&
                hex4(i + 3, low) && low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            }
            put(cp);
        } else {
            out += e;  // \" \ \/
        }
    }
    return out;
}

/// The digits of a JSON number field ("" when missing); IDs past 32 bits stay exact.
std::string JsonDigits(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":";
    const auto at = body.find(field);
    if (at == std::string::npos) return {};
    size_t end = at + field.size();
    while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
    return body.substr(at + field.size(), end - at - field.size());
}

/// Server announcements ("maintenance in 5 minutes"): /ryunet/announce, asked every 30 s while
/// online play is on. A new one goes to the game in ryunet_announce.txt next to bbport.ini
/// (the port draws it over the picture, gpu/shim/bbport_overlay.cpp) and to the launcher's
/// notice line.
struct AnnouncePoll {
    std::mutex mutex;
    ULONGLONG asked = 0;
    bool in_flight = false;
    std::string seen_id;   // the last announcement handed on
    std::string pending;   // its text, for the launcher's notice (UI thread)
};
AnnouncePoll g_announce;

void PollAnnouncement() {
    App& a = g_app;
    if (!a.game.Flag("online", true)) return;
    const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    std::string text;
    {
        std::scoped_lock lock{g_announce.mutex};
        text.swap(g_announce.pending);
        if (!g_announce.in_flight && !server.empty() && GetTickCount64() - g_announce.asked >= 30000) {
            g_announce.in_flight = true;
            g_announce.asked = GetTickCount64();
            const fs::path file = GameIni().parent_path() / L"ryunet_announce.txt";
            std::thread([server, file] {
                const std::string body = HttpGet(server, L"/ryunet/announce");
                const std::string id = JsonDigits(body, "id"), msg = JsonText(body, "text");
                int expires_in = 0;
                JsonNumber(body, "expires_in", expires_in);
                std::scoped_lock done{g_announce.mutex};
                g_announce.in_flight = false;
                if (id.empty() || id == "0" || msg.empty() || expires_in <= 0 || id == g_announce.seen_id) return;
                g_announce.seen_id = id;
                g_announce.pending = msg;
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << "id=" << id << "\nlevel=" << JsonText(body, "level") << "\nexpires="
                    << int64_t(std::time(nullptr)) + expires_in << "\ntext=" << msg << "\n";
            }).detach();
        }
    }
    if (!text.empty()) a.notice = std::string(T("Aviso del servidor: ", "Server notice: ")) + text;
}

/// Called every frame: picks up finished background work.
void ApplyRecoveredNick();

void PollJobs() {
    App& a = g_app;
    UpdateDiscord();
    ApplyRecoveredNick();
    PollAnnouncement();
    DiscordPresence::SetAcceptJoinRequests(a.launcher.Flag("discord_accept_requests", true));
    for (std::string secret; DiscordPresence::TakeJoin(secret);) OnDiscordJoin(secret);
    if (a.pending_play && !a.game_running && GameReady()) {
        a.pending_play = false;
        PressPlay();
    }
    if (a.probe_done && a.probe.joinable()) a.probe.join();
    if (!a.job_finished) return;
    const Job finished = a.job.exchange(Job::None);
    a.job_finished = false;
    JoinWorker();
    if (finished == Job::Merge) {
        std::string error;
        {
            std::scoped_lock lock{a.mutex};
            error = a.job_error;
        }
        if (error.empty()) {
            SaveLauncher();
            StartCheck();
        } else {
            a.notice = error;
        }
    } else if (finished == Job::Play) {
        CloseHandle(a.process);
        a.process = nullptr;
        if (a.job_object) CloseHandle(a.job_object);
        a.job_object = nullptr;
        a.game_running = false;
        a.show_exit = true;
        a.game.Load(GameIni());  // the in-game menu may have changed settings
        SyncOnlineBuffers();
        {
            std::scoped_lock lock{g_nick_check.mutex};  // a free nickname is now ours
            g_nick_check.results.clear();
        }
        a.saves_dirty = true;
        if (a.hwnd) {
            if (a.launcher.Flag("close_on_play", false)) {
                PostQuitMessage(0);
            } else {
                ShowWindow(a.hwnd, SW_RESTORE);
                SetForegroundWindow(a.hwnd);
            }
        }
    }
}

void StartProbe() {
    App& a = g_app;
    if (a.probe.joinable()) a.probe.join();
    a.probe_done = false;
    a.probe = std::thread([] {
        App& app = g_app;
        const fs::path tool = app.bin / L"bb-gpu-capabilities.exe";
        std::vector<std::pair<std::string, std::string>> displays, pads;
        const Captured d = RunCaptured(tool.wstring(), Quote(tool.wstring()) + L" --displays", app.bin, 10000);
        std::stringstream ds(d.output);
        std::string line;
        int number = 0;
        while (std::getline(ds, line)) {
            line = Trim(line);
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            ++number;
            std::string label = std::to_string(number) + ": " + line.substr(0, t1) + " (" +
                                line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1) + ")";
            if (t2 != std::string::npos && line.substr(t2 + 1) == "1") label += g_spanish ? " - principal" : " - primary";
            displays.emplace_back(std::to_string(number), label);
        }
        const Captured p = RunCaptured(tool.wstring(), Quote(tool.wstring()) + L" --gamepads", app.bin, 10000);
        std::stringstream ps(p.output);
        while (std::getline(ps, line)) {
            line = Trim(line);
            const auto t = line.find('\t');
            if (t != std::string::npos) pads.emplace_back(line.substr(0, t), line.substr(t + 1));
        }
        std::scoped_lock lock{app.mutex};
        app.displays = std::move(displays);
        app.gamepads = std::move(pads);
        app.probe_done = true;
    });
}

void ChooseFolder(fs::path& slot, FolderInfo& info, const wchar_t* title) {
    App& a = g_app;
    fs::path start = slot.empty() ? (a.base_dir.empty() ? a.root : a.base_dir.parent_path()) : slot.parent_path();
    auto picked = PickFolder(a.hwnd, title, start);
    if (!picked) return;
    *picked = GameRoot(*picked);
    const FolderInfo found = Analyze(*picked);
    // Whatever was picked goes where it belongs.
    switch (found.kind) {
    case Kind::Complete:
        a.game_dir = *picked;
        a.game_info = found;
        SaveLauncher();
        if (found.split) {
            StartMerge({*picked});  // Image0 + Sc0 -> the PS4 layout, next to it
        } else {
            StartCheck();
        }
        return;
    case Kind::Base:
        a.base_dir = *picked;
        a.base_info = found;
        break;
    case Kind::Update:
        a.update_dir = *picked;
        a.update_info = found;
        break;
    default:
        slot = *picked;
        info = found;
        break;
    }
    SaveLauncher();
}

// ---------------------------------------------------------------------------------------------
// Saves: <port>\user\savedata\<user>\CUSA13795\<dir> holds the game's files and <dir>.sce_sys its
// param.bin (title, subtitle, detail) and icon0.png, as src/runtime_savedata.c keeps them.






std::string FileTimeText(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return {};
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string Fixed(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

std::string FormatSize(std::uintmax_t bytes) {
    char buf[64];
    if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

fs::path SaveRoot() {
    const fs::path base = g_app.root / L"user" / L"savedata";
    std::error_code ec;
    // Player 1's; guest<N> holds split-screen players 2-4's, emptied each run (src/runtime_savedata.c).
    if (fs::is_directory(base / L"1" / GameTitle(), ec)) return base / L"1" / GameTitle();
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->path().filename().wstring().rfind(L"guest", 0) == 0) continue;
        if (fs::is_directory(it->path() / GameTitle(), e2)) return it->path() / GameTitle();
    }
    return base / L"1" / GameTitle();
}

fs::path BackupRoot() { return g_app.root / L"backups"; }

bool IsMetaName(const std::wstring& name) {
    auto ends = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(L".sce_sys") || ends(L".memory");
}

std::vector<SaveSlot> ListSaves() {
    std::vector<SaveSlot> slots;
    const fs::path root = SaveRoot();
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || IsMetaName(it->path().filename().wstring())) continue;
        SaveSlot s;
        s.dir = PathText(it->path().filename());
        s.bytes = FolderBytes(it->path());
        fs::path meta = it->path();
        meta += L".sce_sys";
        const std::string raw = ReadText(meta / L"param.bin");
        if (raw.size() >= sizeof(SaveParam)) {
            SaveParam p;
            std::memcpy(&p, raw.data(), sizeof p);
            s.title = Fixed(p.title, sizeof p.title);
            s.subtitle = Fixed(p.subtitle, sizeof p.subtitle);
            s.detail = Fixed(p.detail, sizeof p.detail);
        }
        s.modified = FileTimeText(it->path());
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::error_code e3;
            if (f->is_regular_file(e3)) {
                const std::string t = FileTimeText(f->path());
                if (t > s.modified) s.modified = t;
            }
        }
        slots.push_back(std::move(s));
    }
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) { return a.dir < b.dir; });
    return slots;
}

std::vector<Backup> ListBackups() {
    std::vector<Backup> list;
    std::error_code ec;
    for (fs::directory_iterator it(BackupRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = PathText(it->path().filename());
        if (!it->is_directory(e2) || name.rfind("saves-", 0) != 0) continue;
        list.push_back({it->path(), name, FileTimeText(it->path()), FolderBytes(it->path())});
    }
    std::sort(list.begin(), list.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    return list;
}

bool HasSaves() {
    std::error_code ec;
    return fs::is_directory(SaveRoot(), ec) && !fs::is_empty(SaveRoot(), ec);
}

std::string CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return ec ? PathText(from) + " -> " + PathText(to) + ": " + ec.message() : std::string();
}

/// A copy of all saves in backups\saves-<date>[-label]; empty error when there is nothing to copy.
std::string BackupSaves(const char* label, fs::path* made = nullptr) {
    if (!HasSaves()) return {};
    fs::path target = BackupRoot() / Widen("saves-" + Stamp() + (label ? std::string("-") + label : ""));
    const std::string error = CopyTree(SaveRoot(), target);
    if (error.empty() && made) *made = target;
    return error;
}

std::string RestoreBackup(const fs::path& backup) {
    std::string error = BackupSaves("auto");
    if (!error.empty()) return error;
    std::error_code ec;
    fs::remove_all(SaveRoot(), ec);
    if (ec) return PathText(SaveRoot()) + ": " + ec.message();
    return CopyTree(backup, SaveRoot());
}

/// A save folder from elsewhere: this launcher's backup or another PC's CUSA13795 folder (copied
/// as it is), or a decrypted PS4 save (Apollo Save Tool, Save Wizard: its files plus
/// sce_sys\param.sfo), converted to the runtime's layout.
std::string ImportSaves(const fs::path& from) {
    std::error_code ec;
    const fs::path sfo_path = from / L"sce_sys" / L"param.sfo";
    if (fs::is_regular_file(sfo_path, ec)) {
        const auto sfo = ReadSfo(sfo_path);
        auto get = [&](const char* k) {
            const auto it = sfo.find(k);
            return it == sfo.end() ? std::string() : it->second;
        };
        std::string dir = get("SAVEDATA_DIRECTORY");
        if (dir.empty() || dir.size() >= 32 || dir.find_first_of("\\/:") != std::string::npos) {
            return T("param.sfo no dice en qué ranura va la partida (SAVEDATA_DIRECTORY).",
                     "param.sfo does not name the save slot (SAVEDATA_DIRECTORY).");
        }
        std::string error = BackupSaves("antes-de-importar");
        if (!error.empty()) return error;
        const fs::path target = SaveRoot() / Widen(dir);
        fs::path meta = target;
        meta += L".sce_sys";
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);
        fs::create_directories(meta, ec);
        for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == L"sce_sys") continue;
            std::error_code e2;
            fs::copy(it->path(), target / it->path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
            if (e2) return PathText(it->path()) + ": " + e2.message();
        }
        SaveParam p{};
        std::snprintf(p.title, sizeof p.title, "%s", get("MAINTITLE").c_str());
        std::snprintf(p.subtitle, sizeof p.subtitle, "%s", get("SUBTITLE").c_str());
        std::snprintf(p.detail, sizeof p.detail, "%s", get("DETAIL").c_str());
        p.mtime = int64_t(time(nullptr));
        if (!WriteTextAtomic(meta / L"param.bin", std::string(reinterpret_cast<const char*>(&p), sizeof p))) {
            return T("No se pudo escribir param.bin.", "Could not write param.bin.");
        }
        fs::copy_file(from / L"sce_sys" / L"icon0.png", meta / L"icon0.png", fs::copy_options::overwrite_existing, ec);
        return {};
    }
    // The runtime's layout: <dir> next to <dir>.sce_sys.
    bool ours = false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path meta = it->path();
        meta += L".sce_sys";
        std::error_code e2;
        if (!IsMetaName(it->path().filename().wstring()) && fs::is_directory(meta, e2)) ours = true;
    }
    if (!ours) {
        return T("No reconozco esa carpeta: elige una copia de seguridad, la carpeta CUSA13795 de las partidas de "
                 "otro PC o una partida de PS4 descifrada (con sce_sys\\param.sfo).",
                 "This folder is not recognized: choose a backup, the CUSA13795 saves folder from another PC or a "
                 "decrypted PS4 save (with sce_sys\\param.sfo).");
    }
    std::string error = BackupSaves("antes-de-importar");
    if (!error.empty()) return error;
    return CopyTree(from, SaveRoot());
}

// ---------------------------------------------------------------------------------------------
// Patches: every eboot patch for CUSA13795 01.21 in patches\*.xml (shadPS4/GoldHEN XML format).
// The three community patches keep their bbport.ini keys (shared with the in-game menu); others
// are switched in patches.json ({"enabled": [...], "disabled": [...]}), as scripts/patches.py reads.



constexpr std::pair<const char*, const char*> kIniPatches[] = {
    {"CrashTeamRacing.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacing.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
    {"CrashTeamRacingUnlockAll.xml/Unlock Everything", "patch_unlock_all"},
    // The European edition's files: the same switches (scripts/patches.py BUNDLED_FILES).
    {"CrashTeamRacingUnlockAllEU.xml/Unlock Everything", "patch_unlock_all"},
    {"CrashTeamRacingEU.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacingEU.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacingEU.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
};

/// "USA" or "EU" for a CTR title ID, "" otherwise.
const char* RegionOf(const std::string& title) {
    if (title == "CUSA13795") return "USA";
    if (title == "CUSA14876") return "EU";
    return "";
}

std::string XmlUnescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#10;", "\n"}, {"&#13;", "\r"}, {"&#9;", "\t"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to))
            s.replace(at, std::strlen(from), to);
    }
    return s;
}

std::map<std::string, std::string> XmlAttributes(const std::string& tag) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < tag.size()) {
        const size_t eq = tag.find('=', i);
        if (eq == std::string::npos) break;
        size_t name_end = eq;
        while (name_end > i && (tag[name_end - 1] == ' ' || tag[name_end - 1] == '\n' || tag[name_end - 1] == '\r' || tag[name_end - 1] == '\t')) --name_end;
        size_t name_start = name_end;
        while (name_start > i && !std::isspace(static_cast<unsigned char>(tag[name_start - 1])) && tag[name_start - 1] != '<') --name_start;
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) break;
        const char quote = tag[q];
        const size_t close = tag.find(quote, q + 1);
        if (close == std::string::npos) break;
        out[tag.substr(name_start, name_end - name_start)] = XmlUnescape(tag.substr(q + 1, close - q - 1));
        i = close + 1;
    }
    return out;
}

void ScanModPackages() {
    App& a = g_app;
    if (!a.python || a.game_running) return;
    const fs::path script = a.root / L"scripts" / L"mod_packages.py";
    if (!fs::is_regular_file(script)) return;
    const std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " +
        Quote(script.wstring()) + L" --scan " + Quote(a.root.wstring());
    const Captured r = RunCaptured(a.python->exe, cmd, a.root, 120000);
    if (!r.output.empty()) AddLog(r.output);
    if (!r.started || r.code != 0) AddLog("Mod scan failed; check the log before enabling packages.");
}

std::vector<PatchInfo> LoadPatches() {
    ScanModPackages();
    std::vector<PatchInfo> list;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(g_app.root / L"patches", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".xml") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string xml = ReadText(file);
        // Files naming other games are skipped; a file without <ID> applies to any.
        bool any_id = false, ours = false;
        std::string file_title;
        for (size_t at = 0; (at = xml.find("<ID>", at)) != std::string::npos; at += 4) {
            const size_t close = xml.find("</ID>", at);
            if (close == std::string::npos) break;
            any_id = true;
            const std::string id = Trim(xml.substr(at + 4, close - at - 4));
            if (IsCtrTitle(id)) {
                ours = true;
                if (file_title.empty()) file_title = id;
            }
        }
        if (any_id && !ours) continue;
        for (size_t at = 0; (at = xml.find("<Metadata", at)) != std::string::npos; ++at) {
            const size_t close = xml.find('>', at);
            if (close == std::string::npos) break;
            const auto attr = XmlAttributes(xml.substr(at + 9, close - at - 9));
            auto get = [&](const char* k) {
                const auto it = attr.find(k);
                return it == attr.end() ? std::string() : it->second;
            };
            const std::string elf = get("AppElf");
            if (get("AppVer") != kUpdateVersion || !(elf.empty() || elf == "eboot.bin")) continue;
            PatchInfo p;
            p.name = get("Name");
            p.key = PathText(file.filename()) + "/" + p.name;
            p.author = get("Author");
            p.note = get("Note");
            p.version = get("PatchVer");
            p.title = file_title;
            std::string on = get("isEnabled");
            std::transform(on.begin(), on.end(), on.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            p.default_on = on == "true";
            p.mod_name = get("ModName");
            p.mod_hash = get("ModHash");
            p.mod_source = get("ModSource");
            p.mod_description = get("ModDescription");
            p.mod_versions = get("ModVersions");
            for (const auto& [key, ini] : kIniPatches)
                if (p.key == key) p.ini_key = ini;
            list.push_back(std::move(p));
        }
    }
    return list;
}

/// patches.json's "enabled" and "disabled" string lists (the file only holds those).
void ReadPatchConfig(std::vector<std::string>& enabled, std::vector<std::string>& disabled) {
    enabled.clear();
    disabled.clear();
    const std::string json = ReadText(g_app.root / L"patches.json");
    auto read_list = [&](const char* name, std::vector<std::string>& out) {
        size_t at = json.find(std::string("\"") + name + "\"");
        if (at == std::string::npos) return;
        at = json.find('[', at);
        const size_t end = at == std::string::npos ? at : json.find(']', at);
        if (at == std::string::npos || end == std::string::npos) return;
        for (size_t i = at + 1; i < end; ++i) {
            if (json[i] != '"') continue;
            std::string value;
            for (++i; i < end && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < end) ++i;
                value += json[i];
            }
            out.push_back(value);
        }
    };
    read_list("enabled", enabled);
    read_list("disabled", disabled);
}

void WritePatchConfig(const std::vector<std::string>& enabled, const std::vector<std::string>& disabled) {
    auto list = [](const std::vector<std::string>& items) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            out += i ? ", \"" : "\"";
            for (const char c : items[i]) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        }
        return out + "]";
    };
    WriteTextAtomic(g_app.root / L"patches.json",
                    "{\n  \"enabled\": " + list(enabled) + ",\n  \"disabled\": " + list(disabled) + "\n}\n");
}

// ---------------------------------------------------------------------------------------------
// UI

std::string Describe(const FolderInfo& f) {
    switch (f.kind) {
    case Kind::Empty: return T("sin elegir", "not chosen");
    case Kind::Missing: return T("la carpeta no existe", "the folder does not exist");
    case Kind::NotGame:
        return T("no tiene eboot.bin y sce_sys\\param.sfo (ni Image0 y Sc0)", "no eboot.bin and sce_sys\\param.sfo (nor Image0 and Sc0)");
    case Kind::OtherTitle: return std::string(T("es otro juego o región: ", "another game or region: ")) + f.title;
    case Kind::OtherVersion: return std::string(T("actualización ", "update ")) + f.version + T(" (se necesita la 01.21)", " (01.21 is needed)");
    case Kind::Base: return std::string(T("juego base ", "base game ")) + f.title + " v" + f.version;
    case Kind::Update: return std::string(T("actualización ", "update ")) + f.version + " (" + FormatBytes(f.bytes) + ")";
    case Kind::Complete: return std::string(f.title) + " v" + f.version + " (" + FormatBytes(f.bytes) + ")";
    }
    return {};
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GameFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.game.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.game.Set(key, v ? "1" : "0");
        SaveGameIni();
    }
    if (help) HelpMarker(help);
}

void LauncherFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.launcher.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.launcher.Set(key, v ? "1" : "0");
        SaveLauncher();
    }
    if (help) HelpMarker(help);
}

template <size_t N>
void ChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback) {
    const std::string current = g_app.launcher.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    if (ImGui::BeginCombo(label, T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected)) {
                g_app.launcher.Set(key, c.value);
                SaveLauncher();
            }
        }
        ImGui::EndCombo();
    }
}

bool CreateDesktopShortcut(std::string& out_error);
bool AddToSteam(std::string& out_msg);
void DrawMenuIcon(ImDrawList* d, int id, ImVec2 c, ImU32 ink, float s);

bool PrimaryPlayButton(const char* label, ImVec2 size, bool disabled) {
    const bool pressed = ImGui::InvisibleButton("##primary_play", size);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 actual_size = ImGui::GetItemRectSize();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float press_offset = active ? 2.0f : 0.0f;
    const float y0 = a.y + 2.0f + press_offset;
    const float y1 = a.y + actual_size.y - 2.0f + press_offset;
    const float cy = (y0 + y1) * 0.5f;
    const UiTheme& th = CurrentTheme();
    ImDrawList* d = ImGui::GetWindowDrawList();

    const ImVec2 r0{a.x + 4.0f, y0};
    const ImVec2 r1{a.x + actual_size.x - 4.0f, y1};
    const float rounding = 8.0f * g_ui_scale;
    if (!active) {
        d->AddRectFilled({r0.x, r0.y + 4.0f}, {r1.x, r1.y + 4.0f},
                         IM_COL32(0, 0, 0, disabled ? 80 : 180), rounding);
    }
    const ImU32 col_left = disabled ? IM_COL32(55, 60, 72, 200)
                                    : Col(hovered ? th.button_active : th.accent, 1.0f);
    const ImU32 col_right = disabled ? IM_COL32(45, 50, 62, 200)
                                     : Col(hovered ? th.accent_alt : th.button_hover, 1.0f);
    d->AddRectFilledMultiColor(r0, r1, col_left, col_right, col_right, col_left);
    d->AddRect(r0, r1, disabled ? IM_COL32(110, 118, 135, 120) : Col(th.accent_alt, hovered ? 0.95f : 0.72f),
               rounding, 0, hovered ? 2.2f : 1.5f);
    d->AddLine({r0.x + 14.0f, y0 + 3.0f}, {r1.x - 14.0f, y0 + 3.0f},
               disabled ? IM_COL32(255, 255, 255, 35) : IM_COL32(255, 255, 255, 130), 1.0f);

    const ImU32 icon_col = disabled ? IM_COL32(140, 148, 165, 200) : IM_COL32(18, 18, 24, 255);
    DrawMenuIcon(d, 0, {a.x + 36.0f, cy}, icon_col, 1.0f);
    ImFont* font = g_ctr_font ? g_ctr_font : ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 1.08f;
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
    const ImVec2 text_pos{a.x + (actual_size.x - text_size.x) * 0.5f, cy - text_size.y * 0.5f};
    if (!disabled) {
        d->AddText(font, font_size, {text_pos.x + 1.0f, text_pos.y + 1.0f}, IM_COL32(255, 255, 255, 90), label);
    }
    d->AddText(font, font_size, text_pos,
               disabled ? IM_COL32(145, 152, 168, 210) : IM_COL32(14, 14, 20, 255), label);
    return pressed;
}

void PlayTab() {
    App& a = g_app;
    const bool busy = a.job != Job::None;
    ImGui::SeparatorText(T("Juego", "Game"));

    const bool ready = a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok;
    if (!a.python) {
        ImGui::TextColored(kRed, "%s", T("Falta Python 3: no se encontró python\\python.exe ni Python instalado.",
                                         "Python 3 is missing: no python\\python.exe and no installed Python."));
    }
    if (a.game_info.kind == Kind::Complete) {
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::TextColored(kYellow, "%s", T("Preparando el juego...", "Preparing the game..."));
            ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0), overlay);
        } else if (a.game_info.split) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kYellow, "%s",
                               T("Esta carpeta tiene el formato de PS4 PKG Tool (Image0 y Sc0). Hay que prepararla: "
                                 "se crea una carpeta nueva con enlaces duros, sin tocar la tuya.",
                                 "This folder has the PS4 PKG Tool layout (Image0 and Sc0). It needs preparing: a "
                                 "new folder of hard links is created, yours is not changed."));
            ImGui::PopTextWrapPos();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(T("Preparar el juego", "Prepare the game"))) StartMerge({a.game_dir});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"), PathText(MergeTarget(a.game_dir)).c_str());
        } else if (!a.game_checked) {
            ImGui::TextColored(kYellow, "%s", T("Comprobando el juego...", "Checking the game..."));
        } else if (a.game_ok) {
            ImGui::TextColored(kGreen, "%s %s", T("Listo:", "Ready:"), Describe(a.game_info).c_str());
            if (a.game.Flag("online", true)) {
                std::string srv = a.game.Get("online_server", "octr.ryu7w7.xyz");
                if (srv.empty()) srv = "octr.ryu7w7.xyz";
                const std::string nick = CurrentNick().empty() ? std::string("-") : CurrentNick();
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "[Online] %s: %s  |  %s: %s",
                                   T("Servidor", "Server"), srv.c_str(), T("Apodo", "Nick"), nick.c_str());
                {
                    ImVec4 nat_color;
                    const std::string nat = NatLabel(Trim(srv), nat_color);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "  |  ");
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(nat_color, "%s", nat.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", T("Si otros jugadores pueden conectarse directamente contigo. Más detalles en la "
                                                  "pestaña Online. Tu IP no se muestra.",
                                                  "Whether other players can connect to you directly. Details in the Online "
                                                  "tab. Your IP is not shown."));
                }
                if (!NickInfo().banned.empty()) ImGui::TextColored(kRed, "%s", T("Baneado del online: mira la pestaña Online.", "Banned from online play: see the Online tab."));
                if (const char* problem = NickProblem(CurrentNick()))
                    ImGui::TextColored(kYellow, "%s %s", problem, T("Se te pedirá al pulsar Jugar.", "You will be asked when you press Play."));
            } else {
                ImGui::TextColored(kGrey, "[Online] %s", T("Modo offline (actívalo en la pestaña Online)", "Offline mode (enable it in the Online tab)"));
            }
        } else {
            ImGui::TextColored(kRed, "%s", T("Estos archivos no sirven:", "These files do not work:"));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(a.check_message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextColored(kGrey, "%s", PathText(a.game_dir).c_str());
        if (ImGui::Button(T("Cambiar carpeta del juego...", "Change game folder...")) && !busy && !a.game_running) {
            ChooseFolder(a.game_dir, a.game_info, L"CUSA13795 + 1.21");
        }
    } else {
        if (a.game_info.kind == Kind::Update) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("solo tiene la actualización 1.21, falta el juego base",
                                 "only has the 1.21 update, the base game is missing"));
        } else if (a.game_info.kind == Kind::Base) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("es el juego base sin la actualización 1.21", "is the base game without the 1.21 update"));
        } else if (a.game_info.kind != Kind::Empty) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(), Describe(a.game_info).c_str());
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("Necesitas tu propio dump de PS4: el juego base (CUSA14876 en Europa, CUSA13795 en "
                                 "América) y la actualización 1.21. Si ya los tienes juntos en una carpeta, elige esa.",
                                 "You need your own PS4 dump: the base game (CUSA14876 in Europe, CUSA13795 in the "
                                 "Americas) and the 1.21 update. If they are already merged in one folder, choose that one."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::BeginTable("setup", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthFixed);
            auto row = [&](const char* what, fs::path& slot, FolderInfo& info, Kind good, const wchar_t* title, const char* id) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(what);
                ImGui::TableNextColumn();
                const bool ok = info.kind == good;
                ImGui::TextColored(ok ? kGreen : (info.kind == Kind::Empty ? kGrey : kRed), "%s", Describe(info).c_str());
                if (!slot.empty()) ImGui::TextColored(kGrey, "%s", PathText(slot).c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(id);
                if (ImGui::Button(T("Elegir...", "Choose...")) && !busy) ChooseFolder(slot, info, title);
                ImGui::PopID();
            };
            row(T("Juego base", "Base game"), a.base_dir, a.base_info, Kind::Base, L"CUSA13795", "base");
            row(T("Actualización 1.21", "Update 1.21"), a.update_dir, a.update_info, Kind::Update, L"CUSA13795-patch", "update");
            ImGui::EndTable();
        }
        const bool can_merge = a.base_info.kind == Kind::Base && a.update_info.kind == Kind::Update;
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            const float fraction = total ? float(done) / float(total) : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        } else {
            ImGui::BeginDisabled(!can_merge || busy);
            if (ImGui::Button(T("Preparar el juego (base + 1.21)", "Prepare the game (base + 1.21)"))) {
                StartMerge({a.base_dir, a.update_dir});
            }
            ImGui::EndDisabled();
            if (can_merge) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"),
                                   PathText(MergeTarget(a.base_dir)).c_str());
            }
            HelpMarker(T("Une la actualización con el juego base en una carpeta nueva usando enlaces duros: no ocupa "
                         "espacio extra ni cambia tus carpetas originales (si están en discos distintos, se copia).",
                         "Merges the update over the base game into a new folder with hard links: no extra disk "
                         "space and your original folders are not changed (copied if they are on different drives)."));
        }
        ImGui::Spacing();
        if (ImGui::Button(T("Ya tengo el juego preparado...", "I already have the merged game...")) && !busy) {
            ChooseFolder(a.game_dir, a.game_info, L"CUSA13795-1.21");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const float play_height = ImGui::GetFontSize() * 3.0f;
    if (a.game_running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.14f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.20f, 0.28f, 1.0f));
        if (ImGui::Button(T("Cerrar el juego", "Stop the game"), ImVec2(-FLT_MIN, play_height))) StopGame();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::BeginDisabled(!ready || busy || !a.python);
        ImGui::PushFont(g_ctr_font, ImGui::GetStyle().FontSizeBase * 1.5f);
        if (PrimaryPlayButton(T("JUGAR", "PLAY"), ImVec2(-FLT_MIN, play_height),
                              !ready || busy || !a.python)) PressPlay();
        ImGui::PopFont();
        ImGui::EndDisabled();
    }

    if (ready && !a.game_running) {
        ImGui::Spacing();
        if (ImGui::Button(T("Crear acceso directo en el Escritorio", "Create Desktop Shortcut"))) {
            std::string err;
            if (CreateDesktopShortcut(err)) {
                a.notice = T("Acceso directo creado en el Escritorio con éxito (inicia directo sin el launcher).",
                             "Desktop shortcut created successfully (launches directly without launcher).");
            } else {
                a.notice = err;
            }
        }
    }

    std::string status;
    {
        std::scoped_lock lock{a.mutex};
        status = a.last_status;
    }
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s", T("El juego está abierto. El primer arranque compila shaders y tarda un poco.",
                                            "The game is running. The first start compiles shaders and takes a while."));
    } else if (a.show_exit) {
        if (a.exit_code == 0 || a.stopped) ImGui::TextColored(kGrey, "%s", T("El juego se cerró.", "The game was closed."));
        else if (a.exit_code == kUnsupportedGpuCode) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextWrapped("%s", T("Tu tarjeta gráfica o su driver no soporta Vulkan 1.3 / sparse binding, que el juego necesita. Actualiza el "
                                       "driver de la GPU; si ya es el último, la tarjeta es demasiado antigua para Windows (en "
                                       "Linux, el driver Mesa da Vulkan 1.3 a muchas GPU antiguas).",
                                       "Your graphics card or its driver doesn't support Vulkan 1.3 / sparse binding, which the game needs. Update "
                                       "the GPU driver; if it is already the latest, the card is too old for Windows (on Linux, "
                                       "the Mesa driver gives Vulkan 1.3 to many older GPUs)."));
            ImGui::PopStyleColor();
        }
        else ImGui::TextColored(kRed, T("El juego se cerró con un error (código %lu). Mira el registro abajo o en last_run.log.",
                                        "The game ended with an error (code %lu). See the log below or last_run.log."),
                                (unsigned long)a.exit_code);
    }
    if (!status.empty()) ImGui::TextColored(kGrey, "%s", status.c_str());

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Registro", "Log"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::scoped_lock lock{a.mutex};
            ImGuiListClipper clipper;
            clipper.Begin(int(a.log.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(a.log[size_t(i)].c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

// bbport.ini render_res: the scene size (scripts/patches.py, run.py; docs/sacredfire/13-resolution.md).
struct RenderRes {
    const char* value;
    const char* es;
    const char* en;
};
constexpr RenderRes kRenderRes[] = {
    {"480", "480p (854 x 480, PCs muy modestos)", "480p (854 x 480, very low-end PCs)"},
    {"720", "720p (1280 x 720, PCs modestos)", "720p (1280 x 720, low-end PCs)"},
    {"1080", "1080p (1920 x 1080)", "1080p (1920 x 1080)"},
    {"1440", "1440p (2560 x 1440, PS4 Pro)", "1440p (2560 x 1440, PS4 Pro)"},
    {"1620", "1620p (2880 x 1620)", "1620p (2880 x 1620)"},
    {"2160", "4K (3840 x 2160)", "4K (3840 x 2160)"},
};

// The render_res in effect: the setting, or for older settings the Pro mode and 1080p patch.
std::string EffectiveRenderRes() {
    const std::string v = g_app.game.Get("render_res");
    for (const auto& r : kRenderRes)
        if (v == r.value) return v;
    return "1080";
}

/// bbport.ini temporal (the upscaler), with the keys of v0.3 test builds before it: fsr3=1, fsr1=0.
std::string GameTemporal() {
    const Ini& game = g_app.game;
    const std::string value = game.Get("temporal");
    for (const char* known : {"off", "fsr1", "fsr3", "dlss", "fsr4"})
        if (value == known) return value;
    if (game.Flag("fsr3", false)) return "fsr3";
    return game.Flag("fsr1", true) ? "fsr1" : "off";
}

/// v0.7.4: FSR 4's model files (the fsr4_shaders folder next to the launcher, where sacredfire.exe
/// looks for them); without them the launcher does not offer it.
bool Fsr4FilesPresent() {
    static const bool present = [] {
        std::error_code ec;
        return fs::is_regular_file(g_app.root / L"fsr4_shaders" / L"LICENSE-FSR4-v07.txt", ec);
    }();
    return present;
}

/// A combo over a bbport.ini key; true when the player picked another value (already set).
template <size_t N>
bool GameChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback,
                     float width_em) {
    App& a = g_app;
    const std::string current = a.game.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    if (ImGui::BeginCombo(label, T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected) && &c != selected) {
                a.game.Set(key, c.value);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RenderResCombo() {
    App& a = g_app;
    const std::string current = EffectiveRenderRes();
    const RenderRes* selected = &kRenderRes[0];
    for (const auto& r : kRenderRes)
        if (current == r.value) selected = &r;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(T("Resolución interna", "Internal resolution"), T(selected->es, selected->en))) {
        for (const auto& r : kRenderRes) {
            if (ImGui::Selectable(T(r.es, r.en), &r == selected)) {
                a.game.Set("render_res", r.value);
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
    HelpMarker(T("El tamaño al que el juego dibuja la escena 3D. Por encima de 1080p usa el modo PS4 Pro del juego "
                 "y la imagen sale en 4K: en una pantalla 1080p eso quita los dientes de sierra. "
                 "4K necesita una GPU potente (una RTX 3070 va a unos 100 FPS); 720p y 480p alivian PCs modestos "
                 "(la imagen sale a 1080p).",
                 "The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path (turned "
                 "on automatically) and the picture is output at 4K: on a 1080p screen that removes jagged "
                 "edges. 4K needs a strong GPU (an RTX 3070 runs it at about 100 FPS); 720p and 480p help low-end "
                 "PCs (the picture is output at 1080p)."));
}

// Aspect ratio is independent of the corrected widescreen layouts: 16:10 prepares the
// display/camera in scripts/aspect_ratio.py; 21:9/32:9 keep tcosi's menus and HUD.
void AspectRatioSettings() {
    App& a = g_app;
    std::string mode = a.game.Get("aspect_ratio", a.game.Get("widescreen_layout", "16x9"));
    static constexpr Choice ratios[] = {{"16:9", "16:9", "16x9"},
                                        {"16:10 (experimental)", "16:10 (experimental)", "16x10"},
                                        {"21:9", "21:9", "21x9"}, {"32:9", "32:9", "32x9"}};
    if (mode != "16x10" && mode != "21x9" && mode != "32x9") mode = "16x9";
    const char* preview = "16:9";
    for (const auto& c : ratios) if (mode == c.value) preview = T(c.es, c.en);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(T("Relación de aspecto", "Aspect ratio"), preview)) {
        for (const auto& c : ratios) {
            if (ImGui::Selectable(T(c.es, c.en), mode == c.value)) {
                mode = c.value;
                a.game.Set("aspect_ratio", mode);
                a.game.Set("widescreen_layout", mode == "21x9" || mode == "32x9" ? mode : "off");
                if (mode != "16x9") {
                    a.game.Set("ultrawide", "0");
                    std::string output = a.game.Get("aspect_output", a.game.Get("widescreen_output"));
                    const bool tall = output == "1920x1200" || output == "1280x800" || output == "2560x1600";
                    const bool wide = output == "3440x1440" || output == "2560x1080";
                    const bool super = output == "5120x1440" || output == "3840x1080" ||
                                       output == "2560x720" || output == "7680x2160";
                    if (!((mode == "16x10" && tall) || (mode == "21x9" && wide) || (mode == "32x9" && super)))
                        output = mode == "16x10" ? "1920x1200" : mode == "21x9" ? "3440x1440" : "5120x1440";
                    a.game.Set("aspect_output", output);
                    if (mode == "21x9" || mode == "32x9") a.game.Set("widescreen_output", output);
                }
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
    if (mode == "16x9") return;
    static constexpr Choice tall[] = {{"1920 x 1200", "1920 x 1200", "1920x1200"},
                                      {"1280 x 800", "1280 x 800", "1280x800"},
                                      {"2560 x 1600", "2560 x 1600", "2560x1600"}};
    static constexpr Choice wide[] = {{"3440 x 1440", "3440 x 1440", "3440x1440"},
                                      {"2560 x 1080", "2560 x 1080", "2560x1080"}};
    static constexpr Choice super[] = {{"5120 x 1440", "5120 x 1440", "5120x1440"},
                                       {"3840 x 1080", "3840 x 1080", "3840x1080"},
                                       {"2560 x 720", "2560 x 720", "2560x720"},
                                       {"7680 x 2160", "7680 x 2160", "7680x2160"}};
    const std::string output = a.game.Get("aspect_output", a.game.Get("widescreen_output"));
    const char* label = T("Tamaño de pantalla", "Screen size");
    bool changed;
    if (mode == "16x10") {
        changed = GameChoiceCombo(label, "aspect_output", tall, output.empty() ? "1920x1200" : output.c_str(), 16.0f);
        ImGui::TextColored(kGrey, "%s", T("16:10 experimental: pantalla y cámara adaptadas; menús y HUD originales. "
            "Actualización 1.21, USA o Europa. Sin FSR ni DLSS. Requiere reiniciar.",
            "Experimental 16:10 display and camera; original menus and HUD. USA or Europe 1.21. "
            "No FSR or DLSS. Requires a restart."));
    } else if (mode == "21x9") {
        changed = GameChoiceCombo(label, "aspect_output", wide, output.empty() ? "3440x1440" : output.c_str(), 16.0f);
    } else {
        changed = GameChoiceCombo(label, "aspect_output", super, output.empty() ? "5120x1440" : output.c_str(), 16.0f);
    }
    if (changed) {
        a.game.Set("aspect_ratio", mode);
        a.game.Set("widescreen_layout", mode == "21x9" || mode == "32x9" ? mode : "off");
        if (mode == "21x9" || mode == "32x9") a.game.Set("widescreen_output", a.game.Get("aspect_output"));
        SaveGameIni();
    }
    if (mode != "16x10") ImGui::TextColored(kGrey, "%s", T("Menús, fondos y HUD centrado corregidos. En línea o sin conexión con la actualización 1.21, USA o Europa "
        "(si no, el juego arranca en 16:9). Sin FSR ni DLSS; la resolución interna elige la variante.",
        "Corrected menus, backgrounds and a centered HUD. Online or offline, with the 1.21 update, USA or Europe "
        "(otherwise the game starts at 16:9). No FSR or DLSS; the internal resolution picks the variant."));
}

void SettingsTab() {
    App& a = g_app;
    ImGui::TextColored(kGrey, "%s", T("Se aplican al pulsar Jugar. El menú del juego (Insert o L3+R3) cambia los mismos ajustes.",
                                      "Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings."));

    ImGui::SeparatorText(T("Rendimiento", "Performance"));
    {
        // renderer=shadps4|native (bbport.ini; run.py sets the native renderer up and builds its
        // shaders from the player's game the first time)
        static constexpr Choice kRenderer[] = {{"Vulkan (shadPS4)", "Vulkan (shadPS4)", "shadps4"},
                                               {"Vulkan nativo (experimental)", "Native Vulkan (experimental)", "native"}};
        if (GameChoiceCombo(T("Motor gráfico", "Graphics engine"), "renderer", kRenderer, "shadps4", 16.0f)) SaveGameIni();
        HelpMarker(T("Vulkan (shadPS4) es el motor de siempre. El nativo dibuja el juego directamente en Vulkan, sin la "
                     "emulación de la GPU de PS4: más FPS y menos tirones, pero es experimental. La primera vez prepara "
                     "sus shaders a partir de tu copia del juego (unos minutos). Con el nativo no hay FSR 3 ni DLSS "
                     "(FSR 1 sí). Si algo va mal, vuelve a Vulkan (shadPS4).",
                     "Vulkan (shadPS4) is the usual engine. The native one draws the game straight with Vulkan, without "
                     "emulating the PS4's GPU: more FPS and fewer hitches, but it is experimental. The first time it "
                     "builds its shaders from your copy of the game (a few minutes). The native engine has no FSR 3 or "
                     "DLSS (FSR 1 works). If something goes wrong, go back to Vulkan (shadPS4)."));
    }
    {
        int limit = a.game.Int("fps_limit", 60);
        auto name = [](int v) -> std::string {
            if (v == 0) return T("Sin límite", "No limit");
            return std::to_string(v) + " FPS";
        };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo(T("Límite de FPS", "FPS limit"), name(limit).c_str())) {
            for (int v : kFpsLimits) {
                if (ImGui::Selectable(name(v).c_str(), v == limit)) {
                    a.game.Set("fps_limit", std::to_string(v));
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Con el parche de 60 FPS el juego va tan rápido como permita tu PC. En una pantalla de 144 Hz sin "
                     "G-Sync/FreeSync, 72 reparte los fotogramas de forma pareja.",
                     "With the 60 FPS patch the game runs as fast as your PC allows. On a 144 Hz screen without "
                     "G-Sync/FreeSync, 72 divides the refresh evenly."));
    }
    GameFlag("async_pipelines", T("Compilar shaders en segundo plano (experimental)", "Compile shaders in the background (experimental)"), false,
             T("Menos tirones la primera vez que aparece un efecto; a veces un efecto tarda un fotograma en verse.",
               "Fewer hitches when an effect is first seen; an effect may show up a frame late."));

    ImGui::SeparatorText(T("Pantalla", "Display"));
    const std::string temporal = GameTemporal();
    const bool dlss_sized = temporal == "dlss" && a.game.Get("dlss_quality", "auto") != "auto";
    ImGui::BeginDisabled(dlss_sized);
    RenderResCombo();
    ImGui::EndDisabled();
    if (dlss_sized) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "%s", T("(la decide la calidad de DLSS)", "(set by the DLSS quality)"));
    }
    AspectRatioSettings();
    GameFlag("skip_intro", T("Saltar los vídeos de inicio", "Skip intro videos"), false,
             T("Sin los logos de Activision y Beenox al arrancar el juego.",
               "Skips the Activision and Beenox logo videos when the game starts."));
    GameFlag("msaa", T("MSAA del juego (2x)", "Game MSAA (2x)"), false,
             T("El antialiasing 2x de la PS4: bordes más limpios en la vegetación. En NVIDIA ya no dibuja "
               "líneas punteadas en el cielo; en otras gráficas puede que sí. Cuesta algo de "
               "rendimiento en gráficas modestas. Requiere reiniciar.",
               "The PS4's 2x anti-aliasing: cleaner foliage edges. On NVIDIA it no longer draws dotted "
               "lines in the sky; on other GPUs it may. Costs some frames on modest GPUs. "
               "Needs a restart."));
    {
        // temporal=off|fsr1|fsr3|dlss|fsr4 (bbport.ini): one upscaler at a time. FSR 4 only with its
        // model files (Fsr4FilesPresent).
        static constexpr Choice kTemporal[] = {{"Desactivado", "Off", "off"},
                                               {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                               {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"},
                                               {"NVIDIA DLSS (experimental)", "NVIDIA DLSS (experimental)", "dlss"}};
        static constexpr Choice kTemporalFsr4[] = {{"Desactivado", "Off", "off"},
                                                   {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                                   {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"},
                                                   {"NVIDIA DLSS (experimental)", "NVIDIA DLSS (experimental)", "dlss"},
                                                   {"AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)", "fsr4"}};
        static constexpr Choice kTemporalNative[] = {{"Desactivado", "Off", "off"},
                                                     {"AMD FSR 1", "AMD FSR 1", "fsr1"}};
        const bool native = a.game.Get("renderer") == "native";
        const bool fsr4 = Fsr4FilesPresent();
        const bool picked = native ? GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporalNative,
                                                     temporal == "off" ? "off" : "fsr1", 16.0f)
                            : fsr4 ? GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporalFsr4,
                                                     temporal.c_str(), 16.0f)
                                   : GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporal,
                                                     temporal.c_str(), 16.0f);
        if (picked) {
            a.game.Erase("fsr1");
            a.game.Erase("fsr3");
            SaveGameIni();
        }
        if (native && (temporal == "fsr3" || temporal == "dlss" || temporal == "fsr4")) {
            ImGui::TextColored(kGrey, "%s", T("FSR 3, FSR 4 y DLSS no funcionan con el motor nativo: se usa FSR 1.",
                                              "FSR 3, FSR 4 and DLSS do not work with the native engine: FSR 1 is used."));
        } else if (temporal == "fsr4" && !fsr4) {
            ImGui::TextColored(kGrey, "%s", T("Faltan los archivos de FSR 4 (carpeta fsr4_shaders): se usa FSR 3.",
                                              "FSR 4's files are missing (fsr4_shaders folder): FSR 3 is used."));
        }
        HelpMarker(T("FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado simple del juego. "
                     "FSR 3 y DLSS reconstruyen el detalle a lo largo de varios cuadros con la profundidad y el movimiento "
                     "del juego, y a la misma resolución también suavizan los bordes. DLSS necesita una tarjeta NVIDIA "
                     "RTX y FSR 4 una gráfica con soporte INT8 y sus archivos (si no, se usa FSR 3). Experimentales: "
                     "si ves estelas o parpadeos, cambia a FSR 1.",
                     "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3 and DLSS "
                     "rebuild detail over several frames from the game's depth and motion, and smooth edges even at the "
                     "same resolution. DLSS needs an NVIDIA RTX card and FSR 4 a GPU with INT8 support and its files "
                     "(FSR 3 runs otherwise). Experimental: switch to FSR 1 if you see trails or shimmering."));
    }
    if (temporal == "dlss") {
        static constexpr Choice kQuality[] = {
            {"Automática (resolución interna)", "Auto (internal resolution)", "auto"},
            {"DLAA (nativa)", "DLAA (native)", "dlaa"},
            {"Calidad", "Quality", "quality"},
            {"Equilibrado", "Balanced", "balanced"},
            {"Rendimiento", "Performance", "performance"},
            {"Ultra rendimiento", "Ultra Performance", "ultra_performance"}};
        if (GameChoiceCombo(T("Calidad de DLSS", "DLSS quality"), "dlss_quality", kQuality, "auto", 16.0f)) SaveGameIni();
        HelpMarker(T("Automática usa la resolución interna de arriba. Las demás dibujan la escena a la proporción de NVIDIA "
                     "respecto a la salida de DLSS: Calidad 67 %, Equilibrado 58 %, Rendimiento 50 %, Ultra rendimiento "
                     "33 %, DLAA 100 %.",
                     "Auto uses the internal resolution above. The others draw the scene at NVIDIA's share of the DLSS "
                     "output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA 100%."));
        if (a.game.Get("dlss_quality", "auto") != "auto") {
            static constexpr Choice kOutput[] = {{"1920 x 1080", "1920 x 1080", "1080"},
                                                 {"3840 x 2160 (4K)", "3840 x 2160 (4K)", "2160"}};
            if (GameChoiceCombo(T("Salida de DLSS", "DLSS output"), "dlss_output", kOutput, "1080", 16.0f)) SaveGameIni();
        }
        static constexpr Choice kModel[] = {{"Transformer (recomendado)", "Transformer (recommended)", "transformer"},
                                            {"CNN", "CNN", "cnn"}};
        if (GameChoiceCombo(T("Modelo de DLSS", "DLSS model"), "dlss_model", kModel, "transformer", 16.0f)) SaveGameIni();
        HelpMarker(T("Transformer: el modelo actual de NVIDIA (DLSS 4), más nítido y estable, algo más pesado. CNN: el "
                     "modelo anterior (preset E), más ligero; NVIDIA lo marca como obsoleto.",
                     "Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. CNN: the "
                     "older model (preset E), lighter; NVIDIA marks it deprecated."));
    }
    if (temporal == "fsr3" || temporal == "fsr4") {
        float sharp = float(std::atof(a.game.Get("fsr3_sharpness", "0.30").c_str()));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderFloat(T("Nitidez de FSR", "FSR sharpness"), &sharp, 0.0f, 1.0f, "%.2f")) {
            char text[16];
            std::snprintf(text, sizeof text, "%.2f", sharp);
            a.game.Set("fsr3_sharpness", text);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
    }
    {
        static constexpr Choice kFrameGen[] = {
            {"Desactivado", "Off", "off"},
            {"AMD FSR 3 (2x, experimental)", "AMD FSR 3 (2x, experimental)", "fsr3"},
        };
        if (GameChoiceCombo(T("Generación de fotogramas", "Frame generation"), "framegen", kFrameGen, "off", 16.0f))
            SaveGameIni();
        HelpMarker(T("Inserta un fotograma interpolado entre cada dos fotogramas del juego (FidelityFX FSR 3.1), duplicando "
                     "los FPS mostrados en pantalla. Añade un fotograma de latencia; funciona mejor a 60 FPS o más.",
                     "Inserts an interpolated frame between each two game frames (FidelityFX FSR 3.1), doubling the "
                     "displayed FPS. Adds one frame of latency; works best at 60 FPS or above."));
    }
    GameFlag("fullscreen", T("Pantalla completa", "Fullscreen"), false);
    GameFlag("borderless", T("Ventana sin bordes", "Borderless window"), false,
             T("Quita los bordes de la ventana en modo ventana.", "Removes window borders in windowed mode."));
    GameFlag("show_fps", T("Contador de FPS en la esquina", "FPS counter in the corner"), false);
    GameFlag("show_ping", T("Contador de ping en la esquina", "Ping counter in the corner"), false,
             T("En una sala online, el ping de cada jugador en ms, debajo del contador de FPS. También en el menú "
               "del juego (Insert o L3+R3), pestaña Sala.",
               "In an online room, each player's ping in ms, under the FPS counter. Also in the in-game menu "
               "(Insert or L3+R3), Room tab."));
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("display");
        std::string preview = T("Principal", "Primary");
        for (const auto& [value, label] : a.displays)
            if (value == current) preview = label;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo(T("Monitor", "Monitor"), preview.c_str())) {
            if (ImGui::Selectable(T("Principal", "Primary"), current.empty())) {
                a.launcher.Set("display", "");
                SaveLauncher();
            }
            for (const auto& [value, label] : a.displays) {
                if (ImGui::Selectable(label.c_str(), value == current)) {
                    a.launcher.Set("display", value);
                    SaveLauncher();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ChoiceCombo(T("Presentación", "Presentation"), "present_mode", kPresentModes, "Mailbox");

    ImGui::SeparatorText(T("Sonido", "Sound"));
    {
        int volume = std::clamp(a.game.Int("volume", 100), 0, 200);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Volumen del juego", "Game volume"), &volume, 0, 200, "%d%%")) {
            a.game.Set("volume", std::to_string(volume));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
        HelpMarker(T("100% es el nivel al que se mezcló el juego. Por encima, los picos más fuertes se suavizan en vez "
                     "de distorsionar. Mientras juegas también se cambia en el menú del juego (Insert o L3+R3).",
                     "100% is the level the game was mixed at. Above it, the loudest peaks are rounded off instead of "
                     "distorting. While you play it can also be changed in the in-game menu (Insert or L3+R3)."));
    }

    ImGui::SeparatorText(T("Mando", "Controller"));
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("gamepad");
        std::string preview = T("El primero conectado", "First connected");
        bool listed = current.empty();
        for (const auto& [guid, name] : a.gamepads)
            if (guid == current) {
                preview = name;
                listed = true;
            }
        if (!listed) preview = a.launcher.Get("gamepad_name", current) + T(" (no conectado)", " (not connected)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo(T("Mando", "Controller"), preview.c_str())) {
            if (ImGui::Selectable(T("El primero conectado", "First connected"), current.empty())) {
                a.launcher.Set("gamepad", "");
                a.launcher.Set("gamepad_name", "");
                SaveLauncher();
            }
            for (const auto& [guid, name] : a.gamepads) {
                ImGui::PushID(guid.c_str());
                if (ImGui::Selectable(name.c_str(), guid == current)) {
                    a.launcher.Set("gamepad", guid);
                    a.launcher.Set("gamepad_name", name);
                    SaveLauncher();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!a.probe_done && a.probe.joinable());
    if (ImGui::Button(T("Buscar mandos", "Refresh"))) {
        if (a.probe.joinable()) a.probe.join();
        StartProbe();
    }
    ImGui::EndDisabled();
    {
        // bbport.ini port.N=keyboard (N 2..4): that split-screen player plays on the keyboard and
        // player 1 on the controller alone (src/runtime_pad.c). None: the keyboard drives player 1 too.
        const auto port_key = [](int n) { return "port." + std::to_string(n); };
        int current = 1;
        for (int n = 2; n <= 4; ++n)
            if (Lower(Trim(a.game.Get(port_key(n)))) == "keyboard") current = n;
        const auto name = [](int n) {
            return n == 1 ? std::string(T("Jugador 1 (junto al mando)", "Player 1 (with the controller)"))
                          : std::string(T("Jugador ", "Player ")) + std::to_string(n);
        };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::BeginCombo(T("Teclado", "Keyboard"), name(current).c_str())) {
            for (int n = 1; n <= 4; ++n) {
                if (!ImGui::Selectable(name(n).c_str(), n == current) || n == current) continue;
                for (int k = 2; k <= 4; ++k)
                    if (Lower(Trim(a.game.Get(port_key(k)))) == "keyboard") a.game.Set(port_key(k), "");
                if (n > 1) a.game.Set(port_key(n), "keyboard");
                SaveGameIni();
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Para jugar dos en pantalla dividida, uno con teclado y otro con mando, elige Jugador 2: el mando "
                     "queda para el jugador 1 y el teclado para el 2 (Espacio es X). Si juegas solo con teclado, déjalo "
                     "en Jugador 1. También se cambia en el menú del juego (Insert o L3+R3).",
                     "For two players in split screen, one on the keyboard and one on a controller, choose Player 2: "
                     "the controller stays player 1 and the keyboard plays player 2 (Space is Cross). If you play on "
                     "the keyboard alone, leave it on Player 1. The in-game menu (Insert or L3+R3) changes it too."));
    }
    // bbport.ini button_prompts (scripts/button_prompts.py): the buttons the game shows.
    {
        static constexpr Choice kButtonPrompts[] = {
            {"PlayStation", "PlayStation", "playstation"},
            {"Xbox", "Xbox", "xbox"},
        };
        if (GameChoiceCombo(T("Iconos de botones", "Button prompts"), "button_prompts", kButtonPrompts, "playstation", 12.0f))
            SaveGameIni();
        HelpMarker(T("Los botones que muestra el juego en menús y avisos. Xbox: A, B, X, Y, LB, RB, LT, RT y View. "
                     "Se aplica al pulsar Jugar.",
                     "The buttons the game shows in menus and prompts. Xbox: A, B, X, Y, LB, RB, LT, RT and View. "
                     "Applies when you press Play."));
    }
    GameFlag("split_auto", T("Pantalla dividida: asignar los mandos automáticamente", "Split screen: assign controllers automatically"),
             true,
             T("Para jugar de 2 a 4 con varios mandos: cada mando que conectes es el siguiente jugador (el primero, el "
               "jugador 1). Desactivado, una persona puede cambiar de mando cuando quiera, y los mandos de los jugadores "
               "2-4 se eligen en el menú del juego (Insert o L3+R3), Controllers.",
               "For 2 to 4 players with several controllers: each controller you connect is the next player (the first "
               "one, player 1). Off, one person can switch controllers at will, and players 2-4's controllers are chosen "
               "in the in-game menu (Insert or L3+R3), Controllers."));
    ImGui::TextColored(kGrey, "%s", T("Más mandos para la pantalla dividida: menú del juego (Insert o L3+R3), Controllers.",
                                      "More controllers for split screen: in-game menu (Insert or L3+R3), Controllers."));

    ImGui::SeparatorText(T("Consola", "Console"));
    if (a.launcher.Get("language") != GameLanguage()) a.launcher.Set("language", GameLanguage());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (IsEuGame()) {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguagesEU, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae la versión europea (CUSA14876): inglés, español de España, francés, alemán, "
                     "italiano y árabe. La lista cambia sola según la versión del juego que elijas. Se aplica al "
                     "pulsar Jugar.",
                     "The languages of the European edition (CUSA14876): English, Spain's Spanish, French, German, "
                     "Italian and Arabic. The list follows the edition of the game you choose. Applied when you "
                     "press Play."));
    } else {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguages, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae la versión de América (CUSA13795): inglés, español latinoamericano, francés "
                     "y portugués de Brasil. La lista cambia sola según la versión del juego que elijas (la europea "
                     "trae español de España, alemán, italiano y árabe). Se aplica al pulsar Jugar.",
                     "The languages of the Americas edition (CUSA13795): English, Latin American Spanish, French and "
                     "Brazilian Portuguese. The list follows the edition of the game you choose (the European one has "
                     "Spain's Spanish, German, Italian and Arabic). Applied when you press Play."));
    }

    ImGui::SeparatorText(T("Ventana y Launcher", "Window and Launcher"));
    LauncherFlag("hide_on_play", T("Ocultar ventana del launcher mientras se juega", "Hide launcher window while playing"), false,
                 T("Oculta completamente la ventana del launcher mientras juegas y la restaura automáticamente al salir.",
                   "Hides the launcher window completely while playing and restores it automatically when exiting."));
    LauncherFlag("close_on_play", T("Cerrar launcher tras jugar", "Close launcher after playing"), false,
                 T("Cierra el launcher por completo una vez que sales del juego.",
                   "Closes the launcher completely once you exit the game."));
}

/// Linking the nickname to the player's Discord account, so that a reinstall or another PC
/// recovers it. The browser half is discord_link.cpp; the server trades the code for the
/// account (ryunet-demonware-ctr/server/discord_link.go).
struct DiscordLinkState {
    std::mutex mutex;
    bool busy = false;
    bool error = false;
    std::string message;     // the last outcome, under the buttons
    std::string other_nick;  // "other_nick": the nickname this Discord account already has
    std::string recovered;   // a recovered nickname for the UI thread to put in the field
};
DiscordLinkState g_discord_link;

/// Link the current nickname, unlink it, or recover the Discord account's own nickname (for a
/// reinstalled launcher that does not know it any more).
enum class DiscordLinkMode { Link, Unlink, Recover };

void StartDiscordLink(DiscordLinkMode mode) {
    const bool unlink = mode == DiscordLinkMode::Unlink;
    App& a = g_app;
    const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    const std::string nick = mode == DiscordLinkMode::Recover ? std::string() : CurrentNick();
    const std::string app_id = DiscordAppId();
    {
        std::scoped_lock lock{g_discord_link.mutex};
        if (g_discord_link.busy) return;
        g_discord_link.busy = true;
        g_discord_link.error = false;
        g_discord_link.message.clear();
        g_discord_link.other_nick.clear();
    }
    std::thread([server, nick, app_id, unlink] {
        std::string message, other, recovered;
        bool error = true;
        const auto form = [](const char* key, const std::string& value) {
            return std::string(key) + "=" + DiscordLink::FormEncode(value);
        };
        if (unlink) {
            const std::string body = HttpRequest(server, L"POST", L"/ryunet/discord/unlink", form("nick", nick), 10000);
            if (JsonBool(body, "unlinked")) {
                error = false;
                message = T("Apodo desvinculado de Discord.", "Nickname unlinked from Discord.");
            } else {
                message = T("No se pudo desvincular.", "Could not unlink.");
            }
        } else {
            const DiscordLink::Result auth = DiscordLink::Authorize(app_id, 180, g_spanish);
            if (!auth.ok) {
                message = auth.error == "denied"  ? T("Cancelaste la autorización en Discord.", "You cancelled the authorization in Discord.")
                          : auth.error == "busy"  ? T("Los puertos 53134 a 53136 están ocupados: cierra otros launchers abiertos.",
                                                      "Ports 53134 to 53136 are in use: close other open launchers.")
                          : auth.error == "timeout" ? T("La autorización de Discord no llegó a tiempo (3 minutos).",
                                                        "Discord's authorization did not arrive in time (3 minutes).")
                                                    : T("No se pudo autorizar con Discord.", "Could not authorize with Discord.");
            } else {
                const std::string body =
                    HttpRequest(server, L"POST", L"/ryunet/discord/link",
                                form("nick", nick) + "&" + form("code", auth.code) + "&" + form("redirect_uri", auth.redirect_uri), 25000,
                                true);
                const std::string status = JsonField(body, "status"), account = JsonField(body, "discord");
                if (status == "linked") {
                    error = false;
                    message = Format(T("Listo: %s queda vinculado a tu cuenta de Discord %s. Si reinstalas o cambias de PC, "
                                       "pulsa Recuperar con Discord.",
                                       "Done: %s is linked to your Discord account %s. If you reinstall or change PC, "
                                       "press Recover with Discord."),
                                     nick, account);
                } else if (status == "recovered") {
                    error = false;
                    recovered = nick.empty() ? JsonField(body, "nick") : nick;
                    message = Format(T("Recuperado: %s vuelve a ser tuyo en esta instalación (Discord: %s).",
                                       "Recovered: %s is yours again on this installation (Discord: %s)."),
                                     recovered, account);
                } else if (status == "own_nick") {
                    error = false;
                    recovered = JsonField(body, "nick");
                    message = Format(T("Esta instalación ya tenía el apodo %s, sin vincular a Discord. Ya está puesto: pulsa "
                                       "Vincular con Discord para poder recuperarlo en cualquier PC.",
                                       "This installation already had the nickname %s, not linked to Discord. It is filled "
                                       "in: press Link with Discord to recover it on any PC."),
                                     recovered);
                } else if (status == "no_nick") {
                    message = Format(T("Tu cuenta de Discord %s no tiene ningún apodo vinculado en este servidor: escribe "
                                       "uno y pulsa Vincular con Discord.",
                                       "Your Discord account %s has no nickname linked on this server: type one and "
                                       "press Link with Discord."),
                                     account);
                } else if (status == "other_nick") {
                    other = JsonField(body, "nick");
                    message = Format(T("Tu cuenta de Discord ya tiene el apodo %s: cada cuenta tiene uno.",
                                       "Your Discord account already has the nickname %s: one per account."),
                                     other);
                } else if (status == "taken") {
                    message = T("Ese apodo es de otra persona o de otra cuenta de Discord.",
                                "That nickname belongs to someone else or to another Discord account.");
                } else if (status == "invalid" || status == "reserved") {
                    message = T("El servidor no aceptó la solicitud.", "The server did not accept the request.");
                } else if (body.empty()) {
                    message = T("No se pudo contactar con el servidor.", "Could not reach the server.");
                } else {
                    message = T("El servidor no pudo confirmar la cuenta con Discord. Prueba otra vez.",
                                "The server could not confirm the account with Discord. Try again.");
                }
            }
        }
        {
            std::scoped_lock lock{g_discord_link.mutex};
            g_discord_link.busy = false;
            g_discord_link.error = error;
            g_discord_link.message = message;
            g_discord_link.other_nick = other;
            g_discord_link.recovered = recovered;
        }
        std::scoped_lock lock{g_nick_check.mutex};  // ask the server for the new state
        g_nick_check.results.clear();
    }).detach();
}

/// The running authorization or the last outcome, with "Use <nickname>" when Discord named one.
void DiscordLinkStatusLines() {
    App& a = g_app;
    bool busy, error;
    std::string message, other;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
        error = g_discord_link.error;
        message = g_discord_link.message;
        other = g_discord_link.other_nick;
    }
    if (busy) {
        ImGui::TextColored(kGrey, "%s", T("Autoriza tu cuenta en la pestaña de Discord que se abrió en el navegador...",
                                          "Authorize your account in the Discord tab that opened in the browser..."));
        return;
    }
    if (message.empty()) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(error ? kYellow : kGreen, "%s", message.c_str());
    ImGui::PopTextWrapPos();
    if (!other.empty() && ImGui::SmallButton(Format(T("Usar %s", "Use %s"), other).c_str())) {
        std::snprintf(a.online_name_buf, sizeof a.online_name_buf, "%s", other.c_str());
        a.game.Set("online_name", a.online_name_buf);
        a.game.Set("user_name", a.online_name_buf);
        SaveGameIni();
        std::scoped_lock lock{g_discord_link.mutex};
        g_discord_link.message.clear();
        g_discord_link.other_nick.clear();
    }
}

/// With no usable nickname yet (a reinstall, another PC): recover the one linked to Discord.
void DiscordRecoverOffer() {
    if (!DiscordLinkAvailable()) return;
    bool busy;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(T("Recuperar mi apodo con Discord", "Recover my nickname with Discord")))
        StartDiscordLink(DiscordLinkMode::Recover);
    ImGui::EndDisabled();
    HelpMarker(T("¿Reinstalaste el juego o cambiaste de PC y tu apodo estaba vinculado a Discord? Autoriza la misma "
                 "cuenta de Discord y el launcher pondrá tu apodo, aunque no lo recuerdes.",
                 "Did you reinstall the game or change PC, and your nickname was linked to Discord? Authorize the "
                 "same Discord account and the launcher fills in your nickname, even if you do not remember it."));
    DiscordLinkStatusLines();
}

/// The recovered nickname, put in the field by the UI thread (PollJobs).
void ApplyRecoveredNick() {
    App& a = g_app;
    std::string nick;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        nick.swap(g_discord_link.recovered);
    }
    if (nick.empty() || NickProblem(nick)) return;
    std::snprintf(a.online_name_buf, sizeof a.online_name_buf, "%s", nick.c_str());
    a.game.Set("online_name", a.online_name_buf);
    a.game.Set("user_name", a.online_name_buf);
    SaveGameIni();
}

/// Under the nickname: its Discord account, and Link / Recover / Unlink.
void DiscordAccountSection() {
    if (NickProblem(CurrentNick())) {
        DiscordRecoverOffer();
        return;
    }
    const NickCheck::Result info = NickInfo();
    bool busy;
    std::string message;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
        message = g_discord_link.message;
    }
    if (!info.link_available && message.empty()) return;  // unknown yet, or this server cannot link
    ImGui::BeginDisabled(busy);
    if (info.status == "yours" && info.linked) {
        ImGui::TextColored(kGreen, "%s", Format(T("Cuenta de Discord: %s", "Discord account: %s"), info.discord).c_str());
        ImGui::SameLine();
        // Unlinking frees the nickname field and stops Discord recovery: a second press confirms.
        static ULONGLONG unlink_armed = 0;
        const bool armed = unlink_armed && GetTickCount64() - unlink_armed < 5000;
        if (ImGui::SmallButton(armed ? T("Pulsa otra vez para desvincular", "Press again to unlink") : T("Desvincular", "Unlink"))) {
            if (armed) {
                unlink_armed = 0;
                StartDiscordLink(DiscordLinkMode::Unlink);
            } else {
                unlink_armed = GetTickCount64();
            }
        }
        if (!armed && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Deja de estar vinculado: podrás cambiar el apodo, pero ya no lo recuperarás con Discord "
                                      "si reinstalas.",
                                      "Unlinks it: you can change the nickname, but can no longer recover it with Discord "
                                      "if you reinstall."));
    } else if (info.status == "yours" || info.status == "free") {
        if (ImGui::Button(T("Vincular con Discord", "Link with Discord"))) StartDiscordLink(DiscordLinkMode::Link);
        if (info.status == "free") {
            // A reinstall shows a free nickname the player just typed; theirs may be linked.
            ImGui::SameLine();
            if (ImGui::SmallButton(T("¿Ya tenías un apodo vinculado? Recupéralo", "Had a linked nickname already? Recover it")))
                StartDiscordLink(DiscordLinkMode::Recover);
        }
        HelpMarker(T("Tu apodo queda ligado a tu cuenta de Discord: si reinstalas el juego o juegas en otro PC, lo "
                     "recuperas autorizando Discord otra vez. Se abre Discord en el navegador para que lo autorices; "
                     "el launcher solo recibe tu identificador de Discord, nunca tu contraseña.",
                     "Your nickname is tied to your Discord account: if you reinstall the game or play on another PC, "
                     "you recover it by authorizing Discord again. Discord opens in the browser for you to authorize; "
                     "the launcher only receives your Discord ID, never your password."));
    } else if (info.status == "taken" && info.linked) {
        if (ImGui::Button(T("Recuperar con Discord", "Recover with Discord"))) StartDiscordLink(DiscordLinkMode::Link);
        HelpMarker(T("Si este apodo es tuyo, autoriza la misma cuenta de Discord con la que lo vinculaste y "
                     "podrás usarlo en esta instalación.",
                     "If this nickname is yours, authorize the same Discord account you linked it with and you can "
                     "use it on this installation."));
    } else if (info.status == "taken") {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kGrey, "%s",
                           T("Si es tuyo y lo perdiste al reinstalar, pide al administrador del servidor que lo libere, "
                             "o espera: un apodo sin vincular y sin uso se libera a los 60 días.",
                             "If it is yours and you lost it when reinstalling, ask the server's administrator to "
                             "release it, or wait: an unlinked nickname nobody uses is released after 60 days."));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndDisabled();
    DiscordLinkStatusLines();
}

void NatTypeSection(const std::string& server) {
    bool running, done;
    NatCheck::Result result;
    {
        std::scoped_lock lock{g_nat.mutex};
        running = g_nat.running;
        done = g_nat.done;
        result = g_nat.result;
    }
    ImGui::BeginDisabled(running);
    if (ImGui::Button(T("Comprobar tipo de NAT", "Check NAT type"))) StartNatCheck(server);
    ImGui::EndDisabled();
    HelpMarker(T("Comprueba si otros jugadores pueden conectarse directamente a tu PC. Pregunta a tu servidor RyuNET "
                 "y a un servidor STUN público (Cloudflare) con qué dirección te ven y las compara aquí: tu IP no se "
                 "muestra ni se guarda.",
                 "Checks whether other players can connect to your PC directly. It asks your RyuNET server and a "
                 "public STUN server (Cloudflare) which address they see you from and compares them here: your IP "
                 "is neither shown nor saved."));
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(kGrey, "%s", T("Comprobando...", "Checking..."));
        return;
    }
    if (!done) return;
    switch (result.type) {
    case NatCheck::Type::Open:
        ImGui::TextColored(kGreen, "%s", T("NAT tipo 1 (abierto): sin router de por medio.",
                                           "NAT type 1 (open): no router in between."));
        break;
    case NatCheck::Type::Moderate:
        ImGui::TextColored(kGreen, "%s", T("NAT tipo 2 (moderado): los demás jugadores pueden conectarse contigo.",
                                           "NAT type 2 (moderate): other players can connect to you."));
        break;
    case NatCheck::Type::Strict:
        ImGui::TextColored(kYellow, "%s", T("NAT tipo 3 (estricto): tu router cambia de puerto según con quién hables.",
                                            "NAT type 3 (strict): your router changes ports for each destination."));
        ImGui::TextWrapped("%s", T("Con otros jugadores de NAT estricto la conexión directa suele fallar y el juego pasa "
                                   "por el túnel del servidor. Ayuda activar UPnP en el router o abrir el puerto UDP 3074 "
                                   "hacia este PC.",
                                   "Direct connections with other strict-NAT players often fail and the game goes "
                                   "through the server's tunnel. Turning on UPnP in the router or forwarding UDP port "
                                   "3074 to this PC helps."));
        break;
    case NatCheck::Type::Unknown:
        ImGui::TextColored(result.error == "server" ? kRed : kGrey, "%s",
                           result.error == "server" ? T("El servidor no respondió (¿está bien escrito? ¿UDP 3074 bloqueado?).",
                                                        "The server did not answer (is it spelled right? UDP 3074 blocked?).")
                           : result.error == "dns" ? T("No se encontró el servidor.", "The server was not found.")
                           : result.error == "stun" ? T("No se pudo comparar (el servidor STUN no respondió).",
                                                        "Could not compare (the STUN server did not answer).")
                                                    : T("No se pudo comprobar.", "Could not check."));
        break;
    }
    if (result.replies_blocked || result.error == "server") {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("%s", T("Tu red bloquea las respuestas del servidor (UDP 3074): el juego dirá \"Failed to initialize "
                                   "network\" al entrar en Online. Permite sacredfire.exe en el Firewall de Windows (redes "
                                   "privadas y públicas), revisa la protección de red del antivirus, reinicia el router o "
                                   "prueba otra red (los datos del móvil).",
                                   "Your network blocks the server's replies (UDP 3074): the game will say \"Failed to "
                                   "initialize network\" when you enter Online. Allow sacredfire.exe in Windows Firewall "
                                   "(private and public networks), check your antivirus' network protection, restart your "
                                   "router or try another network (a phone hotspot)."));
        ImGui::PopStyleColor();
    }
}

void OnlineTab() {
    App& a = g_app;
    ImGui::TextColored(kGrey, "%s", T("Juego online multijugador. Se aplica al pulsar Jugar.",
                                      "Online multiplayer. Applied when you press Play."));

    ImGui::SeparatorText(T("Servidor", "Server"));
    {
        GameFlag("online", T("Habilitar modo online", "Enable online mode"), true,
                 T("Conecta al servidor online para jugar partidas públicas, salas privadas, Pit Stop y marcadores.",
                   "Connects to the online server for public matchmaking, private rooms, Pit Stop, and leaderboards."));

        const bool online_on = a.game.Flag("online", true);
        ImGui::BeginDisabled(!online_on);

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        if (ImGui::InputText(T("Servidor Online (Server)", "Online Server"), a.online_server_buf, sizeof(a.online_server_buf))) {
            a.game.Set("online_server", a.online_server_buf);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (Trim(a.online_server_buf).empty()) {
                std::strncpy(a.online_server_buf, "octr.ryu7w7.xyz", sizeof(a.online_server_buf) - 1);
                a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
                a.game.Set("online_server", a.online_server_buf);
            }
            SaveGameIni();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Por defecto", "Default"))) {
            std::strncpy(a.online_server_buf, "octr.ryu7w7.xyz", sizeof(a.online_server_buf) - 1);
            a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
            a.game.Set("online_server", a.online_server_buf);
            SaveGameIni();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("127.0.0.1")) {
            std::strncpy(a.online_server_buf, "127.0.0.1", sizeof(a.online_server_buf) - 1);
            a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
            a.game.Set("online_server", a.online_server_buf);
            SaveGameIni();
        }
        HelpMarker(T("IP o dominio del servidor online (predeterminado: octr.ryu7w7.xyz, o 127.0.0.1 para servidor local). Puerto HTTP opcional :8080.",
                     "IP or domain of the online server (default: octr.ryu7w7.xyz, or 127.0.0.1 for local server). Optional HTTP port :8080."));
        NatTypeSection(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")));
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText(T("Jugador", "Player"));
    {
        ImGui::BeginDisabled(!a.game.Flag("online", true));

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        // A nickname linked to this player's Discord account is their identity: it is changed by
        // unlinking it first (DiscordAccountSection), not by typing over it.
        const NickCheck::Result nick_info = NickInfo();
        const bool nick_locked = nick_info.status == "yours" && nick_info.linked;
        ImGui::BeginDisabled(nick_locked);
        NickInput(T("Apodo (Nickname)", "Nickname"));
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
        ImGui::EndDisabled();
        HelpMarker(nick_locked
                       ? T("Este apodo está vinculado a tu cuenta de Discord y queda fijo. Para cambiarlo, pulsa "
                           "Desvincular abajo.",
                           "This nickname is linked to your Discord account and stays fixed. To change it, press "
                           "Unlink below.")
                       : T("Tu nombre en el online, obligatorio: de 3 a 16 letras, números, _ o -. Cada apodo es una cuenta "
                           "distinta en el servidor (monedas, objetos del Pit Stop), así que no uses el de otra persona.",
                           "Your online name, required: 3 to 16 letters, digits, _ or -. Each nickname is a separate account "
                           "on the server (coins, Pit Stop items), so do not use someone else's."));
        if (const char* problem = NickProblem(CurrentNick())) {
            ImGui::TextColored(kYellow, "%s", problem);
            if (a.game.Flag("online", true)) DiscordRecoverOffer();
        } else if (a.game.Flag("online", true)) {
            NickAvailabilityLine();
            DiscordAccountSection();
        }

        // Friends: the in-game menu's Friends tab (Insert / L3+R3) finds, adds and invites them;
        // the names of the old Friends field (online_friends) get friend requests once.
        ImGui::TextColored(kGrey, "%s", T("Amigos: dentro del juego, menú (Insert / L3+R3) > Amigos.",
                                          "Friends: in the game, menu (Insert / L3+R3) > Friends."));

        // Which Pit Stop items (characters, skins, karts) count as yours online: the server's,
        // bought with coins and tied to the nickname, or the save's (run.py sends it at login).
        ImGui::Spacing();
        ImGui::TextUnformatted(T("Personajes, skins y karts en online:", "Characters, skins and karts online:"));
        const bool from_save = Lower(Trim(a.game.Get("online_inventory", "server"))) == "save";
        if (ImGui::RadioButton(T("Progresión en el servidor", "Progress on the server"), !from_save)) {
            a.game.Set("online_inventory", "server");
            SaveGameIni();
        }
        HelpMarker(T("Lo consigues jugando: ganas monedas en las carreras y compras en el Pit Stop. Lo que compras "
                     "queda guardado en el servidor, ligado a tu apodo.",
                     "You earn it by playing: races give coins and you buy in the Pit Stop. What you buy is kept on "
                     "the server, tied to your nickname."));
        if (ImGui::RadioButton(T("Progresión de guardado local", "Local save progress"), from_save)) {
            a.game.Set("online_inventory", "save");
            SaveGameIni();
        }
        HelpMarker(T("Online tienes lo mismo que en tu partida guardada, por ejemplo una partida al 100% importada en "
                     "la pestaña Guardados. El Pit Stop no vende nada en este modo: no gastas monedas en objetos que no "
                     "verías.",
                     "Online you have what your save has, for example a 100% save imported in the Saves tab. The Pit "
                     "Stop sells nothing in this mode: no coins are spent on items you would not see."));

        ImGui::EndDisabled();
    }

    // The rules of the online, and where to report and appeal.
    ImGui::SeparatorText(T("Reglas del online", "Online rules"));
    for (const char* rule : {T("Nada de trampas: hacks, trainers, modificaciones de velocidad u objetos, ni tiempos falsos en los récords. Arruinan la partida de todos.", "No cheating: hacks, trainers, speed or item modifications, or fake leaderboard times. They ruin the game for everyone."), T("Respeta a los demás: sin apodos ofensivos, insultos ni acoso.", "Respect others: no offensive nicknames, insults or harassment."), T("No sabotees partidas ni abandones a propósito para molestar.", "Do not sabotage races or quit on purpose to annoy others."), T("Usa tu propio apodo: no te hagas pasar por otra persona.", "Use your own nickname: do not impersonate anyone."), T("¿Ves a alguien haciendo trampas? Repórtalo en el juego: menú (Insert / L3+R3) > Amigos > Reportar jugador.", "See someone cheating? Report them in the game: menu (Insert / L3+R3) > Friends > Report a player.")}) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", rule);
    }
    ImGui::TextColored(kYellow, "%s", T("Romper las reglas puede costarte tus récords y un ban del online, temporal o permanente, también por IP.", "Breaking the rules can cost you your records and a ban from online play, temporary or permanent, also by IP."));

    ImGui::SeparatorText("Discord");
    LauncherFlag("discord_rpc", T("Mostrar actividad en Discord", "Show activity on Discord"), true,
                 T("Mientras juegas, tu estado de Discord muestra CTR Nitro-Fueled, si juegas online o sin conexión, "
                   "tu apodo y el tiempo de juego. Necesita la aplicación de Discord abierta en este PC.",
                   "While you play, your Discord status shows CTR Nitro-Fueled, whether you play online or offline, "
                   "your nickname and the time played. Needs the Discord app running on this PC."));
    if (a.launcher.Flag("discord_rpc", true)) {
        const char* status = "";
        ImVec4 color = kGrey;
        switch (DiscordPresence::GetStatus()) {
        case DiscordPresence::Status::Connected:
            status = T("Conectado a Discord.", "Connected to Discord.");
            color = ImVec4(0.45f, 0.8f, 0.45f, 1.0f);
            break;
        case DiscordPresence::Status::NoDiscord:
            status = T("Discord no está abierto; se conectará solo cuando lo abras.",
                       "Discord is not running; it connects on its own once you open it.");
            break;
        case DiscordPresence::Status::NoAppId:
            status = T("Falta el ID de la aplicación de Discord (discord_app_id en launcher.ini).",
                       "The Discord application ID is missing (discord_app_id in launcher.ini).");
            color = kYellow;
            break;
        case DiscordPresence::Status::Error:
            status = T("Discord rechazó la conexión:", "Discord refused the connection:");
            color = kYellow;
            break;
        case DiscordPresence::Status::Off:
            status = T("Conectando...", "Connecting...");
            break;
        }
        ImGui::TextColored(color, "%s", status);
        if (DiscordPresence::GetStatus() == DiscordPresence::Status::Error) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", DiscordPresence::LastError().c_str());
        }

        LauncherFlag("discord_accept_requests", T("Aceptar a quien pida unirse", "Accept people who ask to join"), true,
                     T("Cuando alguien pulsa \"Pedir unirse\" en tu perfil, se le acepta solo: recibe lo mismo que con "
                       "una invitación tuya (tu apodo y tu servidor).",
                       "When someone presses \"Ask to Join\" on your profile, they are accepted automatically: they "
                       "get the same as from an invite of yours (your nickname and your server)."));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kGrey, "%s",
                           T("Invitar a una privada o a tu grupo: mientras juegas online, en un chat de Discord pulsa "
                             "+ y elige \"Invitar a jugar\". Quien acepte te tendrá en sus amigos y se le abrirá el "
                             "juego; dentro, abre Amigos y únete a ti. Hace falta este launcher y Discord en los dos PC.",
                             "Invite to a private match or your party: while you play online, press + in a Discord "
                             "chat and choose \"Invite to play\". Whoever accepts gets you as a friend and their game "
                             "starts; in it, they open Friends and join you. Both PCs need this launcher and Discord."));
        ImGui::PopTextWrapPos();
    }
}

std::string PackagePatchId(const PatchInfo& patch) {
    const std::string prefix = "OCTR-Mod-Library.xml/Mod: ";
    return patch.key.rfind(prefix, 0) == 0 ? patch.key.substr(prefix.size()) : std::string();
}

bool ModEnableToggle(bool& on) {
    const ImVec2 size{46.0f, 24.0f};
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    const bool changed = ImGui::Button("##mod_enabled", size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (changed) on = !on;
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::GetColorU32(on ? ImVec4(0.19f, 0.77f, 0.97f, 1.0f)
                                           : ImVec4(0.23f, 0.31f, 0.46f, 1.0f));
    draw->AddRectFilled(p, {p.x + size.x, p.y + size.y}, track, size.y * 0.5f);
    draw->AddCircleFilled({p.x + (on ? size.x - 12.0f : 12.0f), p.y + 12.0f}, 9.0f,
                          ImGui::GetColorU32(on ? ImVec4(0.04f, 0.13f, 0.27f, 1) : ImVec4(0.75f, 0.82f, 0.94f, 1)));
    if (ImGui::IsItemFocused()) draw->AddRect({p.x - 2, p.y - 2}, {p.x + size.x + 2, p.y + size.y + 2},
                                             ImGui::GetColorU32(ImGuiCol_Text), 14.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", on ? T("Desactivar mod", "Disable mod")
                                                                       : T("Activar mod", "Enable mod"));
    return changed;
}

void ImportMods(const std::vector<fs::path>& inputs) {
    App& a = g_app;
    size_t added = 0;
    for (const auto& source : inputs) {
        std::error_code ec;
        std::wstring extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (!fs::is_directory(source) && extension != L".pak" && extension != L".zip" && extension != L".xml") {
            a.notice = T("Añade archivos .pak, .zip, .xml o una carpeta.", "Add .pak, .zip, .xml files or a folder.");
            continue;
        }
        const fs::path destination = a.root / L"mods" / source.filename();
        fs::create_directories(a.root / L"mods", ec);
        if (fs::is_directory(source)) {
            // The mods folder itself, or one holding it (the port's), would be copied into
            // itself without end (issue #70); one already inside it is in the library.
            const auto inside = [](const fs::path& path, const fs::path& folder) {
                const fs::path relative = path.lexically_relative(folder);
                return !relative.empty() && *relative.begin() != L"..";
            };
            std::error_code e1, e2;
            const fs::path real = fs::weakly_canonical(source, e1), mods = fs::weakly_canonical(a.root / L"mods", e2);
            if (!e1 && !e2 && inside(mods, real)) {
                a.notice = T("Esa es la carpeta de mods (o la contiene): añade la carpeta del propio mod.",
                             "That is the mods folder (or holds it): add the mod's own folder.");
                continue;
            }
            if (!e1 && !e2 && inside(real, mods)) {
                a.notice = T("Esa carpeta ya está dentro de la carpeta de mods.", "That folder is already in the mods folder.");
                continue;
            }
        }
        if (fs::exists(destination)) {
            a.notice = T("Ya existe un mod con ese nombre. No se ha sobrescrito.",
                         "A mod with that filename already exists. It was not overwritten.");
            continue;
        }
        if (fs::is_directory(source)) fs::copy(source, destination, fs::copy_options::recursive, ec);
        else fs::copy_file(source, destination, fs::copy_options::none, ec);
        if (ec) {
            a.notice = T("No se pudo importar el mod. Revisa el registro.", "Could not import the mod. Check the log.");
            AddLog("Mod import: " + ec.message());
        } else ++added;
    }
    if (added) {
        a.patches = LoadPatches();
        ReadPatchConfig(a.patch_enabled, a.patch_disabled);
        a.notice = T("Mods añadidos. Revisa la lista y actívalos para jugar.",
                     "Mods added. Review the list and enable them to play.");
    }
}


bool ModIsOn(const PatchInfo& p) {
    const App& a = g_app;
    if (p.ini_key) return a.game.Flag(p.ini_key, false);
    const bool enabled = std::find(a.patch_enabled.begin(), a.patch_enabled.end(), p.key) != a.patch_enabled.end();
    const bool disabled = std::find(a.patch_disabled.begin(), a.patch_disabled.end(), p.key) != a.patch_disabled.end();
    return enabled || (p.default_on && !disabled);
}

bool VisibleMod(const PatchInfo& p) {
    if (!p.title.empty() && p.title != Narrow(GameTitle())) return false;
    if (p.key.find("Online: custom Demonware key") != std::string::npos) return false;
    return !p.ini_key || std::strcmp(p.ini_key, "patch_pro_1080p") != 0;
}

void SetModOn(const PatchInfo& p, bool on) {
    App& a = g_app;
    if (p.ini_key) { a.game.Set(p.ini_key, on ? "1" : "0"); SaveGameIni(); return; }
    auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), p.key), v.end()); };
    drop(a.patch_enabled); drop(a.patch_disabled);
    if (on) a.patch_enabled.push_back(p.key);
    else a.patch_disabled.push_back(p.key);
    WritePatchConfig(a.patch_enabled, a.patch_disabled);
}

void ModHeading(const char* title) {
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.12f);
    ImGui::TextUnformatted(title);
    if (g_font_heading) ImGui::PopFont();
}

void ModSection(const char* title, bool builtin) {
    int total = 0, on = 0;
    for (const auto& p : g_app.patches) if (VisibleMod(p) && (p.ini_key != nullptr) == builtin) {
        ++total; if (ModIsOn(p)) ++on;
    }
    ModHeading(title); ImGui::SameLine(0, 12);
    ImGui::TextColored(ImVec4(.65f,.72f,.85f,1), T("%d de %d activos", "%d of %d on"),on,total);
}

bool ModCard(const PatchInfo& p) {
    const bool package = !PackagePatchId(p).empty();
    bool on = ModIsOn(p), remove = false;
    ImGui::PushID(p.key.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, on ? ImVec4(.075f,.19f,.38f,1) : ImVec4(.065f,.14f,.29f,1));
    ImGui::PushStyleColor(ImGuiCol_Border, on ? ImVec4(.12f,.36f,.57f,1) : ImVec4(.12f,.21f,.36f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 11);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(17,10));
    ImGui::BeginChild("card", ImVec2(0,0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginDisabled(g_app.game_running || (package && !g_app.python));
    if (ModEnableToggle(on)) SetModOn(p,on);
    ImGui::EndDisabled();
    ImGui::SameLine(0,18);
    const float text_x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float right = ImGui::GetWindowContentRegionMax().x;
    static std::set<std::string> expanded;
    const bool details = expanded.count(p.key) != 0;
    const float action_width = package ? 76.0f : 32.0f;
    // A separate text child wraps long names/metadata without colliding with actions.
    ImGui::BeginChild("summary", ImVec2(std::max(80.0f,right-text_x-action_width-12),0), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_font_heading) ImGui::PushFont(g_font_heading, 0);
    ImGui::TextWrapped("%s", package ? p.mod_name.c_str() : p.name.c_str());
    if (g_font_heading) ImGui::PopFont();
    std::string meta;
    const char* region = RegionOf(p.title);
    if (*region && !package) meta = std::string(region) + "   ";
    meta += p.author;
    if (!p.version.empty()) meta += "  · v" + p.version;
    if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(meta.c_str()).x + 12) ImGui::SameLine(0,10);
    ImGui::TextColored(ImVec4(.65f,.73f,.87f,1), "%s",meta.c_str());
    const char* description = nullptr;
    if (p.ini_key && std::strcmp(p.ini_key,"patch_60fps")==0)
        description = T("Ejecuta el juego a 60 FPS. La versión 2.0 también elimina el vsync de doble búfer.",
                        "Runs the game at 60 FPS. Version 2.0 also removes double-buffer vsync.");
    if (p.ini_key && std::strcmp(p.ini_key,"patch_no_motion_blur")==0)
        description = T("Desactiva el desenfoque de movimiento: el de la cámara y el de los objetos que se mueven (público, plantas).",
                        "Turns off the motion blur: the camera's and that of moving objects (crowd, plants).");
    const bool unlock = p.ini_key && std::strcmp(p.ini_key,"patch_unlock_all")==0;
    if (unlock) description = T("Desbloquea el contenido que el juego concede al cumplir sus condiciones, con el interruptor de las demos E3/PAX.",
                               "Unlocks what the game normally grants for meeting its unlock conditions, using the same switch as the E3/PAX demos.");
    if (details && description) ImGui::TextWrapped("%s",description);
    if (unlock && details) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.20f,.23f,.30f,1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,7));
        ImGui::BeginChild("save_warning",ImVec2(0,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(kYellow,"%s",T("Copia antes tu partida.","Back up your save first.")); ImGui::SameLine();
        if (ImGui::TextLink(T("Abrir Guardados", "Open Saves tab"))) g_app.active_page=6;
        ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
    }
    if (details && package) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", p.mod_description.empty()
            ? T("El creador no ha añadido una descripción.","The creator has not provided a description.")
            : p.mod_description.c_str());
        ImGui::TextColored(ImVec4(.65f,.73f,.87f,1),"mods/%s",p.mod_source.c_str());
        if (!p.mod_versions.empty()) ImGui::TextWrapped("%s",p.mod_versions.c_str());
    }
    if (details && !package) {
        ImGui::Spacing();
        if (!p.note.empty()) ImGui::TextWrapped("%s",p.note.c_str());
        ImGui::TextColored(ImVec4(.65f,.73f,.87f,1),"%s",p.key.c_str());
    }
    ImGui::EndChild();
    const float end_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(right-action_width,top));
    if (package) {
        ImGui::BeginDisabled(g_app.game_running || !g_app.python);
        if (ImGui::Button("##remove",ImVec2(32,25))) remove=true;
        const ImVec2 q=ImGui::GetItemRectMin();
        const ImVec2 c{q.x+16,q.y+12};
        auto* d=ImGui::GetWindowDrawList(); const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
        d->AddRect({c.x-4,c.y-3},{c.x+4,c.y+6},ink,1);
        d->AddLine({c.x-6,c.y-5},{c.x+6,c.y-5},ink);
        d->AddLine({c.x-2,c.y-7},{c.x+2,c.y-7},ink);
        d->AddLine({c.x-1,c.y-1},{c.x-1,c.y+4},ink);
        d->AddLine({c.x+2,c.y-1},{c.x+2,c.y+4},ink);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",T("Quitar: se conserva una copia y las partidas.","Remove: keeps a recoverable copy and saves."));
        ImGui::EndDisabled();
    }
    // Reserve the same top-right slot regardless of expansion or card height.
    ImGui::SetCursorPos(ImVec2(right-32,top));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    if (ImGui::Button("##details",ImVec2(32,25))) {
        if (details) expanded.erase(p.key); else expanded.insert(p.key);
    }
    const bool hovered=ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip("%s",details ? T("Ocultar detalles","Hide details") : T("Mostrar detalles","Show details"));
    const ImVec2 origin=ImGui::GetItemRectMin();
    const ImVec2 center{origin.x+16,origin.y+12.5f};
    ImGuiStorage* storage=ImGui::GetStateStorage();
    const ImGuiID rotation_id=ImGui::GetID("chevron_rotation");
    const float target=expanded.count(p.key) ? 3.14159265f : 0.0f;
    float angle=storage->GetFloat(rotation_id,details ? 3.14159265f : 0.0f);
    angle+=(target-angle)*std::min(1.0f,ImGui::GetIO().DeltaTime*20.0f);
    storage->SetFloat(rotation_id,angle);
    const auto rotated=[&](float x,float y) {
        return ImVec2(center.x+x*std::cos(angle)-y*std::sin(angle),center.y+x*std::sin(angle)+y*std::cos(angle));
    };
    auto* draw=ImGui::GetWindowDrawList();
    const ImU32 ink=ImGui::GetColorU32(hovered ? ImVec4(.85f,.95f,1,1) : ImVec4(.65f,.77f,.91f,1));
    draw->AddLine(rotated(-5,-2.5f),rotated(0,2.5f),ink,1.8f);
    draw->AddLine(rotated(0,2.5f),rotated(5,-2.5f),ink,1.8f);
    ImGui::PopStyleColor(3); ImGui::PopStyleVar();
    ImGui::SetCursorPosY(std::max(end_y,top+26));
    ImGui::Dummy(ImVec2(0,0));
    ImGui::EndChild();
    ImGui::PopStyleVar(2); ImGui::PopStyleColor(2); ImGui::PopID();
    return remove;
}

bool ModActionIcon(const char* id, const char* tooltip, bool folder, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6);
    const bool clicked=ImGui::Button(id,ImVec2(size,size));
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tooltip);
    const ImVec2 a=ImGui::GetItemRectMin();
    const ImVec2 c{a.x+size*.5f,a.y+size*.5f};
    const ImU32 ink=ImGui::GetColorU32(ImVec4(.70f,.80f,.94f,1));
    ImDrawList* draw=ImGui::GetWindowDrawList();
    if (folder) {
        draw->PathLineTo({c.x-8,c.y-5}); draw->PathLineTo({c.x-2,c.y-5});
        draw->PathLineTo({c.x,c.y-2}); draw->PathLineTo({c.x+8,c.y-2});
        draw->PathLineTo({c.x+8,c.y+6}); draw->PathLineTo({c.x-8,c.y+6});
        draw->PathStroke(ink,ImDrawFlags_Closed,1.5f);
    } else {
        draw->PathArcTo(c,7,.65f,6.0f,24); draw->PathStroke(ink,0,1.5f);
        draw->AddLine({c.x+6.8f,c.y-2},{c.x+6.8f,c.y-8},ink,1.5f);
        draw->AddLine({c.x+6.8f,c.y-2},{c.x+1,c.y-2},ink,1.5f);
    }
    return clicked;
}

void ModsTab() {
    App& a = g_app;
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.83f,.88f,.97f,1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,ImVec4(.63f,.71f,.85f,1));
    ModSection(T("Mods incluidos","Built-in mods"),true);
    ImGui::Spacing();
    for (const auto& p : a.patches) if (VisibleMod(p) && p.ini_key) ModCard(p);
    ImGui::Dummy(ImVec2(0,14));
    const float left=ImGui::GetCursorPosX();
    const float right=ImGui::GetWindowContentRegionMax().x;
    const float gap=ImGui::GetStyle().ItemSpacing.x;
    const float icon_size=36.0f;
    const float folder_width=ImGui::CalcTextSize(T("Añadir carpeta","Add folder")).x+24;
    const float files_width=ImGui::CalcTextSize(T("+ Añadir archivos","+ Add files")).x+28;
    const float toolbar_width=icon_size*2+folder_width+files_width+gap*3;
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ModSection(T("Tus mods y parches","Your mods & patches"),false);
    ImGui::EndGroup();
    const float heading_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
    if (heading_end+24+toolbar_width<=right) ImGui::SameLine(right-toolbar_width);
    else {
        ImGui::Spacing();
        ImGui::SetCursorPosX(std::max(left,right-toolbar_width));
    }
    ImGui::BeginGroup();
    ImGui::BeginDisabled(a.game_running || !a.python);
    if (ModActionIcon("##reload_mods",T("Recargar mods","Reload mods"),false,icon_size)) {
        a.patches=LoadPatches(); ReadPatchConfig(a.patch_enabled,a.patch_disabled);
    }
    ImGui::SameLine();
    if (ModActionIcon("##open_mods",T("Abrir carpeta de mods","Open mods folder"),true,icon_size)) {
        std::error_code ec; fs::create_directories(a.root/L"mods",ec); OpenInShell(a.root/L"mods");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Añadir carpeta","Add folder"),ImVec2(folder_width,icon_size)))
        if (const auto folder=PickFolder(a.hwnd,L"Add mod folder",fs::path())) ImportMods({*folder});
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.17f,.72f,.94f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.02f,.10f,.20f,1));
    if (ImGui::Button(T("+ Añadir archivos","+ Add files"),ImVec2(files_width,icon_size)))
        ImportMods(PickFiles(a.hwnd,L"Add mods or patches",L"Mods (*.pak;*.zip;*.xml)",L"*.pak;*.zip;*.xml"));
    ImGui::PopStyleColor(2); ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::Spacing();
    std::string remove_id, remove_hash;
    for (const auto& p : a.patches) if (VisibleMod(p) && !p.ini_key) {
        if (ModCard(p)) { remove_id=PackagePatchId(p); remove_hash=p.mod_hash; }
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.015f,0.13f,0.32f,0.82f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(.18f,.29f,.44f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,11);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(18,17));
    ImGui::BeginChild("mod_drop_area",ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding);
    const auto centered = [](const char* text) {
        const float available=ImGui::GetContentRegionAvail().x;
        const float width=ImGui::CalcTextSize(text).x;
        if (width<available) ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(available-width)*.5f);
        ImGui::TextWrapped("%s",text);
    };
    centered(T("Añade .pak, .zip, .xml o una carpeta con los botones de arriba.",
               "Use the buttons above to add .pak, .zip or .xml files, or a folder."));
    centered(T("Se copian a la carpeta mods junto al launcher.","They're copied into the mods folder next to the launcher."));
    ImGui::Spacing();
    const char* guide=T("Lee la guía de empaquetado","Read the packaging guide");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,(ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(guide).x)*.5f));
    if (ImGui::TextLink(guide)) OpenInShell(a.root/L"docs"/L"mod-packaging.html");
    // The game's page on GameBanana, where the community shares its mods.
    const char* gamebanana=T("Buscar mods en GameBanana","Find mods on GameBanana");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,(ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(gamebanana).x)*.5f));
    if (ImGui::TextLink(gamebanana)) OpenInShell(L"https://gamebanana.com/games/26138");
    ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopStyleColor(2);
    if (a.game_running) ImGui::TextColored(kYellow,"%s",T("Cierra el juego para cambiar mods.","Close the game to change mods."));
    ImGui::PopStyleColor(2);
    // Reload only after rendering: the loop's references must remain valid.
    if (!remove_id.empty() && a.python) {
        const bool valid_id = std::all_of(remove_id.begin(), remove_id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
        const bool valid_hash = remove_hash.size() == 64 && std::all_of(remove_hash.begin(), remove_hash.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
        });
        if (!valid_id || !valid_hash) {
            a.notice = T("Los datos del mod no son válidos. Pulsa Recargar.", "Mod details are invalid. Press Reload.");
            return;
        }
        const std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " +
            Quote((a.root / L"scripts" / L"mod_packages.py").wstring()) + L" --scan " + Quote(a.root.wstring()) +
            L" --remove " + Quote(Widen(remove_id)) + L" --expected-sha " + Quote(Widen(remove_hash));
        const Captured r = RunCaptured(a.python->exe, cmd, a.root, 120000);
        if (!r.output.empty()) AddLog(r.output);
        a.notice = r.started && r.code == 0 ? T("Mod quitado. El mod y las partidas se han conservado.",
                                                                  "Mod removed. The mod and saves have been kept.")
                                           : T("No se pudo quitar el mod. Revisa el registro y pulsa Recargar.",
                                               "Could not remove the mod. Check the log and press Reload.");
        a.patches = LoadPatches();
        ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    }
}

void SavesTab() {
    App& a = g_app;
    if (a.saves_dirty) {
        a.saves = ListSaves();
        a.backups = ListBackups();
        a.saves_dirty = false;
    }
    const bool locked = a.game_running;
    if (locked) {
        ImGui::TextColored(kYellow, "%s", T("Cierra el juego para copiar, importar o restaurar partidas.",
                                            "Close the game to back up, import or restore saves."));
    }
    ImGui::SeparatorText(T("Partidas", "Saves"));
    ImGui::TextColored(kGrey, "%s", PathText(SaveRoot()).c_str());
    if (a.saves.empty()) {
        ImGui::TextUnformatted(T("Todavía no hay partidas guardadas.", "No saves yet."));
    } else if (ImGui::BeginTable("saves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Ranura", "Slot"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Descripción", "Description"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Modificada", "Modified"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& s : a.saves) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.dir.c_str());
            ImGui::TableNextColumn();
            std::string text = s.title;
            if (!s.subtitle.empty()) text += (text.empty() ? "" : " - ") + s.subtitle;
            if (!s.detail.empty()) text += (text.empty() ? "" : " - ") + s.detail;
            ImGui::TextUnformatted(text.empty() ? "-" : text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(s.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.modified.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(locked);
    ImGui::BeginDisabled(a.saves.empty());
    if (ImGui::Button(T("Hacer copia de seguridad", "Back up now"))) {
        fs::path made;
        const std::string error = BackupSaves(nullptr, &made);
        a.notice = error.empty() ? std::string(T("Copia creada: ", "Backup created: ")) + PathText(made.filename()) : error;
        a.saves_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Exportar...", "Export..."))) {
        if (const auto dest = PickFolder(a.hwnd, L"Exportar partidas / Export saves", fs::path())) {
            const fs::path target = *dest / Widen("CTR-saves-" + Stamp());
            const std::string error = CopyTree(SaveRoot(), target);
            a.notice = error.empty() ? std::string(T("Partidas exportadas a ", "Saves exported to ")) + PathText(target) : error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Importar...", "Import..."))) {
        if (const auto from = PickFolder(a.hwnd, L"Importar partida / Import save", fs::path())) {
            const std::string error = ImportSaves(*from);
            a.notice = error.empty() ? std::string(T("Partida importada. La anterior quedó en una copia de seguridad.",
                                                     "Save imported. The previous one was backed up."))
                                     : error;
            a.saves_dirty = true;
        }
    }
    HelpMarker(T("Acepta una copia de seguridad de este launcher, la carpeta CUSA13795 de las partidas de otro PC, o una "
                 "partida de PS4 descifrada (Apollo Save Tool, Save Wizard: una carpeta con sce_sys\\param.sfo). Antes de "
                 "importar se hace una copia de seguridad de tus partidas.",
                 "Takes a backup made by this launcher, the CUSA13795 saves folder from another PC, or a decrypted PS4 "
                 "save (Apollo Save Tool, Save Wizard: a folder with sce_sys\\param.sfo). Your saves are backed up "
                 "before importing."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir carpeta", "Open folder"))) {
        std::error_code ec;
        fs::create_directories(SaveRoot(), ec);
        OpenInShell(SaveRoot());
    }

    ImGui::SeparatorText(T("Copias de seguridad", "Backups"));
    if (a.backups.empty()) {
        ImGui::TextColored(kGrey, "%s", T("Todavía no hay copias.", "No backups yet."));
    } else if (ImGui::BeginTable("backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Copia", "Backup"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (const auto& b : a.backups) {
            ImGui::PushID(b.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(b.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);
            if (ImGui::SmallButton(T("Restaurar", "Restore"))) {
                a.pending_path = b.path;
                a.pending_action = 1;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(T("Borrar", "Delete"))) {
                a.pending_path = b.path;
                a.pending_action = 2;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(T("Abrir carpeta de copias", "Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(BackupRoot(), ec);
        OpenInShell(BackupRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar", "Refresh"))) a.saves_dirty = true;
}

// ---------------------------------------------------------------------------------------------
// DLC (optional): the player's add-on packages in <port>\dlc, as .pkg files or extracted folders.
// CTR Nitro-Fueled's DLC hold only the license; scripts/content_profile.py reads the same folder
// and the runtime reports each one installed (BB_DLC, BB_DLC_DISABLED from launcher.ini).

fs::path DlcRoot() { return g_app.root / L"dlc"; }

uint32_t Be32(const char* p) {
    return uint32_t(uint8_t(p[0])) << 24 | uint32_t(uint8_t(p[1])) << 16 | uint32_t(uint8_t(p[2])) << 8 | uint32_t(uint8_t(p[3]));
}

/// param.sfo of a PS4 package (entry 0x1000), when it is stored in the clear.
std::map<std::string, std::string> ReadPkgSfo(const fs::path& pkg, std::string& error) {
    std::ifstream f(pkg, std::ios::binary);
    char header[0x20];
    if (!f.read(header, sizeof header) || std::memcmp(header, "\x7f" "CNT", 4) != 0) {
        error = T("no es un paquete de PS4", "not a PS4 package");
        return {};
    }
    const uint32_t count = Be32(header + 0x10), table = Be32(header + 0x18);
    if (count == 0 || count > 4096) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    std::string entries(size_t(count) * 32, '\0');
    f.seekg(table);
    if (!f.read(entries.data(), std::streamsize(entries.size()))) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    for (uint32_t i = 0; i < count; ++i) {
        const char* e = entries.data() + size_t(i) * 32;
        if (Be32(e) != 0x1000) continue;
        const uint32_t flags = Be32(e + 8), offset = Be32(e + 16), size = Be32(e + 20);
        if (flags & 0x80000000u) {
            error = T("param.sfo está cifrado", "param.sfo is encrypted");
            return {};
        }
        if (size > (1u << 20)) break;
        std::string data(size, '\0');
        f.seekg(offset);
        if (!f.read(data.data(), size)) break;
        return ParseSfo(data);
    }
    error = T("no tiene param.sfo", "no param.sfo");
    return {};
}

DlcInfo DescribeDlc(const fs::path& path) {
    DlcInfo d;
    d.path = path;
    d.file = PathText(path.filename());
    std::error_code ec;
    std::map<std::string, std::string> sfo;
    if (fs::is_directory(path, ec)) {
        if (!fs::is_regular_file(path / L"sce_sys" / L"param.sfo", ec)) {
            d.problem = T("la carpeta no tiene sce_sys\\param.sfo", "the folder has no sce_sys\\param.sfo");
            return d;
        }
        sfo = ReadSfo(path / L"sce_sys" / L"param.sfo");
    } else {
        sfo = ReadPkgSfo(path, d.problem);
        if (!d.problem.empty()) return d;
    }
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    const std::string content = get("CONTENT_ID");
    const size_t dash = content.rfind('-');
    d.label = dash == std::string::npos ? std::string() : content.substr(dash + 1);
    d.title = get("TITLE");
    if (!IsCtrTitle(get("TITLE_ID")) || get("CATEGORY") != "ac" || d.label.size() != 16) {
        d.problem = T("no es un DLC de CTR Nitro-Fueled (CUSA13795 / CUSA14876)",
                      "not a CTR Nitro-Fueled (CUSA13795 / CUSA14876) DLC");
        return d;
    }
    if (d.title.empty()) d.title = d.label;
    d.ok = true;
    return d;
}

std::vector<DlcInfo> ScanDlc() {
    std::vector<DlcInfo> list;
    std::error_code ec;
    for (fs::directory_iterator it(DlcRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool pkg = it->is_regular_file(e2) && (it->path().extension() == L".pkg" || it->path().extension() == L".PKG");
        if (pkg || it->is_directory(e2)) list.push_back(DescribeDlc(it->path()));
    }
    std::sort(list.begin(), list.end(), [](const DlcInfo& a, const DlcInfo& b) { return a.title < b.title; });
    return list;
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!Trim(item).empty()) out.push_back(Trim(item));
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
    return out;
}

void AddDlcPackages() {
    App& a = g_app;
    const auto files = PickFiles(a.hwnd, L"DLC (.pkg)", L"PS4 DLC (*.pkg)", L"*.pkg");
    if (files.empty()) return;
    std::error_code ec;
    fs::create_directories(DlcRoot(), ec);
    int added = 0;
    std::string problems;
    for (const auto& file : files) {
        const DlcInfo d = DescribeDlc(file);
        if (!d.ok) {
            problems += PathText(file.filename()) + ": " + d.problem + ". ";
            continue;
        }
        const fs::path target = DlcRoot() / file.filename();
        if (!CopyFileW(file.c_str(), target.c_str(), FALSE)) {
            problems += PathText(file.filename()) + ": error " + std::to_string(GetLastError()) + ". ";
            continue;
        }
        ++added;
    }
    a.notice = std::string(T("DLC añadidos: ", "DLC added: ")) + std::to_string(added) + (problems.empty() ? "" : ". " + problems);
    a.dlc_dirty = true;
}

void DlcTab() {
    App& a = g_app;
    if (a.dlc_dirty) {
        a.dlc = ScanDlc();
        a.dlc_dirty = false;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Opcional: el juego funciona igual sin DLC. Los DLC de CTR Nitro-Fueled solo traen la licencia "
                             "(el contenido ya está en el juego); el port los marca como instalados al iniciar.",
                             "Optional: the game runs the same without DLC. CTR Nitro-Fueled's DLC only carry the license "
                             "(the content is already in the game); the port reports them installed at start."));
    ImGui::TextColored(kGrey, "%s", T("Usa tus propios paquetes DLC de PS4 (.pkg) de CUSA13795. Se copian a la carpeta dlc del "
                                      "port; tus archivos originales no cambian.",
                                      "Use your own PS4 DLC packages (.pkg) for CUSA13795. They are copied into the port's "
                                      "dlc folder; your original files are not changed."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    LauncherFlag("dlc", T("Activar los DLC", "Enable DLC"), true);
    const bool enabled = a.launcher.Flag("dlc", true);
    std::vector<std::string> disabled = SplitList(a.launcher.Get("dlc_disabled"));

    ImGui::SeparatorText(T("DLC encontrados", "DLC found"));
    if (a.dlc.empty()) ImGui::TextColored(kGrey, "%s", T("No hay DLC en la carpeta dlc.", "No DLC in the dlc folder."));
    ImGui::BeginDisabled(!enabled);
    for (const auto& d : a.dlc) {
        ImGui::PushID(d.file.c_str());
        if (d.ok) {
            bool on = std::find(disabled.begin(), disabled.end(), d.label) == disabled.end();
            if (ImGui::Checkbox(d.title.c_str(), &on)) {
                disabled.erase(std::remove(disabled.begin(), disabled.end(), d.label), disabled.end());
                if (!on) disabled.push_back(d.label);
                a.launcher.Set("dlc_disabled", JoinList(disabled));
                SaveLauncher();
            }
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", d.label.c_str());
        } else {
            ImGui::TextColored(kRed, "%s: %s", d.file.c_str(), d.problem.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(a.game_running);
        if (ImGui::SmallButton(T("Quitar", "Remove"))) {
            a.pending_path = d.path;
            a.pending_action = 3;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button(T("Añadir DLC (.pkg)...", "Add DLC (.pkg)..."))) AddDlcPackages();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta dlc", "Open the dlc folder"))) {
        std::error_code ec;
        fs::create_directories(DlcRoot(), ec);
        OpenInShell(DlcRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar##dlc", "Refresh##dlc"))) a.dlc_dirty = true;
    ImGui::TextColored(kGrey, "%s", T("Los cambios se aplican la próxima vez que pulses Jugar.", "Changes apply the next time you press Play."));
}

/// Asked when PLAY is pressed online without a usable nickname: the game only starts once one
/// is chosen (or online is turned off for this launch).
void NickPopup() {
    App& a = g_app;
    if (a.ask_nick) {
        ImGui::OpenPopup("nick_required");
        a.ask_nick = false;
    }
    if (!ImGui::BeginPopupModal("nick_required", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::TextUnformatted(T("Elige tu apodo para jugar online", "Choose your nickname to play online"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextColored(kGrey, "%s", T("Es tu nombre en el online y tu cuenta en el servidor: tus monedas y objetos del Pit Stop "
                                      "van ligados a él. De 3 a 16 letras, números, _ o -.",
                                      "It is your online name and your account on the server: your coins and Pit Stop items "
                                      "belong to it. 3 to 16 letters, digits, _ or -."));
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    NickInput(T("Apodo", "Nickname"));
    const char* problem = NickProblem(CurrentNick());
    if (problem) ImGui::TextColored(kYellow, "%s", problem);
    else NickAvailabilityLine();
    if (problem || NickTaken()) DiscordRecoverOffer();  // a reinstall: the nickname linked to Discord
    ImGui::BeginDisabled(problem != nullptr || NickTaken());
    if (ImGui::Button(T("Guardar y jugar", "Save and play"))) {
        SaveGameIni();
        ImGui::CloseCurrentPopup();
        StartGame();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancelar", "Cancel"))) {
        a.game.Load(GameIni());  // drop what was typed: every other setting is already saved
        SyncOnlineBuffers();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// While run.py builds the native renderer's shaders (the first start with it, ~4 minutes).
void ShaderProgressPopup() {
    App& a = g_app;
    const int percent = a.shader_percent.load();
    if (percent >= 0 && !ImGui::IsPopupOpen("native_shaders")) ImGui::OpenPopup("native_shaders");
    if (!ImGui::BeginPopupModal("native_shaders", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    if (percent < 0 || !a.game_running) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    std::string stage;
    {
        std::scoped_lock lock{a.mutex};
        stage = a.shader_stage;
    }
    const char* step = stage == "extract"     ? T("Leyendo los shaders del juego...", "Reading the game's shaders...")
                       : stage == "export"    ? T("Preparando los programas...", "Preparing the programs...")
                       : stage == "translate" ? T("Traduciendo los shaders...", "Translating the shaders...")
                       : stage == "compile"   ? T("Compilando los shaders...", "Compiling the shaders...")
                                              : T("Terminando...", "Finishing...");
    ImGui::TextUnformatted(T("Compilando shaders del motor nativo", "Compiling the native engine's shaders"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextColored(kGrey, "%s", T("Solo la primera vez: se preparan a partir de tu copia del juego y tardan unos "
                                      "minutos. El juego se abre solo al terminar.",
                                      "Only the first time: they are built from your copy of the game and take a few "
                                      "minutes. The game opens by itself when they are done."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    char overlay[16];
    std::snprintf(overlay, sizeof overlay, "%d %%", percent);
    ImGui::ProgressBar(percent / 100.0f, ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), overlay);
    ImGui::TextColored(kGrey, "%s", step);
    ImGui::Spacing();
    if (ImGui::Button(T("Cancelar", "Cancel"))) {
        StopGame();
        a.shader_percent = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// A Discord invite for a server other than ours: switching is the player's call.
void JoinServerPopup() {
    App& a = g_app;
    if (a.ask_join_server) {
        ImGui::OpenPopup("join_server");
        a.ask_join_server = false;
    }
    if (!ImGui::BeginPopupModal("join_server", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::Text(T("Invitación de %s", "Invite from %s"), a.join_nick.c_str());
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    const std::string mine = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    ImGui::TextColored(kYellow, T("Es para el servidor %s y tú usas %s.", "It is for the server %s and you use %s."),
                       a.join_server.c_str(), mine.c_str());
    ImGui::TextColored(kGrey, "%s", T("Cámbialo solo si confías en ese servidor: verá tu apodo y tu partida online. "
                                      "Puedes volver al tuyo en la pestaña Online.",
                                      "Only switch if you trust that server: it will see your nickname and your online "
                                      "play. You can switch back in the Online tab."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(T("Cambiar de servidor y unirme", "Switch server and join"))) {
        AcceptDiscordJoin(a.join_server, a.join_nick);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SaveConfirmations() {
    App& a = g_app;
    if (a.pending_action) {
        ImGui::OpenPopup("save_confirm");
        a.confirm_action = a.pending_action;
        a.pending_action = 0;
    }
    if (ImGui::BeginPopupModal("save_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const std::string name = PathText(a.pending_path.filename());
        if (a.confirm_action == 1) {
            ImGui::Text(T("¿Restaurar %s?", "Restore %s?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Tus partidas actuales se guardan antes en una copia automática.",
                                              "Your current saves are backed up automatically first."));
        } else if (a.confirm_action == 3) {
            ImGui::Text(T("¿Quitar %s de la carpeta dlc?", "Remove %s from the dlc folder?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Solo se borra la copia del port.", "Only the port's copy is deleted."));
        } else {
            ImGui::Text(T("¿Borrar la copia %s? No se puede deshacer.", "Delete the backup %s? This cannot be undone."), name.c_str());
        }
        if (ImGui::Button(a.confirm_action == 1 ? T("Restaurar", "Restore") : a.confirm_action == 3 ? T("Quitar", "Remove") : T("Borrar", "Delete"))) {
            std::string error;
            if (a.confirm_action == 1) {
                error = RestoreBackup(a.pending_path);
                if (error.empty()) a.notice = std::string(T("Restaurada: ", "Restored: ")) + name;
            } else {
                std::error_code ec;
                fs::remove_all(a.pending_path, ec);
                if (ec) error = name + ": " + ec.message();
            }
            if (!error.empty()) a.notice = error;
            a.saves_dirty = true;
            a.dlc_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// Keyboard tab: bbport.ini key.<input>=<SDL key names> (src/runtime_pad.c reads them at start;
// a missing line keeps the runtime's default). Keys are captured by their scan code (the key's
// place on the keyboard, as SDL's scancodes), so the binding holds whatever the layout.

struct KeyInput {
    const char* id;      // runtime_pad.c input name
    const char* es;
    const char* en;
    const char* fallback;  // runtime_pad.c bind_defaults()
};
constexpr KeyInput kKeyInputs[] = {
    {"move_up", "Stick izquierdo arriba", "Left stick up", "W"},
    {"move_down", "Stick izquierdo abajo", "Left stick down", "S"},
    {"move_left", "Stick izquierdo izquierda (girar)", "Left stick left (steer)", "A"},
    {"move_right", "Stick izquierdo derecha (girar)", "Left stick right (steer)", "D"},
    {"cross", "X (acelerar)", "Cross (accelerate)", "Space"},
    {"square", "Cuadrado (frenar)", "Square (brake)", "E"},
    {"circle", "Círculo", "Circle", "Left Shift"},
    {"triangle", "Triángulo", "Triangle", "Q"},
    {"r1", "R1", "R1", "3"},
    {"l1", "L1", "L1", "1"},
    {"r2", "R2", "R2", "F"},
    {"l2", "L2", "L2", "R"},
    {"l3", "L3", "L3", "Z"},
    {"r3", "R3", "R3", "C"},
    {"options", "Options (pausa)", "Options (pause)", "Return"},
    {"touchpad", "Panel táctil", "Touchpad", "Tab"},
    {"touchpad_right", "Panel táctil (derecha)", "Touchpad (right)", "Backspace"},
    {"up", "Cruceta arriba", "D-pad up", "I"},
    {"down", "Cruceta abajo", "D-pad down", "K"},
    {"left", "Cruceta izquierda", "D-pad left", "J"},
    {"right", "Cruceta derecha", "D-pad right", "L"},
    {"look_up", "Stick derecho arriba", "Right stick up", "Up"},
    {"look_down", "Stick derecho abajo", "Right stick down", "Down"},
    {"look_left", "Stick derecho izquierda", "Right stick left", "Left"},
    {"look_right", "Stick derecho derecha", "Right stick right", "Right"},
};

// Scan code set 1 (WM_KEYDOWN lParam bits 16-23, + 0x100 for E0-extended) -> SDL scancode name.
const char* SdlKeyName(unsigned code) {
    static const std::map<unsigned, const char*> names = {
        {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"}, {0x07, "6"}, {0x08, "7"},
        {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"},
        {0x0F, "Tab"}, {0x10, "Q"}, {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"},
        {0x16, "U"}, {0x17, "I"}, {0x18, "O"}, {0x19, "P"}, {0x1A, "["}, {0x1B, "]"},
        {0x1C, "Return"}, {0x1D, "Left Ctrl"}, {0x1E, "A"}, {0x1F, "S"}, {0x20, "D"}, {0x21, "F"},
        {0x22, "G"}, {0x23, "H"}, {0x24, "J"}, {0x25, "K"}, {0x26, "L"}, {0x27, ";"}, {0x28, "'"},
        {0x29, "`"}, {0x2A, "Left Shift"}, {0x2B, "\\"}, {0x2C, "Z"}, {0x2D, "X"}, {0x2E, "C"},
        {0x2F, "V"}, {0x30, "B"}, {0x31, "N"}, {0x32, "M"}, {0x33, ","}, {0x34, "."}, {0x35, "/"},
        {0x36, "Right Shift"}, {0x37, "Keypad *"}, {0x38, "Left Alt"}, {0x39, "Space"},
        {0x3A, "CapsLock"}, {0x3B, "F1"}, {0x3C, "F2"}, {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"},
        {0x40, "F6"}, {0x41, "F7"}, {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"}, {0x57, "F11"},
        {0x58, "F12"}, {0x47, "Keypad 7"}, {0x48, "Keypad 8"}, {0x49, "Keypad 9"},
        {0x4A, "Keypad -"}, {0x4B, "Keypad 4"}, {0x4C, "Keypad 5"}, {0x4D, "Keypad 6"},
        {0x4E, "Keypad +"}, {0x4F, "Keypad 1"}, {0x50, "Keypad 2"}, {0x51, "Keypad 3"},
        {0x52, "Keypad 0"}, {0x53, "Keypad ."}, {0x56, "NonUSBackslash"},
        {0x11C, "Keypad Enter"}, {0x11D, "Right Ctrl"}, {0x135, "Keypad /"}, {0x138, "Right Alt"},
        {0x147, "Home"}, {0x148, "Up"}, {0x149, "PageUp"}, {0x14B, "Left"}, {0x14D, "Right"},
        {0x14F, "End"}, {0x150, "Down"}, {0x151, "PageDown"}, {0x153, "Delete"},
    };
    const auto it = names.find(code);
    return it == names.end() ? nullptr : it->second;
}

// Mouse buttons and wheel steps as captured keys: kMouseCode + SDL's button number (wheel 6 and 7),
// with the names src/runtime_pad.c and tools/gpu_capabilities.c --read-input use.
constexpr unsigned kMouseCode = 0x1000;
const char* MouseKeyName(unsigned code) {
    static const char* const names[] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1",
                                        "Mouse X2",   "Wheel Up",     "Wheel Down"};
    return code > kMouseCode && code <= kMouseCode + 7 ? names[code - kMouseCode - 1] : nullptr;
}

// How a key name reads in the launcher.
std::string KeyLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"Space", {"Espacio", "Space"}}, {"Return", {"Enter", "Enter"}},
        {"Backspace", {"Retroceso", "Backspace"}}, {"Left Shift", {"Mayús izq.", "Left Shift"}},
        {"Right Shift", {"Mayús der.", "Right Shift"}}, {"Left Ctrl", {"Ctrl izq.", "Left Ctrl"}},
        {"Right Ctrl", {"Ctrl der.", "Right Ctrl"}}, {"Left Alt", {"Alt", "Left Alt"}},
        {"Right Alt", {"Alt Gr", "Right Alt"}}, {"Up", {"Flecha arriba", "Up arrow"}},
        {"Down", {"Flecha abajo", "Down arrow"}}, {"Left", {"Flecha izquierda", "Left arrow"}},
        {"Right", {"Flecha derecha", "Right arrow"}}, {"CapsLock", {"Bloq Mayús", "Caps Lock"}},
        {"Delete", {"Supr", "Delete"}}, {"Home", {"Inicio", "Home"}}, {"End", {"Fin", "End"}},
        {"PageUp", {"Re Pág", "Page Up"}}, {"PageDown", {"Av Pág", "Page Down"}},
        {"`", {"Tecla a la izquierda del 1", "` key"}},
        {"NonUSBackslash", {"Tecla < >", "< > key"}}, {"Keypad Enter", {"Enter (teclado num.)", "Keypad Enter"}},
        {"Mouse Left", {"Clic izquierdo", "Left click"}}, {"Mouse Right", {"Clic derecho", "Right click"}},
        {"Mouse Middle", {"Clic central (rueda)", "Middle click (wheel)"}},
        {"Mouse X1", {"Botón lateral 1 del ratón", "Mouse side button 1"}},
        {"Mouse X2", {"Botón lateral 2 del ratón", "Mouse side button 2"}},
        {"Wheel Up", {"Rueda arriba", "Wheel up"}}, {"Wheel Down", {"Rueda abajo", "Wheel down"}},
    };
    const auto it = labels.find(name);
    if (it != labels.end()) return T(it->second.first, it->second.second);
    if (name.rfind("Keypad ", 0) == 0) return std::string(T("Num ", "Num ")) + name.substr(7);
    return name;
}

std::atomic<int> g_key_capture{-1};      // the input waiting for a key, -1 none
std::atomic<unsigned> g_key_captured{0};  // scan code + 1 (0: nothing yet)
bool g_key_replace = false;                 // the captured key replaces the default binding

std::vector<std::string> SplitKeys(const std::string& value) {
    std::vector<std::string> keys;
    std::stringstream ss(value);
    std::string key;
    while (std::getline(ss, key, ',')) {
        key = Trim(key);
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}

void KeyboardTab() {
    App& a = g_app;
    ImGui::TextWrapped("%s", T("Teclas del jugador 1. El teclado funciona a la vez que el mando. Pulsa \"Cambiar\" y luego la "
                               "tecla; cada acción admite hasta 4 teclas. Se guarda en bbport.ini y vale desde el próximo arranque.",
                               "Player 1's keys. The keyboard works together with the controller. Press \"Change\", then the key; "
                               "each action takes up to 4 keys. Saved in bbport.ini, used from the next start."));
    ImGui::TextColored(kGrey, "%s", T("Insert abre el menú del juego y Esc cancela, por eso no se pueden asignar.",
                                      "Insert opens the in-game menu and Esc cancels, so they cannot be bound."));
    const int capturing = g_key_capture;
    if (capturing >= 0) {
        const unsigned got = g_key_captured.exchange(0);
        if (got) {
            const unsigned code = got - 1;
            if (code == 0x01) {
                g_key_capture = -1;  // Esc: cancel
            } else if (const char* name = code > kMouseCode ? MouseKeyName(code) : SdlKeyName(code);
                       name && code != 0x152) {
                const KeyInput& in = kKeyInputs[capturing];
                const std::string key = std::string("key.") + in.id;
                auto keys = g_key_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
                if (std::find(keys.begin(), keys.end(), name) == keys.end()) {
                    if (keys.size() >= 4) keys.erase(keys.begin());
                    keys.push_back(name);
                }
                std::string value;
                for (const auto& k : keys) value += (value.empty() ? "" : ",") + k;
                a.game.Set(key, value);
                SaveGameIni();
                g_key_capture = -1;
            } else {
                a.notice = T("Esa tecla no se puede asignar.", "That key cannot be bound.");
                g_key_capture = -1;
            }
        }
    }

    ImGui::SeparatorText(T("Controles", "Controls"));
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Teclas", "Keys"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const std::string key = std::string("key.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto keys = SplitKeys(custom ? a.game.Get(key) : in.fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa una tecla o un botón del ratón... (Esc cancela)",
                                                    "Press a key or a mouse button... (Esc cancels)"));
            } else if (keys.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin tecla)", "(none)"));
            } else {
                std::string text;
                for (const auto& k : keys) text += (text.empty() ? "" : "  /  ") + KeyLabel(k);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capturing >= 0);
            if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                // A key replaces the default binding; on a custom one it is added (up to 4).
                g_key_replace = !custom;
                g_key_captured = 0;
                g_key_capture = i;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!custom);
            if (ImGui::SmallButton(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Restaurar todas las teclas", "Restore all keys"))) {
        for (const auto& in : kKeyInputs) a.game.Erase(std::string("key.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: tecla original. Las teclas van por su posición en el teclado.",
                                      "Grey: default key. Keys go by their place on the keyboard."));
}

// ---------------------------------------------------------------------------------------------
// Gamepad tab: bbport.ini pad.<input>=<SDL gamepad button names> (src/runtime_pad.c reads them).

struct PadInput {
    const char* id;
    const char* es;
    const char* en;
    const char* fallback;
};

constexpr PadInput kPadInputs[] = {
    {"cross", "X (Acelerar / Seleccionar)", "Cross (Accelerate / Select)", "a"},
    {"square", "Cuadrado (Frenar / Marcha atrás)", "Square (Brake / Reverse)", "x"},
    {"circle", "Círculo", "Circle", "b"},
    {"triangle", "Triángulo", "Triangle", "y"},
    {"l1", "L1 / LB (Salto / Derrape)", "L1 / LB (Hop / Drift)", "leftshoulder"},
    {"r1", "R1 / RB (Salto / Derrape)", "R1 / RB (Hop / Drift)", "rightshoulder"},
    {"l2", "L2 / LT (Usar objeto / Ítem)", "L2 / LT (Use item)", "lefttrigger"},
    {"r2", "R2 / RT (Mirar atrás)", "R2 / RT (Look back)", "righttrigger"},
    {"l3", "L3 (Click stick izq.)", "L3 (Left stick click)", "leftstick"},
    {"r3", "R3 (Click stick der.)", "R3 (Right stick click)", "rightstick"},
    {"options", "Options / Start (Pausa)", "Options / Start (Pause)", "start"},
    {"touchpad", "Touchpad / Back / Select", "Touchpad / Back / Select", "back,touchpad"},
    {"up", "Cruceta arriba", "D-pad up", "dpup"},
    {"down", "Cruceta abajo", "D-pad down", "dpdown"},
    {"left", "Cruceta izquierda", "D-pad left", "dpleft"},
    {"right", "Cruceta derecha", "D-pad right", "dpright"},
};

std::string PadButtonLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"a", {"Cruz / A (Sur)", "Cross / A (South)"}},
        {"b", {"Círculo / B (Este)", "Circle / B (East)"}},
        {"x", {"Cuadrado / X (Oeste)", "Square / X (West)"}},
        {"y", {"Triángulo / Y (Norte)", "Triangle / Y (North)"}},
        {"leftshoulder", {"L1 / LB", "L1 / LB"}},
        {"rightshoulder", {"R1 / RB", "R1 / RB"}},
        {"lefttrigger", {"Gatillo L2 / LT", "Trigger L2 / LT"}},
        {"righttrigger", {"Gatillo R2 / RT", "Trigger R2 / RT"}},
        {"leftstick", {"Click L3 (Stick Izq.)", "L3 Click (Left Stick)"}},
        {"rightstick", {"Click R3 (Stick Der.)", "R3 Click (Right Stick)"}},
        {"start", {"Options / Start", "Options / Start"}},
        {"back", {"Share / Back / Select", "Share / Back / Select"}},
        {"touchpad", {"Touchpad", "Touchpad"}},
        {"dpup", {"Cruceta Arriba", "D-pad Up"}},
        {"dpdown", {"Cruceta Abajo", "D-pad Down"}},
        {"dpleft", {"Cruceta Izquierda", "D-pad Left"}},
        {"dpright", {"Cruceta Derecha", "D-pad Right"}},
        {"guide", {"Botón Guía / PS", "Guide / PS"}},
        {"misc1", {"Captura / Misc", "Capture / Misc"}},
        {"paddle1", {"Palanca 1", "Paddle 1"}},
        {"paddle2", {"Palanca 2", "Paddle 2"}},
        {"paddle3", {"Palanca 3", "Paddle 3"}},
        {"paddle4", {"Palanca 4", "Paddle 4"}},
    };
    const auto it = labels.find(name);
    return it != labels.end() ? T(it->second.first, it->second.second) : name;
}

#pragma pack(push, 1)
struct LauncherXInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct LauncherXInputState {
    DWORD dwPacketNumber;
    LauncherXInputGamepad Gamepad;
};
#pragma pack(pop)

typedef DWORD (WINAPI *PFN_LauncherXInputGetState)(DWORD dwUserIndex, void* pState);

const char* PollGamepadButton() {
    static HMODULE s_xinput = nullptr;
    static PFN_LauncherXInputGetState s_getState = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        s_xinput = LoadLibraryA("xinput1_4.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput1_3.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput9_1_0.dll");
        if (s_xinput) {
            s_getState = (PFN_LauncherXInputGetState)GetProcAddress(s_xinput, "XInputGetState");
        }
    }
    if (!s_getState) return nullptr;

    for (DWORD i = 0; i < 4; ++i) {
        LauncherXInputState state{};
        if (s_getState(i, &state) == 0) {
            if (state.Gamepad.bLeftTrigger > 80) return "lefttrigger";
            if (state.Gamepad.bRightTrigger > 80) return "righttrigger";
            if (state.Gamepad.wButtons & 0x1000) return "a";
            if (state.Gamepad.wButtons & 0x2000) return "b";
            if (state.Gamepad.wButtons & 0x4000) return "x";
            if (state.Gamepad.wButtons & 0x8000) return "y";
            if (state.Gamepad.wButtons & 0x0100) return "leftshoulder";
            if (state.Gamepad.wButtons & 0x0200) return "rightshoulder";
            if (state.Gamepad.wButtons & 0x0040) return "leftstick";
            if (state.Gamepad.wButtons & 0x0080) return "rightstick";
            if (state.Gamepad.wButtons & 0x0010) return "start";
            if (state.Gamepad.wButtons & 0x0020) return "back";
            if (state.Gamepad.wButtons & 0x0001) return "dpup";
            if (state.Gamepad.wButtons & 0x0002) return "dpdown";
            if (state.Gamepad.wButtons & 0x0004) return "dpleft";
            if (state.Gamepad.wButtons & 0x0008) return "dpright";
        }
    }
    return nullptr;
}

std::atomic<int> g_pad_capture{-1};
bool g_pad_waiting_release = false;

void GamepadTab() {
    App& a = g_app;
    ImGui::TextWrapped("%s", T("Mapeo de botones del mando (Jugador 1). Compatible con mandos de Xbox, PlayStation (DualShock / DualSense), Switch Pro y mandos genéricos. "
                               "Pulsa \"Cambiar\" y presiona el botón en tu mando o selecciónalo en la pantalla. Se guarda en bbport.ini y se aplica en el próximo inicio.",
                               "Player 1 gamepad button mapping. Supports Xbox, PlayStation (DualShock / DualSense), Switch Pro and generic controllers. "
                               "Press \"Change\" and press the button on your controller or choose it on screen. Saved to bbport.ini, applies on next launch."));

    const int capturing = g_pad_capture;
    if (capturing >= 0) {
        const char* polled = PollGamepadButton();
        if (g_pad_waiting_release) {
            if (!polled) g_pad_waiting_release = false;
        } else if (polled) {
            const PadInput& in = kPadInputs[capturing];
            a.game.Set(std::string("pad.") + in.id, polled);
            SaveGameIni();
            g_pad_capture = -1;
        }
    }

    if (capturing >= 0 && capturing < int(std::size(kPadInputs))) {
        const PadInput& in = kPadInputs[capturing];
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Border, kYellow);
        ImGui::BeginChild("pad_assign_box", ImVec2(0, 160), true);
        ImGui::TextColored(kYellow, "%s: %s", T("ASIGNAR BOTÓN PARA", "ASSIGN BUTTON FOR"), T(in.es, in.en));
        ImGui::TextColored(kGrey, "%s", T("Presiona cualquier botón o gatillo en tu mando, o haz clic en uno de abajo:",
                                          "Press any button or trigger on your controller, or click one below:"));
        ImGui::Spacing();

        auto AddBtn = [&](const char* id, const char* label) {
            if (ImGui::Button(label)) {
                a.game.Set(std::string("pad.") + in.id, id);
                SaveGameIni();
                g_pad_capture = -1;
            }
            ImGui::SameLine();
        };

        AddBtn("a", "Cruz / A");
        AddBtn("b", "Círculo / B");
        AddBtn("x", "Cuadrado / X");
        AddBtn("y", "Triángulo / Y");
        AddBtn("leftshoulder", "L1 / LB");
        AddBtn("rightshoulder", "R1 / RB");
        ImGui::NewLine();
        AddBtn("lefttrigger", "Gatillo L2 / LT");
        AddBtn("righttrigger", "Gatillo R2 / RT");
        AddBtn("dpup", "Cruceta Arriba");
        AddBtn("dpdown", "Cruceta Abajo");
        AddBtn("dpleft", "Cruceta Izq.");
        AddBtn("dpright", "Cruceta Der.");
        ImGui::NewLine();
        AddBtn("leftstick", "L3 (Stick Izq.)");
        AddBtn("rightstick", "R3 (Stick Der.)");
        AddBtn("start", "Start / Options");
        AddBtn("back", "Select / Back");
        AddBtn("touchpad", "Touchpad");
        if (ImGui::Button(T("Cancelar (Esc)", "Cancel (Esc)"))) {
            g_pad_capture = -1;
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SeparatorText(T("Botones del mando", "Gamepad buttons"));
    if (ImGui::BeginTable("pad_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Botón asignado", "Assigned button"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < int(std::size(kPadInputs)); ++i) {
            const PadInput& in = kPadInputs[i];
            const std::string key = std::string("pad.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto buttons = SplitKeys(custom ? a.game.Get(key) : in.fallback);

            ImGui::PushID(i + 200);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Esperando botón...", "Waiting for button..."));
            } else if (buttons.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin botón asignado)", "(unbound)"));
            } else {
                std::string text;
                for (const auto& b : buttons) text += (text.empty() ? "" : "  /  ") + PadButtonLabel(b);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capturing >= 0);
            if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                g_pad_capture = i;
                g_pad_waiting_release = (PollGamepadButton() != nullptr);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!custom);
            if (ImGui::SmallButton(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText(T("Preajustes rápidos", "Quick presets"));
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Cruceta Abajo = L2 (Usar Ítem)", "D-pad Down = L2 (Use Item)"))) {
        a.game.Set("pad.l2", "dpdown");
        a.game.Set("pad.down", "lefttrigger");
        SaveGameIni();
        a.notice = T("Preajuste aplicado: Cruceta Abajo configurada como L2 (Ítem) y Gatillo L2 como Cruceta Abajo.",
                     "Preset applied: D-pad Down set to L2 (Item) and Trigger L2 set to D-pad Down.");
    }
    HelpMarker(T("Intercambia la Cruceta Abajo con el Gatillo L2. Ideal para disparar objetos con la flecha de abajo.",
                 "Swaps D-pad Down with Trigger L2. Ideal for using items with down arrow."));
    ImGui::SameLine();
    if (ImGui::Button(T("Restaurar todos los botones", "Restore all pad buttons"))) {
        for (const auto& in : kPadInputs) a.game.Erase(std::string("pad.") + in.id);
        SaveGameIni();
        a.notice = T("Todos los botones del mando se restauraron a los valores originales.",
                     "All gamepad buttons restored to defaults.");
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextColored(kGrey, "%s", T("Gris: asignación por defecto. Blanco: botón personalizado guardado en bbport.ini.",
                                      "Grey: default binding. White: custom binding saved to bbport.ini."));
}

// ---------------------------------------------------------------------------------------------
// Shortcuts & Steam integration (Desktop shortcut and Steam custom artwork / shortcuts.vdf)

uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

fs::path FindSteamPath() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            std::wstring s = buf;
            std::replace(s.begin(), s.end(), L'/', L'\\');
            return fs::path(s);
        }
        RegCloseKey(key);
    }
    for (const auto& p : {L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam", L"D:\\Steam"}) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) return fs::path(p);
    }
    return {};
}

struct SteamShortcut {
    uint32_t appid = 0;
    std::string app_name;
    std::string exe;
    std::string start_dir;
    std::string icon;
    std::string shortcut_path;
    std::string launch_options;
    int32_t is_hidden = 0;
    int32_t allow_desktop_config = 1;
    int32_t allow_overlay = 1;
    int32_t open_vr = 0;
    int32_t devkit = 0;
    std::string devkit_game_id;
    int32_t devkit_override_app_id = 0;
    int32_t last_play_time = 0;
    std::string flatpak_app_id;
    std::string sortas;
    std::vector<std::string> tags;
};

[[maybe_unused]] std::vector<SteamShortcut> ParseShortcuts(const std::vector<uint8_t>& buf) {
    std::vector<SteamShortcut> list;
    size_t i = 0;
    auto read_sz = [&]() -> std::string {
        size_t start = i;
        while (i < buf.size() && buf[i] != 0) ++i;
        std::string s(reinterpret_cast<const char*>(&buf[start]), i - start);
        if (i < buf.size()) ++i;
        return s;
    };
    if (buf.size() < 12 || buf[i++] != 0) return list;
    std::string root = read_sz();
    if (root != "shortcuts") return list;
    while (i < buf.size() && buf[i] != 8) {
        if (buf[i++] != 0) break;
        std::string idx = read_sz();
        SteamShortcut sc;
        auto iequals = [](const std::string& s1, const std::string& s2) {
            if (s1.size() != s2.size()) return false;
            for (size_t k = 0; k < s1.size(); ++k)
                if (::tolower((unsigned char)s1[k]) != ::tolower((unsigned char)s2[k])) return false;
            return true;
        };
        while (i < buf.size() && buf[i] != 8) {
            uint8_t type = buf[i++];
            std::string key = read_sz();
            if (type == 1) { // String
                std::string val = read_sz();
                if (iequals(key, "appname")) sc.app_name = val;
                else if (iequals(key, "exe")) sc.exe = val;
                else if (iequals(key, "startdir")) sc.start_dir = val;
                else if (iequals(key, "icon")) sc.icon = val;
                else if (iequals(key, "shortcutpath")) sc.shortcut_path = val;
                else if (iequals(key, "launchoptions")) sc.launch_options = val;
                else if (iequals(key, "devkitgameid")) sc.devkit_game_id = val;
                else if (iequals(key, "flatpakappid")) sc.flatpak_app_id = val;
                else if (iequals(key, "sortas")) sc.sortas = val;
            } else if (type == 2) { // Int32
                if (i + 4 > buf.size()) break;
                uint32_t val = 0;
                std::memcpy(&val, &buf[i], 4);
                i += 4;
                if (iequals(key, "appid")) sc.appid = val;
                else if (iequals(key, "ishidden")) sc.is_hidden = val;
                else if (iequals(key, "allowdesktopconfig")) sc.allow_desktop_config = val;
                else if (iequals(key, "allowoverlay")) sc.allow_overlay = val;
                else if (iequals(key, "openvr")) sc.open_vr = val;
                else if (iequals(key, "devkit")) sc.devkit = val;
                else if (iequals(key, "devkitoverrideappid")) sc.devkit_override_app_id = val;
                else if (iequals(key, "lastplaytime")) sc.last_play_time = val;
            } else if (type == 0) { // Submap, e.g. tags
                while (i < buf.size() && buf[i] != 8) {
                    uint8_t subtype = buf[i++];
                    std::string tag_idx = read_sz();
                    if (subtype == 1) sc.tags.push_back(read_sz());
                    else if (subtype == 2) i += 4;
                }
                if (i < buf.size() && buf[i] == 8) ++i;
            }
        }
        if (i < buf.size() && buf[i] == 8) ++i;
        list.push_back(sc);
    }
    return list;
}

[[maybe_unused]] std::vector<uint8_t> SerializeShortcuts(const std::vector<SteamShortcut>& list) {
    std::vector<uint8_t> out;
    auto write_byte = [&](uint8_t b) { out.push_back(b); };
    auto write_sz = [&](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    };
    auto write_str = [&](const std::string& key, const std::string& val) {
        write_byte(1);
        write_sz(key);
        write_sz(val);
    };
    auto write_int = [&](const std::string& key, int32_t val) {
        write_byte(2);
        write_sz(key);
        uint8_t b[4];
        std::memcpy(b, &val, 4);
        out.insert(out.end(), b, b + 4);
    };

    write_byte(0);
    write_sz("shortcuts");
    for (size_t idx = 0; idx < list.size(); ++idx) {
        const auto& sc = list[idx];
        write_byte(0);
        write_sz(std::to_string(idx));
        write_int("appid", sc.appid);
        write_str("appname", sc.app_name);
        write_str("exe", sc.exe);
        write_str("StartDir", sc.start_dir);
        write_str("icon", sc.icon);
        write_str("ShortcutPath", sc.shortcut_path);
        write_str("LaunchOptions", sc.launch_options);
        write_int("IsHidden", sc.is_hidden);
        write_int("AllowDesktopConfig", sc.allow_desktop_config);
        write_int("AllowOverlay", sc.allow_overlay);
        write_int("OpenVR", sc.open_vr);
        write_int("Devkit", sc.devkit);
        write_str("DevkitGameID", sc.devkit_game_id);
        write_int("DevkitOverrideAppID", sc.devkit_override_app_id);
        write_int("LastPlayTime", sc.last_play_time);
        write_str("FlatpakAppID", sc.flatpak_app_id);
        write_str("sortas", sc.sortas);
        write_byte(0);
        write_sz("tags");
        for (size_t t = 0; t < sc.tags.size(); ++t) {
            write_byte(1);
            write_sz(std::to_string(t));
            write_sz(sc.tags[t]);
        }
        write_byte(8);
        write_byte(8);
    }
    write_byte(8);
    return out;
}

/// The launcher's name: the packaged executable, the desktop shortcut, Steam's entry and the
/// window all use it. Releases before v0.5.1 shipped CTR-NitroFueled-PC.exe and a desktop
/// shortcut named "CTR Nitro-Fueled".
constexpr const wchar_t* kLauncherName = L"Crash\u2122 Team Racing Nitro-Fueled";
constexpr const wchar_t* kOldLauncherExe = L"CTR-NitroFueled-PC.exe";
constexpr const wchar_t* kOldShortcut = L"CTR Nitro-Fueled.lnk";

/// Where a .lnk points (empty when it cannot be read).
fs::path ShortcutTarget(const fs::path& link) {
    fs::path target;
    IShellLinkW* psl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl)))
        return target;
    IPersistFile* ppf = nullptr;
    if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(ppf->Load(link.c_str(), STGM_READ)) &&
            SUCCEEDED(psl->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)))
            target = path;
        ppf->Release();
    }
    psl->Release();
    return target;
}

/// This executable, whatever it is called.
fs::path LauncherExe() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(std::wstring(path, n));
}

bool CreateDesktopShortcut(std::string& out_error) {
    App& a = g_app;
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        out_error = T("No se pudo obtener la ruta del Escritorio.", "Could not get Desktop path.");
        return false;
    }
    const fs::path target_exe = LauncherExe();
    std::error_code ec;
    // A shortcut of that name may already open something else (another copy of the game, an
    // emulator): never replace it, use the "(PC)" name then.
    const auto ours = [&](const fs::path& link) {
        if (!fs::is_regular_file(link, ec)) return true;
        const fs::path t = ShortcutTarget(link);
        return t.parent_path() == a.root || t == target_exe;
    };
    fs::path link_path = fs::path(desktop) / (std::wstring(kLauncherName) + L".lnk");
    if (!ours(link_path)) link_path = fs::path(desktop) / (std::wstring(kLauncherName) + L" (PC).lnk");
    if (!ours(link_path)) {
        out_error = T("Ya hay accesos directos con ese nombre que abren otra cosa: no se ha cambiado nada.",
                      "Shortcuts with that name already open something else: nothing was changed.");
        return false;
    }

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
    if (FAILED(hr)) {
        out_error = T("Error inicializando IShellLink.", "Error initializing IShellLink.");
        return false;
    }
    psl->SetPath(target_exe.c_str());
    psl->SetArguments(L"--launch");
    psl->SetWorkingDirectory(a.root.c_str());
    psl->SetDescription(kLauncherName);
    psl->SetIconLocation(target_exe.c_str(), 0);  // the icon in the executable

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
    if (SUCCEEDED(hr)) {
        hr = ppf->Save(link_path.c_str(), TRUE);
        ppf->Release();
    }
    psl->Release();
    if (FAILED(hr)) {
        out_error = T("No se pudo guardar el acceso directo en el Escritorio.", "Could not save shortcut to Desktop.");
        return false;
    }
    // The shortcut of releases before v0.5.1, when it opened this folder's launcher.
    const fs::path old_link = fs::path(desktop) / kOldShortcut;
    if (fs::is_regular_file(old_link, ec) && ShortcutTarget(old_link).parent_path() == a.root) fs::remove(old_link, ec);
    return true;
}

/// Releases before v0.5.1 called the launcher CTR-NitroFueled-PC.exe. An update unzipped over
/// one keeps that file and its shortcuts: the old executable becomes a copy of this one, so a
/// Steam entry or any other shortcut naming it opens this launcher, and the desktop shortcut
/// made by the old launcher is made again with the new name.
void MigrateOldLauncher() {
    App& a = g_app;
    std::error_code ec;
    const fs::path self = LauncherExe();
    const fs::path old_exe = a.root / kOldLauncherExe;
    if (self.filename() == kOldLauncherExe) return;
    if (fs::is_regular_file(old_exe, ec) &&
        (fs::file_size(old_exe, ec) != fs::file_size(self, ec) || ReadText(old_exe) != ReadText(self)))
        fs::copy_file(self, old_exe, fs::copy_options::overwrite_existing, ec);
    // The desktop shortcut too when the old executable is already gone (deleted by hand).
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) return;
    const fs::path old_link = fs::path(desktop) / kOldShortcut;
    if (fs::is_regular_file(old_link, ec) && ShortcutTarget(old_link).parent_path() == a.root) {
        std::string error;
        CreateDesktopShortcut(error);
    }
}

[[maybe_unused]] bool AddToSteam(std::string& out_msg) {
    App& a = g_app;
    fs::path steam_path = FindSteamPath();
    if (steam_path.empty()) {
        out_msg = T("No se encontró la instalación de Steam en este equipo.", "Steam installation not found on this computer.");
        return false;
    }
    fs::path userdata = steam_path / L"userdata";
    std::error_code ec;
    if (!fs::is_directory(userdata, ec)) {
        out_msg = T("No se encontró la carpeta userdata de Steam.", "Steam userdata folder not found.");
        return false;
    }

    bool steam_was_running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof pe};
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                    steam_was_running = true;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (steam_was_running) {
        fs::path steam_exe = steam_path / L"steam.exe";
        if (fs::is_regular_file(steam_exe, ec)) {
            RunCaptured(steam_exe.wstring(), Quote(steam_exe.wstring()) + L" -shutdown", steam_path, 4000);
            Sleep(1500);
        }
    }

    fs::path assets_dir;
    for (const auto& candidate : {a.root / L"steamgrid", a.root / L"packaging" / L"windows" / L"steamgrid"}) {
        if (fs::is_directory(candidate, ec)) {
            assets_dir = candidate;
            break;
        }
    }

    const fs::path target_exe = LauncherExe();

    const std::string app_name = Narrow(kLauncherName);
    const std::string exe_str = "\"" + PathText(target_exe) + "\"";
    const std::string start_dir_str = "\"" + PathText(a.root) + "\"";

    fs::path icon_path;
    if (!assets_dir.empty() && fs::is_regular_file(assets_dir / L"icono.jpg", ec)) {
        icon_path = assets_dir / L"icono.jpg";
    } else if (fs::is_regular_file(a.root / L"packaging" / L"windows" / L"icons" / L"ctr.ico", ec)) {
        icon_path = a.root / L"packaging" / L"windows" / L"icons" / L"ctr.ico";
    } else {
        icon_path = target_exe;
    }

    const std::string key = exe_str + app_name;
    const uint32_t crc = Crc32(key.data(), key.size());
    const uint32_t appid = crc | 0x80000000;
    const std::string id32 = std::to_string(appid);
    const std::string id64 = std::to_string(((uint64_t)appid << 32) | 0x02000000);

    int profiles_updated = 0;
    for (const auto& entry : fs::directory_iterator(userdata, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory()) continue;
        const std::string dirname = entry.path().filename().string();
        if (dirname.empty() || !std::all_of(dirname.begin(), dirname.end(), ::isdigit)) continue;

        fs::path config_dir = entry.path() / L"config";
        fs::path grid_dir = config_dir / L"grid";
        fs::create_directories(grid_dir, ec);

        if (!assets_dir.empty()) {
            const std::pair<const wchar_t*, const char*> mapping[] = {
                {L"portada.png", "p.png"},
                {L"fondo.png", "_hero.png"},
                {L"logo.png", "_logo.png"},
                {L"portada_ancha.png", ".png"},
                {L"icono.jpg", "_icon.jpg"}
            };
            for (const auto& [src_name, suffix] : mapping) {
                fs::path src = assets_dir / src_name;
                if (fs::is_regular_file(src, ec)) {
                    fs::copy_file(src, grid_dir / TextPath(id32 + suffix), fs::copy_options::overwrite_existing, ec);
                    fs::copy_file(src, grid_dir / TextPath(id64 + suffix), fs::copy_options::overwrite_existing, ec);
                }
            }
        }

        fs::path vdf_path = config_dir / L"shortcuts.vdf";
        std::vector<SteamShortcut> shortcuts;
        if (fs::is_regular_file(vdf_path, ec)) {
            std::ifstream f(vdf_path, std::ios::binary);
            if (f) {
                std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                shortcuts = ParseShortcuts(buf);
            }
        }

        bool found = false;
        for (auto& sc : shortcuts) {
            if (sc.app_name == app_name || sc.exe.find(PathText(kOldLauncherExe)) != std::string::npos ||
                sc.exe.find(PathText(target_exe.filename())) != std::string::npos) {
                sc.appid = appid;
                sc.app_name = app_name;
                sc.exe = exe_str;
                sc.start_dir = start_dir_str;
                sc.icon = PathText(icon_path);
                sc.launch_options = "--launch";
                found = true;
                break;
            }
        }
        if (!found) {
            SteamShortcut sc;
            sc.appid = appid;
            sc.app_name = app_name;
            sc.exe = exe_str;
            sc.start_dir = start_dir_str;
            sc.icon = PathText(icon_path);
            sc.launch_options = "--launch";
            shortcuts.push_back(sc);
        }

        auto serialized = SerializeShortcuts(shortcuts);
        std::ofstream out(vdf_path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(serialized.data()), serialized.size());
            profiles_updated++;
        }
    }

    if (profiles_updated > 0) {
        if (steam_was_running) {
            out_msg = T("¡Añadido a Steam con portadas con éxito!\nSteam se cerró automáticamente para aplicar los cambios; vuelve a abrirlo para ver el juego en tu biblioteca.",
                        "Successfully added to Steam with custom artwork!\nSteam was closed automatically to apply changes; reopen it to see the game in your library.");
        } else {
            out_msg = T("¡Añadido a Steam con portadas con éxito!\nAbre Steam para ver el juego en tu biblioteca.",
                        "Successfully added to Steam with custom artwork!\nOpen Steam to see the game in your library.");
        }
        return true;
    } else {
        out_msg = T("No se encontró ningún perfil de usuario en Steam.", "No user profiles found in Steam.");
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Updates (port_update.cpp): the latest published release's Windows ZIP, downloaded into
// update\, checked (size, SHA-256), unpacked with the bundled Python and copied over the port.
// Settings, the online key, friends, saves, backups, DLC, logs and the player's own patches are
// never in the package and never written; replaced files go to update\rollback\ first.

struct UpdateState {
    // Dismissed: a newer release the player chose not to install now (Tools offers it again).
    enum Stage { Idle, Checking, UpToDate, Available, Dismissed, Downloading, Unpacking, Installing, Done, Failed };
    std::mutex mutex;
    std::atomic<int> stage{Idle};
    PortUpdate::Release release;  // under mutex
    std::string error;            // under mutex
    PortUpdate::Progress progress;
    std::thread worker;
    std::atomic<bool> auto_install{false};  // install as soon as the check finds a newer release
    bool restart_sent = false;
};
UpdateState g_update;

bool UpdateBusy() {
    const int s = g_update.stage;
    return s == UpdateState::Checking || s == UpdateState::Downloading || s == UpdateState::Unpacking ||
           s == UpdateState::Installing || s == UpdateState::Done;
}

fs::path UpdateDir() { return g_app.root / L"update"; }

void UpdateFail(const std::string& error) {
    {
        std::scoped_lock lock{g_update.mutex};
        g_update.error = error;
    }
    g_update.stage = UpdateState::Failed;
}

void StartUpdateInstall();

/// Asks GitHub for the latest release; with auto_install a newer one is installed at once.
void StartUpdateCheck(bool auto_install) {
    if (UpdateBusy()) return;
    if (g_update.worker.joinable()) g_update.worker.join();
    g_update.stage = UpdateState::Checking;
    g_update.auto_install = auto_install;
    const std::string current = g_app.version;
    g_update.worker = std::thread([current, auto_install] {
        std::string error;
        const std::string url = std::string("https://api.github.com/repos/") + PortUpdate::kRepo + "/releases/latest";
        const auto body = PortUpdate::FetchText(Widen(url), &error);
        if (!body) {
            UpdateFail(std::string(T("No se pudo consultar GitHub: ", "Could not ask GitHub: ")) + error);
            return;
        }
        const auto release = PortUpdate::ParseRelease(*body);
        if (!release) {
            UpdateFail(T("GitHub no devolvió una versión para Windows.", "GitHub returned no Windows release."));
            return;
        }
        {
            std::scoped_lock lock{g_update.mutex};
            g_update.release = *release;
            g_update.error.clear();
        }
        const auto have = PortUpdate::ParseVersion(current);
        const auto latest = PortUpdate::ParseVersion(release->tag);
        if (!have.valid || !PortUpdate::IsNewer(have, latest)) {
            g_update.stage = UpdateState::UpToDate;
            return;
        }
        g_update.stage = UpdateState::Available;  // PollUpdate starts an automatic install
    });
}

void StartUpdateInstall() {
    App& a = g_app;
    if (g_update.stage != UpdateState::Available && g_update.stage != UpdateState::Dismissed &&
        g_update.stage != UpdateState::Failed)
        return;
    if (a.game_running) {
        a.notice = T("Cierra el juego para actualizar.", "Close the game to update.");
        return;
    }
    if (!a.python) {
        UpdateFail(T("Falta Python para descomprimir la actualización.", "Python is missing to unpack the update."));
        return;
    }
    PortUpdate::Release release;
    {
        std::scoped_lock lock{g_update.mutex};
        release = g_update.release;
        g_update.error.clear();
    }
    if (release.asset_url.empty()) return;
    if (g_update.worker.joinable()) g_update.worker.join();
    g_update.progress.cancel = false;
    g_update.stage = UpdateState::Downloading;
    const fs::path root = a.root;
    const Python python = *a.python;
    g_update.worker = std::thread([release, root, python] {
        const fs::path dir = root / L"update";
        const fs::path zip = dir / Widen(release.asset_name);
        const fs::path staging = dir / L"staging";
        std::error_code ec;
        fs::remove_all(staging, ec);
        std::string error = PortUpdate::Download(Widen(release.asset_url), zip, release.size, release.sha256,
                                                 g_update.progress);
        if (!error.empty()) {
            UpdateFail(std::string(T("La descarga falló: ", "The download failed: ")) + error);
            return;
        }
        g_update.stage = UpdateState::Unpacking;
        const std::wstring cmd = Quote(python.exe) + L" " + python.args + L"-I -X utf8 -c " +
                                 Quote(L"import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])") +
                                 L" " + Quote(zip.wstring()) + L" " + Quote(staging.wstring());
        const Captured r = RunCaptured(python.exe, cmd, dir, 600000);
        const fs::path package = staging / PortUpdate::kPackageFolder;
        const std::string unpacked_version = Trim(ReadText(package / L"bin" / L"VERSION"));
        if (!r.started || r.code != 0 || !fs::is_regular_file(package / L"bin" / L"sacredfire.exe", ec) ||
            !PortUpdate::ParseVersion(unpacked_version).valid) {
            fs::remove_all(staging, ec);
            UpdateFail(std::string(T("No se pudo descomprimir la actualización. ", "Could not unpack the update. ")) +
                       Trim(r.output).substr(0, 300));
            return;
        }
        if (g_app.game_running) {
            UpdateFail(T("Cierra el juego para actualizar.", "Close the game to update."));
            return;
        }
        g_update.stage = UpdateState::Installing;
        error = PortUpdate::Install(package, root, dir / L"rollback");
        fs::remove_all(staging, ec);
        if (!error.empty()) {
            UpdateFail(std::string(T("No se pudo instalar (todo quedó como estaba): ",
                                     "Could not install (everything was put back): ")) + error);
            return;
        }
        fs::remove(zip, ec);
        g_update.stage = UpdateState::Done;
    });
}

/// Every frame: an automatic update starts once the check found one; a finished install
/// restarts the launcher in its new version.
void PollUpdate() {
    App& a = g_app;
    if (g_update.auto_install && g_update.stage != UpdateState::Checking) {
        g_update.auto_install = false;
        if (g_update.stage == UpdateState::Available && !a.game_running) StartUpdateInstall();
    }
    if (g_update.stage == UpdateState::Done && !g_update.restart_sent) {
        g_update.restart_sent = true;
        SaveLauncher();
        if (!RestartLauncher(L"--updated"))
            a.notice = T("Actualizado. Cierra y vuelve a abrir el launcher.", "Updated. Close and open the launcher again.");
    }
}

/// At start: the previous update's leftovers (the old launcher in rollback\ may still be running
/// for a moment, so this retries in the background).
void CleanupUpdate() {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::thread([dir] {
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::error_code e;
            fs::remove_all(dir / L"rollback", e);
            fs::remove_all(dir / L"staging", e);
            if (!fs::exists(dir / L"rollback", e) && !fs::exists(dir / L"staging", e)) break;
            Sleep(500);
        }
        std::error_code e;
        fs::remove(dir, e);  // only when empty (a download in progress keeps it)
    }).detach();
}

/// The update line under the header: a newer version, the progress, or what went wrong.
void UpdateBanner() {
    const int stage = g_update.stage;
    if (stage == UpdateState::Idle || stage == UpdateState::Checking || stage == UpdateState::UpToDate ||
        stage == UpdateState::Dismissed)
        return;
    PortUpdate::Release release;
    std::string error;
    {
        std::scoped_lock lock{g_update.mutex};
        release = g_update.release;
        error = g_update.error;
    }
    if (stage == UpdateState::Available) {
        ImGui::TextColored(kGreen, "%s", Format(T("Hay una versión nueva: %s (tienes %s).", "A new version is out: %s (you have %s)."),
                                                release.tag, g_app.version).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(g_app.game_running);
        if (ImGui::SmallButton(T("Actualizar ahora", "Update now"))) StartUpdateInstall();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Novedades", "What's new"))) OpenInShell(Widen(release.page_url));
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Ahora no", "Not now"))) g_update.stage = UpdateState::Dismissed;
    } else if (stage == UpdateState::Downloading) {
        const double total = double(std::max<std::uint64_t>(1, g_update.progress.total));
        const float fraction = float(double(g_update.progress.done) / total);
        ImGui::TextColored(kGrey, "%s", Format(T("Descargando %s...", "Downloading %s..."), release.tag).c_str());
        ImGui::SameLine();
        char text[64];
        std::snprintf(text, sizeof text, "%.0f / %.0f MB", double(g_update.progress.done) / 1e6, total / 1e6);
        ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), text);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Cancelar", "Cancel"))) g_update.progress.cancel = true;
    } else if (stage == UpdateState::Unpacking || stage == UpdateState::Installing) {
        ImGui::TextColored(kGrey, "%s", stage == UpdateState::Unpacking ? T("Descomprimiendo la actualización...", "Unpacking the update...")
                                                                       : T("Instalando la actualización...", "Installing the update..."));
    } else if (stage == UpdateState::Done) {
        ImGui::TextColored(kGreen, "%s", T("Actualizado: reiniciando el launcher...", "Updated: restarting the launcher..."));
    } else if (stage == UpdateState::Failed) {
        ImGui::TextColored(kYellow, "%s", error.c_str());
        ImGui::SameLine();
        if (!release.asset_url.empty() && ImGui::SmallButton(T("Reintentar", "Retry"))) StartUpdateInstall();
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) g_update.stage = UpdateState::Idle;
    }
}

void UpdateSection() {
    App& a = g_app;
    ImGui::SeparatorText(T("Actualizaciones", "Updates"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s",
                       T("Descarga la última versión publicada en GitHub y la instala encima. Tus ajustes, amigos, apodo, "
                         "parches propios, partidas, copias, DLC y registros se mantienen: la actualización solo cambia "
                         "los archivos del port. Si algo falla, todo vuelve a como estaba.",
                         "Downloads the latest release published on GitHub and installs it over this one. Your settings, "
                         "friends, nickname, own patches, saves, backups, DLC and logs are kept: the update only changes "
                         "the port's files. If anything fails, everything is put back."));
    ImGui::PopTextWrapPos();
    ImGui::Text("%s %s", T("Versión instalada:", "Installed version:"), a.version.c_str());
    const int stage = g_update.stage;
    ImGui::BeginDisabled(UpdateBusy());
    if (ImGui::Button(T("Comprobar actualizaciones", "Check for updates"))) StartUpdateCheck(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (stage == UpdateState::Available || stage == UpdateState::Dismissed) {
        PortUpdate::Release release;
        {
            std::scoped_lock lock{g_update.mutex};
            release = g_update.release;
        }
        ImGui::TextColored(kGreen, "%s", Format(T("Versión nueva: %s", "New version: %s"), release.tag).c_str());
        ImGui::TextUnformatted(T("¿Quieres actualizar ahora?", "Do you want to update now?"));
        ImGui::BeginDisabled(a.game_running);
        if (ImGui::Button(T("Actualizar ahora", "Update now"))) StartUpdateInstall();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (stage == UpdateState::Available && ImGui::Button(T("Ahora no", "Not now")))
            g_update.stage = UpdateState::Dismissed;
        ImGui::SameLine();
        if (ImGui::Button(T("Novedades", "What's new"))) OpenInShell(Widen(release.page_url));
        if (a.game_running)
            ImGui::TextColored(kGrey, "%s", T("Cierra el juego para actualizar.", "Close the game to update."));
    } else if (stage == UpdateState::Checking) {
        ImGui::TextColored(kGrey, "%s", T("Consultando GitHub...", "Asking GitHub..."));
    } else if (stage == UpdateState::UpToDate) {
        std::string tag;
        {
            std::scoped_lock lock{g_update.mutex};
            tag = g_update.release.tag;
        }
        ImGui::TextColored(kGreen, "%s", Format(T("Estás al día (última versión: %s).", "You are up to date (latest: %s)."), tag).c_str());
    } else if (stage != UpdateState::Idle) {
        ImGui::TextColored(kGrey, "%s", T("Mira el aviso de arriba.", "See the notice at the top."));
    }
    LauncherFlag("update_check", T("Buscar actualizaciones al abrir el launcher", "Check for updates when the launcher opens"), false,
                 T("Una consulta pequeña a GitHub al abrir. No descarga nada sin tu permiso, salvo que actives la opción de abajo.",
                   "A small request to GitHub at start. Nothing is downloaded without your say, unless the option below is on."));
    LauncherFlag("update_auto", T("Instalar las actualizaciones automáticamente", "Install updates automatically"), false,
                 T("Al abrir el launcher, si hay una versión nueva se descarga e instala sola y el launcher se reinicia. "
                   "Nunca mientras juegas. Si abres el juego directo (acceso directo o Steam), no se actualiza.",
                   "When the launcher opens and a new version is out, it is downloaded and installed and the launcher "
                   "restarts. Never while you play. Starting straight into the game (shortcut, Steam) does not update."));
}

// ---------------------------------------------------------------------------------------------
// Achievements tab (v0.4): the game's trophies (sce_sys/trophy/trophy00.trp), with the ones
// unlocked while playing (<user>/trophies/<TITLE_ID>/unlocked.txt, written by the runtime:
// src/runtime_services.c). Icons are plain PNGs in the TRP. Names, descriptions and grades are
// in its ESFM files, AES-encrypted with the console's trophy key, which is not part of this
// port: with the player's own key (the one shadPS4 uses: entered here or imported from
// shadPS4's keys.json) they are decrypted as shadPS4 does (src/core/file_format/trp.cpp) and
// written to trophies.tsv, which the runtime reads for its in-game notice and the platinum.

ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height);  // after g_device

// The 48 trophies' names, grades and what to do (src/ctr_trophies.h, shared with the runtime).
#include "../../src/ctr_trophies.h"
#define kCtrTrophies ctr_trophies


struct Trophy {
    int id = 0;
    char grade = 0;  // 'P' 'G' 'S' 'B', 0 unknown
    bool hidden = false;
    std::string name, detail;
    ID3D11ShaderResourceView* icon = nullptr;
    uint64_t unlocked = 0;  // unix time, 0 locked
};

struct TrophySet {
    fs::path game;              // the game folder it was read from
    std::string language;       // the game language its names are in
    std::string title_id, np_comm_id, title_name;
    std::vector<Trophy> list;
    ID3D11ShaderResourceView* icon = nullptr;
    bool names = false;         // names and grades known
    std::string error, note;
    fs::file_time_type unlocked_time{};
    double next_poll = 0.0;
};
TrophySet g_trophies;
bool g_trophies_reload = true;
int g_trophy_filter = 0;  // 0 all, 1 unlocked, 2 locked
bool g_trophy_reset_confirm = false;
char g_trophy_key_input[64] = "";

fs::path TrophyDir(const std::string& title) { return g_app.root / L"user" / L"trophies" / Widen(title); }

void ReleaseTrophies(TrophySet& t) {
    for (auto& tr : t.list)
        if (tr.icon) tr.icon->Release();
    if (t.icon) t.icon->Release();
    t = TrophySet{};
}

/// A PNG to a texture, through WIC.
ID3D11ShaderResourceView* PngTexture(const std::string& png) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), DWORD(png.size()))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        std::vector<uint32_t> pixels(size_t(w) * h);
        if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
            view = MakeTexture(pixels.data(), int(w), int(h));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    wic->Release();
    return view;
}

/// The 16-byte trophy key from hex ("" when it is not 32 hex digits).
std::string TrophyKeyBytes(const std::string& hex) {
    std::string clean;
    for (char c : hex)
        if (std::isxdigit(static_cast<unsigned char>(c))) clean += c;
    if (clean.size() != 32) return {};
    std::string out;
    for (size_t i = 0; i < 32; i += 2) out += char(std::stoi(clean.substr(i, 2), nullptr, 16));
    return out;
}

/// AES-128-CBC without padding (Windows CNG). encrypt: true to encrypt.
bool AesCbc(const std::string& key, const unsigned char iv_in[16], const unsigned char* in, size_t size, unsigned char* out,
            bool encrypt) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = false;
    unsigned char iv[16];
    std::memcpy(iv, iv_in, 16);
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                         sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0))) {
        ULONG done = 0;
        ok = BCRYPT_SUCCESS(encrypt ? BCryptEncrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)
                                    : BCryptDecrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)) &&
             done == size;
    }
    if (handle) BCryptDestroyKey(handle);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/// One ESFM entry decrypted to its XML (shadPS4's DecryptEFSM): the key for the file is the
/// NP communication ID (16 bytes, zero-padded) encrypted with the trophy key (CBC, zero IV); the
/// entry is IV (16 bytes) + data, CBC-decrypted with that key and IV.
std::string DecryptEsfm(const std::string& trophy_key, const std::string& np_comm_id, const std::string& entry) {
    if (entry.size() <= 32 || (entry.size() - 16) % 16) return {};
    unsigned char comm[16]{}, zero[16]{}, file_key[16];
    std::memcpy(comm, np_comm_id.data(), std::min<size_t>(np_comm_id.size(), 16));
    if (!AesCbc(trophy_key, zero, comm, 16, file_key, true)) return {};
    std::string xml(entry.size() - 16, '\0');
    if (!AesCbc(std::string(reinterpret_cast<char*>(file_key), 16), reinterpret_cast<const unsigned char*>(entry.data()),
                reinterpret_cast<const unsigned char*>(entry.data()) + 16, entry.size() - 16,
                reinterpret_cast<unsigned char*>(xml.data()), false))
        return {};
    const size_t end = xml.rfind('>');
    if (end == std::string::npos) return {};
    xml.resize(end + 1);
    return xml.rfind("<?xml", 0) == 0 || xml.find("<trophyconf") != std::string::npos ? xml : std::string{};
}

std::string XmlTag(const std::string& xml, const std::string& tag, size_t from = 0, size_t until = std::string::npos) {
    const size_t a = xml.find("<" + tag + ">", from);
    if (a == std::string::npos || a >= until) return {};
    const size_t b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos || b > until) return {};
    return XmlUnescape(xml.substr(a + tag.size() + 2, b - a - tag.size() - 2));
}

/// The trophies in a decrypted TROP*.XML: id -> (grade, hidden, name, detail).
void ParseTrophyXml(const std::string& xml, TrophySet& t) {
    t.title_name = XmlTag(xml, "title-name");
    for (size_t at = 0; (at = xml.find("<trophy ", at)) != std::string::npos;) {
        const size_t close = xml.find('>', at), end = xml.find("</trophy>", at);
        if (close == std::string::npos || end == std::string::npos) break;
        const auto attr = XmlAttributes(xml.substr(at + 8, close - at - 8));
        auto get = [&](const char* k) {
            const auto it = attr.find(k);
            return it == attr.end() ? std::string() : it->second;
        };
        const int id = std::atoi(get("id").c_str());
        for (auto& tr : t.list) {
            if (tr.id != id) continue;
            const std::string type = get("ttype");
            tr.grade = type.empty() ? 0 : type[0];
            tr.hidden = get("hidden") == "yes";
            tr.name = XmlTag(xml, "name", close, end);
            tr.detail = XmlTag(xml, "detail", close, end);
        }
        at = end;
    }
}

std::string Sanitize(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

/// trophies.tsv: "#lang<TAB>code" then id, grade, hidden, name, detail per line.
bool ReadTrophyTsv(const fs::path& file, const std::string& language, TrophySet& t) {
    std::stringstream ss(ReadText(file));
    std::string line;
    bool right_language = false, any = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string field;
        while (std::getline(ls, field, '\t')) f.push_back(field);
        if (f.size() >= 2 && f[0] == "#lang") right_language = f[1] == language;
        if (f.size() >= 2 && f[0] == "#title") t.title_name = f[1];
        if (f.size() < 5 || f[0].empty() || !std::isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        for (auto& tr : t.list)
            if (tr.id == std::atoi(f[0].c_str())) {
                tr.grade = f[1].empty() ? 0 : f[1][0];
                tr.hidden = f[2] == "1";
                tr.name = f[3];
                tr.detail = f[4];
                any = true;
            }
    }
    return any && right_language;
}

void WriteTrophyTsv(const TrophySet& t) {
    std::string out = "#lang\t" + t.language + "\n#title\t" + Sanitize(t.title_name) + "\n";
    for (const auto& tr : t.list)
        out += std::to_string(tr.id) + "\t" + std::string(1, tr.grade ? tr.grade : '?') + "\t" + (tr.hidden ? "1" : "0") + "\t" +
               Sanitize(tr.name) + "\t" + Sanitize(tr.detail) + "\n";
    std::error_code ec;
    fs::create_directories(TrophyDir(t.title_id), ec);
    WriteTextAtomic(TrophyDir(t.title_id) / L"trophies.tsv", out);
}

void ReadUnlocked(TrophySet& t) {
    for (auto& tr : t.list) tr.unlocked = 0;
    std::stringstream ss(ReadText(TrophyDir(t.title_id) / L"unlocked.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        int id = -1;
        unsigned long long at = 0;
        if (std::sscanf(line.c_str(), "%d %llu", &id, &at) != 2) continue;
        for (auto& tr : t.list)
            if (tr.id == id && !tr.unlocked) tr.unlocked = at ? at : 1;
    }
    std::error_code ec;
    t.unlocked_time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
}

/// The trophy key the launcher keeps, else shadPS4's (keys.json "ReleaseTrophyKey").
std::string ShadPs4TrophyKey() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (!appdata) return {};
    const std::string json = ReadText(fs::path(appdata) / L"shadPS4" / L"keys.json");
    const size_t at = json.find("\"ReleaseTrophyKey\"");
    if (at == std::string::npos) return {};
    const size_t colon = json.find(':', at);
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == '"') {  // a hex string
        const size_t end = json.find('"', i + 1);
        return end == std::string::npos ? std::string() : json.substr(i + 1, end - i - 1);
    }
    if (i < json.size() && json[i] == '[') {  // a byte array
        const size_t end = json.find(']', i);
        std::stringstream ss(json.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
        std::string hex, n;
        char buf[3];
        while (std::getline(ss, n, ',')) {
            n = Trim(n);
            if (n.empty()) continue;
            std::snprintf(buf, sizeof buf, "%02x", std::atoi(n.c_str()) & 0xff);
            hex += buf;
        }
        return hex;
    }
    return {};
}

void LoadTrophies() {
    App& a = g_app;
    ReleaseTrophies(g_trophies);
    TrophySet& t = g_trophies;
    t.game = a.game_dir;
    t.language = GameLanguage();
    t.title_id = Narrow(GameTitle());
    const fs::path trp = a.game_dir / L"sce_sys" / L"trophy" / L"trophy00.trp";
    const std::string data = ReadText(trp);
    auto be32 = [&](size_t o) {
        return uint32_t(uint8_t(data[o])) << 24 | uint32_t(uint8_t(data[o + 1])) << 16 | uint32_t(uint8_t(data[o + 2])) << 8 |
               uint32_t(uint8_t(data[o + 3]));
    };
    auto be64 = [&](size_t o) { return uint64_t(be32(o)) << 32 | be32(o + 4); };
    if (data.size() < 0x60 || be32(0) != 0xDCA24D00) {
        t.error = a.game_dir.empty() ? T("Elige primero el juego en la pestaña Jugar.", "Choose the game in the Play tab first.")
                                     : T("Este juego no trae el archivo de trofeos (sce_sys\\trophy\\trophy00.trp).",
                                         "This game has no trophy file (sce_sys\\trophy\\trophy00.trp).");
        return;
    }
    const uint32_t count = be32(0x10), entry_size = be32(0x14);
    std::map<std::string, std::string> entries;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const size_t e = 0x60 + size_t(i) * entry_size;
        if (e + 48 > data.size()) break;
        const std::string name(data.c_str() + e, strnlen(data.c_str() + e, 32));
        const uint64_t pos = be64(e + 32), size = be64(e + 40);
        if (pos + size <= data.size()) entries[name] = data.substr(size_t(pos), size_t(size));
    }
    for (const auto& [name, bytes] : entries) {
        int id = -1;
        if (name.size() == 11 && name.rfind("TROP", 0) == 0 && name.substr(7) == ".PNG" && std::sscanf(name.c_str() + 4, "%3d", &id) == 1) {
            Trophy tr;
            tr.id = id;
            tr.icon = PngTexture(bytes);
            t.list.push_back(std::move(tr));
        }
    }
    std::sort(t.list.begin(), t.list.end(), [](const Trophy& x, const Trophy& y) { return x.id < y.id; });
    if (entries.count("ICON0.PNG")) t.icon = PngTexture(entries["ICON0.PNG"]);
    // The NP communication ID (npbind.dat), needed to decrypt.
    const std::string npbind = ReadText(a.game_dir / L"sce_sys" / L"npbind.dat");
    for (size_t i = 0; i + 12 <= npbind.size(); ++i)
        if (npbind.compare(i, 4, "NPWR") == 0 && npbind[i + 9] == '_') {
            t.np_comm_id = npbind.substr(i, 12);
            break;
        }
    // Names: trophies.tsv in this language, else decrypt with the key.
    if (ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, t)) {
        t.names = true;
    } else {
        const std::string key = TrophyKeyBytes(a.launcher.Get("trophy_key"));
        if (!key.empty() && !t.np_comm_id.empty()) {
            char localized[32];
            std::snprintf(localized, sizeof localized, "TROP_%02d.ESFM", std::atoi(t.language.c_str()));
            const std::string file = entries.count(localized) ? localized : "TROP.ESFM";
            const std::string xml = entries.count(file) ? DecryptEsfm(key, t.np_comm_id, entries[file]) : std::string{};
            if (!xml.empty()) {
                ParseTrophyXml(xml, t);
                t.names = true;
                WriteTrophyTsv(t);
            } else {
                t.note = T("La clave de trofeos no descifra este juego: revisa que sea la de tu PS4 (32 cifras hexadecimales).",
                           "The trophy key does not decrypt this game: check it is your PS4's (32 hex digits).");
            }
        }
    }
    ReadUnlocked(t);
}

void TrophiesTab() {
    App& a = g_app;
    const std::string language = GameLanguage();
    if (g_trophies_reload || g_trophies.game != a.game_dir || g_trophies.language != language) {
        g_trophies_reload = false;
        LoadTrophies();
    }
    TrophySet& t = g_trophies;
    // The game writes unlocked.txt while it runs: re-read it when it changes.
    if (ImGui::GetTime() > t.next_poll && !t.title_id.empty()) {
        t.next_poll = ImGui::GetTime() + 1.0;
        std::error_code ec;
        const auto time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
        if (!ec && time != t.unlocked_time) ReadUnlocked(t);
    }
    if (!t.error.empty()) {
        BeginCard("trophy_error");
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO, t.error.c_str());
        EndCard();
        return;
    }
    auto points = [](char g) { return g == 'P' ? 180 : g == 'G' ? 90 : g == 'S' ? 30 : g == 'B' ? 15 : 0; };
    int unlocked = 0, earned = 0, total_points = 0;
    int by_grade[4][2]{};  // B S G P: unlocked, total
    for (const auto& tr : t.list) {
        unlocked += tr.unlocked != 0;
        total_points += points(tr.grade);
        if (tr.unlocked) earned += points(tr.grade);
        const int g = tr.grade == 'B' ? 0 : tr.grade == 'S' ? 1 : tr.grade == 'G' ? 2 : tr.grade == 'P' ? 3 : -1;
        if (g >= 0) {
            by_grade[g][1]++;
            if (tr.unlocked) by_grade[g][0]++;
        }
    }
    const int total = int(t.list.size());
    const float progress = total_points ? float(earned) / float(total_points) : total ? float(unlocked) / float(total) : 0.0f;
    static const ImVec4 grade_colors[] = {ImVec4(0.82f, 0.54f, 0.32f, 1.0f), ImVec4(0.80f, 0.83f, 0.88f, 1.0f),
                                          ImVec4(1.0f, 0.80f, 0.29f, 1.0f), ImVec4(0.55f, 0.83f, 1.0f, 1.0f)};
    auto grade_index = [](char g) { return g == 'B' ? 0 : g == 'S' ? 1 : g == 'G' ? 2 : g == 'P' ? 3 : -1; };
    auto grade_name = [](int g) {
        static const char* es[] = {"Bronce", "Plata", "Oro", "Platino"};
        static const char* en[] = {"Bronze", "Silver", "Gold", "Platinum"};
        return g_spanish ? es[g] : en[g];
    };

    // Summary.
    BeginCard("trophy_summary");
    const float big = 72.0f * g_ui_scale;
    if (t.icon) {
        ImGui::Image(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(t.icon))), ImVec2(big * 1.78f, big));
        ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
    }
    ImGui::BeginGroup();
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(t.title_name.empty() ? "Crash Team Racing Nitro-Fueled" : t.title_name.c_str());
    if (g_font_heading) ImGui::PopFont();
    ImGui::Text(T("%d de %d logros  ·  %d %%", "%d of %d trophies  ·  %d %%"), unlocked, total, int(progress * 100.0f + 0.5f));
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 8.0f * g_ui_scale), "");
    if (t.names) {
        for (int g = 3; g >= 0; --g) {
            if (!by_grade[g][1]) continue;
            ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F");
            ImGui::SameLine(0.0f, 4.0f * g_ui_scale);
            ImGui::Text("%s %d/%d", grade_name(g), by_grade[g][0], by_grade[g][1]);
            ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
        }
        ImGui::NewLine();
    }
    ImGui::EndGroup();
    EndCard();

    // Names: the player's trophy key.
    if (!t.names) {
        ImGui::Spacing();
        BeginCard("trophy_key");
        ImGui::TextColored(kBlue, "%s", ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("Los logros se guardan igual al jugar y aquí se ven con su imagen. Sus nombres y descripciones "
                                 "vienen cifrados en el juego con la clave de trofeos de la PS4, que no se incluye: si la tienes "
                                 "(es la misma que usa shadPS4), pégala o impórtala para verlos.",
                                 "Trophies are recorded while you play and shown here with their pictures. Their names and "
                                 "descriptions are encrypted in the game with the PS4's trophy key, which is not included: if "
                                 "you have it (the same shadPS4 uses), paste or import it to see them."));
        ImGui::PopTextWrapPos();
        if (!t.note.empty()) ImGui::TextColored(kRed, "%s", t.note.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputTextWithHint("##trophykey", T("Clave de trofeos (32 cifras hex)", "Trophy key (32 hex digits)"), g_trophy_key_input,
                                 sizeof g_trophy_key_input, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar", "Save"))) {
            if (TrophyKeyBytes(g_trophy_key_input).empty()) {
                a.notice = T("La clave tiene que tener 32 cifras hexadecimales.", "The key must be 32 hex digits.");
            } else {
                a.launcher.Set("trophy_key", g_trophy_key_input);
                SaveLauncher();
                g_trophies_reload = true;
            }
            std::memset(g_trophy_key_input, 0, sizeof g_trophy_key_input);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Importar de shadPS4", "Import from shadPS4"))) {
            const std::string key = ShadPs4TrophyKey();
            if (TrophyKeyBytes(key).empty()) {
                a.notice = T("shadPS4 no tiene una clave de trofeos guardada (keys.json).",
                             "shadPS4 has no trophy key saved (keys.json).");
            } else {
                a.launcher.Set("trophy_key", key);
                SaveLauncher();
                g_trophies_reload = true;
            }
        }
        EndCard();
    }

    // Filter and actions.
    ImGui::Spacing();
    ImGui::RadioButton(T("Todos", "All"), &g_trophy_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Desbloqueados", "Unlocked"), &g_trophy_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Por conseguir", "Locked"), &g_trophy_filter, 2);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f);
    if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Carpeta", "Folder")).c_str())) {
        std::error_code ec;
        fs::create_directories(TrophyDir(t.title_id), ec);
        OpenInShell(TrophyDir(t.title_id));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!unlocked || a.game_running);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Reiniciar", "Reset")).c_str())) g_trophy_reset_confirm = true;
    ImGui::EndDisabled();

    // The list.
    ImGui::Spacing();
    const float icon = 60.0f * g_ui_scale;
    if (ImGui::BeginTable("trophies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, icon);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
        // The latest unlocked first, then the rest in the game's order.
        std::vector<const Trophy*> order;
        for (const auto& tr : t.list) order.push_back(&tr);
        std::stable_sort(order.begin(), order.end(), [](const Trophy* x, const Trophy* y) {
            if (bool(x->unlocked) != bool(y->unlocked)) return bool(x->unlocked);
            return x->unlocked > y->unlocked;
        });
        for (const Trophy* item : order) {
            const Trophy& tr = *item;
            if ((g_trophy_filter == 1 && !tr.unlocked) || (g_trophy_filter == 2 && tr.unlocked)) continue;
            const bool secret = tr.hidden && !tr.unlocked;
            ImGui::TableNextRow(0, icon + 8.0f * g_ui_scale);
            ImGui::TableNextColumn();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tr.icon && !secret) {
                dl->AddImageRounded(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(tr.icon))), p, ImVec2(p.x + icon, p.y + icon),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    tr.unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 120, 255), 8.0f * g_ui_scale);
            } else {
                dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), Col(kFrame), 8.0f * g_ui_scale);
                const ImVec2 q = ImGui::CalcTextSize("?");
                dl->AddText(ImVec2(p.x + (icon - q.x) * 0.5f, p.y + (icon - q.y) * 0.5f), Col(kGrey), "?");
            }
            if (!tr.unlocked) dl->AddRect(p, ImVec2(p.x + icon, p.y + icon), Col(kBorder), 8.0f * g_ui_scale);
            ImGui::Dummy(ImVec2(icon, icon));
            ImGui::TableNextColumn();
            if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
            std::string name = secret ? T("Logro oculto", "Hidden trophy")
                               : tr.name.empty() ? std::string(T("Logro #", "Trophy #")) + std::to_string(tr.id)
                                                 : tr.name;
            ImGui::TextColored(tr.unlocked ? kText : kGrey, "%s", name.c_str());
            if (g_font_heading) ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            if (secret) ImGui::TextColored(kGrey, "%s", T("Sigue jugando para descubrirlo.", "Keep playing to find it."));
            else if (!tr.detail.empty()) ImGui::TextColored(kGrey, "%s", tr.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            const int g = grade_index(tr.grade);
            if (g >= 0) {
                ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F %s", grade_name(g));
            }
            if (tr.unlocked > 1) {
                const std::time_t when = std::time_t(tr.unlocked);
                std::tm local{};
                localtime_s(&local, &when);
                char date[32];
                std::strftime(date, sizeof date, "%d/%m/%Y %H:%M", &local);
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, date);
            } else if (tr.unlocked) {
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, T("Conseguido", "Unlocked"));
            } else {
                ImGui::TextColored(kGrey, "%s", T("Por conseguir", "Locked"));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kGrey, "%s", T("Se desbloquean jugando, como en la PS4; el platino llega solo al tener todos los demás.",
                                      "They unlock as you play, as on a PS4; the platinum comes once all the others are."));

    if (g_trophy_reset_confirm) {
        ImGui::OpenPopup("trophy_reset");
        g_trophy_reset_confirm = false;
    }
    if (ImGui::BeginPopupModal("trophy_reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Volver a bloquear todos los logros? Se guarda una copia por si acaso.",
                                 "Lock every trophy again? A copy is kept just in case."));
        if (ImGui::Button(T("Reiniciar", "Reset"))) {
            std::error_code ec;
            const fs::path file = TrophyDir(t.title_id) / L"unlocked.txt";
            fs::path backup = file;
            backup += L"." + std::to_wstring(std::time(nullptr)) + L".bak";
            fs::rename(file, backup, ec);
            if (ec) a.notice = std::string(T("No se pudo reiniciar: ", "Could not reset: ")) + ec.message();
            ReadUnlocked(t);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/// Restarts the launcher (after an update or bringing another installation's settings): the
/// packaged name when it is next to the running one. False when it could not be started.
bool RestartLauncher(const std::wstring& args) {
    App& a = g_app;
    wchar_t self[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    fs::path exe = self;
    std::error_code ec;
    const fs::path packaged = a.root / (std::wstring(kLauncherName) + L".exe");
    if (fs::is_regular_file(packaged, ec)) exe = packaged;
    std::wstring cmd = Quote(exe.wstring()) + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, a.root.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostQuitMessage(0);
    return true;
}

/// Asked on the first start of a new folder when another installation is found (and from the
/// Tools tab): copies its settings and data here (settings_import.h), then restarts.
void ImportPopup() {
    App& a = g_app;
    if (g_import.ask && !a.game_running) {
        ImGui::OpenPopup("import_settings");
        g_import.ask = false;
    }
    if (!ImGui::BeginPopupModal("import_settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    const ImportState state = g_import.state;
    ImGui::TextUnformatted(T("¿Traer tus ajustes de otra instalación?", "Bring your settings from another installation?"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextColored(kGrey, "%s", T("Se copian a esta carpeta tus ajustes, controles, apodo, carpeta del juego, partidas "
                                      "guardadas, logros y DLC. La otra carpeta no se toca.",
                                      "Your settings, controls, nickname, game folder, saves, trophies and DLC are copied into "
                                      "this folder. The other folder is left as it is."));
    ImGui::PopTextWrapPos();
    if (g_import.choice >= int(g_import.from.size())) g_import.choice = 0;
    ImGui::BeginDisabled(state == ImportState::Copying);
    if (g_import.from.size() == 1) {
        ImGui::TextUnformatted(PathText(g_import.from[0]).c_str());
    } else {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
        if (ImGui::BeginCombo("##import_from", PathText(g_import.from[size_t(g_import.choice)]).c_str())) {
            for (int i = 0; i < int(g_import.from.size()); ++i)
                if (ImGui::Selectable(PathText(g_import.from[size_t(i)]).c_str(), i == g_import.choice)) g_import.choice = i;
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    if (state == ImportState::Copying) {
        ImGui::TextColored(kYellow, "%s", T("Copiando...", "Copying..."));
    } else if (state == ImportState::Failed) {
        ImGui::TextColored(kRed, T("No se pudo copiar: %s", "Could not copy: %s"), g_import.error.c_str());
    } else if (state == ImportState::Done) {
        ImGui::CloseCurrentPopup();
        if (!RestartLauncher(L""))
            a.notice = T("Ajustes copiados. Cierra y vuelve a abrir el launcher.", "Settings copied. Close and open the launcher again.");
    }
    ImGui::BeginDisabled(state == ImportState::Copying);
    if (ImGui::Button(T("Traer mis ajustes", "Bring my settings"))) {
        g_import.state = ImportState::Copying;
        const fs::path from = g_import.from[size_t(g_import.choice)], to = a.root;
        std::thread([from, to] {
            const std::string error = SettingsImport::Copy(from, to);
            g_import.error = error;
            g_import.state = error.empty() ? ImportState::Done : ImportState::Failed;
        }).detach();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("No, empezar de cero", "No, start fresh"))) {
        a.launcher.Set("import_offered", "no");
        SaveLauncher();
        g_import.state = ImportState::Idle;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void ToolsTab() {
    App& a = g_app;
    UpdateSection();
    ImGui::SeparatorText(T("Otra instalación", "Another installation"));
    ImGui::BeginDisabled(g_import.state == ImportState::Copying || a.game_running);
    if (ImGui::Button(T("Traer ajustes de otra instalación...", "Bring settings from another installation..."))) {
        if (const auto dir = PickFolder(a.hwnd, L"Otra instalación del port / Another installation of the port",
                                        a.root.parent_path())) {
            std::error_code ec;
            if (SettingsImport::IsUsedInstall(*dir) && !fs::equivalent(*dir, a.root, ec)) {
                g_import.from = {*dir};
                g_import.choice = 0;
                g_import.ask = true;
            } else {
                a.notice = T("Esa carpeta no es otra instalación del port usada (le falta run.py o sus ajustes).",
                             "That folder is not another used installation of the port (no run.py or settings).");
            }
        }
    }
    ImGui::EndDisabled();
    HelpMarker(T("Copia a esta carpeta los ajustes, controles, apodo, carpeta del juego, partidas guardadas, logros y DLC de "
                 "otra carpeta del port (por ejemplo, la de una versión anterior). La otra carpeta no se toca.",
                 "Copies the settings, controls, nickname, game folder, saves, trophies and DLC of another folder of the "
                 "port (an older release's, say) into this one. The other folder is left as it is."));
    ImGui::SeparatorText(T("Carpetas y registro", "Folders and log"));
    if (ImGui::Button(T("Abrir last_run.log", "Open last_run.log"))) OpenInShell(a.root / L"last_run.log");
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta del port", "Open the port folder"))) OpenInShell(a.root);
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir partidas guardadas", "Open saves"))) {
        std::error_code ec;
        fs::create_directories(a.root / L"user", ec);
        OpenInShell(a.root / L"user");
    }
    LauncherFlag("save_log", T("Guardar registro detallado (logs\\, con estadísticas de fotogramas)",
                               "Save a detailed log (logs\\, with frame statistics)"), false,
                 T("Para reportar fallos o tirones: adjunta el archivo de logs\\ en un issue.",
                   "To report crashes or stutter: attach the file from logs\\ to an issue."));

    ImGui::SeparatorText(T("Acceso directo", "Desktop shortcut"));
    if (ImGui::Button(T("Crear acceso directo en el Escritorio", "Create Desktop Shortcut"))) {
        std::string err;
        if (CreateDesktopShortcut(err)) {
            a.notice = T("Acceso directo creado en el Escritorio con éxito (inicia directo sin el launcher).",
                         "Desktop shortcut created successfully (launches directly without launcher).");
        } else {
            a.notice = err;
        }
    }
    HelpMarker(T("Crea un acceso directo en tu Escritorio para iniciar directamente el juego con su icono sin abrir la ventana del launcher.",
                 "Creates a shortcut on your Desktop to start the game directly with its icon without the launcher window."));

    ImGui::SeparatorText(T("Caché de shaders", "Shader cache"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Los shaders se guardan para que los siguientes arranques vayan fluidos. Bórrala solo si "
                                      "actualizaste el driver de la GPU o ves fallos gráficos.",
                                      "Shaders are kept so later sessions run smoothly. Clear it only after a GPU driver "
                                      "update or if you see graphics glitches."));
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(a.game_running);
    if (ImGui::Button(T("Borrar la caché de shaders", "Clear the shader cache"))) a.confirm_clear_cache = true;
    ImGui::EndDisabled();

    ImGui::SeparatorText(T("Idioma y apariencia", "Language & appearance"));
    const char* language_names[] = {"Auto (Windows)", "English", "Español", "Português (Brasil)", "Français",
                                    "Deutsch", "Italiano", "日本語", "简体中文", "Русский"};
    const char* language_codes[] = {"auto", "en", "es", "pt_BR", "fr", "de", "it", "ja", "zh_CN", "ru"};
    int selected_language = 0;
    for (int i = 0; i < 10; ++i) if (g_language == language_codes[i]) selected_language = i;
    if (ImGui::Combo(T("Idioma", "Language"), &selected_language, language_names, 10)) {
        g_language = language_codes[selected_language];
        ApplyLauncherLanguage();
        SaveLauncher();
    }
    const char* theme_names[] = {
        T(kThemes[0].name_es, kThemes[0].name_en),
        T(kThemes[1].name_es, kThemes[1].name_en),
        T(kThemes[2].name_es, kThemes[2].name_en),
        T(kThemes[3].name_es, kThemes[3].name_en),
    };
    if (ImGui::Combo(T("Tema visual", "Visual theme"), &g_ui_theme, theme_names, 4)) {
        ApplyUiTheme();
        SaveLauncher();
    }

    ImGui::SeparatorText(T("Acerca de", "About"));
    ImGui::Text("CTR-NitroFueled-PC %s", a.version.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Fork de Windows de SacredFire (T3CHNOLOG1C), basado en bbport (deadinside28), el port de Windows "
                             "de Supermedo y shadPS4. Parches de illusion y TroyWarez. Sin afiliación con Sony, Activision "
                             "ni Beenox. Licencia GPL v2 o posterior.",
                             "Windows fork of SacredFire (T3CHNOLOG1C), built on bbport (deadinside28), Supermedo's Windows "
                             "port and shadPS4. Patches by illusion and TroyWarez. Not affiliated with Sony, Activision or "
                             "Beenox. GPL v2 or later."));
    ImGui::Spacing();
    ImGui::TextUnformatted(T("Escalado temporal: AMD FidelityFX FSR 3.1 (MIT) y NVIDIA DLSS. DLSS usa el SDK de "
                             "NVIDIA DLSS (nvngx_dlss.dll, redistribuido según su licencia, en licenses\\) a través del "
                             "puente de IFreemz (MIT). NVIDIA, DLSS y GeForce RTX son marcas de NVIDIA Corporation.",
                             "Temporal upscaling: AMD FidelityFX FSR 3.1 (MIT) and NVIDIA DLSS. DLSS uses the NVIDIA DLSS "
                             "SDK (nvngx_dlss.dll, redistributed under its license, in licenses\\) through IFreemz's "
                             "bridge (MIT). NVIDIA, DLSS and GeForce RTX are trademarks of NVIDIA Corporation."));
    ImGui::PopTextWrapPos();
    {
        std::error_code ec;
        const bool dlss = fs::is_regular_file(a.root / L"bin" / L"sacredfire_dlss.dll", ec) &&
                          fs::is_regular_file(a.root / L"bin" / L"nvngx_dlss.dll", ec);
        ImGui::TextColored(kGrey, "%s", dlss ? T("DLSS: archivos presentes (necesita una tarjeta NVIDIA RTX)",
                                                 "DLSS: files present (needs an NVIDIA RTX card)")
                                             : T("DLSS: archivos no incluidos (se usa FSR 3)",
                                                 "DLSS: files not included (FSR 3 is used)"));
        ImGui::TextColored(kGrey, "%s", Fsr4FilesPresent()
                                            ? T("FSR 4: archivos presentes (necesita una gráfica con soporte INT8)",
                                                "FSR 4: files present (needs a GPU with INT8 support)")
                                            : T("FSR 4: archivos no incluidos (carpeta fsr4_shaders)",
                                                "FSR 4: files not included (fsr4_shaders folder)"));
    }
    ImGui::TextLinkOpenURL(kRepoUrl, kRepoUrl);
    if (a.python) ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
}

void ClearShaderCache() {
    std::error_code ec;
    const fs::path cache = g_app.root / L"user" / L"cache" / GameTitle();
    const auto removed = fs::remove_all(cache, ec);
    g_app.notice = ec ? std::string(T("No se pudo borrar la caché: ", "Could not clear the cache: ")) + ec.message()
                      : std::string(T("Caché borrada (", "Cache cleared (")) + std::to_string(removed) +
                            T(" archivos).", " files).");
}

void HeroHeader() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 178.0f * g_ui_scale;
    const ImVec2 end{origin.x + width, origin.y + height};
    const UiTheme& th = CurrentTheme();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (g_hero_art.texture) {
        const float image_aspect = float(g_hero_art.width) / float(g_hero_art.height);
        const float view_aspect = width / height;
        ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
        if (image_aspect < view_aspect) {
            const float visible = image_aspect / view_aspect;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        } else {
            const float visible = view_aspect / image_aspect;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        }
        draw->AddImage((ImTextureID)(intptr_t)g_hero_art.texture, origin, end, uv0, uv1);
    } else {
        draw->AddRectFilled(origin, end, Col(th.nav_bg, 1.0f));
    }
    // Smooth dark vignette fade on left and bottom so the logo and tabs blend into obsidian carbon
    draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + width * 0.52f, end.y),
                                  IM_COL32(8, 10, 14, 195), IM_COL32(8, 10, 14, 0),
                                  IM_COL32(8, 10, 14, 0), IM_COL32(8, 10, 14, 215));
    draw->AddRectFilledMultiColor(ImVec2(origin.x, end.y - 54.0f * g_ui_scale), end,
                                  IM_COL32(10, 12, 18, 0), IM_COL32(10, 12, 18, 0),
                                  Col(th.win_bg, 0.96f), Col(th.win_bg, 0.96f));
    if (g_logo_art.texture) {
        const float max_w = std::min(width * 0.30f, 320.0f * g_ui_scale);
        const float max_h = height * 0.80f;
        const float aspect = float(g_logo_art.width) / float(g_logo_art.height);
        float logo_w = max_w, logo_h = logo_w / aspect;
        if (logo_h > max_h) { logo_h = max_h; logo_w = logo_h * aspect; }
        const ImVec2 lo{origin.x + 24.0f * g_ui_scale, origin.y + (height - logo_h) * 0.5f};
        const ImVec2 hi{lo.x + logo_w, lo.y + logo_h};
        draw->AddImage((ImTextureID)(intptr_t)g_logo_art.texture, lo, hi);
    }
    // Glowing dual-tone racing accent bar along bottom of hero banner
    draw->AddRectFilledMultiColor(ImVec2(origin.x, end.y - 3.0f * g_ui_scale), end,
                                  Col(th.accent, 1.0f), Col(th.accent_alt, 1.0f),
                                  Col(th.accent_alt, 1.0f), Col(th.accent, 1.0f));
    ImGui::Dummy(ImVec2(width, height));
}

void WindowBar(float width) {
    const float height = 42.0f * g_ui_scale;
    ImGui::SetCursorPos(ImVec2(0, 0));
    const ImVec2 p = ImGui::GetWindowPos();
    const UiTheme& th = CurrentTheme();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height), Col(th.titlebar_bg, 1.0f));
    draw->AddRectFilledMultiColor(ImVec2(p.x, p.y + height - 2.0f), ImVec2(p.x + width, p.y + height),
                                  Col(th.accent, 0.95f), Col(th.accent_alt, 0.95f),
                                  Col(th.accent_alt, 0.95f), Col(th.accent, 0.95f));

    // Left: Launcher Title + PC Edition Badge
    ImFont* title_font = g_ctr_font ? g_ctr_font : ImGui::GetFont();
    const float fsize = ImGui::GetFontSize();
    const char* title_txt = "CTR: NITRO-FUELED";
    const ImVec2 tsize = title_font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, title_txt);
    const float ty = p.y + (height - tsize.y) * 0.5f;
    draw->AddRectFilled(ImVec2(p.x + 12.0f * g_ui_scale, p.y + 10.0f * g_ui_scale),
                        ImVec2(p.x + 16.0f * g_ui_scale, p.y + height - 10.0f * g_ui_scale),
                        Col(th.accent, 1.0f), 2.0f);
    draw->AddText(title_font, fsize, ImVec2(p.x + 24.0f * g_ui_scale, ty), IM_COL32(248, 250, 255, 255), title_txt);

    // Region & Status pills on the titlebar
    float pill_x = p.x + 24.0f * g_ui_scale + tsize.x + 14.0f * g_ui_scale;
    auto draw_pill = [&](const std::string& text, ImVec4 border_col, ImVec4 text_col) {
        ImFont* f = ImGui::GetFont();
        const float pfs = fsize * 0.84f;
        const ImVec2 sz = f->CalcTextSizeA(pfs, FLT_MAX, 0.0f, text.c_str());
        const float pw = sz.x + 18.0f * g_ui_scale;
        const float ph = 24.0f * g_ui_scale;
        const float py = p.y + (height - ph) * 0.5f;
        draw->AddRectFilled(ImVec2(pill_x, py), ImVec2(pill_x + pw, py + ph),
                            IM_COL32(18, 22, 32, 220), ph * 0.5f);
        draw->AddRect(ImVec2(pill_x, py), ImVec2(pill_x + pw, py + ph),
                      Col(border_col, 0.70f), ph * 0.5f, 0, 1.2f);
        draw->AddText(f, pfs, ImVec2(pill_x + 9.0f * g_ui_scale, py + (ph - sz.y) * 0.5f),
                      Col(text_col, 1.0f), text.c_str());
        pill_x += pw + 8.0f * g_ui_scale;
    };
    if (IsCtrTitle(g_app.game_info.title)) {
        const std::string region = (g_app.game_info.title == "CUSA14876" ? "Europe (PAL) · " : "Americas (NTSC) · ") +
                                   g_app.game_info.title;
        draw_pill(region, th.accent_alt, th.accent_alt);
    }
    if (g_app.game_info.kind == Kind::Complete && g_app.game_checked && g_app.game_ok) {
        const bool onl = g_app.game.Flag("online", true);
        const std::string status = onl ? std::string(T("● ONLINE ACTIVO", "● ONLINE READY"))
                                       : std::string(T("● LISTO (OFFLINE)", "● READY TO PLAY"));
        draw_pill(status, kGreen, kGreen);
    }

    const float bw = 42.0f * g_ui_scale, bh = 30.0f * g_ui_scale, gap = 3.0f * g_ui_scale;
    const float left = width - 3.0f * bw - 2.0f * gap - 8.0f * g_ui_scale;

    // Theme quick-switch button right next to window controls
    const std::string theme_btn_label = std::string(T("Tema: ", "Theme: ")) + T(th.name_es, th.name_en) + "##theme_cycle";
    const float theme_bw = 165.0f * g_ui_scale;
    const float theme_bx = left - theme_bw - 10.0f * g_ui_scale;
    if (theme_bx > pill_x - p.x + 8.0f) {
        ImGui::SetCursorPos(ImVec2(theme_bx, (height - bh) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, th.frame_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.button_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.button_active);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * g_ui_scale);
        if (ImGui::Button(theme_btn_label.c_str(), ImVec2(theme_bw, bh))) {
            g_ui_theme = (g_ui_theme + 1) % 4;
            ApplyUiTheme();
            SaveLauncher();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    auto button = [&](const char* id, int kind) {
        ImGui::SetCursorPos(ImVec2(left + kind * (bw + gap), (height - bh) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, th.frame_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kind == 2 ? ImVec4(0.85f, 0.20f, 0.24f, 1.0f) : th.button_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kind == 2 ? ImVec4(0.95f, 0.28f, 0.32f, 1.0f) : th.button_active);
        const bool clicked = ImGui::Button(id, ImVec2(bw, bh));
        ImGui::PopStyleColor(3);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const ImU32 ink = IM_COL32(245, 245, 250, 255);
        if (kind == 0) draw->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x + 5, c.y), ink, 1.6f);
        else if (kind == 1) draw->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 0, 0, 1.4f);
        else {
            draw->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 1.6f);
            draw->AddLine(ImVec2(c.x + 5, c.y - 5), ImVec2(c.x - 5, c.y + 5), ink, 1.6f);
        }
        return clicked;
    };
    if (button("##minimize", 0)) ShowWindow(g_app.hwnd, SW_MINIMIZE);
    if (button("##maximize", 1)) ShowWindow(g_app.hwnd, IsZoomed(g_app.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    if (button("##close", 2)) SendMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
    RECT client{};
    GetClientRect(g_app.hwnd, &client);
    const LONG drag_right = std::max<LONG>(0, LONG(theme_bx > 0 ? theme_bx : left));
    g_titlebar_drag = RECT{0, 0, drag_right, LONG(height)};
    ImGui::SetCursorPos(ImVec2(14, height + 6));
}

bool LoadArtworkFile(const fs::path& path, LauncherArtwork& artwork) {
    if (!g_device || !fs::is_regular_file(path)) return false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    std::vector<BYTE> pixels(size_t(w) * h * 4);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{pixels.data(), w * 4, 0};
        hr = g_device->CreateTexture2D(&td, &sd, &texture);
    }
    if (SUCCEEDED(hr)) hr = g_device->CreateShaderResourceView(texture, nullptr, &artwork.texture);
    if (texture) texture->Release(); if (converter) converter->Release(); if (frame) frame->Release();
    if (decoder) decoder->Release(); if (factory) factory->Release();
    if (FAILED(hr)) { artwork = {}; return false; }
    artwork.width = w; artwork.height = h; return true;
}

void LoadArtwork() {
    const fs::path art = g_app.root / L"art";
    LoadArtworkFile(art / L"ctr-hero.png", g_hero_art);
    LoadArtworkFile(art / L"ctr-logo.png", g_logo_art);
    LoadArtworkFile(art / L"ctr-menu-background.png", g_menu_background);
}

void ReleaseArtwork() {
    for (auto* a : {&g_hero_art, &g_logo_art, &g_menu_background}) {
        if (a->texture) a->texture->Release();
        *a = {};
    }
}

void DrawMenuIcon(ImDrawList* d, int id, ImVec2 c, ImU32 ink, float s) {
    const ImU32 navy = IM_COL32(12, 14, 22, 240);
    auto line = [&](ImVec2 a, ImVec2 b, float t = 2.2f) { d->AddLine(a, b, navy, t * s); d->AddLine(a, b, ink, t * s * 0.58f); };
    if (id == 0) { // checkered flag / race
        line({c.x-7*s,c.y+8*s},{c.x-7*s,c.y-8*s},2.5f); d->AddRectFilled({c.x-5*s,c.y-8*s},{c.x+8*s,c.y+1*s},ink,1.5f*s);
        d->AddRectFilled({c.x-2*s,c.y-8*s},{c.x+1*s,c.y-3.5f*s},navy); d->AddRectFilled({c.x+4*s,c.y-3.5f*s},{c.x+7*s,c.y+1*s},navy);
    } else if (id == 1) { // gear
        d->AddCircleFilled(c,7*s,navy,12); d->AddCircleFilled(c,5.5f*s,ink,12); d->AddCircleFilled(c,2*s,navy,12);
        for (int i=0;i<8;++i) { float a=float(i)*0.785398f; ImVec2 p{c.x+7*s*std::cos(a),c.y+7*s*std::sin(a)}; d->AddCircleFilled(p,1.6f*s,ink,8); }
    } else if (id == 2) { // wireless
        d->PathArcTo({c.x,c.y+5*s},8*s,3.85f,5.58f,18); d->PathStroke(navy,0,4*s); d->PathArcTo({c.x,c.y+5*s},8*s,3.85f,5.58f,18); d->PathStroke(ink,0,2.2f*s);
        d->PathArcTo({c.x,c.y+5*s},5*s,3.85f,5.58f,18); d->PathStroke(ink,0,2*s); d->AddCircleFilled({c.x,c.y+5*s},1.7f*s,ink,10);
    } else if (id == 3) { // gamepad
        d->AddRect({c.x-9*s,c.y-5*s},{c.x+9*s,c.y+6*s},navy,5*s,0,3*s); d->AddRect({c.x-8*s,c.y-4*s},{c.x+8*s,c.y+5*s},ink,4*s,0,2*s);
        line({c.x-5*s,c.y},{c.x-1*s,c.y},2); line({c.x-3*s,c.y-2*s},{c.x-3*s,c.y+2*s},2); d->AddCircleFilled({c.x+4*s,c.y-1*s},1.4f*s,navy); d->AddCircleFilled({c.x+6*s,c.y+2*s},1.4f*s,navy);
    } else if (id == 4) { // keyboard
        d->AddRectFilled({c.x-9*s,c.y-6*s},{c.x+9*s,c.y+6*s},navy,2*s); d->AddRectFilled({c.x-7*s,c.y-4*s},{c.x+7*s,c.y+4*s},ink,1*s);
        for(int r=0;r<2;++r) for(int col=0;col<5;++col) d->AddRectFilled({c.x-6*s+col*2.5f*s,c.y-3*s+r*3*s},{c.x-4.5f*s+col*2.5f*s,c.y-1.8f*s+r*3*s},navy,0.4f*s);
    } else if (id == 5) { // patch badge
        const ImVec2 pts[]={{c.x,c.y-9*s},{c.x+8*s,c.y-6*s},{c.x+7*s,c.y+3*s},{c.x,c.y+9*s},{c.x-7*s,c.y+3*s},{c.x-8*s,c.y-6*s}};
        d->AddConvexPolyFilled(pts,6,navy); const ImVec2 in[]={{c.x,c.y-7*s},{c.x+6*s,c.y-4.5f*s},{c.x+5*s,c.y+2*s},{c.x,c.y+7*s},{c.x-5*s,c.y+2*s},{c.x-6*s,c.y-4.5f*s}}; d->AddConvexPolyFilled(in,6,ink);
        line({c.x-3*s,c.y},{c.x-0.5f*s,c.y+2.5f*s},2.4f); line({c.x-0.5f*s,c.y+2.5f*s},{c.x+4*s,c.y-3*s},2.4f);
    } else if (id == 6) { // save card
        d->AddRectFilled({c.x-7*s,c.y-9*s},{c.x+7*s,c.y+9*s},navy,1*s); d->AddRectFilled({c.x-5.5f*s,c.y-7*s},{c.x+5.5f*s,c.y+7*s},ink,0.8f*s);
        d->AddRectFilled({c.x-3.5f*s,c.y-6*s},{c.x+3.5f*s,c.y-2*s},navy); d->AddRectFilled({c.x-3.5f*s,c.y+1*s},{c.x+3.5f*s,c.y+5*s},navy,0.6f*s);
    } else if (id == 7) { // crate
        d->AddRectFilled({c.x-8*s,c.y-7*s},{c.x+8*s,c.y+7*s},navy,1*s); d->AddRectFilled({c.x-6*s,c.y-5*s},{c.x+6*s,c.y+5*s},ink,1*s);
        d->AddRect({c.x-4*s,c.y-3*s},{c.x+4*s,c.y+3*s},navy,0,0,1.6f*s); d->AddLine({c.x-2*s,c.y+1*s},{c.x,c.y-2*s},navy,1.8f*s); d->AddLine({c.x,c.y-2*s},{c.x+2*s,c.y+1*s},navy,1.8f*s);
    } else if (id == 8) { // trophy star
        d->AddCircleFilled(c, 7.2f * s, navy, 16);
        d->AddCircleFilled(c, 5.6f * s, ink, 16);
        d->AddCircleFilled(c, 2.4f * s, navy, 12);
    } else { // crossed tools
        line({c.x-7*s,c.y-7*s},{c.x+7*s,c.y+7*s},3); line({c.x+7*s,c.y-7*s},{c.x-7*s,c.y+7*s},3); d->AddCircleFilled(c,2.5f*s,ink,12);
    }
}

bool MenuEntry(int id, const char* label, bool selected, float width) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size{width, 42.0f * g_ui_scale};
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a{start.x, start.y}, b{start.x + size.x, start.y + size.y};
    const UiTheme& th = CurrentTheme();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float rounding = 7.0f * g_ui_scale;
    const ImVec2 r0{a.x + 2.0f, a.y + 2.0f};
    const ImVec2 r1{b.x - 2.0f, b.y - 2.0f};

    if (selected) {
        d->AddRectFilledMultiColor(r0, r1,
                                   Col(th.accent, 0.34f), Col(th.accent_alt, 0.14f),
                                   Col(th.accent_alt, 0.14f), Col(th.accent, 0.34f));
        d->AddRect(r0, r1, Col(th.accent, 0.85f), rounding, 0, 1.5f);
        d->AddRectFilled({r0.x, r0.y + 4.0f * g_ui_scale},
                         {r0.x + 4.5f * g_ui_scale, r1.y - 4.0f * g_ui_scale},
                         Col(th.accent_alt, 1.0f), 2.0f);
    } else if (hovered) {
        d->AddRectFilled(r0, r1, Col(th.frame_hover, 0.88f), rounding);
        d->AddRect(r0, r1, Col(th.border, 0.70f), rounding, 0, 1.0f);
        d->AddRectFilled({r0.x, r0.y + 8.0f * g_ui_scale},
                         {r0.x + 3.0f * g_ui_scale, r1.y - 8.0f * g_ui_scale},
                         Col(th.accent, 0.80f), 1.5f);
    } else {
        d->AddRectFilled(r0, r1, Col(th.frame_bg, 0.55f), rounding);
    }

    const ImVec2 icon{a.x + 26.0f * g_ui_scale, a.y + size.y * 0.5f};
    const ImU32 icon_col = selected ? Col(th.accent_alt, 1.0f)
                                    : hovered ? IM_COL32(255, 255, 255, 245) : IM_COL32(195, 204, 220, 220);
    DrawMenuIcon(d, id, icon, icon_col, 0.78f * g_ui_scale);
    ImFont* label_font = g_ctr_font ? g_ctr_font : ImGui::GetFont();
    const float label_font_size = ImGui::GetFontSize();
    const ImVec2 text_size = label_font->CalcTextSizeA(label_font_size, FLT_MAX, 0.0f, label);
    const ImVec2 text{a.x + 48.0f * g_ui_scale, a.y + (size.y - text_size.y) * 0.5f};
    const ImU32 txt_col = selected ? Col(th.accent_alt, 1.0f)
                                   : hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(224, 230, 242, 235);
    d->AddText(label_font, label_font_size, text, txt_col, label);
    return clicked;
}

void Frame() {
    App& a = g_app;
    PollJobs();
    PollUpdate();
    const UiTheme& th = CurrentTheme();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (g_menu_background.texture) {
        const float wa=vp->WorkSize.x/vp->WorkSize.y;
        const float ia=float(g_menu_background.width)/float(g_menu_background.height);
        ImVec2 uv0{0,0},uv1{1,1};
        if (ia>wa) { const float v=wa/ia; uv0.x=(1-v)*0.5f; uv1.x=1-uv0.x; }
        else { const float v=ia/wa; uv0.y=(1-v)*0.5f; uv1.y=1-uv0.y; }
        ImDrawList* bg = ImGui::GetWindowDrawList();
        const ImVec2 p1{vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y};
        bg->AddImage((ImTextureID)(intptr_t)g_menu_background.texture, vp->WorkPos, p1, uv0, uv1);
        bg->AddRectFilled(vp->WorkPos, p1, Col(th.win_bg, 0.84f));
    }
    const float width = ImGui::GetWindowSize().x;
    WindowBar(width);
    HeroHeader();

    if (!a.notice.empty()) {
        ImGui::TextColored(kYellow, "%s", a.notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) a.notice.clear();
    }
    UpdateBanner();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, th.nav_bg);
    ImGui::BeginChild("menu_body", ImVec2(0, 0), ImGuiChildFlags_None);
    const float menu_w = 236.0f * g_ui_scale;
    ImGui::BeginChild("menu_navigation", ImVec2(menu_w, 0), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(16.0f * g_ui_scale, 12.0f * g_ui_scale));
    ImGui::TextColored(th.accent_alt, "%s", T("MENÚ DE CARRERA", "RACING MENU"));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f * g_ui_scale);
    const char* pages[] = {T("Jugar", "Play"), T("Ajustes", "Settings"), "Online", T("Mando", "Gamepad"),
                           T("Teclado", "Keyboard"), "Mods", T("Guardados", "Saves"), "DLC",
                           T("Logros", "Achievements"), T("Herramientas", "Tools")};
    for (int i=0; i<10; ++i) {
        ImGui::SetCursorPosX(8.0f * g_ui_scale);
        if (MenuEntry(i, pages[i], a.active_page == i, menu_w - 16.0f * g_ui_scale)) {
            a.active_page = i;
            if (i != 3) g_pad_capture = -1;
            if (i != 4) g_key_capture = -1;
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * g_ui_scale);
    }
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 34.0f * g_ui_scale));
    ImGui::SetCursorPosX(16.0f * g_ui_scale);
    ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.84f, 0.85f), "CTR-NF PC  •  %s", a.version.c_str());
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, th.content_bg);
    // Real child padding applies to every row, table, wrap edge and tab.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * g_ui_scale, 16.0f * g_ui_scale));
    ImGui::BeginChild("menu_content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
    switch (a.active_page) {
    case 0: PlayTab(); break;
    case 1: SettingsTab(); break;
    case 2: OnlineTab(); break;
    case 3: GamepadTab(); break;
    case 4: KeyboardTab(); break;
    case 5: ModsTab(); break;
    case 6: SavesTab(); break;
    case 7: DlcTab(); break;
    case 8: TrophiesTab(); break;
    default: ToolsTab(); break;
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    SaveConfirmations();
    NickPopup();
    JoinServerPopup();
    ImportPopup();
    ShaderProgressPopup();
    if (a.confirm_clear_cache) {
        ImGui::OpenPopup("clear_cache");
        a.confirm_clear_cache = false;
    }
    if (ImGui::BeginPopupModal("clear_cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Borrar la caché de shaders? El próximo arranque volverá a compilarlos.",
                                 "Clear the shader cache? The next start compiles them again."));
        if (ImGui::Button(T("Borrar", "Clear"))) {
            ClearShaderCache();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (a.want_close) {
        ImGui::OpenPopup("close");
        a.want_close = false;
    }
    if (ImGui::BeginPopupModal("close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("El juego sigue abierto.", "The game is still running."));
        if (ImGui::Button(T("Cerrar el juego y el launcher", "Stop the game and quit"))) {
            StopGame();
            PostQuitMessage(0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void LoadState() {
    App& a = g_app;
    a.root = FindRoot();
    std::error_code ec;
    a.bin = fs::is_directory(a.root / L"bin", ec) ? a.root / L"bin" : a.root / L"out";
    for (const fs::path& v : {a.bin / L"VERSION", a.root / L"VERSION", a.root / L"out" / L"VERSION"}) {
        if (fs::is_regular_file(v, ec)) {
            a.version = Trim(ReadText(v));
            break;
        }
    }
    a.python = FindPython(a.root);
    // A folder never used (no launcher.ini): another installation's settings are offered once.
    const bool new_folder = !fs::exists(LauncherIni(), ec);
    // The portable package ships bbport.default.ini (an update unzipped over it keeps bbport.ini).
    if (!fs::exists(GameIni(), ec) && fs::is_regular_file(a.root / L"bbport.default.ini", ec)) {
        fs::copy_file(a.root / L"bbport.default.ini", GameIni(), ec);
    }
    a.launcher.Load(LauncherIni());
    a.game.Load(GameIni());
    if (const fs::path backup = BackupKeyPath(); !backup.empty()) {
        const fs::path installs = backup.parent_path() / L"installs.txt";
        if (new_folder && a.launcher.Get("import_offered").empty()) {
            g_import.from = SettingsImport::Previous(installs, a.root);
            g_import.ask = !g_import.from.empty();
        }
        SettingsImport::Remember(installs, a.root);
    }
    SyncOnlineBuffers();
    const std::string lang = a.launcher.Get("ui_language");
    g_language = lang.empty() ? "auto" : lang;
    g_ui_theme = std::clamp(a.launcher.Int("ui_theme", 0), 0, 3);
    ApplyLauncherLanguage();
    a.game_dir = TextPath(a.launcher.Get("game_dir"));
    a.base_dir = TextPath(a.launcher.Get("base_dir"));
    a.update_dir = TextPath(a.launcher.Get("update_dir"));
    // First start of a portable package: a merged folder next to the launcher is picked up.
    if (a.game_dir.empty()) {
        for (const wchar_t* name : {L"CUSA14876-1.21", L"CUSA13795-1.21", L"CUSA14876", L"CUSA13795"}) {
            if (Analyze(a.root / name).kind == Kind::Complete) {
                a.game_dir = a.root / name;
                break;
            }
        }
        for (const wchar_t* title : {L"CUSA14876", L"CUSA13795"}) {
            const std::wstring base = title, update = base + L"-patch";
            if (a.base_dir.empty() && Analyze(a.root / base).kind == Kind::Base) a.base_dir = a.root / base;
            if (a.update_dir.empty() && Analyze(a.root / update).kind == Kind::Update) a.update_dir = a.root / update;
        }
    }
    a.base_info = Analyze(a.base_dir);
    a.update_info = Analyze(a.update_dir);
    a.patches = LoadPatches();
    ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    StartCheck();
    StartProbe();
}

// ---------------------------------------------------------------------------------------------
// Win32 + Direct3D 11 (Dear ImGui's example_win32_directx11)

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_occluded = false;

ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(width);
    td.Height = UINT(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{bgra, UINT(width * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (!g_device || FAILED(g_device->CreateTexture2D(&td, &data, &texture))) return nullptr;
    g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return view;
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swapchain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swapchain, &g_device, &level, &g_context);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapchain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCHITTEST) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        RECT client{}; GetClientRect(hwnd, &client);
        if (!IsZoomed(hwnd)) {
            const LONG edge = LONG(8.0f * g_ui_scale);
            const bool l=pt.x<edge, r=pt.x>=client.right-edge, t=pt.y<edge, b=pt.y>=client.bottom-edge;
            if (t&&l) return HTTOPLEFT; if (t&&r) return HTTOPRIGHT;
            if (b&&l) return HTBOTTOMLEFT; if (b&&r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (b) return HTBOTTOM;
        }
        return PtInRect(&g_titlebar_drag, pt) ? HTCAPTION : HTCLIENT;
    }
    // Gamepad tab: Esc cancels capture
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_pad_capture >= 0) {
        if (wparam == VK_ESCAPE) {
            g_pad_capture = -1;
            return 0;
        }
    }
    // Keyboard tab: the next key's scan code, before ImGui sees it.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_key_capture >= 0) {
        const unsigned code = unsigned((lparam >> 16) & 0xff) | ((lparam & (1 << 24)) ? 0x100u : 0u);
        g_key_captured = code + 1;
        return 0;
    }
    // ...or the next mouse button or wheel step (kMouseCode + SDL's button number). "Change" acts
    // on the button's release, so this is the click after it.
    if (g_key_capture >= 0) {
        unsigned button = 0;
        if (msg == WM_LBUTTONDOWN) button = 1;
        else if (msg == WM_MBUTTONDOWN) button = 2;
        else if (msg == WM_RBUTTONDOWN) button = 3;
        else if (msg == WM_XBUTTONDOWN) button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 4 : 5;
        else if (msg == WM_MOUSEWHEEL && GET_WHEEL_DELTA_WPARAM(wparam) != 0)
            button = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 6 : 7;
        if (button) {
            g_key_captured = kMouseCode + button + 1;
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wparam) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            if (IsZoomed(hwnd)) {
                MONITORINFO monitor{sizeof monitor};
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                    params->rgrc[0] = monitor.rcWork;
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = LONG(900.0f * g_ui_scale);
        info->ptMinTrackSize.y = LONG(640.0f * g_ui_scale);
        MONITORINFO monitor{sizeof monitor};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
        return 0;
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;
        g_resize_w = LOWORD(lparam);
        g_resize_h = HIWORD(lparam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_app.game_running) {
            g_app.want_close = true;
            ShowWindow(hwnd, SW_RESTORE);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
#ifdef LAUNCHER_SELFTEST
    // Test build only (CMake never defines it): file operations without the window.
    //   --import <folder> <port>, --restore <backup> <port>, --patches <port>; result in <port>\selftest.txt
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 3) {
            const std::wstring op = argv[1];
            g_app.root = argv[argc - 1];
            std::string result;
            if (op == L"--import" && argc == 4) result = ImportSaves(argv[2]);
            else if (op == L"--restore" && argc == 4) result = RestoreBackup(argv[2]);
            else if (op == L"--dlc") {
                for (const auto& d : ScanDlc())
                    result += d.file + " | " + d.title + " | " + d.label + " | " + (d.ok ? "ok" : d.problem) + "\n";
            } else if (op == L"--aes") {
                // FIPS-197 C.1: AES-128, key 00..0f, plaintext 00112233..ff.
                std::string key;
                unsigned char plain[16], out[16], zero[16]{};
                for (int i = 0; i < 16; ++i) {
                    key += char(i);
                    plain[i] = (unsigned char)(i * 0x11);
                }
                AesCbc(key, zero, plain, 16, out, true);
                char hex[40]{};
                for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", out[i]);
                result = std::string("aes ") + hex + (std::string(hex) == "69c4e0d86a7b0430d8cdb78070b4c55a" ? " ok" : " BAD");
            } else if (op == L"--esfm" && argc == 6) {
                // --esfm <key hex> <NP comm ID> <entry file> <port>
                const std::string xml = DecryptEsfm(TrophyKeyBytes(PathText(fs::path(argv[2]))), PathText(fs::path(argv[3])),
                                                    ReadText(argv[4]));
                result = xml.empty() ? "esfm: no xml" : "esfm: " + xml.substr(0, 200);
            } else if (op == L"--trophies" && argc == 4) {
                // --trophies <game folder> <port>
                g_app.game_dir = argv[2];
                LoadTrophies();
                result = "title " + g_trophies.title_id + " np " + g_trophies.np_comm_id + " trophies " +
                         std::to_string(g_trophies.list.size()) + (g_trophies.error.empty() ? "" : " error " + g_trophies.error) + "\n";
                for (const auto& tr : g_trophies.list)
                    result += std::to_string(tr.id) + " " + (tr.unlocked ? "unlocked" : "locked") + "\n";
            } else if (op == L"--patches") {
                for (const auto& p : LoadPatches())
                    result += p.key + " | " + p.author + " | " + p.version + " | " + (p.ini_key ? p.ini_key : "-") +
                              (p.default_on ? " | on" : "") + "\n";
                std::vector<std::string> on, off;
                ReadPatchConfig(on, off);
                for (const auto& k : on) result += "json enabled: " + k + "\n";
                for (const auto& k : off) result += "json disabled: " + k + "\n";
            }
            std::ofstream(g_app.root / L"selftest.txt", std::ios::binary) << (result.empty() ? "OK" : result) << "\n";
            return 0;
        }
    }
#endif
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int cmd_argc = 0;
    LPWSTR* cmd_argv = CommandLineToArgvW(GetCommandLineW(), &cmd_argc);
    bool direct_launch = false, updated = false;
    if (cmd_argv) {
        for (int i = 1; i < cmd_argc; ++i) {
            if (_wcsicmp(cmd_argv[i], L"--updated") == 0) updated = true;  // started by an update (PollUpdate)
            if (_wcsicmp(cmd_argv[i], L"--tab") == 0 && i + 1 < cmd_argc) {
                const std::string tab = Lower(Narrow(cmd_argv[++i]));
                if (tab == "play") g_app.active_page = 0;
                else if (tab == "settings") g_app.active_page = 1;
                else if (tab == "online") g_app.active_page = 2;
                else if (tab == "gamepad" || tab == "controls") g_app.active_page = 3;
                else if (tab == "keyboard") g_app.active_page = 4;
                else if (tab == "mods" || tab == "patches") g_app.active_page = 5;
                else if (tab == "saves") g_app.active_page = 6;
                else if (tab == "dlc") g_app.active_page = 7;
                else if (tab == "achievements" || tab == "trophies") g_app.active_page = 8;
                else if (tab == "tools") g_app.active_page = 9;
                continue;
            }
            if (_wcsicmp(cmd_argv[i], L"--launch") == 0 || _wcsicmp(cmd_argv[i], L"--play") == 0 ||
                _wcsicmp(cmd_argv[i], L"-launch") == 0 || _wcsicmp(cmd_argv[i], L"-play") == 0 ||
                _wcsicmp(cmd_argv[i], L"--direct") == 0) {
                direct_launch = true;
                break;
            }
        }
        LocalFree(cmd_argv);
    }

    LoadState();
    MigrateOldLauncher();
    if (!direct_launch) {
        CleanupUpdate();
        if (updated) {
            g_app.notice = Format(T("Actualizado a %s. Tus ajustes, amigos, parches y partidas se mantienen.",
                                    "Updated to %s. Your settings, friends, patches and saves are kept."),
                                  g_app.version);
        } else if (g_app.launcher.Flag("update_check", false)) {
            StartUpdateCheck(g_app.launcher.Flag("update_auto", false));
        }
    }

    if (direct_launch) {
        if (g_app.worker.joinable()) g_app.worker.join();
        PollJobs();
        if (GameReady()) {
            if (g_app.launcher.Flag("discord_rpc", true))
                RegisterDiscordLaunch(DiscordAppId());
            StartGame();
            while (g_app.game_running) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                PollJobs();
                UpdateDiscord();
                Sleep(50);
            }
            DiscordPresence::Shutdown();
            if (g_app.worker.joinable()) g_app.worker.join();
            if (g_app.probe.joinable()) g_app.probe.join();
            CoUninitialize();
            return g_app.exit_code;
        }
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"CtrNitroFueledPcLauncher", nullptr};
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int width = int(1120 * scale), height = int(840 * scale);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + std::max(0, int(work.right - work.left - width) / 2);
    const int y = work.top + std::max(0, int(work.bottom - work.top - height) / 2);
    g_ui_scale = scale;
    HWND hwnd = CreateWindowW(wc.lpszClassName, kLauncherName,
                              WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                              x, y, width, height, nullptr, nullptr, instance, nullptr);
    g_app.hwnd = hwnd;
    const BOOL dark_titlebar = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark_titlebar, sizeof(dark_titlebar));
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", kLauncherName, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ApplyUiTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 17.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowPadding = ImVec2(16 * scale, 14 * scale);
    style.FramePadding = ImVec2(10 * scale, 6 * scale);
    style.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    style.FrameRounding = 5 * scale;
    style.TabRounding = 5 * scale;
    style.WindowRounding = 9 * scale;
    wchar_t fonts[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(fonts, MAX_PATH);
    const fs::path segoe = fs::path(std::wstring(fonts, n)) / L"Fonts" / L"segoeui.ttf";
    std::error_code ec;
    // Japanese and Chinese, which Segoe UI and the CTR font lack: Windows' own CJK fonts merged in
    // as fallbacks (ImGui 1.92 loads their glyphs only when a text uses them).
    const auto merge_fallbacks = [&](bool with_segoe) {
        std::vector<fs::path> files;
        if (with_segoe) files.push_back(segoe);
        for (const wchar_t* name : {L"msyh.ttc", L"YuGothM.ttc", L"msgothic.ttc"})
            files.push_back(fs::path(std::wstring(fonts, n)) / L"Fonts" / name);
        for (const fs::path& file : files) {
            if (!fs::is_regular_file(file, ec)) continue;
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(PathText(file).c_str(), style.FontSizeBase, &merge);
        }
    };
    if (fs::is_regular_file(segoe, ec)) io.Fonts->AddFontFromFileTTF(PathText(segoe).c_str(), style.FontSizeBase);
    else io.Fonts->AddFontDefault();
    merge_fallbacks(false);
    const fs::path ctr_font = g_app.root / L"art" / L"ctr-menu.ttf";
    if (fs::is_regular_file(ctr_font, ec)) {
        g_ctr_font = io.Fonts->AddFontFromFileTTF(PathText(ctr_font).c_str(), style.FontSizeBase);
        merge_fallbacks(true);  // Cyrillic and CJK labels in the CTR font's places
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    LoadArtwork();
    if (g_app.launcher.Flag("discord_rpc", true))
        RegisterDiscordLaunch(DiscordAppId());

    bool done = false;
    while (!done) {
        MSG msg;
        // Idle while minimized or not in front: wait for input or 100 ms (background work).
        if (IsIconic(hwnd)) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) {
            PollJobs();
            continue;
        }
        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            PollJobs();
            continue;
        }
        g_occluded = false;
        if (g_resize_w && g_resize_h) {
            CleanupRenderTarget();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            CreateRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    DiscordPresence::Shutdown();
    if (g_update.worker.joinable()) {  // a download stops; an install in progress finishes
        g_update.progress.cancel = true;
        g_update.worker.join();
    }
    // Background work finishes on its own; a running game keeps running.
    if (g_app.worker.joinable()) {
        if (g_app.job == Job::Play) g_app.worker.detach();
        else g_app.worker.join();
    }
    if (g_app.probe.joinable()) g_app.probe.join();
    ReleaseTrophies(g_trophies);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseArtwork();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
