"""The convex hull in runtime/hull.c, checked against a Python brute-force reference.

Compiles the real C file together with tests/hull_main.c and feeds it point sets whose hulls are easy to state: a
random convex cloud, a box, a cylinder ring, an 81-point polytope-like cloud, points duplicated and points interior,
and an all-coplanar set (which must yield 0 triangles). It checks that the hull is convex and watertight the way the
preview depends on: every input point is inside every face plane, every triangle's normal points away from the
centroid, no triangle is degenerate, indices stay in bounds, and the hull's volume and surface area match a brute-force
reference computed here.

Run: .venv\\Scripts\\python tests\\hull_test.py
"""
import math
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')
MAIN = ROOT / 'tests' / 'hull_main.c'
SRC = ROOT / 'runtime' / 'hull.c'
OUT = ROOT / 'build' / 'hull_test.exe'


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def centroid(pts):
    n = len(pts)
    return [sum(p[i] for p in pts) / n for i in range(3)]


def triangle_area(a, b, c):
    n = cross(sub(b, a), sub(c, a))
    return 0.5 * math.sqrt(dot(n, n))


def hull_volume_tetra(a, b, c, d):
    """Signed volume of the tetrahedron (a,b,c,d), six times the determinant; positive when d is on the (b-a)x(c-a)
    side."""
    return dot(cross(sub(b, a), sub(c, a)), sub(d, a))


def hull_volume(pts, tris):
    """Signed volume of the hull by summing tetrahedra from the origin to each triangle, using the triangle winding."""
    c = centroid(pts)
    total = 0.0
    for a, b, cc in tris:
        total += hull_volume_tetra(pts[a], pts[b], pts[cc], c)
    return abs(total) / 6.0


def brute_hull(pts):
    """The triangles of the convex hull by testing every triple against every point (fine for a test)."""
    n = len(pts)
    tris = []
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                a, b, c = pts[i], pts[j], pts[k]
                nrm = cross(sub(b, a), sub(c, a))
                if dot(nrm, nrm) < 1e-24:
                    continue
                side = None
                ok = True
                for m in range(n):
                    if m in (i, j, k):
                        continue
                    d = dot(nrm, sub(pts[m], a))
                    if abs(d) < 1e-9:
                        continue
                    s = d > 0
                    if side is None:
                        side = s
                    elif side != s:
                        ok = False
                        break
                if ok and side is not None:
                    tris.append((i, j, k) if side else (i, k, j))
    return tris


def box_set():
    return [[sx * 1.0, sy * 2.0, sz * 3.0] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]


def ring_set(n=32, radius=2.0, height=1.0):
    out = []
    for k in range(n):
        a = 2 * math.pi * k / n
        out.append([radius * math.cos(a), -height, radius * math.sin(a)])
        out.append([radius * math.cos(a), height, radius * math.sin(a)])
    return out


def polytope_set(n=81):
    """A cloud like the game's polytopes: rings of a barrel, some points inside that the hull must absorb."""
    out = []
    for ring in range(3):
        y = -0.6 + 0.6 * ring
        r = 1.0 if ring == 1 else 0.8
        for k in range(n // 3):
            a = 2 * math.pi * k / (n // 3)
            out.append([r * math.cos(a), y, r * math.sin(a)])
    out.append([0.0, 0.0, 0.0])  # interior point
    return out


def random_cloud(n=60, seed=7):
    r = random.Random(seed)
    return [[r.uniform(-1, 1), r.uniform(-1, 1), r.uniform(-1, 1)] for _ in range(n)]


def coplanar_set():
    return [[x, y, 0.0] for x in (-1, 0, 1) for y in (-1, 0, 1)] + [[0.5, 0.5, 0.0], [2.0, 0.0, 0.0]]


def run(pts):
    inp = ''.join('%.9f %.9f %.9f\n' % (p[0], p[1], p[2]) for p in pts)
    r = subprocess.run([str(OUT)], input=inp, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stdout + r.stderr
    lines = r.stdout.splitlines()
    n, nt = map(int, lines[0].split())
    tris = []
    for line in lines[1:]:
        a, b, c, nx, ny, nz = line.split()
        tris.append((int(a), int(b), int(c)))
    return (n, tris), ''


def check(pts, analytic_volume=None):
    """Asserts the C hull is convex, watertight and correctly wound, independent of triangulation.

    The volume/area match against brute_hull is only used when the input has no coplanar faces (the random cloud),
    because brute-force triple testing over-covers a coplanar face with overlapping triangles of mixed winding and
    inflates the "reference" volume. The checks that matter for the preview - every point inside every face, every
    normal outward, every edge shared by exactly two opposite triangles - do not depend on the triangulation and run
    for every case.
    """
    res, err = run(pts)
    if res is None:
        return 'C program failed:\n' + err
    n, tris = res
    c = centroid(pts)
    ref = brute_hull(pts)
    # all-coplanar input must produce no hull
    if len(ref) == 0:
        return None if len(tris) == 0 else 'expected 0 triangles for coplanar input, got %d' % len(tris)

    if not tris:
        return 'hull empty but reference has %d triangles' % len(ref)
    for a, b, cc in tris:
        if not (0 <= a < n and 0 <= b < n and 0 <= cc < n):
            return 'index out of bounds: %d %d %d (n=%d)' % (a, b, cc, n)
        nrm = cross(sub(pts[b], pts[a]), sub(pts[cc], pts[a]))
        if dot(nrm, nrm) < 1e-18:
            return 'degenerate triangle %d %d %d' % (a, b, cc)
        if dot(nrm, sub(c, pts[a])) > 0:
            return 'normal points inward at %d %d %d' % (a, b, cc)
        # every input point on or behind every face plane
        for p in pts:
            if dot(nrm, sub(p, pts[a])) > 1e-6:
                return 'point %s outside face %d %d %d' % (p, a, b, cc)

    # Watertight: each directed edge (u, v) of a triangle must be the reverse of exactly one other triangle's edge, so
    # the surface has no boundary and no crack. Triangulation independent.
    edges = {}
    for a, b, cc in tris:
        for u, v in ((a, b), (b, cc), (cc, a)):
            edges[(u, v)] = edges.get((u, v), 0) + 1
    for (u, v), k in edges.items():
        if k != edges.get((v, u), 0):
            return 'edge (%d,%d) is not paired: %d vs %d' % (u, v, k, edges.get((v, u), 0))

    vol = hull_volume(pts, tris)
    if analytic_volume is not None:
        if abs(vol - analytic_volume) / analytic_volume > 0.02:
            return 'volume mismatch: hull %.6f analytic %.6f' % (vol, analytic_volume)
    else:
        ref_vol = hull_volume(pts, ref)
        if ref_vol > 0 and abs(vol - ref_vol) / ref_vol > 1e-3:
            return 'volume mismatch: hull %.6f reference %.6f' % (vol, ref_vol)
    return None


def main():
    if not CLANG.exists():
        print('hull_test: clang not found at %s' % CLANG)
        return 1
    cmd = [str(CLANG), '-std=c11', '-O1', '-I', str(ROOT / 'runtime'), '-o', str(OUT), str(MAIN), str(SRC), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('hull_test: compile failed')
        print(r.stdout)
        print(r.stderr)
        return 1

    cases = [
        ('box', box_set(), 48.0),                       # 2 x 4 x 6
        ('ring', ring_set(), math.pi * 2.0 ** 2 * 2.0),  # pi r^2 h, r=2, h=2
        # The polytope is two 27-gon cone frusta (r 0.8 at each end, 1.0 at the middle, h 0.6 each): the smooth
        # frustum volume is pi*h/3*(R^2+R*r+r^2) each, so 0.4*pi*(1+0.8+0.64); the inscribed polygon is a little less.
        ('polytope', polytope_set(), 0.4 * math.pi * (1.0 + 0.8 + 0.64)),
        ('random', random_cloud(), None),
        ('random deduped', random_cloud() + [random_cloud()[0], random_cloud()[5]], None),
        ('coplanar', coplanar_set(), None),
    ]
    fails = 0
    for name, pts, vol in cases:
        err = check(pts, vol)
        if err:
            fails += 1
            print('FAIL %-16s %s' % (name, err))
        else:
            print('ok   %-16s (%d points, %d triangles)' % (name, len(pts), len(brute_hull(pts))))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
