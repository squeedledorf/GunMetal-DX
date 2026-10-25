/**
 * @file llcleandenoisevk.h
 * @brief Diffuse / specular lighting denoiser (#85 phase 2): SVGF / ReLAX-style temporal
 *        accumulation + variance-guided a-trous for noisy, DEMODULATED lighting signals.
 *
 * A general denoiser for any pass that estimates lighting stochastically (the first one: the
 * ray-traced projectors' stochastic sampler, RenderVKProjStochastic; next: ReSTIR DI #869, RT
 * reflections, GI). The contract is clean_pdn_common.glsl; in short:
 *
 *   PRODUCER (any compute pass, its own layout) writes, per denoiser texel, at the extent
 *   ensure() allocated (the render extent x 1 or 1/2, sampling render pixel
 *   (m + 0.5 + J) x render / denoiser -- the RT mask's jittered convention):
 *     IN   (inView, rgba16f x 2 layers): demodulated diffuse (rgb, a = confidence) and
 *          specular (rgb, a = hit distance m) -- divided by pdnDiffFactor / pdnSpecFactor;
 *     GEOM (geomView, rgba32ui x 2 layers): layer parity() -- view depth, normal + specular
 *          roughness, depth gradient, the signal reference W (pdnPackMeta);
 *     MOT  (motView, rgba32ui): the camera-only previous positions (surface, virtual image),
 *          the expected previous view depth.
 *   DENOISER (recordTemporal, then recordSpatial): history validation on GEOM + motion vectors,
 *   neighbourhood clamp, W anti-lag, separate diffuse / specular histories (specular length and
 *   kernel follow roughness and hit distance), N a-trous iterations.
 *   CONSUMER reads the FINAL image (finalIsIn(): IN after an odd iteration count, else OUT;
 *   outView / inView), rgb demodulated, diffuse alpha = view depth for a depth-aware upsample,
 *   and multiplies by the same factors with its own full-resolution material.
 *
 * SUN / MOON CHANNEL (#195 / #85 merge, RenderVKRTShadowMerge; ensure(.., sun = true)): a third
 * signal for the merged RT shadow pass -- the sun / moon VISIBILITY (rgb = visibility x the
 * translucent tint, as the RT shadow mask's sun) in IN / OUT layer 2 and its own history in
 * HIST layers 2 + parity (clean_pdn_common.glsl SUN CHANNEL): its own history length, penumbra
 * radius, moving-caster countdown, fast mean and variance, exact reprojection, a penumbra-gated
 * a-trous (contact shadows stay crisp), never cut by the projector W anti-lag. Allocated only
 * while the merge wants it (+16 + 32 B per texel); without it every image has exactly the
 * projector-only layout and the passes take exactly the projector-only paths.
 *
 * SPLIT CHANNEL (#869, RenderVKProjStochasticSplitK > 0; ensure(.., split = true)): the K dominant
 * lights' OWN visibility per texel, keyed by light (clean_pdn_common.glsl SPLIT CHANNEL) -- the
 * temporal pass follows each light's history by its id, the a-trous filters each light over the
 * taps that carry it; layers 0 / 1 then hold the REST's ratio. Present <=> GEOM has 5 layers.
 *
 * VRAM (VKMEM_DENOISE, the RT group of the breakdown): 112 B per denoiser texel (160 with the
 * sun channel; + 64 B with the split channel, + 80 B without the sun's) -- ~103 MB at 2560x1440
 * half extent, ~412 MB full; 4K: ~232 / ~930 MB. Soft
 * allocations (#697): a refusal leaves the 1x1 placeholders and the caller falls back to the
 * exact path. Every image is bounded by the extent; nothing grows.
 * GPU: the caller's sub-marks (c_dn_tmp / c_dn_sp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#ifndef LL_LLCLEANDENOISEVK_H
#define LL_LLCLEANDENOISEVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <cstdint>

class LLCleanDenoiserVK
{
public:
    // The per-frame knobs (the shaders' push block, clean_dn_common.glsl DnPush).
    struct Params
    {
        uint32_t atrous = 3;          // a-trous iterations (1..5)
        float maxHistDiffuse = 32.f;  // frames
        float maxHistSpecular = 24.f; // frames, rough lobes
        float minHistSpecular = 4.f;  // frames, a mirror
        float clampGamma = 1.5f;      // neighbourhood clamp width (sigmas)
        float lumPhi = 4.f;           // a-trous luminance edge-stop (sigmas)
        float depthPhi = 1.f;         // a-trous depth edge-stop
        float normalPow = 64.f;       // a-trous diffuse normal power
        float wCut = 0.25f;           // relative change of W that cuts the history (0 = off)
        uint32_t debug = 0;           // clean_pdn_common.glsl PDN_DBG_* (0..15; 8 / 9 the #869 split views)
        // #870 raw routing (clean_dn_common.glsl DN_F_RAW / DN_F_RAW_SUN): this frame's raw
        // samples pass through (no history, no a-trous filter) without any debug colouring.
        bool  raw = false;            // diffuse / specular channels
        bool  rawSun = false;         // the sun / moon channel
        uint32_t frame = 0;
        // NO-INFORMATION SAMPLES (the projector wall-bleed fix, RenderVKProjStochBleedFix;
        // clean_dn_common.glsl DN_F_INFO): a frame whose picks carried no light is a weight-0
        // sample -- the history holds (or the informative 3x3 seeds it); taps stay in every filter.
        bool  info = false;
        // (#869 / #85) ReSTIR-CONVERGED DIFFUSE: a-trous iterations for texels whose reused
        // reservoir and history are both long (clean_dn_common.glsl DN_CONV_SHIFT); 0 = all of them.
        uint32_t convIters = 0;
        // (#869) THE MOVING-LIGHT RULE (RenderVKReSTIRMoveCut / RenderVKReSTIRMoveHistory;
        // clean_dn_common.glsl DN_MOVE_SHIFT): the history (frames) of a texel lit by a light that
        // moved this frame (the producer's MOT w share); 0 = off.
        uint32_t moveHist = 0;
        // ... applied to the DIFFUSE channel only (clean_dn_common.glsl DN_F_MOVE_DIFF_ONLY): the GI
        // denoiser, whose specular channel is the sky visibility (no light in it to change).
        bool moveDiffuseOnly = false;
        // (#869) MOVING SURFACES (RenderVKReSTIRSurfaceMoveVeto; clean_dn_common.glsl
        // DN_SURF_MOVE_SHIFT): a texel whose surface moved on its own by more than this many
        // denoiser texels (motion vectors vs camera-only) never takes the converged shortcut;
        // 0 = off. Quarter-texel steps, up to 3.75.
        float surfMoveTexels = 0.f;
        // (#87) DISOCCLUSION HISTORY FIX (RenderVKGIDisoccFix; clean_dn_common.glsl DN_HFIX_SHIFT):
        // texels whose diffuse / specular history is shorter than this many frames get the spatial
        // boost (variance floor, history-length-weighted a-trous taps, wider luminance stop); 0 = off.
        // Up to 7.
        uint32_t histFix = 0;
        // (#87) REPROJECTION SEARCH (RenderVKGIReprojSearch; DN_F_REPROJ_SEARCH): a failed bilinear
        // reprojection takes the best-matching texel of the 3x3 around it.
        bool reprojSearch = false;
        // SUN / MOON CHANNEL (the merged RT shadow pass; used only when sun is set AND the
        // images carry the channel -- hasSun()).
        bool  sun = false;            // this frame's producer wrote the sun channel
        bool  sunCut = false;         // restart the sun history (sun <-> moon switch, a jump)
        bool  sunHard = false;        // hard sun (no disc): moving casters keep 1 frame
        bool  sunExact = true;        // exact history reprojection (RenderVKRTMaskExactReproj)
        float sunMaxHist = 32.f;      // frames (RenderVKRTShadowHistory)
        float sunDynHist = 2.f;       // moving-caster hard-shadow history (RenderVKRTShadowDynamicHistory)
        float sunDynMax = 4.f;        // moving-caster longest history (RenderVKRTShadowDynamicMaxHistory)
        float sunMove = 0.f;          // the light's angular step since last frame, in disc radii
        float sunSpatialCap = 12.f;   // a-trous penumbra radius cap (denoiser texels; RenderVKRTShadowSpatial)
        float sunClamp = 1.5f;        // moving-caster raw box width (sigmas; RenderVKRTShadowClamp)
        uint32_t sunPersist = 4;      // moving-caster countdown (frames; RenderVKRTShadowMoverPersist)
        float sunRays = 0.f;          // disc rays per texel this frame (0 = RenderVKRTSunSamples;
                                      // RenderVKShadowSunOneRay: 1) -- the variance bounds' count
    };

    LLCleanDenoiserVK() = default;
    ~LLCleanDenoiserVK() = default;   // destroy() is explicit (needs the device)
    LLCleanDenoiserVK(const LLCleanDenoiserVK&) = delete;
    LLCleanDenoiserVK& operator=(const LLCleanDenoiserVK&) = delete;

    // Layout, set, placeholders; the two pipelines queued (inside a PipelineBatchScope they land
    // with the batch). dslDraw0: the draw set 0 layout (frame info). nearest: a nearest / clamp
    // sampler for the motion vectors. false: nothing usable (the caller never enables it).
    bool init(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkSampler nearest, const char* name);
    void destroy();
    bool pipelinesReady() const { return mTempPipe != VK_NULL_HANDLE && mAtrousPipe != VK_NULL_HANDLE; }

    // The images at w x h (SOFT; waits for the device on any change). true: allocated at that
    // size; false: refused (the placeholders stay, history invalid). The caller rewrites its
    // own descriptors to the new views afterwards.
    // sun: with the sun / moon channel (IN / OUT 3 layers, HIST 4); a change re-allocates.
    // split (#869): with the SPLIT channel (clean_pdn_common.glsl SPLIT CHANNEL: the dominant lights'
    // own visibility; GEOM 5 layers, IN / OUT 4); a change re-allocates.
    bool ensure(uint32_t w, uint32_t h, bool sun = false, bool split = false);
    void release();   // back to the placeholders (waits for the device)
    uint64_t bytesAt(uint32_t w, uint32_t h, bool sun = false, bool split = false) const;
    uint64_t bytes() const { return allocated() ? bytesAt(mW, mH, mSun, mSplit) : 0; }
    bool allocated() const { return mW > 1 || mH > 1; }
    bool hasSun() const { return allocated() && mSun; }
    bool hasSplit() const { return allocated() && mSplit; }
    // this frame's history is valid (beginFrame: the denoiser ran last frame at this size)
    bool historyValid() const { return mHistOk; }
    uint32_t width() const { return mW; }
    uint32_t height() const { return mH; }

    // Stable while allocated (placeholders otherwise): what the producer / consumer bind.
    VkImageView inView() const   { return mIn.view; }
    VkImageView geomView() const { return mGeom.view; }
    VkImageView motView() const  { return mMot.view; }
    VkImageView outView() const  { return mOut.view; }

    // The pre-pass motion vectors (render extent; layout as the renderer keeps them), or
    // VK_NULL_HANDLE for the placeholder. Callers make sure no frame in flight uses the set.
    void setMotion(VkImageView view, VkSampler sampler, VkImageLayout layout);

    // Once per frame that will run, before the producer: flips the GEOM / HIST parity; the
    // history is valid when the last frame ran at this size.
    void beginFrame();
    uint32_t parity() const { return mParity; }
    // The final image's identity for an iteration count (the consumer's push).
    static bool finalIsIn(uint32_t atrous) { return (clampIters(atrous) & 1u) != 0u; }
    static uint32_t clampIters(uint32_t n) { return n < 1u ? 1u : (n > 5u ? 5u : n); }

    // After the producer's writes (the barrier is recorded here): the temporal pass, then the
    // a-trous iterations. dsetDraw0: the draw set 0 (frame info). The final image is complete
    // for compute reads after the caller's next compute -> compute barrier.
    void recordTemporal(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, uint32_t rw, uint32_t rh, const Params& p);
    void recordSpatial(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, const Params& p);

private:
    struct Push { float dims[4]; float prm0[4]; float prm1[4]; uint32_t ctl[4]; float sun0[4]; float sun1[4]; };
    static_assert(sizeof(Push) == 96, "clean_dn_common.glsl DnPush");
    Push makePush(uint32_t rw, uint32_t rh, const Params& p) const;
    bool createSet(GpuImage& in, GpuImage& mot, GpuImage& geom, GpuImage& hist, GpuImage& out,
                   uint32_t w, uint32_t h, bool soft, bool sun, bool split);
    void destroyImages();
    void writeSet();
    void clearImages();

    VkDeviceContext*      mCtx = nullptr;
    const char*           mName = "denoiser";
    VkSampler             mNearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;   // {draw set 0, mDsl}, 64 B push
    VkPipeline            mTempPipe = VK_NULL_HANDLE;
    VkPipeline            mAtrousPipe = VK_NULL_HANDLE;
    GpuImage mIn{}, mMot{}, mGeom{}, mHist{}, mOut{};   // clean_pdn_common.glsl IMAGES
    GpuImage mMotionPh{};                               // binding 5 without motion vectors
    VkImageView   mMotionView = VK_NULL_HANDLE;
    VkSampler     mMotionSampler = VK_NULL_HANDLE;
    VkImageLayout mMotionLayout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t mW = 1, mH = 1;
    bool     mSun = false;      // the images carry the sun / moon channel
    bool     mSplit = false;    // (#869) ... and the split channel
    uint32_t mParity = 0;
    bool     mRan = false;      // recordTemporal ran since the last beginFrame
    bool     mHistOk = false;   // this frame's history is valid
    bool     mSunRan = false;   // the sun channel was accumulated since the last beginFrame
    bool     mSunHistOk = false;   // ... so this frame's sun history is valid
};

#endif // LL_LLCLEANDENOISEVK_H
