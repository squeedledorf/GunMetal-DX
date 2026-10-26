/**
 * @file llcleanrtvk.h
 * @brief Ray tracing FOUNDATION for the Vulkan clean renderer: bottom-level
 *        acceleration structures per packed LOD slot (+ per rigged instance), a
 *        top-level AS rebuilt every frame from the instance SSBO by a compute pass,
 *        and the ray-query debug views that prove it matches the raster.
 *
 * Exists only when the device enabled VK_KHR_acceleration_structure + ray_query
 * (RenderVKRayTracing, LLVulkanContext::rayTracingEnabled()); otherwise the
 * renderer never creates it and every RT path is a no-op.
 *
 * DESIGN (see llcleanrtvk.cpp for the full notes)
 *  - BLAS per SLOT, not per asset: the renderer's unit of geometry is the packed
 *    LOD slot (SlotInfo); the cull picks a slot per instance per frame, so a slot
 *    BLAS is exactly "the LOD the raster drew", shared by every instance of it.
 *    A BLAS is self-contained after its build, so packer repacks / VBO growth
 *    that MOVE a slot never invalidate it; only CONTENT changes do (slotChanged).
 *  - Lazy: the TLAS compute marks the slots it selected; the CPU reads that back
 *    two frames later and builds missing BLAS under a per-frame count/triangle
 *    budget. LRU eviction above RenderVKRTBlasBudgetMB; static BLAS compacted.
 *  - Rigged instances: per-instance BLAS over this frame's pre-skinned positions,
 *    refit (BUILD_MODE_UPDATE) while topology is unchanged, rebuilt on change and
 *    periodically. Only avatars the cull considers visible (av_vis).
 *  - Smooth terrain (#211): the slot a terrain patch draws gets its static BLAS built
 *    the same frame (no lazy feedback lag, no coarser stand-in); a slot whose vertices
 *    the VS geomorphs this frame gets a MORPH BLAS instead -- built / refit from a
 *    compute copy of the vertices morphed exactly as clean_draw.vert does, so shadow
 *    rays leave and meet the surface the raster drew.
 *  - TLAS: dense, one VkAccelerationStructureInstanceKHR per instance id (inactive
 *    = reference 0), written on the GPU, rebuilt every frame (no cached transforms,
 *    so region-crossing origin shifts need nothing).
 *  - Lifetime: every AS / buffer is retired through a per-frame-slot ring; no
 *    vkDeviceWaitIdle on the frame path.
 */
#ifndef LL_LLCLEANRTVK_H
#define LL_LLCLEANRTVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "../llavatarslots.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class LLCleanRendererVK;
class LLCleanClusterRTVK;   // TODO(2b) cluster LOD ray tracing (#748)
struct VKCullPush;
struct VKDrawPush;

class LLCleanRTVK
{
    friend class LLCleanClusterRTVK;   // TODO(2b) #748: arena / retire ring / TLAS records
public:
    explicit LLCleanRTVK(LLCleanRendererVK& r);
    ~LLCleanRTVK();

    // Pipelines + layouts. false => RT unusable (caller drops the object).
    bool init();
    // Device must be idle.
    void shutdown();

    // Extra usage for the renderer's geometry buffers (VBO / IBO) so BLAS builds can
    // read them by device address and the any-hit can read them as SSBOs.
    static VkBufferUsageFlags geometryUsage()
    {
        return VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
             | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    // Packer hooks: slot `sl`'s CONTENT changed (placed, freed, or dropped by a
    // repack). Its BLAS (if any) is retired now; the lazy path rebuilds it if the
    // TLAS pass selects it again. from_repack only feeds the statistics.
    void slotChanged(uint32_t sl, bool from_repack);
    // Renderer hook, every frame before its instance runs are cleared: the instance id
    // runs [first, second) whose CPU rows changed this frame (full = every row was
    // (re)streamed). Keeps the rigged-instance set current without scanning all rows.
    void noteInstanceRuns(const std::vector<std::pair<uint32_t, uint32_t>>& runs, bool full);
    // #568 render-world frame: the agent frame moved by `off` and every GPU instance row with
    // it (a uniform translation). Moves the stored cameras (TLAS build camera, terrain morph
    // copies) so the next TLAS update is a legal refit instead of a camera-jump rebuild.
    void noteOriginShift(const float off[3]);

    // Per frame, after the main cull + pre-skin, before any consumer: BLAS builds /
    // refits, TLAS instance compute, TLAS build, barriers to FRAGMENT|COMPUTE reads.
    void recordBuild(VkCommandBuffer cmd, const VKCullPush& cull, const VKDrawPush& draw, bool do_rigged);
    // Per frame INSTEAD of recordBuild when nothing consumes the TLAS: records no GPU
    // work (no BLAS build / compaction / refit, no TLAS instance compute or build, no rig
    // copy). Invalidates the TLAS for this frame (tlasValid() == false), keeps the retire
    // ring turning (used-slot feedback kept), and makes the next recordBuild
    // REBUILD every rigged BLAS (their positions moved while idle; no refit). Static
    // BLAS stay resident.
    void idleFrame();

    // PHASE 2 (RenderVKOverlapTlas): with the release deferred, recordBuild leaves the
    // final TLAS -> FRAGMENT|COMPUTE barrier owed, so work recorded between the build
    // and the first consumer (alpha sort, particles, shadow tiles, pre-pass, probe culls)
    // is not held behind the AS build. releaseToConsumers records it for `stages`
    // (any subset of FRAGMENT_SHADER | COMPUTE_SHADER still owed); call it before every
    // consumer. Set the mode before recordBuild; it applies to that frame.
    void setDeferRelease(bool on) { mDeferRelease = on; }
    void releaseToConsumers(VkCommandBuffer cmd, VkPipelineStageFlags2 stages);

    // RenderVKDebugView 13 (RT vs raster depth heatmap) / 14 (instance ids) / 15 (focus
    // projector visibility, RenderVKRTDebugFlicker): full-
    // screen over colorView (COLOR_ATTACHMENT_OPTIMAL), after tonemap/AA.
    void recordDebug(VkCommandBuffer cmd, VkImageView colorView, uint32_t w, uint32_t h,
                     const VKDrawPush& draw, uint32_t view);

    bool tlasValid() const;
    // The LAST recordBuild produced a TLAS. Read BEFORE this frame's recordBuild (the light pass,
    // where ReSTIR DI / GI decide), it says last frame's build succeeded -- tlasValid() is always
    // false there (it needs this frame's slot); the passes that trace re-check tlasValid().
    bool tlasBuiltLast() const { return mTlasBuilt; }
    // #697: the frame that recorded this frame's builds was DROPPED (never executed): every
    // structure it built or refit is garbage -- release them all (retired through the frame
    // ring) so the next active frame rebuilds from scratch.
    void dropAfterLostFrame() { releaseAll(); mTlasBuilt = false; }
    // RT CONSUMER set (clean_rt_common.glsl RT_SET) for raster pipelines that trace
    // (ray-traced projector shadows in the lit world / particle FS). The layout is
    // PARTIALLY_BOUND, so binding it is always legal; the shader may only TRACE when
    // tlasValid() for this frame (the renderer gates that through the projector records).
    VkDescriptorSetLayout consumerLayout() const { return mDslRT; }
    VkDescriptorSet consumerSet() const;
    // VKMem accounting: AS storage (BLAS arena + TLAS), and RT scratch/instance/aux buffers.
    uint64_t asBytes() const;
    uint64_t auxBytes() const;
    uint32_t blasResident() const { return mStaticCount + (uint32_t)mRig.size(); }
    // Opacity micromaps usable: device extension + the start-up self-test passed (#194).
    bool ommUsable() const { return mOmmOk; }

    // ---- ONE RT MEMORY BUDGET (#164) ----
    // Covers static + rigged BLAS, the TLAS and every RT working buffer. Default (setting
    // RenderVKRTBlasBudgetMB = 0) is NOT a percentage of VRAM (#1015: the old 12% share dropped
    // avatars from RT shadows with VRAM to spare): RT may use what the device-local heap budget the
    // renderer measures (setDeviceBudget; already capped by "Maximum video memory", the debug fake
    // budget and the VRAM share between viewers) has free, see setDeviceUsage; a non-zero setting
    // is used as given. Over budget RT degrades in order: evict LRU static BLAS -> builds wait ->
    // overBudget() (the renderer falls projector shadows back to shadow maps). Dropping the legacy
    // rigged BLAS (avatars leave the TLAS) is ONLY a VRAM pressure ladder rung (dropRiggedForPressure),
    // temporary, restored when the ladder calms, no setting written. The effective budget and its
    // reason are logged on every change (budgetWhy).
    void setDeviceBudget(uint64_t bytes) { mDeviceBudget = bytes; }
    // HEADROOM (2026-10-13, all cards since #1015): the process's measured device-local usage (all
    // of it, RT included). The auto budget follows the actual headroom: what RT holds + the budget's
    // free VRAM minus a reserve for textures and transient spikes (max(2 GB, 30% of the budget:
    // under every other VRAM controller's line, llcleanrtvk.cpp), at most the budget, never under
    // 384 MB. Hysteresis (grows past +5% and only while the VRAM ladder is calm, shrinks under
    // -10%); the pressure ladder still caps it (soft, nothing written).
    void setDeviceUsage(uint64_t bytes, bool ladder_calm) { mDeviceUsage = bytes; mLadderCalm = ladder_calm; }
    // VRAM PRESSURE LADDER (#268, #405; the renderer's budget manager drives it -- the RT
    // rungs of LLVramBudgetVK, applied in LLCleanRendererVK::applyBudgetCaps). These are RT's INTERNAL,
    // temporary reductions (no user setting behind them). State set here is NOT reset by
    // the idle release (that reset made RT come back at full size 15 s after every
    // pressure episode, re-trigger the pressure and flap): only clearPressure().
    //  - dropRiggedForPressure: rigged BLAS leave the TLAS (static geometry keeps
    //    shadowing). False when there is no rigged BLAS to give.
    //  - yieldForPressure: caps the budget at half of what RT holds now, but never below
    //    pressureFloor() (the working buffers + a minimum geometry share: a cap under the
    //    working set used to force overBudget() on every pressure episode, turning RT
    //    sun shadows off). False when RT has nothing left to give.
    bool dropRiggedForPressure();
    bool yieldForPressure();
    void clearPressure();
    // Lift one of the two independently (the ladder restores rung by rung, #405).
    void clearPressureCap();
    void clearRiggedDrop();
    bool pressureActive() const { return mPressureCap != 0 || mRigPressureDrop; }
    uint64_t pressureFloor() const;
    uint64_t totalBytes() const { return mStaticBytes + mRigBytes + rtAuxBytes(); }
    uint64_t budgetBytes() const;
    std::string budgetWhy() const;   // how budgetBytes() came about (the log line on a change)
    bool overBudget() const { return mOverBudget; }
    bool rigDropped() const { return mRigDropped; }
    // NO-SELF-SHADOW CLASSIFICATION (rigged RT update throttle, llcleanrtvk.cpp): once per frame
    // after the renderer's avatar visibility (rigAssignTiers: distance, on-screen px, self /
    // protected set) and BEFORE av_vis is uploaded -- sets LLCleanRendererVK::kAvVisRtNoSelf on
    // the avatars whose rigged RT update recordBuild may throttle (it throttles no other).
    void classifyNoSelf();
    uint32_t noSelfCount() const { return mNoSelfCount; }
    // CLUSTER LOD REPLACES LEGACY (2026-10-13, "never fall back"): the cluster RT traces the
    // switched avatars (rigged cluster RT wanted, the module active at some tier) -- their groups
    // are cluster-only, never a legacy BLAS (a not-ready frame traces nothing for the group).
    bool clusterTracesSwitched() const;
    // RT reads the LEGACY skinned vertices of a cluster-switched avatar this frame: only when the
    // cluster RT does not trace switched avatars (RenderVKClusterRT 0 / RenderVKClusterRTRigged 0 /
    // module inactive) and the legacy avatar class is in RT (clusterLegacySkipPlan: else RT is no
    // consumer -- "not needed", never "not confirmed").
    bool rigSwitchedLegacyAllowed() const { return !clusterTracesSwitched() && !mRigDropped && !mRigPressureDrop; }
    uint64_t rigBytes() const { return mRigBytes; }
    uint64_t staticBlasBytes() const { return mStaticBytes; }   // (2026-10-14 VRAM-by-path line)
    uint64_t clusterRtBytes() const;                             // the cluster RT arenas + working buffers
    std::string clusterRtMemLine() const;                        // (VRAM by path: its capacity vs live)
    uint64_t workingBytes() const { return rtAuxBytes(); }   // TLAS + scratch + tables + staging
    // Release everything on the next idle frame instead of after 15 s idle (the pressure
    // ladder turned every RT consumer off: give the memory back now).
    void releaseSoon() { mReleaseAsap = true; }
    // WARM-UP (#268): a cold TLAS (first use, after the idle release, after a forced
    // eviction of in-use BLAS) holds almost nothing for a while -- static BLAS are built
    // lazily from the TLAS pass's used-slot feedback (3 frame slots of latency), 64 per
    // frame. Tracing the sun against it lit EVERYTHING (no shadow at all) while the
    // cascades were skipped. sunReady() is true once the static builds have caught up
    // (several consecutive frames without hitting a build limit, few instances waiting
    // to enter the TLAS) or after a timeout; until then the renderer keeps the cascades.
    bool sunReady() const { return mWarm; }
    // CLUSTER LOD LEGACY-SKIN SKIP (renderer clusterLegacySkipPlan): avatar `av` (joint-table
    // slot) traces its CLUSTER cut in RT -- every one of its cluster-switched rigged groups took
    // its members' cluster records in TLAS pass B (mode 4 found them all) on each of the last
    // kClCovStreak harvested frames, the newest at most kClCovMaxAge RT frames ago, at a CLAS
    // tier with rigged cluster RT on. Then its legacy rigged BLAS is not needed (its groups go
    // cluster-only, no rig copy / build / refit). Read back kFrameSlots frames late.
    bool clusterRigCovered(uint32_t av) const;
    static constexpr uint32_t kClCovStreak = 8;
    static constexpr uint32_t kClCovMaxAge = 2;

    // ---- RT SCENE AUDIT (#874, llcleanrtaudit.cpp) ----
    // A DEVICE_LOST in a ray query names no object: the fault is an address. Every frame (setting
    // RenderVKRTLostAudit, default on) recordBuild copies the TLAS instance records it just built
    // and the rt_geom rows the consumers dereference into a host-visible ring (one per frame slot,
    // stamped by the GPU once the copy ran). On DEVICE_LOST (vk_common.h gVkRtLostAuditHook) the
    // audit reads that ring -- plain mapped host memory, no Vulkan call -- and checks every active
    // record's BLAS reference, the CPU slot-address / OMM / rigged tables and the fault addresses
    // against the GPU address registry (vkAddrClassify), plus the rigged index chain
    // (rt_rigmap -> rt_geom / rt_rigpos). RenderVKRTAuditNow runs it on a healthy device (the
    // slot about to be reused: its fence was waited). Process-wide: one RT foundation.
    static void auditLostHook(const VkDeviceAddress* faults, uint32_t n);   // gVkRtLostAuditHook
    static void auditLostMainLoop();                                        // gVkRtLostAuditMainLoop

private:
    // size includes `guard` (#973 RenderVKRTArenaGuard: a pattern-filled band after the AS;
    // createAS uses size - guard).
    struct Alloc { int chunk = -1; VkDeviceSize off = 0, size = 0, guard = 0; };
    // Free blocks indexed twice: by offset (coalescing on free) and by size (best-fit
    // lookup in O(log n); a first-fit walk over the offset map scanned thousands of
    // fragments per allocation once ~30k BLAS were resident). Keep them in sync via
    // freeAdd / freeErase only.
    struct Chunk { GpuBuffer buf; VkDeviceSize size = 0, used = 0; bool dedicated = false;
                   bool guardable = false;   // #973: created with TRANSFER_DST | SRC (arena guard on)
                   std::map<VkDeviceSize, VkDeviceSize> free;            // offset -> size
                   std::multimap<VkDeviceSize, VkDeviceSize> bySize; };  // size -> offset
    static void freeAdd(Chunk& c, VkDeviceSize off, VkDeviceSize size);
    static void freeErase(Chunk& c, std::map<VkDeviceSize, VkDeviceSize>::iterator it);
    struct SlotBlas
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Alloc mem;
        VkDeviceAddress addr = 0;
        uint32_t lastUsed = 0;
        uint32_t serial = 0;      // bumps on every (re)create: compaction results must match
        bool compacted = false;
        bool repackInvalidated = false;
    };
    // One multi-geometry BLAS per rigged GROUP = (owner avatar / skin, TLAS mask class):
    // a geometry per rigged face instance, in rig-entry order. sig = per member (slot,
    // slot generation, verts, indices, opaque) -- a refit needs the same geometries.
    struct RigBlas
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Alloc mem;
        VkDeviceAddress addr = 0;
        std::vector<uint32_t> sig;
        uint32_t lastSeen = 0, builtFrame = 0;
        VkDeviceSize updateScratch = 0;
        // REFIT GATE (RenderVKRigRefitGate): mFrame of the last copy + refit/build, and
        // "positions moved since then" (a dirty frame the far throttle skipped).
        uint32_t lastRefit = 0;
        bool stale = false;
        // #692: consecutive frames its periodic quality rebuild was put off (scratch
        // budget): it keeps its BLAS and refits instead, and goes over the budget after
        // kRigStarveFrames so the rebuild cannot be deferred forever.
        uint32_t qualityDefers = 0;
        // #973: the rig copy's input check collapsed one of its entries (readFeedback): the
        // next (re)plan rebuilds it from scratch instead of refitting (reset by the rebuild).
        bool forceRebuild = false;
    };
    struct RigEntryCPU { uint32_t inst, dst_base, vert_count, first_index, index_count, uv_base, blas_lo, blas_hi; };
    // SMOOTH TERRAIN MORPH BLAS (#211, see llcleanrtvk.cpp "SMOOTH TERRAIN"): per terrain
    // slot whose vertices clean_draw.vert geomorphs this frame. Owned instead of the
    // slot's static BLAS (mTerrOwned: the static path skips it); published through the
    // same slot address, so the TLAS pass needs no change.
    struct TerrMorph
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Alloc mem;
        VkDeviceAddress addr = 0;
        uint32_t inst = 0;                     // the terrain instance drawing the slot
        uint32_t slotGen = 0, vert_count = 0, index_count = 0, first_index = 0;   // build signature
        VkDeviceAddress iboAddr = 0;
        VkDeviceSize updateScratch = 0;
        float cam[3] = {1e30f, 1e30f, 1e30f}; // main camera of the last copy + build / refit
        uint32_t seen = 0;                     // mFrame the plan last wanted it
    };
    struct TerrEntryCPU { uint32_t inst, src_base, dst_base, vert_count; };   // clean_rt_instances.comp TerrEntry
    struct TerrBuild { uint32_t slot; bool update; VkDeviceSize scratch; };
    struct Retire
    {
        std::vector<VkAccelerationStructureKHR> as;
        std::vector<Alloc> allocs;
        std::vector<GpuBuffer> bufs;
        std::vector<VkMicromapEXT> micromaps;
    };
    struct CompactPending { uint32_t slot, serial, query; };

    // arena for AS storage (thousands of small BLAS; one VkDeviceMemory each would
    // blow maxMemoryAllocationCount)
    bool arenaAlloc(VkDeviceSize size, Alloc& out);
    void arenaFree(const Alloc& a);
    // kind/id label the AS in the fault-address registry (vkAddrNote).
    VkAccelerationStructureKHR createAS(const Alloc& a, VkAccelerationStructureTypeKHR type, VkDeviceAddress& addr,
                                        const char* kind, uint32_t id);

    void retireAS(VkAccelerationStructureKHR as, const Alloc& a);
    void retireBuf(GpuBuffer& b);
    void destroyRetired(Retire& r);
    void beginFrame();
    void readFeedback();
    // RenderVKRTDebugFlicker (see clean_rt_instances.comp / clean_rt_debug.frag view 15).
    void readDebugTail(const uint32_t* tail, uint32_t rig_entries);
    void readDebugStats(uint32_t slot);
    void readSunBlocker(uint32_t slot);               // view 18: centre-pixel blockers -> log
    uint32_t mDbgSunBlk[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    // force: also evict BLAS still in use (least recently used first) -- the hard cap
    // when the budget itself dropped (pressure / smaller card); evicted slots stay unbuilt
    // (builds are refused while over budget) instead of churning.
    void evictForBudget(VkDeviceSize need, uint64_t budget, bool force = false);
    void dropSlotBlas(uint32_t sl);
    void setSlotAddr(uint32_t sl, VkDeviceAddress a);
    bool ensureBuf(GpuBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool host = false, bool addr = false);
    void updateSets(uint32_t s);
    // BLAS input guard: every index of slot `sl` must address one of its vert_count
    // vertices (a build reads vertexData + index * stride with no bounds check).
    // Scanned once per slot content generation.
    bool slotIndicesOk(uint32_t sl, uint32_t first_index, uint32_t index_count, uint32_t vert_count);

    LLCleanRendererVK& mR;
    VkDeviceContext* mCtx = nullptr;
    bool mOmmOk = false;

    // ---- OPACITY MICROMAPS (#194, llcleanrtomm.cpp) ----
    // An alpha-mask instance's cut-outs depend on its slot's triangles AND its material,
    // tint and UV transform, so a micromap can't live on the shared slot BLAS: each
    // distinct (slot, alpha key) gets its own micromap + BLAS (an "OMM entry"), and pass A
    // swaps that BLAS in for the instance (omm_inst table) and drops FORCE_NO_OPAQUE so the
    // hardware resolves known-opaque / known-transparent micro-triangles itself.
    // (#1032, RenderVKRTOmmBlend) static alpha-BLEND instances get transparent-only entries
    // (alpha exactly 0 -> known transparent, else unknown: the shader still runs on every
    // visible texel); the entry's material (no cutoff) selects that bake mode.
    struct OmmKey
    {
        uint32_t slot = 0, slotGen = 0, mat = 0, tex = 0, texVer = 0, tintA = 0;
        float uv[6] = {};
        bool operator==(const OmmKey& o) const;
    };
    struct OmmEntry
    {
        OmmKey key;
        VkMicromapEXT mm = VK_NULL_HANDLE;
        Alloc mmMem, asMem;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        VkDeviceAddress addr = 0;
        uint32_t lastUsed = 0;
        bool live = false;
        GpuBuffer mmBuf;   // DEBUG RenderVKRTOmmDedicated: the micromap in its own buffer
    };
    struct OmmBuild;                                   // this frame's plan (llcleanrtomm.cpp)
    std::vector<OmmEntry> mOmm;                        // index = OMM id (addr table row)
    std::deque<std::pair<uint32_t, uint32_t>> mOmmFree;   // (id, frame freed): reused after kOmmQuarantine
    std::vector<std::pair<uint32_t, uint32_t>> mOmmDirtyRuns;   // instance runs changed since the last plan
    bool     mOmmDirtyAll = true;
    uint32_t mOmmCursor = 0;                           // round-robin refresh of instance rows
    std::unordered_multimap<uint64_t, uint32_t> mOmmByHash;
    std::unordered_map<uint32_t, std::vector<uint32_t>> mOmmBySlot;
    std::vector<uint32_t> mOmmAddrCpu;                 // 4 words per OMM id: BLAS addr lo, hi, slot, 0 (0 = none)
    std::vector<uint32_t> mSlotChangedAt;              // mFrame of each slot's last content change
    uint32_t mOmmRetired = 0;                          // this frame (section log)
    std::vector<uint32_t> mOmmInstCpu;                 // 4 words per instance: OMM id + 1 per LOD level
    uint32_t mOmmAddrDirtyLo = ~0u, mOmmAddrDirtyHi = 0, mOmmInstDirtyLo = ~0u, mOmmInstDirtyHi = 0;
    GpuBuffer mOmmAddr, mOmmInst;                      // pass A bindings 15 / 16
    GpuBuffer mOmmUp[3], mOmmData[3];                  // per frame slot: host inputs, baked data
    uint64_t mOmmBytes = 0;
    uint32_t mOmmBuilt = 0, mOmmLive = 0, mOmmUsedInst = 0;
    VkDescriptorSetLayout mDslOmmBake = VK_NULL_HANDLE;
    VkPipelineLayout mOmmBakeLayout = VK_NULL_HANDLE;
    VkPipeline mOmmBakePipe = VK_NULL_HANDLE;
    VkDescriptorSet mSetOmmBake[3] = {};
    VkDescriptorPool mPoolOmm = VK_NULL_HANDLE;
    bool mOmmTablesOn = false;    // this frame uploads the tables: pass A binds them (else the zero dummy)
    bool mOmmTableFill = false;   // tables (re)created: zero-fill before the row uploads
    VkDeviceSize mOmmUpRowsAt = 0;  // where this frame's table rows go in mOmmUp
    void ommAbortPlan();
    std::vector<OmmBuild>* mOmmPlan = nullptr;         // owned; this frame's builds
    bool ommInit();
    void ommShutdown();
    // why: 0 slot changed, 1 release all, 2 LRU, 3 tables/data unavailable, 4 frame abort, 5 debug discard
    void ommRetire(uint32_t id, uint32_t why = 0);
    uint32_t mOmmWhy[6] = {}, mOmmRecorded = 0, mOmmPlanned = 0, mOmmTablesOff = 0;
    double mOmmNextLog = 0.0;
    void ommSlotChanged(uint32_t sl);
    void ommReleaseAll();
    // Plan (CPU, before the scratch plan): per-instance table + builds within budget.
    // Returns the build scratch the plan needs (aligned to salign).
    VkDeviceSize ommPlanFrame(const float lod_cam[3], float radius, VkDeviceSize salign, uint64_t budget);
    // Record: table uploads (before pass A) ...
    void ommRecordUploads(VkCommandBuffer cmd);
    // ... and bake -> micromap build -> OMM BLAS build (after the static BLAS builds).
    void ommRecordBuilds(VkCommandBuffer cmd, const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch);

    std::vector<Chunk> mChunks;
    std::vector<SlotBlas> mSlot;                  // index = slot id
    // STATIC BUILD WANT LIST (crowd perf): slots stamped used (feedback / terrain priority)
    // while they had no BLAS -- the only ones the lazy static build can act on. The
    // build walks this list instead of every slot of the pack each frame; an entry leaves
    // when built, once its stamp is older than the build window, or past the table.
    std::vector<uint32_t> mWant;
    std::vector<uint8_t>  mInWant;                // per slot: listed in mWant
    void wantSlot(uint32_t sl);
    std::vector<uint32_t> mSlotGen;               // content generation per slot
    std::vector<uint32_t> mSlotAddrCpu;           // 2 words per slot (lo, hi)
    std::vector<uint32_t> mIdxCheckGen;           // per slot: mSlotGen + 1 when scanned
    std::vector<uint8_t>  mIdxBad;                // per slot: scan found an index >= vert_count
    uint32_t mIdxBadLogged = 0;
    VkDeviceAddress mRigPosAddrSeen = 0;          // rig_pos reallocated -> rebuild (not refit) every rig BLAS
    uint32_t mAddrDirtyLo = ~0u, mAddrDirtyHi = 0;   // dirty slot range [lo, hi)
    std::unordered_map<uint64_t, RigBlas> mRig;   // key = (owner << 8) | mask class
    // This frame's rigged geometries / ranges, index = rig entry (a group's are contiguous).
    std::vector<VkAccelerationStructureGeometryKHR>        mRigGeoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRigRanges;
    std::vector<RigEntryCPU> mRigEntries;
    // #747 / #748 rigged cluster RT: renderer instance ids the cluster raster draws this frame
    // (bitset; the rig plan keys their faces into their own groups) and, per rig entry, its
    // group's head + 1 when the group is such a cluster-switched group (0 = legacy group).
    std::vector<uint32_t> mClSwitched;
    std::vector<uint32_t> mRigClHead;
    // Legacy-skin skip: per frame slot, the cluster-switched groups pass B saw {head entry,
    // owner avatar} and the rig entry count of that frame (mode 4's flag words follow the heads
    // in the cluster RT's group map); valid once mode 4 was recorded. Harvested when the slot
    // comes round (its fence was waited) into per-avatar streaks of fully covered frames.
    struct ClCovGroup { uint32_t head, av; };
    std::vector<ClCovGroup> mClCov[3];
    uint32_t mClCovEntries[3] = {};
    bool     mClCovValid[3] = {};
    uint32_t mClCovStreak[kAvatarSlotCap] = {};
    uint32_t mClCovFrame[kAvatarSlotCap] = {};
    uint32_t mClCovIn = 0, mClCovFell = 0;        // last harvest: avatars all-cluster / some group fell back
    uint32_t mClOnlyGroups = 0;                   // this frame: switched groups built without a legacy BLAS
    // CLUSTER RT -> TLAS CONSISTENCY (2026-10-14, like with like): per frame slot, the avatars the
    // cluster path draws (switched rigged instances, the cluster side's own avatar slots) split by
    // the legacy RT rules (rtAvTraced: seen by a CPU-decided pass or an RT capture candidate, not
    // a no-shadow reduced-tier wearer) into EXPECTED (must be traced from their cluster cut) and
    // excluded by rule (counted per reason). The harvest compares the expected set with what pass
    // B covered: an expected avatar the plan has no cluster-only group for, or whose group fell
    // back (no cluster records: nothing traced), is a HOLE. Holes lasting kClConsistFrames
    // harvests are logged (WARN, re-logged every kClConsistRelog harvests; INFO once gone).
    struct ClExpect
    {
        uint32_t exp[kAvatarSlotWords] = {};      // expected avatars (bitset, joint-table slots)
        uint32_t cl_av = 0;                       // avatars the cluster path draws
        uint32_t no_shadow = 0, unseen = 0;       // excluded by rule
        uint32_t am_cl_av = 0;                    // of cl_av: animesh (control avatars)
        uint32_t plan_off = 0;                    // expected but the rigged plan did not run (no rig buffers)
        uint32_t np_skin = 0, np_row = 0, np_other = 0;   // expected, no cluster-only group: why
        uint32_t not_plan = 0;                    // = np_skin + np_row + np_other
        uint32_t extra = 0;                       // cluster-only groups of avatars not expected
    };
    ClExpect mClExpect[3];
    ClExpect mClExpLast;                          // the last harvested slot's
    uint32_t mClExpN = 0, mClHoleFell = 0, mClHoles = 0;   // last harvest
    static constexpr uint32_t kClConsistFrames = 300;
    static constexpr uint32_t kClConsistRelog = 3600;
    uint32_t mClConsistStreak = 0;
    uint32_t mClConsistReady = 0;                 // the module's avatars with RT data (informational)
    bool     mClConsistWarned = false;
    // Per avatar: why the current rigged plan rejected a cluster-switched row of it (bit 0: skin
    // past the live palettes, bit 1: row fields -- dead / HUD / no LOD group); cached with the plan.
    uint8_t  mRigPlanAvWhy[kAvatarSlotCap] = {};
    // #692 / undrawn diagnosis: the current plan's legacy (non-cluster) rigged faces, those of them
    // the static cluster-routable rule accepts, and the OR of their kRowNever flag bits.
    uint32_t mRigPlanLegFaces = 0, mRigPlanLegRoutable = 0, mRigPlanLegNever = 0;
    void checkClusterConsistency();
    void harvestClusterCoverage();
    // #692 RIGGED GROUPS OUT OF THE TLAS: a group (owner avatar + mask class) the plan
    // wants in RT that has no BLAS this frame -- why and since when. Used to (1) build a
    // group the scratch budget kept deferring over that budget after kRigStarveFrames
    // (a big avatar group behind the statics / nearer refits used to lose its BLAS at a
    // periodic rebuild and never get it back), (2) back a failed BLAS allocation off
    // (#697: retry some frames later instead of every frame) and (3) log a sustained
    // exclusion with its reason, and the recovery.
    enum RigOut : uint8_t { RIG_OUT_NONE = 0, RIG_OUT_SCRATCH, RIG_OUT_ALLOC, RIG_OUT_SLOT };
    struct RigWait
    {
        uint32_t since = 0;        // mFrame the current stretch out of the TLAS began
        uint32_t lastFrame = 0;    // mFrame of the last frame out
        uint32_t frames = 0;       // consecutive frames out
        uint32_t allocFails = 0;   // consecutive failed BLAS allocations (backoff)
        uint32_t retryAt = 0;      // allocation backoff: no new attempt before this mFrame
        uint32_t faces = 0, tris = 0;           // left out
        uint32_t all_faces = 0, all_tris = 0;   // RIG_OUT_SLOT: the owner's total
        RigOut   reason = RIG_OUT_NONE;
        bool     logged = false;   // the sustained-exclusion line was written
    };
    std::unordered_map<uint64_t, RigWait> mRigWait;   // key = mRig's; (owner << 8) for RIG_OUT_SLOT
    // Plan-time exclusions (RIG_OUT_SLOT): per owner, the rigged faces the plan left out
    // because their slot row failed slotOk (index check / CPU mirror), with the owner's
    // totals. Rebuilt with the plan, noted every frame (the plan may be reused).
    struct RigPlanSkip { uint64_t owner; uint32_t faces, tris, all_faces, all_tris; };
    std::vector<RigPlanSkip> mRigPlanSkips;
    void rigNoteOut(uint64_t key, RigOut why, uint32_t faces, uint32_t tris, bool alloc_failed,
                    uint32_t all_faces = 0, uint32_t all_tris = 0);
    void rigNoteIn(uint64_t key);
    std::string rigOwnerText(uint64_t key) const;
    uint32_t mRigUndrawn = 0;                         // readFeedback: rig entries the cull drew nowhere
    void noteRigInvalid();                            // readFeedback: the GPU check's log line
    bool rigLogOk();                                  // global rate limit of the #692 lines
    double   mRigLogWindow = 0.0;
    uint32_t mRigLogLines = 0;
    // GPU rig copy check (clean_rt_instances.comp rigCopy, #692): entries whose skinned
    // positions did not match the main cull's draw this frame (collapsed to a point: no RT
    // shadow) and the largest such instance id + 1 -- read back with the active counter.
    uint32_t mRigInvalid = 0, mRigInvalidId = 0;
    // #973 rig copy input check (readFeedback): this readback's collapsed entries, totals.
    uint32_t mRigBadFar = 0, mRigBadNonFinite = 0, mRigBadTotal = 0, mRigBadFrames = 0, mRigBadRebuilds = 0;
    double   mRigBadLogNext = 0.0;
    uint32_t mRigInvalidFrames = 0;                   // consecutive readbacks with mRigInvalid > 0
    double   mRigInvalidLogNext = 0.0;
    // C3: the rigged plan (candidates sorted into owner/mask groups, rig entries,
    // geometries + ranges) is reused while its inputs are unchanged: rigged-row plan
    // fields (mRigRowGen), slot contents (mSlotGenTotal), LOD groups, repacks, skin meta,
    // av_vis tier/visibility bits, counts, and (geometries only) the rig_pos / IBO
    // addresses. RenderVKRTRigPlanCache 0 = rebuild every frame.
    // dist: the owner avatar's distance from the camera at plan time (build order: near
    // groups first, so the per-frame scratch budget never starves a close-up avatar).
    struct RigPlanCand { uint64_t key; uint32_t id, slot, opaque; float dist; };
    std::vector<RigPlanCand> mRigCands;
    std::vector<uint64_t> mRigRowSig;             // per instance id: rigged plan fields (0 = not rigged)
    uint32_t mRigRowGen = 0;                      // bumped when any rigged row's plan fields change
    uint32_t mSlotGenTotal = 0;                   // bumped by slotChanged of a slot the plan read
    std::vector<uint8_t>  mRigPlanSlot;           // per slot: read by the current plan
    std::vector<uint32_t> mRigPlanSlotList;       // the marked slots (unmarked on rebuild)
    uint64_t mRigPlanKey = 0;
    bool     mRigPlanValid = false;
    uint32_t mRigPlanVerts = 0, mRigPlanNoShadow = 0;
    bool     mRigGeomValid = false;
    VkDeviceAddress mRigGeomPosAddr = 0, mRigGeomIboAddr = 0;
    // Rigged-instance set (skin != sentinel), maintained incrementally from the
    // renderer's dirty instance runs (+ a rolling self-heal slice) so the per-frame rig
    // plan visits only rigged rows instead of every instance (was ~half of vk_rtBuild).
    std::vector<uint8_t>  mIsRigInst;             // per instance id
    std::vector<uint32_t> mRigIds;                // ascending ids with mIsRigInst set
    std::vector<std::pair<uint32_t, uint32_t>> mPendInstRuns;
    bool     mRigSetFull = true;                  // rescan every row on next refresh
    bool     mRigIdsDirty = true;
    uint32_t mRigHealCursor = 0;
    void refreshRigSet(uint32_t inst_n);
    // Terrain instance set (flag 32), maintained alongside the rigged one (refreshRigSet).
    std::vector<uint8_t>  mIsTerrInst;            // per instance id
    std::vector<uint32_t> mTerrIds;               // ascending ids with mIsTerrInst set
    bool     mTerrIdsDirty = true;
    // SMOOTH TERRAIN (#211): morph BLAS per slot, this frame's copies / builds, and the
    // drawn terrain slots still missing a static BLAS (built ahead of the lazy scan).
    std::unordered_map<uint32_t, TerrMorph> mTerrMorph;   // slot -> morph BLAS
    std::vector<uint8_t>  mTerrOwned;             // per slot: has a mTerrMorph entry
    std::vector<TerrEntryCPU> mTerrEntries;       // this frame's morph copies (mode 3), = mTerrBuilds order
    std::vector<TerrBuild> mTerrBuilds;
    std::vector<VkAccelerationStructureGeometryKHR>        mTerrGeoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> mTerrRanges;
    std::vector<uint32_t> mTerrPriority;
    // Per slot: max |geomorph step| (texcoord0.y) per morph band class (clean_terrain_lod.glsl
    // terrainMorph: 0.125 m, 0.25 m, 0.5 m vertices), scanned once per content generation.
    std::vector<float>    mSlotStep;              // 3 per slot
    std::vector<uint32_t> mSlotStepGen;           // per slot: mSlotGen + 1 when scanned
    uint64_t mTerrBytes = 0;                      // morph BLAS (also counted in mStaticBytes)
    uint32_t mTerrBuiltN = 0, mTerrRefitN = 0;    // this frame (counters)
    // Retire slot `sl`'s morph BLAS; the slot address falls back to its static BLAS (0 = none).
    void dropTerrMorph(uint32_t sl);
    uint32_t mStaticCount = 0;
    uint64_t mStaticBytes = 0, mRigBytes = 0;
    uint32_t mFrame = 0;
    uint32_t mSlotIdx = 0;                        // frame slot (mFrameSlot % 3)

    Retire mRetire[3], mPend;
    std::vector<CompactPending> mCompact[3];

    // ---- #973 ARENA GUARD (RenderVKRTArenaGuard KB, 0 = off; debug A/B) ----
    // BLAS are packed back to back in 64 MB arena chunks: a build / refit / compaction copy that
    // writes past its AS overwrites the next BLAS -- on AMD (in-shader traversal) a corrupt BVH
    // hangs (TDR) or faults far away. With the guard on, every arena allocation made while it is
    // on (new chunks only: they need TRANSFER usage) gets a band of that size after its AS,
    // filled with kArenaGuardPattern before the frame's builds; every kArenaGuardEvery frames
    // the first kArenaGuardProbe bytes of up to kArenaGuardMax bands (round robin) are copied to
    // a host buffer after the frame's builds and compared when that frame slot comes round. An
    // overwritten band names the AS it follows (the likely overrunner) and the AS after it. At
    // DEVICE_LOST the last copies still in host memory are compared too (device-local memory
    // cannot be read after a loss: a band overwritten after the last copy goes unseen).
    struct ArenaGuardRec { VkDeviceSize asSize = 0, guard = 0; uint32_t id = 0, serial = 0, frame = 0;
                           bool filled = false, reported = false; char kind[16] = {}; };
    struct ArenaGuardProbe { int chunk; VkDeviceSize asOff; uint32_t serial; };
    std::map<std::pair<int, VkDeviceSize>, ArenaGuardRec> mArenaGuards;   // (chunk, AS offset)
    std::vector<std::pair<std::pair<int, VkDeviceSize>, uint32_t>> mArenaGuardFills;   // key, serial
    std::vector<ArenaGuardProbe> mArenaGuardProbes[3];
    GpuBuffer mArenaGuardRb[3];
    uint32_t mArenaGuardToken[3] = {};            // the stamp the GPU writes after the copy (0 = none pending)
    std::pair<int, VkDeviceSize> mArenaGuardCursor{-1, 0};
    VkDeviceSize mArenaGuardBytes = 0;            // this frame's setting (bytes)
    uint32_t mArenaGuardSerial = 0, mArenaGuardChecks = 0, mArenaGuardBad = 0, mArenaGuardLastCheck = 0;
    void arenaGuardFlushFills(VkCommandBuffer cmd);
    void arenaGuardRecordCheck(VkCommandBuffer cmd);
    uint32_t arenaGuardCompare(uint32_t slot, bool lost);   // overwritten bands found
    void arenaGuardLostReport();                  // DEVICE_LOST (the RT audit hook)

    // GPU buffers
    GpuBuffer mScratch, mTlasInst, mRtGeom, mSlotAddr, mUsedBits, mRigEntry, mRigValid, mRigPos, mDummy;
    GpuBuffer mRtGeomVc;                          // #874: per instance id, the vertex count of its geometry (GPU-written;
                                                  // #1009: 4 words, + a cluster-traced static's legacy row)
    GpuBuffer mTerrEntry, mTerrPos;               // morph copy entries (CPU-staged) / positions (GPU-only)
    GpuBuffer mStage[3];                          // per frame slot: slot addresses + rig entries
    GpuBuffer mXform[3];                          // per frame slot: static BLAS decode transforms (#573)
    GpuBuffer mUsedRb[3];                         // used-slot bitset readback (+ active counter)
    uint32_t  mUsedRbWords[3] = {0, 0, 0};
    bool      mUsedRbValid[3] = {false, false, false};
    // ---- flicker diagnostic (RenderVKRTDebugFlicker; idle while 0) ----
    bool      mUsedRbDbg[3] = {false, false, false};  // readback carries the diagnostic tail
    uint32_t  mUsedRbRigs[3] = {0, 0, 0};             // rig entries of that frame
    GpuBuffer mDbgStats, mDbgStatsRb[3];              // view 15 stats (+ per-slot readback)
    bool      mDbgStatsValid[3] = {false, false, false};
    bool      mDbgHavePrev = false;
    uint32_t  mDbgPrevN = 0, mDbgPrevXor = 0, mDbgPrevSum = 0, mDbgPrevCls[8] = {};
    std::vector<std::pair<uint32_t, uint32_t>> mDbgPrevIds;   // (id, hash), sorted
    uint32_t  mDbgWinFrames = 0, mDbgWinChanged = 0;
    double    mDbgNextSummary = 0.0;
    bool      mDbgVisHavePrev = false;
    float     mDbgVisPrevBlocked = 0.f, mDbgVisMin = 1.f, mDbgVisMax = 0.f;
    uint32_t  mDbgVisFrames = 0, mDbgVisJumps = 0;
    VkQueryPool mQueryPool = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxBuildsPerFrame = 256;

    // TLAS
    VkAccelerationStructureKHR mTlas = VK_NULL_HANDLE;
    GpuBuffer mTlasBuf;
    uint32_t  mTlasCap = 0;
    VkDeviceSize mTlasScratch = 0;                // max(build, update) over both flag sets
    bool mTlasBuilt = false;
    // TLAS REFIT (RenderVKTLASRefit): MODE_UPDATE in place while everything an update
    // must keep identical still matches the last BUILD (see recordBuild).
    bool      mTlasHasBuild = false;              // mTlas holds a completed (recorded) BUILD
    uint32_t  mTlasBuildFrame = 0;                // mFrame of the last BUILD
    uint32_t  mTlasBuildCount = 0;                // its primitiveCount (VUID-03769)
    VkBuildAccelerationStructureFlagsKHR mTlasBuildFlags = 0;   // its flags (VUID-03759)
    VkDeviceAddress mTlasBuildInstAddr = 0;       // record buffer it read (contents carry activity)
    uint32_t  mTlasBuildPackGen = 0;              // renderer repack generation at that build
    float     mTlasBuildRadius = -1.f;
    float     mTlasBuildCam[3] = {0.f, 0.f, 0.f};
    uint64_t  mTlasBuildRigKey = 0;               // rigged plan it was built with (group record ids)
    uint32_t  mTlasDeferred = 0;                  // readback: instances waiting for a build
    bool      mTlasRefitThis = false;             // this frame's TLAS was an update (counters)
    // Placeholder BLAS for records that were active at the last build and are not wanted
    // now (mask 0, never hit): one degenerate triangle, built once, kept until releaseAll.
    VkAccelerationStructureKHR mDummyBlas = VK_NULL_HANDLE;
    Alloc     mDummyMem;
    VkDeviceAddress mDummyAddr = 0;
    GpuBuffer mDummyVerts;
    uint32_t  mDummyBuiltFrame = 0;               // 0 = not built; usable from the next frame
    uint32_t  mRigRefitSkipped = 0, mRigCopyEntries = 0;   // this frame (counters)
    uint32_t  mRigRefitSkippedMoving = 0;   // #632: of those, held poses whose root moved (avatar-local BLAS)
    // classifyNoSelf: per avatar slot, 1 = no-self-shadow (throttle allowed), the gFrameCount of
    // its last change (minimum dwell), and this frame's count.
    uint8_t   mNoSelf[kAvatarSlotCap] = {};
    uint32_t  mNoSelfChanged[kAvatarSlotCap] = {};
    uint32_t  mNoSelfCount = 0;
    static constexpr uint32_t kCompBindings = 29;   // clean_rt_instances.comp storage buffers (22: cluster RT override, #748; 23: cluster-switched rigged groups; 24: per-geometry vertex counts, #874; 25 / 26: #1007 pose snapshot / pose motion readback; 27 / 28: #1032 materials / alpha-less texture slots)
    // #1032 KNOWN-OPAQUE ALPHA-MASK (clean_rt_instances.comp maskKnownOpaque): one bit per
    // bindless texture slot whose image has no alpha (SharedTex::comps != 4), rebuilt from the
    // renderer's shared-texture registry when its texture generation moved (and every
    // kTexOpaqueRefresh frames as a safety net), copied into this frame slot's host buffer.
    std::vector<uint32_t> mTexOpaqueCpu;
    GpuBuffer mTexOpaque[3];
    uint32_t  mTexOpaqueGen = ~0u, mTexOpaqueBuilt = 0, mTexOpaqueCount = 0;
    bool      mTexOpaqueOn = false;   // this frame binds mTexOpaque[slot] (else the zeroed dummy)
    void texOpaqueUpdate();
    // #1007 rigged refit cadence + pose skip (llcleanrtvk.cpp "POSE SKIP"), this frame's
    // counters: skipped by the distance / screen-size cadence, skipped by the pose test, and
    // the largest staleness (frames since its last copy + refit) a skipped group carries.
    uint32_t  mRigSkipCadence = 0, mRigSkipPose = 0, mRigMaxStale = 0;
    // POSE MOTION READBACK (clean_rt_instances.comp mode 5): mPoseSnap = the joint table at the
    // last mode-5 dispatch (GPU-only); mPoseRb[slot] = per joint-table slot {max translation,
    // max rotation} change of the pelvis-relative joints (float bits), written by the frame
    // that used this frame slot. Valid = mode 5 was recorded there over mPoseRbSlots slots
    // against a snapshot that was not (re)created that frame; mPoseRbFrame = its mFrame.
    GpuBuffer mPoseSnap, mPoseRb[3];
    bool      mPoseRbValid[3] = {false, false, false};
    uint32_t  mPoseRbFrame[3] = {0, 0, 0};
    uint32_t  mPoseRbSlots[3] = {0, 0, 0};
    bool      mPoseSnapReady = false;             // mPoseSnap (this buffer) was written by a mode 5
    // Per joint-table slot: the last kPoseHist readings by the mFrame they measured
    // (a reading covers the change since the previous mode-5 dispatch: the frame before it
    // when RT ran on consecutive frames). Displacement estimate in metres (translation +
    // rotation x kPoseLever); frame 0 = none.
    static constexpr uint32_t kPoseHist = 16;
    struct PoseHist { uint32_t frame[kPoseHist] = {}; float d[kPoseHist] = {}; };
    std::vector<PoseHist> mPoseHist;
    void harvestPoseMotion();   // mPoseRb[this slot] -> mPoseHist (this slot's fence was waited)
    // IMMEDIATE REFIT triggers of the cadence / pose skip: the camera of the last build
    // (a teleport moves it far in one frame), a region crossing (noteOriginShift), and per
    // avatar slot last frame's distance (a sit / in-sim teleport jumps the bounds).
    float     mRigCadCam[3] = {0.f, 0.f, 0.f};
    bool      mRigCadCamValid = false;
    bool      mRigOriginShifted = false;
    float     mRigPrevDist[kAvatarSlotCap] = {};
    uint32_t  mLastBuildGFrame = 0;               // gFrameCount of the last recordBuild (refit gate)
    bool mDeferRelease = false;                   // setDeferRelease (this frame's build)
    VkPipelineStageFlags2 mReleaseOwed = 0;       // consumer stages still owed the release
    bool mIdleSinceBuild = false;                // idleFrame ran since the last full build
    // RELEASE WHEN UNUSED: after RT has had no consumer for a while (projector shadows
    // switched to maps / off, debug view closed) every BLAS, the TLAS and the RT buffers
    // are released through the retire ring -- they used to stay resident for the whole
    // session (~3 GB on an 8 GB card, starving texture residency). Rebuilt lazily on the
    // next active frame, like a fresh start.
    double mIdleSince = 0.0;                      // wall time the current idle stretch began (0 = active)
    uint64_t mDeviceBudget = 0;                   // device-local heap budget (renderer measurement)
    uint64_t mDeviceUsage = 0;                    // the process's device-local usage (renderer measurement)
    bool     mLadderCalm = false;                 // the VRAM ladder has no rung engaged (headroom may grow)
    uint64_t mHeadroomBudget = 0;                 // the headroom rule's budget (hysteresis; 0 = not in use)
    void     updateHeadroomBudget();              // recordBuild, once per frame (before budgetBytes)
    uint64_t autoBudgetBytes() const;             // the fixed share or the headroom rule (no setting / cap)
    uint64_t mBudgetLogged = 0;                   // the effective budget last logged (recordBuild)
    uint64_t mPressureCap = 0;                    // yieldForPressure cap (0 = none)
    bool   mRigDropped = false;                   // rigged BLAS dropped (VRAM pressure ladder only)
    uint64_t mRigDropBytes = 0;
    // Rigged rows (mRigIds) when the drop happened: mRigDropBytes is what THAT scene's avatars
    // held. Leaving a crowded region (PEAK: ~24k rigged rows, 2 GB of legacy avatar BLAS) for a
    // quiet one (~8k rows) must not keep judging the room by the crowd's bytes -- it did, and
    // avatars stayed out of RT for the rest of the session. The lift test scales the held
    // bytes by rows-now / rows-at-drop (never up).
    uint32_t mRigDropRows = 0;
    uint32_t mRigRoomFrames = 0;
    bool   mRigPressureDrop = false;              // dropRiggedForPressure (until clearPressure)
    bool   mReleaseAsap = false;                  // releaseSoon: next idleFrame releases
    bool   mWarm = false;                         // sunReady(): static builds caught up
    uint32_t mWarmFrames = 0, mWarmSettled = 0;   // build frames since cold / consecutive unsaturated
    void   markCold(const char* why);
    bool   mOverBudget = false;                   // RT working set alone exceeds the budget
    uint64_t rtAuxBytes() const;                  // TLAS + scratch + tables (allocated)
    bool   mReleased = false;                     // released; arenaFree also drops the last chunk
    void releaseAll();
    // #161: build scratch spent this frame (static + rigged builds stay under a budget;
    // extra builds wait a frame) and a hysteresis shrink for grow-only aux buffers.
    VkDeviceSize mScratchUsed = 0;
    uint32_t mRigPosOverFrames = 0, mTablesOverFrames = 0;
    // Scratch: the largest need of the current / previous 300-frame window (the buffer shrinks
    // to that once it is over 2x it: a per-frame need swings with the builds of that frame).
    VkDeviceSize mScratchWinMax = 0, mScratchWinPrev = 0;
    uint32_t mScratchWinFrames = 0;
    uint32_t mRigStarvedBuilt = 0;                // this frame: starved rigged builds let past the scratch budget
    uint32_t mRigRefitDeferred = 0;               // this frame: rigged refits deferred by the refit scratch budget
    // Retire `b` when it has stayed > 4x `need` (and > 16 MB) for ~300 frames, so the next
    // ensureBuf reallocates it at the need.
    void shrinkIfOversized(GpuBuffer& b, VkDeviceSize need, uint32_t& over_frames);

    // pipelines
    VkDescriptorSetLayout mDslComp = VK_NULL_HANDLE, mDslRT = VK_NULL_HANDLE, mDslEmpty = VK_NULL_HANDLE;
    VkPipelineLayout mCompLayout = VK_NULL_HANDLE, mDebugLayout = VK_NULL_HANDLE;
    VkPipeline mCompPipe = VK_NULL_HANDLE, mDebugPipe = VK_NULL_HANDLE;
    VkDescPool mPool;   // layout-sized + accounted (vk_descpool.h)
    VkDescriptorSet mSetComp[3] = {}, mSetRT[3] = {};
    std::vector<uint64_t> mSetKeyComp[3], mSetKeyRT[3];   // handles last written per set
    bool mSetRTDepth[3] = {false, false, false};          // RT set binding 7 (raster depth) written

    // ---- RT SCENE AUDIT (#874, llcleanrtaudit.cpp) ----
    // Per frame slot: what the readback holds (written when the copy is recorded; the GPU
    // writes `token` into word 0 of the buffer once the copies ran).
    struct AuditMeta
    {
        bool     valid = false;
        uint32_t token = 0;                       // = rtFrame (never 0)
        uint32_t regFrame = 0;                    // registry frame at record time (vkAddrCurrentFrame)
        uint32_t rtFrame = 0;                     // mFrame
        uint32_t gFrame = 0;                      // viewer frame (gFrameCount)
        uint32_t instN = 0;                       // records copied (= pass A's instance_count)
        uint32_t tlasCap = 0;
        uint32_t rigN = 0;                        // rig entries of the frame
        VkDeviceSize rigOff = 0;                  // their copy in mStage[slot] (rigStaged)
        bool     rigStaged = false;
        uint32_t slotN = 0, idxValid = 0, rigPosVerts = 0, vboVerts = 0;
        bool     tlasUpdate = false, ptlas = false;
        VkDeviceAddress dummy = 0, tlasInstAddr = 0, rtGeomAddr = 0;
    };
    static constexpr VkDeviceSize kAuditHdr = 64;   // readback: stamp word, then records, then rt_geom
    GpuBuffer mAuditRb[3];
    AuditMeta mAuditMeta[3];
    bool      mAuditNowPending = false;           // RenderVKRTAuditNow: audit the slot when it comes round
    uint64_t  mAuditThread = 0;                   // hash of the thread that records (CPU tables: that thread only)
    bool      mAuditCpuDeferred = false;          // a loss raised on another thread: CPU tables from the main loop
    void auditRecordCopy(VkCommandBuffer cmd, uint32_t inst_n, uint32_t slot_n, bool tlas_update, bool ptlas,
                         VkDeviceSize rig_off, bool rig_staged);
    void auditBeforeFrame();                      // recordBuild, before beginFrame: the on-demand check
    // lost: on DEVICE_LOST (vkFaultLog output) / on demand (the log). only_slot < 0: every slot.
    // cpu_tables: the CPU-side tables + instance-store lookups (the recording thread only).
    void runAudit(const VkDeviceAddress* faults, uint32_t nfaults, bool lost, int only_slot, bool cpu_tables,
                  bool gpu_part = true);
    void auditInvalidate();                       // releaseAll / shutdown: the ring is gone

    // ---- RT READ GUARD (#874 / #889, llcleanrtaudit.cpp; clean_rt_common.glsl RTG_*) ----
    // Per frame slot: a 256 B host-visible buffer bound at RT set binding 15. The RT shadow mask
    // records the first out-of-range read of its frame (+ per-site counts) there; the slot's
    // contents are harvested when the slot comes round again (its fence was waited) and stay
    // readable after a DEVICE_LOST (host memory: the report prints them).
    static constexpr uint32_t kGuardWords = 64;
    static constexpr uint32_t kGuardSites = 16;   // = RTG_SITES
    static constexpr uint32_t kGuardRec = 6, kGuardCounts = 20;   // = RTG_REC / RTG_COUNTS
    static constexpr size_t   kGuardSeenCap = 256;   // distinct (site, id) keys logged per session
    GpuBuffer mGuard[3];
    struct GuardMeta { bool valid = false; uint32_t rtFrame = 0, regFrame = 0, gFrame = 0, texCount = 0; };
    GuardMeta mGuardMeta[3];
    std::vector<uint64_t> mGuardSeen;             // logged (site << 32 | id), sorted, <= kGuardSeenCap
    uint64_t  mGuardTotal = 0;                    // violations since start (harvested)
    uint64_t  mGuardSiteTotal[kGuardSites] = {};
    uint64_t  mGuardWinTotal = 0;                 // since the last summary line
    double    mGuardNextSummary = 0.0;
    uint32_t  mGuardFramesHit = 0;                // harvested frames with a violation
    void guardPrepare(uint32_t s);                // updateSets: harvest slot s, then re-arm it
    void guardHarvest(uint32_t s);
    std::string guardDescribe(const uint32_t* w, bool cpu_tables) const;   // the first-offender record
    void guardReport(bool lost, bool cpu_tables); // DEVICE_LOST / on-demand: every slot's guard buffer
    void guardFaultReach(const VkDeviceAddress* faults, uint32_t n, bool lost);   // which RT-set buffer an index could reach a fault from
    void auditRegister(bool on);                  // init / shutdown: the process-wide hooks

    // TODO(2b) CLUSTER LOD RAY TRACING (#748, llcleanclusterrtvk.h): the RT cut of the cluster
    // path, its CLAS / per-cut BLAS, the pass A override (binding 22) and, when the session's
    // TLAS is partitioned, the PTLAS build. Null when its init failed (legacy RT only).
    std::unique_ptr<LLCleanClusterRTVK> mClusterRT;

    // stats (this frame)
    uint32_t mBuiltThisFrame = 0, mRepackRebuilt = 0, mRefits = 0, mRigBuilds = 0, mCompactions = 0,
             mStarved = 0, mTlasActive = 0;
    uint32_t mRepackInvalidPending = 0;
};

#endif
