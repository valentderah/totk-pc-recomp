"""tools/ncz.py and the installer's unpacking of NSZ input (installer/steps.py, Check.unpack) on small made-up files:
an NCA with an AES-CTR section and a plain one, stored as an NCZ with one zstd stream or as NCZBLOCK blocks.

Run: .venv-py314\\Scripts\\python tests/ncz_test.py
"""
import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest
from compression import zstd
from pathlib import Path
from types import SimpleNamespace

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import ncz  # noqa: E402
from installer import steps  # noqa: E402
from installer.events import Reporter  # noqa: E402

KEY, COUNTER = bytes(range(16)), bytes(range(16, 32))
# (offset, size, crypto type): an AES-CTR section, then a plain one
SECTIONS = ((0x4000, 0x9000, 3), (0xD000, 0x3200, 1))


def pfs0(files):
    """An NSP of (name, data) pairs."""
    strings = b''.join(name.encode() + b'\0' for name, _ in files)
    strings += bytes(-(16 + 0x18 * len(files) + len(strings)) % 0x20)
    head = b'PFS0' + struct.pack('<III', len(files), len(strings), 0)
    offset = name_at = 0
    for name, data in files:
        head += struct.pack('<QQII', offset, len(data), name_at, 0)
        offset += len(data)
        name_at += len(name) + 1
    return head + strings + b''.join(data for _, data in files)


def made_up_nca():
    """(the NCA as stored on the console, its body after the header with the sections decrypted)."""
    rand = os.urandom
    plain = rand(0x2000) + bytes(0x7000)    # half random, half compressible
    rest = bytes(0x1200) + rand(0x2000)
    counter = COUNTER[:8] + struct.pack('>Q', 0x4000 >> 4)
    secret = Cipher(algorithms.AES(KEY), modes.CTR(counter)).encryptor().update(plain)
    header = rand(0x4000)
    return header + secret + rest, plain + rest


def ncz_file(nca, body, blocks=False):
    out = nca[:0x4000] + b'NCZSECTN' + struct.pack('<Q', len(SECTIONS))
    for offset, size, crypto in SECTIONS:
        out += struct.pack('<QQQQ', offset, size, crypto, 0) + KEY + COUNTER
    if not blocks:
        return out + zstd.compress(body)
    exponent = 14
    pieces = [body[i:i + (1 << exponent)] for i in range(0, len(body), 1 << exponent)]
    stored = [zstd.compress(p) if p.count(0) > len(p) // 2 else p for p in pieces]   # random blocks stay as they are
    out += b'NCZBLOCK' + bytes([2, 1, 0, exponent]) + struct.pack('<IQ', len(pieces), len(body))
    out += struct.pack(f'<{len(stored)}I', *map(len, stored))
    return out + b''.join(stored)


class BlocksTest(unittest.TestCase):
    def read(self, data, plain=1 << 14):
        header = b'NCZBLOCK' + bytes([2, 1, 0, 14]) + struct.pack('<IQI', 1, plain, len(data))
        return ncz.blocks(io.BytesIO(header + data), 0, len(header) + len(data), plain)

    def test_oversized_compressed_block_is_rejected_before_yielding(self):
        data = zstd.compress(bytes(1 << 20))   # a small fixture, 64 times the declared output
        with self.assertRaisesRegex(ValueError, 'NCZ block decompressed size'):
            next(self.read(data))

    def test_short_compressed_block_is_rejected_before_yielding(self):
        with self.assertRaisesRegex(ValueError, 'NCZ block decompressed size'):
            next(self.read(zstd.compress(bytes(100))))

    def test_compressed_block_can_contain_multiple_frames(self):
        plain = bytes(1 << 14)
        data = zstd.compress(plain[:100]) + zstd.compress(plain[100:])
        self.assertEqual(b''.join(self.read(data)), plain)

    def test_compressed_final_block_uses_its_actual_plain_size(self):
        plain = bytes(123)
        self.assertEqual(b''.join(self.read(zstd.compress(plain), len(plain))), plain)

    def test_compressed_block_must_reach_its_end_marker(self):
        data = zstd.compress(bytes(1 << 14), options={zstd.CompressionParameter.checksum_flag: True})
        with self.assertRaises((zstd.ZstdError, EOFError)):
            next(self.read(data[:-1]))


class UnpackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.nca, self.body = made_up_nca()
        self.name = hashlib.sha256(self.nca).hexdigest()[:32]
        self.tik = b'ticket' * 40

    def tearDown(self):
        self.tmp.cleanup()

    def nsz(self, blocks=False, damage=None):
        data = ncz_file(self.nca, self.body, blocks)
        if damage:
            data = damage(bytearray(data))
        path = self.dir / 'game.nsz'
        path.write_bytes(pfs0([(self.name + '.ncz', bytes(data)), ('a.tik', self.tik)]))
        return path

    def expected(self):
        return pfs0([(self.name + '.nca', self.nca), ('a.tik', self.tik)])

    def padded_nsz(self, indices, truncate=False):
        """Store selected blocks raw, followed by padding included in the stored length, not the NCA body."""
        def pad(data):
            head = ncz.HEADER + 16 + 0x40 * len(SECTIONS)
            exponent = data[head + 11]
            count = struct.unpack_from('<I', data, head + 12)[0]
            for index in indices:
                index %= count
                lengths = struct.unpack_from(f'<{count}I', data, head + 24)
                start = head + 24 + 4 * count + sum(lengths[:index])
                plain = self.body[index << exponent:(index + 1) << exponent]
                padding = bytes(max(16, (1 << exponent) - len(plain)))
                stored = plain + padding
                data[start:start + lengths[index]] = stored
                struct.pack_into('<I', data, head + 24 + 4 * index, len(stored))
            return data[:-1] if truncate else data

        return self.nsz(blocks=True, damage=pad)

    def test_stream_comes_back_byte_for_byte(self):
        src = self.nsz()
        self.assertEqual(ncz.compressed(src)[0], 'nsz')
        self.assertEqual(ncz.unpacked_size(src), len(self.expected()))
        seen = []
        self.assertEqual(ncz.unpack(src, self.dir / 'out.nsp', lambda d, t: seen.append((d, t))), 'nsz')
        self.assertEqual((self.dir / 'out.nsp').read_bytes(), self.expected())
        self.assertEqual(seen[-1], (len(self.expected()), len(self.expected())))
        self.assertFalse((self.dir / 'out.nsp.part').exists())

    def test_blocks_come_back_byte_for_byte(self):
        ncz.unpack(self.nsz(blocks=True), self.dir / 'out.nsp')
        self.assertEqual((self.dir / 'out.nsp').read_bytes(), self.expected())

    def test_raw_block_padding_is_not_written_into_the_nca(self):
        for indices in ((0,), (-1,), (0, -1)):
            with self.subTest(indices=indices):
                seen = []
                ncz.unpack(self.padded_nsz(indices), self.dir / 'out.nsp', lambda d, t: seen.append((d, t)))
                self.assertEqual((self.dir / 'out.nsp').read_bytes(), self.expected())
                self.assertEqual(seen[-1], (len(self.expected()), len(self.expected())))

    def test_a_raw_block_cannot_claim_padding_outside_its_entry(self):
        with self.assertRaisesRegex(ValueError, 'Truncated NCZ block'):
            ncz.unpack(self.padded_nsz((-1,), truncate=True), self.dir / 'out.nsp')
        self.assertFalse((self.dir / 'out.nsp').exists())

    def test_block_of_the_wrong_size_is_a_damaged_file(self):
        """The installer turns DamagedFile into check.input_damaged, which asks for a fresh copy of the file."""
        def short(data):
            head = ncz.HEADER + 16 + 0x40 * len(SECTIONS)
            count = struct.unpack_from('<I', data, head + 12)[0]
            lengths = struct.unpack_from(f'<{count}I', data, head + 24)
            start = head + 24 + 4 * count + lengths[0]
            frame = zstd.compress(self.body[1 << 14:(2 << 14) - 100])   # block 1 is zeros, stored compressed
            data[start:start + lengths[1]] = frame
            struct.pack_into('<I', data, head + 24 + 4, len(frame))
            return data

        with self.assertRaises(ncz.DamagedFile) as e:
            ncz.unpack(self.nsz(blocks=True, damage=short), self.dir / 'out.nsp')
        self.assertIn('decompressed size', e.exception.detail)
        self.assertFalse((self.dir / 'out.nsp').exists())

    def test_plain_nsp_is_not_compressed(self):
        path = self.dir / 'plain.nsp'
        path.write_bytes(self.expected())
        self.assertIsNone(ncz.compressed(path))

    def test_changed_byte_fails_the_hash(self):
        def flip(data):
            data[0x4000 + 16 + 0x40 * len(SECTIONS) + 0x100] ^= 1   # inside a stored (random) block
            return data
        with self.assertRaises(ncz.DamagedFile) as e:
            ncz.unpack(self.nsz(blocks=True, damage=flip), self.dir / 'out.nsp')
        self.assertIn('hash mismatch', e.exception.detail)

    def test_cut_stream_is_damaged(self):
        with self.assertRaises(ValueError):
            ncz.unpack(self.nsz(damage=lambda d: d[:-40]), self.dir / 'out.nsp')
        self.assertFalse((self.dir / 'out.nsp').exists())


class CheckUnpackTest(unittest.TestCase):
    """Check.unpack: an NSZ base becomes <temp>/unpacked/base.nsp, a plain NSP update is read in place, a second run
    reuses the NSP."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        nca, body = made_up_nca()
        name = hashlib.sha256(nca).hexdigest()[:32]
        self.base, self.update = base / 'base.nsz', base / 'update.nsp'
        self.base.write_bytes(pfs0([(name + '.ncz', ncz_file(nca, body))]))
        self.update.write_bytes(pfs0([('a.tik', b'x' * 64)]))
        self.expected = pfs0([(name + '.nca', nca)])
        self.out = io.StringIO()
        self.ctx = SimpleNamespace(options=SimpleNamespace(base=str(self.base), update=str(self.update)),
                                   temp=base / 'temp', reporter=Reporter(out=self.out, log_path=base / 'x.log'))

    def tearDown(self):
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def test_unpacks_the_base_and_reuses_it(self):
        steps.Check().unpack(self.ctx)
        target = self.ctx.temp / 'unpacked' / 'base.nsp'
        self.assertEqual(target.read_bytes(), self.expected)
        self.assertEqual(steps.game_file(self.ctx, 'base'), target)
        self.assertEqual(steps.game_file(self.ctx, 'update'), self.update)
        self.assertIn('"phase": "unpack_base"', self.out.getvalue())
        mtime = target.stat().st_mtime_ns
        steps.Check().unpack(self.ctx)
        self.assertEqual(target.stat().st_mtime_ns, mtime)

    def test_content_decides_not_the_name(self):
        renamed = self.base.with_name('base.nsp')
        self.base.rename(renamed)
        self.ctx.options.base = str(renamed)
        steps.Check().unpack(self.ctx)
        self.assertEqual((self.ctx.temp / 'unpacked' / 'base.nsp').read_bytes(), self.expected)

    def test_damaged_base_names_the_chosen_file(self):
        data = bytearray(self.base.read_bytes())
        data[-100] ^= 0xFF
        self.base.write_bytes(bytes(data))
        with self.assertRaises(steps.InstallError) as e:
            steps.Check().unpack(self.ctx)
        self.assertIn(e.exception.id, ('check.input_damaged', 'check.unpack_failed'))


if __name__ == '__main__':
    unittest.main()
