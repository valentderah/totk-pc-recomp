"""Cheat file to code patch sets: the code cheats of an Atmosphere cheat file become patch sets of the lifted game.

Lifted code cannot be written to while the game runs, so a cheat that writes instructions into main's .text is turned
into `site` lines (recomp/patches.py) when the game is lifted; the runtime (runtime/cheats.c) switches each cheat's set
from the in-game menu. A cheat that only writes data (heap, main's data, pointer chains, button conditions) needs no
patch: runtime/cheats.c runs it in its cheat VM.

  python tools/cheats.py --exefs <dir> --cheats <BUILD_ID>.txt --patches <patch file> --out <patch file> --status <file>

--out gets the --patches file followed by one set per convertible cheat (`cheat_<hash>`, switch TOTK_CHEAT_<HASH>,
default off) and its sites. --status gets one line per cheat of the file, in file order, tab separated:

  <name hash> <content hash> <kind> <buttons> <note>

name hash: FNV-1a 32 of the UTF-8 name (a repeated name: name + "#2", "#3" ...), 8 upper-case hex digits; content
hash: the same over the cheat's words, so the runtime sees an edited cheat that was not rebuilt. kind:
  code       instruction writes, a patch set
  key        instruction writes while buttons are held (8 mask / writes / 21 else / the original words / 20): a patch
             set the runtime switches on while the cheat is on and <buttons> (hex HidNpadButton mask) are held
  master     a {master} cheat with instruction writes: a set that is always on
  restore    writes only the original instructions (switching a cheat off restores them): nothing to do
  heading    no opcodes (a title or a group label)
  data       no instruction writes: the runtime's cheat VM
  unsupported <note> says why
A code cheat that calls or jumps into a code cave it writes (words where main has zeros) gets the cave's body inlined
at the call site; a conditional skip of one store in the body (cbz wN, +8; str wN, ...) is dropped.

Standard library and recomp/ only.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
import flags  # noqa: E402
from lifter import Lifter  # noqa: E402
from nso import Nso  # noqa: E402

BASE = 0x7000000000   # main's base in the layout (recomp/layout.py); only pc-relative decoding sees it
NOP = 0xD503201F
RET = 0xD65F03C0


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
        self.sites = {}   # {offset: [words]}


def parse(text):
    """Cheats in file order; words that are not 8 hex digits are skipped."""
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
        c.key = fnv((c.name if n == 1 else f'{c.name}#{n}').encode())
    return out


def ops(words):
    """[(type, [words])] of a cheat; type is the opcode nibble, 0xC0.. for the extended ones. ValueError when the last
    opcode is cut short."""
    out, i = [], 0
    while i < len(words):
        w = words[i]
        t = w >> 28
        width = (w >> 24) & 0xF
        if t in (0, 1):
            n = 4 if width == 8 else 3
        elif t in (2, 8):
            n = 1
        elif t == 3:
            n = 2 if (w >> 24) & 0xF == 0 else 1
        elif t in (4, 6):
            n = 3
        elif t in (5, 7):
            n = 2
        elif t == 9:
            n = 1 + ((2 if width == 8 else 1) if (w >> 8) & 0xF else 0)
        elif t == 0xA:
            n = 2 if (w >> 8) & 0xF in (2, 4, 5) else 1
        else:
            t = (w >> 24) & 0xFF if t == 0xC else (w >> 20) & 0xFFF
            n = 1
        if i + n > len(words):
            raise ValueError(f'opcode {w:08X} is cut short')
        out.append((t, words[i:i + n]))
        i += n
    return out


VM_TYPES = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA}


def vm_ok(o):
    """None when runtime/cheats.c runs the opcode, else why not."""
    t, w = o
    if t not in VM_TYPES:
        return f'opcode {w[0]:08X} is not supported'
    if t == 5 and (w[0] >> 12) & 0xF > 1:
        return f'opcode {w[0]:08X} (load from a register offset) is not supported'
    if t == 0xA and (w[0] >> 8) & 0xF > 5:
        return f'opcode {w[0]:08X} is not supported'
    if t in (0, 1, 5, 6, 7, 9, 0xA) and (w[0] >> 24) & 0xF not in (1, 2, 4, 8):
        return f'opcode {w[0]:08X} has a bad width'
    return None


# ---- instruction writes

class Main:
    def __init__(self, path):
        n = Nso(str(path))
        self.image = bytes(n.image)
        self.text = n.text   # (offset, size)
        self.code_end = n.rodata[0]   # .text and its padding up to .rodata, where code caves go
        self.lifter = Lifter(BASE, 'c')

    def in_text(self, off):
        return self.text[0] <= off < self.text[0] + self.text[1] and off % 4 == 0

    def in_code(self, off):
        return self.text[0] <= off < self.code_end and off % 4 == 0

    def word(self, off):
        return struct.unpack_from('<I', self.image, off)[0]

    def lift(self, w, off):
        return self.lifter.lift(w, BASE + off)

    def effect(self, w, off):
        ins = self.lift(w, off)
        if ins.kind == 'op':
            return flags.effect(ins.code or '')
        if ins.kind == 'bcond':
            return flags.effect(ins.cond)
        return (False, None)


def code_writes(main, o):
    """{offset: word} of an opcode 0 into main's code (.text and its padding; 4 or 8 bytes, no offset register in
    use), None for any other opcode; ValueError for a write into the code the patch cannot express."""
    t, w = o
    if t != 0 or (w[0] >> 20) & 0xF != 0:
        return None
    width = (w[0] >> 24) & 0xF
    off = ((w[0] & 0xFF) << 32) | w[1]
    if not main.in_code(off - off % 4):
        return None
    if width == 4 and off % 4 == 0:
        return {off: w[2]}
    if width == 8 and off % 4 == 0:
        return {off: w[3], off + 4: w[2]}
    raise ValueError(f'a {width}-byte write into the code at main+{off:X}')


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
    """{offset: [replacement words]} of a set of instruction writes; ValueError when it cannot."""
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
        kind = main.lift(orig, a).kind
        if kind not in ('op', 'bl', 'b', 'bcond'):
            raise ValueError(f'main+{a:X} is a {kind}, which a patch cannot replace')
        last = main.lift(repl[-1], a).kind
        if last not in ('op', 'bl', 'b', 'bcond', 'ret'):
            raise ValueError(f'main+{a:X} gets a {last}')
        want = main.effect(orig, a)[1]
        got = [e[1] for e in (main.effect(x, a) for x in repl) if e[1]]
        if (got[0] if got else None) != want or len(got) > 1:
            # a nop over adds / subs Rd, Rd, #imm: subtract 0 instead, the flags still written and Rd unchanged
            if repl == [NOP] and orig & 0x1F000000 == 0x11000000 and orig & 0x20000000 and \
                    orig & 0x1F == (orig >> 5) & 0x1F:
                repl = [orig & ~0x003FFC00]
            else:
                raise ValueError(f'main+{a:X}: the cheat changes the CPU flags differently from the game')
        sites[a] = repl
    return sites


def classify(main, c):
    try:
        os_ = ops(c.words)
    except ValueError as e:
        c.kind, c.note = 'unsupported', str(e)
        return
    os_ = [o for o in os_ if not (o[0] == 2 and len(os_) == 1)]   # [Link] 20000000: a label
    if not os_:
        c.kind = 'heading'
        return
    try:
        cw = [code_writes(main, o) for o in os_]
    except ValueError as e:
        c.kind, c.note = 'unsupported', str(e)
        return
    if not any(x is not None for x in cw):
        bad = next((r for r in map(vm_ok, os_) if r), None)
        c.kind, c.note = ('unsupported', bad) if bad else ('data', '')
        return
    try:
        if all(x is not None for x in cw):
            writes = {k: v for x in cw for k, v in x.items()}
            if all(main.word(a) == w for a, w in writes.items()):
                c.kind = 'restore'
                return
            c.sites, c.kind = convert(main, writes), 'master' if c.master else 'code'
            return
        # 8 mask, writes, 21000000, the original words, 20000000
        t = [o[0] for o in os_]
        els = next((i for i, o in enumerate(os_) if o[0] == 2 and (o[1][0] >> 24) & 0xF == 1), -1)
        if (t[0] == 8 and t[-1] == 2 and (os_[-1][1][0] >> 24) & 0xF == 0 and els > 1
                and all(x is not None for x in cw[1:els] + cw[els + 1:-1])):
            on = {k: v for x in cw[1:els] for k, v in x.items()}
            off = {k: v for x in cw[els + 1:-1] for k, v in x.items()}
            if set(off) == set(on) and all(main.word(a) == w for a, w in off.items()):
                c.sites, c.kind, c.buttons = convert(main, on), 'key', os_[0][1][0] & 0x0FFFFFFF
                return
        raise ValueError('it mixes instruction writes with other cheat opcodes')
    except ValueError as e:
        c.kind, c.note, c.sites = 'unsupported', str(e), {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--cheats', type=Path, required=True)
    ap.add_argument('--patches', type=Path, required=True, help='the game version\'s patch file')
    ap.add_argument('--out', type=Path, required=True, help='the patch file with the cheats added')
    ap.add_argument('--status', type=Path, required=True)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')   # cheat names in a console without their characters
    m = Main(args.exefs / 'main')
    text = args.patches.read_text()
    cheats = parse(args.cheats.read_text(encoding='utf-8-sig', errors='replace')) if args.cheats.is_file() else []
    sets, sites, status = [], [], []
    for c in cheats:
        classify(m, c)
        if c.sites:
            name = f'cheat_{c.key.lower()}'
            label = c.name.encode('ascii', 'replace').decode()   # recomp/patches.py reads the file as ASCII
            sets.append(f'set {name} TOTK_CHEAT_{c.key} {1 if c.kind == "master" else 0}   # {label}')
            for a, repl in sorted(c.sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
        content = fnv(' '.join(f'{w:08X}' for w in c.words).encode())
        note = c.note.replace('\t', ' ')
        status.append(f'{c.key}\t{content}\t{c.kind}\t{c.buttons:X}\t{note}')
        print(f'cheats: {c.kind:11} {c.name}' + (f' ({note})' if note else ''))
    if sets:
        text += (f'\n# Cheats of {args.cheats.name} (tools/cheats.py)\n' + '\n'.join(sets) + '\n'
                 + '\n'.join(sites) + '\n')
    args.out.write_text(text)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    kinds = {}
    for c in cheats:
        kinds[c.kind] = kinds.get(c.kind, 0) + 1
    print(f'cheats: {len(cheats)} in {args.cheats.name}: '
          + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items())) + f'; {len(sites)} sites')


if __name__ == '__main__':
    main()
