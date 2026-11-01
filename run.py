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
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

PORT = Path(__file__).resolve().parent
TITLE_ID = 'CUSA13795'
APP_VERSION = '01.21'
RESTART_CODE = 75  # the game asks to be started again (in-game "Apply and restart")
UNSUPPORTED_GPU_CODE = 24  # no Vulkan 1.3 or no sparse buffers: GPU, driver or loader (gpu/.../vk_platform.h)

sys.path.insert(0, str(PORT / 'scripts'))
import archive_patch  # noqa: E402  (scripts/)
import button_prompts  # noqa: E402
import speed_mode  # noqa: E402
import item_rules  # noqa: E402
import online_mode  # noqa: E402
import native_shaders  # noqa: E402
import aspect_ratio  # noqa: E402
sys.path.pop(0)

LOG = None  # last_run.log (or BB_SAVE_LOG's file): everything this launch prints
# Vulkan's start-up, from the game's lines: when its window opened, whether the GPU was opened
# ("GPU: <name>, <n> MiB VRAM"), the layers installed. A player's game sat on a black window between
# the two (an overlay's Vulkan layer or a driver hanging): see watch_vulkan_start.
VULKAN_START = {'window': 0.0, 'ready': False, 'layers': ''}
VULKAN_START_SECONDS = 20


def fail(message, code=1):
    print(message, file=sys.stderr)
    if LOG:
        LOG.write(message + '\n')
        LOG.flush()
    sys.exit(code)


def blocked(exe, error):
    """Windows refused to start the game (WinError 5): an antivirus or Smart App Control blocked
    or quarantined it, usually right after an update replaced it with a new build."""
    fail(f'Windows did not let {exe.name} start ({error.strerror or error}). An antivirus or Smart App '
         f'Control blocked it, or moved it to quarantine. Restore it from the quarantine if needed, add '
         f'the game folder ({exe.parent.parent}) as an exclusion, and start the game again.\n'
         f'Windows no dejó iniciar {exe.name}. Un antivirus o el Control inteligente de aplicaciones lo '
         f'bloqueó o lo puso en cuarentena. Restáuralo si hace falta, agrega la carpeta del juego como '
         f'exclusión y vuelve a iniciar el juego.')


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
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except Exception:
        pass
    if LOG:
        try:
            LOG.write(text)
            LOG.flush()
        except Exception:
            pass
    for line in text.splitlines():
        if line.startswith('Window: ') and not VULKAN_START['window']:
            VULKAN_START['window'] = time.time()
        elif line.startswith('GPU: ') and ' MiB VRAM' in line:
            VULKAN_START['ready'] = True
        elif line.startswith('Vulkan: starting'):
            VULKAN_START['layers'] = line.partition('layers installed: ')[2].strip()


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
def render_plan_of(config):
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from patches import read_settings, render_plan
    finally:
        sys.path.pop(0)
    return render_plan(read_settings(Path(config)))


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


# The server derives the player's ID from the nickname, so everyone left on a shared default
# name was the same player online (one wallet, and matchmaking evicting each other). These names
# are refused online; the launcher asks for a real one before it starts the game.
RESERVED_NICKS = {'ryupc', 'ryunet', 'player', 'jugador', 'guest'}
OFFLINE_NAME = 'Player'


def nick_ok(name):
    return (3 <= len(name) <= 16 and all(c.isascii() and (c.isalnum() or c in '_-') for c in name)
            and name.lower() not in RESERVED_NICKS)


def key_store():
    """Where this PC keeps its keys outside the game folders, so that a deleted, moved or
    re-downloaded folder keeps the nicknames: %APPDATA% on Windows (as the launcher keeps them),
    the XDG data folder (~/.local/share) on Linux. None when there is no such folder."""
    if os.name == 'nt':
        base = os.environ.get('APPDATA')
    else:
        base = os.environ.get('XDG_DATA_HOME') or (os.path.join(os.path.expanduser('~'), '.local', 'share')
                                                   if os.environ.get('HOME') else None)
    return Path(base) / 'CTR-NitroFueled-PC' if base else None


def valid_key(k):
    return len(k) == 64 and all(c in '0123456789abcdef' for c in k)


def known_keys():
    """Every key used on this PC (ryunet_online_keys.txt next to the backup; the Windows
    launcher's KnownKeys): a nickname this folder's key does not own may be another folder's."""
    store = key_store()
    try:
        lines = (store / 'ryunet_online_keys.txt').read_text(encoding='ascii', errors='replace').split()
    except (OSError, TypeError):
        return []
    keys = []
    for k in (line.strip().lower() for line in lines):
        if valid_key(k) and k not in keys:
            keys.append(k)
    return keys


def remember_key(key):
    store = key_store()
    if not store or not valid_key(key) or key in known_keys():
        return
    try:
        store.mkdir(parents=True, exist_ok=True)
        with open(store / 'ryunet_online_keys.txt', 'a', encoding='ascii') as f:
            f.write(key + '\n')
    except OSError:
        pass


def use_key(data, key):
    """This folder plays with another of this PC's keys from now on. True when saved."""
    try:
        (Path(data) / 'ryunet_online.key').write_text(key + '\n', encoding='ascii')
    except OSError:
        return False
    return True


def online_key(data):
    """This installation's secret: the server lets the first key to use a nickname keep it, so
    nobody else can play under it. Created once, next to run.py; the launcher reads the same
    file. Copying the folder to another PC keeps the nicknames; losing the file loses them."""
    path = Path(data) / 'ryunet_online.key'
    # A second copy outside the game folder (key_store).
    store = key_store()
    backup = store / 'ryunet_online.key' if store else None

    def read(p):
        try:
            k = p.read_text(encoding='ascii', errors='replace').strip().lower() if p else ''
        except OSError:
            return ''
        return k if valid_key(k) else ''

    key = read(path) or read(backup) or secrets.token_hex(32)
    if read(path) != key:
        try:
            path.write_text(key + '\n', encoding='ascii')
        except OSError as e:
            say(f'Could not save the online key ({e}): the nickname cannot be kept for this installation.')
            return ''
    if backup and not read(backup):
        try:
            backup.parent.mkdir(parents=True, exist_ok=True)
            backup.write_text(key + '\n', encoding='ascii')
        except OSError:
            pass
    remember_key(read(backup))
    remember_key(key)
    return key


def server_key(key, host):
    """What one server sees of the key: HMAC-SHA256(key, host). Each server gets a different
    value, so a server you try (or one an invite points to) cannot reuse it on another to take
    your nicknames. The launcher derives it the same way for its nickname check."""
    return hmac.new(bytes.fromhex(key), host.strip().lower().encode('utf-8'), hashlib.sha256).hexdigest()


def key_hash(skey):
    """What the server stores of a server key (nick_claims.go keyHash): the key of signatures."""
    return hashlib.sha256(skey.strip().lower().encode('ascii')).hexdigest()


# Seconds to add to this PC's clock to get the RyuNET server's. Signatures carry the time and the
# server takes them only within 5 minutes of its own clock: with the PC's clock off, every signed
# request was refused ("nickname signature out of time") and the nickname read as taken.
SERVER_CLOCK_OFFSET = 0


def note_server_clock(reply):
    """An answer of the RyuNET server (a urllib response or HTTPError): its Date header sets
    SERVER_CLOCK_OFFSET."""
    global SERVER_CLOCK_OFFSET
    import email.utils
    try:
        server = email.utils.parsedate_to_datetime(reply.headers.get('Date', '')).timestamp()
    except (AttributeError, TypeError, ValueError, IndexError):
        return
    offset = round(server - time.time())
    # The header has whole seconds: a few seconds either way change nothing.
    SERVER_CLOCK_OFFSET = offset if abs(offset) > 5 else 0


def proof_header(khash, method, target):
    """X-RyuNET-Proof for one request (the server's nick_proof.go): signed with the key's hash, so
    the key never crosses the network, and good once, for a few minutes of the server's time."""
    ts, nonce = str(int(time.time()) + SERVER_CLOCK_OFFSET), secrets.token_hex(16)
    mac = hmac.new(khash.encode('ascii'), f'{ts}\n{nonce}\n{method}\n{target}'.encode('utf-8'),
                   hashlib.sha256).hexdigest()
    return f'1.{ts}.{nonce}.{mac}'


HTTPS_PORT = 443      # the server's nginx: /ryunet/ by HTTPS (tests point it elsewhere)
HTTPS_FAILED = set()  # servers whose HTTPS failed this run: their next requests go to the port


def ryunet_json(host, port, target, timeout, method='GET', headers=None, body=None):
    """A request to the RyuNET server and its JSON answer, with the reply (for its Date header).
    By HTTPS on the server's name first (443, where its nginx passes /ryunet/ to the server): a
    filter on the way cannot rewrite it, and networks that only let 80/443 out reach it. Then by
    plain HTTP on the port, by name and by address when something on the way answers for the
    name: an ISP's filter that redirects to its own warning page (Spectrum's Security Shield) or
    serves one. The game's own requests do the same (src/runtime_http.c). headers(method, target)
    makes each attempt's headers, so a signature is never sent twice. OSError when none works."""
    import ipaddress
    import socket
    import urllib.error
    import urllib.request

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None  # the server never redirects: a redirect comes from something on the way

    opener = urllib.request.build_opener(NoRedirect)
    urls = []
    try:
        ipaddress.ip_address(host)
    except ValueError:  # a name: a certificate can vouch for it (never for a bare address)
        if host not in HTTPS_FAILED:
            urls.append((f'https://{host}' + (f':{HTTPS_PORT}' if HTTPS_PORT != 443 else '') + target, True))
    urls.append((f'http://{host}:{port}{target}', False))
    try:
        address = socket.gethostbyname(host)
        if address != host:
            urls.append((f'http://{address}:{port}{target}', False))
    except OSError:
        pass
    failure = OSError(f'{host}: no answer')
    for url, secure in urls:
        name = url.split('/')[2]
        request = urllib.request.Request(url, method=method,
                                         data=body if body is not None else (b'' if method == 'POST' else None),
                                         headers={**(headers(method, target) if headers else {}),
                                                  **({'Content-Type': 'application/json'} if body is not None else {})})
        try:
            with opener.open(request, timeout=timeout) as reply:
                if 'json' not in (reply.headers.get('Content-Type') or ''):
                    failure = OSError(f'{name}: answered by something on the way, not the server')
                    HTTPS_FAILED.update([host] if secure else [])
                    continue
                return json.loads(reply.read(1 << 16)), reply  # the theme carries the item rules
        except urllib.error.HTTPError as error:
            if secure and 'json' not in (error.headers.get('Content-Type') or ''):
                HTTPS_FAILED.add(host)  # not the server's answer (no /ryunet/ there yet): the port
                failure = OSError(f'{name}: HTTPS answered {error.code}')
                continue
            if 300 <= error.code < 400:
                failure = OSError(f'{name}: redirected to {error.headers.get("Location")}')
                continue
            raise
        except OSError as error:  # no 443, a certificate that does not verify, a clock far off
            if not secure:
                raise
            HTTPS_FAILED.add(host)
            failure = OSError(f'{name}: HTTPS failed ({error})')
    raise failure


def claim_nick(host, port, name, khash):
    """Registers this installation for a free nickname, before the game logs in with it (the
    game's own requests are signed and cannot register). The key's hash crosses the network only
    here, once. Nothing happens when the server cannot be reached or is older."""
    import urllib.parse
    q = urllib.parse.urlencode({'name': name})
    try:
        status, _ = ryunet_json(host, port, f'/ryunet/nick?{q}', 4,
                                headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t)})
        if status.get('status') != 'free':
            return
        claimed, _ = ryunet_json(host, port, f'/ryunet/nick/claim?{q}', 4, method='POST',
                                 headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t),
                                                       'X-RyuNET-Key-Hash': khash})
        say(f'Nickname {name}: {claimed.get("status")} on {host}')
    except (OSError, ValueError) as error:
        say(f'Nickname {name}: could not register it on {host} ({error})')


def server_settings(host, port):
    """What the server announces for every player (GET /ryunet/theme): the season theme, which
    scripts/patches.py loads in every level (THEME_VALUES), and the seconds an online race goes on
    after the first player finishes (end_race_patches). {} when it does not answer."""
    try:
        settings, reply = ryunet_json(host, port, '/ryunet/theme', 3)
    except (OSError, ValueError):
        return {}
    note_server_clock(reply)
    return settings if isinstance(settings, dict) else {}


RELEASES_API = 'https://api.github.com/repos/GetLostIn/CTR-NitroFueled-PC/releases/latest'


def parse_version(text):
    """(major, minor, patch, stage) as the launchers compare versions (launcher/appimage_update.py,
    launcher/windows/port_update.cpp): stage 0 a pre-release, 1 the release, 2 commits after it."""
    m = re.match(r'^\s*[vV]?(\d{1,5})\.(\d{1,5})(?:\.(\d{1,5}))?(.*?)\s*$', text or '')
    if not m:
        return None
    suffix = m.group(4)
    while True:
        for drop in ('-dirty', '-windows', '-linux'):
            if suffix.endswith(drop):
                suffix = suffix[:-len(drop)]
                break
        else:
            break
    stage = 1 if not suffix else 2 if re.fullmatch(r'-\d+-g[0-9a-fA-F]+', suffix) else 0
    return int(m.group(1)), int(m.group(2)), int(m.group(3) or 0), stage


def update_notice(env, version):
    """A release newer than this one: BB_UPDATE_AVAILABLE=<tag>, which the game shows as a notice
    (gpu/shim/bbport_overlay.cpp). Off with BB_UPDATE_CHECK=0; nothing when GitHub does not answer."""
    have = parse_version(version)
    if env.get('BB_UPDATE_CHECK') == '0' or not have:
        return
    import urllib.request
    try:
        request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                                'User-Agent': 'SacredFire-run.py'})
        with urllib.request.urlopen(request, timeout=3) as reply:
            tag = json.loads(reply.read(1 << 20)).get('tag_name', '')
    except (OSError, ValueError, AttributeError):
        return
    latest = parse_version(tag)
    if latest and latest > have:
        env['BB_UPDATE_AVAILABLE'] = tag
        say(f'A newer release is available: {tag} (update from the launcher)')


def setup_online(env, config, data):
    # The item-box rules come only from the server (below): items are drawn on each player's PC,
    # so a local setting would let one player pick their own odds.
    env.pop('BB_ITEM_RULES', None)
    ini_values = {}
    try:
        for l in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = l.partition('=')
            if sep and not k.startswith('#'):
                ini_values[k.strip()] = v.strip()
    except OSError:
        pass

    # Ignore the retired launcher toggle; the native selector owns this session's mode.
    env.pop('BB_SPEED_MODE', None)

    name = env.get('BB_ONLINE_NAME', ini_values.get('online_name', ini_values.get('user_name', ''))).strip()
    online = env.get('BB_ONLINE', ini_values.get('online', '0'))
    if online == '1' and not nick_ok(name):
        fail('Online play needs your own nickname: 3 to 16 letters, digits, _ or -, and not a default '
             'name like RyuPC. Set it in the launcher (Online tab) or as online_name= in bbport.ini.\n'
             'El juego online necesita tu propio apodo: de 3 a 16 letras, números, _ o -, y no un nombre '
             'por defecto como RyuPC. Ponlo en el launcher (pestaña Online) o como online_name= en bbport.ini.')
    env['BB_USER_NAME'] = name or OFFLINE_NAME

    if online != '1':
        return

    env['BB_ONLINE'] = '1'
    env['BB_NET_TRACE'] = '1'
    server = env.get('BB_ONLINE_SERVER', ini_values.get('online_server', 'octr.ryu7w7.xyz'))
    if not server or server.strip() == '':
        server = 'octr.ryu7w7.xyz'
    host = server
    auth_port = env.get('BB_ONLINE_HTTP_PORT', ini_values.get('online_http_port', '8080'))
    if ':' in server and not server.startswith('['):
        h, _, p = server.partition(':')
        if p.isdigit():
            host = h
            auth_port = p

    env['BB_ONLINE_SERVER'] = host
    env['BB_ONLINE_HTTP_PORT'] = auth_port  # the runtime's own requests (Friends tab)
    env['BB_ONLINE_NAME'] = name
    # The server's announcements (used below); its answer also tells its clock, before anything
    # is signed.
    announced = server_settings(host, auth_port)
    if SERVER_CLOCK_OFFSET:
        env['BB_ONLINE_CLOCK_OFFSET'] = str(SERVER_CLOCK_OFFSET)
        if abs(SERVER_CLOCK_OFFSET) > 60:
            minutes = round(abs(SERVER_CLOCK_OFFSET) / 60)
            say(f"This PC's clock is {minutes} min {'behind' if SERVER_CLOCK_OFFSET > 0 else 'ahead of'} "
                "the server's: online uses the server's time, but sync it (Windows: Settings > Time > Sync now).")
    else:
        env.pop('BB_ONLINE_CLOCK_OFFSET', None)
    key = online_key(data)
    env.pop('BB_ONLINE_KEY', None)  # the key is not sent any more: requests are signed with its hash
    if key:
        env['BB_ONLINE_KEYHASH'] = key_hash(server_key(key, host))
        claim_nick(host, auth_port, name, env['BB_ONLINE_KEYHASH'])
    # online_inventory=save: keep the Pit Stop items of this save (a "100%" save) online
    # instead of the ones bought on the server, which are tied to the nickname.
    if env.get('BB_ONLINE_INVENTORY', ini_values.get('online_inventory', 'server')).strip().lower() == 'save':
        env['BB_ONLINE_INVENTORY'] = 'save'
    else:
        env.pop('BB_ONLINE_INVENTORY', None)
    account_id = env.get('BB_ONLINE_ACCOUNT_ID', ini_values.get('online_account_id', ''))
    if account_id:
        env['BB_ONLINE_ACCOUNT_ID'] = account_id
    online_port = env.get('BB_ONLINE_PORT', ini_values.get('online_port', ''))
    if online_port:
        env['BB_ONLINE_PORT'] = online_port
    friends = env.get('BB_ONLINE_FRIENDS', ini_values.get('online_friends', ''))
    if friends:
        env['BB_ONLINE_FRIENDS'] = friends

    overrides_file = data / 'host_overrides.json'
    overrides = {
        "lavender-ps4-auth3.prod.demonware.net": f"http://{host}:{auth_port}",
        "lavender-ps4-auth3.cert.demonware.net": f"http://{host}:{auth_port}",
        "lavender-ps4-auth3.dev.demonware.net":  f"http://{host}:{auth_port}",
        "prod.umbrella.demonware.net": f"http://{host}:{auth_port}",
        "cert.umbrella.demonware.net": f"http://{host}:{auth_port}",
        "dev.umbrella.demonware.net":  f"http://{host}:{auth_port}",
        "prod.uno.demonware.net": f"http://{host}:{auth_port}",
        "cert.uno.demonware.net": f"http://{host}:{auth_port}",
        "dev.uno.demonware.net":  f"http://{host}:{auth_port}",
        "lavender-ps4-lobby.prod.demonware.net": host,
        "lavender-ps4-lobby.cert.demonware.net": host,
        "lavender-ps4-lobby.dev.demonware.net":  host,
        "stun.us.demonware.net": host,
        "stun.eu.demonware.net": host,
        "stun.jp.demonware.net": host,
        "stun.au.demonware.net": host,
        # Telemetry / CDN hosts the game contacts directly over HTTPS: keep
        # them local (the server answers 404, which the game tolerates) and
        # never touch the real servers.
        "pipes-prod-glutton.public.aws.demonware.net": f"http://{host}:{auth_port}",
        "objectstore.prod.demonware.net": f"http://{host}:{auth_port}",
        # Demonware friends API (eboot rodata ~0x1bef504, base http://friends.dev...):
        # list / invitation / pending / pair. Routed to the server's friends
        # handler; without this the game gets EHOSTUNREACH and shows no friends.
        "friends.dev.demonware.net": f"http://{host}:{auth_port}",
    }
    overrides_file.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    say(f'Online active -> server {host}:{auth_port}, user {name}')
    # Every player loads the theme the server announces (bbport.ini theme= only applies offline),
    # and the server's race-end countdown.
    theme = announced.get('theme')
    if not env.get('BB_THEME') and theme in ('default', 'halloween', 'golden'):
        env['BB_THEME'] = theme
        if theme != 'default':
            say(f'Season theme from the server: {theme}')
    end_race = announced.get('end_race_timeout')
    if not env.get('BB_END_RACE_TIMEOUT') and isinstance(end_race, (int, float)) and 10 <= end_race <= 120:
        env['BB_END_RACE_TIMEOUT'] = str(end_race)
        say(f'Online race end countdown from the server: {end_race} s')
    # The item-box rules (odds, cooldowns) the server's administrators chose, for every player
    # (scripts/item_rules.py); none: the game's own.
    items = announced.get('items')
    rules_file = data / 'out' / 'item_rules' / 'server.json'
    if isinstance(items, dict) and items:
        rules_file.parent.mkdir(parents=True, exist_ok=True)
        rules_file.write_text(json.dumps(items, sort_keys=True), encoding='utf-8')
        env['BB_ITEM_RULES'] = str(rules_file)
        say(f'Item rules from the server: {items.get("name") or "custom"}')


def watch_vulkan_start(process, env):
    """Waits for the game; True when Vulkan never finished starting (no GPU opened
    VULKAN_START_SECONDS after the window) and the game was closed to start again without the
    overlay layers. Once without them, a hang is only reported."""
    reported = False
    while True:
        try:
            process.wait(timeout=1)
            return False
        except subprocess.TimeoutExpired:
            pass
        opened = VULKAN_START['window']
        if not opened or VULKAN_START['ready'] or time.time() - opened < VULKAN_START_SECONDS:
            continue
        layers = VULKAN_START['layers'] or 'not listed'
        if not env.get('VK_LOADER_LAYERS_DISABLE'):
            say(f'--- Vulkan did not finish starting in {VULKAN_START_SECONDS} s (black window). Usually a '
                f'program that hooks into games (an overlay or recorder: OBS, RivaTuner/Afterburner, Overwolf, '
                f'Discord, ...) or a broken driver. Starting again without the overlay layers '
                f'(installed: {layers}).')
            process.kill()
            process.wait()
            return True
        if not reported:
            reported = True
            say('--- Vulkan still did not start without the overlay layers: the graphics driver is the likely '
                'cause. Reinstall the latest driver for your GPU (on a laptop, the one from its maker too), '
                'and close overlay and recording programs.')


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

    if 'BB_LANGUAGE' not in env:
        try:
            for l in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
                k, _, v = l.partition('=')
                if k.strip() in ('game_language', 'language') and v.strip():
                    env['BB_LANGUAGE'] = v.strip()
                    break
        except OSError:
            pass

    if 'BB_ASYNC_SUBMIT' not in env:
        v = ini_value(config, 'async_submit')
        if v is not None:
            env['BB_ASYNC_SUBMIT'] = v

    setup_online(env, config, data)

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
    try:
        features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, text=True, creationflags=no_console())
    except PermissionError as error:
        blocked(probe, error)
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
        user_name = env.get('BB_USER_NAME', env.get('BB_ONLINE_NAME', ''))
        log_name = f'last_run_{user_name}.log' if user_name and user_name != OFFLINE_NAME else 'last_run.log'
        try:
            LOG = open(data / log_name, 'a' if restarted else 'w', encoding='utf-8', errors='replace')
        except OSError:
            LOG = open(data / f'last_run_{os.getpid()}.log', 'w', encoding='utf-8', errors='replace')
    say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")}{" (restart)" if restarted else ""}')

    # BB_GAME_DIR: the merged CTR dump (update 1.21 copied over the base game).
    game_dir_env = env.get('BB_GAME_DIR')
    if game_dir_env:
        game = Path(game_dir_env)
    elif (PORT / f'{TITLE_ID}-1.21' / 'eboot.bin').is_file():
        game = PORT / f'{TITLE_ID}-1.21'
    elif (PORT / TITLE_ID / 'eboot.bin').is_file():
        game = PORT / TITLE_ID
    else:
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
    # The game's own title ID and version (the EU edition, CUSA14876 1.00, has its own patches).
    info = run_python('scripts/game_check.py', '--info', game, capture=True).stdout.split()
    title_id, app_version = (info + [TITLE_ID, APP_VERSION][len(info):])[:2]
    say(f'Game profile: {env["BB_GAME_PROFILE"]}')
    if 'BB_LANGUAGE' in env:
        say(f'Game language ID: {env["BB_LANGUAGE"]}')

    import mod_packages
    try:
        game, config = mod_packages.prepare(PORT, data, env, game, config,log=say)
    except (ValueError, RuntimeError, OSError) as error:
        fail('Mod launch blocked: ' + str(error) + '\nChoose compatible settings or disable the indicated mod in Mods.')
    original_game = game.resolve()
    import mod_file_paths
    mod_file_paths.configure(env)
    # The Options menu's Graphics tab and the main menu's Quit Game (scripts/graphics_tab.py): an
    # extra archive of relabelled menus, served by the runtime as archives/update3.pak (no copy of
    # the game folder). USA/EU 1.21: the engine validates the region's hooks
    # (gpu/shim/bbport_graphics_tab.cpp), and without them the runtime leaves the menus alone.
    env.pop('BB_GRAPHICS_TAB_ARCHIVE', None)
    if title_id in ('CUSA13795', 'CUSA14876') and app_version == '01.21':
        result = run_python('scripts/graphics_tab.py', game, '--out', out, '--settings', config,
                            capture=True, check=False)
        for line in result.stdout.splitlines():
            if line.startswith('archives/update3.pak='):
                env['BB_GRAPHICS_TAB_ARCHIVE'] = line.partition('=')[2].strip()
            elif line.strip():
                say(line.strip())
        if result.returncode:
            say(f'Graphics tab: off (scripts/graphics_tab.py stopped with code {result.returncode})')
    game = Path(run_python('scripts/mods.py', game, '--out', out,
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
                   '--dlc-dir', env.get('BB_DLC_DIR') or data / 'dlc')  # empty: dlc\, as run.sh
        # The shader recipe (shaders/recipe.zip): prepared for this GPU at the first start, from the
        # game's own shaders, so new tracks and racers do not stop the game to compile them.
        # BB_SHADER_RECIPE=0 leaves it out; another path uses that recipe.
        recipe_setting = env.get('BB_SHADER_RECIPE', '')
        recipe = Path(recipe_setting) if recipe_setting not in ('', '0') else PORT / 'shaders' / 'recipe.zip'
        if recipe_setting != '0' and recipe.is_file():
            run_python('scripts/gcn_shaders.py', game, '--out', out)
            env['BB_SHADER_RECIPE'] = str(recipe)
            env['BB_GCN_SHADERS'] = str(out / 'gcn_shaders.bin')
        else:
            env.pop('BB_SHADER_RECIPE', None)
            env.pop('BB_GCN_SHADERS', None)
        # Every race track in the online playlist, at the same weight (scripts/online_playlist.py),
        # always: public rooms draw from the same pool for everyone. The game reads it through an
        # archive patch; the archives stay as they are. When it cannot be built, the game's own.
        env.pop('BB_ARCHIVE_PATCHES', None)
        run_python('scripts/online_playlist.py', game, '--out', out)
        playlist_patch = out / 'online_playlist.bbpatch'
        base_patches = {}
        if playlist_patch.is_file():
            base_patches[archive_patch.read(playlist_patch)[1]] = playlist_patch
        # Xbox button prompts (bbport.ini button_prompts, scripts/button_prompts.py): the fonts'
        # button image through archive patches, each keeping the playlist's for the same archive.
        try:
            base_patches.update(button_prompts.prepare(game, out, config, env, base_patches))
        except (OSError, ValueError) as error:
            say(f'Button prompts: {error}; PlayStation buttons')
        # Item-box rules (scripts/item_rules.py): the server's, which setup_online saved.
        try:
            rules_path = env.get('BB_ITEM_RULES')
            rules = json.loads(Path(rules_path).read_text(encoding='utf-8')) if rules_path else None
            base_patches.update(item_rules.prepare(game, out, rules, base_patches, log=say))
        except (OSError, ValueError, KeyError, TypeError) as error:
            say(f"Item rules: {error}; the game's own odds")
        env.setdefault('BB_NEO', '0')
        # The aspect ratio (bbport.ini aspect_ratio, scripts/aspect_ratio.py): the render
        # path and the patches read its settings (the player's, with the ones it was made for),
        # and it adds its archive patches, executable patch and HUD scale. Read again at a restart.
        layout_saved = {}

        def apply_layout():
            env.pop('BB_SPEED_MODE', None)
            for key, value in layout_saved.items():
                if value is None:
                    env.pop(key, None)
                else:
                    env[key] = value
            layout_saved.clear()
            layout = aspect_ratio.prepare(game, out, config, env, title_id, app_version, base_patches)
            if mod_packages.ACTIVE and aspect_ratio.selection(aspect_ratio.read_ini(config)) and not layout:
                raise ValueError('The selected mods cannot use this aspect ratio; see the preparation message above.')
            if layout:
                layout_saved.update({key: env.get(key) for key in layout['env']})
                env.update(layout['env'])
            archives = aspect_ratio.archive_list(base_patches, layout)
            if env.get('BB_MOD_ARCHIVE_PLAN'):
                from mod_overlays import for_launch
                builtins = {archive_patch.read(p)[1]: Path(p) for p in archives.split('|') if p} if archives else {}
                composed = for_launch(game, out / 'mod-overlays', env['BB_MOD_ARCHIVE_PLAN'], builtins)
                archives = '|'.join(str(p) for p in composed.values())
            env.pop('BB_PRIVATE_ARCHIVE_PATCHES', None)
            env.pop('BB_ONLINE_MODE_HOOK', None)
            if env.get('BB_ONLINE') == '1' or env.get('BB_SPEED_SOLO_TEST') == '1':
                if title_id in ('CUSA13795', 'CUSA14876') and app_version == '01.21':
                    base = [Path(p) for p in archives.split('|') if p]
                    if env.get('BB_ONLINE') == '1':
                        base, env['BB_ONLINE_MODE_HOOK'] = online_mode.prepare(
                            game, out / 'online-mode', out / 'eboot.elf', base)
                        env['BB_MOD_FILE_MAP'] = str((out / 'online-mode/cup-files.map').resolve())
                        env.setdefault('BB_MOD_SOURCE_GAME', str(game))
                        env['BB_KNOWN_HEROES'] = online_mode.known_heroes(game)
                    try:
                        normal_patches, speed_patches, track_count = speed_mode.prepare(
                            game, out / 'speed-mode', base)
                    except (ValueError, OSError, KeyError, IndexError) as error:
                        # a damaged game file stops Speed Mode only: online plays as before
                        say(f'ITEMLESS (Experimental) not available: {error}')
                        archives = '|'.join(map(str, base))
                    else:
                        archives = '|'.join(map(str, normal_patches))
                        env['BB_PRIVATE_ARCHIVE_PATCHES'] = '|'.join(map(str, speed_patches))
                        say(f'ITEMLESS (Experimental): {track_count} tracks prepared for private rooms')
            if archives:
                env['BB_ARCHIVE_PATCHES'] = archives
            else:
                env.pop('BB_ARCHIVE_PATCHES', None)
            return (layout['config'], layout['patches_dir']) if layout else (config, None)

        settings, layout_patches = apply_layout()
        if mod_packages.ACTIVE:
            mod_packages.preflight(Path(env['BB_PATCHES_DIR']).parent, settings, Path(env['BB_PATCHES_CONFIG']),
                                   [layout_patches] if layout_patches else [], title_id, app_version, game=game)
        apply_render_res(env, settings)
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
        update_notice(env, version)
        # The graphics engine (bbport.ini renderer): the native Vulkan renderer is experimental; its
        # shaders are built from this game the first time (scripts/native_shaders.py, ~5 minutes).
        # Anything that stops it starts the game with shadPS4's Vulkan, as before.
        if (ini_value(config, 'renderer') or 'shadps4') == 'native' and 'BB_NATIVE_VK' not in env:
            if not native_shaders.supported(title_id, app_version):
                say(f'Native Vulkan: only the USA or EU 1.21 game (CUSA13795, CUSA14876) for now; this is {title_id} '
                    f'{app_version}, so the game starts with Vulkan (shadPS4)')
            else:
                shaders = native_shaders.shader_dir(data, title_id, app_version)
                if not native_shaders.ready(shaders):
                    say('Native Vulkan: preparing its shaders from your game (only the first time, a few minutes)')
                    # "@progress" lines: the launchers show a window with a progress bar meanwhile
                    last = [-1]

                    def progress(percent, stage):
                        if percent != last[0]:
                            last[0] = percent
                            emit(f'@progress native-shaders {percent} {stage}\n')
                    built = native_shaders.build(PORT, game, shaders, say, progress=progress)
                    emit(f'@progress native-shaders done {"ok" if built else "failed"}\n')
                if native_shaders.ready(shaders):
                    env.update(native_shaders.environment(shaders, ini_value(config, 'temporal') or ''))
                    say('Graphics engine: native Vulkan (experimental)')
                else:
                    say('Native Vulkan: its shaders are not ready, so the game starts with Vulkan (shadPS4)')
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
            if not first:  # the in-game menu may have changed render_res
                settings, layout_patches = apply_layout()
                apply_render_res(env, settings)
                if mod_packages.ACTIVE:
                    mod_packages.preflight(Path(env['BB_PATCHES_DIR']).parent, settings, Path(env['BB_PATCHES_CONFIG']),
                                           [layout_patches] if layout_patches else [], title_id, app_version, game=game)
            first = False
            run_python('scripts/patches.py', '--patches-dir', env.get('BB_PATCHES_DIR', 'patches'),
                       *(['--strict-overlap'] if mod_packages.ACTIVE else []),
                       *(['--patches-dir', layout_patches] if layout_patches else []),
                       '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'),
                       '--app-version', app_version, '--title-id', title_id, '--out', out,
                       '--settings', settings)
            # Ultrawide: videos and cutscenes at 16:9 (src/runtime_cinematic.c) unless turned off.
            for key in ('BB_CINEMATIC_16X9', 'BB_CUTSCENE'):
                env.pop(key, None)
            plan = render_plan_of(settings)
            if plan and plan.get('output') and not ini_has(settings, 'ultrawide_cinematics=0'):
                env['BB_CINEMATIC_16X9'] = '1'
                env['BB_CUTSCENE'] = run_python('scripts/patches.py', '--print-cutscene', '--out', out,
                                                capture=True).stdout.strip()
            # Ultrawide: the title and customization screens fill the width (src/runtime_frontend.c).
            env.pop('BB_FRONTEND_FOV', None)
            if plan and plan.get('output') and not ini_has(settings, 'ultrawide_frontend=0'):
                env['BB_FRONTEND_FOV'] = run_python('scripts/patches.py', '--print-frontend', '--out', out,
                                                    capture=True).stdout.strip()
            # Disable Motion Blur: the eboot patch stops the camera's blur; the blur shaders go too
            # (objects moving on their own: the crowd, plants; bb_motion_blur.h)
            env['BB_NO_MOTION_BLUR'] = '1' if ini_has(config, 'patch_no_motion_blur=1') else '0'
            if user_vblank:
                env['BB_VBLANK_HZ'] = user_vblank
            elif ini_has(config, 'patch_60fps=1'):
                # The game's own limiter paces frames; a 60 Hz vblank would quantize late frames.
                env['BB_VBLANK_HZ'] = '480'
                say('60 FPS patch on: virtual vblank 480 Hz')
            else:
                env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')
            # No stdin: an inherited pipe (Git Bash, MSYS2) cost the game its console output.
            try:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, creationflags=no_console())
            except PermissionError as error:
                blocked(probe, error)
            reader = threading.Thread(target=pump, args=(process.stdout,), daemon=True)
            VULKAN_START.update(window=0.0, ready=False)
            reader.start()
            hung = watch_vulkan_start(process, env)
            code = process.wait()
            reader.join(timeout=5)
            if hung:
                # the loader's own switch: no implicit layer this time (the system is not changed)
                env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
                say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} (without overlay layers)')
                continue
            if code != RESTART_CODE:
                say(f'--- the game ended with code {code}')
                if code == UNSUPPORTED_GPU_CODE:
                    say('--- This GPU or its driver lacks what the game needs from Vulkan (version 1.3 and '
                        'sparse buffers; the STOP line above says which). Update the graphics driver; if it '
                        'is already the latest, the GPU is too old for this system.')
                mod_packages.finish(code)
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
