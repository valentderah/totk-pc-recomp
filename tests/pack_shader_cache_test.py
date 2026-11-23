"""tools/pack_shader_cache.py on a fake shader cache: the pack layout gfx_shader.c reads (sorted index, bounds, data),
loose files folded in and deleted, a second run merging new loose files, a loose file winning over its packed copy,
malformed files skipped and --keep. No game files; stdlib only.

Run: python tests/pack_shader_cache_test.py
"""
import hashlib
import struct
import sys
import tempfile
import unittest
import shutil
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import pack_shader_cache as psc  # noqa: E402

USE = struct.pack('<5I', 0x4d554b54, 1, 3, 0, 1)   # gfx_shader.c use_store


def name(seed):
    return hashlib.sha256(seed.encode()).hexdigest()


def read_pack(path):
    """{(kind, name hex): bytes}, checking what gfx_shader.c pack_open checks."""
    b = path.read_bytes()
    assert b[:8] == psc.MAGIC
    count = struct.unpack_from('<I', b, 8)[0]
    out, last = {}, None
    for i in range(count):
        nm, kind, size, offset = struct.unpack_from('<32sIIQ', b, 16 + 48 * i)
        assert offset + size <= len(b) and offset >= 16 + 48 * count
        assert last is None or (last[0], last[1]) < (kind, nm), 'index not sorted by (kind, name)'
        last = (kind, nm)
        out[(kind, nm.hex())] = b[offset:offset + size]
    return out


class PackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name) / 'shader-cache'
        (self.cache / 'in').mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def shader(self, seed, spv=b'\x03\x02\x23\x07spirv', res=b'{"x":1}'):
        k = name(seed)
        (self.cache / f'{k}.spv').write_bytes(spv + seed.encode())
        (self.cache / f'{k}.res.json').write_bytes(res)
        (self.cache / f'{name("use" + seed)}.use').write_bytes(USE)
        (self.cache / 'in').mkdir(exist_ok=True)   # in_store makes it again after a pack removed it
        (self.cache / 'in' / f'{name("in" + seed)}.in').write_bytes(b'TKSI' + seed.encode())
        return k

    def test_pack_holds_every_file_and_deletes_the_loose_ones(self):
        keys = [self.shader(str(i)) for i in range(50)]
        (self.cache / 'notes.txt').write_text('kept')
        (self.cache / f'{keys[0]}.code').write_bytes(b'kept too')
        n, new, skipped, _ = psc.pack(self.cache)
        self.assertEqual((n, new, skipped), (200, 200, 0))
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(1, keys[3])], b'\x03\x02\x23\x07spirv3')
        self.assertEqual(got[(2, keys[3])], b'{"x":1}')
        self.assertEqual(got[(3, name('use3'))], USE)
        self.assertEqual(got[(4, name('in3'))], b'TKSI3')
        left = sorted(p.name for p in self.cache.iterdir())
        self.assertEqual(left, sorted([psc.PACK, 'notes.txt', f'{keys[0]}.code']))   # in/ removed when empty

    def test_second_run_merges_new_loose_files_and_loose_wins(self):
        a = self.shader('a')
        psc.pack(self.cache)
        b = self.shader('b')
        (self.cache / f'{a}.res.json').write_bytes(b'{"newer":1}')
        n, new, _, _ = psc.pack(self.cache)
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(n, 8)
        self.assertEqual(new, 5)
        self.assertEqual(got[(2, a)], b'{"newer":1}')
        self.assertEqual(got[(1, b)], b'\x03\x02\x23\x07spirvb')

    def test_malformed_files_are_skipped_or_replaced_by_the_packed_copy(self):
        a = self.shader('a')
        psc.pack(self.cache)
        (self.cache / f'{name("usea")}.use').write_bytes(b'short')   # bad loose copy: the packed one stays
        (self.cache / f'{name("bad")}.use').write_bytes(b'short')    # bad and nowhere else: skipped
        (self.cache / f'{name("empty")}.spv').write_bytes(b'')
        n, _, skipped, _ = psc.pack(self.cache)
        got = read_pack(self.cache / psc.PACK)
        self.assertEqual(got[(3, name('usea'))], USE)
        self.assertNotIn((3, name('bad')), got)
        self.assertEqual((n, skipped), (4, 2))
        self.assertTrue((self.cache / f'{name("bad")}.use').exists())   # never deleted unless packed

    def test_keep_leaves_the_loose_files(self):
        k = self.shader('a')
        psc.pack(self.cache, keep=True)
        self.assertTrue((self.cache / f'{k}.spv').exists())
        self.assertEqual(len(read_pack(self.cache / psc.PACK)), 4)

    def test_parallel_pack_is_byte_identical_for_fresh_and_incremental_inputs(self):
        for i in range(80):
            self.shader(str(i))
        serial = self.cache.parent / 'serial'
        shutil.copytree(self.cache, serial)
        self.assertEqual(psc.pack(self.cache, read_workers=8), psc.pack(serial, read_workers=1))
        self.assertEqual((self.cache / psc.PACK).read_bytes(), (serial / psc.PACK).read_bytes())
        self.shader('new')
        (self.cache / f'{name("0")}.res.json').write_bytes(b'{"changed":true}')
        (self.cache / f'{name("use1")}.use').write_bytes(b'invalid loose copy')
        shutil.copytree(self.cache, serial, dirs_exist_ok=True)
        self.assertEqual(psc.pack(self.cache, read_workers=8), psc.pack(serial, read_workers=1))
        self.assertEqual((self.cache / psc.PACK).read_bytes(), (serial / psc.PACK).read_bytes())

    def test_prefetch_preserves_order_and_bounds_inflight_reads(self):
        keys = [(1, bytes([i])) for i in range(100)]
        # The first yielded result must not require reading every file; only the bounded prefetch can start.
        from unittest.mock import patch
        with patch.object(psc, 'read_loose', return_value=b'data') as read:
            iterator = psc.prefetched_files(keys, dict.fromkeys(keys, Path('fixture')), 3)
            self.assertEqual(next(iterator), (keys[0], b'data'))
            self.assertLessEqual(read.call_count, 6)
            self.assertEqual([key for key, _ in iterator], keys[1:])

    def test_a_broken_pack_is_rebuilt_from_the_loose_files(self):
        self.shader('a')
        (self.cache / psc.PACK).write_bytes(b'garbage')
        n, _, _, _ = psc.pack(self.cache)
        self.assertEqual(n, 4)

    def bounded_read_pack(self, path):
        """Record real file reads; fail before a corrupt count can request a large allocation, even before the fix."""
        requests = []
        with path.open('rb') as f:
            read = f.read

            def bounded(n):
                requests.append(n)
                self.assertLessEqual(n, 1024, 'corrupt index count reached the allocating read')
                return read(n)

            with mock.patch.object(psc, 'open', return_value=f, create=True), \
                    mock.patch.object(f, 'read', side_effect=bounded):
                result = psc.read_pack(path)
        return result, requests

    def test_truncated_index_is_rejected_before_reading_it(self):
        path = self.cache / psc.PACK
        for count in (1, 0xffffffff):
            with self.subTest(count=count):
                path.write_bytes(psc.MAGIC + struct.pack('<II', count, 0))
                result, requests = self.bounded_read_pack(path)
                self.assertEqual(result, {})
                self.assertEqual(requests, [16])

    def test_runtime_index_limit_is_checked_before_reading_it(self):
        path = self.cache / psc.PACK
        count = (1 << 24) + 1   # gfx_shader.c pack_open rejects more than 2**24 entries
        path.write_bytes(psc.MAGIC + struct.pack('<II', count, 0))
        with mock.patch.object(psc.os, 'fstat', return_value=SimpleNamespace(st_size=16 + 48 * count)):
            result, requests = self.bounded_read_pack(path)
        self.assertEqual(result, {})
        self.assertEqual(requests, [16])

    def test_short_read_after_the_size_check_is_still_rejected(self):
        path = self.cache / psc.PACK
        path.write_bytes(psc.MAGIC + struct.pack('<II', 1, 0) + bytes(24))
        with mock.patch.object(psc.os, 'fstat', return_value=SimpleNamespace(st_size=64)):
            result, requests = self.bounded_read_pack(path)
        self.assertEqual(result, {})
        self.assertEqual(requests, [16, 48])

    def test_a_truncated_pack_is_rebuilt_from_the_loose_files(self):
        key = self.shader('a')
        (self.cache / psc.PACK).write_bytes(psc.MAGIC + struct.pack('<II', 4096, 0))
        n, new, skipped, size = psc.pack(self.cache)
        self.assertEqual((n, new, skipped), (4, 4, 0))
        self.assertEqual(size, (self.cache / psc.PACK).stat().st_size)
        self.assertEqual(read_pack(self.cache / psc.PACK)[(1, key)], b'\x03\x02\x23\x07spirva')

    def test_a_valid_empty_pack_is_not_rewritten(self):
        path = self.cache / psc.PACK
        data = psc.MAGIC + struct.pack('<II', 0, 0)
        path.write_bytes(data)
        before = path.stat().st_mtime_ns
        self.assertEqual(psc.pack(self.cache), (0, 0, 0, len(data)))
        self.assertEqual(path.read_bytes(), data)
        self.assertEqual(path.stat().st_mtime_ns, before)


if __name__ == '__main__':
    unittest.main()
