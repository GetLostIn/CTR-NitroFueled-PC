# SacredFire status

Updated 2026-10-18 (v0.3 Windows: internal resolution, upscaling list with FSR 4, ultrawide in
Settings, frame generation).

## v0.3 Windows (branch v0.3)

- Internal resolution 480p-2160p via the game's own render scale (`render_res`,
  docs/sacredfire/13-resolution.md); PS4 Pro switch and 1080p Pro patch removed.
- One Upscaling list (`temporal` off/fsr1/fsr3/dlss/fsr4) with DLSS quality/output/model; the
  FSR 3 context is made on first FSR 3 use and retried after a failure.
- Frame generation (`framegen` off/fsr3): FSR 3.1.6 interpolation at flip time, generated frame
  first, paced (docs/sacredfire/16-framegen.md). Measured on an RTX 3070 at 144 Hz: FSR 4 +
  frame generation, FPS limit 72, Fifo: 72 game FPS, 144 shown.
- Aspect ratio 16:9/21:9/32:9 in Settings > Display (`aspect_ratio`, by tcosi),
  not the Patches tab; `run.py` calls `scripts/ultrawide_dispatch.py` natively with
  `patches.json` fallback. Outputs 2560x1080, 3440x1440, 5120x1440 added.
- Game languages of CUSA13795; PS4 PKG Tool dumps; fullscreen fix. See notas-v0.3.md.

## Optional platform integration

An opt-in [PSN backend interface](psn-host-backend.md) adds provider-owned identity,
platform requests and notifications without replacing the normal offline or
private-server paths. The standalone and native integration suites use fixtures;
they do not constitute live online acceptance.

## Milestones

| milestone | state |
|---|---|
| Loader reaches the eboot entry, libc.prx/Fios2/NpToolkit2 initialized | reached |
| First frame presented | reached: the license screen renders at 25-30 FPS, 51 draws/frame |
| Sound and input | reached: two audio ports on PipeWire, Steam Deck Controller through SDL3 |
| Saving | reached: user/savedata/1/CUSA13795/savedata{0,1}/SYS-DATA.sav written; user confirmed progress persists between launches (2026-10-09) |
| Main menu | reached (2026-10-09, run 12): after the license screens and Bink intro videos the game's own log shows menu/garage activity ("changeHeroAndOutfit", "Loading hero DriverNGin"), ~300 draws/frame at 30 FPS |
| One full race | reached (2026-10-09, run 14): arcade races play through with sound, the Steam Deck controller and saving (user report) |
| Adventure mode | plays after the sampler fix (user tried it; not played to the end) |
| Split screen | works: a second gamepad on port 2 joins (user-confirmed) |
| Game patches | 60 FPS and motion-blur-off switch from the menu; motion blur off confirmed working |

## How to run

```bash
bash run.sh
```
Game patches (60 FPS, no motion blur, Pro 1080p) are switched in the in-game menu (08-patches.md).
`BB_NEO=1 bash run.sh` runs the game in PS4 Pro mode (Neo: 7-CPU mode and the Pro's 5568 MiB direct
memory budget). run.sh checks the game (`scripts/game_check.py orig/CUSA13795` prints `ctr`), links
libSceNpToolkit2.prx, compiles the chosen game patches and starts out/sacredfire (the game executable, bbport's bb-probe renamed). By hand:

```bash
python3 scripts/prepare.py orig/CUSA13795 --out out
python3 scripts/link_modules.py orig/CUSA13795 --out out --modules libc.prx libSceFios2.prx libSceNpToolkit2.prx
python3 tools/gen_import_names.py --out out
python3 scripts/content_profile.py orig/CUSA13795 --out out
bash build.sh
out/sacredfire out/boot-linked.bin --import-names out/import_names.txt --content-profile out/content.bin \
    --app0 orig/CUSA13795 --user user --timeout 0
```

`out/sacredfire out/boot-linked.bin --list-imports --import-names out/import_names.txt` prints how every
import binds without running the game (GPU imports show as unresolved there: they bind at GPU init).

## Current blocker

None (no STOP). The game runs through license screens and intro videos. Next: reach the main
menu, then a race; implement imports as they are hit (libSceFont and the NpToolkit2 request paths
have not been reached yet).

## Memory (menus, run 12)

RSS about 1.1 GB, VRAM about 1 GB, Vulkan allocations 1.6 GB; the Steam Deck's 14 GiB are enough
with the desktop apps closed.

## Measured at the milestones (run 29, 2026-10-09; `free -g` total/used/free/shared/cache/available, game process RSS)

| milestone | time | free -g | RSS |
|---|---|---|---|
| (a) entry + libc init | 16:39:07 | 14/9/2/0/2/4 | 502,156 KiB |
| (b) first frames (24 FPS, 105 compiles) | 16:39:13 | 14/10/2/0/2/4 | 1,113,012 KiB |
| (c) driver select with the pad | 16:40:02 | 14/11/1/1/2/3 | 1,512,808 KiB |
| (d) race loading | 16:40:02 | 14/11/1/1/2/3 | 1,515,908 KiB |

## Known issues

- **Dotted lines in skies and light shafts** (fixed 2026-10-15, GitHub issues #16, #18; also
  seen in shadPS4): thin dotted bright lines along straight edges in the sky and in light shafts,
  on NVIDIA at any resolution. Frame analysis (sky images saved before and after each sky draw,
  the sky box's vertices and camera matrix recorded and projected): the lines lie exactly on the
  shared edges of the sky glow's triangles (a sky box, multiply-blended over the sky dome), and the
  line pixels match the glow applied twice. The triangles' corners are identical and none of them
  is clipped there; the double write comes from CTR's 2x MSAA: single-sample colour drawn over the
  2x depth buffer. Ruled out on the way: cube map seams, NVIDIA manual interpolation, the depth
  test, depth clipping, guard band clipping (resolution independent), host clipping of the sky box
  (a watertight clipping geometry shader changed nothing) and a sample-0 colour mask. Fix: the
  game's MSAA is a setting, off by default (bbport.ini `msaa`, launcher Settings > Display; needs a
  restart; BB_NO_MSAA overrides): host images, views and pipelines use one sample, the guest
  memory layout is unchanged and MSAA texture reads become plain 2D reads. User-confirmed (lines
  gone, no other differences on several tracks).
- **Adventure hub sky stripes** (fixed 2026-10-09, run 31): the frame analyzer showed the final
  colour-grading pass sampling a 1024x32 lookup table built from height-1 render targets that the
  game samples as 1D textures; the cache gave those 1D views a separate image filled from guest
  memory (never written by the GPU), so the LUT was garbage and gradients banded. Fix: 1D images
  are lowered to 2D with height 1 in the SPIR-V backend (image type, coordinates, offsets,
  derivatives, size queries), views and images (the 1D part of shadPS4 #5133/#5171, without the
  cube-map changes that crashed RADV here). User-confirmed.
- **Clobbered packet headers**: in CTR a single dword inside a submitted command buffer is
  sometimes overwritten with 1 (`Bad PM4 packet ... [00000001]` where a SET_CONTEXT_REG header
  was). Dropping the rest of the buffer froze the game on the fence behind it; the decoder now
  resumes at the next packet header. Who writes the 1 is still unknown (BB_WRITE_LOG=1 lists
  writes into the range).
  2026-10-11: the 1 also lands on register payloads. With upstream 0.5's background pipeline
  compilation active (branch backport-0.51), two of three cold starts ended in a GPU-thread
  segfault reading a shader at an address whose pgm_hi register was 1 (`0x110073e6200`), once
  after three clobbered headers in the same buffer; runs without it stayed clean. The feature is
  therefore opt-in (`BB_ASYNC_PIPELINES=1`), and the clobber is the next bug to chase
  (11-upstream-backports.md has the run table; GitHub issue #2 sums up the evidence and the
  candidate writers).
  2026-10-11, evening (branch issue-2, 12-clobbered-dword-handoff.md): always-on instrumentation
  gives three readings per clobber (guest at submit, copy at decode, guest at decode), a ring of
  the port's guest writes checked against the submitted buffers, and `BB_COPY_PROTECT=1`
  (read-only copies, a host writer faults). Found: the game keeps its fence labels inside its two
  alternating command buffers, so a clobber is a label write arriving after the game rebuilt the
  buffer. Nine instrumented runs (cold driver cache, race loads, 80 ms and 1.5 s forced label
  holds) stayed clean and no guest event wait timed out: the writer is still not caught; the next
  clobber in any session leaves the answer in the log (the hand-off note says what to look for).
  When it fires, the log says `SacredFire issue #2: ...` and asks for itself to be attached to
  the issue; the launcher's Log page shows a notice with an *Open issue #2* button.

- **Texture with no Vulkan format** (fixed 2026-10-11, branch backport-0.51): in some races the
  game binds a texture descriptor with data_format 6 and num_format 0 (10_11_11 Unorm), which the
  format table has no entry for; `SurfaceFormat` asserted and the game stopped with exit code 23
  (logs/run18.log on 2026-10-09, logs/hang-repro-1-assert.log). Such a descriptor is now bound as
  a null descriptor, as invalid ones are (vk_rasterizer.cpp `IsBindableFormat`).

- **Fur/hair characters** (fixed 2026-10-09, see 06-fur.md): previously missing (driver select, run 12): bodies drawn with the fur material
  are missing; rigid parts (eyes, teeth, hats, kart) draw. The only GPU errors in the run are
  "image view type Color1D is incompatible with image type Color2D" (4 images) and one
  geometry-shader ring stride warning. The shadPS4 #5133/#5171 backport crashed RADV at pipeline
  creation and was reverted (patch kept in the session notes). Diagnosis and next step in
  06-fur.md: the fur geometry shader exceeds maxGeometryInputComponents (64) because ring reads of
  the second input vertex are mapped to extra attribute locations. Frame analyzer:
  `BB_CAPTURE_TRIGGER=out/capture_trigger BB_CAPTURE_DIR=out/capture`, `touch out/capture_trigger`.

- The license screen's accept needed more than one attempt (reported by the user); the save that
  follows was written, so the input path works. Watch for a repeat.
- Memory: the Steam Deck has 14 GiB; close other applications before running.
- Upstream Python tests `test_run_settings.*` call run.sh, which builds: never run the full
  `unittest discover` while a build is in progress.

## Launcher and packaging (2026-10-09)

- `bash launcher/bb-launcher.sh` starts the GTK4 launcher (tested in the container after a full
  package upgrade repaired PyGObject); `python3 launcher/sacredfire_launcher.py --play` starts
  the game with the saved settings (tested: PS4 Pro mode on, Pro 1080p patch, two controller
  ports, 30 FPS).
- `packaging/appimage.sh` builds `dist/SacredFire-x86_64.AppImage` (841 MB) with `nix bundle`.
  Built and tested on 2026-10-09 in the archbuild container after installing single-user Nix
  (`pacman -S nix`, /nix owned by the user, nixos-unstable channel): the runtime and GPU library
  were rebuilt inside `nix-shell shell.nix` so their RUNPATHs name store paths, then bundled.
  `AppRun --play` from the extracted image boots the game with a fresh data directory
  (~/.local/share/sacredfire: generated files, saves, shader cache, bbport.ini). Two fixes came out
  of it: the package now carries tools/gen_import_names.py (run.sh needs it), and run.sh exports
  BB_GPU_USER_DIR and creates the user directory before the game starts (the GPU library builds
  its path table when it loads and fell back to ~/.local/share/shadPS4 on a first launch, which the
  per-launch shader cache clearing would have missed). Steps in packaging/appimage.sh's header.

## 2026-10-09, later: shader cache kept, executable renamed, game icon

- The shader/pipeline cache is kept across launches (two fixes in 06-fur.md: the geometry
  specialization comparison and the preload fetch shader). `BB_CLEAR_SHADER_CACHE=1` or the
  launcher's "Clear the shader cache at start" deletes it. User-confirmed: fur right on a fresh
  cache and on the next launch with the cache preloaded.
- The game executable is `out/sacredfire` (bbport's `bb-probe`; the packaged programs are
  `sacredfire`, `sacredfire-game`). `BB_PROBE` still names a wrapper around it.
- The window icon is the game's own `sce_sys/icon0.png`, read from the game folder at start
  (gpu/shim/window.cpp decodes the PNG with miniz; nothing from the dump enters the repository).
  launcher/install-desktop.sh and the launcher copy it into the user's icon theme as
  io.github.sacredfire.Launcher for the menu entry and the launcher window.
- AppImage rebuilt (2026-10-10) with the renamed executable, the icon and the cache fixes:
  `dist/SacredFire-x86_64.AppImage`, programs `sacredfire` (launcher) and `sacredfire-game`.
  Repository: https://github.com/T3CHNOLOG1C/SacredFire.

## Release preparation (2026-10-10)

- Draft release v0.1 on GitHub with `dist/SacredFire-x86_64.AppImage` (841 MB) attached;
  the repository stays private until the user publishes it. Tags 0.1 to 0.5-pre3 are bbport's,
  so SacredFire releases use a `v` prefix.
- `.github/workflows/tests.yml` runs the Python tests, shell syntax and compile checks on every
  push; `.github/ISSUE_TEMPLATE/` has the bug report form; `CONTRIBUTING.md` states the
  no-game-files rule and how changes are made.
- Game patches credited to illusion and TroyWarez (illusionyy/PS-Game-Patch) in the patch
  file, the settings table, the patch compiler, run.sh, the launcher, the README and here.
- A first start without network no longer aborts (import names fall back to the compiled table).

## AppImage rebuilt (2026-10-11)

- `backport-0.51` merged into main (fast-forward to 33847e1). `dist/SacredFire-x86_64.AppImage`
  rebuilt from it with the nix-shell build: 429 MB (was 841 MB) after the packaging cuts from
  bbport 0.51 (RADV and ANV only, no LLVM, stripped game binary, MangoHud 64-bit, GTK4 without
  GStreamer). Booted with `--appimage-extract-and-run --play` to the intro videos on the bundled
  RADV. Not tagged: the build reads v0.1-4-g33847e1; tag before publishing a release.

## v0.2.1 (2026-10-11, evening)

- Tag v0.2.1 at 5b0319d (issue-2 fast-forwarded into main): the issue #2 diagnostics and the
  "SacredFire issue #2" report request. `dist/SacredFire-x86_64.AppImage` rebuilt with the
  nix-shell build (429 MB, nixpkgs 26.11pre1087755.e7439b6b14ad), booted to the start screen with
  `--appimage-extract-and-run --play`, released on GitHub with the .sha256.

## Launcher and controllers (2026-10-11, branch backport-0.51)

- Launcher, log page: *Copy* (clipboard) and *Export log…* (a file dialog; `sacredfire-<date>.log`),
  from bbport PR 78, for bug reports.
- Launcher, settings page: *Steam → Add to Steam* writes a non-Steam shortcut named SacredFire into
  every Steam user's `userdata/<id>/config/shortcuts.vdf` (launcher/steam_shortcuts.py: the binary
  VDF read and written in place, Steam's CRC32 app id, an existing entry replaced). The shortcut
  runs the AppImage (or launcher/bb-launcher.sh from a checkout) with `--play`, so the game starts
  without the launcher window in the Steam Deck's game mode. Steam rewrites that file when it
  exits, so the button refuses while Steam runs. tests/test_steam_shortcuts.py.
- Controllers: the Steam Deck sometimes shows a controller connected after launch only to a fresh
  enumeration. The in-game menu (Insert / L3+R3, section *Other*) has *Rescan controllers*, which
  restarts SDL's gamepad subsystem on the next input sample. The runtime also rescans by itself
  every 5 s while port 1 has no pad at all or a port 2..4 with a controller bound to it lacks it
  (unbound ports are not looked for); after 6 misses in a row the bound controller counts as
  disconnected and the rescans stop until the button, a binding change or a connected pad.

## Before publication (2026-10-10, later)

- `gpu/shim/bbport_gnm_table.inc` and `tools/re/gnm_names.txt` deleted: 500 function offsets
  with prologue bytes from Bloodborne's executable, the only bytes from a commercial binary in
  the tree. `bbport_gnm_hooks.cpp` compiles without the table (`__has_include`; Count 0, the
  BB_GNM_* switches print that no table is compiled in). The table is regenerated locally by
  `tools/re/gen_gnm_table.py` and is git-ignored.
- The `bloodborne-support` tag was deleted locally and on GitHub; docs name the removal commit
  (9773394) instead. The commits before it remain in `main`'s history.
- Branding: the README headline, the desktop entry's Name, the launcher's title bar, the in-game
  menu title and the AppImage name (`SacredFire-x86_64.AppImage`) lead with SacredFire; the game's
  title appears only descriptively, with a trademark note under the README's disclaimer. No
  logos or key art are in the repository (the icon is read from the user's own game folder).
- Builds are stamped: `build.sh` writes `out/VERSION` (nearest `v*` tag or commit, `-dirty`),
  `run.sh` prints it, the launcher's title shows it, the package carries it as `VERSION`, and
  `packaging/appimage.sh` refuses an uncommitted or unbuilt tree and writes
  `dist/SacredFire-x86_64.AppImage.{sha256,build.txt}` (version, commit, nixpkgs). Release steps
  are in CONTRIBUTING.md.

## Older CPUs: MOVBE/BMI1/LZCNT (2026-10-15)

- Sandy Bridge and Ivy Bridge CPUs (issue #31: i5-3570, i7-3770) lack the Haswell instructions the
  image uses (MOVBE 391 sites, BMI1 3528, LZCNT 324) and the game stopped at the first one. The ISA
  rewrite (17-isa-rewrite.md, the design of gow3_pc's G3PROBE5) scans the linked image with iced-x86
  at launch and rewrites the sites at load: 4243 sites, 0 unsupported. The portable ZIP bundles
  iced-x86 (MIT) through `packaging/windows/make_portable.py`; source runs need `pip install
  iced-x86` in the Python that runs `run.py`.
- `--cpu-features` names the missing sets before the game starts; fatal sets (SSE4.2, POPCNT, AVX)
  still stop the launch. Tested on an i7-3770 / RX 580: the table applies with 0 skipped and the
  game boots to the start screen.
