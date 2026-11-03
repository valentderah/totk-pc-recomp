"""Shared parts of the differential tests: Unicorn reference, operand generators, build and run of harness.c.

Adapted from LAS Recomp tools/test_lift.py and tools/test_simd.py @ da35b83 (MIT, Copyright (c) 2026 Peiikai).
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import capstone
import unicorn as uc
from unicorn import arm64_const as A

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'recomp'))

BUF, BUF_SIZE = 0x7200000000, 0x40000
UC_INVALID = (uc.UC_ERR_INSN_INVALID, uc.UC_ERR_EXCEPTION)

# Record layouts of struct In / struct Out in harness.c.
IN_FMT = '<QQ32QQQ'
IN_TAIL = '<QQ'
OUT_SIZE = 32 * 8 + 4 * 8 + 32 * 16

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

F32 = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00001, 0xff812345, 0x7fd00abc,
       0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x7f7fffff,
       0xff7fffff, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x40000000, 0x40400000, 0x3fc00000, 0xbfc00000,
       0x40200000, 0x40100000, 0x4f000000, 0xcf000000, 0x4f800000, 0x5f000000, 0xdf000000, 0x5f800000, 0x4effffff,
       0x4f7fffff, 0x5effffff, 0x5f7fffff, 0x3effffff, 0x3f7fffff, 0x7effffff, 0x01000000, 0x3e800000, 0x7e800000,
       0x7e7fffff, 0x00200000, 0x001fffff, 0x00200001, 0x47800000, 0x477fe000, 0x477ff000, 0x38800000, 0x33800000,
       0x33000000, 0x387fc000, 0x3f7ff000, 0x46fffe00, 0x80000000 | 0x00200000, 0x7f000000, 0x7f7ffffe]
F64 = [0, 1 << 63, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
       0x7ff4000000000001, 0xfff0000000012345, 0x7ffc00000abcdef0, 1, (1 << 63) | 1, 0x000fffffffffffff,
       0x0008000000000000, 0x0010000000000000, 0x8010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
       0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0x4000000000000000, 0x4008000000000000,
       0x3ff8000000000000, 0xbff8000000000000, 0x4004000000000000, 0x41e0000000000000, 0xc1e0000000000000,
       0x41f0000000000000, 0x43e0000000000000, 0xc3e0000000000000, 0x43f0000000000000, 0x41dfffffffc00000,
       0x41efffffffe00000, 0x43dfffffffffffff, 0x43efffffffffffff, 0x3fdfffffffffffff, 0x7fdfffffffffffff,
       0x7fd0000000000000, 0x0004000000000000, 0x0003ffffffffffff, 0x47efffffe0000000, 0x47efffff00000000,
       0x36a0000000000000, 0x3810000000000000, 0x380fffffffffffff, 0x47f0000000000000, 0x3e70000000000000,
       0x3f0ffc0000000000, 0x40effc0000000000, 0x40effe0000000000, 0x3e6ffffffff00000, 0x369fffffffffffff,
       0x3810000010000000, 0x381000000fffffff, 0x3e80000000000000]
F16 = [0, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7d00, 0x7e55, 0x0001, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff,
       0x3c00, 0xbc00, 0x3800, 0x4000, 0x3e00, 0x7fff, 0xffff]

_md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def dis(w, pc=0):
    d = next(_md.disasm(struct.pack('<I', w), pc), None)
    return f'{d.mnemonic} {d.op_str}' if d else '?'


def mnemonic(w):
    return dis(w).split(' ')[0]


def rand_lane(r, sz):
    """Random interesting integer lane of 8 << sz bits."""
    bits_ = 8 << sz
    k = r.random()
    if k < 0.35:
        return r.choice([0, 1, 2, 3, (1 << bits_) - 1, (1 << bits_) - 2, 1 << (bits_ - 1), (1 << (bits_ - 1)) - 1,
                         (1 << (bits_ - 1)) + 1, 0x80 % (1 << bits_), 0x7f, bits_ - 1, bits_, bits_ + 1,
                         (1 << bits_) - bits_, 1 << (bits_ // 2)])
    if k < 0.5:
        return r.getrandbits(r.randint(1, bits_))
    if k < 0.6:
        return ((1 << bits_) - r.getrandbits(r.randint(1, bits_ - 1))) & ((1 << bits_) - 1)
    return r.getrandbits(bits_)


def rand_f32(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F32)
    if k < 0.8:
        e = r.choice([r.randint(110, 145), r.randint(0, 255), r.randint(120, 135)])
        return (r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(23) if r.random() < 0.7 else
                                                        r.getrandbits(4) << 19)
    return struct.unpack('<I', struct.pack('<f', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           float(r.randint(-5, 5)), r.uniform(-1e6, 1e6)])))[0]


def rand_f64(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F64)
    if k < 0.8:
        e = r.choice([r.randint(1000, 1050), r.randint(0, 2047), r.randint(1020, 1030), r.randint(0, 40),
                      r.randint(2000, 2047), r.randint(860, 920)])
        return (r.getrandbits(1) << 63) | (e << 52) | (r.getrandbits(52) if r.random() < 0.7 else
                                                        r.getrandbits(6) << 46)
    return struct.unpack('<Q', struct.pack('<d', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           r.uniform(-1e12, 1e12)])))[0]


def rand_v(r):
    k = r.random()
    out = 0
    if k < 0.3:
        for i in range(4):
            out |= rand_f32(r) << (32 * i)
    elif k < 0.55:
        for i in range(2):
            out |= rand_f64(r) << (64 * i)
    elif k < 0.6:
        for i in range(8):
            out |= (r.choice(F16) if r.random() < 0.5 else r.getrandbits(16)) << (16 * i)
    elif k < 0.9:
        sz = r.randrange(4)
        for i in range(16 >> sz):
            out |= rand_lane(r, sz) << ((8 << sz) * i)
    else:
        out = r.getrandbits(128)
    if r.random() < 0.15:  # upper half zero, like most 64-bit results
        out &= (1 << 64) - 1
    return out


def rand_x(r):
    k = r.random()
    if k < 0.3:
        return r.choice([0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF,
                         0x7FFFFFFF, 0x80000000, 0x100000000, 0xFFFFFFFF80000000, 0x8000000000000001, 0xFFFFFF,
                         0x1000001, 0x20000000000001, 0xFFFFFFFFFF000001, 63, 64, 31, 32, 0xFF, 0xFFFF])
    if k < 0.45:
        return rand_f32(r) | (r.getrandbits(32) << 32 if r.random() < 0.3 else 0)
    if k < 0.6:
        return rand_f64(r)
    if k < 0.75:
        return r.getrandbits(r.randint(1, 64))
    if k < 0.8:
        return (-r.getrandbits(r.randint(1, 63))) & 0xFFFFFFFFFFFFFFFF
    return r.getrandbits(64)


def rand_fpcr(r):
    k = r.random()
    if k < 0.55:
        return 0
    v = 0
    if r.random() < 0.4:
        v |= 1 << 25  # DN
    if r.random() < 0.4:
        v |= 1 << 24  # FZ
    if r.random() < 0.4:
        v |= r.randrange(4) << 22  # RMode
    if r.random() < 0.15:
        v |= 1 << 26  # AHP
    return v


class Emu:
    """Unicorn as a Cortex-A57, the Switch's CPU, unless another model is given (LSE atomics need
    UC_CPU_ARM64_MAX). Images are (base, bytes) pairs mapped at their guest address."""

    def __init__(self, images, model=A.UC_CPU_ARM64_A57):
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.ctl_set_cpu_model(model)
        mu.mem_map(BUF, BUF_SIZE)
        for base, data in images:
            mu.mem_map(base, (len(data) + 0xFFFF) & ~0xFFFF)
            mu.mem_write(base, bytes(data))
        mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)  # enable FP/SIMD
        self.mu = mu
        self.xregs = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
        self.qregs = [A.UC_ARM64_REG_Q0 + i for i in range(32)]
        self.out_regs = self.xregs + [A.UC_ARM64_REG_SP, A.UC_ARM64_REG_NZCV, A.UC_ARM64_REG_PC] + self.qregs

    def run(self, pc, fpcr, x, sp, nzcv, v, count=1):
        """Run count instructions from pc. Returns (x[31], sp, nzcv, pc, v[32])."""
        mu = self.mu
        mu.reg_write_batch(list(zip(self.xregs, x[:31])) + list(zip(self.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        mu.emu_start(pc, pc + 4 * count + 4, count=count)
        o = mu.reg_read_batch(self.out_regs)
        return list(o[:31]), o[31], o[32] & 0xF0000000, o[33], list(o[34:])


def compiler():
    cc = os.environ.get('CC')
    if cc:
        return cc
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def build_harness(work, bodies, jobs, cflags):
    """Compile one C function per test body plus harness.c into work/harness[.exe]."""
    cc = compiler()
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    flags = ['-O1', '-fno-strict-aliasing', '-Wno-unused-label', '-I', str(ROOT / 'runtime')] + cflags.split()
    n = max(1, min(jobs, len(bodies) // 200 + 1))
    per = (len(bodies) + n - 1) // n
    srcs = []
    for j in range(n):
        path = work / f'tests_{j}.c'
        with open(path, 'w') as f:
            f.write('#include "cpu.h"\n')
            for k in range(j * per, min(len(bodies), (j + 1) * per)):
                f.write(f'void t{k}(Cpu *c) {{ {bodies[k]} }}\n')
        srcs.append(path)
    with open(work / 'table.c', 'w') as f:
        f.write('#include "cpu.h"\n')
        f.write(''.join(f'void t{k}(Cpu *c);\n' for k in range(len(bodies))))
        f.write('GuestFn tests[] = {' + ','.join(f't{k}' for k in range(len(bodies))) + '};\n')
        f.write(f'int ntests = {len(bodies)};\n')
    # fma.c: a --cflags=-march=x86-64-v2 run must exercise the software fma the v2 build links, not the C runtime's.
    srcs += [work / 'table.c', ROOT / 'tests' / 'diff' / 'harness.c', ROOT / 'runtime' / 'fma.c']
    objs, procs = [], []
    for s in srcs:
        o = work / (s.stem + '.o')
        objs.append(o)
        procs.append(subprocess.Popen([cc] + flags + ['-c', str(s), '-o', str(o)], env=env))
    if any(p.wait() for p in procs):
        raise SystemExit('compile failed')
    exe = work / ('harness.exe' if os.name == 'nt' else 'harness')
    subprocess.check_call([cc, '-fuse-ld=lld', '-o', str(exe)] + [str(o) for o in objs], env=env)
    return exe


def write_cases(path, cases, stubs=False):
    """cases: (k, fpcr, x[31], sp, nzcv, v[32], memchk). stubs: calls go to harness.c's call stub (rt_stub)."""
    with open(path, 'wb') as f:
        for k, fpcr, x, sp, nzcv, v, memchk in cases:
            f.write(struct.pack(IN_FMT, k, fpcr, *(list(x[:31]) + [0]), sp, nzcv))
            for q in v:
                f.write(q.to_bytes(16, 'little'))
            f.write(struct.pack(IN_TAIL, int(memchk), int(stubs)))


def run_harness(exe, work, buf0, cases, images, stubs=False):
    """Run the lifted tests. Returns (records, stderr tail or None). images: (base, path) pairs."""
    with open(work / 'buf.bin', 'wb') as f:
        f.write(buf0)
    write_cases(work / 'cases.bin', cases, stubs)
    out = work / 'out.bin'
    args = [str(exe), str(work / 'buf.bin'), str(work / 'cases.bin'), str(out)]
    for base, path in images:
        args += [str(path), f'{base:x}']
    p = subprocess.run(args, capture_output=True)
    err = None if p.returncode == 0 else f'exit {p.returncode:#x}: {p.stderr.decode(errors="replace")[-2000:]}'
    data = out.read_bytes() if out.exists() else b''
    recs = []
    for i in range(len(data) // OUT_SIZE):
        rec = data[i * OUT_SIZE:(i + 1) * OUT_SIZE]
        x = struct.unpack_from('<31Q', rec, 0)
        sp, nzcv, pc, mem = struct.unpack_from('<4Q', rec, 256)
        v = [int.from_bytes(rec[288 + 16 * j:304 + 16 * j], 'little') for j in range(32)]
        recs.append((x, sp, nzcv, pc, mem, v))
    return recs, err


def diff_state(got, want, check_pc, check_mem, check_nzcv=True):
    """got: harness record; want: (x, sp, nzcv, pc, v, mem). Returns a list of mismatches."""
    hx, hsp, hnz, hpc, hmem, hv = got
    ex, esp, enz, epc, ev, emem = want
    d = [f'x{j}: got {hx[j]:x} want {ex[j]:x}' for j in range(31) if hx[j] != ex[j]]
    if hsp != esp:
        d.append(f'sp: got {hsp:x} want {esp:x}')
    if check_nzcv and hnz != enz:
        d.append(f'nzcv: got {hnz:x} want {enz:x}')
    if check_pc and hpc != epc:
        d.append(f'pc: got {hpc:x} want {epc:x}')
    if check_mem and hmem != emem:
        d.append('memory differs')
    d += [f'v{j}: got {hv[j]:032x} want {ev[j]:032x}' for j in range(32) if hv[j] != ev[j]]
    return d
