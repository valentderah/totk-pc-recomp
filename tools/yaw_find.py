#!/usr/bin/env python3
"""Find the fields of the game's camera objects that follow the camera's yaw and pitch.

Reads build\\mousecam_yaw.bin (written by TOTK_MOUSECAM_YAWPROBE=1, see runtime/mousecam.c) and, for every aligned 32-bit slot in
the recorded memory, checks whether it tracks the camera's yaw / pitch / orbit radius over the snapshots.

usage: python tools\\yaw_find.py [build\\mousecam_yaw.bin] [--top N]
"""
import struct, sys, math
import numpy as np

def load(path):
    data = open(path, "rb").read()
    snaps, cur, o = [], None, 0
    while o + 32 <= len(data):
        tag, addr, src, ln = struct.unpack_from("<4Q", data, o)
        o += 32
        if o + ln > len(data):
            break                       # truncated tail (game closed mid-write)
        blob = data[o:o + ln]; o += ln
        if tag == 5:
            cur = {"cam": np.frombuffer(blob, "<f4").astype(np.float64), "camaddr": addr, "recs": {}}
            snaps.append(cur)
        elif cur is not None:
            cur["recs"][(tag, src)] = (addr, blob)
    return snaps

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 12
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1]); args = [a for a in args if a != str(top)]
    path = args[0] if args else "build/mousecam_yaw.bin"
    snaps = load(path)
    n = len(snaps)
    print(f"{n} snapshots")
    if n < 20:
        sys.exit("too few snapshots: spin and tilt the camera while it records (needs 60+)")
    cam = np.array([s["cam"] for s in snaps])
    d = cam[:, 0:3] - cam[:, 3:6]                       # focus -> position
    yaw = np.arctan2(d[:, 0], d[:, 2])
    hor = np.hypot(d[:, 0], d[:, 2])
    pitch = np.arctan2(d[:, 1], hor)
    rad = np.linalg.norm(d, axis=1)
    print(f"yaw range {np.degrees(np.ptp(np.unwrap(yaw))):.0f} deg travelled, pitch {np.degrees(pitch.min()):.1f}..{np.degrees(pitch.max()):.1f} deg,"
          f" radius {rad.min():.2f}..{rad.max():.2f}")
    if np.ptp(np.unwrap(yaw)) < math.radians(180):
        print("WARNING: less than half a turn of yaw recorded; results will be weak")
    if np.ptp(pitch) < math.radians(8):
        print("WARNING: pitch hardly changed; pitch results will be unreliable")

    targets = {"yaw": yaw, "pitch": pitch}
    scales = [(1, "rad"), (-1, "rad, sign-flipped"), (math.pi / 180, "deg"), (-math.pi / 180, "deg, sign-flipped"),
              (2 * math.pi, "turns"), (-2 * math.pi, "turns, sign-flipped")]
    keys = {}
    for s in snaps:
        for k in s["recs"]:
            keys[k] = keys.get(k, 0) + 1
    keys = [k for k, c in keys.items() if c >= 0.9 * n]
    hits = {"yaw": [], "pitch": [], "radius": [], "sincos_yaw": [], "sincos_pitch": []}
    sy, cy, sp, cp = np.sin(yaw), np.cos(yaw), np.sin(pitch), np.cos(pitch)

    def corr(col, t):
        if col.std() < 1e-6 or t.std() < 1e-9:
            return 0.0
        return abs(np.corrcoef(col, t)[0, 1])

    for k in keys:
        rows, base = [], None
        for s in snaps:
            r = s["recs"].get(k)
            if r is None:
                rows.append(None); continue
            if base is None: base = r[0]
            rows.append(np.frombuffer(r[1][: len(r[1]) // 4 * 4], "<f4"))
        m = min(len(r) for r in rows if r is not None)
        mat = np.array([r[:m] if r is not None else np.full(m, np.nan, "<f4") for r in rows], dtype=np.float64)
        ok = np.isfinite(mat).all(axis=0) & (np.abs(mat).max(axis=0) < 1e6) & (mat.std(axis=0) > 1e-4)
        for j in np.nonzero(ok)[0]:
            col = mat[:, j]
            where = (k[0], k[1], base, int(j) * 4)
            for name, t in targets.items():
                best = (0, "")
                for sc, lbl in scales:
                    r = abs(np.mean(np.exp(1j * (sc * col - t))))
                    if r > best[0]: best = (r, lbl)
                if best[0] > 0.985:
                    hits[name].append((best[0], where, best[1], col.min(), col.max()))
            if corr(col, rad) > 0.995: hits["radius"].append((corr(col, rad), where, "", col.min(), col.max()))
            if max(corr(col, sy), corr(col, cy)) > 0.995: hits["sincos_yaw"].append((max(corr(col, sy), corr(col, cy)), where, "", col.min(), col.max()))
            if max(corr(col, sp), corr(col, cp)) > 0.995 and col.std() > 1e-3: hits["sincos_pitch"].append((max(corr(col, sp), corr(col, cp)), where, "", col.min(), col.max()))

    names = {1: "param2", 2: "camera floats region", 3: "object at param2+src", 4: "object two pointers deep"}
    for name in ("yaw", "pitch", "radius", "sincos_yaw", "sincos_pitch"):
        h = sorted(hits[name], key=lambda x: -x[0])
        print(f"\n== {name}: {len(h)} candidate slots" + (" (angle stored directly)" if name in ("yaw", "pitch") else ""))
        for score, (tag, src, base, off), lbl, lo, hi in h[:top]:
            where = names[tag]
            if tag == 3: where += f" +0x{src:x}"
            if tag == 4: where += f" (param2+0x{src >> 16:x} -> +0x{src & 0xffff:x})"
            print(f"  score {score:.4f}  {where:48s} slot +0x{off:03x}  addr(first snapshot) 0x{base + off:x}  {lbl}  range {lo:.3f}..{hi:.3f}")
    print("\nA slot that is in the yaw list AND is not part of the nine camera floats (tag 2 region at the floats) is the one to write.\n"
          "Check that it is the same object every snapshot: the address shown is from the first snapshot only.")

if __name__ == "__main__":
    main()
