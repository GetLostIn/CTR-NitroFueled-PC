# SPDX-License-Identifier: GPL-2.0-or-later
"""Updating the Linux AppImage from GitHub releases, as the Windows launcher updates its package
(launcher/windows/port_update.cpp: same repository and version rules).

The latest release names its AppImage SacredFire-<version>-x86_64.AppImage. The new one is
downloaded next to the running one (APPIMAGE) as a .part file, checked against the size and the
SHA-256 the release gives, made executable and renamed into place; only then is the old file
removed. A failed or interrupted update leaves the running AppImage as it was. The Steam shortcut
points at <data dir>/SacredFire.AppImage, which the new launcher moves to itself when it opens
(steam_shortcuts.stable_appimage_link)."""
import hashlib
import json
import os
import re
import urllib.request
from pathlib import Path

REPO = "GetLostIn/CTR-NitroFueled-PC"
LATEST = f"https://api.github.com/repos/{REPO}/releases/latest"
RELEASES_PAGE = f"https://github.com/{REPO}/releases/latest"
ASSET = re.compile(r"^SacredFire-.+-x86_64\.AppImage$")


def parse_version(text):
    """(major, minor, patch, stage) of "v0.5.4", "v0.5.4-linux" or a git describe
    "v0.5.4-3-gabc123-dirty"; stage 0 a pre-release suffix, 1 the release, 2 commits after it.
    None when it is not a version."""
    m = re.match(r"^\s*[vV]?(\d{1,5})\.(\d{1,5})(?:\.(\d{1,5}))?(.*?)\s*$", text or "")
    if not m:
        return None
    suffix = m.group(4)
    while True:
        for drop in ("-dirty", "-windows", "-linux"):
            if suffix.endswith(drop):
                suffix = suffix[:-len(drop)]
                break
        else:
            break
    stage = 1 if not suffix else 2 if re.fullmatch(r"-\d+-g[0-9a-fA-F]+", suffix) else 0
    return int(m.group(1)), int(m.group(2)), int(m.group(3) or 0), stage


def is_newer(have, latest):
    """latest is a newer version than have (both from parse_version)."""
    return bool(have and latest and latest > have)


def parse_release(body):
    """{tag, name, url, size, sha256} of the release's AppImage, None without one."""
    release = json.loads(body)
    tag = release.get("tag_name", "")
    if not parse_version(tag):
        return None
    for asset in release.get("assets") or []:
        name = asset.get("name", "")
        if not ASSET.match(name):
            continue
        url, size = asset.get("browser_download_url", ""), int(asset.get("size") or 0)
        if not url.startswith("https://") or size <= 0:
            return None
        digest = asset.get("digest") or ""
        sha = digest[7:].lower() if digest.startswith("sha256:") else ""
        return {"tag": tag, "name": name, "url": url, "size": size,
                "sha256": sha if re.fullmatch(r"[0-9a-f]{64}", sha) else ""}
    return None


def latest_release(timeout=10):
    """The latest release's AppImage (parse_release), or raises OSError/ValueError."""
    request = urllib.request.Request(LATEST, headers={"Accept": "application/vnd.github+json",
                                                      "User-Agent": "SacredFire-launcher"})
    with urllib.request.urlopen(request, timeout=timeout) as reply:
        return parse_release(reply.read(1 << 20))


def check(current_version):
    """The release to update to, or None when this one is the latest (or newer)."""
    release = latest_release()
    if release and is_newer(parse_version(current_version), parse_version(release["tag"])):
        return release
    return None


def running_appimage():
    """The AppImage this launcher runs from, None outside one (a checkout, a Nix build)."""
    path = os.environ.get("APPIMAGE")
    return Path(path) if path and Path(path).is_file() else None


def install(release, appimage, progress=lambda done, total: None, opener=urllib.request.urlopen):
    """Downloads the release's AppImage next to appimage and puts it in place of it: the new
    file's path. Raises OSError (with the reason) and leaves appimage untouched on failure."""
    folder = Path(appimage).parent
    target = folder / release["name"]
    part = folder / (release["name"] + ".part")
    digest = hashlib.sha256()
    done = 0
    try:
        request = urllib.request.Request(release["url"], headers={"User-Agent": "SacredFire-launcher"})
        with opener(request, timeout=30) as reply, open(part, "wb") as out:
            while chunk := reply.read(1 << 16):
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                progress(done, release["size"])
        if done != release["size"]:
            raise OSError(f"the download has {done} bytes, the release says {release['size']}")
        if release["sha256"] and digest.hexdigest() != release["sha256"]:
            raise OSError("the download's SHA-256 does not match the release")
        part.chmod(0o755)
        os.replace(part, target)
    except BaseException:
        part.unlink(missing_ok=True)
        raise
    old = Path(appimage).resolve()
    if old != target.resolve():
        try:
            old.unlink()
        except OSError:
            pass  # a read-only folder or file: the old one stays next to the new one
    return target
