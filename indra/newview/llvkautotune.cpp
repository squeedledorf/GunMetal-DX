/**
 * @file llvkautotune.cpp
 * @brief "Improve graphics speed" under Vulkan (#422): our own FPS auto-tuner. See the header.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvkautotune.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "llperfstats.h"
#include "llstartup.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvkhitch.h"
#include "llvoavatar.h"
#include "llwindow.h"
#include "llworld.h"
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace
{
    double wallNow()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Localized text with an English fallback (strings.xml "AutoTune*").
    std::string tr(const char* key, const char* fallback, const LLStringUtil::format_map_t& args = {})
    {
        std::string s;
        if (LLTrans::findString(s, key, args)) return s;
        s = fallback;
        LLStringUtil::format(s, args);
        return s;
    }

    std::string fmtMs(float ms)
    {
        return llformat(ms < 10.f ? "%.1f" : "%.0f", ms);
    }

    // ---- measurement constants (#422) ----
    constexpr double kWindowSec     = 3.0;    // one measurement window (camera movement averages out)
    constexpr double kSettleSec     = 2.0;    // after a step, before the next window
    constexpr double kSettleRealloc = 4.0;    // after a step that re-allocates targets (hitches)
    constexpr double kRigDwellSec   = 6.0;    // continuous: extra wait while the rigged controller acts
    constexpr float  kSlowFactor    = 1.08f;  // window p75 above target x this = slow
    constexpr float  kMetFactor     = 1.03f;  // the first window within target x this = already there
    constexpr float  kHeadroom      = 0.97f;  // Tune once stops once busy time <= target x this
    constexpr float  kComfyFactor   = 0.75f;  // continuous: busy time below target x this = headroom
    constexpr uint32_t kMinFrames   = 20;
    constexpr uint32_t kMaxOnceSteps = 25;    // kept changes
    constexpr uint32_t kMaxOnceTests = 48;    // A/B tests (probes, bisection steps, bundles, confirmations)
    constexpr double kMaxOnceSec    = 225.0;  // the whole run (a test running at the limit decides on what it has)
    constexpr double kMinTestSec    = 8.0;    // no new test with less time than this left
    // ---- Keep my FPS experiments ----
    constexpr float  kGainAbsMs     = 0.2f;   // a cap must save at least this much ...
    constexpr float  kGainRel       = 0.025f; // ... and this share of the baseline p75
    constexpr float  kNoiseSigmas   = 2.0f;   // (logged bar: this many standard errors of the difference)
    constexpr double kChunkMs       = 500.0;  // window chunks for the noise estimate (autocorrelated frames)
    // ---- Tune once A/B tests (#422 round 2) ----
    constexpr double kSegSec        = 1.5;    // one A or B segment
    constexpr double kAbSettle      = 0.75;   // after a switch ...
    constexpr double kAbSettleRealloc = 2.5;  // ... of a lever that re-allocates targets
    constexpr double kAbSettleStream  = 2.0;  // ... of a one-way (streaming) lever
    constexpr uint32_t kSegMinFrames = 8;
    constexpr uint32_t kOnewayA     = 2;      // one-way tests: A segments before the switch
    constexpr float  kBarAbsMs      = 0.15f;  // the measurable / worthwhile gain: this much ...
    constexpr float  kBarRel        = 0.02f;  // ... or this share of the level, whichever is larger
    constexpr float  kPromising     = 0.5f;   // more pairs only while the gain is >= this share of the bar
    constexpr float  kBisectShare   = 0.75f;  // bisection: the mildest stage keeping this share of the probe's gain
    constexpr uint32_t kMaxBundle   = 4;
    constexpr float  kConfirmP75    = 0.90f;  // confirmation pass: p75 within target x this ...
    constexpr float  kConfirmBusy   = 0.85f;  // ... and busy time within target x this
    constexpr uint32_t kMaxConfirm  = 2;
    constexpr double kCapBlockSec   = 60.0;   // Keep my FPS: a cap that did not help stays off this long
    constexpr int    kHistMax       = 4;      // per-lever "did not help here" count (prior weight)
    constexpr float  kHistWeight    = 0.75f;  // rank key x (1 + count x this)
}

std::mutex LLVKAutoTune::sCatMutex;
LLVKAutoTune::GpuCats LLVKAutoTune::sCats;
std::atomic<bool> LLVKAutoTune::sWantTimers{false};
std::atomic<float> LLVKAutoTune::sRigTargetFps{0.f};

LLVKAutoTune& LLVKAutoTune::instance()
{
    static LLVKAutoTune s;
    return s;
}

LLVKPerfCaps LLVKPerfCaps::merged(const LLVKPerfCaps& a, const LLVKPerfCaps& b)
{
    LLVKPerfCaps m;
    m.renderScaleMax = std::min(a.renderScaleMax, b.renderScaleMax);
    m.probeResShift = std::max(a.probeResShift, b.probeResShift);
    m.probeCountShift = std::max(a.probeCountShift, b.probeCountShift);
    m.farShadowsOff = a.farShadowsOff || b.farShadowsOff;
    m.mirrorsOff = a.mirrorsOff || b.mirrorsOff;
    m.projShadowsOff = a.projShadowsOff || b.projShadowsOff;
    m.meshLodScale = std::min(a.meshLodScale, b.meshLodScale);
    m.avatarLodScale = std::min(a.avatarLodScale, b.avatarLodScale);
    m.drawDistScale = std::min(a.drawDistScale, b.drawDistScale);
    return m;
}

// ============================================================================
// Settings
// ============================================================================

U32 LLVKAutoTune::clampTargetFPS(U32 v)
{
    if (v == 0) return kTargetFpsDefault;   // the old refresh-rate bug's value
    return llclamp(v, kTargetFpsMin, kTargetFpsMax);
}

void LLVKAutoTune::validateSettings()
{
    const U32 t = gSavedSettings.getU32("TargetFPS");
    const U32 c = clampTargetFPS(t);
    if (c != t)
    {
        LL_WARNS("AutoTune") << "TargetFPS " << t << " out of range: repaired to " << c << LL_ENDL;
        gSavedSettings.setU32("TargetFPS", c);
    }
    // "Keep my FPS" persists only when the user chose "future login sessions".
    if (gSavedSettings.getBOOL("AutoTuneLock") && !gSavedSettings.getU32("KeepAutoTuneLock"))
    {
        gSavedSettings.setBOOL("AutoTuneLock", false);
    }
    instance().mContinuous = gSavedSettings.getBOOL("AutoTuneLock");
}

S32 LLVKAutoTune::refreshRate()
{
    return (gViewerWindow && gViewerWindow->getWindow()) ? gViewerWindow->getWindow()->getRefreshRate() : 0;
}

U32 LLVKAutoTune::effectiveTargetFPS()
{
    static LLCachedControl<U32> s_target(gSavedSettings, "TargetFPS", kTargetFpsDefault);
    static LLCachedControl<bool> s_vsync(gSavedSettings, "RenderVSyncEnable", true);
    static LLCachedControl<S32> s_limit(gSavedSettings, "RenderFPSLimit", 0);
    U32 t = clampTargetFPS((U32)s_target);
    const S32 refresh = refreshRate();
    if (s_vsync && refresh > 0) t = std::min(t, (U32)refresh);   // FIFO: frames never exceed the refresh
    if ((S32)s_limit > 0) t = std::min(t, (U32)(S32)s_limit);
    return std::max(t, 1u);
}

// ============================================================================
// GPU pass categories (timer harvest)
// ============================================================================

const char* LLVKAutoTune::catLabel(uint32_t c)
{
    switch (c)
    {
    case CAT_GEOMETRY:    return "AutoTuneCatGeometry";
    case CAT_AVATARS:     return "AutoTuneCatAvatars";
    case CAT_SHADOWS:     return "AutoTuneCatShadows";
    case CAT_REFLECTIONS: return "AutoTuneCatReflections";
    case CAT_RT:          return "AutoTuneCatRT";
    case CAT_LIGHTING:    return "AutoTuneCatLighting";
    case CAT_TRANSPARENT: return "AutoTuneCatTransparent";
    case CAT_PARTICLES:   return "AutoTuneCatParticles";
    case CAT_POST:        return "AutoTuneCatPost";
    case CAT_UPSCALE:     return "AutoTuneCatUpscale";
    case CAT_UI:          return "AutoTuneCatUI";
    default:              return "AutoTuneCatOther";
    }
}

static const char* cat_fallback(uint32_t c)
{
    static const char* k[] = {"Geometry", "Worn mesh (avatars)", "Shadows", "Reflections", "Ray tracing",
                              "Lighting + AO", "Transparency + water", "Particles", "Post effects",
                              "Upscaling + AA", "HUD + selection", "Other"};
    return c < LLVKAutoTune::CAT_COUNT ? k[c] : k[LLVKAutoTune::CAT_OTHER];
}

std::string LLVKAutoTune::catName(uint32_t c)
{
    return tr(catLabel(c), cat_fallback(c));
}

// Sub-mark name -> category (the renderer's gpuSubMark names; first match wins, so the
// specific prefixes come first). -1 = not a cost (waits, queue-span summaries, begins).
int LLVKAutoTune::catOf(const char* name)
{
    if (!name) return -1;
    static const struct { const char* p; int c; } kMap[] = {
        {"q_", -1}, {"g_fe_wait", -1}, {"begin", -1}, {"f_begin", -1}, {"a_begin", -1}, {"g_pre_fe", -1},
        // worn mesh first (rigged gbuffer / lit before the generic g_gbuf / g_lit)
        {"c_preskin", CAT_AVATARS}, {"c_skin_pal", CAT_AVATARS}, {"c_rigweld", CAT_AVATARS}, {"c_anim", CAT_AVATARS},
        {"g_gbuf_rigged", CAT_AVATARS}, {"g_lit_rigged", CAT_AVATARS},
        {"c_shadow", CAT_SHADOWS}, {"c_sun_", CAT_SHADOWS}, {"c_projshadow", CAT_SHADOWS}, {"g_sun_draw", CAT_SHADOWS},
        {"p_probe", CAT_REFLECTIONS}, {"pr_", CAT_REFLECTIONS}, {"c_prb_", CAT_REFLECTIONS}, {"c_mirror", CAT_REFLECTIONS},
        {"g_mirror", CAT_REFLECTIONS},
        {"c_blas", CAT_RT}, {"c_tlas", CAT_RT}, {"c_rtmask", CAT_RT}, {"g_rtmask", CAT_RT},
        {"g_lit", CAT_LIGHTING}, {"c_lights", CAT_LIGHTING}, {"g_ao", CAT_LIGHTING}, {"g_ssao", CAT_LIGHTING},
        {"g_gtao", CAT_LIGHTING}, {"g_rtao", CAT_LIGHTING}, {"g_term", CAT_LIGHTING}, {"g_cel", CAT_LIGHTING}, {"c_cel", CAT_LIGHTING},
        {"c_pfog", CAT_PARTICLES}, {"g_pfog", CAT_PARTICLES},
        {"c_part", CAT_PARTICLES}, {"g_p2_particles", CAT_PARTICLES}, {"g_p2_part", CAT_PARTICLES},
        {"g_p2", CAT_TRANSPARENT}, {"g_pass2", CAT_TRANSPARENT}, {"c_alpha_sort", CAT_TRANSPARENT},
        {"g_alpha_depth", CAT_TRANSPARENT},
        {"p_fsr", CAT_UPSCALE}, {"p_taa", CAT_UPSCALE}, {"p_aa", CAT_UPSCALE},
        {"p_", CAT_POST}, {"g_godrays", CAT_POST}, {"c_godrays", CAT_POST}, {"g_to_glow", CAT_POST},
        {"g_hud", CAT_UI}, {"g_sel", CAT_UI}, {"g_overlay", CAT_UI}, {"c_hud", CAT_UI},
        {"g_gbuf", CAT_GEOMETRY}, {"g_prepass", CAT_GEOMETRY}, {"g_pass1", CAT_GEOMETRY}, {"c_cull", CAT_GEOMETRY},
        {"c_hiz", CAT_GEOMETRY}, {"c_occl", CAT_GEOMETRY}, {"c_stream", CAT_GEOMETRY}, {"g_occl", CAT_GEOMETRY},
        {"c_vrs", CAT_GEOMETRY}, {"g_sky", CAT_GEOMETRY}, {"g_passes", CAT_GEOMETRY},
    };
    for (const auto& m : kMap)
        if (!std::strncmp(name, m.p, std::strlen(m.p))) return m.c;
    return CAT_OTHER;
}

// Sub-mark name -> the pass group a lever owns (-1 = none). Marks label the work recorded
// since the previous mark, so "c_mirror" is the planar mirror capture alone.
int LLVKAutoTune::grpOf(const char* name)
{
    if (!name) return -1;
    static const struct { const char* p; int g; } kMap[] = {
        {"c_mirror", GRP_MIRROR}, {"g_mirror", GRP_MIRROR},
        {"c_rtmask", GRP_RTMASK}, {"g_rtmask", GRP_RTMASK},
        {"c_blas", GRP_AS}, {"c_tlas", GRP_AS},
        {"c_sun_", GRP_SUNMAP}, {"g_sun_", GRP_SUNMAP}, {"c_shadows", GRP_SUNMAP}, {"c_shadow_rigcull", GRP_SUNMAP},
        {"c_projshadow", GRP_PROJMAP},
        {"p_probe", GRP_PROBE}, {"pr_", GRP_PROBE}, {"c_prb_", GRP_PROBE},
        {"g_ao", GRP_AO}, {"g_ssao", GRP_AO}, {"g_gtao", GRP_AO}, {"g_rtao", GRP_AO},
        {"p_dof", GRP_DOF},
        {"p_bloom", GRP_GLOW}, {"g_to_glow", GRP_GLOW},
    };
    for (const auto& m : kMap)
        if (!std::strncmp(name, m.p, std::strlen(m.p))) return m.g;
    return -1;
}

void LLVKAutoTune::gpuSubSample(const char* name, double ms)
{
    const int c = catOf(name);
    if (c < 0 || !(ms > 0.0) || ms > 1000.0) return;
    const int g = grpOf(name);
    std::lock_guard<std::mutex> lk(sCatMutex);
    sCats.frame[c] += ms;
    if (g >= 0) sCats.gframe[g] += ms;
}

void LLVKAutoTune::gpuFrameDone(double total_ms)
{
    std::lock_guard<std::mutex> lk(sCatMutex);
    double sum = 0.0;
    for (double v : sCats.frame) sum += v;
    const bool first = sCats.frames == 0;
    if (sum > 0.0)   // lean frames (RenderVKGpuSubMarks off) carry no sub-marks: totals only
    {
        for (uint32_t c = 0; c < CAT_COUNT; ++c)
        {
            sCats.sm[c] = first ? (float)sCats.frame[c] : sCats.sm[c] * 0.9f + (float)sCats.frame[c] * 0.1f;
            sCats.frame[c] = 0.0;
        }
        for (uint32_t g = 0; g < GRP_COUNT; ++g)
        {
            sCats.gsm[g] = first ? (float)sCats.gframe[g] : sCats.gsm[g] * 0.9f + (float)sCats.gframe[g] * 0.1f;
            sCats.gframe[g] = 0.0;
        }
        ++sCats.frames;
    }
    if (total_ms > 0.0 && total_ms < 1000.0)
        sCats.total = sCats.total <= 0.f ? (float)total_ms : sCats.total * 0.9f + (float)total_ms * 0.1f;
    sCats.at = wallNow();
}

void LLVKAutoTune::requestPassTimers()
{
    mTimersUntil = std::max(mTimersUntil, wallNow() + 2.0);
}

float LLVKAutoTune::catMs(uint32_t c, bool* known) const
{
    std::lock_guard<std::mutex> lk(sCatMutex);
    const bool fresh = sCats.frames > 30 && wallNow() - sCats.at < 3.0;
    if (known) *known = fresh;
    return fresh && c < CAT_COUNT ? sCats.sm[c] : 0.f;
}

float LLVKAutoTune::grpMs(uint32_t mask, bool* known) const
{
    std::lock_guard<std::mutex> lk(sCatMutex);
    const bool fresh = sCats.frames > 30 && wallNow() - sCats.at < 3.0;
    if (known) *known = fresh;
    if (!fresh) return 0.f;
    float ms = 0.f;
    for (uint32_t g = 0; g < GRP_COUNT; ++g)
        if (mask & (1u << g)) ms += sCats.gsm[g];
    return ms;
}

float LLVKAutoTune::gpuMs() const
{
    if (mSample.valid && mSample.gpuAgeSec < 3.0 && mSample.gpuFrameMs > 0.f) return mSample.gpuFrameMs;
    std::lock_guard<std::mutex> lk(sCatMutex);
    return (wallNow() - sCats.at < 3.0) ? sCats.total : 0.f;
}

// ============================================================================
// The step table (#422). Ordered by cost / visual-quality ratio (cheapest loss first),
// separately for GPU-bound and CPU-bound frames. Tune once writes the setting one ladder
// entry down; "Keep my FPS" engages the lever's cap levels (soft, never written).
// ============================================================================

namespace
{
    enum Fmt : uint8_t { F_NUM, F_BOOL, F_UPSCALER, F_SHADOW_UPDATE, F_PROBE_DETAIL, F_SHADOW_RES, F_METERS,
                         F_PERCENT, F_NONIMP, F_PROJ, F_SUN, F_FACTOR };
    enum Scope : uint8_t { SC_WORLD, SC_AVATARS };

    bool sunShadowsOn() { return gSavedSettings.getS32("RenderShadowDetail") >= 1; }
    bool projShadowsOn() { return gSavedSettings.getS32("RenderShadowDetail") >= 2 && gSavedSettings.getU32("RenderVKProjectorShadows") > 0; }
    bool rtShadowsOn()
    {
        return (sunShadowsOn() && gSavedSettings.getU32("RenderVKSunShadows") == 2)
            || (projShadowsOn() && gSavedSettings.getU32("RenderVKProjectorShadows") == 2);
    }
    bool probesOn() { return gSavedSettings.getBOOL("RenderReflectionsEnabled") && gSavedSettings.getS32("RenderReflectionProbeDetail") >= 0; }
    bool mirrorsOn() { return gSavedSettings.getBOOL("RenderMirrors"); }
    // Custom (6): the user's own scale. DLSS Ray Reconstruction (7-11, #870): fixed render sizes
    // chosen by the network and a mode the user picked on purpose -- the tuner never steps it.
    bool upscalerPreset() { const U32 m = gSavedSettings.getU32("RenderVKUpscaler"); return m != 6u && m < 7u; }
    bool always() { return true; }

    constexpr uint16_t O_MIRROR  = 1u << LLVKAutoTune::GRP_MIRROR;
    constexpr uint16_t O_RTMASK  = 1u << LLVKAutoTune::GRP_RTMASK;
    constexpr uint16_t O_SUNMAP  = 1u << LLVKAutoTune::GRP_SUNMAP;
    constexpr uint16_t O_PROJMAP = 1u << LLVKAutoTune::GRP_PROJMAP;
    constexpr uint16_t O_PROBE   = 1u << LLVKAutoTune::GRP_PROBE;
    constexpr uint16_t O_AO      = 1u << LLVKAutoTune::GRP_AO;
    constexpr uint16_t O_DOF     = 1u << LLVKAutoTune::GRP_DOF;
    constexpr uint16_t O_GLOW    = 1u << LLVKAutoTune::GRP_GLOW;
}

struct LLVKAutoTune::Lever
{
    const char* id;              // log key + strings.xml "AutoTuneLever_<id>"
    const char* label;           // English fallback
    uint8_t gpu_rank;            // order when GPU-bound (0 = not a GPU lever)
    uint8_t cpu_rank;            // order when CPU-bound (0 = not a CPU lever)
    uint8_t scope;               // Scope (TuningFPSStrategy)
    uint8_t cat;                 // pass category that must cost something (CAT_NONE = whole frame)
    uint8_t fmt;
    bool    realloc;             // re-allocates render targets (longer settle)
    bool    (*active)();         // precondition
    // ---- Tune once (control == nullptr: continuous only) ----
    const char* control;
    std::vector<double> ladder;  // from most expensive to cheapest; applies while the current value is within it
    int8_t  dir;                 // +1: a bigger value is cheaper, -1: a smaller one is
    bool    zero_is_max;         // 0 means "unlimited" (the most expensive)
    double  scale;               // > 0: relative step (value x scale) down to the floor control / floor_min
    const char* floor_control;
    double  floor_min;
    // ---- Keep my FPS (cap_levels == 0: Tune once only) ----
    uint8_t cap_levels;
    void    (*cap)(LLVKPerfCaps&, uint8_t level);   // apply levels 1..cap_levels
    bool    nonimp;              // the LL-side effective non-impostor cap instead of LLVKPerfCaps
    bool    segment = false;     // Tune once: a continuation row (only while the value is within its ladder)
    // The pass groups (1 << Grp) whose cost this lever can remove: its "measured cheap" gate
    // and expected-gain estimate. 0 = its category (cat), or unattributable (CAT_NONE).
    uint16_t owns = 0;
    // Tune once: a no-gain verdict on an earlier row of the same control does not stop this
    // row (a different trade, e.g. projector shadows off after RT -> maps did not help).
    bool    split = false;
    // Tune once: switching back re-streams or re-renders (LOD, draw distance, impostors):
    // measured one-way (A A B B ...) instead of interleaved.
    bool    oneway = false;
};

const std::vector<LLVKAutoTune::Lever>& LLVKAutoTune::levers()
{
    // GPU-bound order (gpu_rank) / CPU-bound order (cpu_rank). Tune once rows write a
    // setting; cap rows (cap_levels > 0) are what "Keep my FPS" holds back temporarily.
    static const std::vector<Lever> k = {
        // ---------------- Tune once (settings) ----------------
        {"shadow_update", "Shadow updates", 1, 0, SC_WORLD, CAT_SHADOWS, F_SHADOW_UPDATE, false, sunShadowsOn,
         "RenderVKShadowUpdate", {0, 1, 2}, +1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_SUNMAP | O_PROJMAP},
        {"probe_detail", "Reflection detail", 2, 0, SC_WORLD, CAT_REFLECTIONS, F_PROBE_DETAIL, false, probesOn,
         "RenderReflectionProbeDetail", {3, 2, 1}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_PROBE},
        {"mirrors", "Mirrors", 3, 0, SC_WORLD, CAT_REFLECTIONS, F_BOOL, true, mirrorsOn,
         "RenderMirrors", {1, 0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_MIRROR},
        {"shadow_res", "Shadow quality", 4, 0, SC_WORLD, CAT_SHADOWS, F_SHADOW_RES, true, sunShadowsOn,
         "RenderVKShadowResolution", {8192, 4096, 2048, 1024, 512}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_SUNMAP | O_PROJMAP},
        {"rt_mask", "Ray-traced shadow resolution", 5, 0, SC_WORLD, CAT_RT, F_PERCENT, false, rtShadowsOn,
         "RenderVKRTShadowMaskScale", {1.0, 0.75, 0.5}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_RTMASK},
        {"sun_rt", "Sun shadows", 6, 0, SC_WORLD, CAT_RT, F_SUN, false, sunShadowsOn,
         "RenderVKSunShadows", {2, 1}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_RTMASK},
        {"proj_rt", "Projector shadows", 7, 0, SC_WORLD, CAT_RT, F_PROJ, false, projShadowsOn,
         "RenderVKProjectorShadows", {2, 1}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_RTMASK},
        {"nonimp", "Max. fully rendered avatars", 8, 1, SC_AVATARS, CAT_AVATARS, F_NONIMP, false, always,
         "RenderAvatarMaxNonImpostors", {0, 16, 12, 8, 5, 3}, -1, true, 0.0, nullptr, 0.0, 0, nullptr, false, false, 0, false, true},
        {"ssao", "Ambient occlusion", 9, 0, SC_WORLD, CAT_LIGHTING, F_BOOL, false, always,
         "RenderDeferredSSAO", {1, 0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_AO},
        // Upscaling sits mid-order (FSR Quality / Balanced lose less than coarser LOD, fewer
        // lights or a shorter draw distance); Performance is its own lever near the end.
        {"upscale", "Upscaling (FSR)", 10, 0, SC_WORLD, CAT_NONE, F_UPSCALER, true, upscalerPreset,
         "RenderVKUpscaler", {0, 2, 3}, +1, false, 0.0, nullptr, 0.0, 0, nullptr, false},
        {"local_lights", "Local lights", 11, 0, SC_WORLD, CAT_LIGHTING, F_NUM, false, always,
         "RenderLocalLightCount", {256, 128, 64}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false},
        {"avatar_lod", "Avatar mesh detail", 12, 4, SC_AVATARS, CAT_AVATARS, F_FACTOR, false, always,
         "RenderAvatarLODFactor", {1.0, 0.75, 0.5, 0.35}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, 0, false, true},
        {"volume_lod", "Object detail", 13, 3, SC_WORLD, CAT_GEOMETRY, F_FACTOR, false, always,
         "RenderVolumeLODFactor", {4.0, 3.0, 2.0, 1.5, 1.25, 1.0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, 0, false, true},
        {"far_clip", "Draw distance", 14, 2, SC_WORLD, CAT_NONE, F_METERS, false, always,
         "RenderFarClip", {}, -1, false, 0.75, "AutoTuneRenderFarClipMin", 64.0, 0, nullptr, false, false, 0, false, true},
        {"dof", "Depth of field", 15, 0, SC_WORLD, CAT_POST, F_BOOL, false, always,
         "RenderDepthOfField", {1, 0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_DOF},
        {"proj_off", "Projector shadows", 16, 0, SC_WORLD, CAT_SHADOWS, F_PROJ, false, projShadowsOn,
         "RenderVKProjectorShadows", {1, 0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, true, O_PROJMAP | O_RTMASK, true},
        {"particles", "Max. particles", 17, 0, SC_WORLD, CAT_PARTICLES, F_NUM, false, always,
         "RenderMaxPartCount", {8192, 4096, 2048, 1024}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false},
        {"probe_res", "Reflection resolution", 18, 0, SC_WORLD, CAT_REFLECTIONS, F_NUM, true, probesOn,
         "RenderReflectionProbeResolution", {1024, 512, 256, 128}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_PROBE},
        {"glow", "Glow", 19, 0, SC_WORLD, CAT_POST, F_BOOL, false, always,
         "RenderGlow", {1, 0}, -1, false, 0.0, nullptr, 0.0, 0, nullptr, false, false, O_GLOW},
        {"upscale_p", "Upscaling (FSR)", 20, 0, SC_WORLD, CAT_NONE, F_UPSCALER, true, upscalerPreset,
         "RenderVKUpscaler", {3, 4}, +1, false, 0.0, nullptr, 0.0, 0, nullptr, false, true, 0, true},

        // ---------------- Keep my FPS (temporary caps) ----------------
        {"cap_probe_res", "Reflection resolution", 1, 0, SC_WORLD, CAT_REFLECTIONS, F_NUM, true, probesOn,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 1, [](LLVKPerfCaps& c, uint8_t) { c.probeResShift = 1; }, false, false, O_PROBE},
        {"cap_mirrors", "Mirrors", 2, 0, SC_WORLD, CAT_REFLECTIONS, F_BOOL, true, mirrorsOn,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 1, [](LLVKPerfCaps& c, uint8_t) { c.mirrorsOff = true; }, false, false, O_MIRROR},
        {"cap_far_shadows", "Far shadows", 3, 0, SC_WORLD, CAT_SHADOWS, F_BOOL, false, sunShadowsOn,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 1, [](LLVKPerfCaps& c, uint8_t) { c.farShadowsOff = true; }, false, false, O_SUNMAP | O_RTMASK},
        {"cap_scale_a", "Render resolution", 4, 0, SC_WORLD, CAT_NONE, F_PERCENT, true, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 2,
         [](LLVKPerfCaps& c, uint8_t l) { c.renderScaleMax = std::min(c.renderScaleMax, l >= 2 ? 0.75f : 0.85f); }, false},
        {"cap_proj_shadows", "Projector shadows", 5, 0, SC_WORLD, CAT_SHADOWS, F_BOOL, false, projShadowsOn,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 1, [](LLVKPerfCaps& c, uint8_t) { c.projShadowsOff = true; }, false, false, O_PROJMAP | O_RTMASK},
        {"cap_nonimp", "Fully rendered avatars", 6, 1, SC_AVATARS, CAT_AVATARS, F_NONIMP, false, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 4, nullptr, true},
        {"cap_mesh_lod", "Object detail", 7, 3, SC_WORLD, CAT_GEOMETRY, F_PERCENT, false, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 2,
         [](LLVKPerfCaps& c, uint8_t l) { c.meshLodScale = std::min(c.meshLodScale, l >= 2 ? 0.6f : 0.8f); }, false},
        {"cap_scale_b", "Render resolution", 8, 0, SC_WORLD, CAT_NONE, F_PERCENT, true, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 3,
         [](LLVKPerfCaps& c, uint8_t l) { c.renderScaleMax = std::min(c.renderScaleMax, l >= 3 ? 0.5f : (l >= 2 ? 0.59f : 0.67f)); }, false},
        {"cap_probe_count", "Reflection probes", 9, 0, SC_WORLD, CAT_REFLECTIONS, F_NUM, true, probesOn,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 1, [](LLVKPerfCaps& c, uint8_t) { c.probeCountShift = 1; }, false, false, O_PROBE},
        {"cap_avatar_lod", "Avatar mesh detail", 10, 4, SC_AVATARS, CAT_AVATARS, F_PERCENT, false, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 2,
         [](LLVKPerfCaps& c, uint8_t l) { c.avatarLodScale = std::min(c.avatarLodScale, l >= 2 ? 0.5f : 0.75f); }, false},
        {"cap_draw_dist", "Draw distance", 11, 2, SC_WORLD, CAT_NONE, F_PERCENT, false, always,
         nullptr, {}, 0, false, 0.0, nullptr, 0.0, 2,
         [](LLVKPerfCaps& c, uint8_t l) { c.drawDistScale = std::min(c.drawDistScale, l >= 2 ? 0.5f : 0.75f); }, false},
    };
    return k;
}

bool LLVKAutoTune::leverAllowed(const Lever& l) const
{
    // TuningFPSStrategy ("Settings affect"): 0 avatars only, 1 avatars and world, 2 world only.
    static LLCachedControl<U32> s_strategy(gSavedSettings, "TuningFPSStrategy", 1);
    const U32 st = s_strategy;
    if (l.scope == SC_AVATARS && st == 2u) return false;
    if (l.scope == SC_WORLD && st == 0u) return false;
    return !l.active || l.active();
}

float LLVKAutoTune::leverMs(const Lever& l, bool* known) const
{
    if (l.owns) return grpMs(l.owns, known);
    if (l.cat != CAT_NONE) return catMs(l.cat, known);
    if (known) *known = false;   // whole-frame lever (render scale, draw distance): not attributable
    return 0.f;
}

// Measured cheap: the passes this lever can remove cost next to nothing in this scene
// (e.g. mirrors with no mirror in view: the mirror passes, not every reflection pass).
bool LLVKAutoTune::measuredCheap(const Lever& l, const Window& w) const
{
    bool known = false;
    const float ms = leverMs(l, &known);
    return known && ms < std::max(0.15f, 0.02f * w.p50);
}

// An experiment's verdict. gain = the average of the mean and p75 improvements (ms); it must
// clear the largest of an absolute floor, a share of the baseline and the noise -- twice the
// standard error of the difference of the two window means (each from ~0.5 s chunk means,
// so frame-to-frame correlation does not shrink it).
int LLVKAutoTune::judge(const Window& b, const Window& a, float& gain, float& thr)
{
    gain = 0.5f * ((b.mean - a.mean) + (b.p75 - a.p75));
    const float noise = kNoiseSigmas * std::sqrt(b.se * b.se + a.se * a.se);
    thr = std::max(std::max(kGainAbsMs, kGainRel * b.p75), noise);
    if (gain >= thr) return 1;
    if (gain <= -thr) return -1;
    return 0;
}

// ---- per-machine lever history (AutoTuneLeverHistory: "<GPU + driver>|id=n;id=n") ----
// n = how often the lever did not help on this hardware (a win counts it down). A prior
// only: it pushes the lever back in the order, never forbids it (scenes differ).
void LLVKAutoTune::loadHistory()
{
    if (mHistLoaded) return;
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (!vk.isInitialized()) return;
    std::string name, vendor, api;
    bool nv = false, amd = false, intel = false;
    uint32_t vram_mb = 0;
    vk.getDeviceIdentity(name, vendor, api, nv, amd, intel, vram_mb);
    std::string key = name + " " + vk.driverVersionText();
    for (char& ch : key)
        if (ch == '|' || ch == ';' || ch == '=' || ch == '\n') ch = ' ';
    mHistKey = key;
    mHistLoaded = true;
    mHist.clear();
    const std::string s = gSavedSettings.getString("AutoTuneLeverHistory");
    const size_t bar = s.find('|');
    if (bar == std::string::npos || s.compare(0, bar, key) != 0) return;   // another GPU / driver: start over
    std::istringstream is(s.substr(bar + 1));
    std::string item;
    while (std::getline(is, item, ';') && mHist.size() < 64)
    {
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const int n = std::atoi(item.c_str() + eq + 1);
        if (n > 0) mHist[item.substr(0, eq)] = std::min(n, kHistMax);
    }
}

int LLVKAutoTune::histCount(const char* id)
{
    loadHistory();
    const auto it = mHist.find(id);
    return it != mHist.end() ? it->second : 0;
}

void LLVKAutoTune::noteVerdict(const char* id, bool helped)
{
    loadHistory();
    if (!mHistLoaded || !id) return;
    int& n = mHist[id];
    n = helped ? std::max(0, n - 1) : std::min(kHistMax, n + 1);
    if (!n) mHist.erase(id);
    std::ostringstream os;
    os << mHistKey << '|';
    bool first = true;
    for (const auto& kv : mHist)
    {
        if (!first) os << ';';
        first = false;
        os << kv.first << '=' << kv.second;
    }
    gSavedSettings.setString("AutoTuneLeverHistory", os.str());
}

static std::string fmt_value(uint8_t fmt, double v)
{
    switch (fmt)
    {
    case F_BOOL: return v != 0.0 ? tr("AutoTuneOn", "On") : tr("AutoTuneOff", "Off");
    case F_UPSCALER:
    {
        static const char* k[] = {"AutoTuneUpscaleOff", "AutoTuneUpscaleNative", "AutoTuneUpscaleQuality",
                                  "AutoTuneUpscaleBalanced", "AutoTuneUpscalePerformance", "AutoTuneUpscaleUltra"};
        static const char* f[] = {"Off (native)", "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance"};
        const int i = llclamp((int)v, 0, 5);
        return tr(k[i], f[i]);
    }
    case F_SHADOW_UPDATE:
    {
        static const char* k[] = {"AutoTuneShadowRealtime", "AutoTuneShadowBalanced", "AutoTuneShadowPerformance"};
        static const char* f[] = {"Realtime", "Balanced", "Performance"};
        const int i = llclamp((int)v, 0, 2);
        return tr(k[i], f[i]);
    }
    case F_PROBE_DETAIL:
    {
        static const char* k[] = {"AutoTuneProbeOff", "AutoTuneProbeStatic", "AutoTuneProbeDynamic",
                                  "AutoTuneProbeRealtime", "AutoTuneProbeRT"};
        static const char* f[] = {"Off", "Static only", "Static + dynamic", "Realtime", "Ray traced"};
        const int i = llclamp((int)v + 1, 0, 4);
        return tr(k[i], f[i]);
    }
    case F_SHADOW_RES: return v <= 256 ? tr("AutoTuneLow", "Low") : v <= 512 ? tr("AutoTuneMedium", "Medium")
                            : v <= 1024 ? tr("AutoTuneHigh", "High") : v <= 2048 ? tr("AutoTuneUltra", "Ultra")
                            : llformat("%d", (int)v);
    case F_METERS: return llformat("%d m", (int)llround((F32)v));
    case F_PERCENT: return llformat("%d%%", (int)llround((F32)(v * 100.0)));
    case F_NONIMP: return v <= 0.0 ? tr("AutoTuneUnlimited", "unlimited") : llformat("%d", (int)v);
    case F_PROJ:
    {
        static const char* k[] = {"AutoTuneOff", "AutoTuneShadowMaps", "AutoTuneRayTraced"};
        static const char* f[] = {"Off", "Shadow maps", "Ray traced"};
        const int i = llclamp((int)v, 0, 2);
        return tr(k[i], f[i]);
    }
    case F_SUN:
    {
        static const char* k[] = {"AutoTuneOff", "AutoTuneShadowMaps", "AutoTuneRayTraced"};
        static const char* f[] = {"Off", "Shadow maps", "Ray traced"};
        const int i = llclamp((int)v, 0, 2);
        return tr(k[i], f[i]);
    }
    case F_FACTOR: return llformat("%.2f", v);
    default: return llformat("%d", (int)llround((F32)v));
    }
}

static std::string lever_label(const char* id, const char* fallback)
{
    return tr((std::string("AutoTuneLever_") + id).c_str(), fallback);
}

// The control's value as its type wants it.
static LLSD control_value(LLControlVariable* ctrl, double v)
{
    switch (ctrl ? ctrl->type() : TYPE_S32)
    {
    case TYPE_BOOLEAN: return LLSD(v != 0.0);
    case TYPE_F32: return LLSD((F64)v);
    default: return LLSD((S32)llround((F32)v));
    }
}

bool LLVKAutoTune::stagesOf(const Lever& l, std::vector<double>& stages, double& cur) const
{
    stages.clear();
    if (!l.control) return false;
    LLControlVariable* ctrl = gSavedSettings.getControl(l.control);
    if (!ctrl) return false;
    cur = ctrl->getValue().asReal();
    if (l.scale > 0.0)
    {
        // A scalar (draw distance): x scale per stage, at most 4 stages (~1/3 of the
        // current value), never below the floor.
        double floor_v = l.floor_min;
        if (l.floor_control) floor_v = std::max(floor_v, (double)gSavedSettings.getF32(l.floor_control));
        double v = cur;
        for (int i = 0; i < 4 && v > floor_v + 1.0; ++i)
        {
            v = std::max(floor_v, std::floor(v * l.scale));
            stages.push_back(v);
        }
        return !stages.empty();
    }
    if (l.ladder.size() < 2) return false;
    // key: larger = cheaper
    auto key = [&](double v) {
        if (l.zero_is_max && v <= 0.0) return -1.0e12;   // unlimited: the most expensive
        return l.dir > 0 ? v : -v;
    };
    const double kc = key(cur);
    // A continuation row (a setting's later stages, ranked separately) applies only while
    // the value lies within its ladder; a first row also takes values above its ladder
    // (e.g. 8192 local lights -> 256).
    if (l.segment && kc < key(l.ladder.front()) - 1e-6) return false;
    for (double e : l.ladder)
        if (key(e) > kc + 1e-6) stages.push_back(e);
    return !stages.empty();
}

// ============================================================================
// Measurement
// ============================================================================

bool LLVKAutoTune::sampleBlocked(std::string* why) const
{
    auto block = [&](const char* r) { if (why) *why = r; return true; };
    if (LLStartUp::getStartupState() < STATE_STARTED) return block("login");
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) return block("teleport");
    if (!gFocusMgr.getAppHasFocus()) return block("background");
    if (gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getWindow()->getMinimized()) return block("minimized");
    return false;
}

void LLVKAutoTune::resetWindow()
{
    mFrames.clear();
    mWinStart = wallNow();
}

LLVKAutoTune::Window LLVKAutoTune::measure(uint32_t min_frames) const
{
    return windowOf(mFrames, min_frames);
}

LLVKAutoTune::Window LLVKAutoTune::windowOf(const std::vector<FrameRec>& frames, uint32_t min_frames) const
{
    Window w;
    w.frames = (uint32_t)frames.size();
    if (w.frames < std::max(min_frames, 3u)) return w;
    std::vector<float> f, c;
    f.reserve(frames.size());
    c.reserve(frames.size());
    for (const FrameRec& r : frames) { f.push_back(r.frame_ms); c.push_back(r.cpu_ms); }
    w.p50 = LLVKHitch::percentile(f, 0.50);
    w.p75 = LLVKHitch::percentile(f, 0.75);
    w.p90 = LLVKHitch::percentile(f, 0.90);
    w.cpu50 = LLVKHitch::percentile(c, 0.50);
    // Mean + its standard error from ~0.5 s chunk means (consecutive frames correlate: the
    // per-frame spread would understate the noise of a camera pan or a streaming burst).
    double sum = 0.0;
    for (float v : f) sum += v;
    w.mean = (float)(sum / (double)f.size());
    std::vector<double> chunks;
    double acc = 0.0;
    uint32_t n = 0;
    for (float v : f)
    {
        acc += v;
        ++n;
        if (acc >= kChunkMs) { chunks.push_back(acc / n); acc = 0.0; n = 0; }
    }
    if (n >= 5) chunks.push_back(acc / n);
    double var = 0.0;
    size_t k = chunks.size();
    if (k >= 3)
    {
        double m = 0.0;
        for (double x : chunks) m += x;
        m /= (double)k;
        for (double x : chunks) var += (x - m) * (x - m);
        var /= (double)(k - 1);
    }
    else
    {
        // Too few chunks (very slow frames): the per-frame spread, as if 3 chunks.
        for (float v : f) var += ((double)v - w.mean) * ((double)v - w.mean);
        var /= (double)std::max<size_t>(f.size() - 1, 1);
        k = 3;
    }
    w.se = (float)std::sqrt(var / (double)k);
    w.gpu = gpuMs();
    w.wait_share = w.p50 > 0.f ? std::max(0.f, 1.f - w.cpu50 / w.p50) : 0.f;
    // GPU-bound: the GPU span fills the frame (the rigged controller's definition), or
    // the main thread spends a quarter of the frame waiting on the GPU / present.
    w.gpu_bound = (w.gpu > 0.f && w.gpu >= 0.85f * w.p50) || w.wait_share >= 0.25f;
    w.ok = true;
    return w;
}

void LLVKAutoTune::pollSample()
{
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (!vk.isInitialized()) { mSample.valid = false; return; }
    vk.autoTuneSample(mSample);
}

// #422: LLVOAvatar::mGPURenderTime was only ever written by the GL renderer's per-avatar
// timer queries, so under Vulkan every avatar read 0 -- the Nearby list, the
// "Maximum render time" limit (RenderAvatarMaxART -> isTooSlow) and the cost totals were
// dead. The GPU-driven renderer has no per-avatar timer; the estimate is the measured GPU
// time of all rigged work (pre-skin, weld, rigged raster) shared by each avatar's visible
// skinned vertices. Updated only while the avatar is drawn (vertices > 0): an avatar off
// screen or hidden by the limit keeps its last estimate (no hide / show feedback loop).
void LLVKAutoTune::updateAvatarCosts()
{
    if (!mSample.valid || mSample.gpuAgeSec > 3.0 || mSample.rigGpuMs <= 0.f) return;
    uint64_t total = 0;
    for (const auto& kv : mSample.avVerts) total += kv.second;
    if (total < 1000) return;
    const double per_vert = (double)mSample.rigGpuMs / (double)total;
    for (const auto& kv : mSample.avVerts)
    {
        if (!kv.second) continue;
        LLViewerObject* o = gObjectList.findObject(kv.first);
        LLVOAvatar* av = o ? o->asAvatar() : nullptr;
        if (!av || av->isDead()) continue;
        const F32 est = (F32)(per_vert * (double)kv.second);
        const F32 prev = av->getEstimatedGPURenderTime();
        av->setEstimatedGPURenderTime(prev > 0.f ? prev * 0.6f + est * 0.4f : est);
    }
}

// Effective "max fully rendered avatars" (LLVOAvatar::sMaxNonImpostors): the user's
// RenderAvatarMaxNonImpostors, lowered -- never written -- by "Keep my FPS" (cap_nonimp)
// and by "Simplify avatars beyond N m" (AutoTuneImpostorByDistEnabled: only the avatars
// within that distance of the camera stay fully rendered). Restored once neither applies.
void LLVKAutoTune::applyLLSideCaps()
{
    static LLCachedControl<U32> s_setting(gSavedSettings, "RenderAvatarMaxNonImpostors", 12);
    static LLCachedControl<bool> s_dist_on(gSavedSettings, "AutoTuneImpostorByDistEnabled", false);
    static LLCachedControl<F32> s_dist(gSavedSettings, "AutoTuneImpostorFarAwayDistance", 64.f);
    static LLCachedControl<S32> s_mode(gSavedSettings, "RenderAvatarComplexityMode", 0);
    static LLCachedControl<F32> s_far(gSavedSettings, "RenderFarClip", 128.f);
    const U32 user = (U32)s_setting >= LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER ? 0u : (U32)s_setting;   // 0 = unlimited
    U32 cap = 0;
    auto take = [&](U32 c) { if (c && (!cap || c < cap)) cap = c; };
    take(mNonImpCapTuner);
    if (s_dist_on && (S32)s_mode != LLVOAvatar::AV_RENDER_ONLY_SHOW_FRIENDS && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        std::vector<LLVector3d> positions;
        LLWorld::getInstance()->getAvatars(nullptr, &positions, gAgentCamera.getCameraPositionGlobal(),
                                           std::min((F32)s_far, (F32)s_dist));
        const U32 n = (U32)positions.size();
        if (n < LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER) take(std::max(n, 1u));
    }
    U32 eff = user;
    if (cap && (!eff || cap < eff)) eff = cap;
    if (!cap)
    {
        if (mNonImpCapApplied)   // a cap ended: back to the user's setting
        {
            LLVOAvatar::updateImpostorRendering((U32)s_setting);
            mNonImpCapApplied = 0;
        }
        return;
    }
    const U32 cur = LLVOAvatar::sMaxNonImpostors;   // 0 = unlimited
    if (cur != eff || mNonImpCapApplied != eff)
    {
        LLVOAvatar::updateImpostorRendering(eff ? eff : LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER);
        mNonImpCapApplied = eff;
    }
}

void LLVKAutoTune::logDecision(const std::string& line, bool force)
{
    const double now = wallNow();
    if (!force && (line == mLastLog || now < mNextLog)) return;
    mLastLog = line;
    mNextLog = now + 1.0;
    LL_INFOS("AutoTune") << line << LL_ENDL;
}

// ============================================================================
// Per frame
// ============================================================================

void LLVKAutoTune::frameTick()
{
    const double now = wallNow();
    const double dt_ms = mLastFrameAt > 0.0 ? (now - mLastFrameAt) * 1000.0 : 0.0;
    mLastFrameAt = now;

    // The frame that just ended: wall time, and the main thread's share of it -- minus the
    // waits on the GPU / present (vk_acquireWait: fence + swapchain image; vk_paceWait: the
    // same fence waited at the frame top under RenderVKFramePacing; the async-slot and
    // texture-staging waits) and the RenderFPSLimit sleep. Read before
    // LLVKHitch::frameBoundary clears the slots.
    double waits = 0.0;
    for (const LLVKHitch::Slot& sl : LLVKHitch::state().slots)
    {
        if (!sl.name) break;
        if (!std::strcmp(sl.name, "vk_acquireWait") || !std::strcmp(sl.name, "vk_paceWait")
            || !std::strcmp(sl.name, "vk_asyncSlotWait")
            || !std::strcmp(sl.name, "vk_texStageWait") || !std::strcmp(sl.name, "fpsLimitSleep"))
            waits += sl.ms;
    }
    const bool frame_ok = dt_ms > 0.0 && dt_ms < 500.0;
    const float cpu_ms = frame_ok ? (float)std::max(0.0, dt_ms - waits) : 0.f;
    if (frame_ok)
    {
        mFrameMsSm = mFrameMsSm <= 0.f ? (float)dt_ms : mFrameMsSm * 0.95f + (float)dt_ms * 0.05f;
        mCpuMsSm = mCpuMsSm <= 0.f ? cpu_ms : mCpuMsSm * 0.95f + cpu_ms * 0.05f;
        mWinFrameMs += dt_ms;
        mWinCpuMs += cpu_ms;
        ++mWinN;
    }

    // Per-pass timers: on while the window shows the breakdown or a tune measures.
    sWantTimers.store(now < mTimersUntil || mOnce != ONCE_IDLE, std::memory_order_relaxed);
    sRigTargetFps.store(mContinuous && mOnce == ONCE_IDLE ? (float)effectiveTargetFPS() : 0.f, std::memory_order_relaxed);

    if (now - mSampleAt >= 0.25)
    {
        mSampleAt = now;
        pollSample();
        // Re-assert the caps (a no-op when unchanged; a re-created world renderer starts uncapped).
        if (mCapsPushed != LLVKPerfCaps() && LLVulkanContext::instance().isInitialized())
            LLVulkanContext::instance().setAutoTunePerfCaps(mCapsPushed);
        if (now >= mNextAvCost)
        {
            mNextAvCost = now + 1.0;
            updateAvatarCosts();
        }
        applyLLSideCaps();
    }

    // Blocks: login, teleport, background / minimized, a region change. The window
    // restarts after a short settle (the first frames after a block are not this scene).
    std::string why;
    void* region = (void*)gAgent.getRegion();
    if (region != mLastRegion)
    {
        mLastRegion = region;
        mBlockedUntil = std::max(mBlockedUntil, now + 3.0);
        why = "region change";
        // A new scene: the Keep my FPS "did not help" verdicts were about the old one.
        std::fill(mCapBlockUntil.begin(), mCapBlockUntil.end(), 0.0);
    }
    if (sampleBlocked(&why) || now < mBlockedUntil)
    {
        if (!why.empty()) mBlockReason = why;
        if (now >= mBlockedUntil) mBlockedUntil = now + 2.0;
        resetWindow();
        if (mOnce == ONCE_MEASURE) { mOnce = ONCE_SETTLE; mPhaseUntil = now + kSettleSec; }
        if (mOnce != ONCE_IDLE)
        {
            // The level is not this scene any more: the test in flight is restored without a
            // verdict, and a fresh baseline is measured afterwards.
            if (mAb.on) abAbandon(mBlockReason.empty() ? "blocked" : mBlockReason.c_str());
            mBaseOk = false;
        }
        mCapTrial.on = false;   // Keep my FPS: no verdict across a block (the cap stays)
        return;
    }
    mBlockReason.clear();
    if (frame_ok && mFrames.size() < 4096) mFrames.push_back({(float)dt_ms, cpu_ms});

    const float target_ms = 1000.f / (float)effectiveTargetFPS();

    // ---------------- Tune once ----------------
    if (mOnce != ONCE_IDLE)
    {
        const double seg = mAb.on ? kSegSec : kWindowSec;   // an A/B segment, or a baseline window
        if (mOnce == ONCE_SETTLE)
        {
            if (now >= mPhaseUntil) { mOnce = ONCE_MEASURE; mPhaseUntil = now + seg; resetWindow(); }
            return;
        }
        if (now < mPhaseUntil) return;
        const Window w = measure(mAb.on ? kSegMinFrames : kMinFrames);
        const bool timed_out = now - mOnceStart > kMaxOnceSec || mOnceSteps >= kMaxOnceSteps || mOnceTests >= kMaxOnceTests;
        if (!w.ok)
        {
            resetWindow();
            mPhaseUntil = now + seg;
            if (!timed_out) return;
        }
        const std::string end = onceAdvance(w, timed_out, target_ms);
        if (end.empty()) return;   // the next window is scheduled
        // ---- done ----
        mOnce = ONCE_IDLE;
        const float end_ms = mBaseOk ? mBase.p75 : (w.ok ? w.p75 : mFrameMsSm);
        mSummary = onceSummary(end, end_ms);
        LL_INFOS("AutoTune") << "Tune once finished (" << end << " after " << llformat("%.0f", now - mOnceStart) << " s, "
                             << mUndo.size() << " change(s) kept, " << mTried.size() << " did not help, " << mRestored.size()
                             << " given back, " << mOnceTests << " A/B test(s)): " << mSummary << LL_ENDL;
        mCapChangedAt = now;
        resetWindow();
        return;
    }

    // ---------------- Keep my FPS ----------------
    if (!mContinuous)
    {
        if (!mCapOrder.empty() || mNonImpCapTuner)
        {
            mCapOrder.clear();
            std::fill(mCapLevel.begin(), mCapLevel.end(), 0);
            mNonImpCapTuner = 0;
            mCapTrial.on = false;
            pushCaps();
            logDecision("Keep my FPS off: every temporary cap released", true);
        }
        if (now - mWinStart > 10.0) resetWindow();   // keep the window bounded while idle
        return;
    }
    if (now - mWinStart < kWindowSec) return;
    const double win_start = mWinStart;
    const Window w = measure();
    resetWindow();
    if (!w.ok) return;
    // The cap engaged last is an experiment, judged on the first window measured wholly
    // after its settle: no gain beyond noise -> released, and that lever is left alone for
    // a while (until kCapBlockSec, a region change, or the busy time moving notably).
    if (mCapTrial.on)
    {
        if (win_start < mCapTrial.measure_from) return;
        mCapTrial.on = false;
        const std::vector<Lever>& L = levers();
        const size_t i = mCapTrial.lever;
        float gain = 0.f, thr = 0.f;
        judge(mCapTrial.before, w, gain, thr);
        // Released when the measured gain is below the worthwhile floor. The noise term only
        // informs the log: in the field it (~0.8 ms on two 3 s windows) exceeded what most
        // single caps save, so gating on it released caps that did help.
        const float floor_ms = std::max(kGainAbsMs, kGainRel * mCapTrial.before.p75);
        const int v = gain >= floor_ms ? 1 : (gain <= -thr ? -1 : 0);
        if (i < L.size()) noteVerdict(L[i].id, v > 0);
        const std::string line = llformat("Keep my FPS: holding back %s: p75 %.1f -> %.1f ms, mean %.1f -> %.1f ms, gain %.2f ms "
                                          "(floor %.2f ms; noise bar %.2f ms, se %.2f / %.2f)", i < L.size() ? L[i].id : "?",
                                          mCapTrial.before.p75, w.p75, mCapTrial.before.mean, w.mean, gain, floor_ms, thr,
                                          mCapTrial.before.se, w.se);
        if (v <= 0 && !mCapOrder.empty() && mCapOrder.back().first == i && i < mCapLevel.size())
        {
            mCapLevel[i] = mCapOrder.back().second;
            mCapOrder.pop_back();
            mCapChangedAt = now;
            pushCaps();
            if (mCapBlockUntil.size() != L.size()) { mCapBlockUntil.assign(L.size(), 0.0); mCapBlockBusy.assign(L.size(), 0.f); }
            mCapBlockUntil[i] = now + kCapBlockSec;
            mCapBlockBusy[i] = std::max(w.cpu50, w.gpu);
            logDecision(line + (v < 0 ? ": slower" : ": no gain") + llformat(" -> released, left alone for %.0f s", kCapBlockSec), true);
            return;
        }
        logDecision(line + (v > 0 ? ": kept" : ": no gain, but no longer the last step"), true);
    }
    const float busy = std::max(w.cpu50, w.gpu);
    const bool slow = w.p75 > target_ms * kSlowFactor;
    const bool comfy = !slow && busy > 0.f && busy < target_ms * kComfyFactor && w.p90 < target_ms * 1.05f;
    if (slow)
    {
        mComfySince = 0.0;
        if (mSlowSince <= 0.0) mSlowSince = now;
        // The rigged controller (handed the same target) acts first on a GPU-bound frame
        // whose worn mesh is a large share: give it time before stepping anything else.
        double dwell = kSettleSec;
        const bool rig_busy = w.gpu_bound && mSample.valid && mSample.rigGpuMs > 0.3f * target_ms
                           && (mSample.rigSlod < mSample.rigSlodMax || mSample.rigPerfP < mSample.rigPerfPMax);
        if (rig_busy) dwell += kRigDwellSec;
        if (now - mCapChangedAt < dwell || now - mSlowSince < kWindowSec) return;
        if (capStep(w.gpu_bound, w))
        {
            if (now - mLastReleaseAt < 60.0) mReleaseDwell = std::min(160.0, mReleaseDwell * 2.0);   // flip-flop backoff
            mSlowSince = 0.0;
        }
        return;
    }
    mSlowSince = 0.0;
    if (!comfy) { mComfySince = 0.0; return; }
    if (mComfySince <= 0.0) mComfySince = now;
    if (now - mLastReleaseAt > 300.0) mReleaseDwell = 10.0;   // five quiet minutes reset the backoff
    // Restore only with VRAM to spare (the VRAM ladder would take it straight back).
    if (mSample.valid && mSample.vramMode != 0u) return;
    if (now - mComfySince < mReleaseDwell || now - mCapChangedAt < mReleaseDwell) return;
    if (capRelease()) { mLastReleaseAt = now; mComfySince = now; }
}

// ============================================================================
// Tune once: paired A/B tests (#422 round 2)
// ============================================================================
//
// Per lever: PROBE its cheapest allowed value first (the most it can give). Below the
// measurable / worthwhile bar -> the lever is done ("even X saves only Y ms"; a small
// positive gain joins the BUNDLE pool); above it -> BISECT its stages for the mildest one
// that keeps kBisectShare of that gain (or reaches the target), and keep that. A lever kept
// at a milder stage is DEFERRED: its deeper stages come back only after every other lever
// (no one lever, e.g. the upscaler, takes all the steps). Small positive levers are then
// tried together (BUNDLE), and a winning bundle is SPLIT once to see whether its stronger
// half alone carries it. On reaching the target with headroom, CONFIRM gives kept steps
// back while the target holds.

namespace
{
    // One-sided 90% Student t (dof 1..10, then ~1.36): the bound a gain must clear.
    float tcrit90(uint32_t dof)
    {
        static const float k[] = {3.078f, 1.886f, 1.638f, 1.533f, 1.476f, 1.440f, 1.415f, 1.397f, 1.383f, 1.372f};
        return dof >= 1 && dof <= 10 ? k[dof - 1] : (dof == 0 ? k[0] : 1.36f);
    }

    const char* ab_kind_name(uint8_t k)
    {
        static const char* n[] = {"probe", "bisect", "bundle", "split", "confirm"};
        return k < 5 ? n[k] : "?";
    }
}

float LLVKAutoTune::onceBar() const
{
    const float level = mBaseOk ? 0.5f * (mBase.mean + mBase.p75) : 0.f;
    return std::max(kBarAbsMs, kBarRel * level);
}

void LLVKAutoTune::applyValues(const Values& v, double settle)
{
    bool changed = false;
    for (const auto& cv : v)
    {
        LLControlVariable* ctrl = gSavedSettings.getControl(cv.first);
        if (!ctrl) continue;
        if (std::fabs(ctrl->getValue().asReal() - cv.second.asReal()) < 1e-9) continue;
        ctrl->setValue(cv.second);
        changed = true;
    }
    if (changed) mSettleUntil = std::max(mSettleUntil, wallNow() + settle);
}

void LLVKAutoTune::abSettleTo(bool b)
{
    if (b == mAb.inB) return;
    applyValues(b ? mAb.b : mAb.a, mAb.settle);
    mAb.inB = b;
}

void LLVKAutoTune::abBegin()
{
    mAb.on = true;
    mAb.seg = 0;
    mAb.inB = false;
    ++mOnceTests;
    LL_INFOS("AutoTune") << "Tune once test " << mOnceTests << " (" << ab_kind_name(mAb.kind) << ", "
                         << (mAb.oneway ? "one-way" : "paired A/B") << "): " << mAb.label << " " << mAb.from << " -> " << mAb.to
                         << ", decides on " << mAb.thr << " ms (level " << (mBaseOk ? mBase.p75 : 0.f) << " ms p75), "
                         << mAb.minPairs << ".." << mAb.maxPairs << " pairs" << LL_ENDL;
    mOnce = ONCE_SETTLE;
    mPhaseUntil = std::max(wallNow(), mSettleUntil);
}

void LLVKAutoTune::abAbandon(const char* why)
{
    if (!mAb.on) return;
    abSettleTo(false);
    mAb.on = false;
    LL_INFOS("AutoTune") << "Tune once test " << mOnceTests << " (" << ab_kind_name(mAb.kind) << " " << mAb.label
                         << ") abandoned (" << (why ? why : "?") << "): back to A, no verdict" << LL_ENDL;
    for (size_t i : mAb.levers)
    {
        if (i >= mLeverState.size()) continue;
        if (mAb.kind == AB_BUNDLE || mAb.kind == AB_SPLIT) mLeverState[i] = LS_POOL;
        else if (mAb.kind != AB_CONFIRM) mLeverState[i] = LS_UNTRIED;
    }
    mBisect.on = false;
    if (mOnce != ONCE_IDLE)
    {
        mOnce = ONCE_SETTLE;
        mPhaseUntil = std::max(wallNow() + kSettleSec, mSettleUntil);
    }
}

LLVKAutoTune::Window LLVKAutoTune::abWindow(bool b) const
{
    Window w = windowOf(b ? mAb.fb : mAb.fa, kSegMinFrames);
    const uint32_t ng = b ? mAb.ngB : mAb.ngA;
    if (w.ok && ng)
    {
        w.gpu = (float)((b ? mAb.gpuB : mAb.gpuA) / (double)ng);
        w.gpu_bound = (w.gpu > 0.f && w.gpu >= 0.85f * w.p50) || w.wait_share >= 0.25f;
    }
    return w;
}

// The gain (A minus B, ms of 0.5 x (mean + p75) per segment) and its standard error:
// paired differences (A B B A ...: the scene's drift cancels within a pair), or a
// two-sample comparison for a one-way test.
void LLVKAutoTune::abStats(float& gain, float& se, uint32_t& n, uint32_t& dof) const
{
    gain = se = 0.f;
    n = dof = 0;
    const std::vector<float>& va = mAb.va;
    const std::vector<float>& vb = mAb.vb;
    if (va.empty() || vb.empty()) return;
    if (mAb.oneway)
    {
        const size_t na = va.size(), nb = vb.size();
        double ma = 0.0, mb = 0.0;
        for (float v : va) ma += v;
        for (float v : vb) mb += v;
        ma /= (double)na;
        mb /= (double)nb;
        double sa = 0.0, sb = 0.0;
        for (float v : va) sa += (v - ma) * (v - ma);
        for (float v : vb) sb += (v - mb) * (v - mb);
        double vara = na > 1 ? sa / (double)(na - 1) : -1.0;
        double varb = nb > 1 ? sb / (double)(nb - 1) : -1.0;
        if (vara < 0.0) vara = std::max(varb, 0.0);   // one segment: assume the other state's spread
        if (varb < 0.0) varb = vara;
        gain = (float)(ma - mb);
        se = (float)std::sqrt(vara / (double)na + varb / (double)nb);
        n = (uint32_t)nb;
        dof = (uint32_t)std::max<size_t>(na + nb, 3) - 2;
    }
    else
    {
        n = (uint32_t)std::min(va.size(), vb.size());
        double m = 0.0;
        for (uint32_t i = 0; i < n; ++i) m += (double)va[i] - (double)vb[i];
        m /= (double)n;
        double var = 0.0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const double d = (double)va[i] - (double)vb[i] - m;
            var += d * d;
        }
        gain = (float)m;
        se = n > 1 ? (float)std::sqrt(var / (double)(n - 1) / (double)n) : std::fabs(gain);
        dof = n > 1 ? n - 1 : 1;
    }
    se = std::max(se, 0.03f);   // a few segments can agree by chance
}

// Sequential decision on "gain >= thr": decided once the bound clears it either way, the
// gain is not promising, or the pairs run out (then the point estimate). force: decide on
// what there is (the time limit).
bool LLVKAutoTune::abDecide(bool force)
{
    float gain = 0.f, se = 0.f;
    uint32_t n = 0, dof = 0;
    abStats(gain, se, n, dof);
    if (!n) return false;
    const bool pair_done = mAb.oneway ? mAb.inB : mAb.va.size() == mAb.vb.size();
    if (!force && (!pair_done || n < mAb.minPairs)) return false;
    mAb.gain = gain;
    mAb.se = se;
    mAb.pairs = n;
    if (mAb.kind == AB_CONFIRM)
    {
        if (!force && n < mAb.maxPairs) return false;
        mAb.verdict = 0;
        return true;
    }
    const float t = tcrit90(dof), T = mAb.thr;
    const float lb = gain - t * se, ub = gain + t * se;
    int v = 0;
    if (lb >= T) v = 1;
    else if (ub < 0.f) v = -1;
    else if (ub < T) v = 0;
    else if (gain < kPromising * T) v = 0;   // possible but not promising: no more pairs
    else if (force || n >= mAb.maxPairs) v = gain >= T ? 1 : 0;
    else
    {
        LL_INFOS("AutoTune") << "  " << mAb.label << " " << mAb.to << ": pair " << n << " gain " << gain << " +- " << se
                             << " ms (bound " << lb << ".." << ub << ", decides on " << T << "): undecided, one more pair" << LL_ENDL;
        return false;
    }
    mAb.verdict = v;
    return true;
}

bool LLVKAutoTune::abSegment(const Window& w, bool force)
{
    const float v = 0.5f * (w.mean + w.p75);
    const float g = gpuMs();
    std::vector<float>& vals = mAb.inB ? mAb.vb : mAb.va;
    std::vector<FrameRec>& pool = mAb.inB ? mAb.fb : mAb.fa;
    vals.push_back(v);
    if (pool.size() < 32768) pool.insert(pool.end(), mFrames.begin(), mFrames.end());
    if (g > 0.f)
    {
        (mAb.inB ? mAb.gpuB : mAb.gpuA) += g;
        ++(mAb.inB ? mAb.ngB : mAb.ngA);
    }
    ++mAb.seg;
    if (abDecide(force)) return true;
    // The next segment: A B B A A B B A ... (one switch per pair), one-way A A B B B ...
    const uint32_t s = mAb.seg;
    const bool next_b = mAb.oneway ? s >= kOnewayA : ((s & 3u) == 1u || (s & 3u) == 2u);
    if (next_b != mAb.inB)
    {
        abSettleTo(next_b);
        mOnce = ONCE_SETTLE;
        mPhaseUntil = std::max(wallNow(), mSettleUntil);
    }
    else
    {
        mOnce = ONCE_MEASURE;
        mPhaseUntil = wallNow() + kSegSec;
        resetWindow();
    }
    return false;
}

// Tune once: one finished window -- an A/B segment of the test in flight, or a baseline
// (the current state's level) -- then the next test or the end.
std::string LLVKAutoTune::onceAdvance(const Window& w, bool timed_out, float target_ms)
{
    const std::vector<Lever>& L = levers();
    if (mLeverState.size() != L.size())
    {
        mLeverState.assign(L.size(), LS_UNTRIED);
        mProbeGain.assign(L.size(), 0.f);
    }
    if (mAb.on)
    {
        bool decided = false;
        if (w.ok) decided = abSegment(w, timed_out);
        else if (timed_out) decided = abDecide(true);
        if (decided) onTestDone(timed_out, target_ms);
        else if (!timed_out) return "";   // the next segment is scheduled
        else abAbandon("time limit, nothing measured");
    }
    else if (w.ok)
    {
        mBase = w;
        mBaseOk = true;
    }
    return oncePlan(timed_out, target_ms);
}

void LLVKAutoTune::addTried(size_t lever, const std::string& to, float gain, const char* why)
{
    const Lever& l = levers()[lever];
    LLStringUtil::format_map_t a;
    a["[LEVER]"] = lever_label(l.id, l.label);
    a["[TO]"] = to;
    a["[MS]"] = llformat("%.2f", std::max(0.f, gain));
    if (!std::strcmp(why, "slower"))
        mTried.push_back(tr("AutoTuneTriedSlower", "[LEVER]: [TO] was slower", a));
    else if (!std::strcmp(why, "bundle"))
        mTried.push_back(tr("AutoTuneTriedBundle", "[LEVER]: [TO] saves only [MS] ms, too little even together with others", a));
    else
        mTried.push_back(tr("AutoTuneTriedSmall", "[LEVER]: even [TO] saves only [MS] ms", a));
}

// Keep lever i at value: the value before the test (from the test's A state) is what Undo
// restores.
void LLVKAutoTune::commitKeep(size_t lever, double value, float gain)
{
    const Lever& l = levers()[lever];
    LLControlVariable* ctrl = gSavedSettings.getControl(l.control);
    if (!ctrl) return;
    LLSD before = ctrl->getValue();
    for (const auto& cv : mAb.a)
        if (cv.first == l.control) before = cv.second;
    Kept k;
    k.lever = lever;
    k.rank = mOnceGpuBound ? l.gpu_rank : l.cpu_rank;
    k.control = l.control;
    k.label = lever_label(l.id, l.label);
    k.from = fmt_value(l.fmt, before.asReal());
    k.to = fmt_value(l.fmt, value);
    k.before_value = before;
    k.after_value = control_value(ctrl, value);
    k.gain = gain;
    applyValues({{k.control, k.after_value}}, l.realloc ? kAbSettleRealloc : (l.oneway ? kAbSettleStream : kAbSettle));
    mKept.push_back(k);
    ++mOnceSteps;
    rebuildUndo();
    LL_INFOS("AutoTune") << "Tune once: KEEP " << l.control << " " << k.from << " -> " << k.to << " (gain " << gain << " ms)" << LL_ENDL;
}

void LLVKAutoTune::startBisectTest()
{
    const Lever& l = levers()[mBisect.lever];
    LLControlVariable* ctrl = gSavedSettings.getControl(l.control);
    if (!ctrl) { mBisect.on = false; return; }
    const int mid = mBisect.lo + (mBisect.hi - mBisect.lo) / 2;
    mBisect.mid = mid;
    const double v = mBisect.stages[(size_t)mid];
    mAb = AbTest();
    mAb.kind = AB_BISECT;
    mAb.levers = {mBisect.lever};
    mAb.oneway = l.oneway;
    mAb.settle = l.realloc ? kAbSettleRealloc : (l.oneway ? kAbSettleStream : kAbSettle);
    mAb.a = {{l.control, ctrl->getValue()}};
    mAb.b = {{l.control, control_value(ctrl, v)}};
    mAb.label = lever_label(l.id, l.label);
    mAb.from = fmt_value(l.fmt, ctrl->getValue().asReal());
    mAb.to = fmt_value(l.fmt, v);
    mAb.thr = mBisect.goal;
    mAb.minPairs = 2;
    mAb.maxPairs = 6;
    LL_INFOS("AutoTune") << "Tune once bisect " << l.control << ": stages (" << mBisect.lo + 1 << ".." << mBisect.hi
                         << ") of " << mBisect.stages.size() << ", trying stage " << mid + 1 << " (" << mAb.to << ")" << LL_ENDL;
    abBegin();
}

void LLVKAutoTune::finishBisect()
{
    if (!mBisect.on) return;
    mBisect.on = false;
    const size_t i = mBisect.lever;
    const int hi = mBisect.hi;
    if (hi < 0 || (size_t)hi >= mBisect.stages.size()) return;
    LL_INFOS("AutoTune") << "Tune once bisect " << levers()[i].control << " done: stage " << hi + 1 << " of "
                         << mBisect.stages.size() << " keeps " << mBisect.hiGain << " ms of the probe's " << mBisect.full
                         << " ms (goal " << mBisect.goal << " ms)" << LL_ENDL;
    commitKeep(i, mBisect.stages[(size_t)hi], mBisect.hiGain);
    if (mBisect.hiWin.ok) mBase = mBisect.hiWin;
    if (i < mLeverState.size()) mLeverState[i] = (size_t)hi + 1 < mBisect.stages.size() ? LS_DEFERRED : LS_DONE;
}

bool LLVKAutoTune::startProbe(size_t lever)
{
    const Lever& l = levers()[lever];
    std::vector<double> st;
    double cur = 0.0;
    if (!stagesOf(l, st, cur)) return false;
    LLControlVariable* ctrl = gSavedSettings.getControl(l.control);
    if (!ctrl) return false;
    mBisect = Bisect();
    mBisect.lever = lever;
    mBisect.stages = st;
    mAb = AbTest();
    mAb.kind = AB_PROBE;
    mAb.levers = {lever};
    mAb.oneway = l.oneway;
    mAb.settle = l.realloc ? kAbSettleRealloc : (l.oneway ? kAbSettleStream : kAbSettle);
    mAb.a = {{l.control, ctrl->getValue()}};
    mAb.b = {{l.control, control_value(ctrl, st.back())}};
    mAb.label = lever_label(l.id, l.label);
    mAb.from = fmt_value(l.fmt, cur);
    mAb.to = fmt_value(l.fmt, st.back());
    mAb.thr = onceBar();
    mAb.minPairs = 2;
    mAb.maxPairs = 8;
    abBegin();
    return true;
}

// Confirmation: with real headroom, give back the kept step that cost the most quality
// (highest rank) and whose measured gain fits in the headroom; A = now, B = given back.
bool LLVKAutoTune::startConfirm(const Window& w, float target_ms)
{
    if (mConfirmTries >= kMaxConfirm) return false;
    if (w.p75 > target_ms * kConfirmP75 || std::max(w.cpu50, w.gpu) > target_ms * kConfirmBusy) return false;
    const float room = target_ms * kMetFactor - w.p75;
    int pick = -1;
    for (size_t k = 0; k < mKept.size(); ++k)
    {
        const Kept& s = mKept[k];
        if (s.restored || s.tried_back || s.gain > room) continue;
        // Only the latest kept step of a control (its value is still the one that step set).
        bool later = false;
        for (size_t j = k + 1; j < mKept.size(); ++j) later |= !mKept[j].restored && mKept[j].control == s.control;
        if (later) continue;
        if (pick < 0 || s.rank > mKept[(size_t)pick].rank) pick = (int)k;
    }
    if (pick < 0) return false;
    Kept& s = mKept[(size_t)pick];
    s.tried_back = true;
    ++mConfirmTries;
    const Lever& l = levers()[s.lever];
    mConfirmKept = (size_t)pick;
    mAb = AbTest();
    mAb.kind = AB_CONFIRM;
    mAb.levers = {s.lever};
    mAb.oneway = l.oneway;
    mAb.settle = l.realloc ? kAbSettleRealloc : (l.oneway ? kAbSettleStream : kAbSettle);
    mAb.a = {{s.control, s.after_value}};
    mAb.b = {{s.control, s.before_value}};
    mAb.label = s.label;
    mAb.from = s.to;
    mAb.to = s.from;
    mAb.thr = 0.f;
    mAb.minPairs = mAb.maxPairs = 2;
    LL_INFOS("AutoTune") << "Tune once confirmation " << mConfirmTries << ": headroom (p75 " << w.p75 << " ms, target "
                         << target_ms << " ms) -> try giving back " << s.control << " " << s.to << " -> " << s.from
                         << " (its measured gain " << s.gain << " ms)" << LL_ENDL;
    abBegin();
    return true;
}

void LLVKAutoTune::onTestDone(bool timed_out, float target_ms)
{
    const std::vector<Lever>& L = levers();
    mAb.on = false;
    const Window wa = abWindow(false), wb = abWindow(true);
    if (wa.ok) { mBase = wa; mBaseOk = true; }   // the current state's level, measured just now
    LL_INFOS("AutoTune") << "Tune once test " << mOnceTests << " (" << ab_kind_name(mAb.kind) << ") " << mAb.label << " "
                         << mAb.from << " -> " << mAb.to << ": gain " << mAb.gain << " +- " << mAb.se << " ms over " << mAb.pairs
                         << (mAb.oneway ? " B segment(s), one-way" : " pair(s)") << " (decides on " << mAb.thr << " ms; A p75 "
                         << wa.p75 << " / mean " << wa.mean << ", B p75 " << wb.p75 << " / mean " << wb.mean << "): "
                         << (mAb.kind == AB_CONFIRM ? "measured" : (mAb.verdict > 0 ? "YES" : (mAb.verdict < 0 ? "SLOWER" : "no")))
                         << LL_ENDL;
    auto busy_of = [](const Window& x) { return std::max(x.cpu50, x.gpu); };
    switch (mAb.kind)
    {
    case AB_PROBE:
    {
        const size_t i = mAb.levers.front();
        noteVerdict(L[i].id, mAb.verdict > 0);
        if (mAb.verdict > 0)
        {
            const std::vector<double>& st = mBisect.stages;
            if (st.size() <= 1)
            {
                commitKeep(i, st.back(), mAb.gain);   // one stage: the probe was the change
                if (wb.ok) mBase = wb;
                mLeverState[i] = LS_DONE;
            }
            else
            {
                abSettleTo(false);
                mBisect.on = true;
                mBisect.full = mAb.gain;
                mBisect.lo = -1;
                mBisect.hi = (int)st.size() - 1;
                mBisect.hiGain = mAb.gain;
                mBisect.hiWin = wb;
                const float need = mBase.p75 - target_ms * kMetFactor;
                mBisect.goal = std::max(onceBar(), std::min(kBisectShare * mAb.gain, need));
                LL_INFOS("AutoTune") << "Tune once bisect " << L[i].control << ": " << mAb.to << " gains " << mAb.gain
                                     << " ms; the mildest of " << st.size() << " stages keeping >= " << mBisect.goal
                                     << " ms (" << kBisectShare * 100.f << "% of it, or the " << need << " ms still needed)" << LL_ENDL;
            }
        }
        else
        {
            abSettleTo(false);
            mLeverState[i] = LS_DONE;
            if (mAb.verdict == 0 && mAb.gain > 0.f)
            {
                mLeverState[i] = LS_POOL;   // small but positive: tried again together with others
                mProbeGain[i] = mAb.gain;
            }
            else
                addTried(i, mAb.to, mAb.gain, mAb.verdict < 0 || mAb.gain < -mAb.thr ? "slower" : "small");
        }
        break;
    }
    case AB_BISECT:
    {
        if (mAb.verdict > 0)
        {
            mBisect.hi = mBisect.mid;
            mBisect.hiGain = mAb.gain;
            mBisect.hiWin = wb;
        }
        else
            mBisect.lo = mBisect.mid;
        if (mBisect.hi - mBisect.lo <= 1)
        {
            // Already at the chosen stage: no switch back and forth.
            if (!(mAb.inB && mBisect.hi == mBisect.mid)) abSettleTo(false);
            finishBisect();
        }
        else
            abSettleTo(false);
        break;
    }
    case AB_BUNDLE:
    {
        const std::vector<size_t> members = mAb.levers;
        if (mAb.verdict > 0)
        {
            const bool time_left = wallNow() - mOnceStart < kMaxOnceSec - kMinTestSec;
            if (members.size() >= 2 && !timed_out && time_left)
            {
                // Does the stronger half alone carry it?
                abSettleTo(false);
                AbTest prev = mAb;
                const size_t half = (members.size() + 1) / 2;
                mAb = AbTest();
                mAb.kind = AB_SPLIT;
                mAb.levers = members;
                mAb.half = half;
                mAb.bundleGain = prev.gain;
                mAb.bundleWin = wb;
                mAb.full = prev.b;
                mAb.oneway = prev.oneway;
                mAb.settle = prev.settle;
                mAb.a = prev.a;
                mAb.b.assign(prev.b.begin(), prev.b.begin() + (std::ptrdiff_t)std::min(half, prev.b.size()));
                for (size_t k = 0; k < half; ++k)
                    mAb.label += (k ? " + " : "") + lever_label(L[members[k]].id, L[members[k]].label);
                mAb.from = prev.from;
                mAb.to = prev.to;
                mAb.thr = kBisectShare * prev.gain;
                mAb.minPairs = 2;
                mAb.maxPairs = 6;
                abBegin();
                return;
            }
            // levers / a / b are index-aligned (the values may already be applied: state B).
            for (size_t k = 0; k < members.size() && k < mAb.b.size(); ++k)
            {
                commitKeep(members[k], mAb.b[k].second.asReal(), mProbeGain[members[k]]);
                mLeverState[members[k]] = LS_DONE;
            }
            if (wb.ok) mBase = wb;
        }
        else
        {
            abSettleTo(false);
            for (size_t k = 0; k < members.size() && k < mAb.b.size(); ++k)
            {
                const size_t i = members[k];
                addTried(i, fmt_value(L[i].fmt, mAb.b[k].second.asReal()), mProbeGain[i], "bundle");
                mLeverState[i] = LS_DONE;
            }
        }
        break;
    }
    case AB_SPLIT:
    {
        const std::vector<size_t> members = mAb.levers;
        const size_t keep_n = mAb.verdict > 0 ? mAb.half : members.size();
        if (mAb.verdict <= 0) applyValues(mAb.full, mAb.settle);   // the whole bundle it is
        for (size_t k = 0; k < members.size() && k < mAb.full.size(); ++k)
        {
            const size_t i = members[k];
            const double v = mAb.full[k].second.asReal();   // index-aligned with levers
            if (k < keep_n) commitKeep(i, v, mProbeGain[i]);
            else addTried(i, fmt_value(L[i].fmt, v), mProbeGain[i], "bundle");
            mLeverState[i] = LS_DONE;
        }
        if (mAb.verdict > 0 ? wb.ok : mAb.bundleWin.ok) mBase = mAb.verdict > 0 ? wb : mAb.bundleWin;
        break;
    }
    case AB_CONFIRM:
    {
        const bool ok = wb.ok && wb.p75 <= target_ms * kMetFactor && busy_of(wb) <= target_ms * kHeadroom;
        LL_INFOS("AutoTune") << "Tune once confirmation: " << mAb.label << " back at " << mAb.to << ": p75 " << wb.p75
                             << " ms, busy " << busy_of(wb) << " ms (target " << target_ms << " ms): "
                             << (ok ? "target still met, given back" : "target missed, kept") << LL_ENDL;
        if (ok && mConfirmKept < mKept.size())
        {
            abSettleTo(true);
            mKept[mConfirmKept].restored = true;
            mRestored.push_back(mAb.label + ": " + mAb.from + " -> " + mAb.to);
            rebuildUndo();
            mBase = wb;
        }
        else
            abSettleTo(false);
        break;
    }
    default: break;
    }
}

std::string LLVKAutoTune::oncePlan(bool timed_out, float target_ms)
{
    if (mAb.on) return "";   // onTestDone chained a test (a bundle split)
    const double now = wallNow();
    const std::vector<Lever>& L = levers();
    auto busy_of = [](const Window& x) { return std::max(x.cpu50, x.gpu); };
    const bool out_of_time = timed_out || now - mOnceStart > kMaxOnceSec - kMinTestSec;
    if (!mBaseOk)
    {
        if (timed_out) return "limit";
        mOnce = ONCE_SETTLE;   // a fresh baseline after the last change's settle
        mPhaseUntil = std::max(now + 0.5, mSettleUntil);
        return "";
    }
    if (mOnceStartMs <= 0.f)
    {
        mOnceStartMs = mBase.p75;
        if (mBase.p75 <= target_ms * kMetFactor) return "met";
    }
    if (mBisect.on)
    {
        if (!out_of_time) { startBisectTest(); if (mAb.on) return ""; }
        finishBisect();   // out of time: the best stage known so far
    }
    if (mBase.p75 <= target_ms * kMetFactor && busy_of(mBase) <= target_ms * kHeadroom)
    {
        if (mUndo.empty()) return "met";   // the scene got lighter: nothing to take
        if (!out_of_time && startConfirm(mBase, target_ms)) return "";
        return "reached";
    }
    if (out_of_time) return "limit";
    const bool gpu_bound = mBase.gpu_bound;
    mOnceGpuBound = gpu_bound;
    logDecision(llformat("Tune once: level p75 %.1f ms, mean %.1f ms, target %.1f ms, CPU %.1f ms, GPU %.1f ms, %s-bound, %.0f s left",
                         mBase.p75, mBase.mean, target_ms, mBase.cpu50, mBase.gpu, gpu_bound ? "GPU" : "CPU",
                         kMaxOnceSec - (now - mOnceStart)), true);
    // The next lever to probe: the static rank (the visual-quality prior: cheapest loss
    // first) divided by its attributed pass ms relative to what is needed (GPU-bound; an
    // unattributable lever counts as average), pushed back by this machine's history.
    // Untried levers first; deferred ones (kept at a milder stage) only once none is left.
    const float ref = std::max(0.5f, 0.5f * std::max(0.f, mBase.p75 - target_ms));
    for (int pass = 0; pass < 2; ++pass)
    {
        struct Cand { size_t i; float key; uint8_t rank; };
        std::vector<Cand> cands;
        for (size_t i = 0; i < L.size(); ++i)
        {
            const Lever& l = L[i];
            const uint8_t rank = gpu_bound ? l.gpu_rank : l.cpu_rank;
            if (!l.control || !rank || mLeverState[i] != LS_UNTRIED || !leverAllowed(l)) continue;
            bool known = false;
            const float est = gpu_bound ? leverMs(l, &known) : 0.f;
            if (gpu_bound && known && est < std::max(0.15f, 0.02f * mBase.p50))
            {
                mLeverState[i] = LS_DONE;   // its passes cost next to nothing in this scene
                LL_INFOS("AutoTune") << "Tune once: " << l.id << " skipped, its passes cost " << est << " ms" << LL_ENDL;
                continue;
            }
            float key = (float)rank;
            if (gpu_bound && known) key /= llclamp(est / ref, 0.5f, 3.f);
            key *= 1.f + kHistWeight * (float)histCount(l.id);
            cands.push_back({i, key, rank});
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.key != b.key ? a.key < b.key : a.rank < b.rank;
        });
        for (const Cand& c : cands)
            if (startProbe(c.i)) return "";
        bool rearmed = false;
        for (uint8_t& st : mLeverState)
            if (st == LS_DEFERRED) { st = LS_UNTRIED; rearmed = true; }
        if (!rearmed) break;
    }
    // Small positive levers, together (the largest probe gains, one lever per control).
    std::vector<size_t> pool;
    for (size_t i = 0; i < L.size(); ++i)
        if (mLeverState[i] == LS_POOL) pool.push_back(i);
    std::stable_sort(pool.begin(), pool.end(), [&](size_t a, size_t b) { return mProbeGain[a] > mProbeGain[b]; });
    mAb = AbTest();
    for (size_t i : pool)
    {
        const Lever& l = L[i];
        std::vector<double> st;
        double cur = 0.0;
        LLControlVariable* ctrl = gSavedSettings.getControl(l.control);
        bool dup = false;
        for (const auto& cv : mAb.b) dup |= cv.first == l.control;
        const bool has = ctrl && stagesOf(l, st, cur);
        if (mAb.levers.size() >= kMaxBundle || dup || !has)
        {
            addTried(i, st.empty() ? std::string() : fmt_value(l.fmt, st.back()), mProbeGain[i], "small");
            mLeverState[i] = LS_DONE;
            continue;
        }
        mAb.levers.push_back(i);
        mAb.a.push_back({l.control, ctrl->getValue()});
        mAb.b.push_back({l.control, control_value(ctrl, st.back())});
        mAb.oneway |= l.oneway;
        mAb.settle = std::max(mAb.settle, l.realloc ? kAbSettleRealloc : (l.oneway ? kAbSettleStream : kAbSettle));
        mAb.label += (mAb.label.empty() ? "" : " + ") + lever_label(l.id, l.label);
    }
    if (mAb.levers.size() >= 2)
    {
        mAb.kind = AB_BUNDLE;
        mAb.from = tr("AutoTuneBundleFrom", "your settings");
        mAb.to = tr("AutoTuneBundleTo", "all reduced");
        mAb.thr = onceBar();
        mAb.minPairs = 2;
        mAb.maxPairs = 8;
        abBegin();
        return "";
    }
    for (size_t i : mAb.levers)
    {
        double v = 0.0;
        std::vector<double> st;
        double cur = 0.0;
        if (stagesOf(L[i], st, cur)) v = st.back();
        addTried(i, fmt_value(L[i].fmt, v), mProbeGain[i], "small");
        mLeverState[i] = LS_DONE;
    }
    mAb = AbTest();
    return "exhausted";
}

// Undo = the kept steps not given back, one entry per control: its value before the run
// (the first kept step's) and the last kept step's text.
void LLVKAutoTune::rebuildUndo()
{
    mUndo.clear();
    for (const Kept& k : mKept)
    {
        if (k.restored) continue;
        Change* u = nullptr;
        for (Change& c : mUndo)
            if (c.control == k.control) u = &c;
        if (u)
        {
            u->to = k.to;
            continue;
        }
        Change c;
        c.control = k.control;
        c.label = k.label;
        c.from = k.from;
        c.to = k.to;
        c.old_value = k.before_value;
        mUndo.push_back(c);
    }
}

std::string LLVKAutoTune::onceSummary(const std::string& end, float end_ms)
{
    LLStringUtil::format_map_t args;
    args["[FPS]"] = llformat("%.0f", end_ms > 0.f ? 1000.f / end_ms : 0.f);
    args["[TARGET]"] = llformat("%u", effectiveTargetFPS());
    std::ostringstream os;
    if (end == "met")
        os << tr("AutoTuneSummaryMet", "Already running at [FPS] fps (target [TARGET] fps): nothing changed.", args);
    else if (end == "cancelled")
    {
        os << tr("AutoTuneSummaryCancelled", "Cancelled.");
        if (!mUndo.empty())
        {
            os << " " << tr("AutoTuneSummaryChanged", "Changed:");
            for (const Change& c : mUndo) os << "\n  " << c.label << ": " << c.from << " -> " << c.to;
        }
    }
    else if (mUndo.empty())
        os << tr("AutoTuneSummaryNothing", "No setting left that would help ([FPS] fps, target [TARGET] fps): nothing changed.", args);
    else
    {
        args["[FROM]"] = llformat("%.0f", mOnceStartMs > 0.f ? 1000.f / mOnceStartMs : 0.f);
        os << (end == "reached" ? tr("AutoTuneSummaryReached", "[FROM] -> [FPS] fps (target [TARGET] fps). Changed:", args)
                                : tr("AutoTuneSummaryPartial", "[FROM] -> [FPS] fps; target [TARGET] fps not reached. Changed:", args));
        for (const Change& c : mUndo) os << "\n  " << c.label << ": " << c.from << " -> " << c.to;
    }
    if (!mRestored.empty())
    {
        os << "\n" << tr("AutoTuneSummaryRestored", "Given back (the target still held):");
        for (const std::string& r : mRestored) os << "\n  " << r;
    }
    if (!mTried.empty())
    {
        os << "\n" << tr("AutoTuneSummaryTried", "Tried, did not help (your setting kept):");
        for (const std::string& t : mTried) os << "\n  " << t;
    }
    return os.str();
}

LLVKPerfCaps LLVKAutoTune::capsFor(const std::vector<uint8_t>& levels) const
{
    LLVKPerfCaps c;
    const std::vector<Lever>& L = levers();
    for (size_t i = 0; i < L.size() && i < levels.size(); ++i)
        if (levels[i] && L[i].cap) L[i].cap(c, levels[i]);
    return c;
}

void LLVKAutoTune::pushCaps()
{
    const LLVKPerfCaps c = capsFor(mCapLevel);
    if (c != mCapsPushed)
    {
        mCapsPushed = c;
        LLVulkanContext& vk = LLVulkanContext::instance();
        if (vk.isInitialized()) vk.setAutoTunePerfCaps(c);
    }
    // The non-impostor cap levels: 12 / 8 / 5 / 3 fully rendered avatars.
    const std::vector<Lever>& L = levers();
    U32 nonimp = 0;
    for (size_t i = 0; i < L.size() && i < mCapLevel.size(); ++i)
        if (L[i].nonimp && mCapLevel[i])
        {
            static const U32 k[] = {12, 8, 5, 3};
            nonimp = k[std::min<size_t>(mCapLevel[i], 4) - 1];
        }
    mNonImpCapTuner = nonimp;
    applyLLSideCaps();
}

bool LLVKAutoTune::capStep(bool gpu_bound, const Window& w)
{
    const std::vector<Lever>& L = levers();
    if (mCapLevel.size() != L.size()) mCapLevel.assign(L.size(), 0);
    if (mCapBlockUntil.size() != L.size()) { mCapBlockUntil.assign(L.size(), 0.0); mCapBlockBusy.assign(L.size(), 0.f); }
    const double now = wallNow();
    const float busy = std::max(w.cpu50, w.gpu);
    // The static rank, pushed back by this machine's history of no-gain verdicts.
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < L.size(); ++i)
    {
        const uint8_t rank = gpu_bound ? L[i].gpu_rank : L[i].cpu_rank;
        if (L[i].cap_levels && rank)
            order.push_back({(float)rank * (1.f + kHistWeight * (float)histCount(L[i].id)), i});
    }
    std::stable_sort(order.begin(), order.end(), [](const std::pair<float, size_t>& a, const std::pair<float, size_t>& b) {
        return a.first < b.first;
    });
    const LLVKPerfCaps vram = mSample.valid ? mSample.vram : LLVKPerfCaps();
    for (const auto& o : order)
    {
        const size_t i = o.second;
        const Lever& l = L[i];
        if (mCapLevel[i] >= l.cap_levels || !leverAllowed(l)) continue;
        if (measuredCheap(l, w)) continue;
        // Recently measured as no help: left alone, unless the scene changed notably since.
        if (mCapBlockUntil[i] > now)
        {
            const float b0 = mCapBlockBusy[i];
            if (b0 <= 0.f || (busy < b0 * 1.3f && busy > b0 * 0.7f)) continue;
            mCapBlockUntil[i] = 0.0;
        }
        std::vector<uint8_t> next = mCapLevel;
        ++next[i];
        // Skip what the VRAM ladder already holds (merged caps would not change).
        if (!l.nonimp && LLVKPerfCaps::merged(vram, capsFor(next)) == LLVKPerfCaps::merged(vram, capsFor(mCapLevel)))
            continue;
        if (l.nonimp)
        {
            // The first level below what is in effect now (the user's setting, "simplify
            // avatars beyond", or our own previous level); none -> not useful.
            static const U32 k[] = {12, 8, 5, 3};
            const U32 cur = LLVOAvatar::sMaxNonImpostors;   // 0 = unlimited
            while (next[i] <= l.cap_levels && cur && k[next[i] - 1] >= cur) ++next[i];
            if (next[i] > l.cap_levels) continue;
        }
        mCapOrder.push_back({(uint16_t)i, mCapLevel[i]});
        mCapLevel[i] = next[i];
        mCapChangedAt = now;
        pushCaps();
        // An experiment: judged against this window once it has settled (frameTick).
        mCapTrial.on = true;
        mCapTrial.lever = i;
        mCapTrial.before = w;
        mCapTrial.measure_from = now + (l.realloc ? kSettleRealloc : kSettleSec);
        logDecision(llformat("Keep my FPS: %s-bound (p75 %.1f ms over target %.1f ms; CPU %.1f ms, GPU %.1f ms) -> "
                             "hold back %s (level %u/%u)", gpu_bound ? "GPU" : "CPU", w.p75,
                             1000.f / (float)effectiveTargetFPS(), w.cpu50, w.gpu, l.id, (unsigned)mCapLevel[i],
                             (unsigned)l.cap_levels), true);
        return true;
    }
    logDecision(llformat("Keep my FPS: %s-bound at p75 %.1f ms, nothing left to hold back", gpu_bound ? "GPU" : "CPU", w.p75));
    return false;
}

bool LLVKAutoTune::capRelease()
{
    if (mCapOrder.empty()) return false;
    const size_t i = mCapOrder.back().first;
    if (mCapTrial.on && mCapTrial.lever == i) mCapTrial.on = false;
    if (i < mCapLevel.size()) mCapLevel[i] = mCapOrder.back().second;   // the level before that step
    mCapOrder.pop_back();
    mCapChangedAt = wallNow();
    pushCaps();
    logDecision(llformat("Keep my FPS: headroom (frame %.1f ms, CPU %.1f ms, GPU %.1f ms) -> restored %s (calm wait %.0f s)",
                         mFrameMsSm, mCpuMsSm, gpuMs(), levers()[i].id, mReleaseDwell), true);
    return true;
}

// ============================================================================
// User actions
// ============================================================================

void LLVKAutoTune::startTuneOnce()
{
    if (mOnce != ONCE_IDLE) return;
    // "Keep my FPS" pauses: its temporary caps would distort what Tune once measures.
    if (!mCapOrder.empty() || mNonImpCapTuner)
    {
        mCapOrder.clear();
        std::fill(mCapLevel.begin(), mCapLevel.end(), 0);
        mNonImpCapTuner = 0;
        pushCaps();
    }
    mCapTrial.on = false;
    mUndo.clear();
    mSummary.clear();
    mOnceSteps = 0;
    mOnceStartMs = 0.f;
    mAb = AbTest();
    mBisect = Bisect();
    mKept.clear();
    mTried.clear();
    mRestored.clear();
    mLeverState.assign(levers().size(), LS_UNTRIED);
    mProbeGain.assign(levers().size(), 0.f);
    mBase = Window();
    mBaseOk = false;
    mSettleUntil = 0.0;
    mOnceTests = mConfirmTries = 0;
    mOnceStart = wallNow();
    mOnce = ONCE_SETTLE;
    mPhaseUntil = mOnceStart + 1.0;
    resetWindow();
    LL_INFOS("AutoTune") << "Tune once started: target " << effectiveTargetFPS() << " fps (TargetFPS "
                         << gSavedSettings.getU32("TargetFPS") << ", refresh " << refreshRate() << " Hz)" << LL_ENDL;
}

void LLVKAutoTune::cancelTuneOnce()
{
    if (mOnce == ONCE_IDLE) return;
    abAbandon("cancelled");   // the test in flight is not a kept change
    mBisect.on = false;
    mOnce = ONCE_IDLE;
    mSummary = onceSummary("cancelled", mBaseOk ? mBase.p75 : mFrameMsSm);
    LL_INFOS("AutoTune") << "Tune once cancelled after " << mUndo.size() << " change(s)" << LL_ENDL;
}

void LLVKAutoTune::undo()
{
    if (mOnce != ONCE_IDLE || mUndo.empty()) return;
    for (auto it = mUndo.rbegin(); it != mUndo.rend(); ++it)
    {
        if (LLControlVariable* ctrl = gSavedSettings.getControl(it->control))
        {
            ctrl->setValue(it->old_value);
            LL_INFOS("AutoTune") << "Undo: " << it->control << " -> " << it->from << LL_ENDL;
        }
    }
    mUndo.clear();
    mKept.clear();
    mSummary = tr("AutoTuneSummaryUndone", "Undone: your previous settings are back.");
}

void LLVKAutoTune::setContinuous(bool on)
{
    if (mContinuous == on) return;
    mContinuous = on;
    mCapTrial.on = false;
    mComfySince = mSlowSince = 0.0;
    mReleaseDwell = 10.0;
    mCapChangedAt = wallNow();
    resetWindow();
    LL_INFOS("AutoTune") << "Keep my FPS " << (on ? "on" : "off") << " (target " << effectiveTargetFPS() << " fps)" << LL_ENDL;
}

// ============================================================================
// UI text
// ============================================================================

std::string LLVKAutoTune::statusText() const
{
    LLStringUtil::format_map_t args;
    args["[TARGET]"] = llformat("%u", effectiveTargetFPS());
    if (mOnce != ONCE_IDLE)
    {
        args["[STEPS]"] = llformat("%u", mOnceSteps);
        if (mAb.on)
        {
            const size_t done = mAb.oneway ? mAb.vb.size() : std::min(mAb.va.size(), mAb.vb.size());
            args["[LEVER]"] = mAb.label;
            args["[VALUE]"] = mAb.to;
            args["[TRIED]"] = llformat("%u", (unsigned)mTried.size());
            args["[PAIR]"] = llformat("%u", (unsigned)std::min<size_t>(done + 1, mAb.maxPairs));
            args["[MAX]"] = llformat("%u", mAb.maxPairs);
            switch (mAb.kind)
            {
            case AB_PROBE:
                return tr("AutoTuneStatusProbe", "Tuning for [TARGET] fps: probing [LEVER] at [VALUE] (A/B pair [PAIR], up to [MAX]; [STEPS] kept, [TRIED] did not help)", args);
            case AB_BISECT:
                return tr("AutoTuneStatusBisect", "Tuning for [TARGET] fps: bisecting [LEVER], trying [VALUE] (A/B pair [PAIR], up to [MAX]; [STEPS] kept)", args);
            case AB_BUNDLE:
                return tr("AutoTuneStatusBundle", "Tuning for [TARGET] fps: testing small savings together: [LEVER] (A/B pair [PAIR], up to [MAX])", args);
            case AB_SPLIT:
                return tr("AutoTuneStatusSplit", "Tuning for [TARGET] fps: does [LEVER] alone carry it? (A/B pair [PAIR], up to [MAX])", args);
            default:
                return tr("AutoTuneStatusConfirming", "Tuning for [TARGET] fps: checking whether [LEVER] can stay at [VALUE] (A/B pair [PAIR] of [MAX])", args);
            }
        }
        return tr("AutoTuneStatusTuning", "Tuning for [TARGET] fps... ([STEPS] change(s) so far)", args);
    }
    if (!mBlockReason.empty() && (mContinuous))
        return tr("AutoTuneStatusPaused", "Paused (teleport, region change or window in the background).", args);
    if (mContinuous)
    {
        if (mCapOrder.empty())
            return tr("AutoTuneStatusKeepFull", "Keeping [TARGET] fps: full settings.", args);
        std::string held;
        const std::vector<Lever>& L = levers();
        std::vector<bool> named(L.size(), false);
        for (const auto& step : mCapOrder)
        {
            const uint16_t i = step.first;
            if (i >= L.size() || named[i]) continue;
            named[i] = true;
            if (!held.empty()) held += ", ";
            held += lever_label(L[i].id, L[i].label);
        }
        args["[HELD]"] = held;
        return tr("AutoTuneStatusKeepHeld", "Keeping [TARGET] fps: temporarily reduced [HELD].", args);
    }
    return std::string();
}

std::string LLVKAutoTune::breakdownText(bool detailed) const
{
    std::ostringstream os;
    const float frame = mFrameMsSm, cpu = mCpuMsSm, gpu = gpuMs();
    if (frame <= 0.f) return tr("AutoTuneMeasuring", "Measuring...");
    LLStringUtil::format_map_t a;
    a["[FRAME]"] = fmtMs(frame);
    a["[FPS]"] = llformat("%.0f", 1000.f / frame);
    a["[CPU]"] = fmtMs(cpu);
    a["[GPU]"] = gpu > 0.f ? fmtMs(gpu) : std::string("?");
    os << tr("AutoTuneFrameLine", "Frame [FRAME] ms ([FPS] fps): main thread (CPU) [CPU] ms, graphics card (GPU) [GPU] ms", a);
    // What limits it.
    const float target_ms = 1000.f / (float)effectiveTargetFPS();
    std::string lim;
    if (gpu > 0.f && gpu >= 0.85f * frame) lim = tr("AutoTuneLimitGPU", "the graphics card");
    else if (frame <= target_ms * 1.03f && std::max(cpu, gpu) < frame * 0.8f) lim = tr("AutoTuneLimitCap", "the frame rate cap / vsync");
    else lim = tr("AutoTuneLimitCPU", "the main thread (CPU)");
    a["[LIMIT]"] = lim;
    os << "\n" << tr("AutoTuneLimitLine", "Limited by: [LIMIT]", a);
    // Top GPU passes.
    float sm[CAT_COUNT] = {};
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(sCatMutex);
        fresh = sCats.frames > 30 && wallNow() - sCats.at < 3.0;
        if (fresh) std::copy(sCats.sm, sCats.sm + CAT_COUNT, sm);
    }
    if (fresh)
    {
        uint32_t order[CAT_COUNT];
        for (uint32_t i = 0; i < CAT_COUNT; ++i) order[i] = i;
        std::sort(order, order + CAT_COUNT, [&](uint32_t x, uint32_t y) { return sm[x] > sm[y]; });
        std::string top;
        const uint32_t n = detailed ? 6u : 4u;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint32_t c = order[k];
            if (sm[c] < 0.05f) break;
            if (!top.empty()) top += ", ";
            top += tr(catLabel(c), cat_fallback(c)) + " " + fmtMs(sm[c]);
        }
        a["[TOP]"] = top;
        os << "\n" << tr("AutoTuneTopLine", "GPU passes (ms, approximate; work overlaps, so they need not add up to the GPU total): [TOP]", a);
    }
    else
        os << "\n" << tr("AutoTuneTopWait", "GPU passes: collecting timings...");
    if (detailed && mSample.valid)
    {
        if (mSample.gpuAgeSec < 3.0 && mSample.rigGpuMs > 0.f)
        {
            a["[RIG]"] = fmtMs(mSample.rigGpuMs);
            os << "\n" << tr("AutoTuneRigLine", "Worn mesh (avatars): [RIG] ms of GPU time", a);
        }
        if (mSample.vramBudget > 0)
        {
            a["[USED]"] = llformat("%.1f", (double)mSample.vramUsage / (1024.0 * 1024.0 * 1024.0));
            a["[BUDGET]"] = llformat("%.1f", (double)mSample.vramBudget / (1024.0 * 1024.0 * 1024.0));
            os << "\n" << (mSample.vramRungs ? tr("AutoTuneVramHeld", "Video memory: [USED] of [BUDGET] GB (memory saving active)", a)
                                             : tr("AutoTuneVramLine", "Video memory: [USED] of [BUDGET] GB", a));
        }
    }
    return os.str();
}
