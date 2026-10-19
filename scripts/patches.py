"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's image places eboot
vaddr 0 at image offset 0. Only literal writes are supported (bytes, bytes16/32/64, float32/64,
utf8, utf16); pattern ("mask") patches are rejected and the patch is skipped whole.

A line may carry Signature="41 8b ?? ..." (hex bytes, ?? for any byte) and SignatureOffset (where
the write goes, from the signature's start). The write goes to Address when the signature matches
there (the 1.21 build for firmware 5.05); otherwise to the one place in the executable where it
matches (the 6.72+ build, whose code sits at other addresses). No match, or more than one: the
patch is skipped whole, with a log line.

Patch files: patches/*.xml in the checkout (patches/CrashTeamRacing.xml, from
illusionyy/PS-Game-Patch) and any folder named with --patches-dir. A patch is applied when the
in-game menu or the launcher turned its bbport.ini key on (GAME_PATCH_KEYS), when patches.json
lists it under "enabled", or when its file marks it isEnabled and patches.json does not disable it.
"""
import argparse
import json
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
TITLE_ID='CUSA13795'
APP_VERSION='01.21'
# bbport.ini keys (the in-game menu, the launcher) -> "<file>/<patch name>". The patches are
# community work from illusionyy/PS-Game-Patch (https://github.com/illusionyy/PS-Game-Patch):
# 60 FPS Unlock and Disable Motion Blur by illusion, the 1080p Pro resolution patch by TroyWarez.
GAME_PATCH_KEYS={'patch_60fps':'CrashTeamRacing.xml/60 FPS Unlock',
                 'patch_no_motion_blur':'CrashTeamRacing.xml/Disable Motion Blur',
                 'patch_pro_1080p':'CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)'}
# The bundled files of other editions (the EU one, CUSA14876): their patches are selected under
# CrashTeamRacing.xml's keys, so the menu switches and patches.json work for both.
BUNDLED_FILES=('CrashTeamRacingEU.xml',)


def selection_key(key):
    """key as the menu and patches.json name it (an edition's bundled file as CrashTeamRacing.xml)."""
    name,sep,patch=key.partition('/')
    return 'CrashTeamRacing.xml/'+patch if sep and name in BUNDLED_FILES else key


# bbport.ini render_res: the scene size the game renders in PS4 Pro mode (run.py turns the Pro
# mode on for these), via the game's own render scale (docs/sacredfire/13-resolution.md).
# 1080: the base PS4 path. 1440: the Pro's own scale. Others: the
# scale read at 0x17aa662 is replaced by a constant; 2160 also needs a larger GPU heap.
RENDER_RESOLUTIONS=('480','720','1080','1440','1620','2160')
RENDER_SCALE_SIGNATURE='41 8b 44 24 48 41 89 86 48 05 00 00'  # mov 0x48(%r12),%eax; mov %eax,0x548(%r14)
GPU_HEAP_SIGNATURE='c7 45 c8 00 00 00 3c e9'                  # movl $0x3c000000,-0x38(%rbp); jmp
RENDER_SCALES={'480':480/1080,'720':720/1080,'1620':1.5,'2160':2.0}
# Below 1080p the picture stays 1920x1080: the Pro path's doubling of the output size is removed.
OUTPUT_DOUBLE_SIGNATURE='01 c9 41 89 8e 40 05 00 00 45 8b 86 44 05 00 00 45 01 c0'


def render_res(settings):
    """The render_res setting, or None without one (older settings: patch_pro_1080p decides)."""
    value=(settings or {}).get('render_res')
    return value if value in RENDER_RESOLUTIONS else None


# temporal=dlss with dlss_quality set: DLSS picks the render size from the output (dlss_output,
# 1080 or 2160) and NVIDIA's quality ratios, as PC games do.
DLSS_QUALITIES={'dlaa':1.0,'quality':2/3,'balanced':0.58,'performance':0.5,'ultra_performance':1/3}


def render_plan(settings):
    """How the game renders, from bbport.ini: a dict with `label`, `scale` (the render scale
    relative to 1920x1080 patched at 0x17aa662, None for the game's own), `pro` (the PS4 Pro path,
    BB_NEO=1), `single` (the Pro path's output doubling removed: a 1080p picture) and `heap` (the
    larger GPU heap, needed above a 1.5 scale), or None when nothing is set."""
    s=settings or {}
    quality=s.get('dlss_quality')
    if s.get('temporal')=='dlss' and quality in DLSS_QUALITIES:
        out=2160 if s.get('dlss_output')=='2160' else 1080
        scale=out/1080*DLSS_QUALITIES[quality]
        label=f'DLSS {quality.replace("_"," ")}, {round(1080*scale)}p to {out}p'
        if out==1080 and abs(scale-1.0)<1e-6:
            return dict(label=label,scale=None,pro=False,single=False,heap=False)
        return dict(label=label,scale=scale,pro=True,single=out==1080,heap=scale>1.5+1e-6)
    res=render_res(s)
    if res is None:
        return None
    if res=='1080':
        return dict(label='1080p',scale=None,pro=False,single=False,heap=False)
    return dict(label=f'{res}p',scale=RENDER_SCALES.get(res),pro=True,single=res in ('480','720'),
                heap=res=='2160')


def render_patches(settings):
    """Built-in patch elements for the render plan (a list of (key, None, Metadata element))."""
    plan=render_plan(settings)
    if plan is None:
        return []
    lines=[]
    if plan['scale'] is not None:
        value='b8'+struct.pack('<f',plan['scale']).hex()
        lines.append(dict(Type='bytes',Address='0x017aa662',Value=value,
                          Signature=RENDER_SCALE_SIGNATURE,SignatureOffset='0x0'))
    if plan['single']:
        lines.append(dict(Type='bytes',Address='0x017aa648',Value='9090',
                          Signature=OUTPUT_DOUBLE_SIGNATURE,SignatureOffset='0x0'))
        lines.append(dict(Type='bytes',Address='0x017aa658',Value='909090',
                          Signature=OUTPUT_DOUBLE_SIGNATURE,SignatureOffset='0x10'))
    if plan['heap']:
        # 960 MiB -> 1792 MiB (0x70000000): native 4K with 2x MSAA does not fit the Pro's heap.
        lines.append(dict(Type='bytes',Address='0x01fbb93e',Value='70',
                          Signature=GPU_HEAP_SIGNATURE,SignatureOffset='0x6'))
    if not lines:
        return []
    meta=ET.Element('Metadata',Name=f'Render resolution {plan["label"]}',AppVer=APP_VERSION,
                    AppElf='eboot.bin')
    patch_list=ET.SubElement(meta,'PatchList')
    for line in lines:
        ET.SubElement(patch_list,'Line',**line)
    return [(f'render/{plan["label"]}',None,meta)]


def read_settings(path):
    """bbport.ini as a dict (key=value lines; # comments)."""
    settings={}
    if path.exists():
        for line in path.read_text().splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def eboot_segments(elf):
    """(start, end) of eboot.elf's load segments: patches must land inside one."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def load_segments(elf):
    """[(vaddr, file offset, file size)] of eboot.elf's load segments (signature search)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    found=[]
    for i in range(phnum):
        kind,_,offset,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: found.append((vaddr,offset,filesz))
    return found


def signature_regex(text):
    tokens=text.split()
    if not tokens: raise ValueError('empty signature')
    return re.compile(b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in tokens),re.S)


def locate(line, offset, elf):
    """The image offset a signed line writes to (see the module docstring)."""
    rx=signature_regex(line.get('Signature'))
    shift=int(line.get('SignatureOffset','0'),0)
    loads=load_segments(elf)
    def at(vaddr):
        for start,pos,size in loads:
            if start<=vaddr and vaddr-start<size: return pos+vaddr-start
    pos=at(offset-shift)
    if pos is not None and rx.match(elf,pos): return offset
    hits=[]
    for start,pos,size in loads:
        hits+=[start+m.start()-pos for m in rx.finditer(elf,pos,pos+size)]
        if len(hits)>1: break
    if len(hits)!=1:
        raise ValueError(f'patch site for {line.get("Address")} not found ({len(hits)} signature matches)')
    return hits[0]+shift


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def external_patches(directory, app_version=APP_VERSION, title_id=TITLE_ID):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json). Files whose
    <TitleID> list does not name title_id are skipped (a file without one applies to any game)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and title_id not in ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config, ini=None):
    """Selected patches: patches.json {"enabled": [...], "disabled": [...]} overrides each file's
    isEnabled; bbport.ini patch_* keys (GAME_PATCH_KEYS, the in-game menu) add to it."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    for key,patch in GAME_PATCH_KEYS.items():
        if (ini or {}).get(key)=='1': enabled.add(patch); disabled.discard(patch)
    # The 1080p Pro patch follows render_res (Settings > Internal resolution, the in-game menu).
    pro_1080p=GAME_PATCH_KEYS['patch_pro_1080p']
    # With render_res the Pro mode follows it (run.py): 1080p runs the base PS4 path, so the
    # 1080p Pro patch is never needed (on the base path its flipped branch would take the Pro one).
    if render_plan(ini): enabled.discard(pro_1080p); disabled.add(pro_1080p)
    return [(key,path,meta) for key,path,meta in found
            if selection_key(key) in enabled
            or (selection_key(key) not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments, elf=None):
    """Writes of the selected patches; a patch with unsupported lines is skipped whole. elf (the
    eboot.elf bytes) is needed for lines with a Signature."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if line.get('Signature'):
                    if elf is None: raise ValueError('signature without the executable')
                    moved=locate(line,offset,elf)
                    if moved!=offset: print(f'Patches: {key}: {line.get("Address")} found at {moved+EBOOT_BASE:#010x}')
                    offset=moved
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_blob(out, writes):
    """out/patches.bin (BBPATCH2: the patch base, so the loader can rebase pointers written into
    relocated slots)."""
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def main():
    root=Path(__file__).resolve().parent.parent
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,default=root/'patches',help='patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled patches')
    p.add_argument('--app-version',default=APP_VERSION)
    p.add_argument('--title-id',default=TITLE_ID,help='the game (patch files naming other ids are skipped)')
    p.add_argument('--out',type=Path,default=root/'out')
    p.add_argument('--settings',type=Path,default=root/'ctrnfpc.ini')
    p.add_argument('--external-only',action='store_true',help='accepted for older callers (the only mode)')
    a=p.parse_args()
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    found=external_patches(a.patches_dir,a.app_version,a.title_id)
    settings=read_settings(a.settings)
    writes=compile_external(external_selection(found,a.patches_config,settings)+render_patches(settings),
                            segments,elf)
    write_blob(a.out,writes)
    print(f'Patches: {len(writes)} writes from {len(found)} available patches')


if __name__=='__main__':
    main()
