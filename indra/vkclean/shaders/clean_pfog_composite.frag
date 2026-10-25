#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean -- PARTICLE FOG (#1003): the COMPOSITE (render resolution), the second draw of
// render pass 2, right after the froxel fog's apply (clean_vol_apply.frag) -- so every
// transparent / particle drawn after it lays over the fogged scene, as with the froxel fog.
// Blend ONE / SRC_ALPHA: scene' = scene x Tb + out.rgb.
//
// DEPTH-AWARE UPSAMPLE of the march (checkerboard nearest / farthest representative depths):
// the 2 x 2 march texels around the pixel, bilinear weights x how well each texel's segment
// fits this pixel's surface distance d:
//   d <= the texel's entry distance        -> no medium in front of the surface: identity (exact)
//   open segment (ended at e) and d >= e    -> the same result for any deeper surface: weight 1
//   otherwise                               -> exp(-|d - end| / (0.03 d + 0.05))
// so a post or a railing inside the cloud takes a texel that marched to IT, not to the wall
// behind, and the cloud's edge against the sky stays sharp.
//
// ORDERING WITH THE FROXEL FOG AND THE HAZE (both already on the scene, C1 = C T_f + L_f):
// the brick medium sits at its mean distance m along the ray (a single depth: an
// approximation for a cloud deep in froxel fog). With (L_f', T_f') = the froxel fog over
// [eye, m] (volFogAt) and LL's haze over the same segment (additive / atten):
//   out.rgb = T_f' (Lb x atten + (1 - Tb) x haze) + L_f' (1 - Tb),  out.a = Tb
// = the fog / haze in front of the cloud stays in front of it, the cloud hides the fog and
// haze behind it in proportion to its opacity, and its own light is fogged by what is in front.
layout(location = 0) out vec4 outColor;

#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"
#include "clean_vol_apply.glsl"   // volFogAt (draw set 0 binding 45)
#define PF_SET 1
#define PF_NO_BUFFERS
#include "clean_pfog_common.glsl"

layout(set = 1, binding = 1) uniform sampler2D uDepthC;   // reversed-Z world depth (texelFetch)
layout(set = 1, binding = 2) uniform sampler2D uFogA;     // rgb in-scatter, a transmittance
layout(set = 1, binding = 3) uniform sampler2D uFogB;     // x entry, y mean distance, z segment end (the march's own)

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec2 rdim = pf.dims.xy, mdim = pf.dims.zw;
    const float dz = texelFetch(uDepthC, px, 0).r;
    vec2 ndc = (vec2(px) + 0.5) / rdim * 2.0 - 1.0;
    float dist = PF_FAR * 2.0;   // sky
    if (dz > 0.0)
    {
        vec2 jn = ndc;
        if (ssr.taa_params.w > 0.5) jn -= ssr.taa_jitter.zw;
        const vec4 w = pf.inv_vp * vec4(jn, dz, 1.0);
        dist = length(w.xyz / w.w);
    }
    const vec2 cp = (vec2(px) + 0.5) * (mdim / rdim) - 0.5;
    const ivec2 c0 = ivec2(floor(cp));
    const vec2 f = cp - vec2(c0);
    const ivec2 cmax = ivec2(mdim + 0.5) - 1;
    vec4 acc = vec4(0.0);
    float meanAcc = 0.0, meanW = 0.0, wsum = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const ivec2 q = clamp(c0 + ivec2(i, j), ivec2(0), cmax);
            const float wb = max((i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y), 1e-3);
            const vec4 a = texelFetch(uFogA, q, 0);
            const vec4 b = texelFetch(uFogB, q, 0);
            if (dist <= b.x)
            {
                acc += wb * vec4(0.0, 0.0, 0.0, 1.0);
                wsum += wb;
                continue;
            }
            const bool open = b.z < 0.0;
            const float e = abs(b.z);
            float wd = 1.0;
            if (!(open && dist >= e))
                wd = exp(-abs(dist - e) / (0.03 * min(dist, PF_FAR) + 0.05));
            const float w = wb * max(wd, 1e-4);
            acc += w * a;
            wsum += w;
            if (b.y < PF_FAR) { meanAcc += w * b.y; meanW += w; }
        }
    acc /= max(wsum, 1e-6);
    if (acc.a >= 0.999 && dot(acc.rgb, vec3(1.0)) <= 1e-5) discard;
    const float Tb = clamp(acc.a, 0.0, 1.0);
    vec3 Lb = max(acc.rgb, vec3(0.0));
    const float m = meanW > 0.0 ? meanAcc / meanW : min(dist, 64.0);
    const vec4 wv = pf.inv_vp * vec4(ndc, 0.5, 1.0);
    const vec3 V = normalize(wv.xyz / wv.w);
    const vec3 rel = V * m;
    // LL's haze over [eye, m]
    if (atmosEnabled())
    {
        vec3 additive, atten;
        atmosScatterHaze(rel, additive, atten);
        Lb = Lb * atten.r + (1.0 - Tb) * atmosSrgbToLinear(additive * 2.0) * atmosHdrScale();
    }
    // the froxel fog over [eye, m]
    vec4 fg = vec4(0.0, 0.0, 0.0, 1.0);
    if (volOn()) fg = volFogAt(pf.cam.xyz + rel);
    outColor = vec4(atmosClampHDR(fg.a * Lb + fg.rgb * (1.0 - Tb)), Tb);
}
