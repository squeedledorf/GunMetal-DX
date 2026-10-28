/**
 * @file llcleanvolcloudsvk.cpp
 * @brief Volumetric sky cloud layer (#719): see llcleanvolcloudsvk.h and the shaders
 *        clean_vol_cloud_*.
 */
#include "linden_common.h"

#include "llcleanvolcloudsvk.h"
#include "llcleanweathervk.h"   // #783 M2: the lightning flash (LLCleanWeatherVK::flashNow)
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llagent.h"   // <GMDX> the agent frame origin (Params::gmdx)
#include "llerror.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr uint32_t kBaseSize = 128, kDetailSize = 32;   // 3D noises (texels per edge)
constexpr uint32_t kWeatherSize = 512;                   // weather map (texels per tile edge)
constexpr uint32_t kShadowSize = 512;                    // cloud shadow map
constexpr float    kShadowExtent = 20000.f;              // m covered by the shadow map
constexpr uint32_t kShadowEvery = 4;                     // refresh period (frames)
constexpr uint32_t kShadowSteps = 16;
constexpr uint32_t kPanoW = 512, kPanoH = 256;           // panorama (equirect)
constexpr uint32_t kPanoRows = 32;                       // rows per recording (8 frames per refresh)
constexpr uint32_t kPanoSteps = 48;
// EEP mapping (header of clean_vol_cloud_weather.comp): the weather tile is this many metres
// per unit of EEP cloud scale (LL's default 0.42 -> ~10 km), the base noise repeats about
// every kBasePeriod m (snapped to divide the tile, so the wind wrap is seamless), the detail
// noise 16 times per base period.
constexpr double   kTilePerScale = 24000.0;
constexpr double   kBasePeriod = 6000.0;
constexpr float    kExtinction = 0.04f;                  // sigma_t per m at density 1
constexpr double   kCamJump = 64.0;                      // m between frames: history dropped

// Quality tiers (RenderVKVolCloudQuality 1-3): march pattern (2 one texel per 2x2, 1
// checkerboard, 0 all), steps for a full-length segment, light steps, history frames, clamp
// widening (sparser patterns: wider, the box holds fewer samples).
struct Tier { uint32_t pattern, steps, light, hist; float widen; };
constexpr Tier kTiers[4] = {{0, 0, 0, 0, 0.f}, {2, 48, 4, 12, 0.5f}, {1, 64, 5, 16, 0.35f}, {0, 96, 6, 16, 0.15f}};

VkShaderModule vcModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

// Inverse of a column-major 4x4 in double (as the god rays' own).
void invert4(const float* in, float* out)
{
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) m[i] = (double)in[i];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    const double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    const double id = std::fabs(det) > 1e-30 ? 1.0 / det : 0.0;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] * id);
}

VkSampler makeSampler(VkDevice dev, VkFilter filter, VkSamplerAddressMode u, VkSamplerAddressMode vw)
{
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = u;
    sci.addressModeV = sci.addressModeW = vw;
    sci.maxLod = 0.f;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &s));
    return s;
}

VkImageMemoryBarrier2 toGeneral(VkImage img)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src; mb.srcAccessMask = srcA;
    mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

// ---------------------------------------------------------------------------
// creation / teardown
// ---------------------------------------------------------------------------
bool LLCleanVolCloudsVK::create(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkDescriptorSetLayout dslDraw1,
                                VkFormat hdrFormat)
{
    if (!ctx || !dslDraw0 || !dslDraw1) return false;
    if (created()) destroy(ctx->device());
    mCtx = ctx;
    VkDevice dev = ctx->device();
    VkMemScope memcat(VKMEM_TARGET);   // #407

    mSampRepeat  = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    mSampClamp   = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    mSampExport  = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    mSampNearest = makeSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // ---- set layouts ----
    {
        VkDescriptorSetLayoutBinding b[14]{};   // <GMDX> + 13, the cloud shapes
        for (uint32_t i = 0; i < 14; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            const bool sampled = i == 1 || i == 2 || i == 3 || i == 4 || i == 7 || i == 8 || i == 13;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                : (sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 14; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslVolClouds"));
    }
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1; lci.pBindings = &b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mNoiseDsl, "mDslVolCloudsNoise"));
        // <GMDX> the weather map + 1 the params UBO (gmdx_cloud_weather.comp: the tile and the smallest footprint)
        VkDescriptorSetLayoutBinding wb[2] = {b, b};
        wb[1].binding = 1; wb[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lci.bindingCount = 2; lci.pBindings = wb;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mWxDsl, "mDslVolCloudsWeather"));
    }
    {
        VkDescriptorSetLayoutBinding b[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mCompDsl, "mDslVolCloudsComp"));
    }
    VK_CHECK(mPool.create(dev, "volclouds", {{mDsl, 3}, {mNoiseDsl, 2}, {mWxDsl, 1}, {mCompDsl, 3}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mPreSet), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mMainSet[0]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mMainSet[1]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mNoiseDsl, &mNoiseSet[0]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mNoiseDsl, &mNoiseSet[1]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mWxDsl, &mWxSet), { destroy(dev); return false; });
    for (int i = 0; i < 3; ++i)
        VKD_ALLOC_SOFT(mPool.allocate(mCompDsl, &mCompSet[i]), { destroy(dev); return false; });

    // ---- fixed images + the params UBO (soft: a refusal frees everything, the caller retries) ----
    {
        VkDeviceContext::SoftAllocScope soft(ctx);
        const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        mNoiseBase   = ctx->createImage3D(kBaseSize, kBaseSize, kBaseSize, VK_FORMAT_R8G8B8A8_UNORM, su);
        mNoiseDetail = ctx->createImage3D(kDetailSize, kDetailSize, kDetailSize, VK_FORMAT_R8G8B8A8_UNORM, su);
        mWeather = ctx->createImage2D(kWeatherSize, kWeatherSize, VK_FORMAT_R8G8B8A8_UNORM, su, VK_IMAGE_ASPECT_COLOR_BIT);
        mShadow  = ctx->createImage2D(kShadowSize, kShadowSize, VK_FORMAT_R16G16B16A16_SFLOAT,
                                      su | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        mPano    = ctx->createImage2D(kPanoW, kPanoH, VK_FORMAT_R16G16B16A16_SFLOAT,
                                      su | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        mParamBuf = ctx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mNoiseBase.image || !mNoiseDetail.image || !mWeather.image || !mShadow.image || !mPano.image || !mParamBuf.buffer
        || !mShapes.create(ctx))   // <GMDX>
    {
        LL_WARNS("VKVolClouds") << "volumetric clouds: allocation refused (out of video memory); retrying later" << LL_ENDL;
        destroy(dev);
        return false;
    }
    ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mShadow.image, "volCloudShadowMap");
    ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mPano.image, "volCloudPanorama");
    // GENERAL for good; the shadow map starts lit (1) and the panorama empty (T = 1) -- neither
    // is read before the layer is live, but no garbage may ever sit in a sampled image.
    ctx->submitImmediate([&](VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier2 b[5] = {toGeneral(mNoiseBase.image), toGeneral(mNoiseDetail.image), toGeneral(mWeather.image),
                                      toGeneral(mShadow.image), toGeneral(mPano.image)};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 5; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue lit{{1.f, 0.f, 0.f, 1.f}};
        const VkClearColorValue empty{{0.f, 0.f, 0.f, 1.f}};
        vkCmdClearColorImage(cmd, mShadow.image, VK_IMAGE_LAYOUT_GENERAL, &lit, 1, &rng);
        vkCmdClearColorImage(cmd, mPano.image, VK_IMAGE_LAYOUT_GENERAL, &empty, 1, &rng);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    });

    // ---- the sets that never change: pre (shape + shadow + panorama), noise, weather ----
    {
        const VkDescriptorBufferInfo ub{mParamBuf.buffer, 0, sizeof(Params)};
        const VkDescriptorImageInfo ii[6] = {{mSampRepeat, mNoiseBase.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {mSampRepeat, mNoiseDetail.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {mSampRepeat, mWeather.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, mShadow.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, mPano.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[6]{};
        const uint32_t bind[5] = {1, 2, 3, 11, 12};
        for (int i = 0; i < 6; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mPreSet; w[i].descriptorCount = 1; }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        for (int i = 0; i < 5; ++i)
        {
            w[i + 1].dstBinding = bind[i];
            w[i + 1].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i + 1].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(dev, 6, w, 0, nullptr);
        mShapes.write(dev, mPreSet, mSampClamp);   // <GMDX>
        const VkDescriptorImageInfo ni[3] = {{VK_NULL_HANDLE, mNoiseBase.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, mNoiseDetail.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, mWeather.view, VK_IMAGE_LAYOUT_GENERAL}};
        const VkDescriptorSet ns[3] = {mNoiseSet[0], mNoiseSet[1], mWxSet};
        VkWriteDescriptorSet nw[3]{};
        for (int i = 0; i < 3; ++i)
        {
            nw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; nw[i].dstSet = ns[i]; nw[i].dstBinding = 0;
            nw[i].descriptorCount = 1; nw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; nw[i].pImageInfo = &ni[i];
        }
        vkUpdateDescriptorSets(dev, 3, nw, 0, nullptr);
        // <GMDX> the weather pass reads the params too
        VkWriteDescriptorSet wu{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wu.dstSet = mWxSet; wu.dstBinding = 1; wu.descriptorCount = 1;
        wu.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wu.pBufferInfo = &ub;
        vkUpdateDescriptorSets(dev, 1, &wu, 0, nullptr);
    }

    // ---- pipeline layouts (no push constants: everything comes from the params UBO) ----
    {
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mNoiseDsl;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mNoiseLayout, "mVolCloudsNoiseLayout"));
        const VkDescriptorSetLayout wx[3] = {dslDraw0, dslDraw1, mWxDsl};
        pli.setLayoutCount = 3; pli.pSetLayouts = wx;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mWxLayout, "mVolCloudsWeatherLayout"));
        const VkDescriptorSetLayout ms[2] = {dslDraw0, mDsl};
        pli.setLayoutCount = 2; pli.pSetLayouts = ms;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mVolCloudsLayout"));
        pli.setLayoutCount = 1; pli.pSetLayouts = &mCompDsl;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mCompLayout, "mVolCloudsCompLayout"));
    }
    mPipeDslDraw0 = dslDraw0; mPipeDslDraw1 = dslDraw1; mPipeHdrFormat = hdrFormat;

    // ---- pipelines: one background batch; the layer goes live when it lands ----
    {
        PipelineBatchScope batchScope(dev);
        struct C { const uint32_t* spv; size_t bytes; const char* name; VkPipelineLayout layout; int slot; };
        const C cs[7] = {
            {kWorldVolCloudNoiseBaseSpv,   kWorldVolCloudNoiseBaseSpv_bytes,   "kWorldVolCloudNoiseBaseSpv",   mNoiseLayout, P_NOISE_BASE},
            {kWorldVolCloudNoiseDetailSpv, kWorldVolCloudNoiseDetailSpv_bytes, "kWorldVolCloudNoiseDetailSpv", mNoiseLayout, P_NOISE_DETAIL},
            // <GMDX> the weather map, shadow map, panorama and march run GunMetal-DX's cloud model (gmdx_cloud_*.comp)
            {kWorldGmdxCloudWeatherSpv,    kWorldGmdxCloudWeatherSpv_bytes,    "kWorldGmdxCloudWeatherSpv",    mWxLayout,    P_WEATHER},
            {kWorldGmdxCloudShadowSpv,     kWorldGmdxCloudShadowSpv_bytes,     "kWorldGmdxCloudShadowSpv",     mLayout,      P_SHADOW},
            {kWorldGmdxCloudPanoSpv,       kWorldGmdxCloudPanoSpv_bytes,       "kWorldGmdxCloudPanoSpv",       mLayout,      P_PANO},
            {kWorldGmdxCloudMarchSpv,      kWorldGmdxCloudMarchSpv_bytes,      "kWorldGmdxCloudMarchSpv",      mLayout,      P_MARCH},
            {kWorldVolCloudTemporalSpv,    kWorldVolCloudTemporalSpv_bytes,    "kWorldVolCloudTemporalSpv",    mLayout,      P_TEMPORAL}};
        for (const C& c : cs)
        {
            VkShaderModule m = vcModule(dev, c.spv, c.bytes, c.name);
            createComputePipelineInto(dev, m, c.layout, 0, nullptr, &mStage[c.slot], c.name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
        VkShaderModule vs = vcModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = vcModule(dev, kWorldGmdxCloudCompFragSpv, kWorldGmdxCloudCompFragSpv_bytes, "kWorldGmdxCloudCompFragSpv");   // <GMDX>
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mCompLayout;
        d.colorFormat = hdrFormat;
        d.depthFormat = VK_FORMAT_UNDEFINED;
        // scene' = scene x T + in-scatter (premultiplied: alpha holds the transmittance).
        d.customBlend = true;
        d.srcColor = VK_BLEND_FACTOR_ONE;
        d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[P_COMP], "volumetric clouds composite");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
        PipelineJobOpts o;
        o.what = "volumetric clouds";
        o.gate = false;   // optional effect: never holds the login screen
        mJob.start(pipelineBatchFlushAsync(dev, o));
    }
    mNoiseDone = false;
    mShValid = false; mShAge = ~0u;
    mPanoRow = 0; mPanoFilled = 0;
    mHistValid = false;
    LL_INFOS("VKVolClouds") << "volumetric clouds: resources created ("
                            << ((ctx->allocBytes(mNoiseBase) + ctx->allocBytes(mNoiseDetail) + ctx->allocBytes(mWeather)
                                 + ctx->allocBytes(mShadow) + ctx->allocBytes(mPano)) >> 10)
                            << " KB fixed), pipelines compiling in the background" << LL_ENDL;
    return true;
}

bool LLCleanVolCloudsVK::ensurePipelines()
{
    if (mPipeReady) return true;
    if (mPipeFailed || !mCtx) return false;
    if (!mJob.pending()) return false;
    if (!mJob.poll()) return false;   // still compiling
    bool ok = true;
    for (int i = 0; i < P_COUNT; ++i)
    {
        mPipe[i] = mStage[i];
        mStage[i] = VK_NULL_HANDLE;
        ok = ok && mPipe[i];
    }
    if (!ok)
    {
        LL_WARNS("VKVolClouds") << "volumetric clouds pipelines failed to build; the classic cloud layer stays" << LL_ENDL;
        mPipeFailed = true;
        return false;
    }
    LL_INFOS("VKVolClouds") << "volumetric clouds pipelines built (background)" << LL_ENDL;
    mPipeReady = true;
    return true;
}

void LLCleanVolCloudsVK::destroyTargets()
{
    for (GpuImage* im : {&mMarchA, &mMarchB, &mHistA[0], &mHistA[1], &mHistB[0], &mHistB[1]})
    {
        if (im->image && mCtx) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    mW = mH = 0;
    mBoundDepthView = VK_NULL_HANDLE;
    mBoundDepthGen = 0;
    mHistValid = false;
}

void LLCleanVolCloudsVK::destroy(VkDevice dev)
{
    if (!dev) return;
    // A build in flight references the layouts below: wait, free its result.
    mJob.wait();
    for (VkPipeline& p : mStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    vkDeviceWaitIdle(dev);
    for (VkPipeline& p : mPipe) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    destroyTargets();
    mShapes.destroy(mCtx);   // <GMDX>
    for (GpuImage* im : {&mNoiseBase, &mNoiseDetail, &mWeather, &mShadow, &mPano})
    {
        if (im->image && mCtx) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    if (mParamBuf.buffer && mCtx) mCtx->destroyBuffer(mParamBuf);
    mParamBuf = GpuBuffer{};
    for (VkPipelineLayout* l : {&mNoiseLayout, &mWxLayout, &mLayout, &mCompLayout})
    {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    mPool.destroy();
    for (VkDescriptorSetLayout* l : {&mDsl, &mNoiseDsl, &mWxDsl, &mCompDsl})
    {
        if (*l) vkdDestroySetLayout(dev, *l);
        *l = VK_NULL_HANDLE;
    }
    mPreSet = mWxSet = VK_NULL_HANDLE;
    mNoiseSet[0] = mNoiseSet[1] = VK_NULL_HANDLE;
    mMainSet[0] = mMainSet[1] = VK_NULL_HANDLE;
    for (VkDescriptorSet& s : mCompSet) s = VK_NULL_HANDLE;
    for (VkSampler* s : {&mSampRepeat, &mSampClamp, &mSampExport, &mSampNearest})
    {
        if (*s) vkDestroySampler(dev, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    mPipeDslDraw0 = mPipeDslDraw1 = VK_NULL_HANDLE;
    mPipeHdrFormat = VK_FORMAT_UNDEFINED;
    mPipeReady = mPipeFailed = false;
    mLive = mMainNow = mMarchRan = false;
    mNoiseDone = false;
    mShValid = false;
    mPanoFilled = 0;
    mLogQuality = ~0u;
}

// Main-view targets at the march resolution (+ the history pair). Device wait on a resize
// or a new depth view (the sets / images may be in flight); the main / composite sets are
// rewritten before this recording binds them.
bool LLCleanVolCloudsVK::ensureTargets(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    VkMemScope memcat(VKMEM_TARGET);   // #407
    const bool resize = !mMarchA.image || w != mW || h != mH;
    if (!resize) return true;
    vkDeviceWaitIdle(mCtx->device());
    destroyTargets();
    {
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (GpuImage* im : {&mMarchA, &mMarchB, &mHistA[0], &mHistA[1], &mHistB[0], &mHistB[1]})
            *im = mCtx->createImage2D(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mMarchA.image || !mMarchB.image || !mHistA[0].image || !mHistA[1].image || !mHistB[0].image || !mHistB[1].image)
    {
        LL_WARNS("VKVolClouds") << "volumetric clouds: " << w << "x" << h << " targets refused (out of video memory)" << LL_ENDL;
        destroyTargets();
        return false;
    }
    mW = w; mH = h;
    GpuImage* imgs[6] = {&mMarchA, &mMarchB, &mHistA[0], &mHistA[1], &mHistB[0], &mHistB[1]};
    VkImageMemoryBarrier2 b[6];
    for (int i = 0; i < 6; ++i) b[i] = toGeneral(imgs[i]->image);
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 6; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);
    // No NaN may ever sit in a history (a reset frame never reads it, but bilinear taps at
    // the edge of a valid region could).
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue clrA{{0.f, 0.f, 0.f, 1.f}};
    const VkClearColorValue clrB{{65000.f, 65000.f, 0.f, 0.f}};
    for (int i = 0; i < 6; ++i)
    {
        const bool isB = imgs[i] == &mMarchB || imgs[i] == &mHistB[0] || imgs[i] == &mHistB[1];
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, isB ? &clrB : &clrA, 1, &rng);
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    mBoundDepthView = VK_NULL_HANDLE;   // the sets are written by the caller (depth known there)
    mHistValid = false;
    return true;
}

void LLCleanVolCloudsVK::writeMainSets()
{
    VkDevice dev = mCtx->device();
    const VkDescriptorBufferInfo ub{mParamBuf.buffer, 0, sizeof(Params)};
    for (uint32_t p = 0; p < 2; ++p)
    {
        const uint32_t q = p ^ 1u;
        const VkDescriptorImageInfo ii[10] = {
            {mSampRepeat, mNoiseBase.view, VK_IMAGE_LAYOUT_GENERAL},                 // 1
            {mSampRepeat, mNoiseDetail.view, VK_IMAGE_LAYOUT_GENERAL},               // 2
            {mSampRepeat, mWeather.view, VK_IMAGE_LAYOUT_GENERAL},                   // 3
            {mSampNearest, mBoundDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, // 4
            {VK_NULL_HANDLE, mMarchA.view, VK_IMAGE_LAYOUT_GENERAL},                 // 5
            {VK_NULL_HANDLE, mMarchB.view, VK_IMAGE_LAYOUT_GENERAL},                 // 6
            {mSampClamp, mHistA[q].view, VK_IMAGE_LAYOUT_GENERAL},                   // 7 last frame
            {mSampClamp, mHistB[q].view, VK_IMAGE_LAYOUT_GENERAL},                   // 8
            {VK_NULL_HANDLE, mHistA[p].view, VK_IMAGE_LAYOUT_GENERAL},               // 9 this frame
            {VK_NULL_HANDLE, mHistB[p].view, VK_IMAGE_LAYOUT_GENERAL}};              // 10
        VkWriteDescriptorSet w[11]{};
        for (int i = 0; i < 11; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mMainSet[p];
            w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        for (int i = 1; i < 11; ++i)
        {
            const bool sampled = i <= 4 || i == 7 || i == 8;
            w[i].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &ii[i - 1];
        }
        vkUpdateDescriptorSets(dev, 11, w, 0, nullptr);
        mShapes.write(dev, mMainSet[p], mSampClamp);   // <GMDX>
    }
    // Composite: [0] the raw march, [1 + p] history parity p.
    for (int s = 0; s < 3; ++s)
    {
        const GpuImage& a = s == 0 ? mMarchA : mHistA[s - 1];
        const GpuImage& b = s == 0 ? mMarchB : mHistB[s - 1];
        const VkDescriptorImageInfo ii[3] = {{mSampNearest, mBoundDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                             {mSampNearest, a.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {mSampNearest, b.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mCompSet[s];
            w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
            w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i == 0) w[i].pBufferInfo = &ub; else w[i].pImageInfo = &ii[i - 1];
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// per recording
// ---------------------------------------------------------------------------
bool LLCleanVolCloudsVK::prepare(const Frame& f)
{
    static LLCachedControl<U32> s_quality(gSavedSettings, "RenderVKVolCloudQuality", 3);   // <GMDX> default High
    static LLCachedControl<F32> s_alt(gSavedSettings, "RenderVKVolCloudAltitude", 1500.f);
    static LLCachedControl<F32> s_thick(gSavedSettings, "RenderVKVolCloudThickness", 2500.f);   // <GMDX> default 2500 m
    static LLCachedControl<bool> s_shadows(gSavedSettings, "RenderVKVolCloudShadows", true);
    static LLCachedControl<F32> s_shadow_k(gSavedSettings, "RenderVKVolCloudShadowStrength", 0.85f);
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKVolCloudScale", 1.f);
    static LLCachedControl<F32> s_density(gSavedSettings, "RenderVKVolCloudDensity", 1.f);
    static LLCachedControl<F32> s_maxdist(gSavedSettings, "RenderVKVolCloudMaxDist", 40000.f);
    static LLCachedControl<bool> s_temporal(gSavedSettings, "RenderVKVolCloudTemporal", true);

    mLive = mMainNow = mMarchRan = false;
    mShadowNow = mPanoNow = mTemporalNow = false;
    std::memset(mVc, 0, sizeof(mVc));
    if (!created() || !f.ctx || !ensurePipelines()) return false;
    // The 2D dome's own gate (clean_clouds.glsl: noise texture present, RENDER_TYPE_CLOUDS,
    // scale >= 0.001) and the atmosphere: off -> no clouds at all, as the dome.
    if (!f.clouds || !f.atmos || !f.camTime || !f.viewProjRel || f.clouds[15] < 0.5f || !f.cloudNoise
        || f.atmos[6 * 4 + 2] < 0.5f || !f.dsetDraw0 || !f.dsetDraw1 || f.rw < 2 || f.rh < 2)
        return false;
    mDsetDraw0 = f.dsetDraw0;
    mDsetDraw1 = f.dsetDraw1;
    mOffscreen = f.offscreen;

    const float base = (float)s_alt;
    const float top = base + llclamp((F32)s_thick, 100.f, 10000.f);
    const double tile = kTilePerScale * llclamp((double)f.clouds[3], 0.05, 3.0) * llclamp((double)(F32)s_scale, 0.1, 10.0);
    const double basePeriod = tile / std::max(1.0, std::round(tile / kBasePeriod));
    const double detailPeriod = basePeriod / 16.0;
    // Wind: the EEP scroll is baked into the weather map (u + pd1.xy); the 3D noises follow
    // with the same offset in metres, wrapped by the base period (divides the tile).
    const double pdx = (double)f.clouds[4], pdy = (double)f.clouds[5];
    const double offx = std::fmod((pdx - std::floor(pdx)) * tile, basePeriod);
    const double offy = std::fmod((pdy - std::floor(pdy)) * tile, basePeriod);

    const U32 q = llclamp((U32)s_quality, 1u, 3u);
    const Tier& tt = kTiers[q];
    // <GMDX> In (or at the edge of) the layer every texel is marched each frame and the history
    // only averages the dither: the cloud is all around and close. The trails history once left
    // on the avatar and nearby objects came from blending texels whose march ended at a different
    // surface; the temporal pass now drops those (vc.temporal.w). Without the history the step
    // jitter stood still and every step showed as a band.
    const bool inLayer = f.camTime[2] > base - 300.f && f.camTime[2] < top + 300.f;
    const bool temporal = s_temporal && !f.offscreen;
    uint32_t pattern = temporal && !inLayer ? tt.pattern : 0u;
    uint32_t steps = tt.steps, light = tt.light;
    if (f.offscreen) { steps = steps * 3 / 2; light = 6; }

    // Light (atmos[7]: the sun or the moon, normalized).
    float L[3] = {f.atmos[28], f.atmos[29], f.atmos[30]};
    {
        const float n = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (n > 1e-6f) { L[0] /= n; L[1] /= n; L[2] /= n; } else { L[0] = L[1] = 0.f; L[2] = 1.f; }
    }

    Params& P = mP;
    P = Params{};
    P.shape0[0] = (float)tile; P.shape0[1] = (float)(1.0 / tile);
    P.shape0[2] = (float)(1.0 / basePeriod); P.shape0[3] = (float)(1.0 / detailPeriod);
    P.shape1[0] = (float)offx; P.shape1[1] = (float)offy;
    P.shape1[2] = kExtinction * llclamp((F32)s_density, 0.f, 8.f);
    P.shape1[3] = 0.35f;                                   // detail erosion
    P.shape2[0] = 1.f;                                     // ambient scale
    P.shape2[1] = llclamp((F32)s_maxdist, 4000.f, 60000.f);
    P.shape2[2] = 1.f;                                     // multi-scatter strength
    P.shape2[3] = 0.6f;                                    // powder strength
    P.phase[0] = 0.8f; P.phase[1] = 0.3f; P.phase[2] = 0.25f;
    P.march[0] = (float)steps; P.march[1] = (float)light;
    // #932: a supersampled offscreen shot's recordings are averaged: each a distinct march
    // offset by its sample index (mFrame holds offscreen; the live clock is untouched).
    P.march[2] = (float)(temporal ? (mFrame % 64u) : f.snapSample ? ((mFrame + f.snapSample) % 64u) : 0u);
    P.march[3] = f.offscreen ? 1.f : 0.f;
    P.cam[0] = f.camTime[0]; P.cam[1] = f.camTime[1]; P.cam[2] = f.camTime[2]; P.cam[3] = f.camTime[3];
    // <GMDX> The agent frame's global origin, wrapped by 21 weather tiles (the noises and the
    // anti-tiling term in gmdx_cloud_common.glsl stay periodic): the clouds hold still across a
    // region crossing instead of jumping with the frame.
    const LLVector3d o = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    {
        const double wrap = tile * 21.0;
        // Wind speed (GMDXCloudWindSpeed, x the region's EEP cloud scroll): the scroll's step since
        // the last recording, times (speed - 1), adds to a drift every lookup shares (the weather
        // map, the noises, the bodies, the shadow map), so the clouds travel faster or slower in
        // the region's own direction; 1 = the region's speed.
        static LLCachedControl<F32> s_wind(gSavedSettings, "GMDXCloudWindSpeed", 1.f);
        const double k = llclamp((double)(F32)s_wind, 0.0, 20.0);
        if (mWindPrimed && !f.offscreen)
        {
            for (int i = 0; i < 2; ++i)
            {
                double d = (i ? pdy : pdx) - mWindPrevPd[i];
                if (std::fabs(d) > 0.5) d -= std::round(d);   // (a wrapped scroll)
                mWindExtra[i] = std::fmod(mWindExtra[i] + d * tile * (k - 1.0), wrap);
            }
        }
        mWindPrevPd[0] = pdx; mWindPrevPd[1] = pdy;
        mWindPrimed = true;
        const double ox = o.mdV[VX] + mWindExtra[0], oy = o.mdV[VY] + mWindExtra[1];
        P.gmdx[0] = (float)(ox - std::floor(ox / wrap) * wrap);
        P.gmdx[1] = (float)(oy - std::floor(oy / wrap) * wrap);
        // The EEP scroll the weather map bakes in (its uv offset): the cloud bodies ride on it.
        P.gmdx[2] = (float)(pdx - std::floor(pdx));
        P.gmdx[3] = (float)(pdy - std::floor(pdy));
        static LLCachedControl<F32> s_bodies(gSavedSettings, "GMDXCloudBodies", 1.f);
        P.gmdx[4] = (float)mShapes.count();
        P.gmdx[5] = llclamp((F32)s_bodies, 0.f, 1.f);
        // The smallest cloud footprint the EEP cloud texture may draw (m), a share of the shell's
        // thickness: the coverage stands straight up through the cloud type's height band, so a
        // feature narrower than that is a column, not a cloud (gmdx_cloud_weather.comp).
        static LLCachedControl<F32> s_minw(gSavedSettings, "GMDXCloudMinWidth", 0.05f);
        P.gmdx[6] = llclamp((F32)s_minw, 0.f, 0.5f) * (top - base);
    }
    {
        // #783 M2 LIGHTNING: the flash lights the cloud layer from inside (the composite adds it
        // over the clouds' own opacity; no history, so nothing smears). Not in snapshots.
        float rad[3], dir[3];
        if (!f.offscreen && LLCleanWeatherVK::flashNow(rad, dir))
        {
            P.flash[0] = dir[0]; P.flash[1] = dir[1]; P.flash[2] = dir[2];
            P.flash[3] = 0.2126f * rad[0] + 0.7152f * rad[1] + 0.0722f * rad[2];
        }
    }

    // ---- shadow map: refresh decision + the latched frame ----
    const bool shadowsOn = s_shadows && s_shadow_k > 0.f;
    if (shadowsOn)
    {
        const double texel = (double)kShadowExtent / kShadowSize;
        const double cx = std::floor((double)f.camTime[0] / texel) * texel;
        const double cy = std::floor((double)f.camTime[1] / texel) * texel;
        const float dotL = L[0] * mShLight[0] + L[1] * mShLight[1] + L[2] * mShLight[2];
        const double dx = cx - mShCentre[0], dy = cy - mShCentre[1];
        const bool due = !mShValid || mShAge == ~0u || mShAge + 1u >= kShadowEvery || dotL < 0.99996f   // ~0.5 deg
                       || dx * dx + dy * dy > (double)(kShadowExtent * 0.2f) * (kShadowExtent * 0.2f) || f.offscreen
                       || o.mdV[VX] != mShOrigin[0] || o.mdV[VY] != mShOrigin[1];   // <GMDX> the frame moved (a crossing)
        if (due)
        {
            mShCentre[0] = cx; mShCentre[1] = cy;
            mShOrigin[0] = o.mdV[VX]; mShOrigin[1] = o.mdV[VY];   // <GMDX>
            mShScroll[0] = pdx; mShScroll[1] = pdy;
            mShWind[0] = mWindExtra[0]; mShWind[1] = mWindExtra[1];   // <GMDX>
            mShLight[0] = L[0]; mShLight[1] = L[1]; mShLight[2] = L[2];
            mShAge = 0;
            mShValid = true;
            mShadowNow = true;
        }
        else ++mShAge;
        P.shadow[0] = (float)mShCentre[0]; P.shadow[1] = (float)mShCentre[1];
        P.shadow[2] = kShadowExtent; P.shadow[3] = (float)kShadowSteps;
        // Between refreshes the clouds drift with the scroll: the lookup's centre follows.
        // <GMDX> (+ the wind speed's own drift since the refresh)
        const double driftx = -(pdx - mShScroll[0]) * tile - (mWindExtra[0] - mShWind[0]);
        const double drifty = -(pdy - mShScroll[1]) * tile - (mWindExtra[1] - mShWind[1]);
        mVc[0][3] = llclamp((F32)s_shadow_k, 0.f, 1.f);
        mVc[1][0] = (float)(mShCentre[0] + driftx);
        mVc[1][1] = (float)(mShCentre[1] + drifty);
        mVc[1][2] = 1.f / kShadowExtent;
        mVc[2][0] = mShLight[0]; mVc[2][1] = mShLight[1]; mVc[2][2] = mShLight[2];
    }
    else
        mShValid = false;

    // ---- panorama band (main view only; snapshots keep it) ----
    if (!f.offscreen)
    {
        mPanoNow = true;
        P.pano[0] = (float)mPanoRow;
        P.pano[1] = (float)kPanoRows;
        P.pano[2] = (float)kPanoSteps;
        P.pano[3] = mPanoFilled >= kPanoH ? 0.5f : 0.f;
        mPanoRow = (mPanoRow + kPanoRows) % kPanoH;
        mPanoFilled = std::min<uint32_t>(mPanoFilled + kPanoRows, kPanoH * 2u);
    }
    // (valid once every row was written at least once -- this recording's band included)
    mVc[1][3] = mPanoFilled >= kPanoH ? 1.f : 0.f;

    mVc[0][0] = 2.f; mVc[0][1] = base; mVc[0][2] = top;

    // ---- main view: march / temporal / composite ----
    mMainNow = !f.underwater;
    const uint32_t w = std::max(1u, (f.rw + 1u) / 2u), h = std::max(1u, (f.rh + 1u) / 2u);
    invert4(f.viewProjRel, P.inv_vp);
    P.dims[0] = (float)f.rw; P.dims[1] = (float)f.rh; P.dims[2] = (float)w; P.dims[3] = (float)h;
    const uint32_t phase = pattern == 2 ? (mFrame & 3u) : (mFrame & 1u);
    P.pat[0] = (float)pattern; P.pat[1] = (float)phase; P.pat[2] = (float)steps; P.pat[3] = (float)light;
    mGroupsX = pattern == 0 ? (w + 7u) / 8u : ((w + 1u) / 2u + 7u) / 8u;
    mGroupsY = pattern == 2 ? ((h + 1u) / 2u + 7u) / 8u : (h + 7u) / 8u;
    mTemporalNow = temporal && mMainNow;
    if (mTemporalNow)
    {
        const double dl[3] = {(double)f.camTime[0] - mPrevCam[0], (double)f.camTime[1] - mPrevCam[1],
                              (double)f.camTime[2] - mPrevCam[2]};
        // current camera-relative world -> last frame's clip = last view-proj-rel x
        // translate(current eye - last eye), in double (as the god rays' temporal).
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                double v = (double)mPrevViewProjRel[c * 4 + r];
                if (c == 3)
                    for (int k = 0; k < 3; ++k) v += (double)mPrevViewProjRel[k * 4 + r] * dl[k];
                P.reproj[c * 4 + r] = (float)v;
            }
        const bool jump = dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2] > kCamJump * kCamJump;
        const bool reset = !mHistValid || f.cameraCut || jump || q != mPrevQuality || w != mW || h != mH;
        P.temporal[0] = (float)(inLayer ? std::min(tt.hist, 8u) : tt.hist);   // <GMDX> (in the layer: quicker to follow)
        P.temporal[1] = reset ? 1.f : 0.f;
        P.temporal[2] = pattern == 0u ? kTiers[3].widen : tt.widen;   // <GMDX> (the widening of the pattern marched)
        P.temporal[3] = inLayer ? 1.f : 0.f;   // <GMDX> march-end test + march end kept (clean_vol_cloud_temporal.comp)
        mHistCur ^= 1u;
    }
    if (q != mLogQuality || w != mLogW || h != mLogH)
    {
        LL_INFOS("VKVolClouds") << "volumetric clouds: quality " << q << ", " << w << "x" << h << " (render "
                                << f.rw << "x" << f.rh << "), " << steps << " steps, pattern " << pattern
                                << (temporal ? ", temporal" : "") << ", layer " << base << "-" << top << " m, tile "
                                << (int)tile << " m" << (f.offscreen ? " [snapshot]" : "") << LL_ENDL;
        mLogQuality = q; mLogW = w; mLogH = h;
    }
    // This main-view recording is the next one's "previous" (snapshots never are).
    if (!f.offscreen)
    {
        std::memcpy(mPrevViewProjRel, f.viewProjRel, sizeof(mPrevViewProjRel));
        mPrevCam[0] = f.camTime[0]; mPrevCam[1] = f.camTime[1]; mPrevCam[2] = f.camTime[2];
        mPrevQuality = q;
        ++mFrame;
    }
    else
        mHistValid = false;   // a snapshot re-sizes / overwrites the targets
    mLive = true;
    return true;
}

void LLCleanVolCloudsVK::fillFrameInfo(float out[4][4]) const
{
    if (!mLive) { std::memset(out, 0, sizeof(float) * 16); return; }
    std::memcpy(out, mVc, sizeof(mVc));
}

bool LLCleanVolCloudsVK::recordPre(VkCommandBuffer cmd)
{
    if (!mLive) return false;
    // Earlier readers of the params UBO (last recording's computes / composite) -> this
    // update (WAR); the frame info's transfer write (its own barrier only names the graphics
    // stages) + this update -> the computes / composite (UBO reads).
    // (vkCmdUpdateBuffer executes in the CLEAR stage -- a "clear command" -- hence COPY | CLEAR.)
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd, mParamBuf.buffer, 0, sizeof(Params), &mP);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
    if (!mNoiseDone)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_NOISE_BASE]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mNoiseLayout, 0, 1, &mNoiseSet[0], 0, nullptr);
        vkCmdDispatch(cmd, kBaseSize / 4u, kBaseSize / 4u, kBaseSize / 4u);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_NOISE_DETAIL]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mNoiseLayout, 0, 1, &mNoiseSet[1], 0, nullptr);
        vkCmdDispatch(cmd, kDetailSize / 4u, kDetailSize / 4u, kDetailSize / 4u);
        mNoiseDone = true;
    }
    // Last recording's weather reads (computes) -> this rewrite; the noises -> their readers.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_WEATHER]);
    const VkDescriptorSet ws[3] = {mDsetDraw0, mDsetDraw1, mWxSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mWxLayout, 0, 3, ws, 0, nullptr);
    vkCmdDispatch(cmd, kWeatherSize / 8u, kWeatherSize / 8u, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return true;
}

bool LLCleanVolCloudsVK::recordShadow(VkCommandBuffer cmd)
{
    if (!mLive || !mShadowNow) return false;
    mShadowNow = false;
    // Earlier samples of the map (lit FS / deferred / god rays / probes) -> this rewrite.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_SHADOW]);
    const VkDescriptorSet s[2] = {mDsetDraw0, mPreSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, s, 0, nullptr);
    vkCmdDispatch(cmd, kShadowSize / 8u, kShadowSize / 8u, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return true;
}

bool LLCleanVolCloudsVK::recordPano(VkCommandBuffer cmd)
{
    if (!mLive || !mPanoNow) return false;
    mPanoNow = false;
    // Earlier samples (probe / mirror sky) and its own last band (read-modify-write).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_PANO]);
    const VkDescriptorSet s[2] = {mDsetDraw0, mPreSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, s, 0, nullptr);
    vkCmdDispatch(cmd, kPanoW / 8u, (kPanoRows + 7u) / 8u, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return true;
}

bool LLCleanVolCloudsVK::recordMarch(VkCommandBuffer cmd, VkImage depthImage, VkImageView depthView, uint64_t depthGen)
{
    mMarchRan = false;
    if (!mLive || !mMainNow || !depthImage || !depthView) return false;
    const uint32_t w = (uint32_t)mP.dims[2], h = (uint32_t)mP.dims[3];
    if (!ensureTargets(cmd, w, h)) return false;
    if (mBoundDepthView != depthView || mBoundDepthGen != depthGen)
    {
        if (mBoundDepthView) vkDeviceWaitIdle(mCtx->device());   // the sets may be in flight
        mBoundDepthView = depthView;
        mBoundDepthGen = depthGen;
        writeMainSets();
    }
    // (A history reset -- new targets, no valid history, a cut -- was decided in prepare: the
    //  params UBO already carries it.)
    // Pass 1's depth writes -> sampled here; the march targets: last recording's temporal /
    // composite reads -> these writes (execution).
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ib.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    ib.image = depthImage; ib.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = 0;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd, &d);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_MARCH]);
    const VkDescriptorSet s[2] = {mDsetDraw0, mMainSet[mHistCur]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, s, 0, nullptr);
    vkCmdDispatch(cmd, mGroupsX, mGroupsY, 1);
    // March writes -> the temporal pass (compute) / the composite (fragment).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    mMarchRan = true;
    return true;
}

bool LLCleanVolCloudsVK::recordTemporal(VkCommandBuffer cmd)
{
    if (!mMarchRan || !mTemporalNow) { if (mMarchRan) mHistValid = false; return false; }
    // Last recording's history write (parity p ^ 1) was released by the memory barrier at its
    // end (compute -> fragment); widen it to this compute read, and order the composite's
    // earlier reads of parity p before these writes.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_TEMPORAL]);
    const VkDescriptorSet s[2] = {mDsetDraw0, mMainSet[mHistCur]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, s, 0, nullptr);
    vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    mHistValid = true;
    return true;
}

void LLCleanVolCloudsVK::recordComposite(VkCommandBuffer cmd, VkImageView hdrView)
{
    if (!mMarchRan || !hdrView) return;
    mMarchRan = false;
    const uint32_t rw = (uint32_t)mP.dims[0], rh = (uint32_t)mP.dims[1];
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = hdrView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rw, rh}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0.f, 0.f, (float)rw, (float)rh, 0.f, 1.f};
    VkRect2D sc{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipe[P_COMP]);
    const VkDescriptorSet set = (mTemporalNow && mHistValid) ? mCompSet[1 + mHistCur] : mCompSet[0];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // These colour writes -> pass 2's colour attachment loads / blends.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
