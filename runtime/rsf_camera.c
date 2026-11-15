#include <math.h>
#include "rsf_camera.h"

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
