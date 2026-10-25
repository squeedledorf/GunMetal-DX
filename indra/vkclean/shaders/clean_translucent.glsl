// vkclean — light transmittance through a translucent (alpha-blend) caster (#212).
// Shared by the shadow-map translucency pass (clean_shadow_trans.frag) and the
// ray-traced shadow rays (clean_rt_common.glsl), so both paths tint alike.
//   col = the caster's linear base colour (factor x tint x texel), a = its alpha.
// More alpha blocks more light: (1 - a) passes. What passes takes the caster's hue
// (colour normalised to its brightest channel, so dark glass doesn't double-darken),
// fading in over the first half of the alpha range: a faint film barely tints,
// half-opaque stained glass throws its full colour.
vec3 translucentTransmit(vec3 col, float a)
{
    a = clamp(a, 0.0, 1.0);
    const float m = max(max(col.r, col.g), col.b);
    const vec3 hue = m > 1e-4 ? col / m : vec3(1.0);
    return (1.0 - a) * mix(vec3(1.0), hue, smoothstep(0.0, 0.5, a));
}

// <SS:Skoomastream> LL's SHADOW PASS for an alpha-blend caster texel (Firestorm parity).
// LL draws PASS_ALPHA faces into the shadow map with shadowAlphaMaskF.glsl at
// minimum_alpha = ALPHA_BLEND_CUTOFF 0.598 (pipeline.cpp renderAlphaObjects): the
// TEXTURE alpha below 0.598 casts nothing, then texture x vertex alpha below 0.05 casts
// nothing, below 0.88 is stippled (half the texels: a 50% shadow), from 0.88 solid.
// glTF blend (pbrShadowAlphaBlendF.glsl) skips the 0.598 texture cut.
//   tex_a = the base-colour texel's alpha alone, a = the caster's full alpha.
// Returns the light LL passes: 1 nothing cast, 0.5 stippled, 0 solid.
const float LL_BLEND_SHADOW_SOLID = 0.88;
float llBlendShadowPass(float tex_a, float a, bool gltf)
{
    if ((!gltf && tex_a < 0.598) || a < 0.05) return 1.0;
    return a < LL_BLEND_SHADOW_SOLID ? 0.5 : 0.0;
}
