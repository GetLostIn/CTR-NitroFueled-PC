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


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()
    if LOG:
        LOG.write(text)
        LOG.flush()


def say(message):
    emit(message + '\n')


def run_python(script, *args, capture=False):
    # -X utf8: the embeddable Python of the portable package ignores PYTHONUTF8, and the scripts
    # print the game's title (with its trademark sign) to a pipe.
    command = [sys.executable, '-X', 'utf8', str(PORT / script), *map(str, args)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=None if capture else subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', creationflags=no_console())
    if not capture and result.stdout:
        emit(result.stdout)
    if result.returncode:
        sys.exit(result.returncode)
    return result.stdout if capture else None


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


# Online play (docs/sacredfire/15-online.md): off unless bbport.ini's online switch is on (the
# launcher's Online section) and online_server names a private server. Every name the game resolves
# then answers that server; nothing else is reachable. online_id is the name shown to other players.
# BB_ONLINE_SERVER already in the environment still wins (for a manual run).
def apply_online(env, config):
    if not env.get('BB_ONLINE_SERVER') and ini_value(config, 'online') != '1':
        return  # the switch is off: stay offline, whatever address is stored
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
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    # A portable package ships bbport.default.ini, so a new version unzipped over an old one keeps
    # the player's bbport.ini.
    if not Path(config).exists() and (PORT / 'bbport.default.ini').is_file():
        shutil.copyfile(PORT / 'bbport.default.ini', config)

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('sacredfire.exe')
    if not probe or not probe.is_file():
        fail('sacredfire.exe not found: build it with build.sh in the MSYS2 CLANG64 shell.')
    # A source build runs with the MSYS2 CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

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
    say(f'Game profile: {env["BB_GAME_PROFILE"]}')

    original_game = game.resolve()
    game = Path(run_python('scripts/mods.py', game, '--out', out,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_python('scripts/prepare.py', game, '--out', out)
        run_python('scripts/link_libc.py', game, '--out', out)
        run_python('scripts/link_modules.py', game, '--out', out, '--modules',
                   'libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
        # Symbol names for the eboot's host-side imports (shadPS4's aerolib list, fetched once).
        run_python('tools/gen_import_names.py', '--out', out)
        # DLC (optional): the player's add-on packages in dlc\ (BB_DLC_DIR), reported installed.
        run_python('scripts/content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'),
                   '--dlc-dir', env.get('BB_DLC_DIR', data / 'dlc'))
        env.setdefault('BB_NEO', '0')
        apply_render_res(env, config)
        apply_online(env, config)
        say(f'PS4 Pro mode: {env["BB_NEO"]} (BB_NEO=1 turns it on)')
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        if env.get('BB_CLEAR_SHADER_CACHE', '0') == '1':
            shutil.rmtree(user_dir / 'cache' / TITLE_ID, ignore_errors=True)
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
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        first = True
        while True:
            if not first:  # the in-game menu may have changed render_res
                apply_render_res(env, config)
            first = False
            run_python('scripts/patches.py', '--patches-dir', env.get('BB_PATCHES_DIR', 'patches'),
                       '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'),
                       '--app-version', APP_VERSION, '--title-id', TITLE_ID, '--out', out,
                       '--settings', config)
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
