#!/usr/bin/env python3
"""
find_match.py - find the counterpart of a function from one game build in another build.

Usage (run from the totk-pc-recomp folder):
  python tools\find_match.py --old "C:\\Users\\Chandler\\Downloads\\main" ^
      --new build\\extracted\\v143\\exefs\\main 0xa70604 [--words 120]

How it works: takes N instruction words from the old build at OFFSET, blanks the
fields that change between builds (bl/b/cond-branch targets, adrp/adr, literal loads),
then votes for where that sequence lines up in the new build. Needs: pip install numpy
"""
import argparse
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy: run  pip install numpy  and retry")

from find_calls import load_text, classify


def mask(a):
    a = a.astype(np.uint32)
    out = a.copy()
    m = (a & 0x7C000000) == 0x14000000                      # b / bl
    out[m] = a[m] & 0xFC000000
    m = (a & 0xFF000010) == 0x54000000                      # b.cond
    out[m] = a[m] & 0xFF00001F
    m = (a & 0x7E000000) == 0x34000000                      # cbz / cbnz
    out[m] = a[m] & 0xFF00001F
    m = (a & 0x7E000000) == 0x36000000                      # tbz / tbnz
    out[m] = a[m] & 0xFFF8001F
    m = (a & 0x1F000000) == 0x10000000                      # adr / adrp
    out[m] = a[m] & 0x9F00001F
    m = (a & 0x3B000000) == 0x18000000                      # ldr literal
    out[m] = a[m] & 0xFF00001F
    return out


def words_of(text):
    n = len(text) // 4
    return np.frombuffer(text[: n * 4], dtype="<u4")


def callers_of(words, ti):
    idx = np.flatnonzero((words & 0x7C000000) == 0x14000000)
    imm = (words[idx] & 0x03FFFFFF).astype(np.int64)
    imm = np.where(imm & 0x02000000, imm - 0x04000000, imm)
    return idx[(idx + imm) == ti]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--words", type=int, default=120)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("offset")
    a = ap.parse_args()

    old_text, _ = load_text(a.old)
    new_text, _ = load_text(a.new)
    old = words_of(old_text)
    new = words_of(new_text)
    off = int(a.offset, 16) // 4
    fp = mask(old[off: off + a.words])
    nm = mask(new)
    print("old: %d words, new: %d words, fingerprint: %d words" % (len(old), len(new), len(fp)))

    order = np.argsort(nm, kind="stable")
    sorted_vals = nm[order]

    votes = []
    used = 0
    for k, v in enumerate(fp):
        lo = np.searchsorted(sorted_vals, v, "left")
        hi = np.searchsorted(sorted_vals, v, "right")
        if hi - lo == 0 or hi - lo > 300:     # skip missing or very common words
            continue
        used += 1
        votes.append(order[lo:hi].astype(np.int64) - k)
    if not votes:
        sys.exit("no usable words in fingerprint")
    votes = np.concatenate(votes)
    starts, counts = np.unique(votes, return_counts=True)
    best = np.argsort(-counts)[: a.top]

    print("usable fingerprint words: %d\n" % used)
    for b in best:
        s = int(starts[b])
        if s < 0:
            continue
        c = callers_of(new, s)
        prev = int(new[s - 1]) if s > 0 else 0
        print("candidate %#x  votes %d/%d (%.0f%%)  callers=%d  prev=%s  first=%08x %s" % (
            s * 4, counts[b], used, 100.0 * counts[b] / used, len(c),
            "ret" if prev == 0xD65F03C0 else "%08x" % prev, new[s], classify(int(new[s]))))
        if len(c):
            print("   callers: " + " ".join("%x:%08x" % (int(i) * 4, int(new[i])) for i in c[:6]))


if __name__ == "__main__":
    main()
