/**
 * @file llcleanpickvk.h
 * @brief GPU raster cursor pick (#702 / #704) and particle pick (#481) for the Vulkan clean renderer.
 *
 * RASTER CURSOR PICK -- the viewer's only object pick (#704: no CPU narrowphase). The ray
 * is answered by the GPU against exactly what the raster draws:
 *
 *  - SNAPSHOT (recordSelect, beside clean_sel_rig.comp while cand[] holds the MAIN view's
 *    cull): cand[] / cand_meta[] are copied into pick-owned buffers -- per instance, the
 *    main view's draw: the drawn LOD slot / decimated step and, for rigged draws, this
 *    frame's pre-skinned range -- before the HUD / probe / mirror culls reuse cand[].
 *  - SELECT (clean_pick_select.comp, over the whole instance table, from the snapshot):
 *    every instance the main view draws whose bounds the ray crosses inside the request's
 *    depth window -> a static and a rigged indirect list; the HUD rows the HUD ray crosses
 *    (HUD cull level) -> a third. Recorded every frame for the cursor ray, and for any ray
 *    on demand (pickNow).
 *  - DRAW (recordDraw, after the HUD pass): the three lists into a 1x1 target whose pixel
 *    centre is the ray (LLVKPickRequest::world_vp / hud_vp), through the world's own
 *    vertex shaders (clean_draw.vert / clean_draw_rigged.vert: quantized static stream,
 *    worn attachments on the joint table, spinners, pre-skinned rigged stream, the #554
 *    own-avatar hide) and clean_pick.frag: no depth test, every fragment in the depth
 *    window counted in a depth histogram and appended with the raster's alpha verdict and
 *    its surface (clean_pick_common.glsl).
 *  - HARVEST (beginFrame, when the frame slot comes round: its fence was waited) -> the
 *    newest answer (latest).
 *  - QUERY (queryNow, between frames, #704): the drawn geometry of a list of instances
 *    (clean_pick_query.comp) -- normal sum, first vertex frame, a screen-rectangle test --
 *    for the build tools (rect select, media focus, planar alignment).
 *  - NOW (pickNow, between frames: a click whose ray no answer covers, a depth window the
 *    CPU narrowed or paged, a single-object surface pick off the cursor): select + draw
 *    in one blocking submission into the sync entry. The snapshot and the pre-skinned
 *    stream are the LAST frame's -- nothing rewrites them before the next frame's cull /
 *    pre-skin -- so rigged meshes pick in exactly the pose on screen (no extra skinning,
 *    no ring of old ranges), at the eye the snapshot was taken with.
 *
 * The CPU side (llcleancursorpick.h) sorts the fragments and applies LL's pick rules.
 *
 * PARTICLES (#481) live only in the GPU pool, so their pick happens here too
 * (clean_pick_particle.comp): the segment against every live particle's drawn quad
 * (billboard / streak / ribbon, the draw's alpha rule), nearest by a two-pass reduce.
 * Dispatched ONLY for a pending request: the per-frame one (LLVKWorldFrame::part_pick,
 * recorded at the end of the particle compute, harvested like the raster answer) or a
 * click's synchronous one between frames (partNow). Result: PartPickResult, 48 B.
 */
#ifndef LL_LLCLEANPICKVK_H
#define LL_LLCLEANPICKVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "llvulkancontext.h"   // LLVKPickRequest / LLVKPickResult

#include <cstdint>
#include <vector>

class LLCleanRendererVK;
struct VKDrawPush;

class LLCleanPickVK
{
public:
    explicit LLCleanPickVK(LLCleanRendererVK& r);
    ~LLCleanPickVK() = default;

    // Device must be idle.
    void destroy(VkDevice dev);

    // This frame's request (the cursor rays), before recordWorld.
    void setRequest(const LLVKPickRequest& req) { mReq = req; }
    // Top of recordWorld (this frame slot's fence waited): harvest the answers this slot's
    // previous frame wrote.
    void beginFrame();
    // Beside clean_sel_rig.comp (cand = the main view's cull): the snapshot (every main
    // frame) and, with a request, this frame's candidate lists. eye = the main eye the
    // pre-skinned positions are relative to (cam_time.xyz, w = the texture-animation
    // time); hide_self_w = the main view's rigged hide-self word.
    void recordSelect(VkCommandBuffer cmd, const float cam_time[4], float hide_self_w);
    // After the HUD pass: the 1x1 draws of this frame's lists.
    void recordDraw(VkCommandBuffer cmd);

    bool latest(LLVKPickResult& out) const;
    // The frame that last recorded frame slot `slot` (renderer mFrameSlot) has completed (its
    // fence signaled): its answer now instead of when the slot comes round (beginFrame).
    void harvestDone(uint32_t slot) { harvest(slot % kSlots); }
    // Answer req now against the last frame's snapshot (between frames). Blocking.
    bool pickNow(const LLVKPickRequest& req, LLVKPickResult& out);
    // Drawn-geometry query (#704): one result per instance id. Blocking.
    bool queryNow(const std::vector<uint32_t>& ids, const LLVKRectQuery* rect,
                  std::vector<LLVKFaceQueryResult>& out);
    // The raster pick can run (nothing failed).
    bool available() const;
    // Cluster LOD legacy-skin skip (renderer clusterLegacySkipPlan, after the main view's cluster
    // selection): this frame's picks -- and pickNow until the next frame -- will draw the cluster
    // path (the cluster pick filter + draw: its pipelines and list exist, the main view of this
    // frame), so the legacy rigged layer may drop switched avatars. Creates them on first call.
    bool clusterPickArmed();
    // Why the pick could not run: a latched set-up failure, else the last pickNow's
    // refusal ("" = the last pickNow ran). Static strings.
    const char* failure() const;

    // Particle pick (#481). This frame's request (invalid = no particle pick this frame).
    void setPartRequest(const LLVKPickRequest& req) { mPartReq = req; }
    // End of the particle compute (the pool is final for this frame): the pick pass, when
    // this frame has a request and particles are on.
    void recordParticles(VkCommandBuffer cmd);
    bool latestPart(LLVKPartPickResult& out) const;
    bool partNow(const LLVKPickRequest& req, LLVKPartPickResult& out);

    static constexpr uint32_t kSlots = 3;           // frame-slot entries (frames in flight <= 3)
    static constexpr uint32_t kSyncEntry = kSlots;  // pickNow's / partNow's entry
    // <SS:Skoomastream> AUX RAYS (sscombatray.cpp: the OTS camera collision, aim
    // convergence and line-of-sight rays): up to kAux extra world-only rays per frame,
    // each drawn like the cursor ray after it and harvested when its slot comes round
    // (~2 frames late, never blocking). Entries kSlots + 1 + slot * kAux + k.
    static constexpr uint32_t kAux = LLVKWorldFrame::kAuxPicks;
    static constexpr uint32_t kEntries = kSlots + 1 + kSlots * kAux;
    void setAuxRequests(const LLVKPickRequest* reqs);
    // The aux answers harvested since the last call (moved out).
    void takeAux(std::vector<LLVKPickResult>& out);
    // </SS:Skoomastream>
    // Raster pick entry (clean_pick_common.glsl PickEntry).
    static constexpr uint32_t kWorldFrags = 1024;   // PICK_WORLD_FRAGS
    static constexpr uint32_t kHudFrags = 128;      // PICK_HUD_FRAGS
    static constexpr uint32_t kHistWorld = LLVKPickResult::kHistWorld;   // PICK_HIST_WORLD
    static constexpr uint32_t kHistHud = LLVKPickResult::kHistHud;       // PICK_HIST_HUD
    static constexpr uint32_t kFragBytes = 96;      // PickFrag
    static constexpr uint32_t kHdrBytes = 512;      // PickHdr
    static constexpr uint32_t kEntryBytes = kHdrBytes + (kWorldFrags + kHudFrags) * kFragBytes;   // 111104
    // Candidate lists (clean_pick_select.comp Lists): 16 counters, then the DrawCmds.
    static constexpr uint32_t kListCap = 4096;      // static / rigged world draws
    static constexpr uint32_t kHudListCap = 512;    // HUD draws
    static constexpr uint32_t kListHdrBytes = 64;
    static constexpr uint32_t kDrawCmdBytes = 20;   // VkDrawIndexedIndirectCommand
    static constexpr uint32_t kPartResultBytes = 48;   // PartPickResult (clean_pick_particle.comp)

    // clean_pick_common.glsl pickBin's inverse: the lower edge of depth bin b of nb over the
    // window [lo, hi) (b == nb: the window's end).
    static float binEdge(uint32_t b, uint32_t nb, float lo, float hi);

private:
    struct SlotState
    {
        bool pending = false;        // this slot's last frame recorded the raster pick
        LLVKPickRequest req;
    };
    // This recording's raster pick (recordSelect -> recordDraw).
    struct FrameState
    {
        bool     selected = false;
        uint32_t slot = 0;
        float    cam_time[4] = {};
        float    hide_self_w = 0.f;
    };

    bool ensureBase();
    bool ensureSelect();
    bool ensureDraw();
    bool ensureWindow();
    bool ensureQuery(uint32_t jobs);
    bool ensureSnapshot(uint32_t rows);
    void recordSnapshot(VkCommandBuffer cmd, const float cam_time[4], float hide_self_w);
    void writeHeader(uint32_t entry, const LLVKPickRequest& req);
    // CLUSTER LOD (#702 integration): the cluster path's objects (cluster-owned statics are not
    // in the snapshot; switched avatars' legacy rigged draws are dropped by RC_CLUSTER) are
    // picked from their clusters: the module's filter + draw. True = it runs for this request.
    bool clusterPickOn(const LLVKPickRequest& req);
    // per entry: the cluster pick ran (draw it, drop RC_CLUSTER). <GMDX> Sized for every entry,
    // aux rays included (they index past kSlots and read/wrote out of bounds -> OTS crash).
    bool mClPick[kEntries] = {};
    // The select for req into the lists, its candidate counts into entry's header.
    bool recordSelectInto(VkCommandBuffer cmd, const LLVKPickRequest& req, uint32_t entry);
    // The 1x1 draws of the lists into entry; the serial last. view_w / view_h: draw views.
    void recordDrawInto(VkCommandBuffer cmd, const LLVKPickRequest& req, uint32_t entry,
                        uint32_t view_w, uint32_t view_h);
    // The layers' draw views for req, re-based on the eye cam_time.
    void drawViews(const LLVKPickRequest& req, const float cam_time[4], float hide_self_w,
                   VKDrawPush& wv, VKDrawPush& hv, bool cluster = false) const;
    void harvest(uint32_t slot);
    void readEntry(uint32_t entry, const LLVKPickRequest& req, LLVKPickResult& out) const;
    void hostBarrier(VkCommandBuffer cmd);
    uint32_t slotIndex() const;
    bool ensurePart();
    bool partReady() const;
    void recordPartDispatch(VkCommandBuffer cmd, const LLVKPickRequest& req, uint32_t entry);
    void readPartEntry(uint32_t entry, const LLVKPickRequest& req, LLVKPartPickResult& out) const;

    LLCleanRendererVK& mR;
    VkDeviceContext* mCtx = nullptr;
    LLVKPickRequest mReq;
    // <SS:Skoomastream> aux rays: this frame's requests, per slot the recorded ones, the
    // harvested answers not yet taken.
    void recordAux(VkCommandBuffer cmd);
    LLVKPickRequest mAuxReq[kAux];
    SlotState mAuxSlot[kSlots][kAux];
    std::vector<LLVKPickResult> mAuxDone;
    // </SS:Skoomastream>
    SlotState mSlot[kSlots];
    FrameState mFrame;
    LLVKPickResult mLatest;
    bool mBaseFailed = false, mSelFailed = false, mDrawFailed = false, mWinFailed = false;
    const char* mNowFail = "";        // the last pickNow's refusal (failure())

    GpuBuffer mOut;                    // kEntries entries (host-visible, coherent)
    GpuBuffer mLists;                  // candidate counters + draw lists (device-local)
    GpuImage  mTarget;                 // the 1x1 colour target (write-masked)
    // SNAPSHOT of the main cull (#704): cand[] (DrawCmd) / cand_meta[] per instance row as
    // the last main view left them, and the eye / hide word its rigged draws are skinned for.
    GpuBuffer mSnapCmd, mSnapMeta;
    uint32_t  mSnapCap = 0;            // rows the buffers hold
    uint32_t  mSnapRows = 0;           // rows the last snapshot copied (0 = none yet)
    float     mSnapCam[4] = {};
    float     mSnapHide = 0.f;

    VkDescPool mPool;
    VkDescriptorSetLayout mDslSel = VK_NULL_HANDLE;   // select: 0 snapshot cmd, 1 snapshot meta, 2 slots,
                                                      // 3 lod groups, 4 av_vis, 5 skin meta, 6 lists
    VkDescriptorSetLayout mDslOut = VK_NULL_HANDLE;   // draw: 0 the entries (fragment stage)
    VkDescriptorSet mSetSel[kEntries] = {};
    VkDescriptorSet mSetOut = VK_NULL_HANDLE;
    VkPipelineLayout mSelLayout = VK_NULL_HANDLE, mDrawLayout = VK_NULL_HANDLE;
    VkPipeline mSelPipe = VK_NULL_HANDLE, mStaticPipe = VK_NULL_HANDLE, mRigPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSelLayoutDsl = VK_NULL_HANDLE;    // the draw set 0 layout mSelLayout was made with
    VkDescriptorSetLayout mDrawLayoutDsl[2] = {};            // the draw set 0 / 1 layouts mDrawLayout was made with
    // Count-then-fill (clean_pick_window.comp): set 0 = {the entries, the lists}.
    VkDescriptorSetLayout mDslWin = VK_NULL_HANDLE;
    VkDescriptorSet mSetWin = VK_NULL_HANDLE;
    VkPipelineLayout mWinLayout = VK_NULL_HANDLE;
    VkPipeline mWinPipe = VK_NULL_HANDLE;

    // Drawn-geometry queries (clean_pick_query.comp): set 1 = {snapshot cmd, snapshot meta,
    // slots, LOD groups, jobs, results}; jobs / results host-visible, grown on demand.
    bool mQueryFailed = false;
    VkDescPool mQueryPool;
    VkDescriptorSetLayout mDslQuery = VK_NULL_HANDLE;
    VkDescriptorSet mSetQuery = VK_NULL_HANDLE;
    VkPipelineLayout mQueryLayout = VK_NULL_HANDLE;
    VkPipeline mQueryPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout mQueryLayoutDsl = VK_NULL_HANDLE;
    GpuBuffer mQueryIn, mQueryOut;
    uint32_t mQueryCap = 0;

    // Particle pick (#481). Entries: kSlots frame entries + the sync entry (kSyncEntry).
    struct PartSlotState
    {
        bool pending = false;        // this slot's last frame recorded the pass
        LLVKPickRequest req;
    };
    LLVKPickRequest mPartReq;
    PartSlotState mPartSlot[kSlots];
    LLVKPartPickResult mPartLatest;
    bool mPartFailed = false;
    uint32_t mPartGroups = 0;           // pool workgroups the partial buffers hold
    GpuBuffer mPartOut;                 // host-visible results, kPartResultBytes per entry
    GpuBuffer mPartPartial[kSlots + 1]; // per entry: (t bits, index) per pool workgroup
    VkDescPool mPartDescPool;
    VkDescriptorSetLayout mDslPart = VK_NULL_HANDLE;   // 0 partial, 1 results
    VkDescriptorSet mSetPart[kSlots + 1] = {};
    VkPipelineLayout mPartLayout = VK_NULL_HANDLE;
    VkPipeline mPartPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout mPartLayoutDsl[2] = {};      // the particle compute set + draw set 1
};

#endif
