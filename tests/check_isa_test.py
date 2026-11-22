"""tools/check_isa.py finds instructions above x86-64-v2 in objects and in linked images.

    python tests/check_isa_test.py

The disassembly below is llvm-objdump -d --no-show-raw-insn output for one AVX2 function, once from an object (short
indented addresses) and once from a linked .exe (9+ hex digits at the start of the line). Standard library only.
"""

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_isa  # noqa: E402

OBJECT = '''
avx.o:\tfile format coff-x86-64

Disassembly of section .text:

0000000000000000 <sum8>:
       0:     \tvmovdqu\t(%rcx), %ymm0
       4:     \tvpaddd\t%ymm0, %ymm1, %ymm0
       8:     \tvzeroupper
       b:     \tretq
'''

LINKED = '''
avx.exe:\tfile format coff-x86-64

Disassembly of section .text:

00000001400014b0 <sum8>:
1400014b0:     \tvmovdqu\t(%rcx), %ymm0
1400014b4:     \tvpshufd\t$0x55, %ymm0, %ymm1     # ymm1 = ymm0[1,1,1,1,5,5,5,5]
1400014b9:     \tvpaddd\t%ymm0, %ymm1, %ymm0
1400014bd:     \tvzeroupper
1400014c0:     \tretq

00000001400014d0 <main>:
1400014d0:     \tsubq\t$0x28, %rsp
1400014d4:     \tpopcntl\t%eax, %eax
1400014d8:     \taddq\t$0x28, %rsp
1400014dc:     \tretq
'''


def scan(text):
    done = subprocess.CompletedProcess([], 0, text, '')
    with mock.patch.object(check_isa.subprocess, 'run', return_value=done):
        return check_isa.scan_one(Path('x'), 'llvm-objdump')


class ScanTest(unittest.TestCase):
    def test_an_object_is_scanned(self):
        hits = scan(OBJECT)
        self.assertEqual(set(hits), {('avx.o', 'sum8')})
        self.assertEqual(hits[('avx.o', 'sum8')]['vpaddd'], 1)

    def test_a_linked_image_is_scanned(self):
        hits = scan(LINKED)
        self.assertEqual(set(hits), {('avx.exe', 'sum8')})   # main's popcnt is x86-64-v2
        self.assertEqual(sum(hits[('avx.exe', 'sum8')].values()), 4)

    def test_function_headers_and_blank_lines_are_not_instructions(self):
        self.assertEqual(scan('\nDisassembly of section .text:\n\n00000001400014b0 <vfoo>:\n'), {})


if __name__ == '__main__':
    unittest.main()
