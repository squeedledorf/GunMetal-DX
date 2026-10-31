#version 450
#extension GL_GOOGLE_include_directive : require
// GUNMETAL-DX VOLUMETRIC CLOUDS: the main-view COMPOSITE (render resolution). Slipstream's
// clean_vol_cloud_composite.frag on GunMetal-DX's parameter block, plus the thinning of a surface
// inside its texel's march and the Sky Debugger's texture views. Copyright (C) 2026, squeedledorf
// (GunMetal-DX). LGPL 2.1, as the viewer.
//
// A fullscreen draw over the HDR scene after pass 1 (the opaque world + the sky), before the
// pass-2 transparents / water (they draw over the clouds; the water refraction copy and next
// frame's SSR see them). Blend ONE / SRC_ALPHA: scene' = scene x T + in-scatter.
//
// DEPTH-AWARE UPSAMPLE: the four quarter-resolution texels around the pixel are weighted
// bilinearly, and a texel's clouds only apply where this pixel's surface lies BEYOND the
// texel's cloud entry distance (B.x): a tree, a roof or a mountain in front of the layer
// keeps T = 1 even where its 2x2 block marched to the sky behind it. A pixel whose surface lies
// inside its texel's march (nearer than where the march ended, B.w) takes only the part of the
// cloud in front of it -- inside a cloud, an avatar's edge no longer wears the full depth of fog
// the sky beside it marched through.
// PHYSICAL CAMERA MEDIUM (Slipstream #1266, cmOn): the march holds raw cloud radiance; the medium
// in front of the clouds' mean distance is applied here, after the reconstruction.
layout(location = 0) out vec4 outColor;

#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"
#include "clean_cloud_shadow_map.glsl"   // (uCloudShadowMap: the Sky Debugger's view)
#include "clean_vol_cloud_sky.glsl"      // (uCloudPano: the Sky Debugger's view)
#define VC_SET 1
#include "gmdx_cloud_ubo.glsl"
layout(set = 1, binding = 1) uniform sampler2D uDepth;   // world depth (texelFetch)
layout(set = 1, binding = 2) uniform sampler2D uCloudA;  // rgb in-scatter, a transmittance
layout(set = 1, binding = 3) uniform sampler2D uCloudB;  // x entry distance, y mean distance, w march end
layout(set = 1, binding = 4) uniform sampler2D uWeatherView;   // the weather map (Sky Debugger)
layout(set = 1, binding = 5) uniform sampler2D uWorldView;     // the world coverage (Sky Debugger)

// SKY DEBUGGER TEXTURE VIEWS (vc.debug.x; GMDXSkyTune::cloud_view): one of the module's / the
// physical sky's images in a square at the bottom right (clear of the Sky Debugger's default
// place on the left). false = this pixel is outside it.
bool debugView(ivec2 px, vec2 rdim, out vec3 c)
{
    const int view = int(vc.debug.x + 0.5);
    c = vec3(0.0);
    if (view < 3) return false;
    const float side = floor(min(rdim.x, rdim.y) * 0.5);
    const vec2 uv = (vec2(px) + 0.5 - vec2(rdim.x - 8.0 - side, rdim.y - 8.0 - side)) / side;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    if (view == 3) c = textureLod(uWeatherView, uv, 0.0).rgb;          // coverage, type, density
    else if (view == 4) c = vec3(textureLod(uWeatherView, uv, 0.0).a);   // height scale
    else if (view == 5) c = vec3(textureLod(uWorldView, uv, 0.0).r);     // world coverage
    else if (view == 6) c = vec3(textureLod(uCloudShadowMap, uv, 0.0).r);   // cloud shadow (transmittance)
    else if (view == 7)
    {
        const vec4 p = textureLod(uCloudPano, uv, 0.0);                  // panorama over grey
        c = p.rgb + vec3(0.18) * p.a;
    }
    else if (view == 8) c = textureLod(uPkTrans, uv, 0.0).rgb;           // physical sky: transmittance LUT
    else if (view == 9) c = textureLod(uPkSkyView, uv, 0.0).rgb;         // physical sky: sky-view LUT
    return true;
}

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec2 rdim = vc.dims.xy, cdim = vc.dims.zw;
    {
        vec3 c;
        if (debugView(px, rdim, c)) { outColor = vec4(max(c, vec3(0.0)), 0.0); return; }
    }
    const float d = texelFetch(uDepth, px, 0).r;
    float dist = 1e9;   // sky
    if (d > 0.0)
    {
        vec2 ndc = (vec2(px) + 0.5) / rdim * 2.0 - 1.0;
        const vec4 w = vc.inv_vp * vec4(ndc, d, 1.0);   // (jitter: sub-pixel, irrelevant at km)
        dist = length(w.xyz / w.w);
        if (cmOn())
        {
            vec3 O, V; cmRay((vec2(px) + 0.5) / rdim, MEDIUM_EYE, O, V);
            dist = max(dot(vc.cam.xyz + w.xyz / w.w - O, V), 0.0);
        }
    }
    // Bilinear footprint in the cloud image.
    const vec2 cp = (vec2(px) + 0.5) * (cdim / rdim) - 0.5;
    const ivec2 c0 = ivec2(floor(cp));
    const vec2 f = cp - vec2(c0);
    const ivec2 cmax = ivec2(cdim) - 1;
    vec4 acc = vec4(0.0);
    float wsum = 0.0, meanNum = 0.0, meanDen = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const ivec2 q = clamp(c0 + ivec2(i, j), ivec2(0), cmax);
            const float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            vec4 a = texelFetch(uCloudA, q, 0);
            const vec4 b = texelFetch(uCloudB, q, 0);
            const float entry = b.x, end = b.w;
            // In front of this texel's clouds: nothing (T = 1). Inside its march: the share in
            // front of the surface, as a uniform medium (T^f, in-scatter in proportion).
            if (dist <= entry) a = vec4(0.0, 0.0, 0.0, 1.0);
            else
            {
                if (end > 0.0 && dist < end)
                {
                    const float f = clamp((dist - entry) / max(end - entry, 1.0), 0.0, 1.0);
                    const float Tp = pow(clamp(a.a, 1e-4, 1.0), f);
                    a.rgb *= a.a < 0.9999 ? (1.0 - Tp) / (1.0 - a.a) : f;
                    a.a = Tp;
                }
                const float ow = w * (1.0 - a.a);
                meanNum += ow * b.y;
                meanDen += ow;
            }
            acc += w * a;
            wsum += w;
        }
    acc /= max(wsum, 1e-6);
    if (acc.a >= 0.999 && dot(acc.rgb, vec3(1.0)) <= 1e-5) discard;
    // LIGHTNING (#783 M2, vc.flash): the flash lights the clouds from within -- in proportion to
    // their opacity, most toward the strike (cool white).
    if (vc.flash.w > 0.0)
    {
        const vec2 ndc = (vec2(px) + 0.5) / rdim * 2.0 - 1.0;
        const vec4 w = vc.inv_vp * vec4(ndc, 0.5, 1.0);
        const vec3 dir = normalize(w.xyz / w.w);
        const float toward = max(dot(dir, vc.flash.xyz), 0.0);
        acc.rgb += vec3(0.80, 0.88, 1.0) * (vc.flash.w * (1.0 - acc.a) * (0.3 + 0.7 * toward * toward));
    }
    if (cmOn() && (vc.debug.x < 0.5 || vc.debug.x > 2.5))   // (not for the unlit march views 1 / 2)
    {
        vec3 O, V; cmRay((vec2(px) + 0.5) / rdim, MEDIUM_EYE, O, V);
        const float mean = meanNum / max(meanDen, 1e-7);
        const CameraTransport prefix = cmAt(O + V * mean, MEDIUM_EYE);
        acc.rgb = prefix.T * acc.rgb + (1.0 - acc.a) * prefix.L;
    }
    outColor = vec4(max(acc.rgb, vec3(0.0)), clamp(acc.a, 0.0, 1.0));
}
