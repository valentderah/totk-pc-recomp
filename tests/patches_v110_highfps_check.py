"""Check the 1.1.0 climb/gear numeric, switch, full-width register and local-stack contracts.

Synthetic checks need no game; --exefs also checks local original words and semantic anchors.
Uses the shared differential harness CC selection, Windows/Linux suffixes and mappings, without fixed host paths.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import BUF, BUF_SIZE, Emu, build_harness, compiler, diff_state, run_harness
import patches
from layout import layout
from lifter import Lifter
from module import Module

NL = chr(10)
CLIMB, GEAR_LOAD, GEAR_SCALE = 0x19279f0, 0x213eee4, 0x213f0d4
COMPARES = (0x213f10c, 0x213f174, 0x213f1b0)
SITES = (CLIMB, GEAR_LOAD, GEAR_SCALE)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return int.from_bytes(struct.pack('<f', x), 'little')


def scalar(x):
    return struct.unpack('<f', (x & 0xffffffff).to_bytes(4, 'little'))[0]


def shipping(r):
    # Round in v110 order: multiply, double, then add the intercept; do not fuse the -0.362 multiplication.
    t = f32(f32(r - 1.0) * f32(-0.181))
    return f32(f32(t + t) + f32(1.272))


def compensation(r, raw):
    u = max(raw, r)
    g = math.inf if u == 0 else 0.66265 / u + 0.4982 + 0.0848 * u
    h = max(min(1.272 + 1.189 * (1 - u), 2), min(g - 16 * max(u - 0.5, 0), 8))
    return h


def extract_function(text, name):
    start = text.index(name + '(')
    start = text.rfind(NL, 0, start) + 1
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def gating_check(work, table):
    # Compile the real patches.c and frame_step_init/step_store; stub only config and platform memory boundaries.
    cfg = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
    for key in ('CLIMB_FIX', 'HIGHFPS_FIX', 'REALTIME'):
        assert '{"' + key + '", "1", "0|1"' in cfg, key
    source = (ROOT / 'runtime' / 'patches.c').read_text(encoding='utf-8')
    source = NL.join(x for x in source.splitlines() if not x.startswith('#include'))
    frame = (ROOT / 'runtime' / 'frame_rate.c').read_text(encoding='utf-8')
    pre = '''#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Alignas(16) unsigned char test_page[4096];
static int real_on, climb_on, high_on;
static const char *cfg_get(const char *e) {
 if (!strcmp(e,"TOTK_REALTIME")) return real_on<0?NULL:real_on?"1":"0";
 if (!strcmp(e,"TOTK_CLIMB_FIX")) return climb_on<0?NULL:climb_on?"1":"0";
 if (!strcmp(e,"TOTK_HIGHFPS_FIX")) return high_on<0?NULL:high_on?"1":"0";
 return NULL;
}
static const void *cfg_meta(const char *e) {
 return !strcmp(e,"TOTK_REALTIME") || !strcmp(e,"TOTK_CLIMB_FIX") || !strcmp(e,"TOTK_HIGHFPS_FIX") ? e : NULL;
}
static int cfg_bool(const char *e) { const char *v=cfg_get(e); return !v || strcmp(v,"0"); }
static int plat_mem_commit(uint64_t a, size_t n) { return a==(uintptr_t)test_page && n==4096; }
static void rt_fatal(const char *s, ...) { (void)s; abort(); }
static void rt_log(const char *s, ...) { (void)s; }
'''
    macro = '#define RT_PATCH_COUNT %d' % len(table.sets) + NL
    macro += '#define RT_PATCH_PAGE ((uintptr_t)test_page)' + NL
    macro += '#define RT_PATCH_SETS(X) ' + ' '.join('X(%d,%s,"%s",%d)' % (i, *s) for i, s in enumerate(table.sets)) + NL
    frame_pre = '''static _Atomic uint64_t *step_word;
static _Atomic uint32_t *step_interval;
static double step_now;
#define STEP_FIRST_S (1.0/60.0)
static void frame_limit_live(void) {}
'''
    main = '''int main(void) {
 for (int i=-1;i<8;i++) {
  real_on=i<0?-1:!!(i&1); climb_on=i<0?-1:!!(i&2); high_on=i<0?-1:!!(i&4);
  memset(test_page,0x5a,sizeof test_page); step_word=NULL; step_interval=NULL;
  rt_patches_init(); frame_step_init();
  assert(rt_patch_enabled(rt_patch_set("climb"))==((real_on!=0)&&(climb_on!=0)));
  assert(rt_patch_enabled(rt_patch_set("gears"))==(high_on!=0));
  assert((step_word!=NULL)==(real_on!=0));
  if (real_on==0) for (size_t j=0;j<sizeof test_page;j++) assert(test_page[j]==0x5a);
  else { float v; memcpy(&v,test_page,4); assert(v==0.5f); }
 }
 puts("v110: 9 native default/realtime/climb/highfps gating combinations passed");
 return 0;
}
'''
    c = work / 'gating.c'
    c.write_text(pre + macro + source + frame_pre + extract_function(frame, 'step_store') + NL +
                 extract_function(frame, 'frame_step_init') + NL + main, encoding='utf-8')
    exe = work / ('gating.exe' if os.name == 'nt' else 'gating')
    subprocess.run([compiler(), '-std=gnu11', '-O1', '-UNDEBUG', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    fixture = work / 'unicode-patch.txt'
    # Chinese fixture data intentionally tests UTF-8 decoding independently of the host encoding.
    fixture.write_text('# 中文注释，不依赖宿主默认代码页' + NL + 'set test TEST 1' + NL, encoding='utf-8')
    assert patches.load(fixture, [('main', 0x7000000000, 4096)]).sets == [('test', 'TEST', 1)]


def local_check(exefs, table, mods):
    for name, base, _ in mods:
        patches.check(table, Module(name, exefs / name, base), name)
    main = Module('main', exefs / 'main', mods[0][1])
    base = mods[0][1]
    # Local semantic anchors: original coefficients, saved d12, vtable target and three matching compares/branches.
    assert main.read(base + 0x3816770, 4) == bits(-0.181)
    assert main.read(base + 0x3816774, 4) == bits(1.272)
    assert main.word(base + CLIMB - 8) == 0x1e202800  # The shipping factor first doubles with fadd s0,s0,s0.
    assert main.word(base + 0x213eec8) == 0xfd002bec  # str d12,[sp,#0x50]
    assert main.read(base + 0x347e9f8, 8) == 0x213eec4
    for off, target in zip(COMPARES, (0x213f148, 0x213f13c, 0x213f13c)):
        assert main.word(base + off) == 0x1e2c2100
        branch = main.word(base + off + 4)
        assert branch & 0xff00001f == 0x5400000d
        assert patches.branch_target(branch, base + off + 4) == base + target
    print('v110: all %d opcode guards, %d spin targets and local highfps anchors passed' %
          (sum(map(len, table.sites.values())), sum(map(len, table.spins.values()))))


def preservation(ex, esp, ev, x, sp, v, allowed_x, allowed_v):
    assert esp == sp and sp % 16 == 0
    assert all(ex[i] == x[i] for i in range(31) if i not in allowed_x), 'GPR preservation'
    assert all(ev[i] == v[i] for i in range(32) if i not in allowed_v), 'Full 128-bit SIMD preservation'


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
    if args.exefs:
        local_check(args.exefs, table, mods)
    sets = {name: (env, default) for name, env, default in table.sets}
    assert sets['climb'] == ('TOTK_CLIMB_FIX', 1)
    assert sets['gears'] == ('TOTK_HIGHFPS_FIX', 1)
    base = mods[0][1]
    resolved = table.for_module('main', base)
    original, replacement = {}, {}
    for off in SITES:
        alternatives = table.sites['main'][off]
        assert len(alternatives) == 1
        original[off] = [alternatives[0][2]]
        replacement[off] = resolved[base + off][0][1]
    specs = [(CLIMB,), (GEAR_LOAD,), (GEAR_SCALE,)] + [(GEAR_LOAD, GEAR_SCALE, off) for off in COMPARES]
    programs, bodies = [], []
    lift = Lifter(base, 'c')
    for spec in specs:
        on, off = [], []
        for address in spec:
            if address == GEAR_SCALE and len(spec) == 3:
                # Model a legal callee clobbering the upper 64 bits of q12; delta must rely only on preserved d12.
                on += [0x4e181d0c]  # mov v12.d[1],x8
                off += [0x4e181d0c]
            if address in COMPARES:
                # A handwritten cset records this b.le condition without copying a game function or skipping its test.
                pair = [0x1e2c2100, 0x1a9fc7f8]
                on += pair
                off += pair
            else:
                on += replacement[address]
                off += original[address]
        def lower(words):
            ops = [lift.lift(w, base + spec[0]) for w in words]
            assert all(i.kind == 'op' for i in ops)
            return ' '.join(i.code for i in ops)
        bodies.append('if (c->x[30]) { ' + lower(on) + ' } else { ' + lower(off) + ' }')
        programs.append((off, on))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='v110-highfps-', dir=args.work_dir) as temp:
        work = Path(temp)
        gating_check(work, table)
        exe = build_harness(work, bodies, 2, args.cflags)
        total = 0
        raw_steps = (0.0, 1e-8, 0.125, 0.25, f32(0.5 - 2**-24), 0.5, f32(0.5 + 2**-23), 1.0, 2.0, 16.0)
        for raw in raw_steps:
            raw = f32(raw)
            mem = bytearray((i * 37 + 19) & 255 for i in range(BUF_SIZE))
            struct.pack_into('<Q', mem, 0x2040, BUF + 0x4000)
            struct.pack_into('<f', mem, 0x3008, raw)
            page = bytearray(4096)
            struct.pack_into('<f', page, 0, raw)
            cases, expected = [], []
            for k, spec in enumerate(specs):
                for enabled in (0, 1):
                    for r in (0.0, 1e-8, 0.125, 0.25, 0.5, 0.75, 1.0, 2.0, 16.0):
                        r = f32(r)
                        for edge in (-1, 0, 1):
                            # x30 selects the test path; other GPRs and vector upper bits have distinct sentinels.
                            x = [0x1234567800000000 + i * 0x101 for i in range(31)]
                            x[0], x[1], x[30] = BUF + 0x2000, BUF + 0x3000, enabled
                            v = [((0xfedcba987654321001234567 + i) << 32) | bits(3.0 + i) for i in range(32)]
                            for reg, value in ((0, shipping(r)), (4, 30.0), (9, r), (12, raw)):
                                v[reg] = (v[reg] & ~0xffffffff) | bits(value)
                            assert all(v[reg] >> 64 for reg in (4, 8, 9)), 'Upper 64-bit SIMD sentinels must be nonzero'
                            threshold = f32(f32(shipping(r) / 30) * raw) if enabled else f32(shipping(r) / 30)
                            # Adjacent ULPs exercise greater-than, equal and less-than at all three branch thresholds.
                            tb = bits(threshold)
                            v[8] = (v[8] & ~0xffffffff) | (bits(0.5) if k == 0 else max(0, tb + edge))
                            sp, nzcv = BUF + BUF_SIZE - 0x100, ((total + len(cases)) % 16) << 28
                            code = programs[k][enabled]
                            pc = base + spec[0]
                            lo = pc & ~0xffff
                            text = bytearray(pc - lo + 4 * len(code))
                            struct.pack_into('<' + 'I' * len(code), text, pc - lo, *code)
                            emu = Emu([(lo, text), (table.page, page)])
                            emu.mu.mem_write(BUF, bytes(mem))
                            ex, esp, enz, epc, ev = emu.run(pc, 0, x, sp, nzcv, v, count=len(code))
                            allowed_x = {1} if GEAR_LOAD in spec else set()
                            if len(spec) == 3:
                                allowed_x |= {24}
                            allowed_v = {0, 1} if k == 0 else {12}
                            preservation(ex, esp, ev, x, sp, v, allowed_x, allowed_v)
                            if len(spec) != 3:
                                assert enz == nzcv
                            touched = []
                            if enabled and k == 0:
                                touched = [(sp - 96, x[8].to_bytes(8, 'little'))]
                                touched += [(sp - 80 + i * 16, v[reg].to_bytes(16, 'little'))
                                            for i, reg in enumerate((5, 6, 7, 16, 17))]
                                h = compensation(r, raw)
                                for reg, want in ((0, max(shipping(r), h)), (1, max(1.5, h))):
                                    assert math.isclose(scalar(ev[reg]), want, rel_tol=2e-6, abs_tol=2e-7), (reg, r, raw, scalar(ev[reg]), want)
                                    assert ev[reg] >> 32 == 0
                                if r >= 1 and raw <= r:
                                    assert ev[0] & 0xffffffff == v[0] & 0xffffffff and ev[1] == bits(1.5), 'Shipping factor bits unchanged at 30 Hz and slower steps'
                            if not enabled and k == 0:
                                assert ev[0] == v[0] and ev[1] == bits(1.5)
                            if enabled and GEAR_SCALE in spec:
                                touched = [(sp - 16, v[4].to_bytes(16, 'little'))]
                                want = f32(f32(scalar(v[0]) / scalar(v[4])) * raw)
                                assert ev[12] == bits(want)
                            if GEAR_LOAD in spec:
                                assert ex[1] == BUF + 0x4000
                            if k == 1 and enabled:
                                assert ev[12] == bits(raw)
                            if len(spec) == 3:
                                angle, limit = scalar(v[8]), scalar(ev[12])
                                # ARM64 b.le also accepts unordered (N != V), unlike Python NaN <= comparisons.
                                want_branch = math.isnan(angle) or math.isnan(limit) or angle <= limit
                                assert ex[24] == want_branch, (spec, enabled, raw, r, edge, ex[24], angle, limit, hex(enz))
                            want_mem = bytearray(mem)
                            for address, data in touched:
                                at = address - BUF
                                want_mem[at:at + len(data)] = data
                            actual_mem = bytes(emu.mu.mem_read(BUF, BUF_SIZE))
                            assert actual_mem == want_mem, 'Only exact save slots may change; all other memory, gaps and canaries must stay intact'
                            assert actual_mem[sp - BUF - 112:sp - BUF - 96] == mem[sp - BUF - 112:sp - BUF - 96]
                            assert actual_mem[sp - BUF:sp - BUF + 16] == mem[sp - BUF:sp - BUF + 16]
                            cases.append((k, 0, x, sp, nzcv, v, True))
                            expected.append((ex, esp, enz, epc, ev, zlib.crc32(actual_mem)))
            page_path = work / 'page.bin'
            page_base = table.page & ~0xffff
            page_path.write_bytes(bytes(table.page - page_base) + page)
            records, err = run_harness(exe, work, mem, cases, [(page_base, page_path)])
            assert not err and len(records) == len(cases), err
            for case, want, got in zip(cases, expected, records):
                delta = diff_state(got, want, False, True)
                assert not delta, (specs[case[0]], delta)
            total += len(cases)
        print(f'v110: {total} native/Unicorn cases passed; 6 contracts, on/off, 16 NZCV, full GPR/SIMD/SP/memory')
    return 0


if __name__ == '__main__':
    sys.exit(main())
