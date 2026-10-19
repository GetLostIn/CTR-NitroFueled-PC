#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SacredFire on Windows: the counterpart of run.sh. Prepares the game image and starts
sacredfire.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_USER_DIR, BB_CONFIG, BB_NEO,
BB_FPS, BB_MODS_*, BB_PATCHES_*, BB_CLEAR_SHADER_CACHE, BB_SAVE_LOG, ...). Extra arguments go to
sacredfire.exe. The in-game restart ends the game with RESTART_CODE and this script starts it
again (BB_RESTART_EXIT_CODE); `--after PID` (the older BB_RESTART_COMMAND way) waits for the
previous process to end first.
Works from the MSYS2 CLANG64 Python or a python.org Python.
"""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

PORT = Path(__file__).resolve().parent
TITLE_ID = 'CUSA13795'
APP_VERSION = '01.21'
RESTART_CODE = 75  # the game asks to be started again (in-game "Apply and restart")
LOG = None  # last_run.log (or BB_SAVE_LOG's file): everything this launch prints


def fail(message, code=1):
    print(message, file=sys.stderr)
    if LOG:
        LOG.write(message + '\n')
        LOG.flush()
    sys.exit(code)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console: console programs must not open their own window."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `sacredfire.exe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """BB_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('BB_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()
    if LOG:
        LOG.write(text)
        LOG.flush()


def say(message):
    emit(message + '\n')


def run_python(script, *args, capture=False, check=True):
    # -X utf8: the embeddable Python of the portable package ignores PYTHONUTF8, and the scripts
    # print the game's title (with its trademark sign) to a pipe.
    command = [sys.executable, '-X', 'utf8', str(PORT / script), *map(str, args)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=None if capture else subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', creationflags=no_console())
    if not capture and result.stdout:
        emit(result.stdout)
    if check and result.returncode:
        sys.exit(result.returncode)
    return result if capture else None


def find_executable(name):
    """bin/ (a packaged build), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def ini_has(path, line):
    try:
        return any(l.strip() == line for l in Path(path).read_text(encoding='utf-8', errors='replace').splitlines())
    except OSError:
        return False


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            name, sep, value = line.partition('=')
            if sep and not line.lstrip().startswith('#') and name.strip() == key:
                return value.strip()
    except OSError:
        pass
    return None


# bbport.ini render_res (or a DLSS quality with temporal=dlss) picks the game's rendering path
# (scripts/patches.py render_plan, docs/sacredfire/13-resolution.md): above 1080 and below it the
# PS4 Pro path; the larger GPU heap of native 4K also takes more than the Pro's 5568 MiB of direct
# memory.
def apply_render_res(env, config):
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from patches import read_settings, render_plan
    finally:
        sys.path.pop(0)
    plan = render_plan(read_settings(Path(config)))
    if plan is None:
        return
    env['BB_NEO'] = '1' if plan['pro'] else '0'
    if plan['heap'] and int(env.get('BB_DMEM_MB') or 0) < 7168:
        env['BB_DMEM_MB'] = '7168'
    path = 'PS4 Pro' if plan['pro'] else 'base PS4'
    say(f'Internal resolution: {plan["label"]} ({path} rendering path)')


def uw_disabled(game, config):
    """Fallback when the ultrawide helpers are absent: 16:9, nothing to do."""
    from types import SimpleNamespace
    return SimpleNamespace(enabled=False, game=Path(game), config=str(config),
                           render_config=str(config))


def uw_aspect(config):
    """bbport.ini aspect_ratio ('16:9' when unset)."""
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.partition('=')
            if sep and key.strip() == 'aspect_ratio':
                return value.strip()
    except OSError:
        pass
    return '16:9'


def uw_prepare_native(port, data, env, game, config):
    """v0.3 native ultrawide dispatch (Settings > Display > Aspect ratio).

    bbport.ini aspect_ratio=16:9|21:9|32:9 drives scripts/ultrawide_dispatch.py;
    a legacy patches.json toggle still applies when the aspect is 16:9/unset.
    Missing helpers mean 16:9 (never a fatal error for normal launches).
    """
    sys.path.insert(0, str(Path(port) / 'scripts'))
    try:
        from ultrawide_dispatch import prepare as dispatch_prepare
    except Exception as error:
        print(f'Ultrawide: helpers not available ({error}); running 16:9', flush=True)
        return uw_disabled(game, config)
    try:
        return dispatch_prepare(Path(port), Path(data), env, Path(game), str(config))
    finally:
        try:
            sys.path.remove(str(Path(port) / 'scripts'))
        except ValueError:
            pass


def uw_refresh_native(mode, env, config):
    try:
        sys.path.insert(0, str(PORT / 'scripts'))
        from ultrawide_dispatch import refresh as dispatch_refresh
        return dispatch_refresh(mode, env, config)
    except Exception as error:
        if getattr(mode, 'enabled', False):
            raise
        print(f'Ultrawide refresh skipped: {error}', flush=True)
    finally:
        try:
            sys.path.remove(str(PORT / 'scripts'))
        except ValueError:
            pass


def uw_shaders_native(mode, user_dir, out):
    try:
        sys.path.insert(0, str(PORT / 'scripts'))
        from ultrawide_dispatch import shaders as dispatch_shaders
        return dispatch_shaders(mode, user_dir, out)
    except Exception as error:
        if getattr(mode, 'enabled', False):
            raise
        print(f'Ultrawide shaders skipped: {error}', flush=True)
    finally:
        try:
            sys.path.remove(str(PORT / 'scripts'))
        except ValueError:
            pass


# Short names match the v1.0 external installer hook so its rewrite() can still
# strip and re-apply cleanly on a v0.3 tree (backward compatibility).
uw_prepare = uw_prepare_native
uw_refresh = uw_refresh_native
uw_shaders = uw_shaders_native


# Online play (docs/sacredfire/15-online.md): off unless bbport.ini's online_server (or
# BB_ONLINE_SERVER) names a private server. Every name the game resolves then answers that
# server; nothing else is reachable. online_id is the PSN name shown to other players.
def apply_online(env, config):
    for key, variable in (('online_server', 'BB_ONLINE_SERVER'), ('online_http_port', 'BB_ONLINE_HTTP_PORT'),
                          ('online_id', 'BB_ONLINE_ID'), ('online_allow', 'BB_ONLINE_ALLOW')):
        value = ini_value(config, key)
        if value and not env.get(variable):
            env[variable] = value
    if env.get('BB_ONLINE_SERVER'):
        say(f'Online: private server {env["BB_ONLINE_SERVER"]} (official servers are never contacted)')


def pump(stream):
    for raw in iter(stream.readline, b''):
        # The game writes CRLF; the log is a text file that adds its own CR.
        emit(raw.decode('utf-8', errors='replace').replace('\r\n', '\n'))


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    os.chdir(PORT)  # as run.sh: relative paths are from the port
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'ctrnfpc.ini'))
    # ctrnfpc.ini was bbport.ini: an older one is taken over once.
    if not Path(env['BB_CONFIG']).exists() and (data / 'bbport.ini').is_file():
        shutil.copyfile(data / 'bbport.ini', env['BB_CONFIG'])
    config = env['BB_CONFIG']
    # A portable package ships bbport.default.ini, so a new version unzipped over an old one keeps
    # the player's bbport.ini.
    for defaults in ('ctrnfpc.default.ini', 'bbport.default.ini'):
        if not Path(config).exists() and (PORT / defaults).is_file():
            shutil.copyfile(PORT / defaults, config)

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('sacredfire.exe')
    if not probe or not probe.is_file():
        fail('sacredfire.exe not found: build it with build.sh in the MSYS2 CLANG64 shell.')
    # A source build runs with the MSYS2 CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    # The output of every launch also goes to last_run.log (an in-game restart appends to it);
    # BB_SAVE_LOG=1 (with frame statistics) writes logs/<time>.log instead.
    global LOG
    if env.get('BB_SAVE_LOG') == '1':
        logs = data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y%m%d_%H%M%S')
        env['BB_FRAME_STATS'] = '1'
        env.setdefault('BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
        env.setdefault('BB_READBACK_LOG', str(logs / f'{stamp}.readbacks.csv'))
        LOG = open(logs / f'{stamp}.log', 'w', encoding='utf-8', errors='replace')
        say(f'Log: {logs / (stamp + ".log")}')
    else:
        LOG = open(data / 'last_run.log', 'a' if restarted else 'w', encoding='utf-8', errors='replace')
    say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")}{" (restart)" if restarted else ""}')

    # BB_GAME_DIR: the merged CTR dump (update 1.21 copied over the base game).
    game = Path(env.get('BB_GAME_DIR', PORT / 'orig' / TITLE_ID))
    if not (game / 'eboot.bin').is_file():
        if (game / 'Image0' / 'eboot.bin').is_file():
            fail(f'{game} has the PS4 PKG Tool layout (Image0 and Sc0): prepare it in the launcher first.')
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR to your {TITLE_ID} folder).')
    # The game check compares the executable image with the supported one (CTR NF 1.21).
    check = subprocess.run([sys.executable, '-X', 'utf8', str(PORT / 'scripts' / 'game_check.py'), str(game)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding='utf-8', errors='replace', creationflags=no_console())
    if check.returncode:
        fail(check.stdout.strip() or 'Game check failed.', 2)
    env['BB_GAME_PROFILE'] = check.stdout.strip() or 'ctr'
    # The game's own title ID and version: the European edition (CUSA14876) has its own patches.
    info = subprocess.run([sys.executable, '-X', 'utf8', str(PORT / 'scripts' / 'game_check.py'), '--info',
                           str(game)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                          encoding='utf-8', errors='replace', creationflags=no_console()).stdout.split()
    title_id, app_version = (info + [TITLE_ID, APP_VERSION][len(info):])[:2]
    say(f'Game profile: {env["BB_GAME_PROFILE"]} ({title_id} {app_version})')

    # UltraWide v1.0 by tcosi: dispatch the selected aspect-ratio patch (Settings > Display). Made for
    # CUSA13795; the European edition uses the ported data (scripts/ultrawide_eu.py, v0.4).
    try:
        if title_id not in (TITLE_ID, 'CUSA14876'):
            if uw_aspect(config) in ('21:9', '32:9'):
                say(f'Ultrawide: made for {TITLE_ID} and CUSA14876; {title_id} runs at 16:9')
            uw_mode = uw_disabled(game, config)
        else:
            uw_mode = uw_prepare(PORT, data, env, game, config)
    except RuntimeError as error:
        fail(f'Ultrawide: {error}')
    game, config = uw_mode.game, uw_mode.config

    original_game = game.resolve()
    # The Options menu's Graphics tab (scripts/graphics_tab.py): a relabelled menu archive.
    replace = []
    for line in run_python('scripts/graphics_tab.py', game, '--out', out, '--settings', config,
                           capture=True).stdout.splitlines():
        if line.startswith('archives/') and '=' in line:
            replace += ['--replace', line.strip()]
        elif line.strip():
            say(line.strip())
    game = Path(run_python('scripts/mods.py', game, '--out', out, *replace,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).stdout.strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_python('scripts/prepare.py', game, '--out', out)
        run_python('scripts/link_libc.py', game, '--out', out)
        run_python('scripts/link_modules.py', game, '--out', out, '--modules',
                   'libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            result = run_python('scripts/isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                capture=True, check=False)
            emit(result.stdout)
            if result.returncode == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        # Symbol names for the eboot's host-side imports (shadPS4's aerolib list, fetched once).
        run_python('tools/gen_import_names.py', '--out', out)
        # DLC (optional): the player's add-on packages in dlc\ (BB_DLC_DIR), reported installed.
        run_python('scripts/content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'),
                   '--dlc-dir', env.get('BB_DLC_DIR', data / 'dlc'))
        env.setdefault('BB_NEO', '0')
        apply_render_res(env, uw_mode.render_config if uw_mode.enabled else config)
        apply_online(env, config)
        say(f'PS4 Pro mode: {env["BB_NEO"]} (BB_NEO=1 turns it on)')
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        if env.get('BB_CLEAR_SHADER_CACHE', '0') == '1':
            shutil.rmtree(user_dir / 'cache' / title_id, ignore_errors=True)
            say('Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)')
        env.setdefault('BB_PREUPLOAD', '1')
        # The PC memory model (dma-buf guest memory) is Linux only: the 0.3 model on Windows.
        env['BB_GUEST_IN_PLACE'] = '0'
        env.setdefault('BB_UFFD', '0')
        env.setdefault('BB_COPY_GPU_BUFFERS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS_MAX', '65536')
        fps = env.get('BB_FPS', '30')
        user_vblank = env.get('BB_VBLANK_HZ')

        version = ''
        for candidate in (probe.parent / 'VERSION', PORT / 'VERSION'):
            if candidate.is_file():
                version = candidate.read_text().strip()
                break
        say(f'SacredFire {version or "unknown version"}')
        # The GPU library builds its path table when it loads: the user directory must exist.
        user_dir.mkdir(parents=True, exist_ok=True)
        env['BB_GPU_USER_DIR'] = str(user_dir)
        # The in-game "Apply and restart" ends the game with RESTART_CODE; it is started again
        # here, with the patches chosen in the menu, in the same console and log.
        env['BB_RESTART_EXIT_CODE'] = str(RESTART_CODE)
        env.pop('BB_RESTART_COMMAND', None)
        command = [str(probe), str(out / 'boot-linked.bin'), '--import-names', str(out / 'import_names.txt'),
                   '--content-profile', str(out / 'content.bin'), '--patches', str(out / 'patches.bin'),
                   '--app0', str(game), '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *isa_args, *args]
        first = True
        while True:
            uw_refresh(uw_mode, env, config)
            uw_shaders(uw_mode, user_dir, out)
            if not first:  # the in-game menu may have changed render_res
                apply_render_res(env, uw_mode.render_config if uw_mode.enabled else config)
            first = False
            run_python('scripts/patches.py', '--patches-dir', env.get('BB_PATCHES_DIR', 'patches'),
                       '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'),
                       '--app-version', app_version, '--title-id', title_id, '--out', out,
                       '--settings', uw_mode.render_config if uw_mode.enabled else config)
            if user_vblank:
                env['BB_VBLANK_HZ'] = user_vblank
            elif ini_has(config, 'patch_60fps=1'):
                # The game's own limiter paces frames; a 60 Hz vblank would quantize late frames.
                env['BB_VBLANK_HZ'] = '480'
                say('60 FPS patch on: virtual vblank 480 Hz')
            else:
                env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')
            # No stdin: an inherited pipe (Git Bash, MSYS2) cost the game its console output.
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, creationflags=no_console())
            reader = threading.Thread(target=pump, args=(process.stdout,), daemon=True)
            reader.start()
            code = process.wait()
            reader.join(timeout=5)
            if code != RESTART_CODE:
                say(f'--- the game ended with code {code}')
                return code
            say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} (restart from the menu)')
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
