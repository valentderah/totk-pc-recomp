#version 450
// Portions adapted from RenoDX. Copyright (C) 2025-2026 Carlos Lopez Jr. and RenoDX contributors.
// SPDX-License-Identifier: MIT
// The game's tonemap pass for HDR output (runtime/vk_hdr.c vk_hdr_shader_swap puts it in place of the translated
// shader when TOTK_HDR=1). Same inputs as the game's: fp_t_tcb_8 (bloom, clamped to 10000 as the game does) plus
// fp_t_tcb_A (the scene), weights in c1[0] and the saturation grade in c3[0].
//
// The HDR reconstruction follows RenoDX's TotK defaults more closely than the original recomp implementation:
// - the game's 1 - e^-x tone curve is extended along its tangent above 0.18;
// - the vanilla SDR tonemap is kept as a donor for hue and purity;
// - MacLeod-Boynton hue/purity transfer is performed through XYZ/LMS in BT.2020. hdr.z (TOTK_HDR_HUE, 0..1) scales
//   both: at its default 0.7 the hue moves 70% and the purity 65% of the way to the SDR donor, RenoDX's defaults
//   ("Hue Shift" and "SDR Blowout"); at 0 bright colours keep the extended curve's saturation. This replaces the
//   earlier blend towards the SDR result scaled to the HDR luminance, which the same setting drove;
// - the game's saturation grade is applied after a temporary neutral max-channel compression, then expanded back;
// - the result is constrained to non-negative BT.709 and display-mapped with per-channel Neutwo in linear BT.2020;
// - RenoDX's default 2.2 gamma correction is folded into the native linear path: the Neutwo peak is corrected before
//   display mapping and the corresponding sRGB-to-gamma-2.2 appearance correction is applied after tone mapping.
// The output remains linear BT.709 relative to the HUD's white (1.0), so the game's later HUD passes and the existing
// HDR10 present encoder continue to work unchanged.
//
// Color reconstruction, gamma correction and Neutwo math adapted from RenoDX (MIT), clshortfuse/renodx commit
// 43c067b755a6a741f6500b2b936570f1462dcfbe, notably src/games/totk/tonemap.slang,
// src/games/totk/macleod_boynton.hlsli, src/shaders/colorcorrect.hlsl and src/shaders/tonemap/neutwo.hlsl.
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
const float PURITY_PER_HUE = 0.65 / 0.70;   // RenoDX's default SDR blowout against its default hue shift
const float GAMMA = 2.2;
const float MB_EPSILON = 1e-20;
const float MB_INTERVAL_MAX = 1e30;
const float MB_NEAR_WHITE_EPSILON = 1e-14;
const float MB_T_MIN = 1e-6;

// Column-major GLSL matrices. Values match the RenoDX TotK MacLeod-Boynton implementation.
const mat3 XYZ_TO_LMS_2006 = mat3(
    vec3(0.185082982238733, -0.134433056469973, 0.000789456671966863),
    vec3(0.584081279463687, 0.405752392775348, -0.000912281325916184),
    vec3(-0.0240722415044404, 0.0358252602217631, 0.0198490812339463));
const mat3 LMS_TO_XYZ_2006 = mat3(
    vec3(2.628474773947687, 0.8765342837340055, -0.06425592569153735),
    vec3(-3.761263499279893, 1.2003038085515867, 0.20476359921126536),
    vec3(9.9763571339745, -1.1033785928646218, 49.93266413348867));
const mat3 BT709_TO_XYZ = mat3(
    vec3(0.4123907993, 0.2126390059, 0.0193308187),
    vec3(0.3575843394, 0.7151686788, 0.1191947798),
    vec3(0.1804807884, 0.0721923154, 0.9505321522));
const mat3 XYZ_TO_BT709 = mat3(
    vec3(3.2409699419, -0.9692436363, 0.0556300797),
    vec3(-1.5373831776, 1.8759675015, -0.2039769589),
    vec3(-0.4986107603, 0.0415550574, 1.0569715142));
const mat3 BT2020_TO_XYZ = mat3(
    vec3(0.6369580483, 0.2627002120, 0.0),
    vec3(0.1446169036, 0.6779980715, 0.0280726930),
    vec3(0.1688809752, 0.0593017165, 1.0609850577));
const mat3 XYZ_TO_BT2020 = mat3(
    vec3(1.7166511880, -0.6666843518, 0.0176398574),
    vec3(-0.3556707838, 1.6164812366, -0.0427706133),
    vec3(-0.2533662814, 0.0157685458, 0.9421031212));
// The chains the per-pixel code uses, folded at compile time: one matrix per conversion instead of two.
const mat3 BT709_TO_BT2020 = XYZ_TO_BT2020 * BT709_TO_XYZ;
const mat3 BT2020_TO_BT709 = XYZ_TO_BT709 * BT2020_TO_XYZ;
const mat3 BT2020_TO_LMS = XYZ_TO_LMS_2006 * BT2020_TO_XYZ;
const mat3 LMS_TO_BT2020 = XYZ_TO_BT2020 * LMS_TO_XYZ_2006;
// D65 white (x 0.31272, y 0.32903) in LMS, and its MacLeod-Boynton coordinates (l, s) = (L, S) / (L + M).
const vec3 WHITE_LMS = XYZ_TO_LMS_2006 * vec3(0.31272 / 0.32903, 1.0, (1.0 - 0.31272 - 0.32903) / 0.32903);
const vec2 MB_WHITE = vec2(WHITE_LMS.x, WHITE_LMS.z) / (WHITE_LMS.x + WHITE_LMS.y);

float maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }
float minc(vec3 c) { return min(c.r, min(c.g, c.b)); }
float div_safe(float a, float b, float fallback) { return b == 0.0 ? fallback : a / b; }

vec3 to_bt2020(vec3 c) { return BT709_TO_BT2020 * c; }
vec3 to_bt709(vec3 c) { return BT2020_TO_BT709 * c; }

// RenoDX GammaSafe(c, true, 2.2): sRGBDecode(GammaEncode(c, 2.2)). Used on the peak ratio before Neutwo.
float srgb_decode(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float gamma22_to_srgb_linear(float c) {
    float a = abs(c);
    float v = srgb_decode(pow(a, 1.0 / GAMMA));
    return c < 0.0 ? -v : v;
}

// RenoDX GammaSafe(c, false, 2.2): GammaDecode(sRGBEncode(c), 2.2). In RenoDX this is paired with a gamma-2.2
// intermediate encode/decode. The native recomp stays linear, so applying this once here gives the same linear result.
float srgb_encode(float c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float srgb_linear_to_gamma22_linear(float c) {
    float a = abs(c);
    float v = pow(srgb_encode(a), GAMMA);
    return c < 0.0 ? -v : v;
}
vec3 srgb_linear_to_gamma22_linear(vec3 c) {
    return vec3(srgb_linear_to_gamma22_linear(c.r),
                srgb_linear_to_gamma22_linear(c.g),
                srgb_linear_to_gamma22_linear(c.b));
}

float vanilla_curve(float x) { return 1.0 - exp(-x); }
float extended_curve(float x) {
    float y0 = vanilla_curve(PIVOT);
    return x <= PIVOT ? vanilla_curve(x) : y0 + exp(-PIVOT) * (x - PIVOT);
}
vec3 extended_curve3(vec3 x) {
    return vec3(extended_curve(x.r), extended_curve(x.g), extended_curve(x.b));
}

// RenoDX's generic TotK extended-vanilla path uses BT.601 luma for the SDR donor and HDR extension.
float bt601_luma(vec3 c) { return dot(c, vec3(0.2989030703, 0.5866198547, 0.1144770751)); }

vec3 vanilla_tonemap(vec3 x) {
    float lum_in = bt601_luma(x);
    float lum_out = vanilla_curve(lum_in);
    vec3 by_lum = x * div_safe(lum_out, lum_in, 0.0);
    vec3 by_channel = vec3(vanilla_curve(x.r), vanilla_curve(x.g), vanilla_curve(x.b));
    float blend = clamp(lum_out, 0.0, 1.0);
    return mix(by_lum, by_channel, blend * blend);
}

vec3 extended_vanilla_tonemap(vec3 x) {
    float lum_in = bt601_luma(x);
    float lum_out = extended_curve(lum_in);
    vec3 by_lum = x * div_safe(lum_out, lum_in, 0.0);
    vec3 by_channel = extended_curve3(x);
    float blend = clamp(vanilla_curve(lum_in), 0.0, 1.0);
    return mix(by_lum, by_channel, blend * blend);
}

vec2 mb_from_lms(vec3 lms) {
    float t = lms.x + lms.y;
    return t > 0.0 ? vec2(lms.x / t, lms.z / t) : vec2(0.0);
}

vec3 lms_from_mb_t(vec2 mb, float t) {
    return vec3(mb.x * t, (1.0 - mb.x) * t, mb.y * t);
}

void interval_lower_zero(float a, float b, out float lo, out float hi) {
    if (abs(a) < MB_EPSILON) {
        if (b >= 0.0) {
            lo = -MB_INTERVAL_MAX;
            hi = MB_INTERVAL_MAX;
        } else {
            lo = 1.0;
            hi = 0.0;
        }
        return;
    }
    float t0 = -b / a;
    if (a > 0.0) {
        lo = t0;
        hi = MB_INTERVAL_MAX;
    } else {
        lo = -MB_INTERVAL_MAX;
        hi = t0;
    }
}

struct MBResult {
    vec3 rgb;
    float purity;
};

// Normalized MacLeod-Boynton purity solve in linear BT.2020. Purity 0 is white on the hue ray and purity 1 is the
// first RGB gamut boundary; T = L + M is held constant so the operation changes chromaticity rather than intensity.
MBResult mb_apply_bt2020(vec3 rgb, float purity_value) {
    MBResult outv;
    outv.rgb = rgb;
    outv.purity = 0.0;

    vec3 lms = BT2020_TO_LMS * rgb;
    float t = lms.x + lms.y;
    if (t <= MB_T_MIN) return outv;

    vec2 white = MB_WHITE;
    vec2 direction = mb_from_lms(lms) - white;
    if (dot(direction, direction) < MB_NEAR_WHITE_EPSILON) return outv;

    vec3 rgb_white = LMS_TO_BT2020 * lms_from_mb_t(white, t);
    vec3 a = rgb - rgb_white;
    float t_lo = 0.0, t_hi = MB_INTERVAL_MAX, lo, hi;

    interval_lower_zero(a.r, rgb_white.r, lo, hi); t_lo = max(t_lo, lo); t_hi = min(t_hi, hi);
    interval_lower_zero(a.g, rgb_white.g, lo, hi); t_lo = max(t_lo, lo); t_hi = min(t_hi, hi);
    interval_lower_zero(a.b, rgb_white.b, lo, hi); t_lo = max(t_lo, lo); t_hi = min(t_hi, hi);
    if (t_hi < t_lo) {
        outv.rgb = max(rgb, vec3(0.0));
        return outv;
    }

    float t_max = max(0.0, t_hi);
    outv.purity = t_max > MB_EPSILON ? clamp(1.0 / t_max, 0.0, 1.0) : 0.0;
    float t_final = clamp(purity_value, 0.0, 1.0) * t_max;
    vec3 lms_final = lms_from_mb_t(white + t_final * direction, t);
    outv.rgb = max(LMS_TO_BT2020 * lms_final, vec3(0.0));
    return outv;
}

vec3 correct_purity(vec3 target709, vec3 reference709, float strength) {
    vec3 target2020 = to_bt2020(target709);
    vec3 reference2020 = to_bt2020(reference709);
    float target_purity = mb_apply_bt2020(target2020, 1.0).purity;
    float reference_purity = mb_apply_bt2020(reference2020, 1.0).purity;
    float applied = mix(target_purity, reference_purity, clamp(strength, 0.0, 1.0));
    return to_bt709(mb_apply_bt2020(target2020, applied).rgb);
}

// Transfer SDR donor hue and purity to the extended HDR result using the same MB construction as RenoDX TotK.
vec3 correct_hue_and_purity(vec3 target709, vec3 reference709, float hue_strength, float purity_strength) {
    if (hue_strength <= 0.0 && purity_strength <= 0.0) return target709;
    if (hue_strength <= 0.0) return correct_purity(target709, reference709, purity_strength);

    vec3 target2020 = to_bt2020(target709);
    vec3 reference2020 = to_bt2020(reference709);
    vec3 target_lms = BT2020_TO_LMS * target2020;
    float target_t = target_lms.x + target_lms.y;
    if (target_t <= MB_T_MIN) return target709;

    float target_purity = mb_apply_bt2020(target2020, 1.0).purity;
    float reference_purity = mb_apply_bt2020(reference2020, 1.0).purity;
    float applied_purity = mix(target_purity, reference_purity, clamp(purity_strength, 0.0, 1.0));

    vec3 reference_lms = BT2020_TO_LMS * reference2020;
    vec2 white = MB_WHITE;
    vec2 target_direction = mb_from_lms(target_lms) - white;
    vec2 reference_direction = mb_from_lms(reference_lms) - white;
    float target_len_sq = dot(target_direction, target_direction);
    float reference_len_sq = dot(reference_direction, reference_direction);

    if (target_len_sq < MB_NEAR_WHITE_EPSILON && reference_len_sq < MB_NEAR_WHITE_EPSILON)
        return correct_purity(target709, reference709, purity_strength);

    vec2 target_unit = target_len_sq > MB_NEAR_WHITE_EPSILON
        ? target_direction * inversesqrt(target_len_sq) : vec2(0.0);
    vec2 reference_unit = reference_len_sq > MB_NEAR_WHITE_EPSILON
        ? reference_direction * inversesqrt(reference_len_sq) : target_unit;
    if (target_len_sq <= MB_NEAR_WHITE_EPSILON) target_unit = reference_unit;

    float hue_blend = clamp(hue_strength, 0.0, 1.0);
    vec2 blended_unit = mix(target_unit, reference_unit, hue_blend);
    float blended_len_sq = dot(blended_unit, blended_unit);
    if (blended_len_sq <= MB_NEAR_WHITE_EPSILON) {
        blended_unit = hue_blend >= 0.5 ? reference_unit : target_unit;
        blended_len_sq = dot(blended_unit, blended_unit);
    }
    blended_unit *= inversesqrt(max(blended_len_sq, MB_EPSILON));

    float seed_len = sqrt(max(target_len_sq, 0.0));
    if (seed_len <= 1e-6) seed_len = sqrt(max(reference_len_sq, 0.0));
    seed_len = max(seed_len, 1e-6);

    vec3 seed_lms = lms_from_mb_t(white + blended_unit * seed_len, target_t);
    vec3 seed2020 = LMS_TO_BT2020 * seed_lms;
    return to_bt709(mb_apply_bt2020(seed2020, applied_purity).rgb);
}

float hue_transfer_gate(vec3 donor) {
    float hi = maxc(donor), lo = minc(donor);
    float chroma = div_safe(hi - lo, hi, 0.0);
    return smoothstep(0.02, 0.12, chroma);
}

float neutwo(float x) { return x * inversesqrt(x * x + 1.0); }
float neutwo(float x, float peak) { return peak * x * inversesqrt(x * x + peak * peak); }

float max_channel_neutwo_scale(vec3 c) {
    float top = maxc(abs(c));
    return top > 0.0 ? neutwo(top) / top : 1.0;
}

vec3 saturation_grade(vec3 c, vec4 w, vec4 g) {
    float d = c.r * 0.6666667 + (c.g + c.b) * w.z - 1.0;
    float grade = g.y * (1.0 - d * d) + g.x;
    float m = maxc(c);
    return vec3(m) + (c - vec3(m)) * grade;
}

vec3 per_channel_neutwo_bt2020(vec3 c709, float peak) {
    vec3 c = max(to_bt2020(c709), vec3(0.0));
    c = vec3(neutwo(c.r, peak), neutwo(c.g, peak), neutwo(c.b, peak));
    return to_bt709(c);
}

void main() {
    vec3 x = min(texture(fp_t_tcb_8, uv.xy).rgb, vec3(10000.0)) + texture(fp_t_tcb_A, uv.xy).rgb;
    vec4 w = fp_c1.v[0], g = fp_c3.v[0];
    float game = support_buffer.hdr.x > 0.0 ? support_buffer.hdr.x : 1.0;
    float peak = support_buffer.hdr.y > 1.0 ? support_buffer.hdr.y : 1.0;

    // RenoDX's default Gamma Correction = 2.2 adjusts the peak in the opposite direction of its post-tonemap
    // appearance correction. At 1000/203 this raises the Neutwo peak from about 4.926 to about 5.33.
    peak = gamma22_to_srgb_linear(peak);

    // Extended HDR carrier plus the original bounded SDR rendering used as RenoDX's color donor.
    vec3 c = extended_vanilla_tonemap(x);
    vec3 donor = clamp(vanilla_tonemap(x), vec3(0.0), vec3(1.0));
    float hue = clamp(support_buffer.hdr.z, 0.0, 1.0);
    c = correct_hue_and_purity(c, donor, hue * hue_transfer_gate(donor), hue * PURITY_PER_HUE);

    // RenoDX temporarily compresses the largest channel with neutral Neutwo while the original TotK saturation grade
    // runs, then divides the compression back out. This avoids feeding the SDR-authored grade arbitrarily large HDR RGB.
    float grade_scale = max_channel_neutwo_scale(c);
    c = saturation_grade(c * grade_scale, w, g) / grade_scale;

    // RenoDX's default BT.709 clamp is a non-negative clamp. Final display mapping is per-channel Neutwo in BT.2020,
    // which allows bright colors to converge toward white instead of preserving max-channel RGB ratios.
    c = max(c, vec3(0.0));
    c = per_channel_neutwo_bt2020(c, peak);

    // Fold RenoDX's default 2.2 intermediate gamma correction into this native linear path. RenoDX first maps the
    // linear result from an sRGB display response to a pure 2.2 response, then encodes/decodes the intermediate as 2.2;
    // those latter two operations cancel, leaving this corrected linear value for HDR output.
    c = srgb_linear_to_gamma22_linear(c);

    color = vec4(max(c, vec3(0.0)) * game, 0.0);
}
