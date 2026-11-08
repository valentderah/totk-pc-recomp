#version 450
// The blueprint preview's shading: flat, two-sided, one grey-clay material with a per-part colour.
//
// The reference viewer lights every part with an ambient term of 0.1 and one directional light of 1 attached to the
// camera; a light attached to the camera has a FIXED direction in view space, so the direction here is a constant and no
// light matrix is ever uploaded. Its materials are DoubleSide, so a face turned away from the light is lit as brightly
// as one turned toward it - which is why the dot product is taken of the absolute value.
//
// The normal comes from the derivatives of the view-space position: cross(dFdx, dFdy) is the face's own normal, which is
// exactly the flat shading a clay material with roughness 1 produces, and needs no normal attribute.
layout(location = 0) in vec3 v_view;
layout(location = 1) in vec3 v_color;

layout(location = 0) out vec4 o_color;

// The key light in view space, normalised: an approximate three-quarter key from above and to the left of the viewer,
// the pose the reference's camera-parented light settles in.
const vec3 LIGHT_VIEW = normalize(vec3(0.40, 0.82, 0.41));

void main() {
    vec3 dx = dFdx(v_view);
    vec3 dy = dFdy(v_view);
    vec3 n = cross(dx, dy);
    float len = length(n);
    // A degenerate triangle (or a pixel on a silhouette) has no usable normal: light it flat rather than produce NaN.
    float lit = len > 1e-12 ? abs(dot(n / len, LIGHT_VIEW)) : 1.0;

    // Ambient 0.35 plus the key light's 0.65: never fully black, so a face turned away is still a face.
    float shade = 0.35 + 0.65 * lit;
    o_color = vec4(v_color * shade, 1.0);
}