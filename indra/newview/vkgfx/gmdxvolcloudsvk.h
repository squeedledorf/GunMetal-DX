/**
 * @file gmdxvolcloudsvk.h
 * @brief GunMetal-DX's volumetric clouds for the Vulkan clean renderer: its own cloud model on
 *        Slipstream's #719 volumetric cloud module.
 *
 * Copyright (C) 2026, squeedledorf (GunMetal-DX). The module's structure (images, sets, the
 * partial march + temporal reconstruction + composite, the shadow map and panorama scheduling)
 * is Slipstream's LLCleanVolCloudsVK (llcleanvolcloudsvk.h), which this class follows method for
 * method; the cloud model in the shaders is GunMetal-DX's (gmdx_cloud_common.glsl).
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

// RenderVKVolumetricClouds 1 with GunMetal-DX's cloud model: a raymarched cloud shell following
// the planet's curve between RenderVKVolCloudAltitude and + RenderVKVolCloudThickness, shaped from
// the region's EEP sky by Wicked Engine's cloud types (small / medium / large, with anvil tops),
// sculpted cumulus bodies among them (GMDXCloudBodies), lit per sample by the physical sky's
// sunlight (the sunset's own colour at each cloud), the sky, the ground's bounce and, at night,
// the moon as a second light; the corona around both. Shaders: gmdx_cloud_*.glsl / .comp / .frag.
//
// A drop-in for LLCleanVolCloudsVK: the same public interface and Frame, the same outputs
// (ssr.vol_clouds, draw set 0 bindings 46 shadow map / 47 panorama / 64 panorama mean distance),
// so the renderer holds one or the other (LLCleanRendererVK::mVolClouds). Slipstream's parts kept
// as they are: its 3D noises (clean_vol_cloud_noise.comp), the sculpted bodies' image
// (LLCleanCloudShapesVK), the world coverage field and the coherent wind (#1283), the physical
// camera medium's raw-radiance contract (#1266), the lightning flash (#783).
//
// DATA FLOW, per recording (LLCleanRendererVK::recordWorld) -- as llcleanvolcloudsvk.h:
//   prepare()         CPU, before the frame-info upload: settings, light, wind, view matrices, the
//                     shadow map's refresh, the panorama band. live() -> ssr.vol_clouds mode 2.
//   recordPre()       the params UBO, the 3D noises (once), the EEP weather map + world coverage.
//   recordShadow()    the cloud shadow map (every 4 frames, a light turn, a camera move, a crossing).
//   recordPano()      a band of the panorama (probes + mirror sky).
//   recordMarch()     after pass 1: the quarter-resolution partial march; recordTemporal() its
//                     reconstruction; recordComposite() over the HDR scene before pass 2.
//
// Settings: RenderVKVolCloud* (Graphics > Sky; GunMetal-DX defaults quality 3, thickness 2500 m),
// GMDXCloudBodies, GMDXCloudWindSpeed. The deep tuning is constants with live knobs in the Sky
// Debugger (gmdxskytune.h). GPU sub-marks: the renderer's c_vcloud_*.

#ifndef GMDX_VOLCLOUDSVK_H
#define GMDX_VOLCLOUDSVK_H

#include "llcleanvolcloudsvk.h"   // Frame, LLCleanCloudShapesVK

#include <cstdint>

class GMDXVolCloudsVK
{
public:
    using Frame = LLCleanVolCloudsVK::Frame;   // what prepare() needs from the renderer

    GMDXVolCloudsVK() = default;
    ~GMDXVolCloudsVK() = default;   // destroy() is explicit (needs the device)
    GMDXVolCloudsVK(const GMDXVolCloudsVK&) = delete;
    GMDXVolCloudsVK& operator=(const GMDXVolCloudsVK&) = delete;

    // Device idle, before recording: the fixed-size images, samplers, layouts, pools, the params
    // UBO; starts the pipeline builds. false = an allocation failed (everything freed; retry later).
    bool create(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkDescriptorSetLayout dslDraw1,
                VkFormat hdrFormat);
    void destroy(VkDevice dev);
    bool created() const { return mParamBuf.buffer != VK_NULL_HANDLE; }
    // Draw set 0 bindings 46 / 47 / 64 (LLCleanRendererVK::bindVolCloudImages).
    VkSampler   exportSampler() const { return mSampExport; }
    VkImageView shadowView() const    { return mShadow.view; }
    VkImageView panoView() const      { return mPano.view; }
    VkImageView panoMeanView() const  { return mPanoMean.view; }
    bool layoutsMatch(VkDescriptorSetLayout dslDraw0, VkDescriptorSetLayout dslDraw1, VkFormat hdrFormat) const
    {
        return dslDraw0 == mPipeDslDraw0 && dslDraw1 == mPipeDslDraw1 && hdrFormat == mPipeHdrFormat;
    }
    bool pipelinesPending() const { return mJob.pending(); }

    bool prepare(const Frame& f);
    void invalidateHistory() { mHistValid = false; }
    bool live() const { return mLive; }
    void fillFrameInfo(float out[4][4]) const;
    bool recordPre(VkCommandBuffer cmd);
    bool recordShadow(VkCommandBuffer cmd);
    bool recordPano(VkCommandBuffer cmd);
    bool recordMarch(VkCommandBuffer cmd, VkImage depthImage, VkImageView depthView, uint64_t depthGen);
    bool recordTemporal(VkCommandBuffer cmd);
    void recordComposite(VkCommandBuffer cmd, VkImageView hdrView);
    bool compositeWanted() const { return mLive && mMainNow; }

    // Params UBO (gmdx_cloud_ubo.glsl GmdxCloudParams, std140).
    struct Params
    {
        float shape0[4], shape1[4], shape2[4], shape3[4], light[4], march[4], cam[4], shadow[4], pano[4];
        float inv_vp[16], reproj[16];
        float dims[4], pat[4], temporal[4], flash[4];
        float origin[4], bodies[4];
        float wmap[4], wnoise[4], wrot[4];
        float debug[4];
    };
    static_assert(sizeof(Params) == 432, "Params = gmdx_cloud_ubo.glsl GmdxCloudParams (std140)");

private:
    bool ensurePipelines();
    bool ensureTargets(VkCommandBuffer cmd, uint32_t w, uint32_t h);
    void destroyTargets();
    void writeMainSets();

    VkDeviceContext* mCtx = nullptr;
    // Samplers: linear repeat (noises, weather), linear clamp (history, world coverage, bodies),
    // the export sampler for draw set 0 (linear, U repeat -- the panorama's azimuth -- V clamp),
    // nearest.
    VkSampler mSampRepeat = VK_NULL_HANDLE, mSampClamp = VK_NULL_HANDLE;
    VkSampler mSampExport = VK_NULL_HANDLE, mSampNearest = VK_NULL_HANDLE;

    // Layouts. mDsl (compute set 1, with draw set 0 as set 0): 0 params, 1 base, 2 detail,
    // 3 weather, 4 depth, 5 / 6 march A / B, 7 / 8 history A / B (read), 9 / 10 history A / B
    // (write), 11 shadow map, 12 panorama, 13 the sculpted cloud bodies, 14 panorama mean
    // distance, 15 world coverage. mNoiseDsl: 0 the noise volume (storage). mWxDsl (set 2 after
    // draw sets 0 / 1): 0 the weather map, 1 the params, 2 the world coverage. mCompDsl (fragment,
    // set 1 after draw set 0): 0 params, 1 depth, 2 A, 3 B, 4 weather, 5 world coverage.
    LLCleanCloudShapesVK mShapes;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE, mNoiseDsl = VK_NULL_HANDLE;
    VkDescriptorSetLayout mWxDsl = VK_NULL_HANDLE, mCompDsl = VK_NULL_HANDLE;
    VkDescPool       mPool;
    VkDescriptorSet  mPreSet = VK_NULL_HANDLE, mNoiseSet[2] = {}, mWxSet = VK_NULL_HANDLE;
    VkDescriptorSet  mMainSet[2] = {}, mCompSet[3] = {};
    VkPipelineLayout mNoiseLayout = VK_NULL_HANDLE, mWxLayout = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE, mCompLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mPipeDslDraw0 = VK_NULL_HANDLE, mPipeDslDraw1 = VK_NULL_HANDLE;
    VkFormat         mPipeHdrFormat = VK_FORMAT_UNDEFINED;
    enum { P_NOISE_BASE, P_NOISE_DETAIL, P_WEATHER, P_SHADOW, P_PANO, P_MARCH, P_TEMPORAL, P_COMP, P_COUNT };
    VkPipeline       mPipe[P_COUNT] = {};
    VkPipeline       mStage[P_COUNT] = {};
    PipelineJobSlot  mJob;
    bool             mPipeReady = false, mPipeFailed = false;

    // Fixed images (GENERAL for good).
    GpuImage  mNoiseBase, mNoiseDetail, mWeather, mWorld, mShadow, mPano, mPanoMean;
    GpuBuffer mParamBuf;
    bool      mNoiseDone = false;
    // Main-view targets (march resolution) + the history pair (A / B per parity).
    GpuImage  mMarchA, mMarchB, mHistA[2], mHistB[2];
    uint32_t  mW = 0, mH = 0;
    VkImageView mBoundDepthView = VK_NULL_HANDLE;
    uint64_t  mBoundDepthGen = 0;
    uint32_t  mHistCur = 0;
    bool      mHistValid = false;

    // This recording (prepare -> record*).
    Params    mP{};
    bool      mLive = false, mMainNow = false, mOffscreen = false, mPhysical = false;
    bool      mShadowNow = false, mPanoNow = false, mTemporalNow = false, mMarchRan = false;
    VkDescriptorSet mDsetDraw0 = VK_NULL_HANDLE, mDsetDraw1 = VK_NULL_HANDLE;
    uint32_t  mGroupsX = 0, mGroupsY = 0;
    float     mVc[4][4] = {};               // ssr.vol_clouds
    // Shadow map latch: centre, the agent frame's origin, the unwrapped wind (m) and the light at
    // the refresh; frames since.
    double    mShCentre[2] = {}, mShOrigin[2] = {}, mShWind[2] = {};
    float     mShLight[3] = {};
    uint32_t  mShAge = ~0u;
    bool      mShValid = false;
    uint32_t  mPanoRow = 0, mPanoFilled = 0;
    // Temporal bookkeeping (main view only). Wind (m): the total since the module started
    // (unwrapped, GMDXCloudWindSpeed in it) and its part past the region's own scroll; the region's
    // scroll in tiles (wrapped by 21).
    uint32_t  mFrame = 0;
    float     mPrevViewProjRel[16] = {};
    double    mPrevCam[3] = {};
    double    mWind[2] = {}, mWindExtra[2] = {}, mScrollTiles[2] = {}, mPrevScroll[2] = {};
    bool      mWindValid = false;
    double    mPrevBasePeriod = 0.;
    float     mPrevLayer[2] = {};
    uint32_t  mPrevQuality = 0;
    uint32_t  mLogQuality = ~0u, mLogW = 0, mLogH = 0;
};

#endif // GMDX_VOLCLOUDSVK_H
