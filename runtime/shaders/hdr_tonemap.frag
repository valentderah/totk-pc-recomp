#version 450
// The game's tonemap pass for HDR output (runtime/vk_hdr.c vk_hdr_shader_swap puts it in place of the translated
// shader when TOTK_HDR=1). Same inputs as the game's: fp_t_tcb_8 (bloom, clamped to 10000 as the game does) plus
// fp_t_tcb_A (the scene), weights in c1[0] and the saturation grade in c3[0]. The game maps luminance and each channel
// with 1 - e^-x, blends the two by the luminance curve squared, clamps to 0..1 and grades saturation. Here the curve is
// the game's up to PIVOT and a straight line with its slope above, so mid-greys look as in SDR and highlights keep
// going; the grade works on the colour divided by its largest channel above 1; a roll-off on the largest channel stops
// at the display's peak. The output is relative to the HUD's white (1.0), which the present maps to TOTK_HDR_UI_NITS.
// Hue correction (hdr.z, TOTK_HDR_HUE): the game's SDR clamp turns bright saturated colours towards white (a torch flame
// clips red, then green, and reads yellow-white, the colour its emitted light was tuned to); the extended curve keeps
// their ratios, so they stay saturated. From the picture's white upwards the colour moves towards the game's own SDR
// result at the HDR luminance, by up to hdr.z (RenoDX's TotK mod has the same control).
// Approach after RenoDX's TotK mod by souperman9 and Musa (github.com/clshortfuse/renodx, src/games/totk, MIT): its
// "extended vanilla" curve.
// Bindings are those of the game's translated shader: vk_hdr.c rewrites them from the original's names.
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0, std140) uniform SupportBuffer {
    vec4 pad[90];
    vec4 hdr;   // byte 1440 (vk_exec_draw.c support_fill): game white / HUD white, peak / game white, hue correction
} support_buffer;
layout(set = 0, binding = 1) uniform sampler2D fp_t_tcb_8;
layout(set = 0, binding = 2) uniform sampler2D fp_t_tcb_A;
layout(set = 0, binding = 3, std140) uniform C1 { vec4 v[4096]; } fp_c1;
layout(set = 0, binding = 4, std140) uniform C3 { vec4 v[4096]; } fp_c3;

const float PIVOT = 0.18;

float curve(float x) {   // the game's 1 - e^-x below PIVOT, its tangent there above
    float y0 = 1.0 - exp(-PIVOT);
    return x <= PIVOT ? 1.0 - exp(-x) : y0 + exp(-PIVOT) * (x - PIVOT);
}

vec3 curve3(vec3 x) { return vec3(curve(x.r), curve(x.g), curve(x.b)); }

float maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }

void main() {
    vec3 x = min(texture(fp_t_tcb_8, uv.xy).rgb, vec3(10000.0)) + texture(fp_t_tcb_A, uv.xy).rgb;
    vec4 w = fp_c1.v[0], g = fp_c3.v[0];
    float game = support_buffer.hdr.x > 0.0 ? support_buffer.hdr.x : 1.0;
    float peak = support_buffer.hdr.y > 1.0 ? support_buffer.hdr.y : 1.0;

    float lum = max(x.r * 0.2989 + x.g * w.x + x.b * w.y, 0.0);
    float sdr = 1.0 - exp(-lum);
    vec3 by_lum = lum > 0.0 ? x * (curve(lum) / lum) : x;
    vec3 c = max(mix(by_lum, curve3(x), sdr * sdr), vec3(0.0));

    // the game's saturation grade, on the colour scaled into 0..1 by its largest channel
    float s = max(maxc(c), 1.0);
    vec3 n = c / s;
    float m = maxc(n);
    float d = n.r * 0.6666667 + (n.g + n.b) * w.z - 1.0;
    float grade = g.y * (1.0 - d * d) + g.x;
    c = s * ((n - m) * grade + m);

    // the game's own SDR result: the same blend clamped to 0..1, graded
    vec3 sdr_c = clamp(mix(lum > 0.0 ? x * (sdr / lum) : x, vec3(1.0) - exp(-x), sdr * sdr), 0.0, 1.0);
    float sm = maxc(sdr_c), sd = sdr_c.r * 0.6666667 + (sdr_c.g + sdr_c.b) * w.z - 1.0;
    sdr_c = (sdr_c - sm) * (g.y * (1.0 - sd * sd) + g.x) + sm;
    float hue = clamp(support_buffer.hdr.z, 0.0, 1.0);
    const vec3 Y = vec3(0.2126, 0.7152, 0.0722);   // Rec. 709 luminance
    float yc = dot(c, Y), ys = dot(sdr_c, Y);
    if (hue > 0.0 && ys > 1e-5) c = mix(c, max(sdr_c, vec3(0.0)) * (yc / ys), hue * smoothstep(0.5, 1.5, maxc(c)));

    // roll-off of the largest channel from half the peak to the peak
    float top = maxc(c), knee = 0.5 * peak;
    if (top > knee) {
        float r = peak - knee;
        c *= (knee + r * (1.0 - exp(-(top - knee) / r))) / top;
    }
    color = vec4(max(c, vec3(0.0)) * game, 0.0);
}
