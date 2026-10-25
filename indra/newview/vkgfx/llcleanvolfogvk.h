/**
 * @file llcleanvolfogvk.h
 * @brief Volumetric fog (#718, epic #722 phase 1): froxel grid + local volume primitives for
 *        the Vulkan clean renderer.
 *
 * Our own implementation of published techniques: the camera-aligned froxel grid with
 * exponential depth slices and temporal reprojection (Wronski, "Volumetric Fog", AC4 /
 * SIGGRAPH 2014), physically based participating media and the energy-conserving
 * integration (Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite",
 * SIGGRAPH 2015).
 *
 * PASSES (compute, recorded by recordCompute; GPU sub-marks c_vol_*):
 *   0. PARTICLES (clean_vol_part.comp)      VOLUMETRIC PARTICLES (#721, RenderVKVolumetricParticles,
 *                                           opt-in): the GPU particle pool -> classify per
 *                                           distance bucket -> capped nearest-first selection
 *                                           into soft-sphere records, binned per 8x8-froxel
 *                                           screen tile; writes each particle's sprite fade
 *                                           (glow.z) for the lit particle VS. Sub-mark c_vol_part.
 *   1. INJECT    (clean_vol_inject.comp)    height fog + local volumes + the binned particles
 *                                           (+ the cloud-layer hook) -> medium (sigma_s,
 *                                           sigma_t) per cell; particle emission -> mEmit
 *   2. LIGHT     (clean_vol_light.comp)     sun / moon (cascades x cloudShadow), sky ambient,
 *                                           clustered local lights, HG phase; + the haze's
 *                                           shadowed sun scatter (the god rays term).
 *                                           Shadowed projectors (RenderVKVolumetricFogLocalShadows):
 *                                           atlas tap per shadow-mapped one; ray-traced ones
 *                                           by a stochastic weighted pick + one opaque shadow
 *                                           ray per cell (its VOL_RT variant, sub-mark
 *                                           c_vol_light_rt; {draw 0, draw 1, own, RT set}).
 *                                           Shadowed POINT lights (RenderVKVolumetricFogPointShadows)
 *                                           join the same pick: one ray toward the picked light
 *                                           (own prim sphere + own-fixture rule), so a lamp
 *                                           under a floor no longer lights the fog above it.
 *                                           The same variant carries the OCCLUSION TERMS: the
 *                                           sky ambient x a per-cell sky-visibility ray while
 *                                           GI runs (RenderVKVolumetricFogAmbientOcclusion), and
 *                                           a sun / moon shadow ray in ray-traced sun mode
 *                                           (RenderVKVolumetricFogSunRT): a sealed room's fog
 *                                           stays dark, a window / lamp still lights it
 *   3. TEMPORAL  (clean_vol_temporal.comp)  own reprojection + 6-neighbour clamp (anti-lag)
 *   4. INTEGRATE (clean_vol_integrate.comp) front-to-back -> (in-scatter, transmittance)
 * and the APPLY (clean_vol_apply.frag, recordApply): the first draw of render pass 2,
 * C' = C * T + L over opaques + sky. Transparents (alpha FS, water, particles) sample the
 * integrated volume themselves through draw set 0 binding 45 (clean_vol_apply.glsl).
 *
 * PLACEMENT (LLCleanRendererVK::recordWorld): the froxels need no depth -- only the
 * cascades, the light clusters and the frame info -- so the compute is recorded right before
 * pass 1b (after the AO / RT-mask block, before the ray-traced probe faces) and, with async
 * compute (RenderVKAsyncVolumetrics; the #845 RDNA2 auto-disable applies through the
 * renderer's async availability), runs on the async queue beside pass 1b's G-buffer /
 * lighting raster. Joined (up to its own timeline value only) right before the apply.
 *
 * ZERO COST WHEN OFF: RenderVKVolumetricFog 0 -> the renderer never creates this object;
 * turned off -> destroy() + delete (no pipeline, image or buffer survives; draw set 0
 * binding 45 goes back to the renderer's 1x1x1 identity stub). Camera under water: no
 * passes that frame (the water fog owns it; the history resets).
 *
 * VRAM (RGBA16F 3D, all GENERAL): current cells + 2 history + integrated = 4 x 8 B per
 * cell: low 128x72x48 ~14 MB, medium 160x90x64 ~29.5 MB, high 192x108x96 ~64 MB; + the
 * volume list (2 x 6 KB host-visible). Volumetric particles on (only then): + the emission
 * grid (one more 8 B per cell: 3.5 / 7.4 / 16 MB) + the particle volume buffer (fixed
 * ~540 KB: header, distance histogram, 336 tiles x 256 indices, 4096 records x 48 B) + a
 * 2 x 16 B stats readback. Freed when the setting goes back to 0.
 */
#ifndef LL_LLCLEANVOLFOGVK_H
#define LL_LLCLEANVOLFOGVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot (first-use pipelines off the render thread)

#include <cstdint>
#include <functional>
#include <vector>

class LLCleanVolFogVK
{
public:
    // The frame-info block (LLCleanRendererVK::VKSSRInfo vol_* = clean_frame_info.glsl).
    struct FrameInfo
    {
        float vp[16];       // UNJITTERED P * rotation-only V: (world - eye) -> VK clip
        float inv_vp[16];   // its inverse
        float eye[4];       // xyz grid eye (world), w 1 = fog applies this frame (2: + particle sprite fade, #721)
        float grid[4];      // W, H, D, far (view depth, m)
        float slice[4];     // n (m), ln(1 + far / n), haze sun scatter (/m), HG g
        float fog[4];       // sigma0 (/m), height falloff (1/m), base height (m), ambient scale
        float albedo[4];    // rgb fog albedo, w analytic far segment on
        float misc[4];      // local-light scale, draw distance (m), water cut (m), sun shadows
    };
    static_assert(sizeof(FrameInfo) == 224, "FrameInfo = the vol_* block of clean_frame_info.glsl");

    // What the fog needs from the renderer.
    struct Frame
    {
        VkDeviceContext*      ctx = nullptr;
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;   // draw set 0 (frame info, lights, cascades, 40, 45)
        VkDescriptorSetLayout dslDraw1 = VK_NULL_HANDLE;   // draw set 1 (bindless: projector gobos)
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;          // render pass 2's attachments
        VkFormat glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        uint32_t rw = 0, rh = 0;                           // render extent
        uint32_t frameSlot = 0;                            // frame-in-flight slot (volume list ring)
        const float* viewProjRel = nullptr;                // UNJITTERED P * rotation-only V (16)
        const float* camTime = nullptr;                    // camera world pos xyz, time (s)
        const float* atmos = nullptr;                      // the renderer's atmos[64] (clean_atmos_common.glsl)
        float waterHeight = 0.f;
        float drawDistance = 256.f;                        // the main view's draw distance (m)
        float shadowMode = 0.f;                            // shadow_params.w as known now (0 / 1 / 2 RT)
        bool  underwater = false;
        bool  offscreen = false;                           // snapshot: no history, no jitter
        uint32_t snapSample = 0;                           // #932: a supersampled shot's recording 1..N (0 = none)
        bool  cameraCut = false;                           // the renderer's temporal reset this frame
        // Ray-traced projector shadows in the fog: LLCleanRTVK::consumerLayout() (null: no
        // ray tracing -- the RT light variant is not used), and whether the renderer's
        // projector shadows are ray traced now (builds the RT variant on first want; kept).
        VkDescriptorSetLayout rtLayout = VK_NULL_HANDLE;
        bool rtWanted = false;
        // Volumetric particles (#721): the renderer's GPU particle pool (CleanParticle x
        // partMax; null = no particle system) and whether the main view's particle draw uses
        // the lit VS (RenderVKParticleLighting != 0: the only VS that applies the sprite fade).
        VkBuffer partPool = VK_NULL_HANDLE;
        uint32_t partMax = 0;
        bool partSpriteFade = false;
    };

    // <OCOL> The agent frame's origin in global coordinates (m), set by the viewer each frame
    // (llappviewer.cpp): the OCOL mist's noise is global, continuous across regions.
    static void setOriginGlobal(double x, double y) { sOriginGlobal[0] = x; sOriginGlobal[1] = y; }
    static double sOriginGlobal[2];

    LLCleanVolFogVK() = default;
    ~LLCleanVolFogVK() = default;   // destroy() is explicit (needs the device)
    LLCleanVolFogVK(const LLCleanVolFogVK&) = delete;
    LLCleanVolFogVK& operator=(const LLCleanVolFogVK&) = delete;

    // Once per recording, BEFORE anything binds draw set 0 (the renderer's target-ensure
    // block): reads the settings, (re)creates what changed (device idle when it does), builds
    // the pipelines in the background, latches this frame's parameters. true = the fog runs
    // this recording (recordCompute + recordApply must follow). Off: frees everything.
    bool prepare(VkCommandBuffer cmd, const Frame& f);
    // The frame-info block for this recording (zeros + w 0 when not running).
    void fillFrameInfo(FrameInfo& out) const;
    // The four compute passes (after the cascades, the light clusters and the frame-info
    // upload; may be the async compute segment). shadowMode: the FINAL shadow_params.w
    // (after the lit variant), lightCount: this frame's local lights. Records its own
    // barriers; the integrated volume is released to fragment + compute reads at the end.
    // markFn(cmd, name): the renderer's GPU sub-mark after each pass.
    // draw01: the current draw set 0 and draw set 1 (bindless) descriptor sets.
    // projMode: this frame's projector shadow technique (0 off, 1 map, 2 ray traced);
    // rtSet: LLCleanRTVK::consumerSet() when this frame's TLAS is valid, else null.
    // partLive: this frame's particle compute ran (the pool is current) and particles are
    // shown -- else the volumetric particle pass is skipped.
    // giNow: ray-traced GI (#87) runs this frame -- the fog's ambient then takes the per-cell
    // sky visibility (RenderVKVolumetricFogAmbientOcclusion 1; clean_vol_light.comp header).
    template <class MarkFn>
    void recordCompute(VkCommandBuffer cmd, float shadowMode, uint32_t lightCount, const VkDescriptorSet draw01[2],
                       uint32_t projMode, VkDescriptorSet rtSet, bool partLive, bool giNow, MarkFn&& markFn)
    {
        recordComputeImpl(cmd, shadowMode, lightCount, draw01, projMode, rtSet, partLive, giNow,
                          [&](const char* n) { markFn(cmd, n); });
    }
    // Inside render pass 2's first scope (HDR + glow + reactive + read-only depth; viewport /
    // scissor set), as its first draw. dsetDraw0: the current draw set 0.
    void recordApply(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0);
    // This recording's main-view projection jitter (NDC xy; null = none) for the apply's depth
    // unprojection -- decided after prepare() (the renderer's jitter step).
    void setApplyJitter(const float* ndc)
    {
        mApplyPush.jitter[0] = ndc ? ndc[0] : 0.f;
        mApplyPush.jitter[1] = ndc ? ndc[1] : 0.f;
    }

    bool running() const { return mRunning; }
    // The integrated volume (draw set 0 binding 45 while it exists; else the renderer's stub).
    VkImageView volumeView() const { return mInteg.view; }
    // true once after the integrated volume was (re)created or freed: the renderer rewrites
    // binding 45 (the device is idle then -- prepare() waited before re-creating).
    bool takeBindingDirty() { const bool d = mBindingDirty; mBindingDirty = false; return d; }
    // Nothing allocated (the renderer deletes the object then).
    bool idle() const { return !mInteg.image && !mCur.image && !mInjectPipe && !mDsl && !mJob.pending() && !mRtJob.pending(); }
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);

    // GPU record of one local volume primitive (clean_vol_inject.comp VolPrim).
    struct VolPrimGPU
    {
        float pos_shape[4];   // centre xyz (world), w 0 box / 1 ellipsoid
        float axis_x[4];      // unit X axis, w half extent X (m)
        float axis_y[4];      // unit Y axis, w half extent Y (m)
        float ext_z[4];       // half extent Z, edge softness, core extinction (/m), noise freq (1/m)
        float albedo[4];      // rgb albedo, a noise amount
        float scroll[4];      // noise scroll (m/s), w bounding radius (m)
    };
    static_assert(sizeof(VolPrimGPU) == 96, "VolPrimGPU = clean_vol_inject.comp VolPrim");
    static constexpr uint32_t kVolCap = 64;   // per-frame list capacity (CPU + GPU appenders)

    // VOLUMETRIC PARTICLES (#721): the particle volume buffer (clean_vol_part_common.glsl).
    struct PVolGPU
    {
        float pos_r[4];   // centre xyz (world), w radius (m)
        float scat[4];    // rgb sigma_s at the core (/m), a sigma_t at the core (/m)
        float emit[4];    // rgb emitted radiance per metre at the core, a noise seed
    };
    static_assert(sizeof(PVolGPU) == 48, "PVolGPU = clean_vol_part_common.glsl PVol");
    static constexpr uint32_t kPVTile = 8;          // froxel columns per screen tile side (VP_TILE)
    static constexpr uint32_t kPVMaxTiles = 336;    // VP_MAX_TILES (the high tier's 24 x 14)
    static constexpr uint32_t kPVTileCap = 256;     // VP_TILE_CAP
    static constexpr uint32_t kPVHist = 32;         // VP_HIST
    static constexpr uint32_t kPVCapMax = 4096;     // RenderVKVolumetricParticleMax ceiling (records)
    // Byte offsets: hdr (16) + aux (16) + hist + tile counts = the per-frame cleared prefix;
    // then the tile indices, then the records.
    static constexpr VkDeviceSize kPVClearBytes = 32 + kPVHist * 4 + kPVMaxTiles * 4;
    static constexpr VkDeviceSize kPVRecOffset = kPVClearBytes + (VkDeviceSize)kPVMaxTiles * kPVTileCap * 4;
    static_assert(kPVRecOffset % 16 == 0, "std430: the PVol array starts 16-aligned");

private:
    void recordComputeImpl(VkCommandBuffer cmd, float shadowMode, uint32_t lightCount, const VkDescriptorSet draw01[2],
                           uint32_t projMode, VkDescriptorSet rtSet, bool partLive, bool giNow,
                           const std::function<void(const char*)>& mark);
    bool ensurePipelines(const Frame& f);
    bool ensureRtPipeline(const Frame& f);
    void destroyRtPipeline(VkDevice dev);
    bool ensureImages(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t d);
    void destroyImages();
    void writeSets();
    void updateTestVolumes(const Frame& f, const float* fwd);
    // Volumetric particles (#721): (re)creates / frees the emission grid, the particle volume
    // buffer and its readback for `want`, and rewrites the own sets (device idle) when what
    // bindings 5-7 name changed. false = not available this recording.
    bool ensureParticles(VkCommandBuffer cmd, bool want, VkBuffer pool);
    void destroyParticles();
    void readParticleStats();

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mSampler = VK_NULL_HANDLE;   // linear / clamp (history taps)
    // Own set (clean_vol_pass.glsl): 0 cur, 1 volume list, 2 history prev (sampled),
    // 3 history cur, 4 integrated. One per (history parity, frame slot).
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset[2][2] = {};
    VkPipelineLayout      mPassLayout = VK_NULL_HANDLE;    // {draw 0, draw 1, own}, 128 B push
    VkPipelineLayout      mApplyLayout = VK_NULL_HANDLE;   // {draw 0}, 32 B fragment push
    VkPipeline            mInjectPipe = VK_NULL_HANDLE, mLightPipe = VK_NULL_HANDLE;
    VkPipeline            mTemporalPipe = VK_NULL_HANDLE, mIntegratePipe = VK_NULL_HANDLE;
    VkPipeline            mApplyPipe = VK_NULL_HANDLE;
    VkPipeline            mPartPipe = VK_NULL_HANDLE;   // clean_vol_part.comp (#721; both passes)
    PipelineJobSlot       mJob;
    VkPipeline            mStage[6] = {};
    bool                  mPipeFailed = false;
    VkDescriptorSetLayout mPipeDsl[2] = {};   // draw 0 / draw 1 layouts the pipelines were built for
    VkFormat              mPipeFormats[4] = {};
    // Ray-traced light pass (VOL_RT): {draw 0, draw 1, own, RT consumer set}. Sets 0..2 and
    // the push range are mPassLayout's, so only set 3 is bound on top for its dispatch.
    VkPipelineLayout      mPassRtLayout = VK_NULL_HANDLE;
    VkPipeline            mLightRtPipe = VK_NULL_HANDLE;
    PipelineJobSlot       mRtJob;
    VkPipeline            mRtStage = VK_NULL_HANDLE;
    bool                  mRtFailed = false;
    VkDescriptorSetLayout mPipeRtDsl[4] = {};   // draw 0, draw 1, own, RT the RT pass was built for

    GpuImage  mCur, mHist[2], mInteg;
    uint32_t  mW = 0, mH = 0, mD = 0;
    bool      mBindingDirty = false;
    GpuBuffer mList[2];   // host-visible volume list ring (per frame slot)

    // Volumetric particles (#721).
    GpuImage  mEmit;              // emitted radiance per cell (rgb), the grid's dims
    GpuBuffer mPV;                // the particle volume buffer (device local)
    GpuBuffer mPVRb[2];           // its header per frame slot (host visible: stats)
    bool      mPVRbValid[2] = {};
    VkBuffer  mPartPool = VK_NULL_HANDLE;    // the pool the sets name (null: binding 5 = a stub)
    uint32_t  mPartMax = 0;
    VkBuffer  mSetPool = VK_NULL_HANDLE;     // what bindings 5 / 6 / 7 were last written with
    VkBuffer  mSetPV = VK_NULL_HANDLE;
    VkImageView mSetEmit = VK_NULL_HANDLE;
    bool      mPartOn = false;               // latched: the particle pass runs this recording
    uint32_t  mPartRetryFrame = 0;           // #697: an allocation refusal retries after this
    struct PartPush { float q[8][4]; } mPartPush{};   // clean_vol_part.comp PartVolPush (128 B)
    float     mPartNoise = 0.35f;
    bool      mPartLastOverCap = false;      // (log on the cap state's change)

    // Latched per recording (prepare -> fillFrameInfo / recordCompute / recordApply).
    bool      mRunning = false;
    FrameInfo mInfo{};
    uint32_t  mSlot = 0;
    uint32_t  mHistCur = 0;
    bool      mHistValid = false;
    bool      mReset = true;
    bool      mOffscreen = false;
    bool      mLocalVolumes = true;
    // <OCOL> OCOL mist (OCOLHeightFog): the inject pass's p0.yzw (breakup, bank size, roll
    // height; breakup 0 = off) and p1 (the banks' / wisps' wrapped noise offsets, m).
    float     mOcolMist[4] = {0.f, 0.f, 0.f, 0.f};
    float     mOcolOffsets[4] = {0.f, 0.f, 0.f, 0.f};
    float     mHistWeight = 0.9f;
    float     mLocalLights = 1.f;
    bool      mLocalShadows = true;   // RenderVKVolumetricFogLocalShadows
    float     mShadowDist = 64.f;     // RenderVKVolumetricFogShadowDistance (m; 0 = no limit)
    uint32_t  mShadowPicks = 1;       // RenderVKVolumetricFogShadowRays (1..4)
    bool      mLastRt = false;        // (log line on the RT light variant's state change)
    uint32_t  mLastRtTerms = ~0u;     // (+ which ray-traced terms it carries: 1 projectors, 2 ambient, 4 sun,
                                      //  8 point lights)
    // SHADOWED POINT LIGHTS (clean_vol_light.comp header; the "lamp under the bridge lights the fog
    // above the deck" fix): RenderVKVolumetricFogPointShadows, resolved per recording (0 off,
    // 1 on except attachment lights, 2 + attachment lights; quality tier 0 turns 1 off).
    uint32_t  mPointShadows = 1;
    float     mFixMax = 1.5f;         // RenderVKReSTIRFixtureSize (m): the own-fixture rule's size limit
    // OCCLUSION TERMS (the "fog lights up a sealed dark room" fix; clean_vol_light.comp header):
    uint32_t  mAmbOcc = 1;            // RenderVKVolumetricFogAmbientOcclusion: 0 off, 1 with GI, 2 whenever RT
    float     mAmbLen = 32.f;         // RenderVKVolumetricFogAmbientRayLength (m)
    bool      mSunRt = true;          // RenderVKVolumetricFogSunRT: shadow ray toward the sun in RT sun mode
    float     mOccDist = 128.f;       // RenderVKVolumetricFogOcclusionDistance (view depth, m; 0 = no limit)
    VkDescriptorSetLayout mRtLayoutNow = VK_NULL_HANDLE;   // this recording's RT consumer layout
    struct VolPush { float reproj[16]; float jitter[4]; float fwd[4]; float p0[4]; float p1[4]; } mPush{};
    float mPrecipMotion = 1.f;   // #954 the inject's precipitation motion strength (p0.w)
    struct ApplyPush { float dims[4]; float jitter[4]; } mApplyPush{};
    float     mPrevVP[16] = {};
    double    mPrevEye[3] = {};
    float     mPrevFar = 0.f;
    uint32_t  mFrame = 0;
    uint32_t  mLogTier = ~0u;

    // Debug test volumes (RenderVKVolumetricFogTestVolumes): anchored ahead of the camera
    // when the count changes; agent-frame positions (a region crossing moves them along).
    std::vector<VolPrimGPU> mTestVols;
    uint32_t  mTestCount = 0;
};

#endif
