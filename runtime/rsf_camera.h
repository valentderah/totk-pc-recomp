#pragma once

typedef struct RsfCameraProjection {
    float near_plane, far_plane, vertical_fov;
    int inverted, infinite;
} RsfCameraProjection;

// Row-major perspective matrix with OpenGL clip depth, as stored in scene c4[7..10].
// Returns zero for unsupported or invalid matrices, leaving out unchanged.
int rsf_camera_projection(const float matrix[16], RsfCameraProjection *out);

typedef struct RsfCameraMatrices {
    float view_to_clip[16], clip_to_view[16], view_to_world[16], world_to_view[16], clip_to_previous_clip[16];
} RsfCameraMatrices;
// Inputs multiply column vectors in OpenGL clip space. RSF uses row vectors and
// device depth 0..1. A null previous matrix resets history to identity.
int rsf_camera_matrices(const float projection[16], const float view_projection[16],
    const float previous_view_projection[16], RsfCameraMatrices *out);

#include <math.h>

static inline int rsf_matrix_inverse(const double *matrix, double *out, double threshold, int finite) {
    double rows[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++)
        rows[r][c] = c < 4 ? matrix[r * 4 + c] : c - 4 == r;
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) if (fabs(rows[r][c]) > fabs(rows[pivot][c])) pivot = r;
        if ((finite && !isfinite(rows[pivot][c])) || fabs(rows[pivot][c]) < threshold) return 0;
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
