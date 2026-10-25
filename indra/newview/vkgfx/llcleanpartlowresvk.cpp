/**
 * @file llcleanpartlowresvk.cpp
 * @brief Reduced-resolution particle pass (RenderVKParticleLowRes): see
 *        llcleanpartlowresvk.h and clean_part_lowres.frag.
 */
#include "linden_common.h"

#include "llcleanpartlowresvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <cstring>

namespace
{
VkShaderModule lrModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.image = img; b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

bool LLCleanPartLowResVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    const bool fmtChanged = mCompPipe && (mFormats[0] != f.hdrFormat || mFormats[1] != f.glowFormat
                                          || mFormats[2] != f.reactFormat || mFormats[3] != f.depthFormat);
    if (mDepthPipe && mCompPipe && !fmtChanged) return true;
    if (mDepthPipe || mCompPipe || mDsl) destroy(dev);   // a pass format change (rare): rebuild
    mCtx = f.ctx;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));

    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 5; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslPartLowRes"));
    VK_CHECK(mPool.create(dev, "partlowres", {{mDsl, 1}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset), { destroy(dev); return false; });

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mPartLowResLayout"));

    VkShaderModule vs  = lrModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes);   // fullscreen triangle
    VkShaderModule dfs = lrModule(dev, kWorldPartLowResDepthFragSpv, kWorldPartLowResDepthFragSpv_bytes);
    VkShaderModule cfs = lrModule(dev, kWorldPartLowResCompFragSpv, kWorldPartLowResCompFragSpv_bytes);
    {
        // Depth downsample: depth-only, every texel written (compare ALWAYS).
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = dfs; d.layout = mLayout;
        d.noColor = true;
        d.depthFormat = f.depthFormat; d.depthTest = true; d.depthWrite = true;
        d.depthCompare = VK_COMPARE_OP_ALWAYS;
        d.cullMode = VK_CULL_MODE_NONE;
        // Immediate (not createGraphicsPipelineInto): built from recordWorld on first use.
        mDepthPipe = createGraphicsPipeline(dev, d);
    }
    {
        // Composite into pass 2: dst * T + C on the HDR scene (alpha kept), glow added,
        // reactive coverage MAX (R only: G is the water mask).
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = cfs; d.layout = mLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowAdditive = true;
        d.reactFormat = f.reactFormat; d.reactWriteMask = VK_COLOR_COMPONENT_R_BIT;
        d.depthFormat = f.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.customBlend = true; d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        mCompPipe = createGraphicsPipeline(dev, d);
    }
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, dfs, nullptr);
    vkDestroyShaderModule(dev, cfs, nullptr);
    mFormats[0] = f.hdrFormat; mFormats[1] = f.glowFormat; mFormats[2] = f.reactFormat; mFormats[3] = f.depthFormat;
    if (!mDepthPipe || !mCompPipe)
    {
        LL_WARNS("VKParticles") << "reduced-resolution particle pipelines failed to build; full resolution" << LL_ENDL;
        destroy(dev);
        return false;
    }
    return true;
}

void LLCleanPartLowResVK::destroyTargets()
{
    if (!mCtx) return;
    for (GpuImage* im : {&mColor, &mGlow, &mReact, &mDepth})
        if (im->image) { mCtx->destroyImage(*im); *im = GpuImage{}; }
    mW = mH = 0;
}

void LLCleanPartLowResVK::ensureTargets(const Frame& f)
{
    VkMemScope memcat(VKMEM_TARGET);   // #407
    const uint32_t w = std::max(1u, (f.rw + 1u) / 2u), h = std::max(1u, (f.rh + 1u) / 2u);
    const bool resize = !mColor.image || w != mW || h != mH;
    if (!resize && mBoundDepthView == f.depthView && mBoundDepthGen == f.depthGen) return;
    vkDeviceWaitIdle(f.ctx->device());   // the set / images may be in use by frames in flight
    if (resize)
    {
        destroyTargets();
        const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        mColor = mCtx->createImage2D(w, h, kColorFormat, cu, VK_IMAGE_ASPECT_COLOR_BIT);
        mGlow  = mCtx->createImage2D(w, h, f.glowFormat, cu, VK_IMAGE_ASPECT_COLOR_BIT);
        mReact = mCtx->createImage2D(w, h, f.reactFormat, cu, VK_IMAGE_ASPECT_COLOR_BIT);
        mDepth = mCtx->createImage2D(w, h, f.depthFormat,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mColor.image, "partLowResColor");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mGlow.image, "partLowResGlow");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mReact.image, "partLowResReact");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mDepth.image, "partLowResDepth");
        mW = w; mH = h;
        // No initial transition: every use enters from UNDEFINED (contents discarded) and
        // reaches the layout the set names before anything samples it -- the depth via
        // prepare() (DEPTH_READ_ONLY_OPTIMAL), the colour targets via endScope()
        // (SHADER_READ_ONLY_OPTIMAL). The depth pass binds the set but reads binding 0 only.
    }
    mFullW = f.rw; mFullH = f.rh;
    mBoundDepthView = f.depthView;
    mBoundDepthGen = f.depthGen;
    writeSet();
}

void LLCleanPartLowResVK::writeSet()
{
    if (!mCtx || !mDset || !mColor.image || !mBoundDepthView) return;
    const VkDescriptorImageInfo ii[5] = {
        {mSampler, mBoundDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {mSampler, mDepth.view,     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {mSampler, mColor.view,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {mSampler, mGlow.view,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {mSampler, mReact.view,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[5]{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = mDset; wr[i].dstBinding = i;
        wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 5, wr, 0, nullptr);
}

bool LLCleanPartLowResVK::prepare(VkCommandBuffer cmd, const Frame& f)
{
    if (!f.ctx || !f.depthView || f.rw < 2 || f.rh < 2) return false;
    if (!ensurePipelines(f)) return false;
    ensureTargets(f);
    if (!mColor.image || !mGlow.image || !mReact.image || !mDepth.image) return false;

    mPush.dims[0] = (int32_t)f.rw; mPush.dims[1] = (int32_t)f.rh;
    mPush.dims[2] = (int32_t)mW;   mPush.dims[3] = (int32_t)mH;
    mPush.params[0] = std::max(f.depthTolerance, 1e-4f);
    mPush.params[1] = f.reverseZ ? 1.f : 0.f;
    mPush.params[2] = mPush.params[3] = 0.f;

    // Half-res depth: last frame's readers (the particle raster's depth test, the
    // composite's sampling) -> this write (discarding).
    imageBarrier(cmd, mDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                 | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDepthPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mDset, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(mPush);
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mPush), &mPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // -> the particle raster's read-only depth test + the composite's sampling.
    imageBarrier(cmd, mDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                 | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    return true;
}

void LLCleanPartLowResVK::beginScope(VkCommandBuffer cmd)
{
    // The previous composite's sampling (this frame's other water side, or last frame)
    // -> this round's cleared writes (contents discarded).
    VkImageMemoryBarrier2 b[3]{};
    const VkImage imgs[3] = {mColor.image, mGlow.image, mReact.image};
    for (uint32_t i = 0; i < 3; ++i)
    {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b[i].srcAccessMask = 0;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b[i].image = imgs[i]; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 3; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);

    VkRenderingAttachmentInfo ca[3]{};
    const VkImageView views[3] = {mColor.view, mGlow.view, mReact.view};
    for (uint32_t i = 0; i < 3; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = views[i]; ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue.color = {{0.f, 0.f, 0.f, i == 0 ? 1.f : 0.f}};   // C = 0, T = 1
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;   // read-only (also sampled)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 3; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
}

void LLCleanPartLowResVK::endScope(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier2 b[3]{};
    const VkImage imgs[3] = {mColor.image, mGlow.image, mReact.image};
    for (uint32_t i = 0; i < 3; ++i)
    {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].image = imgs[i]; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 3; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanPartLowResVK::recordComposite(VkCommandBuffer cmd)
{
    if (!mCompPipe || !mDset) return;
    // Plain bind (no dynamic cull / depth state); viewport + scissor come from the pass.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mDset, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(mPush);
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mPush), &mPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanPartLowResVK::destroy(VkDevice dev)
{
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    destroyTargets();
    if (mDepthPipe) vkDestroyPipeline(dev, mDepthPipe, nullptr);
    if (mCompPipe) vkDestroyPipeline(dev, mCompPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mDepthPipe = mCompPipe = VK_NULL_HANDLE;
    mLayout = VK_NULL_HANDLE; mDsl = VK_NULL_HANDLE; mDset = VK_NULL_HANDLE; mSampler = VK_NULL_HANDLE;
    mBoundDepthView = VK_NULL_HANDLE; mBoundDepthGen = 0;
    for (VkFormat& fm : mFormats) fm = VK_FORMAT_UNDEFINED;
}
