#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SacredFire launcher (GTK4 / libadwaita): runs Crash Team Racing: Nitro-Fueled natively on Linux.

Picks the game folder (the merged 1.21 dump), the controller and the start-up options passed to
run.sh as environment variables, edits the port's settings in bbport.ini (game patches, FPS
counter), starts and stops the game and shows its output. Launcher settings live in
~/.config/sacredfire-launcher/settings.json. `--play` starts the game with the saved settings
without a window (Steam Deck game mode).

Derived from bbport's launcher (removed with the Bloodborne profile on 2026-10-09); this one
knows only the CTR profile.
"""

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from launcher_i18n import UI_LANGUAGES, menu_language, set_language, tr

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk  # noqa: E402

import trophies  # noqa: E402  (launcher/: the Achievements page's files and key)

PORT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / "scripts"))
sys.path.insert(0, str(PORT_DIR))
import content_profile  # noqa: E402
import game_check  # noqa: E402
import steam_shortcuts  # noqa: E402
import discord_ipc  # noqa: E402
import discord_oauth  # noqa: E402
import ryunet_http  # noqa: E402
import appimage_update  # noqa: E402

PACKAGED = bool(os.environ.get("BB_PREBUILT"))


def port_version():
    """The build's source revision: VERSION in the package, out/VERSION in a checkout (build.sh
    writes it: the nearest v* tag or the commit, "-dirty" with uncommitted changes)."""
    for name in ("VERSION", "out/VERSION"):
        try:
            return (PORT_DIR / name).read_text().strip()
        except OSError:
            continue
    return ""


DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "sacredfire-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
MAX_LOG_LINES = 5000
UNSUPPORTED_GPU_CODE = 24  # the game found no Vulkan 1.3 (GPU, driver or loader): vk_platform.h
TITLE = "SacredFire"
GAME = "Crash Team Racing: Nitro-Fueled"

# Choices: (label, value); the first is the default.
# The languages each edition has (the .lng texts in its archives), as PS4 system language codes:
# the Americas build (CUSA13795) and the European one (CUSA14876). The list follows the chosen game.
LANGUAGES = [("English", "1"), ("Spanish (Latin America)", "20"), ("French", "2"),
             ("Portuguese (Brazil)", "17")]
LANGUAGES_EU = [("English", "1"), ("Spanish (Spain)", "3"), ("French", "2"), ("German", "4"),
                ("Italian", "5"), ("Arabic", "21")]
REGIONS = {"CUSA13795": "USA", "CUSA14876": "EU"}


def languages_for(title):
    return LANGUAGES_EU if title == "CUSA14876" else LANGUAGES


def language_for(title, saved):
    """The saved game language where the edition has it, else its counterpart (Spanish), else English."""
    codes = [code for _label, code in languages_for(title)]
    if saved in codes:
        return saved
    if saved == "3" and "20" in codes:
        return "20"
    if saved == "20" and "3" in codes:
        return "3"
    return "1"
PRESENT_MODES = [("Mailbox", "Mailbox"), ("FIFO (VSync)", "Fifo"),
                 ("FIFO Relaxed", "FifoRelaxed"), ("Immediate", "Immediate")]
# The game's FPS limit (bbport.ini fps_limit; the in-game menu offers the same values). Above 30
# it needs the 60 FPS patch.
FPS_LIMITS = [("No limit", "0"), ("30", "30"), ("60", "60"), ("72 (half of 144 Hz)", "72"),
              ("90", "90"), ("120", "120"), ("144", "144"), ("240", "240")]
# bbport.ini render_res: the size the game draws its 3D scene at (scripts/patches.py render_plan;
# run.sh turns the PS4 Pro path on above 1080p). "" keeps the older PS4 Pro switches below.
RENDER_RESOLUTIONS = [("Game default (the PS4 Pro switches decide)", ""),
                      ("480p (854 x 480, very low-end PCs)", "480"),
                      ("720p (1280 x 720, low-end PCs, Steam Deck)", "720"),
                      ("1080p (1920 x 1080)", "1080"),
                      ("1440p (2560 x 1440, PS4 Pro)", "1440"),
                      ("1620p (2880 x 1620)", "1620"),
                      ("4K (3840 x 2160)", "2160")]
# bbport.ini aspect_ratio / aspect_output: the display shape and its screen sizes.
# scripts/aspect_ratio.py prepares 16:10; widescreen.py keeps the corrected 21:9/32:9 layouts.
ASPECT_RATIOS = [("16:9", "16x9"), ("16:10 (experimental)", "16x10"),
                 ("21:9 (corrected menus and HUD)", "21x9"),
                 ("32:9 (corrected menus and HUD)", "32x9")]
ASPECT_OUTPUTS = {"16x10": [("1920 x 1200", "1920x1200"), ("1280 x 800", "1280x800"),
                           ("2560 x 1600", "2560x1600")],
                  "21x9": [("3440 x 1440", "3440x1440"), ("2560 x 1080", "2560x1080")],
                  "32x9": [("5120 x 1440", "5120x1440"), ("3840 x 1080", "3840x1080"),
                           ("2560 x 720", "2560x720"), ("7680 x 2160", "7680x2160")]}
# bbport.ini temporal: one upscaler at a time (the game reads it). DLSS is Windows-only (its
# NVIDIA library is a Windows DLL); there "dlss" falls back to FSR 3, so it is not offered here.
UPSCALERS = [("Off", "off"), ("AMD FSR 1", "fsr1"), ("AMD FSR 3 (experimental)", "fsr3")]
# bbport.ini renderer: run.py sets the native renderer up and builds its shaders from the player's
# game the first time (scripts/native_shaders.py); it has no FSR 3 (run.py uses FSR 1 instead).
RENDERERS = [("Vulkan (shadPS4)", "shadps4"), ("Native Vulkan (experimental)", "native")]
# Settings the game reads from bbport.ini itself (gpu/shim/bbport_settings.cpp), with the
# game's defaults: the launcher shows and writes them, the in-game menu changes the same keys.
GAME_FLAGS = {"async_pipelines": "0", "msaa": "0", "borderless": "0"}
VOLUME_DEFAULT = 100
# Patches with a bbport.ini key of their own (shared with the in-game menu); other patches are
# switched in patches.json (scripts/patches.py GAME_PATCH_KEYS).
INI_PATCHES = {"CrashTeamRacing.xml/60 FPS Unlock": "patch_60fps",
               "CrashTeamRacing.xml/Disable Motion Blur": "patch_no_motion_blur",
               "CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)": "patch_pro_1080p",
               "CrashTeamRacingUnlockAll.xml/Unlock Everything": "patch_unlock_all",
               # The European edition's files: the same switches (scripts/patches.py BUNDLED_FILES).
               "CrashTeamRacingUnlockAllEU.xml/Unlock Everything": "patch_unlock_all",
               "CrashTeamRacingEU.xml/60 FPS Unlock": "patch_60fps",
               "CrashTeamRacingEU.xml/Disable Motion Blur": "patch_no_motion_blur",
               "CrashTeamRacingEU.xml/Resolution Patch (1080p for PS4 Pro)": "patch_pro_1080p"}
# Set by the online switch, not by hand (patches.py turns it on for online play).
HIDDEN_PATCHES = {"CrashTeamRacingOnline.xml/Online: custom Demonware key",
                  "CrashTeamRacingOnlineEU.xml/Online: custom Demonware key"}

DEFAULTS = {
    "ui_language": "",
    "game_dir": "" if PACKAGED else str(PORT_DIR / "orig" / "CUSA13795"),
    "user_dir": "",
    "language": "1",
    "fullscreen": False,
    "present_mode": "Mailbox",
    "gamepad": "",
    "gamepad_name": "",
    "neo": False,
    "clear_shader_cache": False,
    "frame_stats": False,
    "save_log": False,
    "mangohud": False,
    "vk_validation": False,
    "extra_env": "",
    # DLC in the data folder's dlc/ (run.py BB_DLC_DIR): all on, some turned off by label.
    "dlc": True,
    "dlc_disabled": "",
    # Discord: the game in the player's status while it runs, with invites; "Ask to Join"
    # requests accepted. discord_app_id names another Discord application (empty: ours).
    "discord_rpc": True,
    "discord_accept_requests": True,
    "discord_app_id": "",
    # The monitor for the game ("": the primary; else its number, BB_DISPLAY), and the launcher
    # window while the game runs: hidden, and closed when the game ends normally.
    "display": "",
    "hide_on_play": False,
    "close_on_play": False,
    # The PS4 trophy key (32 hex digits, the one shadPS4 uses): the Achievements page decrypts
    # the trophies' names with it (launcher/trophies.py).
    "trophy_key": "",
}
DISCORD_APP_ID = "1560408801704943708"

# Controls (src/runtime_pad.c): bbport.ini key.<input>= and pad.<input>= replace a default
# (SDL key and gamepad button names, comma separated); no line keeps the default. The same
# inputs and defaults as the Windows launcher's Keyboard and Gamepad tabs.
KEY_INPUTS = [
    ("move_up", "Left stick up", "W"),
    ("move_down", "Left stick down", "S"),
    ("move_left", "Left stick left (steer)", "A"),
    ("move_right", "Left stick right (steer)", "D"),
    ("cross", "Cross (accelerate)", "Space"),
    ("square", "Square (brake)", "E"),
    ("circle", "Circle", "Left Shift"),
    ("triangle", "Triangle", "Q"),
    ("r1", "R1", "3"),
    ("l1", "L1", "1"),
    ("r2", "R2", "F"),
    ("l2", "L2", "R"),
    ("l3", "L3", "Z"),
    ("r3", "R3", "C"),
    ("options", "Options (pause)", "Return"),
    ("touchpad", "Touchpad", "Tab"),
    ("touchpad_right", "Touchpad (right)", "Backspace"),
    ("up", "D-pad up", "I"),
    ("down", "D-pad down", "K"),
    ("left", "D-pad left", "J"),
    ("right", "D-pad right", "L"),
    ("look_up", "Right stick up", "Up"),
    ("look_down", "Right stick down", "Down"),
    ("look_left", "Right stick left", "Left"),
    ("look_right", "Right stick right", "Right"),
]
PAD_INPUTS = [
    ("cross", "Cross (accelerate / select)", "a"),
    ("square", "Square (brake / reverse)", "x"),
    ("circle", "Circle", "b"),
    ("triangle", "Triangle", "y"),
    ("l1", "L1 / LB (hop / drift)", "leftshoulder"),
    ("r1", "R1 / RB (hop / drift)", "rightshoulder"),
    ("l2", "L2 / LT (use item)", "lefttrigger"),
    ("r2", "R2 / RT (look back)", "righttrigger"),
    ("l3", "L3 (left stick click)", "leftstick"),
    ("r3", "R3 (right stick click)", "rightstick"),
    ("options", "Options / Start (pause)", "start"),
    ("touchpad", "Touchpad / Back / Select", "back,touchpad"),
    ("up", "D-pad up", "dpup"),
    ("down", "D-pad down", "dpdown"),
    ("left", "D-pad left", "dpleft"),
    ("right", "D-pad right", "dpright"),
]
PAD_LABELS = {
    "a": "Cross / A", "b": "Circle / B", "x": "Square / X", "y": "Triangle / Y",
    "leftshoulder": "L1 / LB", "rightshoulder": "R1 / RB", "lefttrigger": "L2 / LT trigger",
    "righttrigger": "R2 / RT trigger", "leftstick": "L3", "rightstick": "R3", "start": "Options / Start",
    "back": "Share / Back / Select", "touchpad": "Touchpad", "dpup": "D-pad up", "dpdown": "D-pad down",
    "dpleft": "D-pad left", "dpright": "D-pad right", "guide": "Guide / PS", "misc1": "Capture / Misc",
}
MAX_KEYS = 4  # bindings per input, as in the Windows launcher
INI_DEFAULTS = {"show_fps": "0", "fps_limit": "60", **{key: "0" for key in INI_PATCHES.values()}}
# RyuNET online (run.py setup_online reads the same bbport.ini keys). The nickname is the
# player's identity on the server: the first installation to use one keeps it.
ONLINE_SERVER = "octr.ryu7w7.xyz"
RESERVED_NICKS = {"ryupc", "ryunet", "player", "jugador", "guest"}
INVENTORIES = [("Bought on the server", "server"), ("Local save progress", "save")]


def nick_ok(name):
    return (3 <= len(name) <= 16 and all(c.isascii() and (c.isalnum() or c in "_-") for c in name)
            and name.lower() not in RESERVED_NICKS)


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text()))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False))


def ini_path():
    return Path(os.environ.get("BB_CONFIG", DATA_DIR / "bbport.ini"))


def load_ini():
    values = dict(INI_DEFAULTS)
    lines = []
    try:
        lines = ini_path().read_text().splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps every other line. A value
    of None removes its line (the game's default again)."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                if values[key] is not None and key not in written:
                    out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# SacredFire / bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written and value is not None:
            out.append(f"{key}={value}")
    ini_path().write_text("\n".join(out) + "\n")


def game_environment(s):
    """Environment for run.sh from the launcher settings."""
    env = dict(os.environ)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = s["user_dir"]
    env["BB_LANGUAGE"] = s["language"]
    env["BB_MENU_LANGUAGE"] = menu_language()
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_PRESENT_MODE"] = s["present_mode"]
    if s.get("display"):
        env["BB_DISPLAY"] = s["display"]  # the monitor's number (gpu/shim/window.cpp)
    if s.get("gamepad"):
        env["BB_GAMEPAD"] = s["gamepad"]
    env["BB_NEO"] = "1" if s.get("neo") else "0"
    if s.get("clear_shader_cache"):
        env["BB_CLEAR_SHADER_CACHE"] = "1"
    if s["frame_stats"]:
        env["BB_FRAME_STATS"] = "1"
    if s.get("save_log"):
        env["BB_SAVE_LOG"] = "1"
    if s["mangohud"]:
        env["MANGOHUD"] = "1"
    if s["vk_validation"]:
        env["BB_VK_VALIDATION"] = "1"
    env["BB_DLC"] = "1" if s.get("dlc", True) else "0"
    env["BB_DLC_DISABLED"] = s.get("dlc_disabled", "")
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env


def connected_gamepads():
    """(GUID, name) of the connected gamepads (bb-gpu-capabilities --gamepads), [] if unknown."""
    tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
    try:
        run = subprocess.run([str(tool), "--gamepads"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return []
    return [tuple(line.split("\t", 1)) for line in run.stdout.splitlines() if "\t" in line]


def connected_displays():
    """(number, label) of the monitors (bb-gpu-capabilities --displays: name, size, primary),
    numbered from 1 as BB_DISPLAY takes them; [] if unknown."""
    tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
    try:
        run = subprocess.run([str(tool), "--displays"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in run.stdout.splitlines():
        parts = line.strip().split("\t")
        if len(parts) < 2:
            continue
        number = str(len(found) + 1)
        label = f"{number}: {parts[0]} ({parts[1]})"
        if len(parts) > 2 and parts[2] == "1":
            label += " - " + tr("primary")
        found.append((number, label))
    return found


# --- patches: every eboot patch for CUSA13795 01.21 in the bundled patches/ and the player's
# own folder (the data folder's patches/; the package's is read-only), as run.sh passes them to
# scripts/patches.py.

def user_patches_dir():
    return DATA_DIR / "patches"


def patch_file_title(path):
    """The CTR edition a patch file names in <TitleID> (CUSA13795 / CUSA14876), "" for any game."""
    import xml.etree.ElementTree as ET
    try:
        ids = [e.text.strip() for e in ET.parse(path).getroot().iter("ID") if e.text]
    except (OSError, ET.ParseError):
        return ""
    return next((i for i in ids if i in REGIONS), "")


def patch_list(title=None, version=None):
    """[(key, name, author, version, note, default_on, file, file title)] of the patches the game
    can use: those for its title and version (as run.sh applies them), the defaults when unknown."""
    import patches as patch_files
    found = patch_files.all_external_patches([PORT_DIR / "patches", user_patches_dir()],
                                             version or patch_files.APP_VERSION, title or patch_files.TITLE_ID)
    listed = []
    for key, path, meta in found:
        if key in HIDDEN_PATCHES:
            continue
        file_title = patch_file_title(path)
        if title and file_title and file_title != title:
            continue
        listed.append((key, meta.get("Name", ""), meta.get("Author", ""), meta.get("PatchVer", ""), meta.get("Note", ""),
                       meta.get("isEnabled", "false").lower() == "true", path, file_title))
    return listed


def patch_config_path():
    return Path(os.environ.get("BB_PATCHES_CONFIG", DATA_DIR / "patches.json"))


def read_patch_config():
    """patches.json's "enabled" and "disabled" lists."""
    try:
        data = json.loads(patch_config_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {}
    return [str(x) for x in data.get("enabled", [])], [str(x) for x in data.get("disabled", [])]


def write_patch_config(enabled, disabled):
    path = patch_config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"enabled": enabled, "disabled": disabled}, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


# --- mods: the packages in the data folder's mods/ (.pak, .zip with or without mod.json, folders,
# .xml patches), as the Windows launcher's Mods tab. scripts/mod_library.py checks them into
# mod-state/ and lists each one in patches/OCTR-Mod-Library.xml as a patch named "Mod: <id>",
# switched in patches.json like any patch; run.sh (scripts/mod_launch.py) applies the enabled ones.

MOD_PREFIX = "OCTR-Mod-Library.xml/Mod: "
MOD_EXTENSIONS = (".pak", ".zip", ".xml")
GAMEBANANA = "https://gamebanana.com/games/26138"  # the game's page, where the community shares mods
MOD_GUIDE_URL = "https://github.com/GetLostIn/CTR-NitroFueled-PC/blob/main/docs/mod-packaging.html"


def mods_dir():
    return Path(os.environ.get("BB_MODS_DIR") or DATA_DIR / "mods")


def scan_mods():
    """The packages in mods/ checked and listed for the game (the Windows launcher's
    mod_packages.py --scan): {"entries": {id: item}, "messages": [problems]}."""
    import mod_library
    from mod_session import launch_lock
    root = mods_dir()
    with launch_lock(root.parent / "mod-state"):
        return mod_library.scan(root, user_patches_dir())


def remove_mod(ident, digest):
    """Moves the mod to mod-state/removed (recoverable); its saves stay. Returns where it went."""
    import mod_library
    return mod_library.remove(mods_dir(), user_patches_dir(), ident, digest)


def mod_entries(title=None, version=None):
    """[(key, metadata)] of the scanned packages the game can use (the library's patch file)."""
    import patches as patch_files
    found = patch_files.external_patches(user_patches_dir(), version or patch_files.APP_VERSION,
                                         title or patch_files.TITLE_ID)
    return [(key, meta) for key, _path, meta in found if key.startswith(MOD_PREFIX)]


def import_mods(sources):
    """Copies files and folders into mods/ (never over one of the same name): (added, problems)."""
    import shutil
    root = mods_dir()
    added, problems = 0, []
    for source in sources:
        source = Path(source)
        if not source.is_dir() and source.suffix.lower() not in MOD_EXTENSIONS:
            problems.append(tr("{}: add .pak, .zip, .xml files or a folder").format(source.name))
            continue
        target = root / source.name
        # The mods folder itself, or one holding it, would be copied into itself without end
        # (issue #70); one already inside it is in the library.
        if source.is_dir():
            real, mods = source.resolve(), root.resolve()
            if mods == real or mods.is_relative_to(real):
                problems.append(tr("{}: this is the mods folder (or holds it); add the mod's own folder").format(source.name))
                continue
            if real.is_relative_to(mods):
                problems.append(tr("{}: already in the mods folder").format(source.name))
                continue
        if target.exists():
            problems.append(tr("{}: a mod with that name already exists; it was not overwritten").format(source.name))
            continue
        try:
            root.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target, symlinks=False)
            else:
                shutil.copy2(source, target)
            added += 1
        except OSError as error:
            problems.append(f"{source.name}: {error}")
    return added, problems


# --- saves: <user>/savedata/<user id>/CUSA13795/<slot> holds the game's files and <slot>.sce_sys
# its param.bin (title, subtitle, detail: OrbisSaveDataParam) and icon0.png, as
# src/runtime_savedata.c keeps them. Backups are copies in the data folder's backups/saves-<date>.

SAVE_PARAM_SIZE = 1328  # char title[128], subtitle[128], detail[1024]; u32; i32; i64 mtime; 32 bytes


# The editions the port runs: the Americas (CUSA13795) and Europe (CUSA14876). Saves, the shader
# cache and DLC follow the chosen game's title ID (set_game_title); the Americas one by default.
GAME_TITLES = ("CUSA13795", "CUSA14876")
GAME_TITLE = ["CUSA13795"]
GAME_VERSION = [""]  # its APP_VER (the EU base game is 01.00), "" until a game is chosen


def set_game_title(game_dir):
    """The title ID of the game folder (param.sfo), when it is one of GAME_TITLES."""
    try:
        info = game_check.sfo_info(Path(game_dir).expanduser())
    except Exception:  # an unreadable param.sfo keeps the last one
        return GAME_TITLE[0]
    if info.get("TITLE_ID", "") in GAME_TITLES:
        GAME_TITLE[0] = info["TITLE_ID"]
        GAME_VERSION[0] = info.get("APP_VER", "")
    return GAME_TITLE[0]


def save_root(user_dir):
    title = GAME_TITLE[0]
    base = Path(user_dir) / "savedata"
    if (base / "1" / title).is_dir():
        return base / "1" / title  # player 1's: guest<N> holds players 2-4's (src/runtime_savedata.c)
    try:
        for entry in sorted(base.iterdir()):
            if not entry.name.startswith("guest") and (entry / title).is_dir():
                return entry / title
    except OSError:
        pass
    return base / "1" / title


def backup_root():
    return DATA_DIR / "backups"


def is_meta_name(name):
    return name.endswith((".sce_sys", ".memory"))


def folder_bytes(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += (Path(root) / name).stat().st_size
            except OSError:
                pass
    return total


def format_size(size):
    return f"{size / (1 << 20):.1f} MB" if size >= 1 << 20 else f"{size / 1024:.0f} KB"


def list_saves(user_dir):
    """[(slot, description, bytes, modified)] of the game's save slots."""
    root = save_root(user_dir)
    slots = []
    try:
        entries = sorted(root.iterdir())
    except OSError:
        return slots
    for entry in entries:
        if not entry.is_dir() or is_meta_name(entry.name):
            continue
        text = []
        try:
            raw = (root / f"{entry.name}.sce_sys" / "param.bin").read_bytes()
            if len(raw) >= SAVE_PARAM_SIZE:
                for start, size in ((0, 128), (128, 128), (256, 1024)):
                    value = raw[start:start + size].split(b"\0", 1)[0].decode("utf-8", "replace").strip()
                    if value:
                        text.append(value)
        except OSError:
            pass
        newest = entry.stat().st_mtime
        for folder, _dirs, files in os.walk(entry):
            for name in files:
                try:
                    newest = max(newest, (Path(folder) / name).stat().st_mtime)
                except OSError:
                    pass
        slots.append((entry.name, " - ".join(text) or "-", folder_bytes(entry),
                      time.strftime("%Y-%m-%d %H:%M", time.localtime(newest))))
    return slots


def list_backups():
    """[(path, bytes)] of the backups, newest first."""
    try:
        found = [p for p in backup_root().iterdir() if p.is_dir() and p.name.startswith("saves-")]
    except OSError:
        return []
    return [(p, folder_bytes(p)) for p in sorted(found, key=lambda p: p.name, reverse=True)]


def has_saves(user_dir):
    root = save_root(user_dir)
    try:
        return root.is_dir() and any(root.iterdir())
    except OSError:
        return False


def backup_saves(user_dir, label=""):
    """A copy of all saves in backups/saves-<date>[-label]; None when there is nothing to copy."""
    import shutil
    if not has_saves(user_dir):
        return None
    target = backup_root() / ("saves-" + time.strftime("%Y%m%d-%H%M%S") + (f"-{label}" if label else ""))
    shutil.copytree(save_root(user_dir), target, dirs_exist_ok=True)
    return target


def restore_backup(user_dir, backup):
    """The saves become the backup's; the current ones are backed up first."""
    import shutil
    backup_saves(user_dir, "auto")
    root = save_root(user_dir)
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(backup, root)


def read_sfo(path):
    """param.sfo's text and number entries as a dict."""
    import struct
    data = Path(path).read_bytes()
    if data[:4] != b"\0PSF":
        raise ValueError("not a param.sfo")
    key_table, data_table, count = struct.unpack_from("<III", data, 8)
    values = {}
    for i in range(count):
        key_offset, fmt, length, _max_length, offset = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[key_table + key_offset:].split(b"\0", 1)[0].decode("utf-8", "replace")
        raw = data[data_table + offset:data_table + offset + length]
        if fmt == 0x0404:
            values[key] = str(struct.unpack("<I", raw[:4])[0]) if len(raw) >= 4 else ""
        else:
            values[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
    return values


def import_saves(user_dir, source):
    """A save folder from elsewhere: this launcher's backup or another PC's CUSA13795 folder (copied
    as it is), or a decrypted PS4 save (Apollo Save Tool, Save Wizard: its files plus
    sce_sys/param.sfo), converted to the runtime's layout. The saves are backed up first.
    Returns an error text, or "" when done."""
    import shutil
    import struct
    source = Path(source)
    root = save_root(user_dir)
    sfo = source / "sce_sys" / "param.sfo"
    if sfo.is_file():
        try:
            values = read_sfo(sfo)
        except (OSError, ValueError, struct.error) as error:
            return str(error)
        slot = values.get("SAVEDATA_DIRECTORY", "")
        if not slot or len(slot) >= 32 or any(c in slot for c in "\\/:"):
            return tr("param.sfo does not name the save slot (SAVEDATA_DIRECTORY).")
        backup_saves(user_dir, "before-import")
        target, meta = root / slot, root / f"{slot}.sce_sys"
        shutil.rmtree(target, ignore_errors=True)
        target.mkdir(parents=True, exist_ok=True)
        meta.mkdir(parents=True, exist_ok=True)
        for entry in source.iterdir():
            if entry.name == "sce_sys":
                continue
            if entry.is_dir():
                shutil.copytree(entry, target / entry.name, dirs_exist_ok=True)
            else:
                shutil.copy2(entry, target / entry.name)
        param = bytearray(SAVE_PARAM_SIZE)
        for start, size, key in ((0, 128, "MAINTITLE"), (128, 128, "SUBTITLE"), (256, 1024, "DETAIL")):
            value = values.get(key, "").encode("utf-8")[:size - 1]
            param[start:start + len(value)] = value
        struct.pack_into("<q", param, 1288, int(time.time()))
        (meta / "param.bin").write_bytes(bytes(param))
        icon = source / "sce_sys" / "icon0.png"
        if icon.is_file():
            shutil.copy2(icon, meta / "icon0.png")
        return ""
    ours = any(not is_meta_name(p.name) and p.is_dir() and (source / f"{p.name}.sce_sys").is_dir()
               for p in source.iterdir()) if source.is_dir() else False
    if not ours:
        return tr("This folder is not recognized: choose a backup, the CUSA13795 saves folder from another PC or a "
                  "decrypted PS4 save (with sce_sys/param.sfo).")
    backup_saves(user_dir, "before-import")
    shutil.copytree(source, root, dirs_exist_ok=True)
    return ""


def discord_activity_for(ini, started, party):
    """What Discord shows while the game runs, from bbport.ini: online with the nickname and a
    joinable party (the Windows launcher's), or offline. party: (players, max) of the lobby the
    player hosts, None when unknown."""
    nick, server = ini.get("online_name", "").strip(), ini.get("online_server", ONLINE_SERVER).strip()
    if ini.get("online") != "1":
        return discord_ipc.Activity(tr("Playing offline"), start=started, large_image="logo",
                                    large_text="Crash Team Racing Nitro-Fueled")
    activity = discord_ipc.Activity(tr("Playing online"), state=tr("Nickname: {}").format(nick) if nick else "",
                                    start=started, large_image="logo", large_text="Crash Team Racing Nitro-Fueled")
    if nick_ok(nick) and ryunet_http.valid_server(server):
        players, maximum = party or (1, 8)  # a server without /ryunet/party: offer it anyway
        if 0 < players <= maximum:
            activity.party_id = f"ctr:{server.lower()}:{nick.lower()}"
            activity.party_size, activity.party_max = players, maximum
            if players < maximum:
                activity.join_secret = f"ctr1|{server}|{nick}"
    return activity


def parse_join_secret(secret):
    """(server, nickname) of a join secret this launcher (or the Windows one) wrote, else None."""
    parts = secret.split("|")
    if len(parts) != 3 or parts[0] != "ctr1" or not ryunet_http.valid_server(parts[1]) or not nick_ok(parts[2]):
        return None
    return parts[1], parts[2]


def apply_join(server, nick):
    """An accepted invite: the host becomes a friend (the game lists friends' lobbies as
    joinable), online on that server, and the game joins their session by itself, as a PS4
    invitation accepted in the system UI (src/runtime_services.c reads ryunet_join.txt next to
    bbport.ini, also while it runs). Returns the friends list written."""
    ini, lines = load_ini()
    friends = [f.strip() for f in ini.get("online_friends", "").split(",") if f.strip()]
    if nick.lower() not in [f.lower() for f in friends]:
        friends.append(nick)
    ini.update({"online": "1", "online_server": server, "online_friends": ",".join(friends)})
    save_ini(ini, lines)
    try:
        (ini_path().parent / "ryunet_join.txt").write_text(
            f"session=cusa13795-session-{nick}\nhost={nick}\ntime={int(time.time())}\n", encoding="utf-8")
    except OSError:
        pass
    return ",".join(friends)


def write_announcement(answer):
    """A server announcement for the game, which draws it over the picture (ryunet_announce.txt
    next to bbport.ini). Returns its text."""
    text = str(answer.get("text", "")).replace("\n", " ")
    try:
        expires = int(time.time()) + int(answer.get("expires_in", 0))
        (ini_path().parent / "ryunet_announce.txt").write_text(
            f"id={answer.get('id')}\nlevel={answer.get('level', '')}\nexpires={expires}\ntext={text}\n",
            encoding="utf-8")
    except (OSError, ValueError):
        pass
    return text


def png_texture(data):
    """A PNG (bytes) as a Gdk.Texture; None when it cannot be read."""
    if not data:
        return None
    try:
        return Gdk.Texture.new_from_bytes(GLib.Bytes.new(data))
    except (GLib.Error, AttributeError, TypeError):
        return None


def file_texture(path):
    try:
        return Gdk.Texture.new_from_filename(str(path)) if Path(path).is_file() else None
    except (GLib.Error, AttributeError, TypeError):
        return None


# The launcher's own look, over libadwaita's: a sidebar with the game's icon and a warm "nitro"
# accent (the Windows launcher has its own, the game menu's blue). Works in light and dark.
CSS = """
.sf-sidebar { background-image: linear-gradient(170deg, alpha(#ff6b1a, 0.13), alpha(#ff6b1a, 0.0) 55%); }
.sf-brand { margin: 6px 14px 10px 14px; padding: 10px; border-radius: 14px; background-color: alpha(currentColor, 0.05); }
.sf-brand-icon { border-radius: 10px; }
.sf-brand-title { font-weight: 800; font-size: 1.05em; }
.sf-brand-sub { font-size: 0.85em; opacity: 0.65; }
.sf-section { font-size: 0.72em; font-weight: 800; letter-spacing: 0.1em; opacity: 0.5; margin: 14px 12px 4px 12px; }
.sf-nav { background: none; }
.sf-nav row { border-radius: 10px; padding: 9px 12px; }
.sf-nav row:selected { background-color: alpha(#ff6b1a, 0.17); box-shadow: inset 3px 0 #ff7a2e; }
.sf-nav row:selected image { color: #ff7a2e; }
.sf-nav row:selected label { font-weight: 700; }
.sf-play { font-weight: 800; padding-left: 18px; padding-right: 18px; }
.sf-trophy-card { padding: 14px; }
.sf-trophy-banner { border-radius: 10px; }
.sf-trophy-icon { border-radius: 8px; }
.sf-trophy-locked { opacity: 0.38; }
.sf-big { font-size: 1.3em; font-weight: 800; }
"""


def install_css():
    provider = Gtk.CssProvider()
    try:
        provider.load_from_string(CSS)
    except AttributeError:  # GTK before 4.12
        provider.load_from_data(CSS, -1)
    display = Gdk.Display.get_default()
    if display is not None:
        Gtk.StyleContext.add_provider_for_display(display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def flat_button(icon, tooltip, handler):
    button = Gtk.Button(icon_name=icon, valign=Gtk.Align.CENTER, tooltip_text=tooltip)
    button.add_css_class("flat")
    button.connect("clicked", handler)
    return button


# What the AppImage's wrapper and runtime add to the environment (packaging/default.nix): the
# bundled GTK's data, libraries, fonts and pixbuf loaders. A browser or file manager started with
# them finds the wrong libraries, and the bundled GIO does not see the system's applications.
FOREIGN_ENV = ("LD_LIBRARY_PATH", "LD_PRELOAD", "GDK_PIXBUF_MODULE_FILE", "FONTCONFIG_FILE", "GIO_EXTRA_MODULES",
               "GI_TYPELIB_PATH", "GSETTINGS_SCHEMA_DIR", "GTK_PATH", "GTK_EXE_PREFIX", "GTK_DATA_PREFIX",
               "PYTHONPATH", "PYTHONHOME", "PYTHONNOUSERSITE", "APPDIR", "APPIMAGE", "ARGV0", "OWD",
               "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_LAYER_PATH", "VK_LAYER_PATH", "LIBGL_DRIVERS_PATH",
               "__EGL_VENDOR_LIBRARY_DIRS", "LIBVA_DRIVERS_PATH")


def host_environment():
    """The environment for a program of the player's system: without what the AppImage added, and
    with the system's own PATH and data directories."""
    env = {k: v for k, v in os.environ.items() if k not in FOREIGN_ENV and not k.startswith(("BB_", "NIX"))}
    path = [p for p in env.get("PATH", "").split(":") if p and not p.startswith("/nix/")]
    env["PATH"] = ":".join(path + [p for p in ("/usr/local/bin", "/usr/bin", "/bin") if p not in path])
    data = [d for d in env.get("XDG_DATA_DIRS", "").split(":") if d and not d.startswith("/nix/")]
    home = Path.home()
    for d in (str(home / ".local/share/flatpak/exports/share"), "/var/lib/flatpak/exports/share",
              "/usr/local/share", "/usr/share"):
        if d not in data:
            data.append(d)
    env["XDG_DATA_DIRS"] = ":".join(data)
    if env.get("LC_ALL") == "C.UTF-8":
        del env["LC_ALL"]  # the wrapper's default, not the player's
    return env


def open_external(target):
    """Opens a web address or a folder with the player's own browser or file manager: the system's
    xdg-open (or gio, kde-open) with host_environment(); GIO as a last resort. True when one of
    them started (whether the page opened is up to the desktop)."""
    env = host_environment()
    for command in (["xdg-open"], ["gio", "open"], ["kde-open"], ["exo-open"]):
        for folder in env["PATH"].split(":"):
            exe = Path(folder) / command[0]
            if not exe.is_file() or str(exe).startswith("/nix/"):
                continue
            try:
                subprocess.Popen([str(exe), *command[1:], str(target)], env=env, start_new_session=True,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return True
            except OSError:
                break
    try:
        uri = target if "://" in str(target) else Gio.File.new_for_path(str(target)).get_uri()
        return Gio.AppInfo.launch_default_for_uri(uri, None)
    except GLib.Error:
        return False


def open_folder(window, path):
    try:
        Path(path).mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    if not open_external(path):
        Gtk.FileLauncher.new(Gio.File.new_for_path(str(path))).launch(window, None, None)


def combo_row(title, subtitle, choices, current):
    model = Gtk.StringList.new([tr(choice[0]) for choice in choices])
    row = Adw.ComboRow(title=title, subtitle=subtitle or "", model=model)
    values = [choice[1] for choice in choices]
    row.set_selected(values.index(current) if current in values else 0)
    row.values = values
    return row


def combo_value(row):
    return row.values[row.get_selected()]


APP_ID = "io.github.sacredfire.Launcher"
# The game's log names this when the known clobbered-dword bug shows itself (gpu/shim/
# bbport_clobber.cpp AskForReport); the log page then asks for the log to be attached to the issue.
ISSUE2_MARKER = "SacredFire issue #2"
ISSUE2_URL = "https://github.com/T3CHNOLOG1C/SacredFire/issues/2"


def install_game_icon(game_dir):
    """The game's own icon (sce_sys/icon0.png) as this app's icon: copied into the user's icon
    theme under the application id, where GTK and the desktop find it (launcher/install-desktop.sh
    does the same for the menu entry). Game files never enter the repository."""
    try:
        source = Path(game_dir).expanduser() / "sce_sys/icon0.png"
        if not source.is_file():
            source = Path(__file__).resolve().parent / f"{APP_ID}.png"  # the package's own
        if not source.is_file():
            return False
        share = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
        target = share / "icons/hicolor/512x512/apps" / f"{APP_ID}.png"
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or target.stat().st_size != source.stat().st_size:
            target.write_bytes(source.read_bytes())
        return True
    except OSError:
        return False


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title=f"{TITLE} {port_version()}".rstrip())
        if install_game_icon(self.settings["game_dir"] if hasattr(self, "settings") else load_settings()["game_dir"]):
            self.set_icon_name(APP_ID)
        self.set_default_size(980, 760)
        self.set_size_request(360, 420)
        self.settings = load_settings()
        set_language(self.settings.get("ui_language", ""))
        self.ini, self.ini_lines = load_ini()
        self.process = None
        self.stream = None
        self.game_started = 0  # Unix time the game started, for Discord's "elapsed"
        self.presence = discord_ipc.Presence()
        self.party = None  # (players, max) of the lobby we host, from the server
        self.party_asked = 0.0
        self.party_busy = False
        self.announce_asked = 0.0
        self.announce_busy = False
        self.announce_seen = ""
        self.nick_info = None  # the server's answer about the nickname in the field
        self.nick_asked = ""   # "server|nick" of that answer
        self.nick_timer = 0
        self.discord_busy = False
        # A Steam shortcut points at <data dir>/SacredFire.AppImage: opening a newer release's
        # AppImage once moves the link to it, so the shortcut keeps working after an update.
        steam_shortcuts.stable_appimage_link()
        self.build()
        self.connect("close-request", self.on_close)
        GLib.timeout_add_seconds(1, self.tick)

    def build(self):
        install_css()
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        # The pages are listed in the sidebar on the left; the header names the one shown.
        self.page_title = Adw.WindowTitle(title=TITLE)
        header.set_title_widget(self.page_title)
        self.sidebar_button = Gtk.ToggleButton(icon_name="sidebar-show-symbolic", tooltip_text=tr("Pages"),
                                               visible=False)
        header.pack_start(self.sidebar_button)
        self.launch_button = Gtk.Button()
        self.launch_button.add_css_class("sf-play")
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)
        # A newer release (check_update): its version and an Update button, under the header.
        self.update_banner = Adw.Banner(revealed=False)
        self.update_banner.connect("button-clicked", self.on_update)
        toolbar.add_top_bar(self.update_banner)
        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        toolbar.set_content(self.toasts)
        # (name, title, icon, sidebar section)
        self.pages = [("settings", tr("Settings"), "preferences-system-symbolic", tr("Game")),
                      ("controls", tr("Controls"), "input-gaming-symbolic", tr("Game")),
                      ("patches", tr("Patches"), "applications-engineering-symbolic", tr("Content")),
                      ("mods", tr("Mods"), "application-x-addon-symbolic", tr("Content")),
                      ("dlc", tr("DLC"), "package-x-generic-symbolic", tr("Content")),
                      ("saves", tr("Saves"), "document-save-symbolic", tr("Progress")),
                      ("trophies", tr("Achievements"), "starred-symbolic", tr("Progress")),
                      ("log", tr("Log"), "utilities-terminal-symbolic", tr("Progress"))]
        self.split = Adw.OverlaySplitView(sidebar=self.build_sidebar(), content=toolbar,
                                          min_sidebar_width=200, max_sidebar_width=250)
        self.sidebar_button.connect("toggled", lambda b: self.split.set_show_sidebar(b.get_active()))
        self.split.connect("notify::show-sidebar",
                           lambda *_: self.sidebar_button.set_active(self.split.get_show_sidebar()))
        # A narrow window: the sidebar slides over the page, from the header's button.
        if hasattr(Adw, "Breakpoint"):
            narrow = Adw.Breakpoint.new(Adw.BreakpointCondition.parse("max-width: 700sp"))
            narrow.add_setter(self.split, "collapsed", True)
            narrow.add_setter(self.sidebar_button, "visible", True)
            self.add_breakpoint(narrow)
        self.set_content(self.split)
        builders = {"settings": self.build_settings_page, "controls": self.build_controls_page,
                    "patches": self.build_patches_page, "mods": self.build_mods_page, "dlc": self.build_dlc_page,
                    "saves": self.build_saves_page, "trophies": self.build_trophies_page, "log": self.build_log_page}
        for name, title, icon, _section in self.pages:
            self.stack.add_titled_with_icon(builders[name](), name, title, icon)
        self.stack.connect("notify::visible-child-name", lambda *_: self.on_page_shown())
        self.on_page_shown()
        self.update_launch_button()
        self.update_game_status()
        self.ask_nick_status()
        self.check_update()

    # --- sidebar -----------------------------------------------------------------------------

    def build_sidebar(self):
        view = Adw.ToolbarView()
        header = Adw.HeaderBar(show_end_title_buttons=False)
        header.set_title_widget(Adw.WindowTitle(title=TITLE, subtitle=port_version()))
        view.add_top_bar(header)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.add_css_class("sf-sidebar")
        # The game: its own icon (sce_sys/icon0.png) and edition, once a folder is chosen.
        brand = Gtk.Box(spacing=12)
        brand.add_css_class("sf-brand")
        self.brand_icon = Gtk.Image(pixel_size=52, icon_name="applications-games-symbolic")
        self.brand_icon.add_css_class("sf-brand-icon")
        brand.append(self.brand_icon)
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER, hexpand=True)
        title = Gtk.Label(label="Crash Team Racing", xalign=0, wrap=True)
        title.add_css_class("sf-brand-title")
        text.append(title)
        self.brand_sub = Gtk.Label(label="Nitro-Fueled", xalign=0, wrap=True)
        self.brand_sub.add_css_class("sf-brand-sub")
        text.append(self.brand_sub)
        brand.append(text)
        box.append(brand)
        self.nav = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE, margin_start=8, margin_end=8, margin_bottom=8)
        self.nav.add_css_class("navigation-sidebar")
        self.nav.add_css_class("sf-nav")
        for _name, title_text, icon, _section in self.pages:
            row_box = Gtk.Box(spacing=12)
            row_box.append(Gtk.Image(icon_name=icon))
            row_box.append(Gtk.Label(label=title_text, xalign=0, hexpand=True))
            self.nav.append(Gtk.ListBoxRow(child=row_box))
        self.nav.set_header_func(self.nav_header)
        self.nav.connect("row-selected", self.on_nav_selected)
        scroller = Gtk.ScrolledWindow(vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER, child=self.nav)
        box.append(scroller)
        view.set_content(box)
        return view

    def nav_header(self, row, before):
        section = self.pages[row.get_index()][3]
        if before is not None and self.pages[before.get_index()][3] == section:
            row.set_header(None)
            return
        label = Gtk.Label(label=section.upper(), xalign=0)
        label.add_css_class("sf-section")
        row.set_header(label)

    def on_nav_selected(self, _box, row):
        if row is None:
            return
        name = self.pages[row.get_index()][0]
        if self.stack.get_visible_child_name() != name:
            self.stack.set_visible_child_name(name)
        if self.split.get_collapsed():
            self.split.set_show_sidebar(False)

    def on_page_shown(self):
        """The sidebar and the header follow the page shown (also when the launcher switches it,
        as to the log when the game starts)."""
        name = self.stack.get_visible_child_name()
        for index, page in enumerate(self.pages):
            if page[0] == name:
                self.page_title.set_title(page[1])
                row = self.nav.get_row_at_index(index)
                if self.nav.get_selected_row() is not row:
                    self.nav.select_row(row)
        if name == "trophies":
            self.fill_trophies()

    def update_brand(self, path, title):
        texture = file_texture(Path(path) / "sce_sys" / "icon0.png") if self.settings["game_dir"] else None
        if texture is not None:
            self.brand_icon.set_from_paintable(texture)
        else:
            self.brand_icon.set_from_icon_name("applications-games-symbolic")
        region = REGIONS.get(title, "")
        self.brand_sub.set_label(f"Nitro-Fueled · {region} ({title})" if region and self.settings["game_dir"]
                                 else "Nitro-Fueled")

    def update_launch_button(self):
        running = self.process is not None
        self.launch_button.set_label(tr("Stop") if running else tr("Play"))
        self.launch_button.remove_css_class("destructive-action" if not running else "suggested-action")
        self.launch_button.add_css_class("destructive-action" if running else "suggested-action")

    # --- settings page -------------------------------------------------------------------

    def build_settings_page(self):
        page = Adw.PreferencesPage()

        game = Adw.PreferencesGroup(title=tr("Game"),
                                    description=tr("The dumped game with its 1.21 update merged over the base (eboot.bin, sce_module, ...)"))
        self.game_row = Adw.ActionRow(title=tr("Game folder (CUSA13795 or CUSA14876)"))
        self.game_status = Gtk.Image(valign=Gtk.Align.CENTER)
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(flat_button("folder-open-symbolic", tr("Choose the folder with eboot.bin"), self.on_choose_game))
        game.add(self.game_row)
        self.user_row = Adw.ActionRow(title=tr("Saves and game data folder"))
        self.user_row.add_suffix(flat_button("folder-open-symbolic", tr("Choose the saves and game data folder"), self.on_choose_user))
        self.user_row.add_suffix(flat_button("edit-undo-symbolic", tr("Back to the default folder"), self.on_reset_user))
        self.user_row.add_suffix(flat_button("folder-symbolic", tr("Open in the file manager"),
                                             lambda _b: open_folder(self, self.user_dir())))
        game.add(self.user_row)
        # The chosen game's edition from the start: built for the USA list first, an EU language
        # (Italian, German...) showed as English, and that English was then kept and saved
        # (issue #111).
        self.language_title = set_game_title(self.game_dir()) if self.settings["game_dir"] else GAME_TITLE[0]
        self.language_row = combo_row(tr("Game language"), tr("The languages of your edition of the game"),
                                      languages_for(self.language_title),
                                      language_for(self.language_title, self.settings["language"]))
        game.add(self.language_row)
        page.add(game)

        online = Adw.PreferencesGroup(title=tr("Online"),
                                      description=tr("Online races, private rooms and the Pit Stop. Your nickname is yours on the server: "
                                                     "this installation keeps it (ryunet_online.key in the data folder); "
                                                     "link it to Discord to recover it after a reinstall or on another PC."))
        self.online_row = Adw.SwitchRow(title=tr("Play online"), active=self.ini.get("online") == "1")
        online.add(self.online_row)
        self.nick_row = Adw.EntryRow(title=tr("Nickname (3-16 letters, digits, _ or -)"),
                                     text=self.ini.get("online_name", ""))
        self.nick_row.connect("changed", lambda _row: self.nick_changed())
        online.add(self.nick_row)
        # What the server says about the nickname, and its Discord account: link it, recover
        # it, unlink it (the Windows launcher's Online tab).
        self.nick_status_row = Adw.ActionRow(title=tr("Nickname on the server"))
        self.discord_link_button = Gtk.Button(label=tr("Link with Discord"), valign=Gtk.Align.CENTER)
        self.discord_link_button.connect("clicked", lambda _b: self.start_discord_link("link"))
        self.nick_status_row.add_suffix(self.discord_link_button)
        self.discord_recover_button = Gtk.Button(label=tr("Recover with Discord"), valign=Gtk.Align.CENTER)
        self.discord_recover_button.set_tooltip_text(tr("Reinstalled or on another PC? Authorize the Discord account your "
                                                        "nickname is linked to and the launcher fills it in."))
        self.discord_recover_button.connect("clicked", lambda _b: self.start_discord_link("recover"))
        self.nick_status_row.add_suffix(self.discord_recover_button)
        self.discord_unlink_button = Gtk.Button(label=tr("Unlink"), valign=Gtk.Align.CENTER)
        self.discord_unlink_button.add_css_class("flat")
        self.discord_unlink_button.connect("clicked", lambda _b: self.confirm_discord_unlink())
        self.nick_status_row.add_suffix(self.discord_unlink_button)
        online.add(self.nick_status_row)
        self.server_row = Adw.EntryRow(title=tr("Server"), text=self.ini.get("online_server") or ONLINE_SERVER)
        self.server_row.connect("changed", lambda _row: self.nick_changed())
        online.add(self.server_row)
        # Friends are found, added and invited in the game's menu (Insert / L3+R3 > Friends); the
        # old field's names (online_friends) get friend requests once (src/runtime_friends.c).
        online.add(Adw.ActionRow(title=tr("Friends"),
                                 subtitle=tr("In the game: menu (Insert or L3+R3) > Friends")))
        self.inventory_row = combo_row(tr("Pit Stop items"), None, [(tr(label), value) for label, value in INVENTORIES],
                                       self.ini.get("online_inventory", "server"))
        online.add(self.inventory_row)
        page.add(online)

        # The rules of the online, and where to report and appeal.
        online_rules = (
            tr("No cheating: hacks, trainers, speed or item modifications, or fake leaderboard times. "
               "They ruin the game for everyone."),
            tr("Respect others: no offensive nicknames, insults or harassment."),
            tr("Do not sabotage races or quit on purpose to annoy others."),
            tr("Use your own nickname: do not impersonate anyone."),
            tr("See someone cheating? Report them in the game: menu (Insert / L3+R3) > Friends > Report a player."),
        )
        rules = Adw.PreferencesGroup(
            title=tr("Online rules"),
            description="\n".join("• " + rule for rule in online_rules) + "\n\n" +
            tr("Breaking the rules can cost you your records and a ban from online play, temporary or permanent, "
               "also by IP."))
        page.add(rules)

        discord = Adw.PreferencesGroup(
            title="Discord",
            description=tr("Invite to a private match or your party: while you play online, press + in a Discord chat and "
                           "choose \"Invite to play\". Whoever accepts gets you as a friend and their game starts; in it, "
                           "they open Friends and join you. Both PCs need this launcher (or the Windows one) and Discord."))
        self.discord_rpc_row = Adw.SwitchRow(
            title=tr("Show activity on Discord"),
            subtitle=tr("While you play, your Discord status shows the game, online or offline, your nickname and the "
                        "time played. Needs the Discord app running on this PC."),
            active=self.settings.get("discord_rpc", True))
        self.discord_rpc_row.connect("notify::active", lambda *_: self.store())
        discord.add(self.discord_rpc_row)
        self.discord_accept_row = Adw.SwitchRow(
            title=tr("Accept people who ask to join"),
            subtitle=tr("When someone presses \"Ask to Join\" on your profile, they get the same as from an invite of "
                        "yours (your nickname and your server)."),
            active=self.settings.get("discord_accept_requests", True))
        self.discord_accept_row.connect("notify::active", lambda *_: self.store())
        discord.add(self.discord_accept_row)
        self.discord_status_row = Adw.ActionRow(title=tr("Connection"))
        discord.add(self.discord_status_row)
        page.add(discord)

        console = Adw.PreferencesGroup(title=tr("Console"))
        self.neo_row = Adw.SwitchRow(title=tr("PS4 Pro mode"),
                                     subtitle=tr("Neo mode, 7 CPUs, 5568 MiB direct memory: the game's Pro rendering path"),
                                     active=self.settings["neo"])
        console.add(self.neo_row)
        page.add(console)

        performance = Adw.PreferencesGroup(
            title=tr("Performance"),
            description=tr("Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings."))
        self.fps_limit_row = combo_row(tr("FPS limit"), tr("Above 30 it needs the 60 FPS unlock patch; also in the in-game menu"),
                                       FPS_LIMITS, self.ini.get("fps_limit", "60"))
        performance.add(self.fps_limit_row)
        self.async_row = Adw.SwitchRow(
            title=tr("Compile shaders in the background (experimental)"),
            subtitle=tr("Fewer hitches when an effect is first seen; an effect may show up a frame late."),
            active=self.ini.get("async_pipelines") == "1")
        performance.add(self.async_row)
        page.add(performance)

        screen = Adw.PreferencesGroup(title=tr("Display"))
        self.renderer_row = combo_row(
            tr("Graphics engine"),
            tr("Vulkan (shadPS4) is the usual engine. The native one draws the game straight with Vulkan, without "
               "emulating the PS4's GPU: more FPS and fewer hitches, but it is experimental. The first time it builds "
               "its shaders from your copy of the game (a few minutes). The native engine has no FSR 3 (FSR 1 works). "
               "If something goes wrong, go back to Vulkan (shadPS4)."),
            RENDERERS, self.ini.get("renderer", "shadps4"))
        screen.add(self.renderer_row)
        self.fullscreen_row = Adw.SwitchRow(title=tr("Fullscreen"), active=self.settings["fullscreen"])
        screen.add(self.fullscreen_row)
        self.borderless_row = Adw.SwitchRow(title=tr("Borderless window"),
                                            subtitle=tr("Removes window borders in windowed mode."),
                                            active=self.ini.get("borderless") == "1")
        screen.add(self.borderless_row)
        displays = connected_displays()
        display_choices = [(tr("Primary"), "")] + displays
        if self.settings.get("display") and self.settings["display"] not in [n for n, _ in displays]:
            display_choices.append((tr("Monitor {} (not connected)").format(self.settings["display"]), self.settings["display"]))
        self.display_row = combo_row(tr("Monitor"), tr("The screen the game window and fullscreen go to"),
                                     display_choices, self.settings.get("display", ""))
        screen.add(self.display_row)
        self.present_row = combo_row(tr("Presentation"), None, PRESENT_MODES, self.settings["present_mode"])
        screen.add(self.present_row)
        self.render_res_row = combo_row(tr("Internal resolution"), tr("Also in the in-game menu"),
                                        RENDER_RESOLUTIONS, self.ini.get("render_res", ""))
        self.render_res_row.set_tooltip_text(
            tr("The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path and outputs 4K, "
               "which removes jagged edges even on a 1080p screen; 4K needs a strong GPU. 720p and 480p help weaker "
               "PCs and the Steam Deck."))
        self.render_res_row.connect("notify::selected", lambda *_: self.update_render_res())
        screen.add(self.render_res_row)
        self.layout_row = combo_row(
            tr("Aspect ratio"),
            tr("16:10 is experimental and keeps the original menus and HUD. 21:9 and 32:9 include menu and HUD corrections. "
               "USA or Europe 1.21; otherwise the game starts at 16:9. No FSR or DLSS. Requires a restart."),
            ASPECT_RATIOS, self.ini.get("aspect_ratio", self.ini.get("widescreen_layout", "16x9")))
        screen.add(self.layout_row)
        self.layout_output_row = Adw.ComboRow(title=tr("Screen size"))
        self.layout_output_row.values = []
        screen.add(self.layout_output_row)
        self.layout_row.connect("notify::selected", lambda *_: self.update_layout())
        self.msaa_row = Adw.SwitchRow(
            title=tr("Game MSAA (2x)"),
            subtitle=tr("The PS4's 2x anti-aliasing. Off by default: on PC it draws dotted lines in the sky and in light "
                        "shafts. At high internal resolutions or with FSR it is barely missed. Needs a restart."),
            active=self.ini.get("msaa") == "1")
        screen.add(self.msaa_row)
        temporal = self.ini.get("temporal", "")
        if temporal not in [v for _l, v in UPSCALERS]:
            # Older settings: fsr1=1 / fsr3=1; "dlss" (a Windows setting) runs FSR 3 here.
            temporal = "fsr3" if temporal == "dlss" or self.ini.get("fsr3") == "1" else \
                "fsr1" if self.ini.get("fsr1") == "1" else "off"
        self.upscaler_row = combo_row(
            tr("Upscaling"),
            tr("FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3 rebuilds detail "
               "over several frames from the game's depth and motion, and smooths edges even at the same resolution. "
               "Experimental: switch to FSR 1 if you see trails or shimmering."),
            UPSCALERS, temporal)
        self.upscaler_row.connect("notify::selected", lambda *_: self.update_upscaler())
        screen.add(self.upscaler_row)
        sharpness = Gtk.Adjustment(lower=0.0, upper=1.0, step_increment=0.05, page_increment=0.1)
        try:
            sharpness.set_value(float(self.ini.get("fsr3_sharpness", "0.30")))
        except ValueError:
            sharpness.set_value(0.30)
        self.sharpness_row = Adw.SpinRow(title=tr("FSR 3 sharpness"), adjustment=sharpness, digits=2)
        screen.add(self.sharpness_row)
        self.fps_row = Adw.SwitchRow(title=tr("FPS counter in the corner"), active=self.ini.get("show_fps") == "1")
        screen.add(self.fps_row)
        self.ping_row = Adw.SwitchRow(
            title=tr("Ping counter in the corner"),
            subtitle=tr("In an online room, each player's ping in ms, under the FPS counter. Also in the in-game menu "
                        "(Insert or L3+R3), Room tab."),
            active=self.ini.get("show_ping") == "1")
        screen.add(self.ping_row)
        page.add(screen)

        sound = Adw.PreferencesGroup(title=tr("Sound"))
        volume_row = Adw.ActionRow(
            title=tr("Game volume"),
            subtitle=tr("100% is the level the game was mixed at. Above it, the loudest peaks are rounded off instead of "
                        "distorting. While you play it can also be changed in the in-game menu (Insert or L3+R3)."))
        try:
            volume = max(0, min(200, int(self.ini.get("volume", VOLUME_DEFAULT))))
        except ValueError:
            volume = VOLUME_DEFAULT
        self.volume_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 200, 5)
        self.volume_scale.set_value(volume)
        self.volume_scale.set_size_request(220, -1)
        self.volume_scale.set_valign(Gtk.Align.CENTER)
        self.volume_scale.set_draw_value(True)
        self.volume_scale.set_format_value_func(lambda _scale, value: f"{int(value)}%")
        self.volume_scale.add_mark(100, Gtk.PositionType.BOTTOM, None)
        volume_row.add_suffix(self.volume_scale)
        sound.add(volume_row)
        page.add(sound)

        self.update_render_res()
        self.update_upscaler()
        self.update_layout()

        dev = Adw.PreferencesGroup(title=tr("Developer"))
        self.mangohud_row = Adw.SwitchRow(title="MangoHud", active=self.settings["mangohud"])
        dev.add(self.mangohud_row)
        self.stats_row = Adw.SwitchRow(title=tr("Frame statistics in the log"), active=self.settings["frame_stats"])
        dev.add(self.stats_row)
        self.save_log_row = Adw.SwitchRow(title=tr("Save the log to a file (logs/)"), active=self.settings["save_log"])
        dev.add(self.save_log_row)
        self.cache_row = Adw.SwitchRow(title=tr("Clear the shader cache at start"),
                                       subtitle=tr("The cache is kept by default so known areas do not compile shaders mid-race; clear it after GPU driver updates or rendering glitches"),
                                       active=self.settings["clear_shader_cache"])
        dev.add(self.cache_row)
        self.validation_row = Adw.SwitchRow(title=tr("Vulkan validation layer (slow)"), active=self.settings["vk_validation"])
        dev.add(self.validation_row)
        self.env_row = Adw.EntryRow(title=tr("Extra environment (VAR=value ...)"), text=self.settings["extra_env"])
        dev.add(self.env_row)
        page.add(dev)

        window = Adw.PreferencesGroup(title=tr("Window and Launcher"))
        self.ui_language_row = combo_row(tr("Launcher language"), tr("Applies after restarting the launcher"),
                                         UI_LANGUAGES, self.settings["ui_language"])
        window.add(self.ui_language_row)
        self.hide_on_play_row = Adw.SwitchRow(
            title=tr("Hide launcher window while playing"),
            subtitle=tr("Hides the launcher window completely while playing and restores it automatically when exiting."),
            active=self.settings.get("hide_on_play", False))
        window.add(self.hide_on_play_row)
        self.close_on_play_row = Adw.SwitchRow(
            title=tr("Close launcher after playing"),
            subtitle=tr("Closes the launcher once you exit the game (it stays open if the game crashed, with its log)."),
            active=self.settings.get("close_on_play", False))
        window.add(self.close_on_play_row)
        page.add(window)

        steam = Adw.PreferencesGroup(title=tr("Steam"))
        exe, _start, _options = steam_shortcuts.launcher_command()
        steam_row = Adw.ActionRow(title=tr("Add to Steam"),
                                  subtitle=tr("A non-Steam shortcut that starts the game with these settings (Steam Deck game mode). Close Steam before adding."))
        steam_row.set_tooltip_text(exe)
        add_button = Gtk.Button(label=tr("Add"), valign=Gtk.Align.CENTER)
        add_button.connect("clicked", self.on_add_to_steam)
        steam_row.add_suffix(add_button)
        steam.add(steam_row)
        page.add(steam)

        # The version, and updates from GitHub releases (appimage_update.py), as on Windows.
        about = Adw.PreferencesGroup(title=tr("Version"))
        self.version_row = Adw.ActionRow(title=port_version() or tr("Unknown version"),
                                         subtitle=tr("Checked against the latest release at start"))
        check_button = Gtk.Button(label=tr("Check for updates"), valign=Gtk.Align.CENTER)
        check_button.connect("clicked", lambda _b: self.check_update(manual=True))
        self.version_row.add_suffix(check_button)
        about.add(self.version_row)
        page.add(about)
        self.update_user_status()
        return page

    # --- updates ---------------------------------------------------------------------------

    def check_update(self, manual=False):
        """Asks GitHub for the latest release in the background; a newer one shows the banner."""
        import threading
        if manual:
            self.version_row.set_subtitle(tr("Checking..."))

        def run():
            try:
                release = appimage_update.check(port_version())
                error = None
            except (OSError, ValueError) as e:
                release, error = None, str(e)
            GLib.idle_add(lambda: self.show_update(release, error, manual) and False)
        threading.Thread(target=run, daemon=True).start()

    def show_update(self, release, error, manual):
        self.update_release = release
        if error:
            self.version_row.set_subtitle(tr("Could not ask GitHub: ") + error)
            return
        if not release:
            self.version_row.set_subtitle(tr("This is the latest version") if manual
                                          else tr("Checked against the latest release at start"))
            return
        print(f"Launcher: new version {release['tag']} available ({release['name']})", flush=True)
        self.version_row.set_subtitle(tr("New version: %s") % release["tag"])
        self.update_banner.set_title(tr("New version: %s") % release["tag"])
        self.update_banner.set_button_label(tr("Update") if appimage_update.running_appimage() else tr("Download"))
        self.update_banner.set_revealed(True)

    def on_update(self, _banner):
        release = getattr(self, "update_release", None)
        appimage = appimage_update.running_appimage()
        if not release:
            return
        if not appimage:  # a checkout or a Nix build: the release page
            open_external(appimage_update.RELEASES_PAGE)
            return
        if self.process is not None:
            self.toasts.add_toast(Adw.Toast(title=tr("Close the game to update.")))
            return
        import threading
        self.update_banner.set_button_label("")
        state = {"shown": -1}

        def progress(done, total):
            percent = done * 100 // max(total, 1)
            if percent != state["shown"]:
                state["shown"] = percent
                GLib.idle_add(lambda: self.update_banner.set_title(
                    tr("Downloading %s... %d%%") % (release["tag"], percent)) and False)

        def run():
            try:
                new = appimage_update.install(release, appimage, progress)
                GLib.idle_add(lambda: self.restart_into(new) and False)
            except OSError as e:
                message = str(e)
                GLib.idle_add(lambda: self.update_failed(message) and False)
        threading.Thread(target=run, daemon=True).start()

    def update_failed(self, message):
        self.update_banner.set_title(tr("The update failed (nothing was changed): ") + message)
        self.update_banner.set_button_label(tr("Retry"))

    def restart_into(self, appimage):
        """The new AppImage starts in place of this launcher (the stable Steam link follows it)."""
        self.store()
        subprocess.Popen([str(appimage)], env=host_environment(), start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.presence.shutdown()
        self.get_application().quit()

    def update_render_res(self):
        """A chosen internal resolution decides the PS4 Pro path and the 1080p Pro patch."""
        chosen = bool(combo_value(self.render_res_row))
        self.neo_row.set_sensitive(not chosen)
        self.neo_row.set_subtitle(tr("Set by the internal resolution (Display)") if chosen else
                                  tr("Neo mode, 7 CPUs, 5568 MiB direct memory: the game's Pro rendering path"))
        self.fill_patches()  # the 1080p Pro patch follows the internal resolution

    def update_layout(self, output=None):
        """The screen sizes of the chosen layout (none for 16:9)."""
        layout = combo_value(self.layout_row)
        choices = ASPECT_OUTPUTS.get(layout, [])
        current = (output or (combo_value(self.layout_output_row) if self.layout_output_row.values else None)
                   or self.ini.get("aspect_output", self.ini.get("widescreen_output", "")))
        values = [v for _l, v in choices]
        self.layout_output_row.set_model(Gtk.StringList.new([label for label, _v in choices]))
        self.layout_output_row.values = values
        self.layout_output_row.set_selected(values.index(current) if current in values else 0)
        self.layout_output_row.set_visible(bool(choices))

    def update_upscaler(self):
        self.sharpness_row.set_visible(combo_value(self.upscaler_row) == "fsr3")

    def game_dir(self):
        return Path(self.settings["game_dir"]).expanduser()

    def user_dir(self):
        return Path(self.settings["user_dir"]).expanduser() if self.settings["user_dir"] else DATA_DIR / "user"

    def update_user_status(self):
        # The game keeps all of its user data here (saves, shader cache, ...): a new folder starts
        # empty, so copy the saves into it to keep them.
        self.user_row.set_subtitle(str(self.user_dir()) + ("" if self.settings["user_dir"] else tr(" (default)")) + "\n" +
                                   tr("Saves, shader cache and the game's other data: a new folder starts empty"))

    def update_game_status(self):
        path = self.game_dir()
        title = set_game_title(path)
        self.update_brand(path, title)
        if hasattr(self, "language_row") and title != self.language_title:
            # Another edition: its own languages (Spanish follows: Latin American / Spain's).
            current = combo_value(self.language_row)
            self.language_title = title
            choices = languages_for(title)
            self.language_row.set_model(Gtk.StringList.new([tr(label) for label, _code in choices]))
            self.language_row.values = [code for _label, code in choices]
            self.language_row.set_selected(self.language_row.values.index(language_for(title, current)))
            self.fill_patches()
        ok = bool(self.settings["game_dir"]) and (path / "eboot.bin").is_file()
        self.game_row.set_subtitle(str(path) if self.settings["game_dir"] else tr("not chosen"))
        problem = game_check.problem(path) if ok else None
        profile = game_check.profile(path) if ok and not problem else None
        if not ok:
            tooltip = tr("No eboot.bin in the folder")
        elif problem:
            tooltip = game_check.explain(*problem)
            self.game_row.set_subtitle(f"{path}\n{tooltip}")
        elif profile and profile["name"] != "ctr":
            tooltip = tr("This is {}; this launcher runs Crash Team Racing: Nitro-Fueled").format(profile["game"])
            problem = ("other_title", profile["title"], profile["version"])
            self.game_row.set_subtitle(f"{path}\n{tooltip}")
        else:
            info = game_check.sfo_info(path)
            tooltip = f"{GAME} {info.get('TITLE_ID', '')}, {info.get('APP_VER', '').lstrip('0')}"
        self.game_status.set_from_icon_name("object-select-symbolic" if ok and not problem else "dialog-warning-symbolic")
        self.game_status.set_tooltip_text(tooltip)
        self.launch_button.set_sensitive((ok and not problem) or self.process is not None)

    def choose_folder(self, title, initial, chosen):
        dialog = Gtk.FileDialog(title=title, modal=True)
        if initial and Path(initial).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(initial)))

        def done(dialog, result):
            try:
                folder = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if folder:
                chosen(folder.get_path())
        dialog.select_folder(self, None, done)

    def on_choose_game(self, _button):
        def chosen(path):
            self.settings["game_dir"] = path
            self.update_game_status()
            self.store()
        self.choose_folder(tr("Game folder (with eboot.bin)"), self.game_dir(), chosen)

    def on_choose_user(self, _button):
        def chosen(path):
            self.settings["user_dir"] = path
            self.update_user_status()
            self.store()
        self.choose_folder(tr("Saves folder"), self.user_dir(), chosen)

    def on_reset_user(self, _button):
        self.settings["user_dir"] = ""
        self.update_user_status()
        self.store()

    def store(self):
        s = self.settings
        s["language"] = combo_value(self.language_row)
        s["gamepad"] = combo_value(self.gamepad_row)
        s["gamepad_name"] = self.gamepad_row.get_selected_item().get_string() if s["gamepad"] else ""
        s["neo"] = self.neo_row.get_active()
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["present_mode"] = combo_value(self.present_row)
        s["mangohud"] = self.mangohud_row.get_active()
        s["frame_stats"] = self.stats_row.get_active()
        s["save_log"] = self.save_log_row.get_active()
        s["clear_shader_cache"] = self.cache_row.get_active()
        s["vk_validation"] = self.validation_row.get_active()
        s["extra_env"] = self.env_row.get_text()
        s["ui_language"] = combo_value(self.ui_language_row)
        s["discord_rpc"] = self.discord_rpc_row.get_active()
        s["discord_accept_requests"] = self.discord_accept_row.get_active()
        s["display"] = combo_value(self.display_row)
        s["hide_on_play"] = self.hide_on_play_row.get_active()
        s["close_on_play"] = self.close_on_play_row.get_active()
        save_settings(s)
        self.ini, self.ini_lines = load_ini()
        self.ini["show_fps"] = "1" if self.fps_row.get_active() else "0"
        self.ini["show_ping"] = "1" if self.ping_row.get_active() else "0"
        self.ini["split_auto"] = "1" if self.split_auto_row.get_active() else "0"
        self.ini["button_prompts"] = combo_value(self.button_prompts_row)
        self.ini["async_pipelines"] = "1" if self.async_row.get_active() else "0"
        self.ini["msaa"] = "1" if self.msaa_row.get_active() else "0"
        self.ini["borderless"] = "1" if self.borderless_row.get_active() else "0"
        self.ini["temporal"] = combo_value(self.upscaler_row)
        self.ini["renderer"] = combo_value(self.renderer_row)
        self.ini["fsr1"] = self.ini["fsr3"] = None  # older keys: temporal decides
        self.ini["fsr3_sharpness"] = f"{self.sharpness_row.get_value():.2f}"
        self.ini["volume"] = str(int(self.volume_scale.get_value()))
        self.ini["fps_limit"] = combo_value(self.fps_limit_row)
        self.ini["render_res"] = combo_value(self.render_res_row) or None  # "": the Pro switches decide
        layout = combo_value(self.layout_row)
        self.ini["aspect_ratio"] = layout
        self.ini["widescreen_layout"] = layout if layout in ("21x9", "32x9") else "off"
        if layout != "16x9":
            self.ini["aspect_output"] = combo_value(self.layout_output_row)
            if layout in ("21x9", "32x9"):
                self.ini["widescreen_output"] = self.ini["aspect_output"]
            self.ini["ultrawide"] = "0"  # the layout sets the size (the in-game ultrawide would not correct the menus)
        self.ini["online"] = "1" if self.online_row.get_active() else "0"
        self.ini["online_name"] = self.nick_row.get_text().strip()
        self.ini["online_server"] = self.server_row.get_text().strip() or ONLINE_SERVER
        self.ini["online_inventory"] = combo_value(self.inventory_row)
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def reload_game_settings(self):
        """bbport.ini as the in-game menu left it: the switches show what the game will use."""
        self.ini, self.ini_lines = load_ini()
        self.fill_patches()
        self.fps_row.set_active(self.ini.get("show_fps") == "1")
        self.ping_row.set_active(self.ini.get("show_ping") == "1")
        self.split_auto_row.set_active(self.ini.get("split_auto") != "0")
        prompts = self.ini.get("button_prompts", "playstation")
        self.button_prompts_row.set_selected(self.button_prompts_row.values.index(prompts)
                                             if prompts in self.button_prompts_row.values else 0)
        self.async_row.set_active(self.ini.get("async_pipelines") == "1")
        self.msaa_row.set_active(self.ini.get("msaa") == "1")
        self.borderless_row.set_active(self.ini.get("borderless") == "1")
        renderer = self.ini.get("renderer", "shadps4")
        if renderer in self.renderer_row.values:
            self.renderer_row.set_selected(self.renderer_row.values.index(renderer))
        temporal = self.ini.get("temporal", "off")
        if temporal in self.upscaler_row.values:
            self.upscaler_row.set_selected(self.upscaler_row.values.index(temporal))
        try:
            self.volume_scale.set_value(max(0, min(200, int(self.ini.get("volume", VOLUME_DEFAULT)))))
        except ValueError:
            pass
        self.show_port_players()
        limit = self.ini.get("fps_limit", "60")
        values = self.fps_limit_row.values
        self.fps_limit_row.set_selected(values.index(limit) if limit in values else values.index("60"))
        res = self.ini.get("render_res", "")
        self.render_res_row.set_selected(self.render_res_row.values.index(res) if res in self.render_res_row.values else 0)
        self.update_render_res()
        layout = self.ini.get("aspect_ratio", self.ini.get("widescreen_layout", "16x9"))
        self.layout_row.set_selected(self.layout_row.values.index(layout) if layout in self.layout_row.values else 0)
        self.update_layout(self.ini.get("aspect_output", self.ini.get("widescreen_output", "")))

    def environment(self):
        return game_environment(self.settings)

    def set_ini(self, changes):
        """Writes some bbport.ini keys at once (None removes a line: the default again),
        keeping whatever the in-game menu changed meanwhile."""
        self.ini, self.ini_lines = load_ini()
        self.ini.update(changes)
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def ini_has(self, key):
        """Whether bbport.ini has its own line for key (a binding of the player's)."""
        return any(line.split("=", 1)[0].strip() == key for line in self.ini_lines
                   if "=" in line and not line.lstrip().startswith("#"))

    # --- controls page -------------------------------------------------------------------

    def build_controls_page(self):
        page = Adw.PreferencesPage()
        controls = Adw.PreferencesGroup(title=tr("Controller"),
                                        description=tr("Player 1 uses the selected controller. If it is disconnected, an assigned controller "
                                                       "from Players 2–4 temporarily becomes Player 1. Players 2–4 play as guests: "
                                                       "nothing is kept for them after the game closes. The in-game menu "
                                                       "(Insert / L3+R3) can also change ports."))
        pads = connected_gamepads()
        primary = self.ini.get("port.1") or self.settings["gamepad"]
        choices = [(tr("First available"), "")] + [(name, guid) for guid, name in pads]
        if primary and primary not in [guid for guid, _name in pads]:
            choices.append((tr("{} (not connected)").format(self.settings["gamepad_name"] or primary), primary))
        self.gamepad_row = combo_row(tr("Player 1 controller"), None, choices, primary)
        self.gamepad_row.connect("notify::selected", lambda *_: self.on_primary_controller())
        controls.add(self.gamepad_row)
        self.port_rows = {}
        for n in (2, 3, 4):
            current = self.ini.get(f"port.{n}", "")
            choices = [(tr("Not assigned"), ""), (tr("Keyboard"), "keyboard")]
            choices += [(name, guid) for guid, name in pads]
            if current and current not in [value for _label, value in choices]:
                choices.append((tr("{} (not connected)").format(current), current))
            row = combo_row(tr("Player {}").format(n), None, choices, current)
            row.connect("notify::selected", lambda _row, _param, player=n: self.on_port_player(player))
            self.port_rows[n] = row
            controls.add(row)
        # bbport.ini split_auto (src/runtime_pad.c): players 2-4 without a controller chosen take the
        # next connected one.
        self.split_auto_row = Adw.SwitchRow(
            title=tr("Split screen: assign controllers automatically"),
            subtitle=tr("Players 2–4 left on Not assigned take the other controllers in the order they were connected. "
                        "Off, they are not used, and another controller you pick up takes over Player 1."),
            active=self.ini.get("split_auto") != "0")
        controls.add(self.split_auto_row)
        # bbport.ini button_prompts (scripts/button_prompts.py): the buttons the game shows.
        self.button_prompts_row = combo_row(
            tr("Button prompts"),
            tr("The buttons the game shows in menus and prompts. Xbox: A, B, X, Y, LB, RB, LT, RT and View. "
               "Applies when you press Play."),
            [("PlayStation", "playstation"), ("Xbox", "xbox")], self.ini.get("button_prompts", "playstation"))
        controls.add(self.button_prompts_row)
        page.add(controls)

        # Bindings: "Change" waits for a key or button in a small window (bb-gpu-capabilities
        # --read-input, the same names the game reads). Grey: the game's default.
        self.binding_rows = {}
        for kind, title, description, inputs in (
                ("pad", tr("Gamepad buttons"),
                 tr("Player 1's buttons on Xbox, PlayStation, Switch Pro and generic controllers. Saved in bbport.ini, "
                    "used from the next start."), PAD_INPUTS),
                ("key", tr("Keyboard"),
                 tr("Player 1's keys; the keyboard works together with the controller. Each action takes up to 4 keys "
                    "(Change adds one to your own; Default goes back to the game's). Insert opens the in-game menu, "
                    "so it cannot be bound."), KEY_INPUTS)):
            group = Adw.PreferencesGroup(title=title, description=description)
            for name, label, default in inputs:
                row = Adw.ActionRow(title=tr(label))
                change = Gtk.Button(label=tr("Change"), valign=Gtk.Align.CENTER)
                change.connect("clicked", lambda _b, k=kind, n=name: self.capture_binding(k, n))
                row.add_suffix(change)
                row.add_suffix(flat_button("edit-clear-symbolic", tr("No key or button"),
                                           lambda _b, k=kind, n=name: self.set_binding(k, n, "")))
                row.add_suffix(flat_button("edit-undo-symbolic", tr("Default"),
                                           lambda _b, k=kind, n=name: self.set_binding(k, n, None)))
                group.add(row)
                self.binding_rows[(kind, name)] = (row, default)
                self.show_binding(kind, name)
            reset = Adw.ActionRow(title=tr("Restore all"))
            reset_button = Gtk.Button(label=tr("Restore"), valign=Gtk.Align.CENTER)
            reset_button.connect("clicked", lambda _b, k=kind, i=inputs: self.set_ini(
                {f"{k}.{name}": None for name, _l, _d in i}) or self.show_bindings())
            reset.add_suffix(reset_button)
            group.add(reset)
            if kind == "pad":
                preset = Adw.ActionRow(title=tr("D-pad down = L2 (use item)"),
                                       subtitle=tr("Swaps D-pad down with the L2 trigger: items on the down arrow"))
                preset_button = Gtk.Button(label=tr("Apply"), valign=Gtk.Align.CENTER)
                preset_button.connect("clicked", lambda _b: self.set_ini(
                    {"pad.l2": "dpdown", "pad.down": "lefttrigger"}) or self.show_bindings())
                preset.add_suffix(preset_button)
                group.add(preset)
            page.add(group)
        return page

    def on_primary_controller(self):
        chosen = combo_value(self.gamepad_row)
        if chosen == (self.ini.get("port.1") or self.settings["gamepad"]):
            return
        self.settings["gamepad"] = chosen
        self.settings["gamepad_name"] = self.gamepad_row.get_selected_item().get_string() if chosen else ""
        save_settings(self.settings)
        self.set_ini({"port.1": chosen or None})

    def on_port_player(self, player):
        chosen = combo_value(self.port_rows[player])
        if chosen == self.ini.get(f"port.{player}", ""):
            return
        changes = {f"port.{player}": chosen or None}
        if chosen == "keyboard":
            changes.update({f"port.{n}": None for n in (2, 3, 4) if n != player and
                            self.ini.get(f"port.{n}", "").lower() == "keyboard"})
        self.set_ini(changes)
        self.show_port_players()

    def show_port_players(self):
        primary = self.ini.get("port.1") or self.settings["gamepad"]
        if primary not in self.gamepad_row.values:
            self.gamepad_row.get_model().append(tr("{} (not connected)").format(primary))
            self.gamepad_row.values.append(primary)
        self.gamepad_row.set_selected(self.gamepad_row.values.index(primary))
        for n, row in self.port_rows.items():
            current = self.ini.get(f"port.{n}", "")
            if current not in row.values:
                row.get_model().append(tr("{} (not connected)").format(current))
                row.values.append(current)
            row.set_selected(row.values.index(current))

    def binding_text(self, kind, value):
        names = [n.strip() for n in value.split(",") if n.strip()]
        if kind == "pad":
            names = [PAD_LABELS.get(n, n) for n in names]
        return "  /  ".join(names)

    def show_binding(self, kind, name):
        row, default = self.binding_rows[(kind, name)]
        key = f"{kind}.{name}"
        if self.ini_has(key):
            value = self.ini.get(key, "")
            row.set_subtitle(GLib.markup_escape_text(self.binding_text(kind, value)) if value.strip()
                             else tr("(none)"))
        else:
            row.set_subtitle("<span alpha='60%'>" + GLib.markup_escape_text(
                tr("{} (default)").format(self.binding_text(kind, default))) + "</span>")

    def show_bindings(self):
        for kind, name in self.binding_rows:
            self.show_binding(kind, name)

    def set_binding(self, kind, name, value):
        """value: the binding, "" for none, None for the game's default."""
        self.set_ini({f"{kind}.{name}": value})
        self.show_binding(kind, name)

    def capture_binding(self, kind, name):
        tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
        try:
            process = Gio.Subprocess.new([str(tool), "--read-input", kind], Gio.SubprocessFlags.STDOUT_PIPE)
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Could not start: {}").format(error.message)))
            return
        row, _default = self.binding_rows[(kind, name)]
        row.set_subtitle(tr("Press a gamepad button… (Escape cancels)") if kind == "pad"
                         else tr("Press a key or a mouse button… (Escape cancels)"))

        def done(proc, result):
            try:
                _ok, out, _err = proc.communicate_utf8_finish(result)
            except GLib.Error:
                out = ""
            words = (out or "").strip().split(" ", 1)
            if len(words) != 2 or words[0] != kind:
                self.show_binding(kind, name)
                return
            got = words[1]
            key = f"{kind}.{name}"
            self.ini, self.ini_lines = load_ini()
            # A key replaces the default binding; on the player's own one it is added (up to 4).
            current = [n.strip() for n in self.ini.get(key, "").split(",") if n.strip()] if self.ini_has(key) else []
            if kind == "pad":
                current = []  # a button replaces, as in the Windows launcher
            if got not in current:
                current.append(got)
            self.set_binding(kind, name, ",".join(current[-MAX_KEYS:]))
        process.communicate_utf8_async(None, None, done)

    # --- patches page ------------------------------------------------------------------------

    def build_patches_page(self):
        page = Adw.PreferencesPage()
        intro = Adw.PreferencesGroup(
            title=tr("Game patches"),
            description=tr("Applied when the game starts (or with the in-game menu's \"Apply and restart\"). Add your own: "
                           "copy patch .xml files in shadPS4 format (like PS-Game-Patch's or GoldHEN's) into the patches "
                           "folder and press Reload. Only those for CUSA13795 version 01.21 are listed."))
        actions = Adw.ActionRow(title=tr("Your patches folder"), subtitle=str(user_patches_dir()))
        add = Gtk.Button(label=tr("Add .xml…"), valign=Gtk.Align.CENTER)
        add.add_css_class("suggested-action")
        add.connect("clicked", self.on_add_patches)
        actions.add_suffix(add)
        actions.add_suffix(flat_button("folder-symbolic", tr("Open the patches folder"),
                                       lambda _b: open_folder(self, user_patches_dir())))
        actions.add_suffix(flat_button("view-refresh-symbolic", tr("Reload"), lambda _b: self.fill_patches()))
        intro.add(actions)
        unlock = Adw.ActionRow(title=tr("To have the game unlocked: turn on Unlock Everything (below) or import a 100% "
                                        "save in the Saves page."))
        unlock.add_css_class("dim-label")
        intro.add(unlock)
        page.add(intro)
        self.patches_page = page
        self.patches_group = None
        self.fill_patches()
        return page

    def fill_patches(self):
        if not hasattr(self, "patches_page"):
            return
        if self.patches_group is not None:
            self.patches_page.remove(self.patches_group)
        title = GAME_TITLE[0]
        self.patches_group = Adw.PreferencesGroup(
            title=tr("Patches found"),
            description=tr("Your game: {} edition ({}). Its patches are shown.").format(REGIONS.get(title, ""), title))
        try:
            # Mod packages have their own page (Mods), with their own switches.
            found = [p for p in patch_list(title, GAME_VERSION[0]) if not p[0].startswith(MOD_PREFIX)]
        except Exception as error:  # a broken file must not take the page down
            found = []
            self.patches_group.add(Adw.ActionRow(title=GLib.markup_escape_text(str(error))))
        if not found:
            self.patches_group.add(Adw.ActionRow(title=tr("No patches in the patches folder.")))
        enabled, disabled = read_patch_config()
        render_res_chosen = bool(combo_value(self.render_res_row)) if hasattr(self, "render_res_row") else False
        bundled = (PORT_DIR / "patches").resolve()
        for key, name, author, version, note, default_on, path, file_title in found:
            ini_key = INI_PATCHES.get(key)
            if ini_key:
                on = self.ini.get(ini_key) == "1"
            else:
                on = key in enabled or (default_on and key not in disabled)
            details = []
            if author:
                details.append(tr("by {}").format(author) + (f"  v{version}" if version else ""))
            if ini_key == "patch_unlock_all":
                details.append(tr("Unlocks what the game grants when its unlock conditions are met, with the same switch "
                                  "the trade-show demos (E3/PAX) used. The author advises backing up your save first "
                                  "(Saves page)."))
            if ini_key == "patch_pro_1080p":
                details.append(tr("Only with PS4 Pro mode, and only when the internal resolution is the game default "
                                  "(Settings > Internal resolution decides otherwise)."))
            if note:
                details.append(note)
            details.append(key)
            region = REGIONS.get(file_title, "")
            label = f"{name or key} [{region}]" if region else (name or key)
            row = Adw.SwitchRow(title=GLib.markup_escape_text(label),
                                subtitle=GLib.markup_escape_text("\n".join(details)), active=on)
            if ini_key == "patch_pro_1080p" and render_res_chosen:
                row.set_sensitive(False)
            row.connect("notify::active", lambda r, *_a, k=key, i=ini_key, d=default_on: self.on_patch_toggled(k, i, d, r.get_active()))
            if path.parent.resolve() != bundled:
                row.add_suffix(flat_button("user-trash-symbolic", tr("Remove"),
                                           lambda _b, p=path: self.confirm_remove_patch(p)))
            self.patches_group.add(row)
        self.patches_page.add(self.patches_group)

    def on_patch_toggled(self, key, ini_key, default_on, on):
        if ini_key:
            self.set_ini({ini_key: "1" if on else "0"})
            return
        enabled, disabled = read_patch_config()
        enabled = [k for k in enabled if k != key]
        disabled = [k for k in disabled if k != key]
        if on and not default_on:
            enabled.append(key)
        elif on:
            pass  # on by its file
        elif default_on:
            disabled.append(key)
        write_patch_config(enabled, disabled)

    def on_add_patches(self, _button):
        dialog = Gtk.FileDialog(title=tr("Patch files (.xml)"), modal=True)
        xml = Gtk.FileFilter(name=tr("shadPS4 patches (*.xml)"))
        xml.add_pattern("*.xml")
        xml.add_pattern("*.XML")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(xml)
        dialog.set_filters(filters)

        def done(dialog, result):
            try:
                files = dialog.open_multiple_finish(result)
            except GLib.Error:
                return
            import patches as patch_files
            folder = user_patches_dir()
            added, problems = 0, []
            for i in range(files.get_n_items()):
                source = Path(files.get_item(i).get_path())
                try:
                    folder.mkdir(parents=True, exist_ok=True)
                    target = folder / (source.stem + ".xml")
                    target.write_bytes(source.read_bytes())
                    if not any(p.name == target.name for _k, p, _m in patch_files.external_patches(folder)):
                        target.unlink()
                        problems.append(tr("{}: no patch for CUSA13795 01.21").format(source.name))
                        continue
                    added += 1
                except OSError as error:
                    problems.append(f"{source.name}: {error}")
            self.toasts.add_toast(Adw.Toast(title=tr("Patch files added: {}").format(added) +
                                            (". " + "; ".join(problems) if problems else ""), timeout=8))
            self.fill_patches()
        dialog.open_multiple(self, None, done)

    def confirm_remove_patch(self, path):
        def remove():
            try:
                path.unlink()
            except OSError as error:
                self.toasts.add_toast(Adw.Toast(title=str(error)))
            self.fill_patches()
        self.confirm(tr("Remove {} from your patches folder?").format(path.name),
                     tr("Every patch in that file goes away."), tr("Remove"), remove)

    # --- mods page ---------------------------------------------------------------------------

    def build_mods_page(self):
        page = Adw.PreferencesPage()
        intro = Adw.PreferencesGroup(
            title=tr("Mods"),
            description=tr("Add .pak, .zip or .xml files, or a folder: they are copied into the mods folder and checked. "
                           "Turn on the ones you want; they apply the next time you press Play, in a separate mod "
                           "profile with its own saves (your normal saves are not touched)."))
        intro.set_header_suffix(self.mods_header_buttons())
        add_files = Adw.ActionRow(title=tr("Add mod files…"), subtitle=str(mods_dir()))
        add_button = Gtk.Button(label=tr("Add files"), valign=Gtk.Align.CENTER)
        add_button.add_css_class("suggested-action")
        add_button.connect("clicked", lambda _b: self.on_add_mods(folder=False))
        add_files.add_suffix(add_button)
        folder_button = Gtk.Button(label=tr("Add folder"), valign=Gtk.Align.CENTER)
        folder_button.connect("clicked", lambda _b: self.on_add_mods(folder=True))
        add_files.add_suffix(folder_button)
        intro.add(add_files)
        self.mods_controls = [add_button, folder_button]
        for title, url in ((tr("Find mods on GameBanana"), GAMEBANANA), (tr("Read the packaging guide"), None)):
            row = Adw.ActionRow(title=title, activatable=True)
            row.add_suffix(Gtk.Image(icon_name="web-browser-symbolic" if url else "help-about-symbolic"))
            row.connect("activated", lambda _r, url=url: self.open_mod_link(url))
            intro.add(row)
        page.add(intro)
        self.mods_page = page
        self.mods_groups = []
        self.mods_scan = None    # the last scan's result
        self.mods_busy = False
        self.fill_mods()
        self.rescan_mods()
        return page

    def mods_header_buttons(self):
        box = Gtk.Box(spacing=4)
        self.mods_reload_button = flat_button("view-refresh-symbolic", tr("Reload mods"), lambda _b: self.rescan_mods())
        box.append(self.mods_reload_button)
        box.append(flat_button("folder-symbolic", tr("Open the mods folder"), lambda _b: open_folder(self, mods_dir())))
        return box

    def open_mod_link(self, url):
        if url is None:  # the guide: the package's copy, else the repository's
            local = PORT_DIR / "docs" / "mod-packaging.html"
            url = local if local.is_file() else MOD_GUIDE_URL
        open_external(url)

    def rescan_mods(self):
        """Checks the mods folder in the background (large .zip files take a moment)."""
        if self.mods_busy or self.process is not None:
            self.fill_mods()
            return
        self.mods_busy = True
        self.fill_mods()

        def work():
            try:
                return scan_mods()
            except (ValueError, OSError) as error:
                return {"entries": {}, "messages": [str(error)]}

        def done(result):
            self.mods_busy = False
            self.mods_scan = result
            self.fill_mods()
            self.fill_patches()  # mods/*.xml are listed with the patches
        self.in_thread(work, done)

    def fill_mods(self):
        for group in self.mods_groups:
            self.mods_page.remove(group)
        self.mods_groups = []
        locked = self.process is not None
        for widget in self.mods_controls + [self.mods_reload_button]:
            widget.set_sensitive(not locked and not self.mods_busy)
        title = GAME_TITLE[0]
        group = Adw.PreferencesGroup(
            title=tr("Your mods"),
            description=tr("Close the game to change mods.") if locked else
            tr("Your game: {} edition ({}).").format(REGIONS.get(title, ""), title))
        if self.mods_busy:
            row = Adw.ActionRow(title=tr("Checking the mods folder…"))
            row.add_prefix(Gtk.Spinner(spinning=True))
            group.add(row)
        try:
            found = mod_entries(title, GAME_VERSION[0])
        except Exception as error:  # a broken library file must not take the page down
            found = []
            group.add(Adw.ActionRow(title=GLib.markup_escape_text(str(error))))
        if not found and not self.mods_busy:
            group.add(Adw.ActionRow(title=tr("No mods yet. Add some with the buttons above.")))
        enabled, disabled = read_patch_config()
        for key, meta in found:
            group.add(self.mod_row(key, meta, key in enabled and key not in disabled, locked))
        self.mods_groups.append(group)
        messages = (self.mods_scan or {}).get("messages", [])
        if messages:
            problems = Adw.PreferencesGroup(title=tr("Mods that cannot be used"),
                                            description=tr("Fix or remove them and press Reload."))
            for message in messages:
                row = Adw.ActionRow(title=GLib.markup_escape_text(message))
                row.set_title_lines(0)
                row.add_prefix(Gtk.Image(icon_name="dialog-warning-symbolic"))
                problems.add(row)
            self.mods_groups.append(problems)
        for g in self.mods_groups:
            self.mods_page.add(g)

    def mod_row(self, key, meta, on, locked):
        ident = key[len(MOD_PREFIX):]
        name = meta.get("ModName") or ident
        details = [meta.get("Author", "")] + ([f"v{meta.get('PatchVer')}"] if meta.get("PatchVer") else [])
        row = Adw.ExpanderRow(title=GLib.markup_escape_text(name),
                              subtitle=GLib.markup_escape_text("  ·  ".join(d for d in details if d)))
        switch = Gtk.Switch(active=on, valign=Gtk.Align.CENTER, sensitive=not locked)
        switch.set_tooltip_text(tr("Disable mod") if on else tr("Enable mod"))
        switch.connect("notify::active", lambda s, *_a: self.on_mod_toggled(key, s))
        (row.add_suffix if hasattr(row, "add_suffix") else row.add_action)(switch)
        description = Adw.ActionRow(title=GLib.markup_escape_text(meta.get("ModDescription") or
                                                                  tr("The creator has not provided a description.")))
        description.set_title_lines(0)
        row.add_row(description)
        source = Adw.ActionRow(title=tr("File"), subtitle=GLib.markup_escape_text("mods/" + meta.get("ModSource", "")))
        if meta.get("ModVersions"):
            source.set_subtitle(GLib.markup_escape_text(f"mods/{meta.get('ModSource', '')}\n{meta.get('ModVersions')}"))
        remove = Gtk.Button(label=tr("Remove"), valign=Gtk.Align.CENTER, sensitive=not locked)
        remove.add_css_class("destructive-action")
        remove.set_tooltip_text(tr("Remove: keeps a recoverable copy and saves."))
        remove.connect("clicked", lambda _b: self.confirm_remove_mod(ident, name, meta.get("ModHash", "")))
        source.add_suffix(remove)
        row.add_row(source)
        return row

    def on_mod_toggled(self, key, switch):
        on = switch.get_active()
        switch.set_tooltip_text(tr("Disable mod") if on else tr("Enable mod"))
        self.on_patch_toggled(key, None, False, on)

    def on_add_mods(self, folder):
        dialog = Gtk.FileDialog(title=tr("Add mod folder") if folder else tr("Add mods or patches"), modal=True)
        if not folder:
            files = Gtk.FileFilter(name=tr("Mods (*.pak, *.zip, *.xml)"))
            for extension in MOD_EXTENSIONS:
                files.add_pattern("*" + extension)
                files.add_pattern("*" + extension.upper())
            filters = Gio.ListStore.new(Gtk.FileFilter)
            filters.append(files)
            dialog.set_filters(filters)

        def done(dialog, result):
            try:
                if folder:
                    chosen = [dialog.select_folder_finish(result)]
                else:
                    picked = dialog.open_multiple_finish(result)
                    chosen = [picked.get_item(i) for i in range(picked.get_n_items())]
            except GLib.Error:
                return
            added, problems = import_mods([c.get_path() for c in chosen if c and c.get_path()])
            self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(
                tr("Mods added: {}. Turn them on to play with them.").format(added) +
                (" " + "; ".join(problems) if problems else "")), timeout=8))
            if added:
                self.rescan_mods()
        if folder:
            dialog.select_folder(self, None, done)
        else:
            dialog.open_multiple(self, None, done)

    def confirm_remove_mod(self, ident, name, digest):
        def remove():
            def work():
                try:
                    return remove_mod(ident, digest), None
                except (ValueError, OSError) as error:
                    return None, str(error)

            def done(result):
                moved, error = result
                self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(
                    tr("Mod removed. The mod and saves have been kept.") if moved
                    else tr("Could not remove the mod: {}").format(error)), timeout=8))
                self.rescan_mods()
            self.in_thread(work, done)
        self.confirm(tr("Remove {}?").format(name),
                     tr("It is moved to mod-state/removed in the data folder, where it can be recovered; its saves are kept."),
                     tr("Remove"), remove)

    # --- saves page --------------------------------------------------------------------------

    def build_saves_page(self):
        page = Adw.PreferencesPage()
        self.saves_page = page
        self.saves_groups = []
        self.fill_saves()
        return page

    def fill_saves(self):
        for group in self.saves_groups:
            self.saves_page.remove(group)
        self.saves_groups = []
        locked = self.process is not None
        user = self.user_dir()
        saves = Adw.PreferencesGroup(
            title=tr("Saves"),
            description=(tr("Close the game to back up, import or restore saves.") + "\n" if locked else "") +
            str(save_root(user)))
        slots = list_saves(user)
        if not slots:
            saves.add(Adw.ActionRow(title=tr("No saves yet.")))
        for slot, text, size, modified in slots:
            row = Adw.ActionRow(title=GLib.markup_escape_text(text),
                                subtitle=GLib.markup_escape_text(f"{slot}  ·  {format_size(size)}  ·  {modified}"))
            saves.add(row)
        actions = Adw.ActionRow(title=tr("Back up, export or import"),
                                subtitle=tr("Import takes a backup made by this launcher, the CUSA13795 saves folder from "
                                            "another PC, or a decrypted PS4 save (Apollo Save Tool, Save Wizard: a folder "
                                            "with sce_sys/param.sfo). Your saves are backed up before importing."))
        backup = Gtk.Button(label=tr("Back up now"), valign=Gtk.Align.CENTER)
        backup.set_sensitive(bool(slots) and not locked)
        backup.connect("clicked", lambda _b: self.on_backup_saves())
        actions.add_suffix(backup)
        export = Gtk.Button(label=tr("Export…"), valign=Gtk.Align.CENTER)
        export.set_sensitive(bool(slots) and not locked)
        export.connect("clicked", lambda _b: self.on_export_saves())
        actions.add_suffix(export)
        imported = Gtk.Button(label=tr("Import…"), valign=Gtk.Align.CENTER)
        imported.set_sensitive(not locked)
        imported.connect("clicked", lambda _b: self.on_import_saves())
        actions.add_suffix(imported)
        actions.add_suffix(flat_button("folder-symbolic", tr("Open folder"), lambda _b: open_folder(self, save_root(user))))
        saves.add(actions)

        backups = Adw.PreferencesGroup(title=tr("Backups"), description=str(backup_root()))
        found = list_backups()
        if not found:
            backups.add(Adw.ActionRow(title=tr("No backups yet.")))
        for path, size in found:
            row = Adw.ActionRow(title=GLib.markup_escape_text(path.name), subtitle=format_size(size))
            restore = Gtk.Button(label=tr("Restore"), valign=Gtk.Align.CENTER)
            restore.set_sensitive(not locked)
            restore.connect("clicked", lambda _b, p=path: self.confirm_restore(p))
            row.add_suffix(restore)
            row.add_suffix(flat_button("user-trash-symbolic", tr("Delete"), lambda _b, p=path: self.confirm_delete_backup(p)))
            backups.add(row)
        folder = Adw.ActionRow(title=tr("Open backups folder"))
        folder.add_suffix(flat_button("folder-symbolic", tr("Open backups folder"), lambda _b: open_folder(self, backup_root())))
        backups.add(folder)
        refresh = Adw.ActionRow(title=tr("Refresh"))
        refresh.add_suffix(flat_button("view-refresh-symbolic", tr("Refresh"), lambda _b: self.fill_saves()))
        backups.add(refresh)
        for group in (saves, backups):
            self.saves_page.add(group)
            self.saves_groups.append(group)

    def saves_notice(self, text):
        self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=8))
        self.fill_saves()

    def on_backup_saves(self):
        try:
            made = backup_saves(self.user_dir())
        except OSError as error:
            self.saves_notice(str(error))
            return
        self.saves_notice(tr("Backup created: {}").format(made.name) if made else tr("No saves yet."))

    def on_export_saves(self):
        dialog = Gtk.FileDialog(title=tr("Export saves"), modal=True)

        def done(dialog, result):
            try:
                chosen = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            import shutil
            target = Path(chosen.get_path()) / ("CTR-saves-" + time.strftime("%Y%m%d-%H%M%S"))
            try:
                shutil.copytree(save_root(self.user_dir()), target)
            except OSError as error:
                self.saves_notice(str(error))
                return
            self.saves_notice(tr("Saves exported to {}").format(target))
        dialog.select_folder(self, None, done)

    def on_import_saves(self):
        dialog = Gtk.FileDialog(title=tr("Import save"), modal=True)

        def done(dialog, result):
            try:
                chosen = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            try:
                error = import_saves(self.user_dir(), chosen.get_path())
            except OSError as failure:
                error = str(failure)
            self.saves_notice(error or tr("Save imported. The previous one was backed up."))
        dialog.select_folder(self, None, done)

    def confirm_restore(self, path):
        def restore():
            try:
                restore_backup(self.user_dir(), path)
            except OSError as error:
                self.saves_notice(str(error))
                return
            self.saves_notice(tr("Backup restored. Your previous saves were backed up first."))
        self.confirm(tr("Restore {}?").format(path.name),
                     tr("Your current saves are replaced by this backup (they are backed up first)."), tr("Restore"), restore)

    def confirm_delete_backup(self, path):
        def delete():
            import shutil
            try:
                shutil.rmtree(path)
            except OSError as error:
                self.saves_notice(str(error))
                return
            self.fill_saves()
        self.confirm(tr("Delete {}?").format(path.name), tr("This backup is deleted for good."), tr("Delete"), delete)

    # --- DLC page ----------------------------------------------------------------------------

    def dlc_dir(self):
        return Path(os.environ.get("BB_DLC_DIR") or DATA_DIR / "dlc")

    def build_dlc_page(self):
        page = Adw.PreferencesPage()
        intro = Adw.PreferencesGroup(
            title=tr("DLC"),
            description=tr("Optional: the game runs the same without DLC. CTR Nitro-Fueled's DLC only carry the license "
                           "(the content is already in the game); the port reports them installed at start. Use your own "
                           "PS4 DLC packages (.pkg) for CUSA13795: they are copied into the dlc folder, your files are not "
                           "changed. Changes apply the next time you press Play."))
        self.dlc_row = Adw.SwitchRow(title=tr("Enable DLC"), active=self.settings.get("dlc", True))
        self.dlc_row.connect("notify::active", lambda *_: self.on_dlc_enabled())
        intro.add(self.dlc_row)
        actions = Adw.ActionRow(title=tr("Add DLC (.pkg)…"), subtitle=str(self.dlc_dir()))
        add = Gtk.Button(label=tr("Add"), valign=Gtk.Align.CENTER)
        add.add_css_class("suggested-action")
        add.connect("clicked", self.on_add_dlc)
        actions.add_suffix(add)
        actions.add_suffix(flat_button("folder-symbolic", tr("Open the dlc folder"),
                                       lambda _b: open_folder(self, self.dlc_dir())))
        actions.add_suffix(flat_button("view-refresh-symbolic", tr("Refresh"), lambda _b: self.fill_dlc()))
        intro.add(actions)
        page.add(intro)
        self.dlc_page = page
        self.dlc_group = None
        self.fill_dlc()
        return page

    def fill_dlc(self):
        if self.dlc_group is not None:
            self.dlc_page.remove(self.dlc_group)
        self.dlc_group = Adw.PreferencesGroup(title=tr("DLC found"))
        disabled = {x.strip() for x in self.settings.get("dlc_disabled", "").split(",") if x.strip()}
        items = []
        if self.dlc_dir().is_dir():
            for path in sorted(self.dlc_dir().iterdir()):
                if content_profile.is_candidate(path):
                    items.append((path, *content_profile.describe_addon(path, GAME_TITLE[0])))
        if not items:
            self.dlc_group.add(Adw.ActionRow(title=tr("No DLC in the dlc folder")))
        for path, label, title, problem in sorted(items, key=lambda i: i[2].lower()):
            if problem:
                row = Adw.ActionRow(title=GLib.markup_escape_text(path.name), subtitle=GLib.markup_escape_text(problem))
                row.add_prefix(Gtk.Image(icon_name="dialog-warning-symbolic"))
            else:
                row = Adw.SwitchRow(title=GLib.markup_escape_text(title), subtitle=label, active=label not in disabled)
                row.connect("notify::active", lambda r, *_a, lb=label: self.on_dlc_toggled(lb, r.get_active()))
            row.add_suffix(flat_button("user-trash-symbolic", tr("Remove"), lambda _b, p=path: self.confirm_remove_dlc(p)))
            self.dlc_group.add(row)
        self.dlc_group.set_sensitive(self.settings.get("dlc", True))
        self.dlc_page.add(self.dlc_group)

    def on_dlc_enabled(self):
        self.settings["dlc"] = self.dlc_row.get_active()
        save_settings(self.settings)
        if self.dlc_group is not None:
            self.dlc_group.set_sensitive(self.settings["dlc"])

    def on_dlc_toggled(self, label, on):
        disabled = [x.strip() for x in self.settings.get("dlc_disabled", "").split(",") if x.strip() and x.strip() != label]
        if not on:
            disabled.append(label)
        self.settings["dlc_disabled"] = ",".join(disabled)
        save_settings(self.settings)

    def on_add_dlc(self, _button):
        dialog = Gtk.FileDialog(title=tr("PS4 DLC packages (.pkg)"), modal=True)
        pkg = Gtk.FileFilter(name=tr("PS4 DLC (*.pkg)"))
        pkg.add_pattern("*.pkg")
        pkg.add_pattern("*.PKG")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(pkg)
        dialog.set_filters(filters)

        def done(dialog, result):
            try:
                files = dialog.open_multiple_finish(result)
            except GLib.Error:
                return
            added, problems = 0, []
            self.dlc_dir().mkdir(parents=True, exist_ok=True)
            for i in range(files.get_n_items()):
                source = Path(files.get_item(i).get_path())
                _label, _title, problem = content_profile.describe_addon(source, GAME_TITLE[0])
                if problem:
                    problems.append(f"{source.name}: {problem}")
                    continue
                try:
                    (self.dlc_dir() / source.name).write_bytes(source.read_bytes())
                    added += 1
                except OSError as error:
                    problems.append(f"{source.name}: {error}")
            self.toasts.add_toast(Adw.Toast(title=tr("DLC added: {}").format(added) +
                                            (". " + "; ".join(problems) if problems else ""), timeout=8))
            self.fill_dlc()
        dialog.open_multiple(self, None, done)

    # --- achievements page ---------------------------------------------------------------------
    # The game's trophies (launcher/trophies.py), as the Windows launcher's Achievements tab. They
    # are recorded while you play either way; the list is shown once the player's PS4 trophy key
    # (the one shadPS4 uses) is given or imported, as it decrypts their names.

    GRADE_COLORS = {"P": "#5ec8ff", "G": "#e6b422", "S": "#9aa6b4", "B": "#c97a3f"}

    @staticmethod
    def grade_mark(grade):
        """The grade's dot and name, as Pango markup."""
        name = {"P": tr("Platinum"), "G": tr("Gold"), "S": tr("Silver"), "B": tr("Bronze")}[grade]
        return f"<span foreground='{LauncherWindow.GRADE_COLORS[grade]}'>●</span> {GLib.markup_escape_text(name)}"

    def build_trophies_page(self):
        page = Adw.PreferencesPage()
        self.trophies_page = page
        self.trophies_groups = []
        self.trophy_set = None
        self.trophy_source = None
        self.trophy_filter = "all"
        self.trophy_stamp = None
        return page

    def current_trophy_source(self):
        return (str(self.game_dir()) if self.settings["game_dir"] else "", str(self.user_dir()), GAME_TITLE[0],
                combo_value(self.language_row), self.settings.get("trophy_key", ""))

    def unlocked_stamp(self):
        try:
            return (self.trophy_set.folder / "unlocked.txt").stat().st_mtime_ns
        except (OSError, AttributeError):
            return None

    def poll_trophies(self):
        """While the page is shown: the game writes unlocked.txt as trophies unlock."""
        if self.stack.get_visible_child_name() != "trophies" or self.trophy_set is None:
            return
        if self.current_trophy_source() != self.trophy_source or self.unlocked_stamp() != self.trophy_stamp:
            self.fill_trophies()

    def fill_trophies(self, reload=False):
        source = self.current_trophy_source()
        if reload or self.trophy_set is None or source != self.trophy_source:
            self.trophy_source = source
            self.trophy_set = trophies.load(*source)
        self.trophy_stamp = self.unlocked_stamp()
        self.trophy_set.refresh_unlocked()
        for group in self.trophies_groups:
            self.trophies_page.remove(group)
        self.trophies_groups = []
        t = self.trophy_set
        if t.problem:
            group = Adw.PreferencesGroup(title=tr("Achievements"))
            row = Adw.ActionRow(title=tr("Choose the game folder in Settings first.") if t.problem == "no_game"
                                else tr("This game has no trophy file (sce_sys/trophy/trophy00.trp)."))
            row.add_prefix(Gtk.Image(icon_name="dialog-information-symbolic"))
            group.add(row)
            self.trophies_groups.append(group)
        elif not t.names:
            self.trophies_groups.append(self.trophy_key_group(t))
        else:
            self.trophies_groups += [self.trophy_summary_group(t), self.trophy_list_group(t)]
        for group in self.trophies_groups:
            self.trophies_page.add(group)

    def trophy_key_group(self, t):
        group = Adw.PreferencesGroup(
            title=tr("Trophy key"),
            description=tr("Trophies are recorded while you play. Their names and descriptions are encrypted in the game "
                           "with the PS4's trophy key, which is not included: if you have it (the same one shadPS4 uses), "
                           "paste or import it to see your achievements here."))
        if t.key_problem:
            row = Adw.ActionRow(title=tr("The trophy key does not decrypt this game: check it is your PS4's (32 hex digits).")
                                if t.key_problem == "bad_key" else
                                tr("The game folder has no NP communication ID (sce_sys/npbind.dat), needed to decrypt."))
            row.set_title_lines(0)
            row.add_css_class("error")
            row.add_prefix(Gtk.Image(icon_name="dialog-warning-symbolic"))
            group.add(row)
        entry = Adw.PasswordEntryRow(title=tr("Trophy key (32 hex digits)"))
        save = Gtk.Button(label=tr("Save"), valign=Gtk.Align.CENTER)
        save.add_css_class("suggested-action")
        save.connect("clicked", lambda _b: self.set_trophy_key(entry.get_text(), entry))
        entry.connect("entry-activated", lambda _e: self.set_trophy_key(entry.get_text(), entry))
        entry.add_suffix(save)
        group.add(entry)
        shadps4 = Adw.ActionRow(title=tr("Import from shadPS4"),
                                subtitle=tr("Reads the key shadPS4 keeps (keys.json in ~/.local/share/shadPS4 or its Flatpak)."))
        import_button = Gtk.Button(label=tr("Import"), valign=Gtk.Align.CENTER)
        import_button.connect("clicked", lambda _b: self.import_shadps4_key())
        shadps4.add_suffix(import_button)
        shadps4.add_suffix(flat_button("document-open-symbolic", tr("Choose shadPS4's keys.json or config.toml…"),
                                       lambda _b: self.choose_key_file()))
        group.add(shadps4)
        return group

    def set_trophy_key(self, text, entry=None):
        if trophies.key_bytes(text) is None:
            self.toasts.add_toast(Adw.Toast(title=tr("The key must be 32 hex digits.")))
            return
        if entry is not None:
            entry.set_text("")
        self.settings["trophy_key"] = text.strip()
        save_settings(self.settings)
        self.fill_trophies(reload=True)
        if self.trophy_set.names:
            self.toasts.add_toast(Adw.Toast(title=tr("Trophy key saved: your achievements are shown.")))

    def import_shadps4_key(self):
        key, _source = trophies.shadps4_key()
        if not key:
            self.toasts.add_toast(Adw.Toast(title=tr("shadPS4 has no trophy key saved (keys.json). Choose its file by hand."),
                                            timeout=8))
            return
        self.set_trophy_key(key)

    def choose_key_file(self):
        dialog = Gtk.FileDialog(title=tr("shadPS4's keys.json or config.toml"), modal=True)
        folders = [d for d in trophies.shadps4_dirs() if d.is_dir()]
        if folders:
            dialog.set_initial_folder(Gio.File.new_for_path(str(folders[0])))

        def done(dialog, result):
            try:
                chosen = dialog.open_finish(result)
            except GLib.Error:
                return
            key = trophies.key_from_file(chosen.get_path()) if chosen else ""
            if key:
                self.set_trophy_key(key)
            else:
                self.toasts.add_toast(Adw.Toast(title=tr("That file has no trophy key.")))
        dialog.open(self, None, done)

    def trophy_summary_group(self, t):
        unlocked = sum(1 for x in t.trophies if x["unlocked"])
        total = len(t.trophies)
        points = sum(trophies.POINTS.get(x["grade"], 0) for x in t.trophies)
        earned = sum(trophies.POINTS.get(x["grade"], 0) for x in t.trophies if x["unlocked"])
        progress = earned / points if points else (unlocked / total if total else 0.0)
        group = Adw.PreferencesGroup()
        card = Gtk.Box(spacing=16)
        card.add_css_class("card")
        card.add_css_class("sf-trophy-card")
        texture = png_texture(t.icon)
        if texture is not None:
            banner = Gtk.Picture(paintable=texture, content_fit=Gtk.ContentFit.COVER, can_shrink=True,
                                 width_request=144, height_request=80, valign=Gtk.Align.CENTER)
            banner.add_css_class("sf-trophy-banner")
            banner.set_overflow(Gtk.Overflow.HIDDEN)
            card.append(banner)
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, hexpand=True, valign=Gtk.Align.CENTER)
        name = Gtk.Label(label=t.title_name or "Crash Team Racing Nitro-Fueled", xalign=0, wrap=True)
        name.add_css_class("sf-big")
        text.append(name)
        text.append(Gtk.Label(label=tr("{} of {} trophies  ·  {} %").format(unlocked, total, round(progress * 100)), xalign=0))
        text.append(Gtk.ProgressBar(fraction=progress))
        grades = []
        for grade in "PGSB":
            have = [x for x in t.trophies if x["grade"] == grade]
            if have:
                grades.append(f"{self.grade_mark(grade)} {sum(1 for x in have if x['unlocked'])}/{len(have)}")
        if grades:
            text.append(Gtk.Label(label="    ".join(grades), use_markup=True, xalign=0, wrap=True))
        card.append(text)
        group.add(card)
        return group

    def trophy_list_group(self, t):
        group = Adw.PreferencesGroup(
            title=tr("Trophies"),
            description=tr("They unlock as you play, as on a PS4; the platinum comes once all the others are."))
        bar = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        filters = Gtk.Box()
        filters.add_css_class("linked")
        first = None
        for value, label in (("all", tr("All")), ("unlocked", tr("Unlocked")), ("locked", tr("Locked"))):
            button = Gtk.ToggleButton(label=label, active=self.trophy_filter == value)
            if first is None:
                first = button
            else:
                button.set_group(first)
            button.connect("toggled", lambda b, v=value: b.get_active() and self.set_trophy_filter(v))
            filters.append(button)
        bar.append(filters)
        bar.append(flat_button("folder-symbolic", tr("Open the trophies folder"), lambda _b: open_folder(self, t.folder)))
        reset = flat_button("edit-undo-symbolic", tr("Lock every trophy again"), lambda _b: self.confirm_reset_trophies())
        reset.set_sensitive(self.process is None and any(x["unlocked"] for x in t.trophies))
        bar.append(reset)
        group.set_header_suffix(bar)
        # The latest unlocked first, then the rest in the game's order.
        order = sorted(t.trophies, key=lambda x: (not x["unlocked"], -x["unlocked"], x["id"]))
        shown = [x for x in order if self.trophy_filter == "all" or bool(x["unlocked"]) == (self.trophy_filter == "unlocked")]
        if not shown:
            group.add(Adw.ActionRow(title=tr("Nothing here yet.")))
        for x in shown:
            group.add(self.trophy_row(x))
        return group

    def trophy_row(self, x):
        secret = x["hidden"] and not x["unlocked"]
        name = tr("Hidden trophy") if secret else x["name"] or tr("Trophy #{}").format(x["id"])
        detail = tr("Keep playing to find it.") if secret else x["detail"]
        row = Adw.ActionRow(title=GLib.markup_escape_text(name), subtitle=GLib.markup_escape_text(detail))
        row.set_subtitle_lines(0)
        texture = None if secret else png_texture(x["icon"])
        icon = Gtk.Image(pixel_size=56, valign=Gtk.Align.CENTER, margin_top=6, margin_bottom=6)
        if texture is not None:
            icon.set_from_paintable(texture)
        else:
            icon.set_from_icon_name("dialog-question-symbolic")
        icon.add_css_class("sf-trophy-icon")
        if not x["unlocked"]:
            icon.add_css_class("sf-trophy-locked")
        row.add_prefix(icon)
        state = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER, spacing=2)
        if x["grade"] in self.GRADE_COLORS:
            state.append(Gtk.Label(label=self.grade_mark(x["grade"]), use_markup=True, xalign=1))
        if x["unlocked"] > 1:
            when = Gtk.Label(label="✓ " + time.strftime("%d/%m/%Y %H:%M", time.localtime(x["unlocked"])), xalign=1)
        else:
            when = Gtk.Label(label="✓ " + tr("Unlocked") if x["unlocked"] else tr("Locked"), xalign=1)
        when.add_css_class("success" if x["unlocked"] else "dim-label")
        when.add_css_class("caption")
        state.append(when)
        row.add_suffix(state)
        return row

    def set_trophy_filter(self, value):
        if value != self.trophy_filter:
            self.trophy_filter = value
            GLib.idle_add(lambda: self.fill_trophies() and False)  # not while the toggle is being changed

    def confirm_reset_trophies(self):
        def reset():
            try:
                trophies.reset_unlocked(self.trophy_set.folder / "unlocked.txt")
            except OSError as error:
                self.toasts.add_toast(Adw.Toast(title=tr("Could not reset: {}").format(error)))
            self.fill_trophies()
        self.confirm(tr("Lock every trophy again?"), tr("A copy is kept just in case."), tr("Reset"), reset)

    def inform(self, heading, body):
        """A message with a single OK."""
        if hasattr(Adw, "AlertDialog"):
            dialog = Adw.AlertDialog(heading=heading, body=body)
            dialog.add_response("ok", "OK")
            dialog.present(self)
        else:
            dialog = Adw.MessageDialog(transient_for=self, heading=heading, body=body)
            dialog.add_response("ok", "OK")
            dialog.present()

    def confirm(self, heading, body, action, on_yes):
        """A yes/no question; on_yes runs on yes."""
        if hasattr(Adw, "AlertDialog"):
            dialog = Adw.AlertDialog(heading=heading, body=body)
            dialog.add_response("cancel", tr("Cancel"))
            dialog.add_response("yes", action)
            dialog.set_response_appearance("yes", Adw.ResponseAppearance.DESTRUCTIVE)
            dialog.connect("response", lambda _d, r: on_yes() if r == "yes" else None)
            dialog.present(self)
        else:
            dialog = Adw.MessageDialog(transient_for=self, heading=heading, body=body)
            dialog.add_response("cancel", tr("Cancel"))
            dialog.add_response("yes", action)
            dialog.set_response_appearance("yes", Adw.ResponseAppearance.DESTRUCTIVE)
            dialog.connect("response", lambda _d, r: on_yes() if r == "yes" else None)
            dialog.present()

    def confirm_remove_dlc(self, path):
        def remove():
            try:
                if path.is_dir():
                    import shutil
                    shutil.rmtree(path)
                else:
                    path.unlink()
            except OSError as error:
                self.toasts.add_toast(Adw.Toast(title=str(error)))
            self.fill_dlc()
        self.confirm(tr("Remove {} from the dlc folder?").format(path.name),
                     tr("Only the port's copy is deleted."), tr("Remove"), remove)

    # --- online: nickname, Discord account, presence, invites, announcements ----------------

    def server(self):
        return self.server_row.get_text().strip() or ONLINE_SERVER

    def nick(self):
        return self.nick_row.get_text().strip()

    def in_thread(self, work, done):
        """work() on a worker thread, then done(result) on the window's."""
        import threading

        def run():
            result = work()
            GLib.idle_add(lambda: done(result) and False)
        threading.Thread(target=run, daemon=True).start()

    def nick_changed(self):
        """A nickname or server typed: asked again once typing stops."""
        if self.nick_timer:
            GLib.source_remove(self.nick_timer)

        def ask():
            self.nick_timer = 0
            self.ask_nick_status()
            return False
        self.nick_timer = GLib.timeout_add(700, ask)

    def ask_nick_status(self):
        server, nick = self.server(), self.nick()
        asked = f"{server}|{nick}"
        self.nick_asked = asked
        self.nick_info = None
        self.show_nick_status()

        def done(info):
            if self.nick_asked == asked:
                self.nick_info = info or {}
                self.show_nick_status()
        self.in_thread(lambda: ryunet_http.nick_status(DATA_DIR, server, nick if nick_ok(nick) else ""), done)

    def show_nick_status(self):
        nick, info = self.nick(), self.nick_info
        link_available = bool(info and info.get("discord_link"))
        status = (info or {}).get("status", "")
        linked = bool(info and info.get("linked"))
        ban = (info or {}).get("banned")
        if ban:
            until = (info or {}).get("ban_until") or 0
            text = " ".join((
                tr("Your connection (IP) is banned from online play.") if (info or {}).get("ban_kind") == "ip"
                else tr("Your account is banned from online play."),
                tr("Reason: {}").format(ban),
                tr("Until: {}").format(time.strftime("%Y-%m-%d %H:%M", time.localtime(until))) if until
                else tr("Duration: permanent"),
                tr("If you think it is a mistake, appeal on our Discord.")))
        elif self.discord_busy:
            text = tr("Authorize your account in the Discord tab that opened in the browser…")
        elif not nick_ok(nick):
            text = tr("Choose a nickname: 3 to 16 letters, digits, _ or - (not RyuPC or Player)")
        elif info is None:
            text = tr("Checking the nickname with the server…")
        elif status == "free":
            text = tr("Available: it becomes yours the first time you play online.")
        elif status == "yours" and info.get("key_switched"):
            text = tr("This nickname is yours (recovered from another installation on this PC).")
        elif status == "yours" and linked:
            text = tr("This nickname is yours. Discord account: {}").format(info.get("discord") or "?")
        elif status == "yours":
            text = tr("This nickname is yours.")
        elif status == "taken" and linked:
            text = tr("That nickname is linked to a Discord account. If it is yours, press Recover with Discord; "
                      "otherwise choose another one.")
        elif status == "taken":
            text = tr("Someone else already uses that nickname: choose another one.")
        elif status in ("reserved", "invalid"):
            text = tr("The server does not accept that nickname: choose another one.")
        else:
            text = tr("The server could not be asked (it checks again when you play).")
        clock_off = abs(ryunet_http.clock_offset())
        if clock_off > 240:
            text += " " + tr("This PC's clock is {} min off: online corrects it, but set the date and time "
                             "to automatic in the system settings.").format(round(clock_off / 60))
        self.nick_status_row.set_subtitle(GLib.markup_escape_text(text))
        idle = not self.discord_busy
        self.discord_link_button.set_visible(link_available and idle and nick_ok(nick) and status in ("free", "yours")
                                             and not linked)
        self.discord_recover_button.set_visible(link_available and idle and (
            not nick_ok(nick) or status == "free" or (status == "taken" and linked)))
        self.discord_unlink_button.set_visible(link_available and idle and status == "yours" and linked)

    def discord_app_id(self):
        return self.settings.get("discord_app_id") or DISCORD_APP_ID

    def start_discord_link(self, mode):
        """mode: "link" this nickname, or "recover" the one linked to the Discord account."""
        if self.discord_busy:
            return
        self.store()
        server, app_id = self.server(), self.discord_app_id()
        nick = self.nick() if mode == "link" else ""
        self.discord_busy = True
        self.show_nick_status()

        def open_url(url):
            def show():
                opened = open_external(url)
                # Whether or not a browser came up: the link can also be pasted by hand.
                toast = Adw.Toast(title=tr("Discord's authorization page opened in your browser") if opened
                                  else tr("Could not open your browser: copy the link and open it there"),
                                  button_label=tr("Copy link"), timeout=0 if not opened else 30)
                toast.connect("button-clicked", lambda _t: self.get_clipboard().set(url))
                self.toasts.add_toast(toast)
                return False
            GLib.idle_add(show)

        def work():
            auth = discord_oauth.authorize(app_id, open_url)
            if not auth.get("ok"):
                return auth, None
            return auth, ryunet_http.discord_link(DATA_DIR, server, nick, auth["code"], auth["redirect_uri"])

        def done(result):
            auth, answer = result
            self.discord_busy = False
            message, use = self.discord_link_message(nick, auth, answer)
            if use and nick_ok(use):
                self.nick_row.set_text(use)
                self.store()
            self.toasts.add_toast(Adw.Toast(title=message, timeout=10))
            self.ask_nick_status()
        self.in_thread(work, done)

    @staticmethod
    def discord_link_message(nick, auth, answer):
        """(message, nickname to put in the field or "") for an authorization's outcome."""
        if not auth.get("ok"):
            error = auth.get("error")
            if error == "denied":
                return tr("You cancelled the authorization in Discord."), ""
            if error == "busy":
                return tr("Ports 53134 to 53136 are in use: close other open launchers."), ""
            if error == "timeout":
                return tr("Discord's authorization did not arrive in time (3 minutes)."), ""
            return tr("Could not authorize with Discord."), ""
        if not answer:
            return tr("Could not reach the server."), ""
        status, account, named = answer.get("status"), answer.get("discord", ""), answer.get("nick", "")
        if status == "linked":
            return tr("Done: {} is linked to your Discord account {}. If you reinstall or change PC, press Recover "
                      "with Discord.").format(nick, account), ""
        if status == "recovered":
            got = named or nick
            return tr("Recovered: {} is yours again on this installation (Discord: {}).").format(got, account), got
        if status == "own_nick":
            return tr("This installation already had the nickname {}, not linked to Discord. It is filled in: press "
                      "Link with Discord to recover it on any PC.").format(named), named
        if status == "no_nick":
            return tr("Your Discord account {} has no nickname linked on this server: type one and press Link with "
                      "Discord.").format(account), ""
        if status == "other_nick":
            return tr("Your Discord account already has the nickname {} (one per account): it is filled in.").format(named), named
        if status == "taken":
            return tr("That nickname belongs to someone else or to another Discord account."), ""
        if status in ("invalid", "reserved"):
            return tr("The server did not accept the request."), ""
        return tr("The server could not confirm the account with Discord. Try again."), ""

    def confirm_discord_unlink(self):
        server, nick = self.server(), self.nick()

        def unlink():
            def done(ok):
                self.toasts.add_toast(Adw.Toast(title=tr("Nickname unlinked from Discord.") if ok
                                                else tr("Could not unlink.")))
                self.ask_nick_status()
            self.in_thread(lambda: ryunet_http.discord_unlink(DATA_DIR, server, nick), done)
        self.confirm(tr("Unlink {} from Discord?").format(nick),
                     tr("You can change the nickname afterwards, but can no longer recover it with Discord if you "
                        "reinstall."), tr("Unlink"), unlink)

    def discord_activity(self):
        """What Discord shows while the game runs; joinable online (the Windows launcher's)."""
        if self.process is None:
            return None
        return discord_activity_for(self.ini, self.game_started, self.party)

    def poll_party(self):
        """The lobby this player hosts, every 5 s while the game runs online: Discord's
        "1 of 8" follows players joining and leaving."""
        if self.process is None or self.ini.get("online") != "1" or self.party_busy:
            return
        if time.monotonic() - self.party_asked < 5:
            return
        self.party_busy, self.party_asked = True, time.monotonic()
        server, nick = self.ini.get("online_server", ONLINE_SERVER), self.ini.get("online_name", "")

        def done(answer):
            self.party_busy = False
            self.party = answer
        self.in_thread(lambda: ryunet_http.party(DATA_DIR, server, nick), done)

    def poll_announcement(self):
        """Server announcements ("maintenance in 5 minutes"), every 30 s while online play is on:
        a new one goes to the game in ryunet_announce.txt next to bbport.ini (drawn over the
        picture) and to a toast."""
        if self.ini.get("online") != "1" or self.announce_busy or time.monotonic() - self.announce_asked < 30:
            return
        self.announce_busy, self.announce_asked = True, time.monotonic()
        server = self.ini.get("online_server", ONLINE_SERVER)

        def done(answer):
            self.announce_busy = False
            if not answer or str(answer.get("id")) == self.announce_seen:
                return
            self.announce_seen = str(answer.get("id"))
            text = write_announcement(answer)
            self.toasts.add_toast(Adw.Toast(title=tr("Server notice: {}").format(text), timeout=10))
        self.in_thread(lambda: ryunet_http.announcement(DATA_DIR, server), done)

    def on_discord_join(self, secret):
        """A join secret from Discord: "ctr1|<server>|<nickname>"."""
        parsed = parse_join_secret(secret)
        if not parsed:
            self.toasts.add_toast(Adw.Toast(title=tr("That Discord invite is not from this launcher.")))
            return
        server, nick = parsed
        if nick.lower() == self.ini.get("online_name", "").lower():
            return  # our own invite
        self.present()
        if server.lower() != self.ini.get("online_server", ONLINE_SERVER).strip().lower():
            # Never switch servers silently: a crafted invite could send us to any server.
            self.confirm(tr("Invite from {}").format(nick),
                         tr("It is for the server {} and you use {}. Only switch if you trust that server: it will see "
                            "your nickname and your online play.").format(server, self.ini.get("online_server", ONLINE_SERVER)),
                         tr("Switch server and join"), lambda: self.accept_join(server, nick))
            return
        self.accept_join(server, nick)

    def accept_join(self, server, nick):
        """The host becomes a friend (the game lists friends' lobbies as joinable) and the game
        joins their session by itself, as a PS4 invitation accepted in the system UI
        (src/runtime_services.c reads ryunet_join.txt next to bbport.ini)."""
        apply_join(server, nick)
        self.ini, self.ini_lines = load_ini()
        self.online_row.set_active(True)
        self.server_row.set_text(server)
        if self.process is not None:
            self.toasts.add_toast(Adw.Toast(title=tr("{}'s invite accepted: the game joins their match in a few seconds. "
                                                     "If it does not, open Friends in the game and join {}.").format(nick, nick),
                                            timeout=10))
            return
        self.toasts.add_toast(Adw.Toast(title=tr("{}'s invite accepted: the game starts and joins their match (if not, "
                                                 "open Friends and join {}).").format(nick, nick), timeout=10))
        if self.launch_button.get_sensitive():
            self.on_launch(None)

    def tick(self):
        """Every second: Discord's presence and its invites, the party size, announcements."""
        self.presence.accept_requests = self.settings.get("discord_accept_requests", True)
        self.presence.update(self.settings.get("discord_rpc", True), self.discord_app_id(), self.discord_activity())
        while (secret := self.presence.take_join()) is not None:
            self.on_discord_join(secret)
        self.poll_party()
        self.poll_announcement()
        self.poll_trophies()
        status, error = self.presence.status()
        text = {discord_ipc.CONNECTED: tr("Connected to Discord."),
                discord_ipc.NO_DISCORD: tr("Discord is not running; it connects on its own once you open it."),
                discord_ipc.NO_APP_ID: tr("The Discord application ID is missing."),
                discord_ipc.ERROR: tr("Discord refused the connection: {}").format(error),
                discord_ipc.OFF: tr("Off") if not self.settings.get("discord_rpc", True) else tr("Connecting…")}[status]
        self.discord_status_row.set_subtitle(GLib.markup_escape_text(text))
        return True

    # --- log page and process ------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        # Shown when the game's log reports the known clobbered-dword bug (issue #2): the log
        # holds the diagnostics that can find its writer, so ask for it.
        self.issue_notice = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8,
                                    margin_top=8, margin_bottom=2, margin_start=8, margin_end=8,
                                    visible=False)
        self.issue_notice.add_css_class("card")
        issue_label = Gtk.Label(
            label=tr("The game hit the known \"clobbered command buffer\" bug (issue #2). "
                     "Please export this log and attach it to the issue so the cause can be fixed."),
            wrap=True, xalign=0, hexpand=True, margin_top=6, margin_bottom=6, margin_start=6)
        self.issue_notice.append(issue_label)
        issue_button = Gtk.Button(label=tr("Open issue #2"), valign=Gtk.Align.CENTER, margin_end=6)
        issue_button.connect("clicked", self.on_open_issue)
        self.issue_notice.append(issue_button)
        box.append(self.issue_notice)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.set_top_margin(8)
        self.log_view.set_left_margin(8)
        self.log_view.set_right_margin(8)
        self.log_scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        box.append(self.log_scroller)
        # bbport 0.5 (PR 78): the log to the clipboard or to a file, for bug reports.
        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6, margin_top=6,
                      margin_bottom=6, margin_start=8, margin_end=8)
        self.export_log_button = Gtk.Button(label=tr("Export log…"))
        self.export_log_button.connect("clicked", self.on_export_log)
        bar.append(self.export_log_button)
        self.copy_log_button = Gtk.Button(label=tr("Copy"))
        self.copy_log_button.connect("clicked", self.on_copy_log)
        bar.append(self.copy_log_button)
        box.append(bar)
        buffer = self.log_view.get_buffer()
        buffer.connect("changed", self.on_log_changed)
        self.on_log_changed(buffer)
        return box

    def on_log_changed(self, buffer):
        has_text = buffer.get_char_count() > 0
        self.copy_log_button.set_sensitive(has_text)
        self.export_log_button.set_sensitive(has_text)

    def log_text(self):
        buffer = self.log_view.get_buffer()
        return buffer.get_text(*buffer.get_bounds(), False)

    def on_open_issue(self, _button):
        open_external(ISSUE2_URL)

    def note_issue_line(self, line):
        """A line of the game's log: the notice appears at the first mention of the bug."""
        if ISSUE2_MARKER in line and not self.issue_notice.get_visible():
            self.issue_notice.set_visible(True)
            self.toasts.add_toast(Adw.Toast(
                title=tr("Known bug (issue #2) hit: please export the log and attach it to the issue"),
                timeout=10))

    def on_copy_log(self, _button):
        self.get_clipboard().set(self.log_text())
        self.toasts.add_toast(Adw.Toast(title=tr("Log copied to the clipboard")))

    def on_export_log(self, _button):
        text = self.log_text()
        dialog = Gtk.FileDialog(title=tr("Export log"))
        dialog.set_initial_name(f"sacredfire-{time.strftime('%Y%m%d_%H%M%S')}.log")

        def finish(dialog, result):
            try:
                chosen = dialog.save_finish(result)
            except GLib.Error:
                return  # cancelled
            try:
                Path(chosen.get_path()).write_text(text, encoding="utf-8")
                self.toasts.add_toast(Adw.Toast(title=tr("Log saved to {}").format(chosen.get_path())))
            except OSError as error:
                self.toasts.add_toast(Adw.Toast(title=tr("Could not save the log: {}").format(error)))
        dialog.save(self, None, finish)

    # --- Steam ---------------------------------------------------------------------------

    def on_add_to_steam(self, _button):
        """A non-Steam shortcut that starts the game directly (--play: no launcher window, so
        it works in the Steam Deck's game mode). Steam rewrites shortcuts.vdf when it exits, so
        it has to be closed while the entry is written."""
        if steam_shortcuts.steam_running():
            self.toasts.add_toast(Adw.Toast(title=tr("Close Steam first: it overwrites its shortcuts when it exits"),
                                            timeout=6))
            return
        files = steam_shortcuts.shortcut_files()
        if not files:
            self.toasts.add_toast(Adw.Toast(title=tr("No Steam user folder found (userdata)"), timeout=6))
            return
        exe, start_dir, options = steam_shortcuts.launcher_command()
        entry = steam_shortcuts.make_entry(exe, start_dir, launch_options=options)
        added = replaced = 0
        for path in files:
            try:
                if steam_shortcuts.add_shortcut(path, entry):
                    added += 1
                else:
                    replaced += 1
            except (OSError, ValueError) as error:
                self.toasts.add_toast(Adw.Toast(title=tr("Could not write {}: {}").format(path, error), timeout=8))
                return
        self.toasts.add_toast(Adw.Toast(
            title=tr("Added to Steam as \"{}\" ({} added, {} updated); start Steam to see it").format(
                steam_shortcuts.NAME, added, replaced), timeout=8))

    def append_log(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        if self.ini.get("online") == "1" and not nick_ok(self.ini.get("online_name", "")):
            self.toasts.add_toast(Adw.Toast(title=tr("Online play needs your own nickname (3-16 letters, digits, _ or -; not RyuPC or Player)"),
                                            timeout=8))
            return
        self.log_view.get_buffer().set_text("")
        self.issue_notice.set_visible(False)
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in self.environment().items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            self.process = launcher.spawnv(["setsid", "bash", str(PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=tr("Could not start: {}").format(error.message)))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.game_started = int(time.time())
        self.party, self.party_asked = None, 0.0
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.update_launch_button()
        self.stack.set_visible_child_name("log")
        self.fill_saves()  # locked while the game runs
        self.fill_mods()
        if self.settings.get("hide_on_play"):
            self.set_visible(False)  # the application keeps running: the window is still its own

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        if line.startswith("@progress "):
            self.shader_progress(line)  # not shown in the log
        else:
            self.append_log(line + "\n")
            self.note_issue_line(line)
        self.read_line()

    def shader_progress(self, line):
        """run.sh's "@progress native-shaders <percent> <stage>" lines (scripts/native_shaders.py):
        a window with a progress bar while the native engine's shaders are built, the first time."""
        parts = line.split()
        if len(parts) < 3 or parts[1] != "native-shaders":
            return
        if parts[2] == "done":
            if getattr(self, "shader_window", None):
                self.shader_window.destroy()
                self.shader_window = None
            if self.settings.get("hide_on_play"):
                self.set_visible(False)
            return
        try:
            percent = max(0, min(100, int(parts[2])))
        except ValueError:
            return
        stage = parts[3] if len(parts) > 3 else ""
        if not getattr(self, "shader_window", None):
            if not self.get_visible():
                self.set_visible(True)
            self.present()
            window = Gtk.Window(transient_for=self, modal=True, title=tr("Compiling the native engine's shaders"),
                                default_width=460, deletable=False)
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=18, margin_bottom=18,
                          margin_start=18, margin_end=18)
            heading = Gtk.Label(label=tr("Compiling the native engine's shaders"), xalign=0)
            heading.add_css_class("title-4")
            note = Gtk.Label(label=tr("Only the first time: they are built from your copy of the game and take a few "
                                      "minutes. The game opens by itself when they are done."),
                             wrap=True, xalign=0)
            note.add_css_class("dim-label")
            self.shader_bar = Gtk.ProgressBar(show_text=True)
            self.shader_step = Gtk.Label(xalign=0)
            self.shader_step.add_css_class("dim-label")
            cancel = Gtk.Button(label=tr("Cancel"), halign=Gtk.Align.END)
            cancel.connect("clicked", lambda _b: self.stop_game())
            for widget in (heading, note, self.shader_bar, self.shader_step, cancel):
                box.append(widget)
            window.set_child(box)
            window.present()
            self.shader_window = window
        steps = {"extract": tr("Reading the game's shaders..."), "export": tr("Preparing the programs..."),
                 "translate": tr("Translating the shaders..."), "compile": tr("Compiling the shaders...")}
        self.shader_bar.set_fraction(percent / 100)
        self.shader_bar.set_text(f"{percent} %")
        self.shader_step.set_label(steps.get(stage, tr("Finishing...")))

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        if getattr(self, "shader_window", None):  # cancelled or failed while building the shaders
            self.shader_window.destroy()
            self.shader_window = None
        self.process = None
        self.game_started = 0
        self.party = None
        self.reload_game_settings()
        self.update_launch_button()
        self.update_game_status()
        self.fill_saves()
        self.fill_mods()
        if self.trophy_set is not None:
            self.fill_trophies()  # new unlocks, and Reset again
        self.append_log(tr("\n— the game exited (code {}) —\n").format(status))
        if self.settings.get("close_on_play") and status == 0:
            self.close()
            return
        if not self.get_visible():
            self.set_visible(True)
        self.present()
        if status == UNSUPPORTED_GPU_CODE:
            self.inform("Vulkan 1.3 / sparse binding", tr("Your graphics card or its driver doesn't support Vulkan 1.3 / sparse binding, which the game "
                                         "needs. Update the GPU driver; if it is already the latest, the card is too "
                                         "old for this system."))

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        self.presence.shutdown()  # clears the Discord status
        return False


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID,
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        window = self.get_active_window() or LauncherWindow(self)
        window.present()


def play():
    """--play: the game with the saved settings, no window (Steam's shortcut, the Steam Deck's
    game mode). The game runs as a child while this process does what the window would: the
    player's Discord status with invites and "Ask to Join", and server announcements."""
    settings = load_settings()
    set_language(settings.get("ui_language", ""))
    if not (Path(settings["game_dir"]).expanduser() / "eboot.bin").is_file():
        print("SacredFire: choose the game folder in the launcher first", file=sys.stderr)
        return 1
    os.chdir(PORT_DIR)
    env = game_environment(settings)
    steam_shortcuts.stable_appimage_link()
    if not settings.get("discord_rpc", True):
        os.execvpe("bash", ["bash", str(PORT_DIR / "run.sh")], env)  # nothing to do alongside
    child = subprocess.Popen(["bash", str(PORT_DIR / "run.sh")], env=env)

    def forward(signum, _frame):
        # Steam stops the game by signalling this process: the game gets it too.
        try:
            child.send_signal(signum)
        except OSError:
            pass
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, forward)

    import threading
    presence = discord_ipc.Presence()
    presence.accept_requests = settings.get("discord_accept_requests", True)
    app_id = settings.get("discord_app_id") or DISCORD_APP_ID
    started = int(time.time())
    state = {"party": None, "party_at": 0.0, "announce_at": 0.0, "seen": "", "busy": set()}

    def in_background(name, work):
        if name in state["busy"]:
            return
        state["busy"].add(name)

        def run():
            try:
                work()
            finally:
                state["busy"].discard(name)
        threading.Thread(target=run, daemon=True).start()

    while child.poll() is None:
        ini, _lines = load_ini()
        online = ini.get("online") == "1"
        server, nick = ini.get("online_server", ONLINE_SERVER), ini.get("online_name", "")
        presence.update(True, app_id, discord_activity_for(ini, started, state["party"]))
        while (secret := presence.take_join()) is not None:
            parsed = parse_join_secret(secret)
            if not parsed or parsed[1].lower() == nick.lower():
                continue
            if parsed[0].lower() != server.strip().lower():
                # Never switch servers silently, and there is no window to ask in.
                print(f"SacredFire: Discord invite from {parsed[1]} for another server ({parsed[0]}) ignored; "
                      "accept it with the launcher open", file=sys.stderr)
                continue
            apply_join(*parsed)
            print(f"SacredFire: {parsed[1]}'s Discord invite accepted: the game joins their match", file=sys.stderr)
        now = time.monotonic()
        if online and now - state["party_at"] >= 5:
            state["party_at"] = now

            def ask_party(server=server, nick=nick):
                state["party"] = ryunet_http.party(DATA_DIR, server, nick)
            in_background("party", ask_party)
        if online and now - state["announce_at"] >= 30:
            state["announce_at"] = now

            def ask_announcement(server=server):
                answer = ryunet_http.announcement(DATA_DIR, server)
                if answer and str(answer.get("id")) != state["seen"]:
                    state["seen"] = str(answer.get("id"))
                    write_announcement(answer)
            in_background("announce", ask_announcement)
        time.sleep(1)
    presence.shutdown()
    code = child.returncode
    return code if code >= 0 else 128 - code


if __name__ == "__main__":
    if "--play" in sys.argv[1:]:
        sys.exit(play())
    sys.exit(LauncherApp().run([a for a in sys.argv if a != "--play"]))
