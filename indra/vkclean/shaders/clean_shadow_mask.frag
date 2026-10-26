#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — shadow-map alpha test (LL shadowAlphaMaskF). Depth-only pass for the
// mask / alpha-blend casters: masks discard below the material cutoff -- THE alpha-test rule
// (#1044, clean_alpha_rule.glsl); alpha-blend casters follow LL's shadow pass
// (clean_translucent.glsl llBlendShadowPass). BLEND_SOLID (translucent shadows on, #212):
// alpha-blend casters keep only the texels LL makes solid; the translucency layer
// (clean_shadow_trans.frag) tints with the rest. Inputs match clean_draw.vert / clean_draw_rigged.vert.
#include "clean_alpha_rule.glsl"
#include "clean_translucent.glsl"
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
// <GMDX> rez effects: the draw push (pad1 bit 31: the build-effect casters only, below)
layout(push_constant) uniform DrawPush { uint view; uint f8; uint pad0; uint pad1; } g_draw_push;

void main()
{
    // Cull-none pipeline for alpha / double-sided casters: single-sided alpha
    // casters still drop their back faces, as LL's culled shadow pass does.
    if (!gl_FrontFacing && (v_flags & 256u) == 0u) discard;
    // <GMDX> REZ EFFECTS: an opaque rigged row the main view routed to the mask buckets for a build
    // effect (slot bit 31, not dying, natively neither blend nor mask; clean_cull.comp) casts solid,
    // as from its own opaque bucket. With avatar shadow detail 0 the renderer casts the rigged mask
    // buckets for these rows alone (push pad1 bit 31): everything else in them stays out.
    const bool rzBuild = (v_flags & 0x80400006u) == 0x80000000u;
    if ((g_draw_push.pad1 & 0x80000000u) != 0u && !rzBuild) discard;
    if (rzBuild) return;
    Material m = materials[v_material_id];
    float ta = 1.0;
    if ((m.flags & 1u) != 0u)
        ta = texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv).a;
    const float a = m.base_color_factor.a * v_tint.a * ta;
    if (m.alpha_cutoff >= 0.0)
    {
        // THE rule (clean_alpha_rule.glsl): the mask's own cutoff, no coverage scale (a shadow caster
        // tests its texel's own coverage). (#1044: the old max(.., 0.05) floor cut faint mask texels --
        // and every texel of a cutoff-0 mask -- that the raster draws and the RT shadow rays block.)
        if (!alphaKeeps(a, m.alpha_cutoff, 0.0, 0.0)) discard;
        return;
    }
    // <SS:Skoomastream> Alpha-blend caster: LL's shadow pass (Firestorm parity), not
    // a flat 0.5 cut. LL stipples its 50% band with screen-X columns, which alias into
    // stripes at grazing sun angles; a texel checkerboard covers the same half and
    // filters to an even 50%.
    const float pass = llBlendShadowPass(ta, a, (m.flags & 64u) != 0u);
#ifdef BLEND_SOLID
    if (pass > 0.0) discard;
#else
    if (pass >= 1.0) discard;
    if (pass > 0.0 && ((uint(gl_FragCoord.x) ^ uint(gl_FragCoord.y)) & 1u) != 0u) discard;
#endif
}
