#version 450
// One triangle over the whole target, for hdr_ui.frag (runtime/vk_hdr.c vk_hdr_ui_compose).
layout(location = 0) out vec2 uv;

void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
