"""Check the 1.1.0 lightning sites: table words, the time-object chain and the chance scaling.

Synthetic cases lift the table's replacement and compare it with Unicorn (Clang through the shared harness);
--exefs adds the local original words, window hashes, the rodata chances and the game's own chain instructions.
A run without Clang checks the table and reports the dynamic part as not run.
"""
import argparse
import hashlib
import math
import os
import shutil
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
import numpy as np
from common import BUF, BUF_SIZE, Emu, build_harness, compiler, diff_state, run_harness
import patches
from layout import layout
from lifter import Lifter
from module import Module
from nso import R_AARCH64_RELATIVE

SITES = (0xbddb30, 0xbddd70, 0xbdddf4)
CHANCE = {0xbddb30: 1, 0xbddd70: 1, 0xbdddf4: 9}   # s1, s1, s9: the register each load fills
DISP = {0xbddb30: 0xc5c, 0xbddd70: 0xf50, 0xbdddf4: 0xf50}
VALUE = {0xbddb30: 0x3c5a72a8, 0xbddd70: 0x3c23d70a, 0xbdddf4: 0x3c23d70a}   # 0.013333, 0.01, 0.01
WINDOWS = {0xbddb30: '8dcb5cfa49320817b226f6c1703317cbc46bfb7d93f3e2ef021436ee2388e2e0',
           0xbddd70: 'dcc9217db839dc30a1fa036c73d06ba3c1fb555d24c1885b40f35437f15ffb45',
           0xbdddf4: 'c4864d703ba6837ac786d4e4b2400de4f2f5455fa114654aca6f5bc9bcf65377'}
SLOT_PAGE, SLOT = 0x4630000, 0x4630920          # the time manager's GOT slot (set bounce reads it at 0xbee714)
CHANCE_BUF = 0x3000                             # x8 in every case; the load reads x8 + DISP
STORE, MGR, TIME = BUF + 0x8000, BUF + 0x8100, BUF + 0x8200   # [slot] -> storage -> manager -> time object


def bits(x):
    return int.from_bytes(struct.pack('<f', x), 'little')


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def ldr_x(rt, rn, off):
    return 0xf9400000 | ((off // 8) << 10) | (rn << 5) | rt


def minnum(a, b):
    """AArch64 FMINNM: the non-NaN operand of the two."""
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    return a if a < b else b


def local_check(exefs, table, mods):
    for name, base, _ in mods:
        patches.check(table, Module(name, exefs / name, base), name)
    main = Module('main', exefs / 'main', mods[0][1])
    base = mods[0][1]
    assert main.read(base + 0x3816c5c, 4) == 0x3c5a72a8, '0.013333'
    assert main.read(base + 0x3816f50, 4) == 0x3c23d70a, '0.01'
    # The chain the replacement copies, from the game's own slow-motion check (set bounce).
    assert main.word(base + 0xbee70c) == patches.adrp(base + 0xbee70c, base + SLOT_PAGE, 26)
    assert main.word(base + 0xbee714) == ldr_x(26, 26, SLOT - SLOT_PAGE)
    assert main.word(base + 0xbee718) == ldr_x(8, 26, 0)
    assert main.word(base + 0xbee7dc) == ldr_x(8, 8, 0xc8)
    assert main.word(base + 0xbee7e0) == 0xbd402900   # ldr s0, [x8, #0x28]
    slot = [(t, a) for o, t, _, a in main.nso.relocs if o == SLOT]
    assert len(slot) == 1 and slot[0][0] == R_AARCH64_RELATIVE, 'the slot is a GOT entry to the singleton storage'
    for site, want in WINDOWS.items():
        assert hashlib.sha256(main.image[site - 0x20:site + 0x20]).hexdigest() == want, hex(site)
    print('v110: %d lightning original words, %d window hashes, the rodata chances and the time-object chain passed'
          % (len(SITES), len(WINDOWS)))


def synthetic(table, base, cflags, work_dir):
    cc = compiler()
    if shutil.which(cc) is None and not Path(cc).exists():
        print('v110: lightning synthetic cases not run: no Clang found (set CC)')
        return
    resolved = table.for_module('main', base)
    lift = Lifter(base, 'c')
    bodies, specs = [], []
    for site in SITES:
        alternatives = resolved[base + site]
        assert len(alternatives) == 1
        on = alternatives[0][1]
        off = [table.sites['main'][site][0][2]]
        def lower(words, pc):
            ops = [lift.lift(w, pc) for w in words]
            assert all(i.kind == 'op' for i in ops)
            return ' '.join(i.code for i in ops)
        bodies.append('if (c->x[30]) { ' + lower(on, base + site) + ' } else { ' + lower(off, base + site) + ' }')
        specs.append((site, on, off))
    args_work = work_dir
    args_work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='v110-lightning-', dir=args_work) as temp:
        work = Path(temp)
        exe = build_harness(work, bodies, 1, cflags)
        page = bytearray(0x10000)
        struct.pack_into('<Q', page, SLOT - SLOT_PAGE, STORE)
        page_path = work / 'chain.bin'
        page_path.write_bytes(page)
        ratios = (0.0, 0.25, 0.5, 0.75, 1.0, 1.0 + 2 ** -23, 2.0, 30.0, math.inf, math.nan)
        total = 0
        for ratio in ratios:
            mem = bytearray(BUF_SIZE)
            struct.pack_into('<Q', mem, STORE - BUF, MGR)
            struct.pack_into('<Q', mem, MGR - BUF + 0xc8, TIME)
            struct.pack_into('<f', mem, TIME - BUF + 0x38, ratio)
            for site in SITES:
                struct.pack_into('<I', mem, CHANCE_BUF + DISP[site], VALUE[site])
            cases, expected = [], []
            for k, (site, on, off) in enumerate(specs):
                for enabled in (0, 1):
                    x = [0x1234567800000000 + i * 0x101 for i in range(31)]
                    x[8], x[30] = BUF + CHANCE_BUF, enabled
                    v = [((0xfedcba987654321001234567 + i) << 32) | bits(3.0 + i) for i in range(32)]
                    chance = CHANCE[site]
                    v[chance] = (v[chance] & ~0xffffffff) | VALUE[site]
                    sp = BUF + BUF_SIZE - 0x100
                    code = on if enabled else off
                    pc = base + site
                    lo = pc & ~0xffff
                    text = bytearray(pc - lo + 4 * len(code))
                    struct.pack_into('<' + 'I' * len(code), text, pc - lo, *code)
                    emu = Emu([(lo, text), (base + SLOT_PAGE, page)])
                    emu.mu.mem_write(BUF, bytes(mem))
                    ex, esp, enz, epc, ev = emu.run(pc, 0, x, sp, 0, v, count=len(code))
                    clamped = f32(minnum(ratio, 1.0))
                    loaded = struct.unpack('<f', struct.pack('<I', VALUE[site]))[0]
                    # Off runs the game's load: the chance stays the rodata value, not the product.
                    want = float(np.float32(np.float32(loaded) * np.float32(clamped))) if enabled else loaded
                    # Only x10, the two scratch registers and the chance may change; the chance's chance value is
                    # its own product (on) or the game's load (off).
                    assert esp == sp and enz == 0, (hex(site), enabled, esp, sp, enz)
                    assert all(ex[i] == x[i] for i in range(31) if i != 10), 'GPR preservation'
                    allowed = {chance} | ({3, 4} if enabled else set())
                    assert all(ev[i] == v[i] for i in range(32) if i not in allowed), 'full SIMD preservation'
                    assert ex[10] == (TIME if enabled else x[10]), 'x10 holds the time object only when on'
                    if enabled:
                        assert ev[3] == bits(clamped), 'clamped ratio in s3'
                        assert ev[4] == bits(1.0), 'the 1.0 in s4'
                    assert ev[chance] == bits(want), (hex(site), enabled, ratio, ev[chance], bits(want))
                    actual = bytes(emu.mu.mem_read(BUF, BUF_SIZE))
                    assert actual == bytes(mem), 'the sites only read memory'
                    cases.append((k, 0, x, sp, 0, v, True))
                    expected.append((ex, esp, enz, epc, ev, zlib.crc32(bytes(mem))))
            records, err = run_harness(exe, work, mem, cases, [(base + SLOT_PAGE, page_path)])
            assert not err and len(records) == len(cases), err
            for case, want, got in zip(cases, expected, records):
                delta = diff_state(got, want, False, True)
                assert not delta, (specs[case[0]][0], delta)
            total += len(cases)
        print(f'v110: {total} lightning native/Unicorn cases passed; 3 sites x on/off x {len(ratios)} ratios, '
              'full GPR/SIMD/SP/NZCV/memory')


def table_check(table, mods):
    """The three sites exist once each, keep the game's load and decode to the intended chain."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    base = mods[0][1]
    resolved = table.for_module('main', base)
    for site in SITES:
        alternatives = table.sites['main'][site]
        assert len(alternatives) == 1
        words = resolved[base + site][0][1]
        assert len(words) == 9 and words[0] == alternatives[0][2], hex(site)
        text = [f'{i.mnemonic} {i.op_str}' for i in md.disasm(b''.join(struct.pack('<I', w) for w in words), base + site)]
        assert text[0] == ('ldr s%d, [x8, #0x%x]' % (CHANCE[site], DISP[site])), (hex(site), text[0])
        assert text[1] == 'adrp x10, #%#x' % (base + SLOT_PAGE), (hex(site), text[1])
        assert text[2] == 'ldr x10, [x10, #0x920]', (hex(site), text[2])
        assert text[3] == 'ldr x10, [x10]', (hex(site), text[3])
        assert text[4] == 'ldr x10, [x10, #0xc8]', (hex(site), text[4])
        assert text[5] == 'ldr s3, [x10, #0x38]', (hex(site), text[5])
        assert text[6] == 'fmov s4, #1.00000000', (hex(site), text[6])
        assert text[7] == 'fminnm s3, s3, s4', (hex(site), text[7])
        assert text[8] == 'fmul s%d, s%d, s3' % (CHANCE[site], CHANCE[site]), (hex(site), text[8])
    print('v110: 3 lightning sites keep their load and decode to the time-object chain, the clamp and the product')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exefs', type=Path)
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    ap.add_argument('--work-dir', type=Path, default=ROOT / 'build')
    args = ap.parse_args()
    mods = layout(args.exefs) if args.exefs else [
        ('main', 0x7000000000, 0x472d000), ('subsdk0', 0x7004800000, 0x400000),
        ('sdk', 0x7004c00000, 0x800000)]
    table = patches.load(ROOT / 'recomp' / 'patches-v110.txt', mods)
    sets = {name: (env, default) for name, env, default in table.sets}
    assert sets['lightning'] == ('TOTK_LIGHTNING_FIX', 1)
    names = [name for name, _, _ in table.sets]
    assert names.index('gears') < names.index('lightning') and names[-1] == 'lightning'
    table_check(table, mods)
    if args.exefs:
        local_check(args.exefs, table, mods)
    synthetic(table, mods[0][1], args.cflags, args.work_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
