/**
 * @file llcleanweathervk_movers.cpp
 * @brief Weather mover tracks (#783 M5): vehicles, physical and animated objects press tracks and
 *        push berms into the snow depth. See llcleanweathervk.h (MOVER TRACKS) and
 *        clean_weather_snow.comp (mvAppend / mvApply).
 *
 * The GPU does the work: the instance pass appends each moving mover instance's box to a bounded
 * stamp list, the cover / ring passes gather it. This side only decides whether a step stamps at
 * all (the ingest's mover tracker has a recently moved caster within the field's reach -- no
 * movers: nothing appended, the passes skip their cull), writes the list's head / parameters and
 * keeps the stats from the head's readback.
 */
#include "linden_common.h"

#include "llcleanweathervk.h"

#include "llerror.h"
#include "llmath.h"
#include "llmemtrend.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// llcleaningest.cpp (#567 / #771): the dynamic casters' agent-space spheres (x, y, z, r) and,
// parallel, 1 = moved within the last two frames / 2 = a GPU spinner / 0 = settling.
extern std::vector<float> gVKDynSpheres;
extern std::vector<uint8_t> gVKDynSphereMoving;

namespace
{
// The least speed (m/s) of a box centre that stamps (jitter / a settling physical object does
// not), the packed layer a pressed track keeps: fresh / packed density (fresh snow ~0.1 g/cm3,
// packed by a wheel ~0.3-0.5: it packs ~3x), x the fill level. The volume counters' unit (m3:
// clean_weather_snow.comp kMvVolUnit).
constexpr float kMoverMinSpeed = 0.08f;
constexpr float kMoverPacked = 0.3f;
// #783 BODY CONTACT's joint fallback: how far a quadruped's hoof reaches below its hand joint (m at
// the default scale; clean_weather_snow.comp limbContact).
constexpr float kArmLegMargin = 0.08f;
constexpr double kMoverVolUnit = 1e-6;
}

LLCleanWeatherVK::MoverStats LLCleanWeatherVK::sMoverStats;

// MemTrend (#883): the mover list's VRAM and its last step's stamps.
void LLCleanWeatherVK::registerMoverProbe()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    LLMemTrend::registerProbe("vk-weather-movers", []
    {
        const MoverStats& s = sMoverStats;
        LLMemTrend::Sample out;
        out.count = s.stamps + s.limbs;
        out.bytes = s.bytes;
        out.cap = 0;
        out.peak = s.bytes;
        return out;
    });
}

// planSnow (live steps with the cover pass due): whether the instance pass stamps movers (movers:
// it runs this step; the tracker has a recently moved caster within the field's reach) and whether
// the avatar pass stamps limbs (limbs, #783 follow-up), and -- for either -- the list's head +
// parameters (recordSnow uploads them with the reset). Sets mMoverDue / mLimbDue.
void LLCleanWeatherVK::planMovers(const Frame& f, bool movers, bool limbs)
{
    static LLCachedControl<bool> s_movers(gSavedSettings, "RenderVKWeatherSnowMovers", true);
    static LLCachedControl<F32>  s_press(gSavedSettings, "RenderVKWeatherSnowMoverPress", 1.f);
    static LLCachedControl<F32>  s_berm(gSavedSettings, "RenderVKWeatherSnowMoverBerm", 0.15f);
    static LLCachedControl<U32>  s_cap(gSavedSettings, "RenderVKWeatherSnowMoverStamps", 256);
    // #783 BODY CONTACT (clean_weather_snow.comp mv_bprm)
    static LLCachedControl<F32>  s_bodySupport(gSavedSettings, "RenderVKWeatherSnowBodySupport", 0.06f);
    static LLCachedControl<F32>  s_bodyDrift(gSavedSettings, "RenderVKWeatherSnowBodyDrift", 0.4f);
    static LLCachedControl<F32>  s_armLegs(gSavedSettings, "RenderVKWeatherSnowArmLegs", 0.15f);
    mMoverDue = mLimbDue = false;
    sMoverStats.tracked = 0;
    const float press = llclamp((F32)s_press, 0.f, 1.f);
    if (press <= 0.f || !f.camTime || !mCoverPipe) return;
    if (movers && s_movers && mInstPipe)
    {
        // The field's reach around the camera: the published inner grid's and the outer ring's
        // half diagonals (both are centred on the camera's column, snapped).
        float reach = 0.f;
        if (mCoverPub >= 0) reach = std::max(reach, 0.7072f * mCoverPubF[2]);
        if (mRingPub >= 0 && mRing[0].image) reach = std::max(reach, 0.7072f * mRingPubF[2]);
        // The tracker: a caster that moved within the last two frames (not a spinner turning in
        // place, not one settling) whose sphere reaches the field. Counted, not listed: the GPU
        // finds the boxes itself (instance bit21).
        const size_t n = reach > 0.f ? gVKDynSpheres.size() / 4 : 0;
        uint32_t tracked = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (i >= gVKDynSphereMoving.size() || gVKDynSphereMoving[i] != 1u) continue;
            const float* c = &gVKDynSpheres[i * 4];
            const float dx = c[0] - f.camTime[0], dy = c[1] - f.camTime[1];
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(c[3])) continue;
            if (std::sqrt(dx * dx + dy * dy) - c[3] < reach) ++tracked;
        }
        sMoverStats.tracked = tracked;
        mMoverDue = tracked > 0;
    }
    mLimbDue = limbs && mAvPipe;
    if (!mMoverDue && !mLimbDue) return;
    const uint32_t cap = llclamp((U32)s_cap, 16u, kMoverStampMax);
    // Head (clean_weather_snow.comp WxMovers): appends, the movers' cap, their cap for boxes outside
    // the inner grid (half), the limbs' appends (their own slots after kMoverStampMax); the
    // parameters: press, berm share, least speed, packed share; the totals (zeroed: removed,
    // deposited, texels pressed / deposited on).
    const float prm[4] = {press, llclamp((F32)s_berm, 0.f, 1.f), kMoverMinSpeed, kMoverPacked};
    mMoverHead[0] = 0u; mMoverHead[1] = cap; mMoverHead[2] = cap / 2u; mMoverHead[3] = 0u;
    std::memcpy(&mMoverHead[4], prm, sizeof(prm));
    mMoverHead[8] = mMoverHead[9] = mMoverHead[10] = mMoverHead[11] = 0u;
    // #783 BODY CONTACT: the support height, the raster-time pelvis drift, the arm-leg distance, the
    // hoof margin below the hand joint (m); the counters (GPU) zeroed.
    const float bprm[4] = {llclamp((F32)s_bodySupport, 0.f, 0.5f), llclamp((F32)s_bodyDrift, 0.f, 4.f),
                           llclamp((F32)s_armLegs, 0.f, 1.f), kArmLegMargin};
    std::memcpy(&mMoverHead[12], bprm, sizeof(bprm));
    std::fill(mMoverHead + 16, mMoverHead + 24, 0u);
    sMoverStats.cap = cap;
    sMoverStats.limbCap = kLimbStampMax;
}

// The head of a frame slot's last mover list (its readback, kMoverHeadBytes): [0] appends (past the
// cap: dropped), [1] the cap, [4..7] the parameters, [8] removed / [9] deposited (kMoverVolUnit m3),
// [10] texels pressed, [11] texels deposited on; #783 BODY CONTACT [12..15] its parameters,
// [16..23] its counters (clean_weather_snow.comp mv_bd0 / mv_bd1).
void LLCleanWeatherVK::readMoverHead(const uint32_t* head)
{
    const uint32_t cap = head[1];
    if (!cap) return;
    sMoverStats.removedM3 = (float)(head[8] * kMoverVolUnit);
    sMoverStats.depositedM3 = (float)(head[9] * kMoverVolUnit);
    sMoverStats.pressedTexels = head[10];
    sMoverStats.bermTexels = head[11];
    sMoverStats.removedTotalM3 += head[8] * kMoverVolUnit;
    sMoverStats.depositedTotalM3 += head[9] * kMoverVolUnit;
    {
        // CONSERVATION check (#783 feedback): every ~10 s while movers press, removed vs deposited
        // (the berms may take at most the berm share of the removed snow, minus what the caps cut).
        static F64 s_next = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (head[8] && now >= s_next)
        {
            s_next = now + 10.0;
            F32 berm = 0.f;
            std::memcpy(&berm, &head[5], sizeof(berm));
            const double r = sMoverStats.removedTotalM3 > 0.0 ? sMoverStats.depositedTotalM3 / sMoverStats.removedTotalM3 : 0.0;
            LL_INFOS("VKWeather") << "weather mover / limb tracks: step removed " << sMoverStats.removedM3 << " m3 ("
                                  << head[10] << " texels), deposited " << sMoverStats.depositedM3 << " m3 (" << head[11]
                                  << " texels); since start " << sMoverStats.removedTotalM3 << " / "
                                  << sMoverStats.depositedTotalM3 << " m3 = " << r << " (berm share " << berm
                                  << ", must stay <= it); stamps " << std::min(head[0], cap) << "/" << cap << ", limbs "
                                  << std::min(head[3], kLimbStampMax) << "/" << kLimbStampMax << LL_ENDL;
        }
    }
    {
        // LIMB CONTACT (#783 follow-up): the limb boxes appended (head[3]; past kLimbStampMax: dropped).
        const uint32_t limbs = std::min(head[3], kLimbStampMax);
        sMoverStats.limbs = limbs;
        sMoverStats.limbDropped = head[3] - limbs;
        sMoverStats.limbPeak = std::max(sMoverStats.limbPeak, limbs);
        if (sMoverStats.limbDropped && (sMoverStats.limbOverflowSteps++ % 600u) == 0u)
            LL_INFOS("VKWeather") << "weather limb contact: " << head[3] << " limb boxes reach the snow, " << kLimbStampMax
                                  << " slots; the rest skipped this step (RenderVKWeatherSnowLimbRange narrows it)" << LL_ENDL;
    }
    {
        // #783 BODY CONTACT: who pressed with the bottom map, who with the joints (that step).
        sMoverStats.bodyAvatars = head[16]; sMoverStats.bodySupport = head[17];
        sMoverStats.bodyOther = head[18]; sMoverStats.bodyStamps = head[19];
        sMoverStats.jointAvatars = head[20]; sMoverStats.jointStamps = head[21];
        sMoverStats.armLegs = head[22]; sMoverStats.bodyMaxContacts = head[23];
        static F64 s_nextBody = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if ((head[16] || head[20]) && now >= s_nextBody)
        {
            s_nextBody = now + 10.0;
            const float per = head[16] ? (float)(head[17] + head[18]) / (float)head[16] : 0.f;
            LL_INFOS("VKWeather") << "weather snow body contact: " << head[16] << " avatars on their bottom map ("
                                  << head[17] << " supporting + " << head[18] << " other contacts, " << per
                                  << " per avatar, most " << head[23] << "; " << head[19] << " footprints), "
                                  << head[20] << " on the joints (" << head[21] << " footprints, " << head[22]
                                  << " arms standing as legs)" << LL_ENDL;
        }
    }
    const uint32_t stamped = std::min(head[0], cap);
    sMoverStats.stamps = stamped;
    sMoverStats.dropped = head[0] - stamped;
    sMoverStats.peak = std::max(sMoverStats.peak, stamped);
    if (sMoverStats.dropped) ++sMoverStats.overflowSteps;
    if (sMoverStats.dropped && sMoverStats.overflowSteps % 600u == 1u)   // (the first, then every 600th)
    {
        LL_INFOS("VKWeather") << "weather mover tracks: " << head[0] << " moving boxes reach the snow, cap " << cap
                              << " (RenderVKWeatherSnowMoverStamps); the rest skipped this step" << LL_ENDL;
    }
}
