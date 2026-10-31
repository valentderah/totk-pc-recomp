"""Fold the loose files of a shader cache into <cache>/shader-cache.pack and delete them.

The translator writes four files per shader into the cache (runtime/gfx_shader.c): <key>.spv, <key>.res.json,
<name>.use and in/<input id>.in, about 117,000 files after an install, which are slow to copy, back up, scan and sync.
totk.exe reads the pack first and the loose files after it, and keeps writing new translations as loose files, so
this tool can run again at any time the game is closed: it merges the existing pack with the loose files (a loose file
wins over a packed one of the same name) and writes the pack anew. The installer runs it after preparing the shaders.

Layout, little-endian (gfx_shader.c, section "cache pack"): b'TKSPACK1', u32 count, u32 0, then count entries of 48
bytes sorted by (kind, name): name (the 32 bytes the file name's 64 hex digits spell), u32 kind (1 .spv, 2 .res.json,
3 .use, 4 in/*.in), u32 size, u64 offset; then the data.

    python tools\\pack_shader_cache.py <install folder>\\shader-cache [--keep]

--keep leaves the loose files in place. Close the game first: a running totk.exe has read the old pack's index.
"""
import argparse
import os
import re
import struct
import sys
from pathlib import Path

MAGIC = b'TKSPACK1'
PACK = 'shader-cache.pack'
KINDS = {'.spv': 1, '.res.json': 2, '.use': 3, '.in': 4}
NAME = re.compile(r'([0-9a-f]{64})(\.spv|\.res\.json|\.use|\.in)')
USE_SIZE = 20   # gfx_shader.c use_store: magic, version, three masks


def loose_files(cache):
    """{(kind, name): path} of the cache's loose files; *.in only in in/."""
    found = {}
    for folder, kinds in ((cache, (1, 2, 3)), (cache / 'in', (4,))):
        if not folder.is_dir():
            continue
        with os.scandir(folder) as it:
            for e in it:
                m = NAME.fullmatch(e.name)
                if not m or not e.is_file():
                    continue
                kind = KINDS[m.group(2)]
                if kind in kinds:
                    found[(kind, bytes.fromhex(m.group(1)))] = Path(e.path)
    return found


def read_pack(path):
    """{(kind, name): (offset, size)} of an existing pack; {} when there is none or it fails a check."""
    try:
        f = open(path, 'rb')
    except FileNotFoundError:
        return {}
    with f:
        size = os.fstat(f.fileno()).st_size
        head = f.read(16)
        if len(head) != 16 or head[:8] != MAGIC:
            print(f'{path}: not a cache pack, rebuilt from the loose files only', flush=True)
            return {}
        count = struct.unpack_from('<I', head, 8)[0]
        raw = f.read(48 * count)
        if len(raw) != 48 * count:
            print(f'{path}: truncated index, rebuilt from the loose files only', flush=True)
            return {}
        out = {}
        for i in range(count):
            name, kind, length, offset = struct.unpack_from('<32sIIQ', raw, 48 * i)
            if offset + length <= size:
                out[(kind, name)] = (offset, length)
        return out


def valid(kind, data):
    return len(data) == USE_SIZE if kind == 3 else len(data) > 0


def pack(cache, keep=False):
    cache = Path(cache)
    out = cache / PACK
    packed = read_pack(out)
    loose = loose_files(cache)
    if not loose:
        # Nothing to fold in: every file of the cache is already in the pack, so writing it again copies the same
        # bytes over the same entries (1.37 GiB here, 2.6 s, and a file system that then has the old pack's blocks to
        # release as well). The installer runs this after every prepare.
        return len(packed), 0, 0, out.stat().st_size if out.is_file() else 0
    keys = sorted(set(packed) | set(loose))
    tmp = out.with_name(PACK + '.tmp')
    index, written, skipped, offset = [], [], 0, 16 + 48 * len(keys)
    old = open(out, 'rb') if packed else None
    try:
        with open(tmp, 'wb') as f:
            f.write(bytes(offset))   # header and index, written once the offsets are known
            for key in keys:
                data = None
                if key in loose:   # a valid loose file, else the packed copy, else nothing
                    try:
                        data = loose[key].read_bytes()
                    except OSError:
                        pass
                    if data is not None and valid(key[0], data):
                        written.append(loose[key])
                    else:
                        data = None
                if data is None and key in packed:
                    old.seek(packed[key][0])
                    data = old.read(packed[key][1])
                if data is None or not valid(key[0], data):
                    skipped += 1
                    continue
                f.write(data)
                index.append(struct.pack('<32sIIQ', key[1], key[0], len(data), offset))
                offset += len(data)
            f.seek(0)
            f.write(MAGIC + struct.pack('<II', len(index), 0) + b''.join(index))
            f.flush()
            os.fsync(f.fileno())
    finally:
        if old:
            old.close()
    os.replace(tmp, out)
    if not keep:
        for p in written:
            p.unlink(missing_ok=True)
        try:
            (cache / 'in').rmdir()   # only when empty
        except OSError:
            pass
    return len(index), len(written), skipped, offset


def main():
    ap = argparse.ArgumentParser(description='Fold a shader cache into shader-cache.pack.')
    ap.add_argument('cache', type=Path, help='the shader-cache folder next to totk.exe')
    ap.add_argument('--keep', action='store_true', help='keep the loose files')
    a = ap.parse_args()
    if not a.cache.is_dir():
        sys.exit(f'{a.cache} is not a folder')
    n, new, skipped, size = pack(a.cache, a.keep)
    print(f'{a.cache / PACK}: {n} files, {size / 2**20:.0f} MiB ({new} loose files folded in'
          f'{", " + str(skipped) + " unreadable or empty skipped" if skipped else ""})', flush=True)


if __name__ == '__main__':
    main()
