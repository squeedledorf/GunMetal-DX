/**
 * @file llcleanclusterstreamvk.h
 * @brief Cluster LOD (#642 / #746, OWNER: 1c): residency -- the mesh registry, the page
 *        pool, the page table, budgeted page uploads, request readback and eviction.
 *
 * Owns (allocates, registers with LLCleanClusterVK::setBuffer, retires):
 *   CL_B_POOL        the page pool: ONE device buffer of N x kPageBytes (N <= kMaxPoolPages,
 *                    bytes <= caps().maxStorageBufferRange), usage STORAGE | TRANSFER_DST |
 *                    TRANSFER_SRC (growth copy) | VERTEX_BUFFER | INDEX_BUFFER (+ the AS-build
 *                    input usage when the device can ray trace; every buffer has a device
 *                    address). The pool GROWS IN STEPS (x2, new buffer + prefix copy + retire;
 *                    slot indices survive) up to its target: RenderVKClusterPagePoolMB, or
 *                    (0 = auto) a sixth of the VRAM room at first use, 64 MB .. 2 GB. A live
 *                    setting change below the current size rebuilds it (every mesh
 *                    re-registers; the legacy path draws meanwhile). Category VKMEM_GEOMETRY.
 *   CL_B_PAGE_TABLE  uint[kMaxVirtualPages] virtual page -> pool slot (kNone = not resident),
 *                    filled with kNone by the first recordUploads. VKMEM_SCENE.
 *   CL_B_MESHES      ClusterMesh[] directory (capacity grows x2 up to kMaxMeshes). VKMEM_SCENE.
 *   CL_B_GROUPS      ClusterGroup[] (each mesh's table at mesh.group_base; first-fit range
 *                    arena, capacity grows x2 up to kMaxGroupsTotal). VKMEM_SCENE.
 *   CL_B_REQUESTS    PER FRAME SLOT: ClusterRequestHeader + ClusterRequest[kRequestCap] in
 *                    HOST-VISIBLE memory (VKMEM_CULL), registered per slot (setBuffer offset).
 *                    When a slot comes round (its fence waited) the CPU reads the requests the
 *                    slot's frame appended and resets the header ({0, cap, frame}) before the
 *                    slot is submitted again -- the readback ring of the contract without a
 *                    GPU copy or GPU reset, so nothing on the GPU side has to be ordered
 *                    against the selection that appends (also under frame pipelining).
 *   CL_B_PAGE_USAGE  uint[pool pages]: last frame a selection touched each pool slot (LRU),
 *                    copied each frame into a per-slot readback (VKMEM_READBACK) harvested
 *                    kFrameSlots frames later (round counter as the core stats).
 * Plus a host-visible staging ring (VKMEM_STAGING, kFrameSlots regions of the upload budget
 * + an update area) and a 2-per-slot timestamp query pool (upload GPU time).
 *
 * RESIDENCY RULES (llcleanclusterformat.h G4-G6), per page state:
 *   ABSENT -> LOADING (cache read / payload copy) -> LOADED (CPU bytes) -> STAGED (copy
 *   recorded in frame F) -> RESIDENT (table flipped when F's slot comes round again: its
 *   fence was waited, so the copy is complete -- fence-gated) -> COOLING (table flipped to
 *   kNone at frame E; the pool slot is reusable at E + kFrameSlots) -> ABSENT.
 *   - a mesh's root pages [0, root_pages) are made resident at registration and pinned;
 *     acquireMesh returns kNone until every root page is RESIDENT (not drawn meanwhile:
 *     2026-10-14, the cluster path replaces the legacy one); the first kWarmPages non-root
 *     pages are requested right after the roots but not waited for;
 *   - CLOSURE: every page counts its non-ABSENT children (dependents). A page is loaded only
 *     with all its parents present (missing parents are pulled in first, G5 order), flips to
 *     RESIDENT only when every parent is RESIDENT (coarse -> fine in one batch), and is
 *     evicted only when no child is present -- a child's count is released when its slot
 *     is freed (COOLING -> ABSENT), never at the flip, so frames in flight keep closure;
 *   - LRU from the CL_B_PAGE_USAGE readback, under the pool's EFFECTIVE cap (pressure).
 * PAGE SOURCES: the build payload while held, else the disk cache. The payload is released
 *   only once LLCleanClusterCache::pageSource says the disk holds that build (a lower
 *   creator LOD build is never persisted: its payload is kept while the mesh lives, released
 *   only under RAM pressure). A wanted page with no source stays ABSENT (parent draws) and
 *   never blocks other loads. A Live mesh is rebuilt (maybeRebuild) when the face publishes
 *   a higher creator LOD than its DAG's, or (RAM calm) when a wanted page has no source.
 * UPLOADS: RenderVKClusterUploadMBPerFrame of page bytes per frame, priority = the request's
 * projected error (px), roots first. Tiers (caps().get(CLCAP_UPLOAD)):
 *   COPY      staging ring + one vkCmdCopyBuffer with a region per page;
 *   INDIRECT  the region list is written into the staging ring and executed by
 *             vkCmdCopyMemoryIndirectKHR (or the NV entry point);
 *   DECOMPRESS detected but held at INDIRECT: no GDeflate encoder is in the tree, so the cache
 *             never holds kCompGDeflate pages. TODO(1c): vendor a permissively licensed
 *             GDeflate encoder (1a writes kCompGDeflate), then decompress straight from the
 *             staging ring with vkCmdDecompressMemory*.
 * QUEUE: page copies go to setUploadCmd()'s command buffer when one is given (an async
 * compute segment: the copies target pool slots no frame references until their fence-gated
 * flip, so the segment needs no join before the selection), else recordUploads' command
 * buffer -- itself an async compute buffer under frame pipelining (#749: the renderer records
 * the frame start inside the front-end segment). Indirect copies and timestamps are used only
 * where the queue family of the buffer that records them supports them (graphics family /
 * async family facts, vkAsyncIsAsyncCmd). A pool growth waits until no copy recorded on a
 * forked async segment is younger than kFrameSlots frames (copies stay on the graphics
 * buffer meanwhile), and the growth frame's copies record after its prefix copy (#749).
 * DROPPED FRAMES (#697 / #749): frameDropped() -- none of the lost recording's copies, table /
 * directory / group writes or fills may have run, so residency restarts (resetResidency) and
 * the usage stamps are zeroed again; every page re-streams, the legacy path draws meanwhile.
 * MESH LIFETIME: a mesh no instance acquired for RenderVKClusterMeshGraceSec is unregistered
 * (pages, table entries, groups, directory slot freed after kFrameSlots; generation bumped;
 * nothing kept for a return) -- free on liveness.
 * PRESSURE (soft): the VRAM ladder / room lowers the pool's effective cap (evict down;
 * restored after 2 s calm, settings untouched); RAM pressure (LLMemoryGovernor) shrinks the
 * CPU-side page buffers and the retained build payloads. A failed allocation never crashes:
 * retried with backoff, the pool runs at the size it has, logged.
 */
#pragma once

#include "vk_device.h"
#include "../llcleanclusterformat.h"
#include "../llcleanclustercache.h"
#include "../llcleanclusterbuild.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class LLCleanClusterVK;
struct LLClusterFrameIn;

class LLCleanClusterStreamVK
{
public:
    explicit LLCleanClusterStreamVK(LLCleanClusterVK& owner);
    ~LLCleanClusterStreamVK();

    bool init();                         // entry points + tier facts; buffers are lazy (beginFrame)
    void destroy();                      // device idle

    // ---- mesh registry (called by 1b's instance gatherer, render thread) ----
    struct MeshRef
    {
        uint32_t mesh = LLClusterFmt::kNone;   // ClusterMesh directory index (kNone = not yet)
        uint32_t generation = 0;               // ClusterInstance::mesh_generation
    };
    // The registered cluster mesh for `key`, or kNone while it is loaded / built / its root
    // pages are uploaded (the instance stays on the legacy path meanwhile). src_slot: the
    // store slot of the face's highest resident LOD (the build source on a cache miss).
    // Marks the mesh live this frame. importance / group: the BUILD PRIORITY of a cache miss
    // (LLCleanClusterBuilder PRIORITY: larger first; group = the avatar, kNone = none); several
    // callers in one frame (one mesh worn by several avatars): the largest importance wins.
    // hint (optional, in / out): the directory index this key had last time (the caller's
    // cache, kNone = unknown). A hint whose entry still holds the key (not Free) skips the
    // key hash lookup; the index used is written back.
    MeshRef acquireMesh(const LLClusterKey& key, uint32_t src_slot, uint64_t frame, float importance = 0.f,
                        uint32_t group = LLClusterFmt::kNone, uint32_t* hint = nullptr);
    // Builder backlog the stream sees (the CLOD summary's build part): meshes waiting for their
    // build source (no CPU mirror / LOD yet), registrations refused last frame (registry full).
    uint32_t buildWaitSource() const { return mWaitSource; }
    // CLUSTER-OWNED MIRROR RELEASE (2026-10-16): Live meshes whose DAG is DURABLE -- built from
    // the highest creator LOD (source_lod 3) and on disk (LLCleanClusterCache::pageSource Ready /
    // Full: the .clod write landed, or the metadata came from the file) -- so the mesh store's
    // CPU mirror of that face is no longer their build source. Each such key is offered once
    // every kMirrorRecheckFrames (a LOD captured later is offered too); the renderer frees the
    // mirror where the face's LOD group is cluster-routed only (LLCleanMeshAsset::
    // queueClusterMirrorRelease). A mesh that is not durable (lower-LOD build, write refused /
    // failed / evicted, read-only or disabled cache) is never offered: its mirror stays.
    void takeMirrorCandidates(std::vector<LLClusterKey>& out) { out.clear(); out.swap(mMirrorCand); }
    uint32_t regRefused() const { return mRegRefusedLast; }
    // 2a (#747): the cluster count (every DAG level: an upper bound of one instance's cut) of a
    // LIVE registered mesh (directory index), 0 otherwise. Sizes the skinned cluster arena.
    uint32_t meshClusterCount(uint32_t dir) const
    {
        return (dir < mMesh.size() && mMesh[dir].state == MS::Live && mMesh[dir].data)
             ? mMesh[dir].data->header.cluster_count : 0u;
    }

    // ---- per frame ----
    // Harvest this slot's readbacks (requests, page usage of kFrameSlots frames ago), poll
    // the cache and the builder, register finished meshes, unregister dead ones, choose
    // evictions + uploads within budget.
    void beginFrame(const LLClusterFrameIn& in);
    // Optional, before tick(): record this frame's page copies into `cmd` (an async compute
    // segment the caller ends) instead of the graphics command buffer. Reset every frame.
    void setUploadCmd(VkCommandBuffer cmd) { mUploadCmd = cmd; }
    // #749: page copies the plan of this frame holds that may go to a forked async segment
    // (0 in a growth frame or while a growth waits: those stay on graphics). The renderer
    // forks an async copy segment only when there are some.
    uint32_t plannedCopies() const
    {
        return (mFrameOpen && !mGrowWaitAsync && mLastGrowFrame != mFrame) ? (uint32_t)mCopies.size() : 0u;
    }
    // #749 (#697): the recording of the last beginFrame / recordUploads was dropped.
    void frameDropped();
    // #749: compact fragment for the cluster summary line ("pages res N/M req R/s up X MB/s").
    void summary(std::string& out) const;
    // 2026-10-14: " | ttfd n N p50 X p95 Y max Z ms (cache .., built ..)" -- per mesh, first
    // acquire -> roots resident (drawable), since the last call (resets the window).
    void ttfdSummary(std::string& out);
    // Record: table / mesh / group updates, usage readback, page copies, then one barrier
    // (TRANSFER -> COMPUTE | VERTEX | INDEX | TASK | MESH).
    void recordUploads(VkCommandBuffer cmd);

    // Off / pressure: drop all non-root pages (returns bytes freed). Soft-degrade only.
    uint64_t trim();
    // RenderVKClusterLOD off: unregister every mesh and retire every buffer (the module is
    // deleted once the retired buffers are freed). Turning it on again re-creates lazily.
    void releaseAll();
    void statsLine(std::string& out) const;
    uint64_t heldBytes() const;
    // Fixed-cost audit (2026-10-14, the 10 s "CLOD mem" line): per buffer "name live/cap" MB
    // (live = what the demand uses, cap = what is allocated), compact.
    void memSummary(std::string& out) const;

    // 2026-10-14 VRAM by path: pool pages allocated / in use / the demand (working set + roots +
    // slack) / the pool size the demand asks for.
    uint32_t poolPages() const { return mPoolPages; }
    uint32_t poolUsedPages() const { return mUsedSlots; }
    uint32_t poolDemandPages() const { return mDemandPages; }
    uint32_t poolWantPages() const { return mPoolWant; }
    // For consumers that bind the pool / table directly (index buffer of the MDI_U8 tier,
    // CLAS build inputs). Valid for the current frame; may change on growth (retired).
    const GpuBuffer& pool() const { return mPool; }
    const GpuBuffer& pageTable() const { return mTable; }

private:
    static constexpr uint32_t kSlots = 3;   // = LLCleanClusterVK::kFrameSlots (static_assert in .cpp)

    enum class MS : uint8_t { Free = 0, Meta, Building, Ready, Rooting, Live, Failed };
    enum PS : uint8_t { PG_ABSENT = 0, PG_LOADING, PG_LOADED, PG_STAGED, PG_RESIDENT, PG_COOLING, PG_BAD };

    struct Page
    {
        uint32_t slot = LLClusterFmt::kNone;   // pool slot (STAGED / RESIDENT / COOLING)
        uint32_t last_used = 0;                // frame stamp (usage readback / upload)
        uint32_t loaded = LLClusterFmt::kNone; // index into mLoaded (LOADED)
        uint32_t want_frame = 0;               // last frame it was wanted (plan dedupe)
        float    want_prio = 0.f;
        uint32_t used = 0;                     // payload bytes staged (page fill stats)
        uint16_t dependents = 0;               // children not ABSENT / BAD (closure)
        uint8_t  st = PG_ABSENT;
        bool     root = false;
    };
    struct Mesh
    {
        LLClusterKey key;
        MS state = MS::Free;
        uint32_t generation = 1;               // never 0; bumped at every unregister
        uint32_t src_slot = 0;
        uint64_t live_frame = 0;
        double   live_time = 0.0;
        uint64_t retry_frame = 0;              // Meta / Ready / Failed back-off
        double   state_time = 0.0;             // entered the current state
        std::shared_ptr<const LLClusterMeshData> data;   // metadata (+ payload while retained)
        bool     payload = false;              // data carries the page payload
        bool     rebuilding = false;           // Live: a rebuild was asked (higher creator LOD / lost page source)
        bool     no_source = false;            // a wanted page had no source (no payload, not on disk)
        int8_t   rebuild_lod = -1;             // creator LOD of the last rebuild asked (one per step)
        bool     wait_source = false;          // Meta: the builder refused for no source data yet
        float    prio = 0.f;                   // build priority (acquireMesh, the largest this frame)
        uint32_t prio_group = LLClusterFmt::kNone;
        uint64_t prio_frame = ~0ull;
        float    req_prio = -1.f;              // Building: the priority / slot last passed to the builder
        uint32_t req_slot = LLClusterFmt::kNone;
        uint32_t lod_slot = LLClusterFmt::kNone;   // src_slot whose creator LOD was last checked
        uint64_t mirror_check = 0;             // next frame this Live mesh may be offered (takeMirrorCandidates)
        double   rebuild_time = 0.0;
        bool     rebuilt_ready = false;        // the rebuild finished: `rebuilt` waits for advanceMeshes
        std::shared_ptr<const LLClusterMeshData> rebuilt;
        bool     validated = false;
        bool     from_builder = false;         // data came from the builder (validated on its worker)
        double   payload_until = 0.0;
        bool     registered = false;           // ranges + records live on the GPU
        uint32_t page_base = 0, group_base = 0;
        uint32_t roots_resident = 0;
        std::vector<Page> pages;
        double   first_time = 0.0;             // first acquire of this entry (time to drawable)
        bool     ttfd_done = false;            // its time-to-drawable sample was taken
        // CLEAN UPGRADE SWAP (#798): a rebuild from a higher creator LOD registers as a SHADOW entry
        // (not in mByKey, never acquired) while this Live one keeps drawing; when the shadow's roots
        // are resident it takes the key over and this one is released -- no frame without the mesh.
        uint32_t shadow = LLClusterFmt::kNone;     // (live entry) its pending replacement (kShadowQueued: being made)
        uint32_t shadow_of = LLClusterFmt::kNone;  // (shadow) the live entry it replaces
    };
    static constexpr uint32_t kShadowQueued = 0xFFFFFFFEu, kShadowInPlace = 0xFFFFFFFDu;
    std::vector<uint32_t> mShadowQ;            // live entries whose rebuilt data waits for a shadow entry
    std::vector<LLClusterKey> mMirrorCand;     // takeMirrorCandidates (bounded: kMirrorCandMax)
    static constexpr uint64_t kMirrorRecheckFrames = 600;
    static constexpr size_t   kMirrorCandMax = 4096;
    uint32_t allocDir();                       // a free directory entry (kNone: none / at the cap)
    void startShadows();                       // after advanceMeshes' loop (it may grow mMesh)
    bool shadowSwap(uint32_t idx);             // a shadow's roots are resident: take the key over
    // #798: Live meshes per DAG source creator LOD (0..3; [4] = unknown), refreshed by advanceMeshes
    uint32_t mSrcLod[5] = {};
    uint32_t mShadows = 0;                     // upgrade swaps waiting for their roots
    struct Ttfd { float ms = 0.f; bool built = false; };
    std::vector<Ttfd> mTtfd;                   // samples since the last summary (bounded)
    uint32_t mTtfdDropped = 0;
    struct PageRef { uint32_t mesh = 0, gen = 0, page = 0; };
    struct Loaded
    {
        PageRef ref;                           // ref.mesh kNone = free entry
        std::vector<uint8_t> bytes;            // kPageBytes (tail zero)
        uint32_t used = 0;
        float prio = 0.f;
        uint64_t frame = 0;
    };
    struct Ticket { PageRef ref; uint64_t frame = 0; };
    struct Deferred                            // released when this slot comes round again
    {
        bool ts_async = false;                 // #749: the timestamps were written on the async family
        std::vector<PageRef> staged;           // copies recorded: flip to RESIDENT
        std::vector<PageRef> cooling;          // flipped to kNone: -> ABSENT, release parents
        std::vector<uint32_t> slots;           // pool slots to free
        std::vector<std::pair<uint32_t, uint32_t>> vranges, granges;
        std::vector<uint32_t> dirs;            // directory indices to free
        bool usage_copied = false;             // usage readback written for this slot
        bool ts_written = false;               // upload timestamps written for this slot
        uint32_t usage_pages = 0;              // pool pages the copy covered
    };
    struct RangeAlloc                          // first-fit, merged free list
    {
        uint32_t cap = 0, used = 0;
        std::vector<std::pair<uint32_t, uint32_t>> free;   // (start, count), sorted
        void reset(uint32_t c) { cap = c; used = 0; free.assign(1, {0u, c}); }
        uint32_t alloc(uint32_t n);
        void release(uint32_t start, uint32_t n);
        uint32_t highWater() const;             // first index past the last allocated one
    };
    struct TableWrite { uint32_t vpage, value; };
    struct MeshWrite { uint32_t dir; LLClusterFmt::ClusterMesh rec; };
    struct GroupWrite { uint32_t base; std::shared_ptr<const LLClusterMeshData> data; };
    struct PageCopy { uint32_t staging_off, slot, bytes; };
    struct BufFill { VkBuffer buffer; VkDeviceSize offset, size; uint32_t value; };
    struct GrowCopy { GpuBuffer from, to; VkDeviceSize bytes; };   // recorded in order, then `from` retires

    // settings / budgets
    void readSettings(const LLClusterFrameIn& in);
    uint32_t effectiveCap() const;
    uint32_t pendingFree() const;              // slots already on their way back (deferred frees)
    // buffers
    bool ensureBuffers(const LLClusterFrameIn& in);
    bool ensurePool(const LLClusterFrameIn& in);
    bool growPool(uint32_t pages);
    // 2026-10-14 VRAM (the pool follows its DEMAND): the working-set scan, the LRU trim of cold
    // pages over the demand, and the shrink (slots past the new size drained, then a smaller pool
    // with the prefix copied -- the slot indices below it survive).
    void demandTick();
    bool shrinkPool(uint32_t pages);
    uint32_t growStep(uint32_t need_free) const;   // the next pool size for `need_free` more slots
    bool ensureDirCap(uint32_t n);
    bool ensureGroupCap(uint32_t n);
    bool ensureStaging();
    void releaseBuffers();
    void resetResidency(const char* why);
    // per frame
    void harvest(uint32_t slot);
    void pollSources(const LLClusterFrameIn& in);
    void advanceMeshes(const LLClusterFrameIn& in);
    bool tryRegister(uint32_t idx);
    void unregister(uint32_t idx, bool keep_entry);
    void freeEntry(uint32_t idx);
    void livenessSweep();
    void takeData(Mesh& m, const std::shared_ptr<const LLClusterMeshData>& data);
    void maybeRebuild(uint32_t idx, uint32_t& asks);
    void rebuildDone(uint32_t idx, const std::shared_ptr<const LLClusterMeshData>& data);
    void planUploads();
    void wantPage(uint32_t mesh, uint32_t page, float prio, uint32_t depth);
    bool startLoad(uint32_t mesh, uint32_t page);
    void pageGone(uint32_t mesh, uint32_t page);   // -> ABSENT, release parents
    void addParents(uint32_t mesh, uint32_t page, int delta);
    bool parentsAll(uint32_t mesh, uint32_t page, uint8_t min_state) const;
    bool evictPage(uint32_t mesh, uint32_t page);
    uint32_t evictLRU(uint32_t want, uint32_t min_age, bool force);
    bool validPage(const Mesh& m, uint32_t page, const uint8_t* bytes, uint32_t& used) const;
    void failMesh(uint32_t idx, const char* why, bool drop_cache);
    void writeMeshRecord(uint32_t idx, bool live);
    uint32_t meshOf(uint32_t vpage, uint32_t& local) const;
    uint32_t updRoom() const;                  // update-area bytes left this frame
    void logOnce(const std::string& what, const std::string& text);

    LLCleanClusterVK& mOwner;
    VkCommandBuffer mUploadCmd = VK_NULL_HANDLE;

    // ---- GPU buffers ----
    GpuBuffer mPool, mTable, mMeshes, mGroups, mReq, mUsage, mUsageRead, mStaging;
    uint32_t mPoolPages = 0;           // allocated slots
    uint32_t mPoolTarget = 0;          // growth target (pages)
    uint32_t mPoolSettingMB = ~0u;     // RenderVKClusterPagePoolMB the target was made from
    uint32_t mPoolPeak = 0;            // high-water of used slots
    uint64_t mLastGrowFrame = 0;
    // demand (demandTick): resident pages used in the last kColdAge frames (non-root) + roots, the
    // resident target derived from it, the pool size it asks for, and the shrink in progress.
    uint32_t mWsPages = 0, mWsRoots = 0, mDemandPages = 0, mPoolWant = 0, mMaxRootSlot = 0;
    // PAGE FILL (fixed-cost audit 2026-10-14, demandTick): payload bytes of the resident pages vs
    // their 80 KiB slots -- every page is a whole slot, so a small mesh's root / warm page holds a
    // few KiB in 80 (the "pages res" MB over-counts the geometry by 1 / fill).
    uint64_t mFillBytes = 0, mFillRootBytes = 0;
    uint32_t mFillPages = 0, mFillRoots = 0;
    uint32_t mShrinkTo = 0;            // pool shrink in progress (0 none): slots >= it are drained
    double   mShrinkSince = 0.0;       // the pool has been over its want since (mNow)
    uint32_t mShrinks = 0;             // pool shrinks done (stats)
    uint32_t mShrinkBlocked = 0;       // pages the shrink could not drop: a root sits past the want
    uint64_t mDemandEvicts = 0;        // cold pages trimmed to the demand (stats)
    uint32_t mDirCap = 0, mGroupCap = 0;
    uint32_t mUsageCap = 0;            // pool pages mUsage / mUsageRead cover
    uint64_t mStagingSlotBytes = 0, mStagingPageBytes = 0;
    uint32_t mStagingMB = 0, mStagingWantMB = 0;
    // staging demand (readSettings): bytes staged per frame, peak over two windows; the ring cut a
    // frame's staging short (grow at once); the ring has been at 2x its want since (shrink)
    uint64_t mStagePeakCur = 0, mStagePeakPrev = 0, mStageWinFrame = 0;
    bool     mStagingStarved = false;
    uint64_t mStagingSmallSince = 0;
    uint64_t mStagingRetryFrame = 0;
    uint64_t mRegEvictFrame = ~0ull;            // registration made room (LRU) this frame
    bool mStagingDeviceLocal = false;
    VkQueryPool mTsPool = VK_NULL_HANDLE;
    bool mTsOk = false;
    double mTsPeriodNs = 1.0;
    uint64_t mTsMask = ~0ull;
    bool mTableFill = false, mDirFill = false;      // whole-buffer refills (reset)
    std::vector<BufFill> mBufFills;                 // fills of fresh buffers
    std::vector<std::pair<uint32_t, uint32_t>> mRangeFills;   // table ranges -> kNone (registration)
    std::vector<GrowCopy> mGrowCopies;
    uint64_t mAllocRetryFrame = 0;
    uint32_t mAllocFails = 0;

    // ---- tiers / queue ----
    PFN_vkVoidFunction mCopyIndirect = nullptr;   // vkCmdCopyMemoryIndirectKHR / NV
    bool mCopyIndirectKHR = false;
    bool mIndirectQueueOk = false;
    bool mIndirectAsyncOk = false;                // #749: indirect copies on the async compute family
    bool mAsyncTsOk = false;                      // #749: the async family writes timestamps
    uint64_t mAsyncTsMask = ~0ull;
    uint64_t mAsyncCopyFrame = 0;                 // last frame copies went to a forked async segment
    bool mGrowWaitAsync = false;                  // a growth waits for those to age: copies on graphics
    int32_t mUploadTier = 0;                      // tier used by the last recorded copies
    bool mAsyncUsed = false;                      // the last copies went to setUploadCmd()'s buffer

    // ---- registry ----
    std::vector<Mesh> mMesh;                      // index = directory index
    std::vector<uint32_t> mFreeDirs;              // reusable indices (after their quarantine)
    std::unordered_map<LLClusterKey, uint32_t, LLClusterKeyHash> mByKey;
    std::vector<std::pair<uint32_t, uint32_t>> mVRanges;   // (page_base, mesh) sorted by base
    RangeAlloc mVAlloc, mGAlloc;
    uint32_t mPending = 0;                        // entries not Live / Failed (bounded)
    uint32_t mRegRefused = 0, mRegRefusedLast = 0; // acquireMesh refusals (registry full), this / last frame
    uint32_t mWaitSource = 0;                     // Meta meshes waiting for their build source
    uint32_t mRootsPendingSlots = 0;              // root pages registered but not resident yet

    // ---- pages ----
    std::vector<PageRef> mSlotOwner;              // pool slot -> page (mesh kNone = free)
    std::vector<uint32_t> mSlotUsed;              // last usage stamp per pool slot
    std::vector<uint32_t> mFreeSlots;
    uint32_t mUsedSlots = 0;                      // STAGED + RESIDENT + COOLING (+ deferred frees)
    std::vector<Loaded> mLoaded;                  // LOADED pages (bounded by mLoadCap)
    std::vector<uint32_t> mLoadedFree;
    uint32_t mLoadedCount = 0;
    std::unordered_map<uint64_t, Ticket> mTickets;
    uint64_t mNextTicket = 1;
    Deferred mDef[kSlots];

    // ---- scratch (kept to avoid per-frame allocations) ----
    std::vector<std::pair<float, PageRef>> mWant;
    std::vector<LLClusterFmt::ClusterRequest> mReqScratch;
    std::vector<std::pair<uint32_t, uint32_t>> mEvictScratch;
    std::vector<uint32_t> mStageScratch;
    std::vector<VkBufferCopy> mRegions;
    std::vector<LLCleanClusterBuilder::Result> mBuildResults;
    std::vector<LLCleanClusterCache::PageRead> mPageReads;

    // ---- this frame's GPU work (built in beginFrame, recorded in recordUploads) ----
    std::vector<TableWrite> mTableWrites;
    std::vector<MeshWrite> mMeshWrites;
    std::vector<GroupWrite> mGroupWrites;
    std::vector<PageCopy> mCopies;
    uint32_t mUpdPlanned = 0;                     // update-area bytes planned this frame
    uint64_t mCopyBytes = 0;
    bool mFrameOpen = false;                      // beginFrame ran (recordUploads may record)

    // ---- frame / settings / pressure ----
    uint32_t mSlot = 0;
    uint64_t mFrame = 0;
    double mNow = 0.0;
    uint64_t mUploadBytesBudget = 0;
    uint32_t mLoadCap = 0;                        // LOADING + LOADED pages
    uint32_t mEffCap = 0;
    double mCalmSince = 0.0;
    uint32_t mPressureCap = 0;                    // effective cap while pressure lasts (0 = none)
    bool mVramPressure = false;
    uint64_t mVramAvail = ~0ull;
    float mGraceSec = 30.f;
    float mBuildMs = 2.f;                         // the builder's render-thread ms per frame (RenderVKClusterBuildMsPerFrame)
    uint64_t mPayloadBudget = 0, mPayloadBytes = 0;
    uint64_t mPayloadNoDisk = 0;                  // of mPayloadBytes: payloads the cache cannot serve (kept)
    bool mRamPressure = false;                    // LLMemoryGovernor TIGHT / CRITICAL

    // ---- stats ----
    struct Rate { uint64_t n = 0; double t0 = 0.0; double per_sec = 0.0; void tick(double now, uint64_t add); };
    Rate mReqRate, mReqUniqRate, mUpRate, mUpPageRate;
    uint64_t mEvictions = 0, mUploadsTotal = 0, mUploadBytesTotal = 0;
    uint64_t mStalls = 0, mReqOverflow = 0, mLoadFails = 0, mBadPages = 0;
    uint32_t mResidentPages = 0, mResidentRoots = 0, mLiveMeshes = 0;
    float mUploadGpuMs = 0.f, mUploadGpuMsMax = 0.f;
    // Stall diagnostics (last frame's plan): wanted ABSENT pages not started, by reason.
    uint32_t mWantNoSource = 0, mWantBusy = 0, mWantCap = 0;
    double mStallSince = 0.0;                     // wanted pages, nothing loading / staged since then (0 = none)
    uint64_t mRebuilds = 0, mRebuildSwaps = 0, mNoSourceTotal = 0;
    std::unordered_map<std::string, double> mLogged;   // throttled log keys (bounded)
};
