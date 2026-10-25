/**
 * @file llcleanpartlowresvk.h
 * @brief Reduced-resolution particle pass (RenderVKParticleLowRes) for the Vulkan clean
 *        renderer: off-screen half-resolution particles + depth-aware composite.
 *
 * Standard practice for heavy particle overdraw (GPU Gems 3 ch. 23 "off-screen
 * particles"; the half-res transparency of most engines): the sprites are rasterised
 * into half-resolution targets -- a quarter of the fragments and blends -- and the
 * result is upsampled into the scene once.
 *
 *   prepare()       after pass 1 (scene depth DEPTH_READ_ONLY_OPTIMAL), outside any
 *                   rendering scope: (re)creates the targets and writes the half-res
 *                   depth, the farthest of each 2x2 (clean_part_lowres.frag PART_LR_DEPTH).
 *   beginScope()    a rendering scope on the half-res targets: colour cleared to
 *                   (0, 0, 0, 1) -- C = 0, transmittance T = 1 --, glow and reactive to 0,
 *                   the half-res depth read-only. The renderer draws the PART_PIPE_LOWRES
 *                   pipelines here: each SL blend pair's own colour factors, the ALPHA
 *                   channel accumulating T (srcAlpha ZERO, dstAlpha = the pair's dst
 *                   factor), so any sequence of the pairs it takes (dst' = dst * f + g, f a
 *                   scalar of the source) folds into dst * T + C exactly.
 *   endScope()      ends it and releases the targets to the composite's sampling.
 *   recordComposite() inside a pass-2 scope: dst * T + C (ONE / SRC_ALPHA), glow added,
 *                   reactive MAX, with nearest-depth upsampling at depth edges.
 *
 * Used once per water side per frame (pass 2a pre-water, pass 2b post-water). Not for
 * the mirror capture or offscreen snapshots (full resolution there). The modulate /
 * destination-driven pairs stay full resolution, drawn right after the composite.
 */
#ifndef LL_LLCLEANPARTLOWRESVK_H
#define LL_LLCLEANPARTLOWRESVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <cstdint>

class LLCleanPartLowResVK
{
public:
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        VkFormat glowFormat = VK_FORMAT_UNDEFINED;    // pass 2 attachment formats (composite)
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkImageView depthView = VK_NULL_HANDLE;       // scene depth, DEPTH_READ_ONLY_OPTIMAL
        uint64_t    depthGen = 0;                     // bumped when the depth image is re-created
        uint32_t rw = 0, rh = 0;                      // render extent
        bool reverseZ = true;
        float depthTolerance = 0.05f;                 // relative depth agreement (composite)
    };

    // The colour format of the accumulator (rgb C, a T): the LOWRES pipelines target it.
    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    LLCleanPartLowResVK() = default;
    ~LLCleanPartLowResVK() = default;   // destroy() is explicit (needs the device)
    LLCleanPartLowResVK(const LLCleanPartLowResVK&) = delete;
    LLCleanPartLowResVK& operator=(const LLCleanPartLowResVK&) = delete;

    bool prepare(VkCommandBuffer cmd, const Frame& f);
    void beginScope(VkCommandBuffer cmd);
    void endScope(VkCommandBuffer cmd);
    void recordComposite(VkCommandBuffer cmd);
    void destroy(VkDevice dev);
    uint32_t width() const { return mW; }
    uint32_t height() const { return mH; }

private:
    bool ensurePipelines(const Frame& f);
    void ensureTargets(const Frame& f);
    void destroyTargets();
    void writeSet();

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mSampler = VK_NULL_HANDLE;   // nearest / clamp (texelFetch only)
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;       // 0 scene depth, 1 half depth, 2 colour, 3 glow, 4 reactive
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;
    VkPipeline            mDepthPipe = VK_NULL_HANDLE;
    VkPipeline            mCompPipe = VK_NULL_HANDLE;
    VkFormat              mFormats[4] = {};            // hdr, glow, react, depth the pipelines were built for

    GpuImage    mColor, mGlow, mReact, mDepth;
    uint32_t    mW = 0, mH = 0, mFullW = 0, mFullH = 0;
    VkImageView mBoundDepthView = VK_NULL_HANDLE;
    uint64_t    mBoundDepthGen = 0;

    struct Push { int32_t dims[4]; float params[4]; } mPush{};
};

#endif
