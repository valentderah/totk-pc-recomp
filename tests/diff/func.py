"""Differential test of whole emitted functions: recomp/gen.py's C for a guest function against Unicorn running the
same function from its entry until it returns.

Picks functions from the ExeFS modules: no branches out of the function, no SVC, BRK, system register, exclusive or
atomic instructions; with conditional branches, and loops, jump tables, SIMD and calls where the module has them.
Emits them with the generator's own Emitter, compiles them with tests/diff/harness.c, and runs each from random
states: x0-x7 point into the scratch buffer (or hold small integers), the buffer holds pointers into itself and random
data, SP sits near the buffer's top and LR is a sentinel address. Unicorn (Cortex-A57) stops when PC reaches the
sentinel. A case is skipped when Unicorn faults, writes outside the scratch buffer (module images are mapped
read-only), leaves the function, or runs longer than --steps instructions. Compares X0-X30, SP, V0-V31 and the
scratch buffer; NZCV is local to a generated function (recomp/flags.py).

Every BL and BLR goes to one stub on both sides (rt_stub in harness.c, Runner.stub here) instead of its callee. The
stub folds the guest state at the call into a digest and sets the caller-saved registers from it, so the test sees
whether the emitted code had every register in the Cpu at the call and reads them back after it. Callers of another
picked function are dropped, as the C side would run that function instead of the stub.

With --patches, the spin waits of the table (recomp/patches.py `spin`) are emitted with their gates, and functions
that hold one come first (a quarter of the picks at most); a case whose loop does not leave runs into --steps.

Usage: func.py [--exefs DIR] [--modules main,subsdk0,sdk] [--functions N] [--cases K] [--seed S] [--jobs J]
               [--patches FILE]
"""
import argparse
import os
import platform
import random
import subprocess
import sys
import time
import zlib
from collections import Counter
from pathlib import Path

import numpy as np
import unicorn as uc
from unicorn import arm64_const as A

from common import BUF, BUF_SIZE, MSYS_BIN, ROOT, Emu, compiler, diff_state, rand_fpcr, rand_v, rand_x, run_harness
import gen
import patches
from layout import layout
from module import Module

SENTINEL = 0x7210000000  # LR on entry; never executed
PTR_LO, PTR_HI = BUF + 0x8000, BUF + 0x28000
STACK_TOP = BUF + 0x3F000
MIN_INSNS, MAX_INSNS = 6, 600


FNV = 0x100000001B3
M64 = (1 << 64) - 1


def splitmix(z):
    z = (z + 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


def call_mask(w):
    return ((w & 0xFC000000) == 0x94000000) | ((w & 0xFFFFFC1F) == 0xD63F0000)   # BL, BLR


def forbidden_mask(w):
    """Words a picked function must not contain."""
    m = (w & 0xFF000000) == 0xD4000000                    # SVC, HVC, SMC, BRK, HLT, DCPS
    sysi = (w & 0xFFC00000) == 0xD5000000                 # system: MRS, MSR, SYS (DC ZVA), hints, barriers
    hint_or_barrier = ((w & 0xFFFFF000) == 0xD5032000) | ((w & 0xFFFFF000) == 0xD5033000)
    m |= sysi & ~hint_or_barrier
    m |= (w & 0x3F000000) == 0x08000000                   # exclusives, load-acquire / store-release, CAS
    m |= (w & 0x3F200C00) == 0x38200000                   # LSE atomics
    return m


def cond_mask(w):
    return (((w & 0xFE000010) == 0x54000000) | ((w & 0x7E000000) == 0x34000000) |
            ((w & 0x7E000000) == 0x36000000))


def simd_mask(w):
    return ((w & 0x0E000000) == 0x0E000000) | (((w & 0x0A000000) == 0x08000000) & (((w >> 26) & 1) == 1))


def pick_functions(name, mod, count, r, spins=None):
    """[(entry, end, tags)] of up to count functions of mod, plus the Emitter that will emit them. spins: {spin wait
    back-edge: head}, emitted with gates."""
    starts, _ = gen.discover(mod)
    w = mod.words
    ends = np.append(starts[1:], len(w))

    def per_fn(mask):
        cs = np.concatenate([[0], np.cumsum(mask.astype(np.int64))])
        return cs[ends] - cs[starts]

    size = ends - starts
    ok = (per_fn(forbidden_mask(w)) == 0) & (per_fn(cond_mask(w)) > 0) & (size >= MIN_INSNS) & (size <= MAX_INSNS)
    has_br = per_fn((w & 0xFFFFFC1F) == 0xD61F0000) > 0
    has_call = per_fn(call_mask(w)) > 0
    cand = np.flatnonzero(ok)
    with_br = [int(i) for i in cand if has_br[i]]
    with_call = [int(i) for i in cand if has_call[i] and not has_br[i]]
    rest = [int(i) for i in cand if not has_br[i] and not has_call[i]]
    spins = spins or {}
    gated = np.searchsorted(mod.lo + starts * 4, np.array(sorted(spins), dtype=np.int64), side='right') - 1
    with_spin = sorted({int(i) for i in gated} & set(cand.tolist()))
    for pool in (with_spin, with_br, with_call, rest):
        r.shuffle(pool)
    emitter = gen.Emitter(mod, set((mod.lo + starts * 4).tolist()), spins=spins)
    picks, tags_seen = [], Counter()
    # Functions with a spin wait first, then with a br (jump tables, fallbacks), at most a quarter of the picks each;
    # then functions with calls, at most half.
    for pool, quota in ((with_spin, count // 4), (with_br, count // 4), (with_call, count // 2), (rest, count)):
        taken = 0
        for i in pool:
            if len(picks) >= count or taken >= quota:
                break
            entry, end = mod.lo + int(starts[i]) * 4, mod.lo + int(ends[i]) * 4
            if any(e == entry for e, _, _ in picks):
                continue
            tags = check_function(emitter, entry, end)
            if tags is None:
                continue
            if any(spins.get(x) is not None for x in range(entry, end, 4)):
                tags.add('spin_gate')
            picks.append((entry, end, tags))
            taken += 1
    picked = {e for e, _, _ in picks}
    picks = [p for p in picks if not callees(emitter, p[0], p[1]) & picked]
    for _, _, tags in picks:
        tags_seen.update(tags)
    print(f'{name}: {len(starts)} functions, {len(cand)} candidates, picked {len(picks)}: '
          f'{dict(tags_seen)}', flush=True)
    return picks, emitter


def call_sites(mod, entry, end):
    """Addresses of the BL and BLR instructions of [entry, end)."""
    return [a for a in range(entry, end, 4) if mod.lift(a).kind in ('bl', 'blr')]


def callees(emitter, entry, end):
    """Function starts the emitted C of [entry, end) calls directly (`f_<addr>(c)`, not through rt_dispatch)."""
    mod = emitter.mod
    return {mod.lift(a).target for a in call_sites(mod, entry, end)
            if mod.lift(a).kind == 'bl' and mod.lift(a).target in emitter.starts}


def check_function(emitter, entry, end):
    """Tags of a function the test can run in isolation, or None."""
    mod = emitter.mod
    insns = [mod.lift(a) for a in range(entry, end, 4)]
    if insns[-1].kind not in gen.TERMINATORS or not any(i.kind == 'ret' for i in insns):
        return None
    tags = set()
    for k, ins in enumerate(insns):
        a = entry + 4 * k
        if ins.kind in ('bl', 'blr'):
            tags.add('calls')
        if ins.kind in ('b', 'bcond'):
            if not entry <= ins.target < end:
                return None
            if ins.target <= a:
                tags.add('loop')
        if ins.kind == 'br':
            jt = gen.jumptables.resolve(mod, a, entry, end)
            if jt:
                tags.add('jump_table')
            elif gen.jumptables.is_computed(mod, a, entry):
                tags.add('all_label_fallback')
            else:
                return None
    if simd_mask(mod.words[(entry - mod.lo) >> 2:(end - mod.lo) >> 2]).any():
        tags.add('simd')
    return tags


def build(work, emitted, jobs, cflags):
    """Compile the emitted functions with harness.c into work/func[.exe]; tests[k] is emitted[k]."""
    cc = compiler()
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    flags = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-I', str(ROOT / 'runtime')]
    flags += cflags.split()
    groups = {}
    for name, entry, end, emitter in emitted:
        groups.setdefault(name, []).append((entry, end))
    srcs, n = [], max(1, jobs)
    for name, fns in groups.items():
        emitter = next(e for m, _, _, e in emitted if m == name)
        per = (len(fns) + n - 1) // n
        for j in range(0, len(fns), per):
            path = work / f'{name}_{j // per:03d}.c'
            path.write_text(gen.chunk_text(name, j // per, emitter, fns[j:j + per]))
            srcs.append(path)
    table = work / 'table.c'
    called = set()
    for _, entry, end, emitter in emitted:
        called |= callees(emitter, entry, end)
    table.write_text('#include "cpu.h"\n' + ''.join(f'void {gen.fname(e)}(Cpu *c);\n' for _, e, _, _ in emitted) +
                     'GuestFn tests[] = {' + ','.join(gen.fname(e) for _, e, _, _ in emitted) + '};\n' +
                     f'int ntests = {len(emitted)};\n' + 'void rt_stub_call(Cpu *c, uint64_t target);\n' +
                     ''.join(f'void {gen.fname(t)}(Cpu *c) {{ rt_stub_call(c, {hex(t)}ull); }}\n'
                             for t in sorted(called)))
    # fma.c: a --cflags=-march=x86-64-v2 run must exercise the software fma the v2 build links, not the C runtime's.
    srcs += [table, ROOT / 'tests' / 'diff' / 'harness.c', ROOT / 'runtime' / 'fma.c']
    objs, procs = [], []
    for s in srcs:
        o = work / (s.stem + '.o')
        objs.append(o)
        procs.append(subprocess.Popen([cc] + flags + ['-c', str(s), '-o', str(o)], env=env))
    if any(p.wait() for p in procs):
        raise SystemExit('compile failed')
    exe = work / ('func.exe' if os.name == 'nt' else 'func')
    subprocess.check_call([cc, '-fuse-ld=lld', '-o', str(exe)] + [str(o) for o in objs], env=env)
    return exe


def make_buffer(r):
    out = bytearray()
    for _ in range(BUF_SIZE // 8):
        k = r.random()
        if k < 0.4:
            v = r.randrange(PTR_LO, PTR_HI, 8)
        elif k < 0.7:
            v = r.randrange(0, 256)
        else:
            v = r.getrandbits(64)
        out += v.to_bytes(8, 'little')
    return bytes(out)


def make_case(r):
    x = [rand_x(r) for _ in range(31)]
    for i in range(8):
        k = r.random()
        if k < 0.7:
            x[i] = r.randrange(PTR_LO, PTR_HI, 16)
        elif k < 0.9:
            x[i] = r.randrange(0, 64)
    x[30] = SENTINEL
    sp = STACK_TOP - r.randrange(0, 0x100, 16)
    return rand_fpcr(r), x, sp, r.getrandbits(4) << 28, [rand_v(r) for _ in range(32)]


class Runner:
    """Unicorn with the module images read-only, a block hook that stops when execution leaves the function, and a
    code hook on every call site of the picked functions that stops before the call, for the stub."""

    MAX_CALLS = 1000

    def __init__(self, images, buf0, sites):
        self.emu = Emu(images)
        mu = self.emu.mu
        for base, data in images:
            mu.mem_protect(base, (len(data) + 0xFFFF) & ~0xFFFF, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        mu.mem_map(SENTINEL, 0x1000)
        mu.mem_write(BUF, buf0)
        self.buf0 = buf0
        self.lo = self.hi = self.h = 0
        self.left = False
        self.at_call = None
        mu.hook_add(uc.UC_HOOK_BLOCK, self._block)
        # One hook per call site, all added before anything is translated, so no cached block lacks one.
        for a in sorted(sites):
            mu.hook_add(uc.UC_HOOK_CODE, self._call, begin=a, end=a)

    def _block(self, mu, address, size, user):
        if not self.lo <= address < self.hi and address != SENTINEL:
            self.left = True
            mu.emu_stop()

    def _call(self, mu, address, size, user):
        if self.lo <= address < self.hi:
            self.at_call = address
            mu.emu_stop()

    def stub(self, a):
        """harness.c rt_stub for the call at a, which Unicorn has not executed yet. Returns the address to resume at."""
        e, mu = self.emu, self.emu.mu
        w = int.from_bytes(mu.mem_read(a, 4), 'little')
        if (w & 0xFC000000) == 0x94000000:   # BL
            imm = w & 0x03FFFFFF
            target = (a + ((imm - (1 << 26) if imm >> 25 else imm) << 2)) & M64
        else:                                 # BLR
            target = mu.reg_read(e.xregs[(w >> 5) & 31])
        mu.reg_write(A.UC_ARM64_REG_X30, a + 4)
        h = ((self.h ^ target) * FNV) & M64
        for val in list(mu.reg_read_batch(e.xregs)) + [mu.reg_read(A.UC_ARM64_REG_SP)]:
            h = ((h ^ val) * FNV) & M64
        for val in mu.reg_read_batch(e.qregs[:8]):
            h = ((h ^ (val & M64)) * FNV) & M64
            h = ((h ^ (val >> 64)) * FNV) & M64
        self.h = h
        regs = []
        for i in range(19):
            s = splitmix((h + i) & M64)
            regs.append((e.xregs[i], PTR_LO + ((s >> 4) % ((PTR_HI - PTR_LO) // 16)) * 16 if i < 8 and s & 3 else s))
        regs.append((A.UC_ARM64_REG_NZCV, splitmix((h + 19) & M64) >> 60 << 28))
        for i in range(8):
            regs.append((e.qregs[i], splitmix((h + 33 + 2 * i) & M64) << 64 | splitmix((h + 32 + 2 * i) & M64)))
        mu.reg_write_batch(regs)
        top = BUF + BUF_SIZE - 16
        n = int.from_bytes(mu.mem_read(top, 8), 'little')
        mu.mem_write(top, ((n + 1) & M64).to_bytes(8, 'little') + h.to_bytes(8, 'little'))
        return a + 4

    def run(self, entry, end, case, steps):
        """(x, sp, nzcv, pc, v, crc of the buffer) or None if the case is skipped."""
        fpcr, x, sp, nzcv, v = case
        e, mu = self.emu, self.emu.mu
        self.lo, self.hi, self.left, self.h = entry, end, False, 0
        mu.reg_write_batch(list(zip(e.xregs, x[:31])) + list(zip(e.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        pc, ok = entry, False
        try:
            for _ in range(self.MAX_CALLS):
                self.at_call = None
                mu.emu_start(pc, SENTINEL, count=steps)
                if self.at_call is None:
                    ok = not self.left and mu.reg_read(A.UC_ARM64_REG_PC) == SENTINEL
                    break
                pc = self.stub(self.at_call)
        except uc.UcError:
            ok = False
        if not ok:
            mu.mem_write(BUF, self.buf0)
            return None
        o = mu.reg_read_batch(e.out_regs)
        crc = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
        mu.mem_write(BUF, self.buf0)
        return list(o[:31]), o[31], o[32] & 0xF0000000, o[33], list(o[34:]), crc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, default=ROOT / 'build' / 'extracted' / 'v121' / 'exefs')
    ap.add_argument('--modules', default='main,subsdk0,sdk')
    ap.add_argument('--functions', type=int, default=400, help='per module')
    ap.add_argument('--cases', type=int, default=12, help='random states tried per function')
    ap.add_argument('--steps', type=int, default=200000, help='Unicorn instruction cap per case')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--show', type=int, default=20)
    ap.add_argument('--patches', type=Path, help='patch table whose spin waits are emitted with their gates')
    ap.add_argument('--cflags', default='-march=x86-64-v3' if platform.machine().lower() in ('amd64', 'x86_64') else '')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    want = args.modules.split(',')

    work = ROOT / 'build' / 'test' / 'func'
    work.mkdir(parents=True, exist_ok=True)
    images, emitted, tags = [], [], []
    mods = layout(args.exefs)
    table = patches.load(args.patches, mods) if args.patches else patches.Table()
    for name, base, _ in mods:
        if name not in want:
            continue
        mod = Module(name, args.exefs / name, base, 'c')
        img = work / f'{name}.img'
        img.write_bytes(mod.image)
        images.append((base, img, mod.image))
        picks, emitter = pick_functions(name, mod, args.functions, r, table.spins_for(name, base))
        for entry, end, t in picks:
            emitted.append((name, entry, end, emitter))
            tags.append(t)
    print(f'picked {len(emitted)} functions ({time.time() - t0:.0f}s)', flush=True)

    t1 = time.time()
    exe = build(work, emitted, args.jobs, args.cflags)
    print(f'compiled ({time.time() - t1:.0f}s)', flush=True)

    buf0 = make_buffer(r)
    sites = set()
    for _, entry, end, emitter in emitted:
        sites.update(call_sites(emitter.mod, entry, end))
    runner = Runner([(base, data) for base, _, data in images], buf0, sites)
    cases, expect, tested, skipped = [], [], set(), 0
    t2 = time.time()
    for k, (name, entry, end, _) in enumerate(emitted):
        for _ in range(args.cases):
            case = make_case(r)
            want_state = runner.run(entry, end, case, args.steps)
            if want_state is None:
                skipped += 1
                continue
            fpcr, x, sp, nzcv, v = case
            cases.append((k, fpcr, x, sp, nzcv, v, True))
            expect.append(want_state)
            tested.add(k)
    print(f'unicorn: {len(cases)} cases on {len(tested)} functions, {skipped} skipped ({time.time() - t2:.0f}s)',
          flush=True)

    t3 = time.time()
    recs, err = run_harness(exe, work, buf0, cases, [(base, img) for base, img, _ in images], stubs=True)
    print(f'harness: {len(recs)} of {len(cases)} cases ({time.time() - t3:.0f}s)')
    if err:
        k = cases[len(recs)][0] if len(recs) < len(cases) else None
        where = f', stopped in {emitted[k][0]} {emitted[k][1]:#x}' if k is not None else ''
        print(f'harness failed: {err}{where}')

    fails, nfail = {}, 0
    for case, want_state, got in zip(cases, expect, recs):
        # Generated functions keep NZCV in a local (recomp/flags.py) and leave c->nzcv as it was: flags only count
        # where the function reads them, which the registers and memory show.
        d = diff_state(got, want_state, check_pc=False, check_mem=True, check_nzcv=False)
        if d:
            nfail += 1
            fails.setdefault(case[0], d[:4])
    by_tag, fail_tag = Counter(), Counter()
    for k in tested:
        by_tag.update(tags[k] or {'plain'})
        if k in fails:
            fail_tag.update(tags[k] or {'plain'})
    print(f'{len(tested)} functions, {len(cases)} cases: {nfail} failing cases, {len(fails)} failing functions '
          f'({time.time() - t0:.0f}s total)')
    print(f'  tested by tag: {dict(by_tag)}; failing by tag: {dict(fail_tag)}')
    for k, d in list(fails.items())[:args.show]:
        name, entry, end, _ = emitted[k]
        print(f'  {name:8} {gen.fname(entry)} ({(end - entry) // 4} insns, {sorted(tags[k])}): {"; ".join(d)}')
    return 1 if fails or err else 0


if __name__ == '__main__':
    sys.exit(main())
