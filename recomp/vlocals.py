"""V registers in C locals (gen.py --v-locals K): a rewrite of one emitted function's C.

The lifter writes V registers as `c->v[n]`, lane forms `((T *)&c->v[n])[i]`, the register macros of runtime/simd.h
(VREG, VQ, VW, VS32, ...) and helper calls that name registers by number (fp_op_s(c, op, d, n, m), ...). A chunk
generated with K > 0 defines RT_VLOCALS, under which those macros and helpers reach a register through a local array
`u128 vr[32]` when its bit in the function's VMASK is set, else through c->v; every access is a whole-vector load or
store, so clang's SROA keeps each local register in an XMM register. This module

- turns the direct forms into the accessors (c->v[n] = e -> VSET, c->v[n] -> VGET, lane forms -> VLANE/VLSET),
- picks the K registers the function names most often (VMASK),
- loads them at entry; before every call, rt_dispatch, rt_svc/rt_udf/rt_brk, a host hook (rt_hook), rt_spin_gate and
  exit stores the ones the function writes (VSYNC_OUT_D: only if the dirty mask may have the bit; the mask folds to a
  constant on most paths); after a call reloads them, except V8-V15 with trust (AAPCS64: a callee preserves their low
  64 bits, and a conforming caller does not read their upper halves after the call).

Why the cap: 32 guest registers on 16 XMM registers spill (all 32 cost +20% instructions, the stack traffic tripled),
while K = 6..8 removed a quarter of the c->v traffic and kept dependent FP chains in registers; build/v143-vloc
NOTES.md has the measurements.
"""
import re
from collections import Counter

# Default K: 8 cut guest time 3.9-6.2% in the town on a 7900X against K = 0 (build/v143-vloc NOTES.md, series s1 and g1).
LOCALS = 8

# runtime/simd.h helpers that take VCTX, with their parameters after c. Register-number parameters are d (written,
# also read by some), n, m, a (read) and t (the first of up to 4 consecutive registers; written by the sl_ld* loads).
HELPERS = {
    'fp_op_s': ['op', 'd', 'n', 'm'],
    'fp_op_d': ['op', 'd', 'n', 'm'],
    'fp_fma_s': ['na', 'nn', 'd', 'n', 'm', 'a'],
    'fp_fma_d': ['na', 'nn', 'd', 'n', 'm', 'a'],
    'sl_gen': ['op', 'sz', 'ne', 'd', 'szd', 'offd', 'keep', 'n', 'szn', 'offn', 'm', 'szm', 'offm', 'bidx', 'bimm'],
    'sl_addlp': ['u', 'acc', 'Q', 'sz', 'd', 'n'],
    'sl_across': ['op', 'u', 'Q', 'sz', 'd', 'n'],
    'sl_pmull64': ['part', 'd', 'n', 'm'],
    'sv_fred_s': ['op', 'd', 'n'],
    'sv_fcvtl': ['sz', 'part', 'd', 'n'],
    'sv_fcvtn': ['sz', 'part', 'odd', 'd', 'n'],
    'sv_tbl': ['Q', 'len', 'tbx', 'd', 'n', 'm'],
    'sv_aes': ['op', 'd', 'n'],
    'sv_sha1hash': ['op', 'd', 'n', 'm'],
    'sv_sha256hash': ['part1', 'd', 'n', 'm'],
    'sv_sha_misc': ['op', 'd', 'n', 'm'],
    'sl_ldm': ['Q', 'sz', 'selem', 'rpt', 't', 'ea'],
    'sl_stm': ['Q', 'sz', 'selem', 'rpt', 't', 'ea'],
    'sl_lds': ['sz', 'selem', 'idx', 't', 'ea'],
    'sl_sts': ['sz', 'selem', 'idx', 't', 'ea'],
    'sl_ldr': ['Q', 'sz', 'selem', 't', 'ea'],
}
REG_PARAMS = ('d', 'n', 'm', 'a', 't')
LANE_T = {'sa16': 'vu16', 'sa32': 'vu32', 'sa64': 'vu64', 'saf32': 'vf32', 'saf64': 'vf64', 'uint8_t': 'vu8',
          'uint16_t': 'vu16', 'uint32_t': 'vu32', 'uint64_t': 'vu64', 'float': 'vf32', 'double': 'vf64'}
CALLEE_SAVED = frozenset(range(8, 16))
PLAIN = '\n    const unsigned VMASK = 0; u128 *const vr = c->v; unsigned vd_ = 0, *const vdp = &vd_; (void)vr; (void)vdp;'

FN = re.compile(r'^(?:static )?void (?:f|fb)_[0-9a-f]+\(Cpu \*c\) \{$', re.M)
CALL = re.compile(r'\b(?:f|fb)_[0-9a-f]+\(c\);|\brt_dispatch\(c\);|\brt_(?:svc|udf|brk|hook|spin_gate)\(c,[^;]*;')
RET = re.compile(r'(?:__attribute__\(\(musttail\)\)\s*)?\breturn\b[^;]*;')
RET_BLOCK = re.compile(r'(\{ [^{}]*?(?:__attribute__\(\(musttail\)\) )?return\b[^;]*; \})')
LANE = re.compile(r'\(\((\w+) \*\)&c->v\[')
WHOLE = re.compile(r'c->v\[')
USES = re.compile(r'\b(' + '|'.join(['VSET', 'VLSET', 'VGET', 'VLANE', 'VS8', 'VS16', 'VS32', 'VS64', 'VSF32', 'VSF64',
                                     'VHI64', 'VREG', 'VQ', 'VW', 'FP_SETS', 'FP_SETD', 'SV_FOP_S', 'SV_FOP_D']
                                    + sorted(HELPERS)) + r')\(')


class Unsupported(ValueError):
    pass


def _close(s, i):
    """s[i-1] opens a bracket; the index after its match"""
    d = 1
    while d:
        d += {'[': 1, ']': -1}.get(s[i], 0)
        i += 1
    return i


def _stmt_end(s, i):
    """index of the ';' that ends the expression starting at s[i]"""
    d = 0
    while True:
        ch = s[i]
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        elif ch == ';' and d == 0:
            return i
        i += 1


def _args(s, i):
    """arguments of the call whose '(' is s[i]"""
    d, cur, out = 0, '', []
    while True:
        ch = s[i]
        if ch == '(':
            d += 1
            if d == 1:
                i += 1
                continue
        elif ch == ')':
            d -= 1
            if d == 0:
                out.append(cur.strip())
                return out
        elif ch == ',' and d == 1:
            out.append(cur.strip())
            cur = ''
            i += 1
            continue
        cur += ch
        i += 1


def accessors(t):
    """c->v forms of emitted C -> the RT_VLOCALS accessors of runtime/simd.h"""
    out, i = [], 0
    while True:
        m1, m2 = LANE.search(t, i), WHOLE.search(t, i)
        m = min((x for x in (m1, m2) if x), key=lambda x: x.start(), default=None)
        if not m:
            out.append(t[i:])
            return ''.join(out)
        out.append(t[i:m.start()])
        if m is m1:
            e = _close(t, m.end())
            if m.group(1) not in LANE_T or t[e:e + 2] != ')[':
                raise Unsupported(f'V register pointer form {t[m.start():e + 10]!r}')
            je = _close(t, e + 2)
            reg, lane, vt = accessors(t[m.end():e - 1]), t[e + 2:je - 1], LANE_T[m.group(1)]
            a = re.match(r'\s*=(?!=)', t[je:])
            if a:
                se = _stmt_end(t, je + a.end())
                out.append(f'VLSET({reg}, {vt}, {lane}, {accessors(t[je + a.end():se]).strip()})')
                i = se
            else:
                out.append(f'VLANE({reg}, {vt}, {lane})')
                i = je
            continue
        if t[m.start() - 1:m.start()] == '&':
            raise Unsupported(f'V register address {t[m.start() - 1:m.start() + 20]!r}')
        e = _close(t, m.end())
        reg = accessors(t[m.end():e - 1])
        a = re.match(r'\s*=(?!=)', t[e:])
        if a:
            se = _stmt_end(t, e + a.end())
            out.append(f'VSET({reg}, {accessors(t[e + a.end():se]).strip()})')
            i = se
        else:
            out.append(f'VGET({reg})')
            i = e


def _lit(a):
    a = a.strip().rstrip('uUlL')
    if re.fullmatch(r'-\d+', a):
        return -1
    return int(a, 0) if re.fullmatch(r'0x[0-9a-fA-F]+|\d+', a) else None


def registers(body):
    """(Counter of register uses, set of written registers) of a body in accessor form"""
    used, wr = Counter(), set()
    for m in USES.finditer(body):
        name = m.group(1)
        args = _args(body, m.end() - 1)
        if name in HELPERS:
            for p, a in zip(['c'] + HELPERS[name], args):
                if p not in REG_PARAMS:
                    continue
                v = _lit(a)
                if v == -1:
                    continue
                if v is None:
                    raise Unsupported(f'register argument {a!r} of {name}')
                span = 4 if p == 't' or (name == 'sv_tbl' and p == 'n') else 1
                rs = [(v + k) & 31 for k in range(span)]
                used.update(rs)
                if p == 'd' or (p == 't' and name.startswith('sl_ld')):
                    wr.update(rs)
            continue
        pos, write = {'VREG': (1, False), 'VQ': (2, False), 'VW': (2, True), 'SV_FOP_S': (2, True),
                      'SV_FOP_D': (2, True), 'VSET': (0, True), 'VLSET': (0, True), 'FP_SETS': (0, True),
                      'FP_SETD': (0, True)}.get(name, (0, False))
        v = _lit(args[pos])
        if v is None or v < 0:
            raise Unsupported(f'register {args[pos]!r} of {name}')
        used[v] += 1
        if write:
            wr.add(v)
    return used, wr


def rewrite_one(body, k, trust, stats):
    """Body of one C function (text between its braces)."""
    try:
        new = accessors(body)
        cnt, written = registers(new)
    except Unsupported:
        # a form this rewrite does not know: every register stays in c->v (VMASK 0)
        stats['v_unsupported_functions'] += 1
        return PLAIN + body
    body = new
    if not cnt:
        return body
    local = {r for r, _ in sorted(cnt.items(), key=lambda x: (-x[1], x[0]))[:k]}
    written &= local
    load = ''.join(f'VSYNC_IN({r}); ' for r in sorted(local))
    reload = ''.join(f'VSYNC_IN({r}); ' for r in sorted(local - (CALLEE_SAVED if trust else set())))
    store = ''.join(f'VSYNC_OUT_D({r}); ' for r in sorted(written))
    stats['v_local_functions'] += 1
    stats['v_local_registers'] += len(local)

    def at_ret(m):
        stats['v_exit_syncs'] += 1
        return '{ ' + store + m.group(0) + ' }'

    def at_call(m):
        stats['v_call_syncs'] += 1
        return '{ ' + store + m.group(0) + ' ' + reload + 'vd_ = 0; }'

    body = RET.sub(at_ret, body)
    parts = RET_BLOCK.split(body)
    body = ''.join(p if i % 2 else CALL.sub(at_call, p) for i, p in enumerate(parts))
    mask = sum(1 << r for r in local)
    return (f'\n    const unsigned VMASK = {mask:#x}u; u128 vr[32]; unsigned vd_ = 0, *const vdp = &vd_; {load.rstrip()}'
            + body + ('\n    ' + store.rstrip() if store else ''))


def rewrite(text, k, trust, stats):
    """C text of one emitted guest function (an f_ and, for a computed branch, its fb_) with K registers local."""
    out, pos = [], 0
    for m in FN.finditer(text):
        d, i = 1, m.end()
        while d:
            d += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        out.append(text[pos:m.end()])
        out.append(rewrite_one(text[m.end():i - 1], k, trust, stats) + text[i - 1])
        pos = i
    out.append(text[pos:])
    return ''.join(out)
