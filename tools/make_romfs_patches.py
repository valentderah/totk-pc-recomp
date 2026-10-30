# Adapted from MacNativeGames games/totk/scripts/make_romfs_patches.py @ ebe621d (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Build the 60 fps RomFS overlay for TotK 1.2.1 or 1.4.3 (runtime/romfs.c, TOTK_ROMFS_PATCHES).

UI/LayoutArchive/Common.Product.<110 or 140>.Nin_NX_NVN.blarc.zs: the weapon / shield / material quick menus capture the game
picture for their background when the panes C_CaptureBG_00, C_CaptureClearBG_00 and P_CaptureClearBG_00 become
visible at animation frame 1.0 (anim/ShortCutBG_00_In.bflan), AutoBuilder the same with C_CaptureBG_00
(anim/AutoBuilder_00_In.bflan). UI animations advance 0.5 frames per game frame at 60 fps, so the capture lands after
the world stopped drawing and the background can be black. The visibility keys move to frame 0.0: the live world is
captured at once (upstream checked weapon, shield, material and ability wheel at 60 fps, and 30 fps).
The game-over flow fix of the same upstream script is 1.0.0 only (1.2.1 and 1.4.3 run DmF_SY_GameOver.110) and is
left out.

--aspect <w>x<h> (a game resolution wider than 16:9, such as 3440x1440) also lays the UI out for that shape
(tools/ultrawide_layout.py): menus stay 16:9 in the middle, full-screen backgrounds cover the width and the HUD moves
to the screen's corners. totk.exe loads that overlay from romfs-patches-<w>x<h> when the game resolution is that size
(runtime/romfs.c overlay_init); the camera's side of it is the patch set aspect (recomp/patches-v143.txt).

Input: build/extracted/<version>/romfs (romfs.bin and index.json, verified by tools/extract.py). Output:
build/<version>/romfs-patches/<path> by default, and <out>/manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original). Run with .venv-py314.
"""
import argparse
import compression.zstd as zstd
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--version', choices=('v121', 'v143'), default='v121')
ap.add_argument('--romfs', type=Path, help='default build/extracted/<version>/romfs')
ap.add_argument('--out', type=Path, help='default build/<version>/romfs-patches')
ap.add_argument('--aspect', help='<w>x<h> of a game resolution wider than 16:9: also lay the UI out for it')
args = ap.parse_args()
args.romfs = args.romfs or root / 'build/extracted' / args.version / 'romfs'
args.out = args.out or root / 'build' / args.version / 'romfs-patches'
index = json.loads((args.romfs / 'index.json').read_text())
assert index['status'] == 'pass'
files = {f['path']: f for f in index['files']}


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'
    names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out


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


COMMON = {'v121': '110', 'v143': '140'}   # the version in the name of each game's Common archive
ARCHIVE = f'UI/LayoutArchive/Common.Product.{COMMON[args.version]}.Nin_NX_NVN.blarc.zs'
MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}

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
    raw = read(ARCHIVE)
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
data = zstd.compress(bytes(blarc), level=19, zstd_dict=dictionary)
assert zstd.decompress(data, zstd_dict=dictionary) == bytes(blarc)
assert zstd.get_frame_info(data).dictionary_id == info.dictionary_id
target = args.out / ARCHIVE
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(data)
line = '\t'.join([ARCHIVE, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()])
(args.out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/make_romfs_patches.py)\n'
                                       + line + '\n')
print(json.dumps(dict(status='pass', path=ARCHIVE, members=patched, size=len(data), changed_bytes=len(diff),
                      ultrawide_layout=dict(aspect=args.aspect, members=len(layout)) if args.aspect else None)))
