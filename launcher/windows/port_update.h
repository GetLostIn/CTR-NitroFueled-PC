// Updating the portable package from the project's GitHub releases, keeping the player's files.
//
// The latest published (not pre-release, not draft) release is asked from the GitHub API; its
// Windows ZIP is downloaded into <port>\update\, checked against the size and SHA-256 GitHub
// reports, unpacked next to it, and its files are copied over the port. Only files the package
// carries are written: settings, the online key, saves, backups, DLC, logs and the player's own
// patches are never in it, and IsProtected keeps them out even if a package had them. Every file
// that is replaced is first moved to update\rollback\; any failure moves them all back.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace PortUpdate {

inline constexpr const char* kRepo = "GetLostIn/CTR-NitroFueled-PC";
inline constexpr const char* kPackageFolder = "CTR-NitroFueled-PC-portable";  // the ZIP's top folder

/// A version as builds and tags name it: v<major>.<minor>[.<patch>][suffix]. A git-describe
/// suffix (-<n>-g<hash>) comes after the tag it counts from; any other suffix (-prueba, -rc1)
/// before it. "-windows" and "-dirty" are ignored.
struct Version {
    int major = 0, minor = 0, patch = 0;
    int stage = 1;  // 0 before the tag (test build), 1 the tag itself, 2 after it (git describe)
    bool valid = false;
};
Version ParseVersion(const std::string& text);
/// True when b is a later version than a (both valid).
bool IsNewer(const Version& a, const Version& b);

struct Release {
    std::string tag, page_url;        // "v0.5.3", its release page
    std::string asset_name, asset_url;
    std::string sha256;               // lowercase hex; empty when GitHub gave none
    std::uint64_t size = 0;
};
/// The release from a GitHub /releases/latest answer, with its Windows ZIP; nullopt when the
/// answer is not one, it is a draft or pre-release, or no asset ends in "-windows.zip".
std::optional<Release> ParseRelease(const std::string& json);

/// Paths (relative to the port folder) an update never writes: the player's settings and data.
bool IsProtected(const std::filesystem::path& relative);

/// The files of an unpacked package (relative paths), protected ones left out.
std::vector<std::filesystem::path> PackageFiles(const std::filesystem::path& package);

/// Copies the package's files over the port. Each existing file is moved to rollback\<path>
/// first (a running launcher can be moved, not overwritten); on any failure everything is put
/// back and the error is returned. "" when done.
std::string Install(const std::filesystem::path& package, const std::filesystem::path& port,
                    const std::filesystem::path& rollback);

// --- network (WinHTTP) ---------------------------------------------------------------------

/// GET https://<url>; the body, or nullopt with *error set.
std::optional<std::string> FetchText(const std::wstring& url, std::string* error);

struct Progress {
    std::atomic<std::uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
};
/// Downloads url into file, checking the expected size and SHA-256 (hex, may be empty). "" when
/// done; the partial file is removed on failure.
std::string Download(const std::wstring& url, const std::filesystem::path& file, std::uint64_t size,
                     const std::string& sha256, Progress& progress);

}  // namespace PortUpdate
