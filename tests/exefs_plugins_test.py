"""recomp/layout.py with and without mod plugins, and tools/exefs_plugins.py's check of a plugin service run record:
a synthetic exlaunch-shaped record with one inline hook that passes, and one bad record per rule. No game files.

Run with .venv (lz4 and numpy, as recomp/): python tests/exefs_plugins_test.py
"""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'recomp'))
import exefs_plugins as xp  # noqa: E402
import layout  # noqa: E402
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


if __name__ == '__main__':
    unittest.main()
