// vkclean -- the world draw's per-VIEW block (#238: push constants <= 128 B).
//
// Formerly the 240-byte draw push constant; now an entry of the draw-view ring (C++
// VKDrawPush, std430 stride 256, draw set 0 binding 36), selected by the 16-byte push
// below. `pc.u_*` reads are unchanged: `pc` names this draw's entry. The view index is
// a push constant, so it is dynamically uniform (scalar / constant-cache loads).
//
// Slot reuse by pass (all producers in llcleanrenderervk.cpp, unchanged by #238):
//   u_sun_color.w      1 = reflection-probe capture, 2 = planar-mirror capture
//   u_ambient_color.w  1 = main-view rigged: hide the agent's own avatar (mouselook)
//                      0.25 = main-view rigged: hide only its system-body head parts
//   u_inv_view_proj    particles: world->view (billboard basis)
//   u_water_fog        world FS: water fog colour / density; rigged in mirror / probe
//                      captures: .xyz main eye; probe capture .w: the light list;
//                      selection: outline colour; particles: .x blend pair, .y pool size
//   u_sky_zenith.w     1 = rigged pass that draws the cluster LOD path's avatars itself (main
//                      view opaque / mask, and the shadow / capture / pick passes whose
//                      cluster rig list runs): drop the instances it owns (#747,
//                      clean_draw_rigged.vert); 0 elsewhere (the F8 collapse mode is
//                      g_draw_push.f8)
#ifndef CLEAN_DRAW_VIEW_GLSL
#define CLEAN_DRAW_VIEW_GLSL

struct DrawView {
    mat4 u_view_proj;      // offset 0   : world -> clip (VK NDC)
    vec4 u_sun_dir;        // offset 64
    vec4 u_sun_color;      // offset 80
    vec4 u_ambient_color;  // offset 96
    vec4 u_cam_time;       // offset 112 : .xyz camera world pos, .w time (s)
    vec4 u_sky_horizon;    // offset 128 : reflection colour near the horizon
    vec4 u_sky_zenith;     // offset 144 : reflection colour overhead
    mat4 u_inv_view_proj;  // offset 160 : VK clip -> world (water refraction depth)
    vec4 u_water_fog;      // offset 224
    vec4 u_origin;         // offset 240 : CAMERA-RELATIVE ORIGIN (#555): u_view_proj maps
                           // (world - u_origin.xyz) to clip in clean_draw.vert and
                           // clean_particle.vert; 0 = a world-space u_view_proj
};                         // 256 B (std430 array stride 256)

#ifdef CLEAN_DRAW_VIEW_UBO
// DEFERRED LIGHTING (#814; clean_deferred_light.comp defines this): the main view's ONE entry
// as a UNIFORM BUFFER -- set 2 binding 10 of LLCleanRendererVK mDslDefer, type
// UNIFORM_BUFFER_DYNAMIC over the same ring region (range 256 B), its dynamic offset = the
// pushed entry x 256 at every bind (recordDeferredLighting). Same bytes as draw_views[view]
// (std140 == std430 for DrawView: vec4 / mat4 members only, struct size 256), so every
// pc.u_* read is unchanged -- but a constant-bank operand instead of a register-loaded
// global read. The whole ring (1 MB: 2048 entries x 2 frame slots, absolute indices) can't
// be a UBO (maxUniformBufferRange 64 KB on NVIDIA), so the per-draw graphics / particle
// passes keep the SSBO below.
layout(std140, set = 2, binding = 10) uniform DrawViewMain { DrawView draw_view_main; };
#else
layout(std430, set = 0, binding = 36) readonly buffer DrawViews { DrawView draw_views[]; };
#endif

// A compute consumer of the draw sets (the deferred lighting pass) declares its own
// push block -- named g_draw_push, with the view index as member `view` -- first.
#ifndef CLEAN_EXTERNAL_DRAW_PUSH
layout(push_constant) uniform DrawPush {
    uint view;   // absolute draw-view ring index
    uint f8;     // F8 alpha collapse: 0 off, 1 the cull-FRONT draw, 2 the cull-BACK draw (drops water),
                 // 3 water only (F2: the mirror water-cap draw over its alpha bucket)
    uint pad0;   // main-view alpha-blend FS (pass 2): VISR_* visibility-reuse flags (clean_draw_common.glsl)
    uint pad1;
} g_draw_push;     // 16 B
#endif

#ifdef CLEAN_DRAW_VIEW_UBO
#define pc draw_view_main
#else
#define pc draw_views[g_draw_push.view]
#endif

#endif
