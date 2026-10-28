#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — DEPTH + NORMAL PRE-PASS (RenderVKDepthPrepass with ambient occlusion on).
// Same draws / VS / depth state as the depth-only pre-pass, plus the shading normal
// the lit FS will use (vertex normal oriented by gl_FrontFacing, normal map through
// the same TBN), written view-space and octahedral-encoded to the RG16F normal
// target that clean_ao.comp reads (LL's SSAO reads the G-buffer normal). RGBA16F (#648):
// .xy that shading normal, .zw the smooth interpolated vertex normal (unperturbed).
//
// PREPASS_MASK: the mask buckets; the discards are clean_prepass_mask.frag's,
// verbatim (pass 1 depth-tests EQUAL, so the surviving fragment set must match the
// lit FS exactly). Without it (opaque buckets) nothing is discarded, exactly like
// the FS-less depth-only opaque pre-pass.
//
// PREPASS_MOTION (temporal #84, RenderVKTAA / RenderVKDebugMotionVectors): also writes
// the RG16F motion-vector target (colour attachment 1). CONVENTION: motion is in UV
// units, current minus previous: mv = (cur_ndc.xy - prev_ndc.xy) * 0.5, so the pixel's
// position last frame is uv - mv (Vulkan NDC y is down, like UV). Both positions go
// through the UNJITTERED matrices (ssr.views[CLEAN_VIEW].taa_cur_vp / taa_prev_vp), so jitter never
// shows up as motion. Projecting the interpolated world positions per pixel keeps it
// perspective-correct; static geometry under a static camera is exactly 0. Rigged
// geometry's previous position is re-skinned with last frame's palettes (stage 2,
// clean_preskin.comp), worn attachments ride last frame's joint table; either with no
// history (new avatar, reset) arrives with v_prev_world_pos == v_world_pos (camera-only).
// Pixels no geometry covers (sky) keep the clear value 0: the TAA stage derives their
// camera motion from depth == 1.

#include "clean_draw_common.glsl"
#include "clean_normal_tex.glsl"   // BC5 normal maps: z rebuilt (#263)
#ifdef PREPASS_MASK
// <GMDX> rez effects: the de-rez cut of a dying object, as the lit FS runs it
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"
#endif

#ifdef PREPASS_MOTION
layout(location = 10) in vec3 v_prev_world_pos;
layout(location = 1) out vec2 outMotion;
// NO-HISTORY MASK (#605, R8, colour attachment 2; plain write, so the nearest surface
// wins like the depth): 1 where this pixel's surface is a live media (or, by setting,
// animated-texture) face -- its content changes with zero motion, so clean_taa.comp /
// clean_fsr_inputs.comp take this frame only there. 0 everywhere else.
layout(location = 2) out float outNoTemporal;
#endif

vec2 mapUV(int slot)
{
    if ((v_flags & 1024u) != 0u)   // <GMDX> glTF slots: own transform after the animation
        return matMapAnimUV(v_material_id, slot, v_uv, (materials[v_material_id].flags & 64u) != 0u);
    return matMapUV(v_material_id, slot, v_uv_raw, v_uv);
}

vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

void main()
{
    Material m = materials[v_material_id];
#ifdef PREPASS_MASK
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u && !rezfxBack(v_flags)) discard;   // single-sided back face (<GMDX> but the printer's cut surface)
    if ((v_flags & 32u) == 0u)                                         // terrain: alpha 1
    {
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

    // clean_draw.frag shade(): N, then the normal map (PBR and legacy alike; terrain
    // uses the normal slot for a detail texture).
    vec3 N = normalize(v_world_normal);
    if (!gl_FrontFacing) N = -N;
    vec3 bump_tn;
    if ((v_flags & 32u) == 0u && legacyBumpCode(m.flags) != 0u)
    {
        // legacy bump preset (#19): clean_draw.frag's legacy branch
        if (legacyBumpTN(m, v_uv, taaLodBias(v_flags), bump_tn))
            N = applyNormalMap(normalize(v_world_normal), v_world_tangent, v_world_pos, v_uv, bump_tn, gl_FrontFacing);
    }
    else if ((v_flags & 32u) == 0u && (m.flags & 2u) != 0u)
    {
        vec3 tn = cleanNormalTex(texture(uTextures[nonuniformEXT(m.normal_handle.x)], mapUV(MAP_NORMAL), taaLodBias(v_flags)).xyz);
        N = applyNormalMap(normalize(v_world_normal), v_world_tangent, v_world_pos,
                           ((v_flags & 1024u) != 0u) ? mapUV(MAP_NORMAL) : v_uv, tn, gl_FrontFacing);   // <GMDX> animated: no vertex tangent, frame from the normal map's UV
    }
    vec3 nv = normalize(mat3(ssr.views[CLEAN_VIEW].cur_view) * N);
    // .zw (#648): the smooth interpolated vertex normal, unperturbed (oriented as N), for
    // the AO kernel / blur -- smooth across triangle edges, no normal-map tilt.
    vec3 N0 = normalize(v_world_normal);
    if (!gl_FrontFacing) N0 = -N0;
    vec3 nv0 = normalize(mat3(ssr.views[CLEAN_VIEW].cur_view) * N0);
    outColor = vec4(octEncode(nv), octEncode(nv0));
#ifdef PREPASS_MOTION
    // Camera-relative (#555): both matrices map (P - cam_origin).
    const vec4 cur  = ssr.views[CLEAN_VIEW].taa_cur_vp  * vec4(v_world_pos - ssr.cam_origin.xyz, 1.0);
    const vec4 prev = ssr.views[CLEAN_VIEW].taa_prev_vp * vec4(v_prev_world_pos - ssr.cam_origin.xyz, 1.0);
    // Behind last frame's eye (w <= 0: no valid projection) -> 0; TAA rejects it anyway.
    outMotion = prev.w > 1e-6 ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(0.0);
    outNoTemporal = noTemporalFace(v_flags) ? 1.0 : 0.0;
#endif
}
