<p align="center">
  <img src="docs/images/logo.png" alt="CTR Nitro-Fueled PC" width="360">
</p>

# CTR-NitroFueled-PC

> **EU mod packages:** [CUSA14876 01.21 loader support and update instructions](docs/eu-mod-packages.md).
> [CNK Drift Gauge + Reserves Meter — EU](https://github.com/Majed-allababedi/CTR-CNK-Drift-Gauge-EU): original mod by tomtotom, EU adaptation by Majed-allababedi. Select **Speedometer HUD** in-game.

**Crash Team Racing: Nitro-Fueled (PS4, CUSA13795 & CUSA14876 v1.21) running natively on Windows 10/11 and
Linux, with online play.**

This is a Windows and Linux port of [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire), the native Linux
port of the game by T3CHNOLOG1C. The game's own x86-64 code runs directly on the CPU, a small
runtime replaces the PS4 system libraries, and a renderer derived from shadPS4 translates the PS4
GPU work to Vulkan. The Windows layer (loader, memory, TLS, exceptions, files, timers) comes from
Supermedo's Windows port of bbport: see [packaging/windows/README.md](packaging/windows/README.md).

On top of that, this build includes **online play** (see [Online](#online) below), **European (CUSA14876 v1.21) and Arabic support**, **AMD FSR 3.1 Frame Generation**, **Native Vulkan renderer**, and **Mod package support**.

> **No game files are included.** You need your own dump of the game and its 1.21 update.
> Both builds of the 1.21 update work: the one for firmware 5.05 (the reference) and the one for
> 6.72+ (since v0.2.1).
> Not affiliated with or endorsed by Sony Interactive Entertainment, Activision or Beenox.
> *Crash Team Racing*, *Nitro-Fueled* and *PlayStation* are trademarks of their owners, named
> here only to identify the game this software runs.

**Status on Windows: playable.** Menus, Adventure and Arcade, races, sound, cutscenes, Xbox/SDL
controllers and keyboard, saving, the community patches (60 FPS, no motion blur), restart from
the in-game menu, and the game's DLC from your own packages (optional). Internal resolution from
480p to native 4K, with AMD FSR 1 or temporal upscaling (AMD FSR 3.1, or NVIDIA DLSS on RTX
cards; experimental). Tested on
Windows 11 24H2 with an NVIDIA RTX 3070 and an Intel i5-9500: 110-130 FPS uncapped in races with
the 60 FPS patch (the FPS limit keeps it at 60 by default; up to 240). From v0.5.3 the first
start prepares the game's shaders for your GPU from your own dump (a few seconds, once:
[shaders/README.md](shaders/README.md)), so new tracks and racers no longer stop the game to
compile them; the few the recipe lacks are compiled when first seen and cached.

| Release | What it brought |
|---|---|
| [v0.4](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.4) | Redesigned launcher with a *Controls* tab (live controller test and rumble, players 1-4, gamepad buttons remapped by pressing them, stick dead zone; the keyboard page moves there); *Achievements*: trophies kept when unlocked, a launcher-style notice in the game (picture, name, grade, what it was for) and a tab with all 48 (picture, name, what to do, progress and dates); the in-game menu restyled in tabs, in Spanish or English; game languages per edition; AMD FSR 4 (experimental) in the *Upscaling* list; *Frame generation* (AMD FSR 3, experimental): an interpolated frame between each pair of game frames; *Aspect ratio* 16:9, 21:9 UltraWide or 32:9 Super UltraWide with centered HUD (by tcosi, in Settings > Display, not Patches); the European edition CUSA14876 (by martijnhe), ultrawide included; the game's 2x MSAA as a setting, off by default (fixes dotted sky lines); CPUs without MOVBE/BMI1/LZCNT (by MetaHeart); damaged dumps reported (by Naif Alqurashi); rare libwinpthread crashes fixed; FSR 3 no longer turns off without Resizable BAR |
| [v0.3](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.3) | Internal resolution 480p to native 4K (the game's own render scale; replaces the PS4 Pro switch); one *Upscaling* list: AMD FSR 1, AMD FSR 3.1 or NVIDIA DLSS (temporal, experimental) with DLSS quality modes, output and model (Transformer/CNN); game languages of this build (English, Latin American Spanish, French, Brazilian Portuguese); PS4 PKG Tool dumps (Image0 + Sc0); fullscreen switch in game fixed |
| [v0.2.1](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.2.1) | The 1.21 update built for firmware 6.72+ is accepted (patches found by signature); keyboard page in the launcher to rebind keys |
| [v0.2.0](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.2.0) | Launcher (`CTR-NitroFueled-PC.exe`): game setup with hard-link merge, settings, patches, save backups and imports, optional DLC; bundled Python; the project's icon |
| [v0.1.1](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.1.1) | First portable build: FPS limit up to 240, fullscreen switch, licenses in the package |

### Quick start (Windows)

**Easiest: the portable build.** Download `CTR-NitroFueled-PC-portable-<version>-windows.zip`
from [Releases](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases), unzip it anywhere and
open **`CTR-NitroFueled-PC.exe`**, the launcher: choose your `CUSA13795` or `CUSA14876` (base game) and
update 1.21 folders, press *Prepare the game* once (it merges them with hard
links, no extra disk space) and then *Play*. Python is bundled; you only need a Vulkan 1.3 GPU.

**Updating:** unzip the new version over your folder to keep your settings, saves and nickname
key, or unzip it into a new folder and copy `user\`, `bbport.ini`, `launcher.ini` and
`ryunet_online.key` across.

**Linux and Steam Deck:** Download the AppImage from the same Releases page (see [AppImage](#appimage-steam-deck) below). Data, saves and the nickname key live in `~/.local/share/sacredfire`.

### Online

Online connects to the community server **octr.ryu7w7.xyz** by default (or any compatible custom
server). From the game's own menus you can:

- race in **public matches** with other players, alone or with your **group**;
- create **private rooms** and invite your friends;
- see your **friends list** in the game;
- buy items in the **Pit Stop**: with the items bought on the server, or with the ones already
  unlocked in your own save (launcher: Online tab).

**Your nickname.** Each player needs their own nickname (3 to 16 letters, digits, `_` or `-`).
Shared default names (RyuPC, Player, Jugador...) are refused. The first installation that plays
with a nickname keeps it, through the key in `ryunet_online.key` (copied to
`%APPDATA%\CTR-NitroFueled-PC`): **never share that file.**

**Discord** (Windows launcher, Online tab):

- **Link with Discord** ties your nickname to your Discord account. *Recover with Discord* gets
  it back on any PC or after reinstalling. Linked nicknames never expire; unlinked ones that
  nobody uses for 60 days become free again.
- **Presence and invites:** with Discord open, your profile shows what you are playing, and an
  invite sent from Discord drops the friend who accepts it straight into your private room.

**Playing offline:** turn online off in the launcher's Online tab, or set `BB_ONLINE=0`. The
server address can be changed there too, to play on your own private server.

**Connection problems:** the game connects players directly to each other (UDP port 3074), as
on the PS4. Some home connections (strict NAT, mobile data, carrier-grade NAT) cannot reach some
other players. If a match keeps failing with the same people, enabling UPnP on your router or
forwarding UDP 3074 to your PC helps.

**No donations:** this is a non-commercial fan project. It does not accept donations and sells
nothing; online play is free and the same for everyone. You need your own copy of the game.

**Picture** (launcher: Settings > Display; in game: Insert):

- *Internal resolution*: the size the game draws its 3D scene at. 480p and 720p for modest PCs;
  1440p to 2160p output a 4K picture, which also smooths edges on a 1080p screen. Needs a restart
  (the in-game menu does it).
- *Upscaling*, one at a time, switched live:
  - **AMD FSR 1** (default): a sharper spatial enlargement than the game's own stretch.
  - **AMD FSR 3** (experimental, any GPU) and **NVIDIA DLSS** (experimental, GeForce RTX; the files
    come in the package): temporal upscaling that rebuilds detail over several frames from the
    game's depth and motion, and works as anti-aliasing at the same resolution. Turn them off if
    you see trails.
- With DLSS: *DLSS quality* (Auto follows the internal resolution; DLAA, Quality, Balanced,
  Performance and Ultra Performance render at NVIDIA's share of the *DLSS output*, 1080p or 4K,
  after a restart) and *DLSS model* (Transformer, recommended, or the lighter CNN).

**Game language** (launcher: Settings > Console): the languages this build (CUSA13795, the
Americas) has: English, Latin American Spanish, French and Brazilian Portuguese. Spain's Spanish is
not in it.

The launcher also has the game patches, FPS limit, display, monitor and controller settings, save
backups and imports (including decrypted PS4 saves), and optional DLC: add your own DLC packages
(`.pkg`) and the port reports them installed (CTR Nitro-Fueled's DLC only carry the license; the
content is in the game). Spanish and English.

### Which game dumps work

The launcher needs the game as **folders of decrypted files** from **CUSA13795** (the USA
edition) or, from v0.5.3, **CUSA14876** (the European edition): the base game (v01.00) and the
1.21 update. The launcher tells which edition you chose and shows its patches and languages. The folder names do not matter. When you
choose a folder, the launcher says in green or red what it found.

| Your dump | What it looks like | Works? |
|---|---|---|
| Dumped from your own PS4 with a game dumper | `eboot.bin`, `sce_module\`, `sce_sys\param.sfo`… directly in each folder (base, update) | Yes |
| Extracted with **PS4 PKG Tool** | `Image0\` (eboot.bin, sce_module, the game data) and `Sc0\` (param.sfo and the rest of sce_sys) | Yes, from v0.3: choose the folder that holds `Image0` and `Sc0` and press *Prepare the game*. With v0.2.1, first copy **everything** in `Image0` to that folder and everything in `Sc0` to a new `sce_sys` folder in it |
| Any other tool | Works if the files end up in one of the two layouts above | Yes |
| A `.pkg` file that was not extracted | | No: extract it first. Only the DLC tab takes `.pkg` files |
| The European edition (CUSA14876) | | Yes, from v0.5.3, also online with the USA edition |
| Another region or edition (JP…) | | No: each one has a different executable |
| Encrypted files (straight from a disc or an official package) | `eboot.bin could not be read as a decrypted PS4 executable` | No: dump the game again |

Both builds of the 1.21 update work (for firmware 5.05 and for 6.72+). If something fails, the
launcher's log and `user\last_run.log` say which file is missing; attach `last_run.log` when you
[open an issue](https://github.com/GetLostIn/CTR-NitroFueled-PC/issues).

**Or build it yourself:**

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64` and the packages listed in
   [packaging/windows/README.md](packaging/windows/README.md).
2. In the **MSYS2 CLANG64** shell: `git clone --recursive <this repository>`, then
   `bash build.sh` (builds `out/sacredfire.exe`).
3. Merge the 1.21 update over the base game without copying it:
   `python tools/merge_update.py <CUSA13795> <CUSA13795-patch> <CUSA13795-1.21>`.
4. Run: `set BB_GAME_DIR=<CUSA13795-1.21>` and `C:\msys64\clang64\bin\python.exe run.py`.
   In the game, **Insert** (or L3+R3) opens the port's menu.

For local builds, GitHub Actions builds, and tagged releases, follow the [desktop build guide](docs/MANUAL_BUILDS.md).

CPUs without MOVBE, BMI1 or LZCNT (Sandy Bridge, Ivy Bridge) also need `iced-x86` for the Python
that runs `run.py`, which the port uses to rewrite those instructions at start
([15-isa-rewrite.md](docs/sacredfire/15-isa-rewrite.md), issue #31). MSYS2's Python cannot install
the wheel: use a python.org Python 3 (`pip install iced-x86`) or the portable ZIP from Releases,
which bundles it.

### Credits

This fork stands on the work of:

- **[T3CHNOLOG1C](https://github.com/T3CHNOLOG1C)**: [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire),
  the native port of CTR Nitro-Fueled this fork is built from.
- **[deadinside28](https://github.com/deadinside28)**: [bbport](https://github.com/deadinside28/bloodborne_pc),
  the native Bloodborne port whose loader, runtime and renderer SacredFire builds on.
- **[Supermedo](https://github.com/Supermedo)**: the [Windows port of bbport](https://github.com/Supermedo/bloodborne_pc)
  (branch `windows`), whose Windows layer is merged here.
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the video core and shader recompiler.
- **illusion** and **TroyWarez**: the game patches ([illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)).
- **[IFreemz](https://github.com/IFreemz)**: [shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR),
  whose DLSS bridge (MIT, `dlss_bridge/`) and NGX loader this fork uses.
- **[FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)**: AMD FidelityFX FSR 3.1 on Vulkan (MIT).
- **NVIDIA DLSS**: `nvngx_dlss.dll` from the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS),
  redistributed under its license (in `licenses\` of the package). NVIDIA, the NVIDIA logo, DLSS
  and GeForce RTX are trademarks and/or registered trademarks of NVIDIA Corporation. The GPL
  program contains no NVIDIA code: it loads the separate bridge DLL when present.
- Windows port of SacredFire: [GetLostIn](https://github.com/GetLostIn), with Claude (Anthropic).
- **[leomoon](https://github.com/leomoon)**: Linux crash diagnostics.
- **Camiloop**: the "Unlock Everything" patch.

Licensed under the GNU GPL v2 or later, as SacredFire, bbport and shadPS4. The rest of this
README is SacredFire's own (Linux, Steam Deck).

---

# SacredFire

SacredFire runs *Crash Team Racing: Nitro-Fueled* (PlayStation 4, CUSA13795, update 1.21) as a
native x86-64 Linux executable. It is built on [bbport](https://github.com/deadinside28/bloodborne_pc)
(GPL-2.0-or-later), the native Linux port of Bloodborne: the game's own code runs on the CPU, a
small runtime replaces the PS4 system libraries, and a renderer derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4) translates the PS4 GPU work to Vulkan.
Developed and tested on a Steam Deck.

> **No game files are included.** You need your own dump of the game and its 1.21 update.
> Both builds of the 1.21 update work: the one for firmware 5.05 (the reference) and the one for
> 6.72+ (since v0.2.1).
> This project is not affiliated with or endorsed by Sony Interactive Entertainment, Activision
> or Beenox. *Crash Team Racing*, *Nitro-Fueled* and *PlayStation* are trademarks of their
> owners, named here only to identify the game this software runs.

**Status: playable.** Menus, arcade and adventure races with sound, controller and saving; split
screen with a second controller; fur and the sky render correctly. 30 FPS as on the PS4, or the
60 FPS patch from the in-game menu (60 FPS in menus, 37–45 FPS in races on the Steam Deck). A
full play-through has not been verified, and the Steam Deck (RADV) is the only tested machine.

## How it works

- **Native execution.** The eboot is converted offline (`scripts/`) into a flat memory image
  with the game's own PS4 libc, libSceFios2 and libSceNpToolkit2 linked in as native code. No
  CPU emulation: the game code runs at full speed.
- **A runtime for this game** (`src/runtime_*.c`) implements the PS4 OS functions the game calls:
  memory, threads, synchronisation, event flags, files, audio (ATRAC9 included), pad, saves,
  user service, network stubs. Imports are resolved by NID and library name; the names come
  from shadPS4's aerolib list, fetched once at the first start.
- **GPU** (`gpu/`): the vendored shadPS4 video core and shader recompiler with bbport's
  multi-threaded command processing (decode and draw recording on separate threads) and
  SacredFire's fixes: geometry-shader ring addressing (fur), 1D textures lowered to 2D (the
  sky's colour LUT), texture overlap resolution at 1080p, PM4 stream resynchronisation, a
  reliable on-disk shader cache.
- **Game patches** from [illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)
  (`patches/CrashTeamRacing.xml`, authors illusion and TroyWarez) are compiled at start:
  60 FPS unlock, motion blur off, 1080p in PS4 Pro mode (the published address of that one was
  off by one byte; our copy is corrected).

## Requirements

- Linux x86-64 with a Vulkan 1.3 GPU. Tested: Steam Deck (AMD Van Gogh, Mesa RADV). Geometry
  shaders are required (fur), so macOS/MoltenVK is out; see
  [docs/sacredfire/10-other-platforms.md](docs/sacredfire/10-other-platforms.md).
- Or Windows 10 (1903+)/11 64-bit with a Vulkan 1.3 GPU, built with MSYS2 CLANG64: see
  [packaging/windows/README.md](packaging/windows/README.md). Tested: Windows 11, NVIDIA RTX 3070.
- Your decrypted dump of CUSA13795 with the 1.21 update merged in: copy the dumped update over
  the base game, replacing files (eboot.bin and sce_sys too), or use `tools/merge_update.py`
  (it also reads PS4 PKG Tool's Image0 + Sc0 layout; see "Which game dumps work" above). The launcher and `run.sh` check
  the executable image and say what is missing (`BB_SKIP_GAME_CHECK=1` skips the check).
- To build: GCC, CMake, Ninja, Python 3 (for CPUs without MOVBE, BMI1 or LZCNT,
  `pip install iced-x86`: [15-isa-rewrite.md](docs/sacredfire/15-isa-rewrite.md)), glslang, SDL3,
  Vulkan headers and the libraries in `shell.nix` (with [Nix](https://nixos.org), `shell.nix`
  provides them). On Arch: `tools/deps.sh` fetches the header-only libraries the distribution
  lacks.

## Build and run

```bash
git clone --recursive https://github.com/T3CHNOLOG1C/SacredFire.git && cd SacredFire
bash build.sh                        # out/sacredfire and out/gpu/libbbgpu.so (~40 CPU-minutes)
BB_GAME_DIR=/path/to/CUSA13795 bash run.sh
```

`run.sh` prepares the image (`out/`), compiles the selected patches, builds if needed and starts
the game. The game folder defaults to `orig/CUSA13795`. Saves and the shader cache go to `user/`,
settings to `bbport.ini`; `BB_DATA_DIR` moves all of them. The first start compiles shaders as it
goes; later starts preload them from the cache (`BB_CLEAR_SHADER_CACHE=1` starts fresh).

Or the launcher (pick the game folder and the controller, then *Start*):

```bash
bash launcher/bb-launcher.sh         # launcher/install-desktop.sh adds it to the app menu
```

The launcher (GTK4/libadwaita) has pages for the game folder, the controller, the game patches,
PS4 Pro mode, display (fullscreen, FPS counter) and developer switches (frame statistics, log to
a file, clear the shader cache, Vulkan validation, extra environment), and shows the game's
output. `--play` starts the game with the saved settings and no window (Steam Deck game mode).
The menu entry and the launcher use the game's own icon (`sce_sys/icon0.png`), copied from your
game folder into your icon theme; the game window shows it too.

**Native LZMA decoder (Linux).** Recognized CTR 01.21 executables use liblzma for independent
archive blocks, keeping the original compressed game files and requiring no disk cache. The
patch checks the decoder and its one-shot caller before installing; unsupported layouts and
blocks retain the original decoder. Set `BB_NATIVE_LZMA=0` in the launcher's extra environment
to compare or disable it. On hosts with at least 12 logical CPUs, the verified streaming queue
uses six existing background workers instead of four. Source builds require liblzma 5.4 or newer (`xz` in `shell.nix`).

**In-game menu** (Insert, or L3+R3 on the pad): game patches, controller ports for split
screen, FPS limit, fullscreen, FPS counter, menu language. It is saved to `bbport.ini`; patches apply after a restart.

**Controllers.** SDL3 gamepads; the launcher's *Controller* page or `BB_GAMEPAD=<GUID or part
of the name>` picks the one for port 1, and the port switches to whichever pad was used last
when the chosen one is missing. Split screen: assign pads to ports 2–4 in the in-game menu (or
`port.N=<GUID>` in `bbport.ini`); each port logs a user in and out as the pad comes and goes.
The keyboard works next to a gamepad: WASD left stick, arrow keys right stick, Space Cross,
LShift Circle, E Square, Q Triangle, 1/3 L1/R1, R/F L2/R2, Z/C L3/R3, Enter Options, IJKL
d-pad. The launcher's *Keyboard* page rebinds them (`key.<input>=` lines in `bbport.ini`;
`pad.<input>=` rebinds gamepad buttons).

**Achievements** (v0.4). The game's trophies unlock as on a PS4: each one is kept in
`user\trophies\<TITLE_ID>\unlocked.txt` with its date, shows a notice over the game, and the
platinum comes once all the others are. The launcher's *Achievements* tab shows them with their
pictures (from `sce_sys\trophy\trophy00.trp`), progress, dates and a reset (with a backup). Names,
descriptions and grades are encrypted in the game with the PS4's trophy key, which this port does
not include: with your own key (the same shadPS4 uses; paste it, or import it from shadPS4's
`keys.json`) they are decrypted as shadPS4 does and kept in `trophies.tsv`.

**PS4 Pro mode** (`BB_NEO=1`, the launcher's switch): the game sees a PS4 Pro (7-CPU mode,
5568 MiB of direct memory) and takes its Pro rendering path; with the 1080p patch it renders
at 1920×1080. Off by default: the Steam Deck is closer to a base PS4.

**Mods (Windows):** the Mods page lists built-in patches first, followed by installed
add-ons. Add `.pak`, `.zip`, `.xml` files or an unpacked folder to `mods/` beside the
launcher, using Add files / Add folder or copying them there and pressing Reload.
Manifest packages can combine assets and executable patches under one switch;
creator descriptions appear when the card's chevron is expanded. Legacy `patches/`
is still read. See the [creator and packaging guide](docs/mod-packaging.html) and
[implementation notes](docs/mod-support.txt), also available from the Mods page.
No actual mod payloads are bundled with this feature. Online compatibility is a
package declaration, not a server approval or whitelist service.

Enabled mods override matching relative paths through the engine's read-only file
map and sparse archive overlays. The selected source game directory stays unchanged:
no original game files are copied, linked, repacked or overwritten. Disabling mods
restores source reads. Only mod payloads, generated metadata and separate user saves/
shader caches are stored. This path requires the matching updated engine, not only
new launcher scripts. Multiple versions of a stable mod ID appear once, using the
newest validated version; older inputs remain
available for rollback. Different content claiming the same selected version is
reported as a conflict.

Mods do not need an engine-version/hash allowlist. Older manifests containing
those fields still load; the fields are no longer a launch gate. Engine features
are negotiated automatically, while game executable hashes, resource checksums,
XML signatures, conflicts and online declarations remain enforced.

The existing loose-folder CLI loader and `mods.json` remain available for older
workflows ([docs/MODS.md](docs/MODS.md), originally written for bbport). The unified
Mods UI/import flow is Windows-only in this revision; AppImage `run.sh` integration
is not included.

## AppImage (Steam Deck)

`packaging/appimage.sh` builds `dist/SacredFire-x86_64.AppImage` (about 430 MB) with Nix: the
binaries are built in `nix-shell shell.nix` so the bundle can collect their libraries, then
`nix bundle` packs them with the GTK launcher, Python and Mesa's Vulkan drivers (the header of
the script has the exact steps). Running it opens the launcher; `--play` starts the game. Data
lives in `~/.local/share/sacredfire`, launcher settings in `~/.config/sacredfire-launcher`.
Without a game folder chosen, the launcher shows the Game row as *not chosen* with Play disabled
until a folder passes the game check.

The [desktop build guide](docs/MANUAL_BUILDS.md) has the GitHub Actions workflow, Nix commands, version checks, Windows ZIP steps, and output checks.

**Adding the AppImage to Steam**: the launcher's *Settings → Steam → Add to Steam* writes the
shortcut (with `--play`, for the Steam Deck's game mode) while Steam is closed; by hand, *Add a
Non-Steam Game* needs no options. The compatibility tool does not matter. Steam's overlay is removed before the port's own programs start. Where
Steam runs games without FUSE, set the launch options to

```
TMPDIR=$HOME/.cache APPIMAGE_EXTRACT_AND_RUN=1 NO_CLEANUP=1 %command%
```

The AppImage then unpacks itself (~1.2 GB, `~/.cache/appimage_extracted_*`) on the first start
and reuses that copy; `TMPDIR` keeps that out of a RAM-backed `/tmp`. Add ` --play` after
`%command%` to skip the launcher. **NVIDIA:** startup discovers the host's 64-bit NVIDIA Vulkan
ICD and exposes its libraries next to the bundled AMD/Intel drivers (`BB_NVIDIA_LIB_DIR` when
they are not under `/usr/lib*`); `--vulkan-info` prints what the package sees. MangoHud is
bundled (launcher switch) and drawn once even when a system MangoHud or Steam's overlay is on.

## Useful variables

`BB_GAMEPAD` (controller for port 1), `BB_NEO=1` (PS4 Pro mode), `BB_FPS=30|60` (virtual
vblank; the 60 FPS patch sets 480 Hz by itself), `BB_FRAME_STATS=1` (frame statistics in the
log, including VRAM, GTT and RSS), `BB_CLEAR_SHADER_CACHE=1`, `BB_VK_VALIDATION=1` (validation
layer), `BB_FULLSCREEN=1`, `BB_PRESENT_MODE`, `BB_TIMEOUT=N` (stop after N seconds with thread
stacks; 0 = never), `BB_GAME_DIR`, `BB_USER_DIR`, `BB_DATA_DIR`, `BB_CONFIG`, `BB_PATCHES_DIR`,
`BB_SKIP_GAME_CHECK=1`, `BB_PAD_RECORD=file` / `BB_PAD_REPLAY=file` (record and replay input),
`BB_PRESENT_DUMP_TRIGGER=file` with `BB_PRESENT_DUMP_COUNT=N` (dump presented frames),
`BB_CAPTURE_TRIGGER=file` (frame analysis), `BB_BREADCRUMBS=0` (no GPU breadcrumbs),
`BB_GPU_PROFILE=1`, `BB_ANISO=N` (anisotropic filtering, 16 by default), `BB_PC_MODEL=1`
(bbport's experimental in-place memory model: frees CPU time but needs more GPU-visible memory
than the Steam Deck has). bbport's temporal upscalers (`BB_UPSCALER`, `BB_OBJECT_MOTION`,
`BB_UPSCALER_MENU=1` for their menu sections) were built for Bloodborne's scene and stay off.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and the runtime |
| `scripts/` | Offline preparation of the game image, module linking, game check, patch compiler, mods |
| `gpu/` | Renderer library: vendored shadPS4 video core (`gpu/VENDOR.txt`) with bbport's and SacredFire's changes, shims, ImGui menu |
| `patches/` | Community patches for CTR (`CrashTeamRacing.xml`) |
| `launcher/`, `packaging/` | GTK4 launcher and desktop entry; Nix package and AppImage |
| `tools/` | Developer tools: import-name generation, scripted runs, screenshots, A/B toggles, GPU checks |
| `tests/` | Loader, runtime, patch, game-check and renderer tests |
| `docs/sacredfire/` | SacredFire's notes: [status](docs/sacredfire/STATUS.md), setup, imports, Bloodborne gating and removal, fur, controllers, patches, other platforms |
| `docs/` | bbport's design notes and history ([upscaler](docs/upscaler.md), [parallel GPU](docs/parallel_gpu.md), [motion vectors](docs/motion_vectors.md), [roadmap](docs/ROADMAP.md)); its README: [docs/bbport/README.md](docs/bbport/README.md) ([Русский](docs/bbport/README.ru.md)) |

Tests: `bash build.sh --test`, `python3 -m unittest discover -s tests`, and
`ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test settings-test`.

## Reporting problems and contributing

Use the bug report template; the log (launcher *Log* tab or `run.sh` output, with
`BB_FRAME_STATS=1` for rendering and performance issues) is what matters. If the log says
`SacredFire issue #2` (the launcher shows a notice with an *Open issue #2* button), the game hit
the known clobbered command buffer bug: please attach that whole log to
[issue #2](https://github.com/T3CHNOLOG1C/SacredFire/issues/2), it carries the diagnostics that
can find the cause. **No game files** in issues or pull requests. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Known limitations

- Races with the 60 FPS patch reach 37–45 FPS on the Steam Deck: the port's GPU command thread
  is the limit at ~1400 draws per frame ([docs/sacredfire/08-patches.md](docs/sacredfire/08-patches.md)).
- Online features are stubbed in SacredFire; this fork plays online (see [Online](#online)).
- Windows: native build, from source only for now, without the GTK launcher and without the
  PC memory model ([packaging/windows/README.md](packaging/windows/README.md)).
- macOS is not supported ([docs/sacredfire/10-other-platforms.md](docs/sacredfire/10-other-platforms.md)).

## History

SacredFire started from bbport on 2026-10-09. The Bloodborne profile, its patches, launcher and
FSR 4 tooling were removed the same day
([docs/sacredfire/09-bloodborne-strip.md](docs/sacredfire/09-bloodborne-strip.md)). bbport's
temporal upscalers and motion vectors remain in `gpu/`, off; its libGnm hook code remains without
a function table (the table was generated from Bloodborne's executable and is not distributed).

## Credits and licenses

SacredFire is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)): it contains code
from bbport and shadPS4 (both GPL-2.0-or-later). Third-party components keep their licenses:
[bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (the loader, runtime and
renderer this builds on), [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader
recompiler, [sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT), AMD FidelityFX SDK (MIT),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui)
(MIT), DejaVu fonts, [miniz](https://github.com/richgel999/miniz) (MIT). Game patches by illusion
and TroyWarez ([illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)). Import
names from shadPS4's aerolib list.

### Aspect ratio

Display settings offer 16:9 (default), experimental 16:10, 21:9 and 32:9. The in-game
Graphics page offers the same choices after a restart. 16:10 supports 1280x800, 1920x1200
and 2560x1600 through its own display/camera preparation, with the original menus and HUD.
See [aspect ratio settings and 16:10 verification](docs/sacredfire/23-aspect-ratios.md).

### Corrected widescreen layouts (Windows)

The 21:9 and 32:9 layouts include menu/background/HUD asset fixes and centered-HUD shaders.
The initial integration is for the tested USA 1.21 game and official v0.5.3 Windows engine, offline.
See [setup, verification and limitations](docs/sacredfire/18-widescreen-layouts.md).
