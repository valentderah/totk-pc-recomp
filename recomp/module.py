"""One loaded NSO module as the generator and its analyses see it: image at a guest base, .text as words,
function seeds, and bounded-cache instruction lifting.

Code lines of the patch table (recomp/patches.py `code`, a cheat's code caves) go into the image's padding after
.text, and the code the generator lifts then runs to their end: `hi` covers them, `cave` is where they start.

The function seeds and the .eh_frame_hdr reader are adapted from LAS Recomp tools/analyze.py and tools/gen.py
@ da35b83 (MIT, Copyright (c) 2026 Peiikai).
"""
import struct
from collections import OrderedDict

import numpy as np

from lifter import Insn, Lifter, Unsupported, hx
from nso import Nso, R_AARCH64_RELATIVE

LIFT_CACHE = 1 << 16  # lifted instructions kept per module; analyses revisit nearby addresses only


def _read_encoded(img, off, enc, pc_base):
    fmt, app = enc & 0x0F, enc & 0x70
    if fmt == 0x0B:  # sdata4
        v, size = struct.unpack_from('<i', img, off)[0], 4
    elif fmt == 0x03:  # udata4
        v, size = struct.unpack_from('<I', img, off)[0], 4
    elif fmt in (0x0C, 0x04):  # sdata8, udata8
        v, size = struct.unpack_from('<q' if fmt == 0x0C else '<Q', img, off)[0], 8
    else:
        raise ValueError(f'unsupported pointer encoding {enc:#x}')
    if app == 0x10:  # pcrel
        v += off
    elif app == 0x30:  # datarel
        v += pc_base
    return v, size


def eh_frame_starts(nso, base):
    """Function starts from the NSO's .eh_frame_hdr binary search table (absolute addresses for base)."""
    img, hdr = nso.image, nso.eh_start
    if not hdr or hdr >= len(img):
        return []
    _, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
    off = hdr + 4
    _, sz = _read_encoded(img, off, ptr_enc, hdr)
    off += sz
    count, sz = _read_encoded(img, off, cnt_enc, hdr)
    off += sz
    starts = []
    for _ in range(count):
        loc, sz = _read_encoded(img, off, tbl_enc, hdr)
        off += sz
        _, sz = _read_encoded(img, off, tbl_enc, hdr)
        off += sz
        starts.append(base + loc)
    return starts


class Module:
    def __init__(self, name, path, base, simd='c', code=None):
        self.name = name
        self.nso = Nso(str(path))
        self.base = base
        n = self.nso
        image, end = bytearray(n.image), n.text[0] + n.text[1]
        self.runs = []   # [(start, end)] module offsets of the code lines' runs (lines that touch join)
        for off, words in sorted((code or {}).items()):
            struct.pack_into(f'<{len(words)}I', image, off, *words)
            if self.runs and self.runs[-1][1] == off:
                self.runs[-1] = (self.runs[-1][0], off + 4 * len(words))
            else:
                self.runs.append((off, off + 4 * len(words)))
            end = max(end, off + 4 * len(words))
        self.image = bytes(image)
        self.lo = base + n.text[0]
        self.cave = self.lo + n.text[1]   # the game's code ends here; code lines follow
        self.hi = base + end
        self.words = np.frombuffer(self.image, dtype='<u4', count=(end - n.text[0]) // 4, offset=n.text[0])
        self.lifter = Lifter(base, simd)
        self._cache = OrderedDict()

    # ---- memory

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def in_cave(self, a):
        """a is in the code lines' part of the code (after the game's .text)."""
        return self.cave <= a < self.hi

    def in_image(self, a, n=1):
        return self.base <= a and a + n <= self.base + len(self.image)

    def word(self, a):
        return int(self.words[(a - self.lo) >> 2])

    def read(self, a, n, signed=False):
        """Little-endian integer of n bytes at guest address a (unrelocated image)."""
        o = a - self.base
        return int.from_bytes(self.image[o:o + n], 'little', signed=signed)

    # ---- lifting

    def lift(self, a):
        """Lifted instruction at a; Unsupported becomes an rt_udf trap."""
        ins = self._cache.get(a)
        if ins is not None:
            self._cache.move_to_end(a)
            return ins
        try:
            ins = self.lifter.lift(self.word(a), a)
        except Unsupported as e:
            ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
        self._cache[a] = ins
        if len(self._cache) > LIFT_CACHE:
            self._cache.popitem(last=False)
        return ins

    # ---- function seeds

    def eh_frame_functions(self):
        """Function starts from .eh_frame_hdr's binary search table (absolute addresses)."""
        return eh_frame_starts(self.nso, self.base)

    def seeds(self):
        """Known function starts: .eh_frame_hdr, exported FUNC symbols, R_AARCH64_RELATIVE targets in .text, and the
        start of each code line run and of the padding before the first (so .text's last function ends where it did)."""
        starts = set(self.eh_frame_functions())
        if self.runs:
            starts.add(self.cave)
            starts.update(self.base + a for a, _ in self.runs)
        for _, info, shndx, value, _ in self.nso.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(self.base + value)
        for _, typ, _, add in self.nso.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(self.base + add)
        return sorted(a for a in starts if self.in_text(a))
