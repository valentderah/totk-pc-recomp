// The triangles of the convex hull of a small point set, for drawing the game's collision "Polytope" shapes as solids
// in the blueprint preview. The game keeps these polytopes at 4 to 81 vertices and a preview assembles up to 21 parts,
// so the hull has to be small, allocation-free and fast enough to run dozens of times in one frame.

#pragma once

#include <stdint.h>

// Fills tris with 3 * count indices into points (three floats per point, x y z) and returns the triangle count, or 0
// when points has fewer than four distinct points, is degenerate (all coplanar), has more than HULL_MAX_POINTS, or the
// hull needs more triangles than max_tris. Triangles are wound counter-clockwise seen from outside, so each triangle's
// normal points out of the solid. The output indices refer to the original points array, not a deduplicated copy.
#define HULL_MAX_POINTS 256
int hull_build(const float *points, int count, uint16_t *tris, int max_tris);
