"""Render the picker's 3D preview to a text picture, from the real projection.

Mirror of preview_project + preview_load in runtime/blueprint_picker.c, fed by the real CAI parser's values (position and
rotation per part). It exists to check that the fixed three-quarter camera actually produces a readable build: the
shapes below should look like the blueprints they come from, which a single projected point cannot tell you.

Usage: .venv\\Scripts\\python tests\\preview_picture.py <file.cai> [--width 60 --height 26]
"""
import argparse
import math
import struct
import sys
from pathlib import Path

BLOB = 6688
ACTOR0 = 12
ACTOR_BYTES = 280
NAME_OFF = 152
NAME_SIZE = 128
BBOX = 6144
MAX_ACTORS = 21

# The camera constants from preview_project.
CY, SYN = 0.819152, 0.573576     # cos / sin 35 degrees (yaw)
CP, SP = 0.882948, 0.469472      # cos / sin 28 degrees (pitch)
DIST = 4.0
SCALE = 0.36


def parse(path):
    """(positions, rotations, bbox_min, bbox_max) from a .cai, through the same offsets the C parser uses."""
    data = Path(path).read_bytes()
    if len(data) < BLOB:
        raise SystemExit('too short for a blueprint')
    blob = data[:BLOB]
    if blob[:6] != b'CmbAct':
        raise SystemExit('not a CmbAct blueprint')
    actors = blob[9]
    if not 1 <= actors <= MAX_ACTORS or blob[10] != actors - 1:
        raise SystemExit('bad actor count')
    pos, rot = [], []
    for k in range(actors):
        a = blob[ACTOR0 + k * ACTOR_BYTES:ACTOR0 + (k + 1) * ACTOR_BYTES]
        m = struct.unpack_from('<12f', a, 0)
        # Row-major: elements 3, 7, 11 are the position; the upper-left 3x3 is the rotation.
        pos.append((m[3], m[7], m[11]))
        rot.append([[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]])
    bb = struct.unpack_from('<6f', blob, BBOX)
    return pos, rot, bb[:3], bb[3:]


def project(p, bmin, bmax):
    n = []
    for i in range(3):
        span = bmax[i] - bmin[i]
        n.append(((p[i] - bmin[i]) / span) * 2.0 - 1.0 if span > 0.001 else 0.0)
    x1 = CY * n[0] + SYN * n[2]
    z1 = -SYN * n[0] + CY * n[2]
    y1 = CP * n[1] - SP * z1
    z2 = SP * n[1] + CP * z1
    k = DIST / (DIST + z2)
    return 0.5 + x1 * SCALE * k, 0.5 - y1 * SCALE * k, z2


def render(pos, rot, bmin, bmax, w, h):
    pts = []
    for k, p in enumerate(pos):
        sx, sy, depth = project(p, bmin, bmax)
        up = rot[k][1][1]                      # the part's local +y in world space: 1 upright, 0 lying flat
        pts.append((depth, sx, sy, max(0.0, min(2.0, 1.0 - up))))
    pts.sort(key=lambda t: -t[0])              # far first, the painter's order the C layer produces

    grid = [[' '] * w for _ in range(h)]
    shade = '.:-=+*#%@'
    for depth, sx, sy, tilt in pts:
        gx = int(sx * (w - 1))
        gy = int(sy * (h - 1))
        if not (0 <= gx < w and 0 <= gy < h):
            continue
        a = max(0.2, min(1.0, 1.0 - (depth + 1.6) / 3.2))
        ch = shade[min(len(shade) - 1, int(a * (len(shade) - 1)))]
        # A tilted part leans: draw it with a slash rather than a dot, so orientation is visible in the picture.
        if tilt > 0.5:
            ch = 'x'
        grid[gy][gx] = ch
    return '\n'.join(''.join(row) for row in grid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--width', type=int, default=60)
    ap.add_argument('--height', type=int, default=26)
    args = ap.parse_args()
    pos, rot, bmin, bmax = parse(args.file)
    print('%s: %d parts, bbox (%.1f %.1f %.1f) - (%.1f %.1f %.1f)' % (Path(args.file).name, len(pos), *bmin, *bmax))
    tilted = sum(1 for r in rot if r[1][1] < 0.5)
    print('parts lying flat (local +y not up): %d of %d' % (tilted, len(rot)))
    print()
    print('+' + '-' * args.width + '+')
    for line in render(pos, rot, bmin, bmax, args.width, args.height).splitlines():
        print('|' + line + '|')
    print('+' + '-' * args.width + '+')
    return 0


if __name__ == '__main__':
    sys.exit(main())
