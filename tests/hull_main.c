// Test main for runtime/hull.c: reads a point set from stdin (one "x y z" triple per line), builds the hull, and
// prints each triangle's three indices and normal. Compiled and driven by tests/hull_test.py, which checks the result
// against a Python brute-force reference.
#include <stdio.h>
#include <stdlib.h>

#include "hull.h"

int main(void) {
    float pts[HULL_MAX_POINTS * 3];
    int n = 0;
    while (n < HULL_MAX_POINTS && scanf("%f %f %f", &pts[3 * n], &pts[3 * n + 1], &pts[3 * n + 2]) == 3) n++;
    if (n == 0) return 2;

    uint16_t tris[3 * (2 * HULL_MAX_POINTS)];
    int nt = hull_build(pts, n, tris, 2 * HULL_MAX_POINTS);
    printf("%d %d\n", n, nt);
    for (int t = 0; t < nt; t++) {
        int a = tris[3 * t], b = tris[3 * t + 1], c = tris[3 * t + 2];
        float ax = pts[3 * a], ay = pts[3 * a + 1], az = pts[3 * a + 2];
        float bx = pts[3 * b], by = pts[3 * b + 1], bz = pts[3 * b + 2];
        float cx = pts[3 * c], cy = pts[3 * c + 1], cz = pts[3 * c + 2];
        float abx = bx - ax, aby = by - ay, abz = bz - az;
        float acx = cx - ax, acy = cy - ay, acz = cz - az;
        float nx = aby * acz - abz * acy, ny = abz * acx - abx * acz, nz = abx * acy - aby * acx;
        printf("%d %d %d %.6f %.6f %.6f\n", a, b, c, nx, ny, nz);
    }
    return 0;
}
