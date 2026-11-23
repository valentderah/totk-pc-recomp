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
  - the init called only the svcs ALLOWED_SVCS, GetInfo with ALLOWED_INFO, and the imports ALLOWED_IMPORTS or the
    game's (--exefs: what the game's main imports, game_imports);
  - every write into a game module is a word of main's .text whose original is no br, blr or ret, and it is one of:
    - an inline hook (MSFix, Full Inventory Expansion): `b <stub>` into a plugin's .text, the stub in exlaunch's
      shape (stur x30, [sp, #-0x10]; bl one of exlaunch's hook impls, HOOK_IMPLS; ldur x30, [sp, #-0x10];
      b <trampoline>; .quad <callback in the plugin's .text>), the trampoline the site's original relocated (the
      same word, the same PC-relative target, or exlaunch's long form of it, relocation_end) and its jump back to
      the site + 4 (returns_to);
    - a call-site rewrite (Quick Weapon Durability): `bl` straight to a function start of the plugin's lift (the
      record lists them), which the generator can emit as a patch site whose replacement is that branch;
    - a replace hook (Full Inventory Expansion's Sort Fixer): `b` from a function start of main to a function start of
      the plugin's lift; the Orig trampoline (the original relocated, back to site+4) is listed when the pool holds
      one.
  - the code the hooks reach (the stubs, exlaunch's hook impls, the callbacks, the replace and call targets, and what
    they reach by direct branches and calls, reach()) makes only the svcs REACH_SVCS and calls only the imports
    REACH_IMPORTS or the game's. An indirect call (blr) is not followed: what it reaches cannot be known before the run.
A site shared with a built-in patch set turns that set off while the mod is on (the status line names it); a site
shared with another mod goes through the run time's pick (the Mods tab order, runtime/patches.c).
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cheats import fnv  # noqa: E402
from exefs_patches import EXEFS_PLACES, builtin_sites  # noqa: E402
from nso import R_AARCH64_JUMP_SLOT, Nso  # noqa: E402

ALLOWED_SVCS = {0x01: 'SetHeapSize', 0x06: 'QueryMemory', 0x1E: 'GetSystemTick', 0x27: 'OutputDebugString',
                0x29: 'GetInfo', 0x74: 'MapProcessMemory', 0x75: 'UnmapProcessMemory'}
ALLOWED_INFO = {2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16, 17, 65001}   # 6, 7, 16, 17: runtime/plugin_init.c header
# Reviewed one by one: SNPrintf and snprintf format into the caller's buffer; GetDisplayVersion is answered by the
# runtime with the build's game version; mersenne_twister_engine<uint64_t, ...>::_M_gen_rand (Full Inventory
# Expansion's alias picker) refills the state of the engine object it is given, which lives in the plugin's .bss;
# memcmp and strlen only read; memcpy, memmove and memset write the buffers they are given; malloc and free use the
# game's heap (sdk's musl allocator), and since the init runs again at every start, a block it keeps exists at play
# time (Quick Weapon Durability's init keeps one in its .bss); nn::os::InitializeMutex fills the mutex object it is
# given, GetSystemTickFrequency is a constant (Zonai Fieldworks).
SAFE_LIBC = {'malloc', 'free', 'memcpy', 'memmove', 'memset', 'memcmp', 'strlen', 'snprintf'}
MT_GEN_RAND = ('_ZNSt23mersenne_twister_engineImLm64ELm312ELm156ELm31ELm13043109905998158313ELm29ELm6148914691236517205'
               'ELm17ELm8202884508482404352ELm37ELm18444473444759240704ELm43ELm6364136223846793005EE11_M_gen_randEv')
SNPRINTF = '_ZN2nn4util8SNPrintfEPcmPKcz'
OS_MUTEX = {'_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi', '_ZN2nn2os9LockMutexEPNS0_9MutexTypeE',
            '_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE', '_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE'}
OS_TIME = {'_ZN2nn2os22GetSystemTickFrequencyEv', '_ZN2nn2os16GetCurrentThreadEv'}
ALLOWED_IMPORTS = SAFE_LIBC | {SNPRINTF, '_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE', MT_GEN_RAND,
                               '_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi', '_ZN2nn2os22GetSystemTickFrequencyEv'}
# nn::fs functions the runtime serves on the host (runtime/fs_host.c): a plugin's files live on the mounts it opens,
# the SD card (MountSdCard, <profile>/sdcard) or the game's RomFS with the mods' files over it.
HOST_FS = {'_ZN2nn2fs11MountSdCardEPKc', '_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci',
           '_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE', '_ZN2nn2fs9CloseFileENS0_10FileHandleE',
           '_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm', '_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm',
           '_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE', '_ZN2nn2fs9FlushFileENS0_10FileHandleE',
           '_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl', '_ZN2nn2fs10CreateFileEPKcl', '_ZN2nn2fs10DeleteFileEPKc',
           '_ZN2nn2fs15CreateDirectoryEPKc', '_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci',
           '_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl',
           '_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE', '_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc',
           '_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE', '_ZN2nn2fs10RenameFileEPKcS2_',
           '_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc', '_ZN2nn2fs6CommitEPKc', '_ZN2nn2fs7UnmountEPKc'}
LANGUAGE = {'_ZN2nn2oe18GetDesiredLanguageEv', '_ZN2nn8settings6detail15GetLanguageCodeEPNS0_12LanguageCodeE'}
# The imports the code the hooks reach may call while the game runs (reach()). Each reviewed for what it touches: the
# libc ones above (the caller's buffers and the game's heap), SNPrintf (the caller's buffer), the sdk's mutexes and
# thread and tick queries (the game's threads use the same ones), the host file functions above and the language
# (runtime/ov_system.c). Nothing that talks to a service or changes memory mappings.
REACH_IMPORTS = SAFE_LIBC | {SNPRINTF} | OS_MUTEX | OS_TIME | HOST_FS | LANGUAGE
# Besides the lists above, a plugin may call what the game's main imports (game_imports): the game runs on those paths,
# so the runtime serves them, through its overrides or the lifted sdk, on every thread. Left out are the imports the
# game calls once at boot or not at all in play, where a second call from a plugin would meet a path nothing has run:
# the module loader (nn::ro), the C runtime's start, the thread-local allocator hook and the shader compiler.
GAME_IMPORTS_LEFT_OUT = re.compile(r'_ZN2nn2ro|__nnmusl_init|SetMemoryAllocatorForThreadLocal|^glslc')
# svcs the code the hooks reach may make: OutputDebugString (the log), GetSystemTick and SleepThread, which the
# runtime serves to every game thread the same way, and Break, an abort or a failed assert, which ends the game with
# the reason in the log as it ends it on the Switch (runtime/svc.c).
REACH_SVCS = {0x0B, 0x1E, 0x26, 0x27}
# exlaunch's inline-hook entry points. Each saves the registers its context holds, calls the callback with that context
# and restores them; none saves NZCV (recomp/flags.py). exl_inline_hook_with_fp_impl (Magic of the Kingdom) saves
# d0..d15 next to x0..x29.
HOOK_IMPLS = ('exl_inline_hook_impl', 'exl_inline_float_hook_impl', 'exl_inline_hook_with_fp_impl')
STUR_X30 = 0xF81F03FE   # stur x30, [sp, #-0x10]
LDUR_X30 = 0xF85F03FE   # ldur x30, [sp, #-0x10]
STUB_WORDS = 6          # 4 instructions and the callback's .quad
TICK_OFFSET = 0x0123456789AB
# An init that calls into main where the lift has no function start (Zonai Fieldworks calls main+0x481918 of 1.4.3,
# the second word of a function): the runtime's fatal names the address (runtime/dispatch.c dispatch_fail).
MID_CALL = re.compile(r'no function at guest pc 0x[0-9a-f]+ \(main\+0x([0-9a-f]+)\): not a function start')


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
    view['pre_images'] = {k: v for k, v in rec.get('pre_images', {}).items() if k in names}
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


# exlaunch's trampolines: its relocation of a hooked instruction (exlaunch source/lib/hook/nx64/hook_impl.cpp,
# __fix_instructions and the __fix_* helpers it calls, after And64InlineHook, MIT). A PC-relative original whose target
# the trampoline cannot reach in one word becomes a long form that keeps the target in a .quad; exlaunch puts one nop
# before the form when the .quad would not be 8-byte aligned. The generated code never runs a trampoline (an inline
# hook's site calls the stub and runs main's own original after it, recomp/patches.py @call; a replace hook's Orig
# reaches an unpatched copy of the function, `orig`), so these forms only tell an exlaunch hook from other code.
NOP = 0xD503201F
LDR_X17_8 = 0x58000051   # ldr x17, #8
LDR_X17_12 = 0x58000071  # ldr x17, #12
ADR_X30_16 = 0x1000009E  # adr x30, #16
BR_X17 = 0xD61F0220      # br x17


def quad(word, a):
    """The .quad at absolute a of word(a) (the plugin word there, None outside its .text), else None."""
    lo, hi = word(a), word(a + 4)
    return None if lo is None or hi is None else lo | hi << 32


def literal_end(word, orig, at):
    """End of exlaunch's relocation of the load-literal original at `at` (__fix_loadlit), else None. A prfm is dropped
    (no words); an ldr or ldrsw becomes the same load from #8 and a b over the literal's bytes, which follow it aligned
    to their size by leading nops. The bytes are not compared: the record holds no image of main."""
    simd, opc = orig >> 26 & 1, orig >> 30
    if not simd and opc == 3:
        return at
    size = (4, 8, 16)[opc] if simd else (4, 8, 4)[opc]
    p = at
    while word(p) == NOP and p < at + 12:
        p += 4
    if (p + 8) % size or word(p + 4 + size) is None:
        return None
    if word(p) != (orig & 0xFF00001F) | 2 << 5 or word(p + 4) != 0x14000001 + size // 4:
        return None
    return p + 8 + size


def relocation_end(word, orig, site, at):
    """End (absolute) of exlaunch's relocation of the original `orig` at `site` when it starts at `at`, else None:
    the same word or a PC-relative one re-encoded to the same target (one word), or one of the long forms:
      b.cond / cbz / tbz:  <the same with offset 8>; b #20; ldr x17, #8; br x17; .quad <target>
      b:                   ldr x17, #8; br x17; .quad <target>
      bl:                  ldr x17, #12; adr x30, #16; br x17; .quad <target>
      adr / adrp:          ldr <rd>, #8; b #12; .quad <its value>
      ldr / ldrsw literal: literal_end
    word(a) is the plugin word at absolute a, None outside its .text."""
    w0 = word(at)
    if w0 is None:
        return None
    if relocated(orig, site, w0, at):
        return at + 4
    rel = pc_relative(orig, site)
    if rel is None:
        return None
    (kind, _), target = rel
    if kind == 'ldr':
        return literal_end(word, orig, at)
    p = at + 4 if w0 == NOP else at
    w = [word(p + 4 * i) for i in range(4)]
    targets = {target}
    if kind in ('b.cond', 'cbz', 'tbz'):
        # exlaunch's __fix_cond_comp_test_branch takes the offset field without its sign: a backward target lands
        # 2^21 (b.cond, cbz) or 2^16 (tbz) bytes too far. Challenge Mode's trampolines hold such .quads; the generated
        # code runs main's own branch instead, to its real target.
        targets.add(site + ((orig >> 5 & (0x3FFF if kind == 'tbz' else 0x7FFFF)) << 2))
        form, data = pc_relative(w[0], p) == (rel[0], p + 8) and w[1:] == [0x14000005, LDR_X17_8, BR_X17], p + 16
    elif kind == 'b' and orig >> 31:
        form, data = w[:3] == [LDR_X17_12, ADR_X30_16, BR_X17], p + 12
    elif kind == 'b':
        form, data = w[:2] == [LDR_X17_8, BR_X17], p + 8
    else:   # adr, adrp
        form, data = w[:2] == [0x58000040 | orig & 0x1F, 0x14000003], p + 8
    return data + 8 if form and data % 8 == 0 and quad(word, data) in targets else None


def returns_to(word, at, back):
    """The trampoline jumps back to `back` from `at`: b <back>, or out of its range the nop-aligned
    ldr x17, #8; br x17; .quad <back> (exlaunch __fix_instructions)."""
    w = word(at)
    if w is not None and w >> 26 == 0x05 and branch_target(w, at) == back:
        return True
    p = at + 4 if w == NOP else at
    return p % 8 == 0 and word(p) == LDR_X17_8 and word(p + 4) == BR_X17 and quad(word, p + 8) == back


def inline_stub(img, o, p):
    """The words at image offset o of plugin p have exlaunch's inline-hook entry shape: stur x30; bl; ldur x30; b;
    and the .quad after them lies inside the plugin's .text."""
    if o + 4 * STUB_WORDS > p['text_end']:
        return False
    s = struct.unpack_from('<4I', img, o)
    return s[0] == STUR_X30 and s[2] == LDUR_X30 and s[1] >> 26 == 0x25 and s[3] >> 26 == 0x05


def text_words(img, p):
    """word(a): the word of plugin p's .text image img at absolute a, None outside that .text."""
    base, end = p['base'], p['text_end']
    return lambda a: struct.unpack_from('<I', img, a - base)[0] if base <= a <= base + end - 4 else None


def orig_trampoline(img, p, original, site):
    """Image offset of code in the plugin's .text that runs the site's original instruction relocated, then jumps back
    to site+4 (exlaunch's Orig trampoline of a replace hook, relocation_end and returns_to), else None."""
    base, word = p['base'], text_words(img, p)
    for o in range(0, p['text_end'] - 4, 4):
        end = relocation_end(word, original, site, base + o)
        if end is not None and returns_to(word, end, site + 4):
            return o
    return None


# ---- records
def load_record(d):
    """record.json of a service run directory, with the images of its plugins as bytes (key 'images') and their
    images from before the init (key 'pre_images', where the run wrote one)."""
    d = Path(d)
    rec = json.loads((d / 'record.json').read_text(encoding='utf-8'))
    rec['images'], rec['pre_images'] = {}, {}
    for m in rec['modules']:
        for k in ('base', 'size', 'text_end', 'data_off', 'init'):
            m[k] = u(m[k])
        if m.get('image'):
            rec['images'][m['name']] = (d / m['image']).read_bytes()
        if m.get('pre_image') and (d / m['pre_image']).is_file():
            rec['pre_images'][m['name']] = (d / m['pre_image']).read_bytes()
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


def game_imports(exefs):
    """The imports of the game's main in the ExeFS folder exefs, without GAME_IMPORTS_LEFT_OUT; empty without one."""
    main = Path(exefs) / 'main' if exefs else None
    if not main or not main.is_file():
        return frozenset()
    return frozenset(s[0] for s in Nso(main).imports() if not GAME_IMPORTS_LEFT_OUT.search(s[0]))


def check_record(rec, impls, game=frozenset()):
    """(hooks, reasons): one dict per hook site, and one line per broken rule (no lines: the record is accepted).
    game: the game's imports (game_imports), which the init may call besides ALLOWED_IMPORTS."""
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
        if calls and name not in ALLOWED_IMPORTS and name not in game:
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
        if w['module'] != 'main' or not main or w['offset'] & 3:
            reasons.append(f'a write outside main: {where} {w["old"]:#010x} -> {w["new"]:#010x}')
            continue
        if w['offset'] >= main['text_end']:
            # main's .data and .bss: the init runs at every start before the game's constructors and leaves these
            # words as it left them here (runtime/plugin_init.c check_writes keeps data pages, compared word by word
            # with the expect lines). .rodata holds what the lift read as constants (the jump tables), so no write
            # goes there. A word that makes a pointer into a plugin's .text (a table entry swapped for a plugin
            # function: Zonai Fieldworks, Material Columns) names a function start of that plugin's lift.
            if w['offset'] < main['data_off']:
                reasons.append(f"a write into main's .rodata: {where} {w['old']:#010x} -> {w['new']:#010x}")
                continue
            hook = {'site': f'main+{w["offset"]:#x}', 'original': f'{w["old"]:#010x}', 'kind': 'data',
                    'new': f'{w["new"]:#010x}'}
            if w['offset'] % 8 == 0:
                p, _ = plugin_word(main['base'] & ~0xFFFFFFFF | w['new'])
                if p is not None:
                    hook['target'] = f'{p["name"]}+{(main["base"] & ~0xFFFFFFFF | w["new"]) - p["base"]:#x}'
            hooks.append(hook)
            continue
        site = main['base'] + w['offset']
        stub = branch_target(w['new'], site)
        link = bool(w['new'] >> 31)
        if stub is None:
            reasons.append(f'{where}: the new word {w["new"]:#010x} is no b or bl')
            continue
        if w['old'] & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000):   # br, ret: the site would end its function
            reasons.append(f'{where}: the original {w["old"]:#010x} is a br or ret')
        p, _ = plugin_word(stub)
        if p is None:
            reasons.append(f"{where}: {'bl' if link else 'b'} {stub:#x} does not reach into a plugin's .text")
            continue
        exl = inline_stub(rec['images'][p['name']], stub - p['base'], p)
        if link and exl:   # an inline stub is entered by b: it saves x30 itself
            reasons.append(f"{where}: bl {p['name']}+{stub - p['base']:#x} into an inline-hook stub")
            continue
        if not exl:
            # A b or bl straight into the plugin's code: a site whose replacement is that branch, its target a
            # function start of the plugin's lift (a seed). A bl returns to the site + 4 inside the lifted function
            # (Quick Weapon Durability's call-site rewrite). A b leaves main's function: at a function start it
            # replaces it (Full Inventory Expansion's Sort Fixer; exlaunch keeps the first instruction relocated +
            # b site+4 for the callback's Orig), inside one it is a far hook (Zonai Fieldworks' veneers and gated
            # edits). Where the plugin's code comes back into main, entries() makes a function start of the lift.
            o = stub - p['base']
            kind = 'call' if link else 'replace' if w.get('fn_start') else 'branch'
            hook = {'site': f'main+{w["offset"]:#x}', 'original': f'{w["old"]:#010x}', 'kind': kind,
                    'target': f'{p["name"]}+{o:#x}'}
            # the Orig trampoline of a replace hook (an `orig` line); a far hook's way back is an entry instead
            orig = orig_trampoline(rec['images'][p['name']], p, w['old'], site) if kind == 'replace' else None
            if orig is not None:
                hook['trampoline'] = f'{p["name"]}+{orig:#x}'
            hooks.append(hook)
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
        tp, t0 = plugin_word(tramp) if tramp is not None else (None, None)
        if tp is None:
            reasons.append(f'{where}: the trampoline {tramp:#x} is outside a plugin\'s .text')
            continue
        word = text_words(rec['images'][tp['name']], tp)
        end = relocation_end(word, w['old'], site, tramp)
        hook['relocated'] = f'{t0:#010x}' + (f' +{(end - tramp) // 4 - 1} words' if end and end != tramp + 4 else '')
        if end is None:
            reasons.append(f'{where}: the trampoline {hook["trampoline"]} starts with {t0:#010x}, not the original '
                           f'{w["old"]:#010x} relocated')
        elif not returns_to(word, end, site + 4):
            back = word(end)
            reasons.append(f'{where}: the trampoline\'s word after the relocated original, '
                           f'{back if back is not None else 0:#010x} at {tp["name"]}+{end - tp["base"]:#x}, is no b '
                           f'to the site + 4')
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


def veneer_slot(img, base, text_end, a):
    """Image offset of the pointer a far-jump veneer at a goes through (adrp x17, page; ldr x17, [x17, #off];
    br x17: Zonai Fieldworks' hook pads), else None."""
    o = a - base
    if not 0 <= o <= text_end - 12:
        return None
    w0, w1, w2 = struct.unpack_from('<3I', img, o)
    if w0 & 0x9F00001F != 0x90000011 or w1 & 0xFFC003FF != 0xF9400231 or w2 != 0xD61F0220:
        return None
    imm = sext((w0 >> 29 & 3) | (w0 >> 5 & 0x7FFFF) << 2, 21)
    slot = (a & ~0xFFF) + (imm << 12) + (w1 >> 10 & 0xFFF) * 8 - base
    return slot if 0 <= slot <= len(img) - 8 else None


def reach(img, base, text_end, roots, plt, stubs=()):
    """What the code from roots (absolute addresses in the plugin's .text) reaches by falling through, direct branches
    and direct calls: (svc immediates, {import: calls}, indirect calls, words outside the .text it branched to,
    veneer targets). A stub's b to its trampoline counts as its return (recomp/patches.py `stub`). A veneer goes on to
    the pointer its slot holds after the init (the plugin may change it later, to code its own lift discovers)."""
    svcs, imports, indirect, outside, veneers = set(), {}, 0, set(), set()
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
            slot = veneer_slot(img, base, text_end, a)
            if slot is not None:
                a = struct.unpack_from('<Q', img, slot)[0]
                veneers.add(a)
                continue
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
    return svcs, imports, indirect, outside, veneers


def reach_check(rec, hooks, impls, game=frozenset()):
    """Reasons the code the hooks reach breaks the rules (svcs, imports), and a summary line. game: the game's imports
    (game_imports), which that code may call besides REACH_IMPORTS."""
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
        roots |= init_code(rec, hooks, p['name'])
        nso = Nso(p['file']) if p.get('file') and Path(p['file']).is_file() else None
        plt = plt_imports(nso, img, base, p['text_end']) if nso else {}
        svcs, imports, indirect, outside, _ = reach(img, base, p['text_end'], sorted(roots), plt, stubs)
        for imm in sorted(svcs - REACH_SVCS):
            reasons.append(f'the code its hooks reach has svc {imm:#x}')
        for name in sorted(set(imports) - REACH_IMPORTS - game):
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
    """The hooks whose stub or target lies in the plugin module: its own, when one record holds several plugins. A
    data write without a target belongs to the plugin whose init ran (each record is one plugin's, TOTK_PLUGIN_ONLY)."""
    return [h for h in hooks if (h.get('stub') or h.get('target') or module).split('+')[0] == module]


def init_code(rec, hooks, module):
    """Absolute starts of the code module's init wrote into its own .text where the file had zeros: exlaunch's
    trampolines of its far hooks (the moved original words and a branch back). The plugin may point a veneer at any of
    them at play (Zonai Fieldworks turns its features' hooks on and off), so each is a root of the reach check and a
    seed of the lift. The trampolines of inline hooks (never run) and the Orig trampolines of replace hooks (the lift
    serves them with the function's unpatched copy) are left out: their way back would split main's functions."""
    p = {m['name']: m for m in rec['modules']}[module]
    img, pre = rec['images'].get(module), rec.get('pre_images', {}).get(module)
    if img is None or pre is None:
        return set()
    n = p['text_end'] // 4
    now, old = struct.unpack_from(f'<{n}I', img), struct.unpack_from(f'<{n}I', pre)
    known = {p['base'] + int(h['trampoline'].split('+')[1], 16) for h in hooks
             if (h.get('trampoline') or '').startswith(module + '+')}
    out, inside = set(), False   # inside: the word before is a written word that goes on to this one
    for i in range(n):
        new = old[i] == 0 and now[i] != 0
        if new and not inside:
            out.add(p['base'] + 4 * i)
        # b, br, ret end a trampoline; one may follow right after
        inside = new and now[i] & 0xFC000000 != 0x14000000 and now[i] & 0xFFFFFC1F not in (0xD61F0000, 0xD65F0000)
    return out - known


def entries(rec, hooks, module):
    """(main offsets, plugin offsets): where module's hook code comes back into main's .text, which the lift makes
    function starts (recomp/patches.py `entry`), and the code its veneers lead to, seeds of its lift. The site + 4 of a
    far hook, and of a replace hook without an Orig trampoline, is one either way: the plugin may point the site's
    veneer at its own code later (Zonai Fieldworks turns a feature's hooks on and off at play), and that code comes
    back there too."""
    mods = {m['name']: m for m in rec['modules']}
    main, p = mods['main'], mods[module]
    out = {int(h['site'].split('+')[1], 16) + 4 for h in hooks
           if h['kind'] == 'branch' or h['kind'] == 'replace' and not h.get('trampoline')}
    base, img = p['base'], rec['images'][module]
    roots = {base + int(h['target'].split('+')[1], 16) for h in hooks
             if h['kind'] in ('branch', 'call', 'data', 'replace') and h.get('target', '').startswith(module + '+')}
    tramps = init_code(rec, hooks, module)
    roots |= tramps
    nso = Nso(p['file']) if p.get('file') and Path(p['file']).is_file() else None
    plt = plt_imports(nso, img, base, p['text_end']) if nso else {}
    _, _, _, outside, veneers = reach(img, base, p['text_end'], sorted(roots), plt)
    out |= {a - main['base'] for a in outside if main['base'] <= a < main['base'] + main['text_end'] and a % 4 == 0}
    return sorted(out), sorted(v - base for v in veneers | tramps if base <= v < base + p['text_end'])


def plugin_lines(rec, hooks, name, record_dir, module):
    """The patch lines of the accepted plugin module (recomp/patches.py) for set name: the plugin line, the expected
    writes (the words at its hooks' sites), the stubs, the seeds, the orig lines and the hook sites."""
    lines = []
    mods = {m['name']: m for m in rec['modules']}
    p = mods[module]
    img = rec['images'][p['name']]
    text_sha = hashlib.sha256(img[:p['text_end']]).hexdigest()
    image = json.dumps((Path(record_dir) / p['image']).resolve().as_posix(), ensure_ascii=False)
    lines.append(f'plugin {name} {p["name"]} {text_sha} {image}')
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
        elif h['kind'] == 'data':
            if h.get('target', '').startswith(module + '+'):
                seeds.setdefault(module, set()).add(at(h['target'])[1])
        else:
            tm, to = at(h['target'])
            seeds.setdefault(tm, set()).add(to)
            new = next(w['new'] for w in rec['writes'] if w['module'] == 'main' and w['offset'] == site)
            lines.append(f'site {name} main {site:x} {u(h["original"]):08x} {new:08x}')
            if h['kind'] == 'replace' and h.get('trampoline'):
                rm, ro = at(h['trampoline'])
                lines.append(f'orig {name} {rm} {ro:x} main {site:x}')
    back, ahead = entries(rec, hooks, module)
    if ahead:
        seeds.setdefault(module, set()).update(ahead)
    for m in sorted(stubs):
        lines += [f'stub {m} {o:x}' for o in sorted(stubs[m])]
    for m in sorted(seeds):
        lines.append(f'seed {m} ' + ' '.join(f'{o:x}' for o in sorted(seeds[m])))
    if back:
        lines.append('entry main ' + ' '.join(f'{o:x}' for o in back))
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
    text = args.patches.read_text(encoding='utf-8') if args.patches else ''
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
    args.patches_out.write_text(text, encoding='utf-8')
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


def report(rec, label, game=frozenset()):
    rec = ran_view(rec)
    impls = hook_impls(rec)
    hooks, reasons = check_record(rec, impls, game)
    more, notes = reach_check(rec, hooks, impls, game)
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
                  f'{h["callback"]}, trampoline {h["trampoline"]} ({h.get("relocated")}; back to site+4)')
        elif h['kind'] == 'data':
            print(f'  data {h["site"]} {h["original"]} -> {h["new"]}' +
                  (f' (points at {h["target"]})' if h.get('target') else ''))
        else:
            print(f'  {h["kind"]} {h["site"]} (was {h["original"]}) -> {"bl" if h["kind"] == "call" else "b"} '
                  f'{h["target"]}' + (f', Orig trampoline {h["trampoline"]}' if h.get('trampoline') else ''))
    for r in reasons:
        print(f'  REFUSED: {r}')
    if not reasons:
        print(f'  accepted: {len(hooks)} hooks')
    return hooks, reasons


def plugin_runs(exe, out, module, timeout, game=frozenset()):
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
            hooks, bad = report(recs[0], f'{module} run 1', game)
            return recs[0], hooks, bad
    hooks, bad = report(recs[0], f'{module} run 1', game)
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
    ap.add_argument('--exefs', type=Path, help="the game's ExeFS folder: a plugin may call what its main imports")
    ap.add_argument('--patches', type=Path, help='the patch file so far')
    ap.add_argument('--patches-out', type=Path, help='the patch file with the plugins\' sets added')
    ap.add_argument('--status', type=Path, help='the plugin status file (build-cache/plugins.status)')
    ap.add_argument('--entries-out', type=Path, help='file for `entry main` lines: addresses of main an init called '
                    'that the lift has no function start for; a lift with them lets the init go on')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    emitting = bool(args.patches_out or args.status)
    if emitting and not (args.mods and args.patches_out and args.status):
        ap.error('--mods, --patches-out and --status go together')
    if args.check:
        rec = load_record(args.check)
        hooks, bad = report(rec, str(args.check), game_imports(args.exefs))
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
    game = game_imports(args.exefs)
    results = []
    for m in (m for m in listing['modules'] if m['plugin']):
        rec, hooks, bad = plugin_runs(args.exe, args.out, m['name'], args.timeout, game)
        print(f'{m["name"]}: ' + (f'refused: {bad[0]}' if bad else f'accepted, {len(hooks)} hooks'))
        results.append((m['name'], m.get('file'), rec, args.out / m['name'] / 'run1', hooks, bad))
    if emitting:
        emit(results, args)
    if args.entries_out:
        found = sorted({int(m.group(1), 16) for r in results for why in r[5] for m in MID_CALL.finditer(why)})
        args.entries_out.write_text(''.join(f'entry main {o:x}\n' for o in found), encoding='utf-8')
        if found:
            print('plugins: inits call main where the lift has no function start: ' +
                  ', '.join(f'main+{o:#x}' for o in found))
    return 1 if any(r[5] for r in results) else 0


if __name__ == '__main__':
    sys.exit(main())
