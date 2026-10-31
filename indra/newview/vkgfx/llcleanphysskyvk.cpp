/**
 * @file llcleanphysskyvk.cpp
 * @brief Physical sky (Hillaire 2020) for the Vulkan clean renderer.
 *
 * Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (vkgfx/gmdxphysskyvk.cpp).
 * Copyright (C) 2026, squeedledorf (GunMetal-DX); adapted for Slipstream.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// Physical air uses the planet model's fixed Mie density/scale height and bounded
// blue-density Rayleigh variation. EEP haze density, density/distance multiplier
// and max_y own one independent artistic height profile in the camera medium.
// Physical tables are shared; finite L/RGB-T grids, origins and history are per eye.
// Sky radiance replaces only its finite air prefix before highlight rolloff.
// Physical-off and coherent resource-failure frames retain the legacy path.

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llviewercontrol.h"
#include "llhazeenv.h"
#include "llhazesky.h"
#include "llenvironment.h"
#include <atomic>
#include "llframetimer.h"
#include "slnightlight.h"   // physical night: the moon as a second light
#include "slspacealt.h"     // space at altitude
#include "gmdxskytune.h"    // <GMDX> the sky's air and aerial perspective (the Sky Debugger's knobs)

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
std::atomic<bool> sHazePhysicalActive{false};
template <typename Sky>
auto hazeTransition(const Sky& sky, int) -> decltype(sky.getHazeTransition())
{
    return sky.getHazeTransition();
}
template <typename Sky>
const LLHaze::Transition* hazeTransition(const Sky&, long) { return nullptr; }
}

LLHaze::Pair llResolveHaze(const LLSettingsSky& sky, double startPreference, double opacityPreference)
{
    const auto& night=slNightLight();
    LLHaze::illuminationPolicy().physicalNight = night.on;
    double moonGain=1.;
    if(night.on)
    {
        const auto current=LLEnvironment::instance().getCurrentSky();
        if(current)
        {
            const double elevation=std::asin(LLHaze::bounded(current->getSunDirection().mV[2],1.,-1.,1.))*180./3.141592653589793;
            const double intrinsic=night.moon_full*(1.-LLHaze::smooth(-10.,4.,elevation));
            if(intrinsic>1.e-8) moonGain=LLHaze::bounded(night.moon/intrinsic,1.,0.,64.);
        }
    }
    LLHaze::illuminationPolicy().moonGain=moonGain;
    const auto* transition = hazeTransition(sky, 0);
    LLHaze::Pair pair;
    if(transition)
    {
        auto current=*transition;
        llHazeRefreshTransition(current);
        pair=LLHaze::resolveTransition(current);
    }
    else pair=LLHaze::resolveEndpoint(llHazeEndpoint(sky));
    return LLHaze::overrideLocal(pair, startPreference, opacityPreference);
}
LLHazeDisplay llGetHazeDisplay(const LLSettingsSky& sky)
{
    const double start = gSavedSettings.controlExists("RenderVKHazeStartDistance") ? gSavedSettings.getF32("RenderVKHazeStartDistance") : -1.;
    const double cap = gSavedSettings.controlExists("RenderVKHazeMaxOpacity") ? gSavedSettings.getF32("RenderVKHazeMaxOpacity") : -1.;
    const auto pair = llResolveHaze(sky, start, cap);
    return {static_cast<float>(pair.startDistance.value), static_cast<float>(pair.maxOpacity.value),
            pair.startDistance.source, pair.maxOpacity.source, sHazePhysicalActive.load(std::memory_order_relaxed)};
}
LLHazeDisplay llGetHazeDisplay()
{
    const auto sky = LLEnvironment::instance().getCurrentSky();
    return sky ? llGetHazeDisplay(*sky) : LLHazeDisplay{};
}

namespace
{
constexpr uint32_t kTransW = 256, kTransH = 64, kMsRes = 32, kViewW = 192, kViewH = 108;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Earth (Hillaire's defaults), per km.
constexpr float kEarthRayleigh[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr float kEarthMieScat = 3.996e-3f, kEarthMieExt = 4.440e-3f, kMieG = 0.8f;
constexpr float kBottomKm = 6360.f, kTopKm = 6460.f, kRayleighH = 8.f, kMieH = 1.2f, kGroundAlbedo = 0.3f;
// EEP's legacy haze defaults (LLSettingsSky).
constexpr float kDefBlueDensity[3] = {0.2447f, 0.4487f, 0.7599f};

constexpr float kDefBlueHorizon[3] = {0.4954f, 0.4954f, 0.6399f}, kDefHazeHorizon = 0.19f;
// Look constants (GunMetal-DX's setting defaults: RenderVKPhysicalSkyExposure / Ambient /
// Highlight / Aerial; minimal config space keeps them constants).
constexpr float kExposure = 8.f;    // the light's illuminance per unit of EEP colour
constexpr float kAmbient = 0.25f;   // EEP ambient scattered toward the viewer (<GMDX> Orion's; Slipstream 0.3)
constexpr float kHighlight = 1.5f;  // the sky's luminance roll-off knee (0 = none)

VkShaderModule pkModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

float srgbToLinear(float cs)
{
    cs=static_cast<float>(LLHaze::bounded(cs,0.,0.,64.));
    return cs <= 0.04045f ? cs / 12.92f : std::pow((std::max(cs, 0.0f) + 0.055f) / 1.055f, 2.4f);
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = ss; m.srcAccessMask = sa; m.dstStageMask = ds; m.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

void LLCleanRendererVK::physSkyFill(VKSSRInfo& si)
{
    static LLCachedControl<bool> on_setting(gSavedSettings, "RenderVKPhysicalSky", true);

    std::memset(si.physky, 0, sizeof(si.physky));
    // SPACE AT ALTITUDE (slspacealt.h): this frame's amount, for every sky (frame info night[1].w).
    // LL's random stars (physical night off) show by day up there.
    const float space = slSpaceUpdate(si.cam_origin[2]);
    si.night[1][3] = space;
    if (space > 0.f) si.bodies[7][0] = std::max(si.bodies[7][0], 0.5f * space);
    const float* a = si.atmos;
    auto v = [&](int row, int c) { return a[row * 4 + c]; };
    // (no sky this frame: WindLight's own off path)
    mPkWanted = on_setting && v(6, 2) > 0.5f && mPkTrans.image && mPkMs.image && mPkView.image && !mPkPipeFailed;
    // The pipelines are built by physSkyRecord once wanted; until then the WindLight sky.
    if (!mPkWanted || !mPkPipe[0] || !mPkPipe[1] || !mPkPipe[2] || (!mCameraMediumReady && gmdxSkyAerial() <= 0.f))   // <GMDX> (own aerial perspective: no medium)
    {
        if(sHazePhysicalActive.exchange(false,std::memory_order_relaxed))
            LL_DEBUGS("HazeResolve") << "physical=0 coherent legacy fallback" << LL_ENDL;
        mCameraMediumReady=false; mVolFogNow=false;
        si.vol_eye[3]=0.f;
        std::memset(mPkPush, 0, sizeof(mPkPush));
        return;
    }
    // <GMDX> GUNMETAL-DX'S AIR: EEP's density_multiplier thickens the whole air (by the square root of
    // its ratio to the default 1e-4), haze_density the Mie haze (default 0.7 = Earth's) and max_y sets
    // the haze's scale height (default 1605 m = Earth's 1.2 km), as before Slipstream's port fixed
    // them: a sunset's dense, low haze reddens the light and turns the sky gold around the sun (and
    // the clouds it lights orange). kGmdxEepAir (the Sky Debugger's knob) blends: 0 = Slipstream's
    // fixed planet air, 1 = GunMetal-DX's.
    constexpr float kGmdxEepAir = 1.f;
    const double eep = LLHaze::bounded(gmdxSkyTune().sky_eep_air(kGmdxEepAir), 1., 0., 1.);
    const float dens = static_cast<float>(std::pow(LLHaze::bounded(std::sqrt(std::max(v(4, 3), 1e-7f) / 1e-4), 1., 0.25, 4.), eep));
    const float mie = static_cast<float>(std::pow(LLHaze::bounded(v(1, 3) / 0.7, 1., 0., 8.), eep)) * dens;
    const float mie_h = kMieH * static_cast<float>(std::pow(LLHaze::bounded(v(3, 3) / 1605., 1., 0.2 / kMieH, 6. / kMieH), eep));
    // </GMDX>
    for (int c = 0; c < 3; ++c)
    {
        si.physky[0][c] = kEarthRayleigh[c] * static_cast<float>(LLHaze::bounded(v(1, c) / kDefBlueDensity[c], 1., 0., 6.)) * dens;   // <GMDX> x dens
        si.physky[1][c] = kEarthMieScat * mie;   // <GMDX> x mie
        si.physky[2][c] = kEarthMieExt * mie;    // <GMDX> x mie
    }
    si.physky[0][3] = kBottomKm;
    si.physky[1][3] = kTopKm;
    si.physky[2][3] = kMieG;

    // The light: the sun's or moon's EEP colour (the transmittance LUT reddens it further low down).
    // Physical night: the sun's own (atmos[8] / [10]), the moon a second light (see the header).
    const SLNightLight& night = slNightLight();
    const int lrow = night.on ? 8 : 2, drow = night.on ? 10 : 7;
    for (int c = 0; c < 3; ++c) si.physky[3][c] = srgbToLinear(std::max(v(lrow, c), 0.f)) * kExposure;
    // Viewer height (km), in 10 m steps: the sky-view table is rebuilt when it moves a step.
    // Space at altitude: the sky's viewer rises into the thin air (clouds keep the real height).
    float view_km = std::clamp(si.cam_origin[2] * 0.001f, 0.f, 60.f);
    if (space > 0.f) view_km = std::min(view_km + space * kSpaceKm, 60.f);
    si.physky[3][3] = std::round(view_km * 100.f) * 0.01f;

    float lx = v(drow, 0), ly = v(drow, 1), lz = v(drow, 2);
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    if (ll > 1e-6f) { lx /= ll; ly /= ll; lz /= ll; } else { lx = 0.f; ly = 0.f; lz = 1.f; }
    si.physky[4][0] = lx; si.physky[4][1] = ly; si.physky[4][2] = lz;
    si.physky[4][3] = 1.f;
    // The moon's half of the sky-view table: while its light counts (well below the horizon its
    // sky is the planet's shadow; a new or faint moon is nothing next to the stars).
    if (night.on && night.moon > 1e-6f)
    {
        const float mz = v(11, 2) / std::max(std::sqrt(v(11, 0) * v(11, 0) + v(11, 1) * v(11, 1) + v(11, 2) * v(11, 2)), 1e-6f);
        if (mz > -0.2f) si.physky[4][3] = 2.f;
    }

    // Physical night: EEP's ambient is no light in the sky, and a night sky's is set for Second Life's
    // full-strength moon; scattered as it is, it made up most of the night sky away from the moon (a
    // flat glow the moonlit clouds could not stand out from). It follows the WindLight sky's own light:
    // the sun's while it is up (as before), the moon's strength at night.
    const float amb_k = night.on ? std::clamp(std::max(night.sky_q, night.moon), 0.f, 1.f) : 1.f;
    for (int c = 0; c < 3; ++c) si.physky[5][c] = srgbToLinear(std::max(v(3, c), 0.f)) * kAmbient * amb_k;
    si.physky[5][3] = -1.f / kRayleighH;
    si.physky[6][0] = -1.f / mie_h;
    si.physky[6][1] = kGroundAlbedo;
    si.physky[6][2] = kHighlight;
    si.physky[6][3] = 1.f; // reserved; scene integration uses metres, without distance stretching
    // <GMDX> GunMetal-DX's aerial perspective (gmdxskytune.h gmdxSkyAerial):
    // physky[6].w its stretch (0 = off), no camera medium (cmOn() false: every pass takes the
    // WindLight haze, clean_atmos_common.glsl atmosScatterHaze colours it by this sky). The haze
    // controls (RenderVKHazeStartDistance / MaxOpacity, a sky's own) as the start (m) and optical
    // depth cap (< 0: none):
    // - haze x / y: the weather layer's mist / weather fog (clean_vol_common.glsl gmdxHazeDepth), the
    //   resolved pair, automatic included, as the camera medium's height haze takes it;
    // - haze z / w: GunMetal-DX's WindLight haze, only a pair the user or the sky set. Automatic is
    //   GunMetal-DX's look there: neither (the derived pair sizes a ground haze a few metres deep;
    //   on the whole haze it would thin the sunset). A sky's day cycle / transition blends
    //   (Source::Blend) even two derived values: a blend counts as set only when one of its ends is
    //   the sky's own (an end frozen from an earlier blend reports Blend: automatic).
    const float gmdxAerial = gmdxSkyAerial();
    si.physky[6][3] = gmdxAerial;
    if (gmdxAerial > 0.f)
    {
        static LLCachedControl<F32> s_hazeStart(gSavedSettings, "RenderVKHazeStartDistance", -1.f);
        static LLCachedControl<F32> s_hazeCap(gSavedSettings, "RenderVKHazeMaxOpacity", -1.f);
        si.haze_mode[0] = 0.f;
        si.haze[0] = si.haze[2] = 0.f;
        si.haze[1] = si.haze[3] = -1.f;
        if (const auto current = LLEnvironment::instance().getCurrentSky())
        {
            using Src = LLHaze::Source;
            const LLHaze::Pair haze = llResolveHaze(*current, s_hazeStart, s_hazeCap);
            const auto* blend = hazeTransition(*current, 0);
            const LLHaze::Pair from = blend ? LLHaze::resolveEndpoint(blend->from) : haze;
            const LLHaze::Pair to = blend ? LLHaze::resolveEndpoint(blend->to) : haze;
            si.haze[0] = static_cast<float>(haze.startDistance.value);
            si.haze[1] = static_cast<float>(LLHaze::opacityOpticalDepth(haze.maxOpacity.value));
            const auto set = [](Src s, Src a, Src b) { return s == Src::User || s == Src::Sky || (s == Src::Blend && (a == Src::Sky || b == Src::Sky)); };
            if (set(haze.startDistance.source, from.startDistance.source, to.startDistance.source))
                si.haze[2] = static_cast<float>(haze.startDistance.value);
            if (set(haze.maxOpacity.source, from.maxOpacity.source, to.maxOpacity.source))
                si.haze[3] = static_cast<float>(LLHaze::opacityOpticalDepth(haze.maxOpacity.value));
        }
        sHazePhysicalActive.store(true, std::memory_order_relaxed);
    }
    // </GMDX>
    // The artist's tints (capped: past ~2 the multiple-scattering series stops converging).
    for (int c = 0; c < 3; ++c)
        si.physky[7][c] = static_cast<float>(LLHaze::bounded(v(0, c)/kDefBlueHorizon[c],1.,0.,2.));
    si.physky[7][3] = static_cast<float>(LLHaze::bounded(std::sqrt(std::max(v(0, 3),0.f)/kDefHazeHorizon),1.,0.,2.));

    const bool becameActive=gmdxAerial<=0.f && !sHazePhysicalActive.exchange(true,std::memory_order_relaxed);   // <GMDX> (not with our aerial)
    const auto sky = gmdxAerial<=0.f ? LLEnvironment::instance().getCurrentSky() : nullptr;   // <GMDX> (ditto)
    if (sky)
    {
        const auto resolved = llGetHazeDisplay(*sky);
        si.haze[0] = si.medium[0].fog[0];
        si.haze[1] = 1.f / std::max(si.medium[0].fog[1],1.e-6f);
        si.haze[2] = resolved.startDistance;
        si.haze[3] = static_cast<float>(LLHaze::opacityOpticalDepth(resolved.maxOpacity));
        si.haze_mode[0] = 1.f;
        const bool volumetric=gSavedSettings.getU32("RenderVKVolumetricFog")!=0;
        si.haze_mode[1] = volumetric ? std::clamp(gSavedSettings.getF32("RenderVKVolumetricFogRange"),32.f,512.f) : 0.f;
        si.haze_mode[2] = si.water[15];
        si.haze_mode[3] = std::clamp(mLightInfoCPU.zinfo[1], 1.f, 512.f);
        const auto derived=LLHaze::derive(llHazeEndpoint(*sky).inputs);
        static float previous[4]={-1.f,-1.f,-1.f,-1.f};
        static LLHaze::Source startSource=LLHaze::Source::Derived,capSource=LLHaze::Source::Derived;
        static double loggedAt=-10.;
        static float previousResolved[2]={-1.f,-1.f};
        const double now=LLFrameTimer::getElapsedSeconds();
        const bool discrete=becameActive || startSource!=resolved.startSource || capSource!=resolved.opacitySource ||
            (resolved.startSource==LLHaze::Source::User && previousResolved[0]!=resolved.startDistance) ||
            (resolved.opacitySource==LLHaze::Source::User && previousResolved[1]!=resolved.maxOpacity);
        bool changed=discrete;
        for(int c=0;c<4;++c) changed=changed || previous[c]!=si.haze[c];
        if(changed && (now-loggedAt>=1. || discrete))
        {
            const auto& input=derived.sanitized;
            LL_DEBUGS("HazeResolve") << "h=" << input.hazeDensity << " d=" << input.densityMultiplier
                << " r=" << input.distanceMultiplier << " y=" << input.maxY
                << " sigma=" << si.haze[0] << " H=" << si.haze[1] << " b=" << derived.brightness
                << " autoStart=" << derived.automatic.startDistance.value << " autoCap=" << derived.automatic.maxOpacity.value
                << " start=" << resolved.startDistance << " source=" << LLHaze::sourceName(resolved.startSource)
                << " cap=" << resolved.maxOpacity << " source=" << LLHaze::sourceName(resolved.opacitySource) << LL_ENDL;
            std::memcpy(previous,si.haze,sizeof(previous));
            startSource=resolved.startSource;capSource=resolved.opacitySource;loggedAt=now;
            previousResolved[0]=resolved.startDistance;previousResolved[1]=resolved.maxOpacity;
        }
    }
    std::memcpy(mPkPush, si.physky, sizeof(mPkPush));
}

// Init (after draw set 0 exists): the tables, their sampler, the one-time layout change to
// GENERAL and draw set 0 bindings 56 (sky view) / 57 (transmittance). A refused allocation (#697)
// leaves the WindLight sky (physSkyFill never turns the mode on; the bindings stay unwritten,
// PARTIALLY_BOUND and never read).
void LLCleanRendererVK::physSkyInit()
{
    if (!mCtx || !mDsetDraw0 || mPkSampler) return;
    VkDevice dev = mCtx->device();
    {
        VkMemScope mem(VKMEM_TARGET);
        const VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        mPkTrans = mCtx->createImage2D(kTransW, kTransH, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);
        mPkMs = mCtx->createImage2D(kMsRes, kMsRes, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);
        mPkView = mCtx->createImage2D(kViewW, kViewH * 2, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);   // sun / moon halves
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mPkSampler));
    if (!mPkTrans.image || !mPkMs.image || !mPkView.image || !mPkSampler)
    {
        LL_WARNS("Vulkan") << "VKPhysSky: the lookup tables were refused (VRAM): the WindLight sky stays" << LL_ENDL;
        return;
    }
    const VkImage imgs[3] = {mPkTrans.image, mPkMs.image, mPkView.image};
    mCtx->submitImmediate([imgs](VkCommandBuffer c)
    {
        VkImageMemoryBarrier2 b[3]{};
        for (int i = 0; i < 3; ++i)
        {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image = imgs[i]; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 3; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(c, &d);
    });
    mPkMediumValid = mPkViewValid = false;
    VkDescriptorImageInfo ii[2]{};
    ii[0] = {mPkSampler, mPkView.view, VK_IMAGE_LAYOUT_GENERAL};
    ii[1] = {mPkSampler, mPkTrans.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = mDsetDraw0; w[i].dstBinding = 56 + i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    VkDescriptorImageInfo ms{mPkSampler, mPkMs.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet mw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    mw.dstSet = mDsetDraw0; mw.dstBinding = 63; mw.descriptorCount = 1;
    mw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; mw.pImageInfo = &ms;
    vkUpdateDescriptorSets(dev, 1, &mw, 0, nullptr);
}

// Right after this recording's frame-info upload (beside recordSkyLut), before any reader.
void LLCleanRendererVK::physSkyRecord(VkCommandBuffer cmd)
{
    if (!mPkWanted) return;
    VkDevice dev = mCtx->device();

    // Built on the first frame that wants the mode (synchronous, three small computes); the
    // frame info turns the mode on from the next frame.
    if (!mPkPipe[0] && !mPkPipeFailed)
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mPkDsl, "mDslPhysSky"));
        VK_CHECK(mPkPool.create(dev, "physsky", {{mPkDsl, 3}}));
        for (int i = 0; i < 3; ++i)
            VKD_ALLOC_SOFT(mPkPool.allocate(mPkDsl, &mPkSet[i]), { mPkPipeFailed = true; return; });

        // Set i writes its table (0 trans, 1 MS, 2 view) and reads the transmittance + MS ones.
        const VkImageView outs[3] = {mPkTrans.view, mPkMs.view, mPkView.view};
        for (int i = 0; i < 3; ++i)
        {
            VkDescriptorImageInfo oi{VK_NULL_HANDLE, outs[i], VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo ti{mPkSampler, mPkTrans.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo mi{mPkSampler, mPkMs.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[3]{};
            for (uint32_t j = 0; j < 3; ++j)
            {
                w[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[j].dstSet = mPkSet[i]; w[j].dstBinding = j; w[j].descriptorCount = 1;
                w[j].descriptorType = j == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            w[0].pImageInfo = &oi; w[1].pImageInfo = &ti; w[2].pImageInfo = &mi;
            vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
        }

        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mPkPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mPkDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mPkLayout, "mPkLayout"));
        const uint32_t* code[3] = {kWorldPhysSkyTransSpv, kWorldPhysSkyMsSpv, kWorldPhysSkyViewSpv};
        const size_t bytes[3] = {kWorldPhysSkyTransSpv_bytes, kWorldPhysSkyMsSpv_bytes, kWorldPhysSkyViewSpv_bytes};
        const char* names[3] = {"kWorldPhysSkyTransSpv", "kWorldPhysSkyMsSpv", "kWorldPhysSkyViewSpv"};
        for (int i = 0; i < 3; ++i)
        {
            VkShaderModule m = pkModule(dev, code[i], bytes[i], names[i]);
            mPkPipe[i] = m ? createComputePipeline(dev, m, mPkLayout) : VK_NULL_HANDLE;
            if (m) vkDestroyShaderModule(dev, m, nullptr);
            if (!mPkPipe[i]) mPkPipeFailed = true;
        }
        if (mPkPipeFailed)
        {
            LL_WARNS("Vulkan") << "VKPhysSky: pipelines failed to build; the WindLight sky stays" << LL_ENDL;
            return;
        }
        LL_INFOS("Vulkan") << "VKPhysSky: physical sky pipelines built (RenderVKPhysicalSky)" << LL_ENDL;
    }
    if (mPkPipeFailed || mPkPush[4][3] < 0.5f) return;   // (built this frame: on from the next)

    // What each table was last built for: the medium (transmittance + MS) and everything the
    // sky view reads on top (the push block whole, and the moon's direction while it is a light).
    float key[16];
    for (int c = 0; c < 4; ++c)
    {
        key[c] = mPkPush[0][c]; key[4 + c] = mPkPush[1][c]; key[8 + c] = mPkPush[2][c]; key[12 + c] = mPkPush[7][c];
    }
    key[3] += mPkPush[5][3] * 1000.f + mPkPush[6][0] * 7.f + mPkPush[6][1] * 13.f;   // (scale heights, albedo)
    const bool medium = !mPkMediumValid || std::memcmp(key, mPkMediumKey, sizeof(key)) != 0;
    float view_key[36];
    std::memcpy(view_key, mPkPush, sizeof(mPkPush));
    for (int c = 0; c < 4; ++c) view_key[32 + c] = mPkPush[4][3] > 1.5f ? mAtmos[11 * 4 + c] : 0.f;
    const bool view = medium || !mPkViewValid || std::memcmp(view_key, mPkViewKey, sizeof(view_key)) != 0;
    if (!view) return;   // nothing changed: last build's tables stand

    // Last frames' readers (sky / lighting FS, computes) before this frame's writes.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdPushConstants(cmd, mPkLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mPkPush), mPkPush);

    // The medium: Rayleigh, Mie (scattering, extinction, g), scale heights, radii.
    if (medium)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[0]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[0], 0, nullptr);
        vkCmdDispatch(cmd, kTransW / 8, kTransH / 8, 1);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[1]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[1], 0, nullptr);
        vkCmdDispatch(cmd, kMsRes, kMsRes, 1);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        std::memcpy(mPkMediumKey, key, sizeof(key));
        mPkMediumValid = true;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[2]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[2], 0, nullptr);
    vkCmdDispatch(cmd, (kViewW + 7) / 8, (kViewH + 7) / 8, 1);
    // Physical night: the moon's half (rows kViewH..), the same light from the moon's
    // direction (atmos[11]); [4].w = 3 tells the pass which half. No barrier between: disjoint rows.
    if (mPkPush[4][3] > 1.5f)
    {
        float moonPush[8][4];
        std::memcpy(moonPush, mPkPush, sizeof(moonPush));
        const float* m = &mAtmos[11 * 4];
        const float ml = std::max(std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), 1e-6f);
        for (int c = 0; c < 3; ++c) moonPush[4][c] = m[c] / ml;
        moonPush[4][3] = 3.f;
        vkCmdPushConstants(cmd, mPkLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(moonPush), moonPush);
        vkCmdDispatch(cmd, (kViewW + 7) / 8, (kViewH + 7) / 8, 1);
    }
    std::memcpy(mPkViewKey, view_key, sizeof(view_key));
    mPkViewValid = true;
    // To every reader: the sky, probe and lighting FS, and the computes that run the atmosphere.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gpuSubMark(cmd, "c_physsky_lut");
}

void LLCleanRendererVK::physSkyDestroy()
{
    sHazePhysicalActive.store(false,std::memory_order_relaxed);
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (int i = 0; i < 3; ++i)
    {
        if (mPkPipe[i]) vkDestroyPipeline(dev, mPkPipe[i], nullptr);
        mPkPipe[i] = VK_NULL_HANDLE;
        mPkSet[i] = VK_NULL_HANDLE;
    }
    if (mPkLayout) vkDestroyPipelineLayout(dev, mPkLayout, nullptr);
    mPkLayout = VK_NULL_HANDLE;
    mPkPool.destroy();
    if (mPkDsl) vkdDestroySetLayout(dev, mPkDsl);
    mPkDsl = VK_NULL_HANDLE;
    if (mPkSampler) vkDestroySampler(dev, mPkSampler, nullptr);
    mPkSampler = VK_NULL_HANDLE;
    if (mPkTrans.image) mCtx->destroyImage(mPkTrans);
    if (mPkMs.image) mCtx->destroyImage(mPkMs);
    if (mPkView.image) mCtx->destroyImage(mPkView);
    mPkTrans = mPkMs = mPkView = GpuImage{};
    mPkMediumValid = mPkViewValid = mPkPipeFailed = mPkWanted = false;
}
