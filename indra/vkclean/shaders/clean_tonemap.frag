#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — tonemap / display resolve (LL postDeferredTonemap + tonemapUtilF +
// exposureMap). Reads the linear HDR scene target and writes display (sRGB-encoded)
// colour to the swapchain. Glow, selection, HUDs and UI are composited after this,
// in display space, as in LL.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1, r32f) uniform readonly image2D uExposure;   // 1x1 dynamic exposure

#include "clean_tonemap_common.glsl"   // #820: the push block, the grade, tonemapTail, the LUT shaper

// ---- Output dither (clean_final.frag's, verbatim: IGN TPDF, 8-bit) ----
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 frameNoiseOffset(bool animate)
{
    uint  frame = animate ? uint(pc.p2.y) : 0u;
    float ff    = float(frame);
    return vec2(hash12(vec2(ff, 13.7)),
                hash12(vec2(ff, 91.3))) * 1024.0;
}
float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
vec3 applyDither(vec3 color, vec2 fragCoord)
{
    vec2 p = fragCoord + frameNoiseOffset(pc.p2.z > 0.5);
    float tpdf = ign(p) + ign(p + vec2(1.618, 2.414)) - 1.0;
    return color + tpdf * (pc.p2.x / 255.0);
}

layout(set = 1, binding = 3) uniform sampler2D uBloom;   // HDR bloom pyramid, mip 0
layout(set = 1, binding = 4) uniform sampler2D uCrossFilter;        // cross-filter streak accumulator
layout(set = 1, binding = 5) uniform sampler2D uLensDirtMap;        // generated lens dirt plate (R)
layout(set = 1, binding = 6) uniform sampler2D uLensFlareStateMap;  // 2x1 sun state
layout(set = 1, binding = 7) uniform sampler2D uMotion;   // RG16F motion vectors (temporal #84), UV units
// (#820) THE BAKED TAIL: tonemapTail over tmLutShape's lattice (RGBA16F, edge pc.p2.w; GENERAL
// layout, written by clean_tonemap_lut.comp). A 1 x 1 x 1 placeholder while the LUT is off.
layout(set = 1, binding = 8) uniform sampler3D uTonemapLut;

#define uLensFlareStrength            grade.flare0.x
#define uLensFlareSunPos              grade.flare0.yz
#define sky_hdr_scale                 grade.flare0.w
#define uLensFlareLightColor          grade.flare1.rgb
#define uLensFlareGlow                grade.flare1.w
#define uLensFlareStreakLength        grade.flare2.x
#define uLensFlareStreakFalloff       grade.flare2.y
#define uLensFlareStreakWidth         grade.flare2.z
#define uLensFlareStreakIntensity     grade.flare2.w
#define uLensFlareStreakTint          grade.flare3.rgb
#define uLensFlareChromaticSpread     grade.flare3.w
#define uLensFlareGlowRadius          grade.flare4.x
#define uLensFlareGlowFalloff         grade.flare4.y
#define uLensFlareGhost               grade.flare4.z
#define uLensFlareGhostCount          int(grade.flare4.w + 0.5)
#define uLensFlareGhostSpacing        grade.flare5.x
#define uLensFlareHalo                grade.flare5.y
#define uLensFlareHaloRadius          grade.flare5.z
#define uLensFlareHaloWidth           grade.flare5.w
#define uLensFlareStarburst           grade.flare6.x
#define uLensFlareStarburstSpikes     int(grade.flare6.y + 0.5)
#define uLensFlareStarburstSharpness  grade.flare6.z
#define uLensFlareStarburstFalloff    grade.flare6.w
#define uResolution                   grade.flare7.xy

// upstream Alchemy postEffectUtilsF.glsl, verbatim
vec3 computeLensFlare(vec2 uv)
{
    // Master gate: cheapest possible early-out.
    if (uLensFlareStrength <= 0.0)
        return vec3(0.0);

    // The filtered drive from the state pass: colour and visibility in one.
    vec3 sun_color = texelFetch(uLensFlareStateMap, ivec2(0, 0), 0).rgb;
    if (max(sun_color.r, max(sun_color.g, sun_color.b)) <= 0.0)
        return vec3(0.0);

    vec2  sun_uv = uLensFlareSunPos;
    float vis    = uLensFlareStrength;

    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec2  delta  = uv - sun_uv;
    vec3  flare  = vec3(0.0);

    // ---- Central glow: soft radial falloff --------------------------------
    // Scaled by sky_hdr_scale (same as starburst) so the glow reads at the
    // same relative brightness as the atmosphere it's painted against.
    if (uLensFlareGlow > 0.0)
    {
        vec2  gd      = vec2(delta.x * aspect, delta.y);
        float radius2 = max(uLensFlareGlowRadius * uLensFlareGlowRadius, 1e-8);
        // r² directly from dot() — avoids the length() sqrt.
        float glow    = exp(-dot(gd, gd) * uLensFlareGlowFalloff / radius2);
        flare += sun_color * glow * uLensFlareGlow * sky_hdr_scale;
    }

    // ---- Anamorphic streak: horizontal band with tight vertical gaussian --
    // Early-out when either the streak is disabled or the fragment is far
    // enough off-axis that the vertical gaussian is numerically zero. The
    // bound uses the widened fringe sigma when chromatic spread is active.
    float streak_w = max(uLensFlareStreakWidth,  1e-5);
    float streak_l = max(uLensFlareStreakLength, 1e-5);
    float fringe_w = streak_w + max(uLensFlareChromaticSpread, 0.0) * 0.5;
    if (uLensFlareStreakIntensity > 0.0 && abs(delta.y) < fringe_w * 8.0)
    {
        float horiz = abs(delta.x) * aspect;
        float vert  = abs(delta.y);

        float vert_falloff  = exp(-vert * vert / (streak_w * streak_w));
        float horiz_falloff = exp(-horiz * uLensFlareStreakFalloff / streak_l);
        float streak        = vert_falloff * horiz_falloff;

        // Chromatic fringing — offset R and B vertically from the base streak.
        // The fringe channels use a widened gaussian so they stay visible
        // even when the spread exceeds the base streak width. `fringe_w` is
        // already computed above for the early-out bound.
        if (uLensFlareChromaticSpread > 0.0)
        {
            float spread   = uLensFlareChromaticSpread;
            float vert_r   = abs(delta.y + spread);
            float vert_b   = abs(delta.y - spread);
            float streak_r = exp(-vert_r * vert_r / (fringe_w * fringe_w)) * horiz_falloff;
            float streak_b = exp(-vert_b * vert_b / (fringe_w * fringe_w)) * horiz_falloff;

            flare.r += sun_color.r * streak_r * uLensFlareStreakTint.r * uLensFlareStreakIntensity;
            flare.g += sun_color.g * streak   * uLensFlareStreakTint.g * uLensFlareStreakIntensity;
            flare.b += sun_color.b * streak_b * uLensFlareStreakTint.b * uLensFlareStreakIntensity;
        }
        else
        {
            flare += sun_color * streak * uLensFlareStreakTint * uLensFlareStreakIntensity;
        }
    }

    // ---- Ghosts: soft disks stepped along the sun→center axis -------------
    if (uLensFlareGhost > 0.0 && uLensFlareGhostCount > 0)
    {
        vec2 ghost_vec = vec2(0.5) - sun_uv;
        for (int i = 0; i < uLensFlareGhostCount; i++)
        {
            float t        = uLensFlareGhostSpacing * float(i + 1);
            vec2  ghost_uv = sun_uv + ghost_vec * t;
            float scale    = 1.0 / float(i + 1);        // later ghosts fade out
            float radius   = 0.04 * scale + 0.02;

            vec2 gd = uv - ghost_uv;
            gd.x *= aspect;
            float d    = length(gd);
            float disk = 1.0 - smoothstep(radius * 0.5, radius, d);

            flare += sun_color * disk * scale * uLensFlareGhost;
        }
    }

    // ---- Halo: thin ring on the opposite side of the screen from the sun -
    if (uLensFlareHalo > 0.0 && uLensFlareHaloRadius > 0.0)
    {
        vec2  halo_center = vec2(0.5) + (vec2(0.5) - sun_uv);
        float halo_dist   = length(uv - halo_center);
        float halo_w      = max(uLensFlareHaloWidth, 0.01);
        float halo        = 1.0 - abs(halo_dist - uLensFlareHaloRadius) / halo_w;
        halo  = clamp(halo, 0.0, 1.0);
        halo *= halo;                                    // soften the edges
        flare += sun_color * halo * 0.3 * uLensFlareHalo;
    }

    // ---- Starburst: angular spikes radiating from the sun -----------------
    // Three stacked cos^N harmonics give a richer star pattern than one.
    //
    // The starburst pattern factors as `envelope(sun_dist) * angular(θ)`.
    // Computing the envelope first lets us skip the three pow/cos terms
    // and the atan when envelope is vanishingly small — which is the case
    // for most fragments (spikes are localized around the sun).
    if (uLensFlareStarburst > 0.0)
    {
        vec2 sd = delta;
        sd.x *= aspect;
        float sun_dist = length(sd);

        // Radial decay × core-fade. The core_fade is what prevents the
        // spikes from stacking on top of the glow's bright center — they
        // emanate from its edge instead.
        float radial    = exp(-sun_dist * max(uLensFlareStarburstFalloff, 0.01));
        float core_fade = smoothstep(0.0, max(uLensFlareGlowRadius, 1e-4), sun_dist);
        float envelope  = radial * core_fade;

        if (envelope > 1e-5)
        {
            float angle   = atan(sd.y, sd.x);
            float primary = float(max(uLensFlareStarburstSpikes, 1));
            float sharp   = max(uLensFlareStarburstSharpness, 1.0);

            float pattern = pow(abs(cos(angle * primary)),             sharp       ) * 0.5
                          + pow(abs(cos(angle * primary * 2.0 + 0.5)), sharp * 1.33) * 0.3
                          + pow(abs(cos(angle * primary * 4.0 + 1.0)), sharp * 1.66) * 0.2;

            flare += sun_color * pattern * envelope
                   * uLensFlareStarburst * sky_hdr_scale;
        }
    }

    // Final scale: 0.15 tames peak intensity to a plausible lens-response
    // range; tint and the master strength apply equally to all sub-effects.
    return max(flare * vis * uLensFlareLightColor * 0.15, vec3(0.0));
}

// (#820) The tail by TETRAHEDRAL interpolation of the baked lattice: the cell's corner on the
// main diagonal and the two corners of the tetrahedron the fractions' order selects (4 fetches).
// Exact along the grey axis (the diagonal is an edge of every tetrahedron), where trilinear blends
// in off-axis colours. Integer fetches: no sampler filtering, no texel-centre offsets.
vec3 tonemapLutSample(vec3 c)
{
    const int n = int(pc.p2.w + 0.5);
    const vec3 u = tmLutShape(mix(c, vec3(0.0), isnan(c))) * float(n - 1);   // (NaN: black, never a wild fetch)
    const ivec3 i0 = clamp(ivec3(u), ivec3(0), ivec3(n - 2));
    const vec3 f = clamp(u - vec3(i0), 0.0, 1.0);
    ivec3 o1, o2;
    vec3 w;   // the fractions in descending order
    if (f.x >= f.y)
    {
        if (f.y >= f.z)      { o1 = ivec3(1, 0, 0); o2 = ivec3(1, 1, 0); w = f.xyz; }
        else if (f.x >= f.z) { o1 = ivec3(1, 0, 0); o2 = ivec3(1, 0, 1); w = f.xzy; }
        else                 { o1 = ivec3(0, 0, 1); o2 = ivec3(1, 0, 1); w = f.zxy; }
    }
    else
    {
        if (f.z >= f.y)      { o1 = ivec3(0, 0, 1); o2 = ivec3(0, 1, 1); w = f.zyx; }
        else if (f.z >= f.x) { o1 = ivec3(0, 1, 0); o2 = ivec3(0, 1, 1); w = f.yzx; }
        else                 { o1 = ivec3(0, 1, 0); o2 = ivec3(1, 1, 0); w = f.yxz; }
    }
    const vec3 c0 = texelFetch(uTonemapLut, i0, 0).rgb;
    const vec3 c1 = texelFetch(uTonemapLut, i0 + o1, 0).rgb;
    const vec3 c2 = texelFetch(uTonemapLut, i0 + o2, 0).rgb;
    const vec3 c3 = texelFetch(uTonemapLut, i0 + ivec3(1), 0).rgb;
    return (1.0 - w.x) * c0 + (w.x - w.y) * c1 + (w.y - w.z) * c2 + w.z * c3;
}

vec3 sampleSceneCA(vec2 uv)
{
    if (grade.ca0.x <= 0.0) return texture(uHdr, uv).rgb;
    vec2 dir = uv - 0.5;
    dir *= vec2(1.0 - max(grade.ca0.z, 0.0), 1.0 - max(-grade.ca0.z, 0.0));
    float aspect = grade.ca1.z / max(grade.ca1.w, 1.0);
    vec2  scale  = max(vec2(aspect, 1.0 / max(aspect, 1e-4)), 1.0);
    float normDist = clamp(length(dir * scale) / (0.5 * length(scale)), 0.0, 1.0);
    float dist = pow(normDist, grade.ca0.y);
    float s = grade.ca1.x, c = grade.ca1.y;
    vec2 rotDir = vec2(c * dir.x - s * dir.y, s * dir.x + c * dir.y);
    mat2 basis = mat2(rotDir, vec2(-rotDir.y, rotDir.x));
    float strength = grade.ca0.x * dist;
    vec3 d;
    d.r = texture(uHdr, uv + (basis * grade.ca2.xy) * strength).r;
    d.g = texture(uHdr, uv).g;
    d.b = texture(uHdr, uv + (basis * grade.ca2.zw) * strength).b;
    return d;
}

// RenderVKDebugMotionVectors: the raw motion-vector target. Hue = direction of the
// screen motion (red = moving right, cyan = left; the wheel turns through yellow-green
// for up and blue-magenta for down), brightness = speed, full at 16 px / frame (sqrt
// ramp so sub-pixel motion still shows). Black = no motion OR no geometry (sky: the TAA
// stage derives its camera motion from depth).
vec3 motionDebug(vec2 uv)
{
    const vec2 px = texture(uMotion, uv).xy * vec2(textureSize(uMotion, 0));
    const float len = length(px);
    if (len < 1e-4) return vec3(0.0);
    const float hue = atan(-px.y, px.x) / 6.28318531 + 1.0;   // screen up = +angle
    const vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return rgb * sqrt(clamp(len / 16.0, 0.0, 1.0));
}

void main()
{
    if (pc.p1.w > 0.5)
    {
        outColor = vec4(motionDebug(v_uv), 1.0);
        return;
    }
    // (Motion blur, #118, runs on the render-size scene before the TAA / FSR resolve:
    // vkgfx/llcleanmotionblurvk.h.)
    vec3 c = sampleSceneCA(v_uv);
    // Light on the front element, accumulated as it is composited (lens dirt).
    vec3 lens_light = vec3(0.0);
    {
        vec3 flare = computeLensFlare(v_uv);
        c += flare;
        lens_light += flare * grade.lens_fx.w;
    }
    // HDR bloom (+ cross-filter streaks), composited before exposure so it rolls
    // through the same curve.
    vec3 c_pre_bloom = c;      // (#998) the pixel before the bloom term, for the saturation preserve
    if (grade.bloom.w > 0.5)
    {
        vec4 b = texture(uBloom, v_uv);
        vec3 bloom_term = b.rgb * grade.bloom.x + b.a * grade.bloom.y * grade.halation_tint.rgb;
        if (grade.lens_fx.x > 0.0)
            bloom_term += texture(uCrossFilter, v_uv).rgb * grade.lens_fx.x;
        c += bloom_term;
        lens_light += bloom_term * grade.lens_fx.z;
    }
    // (#998) BLOOM SATURATION PRESERVE. grade.bloom.z = strength 0..1 (0 = off, today's look).
    // Stage 1, hue/chroma restore (scene-referred, so it works under every tonemapper / LUT):
    // the blur mixes the surroundings' colour over the source and the added energy pushes a
    // coloured highlight toward the tonemapper's white. Keep the bloomed pixel's LUMINANCE
    // (the bloom still adds its light) but pull its CHROMA back toward the source's:
    //     c' = mix(c, (c0 / L0) * L, s * L0 / L)
    // c0/L0 is the source colour normalised to unit luminance, L the post-bloom luminance.
    // The weight L0/L is the source's share of the pixel's light: on the glowing texel itself
    // ~1 (hue = the source's), on dark neighbours ~0 (the halo keeps its own, blended colour).
    float bsp = (grade.bloom.w > 0.5) ? grade.bloom.z : 0.0;
    float bloom_frac = 0.0;   // bloom's share of the pixel's luminance, for stage 2
    if (bsp > 0.0)
    {
        float L0 = dot(max(c_pre_bloom, vec3(0.0)), CG_LUMA);
        float L1 = dot(max(c, vec3(0.0)), CG_LUMA);
        bloom_frac = (L1 > 1e-5) ? clamp(1.0 - L0 / L1, 0.0, 1.0) : 0.0;
        if (L0 > 1e-5 && L1 > L0)
            c = mix(c, (max(c_pre_bloom, vec3(0.0)) / L0) * L1, bsp * (L0 / L1));
    }
    if (grade.lens_fx.y > 0.0)
        c += lens_light * texture(uLensDirtMap, v_uv).r * grade.lens_fx.y;

    if (pc.p1.y > 0.5)   // DEBUG: raw clamped HDR -> sRGB (bypasses exposure/tonemap)
    {
        outColor = vec4(linear_to_srgb(c), 1.0);
        return;
    }

    // no_post (p1.x): a probe-ambiance-0 legacy sky, or vintage mode (HDR off) -- upstream
    // colorCorrect without TONEMAP: no exposure, no tonemapper, a clamp. The colour grade's
    // linear stage (white balance, lift / gamma / gain) runs either way, between exposure
    // and the tonemap / clamp, as upstream's COLOR_GRADE does independently of TONEMAP.
    if (pc.p1.x < 0.5)
    {
        // toneMap(): exposure * dynamic scale, tonemapper, mix with the clamped input.
        float exp_scale = (pc.p1.z > 0.5) ? imageLoad(uExposure, ivec2(0)).r : 1.0;
        if (isnan(exp_scale) || isinf(exp_scale) || exp_scale <= 0.0) exp_scale = 1.0;   // (NaN safety)
        c *= pc.p0.x * exp_scale;
    }
    // (#820) Everything from here to the dither is tonemapTail: one baked-LUT lookup when the
    // renderer baked it for this frame's grade / tonemapper (pc.p2.w = its size), else direct.
    vec3 c_scene = c;   // (#998) scene-referred, post-exposure colour entering the tail
    c = pc.p2.w > 1.5 ? tonemapLutSample(c) : tonemapTail(c);
    // (#998) Stage 2, HUE-PRESERVING HIGHLIGHTS (grade.halation_tint.w = on; scaled by the
    // saturation-preserve strength; only where bloom contributes): a per-channel curve's
    // shoulder drives a bright saturated channel set toward white. Instead tonemap only the
    // pixel's LUMINANCE (the tail applied to the grey of equal luminance -- so it is whatever
    // tonemapper / LUT / grade is active), and rescale the source chroma to that luminance:
    //     out_lin = (c / L) * Ld,   Ld = luminance of tail(grey L)
    // Channels pushed past 1 by the rescale are folded back by desaturating toward Ld only as
    // far as needed (constant luminance), so near true white it still rolls off to white.
    if (bsp > 0.0 && grade.halation_tint.w > 0.5 && pc.p1.x < 0.5 && bloom_frac > 0.0)
    {
        float Ls = dot(max(c_scene, vec3(0.0)), CG_LUMA);
        if (Ls > 1e-5)
        {
            vec3 g = vec3(Ls);
            g = pc.p2.w > 1.5 ? tonemapLutSample(g) : tonemapTail(g);   // display-encoded grey
            vec3 g_lin = mix(pow((g + 0.055) / 1.055, vec3(2.4)), g / 12.92, lessThan(g, vec3(0.04045)));
            float Ld = dot(g_lin, CG_LUMA);
            vec3 hp = (max(c_scene, vec3(0.0)) / Ls) * Ld;
            float mx = max(hp.r, max(hp.g, hp.b));
            if (mx > 1.0 && mx > Ld)
                hp = vec3(Ld) + (hp - vec3(Ld)) * clamp((1.0 - Ld) / (mx - Ld), 0.0, 1.0);
            c = mix(c, linear_to_srgb(hp), bsp * clamp(bloom_frac * 4.0, 0.0, 1.0));
        }
    }
    if (pc.p2.x > 0.0) c = clamp(applyDither(c, gl_FragCoord.xy), vec3(0.0), vec3(1.0));
    outColor = vec4(c, 1.0);
}
