/**
 * @file llcleanclusterskinvk.h
 * @brief Cluster LOD (#642 / #747, OWNER: 2a): skinning and avatar deformation -- the
 *        cluster pre-skin, the joint-stretch bound, posed bounds, the per-avatar switch from
 *        the legacy rigged path to the cluster path, welding on the cut, layering.
 *
 * Owns (allocates, registers with LLCleanClusterVK::setBuffer, retires):
 *   CL_B_JOINT_STRETCH (12)  {rows, pad x3, float[rows x 128]}: replaces the core placeholder.
 *                            Host-visible (zeroed at creation: rows 0 = stretch 1 until the GPU
 *                            writes it), GPU-written every frame by clean_cluster_preskin_prep
 *                            mode 0 before the first selection. VKMEM_RIG.
 *   CL_B_SKIN_OUT (16)       the SKINNED CLUSTER ARENA: 64 compact skinned vertices (20 B,
 *                            clean_skinvert.glsl) per skinned cluster, at word (s x 64 + local)
 *                            x 5 -- the CURRENT region (2b's rigged CLAS read it, #748) --
 *                            then (integration of 2a + 2c) the PREVIOUS-POSITION region: 3
 *                            floats per vertex at word cap x 64 x 5 + (s x 64 + local) x 3,
 *                            eye-relative to THIS frame's eye, written on motion-vector frames
 *                            (the only frames whose motion pre-pass reads them; cap = header
 *                            CLK_CAP). SIZED BY THE CUT: a slot exists only for a cluster in the
 *                            main view's cut (+ kSlotGrace frames), from the STABLE-SLOT pool
 *                            (taken on entry to the cut, kept while it stays -- its previous
 *                            positions stay valid --, freed by the sweep on exit). Capacity (cap)
 *                            = the read-back DEMAND (2026-10-14: CLK_LIVE + CLK_ALLOC_N, peak of
 *                            ~2 x 5 s, + 1/4 + 1024; doubled at once when SHORT; never past the
 *                            main slice's map), shrunk with hysteresis, never past the LIMIT
 *                            (#1010 ARENA PRESSURE below). ~2.3 KiB per cluster. Was the sum of
 *                            every admitted mesh's whole DAG: 129k of 131k clusters held for 21k
 *                            skinned at PEAK, and 14 avatars kept legacy. VKMEM_RIG.
 *   CL_B_SKIN_SCRATCH (17)   header (CLK_*) | per-ClusterInstance posed-bound margin | the
 *                            per-selected-entry map (entry -> skinned cluster) | the owned
 *                            render-instance bitset (stats) | the weld grid. + INDIRECT usage
 *                            (the pre-skin's dispatch args live in the header). VKMEM_RIG.
 * Plus: its own descriptor set (set 1 of its compute pipelines, one per frame slot) over the
 * renderer's rigged buffers (palettes, skin meta, joint table, legacy work list + allocator)
 * + its own #798 REST TABLE (binding 5: the weld's rest-pose pairing space, see below),
 * and a per-slot host-visible readback of the scratch header (stats). Layouts:
 * indra/vkclean/shaders/clean_cluster_skin_common.glsl (GLSL twin of the constants below).
 *
 * ---- PER FRAME -------------------------------------------------------------------------------
 *   renderer clusterTick   setFrameInputs(): the renderer's rigged buffers / counts / eye.
 *   core tick              1b's gather calls the GATHER HOOKS (avatarOfSkin / noteRow /
 *                          noteInstance / admit): the per-avatar switch; then beginFrame():
 *                          buffers, registration, set 1 of this slot.
 *   core recordFrameStart  (#749) nothing of 2a's: the module records no frame-start work.
 *   core recordCull        recordBounds() once, before the first selection -- the main view's,
 *     (front-end segment)  recorded after recordFrameStart in the same command buffer: the
 *                          joint-stretch table + every rigged instance's posed-bound margin (+
 *                          the owned bits). Later views (shadow / probe / mirror) reuse them.
 *   renderer, after the    recordPreskin(): args -> cluster pre-skin of the main view's
 *     legacy pre-skin      selected rigged clusters (+ their previous positions from the
 *     and weld             renderer's previous-frame palettes, by device address, on motion
 *                          frames) (+ weld insert) -> weld -> apply -> the legacy-vertex stats
 *                          -> header readback -> barrier to the draws. GPU time: the span
 *                          c_cl_preskin_begin -> c_cluster_preskin (a cluster class of the
 *                          sampled rigged ring, kept out of the legacy rigged time).
 *   depth pre-pass, then   1b's cluster draws read (map, arena) for rigged entries: the same
 *   G-buffer scope         VS / mesh modules in both, so the skinned path is shared.
 * Everything GPU-side is recorded in ONE command buffer (the front-end segment under frame
 * pipelining; the pre-pass / G-buffer readers record after its graphics join): nothing here
 * depends on graphics work unordered with it, every per-frame word is (re)written before it
 * is read.
 *
 * ---- THE SWITCH (PER MESH since 2026-10-14; RenderVKClusterRigged) ---------------------------
 * The cluster path REPLACES the legacy one (owner decision): in the renderer's cluster-owns-
 * rigged mode (LLClusterFrameIn::owns_rigged) every eligible rigged row (1b's "WHAT IS
 * GATHERED": not HUD / alpha-blend / flexi / system body) is excluded from every legacy pass and
 * has no legacy geometry, whether its mesh is drawable yet or not. Each rigged ClusterInstance
 * (key resolved, mesh registered with its roots resident -- a cut always exists) gets
 * CLI1B_DRAW as soon as it exists (admit()): an avatar is drawn mesh by mesh as they arrive
 * (pop-in; the loading cloud covers it), no "every mesh ready" gate, no legacy fallback. Held
 * back only: the setting / module off, no palette, a broken-off gather, or (RenderVKClusterRigged 1) the cluster pre-pass pipelines
 * still compiling while per-pixel pre-pass readers exist -- such an instance is NOT DRAWN.
 * (Before 2026-10-14 an avatar switched only when every mesh was on the cluster path and kept a
 * full legacy copy for the passes without a cluster draw.)
 * RenderVKClusterRigged: 0 = never (wave-1 behaviour: rest-pose debug overlay only); 1 =
 * (default) real whenever the cluster depth PRE-PASS pipelines exist for the raster tier in
 * force (#749 CLPASS_PREPASS, all three attachment variants: depth / + AO normal / + motion --
 * select().prepassReady()), so the motion vectors, SSAO, the RT shadow mask and the Hi-Z see a
 * switched avatar; without them only while nothing reads the main-view pre-pass per pixel
 * (else a switched avatar would smear, lose its AO or take the RT shadow of what is behind
 * it); 2 = real always (A/B, accepting that).
 *
 * ---- LAYERING (#611) --------------------------------------------------------------------------
 * Cheapest correct option: for an avatar with two or more cluster-eligible rigged meshes, the
 * joint-stretch rows of its skins are multiplied by RenderVKClusterLayerErrorScale (default 2):
 * every mesh of a layered outfit then keeps its simplification error under tol / scale in
 * pixels, so body + clothing errors together stay within one tolerance -- poke-through between
 * layers is bounded by the tolerance (sub-pixel at the default 1 px) instead of twice it. It
 * acts through the stretch factor (one value per joint row, >= 1, the same for every group of a
 * skin), so the cut stays monotonic and watertight; it costs detail only on layered avatars.
 * (Contact locks would need the outfit at build time; a cluster mesh is built per asset.)
 *
 * ---- ARENA PRESSURE (#1010) --------------------------------------------------------------------
 * There is no legacy fallback for a rigged mesh (the cluster path replaces it), so a cut that does
 * not fit the arena must never lose random clusters (the pony party: up to 40k clusters a frame
 * missing at the old fixed 256 MB cap -- missing parts, eyes popping). Three layers:
 *  - LIMIT: RenderVKClusterSkinArenaMB 0 (default) = AUTO: 1/16 of the device's VRAM budget
 *    (vramRoom: OS budget / "Maximum video memory" / GPU share), 128 MB .. 4 GB (2 GB on a 32 GB
 *    card, 512 MB on 8 GB); non-zero = that many MB. Then the VRAM ladder's depth (soft, restored
 *    with the rungs; the setting is untouched: any rung 3/4, a third 1/2, two thirds 1/4), the
 *    binding range, and growth only into the VRAM room under pressure (vram_avail; a failed growth holds the
 *    current size ~2 s, then retries).
 *  - COARSEN WHOLE AVATARS: the cut is fitted to the limit by the controller's ARENA PRESSURE
 *    (LLCleanClusterControlVK::setArenaPressure, a log2 exponent P): every avatar's tol_scale gets
 *    2^clamp(P - L, 0, 8), L = log2(its on-screen height / 1 % of the viewport) in 0..6 and 9 for
 *    your own avatar -- the smallest / farthest avatars coarsen first, the nearest last, yours
 *    after everyone; every mesh of an avatar by the same factor (it stays whole, only coarser). P
 *    rises (+1 per ~0.15 s) while the arena is SHORT at its limit, +0.25 per ~0.15 s over 92 % of
 *    it; falls 0.5 per dwell (2 s, doubled after a release re-raised within 30 s, up to 16 s) under
 *    65 %. Settings untouched.
 *  - WHOLE-AVATAR ADMISSION on the GPU (clean_cluster_skin_common.glsl WHOLE-AVATAR ARENA
 *    ADMISSION): within the read-back lag (a teleport into a crowd) the free list may still not
 *    hold a frame's new clusters -- the avatars past it in admit()'s PRIORITY ORDER (yours first,
 *    then by importance) are hidden WHOLE that frame, never a subset of anyone's parts. At P's
 *    ceiling (every avatar at 256x tolerance) this is what drops the least important avatars
 *    until they fit (no impostor path exists: they are not drawn).
 * CLK_OVERFLOW now counts PARTIAL drops only (an entry without an avatar row): expected 0, warned
 * per episode. Logged: the per-episode warnings and the 10 s "CLOD skin arena" summary line.
 *
 * ---- FAILURE -----------------------------------------------------------------------------------
 * Never a crash: a failed allocation keeps the previous buffer (or none) and the growth is retried
 * (the arena: the pressure above fits the cut to what exists meanwhile); missing pipelines =
 * nothing drawn; retried.
 */
#pragma once

#include "vk_device.h"
#include "vk_descpool.h"
#include "../llcleanclusterformat.h"
#include "../llavatarslots.h"
#include "lluuid.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class LLCleanClusterVK;
struct LLClusterFrameIn;

// The renderer's rigged state for this frame (LLCleanRendererVK::clusterTick).
struct LLClusterSkinIn
{
    const GpuBuffer* palette = nullptr;     // SkinBlock[] (eye-relative palettes + bind shape)
    const GpuBuffer* meta = nullptr;        // LLCleanSkinStore::SkinMeta[]
    const GpuBuffer* joints = nullptr;      // joint table (AvatarJoints[])
    const GpuBuffer* work = nullptr;        // legacy pre-skin work list (stats)
    const GpuBuffer* rig_alloc = nullptr;   // legacy allocator ([0] = work items)
    uint32_t skin_count = 0;                // live palettes (min(mSkinCount, palette cap))
    uint32_t avatar_count = 0;              // joint-table rows the palette compute reads
    uint32_t work_cap = 0;
    float    eye[3] = {0.f, 0.f, 0.f};      // the palettes' eye
    const uint8_t* meta_cpu = nullptr;      // CPU mirror of the skin meta (avatar per skin)
    size_t   meta_cpu_bytes = 0;
    bool     hide_self = false;             // own avatar hidden in the main view (mouselook; still
                                            // admitted: the main view's CLV_HIDE_SELF hides it)
    // Something reads the MAIN-VIEW DEPTH PRE-PASS per pixel (motion vectors / TAA, SSAO, the
    // RT shadow mask). Only matters while the cluster pre-pass pipelines are missing
    // (select().prepassReady() false): then a switched avatar would be absent from it.
    bool     prepass_readers = false;
};

// The previous-frame skinning input of this frame's cluster pre-skin (renderer, at the cluster
// pre-skin: the legacy pre-skin decided it). palette: device address of the renderer's
// previous-frame palettes (SkinBlock[], LAST frame's joints relative to THIS frame's eye --
// clean_preskin.comp u_prev 1), 0 = none this frame (no motion vectors, no joint history, or
// the F6 history mode RenderVKPreskinPrevHistory, which builds none): previous = current.
// motion: this frame draws the motion pre-pass (the only reader of the previous positions):
// without it and without palettes the pre-skin writes no previous positions at all.
struct LLClusterSkinPrevIn
{
    VkDeviceAddress palette = 0;
    // (2026-10-14 step 2) the previous frame's joint table the previous palettes were built from
    // (device address, 0 = none: the characters' previous frames = their current ones), its rows.
    VkDeviceAddress joints = 0;
    uint32_t joints_rows = 0;
    // (2026-10-14 step 3) kAvatarSlotWords words: bit = that joint-table slot's pose relative to its
    // pelvis is PROVED unchanged since the previous frame (null = none proved: every row re-skinned).
    const uint32_t* pose_clean = nullptr;
    uint32_t palette_cap = 0;               // SkinBlocks it holds (skins past it: previous = current)
    bool     motion = false;
};

class LLCleanClusterSkinVK
{
public:
    explicit LLCleanClusterSkinVK(LLCleanClusterVK& owner) : mOwner(owner) {}
    ~LLCleanClusterSkinVK() = default;

    // ---- shared layout constants (= clean_cluster_skin_common.glsl) ----
    static constexpr uint32_t kBindingSkinOut     = LLClusterFmt::CLB_SKIN_OUT;       // 16
    static constexpr uint32_t kBindingSkinScratch = LLClusterFmt::CLB_SKIN_SCRATCH;   // 17
    static constexpr uint32_t kHdrWords   = 48;   // (#798 resolve: 40 -> 48, + NO-OP / MULTI)
    static constexpr uint32_t kHdrUsed = 0, kHdrCap = 1, kHdrOverflow = 2, kHdrSkinVerts = 3, kHdrLegacyVerts = 4,
                              kHdrLegacyItems = 5, kHdrWeldN = 6, kHdrWeldDrop = 7, kHdrWeldApplied = 8,
                              kHdrMapBase = 9, kHdrSelBase = 10, kHdrSelCount = 11, kHdrEye = 12, kHdrFrame = 15,
                              kHdrDispatch = 16, kHdrSkinClusters = 19, kHdrMapCap = 20, kHdrWeldSkip = 21,
                              kHdrWeldDispatch = 22,   // 3 words: the weld passes' indirect args
                              kHdrFreeN = 25, kHdrAllocN = 26, kHdrLive = 27, kHdrAllocDispatch = 28,
                              kHdrReused = 31,         // step 1 / 3: stable slots (clean_cluster_skin_common.glsl)
                              // #798 seam diagnostics (sampled frames): unwelded candidates by reason,
                              // candidates of border-free clusters, avatars with any
                              kHdrWeldNoPair = 32, kHdrWeldWRej = 33, kHdrWeldGRej = 34, kHdrWeldBFree = 35,
                              kHdrWeldBFreeAv = 36,
                              // #798 rest-pose pairing: candidates with a matching partner at rest
                              // (gap or not); boundary vertices of avatars without a rest row yet
                              kHdrWeldPaired = 37, kHdrWeldNoRest = 38,
                              // #798 resolve: candidates left alone (every partner already seamless), with >1 partner
                              kHdrWeldNoop = 39, kHdrWeldMulti = 40,
                              // #826 WELD PAIR CACHE (sampled frames): entries applied from stored
                              // pairs / whose stored partner failed validation / that searched
                              kHdrWeldCached = 41, kHdrWeldInvalid = 42, kHdrWeldSearched = 43,
                              // #1010 WHOLE-AVATAR ARENA ADMISSION: the PRIORITY region's scratch
                              // word, SHORT (new entries > free list), rows refused whole, entries
                              // hidden by a refusal; free-list slots TAKE handed out (word 19 reused)
                              kHdrPrioBase = 44, kHdrShort = 45, kHdrRefusedAv = 46, kHdrHidden = 47,
                              kHdrAllocTaken = 19;
    static constexpr uint32_t kSlotMetaWords = 8;      // CL_SKIN_META_WORDS
    static constexpr uint32_t kCharWords = 36;         // CL_SKIN_CHAR_WORDS (step 2: per character row)
    // CLW_AV_WORDS: the weld's per-avatar drop bits, right before its cell heads; before them the
    // #798 diagnostic bits (avatars with border-free weld candidates), as many words.
    static constexpr uint32_t kWeldAvWords = kAvatarSlotWords;   // (= CLW_AV_WORDS; 32 at 1024 slots)
    // Weld entry capacity granularity (entries; the cell count stays a power of two).
    static constexpr uint32_t kWeldCapStep = 65536;
    // Hard ceiling of RenderVKClusterWeldMaxVerts: 2M entries (~176 MB); the weld dispatch is
    // (cap + 63) / 64 = 32768 groups, inside the guaranteed maxComputeWorkGroupCount[0] (65535).
    static constexpr uint32_t kWeldCapMax = 1u << 21;
    static constexpr uint32_t kInstBase   = 48;          // CLK_INST_BASE
    static constexpr uint32_t kVertsPerCluster = LLClusterFmt::kMaxClusterVerts;
    static constexpr uint32_t kSkinVertexBytes = 20;     // SkinVertex
    static constexpr uint32_t kSkinPrevBytes = 12;       // previous position (xyz, eye-relative)
    // #748 perf pass: SKINNED BOUNDS after the previous-position region (clean_cluster_skin_common
    // .glsl): 8 words per skinned cluster, then 8 words per avatar row (257 rows: joint-table slots
    // + none), the avatar rows zeroed before every pre-skin (the rig-list cull's inputs).
    static constexpr uint32_t kSkinBoxBytes = 32;
    static constexpr uint32_t kSkinAvRows = kAvatarSlotCap + 1;   // (= CL_SKIN_AV_ROWS)
    // #1010 PRIORITY region (scratch, after the layer bits): n | row order [kSkinAvRows] | per-row new
    // entries [kSkinAvRows] | per-row refused [kSkinAvRows] (= CL_SKIN_PRIO_WORDS)
    static constexpr uint32_t kPrioWords = 1u + 3u * kSkinAvRows;
    static constexpr uint32_t kWeldEntWords = 12, kWeldResWords = 8;
    static constexpr uint32_t kMaxAvatars = kAvatarSlotCap;   // joint-table slots (LLCleanSkinStore::MAX_AVATARS)
    static constexpr uint32_t kStretchLanes = 128;
    static constexpr uint32_t kSkinMetaBytes = 7568;     // LLCleanSkinStore::SkinMeta

    bool init();
    void destroy();                       // device idle

    // ---- renderer, before LLCleanClusterVK::tick ----
    void setFrameInputs(const LLClusterSkinIn& in);

    // ---- 1b's gatherer (LLCleanClusterSelectVK::gather), render thread ----
    // RenderVKClusterRigged != 0 and the module works: 1b allocates the exclusion bitset.
    bool riggedWanted() const;
    // Frame start of the gather (render_instances = instance-store rows): reset the tallies.
    void gatherBegin(uint32_t render_instances);
    // The joint-table slot of a skin (kNone = unknown / unresolved avatar).
    uint32_t avatarOfSkin(uint32_t skin_id) const;
    // An eligible rigged row of avatar `av` (before any reason it may not become an instance).
    void noteRow(uint32_t av);
    // ... that became ClusterInstance `inst` (index into the gatherer's list) of mesh `dir`.
    // (+ #798 weld probe: the mesh key and the store slot the cluster mesh is built from)
    // (+ #1010: the wearer's importance -- height over distance, admit()'s PRIORITY ORDER -- and the
    // arena term of its avatar, > 1 = coarsened by the arena pressure: stats)
    void noteInstance(uint32_t av, uint32_t inst, uint32_t mesh_dir, uint32_t skin_id, bool self,
                      const LLUUID& mesh_id = LLUUID::null, int32_t face = 0, uint32_t src_slot = 0xFFFFFFFFu,
                      float importance = 0.f, float arena_scale = 1.f);
    // The gather broke off early (instance cap): no avatar may switch this frame.
    void gatherIncomplete() { mGatherBroken = true; }
    // Decide the avatars drawn by the cluster path; `out` = the gatherer's instance indices
    // to flag CLI1B_DRAW + exclude. allowed = the exclusion bitset exists and the cluster
    // G-buffer draw is live (1b's gate).
    void admit(bool allowed, std::vector<uint32_t>& out);

    // ---- per frame (core) ----
    void beginFrame(const LLClusterFrameIn& in);   // after 1b's beginFrame, before the set write
    // Before the frame's first selection, same command buffer.
    void recordBounds(VkCommandBuffer cmd);
    bool boundsValid() const { return mBoundsRecorded; }
    // After the legacy pre-skin (palettes ready), same command buffer as the selection, before
    // the cluster pre-pass / G-buffer draws.
    // mark = the renderer's GPU sub-timer (2026-10-14 step 0: the window's sub-marks c_cl_sk_*,
    // each the time since the previous mark -- see recordPreskin); may be empty.
    void recordPreskin(VkCommandBuffer cmd, uint32_t main_view, const LLClusterSkinPrevIn& prev,
                       const std::function<void(VkCommandBuffer, const char*)>& mark = {});
    // #697 / #749: the recording of the last tick was dropped (or its GPU half never recorded):
    // its header readback may never have run.
    void frameDropped();

    void statsLine(std::string& out) const;
    // #1010: the 10 s "CLOD skin arena" line (clusters dropped -- expected 0 --, avatars hidden whole /
    // coarsened, the pressure, arena live / cap / peak MB and its limit); resets the window.
    void arenaSummary(std::string& out);
    // #1010 ARENA PRESSURE (log2 exponent, 0 = none): the controller's per-avatar arena term.
    float arenaPressure() const { return mArenaP; }
    uint64_t heldBytes() const;
    // Fixed-cost audit (2026-10-14, the 10 s "CLOD mem" line): per buffer "name live/cap" MB
    // (live = what the demand uses, cap = what is allocated), compact.
    void memSummary(std::string& out) const;

    // 2026-10-14 VRAM by path: the skinned arena's capacity / target (clusters) and its bytes.
    uint64_t arenaClusters() const { return mSkinOutClusters; }
    uint64_t arenaWantClusters() const { return mWantClusters; }
    uint64_t arenaBytes() const { return mSkinOut.size; }
    // Avatars drawn by the cluster path in the main view this frame (admit()).
    uint32_t avatarsOnCluster() const { return mAvCluster; }
    uint32_t instancesAdmitted() const { return mInstAdmitted; }   // rigged ClusterInstances drawn for real (gather)
    // This frame's rigged inputs (setFrameInputs): #748's rigged cluster RT skins its own cut
    // from the same palettes (eye-relative to in.eye).
    const LLClusterSkinIn& frameInputs() const { return mIn; }
    // The skinned arena (SKIN_OUT; device address + AS build input usage when ray tracing is on):
    // #748's CLAS tiers build a switched avatar's rigged CLAS from the shared cut skinned here.
    const GpuBuffer& skinOut() const { return mSkinOut; }
    // The scratch (CL_B_SKIN_SCRATCH): its header's kHdrDispatch words are the pre-skin's per-entry
    // indirect grid, which #901's weather cluster kernels reuse (llcleanweathervk_shelter.cpp).
    VkBuffer scratchBuffer() const { return mScratch.buffer; }
    // #702 integration: recordPreskin skinned the main view's slice in frame `frame` (its map
    // and arena are valid for every reader of that slice: the shared rig lists, the pick).
    bool preskinRecorded(uint64_t frame) const { return mPreskinFrame == frame; }
    // Legacy-skin skip (the renderer's need mask, after the main selection, before the pre-skin
    // is recorded): recordPreskin(main_view) will skin the main slice this frame -- exactly its
    // own early-out conditions, so a pass gated on this cannot find the map stale later.
    bool preskinWillRecord(uint32_t main_view) const;
    // Avatar (joint-table slot) admitted to the cluster path this frame (admit()).
    bool avatarSwitched(uint32_t av) const
    { return av < kMaxAvatars && ((mAdmittedBits[av >> 5] >> (av & 31u)) & 1u) != 0u; }

private:
    struct AvTally
    {
        uint32_t rows = 0, insts = 0, admitted = 0;
        uint64_t clusters = 0;
        bool self = false, bad = false;
        bool coarse = false;         // #1010: an instance coarsened by the arena pressure (tol_scale > 1)
        float importance = 0.f;      // #1010: the wearer's importance (priority order)
        uint32_t frame = 0;          // frame the tally belongs to
    };
    // #1010 ARENA PRESSURE: the arena's limit (clusters) this frame and the controller step.
    uint64_t arenaLimit(const LLClusterFrameIn& in, uint32_t& setting_mb, bool& autosize);
    void arenaControl(double now, uint64_t limit);
    enum Why : uint32_t
    {
        WHY_OK = 0, WHY_OFF, WHY_PREPASS, WHY_PENDING, WHY_SELF, WHY_ARENA, WHY_NOPAL, WHY_GATE, WHY_COUNT
    };

    bool ensureStretch(uint32_t rows);
    bool ensureSkinOut(uint64_t clusters);
    bool ensureScratch(uint32_t inst_cap, uint32_t map_cap, uint32_t owned_words, uint32_t weld_cap);
    // #798 REST TABLE: the CPU plan (beginFrame: rows for the admitted avatars, triggers, rebuilds,
    // capacity) and its upload (recordPreskin, before the skin reads it).
    void restPlan(bool on);
    bool ensureRest(uint32_t rows);
    void restUpload(VkCommandBuffer cmd, uint32_t& xfers);
    void writeSet(uint32_t slot);
    void harvest(uint32_t slot);
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                 VkAccessFlags2 da) const;
    uint32_t weldCapWanted() const;

    LLCleanClusterVK& mOwner;
    bool mReady = false;
    bool mMesh = false;

    // Pipelines: set 0 = the cluster set, set 1 = mDsl (renderer rigged buffers).
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool mPool;
    VkDescriptorSet mSets[3] = {};
    struct SetHave { VkBuffer b[6] = {}; uint64_t s[6] = {}; };   // (= kSetBindings)
    SetHave mSetHave[3];
    VkPipelineLayout mPrepLayout = VK_NULL_HANDLE, mSkinLayout = VK_NULL_HANDLE, mWeldLayout = VK_NULL_HANDLE;
    VkPipeline mPrepPipe = VK_NULL_HANDLE, mSkinPipe = VK_NULL_HANDLE, mWeldPipe = VK_NULL_HANDLE;

    // Buffers.
    GpuBuffer mStretch;               // host-visible
    uint32_t  mStretchRows = 0;
    GpuBuffer mSkinOut;
    uint64_t  mSkinOutClusters = 0;
    uint64_t  mSkinShrinkSince = 0;
    GpuBuffer mScratch;
    uint32_t  mScrInstCap = 0, mScrMapCap = 0, mScrOwnedWords = 0, mScrWeldCap = 0, mScrWeldCells = 0;
    uint32_t  mMapBase = 0, mOwnedBase = 0, mWeldBase = 0;
    GpuBuffer mReadback[3];           // per slot: the scratch header
    bool      mReadbackValid[3] = {};
    uint64_t  mPeakBytes = 0;

    // Frame inputs / state.
    LLClusterSkinIn mIn;
    uint32_t mSlot = 0;
    uint64_t mFrame = 0;
    uint32_t mFrame32 = 0;
    bool mBoundsRecorded = false;     // this frame's margins / stretch were recorded
    uint64_t mPreskinFrame = ~0ull;   // frame of the last complete recordPreskin (#702 integration)
    bool mPreskinNeeded = false;      // admitted instances exist this frame
    bool mGatherBroken = false;
    uint32_t mInstCount = 0;          // ClusterInstances this frame (1b)
    uint32_t mMapCap = 0;             // selected entries the map must cover (1b's arena)
    uint32_t mRenderInstWords = 0;    // owned bitset words (render instances / 32)
    uint64_t mWantClusters = 0;       // arena clusters the admitted avatars need
    AvTally  mAv[kMaxAvatars];
    struct InstNote
    {
        uint32_t av = 0, inst = 0;
        uint32_t clusters = 0;
        bool bad = false, self = false;
        // (#798 weld probe) the mesh key, its skin and the build-source store slot
        LLUUID   mesh_id;
        int32_t  face = 0;
        uint32_t skin_id = LLClusterFmt::kNone, src_slot = LLClusterFmt::kNone;
    };
    // #798 WELD PROBE (RenderVKClusterWeldProbe, one-shot): a CPU replica of the weld's candidate
    // flags (legacy slot flags vs the cluster builder's) and its pairing (rest pose and bind space)
    // over one admitted avatar's meshes, logged ("ClusterWeldProbe").
    void weldProbe(uint32_t mode);
    std::vector<InstNote> mAvInst;    // per noted instance (gatherer order)
    // Skinned arena DEMAND (2026-10-14, the cluster path replaces legacy): the arena follows the
    // clusters the pre-skin actually handed out (header CLK_USED, may exceed the capacity: the
    // overflow), not the sum of every admitted mesh's DAG (that upper bound filled the 256 MB cap
    // at 129k clusters for 21k skinned and kept 14 avatars legacy at PEAK).
    uint64_t mDemandPeakCur = 0, mDemandPeakPrev = 0;
    uint64_t mDemandWinFrame = 0;
    uint32_t mOverflowLast = 0;       // last harvested frame's PARTIAL drops (#1010: expected 0)
    // #1010 ARENA PRESSURE / WHOLE-AVATAR ADMISSION (llcleanclusterskinvk.h ARENA PRESSURE)
    bool     mShortLast = false;      // last harvested frame: new entries exceeded the free list
    uint32_t mRefusedLast = 0, mHiddenLast = 0;   // ... avatar rows refused whole, entries hidden
    bool     mHdrFresh = false;       // this beginFrame harvested a header
    std::vector<uint32_t> mPrioOrder; // admit(): this frame's avatar rows, most important first (<= kSkinAvRows)
    uint32_t mPrioBase = 0;           // scratch word of the PRIORITY region
    float    mArenaP = 0.f;           // the pressure exponent (0 .. kArenaPMax)
    double   mNow = 0.0;
    double   mArenaRaiseAt = 0.0, mArenaUnderSince = 0.0, mArenaReleaseAt = 0.0, mArenaChangeAt = 0.0;
    uint32_t mArenaFlip = 0;
    uint64_t mArenaLimit = 0;         // clusters (this frame's limit)
    uint32_t mArenaSettingMB = 0;     // RenderVKClusterSkinArenaMB as read (0 = auto)
    uint64_t mArenaAutoBytes = 0;     // the auto limit before the ladder (bytes)
    double   mGrowFailAt = -1.0e9;    // a failed arena growth (held ~2 s, then retried)
    uint32_t mAvCoarse = 0;           // avatars with an instance coarsened by the pressure (gather)
    uint64_t mArenaPeakBytes = 0;     // high-water of the arena's allocation
    uint64_t mArenaDemandPeakEver = 0;   // high-water of the demand (clusters)
    // the 10 s summary window
    uint64_t mWinDropped = 0, mWinHidden = 0;
    uint32_t mWinRefusedMax = 0, mWinShortFrames = 0, mWinCoarseMax = 0, mWinFrames = 0;
    float    mWinPMax = 0.f;
    // per-episode warnings (re-armed after kEpisodeQuiet s without the condition)
    bool     mDropEpisode = false, mRefuseEpisode = false;
    double   mDropQuietSince = 0.0, mRefuseQuietSince = 0.0;
    uint64_t mDropEpisodeSum = 0, mRefuseEpisodeHidden = 0;
    uint32_t mRefuseEpisodePeak = 0;
    double   mRefuseEpisodeStart = 0.0;
    double   mGrowHideLogAt = -1.0e9;   // last "arena growing: avatars hidden" INFO (below the limit, <= 1 / 10 s)
    // Weld DEMAND (2026-10-14): candidates the pre-skin tried to insert (header CLK_WELD_N), peak
    // over the same two windows; the last harvested frame's drops.
    uint64_t mWeldPeakCur = 0, mWeldPeakPrev = 0;
    uint32_t mWeldDropLast = 0;
    // #798 seam diagnostics, latched on the sampled frames: candidates, applied, unwelded by
    // reason (no partner within eps / skin weights differ / apart past the max gap), candidates of
    // border-free clusters and the avatars with any
    uint32_t mWeldDiag[14] = {};  // (+ [7] paired at rest, [8] no rest row, [9] no-op, [10] multi: #798;
                                  //  [11] cached, [12] invalid, [13] searched: #826)
    uint32_t mPrepassWait = 0;       // consecutive frames nothing was drawn for missing pre-pass pipelines
    uint32_t mLayerBits[kAvatarSlotWords] = {};   // avatars with >= 2 eligible rigged meshes (#611)
    uint32_t mLayerBase = 0;          // scratch word of the layer bits (PrepPush layer_base; uploaded by recordBounds)
    uint64_t mScrShrinkSince = 0;

    // Stats.
    uint32_t mAvCluster = 0, mAvLegacy = 0;
    uint32_t mAdmittedBits[kMaxAvatars / 32] = {};   // admit(): the avatars on the cluster path
    uint32_t mWhy[WHY_COUNT] = {};
    uint32_t mInstAdmitted = 0;
    uint32_t mPrevMode = 0;           // last pre-skin's previous positions: 0 none, 1 palettes, 2 = current
    uint32_t mLastHdr[kHdrWords] = {};
    bool     mLegacySampled[3] = {};      // #748 perf pass 3: that slot's frame ran the legacy-verts statistic
    uint32_t mLegacyVertsLast = 0, mLegacyItemsLast = 0;   // its last sample
    // Step 0 (2026-10-14): what the c_cl_preskin_begin -> c_cluster_preskin window recorded last
    // frame (dispatches, pipeline barriers, buffer fills / copies), for the stats line.
    uint32_t mWinDispatches = 0, mWinBarriers = 0, mWinXfers = 0;
    uint32_t mWinWeldGroups = 0;      // last harvested frame's weld groups (CLK_WELD_DISPATCH x)
    bool     mSkinVertsSampled[3] = {};   // that slot's frame counted CLK_SKIN_VERTS (every 16th frame)
    uint32_t mSkinVertsLast = 0, mSkinClustersLast = 0;
    // STABLE SLOTS (step 1): the arena was (re)created -- its slot meta must be filled free before
    // the sweep reads it; last harvested frame's slots kept by the sweep / kept in the cut / new.
    bool     mArenaReset = true;
    uint32_t mSlotLiveLast = 0, mSlotKeptLast = 0, mSlotNewLast = 0;
    uint32_t mArenaResets = 0;        // arena (re)creations: every slot re-skinned once
    // HELD POSES (step 3): per character row its pose generation (a new globally unique value
    // whenever the row may have changed pose; uploaded every pre-skin), the counter, the frame of
    // the last upload (a gap re-generates every row), last frame's admitted rows held / re-skinned.
    uint32_t mPoseGen[kSkinAvRows] = {};
    uint32_t mPoseGenCounter = 0;
    uint64_t mPoseGenFrame = ~0ull;
    uint32_t mCharsHeld = 0, mCharsSkinned = 0;
    uint32_t mReusedLast = 0;
    bool     mReuseOn = true;         // RenderVKClusterSkinReuse at the last pre-skin
    mutable uint32_t mBarrierCount = 0;   // barrier() calls (the window: the difference across recordPreskin)

    // ---- #798 REST TABLE (set 1 binding 5; clean_cluster_preskin.comp rest_w) -------------------
    // The weld's PAIRING SPACE is legacy mode 1's (RenderVKRigWeldMode 1, the default): each
    // boundary vertex skinned at its avatar's REST pose. Per joint-table slot of an admitted
    // avatar a rest joint row (LLRigRest::buildRow, the legacy discovery's rows), rebuilt on the
    // legacy triggers: a new owner of the slot, the skeleton serial (shape sliders, >= 300 frames
    // apart) and the bones' rest hash (joint overrides, checked >= 120 frames apart, a few slots a
    // frame). Layout: [0, kMaxAvatars) row per slot (kNone = none: the avatar inserts no weld
    // candidate) | rows x LLRigRest::kRowWords. CAPACITY FOLLOWS DEMAND: rows = the live ones + a
    // quarter + 8, in kRestRowStep steps (grows at once, shrinks with hysteresis -- compacting);
    // a slot not admitted for kRestGrace frames frees its row. The CPU mirror is the source of
    // every upload (dirty rows + the header via vkCmdUpdateBuffer in recordPreskin; a re-created
    // buffer uploads everything once). Device-local, VKMEM_RIG.
    struct RestSlot
    {
        LLUUID   owner;
        uint32_t row = LLClusterFmt::kNone;   // its row (kNone: none)
        uint32_t skel = 0;             // skeleton serial / bone hash at the last build
        uint64_t bones = 0;
        uint32_t built = 0;            // frame of the last build
        uint32_t seen = 0;             // frame last admitted
        uint32_t next_try = 0;         // no avatar behind the slot: retry from this frame
        bool     want = false;         // (re)build wanted
    };
    static constexpr uint32_t kRestRowStep = 16;
    static constexpr uint32_t kRestGrace = 300;      // frames a non-admitted slot keeps its row
    static constexpr uint32_t kRestBuildsPerFrame = 8;
    RestSlot  mRestSlot[kMaxAvatars];
    GpuBuffer mRest;
    uint32_t  mRestCap = 0;                          // rows the buffer holds
    std::vector<uint32_t> mRestCpu;                  // the mirror: header + mRestCap rows
    std::vector<uint32_t> mRestFree;                 // free rows
    std::vector<uint32_t> mRestDirtyRows;            // rows to upload (recordPreskin)
    bool      mRestHdrDirty = true;                  // the header (and, after a re-creation, every row)
    bool      mRestAll = true;                       // upload every row (a new buffer)
    bool      mRestOn = false;                       // rest-pose pairing this frame (RenderVKRigWeldMode 1)
    uint32_t  mRestTouched[kAvatarSlotWords] = {};   // slots whose row changed: re-skin their rows whole
    // #826 WELD PAIR CACHE: the rows to re-pair this frame (rest rows changed; every row when the
    // pairing parameters changed or the cache was just turned on), uploaded when non-zero or when
    // the last upload was; the parameters the stored pairs were made with
    uint32_t  mRepair[(kSkinAvRows + 31u) / 32u] = {};
    bool      mRepairUploaded = false;
    bool      mPairOn = false;
    uint32_t  mPairParamHash = 0;
    uint32_t  mRestOwnersFrame = 0, mRestRR = 0;
    uint64_t  mRestShrinkSince = 0;
    uint32_t  mRestLive = 0, mRestPeak = 0;
    // last ~30 s: builds by trigger (new row, new owner, shape, bones), no avatar behind the slot
    uint32_t  mRestBuilds[4] = {}, mRestNoAv = 0;
    uint32_t  mRestBuildsLast[4] = {}, mRestNoAvLast = 0;
    uint64_t  mRestStatsFrame = 0;
};
