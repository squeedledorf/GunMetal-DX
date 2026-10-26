#version 450
#extension GL_EXT_nonuniform_qualifier : require
// vkclean — sun/moon shadow TRANSLUCENCY layer (#212). Alpha-blend casters are drawn
// twice per cascade into a half-resolution colour + depth pair:
//   (default)     depth only: the NEAREST translucent caster along each light texel
//                 (receivers in front of it are not tinted);
//   TRANS_COLOR   multiplicative blend (dst *= src) of every caster's transmittance
//                 (clean_translucent.glsl), no depth test. Cleared to white.
// Opaque and alpha-mask casters stay in the depth cascade, and so do the alpha-blend
// texels LL's shadow pass makes solid (clean_shadow_mask.frag BLEND_SOLID, alpha 0.88
// and up) -- those block the light at full shadow-map resolution; this layer tints
// with the rest. Inputs match
// clean_draw.vert / clean_draw_rigged.vert (as clean_shadow_mask.frag).
#include "clean_translucent.glsl"
#include "clean_alpha_rule.glsl"   // (#1044: ALPHA_TRANS_MIN / ALPHA_TRANS_FRONT)
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

#ifdef TRANS_COLOR
layout(location = 0) out vec4 outT;
#endif

void main()
{
    // Single-sided casters drop their back faces, as the opaque shadow pass does.
    if (!gl_FrontFacing && (v_flags & 256u) == 0u) discard;
    Material m = materials[v_material_id];
    vec4 base = m.base_color_factor * v_tint;
    if ((m.flags & 1u) != 0u)
        base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv);
    // <SS:Skoomastream> LL-solid texels and mask materials cast in the depth cascade.
    if (base.a >= LL_BLEND_SHADOW_SOLID || m.alpha_cutoff >= 0.0) discard;
#ifdef TRANS_COLOR
    if (base.a < ALPHA_TRANS_MIN) discard;
    outT = vec4(translucentTransmit(base.rgb, base.a), 1.0);
#else
    // Nearly invisible texels don't count as "the glass" for the in-front test.
    if (base.a < ALPHA_TRANS_FRONT) discard;
#endif
}
