// The triangles of the convex hull of a small point set, for drawing the game's collision "Polytope" shapes as solids
// in the blueprint preview. The game keeps these polytopes at 4 to 81 vertices and a preview assembles up to 21 parts,
// so the hull runs when the preview opens (the result is cached per part, not per frame).
//
// Method: a facet is a supporting plane of the point set - a plane every point lies on one side of - and the hull is
// exactly the collection of facets, each the convex polygon of the points coplanar with its plane. For every triple of
// distinct points the plane through it is tested against every point (O(n^4)); a supporting plane is recorded once (by
// its unit normal and offset, so the many triples of one flat face collapse to one facet), and each facet's points are
// convex-hulled in 2D and fanned into triangles. O(n^4) is tens of milliseconds at the 81-point worst case, but a
// caller computes it once and the preview caches it; it is also correct where an incremental hull is not, because a
// flat cap (a cylinder's end, a barrel's rim) is one facet that has to keep every one of its coplanar corner points.
//
// Winding: every triangle is counter-clockwise seen from outside, so its normal points out of the solid.
#include "hull.h"

#include <math.h>

#define MAX_FACETS 512
#define MAX_PLANE_PTS 256

typedef struct { float x, y, z; } V;

static V sub(V a, V b) { return (V){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V cross(V a, V b) { return (V){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len2(V a) { return dot(a, a); }
static V mul(V a, float s) { return (V){a.x * s, a.y * s, a.z * s}; }

int hull_build(const float *points, int count, uint16_t *tris, int max_tris) {
    if (count < 4 || count > HULL_MAX_POINTS) return 0;

    float lo[3] = {points[0], points[1], points[2]}, hi[3] = {points[0], points[1], points[2]};
    for (int i = 1; i < count; i++)
        for (int k = 0; k < 3; k++) {
            float v = points[3 * i + k];
            if (v < lo[k]) lo[k] = v;
            if (v > hi[k]) hi[k] = v;
        }
    float scale = 0.0f;
    for (int k = 0; k < 3; k++) {
        float s = hi[k] - lo[k];
        if (s > scale) scale = s;
    }
    if (scale <= 0.0f) return 0;
    float eps = scale * 1e-6f;            // distance a point may sit off a plane and still count as on it
    float area_eps = scale * scale * 1e-9f;  // squared length that still counts as zero (duplicate / degenerate)

    // Drop coincident points; idx maps hull position to the original index so the output refers to the caller's array.
    int idx[HULL_MAX_POINTS], n = 0;
    for (int i = 0; i < count; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++) {
            float dx = points[3 * i] - points[3 * idx[j]], dy = points[3 * i + 1] - points[3 * idx[j] + 1];
            float dz = points[3 * i + 2] - points[3 * idx[j] + 2];
            if (dx * dx + dy * dy + dz * dz <= area_eps) { dup = 1; break; }
        }
        if (!dup) idx[n++] = i;
    }
    if (n < 4) return 0;

    // A solid needs at least one plane with points strictly on both sides... equivalently, at least one supporting
    // plane with a coplanar point set that does not contain every point. If every point is coplanar with the first
    // supporting plane we find, the input is flat (a polygon, not a solid) and there is no hull to draw.
    //
    // Collect facets: for each triple, the unit normal points out (all points at distance <= eps), the facet is the
    // points coplanar with it, and the plane is keyed by a rounded unit normal plus offset so triples of one face
    // collapse. A hash table is not needed: the number of facets is small (at most 2n-4).
    typedef struct { int v[MAX_PLANE_PTS]; int nv; int kn[3]; int kd; } Facet;
    Facet facets[MAX_FACETS];
    int nf = 0;

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            for (int k = j + 1; k < n; k++) {
                V a = *(V *)(points + 3 * idx[i]), b = *(V *)(points + 3 * idx[j]), c = *(V *)(points + 3 * idx[k]);
                V nrm = cross(sub(b, a), sub(c, a));
                float l2 = len2(nrm);
                if (l2 <= area_eps) continue;
                float il = 1.0f / sqrtf(l2);
                nrm = mul(nrm, il);

                float d = dot(nrm, a);
                // The plane is supporting only when every point sits on one side of it. Early out as soon as a point
                // is found strictly on each side, which is what keeps this from degenerating to a full n^4 inner loop.
                int has_pos = 0, has_neg = 0;
                for (int m = 0; m < n; m++) {
                    float s = dot(nrm, *(V *)(points + 3 * idx[m])) - d;
                    if (s > eps) has_pos = 1;
                    else if (s < -eps) has_neg = 1;
                    if (has_pos && has_neg) break;
                }
                if (has_pos && has_neg) continue;  // points on both sides: not a supporting plane
                if (has_pos) { nrm = mul(nrm, -1.0f); d = -d; }  // flip so the normal points out

                // The facet's vertices: every point coplanar with this supporting plane.
                int verts[MAX_PLANE_PTS], nv = 0;
                for (int m = 0; m < n; m++) {
                    float s = dot(nrm, *(V *)(points + 3 * idx[m])) - d;
                    if (s > -eps && s < eps) verts[nv++] = m;
                }
                if (nv < 3) continue;

                // Plane key: the outward unit normal rounded, and the offset in unit length, so that two triples of
                // the same flat face produce the same key and are kept once.
                int kn[3] = {(int)lroundf(nrm.x * 1e5f), (int)lroundf(nrm.y * 1e5f), (int)lroundf(nrm.z * 1e5f)};
                int kd = (int)lroundf(d * 1e5f);
                int seen = 0;
                for (int f = 0; f < nf; f++) {
                    if (facets[f].kn[0] == kn[0] && facets[f].kn[1] == kn[1] && facets[f].kn[2] == kn[2] &&
                        facets[f].kd == kd) { seen = 1; break; }
                }
                if (seen) continue;
                if (nf >= MAX_FACETS) continue;
                Facet *F = &facets[nf++];
                F->nv = nv;
                F->kn[0] = kn[0]; F->kn[1] = kn[1]; F->kn[2] = kn[2]; F->kd = kd;
                for (int m = 0; m < nv; m++) F->v[m] = verts[m];
            }
        }
    }
    if (nf == 0) return 0;

    // Reject a flat input: when one facet holds every point, the set is a polygon, not a solid.
    int all_on_one = 0;
    for (int f = 0; f < nf; f++) {
        if (facets[f].nv == n) { all_on_one = 1; break; }
    }
    if (all_on_one) return 0;

    // Fan each facet: project its coplanar points to 2D by dropping the axis of the largest normal component, convex
    // hull in 2D (monotone chain), then fan from one hull vertex, oriented so the normal points out.
    int nt = 0;
    for (int f = 0; f < nf; f++) {
        Facet *F = &facets[f];
        V nrm = (V){F->kn[0] * 1e-5f, F->kn[1] * 1e-5f, F->kn[2] * 1e-5f};

        int drop = (fabsf(nrm.x) >= fabsf(nrm.y) && fabsf(nrm.x) >= fabsf(nrm.z)) ? 0 :
                   (fabsf(nrm.y) >= fabsf(nrm.z) ? 1 : 2);
        int ax1 = drop == 0 ? 1 : 0;
        int ax2 = drop == 0 ? 2 : (drop == 1 ? 2 : 1);

        // 2D points (u, v) with the original index, then a monotone-chain hull on (u, v).
        float uv[MAX_PLANE_PTS][2];
        int orig[MAX_PLANE_PTS];
        for (int m = 0; m < F->nv; m++) {
            int pi = idx[F->v[m]];
            float cc[3] = {points[3 * pi], points[3 * pi + 1], points[3 * pi + 2]};
            uv[m][0] = cc[ax1];
            uv[m][1] = cc[ax2];
            orig[m] = pi;
        }
        // Dedupe 2D points (a facet should have none, but a rounded key can merge two near-coplanar facets).
        int nu = 0;
        for (int m = 0; m < F->nv; m++) {
            int d2 = 0;
            for (int t = 0; t < nu; t++) {
                float du = uv[m][0] - uv[t][0], dv = uv[m][1] - uv[t][1];
                if (du * du + dv * dv <= area_eps) { d2 = 1; break; }
            }
            if (!d2) { uv[nu][0] = uv[m][0]; uv[nu][1] = uv[m][1]; orig[nu] = orig[m]; nu++; }
        }
        if (nu < 3) continue;

        // Sort by (u, v).
        for (int a2 = 0; a2 < nu; a2++)
            for (int b2 = a2 + 1; b2 < nu; b2++)
                if (uv[a2][0] > uv[b2][0] || (uv[a2][0] == uv[b2][0] && uv[a2][1] > uv[b2][1])) {
                    float tu = uv[a2][0]; uv[a2][0] = uv[b2][0]; uv[b2][0] = tu;
                    float tv = uv[a2][1]; uv[a2][1] = uv[b2][1]; uv[b2][1] = tv;
                    int ti = orig[a2]; orig[a2] = orig[b2]; orig[b2] = ti;
                }

        // Monotone chain (cross > 0 keeps the polygon counter-clockwise in (u, v)).
        int hull[MAX_PLANE_PTS], nh = 0;
        for (int m = 0; m < nu; m++) {
            while (nh >= 2) {
                float x1 = uv[hull[nh - 1]][0] - uv[hull[nh - 2]][0];
                float y1 = uv[hull[nh - 1]][1] - uv[hull[nh - 2]][1];
                float x2 = uv[m][0] - uv[hull[nh - 1]][0];
                float y2 = uv[m][1] - uv[hull[nh - 1]][1];
                if (x1 * y2 - y1 * x2 <= 0) nh--; else break;
            }
            hull[nh++] = m;
        }
        int lower = nh;
        for (int m = nu - 2; m >= 0; m--) {
            while (nh > lower) {
                float x1 = uv[hull[nh - 1]][0] - uv[hull[nh - 2]][0];
                float y1 = uv[hull[nh - 1]][1] - uv[hull[nh - 2]][1];
                float x2 = uv[m][0] - uv[hull[nh - 1]][0];
                float y2 = uv[m][1] - uv[hull[nh - 1]][1];
                if (x1 * y2 - y1 * x2 <= 0) nh--; else break;
            }
            hull[nh++] = m;
        }
        if (nh > 1) nh--;  // the last point repeats the first

        if (nh < 3) continue;

        // Fan from hull[0]; orient so the triangle normal matches the facet's outward normal.
        for (int m = 1; m + 1 < nh; m++) {
            int p0 = orig[hull[0]], p1 = orig[hull[m]], p2 = orig[hull[m + 1]];
            V a = *(V *)(points + 3 * p0), b = *(V *)(points + 3 * p1), c = *(V *)(points + 3 * p2);
            V tn = cross(sub(b, a), sub(c, a));
            if (dot(tn, nrm) < 0) { int t = p1; p1 = p2; p2 = t; }
            if (nt + 1 > max_tris) return 0;
            tris[3 * nt] = (uint16_t)p0;
            tris[3 * nt + 1] = (uint16_t)p1;
            tris[3 * nt + 2] = (uint16_t)p2;
            nt++;
        }
    }
    return nt;
}
