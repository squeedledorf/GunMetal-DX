/**
 * @file llcleanvolcloudsvk.h
 * @brief Volumetric sky cloud layer (#719, part of the volumetrics epic #722) for the Vulkan
 *        clean renderer.
 *
 * RenderVKVolumetricClouds 1: a raymarched cloud shell between RenderVKVolCloudAltitude and
 * + RenderVKVolCloudThickness (EEP has no altitude), shaped and lit from the region's EEP
 * sky so region skies keep their character (shaders: clean_vol_cloud_*.glsl / .comp / .frag,
 * shared math in clean_vol_cloud_common.glsl). 0 (default): the classic 2D cloud dome
 * (clean_clouds.glsl), this object does not exist and nothing is allocated.
 *
 * DATA FLOW, per recording (LLCleanRendererVK::recordWorld):
 *   prepare()        CPU, before the frame-info upload: settings, light, view matrices, the
 *                    shadow map's centre / refresh decision, the panorama band. live() then
 *                    says whether fillFrameInfo writes mode 2 into ssr.vol_clouds -- the
 *                    switch every consumer reads (2D dome off, cloud shadow, panorama).
 *   recordPre()      after the frame-info upload, before any lit pass / probe capture /
 *                    mirror: the params UBO, the 3D noises (once), the EEP weather map
 *                    (every frame), the cloud SHADOW MAP (every kShadowEvery frames or on a
 *                    light turn / camera move; the lookup's centre follows the wind between
 *                    refreshes) and a band of the PANORAMA (probes + mirror sky).
 *   recordMarch()    after pass 1 (opaque depth DEPTH_READ_ONLY): the quarter-resolution
 *                    partial march, then recordTemporal() the reconstruction.
 *   recordComposite() its own rendering scope on the HDR scene, before pass 2a: depth-aware
 *                    upsample, scene x T + in-scatter.
 *
 * Draw set 0 bindings 46 (shadow map) / 47 (panorama) point at this module's images while
 * it exists (LLCleanRendererVK::bindVolCloudImages, device idle, before recording), the 1x1
 * white stand-in otherwise. Pipelines build in the background; until they land the layer is
 * not live (the 2D dome stays). Snapshots (offscreen) march every texel with no history.
 *
 * GPU sub-marks: c_vcloud_wx (UBO + noise + weather), c_vcloud_shadow, c_vcloud_pano,
 * c_vcloud_march, c_vcloud_temporal, g_vcloud_comp.
 */
#ifndef LL_LLCLEANVOLCLOUDSVK_H
#define LL_LLCLEANVOLCLOUDSVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot

#include <cstdint>

class LLCleanVolCloudsVK
{
public:
    // What prepare() needs from the renderer this recording.
    struct Frame
    {
        VkDeviceContext*      ctx = nullptr;
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;   // draw set 0 (frame info, bindings 46 / 47)
        VkDescriptorSetLayout dslDraw1 = VK_NULL_HANDLE;   // draw set 1 (bindless: the EEP cloud noise)
        VkDescriptorSet       dsetDraw0 = VK_NULL_HANDLE;
        VkDescriptorSet       dsetDraw1 = VK_NULL_HANDLE;  // this frame slot's bindless set
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;          // the composite's attachment
        uint32_t rw = 0, rh = 0;                           // render extent
        const float* viewProjRel = nullptr;                // UNJITTERED P * rotation-only view (16)
        const float* camTime = nullptr;                    // camera world pos xyz, time
        const float* clouds = nullptr;                     // LLCleanRendererVK::mClouds (16; clean_clouds.glsl)
        const float* atmos = nullptr;                      // LLCleanRendererVK::mAtmos (64)
        bool cloudNoise = false;                           // the EEP noise texture is resolved (bindless)
        bool underwater = false;                           // camera under water: no main-view composite
        bool offscreen = false;                            // snapshot: every texel, no history
        uint32_t snapSample = 0;                           // #932: a supersampled shot's recording 1..N (0 = none)
        bool cameraCut = false;                            // the renderer's temporal reset this frame
    };

    LLCleanVolCloudsVK() = default;
    ~LLCleanVolCloudsVK() = default;   // destroy() is explicit (needs the device)
    LLCleanVolCloudsVK(const LLCleanVolCloudsVK&) = delete;
    LLCleanVolCloudsVK& operator=(const LLCleanVolCloudsVK&) = delete;

    // Device idle, before recording: the fixed-size images (noises, weather, shadow map,
    // panorama), samplers, layouts, pools, the params UBO; starts the pipeline builds.
    // false = an allocation failed (everything freed; the caller retries later).
    bool create(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkDescriptorSetLayout dslDraw1,
                VkFormat hdrFormat);
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);
    bool created() const { return mParamBuf.buffer != VK_NULL_HANDLE; }
    // Draw set 0 bindings 46 / 47 (LLCleanRendererVK::bindVolCloudImages).
    VkSampler   exportSampler() const { return mSampExport; }
    VkImageView shadowView() const    { return mShadow.view; }
    VkImageView panoView() const      { return mPano.view; }
    // The pipelines the renderer's layouts were built for changed (a set layout re-created):
    // the caller destroys + re-creates the module.
    bool layoutsMatch(VkDescriptorSetLayout dslDraw0, VkDescriptorSetLayout dslDraw1, VkFormat hdrFormat) const
    {
        return dslDraw0 == mPipeDslDraw0 && dslDraw1 == mPipeDslDraw1 && hdrFormat == mPipeHdrFormat;
    }
    bool pipelinesPending() const { return mJob.pending(); }

    // CPU, every recording, before the frame-info upload. true = live this recording.
    bool prepare(const Frame& f);
    bool live() const { return mLive; }
    // ssr.vol_clouds (clean_frame_info.glsl): zeros unless live().
    void fillFrameInfo(float out[4][4]) const;
    // After the frame-info upload, before the first consumer. Records the UBO update, the noises
    // (first time), the weather map. true = recorded (sub-mark c_vcloud_wx).
    bool recordPre(VkCommandBuffer cmd);
    // After recordPre: the shadow map refresh when due. true = recorded (c_vcloud_shadow).
    bool recordShadow(VkCommandBuffer cmd);
    // After recordShadow: the panorama band. true = recorded (c_vcloud_pano).
    bool recordPano(VkCommandBuffer cmd);
    // After pass 1, depth DEPTH_READ_ONLY_OPTIMAL. true = recorded (c_vcloud_march); the
    // composite is then owed this recording.
    bool recordMarch(VkCommandBuffer cmd, VkImage depthImage, VkImageView depthView, uint64_t depthGen);
    // After recordMarch. true = recorded (c_vcloud_temporal).
    bool recordTemporal(VkCommandBuffer cmd);
    // Outside any rendering scope, after recordMarch / recordTemporal: opens a scope on the HDR
    // scene (COLOR_ATTACHMENT_OPTIMAL, LOAD / STORE), draws, closes it and orders its writes
    // before pass 2's colour attachment access. No-op when the march did not run.
    void recordComposite(VkCommandBuffer cmd, VkImageView hdrView);
    // Whether this recording composites over the main view (prepare's decision: live, not
    // under water, pipelines ready). The sky FS then leaves the clouds to the composite.
    bool compositeWanted() const { return mLive && mMainNow; }

    // Params UBO (clean_vol_cloud_ubo.glsl, std140).
    struct Params
    {
        float shape0[4], shape1[4], shape2[4], phase[4], march[4], cam[4], shadow[4], pano[4];
        float inv_vp[16], reproj[16];
        float dims[4], pat[4], temporal[4];
        float flash[4];   // #783 M2: xyz unit direction to a lightning strike, w its sky radiance (0 = none)
    };
    static_assert(sizeof(Params) == 320, "Params = clean_vol_cloud_ubo.glsl VolCloudParams (std140)");

private:
    bool ensurePipelines();
    bool ensureTargets(VkCommandBuffer cmd, uint32_t w, uint32_t h);
    void destroyTargets();
    void writeMainSets();

    VkDeviceContext* mCtx = nullptr;
    // Samplers: linear repeat (noises, weather), linear clamp (history), the export sampler
    // for bindings 46 / 47 (linear, U repeat -- the panorama's azimuth -- V clamp), nearest.
    VkSampler mSampRepeat = VK_NULL_HANDLE, mSampClamp = VK_NULL_HANDLE;
    VkSampler mSampExport = VK_NULL_HANDLE, mSampNearest = VK_NULL_HANDLE;

    // Layouts. mDsl (compute set 1, with draw set 0 as set 0): 0 params, 1 base, 2 detail,
    // 3 weather, 4 depth, 5 / 6 march A / B, 7 / 8 history A / B (read), 9 / 10 history A / B
    // (write), 11 shadow map, 12 panorama. mNoiseDsl: 0 the noise volume (storage). mWxDsl
    // (set 2 after draw sets 0 / 1): 0 the weather map (storage). mCompDsl (fragment): 0 params,
    // 1 depth, 2 A, 3 B.
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE, mNoiseDsl = VK_NULL_HANDLE;
    VkDescriptorSetLayout mWxDsl = VK_NULL_HANDLE, mCompDsl = VK_NULL_HANDLE;
    VkDescPool       mPool;
    // Sets. mPreSet: params / noises / weather / shadow / panorama (written once at create).
    // mMainSet[p]: + depth, march, history parity p read -> p ^ 1... (writeMainSets; rewritten,
    // device idle, when the targets or the depth view change -- never bound before that in a
    // recording). mCompSet[0] the raw march (snapshots / no temporal), [1 + p] history p.
    VkDescriptorSet  mPreSet = VK_NULL_HANDLE, mNoiseSet[2] = {}, mWxSet = VK_NULL_HANDLE;
    VkDescriptorSet  mMainSet[2] = {}, mCompSet[3] = {};
    VkPipelineLayout mNoiseLayout = VK_NULL_HANDLE, mWxLayout = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE, mCompLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mPipeDslDraw0 = VK_NULL_HANDLE, mPipeDslDraw1 = VK_NULL_HANDLE;
    VkFormat         mPipeHdrFormat = VK_FORMAT_UNDEFINED;
    // Pipelines (background batch: mStage until the job lands).
    enum { P_NOISE_BASE, P_NOISE_DETAIL, P_WEATHER, P_SHADOW, P_PANO, P_MARCH, P_TEMPORAL, P_COMP, P_COUNT };
    VkPipeline       mPipe[P_COUNT] = {};
    VkPipeline       mStage[P_COUNT] = {};
    PipelineJobSlot  mJob;
    bool             mPipeReady = false, mPipeFailed = false;

    // Fixed images (GENERAL for good).
    GpuImage  mNoiseBase, mNoiseDetail, mWeather, mShadow, mPano;
    GpuBuffer mParamBuf;
    bool      mNoiseDone = false;
    // Main-view targets (march resolution) + the history pair (A / B per parity).
    GpuImage  mMarchA, mMarchB, mHistA[2], mHistB[2];
    uint32_t  mW = 0, mH = 0;
    VkImageView mBoundDepthView = VK_NULL_HANDLE;
    uint64_t  mBoundDepthGen = 0;
    uint32_t  mHistCur = 0;          // the parity written this recording
    bool      mHistValid = false;    // the last main-view recording wrote mHist*[mHistCur]

    // This recording (prepare -> record*).
    Params    mP{};
    bool      mLive = false, mMainNow = false, mOffscreen = false;
    bool      mShadowNow = false, mPanoNow = false, mTemporalNow = false, mMarchRan = false;
    VkDescriptorSet mDsetDraw0 = VK_NULL_HANDLE, mDsetDraw1 = VK_NULL_HANDLE;
    uint32_t  mGroupsX = 0, mGroupsY = 0;   // the march dispatch
    float     mVc[4][4] = {};               // ssr.vol_clouds
    // Shadow map latch: centre, light, scroll at the refresh; frames since.
    double    mShCentre[2] = {}, mShScroll[2] = {};
    float     mShLight[3] = {};
    uint32_t  mShAge = ~0u;
    bool      mShValid = false;
    // Panorama: next band, rows written since creation (valid once a full pass is in).
    uint32_t  mPanoRow = 0, mPanoFilled = 0;
    // Temporal bookkeeping (main view only).
    uint32_t  mFrame = 0;
    float     mPrevViewProjRel[16] = {};
    double    mPrevCam[3] = {};
    uint32_t  mPrevQuality = 0;
    uint32_t  mLogQuality = ~0u, mLogW = 0, mLogH = 0;
};

#endif
