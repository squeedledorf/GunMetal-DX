// vkclean -- tonemap / colour grade COMMON (#820): everything clean_tonemap.frag does to a pixel
// AFTER exposure -- the colour grade's linear stage, the tonemapper + its mix with the clamped
// input (or no_post's clamp), split toning, the sRGB encode, legacy gamma and the display-space
// grade (user 3D LUT, tone curves) -- as ONE function of the exposed colour, tonemapTail(), plus
// the declarations it reads (the push block, the grade UBO + its LUT / curve samplers).
//
// Two includers, one source: clean_tonemap.frag evaluates it per pixel (direct path), and
// clean_tonemap_lut.comp bakes it into a 3D lookup table over a log shaper of the exposed colour
// whenever its inputs change (LLCleanRendererVK::recordTonemapLut); the frag then replaces the
// whole tail with one tetrahedral lookup (tonemapLutSample). Everything here depends only on the
// colour and per-grade constants (no uv, no per-frame exposure): exactly what a LUT can hold.
#ifndef CLEAN_TONEMAP_COMMON_GLSL
#define CLEAN_TONEMAP_COMMON_GLSL

layout(push_constant) uniform Push {
    vec4 p0;   // exposure (RenderExposure), tonemap_mix, tonemap_type, legacy gamma (0 = off)
    vec4 p1;   // no_post (1 = LL NoPost / vintage mode: clamp only), debug (1 = raw clamp), dyn exposure on,
               // motion-vector debug view (1 = RenderVKDebugMotionVectors)
    vec4 tonemap_params;   // per-tonemapper terms solved CPU-side (upstream Alchemy tonemapUtilF)
    vec4 p2;   // dither (B1 / F4a: when the tonemap is the last full-screen pass, the
               // final pass is skipped and its dither runs here): amount (0 = off),
               // frame id (mod 2^20), animate; w = (#820) the baked tail LUT's edge size N
               // (0 = evaluate the tail per pixel)
#ifdef GMDX_TONEMAP_NIGHT_EYE   // <GMDX> clean_tonemap.frag only (the LUT bake's range stays 64 B)
    vec4 p3;   // night vision: Purkinje strength, cd/m^2 per renderer unit, log2 exposure at 5 / 0.005 cd/m^2
    vec4 p4;   // low-light noise + acuity loss (0 = off), frame id (mod 1024), photo night's cold grade
               // (GMDXNightLook 1; 0 = none), 0
#endif
} pc;

// ---- Colour grade (Lightbox; upstream Alchemy colorCorrectF + colorGradeUtilF) ----
layout(std140, set = 1, binding = 0) uniform GradeBlock {
    vec4 wb_gain, lift, inv_gamma, gain;
    vec4 shadow_ratio, mid_ratio, hi_ratio;
    vec4 split;      // midtone amount, tone amount
    vec4 ramps;      // shadow ramp.xy, highlight ramp.xy
    vec4 bwp_bc;     // bwp scale, bwp bias, bc scale, bc bias
    vec4 tone;       // highlights (scaled), shadows (scaled), saturation, vibrance
    vec4 misc;       // hue shift / 360, LUT strength, curve amount, grade on
    vec4 lut_size;   // N, flip green, swap blue/green
    vec4 curve;      // curve LUT scale, bias
    // Chromatic aberration (upstream postEffectUtilsF applyChromaticAberration)
    vec4 ca0;        // amount (pre-squared x0.02), 1/falloff, anisotropy, 0
    vec4 ca1;        // angle sin, cos, resolution.xy
    vec4 ca2;        // offset R.xy, offset B.xy
    vec4 bloom;      // strength, halation strength, saturation preserve (#998), active
    vec4 halation_tint;   // rgb, hue-preserving highlights on (#998)
    vec4 lens_fx;    // cross-filter strength (x bloom), lens dirt strength, dirt bloom response, dirt flare response
    // Lens flare (upstream postEffectUtilsF computeLensFlare)
    vec4 flare0;     // strength, sun uv.xy, sky hdr scale
    vec4 flare1;     // light colour rgb, glow
    vec4 flare2;     // streak length, falloff, width, intensity
    vec4 flare3;     // streak tint rgb, chromatic spread
    vec4 flare4;     // glow radius, glow falloff, ghost, ghost count
    vec4 flare5;     // ghost spacing, halo, halo radius, halo width
    vec4 flare6;     // starburst, spikes, sharpness, starburst falloff
    vec4 flare7;     // resolution.xy
} grade;

layout(set = 1, binding = 1) uniform sampler3D uColorGradeLut;
layout(set = 1, binding = 2) uniform sampler2D uToneCurveLut;

const vec3 CG_LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 cg_rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 cg_hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float cg_ramp(float l, vec2 ramp)
{
    float t = clamp(l * ramp.x + ramp.y, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
// Linear, pre-tonemap: white balance, then lift / gamma / gain.
vec3 gradeLinear(vec3 col)
{
    col *= grade.wb_gain.rgb;
    if (any(notEqual(grade.lift.rgb, vec3(0.0))) || any(notEqual(grade.gain.rgb, vec3(1.0)))
        || any(notEqual(grade.inv_gamma.rgb, vec3(1.0))))
    {
        col = col * grade.gain.rgb + grade.lift.rgb;
        col = pow(max(col, 0.0), grade.inv_gamma.rgb);
    }
    return col;
}
// Linear, post-tonemap: split toning (luminance preserving).
vec3 gradeSplitTone(vec3 col)
{
    if (grade.split.y <= 0.0 && grade.split.x <= 0.0) return col;
    float l  = dot(col, CG_LUMA);
    float hi = cg_ramp(l, grade.ramps.zw);
    float lo = 1.0 - cg_ramp(l, grade.ramps.xy);
    float md = max(1.0 - hi - lo, 0.0);
    col = mix(col, grade.shadow_ratio.rgb * l, lo * grade.split.y);
    col = mix(col, grade.mid_ratio.rgb    * l, md * grade.split.x);
    col = mix(col, grade.hi_ratio.rgb     * l, hi * grade.split.y);
    return col;
}
// Display space: black/white point, brightness/contrast, highlight/shadow recovery,
// saturation, vibrance, hue shift, 3D LUT, tone curves.
vec3 gradeDisplay(vec3 col)
{
    col = col * grade.bwp_bc.x + grade.bwp_bc.y;
    col = col * grade.bwp_bc.z + grade.bwp_bc.w;
    if (grade.tone.x != 0.0 || grade.tone.y != 0.0)
    {
        float l = dot(col, CG_LUMA);
        float hMask = smoothstep(0.5, 1.0, l);
        float sMask = 1.0 - smoothstep(0.0, 0.5, l);
        col += (vec3(1.0) - col) * (grade.tone.x * hMask);
        col += col * (grade.tone.y * sMask);
    }
    if (grade.tone.z != 1.0) col = mix(vec3(dot(col, CG_LUMA)), col, grade.tone.z);
    if (grade.tone.w != 0.0)
    {
        float luma = dot(col, CG_LUMA);
        float mx = max(col.r, max(col.g, col.b)), mn = min(col.r, min(col.g, col.b));
        col = clamp(mix(vec3(luma), col, 1.0 + grade.tone.w * (1.0 - (mx - mn))), 0.0, 1.0);
    }
    if (grade.misc.x != 0.0)
    {
        vec3 hsv = cg_rgb2hsv(clamp(col, 0.0, 1.0));
        hsv.x = fract(hsv.x + grade.misc.x);
        col = cg_hsv2rgb(hsv);
    }
    if (grade.misc.y > 0.0)
    {
        vec3 original = col;
        vec3 c = clamp(col, 0.0, 1.0);
        c.g = grade.lut_size.y > 0.5 ? 1.0 - c.g : c.g;
        c   = grade.lut_size.z > 0.5 ? c.rbg : c.rgb;
        float invN = 1.0 / grade.lut_size.x;
        c = textureLod(uColorGradeLut, c * (1.0 - invN) + 0.5 * invN, 0.0).rgb;
        col = mix(original, c, grade.misc.y);
    }
    if (grade.misc.z > 0.0)
    {
        vec3 u = col * grade.curve.x + grade.curve.y;
        vec3 curved = vec3(textureLod(uToneCurveLut, vec2(u.r, 0.5), 0.0).r,
                           textureLod(uToneCurveLut, vec2(u.g, 0.5), 0.0).g,
                           textureLod(uToneCurveLut, vec2(u.b, 0.5), 0.0).b);
        col = mix(col, curved, grade.misc.z);
    }
    return col;
}

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT

const mat3 ACESInputMat = mat3(0.59719, 0.07600, 0.02840,
                               0.35458, 0.90834, 0.13383,
                               0.04823, 0.01566, 0.83777);
// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat = mat3( 1.60475, -0.10208, -0.00327,
                                -0.53108,  1.10813, -0.07276,
                                -0.07367, -0.00605,  1.07602);

vec3 RRTAndODTFit(vec3 color)
{
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}

vec3 toneMapACES_Hill(vec3 color)
{
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, 0.0, 1.0);
}

// Khronos PBR Neutral
vec3 PBRNeutralToneMapping(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1. - startCompression;
    float newPeak = 1. - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
    return mix(color, newPeak * vec3(1, 1, 1), g);
}

// ---- upstream Alchemy tonemappers (class1/deferred/tonemapUtilF.glsl) ----
const float output_max_value = 1.0;

// Reinhard extended (tonemap_params.x = white squared)
vec3 tonemap_reinhard(vec3 color)
{
    float white_squared = pc.tonemap_params.x;
    return color * (1.0 + color / white_squared) / (1.0 + color / output_max_value);
}

// Hable filmic (tonemap_params.x = white through the curve)
vec3 tonemap_filmic(vec3 color)
{
    const float exposure_bias = 2.0;
    const float A = 0.22 * exposure_bias * exposure_bias;
    const float B = 0.30 * exposure_bias;
    const float C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    vec3 t = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
    return t / pc.tonemap_params.x;
}

// ACES fitted, boosted (MJP BakingLab; tonemap_params.x = white through the curve)
vec3 tonemap_aces_boosted(vec3 color)
{
    const float exposure_bias = 1.2;
    const float A = 0.0245786, B = 0.000090537, C = 0.983729, D = 0.4329510, E = 0.238081;
    const mat3 rgb_to_rrt = mat3(
            vec3(0.59719 * exposure_bias, 0.35458 * exposure_bias, 0.04823 * exposure_bias),
            vec3(0.07600 * exposure_bias, 0.90834 * exposure_bias, 0.01566 * exposure_bias),
            vec3(0.02840 * exposure_bias, 0.13383 * exposure_bias, 0.83777 * exposure_bias));
    const mat3 odt_to_rgb = mat3(
            vec3( 1.60475, -0.53108, -0.07367),
            vec3(-0.10208,  1.10813, -0.00605),
            vec3(-0.00327, -0.07276,  1.07602));
    color *= rgb_to_rrt;
    vec3 t = (color * (color + A) - B) / (color * (C * color + D) + E);
    t *= odt_to_rgb;
    return t / pc.tonemap_params.x;
}

// allenwp curve (AgX base); tonemap_params = contrast, toe_a, slope, w
vec3 allenwp_curve(vec3 x)
{
    const float awp_crossover_point = 0.18;
    const float awp_shoulder_max = output_max_value - awp_crossover_point;
    float awp_contrast = pc.tonemap_params.x;
    float awp_toe_a = pc.tonemap_params.y;
    float awp_slope = pc.tonemap_params.z;
    float awp_w = pc.tonemap_params.w;
    vec3 s = x - awp_crossover_point;
    vec3 slope_s = awp_slope * s;
    s = slope_s * (1.0 + s / awp_w) / (1.0 + (slope_s / awp_shoulder_max));
    s += awp_crossover_point;
    vec3 t = pow(x, vec3(awp_contrast));
    t = t / (t + awp_toe_a);
    return mix(s, t, lessThan(x, vec3(awp_crossover_point)));
}

// AgX (EaryChow / Blender approximation, via Godot)
vec3 tonemap_agx(vec3 color)
{
    const mat3 rec709_to_rec2020_agx_inset_matrix = mat3(
            0.544814746488245, 0.140416948464053, 0.0888104196149096,
            0.373787398372697, 0.754137554567394, 0.178871756420858,
            0.0813978551390581, 0.105445496968552, 0.732317823964232);
    const mat3 agx_outset_rec2020_to_rec709_matrix = mat3(
            1.96488741169489, -0.299313364904742, -0.164352742528393,
            -0.855988495690215, 1.32639796461980, -0.238183969428088,
            -0.108898916004672, -0.0270845997150571, 1.40253671195648);
    color = rec709_to_rec2020_agx_inset_matrix * color;
    color = allenwp_curve(color);
    color = min(vec3(output_max_value), color);
    color = agx_outset_rec2020_to_rec709_matrix * color;
    return color;
}

// Uchimura (Gran Turismo)
vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b)
{
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;
    vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
    vec3 w2 = vec3(step(m + l0, x));
    vec3 w1 = vec3(1.0 - w0 - w2);
    vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
    vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
    vec3 L = vec3(m + a * (x - m));
    return T * w0 + L * w1 + S * w2;
}
vec3 uchimura(vec3 color) { return uchimura(color, 1.0, 1.0, 0.22, 0.4, 1.33, 0.0); }

vec3 applyToneMapType(vec3 color, int type)
{
    color = max(color, vec3(0.0));
    switch (type)
    {
    case 0: return PBRNeutralToneMapping(color);
    case 1: return toneMapACES_Hill(color);
    case 2: return tonemap_aces_boosted(color);
    case 3: return tonemap_reinhard(color);
    case 4: return tonemap_filmic(color);
    case 5: return uchimura(color);
    case 6: return tonemap_agx(color);
    }
    return color;
}

vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0.0), vec3(1.0));
    vec3 low  = cl * 12.92;
    vec3 high = 1.055 * pow(cl, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThan(cl, vec3(0.0031308)));
}

// ---- THE TAIL (#820): exposed linear colour -> final display colour, before the dither ----
// (Exactly clean_tonemap.frag's main() from the grade's linear stage through the final clamp.)
vec3 tonemapTail(vec3 c)
{
    if (grade.misc.w > 0.5) c = gradeLinear(c);
    if (pc.p1.x < 0.5)
    {
        vec3 clamped = clamp(c, vec3(0.0), vec3(1.0));
        vec3 t = clamp(applyToneMapType(c, int(pc.p0.z + 0.5)), vec3(0.0), vec3(1.0));
        c = mix(clamped, t, pc.p0.y);
    }
    else
    {
        c = clamp(c, vec3(0.0), vec3(1.0));
    }
    if (grade.misc.w > 0.5) c = gradeSplitTone(c);

    c = linear_to_srgb(c);
    if (pc.p0.w > 0.0)   // LEGACY_GAMMA (gamma inverted CPU-side, as LL)
    {
        vec3 k = 1. - clamp(c, vec3(0.), vec3(1.));
        c = 1. - pow(k, vec3(pc.p0.w));
    }
    if (grade.misc.w > 0.5) c = gradeDisplay(c);
    return clamp(c, vec3(0.0), vec3(1.0));
}

// ---- THE LUT'S INPUT SHAPER (#820) ----
// u = log2(1 + K c) / log2(1 + K M) per channel: linear near black (K c << 1: the sRGB toe and
// every tonemapper's toe stay exactly interpolated), logarithmic above (~0.5 stop per texel at N =
// 32, 0.33 at N = 48), u = 1 at M. Above M every tonemapper (and no_post's clamp) has saturated to
// display white, so clamping there is exact. Error of the TETRAHEDRAL lookup against the direct
// tail (offline, numpy, Khronos Neutral / ACES Hill + sRGB, log-uniform colours 2^-14 .. 2^6):
// N = 48: greys <= 1.2 / 255 (mean 0.05), colours with channels within 4x <= 1.2 / 255; N = 32:
// greys <= 3 / 255. Trilinear (one hardware fetch) is NOT used: the grey axis is the diagonal of
// every cell and Neutral's min / max kinks along it cost up to 10 / 255 there.
const float TM_LUT_K = 256.0;
const float TM_LUT_M = 64.0;
float tmLutLogDen() { return log2(1.0 + TM_LUT_K * TM_LUT_M); }
vec3 tmLutShape(vec3 c)   { return log2(1.0 + TM_LUT_K * clamp(c, vec3(0.0), vec3(TM_LUT_M))) / tmLutLogDen(); }
vec3 tmLutUnshape(vec3 u) { return (exp2(u * tmLutLogDen()) - 1.0) / TM_LUT_K; }

#endif   // CLEAN_TONEMAP_COMMON_GLSL
