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
