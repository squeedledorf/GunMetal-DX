/**
 * @file llvkperfcapture.cpp
 * @brief #966 user-visible performance timings and captures. See the header.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvkperfcapture.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llclipboard.h"
#include "lldate.h"
#include "lldiriterator.h"
#include "llfeaturemanager.h"
#include "llfile.h"
#include "llfloaterreportbug.h"
#include "llfocusmgr.h"
#include "llmemory.h"
#include "llmemorygovernor.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvkautotune.h"
#include "llvkhitch.h"
#include "llwindow.h"
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <sstream>

static_assert(LLVKHitch::kSlots == 512, "LLVKPerfCapture::kSlots mirrors LLVKHitch::kSlots");

namespace
{
    double wallNow()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr double kLiveHoldSec = 2.0;    // requestLive keeps sampling this long
    constexpr double kRecentSec   = 5.0;    // live FPS / 1% low window
    constexpr double kWarmupSec   = 1.0;    // capture: the timers start, first results arrive
    constexpr int    kMaxDepth    = 32;     // section tree walk guard

    // Main-thread time that is not main-thread work: waiting for the GPU / the frame slot
    // (frame pacing), and the swapchain image -- the same set LLVKAutoTune subtracts.
    bool isGpuWait(const char* n)
    {
        return !std::strcmp(n, "vk_acquireWait") || !std::strcmp(n, "vk_paceWait")
            || !std::strcmp(n, "vk_asyncSlotWait") || !std::strcmp(n, "vk_texStageWait");
    }
    bool isPresent(const char* n) { return !std::strcmp(n, "vk_present"); }
    bool isCapSleep(const char* n) { return !std::strcmp(n, "fpsLimitSleep"); }
    // Slots that carry GPU times read back on the CPU (LLVKHitch::add), not CPU work.
    bool isGpuSlot(const char* n) { return !std::strncmp(n, "gpu_", 4); }

    // p-quantile of `total` values of which `v` are the non-zero ones (the rest are 0).
    float pctZeros(std::vector<float> v, size_t total, double q)
    {
        if (!total) return 0.f;
        std::sort(v.begin(), v.end());
        const size_t zeros = total > v.size() ? total - v.size() : 0;
        const size_t i = std::min(total - 1, (size_t)(q * (double)(total - 1) + 0.5));
        return i < zeros ? 0.f : v[i - zeros];
    }

    // 1% low: the average frame rate of the slowest 1% of frames (at least one frame).
    float onePercentLowFps(std::vector<float> ms)
    {
        if (ms.empty()) return 0.f;
        std::sort(ms.begin(), ms.end(), std::greater<float>());
        const size_t n = std::max<size_t>(1, ms.size() / 100);
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += ms[i];
        return sum > 0.0 ? (float)(1000.0 * (double)n / sum) : 0.f;
    }

    std::string fit(const std::string& s, size_t w)
    {
        if (s.size() <= w) return s + std::string(w - s.size(), ' ');
        return s.substr(0, w - 1) + "~";
    }

    std::string gb(U64 bytes) { return llformat("%.1f", (double)bytes / (1024.0 * 1024.0 * 1024.0)); }

    // RenderVKUpscaler values (settings.xml) in plain words.
    std::string upscalerSettingName(U32 v)
    {
        static const char* const k[] = {"Off (native)", "FSR 3.1 Native AA", "FSR 3.1 Quality", "FSR 3.1 Balanced",
                                        "FSR 3.1 Performance", "FSR 3.1 Ultra Performance", "FSR 3.1 Custom scale",
                                        "DLSS DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance",
                                        "DLSS Ultra Performance"};
        return v < sizeof(k) / sizeof(k[0]) ? std::string(k[v]) : llformat("mode %u", v);
    }

    const char* upscalerRanName(uint8_t u)
    {
        switch (u)
        {
        case LLVKAutoTuneSample::UPS_FSR:     return "FSR 3.1";
        case LLVKAutoTuneSample::UPS_DLSS_SR: return "DLSS Super Resolution";
        case LLVKAutoTuneSample::UPS_DLSS_RR: return "DLSS Ray Reconstruction";
        default:                              return "none (TAA / native)";
        }
    }

    // Render settings for the capture header (a subset of the bug report's list; missing
    // ones are skipped).
    const char* const kCaptureSettings[] = {
        "RenderQualityPerformance", "RenderFarClip", "RenderVKUpscaler", "RenderVKDlssRayReconstruction",
        "RenderVKRenderScale", "RenderVSyncEnable", "RenderVKFramePacing", "RenderFPSLimit", "RenderVKRayTracing",
        "RenderVKRTReflections", "RenderShadowDetail", "RenderVKSunShadows", "RenderVKShadowResolution",
        "RenderVKProjectorShadows", "RenderReflectionProbeDetail", "RenderReflectionProbeLevel",
        "RenderScreenSpaceReflections", "RenderMirrors", "RenderVKTAA", "RenderVKGodRays", "RenderDeferredSSAO",
        "RenderVKAOStandard", "RenderDepthOfField", "RenderVolumeLODFactor", "RenderAvatarMaxNonImpostors",
        "RenderAvatarMaxComplexity", "RenderMaxTextureResolution", "RenderVKClusterLOD", "RenderHiDPI",
        "RenderVKGpuSubMarks", "AutoTuneLock",
    };
}

LLVKPerfCapture& LLVKPerfCapture::instance()
{
    static LLVKPerfCapture s;
    return s;
}

// ============================================================================
// Plain-language names (partial; first match wins, so specific prefixes come first)
// ============================================================================

std::string LLVKPerfCapture::gpuPassName(const char* mark)
{
    if (!mark) return std::string();
    static const struct { const char* p; const char* n; } kMap[] = {
        {"q_async_busy", "Async queue busy (total)"},
        // worn mesh (avatars)
        {"c_cluster_preskin", "Avatars: skinning (cluster)"}, {"c_preskin", "Avatars: skinning"},
        {"c_skin_pal", "Avatars: bone matrices"}, {"c_rigweld", "Avatars: mesh seam welding"},
        {"c_anim", "Avatars: animation"}, {"g_gbuf_rigged", "Avatars: geometry"}, {"g_lit_rigged", "Avatars: lighting"},
        {"g_pre_rigged", "Avatars: depth pre-pass"}, {"c_shadow_rigcull", "Avatars: shadow culling"},
        // snow / weather
        {"c_snow", "Snow: mesh build"}, {"g_gbuf_snow", "Snow cover"}, {"g_gbuf_pre_snow", "Snow cover"},
        {"g_pre_snow", "Snow cover (depth)"}, {"g_pre_pre_snow", "Snow cover (depth)"}, {"g_fwd_snow", "Snow cover"},
        {"g_sun_snow", "Snow: shadows"}, {"c_weather", "Weather simulation"}, {"g_weather", "Weather (rain / snow)"},
        // cluster LOD
        {"g_gbuf_cluster", "Geometry (cluster LOD)"}, {"g_cl_", "Geometry (cluster LOD)"},
        {"g_pre_cl", "Depth pre-pass (cluster LOD)"}, {"c_cl", "Cluster LOD selection"}, {"c_cluster", "Cluster LOD"},
        // geometry
        {"g_gbuf", "Opaque geometry"}, {"g_pass1", "Opaque geometry"}, {"g_passes", "Opaque geometry"},
        {"g_prepass", "Depth pre-pass"}, {"g_pre_static", "Depth pre-pass"}, {"g_term", "Terrain"},
        {"c_cull", "Visibility culling"}, {"c_hiz", "Occlusion depth pyramid"}, {"c_occl", "Occlusion culling"},
        {"g_mirror_occl", "Mirror: occlusion"}, {"g_occl", "Occlusion culling"}, {"c_stream", "Scene updates (upload)"},
        {"c_vrs", "Variable-rate shading map"}, {"g_sky", "Sky"}, {"c_sky", "Sky"},
        // shadows
        {"c_projshadow", "Projector shadows"}, {"c_proj_", "Projector shadows"}, {"g_proj_", "Projector shadows"},
        {"c_sun_", "Sun shadow maps"}, {"g_sun_", "Sun shadow maps"}, {"c_shadow", "Sun shadow maps"},
        // ray tracing
        {"c_rtmask", "Ray-traced shadows"}, {"g_rtmask", "Ray-traced shadows"},
        {"c_blas", "Ray tracing: object updates"}, {"c_tlas", "Ray tracing: scene update"},
        {"q_rt_build", "Ray tracing: scene build"},
        {"c_defer_rtrefl", "Ray-traced reflections"}, {"g_defer_rtrefl", "Ray-traced reflections"},
        {"c_gi", "Ray-traced global illumination"}, {"g_gi", "Ray-traced global illumination"},
        {"c_audio", "Audio ray tracing"},
        // reflections
        {"pr_rt", "Reflection probes (ray traced)"}, {"pr_", "Reflection probe capture"},
        {"p_probe", "Reflection probe filtering"}, {"c_prb", "Reflection probe scheduling"},
        {"c_mirror", "Mirror"}, {"g_mirror", "Mirror"},
        {"c_defer_refl", "Reflections"}, {"g_defer_refl", "Reflections"},
        // lighting
        {"c_light_regir", "Light sampling (ReGIR)"}, {"c_light", "Light culling"}, {"g_light_grid", "Light grid"},
        {"g_lit", "Lighting"}, {"g_defer", "Lighting (deferred)"},
        {"g_rtao", "Ray-traced ambient occlusion"}, {"g_ao", "Ambient occlusion"}, {"g_ssao", "Ambient occlusion"},
        {"g_gtao", "Ambient occlusion"}, {"g_cel", "Toon outlines"}, {"c_cel", "Toon outlines"},
        // transparency, particles, volumetrics
        {"c_pfog", "Particle fog"}, {"g_pfog", "Particle fog"},
        {"c_part", "Particles"}, {"g_p2_part", "Particles"}, {"g_p2_particles", "Particles"},
        {"g_p2_water", "Water"}, {"g_p2", "Transparent objects"}, {"g_pass2", "Transparent objects"},
        {"c_alpha_sort", "Transparency sorting"}, {"g_alpha_depth", "Transparency depth"},
        {"c_vcloud", "Volumetric clouds"}, {"g_vcloud", "Volumetric clouds"},
        {"c_vol", "Volumetric fog / light"}, {"g_vol", "Volumetric fog / light"}, {"q_volumetrics", "Volumetric fog / light"},
        {"c_godrays", "God rays"}, {"g_godrays", "God rays"},
        // upscaling, post
        {"p_dlssrr", "DLSS Ray Reconstruction"}, {"p_dlsssr", "DLSS Super Resolution"}, {"p_fsr", "FSR upscaling"},
        {"p_taa", "Anti-aliasing (TAA)"}, {"p_aa", "Anti-aliasing"},
        {"p_bloom", "Glow"}, {"g_to_glow", "Glow"}, {"p_dof", "Depth of field"}, {"p_mblur", "Motion blur"},
        {"p_fx", "Effects"}, {"p_final", "Tonemap + final composite"}, {"p_prelude", "Post-processing setup"},
        {"p_", "Post-processing"},
        // interface
        {"g_hud", "HUD"}, {"c_hud", "HUD"}, {"g_sel", "Selection highlight"}, {"g_overlay", "Overlays"},
        {"c_readback", "GPU readback copy"},
    };
    for (const auto& m : kMap)
        if (!std::strncmp(mark, m.p, std::strlen(m.p))) return m.n;
    const int c = LLVKAutoTune::catOf(mark);
    return c >= 0 ? LLVKAutoTune::catName((uint32_t)c) : std::string(mark);
}

const char* LLVKPerfCapture::cpuSectionName(const char* s)
{
    if (!s) return "";
    static const struct { const char* n; const char* p; } kMap[] = {
        {"display_total", "Rendering"}, {"renderFrame", "Vulkan frame recording + submit"},
        {"buildWorldFrame", "Scene build for the GPU"}, {"ui_draw", "Interface drawing"},
        {"LL_idle_total", "World update"}, {"LL_mainloopWork", "Finished background work"},
        {"LL_idleNetwork", "Network messages"}, {"LL_idleCallbacks", "Idle callbacks"}, {"LL_updateUI", "Interface update"},
        {"LL_objectListUpdate", "Object updates"}, {"LL_updateParticles", "Particle update"},
        {"LL_updateCamera", "Camera update"}, {"LL_audio", "Audio"}, {"LL_updateRegions", "Region update"},
        {"LL_updateMove", "Avatar movement"}, {"LL_updateVisibilities", "Visibility update"},
        {"LL_agentPosition", "Own avatar update"}, {"LL_cleanDead", "Removing objects"},
        {"LL_texUpdateClass", "Texture updates"}, {"LL_updateImages", "Texture updates"},
        {"LL_selectEffects", "Selection effects"}, {"LL_apparentAngles", "Object detail levels"},
        {"vocache_completions", "Object cache"}, {"vk_voCacheCull", "Object cache"},
        {"df_meshRepo", "Mesh loading"}, {"df_textureThreads", "Texture threads"}, {"df_lfs", "File loading"},
        {"df_nativeEvents", "Window events"}, {"df_gatherInput", "Input"}, {"df_scanInput", "Input"},
        {"df_coroSuspend", "Coroutines"}, {"df_mainloopPost", "Main loop"}, {"df_statsRecorder", "Statistics"},
        {"vk_ingestTick", "New object processing"}, {"vk_syncFromStores", "Scene sync to the GPU"},
        {"vk_hudText", "Name tags / hover text"}, {"vk_dynamicTextures", "Media / dynamic textures"},
        {"vk_avatarVis", "Avatar visibility"}, {"vk_worldOverlays", "World overlays"},
        {"vk_hudEffects", "HUD effects"}, {"vk_performPick", "Mouse picking"}, {"vk_partitionQ", "Spatial partition"},
        {"vk_sculptFetch", "Sculpt map loading"}, {"vk_checkSettings", "Settings check"},
        {"bwf_lights", "Light collection"}, {"bwf_probes", "Reflection probe setup"},
    };
    for (const auto& m : kMap)
        if (!std::strcmp(s, m.n)) return m.p;
    return "";
}

// ============================================================================
// Activation
// ============================================================================

void LLVKPerfCapture::requestLive()
{
    if (!active()) activate();
    mLive = true;
    mLiveUntil = std::max(mLiveUntil, wallNow() + kLiveHoldSec);
    mAny.store(true, std::memory_order_relaxed);
    LLVKAutoTune::instance().requestPassTimers();
}

void LLVKPerfCapture::activate()
{
    mEmaFrame = mEmaCpu = mEmaWait = mEmaPresent = mEmaSleep = 0.f;
    mRecent.clear();
    std::fill(mCpuEma, mCpuEma + kSlots, 0.f);
    std::lock_guard<std::mutex> lk(mGpuMutex);
    for (Pass& p : mPasses) { p.ema = 0.f; p.pending = 0.0; }
    mGpuEma = 0.f;
    mGpuLiveFrames = mGpuSubFrames = 0;
}

void LLVKPerfCapture::startCapture(U32 seconds, bool to_clipboard)
{
    if (mPhase != PH_IDLE) return;
    if (!active()) activate();
    mCapSeconds = llclamp(seconds, kMinSeconds, kMaxSeconds);
    mCapClipboard = to_clipboard;
    mPhase = PH_WARMUP;
    mPhaseEnd = wallNow() + kWarmupSec;
    mAny.store(true, std::memory_order_relaxed);
    LLVKAutoTune::instance().requestPassTimers();
    LL_INFOS("PerfCapture") << "performance capture: " << mCapSeconds << " s" << LL_ENDL;
}

void LLVKPerfCapture::cancelCapture()
{
    if (mPhase == PH_IDLE) return;
    mPhase = PH_IDLE;
    {
        std::lock_guard<std::mutex> lk(mGpuMutex);
        mGpuCollect = false;
    }
    mDone();
}

F32 LLVKPerfCapture::captureRemaining() const
{
    if (mPhase == PH_IDLE) return 0.f;
    const double left = mPhaseEnd - wallNow() + (mPhase == PH_WARMUP ? (double)mCapSeconds : 0.0);
    return (F32)std::max(0.0, left);
}

// ============================================================================
// Per frame
// ============================================================================

int LLVKPerfCapture::parentOf(int i)
{
    if (!mParentInit)
    {
        std::fill(mParent, mParent + kSlots, (int16_t)-2);
        mParentInit = true;
    }
    if (i < 0 || i >= kSlots) return -1;
    if (mParent[i] != -2) return mParent[i];
    const LLVKHitch::Slot* slots = LLVKHitch::state().slots;
    const char* p = slots[i].parent;
    if (!p) return -1;   // (not cached: a later nested call may still name a parent)
    for (int j = 0; j < kSlots && slots[j].name; ++j)
    {
        if (slots[j].name == p && j != i)
        {
            mParent[i] = (int16_t)j;
            return j;
        }
    }
    return -1;
}

void LLVKPerfCapture::frameTick()
{
    const double now = wallNow();
    const double dt_ms = mLastFrameAt > 0.0 ? (now - mLastFrameAt) * 1000.0 : 0.0;
    mLastFrameAt = now;

    if (mLive && now > mLiveUntil) mLive = false;
    if (mPhase == PH_WARMUP && now >= mPhaseEnd)
    {
        mPhase = PH_COLLECT;
        mCapStart = now;
        mPhaseEnd = now + (double)mCapSeconds;
        mCapWallStart = LLDate::now().secondsSinceEpoch();
        mCapFrames.clear();
        mCapFrames.reserve((size_t)mCapSeconds * 165);
        for (int i = 0; i < kSlots; ++i) { mCpuCap[i].clear(); mCpuCapSum[i] = 0.0; }
        mCapExcluded = mCapBackground = 0;
        std::lock_guard<std::mutex> lk(mGpuMutex);
        for (Pass& p : mPasses) { p.cap.clear(); p.capSum = 0.0; }
        mCapGpu.clear();
        mCapGpuSubFrames = 0;
        mGpuCollect = true;
    }
    else if (mPhase == PH_COLLECT && now >= mPhaseEnd)
    {
        finishCapture();
    }
    const bool act = active();
    mAny.store(act, std::memory_order_relaxed);
    if (!act) return;
    // Keep the per-pass GPU timers on while measuring (the window's draw asks too).
    LLVKAutoTune::instance().requestPassTimers();
    if (!(dt_ms > 0.0 && dt_ms < 5000.0)) return;

    // ---- the frame that just ended: its split (the hitch slots, before frameBoundary) ----
    const LLVKHitch::Slot* slots = LLVKHitch::state().slots;
    int n = 0;
    double wait = 0.0, present = 0.0, sleep = 0.0;
    for (; n < kSlots && slots[n].name; ++n)
    {
        const LLVKHitch::Slot& sl = slots[n];
        if (!sl.timed) continue;
        if (isGpuWait(sl.name)) wait += sl.ms;
        else if (isPresent(sl.name)) present += sl.ms;
        else if (isCapSleep(sl.name)) sleep += sl.ms;
    }
    const float cpu = (float)std::max(0.0, dt_ms - wait - present - sleep);
    const bool first = mEmaFrame <= 0.f;
    auto ema = [first](float& e, float v) { e = first ? v : e * 0.95f + v * 0.05f; };
    ema(mEmaFrame, (float)dt_ms);
    ema(mEmaCpu, cpu);
    ema(mEmaWait, (float)wait);
    ema(mEmaPresent, (float)present);
    ema(mEmaSleep, (float)sleep);
    mRecent.emplace_back(now, (float)dt_ms);
    while (!mRecent.empty() && now - mRecent.front().first > kRecentSec) mRecent.pop_front();

    // ---- CPU sections: busy time per slot (waits / present / the cap sleep taken out of
    // every section that encloses them) ----
    float busy[kSlots];
    for (int i = 0; i < n; ++i)
        busy[i] = (slots[i].timed && !isGpuSlot(slots[i].name)) ? (float)slots[i].ms : 0.f;
    for (int i = 0; i < n; ++i)
    {
        const char* nm = slots[i].name;
        if (!slots[i].timed || !(isGpuWait(nm) || isPresent(nm) || isCapSleep(nm))) continue;
        const float w = busy[i];
        busy[i] = 0.f;
        int p = parentOf(i);
        for (int d = 0; p >= 0 && p < n && d < kMaxDepth; ++d, p = parentOf(p))
            busy[p] = std::max(0.f, busy[p] - w);
    }
    for (int i = 0; i < n; ++i) mCpuEma[i] = first ? busy[i] : mCpuEma[i] * 0.95f + busy[i] * 0.05f;

    // ---- capture ----
    if (mPhase != PH_COLLECT) return;
    const bool minimized = gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getWindow()->getMinimized();
    if (minimized || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        ++mCapExcluded;   // not this scene's frames
        return;
    }
    if (!gFocusMgr.getAppHasFocus()) ++mCapBackground;
    if (mCapFrames.size() >= kMaxFrames) return;
    mCapFrames.push_back({(float)dt_ms, cpu, (float)wait, (float)present, (float)sleep});
    for (int i = 0; i < n; ++i)
    {
        if (busy[i] < 0.0005f) continue;
        mCpuCap[i].push_back(busy[i]);
        mCpuCapSum[i] += busy[i];
    }
}

// ============================================================================
// GPU timer harvest (any thread)
// ============================================================================

LLVKPerfCapture::Pass* LLVKPerfCapture::passFor(const char* name, bool async)
{
    // Marks arrive in the same order every frame: try the one after the last match first.
    const size_t n = mPasses.size();
    for (size_t k = 0; k < n; ++k)
    {
        Pass& p = mPasses[(mHint + k) % n];
        if (p.async == async && p.name == name)
        {
            mHint = (mHint + k + 1) % n;
            return &p;
        }
    }
    if (n >= kMaxPasses) return nullptr;
    mPasses.emplace_back();
    Pass& p = mPasses.back();
    p.name = name;
    p.async = async;
    mHint = 0;
    return &p;
}

void LLVKPerfCapture::gpuSubSample(const char* name, double ms, bool async)
{
    LLVKPerfCapture& s = instance();
    if (!s.mAny.load(std::memory_order_relaxed) || !name) return;
    // Not costs: waits, begins, the async queue's segment spans (their marks are listed)
    // -- except the async queue's busy total.
    if (LLVKAutoTune::catOf(name) < 0 && std::strcmp(name, "q_async_busy") != 0) return;
    if (!(ms > 0.0) || ms > 1000.0) return;
    std::lock_guard<std::mutex> lk(s.mGpuMutex);
    if (Pass* p = s.passFor(name, async)) p->pending += ms;
}

void LLVKPerfCapture::gpuFrameDone(double total_ms)
{
    LLVKPerfCapture& s = instance();
    if (!s.mAny.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(s.mGpuMutex);
    bool sub = false;
    for (const Pass& p : s.mPasses) sub = sub || p.pending > 0.0;
    const bool valid = total_ms > 0.0 && total_ms < 1000.0;
    if (sub)   // lean frames (RenderVKGpuSubMarks off) carry no sub-marks: keep the pass values
    {
        const bool first = s.mGpuSubFrames == 0;
        for (Pass& p : s.mPasses) p.ema = first ? (float)p.pending : p.ema * 0.9f + (float)p.pending * 0.1f;
        ++s.mGpuSubFrames;
    }
    if (s.mGpuCollect && s.mCapGpu.size() < kMaxFrames)
    {
        if (valid) s.mCapGpu.push_back((float)total_ms);
        if (sub)
        {
            ++s.mCapGpuSubFrames;
            for (Pass& p : s.mPasses)
            {
                if (p.pending <= 0.0) continue;
                p.cap.push_back((float)p.pending);
                p.capSum += p.pending;
            }
        }
    }
    for (Pass& p : s.mPasses) p.pending = 0.0;
    if (valid)
    {
        s.mGpuEma = s.mGpuLiveFrames == 0 ? (float)total_ms : s.mGpuEma * 0.9f + (float)total_ms * 0.1f;
        ++s.mGpuLiveFrames;
        s.mGpuAt = wallNow();
    }
}

// ============================================================================
// Text
// ============================================================================

std::string LLVKPerfCapture::limitText(float frame, float cpu, float gpu, float wait) const
{
    // (LLVKAutoTune::breakdownText's rule, plus the main thread's own wait share.)
    const float target_ms = 1000.f / (float)LLVKAutoTune::effectiveTargetFPS();
    if (frame <= 0.f) return "unknown";
    if (gpu > 0.f && gpu >= 0.85f * frame) return "the graphics card (GPU)";
    if (frame <= target_ms * 1.03f && std::max(cpu, gpu) < frame * 0.8f) return "the frame rate cap / vsync";
    if (wait >= 0.25f * frame && gpu <= 0.f) return "probably the graphics card (GPU time unknown)";
    return "the main thread (CPU)";
}

std::string LLVKPerfCapture::statusLines() const
{
    std::ostringstream os;
    const LLVKAutoTuneSample& smp = LLVKAutoTune::instance().sample();
    static LLCachedControl<U32> s_ups(gSavedSettings, "RenderVKUpscaler", 0);
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKRenderScale", 1.f);
    static LLCachedControl<U32> s_quality(gSavedSettings, "RenderQualityPerformance", 1);
    const U32 ups = s_ups;
    os << "Upscaler: " << upscalerSettingName(ups);
    if (ups == 6u) os << llformat(" (%.0f%%)", (F32)s_scale * 100.f);
    if (smp.valid) os << ", running: " << upscalerRanName(smp.upscalerRan);
    const S32 ww = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0;
    const S32 wh = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0;
    if (smp.valid && smp.renderW && smp.renderH && ww > 0 && wh > 0)
    {
        os << llformat("\nRender scale: %ux%u of %dx%d (%.0f%%)", smp.renderW, smp.renderH, ww, wh,
                       100.0 * (double)smp.renderW / (double)ww);
        if (smp.vram.renderScaleMax < 0.999f) os << llformat(", capped at %.0f%% to save video memory", smp.vram.renderScaleMax * 100.f);
    }
    os << "\nQuality: graphics quality " << (U32)s_quality << " of 7, GPU class "
       << (S32)LLFeatureManager::getInstance()->getGPUClass();
    // Video memory: the VRAM ladder (pressure lowers effective caps, settings untouched).
    if (smp.valid && smp.vramBudget > 0)
    {
        os << "\nVideo memory: " << gb(smp.vramUsage) << " of " << gb(smp.vramBudget) << " GB, ";
        if (smp.vramMode == 0u && smp.vramRungs == 0u) os << "no pressure";
        else os << (smp.vramMode >= 2u ? "inside the reserve" : "under pressure") << ", " << smp.vramRungs
                << " memory saving step(s) active";
    }
    // System memory: the memory governor's state.
    {
        os << "\nSystem memory: ";
        U64 total = 0, avail = 0;
        if (LLMemoryGovernor::isEnabled())
        {
            os << LLMemoryGovernor::stateName(LLMemoryGovernor::getState());
            total = LLMemoryGovernor::getTotalRAM();
            avail = LLMemoryGovernor::getAvailBytes();
        }
        else
        {
            LLMemoryGovernor::SystemMemory sm;
            if (LLMemoryGovernor::readSystemMemory(sm)) { total = sm.total; avail = sm.avail; }
            os << "governor off";
        }
        if (total) os << ", " << gb(avail) << " GB free of " << gb(total) << " GB";
        const U64 rss = LLMemory::getCurrentRSS();
        if (rss) os << ", viewer uses " << gb(rss) << " GB";
    }
    const std::string tune = LLVKAutoTune::instance().statusText();
    if (!tune.empty()) os << "\nAuto-adjust: " << tune;
    return os.str();
}

std::string LLVKPerfCapture::liveText() const
{
    if (mEmaFrame <= 0.f) return "Measuring...";
    std::ostringstream os;
    std::vector<float> recent;
    recent.reserve(mRecent.size());
    double sum = 0.0;
    for (const auto& r : mRecent) { recent.push_back(r.second); sum += r.second; }
    const float fps = sum > 0.0 ? (float)(1000.0 * (double)recent.size() / sum) : 0.f;
    float gpu = 0.f;
    uint32_t sub_frames = 0;
    struct Row { float ms; std::string plain; std::string raw; bool async; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(mGpuMutex);
        if (mGpuLiveFrames > 0 && wallNow() - mGpuAt < 3.0) gpu = mGpuEma;
        sub_frames = mGpuSubFrames;
        for (const Pass& p : mPasses)
            if (p.ema >= 0.02f) rows.push_back({p.ema, gpuPassName(p.name.c_str()), p.name, p.async});
    }
    if (gpu <= 0.f) gpu = LLVKAutoTune::instance().gpuMs();   // (the always-on sampled span)
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

    os << llformat("Frame %.1f ms, %.0f fps, 1%% low %.0f fps (last %.0f s)\n", mEmaFrame, fps,
                   onePercentLowFps(recent), kRecentSec);
    os << llformat("  Main thread (CPU)          %6.2f ms\n", mEmaCpu);
    os << (gpu > 0.f ? llformat("  Graphics card (GPU)        %6.2f ms\n", gpu)
                     : std::string("  Graphics card (GPU)             ? (timers unavailable)\n"));
    os << llformat("  Waiting: GPU/vsync/pacing  %6.2f ms\n", mEmaWait);
    os << llformat("  Present                    %6.2f ms\n", mEmaPresent);
    if (mEmaSleep >= 0.05f) os << llformat("  Frame rate limit           %6.2f ms\n", mEmaSleep);
    os << "Limited by: " << limitText(mEmaFrame, mEmaCpu, gpu, mEmaWait) << "\n\n";
    os << statusLines() << "\n\n";

    os << "GPU passes, ms (passes overlap: they need not add up to the GPU total)\n";
    if (rows.empty())
        os << (sub_frames == 0 ? "  collecting... (no pass timings: RenderVKGpuSubMarks off, or no GPU timers)\n"
                               : "  collecting...\n");
    int shown = 0;
    for (const Row& r : rows)
    {
        if (r.async || shown >= 16) continue;
        os << llformat("  %6.2f  ", r.ms) << fit(r.plain, 34) << " " << r.raw << "\n";
        ++shown;
    }
    shown = 0;
    for (const Row& r : rows)
    {
        if (!r.async || shown >= 6) continue;
        if (!shown) os << "Async compute queue, ms (its own clock; runs beside the passes above)\n";
        os << llformat("  %6.2f  ", r.ms) << fit(r.plain, 34) << " " << r.raw << "\n";
        ++shown;
    }

    // CPU section tree (EMA), roots by cost, children indented.
    os << "\nMain thread sections, ms (a section includes the ones under it; waits taken out)\n";
    const LLVKHitch::Slot* slots = LLVKHitch::state().slots;
    int n = 0;
    while (n < kSlots && slots[n].name) ++n;
    LLVKPerfCapture* self = const_cast<LLVKPerfCapture*>(this);   // (parent cache only)
    std::vector<int> par(n);
    for (int i = 0; i < n; ++i) par[i] = self->parentOf(i);
    int lines = 0;
    std::function<void(int, int)> walk = [&](int p, int depth)
    {
        std::vector<int> kids;
        for (int i = 0; i < n; ++i)
            if (par[i] == p && mCpuEma[i] >= (depth ? 0.1f : 0.05f)) kids.push_back(i);
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return mCpuEma[a] > mCpuEma[b]; });
        int k = 0;
        for (int i : kids)
        {
            if (lines >= 28 || k >= (depth ? 4 : 10)) break;
            const char* plain = cpuSectionName(slots[i].name);
            const std::string label = std::string(2 * depth, ' ') + (*plain ? plain : slots[i].name);
            os << llformat("  %6.2f  ", mCpuEma[i]) << fit(label, 34) << " " << (*plain ? slots[i].name : "") << "\n";
            ++lines;
            ++k;
            if (depth < 3) walk(i, depth + 1);
        }
    };
    walk(-1, 0);
    return os.str();
}

std::string LLVKPerfCapture::headerText() const
{
    std::ostringstream os;
    const LLSD info = LLAppViewer::instance()->getViewerInfo();
    // Only hardware / version keys (getViewerInfo also carries the region and position).
    // ("version" before dotted numbers: the scrubber keeps a dotted quad after a version cue.)
    os << "Viewer: " << info["CHANNEL"].asString() << ", version " << info["VIEWER_VERSION_STR"].asString();
    if (info.has("BUILD_CONFIG")) os << ", " << info["BUILD_CONFIG"].asString();
    os << "\nOS: " << info["OS_VERSION"].asString();
    os << "\nCPU: " << info["CPU"].asString();
    if (info.has("CONCURRENCY")) os << " (" << info["CONCURRENCY"].asInteger() << " threads)";
    if (info.has("MEMORY_MB")) os << ", RAM " << info["MEMORY_MB"].asInteger() << " MB";
    os << "\nGPU: " << info["GRAPHICS_CARD"].asString();
    std::string driver = info["GRAPHICS_DRIVER_VERSION"].asString();
    if (driver.empty()) driver = LLVulkanContext::instance().driverVersionText();
    if (!driver.empty()) os << ", driver version " << driver;
    if (info.has("VULKAN_VERSION"))
        os << "\nVulkan: " << info["VULKAN_VERSION"].asString() << "; ray tracing: " << info["RAY_TRACING"].asString();
    os << "\nDisplay: " << LLVulkanContext::instance().displayInfoText();
    os << "\nSettings:";
    size_t col = 9;
    for (const char* name : kCaptureSettings)
    {
        LLControlVariable* c = gSavedSettings.getControl(name);
        if (!c) continue;
        const LLSD v = c->getValue();
        const std::string item = std::string(" ") + name + "="
            + (v.isBoolean() ? (v.asBoolean() ? "1" : "0") : v.isReal() ? llformat("%g", v.asReal()) : v.asString());
        if (col + item.size() > 100) { os << "\n         "; col = 9; }
        os << item;
        col += item.size();
    }
    const std::string non_default = nonDefaultRenderSettings(24);
    os << "\nNon-default render settings: " << (non_default.empty() ? "none" : non_default);
    return os.str();
}

std::string LLVKPerfCapture::captureText() const
{
    std::ostringstream os;
    const size_t nf = mCapFrames.size();
    std::vector<float> fr(nf), cp(nf), wt(nf), pr(nf), sl(nf);
    double fsum = 0.0, csum = 0.0, wsum = 0.0, psum = 0.0, ssum = 0.0;
    for (size_t i = 0; i < nf; ++i)
    {
        const FrameRec& f = mCapFrames[i];
        fr[i] = f.frame; cp[i] = f.cpu; wt[i] = f.wait; pr[i] = f.present; sl[i] = f.sleep;
        fsum += f.frame; csum += f.cpu; wsum += f.wait; psum += f.present; ssum += f.sleep;
    }
    const double k = nf ? 1.0 / (double)nf : 0.0;
    const float fps = fsum > 0.0 ? (float)(1000.0 * (double)nf / fsum) : 0.f;
    const float low = onePercentLowFps(fr);

    os << "Slipstream performance capture (format 1)\n";
    os << "When: " << LLDate(mCapWallStart).toHTTPDateString(std::string("%Y-%m-%d %H:%M:%S UTC"))
       << ", " << mCapSeconds << " s, " << nf << " frames";
    if (mCapExcluded) os << " (" << mCapExcluded << " excluded: teleport / minimized)";
    if (mCapBackground && nf) os << llformat("; window in the background for %.0f%% of the frames", 100.0 * mCapBackground / (double)nf);
    os << "\n" << headerText() << "\n" << statusLines() << "\n\n";

    os << llformat("FPS: average %.1f, 1%% low %.1f (the average of the slowest 1%% of frames)\n", fps, low);
    os << "FRAME, ms                        avg     p50     p95     p99   worst\n";
    auto line = [&](const char* label, const std::vector<float>& v, double s)
    {
        std::vector<float> t = v;
        const float worst = t.empty() ? 0.f : *std::max_element(t.begin(), t.end());
        const float p50 = LLVKHitch::percentile(t, 0.50), p95 = LLVKHitch::percentile(t, 0.95),
                    p99 = LLVKHitch::percentile(t, 0.99);
        os << "  " << fit(label, 28) << llformat("%7.2f %7.2f %7.2f %7.2f %7.2f\n", s * k, p50, p95, p99, worst);
    };
    line("frame", fr, fsum);
    line("main thread (CPU)", cp, csum);
    line("waiting: GPU / vsync / pacing", wt, wsum);
    line("present", pr, psum);
    if (ssum > 0.0) line("frame rate limit sleep", sl, ssum);

    // GPU: frame totals, then the passes (graphics queue, then the async queue).
    float gpu_avg = 0.f;
    {
        std::lock_guard<std::mutex> lk(mGpuMutex);
        const size_t ng = mCapGpu.size();
        if (ng)
        {
            double g = 0.0;
            for (float v : mCapGpu) g += v;
            gpu_avg = (float)(g / (double)ng);
            std::vector<float> t = mCapGpu;
            const float worst = *std::max_element(t.begin(), t.end());
            os << "  " << fit(llformat("GPU frame total (%u frames)", (unsigned)ng), 28)
               << llformat("%7.2f %7.2f %7.2f %7.2f %7.2f\n", gpu_avg, LLVKHitch::percentile(t, 0.50),
                           LLVKHitch::percentile(t, 0.95), LLVKHitch::percentile(t, 0.99), worst);
        }
        else
            os << "  GPU frame total: no GPU timer results (timers unavailable on this device?)\n";
        os << "Limited by: " << limitText((float)(fsum * k), (float)(csum * k), gpu_avg, (float)(wsum * k)) << "\n\n";

        const uint32_t sf = mCapGpuSubFrames;
        for (int q = 0; q < 2; ++q)
        {
            std::vector<const Pass*> ps;
            for (const Pass& p : mPasses)
                if (p.async == (q == 1) && sf && p.capSum / sf >= 0.005) ps.push_back(&p);
            std::sort(ps.begin(), ps.end(), [](const Pass* a, const Pass* b) { return a->capSum > b->capSum; });
            if (q == 0)
            {
                os << "GPU PASSES, graphics queue, ms per frame over " << sf << " frames"
                   << " (passes overlap: they need not add up to the GPU total)\n";
                if (!sf) os << "  none: no pass timings (RenderVKGpuSubMarks off, or no GPU timers)\n";
            }
            else if (!ps.empty())
                os << "ASYNC COMPUTE QUEUE, ms per frame (its own clock; overlaps the graphics queue)\n";
            if (!ps.empty()) os << "      avg     p50     p95     p99  pass\n";
            size_t shown = 0;
            for (const Pass* p : ps)
            {
                if (shown >= (q ? 20u : 45u)) break;
                os << llformat("  %7.3f %7.3f %7.3f %7.3f  ", p->capSum / sf, pctZeros(p->cap, sf, 0.50),
                               pctZeros(p->cap, sf, 0.95), pctZeros(p->cap, sf, 0.99))
                   << gpuPassName(p->name.c_str()) << " (" << p->name << ")\n";
                ++shown;
            }
            if (ps.size() > shown) os << "  (" << ps.size() - shown << " more under " << llformat("%.3f", ps[shown]->capSum / sf) << " ms)\n";
        }
        // Per category (the auto-tuner's groups), graphics queue.
        if (sf)
        {
            double cat[LLVKAutoTune::CAT_COUNT] = {};
            for (const Pass& p : mPasses)
            {
                const int c = LLVKAutoTune::catOf(p.name.c_str());
                if (!p.async && c >= 0 && c < LLVKAutoTune::CAT_COUNT) cat[c] += p.capSum / sf;
            }
            uint32_t order[LLVKAutoTune::CAT_COUNT];
            for (uint32_t i = 0; i < LLVKAutoTune::CAT_COUNT; ++i) order[i] = i;
            std::sort(order, order + LLVKAutoTune::CAT_COUNT, [&](uint32_t a, uint32_t b) { return cat[a] > cat[b]; });
            os << "By category:";
            for (uint32_t c : order)
                if (cat[c] >= 0.01) os << " " << LLVKAutoTune::catName(c) << " " << llformat("%.2f", cat[c]) << ";";
            os << "\n";
        }
    }

    // CPU section tree, avg / p95 / p99 (frames where a section did not run count as 0).
    os << "\nMAIN THREAD SECTIONS, ms per frame (a section includes the ones indented under it;"
          " GPU / pacing waits, present and the frame rate limit taken out)\n";
    os << "      avg     p50     p95     p99  section\n";
    const LLVKHitch::Slot* slots = LLVKHitch::state().slots;
    int n = 0;
    while (n < kSlots && slots[n].name) ++n;
    LLVKPerfCapture* self = const_cast<LLVKPerfCapture*>(this);   // (parent cache only)
    std::vector<int> par(n);
    for (int i = 0; i < n; ++i) par[i] = self->parentOf(i);
    int lines = 0;
    std::function<void(int, int)> walk = [&](int p, int depth)
    {
        std::vector<int> kids;
        for (int i = 0; i < n; ++i)
            if (par[i] == p && nf && mCpuCapSum[i] * k >= (depth ? 0.05 : 0.02)) kids.push_back(i);
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return mCpuCapSum[a] > mCpuCapSum[b]; });
        int shown = 0;
        for (int i : kids)
        {
            if (lines >= 90 || shown >= (depth ? 8 : 20)) break;
            const char* plain = cpuSectionName(slots[i].name);
            os << llformat("  %7.3f %7.3f %7.3f %7.3f  ", mCpuCapSum[i] * k, pctZeros(mCpuCap[i], nf, 0.50),
                           pctZeros(mCpuCap[i], nf, 0.95), pctZeros(mCpuCap[i], nf, 0.99))
               << std::string(2 * depth, ' ') << (*plain ? std::string(plain) + " (" + slots[i].name + ")"
                                                         : std::string(slots[i].name)) << "\n";
            ++lines;
            ++shown;
            if (depth < 4) walk(i, depth + 1);
        }
    };
    walk(-1, 0);
    return os.str();
}

// ============================================================================
// Finish / save
// ============================================================================

void LLVKPerfCapture::finishCapture()
{
    {
        std::lock_guard<std::mutex> lk(mGpuMutex);
        mGpuCollect = false;
    }
    mPhase = PH_IDLE;
    // No names / positions are written, but the text still goes through the bug report's
    // scrubber (names of nearby avatars, friends and self, UUIDs on auth lines, addresses ...).
    const std::string text = LLFloaterReportBug::scrubText(captureText());
    mLastText = text;
    mLastAt = LLTimer::getTotalSeconds();
    {
        double fsum = 0.0;
        std::vector<float> fr;
        fr.reserve(mCapFrames.size());
        for (const FrameRec& f : mCapFrames) { fsum += f.frame; fr.push_back(f.frame); }
        const float fps = fsum > 0.0 ? (float)(1000.0 * (double)fr.size() / fsum) : 0.f;
        mLastSummary = llformat("%u s, average %.0f fps, 1%% low %.0f fps", mCapSeconds, fps, onePercentLowFps(fr));
    }
    saveCapture(text);
    if (mCapClipboard) copyLastToClipboard();
    LL_INFOS("PerfCapture") << "performance capture done: " << mLastSummary << " -> " << mLastFile << LL_ENDL;
    // Free the capture buffers (bounded anyway; nothing kept but the text).
    mCapFrames.clear();
    mCapFrames.shrink_to_fit();
    for (int i = 0; i < kSlots; ++i) { std::vector<float>().swap(mCpuCap[i]); mCpuCapSum[i] = 0.0; }
    {
        std::lock_guard<std::mutex> lk(mGpuMutex);
        for (Pass& p : mPasses) std::vector<float>().swap(p.cap);
        std::vector<float>().swap(mCapGpu);
    }
    mDone();
}

void LLVKPerfCapture::saveCapture(const std::string& text)
{
    const std::string stamp = LLDate::now().toHTTPDateString(std::string("%Y%m%d-%H%M%S"));
    const std::string name = "slipstream-perf-capture-" + stamp + ".txt";
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
    for (const std::string& p : { path, gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "slipstream-perf-capture-last.txt") })
    {
        llofstream out(p.c_str(), std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            LL_WARNS("PerfCapture") << "could not write " << p << LL_ENDL;
            continue;
        }
        out << text;
    }
    mLastFile = path;
    // Keep the newest kMaxFiles timestamped captures (names sort by time).
    std::vector<std::string> kept;
    LLDirIterator iter(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ""), "slipstream-perf-capture-2*.txt");
    std::string found;
    while (iter.next(found)) kept.push_back(found);
    std::sort(kept.begin(), kept.end());
    for (size_t i = 0; i + kMaxFiles < kept.size(); ++i)
        LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, kept[i]), ENOENT);
}

void LLVKPerfCapture::copyLastToClipboard() const
{
    if (mLastText.empty()) return;
    const LLWString w = utf8str_to_wstring(mLastText);
    LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
}
