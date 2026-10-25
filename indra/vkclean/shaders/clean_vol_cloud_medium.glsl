// vkclean -- VOLUMETRIC FOG (#718) <-> SKY CLOUD LAYER (#719 / #720) interface: CLOUD MEDIUM
// INJECTION HOOK.
//
// volCloudLayerMedium(P, sigma_s, sigma_t): adds the sky cloud layer's medium at world
// position P to a froxel cell's scattering (rgb, /m) and extinction (/m) coefficients. Called
// by the density injection (clean_vol_inject.comp) for every cell, after the height fog and
// the local volumes, so a camera INSIDE (or just below / above) the cloud layer sees the
// cloud as froxel fog: lit by the same sun / shadow / ambient / local-light pass, filtered by
// the same temporal pass and integrated with everything else -- no second near-field
// renderer, and no double counting as long as the cloud renderer leaves out the segment
// [0, froxel far] of the view ray whenever it fills this hook (the froxel far plane is
// ssr.vol_grid.w, view depth in metres: clean_vol_common.glsl volFar()).
//
// HOOK ONLY (#718): no cloud medium is added. #719 fills the BODY (keep the signature).
// Included by clean_vol_inject.comp alone (same pipeline layout as the light pass, so the
// binding rules of clean_vol_cloud_shadow.glsl apply: set 2 = VOL_SET bindings >= 5, or draw
// set 0; explicit-LOD reads only), and cheap: it runs for every cell every frame. Leave
// sigma_s / sigma_t untouched where P is outside the layer.
#ifndef CLEAN_VOL_CLOUD_MEDIUM_GLSL
#define CLEAN_VOL_CLOUD_MEDIUM_GLSL

void volCloudLayerMedium(vec3 P, inout vec3 sigma_s, inout float sigma_t)
{
}

#endif
