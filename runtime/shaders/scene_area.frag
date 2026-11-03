#version 450

layout(location = 0) in vec4 texcoord;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 1) uniform sampler2D scene;

float overlap(float lo, float hi, float texel) {
    return max(min(hi, texel + 1.0) - max(lo, texel), 0.0);
}

void main() {
    ivec2 size = textureSize(scene, 0);
    vec2 center = texcoord.xy * vec2(size);
    vec2 extent = max(abs(dFdx(center)) + abs(dFdy(center)), vec2(1.0)) * 0.5;
    vec2 lo = center - extent, hi = center + extent;
    ivec2 first = clamp(ivec2(floor(lo)), ivec2(0), size - 1);
    ivec2 last = clamp(ivec2(ceil(hi)) - 1, ivec2(0), size - 1);
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = first.y; y <= last.y; y++) {
        float wy = overlap(lo.y, hi.y, float(y));
        for (int x = first.x; x <= last.x; x++) {
            float w = wy * overlap(lo.x, hi.x, float(x));
            sum += texelFetch(scene, ivec2(x, y), 0) * w;
            total += w;
        }
    }
    color = sum / max(total, 1e-6);
}
