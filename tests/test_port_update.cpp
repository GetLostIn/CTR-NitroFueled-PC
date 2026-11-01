// Tests of the launcher's updater (launcher/windows/port_update.cpp): versions, the GitHub
// release answer, the files an update may write, and an install that rolls back on failure.
// Build: ninja -C out/gpu-win port-update-test; run out/gpu-win/port-update-test.exe.
#include "port_update.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace PortUpdate;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void Versions() {
    CHECK(ParseVersion("v0.5.2").valid);
    CHECK(ParseVersion("v0.5.2-windows").stage == 1);
    CHECK(ParseVersion("v0.5.2-3-gcb48552-windows").stage == 2);
    CHECK(ParseVersion("v0.5.2-3-gcb48552-dirty-windows").stage == 2);
    CHECK(ParseVersion("v0.5.3-prueba-windows").stage == 0);
    CHECK(ParseVersion("v0.6").valid && ParseVersion("v0.6").patch == 0);
    CHECK(!ParseVersion("unknown").valid);
    CHECK(!ParseVersion("cb48552").valid);
    CHECK(IsNewer(ParseVersion("v0.5.2-windows"), ParseVersion("v0.5.3")));
    CHECK(IsNewer(ParseVersion("v0.5.2-windows"), ParseVersion("v0.6")));
    CHECK(IsNewer(ParseVersion("v0.5.9-windows"), ParseVersion("v0.10.0")));
    CHECK(!IsNewer(ParseVersion("v0.5.2-windows"), ParseVersion("v0.5.2")));
    CHECK(!IsNewer(ParseVersion("v0.5.3-windows"), ParseVersion("v0.5.2")));
    // A build after the tag is not older than it; a test build of the next version is.
    CHECK(!IsNewer(ParseVersion("v0.5.2-3-gabc1234-windows"), ParseVersion("v0.5.2")));
    CHECK(IsNewer(ParseVersion("v0.5.3-prueba-windows"), ParseVersion("v0.5.3")));
    CHECK(!IsNewer(ParseVersion("unknown"), ParseVersion("v0.5.3")));
}

static void Releases() {
    const std::string answer = R"({
      "url": "x", "html_url": "https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.5.3",
      "tag_name": "v0.5.3", "draft": false, "prerelease": false,
      "body": "Notes with \"quotes\", a \\ and é 😀\n- item",
      "assets": [
        {"name": "SacredFire-v0.5.3-x86_64.AppImage", "size": 449325600,
         "digest": "sha256:d4c996f38f0944b18e9bf414ba83e79ba9697f2d8235cb6074f08f0153fb2633",
         "browser_download_url": "https://github.com/x/SacredFire-v0.5.3-x86_64.AppImage"},
        {"name": "CTR-NitroFueled-PC-portable-v0.5.3-windows.zip", "size": 102586527,
         "digest": "sha256:972DF8BA69DDD3D9F829DF2CA696497D566F1DDB1012B41D5E619C96B311CE44",
         "browser_download_url": "https://github.com/x/CTR-NitroFueled-PC-portable-v0.5.3-windows.zip",
         "uploader": {"login": "GetLostIn", "id": 1}, "label": null}
      ]})";
    const auto r = ParseRelease(answer);
    CHECK(r.has_value());
    if (r) {
        CHECK(r->tag == "v0.5.3");
        CHECK(r->asset_name == "CTR-NitroFueled-PC-portable-v0.5.3-windows.zip");
        CHECK(r->size == 102586527);
        CHECK(r->sha256 == "972df8ba69ddd3d9f829df2ca696497d566f1ddb1012b41d5e619c96b311ce44");
        CHECK(r->asset_url.rfind("https://", 0) == 0);
    }
    std::string pre = answer;
    pre.replace(pre.find("\"prerelease\": false"), 19, "\"prerelease\": true");
    CHECK(!ParseRelease(pre).has_value());
    CHECK(!ParseRelease("{\"message\": \"Not Found\"}").has_value());
    CHECK(!ParseRelease("not json").has_value());
    std::string no_zip = answer;
    no_zip.replace(no_zip.find("-windows.zip\", \"size\""), 12, "-windows.txt");
    CHECK(!ParseRelease(no_zip).has_value());
}

static void Protected() {
    for (const char* p : {"bbport.ini", "launcher.ini", "ryunet_online.key", "patches.json", "BBPORT.INI",
                          "user/savedata/1/x.bin", "backups/saves-1/a", "dlc/a.pkg", "logs/x.log", "update/x.zip",
                          "out/eboot.elf", "out/patches.bin", "CUSA13795-1.21/eboot.bin", "last_run_Ryu.log",
                          "last_run.log", "../escape.txt", "ryunet_join.txt"})
        CHECK(IsProtected(fs::path(reinterpret_cast<const char8_t*>(p))));
    for (const char* p : {"bin/sacredfire.exe", "bin/VERSION", "python/python.exe", "scripts/patches.py",
                          "patches/CrashTeamRacing.xml", "out/aerolib.inl", "run.py", "bbport.default.ini",
                          "LEEME.txt", "art/menu.png"})
        CHECK(!IsProtected(fs::path(reinterpret_cast<const char8_t*>(p))));
}

static void InstallRoundTrip() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path base = fs::path(tmp) / L"port-update-test";
    fs::remove_all(base);
    const fs::path package = base / L"package", port = base / L"port", rollback = base / L"port/update/rollback";
    Write(package / L"bin/VERSION", "v0.5.3-windows\n");
    Write(package / L"bin/new.dll", "new");
    Write(package / L"bbport.ini", "should never be copied");
    Write(package / L"patches/CrashTeamRacing.xml", "<new/>");
    Write(port / L"bin/VERSION", "v0.5.2-windows\n");
    Write(port / L"bbport.ini", "fps_limit=144\n");
    Write(port / L"patches/Mine.xml", "<mine/>");
    Write(port / L"patches/CrashTeamRacing.xml", "<old/>");
    Write(port / L"user/savedata/1/CUSA13795/SAVE0/data", "save");

    CHECK(PackageFiles(package).size() == 3);
    CHECK(Install(package, port, rollback).empty());
    CHECK(Read(port / L"bin/VERSION") == "v0.5.3-windows\n");
    CHECK(Read(port / L"bin/new.dll") == "new");
    CHECK(Read(port / L"bbport.ini") == "fps_limit=144\n");
    CHECK(Read(port / L"patches/Mine.xml") == "<mine/>");
    CHECK(Read(port / L"patches/CrashTeamRacing.xml") == "<new/>");
    CHECK(Read(port / L"user/savedata/1/CUSA13795/SAVE0/data") == "save");
    CHECK(Read(rollback / L"bin/VERSION") == "v0.5.2-windows\n");

    // A failure halfway: a folder where the package has a file. Everything goes back.
    Write(package / L"zz/blocked", "x");
    Write(port / L"zz", "a file named like the folder");
    Write(package / L"bin/VERSION", "v0.5.4-windows\n");
    const std::string error = Install(package, port, rollback);
    CHECK(!error.empty());
    CHECK(Read(port / L"bin/VERSION") == "v0.5.3-windows\n");
    CHECK(Read(port / L"zz") == "a file named like the folder");
    CHECK(Read(port / L"bbport.ini") == "fps_limit=144\n");
    fs::remove_all(base);
}

// --live [download]: asks GitHub for the latest release (and downloads its ZIP into %TEMP%).
static int Live(bool download) {
    std::string error;
    const std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
    const auto body = FetchText(std::wstring(url.begin(), url.end()), &error);
    if (!body) {
        std::fprintf(stderr, "fetch failed: %s\n", error.c_str());
        return 1;
    }
    const auto r = ParseRelease(*body);
    if (!r) {
        std::fprintf(stderr, "no release in the answer (%zu bytes)\n", body->size());
        return 1;
    }
    std::printf("%s %s %llu bytes sha256 %s\n", r->tag.c_str(), r->asset_name.c_str(),
                static_cast<unsigned long long>(r->size), r->sha256.c_str());
    if (!download) return 0;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path file = fs::path(tmp) / L"port-update-live.zip";
    Progress progress;
    const std::string result = Download(std::wstring(r->asset_url.begin(), r->asset_url.end()), file, r->size,
                                        r->sha256, progress);
    std::printf("download: %s (%llu bytes)\n", result.empty() ? "ok, SHA-256 matches" : result.c_str(),
                static_cast<unsigned long long>(progress.done.load()));
    std::error_code ec;
    fs::remove(file, ec);
    return result.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--live") return Live(argc > 2);
    Versions();
    Releases();
    Protected();
    InstallRoundTrip();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("port-update-test: all passed");
    return 0;
}
