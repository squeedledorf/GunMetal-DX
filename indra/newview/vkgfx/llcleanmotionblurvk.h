/**
 * @file llcleanmotionblurvk.h
 * @brief Motion blur (#118, RenderVKMotionBlur) for the Vulkan clean renderer: a
 *        tile-based reconstruction filter (McGuire et al. 2012 with the CoD: Advanced
 *        Warfare / Guertin 2014 refinements) on the render-size HDR scene.
 *
 * WHERE IT RUNS: right after pass 2 (the whole linear HDR scene, transparents
 * included), at the RENDER resolution, BEFORE the TAA / FSR resolve, the snapshot
 * accumulation, the SSR copy and the scaled upscale blit. Reasons:
 *  - the gather's tap jitter is noise; with a temporal resolve after it (TAA or FSR, the
 *    owner's FSR config included) the jitter rotates per frame and the resolve
 *    integrates it -- no grain. Without one (no TAA / upscaler, single-frame snapshots)
 *    the jitter is a fixed per-pixel pattern: a stable fine dither, nothing crawls;
 *  - the motion vectors, depth and water mask are render-sized and unjittered: the same
 *    inputs the resolve uses, no display-size depth upscale needed;
 *  - TAA / FSR then see the blurred scene as their current frame. Their neighbourhood
 *    clamp is built from it too (a blurred streak is inside its own box), so the
 *    history follows the blur instead of fighting it; the cost is a short settle (~the
 *    history window, ~10 frames of TAA) when motion stops abruptly. The reactive mask
 *    (RenderVKReactiveMask, default on) is pass 2's coverage, untouched. With it OFF,
 *    TAA / FSR diff the final scene against mRefract (taken before the blur): blurred
 *    pixels then read as reactive and resolve with less history (more visible noise).
 *  - SSR's previous-frame copy, DoF, bloom and exposure all see the blurred scene.
 *
 * PASSES (GPU sub-marks p_mblur_tile / p_mblur_nmax / p_mblur):
 *  1. clean_mblur_tile.comp: per pixel, the blur radius vector from the motion target
 *     with the camera rotation / translation / object components scaled separately
 *     (RenderVKMotionBlurCameraRotation / CameraTranslation / Object; the shader header
 *     explains why the total vector is scaled, never rebuilt) and the linear depth, into
 *     an RGBA16F velocity image; reduced per 16x16 tile into TileMax.
 *  2. clean_mblur_neighbor.comp: NeighborMax over (2 reach + 1)^2 tiles, reach =
 *     ceil(max radius / 16), with a reach test past the 3x3.
 *  3. clean_mblur_recon.comp: the gather, into an RGBA16F target, only on tiles whose
 *     NeighborMax is over ~0.5 px (whole workgroups return otherwise).
 *  4. clean_mblur_writeback.frag: a fullscreen draw into the scene that copies exactly
 *     those tiles and discards the rest.
 * Still frame: pass 1 reads the motion / depth / water mask and writes the velocity
 * image (~18 B per render pixel), passes 2-4 are near free (tile-sized work, a
 * discarded fullscreen draw). Moving: + the gather (taps x (RGBA16F + velocity fetch)).
 *
 * Off (RenderVKMotionBlur 0, the default) this object does not exist; supersampled
 * snapshot recordings (#417) render a frozen world and skip it, single-frame snapshots
 * carry the live motion (fixed jitter, no resolve follows). Every allocation is soft:
 * a failure turns the effect off for that frame (retried after ~300 frames).
 */
#ifndef LL_LLCLEANMOTIONBLURVK_H
#define LL_LLCLEANMOTIONBLURVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot (first-use pipelines off the render thread)

#include <cstdint>

class LLCleanMotionBlurVK
{
public:
    // What the effect needs from the renderer this recording.
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        uint32_t rw = 0, rh = 0;                           // render extent (the scene's)
        VkFormat sceneFormat = VK_FORMAT_UNDEFINED;
        // The render-size HDR scene: COLOR_ATTACHMENT_OPTIMAL after pass 2; left so.
        VkImage     sceneImage = VK_NULL_HANDLE;
        VkImageView sceneView = VK_NULL_HANDLE;
        // World depth, DEPTH_READ_ONLY_OPTIMAL (pass 2's); motion target,
        // SHADER_READ_ONLY_OPTIMAL; pass 2's reactive / water mask (G = water),
        // COLOR_ATTACHMENT_OPTIMAL after pass 2 -- sampled here and given back so.
        VkImage     depthImage = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkImageView motionView = VK_NULL_HANDLE;
        VkImage     reactImage = VK_NULL_HANDLE;
        VkImageView reactView = VK_NULL_HANDLE;
        VkImageView exposureView = VK_NULL_HANDLE;        // 1x1 R32F dynamic exposure, GENERAL (debug views)
        // The renderer's target / depth generations (registry rule 1): the set rewrite
        // keys on them, never on the views alone (a freed view's handle can come back).
        uint64_t targetGen = 0, depthGen = 0;
        const float* reprojCam = nullptr;   // this frame's unjittered clip -> last frame's clip (16)
        const float* reprojRot = nullptr;   // the same, last frame's eye on this frame's (16)
        float fovComp = 1.f;                // clip-xy scale removing the speed-FoV change (#663), 1 = none
        const float* waterPlane = nullptr;  // water plane, this frame's unjittered clip space (4)
        const float* lin = nullptr;         // view z from device depth: (x d + y) / (z d + w) (4)
        float frameDt = 1.f / 60.f;         // seconds of motion the motion target holds
        float staticExposure = 1.f;
        bool  dynExposure = false;
        bool  motionValid = false;          // the pre-pass wrote the motion target this frame
        bool  temporalResolve = false;      // TAA / FSR resolves this frame (per-frame jitter)
        bool  offscreen = false;            // snapshot recording (keeps its own noise frame)
        uint32_t snapSample = 0;            // #932: a shot's accumulation sample 1..N (0 = none)
    };

    LLCleanMotionBlurVK() = default;
    ~LLCleanMotionBlurVK() = default;   // destroy() is explicit (needs the device)
    LLCleanMotionBlurVK(const LLCleanMotionBlurVK&) = delete;
    LLCleanMotionBlurVK& operator=(const LLCleanMotionBlurVK&) = delete;

    // Reads the settings, (re)creates what changed, updates the parameters and records
    // pass 1 (velocity + TileMax). Outside any rendering scope. false = nothing to do this
    // frame (shutter 0, an input missing, an allocation failed): skip the rest.
    bool recordTiles(VkCommandBuffer cmd, const Frame& f);
    // After recordTiles returned true: pass 2 (NeighborMax).
    void recordNeighbor(VkCommandBuffer cmd);
    // After recordNeighbor: passes 3 + 4 (reconstruction, write-back into the scene);
    // leaves every renderer image in the layout it came in.
    void recordReconstruct(VkCommandBuffer cmd);
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);
    // Login warm-up (LLCleanRendererVK::warmFirstUse): starts the pipeline build for this
    // scene format, nothing recorded. true while it compiles.
    bool warm(VkDeviceContext* ctx, VkFormat sceneFormat);
    bool pipelinesPending() const { return mJob.pending(); }

private:
    bool ensurePipelines(const Frame& f);
    bool ensureTargets(VkCommandBuffer cmd, const Frame& f);   // false = allocation failed
    void destroyTargets();
    void writeSet();

    // Parameter block (clean_mblur_common.glsl MbParams, std140).
    struct Params
    {
        float reproj_cam[16];
        float reproj_rot[16];
        float wplane[4];
        float lin[4];
        float scales[4];
        float p0[4];
        float p1[4];
        float p2[4];
        float p3[4];
        float p4[4];   // #836: x wide uniform-motion check, y Karis HDR tap weights
    };

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mLinear = VK_NULL_HANDLE;    // scene taps (bilinear, clamp)
    VkSampler             mNearest = VK_NULL_HANDLE;   // texelFetch-only bindings
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mSet = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;
    VkPipeline            mTilePipe = VK_NULL_HANDLE;
    VkPipeline            mNeighborPipe = VK_NULL_HANDLE;
    VkPipeline            mReconPipe = VK_NULL_HANDLE;
    VkPipeline            mWritebackPipe = VK_NULL_HANDLE;
    VkFormat              mWritebackFormat = VK_FORMAT_UNDEFINED;
    bool                  mPipeFailed = false;         // build failed: don't retry every frame
    PipelineJobSlot       mJob;                        // the background build (ensurePipelines)
    VkPipeline            mStage[4] = {};              // its outputs: tile, neighbour, reconstruct, write-back

    GpuBuffer mParams;              // one Params block, vkCmdUpdateBuffer per recording
    GpuImage  mVel;                 // RGBA16F, render size: xy radius px, z linear depth
    GpuImage  mTileMax;             // RGBA16F, one texel per 16 px tile
    GpuImage  mNeighbor;            // RGBA16F, per tile
    GpuImage  mOut;                 // RGBA16F, render size: the reconstruction
    uint32_t  mW = 0, mH = 0, mTilesX = 0, mTilesY = 0;
    uint32_t  mRetryFrame = 0;      // allocation failed: no retry before this frame count
    uint32_t  mFrameNo = 0;         // allocations' retry clock (recordings)

    // What the set was last written for (generations, never handles alone).
    uint64_t    mSetTargetGen = 0, mSetDepthGen = 0, mSetOwnGen = 0;
    uint64_t    mOwnGen = 1;        // bumped whenever an own image / buffer is re-created
    VkImageView mSetViews[6] = {};  // + the renderer views (a cheap second check)
    // The renderer views the set names (writeSet).
    VkImageView mSceneView = VK_NULL_HANDLE, mBindDepth = VK_NULL_HANDLE, mBindMotion = VK_NULL_HANDLE;
    VkImageView mBindReact = VK_NULL_HANDLE, mBindExposure = VK_NULL_HANDLE;

    // This recording (recordTiles -> the later passes).
    bool     mArmed = false;
    VkImage  mSceneImage = VK_NULL_HANDLE, mReactImage = VK_NULL_HANDLE;
    uint32_t mNoiseFrame = 0;       // live-view jitter clock

    uint32_t mLogW = 0, mLogH = 0, mLogReach = 0;
};

#endif
