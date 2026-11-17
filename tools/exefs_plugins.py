"""Run a mod plugin's init in the plugin service run and check what it did: the build step before the generator emits
a plugin's hook sites. After the design in #845 by itaskaev2; the init runs on this port's runtime (totk.exe
--plugin-init, runtime/plugin_init.c) instead of under Unicorn.

  python tools/exefs_plugins.py --exe build/<ver>-x/totk.exe --out <dir>     # two runs, their diff, the check
  python tools/exefs_plugins.py --check <dir>/run1                           # the check of one record

The exe must be built with the plugin in its layout (recomp/gen.py --plugins). --out gets run1/ and run2/, one record
each; run2 adds a large offset to every GetSystemTick (TOTK_PLUGIN_TICK_OFFSET), so the init picks other alias
addresses. The words that differ between the two records are listed with what they look like, not refused: an init
keeps its alias addresses and the regions GetInfo gave it in its .data, and a plugin's callbacks must not use them
(the generator's check of the code the callbacks reach, still to come).

The check (check_record) accepts a record only when every rule holds; each broken rule is one reason:
  - the run was not refused (an svc outside the served set, such as CreateSession of exlaunch's process-handle
    fallback, ends the run with the reason);
  - the init called only the svcs ALLOWED_SVCS, GetInfo with ALLOWED_INFO, and the imports ALLOWED_IMPORTS;
  - every write into a game module is a word of main's .text whose original is no br, blr or ret, and it is one of:
    - an inline hook (MSFix, Full Inventory Expansion): `b <stub>` into a plugin's .text, the stub in exlaunch's
      shape (stur x30, [sp, #-0x10]; bl exl_inline_hook_impl or the float one; ldur x30, [sp, #-0x10];
      b <trampoline>; .quad <callback in the plugin's .text>), the trampoline the site's original relocated (the
      same word, or the same PC-relative target) and `b <site + 4>`;
    - a call-site rewrite (Quick Weapon Durability): `bl` straight to a function start of the plugin's lift (the
      record lists them), which the generator can emit as a patch site whose replacement is that branch;
    - a replace hook (Full Inventory Expansion's Sort Fixer): `b` from a function start of main to a function start of
      the plugin's lift; the Orig trampoline (the original relocated, b site+4) is listed when the pool holds one.
Not checked here yet: a site shared with a built-in patch set or another mod, and svcs in the code the callbacks
reach (both need the generator's view).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
from nso import Nso  # noqa: E402

ALLOWED_SVCS = {0x06: 'QueryMemory', 0x1E: 'GetSystemTick', 0x27: 'OutputDebugString', 0x29: 'GetInfo',
                0x74: 'MapProcessMemory', 0x75: 'UnmapProcessMemory'}
ALLOWED_INFO = {2, 3, 4, 5, 12, 13, 14, 15, 65001}
# Reviewed one by one: SNPrintf formats into the caller's buffer; GetDisplayVersion is answered by the service run;
# mersenne_twister_engine<uint64_t, ...>::_M_gen_rand (Full Inventory Expansion's alias picker) refills the state of the
# engine object it is given, which lives in the plugin's .bss; memcmp and strlen only read. malloc stays out: Quick
# Weapon Durability's init keeps a heap block's address in its .bss, and that block does not exist at play time.
ALLOWED_IMPORTS = {'_ZN2nn4util8SNPrintfEPcmPKcz', '_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE', 'memcmp', 'strlen',
                   '_ZNSt23mersenne_twister_engineImLm64ELm312ELm156ELm31ELm13043109905998158313ELm29ELm6148914691236517205'
                   'ELm17ELm8202884508482404352ELm37ELm18444473444759240704ELm43ELm6364136223846793005EE11_M_gen_randEv'}
HOOK_IMPLS = ('exl_inline_hook_impl', 'exl_inline_float_hook_impl')
STUR_X30 = 0xF81F03FE   # stur x30, [sp, #-0x10]
LDUR_X30 = 0xF85F03FE   # ldur x30, [sp, #-0x10]
STUB_WORDS = 6          # 4 instructions and the callback's .quad
TICK_OFFSET = 0x0123456789AB


def u(v):
    return int(v, 16) if isinstance(v, str) else v


# ---- instructions
def sext(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def branch_target(w, pc):
    """Target of b / bl at pc, else None."""
    if w & 0x7C000000 == 0x14000000:
        return pc + sext(w & 0x3FFFFFF, 26) * 4
    return None


def pc_relative(w, pc):
    """(kind, absolute value) of a PC-relative instruction at pc, else None: b, bl, b.cond, cbz/cbnz, tbz/tbnz, adr,
    adrp, ldr (literal). The kind holds the bits besides the offset, so two encodings match when both agree."""
    if w & 0x7C000000 == 0x14000000:
        return ('b', w & 0xFC000000), pc + sext(w & 0x3FFFFFF, 26) * 4
    if w & 0xFF000010 == 0x54000000:
        return ('b.cond', w & 0xFF00001F), pc + sext(w >> 5 & 0x7FFFF, 19) * 4
    if w & 0x7E000000 == 0x34000000:
        return ('cbz', w & 0xFF00001F), pc + sext(w >> 5 & 0x7FFFF, 19) * 4
    if w & 0x7E000000 == 0x36000000:
        return ('tbz', w & 0xFFF8001F), pc + sext(w >> 5 & 0x3FFF, 14) * 4
    if w & 0x9F000000 == 0x10000000:
        return ('adr', w & 0x1F), pc + sext((w >> 5 & 0x7FFFF) << 2 | w >> 29 & 3, 21)
    if w & 0x9F000000 == 0x90000000:
        return ('adrp', w & 0x1F), (pc & ~0xFFF) + (sext((w >> 5 & 0x7FFFF) << 2 | w >> 29 & 3, 21) << 12)
    if w & 0x3B000000 == 0x18000000:
        return ('ldr', w & 0xFF00001F), pc + sext(w >> 5 & 0x7FFFF, 19) * 4
    return None


def is_indirect(w):
    """br, blr, ret (and their pointer-authenticated forms): a site exlaunch could hook but the generator cannot."""
    return w & 0xFE1F0000 == 0xD61F0000 and w >> 21 & 0xF in (0, 1, 2)


def relocated(orig, site, word, at):
    """The trampoline word at `at` does what the original at `site` does."""
    a, b = pc_relative(orig, site), pc_relative(word, at)
    if a is None:
        return word == orig
    return b is not None and a == b


def inline_stub(img, o, p):
    """The words at image offset o of plugin p have exlaunch's inline-hook entry shape: stur x30; bl; ldur x30; b;
    and the .quad after them lies inside the plugin's .text."""
    if o + 4 * STUB_WORDS > p['text_end']:
        return False
    s = struct.unpack_from('<4I', img, o)
    return s[0] == STUR_X30 and s[2] == LDUR_X30 and s[1] >> 26 == 0x25 and s[3] >> 26 == 0x05


def orig_trampoline(img, p, original, site):
    """Image offset of a pair in the plugin's .text that runs the site's original instruction relocated, then b site+4
    (exlaunch's Orig trampoline of a replace hook), else None."""
    base = p['base']
    for o in range(0, p['text_end'] - 4, 4):
        t1 = struct.unpack_from('<I', img, o + 4)[0]
        if t1 >> 26 == 0x05 and branch_target(t1, base + o + 4) == site + 4 and \
                relocated(original, site, struct.unpack_from('<I', img, o)[0], base + o):
            return o
    return None


# ---- records
def load_record(d):
    """record.json of a service run directory, with the images of its plugins as bytes (key 'images')."""
    d = Path(d)
    rec = json.loads((d / 'record.json').read_text(encoding='utf-8'))
    rec['images'] = {}
    for m in rec['modules']:
        for k in ('base', 'size', 'text_end', 'data_off', 'init'):
            m[k] = u(m[k])
        if m.get('image'):
            rec['images'][m['name']] = (d / m['image']).read_bytes()
    for w in rec['writes']:
        for k in ('offset', 'old', 'new'):
            w[k] = u(w[k])
    return rec


def hook_impls(rec):
    """{absolute address: name} of exlaunch's hook entry points in each plugin, from the plugin NSO's exports."""
    out = {}
    for m in rec['modules']:
        if m['plugin'] and m.get('file') and Path(m['file']).is_file():
            for name, _, shndx, value, _ in Nso(m['file']).exports():
                if name in HOOK_IMPLS:
                    out[m['base'] + value] = name
    return out


def check_record(rec, impls):
    """(hooks, reasons): one dict per hook site, and one line per broken rule (no lines: the record is accepted)."""
    reasons, hooks = [], []
    if rec.get('refused'):
        reasons.append(f'the run was refused: {rec["refused"]}')
    for imm in rec.get('svcs', {}):
        if u(imm) not in ALLOWED_SVCS:
            reasons.append(f'the init called svc {u(imm):#x}')
    for i in rec.get('get_info', {}):
        if int(i) not in ALLOWED_INFO:
            reasons.append(f'the init asked GetInfo id {i}')
    for name, calls in rec.get('imports', {}).items():
        if calls and name not in ALLOWED_IMPORTS:
            reasons.append(f'the init called the import {name}')
    mods = {m['name']: m for m in rec['modules']}
    plugins = [m for m in rec['modules'] if m['plugin']]

    def plugin_word(a):
        """(plugin, word) at absolute a inside a plugin's .text image, else (None, None)."""
        for p in plugins:
            if p['base'] <= a < p['base'] + p['text_end'] and p['name'] in rec['images']:
                return p, struct.unpack_from('<I', rec['images'][p['name']], a - p['base'])[0]
        return None, None

    main = mods.get('main')
    for w in rec['writes']:
        where = f'{w["module"]}+{w["offset"]:#x}'
        if w['module'] != 'main' or not main or w['offset'] >= main['text_end'] or w['offset'] & 3:
            reasons.append(f'a write outside main\'s .text: {where} {w["old"]:#010x} -> {w["new"]:#010x}')
            continue
        site = main['base'] + w['offset']
        stub = branch_target(w['new'], site)
        link = bool(w['new'] >> 31)
        if stub is None:
            reasons.append(f'{where}: the new word {w["new"]:#010x} is no b or bl')
            continue
        if is_indirect(w['old']):
            reasons.append(f'{where}: the original {w["old"]:#010x} is a br, blr or ret')
        p, _ = plugin_word(stub)
        if p is None:
            reasons.append(f"{where}: {'bl' if link else 'b'} {stub:#x} does not reach into a plugin's .text")
            continue
        if not (not link and inline_stub(rec['images'][p['name']], stub - p['base'], p)):
            # A b or bl straight into the plugin's code (CodePatcher-style call-site rewrite): a site whose replacement
            # is that branch, when the target is a function start of the plugin's lift.
            # A b replaces the whole main function, so it must sit at a function start of main; exlaunch's replace
            # hook keeps the function's first instruction relocated + b site+4 in its pool for the callback's Orig.
            o = stub - p['base']
            hook = {'site': f'main+{w["offset"]:#x}', 'original': f'{w["old"]:#010x}',
                    'kind': 'call' if link else 'replace', 'target': f'{p["name"]}+{o:#x}'}
            orig = orig_trampoline(rec['images'][p['name']], p, w['old'], site)
            if orig is not None:
                hook['trampoline'] = f'{p["name"]}+{orig:#x}'
            hooks.append(hook)
            if o not in {u(x) for x in p.get('functions', [])}:
                reasons.append(f"{where}: {'bl' if link else 'b'} {p['name']}+{o:#x} is neither an inline-hook stub "
                               f"nor a function start of {p['name']}'s lift")
            if not link and not w.get('fn_start'):
                reasons.append(f"{where}: b {p['name']}+{o:#x} from inside a main function (only a function start can "
                               f"be replaced)")
            continue
        img, o = rec['images'][p['name']], stub - p['base']
        s = struct.unpack_from('<4IQ', img, o)
        impl, tramp, cb = branch_target(s[1], stub + 4), branch_target(s[3], stub + 12), s[4]
        hook = {'site': f'main+{w["offset"]:#x}', 'original': f'{w["old"]:#010x}', 'kind': 'inline',
                'stub': f'{p["name"]}+{o:#x}',
                'impl': impls.get(impl, f'{impl:#x}' if impl is not None else None),
                'callback': f'{p["name"]}+{cb - p["base"]:#x}' if p['base'] <= cb < p['base'] + p['text_end'] else f'{cb:#x}',
                'trampoline': f'{p["name"]}+{tramp - p["base"]:#x}' if tramp is not None else None}
        hooks.append(hook)
        if impl not in impls:
            reasons.append(f'{where}: the stub calls {impl:#x}, not {" or ".join(HOOK_IMPLS)}')
        if not p['base'] <= cb < p['base'] + p['text_end']:
            reasons.append(f'{where}: the stub\'s callback {cb:#x} is outside {p["name"]}\'s .text')
        tp, t0 = plugin_word(tramp)
        _, t1 = plugin_word(tramp + 4)
        if tp is None or t1 is None:
            reasons.append(f'{where}: the trampoline {tramp:#x} is outside a plugin\'s .text')
            continue
        hook['relocated'] = f'{t0:#010x}'
        if not relocated(w['old'], site, t0, tramp):
            reasons.append(f'{where}: the trampoline {hook["trampoline"]} starts with {t0:#010x}, not the original '
                           f'{w["old"]:#010x} relocated')
        if branch_target(t1, tramp + 4) != site + 4 or t1 >> 31:
            reasons.append(f'{where}: the trampoline\'s second word {t1:#010x} is no b to the site + 4')
    return hooks, reasons


# ---- two runs
def where_is(v, rec):
    """What an address-like value points into, for the diff listing."""
    for m in rec['modules']:
        if m['base'] <= v < m['base'] + m['size']:
            return f'{m["name"]}+{v - m["base"]:#x}'
    for k in ('rtld', 'pool'):
        r = rec.get(k)
        if r and u(r['base']) <= v < u(r['base']) + u(r['size']):
            return f'the {"alias pool" if k == "pool" else k} (+{v - u(r["base"]):#x})'
    for a in rec.get('aliases', []):
        if u(a['dst']) <= v < u(a['dst']) + u(a['size']):
            return f'an alias of {u(a["src"]):#x}'
    return None


def section(m, off, nso_path):
    if off < m['text_end']:
        return '.text'
    if off < m['data_off']:
        return '.rodata'
    if nso_path and Path(nso_path).is_file():
        n = Nso(nso_path)
        if off < n.data[0] + n.data[1]:
            return '.data'
        return '.bss'
    return '.data/.bss'


def diff_records(a, b):
    """Lines naming every difference between two records: writes into game modules, plugin image words (8 bytes)."""
    out = []
    wa = {(w['module'], w['offset']): w for w in a['writes']}
    wb = {(w['module'], w['offset']): w for w in b['writes']}
    for k in sorted(set(wa) | set(wb)):
        x, y = wa.get(k), wb.get(k)
        if not x or not y or x['new'] != y['new']:
            out.append(f'write {k[0]}+{k[1]:#x}: run 1 {x and hex(x["new"])}, run 2 {y and hex(y["new"])}')
    for m in a['modules']:
        if not m['plugin']:
            continue
        ia, ib = a['images'].get(m['name']), b['images'].get(m['name'])
        if ia is None or ib is None or len(ia) != len(ib):
            out.append(f'{m["name"]}: image missing or of another size')
            continue
        for off in range(0, len(ia), 8):
            if ia[off:off + 8] == ib[off:off + 8]:
                continue
            va, vb = struct.unpack_from('<Q', ia, off)[0], struct.unpack_from('<Q', ib, off)[0]
            what = where_is(va, a) or where_is(vb, b)
            out.append(f'{m["name"]}+{off:#x} ({section(m, off, m.get("file"))}): {va:#x} / {vb:#x}'
                       + (f', {what}' if what else ''))
    return out


def run(exe, out, tick_offset, timeout):
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TOTK_PLUGIN_TICK_OFFSET=f'{tick_offset:#x}', TOTK_BACKGROUND_WINDOW='1',
               TOTK_ASSUME_FOCUS='1')
    r = subprocess.run([str(exe), '--plugin-init', str(out)], env=env, capture_output=True, text=True,
                       errors='replace', timeout=timeout)
    (out / 'run.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    return r.returncode


def report(rec, label):
    hooks, reasons = check_record(rec, hook_impls(rec))
    print(f'{label}: {len(rec["writes"])} words written into game modules, {len(rec["aliases"])} process-memory maps, '
          f'svcs {", ".join(f"{ALLOWED_SVCS.get(u(k), hex(u(k)))} x{v}" for k, v in rec["svcs"].items())}; '
          f'GetInfo {", ".join(f"{k} x{v}" for k, v in rec["get_info"].items())}; '
          f'imports {", ".join(f"{k} x{v}" for k, v in rec["imports"].items())}')
    for h in hooks:
        if h['kind'] == 'inline':
            print(f'  inline hook {h["site"]} (was {h["original"]}) -> stub {h["stub"]} -> {h["impl"]}, callback '
                  f'{h["callback"]}, trampoline {h["trampoline"]} ({h.get("relocated")}; b site+4)')
        else:
            print(f'  {h["kind"]} {h["site"]} (was {h["original"]}) -> {"bl" if h["kind"] == "call" else "b"} '
                  f'{h["target"]}' + (f', Orig trampoline {h["trampoline"]}' if h.get('trampoline') else ''))
    for r in reasons:
        print(f'  REFUSED: {r}')
    if not reasons:
        print(f'  accepted: {len(hooks)} hooks')
    return reasons


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', type=Path, help='totk.exe built with the plugin in its layout')
    ap.add_argument('--out', type=Path, help='directory for run1/ and run2/')
    ap.add_argument('--check', type=Path, help='check the record in this directory only')
    ap.add_argument('--timeout', type=int, default=300)
    args = ap.parse_args()
    if args.check:
        return 1 if report(load_record(args.check), str(args.check)) else 0
    if not args.exe or not args.out:
        ap.error('--exe and --out, or --check')
    recs = []
    for i, offset in enumerate((0, TICK_OFFSET), 1):
        d = args.out / f'run{i}'
        code = run(args.exe, d, offset, args.timeout)
        if not (d / 'record.json').is_file():
            print(f'run {i}: no record (exit {code}); see {d / "run.log"}')
            return 1
        recs.append(load_record(d))
        print(f'run {i}: exit {code}, tick offset {offset:#x}')
    bad = report(recs[0], 'run 1')
    diff = diff_records(*recs)
    print(f'run 1 against run 2: {len(diff)} differences' + (':' if diff else ''))
    for line in diff:
        print(f'  {line}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
