"""Render the picker's 3D preview to a text picture, from the real box/camera math.

Mirror of preview_load in runtime/blueprint_picker.c: each part becomes a box turned by its own rotation, its
camera-facing faces are kept by winding, sorted far to near, and drawn. Fed by the real CAI parser's offsets.

This exists because the only honest check of "does it look like a build" is to look at it. Run it on a real .cai and the
picture should show the blueprint's structure - a wheel-and-beam cart should read as one, not as scattered dots.

Usage: .venv\\Scripts\\python tests\\preview_picture.py <file.cai> [--width 64 --height 26] [--yaw DEG] [--pitch DEG]
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

# The six faces of a box, wound counter-clockwise seen from outside (design 5.4).
FACE = [[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]]
FACE_NORMAL = [(0, 0, -1), (0, 0, 1), (0, -1, 0), (0, 1, 0), (-1, 0, 0), (1, 0, 0)]
LIGHT = (0.40, 0.82, 0.41)
DIST = 4.0


def parse(path):
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
        pos.append((m[3], m[7], m[11]))
        # rot[world][local]: row-major upper-left 3x3
        rot.append([[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]])
    bb = struct.unpack_from('<6f', blob, BBOX)
    return pos, rot, bb[:3], bb[3:]


def make_projector(yaw, pitch, bmin, bmax):
    cy, syn = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)

    def project(p):
        n = [((p[i] - bmin[i]) / (bmax[i] - bmin[i])) * 2.0 - 1.0 if (bmax[i] - bmin[i]) > 0.001 else 0.0
             for i in range(3)]
        x1 = cy * n[0] + syn * n[2]
        z1 = -syn * n[0] + cy * n[2]
        y1 = cp * n[1] - sp * z1
        z2 = sp * n[1] + cp * z1
        k = DIST / (DIST + z2)
        return 0.5 + x1 * 0.36 * k, 0.5 - y1 * 0.36 * k, z2

    return project


def area2(q):
    a = 0.0
    for i in range(4):
        j = (i + 1) & 3
        a += q[i][0] * q[j][1] - q[j][0] * q[i][1]
    return a


def build_faces(pos, rot, bmin, bmax, yaw, pitch):
    project = make_projector(yaw, pitch, bmin, bmax)
    span = max(bmax[i] - bmin[i] for i in range(3))
    half = span * 0.06
    faces = []
    for k in range(len(pos)):
        corners = []
        for c in range(8):
            lx = half if (c & 1) else -half
            ly = half if (c & 2) else -half
            lz = half if (c & 4) else -half
            corners.append(tuple(pos[k][i] + rot[k][i][0] * lx + rot[k][i][1] * ly + rot[k][i][2] * lz
                                 for i in range(3)))
        proj = [project(c) for c in corners]
        for f in range(6):
            q = [(proj[c][0], proj[c][1]) for c in FACE[f]]
            if area2(q) >= 0.0:
                continue
            depth = sum(proj[c][2] for c in FACE[f]) / 4.0
            nrm = [sum(rot[k][i][j] * FACE_NORMAL[f][j] for j in range(3)) for i in range(3)]
            lit = max(0.0, sum(nrm[i] * LIGHT[i] for i in range(3)))
            shade = (0.30 + 0.70 * lit) * (0.65 + 0.35 * (1.0 - (depth + 1.6) / 3.2))
            faces.append((depth, q, max(0.12, min(1.0, shade))))
    faces.sort(key=lambda t: -t[0])
    return faces


def render(faces, w, h):
    """Fill a character grid: a face's shade picks the character, and a nearer face overwrites a further one."""
    grid = [[' '] * w for _ in range(h)]
    ramp = '.:-=+*#%@'
    for depth, quad, shade in faces:
        # Rasterise the quad by its bounding box and an inside test (two triangles is enough for a convex quad).
        xs = [p[0] for p in quad]
        ys = [p[1] for p in quad]
        x0, x1 = max(0, int(min(xs) * (w - 1))), min(w - 1, int(max(xs) * (w - 1) + 0.999))
        y0, y1 = max(0, int(min(ys) * (h - 1))), min(h - 1, int(max(ys) * (h - 1) + 0.999))
        ch = ramp[min(len(ramp) - 1, int(shade * (len(ramp) - 1)))]

        def inside(px, py):
            # standard crossing test against the quad's edges
            c = False
            for i in range(4):
                j = (i + 1) & 3
                ax, ay = quad[i]
                bx, by = quad[j]
                if (ay > py) != (by > py) and px < ax + (py - ay) / (by - ay + 1e-12) * (bx - ax):
                    c = not c
            return c

        for gy in range(y0, y1 + 1):
            py = (gy + 0.5) / h
            for gx in range(x0, x1 + 1):
                px = (gx + 0.5) / w
                if inside(px, py):
                    grid[gy][gx] = ch
    return '\n'.join(''.join(r) for r in grid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--width', type=int, default=64)
    ap.add_argument('--height', type=int, default=26)
    ap.add_argument('--yaw', type=float, default=35.0)
    ap.add_argument('--pitch', type=float, default=28.0)
    args = ap.parse_args()

    pos, rot, bmin, bmax = parse(args.file)
    yaw, pitch = math.radians(args.yaw), math.radians(args.pitch)
    faces = build_faces(pos, rot, bmin, bmax, yaw, pitch)
    print('%s: %d parts, bbox (%.1f %.1f %.1f) - (%.1f %.1f %.1f), yaw %.0f pitch %.0f'
          % (Path(args.file).name, len(pos), *bmin, *bmax, args.yaw, args.pitch))
    print('visible faces: %d (%.1f per part)' % (len(faces), len(faces) / max(1, len(pos))))
    print()
    print('+' + '-' * args.width + '+')
    for line in render(faces, args.width, args.height).splitlines():
        print('|' + line + '|')
    print('+' + '-' * args.width + '+')
    return 0


if __name__ == '__main__':
    sys.exit(main())
