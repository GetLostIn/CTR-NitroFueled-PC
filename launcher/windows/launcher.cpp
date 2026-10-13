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
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
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

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kTitleId = "CUSA13795";
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
    if (info.title != kTitleId) {
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

std::string EffectiveRenderRes();

fs::path LauncherIni() { return g_app.root / L"launcher.ini"; }
fs::path GameIni() { return g_app.root / L"bbport.ini"; }

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
        g_app.notice = T("No se pudo guardar bbport.ini.", "Could not save bbport.ini.");
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
    fs::path target = parent / L"CUSA13795-1.21";
    if (target == near_dir || target == a.base_dir || target == a.update_dir || target == a.game_dir) {
        target = parent / L"CUSA13795-1.21-merged";
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

const char* DefaultGameLanguage() { return g_spanish ? "20" : "1"; }

/// The game language to start with: the saved one when the build has it, else the default (older
/// launchers saved codes this build lacks, such as 3 for Spain's Spanish).
std::string GameLanguage() {
    const std::string saved = g_app.launcher.Get("language");
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
        if (fs::is_directory(it->path() / L"CUSA13795", e2)) return it->path() / L"CUSA13795";
    }
    return base / L"1" / L"CUSA13795";
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
            if (Trim(xml.substr(at + 4, close - at - 4)) == kTitleId) ours = true;
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

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kRed{1.0f, 0.45f, 0.40f, 1.0f};
const ImVec4 kYellow{1.0f, 0.80f, 0.35f, 1.0f};
const ImVec4 kGrey{0.65f, 0.65f, 0.68f, 1.0f};

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
        ImGui::TextUnformatted(T("Necesitas tu propio dump de PS4: el juego base (CUSA13795) y la actualización 1.21 "
                                 "(CUSA13795-patch). Si ya los tienes juntos en una carpeta, elige esa.",
                                 "You need your own PS4 dump: the base game (CUSA13795) and the 1.21 update "
                                 "(CUSA13795-patch). If they are already merged in one folder, choose that one."));
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
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.16f, 1.0f));
        if (ImGui::Button(T("Cerrar el juego", "Stop the game"), ImVec2(-FLT_MIN, play_height))) StopGame();
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginDisabled(!ready || busy || !a.python);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.58f, 0.26f, 1.0f));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
        if (ImGui::Button(T("JUGAR", "PLAY"), ImVec2(-FLT_MIN, play_height))) StartGame();
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
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
    for (const char* known : {"off", "fsr1", "fsr3", "dlss"})
        if (value == known) return value;
    if (game.Flag("fsr3", false)) return "fsr3";
    return game.Flag("fsr1", true) ? "fsr1" : "off";
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

    ImGui::SeparatorText(T("Rendimiento", "Performance"));
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
    {
        // temporal=off|fsr1|fsr3|dlss (bbport.ini): one upscaler at a time.
        static constexpr Choice kTemporal[] = {{"Desactivado", "Off", "off"},
                                               {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                               {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"},
                                               {"NVIDIA DLSS (experimental)", "NVIDIA DLSS (experimental)", "dlss"}};
        if (GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporal, temporal.c_str(), 16.0f)) {
            a.game.Erase("fsr1");
            a.game.Erase("fsr3");
            SaveGameIni();
        }
        HelpMarker(T("FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado simple del juego. "
                     "FSR 3 y DLSS reconstruyen el detalle a lo largo de varios cuadros con la profundidad y el movimiento "
                     "del juego, y a la misma resolución también suavizan los bordes. DLSS necesita una tarjeta NVIDIA "
                     "RTX (si no, se usa FSR 3). Experimentales: si ves estelas o parpadeos, cambia a FSR 1.",
                     "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3 and DLSS "
                     "rebuild detail over several frames from the game's depth and motion, and smooth edges even at the "
                     "same resolution. DLSS needs an NVIDIA RTX card (FSR 3 runs otherwise). Experimental: switch to "
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
    ImGui::TextColored(kGrey, "%s", T("El teclado funciona siempre a la vez. La pantalla dividida se asigna en el menú del juego.",
                                      "The keyboard always works too. Split-screen ports are assigned in the in-game menu."));

    ImGui::SeparatorText(T("Consola", "Console"));
    if (a.launcher.Get("language") != GameLanguage()) a.launcher.Set("language", GameLanguage());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguages, DefaultGameLanguage());
    HelpMarker(T("Los idiomas que trae esta versión del juego (CUSA13795, América): inglés, español latinoamericano, "
                 "francés y portugués de Brasil. El español de España no está en ella. Se aplica al pulsar Jugar.",
                 "The languages this build of the game has (CUSA13795, the Americas): English, Latin American "
                 "Spanish, French and Brazilian Portuguese. Spain's Spanish is not in it. Applied when you press Play."));

    // Online (private server): writes online / online_server / online_http_port / online_id to
    // bbport.ini; run.py turns the network layer on only when "online" is 1 and a server is set.
    // The launcher only stores where to connect; it is not involved in how a server authenticates.
    ImGui::SeparatorText(T("En línea (servidor privado)", "Online (private server)"));
    {
        bool on = a.game.Flag("online", false);
        if (ImGui::Checkbox(T("Activar juego en línea", "Enable online play"), &on)) {
            a.game.Set("online", on ? "1" : "0");
            SaveGameIni();
        }
        HelpMarker(T("Apagado por defecto. El juego se conecta solo al servidor privado que indiques aquí; "
                     "los servidores oficiales de Activision y de PSN nunca se contactan. Necesitas un servidor "
                     "privado en marcha para que funcione.",
                     "Off by default. The game connects only to the private server you set here; Activision's and "
                     "PSN's official servers are never contacted. You need a running private server for this to work."));
        if (on) {
            ImGui::Indent();
            auto field = [&](const char* label, const char* key, const char* fallback, const char* hint) {
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s", a.game.Get(key, fallback).c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
                if (ImGui::InputText(label, buf, sizeof buf)) a.game.Set(key, buf);
                if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
                if (hint) HelpMarker(hint);
            };
            field(T("Dirección del servidor", "Server address"), "online_server", "",
                  T("La IP o el nombre del servidor privado (por ejemplo 192.168.1.50).",
                    "The private server's IP or host name (e.g. 192.168.1.50)."));
            field(T("Puerto HTTP", "HTTP port"), "online_http_port", "8080",
                  T("El puerto por el que el servidor atiende las peticiones (8080 por defecto).",
                    "The port the server answers requests on (8080 by default)."));
            {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%s", a.game.Get("online_id", "").c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
                if (ImGui::InputText(T("Nombre de jugador", "Player name"), buf, sizeof buf)) a.game.Set("online_id", buf);
                if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
                const std::string id = a.game.Get("online_id", "");
                bool valid = id.size() >= 3 && id.size() <= 16;
                for (char c : id)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) valid = false;
                if (!id.empty() && !valid)
                    ImGui::TextColored(kYellow, "%s", T("3 a 16 caracteres: letras, números, - o _.",
                                                        "3 to 16 characters: letters, digits, - or _."));
                else
                    ImGui::TextColored(kGrey, "%s", T("El nombre que ven otros jugadores (vacío: tu usuario de Windows).",
                                                      "The name other players see (empty: your Windows user)."));
            }
            ImGui::TextColored(kYellow, "%s", T("Solo servidores privados. Los servidores oficiales nunca se contactan.",
                                                "Private servers only. Official servers are never contacted."));
            ImGui::Unindent();
        }
    }
}

void PatchesTab() {
    App& a = g_app;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Se aplican al iniciar el juego (o con \"Apply and restart\" del menú del juego).",
                                      "Applied when the game starts (or with the in-game menu's \"Apply and restart\")."));
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
        if (needs_neo) {  // replaced by Settings > Internal resolution
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

    ImGui::SeparatorText(T("Añadir parches", "Add patches"));
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
    if (get("TITLE_ID") != kTitleId || get("CATEGORY") != "ac" || d.label.size() != 16) {
        d.problem = T("no es un DLC de CTR Nitro-Fueled (CUSA13795)", "not a CTR Nitro-Fueled (CUSA13795) DLC");
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

void ToolsTab() {
    App& a = g_app;
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

    ImGui::SeparatorText(T("Idioma del launcher", "Launcher language"));
    if (ImGui::RadioButton("Español", g_spanish)) {
        g_spanish = true;
        SaveLauncher();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("English", !g_spanish)) {
        g_spanish = false;
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
    }
    ImGui::TextLinkOpenURL(kRepoUrl, kRepoUrl);
    if (a.python) ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
}

void ClearShaderCache() {
    std::error_code ec;
    const fs::path cache = g_app.root / L"user" / L"cache" / L"CUSA13795";
    const auto removed = fs::remove_all(cache, ec);
    g_app.notice = ec ? std::string(T("No se pudo borrar la caché: ", "Could not clear the cache: ")) + ec.message()
                      : std::string(T("Caché borrada (", "Cache cleared (")) + std::to_string(removed) +
                            T(" archivos).", " files).");
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
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("CTR Nitro-Fueled PC");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "%s", a.version.c_str());

    if (!a.notice.empty()) {
        ImGui::TextColored(kYellow, "%s", a.notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) a.notice.clear();
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(T("Jugar", "Play"))) {
            PlayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Ajustes", "Settings"))) {
            ImGui::BeginChild("settings");
            SettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Teclado", "Keyboard"))) {
            ImGui::BeginChild("keyboard");
            KeyboardTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        } else if (g_key_capture >= 0) {
            g_key_capture = -1;  // left the tab while waiting for a key
        }
        if (ImGui::BeginTabItem(T("Parches", "Patches"))) {
            ImGui::BeginChild("patches");
            PatchesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Guardados", "Saves"))) {
            ImGui::BeginChild("saves");
            SavesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("DLC")) {
            ImGui::BeginChild("dlc");
            DlcTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Herramientas", "Tools"))) {
            ToolsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

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
    // The portable package ships bbport.default.ini (an update unzipped over it keeps bbport.ini).
    if (!fs::exists(GameIni(), ec) && fs::is_regular_file(a.root / L"bbport.default.ini", ec)) {
        fs::copy_file(a.root / L"bbport.default.ini", GameIni(), ec);
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
        for (const wchar_t* name : {L"CUSA13795-1.21", L"CUSA13795"}) {
            if (Analyze(a.root / name).kind == Kind::Complete) {
                a.game_dir = a.root / name;
                break;
            }
        }
        if (a.base_dir.empty() && Analyze(a.root / L"CUSA13795").kind == Kind::Base) a.base_dir = a.root / L"CUSA13795";
        if (a.update_dir.empty() && Analyze(a.root / L"CUSA13795-patch").kind == Kind::Update)
            a.update_dir = a.root / L"CUSA13795-patch";
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
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"CtrNitroFueledPcLauncher", nullptr};
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int width = int(820 * scale), height = int(720 * scale);
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
    style.WindowPadding = ImVec2(16 * scale, 14 * scale);
    style.FramePadding = ImVec2(10 * scale, 6 * scale);
    style.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    style.FrameRounding = 4 * scale;
    style.TabRounding = 4 * scale;
    wchar_t fonts[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(fonts, MAX_PATH);
    const fs::path segoe = fs::path(std::wstring(fonts, n)) / L"Fonts" / L"segoeui.ttf";
    std::error_code ec;
    if (fs::is_regular_file(segoe, ec)) io.Fonts->AddFontFromFileTTF(PathText(segoe).c_str());
    else io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
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
        const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
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
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
