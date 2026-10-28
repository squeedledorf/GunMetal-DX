#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — DEPTH + MOTION PRE-PASS, no normals (RenderVKPrepassDepthOnlyDeferred, temporal
// frames that go DEFERRED): the G-buffer pass writes the normals the AO reads (G1), so pass
// 1a carries only the motion target (attachment 0, RG16F) and the no-history mask
// (attachment 1, R8). The motion / no-history values and the mask discards are
// clean_prepass_normal.frag's PREPASS_MOTION / PREPASS_MASK, verbatim (same convention:
// UV units, current minus previous, unjittered matrices, camera-relative; the EQUAL passes
// after it need the identical surviving fragment set).
//
// PREPASS_MASK: the mask buckets (and every cluster draw); without it the opaque buckets
// (nothing discarded; still an FS: the motion output).
//
// Inputs: exactly the slim motion VS's outputs (clean_draw{,_rigged}.vert / cluster stages
// -DCLEAN_DEPTH_ONLY -DCLEAN_DEPTH_MOTION): 0 material, 1 uv, 3 tint, 4 flags, 5 world
// position, 10 previous world position. The full VS writes a superset (RenderVKDepthOnlyVS 0).
#include "clean_frame_info.glsl"   // ssr: taa matrices, cam_origin, noTemporalFace, Highlight Transparent
layout(location = 0)  flat in uint v_material_id;
layout(location = 1)       in vec2 v_uv;
layout(location = 3)  flat in vec4 v_tint;
layout(location = 4)  flat in uint v_flags;
layout(location = 5)       in vec3 v_world_pos;
layout(location = 10)      in vec3 v_prev_world_pos;

layout(location = 0) out vec2 outMotion;
layout(location = 1) out float outNoTemporal;

#ifdef PREPASS_MASK
// 96 bytes — matches LLCleanMaterialStore::Material (see clean_draw_common.glsl).
struct Material {
    uvec2 base_color_handle;
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
// <GMDX> rez effects: the de-rez cut of a dying object, as the lit FS runs it
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"
#endif

void main()
{
#ifdef PREPASS_MASK
    // clean_prepass_mask.frag's discards (= the lit FS's), verbatim.
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u && !rezfxBack(v_flags)) discard;   // single-sided back face (<GMDX> but the printer's cut surface)
    if ((v_flags & 32u) == 0u)                                         // terrain: alpha 1
    {
        const Material m = materials[v_material_id];
        precise vec4 base = m.base_color_factor * v_tint;   // same evaluation order as the lit FS
        float a_lod = 0.0;
        if ((m.flags & 1u) != 0u)
        {
            base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));   // + the lit FS's TAA mip bias
            a_lod = textureQueryLod(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv).y + taaLodBias(v_flags);
        }
        rezfxDiscard(v_flags);   // <GMDX> the de-rez cut (= the lit FS's), after the implicit-LOD fetch
        if (ssr.ao_params2.z <= 0.5 && m.alpha_cutoff >= 0.0 && !rezfxRerouted(v_flags) && maskTestAlpha(base.a, a_lod, m.alpha_cutoff) < m.alpha_cutoff)   // Highlight Transparent keeps cutouts; <GMDX> rerouted opaque rows never
            discard;
    }
#endif
    // Camera-relative (#555): both matrices map (P - cam_origin).
    const vec4 cur  = ssr.views[CLEAN_VIEW].taa_cur_vp  * vec4(v_world_pos - ssr.cam_origin.xyz, 1.0);
    const vec4 prev = ssr.views[CLEAN_VIEW].taa_prev_vp * vec4(v_prev_world_pos - ssr.cam_origin.xyz, 1.0);
    // Behind last frame's eye (w <= 0: no valid projection) -> 0; TAA rejects it anyway.
    outMotion = prev.w > 1e-6 ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(0.0);
    outNoTemporal = noTemporalFace(v_flags) ? 1.0 : 0.0;
}
