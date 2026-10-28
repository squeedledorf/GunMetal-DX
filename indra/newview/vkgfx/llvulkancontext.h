/**
 * @file llvulkancontext.h
 * @brief Viewer-facing Vulkan bring-up: instance + surface + device + swapchain
 *        + a clear-and-present frame. This is the seam between the viewer's
 *        SDL window / display loop and the native-Vulkan vkgfx layer.
 *
 * INTEGRATION MODEL (milestone-scoped): GL remains the default and is entirely
 * untouched. When the RenderUseVulkan setting is TRUE, LLWindowSDL creates the
 * window with SDL_WINDOW_VULKAN (NOT SDL_WINDOW_OPENGL — SDL3 forbids both on one
 * window, see llwindowsdl.cpp), skips GL context creation, and the viewer's
 * display() diverts to LLVulkanContext::renderFrame() instead of the GL frame.
 *
 * The clean GPU-driven scene render into the swapchain is the NEXT step; this
 * milestone lands the swapchain + present path (clear to a known color) which is
 * the actual integration unknown. renderFrame() is structured so the recorded
 * command buffer is the single place a vkclean scene draw would slot in.
 */
#ifndef LL_LLVULKANCONTEXT_H
#define LL_LLVULKANCONTEXT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward-declared so this header does not pull Vulkan/SDL into viewer TUs that
// only need to poke the singleton. The .cpp owns the concrete Vulkan objects.
struct SDL_Window;
class LLRenderBackend;
class LLUUID;
struct LLVKPerfCaps;          // llvkautotune.h (#422)
struct LLVKAutoTuneSample;

// GPU RASTER CURSOR PICK (#702 / #704). Every frame the renderer draws what the main view
// (and the HUD) draws along the cursor ray into a 1x1 target centred on it -- the world's
// own vertex shaders, no depth test -- and lists every fragment inside the request's depth
// window with the raster's alpha verdict and the surface LL's pick reports; the CPU
// (llcleancursorpick.h) sorts the list and applies LL's pick rules. One answer per
// request, ~2 frames after it; a click's ray is answered synchronously between frames
// (LLVulkanContext::pickNow). It is the only pick: there is no CPU narrowphase (#704).
// See vkgfx/llcleanpickvk.h.
// The particle pick (#481) shares the ray fields (origin, dir, length, pixel_angle,
// alpha_threshold, serial).
struct LLVKPickRequest
{
    bool     valid = false;
    float    origin[3] = {};      // agent space (LLViewerWindow::cursorIntersect's start)
    float    dir[3] = {};         // unit
    float    length = 0.f;        // metres
    float    pixel_angle = 0.f;   // radians per pixel
    float    alpha_threshold = 0.1f;   // alpha-blend surfaces: alpha below it is empty (#276)
    int32_t  mouse_x = 0, mouse_y = 0;
    uint32_t serial = 0;
    // Raster pick (#702) only:
    // The world layer's 1x1 projection: maps (world - apex) to VK clip, the ray through the
    // pixel centre (column-major). apex = the camera origin it was built from.
    float    apex[3] = {};
    float    world_vp[16] = {};
    // The HUD layer (drawn only when `hud`): ray (HUD space) and its 1x1 projection of HUD
    // space (LL's HUD camera zoomed on the cursor).
    bool     hud = false;
    float    hud_origin[3] = {};
    float    hud_dir[3] = {};
    float    hud_length = 0.f;
    float    hud_vp[16] = {};
    // Depth windows along the rays (#704): only fragments with lo <= t < hi are counted and
    // listed -- world [win[0], win[1]), HUD [win[2], win[3]). The default takes everything;
    // an overflowing answer is asked again for a nearer window, a decided-nothing one for
    // the next (llcleancursorpick.cpp).
    float    win[4] = {-1.f, 1.0e30f, -1.f, 1.0e30f};
    uint32_t mode = 0;            // MODE_* bits
    static constexpr uint32_t MODE_PREPASS = 2u;   // RenderVKDepthPrepass: opaque buckets never alpha-cut
    // <SS:Skoomastream> also the static rows the main view did not draw (aux rays,
    // sscombatray.cpp; clean_pick_select.comp job.w bit 2)
    static constexpr uint32_t MODE_ALL_STATIC = 4u;
};
// One fragment of the raster pick (clean_pick_common.glsl PickFrag).
struct LLVKPickFrag
{
    static constexpr uint32_t ALPHA_PASS = 1u;   // the raster keeps it (the draw's alpha rule)
    static constexpr uint32_t BACK_FACE  = 2u;   // back face of a double-sided face
    static constexpr uint32_t DERIV_TAN  = 4u;   // no stream tangent: from screen derivatives
    uint32_t instance = 0xFFFFFFFFu;   // clean Instance id
    uint32_t flags = 0;
    float    alpha = 1.f;              // effective alpha (factor x tint x texel)
    float    t = 0.f;                  // along the layer's request ray (m; HUD: HUD units)
    float    pos[3] = {};              // agent space (HUD layer: HUD space)
    float    normal[3] = {};           // interpolated normal, unit
    float    tangent[4] = {};          // the vertex stream's tangent, unit; w bitangent sign
    float    st[2] = {};               // raw texcoord0 (LL's surface ST, before texgen)
    float    uv[2] = {};               // the UV the raster samples (texgen + TE / glTF / anim)
    float    duv[4] = {};              // uv per screen pixel: du/dx, dv/dx, du/dy, dv/dy
};
struct LLVKPickResult
{
    static constexpr uint32_t kHistWorld = 64, kHistHud = 32;   // clean_pick_common.glsl
    bool     valid = false;
    LLVKPickRequest req;               // the request it answers
    uint32_t cand_static = 0, cand_rigged = 0, cand_hud = 0;   // candidate draws
    // A candidate list passed its cap: instances crossing the ray inside the window were
    // never drawn, so the layer is incomplete (the CPU narrows the window, #704).
    bool     cand_overflow_world = false;   // the STATIC world list
    bool     cand_overflow_hud = false;
    // The RIGGED world list passed its cap: a subset of the rigged draws ran. Not a reason
    // to void the layer -- wearers whose box is unknown / absurd (attachments still rezzing
    // after a teleport, extents lagging) pass the select for EVERY window, so narrowing
    // can't resolve it, and voiding the layer left nothing in the world clickable until
    // their boxes settled. The statics are complete; avatars also answer by their collision
    // volumes (llcleancursorpick.cpp).
    bool     cand_overflow_rigged = false;
    bool     overflow_world = false;   // more world fragments in the window than the list holds
    bool     overflow_hud = false;
    uint32_t n_world = 0, n_hud = 0;   // fragments in the windows (stored: min(n, cap))
    // The windows the answer is EXACT for (world lo / hi, HUD lo / hi): the request's, or
    // nearer when the GPU's count-then-fill round narrowed an overflowing layer.
    float    win[4] = {-1.f, 1.0e30f, -1.f, 1.0e30f};
    uint32_t hist_world[kHistWorld] = {};   // in-window fragments per depth bin
    uint32_t hist_hud[kHistHud] = {};
    std::vector<LLVKPickFrag> world;   // unsorted (empty when the layer overflowed)
    std::vector<LLVKPickFrag> hud;
};
// DRAWN-GEOMETRY QUERIES (#704): what the build tools asked the CPU geometry mirror for,
// answered from the geometry the renderer draws (clean_pick_query.comp) -- the instance's
// drawn slot (the main view's LOD choice), else its highest resident level. Per instance:
struct LLVKFaceQueryResult
{
    bool     valid = false;            // a resident slot was found
    float    normal_sum[3] = {};       // the vertex normals summed (volume space): the face's direction
    uint32_t verts = 0;
    float    normal0[3] = {};          // the first vertex's normal and tangent (volume space;
    float    tangent0[4] = {};         // w handedness, all 0 = none)
    bool     any_in = false;           // rect query: a vertex projects inside the rectangle
    bool     any_out = false;          // ... outside it, or lies behind the camera
};
// ACOUSTICS PROBE (#683 phase 0, AudioAcoustics; vkgfx/llcleanaudiortvk.h). Per frame the
// renderer traces, against its scene TLAS (ray tracing on only), kRoomRays rays over the
// sphere from the listener and one listener -> source ray per listed sound source, and
// answers ~3 frames later (the frame slot's fence). newview/llcleanacoustics.h turns the
// answers into the EFX room reverb + per-source low-pass.
//
// #858 DIFFRACTION: on room frames every listener room ray's hit point gets one shadow ray to
// each listed occluded source (diff_src, <= kMaxDiffSources): next-event estimation of a
// one-bounce path around a corner / through a doorway.
// #857 / #858 PATH QUERIES: a CPU-built list (<= kMaxQueries, extra workgroups only as
// needed) of RAY queries (trace from a point along a direction, then one visibility ray from
// the end point to a target: the source-room probe, guided path discovery) and SEGMENT
// queries (is a -> b clear: cached-path re-validation, two-bounce joins).
struct LLVKAcousticsQuery
{
    float a[4] = {};   // xyz origin; w > 0: RAY range (m), <= 0: SEGMENT a -> b
    float b[4] = {};   // xyz visibility target; w end margin at the target (m)
    float d[4] = {};   // RAY: xyz unit direction, w tmin (m)
    float e[4] = {};   // RAY: x miss end distance (m)
};
struct LLVKAcousticsQueryHit
{
    float    dist = -1.f;   // RAY hit distance (< 0 miss); SEGMENT -1
    float    vis = -1.f;    // |P - b| when P -> b is clear, < 0 blocked
    uint32_t inst = 0xFFFFFFFFu;
    float    hint = 0.f;
    uint8_t  kind = 0;
    float    p[3] = {};     // the tested point P (agent space)
    float    snow = 0.f;      // #930: RAY hit's lying-snow share (0 bare .. 1, x the strength)
    float    snow_pack = 0.f; // #930: ... and how packed it is (0 fresh .. 1 packed)
};
struct LLVKAcousticsRequest
{
    static constexpr uint32_t kMaxSources = 64;   // clean_audio_rt.comp AC_MAX_SOURCES (30 channels + 34 channel-less: #892 speakers (up to 32), #783 thunder)
    static constexpr uint32_t kMaxDiffSources = 4;      // clean_audio_rt.comp AC_MAX_DIFF
    static constexpr uint32_t kMaxQueries = 128;        // clean_audio_rt.comp AC_MAX_QUERIES
    static constexpr uint32_t kNone = 0xFFFFFFFFu;      // an unused diff_src entry
    bool     valid = false;
    uint32_t serial = 0;              // echoed in the answer (0 = never)
    float    listener[3] = {};        // agent space
    float    room_range = 30.f;       // room ray length (m)
    uint32_t room_interval = 3;       // room rays every N frames (1 = every frame); occlusion rays every frame
    float    foliage_test_dist = 15.f;   // beyond this, room rays take alpha-mask hits without the alpha test (m)
    bool     alpha_blocks = true;     // AudioAcousticsAlphaBlocks: visible alpha-blend blocks (room + occlusion), alpha-mask occludes
    uint32_t source_count = 0;        // <= kMaxSources
    float    sources[kMaxSources][4] = {};   // xyz agent space, w end margin (m: the emitter's own size)
    uint32_t diff_src[kMaxDiffSources] = {kNone, kNone, kNone, kNone};    // #858: indices into sources (used on room frames)
    uint32_t query_count = 0;                 // <= kMaxQueries
    LLVKAcousticsQuery queries[kMaxQueries];  // #857 / #858 path queries (agent space)
    // #930 LYING SNOW (AudioAcousticsSnowAbsorb, only while snow lies and the weather is on):
    // > 0 = every committed hit (room rays, RAY queries, bounce hits) looks up the weather's snow
    // grids; the bounce pass blends toward bounce_class[10] (fresh) / [11] (packed). 0 = today's probe.
    float    snow_strength = 0.f;
    float    snow_depth = 0.f;        // the global (undisturbed, open-ground) depth (m): the compaction proxy's reference

    // #683 PHASE 1 BOUNCE PASS (AudioAcousticsBounce; clean_audio_rt.comp BOUNCE PASS): multi-bounce
    // energy tracing from the listener (set 0) and up to kMaxBounceSets - 1 sources (#857: their
    // own rooms), on the room_interval cadence one frame after the room rays. Answered as
    // LLVKAcousticsBounce (per set: a 3-band echogram, early / late arrivals and directions, the
    // energy balance), separately from LLVKAcousticsResult.
    static constexpr uint32_t kMaxBounceSets = 5;        // AC_MAX_BOUNCE_SETS
    static constexpr uint32_t kMaxBounceWorkgroups = 32; // AC_MAX_BOUNCE_WG (64 rays each, all sets)
    static constexpr uint32_t kMaxBounceListenerRays = 1024;
    static constexpr uint32_t kMaxBounceSourceRays = 256;
    static constexpr uint32_t kMaxBounces = 8;           // AC_MAX_BOUNCES
    static constexpr uint32_t kBounceClasses = 16;       // AC_CLASSES (ACC_* in the shader)
    static constexpr uint32_t kBounceMatMax = 192;       // listed instances (the 256-slot hash: load <= 0.75)
    // A source set: rays from the source. Room-decay sets (#857) have the source as their
    // receiver too; EARLY-GAIN sets (#683 early gain, to_listener) have the LISTENER as receiver:
    // their arrivals are the reflected energy reaching the listener from that source (own bounce
    // cap, own receiver radius; the shader leaves out the first-order floor / ground reflection).
    struct BounceSource
    {
        uint32_t src = kNone;        // index into sources (the answer echoes it)
        float    pos[3] = {};        // agent space (the origin; the receiver too unless to_listener)
        float    margin = 0.5f;      // the emitter's own size: first segment's tmin (and the receiver's end margin when it is the source)
        uint32_t rays = 128;         // rounded up to 64, <= kMaxBounceSourceRays
        bool     to_listener = false;   // early gain: the receiver is `listener`
        float    recv_r = 0.f;          // receiver radius (m), 0 = receiver_radius
        uint32_t bounces = 0;           // this set's bounce cap, 0 = bounce_max
    };
    bool     bounce = false;
    bool     bounce_listener = true;          // trace the listener's own set (AudioAcousticsBounce); off: only source sets
    uint32_t bounce_rays = 512;               // listener rays (rounded up to 64, <= kMaxBounceListenerRays)
    uint32_t bounce_max = 6;                  // bounces per ray (1 .. kMaxBounces)
    float    bounce_range = 60.f;             // ray range (m): beyond it a ray escapes
    float    receiver_radius = 1.f;           // m (diffuse rain solid angle / specular detection)
    float    hard_abs = 0.03f;                // a full hardness hint pulls absorption down to this
    float    polished_scatter = 0.05f;        // ... and scattering down to this
    float    air[3] = {};                     // air absorption per band (energy, 1/m): low / mid / high
    float    speed = 343.f;                   // m/s
    float    bounce_class[kBounceClasses][4] = {};   // absorption low / mid / high, scattering (0..1)
    uint32_t bounce_mat_count = 0;                    // <= kBounceMatMax
    uint32_t bounce_mat_inst[kBounceMatMax] = {};     // clean Instance id ...
    uint8_t  bounce_mat_class[kBounceMatMax] = {};    // ... -> class (the prim's physical material)
    uint32_t bounce_src_count = 0;                    // <= kMaxBounceSets - 1 (room-decay and early-gain sets)
    BounceSource bounce_src[kMaxBounceSets - 1];
};

// One bounce ray set's answer (#683 phase 1), summed over its workgroups. Energies in ray units
// (each ray starts at 1; divide by `rays` for per-ray values). See clean_audio_rt.comp BOUNCE PASS.
struct LLVKAcousticsBounceSet
{
    static constexpr uint32_t kBins = 256;        // AC_BINS: 5 ms (0 .. 1.28 s)
    static constexpr uint32_t kEarlyBins = 128;   // AC_EBINS: 1 ms, orders 1-2 (last bin: everything later)
    static constexpr uint32_t kLateBins = 64;     // AC_LBINS: 5 ms, orders >= 3 (last bin: everything later)
    static constexpr uint32_t kCapBins = 64;      // AC_CBINS: 20 ms, rays ended by the bounce cap
    static constexpr float kBinS = 0.005f, kEarlyBinS = 0.001f, kLateBinS = 0.005f, kCapBinS = 0.02f;
    uint32_t src = 0xFFFFFFFFu;   // the request's source index (0xFFFFFFFF: the listener)
    float    origin[3] = {};
    bool     to_listener = false; // an early-gain set (#683): origin = the source, receiver = recv
    float    recv[3] = {};        // the receiver (= origin unless to_listener)
    float    recv_r = 1.f;        // its radius (m): the free-field reference uses the same sphere
    uint32_t bounces = 0;         // the cap this set ran with
    uint32_t rays = 0;            // traced (the counter)
    float    hist[3][kBins] = {};   // per band (low / mid / high), arrivals by time
    float    early[kEarlyBins] = {};
    float    late[kLateBins] = {};
    float    cap[kCapBins] = {};
    float    early_dir[3] = {};     // energy-weighted arrival directions (from the receiver), orders 1-2
    float    late_dir[3] = {};      // ... orders >= 3
    float    e_pre[3] = {}, e_abs[3] = {}, e_esc[3] = {}, e_air[3] = {};   // per band: at hits, absorbed, escaped, air
    float    hits = 0.f, escapes = 0.f, capped = 0.f, nee_vis = 0.f, spec = 0.f;
    float    seg_len = 0.f;         // m, the hit segments' total length
};
struct LLVKAcousticsBounce
{
    bool     valid = false;
    uint32_t serial = 0;            // the request it answers
    uint32_t frames_old = 0;
    uint32_t bounces = 0;           // the cap it ran with
    uint32_t workgroups = 0;
    float    range = 0.f;
    float    receiver_radius = 1.f;
    float    air[3] = {};
    float    speed = 343.f;
    uint32_t set_count = 0;
    LLVKAcousticsBounceSet set[LLVKAcousticsRequest::kMaxBounceSets];
};
struct LLVKAcousticsResult
{
    static constexpr uint32_t kRoomRays = 64;     // clean_audio_rt.comp AC_ROOM_RAYS
    bool     valid = false;
    uint32_t serial = 0;              // the request it answers
    uint32_t frames_old = 0;          // renderer frames between the request and its harvest
    float    listener[3] = {};        // the request's listener
    float    room_range = 0.f;
    uint32_t room_serial = 0;         // the request whose room rays the room_* arrays hold (0 = none yet)
    float    room_dist[kRoomRays] = {};   // hit distance (m), < 0 = miss within room_range
    float    room_dir_z[kRoomRays] = {};  // the ray's world z (-1 down .. 1 up)
    uint32_t room_inst[kRoomRays] = {};   // the hit's clean Instance id (0xFFFFFFFF: miss / unresolvable)
    float    room_hint[kRoomRays] = {};   // render-material hardness hint 0..1 (metal / polished / env), -1 terrain, -2 water (#953)
    float    room_clutter[kRoomRays] = {};   // geometric clutter score 0..1 (< 0: no enclosing neighbour triangle)
    uint8_t  room_kind[kRoomRays] = {};      // 1 = alpha-mask geometry (foliage cut-out, alpha-tested), 2 = visible alpha-blend (glass), 0 = other / miss
    uint32_t source_count = 0;
    float    occ_dist[LLVKAcousticsRequest::kMaxSources] = {};   // opaque / terrain blocking hit (m) on the source ray, < 0 = clear
    // Alpha blocks: the nearest visible alpha-blend / alpha-mask surface the source ray crosses
    // (clean Instance id, 0xFFFFFFFF none); the CPU weighs them by the prim's physical material.
    uint32_t occ_blend_inst[LLVKAcousticsRequest::kMaxSources] = {};
    uint32_t occ_mask_inst[LLVKAcousticsRequest::kMaxSources] = {};
    // With the room rays (carried forward like them): the ray's world x / y (with room_dir_z the
    // full direction, so the hit point is known: #858), the room request's diff_src, and per
    // room ray and diff_src entry the shadow ray's answer: |hit point - source| when the source
    // is visible from the hit point, < 0 = blocked / no hit / unused.
    float    room_dir_x[kRoomRays] = {};
    float    room_dir_y[kRoomRays] = {};
    uint32_t diff_src[LLVKAcousticsRequest::kMaxDiffSources] = {};
    float    room_nee[kRoomRays][LLVKAcousticsRequest::kMaxDiffSources] = {};
    // #857 / #858 path queries of THIS answer's request (never carried forward), in request
    // order; the requester keeps what each one asked.
    uint32_t query_count = 0;
    LLVKAcousticsQueryHit query[LLVKAcousticsRequest::kMaxQueries];
    // #930 (with the room rays, carried forward like them): per room ray the hit's lying-snow
    // share (0 bare .. 1, x the strength; 0 without snow) and how packed it is (0 fresh .. 1).
    float    room_snow[kRoomRays] = {};
    float    room_snow_pack[kRoomRays] = {};
};
// The rectangle test (LLToolSelectRect's screen projection; agent space, window pixels).
struct LLVKRectQuery
{
    float eye[3] = {}, at[3] = {}, left[3] = {}, up[3] = {};
    float tan_x = 1.f, tan_y = 1.f;   // tan of the half fields of view
    float viewport[4] = {};           // x, y, width, height
    float rect[4] = {};               // left, right, bottom, top
};

// GPU CURSOR PICK OF PARTICLES (#481): the particles exist only in the GPU pool, so the
// renderer tests the pick segment against every live particle's drawn quad (LLVKPickRequest:
// origin / dir / length / alpha_threshold / serial; < 0 threshold = no texture test) and
// hands back the nearest. See vkgfx/llcleanpickvk.h and llcleanpartpick.h.
struct LLVKPartPickResult
{
    bool     valid = false;
    uint32_t serial = 0;
    float    origin[3] = {}, dir[3] = {};
    float    length = 0.f;
    bool     hit = false;
    uint32_t index = 0xFFFFFFFFu;      // pool slot
    uint32_t emitter = 0xFFFFFFFFu;    // emitter slot (LLCleanParticleStore)
    uint32_t source_id = 0;            // LLViewerPartSource mID, baked at emit
    float    t = 0.f;                  // along the request ray (m)
    float    pos[3] = {};              // agent space
};

// World render inputs. Filled by the caller (from LLViewerCamera) each frame and
// consumed by the GPU-driven static-opaque world pass inside renderFrame(). Kept
// as plain floats so this header stays free of Vulkan/glm/matrix types.
// One local light for the VK world (point light or projector), world space. Built
// on the CPU from LL's light list each frame (buildVulkanWorldFrame); the renderer
// resolves tex_uuid to a bindless index and clusters the list on the GPU.
struct LLVKLight
{
    float   pos_radius[4];    // world pos, LL deferred radius (getLightRadius * 1.5)
    float   color_falloff[4]; // linear rgb (intensity folded in), falloff
    float   proj_params[4];   // projector: focus, range (far - near), lod range, ambiance
    float   proj_origin[4];   // projector apex (world) .xyz; .w = 1 projector, 0 point
    float   proj_axis[4];     // projector forward axis (world) .xyz, .w near clip (apex distance)
    float   proj_mat[16];     // world -> projector texcoord (LL setupSpotLight, world space)
    uint8_t tex_uuid[16];     // projector texture UUID (all zero: white)
    // Projector shadows: world -> VK clip of the projector's frustum (same fov / aspect /
    // near / far as proj_mat, rendered and sampled with this one matrix), the tangents
    // of its half-angles (x, y: texel size at a distance), and a stable identity.
    float    shadow_vp[16] = {};
    float    shadow_tan[2] = {};
    float    shadow_near = 0.f;   // near-plane distance from the apex (ray-traced shadows stop there)
    uint64_t key = 0;
    // (#980, the GI changed-light log only) 1 = worn (an attachment), 2 = an active drawable (its pose
    // is re-read every frame: scripted motion, physics, target omega, a selection)
    uint8_t  dbg_flags = 0;
};

// One MANUAL reflection probe object for the VK world (LLVOVolume reflection-probe
// params; mirrors go to the hero path). Agent space. The default sky probe, the terrain
// grid and the automatic (level 3) probes are made on the GPU, which also assigns the
// cube slots and schedules every capture (#478, LLCleanRendererVK::setProbes /
// clean_probe_gpu.comp); the renderer keeps each manual probe at a stable table position
// by key.
struct LLVKProbe
{
    uint64_t key;          // stable identity (the object's UUID, bit 63 clear)
    float    sphere[4];    // agent origin .xyz, influence radius .w
    float    box[16];      // world -> unit cube (box probes)
    float    ambiance;     // probe ambiance (0..)
    float    near_clip;    // capture near plane
    uint8_t  is_box, manual, dynamic, parallax_sphere;
};

// One region's data for the GPU probes (#478): where it is in the world (global 16 m
// cells: region origins are multiples of 256 m) and its terrain -- the 8 x 8 terrain /
// water grid probes' heights (LLViewerRegion::updateReflectionProbes: max(water, land) +
// 2 at (16 + 32 i, 16 + 32 j)) and a 17 x 17 land-height lattice (the automatic probes are
// lifted above the land). `version` changes when the heights were re-sampled; the
// renderer uploads a region only then (and when it appears).
struct LLVKProbeRegion
{
    uint64_t handle = 0;
    int32_t  cell[2] = {0, 0};   // SW corner in global 16 m cells
    int32_t  width_cells = 16;   // region width / 16
    uint32_t version = 0;
    float    water = 0.f;        // water height
    float    spacing = 16.f;     // land lattice spacing (width / 16)
    float    grid_z[64] = {};    // [i * 8 + j]
    float    land[289] = {};     // [y * 17 + x]
};

// #1075 LAYERED STEREO (research/VR_LAYERED_STEREO_DESIGN.md §1, step 1 "frame split"): one
// VIEW of the frame -- an eye in VR, the camera on the desktop. LLVKWorldFrame keeps everything
// shared by the views (the head / union camera: cull frustum, LOD, lights, cascades, probes,
// atmosphere) in its own fields; views[v] carries what differs per view. Matrices follow the
// shared fields' conventions (column-major, VK clip with the Y flip, reversed-Z with the same
// near / far). The camera-relative forms map (world - cam_pos.xyz): ONE origin for every view.
struct LLVKViewFrame
{
    float view[16]      = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // world -> view (this eye)
    float proj[16]      = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // view -> clip (off-axis in VR)
    float inv_proj[16]  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // world -> clip
    float inv_view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float view_proj_rel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // (world - cam_pos) -> clip
    float inv_view_proj_rel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float eye[4]  = {};               // .xyz this view's eye (world); the shading eye of its pixels
    float tan[4]  = {-1.f, 1.f, -1.f, 1.f};   // FOV tangents: left, right, down, up (left / down < 0)
};

struct LLVKWorldFrame
{
    bool  enabled = false;         // render the 3D world this frame?
    // #1075 LAYERED STEREO: the views of this frame (1 on the desktop: views[0] = the camera,
    // the same numbers as the shared fields below; 2 in VR stereo: the eyes, the shared fields
    // being the head / union camera). kMaxViews = LLCleanRendererVK::kMaxViews.
    static constexpr uint32_t kMaxViews = 4;
    uint32_t view_count = 1;
    LLVKViewFrame views[kMaxViews];
    // #1075: views[] are the headset's located eyes this frame (VR stereo; false on the desktop,
    // the debug stereo mode, and VR frames whose eyes were not located: every view = the head).
    bool views_are_eyes = false;
    float view_proj[16];            // column-major MVP (world -> clip, VK NDC)
    float frustum_planes[6][4];     // ax+by+cz+d >= 0 => inside
    float cam_pos[4];               // .xyz camera world pos, .w LOD factor
    float lod_cam_pos[4];           // .xyz main-camera pos (LOD distance)

    // Lighting / sky (from LLEnvironment). Sourced each frame so the VK world
    // and the sky gradient track the real day cycle. Defaults are a plausible
    // noon so an empty/early frame still looks like daylight (never all-black).
    float sun_dir[4]      = {0.40f, 0.80f, 0.50f, 0.0f}; // .xyz world sun dir (normalized)
    float sun_color[4]    = {1.00f, 0.97f, 0.90f, 0.0f}; // sunlight RGB
    float ambient_color[4]= {0.30f, 0.33f, 0.40f, 0.0f}; // ambient RGB
    float sky_horizon[4]  = {0.62f, 0.72f, 0.86f, 0.0f}; // sky gradient at horizon
    float sky_zenith[4]   = {0.18f, 0.35f, 0.66f, 0.0f}; // sky gradient at zenith
    float time            = 0.0f;                        // seconds, for water wave animation

    // Water depth-fog. inv_view_proj un-projects the sampled scene depth back to
    // world space in the water FS; water_fog is the LLEnvironment water fog colour
    // (.rgb) + density (.w). Identity default so an early frame is a harmless no-op.
    float inv_view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float water_fog[4]      = {0.02f, 0.12f, 0.18f, 0.03f}; // .rgb colour, .w density
    // WindLight atmosphere + post parameters; layout in clean_atmos_common.glsl.
    float atmos[64]         = {};
    // Water surface parameters (clean_water.frag layout) + the two normal-map UUIDs
    // (resolved to bindless indices by the renderer into water[21], water[22]).
    float   water[24]         = {};
    uint8_t water_normal[2][16] = {};
    // Cloud layer (LL cloudsV/F inputs; layout: clean_clouds.glsl). [0] cloud colour.rgb,
    // cloud scale  [1] pos/density1 (+ scroll), variance  [2] pos/density2, noise blend
    // [3] noise idx A, B (renderer fills from cloud_noise), 0, enabled.
    float   clouds[16]        = {};
    uint8_t cloud_noise[2][16] = {};
    // EEP heavenly bodies, stars, rainbow/halo (layout: clean_sky_bodies.glsl) + the
    // six sky textures (sun A, sun B, moon, bloom, rainbow, halo); the renderer fills
    // their bindless indices into bodies[32..37].
    float   bodies[40]        = {};
    uint8_t sky_tex[6][16]    = {};

    // HUD attachments: drawn in a separate pass after the world with LL's HUD
    // camera (get_hud_matrices: ortho * HUD model), converted to Vulkan clip space.
    bool  hud_enabled = false;
    float hud_view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // LL's world-view rectangle in framebuffer pixels, top-left origin {x, y, w, h}:
    // the HUD (like LL's setup3DViewport) renders into it, and LL's HUD mouse pick
    // (mousePointHUD) is centred on it. 0 width => full framebuffer.
    int   hud_viewport[4] = {0, 0, 0, 0};
    // For LL's immediate-mode edit overlays (manipulators) drawn in the full-window
    // UI frame: HUD projection with the world-view viewport baked in, and the HUD
    // modelview (get_hud_matrices), loaded into gGL separately.
    float hud_proj_ui[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float hud_model[16]   = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // Edit-mode selection outline: clean instance ids of the selected faces, each one's
    // kind (1 = root prim, 0 = child prim, 2 = hover preview from the build floater's
    // links / faces list, #252), LL's silhouette colours, and whether occluded edges show.
    std::vector<uint32_t> sel_ids;
    std::vector<uint8_t>  sel_root;
    float sel_parent_color[4] = {1.f, 1.f, 0.f, 1.f};
    float sel_child_color[4]  = {0.f, 0.5f, 1.f, 1.f};
    float sel_hover_color[4]  = {1.f, 0.35f, 1.f, 1.f};
    bool  sel_hidden = true;
    // Build > Options > Hide Selection (HideSelectedObjects, #29; LL markVisible): clean
    // instance ids of every face of every selected object, dropped by every cull pass.
    std::vector<uint32_t> hide_ids;
    // Reflection probes (#478): the manual probe objects and the regions' terrain data; the
    // GPU makes the rest (default, terrain grid, automatic) and schedules the captures.
    std::vector<LLVKProbe> probes;               // manual probe objects
    std::vector<LLVKProbeRegion> probe_regions;  // regions (any order; keyed by handle)
    int32_t probe_agent_cell[2] = {0, 0};        // the agent frame's origin (its region's SW corner), 16 m cells
    bool  probes_enabled   = false;
    float probe_sky_ambiance = 0.f;   // LLSettingsSky::getReflectionProbeAmbiance
    int   probe_detail     = 1;       // RenderReflectionProbeDetail (3 = Raytraced: realtime cadence, traced faces)
    int   probe_level      = 3;       // RenderReflectionProbeLevel
    // Local lights (nearest first, capped), see LLVKLight, plus the camera terms the
    // clustered light cull needs (world->view matrix, tan(half fov), near/far, fwd).
    std::vector<LLVKLight> lights;
    // (#91 LIGHT TAKEOVER, RenderVKReSTIREmissiveLightTakeover) the faces of each kept POINT light's own
    // prim whose material may emit (PBR emissive factor, or legacy fullbright + glow; not attached, not a
    // projector): (clean instance id, index into `lights`). The GPU decides per frame whether they take the
    // light over (clean_regir.glsl LIGHT TAKEOVER); the renderer sorts and caps the table.
    std::vector<std::pair<uint32_t, uint32_t>> light_takeover;
    float view[16]     = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float tan_half[2]  = {0.7f, 0.4f};   // x, y
    float cluster_near = 0.25f, cluster_far = 256.f;
    float cam_fwd[3]   = {1.f, 0.f, 0.f};
    // P * view WITHOUT the eye translation: projects eye-relative (camera-relative)
    // positions -- the rigged pre-skin output.
    float view_proj_rel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Its inverse (VK clip -> camera-relative world): the sky's view rays (#555). The
    // full inverse_view_proj carries the ~km eye in its translation; un-projecting the
    // sky through it and subtracting the eye lost ~2-7 px of direction at Z 3000-8000
    // (stars jumping), where this one is altitude independent.
    float inv_view_proj_rel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Projection (VK clip, Y-flipped) and its inverse: SSR reprojection into the
    // previous frame's view space.
    float proj[16]     = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float inv_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Sun/moon shadow cascades (LL generateSunShadow splits, stable ortho fit):
    // world -> shadow NDC per cascade (xy [-1,1] -> map uv, z [0,1]), the cascade cull
    // planes (near pulled toward the light so casters behind the camera survive),
    // cascade far splits as view depths, world texel size, and sampling params.
    bool  shadow_enabled = false;
    // CAMERA-RELATIVE (#555): shadow_mat[c] maps (world - cam_pos.xyz), not world, so the
    // ~km agent-space translation never enters float math on the GPU (at Z 3000 the
    // world-space form's receiver-vs-caster depth noise exceeded the depth bias:
    // flickering acne on sun-facing floors). The renderer rebases cached cascades.
    float shadow_mat[4][16] = {};
    float shadow_planes[4][6][4] = {};
    float shadow_clip[4] = {};
    float shadow_texel[4] = {};
    // Shadow caching (RenderVKShadowUpdate, #98): the cascade's true view-slice sphere
    // (agent space xyz + radius, before the caching pad) so the renderer can test
    // whether a CACHED cascade map still covers this frame's slice; and the mode
    // (0 realtime, 1 balanced, 2 performance).
    float shadow_slice[4][4] = {};
    unsigned shadow_update = 0;
    // Per-cascade SHADOW-CASTER planes: the planes of the cascade's view-frustum slice
    // that a caster's shadow can't cross (normal not along the light's travel), so a
    // caster must already be inside them to shade anything visible. Unused planes are
    // (0,0,0,1) = always inside.
    float shadow_caster_planes[4][6][4] = {};
    float shadow_bias = 0.f, shadow_offset = 0.01f;
    // Planar MIRROR (LL hero reflection probe; the nearest visible mirror probe).
    // The capture renders the world with the view reflected about the mirror plane
    // (det -1 -> clockwise front faces) and an oblique near plane on the mirror, into
    // an HDR target the main view samples at the same screen pixel.
    bool  mirror_valid = false;
    // Mouselook with "Show me in Mouselook" off (LLVOAvatar::sVisibleInFirstPerson):
    // hide the agent's own avatar (rigged + worn attachments) in the MAIN view only;
    // mirrors, reflection probes and shadows still draw it.
    bool  hide_self = false;
    // !gAgent.needsRenderHead() (mouselook with "Show me in Mouselook" on, RLVa
    // showselfhead): hide only the own system-avatar HEAD parts in the main view.
    bool  hide_self_head = false;
    float mirror_view_proj[16] = {};       // oblique P * V * R (world -> clip)
    float mirror_view_proj_sky[16] = {};   // non-oblique P * V * R (sky ray setup)
    float mirror_view_proj_sky_rel[16] = {};   // the same without the translation: maps
                                               // (world - mirror_cam) (#555, sky rays)
    float mirror_view_proj_rel[16] = {};   // oblique, maps (world - mirror_cam) (#555, draws)
    float mirror_frustum[6][4] = {};       // cull planes (near = the mirror plane)
    float mirror_cam[4] = {};              // reflected eye (.xyz)
    float mirror_plane[4] = {};            // world plane n.xyz, d (viewer side > 0)
    float mirror_box[16] = {};             // world -> mirror probe box [-1,1]^3
    float mirror_res_scale = 1.f;          // capture size / main size
    uint32_t mirror_max_res = 1024;        // RenderHeroProbeResolution: capture long-side cap (px)
    bool  mirror_dynamic = false;          // the mirror's Dynamic flag (LL hero detail rule): avatars drawn
    bool  mirror_static = false;           // camera + mirror unchanged since last frame
    // Mirror identity + selection metrics (occlusion-query gate + change logging).
    uint64_t mirror_key = 0;               // object identity (LLViewerObject pointer); 0 = none
    uint32_t mirror_local_id = 0;          // LL local ID (log)
    uint8_t  mirror_uuid[16] = {};         // object UUID bytes (log)
    float    mirror_dist = 0.f;            // eye -> mirror centre (m)
    float    mirror_size = 0.f;            // bounding-sphere diameter (m)
    float    mirror_px = 0.f;              // estimated on-screen size (px, from the sphere)
    bool     mirror_cam_inside = false;    // eye inside the (near-clip inflated) probe box

    // Depth of field (LL renderDoF; focus + lens terms computed in llappviewer.cpp):
    // dof_lens = focal distance (view z, m, negative), blur constant, tan pixel angle,
    // magnification; dof_params = max CoF (px), resolution scale.
    bool  dof_enabled = false;
    float dof_lens[4]   = {};
    float dof_params[4] = {};

    // RLVa @setsphere vision restriction (not a graphics option: no setting gates it).
    // The enabled effects in LLVfxManager order (RlvSphereRenderParams); origin is agent
    // space, values = value min, dist min, value max, dist max.
    struct RlvSphere { float origin[3]; int mode; float values[4]; float params[4]; bool extend_min, extend_max; };
    static constexpr uint32_t kMaxRlvSpheres = 8;
    RlvSphere rlv_spheres[kMaxRlvSpheres] = {};
    uint32_t  rlv_sphere_count = 0;

    // Temporal history reset (#84): the previous frame is no reference for this one
    // (teleport in progress / just arrived). The renderer also resets on its own on an
    // agent origin shift (region crossing) and on a camera cut.
    bool  temporal_reset = false;

    // <GMDX> REGION MAP CAPTURE (gmdxregioncapture.h): an orthographic view straight down. The cull
    // takes every LOD at its finest and sizes textures per metre instead of per distance
    // (clean_cull.comp CULL_ORTHO); ortho_ppm > 0 is that texel demand in px per metre, whatever
    // the size of this render (0 = its own). Offscreen renders only (renderOffscreen).
    bool  ortho = false;
    float ortho_ppm = 0.f;
    // <GMDX> The live orthographic view (LLViewerCamera::getOrtho, buildVulkanWorldFrame): the
    // cull / LOD / texel demand of `ortho` above at the projection's own px per metre, and
    // tan_half carries the half extents in metres; none of the map capture's clean-plate rules.
    bool  ortho_view = false;
    // <GMDX> AVATAR MATTE SNAPSHOT (gmdxsnapshotavatars.h): an avatar layer -- the recordings after
    // the warm-up keep avatars only in the main view and read their coverage back
    // (LLCleanRendererVK::snapMatte*; LLVulkanContext::takeSnapshotMatte). Offscreen renders only.
    bool  avatar_matte = false;

    // Cursor pick ray for the GPU raster pick (#702; LLCleanCursorPick::fillRequest).
    LLVKPickRequest pick;
    // Cursor pick ray for the particle GPU pick (#481; LLCleanPartPick::fillRequest):
    // valid only on frames something asked for a per-frame particle answer.
    LLVKPickRequest part_pick;
    // <SS:Skoomastream> Extra world-only rays answered like the cursor ray, ~2 frames
    // late (sscombatray.cpp: OTS camera collision, aim convergence, line of sight).
    static constexpr uint32_t kAuxPicks = 8;
    LLVKPickRequest aux_pick[kAuxPicks];
    // Acoustics probe (#683, LLCleanAcoustics::fillRequest): valid only while AudioAcoustics
    // is on and the audio engine's EFX path is active.
    LLVKAcousticsRequest acoustics;
};

// VRAM budget summary (#405 / #407) for code outside the renderer (Preferences, bug
// reports, the RAM texture tiers): plain numbers, no Vulkan types. mode: 0 calm,
// 1 soft pressure (the ladder steps down at normal pace), 2 inside the 10% reserve
// (fast). rung / step: how far the soft ladder holds things back (0 = nothing).
struct LLVKVramSummary
{
    bool     valid = false;
    uint64_t capacity = 0, reserve = 0, budget = 0, usage = 0, fixed = 0, overhead = 0;
    // #407: measured fixed term (smoothed), the room it leaves the scalable consumers,
    // swapchain (measured), VRAM suballocator slack, driver page padding, unattributed.
    uint64_t fixedSmoothed = 0, scalable = 0, swapchain = 0, slack = 0, pad = 0, unattributed = 0;
    uint32_t rung = 0, rungCount = 0, step = 0, mode = 0;
};

class LLVulkanContext
{
public:
    static LLVulkanContext& instance();
    // Setting specialization (LLCleanRendererVK::specStatusText, llcleanspecvk.cpp): "" when
    // every graphics setting is applied with its specialized pipelines, else the labels of the
    // ones still compiling; fallback = their new values already apply (generic pipelines).
    static std::string specStatusText(bool* fallback = nullptr);

    // Bring up VkInstance (with the surface extensions SDL needs), create the
    // surface from the given SDL_WINDOW_VULKAN window, pick the device, and
    // create the swapchain. Safe to call once. Returns false and logs on any
    // failure (caller keeps the viewer alive / reports). enableValidation turns
    // on the Khronos validation layer.
    bool init(SDL_Window* window, uint32_t width, uint32_t height,
              bool enableValidation);

    void shutdown();

    // #526: after init() returned false, the text for the "can't run here" message box
    // (vkRequirementsUserMessage: the GPUs found, what is missing, what to do) -- empty
    // when the failure happened before Vulkan was reached.
    const std::string& initFailureMessage() const { return mInitFailureMessage; }

    bool isInitialized() const { return mInitialized; }
    // Hardware ray queries available (acceleration structure + ray query extensions):
    // gates the "Ray traced" projector-shadow option. False before init.
    bool rayQuerySupported() const;
    // LL's depth-tested 3D overlays (property/ban lines, parcel highlight, beacons,
    // edit manipulators): gGL drawing between these two calls, issued from the UI
    // render callback, is recorded against the world depth before the HUD pass.
    // beginWorldOverlay() returns false when there's no world pass this frame.
    bool beginWorldOverlay();
    void endWorldOverlay();
    // In-world 2D text (name tags, chat bubbles, prim hover text, HUD effects, #468):
    // gGL drawing between these two calls is recorded after the world overlays and
    // BEFORE the HUD attachment pass, without depth, so HUD attachments cover it as in
    // LL. False when there's no world pass this frame (the caller draws as plain UI).
    // proj / view (#487): the world camera (LLVKWorldFrame proj / view) -- labels
    // anchored with gGL.setLabelAnchor are then occluded by the scene depth as a
    // whole; null keeps the layer depth-free.
    bool beginPreHudLayer(const float* proj = nullptr, const float* view = nullptr);
    void endPreHudLayer();
    // RT extensions actually ENABLED on the device (requested via RenderVKRayTracing
    // before init, AND supported). Every RT feature / UI option gates on this.
    bool rayTracingEnabled() const;
    // BC texture compression usable (RenderVKTextureCompression, #263).
    bool textureCompressionBC() const;
    // One-time hardware default of RenderVKTextureCompression (llappviewer.cpp): the level
    // every BC-capable card gets, 4 = Adaptive (version 1 gave High quality to cards with
    // <= 16 GB and Off above).
    // kTexCompDefaultVersion: raise it to re-apply the default once for every user who has
    // not chosen a level (RenderVKTexCompressionDefaultApplied holds the applied version).
    static constexpr unsigned kTexCompDefault = 4;
    static constexpr unsigned kTexCompDefaultVersion = 2;
    // Human-readable RT state / reason it is off (log, About info, prefs tooltip).
    std::string rayTracingStatus() const;
    void setRayTracingRequested(bool on) { mRayTracingRequested = on; }
    void setClusterRequested(bool on) { mClusterRequested = on; }
    // #870 DLSS Ray Reconstruction through the user-installed slip_nr shim (llvkneuralrecon.h):
    // set before init() -- the shim is loaded before the instance (its extensions).
    // enabled = RenderVKDlssShim; shimPath = RenderVKDlssShimPath (folder or file, may be
    // empty); libDir = the viewer's own library folder; dataDir = a writable folder for NGX.
    void setNeuralReconConfig(bool enabled, const std::string& shimPath, const std::string& libDir,
                              const std::string& dataDir, const std::string& engineVersion,
                              const std::string& defaultDir = std::string())
    {
        mNrDefaultDir = defaultDir;
        mNrEnabled = enabled; mNrShimPath = shimPath; mNrLibDir = libDir; mNrDataDir = dataDir;
        mNrEngineVersion = engineVersion;
    }
    // RenderVKPipelineStats (set before init(): the device extension is chosen at creation).
    void setPipelineStatsRequested(bool on) { mPipelineStatsRequested = on; }
    // RenderVKShaderClockLighting (set before init(): VK_KHR_shader_clock is chosen at creation).
    void setShaderClockRequested(bool on) { mShaderClockRequested = on; }
    // RenderVKClusterDeviceFeatures (#743): which cluster extensions the device enables with
    // RenderVKClusterLOD off at startup (0 none, 1 the inert ones, 2 + mesh shaders). Set before init().
    void setClusterDeviceFeatures(uint32_t level) { mClusterDeviceFeatures = level; }
    // Async compute queue (RenderVKAsyncComputeQueue, phase 3a): set before init().
    void setAsyncComputeMode(uint32_t m) { mAsyncComputeMode = m; }
    // RenderVKAsyncCompute -1 (auto) and RenderVKAsyncComputeQueue at its default: the device
    // applies the per-architecture default (VkDeviceContext::asyncAutoDecide).
    void setAsyncComputeAuto(bool a) { mAsyncComputeAuto = a; }
    // "Async compute: <decision>; queue: <status>" (log, About info, device-lost report).
    std::string asyncComputeSummary() const;
    // RenderVKDebugRobustness (#845 / #917 diagnostic): device-wide robust descriptor access. Before init().
    void setDebugRobustness(bool on) { mDebugRobustness = on; }
    // Cluster LOD (#743): RenderVKClusterForceTier for the startup tier log line (the module
    // re-reads the setting live).
    void setClusterForceTier(int32_t t) { mClusterForceTier = t; }
    // Persistent pipeline cache (RenderVKPipelineCacheDisk): set before init(). An empty
    // path keeps the cache in memory only. buildTag (viewer channel + version) must match
    // the file's for it to load. See vkgfx/vk_pipecache.h.
    void setPipelineCacheFile(const std::string& path, const std::string& buildTag)
    {
        mPipeCachePath = path;
        mPipeCacheTag = buildTag;
    }
    // #471 learned pipeline compile costs (vk_pipecache.h loadCosts): set before init().
    // Empty = learned in memory only for this session.
    void setPipelineCostFile(const std::string& path) { mPipeCostPath = path; }
    // #471 RenderVKPipelineCompileBudgetMB: > 0 replaces the live memory headroom the
    // compile scheduler admits against with a fixed budget (testing a low-memory machine);
    // 0 = automatic. Any time.
    static void setPipelineCompileBudgetMB(uint32_t mb);
    // The world renderer's pipelines are still compiling (background batch, started on
    // the login screen). False once they are ready, and when the world renderer failed
    // to initialise (nothing to wait for). The startup sequence holds the progress
    // screen while this is true so the world never reveals black (llstartup.cpp).
    // Also true while any SHADER-GATE job compiles (vk_pipeline.h vkPipelineGateProgress):
    // the ray-query lit variant, the first-use batches (post effects, god rays, motion blur),
    // the cluster LOD draw pipelines -- everything a world frame would otherwise compile
    // (LLCleanRendererVK::warmFirstUse). Not the setting-specialized sets (the generic
    // pipelines draw the world; a set binds when it lands, without a hitch).
    bool worldPipelinesPending() const;
    // Pipelines finished / queued by every compile batch since launch, and seconds since the
    // last one finished (the login progress screen: progress, and a stalled driver).
    void pipelineCompileProgress(U32& done, U32& queued, F64& idle_sec) const;
    // The same for the shader-gate jobs only (the "Compiling shaders (N / M)" count).
    void shaderGateProgress(U32& done, U32& queued) const;

    // Record + submit + present one frame. Clears the acquired swapchain image to
    // a known color, then (if a UI callback is supplied) runs the viewer's real
    // 2D/UI render through the LLRenderVK facade on top of the clear, and presents.
    // The callback issues gGL (LLRender) calls, which dispatch to the VK backend.
    // Passing an empty callback reproduces the old clear-only present. Returns
    // false on a hard error (already logged); recreates the swapchain on resize.
    // Newest world-frame capture (post tonemap/glow, pre HUD/UI) as bottom-up RGB,
    // for the logout screenshot. False when nothing has been captured yet.
    bool readWorldSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);
    // One RGBA pixel of the newest world-frame capture (world only, refreshed every
    // ~2 s; GL-convention coords, origin bottom-left). No GPU wait. DebugShowColor.
    bool readWorldPixel(int32_t x, int32_t y, uint8_t rgba[4]);
    // User snapshots: request a one-shot capture of the NEXT rendered frame at
    // `point` (1 world only, 2 world + HUD, 3 full frame with UI), render a frame,
    // then read it (waits for the GPU; bottom-up RGB). False if nothing was captured.
    bool requestCapture(uint32_t point);
    bool readCapture(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);

    // LIVE SNAPSHOTS (#873; vkgfx/llcleansnapshotvk.h): the shot comes from the main render
    // path. beginLiveSnapshot arms it (allocating only the shot's buffers, soft: false + `why`
    // when they don't fit -- the caller tries smaller, then the offscreen path); the caller
    // then renders live frames (display()) until liveSnapshotFinished(), and
    // readLiveSnapshot waits for the last frame's fence (no device idle) and returns the
    // image as LLImageRaw RGB (rows bottom-up). Modes: 0 resample (one frame, Lanczos,
    // cropped), 1 depth (point 1 only), 2 jittered accumulation over `frames` live frames.
    struct LiveSnapshot
    {
        uint32_t point = 1;              // 1 world only, 2 world + HUD, 3 the whole frame with UI
        uint32_t mode = 0;               // 0 resample, 1 depth, 2 accumulate
        uint32_t outW = 0, outH = 0;
        float    crop[4] = {0.f, 0.f, 1.f, 1.f};   // normalised source rectangle x0, y0, w, h
        uint32_t frames = 1;             // mode 2
        bool     motionReject = false;   // mode 2: drop moving pixels by motion vectors
        bool     dither = true;          // mode 2: dither the resolved 8-bit output once (#932)
        float    sharpen = 0.f;          // mode 2: CAS strength 0..1 on the resolved mean (#1093); 0 = off
        bool     adaptiveSigma = true;   // mode 2: reconstruction width from the samples per pixel (#1093)
        float    mipBias = 0.f;          // mode 2: texture LOD bias offset while the shot renders (#1093)
        float    depthNear = 0.1f, depthFar = 128.f;   // mode 1: 8-bit range (metres)
    };
    bool beginLiveSnapshot(const LiveSnapshot& req, std::string& why);
    bool liveSnapshotFinished() const;
    bool readLiveSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h, std::string& why);
    void cancelLiveSnapshot();
    // The live frame's size (swapchain extent) and whether its format can be captured.
    bool liveFrameInfo(uint32_t& w, uint32_t& h) const;

    bool renderFrame(const std::function<void()>& uiRender = {},
                     const LLVKWorldFrame* world = nullptr);

    // OFFSCREEN world render (high-res / exact-size snapshots): renders `world` at
    // w x h into a private image (no UI; HUD when world.hud_enabled), `frames` times
    // (warm-up for the history passes), and reads back the last one as bottom-up RGB
    // (+ the raw [0,1] depth, rows top-down, when `depth` is given; needs no HUD).
    // aaFrames > 0 (#417, colour only): supersampled instead -- a warm-up, then aaFrames
    // jittered recordings averaged with equal weights (falls back to `frames` when the
    // accumulation targets don't fit). The live view's temporal state is saved and
    // restored around it. Waits for the GPU; the renderer's targets resize back on the
    // next frame.
    bool renderOffscreen(const LLVKWorldFrame& world, uint32_t w, uint32_t h,
                         std::vector<uint8_t>& rgb, std::vector<float>* depth = nullptr,
                         uint32_t frames = 2, uint32_t aaFrames = 0);
    // <GMDX> The last avatar_matte render's matte (8-bit, LLImageRaw rows bottom-up), once.
    bool takeSnapshotMatte(std::vector<uint8_t>& alpha, uint32_t& w, uint32_t& h);
    uint32_t maxImageDimension() const;
    // Best-case free device-local memory (budget minus usage) -- sizing snapshots.
    uint64_t freeDeviceMemory() const;
    // #730 SNAPSHOT SIZING: an offscreen render at w x h resizes the renderer's
    // resolution-sized images (render targets + RT shadow mask) to w x h. Measured now:
    // their bytes at the live view's size and that size's pixel count (bytes / px is what a
    // snapshot costs per pixel with THESE settings -- RT, SSR, AO, VRS ... included), and
    // the video memory a snapshot may add: the free part of the OS budget or of the
    // renderer's own budget ("Maximum video memory"), whichever is smaller.
    struct SnapshotVram
    {
        uint64_t usable = 0;            // bytes free under both budgets
        uint64_t liveTargetBytes = 0;   // VKMEM_TARGET + VKMEM_RT_MASK, device-local
        uint64_t livePx = 0;            // the live view's pixels (swapchain extent)
    };
    SnapshotVram snapshotVram() const;
    // ONE VRAM BUDGET (#405 / #407): the renderer's budget + ladder state as numbers,
    // one compact line (bug reports, log), and a short multi-line readout (Preferences).
    LLVKVramSummary vramSummary() const;
    // part 0: budget / usage / ladder (one short line), 1: per-category MB, 2: both + detail,
    // 3: what the overhead is made of + slack (#407), 4: texture pools (#411; "" = off).
    std::string vramStatusText(int part = 2) const;
    std::string vramReadoutText() const;
    // #685 TEXTURE INSPECTOR (RenderVKTexInspector): the VK residency state of one map
    // (0 base colour, 1 normal, 2 metal-rough, 3 emissive) of the face under the cursor --
    // see LLCleanRendererVK::texInspectText. uv_px: raw UV per screen pixel along x (0, 1)
    // and y (2, 3), or null. Multi-line; main thread; "" before the world renderer exists.
    std::string texInspectText(const LLUUID& obj, int face, unsigned map, const float* uv_px) const;
    // #684 Texture Console: short Vulkan residency line for a texture id; "" if not resident.
    std::string texResidencyBrief(const LLUUID& id) const;
    // <GMDX> LLCleanRendererVK::texOnGpu (true without the world renderer: nothing to wait for)
    bool texOnGpu(const LLUUID& id) const;
    // #458 HOST MEMORY: host-visible live MB per category (the non-zero ones), device-only
    // live MB, and the allocation traffic (vkAllocateMemory / vkFreeMemory per second and
    // MB/s, host-visible vs device-only, busiest categories) over the last hostMemTick
    // window. One line; "" before init. Bug reports ("hostmem"), MemTimeline lines.
    std::string hostMemText() const;
    // #471: the heaviest pipeline batch of this session as one line (< 200 chars): its
    // pipelines / compiled count, wall time, peak committed growth, threads, starts over
    // budget, and the slowest pipelines (name ms MB concurrency). "" before any batch.
    // Bug reports ("pipelines"). Any thread.
    static std::string pipelineCompileText();
    // #876 / #890: the driver is Intel's proprietary Windows one (VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS):
    // parallel pipeline creation crashes it. Known after init(); false before.
    static bool driverIsIntelWindows();
    // #1064: the driver identity the per-pipeline compile-crash quarantine is keyed by, as one
    // token "<vendorID hex>-<driverID>-<driverVersion hex>" (e.g. "1002-1-00800161"), and a
    // readable label (driverName + driverInfo). Known after init(); "" before. Any thread.
    static std::string driverIdentityToken();
    static std::string driverIdentityLabel();
    // The compile-thread cap decision (llappviewer) as one clause for the "pipelines" report
    // line, e.g. "capped to 1 thread: Intel Windows driver (#876)". "" = automatic. Any thread.
    static void setPipelineCompileCapNote(const std::string& note);
    // Main loop, every frame (cheap): samples the traffic once a second, and logs
    // hostMemText (VKMem) every 5 s while it is high (> 20 host-visible allocations/s or
    // > 64 MB/s) or `memTight` (the memory governor is tight / critical). Also applies
    // the RAM-scaled cap of the UI upload staging ring.
    void hostMemTick(double now, bool memTight);
    // Display context for the VRAM numbers (#407, bug reports; one line < 200 chars): the
    // swapchain (extent, format / colour space, present mode, image count, measured MB)
    // and the monitors (count, the window's display mode + scale, HDR on/off).
    std::string displayInfoText() const;
    // VRAM ladder effects the GL-side frame builder applies (buildVulkanWorldFrame): the
    // sun cascade range scale (far shadows off = 0.5; 1 = none) and the cascade map size
    // actually allocated (0 = unknown: use the setting) for the texel snapping.
    float vramShadowRangeScale() const;
    uint32_t shadowMapResolution() const;
    // VRAM ladder mesh detail / far geometry (#405 steps 4 and 6): multipliers on the
    // EFFECTIVE LOD factors (volume / avatar / terrain) and the effective draw distance
    // for a user RenderFarClip (floored at 64 m, never raised). All 1 / the setting when
    // nothing is held back or there is no Vulkan world renderer. Settings are never written.
    float vramMeshLodScale() const;
    float vramAvatarLodScale() const;
    float vramTerrainLodScale() const;
    float vramDrawDistance(float setting) const;
    // #422 AUTO-TUNE (LLVKAutoTune): the world renderer's measurement sample (false / invalid
    // without a world renderer), and the "Keep my FPS" temporary caps -- merged with the VRAM
    // ladder's caps (most restrictive wins), never a setting write. Main thread.
    bool autoTuneSample(LLVKAutoTuneSample& out) const;
    void setAutoTunePerfCaps(const LLVKPerfCaps& caps);
    // #1076 VR safety governor (LLVRSafety): its temporary caps, merged with "Keep my FPS"'s
    // (most restrictive wins) into what the renderer gets. Never called on the desktop.
    void setVrSafetyPerfCaps(const LLVKPerfCaps& caps);

    // Initialize the LLRenderVK facade (gVK) against this context's device using
    // the embedded UI SPIR-V, and install the concrete LLRenderBackend into
    // LLRender so gGL dispatches to Vulkan. Call once after init(). Returns the
    // installed backend (owned by the context) or null on failure.
    LLRenderBackend* initRenderBackend();

    // #1074 VR: end the OpenXR session and destroy the XrInstance at the viewer's exit
    // (LLAppViewer::cleanup, while logging still runs; shutdown() is not reached on a normal
    // exit). No-op without VR.
    void shutdownXr();
    // Notify of a resize (defers actual recreate to the next renderFrame).
    void onResize(uint32_t width, uint32_t height);
    // RenderVSyncEnable: picks the present mode; rebuilds the swapchain next frame.
    // #1073 VR: while an OpenXR session runs the window is a mirror that must never pace the
    // headset, so its swapchain stays non-FIFO; the user's choice is kept and comes back when
    // the session stops.
    void setVSync(bool on);
    // RenderVKNoVsyncPresentMode (vsync off only): 0 = MAILBOX first, 1 = IMMEDIATE first.
    // Rebuilds the swapchain next frame, like setVSync.
    void setNoVsyncPresentMode(uint32_t mode);
    // #919 FULL-SCREEN EXCLUSIVE (Windows only: VK_EXT_full_screen_exclusive, application-
    // controlled; always false / no-op elsewhere).
    // True when the device has the extension AND the surface reports exclusive support on
    // the monitor the window is on now. (Decides FullScreenMode "automatic" on Windows.)
    bool fullScreenExclusiveAvailable();
    // Arms / disarms exclusive presentation. Armed, the swapchain is rebuilt with the
    // exclusive chain at the next frame and the mode is acquired while the window is
    // focused and not minimised; released on focus loss / minimise / disarm, re-acquired on
    // focus gain. A lost mode (VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) is re-acquired,
    // or the swapchain goes back to composited presentation until focus returns.
    void setFullScreenExclusive(bool want);
    bool fullScreenExclusiveArmed() const;
    bool fullScreenExclusiveHeld() const;   // the mode is acquired right now
    void onWindowFocusChanged(bool focused);
    void onWindowMinimized(bool minimized);
    // FRAME PACING (RenderVKFramePacing): block until the frame-in-flight slot the next
    // renderFrame() will use is retired. Called at the TOP of the main-loop frame, before
    // input / idle / simulation, so the frame's world snapshot is taken right after the GPU
    // frees a slot instead of the CPU running ahead and then stalling mid-frame in acquire
    // (uneven simulated time per displayed frame = micro-stutter on a steady GPU). Timed as
    // "vk_paceWait"; renderFrame's acquire wait then shows only the swapchain image wait.
    // No-op before init, after shutdown and on a lost device. Main thread.
    void waitFrameSlot();

    // MILESTONE PROOF (no login, no swapchain): render the current clean-store
    // contents through the REAL GPU-driven world path (fillFromCleanStores ->
    // compute cull -> indirect draw) into an OFFSCREEN RGBA8 target using the
    // supplied camera, then read it back and write a PNG. Used by the synthetic-
    // cube self-test to prove the ingest -> stores -> VK draw path renders real
    // geometry without a windowed frame. Returns true on success (PNG written).
    bool runWorldSynthTest(const std::string& png_path, const LLVKWorldFrame& cam,
                           uint32_t width = 768, uint32_t height = 768);

    // RASTER CURSOR PICK (#702). Newest completed per-frame answer (the frame's
    // LLVKWorldFrame::pick, ~2 frames old; false when none yet).
    bool latestPick(LLVKPickResult& out) const;
    // Harvest the answers of frames in flight that completed already (non-blocking fence
    // checks), instead of when their slot comes round: a held click (llcleancursorpick.h).
    void pollPickAnswers();
    // Answer `req` NOW (#704): the select + the 1x1 draws for this ray against the last
    // rendered frame (its main-view draws, its posed rigged vertices), between frames.
    // Blocking (waits for the frames in flight). False when it can't run.
    bool pickNow(const LLVKPickRequest& req, LLVKPickResult& out);
    // <SS:Skoomastream> The answers to LLVKWorldFrame::aux_pick harvested since the last
    // call (appended; empty when none).
    void takeAuxPicks(std::vector<LLVKPickResult>& out);
    // DRAWN-GEOMETRY QUERY (#704): one result per clean instance id (rect: also the
    // rectangle test). Blocking (waits for the frames in flight). False = it can't run.
    bool faceQuery(const std::vector<uint32_t>& instance_ids, const LLVKRectQuery* rect,
                   std::vector<LLVKFaceQueryResult>& out);
    // The raster pick can run (world up, pipelines built or buildable).
    bool pickGpuAvailable() const;
    // Why the last pickNow / pickGpuAvailable said no ("" = no failure recorded): the
    // always-on VKPick diagnostics (llcleancursorpick.cpp).
    const char* pickFailure() const;
    // PARTICLE CURSOR PICK (#481). Newest completed per-frame answer (the frame's
    // LLVKWorldFrame::part_pick, ~2 frames old; false when none yet).
    bool latestPartPick(LLVKPartPickResult& out) const;
    // Answer `req` NOW against the last rendered frame's particle pool (blocking: waits for
    // the GPU to finish the frames in flight). False when it can't run (no world /
    // particles off).
    bool partPickNow(const LLVKPickRequest& req, LLVKPartPickResult& out);
    // ACOUSTICS PROBE (#683). Newest completed answer to LLVKWorldFrame::acoustics (~3
    // frames old; false when none yet / ray tracing unavailable). Non-blocking.
    bool latestAcoustics(LLVKAcousticsResult& out) const;
    // #683 phase 1: the newest bounce-pass answer, copied only when its serial differs from
    // `seen` (it is ~25 KB). false: none newer / unavailable.
    bool latestAcousticsBounce(LLVKAcousticsBounce& out, uint32_t seen) const;
    // The request serial whose bounce sets were last DISPATCHED (0: none yet): the bounce pass
    // runs only on its own frames, so the CPU learns which of its source sets really went out.
    uint32_t acousticsBounceDispatched() const;
    // #1075 LAYERED STEREO: the number of views the world renders (LLVKWorldFrame::view_count to
    // fill): 1 on the desktop and before the world renderer exists; N in VR stereo / the debug
    // N = 2 mode (fixed for the session).
    uint32_t worldViewCount() const;

    // Physical-device identity for the (context-less-GL) info/stats paths. Filled
    // from VkPhysicalDeviceProperties of the selected device. No-op leaving the
    // outputs untouched if the context is not initialized. Kept as plain strings
    // so llrender can consume it without a Vulkan dependency (the viewer copies
    // these into gGPUInfo after bring-up). vramMB is derived
    // from the largest DEVICE_LOCAL heap.
    // Hardware tier inputs (first-run graphics preset): device type and whether the
    // device exposes hardware ray tracing (independent of RenderVKRayTracing).
    bool isIntegratedGPU() const;
    bool supportsHardwareRayTracing() const;
    // Device-lost report (#135): GPU, driver name / info / decoded version, VRAM use and
    // budget per device-local heap, allocation totals, RT / async-queue state. Multi-line,
    // each line '\n'-terminated. Physical-device queries only: valid on a lost device.
    std::string deviceLostDiagnostics() const;
    void getDeviceIdentity(std::string& deviceName,
                           std::string& vendorShort,
                           std::string& apiVersion,
                           bool& isNVIDIA, bool& isAMD, bool& isIntel,
                           uint32_t& vramMB) const;
    // Max sampler anisotropy the device supports (0 when samplerAnisotropy is
    // unavailable or the context isn't up). Feeds gGPUInfo -- otherwise the feature
    // table masks anisotropic off.
    float getMaxSamplerAnisotropy() const;
    // Driver identity for the About box / reports / gGPUInfo: VkPhysicalDeviceDriverProperties
    // driverName + driverInfo, or the decoded driverVersion when driverInfo is empty.
    std::string driverVersionText() const;

private:
    LLVulkanContext() = default;
    ~LLVulkanContext() = default;
    LLVulkanContext(const LLVulkanContext&) = delete;
    LLVulkanContext& operator=(const LLVulkanContext&) = delete;

    struct Impl;
    Impl* mImpl = nullptr;
    // #919: full-screen exclusive state machine, run by renderFrame around the recreate.
    void fseBeforeRecreate();
    void fseAfterRecreate();
    bool mInitialized = false;
    std::string mInitFailureMessage;   // #526
    bool mRayTracingRequested = false;
    bool mClusterRequested = false;
    bool mNrEnabled = false;                // #870 setNeuralReconConfig
    std::string mNrDefaultDir, mNrShimPath, mNrLibDir, mNrDataDir, mNrEngineVersion;
    bool mPipelineStatsRequested = false;   // RenderVKPipelineStats
    bool mShaderClockRequested = false;     // RenderVKShaderClockLighting
    uint32_t mClusterDeviceFeatures = 1;
    uint32_t mAsyncComputeMode = 1;
    bool     mAsyncComputeAuto = false;   // #845
    bool     mDebugRobustness = false;    // RenderVKDebugRobustness
    int32_t mClusterForceTier = -1;   // #743 RenderVKClusterForceTier (startup tier log line)
    std::string mPipeCachePath, mPipeCacheTag;
    std::string mPipeCostPath;
    // #1073 VR: an OpenXR session was created on this device (LLXrSession; false on the
    // desktop: every VR branch in renderFrame tests only this). mXrPresentOverride: the session
    // runs, so the window swapchain is non-FIFO whatever mUserVSync says.
    bool mXrHooked = false;    // the device hooks were installed (VR chosen and the probe succeeded)
    bool mXrSession = false;
    bool mXrPresentOverride = false;
    bool mUserVSync = true;
    void onXrRunningChanged(bool running);
};

#endif // LL_LLVULKANCONTEXT_H
