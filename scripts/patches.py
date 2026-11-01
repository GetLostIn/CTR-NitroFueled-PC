"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's image places eboot
vaddr 0 at image offset 0. Only literal writes are supported (bytes, bytes16/32/64, float32/64,
utf8, utf16); pattern ("mask") patches are rejected and the patch is skipped whole.

A line may carry Signature="41 8b ?? ..." (hex bytes, ?? for any byte) and SignatureOffset (where
the write goes, from the signature's start). The write goes to Address when the signature matches
there (the 1.21 build for firmware 5.05); otherwise to the one place in the executable where it
matches (the 6.72+ build, whose code sits at other addresses). No match, or more than one: the
patch is skipped whole, with a log line.

ExpectedSHA256="..." verifies the original bytes across the entire write length in a
file-backed load segment. It does not search/relocate fixed-address injected code.
Declarative mod writes must carry Signature and/or ExpectedSHA256; legacy built-in
patches keep their existing behavior. Whole-file executable hashes are not gates.

Patch files: patches/*.xml in the checkout (patches/CrashTeamRacing.xml, from
illusionyy/PS-Game-Patch) and any folder named with --patches-dir. A patch is applied when the
in-game menu or the launcher turned its bbport.ini key on (GAME_PATCH_KEYS), when patches.json
lists it under "enabled", or when its file marks it isEnabled and patches.json does not disable it.
"""
import hashlib
import argparse
import json
import os
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
# 60 FPS Unlock and Disable Motion Blur by illusion, the 1080p Pro resolution patch by TroyWarez;
# Unlock Everything by Camiloop.
GAME_PATCH_KEYS={'patch_60fps':'CrashTeamRacing.xml/60 FPS Unlock',
                 'patch_no_motion_blur':'CrashTeamRacing.xml/Disable Motion Blur',
                 'patch_pro_1080p':'CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)',
                 'patch_unlock_all':'CrashTeamRacingUnlockAll.xml/Unlock Everything',
                 'patch_online_key':'CrashTeamRacingOnline.xml/Online: custom Demonware key'}
# The bundled files of other editions: their patches are selected under the USA files' keys.
BUNDLED_FILES={'CrashTeamRacingEU.xml':'CrashTeamRacing.xml',
               'CrashTeamRacingUnlockAllEU.xml':'CrashTeamRacingUnlockAll.xml',
               'CrashTeamRacingOnlineEU.xml':'CrashTeamRacingOnline.xml'}


def selection_key(key):
    """key as the menu and patches.json name it (an edition's bundled file as the USA one)."""
    name,sep,patch=key.partition('/')
    return BUNDLED_FILES[name]+'/'+patch if sep and name in BUNDLED_FILES else key


# bbport.ini render_res: the scene size the game renders in PS4 Pro mode (run.py turns the Pro
# mode on for these), via the game's own render scale (docs/sacredfire/13-resolution.md).
# 1080: the base PS4 path. 1440: the Pro's own scale. Others: the
# scale read at 0x17aa662 is replaced by a constant; 2160 also needs a larger GPU heap.
RENDER_RESOLUTIONS=('480','720','1080','1440','1620','2160')
RENDER_SCALE_SIGNATURE='41 8b 44 24 48 41 89 86 48 05 00 00'  # mov 0x48(%r12),%eax; mov %eax,0x548(%r14)
GPU_HEAP_SIGNATURE='c7 45 c8 00 00 00 3c e9'                  # movl $0x3c000000,-0x38(%rbp); jmp
RENDER_SCALES={'480':480/1080,'720':720/1080,'1620':1.5,'2160':2.0}
# Below 1080p the picture stays 1920x1080: the Pro path's doubling of the output size is removed.
# Ultrawide (bbport.ini ultrawide=1, ultrawide_size=WxH): the display setup's two loads of the
# output size from the game's settings (mov 0x10(%r12),%eax and mov 0x14(%r12),%eax, 5 bytes each)
# become `mov $W,%eax` and `mov $H,%eax`; the base PS4 path then draws the scene at that size and
# the camera follows its aspect. Found in the EU 1.00 executable (0x12e6bc1).
OUTPUT_SIZE_SIGNATURE=('41 8b 44 24 10 85 c0 7f 05 41 8b 44 24 08 41 89 86 40 05 00 00 '
                       '41 8b 44 24 14 85 c0 7f 05 41 8b 44 24 0c 41 89 86 44 05 00 00 '
                       '41 8b 44 24 18 41 89 86 88 04 00 00')
OUTPUT_DOUBLE_SIGNATURE='01 c9 41 89 8e 40 05 00 00 45 8b 86 44 05 00 00 45 01 c0'


def render_res(settings):
    """The render_res setting, or None without one (older settings: patch_pro_1080p decides)."""
    value=(settings or {}).get('render_res')
    return value if value in RENDER_RESOLUTIONS else None


# temporal=dlss with dlss_quality set: DLSS picks the render size from the output (dlss_output,
# 1080 or 2160) and NVIDIA's quality ratios, as PC games do.
DLSS_QUALITIES={'dlaa':1.0,'quality':2/3,'balanced':0.58,'performance':0.5,'ultra_performance':1/3}


def ultrawide_size(settings):
    """(width, height) when ultrawide is on (bbport.ini ultrawide=1 with ultrawide_size=WxH), else None."""
    s=settings or {}
    if s.get('ultrawide')!='1': return None
    w,sep,h=s.get('ultrawide_size','').partition('x')
    if not (sep and w.isdigit() and h.isdigit()): return None
    w,h=int(w),int(h)
    return (w,h) if 640<=w<=16384 and 360<=h<=8640 else None


def render_plan(settings):
    """How the game renders, from bbport.ini: a dict with `label`, `scale` (the render scale
    relative to 1920x1080 patched at 0x17aa662, None for the game's own), `pro` (the PS4 Pro path,
    BB_NEO=1), `single` (the Pro path's output doubling removed: a 1080p picture) and `heap` (the
    larger GPU heap, needed above a 1.5 scale), or None when nothing is set."""
    s=settings or {}
    if size:=ultrawide_size(s):
        return dict(label=f'ultrawide {size[0]}x{size[1]}',scale=None,pro=False,single=False,heap=False,
                    output=size)
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
    layout=s.get('layout_internal')
    if res=='1080' and layout in RENDER_RESOLUTIONS and layout!='1080':
        # An aspect ratio patch writes the display size itself (WxH at 0x17a9d61), but the 3D
        # targets are 1920x1080 times the scale getter at 0x13e2eb0, which returns 1.0 on the base
        # path. So the scene is made larger as at 16:9: the Pro path with the scale, and without
        # its doubling of the output, which would double the layout's size.
        return dict(label=f'{layout}p {s.get("aspect_ratio", "ultrawide").replace("x", ":")}',scale=RENDER_SCALES.get(layout),pro=True,single=True,
                    heap=layout=='2160')
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
    if plan.get('output'):
        w,h=plan['output']
        lines.append(dict(Type='bytes',Address='0x016e6bc1',Value='b8'+struct.pack('<I',w).hex(),
                          Signature=OUTPUT_SIZE_SIGNATURE,SignatureOffset='0x0'))
        lines.append(dict(Type='bytes',Address='0x016e6bd6',Value='b8'+struct.pack('<I',h).hex(),
                          Signature=OUTPUT_SIZE_SIGNATURE,SignatureOffset='0x15'))
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


# Season events: the Grand Prix "hidden object" events still in the game (LooseData/
# OctaneHiddenObjectGameplayData.igz in update2.pak): ghosts (the Spooky Grand Prix) or golden Wumpa
# fruit that escape from crates, to be caught during races. The online config the official server
# sent (NetConfig, CNetworkConfig._hiddenObjectMode) chose one; ours sends none, so the event is off.
# On each level load the hidden-object manager (0xfb5f50 in eboot.elf) reads that mode from the
# config ([[config+0x130]+0x18]) and calls its setMode (0xfb7710); the patch hands setMode a
# constant instead: mov rdi,r13; mov esi,<mode>; 5-byte nop, in place of the 13-byte read.
# EOctaneHiddenObjectGameplayModes: eOOGM_None 0, eOOGM_SpookyGhost 1, eOOGM_GoldenWumpa 2 (the
# game itself swaps golden Wumpa for ghosts on Twilight Tour). BB_THEME (run.py: the season the
# online server announces) or bbport.ini theme= picks it.
THEME_VALUES={'default':0,'halloween':1,'golden':2}
THEME_SIGNATURE='49 8b 87 30 01 00 00 4c 89 ef 8b 70 18 e8'


def theme_patches(settings):
    """The built-in patch element for the season event (none for the normal season)."""
    theme=(os.environ.get('BB_THEME') or (settings or {}).get('theme') or 'default').strip().lower()
    value=THEME_VALUES.get(theme,0)
    if not value:
        return []
    meta=ET.Element('Metadata',Name=f'Season event {theme}',AppVer=APP_VERSION,AppElf='eboot.bin')
    ET.SubElement(ET.SubElement(meta,'PatchList'),'Line',Type='bytes',Address='0x013b601b',
                  Value=f'4c89efbe{value:02x}0000000f1f440000',Signature=THEME_SIGNATURE,
                  SignatureOffset='0x0')
    return [(f'theme/{theme}',None,meta)]


# Online race end: once the first player finishes, the others have CNetworkConfig._endRaceTimeout
# seconds ("ends in" countdown). The official NetConfig set it; without one the game keeps the
# default its type registration gives: 20.0, in the field's setDefault (eboot.elf 0x196b610,
# loaded at 0x1d6b610: mov dword [rbp-34h],<float>). BB_END_RACE_TIMEOUT (run.py: what the
# RyuNET server announces) replaces that float.
END_RACE_SIGNATURE='4b 8b bc fe 60 02 00 00 c7 45 cc 00 00 a0 41'


def end_race_patches():
    """The built-in patch element for the online race-end countdown (none for the default 20 s)."""
    try:
        seconds=float(os.environ.get('BB_END_RACE_TIMEOUT') or 20)
    except ValueError:
        return []
    if seconds==20 or not 10<=seconds<=120:
        return []
    value=struct.pack('<f',seconds).hex()
    meta=ET.Element('Metadata',Name=f'Online race end {seconds:g} s',AppVer=APP_VERSION,AppElf='eboot.bin')
    ET.SubElement(ET.SubElement(meta,'PatchList'),'Line',Type='bytes',Address='0x01d6b610',
                  Value='4b8bbcfe60020000c745cc'+value,Signature=END_RACE_SIGNATURE,SignatureOffset='0x0')
    return [(f'end_race/{seconds:g}',None,meta)]


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


def reflected_fields(image,rel,field):
    """The fields of the Alchemy class that has `field` (bytes, a field name), as (name -> u16
    offset, metadata slots), or None. The class's field registration gets its field names (lea
    rsi), per-field metadata slots (lea rdx) and u16 offsets (lea rcx)."""
    def text(a):
        end=image.find(b'\0',a)
        return image[a:end] if 0<=a<len(image) and end>a else b''
    name=image.find(b'\0'+field+b'\0')
    if name<0: return None
    name+=1
    slots=[t for t,v in rel.items() if v==name]
    if not slots: return None
    start=slots[0]
    while text(rel.get(start-8,-1)).startswith(b'_'): start-=8
    fields=[]
    while text(rel.get(start+8*len(fields),-1)).startswith(b'_'): fields.append(text(rel[start+8*len(fields)]).decode())
    def lea_target(at,opcode):
        for back in range(at,at+0x30):
            if image[back:back+3]==opcode:
                return back+7+struct.unpack_from('<i',image,back+3)[0]
    i=image.find(b'\x48\x8d\x35')
    while i!=-1 and i<len(image)-7:
        if i+7+struct.unpack_from('<i',image,i+3)[0]==start:
            storage,offsets=lea_target(i,b'\x48\x8d\x15'),lea_target(i,b'\x48\x8d\x0d')
            if storage is None or offsets is None: return None
            offs=struct.unpack_from(f'<{len(fields)}H',image,offsets)
            return dict(zip(fields,offs)),[rel.get(storage+8*k,0) for k in range(len(fields))]
        i=image.find(b'\x48\x8d\x35',i+1)
    return None


def boot_image(out):
    """(image, relocations) of the prepared image out/boot.bin: the loaded image and its absolute
    relocations (target -> value)."""
    data=Path(out,'boot.bin').read_bytes()
    _magic,size,_entry,nl,nr,nn,_=struct.unpack_from('<8sQQQQQQ',data,0)
    pos=56+nl*24+nn*128
    rel={}
    for i in range(nr):
        target,kind,value,_=struct.unpack_from('<QQqq',data,pos+i*32)
        if kind==0: rel[target]=value
    return data[pos+nr*32:pos+nr*32+size],rel


def registered_vtable(image,rel,name):
    """Image offset of an Alchemy class's vtable from its registration in the prepared image, or
    None: the code that loads the class name (lea r8,[rip+name]) loads, a little before, tables of
    code pointers (lea reg,[rip+table]); the last one before the name is the class's vtable (an
    earlier one, when there are two, belongs to its meta object)."""
    text=image.find(b'\0'+name.encode()+b'\0')
    if text<0 or not rel: return None
    text+=1
    code_end=min(rel)  # the relocated pointers live in data, after the code
    i=image.find(b'\x4c\x8d\x05')
    while i!=-1 and i<len(image)-7:
        if i+7+struct.unpack_from('<i',image,i+3)[0]==text:
            found=None
            for back in range(max(i-0x120,0),i):
                if image[back]==0x48 and image[back+1]==0x8d and (image[back+2]&0xc7)==0x05:
                    t=back+7+struct.unpack_from('<i',image,back+3)[0]
                    if all(0<rel.get(t+8*k,-1)<code_end for k in range(3)): found=t
            if found is not None: return found
        i=image.find(b'\x4c\x8d\x05',i+1)
    return None


# The instruction that turns the render camera's field of view into degrees each frame: load the
# camera setup's radians (+0xd0), vmulss xmm0,[rip+57.29578], keep the aspect, store to the render
# camera's +0x3c. Found in the EU 1.21 executable (0xaf7278); src/runtime_frontend.c points it at a
# factor of its own.
FRONTEND_FOV_SIGNATURE=bytes.fromhex('c4c17a1085d0000000c5fa5905')
FRONTEND_FOV_AFTER=bytes.fromhex('c5fa1195f0feffffc5fa11433c')


FRONTEND_POSITION_LOAD=bytes.fromhex('c4c178109db0010000')  # vmovups xmm3,[r13+0x1b0]
FRONTEND_POSITION_STORE=bytes.fromhex('c5f8119ba0000000')   # vmovups [rbx+0xa0],xmm3


def frontend_state(out):
    """(instruction, CPreRenderUpdatePass vtable, slot) image offsets for the field of view of the
    title and customization screens in ultrawide (src/runtime_frontend.c), or None. The slot is a
    float in the unused tail of the image's last page."""
    image,rel=boot_image(out)
    sites=[]
    i=image.find(FRONTEND_FOV_SIGNATURE)
    while i!=-1:
        if image[i+len(FRONTEND_FOV_SIGNATURE)+4:i+len(FRONTEND_FOV_SIGNATURE)+4+len(FRONTEND_FOV_AFTER)]==FRONTEND_FOV_AFTER:
            sites.append(i+9)
        i=image.find(FRONTEND_FOV_SIGNATURE,i+1)
    if len(sites)!=1: return None
    fields=reflected_fields(image,rel,b'_cameraAspectRatioMultiplier')
    if not fields or fields[0].get('_cameraName')!=0x138 or fields[0].get('_renderCamera')!=0x168: return None
    vtable=registered_vtable(image,rel,'CPreRenderUpdatePass')
    slot=(len(image)+15)&~15
    if vtable is None or slot+4>(len(image)+0xfff)&~0xfff: return None
    # The same function stores the camera's world matrix; its last row (the position) is stored by
    # vmovups [rbx+0xa0],xmm3 after loading it from the camera setup (vmovups xmm3,[r13+0x1b0]).
    # The runtime lowers the camera there on the title screen (the logo then clears "Press X").
    near=image[sites[0]:sites[0]+0x100]
    load=near.find(FRONTEND_POSITION_LOAD)
    store=near.find(FRONTEND_POSITION_STORE,load) if load>=0 else -1
    position=sites[0]+store if store>=0 and store-load<0x30 else None
    return (sites[0],vtable,slot)+((position,) if position is not None else ())


def cutscene_state(out):
    """(instance slot, active field, designer field, flyby field or None) of CCutsceneManager in
    the prepared image (out/boot.bin: the loaded image and its relocations), or None. The static
    _Instance follows the last metadata slot. flyby: CCutsceneData's _isFlybyIntro (a bool), set
    for the camera fly-over before a race, which stays ultrawide. Used for 16:9 cutscenes in
    ultrawide."""
    data=Path(out,'boot.bin').read_bytes()
    _magic,size,_entry,nl,nr,nn,_=struct.unpack_from('<8sQQQQQQ',data,0)
    pos=56+nl*24+nn*128
    rel={}
    for i in range(nr):
        target,kind,value,_=struct.unpack_from('<QQqq',data,pos+i*32)
        if kind==0: rel[target]=value
    image=data[pos+nr*32:pos+nr*32+size]
    manager=reflected_fields(image,rel,b'_activeCutsceneData')
    if not manager: return None
    by_name,meta=manager
    if '_activeDesignerCutscene' not in by_name: return None
    cutscene=reflected_fields(image,rel,b'_isFlybyIntro')
    flyby=cutscene[0].get('_isFlybyIntro') if cutscene else None
    return max(meta)+8,by_name['_activeCutsceneData'],by_name['_activeDesignerCutscene'],flyby


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


def locate_expected(line, offset, elf):
    """The image offset a line with Expected="..." writes to (checking Address and AlternateAddress)."""
    expected=bytes.fromhex(line.get('Expected').replace(' ',''))
    loads=load_segments(elf)
    def matches(vaddr):
        for start,pos,size in loads:
            if start<=vaddr and vaddr+len(expected)-start<=size:
                return elf[pos+vaddr-start:pos+vaddr-start+len(expected)]==expected
        return False
    candidates=[offset]
    for token in (line.get('AlternateAddress') or '').replace(',',' ').split():
        alt=int(token,0)-EBOOT_BASE
        if alt not in candidates: candidates.append(alt)
    hits=[cand for cand in candidates if matches(cand)]
    if len(hits)!=1:
        raise ValueError(f'patch site for {line.get("Address")} not verified ({len(hits)} expected matches)')
    return hits[0]


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


def all_external_patches(directories, app_version=APP_VERSION, title_id=TITLE_ID):
    """external_patches of several folders (the bundled patches/, then the player's own: the Linux
    package's is read-only). A later folder's file replaces the patches of a file of the same name."""
    by_file,seen={},set()
    for directory in directories:
        folder=Path(directory).resolve()
        if folder in seen: continue  # the same folder named twice (a checkout's data folder)
        seen.add(folder)
        for key,path,meta in external_patches(directory,app_version,title_id):
            if path.name in by_file and by_file[path.name][0]!=path.parent:
                by_file[path.name]=(path.parent,[])
            by_file.setdefault(path.name,(path.parent,[]))[1].append((key,path,meta))
    return [patch for name in sorted(by_file) for patch in by_file[name][1]]


def external_selection(found, config, ini=None):
    """Selected patches: patches.json {"enabled": [...], "disabled": [...]} overrides each file's
    isEnabled; bbport.ini patch_* keys (GAME_PATCH_KEYS, the in-game menu) add to it."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    for key,patch in GAME_PATCH_KEYS.items():
        if (ini or {}).get(key)=='1': enabled.add(patch); disabled.discard(patch)
    if os.environ.get('BB_ONLINE', (ini or {}).get('online', '0'))=='1':
        enabled.add('CrashTeamRacingOnline.xml/Online: custom Demonware key')
    # The 1080p Pro patch follows render_res (Settings > Internal resolution, the in-game menu).
    pro_1080p=GAME_PATCH_KEYS['patch_pro_1080p']
    # With render_res the Pro mode follows it (run.py): 1080p runs the base PS4 path, so the
    # 1080p Pro patch is never needed (on the base path its flipped branch would take the Pro one).
    if render_plan(ini): enabled.discard(pro_1080p); disabled.add(pro_1080p)
    return [(key,path,meta) for key,path,meta in found
            if selection_key(key) in enabled
            or (selection_key(key) not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments, elf=None, strict=False, require_verification=False):
    """Writes of the selected patches; a patch with unsupported lines is skipped whole. elf (the
    eboot.elf bytes) is needed for lines with a Signature."""
    writes,claims=[],[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if require_verification and not (line.get('Signature') or line.get('ExpectedSHA256') or line.get('Expected')):
                    raise ValueError('unverified executable write; update this mod with code signatures: '+str(line.get('Address')))
                if line.get('Signature'):
                    if elf is None: raise ValueError('signature without the executable')
                    moved=locate(line,offset,elf)
                    if moved!=offset and line.get('ExpectedSHA256'):
                        raise ValueError('fixed-address code signature moved; this mod needs a port: '+str(line.get('Address')))
                    if moved!=offset: print(f'Patches: {key}: {line.get("Address")} found at {moved+EBOOT_BASE:#010x}')
                    offset=moved
                elif line.get('Expected'):
                    if elf is None: raise ValueError('expected bytes without the executable')
                    moved=locate_expected(line,offset,elf)
                    if moved!=offset: print(f'Patches: {key}: {line.get("Address")} verified at {moved+EBOOT_BASE:#010x}')
                    offset=moved
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                expected=line.get('ExpectedSHA256')
                if expected is not None:
                    if elf is None:raise ValueError('ExpectedSHA256 without the executable')
                    if not re.fullmatch('[0-9a-f]{64}',expected):raise ValueError('invalid ExpectedSHA256')
                    original=None
                    for start,pos,size in load_segments(elf):
                        if start<=offset and offset+len(data)<=start+size:
                            original=elf[pos+offset-start:pos+offset-start+len(data)];break
                    if original is None or hashlib.sha256(original).hexdigest()!=expected:
                        raise ValueError('executable code signature mismatch at '+str(line.get('Address')))
                ours.append((offset,data))
        except ValueError as error:
            if strict:raise ValueError(f'Cannot apply enabled mod patch {key}: {error}') from error
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        # Two patches writing the same bytes: the later one's bytes win, and neither works as
        # made (a widescreen layout and a patch of the same code, scripts/widescreen.py).
        for offset,data in ours:
            for start,end,other in claims:
                if other!=key and offset<end and start<offset+len(data):
                    if strict:raise ValueError(f'Conflicting executable patches: {key} / {other}')
                    print(f'Patches: {key} and {other} both write {offset+EBOOT_BASE:#010x}: the later one wins',file=sys.stderr)
                    break
        claims+=[(offset,offset+len(data),key) for offset,data in ours]
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
    p.add_argument('--strict-overlap',action='store_true',help='reject skipped/overlapping writes in mod sessions')
    p.add_argument('--patches-dir',type=Path,action='append',
                   help='patch XML files (shadPS4 format); repeatable, a later folder\'s file replaces one of the same name '
                        '(default: patches/ in the checkout)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled patches')
    p.add_argument('--app-version',default=APP_VERSION)
    p.add_argument('--title-id',default=TITLE_ID,help='the game (patch files naming other ids are skipped)')
    p.add_argument('--out',type=Path,default=root/'out')
    p.add_argument('--settings',type=Path,default=root/'bbport.ini')
    p.add_argument('--external-only',action='store_true',help='accepted for older callers (the only mode)')
    p.add_argument('--print-cutscene',action='store_true',
                   help='print "INSTANCE ACTIVE DESIGNER [FLYBY]" of the cutscene manager (ultrawide 16:9 cutscenes), or nothing')
    p.add_argument('--print-frontend',action='store_true',
                   help='print "INSTRUCTION VTABLE SLOT" for the ultrawide title/customization field of view, or nothing')
    p.add_argument('--print-plan',action='store_true',
                   help='print "PRO HEAP LABEL" of the render plan (run.sh), or nothing, and exit')
    a=p.parse_args()
    if a.print_cutscene:
        found=cutscene_state(a.out)
        if found: print(f'{found[0]:#x} {found[1]:#x} {found[2]:#x}'+(f' {found[3]:#x}' if found[3] is not None else ''))
        return
    if a.print_frontend:
        found=frontend_state(a.out)
        if found: print(' '.join(f'{v:#x}' for v in found))
        return
    if a.print_plan:
        plan=render_plan(read_settings(a.settings))
        if plan: print(int(plan['pro']),int(plan['heap']),plan['label'])
        return
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    found=all_external_patches(a.patches_dir or [root/'patches'],a.app_version,a.title_id)
    settings=read_settings(a.settings)
    writes=compile_external(external_selection(found,a.patches_config,settings)+render_patches(settings)
                            +theme_patches(settings)+end_race_patches(),
                            segments,elf,strict=a.strict_overlap)
    write_blob(a.out,writes)
    print(f'Patches: {len(writes)} writes from {len(found)} available patches')


if __name__=='__main__':
    main()
