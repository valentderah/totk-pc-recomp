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
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock
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


def quad(v):
    """The two words of a .quad v."""
    return [v & 0xFFFFFFFF, v >> 32]


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


def far_jump(site, target, reg=17):
    """adrp x<reg>, target; add x<reg>, x<reg>, #lo; br x<reg> at main+site (SageWheel's hook)."""
    a = MAIN + site
    imm = ((target & ~0xFFF) - (a & ~0xFFF)) >> 12
    return [0x90000000 | reg | (imm & 3) << 29 | (imm >> 2 & 0x7FFFF) << 5,
            0x91000000 | reg | reg << 5 | (target & 0xFFF) << 10, 0xD61F0000 | reg << 5]


def far_record(target=PLUGIN + THUNK_OFF, back=None):
    """Three words over a function's prologue that jump into the plugin; back: a main address the init stored in
    the plugin's .bss, its way back."""
    words = far_jump(SITE_OFF, target)
    old = [0xD10243FF, 0xA9037BFD, 0xA9046FFC]
    rec = record(writes=[{'module': 'main', 'offset': SITE_OFF + 4 * i, 'old': o, 'new': n, 'fn_start': i == 0}
                         for i, (o, n) in enumerate(zip(old, words))])
    if back is not None:
        img = bytearray(rec['images']['plugin0'])
        rec['pre_images'] = {'plugin0': bytes(img)}
        struct.pack_into('<Q', img, 0x12000, back)
        rec['images']['plugin0'] = bytes(img)
    return rec


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

    def test_good_with_fp_impl(self):   # Magic of the Kingdom: the impl that saves d0..d15 as well
        self.assertIn('exl_inline_hook_with_fp_impl', xp.HOOK_IMPLS)
        hooks, reasons = xp.check_record(record(), {PLUGIN + IMPL_OFF: 'exl_inline_hook_with_fp_impl'})
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['impl'], 'exl_inline_hook_with_fp_impl')

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

    def test_branch_inside_function(self):   # a far hook: the plugin's code comes back through an entry
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + THUNK_OFF),
              'fn_start': False}]
        hooks, reasons = xp.check_record(record(writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['kind'], 'branch')

    def test_call_into_function_middle(self):   # the target becomes a seed of the plugin's lift
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + 0xf08, True)}]
        hooks, reasons = xp.check_record(record(writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual((hooks[0]['kind'], hooks[0]['target']), ('call', 'plugin0+0xf08'))

    def test_bl_to_stub(self):   # an inline stub is entered by b; a bl there would skip the saved x30
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, PLUGIN + STUB_OFF, True)}]
        self.refused(record(writes=w), 'into an inline-hook stub')

    def test_refused_run(self):
        self.refused(record(refused='svc 0x40 at plugin0+0x76a0'), 'the run was refused')

    def test_svc(self):
        self.refused(record(svcs={'0x6': 1, '0x40': 1}), 'svc 0x40')

    def test_get_info(self):
        self.refused(record(get_info={'2': 1, '18': 1}), 'GetInfo id 18')

    def test_import(self):
        self.refused(record(imports={'_ZN2nn2fs8MountRomEPKcPvm': 1}), 'the import _ZN2nn2fs8MountRom')

    def test_import_the_game_imports_is_allowed(self):
        rec = record(imports={'_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj': 1})
        self.assertTrue(xp.check_record(rec, IMPLS)[1])
        self.assertEqual(xp.check_record(rec, IMPLS, frozenset(rec['imports']))[1], [])

    def test_game_imports_leave_out_the_loader_and_the_boot_hooks(self):
        names = ['_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi', '__nnmusl_init_dso', 'glslcCompile',
                 '_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE']
        self.assertEqual([n for n in names if not xp.GAME_IMPORTS_LEFT_OUT.search(n)], [])
        kept = ['abort', '_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi', '_ZN2nn2fs6CommitEPKc']
        self.assertEqual([n for n in kept if xp.GAME_IMPORTS_LEFT_OUT.search(n)], [])

    def test_game_imports_of_a_missing_exefs_are_none(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(xp.game_imports(Path(d)), frozenset())
        self.assertEqual(xp.game_imports(None), frozenset())

    def test_write_main_data(self):   # kept as written, compared at every start
        w = [{'module': 'main', 'offset': 0x180008, 'old': 0, 'new': 1}]
        hooks, reasons = xp.check_record(record(writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['kind'], 'data')
        self.assertNotIn('target', hooks[0])

    def test_pointer_swap(self):   # a table entry of main's .data now points at a plugin function
        w = [{'module': 'main', 'offset': 0x180010, 'old': 0x00001000, 'new': (PLUGIN + THUNK_OFF) & 0xFFFFFFFF}]
        hooks, reasons = xp.check_record(record(writes=w), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['target'], f'plugin0+{THUNK_OFF:#x}')

    def test_write_main_rodata(self):   # the lift read .rodata as constants
        w = [{'module': 'main', 'offset': 0x120000, 'old': 0, 'new': 1}]
        self.refused(record(writes=w), "a write into main's .rodata: main+0x120000")

    def test_write_other_module(self):
        w = [{'module': 'sdk', 'offset': 0x100, 'old': 0, 'new': 1}]
        self.refused(record(writes=w), 'a write outside main: sdk+0x100')

    def code(self, rec):
        """The hooks of an accepted record, which must all be new code (recomp/patches.py `code`)."""
        hooks, reasons = xp.check_record(rec, IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual({h['kind'] for h in hooks}, {'code'})
        return {int(h['site'].split('+')[1], 16): h for h in hooks}

    def test_not_a_branch(self):   # a nop over a call: new code, lifted in a copy of its function
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': 0xD503201F}]
        h = self.code(record(writes=w))[SITE_OFF]
        self.assertEqual((h['new'], h.get('to'), h.get('back')), ('0xd503201f', None, None))

    def test_branch_elsewhere(self):   # a b to elsewhere in main: its target becomes a function start
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': b(MAIN + SITE_OFF, MAIN + 0x8000)}]
        self.assertEqual(self.code(record(writes=w))[SITE_OFF]['back'], [0x8000])

    def test_far_jump(self):   # SageWheel: adrp x17; add x17; br x17 into the plugin, back through its .bss
        hooks = self.code(far_record(back=MAIN + SITE_OFF + 12))
        self.assertEqual(hooks[SITE_OFF + 8]['to'], [f'plugin0+{THUNK_OFF:#x}'])
        self.assertEqual(hooks[SITE_OFF]['back'], [SITE_OFF + 12])
        rec = far_record(back=MAIN + SITE_OFF + 12)
        hooks, _ = xp.check_record(rec, IMPLS)
        back, ahead = xp.entries(rec, hooks, 'plugin0')
        self.assertEqual(back, [SITE_OFF + 12])
        self.assertEqual(xp.reach_check(rec, hooks, IMPLS)[0], [])
        far = far_record(back=MAIN + SITE_OFF + 12 + xp.BACK_WINDOW + 4)   # too far after the run: not its way back
        self.assertNotIn('back', self.code(far)[SITE_OFF])

    def test_far_jump_out_of_the_code(self):
        self.refused(far_record(target=0x7006000000), "outside main's and the plugin's code")
        self.refused(far_record(target=PLUGIN + 0x12000), "outside main's and the plugin's code")   # its .data

    def test_far_jump_into_a_stub(self):   # a stub's lift returns at its b to the trampoline
        rec = far_record(target=PLUGIN + STUB_OFF)
        rec['writes'].append({'module': 'main', 'offset': 0x2000, 'old': 0xAA0003F3, 'new': b(MAIN + 0x2000,
                                                                                           PLUGIN + STUB_OFF)})
        self.refused(rec, 'into the inline-hook stub plugin0+0x4000')

    def test_slot_load_seeds_its_target(self):   # adrp x16, page; ldr x16, [x16, #8]; br x16 through the plugin's data
        site, slot = MAIN + SITE_OFF, PLUGIN + 0xd008
        imm = ((slot & ~0xFFF) - (site & ~0xFFF)) >> 12
        words = [0x90000010 | (imm & 3) << 29 | (imm >> 2 & 0x7FFFF) << 5, 0xF9400210 | 1 << 10, 0xD61F0200]
        rec = record(writes=[{'module': 'main', 'offset': SITE_OFF + 4 * i, 'old': 0xAA0003F3, 'new': n}
                             for i, n in enumerate(words)])
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<Q', img, 0xd008, PLUGIN + CB_OFF)
        rec['images']['plugin0'] = bytes(img)
        self.assertEqual(self.code(rec)[SITE_OFF + 4]['to'], [f'plugin0+{CB_OFF:#x}'])

    def test_code_svc(self):
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0xAA0003F3, 'new': 0xD4000801}]   # svc #0x40
        self.refused(record(writes=w), 'the new code has svc 0x40')

    def test_code_literal_from_written_words(self):   # main keeps the game's bytes at play
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0xAA0003F3, 'new': 0x58000051},   # ldr x17, #8
             {'module': 'main', 'offset': SITE_OFF + 4, 'old': 0xAA0003F3, 'new': 0xD61F0220},
             {'module': 'main', 'offset': SITE_OFF + 8, 'old': 0xAA0003F3, 'new': 0x1234}]
        self.refused(record(writes=w), 'loads a literal from words the init wrote')
        w[2]['offset'] = SITE_OFF + 16   # the literal at +8..+15 is the game's own again: no reason
        self.assertEqual(self.reasons(record(writes=w)), [])

    def test_region_targets(self):
        # movz x17, #0x1234; movk x17, #0x5678, lsl #16; blr x17; then a label resets what is known
        words = {0x1000: 0xD2824691, 0x1004: 0xF2AACF11, 0x1008: 0xD63F0220}
        self.assertEqual(patches.region_targets(words), {0x1008: ('blr', 0x56781234)})
        self.assertEqual(patches.region_targets(words, {0x1004}), {})
        words[0x1004] = 0xAA0103F1   # mov x17, x1: unknown
        self.assertEqual(patches.region_targets(words), {})
        far = dict(zip(range(MAIN + 0x100, MAIN + 0x10c, 4), far_jump(0x100, PLUGIN + 0x3f04)))
        self.assertEqual(patches.region_targets(far), {MAIN + 0x108: ('br', PLUGIN + 0x3f04)})
        self.assertEqual(patches.region_targets({0x2000: b(0x2000, 0x2400, True), 0x2004: 0x54000040}),
                         {0x2000: ('bl', 0x2400), 0x2004: ('bcond', 0x200c)})

    def test_stub_shape(self):   # not exlaunch's stub: plugin code the site branches to, a far hook
        stub_at = PLUGIN + STUB_OFF
        words = [0xD503201F, b(stub_at + 4, PLUGIN + IMPL_OFF, True), xp.LDUR_X30, b(stub_at + 12, PLUGIN + TRAMP_OFF)]
        hooks, reasons = xp.check_record(record(stub=words), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['kind'], 'branch')

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

    def test_indirect_original(self):   # a b over a ret or br: the copy of the function ends there instead
        for original in (0xD65F03C0, 0xD61F0100):
            w = [{'module': 'main', 'offset': SITE_OFF, 'old': original, 'new': b(MAIN + SITE_OFF, PLUGIN + THUNK_OFF)}]
            self.assertEqual(self.code(record(writes=w))[SITE_OFF]['to'], [f'plugin0+{THUNK_OFF:#x}'])

    def test_blr_original(self):   # Zonai Fieldworks hooks blr x8 sites: the call returns, the site goes on
        self.assertEqual(self.reasons(record(original=0xD63F0100)), [])

    def test_veneer_entries(self):   # Zonai Fieldworks: site -> veneer -> slot -> trampoline -> b site + 4
        site, ven, slot, tramp = MAIN + SITE_OFF, PLUGIN + 0x5000, PLUGIN + 0xd000, PLUGIN + 0x5100
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0xD5033BBF, 'new': b(site, ven), 'fn_start': False}]
        rec = record(writes=w)
        img = bytearray(rec['images']['plugin0'])
        imm = ((slot & ~0xFFF) - (ven & ~0xFFF)) >> 12
        adrp = 0x90000011 | (imm & 3) << 29 | (imm >> 2 & 0x7FFFF) << 5
        struct.pack_into('<3I', img, 0x5000, adrp, 0xF9400231 | ((slot & 0xFFF) // 8) << 10, 0xD61F0220)
        struct.pack_into('<Q', img, 0xd000, tramp)
        struct.pack_into('<2I', img, 0x5100, 0xD5033BBF, b(tramp + 4, site + 4))
        rec['images']['plugin0'] = bytes(img)
        hooks, reasons = xp.check_record(rec, IMPLS)
        self.assertEqual(reasons, [])
        back, ahead = xp.entries(rec, hooks, 'plugin0')
        self.assertEqual(back, [SITE_OFF + 4])
        self.assertEqual(ahead, [0x5100])

    # exlaunch's long forms (tools/exefs_plugins.py relocation_end, returns_to): the trampoline at plugin0+off
    def long_form(self, original, off, words):
        rec = record(original=original)
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<I', img, STUB_OFF + 12, b(PLUGIN + STUB_OFF + 12, PLUGIN + off))
        struct.pack_into(f'<{len(words)}I', img, off, *words)
        rec['images']['plugin0'] = bytes(img)
        return rec

    def test_trampoline_tbnz_long(self):   # Challenge: tbnz w0, #0 out of the plugin's reach
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        words = [0x37000040, 0x14000005, xp.LDR_X17_8, xp.BR_X17, *quad(site + 0x14), b(t + 24, site + 4)]
        hooks, reasons = xp.check_record(self.long_form(0x370000A0, TRAMP_OFF, words), IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['relocated'], '0x37000040 +5 words')

    def test_trampoline_backward_unsigned(self):   # Challenge: b.eq back 0x2ec; exlaunch's .quad lacks the sign
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        form = [0x54000040, 0x14000005, xp.LDR_X17_8, xp.BR_X17]
        for target in (site + 0x1FFD14, site - 0x2EC):
            words = [*form, *quad(target), b(t + 24, site + 4)]
            self.assertEqual(self.reasons(self.long_form(0x54FFE8A0, TRAMP_OFF, words)), [], hex(target))
        self.refused(self.long_form(0x54FFE8A0, TRAMP_OFF, [*form, *quad(site + 0x1FFD18), b(t + 24, site + 4)]),
                     'not the original')

    def test_trampoline_long_nop(self):   # the .quad 8-byte aligned by one nop before the form
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF + 4
        form = [0x37000040, 0x14000005, xp.LDR_X17_8, xp.BR_X17, *quad(site + 0x14)]
        self.assertEqual(self.reasons(self.long_form(0x370000A0, TRAMP_OFF + 4,
                                                     [xp.NOP, *form, b(t + 28, site + 4)])), [])
        self.refused(self.long_form(0x370000A0, TRAMP_OFF + 4, [*form, b(t + 24, site + 4)]), 'not the original')

    def test_trampoline_long_wrong_target(self):
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        words = [0x37000040, 0x14000005, xp.LDR_X17_8, xp.BR_X17, *quad(site + 0x18), b(t + 24, site + 4)]
        self.refused(self.long_form(0x370000A0, TRAMP_OFF, words), 'not the original')

    def test_trampoline_bl_long(self):   # ldr x17, #12; adr x30, #16; br x17; .quad, behind a nop here
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        words = [xp.NOP, xp.LDR_X17_12, xp.ADR_X30_16, xp.BR_X17, *quad(site + 0x40), b(t + 24, site + 4)]
        self.assertEqual(self.reasons(self.long_form(0x94000010, TRAMP_OFF, words)), [])

    def test_trampoline_adrp_long(self):   # adrp x8: ldr x8, #8; b #12; .quad <page>
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        words = [0x58000048, 0x14000003, *quad(site & ~0xFFF), b(t + 16, site + 4)]
        self.assertEqual(self.reasons(self.long_form(0x90000008, TRAMP_OFF, words)), [])

    def test_trampoline_ldr_literal_long(self):   # ldr x1, <literal>: ldr x1, #8; b over the 8 bytes; the bytes
        site, t = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF
        words = [0x58000041, 0x14000003, 0x11223344, 0x55667788, b(t + 16, site + 4)]
        self.assertEqual(self.reasons(self.long_form(0x58000801, TRAMP_OFF, words)), [])

    def test_trampoline_absolute_return(self):   # back to the site + 4 by ldr x17, #8; br x17; .quad, nop-aligned
        site = MAIN + SITE_OFF
        words = [0xAA0003F3, xp.NOP, xp.LDR_X17_8, xp.BR_X17, *quad(site + 4)]
        self.assertEqual(self.reasons(self.long_form(0xAA0003F3, TRAMP_OFF, words)), [])
        words[-2:] = quad(site + 8)
        self.refused(self.long_form(0xAA0003F3, TRAMP_OFF, words), 'no b to the site + 4')

    def test_replace_hook_adrp_orig(self):   # a function starting with adrp: its Orig trampoline is the long form
        site, t, adrp = MAIN + SITE_OFF, PLUGIN + TRAMP_OFF, 0x90000008
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': adrp, 'new': b(site, PLUGIN + THUNK_OFF), 'fn_start': True}]
        rec = self.long_form(adrp, TRAMP_OFF, [0x58000048, 0x14000003, *quad(site & ~0xFFF), b(t + 16, site + 4)])
        rec['writes'] = w
        hooks, reasons = xp.check_record(rec, IMPLS)
        self.assertEqual(reasons, [])
        self.assertEqual(hooks[0]['trampoline'], f'plugin0+{TRAMP_OFF:#x}')


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
        svcs, imports, indirect, outside, _ = xp.reach(bytes(img), self.BASE, 0x200, [self.BASE], {})
        self.assertEqual(svcs, {0x40})

    def test_imports_through_plt(self):
        img = code([b(self.BASE, self.BASE + 0x80, True), RET_W])
        svcs, imports, _, _, _ = xp.reach(img, self.BASE, 0x200, [self.BASE], {self.BASE + 0x80: 'malloc'})
        self.assertEqual((svcs, imports), (set(), {'malloc': 1}))

    def test_own_function_through_plt_is_followed(self):
        # bl plt; ret.  The PLT stub at 0x80 binds to the plugin's own function at 0x100: svc 0x40; ret
        img = bytearray(code([b(self.BASE, self.BASE + 0x80, True), RET_W]))
        struct.pack_into('<II', img, 0x100, SVC_40, RET_W)
        svcs, imports, _, _, _ = xp.reach(bytes(img), self.BASE, 0x200, [self.BASE],
                                          {self.BASE + 0x80: self.BASE + 0x100})
        self.assertEqual((svcs, imports), ({0x40}, {}))

    def test_plt_slot_of_own_symbol_maps_to_its_code(self):
        # Two PLT stubs (adrp x16, page 0; ldr x17, [x16, #slot]; add x16, x16, #slot; br x17) over GOT slots 0x180
        # and 0x188: the first binds an import, the second a symbol the plugin defines at +0x40.
        def stub(slot):
            return [0x90000010, 0xF9400211 | (slot // 8) << 10, 0x91000210 | slot << 10, 0xD61F0220]
        img = code(stub(0x180) + stub(0x188))
        nso = mock.Mock(relocs=[(0x180, xp.R_AARCH64_JUMP_SLOT, 1, 0), (0x188, xp.R_AARCH64_JUMP_SLOT, 2, 0)],
                        syms=[('', 0, 0, 0, 0), ('malloc', 0x12, 0, 0, 0),
                              ('_ZNSt6vectorIPKcSaIS1_EEC1ESt16initializer_listIS1_ERKS2_', 0x22, 7, 0x40, 8)])
        self.assertEqual(xp.plt_imports(nso, img, self.BASE, 0x200),
                         {self.BASE: 'malloc', self.BASE + 0x10: self.BASE + 0x40})

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

    def test_check_allows_the_game_imports_in_callbacks(self):
        # callback: bl <PLT stub of a hid import>; ret
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        plt_at = PLUGIN + 0x7000
        struct.pack_into('<II', img, CB_OFF, b(PLUGIN + CB_OFF, plt_at, True), RET_W)
        struct.pack_into('<I', img, IMPL_OFF, RET_W)
        rec['images']['plugin0'] = bytes(img)
        hid = '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj'
        hooks, _ = xp.check_record(rec, IMPLS)
        with tempfile.NamedTemporaryFile(delete=False) as f:
            rec['modules'][2]['file'] = f.name
        try:
            with mock.patch.object(xp, 'Nso'), mock.patch.object(xp, 'plt_imports', return_value={plt_at: hid}):
                refused, _ = xp.reach_check(rec, hooks, IMPLS)
                allowed, _ = xp.reach_check(rec, hooks, IMPLS, frozenset({hid}))
        finally:
            Path(f.name).unlink()
        self.assertEqual(refused, [f'the code its hooks reach calls the import {hid}'])
        self.assertEqual(allowed, [])

    def test_check_allows_break(self):   # an abort or a failed assert ends the game, as on the Switch
        rec = record()
        img = bytearray(rec['images']['plugin0'])
        struct.pack_into('<II', img, CB_OFF, 0xD40004C1, RET_W)   # svc 0x26
        struct.pack_into('<I', img, IMPL_OFF, RET_W)
        rec['images']['plugin0'] = bytes(img)
        hooks, _ = xp.check_record(rec, IMPLS)
        self.assertEqual(xp.reach_check(rec, hooks, IMPLS)[0], [])

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

    def test_code(self):   # SageWheel's far jump: code lines, the plugin target a seed, the way back an entry
        rec = far_record(back=MAIN + SITE_OFF + 12)
        out = self.lines(rec)
        words = [w['new'] for w in rec['writes']]
        for i, (old, new) in enumerate(zip((0xD10243FF, 0xA9037BFD, 0xA9046FFC), words)):
            self.assertIn(f'code mod_x main {SITE_OFF + 4 * i:x} {old:08x} {new:08x}', out)
            self.assertIn(f'expect mod_x main {SITE_OFF + 4 * i:x} {old:08x} {new:08x}', out)
        self.assertIn(f'seed plugin0 {THUNK_OFF:x}', out)
        self.assertIn(f'entry main {SITE_OFF + 12:x}', out)
        self.assertFalse(any(x.startswith(('site', 'orig', 'stub')) for x in out), out)

    def test_image_path_roundtrips_through_the_patch_table(self):
        rec = record()
        mods = [(m['name'], m['base'], m['size']) for m in rec['modules']]
        with tempfile.TemporaryDirectory() as d:
            for folder in ('plain', 'with spaces', 'with#hash', 'caf\u00e9 #1'):
                with self.subTest(folder=folder):
                    dest = Path(d) / folder
                    dest.mkdir()
                    image = dest / 'plugin0.img'
                    image.write_bytes(rec['images']['plugin0'])
                    rec['modules'][2]['image'] = image.name
                    lines = xp.plugin_lines(rec, [], 'mod_x', dest, 'plugin0')
                    patch = dest / 'patches.txt'
                    patch.write_text('set mod_x TOTK_MODPATCH_X 0 ordered\n' + '\n'.join(lines), encoding='utf-8')
                    actual = patches.load(patch, mods).plugins['plugin0']['image']
                    self.assertEqual(actual, image.resolve().as_posix())
                    self.assertEqual(Path(actual).read_bytes(), rec['images']['plugin0'])

    def test_quoted_image_and_legacy_unquoted_paths(self):
        mods = [('main', MAIN, 0x200000), ('plugin0', PLUGIN, 0x14000)]
        with tempfile.TemporaryDirectory() as d:
            patch = Path(d) / 'patches.txt'
            for image, field in (('C:/folder #1/plugin.img', '"C:/folder #1/plugin.img" # comment'),
                                 ('C:/plain/plugin.img', 'C:/plain/plugin.img # comment'),
                                 (r'C:\with space\plugin.img', json.dumps(r'C:\with space\plugin.img')),
                                 (r'C:\plain\plugin.img', r'C:\plain\plugin.img')):
                with self.subTest(field=field):
                    patch.write_text(f'set x ENV 0\nplugin x plugin0 {"0" * 64} {field}\n')
                    self.assertEqual(patches.load(patch, mods).plugins['plugin0']['image'], image)

    def test_malformed_quoted_image_names_the_patch_line(self):
        mods = [('main', MAIN, 0x200000), ('plugin0', PLUGIN, 0x14000)]
        with tempfile.TemporaryDirectory() as d:
            patch = Path(d) / 'patches.txt'
            for field in ('"unfinished', '""', '"image.img" extra', '"image.img" "#not a comment"'):
                with self.subTest(field=field):
                    patch.write_text(f'set x ENV 0\nplugin x plugin0 {"0" * 64} {field}\n')
                    with self.assertRaises(patches.PatchError) as error:
                        patches.load(patch, mods)
                    self.assertIn(f'{patch}:2:', str(error.exception))

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

    def test_stubs_written_back_to_back_are_each_a_start(self):
        # Zonai Fieldworks writes an inline-hook stub per member hook at init, back to back; a member switched on at
        # play points its gate at one (1.4.3 plugin+0x135048, the 4th). The .quad callback after each stub is data.
        rec = record()
        img, row = bytearray(rec['images']['plugin0']), 0x8000
        for k in range(3):
            at = row + 0x18 * k
            struct.pack_into('<4IQ', img, at, xp.STUR_X30, b(PLUGIN + at + 4, PLUGIN + IMPL_OFF, True), xp.LDUR_X30,
                             b(PLUGIN + at + 12, PLUGIN + TRAMP_OFF), PLUGIN + CB_OFF)
        rec['images']['plugin0'] = bytes(img)
        pre = bytearray(img)
        pre[row:row + 0x48] = bytes(0x48)
        rec['pre_images'] = {'plugin0': bytes(pre)}
        starts = xp.init_code(rec, [], 'plugin0')
        self.assertEqual(starts, {PLUGIN + row, PLUGIN + row + 0x18, PLUGIN + row + 0x30, PLUGIN + CB_OFF})

    def test_yield_loops_are_spin_waits(self):
        # Zonai Fieldworks' hook wrapper takes a byte lock (plugin+0xe3ff4 on 1.4.3): yield; ldaxrb; stxrb; cbnz; tbnz
        # back to the yield. A loop with a yield and a call in it, or a back branch with no yield, is no spin wait.
        ldaxrb, stxrb = 0x085FFC40, 0x08037C41
        words = [xp.NOP] * 64
        words[1:6] = [xp.YIELD, ldaxrb, stxrb, 0x35FFFFC3, 0x3707FF80]   # cbnz w3, -8; tbnz w0, #0, -16
        words[10:14] = [xp.YIELD, 0x94000100, xp.NOP, 0x17FFFFFD]        # yield; bl; nop; b -12
        words[20:23] = [xp.NOP, xp.NOP, 0x17FFFFFE]                      # b -8
        img = struct.pack('<64I', *words)
        self.assertEqual(xp.yield_spins(img, len(img)), {4: [20]})

    def test_hook_on_main_header_keeps_only_its_expect(self):
        # Challenge Mode's Enemy Health Regeneration on an update its table lacks: a replace hook at main+0x0, whose
        # word 1 is main's module header. The write is checked at start; the lift gets no site, orig or seed for it.
        new = b(MAIN, PLUGIN + THUNK_OFF)
        rec = record(writes=[{'module': 'main', 'offset': 0, 'old': 1, 'new': new}])
        hook = {'kind': 'replace', 'site': 'main+0x0', 'original': '0x00000001', 'target': f'plugin0+{THUNK_OFF:#x}',
                'trampoline': f'plugin0+{TRAMP_OFF:#x}'}
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'plugin0.img').write_bytes(rec['images']['plugin0'])
            rec['modules'][2]['image'] = 'plugin0.img'
            out = xp.plugin_lines(rec, [hook], 'mod_x', d, 'plugin0')
        self.assertIn(f'expect mod_x main 0 00000001 {new:08x}', out)
        self.assertFalse(any(x.startswith(('site', 'orig', 'seed', 'entry')) for x in out), out)

    def test_call(self):
        new = b(MAIN + SITE_OFF, PLUGIN + THUNK_OFF, True)
        w = [{'module': 'main', 'offset': SITE_OFF, 'old': 0x94000010, 'new': new}]
        out = self.lines(record(writes=w))
        self.assertIn(f'site mod_x main {SITE_OFF:x} 94000010 {new:08x}', out)
        self.assertFalse(any(x.startswith(('orig', 'stub')) for x in out))

    def test_data_word_matches_by_place(self):
        # Zonai Fieldworks' Self Recall takes its storage off a pool size in main's .bss (1.4.3 main+0x3ad6eb8): the
        # service run recorded 84800000 -> 7f1c0000, a start whose resolution grew the texture heap has
        # 87a00000 -> 823c0000 there. The word shapes no lifted code, so its expect line takes any value.
        rec = record(writes=[{'module': 'main', 'offset': 0x3ad6eb8, 'old': 0x84800000, 'new': 0x7F1C0000}])
        rec['modules'][0]['size'] = 0x3adf000
        out = self.lines(rec)
        self.assertIn('expect mod_x main 3ad6eb8 * *', out)
        self.assertFalse(any('84800000' in x or '7f1c0000' in x for x in out), out)

    def test_pointer_swap_keeps_its_words(self):
        # a table entry that now points at a plugin function is a seed of the lift: both its words stay exact
        new = PLUGIN + THUNK_OFF
        w = [{'module': 'main', 'offset': 0x180010, 'old': 0x00001000, 'new': new & 0xFFFFFFFF},
             {'module': 'main', 'offset': 0x180014, 'old': 0, 'new': new >> 32},
             {'module': 'main', 'offset': 0x180020, 'old': 5, 'new': 6}]
        out = self.lines(record(writes=w))
        self.assertIn(f'expect mod_x main 180010 00001000 {new & 0xFFFFFFFF:08x}', out)
        self.assertIn(f'expect mod_x main 180014 00000000 {new >> 32:08x}', out)
        self.assertIn('expect mod_x main 180020 * *', out)


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
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')   # clang for the start-up lookup below, as tests/flags_check.py
CC = (str(MSYS_BIN / 'clang.exe') if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists() else
      os.environ.get('CC') or shutil.which('clang'))
CC_ENV = dict(os.environ, PATH=str(MSYS_BIN) + os.pathsep + os.environ.get('PATH', '')) if os.name == 'nt' else None


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

    def table(self, site_line, extra=(), image=None):
        image = image or self.d / 'plugin0.img'
        text = '\n'.join(['set mod_x TOTK_MODPATCH_X 0 ordered',
                          f'plugin mod_x plugin0 {self.sha} {json.dumps(image.as_posix())}',
                          f'expect mod_x main {FN:x} aa0003f3 14000000', site_line, *extra]) + '\n'
        (self.d / 'p.txt').write_text(text)
        return patches.load(self.d / 'p.txt', self.mods)

    def test_generator_cli_reads_image_in_a_folder_with_spaces_and_hash(self):
        # The direct-emitter fixture needs a discoverable caller for a full CLI lift.
        main = self.d / 'exefs' / 'main'
        raw = bytearray(main.read_bytes())
        struct.pack_into('<II', raw, 0x108, b(8, FN, True), RET_W)
        main.write_bytes(raw)
        image = self.d / 'record files #1' / 'plugin0.img'
        image.parent.mkdir()
        image.write_bytes((self.d / 'plugin0.img').read_bytes())
        self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3',
                   ['stub plugin0 4000', 'seed plugin0 4100 4200'], image=image)
        out = self.d / 'generated'
        run = subprocess.run([sys.executable, str(ROOT / 'recomp/gen.py'), '--exefs', str(self.d / 'exefs'),
                              '--out', str(out), '--modules', 'main', '--plugins', str(self.plugin_file),
                              '--patches', str(self.d / 'p.txt'), '--jobs', '1'],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(layout.read(out), self.mods)
        self.assertTrue(list((out / 'plugin0').glob('*.c')))

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
        self.assertIn(f'X(0, "main", {FN:#x}, 0xaa0003f3u, 0x14000000u, 0)', h)
        self.assertIn('X("plugin0", 0x4100) X("plugin0", 0x4200)', h)

    def test_any_value_expect_line(self):
        # `*` for both words: runtime/plugin_init.c check_writes matches the word by place only
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3',
                       ['stub plugin0 4000', 'expect mod_x main 3ad6eb8 * *'])
        self.assertIn(('main', 0x3ad6eb8, None, None), t.plugins['plugin0']['expect'])
        h = patches.header_text(t, 'MARK')
        self.assertIn('X(0, "main", 0x3ad6eb8, 0x00000000u, 0x00000000u, 1)', h)
        self.assertIn(f'X(0, "main", {FN:#x}, 0xaa0003f3u, 0x14000000u, 0)', h)
        for half in ('* 7f1c0000', '84800000 *'):
            with self.subTest(half=half), self.assertRaises(patches.PatchError):
                self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3',
                           ['stub plugin0 4000', f'expect mod_x main 3ad6eb8 {half}'])

    # exlaunch's hook impl saves no NZCV: flags pass from the site to the callback and back (recomp/flags.py)
    def code(self, main_words, plugin_words=None):
        """Main's hooked function and the plugin image's words replaced; the plugin hash follows."""
        (self.d / 'exefs' / 'main').write_bytes(code_nso(main_words))
        img = bytearray((self.d / 'plugin0.img').read_bytes())
        for off, w in (plugin_words or {}).items():
            struct.pack_into('<I', img, off, w)
        (self.d / 'plugin0.img').write_bytes(bytes(img))
        n = Nso(str(self.plugin_file))
        self.sha = hashlib.sha256(bytes(img[n.text[0]:n.text[0] + n.text[1]])).hexdigest()

    def test_hook_site_hands_the_flags_to_the_callback_and_back(self):
        # cmp x0, x1; b.eq +8 (hooked); mov x0, x19; ret
        self.code({FN: 0xEB01001F, FN + 4: 0x54000040, FN + 8: 0xAA1303E0, FN + 12: RET_W})
        t = self.table(f'site mod_x main {FN + 4:x} 54000040 @call:plugin0+4000 54000040', ['stub plugin0 4000'])
        m = self.bases['main'] + FN
        c = self.emit(t, 'main', m, m + 16)
        stub = self.bases['plugin0'] + 0x4000
        on = c[c.index('if (rt_patch_on[0])'):]
        on, off = on[:on.index('} else {')], on[on.index('} else {'):]
        call = on.index(f'f_{stub:x}(c);')
        self.assertIn('c->nzcv = fnz_', on[:call])             # the callback sees the cmp's flags
        self.assertIn('F.nz = c->nzcv;', on[call:])            # the b.eq reads what the callback left
        self.assertIn('fc_nz(&F, CC_EQ)', on[call:])
        self.assertIn('fc_nz(&F, CC_EQ)', off[:off.index('}', 8)])   # off: the cmp's flags, packed into F.nz
        self.assertIn('F.nz = fnz_sub64(&F);', c)

    def test_plugin_return_leaves_its_flags(self):
        pb = self.bases['plugin0']
        # the callback: cmp w1, #5; ret
        self.code({FN: 0xAA0003F3, FN + 4: 0xAA1303E0, FN + 8: RET_W}, {0x4200: 0x7100143F, 0x4204: RET_W})
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3',
                       ['stub plugin0 4000', 'seed plugin0 4100 4200'])
        c = self.emit(t, 'plugin0', pb + 0x4200, pb + 0x4208)
        self.assertIn('c->nzcv = fnz_sub32(&F); return;', c)

    def test_plugin_call_takes_the_callee_flags(self):
        pb = self.bases['plugin0']
        # bl 0x4200; b.eq +8; ret; ret
        self.code({FN: 0xAA0003F3, FN + 4: 0xAA1303E0, FN + 8: RET_W},
                  {0x4100: b(pb + 0x4100, pb + 0x4200, True), 0x4104: 0x54000040, 0x4108: RET_W, 0x410c: RET_W})
        t = self.table(f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3',
                       ['stub plugin0 4000', 'seed plugin0 4100 4200'])
        c = self.emit(t, 'plugin0', pb + 0x4100, pb + 0x4110)
        body = c[c.index(f'void f_{pb + 0x4100:x}(Cpu *c) {{'):]
        self.assertLess(body.index(f'c->pc = {pb + 0x4200:#x}ull; rt_dispatch(c);'), body.index('F.nz = c->nzcv;'))
        self.assertIn('fc_nz(&F, CC_EQ)', body)
        self.assertEqual(body.count('c->nzcv = fnz_nz(&F); return;'), 2)   # both returns leave the flags

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

    def test_branch_hook_on_a_flag_write_leaves_with_the_flags_before_it(self):
        # cmp x0, x1; cmp x2, x3 (b to the plugin's trampoline, which runs the cmp); b.eq +8; ret; ret
        pb = self.bases['plugin0']
        m = self.bases['main'] + FN
        self.code({FN: 0xEB01001F, FN + 4: 0xEB03005F, FN + 8: 0x54000040, FN + 12: RET_W, FN + 16: RET_W})
        t = self.table(f'site mod_x main {FN + 4:x} eb03005f {b(m + 4, pb + 0x4200):08x}', ['seed plugin0 4200'])
        c = self.emit(t, 'main', m, m + 20)
        on = c[c.index('if (rt_patch_on[0])'):]
        on = on[:on.index('} else {')]
        self.assertIn('c->nzcv = fnz_sub64(&F);', on)      # the first cmp's flags go to the trampoline
        self.assertIn(f'{pb + 0x4200:#x}', on)

    def test_inline_hook_on_a_register_call(self):
        # blr x8 (inline-hooked); ret
        self.code({FN: 0xD63F0100, FN + 4: RET_W})
        t = self.table(f'site mod_x main {FN:x} d63f0100 @call:plugin0+4000 d63f0100', ['stub plugin0 4000'])
        m = self.bases['main'] + FN
        c = self.emit(t, 'main', m, m + 8)
        stub = self.bases['plugin0'] + 0x4000
        on = c[c.index('if (rt_patch_on[0])'):]
        on = on[:on.index('} else {')]
        self.assertLess(on.index(f'f_{stub:x}(c);'), on.index('rt_dispatch(c);'))   # the hook, then the blr
        self.assertIn(f'x30 = {m + 4:#x}ull; c->x[30] = x30; rt_dispatch(c);', on)

    def test_orig_trampoline_counts_as_a_seed(self):
        # a veneer slot names a replace hook's trampoline: dispatch serves it with the function's unpatched copy
        main = self.d / 'exefs' / 'main'
        raw = bytearray(main.read_bytes())
        struct.pack_into('<II', raw, 0x108, b(8, FN, True), RET_W)
        main.write_bytes(raw)
        m = self.bases['main'] + FN
        self.table(f'site mod_x main {FN:x} aa0003f3 {b(m, self.bases["plugin0"] + 0x4200):08x}',
                   [f'orig mod_x plugin0 2000 main {FN:x}', 'seed plugin0 4200 2000'])
        run = subprocess.run([sys.executable, str(ROOT / 'recomp/gen.py'), '--exefs', str(self.d / 'exefs'),
                              '--out', str(self.d / 'generated'), '--modules', 'main,plugin0', '--plugins',
                              str(self.plugin_file), '--patches', str(self.d / 'p.txt'), '--jobs', '1'],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(CC, 'no clang')
    def test_start_matches_the_record(self):
        # runtime/plugin_writes.h plugin_write_find on this table's patches.h, as runtime/plugin_init.c check_writes
        # looks up each word an init changed: Self Recall's pool size at main+0x3ad6eb8, recorded by the service run as
        # 84800000 -> 7f1c0000 and written at a start with a grown texture heap as 87a00000 -> 823c0000
        site = f'site mod_x main {FN:x} aa0003f3 @call:plugin0+4000 aa0003f3'
        cases = [(FN, 0xAA0003F3, 0x14000000, 0), (FN, 0xAA0003F3, 0x14000001, -1),
                 (0x3AD6EB8, 0x84800000, 0x7F1C0000, 1), (0x3AD6EB8, 0x87A00000, 0x823C0000, 1),
                 (0x3AD6EBC, 0x87A00000, 0x823C0000, -1)]
        main = ['#include "patches.h"', '#include "plugin_writes.h"',
                'static const PluginWrite tab[] = {RT_PLUGIN_WRITES(PLUGIN_WRITE)};',
                'int main(void) {', '    int n = sizeof tab / sizeof *tab, bad = 0;']
        for i, (off, old, new, want) in enumerate(cases):
            main.append(f'    bad |= (plugin_write_find(tab, n, 0, "main", {off:#x}, {old:#x}u, {new:#x}u) != {want}) '
                        f'<< {i};')
        main += ['    return bad;', '}']
        (self.d / 'start.c').write_text('\n'.join(main) + '\n')
        exe = self.d / 'start.exe'
        for record, failed in (('expect mod_x main 3ad6eb8 84800000 7f1c0000', 1 << 3),
                               ('expect mod_x main 3ad6eb8 * *', 0)):
            with self.subTest(record=record):
                (self.d / 'patches.h').write_text(patches.header_text(self.table(site, ['stub plugin0 4000', record]),
                                                                      'MARK'))
                subprocess.run([CC, '-std=gnu11', '-Wall', '-Werror', '-I', str(self.d), '-I', str(ROOT / 'runtime'),
                                str(self.d / 'start.c'), '-o', str(exe)], check=True, env=CC_ENV, timeout=60)
                # the exact record misses only the play-time write
                self.assertEqual(subprocess.run([str(exe)], env=CC_ENV, timeout=60).returncode, failed)

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

    def code_table(self, words, extra=()):
        """A table with the plugin and `code` lines putting words ({main offset: word}) over main's function."""
        old = {FN: 0xAA0003F3, FN + 4: 0xAA1303E0, FN + 8: RET_W}
        text = ['set mod_x TOTK_MODPATCH_X 0 ordered',
                f'plugin mod_x plugin0 {self.sha} {json.dumps((self.d / "plugin0.img").as_posix())}', 'seed plugin0 4200',
                *[f'code mod_x main {o:x} {old[o]:08x} {w:08x}' for o, w in words.items()], *extra]
        (self.d / 'c.txt').write_text('\n'.join(text) + '\n')
        return patches.load(self.d / 'c.txt', self.mods)

    def emit_code(self, t, starts=()):
        m = self.bases['main'] + FN
        mod = gen.open_module('main', self.d / 'exefs/main', self.bases['main'], t)
        patches.check(t, mod, 'main')
        e = gen.Emitter(mod, {m, *starts}, t.for_module('main', self.bases['main']),
                        codes=t.codes_for('main', self.bases['main']))
        return gen.chunk_text('main', 0, e, [(m, m + 12)])

    def test_code_copy_of_a_far_jump(self):
        # SageWheel: adrp x17; add x17, x17, #lo; br x17 over the function's first three words, into plugin0+0x4200
        m, target = self.bases['main'] + FN, self.bases['plugin0'] + 0x4200
        words = dict(zip((FN, FN + 4, FN + 8), far_jump(FN, target)))
        t = self.code_table(words)
        c = self.emit_code(t)
        own, copy = c[c.index(f'void f_{m:x}(Cpu *c) {{'):c.index(f'static void fw0_{m:x}(Cpu *c) {{')], \
            c[c.index(f'static void fw0_{m:x}(Cpu *c) {{'):]
        self.assertIn(f'static void fw0_{m:x}(Cpu *c);', c[:c.index(f'void f_{m:x}(Cpu *c) {{')])
        self.assertIn(f'if (rt_patch_on[0]) {{ __attribute__((musttail)) return fw0_{m:x}(c); }}', own)
        self.assertIn('return;', own)                                       # the game's own body stays
        self.assertNotIn('rt_patch_on', copy)
        # br x17 as a branch to its constant: through the dispatch table, where plugin_trace counts the hook
        self.assertIn(f'c->pc = {target:#x}ull; __attribute__((musttail)) return rt_dispatch(c);', copy)
        self.assertNotIn('c->pc = x17', copy)

    def test_code_copy_keeps_other_sets_sites(self):
        # a cheat on the function's last word stays in the copy; the plugin's nop over the first word is in it
        t = self.code_table({FN: xp.NOP}, ['set cheat TOTK_CHEAT 0', f'site cheat main {FN + 4:x} aa1303e0 aa1f03e0'])
        c = self.emit_code(t)
        copy = c[c.index('static void fw0_'):]
        self.assertIn('rt_patch_on[1]', copy)
        self.assertNotIn('x19 = x0', copy.split('rt_patch_on[1]')[0])     # the mov the nop replaced is gone

    def test_code_copy_branch_needs_a_start(self):
        m = self.bases['main'] + FN
        t = self.code_table({FN: b(m, m + 0x400)})
        with self.assertRaises(patches.PatchError):
            self.emit_code(t)
        self.assertIn(f'__attribute__((musttail)) return f_{m + 0x400:x}(c);', self.emit_code(t, {m + 0x400}))

    def test_code_line_checks_the_original(self):
        t = self.code_table({FN: xp.NOP})
        t.codes['main'][FN] = [(0, xp.NOP, 0x12345678)]
        with self.assertRaises(patches.PatchError):
            patches.check(t, gen.open_module('main', self.d / 'exefs/main', self.bases['main'], t), 'main')

    def test_no_plugin_lines_no_plugin_header(self):
        (self.d / 'q.txt').write_text('set fps60 TOTK_FPS60 0\n')
        self.assertNotIn('PLUGIN', patches.header_text(patches.load(self.d / 'q.txt', self.mods), 'MARK'))


if __name__ == '__main__':
    unittest.main()
