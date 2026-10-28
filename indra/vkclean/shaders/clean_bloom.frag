#version 450
// vkclean — HDR bloom pyramid (Lightbox; port of upstream Alchemy class1/effects/
// bloomExtractF, bloomDownsampleF, bloomUpsampleF). One module, the pass picked by
// push mode: 0 extract (soft threshold + firefly clamp + per-face glow, halation in
// alpha), 1 first downsample (Karis average), 2 downsample, 3 upsample (tent, drawn
// with additive blending into the next larger mip).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;    // scene (extract) or the source mip
layout(set = 0, binding = 1) uniform sampler2D uGlow;   // per-face glow target (extract only)

layout(push_constant) uniform BloomPush {
    vec4 p0;   // texel size of the source (xy), mode, scatter (mode 3) / <GMDX> threshold term weight (mode 0, 0 = 1)
    vec4 p1;   // threshold, knee, firefly clamp, alpha glow boost
    vec4 p2;   // halation luminance weights (rgb), halation on
} pc;

float softThreshold(float lum, float T, float K)
{
    float k = max(K, 1e-4);
    float soft = clamp(lum - T + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 1e-4);
    return max(soft, lum - T);
}
float maxLum(vec4 c) { return max(max(c.r, c.g), c.b); }
vec4 karisBox(vec4 a, vec4 b, vec4 c, vec4 d)
{
    float la = maxLum(a), lb = maxLum(b), lc = maxLum(c), ld = maxLum(d);
    float wa = 1.0 / (1.0 + max(la - max(max(lb, lc), ld), 0.0));
    float wb = 1.0 / (1.0 + max(lb - max(max(la, lc), ld), 0.0));
    float wc = 1.0 / (1.0 + max(lc - max(max(la, lb), ld), 0.0));
    float wd = 1.0 / (1.0 + max(ld - max(max(la, lb), lc), 0.0));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

void main()
{
    const int mode = int(pc.p0.z + 0.5);
    const vec2 uv = v_uv;
    if (mode == 0)
    {
        vec3 rgb = max(texture(uSrc, uv).rgb, vec3(0.0));
        float peak = max(max(rgb.r, rgb.g), rgb.b);
        rgb *= (peak > pc.p1.z) ? (pc.p1.z / peak) : 1.0;
        float lum = max(max(rgb.r, rgb.g), rgb.b);
        // <SS:Skoomastream> Threshold bloom is OFF by default (p1.x < 0: RenderBloomThreshold
        // at its maximum, 8): Firestorm / LL have none (GLOW_MIN_LUMINANCE 9999), so only
        // face glow blooms. Halation is a threshold effect too and goes with it.
        const bool thr_on = pc.p1.x >= 0.0;
        float factor = thr_on ? softThreshold(lum, pc.p1.x, pc.p1.y) / max(lum, 1e-4) : 0.0;
        factor *= pc.p0.w > 0.0 ? pc.p0.w : 1.0;   // <GMDX> night bloom (llcleanrenderervk.cpp recordBloom)
        // Per-face glow as LL: the glow target holds the glow amount (blended coverage),
        // the colour comes from the composited scene here -- clamped to the LDR range
        // (peak normalised to 1), since LL's glowExtract reads the tone-mapped pixel
        // (<= 1), not this HDR one (up to the firefly clamp, 24).
        // Boost (p1.w, RenderBloomAlphaGlowBoost, default 2.0) vs LL's LDR glow: LL adds
        // (5.1 kernel sum * RenderGlowStrength 0.325)^(2 * 2 iterations) ~= 7.5x the
        // extracted energy; this pyramid sums its 5 mips into mip 0, composited at
        // RenderBloomStrength 0.325, i.e. ~1.6 * boost. Equal energy would be ~4.6, but
        // this halo is added in LINEAR light before the tonemap + sRGB encode, which
        // lifts the faint tail (a 10% linear add reads ~35% on screen): matching LL's
        // visible halo over a dark surround (5..20% of the face's extracted energy per
        // pixel) needs ~1.5..3.3. 2.0 sits there, below equal energy so the wide tail
        // does not out-glow LL.
        float glow_amt = max(texture(uGlow, uv).r, 0.0);
        vec3 glow_rgb = rgb / max(lum, 1.0);
        vec3 bloom = rgb * factor + glow_rgb * glow_amt * pc.p1.w;
        // </SS:Skoomastream>
        float hal = 0.0;
        if (thr_on && pc.p2.w > 0.5)
        {
            float warmth = max(rgb.r * pc.p2.r, max(rgb.g * pc.p2.g, rgb.b * pc.p2.b));
            hal = softThreshold(warmth, pc.p1.x, pc.p1.y);
        }
        outColor = vec4(bloom, hal);
        return;
    }
    const vec2 t = pc.p0.xy;
    if (mode == 3)
    {
        vec2 s = t * pc.p0.w;
        vec4 r;
        r  = texture(uSrc, uv + s * vec2(-1.0, -1.0)) * (1.0 / 16.0);
        r += texture(uSrc, uv + s * vec2( 0.0, -1.0)) * (2.0 / 16.0);
        r += texture(uSrc, uv + s * vec2( 1.0, -1.0)) * (1.0 / 16.0);
        r += texture(uSrc, uv + s * vec2(-1.0,  0.0)) * (2.0 / 16.0);
        r += texture(uSrc, uv)                         * (4.0 / 16.0);
        r += texture(uSrc, uv + s * vec2( 1.0,  0.0)) * (2.0 / 16.0);
        r += texture(uSrc, uv + s * vec2(-1.0,  1.0)) * (1.0 / 16.0);
        r += texture(uSrc, uv + s * vec2( 0.0,  1.0)) * (2.0 / 16.0);
        r += texture(uSrc, uv + s * vec2( 1.0,  1.0)) * (1.0 / 16.0);
        outColor = r;
        return;
    }
    vec4 a = texture(uSrc, uv + t * vec2(-2.0, -2.0));
    vec4 b = texture(uSrc, uv + t * vec2( 0.0, -2.0));
    vec4 c = texture(uSrc, uv + t * vec2( 2.0, -2.0));
    vec4 d = texture(uSrc, uv + t * vec2(-1.0, -1.0));
    vec4 e = texture(uSrc, uv + t * vec2( 1.0, -1.0));
    vec4 f = texture(uSrc, uv + t * vec2(-2.0,  0.0));
    vec4 g = texture(uSrc, uv);
    vec4 h = texture(uSrc, uv + t * vec2( 2.0,  0.0));
    vec4 i = texture(uSrc, uv + t * vec2(-1.0,  1.0));
    vec4 j = texture(uSrc, uv + t * vec2( 1.0,  1.0));
    vec4 k = texture(uSrc, uv + t * vec2(-2.0,  2.0));
    vec4 l = texture(uSrc, uv + t * vec2( 0.0,  2.0));
    vec4 m = texture(uSrc, uv + t * vec2( 2.0,  2.0));
    vec4 grp0, grp1, grp2, grp3, grp4;
    if (mode == 1)
    {
        grp0 = karisBox(d, e, i, j); grp1 = karisBox(a, b, f, g); grp2 = karisBox(b, c, g, h);
        grp3 = karisBox(f, g, k, l); grp4 = karisBox(g, h, l, m);
    }
    else
    {
        grp0 = (d + e + i + j) * 0.25; grp1 = (a + b + f + g) * 0.25; grp2 = (b + c + g + h) * 0.25;
        grp3 = (f + g + k + l) * 0.25; grp4 = (g + h + l + m) * 0.25;
    }
    outColor = grp0 * 0.5 + (grp1 + grp2 + grp3 + grp4) * 0.125;
}
