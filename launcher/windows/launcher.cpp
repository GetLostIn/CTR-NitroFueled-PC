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
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
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

#include "gamepads.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace fs = std::filesystem;

namespace {

// The Americas build (CUSA13795) and the European one (CUSA14876, as in martijnhe's
// OCTR-NitroFueled-PC-EU): the port runs both.
constexpr const char* kTitleId = "CUSA13795";
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

// ---------------------------------------------------------------------------------------------
// Language: Spanish or English, chosen in the launcher (default: Windows' language).

bool g_spanish = true;
const char* T(const char* es, const char* en) { return g_spanish ? es : en; }

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
// The European build (CUSA14876): the languages of the PAL release (its store page: English,
// French, German, Italian, Spanish and Arabic), as PS4 codes. A language the files do not hold
// falls back to English in the game.
constexpr Choice kGameLanguagesEu[] = {
    {"Inglés (Reino Unido)", "English (UK)", "18"},
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
    std::string key, name, author, note, version;
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
    fs::path root, bin;
    std::string version;
    std::optional<Python> python;

    Ini launcher;  // launcher.ini
    Ini game;      // bbport.ini

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
    DWORD exit_code = 0;
    bool show_exit = false, stopped = false;

    // Monitors and controllers (bb-gpu-capabilities).
    std::vector<std::pair<std::string, std::string>> displays;  // value (number), label
    std::vector<std::pair<std::string, std::string>> gamepads;  // GUID, name
    std::thread probe;
    std::atomic<bool> probe_done{false};

    bool want_close = false;
    bool confirm_clear_cache = false;
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

/// The title ID of the chosen game (the merged folder, else the update or the base game): the
/// saves, the shader cache, the languages and the merged folder's name follow it.
std::string GameTitleId() {
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info})
        if (IsCtrTitle(info->title)) return info->title;
    return kTitleId;
}
std::wstring GameTitle() { return Widen(GameTitleId()); }
bool EuEdition() { return GameTitle() == L"CUSA14876"; }

std::string EffectiveRenderRes();
std::string GameAspect();
bool UltrawideCacheWarm();

fs::path LauncherIni() { return g_app.root / L"launcher.ini"; }
fs::path GameIni() { return g_app.root / L"ctrnfpc.ini"; }

void SaveLauncher() {
    App& a = g_app;
    a.launcher.Set("game_dir", PathText(a.game_dir));
    a.launcher.Set("base_dir", PathText(a.base_dir));
    a.launcher.Set("update_dir", PathText(a.update_dir));
    a.launcher.Set("ui_language", g_spanish ? "es" : "en");
    if (!a.launcher.Save(LauncherIni(), "# CTR-NitroFueled-PC launcher")) {
        a.notice = T("No se pudo guardar launcher.ini.", "Could not save launcher.ini.");
    }
}

void SaveGameIni() {
    if (!g_app.game.Save(GameIni(), "# bbport settings (in-game menu: Insert / L3+R3)")) {
        g_app.notice = T("No se pudo guardar ctrnfpc.ini.", "Could not save ctrnfpc.ini.");
    }
}

void AddLog(const std::string& line) {
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

const char* DefaultGameLanguage() {
    if (EuEdition()) return g_spanish ? "3" : "18";
    return g_spanish ? "20" : "1";
}

/// The game language to start with: the saved one when the build has it, else the default (older
/// launchers saved codes this build lacks, such as 3 for Spain's Spanish).
std::string GameLanguage() {
    const std::string saved = g_app.launcher.Get("language");
    if (EuEdition()) {
        for (const auto& c : kGameLanguagesEu)
            if (saved == c.value) return saved;
        if (saved == "20") return "3"; // Spanish: Spain's, the one this build has
        if (saved == "1") return "18"; // English: the UK one
        return DefaultGameLanguage();
    }
    for (const auto& c : kGameLanguages)
        if (saved == c.value) return saved;
    if (saved == "3") return "20"; // Spanish: the Latin American one this build has
    return DefaultGameLanguage();
}

void SetEnv(const wchar_t* name, const std::string& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : Widen(value).c_str());
}

void StartGame() {
    App& a = g_app;
    if (!a.python || a.game_running) return;
    SaveGameIni();
    SaveLauncher();
    // Friendly pre-check (run.py refuses the same way): no cold-cache ultrawide
    // launch that could only end in an error code.
    if (GameAspect() != "16:9" && !UltrawideCacheWarm()) {
        a.notice = T("Primero juega una vez en 16:9 hasta el menú y ciérralo normal; "
                     "entonces se desbloquea el ultra-panorámico.",
                     "First play once in 16:9 up to the menu and close normally; "
                     "ultrawide unlocks then.");
        return;
    }
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
    // The Controls tab's sticks settings (src/runtime_pad.c); the game gets the controllers alone.
    SetEnv(L"BB_PAD_DEADZONE", a.launcher.Get("pad_deadzone"));
    SetEnv(L"BB_PAD_DEADZONE_OUTER", a.launcher.Get("pad_deadzone_outer"));
    SetEnv(L"BB_PAD_CENTER_CAL", a.launcher.Flag("pad_center_cal", true) ? "" : "0");
    Pads::Stop();
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
    ShowWindow(a.hwnd, SW_MINIMIZE);
}

void StopGame() {
    g_app.stopped = true;
    if (g_app.job_object) TerminateJobObject(g_app.job_object, 1);
    else if (g_app.process) TerminateProcess(g_app.process, 1);
}

/// Called every frame: picks up finished background work.
void PollJobs() {
    App& a = g_app;
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
        a.saves_dirty = true;
        ShowWindow(a.hwnd, SW_RESTORE);
        SetForegroundWindow(a.hwnd);
    }
}

void StartProbe() {
    App& a = g_app;
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
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
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
// Patches: every eboot patch for the game's title (CUSA13795 or CUSA14876) 01.21 in patches\*.xml (shadPS4/GoldHEN XML format).
// The three community patches keep their bbport.ini keys (shared with the in-game menu); others
// are switched in patches.json ({"enabled": [...], "disabled": [...]}), as scripts/patches.py reads.



constexpr std::pair<const char*, const char*> kIniPatches[] = {
    {"CrashTeamRacing.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacing.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
};

std::string XmlUnescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
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

std::vector<PatchInfo> LoadPatches() {
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
        for (size_t at = 0; (at = xml.find("<ID>", at)) != std::string::npos; at += 4) {
            const size_t close = xml.find("</ID>", at);
            if (close == std::string::npos) break;
            any_id = true;
            if (Trim(xml.substr(at + 4, close - at - 4)) == GameTitleId()) ours = true;
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
            // The EU edition's bundled file shares CrashTeamRacing.xml's keys (scripts/patches.py
            // selection_key), so the in-game menu and patches.json switch the same patches.
            const std::string file_name = PathText(file.filename());
            p.key = (file_name == "CrashTeamRacingEU.xml" ? std::string("CrashTeamRacing.xml") : file_name) + "/" + p.name;
            p.author = get("Author");
            p.note = get("Note");
            p.version = get("PatchVer");
            std::string on = get("isEnabled");
            std::transform(on.begin(), on.end(), on.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            p.default_on = on == "true";
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

// v0.4 look: a dark theme with the Nitro orange as accent (ApplyTheme), Segoe UI for text,
// Bahnschrift for titles and Segoe Fluent Icons / MDL2 Assets merged in for the icons below.
const ImVec4 kGreen{0.42f, 0.86f, 0.52f, 1.0f};
const ImVec4 kRed{1.0f, 0.42f, 0.40f, 1.0f};
const ImVec4 kYellow{1.0f, 0.78f, 0.30f, 1.0f};
const ImVec4 kGrey{0.58f, 0.61f, 0.70f, 1.0f};
const ImVec4 kText{0.92f, 0.93f, 0.96f, 1.0f};
const ImVec4 kAccent{1.0f, 0.54f, 0.12f, 1.0f};        // #FF8A1F
const ImVec4 kAccentHover{1.0f, 0.63f, 0.26f, 1.0f};
const ImVec4 kAccentActive{0.91f, 0.45f, 0.06f, 1.0f};
const ImVec4 kBlue{0.30f, 0.64f, 1.0f, 1.0f};
const ImVec4 kBg{0.067f, 0.075f, 0.102f, 1.0f};         // #11131A
const ImVec4 kPanel{0.098f, 0.110f, 0.149f, 1.0f};      // #191C26
const ImVec4 kCard{0.118f, 0.133f, 0.188f, 1.0f};       // #1E2230
const ImVec4 kFrame{0.149f, 0.169f, 0.231f, 1.0f};      // #262B3B
const ImVec4 kFrameHover{0.184f, 0.208f, 0.314f, 1.0f};
const ImVec4 kFrameActive{0.220f, 0.251f, 0.388f, 1.0f};
const ImVec4 kBorder{0.173f, 0.192f, 0.259f, 1.0f};

// Segoe Fluent Icons / Segoe MDL2 Assets code points, as UTF-8.
#define ICON_PLAY "\xEE\x9D\xA8"      // E768
#define ICON_SETTINGS "\xEE\x9C\x93"  // E713
#define ICON_GAMEPAD "\xEE\x9F\xBC"   // E7FC
#define ICON_KEYBOARD "\xEE\x9D\xA5"  // E765
#define ICON_PUZZLE "\xEE\xAA\x86"    // EA86
#define ICON_SAVE "\xEE\x9D\x8E"      // E74E
#define ICON_PACKAGE "\xEE\x9E\xB8"   // E7B8
#define ICON_TOOLS "\xEE\xA4\x8F"     // E90F
#define ICON_FOLDER "\xEE\xA2\xB7"    // E8B7
#define ICON_REFRESH "\xEE\x9C\xAC"   // E72C
#define ICON_CHECK "\xEE\x9C\xBE"     // E73E
#define ICON_WARN "\xEE\x9E\xBA"      // E7BA
#define ICON_INFO "\xEE\xA5\x86"      // E946
#define ICON_STOP "\xEE\x9C\x9A"      // E71A
#define ICON_GLOBE "\xEE\x9D\xB4"     // E774
#define ICON_BLUETOOTH "\xEE\x9C\x82" // E702
#define ICON_USB "\xEE\xA2\x8E"       // E88E
#define ICON_CANCEL "\xEE\x9C\x91"    // E711
#define ICON_UNDO "\xEE\x9E\xA7"      // E7A7
#define ICON_VIBRATE "\xEE\xA1\xB7"   // E877
#define ICON_PEOPLE "\xEE\x9C\x96"    // E716
#define ICON_DISPLAY "\xEE\x9F\xB4"   // E7F4
#define ICON_DOWNLOAD "\xEE\xA2\x96"  // E896
#define ICON_STAR "\xEE\x9C\xB4"      // E734

ImFont* g_font_heading = nullptr;  // Segoe UI Semibold (section titles)
ImFont* g_font_title = nullptr;    // Bahnschrift (the header, the Play button)
float g_ui_scale = 1.0f;
ID3D11ShaderResourceView* g_logo = nullptr;  // the app icon (LoadLogo)
const char* g_goto_tab = nullptr;             // a tab to bring to front on the next frame
void PlaySummary();
std::string EditionLabel();

/// A section title: the semibold font in the accent colour over a separator line.
void Section(const char* label) {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (g_font_heading) ImGui::PopFont();
}

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

void ApplyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18 * scale, 14 * scale);
    st.FramePadding = ImVec2(10 * scale, 6 * scale);
    st.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    st.ItemInnerSpacing = ImVec2(8 * scale, 6 * scale);
    st.CellPadding = ImVec2(8 * scale, 5 * scale);
    st.ScrollbarSize = 12 * scale;
    st.GrabMinSize = 12 * scale;
    st.WindowRounding = 0;
    st.ChildRounding = 8 * scale;
    st.FrameRounding = 6 * scale;
    st.PopupRounding = 8 * scale;
    st.ScrollbarRounding = 8 * scale;
    st.GrabRounding = 6 * scale;
    st.TabRounding = 6 * scale;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 1;
    st.TabBarOverlineSize = 2 * scale;
    st.SeparatorTextBorderSize = 2 * scale;
    st.SeparatorTextPadding = ImVec2(14 * scale, 4 * scale);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.56f, 1.0f);
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kPanel;
    c[ImGuiCol_TitleBgActive] = kPanel;
    c[ImGuiCol_MenuBarBg] = kPanel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
    c[ImGuiCol_ScrollbarGrabActive] = kAccentActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = ImVec4(1.0f, 0.54f, 0.12f, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.54f, 0.12f, 0.30f);
    c[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.54f, 0.12f, 0.42f);
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_SeparatorHovered] = kAccentHover;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = kPanel;
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = kCard;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kPanel;
    c[ImGuiCol_TabDimmedSelected] = kCard;
    c[ImGuiCol_TabDimmedSelectedOverline] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    c[ImGuiCol_TableHeaderBg] = kFrame;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.17f, 0.23f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextLink] = kBlue;
    c[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 0.54f, 0.12f, 0.35f);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

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

void PlayTab() {
    App& a = g_app;
    const bool busy = a.job != Job::None;
    BeginCard("game");
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted((std::string(ICON_FOLDER "  ") + T("Tu juego", "Your game")).c_str());
    if (g_font_heading) ImGui::PopFont();

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
            ImGui::TextColored(kGreen, "%s  %s %s", ICON_CHECK, T("Listo:", "Ready:"), Describe(a.game_info).c_str());
            const std::string edition = EditionLabel();
            if (!edition.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(kBlue, "·  %s", edition.c_str());
            }
        } else {
            ImGui::TextColored(kRed, "%s", T("Estos archivos no sirven:", "These files do not work:"));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(a.check_message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextColored(kGrey, "%s", PathText(a.game_dir).c_str());
        if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Cambiar carpeta del juego...", "Change game folder...")).c_str()) &&
            !busy && !a.game_running) {
            ChooseFolder(a.game_dir, a.game_info, L"CUSA13795 + 1.21");
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Abrir carpeta", "Open folder"))) OpenInShell(a.game_dir);
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
        ImGui::TextUnformatted(T("Necesitas tu propio dump de PS4: el juego base (CUSA13795) y la actualización 1.21 "
                                 "(CUSA13795-patch; o la edición europea CUSA14876 con su 1.21). Si ya los tienes juntos en una carpeta, elige esa.",
                                 "You need your own PS4 dump: the base game (CUSA13795) and the 1.21 update "
                                 "(CUSA13795-patch; or the European edition CUSA14876 with its 1.21). If they are already merged in one folder, "
                                 "choose that one."));
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

    EndCard();

    if (ready && !a.game_running) {
        ImGui::Spacing();
        BeginCard("summary");
        if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextUnformatted((std::string(ICON_DISPLAY "  ") + T("Así arrancará", "It will start with")).c_str());
        if (g_font_heading) ImGui::PopFont();
        PlaySummary();
        EndCard();
    }

    ImGui::Spacing();
    const float play_height = ImGui::GetFontSize() * 3.2f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * g_ui_scale);
    ImGui::PushFont(g_font_title, ImGui::GetStyle().FontSizeBase * 1.7f);
    if (a.game_running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.26f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.16f, 0.14f, 1.0f));
        if (ImGui::Button((std::string(ICON_STOP "  ") + T("CERRAR EL JUEGO", "STOP THE GAME")).c_str(),
                          ImVec2(-FLT_MIN, play_height)))
            StopGame();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::BeginDisabled(!ready || busy || !a.python);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.04f, 1.0f));
        if (ImGui::Button((std::string(ICON_PLAY "  ") + T("JUGAR", "PLAY")).c_str(), ImVec2(-FLT_MIN, play_height)))
            StartGame();
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();

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
        else ImGui::TextColored(kRed, T("El juego se cerró con un error (código %lu). Mira el registro abajo o en last_run.log.",
                                        "The game ended with an error (code %lu). See the log below or last_run.log."),
                                (unsigned long)a.exit_code);
    }
    if (!status.empty()) ImGui::TextColored(kGrey, "%s", status.c_str());

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Registro", "Log"), a.game_running || a.show_exit ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("log", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 12.0f)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleColor();
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

/// v0.3 native ultrawide (by tcosi): bbport.ini aspect_ratio, Settings > Display.
/// 16:9 off; 21:9 needs a 2560x1080/3440x1440 output; 32:9 needs 5120x1440.
std::string GameAspect() {
    const std::string v = g_app.game.Get("aspect_ratio", "16:9");
    if (v == "21:9" || v == "21x9") return "21:9";
    if (v == "32:9" || v == "32x9") return "32:9";
    return "16:9";
}

bool IsUltrawideKey(const std::string& key) {
    return key == "CTR-UltraWide21x9-v1.0.xml/21:9 UltraWide" ||
           key == "CTR-SuperUltraWide32x9-v1.0.xml/32:9 Super UltraWide";
}

/// v0.3: the ultrawide options unlock only with a warm HUD shader cache
/// (user/cache/CUSA13795 from a normal 16:9 launch to the menu), mirroring
/// scripts/ultrawide_*_shader.py require_warm_cache. Otherwise every new player
/// would hit the launch-time refusal on their first ultrawide try.
bool UltrawideCacheWarm() {
    const fs::path cache = g_app.root / L"user" / L"cache" / GameTitle();
    std::error_code ec;
    auto present = [&](const wchar_t* name) {
        std::error_code e2;
        return fs::is_regular_file(cache / name, e2) && fs::file_size(cache / name, e2) > 0;
    };
    if (!present(L"profile.bin") || !present(L"0x00000000d4aaf527_0.spv") ||
        !present(L"0x0000000008948e32_0.spv")) {
        return false;
    }
    bool meta = false, key = false;
    for (fs::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec)) {
        const auto ext = it->path().extension();
        if (ext == L".meta") meta = true;
        if (ext == L".key") key = true;
    }
    return meta && key;
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

void SettingsTab() {
    App& a = g_app;
    ImGui::TextColored(kGrey, "%s", T("Se aplican al pulsar Jugar. El menú del juego (Insert o L3+R3) cambia los mismos ajustes.",
                                      "Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings."));

    Section(T("Rendimiento", "Performance"));
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

    Section(T("Pantalla", "Display"));
    {
        // v0.3 native (by tcosi): ONE dropdown sets aspect + output together, so they
        // can never mismatch. The two ultrawide XMLs stay hidden in Patches.
        struct AspectOutput { const char* es; const char* en; const char* aspect; const char* output; };
        static constexpr AspectOutput kOptions[] = {
            {"16:9 — 1280 x 720", "16:9 — 1280 x 720", "16:9", "1280x720"},
            {"16:9 — 1920 x 1080", "16:9 — 1920 x 1080", "16:9", "1920x1080"},
            {"16:9 — 2560 x 1440", "16:9 — 2560 x 1440", "16:9", "2560x1440"},
            {"16:9 — 3840 x 2160 (4K)", "16:9 — 3840 x 2160 (4K)", "16:9", "3840x2160"},
            {"21:9 UltraWide — 2560 x 1080", "21:9 UltraWide — 2560 x 1080", "21:9", "2560x1080"},
            {"21:9 UltraWide — 3440 x 1440", "21:9 UltraWide — 3440 x 1440", "21:9", "3440x1440"},
            {"32:9 Super UltraWide — 5120 x 1440", "32:9 Super UltraWide — 5120 x 1440", "32:9", "5120x1440"},
        };
        const std::string aspect = GameAspect();
        const std::string output = g_app.game.Get("output_res", "1920x1080");
        const AspectOutput* selected = &kOptions[1];
        for (const auto& o : kOptions) {
            if (output == o.output && aspect == o.aspect) { selected = &o; break; }
        }
        if (selected->output != output) {
            for (const auto& o : kOptions) {
                if (output == o.output) { selected = &o; break; }
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        // The ultrawide rows unlock only with a warm shader cache (a normal 16:9
        // launch to the menu first); otherwise every new player would hit the
        // launch-time refusal. run.py keeps the same guard as a safety net.
        const bool warm = UltrawideCacheWarm();
        if (!warm) {
            ImGui::TextColored(kYellow, "%s",
                               T("El ultra-panorámico se desbloquea tras jugar una vez en 16:9 hasta el menú.",
                                 "Ultrawide unlocks after playing once in 16:9 up to the menu."));
        }
        if (ImGui::BeginCombo(T("Aspecto / salida", "Aspect / output"), T(selected->es, selected->en))) {
            for (const auto& o : kOptions) {
                const bool locked = std::strcmp(o.aspect, "16:9") != 0 && !warm;
                ImGui::BeginDisabled(locked);
                if (ImGui::Selectable(T(o.es, o.en), &o == selected)) {
                    a.game.Set("aspect_ratio", o.aspect);
                    a.game.Set("output_res", o.output);
                    SaveGameIni();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Una sola opción: aspecto y salida juntos, con HUD centrado en ultra-panorámico. "
                     "Mientras hay ultra-panorámico el escalado se desactiva y la resolución interna se limita a 1440p. "
                     "La primera vez prepara unos 9 GB de archivos; tus archivos originales no cambian. "
                     "Antes de usarlo, una vez en 16:9: 1) JUGAR hasta el menú principal y quédate unos segundos "
                     "(entra a Opciones y sal); 2) cierra el juego desde su menú, no lo mates ni uses Alt+F4, "
                     "para que la caché se guarde. Si al lanzar el ultra te pide caché igualmente, entra a una "
                     "carrera 30 segundos y sal: eso compila el segundo shader del HUD.",
                     "One choice: aspect and output together, with centered HUD in ultrawide. "
                     "Upscaling is off while ultrawide is on and internal resolution is capped at 1440p. "
                     "The first run prepares about 9 GB of files; your original files are kept. "
                     "Before using it, once in 16:9: 1) PLAY to the main menu and stay a few seconds "
                     "(open Options and exit); 2) close the game from its menu, do not kill it nor Alt+F4, "
                     "so the cache is saved. If launching ultrawide still asks for cache, enter a race "
                     "for 30 seconds and exit: that compiles the second HUD shader."));
    }
    GameFlag("msaa", T("MSAA del juego (2x)", "Game MSAA (2x)"), false,
             T("El antialiasing 2x de la PS4. Desactivado por defecto: en PC dibuja líneas punteadas en el "
               "cielo y en los rayos de luz. A resoluciones altas o con FSR/DLSS casi no se nota. "
               "Requiere reiniciar.",
               "The PS4's 2x anti-aliasing. Off by default: on PC it draws dotted lines in the sky and "
               "in light shafts. At high internal resolutions or with FSR/DLSS it is barely missed. "
               "Needs a restart."));
    const std::string temporal = GameTemporal();
    const std::string aspect_now = GameAspect();
    const bool uw_on = aspect_now != "16:9";
    const bool dlss_sized = temporal == "dlss" && a.game.Get("dlss_quality", "auto") != "auto";
    ImGui::BeginDisabled(dlss_sized);
    RenderResCombo();
    ImGui::EndDisabled();
    if (dlss_sized) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "%s", T("(la decide la calidad de DLSS)", "(set by the DLSS quality)"));
    }
    {
        // temporal=off|fsr1|fsr3|dlss|fsr4 (bbport.ini): one upscaler at a time.
        // v0.3: off while ultrawide is on (run.py forces it).
        static constexpr Choice kTemporal[] = {{"Desactivado", "Off", "off"},
                                               {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                               {"AMD FSR 3", "AMD FSR 3", "fsr3"},
                                               {"AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)", "fsr4"},
                                               {"NVIDIA DLSS", "NVIDIA DLSS", "dlss"}};
        ImGui::BeginDisabled(uw_on);
        if (GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporal, temporal.c_str(), 16.0f)) {
            a.game.Erase("fsr1");
            a.game.Erase("fsr3");
            SaveGameIni();
        }
        ImGui::EndDisabled();
        if (uw_on) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", T("(desactivado con ultra-panorámico)", "(off with ultrawide)"));
        }
        HelpMarker(T("FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado simple del juego. "
                     "FSR 3, FSR 4 y DLSS reconstruyen el detalle a lo largo de varios cuadros con la profundidad y el movimiento "
                     "del juego, y a la misma resolución también suavizan los bordes. DLSS necesita una tarjeta NVIDIA "
                     "RTX (si no, se usa FSR 3); FSR 4 necesita soporte INT8 y sus modelos (si no, se usa FSR 3). "
                     "FSR 4 es experimental. Si ves estelas o parpadeos, cambia a FSR 1.",
                     "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3, FSR 4 and DLSS "
                     "rebuild detail over several frames from the game's depth and motion, and smooth edges even at the "
                     "same resolution. DLSS needs an NVIDIA RTX card (FSR 3 runs otherwise); FSR 4 needs INT8 support "
                     "and its model files (FSR 3 runs otherwise). FSR 4 is experimental. Switch to "
                     "FSR 1 if you see trails or shimmering."));
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
    if (temporal == "fsr3") {
        float sharp = float(std::atof(a.game.Get("fsr3_sharpness", "0.30").c_str()));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderFloat(T("Nitidez de FSR 3", "FSR 3 sharpness"), &sharp, 0.0f, 1.0f, "%.2f")) {
            char text[16];
            std::snprintf(text, sizeof text, "%.2f", sharp);
            a.game.Set("fsr3_sharpness", text);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
    }
    GameFlag("fullscreen", T("Pantalla completa", "Fullscreen"), false);
    GameFlag("show_fps", T("Contador de FPS en la esquina", "FPS counter in the corner"), false);
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
    {
        // framegen=off|fsr3 (bbport.ini): AMD FSR 3 frame generation, one interpolated
        // frame between each pair of game frames. Live in game (Insert menu) too.
        static constexpr Choice kFramegen[] = {{"Desactivada", "Off", "off"},
                                               {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"}};
        std::string current = g_app.game.Get("framegen", "off");
        if (current == "fsr" || current == "on" || current == "1") current = "fsr3"; // test builds
        const Choice* selected = &kFramegen[0];
        for (const auto& c : kFramegen)
            if (current == c.value) selected = &c;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::BeginCombo(T("Generación de cuadros", "Frame generation"), T(selected->es, selected->en))) {
            for (const auto& c : kFramegen) {
                if (ImGui::Selectable(T(c.es, c.en), &c == selected)) {
                    a.game.Set("framegen", c.value);
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Muestra un cuadro interpolado entre cada par de cuadros del juego: casi el doble en pantalla. "
                     "El juego, su límite de FPS y su ritmo no cambian. Ideal en monitores de 120 Hz o más con el juego "
                     "a la mitad de la frecuencia (p. ej. límite 72 en 144 Hz). Funciona con cualquier escalado; FSR 3/4 "
                     "y DLSS le dan además el movimiento del juego. Experimental: el HUD rápido puede vibrar.",
                     "Shows an interpolated frame between each pair of game frames: about twice the frames on screen. "
                     "The game, its FPS limit and its timing do not change. Best on 120 Hz+ screens with the game at "
                     "half the refresh rate (e.g. a 72 limit at 144 Hz). Works with any upscaler; FSR 3/4 and DLSS "
                     "also give it the game's motion. Experimental: fast HUD elements may shimmer."));
        if (current != "off") {
            static constexpr Choice kMotion[] = {
                {"Flujo óptico (recomendado)", "Optical flow (recommended)", "flow"},
                {"Movimiento del juego + flujo óptico", "Game motion + optical flow", "game"}};
            const std::string motion = g_app.game.Get("framegen_motion", "flow");
            const Choice* chosen = &kMotion[0];
            for (const auto& c : kMotion)
                if (motion == c.value) chosen = &c;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            if (ImGui::BeginCombo(T("Movimiento para generar", "Frame generation motion"),
                                  T(chosen->es, chosen->en))) {
                for (const auto& c : kMotion) {
                    if (ImGui::Selectable(T(c.es, c.en), &c == chosen)) {
                        a.game.Set("framegen_motion", c.value);
                        SaveGameIni();
                    }
                }
                ImGui::EndCombo();
            }
            HelpMarker(T("El flujo óptico sigue lo que se mueve en pantalla, karts incluidos. El movimiento del juego "
                         "(con FSR 3/4 o DLSS) mueve los karts con la cámara y puede emborronarlos.",
                         "Optical flow follows what moves on screen, karts included. The game's motion (with FSR 3/4 "
                         "or DLSS) moves karts with the camera and can smear them."));
        }
    }

    Section(T("Mando", "Controller"));
    ImGui::TextColored(kGrey, "%s  %s", ICON_GAMEPAD,
                       T("Los mandos, los jugadores y los botones están en la pestaña Controles.",
                         "Controllers, players and buttons are in the Controls tab."));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Ir a Controles", "Open Controls"))) g_goto_tab = "controls";

    Section(T("Consola", "Console"));
    if (a.launcher.Get("language") != GameLanguage()) a.launcher.Set("language", GameLanguage());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (EuEdition()) {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguagesEu, DefaultGameLanguage());
        HelpMarker(T("Edición europea (CUSA14876): los idiomas de la versión PAL. Si el juego no trae uno, sale en "
                     "inglés. Se aplica al pulsar Jugar.",
                     "European edition (CUSA14876): the PAL release's languages. A language the game lacks shows "
                     "English. Applied when you press Play."));
    } else {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguages, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae esta versión del juego (CUSA13795, América): inglés, español "
                     "latinoamericano, francés y portugués de Brasil. El español de España no está en ella. Se aplica "
                     "al pulsar Jugar.",
                     "The languages this build of the game has (CUSA13795, the Americas): English, Latin American "
                     "Spanish, French and Brazilian Portuguese. Spain's Spanish is not in it. Applied when you press "
                     "Play."));
    }
}

void PatchesTab() {
    App& a = g_app;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Se aplican al iniciar el juego (o con \"Apply and restart\" del menú del juego).",
                                      "Applied when the game starts (or with the in-game menu's \"Apply and restart\")."));
    ImGui::TextColored(kGrey, "%s", T("El ultra-panorámico 21:9/32:9 está en Ajustes > Pantalla > Aspecto / salida.",
                                      "Ultrawide 21:9/32:9 is in Settings > Display > Aspect / output."));
    ImGui::PopTextWrapPos();
    const bool neo = a.launcher.Flag("neo", false);
    if (a.patches.empty()) {
        ImGui::TextColored(kYellow, "%s", T("No hay parches en la carpeta patches.", "No patches in the patches folder."));
    }
    for (const auto& p : a.patches) {
        ImGui::PushID(p.key.c_str());
        bool on;
        if (p.ini_key) {
            on = a.game.Flag(p.ini_key, false);
        } else {
            const bool listed_on = std::find(a.patch_enabled.begin(), a.patch_enabled.end(), p.key) != a.patch_enabled.end();
            const bool listed_off = std::find(a.patch_disabled.begin(), a.patch_disabled.end(), p.key) != a.patch_disabled.end();
            on = listed_on || (p.default_on && !listed_off);
        }
        const bool needs_neo = p.ini_key && std::strcmp(p.ini_key, "patch_pro_1080p") == 0;
        // v0.3 native: ultrawide lives in Settings > Display > Aspect ratio (aspect_ratio),
        // not here. Old patches.json toggles still work until an aspect is picked.
        const bool is_ultrawide = IsUltrawideKey(p.key);
        if (needs_neo || is_ultrawide) {  // replaced by Settings
            ImGui::PopID();
            continue;
        }
        // render_res decides the 1080p Pro patch (Settings > Internal resolution).
        if (needs_neo) on = EffectiveRenderRes() == "1080" && neo;
        ImGui::BeginDisabled(needs_neo);
        if (ImGui::Checkbox(p.name.c_str(), &on)) {
            if (p.ini_key) {
                a.game.Set(p.ini_key, on ? "1" : "0");
                SaveGameIni();
            } else {
                auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), p.key), v.end()); };
                drop(a.patch_enabled);
                drop(a.patch_disabled);
                if (on) a.patch_enabled.push_back(p.key);
                else if (p.default_on) a.patch_disabled.push_back(p.key);
                WritePatchConfig(a.patch_enabled, a.patch_disabled);
            }
        }
        ImGui::EndDisabled();
        if (!p.author.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s %s%s%s", T("de", "by"), p.author.c_str(), p.version.empty() ? "" : "  v",
                               p.version.c_str());
        }
        ImGui::Indent();
        ImGui::PushTextWrapPos(0.0f);
        if (needs_neo) ImGui::TextColored(kGrey, "%s", T("Lo controla Ajustes > Resolución interna (1080p con el modo PS4 Pro).",
                                                          "Controlled by Settings > Internal resolution (1080p with PS4 Pro mode)."));
        if (!p.note.empty()) ImGui::TextColored(kGrey, "%s", p.note.c_str());
        ImGui::TextColored(kGrey, "%s", p.key.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
        ImGui::PopID();
    }

    Section(T("Añadir parches", "Add patches"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Copia archivos .xml de parches en formato shadPS4 (como los de PS-Game-Patch o GoldHEN) en la "
                             "carpeta patches y pulsa Recargar. Solo aparecen los que son para CUSA13795 versión 01.21.",
                             "Copy patch .xml files in shadPS4 format (like PS-Game-Patch's or GoldHEN's) into the patches "
                             "folder and press Reload. Only those for CUSA13795 version 01.21 are listed."));
    ImGui::TextColored(kGrey, "%s", T("Por ahora no existen trucos públicos para CTR Nitro-Fueled (como desbloquear todos los "
                                      "personajes). Para tener el juego desbloqueado puedes importar una partida en la pestaña "
                                      "Guardados.",
                                      "There are no public cheats for CTR Nitro-Fueled yet (like unlocking every character). To "
                                      "have the game unlocked you can import a save in the Saves tab."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(T("Abrir la carpeta de parches", "Open the patches folder"))) {
        std::error_code ec;
        fs::create_directories(a.root / L"patches", ec);
        OpenInShell(a.root / L"patches");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Recargar", "Reload"))) {
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
    Section(T("Partidas", "Saves"));
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

    Section(T("Copias de seguridad", "Backups"));
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

    Section(T("DLC encontrados", "DLC found"));
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
                               "tecla; cada acción admite hasta 4 teclas. Se guarda en ctrnfpc.ini y vale desde el próximo arranque.",
                               "Player 1's keys. The keyboard works together with the controller. Press \"Change\", then the key; "
                               "each action takes up to 4 keys. Saved in ctrnfpc.ini, used from the next start."));
    ImGui::TextColored(kGrey, "%s", T("Insert abre el menú del juego y Esc cancela, por eso no se pueden asignar.",
                                      "Insert opens the in-game menu and Esc cancels, so they cannot be bound."));
    const int capturing = g_key_capture;
    if (capturing >= 0) {
        const unsigned got = g_key_captured.exchange(0);
        if (got) {
            const unsigned code = got - 1;
            if (code == 0x01) {
                g_key_capture = -1;  // Esc: cancel
            } else if (const char* name = SdlKeyName(code); name && code != 0x152) {
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

    Section(T("Controles", "Controls"));
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Teclas", "Keys"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
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
                ImGui::TextColored(kYellow, "%s", T("Pulsa una tecla... (Esc cancela)", "Press a key... (Esc cancels)"));
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
// Controls tab (v0.4). The connected controllers through the game's own SDL3 (gamepads.cpp), so
// names, GUIDs and buttons are what src/runtime_pad.c sees: a live test, the players'
// controllers (bbport.ini port.1-4, as the in-game menu writes them; player 1 also launcher.ini
// gamepad = BB_GAMEPAD), the gamepad buttons (bbport.ini pad.<input>) and the sticks' dead zone
// (launcher.ini pad_deadzone*, passed as BB_PAD_DEADZONE*). The keyboard is its second page.

uint32_t g_pad_selected = 0;
int g_pad_capture = -1;  // kKeyInputs index waiting for a gamepad button, -1 none
bool g_pad_replace = false;
double g_pads_retry = 0.0;

// runtime_pad.c bind_defaults() per kKeyInputs id; nullptr: not a gamepad binding (the sticks).
const char* PadFallback(const std::string& id) {
    static const std::map<std::string, const char*> defaults = {
        {"cross", "a"},           {"circle", "b"},         {"square", "x"},           {"triangle", "y"},
        {"l1", "leftshoulder"},   {"r1", "rightshoulder"}, {"l2", "lefttrigger"},     {"r2", "righttrigger"},
        {"l3", "leftstick"},      {"r3", "rightstick"},    {"options", "start"},      {"touchpad", "back,touchpad"},
        {"touchpad_right", ""},   {"up", "dpup"},          {"down", "dpdown"},        {"left", "dpleft"},
        {"right", "dpright"},
    };
    const auto it = defaults.find(id);
    return it == defaults.end() ? nullptr : it->second;
}

void LeaveControls() {
    if (g_key_capture >= 0) g_key_capture = -1;
    if (g_pad_capture >= 0) {
        g_pad_capture = -1;
        Pads::CancelCapture();
    }
}

/// SDL's gamepads while the launcher shows them; never while the game runs (it has them).
bool EnsurePads() {
    App& a = g_app;
    if (a.game_running) {
        Pads::Stop();
        return false;
    }
    if (Pads::Running()) return true;
    const double now = ImGui::GetTime();
    if (now < g_pads_retry) return false;
    if (!Pads::Start((a.bin / L"SDL3.dll").wstring())) {
        g_pads_retry = now + 3.0;
        return false;
    }
    return true;
}

/// The controller of player p (1..4): a GUID, or "" for automatic (p = 1) / nobody.
std::string PlayerPad(int p) {
    std::string v = g_app.game.Get("port." + std::to_string(p));
    if (p == 1 && v.empty()) v = g_app.launcher.Get("gamepad");
    return v;
}

void SetPlayerPad(int p, const Pads::Info* pad) {
    App& a = g_app;
    a.game.Set("port." + std::to_string(p), pad ? pad->guid : "");
    SaveGameIni();
    if (p == 1) {
        a.launcher.Set("gamepad", pad ? pad->guid : "");
        a.launcher.Set("gamepad_name", pad ? pad->name : "");
    }
    a.launcher.Set("port_name." + std::to_string(p), pad ? pad->name : "");
    SaveLauncher();
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

/// A drawing of the controller with what is pressed now (the raw input, before any remapping).
void DrawPad(const Pads::Info& info, const Pads::State& st, float dz_inner, float dz_outer) {
    using namespace Pads;
    const bool ps = info.family == Family::PlayStation;
    const float W = std::min(ImGui::GetContentRegionAvail().x, 430.0f * g_ui_scale);
    const float H = W * 0.56f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(o.x + x * W, o.y + y * W); };
    auto R = [&](float r) { return r * W; };
    const ImU32 body = Col(kFrame), edge = Col(kBorder), idle = Col(ImVec4(0.24f, 0.27f, 0.37f, 1.0f)),
                on = Col(kAccent), text = Col(kText), dim = Col(kGrey);
    auto centered = [&](ImVec2 c, const std::string& label, ImU32 color) {
        const ImVec2 t = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), color, label.c_str());
    };
    // Body and grips.
    dl->AddCircleFilled(P(0.235f, 0.405f), R(0.125f), body, 48);
    dl->AddCircleFilled(P(0.765f, 0.405f), R(0.125f), body, 48);
    dl->AddRectFilled(P(0.11f, 0.115f), P(0.89f, 0.43f), body, R(0.095f));
    // Triggers (analog fill) and shoulders.
    auto trigger = [&](float x0, float x1, int axis, int button) {
        const float v = std::clamp(st.axes[axis] / 32767.0f, 0.0f, 1.0f);
        dl->AddRectFilled(P(x0, 0.0f), P(x1, 0.04f), idle, R(0.012f));
        if (v > 0.01f) dl->AddRectFilled(P(x0, 0.0f), P(x0 + (x1 - x0) * v, 0.04f), on, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.02f), Label(button, info.family, info.type, g_spanish), text);
    };
    trigger(0.14f, 0.34f, AxTriggerLeft, BtnLeftTrigger);
    trigger(0.66f, 0.86f, AxTriggerRight, BtnRightTrigger);
    auto shoulder = [&](float x0, float x1, int button) {
        dl->AddRectFilled(P(x0, 0.055f), P(x1, 0.095f), st.buttons[button] ? on : idle, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.075f), Label(button, info.family, info.type, g_spanish), text);
    };
    shoulder(0.14f, 0.34f, BtnLeftShoulder);
    shoulder(0.66f, 0.86f, BtnRightShoulder);
    // D-pad and left stick: PlayStation pads have the d-pad on top, the others the stick.
    const ImVec2 dpad = ps ? ImVec2(0.25f, 0.25f) : ImVec2(0.37f, 0.37f);
    const ImVec2 lstick = ps ? ImVec2(0.37f, 0.37f) : ImVec2(0.25f, 0.25f);
    {
        const float a = 0.027f;
        const struct { float dx, dy; int b; } arms[] = {
            {0, -2 * a, BtnDpadUp}, {0, 2 * a, BtnDpadDown}, {-2 * a, 0, BtnDpadLeft}, {2 * a, 0, BtnDpadRight}};
        dl->AddRectFilled(P(dpad.x - a, dpad.y - a), P(dpad.x + a, dpad.y + a), idle);
        for (const auto& arm : arms)
            dl->AddRectFilled(P(dpad.x + arm.dx - a, dpad.y + arm.dy - a), P(dpad.x + arm.dx + a, dpad.y + arm.dy + a),
                              st.buttons[arm.b] ? on : idle, R(0.006f));
    }
    // Face buttons, with the shapes or letters of this controller.
    {
        const ImVec2 c(0.75f, 0.25f);
        const float d = 0.058f, r = 0.028f;
        const struct { float dx, dy; int b; } face[] = {
            {0, -d, BtnNorth}, {0, d, BtnSouth}, {-d, 0, BtnWest}, {d, 0, BtnEast}};
        for (const auto& f : face) {
            const ImVec2 at = P(c.x + f.dx, c.y + f.dy);
            const bool down = st.buttons[f.b];
            dl->AddCircleFilled(at, R(r), down ? on : idle, 32);
            const float k = R(r) * 0.48f;
            const ImU32 ink = down ? Col(kBg) : text;
            if (ps) {
                const float th = 1.8f * g_ui_scale;
                if (f.b == BtnNorth) {
                    dl->AddTriangle(ImVec2(at.x, at.y - k), ImVec2(at.x - k, at.y + k * 0.75f), ImVec2(at.x + k, at.y + k * 0.75f),
                                    down ? ink : Col(ImVec4(0.35f, 0.88f, 0.66f, 1.0f)), th);
                } else if (f.b == BtnEast) {
                    dl->AddCircle(at, k, down ? ink : Col(ImVec4(1.0f, 0.45f, 0.45f, 1.0f)), 24, th);
                } else if (f.b == BtnSouth) {
                    const ImU32 blue = down ? ink : Col(ImVec4(0.45f, 0.66f, 1.0f, 1.0f));
                    dl->AddLine(ImVec2(at.x - k, at.y - k), ImVec2(at.x + k, at.y + k), blue, th);
                    dl->AddLine(ImVec2(at.x - k, at.y + k), ImVec2(at.x + k, at.y - k), blue, th);
                } else {
                    dl->AddRect(ImVec2(at.x - k * 0.85f, at.y - k * 0.85f), ImVec2(at.x + k * 0.85f, at.y + k * 0.85f),
                                down ? ink : Col(ImVec4(0.96f, 0.60f, 0.88f, 1.0f)), 0.0f, 0, th);
                }
            } else {
                std::string l = Label(f.b, info.family, info.type, g_spanish);
                if (l.size() > 1) l = l.substr(0, 1);
                centered(at, l, ink);
            }
        }
    }
    // Centre buttons: the touchpad on PlayStation pads.
    if (ps) {
        dl->AddRectFilled(P(0.39f, 0.12f), P(0.61f, 0.27f), st.buttons[BtnTouchpad] ? on : idle, R(0.02f));
        dl->AddRectFilled(P(0.335f, 0.13f), P(0.365f, 0.17f), st.buttons[BtnBack] ? on : idle, R(0.01f));
        dl->AddRectFilled(P(0.635f, 0.13f), P(0.665f, 0.17f), st.buttons[BtnStart] ? on : idle, R(0.01f));
        dl->AddCircleFilled(P(0.5f, 0.32f), R(0.02f), st.buttons[BtnGuide] ? on : idle, 24);
    } else {
        dl->AddCircleFilled(P(0.44f, 0.25f), R(0.017f), st.buttons[BtnBack] ? on : idle, 24);
        dl->AddCircleFilled(P(0.56f, 0.25f), R(0.017f), st.buttons[BtnStart] ? on : idle, 24);
        dl->AddCircleFilled(P(0.5f, 0.16f), R(0.028f), st.buttons[BtnGuide] ? on : idle, 32);
    }
    // Sticks: the dead zone (inner ring) and the full-travel ring, the stick's position.
    auto stick = [&](ImVec2 c, int ax, int ay, int button) {
        const ImVec2 at = P(c.x, c.y);
        const float rad = R(0.068f);
        dl->AddCircleFilled(at, rad, Col(kBg), 40);
        dl->AddCircle(at, rad, edge, 40, 1.5f * g_ui_scale);
        if (dz_inner > 0.0f) dl->AddCircle(at, rad * dz_inner, dim, 32, 1.0f * g_ui_scale);
        if (dz_outer < 0.999f) dl->AddCircle(at, rad * dz_outer, Col(kAccent, 0.45f), 40, 1.0f * g_ui_scale);
        const float x = std::clamp(st.axes[ax] / 32767.0f, -1.0f, 1.0f), y = std::clamp(st.axes[ay] / 32767.0f, -1.0f, 1.0f);
        const ImVec2 dot(at.x + x * rad, at.y + y * rad);
        dl->AddLine(at, dot, Col(kAccent, 0.6f), 2.0f * g_ui_scale);
        dl->AddCircleFilled(dot, R(0.026f), st.buttons[button] ? on : text, 24);
    };
    stick(lstick, AxLeftX, AxLeftY, BtnLeftStick);
    stick(ImVec2(0.63f, 0.37f), AxRightX, AxRightY, BtnRightStick);
    ImGui::Dummy(ImVec2(W, H));
}

void ControllersPage() {
    App& a = g_app;
    const bool sdl = EnsurePads();
    if (sdl) Pads::Update();
    const auto& pads = Pads::List();
    // The same model twice: "#2" tells them apart.
    auto display = [&](const Pads::Info& p) {
        int same = 0, index = 0;
        for (const auto& q : pads) {
            if (q.name == p.name) ++same;
            if (q.id == p.id) index = same;
        }
        return same > 1 ? p.name + " #" + std::to_string(index) : p.name;
    };
    auto players_of = [&](const Pads::Info& p) {
        std::string out;
        for (int n = 1; n <= 4; ++n)
            if (PlayerPad(n) == p.guid) out += (out.empty() ? "" : " ") + std::string(T("J", "P")) + std::to_string(n);
        return out;
    };

    Section(T("Mandos conectados", "Connected controllers"));
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO,
                           T("El juego está abierto y usa los mandos. Ciérralo para probarlos aquí; dentro del juego, "
                             "Insert › Controllers asigna los puertos.",
                             "The game is running and has the controllers. Close it to test them here; in the game, "
                             "Insert › Controllers assigns the ports."));
    } else if (!sdl) {
        ImGui::TextColored(kRed, "%s  %s: %s", ICON_WARN, T("No se pueden leer los mandos", "Controllers cannot be read"),
                           Pads::Error().c_str());
    } else if (pads.empty()) {
        BeginCard("nopads");
        ImGui::TextColored(kGrey, "%s", ICON_GAMEPAD);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", T("No hay mandos conectados. Conecta uno por USB o Bluetooth: aparece aquí al momento. "
                                   "El teclado funciona siempre (página Teclado).",
                                   "No controllers connected. Plug one in over USB or pair it over Bluetooth: it shows "
                                   "up here at once. The keyboard always works (Keyboard page)."));
        EndCard();
    } else {
        bool found = false;
        for (const auto& p : pads) found |= p.id == g_pad_selected;
        if (!found) g_pad_selected = pads.front().id;
        if (ImGui::BeginTable("padlayout", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("test", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float row = ImGui::GetTextLineHeightWithSpacing() * 2.4f;
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const bool selected = p.id == g_pad_selected;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##pad", selected, 0, ImVec2(0, row))) g_pad_selected = p.id;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float pad_x = 10.0f * g_ui_scale, line = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(at.x + pad_x, at.y + row * 0.5f - line * 0.5f), Col(selected ? kAccent : kGrey),
                            ICON_GAMEPAD);
                const float tx = at.x + pad_x + ImGui::GetFontSize() * 2.0f;
                dl->AddText(g_font_heading ? g_font_heading : ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + row * 0.12f), Col(kText),
                            display(p).c_str());
                std::string sub = Pads::TypeName(p.type, g_spanish);
                if (p.wireless) sub += std::string(" · ") + ICON_BLUETOOTH + T(" inalámbrico", " wireless");
                else if (p.wired) sub += std::string(" · ") + ICON_USB + " USB";
                if (p.battery >= 0) sub += " · " + std::to_string(p.battery) + "%" + (p.charging ? T(" cargando", " charging") : "");
                dl->AddText(ImVec2(tx, at.y + row * 0.12f + line * 1.15f), Col(kGrey), sub.c_str());
                const std::string who = players_of(p);
                if (!who.empty()) {
                    const ImVec2 t = ImGui::CalcTextSize(who.c_str());
                    const float right = at.x + ImGui::GetContentRegionAvail().x - pad_x;
                    dl->AddRectFilled(ImVec2(right - t.x - 12 * g_ui_scale, at.y + 6 * g_ui_scale),
                                      ImVec2(right, at.y + 6 * g_ui_scale + t.y + 4 * g_ui_scale), Col(kAccent, 0.25f), 99.0f);
                    dl->AddText(ImVec2(right - t.x - 6 * g_ui_scale, at.y + 8 * g_ui_scale), Col(kAccent), who.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            for (const auto& p : pads) {
                if (p.id != g_pad_selected) continue;
                Pads::State st;
                Pads::Read(p.id, st);
                const int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
                const int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
                DrawPad(p, st, inner / 127.0f, outer / 127.0f);
                ImGui::TextColored(kGrey, T("Stick izq. %+.2f %+.2f   Stick der. %+.2f %+.2f", "Left %+.2f %+.2f   Right %+.2f %+.2f"),
                                   st.axes[Pads::AxLeftX] / 32767.0f, st.axes[Pads::AxLeftY] / 32767.0f,
                                   st.axes[Pads::AxRightX] / 32767.0f, st.axes[Pads::AxRightY] / 32767.0f);
                if (ImGui::Button((std::string(ICON_VIBRATE "  ") + T("Vibrar", "Rumble")).c_str())) {
                    if (!Pads::Rumble(p.id, 450)) a.notice = T("Este mando no admite vibración.", "This controller has no rumble.");
                }
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s", T("Pulsa botones y mueve los sticks para probarlo.",
                                                  "Press buttons and move the sticks to test it."));
            }
            ImGui::EndTable();
        }
    }

    Section(T("Jugadores", "Players"));
    for (int n = 1; n <= 4; ++n) {
        ImGui::PushID(n);
        const std::string current = PlayerPad(n);
        std::string preview = n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                     : T("Nadie", "Nobody");
        if (!current.empty()) {
            preview = a.launcher.Get("port_name." + std::to_string(n));
            if (preview.empty() && n == 1) preview = a.launcher.Get("gamepad_name");
            if (preview.empty()) preview = current;
            bool connected = false;
            for (const auto& p : pads)
                if (p.guid == current && !connected) {
                    preview = display(p);
                    connected = true;
                }
            if (!connected && sdl) preview += T(" (no conectado)", " (not connected)");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((std::string(T("Jugador ", "Player ")) + std::to_string(n)).c_str());
        ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo("##player", preview.c_str())) {
            if (ImGui::Selectable(n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                         : T("Nadie", "Nobody"),
                                  current.empty()))
                SetPlayerPad(n, nullptr);
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const std::string label = display(p) + "  (" + Pads::TypeName(p.type, g_spanish) + ")";
                if (ImGui::Selectable(label.c_str(), p.guid == current)) SetPlayerPad(n, &p);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (n == 1)
            HelpMarker(T("El jugador 1 también usa el teclado. En automático juega el mando que toques: coge cualquiera y pulsa un botón.",
                         "Player 1 also uses the keyboard. On automatic, the controller you touch plays: pick any and press a button."));
        if (n == 2)
            HelpMarker(T("Jugadores 2 a 4: pantalla dividida. Entran al juego mientras su mando está conectado. También se cambia "
                         "dentro del juego (Insert › Controllers).",
                         "Players 2 to 4: split screen. They join the game while their controller is connected. Also set in "
                         "the game (Insert › Controllers)."));
        ImGui::PopID();
    }

    // Gamepad buttons: pad.<input> in bbport.ini.
    Section(T("Botones del mando", "Controller buttons"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Lo que hace cada botón en el juego, para todos los mandos. Pulsa \"Cambiar\" y luego el botón "
                                      "en el mando; cada acción admite hasta 4. Se guarda en ctrnfpc.ini y vale desde el próximo arranque.",
                                      "What each button does in the game, for every controller. Press \"Change\", then the button on "
                                      "the controller; each action takes up to 4. Saved in ctrnfpc.ini, used from the next start."));
    ImGui::PopTextWrapPos();
    const Pads::Info* shown = nullptr;
    for (const auto& p : pads)
        if (p.id == g_pad_selected) shown = &p;
    const Pads::Family family = shown ? shown->family : Pads::Family::Xbox;
    const int type = shown ? shown->type : 3;  // SDL_GAMEPAD_TYPE_XBOXONE
    if (g_pad_capture >= 0) {
        std::string name;
        uint32_t from = 0;
        if (Pads::TakeCapture(name, from) && !name.empty()) {
            const KeyInput& in = kKeyInputs[g_pad_capture];
            const std::string key = std::string("pad.") + in.id;
            auto names = g_pad_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                if (names.size() >= 4) names.erase(names.begin());
                names.push_back(name);
            }
            std::string value;
            for (const auto& n : names) value += (value.empty() ? "" : ",") + n;
            a.game.Set(key, value);
            SaveGameIni();
            g_pad_capture = -1;
        } else if (!Pads::Capturing()) {
            g_pad_capture = -1;  // the controllers went away
        }
    }
    if (ImGui::BeginTable("padbuttons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Botones", "Buttons"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const char* fallback = PadFallback(in.id);
            if (!fallback) continue;
            const std::string key = std::string("pad.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto names = SplitKeys(custom ? a.game.Get(key) : fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (g_pad_capture == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa un botón del mando...", "Press a controller button..."));
            } else if (names.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin botón)", "(none)"));
            } else {
                std::string text;
                for (const auto& n : names) {
                    const int input = Pads::InputFromName(n);
                    text += (text.empty() ? "" : "  /  ") + (input >= 0 ? Pads::Label(input, family, type, g_spanish) : n);
                }
                ImGui::TextColored(custom ? kText : kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            if (g_pad_capture == i) {
                if (ImGui::SmallButton(T("Cancelar", "Cancel"))) {
                    g_pad_capture = -1;
                    Pads::CancelCapture();
                }
            } else {
                ImGui::BeginDisabled(g_pad_capture >= 0 || !sdl || pads.empty());
                if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                    g_pad_replace = !custom;
                    g_pad_capture = i;
                    Pads::ArmCapture();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(g_pad_capture >= 0);
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
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(g_pad_capture >= 0);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Restaurar todos los botones", "Restore all buttons")).c_str())) {
        for (const auto& in : kKeyInputs)
            if (PadFallback(in.id)) a.game.Erase(std::string("pad.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: botón original. Los nombres siguen al mando elegido arriba.",
                                      "Grey: default button. Names follow the controller chosen above."));

    Section(T("Sticks", "Sticks"));
    {
        int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
        int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
        int inner_pct = int(std::lround(inner * 100.0 / 127.0)), outer_pct = int(std::lround(outer * 100.0 / 127.0));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Zona muerta", "Dead zone"), &inner_pct, 0, 40, "%d %%")) {
            a.launcher.Set("pad_deadzone", std::to_string(int(std::lround(inner_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Cuánto hay que mover el stick antes de que el juego lo note. Súbela si el kart gira solo (stick con deriva). "
                     "Por defecto 4 %. Se ve como el anillo gris en la prueba de arriba.",
                     "How far the stick moves before the game notices. Raise it if the kart steers by itself (stick drift). "
                     "Default 4 %. It is the grey ring in the test above."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Giro completo al", "Full turn at"), &outer_pct, 60, 100, "%d %%")) {
            a.launcher.Set("pad_deadzone_outer", std::to_string(int(std::lround(outer_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Con menos del 100 % el giro llega al máximo antes del tope del stick: útil con sticks gastados o mandos "
                     "que no llegan al borde. Es el anillo naranja.",
                     "Below 100 % the turn reaches its maximum before the stick's edge: useful for worn sticks or "
                     "controllers that do not reach the edge. It is the orange ring."));
        LauncherFlag("pad_center_cal", T("Corregir el centro de mandos genéricos", "Correct the centre of generic controllers"), true,
                     T("Algunos mandos clónicos dicen estar inclinados con el stick suelto; el port mide su centro al conectarlos. "
                       "Desactívalo solo si un mando original se comporta raro.",
                       "Some clone controllers report a tilt with the stick at rest; the port measures their centre when they "
                       "connect. Turn it off only if a genuine controller behaves oddly."));
        if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Valores por defecto", "Defaults")).c_str())) {
            a.launcher.Erase("pad_deadzone");
            a.launcher.Erase("pad_deadzone_outer");
            a.launcher.Erase("pad_center_cal");
            SaveLauncher();
        }
    }
}

void ControlsTab() {
    if (ImGui::BeginTabBar("controls")) {
        if (ImGui::BeginTabItem((std::string(ICON_GAMEPAD "  ") + T("Mandos", "Controllers") + "###pads").c_str())) {
            if (g_key_capture >= 0) g_key_capture = -1;
            ImGui::BeginChild("pads");
            ControllersPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem((std::string(ICON_KEYBOARD "  ") + T("Teclado", "Keyboard") + "###keys").c_str())) {
            if (g_pad_capture >= 0) {
                g_pad_capture = -1;
                Pads::CancelCapture();
            }
            ImGui::BeginChild("keyboard");
            KeyboardTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
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
    bool builtin = false;       // ... from kCtrTrophies, not the game's own (encrypted) texts
    bool spanish = false;       // the launcher language its built-in texts are in
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

/// trophies.tsv: "#source<TAB>key|builtin", "#lang<TAB>code", then id, grade, hidden, name,
/// detail per line. True for the game's own texts (decrypted with the key) in this language.
bool ReadTrophyTsv(const fs::path& file, const std::string& language, TrophySet& t, std::string* source = nullptr) {
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
        if (f.size() >= 2 && f[0] == "#source" && source) *source = f[1];
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
    return any && right_language && (!source || *source != "builtin");
}

void WriteTrophyTsv(const TrophySet& t) {
    std::string out = std::string("#source\t") + (t.builtin ? "builtin" : "key") + "\n#lang\t" + t.language + "\n#title\t" +
                      Sanitize(t.title_name) + "\n";
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
    t.spanish = g_spanish;
    t.title_id = GameTitleId();
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
    // Names: the game's own texts (trophies.tsv in this language, else decrypted with the key),
    // else the built-in list.
    std::string source;
    TrophySet probe;
    for (const auto& tr : t.list) probe.list.push_back(Trophy{tr.id});
    if (ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, probe, &source)) {
        ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, t);
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
    if (!t.names && IsCtrTitle(t.title_id) && t.list.size() == std::size(kCtrTrophies)) {
        for (auto& tr : t.list)
            for (const auto& b : kCtrTrophies)
                if (b.id == tr.id) {
                    tr.grade = b.grade;
                    tr.name = b.name;
                    tr.detail = T(b.es, b.en);
                }
        t.title_name = "Crash Team Racing Nitro-Fueled";
        t.names = t.builtin = true;
        // The runtime's in-game notice reads the names and grades (and finds the platinum) here.
        if (source != "key") WriteTrophyTsv(t);
    }
    ReadUnlocked(t);
}

void TrophiesTab() {
    App& a = g_app;
    const std::string language = GameLanguage();
    if (g_trophies_reload || g_trophies.game != a.game_dir || g_trophies.language != language ||
        (g_trophies.builtin && g_trophies.spanish != g_spanish)) {
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

    // Names: the player's trophy key (the game's own texts, in its language).
    bool show_key = !t.names;
    if (t.builtin) {
        ImGui::Spacing();
        show_key = ImGui::CollapsingHeader(T("Usar los textos del propio juego (clave de trofeos)",
                                             "Use the game's own texts (trophy key)"));
    }
    if (show_key) {
        ImGui::Spacing();
        BeginCard("trophy_key");
        ImGui::TextColored(kBlue, "%s", ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        if (t.builtin)
            ImGui::TextUnformatted(T("Los nombres son los oficiales y las descripciones, un resumen de qué hay que hacer. Los textos "
                                     "del propio juego (en su idioma) vienen cifrados con la clave de trofeos de la PS4, que no se "
                                     "incluye: si la tienes (la misma que usa shadPS4), pégala o impórtala para usarlos.",
                                     "Names are the official ones and descriptions a summary of what to do. The game's own texts "
                                     "(in its language) are encrypted with the PS4's trophy key, which is not included: if you "
                                     "have it (the same shadPS4 uses), paste or import it to use them."));
        else
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

void ToolsTab() {
    App& a = g_app;
    Section(T("Carpetas y registro", "Folders and log"));
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

    Section(T("Caché de shaders", "Shader cache"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Los shaders se guardan para que los siguientes arranques vayan fluidos. Bórrala solo si "
                                      "actualizaste el driver de la GPU o ves fallos gráficos.",
                                      "Shaders are kept so later sessions run smoothly. Clear it only after a GPU driver "
                                      "update or if you see graphics glitches."));
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(a.game_running);
    if (ImGui::Button(T("Borrar la caché de shaders", "Clear the shader cache"))) a.confirm_clear_cache = true;
    ImGui::EndDisabled();

    Section(T("Idioma del launcher", "Launcher language"));
    if (ImGui::RadioButton("Español", g_spanish)) {
        g_spanish = true;
        SaveLauncher();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("English", !g_spanish)) {
        g_spanish = false;
        SaveLauncher();
    }

    Section(T("Acerca de", "About"));
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
    ImGui::Spacing();
    ImGui::TextUnformatted(T("FSR 3.1, la generación de cuadros FSR 3.1.6 y FSR 4 vienen de AMD FidelityFX a través de "
                             "FireBurn/FSR-Vulkan (MIT). Ultra-panorámico de tcosi; edición europea basada en "
                             "OCTR-NitroFueled-PC-EU de martijnhe; CPUs antiguas (MetaHeart) y dumps dañados (Naif Alqurashi).",
                             "FSR 3.1, FSR 3.1.6 frame generation and FSR 4 come from AMD FidelityFX through "
                             "FireBurn/FSR-Vulkan (MIT). Ultrawide by tcosi; the European edition is based on martijnhe's "
                             "OCTR-NitroFueled-PC-EU; old CPUs (MetaHeart) and damaged dumps (Naif Alqurashi)."));
    ImGui::PopTextWrapPos();
    {
        std::error_code ec;
        const bool dlss = fs::is_regular_file(a.root / L"bin" / L"sacredfire_dlss.dll", ec) &&
                          fs::is_regular_file(a.root / L"bin" / L"nvngx_dlss.dll", ec);
        ImGui::TextColored(kGrey, "%s", dlss ? T("DLSS: archivos presentes (necesita una tarjeta NVIDIA RTX)",
                                                 "DLSS: files present (needs an NVIDIA RTX card)")
                                             : T("DLSS: archivos no incluidos (se usa FSR 3)",
                                                 "DLSS: files not included (FSR 3 is used)"));
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

// ---------------------------------------------------------------------------------------------
// v0.4 header: the icon, the title, the version and the edition of the chosen game.

/// "América (USA) · CUSA13795", "Europa (PAL) · CUSA14876" or "" without a game.
std::string EditionLabel() {
    const std::string title = GameTitleId();
    bool known = false;
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info}) known |= IsCtrTitle(info->title);
    if (!known) return {};
    return std::string(title == "CUSA14876" ? T("Europa (PAL)", "Europe (PAL)") : T("América (USA)", "Americas (USA)")) +
           " · " + title;
}

void Header() {
    App& a = g_app;
    const float s = g_ui_scale;
    const ImVec2 win = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowSize().x;
    const float height = 74.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 left = IM_COL32(0x2A, 0x1B, 0x3E, 0xFF), right = IM_COL32(0x15, 0x19, 0x2A, 0xFF);
    dl->AddRectFilledMultiColor(win, ImVec2(win.x + width, win.y + height), left, right, right, left);
    dl->AddRectFilledMultiColor(ImVec2(win.x, win.y + height - 3 * s), ImVec2(win.x + width, win.y + height),
                                IM_COL32(0xFF, 0x7A, 0x18, 0xFF), IM_COL32(0xFF, 0xD2, 0x3F, 0xFF),
                                IM_COL32(0xFF, 0xD2, 0x3F, 0xFF), IM_COL32(0xFF, 0x7A, 0x18, 0xFF));
    const float pad = 18.0f * s, logo = 50.0f * s;
    float x = win.x + pad;
    if (g_logo) {
        const float y = win.y + (height - logo) * 0.5f;
        dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(g_logo))), ImVec2(x, y), ImVec2(x + logo, y + logo));
        x += logo + 14.0f * s;
    }
    ImFont* title_font = g_font_title ? g_font_title : ImGui::GetFont();
    const float title_size = 30.0f * s;
    dl->AddText(title_font, title_size, ImVec2(x, win.y + 10.0f * s), IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), "CTR Nitro-Fueled PC");
    std::string version = a.version;
    if (const auto dash = version.find("-windows"); dash != std::string::npos) version.erase(dash);
    const std::string subtitle =
        std::string(T("Port nativo para Windows", "Native Windows port")) + (version.empty() ? "" : "  ·  " + version);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.92f, ImVec2(x + 2 * s, win.y + 10.0f * s + title_size + 2 * s),
                Col(kGrey), subtitle.c_str());

    // Right: the game's state and edition.
    std::vector<std::pair<std::string, ImVec4>> pills;
    const std::string edition = EditionLabel();
    if (a.game_running) pills.emplace_back(std::string(ICON_PLAY "  ") + T("En juego", "Playing"), kGreen);
    else if (a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok)
        pills.emplace_back(std::string(ICON_CHECK "  ") + T("Listo para jugar", "Ready to play"), kGreen);
    else pills.emplace_back(std::string(ICON_WARN "  ") + T("Falta preparar el juego", "Game not set up"), kYellow);
    if (!edition.empty()) pills.emplace_back(std::string(ICON_GLOBE "  ") + edition, kBlue);
    float rx = win.x + width - pad;
    const ImVec2 pp(8.0f * s, 4.0f * s);
    const float line = ImGui::GetTextLineHeight();
    for (const auto& [text, color] : pills) {
        const ImVec2 t = ImGui::CalcTextSize(text.c_str());
        const ImVec2 a0(rx - t.x - pp.x * 2, win.y + (height - line - pp.y * 2) * 0.5f);
        const ImVec2 a1(rx, a0.y + line + pp.y * 2);
        dl->AddRectFilled(a0, a1, Col(color, 0.16f), 99.0f);
        dl->AddRect(a0, a1, Col(color, 0.8f), 99.0f, 0, 1.0f * s);
        dl->AddText(ImVec2(a0.x + pp.x, a0.y + pp.y), Col(color), text.c_str());
        rx = a0.x - 8.0f * s;
    }
    ImGui::SetCursorScreenPos(ImVec2(win.x + ImGui::GetStyle().WindowPadding.x, win.y + height + 10.0f * s));
}

// ---------------------------------------------------------------------------------------------
// Play tab: what the game will start with, at a glance.

void PlaySummary() {
    App& a = g_app;
    auto res = [&]() -> std::string {
        const std::string v = EffectiveRenderRes();
        for (const auto& r : kRenderRes)
            if (v == r.value) return T(r.es, r.en);
        return v;
    };
    auto upscaler = [&]() -> std::string {
        const std::string t = GameTemporal();
        if (t == "fsr1") return "AMD FSR 1";
        if (t == "fsr3") return "AMD FSR 3";
        if (t == "fsr4") return T("AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)");
        if (t == "dlss") return "NVIDIA DLSS";
        return T("Desactivado", "Off");
    };
    auto language = [&]() -> std::string {
        const std::string v = GameLanguage();
        if (EuEdition()) {
            for (const auto& c : kGameLanguagesEu)
                if (v == c.value) return T(c.es, c.en);
        } else {
            for (const auto& c : kGameLanguages)
                if (v == c.value) return T(c.es, c.en);
        }
        return v;
    };
    const int fps = a.game.Int("fps_limit", 60);
    const std::string framegen = a.game.Get("framegen", "off");
    const std::string aspect = GameAspect();
    const std::string output = a.game.Get("output_res");
    const std::pair<std::string, std::string> rows[] = {
        {T("Resolución interna", "Internal resolution"),
         GameAspect() != "16:9" && std::atoi(EffectiveRenderRes().c_str()) > 1440
             ? std::string(T("1440p (el máximo con ultra-panorámico)", "1440p (the most with ultrawide)"))
             : res()},
        {T("Escalado", "Upscaling"), GameAspect() != "16:9" ? T("(apagado con ultra-panorámico)", "(off with ultrawide)") : upscaler()},
        {T("Generación de cuadros", "Frame generation"),
         framegen == "fsr3" || framegen == "fsr" ? "AMD FSR 3" : T("Desactivada", "Off")},
        {T("Aspecto / salida", "Aspect / output"), aspect + (output.empty() ? "" : "  ·  " + output)},
        {T("Límite de FPS", "FPS limit"), fps == 0 ? T("Sin límite", "No limit") : std::to_string(fps) + " FPS"},
        {T("Idioma del juego", "Game language"), language()},
    };
    if (ImGui::BeginTable("summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& [k, v] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kGrey, "%s", k.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::Button((std::string(ICON_SETTINGS "  ") + T("Cambiar ajustes", "Change settings")).c_str())) g_goto_tab = "settings";
    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_GAMEPAD "  ") + T("Mandos y teclado", "Controllers and keyboard")).c_str()))
        g_goto_tab = "controls";
}

void Frame() {
    App& a = g_app;
    PollJobs();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    Header();

    if (!a.notice.empty()) {
        BeginCard("notice");
        ImGui::TextColored(kYellow, ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 3.0f);
        ImGui::TextUnformatted(a.notice.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
        if (ImGui::SmallButton("OK")) a.notice.clear();
        EndCard();
    }

    bool on_controls = false;
    if (ImGui::BeginTabBar("tabs")) {
        const char* go = g_goto_tab;
        g_goto_tab = nullptr;
        auto tab = [go](const char* icon, const char* label, const char* id) {
            const std::string text = std::string(icon) + "  " + label + "###" + id;
            const bool select = go && std::strcmp(go, id) == 0;
            return ImGui::BeginTabItem(text.c_str(), nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
        };
        if (tab(ICON_PLAY, T("Jugar", "Play"), "play")) {
            ImGui::BeginChild("play");
            PlayTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_SETTINGS, T("Ajustes", "Settings"), "settings")) {
            ImGui::BeginChild("settings");
            SettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_GAMEPAD, T("Controles", "Controls"), "controls")) {
            on_controls = true;
            ControlsTab();
            ImGui::EndTabItem();
        }
        if (tab(ICON_STAR, T("Logros", "Achievements"), "achievements")) {
            ImGui::BeginChild("achievements");
            TrophiesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_PUZZLE, T("Parches", "Patches"), "patches")) {
            ImGui::BeginChild("patches");
            PatchesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_SAVE, T("Guardados", "Saves"), "saves")) {
            ImGui::BeginChild("saves");
            SavesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_PACKAGE, "DLC", "dlc")) {
            ImGui::BeginChild("dlc");
            DlcTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_TOOLS, T("Herramientas", "Tools"), "tools")) {
            ImGui::BeginChild("tools");
            ToolsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!on_controls) LeaveControls();

    SaveConfirmations();
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
    // ctrnfpc.ini was bbport.ini: an older one is taken over once. The portable package ships
    // ctrnfpc.default.ini (an update unzipped over it keeps ctrnfpc.ini).
    for (const wchar_t* source : {L"bbport.ini", L"ctrnfpc.default.ini", L"bbport.default.ini"}) {
        if (!fs::exists(GameIni(), ec) && fs::is_regular_file(a.root / source, ec)) {
            fs::copy_file(a.root / source, GameIni(), ec);
        }
    }
    a.launcher.Load(LauncherIni());
    a.game.Load(GameIni());
    const std::string lang = a.launcher.Get("ui_language");
    g_spanish = lang.empty() ? PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH : lang == "es";
    a.game_dir = TextPath(a.launcher.Get("game_dir"));
    a.base_dir = TextPath(a.launcher.Get("base_dir"));
    a.update_dir = TextPath(a.launcher.Get("update_dir"));
    // First start of a portable package: a merged folder next to the launcher is picked up.
    if (a.game_dir.empty()) {
        for (const wchar_t* name : {L"CUSA13795-1.21", L"CUSA14876-1.21", L"CUSA13795", L"CUSA14876"}) {
            if (Analyze(a.root / name).kind == Kind::Complete) {
                a.game_dir = a.root / name;
                break;
            }
        }
        for (const wchar_t* title : {L"CUSA13795", L"CUSA14876"}) {
            const std::wstring base = title, update = base + L"-patch";
            if (a.base_dir.empty() && Analyze(a.root / base).kind == Kind::Base) a.base_dir = a.root / base;
            if (a.update_dir.empty() && Analyze(a.root / update).kind == Kind::Update) a.update_dir = a.root / update;
        }
    }
    a.base_info = Analyze(a.base_dir);
    a.update_info = Analyze(a.update_dir);
    a.patches = LoadPatches();
    ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    // v0.3 one-time migration: ultrawide moves from the Patches tab (patches.json)
    // to Settings > Display > Aspect ratio (bbport.ini aspect_ratio, single dropdown).
    // If the game settings have no aspect key yet but a Patches toggle enabled an
    // ultrawide XML, adopt it as the aspect and drop both XMLs from patches.json so
    // Patches (which hides them now) can never re-enable them behind the dropdown.
    if (a.game.Get("aspect_ratio").empty()) {
        const char* adopted = nullptr;
        bool both = false;
        for (const auto& key : a.patch_enabled) {
            const char* mine = nullptr;
            if (key == "CTR-UltraWide21x9-v1.0.xml/21:9 UltraWide") mine = "21:9";
            if (key == "CTR-SuperUltraWide32x9-v1.0.xml/32:9 Super UltraWide") mine = "32:9";
            if (mine) {
                if (adopted) both = true;
                adopted = mine;
            }
        }
        if (both) adopted = nullptr;  // both on: leave off, the player picks one
        auto drop_uw = [&](std::vector<std::string>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [](const std::string& k) { return IsUltrawideKey(k); }), v.end());
        };
        if (adopted) {
            a.game.Set("aspect_ratio", adopted);
            SaveGameIni();
            drop_uw(a.patch_enabled);
            drop_uw(a.patch_disabled);
            WritePatchConfig(a.patch_enabled, a.patch_disabled);
            a.notice = T("Ultra-panorámico movido a Ajustes > Pantalla > Aspecto / salida.",
                         "Ultrawide moved to Settings > Display > Aspect / output.");
        } else {
            // No single legacy toggle: just hide any stale entries from the JSON.
            bool had = false;
            for (const auto& k : a.patch_enabled) had = had || IsUltrawideKey(k);
            for (const auto& k : a.patch_disabled) had = had || IsUltrawideKey(k);
            if (had) {
                drop_uw(a.patch_enabled);
                drop_uw(a.patch_disabled);
                WritePatchConfig(a.patch_enabled, a.patch_disabled);
            }
        }
    }
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
    // Keyboard tab: the next key's scan code, before ImGui sees it.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_key_capture >= 0) {
        const unsigned code = unsigned((lparam >> 16) & 0xff) | ((lparam & (1 << 24)) ? 0x100u : 0u);
        g_key_captured = code + 1;
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
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

// Text in Segoe UI, with Segoe UI Symbol for the PlayStation shapes and the icon font merged in;
// section titles in Segoe UI Semibold; the header in Bahnschrift. Missing fonts fall back.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windows[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path dir = fs::path(std::wstring(windows, n)) / L"Fonts";
    std::error_code ec;
    auto font = [&](std::initializer_list<const wchar_t*> names) -> std::string {
        for (const wchar_t* name : names)
            if (fs::is_regular_file(dir / name, ec)) return PathText(dir / name);
        return {};
    };
    const std::string body = font({L"segoeui.ttf"}), semibold = font({L"seguisb.ttf", L"segoeuib.ttf", L"segoeui.ttf"});
    const std::string title = font({L"bahnschrift.ttf", L"seguisb.ttf", L"segoeui.ttf"});
    const std::string symbols = font({L"seguisym.ttf"}), icons = font({L"SegoeIcons.ttf", L"segmdl2.ttf"});
    static const ImWchar kIconRanges[] = {0xE700, 0xF8FF, 0};
    auto merge = [&] {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        if (!symbols.empty()) io.Fonts->AddFontFromFileTTF(symbols.c_str(), 0.0f, &cfg);
        if (!icons.empty()) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.GlyphRanges = kIconRanges;
            ic.GlyphOffset = ImVec2(0.0f, 2.0f);
            io.Fonts->AddFontFromFileTTF(icons.c_str(), 0.0f, &ic);
        }
    };
    if (body.empty()) {
        io.Fonts->AddFontDefault();
        return;
    }
    io.Fonts->AddFontFromFileTTF(body.c_str());
    merge();
    g_font_heading = io.Fonts->AddFontFromFileTTF(semibold.c_str());
    merge();
    g_font_title = io.Fonts->AddFontFromFileTTF(title.c_str());
    merge();
}

// The application icon (resource 1) as a texture for the header.
void LoadLogo(HINSTANCE instance) {
    constexpr int px = 128;
    HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon) return;
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) {
        DestroyIcon(icon);
        return;
    }
    std::vector<uint32_t> pixels(size_t(px) * px);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    const int rows = ii.hbmColor ? GetDIBits(dc, ii.hbmColor, 0, px, pixels.data(), &bi, DIB_RGB_COLORS) : 0;
    ReleaseDC(nullptr, dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    if (rows != px) return;
    g_logo = MakeTexture(pixels.data(), px, px);
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
    // --tab <play|settings|controls|patches|saves|dlc|tools>: the tab to open first (shortcuts, docs).
    static std::string first_tab;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--tab") first_tab = PathText(fs::path(argv[i + 1]));
        if (argv) LocalFree(argv);
        if (!first_tab.empty()) g_goto_tab = first_tab.c_str();
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
    const int width = int(940 * scale), height = int(800 * scale);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + std::max(0, int(work.right - work.left - width) / 2);
    const int y = work.top + std::max(0, int(work.bottom - work.top - height) / 2);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"CTR Nitro-Fueled PC", WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                              nullptr, instance, nullptr);
    g_app.hwnd = hwnd;
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"CTR Nitro-Fueled PC", MB_ICONERROR);
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
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 17.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    g_ui_scale = scale;
    ApplyTheme(scale);
    LoadFonts();
    // A dark title bar in the theme's colour (Windows 10 2004+ / 11; ignored elsewhere).
    {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        const COLORREF caption = RGB(0x11, 0x13, 0x1A);
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    LoadLogo(instance);
    LoadState();

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
        const float clear[4] = {kBg.x, kBg.y, kBg.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    // Background work finishes on its own; a running game keeps running.
    if (g_app.worker.joinable()) {
        if (g_app.job == Job::Play) g_app.worker.detach();
        else g_app.worker.join();
    }
    if (g_app.probe.joinable()) g_app.probe.join();
    Pads::Stop();
    ReleaseTrophies(g_trophies);
    if (g_logo) g_logo->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
