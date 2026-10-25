#!/usr/bin/env python3
# Generate vk_world_shaders_spv.h, vk_fsr3_shaders_spv.h and vk_world_shaders_hash.h from the
# vkclean GLSL sources via glslc --target-env=vulkan1.3 -O. CMake runs this at build time
# (indra/newview/CMakeLists.txt, target vk_shaders) into the build tree; the headers are not
# in git. By hand: regen_world_spv.py --out-dir <dir>. Compiles are parallel and cached
# (spvbuild.py), so an edit to one shader recompiles only the variants it changes.
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # push_budget.py, spvbuild.py
import spvbuild

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "shaders")

# Shader stages this script compiles (glslc -fshader-stage names). task / mesh (#743,
# VK_EXT_mesh_shader; the cluster LOD mesh-shader tier): SPIR-V 1.6 under vulkan1.3, so the
# same flags work; the embedded module is only used where LLCleanClusterCaps reports the tier.
STAGES = ("comp", "vert", "frag", "task", "mesh")

# (glsl file, stage, array symbol)
SHADERS = [
    ("clean_cull.comp",        "comp", "kWorldCullSpv"),
    # Occlusion culling (#167): Hi-Z pyramid + in-place compaction of the static buckets.
    ("clean_hiz.comp",         "comp", "kWorldHizSpv"),
    ("clean_occl.comp",        "comp", "kWorldOcclSpv"),
    # Two-phase occlusion (RenderVKOcclusion2Phase): split (P1 list) + Hi-Z test / compaction.
    ("clean_occl2.comp",       "comp", "kWorldOccl2Spv"),
    ("clean_draw.vert",        "vert", "kWorldVertSpv"),
    ("clean_draw.frag",        "frag", "kWorldFragSpv"),
    ("clean_water.frag",       "frag", "kWorldWaterFragSpv"),
    # Opaque capture water (#36): reflection-probe faces + planar mirror; no set 2.
    ("clean_water.frag",       "frag", "kWorldWaterCapFragSpv", ["-DWATER_OPAQUE"]),
    # Water exclusion mask (invisiprims, #27): R8, sampled by clean_water.frag.
    ("clean_water_excl.frag",  "frag", "kWorldWaterExclFragSpv"),
    ("clean_sky.vert",         "vert", "kWorldSkyVertSpv"),
    ("clean_sky.frag",         "frag", "kWorldSkyFragSpv"),
    # (#819) the sky-view LUT bake (RenderVKSkyLUT; recordSkyLut)
    ("clean_sky_lut.comp",     "comp", "kWorldSkyLutSpv"),
    ("clean_preskin.comp",     "comp", "kWorldPreskinSpv"),
    # Rigged vertex welding (#574, RenderVKRigWeld): insert / weld / apply after the pre-skin.
    ("clean_rig_weld.comp",    "comp", "kWorldRigWeldSpv"),
    ("clean_skin_palette.comp","comp", "kWorldSkinPaletteSpv"),
    ("clean_anim.comp",        "comp", "kWorldAnimSpv"),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertSpv"),
    # DEPTH-ONLY VS variants (RenderVKDepthOnlyVS): the static / rigged world VS reduced to the
    # position chain (no-FS depth pipes: shadow / projector casters, the opaque pre-pass) and
    # + the alpha test's inputs (mask / alpha-depth / translucency FS pipes). Same source lines
    # as the full VS for gl_Position (invariant), so the EQUAL passes after them still match.
    ("clean_draw.vert",        "vert", "kWorldVertDepthSpv",           ["-DCLEAN_DEPTH_ONLY"]),
    ("clean_draw.vert",        "vert", "kWorldVertDepthMaskSpv",       ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertDepthSpv",     ["-DCLEAN_DEPTH_ONLY"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertDepthMaskSpv", ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK"]),
    # SLIM PRE-PASS VS variants (RenderVKDepthOnlyVS twins of the normal / motion pre-pass
    # pipelines): the MASK depth flavour + the motion chain (5 / 10: clean_prepass_motion.frag,
    # deferred temporal frames) / + the normal frame (2 / 5 / 7 / 8: clean_prepass_normal.frag,
    # forward AO frames) / + both (forward temporal frames). Same gl_Position (invariant).
    ("clean_draw.vert",        "vert", "kWorldVertMotionSpv",          ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw.vert",        "vert", "kWorldVertNormalSpv",          ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL"]),
    ("clean_draw.vert",        "vert", "kWorldVertNormalMotionSpv",    ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertMotionSpv",    ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertNormalSpv",    ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertNormalMotionSpv", ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_glow.comp",         "comp", "kWorldGlowSpv"),
    ("clean_glow_combine.vert", "vert", "kWorldGlowCombineVertSpv"),
    ("clean_glow_combine.frag", "frag", "kWorldGlowCombineFragSpv"),
    ("clean_lights.comp",       "comp", "kWorldLightsSpv"),
    # Froxel light masks (RenderVKLightCulling 2): per light, its covered froxels' 256-bit masks.
    ("clean_light_froxel.comp", "comp", "kWorldLightFroxelSpv"),
    # (#869) World-space light reservoir grid (RenderVKLightGrid; clean_regir.glsl): per cell of a
    # camera clipmap, RIS light samples read by the RT reflection hits.
    ("clean_regir_build.comp",  "comp", "kWorldRegirBuildSpv"),
    ("clean_probe_sky.vert",    "vert", "kWorldProbeSkyVertSpv"),
    ("clean_probe_sky.frag",    "frag", "kWorldProbeSkyFragSpv"),
    ("clean_probe_filter.comp", "comp", "kWorldProbeFilterSpv"),
    # RenderVKProbeFormat (#565): the filter writing RGB9E5 / R11G11B10 through an r32ui
    # alias, and the BC6H encoder (format 3: scratch cube -> BC6H array / live cubes).
    ("clean_probe_filter.comp", "comp", "kWorldProbeFilterRgb9e5Spv", ["-DPROBE_PACK=1"]),
    ("clean_probe_filter.comp", "comp", "kWorldProbeFilterR11g11b10Spv", ["-DPROBE_PACK=2"]),
    ("clean_probe_bc6h.comp",   "comp", "kWorldProbeBc6hSpv"),
    ("clean_probe_cluster.comp","comp", "kWorldProbeClusterSpv"),
    # GPU-driven reflection probes (#478): placement / residency / scheduling, and the
    # capture's ring-entry camera patch.
    ("clean_probe_gpu.comp",    "comp", "kWorldProbeGpuSpv"),
    ("clean_probe_patch.comp",  "comp", "kWorldProbePatchSpv"),
    # #568 render-world frame: a region crossing's agent shift on the GPU instance rows.
    ("clean_inst_shift.comp",   "comp", "kWorldInstShiftSpv"),
    ("clean_select.frag",       "frag", "kWorldSelectFragSpv"),
    # <GMDX> selection outline (GMDXSelectionStyle 1, vkgfx/gmdxseloutlinevk.cpp): the filled
    # selection mask + tile marks, and the tile-quad edge pass over the finished frame.
    ("gmdx_sel_mask.frag",      "frag", "kGmdxSelMaskFragSpv"),
    ("gmdx_sel_outline.vert",   "vert", "kGmdxSelOutlineVertSpv"),
    ("gmdx_sel_outline.frag",   "frag", "kGmdxSelOutlineFragSpv"),
    # <GMDX> instant replay (vkgfx/gmdxreplayvk.cpp): the captured frame -> the encoder's 4:2:0 input
    ("gmdx_replay_nv12.comp",   "comp", "kGmdxReplayNv12Spv"),
    ("clean_tonemap.frag",      "frag", "kWorldTonemapFragSpv"),
    # (#820) the tonemap / grade tail baked into a 3D LUT (RenderVKTonemapLut; recordTonemapLut).
    ("clean_tonemap_lut.comp",  "comp", "kWorldTonemapLutSpv"),
    ("clean_exposure.comp",     "comp", "kWorldExposureSpv"),
    ("clean_alpha_sort.comp",   "comp", "kWorldAlphaSortSpv"),
    # Alpha sort past the single-workgroup cap (B9): multi-workgroup bitonic.
    ("clean_alpha_sort_large.comp", "comp", "kWorldAlphaSortLargeSpv"),
    # #238: 2048-draw single-workgroup variants (~20 KiB shared) for devices whose
    # maxComputeSharedMemorySize can't hold the 4096 one (~36 KiB; AMD GCN / Polaris).
    ("clean_alpha_sort.comp",       "comp", "kWorldAlphaSortSmallSmemSpv",      ["-DALPHA_SORT_MAXS=2048u"]),
    ("clean_alpha_sort_large.comp", "comp", "kWorldAlphaSortLargeSmallSmemSpv", ["-DALPHA_SORT_MAXS=2048u"]),
    # Dynamic probe faces: rigged draws culled per face by wearer bounds.
    ("clean_probe_rigcull.comp","comp", "kWorldProbeRigCullSpv"),
    # Shadow views (#564): rigged casters culled per cascade / projector tile by wearer bounds.
    ("clean_shadow_rigcull.comp","comp", "kWorldShadowRigCullSpv"),
    ("clean_sel_rig.comp",      "comp", "kWorldSelRigSpv"),
    ("clean_shadow_mask.frag",  "frag", "kWorldShadowMaskFragSpv"),
    ("clean_shadow_trans.frag", "frag", "kWorldShadowTransDepthFragSpv"),
    ("clean_rt_shadow_mask.comp", "comp", "kWorldRtShadowMaskSpv", ["-DRT_SET=2", "-DCLEAN_PROJ_SHADOW_RAYQUERY"]),
    # RT shadow denoiser (clean_rt_mask_common.glsl): temporal + spatial passes.
    ("clean_rt_mask_temporal.comp", "comp", "kWorldRtMaskTemporalSpv"),
    ("clean_rt_mask_spatial.comp",  "comp", "kWorldRtMaskSpatialSpv"),
    # #85 phase 2: the diffuse / specular lighting denoiser (LLCleanDenoiserVK): temporal + a-trous.
    ("clean_dn_temporal.comp",      "comp", "kWorldDnTemporalSpv"),
    ("clean_dn_atrous.comp",        "comp", "kWorldDnAtrousSpv"),
    # #771 RT sun shadow cache: tile classifier + indirect-args finalize.
    ("clean_rt_mask_cache.comp",    "comp", "kWorldRtMaskCacheSpv"),
    # #771 Phase 2 sun exposure certificates (SXC): the per-frame table maintenance kernel.
    ("clean_sun_cert.comp",         "comp", "kWorldSunCertSpv"),
    # Moving-caster shadow change -> mReact.R (TAA / FSR reactive, RenderVKRTShadowReactive).
    ("clean_rt_mask_react.frag",    "frag", "kWorldRtMaskReactFragSpv"),
    ("clean_shadow_trans.frag", "frag", "kWorldShadowTransColorFragSpv", ["-DTRANS_COLOR"]),
    # <SS:Skoomastream> Translucent shadows on: the LL-solid texels of alpha-blend casters into depth.
    ("clean_shadow_mask.frag",  "frag", "kWorldShadowMaskSolidFragSpv", ["-DBLEND_SOLID"]),
    # Planar-mirror occlusion-query proxy box (36 verts from gl_VertexIndex, no outputs).
    ("clean_occ_box.vert",      "vert", "kWorldOccBoxVertSpv"),
    ("clean_occ_box.frag",      "frag", "kWorldOccBoxFragSpv"),
    ("clean_particle_sim.comp",      "comp", "kWorldPartSimSpv"),
    ("clean_particle_emit.comp",     "comp", "kWorldPartEmitSpv"),
    ("clean_particle_compact.comp",  "comp", "kWorldPartCompactSpv"),
    ("clean_particle_sort.comp",     "comp", "kWorldPartSortSpv"),
    ("clean_particle_indirect.comp", "comp", "kWorldPartIndirectSpv"),
    ("clean_particle.vert",          "vert", "kWorldPartVertSpv"),
    ("clean_particle.frag",          "frag", "kWorldPartFragSpv"),
    # Per-particle lighting (RenderVKParticleLighting 1): the compute that lights every
    # live particle once (plain + ray-query sun / projector shadows), and the main-view
    # draw VS / FS that only forward / apply its record.
    ("clean_particle_light.comp",    "comp", "kWorldPartLightSpv"),
    ("clean_particle_light.comp",    "comp", "kWorldPartLightRtSpv", ["-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"]),
    ("clean_particle.vert",          "vert", "kWorldPartVertLitSpv", ["-DPART_VERTEX_LIGHT"]),
    ("clean_particle.frag",          "frag", "kWorldPartFragLitSpv", ["-DPART_VERTEX_LIGHT"]),
    # Reduced-resolution particles (RenderVKParticleLowRes): farthest-depth 2x2 downsample
    # and the depth-aware (nearest-depth) upsample composite (llcleanpartlowresvk.cpp).
    ("clean_part_lowres.frag",       "frag", "kWorldPartLowResDepthFragSpv", ["-DPART_LR_DEPTH"]),
    ("clean_part_lowres.frag",       "frag", "kWorldPartLowResCompFragSpv"),
    # Particle sun-shadow casters (#39): alpha-tested depth, translucency depth / colour.
    ("clean_particle_shadow.frag",   "frag", "kWorldPartShadowFragSpv"),
    ("clean_particle_shadow.frag",   "frag", "kWorldPartShadowTransDepthFragSpv", ["-DTRANS_DEPTH"]),
    ("clean_particle_shadow.frag",   "frag", "kWorldPartShadowTransColorFragSpv", ["-DTRANS_COLOR"]),
    # Post-process AA (LL RenderFSAAType 1 = FXAA, 2 = SMAA; RenderFSAASamples 0..3 = preset).
    ("fxaa.frag",            "frag", "kWorldFxaaFragSpv0", ["-DFXAA_QUALITY__PRESET=12", "-w"]),
    ("fxaa.frag",            "frag", "kWorldFxaaFragSpv1", ["-DFXAA_QUALITY__PRESET=23", "-w"]),
    ("fxaa.frag",            "frag", "kWorldFxaaFragSpv2", ["-DFXAA_QUALITY__PRESET=28", "-w"]),
    ("fxaa.frag",            "frag", "kWorldFxaaFragSpv3", ["-DFXAA_QUALITY__PRESET=39", "-w"]),
    ("smaa_edge.vert",       "vert", "kWorldSmaaEdgeVertSpv"),
    ("smaa_edge.frag",       "frag", "kWorldSmaaEdgeFragSpv0",  ["-DSMAA_PRESET_LOW"]),
    ("smaa_edge.frag",       "frag", "kWorldSmaaEdgeFragSpv1",  ["-DSMAA_PRESET_MEDIUM"]),
    ("smaa_edge.frag",       "frag", "kWorldSmaaEdgeFragSpv2",  ["-DSMAA_PRESET_HIGH"]),
    ("smaa_edge.frag",       "frag", "kWorldSmaaEdgeFragSpv3",  ["-DSMAA_PRESET_ULTRA"]),
    ("smaa_blend.vert",      "vert", "kWorldSmaaBlendVertSpv0", ["-DSMAA_PRESET_LOW"]),
    ("smaa_blend.vert",      "vert", "kWorldSmaaBlendVertSpv1", ["-DSMAA_PRESET_MEDIUM"]),
    ("smaa_blend.vert",      "vert", "kWorldSmaaBlendVertSpv2", ["-DSMAA_PRESET_HIGH"]),
    ("smaa_blend.vert",      "vert", "kWorldSmaaBlendVertSpv3", ["-DSMAA_PRESET_ULTRA"]),
    ("smaa_blend.frag",      "frag", "kWorldSmaaBlendFragSpv0", ["-DSMAA_PRESET_LOW"]),
    ("smaa_blend.frag",      "frag", "kWorldSmaaBlendFragSpv1", ["-DSMAA_PRESET_MEDIUM"]),
    ("smaa_blend.frag",      "frag", "kWorldSmaaBlendFragSpv2", ["-DSMAA_PRESET_HIGH"]),
    ("smaa_blend.frag",      "frag", "kWorldSmaaBlendFragSpv3", ["-DSMAA_PRESET_ULTRA"]),
    ("smaa_neighbor.vert",   "vert", "kWorldSmaaNeighborVertSpv"),
    ("smaa_neighbor.frag",   "frag", "kWorldSmaaNeighborFragSpv"),
    # Ray tracing foundation (LLCleanRTVK): TLAS-instance / rig-copy compute and the
    # ray-query acceptance debug views. Only created when the RT extensions are enabled.
    ("clean_rt_instances.comp", "comp", "kWorldRtInstancesSpv"),
    ("clean_rt_omm_test.comp",  "comp", "kWorldRtOmmTestSpv"),
    ("clean_rt_omm_bake.comp",  "comp", "kWorldRtOmmBakeSpv"),
    ("clean_rt_debug.frag",     "frag", "kWorldRtDebugFragSpv"),
    # Ray-traced reflection-probe capture (RenderReflectionProbeDetail 3): the capture
    # cube's faces traced against the TLAS (clean_rt_surface / clean_rt_shade hit shading).
    ("clean_probe_rt.comp",     "comp", "kWorldProbeRtSpv", ["-DRT_SET=2"]),
    # Acoustics probe (#683 phase 0, AudioAcoustics): listener room rays + one occlusion
    # ray per traced sound source against the TLAS (set 0 = the RT consumer set).
    ("clean_audio_rt.comp",     "comp", "kWorldAudioRtSpv"),
    # #930: + LYING SNOW (set 2 = the weather's published snow grids, read only); used only
    # while snow lies (AudioAcousticsSnowAbsorb > 0), else the plain probe above runs.
    ("clean_audio_rt.comp",     "comp", "kWorldAudioRtSnowSpv", ["-DAC_SNOW"]),
    # GPU raster cursor pick (#702 / #704): the candidates along any ray (the main cull's
    # snapshot + HUD rows), the fragment list of their 1x1 draw (the world VS modules), and
    # the count-then-fill window narrowing between its two draw rounds.
    ("clean_pick_select.comp",  "comp", "kWorldPickSelectSpv"),
    ("clean_pick.frag",         "frag", "kWorldPickFragSpv"),
    ("clean_pick_window.comp",  "comp", "kWorldPickWindowSpv"),
    # Drawn-geometry queries (#704): normal sum / first vertex frame / screen-rect test of
    # instances' drawn slots, for the build tools.
    ("clean_pick_query.comp",   "comp", "kWorldPickQuerySpv"),
    # GPU cursor pick of particles (#481): the pick segment vs every live particle quad.
    ("clean_pick_particle.comp", "comp", "kWorldPickParticleSpv"),
    # Ray-query variants of the lit world / particle FS (ray-traced projector shadows,
    # RenderVKProjectorShadows 2). RT set at set 2 (draw layout) / set 3 (particles).
    ("clean_draw.frag",     "frag", "kWorldFragRtSpv",     ["-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    ("clean_particle.frag", "frag", "kWorldPartFragRtSpv", ["-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"]),
    # <GMDX> REZ EFFECTS: the lit FS twins with the Tron rez-in wireframe (gmdx_rezfx.glsl; they read
    # VK_KHR_fragment_shader_barycentric, so the renderer binds them only on a device that has it:
    # vkgfx/gmdxrezfxvk.cpp GMDXRezFx::wireModule swaps them in by module name).
    ("clean_draw.frag", "frag", "kWorldFragWireSpv",       ["-DREZFX_WIRE"]),
    ("clean_draw.frag", "frag", "kWorldFragEzWireSpv",     ["-DREZFX_WIRE", "-DCLEAN_EARLY_Z"]),
    ("clean_draw.frag", "frag", "kWorldFragRtWireSpv",     ["-DREZFX_WIRE", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    ("clean_draw.frag", "frag", "kWorldFragRtEzWireSpv",   ["-DREZFX_WIRE", "-DCLEAN_EARLY_Z", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    ("clean_draw.frag", "frag", "kWorldFragBlendWireSpv",  ["-DREZFX_WIRE", "-DCLEAN_BLEND_TOL"]),
    ("clean_draw.frag", "frag", "kWorldFragBlendRtWireSpv",["-DREZFX_WIRE", "-DCLEAN_BLEND_TOL", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufWireSpv",   ["-DREZFX_WIRE", "-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufNtWireSpv", ["-DREZFX_WIRE", "-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_NO_TERRAIN"]),
    # Reflection-probe CAPTURE FS (#829, RenderVKProbeCaptureSlim): probeCapture() constant,
    # no probes / SSR / mirror / AO / debug / glow; the ray-query twin traces one hard sun ray.
    ("clean_draw.frag",     "frag", "kWorldFragCapSpv",    ["-DCLEAN_CAPTURE"]),
    ("clean_draw.frag",     "frag", "kWorldFragCapRtSpv",  ["-DCLEAN_CAPTURE", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    # DEFERRED PROBE CAPTURE (#833, RenderVKProbeDeferred): the probe faces' G-buffer FS (capture
    # states, a ready terrain bake at every distance) and the probe-resolution lighting kernel
    # (the capture FS's lightSurface in compute; the ray-query twin: RT set 3, one hard sun ray).
    ("clean_draw.frag",        "frag", "kWorldFragGbufCapSpv",  ["-DCLEAN_GBUFFER", "-DCLEAN_CAPTURE_GBUF"]),
    ("clean_probe_defer.comp", "comp", "kWorldProbeDeferSpv"),
    ("clean_probe_defer.comp", "comp", "kWorldProbeDeferRtSpv", ["-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"]),
    # Alpha-blend depth tolerance (#731): the main view's LIT_ALPHA / LIT_RIG_ALPHA FS (no
    # hardware depth test; metric test against the pass-1 depth at draw set 0 binding 40),
    # plain and ray-query.
    ("clean_draw.frag",     "frag", "kWorldFragBlendSpv",   ["-DCLEAN_BLEND_TOL"]),
    ("clean_draw.frag",     "frag", "kWorldFragBlendRtSpv", ["-DCLEAN_BLEND_TOL", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    # Depth pre-pass (RenderVKDepthPrepass): pass-1 lit FS with early_fragment_tests
    # (drawn EQUAL / no depth write after the pre-pass), plain and ray-query, and the
    # pre-pass alpha-test FS for the mask buckets (opaque buckets pre-pass with no FS).
    ("clean_draw.frag",         "frag", "kWorldFragEzSpv",   ["-DCLEAN_EARLY_Z"]),
    ("clean_draw.frag",         "frag", "kWorldFragRtEzSpv", ["-DCLEAN_EARLY_Z", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    ("clean_prepass_mask.frag", "frag", "kWorldPrepassMaskFragSpv"),
    ("clean_prepass_blend.frag", "frag", "kWorldPrepassBlendFragSpv"),
    # #664: the RT terminator triangle pass (EQUAL after the pre-pass; Hanika lift of the
    # exact rasterized triangle, VK_KHR_fragment_shader_barycentric).
    ("clean_term_tri.frag", "frag", "kWorldTermTriFragSpv"),
    # SSAO + depth-aware blur (RenderDeferredSSAO) into the shared AO target; one
    # variant per storage format (the image2D format qualifier must match the view).
    ("clean_ao.comp", "comp", "kWorldAoR8Spv",  ["-DAO_FORMAT=r8"]),
    ("clean_ao.comp", "comp", "kWorldAoR16Spv", ["-DAO_FORMAT=r16f"]),
    ("clean_ao.comp", "comp", "kWorldAoR32Spv", ["-DAO_FORMAT=r32f"]),
    # #86 RAY-TRACED AO (RenderVKAOMethod 2): the AO kernel stage over the TLAS, one variant
    # per AO storage format (as clean_ao.comp); sets: AO set, draw set 1, RT set, draw set 0.
    ("clean_rtao.comp", "comp", "kWorldRtaoR8Spv",  ["-DAO_FORMAT=r8",   "-DRT_SET=2"]),
    ("clean_rtao.comp", "comp", "kWorldRtaoR16Spv", ["-DAO_FORMAT=r16f", "-DRT_SET=2"]),
    ("clean_rtao.comp", "comp", "kWorldRtaoR32Spv", ["-DAO_FORMAT=r32f", "-DRT_SET=2"]),
    # #648 (RenderVKAODepthMips): the AO linear view-depth MIP chain (XeGTAO prefilter).
    ("clean_ao_depth.comp", "comp", "kWorldAoDepthSpv"),
    # #828 (RenderVKAODepthSPD): the same chain, 64 x 64 tile per workgroup (SPD-style).
    ("clean_ao_depth_spd.comp", "comp", "kWorldAoDepthSpdSpv"),
    # Depth + normal pre-pass FS (AO frames): opaque buckets and the mask buckets.
    ("clean_prepass_normal.frag", "frag", "kWorldPrepassNormalFragSpv"),
    ("clean_prepass_normal.frag", "frag", "kWorldPrepassNormalMaskFragSpv", ["-DPREPASS_MASK"]),
    # Depth + normal + MOTION pre-pass FS (temporal #84): RG16F motion vectors at attachment 1.
    ("clean_prepass_normal.frag", "frag", "kWorldPrepassMotionFragSpv", ["-DPREPASS_MOTION"]),
    ("clean_prepass_normal.frag", "frag", "kWorldPrepassMotionMaskFragSpv", ["-DPREPASS_MOTION", "-DPREPASS_MASK"]),
    # Depth + MOTION pre-pass FS, no normals (RenderVKPrepassDepthOnlyDeferred: deferred
    # temporal frames; the G-buffer carries the normals): motion at 0, no-history mask at 1.
    ("clean_prepass_motion.frag", "frag", "kWorldPrepassMotionOnlyFragSpv"),
    ("clean_prepass_motion.frag", "frag", "kWorldPrepassMotionOnlyMaskFragSpv", ["-DPREPASS_MASK"]),
    # Depth of field (RenderDepthOfField): LL cofF -> postDeferredF -> dofCombineF.
    ("clean_dof_cof.frag",     "frag", "kWorldDofCofFragSpv"),
    ("clean_dof_gather.frag",  "frag", "kWorldDofGatherFragSpv"),
    ("clean_dof_gather.frag",  "frag", "kWorldDofGatherShapedFragSpv", ["-DDOF_SHAPED=1"]),
    ("clean_dof_combine.frag", "frag", "kWorldDofCombineFragSpv"),
    # Lightbox final display-space effects (distortion, vignette, CVD, grain, dither, preview).
    ("clean_final.frag",       "frag", "kWorldFinalFragSpv"),
    # RLVa @setsphere vision restriction (display space, ahead of the final pass).
    ("clean_rlv_sphere.frag",  "frag", "kWorldRlvSphereFragSpv"),
    # Lightbox HDR bloom pyramid (extract / downsample / upsample by push mode).
    ("clean_bloom.frag",       "frag", "kWorldBloomFragSpv"),
    ("clean_cross.frag",       "frag", "kWorldCrossFragSpv"),
    ("clean_lensdirt.frag",    "frag", "kWorldLensDirtFragSpv"),
    ("clean_flarestate.frag",  "frag", "kWorldFlareStateFragSpv"),
    # Temporal anti-aliasing resolve (temporal #84 stage 3; RenderFSAAType 3 / RenderVKTAA).
    ("clean_taa.comp",         "comp", "kWorldTaaSpv"),
    ("clean_taa.comp",         "comp", "kWorldTaaAccR16Spv", ["-DTAA_ACC_FORMAT=r16f"]),
    # Render scale (#181): unjittered display-size depth for the display-space passes.
    ("clean_depth_up.comp",    "comp", "kWorldDepthUpSpv"),
    # Sharpening (#183; RenderCASSharpness): AMD RCAS on the resolved HDR scene.
    ("clean_rcas.comp",        "comp", "kWorldRcasSpv"),
    # Snapshot supersampling (#417; RenderSnapshotTemporalFrames): equal-weight running
    # mean of N jittered offscreen frames (scene + glow).
    ("clean_snap_accum.comp",  "comp", "kWorldSnapAccumSpv"),
    # Live snapshots (#873; llcleansnapshotvk.h): the live frame / depth resampled to the shot
    # size (Lanczos H / V, depth), and the jittered accumulation of upscaled shots (acc / resolve).
    ("clean_snap_live.comp",   "comp", "kWorldSnapLiveHSpv",       ["-DSNAP_MODE=0"]),
    ("clean_snap_live.comp",   "comp", "kWorldSnapLiveVSpv",       ["-DSNAP_MODE=1"]),
    ("clean_snap_live.comp",   "comp", "kWorldSnapLiveDepthSpv",   ["-DSNAP_MODE=2"]),
    ("clean_snap_live.comp",   "comp", "kWorldSnapLiveAccSpv",     ["-DSNAP_MODE=3"]),
    ("clean_snap_live.comp",   "comp", "kWorldSnapLiveResolveSpv", ["-DSNAP_MODE=4"]),
    # God rays / volumetric sun shafts (#208; RenderVKGodRays): cascade march at reduced
    # resolution + depth-aware upsample / additive composite in pass 2a.
    # <GMDX> Physical sky (RenderVKPhysicalSky, gmdxphysskyvk.cpp): Hillaire's transmittance,
    # multiple-scattering and sky-view lookup tables.
    ("clean_physky_lut.comp",         "comp", "kWorldPhysSkyTransSpv",   ["-DPK_PASS_TRANS"]),
    ("clean_physky_lut.comp",         "comp", "kWorldPhysSkyMsSpv",      ["-DPK_PASS_MS"]),
    ("clean_physky_lut.comp",         "comp", "kWorldPhysSkyViewSpv",    ["-DPK_PASS_SKYVIEW"]),
    ("clean_godrays.comp",            "comp", "kWorldGodRaysSpv"),
    ("clean_godrays.comp",            "comp", "kWorldGodRaysRtSpv", ["-DGODRAYS_RT", "-DRT_SET=2"]),
    ("clean_godrays_temporal.comp",   "comp", "kWorldGodRaysTemporalSpv"),
    ("clean_godrays_composite.frag",  "frag", "kWorldGodRaysCompFragSpv"),
    # God rays denoiser (#657; RenderVKGodRaysDenoise): the RT march with moving-caster
    # tracking (clean_rt_moving_caster.glsl), the RT-shadow-style temporal pass and the
    # a-trous spatial / finalize pass (shared helpers: clean_denoise_common.glsl).
    ("clean_godrays.comp",            "comp", "kWorldGodRaysRtDnSpv", ["-DGODRAYS_RT", "-DGODRAYS_TRACK_MOVERS", "-DRT_SET=2"]),
    ("clean_godrays_dn_temporal.comp","comp", "kWorldGodRaysDnTemporalSpv"),
    ("clean_godrays_atrous.comp",     "comp", "kWorldGodRaysAtrousSpv"),
    # Volumetric fog (#718; RenderVKVolumetricFog, vkgfx/llcleanvolfogvk.cpp): the froxel
    # passes (density inject, light per cell, temporal reprojection, front-to-back
    # integration) and the opaque + sky apply at the start of pass 2.
    ("clean_vol_inject.comp",         "comp", "kWorldVolInjectSpv"),
    ("clean_vol_light.comp",          "comp", "kWorldVolLightSpv"),
    # (+ ray-traced projector shadows in the fog: the RT consumer set at 3, above the own set)
    ("clean_vol_light.comp",          "comp", "kWorldVolLightRtSpv", ["-DVOL_RT", "-DRT_SET=3"]),
    ("clean_vol_temporal.comp",       "comp", "kWorldVolTemporalSpv"),
    ("clean_vol_integrate.comp",      "comp", "kWorldVolIntegrateSpv"),
    ("clean_vol_apply.frag",          "frag", "kWorldVolApplyFragSpv"),
    # (+ volumetric particles #721, RenderVKVolumetricParticles: the pool -> per-distance
    # classify + capped selection / screen-tile binning pass, before the inject)
    ("clean_vol_part.comp",           "comp", "kWorldVolPartSpv"),
    # Particle fog (#1003; RenderVKParticleFog, vkgfx/llcleanpartfogvk.cpp): fog-like particle
    # systems become dense local density bricks -- per-texture stats, bin + mass-conserving
    # splat, resolve, in-brick light grid, the noise volume, the reduced-resolution march, its
    # temporal accumulation and the composite after the froxel fog's apply in pass 2
    # (VS: kWorldGlowCombineVertSpv).
    ("clean_pfog_texstats.comp",      "comp", "kWorldPFogTexStatsSpv"),
    ("clean_pfog_bin.comp",           "comp", "kWorldPFogBinSpv"),
    ("clean_pfog_splat.comp",         "comp", "kWorldPFogSplatSpv"),
    ("clean_pfog_resolve.comp",       "comp", "kWorldPFogResolveSpv"),
    ("clean_pfog_light.comp",         "comp", "kWorldPFogLightSpv"),
    ("clean_pfog_noise.comp",         "comp", "kWorldPFogNoiseSpv"),
    ("clean_pfog_march.comp",         "comp", "kWorldPFogMarchSpv"),
    ("clean_pfog_temporal.comp",      "comp", "kWorldPFogTemporalSpv"),
    ("clean_pfog_composite.frag",     "frag", "kWorldPFogCompFragSpv"),
    # Volumetric sky clouds (#719; RenderVKVolumetricClouds, llcleanvolcloudsvk.cpp): the
    # tiling 3D noises (once), the EEP weather map, the cloud shadow map, the probe / mirror
    # panorama, the quarter-res partial march, its temporal reconstruction and the
    # depth-aware composite (VS: kWorldGlowCombineVertSpv).
    ("clean_vol_cloud_noise.comp",     "comp", "kWorldVolCloudNoiseBaseSpv"),
    ("clean_vol_cloud_noise.comp",     "comp", "kWorldVolCloudNoiseDetailSpv", ["-DNOISE_DETAIL"]),
    ("clean_vol_cloud_weather.comp",   "comp", "kWorldVolCloudWeatherSpv"),
    ("clean_vol_cloud_shadowmap.comp", "comp", "kWorldVolCloudShadowSpv"),
    ("clean_vol_cloud_pano.comp",      "comp", "kWorldVolCloudPanoSpv"),
    ("clean_vol_cloud_march.comp",     "comp", "kWorldVolCloudMarchSpv"),
    ("clean_vol_cloud_temporal.comp",  "comp", "kWorldVolCloudTemporalSpv"),
    ("clean_vol_cloud_composite.frag", "frag", "kWorldVolCloudCompFragSpv"),
    # Weather (#783; WeatherEnabled, vkgfx/llcleanweathervk.cpp): the precipitation simulation +
    # visible-list compaction around the camera and its streak / flake draw in pass 2b. (The wet
    # surfaces are clean_draw.frag's weatherSurface, clean_weather_surface.glsl.)
    ("clean_weather_sim.comp",         "comp", "kWorldWeatherSimSpv"),
    ("clean_weather.vert",             "vert", "kWorldWeatherVertSpv"),
    ("clean_weather.frag",             "frag", "kWorldWeatherFragSpv"),
    # #954 the precipitation veil (opaque + sky; the fullscreen glow-combine VS): llcleanweathervk.cpp
    ("clean_weather_veil.frag",        "frag", "kWorldWeatherVeilFragSpv"),
    # M2 persistent snow: the cover grid's integration and the moving objects' own snow
    # (clean_weather_snow.comp; RenderVKWeatherSnowCover / RenderVKWeatherSnowMovers).
    ("clean_weather.vert",             "vert", "kWorldWeatherSplashVertSpv", ["-DWEATHER_SPLASH"]),
    ("clean_weather.frag",             "frag", "kWorldWeatherSplashFragSpv", ["-DWEATHER_SPLASH"]),
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowCoverSpv"),
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowInstSpv", ["-DWX_SNOW_INST"]),
    # M3: the avatars' own snow + their footprints' stamp list, the stamps; the snow surface
    # mesh's build (one compute) and its draw (G-buffer + the sun cascades' depth-only VS).
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowAvatarSpv", ["-DWX_SNOW_AVATAR"]),
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowStampSpv", ["-DWX_SNOW_STAMP"]),
    # M5 mover tracks: the berm gather after the cover pass (the displaced share of what the presses removed).
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowBermSpv", ["-DWX_SNOW_BERM"]),
    ("clean_weather_snow.comp",        "comp", "kWorldWeatherSnowRingSpv", ["-DWX_SNOW_RING"]),
    # #901 per-avatar shelter maps (clean_weather_shelter.comp; llcleanweathervk_shelter.cpp): the
    # layers' frames + per-slot rows, the work list from the main cull's candidates, the due tiles'
    # clear, the top-down raster of the avatars' skinned / worn geometry, the atlas publish.
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterPrepSpv", ["-DWX_SH_PREP"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterFilterSpv", ["-DWX_SH_FILTER"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterClearSpv", ["-DWX_SH_CLEAR"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterRasterSpv", ["-DWX_SH_RASTER"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterPublishSpv", ["-DWX_SH_PUBLISH"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterMeshSpv", ["-DWX_SH_MESH"]),
    # #901 rest-pose accumulation (clean_weather_accum.comp; llcleanweathervk_accum.cpp): the shelter
    # layout, after the shelter chain.
    ("clean_weather_accum.comp",       "comp", "kWorldWeatherAccumPrepSpv", ["-DWX_AC_PREP"]),
    ("clean_weather_accum.comp",       "comp", "kWorldWeatherAccumSpv", ["-DWX_AC_ACCUM"]),
    ("clean_weather_accum.comp",       "comp", "kWorldWeatherAccumStageSpv", ["-DWX_AC_STAGE"]),
    ("clean_weather_accum.comp",       "comp", "kWorldWeatherAccumResolveSpv", ["-DWX_AC_RESOLVE"]),
    # #901 / #909 follow-up: the CLUSTER LOD avatars in the shelter maps and the grids (the skinned
    # arena; layout {draw set 0, shelter set, cluster set}: llcleanweathervk_shelter.cpp
    # ensureShelterCluster).
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterClFilterSpv", ["-DWX_SH_CL_FILTER"]),
    ("clean_weather_shelter.comp",     "comp", "kWorldWeatherShelterClRasterSpv", ["-DWX_SH_CL_RASTER"]),
    ("clean_weather_accum.comp",       "comp", "kWorldWeatherAccumClSpv", ["-DWX_AC_ACCUM_CL"]),
    ("clean_weather_snowmesh.comp",    "comp", "kWorldWeatherSnowMeshSpv"),
    ("clean_weather_snowmesh.vert",    "vert", "kWorldWeatherSnowMeshVertSpv"),
    ("clean_weather_snowmesh.vert",    "vert", "kWorldWeatherSnowMeshDepthSpv", ["-DWX_MESH_DEPTH"]),
    ("clean_weather_snowmesh.frag",    "frag", "kWorldWeatherSnowMeshFragSpv"),
    # Its DEPTH PRE-PASS twins (RenderVKWeatherSnowMeshPrepass): one per pre-pass attachment set
    # (LLCleanRendererVK mPreDescCl: depth only, + AO normal, + normal / motion / no-history,
    # motion / no-history only).
    ("clean_weather_snowmesh.frag",    "frag", "kWorldWeatherSnowMeshPre0Spv", ["-DWX_MESH_PRE=0"]),
    ("clean_weather_snowmesh.frag",    "frag", "kWorldWeatherSnowMeshPre1Spv", ["-DWX_MESH_PRE=1"]),
    ("clean_weather_snowmesh.frag",    "frag", "kWorldWeatherSnowMeshPre2Spv", ["-DWX_MESH_PRE=2"]),
    ("clean_weather_snowmesh.frag",    "frag", "kWorldWeatherSnowMeshPre3Spv", ["-DWX_MESH_PRE=3"]),
    # Its FORWARD lit pass 1 draw (frames that do not go deferred -- a supersampled snapshot with
    # RT shadows among them): the lit FS's interface + clean_draw.frag's snow variant, plain and
    # ray-query.
    ("clean_weather_snowmesh.vert",    "vert", "kWorldWeatherSnowMeshFwdVertSpv", ["-DWX_MESH_FORWARD"]),
    ("clean_draw.frag",                "frag", "kWorldWeatherSnowMeshFwdSpv", ["-DCLEAN_WEATHER_SNOWMESH"]),
    ("clean_draw.frag",                "frag", "kWorldWeatherSnowMeshFwdRtSpv", ["-DCLEAN_WEATHER_SNOWMESH", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
    # M4 run-off: the flow build (clean_weather_runoff.comp, one pipeline, a pass per push),
    # the drips' spawn kernel and the streams' ribbon draw.
    ("clean_weather_runoff.comp",      "comp", "kWorldWeatherRunoffSpv"),
    ("clean_weather_sim.comp",         "comp", "kWorldWeatherRunoffSpawnSpv", ["-DWX_RUNOFF_SPAWN"]),
    ("clean_weather.vert",             "vert", "kWorldWeatherStreamVertSpv", ["-DWEATHER_STREAM"]),
    ("clean_weather.frag",             "frag", "kWorldWeatherStreamFragSpv", ["-DWEATHER_STREAM"]),
    # Motion blur (#118; RenderVKMotionBlur): velocity + TileMax, NeighborMax, the
    # reconstruction gather and its write-back into the render-size scene (before TAA / FSR).
    ("clean_mblur_tile.comp",         "comp", "kWorldMblurTileSpv"),
    ("clean_mblur_neighbor.comp",     "comp", "kWorldMblurNeighborSpv"),
    ("clean_mblur_recon.comp",        "comp", "kWorldMblurReconSpv"),
    ("clean_mblur_writeback.frag",    "frag", "kWorldMblurWritebackFragSpv"),
    # Opaque variable-rate shading (VRS tier 2; RenderVKOpaqueShadingRate): the rate image.
    ("clean_vrs_rate.comp",           "comp", "kWorldVrsRateSpv"),
    # Deferred lighting (RenderVKDeferredLighting): pass 1b's G-buffer variant of the lit FS
    # (EQUAL after the pre-pass, decode + pack) and the compute that lights it.
    ("clean_draw.frag",               "frag", "kWorldFragGbufSpv", ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER"]),
    # G-buffer terrain split (RenderVKGbufTerrainSplit): the terrain-free FS (buckets 0 1 6 7)
    # with the VS that collapses terrain draws, and the terrain-only FS (the cull's terrain
    # list; hex tiling as specialization constant 2).
    ("clean_draw.frag",  "frag", "kWorldFragGbufNtSpv",   ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_NO_TERRAIN"]),
    ("clean_draw.frag",  "frag", "kWorldFragGbufTerrSpv", ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_TERRAIN"]),
    # Slim G-buffer VS (RenderVKGbufSlimVS): only what the G-buffer FS reads (no previous-frame
    # pose, no pick outputs); same gl_Position source lines (invariant). + the split's
    # terrain-collapsing twin.
    ("clean_draw.vert",        "vert", "kWorldVertGbufSpv",          ["-DCLEAN_GBUFFER_VS"]),
    ("clean_draw.vert",        "vert", "kWorldVertGbufSkipTerrSpv",  ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VS_SKIP_TERRAIN"]),
    ("clean_draw_rigged.vert", "vert", "kWorldRiggedVertGbufSpv",    ["-DCLEAN_GBUFFER_VS"]),
    # Terrain bake (#811, RenderVKTerrainBake): the per-region bake compute, the slim bake-only
    # terrain G-buffer FS, and the terrain list's two VS sides (1: live terrain FS -- drops the
    # patches the bake covers; 2: bake-only FS -- keeps only those).
    ("clean_terrain_bake.comp", "comp", "kWorldTerrainBakeSpv"),
    ("clean_draw.frag",  "frag", "kWorldFragGbufTerrBakeSpv", ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_TERRAIN_BAKE"]),
    ("clean_draw.vert",  "vert", "kWorldVertGbufTerrLiveSpv", ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VS_TERRAIN_SIDE=1"]),
    ("clean_draw.vert",  "vert", "kWorldVertGbufTerrBakeSpv", ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VS_TERRAIN_SIDE=2"]),
    # G-buffer register probes (RenderVKGbufRegProbe: built + logged, never drawn)
    ("clean_draw.frag", "frag", "kWorldFragGbufProbeTerrSpv",  ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBPROBE_NO_TERRAIN"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufProbeHexSpv",   ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBPROBE_NO_HEX"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufProbeDbgSpv",   ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBPROBE_NO_DEBUG"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufProbeBumpSpv",  ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBPROBE_NO_BUMP"]),
    ("clean_draw.frag", "frag", "kWorldFragGbufProbeAllSpv",   ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBPROBE_NO_TERRAIN", "-DCLEAN_GBPROBE_NO_HEX", "-DCLEAN_GBPROBE_NO_DEBUG", "-DCLEAN_GBPROBE_NO_BUMP"]),
    # (#821) SLIM G-BUFFER VS REGISTER PROBES (RenderVKGbufRegProbe; clean_draw.vert CLEAN_VSPROBE_*):
    # the slim VS with one per-vertex feature compiled out per variant (+ all six), never drawn
    # unless RenderVKGbufProbeDraw 11..17 asks.
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeTexAnimSpv", ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_TEXANIM"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeTangentSpv", ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_TANGENT"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeEncloseSpv", ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_ENCLOSE"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeRestSpv",    ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_REST"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeCelSpv",     ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_CEL"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbePoseSpv",    ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_POSE"]),
    ("clean_draw.vert", "vert", "kWorldVertGbufProbeAllSpv",     ["-DCLEAN_GBUFFER_VS", "-DCLEAN_VSPROBE_NO_TEXANIM", "-DCLEAN_VSPROBE_NO_TANGENT",
                                                                   "-DCLEAN_VSPROBE_NO_ENCLOSE", "-DCLEAN_VSPROBE_NO_REST", "-DCLEAN_VSPROBE_NO_CEL",
                                                                   "-DCLEAN_VSPROBE_NO_POSE"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferredLightSpv"),
    # Compact glow target (RenderVKGlowTargetCompact: B10G11R11_UFLOAT) as the glow output.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferredLightG11Spv", ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    # Lighting register probes (RenderVKLightRegProbe: built with the live spec constants, logged, never bound)
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeLocalSpv", ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_LOCAL"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeReflSpv",  ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_REFL"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeAtmosSpv", ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_ATMOS"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeOnlyLocalSpv", ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_REFL", "-DCLEAN_LPROBE_NO_ATMOS"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeOnlyReflSpv",  ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_LOCAL", "-DCLEAN_LPROBE_NO_ATMOS"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeOnlyAtmosSpv", ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_LOCAL", "-DCLEAN_LPROBE_NO_REFL"]),
    ("clean_deferred_light.comp", "comp", "kWorldLightProbeAllSpv",   ["-DGLOW_OUT_FORMAT=r11f_g11f_b10f", "-DCLEAN_LPROBE_NO_LOCAL", "-DCLEAN_LPROBE_NO_REFL", "-DCLEAN_LPROBE_NO_ATMOS"]),
    # Split deferred lighting (RenderVKDeferredSplit): shadow resolve -> visibility buffer,
    # reflection resolve -> reflection buffer, then the combine (tile lights, HDR + glow).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowSpv",     ["-DCLEAN_DEFER_PASS=1"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectSpv",    ["-DCLEAN_DEFER_PASS=2"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferCombineSpv",    ["-DCLEAN_DEFER_PASS=3"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferCombineG11Spv", ["-DCLEAN_DEFER_PASS=3", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    # Half-resolution reflections (RenderVKDeferReflectHalfRes): per 2 x 2 block, + the sharp pixels.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectHalfSpv",  ["-DCLEAN_DEFER_PASS=4"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectSharpSpv", ["-DCLEAN_DEFER_PASS=5"]),
    # Light grid (RenderVKLightCulling 1): the per-16x16-tile light masks the uber / combine walk.
    ("clean_deferred_light.comp",     "comp", "kWorldLightGridSpv",         ["-DCLEAN_DEFER_PASS=6"]),
    # Feature split (#827, RenderVKDeferredFeatureSplit): the combine as BASE (no local lights) +
    # LOCAL (local lights over the listed tiles) + the tile classification (grid build + tile list).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferBaseSpv",         ["-DCLEAN_DEFER_PASS=7"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferBaseG11Spv",      ["-DCLEAN_DEFER_PASS=7", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferLocalSpv",        ["-DCLEAN_DEFER_PASS=8"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferLocalG11Spv",     ["-DCLEAN_DEFER_PASS=8", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferTileClassifySpv", ["-DCLEAN_DEFER_PASS=9"]),
    # #831 (RenderVKDeferClassifyFused): the shadow resolve with the tile classification as its prologue.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowClassifySpv", ["-DCLEAN_DEFER_PASS=10"]),
    # #859 (RenderVKRTReflections 1): hybrid RT reflections -- the trace over the RT tile list
    # (RT set 4) and its resolve (glossy filter + splice) between REFLECT and BASE / COMBINE; the
    # uber frames' pre-trace (RT set 3); the forward / water layer's reflections (RT set 4).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtTraceSpv",    ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"]),
    # #859 perf (RenderVKRTReflectionsRayList): the RAY LIST -- the per-tile ray selection (pass 16,
    # no trace) appends the rays, the list trace (pass 11 -DCLEAN_RT_LIST) runs one lane per ray.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtSelectSpv",    ["-DCLEAN_DEFER_PASS=16"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtTraceListSpv", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_RT_LIST"]),
    # (#87) the ray-traced GI trace (RenderVKGI; clean_gi_common.glsl): pass 11's layout + hit shading.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferGiTraceSpv", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_GI_TRACE"]),
    # (#87 GI-4) the GI radiance cache's resolve (RenderVKGICache): the GI trace's layout, one slot per lane.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferGiResolveSpv", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_GI_TRACE", "-DCLEAN_GI_RESOLVE"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtPreTraceSpv", ["-DCLEAN_DEFER_PASS=12", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"]),
    # #860 (RenderVKRTMirrors): the hero mirror image ray traced (pass 12's layout + set 4: the image).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtMirrorSpv",   ["-DCLEAN_DEFER_PASS=12", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3", "-DCLEAN_RT_MIRROR"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtResolveSpv",  ["-DCLEAN_DEFER_PASS=13"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRtFwdSpv",      ["-DCLEAN_DEFER_PASS=14", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"]),
    # #85 phase 2: the projector stochastic sampler (the denoiser's first producer; RT layout).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferProjStochSpv",  ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"]),
    # #195 / #85 merged RT shadows: the same sampler + the sun / moon trace into the denoiser's sun channel.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowMergeSpv", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_MERGE"]),
    # #85 / #869 Phase 1 (RenderVKShadowGenericCore): the same two kernels on the generic shadow core
    # (clean_shadow_core.glsl: one receiver setup, one light-sample loop, ONE shadow walk).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferProjStochCoreSpv",  ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_CORE"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowMergeCoreSpv", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_MERGE", "-DCLEAN_SHADOW_CORE"]),
    # (#91 / #869) the emissive triangle lights (RenderVKReSTIREmissive): pass 15's layout, before it.
    ("clean_deferred_light.comp",     "comp", "kWorldDeferRsEmissiveSpv", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_RS_EMISSIVE_BUILD"]),
    # Shader-clock twins of every lighting kernel (RenderVKShaderClockLighting, restart; needs
    # VK_KHR_shader_clock shaderSubgroupClock): per-stage cycle counters (clean_shader_clock.glsl).
    ("clean_deferred_light.comp",     "comp", "kWorldDeferredLightClockSpv",    ["-DCLEAN_SHADER_CLOCK"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferredLightG11ClockSpv", ["-DCLEAN_SHADER_CLOCK", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowClockSpv",      ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=1"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectClockSpv",     ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=2"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferCombineClockSpv",     ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=3"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferCombineG11ClockSpv",  ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=3", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectHalfClockSpv", ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=4"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferReflectSharpClockSpv",["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=5"]),
    ("clean_deferred_light.comp",     "comp", "kWorldLightGridClockSpv",        ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=6"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferBaseClockSpv",        ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=7"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferBaseG11ClockSpv",     ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=7", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferLocalClockSpv",       ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=8"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferLocalG11ClockSpv",    ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=8", "-DGLOW_OUT_FORMAT=r11f_g11f_b10f"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferTileClassifyClockSpv",["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=9"]),
    ("clean_deferred_light.comp",     "comp", "kWorldDeferShadowClassifyClockSpv", ["-DCLEAN_SHADER_CLOCK", "-DCLEAN_DEFER_PASS=10"]),
    # GPU block-compression encoder for sampled textures (RenderVKTextureCompression, #263):
    # BC1 / BC3 / BC4 / BC5 / BC7 (fast + HQ) and the BC -> RGBA8 decode copy, by push mode.
    ("clean_texenc.comp",             "comp", "kWorldTexEncSpv"),
    # Avatar mask (#549, vkgfx/llcelshadevk.cpp): the avatar-id pass FS (opaque / mask draws
    # EQUAL, alpha-blend hair LEQUAL + alpha threshold), the static buckets' worn-attachment
    # compaction, and the id target's display-res resolve (FX) / debug view.
    ("clean_avatar_id.frag",          "frag", "kWorldAvatarIdFragSpv"),
    ("clean_avatar_id.frag",          "frag", "kWorldAvatarIdAlphaFragSpv", ["-DID_ALPHA"]),
    ("clean_cel_filter.comp",         "comp", "kWorldCelFilterSpv"),
    ("clean_avatar_id_view.frag",     "frag", "kWorldAvatarIdResolveFragSpv", ["-DID_RESOLVE"]),
    ("clean_avatar_id_view.frag",     "frag", "kWorldAvatarIdDebugFragSpv"),
    # Cel shading outlines (#550): id / depth / crease edges into the render-res scene HDR.
    ("clean_cel_outline.frag",        "frag", "kWorldCelOutlineFragSpv"),
    # CLUSTER LOD selection / emission (#745, vkgfx/llcleanclusterselectvk.cpp): per-view cut
    # selection + culling + draw records, the per-view finalize (counts, dispatch / task args),
    # the cluster VS per MDI raster tier (1 uint8 indexed, 0 vertex pulling), the mesh-shader
    # tier's task + mesh stages, the G-buffer FS without the pre-pass EQUAL assumption (the
    # cluster draws write their own depth) and the debug-view G-buffer FS.
    ("clean_cluster_select.comp",          "comp", "kWorldClusterSelectSpv"),
    ("clean_cluster_select_finalize.comp", "comp", "kWorldClusterFinalizeSpv"),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8Spv",   ["-DCLEAN_CLUSTER"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullSpv", ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL"]),
    ("clean_cluster_draw.task",            "task", "kWorldClusterTaskSpv"),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshSpv"),
    # RenderVKDepthOnlyVS: the cluster tiers' depth passes (pre-pass, sun / projector, alpha-blend
    # depth, translucency) all alpha-test, so the MASK depth flavour only (task stage shared).
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8DepthSpv",   ["-DCLEAN_CLUSTER", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullDepthSpv", ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK"]),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshDepthSpv",     ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK"]),
    # ... and the slim pre-pass flavours (the normal / motion / normal + motion pre-pass twins).
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8MotionSpv",         ["-DCLEAN_CLUSTER", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8NormalSpv",         ["-DCLEAN_CLUSTER", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8NormalMotionSpv",   ["-DCLEAN_CLUSTER", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullMotionSpv",       ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullNormalSpv",       ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullNormalMotionSpv", ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshMotionSpv",           ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshNormalSpv",           ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL"]),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshNormalMotionSpv",     ["-DCLEAN_DEPTH_ONLY", "-DCLEAN_DEPTH_MASK", "-DCLEAN_DEPTH_NORMAL", "-DCLEAN_DEPTH_MOTION"]),
    ("clean_draw.frag",                    "frag", "kWorldFragGbufClusterSpv", ["-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_NO_TERRAIN"]),   # (terrain never takes the cluster path)
    ("clean_cluster_draw_debug.frag",      "frag", "kWorldClusterDebugFragSpv"),
    # The same draw stages with the cluster set at 3, for the captures' RAY-QUERY lit variant
    # ({draw0, draw1, RT consumer set, cluster}; FS kWorldFragRtSpv, RT_SET=2): mirror / probe
    # faces draw the switched avatars in ray-query lit frames too (legacy-skin skip).
    ("clean_draw.vert",                    "vert", "kWorldClusterVertU8RtSpv",   ["-DCLEAN_CLUSTER", "-DCLUSTER_SET=3"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPullRtSpv", ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLUSTER_SET=3"]),
    ("clean_cluster_draw.task",            "task", "kWorldClusterTaskRtSpv", ["-DCLUSTER_SET=3"]),
    ("clean_cluster_draw.mesh",            "mesh", "kWorldClusterMeshRtSpv", ["-DCLUSTER_SET=3"]),
    # Cluster GPU pick (#702 for cluster-drawn objects): the main view's selected entries the
    # pick ray crosses -> tier-0 records; the pull VS with the pick outputs; the pick FS with its
    # output set at 3 (the cluster set takes 2 in {draw0, draw1, cluster, pick out}).
    ("clean_cluster_pick.comp",            "comp", "kWorldClusterPickSpv"),
    # #748 perf pass: the MDI tiers' per-view rig-list cull (the mesh tier culls in its task stage).
    ("clean_cluster_rigcull.comp",         "comp", "kWorldClusterRigCullSpv"),
    # Rigged ALPHA-BLEND on the cluster path: the alpha lists' order (prep / scatter, and the
    # one-workgroup sort + prefix variant), vkgfx/llcleanclusterselectvk.cpp recordAlphaOrder.
    ("clean_cluster_alpha.comp",           "comp", "kWorldClusterAlphaSpv"),
    ("clean_cluster_alpha.comp",           "comp", "kWorldClusterAlphaOrderSpv", ["-DCL_ALPHA_ORDER"]),
    ("clean_draw.vert",                    "vert", "kWorldClusterVertPickSpv", ["-DCLEAN_CLUSTER", "-DCLEAN_CLUSTER_PULL", "-DCLEAN_PICK"]),
    ("clean_pick.frag",                    "frag", "kWorldPickFragClusterSpv", ["-DPICK_OUT_SET=3"]),
    # CLUSTER LOD skinning (#747, vkgfx/llcleanclusterskinvk.cpp): joint-stretch table, posed-
    # bound margins, legacy stats and the pre-skin args (prep modes 0..3), the cluster pre-skin
    # of the main view's selected rigged clusters (+ weld insert), the weld / apply on the cut.
    ("clean_cluster_preskin_prep.comp",    "comp", "kWorldClusterPreskinPrepSpv"),
    ("clean_cluster_preskin.comp",         "comp", "kWorldClusterPreskinSpv"),
    ("clean_cluster_preskin_weld.comp",    "comp", "kWorldClusterPreskinWeldSpv"),
    # ---- 2b (#748, vkgfx/llcleanclusterrtvk.cpp): CLUSTER LOD RAY TRACING -- the RT cut, CLAS
    # residency / decode / build arguments, cluster BLAS arguments, override rows, tier-0 cut
    # expansion; and the partitioned-TLAS diff alone (no cluster set: it runs in every RT frame
    # of a PTLAS session).
    ("clean_cluster_rt.comp",              "comp", "kWorldClusterRtSpv"),
    ("clean_cluster_rt.comp",              "comp", "kWorldClusterRtPtlasSpv", ["-DCL_RT_PTLAS_ONLY"]),
    # ---- end 2b ----
]

# AMD FidelityFX Super Resolution 3.1 upscaler (RenderVKUpscaler 1-6): the SDK's
# own Vulkan pass entry points (third_party/fidelityfx, MIT; see its README for the
# version and the local patches), one permutation fixed to this renderer's inputs, plus
# our input-preparation pass. Written to a separate header (vk_fsr3_shaders_spv.h).
FFX = os.path.join(HERE, "third_party", "fidelityfx")
FFX_PASS = os.path.join("..", "third_party", "fidelityfx", "shaders", "fsr3upscaler")
FFX_DEFS = [
    "-I" + os.path.join(FFX, "gpu"), "-I" + os.path.join(FFX, "gpu", "fsr3upscaler"),
    "-DFFX_GPU=1", "-DFFX_GLSL=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",     # the SDK's fixed choice
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",    # reference (no LUT; the SDK's non-AMD path)
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1",               # linear HDR scene
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1", # render-size motion
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",       # our motion is unjittered
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1",                # reversed-Z
    "-DFFX_FSR3UPSCALER_UPSCALED_OUTPUT_FORMAT=rgba16f",         # local patch 2: mHdr's format
    "-w",
]
FFX_SHARP0 = FFX_DEFS + ["-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0"]
FFX_SHARP1 = FFX_DEFS + ["-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1"]
FFX_NOWAVE = FFX_SHARP0 + ["-DFFX_SPD_NO_WAVE_OPERATIONS"]
def ffx(name): return os.path.join(FFX_PASS, f"ffx_fsr3upscaler_{name}_pass.glsl")
FSR_SHADERS = [
    ("clean_fsr_inputs.comp", "comp", "kFsrInputsSpv"),
    ("clean_fsr_probe.comp",  "comp", "kFsrProbeSpv"),   # RenderVKFSRDebugProbe (debug readback)
    ("clean_rr_guides.comp",  "comp", "kRrGuidesSpv"),   # #870 guide buffers (LLCleanDlssRRVK)
    ("clean_rr_unexpose.comp", "comp", "kRrUnexposeSpv"),   # #870 divide the exposure back out
    ("clean_rr_debug.comp",   "comp", "kRrDebugSpv"),      # #870 RenderVKDlssDebug (one network input on screen)
    ("clean_sr_color.comp",   "comp", "kSrColorSpv"),      # #962 DLSS Super Resolution colour + bias mask
    (ffx("prepare_inputs"),         "comp", "kFsrPrepareInputsSpv",        FFX_SHARP0),
    # SPD passes: subgroup-quad (wave) variant + an LDS-only fallback for devices without
    # subgroup quad operations in compute.
    (ffx("luma_pyramid"),           "comp", "kFsrLumaPyramidSpv",          FFX_SHARP0),
    (ffx("luma_pyramid"),           "comp", "kFsrLumaPyramidNoWaveSpv",    FFX_NOWAVE),
    (ffx("shading_change_pyramid"), "comp", "kFsrShadingPyramidSpv",       FFX_SHARP0),
    (ffx("shading_change_pyramid"), "comp", "kFsrShadingPyramidNoWaveSpv", FFX_NOWAVE),
    (ffx("shading_change"),         "comp", "kFsrShadingChangeSpv",        FFX_SHARP0),
    (ffx("prepare_reactivity"),     "comp", "kFsrPrepareReactivitySpv",    FFX_SHARP0),
    (ffx("luma_instability"),       "comp", "kFsrLumaInstabilitySpv",      FFX_SHARP0),
    (ffx("accumulate"),             "comp", "kFsrAccumulateSpv",           FFX_SHARP0),
    (ffx("accumulate"),             "comp", "kFsrAccumulateSharpenSpv",    FFX_SHARP1),
    (ffx("rcas"),                   "comp", "kFsrRcasSpv",                 FFX_SHARP0),
    (ffx("debug_view"),             "comp", "kFsrDebugViewSpv",            FFX_SHARP0),
]

def variant(entry):
    fname, stage = entry[0], entry[1]
    if stage not in STAGES:
        raise SystemExit(f"{fname}: unknown stage '{stage}' (expected one of {', '.join(STAGES)})")
    return (os.path.join(SRC, fname), stage, tuple(entry[3]) if len(entry) > 3 else ())

# SHADER CONTENT HASH (vk_world_shaders_hash.h): 64-bit BLAKE2b over every embedded SPIR-V word
# (world + FSR headers, in table order). The viewer appends it to the pipeline-cache build
# tag (llappviewer.cpp, vk_pipecache.h): a shader change starts the disk caches fresh
# instead of piling the old binaries up under one viewer version (the 225 MB file).
import hashlib
_hasher = hashlib.blake2b(digest_size=8)
def hash_words(words):
    _hasher.update(struct.pack(f"<{len(words)}I", *words))

HEADER = ["#pragma once", "#include <cstddef>", "#include <cstdint>", ""]

def main():
    a = spvbuild.args("Generate the embedded world / FSR SPIR-V headers", "world")
    jobs = [variant(e) for e in FSR_SHADERS] + [variant(e) for e in SHADERS]
    words_all = spvbuild.compile_many(jobs, a.cache_dir)
    fsr_words, world_words = words_all[:len(FSR_SHADERS)], words_all[len(FSR_SHADERS):]

    # #238: every push block <= 128 B (the Vulkan minimum maxPushConstantsSize).
    import push_budget
    push_budget.check([(p, st, fl, e[2]) for (p, st, fl), e in zip(jobs, FSR_SHADERS + SHADERS)], a.cache_dir)

    body = ["// AUTO-GENERATED at build time: AMD FidelityFX Super Resolution 3.1 upscaler passes",
            "// (indra/vkclean/third_party/fidelityfx, MIT -- see its LICENSE.txt / README.md) and",
            "// indra/vkclean/shaders/clean_fsr_{inputs,probe}.comp + clean_rr_{guides,unexpose,debug}.comp + clean_sr_color.comp, via glslc --target-env=vulkan1.3 -O",
            "// (indra/vkclean/regen_world_spv.py). Not in git."] + HEADER
    for entry, words in zip(FSR_SHADERS, fsr_words):
        hash_words(words)
        body.append(spvbuild.emit_array(entry[2], words))
    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_fsr3_shaders_spv.h"), "\n".join(body) + "\n")

    body = ["// AUTO-GENERATED at build time from indra/vkclean/shaders via glslc --target-env=vulkan1.3 -O",
            "// (indra/vkclean/regen_world_spv.py). Embedded so the viewer VK world path needs no runtime",
            "// shader files/packaging. Not in git."] + HEADER
    # <GMDX> The world SPIR-V (~170 MB of array text) is DEFINED once, in vk_world_shaders_spv.cpp;
    # the header only declares each array with its bound (sizeof() still works) and its byte size.
    # Before, every one of the ~27 renderer files that includes the header parsed and held all of
    # it: 3-4+ GB per compiler process, and parallel builds ran out of memory.
    defs = ["// AUTO-GENERATED at build time by indra/vkclean/regen_world_spv.py: the world SPIR-V",
            "// arrays declared in vk_world_shaders_spv.h. Not in git.",
            '#include "vk_world_shaders_spv.h"', ""]
    comp_words, task_words, mesh_words = [], [], []
    for entry, words in zip(SHADERS, world_words):
        sym, stage = entry[2], entry[1]
        hash_words(words)
        body.append(f"extern const uint32_t {sym}[{len(words)}];")
        body.append(f"static constexpr size_t {sym}_bytes = {len(words) * 4};")
        defs.append(spvbuild.emit_array(sym, words, definition=True))
        if stage == "comp": comp_words.append((sym, words))
        if stage == "task": task_words.append((sym, words))
        if stage == "mesh": mesh_words.append((sym, words))
    # #238: workgroup shared memory <= 32 KiB (AMD GCN), or a gated fallback that fits.
    push_budget.check_shared(comp_words, {"kWorldAlphaSortSpv": "kWorldAlphaSortSmallSmemSpv"})
    # #743: task / mesh shared memory against their own spec minimums (mesh: 28 KiB).
    if task_words: push_budget.check_shared(task_words, budget=push_budget.TASK_SMEM_BUDGET, what="task")
    if mesh_words: push_budget.check_shared(mesh_words, budget=push_budget.MESH_SMEM_BUDGET, what="mesh")
    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_world_shaders_spv.h"), "\n".join(body) + "\n")
    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_world_shaders_spv.cpp"), "\n".join(defs) + "\n")   # <GMDX>
    print(f"{len(SHADERS)} world + {len(FSR_SHADERS)} FSR shader variants")

    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_world_shaders_hash.h"),
        "// AUTO-GENERATED by indra/vkclean/regen_world_spv.py: 64-bit BLAKE2b of every embedded SPIR-V\n"
        "// word (vk_world_shaders_spv.h + vk_fsr3_shaders_spv.h). Keys the persistent pipeline\n"
        "// caches (llappviewer.cpp pipeline-cache build tag): a shader change starts them fresh.\n"
        "#pragma once\n#include <cstdint>\n\n"
        f"static constexpr uint64_t kVkWorldShadersHash = 0x{_hasher.hexdigest()}ull;\n")
    spvbuild.prune_cache(a.cache_dir)

if __name__ == "__main__":
    main()
