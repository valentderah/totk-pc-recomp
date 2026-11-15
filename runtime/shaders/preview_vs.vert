#version 450
// The blueprint preview's mesh pass: one part's triangles, transformed by the part's matrix and the camera's projection.
//
// The parts carry no normals (the pack stores positions and indices only), and a flat-lit look is what the reference
// viewer shows anyway, so the face normal is taken in the fragment stage from the screen-space derivatives of the view
// position. That keeps the vertex format down to three floats and the upload small.
//
// Push constant: mat4 mvp (64 bytes), vec4 colour (16 bytes) - 80 bytes, well inside the 128-byte minimum.
layout(location = 0) in vec3 a_pos;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
} pc;

layout(location = 0) out vec3 v_view;
layout(location = 1) out vec3 v_color;

void main() {
    // The caller hands over a matrix that already contains the part placement, the camera and the projection, so the
    // vertex stage has nothing left to do but transform. The view-space position is passed on for the flat normal.
    vec4 view = pc.mvp * vec4(a_pos, 1.0);
    v_view = view.xyz / view.w;
    v_color = pc.color.rgb;
    gl_Position = view;
}