// vkclean -- VOLUMETRIC FOG (#718) <-> SKY CLOUD LAYER (#719) interface: CLOUD SHADOW.
//
// cloudShadow(worldPos): the fraction [0, 1] of the sun / moon beam that reaches worldPos
// through the sky cloud layer (1 = no cloud above). The froxel light pass
// (clean_vol_light.comp) multiplies the sun / moon in-scattering of every cell -- the extra
// fog medium AND the haze's sun scatter -- by it, so cloud shadows sweep through the fog the
// way they sweep over the ground.
//
// Wired to #719's cloud shadow map (clean_cloud_shadow_map.glsl, draw set 0 binding 46: the
// map while the volumetric layer is live, else a 1x1 white stand-in; it returns 1.0 when the
// layer or its shadow is off). The fog's own cascade tap (sunVisibility) does not include it,
// so the cloud term is counted once.
//
// WHERE IT RUNS: compute only, included by clean_vol_light.comp ALONE (not by the consumers:
// the world / water / particle FS never see it). That pipeline's layout is
//   {set 0 = draw set 0, set 1 = draw set 1 (bindless), set 2 = VOL_SET (own; bindings 0-4 taken)}
// (clean_vol_pass.glsl). A new texture / buffer goes at `set = VOL_SET, binding >= 5` and must
// be added to the own set layout in LLCleanVolFogVK::ensurePipelines + its writeSets (or into
// draw set 0 if the world shares it); per-frame matrices / parameters go in their own SSRBuf
// block appended after vol_misc (clean_frame_info.glsl; the VKSSRInfo size / offset asserts move
// with it). Explicit-LOD reads only (no derivatives in compute), and cheap: it runs once per
// froxel cell per frame (~0.9 M cells at the default tier) where the sun term is evaluated.
#ifndef CLEAN_VOL_CLOUD_SHADOW_GLSL
#define CLEAN_VOL_CLOUD_SHADOW_GLSL

#include "clean_cloud_shadow_map.glsl"

float cloudShadow(vec3 worldPos)
{
    return cloudShadowMap(worldPos);
}

#endif
