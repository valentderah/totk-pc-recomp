"""recomp/layout.py with and without mod plugins, and tools/exefs_plugins.py's check of a plugin service run record:
a synthetic exlaunch-shaped record with one inline hook that passes, and one bad record per rule; the check of the
code the hooks reach; the patch lines of the three hook kinds; and what recomp/patches.py and recomp/gen.py make of
those lines (the hook call, the stub's return, the unpatched copy a replace hook's trampoline reaches, the start-up
record in patches.h). The start-up comparison itself runs in the game (runtime/plugin_init.c plugins_start); its
mods.c side is in totk.exe --probe-mods. No game files.

Run with .venv (lz4 and numpy, as recomp/): python tests/exefs_plugins_test.py
"""
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'recomp'))
import exefs_plugins as xp  # noqa: E402
import gen  # noqa: E402
import layout  # noqa: E402
import patches  # noqa: E402
from nso import Nso  # noqa: E402


def nso(size_text, bss=0x1000):
    """An uncompressed NSO: .text of size_text bytes (b over MOD0's offset word), .rodata with MOD0 at 0x1000 past
    .text, .data with an empty .dynamic."""
    text = bytearray(size_text)
    struct.pack_into('<II', text, 0, 0x14000002, size_text)   # b +8; the MOD0 offset
    ro = b'MOD0' + struct.pack('<6i', 0x1000, 0, 0, 0, 0, 0)  # .dynamic at .rodata + 0x1000 = .data
    data = bytes(16)                                         # DT_NULL
    segs = [(0, bytes(text)), (size_text, ro), (size_text + 0x1000, data)]
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off = 0x100
    for i, (mem, d) in enumerate(segs):
        struct.pack_into('<IIII', hdr, 0x10 + 0x10 * i, off, mem, len(d), bss if i == 2 else 0)
        struct.pack_into('<I', hdr, 0x60 + 4 * i, len(d))
        off += len(d)
    return bytes(hdr) + b''.join(d for _, d in segs)


def dev_layout_files(exefs, out):
    """layout.json and layout.h as recomp/layout.py wrote them before plugins (dev f8505807), the reference for the
    no-plugin case."""
    out.mkdir(parents=True, exist_ok=True)
    mods, base = [], layout.FIRST_BASE
    for name in layout.ORDER:
        if (exefs / name).exists():
            size = Nso(str(exefs / name)).size
            mods.append((name, base, size))
            base = layout.align_up(base + size)
    (out / 'layout.json').write_text(json.dumps(
        [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'} for n, b, s in mods], indent=1) + '\n')
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    (out / 'layout.h').write_text('\n'.join(lines))


class Layout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.exefs = cls.dir / 'exefs'
        cls.exefs.mkdir()
        for name, size in (('main', 0x30000), ('subsdk0', 0x5000), ('sdk', 0x21000)):
            (cls.exefs / name).write_bytes(nso(size))
        cls.plugins = [cls.dir / 'mod a' / 'subsdk9', cls.dir / 'mod b' / 'subsdk8']
        for p, size in zip(cls.plugins, (0x9000, 0x3000)):
            p.parent.mkdir()
            p.write_bytes(nso(size))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_no_plugins_identical(self):
        dev, ours = self.dir / 'dev', self.dir / 'ours'
        dev_layout_files(self.exefs, dev)
        layout.write(layout.layout(self.exefs), ours, layout.plugin_files([]))
        for f in ('layout.json', 'layout.h'):
            self.assertEqual((dev / f).read_bytes(), (ours / f).read_bytes(), f)

    def test_plugins_after_sdk(self):
        plain = layout.layout(self.exefs)
        mods = layout.layout(self.exefs, self.plugins)
        self.assertEqual(mods[:3], plain)   # the game modules keep their bases
        self.assertEqual([n for n, _, _ in mods], ['main', 'subsdk0', 'sdk', 'plugin0', 'plugin1'])
        for (_, b0, s0), (_, b1, _) in zip(mods, mods[1:]):
            self.assertEqual(b1, layout.align_up(b0 + s0))
        self.assertEqual(mods[3][2], Nso(str(self.plugins[0])).size)
        out = self.dir / 'with'
        files = layout.plugin_files(self.plugins)
        layout.write(mods, out, files)
        data = json.loads((out / 'layout.json').read_text())
        self.assertNotIn('file', data[2])
        self.assertEqual(data[3]['file'], self.plugins[0].resolve().as_posix())
        self.assertEqual(len(data[3]['sha256']), 64)
        self.assertEqual(layout.read(out), mods)
        self.assertIn('#define MOD_PLUGIN1_BASE', (out / 'layout.h').read_text())
        self.assertIn('#define MOD_COUNT 5', (out / 'layout.h').read_text())

    def test_address_limit(self):
        old = layout.FIRST_BASE
        layout.FIRST_BASE = layout.ADDRESS_LIMIT - 0x400000   # the plugins end above 2^39
        try:
            with self.assertRaises(ValueError):
                layout.layout(self.exefs, self.plugins)
        finally:
            layout.FIRST_BASE = old


# ---- records
MAIN, PLUGIN = 0x7000000000, 0x7005200000
SITE_OFF, STUB_OFF, TRAMP_OFF, CB_OFF, IMPL_OFF, THUNK_OFF = 0x1000, 0x4000, 0x2000, 0x66f0, 0x73ec, 0xf00
MAIN_TEXT, PLUGIN_TEXT = 0x100000, 0x9000


def b(src, dst, link=False):
    return (0x94000000 if link else 0x14000000) | ((dst - src) >> 2) & 0x3FFFFFF


def record(original=0x94000010, trampoline_first=None, stub=None, writes=None, **extra):
    """A record of one inline hook at main+SITE_OFF whose original is `original` (default bl main+SITE_OFF+0x40)."""
    site, stub_at, tramp = MAIN + SITE_OFF, PLUGIN + STUB_OFF, PLUGIN + TRAMP_OFF
    img = bytearray(0x14000)
    words = stub or [xp.STUR_X30, b(stub_at + 4, PLUGIN + IMPL_OFF, True), xp.LDUR_X30, b(stub_at + 12, tramp)]
    struct.pack_into('<4IQ', img, STUB_OFF, *words, PLUGIN + CB_OFF)
    if trampoline_first is None:
        rel = xp.pc_relative(original, site)
        trampoline_first = (b(tramp, rel[1], True) if rel and rel[0][0] == 'b' else original)
    struct.pack_into('<II', img, TRAMP_OFF, trampoline_first, b(tramp + 4, site + 4))
    rec = {
        'refused': None, 'svcs': {'0x6': 52, '0x1e': 10, '0x27': 18, '0x29': 17, '0x74': 11, '0x75': 7},
        'get_info': {'2': 1, '3': 1, '12': 1, '13': 1, '65001': 1},
        'imports': {'_ZN2nn4util8SNPrintfEPcmPKcz': 20, '_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE': 1},
        'modules': [
            {'name': 'main', 'base': MAIN, 'size': 0x200000, 'text_end': MAIN_TEXT, 'data_off': 0x180000, 'init': 0,
             'plugin': False},
            {'name': 'sdk', 'base': 0x7004400000, 'size': 0x100000, 'text_end': 0x80000, 'data_off': 0xc0000,
             'init': 0, 'plugin': False},
            {'name': 'plugin0', 'base': PLUGIN, 'size': 0x14000, 'text_end': PLUGIN_TEXT, 'data_off': 0xd000,
             'init': PLUGIN + 0x50a0, 'plugin': True, 'functions': [hex(CB_OFF), hex(IMPL_OFF), hex(THUNK_OFF)]}],
        'writes': writes if writes is not None else [
            {'module': 'main', 'offset': SITE_OFF, 'old': original, 'new': b(site, stub_at)}],
        'aliases': [], 'images': {'plugin0': bytes(img)}}
    rec.update(extra)
    return rec


IMPLS = {PLUGIN + IMPL_OFF: 'exl_inline_hook_impl'}


class Check(unittest.TestCase):
    def reasons(self, rec):
        return xp.check_record(rec, IMPLS)[1]

    def refused(self, rec, text):
        r = self.reasons(rec)
        self.assertTrue(r, 'accepted')
        self.assertTrue(any(text in x for x in r), r)

    def test_good(self):
        hooks, reasons = xp.check_record(record(), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks[0]['callback'], f'plugin0+{CB_OFF:#x}')
        self.assertEqual(hooks[0]['trampoline'], f'plugin0+{TRAMP_OFF:#x}')

    def test_good_plain_original(self):
        self.assertEqual(self.reasons(record(original=0xAA0003F3)), [])   # mov x19, x0, copied as it is

    def test_call_site_rewrite(self):   # Quick Weapon Durability: bl straight to a thunk of the plugin
        site = MAIN + SITE_OFF
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(site, PLUGIN + THUNK_OFF, True)}]
        hooks, reasons = xp.check_record(record(writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['kind'], 'call')
        self.assertEqual(hooks[0]['target'], f'plugin0+{THUNK_OFF:#x}')

    def test_replace_hook(self):   # Full Inventory Expansion: b from a main function start, Orig trampoline in the pool
        site, prologue = MAIN + SITE_OFF, 0xA9BA7BFD   # stp x29, x30, [sp, #-0x60]!
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': prologue, 'new': b(site, PLUGIN + THUNK_OFF),
              'fn_start': True}]
        hooks, reasons = xp.check_record(record(original=prologue, writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['kind'], 'replace')
        self.assertEqual(hooks[0]['trampoline'], f'plugin0+{TRAMP_OFF:#x}')

    def test_replace_inside_function(self):
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + THUNK_OFF),
              'fn_start': False}]
        self.refused(record(writes=w), 'from inside a main function')

    def test_call_into_function_middle(self):
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + 0xf08, True)}]
        self.refused(record(writes=w), 'neither an inline-hook stub nor a function start')

    def test_bl_to_stub(self):   # an inline stub is entered by b; a bl there would skip the saved x30
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + STUB_OFF, True)}]
        self.refused(record(writes=w), 'neither an inline-hook stub nor a function start')

    def test_refused_run(self):
        self.refused(record(refused='svc 0x40 at plugin0+0x76a0'), 'the run was refused')

    def test_svc(self):
        self.refused(record(svcs={'0x6': 1, '0x40': 1}), 'svc 0x40')

    def test_get_info(self):
        self.refused(record(get_info={'2': 1, '18': 1}), 'GetInfo id 18')

    def test_import(self):
        self.refused(record(imports={'_ZN2nn2fs8MountRomEPKcPvm': 1}), 'the import _ZN2nn2fs8MountRom')

    def test_write_main_data(self):
        w = [{'module': 'main', 'offset': 0x180008, 'old': 0, 'new': 1}]
        self.refused(record(writes=w), "outside main's .text: main+0x180008")

    def test_write_other_module(self):
        w = [{'module': 'sdk', 'offset': 0x100, 'old': 0, 'new': 1}]
        self.refused(record(writes=w), "outside main's .text: sdk+0x100")

    def test_not_a_branch(self):
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': 0xD503201F}]
        self.refused(record(writes=w), 'is no b')

    def test_branch_elsewhere(self):
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, MAIN + 0x8000)}]
        self.refused(record(writes=w), "does not reach into a plugin's .text")

    def test_stub_shape(self):
        stub_at = PLUGIN + STUB_OFF
        words = [0xD503201F, b(stub_at + 4, PLUGIN + IMPL_OFF, True), xp.LDUR_X30, b(stub_at + 12, PLUGIN + TRAMP_OFF)]
        self.refused(record(stub=words), 'neither an inline-hook stub nor a function start')

    def test_stub_impl(self):
        stub_at = PLUGIN + STUB_OFF
        words = [xp.STUR_X30, b(stub_at + 4, PLUGIN + 0x100, True), xp.LDUR_X30, b(stub_at + 12, PLUGIN + TRAMP_OFF)]
        self.refused(record(stub=words), 'not exl_inline_hook_impl')

    def test_callback_outside(self):
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<Q', img, STUB_OFF + 16, MAIN + 0x500)
        rec['images']['plugin0'] = bytes(img)
        self.refused(rec, "callback 0x7000000500 is outside plugin0's .text")

    def test_trampoline_not_original(self):
        self.refused(record(original=0xAA0003F3, trampoline_first=0xAA0003F4), 'not the original')

    def test_trampoline_bl_elsewhere(self):
        tramp = PLUGIN + TRAMP_OFF
        self.refused(record(trampoline_first=b(tramp, MAIN + SITE_OFF + 0x44, True)), 'not the original')

    def test_trampoline_return(self):
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<I', img, TRAMP_OFF + 4, b(PLUGIN + TRAMP_OFF + 4, MAIN + SITE_OFF + 8))
        rec['images']['plugin0'] = bytes(img)
        self.refused(rec, 'no b to the site + 4')

    def test_indirect_original(self):
        self.refused(record(original=0xD65F03C0), 'is a br, blr or ret')


class Diff(unittest.TestCase):
    def test_lists_differing_words(self):
        a, c = record(), record()
        img = bytearray(c['images']['plugin0'])
        struct.pack_into('<Q', img, 0x13450, 0x2345000000)
        c['images']['plugin0'] = bytes(img)
        a['pool'] = c['pool'] = {'base': '0x2000000000', 'size': '0x1000000000'}
        lines = xp.diff_records(a, c)
        self.assertEqual(len(lines), 1)
        self.assertIn('plugin0+0x13450', lines[0])
        self.assertIn('alias pool', lines[0])
        self.assertEqual(xp.diff_records(a, record()), [])


# ---- the code the hooks reach
SVC_40, RET_W, NOP = 0xD4000801, 0xD65F03C0, 0xD503201F


def code(words, at=0):
    img = bytearray(0x200)
    for i, w in enumerate(words):
        struct.pack_into('<I', img, at + 4 * i, w)
    return bytes(img)


class Reach(unittest.TestCase):
    BASE = 0x7005200000

    def test_follows_calls_and_finds_svc(self):
        # root: bl +0x40; ret.  callee at 0x40: svc 0x40; ret
        img = bytearray(code([b(self.BASE, self.BASE + 0x40, True), RET_W]))
        struct.pack_into('<II', img, 0x40, SVC_40, RET_W)
        svcs, imports, indirect, outside = xp.reach(bytes(img), self.BASE, 0x200, [self.BASE], {})
        self.assertEqual(svcs, {0x40})

    def test_imports_through_plt(self):
        img = code([b(self.BASE, self.BASE + 0x80, True), RET_W])
        svcs, imports, _, _ = xp.reach(img, self.BASE, 0x200, [self.BASE], {self.BASE + 0x80: 'malloc'})
        self.assertEqual((svcs, imports), (set(), {'malloc': 1}))

    def test_stub_returns_at_its_trampoline_branch(self):
        # stur; bl impl; ldur; b trampoline (where an svc sits): the b is the stub's return
        stub = [xp.STUR_X30, b(self.BASE + 4, self.BASE + 0x40, True), xp.LDUR_X30, b(self.BASE + 12, self.BASE + 0x100)]
        img = bytearray(code(stub))
        struct.pack_into('<I', img, 0x40, RET_W)
        struct.pack_into('<I', img, 0x100, SVC_40)
        self.assertEqual(xp.reach(bytes(img), self.BASE, 0x200, [self.BASE], {}, stubs={self.BASE})[0], set())
        self.assertEqual(xp.reach(bytes(img), self.BASE, 0x200, [self.BASE], {})[0], {0x40})

    def test_indirect_calls_counted(self):
        img = code([0xD63F0100, RET_W])   # blr x8
        self.assertEqual(xp.reach(img, self.BASE, 0x200, [self.BASE], {})[2], 1)

    def test_check_refuses_svc_in_callback(self):
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<II', img, CB_OFF, SVC_40, RET_W)
        struct.pack_into('<I', img, IMPL_OFF, RET_W)
        rec['images']['plugin0'] = bytes(img)
        hooks, _ = xp.check_record(rec, IMPLS)
        reasons, _ = xp.reach_check(rec, hooks, IMPLS)
        self.assertTrue(any('svc 0x40' in r for r in reasons), reasons)

    def test_check_allows_output_debug_string(self):
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<II', img, CB_OFF, 0xD40004E1, RET_W)   # svc 0x27
        struct.pack_into('<I', img, IMPL_OFF, RET_W)
        rec['images']['plugin0'] = bytes(img)
        hooks, _ = xp.check_record(rec, IMPLS)
        self.assertEqual(xp.reach_check(rec, hooks, IMPLS)[0], [])


# ---- patch lines of the three hook kinds
class Lines(unittest.TestCase):
    def lines(self, rec):
        hooks, reasons = xp.check_record(rec, IMPLS)
        self.assertEqual(reasons, [])
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'plugin0.img').write_bytes(rec['images']['plugin0'])
            rec['modules'][2]['image'] = 'plugin0.img'
            return xp.plugin_lines(rec, xp.hooks_of(hooks, 'plugin0'), 'mod_x', d, 'plugin0')

    def test_inline(self):
        out = self.lines(record(original=0xAA0003F3))
        self.assertIn(f'site mod_x main {SITE_OFF:x} aa0003f3 @call:plugin0+{STUB_OFF:x} aa0003f3', out)
        self.assertIn(f'stub plugin0 {STUB_OFF:x}', out)
        self.assertIn(f'seed plugin0 {CB_OFF:x} {IMPL_OFF:x}', out)
        self.assertIn(f'expect mod_x main {SITE_OFF:x} aa0003f3 {b(MAIN + SITE_OFF, PLUGIN + STUB_OFF):08x}', out)
        self.assertTrue(out[0].startswith('plugin mod_x plugin0 '))

    def test_replace(self):
        site, prologue = MAIN + SITE_OFF, 0xA9BA7BFD
        new = b(site, PLUGIN + THUNK_OFF)
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': prologue, 'new': new, 'fn_start': True}]
        out = self.lines(record(original=prologue, writes=w))
        self.assertIn(f'site mod_x main {SITE_OFF:x} a9ba7bfd {new:08x}', out)
        self.assertIn(f'orig mod_x plugin0 {TRAMP_OFF:x} main {SITE_OFF:x}', out)
        self.assertIn(f'seed plugin0 {THUNK_OFF:x}', out)

    def test_call(self):
        new = b(MAIN + SITE_OFF, PLUGIN + THUNK_OFF, True)
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': new}]
        out = self.lines(record(writes=w))
        self.assertIn(f'site mod_x main {SITE_OFF:x} 94000010 {new:08x}', out)
        self.assertFalse(any(x.startswith(('orig', 'stub')) for x in out))


# ---- one service run per plugin
class PerPlugin(unittest.TestCase):
    """main() with three plugins in three mods and a stand-in for the service run: plugin0 hooks its site, plugin1's
    init ends the run without a record (a crash), plugin2's run is refused (an svc). Each gets its own status line;
    only plugin0's set goes into the patch file."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.d = Path(tmp.name)
        self.files = []
        for name in ('A', 'B', 'C'):
            f = self.d / 'mods' / name / 'exefs' / 'subsdk9'
            f.parent.mkdir(parents=True)
            f.write_bytes(nso(PLUGIN_TEXT) + name.encode())
            self.files.append(f)
        (self.d / 'patches.txt').write_text('set fps60 TOTK_FPS60 1\n')
        self.runs = []
        saved = xp.run, xp.hook_impls
        self.real_run = xp.run
        self.addCleanup(lambda: (setattr(xp, 'run', saved[0]), setattr(xp, 'hook_impls', saved[1])))
        xp.run, xp.hook_impls = self.fake_run, lambda rec: IMPLS

    def fake_run(self, exe, out, tick_offset, timeout, only):
        self.runs.append(only)
        out.mkdir(parents=True, exist_ok=True)
        if only == 'plugin1':
            return 0xC0000005, 'its init ended the service run without a record (exit 0xc0000005)'
        rec = record(writes=[] if only != 'plugin0' else None)
        rec['modules'][2]['file'] = str(self.files[0])
        for i in (1, 2):
            rec['modules'].append(dict(rec['modules'][2], name=f'plugin{i}', base=PLUGIN + i * 0x100000,
                                       file=str(self.files[i])))
        for m in rec['modules'][2:]:
            m['ran'] = m['name'] == only
        if only == 'plugin2':
            rec['refused'] = 'svc 0x1 at plugin2+0x50a0'
        (out / 'plugin0.img').write_bytes(rec.pop('images')['plugin0'])
        rec['modules'][2]['image'] = 'plugin0.img'
        (out / 'record.json').write_text(json.dumps(rec))
        return 3 if rec['refused'] else 0, None

    def test_each_plugin_alone(self):
        out, status, patched = self.d / 'rec', self.d / 'plugins.status', self.d / 'patches-out.txt'
        argv = sys.argv
        sys.argv = ['exefs_plugins.py', '--exe', 'totk.exe', '--out', str(out), '--mods', str(self.d / 'mods'),
                    '--patches', str(self.d / 'patches.txt'), '--patches-out', str(patched), '--status', str(status)]
        try:
            code = xp.main()
        finally:
            sys.argv = argv
        self.assertEqual(code, 1)
        self.assertEqual(self.runs, ['-', 'plugin0', 'plugin0', 'plugin1', 'plugin2'])
        lines = {line.split('\t')[0]: line.split('\t') for line in status.read_text().splitlines()}
        key = {n: xp.fnv(f'{n}/exefs/subsdk9'.encode()) for n in 'ABC'}
        self.assertEqual(lines[key['A']][2:], ['plugin', '-', 'plugin0, 1 hooks'])
        self.assertEqual(lines[key['B']][2], 'unsupported')
        self.assertIn('ended the service run', lines[key['B']][4])
        self.assertEqual(lines[key['C']][2], 'unsupported')
        self.assertIn('svc 0x1', lines[key['C']][4])
        text = patched.read_text()
        self.assertIn(f'set mod_{key["A"].lower()} ', text)
        self.assertNotIn(f'mod_{key["B"].lower()}', text)
        self.assertNotIn(f'mod_{key["C"].lower()}', text)
        self.assertIn((out / 'plugin0' / 'run1' / 'plugin0.img').resolve().as_posix(), text)

    def test_run_reasons(self):
        # the real run(): a hang becomes its own reason, a crash names the FATAL line
        saved = xp.subprocess.run
        self.addCleanup(setattr, xp.subprocess, 'run', saved)

        def hang(cmd, **kw):
            raise xp.subprocess.TimeoutExpired(cmd, kw['timeout'], output=b'started', stderr=None)
        xp.subprocess.run = hang
        self.assertEqual(self.real_run(None, self.d / 'h', 0, 7, 'plugin0'), (None, 'its init did not return within 7 s'))
        xp.subprocess.run = lambda cmd, **kw: SimpleNamespace(returncode=0xC0000005, stdout='x\nFATAL: access violation '
                                                              '(0xc0000005) at host 1\n', stderr='')
        code, why = self.real_run(None, self.d / 'c', 0, 7, 'plugin0')
        self.assertEqual(why, 'its init crashed the service run: access violation (0xc0000005) at host 1')

    def test_ran_view_leaves_out_plugins_that_did_not_run(self):
        rec = record()
        rec['modules'].append(dict(rec['modules'][2], name='plugin1', base=PLUGIN + 0x100000, ran=False))
        rec['modules'][2]['ran'] = True
        rec['images']['plugin1'] = rec['images']['plugin0']
        view = xp.ran_view(rec)
        self.assertEqual([m['name'] for m in view['modules'] if m['plugin']], ['plugin0'])
        self.assertEqual(set(view['images']), {'plugin0'})
        whole = record()   # no "ran": every init ran, the record is its own view
        self.assertIs(xp.ran_view(whole), whole)


# ---- the generator on those lines
def code_nso(words_at, size_text=0x5000):
    """An NSO as nso() whose .text holds words_at {offset: word}."""
    raw = bytearray(nso(size_text))
    for off, w in words_at.items():
        struct.pack_into('<I', raw, 0x100 + off, w)
    return bytes(raw)


FN = 0x100   # the hooked main function: mov x19, x0; mov x0, x19; ret


class Generator(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.d = Path(tmp.name)
        exefs = self.d / 'exefs'
        exefs.mkdir()
        (exefs / 'main').write_bytes(code_nso({FN: 0xAA0003F3, FN + 4: 0xAA1303E0, FN + 8: RET_W}))
        (exefs / 'sdk').write_bytes(nso(0x1000))
        stub = PLUGIN_TEXT_STUB = 0x4000
        self.plugin_file = self.d / 'subsdk9'
        self.plugin_file.write_bytes(nso(0x5000))
        self.mods = layout.layout(exefs, [self.plugin_file])
        self.bases = {n: b_ for n, b_, _ in self.mods}
        pb = self.bases['plugin0']
        # the image after the init: a stub at 0x4000 (b to a trampoline at 0x2000), an impl at 0x4100, a callback at
        # 0x4200
        img = bytearray(Nso(str(self.plugin_file)).image)
        struct.pack_into('<4IQ', img, stub, xp.STUR_X30, b(pb + stub + 4, pb + 0x4100, True), xp.LDUR_X30,
                         b(pb + stub + 12, pb + 0x2000), pb + 0x4200)
        struct.pack_into('<I', img, 0x4100, RET_W)
        struct.pack_into('<I', img, 0x4200, RET_W)
        struct.pack_into('<II', img, 0x2000, 0xAA0003F3, b(pb + 0x2004, self.bases['main'] + FN + 4))
        (self.d / 'plugin0.img').write_bytes(bytes(img))
        n = Nso(str(self.plugin_file))
        self.sha = hashlib.sha256(bytes(img[n.text[0]:n.text[0] + n.text[1]])).hexdigest()

    def table(self, site_line, extra=()):
        text = '\n'.join(['set mod_x TOTK_MODPATCH_X 0 ordered',
                          f'plugin mod_x plugin0 {self.sha} {(self.d / "plugin0.img").as_posix()}',
                          f'expect mod_x main {FN:x} aa0003f3 14000000', site_line, *extra]) + '\n'
        (self.d / 'p.txt').write_text(text)
        return patches.load(self.d / 'p.txt', self.mods)

    def emit(self, table, name, entry, end, origs=()):
        mod = gen.open_module(name, self.d / ('exefs/main' if name == 'main' else 'subsdk9'), self.bases[name], table,
                              plugin=name != 'main')
        e = gen.Emitter(mod, {entry}, table.for_module(name, self.bases[name]), origs=set(origs))
        return gen.chunk_text(name, 0, e, [(entry, end)])

    def test_inline_hook_calls_the_stub_then_the_original(self):
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3', ['stub plugin0 4000',
                                                                                         'seed plugin0 4100 4200'])
        m = self.bases['main'] + FN
        c = self.emit(t, 'main', m, m + 12)
        stub = self.bases['plugin0'] + 0x4000
        on = c[c.index('if (rt_patch_on[0])'):]
        self.assertLess(on.index(f'f_{stub:x}(c);'), on.index('} else {'))   # the hook, then the original, when on
        head = on[:on.index(f'f_{stub:x}(c);')]
        self.assertNotIn('c->x[30] =', head)                                  # no x30 written for the call
        self.assertIn(f'void f_{stub:x}(Cpu *c);', c)
        h = patches.header_text(t, 'MARK')
        self.assertIn('#define RT_PLUGIN_COUNT 1', h)
        self.assertIn(f'X(0, "main", {FN:#x}, 0xaa0003f3u, 0x14000000u)', h)
        self.assertIn('X("plugin0", 0x4100) X("plugin0", 0x4200)', h)

    def test_stub_returns_instead_of_branching_to_its_trampoline(self):
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3', ['stub plugin0 4000'])
        stub = self.bases['plugin0'] + 0x4000
        c = self.emit(t, 'plugin0', stub, stub + 16)
        body = c[c.index(f'void f_{stub:x}(Cpu *c) {{'):]
        self.assertIn('return;', body)
        self.assertNotIn(f'{self.bases["plugin0"] + 0x2000:#x}', body)   # the trampoline is not reached

    def test_replace_hook_keeps_an_unpatched_copy(self):
        pb = self.bases['plugin0']
        m = self.bases['main'] + FN
        new = b(m, pb + 0x4200)
        t = self.table(f'site mod_x main {FN:x} aa0003f3 {new:08x}', [f'orig mod_x plugin0 2000 main {FN:x}',
                                                                        'seed plugin0 4200'])
        self.assertEqual(t.origs_for('main', self.bases['main']), {m})
        self.assertEqual(t.orig_entries('plugin0', self.bases), {pb + 0x2000: m})
        c = self.emit(t, 'main', m, m + 12, origs=[m])
        copy = c[c.index(f'void fo_{m:x}(Cpu *c) {{'):]
        self.assertNotIn('rt_patch_on', copy)
        self.assertIn('rt_patch_on[0]', c[:c.index(f'void fo_{m:x}')])
        tab = gen.table_text('plugin0', [pb + 0x4200, pb + 0x2000], {pb + 0x2000: gen.orig_name(m)})
        self.assertIn(f'void fo_{m:x}(Cpu *c);', tab)
        self.assertIn(f'fo_{m:x}', tab[tab.index('_fn_ptrs'):])

    def test_image_must_match_its_hash(self):
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3', ['stub plugin0 4000'])
        t.plugins['plugin0']['sha256'] = '0' * 64
        with self.assertRaises(patches.PatchError):
            gen.open_module('plugin0', self.plugin_file, self.bases['plugin0'], t)

    def test_chunk_key_takes_the_unpatched_copy(self):
        # the lift cache (recomp/gen.py chunk_key): a chunk whose function gets an unpatched copy is keyed apart, and a
        # chunk without one keeps the key it had before plugins
        fns = [(0x100, 0x10c)]
        plain = gen.chunk_key('m', 0, fns, {}, [], {}, [], None, None)
        self.assertEqual(gen.chunk_key('m', 0, fns, {}, [], {}, [], None, None, {0x200}), plain)
        self.assertNotEqual(gen.chunk_key('m', 0, fns, {}, [], {}, [], None, None, {0x100}), plain)
        site = {0x100: [(0, [('call', 0x7005204000), 0xAA0003F3])]}
        self.assertNotEqual(gen.chunk_key('m', 0, fns, site, [0x100], {}, [], None, None),
                            gen.chunk_key('m', 0, fns, {0x100: [(0, [('call', 0x7005204010), 0xAA0003F3])]}, [0x100],
                                          {}, [], None, None))

    def test_no_plugin_lines_no_plugin_header(self):
        (self.d / 'q.txt').write_text('set fps60 TOTK_FPS60 0\n')
        self.assertNotIn('PLUGIN', patches.header_text(patches.load(self.d / 'q.txt', self.mods), 'MARK'))


if __name__ == '__main__':
    unittest.main()
