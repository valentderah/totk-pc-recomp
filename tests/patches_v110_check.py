"""Check the v110 patch contracts without game data; optionally check every site and spin against local ExeFS.

python tests/patches_v110_check.py --exefs <local ExeFS> [--cflags=-march=x86-64-v2]
Uses the differential harness, Clang (CC) and Unicorn. Extracted bytes and generated C stay in build/.
"""
import argparse
import math
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import BUF, BUF_SIZE, Emu, build_harness, diff_state, run_harness
import patches
from lifter import Lifter
from layout import layout
from module import Module

# Set, offset, target register, result kind. The patch file owns the instruction words.
SITES = [
    ('realtime', 0xa20518, 1, 'zero'),
    ('cursor', 0xf31e80, 0, 'map'),
    ('cursor', 0x1d55f24, 12, 'aim'),
    ('cursor', 0x1d556a4, 3, 'gyro'),
    *[('aspect', a, 0, 'aspect') for a in (0xcdbf30, 0x1261a64, 0x19c2260, 0x1ed8fa4)],
    *[('fov', a, 0, 'fov') for a in (0xcdbef8, 0x1261a2c)],
    ('pooloff', 0x9b93a8, 2, 'offset'), ('pooloff', 0x9b93c4, 1, 'source'),
    ('shadow', 0xd0760c, 11, 'shadow'),
    *[('cascade', a, 8, 'cascade') for a in (0x1157fb8, 0x1158040, 0x11580c8, 0x115814c)],
]


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def scalar(v):
    return struct.unpack('<f', (v & 0xffffffff).to_bytes(4, 'little'))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exefs', type=Path)
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    args = ap.parse_args()
    mods = layout(args.exefs) if args.exefs else [
        ('main', 0x7000000000, 0x472d000), ('subsdk0', 0x7004800000, 0x400000),
        ('sdk', 0x7004c00000, 0x800000)]
    table = patches.load(ROOT / 'recomp' / 'patches-v110.txt', mods)
    base = mods[0][1]
    if args.exefs:
        for name, at, _ in mods:
            mod = Module(name, args.exefs / name, at)
            patches.check(table, mod, name)
        print(f'v110: all {sum(len(s) for s in table.sites.values())} sites and '
              f'{sum(len(s) for s in table.spins.values())} spin back-edges match local ExeFS')
    by_name = {name: i for i, (name, _, _) in enumerate(table.sets)}
    resolved = table.for_module('main', base)
    bodies, words = [], []
    lift = Lifter(base, 'c')
    for name, off, _, _ in SITES:
        alternatives = table.sites['main'][off]
        assert len(alternatives) == 1 and alternatives[0][0] == by_name[name]
        orig = alternatives[0][2]
        repl = resolved[base + off][0][1]
        original = lift.lift(orig, base + off)
        replacement = [lift.lift(w, base + off) for w in repl]
        assert original.kind == 'op' and all(i.kind == 'op' for i in replacement)
        bodies.append('if (c->x[30]) { ' + ' '.join(i.code for i in replacement) +
                      ' } else { ' + original.code + ' }')
        words.append((orig, repl))
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    groups = {ratio: ([], []) for ratio in (1.0, 0.5, 0.25)}
    page = bytearray(0x1000)
    # Distinct fields reveal an accidental load from the neighboring page slot.
    for off, value in ((0, 0.5), (4, 1 / 60), (56, 21 / 9), (60, 1.2),
                       (64, 25), (68, 50), (72, 100), (76, 200)):
        struct.pack_into('<f', page, off, value)
    struct.pack_into('<I', page, 92, 2048)
    initial = bytearray(BUF_SIZE)
    for off, value in ((0, 3), (4, 5), (8, 0.5), (0x13c, 0.9), (0x140, 16 / 9)):
        struct.pack_into('<f', initial, 0x2000 + off, value)
    struct.pack_into('<Q', initial, 0x2000, BUF + 0x3000)
    struct.pack_into('<f', initial, 0x3008, 0.5)
    with tempfile.TemporaryDirectory(prefix='patch-v110-', dir=build) as temp:
        work = Path(temp)
        exe = build_harness(work, bodies, 2, args.cflags)
        for k, (name, off, target, kind) in enumerate(SITES):
            pc = base + off
            for enabled in (0, 1):
                for ratio in (1.0, 0.5, 0.25):
                    for offset in (0, 0x7fffffff, 0x80000000, 0xffffffff):
                        mem = bytearray(initial)
                        struct.pack_into('<f', mem, 0x2008, ratio)
                        struct.pack_into('<f', mem, 0x3008, ratio)
                        code = words[k][1] if enabled else [words[k][0]]
                        lo = pc & ~0xffff
                        text = bytearray((pc - lo) + len(code) * 4)
                        struct.pack_into('<' + 'I' * len(code), text, pc - lo, *code)
                        emu = Emu([(lo, text), (table.page, page)])
                        emu.mu.mem_write(BUF, bytes(mem))
                        x = [0x1234567800000000 + i for i in range(31)]
                        for i in (8, 19, 20, 23, 27):
                            x[i] = BUF + 0x2000
                        x[25], x[30] = offset, enabled
                        v = [(0x123456789abcdef << 32) | int.from_bytes(struct.pack('<f', 2.0), 'little')] * 32
                        v[1] = int.from_bytes(struct.pack('<f', 4.0), 'little')
                        v[9] = int.from_bytes(struct.pack('<f', 3.0), 'little')
                        v[10] = int.from_bytes(struct.pack('<f', 4.0), 'little')
                        v[12] = int.from_bytes(struct.pack('<f', 8.0), 'little')
                        sp, nzcv = BUF + BUF_SIZE - 0x100, 0xa0000000
                        ex, esp, enz, epc, ev = emu.run(pc, 0, x, sp, nzcv, v, count=len(code))
                        assert esp == sp and enz == nzcv
                        if enabled:
                            want = {'zero': 0, 'map': 2 * ratio, 'aim': 2 * ratio, 'gyro': 6 * ratio,
                                    'aspect': f32(21 / 9), 'fov': f32(f32(0.9) * f32(1.2))}
                            if kind in want:
                                assert math.isclose(scalar(ev[target]), want[kind], rel_tol=1e-6, abs_tol=1e-7), (kind, off)
                            elif kind == 'offset':
                                assert ex[target] == offset
                            elif kind == 'source':
                                assert ex[target] == x[8] + offset
                            elif kind == 'shadow':
                                assert ex[target] == 2055
                            elif kind == 'cascade':
                                slot = (64, 68, 72, 76)[(0x1157fb8, 0x1158040, 0x11580c8, 0x115814c).index(off)]
                                assert ex[target] == struct.unpack_from('<I', page, slot)[0]
                            # The stack save must preserve the complete x10, not just its lower word.
                            if name in ('aspect', 'fov'):
                                assert ex[10] == x[10]
                        allowed_x = {target} if kind in ('offset', 'source', 'shadow', 'cascade') else {9} if kind == 'map' else set()
                        allowed_v = {0, 1} if kind == 'map' else {target}
                        if enabled and kind in ('map', 'aim', 'gyro'):
                            allowed_v |= {16}
                            if kind == 'map':
                                allowed_v |= {1}
                        if enabled and kind == 'fov':
                            allowed_v |= {8}
                        assert all(ex[i] == x[i] for i in range(31) if i not in allowed_x)
                        assert all(ev[i] == v[i] for i in range(32) if i not in allowed_v)
                        cases, expect = groups[ratio]
                        cases.append((k, 0, x, sp, nzcv, v, True))
                        memory = zlib.crc32(emu.mu.mem_read(BUF, BUF_SIZE))
                        expect.append((ex, esp, enz, epc, ev, memory))
        page_path = work / 'page.bin'
        page_base = table.page & ~0xffff  # Windows reserves at 64 KiB allocation granularity
        page_path.write_bytes(bytes(table.page - page_base) + page)
        total = 0
        for ratio, (cases, expect) in groups.items():
            mem = bytearray(initial)
            struct.pack_into('<f', mem, 0x2008, ratio)
            struct.pack_into('<f', mem, 0x3008, ratio)
            records, err = run_harness(exe, work, mem, cases, [(page_base, page_path)])
            assert not err and len(records) == len(cases), err
            for case, want, got in zip(cases, expect, records):
                differences = diff_state(got, want, False, True)
                assert not differences, (SITES[case[0]], differences)
            total += len(cases)
        print(f'v110: {len(SITES)} patch contracts, {total} native/Unicorn cases passed (on and off)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
