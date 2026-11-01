#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merges a dumped update into the base game without copying it: the game folder SacredFire runs.

    python tools/merge_update.py <base game folder> <update folder> <merged folder>

The merged folder holds the base game's files with the update's files over them (eboot.bin and
sce_sys included), as hard links to the originals: no extra disk space, and the two dumps stay as
they are. Hard links need the three folders on one drive; across drives the files are copied.
The game never writes to its own folder (/app0 is read-only in the runtime), so the originals are
not changed through the links.

Dumps extracted with PS4 PKG Tool keep the files in Image0/ and the sce_sys files in Sc0/; such a
folder is read in that layout and merged into the usual one (Image0/* at the root, Sc0/* in
sce_sys/), as the launcher does.
"""
import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path


def link_or_copy(source, target, counts):
    if target.exists() and os.path.samefile(source, target):
        counts['linked'] += 1
        return
    fd, staged = tempfile.mkstemp(prefix=f'.{target.name}.', dir=target.parent)
    os.close(fd)
    staged = Path(staged)
    staged.unlink()
    try:
        try:
            os.link(source, staged)
            counts['linked'] += 1
        except OSError:
            shutil.copy2(source, staged)
            counts['copied'] += 1
        os.replace(staged, target)
    finally:
        staged.unlink(missing_ok=True)


def layout(folder):
    """The (source, destination prefix) trees of a dump, or None when it has no game. A folder
    fixed by hand (eboot.bin copied next to Image0) counts as flat only with sce_module in it."""
    folder = Path(folder)
    image = folder / 'Image0'
    flat = (folder / 'eboot.bin').is_file() and (folder / 'sce_sys/param.sfo').is_file()
    if flat and (not (image / 'eboot.bin').is_file() or (folder / 'sce_module').is_dir()):
        return [(folder, Path())]
    if not (image / 'eboot.bin').is_file():
        return None
    for sys_dir in (image / 'sce_sys', folder / 'Sc0', folder / 'sce_sys'):
        if (sys_dir / 'param.sfo').is_file():
            return [(image, Path())] + ([] if sys_dir == image / 'sce_sys' else [(sys_dir, Path('sce_sys'))])
    return None


def add_tree(root, out, counts, replaced=None, prefix=Path()):
    for directory, _subdirs, files in os.walk(root):
        relative = prefix / Path(directory).relative_to(root)
        (out / relative).mkdir(parents=True, exist_ok=True)
        for name in files:
            target = out / relative / name
            if replaced is not None and target.exists():
                replaced.append(str(relative / name))
            link_or_copy(Path(directory) / name, target, counts)


def merge_game(base, update, out):
    base, update, out = Path(base), Path(update), Path(out)
    trees = {}
    for folder in (base, update):
        trees[folder] = layout(folder)
        if trees[folder] is None:
            raise ValueError(f'No eboot.bin and sce_sys/param.sfo (nor Image0 and Sc0) in {folder}')
    if out.resolve() in (base.resolve(), update.resolve()):
        raise ValueError('The merged folder must be a new folder')
    out.mkdir(parents=True, exist_ok=True)
    counts = {'linked': 0, 'copied': 0}
    replaced = []
    for root, prefix in trees[base]:
        add_tree(root, out, counts, prefix=prefix)
    for root, prefix in trees[update]:
        add_tree(root, out, counts, replaced, prefix)
    return counts, replaced


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('base', type=Path)
    p.add_argument('update', type=Path)
    p.add_argument('out', type=Path)
    a = p.parse_args()
    try:
        counts, replaced = merge_game(a.base, a.update, a.out)
    except ValueError as error:
        sys.exit(str(error))
    print(f'Merged {a.update} over {a.base} into {a.out}: {counts["linked"]} files linked, '
          f'{counts["copied"]} copied, {len(replaced)} replaced by the update')


if __name__ == '__main__':
    main()
