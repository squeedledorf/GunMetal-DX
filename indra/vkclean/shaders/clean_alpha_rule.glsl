// vkclean -- THE ALPHA-TEST RULE (#1044): when is a texel of a surface SOLID, for every path that asks --
// the lit FS and its pre-passes (clean_draw.frag, clean_prepass_*.frag: maskTestAlpha), the shadow-map
// casters (clean_shadow_mask.frag, clean_shadow_trans.frag, clean_particle_shadow.frag), the pick
// (clean_pick.frag) and every ray-traced candidate (clean_rt_common.glsl rtCandidateAlphaCol, the walks in
// clean_vis_walk.glsl, the closest-hit / world-blocker / blend-layer walks). Before, the rule existed in
// five forms that disagreed: the cascade casters cut every mask at max(cutoff, 0.05) -- a mask material
// with a cutoff below 0.05 (0 included) was solid on screen and to the RT shadow rays but cut out of the
// sun's shadow map.
// No dependencies (constants + pure functions): included by clean_frame_info.glsl and clean_rt_common.glsl,
// and by the shadow-pass FS directly.
//
// THE RULE: alpha a = base_color_factor.a x tint.a x the base texel's alpha (untextured: the factor
// alone). A MASK material (alpha_cutoff >= 0) is solid where alphaCoverage(a, lod, k) >= alpha_cutoff;
// an ALPHA-BLEND material (alpha_cutoff < 0) asked a binary question is solid at ALPHA_BLEND_CUTOFF.
// The deliberate per-consumer parameters (the only differences left):
//   * COVERAGE SCALE k (#648, RenderVKAlphaMipCoverage): the raster's tests (lit FS, pre-passes, pick) and
//     the reflection rays' mask test (rtrMaskCone) scale the TESTED alpha by 1 + lod x k at their own
//     sampled mip -- it keeps a mask's on-screen density across mips. Shadow casters (cascades, RT
//     shadow rays) pass k = 0: they test the texel's own coverage at their footprint's LOD.
//   * LOD: each path samples at its own footprint (screen derivatives + the TAA bias, the shadow-map
//     texel, the ray cone / receiver footprint); the rule is the same, the texel is the one its pixel sees.
//   * BLEND CASTERS in shadows are not binary: translucent (1 - alpha, or the colour transmittance
//     #212), with LL's shadow-pass parity (RenderVKRTBlendShadowParity) cutting below ALPHA_BLEND_NONE and
//     going solid at ALPHA_BLEND_SOLID; the cascades keep a binary depth layer at ALPHA_BLEND_CUTOFF plus
//     the translucency layer (ALPHA_TRANS_MIN / ALPHA_TRANS_FRONT).
//   * VISIBILITY questions (the acoustics / weather world blockers, the reflection blend layers, the
//     probe RT blend pass): a blend texel counts once anything shows -- above ALPHA_VISIBLE_EPS.
//   * The alpha-blend DEPTH pre-pass (clean_prepass_blend.frag) writes depth only where a >= 0.99 and the
//     unscaled cutoff passes -- a strict subset of what the lit FS keeps (scaling only raises the alpha).
#ifndef CLEAN_ALPHA_RULE_GLSL
#define CLEAN_ALPHA_RULE_GLSL

#define ALPHA_BLEND_CUTOFF 0.5            // an alpha-blend material asked a binary question
#define ALPHA_BLEND_SOLID  0.88           // LL shadow parity: a blend caster this opaque is solid
#define ALPHA_BLEND_NONE   0.05           // LL shadow parity: a fainter blend caster casts nothing
#define ALPHA_TRANS_MIN    0.004          // a translucent layer this faint transmits everything
#define ALPHA_TRANS_FRONT  0.02           // the translucency layer's "this is the glass" depth test
#define ALPHA_VISIBLE_EPS  (1.0 / 255.0)  // "anything shows": the visibility questions

// The cutoff a material's texel is tested against (blend: the binary question's).
float alphaRuleCutoff(float materialCutoff) { return materialCutoff >= 0.0 ? materialCutoff : ALPHA_BLEND_CUTOFF; }
// The TESTED alpha (#648 maskTestAlpha; Golus 2017, "Anti-aliased alpha test"): a scaled by 1 + lod x k
// at the sampled level lod -- only the test, never the blended / tinting alpha. k = 0: a itself.
float alphaCoverage(float a, float lod, float k) { return k > 0.0 ? a * (1.0 + max(lod, 0.0) * k) : a; }
// THE TEST: solid at this texel for a consumer with coverage scale k.
bool alphaKeeps(float a, float cutoff, float lod, float k) { return alphaCoverage(a, lod, k) >= cutoff; }

#endif
