// vkclean — per-frame info block (set 0, binding 9): SSR reprojection data plus the
// WindLight atmosphere/post parameters (layout: clean_atmos_common.glsl). Byte layout
// = LLCleanRendererVK::VKSSRInfo. Shared so every consumer declares it identically.
// UNIFORM BUFFER (#814; was a readonly std430 SSBO): every read here is dynamically uniform,
// and on NVIDIA a UBO member is a constant-bank operand (c[bank][off] straight into the ALU
// op) where a uniform-address SSBO read is a per-lane global load into a REGISTER -- ~10 of
// them live at once in the atmosphere / shadow setup of the ~128-register lighting kernels.
// std140 == std430 for this block, byte for byte: every member is a vec4, a mat4 or an array
// of them (base alignment 16, array / matrix-column stride 16 under both rules), so no
// offset moves (C++: the static_asserts at VKSSRInfo). 4.8 KB << maxUniformBufferRange
// (>= 16 KB by the spec, 64 KB on NVIDIA). Descriptor type UNIFORM_BUFFER in draw set 0 and
// the AO set (LLCleanRendererVK writeSSRDescriptors / writeAoSet).
// LAYERED STEREO (#1075; research/VR_LAYERED_STEREO_DESIGN.md §4.1): the PER-VIEW part of the
// block, one SSRView per view at the end (C++ VKSSRView, VKSSRInfo::views). Readers index it by
// their view: ssr.views[CLEAN_VIEW] (clean_view.glsl: the multiview raster stages' gl_ViewIndex,
// folded to 0 on single-view pipelines; 0 in compute until a kernel says otherwise). Every
// matrix maps camera-relative positions (P - cam_origin): ONE origin for all views (the head in
// VR), so cam_origin and shadow_mat stay shared. The desktop fills views[0] only.
#include "clean_view.glsl"
struct SSRView {
    mat4 prev_view;       // SSR: last frame's world -> view (camera-relative), projection, inverse
    mat4 prev_proj;
    mat4 prev_inv_proj;
    mat4 cur_view;        // current world -> view (the depth+normal pre-pass writes view-space normals)
    mat4 taa_cur_vp;      // TEMPORAL: UNJITTERED (P - cam_origin) -> clip of this frame / the previous one
    mat4 taa_prev_vp;
};                        // 384 B
layout(std140, set = 0, binding = 9) uniform SSRBuf {
    vec4 params0;   // iterations, ray step, distance bias, depth reject bias
    vec4 params1;   // adaptive step multiplier, enabled
    vec4 atmos[16]; // atmosphere + post (clean_atmos_common.glsl)
    // Water surface (clean_water.frag):
    //  [0] wave1 dir.xy, wave2 dir.xy      [1] normal scale.xyz, normal-map blend
    //  [2] fresnel scale, fresnel offset, roughness (blur*2), refraction scale
    //  [3] specular colour.rgb, water height
    //  [4] fog colour.rgb (sRGB), fog density     [5] fog ks, normal map A idx, B idx, cam under water
    vec4 water[6];
    // Sun/moon shadow cascades (clean_draw_common.glsl sunShadow): CAMERA-RELATIVE world
    // (P - cam_origin, #555; shadowSpace) -> shadow NDC (xy -> uv, z = [0,1] depth),
    // cascade far splits as view depths, params
    // (x sample bias, y light-dir offset, z map resolution, w 0 off / 1 cascades /
    // 2 ray traced -- ray-query variants trace, the rest keep the cascades) and each
    // cascade's world-space texel size (normal offset).
    mat4 shadow_mat[4];
    vec4 shadow_clip;
    vec4 shadow_params;
    vec4 shadow_texel;
    // Planar MIRROR (hero reflection probe) of this frame: world -> unit box of the
    // mirror probe volume ([-1,1]^3), the mirror plane (n.xyz, d; viewer side > 0),
    // params (x valid, y 1/main width, z 1/main height).
    mat4 mirror_box;
    vec4 mirror_plane;
    vec4 mirror_params;
    // Ambient occlusion (clean_draw.frag adjustIrradiance; params1.w = enabled):
    // RenderSSAOIrradianceScale, RenderSSAOIrradianceMax, effect-matrix diag, nondiag.
    vec4 ao_params;
    // AO extras (the current camera view is per view: views[v].cur_view): x = specular
    // occlusion on (RenderVKSpecularOcclusion), y = standard AO multiply (RenderVKAOStandard;
    // else LL adjustIrradiance).
    vec4 ao_params2;        // .z: Highlight Transparent (LLPipeline::sShowDebugAlpha),
                            // .w: opaque water (RenderTransparentWater off)
    // Lightbox sky effects (clean_sky.frag / atmosStarField): sky_fx = aurora
    // intensity (0 = off), aurora time (s), meteor count, radians per screen pixel;
    // sky_fx2.x = star density (RenderStarCount over the upstream default total).
    vec4 sky_fx;
    vec4 sky_fx2;
    // Per meteor i: [3i] head dir.xyz (world, unit), width scale; [3i+1] tail dir.xyz;
    // [3i+2] linear colour x envelope. <GMDX> At most 56 meteors: [168..191] are the night
    // sky's (gmdx_night_sky.glsl NS_SPACE: planets, satellites, star options).
    vec4 meteors[192];
    // Cloud layer (clean_clouds.glsl): [0] colour.rgb, scale  [1] pos/density1 (scrolled),
    // variance  [2] pos/density2, noise blend  [3] noise idx A, B, 0, enabled.
    vec4 clouds[4];
    // Sun/moon billboards, stars, rainbow/halo (clean_sky_bodies.glsl): [0..2] sun
    // dir+drawn, right+blend, up  [3..5] moon  [6] moisture, droplet radius, ice
    // [7] star alpha, rotation, twinkle time  [8] tex idx sun A, B, moon, bloom
    // [9] tex idx rainbow, halo.
    vec4 bodies[10];
    // TEMPORAL (#84, stage 1): the UNJITTERED camera-relative world (P - cam_origin, #555)
    // -> clip of this frame and of the previous frame are per view (views[v].taa_cur_vp /
    // taa_prev_vp: motion vectors are computed from these, never from the jittered push
    // matrix); here the projection jitter of this frame's main view (xy pixels, x right / y
    // down; zw the same offset in NDC, added to clip.xy / w -- ONE phase for every view) and
    // params: x previous frame's time (s, GPU spinners' prev pose), y 1 = the motion-vector
    // target was written this frame, z 1 = history reset this frame (teleport / region
    // crossing / origin shift / camera cut: prev == current), w 1 = jitter on (RenderVKTAA).
    vec4 taa_jitter;
    vec4 taa_params;
    // RENDER SCALE (#181, RenderVKRenderScale): x the material mip bias while jittered
    // (-0.5 + log2(render / display)), y render / display (1 = native); z RT sun:
    // distance past which sun rays skip the alpha test (RenderVKRTSunAlphaDist, 0 = off);
    // w RT soft sun: the disc samples' elevation floor near the terminator, in disc radii
    // (RenderVKRTShadowTerminatorFloor, 0 = off; clean_shadow_rt.glsl sunShadowRayCore).
    vec4 taa_scale;
    // RAY-TRACED SUN / MOON SHADOWS (#185; shadow_params.w == 2, ray-query variants):
    // x sun disc angular radius (rad, 0 = hard), y rays per pixel, z EXTRA receiver offset
    // (m, RenderVKRTShadowBias, default 0: clean_rt_common.glsl rtReceiverOrigin),
    // w ray length (m).
    vec4 sun_rt;
    // Render options: x shadow-map filter (SlipstreamRenderShadowFilterQuality: 0 Low,
    // 1 Medium, 2 High, 3 Ultra PCSS), y terrain hex tiling (SlipstreamRenderTerrainHexTiling),
    // z translucent shadows (#212: cascade layer + coloured shadow rays), w the
    // projector atlas's translucency layer.
    vec4 render_opts;
    // RAY-TRACED SHADOW MASK (#195): x 1 = pass 1 of the main view reads the mask
    // (clean_rt_shadow_mask.comp) instead of tracing, y mask px per render px, zw the
    // mask size.
    vec4 rt_mask;
    // LIT-PASS COST SWITCHES (RenderVKLitDebugMask, live): x = the bit mask as a float
    // (clean_draw_common.glsl litDbg). Main view only; 0 in normal use.
    // LIT-PASS TRIMS (every view): y = probe top-N (RenderVKProbeTopN, 0 = all; clamp 4),
    // z = local-light brightness cutoff (RenderVKLightCutoff, linear; 0 = off).
    vec4 lit_dbg;
    // SMOOTH TERRAIN (#211, clean_terrain_lod.glsl): xyz the MAIN camera's LOD position
    // (the cull's u_lod_cam_pos of the main view: every view morphs terrain the same way),
    // w 0 = smooth terrain off, else 1 + flags (an exact integer; geomorph on whenever
    // > 0.5): + 1 the RT shadow mask's terrain receiver re-anchor
    // (RenderVKRTTerrainOriginSnap; clean_rt_shadow_mask.comp maskTerrainExit), + 2 its
    // debug view (RenderVKRTShadowDebug 12).
    vec4 terrain_lod;
    // CAMERA-RELATIVE ORIGIN (#555): xyz the main eye of this recording. shadow_mat[c]
    // and taa_cur_vp / taa_prev_vp map (P - cam_origin.xyz), not P: with the ~km agent-space translation folded in
    // on the CPU (double), shadow lookups keep ground-level precision at any altitude.
    // Use shadowSpace() below.
    // w: TEMPORAL EXCLUSION bits (#605; 0 unless the main view is jittered): 1 = media
    // faces (RenderVKMediaNoTemporal), 2 = animated-texture faces (RenderVKAnimTexNoTemporal)
    // skip the TAA / FSR history (noTemporalFace below).
    vec4 cam_origin;
    // ALPHA-BLEND DEPTH TOLERANCE (#731, clean_draw.frag -DCLEAN_BLEND_TOL): x base (m,
    // RenderVKBlendDepthTolMM / 1000), y distance scale (m per m, RenderVKBlendDepthTolScale),
    // z grazing multiplier cap (>= 1), w 1 = debug tint of the fragments the tolerance kept.
    vec4 blend_tol;
    // RT FOLIAGE TRANSMISSION (#735, clean_rt_common.glsl rtFoliageSet; sun / moon rays
    // only): x mode (RenderVKRTFoliageTransmission 0 off / 1 cull / 2 tinted) | 4 facing
    // flip (RenderVKRTFoliageFacingFlip) | 8 rigged / worn facing parity
    // (RenderVKRTRiggedFacingParity) | 16 back-face self-shadow (RenderVKRTBackfaceSelfShadow;
    // clean_shadow_rt.glsl rtBackfaceSelfShadow -- every RT shadow ray, not only foliage)
    // | 32 shadowed projector ambiance (RenderVKProjAmbianceShadowedMode; projAmbShadowed),
    // y strength (RenderVKRTFoliageTransmissionStrength),
    // z tint amount (RenderVKRTFoliageTransmissionTint), w layer cap
    // (RenderVKRTFoliageTransmissionLayers).
    vec4 foliage_rt;
    // CLUSTER LOD SHADOW RECEIVER LIFT (#747 interim, clean_rt_shadow_mask.comp): x metres
    // of lift per metre of camera distance (the main view's tolerance / px_scale x
    // RenderVKClusterShadowLiftScale), y the near clip (m), z 1 = an avatar is drawn by the
    // cluster path this frame (0: no lift, no probe).
    // w <GMDX> (unused upstream): space at altitude, 0 .. 1 (clean_atmos_common.glsl gmdxSpace).
    vec4 cluster_rt;
    // AMBIENT OCCLUSION RESOLUTION (RenderVKAOHalfRes / RenderVKAOUpsample; clean_surface_light
    // aoSample): x 0 = the AO target is full resolution (read at the pixel), 1 = half resolution,
    // plain bilinear, 2 = half resolution, depth-aware (the meta at draw set 0 binding 21 [1]);
    // y the upsample's absolute plane tolerance (m: RenderVKAOBlurTol, 0.01 when that is 0).
    // z (#819, not AO): 1 = the sky-view LUT (draw set 0 binding 52) holds this frame's sky
    // (RenderVKSkyLUT; clean_atmos_common.glsl skyColor), w (#1086, not AO; an exact integer): bit 0 the RT
    // shadow mask upsample's thin-receiver fallback (RenderVKRTShadowMaskThinFallback), bits 1+
    // its plane tolerance floor in mm (RenderVKRTShadowMaskUpsampleTolMM;
    // clean_shadow_mask_read.glsl rtMaskThinFallback / rtMaskPlaneTolFloor).
    vec4 ao_up;
    // PER-FRAME HAZE CONSTANTS (#816; clean_atmos_common.glsl header, LLCleanRendererVK
    // fillAtmosK): what atmosScatter / atmosLightTerms used to recompute per pixel from atmos[].
    vec4 atmos_k[6];
    // VOLUMETRIC FOG (#718; clean_vol_common.glsl has the accessors and the contract,
    // LLCleanVolFogVK::fillFrameInfo the values). All zero while the fog is off.
    //  vol_vp     UNJITTERED P * rotation-only V of the froxel grid: (world - vol_eye.xyz) ->
    //             VK clip (the main view's; clip.w = view depth)
    //  vol_eye    xyz the grid's eye (world), w 1 = the integrated volume (draw set 0
    //             binding 45) is valid this frame: main-view consumers apply it; w 2 = also
    //             volumetric particles with sprite replacement (#721): the lit particle VS
    //             fades each sprite by its Particle.glow.z (clean_vol_part.comp)
    //  vol_grid   froxels x, y, depth slices, far plane (view depth, m)
    //  vol_slice  x slice distribution n (m), y ln(1 + far / n), z the haze's sun scatter
    //             coefficient (sigma_s haze x gain x intensity, /m; the god rays term), w HG g
    //  vol_fog    extra height fog: x sigma at the base height (/m), y height falloff (1/m),
    //             z base height (m, world z), w ambient scale
    //  vol_albedo rgb the extra fog's scattering albedo, w 1 = the analytic height fog past
    //             the far plane
    //  vol_misc   x local-light scale, y draw distance (m; the sky's fog distance), z water
    //             height cut (world z; no extra fog below), w 1 = sun shadows available
    mat4 vol_vp;
    mat4 vol_inv_vp;
    vec4 vol_eye;
    vec4 vol_grid;
    vec4 vol_slice;
    vec4 vol_fog;
    vec4 vol_albedo;
    vec4 vol_misc;
    // VOLUMETRIC CLOUDS (#719; LLCleanVolCloudsVK, clean_cloud_shadow_map.glsl / clean_vol_cloud_sky.glsl):
    // [0] mode (0 = classic 2D layer, 2 = volumetric layer live), base / top altitude (m),
    //     cloud shadow strength (0 = no cloud shadow map)
    // [1] shadow map centre xy (agent world, on the base plane), 1 / extent (per m), panorama valid
    // [2] light direction the shadow map was built for (xyz), w <GMDX> the clouds' moonlight gain
    //     (GMDXNightCloudLight; clean_atmos_common.glsl gmdxCloudMoonGain, every frame)
    // [3] reserved (<GMDX> cloud corona xyz, the night sky's light w)
    vec4 vol_clouds[4];
    // WEATHER (#783; LLCleanWeatherVK::fillFrameInfo, clean_weather_surface.glsl has the
    // contract): [0] wetness, rain, time, ripple strength; [1] coarse occlusion map corner xy,
    // 1 / extent, which map (0 none, 1 / 2); [2] its top, span, cover bias, texel; [3] / [4]
    // the same for the near cascade; [5] snow cover, jitter phase, PCF radius (m, texels);
    // M2 [6] - [12]: the maps' shears, lightning, cloud / sky, persistent snow cover; M4 [13] -
    // [14]: the run-off flow map's frame and strength; #954 [15] the fog-off precipitation veil
    // (rgb its in-scatter colour, w its extinction /m), [16] the precipitation medium in the
    // volumetric fog (clean_weather_veil.glsl has both contracts). Zeros = off.
    vec4 weather[17];
    // SUN / MOON SHADOW-MAP RECEIVER FIT (clean_draw_common.glsl shadowFitOn; LLCleanRendererVK
    // fills it from RenderVKShadowReceiverFit / RenderVKShadowPlaneTolerance /
    // RenderVKShadowPCSSMaxRadius): x 1 = on (0 = the old receiver offsets and filter compares),
    // y the receiver-plane slope tolerance (depth m per m of filter-tap offset), z the PCSS filter
    // radius cap (world m, 0 = none), w <GMDX> the moon disc's brightness as a light (GMDXMoonDisc,
    // clean_atmos_common.glsl gmdxMoonDiscGain; 0 = LL's disc, -1 = a creator's moon texture: LL's disc, no phase).
    vec4 shadow_fit;
    // <GMDX> PHYSICAL SKY parameters (RenderVKPhysicalSky; clean_physky.glsl has the layout,
    // LLCleanRendererVK::physSkyFill fills them). [4].w = 0: the WindLight sky as before.
    vec4 physky[8];
    // <GMDX> ALPHA-MASK PULL (clean_draw.vert; GMDXAlphaMaskPull*): x base metres, y metres per
    // metre of distance, z grazing-factor cap; x = 0: off.
    vec4 gmdx_misc;
    // LAYERED STEREO (#1075): the per-view block (SSRView above), [0] = the desktop's / the left eye's.
    SSRView views[CLEAN_MAX_VIEWS];
} ssr;

// Cascade c's shadow NDC (xy [-1,1] -> uv, z [0,1]) of world position P (#555).
vec4 shadowSpace(int c, vec3 P) { return ssr.shadow_mat[c] * vec4(P - ssr.cam_origin.xyz, 1.0); }
// SHADOWED PROJECTOR AMBIANCE (RenderVKProjAmbianceShadowedMode, frame info foliage_rt.x bit 5,
// default on): a shadowed projector's ambiance term takes its diffuse visibility (localLight,
// clean_rt_shade.glsl, the RT shadow mask's / stochastic sampler's ambiance-only coverage).
// 0 = the old unshadowed ambiance (LL: it reached through walls). Every includer, particles too.
// (foliage_rt.x bit 4 is RenderVKRTBackfaceSelfShadow: clean_shadow_rt.glsl / localLight projAmbBackSelf.)
bool projAmbShadowed() { return (uint(ssr.foliage_rt.x + 0.5) & 32u) != 0u; }
// ReSTIR DI FRAME (#869 Phase 5, RenderVKReSTIRDI; foliage_rt.x bit 64): the light pass built the
// light grid's ReSTIR population this frame (clean_regir.glsl RS_F_BUILT) -- pass 15 draws its
// candidates from it and the lit kernels shade its light set with the ratio. Only then may a
// kernel read the grid header's rs_* words (else set 2 binding 12 can be a placeholder).
bool rsFrameOn() { return (uint(ssr.foliage_rt.x + 0.5) & 64u) != 0u; }

// TAA MIP BIAS (temporal #84, stage 3): while the main view is jittered (RenderFSAAType
// 3 / RenderVKTAA), material textures sample half a mip sharper -- the resolve averages
// the extra detail across jitter phases instead of blurring it a second time. Below
// native resolution (#181) the bias also drops by log2(render / display), so textures
// keep the detail of the DISPLAY resolution the resolve reconstructs. HUD faces
// (flag bit 3) are never jittered. The depth pre-pass alpha tests (clean_prepass_mask /
// clean_prepass_normal) use the SAME bias on the same sample, so the EQUAL test's
// fragment sets still match bit for bit.
// TEMPORAL EXCLUSION (#605): a face whose texture changes every frame with no geometric
// motion -- live media (instance bit 20, set at ingest for a MEDIA_TEXTURE base colour)
// or, optionally, a GPU texture animation (bit 10) -- has no usable history: the motion
// pre-pass (opaque / mask) and the alpha FS (blend, mReact.B) write its coverage into the
// masks clean_taa.comp / clean_fsr_inputs.comp read as "this frame only". Gated live by
// cam_origin.w (C++ zeroes it when the view is not jittered). HUD faces never resolve.
const uint INST_MEDIA = 1048576u;
bool noTemporalFace(uint flags)
{
    const uint b = uint(ssr.cam_origin.w + 0.5);
    return (flags & 8u) == 0u
        && (((b & 1u) != 0u && (flags & INST_MEDIA) != 0u) || ((b & 2u) != 0u && (flags & 1024u) != 0u));
}

// No-history faces keep the plain mip: a sharpened sample without the accumulation that
// averages it is moire / shimmer (video, scrolling text).
// ALPHA-MASK COVERAGE PER MIP (#648 round 4, RenderVKAlphaMipCoverage; ssr.lit_dbg.x bits
// [18,24) in 1/16): a mask texture's lower mips average blade / leaf / fence alpha toward
// the background, so past some distance its texels fall below the cutoff and the cutout
// geometry thins out -- for dense alpha shell grass a sharp line at a fixed camera
// distance (where the depth pre-pass, and with it the AO, loses the blades). The cutoff
// TEST alpha is scaled by 1 + lod * k (Golus 2017, "Anti-aliased alpha test"), lod the
// sampled level (textureQueryLod + the same bias), so coverage stays near level 0's.
// Every alpha-test site of one surface (the pre-passes and the lit FS's EQUAL pass) must
// use it identically. Only the TEST is scaled; blending keeps the real alpha. 0 = off.
// (#1044) The raster's form of THE alpha-test rule (clean_alpha_rule.glsl): its coverage scale.
#include "clean_alpha_rule.glsl"
float maskCoverageK() { return float((uint(ssr.lit_dbg.x + 0.5) >> 18) & 63u) * (1.0 / 16.0); }
float maskTestAlpha(float a, float lod) { return alphaCoverage(a, lod, maskCoverageK()); }
// <GMDX> with the material's cutoff: above the cutoff ceiling (247.5/255, llvovolume.cpp
// kMaskCutoffCeiling) it is a dissolve's "only fully opaque" (clean_mask_cutoff): no coverage
// scale, or the faded parts come back.
float maskTestAlpha(float a, float lod, float cutoff) { return cutoff <= 0.9706 ? maskTestAlpha(a, lod) : a; }

float taaLodBias(uint flags) { return (ssr.taa_params.w > 0.5 && (flags & 8u) == 0u && !noTemporalFace(flags)) ? ssr.taa_scale.x : 0.0; }

// ---- SETTING SPECIALIZATION (RenderVKSpecPipelines; LLCleanRendererVK spec sets,
// llcleanspecvk.cpp kSpecTable = THE table of setting -> constant id -> value) ----
// The heavy lit shaders (clean_draw.frag lit variants, clean_deferred_light.comp uber +
// split passes, clean_water.frag, clean_particle.frag, clean_particle_light.comp) define
// CLEAN_SPEC_SETTINGS and get one specialization constant per graphics setting below;
// every other includer gets the same names as plain constants. -1 (the default, and every
// plain constant) = GENERIC: the frame-info field is tested at run time exactly as before
// -- a pipeline built without specialization data is the runtime-branch build. A
// specialized pipeline carries the setting's value instead, and the driver drops the
// paths that value makes dead. Two kinds:
//  * EXACT (filter quality, probe top-N, AO mode): the value replaces the field.
//  * CAP (the rest): an upper bound of the field -- 0 folds the feature away, a higher
//    value keeps the run-time test where the frame can still differ from the setting
//    (SSR history missing, the TLAS not valid, the mirror out of view, ...).
// The renderer writes each field through LLCleanRendererVK::specApply with the bound
// pipelines' key (exact value / field clamped to the cap), so the field and the folded
// code always agree and every helper below returns what the run-time test would.
#ifdef CLEAN_SPEC_SETTINGS
layout(constant_id = 8)  const int kSpecFilter = -1;   // EXACT SlipstreamRenderShadowFilterQuality 0..3 (render_opts.x)
layout(constant_id = 9)  const int kSpecSun = -1;      // CAP sun / moon: 0 off, 1 cascades, 2 + ray traced (shadow_params.w)
layout(constant_id = 10) const int kSpecProj = -1;     // CAP projector shadows: 0 off, 1 maps, 2 + ray traced (pshadow params.z)
layout(constant_id = 11) const int kSpecTrans = -1;    // CAP translucent shadows 0 / 1 (render_opts.z / .w)
layout(constant_id = 12) const int kSpecSSR = -1;      // CAP screen-space reflections 0 / 1 (params1.y)
layout(constant_id = 13) const int kSpecMirror = -1;   // CAP planar mirror 0 / 1 (mirror_params.x)
layout(constant_id = 14) const int kSpecCel = -1;      // CAP cel shading 0 / 1 (per-surface flag stays run time)
layout(constant_id = 15) const int kSpecTopN = -1;     // EXACT RenderVKProbeTopN 0..4 (lit_dbg.y)
layout(constant_id = 16) const int kSpecAO = -1;       // CAP ambient occlusion 0 / 1 (params1.w)
layout(constant_id = 17) const int kSpecAOMode = -1;   // EXACT bit 0 specular occlusion, bit 1 standard AO (ao_params2.xy)
// (#818) Not settings, but per-frame modes the lit paths branch on three ways / two ways:
layout(constant_id = 18) const int kSpecSky = -1;      // EXACT the sky's lighting mode: 0 linear, 1 classic (atmos[8].w)
layout(constant_id = 19) const int kSpecAOUp = -1;     // EXACT AO upsample: 0 full res, 1 bilinear, 2 depth-aware (ao_up.x)
#else
const int kSpecFilter = -1, kSpecSun = -1, kSpecProj = -1, kSpecTrans = -1, kSpecSSR = -1;
const int kSpecMirror = -1, kSpecCel = -1, kSpecTopN = -1, kSpecAO = -1, kSpecAOMode = -1;
const int kSpecSky = -1, kSpecAOUp = -1;
#endif
int  specFilterQuality() { return kSpecFilter >= 0 ? kSpecFilter : clamp(int(ssr.render_opts.x + 0.5), 0, 3); }
bool specSunOn()         { return kSpecSun != 0 && ssr.shadow_params.w > 0.5; }
bool specSunTraced()     { return (kSpecSun < 0 || kSpecSun >= 2) && ssr.shadow_params.w > 1.5; }
bool specProjOn()        { return kSpecProj != 0; }                     // (records still per light)
bool specProjTraced()    { return kSpecProj < 0 || kSpecProj >= 2; }    // a record may be ray traced
bool specTransOn()       { return kSpecTrans != 0 && ssr.render_opts.z > 0.5; }
bool specProjTransOn()   { return kSpecTrans != 0 && ssr.render_opts.w > 0.5; }
bool specSSROn()         { return kSpecSSR != 0 && ssr.params1.y > 0.5; }
bool specMirrorOn()      { return kSpecMirror != 0 && ssr.mirror_params.x > 0.5; }
bool specCelOn()         { return kSpecCel != 0; }
uint specProbeTopN()     { return kSpecTopN >= 0 ? uint(kSpecTopN) : min(uint(ssr.lit_dbg.y + 0.5), 4u); }
bool specAOOn()          { return kSpecAO != 0 && ssr.params1.w > 0.5; }
bool specSpecOcc()       { return kSpecAOMode >= 0 ? (kSpecAOMode & 1) != 0 : ssr.ao_params2.x > 0.5; }
bool specAOStandard()    { return kSpecAOMode >= 0 ? (kSpecAOMode & 2) != 0 : ssr.ao_params2.y > 0.5; }
