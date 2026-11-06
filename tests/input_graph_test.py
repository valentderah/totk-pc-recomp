"""Instruction-reference analysis against synthetic encodings; no game files needed."""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from input_graph import direct_branches, materializations


class References(unittest.TestCase):
    def test_calls_tail_branches_and_sign(self):
        words = np.array([0x94000002, 0x17ffffff, 0xd65f03c0], dtype='<u4')
        source, dest, call = direct_branches(words, 0x1000)
        self.assertEqual(source.tolist(), [0x1000, 0x1004])
        self.assertEqual(dest.tolist(), [0x1008, 0x1000])
        self.assertEqual(call.tolist(), [True, False])

    def test_page_boundary_and_slot(self):
        # ADRP x8,current page at 0x1ffc; ADD x0,x8,#0x234; later ADRP x8,+1 page; LDR x9,[x8,#0x20].
        words = np.array([0x90000008, 0x9108d100, 0xb0000008, 0xf9401109], dtype='<u4')
        self.assertEqual(materializations(words, 0x1ffc),
                         [(0x1ffc, 0x1234, 'adrp-add'), (0x2004, 0x3020, 'adrp-ldr64-slot')])

    def test_negative_page_and_register_mismatch(self):
        words = np.array([0xf0ffffe8, 0x91000120], dtype='<u4')  # ADD uses x9, not x8.
        self.assertEqual(materializations(words, 0x2000), [])
        words[1] = 0x91000100  # ADD x0,x8,#0
        self.assertEqual(materializations(words, 0x2000), [(0x2000, 0x1000, 'adrp-add')])

    def test_interleaving_clobber_and_control_flow(self):
        # ADRP x8; MOV x19,x0; LDR x8,[x8,#0x20] retains x8 through the unrelated write.
        words = np.array([0x90000008, 0xaa0003f3, 0xf9401108], dtype='<u4')
        self.assertEqual(materializations(words, 0x2000), [(0x2000, 0x2020, 'adrp-ldr64-slot')])
        for intervening in (0xaa0003e8, 0x2a0003e8, 0x94000002, 0x14000002, 0xd65f03c0):
            words[1] = intervening
            self.assertEqual(materializations(words, 0x2000), [])
        # Capstone names x29/x30 as fp/lr; writes to either alias must also invalidate a page value.
        self.assertEqual(materializations(np.array([0x9000001d, 0xaa0003fd, 0xf94013a8], dtype='<u4'), 0x2000), [])
        words = np.array([0x90000008, 0xf9401109, 0xf940150a], dtype='<u4')
        self.assertEqual(materializations(words, 0x2000), [(0x2000, 0x2020, 'adrp-ldr64-slot'),
                                                         (0x2000, 0x2028, 'adrp-ldr64-slot')])


if __name__ == '__main__':
    unittest.main()
