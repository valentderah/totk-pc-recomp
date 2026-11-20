"""Check the 1.1.0 free-camera site, native host contract and emitted BL; --exefs proves local CFG/layout anchors.

No game data is required for synthetic checks. All generated files stay under build/.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import BUF, BUF_SIZE, Emu, build_harness, compiler, diff_state, run_harness
import patches
from gen import Emitter
from layout import layout
from lifter import Lifter
from module import Module

SITE, ENTRY, END = 0xcdb6cc, 0xcdb628, 0xcdc5d4
WINDOWS = (
    (ENTRY, 0xa8, 'f1a68a482cbae58388ab6df3de36aa20a496966a6c0153029d475191893b104a'),
    (SITE, 0x120, '6df07e4d45d22d45b0f0d2241c33d5aadcab72cd621a351e1cfdf328eb323632'),
    (0xcdc250, 0x24, 'f00fcfb42d15c8a61e23af148912abf84ae8157140158b1a2b23609b3cc76e1b'),
    (0xe123a4, 0x1c, '8ea77ad0b84a11ee2677c2b245fea1bd9e92fbcaaa0c3a80768e5e9cb63f60c4'),
    (0xe1c0c0, 0x30, 'eab08d554dbee22d060e2fbb0c5e3e636068a970e0123340002443245d8fcc86'),
)


def local_check(exefs, mods, table):
    for name, base, _ in mods:
        patches.check(table, Module(name, exefs / name, base), name)
    base = mods[0][1]
    mod = Module('main', exefs / 'main', base)
    assert mod.nso.build_id == 'd5ad6ac71ef53e3e52417c1b81dbc9b4142aa3b3'
    starts = set(mod.eh_frame_functions())
    assert base + ENTRY in starts and base + END in starts
    assert not any(base + ENTRY < a < base + END for a in starts)
    for off, length, digest in WINDOWS:
        assert hashlib.sha256(mod.image[off:off + length]).hexdigest() == digest, hex(off)
    assert mod.word(base + SITE) == 0xf9400e68
    emitter = Emitter(mod, starts)
    insns = [mod.lift(a) for a in range(base + ENTRY, base + END, 4)]
    assert not any(i.kind in ('br', 'trap') for i in insns), 'Every local edge must be resolved'
    assert all(base + ENTRY <= i.target < base + END for i in insns if i.kind in ('b', 'bcond'))
    flow = emitter.flow(insns, base + ENTRY, base + END, {})
    def reachable(blocked=None):
        seen, todo = set(), [0]
        while todo:
            i = todo.pop()
            if i == blocked or i in seen:
                continue
            seen.add(i)
            todo.extend(flow[i])
        return seen
    reached = reachable()
    bypass = reachable((SITE - ENTRY) // 4)
    assert len(reached) == 1003
    pose_reads = (0xcdb6d0, 0xcdb6f4, 0xcdb6f8, 0xcdb734, 0xcdb754, 0xcdb764, 0xcdb7a0, 0xcdc250, 0xcdc264)
    for off in pose_reads:
        idx = (off - ENTRY) // 4
        assert idx in reached and idx not in bypass, 'Hook must dominate pose reads and override blocks'
    returns = [ENTRY + i * 4 for i in reached if insns[i].kind == 'ret']
    assert returns == [0xcdc018]
    # Saved link register and caller/getter layout: component = actor.components[0x270 / 8].
    from common import dis
    assert dis(mod.word(base + 0xcdb630)) == 'stp x29, x30, [sp, #0x80]'
    assert dis(mod.word(base + 0xcdc010)) == 'ldp x29, x30, [sp, #0x80]'
    assert dis(mod.word(base + 0xcdb654)) == 'mov x19, x0'
    assert mod.lift(base + 0xe123b8).target == base + ENTRY
    assert mod.lift(base + 0xe123ac).target == base + 0xe1c0c0
    assert mod.word(base + 0xe1c0c0) == 0xf9411408  # ldr x8,[x0,#0x228]
    assert mod.word(base + 0xe1c0c8) == 0xf9413900  # ldr x0,[x8,#0x270]
    print('v110 freecam: all opcode/spin guards, 5 window hashes, EH bounds, 1003-node CFG dominance and actor getter passed')


def native_check(work, table, source=None, cflags='-march=x86-64-v3'):
    # The actual emitter handles the patch BL, spilled x19, x8 reload and link save/restore, not handwritten C.
    base = 0x7000000000
    words = {SITE - 4: 0xf81f0ffe, SITE: 0xf9400e68, SITE + 4: 0xf84107fe, SITE + 8: 0xd65f03c0}
    lift = Lifter(base, 'c')
    mod = SimpleNamespace(lifter=lift, hi=base + SITE + 12,
                          lift=lambda a: lift.lift(words[a - base], a), plugin=False)
    emitted, refs = Emitter(mod, {base + SITE - 4}, table.for_module('main', base), hooks=set()).function(
        base + SITE - 4, base + SITE + 12)
    assert not refs
    nl = chr(10)
    generated = nl.join(emitted).replace('f_%x' % (base + SITE - 4), 'test_lift')
    from gen import header
    (work / 'freecam_lift.h').write_text(header('synthetic freecam contract') + generated, encoding='utf-8')
    defs = '#pragma once' + nl + '#define RT_PATCH_COUNT %d' % len(table.sets) + nl
    defs += '#define RT_PATCH_PAGE %#xull' % table.page + nl
    defs += '#define RT_PATCH_SETS(X) ' + ' '.join('X(%d,%s,"%s",%d)' % (i, *s) for i, s in enumerate(table.sets)) + nl
    (work / 'patches.h').write_text(defs, encoding='utf-8')
    origin = source or ROOT / 'runtime' / 'freecam.c'
    (work / 'freecam_source.h').write_text('#include "' + origin.as_posix() + '"' + nl, encoding='utf-8')
    for version in (110, 121, 143, 100, 144):
        exe = work / ('native-%d' % version + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([compiler(), '-std=gnu11', '-O1', '-UNDEBUG', *cflags.split(), '-DTEST_VERSION=%d' % version,
                        '-I', str(work), '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'freecam_contract.c'),
                        str(ROOT / 'runtime' / 'patches.c'),
                        '-o', str(exe)] + ([] if os.name == 'nt' else ['-lm']), check=True)
        subprocess.run([str(exe)], check=True)
    print('v110 freecam: 5 native version gates and default/off/on settings passed')


def diff_check(work, table, cflags):
    base = 0x7000000000
    word = table.sites['main'][SITE][0][2]
    lift = Lifter(base, 'c')
    exe = build_harness(work, [lift.lift(word, base + SITE).code], 1, cflags)
    mem = bytearray((i * 37 + 19) & 255 for i in range(BUF_SIZE))
    cases, expected = [], []
    for k in range(32):
        x = [0x1234567800000000 + i * 0x101 for i in range(31)]
        x[19] = BUF + 0x1000 + k * 0x100
        v = [(0xfedcba98765432100123456789abcdef + i) for i in range(32)]
        sp, nzcv = BUF + BUF_SIZE - 0x100, (k % 16) << 28
        lo = (base + SITE) & ~0xffff
        code = bytes(base + SITE - lo) + struct.pack('<I', word)
        emu = Emu([(lo, code)])
        emu.mu.mem_write(BUF, bytes(mem))
        ex, esp, enz, epc, ev = emu.run(base + SITE, 0, x, sp, nzcv, v, count=1)
        cases.append((0, 0, x, sp, nzcv, v, True))
        expected.append((ex, esp, enz, epc, ev, zlib.crc32(mem)))
    page = work / 'page.bin'
    page.write_bytes(bytes(65536))
    records, err = run_harness(exe, work, mem, cases, [(table.page & ~0xffff, page)])
    assert not err and len(records) == len(cases), err
    for want, got in zip(expected, records):
        assert not diff_state(got, want, False, True), diff_state(got, want, False, True)
    print('v110 freecam: 32 original-load native/Unicorn cases passed, all GPR/SIMD/SP/NZCV/memory')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exefs', type=Path)
    ap.add_argument('--cflags', default='-march=x86-64-v3')
    args = ap.parse_args()
    mods = layout(args.exefs) if args.exefs else [
        ('main', 0x7000000000, 0x472d000), ('subsdk0', 0x7004800000, 0x400000),
        ('sdk', 0x7004c00000, 0x800000)]
    table = patches.load(ROOT / 'recomp' / 'patches-v110.txt', mods)
    assert table.sets[16] == ('freecam', 'TOTK_FREECAM_PATCH', 0)
    assert len(table.sets) == 17 and sum(map(len, table.sites.values())) == 56
    assert sum(map(len, table.spins.values())) == 889
    branch = table.for_module('main', mods[0][1])[mods[0][1] + SITE][0][1][0]
    call = Lifter(mods[0][1], 'c').lift(branch, mods[0][1] + SITE)
    assert call.kind == 'bl' and call.target == table.page + 0xf50
    if args.exefs:
        local_check(args.exefs, mods, table)
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='v110-freecam-', dir=build) as temp:
        work = Path(temp)
        native_check(work, table, cflags=args.cflags)
        diff_check(work, table, args.cflags)


if __name__ == '__main__':
    main()
