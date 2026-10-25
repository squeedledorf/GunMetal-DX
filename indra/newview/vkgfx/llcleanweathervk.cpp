/**
 * @file llcleanweathervk.cpp
 * @brief Weather rendering (#783): see llcleanweathervk.h and the shaders clean_weather_*.
 */
#include "linden_common.h"

#include "llcleanweathervk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llcleanskinstore.h"   // M3: the avatars' joint-table slots
#include "llagent.h"            // #783 the snow dump's window (the camera's focus)
#include "llagentcamera.h"
#include "lldir.h"              // #783 the snow dump's file (logs/)
#include "llerror.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llworld.h"            // the ground under the camera (the maps' centring)
#include "llviewerregion.h"     // #783 the snow dump's water height
#include "llvoavatar.h"
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

namespace
{
// Particle budget per RenderVKWeatherQuality (0 / 1 / 2).
constexpr uint32_t kCaps[3] = {16384u, 32768u, 65536u};
// The occlusion map: extent = this x the precipitation radius (clamped), centre snapped to an
// eighth of it (a whole number of texels at kOcclRes), height snapped to kOcclSnapZ; the
// ortho box spans kOcclAbove over to kOcclBelow under the snapped height.
constexpr float  kOcclExtentPerRadius = 4.f;
constexpr double kOcclSnapZ = 32.0;
constexpr float  kOcclAbove = 128.f;
constexpr float  kOcclBelow = 64.f;
// A surface / drop within this of the map's top surface counts as that surface (texel
// footprint on slopes, the casters' depth bias). The near cascade's texels are ~12x finer
// and its readers correct for the receiver's slope (clean_weather_surface.glsl): a few
// texels, at least kNearBiasMin.
constexpr float  kCoverBias = 0.4f;
constexpr float  kNearBiasTexels = 3.f;
constexpr float  kNearBiasMin = 0.08f;
// The surfaces' PCF kernel radius (m; clean_weather_surface.glsl): at least this, at least
// kPcfTexels of the cascade read -- the drip line is soft at both resolutions.
constexpr float  kPcfRadius = 0.12f;
constexpr float  kPcfTexels = 0.75f;
// Shapes: rain streak = the drop's travel over this exposure (s; ~0.22 m at 9 m/s), width
// (m), opacity; snow flake size (m), opacity.
constexpr float kRainExposure = 0.025f;
constexpr float kRainWidth = 0.006f;
constexpr float kRainOpacity = 0.35f;
constexpr float kSnowSize = 0.025f;
constexpr float kSnowOpacity = 0.85f;
// Ripple normal strength (clean_weather_surface.glsl weatherRipple slope scale).
constexpr float kRippleStrength = 0.08f;
// The listener-cover readback's tap ring radius (m; clean_weather_sim.comp).
constexpr float kEarTapRadius = 1.f;
// A refused allocation (#697 policy) is retried this much later.
constexpr double kAllocRetrySecs = 10.0;
// M2 SPLASHES: a splash lives this long (s); rain hits spawn one within this radius (m) of the
// camera (beyond it they would be sub-pixel; the ring holds ~the near field's hits for a life).
constexpr float kSplashLife = 0.3f;
constexpr float kSplashRadius = 14.f;
// M2 TILTED MAP: the precipitation's mean fall (clean_weather_common.glsl wxVelocity): rain
// ~9.25 m/s down carried by the whole wind, snow ~1 m/s down carried by 0.9 of it. The shear
// s = horizontal drift per metre of fall is capped at tan(kMaxTiltDeg); a cascade re-renders
// once the direction it was rendered for is more than kTiltStepDeg off.
constexpr float kRainFall = 9.25f;
constexpr float kSnowFall = 1.0f;
// #954 PRECIPITATION RATE (clean_weather_common.glsl): the streak exposure / opacity growth with the
// rain rate, the snow's motion-blur exposure (s), its swirl at full wind (x the wind speed), the
// blowing snow's largest share of the slots and its layer height scale (m), and how far the box
// shrinks at the top of the ladder (the same pool denser near the camera; the veil carries the rest).
constexpr float kRainExposureHeavy = 0.3f;     // x 1 + this at 80 mm/h
constexpr float kRainOpacityHeavy = 0.35f;     // x 0.85 + this at 50 mm/h
constexpr float kSnowExposure = 0.03f;
constexpr float kSnowSwirl = 0.25f;
constexpr float kBlowShare = 0.35f;
constexpr float kBlowHeight = 0.35f;
constexpr float kBoxShrink = 0.4f;

float wxSmooth(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
// #954 PRECIPITATION VEIL (clean_weather_veil.frag): the precipitation's ceiling above the camera
// (m; clean_weather_veil.glsl kWxVeilCeiling), the curtains' strength, and the in-scatter colour's
// share of the world draws' ambient / sun (rain: a grey sheen of the sky it refracts; snow: a white
// diffuser, as the particles' own colours in clean_weather.frag).
constexpr float kVeilCeiling = 1500.f;
constexpr float kVeilRainAmb = 0.8f, kVeilRainSun = 0.25f;
constexpr float kVeilSnowAmb = 0.95f, kVeilSnowSun = 0.55f;

// A column-major 4 x 4 inverse (false: singular).
bool wxInvert4(const float* m, float* out)
{
    double inv[16];
    inv[0] = (double)m[5] * m[10] * m[15] - (double)m[5] * m[11] * m[14] - (double)m[9] * m[6] * m[15] + (double)m[9] * m[7] * m[14]
           + (double)m[13] * m[6] * m[11] - (double)m[13] * m[7] * m[10];
    inv[4] = -(double)m[4] * m[10] * m[15] + (double)m[4] * m[11] * m[14] + (double)m[8] * m[6] * m[15] - (double)m[8] * m[7] * m[14]
           - (double)m[12] * m[6] * m[11] + (double)m[12] * m[7] * m[10];
    inv[8] = (double)m[4] * m[9] * m[15] - (double)m[4] * m[11] * m[13] - (double)m[8] * m[5] * m[15] + (double)m[8] * m[7] * m[13]
           + (double)m[12] * m[5] * m[11] - (double)m[12] * m[7] * m[9];
    inv[12] = -(double)m[4] * m[9] * m[14] + (double)m[4] * m[10] * m[13] + (double)m[8] * m[5] * m[14] - (double)m[8] * m[6] * m[13]
            - (double)m[12] * m[5] * m[10] + (double)m[12] * m[6] * m[9];
    inv[1] = -(double)m[1] * m[10] * m[15] + (double)m[1] * m[11] * m[14] + (double)m[9] * m[2] * m[15] - (double)m[9] * m[3] * m[14]
           - (double)m[13] * m[2] * m[11] + (double)m[13] * m[3] * m[10];
    inv[5] = (double)m[0] * m[10] * m[15] - (double)m[0] * m[11] * m[14] - (double)m[8] * m[2] * m[15] + (double)m[8] * m[3] * m[14]
           + (double)m[12] * m[2] * m[11] - (double)m[12] * m[3] * m[10];
    inv[9] = -(double)m[0] * m[9] * m[15] + (double)m[0] * m[11] * m[13] + (double)m[8] * m[1] * m[15] - (double)m[8] * m[3] * m[13]
           - (double)m[12] * m[1] * m[11] + (double)m[12] * m[3] * m[9];
    inv[13] = (double)m[0] * m[9] * m[14] - (double)m[0] * m[10] * m[13] - (double)m[8] * m[1] * m[14] + (double)m[8] * m[2] * m[13]
            + (double)m[12] * m[1] * m[10] - (double)m[12] * m[2] * m[9];
    inv[2] = (double)m[1] * m[6] * m[15] - (double)m[1] * m[7] * m[14] - (double)m[5] * m[2] * m[15] + (double)m[5] * m[3] * m[14]
           + (double)m[13] * m[2] * m[7] - (double)m[13] * m[3] * m[6];
    inv[6] = -(double)m[0] * m[6] * m[15] + (double)m[0] * m[7] * m[14] + (double)m[4] * m[2] * m[15] - (double)m[4] * m[3] * m[14]
           - (double)m[12] * m[2] * m[7] + (double)m[12] * m[3] * m[6];
    inv[10] = (double)m[0] * m[5] * m[15] - (double)m[0] * m[7] * m[13] - (double)m[4] * m[1] * m[15] + (double)m[4] * m[3] * m[13]
            + (double)m[12] * m[1] * m[7] - (double)m[12] * m[3] * m[5];
    inv[14] = -(double)m[0] * m[5] * m[14] + (double)m[0] * m[6] * m[13] + (double)m[4] * m[1] * m[14] - (double)m[4] * m[2] * m[13]
            - (double)m[12] * m[1] * m[6] + (double)m[12] * m[2] * m[5];
    inv[3] = -(double)m[1] * m[6] * m[11] + (double)m[1] * m[7] * m[10] + (double)m[5] * m[2] * m[11] - (double)m[5] * m[3] * m[10]
           - (double)m[9] * m[2] * m[7] + (double)m[9] * m[3] * m[6];
    inv[7] = (double)m[0] * m[6] * m[11] - (double)m[0] * m[7] * m[10] - (double)m[4] * m[2] * m[11] + (double)m[4] * m[3] * m[10]
           + (double)m[8] * m[2] * m[7] - (double)m[8] * m[3] * m[6];
    inv[11] = -(double)m[0] * m[5] * m[11] + (double)m[0] * m[7] * m[9] + (double)m[4] * m[1] * m[11] - (double)m[4] * m[3] * m[9]
            - (double)m[8] * m[1] * m[7] + (double)m[8] * m[3] * m[5];
    inv[15] = (double)m[0] * m[5] * m[10] - (double)m[0] * m[6] * m[9] - (double)m[4] * m[1] * m[10] + (double)m[4] * m[2] * m[9]
            + (double)m[8] * m[1] * m[6] - (double)m[8] * m[2] * m[5];
    const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
    if (!(std::fabs(det) > 1e-30) || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
    return true;
}
constexpr float kSnowDrift = 0.9f;
constexpr float kMaxTiltDeg = 60.f;
constexpr float kTiltStepDeg = 2.5f;

// M2 LIGHTNING (RenderVKWeatherLightning): the flash's fill radiance at full flash (linear
// scene units, cool white), its falloff with the strike's distance (full within kFlashNear,
// ~1 / distance past it, at least kFlashFarMin), and the sky's share of it (clean_sky.frag).
constexpr float kFlashFill = 1.6f;
constexpr float kFlashColor[3] = {0.80f, 0.88f, 1.0f};
constexpr float kFlashNear = 1500.f;
constexpr float kFlashFarMin = 0.2f;
// M2 CLOUD / SKY (RenderVKWeatherSky): the sky darkens by up to this with full cloud and rain.
constexpr float kSkyDarkCloud = 0.22f;
constexpr float kSkyDarkRain = 0.12f;

// M2 PERSISTENT SNOW (clean_weather_snow.comp): the per-instance image's width and its row
// growth step (allocated for the live instance count, grown in steps), the cover grid's height
// tolerance (m; at least, or a few near-map texels along the tilted column), the speed (m/s)
// past which a moving object sheds its snow.
constexpr uint32_t kInstSnowWidth = 1024;
constexpr uint32_t kInstSnowRowStep = 16;
constexpr float    kCoverTolMin = 0.3f;
constexpr float    kCoverTolTexels = 3.f;
constexpr float    kShedSpeed = 4.f;
// The state's snowfall threshold (slweather.cpp kSnowFalling): below it the cover melts.
constexpr float    kSnowFalling = 0.02f;

// M3 SNOW DEPTH / TRANSPORT (clean_weather_snow.comp). Feature bits of the push block; the
// drift's erosion rate (share of the exposed LOOSE LAYER moved one texel downwind per second,
// per m/s of mean wind past kDriftWindMin), its pile cap (x the global depth); the REPOSE
// (settle / slide): the share of the excess over the angle of repose an avalanche moves per
// step (x 0.5 per neighbour) -- not frame-time scaled (an avalanche outruns the wind's supply).
enum : uint32_t { kFDepth = 1u, kFDrift = 2u, kFSlide = 4u, kFSettle = 8u, kFFoot = 16u, kFAvatar = 32u, kFMover = 64u, kFLimb = 128u,
                  kFBody = 256u };   // (#783 BODY CONTACT)
constexpr float    kDriftWindMin = 3.f;
constexpr float    kDriftRate = 0.004f;
constexpr float    kDriftCap = 3.f;
constexpr float    kAvalancheShare = 0.2f;
// M3 AVATARS: the smoothed pelvis speeds (m/s) under which an avatar counts as standing still
// (horizontal), past which it sheds its snow (horizontal: a fast walk / run / flight; vertical:
// a jump / fall / climb), and the footprint list's cap (two per avatar near the camera).
constexpr float    kAvStillSpeed = 0.35f;
constexpr float    kAvShedSpeed = 3.6f;
constexpr float    kAvShedVSpeed = 1.6f;
constexpr uint32_t kMaxStamps = 256;
constexpr uint32_t kAvSlots = 1024;   // LLCleanSkinStore::MAX_AVATARS (clean_av_slots.glsl AV_SLOT_CAP)
// M3 SNOW MESH (clean_weather_snowmesh.comp): the visual threshold (m: thinner snow stays the
// coating), the stride-2 / stride-4 distances (m), the cos of the kept view cone's half angle
// and the radius always kept (m), the lift against z-fighting (m, + 0.4 mm per metre of
// distance), the triangle budget (x 12 B of indices: the sheet + its skirts).
constexpr float    kMeshMinDepth = 0.02f;
constexpr float    kMeshLod1 = 10.f;
constexpr float    kMeshLod2 = 20.f;
constexpr float    kMeshConeCos = 0.25f;
constexpr float    kMeshKeepRadius = 4.f;
constexpr float    kMeshLift = 0.004f;
constexpr float    kMeshRebuildTurnCos = 0.990f;   // #946: a view turn past ~8 degrees rebuilds (the cells' view cone)
constexpr uint32_t kMeshMaxTris = 786432;   // 9 MB of indices (the inner grid + the outer ring)
// M3 OUTER RING (clean_weather_snow.comp -DWX_SNOW_RING; RenderVKWeatherSnowOuterRing): a
// coarser depth grid on the COARSE map's top plane around the inner grid. The coarse map's
// extent while it is on (x the ring's: the ring's snapped window stays inside it), the ring's
// extent / resolution bounds, its step interval bounds (recordings).
constexpr float    kRingCoarseMargin = 1.25f;
constexpr float    kRingExtentMin = 48.f, kRingExtentMax = 192.f;
constexpr uint32_t kRingResMin = 64, kRingResMax = 512;

// #783 THE ANGLE OF REPOSE (RenderVKWeatherSnowReposeAngle, degrees) as dz / dx: the depth's
// talus relaxation (cover / ring kernels), the mesh's spike clamp and toe. Clamped to 25 .. 43
// degrees: past 45 the connectivity rule's slope allowance (clean_weather_snow_join.glsl
// kSnowJoinSlope, 45 degrees) would cut the slopes it should relax.
float snowReposeTan()
{
    static LLCachedControl<F32> s_deg(gSavedSettings, "RenderVKWeatherSnowReposeAngle", 38.f);
    return std::tan(llclamp((F32)s_deg, 25.f, 43.f) * 0.017453293f);
}

// The angle (degrees) between the fall directions of two shears (d ~ (s, -1)).
float tiltAngleDeg(const float a[2], const float b[2])
{
    const float dot = a[0] * b[0] + a[1] * b[1] + 1.f;
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + 1.f), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + 1.f);
    return std::acos(std::clamp(dot / (la * lb), -1.f, 1.f)) * 57.29578f;
}

VkShaderModule wxModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

// #946 the VRAM tier's hysteresis, the sync line's period, the re-creation-loop run.
constexpr double   kTierCalm = 30.0;        // s without pressure before the small tier is left
constexpr double   kSyncLogSecs = 10.0;
constexpr uint32_t kSyncLoopRun = 3;        // live recordings in a row with a device wait
// The grid / ring resolutions kMeshMaxTris was sized for (the defaults).
constexpr uint64_t kMeshFullCells = 512ull * 512ull + 256ull * 256ull;
} // namespace

// ---- #946 SYNC / VRAM DIAGNOSTICS AND THE VRAM TIER ----
// Report #946 (RTX 3050 Laptop, 4 GB, weather on: 15 fps, off: 30-40): the CPU waited ~50 ms a
// frame for a GPU whose timestamps covered ~25-30 ms -- the rest was NOT in any GPU timer and no
// log line said what the weather did to the queue or to VRAM. Every device wait the weather
// causes is now timed and counted (a re-creation loop would show at once), the module's VRAM is
// reported by part next to the driver's heap usage / budget, and a small card runs the SMALL
// tier: about a quarter of the default footprint. (By CAPACITY only, not the ladder's transient
// modes: a tier change re-creates the near maps, and with them the snow grids -- the lying snow
// would start over on every pressure spike. The shelter atlas keeps its own pressure tiers.)
LLCleanWeatherVK::SyncStats LLCleanWeatherVK::sSync;
bool LLCleanWeatherVK::sSmallVram = false;

void LLCleanWeatherVK::noteRendererWait(double ms, const char* why)
{
    ++sSync.waits;
    ++sSync.bindingRewrites;
    sSync.waitMs += ms;
    sSync.worstWaitMs = std::max(sSync.worstWaitMs, ms);
    sSync.lastWhy = why ? why : "";
}

void LLCleanWeatherVK::wxWaitIdle(VkDevice dev, const char* why)
{
    if (!dev) return;
    const auto t0 = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(dev);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++sSync.waits;
    sSync.waitMs += ms;
    sSync.worstWaitMs = std::max(sSync.worstWaitMs, ms);
    sSync.lastWhy = why ? why : "";
}

void LLCleanWeatherVK::updateVramTier(const Frame& f)
{
    // RenderVKWeatherVramTier: 0 auto (small below kSmallVramCapacity), 1 always the full tier,
    // 2 always the small tier (A/B).
    static LLCachedControl<U32> s_tier(gSavedSettings, "RenderVKWeatherVramTier", 0);
    const uint32_t mode = s_tier;
    const bool smallCard = f.vramCapacity > 0 && f.vramCapacity < kSmallVramCapacity;
    // (#946: RenderVKWeatherQuality 0 "low" runs the small tier on any card -- the cheap mesh)
    static LLCachedControl<U32> s_quality(gSavedSettings, "RenderVKWeatherQuality", 1);
    const bool want = mode == 2u || (mode == 0u && (smallCard || (U32)s_quality == 0u));
    bool next = sSmallVram;
    if (want)
    {
        mTierCalmSince = mNow;
        next = true;
    }
    else if (sSmallVram && (mode == 1u || mTierCalmSince < 0.0 || mNow - mTierCalmSince > kTierCalm))
        next = false;
    if (next == sSmallVram) return;
    sSmallVram = next;
    LL_INFOS("VKWeather") << "weather VRAM tier: " << (next ? "SMALL" : "full") << " (capacity "
                          << (f.vramCapacity >> 20) << " MB, small below " << (kSmallVramCapacity >> 20)
                          << " MB; budget mode " << f.vramMode << ", rung " << f.vramRung << "; RenderVKWeatherVramTier "
                          << mode << ")"
                          << (next ? ": near maps <= 512^2 (snow grids <= 256^2), outer ring <= 128^2, snow mesh scaled with"
                                     " them, avatar shelter <= 8 layers at the half-height atlas, accumulation <= 4 grids of"
                                     " 16 x 16 x 32 -- each part as it is next (re-)created"
                                   : ": the settings' resolutions / counts again, each part as it is next (re-)created")
                          << LL_ENDL;
}

uint32_t LLCleanWeatherVK::meshTriCap() const
{
    const uint64_t cells = (uint64_t)mCoverRes * mCoverRes + (uint64_t)mRingRes * mRingRes;
    // (the full tier keeps the fixed cap whatever the resolutions: a ring turned off must not
    // shrink it under a configuration that had it)
    if (!sSmallVram || !cells || cells >= kMeshFullCells) return kMeshMaxTris;
    // (the same triangles per cell as the defaults' cap)
    return (uint32_t)std::max<uint64_t>(((uint64_t)kMeshMaxTris * cells + kMeshFullCells - 1u) / kMeshFullCells, 4096u);
}

uint64_t LLCleanWeatherVK::vramBytes(std::string* parts) const
{
    if (!mCtx) return 0;
    auto img = [&](const GpuImage& i) -> uint64_t { return i.image ? mCtx->allocBytes(i) : 0ull; };
    // (device-local only: the mapped readback / parameter buffers live in host memory)
    auto buf = [](const GpuBuffer& b) -> uint64_t { return b.buffer && !b.mapped ? (uint64_t)b.size : 0ull; };
    uint64_t maps = 0, precip = 0, snow = 0, mesh = 0, runoff = 0, shelter = 0, accum = 0;
    for (const OcclMap& m : mMap)
        for (const GpuImage& i : m.img) maps += img(i);
    for (const GpuBuffer* b : {&mParticles, &mVisible, &mArgs, &mSplashes, &mSplashHead}) precip += buf(*b);
    for (const GpuImage* i : {&mCover[0], &mCover[1], &mSnowBase[0], &mSnowBase[1], &mRing[0], &mRing[1], &mInstSnow, &mBodyStandIn})
        snow += img(*i);
    for (const GpuBuffer* b : {&mAvBits, &mAvState, &mStamps, &mMovers}) snow += buf(*b);
    for (const GpuBuffer* b : {&mMeshVerts, &mMeshIdx, &mMeshArgs, &mMeshMarks}) mesh += buf(*b);
    for (const GpuBuffer* b : {&mRunoffScratch, &mEmitters}) runoff += buf(*b);
    runoff += img(mFlowImg);
    for (const GpuImage* i : {&mShH, &mShS[0], &mShS[1]}) shelter += img(*i);
    for (const GpuBuffer* b : {&mShBuf, &mShMeshV, &mShMeshI, &mShMeshA}) shelter += buf(*b);
    accum += img(mAcImg);
    for (const GpuBuffer* b : {&mAcRec, &mAcScratch}) accum += buf(*b);
    if (parts)
    {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(1);
        const double M = 1.0 / (1024.0 * 1024.0);
        os << "maps " << maps * M << ", precipitation " << precip * M << ", snow grids " << snow * M << ", snow mesh "
           << mesh * M << ", run-off " << runoff * M << ", shelter " << shelter * M << ", accumulation " << accum * M;
        *parts = os.str();
    }
    return maps + precip + snow + mesh + runoff + shelter + accum;
}

void LLCleanWeatherVK::logSync(const Frame& f, bool waited)
{
    if (f.offscreen) return;
    ++sSync.frames;
    if (waited)
    {
        ++sSync.framesWithWait;
        ++sSync.streak;
        sSync.worstStreak = std::max(sSync.worstStreak, sSync.streak);
    }
    else
        sSync.streak = 0;
    if (sSync.streak >= kSyncLoopRun && mNow >= mSyncLoopWarnAt)
    {
        mSyncLoopWarnAt = mNow + kSyncLogSecs;
        LL_WARNS("VKWeather") << "weather sync: device waits on " << sSync.streak << " recordings in a row (last: "
                              << sSync.lastWhy << ") -- a re-creation loop: each wait drains the GPU queue" << LL_ENDL;
    }
    if (mNow < mSyncNextLog) return;
    const double span = mSyncNextLog > 0.0 ? mNow - (mSyncNextLog - kSyncLogSecs) : 0.0;
    mSyncNextLog = mNow + kSyncLogSecs;
    const SyncStats& c = sSync;
    const SyncStats& p = mSyncPrev;
    std::string parts;
    const uint64_t bytes = vramBytes(&parts);
    uint64_t heapUse = 0, heapBudget = 0;
    const bool live = mCtx && mCtx->liveDeviceBudget(heapUse, heapBudget);
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << "weather sync (" << span << " s, " << (c.frames - p.frames) << " live recordings): device waits "
       << (c.waits - p.waits) << " (" << (c.waitMs - p.waitMs) << " ms; worst ever " << c.worstWaitMs << " ms, last '"
       << c.lastWhy << "'), recordings with a wait " << (c.framesWithWait - p.framesWithWait) << " (longest run "
       << c.worstStreak << "), re-creations " << (c.recreates - p.recreates) << ", renderer binding 49-51 rewrites "
       << (c.bindingRewrites - p.bindingRewrites)
       << "; snow mesh builds " << (c.meshBuilds - p.meshBuilds) << " (reused " << (c.meshSkips - p.meshSkips)
       << ", of which locally patched " << (c.meshPatches - p.meshPatches) << ")"
       << "; own submits 0 (records into the frame), readbacks per frame slot (fence-waited, never this frame's)"
       << " | VRAM: weather " << bytes / (1024.0 * 1024.0) << " MB [" << parts << "], tier "
       << (sSmallVram ? "SMALL" : "full") << " (capacity " << (f.vramCapacity >> 20) << " MB, budget mode " << f.vramMode
       << ", rung " << f.vramRung << ")";
    if (live)
        os << ", device heap " << (heapUse >> 20) << " / " << (heapBudget >> 20) << " MB (driver usage / OS budget)";
    LL_INFOS("VKWeather") << os.str() << LL_ENDL;
    mSyncPrev = c;
}

// FIRST USE OFF THE RENDER THREAD (the fog's pattern): sampler, set layout, pool and pipeline
// layout here; the two pipelines go to an async batch and land on a later prepare() -- the
// precipitation is skipped until then (the wet surfaces need none of them). A failed build is
// latched for these formats.
bool LLCleanWeatherVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mSimPipe = mStage[0]; mDrawPipe = mStage[1];
        // (M2 snow: built with the first batch only; a failure disables the snow alone)
        if (mStage[2]) mCoverPipe = mStage[2];
        if (mStage[3]) mInstPipe = mStage[3];
        mSplashPipe = mStage[4];   // (M2: a failure only loses the splashes)
        mVeilPipe = mStage[kVeilStage];   // (#954: a failure only loses the veil)
        if (mVeilLayout && !mVeilPipe) LL_WARNS("VKWeather") << "weather veil pipeline failed to build; no precipitation veil" << LL_ENDL;
        if (mSnowLayout && (!mCoverPipe || !mInstPipe))
            LL_WARNS("VKWeather") << "weather snow pipelines failed to build; persistent snow disabled" << LL_ENDL;
        // M3 (first batch only, like the snow's; each failure loses only its own feature)
        if (mStage[5]) mAvPipe = mStage[5];
        if (mStage[6]) mStampPipe = mStage[6];
        if (mStage[16]) mBermPipe = mStage[16];   // (M5: a failure leaves flat tracks)
        // #901 the shelter chain (first batch only; a failure loses the shelter alone)
        for (uint32_t k = 0; k < kShKernels; ++k)
            if (mStage[kShStage0 + k]) mShPipe[k] = mStage[kShStage0 + k];
        // #901 rest-pose accumulation (a failure loses the accumulation alone: the per-avatar values)
        for (uint32_t k = 0; k < kAcKernels; ++k)
            if (mStage[kAcStage0 + k]) mAcPipe[k] = mStage[kAcStage0 + k];
        if (mShLayout && (!mAcPipe[0] || !mAcPipe[1] || !mAcPipe[2] || !mAcPipe[3]))
            LL_WARNS("VKWeather") << "weather avatar accumulation pipelines failed to build; per-avatar values only" << LL_ENDL;
        if (mShLayout && (!mShPipe[0] || !mShPipe[1] || !mShPipe[2] || !mShPipe[3] || !mShPipe[4]))   // (the mesh [5] optional)
            LL_WARNS("VKWeather") << "weather avatar shelter pipelines failed to build; avatar shelter disabled" << LL_ENDL;
        if (mStage[7]) mMeshBuildPipe = mStage[7];
        if (mStage[8]) mMeshGbufPipe = mStage[8];
        if (mStage[9]) mMeshShadowPipe = mStage[9];
        if (mStage[14]) mMeshFwdPipe[0] = mStage[14];
        if (mStage[15]) mMeshFwdPipe[1] = mStage[15];
        for (uint32_t v = 0; v < 4u; ++v)
            if (mStage[kPreStage0 + v]) mMeshPrePipe[v] = mStage[kPreStage0 + v];
        if (mStage[10]) mRingPipe = mStage[10];
        // M4 run-off: the build (first batch only), the spawn kernel (with the simulation), the
        // stream draw (with the precipitation draw); a failure loses the run-off alone.
        if (mStage[11]) mRunoffPipe = mStage[11];
        mSpawnPipe = mStage[12];
        mStreamPipe = mStage[13];
        if (mRunoffLayout && (!mRunoffPipe || !mSpawnPipe))
            LL_WARNS("VKWeather") << "weather run-off pipelines failed to build; run-off disabled" << LL_ENDL;
        if (mSnowLayout && (!mAvPipe || !mStampPipe))
            LL_WARNS("VKWeather") << "weather avatar snow / footprint pipelines failed to build; disabled" << LL_ENDL;
        if (mMeshLayout && (!mMeshBuildPipe || !mMeshGbufPipe))
            LL_WARNS("VKWeather") << "weather snow mesh pipelines failed to build; the snow stays a coating" << LL_ENDL;
        for (VkPipeline& p : mStage) p = VK_NULL_HANDLE;
        if (!mSimPipe || !mDrawPipe)
        {
            LL_WARNS("VKWeather") << "weather pipelines failed to build; precipitation disabled" << LL_ENDL;
            if (mSimPipe) vkDestroyPipeline(dev, mSimPipe, nullptr);
            if (mDrawPipe) { vkForgetPipeline(mDrawPipe); vkDestroyPipeline(dev, mDrawPipe, nullptr); }
            mSimPipe = mDrawPipe = VK_NULL_HANDLE;
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VKWeather") << "weather pipelines built (background)" << LL_ENDL;
    }
    const bool same = mPipeFormats[0] == f.hdrFormat && mPipeFormats[1] == f.glowFormat
                   && mPipeFormats[2] == f.reactFormat && mPipeFormats[3] == f.depthFormat
                   && mPipeReverseZ == f.reverseZ;
    if (mDrawPipe && same) return true;
    if (mPipeFailed && same) return false;
    mPipeFailed = false;
    if (mDrawPipe || mSimPipe)
    {
        // A pass format change (rare): rebuild the pipelines (the layouts stay).
        wxWaitIdle(dev, "precipitation pipelines rebuilt");
        if (mSimPipe) vkDestroyPipeline(dev, mSimPipe, nullptr);
        if (mDrawPipe) { vkForgetPipeline(mDrawPipe); vkDestroyPipeline(dev, mDrawPipe, nullptr); }
        if (mSplashPipe) { vkForgetPipeline(mSplashPipe); vkDestroyPipeline(dev, mSplashPipe, nullptr); }
        if (mSpawnPipe) vkDestroyPipeline(dev, mSpawnPipe, nullptr);
        if (mStreamPipe) { vkForgetPipeline(mStreamPipe); vkDestroyPipeline(dev, mStreamPipe, nullptr); }
        if (mVeilPipe) { vkForgetPipeline(mVeilPipe); vkDestroyPipeline(dev, mVeilPipe, nullptr); }
        mSimPipe = mDrawPipe = mSplashPipe = mSpawnPipe = mStreamPipe = mVeilPipe = VK_NULL_HANDLE;
    }
    if (!mLayout) return false;   // (created with the buffers)
    // M4 RUN-OFF: the flow build's layout {own} (+ its 80 B push), first use only.
    const bool runoffPipes = !mRunoffLayout;
    if (runoffPipes)
    {
        // 0 the near maps (sampled), 1 the scratch, 2 the flow map (storage image), 3 the emitters.
        VkDescriptorSetLayoutBinding b[4]{};
        const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (uint32_t i = 0; i < 4; ++i)
        {
            b[i].binding = i; b[i].descriptorType = types[i]; b[i].descriptorCount = i == 0 ? 2u : 1u;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mRunoffDsl, "mDslWeatherRunoff"));
        VK_CHECK(mRunoffPool.create(dev, "weather run-off", {{mRunoffDsl, 1}}));
        VKD_ALLOC_SOFT(mRunoffPool.allocate(mRunoffDsl, &mRunoffSet), { mRunoffSet = VK_NULL_HANDLE; });
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80u};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mRunoffDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mRunoffLayout, "mWeatherRunoffLayout"));
    }
    // M2 PERSISTENT SNOW: layout {draw set 0, own} (+ the 128 B push block), first use only.
    const bool snowPipes = !mSnowLayout && f.dslDraw0;
    if (snowPipes)
    {
        // own: 0 the grids, 1 the per-instance image (storage images); M3 2 the avatars' bits,
        // 3 their motion state, 4 the stamps (storage buffers), 5 the filtered bases, 6 the
        // outer rings (storage images); M5 7 the mover stamp list (storage buffer); #783 8 the
        // shelter chain's raster target (the avatars' bottom maps; storage image, read only);
        // #946 9 the snow mesh's local-edit marks (storage buffer).
        VkDescriptorSetLayoutBinding b[10]{};
        for (uint32_t i = 0; i < 10; ++i)
        {
            b[i].binding = i;
            const bool img = i < 2 || i == 5 || i == 6 || i == 8;
            b[i].descriptorType = img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = (i == 0 || i == 5 || i == 6) ? 2u : 1u; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 10; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mSnowDsl, "mDslWeatherSnow"));
        VK_CHECK(mSnowPool.create(dev, "weather snow", {{mSnowDsl, 1}}));
        VKD_ALLOC_SOFT(mSnowPool.allocate(mSnowDsl, &mSnowSet), { mSnowSet = VK_NULL_HANDLE; });
        const VkDescriptorSetLayout sl[2] = {f.dslDraw0, mSnowDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mSnowPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2; pli.pSetLayouts = sl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mSnowLayout, "mWeatherSnowLayout"));
        createShelterLayouts(dev, f.dslDraw0);   // #901 (its pipelines with this batch)
    }
    // M3 SNOW MESH: layout {own} (+ its 96 B push), first use only, when the renderer handed
    // the draw templates (else: no mesh, the coating alone).
    const bool meshPipes = snowPipes && !mMeshLayout && f.drawLayout && f.gbufDesc;
    if (meshPipes)
    {
        // 0 the grids, 1 - 3 the vertices / indices / draw arguments (+ #946 the tile table), 4 the
        // filtered bases, 5 the outer rings, #946 6 the local-edit marks, #783 7 the snow dump.
        VkDescriptorSetLayoutBinding b[8]{};
        for (uint32_t i = 0; i < 8; ++i)
        {
            const bool img = i == 0 || i == 4 || i == 5;
            b[i].binding = i;
            b[i].descriptorType = img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = img ? 2u : 1u; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 8; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mMeshDsl, "mDslWeatherSnowMesh"));
        VK_CHECK(mMeshPool.create(dev, "weather snow mesh", {{mMeshDsl, 1}}));
        VKD_ALLOC_SOFT(mMeshPool.allocate(mMeshDsl, &mMeshSet), { mMeshSet = VK_NULL_HANDLE; });
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128u};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mMeshDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mMeshLayout, "mWeatherSnowMeshLayout"));
    }
    PipelineBatchScope batchScope(dev);
    {
        VkShaderModule cs = wxModule(dev, kWorldWeatherSimSpv, kWorldWeatherSimSpv_bytes, "kWorldWeatherSimSpv");
        createComputePipelineInto(dev, cs, mLayout, 0, nullptr, &mStage[0], "weather simulation");
        destroyShaderModuleDeferred(dev, cs, nullptr);
        // M4: the drips' spawn kernel (the simulation's set / layout).
        VkShaderModule csp = wxModule(dev, kWorldWeatherRunoffSpawnSpv, kWorldWeatherRunoffSpawnSpv_bytes, "kWorldWeatherRunoffSpawnSpv");
        createComputePipelineInto(dev, csp, mLayout, 0, nullptr, &mStage[12], "weather run-off drip spawn");
        destroyShaderModuleDeferred(dev, csp, nullptr);
    }
    if (runoffPipes)
    {
        VkShaderModule cr = wxModule(dev, kWorldWeatherRunoffSpv, kWorldWeatherRunoffSpv_bytes, "kWorldWeatherRunoffSpv");
        createComputePipelineInto(dev, cr, mRunoffLayout, 0, nullptr, &mStage[11], "weather run-off flow");
        destroyShaderModuleDeferred(dev, cr, nullptr);
    }
    if (snowPipes)
    {
        VkShaderModule cs = wxModule(dev, kWorldWeatherSnowCoverSpv, kWorldWeatherSnowCoverSpv_bytes, "kWorldWeatherSnowCoverSpv");
        createComputePipelineInto(dev, cs, mSnowLayout, 0, nullptr, &mStage[2], "weather snow cover");
        destroyShaderModuleDeferred(dev, cs, nullptr);
        VkShaderModule ci = wxModule(dev, kWorldWeatherSnowInstSpv, kWorldWeatherSnowInstSpv_bytes, "kWorldWeatherSnowInstSpv");
        createComputePipelineInto(dev, ci, mSnowLayout, 0, nullptr, &mStage[3], "weather snow instances");
        destroyShaderModuleDeferred(dev, ci, nullptr);
        VkShaderModule ca = wxModule(dev, kWorldWeatherSnowAvatarSpv, kWorldWeatherSnowAvatarSpv_bytes, "kWorldWeatherSnowAvatarSpv");
        createComputePipelineInto(dev, ca, mSnowLayout, 0, nullptr, &mStage[5], "weather snow avatars");
        destroyShaderModuleDeferred(dev, ca, nullptr);
        VkShaderModule cs2 = wxModule(dev, kWorldWeatherSnowStampSpv, kWorldWeatherSnowStampSpv_bytes, "kWorldWeatherSnowStampSpv");
        createComputePipelineInto(dev, cs2, mSnowLayout, 0, nullptr, &mStage[6], "weather snow footprints");
        destroyShaderModuleDeferred(dev, cs2, nullptr);
        VkShaderModule cb = wxModule(dev, kWorldWeatherSnowBermSpv, kWorldWeatherSnowBermSpv_bytes, "kWorldWeatherSnowBermSpv");
        createComputePipelineInto(dev, cb, mSnowLayout, 0, nullptr, &mStage[16], "weather snow mover berms");
        destroyShaderModuleDeferred(dev, cb, nullptr);
        VkShaderModule cr = wxModule(dev, kWorldWeatherSnowRingSpv, kWorldWeatherSnowRingSpv_bytes, "kWorldWeatherSnowRingSpv");
        createComputePipelineInto(dev, cr, mSnowLayout, 0, nullptr, &mStage[10], "weather snow outer ring");
        destroyShaderModuleDeferred(dev, cr, nullptr);
        buildShelterPipelines(dev);   // #901
    }
    if (meshPipes)
    {
        VkShaderModule cm = wxModule(dev, kWorldWeatherSnowMeshSpv, kWorldWeatherSnowMeshSpv_bytes, "kWorldWeatherSnowMeshSpv");
        createComputePipelineInto(dev, cm, mMeshLayout, 0, nullptr, &mStage[7], "weather snow mesh build");
        destroyShaderModuleDeferred(dev, cm, nullptr);
        // The draws: the world draw layout, the mesh's own vertex stream (vec3 world + the
        // packed normal word, 16 B), 32-bit indices.
        auto meshVI = [](GraphicsPipelineDesc& d) {
            d.bindings = {{0u, 16u, VK_VERTEX_INPUT_RATE_VERTEX}};
            d.attributes = {{0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u}, {1u, 0u, VK_FORMAT_R32_UINT, 12u}};
        };
        VkShaderModule mv = wxModule(dev, kWorldWeatherSnowMeshVertSpv, kWorldWeatherSnowMeshVertSpv_bytes, "kWorldWeatherSnowMeshVertSpv");
        VkShaderModule mf = wxModule(dev, kWorldWeatherSnowMeshFragSpv, kWorldWeatherSnowMeshFragSpv_bytes, "kWorldWeatherSnowMeshFragSpv");
        {
            // The deferred G-buffer twins' state, but depth-tested and WRITTEN (the mesh is not
            // in the depth pre-pass the twins test EQUAL against), two-sided, full shading rate.
            GraphicsPipelineDesc d = *f.gbufDesc;
            d.vert = mv; d.frag = mf; d.layout = f.drawLayout; meshVI(d);
            d.depthTest = true; d.depthWrite = true; d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
            d.cullMode = VK_CULL_MODE_NONE;
            d.dynamicShadingRate = false;
            d.fragSpec = nullptr;
            createGraphicsPipelineInto(dev, d, &mStage[8], "weather snow mesh (G-buffer)");
        }
        if (f.shadowDesc)
        {
            VkShaderModule mvd = wxModule(dev, kWorldWeatherSnowMeshDepthSpv, kWorldWeatherSnowMeshDepthSpv_bytes, "kWorldWeatherSnowMeshDepthSpv");
            GraphicsPipelineDesc d = *f.shadowDesc;   // (conventional depth, the cascades' bias / clamp)
            d.vert = mvd; d.frag = VK_NULL_HANDLE; d.layout = f.drawLayout; meshVI(d);
            d.cullMode = VK_CULL_MODE_NONE;
            d.fragSpec = nullptr;
            createGraphicsPipelineInto(dev, d, &mStage[9], "weather snow mesh (sun shadow)");
            destroyShaderModuleDeferred(dev, mvd, nullptr);
        }
        {
            // DEPTH PRE-PASS (RenderVKWeatherSnowMeshPrepass): the renderer's pre-pass templates
            // (depth LESS + write, its attachment sets), the G-buffer draw's VS (invariant
            // position: the G-buffer / forward draw at LEQUAL keeps exactly these fragments),
            // two-sided, full rate; every variant has an FS (the ring hand-over's discard).
            static const struct { const uint32_t* spv; size_t bytes; const char* name; } kPre[4] = {
                {kWorldWeatherSnowMeshPre0Spv, kWorldWeatherSnowMeshPre0Spv_bytes, "kWorldWeatherSnowMeshPre0Spv"},
                {kWorldWeatherSnowMeshPre1Spv, kWorldWeatherSnowMeshPre1Spv_bytes, "kWorldWeatherSnowMeshPre1Spv"},
                {kWorldWeatherSnowMeshPre2Spv, kWorldWeatherSnowMeshPre2Spv_bytes, "kWorldWeatherSnowMeshPre2Spv"},
                {kWorldWeatherSnowMeshPre3Spv, kWorldWeatherSnowMeshPre3Spv_bytes, "kWorldWeatherSnowMeshPre3Spv"}};
            static const char* const kPreName[4] = {"weather snow mesh (pre-pass depth)", "weather snow mesh (pre-pass + normal)",
                                                    "weather snow mesh (pre-pass + normal + motion)",
                                                    "weather snow mesh (pre-pass + motion)"};
            for (uint32_t v = 0; v < 4u; ++v)
            {
                if (!f.preDesc[v]) continue;
                VkShaderModule pf = wxModule(dev, kPre[v].spv, kPre[v].bytes, kPre[v].name);
                GraphicsPipelineDesc d = *f.preDesc[v];
                d.vert = mv; d.frag = pf; d.layout = f.drawLayout; meshVI(d);
                d.depthTest = true; d.depthWrite = true;
                d.cullMode = VK_CULL_MODE_NONE;
                d.dynamicShadingRate = false;
                d.fragSpec = nullptr;
                createGraphicsPipelineInto(dev, d, &mStage[kPreStage0 + v], kPreName[v]);
                destroyShaderModuleDeferred(dev, pf, nullptr);
            }
        }
        if (f.fwdDesc)
        {
            // FORWARD lit pass 1 (frames without a G-buffer -- snapshots supersampled with RT
            // shadows among them): the LIT_RIG template's state, depth-tested and WRITTEN (not in
            // the pre-pass), two-sided, full rate; the plain lit FS's snow variant on the world
            // draw layout and, with RT, its ray-query twin on the RT draw layout (RT set at 2).
            VkShaderModule fv = wxModule(dev, kWorldWeatherSnowMeshFwdVertSpv, kWorldWeatherSnowMeshFwdVertSpv_bytes,
                                         "kWorldWeatherSnowMeshFwdVertSpv");
            for (uint32_t rt = 0; rt < 2; ++rt)
            {
                const VkPipelineLayout lay = rt ? f.drawLayoutRt : f.drawLayout;
                if (!lay) continue;
                VkShaderModule ff = rt ? wxModule(dev, kWorldWeatherSnowMeshFwdRtSpv, kWorldWeatherSnowMeshFwdRtSpv_bytes,
                                                  "kWorldWeatherSnowMeshFwdRtSpv")
                                       : wxModule(dev, kWorldWeatherSnowMeshFwdSpv, kWorldWeatherSnowMeshFwdSpv_bytes,
                                                  "kWorldWeatherSnowMeshFwdSpv");
                GraphicsPipelineDesc d = *f.fwdDesc;
                d.vert = fv; d.frag = ff; d.layout = lay; meshVI(d);
                d.depthTest = true; d.depthWrite = true; d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
                d.cullMode = VK_CULL_MODE_NONE;
                d.dynamicShadingRate = false;
                d.fragSpec = nullptr;
                createGraphicsPipelineInto(dev, d, &mStage[14 + rt], rt ? "weather snow mesh (forward, ray query)"
                                                                         : "weather snow mesh (forward)");
                destroyShaderModuleDeferred(dev, ff, nullptr);
            }
            destroyShaderModuleDeferred(dev, fv, nullptr);
        }
        destroyShaderModuleDeferred(dev, mv, nullptr);
        destroyShaderModuleDeferred(dev, mf, nullptr);
    }
    {
        // Pass 2b's attachments: premultiplied over the scene (its alpha preserved), glow
        // write-masked, coverage into the reactive mask's R (MAX), depth tested, not written.
        VkShaderModule vs = wxModule(dev, kWorldWeatherVertSpv, kWorldWeatherVertSpv_bytes, "kWorldWeatherVertSpv");
        VkShaderModule fs = wxModule(dev, kWorldWeatherFragSpv, kWorldWeatherFragSpv_bytes, "kWorldWeatherFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowWrite = false;
        d.reactFormat = f.reactFormat; d.reactWriteMask = VK_COLOR_COMPONENT_R_BIT;
        d.depthFormat = f.depthFormat; d.depthTest = true; d.depthWrite = false;
        d.reverseZ = f.reverseZ;
        d.customBlend = true;
        d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[1], "weather precipitation");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
        // M2 SPLASHES: the same pass state, the splash VS / FS (the ring, non-indexed instances).
        VkShaderModule svs = wxModule(dev, kWorldWeatherSplashVertSpv, kWorldWeatherSplashVertSpv_bytes, "kWorldWeatherSplashVertSpv");
        VkShaderModule sfs = wxModule(dev, kWorldWeatherSplashFragSpv, kWorldWeatherSplashFragSpv_bytes, "kWorldWeatherSplashFragSpv");
        d.vert = svs; d.frag = sfs;
        createGraphicsPipelineInto(dev, d, &mStage[4], "weather splashes");
        destroyShaderModuleDeferred(dev, svs, nullptr);
        destroyShaderModuleDeferred(dev, sfs, nullptr);
        // M4 RUN-OFF STREAMS: the same pass state, the ribbon VS / FS (the emitter list's indirect draw).
        VkShaderModule tvs = wxModule(dev, kWorldWeatherStreamVertSpv, kWorldWeatherStreamVertSpv_bytes, "kWorldWeatherStreamVertSpv");
        VkShaderModule tfs = wxModule(dev, kWorldWeatherStreamFragSpv, kWorldWeatherStreamFragSpv_bytes, "kWorldWeatherStreamFragSpv");
        d.vert = tvs; d.frag = tfs;
        createGraphicsPipelineInto(dev, d, &mStage[13], "weather run-off streams");
        destroyShaderModuleDeferred(dev, tvs, nullptr);
        destroyShaderModuleDeferred(dev, tfs, nullptr);
    }
    if (f.dslDraw0)
    {
        // #954 PRECIPITATION VEIL: {draw set 0 (binding 40: the opaque depth), own (the parameters)},
        // a fullscreen triangle on pass 2's attachments, premultiplied over the scene, glow and the
        // reactive mask write-masked, no depth test.
        if (!mVeilLayout)
        {
            const VkDescriptorSetLayout sl[2] = {f.dslDraw0, mDsl};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 2; pli.pSetLayouts = sl;
            VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mVeilLayout, "mWeatherVeilLayout"));
        }
        VkShaderModule vs = wxModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = wxModule(dev, kWorldWeatherVeilFragSpv, kWorldWeatherVeilFragSpv_bytes, "kWorldWeatherVeilFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mVeilLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowWrite = false;
        d.reactFormat = f.reactFormat; d.reactWriteMask = 0;
        d.depthFormat = f.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.customBlend = true;
        d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[kVeilStage], "weather precipitation veil");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
    }
    mPipeFormats[0] = f.hdrFormat; mPipeFormats[1] = f.glowFormat;
    mPipeFormats[2] = f.reactFormat; mPipeFormats[3] = f.depthFormat;
    mPipeReverseZ = f.reverseZ;
    PipelineJobOpts o;
    o.what = "weather";
    o.gate = false;   // (optional: never holds the login progress screen)
    mJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKWeather") << "weather pipelines compiling in the background (the precipitation starts when they land)" << LL_ENDL;
    return false;
}

void LLCleanWeatherVK::destroyBuffers()
{
    if (mCtx)
    {
        for (GpuBuffer* b : {&mParticles, &mVisible, &mArgs, &mParams[0], &mParams[1], &mListen[0], &mListen[1],
                             &mSplashes, &mSplashHead})
            if (b->buffer) mCtx->destroyBuffer(*b);
        for (GpuImage& im : mMap[OCCL_COARSE].img)
            if (im.image) mCtx->destroyImage(im);
    }
    mParticles = mVisible = mArgs = mSplashes = mSplashHead = GpuBuffer{};
    mParams[0] = mParams[1] = GpuBuffer{};
    mListen[0] = mListen[1] = GpuBuffer{};
    mListenWritten[0] = mListenWritten[1] = false;
    mMap[OCCL_COARSE].img[0] = mMap[OCCL_COARSE].img[1] = GpuImage{};
    mMap[OCCL_COARSE].res = 0;
    mMap[OCCL_COARSE].resetSchedule();
    mCap = 0;
    mPendMap = -1;
}

// The near cascade's maps (and its schedule). The caller waited for the device.
void LLCleanWeatherVK::destroyNear()
{
    OcclMap& m = mMap[OCCL_NEAR];
    if (mCtx)
        for (GpuImage& im : m.img)
            if (im.image) mCtx->destroyImage(im);
    m.img[0] = m.img[1] = GpuImage{};
    m.res = 0;
    m.resetSchedule();
    if (mPendMap == (int)OCCL_NEAR) mPendMap = -1;
}

void LLCleanWeatherVK::initMaps(VkCommandBuffer cmd, GpuImage* maps, uint32_t n)
{
    // Cleared to open sky and left in their sampled layout (the layout every reader --
    // binding 49, the simulation -- names), so they are valid before their first render.
    VkImageMemoryBarrier2 ib[2]{};
    n = std::min<uint32_t>(n, 2u);
    for (uint32_t i = 0; i < n; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib[i].image = maps[i].image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = n; dep.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearDepthStencilValue one{1.f, 0};
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < n; ++i)
        vkCmdClearDepthStencilImage(cmd, maps[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &rng);
    for (uint32_t i = 0; i < n; ++i)
    {
        ib[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                           | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib[i].newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier2(cmd, &dep);
}

// THE NEAR CASCADE (RenderVKWeatherNearCascade / NearRes): created with the module's other
// resources (only while there is weather), re-created on a resolution change, freed when
// turned off -- each behind a device wait (binding 49 then dirty). A refused allocation
// (#697) leaves the coarse map alone in charge and retries later. true = the maps exist.
bool LLCleanWeatherVK::ensureNear(VkCommandBuffer cmd)
{
    static LLCachedControl<bool> s_near(gSavedSettings, "RenderVKWeatherNearCascade", true);
    static LLCachedControl<bool> s_occl(gSavedSettings, "RenderVKWeatherOcclusion", true);
    static LLCachedControl<U32>  s_res(gSavedSettings, "RenderVKWeatherNearRes", 1024);
    OcclMap& m = mMap[OCCL_NEAR];
    // A multiple of 64: the eighth-extent snap is then a whole number of texels.
    // (#946: the SMALL VRAM tier caps it at 512^2 -- the snow grids follow at half of it)
    const uint32_t res0 = (std::clamp<uint32_t>((U32)s_res, 256u, 2048u) + 32u) & ~63u;
    const uint32_t res = sSmallVram ? std::min(res0, 512u) : res0;
    const bool want = s_near && s_occl;
    if (want && m.img[0].image && m.res == res) return true;
    if (!want && !m.img[0].image) return false;
    if (want && mNow < mNearRetryAt) return false;

    wxWaitIdle(mCtx->device(), "near maps");   // the old maps may be in use by frames in flight (binding 49)
    const bool had = m.img[0].image != VK_NULL_HANDLE;
    destroyNear();
    mBindingDirty = mBindingDirty || had;
    if (!want)
    {
        LL_INFOS("VKWeather") << "weather near occlusion cascade freed" << LL_ENDL;
        writeSets();   // (M2: the simulation's near slots back to the coarse stand-ins; device idle)
        return false;
    }
    bool ok = true;
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        for (GpuImage& im : m.img)
            im = mCtx->createImage2D(res, res, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
        ok = m.img[0].image && m.img[0].view && m.img[1].image && m.img[1].view;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather near occlusion maps (2 x " << res << "^2) failed to allocate; coarse map only, retrying in "
                              << kAllocRetrySecs << " s" << LL_ENDL;
        destroyNear();
        mNearRetryAt = mNow + kAllocRetrySecs;
        writeSets();
        return false;
    }
    m.res = res;
    initMaps(cmd, m.img, 2);
    mBindingDirty = true;
    writeSets();   // (M2: the simulation reads the near maps for the splash heights; device idle)
    LL_INFOS("VKWeather") << "weather near occlusion cascade: 2 x " << res << "^2 D32 ("
                          << (2ull * res * res * 4ull) / (1024ull * 1024ull) << " MB)" << LL_ENDL;
    return true;
}

// The particle buffers (budget change / first use), the parameter ring and the occlusion map,
// with the own set layout / pool / pipeline layout on first use. true = they exist at `cap`.
bool LLCleanWeatherVK::ensureBuffers(VkCommandBuffer cmd, uint32_t cap)
{
    VkDevice dev = mCtx->device();
    if (!mDsl)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // depth 1: open sky
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));
        // Own set (clean_weather_common.glsl): 0 params (UBO), 1 particles, 2 visible list,
        // 3 indirect args, 4 the occlusion maps (coarse [0] / [1], near [2] / [3]), 5 the
        // listener-cover readback, M2: 6 the splash ring (compute writes, VS reads), 7 its head,
        // M4: 8 the run-off emitters (the spawn kernel and the stream VS read them), #901: 9 the
        // avatar shelter atlas (two images), 10 its record (the simulation's rain test).
        VkDescriptorSetLayoutBinding b[11]{};
        const VkDescriptorType types[11] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (uint32_t i = 0; i < 11; ++i)
        {
            b[i].binding = i; b[i].descriptorType = types[i]; b[i].descriptorCount = i == 4 ? 4u : (i == 9 ? 2u : 1u);
            // (#954: the occlusion maps [4] also for the veil's fragment shader -- its cover test)
            b[i].stageFlags = (i < 3 || i == 6 || i == 8) ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                            : i == 4                    ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                                        : VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 11; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslWeather"));
        VK_CHECK(mPool.create(dev, "weather", {{mDsl, 2}}));
        for (VkDescriptorSet& s : mDset)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl, &s), { destroy(dev); return false; });
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mWeatherLayout"));
    }
    if (mParticles.buffer && mCap == cap) return true;
    if (mNow < mRetryAt) return false;   // (a refused allocation: retried later, not every frame)

    ++sSync.recreates;   // #946 (the "weather sync" line)
    VkMemScope memcat(VKMEM_PARTICLE);   // #407
    wxWaitIdle(dev, "particle buffers");   // the old buffers / maps may be in use by frames in flight (binding 49)
    const bool hadMap = mMap[OCCL_COARSE].img[0].image != VK_NULL_HANDLE;
    destroyBuffers();
    mBindingDirty = mBindingDirty || hadMap;
    bool ok = true;
    {
        // #697 allocation policy: a refusal skips the weather (retried on the next change).
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        // (M4: TRANSFER_DST -- the drip slots are cleared when the run-off (re)starts)
        mParticles = mCtx->createBuffer((VkDeviceSize)cap * 16u, ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mVisible = mCtx->createBuffer((VkDeviceSize)cap * 4u, ssbo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mArgs = mCtx->createBuffer(16u, ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (GpuBuffer& p : mParams)
            p = mCtx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (GpuBuffer& l : mListen)
            l = mCtx->createBuffer(16u, ssbo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        mSplashes = mCtx->createBuffer((VkDeviceSize)kSplashRing * 16u, ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // (M4: its spare words carry the run-off spawn's sums: cleared below)
        mSplashHead = mCtx->createBuffer(16u, ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (GpuImage& im : mMap[OCCL_COARSE].img)
            im = mCtx->createImage2D(kOcclRes, kOcclRes, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
        ok = mParticles.buffer && mVisible.buffer && mArgs.buffer && mParams[0].mapped && mParams[1].mapped
          && mListen[0].mapped && mListen[1].mapped && mSplashes.buffer && mSplashHead.buffer
          && mMap[OCCL_COARSE].img[0].image && mMap[OCCL_COARSE].img[0].view
          && mMap[OCCL_COARSE].img[1].image && mMap[OCCL_COARSE].img[1].view;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather buffers (" << cap << " particles) failed to allocate; weather skipped, retrying in "
                              << kAllocRetrySecs << " s" << LL_ENDL;
        destroyBuffers();
        mRetryAt = mNow + kAllocRetrySecs;
        return false;
    }
    mCap = cap;
    mReset = true;
    mDripsClean = false;   // (M4: the drip slots start cleared)
    mBindingDirty = true;
    mMap[OCCL_COARSE].res = kOcclRes;
    initMaps(cmd, mMap[OCCL_COARSE].img, 2);
    // M2: the splash ring starts empty (birth time 0 = never used).
    vkCmdFillBuffer(cmd, mSplashes.buffer, 0, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, mSplashHead.buffer, 0, VK_WHOLE_SIZE, 0u);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    writeSets();
    LL_INFOS("VKWeather") << "weather: " << cap << " precipitation particles, 2 x " << kOcclRes << "^2 occlusion maps" << LL_ENDL;
    return true;
}

void LLCleanWeatherVK::writeSets()
{
    const GpuImage* coarse = mMap[OCCL_COARSE].img;
    if (!mCtx || !mDsl || !mParticles.buffer || !coarse[0].view || !coarse[1].view) return;
    for (uint32_t s = 0; s < 2; ++s)
    {
        VkDescriptorBufferInfo bi[9] = {{mParams[s].buffer, 0, sizeof(Params)},
                                        {mParticles.buffer, 0, VK_WHOLE_SIZE},
                                        {mVisible.buffer, 0, VK_WHOLE_SIZE},
                                        {mArgs.buffer, 0, VK_WHOLE_SIZE},
                                        {},
                                        {mListen[s].buffer, 0, VK_WHOLE_SIZE},
                                        {mSplashes.buffer, 0, VK_WHOLE_SIZE},
                                        {mSplashHead.buffer, 0, VK_WHOLE_SIZE},
                                        // (M4: the emitters; the splash ring stands in -- never read then)
                                        {mEmitters.buffer ? mEmitters.buffer : mSplashes.buffer, 0, VK_WHOLE_SIZE}};
        // (the simulation's roof test reads the coarse cascade: a drop needs no 3 cm texels; M2:
        // the splashes take their heights from the near one where it exists -- the coarse maps
        // stand in for it otherwise, never read then: occlN.w = 0)
        const GpuImage* nearImg = mMap[OCCL_NEAR].img;
        const bool haveNear = nearImg[0].view && nearImg[1].view;
        VkDescriptorImageInfo ii[4] = {{mSampler, coarse[0].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {mSampler, coarse[1].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {mSampler, haveNear ? nearImg[0].view : coarse[0].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {mSampler, haveNear ? nearImg[1].view : coarse[1].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
        // #901: the shelter atlas + its record while they exist; the coarse maps / the splash ring
        // stand in otherwise (never read then: wp.shelter.x = 0).
        const bool haveSh = mShS[0].view && mShS[1].view && mShBuf.buffer;
        VkDescriptorImageInfo si[2] = {{mSampler, haveSh ? mShS[0].view : coarse[0].view,
                                        haveSh ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {mSampler, haveSh ? mShS[1].view : coarse[1].view,
                                        haveSh ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo sb{haveSh ? mShBuf.buffer : mSplashes.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[11]{};
        for (uint32_t i = 0; i < 11; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = mDset[s]; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i < 9 && i != 4) wr[i].pBufferInfo = &bi[i];
        }
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[4].pImageInfo = ii;
        wr[4].descriptorCount = 4;
        wr[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[9].pImageInfo = si;
        wr[9].descriptorCount = 2;
        wr[10].pBufferInfo = &sb;
        vkUpdateDescriptorSets(mCtx->device(), 11, wr, 0, nullptr);
    }
    writeRunoffSets();   // (M4: names the near maps)
    writeSnowSets();     // (#783: the snow set names the shelter's raster target -- re-created with the atlas)
}

// The occlusion maps' schedule: a cascade is due when the camera crossed into another of its
// snap cells, after a camera cut, when it has no map yet, or every
// RenderVKWeatherOcclusionInterval recordings. At most ONE renders per recording (the per-frame
// cost stays one projector-tile-sized pass): both due -> they take turns, the other stays due
// (its cell / age untouched) and renders next recording. The pending frame is latched here and
// rendered into the cascade's map that is NOT published; the frame info keeps describing the
// published one until the next prepare() publishes it.
void LLCleanWeatherVK::planOcclusion(const Frame& f, float radius, const float tilt[2])
{
    static LLCachedControl<bool> s_occl(gSavedSettings, "RenderVKWeatherOcclusion", true);
    static LLCachedControl<U32>  s_every(gSavedSettings, "RenderVKWeatherOcclusionInterval", 30);
    static LLCachedControl<F32>  s_nearExt(gSavedSettings, "RenderVKWeatherNearExtent", 32.f);
    mPendMap = -1;
    mOcclOn = s_occl;
    if (!mOcclOn)
    {
        for (OcclMap& m : mMap) m.resetSchedule();
        return;
    }
    // (M3 OUTER RING: while snow falls or lies, the coarse map grows to hold the ring's snapped
    // window; rain alone keeps its finer texels)
    const SLWeather::State& wst = SLWeather::instance().state();
    const bool snowy = wst.snow > SLWeather::kEpsilon || wst.snowCover > SLWeather::kEpsilon;
    const float ringExt = (snowy && ringWanted()) ? ringExtent() * kRingCoarseMargin : 0.f;
    const float extent[OCCL_COUNT] = {llclamp(std::max(kOcclExtentPerRadius * radius, ringExt), 64.f, 256.f),
                                      llclamp((F32)s_nearExt, 8.f, 64.f)};
    const U32 every = std::max<U32>(s_every, 1u);
    const float groundRef = groundUnderCamera(f);
    double cells[OCCL_COUNT][3] = {};
    float tilts[OCCL_COUNT][2] = {};
    bool due[OCCL_COUNT] = {};
    for (uint32_t c = 0; c < OCCL_COUNT; ++c)
    {
        OcclMap& m = mMap[c];
        if (!m.img[0].image || !m.res) continue;   // (the near cascade off / not allocated)
        // M2 TILT: the shear this cascade's next render uses -- the one it was last rendered
        // with while the precipitation's direction stays within kTiltStepDeg of it (a slow
        // wind change neither re-renders nor moves the snap cell every frame); the current
        // one past that (due now: the double buffer swaps it in seamlessly).
        const bool tiltStep = !m.haveTilt || tiltAngleDeg(tilt, m.cellTilt) > kTiltStepDeg;
        float* s = tilts[c];
        s[0] = tiltStep ? tilt[0] : m.cellTilt[0];
        s[1] = tiltStep ? tilt[1] : m.cellTilt[1];
        // The map's texel grid is horizontal on its top plane; the camera's own column meets
        // that plane UPWIND of it by the slant (top - camera height) x s: the cascade is
        // centred (and snapped) there, so the near field around the camera stays covered.
        const double snap = (double)extent[c] / 8.0;
        double* cell = cells[c];
        cell[2] = std::floor((double)f.camTime[2] / kOcclSnapZ + 0.5) * kOcclSnapZ;
        // The column through the GROUND under the camera (#783): centred on the camera's own
        // height, a raised camera under a wind tilt saw the ground below it (cam.z - ground) x |s|
        // off-centre -- past the near map's half extent the ground around the camera fell to
        // the coarse map / the snow's outer ring, which moved with the camera height and angle.
        const double slant = cell[2] + (double)kOcclAbove - (double)groundRef;
        cell[0] = std::floor(((double)f.camTime[0] - slant * s[0]) / snap + 0.5) * snap;
        cell[1] = std::floor(((double)f.camTime[1] - slant * s[1]) / snap + 0.5) * snap;
        const bool moved = cell[0] != m.cell[0] || cell[1] != m.cell[1] || cell[2] != m.cell[2];
        if (!f.offscreen) ++m.age;
        const bool stale = m.age >= every;
        if (f.cameraCut) m.forced = true;   // (kept until this cascade's render: they alternate)
        due[c] = m.pubIdx < 0 || moved || m.forced || (tiltStep && m.haveTilt) || (stale && !f.offscreen);
    }
    int pick = -1;
    if (due[OCCL_COARSE] && due[OCCL_NEAR])
    {
        // A cascade with nothing published goes first (the coarse one on a tie); else turns.
        const bool cNone = mMap[OCCL_COARSE].pubIdx < 0, nNone = mMap[OCCL_NEAR].pubIdx < 0;
        pick = cNone ? (int)OCCL_COARSE : nNone ? (int)OCCL_NEAR : (int)(mOcclTurn & 1u);
        if (!f.offscreen) ++mOcclTurn;
    }
    else if (due[OCCL_COARSE]) pick = OCCL_COARSE;
    else if (due[OCCL_NEAR]) pick = OCCL_NEAR;
    if (pick < 0) return;

    OcclMap& m = mMap[pick];
    const float E = extent[pick];
    const double* cell = cells[pick];
    const float sx = tilts[pick][0], sy = tilts[pick][1];
    m.target = m.pubIdx >= 0 ? (uint32_t)(m.pubIdx ^ 1) : 0u;
    const float x0 = (float)(cell[0] - 0.5 * E), y0 = (float)(cell[1] - 0.5 * E);
    const float top = (float)cell[2] + kOcclAbove, bottom = (float)cell[2] - kOcclBelow;
    const float span = top - bottom;
    // World -> VK clip, column-major. A SHEARED orthographic projection (a directional shadow
    // map's, along the fall direction d ~ (s, -1)): world P lies on the map column
    // u = P.xy - (top - P.z) s = P.xy + P.z s - top s, and u over [x0, x0 + E] -> [-1, 1]
    // (texel row 0 at y0) -- the texels stay on the horizontal snapped grid of the top plane,
    // only the depth axis leans into the wind. Depth (top - z) / span as before (0 at the top:
    // LESS_OR_EQUAL keeps the first surface the precipitation meets along its column).
    float* M = m.pendVP;
    std::memset(M, 0, sizeof(m.pendVP));
    M[0] = 2.f / E;  M[8] = 2.f * sx / E;  M[12] = -2.f * (x0 + top * sx) / E - 1.f;
    M[5] = 2.f / E;  M[9] = 2.f * sy / E;  M[13] = -2.f * (y0 + top * sy) / E - 1.f;
    M[10] = -1.f / span; M[14] = top / span;
    M[15] = 1.f;
    // The box is a parallelepiped: the side planes lean with the shear (normalized: the
    // cull's origin test reads the plane distance in metres).
    float planes[6][4] = {{1.f, 0.f, sx, -(x0 + top * sx)}, {-1.f, 0.f, -sx, x0 + E + top * sx},
                          {0.f, 1.f, sy, -(y0 + top * sy)}, {0.f, -1.f, -sy, y0 + E + top * sy},
                          {0.f, 0.f, 1.f, -bottom}, {0.f, 0.f, -1.f, top}};
    for (auto& pl : planes)
    {
        const float l = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        for (float& v : pl) v /= l;
    }
    std::memcpy(m.pendPlanes, planes, sizeof(planes));
    m.pend[0] = x0; m.pend[1] = y0; m.pend[2] = E;
    m.pend[3] = top; m.pend[4] = span; m.pend[5] = E / (float)m.res;
    m.pend[6] = sx; m.pend[7] = sy;
    m.pendCell[0] = cell[0]; m.pendCell[1] = cell[1]; m.pendCell[2] = cell[2];
    mPendMap = pick;
}

void LLCleanWeatherVK::occlusionRendered()
{
    if (mPendMap < 0) return;
    OcclMap& m = mMap[mPendMap];
    std::memcpy(m.ready, m.pend, sizeof(m.ready));
    for (int i = 0; i < 3; ++i) m.cell[i] = m.pendCell[i];
    m.cellTilt[0] = m.pend[6]; m.cellTilt[1] = m.pend[7];
    m.haveTilt = true;
    m.readyIdx = (int)m.target;
    m.age = 0;
    m.forced = false;
    mPendMap = -1;
}

bool LLCleanWeatherVK::prepare(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<U32>  s_quality(gSavedSettings, "RenderVKWeatherQuality", 1);
    static LLCachedControl<F32>  s_radius(gSavedSettings, "RenderVKWeatherRadius", 24.f);
    static LLCachedControl<bool> s_ripples(gSavedSettings, "RenderVKWeatherRipples", true);

    mRunning = false;
    mPrecip = false;
    mSimRecorded = false;
    mPendMap = -1;
    const SLWeather::State& st = SLWeather::instance().state();
    if (!f.ctx || !(st.visible() || avatarsWetHold()))   // (#901: a wet avatar dries on its own clock)
    {
        if (!idle() && f.ctx) destroy(f.ctx->device());
        return false;
    }
    if (!f.camTime || !f.sunColor || !f.ambient || !f.sunDir || f.rw < 2 || f.rh < 2) return false;
    mCtx = f.ctx;
    mNow = (double)f.camTime[3];
    // #946: the last recording's device waits (the module's re-creations and the renderer's
    // binding 49-51 rewrites after it) into the "weather sync" line; this recording's VRAM tier
    // (before anything below sizes a map or a grid).
    logSync(f, sSync.waits != mSyncWaitsAtFrame);
    updateVramTier(f);
    mSyncWaitsAtFrame = sSync.waits;
    const uint32_t cap = kCaps[std::min<U32>(s_quality, 2u)];
    if (!ensureBuffers(cmd, cap)) return false;
    ensureNear(cmd);
    const bool pipes = ensurePipelines(f);
    mSlot = f.frameSlot & 1u;

    // The listener's cover, written by this slot's last simulation (its fence has been waited:
    // host-coherent, made host-visible by recordSim's barrier). No simulation there: no answer.
    {
        float open = -1.f;
        if (mListenWritten[mSlot] && mListen[mSlot].mapped)
        {
            std::memcpy(&open, mListen[mSlot].mapped, sizeof(open));
            // M4: the run-off words the simulation copied from the spawn kernel's device-local
            // sums (loudness, the list's appends, the frame's drips).
            uint32_t w[4];
            std::memcpy(w, mListen[mSlot].mapped, sizeof(w));
            if (!f.offscreen) readRunoffWords(w);
        }
        mListenWritten[mSlot] = false;
        // M5: the mover stamp list's head this slot's last snow step copied back (its stats).
        if (mMoverReadWritten[mSlot] && mMoverRead[mSlot].mapped)
        {
            uint32_t h[kMoverHeadBytes / 4];
            std::memcpy(h, mMoverRead[mSlot].mapped, sizeof(h));
            if (!f.offscreen) readMoverHead(h);
        }
        mMoverReadWritten[mSlot] = false;
        // #901: the shelter record's head this slot's last chain copied back (its stats).
        if (mShReadWritten[mSlot] && mShRead[mSlot].mapped)
        {
            static uint32_t s_rec[kShRecordBytes / 4];   // (the whole record before its items: the self layer's log)
            std::memcpy(s_rec, mShRead[mSlot].mapped, sizeof(s_rec));
            if (!f.offscreen) readShelterHead(s_rec);
        }
        mShReadWritten[mSlot] = false;
        if (!f.offscreen) readAccumBack();   // #901 the accumulation's boxes / stats / own-grid snapshot
        if (mDumpWritten && mDumpSlot == mSlot) readDump();   // #783 the snow dump (this slot's fence waited)
        if (!f.offscreen) SLWeather::instance().setListenerOpen(open, open >= 0.f);
        if (!f.offscreen && !mRunoffNow) SLWeather::instance().setRunoffNear(0.f);
    }

    // Publish the maps the previous recording rendered (their frame info and their images
    // agree from this recording on, whatever order the readers run in).
    for (OcclMap& m : mMap)
        if (m.readyIdx >= 0)
        {
            m.pubIdx = m.readyIdx;
            std::memcpy(m.pub, m.ready, sizeof(m.pub));
            m.readyIdx = -1;
            if (&m == &mMap[OCCL_NEAR]) ++mNearGen;   // (M4: the run-off's flow follows it)
        }
    const float radius = llclamp((F32)s_radius, 8.f, 64.f);
    {
        // M2 TILTED MAP (RenderVKWeatherTiltedMap): the shear of the precipitation's mean fall
        // in the slow mean wind (no gusts: the map must not re-render with every gust).
        // Rain and snow blend by their shares; with nothing falling the last direction is kept
        // (the wet / snowy surfaces keep their map, no re-render). Capped at kMaxTiltDeg.
        static LLCachedControl<bool> s_tilted(gSavedSettings, "RenderVKWeatherTiltedMap", true);
        const float fall = st.rain + st.snow;
        if (!s_tilted)
            mTilt[0] = mTilt[1] = 0.f;
        else if (fall > SLWeather::kEpsilon)
        {
            const float share = st.snow / fall;
            const float k = (1.f - share) / kRainFall + share * kSnowDrift / kSnowFall;
            float sx = st.windMean[0] * k, sy = st.windMean[1] * k;
            const float len = std::sqrt(sx * sx + sy * sy);
            const float cap = std::tan(kMaxTiltDeg * 0.017453293f);
            if (len > cap) { sx *= cap / len; sy *= cap / len; }
            mTilt[0] = sx; mTilt[1] = sy;
        }
    }
    // M3 (#783): while snow falls or lies the maps render UNTILTED -- their column tops are the
    // true vertical tops the snow rests on. A sheared column through a vertical face (a TV
    // panel, a post) met that face: its tilted footprint became a sloped "support" the snow lay
    // on and was drawn across, and the ground in an object's wind shadow was no column top at
    // all (its column met the object), so the snow beside it ended in a cut. The fall
    // direction still shapes where snow lands: the snow kernels march it over the vertical
    // heightfield (ssr.weather[7].zw), the surfaces take their facing from it.
    {
        const bool snowy = st.snow > SLWeather::kEpsilon || st.snowCover > SLWeather::kEpsilon;
        const float mapTilt[2] = {snowy ? 0.f : mTilt[0], snowy ? 0.f : mTilt[1]};
        planOcclusion(f, radius, mapTilt);
    }

    // ---- the frame info (ssr.weather) ----
    std::memset(mWx, 0, sizeof(mWx));
    // SNAPSHOTS: the weather's clock holds at the last live recording's for the whole offscreen
    // sequence (the particles are frozen there already: dt 0) -- every recording of a snapshot
    // (the supersampled jitter frames, the six faces of a 360 capture, taken over a second or
    // more) sees the same ripples, rivulets, splashes, drips and streams, instead of splashes
    // and drips aging out between faces.
    const double clock = (f.offscreen && mLastTime >= 0.0) ? mLastTime : (double)f.camTime[3];
    const float time = (float)std::fmod(clock, 1000.0);
    mWx[0][0] = st.wetness;
    mWx[0][1] = st.rain;
    mWx[0][2] = time;
    mWx[0][3] = s_ripples ? kRippleStrength : 0.f;
    const OcclMap& coarse = mMap[OCCL_COARSE];
    if (coarse.pubIdx >= 0)
    {
        mWx[1][0] = coarse.pub[0]; mWx[1][1] = coarse.pub[1]; mWx[1][2] = 1.f / coarse.pub[2];
        mWx[1][3] = 1.f + (float)coarse.pubIdx;   // which map (0 = none)
        mWx[2][0] = coarse.pub[3]; mWx[2][1] = coarse.pub[4]; mWx[2][2] = kCoverBias; mWx[2][3] = coarse.pub[5];
    }
    const OcclMap& nearMap = mMap[OCCL_NEAR];
    if (nearMap.pubIdx >= 0 && nearMap.img[0].image)
    {
        mWx[3][0] = nearMap.pub[0]; mWx[3][1] = nearMap.pub[1]; mWx[3][2] = 1.f / nearMap.pub[2];
        mWx[3][3] = 1.f + (float)nearMap.pubIdx;     // which near map (0 = none): binding 49 [2] / [3]
        mWx[4][0] = nearMap.pub[3]; mWx[4][1] = nearMap.pub[4];
        mWx[4][2] = std::max(kNearBiasMin, kNearBiasTexels * nearMap.pub[5]); mWx[4][3] = nearMap.pub[5];
    }
    mWx[5][0] = st.snowCover;                   // (0 with WeatherSnowCoating off: SLWeather)
    // The PCF's jitter phase (TAA integrates it; #932: a supersampled shot's averaged
    // recordings add their sample index -- mFrame holds offscreen).
    mWx[5][1] = (float)((mFrame + f.snapSample) & 255u);
    mWx[5][2] = kPcfRadius;
    mWx[5][3] = kPcfTexels;
    // M2: the published maps' shears (each cascade reads with the one it was rendered with).
    if (coarse.pubIdx >= 0) { mWx[6][0] = coarse.pub[6]; mWx[6][1] = coarse.pub[7]; }
    if (nearMap.pubIdx >= 0 && nearMap.img[0].image) { mWx[7][0] = nearMap.pub[6]; mWx[7][1] = nearMap.pub[7]; }
    mWx[7][2] = mTilt[0]; mWx[7][3] = mTilt[1];   // M3: the precipitation's own fall shear (the maps may be untilted)
    {
        // M2 LIGHTNING: the state side times the flashes (SLWeather::State::lightning*); this only
        // draws what it says -- a soft, unshadowed cool-white fill from the strike's direction
        // (clean_surface_light.glsl weatherFlashLight) and a brighter sky (clean_sky.frag). Not
        // in snapshots: a reflection probe that caught a flash would hold it for minutes.
        if (!f.offscreen && flashNow(mWx[8], mWx[9])) mWx[8][3] = std::min(st.lightning, 1.f);
        // M2 CLOUD / SKY (RenderVKWeatherSky): the weather's cloud cover for the cloud layers
        // (the 2D dome and the volumetric layer take max(region / EEP coverage, this): weather
        // only ever ADDS cloud, so with the weather off -- this row zero, the module gone -- the
        // region's own sky is untouched) and a slight darkening of the sky with cloud and rain.
        static LLCachedControl<bool> s_sky(gSavedSettings, "RenderVKWeatherSky", true);
        if (s_sky)
        {
            mWx[9][3] = llclamp(kSkyDarkCloud * st.cloud + kSkyDarkRain * std::min(st.rain * 1.5f, 1.f), 0.f, 0.5f);
            mWx[11][2] = llclamp(st.cloud, 0.f, 1.f);
        }
    }

    // ---- the precipitation parameters (view / eye filled at recordSim) ----
    const float amount = std::min(st.rain + st.snow, 1.f);
    // #954 BLOWING SNOW (RenderVKWeatherBlowingSnow; not on the low quality): a share of the slots
    // streams along the ground in a strong wind over lying snow (SLWeather::State::blowing).
    static LLCachedControl<bool> s_blow(gSavedSettings, "RenderVKWeatherBlowingSnow", true);
    const float blowShare = (s_blow && (U32)s_quality > 0u) ? kBlowShare * llclamp(st.blowing, 0.f, 1.f) : 0.f;
    const bool blowNow = blowShare > 0.01f;
    mPrecip = pipes && !f.underwater && (amount > SLWeather::kEpsilon || blowNow);
    Params& P = mP;
    P = Params{};
    P.eye[3] = (float)std::fmod(clock, 4096.0);   // (held while offscreen: above)
    // #954: the box shrinks toward the top of the ladder -- the same pool denser near the camera.
    const float boxR = radius * (1.f - kBoxShrink * wxSmooth(0.55f, 0.95f, std::max(st.rain, st.snow)));
    P.box[0] = boxR;
    P.box[1] = llclamp(boxR * 0.6f, 8.f, 24.f);
    P.box[2] = (float)mCap;
    // dt: wall time between live recordings (a snapshot freezes the particles).
    const double now = (double)f.camTime[3];
    const double dt = (mLastTime < 0.0 || f.offscreen) ? 0.0 : std::clamp(now - mLastTime, 0.0, 0.1);
    if (!f.offscreen) mLastTime = now;
    mDt = (float)dt;
    P.wind[0] = st.wind[0]; P.wind[1] = st.wind[1]; P.wind[3] = (float)dt;
    P.amounts[0] = st.rain; P.amounts[1] = st.snow;
    P.amounts[2] = st.snow / std::max(st.rain + st.snow, 1e-6f);
    P.amounts[3] = amount;
    if (coarse.pubIdx >= 0)
    {
        P.occl[0] = mWx[1][0]; P.occl[1] = mWx[1][1]; P.occl[2] = mWx[1][2]; P.occl[3] = mWx[1][3];
        P.occl2[0] = coarse.pub[3]; P.occl2[1] = coarse.pub[4]; P.occl2[2] = kCoverBias;
        P.tilt[0] = coarse.pub[6]; P.tilt[1] = coarse.pub[7];
    }
    if (nearMap.pubIdx >= 0 && nearMap.img[0].image)
    {
        P.occlN[0] = mWx[3][0]; P.occlN[1] = mWx[3][1]; P.occlN[2] = mWx[3][2]; P.occlN[3] = mWx[3][3];
        P.occlN2[0] = nearMap.pub[3]; P.occlN2[1] = nearMap.pub[4]; P.occlN2[2] = mWx[4][2];
        P.tilt[2] = nearMap.pub[6]; P.tilt[3] = nearMap.pub[7];
    }
    {
        // M2 SPLASHES (RenderVKWeatherSplashes): rain hits within kSplashRadius of the camera
        // spawn into the ring (live recordings only: dt is 0 in a snapshot).
        static LLCachedControl<bool> s_splash(gSavedSettings, "RenderVKWeatherSplashes", true);
        P.splash[0] = (s_splash && mSplashPipe && st.rain > SLWeather::kEpsilon) ? 1.f : 0.f;
        P.splash[1] = (float)kSplashRing;
        P.splash[2] = kSplashLife;
        P.splash[3] = kSplashRadius;
    }
    P.occl2[3] = f.waterHeight;
    P.sun[0] = f.sunColor[0]; P.sun[1] = f.sunColor[1]; P.sun[2] = f.sunColor[2];
    P.sun[3] = llclamp(f.sunDir[2] * 4.f, 0.f, 1.f);
    P.amb[0] = f.ambient[0]; P.amb[1] = f.ambient[1]; P.amb[2] = f.ambient[2];
    P.screen[0] = (float)f.rw; P.screen[1] = (float)f.rh;
    P.screen[2] = 1.f / (float)f.rw; P.screen[3] = 1.f / (float)f.rh;
    // #954: longer, denser streaks in heavy rain (the drops' own size / speed spread is in the shaders).
    P.shape[0] = kRainExposure * (1.f + kRainExposureHeavy * wxSmooth(10.f, 80.f, st.rainFall));
    P.shape[1] = kRainWidth; P.shape[2] = kSnowSize;
    P.shape[3] = kRainOpacity * (0.85f + kRainOpacityHeavy * wxSmooth(2.f, 50.f, st.rainFall));
    {
        // #954 PRECIPITATION RATE (clean_weather_common.glsl): the rates, the blowing snow, the drop
        // size scale (2.5 x the Marshall-Palmer mean 1 / L = R^0.21 / 4.1 mm), the flake clumping
        // (bigger aggregates in heavy calm snow, broken up by a strong wind) and the snow's swirl.
        const float wind = std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]);
        // (the look saturates at a heavy blizzard; faster snow only builds up faster)
        P.fall[0] = st.rainFall; P.fall[1] = std::min(st.snowFall, SLWeather::kSnowFallLookMax);
        P.fall[2] = blowNow ? blowShare : 0.f;
        P.fall[3] = kBlowHeight;
        P.shape2[0] = 2.5f * std::pow(std::max(st.rainFall, 0.1f), 0.21f) / 4.1f;
        P.shape2[1] = (0.75f + 0.55f * wxSmooth(0.5f, 8.f, st.snowFall)) * (1.f - 0.45f * wxSmooth(8.f, 18.f, wind));
        P.shape2[2] = kSnowExposure;
        P.shape2[3] = kSnowSwirl * wxSmooth(3.f, 12.f, wind);
    }
    P.misc[0] = (float)(mFrame & 0xFFFFu);
    P.misc[1] = mReset ? 1.f : 0.f;
    P.misc[2] = kSnowOpacity;
    P.misc[3] = f.reverseZ ? 1.f : 0.f;   // (#954: the veil's sky test)
    {
        // #954 PRECIPITATION MEDIUM (clean_weather_veil.glsl; owner feedback: no screen-space pattern):
        // SLWeather's extinction (rain / snow / blowing snow, x WeatherFogAmount) -- ONE source:
        //   volumetric fog running (Frame::volFogOn): ssr.weather[16] -- the froxels inject it where
        //     it falls (open sky at the cell), moving with the wind (motion noise octaves: none on
        //     the low quality, one on the small VRAM tier, two otherwise); nothing else veils;
        //   fog off (RenderVKWeatherVeil): ssr.weather[15] -- the noise-free analytic veil, only on
        //     surfaces open to the sky (clean_weather_veil.frag + the transparents).
        // The precipitation particles take the analytic veil of their own distance either way (P.veil).
        static LLCachedControl<bool> s_veil(gSavedSettings, "RenderVKWeatherVeil", true);
        mVeilPass = false;
        const float sigma = (mPrecip && !f.underwater) ? std::max(st.precipExt, 0.f) : 0.f;
        if (sigma > 1e-6f)
        {
            const float all = SLWeather::precipExtinction(st.rainFall, st.snowFall, st.blowing);
            const float snowPart = SLWeather::precipExtinction(0.f, st.snowFall, st.blowing);
            const float share = all > 0.f ? llclamp(snowPart / all, 0.f, 1.f) : 0.f;
            const float sunW = P.sun[3];
            for (int c = 0; c < 3; ++c)
            {
                const float rainC = f.ambient[c] * kVeilRainAmb + f.sunColor[c] * kVeilRainSun * sunW;
                const float snowC = f.ambient[c] * kVeilSnowAmb + f.sunColor[c] * kVeilSnowSun * sunW;
                P.veil[c] = std::max(rainC + (snowC - rainC) * share, 0.f);
            }
            P.veil[3] = sigma;
            P.veil2[0] = kVeilCeiling;
            P.veil2[3] = share;
            if (f.volFogOn)
            {
                mWx[16][0] = std::max(sigma - st.precipExtBlow, 0.f);
                mWx[16][1] = std::max(st.precipExtBlow, 0.f);
                mWx[16][2] = share;
                mWx[16][3] = (U32)s_quality == 0u ? 0.f : (sSmallVram ? 1.f : 2.f);
            }
            else if (s_veil)
            {
                for (int c = 0; c < 3; ++c) mWx[15][c] = P.veil[c];
                mWx[15][3] = sigma;
                mVeilPass = true;
            }
        }
    }
    {
        // The audio listener (SLWeather::earOffset: the ear relative to the camera, set by the
        // weather audio) -- its cover is reported by live recordings only.
        const float* eo = SLWeather::instance().earOffset();
        P.ear[0] = f.camTime[0] + eo[0]; P.ear[1] = f.camTime[1] + eo[1]; P.ear[2] = f.camTime[2] + eo[2];
        P.ear[3] = f.offscreen ? 0.f : kEarTapRadius;
    }
    // M4 RUN-OFF: the flow build when a new near map was published, the drips' share of the
    // pool, ssr.weather[13] / [14]. The precipitation pipelines then run for the drips alone
    // while it drains (no rain falling: the rain's slots stand still).
    planRunoff(cmd, f, s_quality);
    if (mRunoffNow)
    {
        P.box[2] = (amount > SLWeather::kEpsilon || blowNow) ? (float)mDripBase : 0.f;
        mPrecip = mPrecip || (pipes && !f.underwater);
        static LLCachedControl<bool> s_splash2(gSavedSettings, "RenderVKWeatherSplashes", true);
        if (s_splash2 && mSplashPipe) P.splash[0] = 1.f;
    }
    // M2 PERSISTENT SNOW: this recording's grid / instance passes, and the published grid's rows.
    planSnow(cmd, f);
    // M3 SNOW MESH: planned now, from the grid just published; the renderer records the build
    // (recordMeshBuild) after its frame timers began, before every pass that draws it.
    planMeshBuild(cmd, f);
    if (mCoverPub >= 0 && mCover[0].image)
    {
        mWx[10][0] = mCoverPubF[0]; mWx[10][1] = mCoverPubF[1]; mWx[10][2] = 1.f / mCoverPubF[2];
        mWx[10][3] = 1.f + (float)mCoverPub;   // uWeatherOccl[4] / [5]
        mWx[12][0] = mCoverPubF[3]; mWx[12][1] = mCoverPubF[6]; mWx[12][2] = mCoverPubF[7];
        mWx[12][3] = mCoverPubF[2] / (float)std::max(mCoverRes, 1u);
    }
    {
        static LLCachedControl<bool> s_movers(gSavedSettings, "RenderVKWeatherSnowMovers", true);
        if (mInstSnow.image && s_movers) { mWx[11][0] = (float)mInstRows; mWx[11][1] = (float)kInstSnowWidth; }
        mWx[11][3] = mSnowPush[4];   // the grid's height tolerance (m)
        // M3 SNOW ON AVATARS: the avatar rows (after the instance rows) + 1; 0 = off / none yet.
        static LLCachedControl<bool> s_avatars(gSavedSettings, "RenderVKWeatherSnowAvatars", true);
        if (mInstSnow.image && s_avatars && mAvPipe) mWx[6][2] = (float)mInstRows + 1.f;
    }
    // #901 PER-AVATAR SHELTER: this recording's layers (the chain runs after the main cull:
    // recordShelter) -- the atlas image it writes is the one every reader of this recording uses.
    if (planShelter(cmd, f))
    {
        mWx[6][3] = 1.f + (float)mShDst;
        P.shelter[0] = (float)mShLayersUsed;
        P.shelter[1] = (float)mShDst;
    }
    else if (mShPub >= 0 && mShS[0].image && mShLayersUsed > 0)
    {
        // (no chain this recording -- a snapshot, nothing due: the last written atlas holds)
        mWx[6][3] = 1.f + (float)mShPub;
        P.shelter[0] = (float)mShLayersUsed;
        P.shelter[1] = (float)mShPub;
    }
    {
        // RenderVKWeatherAvatarShelterDebug: the lit FS tints avatar fragments by their map
        // (clean_weather_surface.glsl; + 10 on ssr.weather[6].w), the log reports the own layer often.
        static LLCachedControl<bool> s_dbg(gSavedSettings, "RenderVKWeatherAvatarShelterDebug", false);
        if (s_dbg && mWx[6][3] > 0.5f && !f.offscreen) mWx[6][3] += 10.f;
    }
    {
        // #901 REST-POSE ACCUMULATION: the grids are live (ssr.weather[14].w bit 8; draw set 0 binding 50
        // / 51 name them) -- the VS writes rest positions, the FS reads the grids; bit 16 the debug view.
        static LLCachedControl<bool> s_acc(gSavedSettings, "RenderVKWeatherAvatarAccum", true);
        static LLCachedControl<bool> s_accDbg(gSavedSettings, "RenderVKWeatherAvatarAccumDebug", false);
        if (s_acc && mAcImg.image && mAcRec.buffer && mWx[6][3] > 0.5f)
            mWx[14][3] = (float)((uint32_t)(mWx[14][3] + 0.5f) | 8u | (s_accDbg && !f.offscreen ? 16u : 0u));
    }
    {
        // #783 RenderVKWeatherSnowDepthDebug: the lit FS paints the depth grid's view of every static
        // surface (ssr.weather[14].w bit 32; clean_weather_surface.glsl weatherSnowDepthDebug).
        static LLCachedControl<bool> s_depthDbg(gSavedSettings, "RenderVKWeatherSnowDepthDebug", false);
        if (s_depthDbg && !f.offscreen) mWx[14][3] = (float)((uint32_t)(mWx[14][3] + 0.5f) | 32u);
    }
    {
        // #970 WEATHER AT RAY-TRACED HITS (clean_rt_weather.glsl; the RT reflections, the GI trace and
        // the RT probe capture): bit 64 the coat (weatherSurface at the hit), bit 128 the LYING SNOW
        // FIELD (the reflections' height-field march against the published depth grid -- only while
        // the snow mesh is drawn this recording, so a reflection never shows snow the view does not).
        // No AS / descriptor work: binding 49 and the frame info are all they read.
        static LLCachedControl<bool> s_rtHits(gSavedSettings, "RenderVKRTWeatherHits", true);
        static LLCachedControl<bool> s_rtField(gSavedSettings, "RenderVKRTSnowField", true);
        uint32_t b = (uint32_t)(mWx[14][3] + 0.5f);
        if (s_rtHits) b |= 64u;
        if (s_rtField && mMeshBuilt && mCoverPub >= 0 && mCover[0].image) b |= 128u;
        mWx[14][3] = (float)b;
    }
    if (!f.offscreen) ++mFrame;
    mRunning = true;
    return true;
}

// The height the weather maps centre their columns on: the land under the camera, kept within
// the maps' box below it (a camera far above the ground keeps the box's floor).
float LLCleanWeatherVK::groundUnderCamera(const Frame& f)
{
    const float cz = f.camTime[2];
    float z = cz;
    if (LLWorld* w = LLWorld::getInstance())
    {
        const F32 land = w->resolveLandHeightAgent(LLVector3(f.camTime[0], f.camTime[1], cz));
        if (std::isfinite(land)) z = land;
    }
    return llclamp(z, cz - 0.75f * kOcclBelow, cz);
}

// M3 OUTER RING settings (RenderVKWeatherSnowOuterRing / RingExtent / RingRes).
bool LLCleanWeatherVK::ringWanted()
{
    static LLCachedControl<bool> s_ring(gSavedSettings, "RenderVKWeatherSnowOuterRing", true);
    return s_ring;
}
float LLCleanWeatherVK::ringExtent()
{
    static LLCachedControl<F32> s_ext(gSavedSettings, "RenderVKWeatherSnowRingExtent", 128.f);
    return llclamp((F32)s_ext, kRingExtentMin, kRingExtentMax);
}
uint32_t LLCleanWeatherVK::ringResolution()
{
    static LLCachedControl<U32> s_res(gSavedSettings, "RenderVKWeatherSnowRingRes", 256);
    // (a multiple of 8: the snap is an eighth of the extent, a whole number of texels; #946: the
    // SMALL VRAM tier caps it at 128^2)
    const uint32_t res = (llclamp((U32)s_res, kRingResMin, kRingResMax) + 7u) / 8u * 8u;
    return sSmallVram ? std::min(res, 128u) : res;
}

bool LLCleanWeatherVK::flashNow(float radiance[3], float dir[3])
{
    static LLCachedControl<bool> s_flash(gSavedSettings, "RenderVKWeatherLightning", true);
    const SLWeather::State& st = SLWeather::instance().state();
    if (!s_flash || !(st.lightning > SLWeather::kEpsilon)) return false;
    const float dist = std::max(st.lightningDist, 1.f);
    const float k = kFlashFill * std::min(st.lightning, 1.f) * llclamp(kFlashNear / dist, kFlashFarMin, 1.f);
    for (int i = 0; i < 3; ++i) radiance[i] = kFlashColor[i] * k;
    float d[3] = {st.lightningDir[0], st.lightningDir[1], st.lightningDir[2]};
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n > 1e-4f) { for (float& v : d) v /= n; } else { d[0] = d[1] = 0.f; d[2] = 1.f; }
    for (int i = 0; i < 3; ++i) dir[i] = d[i];
    return true;
}

void LLCleanWeatherVK::fillFrameInfo(float out[kInfoRows][4]) const
{
    if (mRunning) std::memcpy(out, mWx, sizeof(mWx));
    else std::memset(out, 0, sizeof(mWx));
}

void LLCleanWeatherVK::recordSim(VkCommandBuffer cmd, const float* viewProjRel, const float* eye)
{
    if (!precipNow() || !mSimPipe || !mParticles.buffer || !viewProjRel || !eye) return;
    Params& P = mP;
    std::memcpy(P.view_proj, viewProjRel, sizeof(P.view_proj));
    P.eye[0] = eye[0]; P.eye[1] = eye[1]; P.eye[2] = eye[2];
    // #954 the veil rebuilds camera-relative points from the scene depth with the same matrix (a
    // singular one -- never for a real view -- leaves the veil off this recording).
    if (!wxInvert4(P.view_proj, P.inv_view_proj)) mVeilPass = false;
    std::memcpy(mParams[mSlot].mapped, &P, sizeof(Params));   // (host-coherent; this slot's last use is fenced)

    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // Last frame's draw (indirect + vertex reads) and simulation -> the arguments reset + this
    // simulation's writes.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | C,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | C,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    // VkDrawIndirectCommand: 6 vertices (one quad), the instance count from the simulation.
    const uint32_t args[4] = {6u, 0u, 0u, 0u};
    vkCmdUpdateBuffer(cmd, mArgs.buffer, 0, sizeof(args), args);
    // M4: the drip slots cleared when the run-off (re)starts or after a camera cut.
    if (mRunoffNow && !mDripsClean && mDripCount)
    {
        vkCmdFillBuffer(cmd, mParticles.buffer, (VkDeviceSize)mDripBase * 16u, (VkDeviceSize)mDripCount * 32u, 0u);
        mDripsClean = true;
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               C, RW);
    // M4: the drips' spawn (one invocation per emitter, indirect).
    recordRunoffSpawn(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSimPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mDset[mSlot], 0, nullptr);
    // The rain's slots (box.z) and M4 the drips after them.
    const uint32_t threads = (uint32_t)(P.box[2] + 0.5f) + (mRunoffNow ? mDripCount : 0u);
    if (threads) vkCmdDispatch(cmd, (threads + 255u) / 256u, 1, 1);
    // -> the draw's indirect read and vertex fetches (pass 2b), and the host (the listener word,
    // read when this frame slot comes round again).
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
               VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    if (mP.ear[3] > 0.f) mListenWritten[mSlot] = true;
    mReset = false;
    mSimRecorded = true;
}

void LLCleanWeatherVK::recordVeil(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0)
{
    if (!veilNow() || !dsetDraw0 || !mVeilLayout) return;
    bindGraphicsPipeline(cmd, mVeilPipe);
    const VkDescriptorSet sets[2] = {dsetDraw0, mDset[mSlot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mVeilLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanWeatherVK::recordDraw(VkCommandBuffer cmd)
{
    if (!mSimRecorded || !mDrawPipe) return;
    mSimRecorded = false;
    bindGraphicsPipeline(cmd, mDrawPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mDset[mSlot], 0, nullptr);
    vkCmdDrawIndirect(cmd, mArgs.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
    // M2 SPLASHES: the whole ring (the VS collapses the dead slots: ~12k vertices, no indirection).
    // Drawn while it rains and for a splash life after (the ring drains on its own).
    if (mSplashPipe && mSplashes.buffer && (mP.splash[0] > 0.5f || mNow - mLastSplashOn < (double)kSplashLife))
    {
        if (mP.splash[0] > 0.5f) mLastSplashOn = mNow;
        bindGraphicsPipeline(cmd, mSplashPipe);
        vkCmdDraw(cmd, 6, kSplashRing, 0, 0);
    }
    // M4 RUN-OFF STREAMS: one instance per emitter (the list's indirect draw; the VS collapses
    // the ones under the stream threshold).
    if (mRunoffNow && mStreamPipe && mEmitters.buffer && mP.runoff2[2] > 0.5f)
    {
        bindGraphicsPipeline(cmd, mStreamPipe);
        vkCmdDrawIndirect(cmd, mEmitters.buffer, 16, 1, sizeof(VkDrawIndirectCommand));
    }
}

// ---- M2 PERSISTENT SNOW ----
void LLCleanWeatherVK::destroySnow()
{
    destroyMesh();   // (M3: it reads the grids)
    if (mCtx)
    {
        for (GpuImage& im : mCover)
            if (im.image) mCtx->destroyImage(im);
        for (GpuImage& im : mSnowBase)
            if (im.image) mCtx->destroyImage(im);
        for (GpuImage& im : mRing)
            if (im.image) mCtx->destroyImage(im);
        if (mInstSnow.image) mCtx->destroyImage(mInstSnow);
        for (GpuBuffer* b : {&mAvBits, &mAvState, &mStamps, &mMovers, &mMoverRead[0], &mMoverRead[1], &mMeshMarks})
            if (b->buffer) mCtx->destroyBuffer(*b);
        if (mBodyStandIn.image) mCtx->destroyImage(mBodyStandIn);
    }
    mMeshMarks = GpuBuffer{};
    mBodyStandIn = GpuImage{};
    mBodyDue = false;
    mMovers = mMoverRead[0] = mMoverRead[1] = GpuBuffer{};
    mMoverReadWritten[0] = mMoverReadWritten[1] = false;
    mMoverDue = mLimbDue = false;
    sMoverStats.bytes = 0;
    sMoverStats.limbs = sMoverStats.limbCap = sMoverStats.limbDropped = 0;
    sMoverStats.stamps = sMoverStats.cap = sMoverStats.dropped = sMoverStats.tracked = 0;
    mCover[0] = mCover[1] = mSnowBase[0] = mSnowBase[1] = mRing[0] = mRing[1] = mInstSnow = GpuImage{};
    mRingRes = 0;
    mRingPub = mRingReady = -1;
    mRingFresh = true;
    mRingDue = false;
    mRingDt = 0.f;
    mAvBits = mAvState = mStamps = GpuBuffer{};
    mCoverRes = mInstRows = 0;
    mCoverPub = mCoverReady = -1;
    mCoverFresh = true;
    mCoverDue = mInstDue = mAvDue = mStampDue = false;
}

// The snow set (0 the grids, 1 the per-instance image, M3 2 / 3 / 4 the avatar buffers, 5 the
// filtered bases) and the mesh set's grids + bases. Device idle (the callers re-created what they name).
void LLCleanWeatherVK::writeSnowSets()
{
    if (!mCtx || !mCover[0].view || !mCover[1].view || !mSnowBase[0].view || !mSnowBase[1].view || !mRing[0].view
        || !mRing[1].view || !mInstSnow.view) return;
    const VkDescriptorImageInfo ri[2] = {{VK_NULL_HANDLE, mRing[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                         {VK_NULL_HANDLE, mRing[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorImageInfo ci[2] = {{VK_NULL_HANDLE, mCover[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                         {VK_NULL_HANDLE, mCover[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorImageInfo bi2[2] = {{VK_NULL_HANDLE, mSnowBase[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, mSnowBase[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    if (mSnowSet && mAvBits.buffer && mAvState.buffer && mStamps.buffer && mMovers.buffer && mBodyStandIn.view && mMeshMarks.buffer)
    {
        const VkDescriptorImageInfo ii{VK_NULL_HANDLE, mInstSnow.view, VK_IMAGE_LAYOUT_GENERAL};
        // #783: the shelter's raster target (its bottom maps) while the atlas exists, else the stand-in
        const VkDescriptorImageInfo bdi{VK_NULL_HANDLE, mShH.view ? mShH.view : mBodyStandIn.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo bi[3] = {{mAvBits.buffer, 0, VK_WHOLE_SIZE}, {mAvState.buffer, 0, VK_WHOLE_SIZE},
                                              {mStamps.buffer, 0, VK_WHOLE_SIZE}};
        const VkDescriptorBufferInfo mvi{mMovers.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo mki{mMeshMarks.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[10]{};
        for (uint32_t i = 0; i < 10; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = mSnowSet; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
            const bool img = i < 2 || i == 5 || i == 6 || i == 8;
            wr[i].descriptorType = img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i >= 2 && i < 5) wr[i].pBufferInfo = &bi[i - 2];
        }
        wr[0].descriptorCount = 2; wr[0].pImageInfo = ci;
        wr[1].pImageInfo = &ii;
        wr[5].descriptorCount = 2; wr[5].pImageInfo = bi2;
        wr[6].descriptorCount = 2; wr[6].pImageInfo = ri;
        wr[7].pBufferInfo = &mvi;   // M5: the mover stamp list
        wr[8].pImageInfo = &bdi;    // #783 the bottom maps
        wr[9].pBufferInfo = &mki;   // #946 the snow mesh's local-edit marks
        vkUpdateDescriptorSets(mCtx->device(), 10, wr, 0, nullptr);
    }
    if (mMeshSet && mMeshVerts.buffer && mMeshIdx.buffer && mMeshArgs.buffer && mMeshMarks.buffer && mDumpBuf.buffer)
    {
        const VkDescriptorBufferInfo bi[3] = {{mMeshVerts.buffer, 0, VK_WHOLE_SIZE}, {mMeshIdx.buffer, 0, VK_WHOLE_SIZE},
                                              {mMeshArgs.buffer, 0, VK_WHOLE_SIZE}};
        const VkDescriptorBufferInfo mki{mMeshMarks.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo dmi{mDumpBuf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[8]{};
        for (uint32_t i = 0; i < 8; ++i)
        {
            const bool img = i == 0 || i == 4 || i == 5;
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = mMeshSet; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
            wr[i].descriptorType = img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i >= 1 && i < 4) wr[i].pBufferInfo = &bi[i - 1];
        }
        wr[0].descriptorCount = 2; wr[0].pImageInfo = ci;
        wr[4].descriptorCount = 2; wr[4].pImageInfo = bi2;
        wr[5].descriptorCount = 2; wr[5].pImageInfo = ri;
        wr[6].pBufferInfo = &mki;   // #946 the local-edit marks
        wr[7].pBufferInfo = &dmi;   // #783 the snow dump (a 256 B placeholder while not armed)
        vkUpdateDescriptorSets(mCtx->device(), 8, wr, 0, nullptr);
    }
}

// The grids (half the near map's resolution over its extent: ~6 cm at the defaults; M3
// rgba32f: 2 x 512^2 x 16 B = 8 MB) and the per-instance image (kInstSnowWidth x rows + the
// two M3 avatar rows, rgba32f: 256 KB per 16k instances + 32 KB), M3 the avatar buffers
// (36 KB), while wanted; re-created on a near-map resolution change or when the instances
// outgrow the image (device wait; binding 49 then dirty). A refused allocation skips the
// snow for kAllocRetrySecs.
void LLCleanWeatherVK::ensureSnow(VkCommandBuffer cmd, bool want, uint32_t instCount)
{
    const uint32_t res = mMap[OCCL_NEAR].res / 2u;
    const uint32_t rows = std::max<uint32_t>((instCount + kInstSnowWidth - 1u) / kInstSnowWidth, 1u);
    if (!want || res == 0 || !mSnowSet)
    {
        if (mCover[0].image || mInstSnow.image)
        {
            wxWaitIdle(mCtx->device(), "snow cover freed");   // (binding 49 names them in frames in flight)
            destroySnow();
            mBindingDirty = true;
            LL_INFOS("VKWeather") << "weather snow cover freed" << LL_ENDL;
        }
        return;
    }
    const uint32_t ringRes = ringResolution();
    const bool needCover = !mCover[0].image || mCoverRes != res;
    const bool needRing = !mRing[0].image || mRingRes != ringRes;
    const bool needInst = !mInstSnow.image || mInstRows < rows;
    const bool needAv = !mAvBits.buffer || !mAvState.buffer || !mStamps.buffer || !mMovers.buffer
                     || !mMoverRead[0].mapped || !mMoverRead[1].mapped    // (M5: the mover list with them)
                     || !mBodyStandIn.view;                               // (#783: the bottom maps' stand-in)
    if (!needCover && !needRing && !needInst && !needAv) return;
    if (mNow < mSnowRetryAt) return;
    wxWaitIdle(mCtx->device(), "snow cover");   // (binding 49 names them in frames in flight)
    // Re-created separately: the instances outgrowing their image (a region loading) must not
    // reset the cover grid.
    const uint32_t newRows = (rows + kInstSnowRowStep - 1u) / kInstSnowRowStep * kInstSnowRowStep;
    if (needCover)
    {
        for (GpuImage& im : mCover)
            if (im.image) mCtx->destroyImage(im);
        for (GpuImage& im : mSnowBase)
            if (im.image) mCtx->destroyImage(im);
        mCover[0] = mCover[1] = mSnowBase[0] = mSnowBase[1] = GpuImage{};
        if (mMeshMarks.buffer) mCtx->destroyBuffer(mMeshMarks);
        mMeshMarks = GpuBuffer{};
        mCoverRes = 0;
        mCoverPub = mCoverReady = -1;
        mCoverFresh = true;
        mMeshDirty = true;   // (#946: the mesh's tile table and marks start over with the grids)
    }
    if (needRing)
    {
        for (GpuImage& im : mRing)
            if (im.image) mCtx->destroyImage(im);
        mRing[0] = mRing[1] = GpuImage{};
        mRingRes = 0;
        mRingPub = mRingReady = -1;
        mRingFresh = true;
        mRingDt = 0.f;
    }
    if (needInst)
    {
        if (mInstSnow.image) mCtx->destroyImage(mInstSnow);
        mInstSnow = GpuImage{};
        mInstRows = 0;
    }
    if (needAv)
    {
        for (GpuBuffer* b : {&mAvBits, &mAvState, &mStamps, &mMovers, &mMoverRead[0], &mMoverRead[1]})
        {
            if (b->buffer) mCtx->destroyBuffer(*b);
            *b = GpuBuffer{};
        }
        if (mBodyStandIn.image) mCtx->destroyImage(mBodyStandIn);
        mBodyStandIn = GpuImage{};
        mMoverReadWritten[0] = mMoverReadWritten[1] = false;
    }
    mBindingDirty = true;
    bool ok = true;
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (needCover)
        {
            for (GpuImage& im : mCover)
                im = mCtx->createImage2D(res, res, VK_FORMAT_R32G32B32A32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
            for (GpuImage& im : mSnowBase)
                im = mCtx->createImage2D(res, res, VK_FORMAT_R32G32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);   // (+ the support cap)
            // #946 the snow mesh's local-edit marks (33 KB at 512^2)
            mMeshMarks = mCtx->createBuffer(meshMarksBytes(res),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (needRing)
            for (GpuImage& im : mRing)
                im = mCtx->createImage2D(ringRes, ringRes, VK_FORMAT_R32G32B32A32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);   // (+ the support cap)
        // (+ 2 rows: the M3 avatars, kAvSlots <= kInstSnowWidth wide)
        if (needInst)
            mInstSnow = mCtx->createImage2D(kInstSnowWidth, newRows + 2u, VK_FORMAT_R32G32B32A32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        if (needAv)
        {
            const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            mAvBits = mCtx->createBuffer(sizeof(mAvBitsCpu), su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            mAvState = mCtx->createBuffer((VkDeviceSize)kAvSlots * 32u, su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            mStamps = mCtx->createBuffer(16u + (VkDeviceSize)kMaxStamps * 32u, su | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            // M5: the mover stamp list (kMoverHeadBytes head + parameters + #783 body counters, the per-box volumes, 64 B per box) and its head's
            // per-frame-slot readback.
            mMovers = mCtx->createBuffer(kMoverHeadBytes + kMoverVolBytes + (VkDeviceSize)kStampSlots * 64u,
                                         su | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            for (GpuBuffer& r : mMoverRead)
                r = mCtx->createBuffer(kMoverHeadBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            // #783: the bottom maps' stand-in (the snow set's binding 8 while there is no shelter atlas)
            mBodyStandIn = mCtx->createImage2D(1, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                               VK_IMAGE_ASPECT_COLOR_BIT);
        }
        ok = mCover[0].image && mCover[0].view && mCover[1].image && mCover[1].view && mSnowBase[0].image && mSnowBase[0].view
          && mSnowBase[1].image && mSnowBase[1].view && mRing[0].image && mRing[0].view && mRing[1].image && mRing[1].view
          && mMeshMarks.buffer
          && mInstSnow.image && mInstSnow.view
          && mAvBits.buffer && mAvState.buffer && mStamps.buffer
          && mMovers.buffer && mMoverRead[0].mapped && mMoverRead[1].mapped
          && mBodyStandIn.image && mBodyStandIn.view;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather snow cover (2 x " << res << "^2, " << newRows << " instance rows) failed to allocate; "
                              << "snow skipped, retrying in " << kAllocRetrySecs << " s" << LL_ENDL;
        destroySnow();
        mSnowRetryAt = mNow + kAllocRetrySecs;
        return;
    }
    if (needCover)
    {
        mCoverRes = res;
        // #946: no marks, an empty head (the patch's dispatches)
        vkCmdFillBuffer(cmd, mMeshMarks.buffer, 0, VK_WHOLE_SIZE, 0u);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    if (needRing) mRingRes = ringRes;
    if (needInst) mInstRows = newRows;
    if (needAv)
    {
        registerMoverProbe();
        sMoverStats.bytes = kMoverHeadBytes + kMoverVolBytes + (uint64_t)kStampSlots * 64u + 2u * kMoverHeadBytes;
        // The motion state starts invalid (zeros), the bits empty, no stamps.
        vkCmdFillBuffer(cmd, mAvState.buffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, mAvBits.buffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, mStamps.buffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, mMovers.buffer, 0, VK_WHOLE_SIZE, 0u);   // (M5: an empty list)
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // The new images cleared (the grids to nothing, the bases to "no base", the instances to
    // -1 = "static") and left in GENERAL: the compute writes them as storage, binding 49
    // samples the grids.
    {
        GpuImage* ims[8] = {};
        VkClearColorValue clr[8] = {};
        uint32_t n = 0;
        if (needRing)
            for (GpuImage& im : mRing)
            {
                clr[n].float32[0] = 1e30f;   // (no base, no depth, no support cap)
                clr[n].float32[2] = 1e30f;
                ims[n++] = &im;
            }
        if (needCover)
        {
            ims[n++] = &mCover[0]; ims[n++] = &mCover[1];
            for (GpuImage& im : mSnowBase)
            {
                for (float& v : clr[n].float32) v = 1e30f;   // (clean_weather_snow.comp kNoBase)
                ims[n++] = &im;
            }
        }
        if (needInst)
        {
            for (float& v : clr[n].float32) v = -1.f;
            ims[n++] = &mInstSnow;
        }
        if (needAv) ims[n++] = &mBodyStandIn;   // (#783: zero -- "no layer"; never read)
        VkImageMemoryBarrier2 ib[8]{};
        for (uint32_t i = 0; i < n; ++i)
        {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib[i].image = ims[i]->image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = n; dep.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (uint32_t i = 0; i < n; ++i)
            vkCmdClearColorImage(cmd, ims[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr[i], 1, &rng);
        for (uint32_t i = 0; i < n; ++i)
        {
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    // The own set: 0 the two grids, 1 the per-instance image (storage, GENERAL), M3 2 - 4 the
    // avatar buffers; and the mesh set's grids.
    writeSnowSets();
    LL_INFOS("VKWeather") << "weather snow cover: 2 x " << res << "^2 rgba32f + 2 x " << res << "^2 rg32f (bases + support caps) + 2 x "
                          << mRingRes << "^2 rgba32f (outer ring) + " << kInstSnowWidth << " x " << mInstRows
                          << " (+2 avatar rows) rgba32f (instances / avatars)" << LL_ENDL;
}

// This recording's snow work (after the maps' schedule; the frame info rows 10 - 12 follow
// the published grid). A grid integrated last recording is published here; this recording
// writes the other one in the near map's PUBLISHED frame (the frame its readers see).
void LLCleanWeatherVK::planSnow(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<bool> s_cover(gSavedSettings, "RenderVKWeatherSnowCover", true);
    static LLCachedControl<bool> s_movers(gSavedSettings, "RenderVKWeatherSnowMovers", true);
    static LLCachedControl<F32>  s_build(gSavedSettings, "WeatherSnowBuildSeconds", 600.f);
    // M3
    static LLCachedControl<bool> s_depth(gSavedSettings, "RenderVKWeatherSnowDepth", true);
    static LLCachedControl<bool> s_drift(gSavedSettings, "RenderVKWeatherSnowDrift", true);
    static LLCachedControl<bool> s_slide(gSavedSettings, "RenderVKWeatherSnowSlide", true);
    static LLCachedControl<bool> s_settle(gSavedSettings, "RenderVKWeatherSnowSettle", true);
    static LLCachedControl<bool> s_foot(gSavedSettings, "RenderVKWeatherSnowFootprints", true);
    static LLCachedControl<bool> s_avatars(gSavedSettings, "RenderVKWeatherSnowAvatars", true);
    static LLCachedControl<bool> s_limbs(gSavedSettings, "RenderVKWeatherSnowLimbs", true);
    static LLCachedControl<F32>  s_limbRange(gSavedSettings, "RenderVKWeatherSnowLimbRange", 24.f);
    static LLCachedControl<bool> s_body(gSavedSettings, "RenderVKWeatherSnowBodyContact", true);
    mCoverDue = mInstDue = mAvDue = mStampDue = false;
    mMoverDue = mLimbDue = mBodyDue = false;
    if (mCoverReady >= 0)
    {
        mCoverPub = mCoverReady;
        std::memcpy(mCoverPubF, mCoverReadyF, sizeof(mCoverPubF));
        mCoverReady = -1;
        ++mCoverGen;   // (#946: the mesh patches each published grid's local edits)
    }
    mRingDue = false;
    if (mRingReady >= 0)
    {
        mRingPub = mRingReady;
        std::memcpy(mRingPubF, mRingReadyF, sizeof(mRingPubF));
        mRingReady = -1;
    }
    // Cover grid switched off: unpublish it (the surfaces fall back to the global cover x the
    // openness) instead of leaving the last integrated grid frozen in place; switched back on,
    // it starts fresh.
    if (!s_cover) { mCoverPub = mCoverReady = -1; mCoverFresh = true; }
    const SLWeather::State& st = SLWeather::instance().state();
    // Only while it snows or snow lies (the state's cover; 0 with WeatherSnowCoating off).
    const bool snowy = st.snow > SLWeather::kEpsilon || st.snowCover > SLWeather::kEpsilon;
    const bool pipes = mCoverPipe && mInstPipe;
    ensureSnow(cmd, snowy && pipes && mOcclOn && (s_cover || s_movers || s_avatars), f.instanceCount);
    if (!mCover[0].image || !pipes) return;
    // #895: a camera cut no longer drops the grids. The cut is the renderer's TEMPORAL one (a 20 m
    // camera jump, a snap turn, a fresh instance stream, a lost frame -- and an origin shift in the
    // legacy frame): none of those moves the snow. A region crossing re-bases the frames
    // (shiftOrigin), a teleport starts the ground over (resetGround); any other jump carries the
    // grid by world position like a snap step (texels that enter start at the global depth).
    if (!ringWanted() || !s_depth || !s_cover) { mRingPub = mRingReady = -1; mRingFresh = true; mRingDt = 0.f; }
    if (f.offscreen) return;   // (a snapshot: the cover holds still)
    const OcclMap& nm = mMap[OCCL_NEAR];
    // This step's build (the state's own rate: SLWeather::update) and the cap (the global,
    // open-sky cover: everything melts with it).
    const bool falling = st.snow > kSnowFalling;
    float build = falling ? mDt / std::max((float)s_build, 1.f) * std::min(st.snow * 2.f, 1.f) : 0.f;
    // #783 SHARED SNOW DEPTH: the grid's cover rises at least as fast as the global cover (a parcel
    // tag's level switches the cover on at once on arrival, or over its re-sync fade) -- the same
    // rule as the depth below.
    if (mPrevGlobalCover >= 0.f) build = std::max(build, st.snowCover - mPrevGlobalCover);
    mPrevGlobalCover = st.snowCover;
    const float sxy = std::sqrt(nm.pub[6] * nm.pub[6] + nm.pub[7] * nm.pub[7]);
    const float tol = std::max(kCoverTolMin, kCoverTolTexels * nm.pub[5] * (1.f + sxy));
    if (s_cover && nm.pubIdx >= 0 && nm.img[0].image)
    {
        mCoverTarget = mCoverPub >= 0 ? (uint32_t)(mCoverPub ^ 1) : 0u;
        std::memcpy(mCoverPendF, nm.pub, sizeof(mCoverPendF));
        mCoverDue = true;
    }
    const bool mapsOk = mMap[OCCL_COARSE].pubIdx >= 0 || (nm.pubIdx >= 0 && nm.img[0].image);
    mInstDue = s_movers && f.instanceCount > 0 && mapsOk;
    // M3 AVATARS / FOOTPRINTS: one pass over the joint-table slots (the footprints need the
    // grid this recording writes).
    const bool foot = s_foot && s_depth && mCoverDue;
    // LIMB CONTACT (#783 follow-up): the same pass appends the limbs that reach the snow to the
    // stamp list (the cover pass presses them, the berm pass lays their rims).
    const bool limbs = s_limbs && s_depth && mCoverDue && s_limbRange > 0.f && mMovers.buffer;
    mAvDue = mAvPipe && mapsOk && (s_avatars || foot || limbs) && mAvBits.buffer && mAvState.buffer && mStamps.buffer;
    mStampDue = mAvDue && foot && mStampPipe;
    if (mAvDue) gatherAvatarBits();

    // M3 DEPTH: this step's snowfall (m), melt (m) and transport shares.
    uint32_t feat = 0;
    if (s_depth) feat |= kFDepth;
    if (s_depth && s_drift) feat |= kFDrift;
    if (s_depth && s_slide) feat |= kFSlide;
    if (s_depth && s_settle) feat |= kFSettle;
    if (mStampDue) feat |= kFFoot;
    if (s_avatars) feat |= kFAvatar;
    // M5 MOVER TRACKS: the instance pass stamps moving boxes into this step's grid (and the ring)
    // while the tracker has a recently moved caster within the field's reach.
    const bool movers = mInstDue && mCoverDue && s_depth && mMovers.buffer;
    if (movers || (limbs && mAvDue)) planMovers(f, movers, limbs && mAvDue);
    if (mMoverDue) feat |= kFMover;
    if (mLimbDue) feat |= kFLimb;
    // #783 BODY CONTACT: the avatars with a shelter layer press with their bottom maps (the limb
    // list carries their boxes; the shelter's raster target is the snow set's binding 8).
    mBodyDue = mLimbDue && s_body && mShH.image && mShH.view && mShConfig != 3u;   // (3: kShConfigNone)
    if (mBodyDue) feat |= kFBody;
    // #783 SHARED SNOW DEPTH: the state's own rates (the settings x the intensity, or a parcel tag's
    // rate= -- exactly, so the grid keeps step with the shared level; a held tag depth melts nothing).
    float depthBuild = mDt * std::max(st.snowDepthBuild, 0.f);
    float depthMelt = mDt * std::max(st.snowDepthMelt, 0.f);
    // #895: the grid melts at least as fast as the global depth drops -- a move into a place
    // without snow fades the global depth over WeatherCrossingFadeSeconds (slweather.h PLACES);
    // through the drift cap alone (kDriftCap x the global) the level snow would only start to
    // drop in the fade's last third. (At the natural rate both are the same formula.)
    // #783: and it fills at least as fast as the global depth rises (a parcel tag's re-sync fade
    // up to its shared level; the shader's fill is clamped to the level, so it never overshoots).
    if (mPrevGlobalDepth >= 0.f)
    {
        depthMelt = std::max(depthMelt, mPrevGlobalDepth - st.snowDepth);
        depthBuild = std::max(depthBuild, st.snowDepth - mPrevGlobalDepth);
    }
    mPrevGlobalDepth = st.snowDepth;
    // Drift: the mean wind past kDriftWindMin, quantized to the 8 grid directions.
    int32_t step[2] = {0, 0};
    float driftShare = 0.f;
    {
        const float wx = st.windMean[0], wy = st.windMean[1];
        const float ws = std::sqrt(wx * wx + wy * wy);
        if (ws > kDriftWindMin)
        {
            const float a = std::atan2(wy, wx);
            const int oct = ((int)std::lround(a / 0.78539816f) + 8) & 7;
            static const int32_t kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
            step[0] = kDir[oct][0]; step[1] = kDir[oct][1];
            driftShare = std::min(kDriftRate * (ws - kDriftWindMin) * mDt, 0.25f);
        }
    }
    // The push block (clean_weather_snow.comp SnowPush, 128 B).
    float* p = mSnowPush;
    std::memset(mSnowPush, 0, sizeof(mSnowPush));
    p[0] = (float)mCoverTarget; p[1] = (float)mCoverRes; p[2] = build; p[3] = st.snowCover;
    p[4] = tol; p[5] = (mCoverFresh || mCoverPub < 0) ? 1.f : 0.f; p[6] = mDt; p[7] = kShedSpeed;
    const uint32_t c[4] = {std::min(f.instanceCount, kInstSnowWidth * mInstRows), kInstSnowWidth, mInstRows, feat};
    std::memcpy(&p[8], c, sizeof(c));
    p[12] = st.snowDepth; p[13] = depthBuild; p[14] = depthMelt; p[15] = driftShare;
    const int32_t e[4] = {step[0], step[1], (int32_t)kMaxStamps, 0};
    std::memcpy(&p[16], e, sizeof(e));
    // (f.x the repose's avalanche share -- not frame-time scaled --, 0 with settle and slide both
    // off; f.y #783 tan of the angle of repose)
    p[20] = (feat & (kFSettle | kFSlide)) ? kAvalancheShare : 0.f; p[21] = snowReposeTan(); p[22] = kDriftCap;
    p[23] = mLimbDue ? llclamp((F32)s_limbRange, 0.f, 64.f) : 0.f;           // f.w: the limbs' range from the camera (m)
    p[24] = kAvStillSpeed; p[25] = kAvShedSpeed; p[26] = kAvShedVSpeed;
    // M3 OUTER RING: the published ring's frame for the inner grid's entering texels (h: x0, y0,
    // +-1 / extent -- the sign names the ring, 0 none -- top; g.w / e.w its shear).
    if (mRingPub >= 0 && mRing[0].image)
    {
        p[28] = mRingPubF[0]; p[29] = mRingPubF[1];
        p[30] = (mRingPub == 0 ? 1.f : -1.f) / std::max(mRingPubF[2], 1e-3f); p[31] = mRingPubF[3];
        p[27] = mRingPubF[6];
        std::memcpy(&p[19], &mRingPubF[7], 4);   // (e.w: the bits of sy)
    }
    planRing(f, tol, depthBuild, depthMelt, feat);
}

// #895 ORIGIN RE-BASE (llcleanweathervk.h): every agent-space frame the module keeps moves by off.
// The images hold depths relative to their frame's top plane / heights along z (unchanged by a
// horizontal shift), so they stay valid as they are. The pending occlusion frame is re-planned by
// prepare(); the drips (agent-space particles in the pool) are dropped -- they live a second.
void LLCleanWeatherVK::shiftOrigin(const float off[3])
{
    if (!off) return;
    if (std::fabs(off[2]) > 1e-3f)
    {
        resetGround("a vertical agent-origin shift");
        return;
    }
    const float dx = off[0], dy = off[1];
    if (dx == 0.f && dy == 0.f) return;
    auto frame = [dx, dy](float* F) { F[0] += dx; F[1] += dy; };   // (x0, y0, extent, top, span, texel, sx, sy)
    for (OcclMap& m : mMap)
    {
        frame(m.pend); frame(m.ready); frame(m.pub);
        m.cell[0] += dx; m.cell[1] += dy;            // (1e30: none yet -- stays "none")
        m.pendCell[0] += dx; m.pendCell[1] += dy;
    }
    frame(mCoverPubF); frame(mCoverReadyF); frame(mCoverPendF);
    frame(mRingPubF); frame(mRingReadyF); frame(mRingPendF);
    frame(mFlowF);
    mDripsClean = false;
    mMeshDirty = true;   // (#946: the mesh's world-space vertices are in the old frame)
    LL_INFOS("VKWeather") << "weather: maps re-based by the agent-origin shift (" << dx << ", " << dy << ") -- snow grid "
                          << (mCoverPub >= 0 ? "kept" : "none") << ", outer ring " << (mRingPub >= 0 ? "kept" : "none")
                          << ", roof maps kept" << LL_ENDL;
}

// #895 NEW PLACE (llcleanweathervk.h): the ground starts over; the module stays.
void LLCleanWeatherVK::resetGround(const char* why)
{
    LL_INFOS("VKWeather") << "weather: ground maps reset (" << (why ? why : "new place") << ") -- snow grid "
                          << (mCoverPub >= 0 ? "dropped" : "none") << ", outer ring " << (mRingPub >= 0 ? "dropped" : "none")
                          << ", run-off flow / drips and particles restart; the module stays" << LL_ENDL;
    mCoverPub = mCoverReady = -1; mCoverFresh = true;
    mRingPub = mRingReady = -1; mRingFresh = true; mRingDt = 0.f;
    for (OcclMap& m : mMap) m.forced = true;   // (the roof maps re-render at the destination)
    mFlowValid = false;
    mDripsClean = false;
    mMeshDirty = true;   // (#946)
    mPrevGlobalDepth = -1.f;
    mPrevGlobalCover = -1.f;
    mReset = true;                              // (the precipitation reseeds around the new camera)
}

// M3 OUTER RING: this recording's ring step, when due. The ring lies on the PUBLISHED coarse
// map's top plane and shear (its columns), centred on the camera's column snapped to an
// eighth of its extent (a whole number of texels: the step carries exact depths); it steps
// every RenderVKWeatherSnowRingInterval recordings with the time since its last step, and at
// once when it moved a cell, the coarse map changed frame or it has nothing published.
void LLCleanWeatherVK::planRing(const Frame& f, float tol, float depthBuild, float depthMelt, uint32_t feat)
{
    static LLCachedControl<U32> s_every(gSavedSettings, "RenderVKWeatherSnowRingInterval", 4);
    static LLCachedControl<bool> s_depth(gSavedSettings, "RenderVKWeatherSnowDepth", true);
    const OcclMap& cm = mMap[OCCL_COARSE];
    if (!ringWanted() || !s_depth || !mRingPipe || !mRing[0].image || cm.pubIdx < 0 || !mCoverDue) return;
    mRingDt += mDt;
    const float E = ringExtent();
    const float top = cm.pub[3], sx = cm.pub[6], sy = cm.pub[7];
    // The camera's column on the coarse map's top plane, snapped.
    const double snap = (double)E / 8.0;
    const double zr = (double)groundUnderCamera(f);   // (as the maps: the ground's column)
    const double colX = (double)f.camTime[0] - ((double)top - zr) * sx;
    const double colY = (double)f.camTime[1] - ((double)top - zr) * sy;
    const float cx = (float)(std::floor(colX / snap + 0.5) * snap), cy = (float)(std::floor(colY / snap + 0.5) * snap);
    const float x0 = cx - 0.5f * E, y0 = cy - 0.5f * E;
    const bool have = mRingPub >= 0;
    const bool moved = !have || x0 != mRingPubF[0] || y0 != mRingPubF[1] || E != mRingPubF[2];
    const bool frame = !have || top != mRingPubF[3] || sx != mRingPubF[6] || sy != mRingPubF[7];
    ++mRingAge;
    if (!(mRingFresh || moved || frame || mRingAge >= std::max<U32>(s_every, 1u))) return;
    mRingAge = 0;
    mRingTarget = have ? (uint32_t)(mRingPub ^ 1) : 0u;
    const float fr[8] = {x0, y0, E, top, cm.pub[4], E / (float)mRingRes, sx, sy};
    std::memcpy(mRingPendF, fr, sizeof(mRingPendF));
    // The ring's push block (clean_weather_snow.comp RingPush, 128 B): the time since its last
    // step (its snowfall / melt / repose share), its new and previous frames.
    const float k = mDt > 1e-6f ? mRingDt / mDt : 1.f;
    float* p = mRingPush;
    std::memset(mRingPush, 0, sizeof(mRingPush));
    p[0] = (float)mRingTarget; p[1] = (float)mRingRes; p[2] = (mRingFresh || !have) ? 0.f : 1.f + (float)mRingPub; p[3] = tol;
    std::memcpy(&p[4], fr, sizeof(fr));
    if (have) std::memcpy(&p[12], mRingPubF, sizeof(mRingPubF));
    p[20] = SLWeather::instance().state().snowDepth; p[21] = depthBuild * k; p[22] = depthMelt * k;
    p[23] = kAvalancheShare;
    std::memcpy(&p[24], &feat, 4);
    p[25] = kDriftCap;
    p[26] = snowReposeTan();   // (m.z, #783)
    mRingDt = 0.f;
    mRingDue = true;
}

// M3: the alive / sitting bit of every avatar joint-table slot (the skin store's slots; an
// avatar or animesh object that is gone or dead is not alive). One pass over the assigned
// slots per recording while the avatar pass runs.
void LLCleanWeatherVK::gatherAvatarBits()
{
    std::memset(mAvBitsCpu, 0, sizeof(mAvBitsCpu));
    static std::vector<std::pair<LLUUID, U32>> s_slots;
    LLCleanSkinStore::instance().copyAvatarSlots(s_slots);
    for (const auto& kv : s_slots)
    {
        const U32 slot = kv.second;
        if (slot >= kAvSlots) continue;
        LLViewerObject* obj = gObjectList.findObject(kv.first);
        LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
        if (!av || av->isDead()) continue;
        mAvBitsCpu[slot >> 5] |= 1u << (slot & 31u);
        if (av->isSitting()) mAvBitsCpu[32u + (slot >> 5)] |= 1u << (slot & 31u);
    }
}

void LLCleanWeatherVK::recordSnow(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0)
{
    if (!snowDue() || !mSnowLayout || !mSnowSet || !dsetDraw0) return;
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (mCoverDue && mMeshMarks.buffer && mCoverRes)
    {
        // #946 the snow mesh's local-edit mask of the grid this step writes, cleared -- after its
        // last readers (the mesh patch's expand step two recordings ago, its indirect dispatches)
        // and before the cover / berm / stamp passes set it.
        const VkDeviceSize W = meshMaskWords(mCoverRes);
        memBarrier(cmd, C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, mMeshMarks.buffer, ((VkDeviceSize)kMkHead + mCoverTarget * W) * 4u, W * 4u, 0u);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    if (mAvDue || listDue())
    {
        // M3: this recording's avatar bits and an empty stamp list (head: 0 workgroups, 1, 1,
        // 0 appends); M5: an empty mover list + this step's parameters -- after the last
        // recordings' readers (the avatar pass, the stamps' indirect dispatch, the cover / ring
        // passes, the mover head's readback copy).
        memBarrier(cmd, C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);   // (update + fill)
        if (mAvDue)
        {
            vkCmdUpdateBuffer(cmd, mAvBits.buffer, 0, sizeof(mAvBitsCpu), mAvBitsCpu);
            const uint32_t head[4] = {0u, 1u, 1u, 0u};
            vkCmdUpdateBuffer(cmd, mStamps.buffer, 0, sizeof(head), head);
        }
        if (listDue())
        {
            // head + parameters + zeroed totals, and the per-box volume counters (disjoint ranges)
            vkCmdUpdateBuffer(cmd, mMovers.buffer, 0, sizeof(mMoverHead), mMoverHead);
            vkCmdFillBuffer(cmd, mMovers.buffer, kMoverHeadBytes, kMoverVolBytes, 0u);
        }
        memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // Earlier readers (last recordings' and this recording's early lit / G-buffer FS, the
    // last instance pass, M3 this recording's mesh build) -> this pass's writes.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet sets[2] = {dsetDraw0, mSnowSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSnowLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mSnowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mSnowPush), mSnowPush);
    bool moverCopied = false;
    if (mInstDue)
    {
        // The movers' own snow FIRST (M5: it appends this step's mover stamps, which the cover
        // and ring passes below gather). It reads only the PUBLISHED grid / ring and writes the
        // per-instance image, which the cover pass never reads: the order changes nothing else.
        uint32_t n = 0;
        std::memcpy(&n, &mSnowPush[8], sizeof(n));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mInstPipe);
        if (n) vkCmdDispatch(cmd, (n + 63u) / 64u, 1, 1);
    }
    if (mAvDue)
    {
        // M3: every joint-table slot (its own image rows; the published maps / grid only), BEFORE
        // the cover pass: #783 follow-up, it appends the limb boxes the cover / ring / berm passes
        // gather (its own list range: no ordering against the instance pass's appends). The
        // footprints' stamp list is consumed after the cover pass (below).
        // (#783 BODY CONTACT: + one workgroup per shelter layer -- its contacts from its bottom map)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAvPipe);
        vkCmdDispatch(cmd, (kAvSlots + 63u) / 64u + (mBodyDue ? kShLayers : 0u), 1, 1);
    }
    if (listDue())   // the list -> the cover / ring gathers (their presses count into it)
        memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (mCoverDue)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCoverPipe);
        vkCmdDispatch(cmd, (mCoverRes + 7u) / 8u, (mCoverRes + 7u) / 8u, 1);
        mCoverReady = (int)mCoverTarget;
        std::memcpy(mCoverReadyF, mCoverPendF, sizeof(mCoverReadyF));
        mCoverFresh = false;
        if (listDue())
        {
            // M5 BERMS: the presses' writes (grid + per-box removed volumes) -> the berm gather,
            // in place on the grid just written (each texel writes only itself).
            memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            if (mBermPipe)   // (the cover's push block, still bound)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mBermPipe);
                vkCmdDispatch(cmd, (mCoverRes + 7u) / 8u, (mCoverRes + 7u) / 8u, 1);
            }
        }
    }
    if (mRingDue)
    {
        // M3 OUTER RING (its own push block; reads the published inner grid and ring, writes
        // the other ring: no barrier against the cover pass).
        vkCmdPushConstants(cmd, mSnowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mRingPush), mRingPush);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRingPipe);
        vkCmdDispatch(cmd, (mRingRes + 7u) / 8u, (mRingRes + 7u) / 8u, 1);
        vkCmdPushConstants(cmd, mSnowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mSnowPush), mSnowPush);
        mRingReady = (int)mRingTarget;
        std::memcpy(mRingReadyF, mRingPendF, sizeof(mRingReadyF));
        mRingFresh = false;
        mRingDue = false;
    }
    if (mAvDue)
    {
        if (mStampDue && mCoverReady >= 0)
        {
            // The footprints press into the grid the cover pass just wrote: its writes and the
            // stamp list -> the stamps' reads / writes and indirect dispatch.
            memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                           | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mStampPipe);
            vkCmdDispatchIndirect(cmd, mStamps.buffer, 0);
        }
    }
    // -> the lit / G-buffer FS (binding 49) and the next recordings' passes (M3: the mesh build
    // reads the grid as a storage image).
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (listDue() && mMoverRead[mSlot].buffer)
    {
        // M5: the head + totals (after the presses, the berms and the ring) -> this slot's readback.
        memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy rc{0, 0, kMoverHeadBytes};
        vkCmdCopyBuffer(cmd, mMovers.buffer, mMoverRead[mSlot].buffer, 1, &rc);
        moverCopied = true;
    }
    if (moverCopied)
    {
        // M5: the head's copy -> the host (read when this frame slot comes round again).
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mMoverReadWritten[mSlot] = true;
    }
    mCoverDue = mInstDue = mAvDue = mStampDue = false;
    mMoverDue = mLimbDue = mBodyDue = false;
}

// ---- M3 SNOW SURFACE MESH ----
void LLCleanWeatherVK::destroyMesh()
{
    if (mCtx)
        for (GpuBuffer* b : {&mMeshVerts, &mMeshIdx, &mMeshArgs})
            if (b->buffer) mCtx->destroyBuffer(*b);
    mMeshVerts = mMeshIdx = mMeshArgs = GpuBuffer{};
    freeDump(false);   // #783 (callers waited for the device)
    mMeshRes = mMeshRingRes = mMeshTris = 0;
    mMeshBuilt = false;
    mMeshValid = false;      // (#946)
    mMeshBuildDue = false;
    mMeshDirty = true;
}

// The mesh's buffers (two vertices per grid texel -- the snow top and the bare surface, a
// skirt's foot -- 16 B each: 8 MB at 512^2; kMeshMaxTris x 12 B of indices: 6 MB; the
// indirect arguments) while wanted, re-created with the grid's resolution
// (device wait: frames in flight draw them). A refused allocation keeps the coating alone
// (#697) and retries later. true = they exist.
bool LLCleanWeatherVK::ensureMesh(VkCommandBuffer cmd, bool want)
{
    if (!want)
    {
        if (mMeshVerts.buffer)
        {
            wxWaitIdle(mCtx->device(), "snow mesh freed");
            destroyMesh();
            LL_INFOS("VKWeather") << "weather snow mesh freed" << LL_ENDL;
        }
        return false;
    }
    if (mMeshVerts.buffer && mMeshRes == mCoverRes && mMeshRingRes == mRingRes) return true;
    if (mNow < mMeshRetryAt) return false;
    wxWaitIdle(mCtx->device(), "snow mesh");
    destroyMesh();
    bool ok = true;
    // #946: the index buffer follows the grid / ring resolutions (the defaults' triangles per
    // cell): the SMALL VRAM tier's 256^2 + 128^2 grids take a quarter of the 9 MB.
    const uint32_t triCap = meshTriCap();
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        // (#783 TRANSFER_SRC: the snow dump copies the mesh out)
        const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        // (three 16 B vertices per texel of each layer: the snow top, the skirt's base, #783 the apron;
        // + #783 round 10 the SIDE ARRAY: one word per texel, the apron's drop rate, at the very end --
        // clean_weather_snowmesh.comp aprSideWord indexes it from the buffer's end, ring then inner)
        const VkDeviceSize texels = (VkDeviceSize)mCoverRes * mCoverRes + (VkDeviceSize)mRingRes * mRingRes;
        mMeshVerts = mCtx->createBuffer(texels * 48u + (texels + 3u) / 4u * 16u,
                                        su | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mMeshIdx = mCtx->createBuffer((VkDeviceSize)triCap * 12u, su | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // (+ #946 the inner layer's tile table: first triangle, capacity per 8 x 8 tile)
        const VkDeviceSize tiles = (VkDeviceSize)meshTiles(mCoverRes) * meshTiles(mCoverRes);
        mMeshArgs = mCtx->createBuffer(32u + tiles * 8u, su | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // #783 the snow dump's binding while not armed (planDump grows it)
        mDumpBuf = mCtx->createBuffer(256u, su | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = mMeshVerts.buffer && mMeshIdx.buffer && mMeshArgs.buffer && mDumpBuf.buffer;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather snow mesh (" << mCoverRes << "^2 vertices) failed to allocate; the snow stays a "
                              << "coating, retrying in " << kAllocRetrySecs << " s" << LL_ENDL;
        destroyMesh();
        mMeshRetryAt = mNow + kAllocRetrySecs;
        return false;
    }
    mMeshRes = mCoverRes;
    mMeshRingRes = mRingRes;
    mMeshTris = triCap;
    // (an empty draw until the first build)
    vkCmdFillBuffer(cmd, mMeshArgs.buffer, 0, VK_WHOLE_SIZE, 0u);
    // ALL_TRANSFER on the second scope: the next writer is recordMeshBuild's vkCmdUpdateBuffer,
    // a CLEAR-stage command (COPY alone left a validation WAW hazard).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                   | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    writeSnowSets();
    LL_INFOS("VKWeather") << "weather snow mesh: 3 x (" << mCoverRes << "^2 + " << mRingRes << "^2) vertices + " << triCap
                          << " triangles (" << (((uint64_t)mCoverRes * mCoverRes + (uint64_t)mRingRes * mRingRes) * 48u
                                                + (uint64_t)triCap * 12u) / (1024u * 1024u) << " MB)" << LL_ENDL;
    return true;
}

// This recording's mesh, from the PUBLISHED grid (prepare(): before the cascades and the
// G-buffer read it; never binds draw set 0 -- its own set names the grids).
// #946 THE BUILD'S CADENCE (Beta 5/6 regression, #946: the build ran every recording, and its
// apron / closure dilation -- RenderVKWeatherSnowToeReach -- cost a 3050 Laptop more than the rest
// of the frame). The vertices are world-space and the vertex shader reads nothing else, so a build
// stays drawable until what it was built FOR changes: the grid / ring frame (a snap), the camera
// (the stride morphs, the hand-over band and the lift follow it: RenderVKWeatherSnowMeshRebuildMove),
// the view direction (the cells' view cone: kMeshRebuildTurnCos), a camera cut, an origin shift /
// ground reset, a snapshot (its own camera), a re-created buffer. The snow itself (accumulation,
// melt, footprints, movers) changes a little every recording: it is picked up at least every
// RenderVKWeatherSnowMeshInterval recordings (1: every recording, the old behaviour).
// #946 LOCAL PATCHES (RenderVKWeatherSnowMeshPatch): what the snow passes changed LOCALLY -- a
// footprint, an avatar / a vehicle ploughing, a limb / body press, their berms -- is not left to the
// interval: every recording that does not rebuild patches the tiles around the published grid's
// marks (recordMeshPatch; clean_weather_snowmesh.comp LOCAL PATCHES), with the last build's push
// block so the patched tiles agree with their neighbours. A published grid that neither a build nor
// a patch saw (its marks are gone) forces a build.
void LLCleanWeatherVK::planMeshBuild(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<bool> s_mesh(gSavedSettings, "RenderVKWeatherSnowMesh", true);
    static LLCachedControl<bool> s_depth(gSavedSettings, "RenderVKWeatherSnowDepth", true);
    static LLCachedControl<bool> s_cover(gSavedSettings, "RenderVKWeatherSnowCover", true);
    static LLCachedControl<bool> s_shadow(gSavedSettings, "RenderVKWeatherSnowMeshShadows", true);
    static LLCachedControl<F32>  s_toe(gSavedSettings, "RenderVKWeatherSnowToeReach", 1.f);           // #783 (the apron)
    static LLCachedControl<F32>  s_ringSup(gSavedSettings, "RenderVKWeatherSnowRingMinSupport", 1.f);  // #783
    static LLCachedControl<bool> s_smooth(gSavedSettings, "RenderVKWeatherSnowMeshSmooth", true);      // #783
    static LLCachedControl<F32>  s_ringCov(gSavedSettings, "RenderVKWeatherSnowRingMinCoverage", 0.6f); // #783
    static LLCachedControl<U32>  s_every(gSavedSettings, "RenderVKWeatherSnowMeshInterval", 4);
    static LLCachedControl<F32>  s_move(gSavedSettings, "RenderVKWeatherSnowMeshRebuildMove", 0.25f);
    static LLCachedControl<bool> s_patch(gSavedSettings, "RenderVKWeatherSnowMeshPatch", true);      // #946
    static LLCachedControl<U32>  s_patchTiles(gSavedSettings, "RenderVKWeatherSnowMeshPatchTiles", 1024);
    mMeshBuilt = false;
    mMeshBuildDue = false;
    mMeshPatchDue = false;
    mMeshShadowOn = s_shadow;
    const SLWeather::State& st = SLWeather::instance().state();
    const bool want = s_mesh && s_depth && s_cover && mMeshBuildPipe && mMeshGbufPipe && mMeshSet && mMeshLayout
                   && mCover[0].image && st.snowDepth > kMeshMinDepth;
    if (!ensureMesh(cmd, want) || mCoverPub < 0 || !f.viewProj) return;
    float dir[3];
    {
        // The view direction: the projection's w row (w = -z_view; column-major).
        const float* M = f.viewProj;
        dir[0] = M[3]; dir[1] = M[7]; dir[2] = M[11];
        const float n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (n > 1e-8f) { for (float& v : dir) v /= n; } else { dir[0] = dir[1] = dir[2] = 0.f; }
    }
    const bool ring = mRingPub >= 0 && mRing[0].image && mMeshRingRes == mRingRes;
    const uint32_t ageBefore = mMeshAge;
    const bool dumpNow = planDump(f, ring);   // #783 the snow dump: a full build, recorded
    if (dumpNow) mDumpMeshAge = ageBefore;
    bool due = dumpNow || !mMeshValid || mMeshDirty || f.offscreen || f.cameraCut || ring != mMeshRingNow;
    // #946 the local patches: the marks of the grid published since the last build / patch
    const bool patchOn = s_patch && !f.offscreen && mMeshMarks.buffer && mMeshRes == mCoverRes;
    if (!due && patchOn && mCoverGen - mMeshGen > 1u) due = true;   // (a grid's marks were never seen)
    if (!due)
    {
        const float dx = f.camTime[0] - mMeshCam[0], dy = f.camTime[1] - mMeshCam[1], dz = f.camTime[2] - mMeshCam[2];
        const float mv = llclamp((F32)s_move, 0.f, 4.f);
        const float dot = dir[0] * mMeshDir[0] + dir[1] * mMeshDir[1] + dir[2] * mMeshDir[2];
        due = dx * dx + dy * dy + dz * dz > mv * mv
           || dot < kMeshRebuildTurnCos
           || std::memcmp(mCoverPubF, mMeshCoverF, sizeof(mMeshCoverF)) != 0
           || (ring && std::memcmp(mRingPubF, mMeshRingF, sizeof(mMeshRingF)) != 0)
           || mMeshAge + 1u >= std::max<uint32_t>((U32)s_every, 1u);
    }
    if (!due)
    {
        // (the last build stays drawn; the snow's own slow change waits for the interval)
        ++mMeshAge;
        ++sSync.meshSkips;
        mMeshBuilt = true;
        if (patchOn && mCoverGen != mMeshGen)
        {
            // #946 the local edits: the last build's push block (its camera, cone, strides and
            // bands -- the patched tiles must agree with their neighbours) with this recording's
            // grids (the same frames: else the build above was due).
            float* pp = mMeshPatchPush;
            std::memcpy(pp, mMeshPush, sizeof(mMeshPatchPush));
            pp[3] = (float)mCoverPub;
            if (mMeshRingNow && mRingPub >= 0) pp[26] = (mRingPub == 0 ? 1.f : -1.f) / std::max(mRingPubF[2], 1e-3f);
            const uint32_t nt = meshTiles(mMeshRes) * meshTiles(mMeshRes);
            mMeshPatchCap = std::max<uint32_t>(std::min<uint32_t>((U32)s_patchTiles, sSmallVram ? nt / 4u : nt), 1u);
            mMeshPatchDue = true;
            mMeshBuildDue = true;   // (recordMeshBuild records the patch: c_snow_mesh_build)
            mMeshGen = mCoverGen;
            ++sSync.meshPatches;
        }
        return;
    }
    // The push block (clean_weather_snowmesh.comp MeshPush, 128 B).
    float* mp = mMeshPush;
    std::fill(mp, mp + 32, 0.f);
    mp[0] = mCoverPubF[0]; mp[1] = mCoverPubF[1]; mp[2] = 1.f / std::max(mCoverPubF[2], 1e-3f); mp[3] = (float)mCoverPub;
    mp[4] = mCoverPubF[3]; mp[5] = mCoverPubF[6]; mp[6] = mCoverPubF[7]; mp[7] = mCoverPubF[4];
    mp[8] = f.camTime[0]; mp[9] = f.camTime[1]; mp[10] = f.camTime[2]; mp[11] = kMeshConeCos;
    mp[16] = kMeshMinDepth; mp[17] = snowReposeTan(); mp[18] = kMeshLod1; mp[19] = kMeshLod2;
    uint32_t liftBits;
    std::memcpy(&liftBits, &kMeshLift, 4);
    // #783 the options word (clean_weather_snowmesh.comp opt*): the apron's reach (cm), the smooth
    // reconstruction, the ring's least support half-width (x 0.1 m), its least support coverage (x 255).
    uint32_t opts = (uint32_t)std::lround(llclamp((F32)s_toe, 0.f, 4.f) * 100.f);
    if (s_smooth) opts |= 0x10000u;
    opts |= (uint32_t)std::lround(llclamp((F32)s_ringSup, 0.f, 12.7f) * 10.f) << 17;
    opts |= (uint32_t)std::lround(llclamp((F32)s_ringCov, 0.f, 1.f) * 255.f) << 24;
    static LLCachedControl<bool> s_depthDbg(gSavedSettings, "RenderVKWeatherSnowDepthDebug", false);   // #783 (the strides)
    // #946 u0.x bits 16..17: the BOUNDARY LOD (clean_weather_snowmesh.comp optEdgeLod;
    // RenderVKWeatherSnowMeshEdgeLod; the SMALL tier: at least 1 -- full resolution near the camera).
    static LLCachedControl<U32> s_edgeLod(gSavedSettings, "RenderVKWeatherSnowMeshEdgeLod", 0);
    const uint32_t edgeLod = std::min<uint32_t>(std::max<uint32_t>((U32)s_edgeLod, sSmallVram ? 1u : 0u), 2u);
    const uint32_t u[4] = {mCoverRes | (edgeLod << 16) | (s_depthDbg ? 0x80000000u : 0u), mMeshTris, opts, liftBits};
    std::memcpy(&mp[20], u, sizeof(u));
    // M3 OUTER RING: the published ring's frame (r0: x0, y0, +-1 / extent -- the sign names the
    // ring, 0 none -- top; r1: shear, resolution, span).
    if (ring)
    {
        mp[24] = mRingPubF[0]; mp[25] = mRingPubF[1];
        mp[26] = (mRingPub == 0 ? 1.f : -1.f) / std::max(mRingPubF[2], 1e-3f); mp[27] = mRingPubF[3];
        mp[28] = mRingPubF[6]; mp[29] = mRingPubF[7]; mp[30] = (float)mRingRes; mp[31] = mRingPubF[4];
    }
    {
        // (#946: the view direction computed above; none: keep everything)
        mp[12] = dir[0]; mp[13] = dir[1]; mp[14] = dir[2]; mp[15] = kMeshKeepRadius;
        if (dir[0] == 0.f && dir[1] == 0.f && dir[2] == 0.f) { mp[12] = 1.f; mp[11] = -2.f; }
    }
    mMeshRingNow = ring;
    // z 0: the inner grid, z 1: the outer ring (its cells only where the inner grid fades).
    mMeshGroups = (std::max(mCoverRes, ring ? mRingRes : 0u) + 7u) / 8u;
    mMeshBuildDue = true;
    mMeshBuilt = true;   // (recordMeshBuild runs before any pass of this recording draws it)
    mMeshAge = 0;
    std::copy(f.camTime, f.camTime + 3, mMeshCam);
    std::copy(dir, dir + 3, mMeshDir);
    std::memcpy(mMeshCoverF, mCoverPubF, sizeof(mMeshCoverF));
    std::memcpy(mMeshRingF, mRingPubF, sizeof(mMeshRingF));
    // A snapshot's build (its own camera) is never the live view's: the next live one rebuilds.
    mMeshDirty = f.offscreen;
    mMeshGen = mCoverGen;   // (#946: this build holds the published grid's edits)
    if (!f.offscreen) ++sSync.meshBuilds;
}

// The planned build (#946: recorded by the renderer after gpuTimersBegin -- c_snow_mesh_build).
void LLCleanWeatherVK::recordMeshBuild(VkCommandBuffer cmd)
{
    if (!mMeshBuildDue) return;
    mMeshBuildDue = false;
    if (mMeshPatchDue)
    {
        mMeshPatchDue = false;
        recordMeshPatch(cmd);
        return;
    }
    if (!mMeshVerts.buffer || !mMeshBuildPipe || !mMeshSet || !mMeshLayout || !mMeshGroups) { mMeshBuilt = false; return; }
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags2 DRAW = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
                                     | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    // The last recordings' draws (vertex / index / indirect reads) and #946 local patches (compute
    // reads / writes of the same buffers) -> the argument reset and this build's writes.
    memBarrier(cmd, DRAW | C, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
                                  | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                   | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // VkDrawIndexedIndirectCommand {0 indices, 1 instance, 0, 0, 0} + [5] the triangle counter.
    // (vkCmdUpdateBuffer runs in the CLEAR stage: ALL_TRANSFER on both sides of it.)
    const uint32_t args[8] = {0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
    // #783 THE SNOW DUMP: the mesh as drawn until now (the last build + any #946 patches since) ->
    // the staging's "pre" copy, before this build overwrites it.
    const bool dump = mDumpArmed && mDumpStage.buffer && mDumpBuf.size > 256u;
    if (dump)
    {
        memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy cv{0, mDumpOff[0], mMeshVerts.size}, ci{0, mDumpOff[1], mMeshIdx.size}, ca{0, mDumpOff[2], mMeshArgs.size};
        vkCmdCopyBuffer(cmd, mMeshVerts.buffer, mDumpStage.buffer, 1, &cv);
        vkCmdCopyBuffer(cmd, mMeshIdx.buffer, mDumpStage.buffer, 1, &ci);
        vkCmdCopyBuffer(cmd, mMeshArgs.buffer, mDumpStage.buffer, 1, &ca);
        // (the copies' reads -> the argument reset / this build's writes: WAR)
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    vkCmdUpdateBuffer(cmd, mMeshArgs.buffer, 0, sizeof(args), args);
    if (dump)
    {
        // the records cleared (an unwritten one reads 0: snowdump.py's "not written") + the head
        vkCmdFillBuffer(cmd, mDumpBuf.buffer, 0, VK_WHOLE_SIZE, 0u);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        uint32_t head[8] = {0x534E4F57u /* 'SNOW' */, mDumpW, 0u, 0u, 0u, 0u, 0u, 0u};
        std::memcpy(&head[2], mDumpOrg, sizeof(mDumpOrg));
        vkCmdUpdateBuffer(cmd, mDumpBuf.buffer, 0, sizeof(head), head);
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMeshBuildPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMeshLayout, 0, 1, &mMeshSet, 0, nullptr);
    {
        // (#783 the dump: u0.x bit 30 arms the shader's records for this build only -- mMeshPush
        // stays clean, the #946 patches copy it)
        float pb[32];
        std::memcpy(pb, mMeshPush, sizeof(pb));
        if (dump)
        {
            uint32_t u0x;
            std::memcpy(&u0x, &pb[20], 4);
            u0x |= 0x40000000u;
            std::memcpy(&pb[20], &u0x, 4);
            std::memcpy(mDumpPush, pb, sizeof(pb));
        }
        vkCmdPushConstants(cmd, mMeshLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb), pb);
    }
    // #783 two phases: the VERTICES, then the CELLS (u0.y bit 31), which read the apron vertices
    // their neighbours wrote.
    const uint32_t g = mMeshGroups;
    vkCmdDispatch(cmd, g, g, mMeshRingNow ? 2u : 1u);
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        const uint32_t cellsTris = mMeshTris | 0x80000000u;
        vkCmdPushConstants(cmd, mMeshLayout, VK_SHADER_STAGE_COMPUTE_BIT, 21u * 4u, 4u, &cellsTris);
    }
    vkCmdDispatch(cmd, g, g, mMeshRingNow ? 2u : 1u);
    // -> this recording's draws (the depth pre-pass -- RenderVKWeatherSnowMeshPrepass --, the
    // cascades and the G-buffer / forward pass).
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               DRAW, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (dump)
    {
        // #783 the snow dump: this build's mesh + records -> the staging -> the host (read by the
        // next prepare() on this frame slot, after its fence)
        memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy cv{0, mDumpOff[3], mMeshVerts.size}, ci{0, mDumpOff[4], mMeshIdx.size}, ca{0, mDumpOff[5], mMeshArgs.size},
                           cd{0, mDumpOff[6], mDumpBuf.size};
        vkCmdCopyBuffer(cmd, mMeshVerts.buffer, mDumpStage.buffer, 1, &cv);
        vkCmdCopyBuffer(cmd, mMeshIdx.buffer, mDumpStage.buffer, 1, &ci);
        vkCmdCopyBuffer(cmd, mMeshArgs.buffer, mDumpStage.buffer, 1, &ca);
        vkCmdCopyBuffer(cmd, mDumpBuf.buffer, mDumpStage.buffer, 1, &cd);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mDumpWritten = true;
        mDumpSlot = mSlot;
        LL_INFOS("VKWeather") << "snow dump: recorded (frame slot " << mSlot << "); written after its fence" << LL_ENDL;
    }
    mDumpArmed = false;
    mMeshValid = true;
}

// #946 LOCAL PATCH (clean_weather_snowmesh.comp LOCAL PATCHES): the published grid's marks -> the
// vertex / cell tile lists (EXPAND, one invocation per tile) -> the VERTICES phase over the vertex
// tiles and the CELLS phase over the cell tiles (indirect: the lists' counts; nothing marked = two
// empty dispatches). Same buffers as the build, same barrier shape: the last recordings' draws ->
// these writes -> this recording's draws (and the snow passes after it, which write the marks).
// COST: per marked tile <= 25 vertex + 49 cell workgroups (a walking avatar's feet: 1 - 2 tiles,
// overlapping), against (R / 8)^2 x 2 (+ the ring's) for a build -- at 512^2, ~2 - 3 % of the
// inner layer's workgroups per avatar; the expand step is (R / 64)^2 tiny workgroups.
void LLCleanWeatherVK::recordMeshPatch(VkCommandBuffer cmd)
{
    if (!mMeshValid || !mMeshVerts.buffer || !mMeshMarks.buffer || !mMeshBuildPipe || !mMeshSet || !mMeshLayout) return;
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags2 IND = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    const VkPipelineStageFlags2 DRAW = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | IND;
    const VkAccessFlags2 DRAW_R = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    const VkAccessFlags2 SRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // The last recordings' draws (vertex / index / indirect reads), the snow passes' marks and the
    // last patch's lists / indirect dispatches -> the head's reset and this patch's writes.
    // (vkCmdUpdateBuffer runs in the CLEAR stage: ALL_TRANSFER on both sides of it.)
    memBarrier(cmd, DRAW | C, DRAW_R | SRW, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | SRW);
    // The head: two empty dispatches {0, 1, 1}, no overflow, the list cap.
    const uint32_t head[kMkHead] = {0u, 1u, 1u, 0u, 1u, 1u, 0u, mMeshPatchCap};
    vkCmdUpdateBuffer(cmd, mMeshMarks.buffer, 0, sizeof(head), head);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, C, SRW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMeshBuildPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMeshLayout, 0, 1, &mMeshSet, 0, nullptr);
    // u0.y: the triangle budget + bit 29 EXPAND, bit 30 PATCH, bit 31 the CELLS phase
    uint32_t u0y = mMeshTris | 0x20000000u;
    std::memcpy(&mMeshPatchPush[21], &u0y, 4);
    vkCmdPushConstants(cmd, mMeshLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mMeshPatchPush), mMeshPatchPush);
    const uint32_t eg = (meshTiles(mMeshRes) + 7u) / 8u;
    vkCmdDispatch(cmd, eg, eg, 1);
    // the lists + counts -> the two phases' reads and indirect dispatches
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C | IND, SRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    u0y = mMeshTris | 0x40000000u;
    vkCmdPushConstants(cmd, mMeshLayout, VK_SHADER_STAGE_COMPUTE_BIT, 21u * 4u, 4u, &u0y);
    vkCmdDispatchIndirect(cmd, mMeshMarks.buffer, 0);
    // the vertex tiles -> the cells (they read the apron vertices)
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, SRW);
    u0y = mMeshTris | 0xC0000000u;
    vkCmdPushConstants(cmd, mMeshLayout, VK_SHADER_STAGE_COMPUTE_BIT, 21u * 4u, 4u, &u0y);
    vkCmdDispatchIndirect(cmd, mMeshMarks.buffer, 3u * 4u);
    // -> this recording's draws, and the snow passes after this (they write the marks; the
    // indirect reads above are in this barrier's first scope)
    memBarrier(cmd, C | IND, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, DRAW | C, DRAW_R | SRW);
}

// ---- #783 THE SNOW DUMP (RenderVKWeatherSnowDump; clean_weather_snowmesh.comp binding 7 / kD*;
// scripts/dev/snowdump.py). A one-shot measurement, never a per-frame cost: armed by the setting
// (it resets itself to 0), it forces a FULL build and records it (recordMeshBuild); the file is
// written by the next prepare() on the same frame slot. Window: RenderVKWeatherSnowDumpWindow
// texels square (16 .. 256) of each layer, centred on the camera's FOCUS (alt-click the spot; the
// focus's z is also the script's default "floor": alt-click the floor next to the wall).
static const uint32_t kDumpRecWords = 32u, kDumpHeadWords = 64u;
static VkDeviceSize dumpAlign(VkDeviceSize v) { return (v + 255u) & ~(VkDeviceSize)255u; }

void LLCleanWeatherVK::freeDump(bool keepPlaceholder)
{
    if (mCtx)
    {
        if (mDumpStage.buffer) mCtx->destroyBuffer(mDumpStage);
        if (mDumpBuf.buffer) mCtx->destroyBuffer(mDumpBuf);
    }
    mDumpStage = mDumpBuf = GpuBuffer{};
    mDumpArmed = mDumpWritten = false;
    if (keepPlaceholder && mCtx)
    {
        VkMemScope memcat(VKMEM_PARTICLE);
        mDumpBuf = mCtx->createBuffer(256u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
}

bool LLCleanWeatherVK::planDump(const Frame& f, bool ring)
{
    static LLCachedControl<U32> s_dump(gSavedSettings, "RenderVKWeatherSnowDump", 0);
    static LLCachedControl<U32> s_win(gSavedSettings, "RenderVKWeatherSnowDumpWindow", 128);
    if ((U32)s_dump == 0u || f.offscreen || mDumpWritten) return false;
    gSavedSettings.setU32("RenderVKWeatherSnowDump", 0);   // (one shot)
    if (!mCtx || !mMeshVerts.buffer || !mMeshIdx.buffer || !mMeshArgs.buffer || !mCoverRes || mCoverPub < 0)
    {
        LL_WARNS("VKWeather") << "snow dump: no snow mesh to dump (snow depth / mesh off, or no grid yet)" << LL_ENDL;
        return false;
    }
    const uint32_t W = llclamp((U32)s_win, 16u, 256u);
    // The window's centre: the camera's focus (agent frame), else the camera.
    float P[3] = {f.camTime[0], f.camTime[1], f.camTime[2]};
    mDumpFocusOk = false;
    {
        const LLVector3 a = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal());
        if (std::isfinite(a.mV[0]) && std::isfinite(a.mV[1]) && std::isfinite(a.mV[2]) && !a.isExactlyZero())
        {
            P[0] = a.mV[0]; P[1] = a.mV[1]; P[2] = a.mV[2];
            mDumpFocusOk = true;
        }
    }
    std::copy(P, P + 3, mDumpFocus);
    std::copy(f.camTime, f.camTime + 3, mDumpCam);
    // (the column through P on a grid's top plane: clean_weather_snowmesh.comp columnPoint inverted)
    auto org = [&](const float* F, uint32_t res, int32_t* og)
    {
        const float ext = std::max(F[2], 1e-3f);
        const float u = (P[0] - (F[3] - P[2]) * F[6] - F[0]) / ext, v = (P[1] - (F[3] - P[2]) * F[7] - F[1]) / ext;
        og[0] = (int32_t)std::floor(u * (float)res) - (int32_t)W / 2;
        og[1] = (int32_t)std::floor(v * (float)res) - (int32_t)W / 2;
    };
    org(mCoverPubF, mCoverRes, &mDumpOrg[0]);
    if (ring && mRingRes) org(mRingPubF, mRingRes, &mDumpOrg[2]);
    else mDumpOrg[2] = mDumpOrg[3] = -1000000;   // (no ring: nothing inside)
    mDumpW = W;
    const SLWeather::State& st = SLWeather::instance().state();
    mDumpGlobalDepth = st.snowDepth;
    mDumpWater = gAgent.getRegion() ? gAgent.getRegion()->getWaterHeight() : 0.f;
    // The buffers: the records (device-local, binding 7) and the host-visible staging (the mesh
    // before and after + the records). A device wait: binding 7 is rewritten (the prestep's rule).
    const VkDeviceSize recBytes = (VkDeviceSize)(kDumpHeadWords + 2u * W * W * kDumpRecWords) * 4u;
    VkDeviceSize o = 0;
    for (int k = 0; k < 2; ++k)
    {
        mDumpOff[3 * k + 0] = o; o = dumpAlign(o + mMeshVerts.size);
        mDumpOff[3 * k + 1] = o; o = dumpAlign(o + mMeshIdx.size);
        mDumpOff[3 * k + 2] = o; o = dumpAlign(o + mMeshArgs.size);
    }
    mDumpOff[6] = o; o = dumpAlign(o + recBytes);
    wxWaitIdle(mCtx->device(), "snow dump");
    freeDump(false);
    {
        VkMemScope memcat(VKMEM_PARTICLE);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mDumpBuf = mCtx->createBuffer(recBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mDumpStage = mCtx->createBuffer(o, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mDumpBuf.buffer || !mDumpStage.buffer || !mDumpStage.mapped)
    {
        LL_WARNS("VKWeather") << "snow dump: " << (o >> 20) << " MB staging failed to allocate; no dump" << LL_ENDL;
        freeDump(true);
        writeSnowSets();
        return false;
    }
    writeSnowSets();
    mDumpArmed = true;
    LL_INFOS("VKWeather") << "snow dump: armed, " << W << "^2 texels around (" << P[0] << ", " << P[1] << ", " << P[2]
                          << (mDumpFocusOk ? ") (the camera's focus)" : ") (the camera)") << ", staging " << (o >> 20) << " MB"
                          << LL_ENDL;
    return true;
}

void LLCleanWeatherVK::readDump()
{
    mDumpWritten = false;
    if (!mCtx) { freeDump(false); return; }
    if (!mDumpStage.mapped || !mMeshRes)
    {
        wxWaitIdle(mCtx->device(), "snow dump freed");
        freeDump(true);
        writeSnowSets();
        return;
    }
    const uint8_t* B = (const uint8_t*)mDumpStage.mapped;
    const uint32_t W = mDumpW, resI = mMeshRes, resR = mMeshRingRes;
    const uint64_t nvI = (uint64_t)resI * resI, nvR = (uint64_t)resR * resR;
    const uint64_t nVerts = 3u * (nvI + nvR);
    // a vertex index -> in a window (layer, slot, texel)
    auto inWin = [&](uint64_t vi) -> bool
    {
        uint64_t r;
        uint32_t res;
        const int32_t* o;
        if (vi < 3u * nvI) { r = vi % nvI; res = resI; o = &mDumpOrg[0]; }
        else if (vi < nVerts && nvR) { r = (vi - 3u * nvI) % nvR; res = resR; o = &mDumpOrg[2]; }
        else return false;
        const int64_t x = (int64_t)(r % res) - o[0], y = (int64_t)(r / res) - o[1];
        return x >= 0 && y >= 0 && x < (int64_t)W && y < (int64_t)W;
    };
    struct Sec { std::string name, dtype, shape; std::vector<uint8_t> data; };
    std::vector<Sec> secs;
    uint32_t args[2][8] = {};
    uint64_t keptTris[2] = {}, allTris[2] = {};
    for (int k = 0; k < 2; ++k)
    {
        const uint8_t* V = B + mDumpOff[3 * k + 0];
        const uint32_t* I = (const uint32_t*)(B + mDumpOff[3 * k + 1]);
        std::memcpy(args[k], B + mDumpOff[3 * k + 2], sizeof(args[k]));
        const uint64_t nt = std::min<uint64_t>(args[k][0] / 3u, mMeshIdx.size / 12u);
        allTris[k] = nt;
        std::vector<uint8_t> mark(nVerts, 0);
        Sec tri{k == 0 ? "pre_tris" : "post_tris", "u4", "", {}};
        for (uint64_t i = 0; i < nt; ++i)
        {
            const uint32_t* t = I + 3u * i;
            if ((t[0] | t[1] | t[2]) == 0u) continue;   // (a #946 patch's degenerate pad)
            if (t[0] >= nVerts || t[1] >= nVerts || t[2] >= nVerts) continue;
            if (!inWin(t[0]) && !inWin(t[1]) && !inWin(t[2])) continue;
            // (the triangle's ordinal in the index buffer first: the cell records' kDCTri map to it)
            const uint32_t w4[4] = {(uint32_t)i, t[0], t[1], t[2]};
            tri.data.insert(tri.data.end(), (const uint8_t*)w4, (const uint8_t*)w4 + 16);
            mark[t[0]] = mark[t[1]] = mark[t[2]] = 1;
            ++keptTris[k];
        }
        tri.shape = "[" + std::to_string(keptTris[k]) + ", 4]";
        for (uint64_t vi = 0; vi < nVerts; ++vi)
            if (!mark[vi] && inWin(vi)) mark[vi] = 1;
        Sec ver{k == 0 ? "pre_verts" : "post_verts", "u4", "", {}};
        uint64_t nv = 0;
        for (uint64_t vi = 0; vi < nVerts; ++vi)
        {
            if (!mark[vi]) continue;
            uint32_t w5[5];
            w5[0] = (uint32_t)vi;
            std::memcpy(&w5[1], V + vi * 16u, 16);
            ver.data.insert(ver.data.end(), (const uint8_t*)w5, (const uint8_t*)w5 + 20);
            ++nv;
        }
        ver.shape = "[" + std::to_string(nv) + ", 5]";
        secs.push_back(std::move(tri));
        secs.push_back(std::move(ver));
    }
    // The records (head + 2 x W^2 x kDumpRecWords).
    const uint64_t recWords = kDumpHeadWords + 2ull * W * W * kDumpRecWords;
    const uint32_t* R = (const uint32_t*)(B + mDumpOff[6]);
    {
        Sec rec{"records", "u4", "[" + std::to_string(recWords) + "]", {}};
        rec.data.assign((const uint8_t*)R, (const uint8_t*)R + recWords * 4u);
        secs.push_back(std::move(rec));
    }
    // The land under each texel's column (CPU; the floor the depth knows nothing of is the terrain's
    // or a prim's -- snowdump.py compares the snow's base with it). NaN: no base / no land.
    {
        Sec ter{"terrain", "f4", "[2, " + std::to_string(W) + ", " + std::to_string(W) + "]", {}};
        ter.data.resize(2ull * W * W * 4u);
        float* T = (float*)ter.data.data();
        LLWorld* world = LLWorld::getInstance();
        for (uint32_t L = 0; L < 2; ++L)
        {
            const uint32_t res = L == 0 ? resI : resR;
            float x0, y0, inv, top, sx, sy;
            if (L == 0) { x0 = mDumpPush[0]; y0 = mDumpPush[1]; inv = mDumpPush[2]; top = mDumpPush[4]; sx = mDumpPush[5]; sy = mDumpPush[6]; }
            else { x0 = mDumpPush[24]; y0 = mDumpPush[25]; inv = std::fabs(mDumpPush[26]); top = mDumpPush[27]; sx = mDumpPush[28]; sy = mDumpPush[29]; }
            for (uint32_t j = 0; j < W; ++j)
                for (uint32_t i = 0; i < W; ++i)
                {
                    float& out = T[(L * W + j) * W + i];
                    out = std::numeric_limits<float>::quiet_NaN();
                    const uint32_t* r = R + kDumpHeadWords + ((uint64_t)(L * W + j) * W + i) * kDumpRecWords;
                    if (!res || inv <= 0.f || !world || (r[0] & 0x80000001u) != 0x80000001u) continue;
                    float h;
                    std::memcpy(&h, &r[1], 4);
                    const float tx = (float)(mDumpOrg[2 * L] + (int32_t)i) + 0.5f, ty = (float)(mDumpOrg[2 * L + 1] + (int32_t)j) + 0.5f;
                    const float px = x0 + tx / ((float)res * inv) + (top - h) * sx, py = y0 + ty / ((float)res * inv) + (top - h) * sy;
                    const F32 land = world->resolveLandHeightAgent(LLVector3(px, py, h));
                    if (std::isfinite(land)) out = land;
                }
        }
        secs.push_back(std::move(ter));
    }
    // The JSON head.
    std::ostringstream js;
    js.precision(9);
    auto fl = [](float v) -> std::string { if (!std::isfinite(v)) return "null"; std::ostringstream s; s.precision(9); s << v; return s.str(); };
    uint32_t pu[32];
    std::memcpy(pu, mDumpPush, sizeof(pu));
    js << "{\"format\": \"slipstream-snowdump\", \"version\": 1, \"issue\": 783"
       << ", \"window\": " << W << ", \"record_words\": " << kDumpRecWords << ", \"head_words\": " << kDumpHeadWords
       << ", \"inner\": {\"res\": " << resI << ", \"origin\": [" << mDumpOrg[0] << ", " << mDumpOrg[1] << "]"
       << ", \"x0\": " << fl(mDumpPush[0]) << ", \"y0\": " << fl(mDumpPush[1]) << ", \"extent\": " << fl(1.f / std::max(mDumpPush[2], 1e-9f))
       << ", \"top\": " << fl(mDumpPush[4]) << ", \"shear\": [" << fl(mDumpPush[5]) << ", " << fl(mDumpPush[6]) << "]"
       << ", \"vbase\": 0, \"nv\": " << nvI << "}"
       << ", \"ring\": {\"on\": " << (mDumpPush[26] != 0.f ? "true" : "false") << ", \"res\": " << resR
       << ", \"origin\": [" << mDumpOrg[2] << ", " << mDumpOrg[3] << "]"
       << ", \"x0\": " << fl(mDumpPush[24]) << ", \"y0\": " << fl(mDumpPush[25])
       << ", \"extent\": " << fl(mDumpPush[26] != 0.f ? 1.f / std::fabs(mDumpPush[26]) : 0.f)
       << ", \"top\": " << fl(mDumpPush[27]) << ", \"shear\": [" << fl(mDumpPush[28]) << ", " << fl(mDumpPush[29]) << "]"
       << ", \"vbase\": " << 3u * nvI << ", \"nv\": " << nvR << "}"
       << ", \"thr\": " << fl(mDumpPush[16]) << ", \"tan_repose\": " << fl(mDumpPush[17])
       << ", \"repose_deg\": " << fl(gSavedSettings.getF32("RenderVKWeatherSnowReposeAngle"))
       << ", \"lod1\": " << fl(mDumpPush[18]) << ", \"lod2\": " << fl(mDumpPush[19])
       << ", \"u0\": [" << pu[20] << ", " << pu[21] << ", " << pu[22] << ", " << pu[23] << "]"
       << ", \"edge_lod\": " << ((pu[20] >> 16) & 3u) << ", \"toe_reach_opt\": " << fl((float)(pu[22] & 0xFFFFu) * 0.01f)
       << ", \"smooth\": " << (((pu[22] >> 16) & 1u) ? "true" : "false")
       << ", \"lift\": " << fl(mDumpPush[23])
       << ", \"cam\": [" << fl(mDumpCam[0]) << ", " << fl(mDumpCam[1]) << ", " << fl(mDumpCam[2]) << "]"
       << ", \"focus\": [" << fl(mDumpFocus[0]) << ", " << fl(mDumpFocus[1]) << ", " << fl(mDumpFocus[2]) << "]"
       << ", \"focus_is_focus\": " << (mDumpFocusOk ? "true" : "false")
       << ", \"global_depth\": " << fl(mDumpGlobalDepth) << ", \"water\": " << fl(mDumpWater)
       << ", \"small_vram\": " << (sSmallVram ? "true" : "false")
       << ", \"mesh_age_before\": " << mDumpMeshAge
       << ", \"mesh_interval\": " << gSavedSettings.getU32("RenderVKWeatherSnowMeshInterval")
       << ", \"mesh_patch\": " << (gSavedSettings.getBOOL("RenderVKWeatherSnowMeshPatch") ? "true" : "false")
       << ", \"max_tris\": " << mMeshTris
       << ", \"pre_args\": [";
    for (int i = 0; i < 8; ++i) js << (i ? ", " : "") << args[0][i];
    js << "], \"post_args\": [";
    for (int i = 0; i < 8; ++i) js << (i ? ", " : "") << args[1][i];
    js << "], \"tris_in_buffer\": [" << allTris[0] << ", " << allTris[1] << "]";
    js << ", \"sections\": [";
    uint64_t off = 0;
    for (size_t i = 0; i < secs.size(); ++i)
    {
        js << (i ? ", " : "") << "{\"name\": \"" << secs[i].name << "\", \"dtype\": \"" << secs[i].dtype << "\", \"shape\": "
           << secs[i].shape << ", \"offset\": " << off << ", \"bytes\": " << secs[i].data.size() << "}";
        off += secs[i].data.size();
    }
    js << "]}";
    const std::string head = js.str();
    // logs/snowdump-<local time>.bin: "SNOWDUMP", u32 version, u32 JSON bytes, the JSON, the sections
    char stamp[32] = "0";
    {
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#if LL_WINDOWS
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    }
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, std::string("snowdump-") + stamp + ".bin");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
    {
        const uint32_t ver = 1u, hl = (uint32_t)head.size();
        out.write("SNOWDUMP", 8);
        out.write((const char*)&ver, 4);
        out.write((const char*)&hl, 4);
        out.write(head.data(), head.size());
        for (const Sec& s : secs) out.write((const char*)s.data.data(), (std::streamsize)s.data.size());
    }
    if (out.good())
        LL_INFOS("VKWeather") << "snow dump written: " << path << " (" << W << "^2 texels, " << keptTris[1] << " triangles after the build, "
                              << keptTris[0] << " before; python3 scripts/dev/snowdump.py <file>)" << LL_ENDL;
    else
        LL_WARNS("VKWeather") << "snow dump: could not write " << path << LL_ENDL;
    // The buffers go (binding 7 back to the placeholder: a device wait, the prestep's rule).
    wxWaitIdle(mCtx->device(), "snow dump freed");
    freeDump(true);
    writeSnowSets();
}

uint32_t LLCleanWeatherVK::meshShadowCascades() const
{
    static LLCachedControl<U32> s_n(gSavedSettings, "RenderVKWeatherSnowMeshShadowCascades", 4);
    const uint32_t n = std::min<uint32_t>((U32)s_n, 8u);
    return sSmallVram ? std::min(n, 1u) : n;
}

void LLCleanWeatherVK::recordMeshDrawFwd(VkCommandBuffer cmd, bool rt) const
{
    if (!meshFwdReady(rt)) return;
    bindGraphicsPipeline(cmd, mMeshFwdPipe[rt ? 1 : 0]);
    const VkDeviceSize off = 0;
    if (mMeshBuilt)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &mMeshVerts.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mMeshIdx.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, mMeshArgs.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    }
    if (mShMeshBuilt && mShMeshV.buffer)   // #901: the avatars' snow (same pipelines, its own buffers)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &mShMeshV.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mShMeshI.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, mShMeshA.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void LLCleanWeatherVK::recordMeshDrawPre(VkCommandBuffer cmd, uint32_t variant) const
{
    // The ground's mesh only. NOT the #901 avatar snow mesh: it rides a moving avatar (rebuilt
    // from the skinned surface each recording) and this pass writes camera-only motion -- the
    // pre-pass keeps the avatar under it (its true motion, a few centimetres of parallax).
    if (!meshPreReady(variant) || !mMeshBuilt) return;
    bindGraphicsPipeline(cmd, mMeshPrePipe[variant]);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mMeshVerts.buffer, &off);
    vkCmdBindIndexBuffer(cmd, mMeshIdx.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(cmd, mMeshArgs.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
}

void LLCleanWeatherVK::recordMeshDraw(VkCommandBuffer cmd, bool shadow) const
{
    if (!meshReady(shadow)) return;
    bindGraphicsPipeline(cmd, shadow ? mMeshShadowPipe : mMeshGbufPipe);
    const VkDeviceSize off = 0;
    if (mMeshBuilt)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &mMeshVerts.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mMeshIdx.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, mMeshArgs.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    }
    if (mShMeshBuilt && mShMeshV.buffer)   // #901: the avatars' snow (same pipelines, its own buffers)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &mShMeshV.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mShMeshI.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, mShMeshA.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void LLCleanWeatherVK::destroy(VkDevice dev)
{
    if (!dev) return;
    mJob.wait();
    for (VkPipeline& p : mStage)
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    wxWaitIdle(dev, "weather freed");
    const bool hadMap = mMap[OCCL_COARSE].img[0].image || mMap[OCCL_NEAR].img[0].image || mCover[0].image
                     || mInstSnow.image || mFlowImg.image;
    destroySnow();
    destroyRunoff();
    destroyShelter();   // #901
    destroyAccum();     // #901 rest-pose accumulation (the own grid's snapshot lives on, static)
    destroyShelterPipelines(dev);
    destroyBuffers();
    destroyNear();
    for (VkPipeline* p : {&mRunoffPipe, &mSpawnPipe})
        if (*p) { vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (mStreamPipe) { vkForgetPipeline(mStreamPipe); vkDestroyPipeline(dev, mStreamPipe, nullptr); mStreamPipe = VK_NULL_HANDLE; }
    if (mRunoffLayout) vkDestroyPipelineLayout(dev, mRunoffLayout, nullptr);
    mRunoffLayout = VK_NULL_HANDLE;
    mRunoffPool.destroy();
    mRunoffSet = VK_NULL_HANDLE;
    if (mRunoffDsl) vkdDestroySetLayout(dev, mRunoffDsl);
    mRunoffDsl = VK_NULL_HANDLE;
    mRunoffRetryAt = 0.0;
    mNearGen = 0;
    for (VkPipeline* p : {&mCoverPipe, &mInstPipe, &mAvPipe, &mStampPipe, &mBermPipe, &mMeshBuildPipe, &mRingPipe})
        if (*p) { vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipeline* p : {&mMeshGbufPipe, &mMeshShadowPipe, &mMeshFwdPipe[0], &mMeshFwdPipe[1],
                          &mMeshPrePipe[0], &mMeshPrePipe[1], &mMeshPrePipe[2], &mMeshPrePipe[3]})
        if (*p) { vkForgetPipeline(*p); vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (mMeshLayout) vkDestroyPipelineLayout(dev, mMeshLayout, nullptr);
    mMeshLayout = VK_NULL_HANDLE;
    mMeshPool.destroy();
    mMeshSet = VK_NULL_HANDLE;
    if (mMeshDsl) vkdDestroySetLayout(dev, mMeshDsl);
    mMeshDsl = VK_NULL_HANDLE;
    mMeshRetryAt = 0.0;
    if (mSnowLayout) vkDestroyPipelineLayout(dev, mSnowLayout, nullptr);
    mSnowLayout = VK_NULL_HANDLE;
    mSnowPool.destroy();
    mSnowSet = VK_NULL_HANDLE;
    if (mSnowDsl) vkdDestroySetLayout(dev, mSnowDsl);
    mSnowDsl = VK_NULL_HANDLE;
    mSnowRetryAt = 0.0;
    mBindingDirty = mBindingDirty || hadMap;
    if (mSimPipe) vkDestroyPipeline(dev, mSimPipe, nullptr);
    if (mDrawPipe) { vkForgetPipeline(mDrawPipe); vkDestroyPipeline(dev, mDrawPipe, nullptr); }
    if (mSplashPipe) { vkForgetPipeline(mSplashPipe); vkDestroyPipeline(dev, mSplashPipe, nullptr); }
    mSimPipe = mDrawPipe = mSplashPipe = VK_NULL_HANDLE;
    if (mVeilPipe) { vkForgetPipeline(mVeilPipe); vkDestroyPipeline(dev, mVeilPipe, nullptr); mVeilPipe = VK_NULL_HANDLE; }
    if (mVeilLayout) vkDestroyPipelineLayout(dev, mVeilLayout, nullptr);
    mVeilLayout = VK_NULL_HANDLE;
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mLayout = VK_NULL_HANDLE;
    mPool.destroy();
    mDset[0] = mDset[1] = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mSampler = VK_NULL_HANDLE;
    for (VkFormat& fm : mPipeFormats) fm = VK_FORMAT_UNDEFINED;
    mPipeFailed = false;
    mRunning = mPrecip = mSimRecorded = false;
    mReset = true;
    mLastTime = -1.0;
    for (OcclMap& m : mMap) m.age = 0;
    mOcclTurn = 0;
    mNearRetryAt = 0.0;
    mTilt[0] = mTilt[1] = 0.f;
}
