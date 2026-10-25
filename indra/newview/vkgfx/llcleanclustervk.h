/**
 * @file llcleanclustervk.h
 * @brief Cluster LOD (#642): the GPU module's THIN OWNER (wave 0, #743).
 *
 * Owns what selection (1b, LLCleanClusterSelectVK) and streaming (1c,
 * LLCleanClusterStreamVK) share, so neither touches the other's files:
 *   - the module's descriptor set LAYOUT (bindings: clean_cluster_common.glsl) and one SET
 *     per frame slot, rewritten when the slot comes round from the buffers each owner
 *     REGISTERED (setBuffer) -- an owner (re)allocates, registers, and retires the old buffer
 *     through retire(); it never writes descriptors itself;
 *   - the capability tiers (LLCleanClusterCaps, RenderVKClusterForceTier applied live);
 *   - the GPU stats block (CL_B_STATS) + its readback, the joint-stretch placeholder
 *     (CL_B_JOINT_STRETCH, 2a replaces it), the Hi-Z import (CL_B_HIZ);
 *   - the per-frame ORDER (tick / recordCull / recordDraw) the renderer hooks call;
 *   - deferred destruction (retire) after kFrameSlots frames.
 *
 * Lifetime: created by the renderer the first frame RenderVKClusterLOD is on (never before:
 * with the toggle off nothing here exists and every renderer path is unchanged); destroyed
 * at renderer shutdown (device idle). Toggled off at runtime: tick() stops being called and
 * the renderer calls idleRelease() each frame, which frees the pools after kFrameSlots frames.
 *
 * Threading: render thread only (the renderer's recordWorld).
 */
#pragma once

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"
#include "llcleanclustercaps.h"
#include "llcleanclustercontrolvk.h"
#include "../llcleanclusterformat.h"
#include "../llavatarslots.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LLCleanClusterSelectVK;
class LLCleanClusterStreamVK;
class LLCleanClusterSkinVK;

// What the renderer hands over at creation: what cluster DRAW pipelines must be compatible
// with. Cluster draw layout = {drawSet0, drawSet1, cluster set} (cluster set at index 2) with
// the renderer's draw push range {VS|FS, 0, 16} (VKDrawViewPush): binding a cluster pipeline
// then leaves the renderer's bound sets 0/1 and the pushed view valid. A mesh-shader layout
// (push range with TASK|MESH stages) is NOT push-compatible: it re-binds sets 0/1 and
// re-pushes from LLClusterDrawIn.
struct LLClusterRenderEnv
{
    VkDescriptorSetLayout drawSet0 = VK_NULL_HANDLE;   // renderer mDslDraw0
    VkDescriptorSetLayout drawSet1 = VK_NULL_HANDLE;   // renderer mDslDraw1
    uint32_t drawPushBytes = 16;                       // sizeof(VKDrawViewPush)
    // Template of the deferred G-buffer pipelines (mGbufPipe): MRT formats, depth state
    // (EQUAL after the pre-pass, reversed-Z), VRS attachment, dynamic cull / depth state.
    // vert / frag / layout / vertex input are cleared: 1b fills its own. Valid when
    // gbufValid (deferred lighting pipelines exist).
    GraphicsPipelineDesc gbufDesc;
    bool gbufValid = false;
    bool reverseZ = true;
    // #749: templates of the main view's depth PRE-PASS pipelines (vert / frag / vertex input
    // cleared), one per pre-pass attachment set: [0] depth only inside pass 1's scope (colour
    // masked), [1] depth + AO normal (pass 1a), [2] depth + normal + motion + no-history mask
    // (pass 1a, temporal frames), [3] depth + motion + no-history mask WITHOUT normals (pass 1a,
    // deferred temporal frames: RenderVKPrepassDepthOnlyDeferred). Depth LESS + write,
    // reversed-Z. preValid: they exist.
    GraphicsPipelineDesc preDesc[4];
    bool preValid = false;
    // #702 integration: templates of the SECONDARY passes the cluster path draws in (vert /
    // frag / layout / vertex input cleared): the sun cascades (depth only, conventional depth,
    // bias, depth clamp, front = CCW), the projector tiles (the same without depth clamp; both
    // faces cast), the captures (the planar mirror / reflection-probe faces: the probe lit
    // pipelines' state, RGBA16F, front = CW, conventional depth). Valid flags: they exist.
    GraphicsPipelineDesc shadowDesc, projDesc, captureDesc;
    bool shadowValid = false, projValid = false, captureValid = false;
    // Legacy-skin skip: the RT consumer set layout (renderer mRT->consumerLayout(); null = no
    // RT). The captures' RAY-QUERY lit variant is built against it: layout {draw0, draw1, RT
    // set, cluster set at 3} with the ray-query lit FS, so mirror / probe faces draw the
    // switched avatars in ray-query lit frames too (captureDesc's state).
    VkDescriptorSetLayout rtSetLayout = VK_NULL_HANDLE;
    // RIGGED ALPHA-BLEND on the cluster path (CLI1B_BLEND): templates of the legacy rigged alpha
    // pipelines (vert / frag / layout / vertex input cleared; the cluster module brings its stages
    // and the same FS modules): pass 2's blend (LIT_RIG_ALPHA: no depth test / write, src-alpha
    // blend, the react attachment, dynamic shading rate and cull -- the plain and the ray-query
    // lit FS), the alpha-blend depth pre-pass (mPreBlendPipe), the sun cascades' translucency
    // layer (depth, colour), the captures' rigged alpha (mMirrorRigAlphaPipe: mirror + probe faces).
    GraphicsPipelineDesc alphaDesc, alphaDepthDesc, transDesc[2], captureAlphaDesc;
    bool alphaValid = false, alphaDepthValid = false, transValid = false, captureAlphaValid = false;
    // CLPASS_FORWARD (2026-10-14, snapshot fix): the main view's FORWARD lit pass 1 (LIT_RIG's
    // state: HDR + glow, reversed-Z, depth test + write, the VRS attachment flag, dynamic cull /
    // depth). Frames that do not take the deferred path -- supersampled snapshots with RT shadows
    // (no RT mask), RenderVKSnapshotDeferred off, a pass 1 that is not split, debug views -- draw
    // the cluster path here (plain lit FS, or the ray-query one with the RT set at 2 and the
    // cluster set at 3, as the captures). Without it a cluster-owned rigged mesh has no draw.
    GraphicsPipelineDesc forwardDesc;
    bool forwardValid = false;
};

// #749: what the renderer measured, for the A/B diagnostic line against the old (#561 / #613)
// rigged LOD controller and the cluster GPU spans (-1 = not measured this sample).
struct LLClusterDiagIn
{
    uint64_t skinned_verts = 0;      // skinned vertices the legacy pre-skin did (its GPU demand)
    // Legacy-skin skip (renderer clusterLegacySkipPlan): switched avatars' rows that got NO
    // legacy pre-skin (GPU count, kFrameSlots frames old) and their vertices; avatars skipped /
    // switched; switched avatars kept legacy, by reason ("sun proj mir prb pick rt sel off").
    uint32_t skip_insts = 0;
    uint64_t skip_verts = 0;
    uint32_t skip_av = 0, switched_av = 0;
    uint32_t keep_why[8] = {};
    // RT's role for switched avatars' legacy vertices this frame: 0 a consumer (it may build their
    // legacy fallback BLAS until their cluster cut is confirmed), 1 RT traces nothing, 2 RT keeps
    // switched avatars cluster-only (over-budget / pressure drop): no "keep rt" possible.
    uint32_t rt_role = 0;
    float    rig_gpu_ms = -1.f;      // legacy rigged spans (pre-skin + weld + rigged raster)
    float    select_ms = -1.f;       // cluster selection (graphics queue only)
    float    prepass_ms = -1.f;      // cluster pre-pass draws
    float    draw_ms = -1.f;         // cluster G-buffer draws
    float    preskin_ms = -1.f;      // cluster pre-skin (#747; graphics queue only: -1 / 0 under frame pipelining)
    uint32_t rig_slod = 0;           // old controller: decimation tolerance level A
    uint32_t rig_perf_p = 0;         // ... perf creator-LOD pressure
    uint32_t rig_pressure = 0;       // ... memory creator-LOD pressure
    uint32_t rig_ladder = 0;         // ... VRAM ladder rigged rung
    // Rigged ALPHA-BLEND still on the legacy path (the CLOD main line's "vs legacy"): the main
    // cull's rigged alpha draws (faces, pre + post water; read back) and pass 2's rigged alpha
    // triangles per frame (pipeline statistics, -1 = not measured).
    int64_t  legacy_alpha_faces = -1;
    int64_t  legacy_alpha_tris = -1;
    // Fixed-cost audit (2026-10-14): the cluster RT module's bytes (LLCleanRTVK::clusterRtBytes) and
    // its capacity-vs-live line, for the "CLOD mem" line; the cluster / comparison GPU sub-timers of
    // the last 5 s window ("name=avg/worst ms", RenderVKGpuTimers + sub-marks; empty = off).
    uint64_t rt_bytes = 0;
    std::string rt_mem;
    std::string gpu_sub;
};

// Per-frame input to tick().
struct LLClusterFrameIn
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t slot = 0;               // frame slot 0..kFrameSlots-1; its fence was waited
    uint64_t frame = 0;              // frame number
    uint64_t vram_avail = ~0ull;     // renderer vramRoom(0).avail at tick time
    uint64_t vram_budget = 0;        // renderer vramRoom(0).budget (0 = no device-local heap reported;
                                     // #1010: the skinned arena's auto limit)
    uint32_t vram_ladder = 0;        // renderer VRAM pressure ladder level (0 = none)
    bool     offscreen = false;      // a snapshot's 2nd+ recording: no streaming changes
    // Hi-Z of the main view (two-phase occlusion / RenderVKOcclusionCull), or null.
    VkImageView hiz_view = VK_NULL_HANDLE;
    VkSampler   hiz_sampler = VK_NULL_HANDLE;
    uint32_t    hiz_w = 0, hiz_h = 0;
    // Bumped by the renderer every time it re-creates the pyramid (resize: FSR mode, window).
    // A re-created view can reuse the old VkImageView handle value, so the descriptor sets
    // are refreshed on a generation change, not on a handle change.
    uint32_t    hiz_gen = 0;
    // #749 controller inputs: the GPU-time measurement + frame target, and the main view
    // (the culling camera, its px_scale = 0.5 x H x |P[1][1]|, the viewport height, the near clip).
    LLClusterPressureIn pressure;
    LLClusterDiagIn diag;
    bool     snapshot = false;       // any recording of a snapshot (offscreen), the first one too:
                                     // the controller runs at the base tolerance, live camera
    double   now = 0.0;              // seconds (any monotonic clock)
    float    main_cam[3] = {};
    float    main_px_scale = 0.f;
    float    main_h = 0.f;
    float    znear = 0.1f;
    // 2026-10-14 (the cluster path REPLACES the legacy one): the renderer's session mode
    // (LLCleanRendererVK::clusterOwnsRigged) -- every cluster-eligible rigged row belongs to the
    // cluster path whether its mesh is drawable yet or not: it is excluded from every legacy pass
    // and its legacy GPU geometry is never uploaded. False: the per-row eligibility still selects
    // and streams, nothing is excluded that the cluster path does not draw.
    bool     owns_rigged = false;
};

// One view to select clusters for (recordCull). Mirrors ClusterView's inputs.
struct LLClusterViewIn
{
    uint32_t flags = 0;              // CLV_*
    float view_proj[16] = {};        // (world - origin) -> clip, column-major
    float planes[6][4] = {};         // world space
    float cam_pos[3] = {};
    float znear = 0.1f;
    float origin[3] = {};
    float px_scale = 0.f;            // see llcleanclusterformat.h ERROR UNITS
    float tol_px = 1.f;
    float viewport_px[2] = {};
    // #749: the view's role (LLClusterViewRole: arena slice, tolerance scale, LOD camera) and
    // its index within the role (cascade / face). The core's recordCull fills tol_px and the
    // LOD camera from the controller for every role but CLROLE_OTHER (RT keeps what it set).
    uint32_t role = CLROLE_MAIN;
    uint32_t sub = 0;
    float lod_cam[4] = {};           // xyz + margin; set by recordCull (CLROLE_OTHER: = cam_pos)
    // #702 integration: skip moving casters (the #567 static / dynamic shadow split: its
    // dynamic overlay draws them legacy). Secondary views only.
    bool skip_dyn = false;
};

enum LLClusterDrawPass : uint32_t
{
    CLPASS_GBUFFER = 0,   // deferred G-buffer scope (main view)       [wave 1: 1b]
    CLPASS_PREPASS,       // depth (+ normal / motion) pre-pass        [#749: variant]
    CLPASS_FORWARD,       // forward lit pass 1 (no deferred lighting; plain / ray-query lit FS)
    CLPASS_SHADOW,        // sun cascades (depth only)                 [#702 integration]
    CLPASS_PROBE,         // CAPTURES: planar mirror + reflection-probe faces (plain lit FS)
    CLPASS_PROJ,          // projector shadow tiles (depth only, both faces)
    // Rigged ALPHA-BLEND (the alpha lists, LLClusterDrawIn::alpha): pass 2's blend (main view:
    // alpha_list 0 pre-water / 1 post-water), the alpha-blend depth pre-pass, the sun cascades'
    // translucency layer (depth, then colour). The alpha lists of CLPASS_SHADOW / CLPASS_PROJ
    // (alpha-tested into the depth map) and CLPASS_PROBE (captures, blended) use those passes.
    CLPASS_ALPHA,
    CLPASS_ALPHA_DEPTH,
    CLPASS_TRANS_DEPTH,
    CLPASS_TRANS_COLOR,
    CLPASS_COUNT
};
// LLClusterDrawIn::list: which records a draw takes.
enum : uint32_t
{
    CLLIST_VIEW = 0,            // the view's own records (statics of a secondary view)
    CLLIST_RIG_SHADOW = 1,      // the main view's shared rig list: shadow casters
    CLLIST_RIG_CAPTURE = 2,     // ... mirror / probe captures
};

// Inside a dynamic-rendering scope the renderer opened; its draw sets 0/1 are bound and the
// draw view pushed (VKDrawViewPush{draw_view, 0}).
struct LLClusterDrawIn
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t view = 0;                 // cluster view index (recordCull's return)
    LLClusterDrawPass pass = CLPASS_GBUFFER;
    uint32_t draw_view = 0;            // renderer draw-view ring index (re-push for mesh layouts)
    VkDescriptorSet draw_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool vrs_attach = false;           // the G-buffer scope uses the shading-rate attachment
    uint32_t pre_variant = 0;          // CLPASS_PREPASS: LLClusterRenderEnv::preDesc index (0 / 1 / 2 / 3)
    uint32_t list = CLLIST_VIEW;       // #702 integration: CLLIST_* (rig lists: view = the main view)
    // CLPASS_PROBE / CLPASS_FORWARD in a ray-query lit frame: this frame's RT consumer set -- the
    // ray-query lit variant draws (RT set at 2, the cluster set at 3). Null: the plain variant.
    VkDescriptorSet rt_set = VK_NULL_HANDLE;
    // #748 perf pass, MDI tiers: a rig list drawn from the per-view culled copy the select module's
    // recordRigCull made for THIS view just before its scope (false: the whole list).
    bool rig_culled = false;
    // RIGGED ALPHA-BLEND: draw the ALPHA list instead of the opaque buckets. Main view (list
    // CLLIST_VIEW, view = the main view): alpha_list 0 = pre-water, 1 = post-water (both for
    // CLPASS_ALPHA_DEPTH); a rig list: its alpha list. Implied by CLPASS_ALPHA / _ALPHA_DEPTH /
    // _TRANS_*. Blend passes draw it twice (cull FRONT, then BACK: LL's inside-out fix for
    // double-sided blend meshes) when the pipeline's cull is dynamic.
    bool alpha = false;
    uint32_t alpha_list = 0;
};

class LLCleanClusterVK
{
public:
    static constexpr uint32_t kFrameSlots = 3;           // = LLCleanRendererVK::kTexDescSets
    static constexpr uint32_t kAllSlots = 0xFFFFFFFFu;
    static constexpr uint32_t kBindingCount = LLClusterFmt::CLB_COUNT;   // 0..23, reserved ones included

    LLCleanClusterVK();
    ~LLCleanClusterVK();

    // Device + renderer environment. False = the module stays off (logged); never fatal.
    bool init(VkDeviceContext& ctx, const LLClusterRenderEnv& env);
    void destroy();                                    // device idle
    bool ready() const { return mReady; }

    // ---- per frame (renderer hooks, guarded by RenderVKClusterLOD) ----
    // Frame start, after the texture frame began (slot fence waited), before the main cull:
    // settings + tiers, the controller (#749), streaming (1c) and selection (1b) frame setup
    // (CPU), the slot's descriptor set. Records NOTHING (#749 frame pipelining): the GPU part
    // is recordFrameStart, which the renderer records where the front end runs.
    void tick(const LLClusterFrameIn& in);
    // The GPU part of the frame start (stats block -> readback + zero, 1c's uploads, 1b's
    // counter zero, the barrier to the shaders), into the command buffer the cluster
    // selection of this frame runs in: the front-end segment F under RenderVKFramePipelining
    // (F waits only the previous recording's P, so these writes must be in F itself), else
    // graphics. cmd_async: cmd is an async compute command buffer. Once per tick.
    void recordFrameStart(VkCommandBuffer cmd, bool cmd_async);
    // The streaming plan of this tick has page copies (the renderer forks an async copy
    // segment only then: an empty segment costs a submission).
    bool uploadsPlanned() const;
    // #697: the recording the last tick belonged to was DROPPED (out of memory at End /
    // submit): none of its GPU work may have run -- page copies, table / directory / group
    // writes, fills, growth copies, counter zeroing. Residency restarts (every mesh
    // re-registers and re-streams; the legacy path draws meanwhile), the per-frame zero fills
    // are redone. Rare; never crashes.
    void frameDropped();
    // Select + cull + emit for one view (compute, outside any rendering scope). Returns the
    // cluster view index (kNone = nothing to draw: no instances, view budget full, not ready).
    uint32_t recordCull(VkCommandBuffer cmd, const LLClusterViewIn& view);
    // Draw a view's emitted clusters inside the renderer's rendering scope. Returns true when
    // draws were recorded.
    bool recordDraw(const LLClusterDrawIn& in);

    // ---- #702 integration: secondary views, the shared rig cut, the GPU pick ----
    // Before the main view's selection: the per-avatar rig-list bits (joint-table slot ->
    // passes that draw the avatar this frame); on = false: no shared cut this frame.
    void setRigLists(const uint32_t bits[2][kAvatarSlotWords], bool on, uint32_t emit = 3u);
    // A rig list can be drawn in `pass` this frame: the main view selected the shared cut, the
    // cluster pre-skin was recorded over it, and the pass's pipelines exist. Decided once the
    // pre-skin was recorded (the renderer's clusterRigReady); the passes read the result.
    bool rigListReady(LLClusterDrawPass pass) const;
    // The same, decided BEFORE the cluster pre-skin is recorded (after the main selection):
    // true = rigListReady(pass) will hold for this frame's passes (the pre-skin's own early-outs
    // are known by then). The legacy-skin skip's need mask (renderer clusterLegacySkipPlan).
    bool rigListWillDraw(LLClusterDrawPass pass) const;
    // A secondary view's own (static) records can be drawn in `pass`: the view was selected
    // this frame and the pass's pipelines exist.
    bool viewDrawReady(uint32_t view, LLClusterDrawPass pass) const;
    // RIGGED ALPHA-BLEND (llcleanclusterselectvk.cpp "RIGGED ALPHA-BLEND"): after the cluster
    // pre-skin, the back-to-front order of this frame's alpha lists (cam / water / under: the
    // legacy alpha sort's inputs). Then alphaReady(pass): the pass can draw its alpha list.
    bool recordAlphaOrder(VkCommandBuffer cmd, const float cam[3], float water, bool under);
    bool alphaReady(LLClusterDrawPass pass) const;
    // GPU pick of cluster-drawn objects (llcleanpickvk.cpp).
    bool pickReady(VkDescriptorSetLayout out_dsl);
    bool recordPickFilter(VkCommandBuffer cmd, const float org[3], const float dir[3], float t0, float t1, float cone);
    void recordPickDraw(VkCommandBuffer cmd, const VkDescriptorSet draw_sets[2], VkDescriptorSet out_set,
                        const uint32_t push[4], uint32_t round);
    uint64_t pickListAddress() const;
    // SHADOW RECEIVER LIFT (#747 interim, clean_frame_info.glsl cluster_rt): this frame's
    // bound of how far a cluster surface of an avatar switched to the cluster path lies from
    // its LOD0 (and so from the legacy rigged geometry that still casts its shadows):
    // eps(P) <= tol_px * max(|P - cam|, znear) / px_scale with the main view's post-controller
    // tolerance and held px_scale (the selection's own projection; its LOD-camera band covers
    // |cam - lod_cam|). m_per_m = tol_px / px_scale. False = no avatar switched this frame or
    // no main-view selection recorded (then nothing is lifted).
    bool shadowLiftBound(float& m_per_m, float& znear) const;
    // #748 rigged cluster RT: the cluster view index of THIS frame's main-view selection (its
    // ClusterView in CL_B_VIEWS -- projection, LOD camera, tolerance -- is the rigged RT cut's
    // projection), kNone when no main view was selected this frame.
    uint32_t mainView() const { return (mReady && mMainViewFrame == mFrame) ? mMainView : 0xFFFFFFFFu; }
    // RenderVKClusterLOD turned off: no tick; frees everything after kFrameSlots frames.
    // Returns true once nothing is held (the renderer may then delete the module).
    bool idleRelease(uint64_t frame);
    // VKMem / stats fragment (renderer logMemory).
    std::string statsLine();
    // #749: the cluster summary + the A/B line against the old controller, rate-limited
    // (LL_INFOS "Vulkan" from vkgfx: reaches bug-report excerpts). Called from tick().
    void logSummary(const LLClusterFrameIn& in);
    // The 10 s summary logs at this tick (the renderer fills the diag's strings only then).
    bool summaryDue(double now) const { return mSummaryAt <= 0.0 || now - mSummaryAt >= 10.0; }

    // ---- shared services (1b / 1c / 2x) ----
    VkDeviceContext& ctx() { return *mCtx; }
    const LLCleanClusterCaps& caps() const { return mCaps; }
    const LLClusterRenderEnv& env() const { return mEnv; }
    VkDescriptorSetLayout setLayout() const { return mDsl; }
    VkDescriptorSet set(uint32_t slot) const { return mSets[slot % kFrameSlots]; }
    uint32_t slot() const { return mSlot; }
    uint64_t frame() const { return mFrame; }
    // Register the buffer behind a binding (all slots, or one slot for per-slot buffers).
    // Takes effect for each slot the next time it comes round. range VK_WHOLE_SIZE = all.
    void setBuffer(uint32_t binding, const GpuBuffer& buf, VkDeviceSize offset = 0,
                   VkDeviceSize range = VK_WHOLE_SIZE, uint32_t slot = kAllSlots);
    // The buffer registered behind a binding for the current slot (VK_NULL_HANDLE = none):
    // for consumers that bind it outside the set, e.g. the page pool as the vertex / UINT8
    // index buffer of the indexed raster tier (#745).
    VkBuffer registeredBuffer(uint32_t binding) const
    { return binding < kBindingCount ? mWant[mSlot % kFrameSlots][binding].buffer : VK_NULL_HANDLE; }
    // Free a buffer once every frame slot that may reference it has finished.
    void retire(GpuBuffer& buf);
    // Shader stages the cluster set is visible to (compute | vertex | fragment, + task |
    // mesh when the mesh shader tier exists): pipeline layouts must not exceed it.
    VkShaderStageFlags setStages() const { return mStages; }

    LLCleanClusterSelectVK& select() { return *mSelect; }
    LLCleanClusterStreamVK& stream() { return *mStream; }
    LLCleanClusterControlVK& control() { return mControl; }
    const LLCleanClusterControlVK& control() const { return mControl; }
    LLCleanClusterSkinVK& skin() { return *mSkin; }       // 2a (#747)

private:
    struct Bound
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64_t serial = 0;
        VkDeviceSize offset = 0, range = VK_WHOLE_SIZE;
    };
    struct Retired { GpuBuffer buf; uint64_t frame = 0; };

    void writeSlotSet(uint32_t slot);
    void freeRetired(bool all);
    void ensureCoreBuffers();
    void harvestStats(uint32_t slot);

    VkDeviceContext* mCtx = nullptr;
    LLClusterRenderEnv mEnv;
    LLCleanClusterCaps mCaps;
    bool mReady = false;
    VkShaderStageFlags mStages = 0;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool mPool;
    VkDescriptorSet mSets[kFrameSlots] = {};
    Bound mWant[kFrameSlots][kBindingCount];     // what each slot SHOULD reference
    Bound mHave[kFrameSlots][kBindingCount];     // what each slot's set references
    // CL_B_HIZ written per slot. Keyed on (view, size): a re-created Hi-Z can reuse a
    // destroyed view's handle, its size change then still forces the rewrite.
    VkImageView mHizView[kFrameSlots] = {};
    uint32_t    mHizSize[kFrameSlots] = {};
    uint32_t    mHizGenSet[kFrameSlots] = {};   // hiz_gen each slot's set was written with
    uint32_t    mHizWantGen = 0;
    VkImageView mHizWant = VK_NULL_HANDLE;
    uint32_t    mHizWantSize = 0;
    VkSampler   mHizSampler = VK_NULL_HANDLE;
    std::vector<Retired> mRetired;
    uint32_t mSlot = 0;
    uint64_t mFrame = 0;
    uint64_t mIdleSince = 0;
    bool mTicked = false;                        // tick ran, recordFrameStart not yet
    bool mOffscreen = false;                     // this tick: a snapshot's 2nd+ recording
    LLCleanClusterControlVK mControl;            // #749 tolerance controller
    double mSummaryAt = 0.0;                     // last summary line (s)
    // CPU cost of tick() (the main-thread frame setup: controller, 1c, 1b gather, 2a) over the
    // summary window: sum / max ms and ticks (the CLOD line's "cpu tick").
    double mTickMsAcc = 0.0, mTickMsMax = 0.0;
    uint32_t mTickN = 0;
    // #1011 PROFILE: the tick's CPU split (summed ms over the log window): controller, 1c stream,
    // 1b select (the gather), 2a skin
    double mTickCtlMs = 0.0, mTickStreamMs = 0.0, mTickSelectMs = 0.0, mTickSkinMs = 0.0;
    uint32_t mViewsSkipped = 0;                  // secondary view selections skipped (no static drawn), summary window
    // The main view's selection inputs this frame (shadowLiftBound): post-controller.
    float mMainTolPx = 0.f, mMainPxScale = 0.f, mMainZnear = 0.1f;
    uint64_t mMainViewFrame = ~0ull;             // frame the main view was selected (~0 = none)
    uint32_t mMainView = 0xFFFFFFFFu;            // its cluster view index (mainView)

    // Core-owned buffers.
    GpuBuffer mStats;                            // kFrameSlots x CLS_COUNT uint (device)
    GpuBuffer mStatsRead;                        // host readback, same layout
    GpuBuffer mJointStretchDummy;                // {rows = 0}: every stretch = 1 (2a replaces)
    uint32_t mStatsLast[LLClusterFmt::CLS_COUNT] = {};
    uint8_t mStatsRounds[kFrameSlots] = {};      // copies recorded per slot (readback valid at 2)

    std::unique_ptr<LLCleanClusterSelectVK> mSelect;
    std::unique_ptr<LLCleanClusterStreamVK> mStream;
    std::unique_ptr<LLCleanClusterSkinVK> mSkin;        // 2a (#747): skinning / avatar deformation
};
