#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the Windows portable ZIP from a finished build (out/sacredfire.exe).

    C:\\msys64\\clang64\\bin\\python.exe packaging/windows/make_portable.py [--version v0.1.1]
        [--dlss-sdk <NVIDIA DLSS SDK folder>]

With --dlss-sdk, bin\\ also gets the DLSS bridge (dlss_bridge\\sacredfire_dlss.dll, built with
dlss_bridge\\build.bat) and the SDK's nvngx_dlss.dll, which NVIDIA's DLSS SDK license lets an
application redistribute; their licenses go in licenses\\.

Needs MSYS2 (CLANG64) for the DLLs and for pacman, which tells THIRD-PARTY.txt which package,
version and license each bundled DLL comes from. Output: dist/CTR-NitroFueled-PC-portable-windows.zip
(+ the unpacked folder next to it, and its SHA256). Nothing from the user's own machine goes in:
no user/ folder, no saves, no logs, no personal bbport.ini (the defaults are
packaging/windows/portable/bbport.ini).
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PORTABLE = ROOT / 'packaging' / 'windows' / 'portable'
MSYS2 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64'))
CLANG_BIN = MSYS2 / 'clang64' / 'bin'
NAME = 'CTR-NitroFueled-PC-portable'
LAUNCHER = 'CTR-NitroFueled-PC.exe'
# The Python that runs run.py and scripts/ (python.org's "embeddable package": no installer, no
# registry, no PATH). Pinned by version and SHA-256.
PYTHON_VERSION = '3.14.8'
PYTHON_URL = f'https://www.python.org/ftp/python/{PYTHON_VERSION}/python-{PYTHON_VERSION}-embed-amd64.zip'
PYTHON_SHA256 = 'a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310'
# iced-x86: run.py scans the linked image for Haswell-only instructions on CPUs without MOVBE,
# BMI1 or LZCNT (docs/sacredfire/15-isa-rewrite.md). The cp38-abi3 wheel is a zip and loads on
# the bundled 3.14 without pip. Pinned by version and SHA-256.
ICED_X86_VERSION = '1.21.0'
ICED_X86_URL = ('https://files.pythonhosted.org/packages/04/9e/'
                'ab21c70472b605bbcaca5a181cfda56b0750b061f4a946192888142ec37c/'
                f'iced_x86-{ICED_X86_VERSION}-cp38-abi3-win_amd64.whl')
ICED_X86_SHA256 = '04e04a8cc49ca7d89448fb78e3bab4ae364cf1fc888c996540a791c9999077c1'
# Windows' own DLLs: never bundled.
SYSTEM_DLLS = {'kernel32', 'user32', 'gdi32', 'advapi32', 'shell32', 'ole32', 'oleaut32', 'ws2_32',
               'msvcrt', 'ntdll', 'bcrypt', 'bcryptprimitives', 'crypt32', 'secur32', 'winmm',
               'imm32', 'setupapi', 'version', 'shlwapi', 'comdlg32', 'comctl32', 'rpcrt4',
               'dwmapi', 'd3d11', 'dxgi', 'd3d9', 'd3d12', 'dxva2', 'mf', 'mfplat', 'mfuuid',
               'mfreadwrite', 'uxtheme', 'cfgmgr32', 'hid', 'opengl32', 'vulkan-1', 'iphlpapi',
               'mswsock', 'dnsapi', 'ncrypt', 'usp10', 'shcore', 'cabinet', 'wldap32', 'normaliz',
               'avrt', 'msimg32', 'propsys', 'windowscodecs', 'mmdevapi', 'powrprof', 'userenv',
               'psapi', 'netapi32', 'wsock32', 'd3dcompiler_47', 'gdiplus', 'd2d1', 'dwrite',
               'dinput8', 'xinput1_4', 'ucrtbase', 'vcruntime140', 'msvcp140', 'winspool',
               'cryptbase', 'sspicli', 'wintrust', 'rtworkq', 'ksuser', 'wevtapi', 'ntmarta'}


def imports(path):
    """DLL names a PE file imports (llvm-objdump from the CLANG64 toolchain)."""
    out = subprocess.run([str(CLANG_BIN / 'llvm-objdump.exe'), '-p', str(path)], capture_output=True,
                         text=True, errors='replace').stdout
    return {m.group(1).lower() for m in re.finditer(r'DLL Name:\s*(\S+)', out)}


def closure(binaries):
    """Every non-system DLL the binaries need, found in the CLANG64 bin folder."""
    found, queue = {}, list(binaries)
    while queue:
        for dll in imports(queue.pop()):
            stem = dll[:-4] if dll.endswith('.dll') else dll
            if dll in found or stem in SYSTEM_DLLS or dll.startswith(('api-ms-', 'ext-ms-')):
                continue
            path = CLANG_BIN / dll
            if not path.is_file():
                sys.exit(f'{dll} is needed but is not in {CLANG_BIN}')
            found[dll] = path
            queue.append(path)
    return found


def pacman(*args):
    return subprocess.run([str(MSYS2 / 'usr' / 'bin' / 'pacman.exe'), *args], capture_output=True,
                          text=True, errors='replace').stdout


DLSS_THIRD_PARTY = [
    'sacredfire_dlss.dll, in bin\\ (the DLSS bridge)',
    '  license: MIT (licenses\\dlss-bridge-LICENSE.txt), by IFreemz',
    '  source:  dlss_bridge\\ in this repository; from https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR',
    '  It links NVIDIA\'s DLSS SDK (NGX). sacredfire.exe contains no NVIDIA code: it loads the',
    '  bridge at run time when it is present.', '',
    'nvngx_dlss.dll, in bin\\ (NVIDIA DLSS)',
    '  license: NVIDIA RTX SDKs license (licenses\\NVIDIA-DLSS-SDK-LICENSE.txt), redistributed',
    '           unchanged as part of this application under its terms',
    '  home:    https://github.com/NVIDIA/DLSS',
    '  NVIDIA, the NVIDIA logo, DLSS and GeForce RTX are trademarks and/or registered trademarks of',
    '  NVIDIA Corporation in the U.S. and other countries.', '',
]


def third_party(dlls, python=None, dlss=False):
    """THIRD-PARTY.txt: the MSYS2 package, version, license and home page of every bundled DLL."""
    packages = {}
    for dll in sorted(dlls):
        owner = pacman('-Qo', f'/clang64/bin/{dll}').strip()
        m = re.search(r'is owned by (\S+) (\S+)', owner)
        packages.setdefault(m.group(1) if m else '(unknown)', {'version': m.group(2) if m else '', 'dlls': []})['dlls'].append(dll)
    lines = ['THIRD-PARTY SOFTWARE in bin\\',
             '=' * 60,
             'CTR-NitroFueled-PC itself is GPL-2.0-or-later (see LICENSE). The libraries below are',
             'MSYS2 CLANG64 packages, copied unchanged next to sacredfire.exe. Each keeps its own',
             'license; the package page lists the source and the build recipe',
             '(https://packages.msys2.org/ and https://github.com/msys2/MINGW-packages).', '']
    for package in sorted(packages):
        info = pacman('-Qi', package)
        field = lambda key: next((l.split(':', 1)[1].strip() for l in info.splitlines() if l.startswith(key)), '?')
        lines += [f'{package} {packages[package]["version"]}',
                  f'  license: {field("Licenses")}',
                  f'  home:    {field("URL")}',
                  f'  source:  https://packages.msys2.org/packages/{package}',
                  f'  files:   {", ".join(packages[package]["dlls"])}', '']
    if python:
        lines += [f'{python}, in python\\',
                  '  license: PSF-2.0 (python\\LICENSE.txt; also covers the bundled OpenSSL, libffi,',
                  '           SQLite and the Microsoft C runtime redistributables)',
                  '  home:    https://www.python.org/',
                  f'  source:  https://www.python.org/downloads/release/python-{PYTHON_VERSION.replace(".", "")}/', '']
        lines += [f'iced-x86 {ICED_X86_VERSION}, in python\\Lib\\site-packages\\ (run.py\'s ISA',
                  '  rewrite table for CPUs without MOVBE/BMI1/LZCNT, docs/sacredfire/15-isa-rewrite.md)',
                  '  license: MIT (https://github.com/icedland/iced/blob/master/LICENSE.txt)',
                  '  home:    https://github.com/icedland/iced',
                  f'  source:  https://pypi.org/project/iced-x86/{ICED_X86_VERSION}/#files', '']
    if dlss:
        lines += DLSS_THIRD_PARTY
    lines += ['sacredfire.exe includes AMD FidelityFX FSR 3.1 (MIT, through',
              'https://github.com/FireBurn/FSR-Vulkan).', '']
    lines += ['The launcher (CTR-NitroFueled-PC.exe) and sacredfire.exe include Dear ImGui (MIT,',
              'https://github.com/ocornut/imgui) and the other libraries listed in the repository.', '']
    lines += ['The game files are not included and are not covered by any of these licenses.',
              'Not affiliated with Sony, Activision or Beenox.', '']
    return '\r\n'.join(lines)


def bundle_python(folder, cache):
    """python/ in the package: python.org's embeddable Python, with scripts/ and tools/ on its
    path (its ._pth file replaces sys.path, and the scripts import their neighbours)."""
    archive = cache / Path(PYTHON_URL).name
    if not archive.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        print(f'Downloading {PYTHON_URL}')
        with urllib.request.urlopen(PYTHON_URL, timeout=120) as response:
            archive.write_bytes(response.read())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != PYTHON_SHA256:
        archive.unlink()
        sys.exit(f'{archive.name}: SHA-256 {digest}, expected {PYTHON_SHA256}')
    target = folder / 'python'
    with zipfile.ZipFile(archive) as z:
        z.extractall(target)
    pth = next(target.glob('python3*._pth'))
    zipped = next(target.glob('python3*.zip')).name
    pth.write_bytes(f'{zipped}\r\n.\r\n..\\scripts\r\n..\\tools\r\nLib\\site-packages\r\n'.encode())
    return f'Python {PYTHON_VERSION} (python.org embeddable package)'


def bundle_iced_x86(folder, cache):
    """python/Lib/site-packages in the package: iced-x86, which run.py needs to build the ISA
    rewrite table on CPUs without MOVBE/BMI1/LZCNT (docs/sacredfire/15-isa-rewrite.md). A wheel
    is a zip, so no pip is needed; the cp38-abi3 extension loads on the bundled Python."""
    wheel = cache / Path(ICED_X86_URL).name
    if not wheel.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        print(f'Downloading {ICED_X86_URL}')
        with urllib.request.urlopen(ICED_X86_URL, timeout=120) as response:
            wheel.write_bytes(response.read())
    digest = hashlib.sha256(wheel.read_bytes()).hexdigest()
    if digest != ICED_X86_SHA256:
        wheel.unlink()
        sys.exit(f'{wheel.name}: SHA-256 {digest}, expected {ICED_X86_SHA256}')
    target = folder / 'python' / 'Lib' / 'site-packages'
    prefix = f'iced_x86-{ICED_X86_VERSION}.dist-info'
    with zipfile.ZipFile(wheel) as z:
        for name in z.namelist():
            if name.split('/')[0] in ('iced_x86', prefix):
                z.extract(name, target)


def write_text(path, text):
    path.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8'))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--version', default='')
    p.add_argument('--dist', type=Path, default=ROOT / 'dist')
    p.add_argument('--dlss-sdk', type=Path, help='NVIDIA DLSS SDK folder: bundle DLSS')
    a = p.parse_args()
    dlss_files = []
    if a.dlss_sdk:
        dlss_files = [ROOT / 'dlss_bridge' / 'sacredfire_dlss.dll',
                      a.dlss_sdk / 'lib' / 'Windows_x86_64' / 'rel' / 'nvngx_dlss.dll']
        for f in dlss_files + [a.dlss_sdk / 'LICENSE.txt']:
            if not f.is_file():
                sys.exit(f'{f} not found (dlss_bridge\\build.bat builds the bridge)')
    exe, caps = ROOT / 'out' / 'sacredfire.exe', ROOT / 'out' / 'bb-gpu-capabilities.exe'
    launcher = ROOT / 'out' / LAUNCHER
    for f in (exe, caps, launcher):
        if not f.is_file():
            sys.exit(f'{f} not found: build with build.sh first')
    folder = a.dist / NAME
    shutil.rmtree(folder, ignore_errors=True)
    if folder.exists():
        sys.exit(f'{folder} could not be removed (is it open?)')
    for sub in ('bin', 'scripts', 'tools', 'patches', 'out'):
        (folder / sub).mkdir(parents=True)
    shutil.copy2(exe, folder / 'bin')
    shutil.copy2(caps, folder / 'bin')
    version = a.version and f'{a.version}-windows' or (ROOT / 'out' / 'VERSION').read_text().strip()
    (folder / 'bin' / 'VERSION').write_text(version + '\n')
    dlls = closure([exe, caps])
    for path in dlls.values():
        shutil.copy2(path, folder / 'bin')
    if dlss_files:
        for path in dlss_files:
            shutil.copy2(path, folder / 'bin')
        (folder / 'licenses').mkdir()
        shutil.copy2(a.dlss_sdk / 'LICENSE.txt', folder / 'licenses' / 'NVIDIA-DLSS-SDK-LICENSE.txt')
        write_text(folder / 'licenses' / 'dlss-bridge-LICENSE.txt',
                   (ROOT / 'dlss_bridge' / 'LICENSE.txt').read_text(encoding='utf-8'))
    shutil.copy2(launcher, folder)
    python = bundle_python(folder, a.dist / 'pycache')
    bundle_iced_x86(folder, a.dist / 'pycache')
    shutil.copy2(ROOT / 'run.py', folder)
    for script in (ROOT / 'scripts').glob('*.py'):
        shutil.copy2(script, folder / 'scripts')
    for tool in ('gen_import_names.py', 'merge_update.py'):
        shutil.copy2(ROOT / 'tools' / tool, folder / 'tools')
    shutil.copy2(ROOT / 'patches' / 'CrashTeamRacing.xml', folder / 'patches')
    if (ROOT / 'out' / 'aerolib.inl').is_file():
        shutil.copy2(ROOT / 'out' / 'aerolib.inl', folder / 'out')
    # bbport.ini is in .gitignore (it is the player's settings file), so the defaults live in
    # bbport.default.ini and ship under the real name.
    # The launcher sets up the game folder and starts it; the .bat is the way without it.
    # bbport.default.ini, not bbport.ini: unzipping a new version over an old one keeps the
    # player's settings (run.py and the launcher copy the defaults when bbport.ini is missing).
    for source, target in (('bbport.default.ini', 'bbport.default.ini'),
                           ('Jugar CTR Nitro-Fueled.bat', 'Jugar sin launcher.bat'),
                           ('LEEME.txt', 'LEEME.txt')):
        write_text(folder / target, (PORTABLE / source).read_text(encoding='utf-8'))
    write_text(folder / 'LICENSE', (ROOT / 'LICENSE').read_text(encoding='utf-8'))
    (folder / 'THIRD-PARTY.txt').write_bytes(third_party(dlls, python, bool(dlss_files)).encode('utf-8'))

    archive = a.dist / f'{NAME}-windows.zip'
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(folder.rglob('*')):
            if path.is_file():  # forward slashes in the entry names, as the ZIP format asks
                z.write(path, f'{NAME}/{path.relative_to(folder).as_posix()}')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest().upper()
    print(f'{archive}  {archive.stat().st_size / 1e6:.1f} MB  {len(dlls)} DLLs  {version}'
          f'{"  with DLSS" if dlss_files else ""}')
    print(f'SHA256 {digest}')
    (a.dist / f'{archive.name}.sha256').write_text(f'{digest}  {archive.name}\n')


if __name__ == '__main__':
    main()
