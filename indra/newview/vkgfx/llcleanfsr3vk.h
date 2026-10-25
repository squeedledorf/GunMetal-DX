/**
 * @file llcleanfsr3vk.h
 * @brief AMD FidelityFX Super Resolution 3.1 (upscaler only) for the Vulkan clean
 *        renderer: RenderVKUpscaler 1-6.
 *
 * The FSR 3.1.4 upscaler passes are the SDK's own GLSL (indra/vkclean/third_party/
 * fidelityfx, MIT; vendored with its LICENSE and a README naming the exact version /
 * commit and its local patches), compiled with this renderer's other shaders into
 * vk_fsr3_shaders_spv.h. This class replaces the SDK's host side (ffx_fsr3upscaler.cpp
 * + the ffx Vulkan backend): it owns the internal resources, descriptor sets, constant
 * buffers and the per-frame pass sequence of fsr3upscalerDispatch(), recorded straight
 * into the renderer's command buffer with its own barriers.
 *
 * Per frame (all compute, graphics queue, after pass 2 -- where the TAA resolve runs):
 *   0. INPUTS (clean_fsr_inputs.comp, ours): complete motion vectors in FSR's convention
 *      (camera reprojection for sky / water / no pre-pass motion), depth with the water
 *      plane substituted, reactive (pass 2b's coverage mask or the reference-copy
 *      difference), transparency & composition (the water surface) and the exposure.
 *   1. prepare inputs   2. luma pyramid (SPD)   3. shading change pyramid (SPD)
 *   4. shading change   5. prepare reactivity   6. luma instability
 *   7. accumulate (+ sharpen permutation)   8. RCAS (sharpening on)   9. debug view
 * The output is the display-size linear HDR scene the post chain reads.
 *
 * Off (no FSR mode) this object does not exist: the renderer creates it on the first
 * FSR frame and deletes it once FSR is off again (VRAM discipline, as LLCleanGodRaysVK).
 */
#ifndef LL_LLCLEANFSR3VK_H
#define LL_LLCLEANFSR3VK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <chrono>
#include <cstdint>

class LLCleanFSRProbeVK;

class LLCleanFSR3VK
{
public:
    // What one upscale needs from the renderer. Input layouts at record time: colour /
    // motion / react / ref SHADER_READ_ONLY_OPTIMAL, depth DEPTH_READ_ONLY_OPTIMAL, the
    // exposure image GENERAL; every one of them already made visible to compute reads.
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        uint32_t rw = 0, rh = 0;        // render extent (all inputs)
        uint32_t dw = 0, dh = 0;        // display extent (the output)
        VkImageView colorView = VK_NULL_HANDLE;     // jittered linear HDR scene (render size)
        VkImageView depthView = VK_NULL_HANDLE;     // reversed-Z world depth (render size)
        VkImageView motionView = VK_NULL_HANDLE;    // RG16F pre-pass motion (cur - prev, UV)
        VkImageView reactView = VK_NULL_HANDLE;     // pass 2b mask: R reactive, G water, B no-history (blended, #605)
        VkImageView noTemporalView = VK_NULL_HANDLE;   // R8 no-history mask (motion pre-pass, #605)
        VkImageView refView = VK_NULL_HANDLE;       // scene before post-water alpha (mRefract)
        VkImageView exposureView = VK_NULL_HANDLE;  // 1x1 R32F dynamic exposure (storage, GENERAL)
        VkSampler   nearestSampler = VK_NULL_HANDLE;   // the inputs' texelFetch sampler
        // The renderer's render-target generation (LLCleanRendererVK::targetGen()): bumped
        // whenever any size-dependent target is re-created. The sets are rewritten when it
        // moves, NOT only when a view handle changes: a re-created target can get the freed
        // view's handle back (snapcrash #2 -- a 700x381 snapshot thumbnail re-created depth /
        // HDR / motion / reactive / refraction and the main view re-created them at the old
        // handles; these sets kept the freed images -> READ_INVALID -> VK_ERROR_DEVICE_LOST).
        uint64_t    targetGen = 0;
        // Output (display size, RGBA16F, STORAGE). VK_NULL_HANDLE: written into this
        // object's own staging image (stagingImage()), which the caller copies -- for
        // native scale, where the colour input IS the post chain's image.
        VkImage     outImage = VK_NULL_HANDLE;
        VkImageView outView = VK_NULL_HANDLE;
        const float* reproj = nullptr;      // 16: unjittered clip -> last frame's clip (camera)
        const float* waterPlane = nullptr;  // 4: water plane, unjittered clip space (0 = none)
        const float* proj = nullptr;        // 16: unjittered projection (column-major)
        const float* invProj = nullptr;     // 16: its inverse
        float    jitterPx[2] = {0.f, 0.f};  // render pixels, x right / y down (sample q shows q + 0.5 - jitter)
        uint32_t jitterPhases = 16;         // the Halton cycle length this frame uses
        bool     motionValid = false;       // the pre-pass wrote the motion target this frame
        float    staticExposure = 1.f;      // RenderExposure
        bool     dynamicExposure = false;   // multiply by the exposure image
        float    reactiveMaskGain = 0.f;    // > 0: mReact.R x gain; 0: reference-copy difference
        float    reactiveDiffGain = 4.f;    // the difference mode's gain (clean_taa.comp params1.y)
        float    reactiveCap = 0.9f;        // FSR guidance: keep reactive below 1
        float    waterTc = 0.5f;            // transparency & composition value on the water
        float    sharpness = 0.f;           // RCAS, 0..1 (RenderCASSharpness); 0 = off
        bool     reset = false;             // camera cut / history invalid
        uint32_t debugView = 0;             // RenderVKFSRDebug: 1 = the SDK's debug viewports
        uint32_t slot = 0, slotCount = 2;   // frame-in-flight slot (constant-buffer ring)
        // RenderVKFSRDebugProbe: after the upscale, probe the colour input, the pre-RCAS
        // internal upscaled colour and the output (nullptr = off).
        LLCleanFSRProbeVK* probe = nullptr;
        float    probeUv[2] = {0.5f, 0.5f};
    };

    LLCleanFSR3VK() = default;
    ~LLCleanFSR3VK() = default;   // destroy() is explicit (needs the device)
    LLCleanFSR3VK(const LLCleanFSR3VK&) = delete;
    LLCleanFSR3VK& operator=(const LLCleanFSR3VK&) = delete;

    // Device capabilities FSR needs (storage formats incl. the extended ones). Logs
    // the reason once when false.
    static bool deviceSupported(VkDeviceContext* ctx);

    // Pipelines, resources (their UNDEFINED -> GENERAL barriers go into cmd) and sets for
    // this frame. false = FSR can't run (nothing that matters was recorded): the caller
    // resolves with its TAA instead. Call before the caller's own resolve barriers.
    bool prepare(VkCommandBuffer cmd, const Frame& f);
    // Records the whole upscale (after a successful prepare() with the same Frame). The
    // output (outView, or the staging image) then holds this frame's upscaled scene:
    // GENERAL, last written by a COMPUTE storage write. sharpened: RCAS ran (the caller's
    // final-pass CAS stands down).
    void record(VkCommandBuffer cmd, const Frame& f, bool* sharpened);

    VkImage stagingImage() const { return mStage.image; }
    void destroy(VkDevice dev);

private:
    enum Pass { P_INPUTS, P_PREPARE_INPUTS, P_LUMA_PYRAMID, P_SHADING_PYRAMID, P_SHADING_CHANGE,
                P_PREPARE_REACTIVITY, P_LUMA_INSTABILITY, P_ACCUMULATE, P_RCAS, P_DEBUG, P_COUNT };

    bool ensurePipelines(VkDeviceContext* ctx);
    bool ensureResources(VkCommandBuffer cmd, const Frame& f);   // true = usable
    void destroyResources();
    bool ensureCb(uint32_t slotCount);
    void writeSets(const Frame& f, VkImageView outView);
    void clearImage(VkCommandBuffer cmd, VkImage img, const float rgba[4], uint32_t mips = 1);
    void barrier(VkCommandBuffer cmd, bool fromClear = false);
    void dispatch(VkCommandBuffer cmd, Pass p, VkPipeline pipe, uint32_t parity, uint32_t gx, uint32_t gy);

    VkDeviceContext* mCtx = nullptr;
    VkSampler mPointClamp = VK_NULL_HANDLE, mLinearClamp = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl[P_COUNT] = {};
    VkPipelineLayout mLayout[P_COUNT] = {};
    VkDescPool mPool;
    VkDescriptorSet mSet[P_COUNT][2] = {};   // [pass][frame parity]
    VkPipeline mPipe[P_COUNT] = {};
    VkPipeline mAccumulateSharpen = VK_NULL_HANDLE;
    bool mWaveOps = false;   // SPD passes use subgroup quad operations

    // Constant buffers (host-visible, coherent): per frame slot {FSR, SPD, RCAS, inputs},
    // bound as dynamic UBOs (dynamic offset = slot * kCbSlotBytes).
    static constexpr uint32_t kCbFsr = 0, kCbSpd = 256, kCbRcas = 512, kCbInputs = 768;
    static constexpr uint32_t kCbSlotBytes = 1024;
    GpuBuffer mCb;
    uint32_t  mCbSlots = 0;
    uint32_t  mCurSlotOffset = 0;   // this record's dynamic offset

    // Resources (render size unless noted). [0] / [1] = the SDK's *_1 / *_2 ping-pong.
    GpuImage mInMotion, mInDepth, mInReactive, mInTc, mInExposure;     // INPUTS pass outputs
    GpuImage mDilMotion, mDilDepth, mReconDepth, mIntermediate;        // FP16x1: farthest depth, then luma instability
    GpuImage mLuma[2], mLumaHist[2], mAccum[2];
    GpuImage mSpdAtomic, mFrameInfo;                                   // 1x1
    GpuImage mSpdMips;                                                 // half render size, full chain
    VkImageView mSpdMipView[6] = {};
    uint32_t mSpdMipCount = 0;
    GpuImage mFarthestMip1, mShading;                                  // half render size
    GpuImage mDilReactive;
    GpuImage mNewLocks;                                                // display size
    GpuImage mUpscaled[2];                                             // display size
    GpuImage mStage;                                                   // display size (native output)
    uint32_t mRw = 0, mRh = 0, mDw = 0, mDh = 0;
    bool     mFirst = true;          // resources fresh: clear the histories on the first record
    VkImageView mBound[8] = {};      // the external views the sets were written for
    VkSampler   mBoundSampler = VK_NULL_HANDLE;
    uint64_t    mBoundGen = 0;       // Frame::targetGen the sets were written with (0 = never)

    // Per-frame state (fsr3upscalerDispatch's context constants).
    uint32_t mFrameIndex = 0;        // resourceFrameIndex parity source
    float    mFrameCount = 0.f;      // constants.frameIndex
    float    mJitterPhaseCount = 0.f;
    float    mPrevJitter[2] = {};
    int32_t  mPrevRender[2] = {}, mPrevDisplay[2] = {};
    std::chrono::steady_clock::time_point mLastTime{};
    bool     mHaveTime = false;
};

// RenderVKFSRDebugProbe: a numeric HDR readback (clean_fsr_probe.comp) for the FSR / TAA
// brightness comparison. Each record() reads up to three images (probe pixel, exposure,
// whole-image outlier counts, 64x64-grid mean) into this frame slot's host-visible region;
// collect() logs what the slot's previous use recorded (its fence has been waited), at
// most about once a second per probe point, and zeroes the region for the next use.
class LLCleanFSRProbeVK
{
public:
    // PT_BLOOM: after the bloom pyramid -- its composited mip 0 and the per-face glow the
    // extract read (snapshot vs live wash-out triage).
    enum Point { PT_FSR = 0, PT_POST = 1, PT_BLOOM = 2, PT_COUNT };
    struct Images
    {
        VkImageView   view[3] = {};
        VkImageLayout layout[3] = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        uint32_t      w[3] = {}, h[3] = {};
        const char*   name[3] = {"", "", ""};
        uint32_t      count = 1;            // 1..3 (the unused slots are bound to image 0)
    };
    LLCleanFSRProbeVK() = default;
    ~LLCleanFSRProbeVK() = default;   // destroy() is explicit (needs the device)
    LLCleanFSRProbeVK(const LLCleanFSRProbeVK&) = delete;
    LLCleanFSRProbeVK& operator=(const LLCleanFSRProbeVK&) = delete;

    // Once per frame, before any record() into this slot (the slot's fence waited).
    void collect(uint32_t slot);
    // Every image: sampled, its last write already made visible to compute (the probe
    // adds a conservative barrier anyway). exposureView: the 1x1 R32F exposure image
    // (GENERAL); the reported exposure = staticExposure x (dynamicExposure ? its value : 1).
    void record(VkDeviceContext* ctx, VkCommandBuffer cmd, Point pt, uint32_t slot, uint32_t slotCount,
                const Images& im, VkImageView exposureView, float staticExposure, bool dynamicExposure,
                const float uv[2], const char* label);
    // Records made while this is set (offscreen snapshot frames) are logged whatever the
    // once-a-second throttle says: a snapshot is two frames, the throttle would drop them.
    void setForceLog(bool on) { mForceLog = on; }
    void destroy(VkDevice dev);

    static constexpr uint32_t kRegionWords = 32;   // clean_fsr_probe.comp's region layout

private:
    bool ensure(VkDeviceContext* ctx, uint32_t slotCount);

    static constexpr uint32_t kMaxSlots = 4;
    VkDeviceContext* mCtx = nullptr;
    VkSampler mSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    VkDescPool mPool;
    VkDescriptorSet mSet[PT_COUNT][kMaxSlots] = {};
    GpuBuffer mBuf[kMaxSlots];                        // host-visible: PT_COUNT regions each
    uint32_t mSlots = 0;
    bool mFailed = false;
    struct Meta   // what a slot's region was recorded with (for the log line)
    {
        bool     recorded = false;
        bool     force = false;      // offscreen (snapshot) record: always logged
        char     label[64] = {};
        char     name[3][16] = {};
        uint32_t count = 0;
        uint32_t px[3][2] = {};
    };
    Meta mMeta[kMaxSlots][PT_COUNT];
    std::chrono::steady_clock::time_point mLastLog[PT_COUNT]{};
    bool mForceLog = false;
};

#endif
