"""Cheat files to code patch sets: the code cheats of the mods' Atmosphere cheat files become patch sets of the lifted
game.

Lifted code cannot be written to while the game runs, so a cheat that writes instructions into main's .text is turned
into `site` lines (recomp/patches.py) when the game is lifted, and the instructions cheats put into the padding after
.text (code caves) into `code` lines; the runtime (runtime/cheats.c) switches each cheat's set from the in-game menu.
Everything else a cheat writes (heap, main's data, the data in a code cave, pointer chains, button conditions) needs
no patch: runtime/cheats.c runs it in its cheat VM.

  python tools/cheats.py --exefs <dir> --mods <mods folder> --patches <patch file> --out <patch file> --status <file>

The cheat files are <mod>/cheats/<build id>.txt in each folder of --mods (or the Atmosphere places of that folder,
as runtime/mods.c finds them; the name's case does not matter), the build id the first 8 bytes of main's. Every mod's
file is built, switched on or off: the runtime decides which run.

The addresses a cheat writes come from running it symbolically the way Atmosphere's dmnt runs a cheat file: registers
start at 0 every frame and carry from one cheat to the next, so a write through a register counts as much as an
opcode 0. A cheat that uses a register before setting it gets what the setup cheats leave there: the first cheat with
opcodes of a file, and every {master} cheat (a "master code" that loads a pointer into a register for the rest). A
value is a constant, an offset into main (a pointer main's relocations make, read from main or computed from one), or
unknown (anything read from memory the game writes).

--out gets the --patches file followed by one set per cheat that changes code (`cheat_<hash>`, switch
TOTK_CHEAT_<HASH>, default off; on for a {master} cheat), its sites, and `code` lines with the caves. --status gets
one line per cheat, mods by name, cheats in file order, tab separated:

  <name hash> <content hash> <kind> <buttons> <flags> <registers> <note>

name hash: FNV-1a 32 of "<mod folder name>/<cheat name>" in UTF-8 (a name repeated in one file: name + "#2", "#3" ...),
8 upper-case hex digits, so two mods' cheats of one name are apart; content hash: the same over the cheat's words, so
the runtime sees an edited cheat that was not rebuilt. kind:
  code       instruction writes, a patch set
  key        instruction writes while buttons are held (8 mask / writes / 21 else / the original words / 20): a patch
             set the runtime switches on while the cheat is on and <buttons> (hex HidNpadButton mask) are held
  master     a {master} cheat: always on (its set, its VM run)
  builtin    nothing for the player to switch: a setup cheat (below) that writes only into the padding after .text
             (the caves the build has, their data: flag v, always on) and registers, or a cheat that writes only
             caves
  restore    writes only the original instructions (switching a cheat off restores them): nothing to do
  heading    no opcodes (a title or a group label)
  data       no instruction writes: the runtime's cheat VM
  unsupported <note> says why
flags: `v` the runtime runs the cheat in its VM while it is on (the writes no patch made: data, cave data; the VM
skips writes into .text), `s` a setup cheat whose registers the runtime hands to the other cheats (only with `v`: one
that leaves registers only a run can tell runs for that alone). registers: what
the runtime loads into the registers before it runs the cheat, the values the setup cheats leave that are the same
every run: `<register>=<hex>` (an `m` before the number: an offset into main), comma separated.

A cave is what the cheats' branches into the padding reach, following the code to a ret or b; what else cheats write
there is data (a float a cave loads, a table), which the VM writes. A branch the cheat writes into the game's code
must stay in its function or reach a function's start (.eh_frame_hdr, symbols): one into the middle of another
function is a cheat made for another update. Two mods that write one byte of a cave differently conflict: the mod
later by name gets `unsupported`. Standard library, recomp/ and numpy (as recomp/) only.
"""
import argparse
import bisect
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
import flags  # noqa: E402
from lifter import Lifter, Unsupported  # noqa: E402
from module import eh_frame_starts  # noqa: E402
from nso import Nso, R_AARCH64_RELATIVE  # noqa: E402

BASE = 0x7000000000   # main's base in the layout (recomp/layout.py); only pc-relative decoding sees it
M64 = (1 << 64) - 1
NOP = 0xD503201F
RET = 0xD65F03C0
TITLE = '0100F2C0115B6000'


def fnv(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f'{h:08X}'


def build_id(main):
    """The first 8 bytes of the NSO build id, as cheat files name themselves."""
    return Path(main).read_bytes()[0x40:0x48].hex().upper()


# ---- parsing

class Cheat:
    def __init__(self, name, master, line):
        self.name, self.master, self.line, self.words = name, master, line, []
        self.key = self.kind = self.note = ''
        self.buttons = 0
        self.ops = []
        self.setup = False   # its registers carry into the cheats after it
        self.vm = False      # the runtime runs it in its VM
        self.regs = {}       # {register: value} the runtime loads before running it
        self.on = {}         # {word offset: word} instruction writes (while the buttons are held for a key cheat)
        self.off = {}        # key cheat: {word offset: word} written while they are not
        self.pad = {}        # {byte offset: byte, None when a register's} written into the padding after .text
        self.sites = {}      # {word offset: [words]}
        self.lifted = False  # writes cave code the build lifts
        self.game = False    # writes the game's memory (not .text or its padding)


def read_cheats(path):
    """The cheat file's text: UTF-8 (a BOM is skipped), else GBK (code page 936; older Chinese cheat files), as
    runtime/cheats.c reads it, so the names and their hashes agree."""
    raw = path.read_bytes()
    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        return raw.decode('cp936', errors='replace')


def parse(text, mod):
    """Cheats in file order, keyed under the mod folder name; words that are not 8 hex digits are skipped."""
    out = []
    for no, raw in enumerate(text.splitlines(), 1):
        s = raw.strip()
        if len(s) >= 2 and s[0] in '[{' and s[-1] in ']}':
            out.append(Cheat(s[1:-1].strip(), s[0] == '{', no))
        elif out:
            for w in s.split():
                if len(w) == 8:
                    try:
                        out[-1].words.append(int(w, 16))
                    except ValueError:
                        pass
    seen = {}
    for c in out:
        n = seen[c.name] = seen.get(c.name, 0) + 1
        c.key = fnv((f'{mod}/' + (c.name if n == 1 else f'{c.name}#{n}')).encode())
    return out


def cheat_files(mods, bid):
    """[(mod folder name, cheat file)] of the folders of mods, by name: <mod>/cheats/<bid>.txt or its Atmosphere places
    (runtime/mods.c find_root), the file name in any case."""
    out = []
    for mod in sorted((d for d in mods.iterdir() if d.is_dir()), key=lambda d: d.name) if mods.is_dir() else []:
        for where in ('cheats', f'atmosphere/contents/{TITLE}/cheats', f'contents/{TITLE}/cheats', f'{TITLE}/cheats'):
            folder = mod / where
            if folder.is_dir():
                f = next((p for p in sorted(folder.iterdir()) if p.is_file() and p.name.lower() == f'{bid}.txt'.lower()),
                         None)
                if f:
                    out.append((mod.name, f))
                break
    return out


def ops(words):
    """[(type, [words])] of a cheat; type is the opcode nibble, 0xC0 .. 0xC3 and 0xFF0 / 0xFF1 / 0xFFF for the
    extended ones. ValueError when the last opcode is cut short. runtime/cheats.c op_len agrees."""
    out, i = [], 0
    while i < len(words):
        w = words[i]
        t, width, x = w >> 28, (w >> 24) & 0xF, (w >> 8) & 0xF
        if t in (0, 1):
            n = 4 if width == 8 else 3
        elif t in (2, 8):
            n = 1
        elif t == 3:
            n = 2 if width == 0 else 1
        elif t in (4, 6):
            n = 3
        elif t in (5, 7):
            n = 2
        elif t == 9:
            n = 1 + ((2 if width == 8 else 1) if x else 0)
        elif t == 0xA:
            n = 2 if x in (2, 4, 5) else 1
        elif t == 0xC:
            t, n = (w >> 24) & 0xFF, 1
            if t == 0xC0 and x in (0, 2):
                n = 2
            elif t == 0xC0 and x == 4:
                n = 3 if (w >> 20) & 0xF == 8 else 2
        elif t == 0xF:
            t = (w >> 20) & 0xFFF
            n = 2 if t == 0xFFF and x in (0, 2) else 1
        else:
            n = 1
        if i + n > len(words):
            raise ValueError(f'opcode {w:08X} is cut short')
        out.append((t, words[i:i + n]))
        i += n
    return out


VM_TYPES = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xC0, 0xC1, 0xC2, 0xC3, 0xFF0, 0xFF1, 0xFFF}


def vm_ok(o):
    """None when runtime/cheats.c runs the opcode, else why not."""
    t, w = o
    f = w[0]
    if t not in VM_TYPES:
        return f'opcode {f:08X} is not supported'
    width = (f >> 20) & 0xF if t == 0xC0 else (f >> 24) & 0xF
    x = (f >> 8) & 0xF
    if ((t == 5 and (f >> 12) & 0xF > 3) or (t in (0xA, 0xC0) and x > 5) or (t == 9 and (f >> 20) & 0xF > 13)
            or (t == 0xC1 and (f >> 4) & 0xF > 3) or (t == 0xC2 and (f >> 20) & 0xF > 3)):
        return f'opcode {f:08X} is not supported'
    if t in (0, 1, 5, 6, 7, 9, 0xA, 0xC0) and width not in (1, 2, 4, 8):
        return f'opcode {f:08X} has a bad width'
    if t == 9 and (f >> 20) & 0xF >= 10 and width not in (4, 8):
        return f'opcode {f:08X} is float arithmetic on {width} bytes'
    return None


# ---- main

class Main:
    def __init__(self, path):
        n = Nso(str(path))
        self.image = bytes(n.image)
        self.text = n.text   # (offset, size)
        self.text_end = n.text[0] + n.text[1]
        self.code_end = n.rodata[0]   # .text and its padding up to .rodata, where code caves go
        self.ro_end = n.data[0]       # .text and .rodata never change at run time
        self.size = n.size
        self.rel = {off: add for off, typ, _, add in n.relocs if typ == R_AARCH64_RELATIVE}
        self.lifter = Lifter(BASE, 'c')
        # Functions: their extents from .eh_frame_hdr and the symbols; as entries also what discovery seeds
        # (recomp/gen.py): relocation targets and the game's own calls.
        fns = set(eh_frame_starts(n, 0))
        fns.update(value for _, info, shndx, value, _ in n.syms if shndx and info & 0xF == 2)
        self.functions = sorted(a for a in fns if self.in_text(a))
        fns.update(self.rel.values())
        w = np.frombuffer(self.image, dtype='<u4', count=n.text[1] // 4, offset=n.text[0])
        bl = np.flatnonzero((w & 0xFC000000) == 0x94000000)
        imm = (w[bl] & 0x03FFFFFF).astype(np.int64)
        fns.update((n.text[0] + 4 * (bl + np.where(imm >= 1 << 25, imm - (1 << 26), imm))).tolist())
        self.entries = {a for a in fns if self.in_text(a)}

    def function_of(self, off):
        """Start of the function off is in (.eh_frame_hdr and symbols), None before the first."""
        i = bisect.bisect_right(self.functions, off) - 1
        return self.functions[i] if i >= 0 else None

    def in_text(self, off):
        return self.text[0] <= off < self.text_end

    def in_pad(self, off):
        return self.text_end <= off < self.code_end

    def in_code(self, off):
        return self.text[0] <= off < self.code_end and off % 4 == 0

    def word(self, off):
        return int.from_bytes(self.image[off:off + 4], 'little')

    def lift(self, w, off):
        """Lifted instruction at off; Unsupported becomes a 'trap' (recomp/module.py does the same)."""
        try:
            return self.lifter.lift(w, BASE + off)
        except Unsupported:
            return None

    def effect(self, ins):
        if ins.kind == 'op':
            return flags.effect(ins.code or '')
        if ins.kind == 'bcond':
            return flags.effect(ins.cond)
        return (False, None)

    def word_effect(self, w, off):
        """effect() of the word w at off; ValueError when the lifter cannot take it."""
        ins = self.lift(w, off)
        if ins is None:
            raise ValueError(f'main+{off:X} gets {w:08X}, which the lifter cannot take')
        return self.effect(ins)

    def load(self, off, width):
        """The value at main+off when it is the same in every run: a pointer main's relocations make (8 bytes),
        .text or .rodata; None for what the game or a cheat may write."""
        if width == 8 and off in self.rel:
            return ('m', self.rel[off])
        if 0 <= off and off + width <= self.ro_end and not (off < self.code_end and off + width > self.text_end):
            return ('k', int.from_bytes(self.image[off:off + width], 'little'))
        return None


# ---- symbolic values: ('k', constant), ('m', offset into main), None (unknown)

def add(a, b):
    if a is None or b is None or (a[0] == 'm' and b[0] == 'm'):
        return None
    return ('m' if 'm' in (a[0], b[0]) else 'k', (a[1] + b[1]) & M64)


def masked(v, width):
    if v is None or width >= 8:
        return v
    return ('k', v[1] & ((1 << 8 * width) - 1)) if v[0] == 'k' else None


def arith(op, a, b, width):
    """dmnt's register arithmetic (opcodes 7 and 9) on symbolic values."""
    if op == 9:
        r = a
    elif a is None:
        r = None
    elif op == 7:
        r = ('k', ~a[1] & M64) if a[0] == 'k' else None
    elif b is None:
        r = None
    elif op == 0:
        r = add(a, b)
    elif op == 1:
        r = add(a, ('k', -b[1] & M64)) if b[0] == 'k' else ('k', (a[1] - b[1]) & M64) if a[0] == 'm' else None
    elif a[0] == 'k' and b[0] == 'k' and op in (2, 3, 4, 5, 6, 8):
        x, y = a[1], b[1]
        r = ('k', (x * y if op == 2 else (x << y if y < 64 else 0) if op == 3 else (x >> y if y < 64 else 0)
                   if op == 4 else x & y if op == 5 else x | y if op == 6 else x ^ y) & M64)
    else:
        r = None   # float arithmetic, or on an offset into main
    return masked(r, width)


def structure(os_):
    """({index of a conditional (1, 8, C0): (index of its else or None, index of its end)}, {index of a loop start:
    index of its end}), matched as dmnt skips them; a block left open runs to the cheat's end."""
    conds, loops, stack = {}, {}, []
    for i, (t, w) in enumerate(os_):
        if t in (1, 8, 0xC0):
            stack.append([i, None])
        elif t == 2 and stack:
            if (w[0] >> 24) & 0xF == 1:
                if stack[-1][1] is None:
                    stack[-1][1] = i
            else:
                s, e = stack.pop()
                conds[s] = (e, i)
        elif t == 3 and (w[0] >> 24) & 0xF == 0:
            r = (w[0] >> 16) & 0xF
            loops[i] = next((k for k in range(i + 1, len(os_)) if os_[k][0] == 3 and (os_[k][1][0] >> 24) & 0xF
                             and (os_[k][1][0] >> 16) & 0xF == r), len(os_))
    for s, e in stack:
        conds[s] = (e, len(os_))
    return conds, loops


def written(os_):
    """Registers the opcodes may write."""
    out = set()
    for t, w in os_:
        f = w[0]
        r = (f >> 16) & 0xF
        if t in (3, 4, 5, 7, 9) or (t in (6, 0xA) and (f >> 12) & 0xF):
            out.add(r)
        elif t == 0xC1 and (f >> 4) & 0xF in (0, 3):
            out.add(r)
        elif t == 0xC2 and (f >> 20) & 0xF in (0, 3):
            out |= {k for k in range(16) if f >> k & 1}
        elif t == 0xC3 and (f >> 4) & 0xFF < 0x80:
            out.add(f & 0xF)
    return out


class Run:
    """One cheat run symbolically from the registers regs. writes: [(address, width, value, conditions)], the address
    and value symbolic; conditions: ((('key', mask) | ('if', index), taken), ...) of the blocks around the write."""

    def __init__(self, main, os_, regs):
        self.main, self.os = main, os_
        self.conds, self.loops = structure(os_)
        self.writes = []
        self.regs, self.saved = list(regs), [('k', 0)] * 16
        self.block(0, len(os_), self.regs, self.saved, ())

    def block(self, i, end, regs, saved, conds):
        while i < end:
            t, w = self.os[i]
            if t in (1, 8, 0xC0):
                els, fin = self.conds[i]
                cond = ('key', w[0] & 0x0FFFFFFF) if t == 8 else ('if', i)
                r1, s1, r2, s2 = list(regs), list(saved), list(regs), list(saved)
                self.block(i + 1, fin if els is None else els, r1, s1, conds + ((cond, True),))
                if els is not None:
                    self.block(els + 1, fin, r2, s2, conds + ((cond, False),))
                regs[:] = [a if a == b else None for a, b in zip(r1, r2)]
                saved[:] = [a if a == b else None for a, b in zip(s1, s2)]
                i = fin + 1
            elif t == 3 and i in self.loops:
                # The body once, with what it changes unknown; a loop runs it at least once.
                fin = self.loops[i]
                changed = written(self.os[i + 1:fin]) | {(w[0] >> 16) & 0xF}
                for r in changed:
                    regs[r] = None
                self.block(i + 1, fin, regs, saved, conds)
                for r in changed:
                    regs[r] = None
                regs[(w[0] >> 16) & 0xF] = ('k', 0)
                i = fin + 1
            else:
                self.op(t, w, regs, saved, conds)
                i += 1

    def load(self, src, width):
        return self.main.load(src[1], width) if src and src[0] == 'm' else None

    def op(self, t, w, regs, saved, conds):
        f = w[0]
        width, m, r, x = (f >> 24) & 0xF, (f >> 20) & 0xF, (f >> 16) & 0xF, (f >> 8) & 0xF
        if t == 0:     # 0TMR00AA AAAAAAAA V (V): [region M + A + register R] = V
            rel = (f & 0xFF) << 32 | w[1]
            v = w[2] << 32 | w[3] if width == 8 else w[2]
            self.writes.append((add(('m', rel), regs[r]) if m == 0 else None, width, masked(('k', v), width), conds))
        elif t == 4:   # 400R0000 V V
            regs[r] = ('k', w[1] << 32 | w[2])
        elif t == 5:   # 5TMR00AA AAAAAAAA, 5T0R10AA, 5T0R2SAA, 5TMR3SAA: R = [address]
            how, rel = (f >> 12) & 0xF, (f & 0xFF) << 32 | w[1]
            if how == 0:
                src = ('m', rel) if m == 0 else None
            elif how in (1, 2):
                src = add(regs[r if how == 1 else x], ('k', rel))
            else:
                src = add(('m', rel), regs[x]) if m == 0 else None
            regs[r] = self.load(src, width)   # from an absolute address: memory the game owns, unknown
        elif t == 6:   # 6T0RIor0 V V: [R (+ register r)] = V
            a = add(regs[r], regs[(f >> 4) & 0xF]) if x else regs[r]
            self.writes.append((a, width, masked(('k', w[1] << 32 | w[2]), width), conds))
            if (f >> 12) & 0xF:
                regs[r] = add(regs[r], ('k', width))
        elif t == 7:   # 7T0RC000 V: R = R <op> V (add, sub, mul, shl, shr)
            op = (f >> 12) & 0xF
            regs[r] = arith(op, regs[r], ('k', w[1]), width) if op <= 4 else masked(regs[r], width)
        elif t == 9:   # 9TCRS0s0 / 9TCRS100 V (V): R = S <op> (s | V)
            b = ('k', (w[1] << 32 | w[2]) if width == 8 else w[1]) if x else regs[(f >> 4) & 0xF]
            regs[r] = arith(m, regs[(f >> 12) & 0xF], b, width)
        elif t == 0xA:   # ATSRIOxa (aaaaaaaa): [address of type O] = register S
            s, inc, o = m, (f >> 12) & 0xF, x
            rel = ('k', (f & 0xF) << 32 | w[1]) if o in (2, 4, 5) else ('k', 0)
            base, reg = regs[r], (f >> 4) & 0xF

            def region(v):
                return ('m', v[1]) if reg == 0 and v and v[0] == 'k' else None
            a = (base if o == 0 else add(base, regs[reg]) if o == 1 else add(base, rel) if o == 2 else region(base)
                 if o == 3 else region(rel) if o == 4 else region(add(base, rel)))
            self.writes.append((a, width, masked(regs[s], width), conds))
            if inc:
                regs[r] = add(base, ('k', width))
        elif t in (0xC1, 0xC2):   # save / restore / clear registers
            how = (f >> 4) & 0xF if t == 0xC1 else m
            pairs = [(r, x)] if t == 0xC1 else [(k, k) for k in range(16) if f >> k & 1]
            for d, s in pairs:
                if how == 0:
                    regs[d] = saved[s]
                elif how == 1:
                    saved[d] = regs[s]
                elif how == 2:
                    saved[d] = ('k', 0)
                elif how == 3:
                    regs[d] = ('k', 0)
        elif t == 0xC3 and (f >> 4) & 0xFF < 0x80:   # a static register: set by the host
            regs[f & 0xF] = None
        # 1, 8, C0 (blocks), 2 (an end or else outside a block), 3 (a loop end), FF0 / FF1 / FFF: nothing


def analyse(main, c, regs):
    """Runs c from regs and returns the registers it leaves; fills c.on / c.off / c.pad / c.vm, or c.kind and c.note
    when it cannot be used."""
    run = Run(main, c.ops, regs)
    try:
        effects(main, c, run)
    except ValueError as e:
        c.kind, c.note = 'unsupported', str(e)
    return run.regs


def effects(main, c, run):
    text = {}   # {conditions: {byte offset: byte}}
    for a, width, v, conds in run.writes:
        off = a[1] if a and a[0] == 'm' else None
        if off is None or off + width <= main.text[0] or off >= main.code_end:
            c.vm = c.game = True   # heap, main's data, an address known at run time
            continue
        for k in range(width):
            b = None if v is None else v[1] >> 8 * k & 0xFF
            if main.in_text(off + k):
                if b is None:
                    raise ValueError(f'it writes a register into the code at main+{off + k:X}')
                text.setdefault(conds, {})[off + k] = b
            elif main.in_pad(off + k):
                c.pad[off + k] = b
            else:
                c.vm = c.game = True
    # Instruction writes: always (u), while buttons are held (held), while not (rest). The writes in a block come
    # after the ones before it, as cheats write the original words and then the changed ones while held.
    u, held, rest, mask = {}, {}, {}, None
    for conds, bytes_ in text.items():
        if conds and (len(conds) > 1 or conds[0][0][0] != 'key' or mask not in (None, conds[0][0][1])):
            raise ValueError('it writes the game\'s code under a condition other than buttons held')
        if conds:
            mask = conds[0][0][1]
        ws = u if not conds else held if conds[0][1] else rest
        for a, b in bytes_.items():
            w = ws.get(a & ~3, main.word(a & ~3))
            ws[a & ~3] = w & ~(0xFF << 8 * (a & 3)) | b << 8 * (a & 3)
    if mask is None:
        c.on = u
    else:
        both = set(u) | set(held) | set(rest)
        c.on = {a: held.get(a, u.get(a, main.word(a))) for a in both}
        c.off = {a: rest.get(a, u.get(a, main.word(a))) for a in both}
        if any(main.word(a) != w for a, w in c.off.items()):
            raise ValueError('it changes the game\'s code while buttons are held, and keeps it changed otherwise')
        c.buttons = mask


# ---- code caves

def branch_target(main, w, off):
    """Offset of the direct branch w at off (b, bl, b.cond, cbz, tbz), else None."""
    ins = main.lift(w, off)
    return ins.target - BASE if ins is not None and ins.kind in ('b', 'bl', 'bcond') else None


def caves(main, cheats):
    """{word offset: word} of the code in the padding: what the cheats' branches reach, following the code to a ret,
    b or br. A byte gets the value of the first cheat in file order that writes it (a later one writing another value
    is a conflict, see classify)."""
    content = {}
    for c in cheats:
        for a, b in c.pad.items():
            if b is not None:
                content.setdefault(a, b)
    todo = [t for c in cheats for a, w in c.on.items() for t in [branch_target(main, w, a)]
            if t is not None and main.in_pad(t)]
    code = {}
    while todo:
        a = todo.pop()
        while main.in_pad(a) and a % 4 == 0 and a not in code:
            if any(a + k not in content for k in range(4)):
                break   # nobody writes it: the branch's cheat finds no code there (classify)
            w = int.from_bytes(bytes(content[a + k] for k in range(4)), 'little')
            code[a] = w
            ins = main.lift(w, a)
            t = branch_target(main, w, a)
            if t is not None and main.in_pad(t):
                todo.append(t)
            if ins is None or ins.kind in ('b', 'br', 'ret', 'trap'):
                break
            a += 4
    return code


def runs(code):
    """[(offset, [words])] of the code's contiguous runs."""
    out = []
    for a in sorted(code):
        if out and out[-1][0] + 4 * len(out[-1][1]) == a:
            out[-1][1].append(code[a])
        else:
            out.append((a, [code[a]]))
    return out


# ---- sites

def flag_rule(main, a, words):
    """recomp/gen.py patched_effect's rules for the site main+a with the replacements words (one per cheat, in file
    order); ValueError when the generator would stop on them."""
    o = main.lift(main.word(a), a)
    reads, kinds, calls, cave_flags = main.effect(o)[0], [main.effect(o)[1]], o.kind == 'bl', False
    for w in words:
        r = main.lift(w, a)
        e = main.effect(r)
        reads |= e[0]
        t = branch_target(main, w, a)
        cave = r.kind if r.kind in ('b', 'bl') and t is not None and main.in_pad(t) else None
        if cave == 'bl':
            kinds.append('nz' if kinds[0] else None)
            cave_flags |= bool(kinds[0])
        elif cave != 'b':
            kinds.append(e[1])
            calls |= r.kind == 'bl'
    if (kinds[0] or reads) and calls:
        raise ValueError(f'main+{a:X}: a call in place of an instruction that uses flags')
    if cave_flags and reads:
        raise ValueError(f'main+{a:X}: a call into a code cave in place of an instruction that reads flags')


def site(main, code, a, w):
    """The replacement of the instruction at main+a by w (recomp/patches.py `site`); ValueError when the generator
    cannot take it."""
    orig = main.word(a)
    o, r = main.lift(orig, a), main.lift(w, a)
    if o is None or o.kind not in ('op', 'bl', 'b', 'bcond'):
        raise ValueError(f'main+{a:X} is a {o.kind if o else "an unsupported instruction"}, which a patch cannot '
                         f'replace')
    if r is None or r.kind not in ('op', 'bl', 'b', 'bcond', 'ret'):
        raise ValueError(f'main+{a:X} gets {w:08X}, which the lifter cannot take')
    t = branch_target(main, w, a)
    if t is not None and main.in_pad(t):
        if t not in code:
            raise ValueError(f'it branches to main+{t:X}, where no cheat writes code')
    elif t is not None and r.kind == 'bl' and t not in main.entries:
        raise ValueError(f'main+{a:X} calls main+{t:X}, which is not a function of the game (is the cheat for '
                         f'another version?)')
    elif t is not None and main.function_of(t) != main.function_of(a) and t not in main.entries:
        raise ValueError(f'main+{a:X} branches into the middle of another function, at main+{t:X} (is the cheat for '
                         f'another version?)')
    flag_rule(main, a, [w])
    return [w]   # other flag changes: recomp/gen.py patched_effect


def classify(main, c, code, content):
    """c.kind (and c.sites, c.vm) once the caves are known; content: {byte offset: (byte, cheat)} of the padding."""
    for a, b in c.pad.items():
        if a & ~3 in code:
            if b is None or b != content.get(a, (b,))[0]:
                raise ValueError(f'it writes main+{a:X} in a code cave differently from {content[a][1].name!r}')
            c.lifted = True
        else:
            c.vm = True   # data in the padding (a float, a table a cave reads)
    changed = {a: w for a, w in c.on.items() if w != main.word(a)}
    for a, w in sorted(changed.items()):
        c.sites[a] = site(main, code, a, w)
    if c.sites:
        return 'key' if c.off else 'master' if c.master else 'code'
    if c.master:
        return 'master' if c.vm else 'builtin'
    if c.on:
        return 'data' if c.vm else 'restore'
    if c.setup and not c.game:
        return 'builtin'   # code caves, their data and registers for the rest: always run when anything needs it
    if c.vm:
        return 'data'
    return 'builtin' if c.lifted else 'data'


# ---- in-place patches

def branch(w, off):
    """(kind, target offset) of a b / bl word at off, else None."""
    if w & 0x7C000000 == 0x14000000:
        return ('bl' if w >> 31 else 'b', off + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2))
    return None


def pc_relative(w):
    """adr / adrp, a literal load, or any branch: wrong once moved to another address."""
    return (w & 0x1F000000 == 0x10000000 or w & 0x3B000000 == 0x18000000 or w & 0x7C000000 == 0x14000000
            or w & 0xFE000000 in (0x54000000, 0x34000000, 0x36000000, 0xB4000000, 0xB6000000)
            or w & 0xFE000000 == 0xD6000000)


def skip_store(body, i):
    """body[i] is cbz / cbnz wN / xN, +8 and body[i + 1] a str of that register: the skip can go (the cheat stores
    only when the value is non-zero)."""
    w = body[i]
    if w & 0x7E000000 != 0x34000000 or (w >> 5) & 0x7FFFF != 2 or i + 1 >= len(body):
        return False
    s = body[i + 1]
    return s & 0x3B400000 == 0x39000000 and s & 0x1F == w & 0x1F   # str (unsigned offset), same Rt


def cave_body(writes, caves, start, back, call):
    """The cave's words from start up to its ret (call) or its b to back."""
    body, a = [], start
    while True:
        if a not in caves:
            raise ValueError(f'the cheat branches to main+{a:X}, which it does not write')
        w = writes[a]
        if call and w == RET:
            return body
        b = branch(w, a)
        if not call and b and b[0] == 'b' and b[1] == back:
            return body
        body.append(w)
        a += 4
        if len(body) > 64:
            raise ValueError('a code cave of more than 64 instructions')


def convert(main, writes):
    """{offset: [replacement words]} of a set of instruction writes that goes in place: a cave is inlined at its call
    site (tools/exefs_patches.py, which has no `code` lines); ValueError when it cannot."""
    caves = {a for a in writes if main.word(a) == 0}
    sites = {}
    for a, w in sorted(writes.items()):
        if a in caves or w == main.word(a):
            continue
        if not main.in_text(a):
            raise ValueError(f'main+{a:X} is past the end of the code')
        b = branch(w, a)
        if b and b[1] in caves:
            body = cave_body(writes, caves, b[1], a + 4, b[0] == 'bl')
            out = []
            for i, x in enumerate(body):
                if skip_store(body, i):
                    continue
                if pc_relative(x):
                    raise ValueError(f'the code cave at main+{b[1]:X} has a branch or pc-relative instruction')
                out.append(x)
            repl = out or [NOP]
        else:
            repl = [w]
        orig = main.word(a)
        kind = main.lift(orig, a)
        if kind is None or kind.kind not in ('op', 'bl', 'b', 'bcond'):
            raise ValueError(f'main+{a:X} is a {kind.kind if kind else "an unsupported instruction"}, which a patch '
                             f'cannot replace')
        last = main.lift(repl[-1], a)
        if last is None or last.kind not in ('op', 'bl', 'b', 'bcond', 'ret'):
            raise ValueError(f'main+{a:X} gets {last.kind if last else "an unsupported instruction"}')
        want = main.word_effect(orig, a)[1]
        got = [e[1] for e in (main.word_effect(x, a) for x in repl) if e[1]]
        if (got[0] if got else None) != want or len(got) > 1:
            # a nop over adds / subs Rd, Rd, #imm: subtract 0 instead, the flags still written and Rd unchanged
            if repl == [NOP] and orig & 0x1F000000 == 0x11000000 and orig & 0x20000000 and \
                    orig & 0x1F == (orig >> 5) & 0x1F:
                repl = [orig & ~0x003FFC00]
            else:
                raise ValueError(f'main+{a:X}: the cheat changes the CPU flags differently from the game')
        sites[a] = repl
    return sites


def value(v):
    return f'm{v[1]:x}' if v[0] == 'm' else f'{v[1]:x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, required=True, help='the mods folder next to totk.exe')
    ap.add_argument('--patches', type=Path, required=True, help='the game version\'s patch file')
    ap.add_argument('--out', type=Path, required=True, help='the patch file with the cheats added')
    ap.add_argument('--status', type=Path, required=True)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')   # cheat names in a console without their characters
    m = Main(args.exefs / 'main')
    text = args.patches.read_text()
    files = cheat_files(args.mods, build_id(args.exefs / 'main'))
    parsed = [parse(read_cheats(f), mod) for mod, f in files]
    cheats = [c for cs in parsed for c in cs]

    # Symbolic runs in file order, mods by name; the setup cheats' registers carry, as dmnt carries them across a
    # title's cheats. A file of one cheat sets up nothing of its own: a package that keeps each cheat in a folder of its
    # own has its master code's folder first, and that one alone is the setup.
    carried, seen = [('k', 0)] * 16, False
    for file_cheats in parsed:
        first = not seen or sum(1 for c in file_cheats if c.words) > 1
        for c in file_cheats:
            try:
                c.ops = ops(c.words)
            except ValueError as e:
                c.kind, c.note = 'unsupported', str(e)
                continue
            c.ops = [o for o in c.ops if not (o[0] == 2 and len(c.ops) == 1)]   # [Link] 20000000: a label
            if not c.ops:
                c.kind = 'heading'
                continue
            bad = next((r for r in map(vm_ok, c.ops) if r), None)
            if bad:
                c.kind, c.note = 'unsupported', bad
                continue
            c.setup, first, seen = first or c.master, False, True
            c.regs = {r: v for r, v in enumerate(carried) if v and v != ('k', 0)}
            left = analyse(m, c, carried)
            if c.setup:
                carried = left
                c.vm |= None in left   # registers only a run can tell: the runtime runs it for the other cheats

    live = [c for c in cheats if not c.kind]
    code = caves(m, live)
    content = {}
    for c in live:
        for a, b in c.pad.items():
            if b is not None:
                content.setdefault(a, (b, c))
    alts = {}   # {offset: [replacement words]} of the sites so far, in file order
    for c in live:
        try:
            c.kind = classify(m, c, code, content)
            for a, repl in c.sites.items():   # with the other cheats that patch the same instruction
                flag_rule(m, a, alts.get(a, []) + repl)
        except ValueError as e:
            c.kind, c.note, c.sites = 'unsupported', str(e), {}
        for a, repl in c.sites.items():
            alts.setdefault(a, []).extend(repl)

    sets, sites, status = [], [], []
    for c in cheats:
        if c.sites:
            name = f'cheat_{c.key.lower()}'
            label = c.name.encode('ascii', 'replace').decode()   # recomp/patches.py reads the file as ASCII
            sets.append(f'set {name} TOTK_CHEAT_{c.key} {1 if c.kind == "master" else 0}   # {label}')
            for a, repl in sorted(c.sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
        content_hash = fnv(' '.join(f'{w:08X}' for w in c.words).encode())
        vm = c.vm and c.kind in ('code', 'key', 'master', 'builtin', 'data')
        fl = ('v' if vm else '') + ('s' if c.setup and vm else '')
        regs = ','.join(f'{r:X}={value(v)}' for r, v in sorted(c.regs.items())) if vm else ''
        note = c.note.replace('\t', ' ')
        status.append(f'{c.key}\t{content_hash}\t{c.kind}\t{c.buttons:X}\t{fl}\t{regs}\t{note}')
        kind = c.kind + ('+vm' if vm and c.kind != 'data' else '')
        print(f'cheats: {kind:11} {c.name}' + (f' ({note})' if note else ''))
    lines = []
    for off, words in runs(code):
        for k in range(0, len(words), 8):
            lines.append(f'code main {off + 4 * k:x} {" ".join(f"{w:08x}" for w in words[k:k + 8])}')
    if sets or lines:
        text += ('\n# Cheats of the mods folder (tools/cheats.py)\n' + '\n'.join(sets + sites + lines) + '\n')
    args.out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    kinds = {}
    for c in cheats:
        kinds[c.kind] = kinds.get(c.kind, 0) + 1
    print(f'cheats: {len(cheats)} in {len(files)} files ({", ".join(mod for mod, _ in files) or "none"}): '
          + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items())) + f'; {len(sites)} sites, {len(code)} cave words')


if __name__ == '__main__':
    main()
