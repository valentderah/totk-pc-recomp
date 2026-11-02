"""The mods folder served as RomFS overlays (runtime/romfs.c, runtime/mods.c): load order, added files, case, Unicode.

    .venv\\Scripts\\python tests\\mods_romfs_check.py --exe build\\v121-<tag>\\totk.exe --version v121

Builds a mods folder in a temp directory with files of its own bytes at real RomFS paths (from
build/extracted/<version>/romfs/index.json), runs `totk.exe --probe-fs` on it and reads the `mods:` lines of the log.
--probe-fs compares the bytes it reads through the RomFS with every overlay file, so a wrong winner fails it too.
A second folder checks the merged resource size table the start writes to the profile (runtime/romfs.c
restbl_overlay): a grown file's entry, an added file's, a shipped table's and another version's table's.
Needs the extracted RomFS of the version.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import zstandard

ROOT = Path(__file__).resolve().parents[1]
UNI = 'Ж мод'


def pick_paths(version):
    index = json.loads((ROOT / 'build/extracted' / version / 'romfs/index.json').read_text())
    paths = [f['path'] for f in index['files'] if 1024 <= f['size'] <= 65536]
    have = set(f['path'] for f in index['files'])
    p1 = next(p for p in paths if any(c.isalpha() for c in p.rsplit('/', 1)[-1]))
    p2 = next(p for p in paths if p != p1)
    i = next(k for k, c in enumerate(p1) if c.isalpha() and k > p1.rfind('/'))
    p1case = p1[:i] + p1[i].swapcase() + p1[i + 1:]
    assert p1case not in have
    return p1, p2, p1case


def put(root, mod, rel, data, part='romfs', times=64):
    f = Path(root) / mod / part / rel
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_bytes(data * times)


def run(exe, mods, order, disabled=''):
    """--probe-fs with a profile of its own beside mods (the merged resource size table is written there)."""
    env = dict(os.environ, TOTK_MODS_DIR=str(mods), TOTK_MODS_ORDER=order, TOTK_MODS_DISABLED=disabled)
    profile = Path(mods).parent / 'profile'
    r = subprocess.run([str(exe), '--profile', str(profile), '--probe-fs'], env=env, capture_output=True, timeout=600)
    log = (r.stdout + r.stderr).decode('utf-8', 'replace')
    return r.returncode, log


# ---- the resource size table (RESTBL: "RESTBL", u32 version, u32 name length, u32 hashed count, u32 named count,
# {u32 crc32, u32 size} sorted, then {char[name length], u32 size})
def restbl_read(data):
    raw = zstandard.ZstdDecompressor().decompress(data)
    assert raw[:6] == b'RESTBL'
    _, name_len, hashed, named = struct.unpack_from('<4I', raw, 6)
    out = dict(struct.unpack_from('<II', raw, 22 + i * 8) for i in range(hashed))
    hashes = [struct.unpack_from('<I', raw, 22 + i * 8)[0] for i in range(hashed)]
    assert hashes == sorted(hashes), 'hashed entries out of order'
    at = 22 + hashed * 8
    for _ in range(named):
        out[raw[at:at + name_len].split(b'\0')[0].decode()] = struct.unpack_from('<I', raw, at + name_len)[0]
        at += name_len + 4
    return out, raw


def restbl_pack(entries, raw):
    """raw (a table) with the sizes of its hashed entries set from entries (crc32 -> size, existing hashes only)."""
    out = bytearray(raw)
    hashed = struct.unpack_from('<I', raw, 14)[0]
    for i in range(hashed):
        h = struct.unpack_from('<I', raw, 22 + i * 8)[0]
        if h in entries:
            struct.pack_into('<I', out, 26 + i * 8, entries[h])
    return zstandard.ZstdCompressor(write_content_size=True).compress(bytes(out))


def key(path):
    return zlib.crc32((path[:-3] if path.endswith(('.zs', '.mc')) else path).encode())


def restbl_case(exe, version, check, tmp):
    romfs = ROOT / 'build/extracted' / version / 'romfs'
    index = json.loads((romfs / 'index.json').read_text())
    files = {f['path']: f for f in index['files']}
    table_path = next(p for p in files if p.startswith('System/Resource/ResourceSizeTable.') and
                      p.endswith('.rsizetable.zs'))
    with (romfs / 'romfs.bin').open('rb') as image:
        def read(path, n=None):
            image.seek(files[path]['offset'])
            return image.read(n or files[path]['size'])
        game, game_raw = restbl_read(read(table_path))
        packs = [p for p in files if p.startswith('Pack/Actor/') and p.endswith('.pack.zs') and key(p) in game]
        grown, shipped_key, old_key, old_other = packs[0], packs[1], packs[2], packs[3]
        was = zstandard.get_frame_parameters(read(grown, 18)).content_size
    zs = zstandard.ZstdCompressor(write_content_size=True)
    mods = Path(tmp) / 'restbl' / 'mods'
    put(mods, 'Grow', grown, zs.compress(bytes(was + 100000)), times=1)
    put(mods, 'Grow', 'Pack/Actor/ModTest_Added.pack.zs', zs.compress(bytes(50000)), times=1)
    put(mods, 'Ships', table_path, restbl_pack({key(shipped_key): game[key(shipped_key)] + 12345}, game_raw), times=1)
    put(mods, 'Old', old_key, b'old', times=1)   # smaller than the original: only its table raises it
    put(mods, 'Old', table_path.replace('.Product.', '.Product.999', 1) if '.Product.' in table_path else
        'System/Resource/ResourceSizeTable.Product.999.Nin_NX_NVN.rsizetable.zs',
        restbl_pack({key(old_key): game[key(old_key)] + 7777, key(old_other): game[key(old_other)] + 5555},
                    game_raw), times=1)
    code, log = run(exe, mods, 'Grow|Ships|Old')
    check(code == 0, 'probe-fs passes with the resource size table mods', log)
    merged_file = mods.parent / 'profile' / 'mods' / table_path.rsplit('/', 1)[1]
    check(merged_file.is_file(), f'the merged table is written to the profile ({merged_file.name})', log)
    if not merged_file.is_file():
        return
    merged, _ = restbl_read(merged_file.read_bytes())
    g = game[key(grown)]
    check(merged[key(grown)] >= g + 100000, f'a file grown by 100000 bytes: entry {g} -> {merged[key(grown)]}')
    added = merged.get(key('Pack/Actor/ModTest_Added.pack.zs'), 0)
    check(added >= 50000 + 256, f'an added 50000-byte pack gets an entry ({added})')
    check(merged[key(shipped_key)] == game[key(shipped_key)] + 12345, 'a shipped table\'s larger entry is merged')
    check(merged[key(old_key)] == game[key(old_key)] + 7777, 'another version\'s table raises its own mod\'s file')
    check(merged[key(old_other)] == game[key(old_other)], 'another version\'s table leaves other files alone')
    lower = [h for h, s in game.items() if merged.get(h, 0) < s]
    check(not lower, f'no entry of the game\'s table is lowered ({len(lower)})')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--version', choices=('v121', 'v143'), required=True)
    args = ap.parse_args()
    p1, p2, p1case = pick_paths(args.version)
    fails = 0

    def check(ok, what, log=None):
        nonlocal fails
        print(('PASS ' if ok else 'FAIL ') + what)
        if not ok:
            fails += 1
            if log:
                print('\n'.join(l for l in log.splitlines() if 'mods' in l or 'overlay' in l.lower())[-3000:])

    with tempfile.TemporaryDirectory() as tmp:
        mods = Path(tmp) / 'mods'
        put(mods, 'Top', p1, b'mod-A')
        put(mods, 'Low', p1, b'mod-B')
        put(mods, 'Low', p2, b'mod-C')
        put(mods, 'Top', 'New/added.bin', b'mod-D')
        put(mods, 'Low', 'New/added.bin', b'mod-E')
        put(mods, 'Case', p1case, b'mod-F')
        put(mods, UNI, p2, b'mod-G')
        # a file of another game version's (no RomFS has a Product.999 table): counted, never served
        put(mods, 'Low', 'System/Resource/ResourceSizeTable.Product.999.rsizetable.zs', b'mod-H')
        put(mods, 'Lite', 'Lite/added.bin', b'mod-I', part='romfslite')   # TKMM's romfslite folder

        code, log = run(args.exe, mods, f'Top|Low|{UNI}|Case')
        check(code == 0, 'probe-fs passes with five mods (served bytes equal the winners\' files)', log)
        for line in ('mods: Top: 2 files served (1 replaced, 1 added), 0 overridden',
                     'mods: Low: 1 files served (1 replaced, 0 added), 2 overridden by Top, 1 for another game version',
                     'mods: Case: 0 files served (0 replaced, 0 added), 1 overridden by Top',
                     f'mods: {UNI}: 0 files served (0 replaced, 0 added), 1 overridden by Low',
                     'mods: Lite: 1 files served (0 replaced, 1 added), 0 overridden'):
            check(line in log, line, log)
        check(p1case not in log, 'the case-flipped path is served as the RomFS path, not added', log)

        code, log = run(args.exe, mods, f'Top|Low|{UNI}|Case', disabled='Top')
        check(code == 0, 'probe-fs passes with Top switched off', log)
        check('mods: Low: 3 files served (2 replaced, 1 added), 0 overridden' in log, 'Low serves P1 and the added file', log)
        check('mods: Top: ' not in log.replace('mods: Top: off', ''), 'a switched-off mod serves nothing', log)

        restbl_case(args.exe, args.version, check, tmp)
    print('mods_romfs_check:', 'FAIL' if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
