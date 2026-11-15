#version 450
// The blueprint preview's shading: flat, two-sided, one grey-clay material with a per-part colour.
//
// The parts carry no normals, so the face normal comes from the screen-space derivatives of the view position: that is the
// exact flat-shaded normal of a roughness-1 clay surface and needs no normal attribute. What matters for the preview is that
// the form READS: a face turned from the light has to be clearly darker than one facing it, or a spray of parts looks like a
// flat cut-out and is hard to look at.
//
// The camera is fixed by design (a drag turns the BUILD, not the camera), so a light fixed in the build's own space is a
// constant here: world +Y seen by this camera is (0, cos pitch, sin pitch), and the default pitch is 0.488692 rad.
layout(location = 0) in vec3 v_view;
layout(location = 1) in vec3 v_color;

layout(location = 0) out vec4 o_color;

// The key light: straight down the build's up axis, so the top of every part is bright and the underside is dark and the
// turn of each face is visible.
const vec3 KEY_DIR = normalize(vec3(0.0, 0.883, 0.469));
// A fill light from the lower right of the viewer: keeps a shadowed face readable without washing out the key's contrast.
const vec3 FILL_DIR = normalize(vec3(0.62, -0.28, 0.73));

void main() {
    vec3 dx = dFdx(v_view);
    vec3 dy = dFdy(v_view);
    vec3 n = cross(dx, dy);
    float len = length(n);
    vec3 nn = len > 1e-12 ? n / len : vec3(0.0, 0.0, 1.0);
    // Two-sided: a face seen from behind gets the normal flipped towards the viewer, so it is shaded like the front it is
    // the back of, instead of reading as an unlit hole. (Missing this is what made the old shading look "off".)
    if (nn.z < 0.0) nn = -nn;

    const float key = max(dot(nn, KEY_DIR), 0.0);
    const float fill = max(dot(nn, FILL_DIR), 0.0);
    // Ambient 0.16, key 0.74, fill 0.10: dark enough on the far side to show the form, never black.
    const float shade = 0.16 + 0.74 * key + 0.10 * fill;
    o_color = vec4(v_color * shade, 1.0);
}