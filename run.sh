#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# BB_PREBUILT=1 (packaged builds, the AppImage): out/sacredfire (the game executable) and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# BB_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default this directory.
data=${BB_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export BB_CONFIG=${BB_CONFIG:-$data/bbport.ini}
if [[ -z ${BB_PREBUILT:-} && -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
# BB_SAVE_LOG=1 (launcher: "Save the log and statistics to a file"): this run's output and its
# per-frame statistics also go to $data/logs/<time>.log, .frames.csv and .readbacks.csv.
if [[ ${BB_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    mkdir -p "$logs"
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
    export BB_FRAME_STATS=1 BB_FRAME_LOG=${BB_FRAME_LOG:-$logs/$stamp.frames.csv}
    export BB_READBACK_LOG=${BB_READBACK_LOG:-$logs/$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    exec 3>"$logs/$stamp.log"
    # Bash builtins only (the AppImage's PATH has no tee).
    exec > >(while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; printf '%s\n' "$line" >&3; done) 2>&1
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# Configure the private online server before patches.py selects the game's key patch.
online_env=$(mktemp "$out/online-env.XXXXXX")
if ! "$PYTHON" scripts/online_config.py --config "$BB_CONFIG" --data "$data" --env-file "$online_env"; then
    rm -f "$online_env"
    exit 1
fi
while IFS= read -r -d '' setting; do export "$setting"; done < "$online_env"
rm -f "$online_env"
if [[ ${BB_ONLINE:-0} == 1 ]]; then
    export BB_SESSIONS_DIR=${BB_SESSIONS_DIR:-$data/sessions}
    mkdir -p "$BB_SESSIONS_DIR"
fi
# BB_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default: orig/CUSA13795, SacredFire's
# read-only link tree of the merged CTR dump (update 1.21 over the base game).
game=${BB_GAME_DIR:-orig/CUSA13795}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2; exit 1; fi
# The game check (scripts/game_check.py) compares the executable image with the supported one
# (Crash Team Racing Nitro-Fueled CUSA13795 1.21) and names the profile the runtime reads.
profile=$("$PYTHON" scripts/game_check.py "$game") || { echo "$profile" >&2; exit 2; }
export BB_GAME_PROFILE=${profile:-ctr}
# The edition's title ID and version (the EU edition has its own patches); the USA 1.21 update when
# param.sfo cannot be read, as run.py.
read -r title_id app_version < <("$PYTHON" scripts/game_check.py --info "$game") || true
title_id=${title_id:-CUSA13795} app_version=${app_version:-01.21}
echo "Game profile: $BB_GAME_PROFILE ($title_id $app_version)"
# The game executable (built here in a checkout): mod packages ask it what it supports.
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/sacredfire}
    version=$(cat VERSION 2>/dev/null || true)
else
    bash build.sh
    probe=${BB_PROBE:-out/sacredfire}  # BB_PROBE: a wrapper (gdb) around it
    version=$(cat out/VERSION 2>/dev/null || true)
fi
# Mod packages (the launcher's Mods page: .pak, .zip and folders in the data folder's mods/,
# switched in patches.json), as run.py: scripts/mod_packages.py prepares a separate mod session
# (its own bbport.ini, saves and patches, the mods' files redirected by the engine, never a game
# copy) and its saves go back to the mod profile after a normal exit. Its library's patch file
# goes to the data folder's patches/ (the package's is read-only). An in-game restart runs this
# script again inside the game's process with the session's environment: it keeps the session,
# and this first run finishes it.
export BB_MOD_LIBRARY_DIR=${BB_MOD_LIBRARY_DIR:-$data/patches}
mod_session_owner=0
if [[ -z ${BB_MOD_SESSION:-} ]]; then
    mod_env=$(mktemp "$out/mod-env.XXXXXX")
    if ! "$PYTHON" scripts/mod_launch.py prepare --data "$data" --game "$game" --config "$BB_CONFIG" --env-file "$mod_env"; then
        rm -f "$mod_env"
        exit 1
    fi
    while IFS= read -r -d '' setting; do
        case $setting in
            @game=*) game=${setting#@game=} ;;
            -*) unset "${setting#-}" ;;
            *) export "$setting" ;;
        esac
    done < "$mod_env"
    rm -f "$mod_env"
    if [[ -n ${BB_MOD_SESSION:-} ]]; then mod_session_owner=1; fi
fi
original_game=$game
# The Options menu's Graphics tab and the main menu's Quit Game (scripts/graphics_tab.py, as in
# run.py): an extra archive the runtime serves as archives/update3.pak. USA/EU 1.21.
unset BB_GRAPHICS_TAB_ARCHIVE
if [[ ( $title_id == CUSA13795 || $title_id == CUSA14876 ) && $app_version == 01.21 ]]; then
    while IFS= read -r line; do
        case $line in
            archives/update3.pak=*) export BB_GRAPHICS_TAB_ARCHIVE=${line#archives/update3.pak=} ;;
            ?*) echo "$line" ;;
        esac
    done < <("$PYTHON" scripts/graphics_tab.py "$game" --out "$out" --settings "$BB_CONFIG" ||
             echo "Graphics tab: off (scripts/graphics_tab.py stopped)")
fi
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${BB_MODS_DIR:-$data/mods}" --config "${BB_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${BB_MODS_ENABLED:-1}")
# A private merged view lasts for this launch, including restarts. Cleanup only our own view.
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    trap '"$PYTHON" -c '\''import shutil,sys; shutil.rmtree(sys.argv[1])'\'' "$mod_game"' EXIT
fi
"$PYTHON" scripts/prepare.py "$game" --out "$out"
"$PYTHON" scripts/link_libc.py "$game" --out "$out"
"$PYTHON" scripts/link_modules.py "$game" --out "$out" --modules libc.prx libSceFios2.prx libSceNpToolkit2.prx
# SacredFire: symbol names for this eboot's host-side imports (shadPS4's aerolib list, fetched once).
"$PYTHON" tools/gen_import_names.py --out "$out"
# Optional DLC from the writable data directory (also used by the launcher).
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${BB_CONTENT_SKU:-full}" \
    --dlc-dir "${BB_DLC_DIR:-$data/dlc}"
# The shader recipe (shaders/recipe.zip): prepared for this GPU at the first start, from the game's
# own shaders, so new tracks and racers do not stop the game to compile them (as run.py).
# BB_SHADER_RECIPE=0 leaves it out; another path uses that recipe.
recipe=${BB_SHADER_RECIPE:-shaders/recipe.zip}
if [[ ${BB_SHADER_RECIPE:-} != 0 && -f $recipe ]]; then
    "$PYTHON" scripts/gcn_shaders.py "$game" --out "$out"
    BB_SHADER_RECIPE=$(realpath "$recipe")
    BB_GCN_SHADERS=$(realpath "$out")/gcn_shaders.bin
    export BB_SHADER_RECIPE BB_GCN_SHADERS
else
    unset BB_SHADER_RECIPE BB_GCN_SHADERS
fi
# Every race track in the online playlist, at the same weight, always (as run.py): the game reads
# it through an archive patch. When it cannot be built, the game keeps its own.
unset BB_ARCHIVE_PATCHES BB_PRIVATE_ARCHIVE_PATCHES
"$PYTHON" scripts/online_playlist.py "$game" --out "$out"
base_patches=()
if [[ -f $out/online_playlist.bbpatch ]]; then base_patches=(--base "$out/online_playlist.bbpatch"); fi
# Xbox button prompts (bbport.ini button_prompts, scripts/button_prompts.py), as run.py: the fonts'
# button image through archive patches, each keeping the patch before it for the same archive.
while IFS= read -r patch; do
    if [[ -n $patch ]]; then base_patches+=(--base "$patch"); fi
done < <("$PYTHON" scripts/button_prompts.py "$game" --out "$out" --settings "$BB_CONFIG" "${base_patches[@]}")
# The aspect ratio (bbport.ini aspect_ratio, scripts/aspect_ratio.py), as run.py: the
# render path and the patches read its settings, and it adds its archive patches (the playlist's
# kept in them), executable patch and HUD scale.
settings=$BB_CONFIG layout_patches=()
while IFS='=' read -r key value; do
    case $key in
        settings) settings=$value ;;
        patches_dir) if [[ -n $value ]]; then layout_patches=(--patches-dir "$value"); fi ;;
        archive_patches) if [[ -n $value ]]; then export BB_ARCHIVE_PATCHES=$value; fi ;;
        env.*) export "${key#env.}=$value" ;;
    esac
done < <("$PYTHON" scripts/aspect_ratio.py "$game" --out "$out" --settings "$BB_CONFIG" \
             --title-id "$title_id" --app-version "$app_version" "${base_patches[@]}")
# A mod session (above): the packages' archive entries over those archive patches, and their
# executable patches checked against each other and the layout, as run.py.
strict_overlap=()
if [[ -n ${BB_MOD_SESSION:-} ]]; then
    if [[ -n ${BB_MOD_ARCHIVE_PLAN:-} ]]; then
        archives=$("$PYTHON" scripts/mod_launch.py overlays --game "$game" --out "$out") || exit 1
        if [[ -n $archives ]]; then export BB_ARCHIVE_PATCHES=$archives; else unset BB_ARCHIVE_PATCHES; fi
    fi
    "$PYTHON" scripts/mod_launch.py preflight --game "$game" --config "$BB_CONFIG" --settings "$settings" \
        --layout-patches "${layout_patches[1]:-}" --title-id "$title_id" --app-version "$app_version" || exit 1
    strict_overlap=(--strict-overlap)
fi
unset BB_ONLINE_MODE_HOOK BB_SPEED_MODE
if [[ ${BB_ONLINE:-0} == 1 || ${BB_SPEED_SOLO_TEST:-0} == 1 ]]; then
    if [[ ( $title_id == CUSA13795 || $title_id == CUSA14876 ) && $app_version == 01.21 ]]; then
        speed_bases=()
        IFS='|' read -r -a speed_existing <<< "${BB_ARCHIVE_PATCHES:-}"
        for speed_patch in "${speed_existing[@]}"; do
            if [[ -n $speed_patch ]]; then speed_bases+=(--base "$speed_patch"); fi
        done
        if [[ ${BB_ONLINE:-0} == 1 ]]; then
            mode_result=$("$PYTHON" scripts/online_mode.py "$game" --out "$out/online-mode" \
                --elf "$out/eboot.elf" "${speed_bases[@]}") || exit 1
            while IFS='=' read -r key value; do
                case $key in
                    archive_patches) IFS='|' read -r -a speed_existing <<< "$value" ;;
                    online_mode_hook) export BB_ONLINE_MODE_HOOK=$value ;;
                    cup_file_map) export BB_MOD_FILE_MAP=$value BB_MOD_SOURCE_GAME=${BB_MOD_SOURCE_GAME:-$game} ;;
                    known_heroes) export BB_KNOWN_HEROES=$value ;;
                esac
            done <<< "$mode_result"
            speed_bases=()
            for speed_patch in "${speed_existing[@]}"; do speed_bases+=(--base "$speed_patch"); done
        fi
        # a damaged game file stops Speed Mode only (speed_mode.py says why): online plays as before
        if ! speed_result=$("$PYTHON" scripts/speed_mode.py "$game" --out "$out/speed-mode" "${speed_bases[@]}"); then
            speed_joined=$(IFS='|'; echo "${speed_existing[*]}")
            speed_result="archive_patches=$speed_joined"
        fi
        while IFS='=' read -r key value; do
            case $key in
                archive_patches) export BB_ARCHIVE_PATCHES=$value ;;
                private_archive_patches) export BB_PRIVATE_ARCHIVE_PATCHES=$value ;;
                'ITEMLESS (Experimental): private overlays prepared for '* ) echo "$key" ;;
            esac
        done <<< "$speed_result"
    fi
fi
# The game runs at its own 30 FPS against a 60 Hz vblank unless the 60 FPS patch is on. Patches
# come from patches/CrashTeamRacing.xml (community patches by illusion and TroyWarez, from
# https://github.com/illusionyy/PS-Game-Patch), chosen in the in-game menu (bbport.ini patch_*
# keys) or patches.json; BB_PATCHES_DIR replaces the bundled folder. The player's own XML files go
# in the data folder's patches/ (the launcher's Patches page; the package's folder is read-only).
fps=${BB_FPS:-30}
"$PYTHON" scripts/patches.py --patches-dir "${BB_PATCHES_DIR:-patches}" --patches-dir "$data/patches" \
    "${layout_patches[@]}" "${strict_overlap[@]}" --patches-config "${BB_PATCHES_CONFIG:-$data/patches.json}" \
    --app-version "$app_version" --title-id "$title_id" --out "$out" --settings "$settings"
patch_args=(--patches "$out/patches.bin")
# bbport.ini render_res / ultrawide (scripts/patches.py render_plan), as run.py: the PS4 Pro path
# above 1080p, the base path for ultrawide; native 4K's larger GPU heap needs more direct memory.
read -r plan_pro plan_heap plan_label < <("$PYTHON" scripts/patches.py --print-plan --settings "$settings") || true
if [[ -n ${plan_pro:-} ]]; then
    export BB_NEO=$plan_pro
    if [[ $plan_heap == 1 && ${BB_DMEM_MB:-0} -lt 7168 ]]; then export BB_DMEM_MB=7168; fi
    echo "Internal resolution: $plan_label"
fi
# Ultrawide: videos and cutscenes at 16:9 with black bars (src/runtime_cinematic.c) unless
# bbport.ini has ultrawide_cinematics=0.
unset BB_CINEMATIC_16X9 BB_CUTSCENE
if [[ ${plan_label:-} == ultrawide* ]] && ! grep -qx 'ultrawide_cinematics=0' "$settings" 2>/dev/null; then
    export BB_CINEMATIC_16X9=1
    BB_CUTSCENE=$("$PYTHON" scripts/patches.py --print-cutscene --out "$out" || true)
    export BB_CUTSCENE
fi
# Ultrawide: the title and customization screens with a 16:9 field of view, so their backdrop
# fills the width (src/runtime_frontend.c), unless bbport.ini has ultrawide_frontend=0.
unset BB_FRONTEND_FOV
if [[ ${plan_label:-} == ultrawide* ]] && ! grep -qx 'ultrawide_frontend=0' "$settings" 2>/dev/null; then
    BB_FRONTEND_FOV=$("$PYTHON" scripts/patches.py --print-frontend --out "$out" || true)
    export BB_FRONTEND_FOV
fi
# BB_NEO=1: PS4 Pro mode (sceKernelIsNeoMode, 7-CPU mode, 5568 MiB direct memory); the game then
# picks its Pro rendering path. Off by default: the Steam Deck is closer to a base PS4.
export BB_NEO=${BB_NEO:-0}
echo "PS4 Pro mode: ${BB_NEO} (BB_NEO=1 turns it on)"
# The on-disk shader/pipeline cache (user/cache/CUSA13795) is kept across launches: the GPU
# library preloads every pipeline from earlier sessions at start, so areas seen before no longer
# compile shaders mid-race. BB_CLEAR_SHADER_CACHE=1 (launcher: "Clear the shader cache at start")
# deletes it first; the fur breakage that once forced a clear per launch is fixed
# (docs/sacredfire/06-fur.md: geometry specialization and preload fetch shader).
if [[ ${BB_CLEAR_SHADER_CACHE:-0} == 1 ]]; then
    rm -rf "${BB_USER_DIR:-$data/user}/cache/$title_id"
    echo "Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)"
fi
# With the 60 FPS patch the game's own limiter paces frames; a 60 Hz virtual vblank would
# quantize a slightly late frame to 30 FPS (measured: 40-44 FPS alternating 16.7/33 ms).
# A 480 Hz vblank lets frames flip as they come (menus 60 FPS instead of 50).
if [[ -f $BB_CONFIG ]] && grep -qx 'patch_60fps=1' "$BB_CONFIG"; then
    export BB_VBLANK_HZ=${BB_VBLANK_HZ:-480}
    echo "60 FPS patch on: virtual vblank ${BB_VBLANK_HZ} Hz"
fi
# Disable Motion Blur: the eboot patch stops the camera's blur; the blur shaders go too (objects
# moving on their own: the crowd, plants; bb_motion_blur.h), as run.py.
if [[ -f $BB_CONFIG ]] && grep -qx 'patch_no_motion_blur=1' "$BB_CONFIG"; then
    export BB_NO_MOTION_BLUR=1
else
    export BB_NO_MOTION_BLUR=0
fi
# Background upload of the game's GPU memory into VRAM ahead of use (BufferCache::Preupload):
# 1 = only memory already in VRAM that the game rewrote (no new VRAM), 2 = all of it (~3 GB more
# VRAM), 0 = off. BB_GUEST_GPU_MEMORY=1 puts guest direct memory in GPU-visible dma-buf chunks
# (BB_GUEST_IN_PLACE below implies it: it commits all allocated memory up front and rules out BB_UFFD).
export BB_PREUPLOAD=${BB_PREUPLOAD:-1}
# Memory model and translation. BB_PC_MODEL=1 (the launcher's "New memory and translation model",
# experimental, off by default) selects how a PC release would work: the GPU uses the game's
# memory where it is (GPU-visible system memory) and keeps the data it reads often in VRAM, the
# command processor's work is translated rather than emulated (BB_GUEST_IN_PLACE=1 and what
# depends on it). AMD GPUs only for now: on others the GPU library keeps it off
# (BB_PC_MODEL_ANY_GPU=1: try it anyway; NVIDIA, whose dma-buf maps at offset 0 only, gets one
# chunk per direct memory allocation). 0 (default): the model of 0.3 (VRAM copies of the game's
# memory, write tracking), with the fixes made since. BB_GUEST_IN_PLACE set by hand overrides it.
# BB_AS_0_3=1 (the launcher's developer switch "Synchronisation as in 0.3"): what changed since the
# 0.3 release is reverted for comparisons: the 0.3 memory model, WRITE_DATA/DMA waiting for every
# host copy, the scheduler's concurrent recording check.
if [[ ${BB_AS_0_3:-0} == 1 ]]; then
    export BB_GUEST_IN_PLACE=0 BB_HOST_COPY_WAITS=all BB_PRODUCER_CHECK=1
fi
export BB_GUEST_IN_PLACE=${BB_GUEST_IN_PLACE:-${BB_PC_MODEL:-0}}
# The game's GPU (vendor ID and name) for the driver workarounds below (bb-gpu-capabilities
# --device, bbport 0.5).
if [[ -n ${BB_PROBE:-} ]]; then caps=$(dirname -- "$BB_PROBE")/bb-gpu-capabilities
elif [[ -n ${BB_PREBUILT:-} ]]; then caps=bin/bb-gpu-capabilities
else caps=out/bb-gpu-capabilities; fi
gpu_vendor= gpu_name=
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && -x $caps ]]; then
    IFS=$'\t' read -r gpu_vendor gpu_name < <("$caps" --device 2> /dev/null) || true
fi
# GCN 3-5 under RADV (Polaris, Vega and their APUs): with HiZ, depth compression showed black
# halos and missing geometry (bbport issues #54, #99); RADV_DEBUG=nohiz fixed it at some cost. Set
# unless RADV_DEBUG is the user's, BB_AUTO_NOHIZ=0: off.
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && $gpu_name == *"(RADV "* ]]; then
    for chip in TONGA ICELAND CARRIZO FIJI STONEY POLARIS VEGA RAVEN RENOIR; do
        if [[ $gpu_name == *"(RADV $chip"* ]]; then
            export RADV_DEBUG=nohiz
            echo "RADV: $gpu_name: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; BB_AUTO_NOHIZ=0: off)"
            break
        fi
    done
fi
# The drivers' own shader caches keep compiled pipelines between sessions (the port's cache gives
# them the shaders again at startup): NVIDIA's without its cleanup and with room for the game's
# pipelines (as Steam sets it), Mesa's with more room than its default (bbport 0.5).
export __GL_SHADER_DISK_CACHE=${__GL_SHADER_DISK_CACHE:-1}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
# MangoHud (launcher switch: MANGOHUD=1) must be drawn once. Two overlays on top of each other
# showed doubled, offset text: the Steam Deck's performance overlay (mangoapp, game mode) plus the
# in-game layer, or the AppImage's bundled layer plus a system MangoHud (the layer names differ,
# so the Vulkan loader loads both).
if [[ ${MANGOHUD:-0} == 1 ]]; then
    if pgrep -x mangoapp > /dev/null 2>&1; then
        echo "MangoHud: Steam's performance overlay is on; the in-game MangoHud stays off"
        unset MANGOHUD
        export DISABLE_MANGOHUD=1
    elif grep -qs '"VK_LAYER_MANGOHUD' /usr/share/vulkan/implicit_layer.d/*.json \
            /etc/vulkan/implicit_layer.d/*.json \
            "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json; then
        # The system's own MangoHud (and its config) is used; the bundled one is skipped.
        export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:+$VK_LOADER_LAYERS_DISABLE,}VK_LAYER_MANGOHUD_overlay_64_x86_64
    fi
fi
# Write tracking with userfaultfd write-protection instead of mprotect (no address-space write lock:
# a streaming burst re-protected thousands of pages, ~10-20 ms); falls back to mprotect when the
# kernel lacks it. Off by default for now: the Steam Deck crashed 4 times in 9 minutes with it
# (2026-10-04, PM4 type 0 = zeroed command buffers, heap corruption); 1 turns it on.
export BB_UFFD=${BB_UFFD:-0}
# The command buffers of each submission are copied when the game submits them and decoded from
# the copy: while an area loads the game reused that memory before the GPU thread got to it
# ("Unimplemented PM4 type 0" a few seconds after loading a save). 0 decodes guest memory.
export BB_COPY_GPU_BUFFERS=${BB_COPY_GPU_BUFFERS:-1}
# A guest write next to small GPU outputs the GPU is still writing (counters, compute results,
# up to BB_GPU_WRITE_TWINS_MAX bytes per page) does not wait for the GPU (BufferCache twins).
export BB_GPU_WRITE_TWINS=${BB_GPU_WRITE_TWINS:-1} BB_GPU_WRITE_TWINS_MAX=${BB_GPU_WRITE_TWINS_MAX:-65536}
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=480 ;; 90) export BB_VBLANK_HZ=90 ;; *) export BB_VBLANK_HZ=60 ;; esac
fi
echo "SacredFire ${version:-unknown version}"
# The graphics engine (bbport.ini renderer), as run.py: the native Vulkan renderer is experimental;
# its shaders are built from this game the first time (scripts/native_shaders.py, a few minutes;
# "@progress" lines for the launcher's window). Anything that stops it leaves shadPS4's Vulkan.
if [[ -z ${BB_NATIVE_VK:-} && -f $BB_CONFIG ]] && grep -qx 'renderer=native' "$BB_CONFIG"; then
    native_env=$(mktemp "$out/native-env.XXXXXX")
    temporal=$(sed -n 's/^temporal=//p' "$BB_CONFIG" | tail -n 1)
    "$PYTHON" scripts/native_shaders.py --data "$data" --game "$game" --title-id "$title_id" \
        --app-version "$app_version" --temporal "$temporal" --env-file "$native_env" || true
    while IFS= read -r -d '' setting; do export "$setting"; done < "$native_env"
    rm -f "$native_env"
fi
# The user directory (saves, shader cache) must exist and be named to the GPU library before the
# game starts: its path table is built when the library loads (before the port hands it the
# directory), and a missing directory makes it fall back to ~/.local/share/shadPS4, which the
# cache clearing above would miss (seen on the first launch from a fresh data directory).
export BB_GPU_USER_DIR=${BB_USER_DIR:-$data/user}
mkdir -p "$BB_GPU_USER_DIR"
probe_args=("$out/boot-linked.bin" --import-names "$out/import_names.txt" --content-profile "$out/content.bin" "${patch_args[@]}" --app0 "$game" --user "${BB_USER_DIR:-$data/user}" --timeout "${BB_TIMEOUT:-0}" "$@")
if [[ -n ${mod_game:-} || $mod_session_owner == 1 ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    # An interrupted wait must finish the child before removing its mounted view.
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    if [[ $mod_session_owner == 1 ]]; then
        "$PYTHON" scripts/mod_launch.py finish --session "$BB_MOD_SESSION" --state "$BB_MOD_STATE" --code "$mod_status" ||
            echo "Mods: the session's saves could not be copied back; they are kept in $BB_MOD_SESSION" >&2
    fi
    exit "$mod_status"
fi
exec "$probe" "${probe_args[@]}"
