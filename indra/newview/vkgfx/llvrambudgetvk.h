/**
 * @file llvrambudgetvk.h
 * @brief One VRAM budget for the Vulkan renderer (#405 part A, #407).
 *
 * LLVramStatus: the VRAM picture the renderer publishes (log, Preferences readout,
 * bug reports, other subsystems such as the RAM texture tiers): the budget, the
 * process usage, our tracked allocations per category (#407), suballocator slack and
 * the driver / external overhead nobody tracks.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#pragma once

#include "vk_device.h"   // VkMemCat / VKMEM_COUNT

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

struct LLVramStatus
{
    bool     valid = false;          // the renderer filled it (false: no VK world yet)
    // ---- budget (#405) ----
    uint64_t osBudget = 0;           // VK_EXT_memory_budget per-process budget (main heap; else heap size)
    uint64_t userCap = 0;            // "Maximum video memory" (0 = Auto)
    uint64_t capacity = 0;           // min(user cap, OS budget[, debug fake budget])
    uint64_t reserve = 0;            // 10% of capacity (entering it = fast ladder)
    uint64_t budget = 0;             // capacity - reserve
    uint64_t usage = 0;              // process usage on the main heap (driver), else ours
    bool     uma = false;            // #695: budget/usage = carve-out + shared host heap (integrated GPU)
    // ---- sharing the GPU (#403; LLVramShareVK) ----
    uint64_t shareCap = 0;           // min(smoothed OS budget, peer share) (0 = not measured)
    uint64_t osSmoothed = 0;         // the OS budget, down at once / up over ~30 s
    uint64_t peerCap = 0;            // this viewer's share next to other Slipstream viewers (0 = alone)
    uint32_t peers = 0;              // other live Slipstream viewers on this GPU
    uint64_t peerClaims = 0;         // what they use / claim
    // ---- accounting (#407) ----
    uint64_t tracked = 0;            // our live allocations on the main heap
    uint64_t trackedPeak = 0;        // high-water of our device-local allocations
    uint64_t fixed = 0;              // what no ladder rung can shrink: usage - scalable categories
                                     // (+ their page padding); overhead + swapchain included
    uint64_t fixedSmoothed = 0;      // the budget manager's smoothed fixed term (0 = not yet)
    uint64_t scalable = 0;           // budget - fixedSmoothed: room for the scalable consumers
    uint64_t slack = 0;              // suballocator capacity not holding live data (both heaps)
    uint64_t slackDev = 0;           //   of which on device-local heaps (VRAM)
    uint64_t slackRing = 0, slackGeom = 0, slackTables = 0, slackInst = 0;   // by contributor
    uint64_t geomGarbage = 0;        // dead vertices / indices in the packed geometry (awaiting repack)
    uint64_t pendingFree = 0;        // retired images / buffers waiting on frames in flight
    uint64_t swapchain = 0;          // swapchain images (driver-owned; measured)
    uint64_t overhead = 0;           // usage - tracked - swapchain (driver / external, >= 0)
    // ---- what the overhead consists of (#407) ----
    uint64_t pad = 0;                // driver page padding of our device-local allocations
    uint64_t granularity = 0;        // the driver's allocation granularity
    bool     granularityMeasured = false;
    uint32_t allocCount = 0;         // live tracked allocations
    uint32_t allocLimit = 0;         // maxMemoryAllocationCount (4096 on some drivers)
    bool     drvReport = false;      // VK_EXT_device_memory_report active
    uint64_t drv[VKDRV_COUNT] = {};  // driver-reported live bytes on the main heap per bucket
    uint64_t drvImport = 0;          // imported (external) memory on the main heap
    uint64_t drvInternal = 0;        // driver-internal total (driverInternalBytes)
    uint64_t untrackedDevMem = 0;    // reported vkAllocateMemory bytes beyond our tracker (bypass)
    uint64_t unattributed = 0;       // overhead - pad - drvInternal - untrackedDevMem (>= 0)
    uint64_t overheadMin = 0;        // lowest overhead seen this session (the driver baseline)
    uint64_t inferred = 0;           // bytes whose category was inferred (no VkMemScope)
    uint64_t catDev[VKMEM_COUNT] = {};   // per category, device-local heaps
    uint64_t catPad[VKMEM_COUNT] = {};   // per category, driver page padding (device-local)
    uint64_t catHost[VKMEM_COUNT] = {};  // per category, system heap
    uint64_t catPeak[VKMEM_COUNT] = {};  // per category high-water (both heaps)
    // ---- texture pools (#411; VkDeviceContext::MemStats) ----
    // Pool blocks are tracked allocations of no category; texture / UI count their live
    // suballocations. texPoolSlack (block bytes - live bytes) is also in slack / slackDev,
    // and counts as scalable (it shrinks as textures leave and blocks empty).
    bool     texPools = false;
    uint32_t texPoolBlocks = 0, texPooledImages = 0, texPoolCount = 0;
    uint64_t texPoolBlockBytes = 0, texPoolRounded = 0, texPoolLive = 0, texPoolSlack = 0;
    uint64_t texSmallMax = 0, texMediumMax = 0;
    uint64_t texClassHit = 0, texClassMiss = 0, texMedAlignMax = 0;   // #587 (monotonic / max)
    VkDeviceContext::MemStats::TexPool texPool[VkDeviceContext::MemStats::kTexPoolMax] = {};
    // Hitch comparison (#411): texture + UI image vkAllocateMemory / vkFreeMemory calls and
    // pool suballocations / frees per second over the last log interval (~5 s).
    double   texAllocPerSec = 0.0, texFreePerSec = 0.0, texSubAllocPerSec = 0.0, texSubFreePerSec = 0.0;
    // #614 compaction: reclaimable bytes (what a perfect packing frees), the empty blocks VMA
    // keeps, the counters (monotonic: moves, bytes, busy skips, source blocks evacuated,
    // destinations refused, empty blocks released early), the moved rate over the last log
    // interval, and the mode (0 off, 1 background, 2 pressure pre-step).
    uint64_t texPoolReclaim = 0, texPoolEmptyBytes = 0;
    uint32_t texPoolEmptyBlocks = 0;
    uint64_t texCompactMoves = 0, texCompactMoved = 0, texCompactSkipped = 0, texCompactEvacuated = 0;
    uint64_t texCompactPlaceFail = 0, texCompactReleased = 0, texCompactReleasedBytes = 0;
    double   texCompactMBPerSec = 0.0;
    uint32_t texCompactMode = 0;
    // ---- soft ladder (#405) ----
    uint32_t rung = 0;               // rungs engaged (0 = nothing held back)
    uint32_t rungCount = 0;
    uint32_t step = 0;               // 1..13 = the deepest engaged ladder step (0 = none)
    uint32_t mode = 0;               // 0 calm, 1 soft (normal pacing), 2 in the reserve (fast)
    uint32_t hardSteps = 0;          // last-ditch setting writes this session
    std::string holding;             // what the ladder is holding back ("" = nothing)
    std::string fitNotes;            // settings applied below their value because they don't fit (#226)

    // Short lines for bug reports (each well under 200 chars): budget / usage /
    // overhead / ladder, and the per-category device-local MB. compactText = both +
    // what the ladder holds back + fit caps (the log).
    std::string summaryText() const;
    std::string categoryText() const;
    std::string compactText() const;
    // What the overhead is made of (#407, bug reports / log): page padding, driver-reported
    // internal memory by bucket, untracked vkAllocateMemory, the unattributed rest, slack.
    std::string overheadText() const;
    // Texture pools (#411), one short line: blocks / MB, images, live MB + occupancy, slack
    // (free space + class rounding), driver allocation / free rate. "" = pools off.
    std::string texPoolText() const;
    // Per pool: class (KiB, "med" = TLSF) blocks / MB, occupancy (live / block bytes).
    std::string texPoolClassText() const;
    // A few short lines for the Preferences readout (plain language).
    std::string readoutText() const;
};

// ---- THE BUDGET MANAGER + SOFT LADDER (#405 part A) ----
// budget = min(user cap "Maximum video memory", OS per-process budget) - reserve, the
// reserve being 10% of that capacity. One state machine walks a flat list of RUNGS
// (grouped into the owner's twelve steps, lightest visual loss first). The effective caps
// the renderer applies are a pure function of how many rungs are engaged; user settings
// are never written by the soft ladder (Preferences keep showing the user's choices).
//
// Pacing (decision, see the report): usage above 95% of the budget steps down one rung
// per ~2 s (normal); usage inside the reserve (above the budget) steps down FAST: up to 3
// rungs per 0.25 s check, no hysteresis wait. A new failed allocation (#563) steps down
// one rung per 0.5 s only while the failed request would still not fit (usage + its size +
// 3% under the capacity) -- one failure at 80% no longer runs the whole ladder.
// Recovery runs in reverse order, one applied rung at a time, after usage stayed under
// 80% of the budget -- and the rung's measured re-add fits under 85% -- for the calm
// period (10 s; pressure returning within 60 s of a restore doubles it, up to 160 s;
// five quiet minutes reset it). Rungs that would change nothing right now are skipped
// on the way down (engaged but not "applied") and popped for free on the way up.
//
// PRE-STEP (#614): before a rung is taken, the renderer's texture pool compaction frees
// pool slack (nothing visible degrades) while Input::holdLadder is set -- bounded by the
// renderer (a few seconds; never against a failed request that still does not fit).
//
// The hard step (setting writes) is NOT here: hardStepDue() only says when the ladder
// is exhausted and allocations keep failing / usage stays over capacity; the renderer
// performs it (write + latch + notify).
class LLVramBudgetVK
{
public:
    enum Step : uint8_t
    {
        STEP_NONE = 0,
        STEP_TEX_OFFSCREEN,   // 1 off-screen textures mip down (largest first)
        STEP_PROBES,          // 2 reflection probes: resolution, then count
        STEP_TEX_FAR,         // 3 far textures mip down
        STEP_GEOM_LOD,        // 4 mesh / terrain detail: effective LOD factors down (far first)
        STEP_FAR_SHADOWS,     // 5 far shadows off (cascade range + RT distance)
        STEP_FAR_DRAW,        // 6 far geometry: effective draw distance down, far objects unloaded
        STEP_RT,              // 7 smaller RT budget
        STEP_MIRRORS,         // 8 mirrors off (targets released)
        STEP_TEX_NEAR,        // 9 near textures mip down, then the global mip bias
        STEP_PROJ_SHADOWS,    // 10 projector shadows off
        STEP_SUN_SHADOWS,     // 11 sun shadows off
        STEP_RENDER_SCALE,    // 12 render scale (internal resolution), last soft step
        STEP_RIGGED,          // 13 (#613) far avatars: decimated meshes, creator LODs, smaller rigged buffers
        STEP_COUNT
    };
    struct Rung { uint8_t step; uint8_t sub; const char* what; };
    static uint32_t rungCount();
    static const Rung& rung(uint32_t i);
    static const char* stepName(uint32_t step);

    // RT sub-levels (Caps::rtLevel).
    // RT_PROBE_TRACE_OFF: ray-traced reflection probe capture (detail 3) falls back to the
    // raster faces -- the last TLAS consumer no other rung covers (frees the AS / aux once
    // the shadow rungs have stopped tracing too; costs GPU time, not VRAM).
    enum { RT_BLAS_CAP = 1, RT_RIGGED_OUT = 2, RT_MASK_3_4 = 3, RT_MASK_HALF = 4, RT_SHORT_RANGE = 5, RT_PROBE_TRACE_OFF = 6 };
    // RIGGED sub-levels (#613; Caps::rigEarly / rigLate, cumulative). They replace the rigged
    // controller's parallel memory path (LLCleanRendererVK::rigPressure): the ladder's order,
    // pacing, hysteresis and recovery decide when far avatars give memory back.
    //  RIG_DECIMATE: decimated steps for every unprotected avatar (screen-LOD tolerance at its
    //                top, LLCleanRigDecimator) -- error-bounded, ~invisible; no borrowing past
    //                the rigged share (RenderVKRiggedBudgetPct);
    //  RIG_CREATOR:  creator-LOD caps on far, unprotected tiers (never the nearest) as far as
    //                the rigged budget needs, budget 3/4 of the share;
    //  RIG_HALF:     rigged budget 1/2 of the share (drops stay the last resort, far tail).
    // Every rigged rung is in the table TWICE: the EARLY copy (decimation just before the
    // mesh-detail rungs, creator LODs and the 1/2 budget right after them, where the old
    // avatarLodScale squeeze of the rigged budget began) applies on cards with more than
    // kRigSmallCardBytes of capacity, the LATE copy (after the near-texture rungs) on smaller
    // ones, so an 8 GB card blurs mid textures before far bodies poke through clothes. The
    // renderer picks the copy (rigEffective) and reports the other one as not useful.
    enum { RIG_DECIMATE = 1, RIG_CREATOR = 2, RIG_HALF = 3 };
    static constexpr uint64_t kRigSmallCardBytes = 9ull << 30;   // capacity: 8 GB cards and below
    static uint32_t rigEffective(uint32_t early, uint32_t late, bool small_card)
    {
        return small_card ? late : std::max(early, late);
    }
    static float rigBudgetScale(uint32_t level) { return level >= RIG_HALF ? 0.5f : (level >= RIG_CREATOR ? 0.75f : 1.f); }
    // Effective caps (all "no cap" by default).
    struct Caps
    {
        uint32_t texStepMin = 0;        // floor on the renderer's texture step (#282 levels, then bias)
        uint32_t probeResShift = 0;     // probe resolution >> shift (floor 64 px)
        uint32_t probeCountShift = 0;   // probe count >> shift (floor 32)
        bool     farShadowsOff = false; // sun cascades end nearer; RT shadow / TLAS range limited
        uint32_t rtLevel = 0;           // RT_* above (cumulative)
        bool     mirrorsOff = false;
        bool     projShadowsOff = false;
        bool     sunShadowsOff = false;
        float    renderScaleMax = 1.f;  // internal render scale cap (FSR / bilinear upscale)
        // Mesh / terrain detail (step 4): multipliers on the EFFECTIVE LOD factors
        // (RenderVolumeLODFactor -> LLVOVolume::sLODFactor, RenderAvatarLODFactor,
        // RenderTerrainLODFactor). A lower factor drops far objects' LOD first.
        float    meshLodScale = 1.f;
        float    avatarLodScale = 1.f;
        float    terrainLodScale = 1.f;
        // Far geometry (step 6): multiplier on the EFFECTIVE draw distance (RenderFarClip
        // -> gAgentCamera.mDrawDistance), floored at kFarDrawFloorM; cacheable objects
        // beyond it are unloaded (back to the region object cache) with their geometry
        // and textures, and stream back in as the distance is restored.
        float    drawDistScale = 1.f;
        // Rigged (#613): RIG_* level of the early / late copies of the rigged rungs
        // (rigEffective picks by card size).
        uint32_t rigEarly = 0;
        uint32_t rigLate = 0;
    };
    static constexpr float kFarDrawFloorM = 64.f;
    static constexpr float kFarDrawCeilM = 512.f;
    // The effective draw distance for a user setting under drawDistScale (never raised).
    static float effectiveDrawDistance(float setting, float scale)
    {
        if (scale >= 0.999f || setting <= kFarDrawFloorM) return setting;
        // A fraction of the setting AND an absolute ceiling (512 m x scale: 384 / 256 /
        // 179 m), so a 1-4 km draw distance actually sheds geometry.
        const float d = std::min(setting * scale, kFarDrawCeilM * scale);
        return d < kFarDrawFloorM ? kFarDrawFloorM : d;
    }
    static Caps capsAt(uint32_t engaged);
    const Caps& caps() const { return mCaps; }

    // ---- MEASURED FIXED COSTS (#407) ----
    // Categories a ladder rung can shrink (textures, RT, probes, shadows, mirrors, geometry).
    // Everything else -- swapchain, driver overhead, page padding of the rest, targets /
    // upscaler (only the LAST step, render scale, shrinks them), cull, scene tables, UI,
    // particles, staging -- is FIXED as far as the ladder is concerned. A new rung that
    // shrinks another category must list it here.
    static bool scalableCategory(uint32_t cat);
    // usage - sum of the scalable categories (+ their padding, + texture pool slack), >= 0.
    static uint64_t fixedOf(const VkDeviceContext::MemStats& ms, uint64_t usage);
    // Driver-internal bytes on heap h from VK_EXT_device_memory_report (0 without it):
    // every bucket but devmem (vkAllocateMemory) and swapchain (measured separately);
    // device and pipe overlap on NVIDIA (pipelines live in code-heap chunks reported as
    // device), so they count as max(device, pipe).
    static uint64_t driverInternalBytes(const VkDeviceContext::MemStats& ms, uint32_t h);
    // Process usage of heap h: VK_EXT_memory_budget when present; without it, what we
    // know is there -- our allocations + swapchain + page padding + driver-reported.
    static uint64_t heapUsage(const VkDeviceContext::MemStats& ms, uint32_t h, uint64_t swapchain);
    // #695: the usage that matches VkDeviceContext::budgetHeaps' budget -- the main heap's,
    // plus the shared host heap's on UMA.
    static uint64_t budgetUsage(const VkDeviceContext::MemStats& ms, const VkDeviceContext::BudgetHeaps& bh,
                                uint64_t swapchain);

    struct Input
    {
        double   now = 0.0;             // wall seconds
        uint64_t osBudget = 0;          // per-process heap budget (VK_EXT_memory_budget)
        uint64_t userCap = 0;           // "Maximum video memory" bytes (0 = Auto)
        uint64_t debugCap = 0;          // RenderVKDebugFakeVramBudgetMB bytes (0 = off)
        uint64_t shareCap = 0;          // LLVramShareVK::cap() (#403; 0 = none). Like the user
                                        // cap it sizes the budget but is no crash risk: the hard
                                        // step keeps judging the raw OS budget.
        uint64_t usage = 0;             // process usage on that heap
        uint32_t allocFailures = 0;     // VkDeviceContext::allocFailures() (monotonic)
        uint64_t failBytes = 0;         // size of the latest failed request (0 = unknown)
        uint64_t fixed = 0;             // measured fixed costs (fixedOf; 0 = not measured)
        // #614 pressure PRE-STEP: texture pool compaction is freeing slack (nothing visible
        // degrades): no rung is engaged while it runs -- mode / failure tracking go on, and
        // a failed request that still would not fit steps the ladder anyway. The renderer
        // bounds how long it holds.
        bool     holdLadder = false;
    };
    // One check. useful(r): engaging rung r would change something right now. readd(r)
    // (optional): the renderer's own estimate of what restoring applied rung r would
    // allocate again (~0ull = unknown: the measured, marginal drop is used). Returns the
    // rungs moved (+ engaged, - restored, 0 none). Logging is the caller's.
    int update(const Input& in, const std::function<bool(uint32_t)>& useful,
               const std::function<uint64_t(uint32_t)>& readd = {});
    // #563: a failed allocation is answered until the failed request would fit again
    // (usage + its size + a margin under the capacity), not for a fixed 5 s of fast steps.
    bool failurePending() const { return mFailNeed; }
    // The re-add the next restore waits for (the last applied rung; 0 = nothing engaged).
    // The texture residency budget leaves this much headroom free so the ladder can step
    // back up (#563: textures must not refill the room a recovery is waiting for).
    uint64_t pendingReadd() const { return mPendingReadd; }
    int32_t  pendingRung() const { return mPendingRung; }   // its rung (-1 = none)
    // The renderer may skip measuring between checks: true when a check is due.
    bool checkDue(double now) const { return now >= mNextCheck; }

    // Hard step (last-ditch crash avoidance, renderer-performed): the ladder has no
    // useful rung left AND (an allocation failed AFTER it ran out, with usage + that
    // request near the capacity, OR usage stayed above 97% of the OS budget for 5 s); at
    // most one per 15 s. (#563: a failure from before the ladder finished stepping no
    // longer counts -- it wrote RT settings at 40% usage.)
    bool hardStepDue(double now) const;
    void noteHardStep(double now, bool done);
    uint32_t hardSteps() const { return mHardSteps; }

    uint32_t engaged() const { return mRung; }
    uint32_t mode() const { return mMode; }           // 0 calm, 1 soft, 2 reserve (fast)
    bool     exhausted() const { return mExhausted; }
    uint64_t capacity() const { return mCapacity; }
    uint64_t reserve() const { return mReserve; }
    uint64_t budget() const { return mBudget; }
    // The measured fixed term (#407), smoothed (rises within ~2 s, decays over ~30 s so a
    // transient dip does not hand the room out), and the room it leaves the scalable
    // consumers: budget - fixed. Only sizes them (the texture residency budget); the
    // ladder itself keeps acting on total usage, so its order is unchanged.
    bool     fixedKnown() const { return mFixedInit && mBudget > 0; }
    uint64_t fixedCost() const { return (uint64_t)mFixedSm; }
    uint64_t scalableBudget() const { return mBudget > (uint64_t)mFixedSm ? mBudget - (uint64_t)mFixedSm : 0; }
    uint32_t deepestStep() const;
    // "off-screen textures, reflection probes (1/2 resolution), ..." ("" = nothing held).
    // label(i), when given and non-empty, replaces rung i's static text (live values such
    // as "draw distance 128 m (set 256 m)").
    std::string holdingText(const std::function<std::string(uint32_t)>& label = {}) const;
    double   calmNeed() const { return mCalmNeed; }
    // App focus regained (alt-tab recovery): skip the calm wait once.
    void     fastRecover(double now);

private:
    bool engageNext(double now, uint64_t usage, const std::function<bool(uint32_t)>& useful);
    double readdOf(uint32_t i, const std::function<uint64_t(uint32_t)>& readd) const;
    Caps     mCaps;
    uint32_t mRung = 0;
    uint32_t mMode = 0;
    bool     mExhausted = false;
    uint64_t mCapacity = 0, mReserve = 0, mBudget = 0, mUsage = 0, mOsBudget = 0;
    double   mFixedSm = 0.0, mFixedAt = 0.0;
    bool     mFixedInit = false;
    uint32_t mFailSeen = 0;
    bool     mFailInit = false;
    double   mFailUntil = -1.0;          // new allocation failure: answered at most until then
    double   mFailAt = -1.0e9;           // the latest new allocation failure
    uint64_t mFailBytes = 0;             // its request size (0 = unknown)
    uint32_t mFailRepeat = 0;            // failures within 10 s of each other
    bool     mFailNeed = false;          // ... and it would still not fit
    double   mExhaustedAt = 1.0e18;      // the ladder ran out of useful rungs (hard step gate)
    uint64_t mPendingReadd = 0;
    int32_t  mPendingRung = -1;
    double   mOverCapSince = 0.0;        // usage near the OS/driver budget since (0 = not); the
                                         // user's cap is a preference, not a crash risk
    double   mNextCheck = 0.0;
    double   mNextDown = 0.0;
    double   mCalmSince = 0.0;
    double   mCalmNeed = 10.0;
    double   mLastRestore = -1.0e9;
    double   mNextHard = 0.0;
    uint32_t mHardSteps = 0;
    static constexpr uint32_t kMaxRungs = 48;
    bool     mApplied[kMaxRungs] = {};
    uint64_t mUsageAt[kMaxRungs] = {};   // usage when the rung was engaged
    double   mAppliedAt[kMaxRungs] = {};
    uint64_t mFreed[kMaxRungs] = {};     // measured ~3 s (geometry rungs ~12 s) after engaging, and never
                                         // past the next rung's engagement (#563: marginal, so a burst does
                                         // not credit every rung with the whole drop) -- the re-add estimate
    bool     mFreedKnown[kMaxRungs] = {};
};

// ---- SHARING THE GPU WITH OTHER PROCESSES (#403) ----
// VK_EXT_memory_budget's heapBudget is a PER-PROCESS estimate, and how much it reflects
// other processes differs by platform:
//  - Windows (WDDM, AMD and NVIDIA): the OS budget of each process stays near the card
//    size while another process holds VRAM; when the sum oversubscribes the card WDDM
//    DEMOTES allocations to system memory (residency paging: multi-second freezes, and the
//    demoted bytes land in host RAM) before -- or instead of -- shrinking the budget. Two
//    viewers on "Auto" each sized themselves to most of an 8 GB card (#403).
//  - Linux (RADV, NVIDIA): the budget is about "ours + free VRAM", so it does shrink as
//    another process grows -- but first come, first served: the viewer that loaded first
//    keeps the card and the second one runs its whole ladder.
// Two parts, both soft (effective budget only; no setting is ever written):
//  1. The OS budget is SMOOTHED: down at once, up over ~30 s. A budget that bounces as
//     another process frees and refills (alt-tab, a peer's texture churn) no longer
//     re-grows the texture / RT targets into room that is about to vanish again.
//  2. SLIPSTREAM PEERS on the same GPU (matched by device UUID) publish their usage to a
//     small file each (temp dir, rewritten ~1/s, ignored when stale >30 s, removed when
//     >120 s -- a crashed viewer is forgotten). Each viewer's share is
//        max(pool / N, pool - sum(peer claims) - pool / 20),   pool = 90% of the heap
//     i.e. a fair share is always guaranteed and idle room may be borrowed. A peer under
//     memory pressure CLAIMS at least its fair share, so a borrower gives room back (no
//     first-come-takes-all). The share falls at once and rises over ~20 s (no fighting),
//     also after the last peer leaves.
// Not covered: a non-Slipstream app on Windows (a game) -- Vulkan has no device-wide
// usage there; the smoothed OS budget is all we see. The user cap still applies.
class LLVramShareVK
{
public:
    // dir: where the peer files live ("" = peers off); device_key: identifies the GPU
    // (device UUID hex); heap_size: the main device-local heap's size; pid: this process.
    void init(const std::string& dir, const std::string& device_key, uint64_t heap_size, long pid);
    bool initialized() const { return mInit; }
    struct Input
    {
        double   now = 0.0;             // wall seconds (any clock; deltas only)
        uint64_t osBudget = 0;          // raw OS per-process budget of the main heap
        uint64_t usage = 0;             // our usage of that heap
        uint64_t ownCeil = 0;           // what we could use without peers (user / debug cap; 0 = none)
        bool     pressured = false;     // the ladder holds something back / usage near the budget
        bool     peersOn = true;        // RenderVKVramShare
    };
    // Returns cap(): min(smoothed OS budget, peer share); 0 = nothing measured.
    uint64_t update(const Input& in);
    // App focus regained: the OS budget may jump back at once (the peer share does not) --
    // unless the OS budget is VOLATILE (#597 #599: it dropped > 5% within the last 5 min,
    // another app taking and releasing VRAM): then it keeps rising slowly.
    void     fastRecover() { mOsSnap = true; }
    // The OS budget fell by > 5% within the last 5 min (rises then take ~2 min, not 30 s).
    bool     osVolatile(double now) const { return mOsDropAt > 0.0 && now - mOsDropAt < kOsVolatileSecs; }
    static constexpr double kOsVolatileSecs = 300.0;
    // Remove our peer file (renderer shutdown).
    void     shutdown();

    uint64_t cap() const { return mCap; }
    uint64_t osSmoothed() const { return (uint64_t)mOsSm; }
    uint64_t peerCap() const { return mPeers ? (uint64_t)mPeerSm : 0; }
    uint32_t peers() const { return mPeers; }
    uint64_t peerClaims() const { return mPeerClaims; }
    // Pure: the share for this viewer given the pool, live viewer count (incl. us) and
    // the sum of the other viewers' claims.
    static uint64_t shareOf(uint64_t pool, uint32_t n, uint64_t others);

private:
    void publishAndScan(double wall, uint64_t usage, uint64_t claim);
    bool        mInit = false;
    std::string mDir, mKey, mFile;
    long        mPid = 0;
    uint64_t    mHeap = 0;
    double      mOsSm = 0.0, mOsAt = 0.0;
    bool        mOsSnap = false;
    double      mOsDropAt = 0.0;     // when the OS budget last fell by > 5% (0 = never)
    double      mPeerSm = 0.0, mPeerAt = 0.0;
    uint32_t    mPeers = 0;
    uint64_t    mPeerClaims = 0;
    double      mNextIo = 0.0;
    double      mNextSweep = 0.0;
    uint64_t    mCap = 0;
};
