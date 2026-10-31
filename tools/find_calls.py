#!/usr/bin/env python3
"""
find_calls.py - list direct callers (bl/b) of an offset in a Switch 'main' NSO.

Usage:
  python tools/find_calls.py --main build/extracted/v143/exefs/main 0xa70604
  python tools/find_calls.py --main <path> 0xa70604 0xa2e4b0 --context 8

Offsets are relative to the start of .text (same as the mod's "main+0x..." values).
Works on a real NSO (LZ4-compressed segments need: pip install lz4) or on an
already-decompressed flat .text image (auto-detected by the missing NSO0 magic).

Output:
  - words around the target (is it a function start or the middle of one?)
  - callers as  OFFSET:WORD  (OFFSET = caller address, WORD = its instruction, hex)
"""
import argparse
import struct
import sys


def load_text(path):
    data = open(path, "rb").read()
    if data[:4] != b"NSO0":
        return data, "flat image (no NSO header)"
    flags = struct.unpack_from("<I", data, 0x0C)[0]
    # segment headers: file_off, mem_off, size  (text, rodata, data) at 0x10, 0x20, 0x30
    file_off, mem_off, size = struct.unpack_from("<III", data, 0x10)
    comp_size = struct.unpack_from("<I", data, 0x60)[0]
    if flags & 1:
        try:
            import lz4.block
        except ImportError:
            sys.exit("main is LZ4-compressed: run  pip install lz4  and retry")
        text = lz4.block.decompress(data[file_off:file_off + comp_size], uncompressed_size=size)
    else:
        text = data[file_off:file_off + size]
    # place at mem_off (normally 0)
    return b"\0" * mem_off + text, "NSO .text (%#x bytes)" % size


def classify(w):
    notes = []
    if (w & 0xFC000000) == 0x94000000:
        notes.append("bl")
    elif (w & 0xFC000000) == 0x14000000:
        notes.append("b")
    if w == 0xD65F03C0:
        notes.append("ret")
    if (w & 0xFFC003E0) == 0xA98003E0 and (w & 0x7C00) == (30 << 10) or (w & 0xFFC07FFF) == 0xA9807BFD:
        notes.append("stp x29,x30 (prologue)")
    if (w & 0xFF0003FF) == 0xD10003FF:
        notes.append("sub sp (prologue)")
    if (w & 0xFFC003E0) == 0xA8C003E0 or (w & 0xFFC07FFF) == 0xA8C07BFD:
        notes.append("ldp (epilogue?)")
    return ",".join(notes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", required=True)
    ap.add_argument("--context", type=int, default=6, help="words to show each side")
    ap.add_argument("offsets", nargs="+", help="hex offsets into .text")
    a = ap.parse_args()

    text, kind = load_text(a.main)
    n = len(text) // 4
    words = struct.unpack("<%dI" % n, text[: n * 4])
    print("loaded:", kind, "-", n, "words")

    for s in a.offsets:
        target = int(s, 16)
        if target % 4 or target // 4 >= n:
            print("\n== %#x: not a valid 4-aligned offset inside .text" % target)
            continue
        ti = target // 4
        print("\n== target %#x" % target)
        for i in range(max(0, ti - a.context), min(n, ti + a.context + 1)):
            mark = "  <== target" if i == ti else ""
            print("  %08x: %08x  %s%s" % (i * 4, words[i], classify(words[i]), mark))

        callers = []
        for i, w in enumerate(words):
            top = w & 0xFC000000
            if top == 0x94000000 or top == 0x14000000:
                imm = w & 0x03FFFFFF
                if imm & 0x02000000:
                    imm -= 0x04000000
                if i + imm == ti:
                    callers.append((i * 4, w))
        print("\n  direct callers (bl/b): %d" % len(callers))
        for off, w in callers:
            print("  %x:%08x  %s" % (off, w, "bl" if (w >> 26) == 0x25 else "b"))
        if not callers:
            print("  none - target is probably not a function start, or is called indirectly")


if __name__ == "__main__":
    main()
