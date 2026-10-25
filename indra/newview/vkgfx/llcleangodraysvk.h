/**
 * @file llcleangodraysvk.h
 * @brief God rays / volumetric sun shafts (#208) for the Vulkan clean renderer.
 *
 * Two passes inside LLCleanRendererVK::recordWorld, owned here:
 *   1. MARCH (clean_godrays.comp): after pass 1, from the finished opaque depth, at a
 *      reduced resolution (tier / RenderVKGodRaysScale). Each texel marches its view ray
 *      through the sun/moon shadow cascades and integrates single scattering through
 *      the WindLight haze (Henyey-Greenstein phase) into an RGBA16F in-scatter target
 *      (rgb linear HDR, a = the sample's view distance).
 *   1b. TEMPORAL (clean_godrays_temporal.comp, RenderVKGodRaysTemporal, default on): the
 *      march is stochastic (rotating IGN start offset, cascade-blend dither, RT: one
 *      disc-jittered shadow ray per step), so its output is accumulated over frames at
 *      the march resolution -- camera reprojection on the stored view distance, a 2x2
 *      depth-tested history fetch, a variance-box clamp to the current 3x3, ~0.92
 *      history -- into a ping-pong history pair the composite then reads. Snapshots
 *      (offscreen) skip it. GPU sub-mark c_godrays_temporal.
 *   2. COMPOSITE (clean_godrays_composite.frag): a fullscreen draw, depth-aware 3x3
 *      upsample against the opaque depth, added ONE + ONE to the HDR scene (pre-exposure,
 *      pre-tonemap, so bloom / glow / TAA see it). RenderVKGodRaysAfterAlpha (default):
 *      the last draw of pass 2, over water + post-water alpha + particles (with
 *      RenderVKReactiveMask off + TAA: right after the water, before the reactive
 *      reference copy). Off: in pass 2a, after the pre-water alpha, before the
 *      refraction copy (the old order: transparents covered shafts in front of them).
 *
 *   1c. DENOISER (#657, RenderVKGodRaysDenoise; replaces 1b where it applies -- 1 = the
 *      ray-traced sun, 2 = the cascades too): the RT shadow mask's denoiser on the god
 *      rays. The march splits its estimate into a [0, 1] beam visibility v and the
 *      noise-free lit in-scatter Lmax (side image), and flags texels whose shadow rays met
 *      a MOVING caster (the RT mask's detector, clean_rt_moving_caster.glsl). TEMPORAL
 *      (clean_godrays_dn_temporal.comp): adaptive history length, neighbourhood /
 *      fast-history clamp, moving-caster change detector with a fast reset only there,
 *      reactive for TAA / FSR. SPATIAL (clean_godrays_atrous.comp): N a-trous iterations
 *      keyed on the view-distance key + luminance variance, then the finalize v x Lmax into
 *      the march target the composite reads. GPU sub-marks c_godrays_dn_tmp /
 *      c_godrays_dn_sp. Extra VRAM: Lmax + a meta pair, 3 x 8 B per march texel
 *      (~12.4 MB at 4K quarter resolution), only while it runs.
 *
 * Off (RenderVKGodRays 0, the default) this object does not exist: the renderer
 * creates it on the first enabled frame and deletes it once the effect is off again.
 * Ray-traced sun / moon (RenderVKSunShadows 2, shadow_params.w 2) renders no cascades:
 * when this frame traces (Frame::rtTrace) the march runs its GODRAYS_RT variant, one
 * shadow ray toward the light per step against the RT TLAS (the RT sun shadow's caster
 * rules), at quarter resolution with 4 / 6 / 8 rays per texel per frame (kRtTiers),
 * opaque-only past RenderVKGodRaysRTAlphaDist; its GPU sub-mark is c_godrays_rt
 * (c_godrays for the cascade march).
 * Skipped frames: shadows off, ray-traced sun mode without a traced TLAS this frame,
 * camera under water, no light.
 */
#ifndef LL_LLCLEANGODRAYSVK_H
#define LL_LLCLEANGODRAYSVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot (first-use pipelines off the render thread)

#include <cstdint>

class LLCleanGodRaysVK
{
public:
    // What the march needs from the renderer this frame.
    struct Frame
    {
        VkDeviceContext*      ctx = nullptr;
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;   // the draw set 0 layout (frame info, cascades)
        VkDescriptorSet       dsetDraw0 = VK_NULL_HANDLE;
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;          // pass 2 attachment formats
        VkFormat glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED;   // pass 2's reactive / water mask (F5; write-masked)
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkImage     depthImage = VK_NULL_HANDLE;           // world depth (render extent),
        VkImageView depthView = VK_NULL_HANDLE;            // DEPTH_READ_ONLY_OPTIMAL here
        // Bumped by the renderer whenever the depth image is re-created: the set rewrite
        // keys on it, not on depthView (a freed view's handle can come straight back for
        // its replacement, and the set would keep pointing at the freed image).
        uint64_t    depthGen = 0;
        uint32_t rw = 0, rh = 0;                           // render extent
        const float* viewProjRel = nullptr;                // UNJITTERED P * rotation-only view (16)
        const float* camTime = nullptr;                    // camera world pos xyz, time
        const float* jitterNdc = nullptr;                  // this frame's projection jitter, NDC (2)
        float shadowMode = 0.f;                            // frame info shadow_params.w (0 / 1 / 2 RT)
        // Ray-traced sun (shadowMode 2): this frame's TLAS + consumer set are valid and
        // the lit passes trace (mLitRt). The RT march binds {draw 0, draw 1, RT, own}.
        bool rtTrace = false;
        VkDescriptorSetLayout dslDraw1 = VK_NULL_HANDLE;   // draw set 1 (bindless textures)
        VkDescriptorSet       dsetDraw1 = VK_NULL_HANDLE;
        VkDescriptorSetLayout rtLayout = VK_NULL_HANDLE;   // LLCleanRTVK::consumerLayout()
        VkDescriptorSet       rtSet = VK_NULL_HANDLE;      // LLCleanRTVK::consumerSet()
        bool  underwater = false;                          // camera under water
        float lightLuma = 0.f;                             // sun/moon light colour luminance
        bool  offscreen = false;                           // snapshot: full res, 4x steps
        uint32_t snapSample = 0;                           // #932: a supersampled shot's recording 1..N (0 = none)
        // The renderer's temporal history reset this frame (teleport / region crossing /
        // origin shift / camera cut). Only meaningful while the renderer's temporal
        // state is on -- the caller gates it.
        bool cameraCut = false;
        // PHASE 2 (RenderVKOverlapGodRays): leave the march / temporal output's release
        // to the composite's fragment reads to recordCompositeRelease(), recorded right
        // before the composite, so pass 2's raster before it is not held behind the
        // march. Off: the release is recorded at the end of the march / temporal.
        bool deferRelease = false;
    };

    // Whether the last recordTemporal ran the denoiser's temporal pass (sub-mark name).
    bool lastDenoise() const { return mDnRan; }

    LLCleanGodRaysVK() = default;
    ~LLCleanGodRaysVK() = default;   // destroy() is explicit (needs the device)
    LLCleanGodRaysVK(const LLCleanGodRaysVK&) = delete;
    LLCleanGodRaysVK& operator=(const LLCleanGodRaysVK&) = delete;

    // Reads the settings, (re)creates what changed and dispatches the march. Call after
    // pass 1 has ended and the world depth is DEPTH_READ_ONLY_OPTIMAL; records its own
    // barriers. true = recordComposite() must be recorded in pass 2 this frame.
    bool recordMarch(VkCommandBuffer cmd, const Frame& f);
    // Inside a pass-2 rendering scope (HDR scene + glow + reactive mask + read-only
    // depth attachments, viewport / scissor already set): 2a, or the end of 2b
    // (RenderVKGodRaysAfterAlpha). Only after recordMarch returned true.
    // reactive: this site's mReact.R feeds the TAA / FSR resolve (RenderVKReactiveMask,
    // pass 2b) -- the denoiser's moving-caster shading change is written there.
    void recordComposite(VkCommandBuffer cmd, bool reactive = false);
    // After recordMarch returned true, before pass 2: the temporal accumulation
    // (RenderVKGodRaysTemporal). true = it ran (the caller's GPU sub-mark); the
    // composite then reads the accumulated history instead of the raw march.
    bool recordTemporal(VkCommandBuffer cmd);
    // After recordTemporal, denoiser mode only: the a-trous iterations + finalize into
    // the march target (RenderVKGodRaysAtrous). true = it ran (the caller's sub-mark).
    bool recordSpatial(VkCommandBuffer cmd);
    // Deferred release (Frame::deferRelease): the march / temporal writes -> the
    // composite's fragment reads. Outside any rendering scope, after recordMarch (and
    // recordTemporal) and before recordComposite. No-op when nothing is owed.
    void recordCompositeRelease(VkCommandBuffer cmd);
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);
    // Which march the last recordMarch() that returned true ran (sub-mark name).
    bool lastRt() const { return mLastRt; }
    // The next main-view frame starts a fresh temporal history (the renderer's
    // post-snapshot / camera-cut reset).
    void invalidateHistory() { mHistValid = false; }
    // Nothing allocated (the renderer drops the object then).
    bool idle() const { return !mMarchPipe && !mMarchRtPipe && !mOut.image && !mHist[0].image && !mLmax.image
                               && !pipelinesPending(); }
    // Login warm-up (LLCleanRendererVK::warmFirstUse): starts the pipeline builds this Frame's
    // formats / layouts need (+ the ray-traced march when f.rtLayout / f.dslDraw1 are set),
    // nothing recorded. true while a build is in flight.
    bool warm(const Frame& f);
    bool pipelinesPending() const { return mJob.pending() || mRtJob.pending(); }

private:
    bool ensurePipelines(const Frame& f);
    bool ensureRtPipeline(const Frame& f);
    void destroyRtPipeline(VkDevice dev);
    void ensureTarget(VkCommandBuffer cmd, const Frame& f, uint32_t w, uint32_t h);
    void destroyTarget();
    // true = (re)created. dn: also the denoiser's meta pair + Lmax (freed without it).
    bool ensureHistory(VkCommandBuffer cmd, uint32_t w, uint32_t h, bool dn);
    void destroyHistory();
    void destroyDenoiseImages();
    void writeSets();   // every set from the current targets (callers waited idle)

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mSampler = VK_NULL_HANDLE;   // nearest / clamp (texelFetch only)
    // 0 depth, 1 in-scatter (storage), 2 in-scatter (sampled), 3 Lmax (storage: the
    // march's denoiser output), 4 Lmax (sampled: a = the composite's reactive). 3 / 4 hold
    // mOut as a placeholder without the denoiser (never dynamically used then).
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mMarchLayout = VK_NULL_HANDLE;   // {draw set 0, mDsl}
    VkPipelineLayout      mCompLayout = VK_NULL_HANDLE;    // {mDsl}
    VkPipeline            mMarchPipe = VK_NULL_HANDLE;
    VkPipeline            mCompPipe = VK_NULL_HANDLE;
    // Temporal accumulation: {mTempDsl} = 0 march output, 1 history (read), 2 history
    // (write); one set per ping-pong parity, and a composite set (mDsl) per parity whose
    // binding 2 is that parity's history. Pre-built: never updated while in flight.
    VkDescriptorSetLayout mTempDsl = VK_NULL_HANDLE;
    VkPipelineLayout      mTempLayout = VK_NULL_HANDLE;
    VkPipeline            mTempPipe = VK_NULL_HANDLE;
    VkDescriptorSet       mTempDset[2] = {};
    VkDescriptorSet       mCompDset[2] = {};
    VkFormat              mCompFormats[4] = {};            // hdr, glow, depth, react the composite was built for
    VkDescriptorSetLayout mPipeDslDraw0 = VK_NULL_HANDLE;  // set 0 layout the march was built for
    // Ray-traced march (GODRAYS_RT): {draw set 0, draw set 1, RT consumer set, mDsl},
    // built on the first traced frame; rebuilt when a set layout it was built for changes.
    VkPipelineLayout      mMarchRtLayout = VK_NULL_HANDLE;
    VkPipeline            mMarchRtPipe = VK_NULL_HANDLE;
    VkPipeline            mMarchRtDnPipe = VK_NULL_HANDLE;   // + moving-caster tracking (denoiser)
    VkDescriptorSetLayout mPipeRtDsl[3] = {};              // draw 0, draw 1, RT the RT march was built for
    bool                  mRtFailed = false;               // build failed: don't retry every frame
    // Background builds (ensurePipelines / ensureRtPipeline): the workers write the stages,
    // installed when the job has landed.
    PipelineJobSlot       mJob, mRtJob;
    VkPipeline            mStage[5] = {};                  // temporal, dn temporal, a-trous, march, composite
    VkPipeline            mRtStage[2] = {};                // RT march, RT march + denoiser
    bool                  mPipeFailed = false;             // the last build failed for mFail*: no rebuild
    VkFormat              mFailFormats[4] = {};
    VkDescriptorSetLayout mFailDsl = VK_NULL_HANDLE;
    bool                  mLastRt = false;

    // DENOISER (#657). Temporal {mDnTempDsl}: 0 march (sampled), 1 last acc, 2 last meta
    // (sampled), 3 acc out, 4 meta out (storage); a-trous {mAtDsl}: 0 acc, 1 meta,
    // 2 / 3 scratch (= last frame's acc / meta), 4 Lmax, 5 the march target (storage).
    // One set per parity each, pre-built.
    VkDescriptorSetLayout mDnTempDsl = VK_NULL_HANDLE;
    VkPipelineLayout      mDnTempLayout = VK_NULL_HANDLE;
    VkPipeline            mDnTempPipe = VK_NULL_HANDLE;
    VkDescriptorSet       mDnTempDset[2] = {};
    VkDescriptorSetLayout mAtDsl = VK_NULL_HANDLE;
    VkPipelineLayout      mAtLayout = VK_NULL_HANDLE;
    VkPipeline            mAtPipe = VK_NULL_HANDLE;
    VkDescriptorSet       mAtDset[2] = {};
    GpuImage mMeta[2];              // RGBA16F: length (+128 moving flag), fast moments, reactive
    GpuImage mLmax;                 // RGBA16F: the lit in-scatter (rgb), a = reactive (finalize)
    bool     mDnNow = false;        // this frame: the temporal pass is the denoiser's
    bool     mDnRan = false;        // the last recordTemporal ran it
    bool     mSpatialNow = false;   // this frame: recordSpatial dispatches
    bool     mCompReact = false;    // this frame's composite may write the reactive
    bool     mPrevDn = false;
    bool     mDnAllocFailed = false; // the denoiser images did not fit: plain temporal until destroy()
    uint32_t mAtIters = 1;          // dispatches (>= 1: the finalize)
    struct AtPush { float p0[4]; float p1[4]; } mAtPush{};

    GpuImage    mOut;                       // RGBA16F in-scatter, GENERAL for good
    uint32_t    mW = 0, mH = 0;
    VkImageView mBoundDepthView = VK_NULL_HANDLE;
    uint64_t    mBoundDepthGen = 0;

    // Temporal history (main view only; independent of mOut, which snapshots resize).
    GpuImage mHist[2];
    uint32_t mHistW = 0, mHistH = 0;
    uint32_t mHistCur = 0;          // the parity written this frame
    bool     mHistValid = false;    // the last main-view frame accumulated into mHist[mHistCur]
    bool     mTemporalNow = false;  // this frame: recordTemporal dispatches
    bool     mCompHist = false;     // this frame's composite reads mHist[mHistCur]
    bool     mDeferRelease = false; // this frame's Frame::deferRelease
    bool     mReleaseOwed = false;  // recordCompositeRelease still has to run
    uint32_t mFrame = 0;            // own noise-rotation counter (TAA's stops without TAA)
    float    mPrevViewProjRel[16] = {};
    double   mPrevCam[3] = {};
    bool     mPrevRt = false;
    uint32_t mPrevTier = 0, mPrevSteps = 0;
    struct TempPush { float reproj[16]; float ray_a[4]; float ray_b[4]; float ray_c[4]; float dims[4]; } mTempPush{};

    // This frame's composite push (recordMarch -> recordComposite).
    struct CompPush { float inv_view_proj[16]; float dims[4]; float jitter[4]; } mCompPush{};
    uint32_t mLogTier = ~0u, mLogW = 0, mLogH = 0, mLogSteps = 0;
    bool     mLogRt = false;
    bool     mLogDn = false;
};

#endif
