"""One loaded NSO module as the generator and its analyses see it: image at a guest base, .text as words,
function seeds, and bounded-cache instruction lifting.

The function seeds and the .eh_frame_hdr reader are adapted from LAS Recomp tools/analyze.py and tools/gen.py
@ da35b83 (MIT, Copyright (c) 2026 Peiikai).
"""
import struct
from collections import OrderedDict

import numpy as np

from lifter import Insn, Lifter, Unsupported, hx
from nso import DT_INIT, DT_INIT_ARRAY, DT_INIT_ARRAYSZ, Nso, R_AARCH64_RELATIVE

RET = 0xD65F03C0
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


class Module:
    def __init__(self, name, path, base, simd='c', plugin=False, image=None, rets=(), seeds=()):
        """image, rets, seeds: a plugin's lift (recomp/patches.py `plugin`): its .text is taken from image (the plugin's
        image after its init, holding the hook stubs and trampolines the file has as zeros), the words at the image
        offsets rets become `ret` (an inline-hook stub's b to its trampoline), and the image offsets seeds are function
        starts too."""
        self.name = name
        self.nso = Nso(str(path))
        self.base = base
        self.plugin = plugin   # a mod's code module (recomp/layout.py plugin<N>): its init functions are seeds too
        n = self.nso
        self.image = bytes(n.image)
        if image is not None:
            t0, t1 = n.text[0], n.text[0] + n.text[1]
            if len(image) < t1:
                raise ValueError(f'{name}: the image after the init is shorter than its .text')
            img = bytearray(self.image)
            img[t0:t1] = image[t0:t1]
            for o in rets:
                img[o:o + 4] = RET.to_bytes(4, 'little')
            self.image = bytes(img)
        self.extra_seeds = [base + o for o in seeds]
        self.lo = base + n.text[0]
        self.hi = self.lo + n.text[1]
        self.words = np.frombuffer(self.image, dtype='<u4', count=n.text[1] // 4, offset=n.text[0])
        self.lifter = Lifter(base, simd)
        self._cache = OrderedDict()

    # ---- memory

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

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
        img, hdr = self.image, self.nso.eh_start
        if not hdr or hdr >= len(img) or self.nso.eh_end <= hdr:   # MOD0 with an empty .eh_frame_hdr range (exlaunch)
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
            starts.append(self.base + loc)
        return starts

    def init_functions(self):
        """DT_INIT and the DT_INIT_ARRAY entries (absolute addresses). rtld calls DT_INIT only; the module's DT_INIT
        runs the array through blr (SDK and exlaunch modules alike), so the entries are seeds too. An entry holds its
        target as the addend of its R_AARCH64_RELATIVE relocation, or as the word itself when no relocation names it."""
        dyn, out = self.nso.dyn, []
        if dyn.get(DT_INIT):
            out.append(self.base + dyn[DT_INIT])
        if dyn.get(DT_INIT_ARRAY):
            addends = {off: add for off, typ, _, add in self.nso.relocs if typ == R_AARCH64_RELATIVE}
            for off in range(dyn[DT_INIT_ARRAY], dyn[DT_INIT_ARRAY] + dyn.get(DT_INIT_ARRAYSZ, 0), 8):
                out.append(self.base + addends.get(off, self.nso.u64(off)))
        return out

    def seeds(self):
        """Known function starts: .eh_frame_hdr, exported FUNC symbols, R_AARCH64_RELATIVE targets in .text; for a
        plugin also its init functions (exlaunch's DT_INIT starts inside a function discovery would merge it into)."""
        starts = set(self.eh_frame_functions())
        if self.plugin:
            starts.update(self.init_functions())
        starts.update(self.extra_seeds)
        for _, info, shndx, value, _ in self.nso.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(self.base + value)
        for _, typ, _, add in self.nso.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(self.base + add)
        return sorted(a for a in starts if self.in_text(a))
