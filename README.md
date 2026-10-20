<p align="center">
  <img src="docs/images/logo.png" alt="CTR Nitro-Fueled PC" width="360">
</p>

# CTR-NitroFueled-PC

**Crash Team Racing: Nitro-Fueled (PS4, CUSA13795 v1.21) running natively on Windows 10/11.**

This is a Windows fork of [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire), the native
Linux port of the game by T3CHNOLOG1C. The game's own x86-64 code runs directly on the CPU, a
small runtime replaces the PS4 system libraries, and a renderer derived from shadPS4 translates
the PS4 GPU work to Vulkan. This fork adds the Windows layer (loader, memory, TLS, exceptions,
files, timers) from Supermedo's Windows port of bbport, plus the fixes SacredFire needed on top
of it: see [packaging/windows/README.md](packaging/windows/README.md).

Platform integrations can use the optional [PSN backend interface](docs/sacredfire/psn-host-backend.md).
It preserves the existing offline and private-server behavior unless an external
backend is explicitly selected; backends build independently of the port.

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
the 60 FPS patch (the FPS limit keeps it at 60 by default; up to 240). Shaders seen for the first
time cause short stalls; they are cached and load at start from the next session on, or compile
in the background with the experimental switch.

| Release | What it brought |
|---|---|
| [v0.4](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.4) | Redesigned launcher with a *Controls* tab (live controller test and rumble, players 1-4, gamepad buttons remapped by pressing them, stick dead zone; the keyboard page moves there); *Achievements*: trophies kept when unlocked, a launcher-style notice in the game (picture, name, grade, what it was for) and a tab with all 48 (picture, name, what to do, progress and dates); the in-game menu restyled in tabs, in Spanish or English; game languages per edition; AMD FSR 4 (experimental) in the *Upscaling* list; *Frame generation* (AMD FSR 3, experimental): an interpolated frame between each pair of game frames; *Aspect ratio* 16:9, 21:9 UltraWide or 32:9 Super UltraWide with centered HUD (by tcosi, in Settings > Display, not Patches); the European edition CUSA14876 (by martijnhe), ultrawide included; the game's 2x MSAA as a setting, off by default (fixes dotted sky lines); CPUs without MOVBE/BMI1/LZCNT (by MetaHeart); damaged dumps reported (by Naif Alqurashi); rare libwinpthread crashes fixed; FSR 3 no longer turns off without Resizable BAR |
| [v0.3](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.3) | Internal resolution 480p to native 4K (the game's own render scale; replaces the PS4 Pro switch); one *Upscaling* list: AMD FSR 1, AMD FSR 3.1 or NVIDIA DLSS (temporal, experimental) with DLSS quality modes, output and model (Transformer/CNN); game languages of this build (English, Latin American Spanish, French, Brazilian Portuguese); PS4 PKG Tool dumps (Image0 + Sc0); fullscreen switch in game fixed |
| [v0.2.1](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.2.1) | The 1.21 update built for firmware 6.72+ is accepted (patches found by signature); keyboard page in the launcher to rebind keys |
| [v0.2.0](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.2.0) | Launcher (`CTR-NitroFueled-PC.exe`): game setup with hard-link merge, settings, patches, save backups and imports, optional DLC; bundled Python; the project's icon |
| [v0.1.1](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases/tag/v0.1.1) | First portable build: FPS limit up to 240, fullscreen switch, licenses in the package |

### Quick start (Windows)

**Easiest: the portable build.** Download `CTR-NitroFueled-PC-portable-windows.zip` from
[Releases](https://github.com/GetLostIn/CTR-NitroFueled-PC/releases), unzip it anywhere and open
**`CTR-NitroFueled-PC.exe`**, the launcher: choose your `CUSA13795` (base game) and
`CUSA13795-patch` (update 1.21) folders, press *Prepare the game* once (it merges them with hard
links, no extra disk space) and then *Play*. Python is bundled; you only need a Vulkan 1.3 GPU.

**Picture** (launcher: Settings > Display; in game: Insert):

- *Internal resolution*: the size the game draws its 3D scene at. 480p and 720p for modest PCs;
  1440p to 2160p output a 4K picture, which also smooths edges on a 1080p screen. Needs a restart
  (the in-game menu does it). Capped at 1440p while ultrawide is on.
- *Aspect ratio* (v0.4, by tcosi; in Settings, not Patches): one *Aspect / output* choice —
  16:9 with 1280x720, 1920x1080, 2560x1440 or 3840x2160; 21:9 UltraWide with
  2560x1080 or 3440x1440; 32:9 Super UltraWide with 5120x1440 — with centered HUD.
  Upscaling is off while ultrawide is on. The first launch prepares about 9 GB of patched
  archives (originals kept); launch once normally to the menu first for the shader cache.
- *Upscaling*, one at a time, switched live (off while ultrawide is on):
  - **AMD FSR 1** (default): a sharper spatial enlargement than the game's own stretch.
  - **AMD FSR 3** (any GPU), **AMD FSR 4** (experimental, the v07 INT8 model; needs INT8
    dot product support, checked at start; FSR 3 runs while the model loads) and **NVIDIA
    DLSS** (GeForce RTX; the files come in the package): temporal upscaling that
    rebuilds detail over several frames from the game's depth and motion, and works as
    anti-aliasing at the same resolution. Turn them off if you see trails.
- *Frame generation* (v0.4, experimental, live): AMD FSR 3.1.6 frame interpolation shows a
  generated frame halfway between each pair of game frames, about twice the images on screen; the
  game, its FPS limit and its timing do not change. Best on a 120 Hz+ screen with the game at half
  the refresh rate (e.g. FPS limit 72 at 144 Hz, with V-Sync/Fifo or Mailbox). Works with any
  upscaler (FSR 3/4 and DLSS also give it the game's motion). Fast HUD elements may shimmer.
  Details: [docs/sacredfire/16-framegen.md](docs/sacredfire/16-framegen.md).
- With DLSS: *DLSS quality* (Auto follows the internal resolution; DLAA, Quality, Balanced,
  Performance and Ultra Performance render at NVIDIA's share of the *DLSS output*, 1080p or 4K,
  after a restart) and *DLSS model* (Transformer, recommended, or the lighter CNN).

**Game language** (launcher: Settings > Console): the languages this build (CUSA13795, the
Americas) has: English, Latin American Spanish, French and Brazilian Portuguese. Spain's Spanish is
not in it. With the European edition (CUSA14876) the list shows the PAL release's languages instead
(English, Spain's Spanish, French, German, Italian, Arabic).

The launcher also has the game patches, FPS limit, display, monitor and controller settings, save
backups and imports (including decrypted PS4 saves), and optional DLC: add your own DLC packages
(`.pkg`) and the port reports them installed (CTR Nitro-Fueled's DLC only carry the license; the
content is in the game). Spanish and English.

### Which game dumps work

The launcher needs the game as **folders of decrypted files** from **CUSA13795** (the USA
edition) or, from v0.4, **CUSA14876** (the European edition, as in
[martijnhe/OCTR-NitroFueled-PC-EU](https://github.com/martijnhe/OCTR-NitroFueled-PC-EU)): the base
game (v01.00) and the 1.21 update. The folder names do not matter. When you
choose a folder, the launcher says in green or red what it found.

| Your dump | What it looks like | Works? |
|---|---|---|
| Dumped from your own PS4 with a game dumper | `eboot.bin`, `sce_module\`, `sce_sys\param.sfo`… directly in each folder (base, update) | Yes |
| Extracted with **PS4 PKG Tool** | `Image0\` (eboot.bin, sce_module, the game data) and `Sc0\` (param.sfo and the rest of sce_sys) | Yes, from v0.3: choose the folder that holds `Image0` and `Sc0` and press *Prepare the game*. With v0.2.1, first copy **everything** in `Image0` to that folder and everything in `Sc0` to a new `sce_sys` folder in it |
| Any other tool | Works if the files end up in one of the two layouts above | Yes |
| A `.pkg` file that was not extracted | | No: extract it first. Only the DLC tab takes `.pkg` files |
| The European edition (CUSA14876) | Same layouts | Yes, from v0.4, ultrawide included (tcosi's patches ported to its executable and archives) |
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

CPUs without MOVBE, BMI1 or LZCNT (Sandy Bridge, Ivy Bridge) also need `iced-x86` for the Python
that runs `run.py`, which the port uses to rewrite those instructions at start
([17-isa-rewrite.md](docs/sacredfire/17-isa-rewrite.md), issue #31). MSYS2's Python cannot install
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
- **tcosi**: the 21:9 UltraWide and 32:9 Super UltraWide aspect patches with centered HUD
  (`patches/CTR-UltraWide21x9-v1.0.xml`, `patches/CTR-SuperUltraWide32x9-v1.0.xml`,
  `scripts/ultrawide_*`), integrated in v0.4 under Settings > Display > Aspect ratio.
- **[IFreemz](https://github.com/IFreemz)**: [shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR),
  whose DLSS bridge (MIT, `dlss_bridge/`) and NGX loader this fork uses.
- **[FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)**: AMD FidelityFX FSR 3.1, FSR 3.1.6
  frame generation and the FSR 4 v07 provider on Vulkan (MIT).
- **NVIDIA DLSS**: `nvngx_dlss.dll` from the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS),
  redistributed under its license (in `licenses\` of the package). NVIDIA, the NVIDIA logo, DLSS
  and GeForce RTX are trademarks and/or registered trademarks of NVIDIA Corporation. The GPL
  program contains no NVIDIA code: it loads the separate bridge DLL when present.
- Windows port of SacredFire: [GetLostIn](https://github.com/GetLostIn), with Claude (Anthropic).

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
  `pip install iced-x86`: [17-isa-rewrite.md](docs/sacredfire/17-isa-rewrite.md)), glslang, SDL3,
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

**In-game menu** (Insert, or L3+R3 on the pad): game patches, controller ports for split
screen, FPS limit, fullscreen, FPS counter, menu language. It is saved to `bbport.ini`; patches apply after a restart.

**Controllers.** SDL3 gamepads. The launcher's *Controls* tab (v0.4) lists the connected ones
through the game's own SDL3 with a live test and rumble, assigns players 1–4 (`port.N=<GUID>` in
`bbport.ini`, as the in-game menu does; player 1 also `BB_GAMEPAD=<GUID or part of the name>`),
rebinds the gamepad buttons by pressing them (`pad.<input>=` lines) and sets the sticks' dead
zone (`BB_PAD_DEADZONE`, `BB_PAD_DEADZONE_OUTER`). Player 1 on automatic switches to whichever
pad was used last. Ports 2–4 log a user in and out as their pad comes and goes (split screen).
The keyboard works next to a gamepad: WASD left stick, arrow keys right stick, Space Cross,
LShift Circle, E Square, Q Triangle, 1/3 L1/R1, R/F L2/R2, Z/C L3/R3, Enter Options, IJKL
d-pad; *Controls › Keyboard* rebinds them (`key.<input>=` lines in `bbport.ini`).

**Achievements** (v0.4). The game's trophies unlock as on a PS4: each one is kept in
`user\trophies\<TITLE_ID>\unlocked.txt` with its date, shows a notice over the game, and the
platinum comes once all the others are. The launcher's *Achievements* tab shows them with their
pictures (from `sce_sys\trophy\trophy00.trp`), names, what to do, grades, progress, dates and a
reset (with a backup). The names and descriptions are built in (English and Spanish); the game's
own texts are encrypted with the PS4's trophy key, which this port does not include: with your own
key (the same shadPS4 uses; paste it, or import it from shadPS4's `keys.json`) they are decrypted
as shadPS4 does, in the game's language, and kept in `trophies.tsv`.

**PS4 Pro mode** (`BB_NEO=1`, the launcher's switch): the game sees a PS4 Pro (7-CPU mode,
5568 MiB of direct memory) and takes its Pro rendering path; with the 1080p patch it renders
at 1920×1080. Off by default: the Steam Deck is closer to a base PS4.

**Mods:** loose-file mod folders (with the game's folders inside) are overlaid on the game
folder at start, with enable switches and load order in `mods.json`
(`scripts/mods.py`, [docs/MODS.md](docs/MODS.md), written for bbport). Further shadPS4-format
patch files go into `patches/` or `BB_PATCHES_DIR`, selected through `patches.json`.

## AppImage (Steam Deck)

`packaging/appimage.sh` builds `dist/SacredFire-x86_64.AppImage` (about 430 MB) with Nix: the
binaries are built in `nix-shell shell.nix` so the bundle can collect their libraries, then
`nix bundle` packs them with the GTK launcher, Python and Mesa's Vulkan drivers (the header of
the script has the exact steps). Running it opens the launcher; `--play` starts the game. Data
lives in `~/.local/share/sacredfire`, launcher settings in `~/.config/sacredfire-launcher`.
Without a game folder chosen, the launcher shows the Game row as *not chosen* with Play disabled
until a folder passes the game check.

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
- Online features are stubbed; the game runs offline.
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
