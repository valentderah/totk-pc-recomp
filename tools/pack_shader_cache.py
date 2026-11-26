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
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from itertools import islice
import os
import re
import struct
import sys
import time
from pathlib import Path

import hostmem

MAGIC = b'TKSPACK1'
PACK = 'shader-cache.pack'
KINDS = {'.spv': 1, '.res.json': 2, '.use': 3, '.in': 4}
NAME = re.compile(r'([0-9a-f]{64})(\.spv|\.res\.json|\.use|\.in)')
USE_SIZE = 20   # gfx_shader.c use_store: magic, version, three masks
MAX_ENTRIES = 1 << 24   # gfx_shader.c pack_open's index limit
MAX_READERS = 8
# File data read ahead of the writer. The cache's files are 20 B to ~0.5 MiB (a 1.4.3 install: 117k files, 1.5 GiB),
# so the count limit (two files per reader) binds first; this cap keeps a cache of unusually large files from holding
# 16 of them in memory at once.
READ_AHEAD_BYTES = 32 << 20
# Below this much free RAM the files are read one at a time, as before the readers existed. It is 16 times the
# read-ahead cap, so the parallel path never asks for more than a sixteenth of what is free; under it the machine is
# already paging, and extra concurrent reads only compete with the page-ins of the other programs.
LOW_FREE_RAM = 512 << 20


def loose_files(cache):
    """({(kind, name): path}, {(kind, name): size}) of the cache's loose files; *.in only in in/. The sizes come with
    the directory listing on Windows and bound the read-ahead in prefetched_files."""
    found, sizes = {}, {}
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
                    key = (kind, bytes.fromhex(m.group(1)))
                    found[key] = Path(e.path)
                    sizes[key] = e.stat().st_size
    return found, sizes


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
        index_size = 48 * count
        # Check the count before read() allocates its buffer: a damaged header can request hundreds of GiB.
        if count > MAX_ENTRIES or index_size > size - 16:
            print(f'{path}: invalid index size, rebuilt from the loose files only', flush=True)
            return {}
        raw = f.read(index_size)
        if len(raw) != index_size:
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


def read_workers(cpus=None, free=None):
    """Readers for this machine: one per two logical CPUs (one per core on SMT CPUs), 1 to MAX_READERS, and 1 when
    less than LOW_FREE_RAM is free. A reader spends its time in the kernel and in the antivirus scan of each opened
    file, which runs on threads of its own, so half the logical CPUs keep the disk and the scanner busy and leave the
    rest to them and to the setup window; a 1- or 2-CPU machine reads one file at a time. cpus: os.cpu_count();
    free: hostmem.available_ram(), None when unknown."""
    if free is not None and free < LOW_FREE_RAM:
        return 1
    return min(MAX_READERS, max(1, (cpus or 1) // 2))


def machine_read_workers():
    """read_workers() for this machine and a line saying why, for the log."""
    cpus = os.cpu_count()
    try:
        free = hostmem.available_ram()
    except (OSError, AttributeError, ValueError):   # no answer from the OS: decide by the CPU count alone
        free = None
    n = read_workers(cpus, free)
    known = 'free memory unknown' if free is None else f'{free / 2**20:.0f} MiB free memory'
    return n, f'{n} reader{"s" if n > 1 else ""} ({cpus} CPUs, {known})'


def read_loose(path):
    if path is not None:
        try:
            return path.read_bytes()
        except OSError:
            pass
    return None


def parallel_reads(keys, loose, workers, sizes, max_bytes):
    """(key, data) in the order of keys, read on a pool of workers threads. At most 2 * workers files and max_bytes of
    their data (by the listed size) are read ahead of the caller; a file larger than max_bytes is read alone."""
    pool = ThreadPoolExecutor(max_workers=workers)
    try:
        pending, held = deque(), 0   # (key, future or None, bytes reserved), and the reserved bytes in total
        following = iter(keys)
        key = next(following, None)
        while True:
            while key is not None and len(pending) < 2 * workers:
                size = sizes.get(key, 0) if key in loose else 0
                if pending and held + size > max_bytes:
                    break
                pending.append((key, pool.submit(read_loose, loose[key]) if key in loose else None, size))
                held += size
                key = next(following, None)
            if not pending:
                return
            done, future, size = pending.popleft()
            data = future.result() if future else None
            held -= size
            yield done, data
    finally:
        pool.shutdown(wait=True, cancel_futures=True)


def prefetched_files(keys, loose, workers, sizes=None, max_bytes=READ_AHEAD_BYTES):
    """(key, loose file's bytes or None) for every key in order, the same bytes as a serial read so the pack comes out
    identical. workers > 1 reads ahead on threads (parallel_reads). Any failure of that path (a thread that does not
    start, a MemoryError in a reader, an executor error) switches to serial reads from the first key not yet given
    out, so the pack still completes; a file that cannot be read gives None in either path."""
    given = 0
    if workers > 1:
        reads = parallel_reads(keys, loose, workers, sizes or {}, max_bytes)
        while True:
            try:
                item = next(reads)
            except StopIteration:
                return
            except Exception as e:   # noqa: BLE001  the serial path below reads the rest
                print(f'pack-cache: parallel reads failed ({type(e).__name__}: {e}); reading the remaining '
                      f'{len(keys) - given} files one at a time', flush=True)
                try:
                    reads.close()
                except Exception:   # noqa: BLE001  the pool is dropped either way
                    pass
                break
            yield item   # outside the try: errors of the caller never reach the fallback
            given += 1
    for key in islice(keys, given, None):
        yield key, read_loose(loose.get(key))


def pack(cache, keep=False, read_workers=None, progress=None):
    """Fold the loose files into the pack. read_workers: concurrent file reads, 1..16; None picks them for the
    machine (machine_read_workers)."""
    if read_workers is None:
        read_workers = machine_read_workers()[0]
    if not 1 <= read_workers <= 16:
        raise ValueError('read_workers must be 1..16')
    cache = Path(cache)
    out = cache / PACK
    packed = read_pack(out)
    loose, sizes = loose_files(cache)
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
            for done, (key, data) in enumerate(prefetched_files(keys, loose, read_workers, sizes), 1):
                if progress and (done == len(keys) or done % 128 == 0):
                    progress('read', done, len(keys))
                if key in loose:   # a valid loose file, else the packed copy, else nothing
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
        for done, p in enumerate(written, 1):
            p.unlink(missing_ok=True)
            if progress and (done == len(written) or done % 128 == 0):
                progress('cleanup', done, len(written))
        try:
            (cache / 'in').rmdir()   # only when empty
        except OSError:
            pass
    return len(index), len(written), skipped, offset


def main():
    ap = argparse.ArgumentParser(description='Fold a shader cache into shader-cache.pack.')
    ap.add_argument('cache', type=Path, help='the shader-cache folder next to totk.exe')
    ap.add_argument('--keep', action='store_true', help='keep the loose files')
    ap.add_argument('--read-workers', type=int, choices=range(1, 17), metavar='1..16',
                    help='concurrent file reads (default: half the logical CPUs, 1 to 8, and 1 with less than '
                         f'{LOW_FREE_RAM >> 20} MiB free memory; 1 reads serially)')
    a = ap.parse_args()
    if not a.cache.is_dir():
        sys.exit(f'{a.cache} is not a folder')
    if a.read_workers:
        workers = a.read_workers
        print(f'pack-cache: {workers} reader{"s" if workers > 1 else ""} (--read-workers)', flush=True)
    else:
        workers, why = machine_read_workers()
        print(f'pack-cache: {why}', flush=True)
    last_progress = [0.0]
    def progress(phase, done, total):
        now = time.monotonic()
        if done == total or now - last_progress[0] >= 5:
            print(f'pack-cache: {phase}: {done} of {total} files', flush=True)
            last_progress[0] = now
    n, new, skipped, size = pack(a.cache, a.keep, workers, progress)
    print(f'{a.cache / PACK}: {n} files, {size / 2**20:.0f} MiB ({new} loose files folded in'
          f'{", " + str(skipped) + " unreadable or empty skipped" if skipped else ""})', flush=True)


if __name__ == '__main__':
    main()
