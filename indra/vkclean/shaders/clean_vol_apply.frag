#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean -- VOLUMETRIC FOG (#718): the OPAQUE + SKY APPLY.
//
// A fullscreen triangle, the FIRST draw of render pass 2 (after forward pass 1 / the deferred
// lighting + sky, before any transparent, the refraction copy and the water), blending
//   dst' = src.rgb + dst.rgb x src.a   (src = (L, T): customBlend ONE / SRC_ALPHA, the
//                                       scene's alpha preserved)
// i.e. C' = C_LLhazed x T + L (clean_vol_apply.glsl) at every pixel, with P rebuilt from the
// opaque depth (draw set 0 binding 40, pass 1's final depth, DEPTH_READ_ONLY in pass 2).
// Sky pixels (reversed-Z depth 0) take the fog up to the draw distance along their ray
// (vol_misc.y): the extra fog's horizon then meets the far terrain's, no seam. The glow
// target and the TAA reactive mask are write-masked off (fog dims glow only in the
// tonemap's bloom, which sees the fogged scene: noted P1 gap).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_glow;    // (write-masked)
layout(location = 2) out vec4 o_react;   // (write-masked)

#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"
#include "clean_vol_apply.glsl"

layout(set = 0, binding = 40) uniform sampler2D uSceneDepth;

layout(push_constant) uniform VolApplyPush {
    vec4 dims;     // render w, h, 1 / w, 1 / h
    vec4 jitter;   // xy this frame's projection jitter (NDC; 0 without TAA)
} ap;

void main()
{
    o_glow = vec4(0.0);
    o_react = vec4(0.0);
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const float d = texelFetch(uSceneDepth, p, 0).r;
    const vec2 ndc = (vec2(p) + 0.5) * ap.dims.zw * 2.0 - 1.0 - ap.jitter.xy;
    vec3 rel;
    if (d > 0.0)
    {
        const vec4 w = ssr.vol_inv_vp * vec4(ndc, d, 1.0);
        rel = w.xyz / w.w;
    }
    else
    {
        const vec4 w = ssr.vol_inv_vp * vec4(ndc, 0.5, 1.0);
        rel = normalize(w.xyz / w.w) * ssr.vol_misc.y;
    }
    const vec4 f = volOn() ? volFogAt(ssr.vol_eye.xyz + rel) : vec4(0.0, 0.0, 0.0, 1.0);
    o_color = vec4(f.rgb, clamp(f.a, 0.0, 1.0));
}
