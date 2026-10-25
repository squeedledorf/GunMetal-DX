/**
 * @file llcleanclustercontrolvk.cpp
 * @brief Cluster LOD tolerance controller (#749, OWNER: 2c). See the header.
 *
 * Method: published material only (screen-space error budgets as presented in the SIGGRAPH
 * 2021 Nanite talk; the dwell / hysteresis pacing mirrors this viewer's own #613 controller).
 */
#include "linden_common.h"

#include "llcleanclustercontrolvk.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr float  kMaxTolPx      = 1024.f;  // clamp of the three pixel-tolerance settings
constexpr float  kLagRefM       = 2.f;     // LOD-camera dead band = hysteresis x this (metres)
constexpr float  kProtLoFrac    = 0.10f;   // avatar height / viewport height: protection starts
constexpr float  kProtHiFrac    = 0.60f;   // ... full protection
constexpr double kRaiseAfter    = 0.5;     // s over budget before a raise
constexpr double kMinStep       = 2.0;     // s between two raises
constexpr double kDwell         = 10.0;    // s comfortably under before a release (x2 per flip)
constexpr double kVramCalm      = 2.0;     // s of calm ladder before the VRAM floor drops
constexpr double kEaseUpSec     = 0.5;     // s per doubling while raising
constexpr double kEaseDownSec   = 2.0;     // s per halving while releasing
// Tolerance scale per view role (x the knob): see the header.
constexpr float  kRoleScale[CLROLE_COUNT] = {1.f, 2.f, 4.f, 1.f, 1.f};

float smooth01(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
float dist3(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
bool finite3(const float a[3]) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
} // namespace

void LLCleanClusterControlVK::update(const LLClusterPressureIn& in, double now, bool offscreen)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_tol(gSavedSettings, "RenderVKClusterTolerancePx", 1.f);
    static LLCachedControl<F32> s_prot(gSavedSettings, "RenderVKClusterProtectPx", 0.5f);
    static LLCachedControl<F32> s_rt(gSavedSettings, "RenderVKClusterRTTolerancePx", 2.f);
    static LLCachedControl<F32> s_hyst(gSavedSettings, "RenderVKClusterHysteresis", 0.15f);
    // (up to kMaxTolPx so the coarsest DAG levels can be forced for testing)
    mBase = std::clamp((float)(F32)s_tol, 0.05f, kMaxTolPx);
    mProtect = std::clamp((float)(F32)s_prot, 0.05f, kMaxTolPx);
    mRtTol = std::clamp((float)(F32)s_rt, 0.05f, kMaxTolPx);
    mHyst = std::clamp((float)(F32)s_hyst, 0.f, 0.5f);
#endif
    mOffscreen = offscreen;
    if (offscreen) return;   // a snapshot: the live state stays as it is

    const double dt = (mLastNow > 0.0) ? std::clamp(now - mLastNow, 0.0, 0.25) : 0.0;
    mLastNow = now;
    mIn = in;

    // ---- VRAM floor (the ladder decides; we follow its depth) ----
    uint32_t vram = 0;
    const uint32_t total = std::max<uint32_t>(in.vram_rungs, 3u);
    if (in.vram_ladder * 3u >= total * 2u) vram = 3;
    else if (in.vram_ladder * 3u >= total) vram = 2;
    else if (in.vram_ladder >= 1u) vram = 1;
    if (in.vram_avail != ~0ull && in.vram_avail < (256ull << 20)) vram = std::max(vram, 2u);
    if (vram >= mVram) { mVram = vram; mVramCalmSince = 0.0; }
    else if (mVramCalmSince <= 0.0) mVramCalmSince = now;
    else if (now - mVramCalmSince >= kVramCalm) { mVram = vram; mVramCalmSince = 0.0; }

    // ---- GPU time (the #613 measurement + the frame target) ----
    const float total_ms = in.rig_gpu_ms + in.cluster_gpu_ms;
    const bool gpu_bound = in.gpu_ok && in.frame_ms > 0.f && in.gpu_frame_ms >= 0.85f * in.frame_ms;
    const bool slow = in.frame_ms > in.target_ms;
    const bool fast = in.frame_ms > 0.f && in.frame_ms < in.target_ms * 0.75f;
    // The tolerance only moves the cluster raster: legacy-drawn work cannot be helped by it.
    const bool lever = in.cluster_gpu_ms >= 0.2f * in.budget_ms || in.cluster_gpu_ms >= 0.25f * total_ms;
    const bool over = in.perf_on && slow && gpu_bound && total_ms > in.budget_ms && lever;
    const float next_ms = in.rig_gpu_ms + in.cluster_gpu_ms * 4.f;   // one level down: ~4x the triangles
    const bool under = !in.perf_on || !in.gpu_ok || fast || next_ms <= in.budget_ms * 0.7f;
    mOverSince = over ? (mOverSince > 0.0 ? mOverSince : now) : 0.0;
    mUnderSince = under ? (mUnderSince > 0.0 ? mUnderSince : now) : 0.0;
    const uint32_t p0 = mPerf;
    const double dwell = kDwell * (double)(1u << std::min<uint32_t>(mFlip, 3u));
    if (!in.perf_on)
        mPerf = 0;
    else if (over && now - mOverSince >= kRaiseAfter && now - mChangeAt >= kMinStep && mPerf < kMaxLevel)
    {
        ++mPerf;
        if (mReleaseAt > 0.0 && now - mReleaseAt < 30.0) mFlip = std::min<uint32_t>(mFlip + 1u, 4u);
    }
    else if (under && mPerf > 0 && mUnderSince > 0.0 && now - mUnderSince >= dwell && now - mChangeAt >= dwell)
    {
        --mPerf;
        mReleaseAt = now;
        mUnderSince = now;   // the next step waits a full dwell
    }
    if (mFlip && mReleaseAt > 0.0 && now - mReleaseAt > 300.0) mFlip = 0;
    const uint32_t lvl = std::max(mPerf, mVram);
    if (mPerf != p0 || lvl != mLevel)
    {
        LL_INFOS("Vulkan") << "Cluster LOD control: tolerance level " << mLevel << " -> " << lvl << " (x"
                           << (1u << lvl) << ", " << mBase * (float)(1u << lvl) << " px; perf " << mPerf << " vram "
                           << mVram << "; cluster+rigged GPU " << total_ms << " ms vs budget " << in.budget_ms
                           << " ms, frame " << in.frame_ms << " / target " << in.target_ms << " ms, "
                           << (gpu_bound ? "GPU-bound" : (in.gpu_ok ? "not GPU-bound" : "no GPU timestamps"))
                           << ")" << LL_ENDL;
        mChangeAt = now;
    }
    mLevel = lvl;

    // ---- the eased tolerance (log space) ----
    const float target = mBase * (float)(1u << mLevel);
    if (dt <= 0.0 || !(mTol > 0.f) || !std::isfinite(mTol)) mTol = target;
    else
    {
        const double lt = std::log((double)target), lc = std::log((double)mTol);
        const double up = dt * std::log(2.0) / kEaseUpSec, down = dt * std::log(2.0) / kEaseDownSec;
        const double ln = lt > lc ? std::min(lt, lc + up) : std::max(lt, lc - down);
        mTol = (float)std::exp(ln);
        if (std::fabs(mTol - target) < target * 1e-3f) mTol = target;
    }
}

void LLCleanClusterControlVK::setMainView(const float cam[3], float px_scale, float viewport_h, float znear)
{
    mZnear = std::max(znear, 1e-3f);
    mMainH = viewport_h;
    if (!finite3(cam) || !(px_scale > 0.f) || !std::isfinite(px_scale))
    {
        mMainValid = false;
        return;
    }
    float lc[4];
    float px = px_scale, tol = mTol;
    applyView(CLROLE_MAIN, 0, cam, false, px, tol, lc);
    mMainCam[0] = lc[0]; mMainCam[1] = lc[1]; mMainCam[2] = lc[2];
    mMainPx = px;
    mMainValid = true;
}

float LLCleanClusterControlVK::roleTolPx(uint32_t role) const
{
    if (role >= CLROLE_OTHER) return mRtTol;
    return tolPx() * kRoleScale[role];
}

void LLCleanClusterControlVK::applyView(uint32_t role, uint32_t sub, const float cam[3], bool ortho, float& px_scale,
                                        float& tol_px, float lod_cam[4])
{
    lod_cam[0] = cam[0]; lod_cam[1] = cam[1]; lod_cam[2] = cam[2]; lod_cam[3] = 0.f;
    if (role >= CLROLE_OTHER) return;   // RT / other: as given
    tol_px = roleTolPx(role);
    if (mOffscreen || mHyst <= 0.f || !finite3(cam) || !(px_scale > 0.f)) return;   // live inputs
    Lag& L = lag(role, sub);
    // px_scale: finer at once, coarser only past the margin.
    if (!L.valid || px_scale > L.px || px_scale < L.px * (1.f - mHyst)) L.px = px_scale;
    px_scale = L.px;
    // The LOD camera (perspective views with a moving camera: main, mirror).
    if (ortho || role == CLROLE_PROBE || role == CLROLE_SHADOW)
    {
        L.valid = true;
        return;
    }
    const float band = mHyst * kLagRefM;
    if (!L.valid || dist3(cam, L.cam) > band)
    {
        L.cam[0] = cam[0]; L.cam[1] = cam[1]; L.cam[2] = cam[2];
        if (L.valid && role == CLROLE_MAIN) ++mLagJumps;
    }
    L.valid = true;
    lod_cam[0] = L.cam[0]; lod_cam[1] = L.cam[1]; lod_cam[2] = L.cam[2];
    lod_cam[3] = band;   // >= |cam - lod_cam|: the projection never under-estimates
}

float LLCleanClusterControlVK::avatarTolScale(bool self, const float av_pos[3], float av_height_m, float* arena_out) const
{
    // The avatar's on-screen height as a fraction of the viewport (from the held LOD camera);
    // -1 = unknown (no main view yet / non-finite position).
    float frac = -1.f;
    if (mMainValid && finite3(av_pos) && mMainH > 0.f)
    {
        const float margin = (mOffscreen || mHyst <= 0.f) ? 0.f : mHyst * kLagRefM;
        const float d = std::max(dist3(av_pos, mMainCam) - margin, mZnear);
        const float h_px = std::max(av_height_m, 0.5f) * mMainPx / d;
        frac = h_px / mMainH;
    }
    // #1010 ARENA term: 2^clamp(P - L, 0, max) -- L grows with the on-screen size (an avatar of
    // unknown size counts as mid-size), your own avatar last.
    float arena = 1.f;
    if (mArenaP > 0.f)
    {
        const float L = self ? kArenaSelfL
                      : frac > 0.f ? std::clamp(std::log2(frac / kArenaRefFrac), 0.f, kArenaSpan) : kArenaSpan * 0.5f;
        const float extra = std::clamp(mArenaP - L, 0.f, kArenaMaxExtra);
        if (extra > 0.f) arena = std::exp2(extra);
    }
    if (arena_out) *arena_out = arena;
    float w = 1.f;
    if (!self)
    {
        if (frac < 0.f) return 1.f;
        w = smooth01(kProtLoFrac, kProtHiFrac, frac);
        if (w <= 0.f) return 1.f;
    }
    const float mult = mOffscreen ? 1.f : (float)(1u << mLevel);
    const float protect = mProtect * std::sqrt(mult);
    const float tol = std::max(tolPx(), 1e-3f);
    const float s = std::min(1.f, protect / tol);
    return 1.f + (s - 1.f) * w;
}

const char* LLCleanClusterControlVK::reason() const
{
    if (mOffscreen) return "snapshot";
    if (mLevel == 0) return mTol > mBase * 1.001f ? "easing" : "none";
    if (mVram >= mPerf) return mPerf == mVram ? "gpu+vram" : "vram";
    return "gpu";
}

void LLCleanClusterControlVK::summary(std::string& out) const
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "tol %.2f px (x%u %s)", tolPx(), 1u << mLevel, reason());
    out += buf;
    if (mArenaP > 0.f)
    {   // #1010: the skinned arena's per-avatar coarsening is on
        std::snprintf(buf, sizeof(buf), " arena P %.2f", mArenaP);
        out += buf;
    }
}

void LLCleanClusterControlVK::statsLine(std::string& out) const
{
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "control tol %.2f px base %.2f level %u (perf %u vram %u, %s) protect %.2f px hyst %.2f "
                  "lod-cam jumps %u; gpu rig %.2f + cluster %.2f ms / budget %.2f, frame %.1f / target %.1f ms; arena P %.2f",
                  tolPx(), mBase, mLevel, mPerf, mVram, reason(), mProtect, mHyst, mLagJumps, mIn.rig_gpu_ms,
                  mIn.cluster_gpu_ms, mIn.budget_ms, mIn.frame_ms, mIn.target_ms, mArenaP);
    out += buf;
}
