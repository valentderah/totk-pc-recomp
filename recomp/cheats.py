"""Atmosphere cheat files as code patch sets (gen.py --cheats), switched in the settings menu while the game runs.

Lifted code cannot be written at run time, so a cheat is applied when the C is generated: every instruction a cheat
changes becomes a patch site of its own set (recomp/patches.py), off until the player switches it on (runtime/patches.c
rt_cheat_switch; totk.ini `cheats = <id>, <id>`). Input is the file Atmosphere reads, named after main's build id
(cheats/<first 16 hex digits of the build id>.txt): `[Name]` starts a cheat, the opcode words follow; `{Name}` master
codes and `#` comment lines are skipped.

What converts: static writes (opcode type 0) to main, of any width. A write into main's .text replaces those
instructions. A write outside .text is a code cave and converts only as the body of a `bl` into it that the same cheat
writes in .text: the call is replaced by the cave's instructions up to its `ret` (inlined, so x30 stays the caller's).
The replacements follow the patch rules (no branch except a final bl; a changed instruction is not a branch other
than bl; both versions set the same flags). A cheat with any other opcode (conditionals, registers, heap or pointer
writes, button checks) or a write that breaks a rule is skipped with a warning; the others still load.

A cheat's id (the menu row's key in totk.ini) is its name in lower case with runs of other characters as `_`, after a
leading "№12." numbering is removed: "[№31. Inf. Equipment Durability]" -> inf_equipment_durability.
"""
import re
import struct
from pathlib import Path

import flags
from lifter import Lifter
from nso import Nso
from patches import PatchError

RET = 0xD65F03C0
NOP = 0xD503201F
MAX_CAVE = 64   # instructions inlined from one cave at most


def is_bl(w): return w & 0xFC000000 == 0x94000000


def is_branch(w):
    """b, bl, b.cond, cbz / cbnz, tbz / tbnz, br / blr / ret / eret."""
    return (w & 0x7C000000 == 0x14000000 or w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000
            or w & 0x7E000000 == 0x36000000 or w & 0xFE000000 == 0xD6000000)


def bl_target(w, a): return a + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2)


def cheat_id(name):
    name = re.sub(r'^\W*\d+\s*[.):-]\s*', '', name)
    return re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_') or 'cheat'


def display_name(name):
    return re.sub(r'^\W*\d+\s*[.):-]\s*', '', name).strip() or name


class Skip(Exception):
    pass


def parse(text):
    """[(name, [opcode words], master)] in file order."""
    out, cur = [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line[0] in '[{' and line[-1] in ']}':
            cur = (line[1:-1].strip(), [], line[0] == '{')
            out.append(cur)
            continue
        if cur is None:
            continue
        for tok in line.split():
            if not re.fullmatch(r'[0-9A-Fa-f]{8}', tok):
                raise PatchError(f'cheat {cur[0]!r}: {tok!r} is not an opcode word')
            cur[1].append(int(tok, 16))
    return out


def writes(words):
    """{main offset: byte} of a cheat of type-0 main writes only; Skip otherwise."""
    out, i = {}, 0
    while i < len(words):
        w = words[i]
        kind = w >> 28
        if kind != 0:
            raise Skip(f'opcode type {kind:X} (only static writes to main convert)')
        width, region, reg = (w >> 24) & 0xF, (w >> 20) & 0xF, (w >> 16) & 0xF
        if width not in (1, 2, 4, 8):
            raise Skip(f'write width {width}')
        if region != 0:
            raise Skip(f'write to memory region {region} (only main converts)')
        n = 4 if width == 8 else 3
        if i + n > len(words):
            raise PatchError('truncated opcode')
        off = (w & 0xFF) << 32 | words[i + 1]
        value = words[i + 2] << 32 | words[i + 3] if width == 8 else words[i + 2]
        for k, b in enumerate(value.to_bytes(8, 'little')[:width]):
            out[off + k] = b   # a later write of the same byte wins, as in the cheat VM
        i += n
    del reg
    return out


class Converter:
    def __init__(self, nso, base):
        self.nso, self.base = nso, base
        self.text = (nso.text[0], nso.text[0] + nso.text[1])
        self.lifter = Lifter(base, 'c')

    def orig(self, off):
        return struct.unpack_from('<I', self.nso.image, off)[0] if off + 4 <= len(self.nso.image) else 0

    def in_text(self, off):
        return self.text[0] <= off < self.text[1]

    def fx(self, words, a):
        """The flag effect of a list of words, as gen.py Emitter.patched_effect sees one alternative."""
        fx = []
        for w in words:
            ins = self.lifter.lift(w, a)
            if ins.kind == 'op':
                e = flags.effect(ins.code or '')
                if e[0] or e[1]:
                    fx.append(e)
        if len(fx) > 1:
            raise Skip(f'main+{a - self.base:#x}: several flag-using instructions in one replacement')
        return fx[0] if fx else (False, None)

    def sites(self, byte_writes):
        """{main offset: [replacement words]} of one cheat; Skip when a write does not convert."""
        words = {}
        for off, b in byte_writes.items():
            wo = off & ~3
            cur = words.get(wo)
            if cur is None:
                cur = bytearray(struct.pack('<I', self.orig(wo)))
            cur[off - wo] = b
            words[wo] = cur
        words = {o: struct.unpack('<I', bytes(v))[0] for o, v in words.items()}
        cave = {o: w for o, w in words.items() if not self.in_text(o)}
        used, out = set(), {}
        for off in sorted(o for o in words if self.in_text(o)):
            w, was = words[off], self.orig(off)
            if w == was:
                continue
            repl = [w]
            if is_bl(w) and bl_target(w, off) in cave:
                t, body = bl_target(w, off), []
                while True:
                    if t not in cave:
                        raise Skip(f'main+{off:#x}: the cave at main+{t:#x} ends before its ret')
                    if cave[t] == RET:
                        used.add(t)
                        break
                    body.append(cave[t])
                    used.add(t)
                    t += 4
                    if len(body) > MAX_CAVE:
                        raise Skip(f'main+{off:#x}: cave longer than {MAX_CAVE} instructions')
                repl = body or [NOP]
            if is_branch(was) and not is_bl(was):
                raise Skip(f'main+{off:#x}: changes a branch ({was:08x})')
            for j, r in enumerate(repl):
                if is_branch(r) and not (is_bl(r) and j == len(repl) - 1):
                    raise Skip(f'main+{off:#x}: the replacement branches ({r:08x})')
            a = self.base + off
            new, old = self.fx(repl, a), self.fx([was], a)
            if new[1] != old[1]:
                raise Skip(f'main+{off:#x}: the replacement sets other flags than the original')
            if (new[0] or old[0] or old[1]) and is_bl(was):
                raise Skip(f'main+{off:#x}: changes a call that uses flags')
            out[off] = repl
        loose = sorted(o for o in cave if o not in used and cave[o] != self.orig(o))
        if loose:
            raise Skip(f'main+{loose[0]:#x}: a write outside .text that no call of the cheat reaches (data writes '
                       'do not convert)')
        return out


def find_file(path, build_id):
    """The cheat file: path itself, or <path>/<BID>.txt for a directory (None when it has none for this build)."""
    path = Path(path)
    if path.is_dir():
        for p in path.iterdir():
            if p.suffix.lower() == '.txt' and p.stem.lower() == build_id[:16].lower():
                return p
        return None
    return path


def merge(table, path, exefs, mods, log):
    """Adds the cheats of path (file or directory) as patch sets to table. Returns the number added."""
    main = next((m for m in mods if m[0] == 'main'), None)
    if main is None:
        raise PatchError('cheats: main is not in the layout')
    nso = Nso(Path(exefs) / 'main')
    f = find_file(path, nso.build_id)
    if f is None:
        log(f'cheats: no {nso.build_id[:16].upper()}.txt in {path}; no cheats')
        return 0
    head = f.read_text(encoding='utf-8-sig')
    m = re.search(r'BID\s*=\s*([0-9A-Fa-f]{16})', head)
    if (m and m.group(1).lower() != nso.build_id[:16]) or (f.stem.lower() != nso.build_id[:16] and not m):
        raise PatchError(f'cheats: {f} is for build {m.group(1) if m else f.stem}, main is {nso.build_id[:16]}')
    conv = Converter(nso, main[1])
    taken = {off for off in table.sites.get('main', {})}
    ids, added = set(), 0
    for name, words, master in parse(head):
        if master or not words or all(w == 0x20000000 for w in words):   # section labels: just an end
            continue
        cid = cheat_id(name)
        try:
            sites = conv.sites(writes(words))
            if not sites:
                raise Skip('changes nothing in this build (a restore code?)')
            clash = sorted(taken & sites.keys())
            if clash:
                raise Skip(f'main+{clash[0]:#x} is already patched (by a code patch or an earlier cheat)')
        except Skip as e:
            log(f'cheats: skipped [{name}]: {e}')
            continue
        base_id, k = cid, 2
        while cid in ids:
            cid, k = f'{base_id}_{k}', k + 1
        ids.add(cid)
        index = len(table.sets)
        table.sets.append((f'cheat_{cid}', '', 0))
        mod = table.sites.setdefault('main', {})
        for off, repl in sites.items():
            mod[off] = (index, repl, conv.orig(off))
            taken.add(off)
        table.cheats.append((index, cid, display_name(name), len(sites)))
        added += 1
        log(f'cheats: [{name}] -> {cid}, {len(sites)} site(s)')
    return added
