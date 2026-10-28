/**
 * @file llcleandenoisevk.cpp
 * @brief Diffuse / specular lighting denoiser (#85 phase 2): see llcleandenoisevk.h and
 *        indra/vkclean/shaders/clean_pdn_common.glsl. The renderer side (the projector
 *        stochastic sampler, its settings, the set 3 bindings) is llcleanprojstochvk.cpp.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleandenoisevk.h"

#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>

namespace
{
VkShaderModule dnModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);   // (names the batch report / straggler lines)
    return m;
}

// clean_pdn_common.glsl IMAGES: IN rgba16f[2], MOT rgba32ui, GEOM rgba32ui[2], HIST rgba32ui[2],
// OUT rgba16f[2] -- 112 B per texel. With the sun / moon channel (the merged RT shadow pass):
// IN / OUT one more layer (2), HIST two more (2 + parity) -- 160 B per texel. (#869) With the SPLIT
// channel (clean_pdn_common.glsl SPLIT CHANNEL): GEOM three more (2 + parity, 4 the final), IN / OUT
// the sun layer (allocated with it either way, so the split layer is always the last) + one more
// -- +64 B per texel with the sun channel, +80 B without it.
struct DnImg { VkFormat f; uint32_t layers; uint32_t sunLayers; uint32_t splitLayers; };
const DnImg kDnImg[5] = {{VK_FORMAT_R16G16B16A16_SFLOAT, 2u, 3u, 4u}, {VK_FORMAT_R32G32B32A32_UINT, 1u, 1u, 1u},
                         {VK_FORMAT_R32G32B32A32_UINT, 2u, 2u, 5u}, {VK_FORMAT_R32G32B32A32_UINT, 2u, 4u, 2u},
                         {VK_FORMAT_R16G16B16A16_SFLOAT, 2u, 3u, 4u}};
// The layers of image e: the base, the sun channel's, the split channel's (each a superset of the
// base; HIST's sun layers only with the sun channel).
uint32_t dnLayers(const DnImg& e, bool sun, bool split)
{
    uint32_t n = sun ? e.sunLayers : e.layers;
    if (split) n = std::max(n, e.splitLayers);
    return n;
}
constexpr VkImageUsageFlags kDnUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
} // namespace

bool LLCleanDenoiserVK::init(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkSampler nearest, const char* name)
{
    if (!ctx || !dslDraw0 || !nearest) return false;
    mCtx = ctx;
    mName = name ? name : "denoiser";
    mNearest = nearest;
    VkDevice dev = mCtx->device();
    // Set 1: 0 IN, 1 MOT, 2 GEOM, 3 HIST, 4 OUT (storage images), 5 the motion vectors (sampled).
    VkDescriptorSetLayoutBinding b[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6; li.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &li, &mDsl, "LLCleanDenoiserVK::mDsl"));
    VK_CHECK(mPool.create(dev, "diffuse / specular denoiser", {{mDsl, 1}}));
    VK_CHECK(mPool.allocate(mDsl, &mDset));
    const VkDescriptorSetLayout sl[2] = {dslDraw0, mDsl};
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = sl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "LLCleanDenoiserVK::mLayout"));
    // Placeholders (hard: tiny) so the set -- and the callers' own bindings of these views --
    // are always valid.
    {
        VkMemScope memcat(VKMEM_DENOISE);
        if (!createSet(mIn, mMot, mGeom, mHist, mOut, 1u, 1u, false, false, false)) return false;
        mMotionPh = mCtx->createImage2D(1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!mMotionPh.image) return false;
    }
    mW = mH = 1u;
    clearImages();
    writeSet();
    // The two passes: queued into the caller's pipeline batch when one is open.
    VkShaderModule tm = dnModule(dev, kWorldDnTemporalSpv, kWorldDnTemporalSpv_bytes, "DnTemporal");
    createComputePipelineInto(dev, tm, mLayout, 0u, nullptr, &mTempPipe, "LLCleanDenoiserVK temporal");
    destroyShaderModuleDeferred(dev, tm, nullptr);
    VkShaderModule am = dnModule(dev, kWorldDnAtrousSpv, kWorldDnAtrousSpv_bytes, "DnAtrous");
    createComputePipelineInto(dev, am, mLayout, 0u, nullptr, &mAtrousPipe, "LLCleanDenoiserVK a-trous");
    destroyShaderModuleDeferred(dev, am, nullptr);
    return true;
}

void LLCleanDenoiserVK::destroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    vkDeviceWaitIdle(dev);
    destroyImages();
    if (mMotionPh.image) { mCtx->destroyImage(mMotionPh); mMotionPh = GpuImage{}; }
    if (mTempPipe) vkDestroyPipeline(dev, mTempPipe, nullptr);
    if (mAtrousPipe) vkDestroyPipeline(dev, mAtrousPipe, nullptr);
    mTempPipe = mAtrousPipe = VK_NULL_HANDLE;
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mLayout = VK_NULL_HANDLE;
    mPool.destroy();
    mDset = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    mCtx = nullptr;
}

uint64_t LLCleanDenoiserVK::bytesAt(uint32_t w, uint32_t h, bool sun, bool split) const
{
    if (!mCtx) return 0;
    uint64_t b = 0;
    for (const DnImg& e : kDnImg) b += mCtx->imageMemoryBytes(w, h, dnLayers(e, sun, split), 1, e.f, kDnUsage, false);
    return b;
}

bool LLCleanDenoiserVK::createSet(GpuImage& in, GpuImage& mot, GpuImage& geom, GpuImage& hist, GpuImage& out,
                                  uint32_t w, uint32_t h, bool soft, bool sun, bool split)
{
    VkDeviceContext::SoftAllocScope scope(mCtx, soft);
    GpuImage* im[5] = {&in, &mot, &geom, &hist, &out};
    for (int i = 0; i < 5; ++i)
    {
        const DnImg& e = kDnImg[i];
        const uint32_t layers = dnLayers(e, sun, split);
        *im[i] = layers > 1u ? mCtx->createImageLayers(w, h, layers, 1, e.f, kDnUsage, VK_IMAGE_ASPECT_COLOR_BIT, false,
                                                        VK_IMAGE_VIEW_TYPE_2D_ARRAY)
                             : mCtx->createImage2D(w, h, e.f, kDnUsage, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!im[i]->image) return false;
    }
    static const char* const kNames[5] = {"denoiseIn", "denoiseMot", "denoiseGeom", "denoiseHist", "denoiseOut"};
    for (int i = 0; i < 5; ++i) mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)im[i]->image, kNames[i]);
    return true;
}

void LLCleanDenoiserVK::destroyImages()
{
    for (GpuImage* im : {&mIn, &mMot, &mGeom, &mHist, &mOut})
        if (im->image) { mCtx->destroyImage(*im); *im = GpuImage{}; }
}

// Every image -> GENERAL, zeroed (GEOM depth 0 = no surface: a fresh history validates nothing).
void LLCleanDenoiserVK::clearImages()
{
    mCtx->submitImmediate([&](VkCommandBuffer c) {
        GpuImage* im[6] = {&mIn, &mMot, &mGeom, &mHist, &mOut, &mMotionPh};
        VkImageMemoryBarrier2 b[6]{};
        uint32_t n = 0;
        for (GpuImage* g : im)
        {
            if (!g->image) continue;
            VkImageMemoryBarrier2& x = b[n++];
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            x.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            x.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; x.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            x.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; x.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            x.image = g->image;
            x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(c, &d);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        for (uint32_t i = 0; i < n; ++i) vkCmdClearColorImage(c, b[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rr);
        for (uint32_t i = 0; i < n; ++i)
        {
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; b[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier2(c, &d);
    });
}

void LLCleanDenoiserVK::writeSet()
{
    if (!mDset) return;
    const VkImageView sv[5] = {mIn.view, mMot.view, mGeom.view, mHist.view, mOut.view};
    VkDescriptorImageInfo ii[6]{};
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        if (i < 5) ii[i] = {VK_NULL_HANDLE, sv[i], VK_IMAGE_LAYOUT_GENERAL};
        else if (mMotionView) ii[i] = {mMotionSampler ? mMotionSampler : mNearest, mMotionView, mMotionLayout};
        else ii[i] = {mNearest, mMotionPh.view, VK_IMAGE_LAYOUT_GENERAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDset; w[i].dstBinding = i;
        w[i].descriptorCount = 1; w[i].pImageInfo = &ii[i];
        w[i].descriptorType = i == 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    vkUpdateDescriptorSets(mCtx->device(), 6, w, 0, nullptr);
}

void LLCleanDenoiserVK::setMotion(VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    mMotionView = view;
    mMotionSampler = sampler;
    mMotionLayout = layout;
    writeSet();
}

bool LLCleanDenoiserVK::ensure(uint32_t w, uint32_t h, bool sun, bool split)
{
    if (!mCtx || !mDset) return false;
    w = std::max(w, 1u); h = std::max(h, 1u);
    if (allocated() && w == mW && h == mH && sun == mSun && split == mSplit) return true;
    vkDeviceWaitIdle(mCtx->device());   // the set and the callers' bindings may be in flight
    destroyImages();
    bool ok;
    {
        VkMemScope memcat(VKMEM_DENOISE);
        ok = createSet(mIn, mMot, mGeom, mHist, mOut, w, h, true, sun, split);
        if (!ok)
        {
            // #697: never a crash -- the placeholders back, the caller keeps the exact path.
            destroyImages();
            LL_WARNS("Vulkan") << mName << ": " << w << "x" << h << (sun ? " (+ sun / moon channel)" : "")
                               << (split ? " (+ split channel)" : "")
                               << " refused (VRAM, ~" << (bytesAt(w, h, sun, split) >> 20) << " MB); placeholders" << LL_ENDL;
            createSet(mIn, mMot, mGeom, mHist, mOut, 1u, 1u, false, false, false);
            w = h = 1u;
            sun = false;
            split = false;
        }
    }
    mW = w; mH = h;
    mSun = sun;
    mSplit = split;
    mHistOk = mRan = false;
    mSunHistOk = mSunRan = false;
    clearImages();
    writeSet();
    if (ok)
        LL_INFOS("Vulkan") << mName << ": " << w << "x" << h << (mSun ? " + sun / moon channel" : "")
                           << (mSplit ? " + split channel" : "") << ", "
                           << (bytes() >> 20) << " MB (" << bytes() / ((uint64_t)w * h) << " B per texel)" << LL_ENDL;
    return ok;
}

void LLCleanDenoiserVK::release()
{
    if (!mCtx || !allocated()) return;
    vkDeviceWaitIdle(mCtx->device());
    destroyImages();
    {
        VkMemScope memcat(VKMEM_DENOISE);
        createSet(mIn, mMot, mGeom, mHist, mOut, 1u, 1u, false, false, false);
    }
    mW = mH = 1u;
    mSun = false;
    mSplit = false;
    mHistOk = mRan = false;
    mSunHistOk = mSunRan = false;
    clearImages();
    writeSet();
    LL_INFOS("Vulkan") << mName << ": released" << LL_ENDL;
}

void LLCleanDenoiserVK::beginFrame()
{
    mHistOk = mRan && allocated();
    mRan = false;
    // The sun channel's history: the channel was accumulated last frame as well (a frame
    // whose producer skipped it -- merge off, the sun not traced -- leaves nothing to reproject).
    mSunHistOk = mSunRan && mHistOk && hasSun();
    mSunRan = false;
    mParity ^= 1u;
}

LLCleanDenoiserVK::Push LLCleanDenoiserVK::makePush(uint32_t rw, uint32_t rh, const Params& p) const
{
    Push q{};
    q.dims[0] = (float)mW; q.dims[1] = (float)mH; q.dims[2] = (float)rw; q.dims[3] = (float)rh;
    q.prm0[0] = std::clamp(p.maxHistDiffuse, 1.f, 63.f);
    q.prm0[1] = std::clamp(p.maxHistSpecular, 1.f, 63.f);
    q.prm0[2] = std::clamp(p.clampGamma, 0.25f, 16.f);
    q.prm0[3] = std::clamp(p.lumPhi, 0.1f, 64.f);
    q.prm1[0] = std::clamp(p.depthPhi, 0.05f, 16.f);
    q.prm1[1] = std::clamp(p.normalPow, 1.f, 512.f);
    q.prm1[2] = std::clamp(p.wCut, 0.f, 4.f);
    q.prm1[3] = std::clamp(p.minHistSpecular, 1.f, q.prm0[1]);
    // clean_dn_common.glsl DN_F_*: bit 0 history, bit 1 parity, bits 4-7 the debug view; the
    // sun / moon channel (only with the images carrying it): bit 8 on, bit 9 its history is
    // valid, bit 10 hard sun, bit 11 exact reprojection; bit 12 the no-information rule (DN_F_INFO);
    // bits 13-15 the ReSTIR-converged diffuse's a-trous iterations (DN_CONV_SHIFT); bits 16-19 the
    // moving-light history (DN_MOVE_SHIFT, 0 = off); bits 27-29 the disocclusion history fix
    // (DN_HFIX_SHIFT), bit 30 the reprojection search (DN_F_REPROJ_SEARCH); (#1038) bit 2 the young-texel
    // boost (DN_F_YOUNG_BOOST), bit 31 the moving-surface depth tolerance (DN_F_MOVE_ZTOL).
    const bool sun = p.sun && hasSun();
    q.ctl[0] = (mHistOk ? 1u : 0u) | (mParity ? 2u : 0u) | (p.youngBoost ? 4u : 0u) | (p.moveZTol ? (1u << 31) : 0u)
             | ((std::min(p.debug, 15u)) << 4u)
             | (sun ? 256u : 0u) | (sun && mSunHistOk && !p.sunCut ? 512u : 0u) | (sun && p.sunHard ? 1024u : 0u)
             | (sun && p.sunExact ? 2048u : 0u) | (p.info ? 4096u : 0u)
             | (std::min(p.convIters, 7u) << 13u)    // (#869 / #85: DN_CONV_SHIFT)
             | (std::min(p.moveHist, 15u) << 16u)    // (#869: DN_MOVE_SHIFT)
             | (p.raw ? (1u << 20) : 0u) | (p.rawSun ? (1u << 21) : 0u)    // (#870: DN_F_RAW / DN_F_RAW_SUN)
             | ((uint32_t)std::clamp(p.surfMoveTexels * 4.f + 0.5f, 0.f, 15.f) << 22u)   // (#869: DN_SURF_MOVE_SHIFT)
             | (p.moveDiffuseOnly ? (1u << 26) : 0u)    // (DN_F_MOVE_DIFF_ONLY)
             | (std::min(p.histFix, 7u) << 27u)         // (#87: DN_HFIX_SHIFT)
             | (p.reprojSearch ? (1u << 30) : 0u);      // (#87: DN_F_REPROJ_SEARCH)
    q.ctl[1] = 0u;
    q.ctl[2] = clampIters(p.atrous);
    q.ctl[3] = p.frame;
    q.sun0[0] = std::clamp(p.sunMaxHist, 1.f, 63.f);
    q.sun0[1] = std::clamp(p.sunDynHist, 1.f, 15.f);
    q.sun0[2] = std::clamp(p.sunDynMax, 0.f, 15.f);
    q.sun0[3] = std::clamp(p.sunMove, 0.f, 64.f);
    q.sun1[0] = std::clamp(p.sunSpatialCap, 0.f, 16.f);
    q.sun1[1] = std::clamp(p.sunClamp, 0.5f, 8.f);
    q.sun1[2] = (float)std::min(p.sunPersist, 7u);
    q.sun1[3] = p.sunRays > 0.f ? std::min(p.sunRays, 64.f) : 0.f;   // (0 = the frame info's sun_rt.y)
    return q;
}

static void dnComputeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanDenoiserVK::recordTemporal(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, uint32_t rw, uint32_t rh,
                                       const Params& p)
{
    if (!pipelinesReady() || !dsetDraw0) return;
    dnComputeBarrier(cmd);   // the producer's IN / MOT / GEOM writes
    const VkDescriptorSet sets[2] = {dsetDraw0, mDset};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, sets, 0, nullptr);
    const Push q = makePush(rw, rh, p);
    VK_PUSH_BUDGET_CHECK(q);
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTempPipe);
    vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1);
    mRan = true;
    mSunRan = p.sun && hasSun();
}

void LLCleanDenoiserVK::recordSpatial(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, const Params& p)
{
    if (!pipelinesReady() || !mRan) return;
    // (the sets are still bound from recordTemporal: same layout, nothing bound in between)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAtrousPipe);
    Push q = makePush(rw, rh, p);
    const uint32_t n = clampIters(p.atrous);
    for (uint32_t k = 0; k < n; ++k)
    {
        dnComputeBarrier(cmd);   // the previous pass's writes -> this iteration's neighbourhood reads
        q.ctl[1] = k;
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
        vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1);
    }
}
