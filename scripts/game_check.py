#!/usr/bin/env python3
"""The game files this port runs, keyed on the executable's loaded image.

Profile: Crash Team Racing Nitro-Fueled CUSA13795 with the 1.21 update merged in. The check
compares the loaded executable image, whose hash is the same whatever tool dumped it (the SELF
headers around it differ). BB_SKIP_GAME_CHECK=1 skips it. bbport's Bloodborne profile was removed
on 2026-10-09.

Two builds of the 1.21 update exist: the one rebuilt for firmware 5.05 (sdk_ver=05050000 in
param.sfo; the image in SUPPORTED, which the port was developed on) and the original one for
firmware 6.72+. Same game, different executable. A 1.21 executable that is not a known old one is
accepted as `variant='6.72+'`: patches.py finds its patch sites by signature instead of address,
and the log says it is the less-tested build.
"""
import hashlib
import os
import struct
import sys
from pathlib import Path

# image sha256 -> profile. `name` is what the runtime and the GPU menu read (BB_GAME_PROFILE);
# `modules` are the sce_module PRXs link_modules.py links natively; `patches` says whether
# patches.py has a patch file for it.
SUPPORTED = {
    '2f2115893ac6656925d20a0221f118cff398fbd754cb9c10154451411dbcf8e6': dict(
        name='ctr', title='CUSA13795', version='01.21', game='Crash Team Racing Nitro-Fueled', update='CUSA13795-patch',
        modules=('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'), patches=True),
}
BY_TITLE = {p['title']: p for p in SUPPORTED.values()}
# Images that are known NOT to be 1.21: the base game's executable (1.00). With a 1.21 param.sfo
# these mean the update's eboot.bin was not copied in.
OLD_IMAGES = {
    'a9cdc80f2170624e08bdddab128fc9cdaf5c71aa90c00fc669d830e366ba6833': '01.00',
}
SUPPORTED_TITLE = 'CUSA13795'
SUPPORTED_VERSION = '01.21'
SUPPORTED_IMAGE = '2f2115893ac6656925d20a0221f118cff398fbd754cb9c10154451411dbcf8e6'


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span  # same directory
    elf, header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def profile(game, image_hash=None):
    """The profile dict for a supported game folder, else None (BB_SKIP_GAME_CHECK=1: a profile
    guessed from param.sfo's TITLE_ID when one exists, so run.sh can still branch)."""
    try:
        image_hash = image_hash or image_sha256(game)
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        image_hash = None
    if image_hash in SUPPORTED:
        return dict(SUPPORTED[image_hash], variant='5.05')
    if image_hash and problem(game, image_hash) is None and os.environ.get('BB_SKIP_GAME_CHECK') != '1':
        info = sfo_info(game)
        return dict(BY_TITLE[info['TITLE_ID']], variant='6.72+')
    if os.environ.get('BB_SKIP_GAME_CHECK') != '1':
        return None
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        return None
    return BY_TITLE.get(info.get('TITLE_ID'))


def sfo_info(game):
    from prepare import sfo
    try:
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        return {}


def split_layout(game):
    """True for a PS4 PKG Tool dump (the files in Image0, sce_sys in Sc0) that was not flattened,
    including one fixed by hand only as far as eboot.bin and param.sfo."""
    game = Path(game)
    return (game / 'Image0/eboot.bin').is_file() and not (game / 'sce_module').is_dir()


def damaged_archives(game):
    """The archives/*.pak (IGA) files whose file-name table is all zeros. An extractor that
    inflates the package's compressed PFS blocks only partly (LibOrbisPkg's PFSCReader on .NET Core
    and later: one DeflateStream.Read per 64 KiB block) leaves zeros at the ends of blocks; where
    they land on an archive's name table the game cannot find its files, and with loosefiles.pak
    (localizationData.igz) it stops at start with a read of address 0x20 (code 3)."""
    damaged = []
    for path in sorted((Path(game) / 'archives').glob('*.pak')):
        try:
            with open(path, 'rb') as f:
                header = f.read(0x38)
                if len(header) < 0x38 or header[:4] != b'IGA\x1a':
                    continue
                offset, size = struct.unpack_from('<I', header, 0x28)[0], struct.unpack_from('<I', header, 0x30)[0]
                if not size:
                    continue
                f.seek(offset)
                names = f.read(size)
        except OSError:
            continue
        if len(names) == size and not any(names):
            damaged.append(path.name)
    return damaged


def problem(game, image_hash=None):
    """None for a supported game, else (kind, title, version): kind is 'missing_update' (base
    game or an older update), 'wrong_eboot' (param.sfo names the supported update, eboot.bin is
    another version), 'split_layout' (Image0 + Sc0 not flattened), 'other_title' (another edition, region or game),
    'unreadable' or 'damaged_archives' (archives with zeroed name tables: a bad extraction)."""
    if os.environ.get('BB_SKIP_GAME_CHECK') == '1':
        return None
    if split_layout(game):
        return 'split_layout', '?', '?'
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    try:
        image_hash = image_hash or image_sha256(game)
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return 'unreadable', title, version
    if image_hash not in SUPPORTED:
        if title not in BY_TITLE:
            return 'other_title', title, version
        if version != BY_TITLE[title]['version']:
            return 'missing_update', title, version
        if image_hash in OLD_IMAGES:
            return 'wrong_eboot', title, version
        # otherwise another 1.21 build (6.72+): accepted, see the module docstring
    if damaged_archives(game):
        return 'damaged_archives', title, version
    return None


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f'Found {title} version {version} (sce_sys/param.sfo).'
    p = BY_TITLE.get(title, BY_TITLE[SUPPORTED_TITLE])
    supported = ', '.join(f"{q['game']} {q['title']} {q['version']}" for q in SUPPORTED.values())
    return {
        'missing_update': f"{found} The port needs the {p['version']} update merged into the game folder: copy "
                          f"everything from the dumped {p['version']} update (e.g. {p['update']}) into the "
                          'game folder, replacing files (eboot.bin and sce_sys too).',
        'wrong_eboot': f"{found} param.sfo is from {p['version']} but eboot.bin is not: copy eboot.bin from "
                       f"the dumped {p['version']} update into the game folder, replacing the old one.",
        'other_title': f'{found} Supported: {supported}; other editions and regions have a different executable.',
        'split_layout': 'The game folder has the PS4 PKG Tool layout (Image0 and Sc0): choose it in the '
                        'launcher and press "Prepare the game", or copy everything in Image0 to the game folder '
                        'and everything in Sc0 to its sce_sys folder.',
        'unreadable': f'{found} eboot.bin could not be read as a decrypted PS4 executable: dump '
                      'the game and its update again.',
        'damaged_archives': f'{found} Some archives/*.pak files have their file-name table filled with zeros, '
                            'so the game cannot find its files (it would stop at start with code 3). The '
                            'package was extracted badly: extract the game and its update again with another '
                            'tool, or with a LibOrbisPkg/PkgTool build that reads every compressed PFS block '
                            'completely, then merge them again.',
    }[kind]


if __name__ == '__main__':
    # game_check.py GAME_DIR: prints the profile name (run.sh branches on it), or fails with the reason.
    game = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
    found = problem(game)
    if found:
        message = 'Unsupported game files: ' + explain(*found)
        if found[0] == 'damaged_archives':
            damaged = damaged_archives(game)
            message += f' Damaged ({len(damaged)}): ' + ', '.join(damaged[:10]) + (', ...' if len(damaged) > 10 else '')
        sys.exit(message)
    p = profile(game)
    print(p['name'] if p else 'unknown')

