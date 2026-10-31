"""Compare folded cache-register reads with ARM64 execution, including flags."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x100000
MRS_CTR = 0xD53B0020
NOP = 0xD503201F


class CacheRegisterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='totk-ctr-')
        work = Path(cls.tmp.name)
        source = work / 'ctr.c'
        source.write_text('''
#include "native_cache_register.h"
ptrdiff_t fold(uint8_t *p, size_t text, size_t end, uint32_t value) {
    return native_fold_ctr(p, text, (text + 15) & ~(size_t)15, end, value);
}
bool branch(size_t from, size_t to, uint32_t *word) {
    return native_ctr_branch(from, to, word);
}
uint32_t host_value(void) { return NATIVE_CTR_EL0; }
''')
        library = work / 'ctr.dylib'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-dynamiclib', '-I', ROOT / 'runtime/macos',
                        source, '-o', library], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.fold.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.c_size_t, ctypes.c_uint32]
        cls.lib.fold.restype = ctypes.c_ssize_t
        cls.lib.branch.argtypes = [ctypes.c_size_t, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.branch.restype = ctypes.c_bool
        cls.lib.host_value.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fold(self, image, text, end=None, value=0x8555C005):
        out = ctypes.create_string_buffer(bytes(image), len(image))
        result = self.lib.fold(out, text, len(image) if end is None else end, value)
        return result, out.raw

    def run_arm(self, image, text):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(BASE, 0x1000)
        cpu.mem_write(BASE, bytes(image))
        rng = random.Random(417)
        registers = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)]
        registers += [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
        registers += [reg.UC_ARM64_REG_SP, reg.UC_ARM64_REG_NZCV]
        for register in registers:
            cpu.reg_write(register, rng.getrandbits(64))
        cpu.emu_start(BASE, BASE + text, count=1000)
        self.assertEqual(cpu.reg_read(reg.UC_ARM64_REG_PC), BASE + text)
        return tuple(cpu.reg_read(register) for register in registers)

    def test_all_destinations_match_cpu(self):
        # Unicorn's read-only CTR describes its emulated CPU. Compare with that
        # value; separately check the runtime's Apple Silicon cache geometry.
        reference = self.run_arm(struct.pack('<I', MRS_CTR), 4)[0]
        self.assertLess(reference, 1 << 32)
        for destination in range(32):
            with self.subTest(destination=destination):
                image = struct.pack('<I', MRS_CTR | destination) + bytes(124)
                count, folded = self.fold(image, 4, value=reference)
                self.assertEqual(count, 1)
                self.assertEqual(self.run_arm(image, 4), self.run_arm(folded, 4))

    def test_multiple_reads_and_existing_instructions(self):
        words = [MRS_CTR, NOP, MRS_CTR | 30, MRS_CTR | 31, NOP, MRS_CTR | 1]
        image = struct.pack('<6I', *words) + bytes(104)
        reference = self.run_arm(struct.pack('<I', MRS_CTR), 4)[0]
        count, folded = self.fold(image, 24, value=reference)
        self.assertEqual(count, 4)
        self.assertEqual(self.run_arm(image, 24), self.run_arm(folded, 24))
        self.assertEqual(folded[4:8], image[4:8])
        self.assertEqual(folded[16:20], image[16:20])

    def test_host_cache_geometry_and_encoding(self):
        value = self.lib.host_value()
        self.assertEqual(value, 0x8555C005)
        self.assertEqual(4 << (value & 15), 128)
        self.assertEqual(4 << ((value >> 16) & 15), 128)
        image = struct.pack('<I', MRS_CTR | 7) + bytes(124)
        _, folded = self.fold(image, 4, value=value)
        after = self.run_arm(folded, 4)
        self.assertEqual(after[7], value)

    def test_rejected_layouts_leave_every_byte_intact(self):
        image = struct.pack('<2I', MRS_CTR, MRS_CTR | 1) + bytes(56)
        for end in (0, 4, 8, 16, 27, 28, 39):
            with self.subTest(end=end):
                count, out = self.fold(image, 8, end=end)
                self.assertEqual(count, -1)
                self.assertEqual(out, image)
        # The last byte of the second stub is occupied: no partial first patch.
        occupied = bytearray(image)
        occupied[39] = 1
        count, out = self.fold(occupied, 8)
        self.assertEqual(count, -1)
        self.assertEqual(out, occupied)
        count, out = self.fold(image, 7)
        self.assertEqual(count, -1)
        self.assertEqual(out, image)

    def test_no_reads_need_no_padding(self):
        image = struct.pack('<2I', NOP, NOP)
        self.assertEqual(self.fold(image, 8), (0, image))

    def test_branch_boundaries(self):
        word = ctypes.c_uint32()
        limit = 1 << 27
        for source, target in ((0, limit - 4), (limit, 0), (16, 16)):
            self.assertTrue(self.lib.branch(source, target, ctypes.byref(word)))
            immediate = word.value & 0x3FFFFFF
            if immediate & 0x2000000:
                immediate -= 0x4000000
            self.assertEqual(source + 4 * immediate, target)
        for source, target in ((0, limit), (limit + 4, 0), (1, 4), (4, 1)):
            self.assertFalse(self.lib.branch(source, target, ctypes.byref(word)))


if __name__ == '__main__':
    unittest.main()
