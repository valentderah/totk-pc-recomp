"""Code patch table for the generator: guest instructions replaced in the lifted C, switchable at run time.

Lifted code cannot be byte-patched when the game runs, so a patch is applied when the C is generated. A patch file
(gen.py --patches) has these kinds of lines; `#` starts a comment, fields are separated by whitespace:

  set  <name> <env> <default> [ordered]     a patch set: a run-time switch <env>=0/1, <default> when it is unset
  site <set> <module> <address> <original> <replacement> [<replacement> ...]

  spin <module> <head> <back-edge> [<back-edge> ...]

  hook <module> <function> [<function> ...]

  plugin <set> <module> <text sha256> <image>
  expect <set> <module> <offset> <old> <new>
  stub <module> <offset>
  seed <module> <offset> [<offset> ...]
  orig <set> <module> <offset> <function module> <function offset>

<address> is a module offset (hex), <original> the instruction word the module has there (checked; a mismatch stops
the generator), each <replacement> an instruction word (hex) or `@page:x<n>`, which is `adrp x<n>, <patch page>`
encoded for the site's address. The patch page is the first 4 KiB page after main's image (layout.py leaves the rest
of the 2 MiB-aligned module slot free); the runtime commits it and owns its contents. Words in use, by byte offset:
+0..15 the frame-rate step (runtime/frame_rate.c), +16..55 the output, swapchain and scene sizes
(runtime/resolution.c res_init), +56..59 the camera's aspect ratio (runtime/resolution.c res_init, set aspect), +60..63
the field-of-view factor (runtime/resolution.c res_fov_live, set fov), +92..95 the sun shadow map size
(runtime/resolution.c shadow_init, set shadow). A new set takes words outside these.

The replacement words are lifted at the site's address in place of the original instruction, and the generated C reads
`if (rt_patch_on[<set>]) { replacement } else { original }`: one byte load and a branch per executed site. Several
sets may patch one address (cheats that write different values there): `if (a) {..} else if (b) {..} else {..}`, the
first set in file order wins.

An `ordered` set belongs to something the player puts in an order (a mod of the Mods tab: tools/exefs_patches.py marks
its sets so). A site that two or more ordered sets patch is contested: the run time, not the file order, picks which
of them runs. Each contested site gets a byte rt_patch_pick[<n>] (n counts the contested sites, by module name and
offset), and each of its ordered alternatives tests `rt_patch_pick[<n>] == <j>`, j being the alternative's place
among the site's ordered sets in file order (1 up; 0 runs none of them): one byte load for the whole group. The
runtime fills the bytes from the sets that are on and their ranks (runtime/patches.c rt_patch_picks: the lowest rank
wins, a tie goes to the set later in the file). The other alternatives of a contested site keep their place and
their rt_patch_on test, so a cheat before the mods' sets still wins while it is on. A site with at most one ordered
set, and a table without contested sites, is emitted exactly as without the keyword.

The original may be any instruction but br, blr and ret; a replacement's last word may be
a bl (lifted with the site's return address), a b or b.cond / cbz / tbz (its target joins the function's labels when
inside it) or a ret; earlier words must not branch. Every alternative of a site writes the same flags. The generator
writes patches.h (set names, switches, the page address, the contested sites) next to layout.h for the runtime.

A `spin` line names a spin wait found by tools/spin_waits.py: a loop with head <head> that only another thread can end,
and the direct branches back to it (module offsets). Each back-edge (checked: a b, b.cond, cbz / cbnz or tbz / tbnz to
<head>) is lifted as `rt_spin_gate(c, <head>); goto <head>`, which keeps every register and only adds time
(runtime/kernel.c kern_spin_gate).

The plugin lines carry a mod's exlaunch plugin (layout.py plugin<N>), whose init runs at every start
(runtime/plugin_init.c) and whose hooks are sites of its set (tools/exefs_plugins.py writes them from the plugin service
run's record; the design follows #845 by itaskaev2):
  - `plugin`: <module> belongs to the set <set>; it is lifted from <image>, the plugin's image after its init (the
    service run's plugin<N>.img), whose .text holds the hook stubs and trampolines the NSO file has as zeros. <text
    sha256> is that .text's hash: the runtime compares it with the .text its own run of the init leaves, and keeps the
    set off when they differ.
  - `expect`: one word the init writes into a game module (the hook branches into main's .text); the runtime compares
    the words its run of the init wrote with these, all or nothing per plugin.
  - `stub`: an exlaunch inline-hook entry stub (stur x30; bl impl; ldur x30; b trampoline; .quad callback). Its fourth
    word is lifted as a `ret`: the generated site calls the stub and runs its own original instruction after it
    (`@call` below), so the trampoline (the original relocated, b site+4) never runs.
  - `seed`: function starts of the module the lift must have (the callbacks, the hook impls, branch targets).
  - `orig`: the plugin calls its replace hook's original through the trampoline at <module>+<offset>; the generator
    lifts the function at <function module>+<function offset> a second time without its patched sites
    (fo_<address>) and lists that copy at the trampoline's address in <module>'s function table, so the call reaches
    the original code and not the hook again.
A replacement word `@call:<module>+<offset>` is a call of the function there that writes no x30: every register in
the Cpu at the call and all read back after it (the callback may change any of them, as exlaunch's hook context
allows); the flags stay as they were. It may stand before the site's last word, which is then the original
instruction.

A `hook` line names guest functions (module offsets of function starts; gen.py stops on any other address) whose
entry the runtime watches: their lifted C starts with FN_ENTER (runtime/cpu.h), a load of g_fnprof and a predicted
branch to fnprof_hit while the runtime sets it. A table with hook lines puts FN_ENTER into these functions only; a table
without any keeps it in every function, as does gen.py --fn-enter-all (a whole-game function-entry profile).
"""
from pathlib import Path

PAGE = 0x1000
PICK_MAX = 255   # ordered sets on one site: rt_patch_pick is a byte, 0 for none


class PatchError(ValueError):
    pass


def adrp(pc, target, rd):
    imm = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= imm < (1 << 20):
        raise PatchError(f'{pc:#x}: patch page {target:#x} out of ADRP range')
    imm &= 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


class Table:
    """sets: [(name, env, default)]; sites: {module: {offset: [(set index, [word | ('page', reg)], original)]}} (the
    alternatives of a site in file order); page; ordered: the indices of the ordered sets. What the generated C takes
    from here (for_module, picks_for, spins_for, hooks_for) is in the lift cache's key (recomp/gen.py chunk_key); a new
    kind of line the Emitter reads must go there too."""

    def __init__(self, sets=(), sites=None, page=0, spins=None, hooks=None, ordered=(), plugins=None):
        self.sets = list(sets)
        self.sites = sites or {}
        self.page = page
        self.spins = spins or {}   # {module: {back-edge offset: head offset}}
        self.hooks = hooks or {}   # {module: {function offset}}; empty: FN_ENTER in every function
        self.ordered = set(ordered)
        # {module: {'set', 'sha256', 'image', 'expect': [(module, offset, old, new)], 'stubs': {offset},
        #  'seeds': {offset}, 'origs': {trampoline offset: (function module, function offset)}}}; empty without
        # plugin lines
        self.plugins = plugins or {}
        self.bases = {}            # {module: base} of the layout load() read

    def plugin_inputs(self, name):
        """What the lift of module name takes from its plugin lines (for the lift cache's key), or None."""
        p = self.plugins.get(name)
        if not p:
            return None
        return [p['image'], p['sha256'], sorted(p['stubs']), sorted(p['seeds']), sorted(p['origs'])]

    def origs_for(self, name, base):
        """The absolute addresses of module name's functions that a plugin's `orig` line copies."""
        return {base + fo for p in self.plugins.values() for fm, fo in p['origs'].values() if fm == name}

    def orig_entries(self, name, bases):
        """{trampoline address in module name: absolute address of the function it reaches} (`orig`)."""
        p = self.plugins.get(name)
        if not p:
            return {}
        return {bases[name] + t: bases[fm] + fo for t, (fm, fo) in p['origs'].items()}

    def picks(self):
        """The contested sites, by module name and offset: [(module, offset, [ordered set index in file order])]; the
        list index is the site's rt_patch_pick byte."""
        out = []
        for name in sorted(self.sites):
            for off in sorted(self.sites[name]):
                sets = [k for k, _, _ in self.sites[name][off] if k in self.ordered]
                if len(sets) > 1:
                    if len(sets) > PICK_MAX:
                        raise PatchError(f'{name}+{off:#x}: {len(sets)} ordered sets, at most {PICK_MAX}')
                    out.append((name, off, sets))
        return out

    def picks_for(self, name, base):
        """{absolute address of a contested site: (its pick index, {ordered set index: the value that runs it})}."""
        return {base + off: (n, {k: j + 1 for j, k in enumerate(sets)})
                for n, (mod, off, sets) in enumerate(self.picks()) if mod == name}

    def hooks_for(self, name, base):
        """The absolute addresses of the module's hooked functions, or None when the table has no hook line at all
        (FN_ENTER in every function)."""
        if not self.hooks:
            return None
        return {base + o for o in self.hooks.get(name, ())}

    def spins_for(self, name, base):
        """{absolute back-edge address: absolute head address}."""
        return {base + b: base + h for b, h in self.spins.get(name, {}).items()}

    def for_module(self, name, base, bases=None):
        """{absolute address: [(set index, [words])]} with @page resolved; an @call word becomes ('call', absolute
        address), which needs bases ({module: base})."""
        out = {}

        def word(a, w):
            if not isinstance(w, tuple):
                return w
            if w[0] == 'page':
                return adrp(a, self.page, w[1])
            known = bases or self.bases
            if w[1] not in known:
                raise PatchError(f'{name}+{a - base:#x}: @call into {w[1]}, which is not in the layout')
            return ('call', known[w[1]] + w[2])

        for off, alts in self.sites.get(name, {}).items():
            a = base + off
            out[a] = [(k, [word(a, w) for w in repl]) for k, repl, _ in alts]
        return out


def load(path, mods):
    """Table from a patch file; mods is layout() output [(name, base, size)]."""
    sets, index, sites, spins, hooks, ordered, plugins = [], {}, {}, {}, {}, set(), {}
    bases = {n: (b, s) for n, b, s in mods}
    for no, raw in enumerate(Path(path).read_text().splitlines(), 1):
        f = raw.split('#', 1)[0].split()
        if not f:
            continue
        where = f'{path}:{no}'
        if f[0] == 'set' and (len(f) == 4 or len(f) == 5 and f[4] == 'ordered'):
            if f[1] in index:
                raise PatchError(f'{where}: set {f[1]} defined twice')
            index[f[1]] = len(sets)
            if len(f) == 5:
                ordered.add(len(sets))
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
                elif w.startswith('@call:') and '+' in w:
                    target, at = w[6:].split('+', 1)
                    if target not in bases:
                        raise PatchError(f'{where}: @call into {target}, which is not in the layout')
                    repl.append(('call', target, int(at, 16)))
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
        elif f[0] == 'plugin' and len(f) == 5:
            if f[1] not in index or f[2] not in bases:
                raise PatchError(f'{where}: unknown set {f[1]} or module {f[2]}')
            if f[2] in plugins:
                raise PatchError(f'{where}: module {f[2]} has two plugin lines')
            plugins[f[2]] = {'set': index[f[1]], 'sha256': f[3], 'image': f[4], 'expect': [], 'stubs': set(),
                             'seeds': set(), 'origs': {}}
        elif f[0] in ('expect', 'stub', 'seed', 'orig'):
            plugin_line(f, where, index, bases, plugins)
        elif f[0] == 'hook' and len(f) >= 3:
            if f[1] not in bases:
                raise PatchError(f'{where}: module {f[1]} not in the layout')
            hooks.setdefault(f[1], set()).update(int(o, 16) for o in f[2:])
        else:
            raise PatchError(f'{where}: cannot parse {raw!r}')
    base, size = bases['main'][0], bases['main'][1]
    page = (base + size + PAGE - 1) & ~(PAGE - 1)
    nxt = min((b for _, b, _ in mods if b > base), default=None)
    if nxt is not None and page + PAGE > nxt:
        raise PatchError(f'no room for the patch page after main ({page:#x})')
    table = Table(sets, sites, page, spins, hooks, ordered, plugins)
    table.bases = {n: b for n, (b, _) in bases.items()}
    return table


def plugin_line(f, where, index, bases, plugins):
    """An expect, stub, seed or orig line into the plugin entry it belongs to (its `plugin` line comes first)."""
    def plugin(name):
        if name not in plugins:
            raise PatchError(f'{where}: {name} has no plugin line before this one')
        return plugins[name]

    if f[0] == 'expect' and len(f) == 6:
        if f[1] not in index or f[2] not in bases:
            raise PatchError(f'{where}: unknown set {f[1]} or module {f[2]}')
        owner = [p for p in plugins.values() if p['set'] == index[f[1]]]
        if not owner:
            raise PatchError(f'{where}: set {f[1]} owns no plugin')
        owner[0]['expect'].append((f[2], int(f[3], 16), int(f[4], 16), int(f[5], 16)))
    elif f[0] == 'stub' and len(f) == 3:
        plugin(f[1])['stubs'].add(int(f[2], 16))
    elif f[0] == 'seed' and len(f) >= 3:
        plugin(f[1])['seeds'].update(int(o, 16) for o in f[2:])
    elif f[0] == 'orig' and len(f) == 6:
        p = plugin(f[2])
        if f[1] not in index or p['set'] != index[f[1]] or f[4] not in bases:
            raise PatchError(f'{where}: orig of set {f[1]} for {f[2]}, which another set owns, or module {f[4]}')
        p['origs'][int(f[3], 16)] = (f[4], int(f[5], 16))
    else:
        raise PatchError(f'{where}: cannot parse {" ".join(f)!r}')


def check(table, mod, name):
    """Stops on a site whose original word differs from the module's."""
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
    lines = [f'// {marker}: code patch sets, generated by recomp/gen.py --patches. Do not edit or commit.',
             '#pragma once', '', f'#define RT_PATCH_COUNT {len(table.sets)}',
             f'#define RT_PATCH_PAGE {table.page:#x}ull',
             '// X(index, name, env switch, default)',
             '#define RT_PATCH_SETS(X) ' + ' '.join(f'X({i}, {n}, "{e}", {d})' for i, (n, e, d) in
                                                    enumerate(table.sets)), '']
    if table.plugins:   # none: the header is what it was before plugins
        names = sorted(table.plugins)
        writes = [f'X({i}, "{m}", {o:#x}, {old:#010x}u, {new:#010x}u)' for i, n in enumerate(names)
                  for m, o, old, new in sorted(table.plugins[n]['expect'])]
        lines += [f'#define RT_PLUGIN_COUNT {len(names)}',
                  '// X(index, module, its set, SHA-256 of its .text after the init)',
                  '#define RT_PLUGINS(X) ' + ' '.join(f'X({i}, "{n}", {table.plugins[n]["set"]}, '
                                                     f'"{table.plugins[n]["sha256"]}")' for i, n in enumerate(names)),
                  f'#define RT_PLUGIN_WRITE_COUNT {len(writes)}',
                  '// X(plugin index, module, offset, old word, new word): what the init writes into game modules',
                  '#define RT_PLUGIN_WRITES(X) ' + ' '.join(writes),
                  '// X(module, offset): the hook entries (seed lines) TOTK_PLUGIN_TRACE counts (runtime/plugin_init.c)',
                  '#define RT_PLUGIN_ROOTS(X) ' + ' '.join(f'X("{n}", {o:#x})' for n in names
                                                          for o in sorted(table.plugins[n]['seeds'])), '']
    picks = table.picks()
    if picks:   # none: the header is what it was before ordered sets
        lines += [f'#define RT_PATCH_PICK_COUNT {len(picks)}',
                  '// X(index, module, module offset, its ordered sets in the order its C tests them: rt_patch_pick 1 up)',
                  '#define RT_PATCH_PICKS(X) ' + ' '.join(f'X({i}, "{m}", {o:#x}, {", ".join(map(str, s))})'
                                                         for i, (m, o, s) in enumerate(picks)), '']
    return '\n'.join(lines)
