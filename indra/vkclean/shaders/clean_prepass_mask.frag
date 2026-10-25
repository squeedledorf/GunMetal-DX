#version 460
#extension GL_EXT_nonuniform_qualifier : require
// vkclean — DEPTH PRE-PASS alpha test (RenderVKDepthPrepass) for the MASK buckets
// (static 1 / 7, rigged 4 / 9). Opaque buckets pre-pass with no FS at all.
//
// Pass 1 then depth-tests EQUAL, so the set of fragments that survive here must be
// EXACTLY the set the lit FS (clean_draw.frag, CLEAN_EARLY_Z variant) keeps for these
// buckets; any mismatch is a hole (pre-pass drops what lit keeps) or a sky-coloured
// cutout (pre-pass keeps what lit drops). So this replicates the lit FS's three
// discards verbatim: water (bit 11), single-sided back face on a cull-none pipeline,
// and the alpha cutoff on base_color_factor * tint * base texel, sampled with the
// same v_uv interpolant and sampler (identical implicit LOD). Terrain (bit 5) is
// alpha 1 in the lit FS and never cut out. Inputs match clean_draw.vert /
// clean_draw_rigged.vert (same modules, invariant gl_Position).
#extension GL_GOOGLE_include_directive : require
#include "clean_frame_info.glsl"   // ssr.ao_params2.z: Highlight Transparent
layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;

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
// <GMDX> rez effects: the de-rez cut of a dying object and a build effect's hidden part, as the
// lit FS runs them (the draw view: probe captures cut nothing)
#include "clean_draw_view.glsl"
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"

void main()
{
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u && !rezfxBack(v_flags)) discard;   // single-sided back face (<GMDX> but the printer's cut surface)
    if ((v_flags & 32u) != 0u) return;                                 // terrain: alpha 1
    Material m = materials[v_material_id];
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
