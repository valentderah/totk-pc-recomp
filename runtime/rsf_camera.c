#include <math.h>
#include "rsf_camera.h"
#include <string.h>

static void multiply(const double *a, const double *b, double *out) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double value = 0;
        for (int k = 0; k < 4; k++) value += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = value;
    }
}

static int inverse(const double *matrix, double *out) {
    double rows[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++)
        rows[r][c] = c < 4 ? matrix[r * 4 + c] : c - 4 == r;
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) if (fabs(rows[r][c]) > fabs(rows[pivot][c])) pivot = r;
        if (!isfinite(rows[pivot][c]) || fabs(rows[pivot][c]) < 1e-15) return 0;
        for (int k = 0; k < 8; k++) { double value = rows[c][k]; rows[c][k] = rows[pivot][k]; rows[pivot][k] = value; }
        double divisor = rows[c][c];
        for (int k = 0; k < 8; k++) rows[c][k] /= divisor;
        for (int r = 0; r < 4; r++) if (r != c) {
            double factor = rows[r][c];
            for (int k = 0; k < 8; k++) rows[r][k] -= factor * rows[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = rows[r][c + 4];
    return 1;
}

static int sdk_matrix(const double *matrix, float *out) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float value = (float)matrix[c * 4 + r];
        if (!isfinite(value)) return 0;
        out[r * 4 + c] = value;
    }
    return 1;
}

int rsf_camera_matrices(const float *projection, const float *view_projection,
    const float *previous, RsfCameraMatrices *out) {
    if (!projection || !view_projection || !out) return 0;
    double p[16], vp[16], old[16], pi[16], vi[16], view[16], world[16], api[16], api_inverse[16];
    const double normalize[16] = {1,0,0,0, 0,1,0,0, 0,0,0.5,0.5, 0,0,0,1};
    const double denormalize[16] = {1,0,0,0, 0,1,0,0, 0,0,2,-1, 0,0,0,1};
    for (int i = 0; i < 16; i++) {
        p[i] = projection[i]; vp[i] = view_projection[i]; old[i] = previous ? previous[i] : view_projection[i];
        if (!isfinite(p[i]) || !isfinite(vp[i]) || !isfinite(old[i])) return 0;
    }
    if (!inverse(p, pi) || !inverse(vp, vi)) return 0;
    multiply(pi, vp, view);
    if (!inverse(view, world)) return 0;
    multiply(normalize, p, api);
    if (!inverse(api, api_inverse)) return 0;
    double motion[16], normalized[16], result[16];
    if (previous) {
        multiply(old, vi, motion);
        multiply(normalize, motion, normalized);
        multiply(normalized, denormalize, result);
        double check[16];
        if (!inverse(result, check)) return 0;
    } else for (int i = 0; i < 16; i++) result[i] = i / 4 == i % 4;
    RsfCameraMatrices camera;
    if (!sdk_matrix(api, camera.view_to_clip) || !sdk_matrix(api_inverse, camera.clip_to_view) ||
        !sdk_matrix(view, camera.world_to_view) || !sdk_matrix(world, camera.view_to_world) ||
        !sdk_matrix(result, camera.clip_to_previous_clip)) return 0;
    memcpy(out, &camera, sizeof camera);
    return 1;
}

int rsf_camera_projection(const float m[16], RsfCameraProjection *out) {
    if (!m || !out) return 0;
    for (int i = 0; i < 16; i++) if (!isfinite(m[i])) return 0;
    // Perspective divide must depend on view Z alone. Oblique depth planes and
    // orthographic cameras need a different depth interpretation.
    if (m[12] != 0 || m[13] != 0 || m[15] != 0 || m[14] == 0 ||
        m[8] != 0 || m[9] != 0 || m[0] == 0 || m[5] == 0) return 0;
    double a = m[10], b = m[11], c = m[14];
    double denominator[2] = {a + c, a - c};
    double distance[2];
    for (int i = 0; i < 2; i++) {
        distance[i] = denominator[i] == 0 ? INFINITY : b / denominator[i];
        if (!(distance[i] > 0)) return 0;
    }
    int inverted = distance[1] < distance[0];
    double near_plane = distance[inverted], far_plane = distance[!inverted];
    double fov = 2 * atan(fabs(c / m[5]));
    if (!isfinite(near_plane) || !(far_plane > near_plane) || !(fov > 0 && fov < 3.141593)) return 0;
    RsfCameraProjection result = {(float)near_plane, isinf(far_plane) ? 0 : (float)far_plane,
                                  (float)fov, inverted, isinf(far_plane) != 0};
    if (!isfinite(result.near_plane) || !isfinite(result.far_plane) || result.near_plane <= 0) return 0;
    *out = result;
    return 1;
}
