/**
 * @file llcleandlssrrvk.cpp
 * @brief #870 DLSS Ray Reconstruction + #962 DLSS Super Resolution host side: see llcleandlssrrvk.h.
 */
#include "linden_common.h"

#include "llcleandlssrrvk.h"
#include "vk_pipeline.h"
#include "vk_fsr3_shaders_spv.h"   // kFsrInputsSpv, kRrGuidesSpv, kRrUnexposeSpv, kRrDebugSpv, kSrColorSpv

#include "llerror.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
// clean_fsr_inputs.comp FsrInputs (std140, 144 B): the same block LLCleanFSR3VK fills.
struct InputsParams
{
    float reproj[16];
    float rscreen[4];
    float jitter[4];
    float wplane[4];
    float p0[4];
    float p1[4];
};
static_assert(sizeof(InputsParams) == 144, "InputsParams must match clean_fsr_inputs.comp");

// clean_rr_guides.comp GuideParams (std140, 176 B).
struct GuideParams
{
    float    clipToRel[16];
    float    relToClip[16];
    float    size[4];
    uint32_t flags[4];
    float    march[4];
};
static_assert(sizeof(GuideParams) == 176, "GuideParams must match clean_rr_guides.comp");
static_assert(sizeof(InputsParams) <= 256 && sizeof(GuideParams) <= 256, "constant-buffer sub-slots are 256 B");

constexpr VkDescriptorType S  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType U  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType C  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
constexpr VkDescriptorType CI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
constexpr VkDescriptorType B  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

struct Bind { uint32_t binding; VkDescriptorType type; };
struct PassDesc { const char* name; Bind binds[24]; uint32_t count; uint32_t push; };
const PassDesc kPasses[] = {
    // clean_fsr_inputs.comp (exactly LLCleanFSR3VK's P_INPUTS set)
    {"rrInputs",   {{0,CI},{1,CI},{2,CI},{3,CI},{4,CI},{5,U},{6,U},{7,U},{8,U},{9,U},{10,U},{11,C},{12,CI}}, 13, 0},
    {"rrGuides",   {{0,S},{1,S},{2,S},{3,S},{4,CI},{5,U},{6,B},{7,U},{8,U},{9,U},{10,U},{11,C},{12,CI},{13,U},{14,U},
                    {15,CI},{16,CI},{17,U},{18,U},{19,CI}}, 20, 0},
    {"rrUnexpose", {{0,U},{1,U}}, 2, 16},
    {"rrDebug",    {{0,U},{1,U},{2,U},{3,U},{4,U},{5,U},{6,U},{7,U},{8,U},{9,U}}, 10, 32},
    // clean_sr_color.comp (#962: Super Resolution's colour + bias mask)
    {"srColor",    {{0,CI},{1,CI},{2,CI},{3,U},{4,U},{5,U}}, 6, 32},
};
static_assert(sizeof(kPasses) / sizeof(kPasses[0]) == 5, "one PassDesc per LLCleanDlssRRVK::Pass");

VkShaderModule rrModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

// Column-major M -> row-major (the network's matrices: row-major, left multiplication).
void toRowMajor(const float* colMajor, float* rowMajor)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            rowMajor[r * 4 + c] = colMajor[c * 4 + r];
}

LLVKNeuralRecon::Image nrImage(const GpuImage& im, VkFormat fmt, uint32_t w, uint32_t h)
{
    LLVKNeuralRecon::Image o;
    o.image = im.image;
    o.view = im.view;
    o.format = fmt;
    o.w = w;
    o.h = h;
    o.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    return o;
}
} // namespace

bool LLCleanDlssRRVK::Targets::operator==(const Targets& o) const
{
    return depth == o.depth && motion == o.motion && react == o.react && noTemporal == o.noTemporal
        && scene == o.scene && ref == o.ref && exposure == o.exposure && out == o.out
        && std::memcmp(gbuf, o.gbuf, sizeof(gbuf)) == 0 && rtTrace == o.rtTrace && rtTiles == o.rtTiles
        && nearest == o.nearest && targetGen == o.targetGen && rtEpoch == o.rtEpoch;
}

bool LLCleanDlssRRVK::ready(const Want& w) const
{
    return mBuiltValid && mFeature && mPipe[P_INPUTS] && mBuilt.kind == w.kind && mBuilt.rw == w.rw && mBuilt.rh == w.rh
        && mBuilt.dw == w.dw && mBuilt.dh == w.dh && mBuilt.q == w.q && mBuilt.outImage == w.outImage
        && mBuilt.outFormat == w.outFormat && mBuilt.t == w.t && w.t.targetGen != 0;
}

bool LLCleanDlssRRVK::ensurePipelines(VkDeviceContext* ctx)
{
    if (mPipe[P_INPUTS] && mPipe[P_GUIDES] && mPipe[P_UNEXPOSE] && mPipe[P_DEBUG] && mPipe[P_SRCOLOR]) return true;
    if (mDsl[0]) return false;   // (a previous attempt failed half way: release() first)
    mCtx = ctx;
    VkDevice dev = ctx->device();
    std::vector<VkdPoolUse> uses;
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        const PassDesc& pd = kPasses[p];
        VkDescriptorSetLayoutBinding b[24]{};
        for (uint32_t i = 0; i < pd.count; ++i)
        {
            b[i].binding = pd.binds[i].binding; b[i].descriptorType = pd.binds[i].type;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = pd.count; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl[p], pd.name));
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pd.push};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl[p];
        if (pd.push) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr; }
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout[p], pd.name));
        uses.push_back({mDsl[p], 1});
    }
    VK_CHECK(mPool.create(dev, "dlssrr", uses));
    for (uint32_t p = 0; p < P_COUNT; ++p)
        VKD_ALLOC_SOFT(mPool.allocate(mDsl[p], &mSet[p]), { return false; });
    auto make = [&](Pass p, const uint32_t* code, size_t bytes) -> VkPipeline
    {
        VkShaderModule m = rrModule(dev, code, bytes);
        VkPipeline pipe = createComputePipeline(dev, m, mLayout[p]);
        vkDestroyShaderModule(dev, m, nullptr);
        return pipe;
    };
    mPipe[P_INPUTS]   = make(P_INPUTS, kFsrInputsSpv, kFsrInputsSpv_bytes);
    mPipe[P_GUIDES]   = make(P_GUIDES, kRrGuidesSpv, kRrGuidesSpv_bytes);
    mPipe[P_UNEXPOSE] = make(P_UNEXPOSE, kRrUnexposeSpv, kRrUnexposeSpv_bytes);
    mPipe[P_DEBUG]    = make(P_DEBUG, kRrDebugSpv, kRrDebugSpv_bytes);
    mPipe[P_SRCOLOR]  = make(P_SRCOLOR, kSrColorSpv, kSrColorSpv_bytes);
    for (uint32_t p = 0; p < P_COUNT; ++p)
        if (!mPipe[p]) { LL_WARNS("Vulkan") << "DLSS RR: pipeline " << kPasses[p].name << " failed" << LL_ENDL; return false; }
    return true;
}

bool LLCleanDlssRRVK::ensureCb(uint32_t slotCount)
{
    VkMemScope memcat(VKMEM_UPSCALER);   // #407
    slotCount = std::max(slotCount, 1u);
    if (mCb.buffer && mCbSlots >= slotCount) return true;
    if (mCb.buffer) mCtx->destroyBuffer(mCb);   // (callers waited for the device)
    mCb = mCtx->createBuffer((VkDeviceSize)kCbSlotBytes * slotCount, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mCbSlots = mCb.buffer ? slotCount : 0;
    mBuiltValid = false;   // the sets name the buffer
    return mCb.buffer && mCb.mapped;
}

void LLCleanDlssRRVK::destroyResources()
{
    if (!mCtx) return;
    for (GpuImage* im : {&mInMotion, &mInDepth, &mInReactive, &mInTc, &mInExposure, &mGuideDiff, &mGuideSpec,
                         &mGuideNormal, &mGuideHit, &mColor, &mTransp, &mResp, &mRtTracePh})
        if (im->image) { mCtx->destroyImage(*im); *im = GpuImage{}; }
    if (mRtTilesPh.buffer) { mCtx->destroyBuffer(mRtTilesPh); mRtTilesPh = GpuBuffer{}; }
    mResW = mResH = 0;
    mBuiltValid = false;
}

bool LLCleanDlssRRVK::ensureResources(const Want& w)
{
    VkMemScope memcat(VKMEM_UPSCALER);   // #407
    if (mColor.image && mResW == w.rw && mResH == w.rh && mResKind == w.kind) return true;
    destroyResources();
    const bool rr = w.kind == LLVKNeuralRecon::F_RR;
    VkDeviceContext::SoftAllocScope soft(mCtx);   // #697: a refusal = RR off for now, never a crash
    // SAMPLED: the network reads them as sampled images; STORAGE: our passes write them.
    const VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = true;
    std::vector<VkImage> fresh;
    auto mk = [&](GpuImage& im, uint32_t iw, uint32_t ih, VkFormat fmt, const char* name)
    {
        im = mCtx->createImage2D(std::max(iw, 1u), std::max(ih, 1u), fmt, use, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!im.image) { ok = false; return; }
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)im.image, name);
        fresh.push_back(im.image);
    };
    mk(mInMotion,    w.rw, w.rh, VK_FORMAT_R16G16_SFLOAT,       "rrMotion");
    mk(mInDepth,     w.rw, w.rh, VK_FORMAT_R32_SFLOAT,          "rrDepth");
    mk(mInReactive,  w.rw, w.rh, VK_FORMAT_R8_UNORM,            "rrInputsReactive");   // (written, unused)
    mk(mInTc,        w.rw, w.rh, VK_FORMAT_R8_UNORM,            "rrInputsTc");         // (written, unused)
    mk(mInExposure,  1, 1,       VK_FORMAT_R32_SFLOAT,          "rrExposure");
    mk(mColor,       w.rw, w.rh, VK_FORMAT_R16G16B16A16_SFLOAT, rr ? "rrColorPreExposed" : "srColor");
    mk(mResp,        w.rw, w.rh, VK_FORMAT_R16_SFLOAT,          rr ? "rrResponsivity" : "srBiasCurrentColor");
    if (rr)
    {
        // (the guides, the overlay and the RT placeholders: Ray Reconstruction only)
        mk(mGuideDiff,   w.rw, w.rh, VK_FORMAT_R8G8B8A8_UNORM,      "rrGuideDiffuseAlbedo");
        mk(mGuideSpec,   w.rw, w.rh, VK_FORMAT_R16G16B16A16_SFLOAT, "rrGuideSpecularAlbedo");
        mk(mGuideNormal, w.rw, w.rh, VK_FORMAT_R16G16B16A16_SFLOAT, "rrGuideNormalRoughness");
        mk(mGuideHit,    w.rw, w.rh, VK_FORMAT_R16_SFLOAT,          "rrGuideSpecHitDistance");
        mk(mTransp,      w.rw, w.rh, VK_FORMAT_R16G16B16A16_SFLOAT, "rrTransparencyOverlay");
        mk(mRtTracePh,   1, 1,       VK_FORMAT_R16G16B16A16_SFLOAT, "rrRtTracePlaceholder");
    }
    if (ok && rr)
    {
        mRtTilesPh = mCtx->createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!mRtTilesPh.buffer) ok = false;
    }
    if (!ok)
    {
        LL_WARNS("Vulkan") << "DLSS " << LLVKNeuralRecon::kindName(w.kind) << ": target allocation failed at "
                           << w.rw << "x" << w.rh << LL_ENDL;
        destroyResources();
        return false;
    }
    // Everything GENERAL for good; the placeholders zero.
    mCtx->submitImmediate([&](VkCommandBuffer c)
    {
        std::vector<VkImageMemoryBarrier2> b(fresh.size());
        for (size_t i = 0; i < fresh.size(); ++i)
        {
            b[i] = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                               | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image = fresh[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = (uint32_t)b.size(); dep.pImageMemoryBarriers = b.data();
        vkCmdPipelineBarrier2(c, &dep);
        if (rr)
        {
            const VkClearColorValue z{};
            const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(c, mRtTracePh.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            vkCmdFillBuffer(c, mRtTilesPh.buffer, 0, VK_WHOLE_SIZE, 0u);
        }
    });
    mResW = w.rw; mResH = w.rh; mResKind = w.kind;
    LL_INFOS("Vulkan") << "DLSS " << LLVKNeuralRecon::kindName(w.kind) << ": targets " << w.rw << "x" << w.rh
                       << " -> " << w.dw << "x" << w.dh << LL_ENDL;
    return true;
}

void LLCleanDlssRRVK::writeSets(const Want& w)
{
    std::vector<VkWriteDescriptorSet> wr;
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<VkDescriptorBufferInfo> bi;
    ii.reserve(64); bi.reserve(8);   // stable addresses
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        DRO = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    auto img = [&](Pass p, uint32_t binding, VkImageView v, VkImageLayout l, VkDescriptorType t, VkSampler s = VK_NULL_HANDLE)
    {
        ii.push_back({s, v, l});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = mSet[p]; x.dstBinding = binding; x.descriptorCount = 1;
        x.descriptorType = t; x.pImageInfo = &ii.back();
        wr.push_back(x);
    };
    auto buf = [&](Pass p, uint32_t binding, VkBuffer b, VkDeviceSize off, VkDeviceSize range, VkDescriptorType t)
    {
        bi.push_back({b, off, range});
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = mSet[p]; x.dstBinding = binding; x.descriptorCount = 1;
        x.descriptorType = t; x.pBufferInfo = &bi.back();
        wr.push_back(x);
    };
    const Targets& t = w.t;
    // inputs (clean_fsr_inputs.comp)
    img(P_INPUTS, 0, t.depth, DRO, CI, t.nearest);
    img(P_INPUTS, 1, t.motion, RO, CI, t.nearest);
    img(P_INPUTS, 2, t.react, RO, CI, t.nearest);
    img(P_INPUTS, 3, t.scene, RO, CI, t.nearest);
    img(P_INPUTS, 4, t.ref, RO, CI, t.nearest);
    img(P_INPUTS, 12, t.noTemporal, RO, CI, t.nearest);
    img(P_INPUTS, 5, t.exposure, G, U);
    img(P_INPUTS, 6, mInMotion.view, G, U);
    img(P_INPUTS, 7, mInDepth.view, G, U);
    img(P_INPUTS, 8, mInReactive.view, G, U);
    img(P_INPUTS, 9, mInTc.view, G, U);
    img(P_INPUTS, 10, mInExposure.view, G, U);
    buf(P_INPUTS, 11, mCb.buffer, kCbInputs, 256, C);
    // unexpose
    img(P_UNEXPOSE, 0, t.out, G, U);
    img(P_UNEXPOSE, 1, mInExposure.view, G, U);
    if (w.kind == LLVKNeuralRecon::F_SR)
    {
        // Super Resolution (#962): the colour copy + bias mask only (the guide / debug sets are
        // not bound in this mode; their targets are not allocated)
        img(P_SRCOLOR, 0, t.scene, RO, CI, t.nearest);
        img(P_SRCOLOR, 1, t.noTemporal, RO, CI, t.nearest);
        img(P_SRCOLOR, 2, t.react, RO, CI, t.nearest);
        img(P_SRCOLOR, 3, mInExposure.view, G, U);
        img(P_SRCOLOR, 4, mColor.view, G, U);
        img(P_SRCOLOR, 5, mResp.view, G, U);
        vkUpdateDescriptorSets(mCtx->device(), (uint32_t)wr.size(), wr.data(), 0, nullptr);
        return;
    }
    // guides
    for (uint32_t i = 0; i < 4; ++i) img(P_GUIDES, i, t.gbuf[i], RO, S);
    img(P_GUIDES, 4, t.depth, DRO, CI, t.nearest);
    img(P_GUIDES, 5, t.rtTrace ? t.rtTrace : mRtTracePh.view, G, U);
    buf(P_GUIDES, 6, t.rtTiles ? t.rtTiles : mRtTilesPh.buffer, 0, VK_WHOLE_SIZE, B);
    img(P_GUIDES, 7, mGuideDiff.view, G, U);
    img(P_GUIDES, 8, mGuideSpec.view, G, U);
    img(P_GUIDES, 9, mGuideNormal.view, G, U);
    img(P_GUIDES, 10, mGuideHit.view, G, U);
    buf(P_GUIDES, 11, mCb.buffer, kCbGuides, 256, C);
    img(P_GUIDES, 12, t.scene, RO, CI, t.nearest);
    img(P_GUIDES, 13, mInExposure.view, G, U);
    img(P_GUIDES, 14, mColor.view, G, U);
    img(P_GUIDES, 15, t.ref, RO, CI, t.nearest);
    img(P_GUIDES, 16, t.react, RO, CI, t.nearest);
    img(P_GUIDES, 17, mTransp.view, G, U);
    img(P_GUIDES, 18, mResp.view, G, U);
    img(P_GUIDES, 19, t.noTemporal, RO, CI, t.nearest);
    // debug view (RenderVKDlssDebug)
    img(P_DEBUG, 0, t.out, G, U);
    img(P_DEBUG, 1, mColor.view, G, U);
    img(P_DEBUG, 2, mGuideDiff.view, G, U);
    img(P_DEBUG, 3, mGuideSpec.view, G, U);
    img(P_DEBUG, 4, mGuideNormal.view, G, U);
    img(P_DEBUG, 5, mGuideHit.view, G, U);
    img(P_DEBUG, 6, mInMotion.view, G, U);
    img(P_DEBUG, 7, mTransp.view, G, U);
    img(P_DEBUG, 8, mResp.view, G, U);
    img(P_DEBUG, 9, mInDepth.view, G, U);
    vkUpdateDescriptorSets(mCtx->device(), (uint32_t)wr.size(), wr.data(), 0, nullptr);
}

bool LLCleanDlssRRVK::prestep(VkDeviceContext* ctx, const Want& w, uint32_t slotCount, std::string& err)
{
    if (ready(w) && mCbSlots >= std::max(slotCount, 1u)) return true;
    const bool rr = w.kind == LLVKNeuralRecon::F_RR;
    if (!ctx || !w.rw || !w.rh || !w.dw || !w.dh || !w.outImage || !w.t.out || !w.t.depth || !w.t.motion
        || !w.t.react || !w.t.noTemporal || !w.t.scene || !w.t.ref || !w.t.exposure || !w.t.nearest
        || (rr && (!w.t.gbuf[0] || !w.t.gbuf[1] || !w.t.gbuf[2] || !w.t.gbuf[3])))
    {
        err = "a renderer target is missing";
        return false;
    }
    LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
    if (!nr.available(w.kind)) { err = nr.reason(w.kind); return false; }
    mCtx = ctx;
    vkDeviceWaitIdle(ctx->device());   // nothing in flight names the sets / targets / feature below
    if (!ensurePipelines(ctx)) { err = "pipeline creation failed"; return false; }
    if (!ensureCb(slotCount)) { err = "constant buffer allocation failed"; return false; }
    if (!ensureResources(w)) { err = "target allocation failed"; return false; }
    writeSets(w);
    // The network feature: per network, render / display extent and mode. (A network switch,
    // RR <-> SR, lands here like an extent change: outside any recording, after the wait above.)
    const bool featureOk = mFeature && mBuilt.kind == w.kind && mBuilt.rw == w.rw && mBuilt.rh == w.rh
                        && mBuilt.dw == w.dw && mBuilt.dh == w.dh && mBuilt.q == w.q;
    if (!featureOk)
    {
        if (mFeature) { nr.destroy(mFeature); mFeature = nullptr; }
        uint32_t flags = LLVKNeuralRecon::kHdr | LLVKNeuralRecon::kDepthInverted | LLVKNeuralRecon::kMvRenderRes;
        if (rr) flags |= LLVKNeuralRecon::kRoughnessPacked | LLVKNeuralRecon::kHwDepth;
        std::string why;
        LLVKNeuralRecon::Feature* f = nullptr;
        ctx->submitImmediate([&](VkCommandBuffer c)
        {
            VkMemScope memcat(VKMEM_UPSCALER);
            f = nr.create(c, w.kind, w.rw, w.rh, w.dw, w.dh, w.q, flags, why);
        });
        if (!f)
        {
            err = "feature creation failed: " + why;
            LL_WARNS("Vulkan") << "DLSS " << LLVKNeuralRecon::kindName(w.kind) << ": " << err << " (" << w.rw << "x"
                               << w.rh << " -> " << w.dw << "x" << w.dh << ", " << LLVKNeuralRecon::qualityName(w.q)
                               << ")" << LL_ENDL;
            mBuiltValid = false;
            return false;
        }
        mFeature = f;
        mFirstEval = true;   // (the new network's first evaluate resets its history: once)
        LL_INFOS("Vulkan") << "DLSS " << LLVKNeuralRecon::kindName(w.kind) << ": feature created "
                           << LLVKNeuralRecon::qualityName(w.q) << " " << w.rw << "x" << w.rh << " -> " << w.dw << "x"
                           << w.dh << LL_ENDL;
    }
    mBuilt = w;
    mBuiltValid = true;
    return true;
}

void LLCleanDlssRRVK::release()
{
    if (!holding() || !mCtx) return;
    vkDeviceWaitIdle(mCtx->device());
    if (mFeature) { LLVKNeuralRecon::instance().destroy(mFeature); mFeature = nullptr; }
    destroyResources();
    if (mCb.buffer) { mCtx->destroyBuffer(mCb); mCb = GpuBuffer{}; mCbSlots = 0; }
    VkDevice dev = mCtx->device();
    for (uint32_t p = 0; p < P_COUNT; ++p)
    {
        if (mPipe[p]) { vkDestroyPipeline(dev, mPipe[p], nullptr); mPipe[p] = VK_NULL_HANDLE; }
        if (mLayout[p]) { vkDestroyPipelineLayout(dev, mLayout[p], nullptr); mLayout[p] = VK_NULL_HANDLE; }
        mSet[p] = VK_NULL_HANDLE;
    }
    mPool.destroy();
    for (uint32_t p = 0; p < P_COUNT; ++p)
        if (mDsl[p]) { vkdDestroySetLayout(dev, mDsl[p]); mDsl[p] = VK_NULL_HANDLE; }
    mBuiltValid = false;
    mHaveTime = false;
    LL_INFOS("Vulkan") << "DLSS: released" << LL_ENDL;
}

void LLCleanDlssRRVK::destroy()
{
    release();
}

void LLCleanDlssRRVK::computeBarrier(VkCommandBuffer cmd)
{
    // (ALL_COMMANDS: the network's own passes run as compute but may use other stages)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                    | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
                    | VK_ACCESS_2_UNIFORM_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool LLCleanDlssRRVK::record(VkCommandBuffer cmd, const Frame& f, std::string& err)
{
    const Want& w = f.w;
    mOutGeneral = false;
    if (!ready(w) || !mCb.mapped || !f.reproj || !f.view || !f.proj || !f.viewProjRel)
    {
        err = "not ready";
        return false;
    }
    const uint32_t slotOff = (f.slot % std::max(mCbSlots, 1u)) * kCbSlotBytes;

    // ---- constants ----
    InputsParams ip{};
    std::memcpy(ip.reproj, f.reproj, sizeof(ip.reproj));
    ip.rscreen[0] = (float)w.rw; ip.rscreen[1] = (float)w.rh;
    ip.rscreen[2] = 1.f / (float)w.rw; ip.rscreen[3] = 1.f / (float)w.rh;
    ip.jitter[0] = f.jitterPx[0]; ip.jitter[1] = f.jitterPx[1];
    ip.jitter[2] = f.jitterPx[0] / (float)w.rw; ip.jitter[3] = f.jitterPx[1] / (float)w.rh;
    if (f.waterPlane) std::memcpy(ip.wplane, f.waterPlane, sizeof(ip.wplane));
    ip.p0[0] = f.motionValid ? 1.f : 0.f;
    ip.p0[1] = f.staticExposure;
    ip.p0[2] = f.dynamicExposure ? 1.f : 0.f;
    ip.p0[3] = 0.f;
    ip.p1[0] = 0.5f; ip.p1[1] = 0.9f; ip.p1[2] = 4.f;

    GuideParams gp{};
    {
        float vp[16];
        std::memcpy(vp, f.viewProjRel, sizeof(vp));
        // (jitterClip: the main view's rasterization was jittered the same way)
        for (int c = 0; c < 4; ++c) { vp[c * 4 + 0] += f.jitterNdc[0] * vp[c * 4 + 3]; vp[c * 4 + 1] += f.jitterNdc[1] * vp[c * 4 + 3]; }
        const glm::mat4 m = glm::make_mat4(vp);
        const glm::mat4 inv = glm::inverse(m);
        std::memcpy(gp.relToClip, vp, sizeof(vp));
        std::memcpy(gp.clipToRel, glm::value_ptr(inv), sizeof(gp.clipToRel));
    }
    gp.size[0] = (float)w.rw; gp.size[1] = (float)w.rh; gp.size[2] = 1.f / (float)w.rw; gp.size[3] = 1.f / (float)w.rh;
    const bool rt = f.rtValid && w.t.rtTrace && w.t.rtTiles && f.rtTileCount;
    gp.flags[0] = rt ? 1u : 0u;
    gp.flags[1] = rt ? f.rtTilesPerRow : 0u;
    gp.flags[2] = rt ? f.rtTileCount : 0u;
    gp.flags[3] = rt ? f.rtHdrWords : 0u;
    gp.march[0] = f.marchDist; gp.march[1] = (float)f.marchSteps; gp.march[2] = 0.02f;
    gp.march[3] = f.responsivity;
    if (f.overlay) gp.flags[0] |= 2u;

    uint8_t* base = (uint8_t*)mCb.mapped + slotOff;
    std::memcpy(base + kCbInputs, &ip, sizeof(ip));
    std::memcpy(base + kCbGuides, &gp, sizeof(gp));

    // Last frame's network reads / our writes of these targets -> this frame's writes.
    computeBarrier(cmd);

    const bool sr = w.kind == LLVKNeuralRecon::F_SR;
    const uint32_t gx = (w.rw + 7) / 8, gy = (w.rh + 7) / 8;
    uint32_t dyn = slotOff;
    vkBreadcrumb(cmd, "rrInputs");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_INPUTS]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[P_INPUTS], 0, 1, &mSet[P_INPUTS], 1, &dyn);
    vkCmdDispatch(cmd, gx, gy, 1);
    computeBarrier(cmd);
    if (sr)
    {
        // Super Resolution (#962): the colour copy + bias mask; no guides.
        vkBreadcrumb(cmd, "srColor");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_SRCOLOR]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[P_SRCOLOR], 0, 1, &mSet[P_SRCOLOR], 0, nullptr);
        struct { uint32_t size[4]; float p[4]; } spc{{w.rw, w.rh, f.srPreExpose ? 1u : 0u, 0u},
                                                     {std::max(f.responsivity, 0.f), 0.f, 0.f, 0.f}};
        static_assert(sizeof(spc) == 32, "clean_sr_color.comp push block");
        vkCmdPushConstants(cmd, mLayout[P_SRCOLOR], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    else
    {
        vkBreadcrumb(cmd, "rrGuides");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_GUIDES]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[P_GUIDES], 0, 1, &mSet[P_GUIDES], 1, &dyn);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    computeBarrier(cmd);
    if (f.outIsScene)
    {
        // Render = display: the output is the scene image the two passes above sampled. Their
        // reads done -> the network's storage writes (old contents discarded).
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = w.outImage; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        mOutGeneral = true;
    }

    // ---- the network ----
    LLVKNeuralRecon::Eval e;
    e.color          = nrImage(mColor, VK_FORMAT_R16G16B16A16_SFLOAT, w.rw, w.rh);
    e.depth          = nrImage(mInDepth, VK_FORMAT_R32_SFLOAT, w.rw, w.rh);
    e.motion         = nrImage(mInMotion, VK_FORMAT_R16G16_SFLOAT, w.rw, w.rh);
    if (sr)
    {
        // (unexposed colour: the network applies the resolved exposure itself)
        if (!f.srPreExpose) e.exposure = nrImage(mInExposure, VK_FORMAT_R32_SFLOAT, 1, 1);
    }
    else
    {
        e.diffuseAlbedo  = nrImage(mGuideDiff, VK_FORMAT_R8G8B8A8_UNORM, w.rw, w.rh);
        e.specularAlbedo = nrImage(mGuideSpec, VK_FORMAT_R16G16B16A16_SFLOAT, w.rw, w.rh);
        e.normals        = nrImage(mGuideNormal, VK_FORMAT_R16G16B16A16_SFLOAT, w.rw, w.rh);
        e.specHitDist    = nrImage(mGuideHit, VK_FORMAT_R16_SFLOAT, w.rw, w.rh);
    }
    e.output.image = w.outImage; e.output.view = w.t.out; e.output.format = w.outFormat;
    e.output.w = w.dw; e.output.h = w.dh; e.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    e.jitter[0] = f.jitterPx[0] * f.jitterSign;
    e.jitter[1] = f.jitterPx[1] * f.jitterSign;
    // The inputs pass writes FSR's convention (prev_uv - uv, UV units): x render size = the
    // network's pixels, current -> previous.
    e.mvScale[0] = (float)w.rw;
    e.mvScale[1] = (float)w.rh;
    e.rw = w.rw; e.rh = w.rh;
    e.reset = f.reset || mFirstEval;
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = 16.6f;
        if (mHaveTime) dt = std::chrono::duration<float, std::milli>(now - mLastTime).count();
        mLastTime = now; mHaveTime = true;
        e.frameTimeMs = std::clamp(dt, 0.1f, 1000.f);
    }
    toRowMajor(f.view, e.worldToView);
    toRowMajor(f.proj, e.viewToClip);
    if (f.overlay && !sr) e.transparency = nrImage(mTransp, VK_FORMAT_R16G16B16A16_SFLOAT, w.rw, w.rh);
    if (sr ? f.responsivity > 0.f : f.responsivity != 0.f)
        e.responsivity = nrImage(mResp, VK_FORMAT_R16_SFLOAT, w.rw, w.rh);
    if (f.debug && !sr)
    {
        // RenderVKDlssDebug: one input on screen instead of the network (not evaluated: its
        // history restarts when the view is turned off).
        vkBreadcrumb(cmd, "rrDebug");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_DEBUG]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[P_DEBUG], 0, 1, &mSet[P_DEBUG], 0, nullptr);
        const uint32_t dpc[8] = {w.dw, w.dh, w.rw, w.rh, f.debug, 0u, 0u, 0u};
        vkCmdPushConstants(cmd, mLayout[P_DEBUG], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dpc), dpc);
        vkCmdDispatch(cmd, (w.dw + 7) / 8, (w.dh + 7) / 8, 1);
        mFirstEval = true;
    }
    else
    {
        vkBreadcrumb(cmd, "rrEvaluate");
        if (!LLVKNeuralRecon::instance().evaluate(cmd, mFeature, e, err)) return false;
        mFirstEval = false;
    }
    computeBarrier(cmd);
    if (sr && !f.srPreExpose) return true;   // (SR's output is already in the scene's units)

    // ---- unexpose (display size, in place) ----
    vkBreadcrumb(cmd, "rrUnexpose");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_UNEXPOSE]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout[P_UNEXPOSE], 0, 1, &mSet[P_UNEXPOSE], 0, nullptr);
    const uint32_t pc[4] = {w.dw, w.dh, 0u, 0u};
    vkCmdPushConstants(cmd, mLayout[P_UNEXPOSE], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, (w.dw + 7) / 8, (w.dh + 7) / 8, 1);
    return true;
}
