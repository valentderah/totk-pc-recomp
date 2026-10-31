#!/usr/bin/env python3
"""Compare mousecam probe snapshots to find a game-state word that flips while Ultrahand holds an object.

Usage (from the totk-pc-recomp folder):
    python tools\\probe_diff.py build\\mousecam_probe_*.bin

Expects 6 snapshots in the order they were taken:
    00, 01  normal play        (two snapshots a second or two apart, to filter noise)
    02, 03  holding an object  (Ultrahand active)
    04, 05  object put down, camera back to normal

A word is a candidate when it is stable inside each pair and differs between normal and holding.
"strict" candidates also return to the normal value after release: those are the best flags.
"""
import glob
import struct
import sys


def load(path):
    data = open(path, "rb").read()
    recs, p = {}, 0
    while p + 32 <= len(data):
        tag, addr, src, ln = struct.unpack_from("<4Q", data, p)
        p += 32
        body = data[p:p + ln]
        p += ln
        if len(body) < ln:
            break
        recs[(tag, src)] = (addr, body)
    return recs


def f32(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def fmt(w):
    f = f32(w)
    fs = "%.4g" % f if f == f and abs(f) < 1e9 and (abs(f) > 1e-6 or w == 0) else "-"
    return "0x%08x (int %d, float %s)" % (w, w if w < 0x80000000 else w - (1 << 32), fs)


def main(args):
    # Windows cmd and PowerShell may pass "build\mousecam_probe_*.bin" through unexpanded, so expand it here.
    paths = []
    for a in args:
        paths.extend(glob.glob(a) if any(c in a for c in "*?[") else [a])
    paths = sorted(set(paths))
    print("snapshots found: %d" % len(paths))
    for p in paths:
        print("  " + p)
    if len(paths) != 6:
        sys.exit("need exactly 6 snapshot files (2 normal, 2 holding, 2 released); got %d. "
                 "If you have more, pass the six you want by name." % len(paths))
    snaps = [load(p) for p in sorted(paths)]
    names = {1: "param2 object", 2: "memory around camera floats", 3: "object pointed to from param2"}
    strict, loose = [], []
    for key in snaps[0]:
        if not all(key in s for s in snaps):
            continue
        addrs = {s[key][0] for s in snaps}
        if len(addrs) != 1:   # the object moved between snapshots: offsets are not comparable
            continue
        bodies = [s[key][1] for s in snaps]
        n = min(len(b) for b in bodies) // 4
        for i in range(n):
            w = [struct.unpack_from("<I", b, i * 4)[0] for b in bodies]
            if w[0] != w[1] or w[2] != w[3] or w[4] != w[5]:
                continue
            N, H, R = w[0], w[2], w[4]
            if H == N:
                continue
            (strict if R == N else loose).append((key, i * 4, N, H, R, next(iter(addrs))))

    def small(c):   # flags are usually 0/1 or small ints: list those first
        return (0 if max(c[2], c[3]) < 0x100 else 1, c[0][0], c[0][1], c[1])

    def show(title, items):
        print("\n== %s: %d" % (title, len(items)))
        for key, off, N, H, R, addr in sorted(items, key=small)[:40]:
            where = names.get(key[0], "?")
            src = " via param2+0x%x" % key[1] if key[0] == 3 else ""
            print("  %s%s, object at 0x%x, offset 0x%x" % (where, src, addr, off))
            print("      normal  %s\n      holding %s\n      released %s" % (fmt(N), fmt(H), fmt(R)))
        if len(items) > 40:
            print("  ... %d more not shown" % (len(items) - 40))

    show("STRICT (normal == released != holding)", strict)
    show("LOOSE (normal != holding != released, each stable)", loose)


if __name__ == "__main__":
    main(sys.argv[1:])
