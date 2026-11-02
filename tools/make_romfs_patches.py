# Adapted from MacNativeGames games/totk/scripts/make_romfs_patches.py @ ebe621d (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Build the 60 fps RomFS overlay for TotK 1.1.0, 1.2.1 or 1.4.3 (runtime/romfs.c, TOTK_ROMFS_PATCHES).

UI/LayoutArchive/Common.Product.<110 or 140>.Nin_NX_NVN.blarc.zs: the weapon / shield / material quick menus capture the game
picture for their background when the panes C_CaptureBG_00, C_CaptureClearBG_00 and P_CaptureClearBG_00 become
visible at animation frame 1.0 (anim/ShortCutBG_00_In.bflan), AutoBuilder the same with C_CaptureBG_00
(anim/AutoBuilder_00_In.bflan). UI animations advance 0.5 frames per game frame at 60 fps, so the capture lands after
the world stopped drawing and the background can be black. The visibility keys move to frame 0.0: the live world is
captured at once (upstream checked weapon, shield, material and ability wheel at 60 fps, and 30 fps).
The game-over flow fix of the same upstream script is 1.0.0 only (1.1.0, 1.2.1 and 1.4.3 run DmF_SY_GameOver.110) and is
left out.

--aspect <w>x<h> (a game resolution of another shape than 16:9, such as 3440x1440 or 1280x800) also lays the UI out
for that shape (tools/ultrawide_layout.py): menus keep their 16:9 proportions in the middle, full-screen backgrounds
cover the screen and the HUD moves to the screen's corners (wider) or its top and bottom edges (narrower). totk.exe
loads that overlay from romfs-patches-<w>x<h> when the game resolution is that size (runtime/romfs.c overlay_init);
the camera's side of it is the patch set aspect (recomp/patches-v121.txt, patches-v143.txt).

--prompts xbox | xbox-label: the same overlay with Xbox button icons in the game's prompts, for prompts = xbox
(runtime/romfs.c overlay_init loads romfs-patches-xbox or romfs-patches-xbox-label in place of romfs-patches). Drawn
from the game's own files on the player's machine, nothing added (tools/button_icons.py, which needs fontTools):
- both: L, R, ZL, ZR, Plus, Minus and the d-pad become LB, RB, LT, RT, Menu, View and a cross in the button font
  (scft/External.bfotf, the same font in each Font/Font*.Nin_NX_NVN.bfarc.zs), and L and ZL in the button guide's images
  (Nt_KeyTexL_00 and Nt_KeyTexZL_00 of timg/__Combined.bntx in the Common archive). The fonts grow, so the overlay also
  carries the resource size table (System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs) with their entries
  raised by as much.
- xbox, for pad_layout = nintendo. That layout plays an Xbox pad by position: the button that does Switch A is
  labelled B, so the face buttons' prompts are swapped to name the labels, A with B and X with Y:
  - the action prompt (blyt/DoCommand_00.bflyt) draws the letter images Nt_KeyTexA/B/X/Y_00 of timg/__Combined.bntx in
    the Common archive: their image data is swapped (same format, size and tiling). Around it, a diamond names the
    other face buttons with one-letter text panes (T_B_00, T_X_00, ...). The game fills them from the messages of the
    same name in LayoutMsg/DoCommand_00.msbt of each Mals/<language> archive, over the layout's own text: both have
    their letters swapped the same way. The diamond's shape (Nt_KeyBtnSubBase*) shows the button's position, which
    stays right.
  - every other prompt: the glyphs U+E040 A, U+E041 B, U+E042 X, U+E043 Y of the button font trade places.
- xbox-label, for pad_layout = xbox and auto. Those layouts put Switch A on the button labelled A (auto picks xbox for
  every pad not labelled the Nintendo way), so the face buttons keep their letters; the action prompt's diamond keeps
  the Switch positions (A on the right).
--xbox-prompts is the older name of --prompts xbox.

Input: build/extracted/<version>/romfs (romfs.bin and index.json, verified by tools/extract.py). Output:
build/<version>/romfs-patches/<path> by default (romfs-patches-<style> with --prompts), and <out>/manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original). Run with .venv-py314.
"""
import argparse
import compression.zstd as zstd
import hashlib
import importlib.util
import json
import re
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--version', choices=('v110', 'v121', 'v143'), default='v121')
ap.add_argument('--romfs', type=Path, help='default build/extracted/<version>/romfs')
ap.add_argument('--out', type=Path,
                help='default build/<version>/romfs-patches (romfs-patches-<style> with --prompts)')
ap.add_argument('--aspect', help='<w>x<h> of a game resolution of another shape than 16:9: also lay the UI out for it')
ap.add_argument('--prompts', choices=('xbox', 'xbox-label'),
                help='Xbox button icons in the prompts: xbox for pad_layout = nintendo (A shown as B, X as Y), '
                     'xbox-label for pad_layout = xbox or auto (the face buttons keep their letters)')
ap.add_argument('--xbox-prompts', dest='prompts', action='store_const', const='xbox', help='the same as --prompts xbox')
args = ap.parse_args()
args.romfs = args.romfs or root / 'build/extracted' / args.version / 'romfs'
args.out = args.out or root / 'build' / args.version / (f'romfs-patches-{args.prompts}' if args.prompts else 'romfs-patches')
index = json.loads((args.romfs / 'index.json').read_text())
assert index['status'] == 'pass'
files = {f['path']: f for f in index['files']}


def sarc_nodes(data):
    """(name, node offset, absolute start, end) of every SARC member, in SFAT order."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'
    names += 8
    out = []
    for i in range(count):
        node = header + 12 + i * 16
        _, attribute, lo, hi = struct.unpack_from('<4I', data, node)
        offset = names + (attribute & 0xffffff) * 4
        out.append((data[offset:data.index(0, offset, start)].decode(), node, start + lo, start + hi))
    return out


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    return {name: (lo, hi) for name, _, lo, hi in sarc_nodes(data)}


def flvi_keys(data, base):
    """(pane name, absolute key offset, keys) of every FLVI (visibility) step track in the BFLAN at data[base:]."""
    assert data[base:base + 4] == b'FLAN' and data[base + 4:base + 6] == b'\xff\xfe'
    pos, out = base + 0x14, []
    for _ in range(struct.unpack_from('<H', data, base + 0x10)[0]):
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'pai1':
            count, table = struct.unpack_from('<H', data, pos + 14)[0], pos + struct.unpack_from('<I', data, pos + 16)[0]
            for e in range(count):
                eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
                name = data[eo:eo + 28].split(b'\0')[0].decode()
                for t in range(data[eo + 28]):
                    to = eo + struct.unpack_from('<I', data, eo + 32 + 4 * t)[0]
                    if data[to:to + 4] != b'FLVI':
                        continue
                    for g in range(data[to + 4]):
                        go = to + struct.unpack_from('<I', data, to + 8 + 4 * g)[0]
                        assert data[go + 2] == 1, 'visibility tracks are step curves'
                        ko = go + struct.unpack_from('<I', data, go + 8)[0]
                        keys = [struct.unpack_from('<fH', data, ko + 8 * k)
                                for k in range(struct.unpack_from('<H', data, go + 4)[0])]
                        out.append((name, ko, keys))
        pos += size
    return out


def bntx_swap(data, base, pairs):
    """Swap the image data of each (name, name) pair of textures in the BNTX at data[base:] (names without the ^
    suffix). Both of a pair must have the same header from flags to layout (format, size, tiling, mips) and image
    size, so the bytes of one are a valid image of the other."""
    assert data[base:base + 4] == b'BNTX' and data[base + 0x20:base + 0x24] == b'NX  '
    count, table = struct.unpack_from('<IQ', data, base + 0x24)
    found = {}
    for k in range(count):
        info = base + struct.unpack_from('<Q', data, base + table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        name = base + struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[name + 2:name + 2 + struct.unpack_from('<H', data, name)[0]].decode().split('^')[0]
        mips = base + struct.unpack_from('<Q', data, info + 0x70)[0]
        shape = bytes(data[info + 0x10:info + 0x3c]) + bytes(data[info + 0x50:info + 0x54])
        start, size = base + struct.unpack_from('<Q', data, mips)[0], struct.unpack_from('<I', data, info + 0x50)[0]
        found[name] = (shape, start, size)
    for a, b in pairs:
        assert a in found and b in found, f'{a} or {b} missing from the texture list'
        (shape_a, at_a, size), (shape_b, at_b, _) = found[a], found[b]
        assert shape_a == shape_b, f'{a} and {b} differ in format, size or tiling'
        data[at_a:at_a + size], data[at_b:at_b + size] = data[at_b:at_b + size], data[at_a:at_a + size]


def bflyt_letters(data, base, end, swap):
    """Swap the text of the one-letter text panes T_<letter>_<n> (A, B, X or Y) in the BFLYT at data[base:end] by
    swap (letter -> letter); the names of the panes changed. Same length (one UTF-16 letter and its terminator)."""
    assert data[base:base + 4] == b'FLYT'
    pos, changed = base + struct.unpack_from('<H', data, base + 6)[0], []
    while pos + 8 <= end:
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'txt1':
            name = data[pos + 12:pos + 36].split(b'\0')[0].decode()
            length = struct.unpack_from('<H', data, pos + 0x56)[0]
            at = pos + struct.unpack_from('<I', data, pos + 0x64)[0]
            letter = data[at:at + 2].decode('utf-16-le') if length == 4 and at > pos else ''
            if letter in swap and re.fullmatch(r'T_[ABXY]_\d+', name):
                data[at:at + 2] = swap[letter].encode('utf-16-le')
                changed.append(name)
        if not size:
            break
        pos += size
    return changed


def msbt_letters(data, swap):
    """Swap the one-letter messages labelled T_<letter>_<n> (A, B, X or Y) in the UTF-16 MSBT data (a bytearray) by
    swap; the labels changed. The game sets a layout's text panes from the messages of the same name in
    Mals/<language>/LayoutMsg/<layout>.msbt, over the text stored in the layout."""
    assert data[:8] == b'MsgStdBn' and data[0x0c] == 1, 'not a UTF-16 MSBT'
    sections, pos = {}, 0x20
    for _ in range(struct.unpack_from('<H', data, 0x0e)[0]):
        size = struct.unpack_from('<I', data, pos + 4)[0]
        sections[bytes(data[pos:pos + 4])] = (pos + 16, size)
        pos = (pos + 16 + size + 15) & ~15
    labels, (base, _) = {}, sections[b'LBL1']
    for g in range(struct.unpack_from('<I', data, base)[0]):
        count, at = struct.unpack_from('<II', data, base + 4 + 8 * g)
        at += base
        for _ in range(count):
            length = data[at]
            labels[struct.unpack_from('<I', data, at + 1 + length)[0]] = data[at + 1:at + 1 + length].decode()
            at += 1 + length + 4
    (base, size), changed = sections[b'TXT2'], []
    count = struct.unpack_from('<I', data, base)[0]
    offsets = list(struct.unpack_from(f'<{count}I', data, base + 4)) + [size]
    for i in range(count):
        at, length = base + offsets[i], offsets[i + 1] - offsets[i]
        letter = data[at:at + 2].decode('utf-16-le') if length == 4 else ''
        if letter in swap and re.fullmatch(r'T_[ABXY]_\d+', labels.get(i, '')):
            data[at:at + 2] = swap[letter].encode('utf-16-le')
            changed.append(labels[i])
    return changed


def sarc_replace(data, new):
    """The SARC data with the members named in new (name -> bytes) replaced. The header, SFAT and SFNT stay; the
    members are laid out again in their old order, each at an offset with at least the alignment its old one had."""
    start = struct.unpack_from('<I', data, 12)[0]
    out = bytearray(data[:start])
    for name, node, lo, hi in sorted(sarc_nodes(data), key=lambda n: n[2]):
        align = min(lo & -lo, 0x2000) if lo else 0x2000
        out += bytes(-len(out) % align)
        body = new.get(name, data[lo:hi])
        struct.pack_into('<II', out, node + 8, len(out) - start, len(out) - start + len(body))
        out += body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


COMMON = {'v110': '110', 'v121': '110', 'v143': '140'}   # the version in the name of each game's Common archive
ARCHIVE = f'UI/LayoutArchive/Common.Product.{COMMON[args.version]}.Nin_NX_NVN.blarc.zs'
MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}
KEY_TEXTURES = (('Nt_KeyTexA_00', 'Nt_KeyTexB_00'), ('Nt_KeyTexX_00', 'Nt_KeyTexY_00'))
KEY_GLYPHS = ((0xE040, 0xE041), (0xE042, 0xE043))   # A B, X Y in scft/External.bfotf
KEY_LETTERS = {'A': 'B', 'B': 'A', 'X': 'Y', 'Y': 'X'}   # the face-button diamond around the action prompt
FONTS = sorted(p for p in files if re.fullmatch(r'Font/Font(_\w+)?\.Nin_NX_NVN\.bfarc\.zs', p))
# The message archive of each language: the newest version in the RomFS (Mals/USen.Product.121.sarc.zs on 1.2.1).
MALS = {}
for p in files:
    m = re.fullmatch(r'Mals/(\w+)\.Product\.(\d+)\.sarc\.zs', p)
    if m and int(m[2]) > MALS.get(m[1], (0, ''))[0]:
        MALS[m[1]] = (int(m[2]), p)
MALS = sorted(p for _, p in MALS.values())
LAYOUT_MSG = 'LayoutMsg/DoCommand_00.msbt'
# The resource size table (System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs, matched as loosely as
# MacNativeGames make_button_icons.py does), likewise the newest version in the RomFS.
RESTBL = max((p for p in files if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.rsizetable.zs')),
             key=lambda p: int((re.search(r'\.(\d+)\.', p) or [0, 0])[1]), default=None)
by_position = args.prompts == 'xbox'   # the face buttons' prompts swapped to the labels (pad_layout = nintendo)
if args.prompts:
    assert FONTS, 'no Font/Font*.Nin_NX_NVN.bfarc.zs in the RomFS'
    assert MALS or not by_position, 'no Mals/<language>.Product.<version>.sarc.zs in the RomFS'
    assert RESTBL, 'no System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs in the RomFS'
    spec = importlib.util.spec_from_file_location('button_icons', Path(__file__).with_name('button_icons.py'))
    icons = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(icons)
    assert icons.TEXT_ARCHIVE in FONTS, f'no {icons.TEXT_ARCHIVE} in the RomFS'

with (args.romfs / 'romfs.bin').open('rb') as image:
    def read(path):
        f = files[path]
        image.seek(f['offset'])
        b = image.read(f['size'])
        assert len(b) == f['size']
        return b
    dictionaries = {}
    pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
    for name, (lo, hi) in sarc_members(pack).items():
        d = zstd.ZstdDict(pack[lo:hi])
        dictionaries[d.dict_id] = d
    prompt_files = (FONTS + (MALS if by_position else []) + [RESTBL]) if args.prompts else []
    originals = {path: read(path) for path in [ARCHIVE] + prompt_files}
raw = originals[ARCHIVE]
info = zstd.get_frame_info(raw)
dictionary = dictionaries[info.dictionary_id]
original = zstd.decompress(raw, zstd_dict=dictionary)
blarc = bytearray(original)
assert len(blarc) == info.decompressed_size
members = sarc_members(blarc)
patched = {}
for member, panes in MEMBERS.items():
    lo, hi = members[member]
    changed = []
    for name, ko, keys in flvi_keys(blarc, lo):
        if name not in panes:
            continue
        assert [(f, v) for f, v in keys] == [(0.0, 0), (1.0, 1)], (member, name, keys)
        struct.pack_into('<f', blarc, ko + 8, 0.0)
        changed.append(name)
    assert sorted(changed) == sorted(panes), (member, changed)
    patched[member] = changed
layout = []
if args.aspect:
    width, height = (int(v) for v in args.aspect.lower().split('x'))
    spec = importlib.util.spec_from_file_location('ultrawide_layout', Path(__file__).with_name('ultrawide_layout.py'))
    ultrawide = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ultrawide)
    layout = ultrawide.apply(blarc, members, width, height)
diff = [i for i in range(len(original)) if original[i] != blarc[i]]
touched = [members[m] for m in (*MEMBERS, *layout)]
assert diff and all(any(lo <= i < hi for lo, hi in touched) for i in diff), diff
outputs = {ARCHIVE: bytes(blarc)}   # RomFS path -> new contents (before compression)
prompts = None
if args.prompts:
    sizes = {ARCHIVE: len(original)}   # RomFS path -> size of the original's contents

    def unpack(path):
        frame = zstd.get_frame_info(originals[path])
        data = zstd.decompress(originals[path], zstd_dict=dictionaries.get(frame.dictionary_id))
        sizes[path] = len(data)
        return data
    archives = {path: unpack(path) for path in FONTS}
    text = sarc_members(archives[icons.TEXT_ARCHIVE])
    missing = [name for name in icons.TEXT_FONTS if name not in text]
    assert not missing, f'no {", ".join(missing)} in {icons.TEXT_ARCHIVE}'
    drawn = icons.xbox_icons(icons.text_glyphs(archives[icons.TEXT_ARCHIVE][slice(*text[name])]
                                               for name in icons.TEXT_FONTS))
    bntx = members['timg/__Combined.bntx'][0]
    letters = {}
    if by_position:
        bntx_swap(blarc, bntx, KEY_TEXTURES)
        for member, (lo, hi) in members.items():
            if member.endswith('.bflyt'):
                changed = bflyt_letters(blarc, lo, hi, KEY_LETTERS)
                if changed:
                    letters[member] = changed
        assert 'blyt/DoCommand_00.bflyt' in letters, 'no letter panes in blyt/DoCommand_00.bflyt'
    hud = icons.bntx_redraw(blarc, bntx, icons.HUD_TEXTURES, drawn)
    outputs[ARCHIVE] = bytes(blarc)
    fonts = {}   # each distinct font is drawn once (1.4.3's four archives hold the same one)
    for path, bfarc in archives.items():
        lo, hi = sarc_members(bfarc)[icons.FONT]
        otf, key = icons.bfotf_decode(bfarc[lo:hi])
        if otf not in fonts:
            fonts[otf] = icons.redraw_font(otf, drawn, KEY_GLYPHS if by_position else ())
        outputs[path] = sarc_replace(bfarc, {icons.FONT: icons.bfotf_encode(fonts[otf], key, bfarc[lo:hi])})
    messages = {}
    for path in MALS if by_position else []:
        mals = bytearray(unpack(path))
        lo, hi = sarc_members(mals)[LAYOUT_MSG]
        msbt = bytearray(mals[lo:hi])
        changed = msbt_letters(msbt, KEY_LETTERS)
        assert len(changed) == sum(len(v) for v in letters.values()), (path, changed)
        mals[lo:hi] = msbt
        outputs[path] = bytes(mals)
        messages[path] = len(changed)
    # An archive that grew needs as much more room in the resource size table, or the game stops when it loads it.
    grown = {path: len(data) - sizes[path] for path, data in outputs.items() if len(data) > sizes[path]}
    if grown:
        outputs[RESTBL] = icons.grow_restbl(unpack(RESTBL), grown)
    prompts = dict(style=args.prompts, textures=KEY_TEXTURES if by_position else [], letters=letters,
                   messages=messages, hud=hud, icons=len(drawn), fonts=FONTS, grown=grown)

lines = []
for path, contents in outputs.items():
    raw = originals[path]
    dictionary = dictionaries.get(zstd.get_frame_info(raw).dictionary_id)
    data = zstd.compress(contents, level=19, zstd_dict=dictionary)
    assert zstd.decompress(data, zstd_dict=dictionary) == contents
    assert zstd.get_frame_info(data).dictionary_id == zstd.get_frame_info(raw).dictionary_id
    target = args.out / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()]))
(args.out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/make_romfs_patches.py)\n'
                                       + ''.join(line + '\n' for line in lines))
print(json.dumps(dict(status='pass', path=ARCHIVE, members=patched, changed_bytes=len(diff),
                      ultrawide_layout=dict(aspect=args.aspect, members=len(layout)) if args.aspect else None,
                      prompts=prompts)))
