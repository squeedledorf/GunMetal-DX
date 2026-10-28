// vkclean -- LAYERED STEREO view index (#1075; research/VR_LAYERED_STEREO_DESIGN.md §3.2).
//
// One SPIR-V serves the desktop (N = 1 view) and VR (N = 2): screen passes render the N views
// as layers of one target -- raster passes through MULTIVIEW (one draw, gl_ViewIndex selects
// the eye), compute passes over the view layers. CLEAN_VIEW is "this invocation's view":
//
//   vertex / fragment   kViewCount > 1 ? gl_ViewIndex : 0. kViewCount is specialization
//                       constant 20, set only on multiview pipelines (GraphicsPipelineDesc::
//                       views > 1); every other pipeline keeps the default 1, so the driver
//                       folds CLEAN_VIEW to the constant 0 and every per-view read
//                       (draw_views[base + CLEAN_VIEW], ssr.x[CLEAN_VIEW]) is the same
//                       constant-offset load as before. The module declares the MultiView
//                       capability (GL_EXT_multiview): the device enables `multiview`
//                       wherever it is supported, desktop included (vk_device.cpp).
//   task / mesh         0 for now: the cluster mesh tier is not drawn multiview (the design
//                       falls back to its vertex tier in VR without multiviewMeshShader).
//   compute             0 unless the kernel #defines CLEAN_VIEW before the first include of
//                       this file: a LAYERED kernel takes its view from the dispatch
//                       (#define CLEAN_VIEW CLEAN_VIEW_Z, gl_WorkGroupID.z with z = N: "shape
//                       A") or from a record ("shape B"). Check the kernel's own dispatch z
//                       before making it shape A. kViewCount is specialization constant 20
//                       in compute too, set (= N) only on the layered kernels' stereo
//                       pipelines (LLCleanRendererVK::viewSpec): every other compute pipeline
//                       keeps the default 1, so CLEAN_VIEW_Z folds to the constant 0 there and
//                       shared code that tests kViewCount (clean_draw_common.glsl
//                       mainClusterFrag: the head's superset grid) takes its desktop path.
//
// The stage comes from regen_world_spv.py (-DCLEAN_STAGE_<stage> on every variant). Without
// one (a standalone compile) CLEAN_VIEW is the constant 0.
#ifndef CLEAN_VIEW_GLSL
#define CLEAN_VIEW_GLSL

// The array bound of every per-view block (frame info, light info, TAA params): quad-view
// headsets need 4; the viewer ships N = 2 (owner decision, design §10.6). = C++
// LLCleanRendererVK::kMaxViews.
#define CLEAN_MAX_VIEWS 4

#if defined(CLEAN_STAGE_VERT) || defined(CLEAN_STAGE_FRAG)
#extension GL_EXT_multiview : require
layout(constant_id = 20) const int kViewCount = 1;
#define CLEAN_VIEW (kViewCount > 1 ? uint(gl_ViewIndex) : 0u)
#elif defined(CLEAN_STAGE_COMP)
layout(constant_id = 20) const int kViewCount = 1;
// The dispatch's view layer (shape A, z = N). Folded to the constant 0 on a single-view
// pipeline: the desktop's per-view reads stay the constant-offset loads they were.
#define CLEAN_VIEW_Z (kViewCount > 1 ? gl_WorkGroupID.z : 0u)
#else
const int kViewCount = 1;   // (task / mesh: shared code tests it; always folded)
#endif
#if !defined(CLEAN_STAGE_VERT) && !defined(CLEAN_STAGE_FRAG)
#ifndef CLEAN_VIEW
#define CLEAN_VIEW 0u
#endif
#endif

#endif
