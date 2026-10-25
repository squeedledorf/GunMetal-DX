#version 450
// vkclean -- GOD RAYS (#208): depth-aware UPSAMPLE + additive COMPOSITE of the
// reduced-resolution in-scatter (clean_godrays.comp) into the linear HDR scene.
//
// Drawn as a fullscreen triangle blending ONE + ONE with alpha 0 (the scene's alpha is
// untouched), write-masked off the glow and the reactive / water mask (never reactive:
// the dither accumulates). Default (RenderVKGodRaysAfterAlpha): the LAST draw of pass 2,
// over water, post-water alpha and particles, so no transparent covers a shaft in front
// of it (RenderVKReactiveMask off + TAA: right after the water, before TAA's reactive
// reference copy). Off: in pass 2a, before the refraction copy (water and post-water
// alpha then layer over it). Depth-aware against the opaque depth either way. The
// input is the temporally accumulated in-scatter
// (clean_godrays_temporal.comp; RenderVKGodRaysTemporal) or, without it / in snapshots,
// the raw march.
//
// Upsample: the 3x3 in-scatter texels around this pixel, weighted by distance in the
// low-res grid (a soft tent) and by view-distance similarity to this pixel (alpha of
// each texel is the view distance it was marched to), so shafts never bleed across
// silhouettes and the interleaved-gradient dither averages out spatially.
//
// DENOISER (#657, RenderVKGodRaysDenoise): the input is the march target the god rays'
// a-trous pass finalized (clean_godrays_atrous.comp), same layout. REACTIVE
// (RenderVKGodRaysReactive): where the denoiser's moving-caster change detector fired
// (a walking avatar's shadow volume crossing a shaft), binding 4's alpha holds the shaft's
// shading change; the max over the same-depth taps goes to mReact.R (MAX-blended, the
// write mask's only channel) so TAA / FSR do not drag the old shaft behind the mover --
// the RT shadow mask's clean_rt_mask_react.frag, for the in-scatter. jitter.z = 0 (old
// path, no reactive mask, the 2a site): an exact 0, a no-op under MAX.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(location = 2) out vec4 o_react;   // mReact (R only; MAX blend)

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // world depth (texelFetch)
layout(set = 0, binding = 2) uniform sampler2D uInscatter;
layout(set = 0, binding = 4) uniform sampler2D uReact;   // a = reactive (denoiser; else a placeholder)

layout(push_constant) uniform GodRaysCompPush {
    mat4 inv_view_proj;   // UNJITTERED VK clip -> camera-relative world
    vec4 dims;            // render w, h, in-scatter w, h
    vec4 jitter;          // xy this frame's projection jitter in NDC (0 without TAA),
                          // z 1 = write the denoiser's reactive (header)
} cp;

const float kSkyDist = 65000.0;

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const float d = texelFetch(uDepth, p, 0).r;
    float dist = kSkyDist;
    if (d > 0.0)   // reversed-Z: sky = 0
    {
        const vec2 ndc = (vec2(p) + 0.5) / cp.dims.xy * 2.0 - 1.0 - cp.jitter.xy;
        const vec4 w = cp.inv_view_proj * vec4(ndc, d, 1.0);
        dist = length(w.xyz / w.w);
    }

    const vec2 lp = (vec2(p) + 0.5) * (cp.dims.zw / cp.dims.xy);   // low-res texel units
    const ivec2 c = ivec2(floor(lp));
    const ivec2 mx = ivec2(cp.dims.zw) - 1;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    const bool react_on = cp.jitter.z > 0.5;
    float react = 0.0;
    vec3 nearest = vec3(0.0);
    float nearest_err = 1e30;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 q = clamp(c + ivec2(x, y), ivec2(0), mx);
            const vec4 s = texelFetch(uInscatter, q, 0);
            const vec2 dd = vec2(q) + 0.5 - lp;
            const float ws = exp(-dot(dd, dd) * 0.78);   // gaussian, sigma ~0.8 texel
            const float err = abs(s.a - dist) / max(min(s.a, dist), 0.1);   // relative depth gap
            const float wd = exp(-err * 24.0);
            sum += s.rgb * ws * wd;
            wsum += ws * wd;
            if (err < nearest_err) { nearest_err = err; nearest = s.rgb; }
            if (react_on && wd > 0.3) react = max(react, texelFetch(uReact, q, 0).a);
        }
    // No texel on this surface (thin geometry under a coarse grid): the closest depth.
    const vec3 L = wsum > 1e-4 ? sum / wsum : nearest;
    o_color = vec4(L, 0.0);
    o_react = vec4(react_on ? clamp(react, 0.0, 1.0) : 0.0, 0.0, 0.0, 0.0);
}
