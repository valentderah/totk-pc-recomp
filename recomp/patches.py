"""Code patch table for the generator: guest instructions replaced in the lifted C, switchable at run time.

Lifted code cannot be byte-patched when the game runs, so a patch is applied when the C is generated. A patch file
(gen.py --patches) has these kinds of lines; `#` starts a comment, fields are separated by whitespace:

  set  <name> <env> <default>               a patch set: a run-time switch <env>=0/1, <default> when it is unset
  site <set> <module> <address> <original> <replacement> [<replacement> ...]

  spin <module> <head> <back-edge> [<back-edge> ...]

  hook <name> <handler> [<argument> ...]    a host hook: C code of the runtime that a site's replacement calls

<address> is a module offset (hex), <original> the instruction word the module has there (checked; a mismatch stops
the generator), each <replacement> an instruction word (hex), `@hook:<name>` (below) or `@page:x<n>`, which is
`adrp x<n>, <patch page>` encoded for the site's address. The patch page is the first 4 KiB page after main's image
(layout.py leaves the rest of the 2 MiB-aligned module slot free); the runtime commits it and owns its contents. Words
in use, by byte offset:
+0..15 the frame-rate step (runtime/frame_rate.c), +16..55 the output, swapchain and scene sizes
(runtime/resolution.c res_init), +56..59 the camera's aspect ratio (runtime/resolution.c res_init, set aspect), +60..63
the field-of-view factor (runtime/resolution.c res_fov_live, set fov), +92..95 the sun shadow map size
(runtime/resolution.c shadow_init, set shadow). A new set takes words outside these.

The replacement words are lifted at the site's address in place of the original instruction, and the generated C reads
`if (rt_patch_on[<set>]) { replacement } else { original }`: one byte load and a branch per executed site. Several
sets may patch one address (cheats that write different values there): `if (a) {..} else if (b) {..} else {..}`, the
first set in file order wins. The original may be any instruction but br, blr and ret; a replacement's last word may be
a bl (lifted with the site's return address), a b or b.cond / cbz / tbz (its target joins the function's labels when
inside it) or a ret; earlier words must not branch. Every alternative of a site writes the same flags. The generator
writes patches.h (set names, switches, the page address) next to layout.h for the runtime.

A `spin` line names a spin wait found by tools/spin_waits.py: a loop with head <head> that only another thread can end,
and the direct branches back to it (module offsets). Each back-edge (checked: a b, b.cond, cbz / cbnz or tbz / tbnz to
<head>) is lifted as `rt_spin_gate(c, <head>); goto <head>`, which keeps every register and only adds time
(runtime/kernel.c kern_spin_gate).

A `hook` line names C code of the runtime for sites to call: <handler> is a name runtime/plugins.c rt_hook_bind knows,
and up to HOOK_ARGS arguments go with it, each a hex number or `fn:<module>+<offset>`, a function of a module of the
layout: its absolute address is the argument, and the generator makes the address a function start, so the handler
can call it with rt_call. A hook line comes before the sites that use it. The replacement word `@hook:<name>` is lifted
as `rt_hook(c, <index of the hook>, <site address>)` with every register stored to the Cpu before and loaded again
after (as around an SVC), so the handler sees and may change the whole guest state; the flags count as neither read
nor written, and the handler keeps them. `@hook:<name> <original>` runs the hook and then the instruction, as an
exlaunch inline hook does. The hooks go to patches.h (RT_HOOKS), in file order.
"""
import re
from pathlib import Path

PAGE = 0x1000
HOOK_ARGS = 4   # arguments of a hook (runtime/host.h RtHook)
NAME = re.compile(r'[A-Za-z0-9_.]+')   # hook and handler names: they become C strings of patches.h


class PatchError(ValueError):
    pass


def adrp(pc, target, rd):
    imm = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= imm < (1 << 20):
        raise PatchError(f'{pc:#x}: patch page {target:#x} out of ADRP range')
    imm &= 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


class Table:
    """sets: [(name, env, default)]; sites: {module: {offset: [(set index, [word | ('page', reg) | ('hook', index)],
    original)]}} (the alternatives of a site in file order); page; hooks: [(name, handler, [arguments])] in file order,
    a fn: argument as its absolute address; entries: {module: {offset}}, the functions fn: arguments name."""

    def __init__(self, sets=(), sites=None, page=0, spins=None, hooks=None, entries=None):
        self.sets = list(sets)
        self.sites = sites or {}
        self.page = page
        self.spins = spins or {}   # {module: {back-edge offset: head offset}}
        self.hooks = list(hooks or [])
        self.entries = entries or {}

    def spins_for(self, name, base):
        """{absolute back-edge address: absolute head address}."""
        return {base + b: base + h for b, h in self.spins.get(name, {}).items()}

    def entries_for(self, name, base):
        """Absolute addresses the hooks' fn: arguments name in the module: function starts for discovery."""
        return sorted(base + o for o in self.entries.get(name, ()))

    def for_module(self, name, base):
        """{absolute address: [(set index, [words])]} with @page resolved; a hook stays ('hook', index)."""
        out = {}
        for off, alts in self.sites.get(name, {}).items():
            a = base + off
            out[a] = [(k, [adrp(a, self.page, w[1]) if isinstance(w, tuple) and w[0] == 'page' else w for w in repl])
                      for k, repl, _ in alts]
        return out


def hook_argument(token, bases, entries, where):
    """The value of one argument of a hook line: a hex number, or `fn:<module>+<offset>` (recorded in entries)."""
    if not token.startswith('fn:'):
        return int(token, 16)
    mod, plus, off = token[3:].partition('+')
    if mod not in bases or not plus:
        raise PatchError(f'{where}: {token} is not fn:<module of the layout>+<offset>')
    o = int(off, 16)
    if not 0 <= o < bases[mod][1] or o % 4:
        raise PatchError(f'{where}: {token} is outside its module or misaligned')
    entries.setdefault(mod, set()).add(o)
    return bases[mod][0] + o


def load(path, mods):
    """Table from a patch file; mods is layout() output [(name, base, size)]."""
    sets, index, sites, spins, hooks, hook_index, entries = [], {}, {}, {}, [], {}, {}
    bases = {n: (b, s) for n, b, s in mods}
    for no, raw in enumerate(Path(path).read_text().splitlines(), 1):
        f = raw.split('#', 1)[0].split()
        if not f:
            continue
        where = f'{path}:{no}'
        if f[0] == 'set' and len(f) == 4:
            if f[1] in index:
                raise PatchError(f'{where}: set {f[1]} defined twice')
            index[f[1]] = len(sets)
            sets.append((f[1], f[2], int(f[3])))
        elif f[0] == 'site' and len(f) >= 6:
            if f[1] not in index:
                raise PatchError(f'{where}: unknown set {f[1]}')
            if f[2] not in bases:
                raise PatchError(f'{where}: module {f[2]} not in the layout')
            off, orig = int(f[3], 16), int(f[4], 16)
            repl = []
            for w in f[5:]:
                if w.startswith('@page:x'):
                    repl.append(('page', int(w[7:])))
                elif w.startswith('@hook:'):
                    if w[6:] not in hook_index:
                        raise PatchError(f'{where}: unknown hook {w[6:]} (its hook line comes first)')
                    repl.append(('hook', hook_index[w[6:]]))
                else:
                    repl.append(int(w, 16))
            alts = sites.setdefault(f[2], {}).setdefault(off, [])
            if any(k == index[f[1]] for k, _, _ in alts):
                raise PatchError(f'{where}: {f[2]}+{off:#x} patched twice by set {f[1]}')
            if alts and alts[0][2] != orig:
                raise PatchError(f'{where}: {f[2]}+{off:#x} expects {orig:08x}, an earlier line {alts[0][2]:08x}')
            alts.append((index[f[1]], repl, orig))
        elif f[0] == 'spin' and len(f) >= 4:
            if f[1] not in bases:
                raise PatchError(f'{where}: module {f[1]} not in the layout')
            mod, head = spins.setdefault(f[1], {}), int(f[2], 16)
            for b in f[3:]:
                b = int(b, 16)
                if b in mod:
                    raise PatchError(f'{where}: {f[1]}+{b:#x} is a back-edge twice')
                mod[b] = head
        elif f[0] == 'hook' and len(f) >= 3:
            if not NAME.fullmatch(f[1]) or not NAME.fullmatch(f[2]):
                raise PatchError(f'{where}: a hook or handler name holds other characters than A-Z a-z 0-9 _ .')
            if f[1] in hook_index:
                raise PatchError(f'{where}: hook {f[1]} defined twice')
            if len(f) - 3 > HOOK_ARGS:
                raise PatchError(f'{where}: more than {HOOK_ARGS} arguments')
            hook_index[f[1]] = len(hooks)
            hooks.append((f[1], f[2], [hook_argument(t, bases, entries, where) for t in f[3:]]))
        else:
            raise PatchError(f'{where}: cannot parse {raw!r}')
    base, size = bases['main'][0], bases['main'][1]
    page = (base + size + PAGE - 1) & ~(PAGE - 1)
    nxt = min((b for _, b, _ in mods if b > base), default=None)
    if nxt is not None and page + PAGE > nxt:
        raise PatchError(f'no room for the patch page after main ({page:#x})')
    return Table(sets, sites, page, spins, hooks, entries)


def check(table, mod, name):
    """Stops on a site whose original word differs from the module's, a spin back-edge that is no branch to its head
    and a hook argument naming a function outside .text."""
    for off, [(_, _, want), *_] in table.sites.get(name, {}).items():
        a = mod.base + off
        if not mod.in_text(a):
            raise PatchError(f'{name}+{off:#x}: patch site outside .text')
        have = mod.word(a)
        if have != want:
            raise PatchError(f'{name}+{off:#x}: module has {have:08x}, the patch expects {want:08x}')
    for b, head in table.spins.get(name, {}).items():
        a = mod.base + b
        if not mod.in_text(a) or branch_target(mod.word(a), a) != mod.base + head:
            raise PatchError(f'{name}+{b:#x}: not a direct branch to the spin wait head {name}+{head:#x}')
    for off in sorted(table.entries.get(name, ())):
        if not mod.in_text(mod.base + off):
            raise PatchError(f'{name}+{off:#x}: a hook argument names a function outside .text')


def branch_target(w, a):
    """Target of the direct branch w at a (b, b.cond, cbz / cbnz, tbz / tbnz), or None."""
    if w >> 26 == 0x05:
        return a + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:
        return a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000 << 2)
    if w & 0x7E000000 == 0x36000000:
        return a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000 << 2)
    return None


def header_text(table, marker):
    def hook(i, name, handler, args):
        values = ', '.join(f'{v:#x}ull' for v in args + [0] * (HOOK_ARGS - len(args)))
        return f'X({i}, "{name}", "{handler}", {len(args)}, {values})'

    lines = [f'// {marker}: code patch sets, generated by recomp/gen.py --patches. Do not edit or commit.',
             '#pragma once', '', f'#define RT_PATCH_COUNT {len(table.sets)}',
             f'#define RT_PATCH_PAGE {table.page:#x}ull',
             '// X(index, name, env switch, default)',
             '#define RT_PATCH_SETS(X) ' + ' '.join(f'X({i}, {n}, "{e}", {d})' for i, (n, e, d) in
                                                    enumerate(table.sets)),
             f'#define RT_HOOK_COUNT {len(table.hooks)}',
             f'// X(index, name, handler, argument count, {HOOK_ARGS} arguments): the hook lines, fn: arguments '
             f'as absolute addresses',
             '#define RT_HOOKS(X)' + ''.join(' ' + hook(i, *h) for i, h in enumerate(table.hooks)), '']
    return '\n'.join(lines)
