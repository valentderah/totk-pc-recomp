#pragma once

typedef struct RsfCameraProjection {
    float near_plane, far_plane, vertical_fov;
    int inverted, infinite;
} RsfCameraProjection;

// Row-major perspective matrix with OpenGL clip depth, as stored in scene c4[7..10].
// Returns zero for unsupported or invalid matrices, leaving out unchanged.
int rsf_camera_projection(const float matrix[16], RsfCameraProjection *out);
