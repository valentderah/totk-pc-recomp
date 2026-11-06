"""tools/exefs_patches.py on modules it makes itself: code writes become sites, data writes become runs of the data file
as emulators apply a patch file before the dynamic linker (plain bytes, DT_RELR words rebased, writes into words an
explicit-addend relocation fills dropped), the writes it refuses (outside the image, into the linker's tables or a
lazily bound pointer), the status and data files, and the check after the lift against the bytes recomp/gen.py lists in
folded.txt, with gen.py run on a module with one jump table. No game files.

Run with .venv (lz4 and numpy, as recomp/): python tests/exefs_patches_test.py
"""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import exefs_patches as ep  # noqa: E402

BUILD_ID = bytes.fromhex('0123456789abcdef') + bytes(24)
BID_NAME = '0123456789ABCDEF'   # an IPS file name may leave the build id's trailing zeros out
MOV_W8_100, MOV_W8_200, RET = 0x52802008, 0x52804008, 0xD65F03C0
R_RELATIVE, R_GLOB_DAT, R_JUMP_SLOT = 1027, 1025, 1026


def nso(text, ro, data, bss=0x1000):
    """An uncompressed NSO with .text at 0, .rodata at 0x1000 and .data at 0x2000."""
    segs = [(0x0, text), (0x1000, ro), (0x2000, data)]
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off = 0x100
    for i, (mem, d) in enumerate(segs):
        struct.pack_into('<IIII', hdr, 0x10 + 0x10 * i, off, mem, len(d), bss if i == 2 else 0)
        struct.pack_into('<I', hdr, 0x60 + 4 * i, len(d))
        off += len(d)
    hdr[0x40:0x60] = BUILD_ID
    return bytes(hdr) + b''.join(d for _, d in segs)


def put(buf, off, data):
    buf[off:off + len(data)] = data


def reloc_module():
    """fn at 0x8 (mov w8, #0x100; ret); .rodata: constants at 0x1000, MOD0 at 0x1100 and the relocation tables at
    0x1200 (RELA: a RELATIVE word at 0x2100, a GLOB_DAT word at 0x2108), 0x1300 (JMPREL: a JUMP_SLOT word at 0x2110)
    and 0x1400 (RELR: the word at 0x2118, addend 0x30); .data: .dynamic at 0x2000, plain bytes at 0x2200."""
    text = struct.pack('<4I', 0x14000002, 0x1100, MOV_W8_100, RET)
    ro = bytearray(0x500)
    put(ro, 0x000, b'constant bytes!!')
    put(ro, 0x100, b'MOD0' + struct.pack('<6i', 0x2000 - 0x1100, 0, 0, -0x1100, -0x1100, 0))
    put(ro, 0x200, struct.pack('<QQq', 0x2100, R_RELATIVE, 0x8) + struct.pack('<QQq', 0x2108, R_GLOB_DAT, 0))
    put(ro, 0x300, struct.pack('<QQq', 0x2110, R_JUMP_SLOT, 0))
    put(ro, 0x400, struct.pack('<Q', 0x2118))
    data = bytearray(0x300)
    dyn = [(7, 0x1200), (8, 48), (23, 0x1300), (2, 24), (0x24, 0x1400), (0x23, 8), (0, 0)]
    put(data, 0x000, b''.join(struct.pack('<QQ', t, v) for t, v in dyn))
    put(data, 0x118, struct.pack('<Q', 0x30))
    put(data, 0x200, b'ABCDEFGH')
    return nso(text, bytes(ro), bytes(data))


# A module with one jump table (clang's compressed form), assembled from:
#        b fn                         ; 0x0
#        .word mod0                   ; 0x4: the MOD0 offset
#   fn:  cmp w0, #3                   ; 0x8
#        b.hi Ldefault
#        adrp x8, table               ; table = 0x1020: .byte 0, 2, 4, 6
#        add x8, x8, #0x20
#        adr x9, L0
#        ldrb w10, [x8, w0, uxtw]
#        add x9, x9, x10, lsl #2
#        br x9
#   L0..L3: mov w0, #10/20/30/40; ret ; Ldefault: mov w0, #0; ret
# .rodata: 32 bytes of text before the table, 21 after it, MOD0 at 0x1040 and a RELATIVE relocation to fn (the
# lifter's function seed); .data: .dynamic (DT_RELA) and the relocated word.
JT_TEXT = bytes.fromhex(
    '02000014401000001f0c0071e8010054080000b008810091890000100a49603829090a8b20011fd640018052c0035fd680028052'
    'c0035fd6c0038052c0035fd600058052c0035fd600008052c0035fd6')
JT_RO = bytes.fromhex(
    '756e746f7563686564206279746573206265666f726520746865207461626c6500020406627974657320616674657220746865'
    '207461626c65000000000000004d4f4430c00f00000000000000000000c0efffffc0efffff00000000000000003020000000000000'
    '03040000000000000800000000000000')
JT_DATA = struct.pack('<7Q', 7, 0x1060, 8, 24, 0, 0, 0)   # DT_RELA, DT_RELASZ, DT_NULL; the word the RELATIVE fills


def ips32(writes):
    """An IPS32 file of {image offset: bytes} (offsets count the NSO header, as emulators read them)."""
    body = b''.join((off + ep.NSO_HEADER).to_bytes(4, 'big') + len(d).to_bytes(2, 'big') + d
                    for off, d in sorted(writes.items()))
    return b'IPS32' + body + b'EEOF'


class Classify(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        path = Path(cls.tmp.name) / 'main'
        path.write_bytes(reloc_module())
        cls.m = ep.Main(path)
        cls.md = ep.MainData(cls.m)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_file(self, writes, md=True):
        return ep.classify(self.m, self.md if md else None, self.m.nso.build_id, Path(f'{BID_NAME}.ips'),
                           ips32(writes))

    def test_relocated_words(self):
        self.assertEqual(self.md.rela, {0x2100: R_RELATIVE, 0x2108: R_GLOB_DAT, 0x2110: R_JUMP_SLOT})
        self.assertEqual(self.md.relr, {0x2118})

    def test_code_only(self):
        kind, note, sites, runs, dropped = self.run_file({0x8: struct.pack('<I', MOV_W8_200)})
        self.assertEqual((kind, note, runs, dropped), ('code', '', [], 0))
        self.assertEqual(sites, {0x8: [MOV_W8_200]})

    def test_data_only(self):
        kind, note, sites, runs, dropped = self.run_file({0x1000: b'\xAA\xBB', 0x2200: b'XY'})
        self.assertEqual((kind, sites, dropped), ('data', {}, 0))
        self.assertEqual(runs, [(0x1000, b'\xAA\xBB', False), (0x2200, b'XY', False)])

    def test_code_and_data(self):
        kind, _, sites, runs, _ = self.run_file({0x8: struct.pack('<I', MOV_W8_200), 0x1004: b'\x01'})
        self.assertEqual(kind, 'code')
        self.assertEqual(sites, {0x8: [MOV_W8_200]})
        self.assertEqual(runs, [(0x1004, b'\x01', False)])

    def test_same_bytes_are_nothing(self):
        kind, _, _, runs, _ = self.run_file({0x1000: b'constant'})
        self.assertEqual((kind, runs), ('empty', []))

    def test_explicit_addend_words_change_nothing(self):
        kind, _, _, runs, dropped = self.run_file({0x2100: b'\x11\x22\x33\x44', 0x2108: b'\x55'})
        self.assertEqual((kind, runs, dropped), ('empty', [], 5))

    def test_relr_word_is_rebased(self):
        kind, _, _, runs, _ = self.run_file({0x2118: b'\x31'})
        self.assertEqual(kind, 'data')
        self.assertEqual(runs, [(0x2118, struct.pack('<Q', 0x31), True)])

    def test_long_runs_are_split(self):
        _, _, _, runs, _ = self.run_file({0x2200: bytes(i % 255 + 1 for i in range(ep.RUN_MAX)) + b'\xFF'})
        self.assertEqual([(o, len(d)) for o, d, _ in runs], [(0x2200, ep.RUN_MAX), (0x2200 + ep.RUN_MAX, 1)])

    def test_refused_writes(self):
        for writes, why in (({0x2110: b'\x01'}, 'binds lazily'), ({0x2000: b'\x01'}, 'linker\'s tables'),
                            ({0x1208: b'\x01'}, 'linker\'s tables'), ({0x4000: b'\x01'}, 'outside main\'s image'),
                            ({-0x80: b'\x01'}, 'outside main\'s image')):
            kind, note, sites, runs, _ = self.run_file({0x8: struct.pack('<I', MOV_W8_200), **writes})
            self.assertEqual((kind, sites, runs), ('unsupported', {}, []), note)
            self.assertIn(why, note)

    def test_data_without_a_data_file(self):
        kind, note, _, _, _ = self.run_file({0x1000: b'\xAA'}, md=False)
        self.assertEqual(kind, 'unsupported')
        self.assertIn('only code patches are supported', note)

    def test_pchtxt(self):
        text = (f'@nsobid-{BID_NAME}\n@flag offset_shift 0x100\n@enabled\n'
                f'00001000 AABB\n00002200 "XY"\n@disabled\n00002204 CC\n')
        kind, _, _, runs, _ = ep.classify(self.m, self.md, self.m.nso.build_id, Path('x.pchtxt'), text.encode())
        self.assertEqual(kind, 'data')
        self.assertEqual(runs, [(0x1000, b'\xAA\xBB', False), (0x2200, b'XY', False)])


class Tool(unittest.TestCase):
    """The command line: build, then the check after the lift."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / 'exefs').mkdir()
        (self.d / 'patches.txt').write_text('# no sets of the port\n')

    def tearDown(self):
        self.tmp.cleanup()

    def mod_file(self, mod, writes):
        f = self.d / 'mods' / mod / 'exefs' / f'{BID_NAME}.ips'
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(ips32(writes))

    def tool(self, *args):
        r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'exefs_patches.py'), '--exefs', str(self.d / 'exefs'),
                            *map(str, args)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def build(self):
        self.tool('--mods', self.d / 'mods', '--patches', self.d / 'patches.txt', '--out', self.d / 'out.txt',
                  '--status', self.d / 'exefs.status', '--data', self.d / 'exefs.data')
        status = {f[0]: f for f in (line.split('\t') for line in (self.d / 'exefs.status').read_text().splitlines())}
        data = [line.split('\t') for line in (self.d / 'exefs.data').read_text().splitlines()
                if not line.startswith('#')]
        return status, data

    def key(self, mod):
        return ep.fnv(f'{mod}/exefs/{BID_NAME}.ips'.encode())

    def test_build_and_check(self):
        (self.d / 'exefs' / 'main').write_bytes(reloc_module())
        self.mod_file('Code', {0x8: struct.pack('<I', MOV_W8_200), 0x2200: b'Q'})
        self.mod_file('Data', {0x1000: b'\xAA', 0x2118: b'\x31'})
        self.mod_file('Bad', {0x2110: b'\x01'})
        status, data = self.build()
        code, dat, bad = self.key('Code'), self.key('Data'), self.key('Bad')
        self.assertEqual([status[k][2] for k in (code, dat, bad)], ['code', 'data', 'unsupported'])
        self.assertEqual([status[k][4] for k in (code, dat)], ['data 1', 'data 9'])   # bytes its data lines hold
        self.assertIn(f'set mod_{code.lower()} TOTK_MODPATCH_{code} 0', (self.d / 'out.txt').read_text())
        self.assertEqual(sorted((f[0], f[2], f[3], f[4:]) for f in data),
                         sorted([(code, '2200', '51', []), (dat, '1000', 'aa', []),
                                 (dat, '2118', '3100000000000000', ['base'])]))
        # after the lift: .rodata bytes the lifted code read make the Data file unsupported; the Code file's data is
        # in .data, which the lifter never reads
        folded = self.d / 'folded.txt'
        folded.write_text('# test\n1000 1001\n')
        self.tool('--status', self.d / 'exefs.status', '--data', self.d / 'exefs.data', '--folded', folded)
        status2 = {f[0]: f for f in (line.split('\t') for line in (self.d / 'exefs.status').read_text().splitlines())}
        self.assertEqual(status2[code][2], 'code')
        self.assertEqual(status2[dat][2:4], ['unsupported', '-'])
        self.assertIn('jump table', status2[dat][4])
        left = [line.split('\t')[0] for line in (self.d / 'exefs.data').read_text().splitlines()
                if not line.startswith('#')]
        self.assertEqual(left, [code])

    def test_check_without_the_lift_list(self):
        (self.d / 'exefs' / 'main').write_bytes(reloc_module())
        self.mod_file('Ro', {0x1000: b'\xAA'})
        self.mod_file('Rw', {0x2200: b'Q'})
        self.build()
        self.tool('--status', self.d / 'exefs.status', '--data', self.d / 'exefs.data', '--folded',
                  self.d / 'missing.txt')
        status = {f[0]: f[2] for f in (line.split('\t') for line in (self.d / 'exefs.status').read_text().splitlines())}
        self.assertEqual((status[self.key('Ro')], status[self.key('Rw')]), ('unsupported', 'data'))

    def test_jump_table_through_the_lift(self):
        """recomp/gen.py lists the jump table's bytes in folded.txt; a write into them is refused, one beside them
        is kept."""
        (self.d / 'exefs' / 'main').write_bytes(nso(JT_TEXT, JT_RO, JT_DATA))
        r = subprocess.run([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                            '--out', str(self.d / 'gen'), '--modules', 'main', '--jobs', '1'],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        folded = self.d / 'gen' / 'main' / 'folded.txt'
        self.assertEqual(ep.read_ranges(folded), [(0x1020, 0x1024)])
        self.mod_file('Table', {0x1022: b'\x00'})
        self.mod_file('Beside', {0x1024: b'B'})
        self.build()
        self.tool('--status', self.d / 'exefs.status', '--data', self.d / 'exefs.data', '--folded', folded)
        status = {f[0]: f[2] for f in (line.split('\t') for line in (self.d / 'exefs.status').read_text().splitlines())}
        self.assertEqual((status[self.key('Table')], status[self.key('Beside')]), ('unsupported', 'data'))


if __name__ == '__main__':
    unittest.main()
