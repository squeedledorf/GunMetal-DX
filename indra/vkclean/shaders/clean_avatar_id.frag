#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — AVATAR ID PASS (#549, llcelshadevk.cpp): writes the avatar slot + 1 of the
// visible surface into the R16_UINT avatar-id target (0 = no avatar, the clear value).
//
// Drawn after pass 1 against the finished opaque depth (read-only), with the SAME VS
// modules as the lit passes (invariant gl_Position):
//   * opaque / mask draws (rigged buckets 3 4 8 9 + the worn rigid attachments compacted
//     out of the static buckets by clean_cel_filter.comp): depth EQUAL, no alpha test --
//     a cut-out texel's pixel holds what is behind it, so EQUAL already fails there.
//   * ID_ALPHA (rigged alpha bucket 5 + post-water list, alpha attachments): the
//     alpha-blended hair / sheer layers, depth LESS_OR_EQUAL (in front of the opaque
//     scene), no depth write, kept where the texel is at least half opaque, so outlines
//     and FX masks follow the hair instead of stopping at the hairline.
// The VS resolved the slot (clean_cel_common.glsl celVertexWord): v_cel bits 0..15.
#include "clean_frame_info.glsl"   // taaLodBias (the lit FS's TAA mip bias)
layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;
layout(location = 11) flat in uint v_cel;

layout(location = 0) out uint outId;

#ifdef ID_ALPHA
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
const float kIdAlpha = 0.5;
#endif

void main()
{
    const uint id = v_cel & 0xFFFFu;
    if (id == 0u) discard;
#ifdef ID_ALPHA
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;     // single-sided back face
    Material m = materials[v_material_id];
    vec4 base = m.base_color_factor * v_tint;
    if ((m.flags & 1u) != 0u)
        base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));
    if (base.a < kIdAlpha) discard;
#endif
    outId = id;
}
