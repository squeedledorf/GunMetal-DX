#version 450
// vkclean — REDUCED-RESOLUTION PARTICLES (RenderVKParticleLowRes; llcleanpartlowresvk.cpp).
// Two fullscreen passes around the half-resolution particle draw:
//
//  -DPART_LR_DEPTH  DEPTH DOWNSAMPLE (depth-only scope, gl_FragDepth, compare ALWAYS):
//     each half-res texel takes the FARTHEST of its 2x2 full-res depths. A texel that
//     straddles a silhouette then lets a particle between the two surfaces through (it
//     is behind the near one, in front of the far one); the composite's nearest-depth
//     upsample gives that particle to the far-side pixels only. (Thin foreground features
//     wholly inside mixed texels can show a faint particle halo: the standard trade.)
//
//  default          COMPOSITE (inside a pass-2 scope; HDR + glow + reactive + depth):
//     the accumulator holds C (rgb) and the destination transmittance T (a) of every
//     particle the half-res pass drew (dst' = dst * T + C, exact for the pairs it takes);
//     glow and the reactive coverage alongside. Each full-res pixel takes the bilinear
//     2x2 footprint restricted to the half-res taps whose depth agrees with its own
//     (renormalised; #879 -- was all-or-nothing), else the single tap whose depth is
//     nearest (nearest-depth upsampling). Output blends:
//     colour ONE / SRC_ALPHA (dst * T + C), glow ONE / ONE, reactive MAX.
//
// Depth is REVERSED-Z (near 1, far 0, ~ 1 / view distance), so a RELATIVE depth
// difference is a relative distance difference.

layout(set = 0, binding = 0) uniform sampler2D uDepth;      // full-res scene depth (read-only)

layout(push_constant) uniform LowResPush {
    ivec4 dims;     // x, y full-res extent; z, w half-res extent
    vec4  params;   // x relative depth tolerance (composite), y 1 = reversed Z
} pc;

#ifdef PART_LR_DEPTH

void main()
{
    const ivec2 o = ivec2(gl_FragCoord.xy) * 2;
    const ivec2 mx = pc.dims.xy - 1;
    const float a = texelFetch(uDepth, min(o,               mx), 0).r;
    const float b = texelFetch(uDepth, min(o + ivec2(1, 0), mx), 0).r;
    const float c = texelFetch(uDepth, min(o + ivec2(0, 1), mx), 0).r;
    const float d = texelFetch(uDepth, min(o + ivec2(1, 1), mx), 0).r;
    gl_FragDepth = (pc.params.y > 0.5) ? min(min(a, b), min(c, d))    // reversed Z: far = small
                                       : max(max(a, b), max(c, d));
}

#else

layout(set = 0, binding = 1) uniform sampler2D uLowDepth;
layout(set = 0, binding = 2) uniform sampler2D uLowColor;   // rgb C, a T
layout(set = 0, binding = 3) uniform sampler2D uLowGlow;
layout(set = 0, binding = 4) uniform sampler2D uLowReact;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outReact;

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const float d = texelFetch(uDepth, px, 0).r;
    // Half-res texel space: full-res pixel centre (p + 0.5) maps to (p + 0.5) / 2 - 0.5.
    const vec2 lp = (vec2(px) + 0.5) * 0.5 - 0.5;
    const ivec2 b = ivec2(floor(lp));
    const vec2 f = lp - vec2(b);
    const ivec2 lmx = pc.dims.zw - 1;
    ivec2 t[4];
    t[0] = clamp(b,               ivec2(0), lmx);
    t[1] = clamp(b + ivec2(1, 0), ivec2(0), lmx);
    t[2] = clamp(b + ivec2(0, 1), ivec2(0), lmx);
    t[3] = clamp(b + ivec2(1, 1), ivec2(0), lmx);
    const float w[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                                (1.0 - f.x) * f.y,         f.x * f.y);
    // EDGE-AWARE (#879): the bilinear weights of the taps whose depth agrees with this
    // pixel's, renormalised -- all four inside a surface (plain bilinear), the agreeing
    // subset along a silhouette (a smooth edge instead of one tap's 2x2 block); the
    // single nearest-depth tap only when no tap agrees.
    float wa[4];
    float wsum = 0.0;
    int best = 0;
    float bestE = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        const float ld = texelFetch(uLowDepth, t[i], 0).r;
        const float e = abs(ld - d);
        wa[i] = (e > pc.params.x * max(d, ld) + 1e-7) ? 0.0 : w[i];
        wsum += wa[i];
        if (e < bestE) { bestE = e; best = i; }
    }
    vec4 c, g;
    float r;
    if (wsum > 1e-4)
    {
        c = vec4(0.0); g = vec4(0.0); r = 0.0;
        const float inv = 1.0 / wsum;
        for (int i = 0; i < 4; ++i)
        {
            if (wa[i] <= 0.0) continue;
            const float k = wa[i] * inv;
            c += texelFetch(uLowColor, t[i], 0) * k;
            g += texelFetch(uLowGlow,  t[i], 0) * k;
            r += texelFetch(uLowReact, t[i], 0).r * k;
        }
    }
    else
    {
        c = texelFetch(uLowColor, t[best], 0);
        g = texelFetch(uLowGlow,  t[best], 0);
        r = texelFetch(uLowReact, t[best], 0).r;
    }
    // Nothing drawn here (T = 1, C = 0, no glow, no coverage): leave the targets alone.
    if (c.a >= 0.99999 && max(max(c.r, c.g), c.b) <= 0.0 && max(max(g.r, g.g), g.b) <= 0.0 && r <= 0.0)
        discard;
    outColor = vec4(max(c.rgb, vec3(0.0)), clamp(c.a, 0.0, 1.0));
    outGlow  = vec4(max(g.rgb, vec3(0.0)), 0.0);
    outReact = vec4(clamp(r, 0.0, 1.0), 0.0, 0.0, 0.0);
}

#endif
