"""Where a UI layout's panes sit, read from the game's own layout archive.

Used to settle a question about the game's UI without guessing at a running process: which panes a screen has, what they
are called and where they sit. The blueprint import row needed exactly that - it belongs to the Autobuild screen's three
tabs, not to its card list, and the tabs are told apart at run time by the layout each button was built from. This tool
is what confirmed those template names (PaAutoBuilderTabBtn_00 for a tab, PaAutoBuilderPhotoBtn_00 for a card).

Usage:
  .venv-py314\\Scripts\\python tools\\dump_ui_panes.py                       # the Autobuild screen's panes
  .venv-py314\\Scripts\\python tools\\dump_ui_panes.py --match PaAutoBuilder # every member mentioning this
  .venv-py314\\Scripts\\python tools\\dump_ui_panes.py --layout blyt/AutoBuilder_00.bflyt

Needs Python 3.14 (compression.zstd) for the archive's zstd-with-dictionary frames, and the extracted RomFS
(build/extracted/<version>/romfs). Read-only.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import compression.zstd as zstd

ROOT = Path(__file__).resolve().parent.parent
ROMFS_DIR = ROOT / 'build' / 'extracted' / 'v143' / 'romfs'
ROMFS = ROMFS_DIR / 'romfs.bin'
INDEX = ROMFS_DIR / 'index.json'
ARCHIVE = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'
DIC_PACK = 'Pack/ZsDic.pack.zs'

# BFLYT panes carry their name at +12 (24 bytes), translate x / y at +44 / +48 and scale at +68 / +72.
PANE_SECTIONS = (b'pan1', b'prt1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'ali1')


def read_entry(index, path):
    ent = next((f for f in index['files'] if f['path'] == path), None)
    if not ent:
        raise SystemExit('not in the RomFS index: %s' % path)
    with ROMFS.open('rb') as f:
        f.seek(ent['offset'])
        return f.read(ent['size'])


def sarc_members(blarc):
    """{name: (offset, size)} for every file in a SARC."""
    data_off = struct.unpack_from('<I', blarc, 0x0C)[0]
    sfat = blarc.find(b'SFAT')
    node_count = struct.unpack_from('<H', blarc, sfat + 6)[0]
    nodes = sfat + 0x0C
    name_base = blarc.find(b'SFNT') + 8
    out = {}
    for i in range(node_count):
        _h, attrs, start, end = struct.unpack_from('<IIII', blarc, nodes + i * 16)
        at = name_base + (attrs & 0xFFFFFF) * 4
        name = blarc[at:blarc.index(b'\0', at)].decode('utf-8', 'replace')
        out[name] = (data_off + start, end - start)
    return out


def panes_of(bflyt):
    """(section, name, x, y, scale x, scale y) for every pane in a BFLYT, in file order."""
    out = []
    for magic in PANE_SECTIONS:
        at = 0
        while True:
            at = bflyt.find(magic, at)
            if at < 0:
                break
            name = bflyt[at + 12:at + 36].split(b'\0')[0].decode('utf-8', 'replace')
            tx, ty = struct.unpack_from('<ff', bflyt, at + 44)
            sx, sy = struct.unpack_from('<ff', bflyt, at + 68)
            out.append((magic.decode(), name, tx, ty, sx, sy))
            at += 4
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--match', default='AutoBuilder', help='only members whose name contains this (default AutoBuilder)')
    ap.add_argument('--layout', default='', help='dump this member\'s panes (default: every matching .bflyt)')
    args = ap.parse_args()

    if not ROMFS.is_file():
        raise SystemExit('romfs.bin not found: %s (extract the game first)' % ROMFS)
    index = json.loads(INDEX.read_text(encoding='utf-8'))

    dictionaries = {}
    pack = zstd.decompress(read_entry(index, DIC_PACK))
    for name, (lo, size) in sarc_members(pack).items():
        d = zstd.ZstdDict(pack[lo:lo + size])
        dictionaries[d.dict_id] = d

    raw = read_entry(index, ARCHIVE)
    info = zstd.get_frame_info(raw)
    blarc = zstd.decompress(raw, zstd_dict=dictionaries.get(info.dictionary_id))
    members = sarc_members(blarc)
    print('archive: %d bytes, %d members' % (len(blarc), len(members)))

    if args.layout:
        if args.layout not in members:
            raise SystemExit('no such member: %s' % args.layout)
        lo, size = members[args.layout]
        for kind, name, x, y, sx, sy in panes_of(blarc[lo:lo + size]):
            print('  %-5s %-24s x %9.2f y %9.2f scale %.2f %.2f' % (kind, name, x, y, sx, sy))
        return 0

    for name, (lo, size) in sorted(members.items()):
        if args.match.lower() not in name.lower():
            continue
        print('\n=== %s (%d bytes)' % (name, size))
        if not name.endswith('.bflyt'):
            continue
        for kind, pane, x, y, sx, sy in panes_of(blarc[lo:lo + size]):
            print('  %-5s %-24s x %9.2f y %9.2f scale %.2f %.2f' % (kind, pane, x, y, sx, sy))
    return 0


if __name__ == '__main__':
    sys.exit(main())
