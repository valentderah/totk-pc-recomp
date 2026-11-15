#include <assert.h>
#include <math.h>
#include <string.h>
#include "rsf_camera.h"

static int close_to(float a, float b) { return fabsf(a - b) < 0.002f * fmaxf(1, fabsf(b)); }

int main(void) {
    float p[16] = {0};
    p[0] = 1; p[5] = 2; p[10] = -101.0f / 99; p[11] = -200.0f / 99; p[14] = -1;
    RsfCameraProjection camera;
    assert(rsf_camera_projection(p, &camera));
    assert(close_to(camera.near_plane, 1) && close_to(camera.far_plane, 100));
    assert(close_to(camera.vertical_fov, 2 * atanf(0.5f)));
    assert(!camera.inverted && !camera.infinite);
    p[10] = -p[10]; p[11] = -p[11];
    assert(rsf_camera_projection(p, &camera));
    assert(camera.inverted && close_to(camera.near_plane, 1) && close_to(camera.far_plane, 100));
    p[10] = -1; p[11] = -2;
    assert(rsf_camera_projection(p, &camera));
    assert(camera.infinite && camera.far_plane == 0 && close_to(camera.near_plane, 1));
    p[10] = 1; p[11] = 2;
    assert(rsf_camera_projection(p, &camera));
    assert(camera.infinite && camera.inverted && close_to(camera.near_plane, 1));
    RsfCameraProjection saved = camera;
    p[15] = 1;
    assert(!rsf_camera_projection(p, &camera));
    assert(!memcmp(&saved, &camera, sizeof camera));
    p[15] = 0; p[5] = NAN;
    assert(!rsf_camera_projection(p, &camera));
    return 0;
}
