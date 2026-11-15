#include <assert.h>
#include <math.h>
#include <string.h>
#include "rsf_camera.h"

static int close_to(float a, float b) { return fabsf(a - b) < 0.002f * fmaxf(1, fabsf(b)); }

static void transform(const float point[4], const float matrix[16], float result[4]) {
    for (int c = 0; c < 4; c++) {
        result[c] = 0;
        for (int r = 0; r < 4; r++) result[c] += point[r] * matrix[r * 4 + c];
    }
}

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
    memset(p, 0, sizeof p);
    p[0] = 1; p[5] = 2; p[10] = -101.0f / 99; p[11] = -200.0f / 99; p[14] = -1;
    float vp[16], old[16];
    memcpy(vp, p, sizeof p); memcpy(old, p, sizeof p);
    for (int r = 0; r < 4; r++) {
        vp[r * 4 + 3] -= 3 * p[r * 4] + 4 * p[r * 4 + 1] + 5 * p[r * 4 + 2];
        old[r * 4 + 3] -= 2 * p[r * 4] + 4 * p[r * 4 + 1] + 5 * p[r * 4 + 2];
    }
    RsfCameraMatrices matrices;
    assert(rsf_camera_matrices(p, vp, old, &matrices));
    assert(close_to(matrices.view_to_world[12], 3) && close_to(matrices.view_to_world[13], 4) &&
           close_to(matrices.view_to_world[14], 5));
    float world[4] = {3, 4, 4, 1}, view[4], clip[4], previous[4];
    transform(world, matrices.world_to_view, view);
    transform(view, matrices.view_to_clip, clip);
    assert(close_to(clip[0], 0) && close_to(clip[1], 0) && close_to(clip[2] / clip[3], 0));
    transform(clip, matrices.clip_to_previous_clip, previous);
    assert(close_to(previous[0] / previous[3], 1) && close_to(previous[2] / previous[3], 0));
    world[2] = -95;
    transform(world, matrices.world_to_view, view);
    transform(view, matrices.view_to_clip, clip);
    assert(close_to(clip[2] / clip[3], 1));
    transform(clip, matrices.clip_to_view, view);
    assert(close_to(view[2] / view[3], -100));
    assert(rsf_camera_matrices(p, vp, NULL, &matrices));
    for (int i = 0; i < 16; i++) assert(close_to(matrices.clip_to_previous_clip[i], i / 4 == i % 4));
    RsfCameraMatrices retained = matrices;
    memset(old, 0, sizeof old);
    assert(!rsf_camera_matrices(p, vp, old, &matrices));
    assert(!memcmp(&retained, &matrices, sizeof matrices));
    // A rotated current camera and a changed projection must still reproject
    // to the previous camera, rather than mixing transposed transforms.
    memcpy(old, p, sizeof old);
    p[0] = 2; p[5] = 3;
    for (int r = 0; r < 4; r++) {
        vp[r * 4] = p[r * 4 + 2];
        vp[r * 4 + 1] = p[r * 4 + 1];
        vp[r * 4 + 2] = -p[r * 4];
        vp[r * 4 + 3] = p[r * 4 + 3];
    }
    assert(rsf_camera_matrices(p, vp, old, &matrices));
    world[0] = -2; world[1] = 0; world[2] = -3;
    transform(world, matrices.world_to_view, view);
    assert(close_to(view[0], 3) && close_to(view[2], -2));
    transform(view, matrices.view_to_clip, clip);
    transform(clip, matrices.clip_to_previous_clip, previous);
    assert(close_to(previous[0], -2) && close_to(previous[3], 3));
    assert(close_to(previous[2], (-3 * p[10] + p[11] + 3) / 2));
    return 0;
}
