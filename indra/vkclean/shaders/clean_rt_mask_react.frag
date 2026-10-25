#version 460
// vkclean — RT SHADOW SHADING-CHANGE -> TAA / FSR REACTIVE (RenderVKRTShadowReactive).
// A fullscreen triangle inside render pass 2b (after mReact's clear), drawn only when
// the resolve reads the coverage mask (RenderVKReactiveMask) and the RT shadow mask ran.
// A moving caster's shadow sweeping a STATIC receiver is a shading change with a zero
// motion vector: the resolve's history is valid by every geometric test, and under a
// bright sun its colour box can span lit and shadowed luma at the edge, so the old
// shadow survives in the TAA / FSR history after the mask itself has snapped. The
// temporal pass (clean_rt_mask_temporal.comp) writes, per mask texel, how far the
// shadow moved this frame where its moving-caster change detector fired (never for
// noise or jitter), gain applied, into the final sun mask's alpha; this pass upsamples
// it (max of the 2x2 texels around the fragment, so the band edges stay covered) into
// mReact.R, MAX-blended with the alpha / particle coverage already there. FSR 3.1 takes
// it through its reactive input, our TAA through its reactive weight.
// Attachments 0 / 1 (HDR, glow) get an additive zero (write-masked off for glow).
#include "clean_frame_info.glsl"

layout(set = 0, binding = 31) uniform sampler2D uRtMaskSun;   // a = shading change

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outReact;

void main()
{
    outColor = vec4(0.0);
    outGlow = vec4(0.0);
    if (ssr.rt_mask.x < 0.5) discard;   // pass 1 did not read the mask this frame
    const float sc = ssr.rt_mask.y;
    const ivec2 mx = ivec2(ssr.rt_mask.zw) - 1;
    // The mask texel m sampled render px (m + 0.5 + j) / scale (clean_rt_mask_common.glsl
    // rtMaskSamplePx; j = the TAA jitter below full resolution): invert for the 2x2.
    const vec2 j = (ssr.taa_params.w > 0.5 && sc < 0.999) ? ssr.taa_jitter.xy : vec2(0.0);
    const vec2 f = gl_FragCoord.xy * min(sc, 1.0) - 0.5 - j;
    const ivec2 b = ivec2(floor(f));
    float r = 0.0;
    for (int k = 0; k < 4; ++k)
        r = max(r, texelFetch(uRtMaskSun, clamp(b + ivec2(k & 1, k >> 1), ivec2(0), mx), 0).a);
    if (r <= 1.0 / 255.0) discard;
    outReact = vec4(r, 0.0, 0.0, 0.0);
}
