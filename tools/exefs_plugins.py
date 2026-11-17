"""Run a mod plugin's init in the plugin service run and check what it did: the build step before the generator emits
a plugin's hook sites. After the design in #845 by itaskaev2; the init runs on this port's runtime (totk.exe
--plugin-init, runtime/plugin_init.c) instead of under Unicorn, and it runs again at every start of the game in the
same environment (plugin_init.c plugins_start), so the plugin keeps real state (its heap blocks, its GOT, its alias
addresses). The record says what the build expects that start-up to write.

  python tools/exefs_plugins.py --exe build/<ver>-x/totk.exe --out <dir>     # per plugin: two runs, diff, check
  python tools/exefs_plugins.py --check <dir>/<plugin>/run1                  # the check of one record
  python tools/exefs_plugins.py --exe <exe> --out <dir> --mods <mods folder> --patches <patch file>
                                --patches-out <patch file> --status <file>   # and the patch lines for the generator

With --mods, --patches, --patches-out and --status, an accepted plugin becomes one `ordered` patch set mod_<hash>
(recomp/patches.py: a plugin line, its expected writes, stubs, seeds, orig lines and its hook sites) appended to the
--patches file into --patches-out, and every plugin gets a line of --status (runtime/mods.c reads it as
build-cache/plugins.status), in the format of tools/exefs_patches.py --status:

  <file hash> <content hash> <kind> <built-in sets> <note>

file hash: FNV-1a 32 of "<mod folder name>/<path of the NSO in the mod>"; content hash: the same over its bytes; kind
`plugin` (note: the module and its hook count) or `unsupported` (note: the first reason); built-in sets: the port's
own sets with a site the plugin hooks too ("-" none), which the runtime switches off while the mod is on.

The exe must be built with the plugins in its layout (recomp/gen.py --plugins). Each plugin runs in service runs of
its own (TOTK_PLUGIN_ONLY): the other plugins stay loaded, their inits do not run. A plugin that is refused, crashes,
hangs or fails the check is refused alone, with its reason; the others are built in. At every start the game runs each
init against the game's own bytes as well (runtime/plugin_init.c plugins_start takes a plugin's writes back before the
next init), so a plugin's own record is what its start must match. --out gets list/ (a run without inits, which names
the plugin modules), and <plugin>/run1 and <plugin>/run2 for each plugin; run2 adds a large offset to every
GetSystemTick (TOTK_PLUGIN_TICK_OFFSET), so the init picks other alias addresses. The words that differ between the two
records are listed with what they look like, not refused: an init keeps its alias addresses and the regions GetInfo
gave it in its .data, and a plugin's callbacks must not use them. A difference in code (a write into a game module,
the plugin's .text or .rodata) refuses the plugin: its start could not be compared with the record.

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
  - the code the hooks reach (the stubs, exlaunch's hook impls, the callbacks, the replace and call targets, and what
    they reach by direct branches and calls, reach()) has no svc but OutputDebugString and calls only the imports
    REACH_IMPORTS. An indirect call (blr) is not followed: what it reaches cannot be known before the run.
A site shared with a built-in patch set turns that set off while the mod is on (the status line names it); a site
shared with another mod goes through the run time's pick (the Mods tab order, runtime/patches.c).
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cheats import fnv  # noqa: E402
from exefs_patches import EXEFS_PLACES, builtin_sites  # noqa: E402
from nso import R_AARCH64_JUMP_SLOT, Nso  # noqa: E402

ALLOWED_SVCS = {0x06: 'QueryMemory', 0x1E: 'GetSystemTick', 0x27: 'OutputDebugString', 0x29: 'GetInfo',
                0x74: 'MapProcessMemory', 0x75: 'UnmapProcessMemory'}
ALLOWED_INFO = {2, 3, 4, 5, 12, 13, 14, 15, 65001}
# Reviewed one by one: SNPrintf formats into the caller's buffer; GetDisplayVersion is answered by the runtime with
# the build's game version; mersenne_twister_engine<uint64_t, ...>::_M_gen_rand (Full Inventory Expansion's alias
# picker) refills the state of the engine object it is given, which lives in the plugin's .bss; memcmp and strlen only
# read; memcpy, memmove and memset write the buffers they are given; malloc and free use the game's heap (sdk's musl
# allocator), and since the init runs again at every start, a block it keeps exists at play time (Quick Weapon
# Durability's init keeps one in its .bss).
SAFE_LIBC = {'malloc', 'free', 'memcpy', 'memmove', 'memset', 'memcmp', 'strlen'}
MT_GEN_RAND = ('_ZNSt23mersenne_twister_engineImLm64ELm312ELm156ELm31ELm13043109905998158313ELm29ELm6148914691236517205'
               'ELm17ELm8202884508482404352ELm37ELm18444473444759240704ELm43ELm6364136223846793005EE11_M_gen_randEv')
SNPRINTF = '_ZN2nn4util8SNPrintfEPcmPKcz'
ALLOWED_IMPORTS = SAFE_LIBC | {SNPRINTF, '_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE', MT_GEN_RAND}
# The imports the code the hooks reach may call while the game runs (reach()). Each reviewed for what it touches: the
# libc ones above (the caller's buffers and the game's heap), SNPrintf (the caller's buffer). Nothing that waits,
# talks to a service or changes memory mappings.
REACH_IMPORTS = SAFE_LIBC | {SNPRINTF}
SVC_OUTPUT_DEBUG_STRING = 0x27
HOOK_IMPLS = ('exl_inline_hook_impl', 'exl_inline_float_hook_impl')
STUR_X30 = 0xF81F03FE   # stur x30, [sp, #-0x10]
LDUR_X30 = 0xF85F03FE   # ldur x30, [sp, #-0x10]
STUB_WORDS = 6          # 4 instructions and the callback's .quad
TICK_OFFSET = 0x0123456789AB


def u(v):
    return int(v, 16) if isinstance(v, str) else v


def ran_view(rec):
    """The record as seen from the plugins whose inits ran: a plugin that only stayed loaded (TOTK_PLUGIN_ONLY) counts
    as a module that is no plugin, so its exports and code are not checked as this record's. A record without "ran"
    (every init ran) comes back as it is."""
    if all(m.get('ran', True) for m in rec['modules'] if m['plugin']):
        return rec
    view = dict(rec)
    view['modules'] = [dict(m, plugin=False) if m['plugin'] and not m.get('ran', True) else m for m in rec['modules']]
    names = {m['name'] for m in view['modules'] if m['plugin']}
    view['images'] = {k: v for k, v in rec['images'].items() if k in names}
    return view


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


# ---- the code the hooks reach
def plt_imports(nso, img, base, text_end):
    """{absolute address of a PLT stub: the import its GOT slot binds} (adrp x16; ldr x17, [x16, #slot]; add x16, x16,
    #slot; br x17), from the JUMP_SLOT relocations."""
    slots = {off: nso.syms[sym][0] for off, typ, sym, _ in nso.relocs if typ == R_AARCH64_JUMP_SLOT}
    out = {}
    for o in range(0, text_end - 16, 4):
        w0, w1, _, w3 = struct.unpack_from('<4I', img, o)
        if w0 & 0x9F00001F != 0x90000010 or w1 & 0xFFC003FF != 0xF9400211 or w3 != 0xD61F0220:
            continue
        imm = sext((w0 >> 29 & 3) | (w0 >> 5 & 0x7FFFF) << 2, 21)
        slot = ((o & ~0xFFF) + (imm << 12)) + (w1 >> 10 & 0xFFF) * 8
        if slot in slots:
            out[base + o] = slots[slot]
    return out


def reach(img, base, text_end, roots, plt, stubs=()):
    """What the code from roots (absolute addresses in the plugin's .text) reaches by falling through, direct branches
    and direct calls: (svc immediates, {import: calls}, indirect calls, words outside the .text it branched to). A
    stub's b to its trampoline counts as its return (recomp/patches.py `stub`)."""
    svcs, imports, indirect, outside = set(), {}, 0, set()
    seen, work = set(), list(roots)
    stub_rets = {s + 12 for s in stubs}
    while work:
        a = work.pop()
        while True:
            if a in seen:
                break
            if a in plt:
                imports[plt[a]] = imports.get(plt[a], 0) + 1
                break
            if not base <= a < base + text_end:
                outside.add(a)
                break
            seen.add(a)
            w = struct.unpack_from('<I', img, a - base)[0]
            if a in stub_rets:
                break
            if w & 0xFFE0001F == 0xD4000001:   # svc #imm
                svcs.add(w >> 5 & 0xFFFF)
            if w & 0x7C000000 == 0x14000000:   # b, bl
                t = branch_target(w, a)
                if w >> 31:
                    work.append(t)
                else:
                    a = t
                    continue
            elif pc_relative(w, a) and pc_relative(w, a)[0][0] in ('b.cond', 'cbz', 'tbz'):
                work.append(pc_relative(w, a)[1])
            elif w & 0xFFFFFC1F == 0xD63F0000:   # blr
                indirect += 1
            elif is_indirect(w):   # br, ret: the path ends
                break
            elif w == 0 or w & 0xFFE0001F == 0xD4200000:   # udf, brk: ends too
                break
            a += 4
    return svcs, imports, indirect, outside


def reach_check(rec, hooks, impls):
    """Reasons the code the hooks reach breaks the rules (svcs, imports), and a summary line."""
    reasons, notes = [], []
    for p in (m for m in rec['modules'] if m['plugin'] and m['name'] in rec['images']):
        img, base = rec['images'][p['name']], p['base']
        roots, stubs = set(), set()
        for h in hooks:
            for k in ('stub', 'callback', 'target'):
                if h.get(k, '').startswith(p['name'] + '+'):
                    roots.add(base + int(h[k].split('+')[1], 16))
            if h['kind'] == 'inline' and h['stub'].startswith(p['name'] + '+'):
                stubs.add(base + int(h['stub'].split('+')[1], 16))
        roots |= {a for a in impls if base <= a < base + p['text_end']}
        nso = Nso(p['file']) if p.get('file') and Path(p['file']).is_file() else None
        plt = plt_imports(nso, img, base, p['text_end']) if nso else {}
        svcs, imports, indirect, outside = reach(img, base, p['text_end'], sorted(roots), plt, stubs)
        for imm in sorted(svcs - {SVC_OUTPUT_DEBUG_STRING}):
            reasons.append(f'the code its hooks reach has svc {imm:#x}')
        for name in sorted(set(imports) - REACH_IMPORTS):
            reasons.append(f'the code its hooks reach calls the import {name}')
        notes.append(f'{p["name"]}: hooks reach {len(roots)} roots, imports {", ".join(sorted(imports)) or "none"}, '
                     f'{indirect} indirect calls not followed' + (f', {len(outside)} branches out of its .text'
                                                                  if outside else ''))
    return reasons, notes


# ---- patch lines and status for the generator and the runtime
def mod_of(nso_path, mods):
    """(mod folder name, path of the NSO in the mod) of a plugin file under the mods folder, else None."""
    try:
        rel = Path(nso_path).resolve().relative_to(Path(mods).resolve())
    except ValueError:
        return None
    return rel.parts[0], '/'.join(rel.parts[1:])


def hooks_of(hooks, module):
    """The hooks whose stub or target lies in the plugin module: its own, when one record holds several plugins."""
    return [h for h in hooks if (h.get('stub') or h.get('target') or '').split('+')[0] == module]


def plugin_lines(rec, hooks, name, record_dir, module):
    """The patch lines of the accepted plugin module (recomp/patches.py) for set name: the plugin line, the expected
    writes (the words at its hooks' sites), the stubs, the seeds, the orig lines and the hook sites."""
    lines = []
    mods = {m['name']: m for m in rec['modules']}
    p = mods[module]
    img = rec['images'][p['name']]
    text_sha = hashlib.sha256(img[:p['text_end']]).hexdigest()
    lines.append(f'plugin {name} {p["name"]} {text_sha} {(Path(record_dir) / p["image"]).resolve().as_posix()}')
    own = {int(h['site'].split('+')[1], 16) for h in hooks}
    for w in rec['writes']:
        if w['module'] == 'main' and w['offset'] in own:
            lines.append(f'expect {name} {w["module"]} {w["offset"]:x} {w["old"]:08x} {w["new"]:08x}')
    seeds, stubs = {}, {}

    def at(ref):
        mod, off = ref.split('+')
        return mod, int(off, 16)

    for h in hooks:
        site = int(h['site'].split('+')[1], 16)
        if h['kind'] == 'inline':
            sm, so = at(h['stub'])
            stubs.setdefault(sm, set()).add(so)
            cm, co = at(h['callback'])
            seeds.setdefault(cm, set()).add(co)
            # the hook impl the stub calls: its bl, as check_record read it
            stub_at = mods[sm]['base'] + so
            impl = branch_target(struct.unpack_from('<I', rec['images'][sm], so + 4)[0], stub_at + 4)
            seeds.setdefault(sm, set()).add(impl - mods[sm]['base'])
            lines.append(f'site {name} main {site:x} {u(h["original"]):08x} @call:{sm}+{so:x} {u(h["original"]):08x}')
        else:
            tm, to = at(h['target'])
            seeds.setdefault(tm, set()).add(to)
            new = next(w['new'] for w in rec['writes'] if w['module'] == 'main' and w['offset'] == site)
            lines.append(f'site {name} main {site:x} {u(h["original"]):08x} {new:08x}')
            if h['kind'] == 'replace' and h.get('trampoline'):
                rm, ro = at(h['trampoline'])
                lines.append(f'orig {name} {rm} {ro:x} main {site:x}')
    for m in sorted(stubs):
        lines += [f'stub {m} {o:x}' for o in sorted(stubs[m])]
    for m in sorted(seeds):
        lines.append(f'seed {m} ' + ' '.join(f'{o:x}' for o in sorted(seeds[m])))
    return lines


def plugin_files(mods):
    """[(mod folder name, path in the mod, file)] of the exlaunch plugins of the mods folder, by mod name: the subsdk*
    files of each mod's first ExeFS place (tools/exefs_patches.py EXEFS_PLACES, runtime/mods.c find_root). A replaced
    main is no plugin."""
    out = []
    for mod in sorted((d for d in Path(mods).iterdir() if d.is_dir()), key=lambda d: d.name) if Path(mods).is_dir() else []:
        exefs = next((mod / w for w in EXEFS_PLACES if (mod / w).is_dir()), None)
        if exefs:
            for f in sorted(exefs.iterdir(), key=lambda f: f.name.lower()):
                if f.is_file() and f.name.lower().startswith('subsdk'):
                    out.append((mod.name, f.relative_to(mod).as_posix(), f))
    return out


def emit(results, args):
    """--patches-out and --status from the checked plugins. results: one (module, its NSO file, its record or None,
    the record's directory, hooks, reasons) per plugin of the layout; no reasons: accepted."""
    text = args.patches.read_text() if args.patches else ''
    builtin = builtin_sites(text)
    status, sets = [], []
    files = {str(f.resolve()): (m, rel, f) for m, rel, f in plugin_files(args.mods)}
    for module, file, rec, record_dir, hooks, reasons in results:
        hit = files.get(str(Path(file or '').resolve()))
        if not hit:
            print(f'plugins: {module}: {file} is not under {args.mods}')
            continue
        mod, rel, f = hit
        key, content = fnv(f'{mod}/{rel}'.encode()), fnv(f.read_bytes())
        if reasons:
            note = ' '.join(reasons[0].split())
            status.append(f'{key}\t{content}\tunsupported\t-\t{note}')
            print(f'plugins: unsupported {mod}/{rel} ({note})')
            continue
        name = f'mod_{key.lower()}'
        own = hooks_of(hooks, module)
        sites = [int(h['site'].split('+')[1], 16) for h in own]
        over = sorted({b for a in sites for b in builtin.get(a, [])})
        label = f'{mod}/{rel}'.encode('ascii', 'replace').decode()
        sets.append([f'set {name} TOTK_MODPATCH_{key} 0 ordered   # {label}'] + plugin_lines(rec, own, name, record_dir,
                                                                                          module))
        status.append(f'{key}\t{content}\tplugin\t{",".join(over) or "-"}\t{module}, {len(own)} hooks')
        print(f'plugins: plugin {mod}/{rel} as {module}, {len(own)} hooks' + (f', over {", ".join(over)}'
                                                                               if over else ''))
    if sets:
        text += '\n# Mod plugins (tools/exefs_plugins.py)\n' + '\n'.join(line for s in sets for line in s) + '\n'
    args.patches_out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')


# ---- the service runs
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


def run(exe, out, tick_offset, timeout, only):
    """One service run into out, only module `only`'s init running ("-": none). (exit code or None, why the run gave
    no record or None); a refused run writes its record too."""
    out.mkdir(parents=True, exist_ok=True)
    for f in out.iterdir():   # an earlier run's record must not pass for this one's
        if f.is_file() and (f.name in ('record.json', 'run.log') or f.suffix == '.img'):
            f.unlink()
    env = dict(os.environ, TOTK_PLUGIN_TICK_OFFSET=f'{tick_offset:#x}', TOTK_PLUGIN_ONLY=only,
               TOTK_BACKGROUND_WINDOW='1', TOTK_ASSUME_FOCUS='1')
    try:
        r = subprocess.run([str(exe), '--plugin-init', str(out)], env=env, capture_output=True, text=True,
                           errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired as e:
        log = b''.join(x if isinstance(x, bytes) else x.encode() for x in (e.stdout, e.stderr) if x)
        (out / 'run.log').write_text(log.decode(errors='replace'), encoding='utf-8')
        return None, f'its init did not return within {timeout} s'
    (out / 'run.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    if not (out / 'record.json').is_file():
        # the runtime's crash report names the fault in a FATAL line (an access violation, an abort)
        fatal = next((line.split('FATAL:', 1)[1].strip() for line in (r.stdout + r.stderr).splitlines()
                      if 'FATAL:' in line), None)
        return r.returncode, (f'its init crashed the service run: {fatal}' if fatal else
                              f'its init ended the service run without a record (exit {r.returncode & 0xFFFFFFFF:#x})')
    return r.returncode, None


def report(rec, label):
    rec = ran_view(rec)
    impls = hook_impls(rec)
    hooks, reasons = check_record(rec, impls)
    more, notes = reach_check(rec, hooks, impls)
    reasons += more
    for n in notes:
        print(f'  {n}')
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
    return hooks, reasons


def plugin_runs(exe, out, module, timeout):
    """The two service runs of one plugin (run2 with the tick offset) into out/<module>: (its first record or None,
    hooks, reasons)."""
    recs = []
    for i, offset in enumerate((0, TICK_OFFSET), 1):
        d = out / module / f'run{i}'
        code, why = run(exe, d, offset, timeout, module)
        if why:
            print(f'{module} run {i}: {why}; see {d / "run.log"}')
            return None, [], [why]
        recs.append(load_record(d))
        print(f'{module} run {i}: exit {code}, tick offset {offset:#x}')
        if recs[0].get('refused'):   # refused: a second run says nothing more
            hooks, bad = report(recs[0], f'{module} run 1')
            return recs[0], hooks, bad
    hooks, bad = report(recs[0], f'{module} run 1')
    diff = diff_records(ran_view(recs[0]), ran_view(recs[1]))
    print(f'{module} run 1 against run 2: {len(diff)} differences' + (':' if diff else ''))
    for line in diff:
        print(f'  {line}')
    # Words of a game module or of the plugin's .text that differ between the runs: the start-up could not be compared
    # with the record. Its .data and .bss may differ (alias addresses).
    unstable = [line for line in diff if line.startswith('write ') or '(.text)' in line or '(.rodata)' in line]
    if unstable:
        bad = bad + [f'two runs of the init differ in code: {unstable[0]}']
    return recs[0], hooks, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', type=Path, help='totk.exe built with the plugins in its layout')
    ap.add_argument('--out', type=Path, help='directory for list/ and <plugin>/run1, run2')
    ap.add_argument('--check', type=Path, help='check the record in this directory only')
    ap.add_argument('--timeout', type=int, default=120, help='seconds a service run may take')
    ap.add_argument('--mods', type=Path, help='the mods folder the plugins come from')
    ap.add_argument('--patches', type=Path, help='the patch file so far')
    ap.add_argument('--patches-out', type=Path, help='the patch file with the plugins\' sets added')
    ap.add_argument('--status', type=Path, help='the plugin status file (build-cache/plugins.status)')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    emitting = bool(args.patches_out or args.status)
    if emitting and not (args.mods and args.patches_out and args.status):
        ap.error('--mods, --patches-out and --status go together')
    if args.check:
        rec = load_record(args.check)
        hooks, bad = report(rec, str(args.check))
        if emitting:
            emit([(m['name'], m.get('file'), rec, args.check, hooks, bad) for m in ran_view(rec)['modules']
                  if m['plugin']], args)
        return 1 if bad else 0
    if not args.exe or not args.out:
        ap.error('--exe and --out, or --check')
    # A run without any init names the plugin modules of the exe's layout and checks that the service run works at all:
    # without it no plugin can be built.
    code, why = run(args.exe, args.out / 'list', 0, args.timeout, '-')
    if why:
        print(f'the service run without inits gave no record: {why}; see {args.out / "list" / "run.log"}')
        return 2
    listing = load_record(args.out / 'list')
    results = []
    for m in (m for m in listing['modules'] if m['plugin']):
        rec, hooks, bad = plugin_runs(args.exe, args.out, m['name'], args.timeout)
        print(f'{m["name"]}: ' + (f'refused: {bad[0]}' if bad else f'accepted, {len(hooks)} hooks'))
        results.append((m['name'], m.get('file'), rec, args.out / m['name'] / 'run1', hooks, bad))
    if emitting:
        emit(results, args)
    return 1 if any(r[5] for r in results) else 0


if __name__ == '__main__':
    sys.exit(main())
