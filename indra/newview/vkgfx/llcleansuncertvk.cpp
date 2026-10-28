/**
 * @file llcleansuncertvk.cpp
 * @brief SUN EXPOSURE CERTIFICATES (SXC; #771 Phase 2, RenderVKSunCert): a world-space cache of
 *        "this surface cannot see the sun for any sun direction along the next stretch of the
 *        sun's path", so the merged RT shadow pass skips the sun / moon sample where it is proven
 *        blocked -- and keeps skipping while the sun moves.
 *
 * WHY THE EARLIER SUN CACHES POPPED. The #771 RT mask tile cache certified "history uniform and
 * nothing changed since last frame" in screen space: the default day moves the sun ~7e-6 rad a
 * frame, above its 1e-6 drift threshold, so a 1/16 refresh slice re-traced every frame forever;
 * environment transitions forced every tile; camera motion lost the screen-space history. The
 * #567 cascade cache re-rendered whenever the sun moved half a shadow texel (every ~0.3 s). Both
 * defined validity against the PRESENT; a moving sun violates that everywhere, a little, always.
 *
 * WHAT SXC CERTIFIES INSTEAD: a stretch of the sun's FUTURE path.
 *   * THE ARC. The sun moves along a great circle per day-cycle span (a slerp between keyframes:
 *     llsettingssky / LLTrackBlenderLoopingTime). The host anchors it (La, tangent B, axis A = La x
 *     B) and cuts it into BINS of RenderVKSunCertBinDeg (2 deg: ~80 s at the default 4 h day). A
 *     bin's WEDGE is the bin widened by the sun's disc along the arc and by +-(lateral + disc)
 *     across it (RenderVKSunCertLateralDeg 0.5). The sun's position is a bin index; it advances
 *     once a bin, not once a frame.
 *   * THE CELL. Receivers hash to world-space cells (agent space; RenderVKSunCertCellSize at level
 *     0, doubled per level so a cell holds >= RenderVKSunCertCellPixels render pixels across) x
 *     the normal's dominant axis +- (the two faces of a thin wall / roof never share a cell) --
 *     camera motion does not touch them.
 *   * THE ROUND. A cell certifies the bins [rs, rs + 1 + RenderVKSunCertLookahead) from the sun's
 *     current one: RenderVKSunCertRays rays per bin, wedge corners first, from the receivers' own
 *     sun-ray origins (RenderVKSunCertTickets texels per cell per frame, one ray per bin each),
 *     against STATIC OPAQUE geometry only (opaque + terrain classes, first hit; a hit that moved
 *     since last frame / spins / is rigged or attached counts as open). A bin is blocked when no
 *     ray found it open. The round also records which 4 x 4 sub-cells traced it (coverage) and
 *     the nearest blocker (the penumbra radius a skipped texel reports).
 *   * THE SKIP (pass 15, sun sample only): certified for every bin the sun's disc touches now +
 *     the texel's sub-cell covered + younger than RenderVKSunCertMaxAge + not a spot check
 *     (1 / RenderVKSunCertSpotCheck of them trace anyway) -> the exact value a fully blocked
 *     trace writes; no ray, no terrain re-anchor, no terminator / rig probe.
 *   * RENEWAL: when the sun's disc reaches the round's last bin (or the certificate nears its
 *     age) the next round starts at the sun's current bin and commits before the old certificate
 *     runs out -- skipping never pauses on a steady day. A sun that does not move (fixed skies,
 *     most clubs) certifies one CONE around the anchor (0.15 deg + the disc) that never expires; once the sun
 *     has moved 0.1 deg its arc is known and the bins take over (one new epoch).
 * So a certificate dies only by the sun's real motion (minutes to hours), a proven change, or the
 * safety age -- never because a frame passed.
 *
 * INVALIDATION (each a reason the certificate can no longer be trusted, nothing else):
 *   * geometry: the ingest's static change spheres (gVKChangedSpheres: moved / re-registered /
 *     rezzed / deleted objects, LOD / BLAS swaps, mover class transitions), kept
 *     RenderVKSunCertChangeFrames frames (a new BLAS lands a few frames late), revoke every cell
 *     whose sweep toward the sun crosses them (maintenance kernel). Moving casters (avatars,
 *     animesh, movers, spinners) never certify, so they need no revocation: their shadow on an
 *     already dark cell changes nothing, and uncertified cells trace as today;
 *   * a lit ray: a spot check or an ordinary trace that comes back lit in a cell certified for
 *     the current bins revokes the cell and its 6 neighbours (a gap narrower than the rays'
 *     spacing, a change nobody reported); HYSTERESIS: each such revocation backs the cell's next
 *     round off x4 (RenderVKSunCertGapFrames x 4^n, n <= 3), forgiven one step per certificate
 *     that holds -- a marginal cell settles on "traced" instead of flickering;
 *   * the sun: its disc leaving the certified bins (renewal above); a jump past
 *     RenderVKSunCertJumpDeg in a frame, a sun <-> moon switch, the arc's lateral deviation past
 *     half its width (a keyframe kink): a new EPOCH (every entry reset lazily by the maintenance
 *     kernel; traced as today until re-certified, a few frames); FAST motion (an environment
 *     transition sweeps the sun at ~0.1-0.5 deg a frame) PAUSES SXC (everything traced) until
 *     the sun has been slow for 30 frames, then a new epoch;
 *   * the world: an agent origin shift (gVKOriginShiftGen: cells are in agent space), a settings
 *     change, the change-sphere list overflowing.
 *
 * MEMORY: one device-local buffer, RenderVKSunCertCap entries (rounded to 8-slot buckets) x 64 B
 * + a 4.5 KB header / stats / sphere block: 4 MB at the default 65536. Fixed: an entry no texel
 * looked up for RenderVKSunCertIdleSecs is evicted; past 85 % live the idle limit halves (LRU by
 * age) and recovers below 60 %. Live / cap / peak in the log. Freed when the setting is off or no
 * merged frame used it for 30 s; a refused allocation retries after 30 s (never fatal).
 * GPU: the maintenance kernel (one invocation per slot: ~4 MB read, a few us) is c_sxc /
 * g_sxc; its savings show in c_defer_shadowmerge. Round starts are budgeted per frame
 * (RenderVKSunCertRoundBudget): a teleport's cold start certifies over several frames, no burst.
 * LOG (RenderVKSunCertLog, ~5 s): "VKSunCert" -- skip %, the traced reasons, certification rays
 * per frame, rounds, revocations by cause, epochs by cause, cells live / cap / peak, the sun's
 * speed, the GPU ms.
 *
 * Requires the merged RT shadow pass on the generic shadow core (RenderVKRTShadowMerge +
 * RenderVKProjStochastic + RenderVKShadowGenericCore): SXC lives on the core's sun sample only
 * (sample 0), so ReSTIR DI's samples (#869) never see it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanrenderervk.h"

#include "llviewercontrol.h"
#include "llappviewer.h"        // gFrameCount
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

extern std::vector<float> gVKChangedSpheres;   // llcleaningest.cpp: static changes this frame (agent space)
extern bool gVKChangedOverflow;
extern U32 gVKOriginShiftGen;
extern std::vector<float> gVKDynSpheres;       // llcleaningest.cpp: dynamic-class casters (agent space)
extern std::vector<uint8_t> gVKDynSphereMoving; // ... 1 = moved recently / spins

namespace
{
// ---- clean_sun_cert.glsl's layout (keep in step) ----
constexpr uint32_t kHeaderWords = 64, kStatsOff = 64, kStatsWords = 64, kSpheresOff = 128, kMaxSpheres = 256;
constexpr uint32_t kTableOff = 1152, kEntryWords = 16, kSlotsPerBucket = 8, kBin0 = 1024;
constexpr uint32_t kHostWords = 48;   // header words the host writes each frame (40 / 41: the per-frame counters, zeroed)
static_assert(kHostWords <= kHeaderWords && kHeaderWords <= kStatsOff && kStatsOff + kStatsWords <= kSpheresOff
              && kSpheresOff + 4u * kMaxSpheres <= kTableOff, "clean_sun_cert.glsl layout");
enum : uint32_t
{
    H_LA = 0, H_B = 4, H_A = 8, H_EPOCH = 12, H_FRAME = 13, H_FLAGS = 14, H_CAPMASK = 15, H_BINLO = 16, H_BINHI = 17,
    H_SUNBIN = 18, H_NB = 19, H_SPOT = 20, H_TICKETS = 21, H_RPB = 22, H_BUDGET = 23, H_CELL = 24, H_LEVELS = 25,
    H_PXCELL = 26, H_TMAX = 27, H_NSPH = 28, H_IDLE = 29, H_MAXAGE = 30, H_GAP = 31, H_DEBUG = 32, H_CONE = 33,
    H_SPREAD = 34, H_ROUNDS = 40, H_LIVE = 41
};
enum : uint32_t
{
    S_ELIG = 0, S_SKIP, S_SPOT, S_UNCOV, S_NOCELL, S_NOCERT, S_CRAYS, S_CBLOCK, S_REVLIT, S_INSERT, S_FULL, S_INELIG,
    S_SPOTLIT, S_CERTTX, S_LIVE = 16, S_COMMIT, S_CERTNOW, S_RACTIVE, S_RSTART, S_RDONE, S_RBLOCK, S_REVCHG, S_REVFLAG, S_EVICT,
    S_EPOCH, S_FRAMES, S_DEFER, S_RRESTART
};
constexpr double kDeg = 0.017453292519943295;

struct SxcCfg
{
    uint32_t on = 0, debug = 0, lookahead = 4, rays = 6, spot = 16, cap = 65536, tickets = 4, budget = 4096,
             change_frames = 12, gap = 30, levels = 6;
    float bin_deg = 2.f, lat_deg = 0.5f, cell = 0.25f, cell_px = 8.f, max_age = 120.f, idle = 60.f, jump_deg = 2.f,
          fast_deg = 0.05f, soft = 0.01f;
    bool log = true;
};
SxcCfg sxcCfg()
{
    SxcCfg c;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_on(gSavedSettings, "RenderVKSunCert", 0U);
    static LLCachedControl<U32>  s_dbg(gSavedSettings, "RenderVKSunCertDebug", 0U);
    static LLCachedControl<F32>  s_bin(gSavedSettings, "RenderVKSunCertBinDeg", 2.f);
    static LLCachedControl<F32>  s_lat(gSavedSettings, "RenderVKSunCertLateralDeg", 0.5f);
    static LLCachedControl<U32>  s_look(gSavedSettings, "RenderVKSunCertLookahead", 4U);
    static LLCachedControl<U32>  s_rays(gSavedSettings, "RenderVKSunCertRays", 6U);
    static LLCachedControl<F32>  s_cell(gSavedSettings, "RenderVKSunCertCellSize", 0.25f);
    static LLCachedControl<F32>  s_cpx(gSavedSettings, "RenderVKSunCertCellPixels", 8.f);
    static LLCachedControl<U32>  s_spot(gSavedSettings, "RenderVKSunCertSpotCheck", 16U);
    static LLCachedControl<F32>  s_age(gSavedSettings, "RenderVKSunCertMaxAge", 120.f);
    static LLCachedControl<U32>  s_cap(gSavedSettings, "RenderVKSunCertCap", 65536U);
    static LLCachedControl<U32>  s_tk(gSavedSettings, "RenderVKSunCertTickets", 4U);
    static LLCachedControl<U32>  s_bud(gSavedSettings, "RenderVKSunCertRoundBudget", 4096U);
    static LLCachedControl<F32>  s_idle(gSavedSettings, "RenderVKSunCertIdleSecs", 60.f);
    static LLCachedControl<U32>  s_chg(gSavedSettings, "RenderVKSunCertChangeFrames", 12U);
    static LLCachedControl<U32>  s_gap(gSavedSettings, "RenderVKSunCertGapFrames", 30U);
    static LLCachedControl<F32>  s_jump(gSavedSettings, "RenderVKSunCertJumpDeg", 2.f);
    static LLCachedControl<F32>  s_fast(gSavedSettings, "RenderVKSunCertFastDeg", 0.05f);
    static LLCachedControl<bool> s_log(gSavedSettings, "RenderVKSunCertLog", true);
    static LLCachedControl<F32>  s_soft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
    c.on = s_on; c.debug = std::min<U32>(s_dbg, 2u);
    c.bin_deg = llclamp((F32)s_bin, 0.25f, 15.f);
    c.lat_deg = llclamp((F32)s_lat, 0.05f, 5.f);
    c.lookahead = std::min<U32>(s_look, 31u);
    c.rays = std::clamp<U32>(s_rays, 4u, 64u);
    c.cell = llclamp((F32)s_cell, 0.05f, 4.f);
    c.cell_px = llclamp((F32)s_cpx, 1.f, 64.f);
    c.spot = s_spot;
    c.max_age = llclamp((F32)s_age, 1.f, 3600.f);
    c.cap = std::clamp<U32>(s_cap, 4096u, 1u << 20);
    c.tickets = std::clamp<U32>(s_tk, 1u, 64u);
    c.budget = std::clamp<U32>(s_bud, 16u, 1u << 20);
    c.idle = llclamp((F32)s_idle, 1.f, 3600.f);
    c.change_frames = std::clamp<U32>(s_chg, 1u, 600u);
    c.gap = std::clamp<U32>(s_gap, 1u, 6000u);
    c.jump_deg = llclamp((F32)s_jump, 0.1f, 90.f);
    c.fast_deg = llclamp((F32)s_fast, 0.001f, 10.f);
    c.log = s_log;
    c.soft = s_soft;
#endif
    return c;
}
// A change of any of these re-certifies everything (a new epoch).
uint32_t cfgKey(const SxcCfg& c)
{
    uint32_t h = 2166136261u;
    auto mixu = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    auto mixf = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); mixu(u); };
    mixu(c.lookahead); mixu(c.rays); mixu(c.levels);
    mixf(c.bin_deg); mixf(c.lat_deg); mixf(c.cell); mixf(c.cell_px); mixf(c.soft);
    return h | 1u;
}
uint32_t pow2Floor(uint32_t v) { uint32_t p = 1u; while (p * 2u <= v) p *= 2u; return p; }
double sxcClock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sxcBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
void norm3(double v[3])
{
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const double a[3], const double b[3], double o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
double angle3(const double a[3], const double b[3])
{
    double c[3];
    cross3(a, b, c);
    return std::atan2(std::sqrt(dot3(c, c)), dot3(a, b));
}
// Epoch causes (the log).
enum { E_FIRST = 0, E_CFG, E_SHIFT, E_MOONSUN, E_JUMP, E_FAST, E_KINK, E_BACK, E_ARC, E_SPHERES, E_COUNT };
const char* const kEpochNames[E_COUNT] = {"start", "settings", "origin shift", "sun<->moon", "jump", "transition",
                                          "arc kink", "sun reversed / arc overrun", "cone->arc", "change overflow"};
} // namespace

struct LLCleanRendererVK::SunCertState
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    GpuBuffer buf{};
    uint32_t buckets = 0;          // power of two
    bool zeroed = false;           // the fill after allocation recorded
    double retryAt = 0.0;          // a refused allocation: next attempt
    double lastUse = -1e9;
    // ---- the sun's arc ----
    bool anchored = false, cone = true, paused = false;
    double La[3] = {0, 0, 1}, B[3] = {1, 0, 0}, A[3] = {0, 1, 0};
    double prevL[3] = {0, 0, 1};
    bool prevOk = false;
    float prevSunUp = -1.f;
    uint32_t calm = 0;
    uint32_t epoch = 0, frame = 0, key = 0, shiftGen = 0;
    bool epochWanted = false;
    int epochCause = E_FIRST;
    uint32_t epochBy[E_COUNT] = {};
    // the sun's speed (deg / s, EMA) and the frame rate (s / frame, EMA) for the seconds settings
    double speed = 0.0, dt = 1.0 / 60.0, lastT = 0.0;
    // ---- change spheres (agent space), kept change_frames ----
    struct Sph { float s[4]; uint32_t until; };
    std::vector<Sph> spheres;
    // ---- the table's fill (adaptive idle limit) ----
    uint32_t idleFrames = 3600;
    uint32_t live = 0, peak = 0;
    // ---- stats readback (2 frame slots, ~5 s) ----
    GpuBuffer rb[2]{};
    bool rbPending[2] = {false, false};
    GpuBuffer liveRb[2]{};          // last frame's live count, every frame (the adaptive idle limit)
    bool livePending[2] = {false, false};
    double logNext = 0.0;
    struct Host { uint32_t frames = 0, paused = 0, cone = 0, arc = 0, sph = 0, dyn = 0; double speed = 0.0; };
    Host host{}, hostRb[2]{};
    uint32_t epochRb[2][E_COUNT] = {};
    double msMaint = -1.0, msMerge = -1.0;
};

// pdnInit (inside the world PipelineBatchScope): the maintenance kernel and its push-only layout.
void LLCleanRendererVK::sunCertInit(VkDevice dev)
{
    if (!mSunCert) mSunCert = new SunCertState();
    SunCertState& st = *mSunCert;
    if (!st.layout)
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &li, nullptr, &st.layout) != VK_SUCCESS) st.layout = VK_NULL_HANDLE;
    }
    if (st.layout && !st.pipe)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = kWorldSunCertSpv_bytes;
        ci.pCode = kWorldSunCertSpv;
        VkShaderModule m = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
        vkNoteShaderModule(m, kWorldSunCertSpv, kWorldSunCertSpv_bytes, "SunCert");
        createComputePipelineInto(dev, m, st.layout, 0u, nullptr, &st.pipe, "mSunCert (SunCert maintenance)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
}

void LLCleanRendererVK::sunCertDestroy()
{
    if (!mSunCert || !mCtx) return;
    SunCertState& st = *mSunCert;
    VkDevice dev = mCtx->device();
    if (st.pipe) vkDestroyPipeline(dev, st.pipe, nullptr);
    if (st.layout) vkDestroyPipelineLayout(dev, st.layout, nullptr);
    for (GpuBuffer* b : {&st.buf, &st.rb[0], &st.rb[1], &st.liveRb[0], &st.liveRb[1]})
        if (b->buffer) { mCtx->destroyBuffer(*b); *b = GpuBuffer{}; }
    delete mSunCert;
    mSunCert = nullptr;
}

bool LLCleanRendererVK::sunCertWanted() const
{
    if (!mSunCert || !mSunCert->pipe) return false;
    U32 core = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_core(gSavedSettings, "RenderVKShadowGenericCore", 1U);
    core = s_core;
#endif
    return sxcCfg().on != 0u && core != 0u && mSunRtNow;
}

// invalidateProjShadows, before the ingest's list is cleared: this frame's static change spheres,
// kept RenderVKSunCertChangeFrames frames (a re-registered object's new BLAS can land late).
void LLCleanRendererVK::sunCertNoteSpheres()
{
    if (!mSunCert) return;
    SunCertState& st = *mSunCert;
    if (!sunCertWanted() || !st.buf.buffer)
    {
        st.spheres.clear();
        return;
    }
    const uint32_t now = (uint32_t)gFrameCount;
    st.spheres.erase(std::remove_if(st.spheres.begin(), st.spheres.end(),
                                    [&](const SunCertState::Sph& s) { return (int32_t)(s.until - now) < 0; }),
                     st.spheres.end());
    const size_t n = gVKChangedSpheres.size() / 4;
    if (gVKChangedOverflow || st.spheres.size() + n > kMaxSpheres)
    {
        // more changes than the maintenance kernel tests: start over (a region loading, a rez storm)
        st.spheres.clear();
        if (!st.epochWanted) { st.epochWanted = true; st.epochCause = E_SPHERES; }
        return;
    }
    const uint32_t until = now + sxcCfg().change_frames;
    for (size_t i = 0; i < n; ++i)
    {
        SunCertState::Sph s;
        std::memcpy(s.s, &gVKChangedSpheres[i * 4], sizeof(s.s));
        if (!std::isfinite(s.s[0]) || !std::isfinite(s.s[1]) || !std::isfinite(s.s[2]) || !std::isfinite(s.s[3])) continue;
        s.until = until;
        st.spheres.push_back(s);
    }
}

// recordLightPrep (the light prep: its own async segment ahead of the lighting, or inline), merged frames
// on the core kernel, between the prep's opening and closing barriers, before pass 15: the stats
// readback, the per-frame header + change spheres, the maintenance kernel. Returns the table's
// device address for the push (0: SXC off this frame -- pass 15 traces every sun ray).
uint64_t LLCleanRendererVK::sunCertRecord(VkCommandBuffer cmd, bool onAsync, bool coreKernel)
{
    if (!mSunCert || !mCtx) return 0;
    SunCertState& st = *mSunCert;
    const SxcCfg cfg = sxcCfg();
    const double now = sxcClock();
    sunCertHarvest();
    if (cfg.on != 0u && !coreKernel)
    {
        static bool s_told = false;
        if (!s_told)
            LL_INFOS("VKSunCert") << "sun exposure certificates (RenderVKSunCert) need the generic shadow core "
                                     "(RenderVKShadowGenericCore 1, its kernel compiled): the sun is traced everywhere"
                                  << LL_ENDL;
        s_told = true;
    }
    if (!coreKernel || !sunCertWanted())
    {
        // off: free the table once nothing used it for 30 s (or at once with the setting off)
        if (st.buf.buffer && (cfg.on == 0u || now - st.lastUse > 30.0))
        {
            vkDeviceWaitIdle(mCtx->device());   // (frames in flight may still read it)
            mCtx->destroyBuffer(st.buf);
            st.buf = GpuBuffer{};
            st.anchored = false;
            st.spheres.clear();
        }
        return 0;
    }
    st.lastUse = now;
    // ---- the table (fixed capacity; a cap change reallocates) ----
    const uint32_t buckets = pow2Floor(std::max(cfg.cap / kSlotsPerBucket, 1u));
    if (st.buf.buffer && st.buckets != buckets)
    {
        vkDeviceWaitIdle(mCtx->device());
        mCtx->destroyBuffer(st.buf);
        st.buf = GpuBuffer{};
    }
    if (!st.buf.buffer)
    {
        if (now < st.retryAt) return 0;
        const VkDeviceSize bytes = ((VkDeviceSize)kTableOff + (VkDeviceSize)buckets * kSlotsPerBucket * kEntryWords) * 4u;
        {
            VkMemScope memcat(VKMEM_RT_MASK);
            st.buf = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                               | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        }
        if (!st.buf.buffer || !st.buf.address)
        {
            // (#697: a refused allocation is never fatal -- traced as without SXC, retried later)
            if (st.buf.buffer) mCtx->destroyBuffer(st.buf);
            st.buf = GpuBuffer{};
            st.retryAt = now + 30.0;
            LL_WARNS("VKSunCert") << "sun exposure certificates: table (" << bytes / (1024 * 1024)
                                  << " MB) refused; the sun is traced everywhere, retry in 30 s" << LL_ENDL;
            return 0;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)st.buf.buffer, "sunCertTable");
        st.buckets = buckets;
        st.zeroed = false;
        st.anchored = false;
        st.rbPending[0] = st.rbPending[1] = false;
        st.peak = st.live = 0;
        LL_INFOS("VKSunCert") << "sun exposure certificates: table " << buckets * kSlotsPerBucket << " entries ("
                              << bytes / 1024 << " KB)" << LL_ENDL;
    }
    if (!st.zeroed)
    {
        vkCmdFillBuffer(cmd, st.buf.buffer, 0, VK_WHOLE_SIZE, 0u);
        sxcBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        st.zeroed = true;
    }

    // ---- the light: direction, speed, epoch triggers ----
    const float* ld = &mAtmos[7 * 4];
    double L[3] = {ld[0], ld[1], ld[2]};
    norm3(L);
    const float sunUp = mAtmos[9 * 4 + 3];
    const double disc = std::max((double)llclamp(cfg.soft, 0.f, 1.f) * 5.0, 0.05) * kDeg;   // (shadowMergeSunState's disc)
    const double beta = cfg.bin_deg * kDeg, mu = cfg.lat_deg * kDeg;
    if (st.lastT > 0.0) st.dt = 0.95 * st.dt + 0.05 * std::clamp(now - st.lastT, 1e-4, 0.5);
    st.lastT = now;
    double step = 0.0;
    if (st.prevOk) step = angle3(L, st.prevL);
    if (!std::isfinite(step)) step = 1e9;
    st.speed = 0.98 * st.speed + 0.02 * (step / std::max(st.dt, 1e-4) / kDeg);
    const uint32_t key = cfgKey(cfg);
    auto newEpoch = [&](int cause) { st.epochWanted = true; st.epochCause = cause; };
    if (!st.anchored) newEpoch(E_FIRST);
    if (key != st.key) { if (st.key) newEpoch(E_CFG); st.key = key; }
    if (gVKOriginShiftGen != st.shiftGen) { newEpoch(E_SHIFT); st.shiftGen = gVKOriginShiftGen; }
    if (st.prevOk && (sunUp > 0.5f) != (st.prevSunUp > 0.5f)) newEpoch(E_MOONSUN);
    if (st.prevOk && step > cfg.jump_deg * kDeg) newEpoch(E_JUMP);
    // FAST MOTION (an environment transition, a time-of-day drag): pause until it has been calm
    if (st.prevOk && step > cfg.fast_deg * kDeg) { st.paused = true; st.calm = 0; }
    else if (st.paused && ++st.calm >= 30u) { st.paused = false; newEpoch(E_FAST); }
    st.prevL[0] = L[0]; st.prevL[1] = L[1]; st.prevL[2] = L[2];
    st.prevSunUp = sunUp;
    st.prevOk = true;
    // ---- the arc: cone mode (a still sun) / arc mode (bins along the sun's great circle) ----
    auto anchorCone = [&]() {
        st.cone = true;
        st.La[0] = L[0]; st.La[1] = L[1]; st.La[2] = L[2];
        double up[3] = {0, 0, 1};
        if (std::fabs(L[2]) > 0.9) { up[0] = 1; up[2] = 0; }
        cross3(up, st.La, st.B); norm3(st.B);
        cross3(st.La, st.B, st.A); norm3(st.A);
    };
    if (st.epochWanted)
    {
        anchorCone();
        st.anchored = true;
    }
    uint32_t binLo = kBin0, binHi = kBin0, sunBin = kBin0, nb = 1;
    double theta = 0.0;
    if (st.cone)
    {
        // a still sun: one cone around the anchor; once it has moved a tenth of a degree its arc
        // is known well (the float noise of the light lane is ~1e-7 rad) -> arc mode
        const double ang = angle3(L, st.La);
        if (ang > 0.1 * kDeg)
        {
            double t[3] = {L[0] - st.La[0] * dot3(L, st.La), L[1] - st.La[1] * dot3(L, st.La), L[2] - st.La[2] * dot3(L, st.La)};
            norm3(t);
            st.B[0] = t[0]; st.B[1] = t[1]; st.B[2] = t[2];
            cross3(st.La, st.B, st.A); norm3(st.A);
            st.cone = false;
            if (!st.epochWanted) newEpoch(E_ARC);
        }
    }
    if (!st.cone)
    {
        theta = std::atan2(dot3(L, st.B), dot3(L, st.La));
        const double lat = std::asin(std::clamp(dot3(L, st.A), -1.0, 1.0));
        if (std::fabs(lat) > 0.5 * mu || theta < -0.5 * beta + disc || theta > 500.0 * beta)
        {
            // a keyframe kink (another great circle), the sun going back: re-anchor (cone first)
            if (!st.epochWanted) newEpoch(std::fabs(lat) > 0.5 * mu ? E_KINK : E_BACK);
            anchorCone();
            theta = 0.0;
        }
    }
    if (st.cone)
    {
        nb = 1;
    }
    else
    {
        nb = std::min(1u + cfg.lookahead, 32u);
        binLo = kBin0 + (uint32_t)std::floor((theta - disc) / beta + 0.5);
        binHi = kBin0 + (uint32_t)std::floor((theta + disc) / beta + 0.5);
        sunBin = kBin0 + (uint32_t)std::floor(theta / beta + 0.5);
    }
    if (st.epochWanted)
    {
        ++st.epoch;
        if ((st.epoch & 0xFFFFu) == 0u) ++st.epoch;   // (epoch 0: a never-initialised entry)
        ++st.epochBy[st.epochCause];
        st.epochWanted = false;
    }
    ++st.frame;
    st.host.frames++;
    if (st.paused) st.host.paused++;
    else if (st.cone) st.host.cone++;
    else st.host.arc++;
    st.host.speed += st.speed;

    // ---- the adaptive idle limit (bounded table: evict sooner while it is nearly full) ----
    const uint32_t cap = st.buckets * kSlotsPerBucket;
    const uint32_t idleMax = std::max(1u, (uint32_t)(cfg.idle / std::max(st.dt, 1e-3)));
    if (st.live > cap / 100u * 85u) st.idleFrames = std::max(st.idleFrames / 2u, 30u);
    else if (st.live < cap / 100u * 60u) st.idleFrames = std::min(idleMax, std::max(st.idleFrames + st.idleFrames / 8u + 1u, 30u));
    st.idleFrames = std::min(st.idleFrames, idleMax);

    // ---- the header (+ the per-frame counters zeroed) and the change spheres ----
    uint32_t h[kHostWords] = {};
    auto fw = [&](uint32_t i, double v) { const float f = (float)v; std::memcpy(&h[i], &f, 4); };
    for (int i = 0; i < 3; ++i) { fw(H_LA + i, st.La[i]); fw(H_B + i, st.B[i]); fw(H_A + i, st.A[i]); }
    fw(H_LA + 3, beta);
    fw(H_B + 3, mu + disc);
    fw(H_A + 3, disc);
    h[H_EPOCH] = st.epoch;
    h[H_FRAME] = st.frame;
    h[H_FLAGS] = st.paused ? 0u : 1u;
    h[H_CAPMASK] = st.buckets - 1u;
    h[H_BINLO] = binLo; h[H_BINHI] = binHi; h[H_SUNBIN] = sunBin; h[H_NB] = nb;
    h[H_SPOT] = cfg.spot <= 1u ? 0u : pow2Floor(cfg.spot) - 1u;
    if (cfg.spot == 0u) h[H_SPOT] = 0xFFFFFFFFu;   // (0: never a spot check)
    h[H_TICKETS] = cfg.tickets;
    h[H_RPB] = cfg.rays;
    h[H_BUDGET] = cfg.budget;
    fw(H_CELL, cfg.cell);
    h[H_LEVELS] = cfg.levels;
    fw(H_PXCELL, cfg.cell_px);
    fw(H_TMAX, 1024.0);   // (the sun ray's length: frame info sun_rt.w)
    // the sweep list: the kept static change spheres, then this frame's MOVING dynamic-class casters
    // (gVKDynSpheres + gVKDynSphereMoving: a mover the class tracks emits no change sphere while it
    // moves, and one that was still for a frame could have passed the certification rays' static
    // test) -- not kept, re-sent every frame they move; past the cap the movers are dropped, never
    // the static changes (those overflow into a new epoch in sunCertNoteSpheres)
    static float sph[kMaxSpheres * 4];
    uint32_t ns = (uint32_t)std::min<size_t>(st.spheres.size(), kMaxSpheres);
    for (uint32_t i = 0; i < ns; ++i) std::memcpy(&sph[i * 4], st.spheres[i].s, 16);
    {
        const size_t nd = std::min(gVKDynSpheres.size() / 4, gVKDynSphereMoving.size());
        for (size_t i = 0; i < nd && ns < kMaxSpheres; ++i)
        {
            if (!gVKDynSphereMoving[i]) continue;
            const float* d = &gVKDynSpheres[i * 4];
            if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2]) || !std::isfinite(d[3])) continue;
            std::memcpy(&sph[ns * 4], d, 16);
            ++ns;
            st.host.dyn++;
        }
    }
    h[H_NSPH] = st.paused ? 0u : ns;
    h[H_IDLE] = st.idleFrames;
    h[H_MAXAGE] = std::max(1u, (uint32_t)(cfg.max_age / std::max(st.dt, 1e-3)));
    h[H_GAP] = cfg.gap;
    h[H_DEBUG] = cfg.debug;
    // (cone mode: the anchor + the 0.1 deg the sun may move before its arc is taken + its disc)
    fw(H_CONE, st.cone ? 0.15 * kDeg + disc : 0.0);
    // the change spheres' sweep: every bin a cell holds or certifies around the range's middle
    fw(H_SPREAD, st.cone ? 0.15 * kDeg + 2.0 * disc : 0.5 * nb * beta + beta + mu + disc);
    // (40 / 41: the per-frame round-start and live counters start at 0)
    // (WAR -- last frame's pass 15 / maintenance / copies -> the copies + header update below: the light prep's
    //  opening barrier, recordLightPrep)
    // ---- stats readback (~5 s; harvested when this frame slot comes round again) ----
    const uint32_t slot = mFrameSlot & 1u;
    {
        // last frame's live count (the maintenance kernel's per-frame word), every frame
        GpuBuffer& lb = st.liveRb[slot];
        if (!lb.buffer)
            lb = mCtx->createBuffer(4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (lb.buffer && lb.mapped)
        {
            const VkBufferCopy c{(VkDeviceSize)H_LIVE * 4u, 0, 4u};
            vkCmdCopyBuffer(cmd, st.buf.buffer, lb.buffer, 1, &c);
            // (-> the host; and the header update below overwrites the word: WAR)
            sxcBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
            st.livePending[slot] = true;
        }
    }
    if (cfg.log && now >= st.logNext && !st.rbPending[slot])
    {
        GpuBuffer& rb = st.rb[slot];
        if (!rb.buffer)
            rb = mCtx->createBuffer((kStatsWords + 4u) * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (rb.buffer && rb.mapped)
        {
            st.logNext = now + 5.0;
            const VkBufferCopy c[2] = {{(VkDeviceSize)kStatsOff * 4u, 0, (VkDeviceSize)kStatsWords * 4u},
                                       {(VkDeviceSize)H_LIVE * 4u, (VkDeviceSize)kStatsWords * 4u, 4u}};
            vkCmdCopyBuffer(cmd, st.buf.buffer, rb.buffer, 2, c);
            sxcBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
            vkCmdFillBuffer(cmd, st.buf.buffer, (VkDeviceSize)kStatsOff * 4u, (VkDeviceSize)kStatsWords * 4u, 0u);
            st.rbPending[slot] = true;
            st.hostRb[slot] = st.host;
            std::memcpy(st.epochRb[slot], st.epochBy, sizeof(st.epochBy));
            std::memset(st.epochBy, 0, sizeof(st.epochBy));
            st.host = SunCertState::Host{};
        }
    }
    vkCmdUpdateBuffer(cmd, st.buf.buffer, 0, sizeof(h), h);
    if (ns > 0u && !st.paused)
    {
        vkCmdUpdateBuffer(cmd, st.buf.buffer, (VkDeviceSize)kSpheresOff * 4u, (VkDeviceSize)ns * 16u, sph);
        st.host.sph += ns;
    }
    sxcBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (st.paused) return st.buf.address;   // (flags 0: pass 15 traces everything, nothing is touched)
    // ---- maintenance ----
    struct { uint64_t addr; uint32_t slots; uint32_t pad; } pc{st.buf.address, cap, 0u};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st.pipe);
    vkCmdPushConstants(cmd, st.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (cap + 255u) / 256u, 1, 1);
    // (-> pass 15: the light prep's closing barrier)
    gpuSubMark(cmd, onAsync ? "c_sxc" : "g_sxc");
    return st.buf.address;
}

// This frame slot's stats copy (its fence was waited by acquire) -> one log line.
void LLCleanRendererVK::sunCertHarvest()
{
    if (!mSunCert) return;
    SunCertState& st = *mSunCert;
    const uint32_t slot = mFrameSlot & 1u;
    if (st.livePending[slot] && st.liveRb[slot].mapped)
    {
        st.livePending[slot] = false;
        std::memcpy(&st.live, st.liveRb[slot].mapped, 4);
        st.peak = std::max(st.peak, st.live);
    }
    if (!st.rbPending[slot] || !st.rb[slot].mapped) return;
    st.rbPending[slot] = false;
    uint32_t s[kStatsWords + 4] = {};
    std::memcpy(s, st.rb[slot].mapped, sizeof(s));
    const SunCertState::Host& h = st.hostRb[slot];
    if (!h.frames) return;
    const double fr = (double)h.frames;
    const uint32_t mf = std::max(s[S_FRAMES], 1u);
    const double elig = std::max((double)s[S_ELIG], 1.0);
    const uint32_t cap = st.buckets * kSlotsPerBucket;
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << "VKSunCert (#771 SXC): " << h.frames << " frames (arc " << h.arc << ", cone " << h.cone << ", paused "
       << h.paused << ")"
       << " | sun texels/frame " << (s[S_ELIG] + s[S_INELIG]) / fr << ": skipped " << 100.0 * s[S_SKIP] / elig
       << "% of eligible (" << s[S_ELIG] / fr << "/frame; not eligible " << s[S_INELIG] / fr << "/frame)"
       << " | traced: spot " << 100.0 * s[S_SPOT] / elig << "% (lit " << s[S_SPOTLIT] << ")"
       << " uncovered " << 100.0 * s[S_UNCOV] / elig << "% uncertified " << 100.0 * s[S_NOCERT] / elig
       << "% no cell " << 100.0 * s[S_NOCELL] / elig << "% certifying " << 100.0 * s[S_CERTTX] / elig << "%"
       << " | cert rays/frame " << s[S_CRAYS] / fr << " (blocked " << (s[S_CRAYS] ? 100.0 * s[S_CBLOCK] / s[S_CRAYS] : 0.0)
       << "%)"
       << " | rounds/frame started " << (double)s[S_RSTART] / mf << " committed " << (double)s[S_RDONE] / mf
       << " (with blocked bins " << (double)s[S_RBLOCK] / mf << ") active " << (double)s[S_RACTIVE] / mf
       << " deferred " << (double)s[S_DEFER] / mf << " restarted " << s[S_RRESTART]
       << " | revoked: lit ray " << s[S_REVLIT] << " (applied " << s[S_REVFLAG] << ") change " << s[S_REVCHG]
       << " (spheres/frame " << h.sph / fr << ", of them moving casters " << h.dyn / fr << ")"
       << " | cells live " << st.live << " / cap " << cap << " / peak " << st.peak << " (avg "
       << (double)s[S_LIVE] / mf << ", certified now " << (double)s[S_CERTNOW] / mf << ", holding "
       << (double)s[S_COMMIT] / mf << "), new " << s[S_INSERT] << " evicted " << s[S_EVICT] << " bucket full "
       << s[S_FULL] << ", idle limit " << st.idleFrames << " frames"
       << " | epochs:";
    bool any = false;
    for (int i = 0; i < E_COUNT; ++i)
        if (st.epochRb[slot][i]) { os << " " << kEpochNames[i] << " " << st.epochRb[slot][i]; any = true; }
    if (!any) os << " none";
    os << " (entries reset " << s[S_EPOCH] << ")";
    os.precision(4);
    os << " | sun " << h.speed / fr << " deg/s";
    os.precision(3);
    os << " | GPU ms/frame sxc=";
    if (st.msMaint >= 0.0) os << st.msMaint; else os << "n/a";
    os << " shadowmerge=";
    if (st.msMerge >= 0.0) os << st.msMerge; else os << "n/a";
    os << " (RenderVKGpuTimers window)";
    LL_INFOS("VKSunCert") << os.str() << LL_ENDL;
}

// The GPU timer window (RenderVKGpuTimers): the maintenance kernel and the merged pass, per frame.
void LLCleanRendererVK::sunCertGpuWindow(double k)
{
    if (!mSunCert) return;
    double sx = -1.0, mg = -1.0;
    for (const SubAcc& a : mSubAcc)
    {
        if (!std::strcmp(a.name, "c_sxc") || !std::strcmp(a.name, "g_sxc")) sx = std::max(sx, 0.0) + a.ms * k;
        else if (!std::strcmp(a.name, "c_defer_shadowmerge") || !std::strcmp(a.name, "g_defer_shadowmerge"))
            mg = std::max(mg, 0.0) + a.ms * k;
    }
    mSunCert->msMaint = sx;
    mSunCert->msMerge = mg;
}
