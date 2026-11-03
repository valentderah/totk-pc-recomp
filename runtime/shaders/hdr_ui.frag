#version 450
// The in-game menu on an HDR10 swapchain (runtime/vk_hdr.c vk_hdr_ui_compose): ImGui drew it into an RGBA8 image,
// premultiplied sRGB colour over a clear 0; this turns each pixel back into its colour, puts its white at p.x nits and
// writes the PQ code with its coverage, which the pipeline blends over the picture (in PQ code values, as the menu
// blends in sRGB code values on an SDR swapchain). PQ: SMPTE ST 2084; Rec. 709 to Rec. 2020: ITU-R BT.2087.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D ui;
layout(push_constant) uniform P { vec4 p; } pc;

vec3 srgb_decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 pq(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main() {
    vec4 u = texture(ui, uv);
    if (u.a <= 0.0) discard;
    vec3 c = srgb_decode(clamp(u.rgb / u.a, 0.0, 1.0));
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
    color = vec4(pq(max(to2020 * c, vec3(0.0)) * pc.p.x), u.a);
}
