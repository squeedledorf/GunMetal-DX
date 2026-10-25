/**
 * @file llcleanvolfogvk.cpp
 * @brief Volumetric fog (#718): see llcleanvolfogvk.h and the shaders clean_vol_*.
 */
#include "linden_common.h"

#include "llcleanvolfogvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "lldate.h"   // <OCOL> the mist's wall-clock drift
#include "llerror.h"
#include "llviewercontrol.h"
#include "llvkhitch.h"   // volumetric particle counters (#721)
#include "slweather.h"   // weather drives the height fog (#783)
#include "v3math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Grid tiers (RenderVKVolumetricFogQuality 0 / 1 / 2): froxels x, y (16:9, uniform in the
// unjittered screen) x depth slices. 1 = the ticket's ~160 x 90 x 64.
struct Tier { uint32_t w, h, d; };
constexpr Tier kTiers[3] = {{128u, 72u, 48u}, {160u, 90u, 64u}, {192u, 108u, 96u}};

// Haze sun scatter gain over the physical haze sigma_s: the god rays' kScatterGain
// (llcleangodraysvk.cpp; the physical haze makes invisible shafts), so fog-on shafts read
// like god-rays-on shafts at RenderVKVolumetricFogSunScatter 1.
constexpr float kScatterGain = 8.f;
// EEP-derived ("auto") height fog: sigma0 = this x the haze's Mie extinction (haze_density
// x density_multiplier x distance_multiplier) at the base height -- a hazier sky has a
// thicker ground fog, a clear one (haze ~0) next to none. ~4.3e-4 /m on the default sky:
// a subtle ground haze (T ~ 0.92 over 200 m at the base).
constexpr float kAutoFog = 4.f;
// Auto height falloff: scale height = this x the sky's haze ceiling max_y (default 1605 m
// -> ~32 m), clamped to [10, 200] m.
constexpr float kAutoScaleHeight = 0.02f;
// Auto HG anisotropy (EEP has no phase parameter): the god rays' default.
constexpr float kAutoAniso = 0.6f;
// Temporal: camera move per frame past which the history is dropped (teleport / origin
// shift the renderer did not flag), and the neighbourhood-box widening.
constexpr double kCamJump = 32.0;
constexpr float  kClampWiden = 0.25f;
// WEATHER (#783): extra height-fog extinction (per metre) at weather fog 1 (SLWeather::State::fog:
// #954 only a source's explicit fog=, e.g. a parcel tag) -- T ~ 0.45 over 100 m at the base. The
// precipitation's own medium is not this: clean_vol_inject.comp injects it where it falls
// (clean_weather_veil.glsl, ssr.weather[16]).
constexpr float  kWeatherFog = 0.008f;

VkShaderModule vfModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

// Inverse of a column-major 4x4 (double inside; the camera-relative view-projection).
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

float halton(uint32_t i, uint32_t b)
{
    float f = 1.f, r = 0.f;
    while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

// FIRST USE OFF THE RENDER THREAD (the god rays' pattern): set layout, pool and pipeline
// layouts here; the five pipelines go to an async batch and land on a later prepare() --
// the fog is skipped until then. A failed build is latched for these layouts / formats.
bool LLCleanVolFogVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mInjectPipe = mStage[0]; mLightPipe = mStage[1]; mTemporalPipe = mStage[2];
        mIntegratePipe = mStage[3]; mApplyPipe = mStage[4]; mPartPipe = mStage[5];
        for (VkPipeline& p : mStage) p = VK_NULL_HANDLE;
        if (!mInjectPipe || !mLightPipe || !mTemporalPipe || !mIntegratePipe || !mApplyPipe || !mPartPipe)
        {
            LL_WARNS("VKVolFog") << "volumetric fog pipelines failed to build; effect disabled" << LL_ENDL;
            const VkDescriptorSetLayout d0 = mPipeDsl[0], d1 = mPipeDsl[1];
            VkFormat ff[4] = {mPipeFormats[0], mPipeFormats[1], mPipeFormats[2], mPipeFormats[3]};
            destroy(dev);
            mPipeDsl[0] = d0; mPipeDsl[1] = d1;
            for (int i = 0; i < 4; ++i) mPipeFormats[i] = ff[i];
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VKVolFog") << "volumetric fog pipelines built (background)" << LL_ENDL;
    }
    const bool same = mPipeDsl[0] == f.dslDraw0 && mPipeDsl[1] == f.dslDraw1
                   && mPipeFormats[0] == f.hdrFormat && mPipeFormats[1] == f.glowFormat
                   && mPipeFormats[2] == f.reactFormat && mPipeFormats[3] == f.depthFormat;
    if (mApplyPipe && same) return true;
    if (mPipeFailed && same) return false;
    mPipeFailed = false;
    if (mApplyPipe || mDsl)
    {
        // A pass format / set layout change (rare): rebuild from scratch.
        destroy(dev);
    }
    mCtx = f.ctx;
    PipelineBatchScope batchScope(dev);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));

    // Own set (clean_vol_pass.glsl): 0 cur (storage), 1 volume list (SSBO), 2 history prev
    // (sampled), 3 history cur (storage), 4 integrated (storage); volumetric particles (#721):
    // 5 the particle pool (SSBO), 6 the particle volume buffer (SSBO), 7 the emission grid
    // (storage). While the particles are off 5 / 6 name the volume list and 7 the current
    // cells (never accessed then: the passes' push flags gate them).
    VkDescriptorSetLayoutBinding b[8]{};
    const VkDescriptorType types[8] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (uint32_t i = 0; i < 8; ++i)
    {
        b[i].binding = i; b[i].descriptorType = types[i];
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 8; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslVolFog"));
    VK_CHECK(mPool.create(dev, "volfog", {{mDsl, 4}}));
    for (int p = 0; p < 2; ++p)
        for (int s = 0; s < 2; ++s)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset[p][s]), { destroy(dev); return false; });

    // The four passes: {draw set 0, draw set 1, own set}, 128 B push.
    {
        const VkDescriptorSetLayout sets[3] = {f.dslDraw0, f.dslDraw1, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mPassLayout, "mVolFogPassLayout"));
        struct { const uint32_t* code; size_t bytes; const char* name; VkPipeline* out; } cs[5] = {
            {kWorldVolInjectSpv,    kWorldVolInjectSpv_bytes,    "kWorldVolInjectSpv",    &mStage[0]},
            {kWorldVolLightSpv,     kWorldVolLightSpv_bytes,     "kWorldVolLightSpv",     &mStage[1]},
            {kWorldVolTemporalSpv,  kWorldVolTemporalSpv_bytes,  "kWorldVolTemporalSpv",  &mStage[2]},
            {kWorldVolIntegrateSpv, kWorldVolIntegrateSpv_bytes, "kWorldVolIntegrateSpv", &mStage[3]},
            // (#721: the particle classify / bin pass -- same layout and push range)
            {kWorldVolPartSpv,      kWorldVolPartSpv_bytes,      "kWorldVolPartSpv",      &mStage[5]}};
        for (auto& c : cs)
        {
            VkShaderModule m = vfModule(dev, c.code, c.bytes, c.name);
            createComputePipelineInto(dev, m, mPassLayout, 0, nullptr, c.out, c.name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
    }
    // Apply: {draw set 0}, fragment push; render pass 2's attachments, C' = src + dst x src.a
    // on the scene (its alpha preserved), glow / reactive write-masked, no depth test.
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ApplyPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &f.dslDraw0;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mApplyLayout, "mVolFogApplyLayout"));
        VkShaderModule vs = vfModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = vfModule(dev, kWorldVolApplyFragSpv, kWorldVolApplyFragSpv_bytes, "kWorldVolApplyFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mApplyLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowWrite = false;
        d.reactFormat = f.reactFormat; d.reactWriteMask = 0;
        d.depthFormat = f.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.customBlend = true;
        d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[4], "volumetric fog apply");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
    }
    mPipeDsl[0] = f.dslDraw0; mPipeDsl[1] = f.dslDraw1;
    mPipeFormats[0] = f.hdrFormat; mPipeFormats[1] = f.glowFormat;
    mPipeFormats[2] = f.reactFormat; mPipeFormats[3] = f.depthFormat;
    PipelineJobOpts o;
    o.what = "volumetric fog";
    mJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKVolFog") << "volumetric fog pipelines compiling in the background (the effect starts when they land)" << LL_ENDL;
    return false;
}

// The ray-traced light pass (VOL_RT; the god rays' ensureRtPipeline pattern): built in the
// background the first time the renderer's projector shadows are ray traced; until it lands
// (or if it fails) the plain light pass runs and ray-traced records stay unshadowed in the
// fog. Needs mDsl / mPassLayout (ensurePipelines) first.
bool LLCleanVolFogVK::ensureRtPipeline(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    const VkDescriptorSetLayout want[4] = {f.dslDraw0, f.dslDraw1, mDsl, f.rtLayout};
    const bool same = mPipeRtDsl[0] == want[0] && mPipeRtDsl[1] == want[1] && mPipeRtDsl[2] == want[2]
                   && mPipeRtDsl[3] == want[3];
    if (mRtJob.pending())
    {
        if (!mRtJob.poll()) return false;
        mLightRtPipe = mRtStage;
        mRtStage = VK_NULL_HANDLE;
        if (!mLightRtPipe)
        {
            LL_WARNS("VKVolFog") << "volumetric fog ray-traced light pass failed to build; ray-traced projectors "
                                    "stay unshadowed in the fog" << LL_ENDL;
            if (mPassRtLayout) vkDestroyPipelineLayout(dev, mPassRtLayout, nullptr);
            mPassRtLayout = VK_NULL_HANDLE;
            mRtFailed = true;
        }
        else
            LL_INFOS("VKVolFog") << "volumetric fog ray-traced light pass built (background)" << LL_ENDL;
    }
    if (mLightRtPipe && same) return true;
    if (mRtFailed && same) return false;
    if (mLightRtPipe || mPassRtLayout) destroyRtPipeline(dev);
    mRtFailed = false;
    for (int i = 0; i < 4; ++i) mPipeRtDsl[i] = want[i];
    PipelineBatchScope batchScope(dev);
    const VkDescriptorSetLayout sets[4] = {f.dslDraw0, f.dslDraw1, mDsl, f.rtLayout};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolPush)};   // = mPassLayout's
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4; pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mPassRtLayout, "mVolFogPassRtLayout"));
    VkShaderModule m = vfModule(dev, kWorldVolLightRtSpv, kWorldVolLightRtSpv_bytes, "kWorldVolLightRtSpv");
    createComputePipelineInto(dev, m, mPassRtLayout, 0, nullptr, &mRtStage, "volumetric fog light (ray traced)");
    destroyShaderModuleDeferred(dev, m, nullptr);
    PipelineJobOpts o;
    o.what = "volumetric fog (ray traced)";
    mRtJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKVolFog") << "volumetric fog ray-traced light pass compiling in the background" << LL_ENDL;
    return false;
}

void LLCleanVolFogVK::destroyRtPipeline(VkDevice dev)
{
    mRtJob.wait();   // a build in flight writes the stage and references the layout
    if (mRtStage) { vkDestroyPipeline(dev, mRtStage, nullptr); mRtStage = VK_NULL_HANDLE; }
    if (mLightRtPipe || mPassRtLayout) vkDeviceWaitIdle(dev);   // may be in flight
    if (mLightRtPipe) vkDestroyPipeline(dev, mLightRtPipe, nullptr);
    if (mPassRtLayout) vkDestroyPipelineLayout(dev, mPassRtLayout, nullptr);
    mLightRtPipe = VK_NULL_HANDLE;
    mPassRtLayout = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout& l : mPipeRtDsl) l = VK_NULL_HANDLE;
    mRtFailed = false;
}

void LLCleanVolFogVK::destroyImages()
{
    for (GpuImage* im : {&mCur, &mHist[0], &mHist[1], &mInteg, &mEmit})
    {
        if (im->image && mCtx) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    mW = mH = mD = 0;
    mHistValid = false;
}

// The grid images (tier change / first use). true = they exist at w x h x d.
bool LLCleanVolFogVK::ensureImages(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t d)
{
    if (mInteg.image && mW == w && mH == h && mD == d) return true;
    VkMemScope memcat(VKMEM_TARGET);   // #407
    vkDeviceWaitIdle(mCtx->device());  // in use by frames in flight (and named by draw set 0 binding 45)
    const bool had = mInteg.image != VK_NULL_HANDLE;
    destroyImages();
    mBindingDirty = mBindingDirty || had;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    GpuImage* imgs[4] = {&mCur, &mHist[0], &mHist[1], &mInteg};
    bool ok = true;
    for (GpuImage* im : imgs)
    {
        *im = mCtx->createImage3D(w, h, d, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        ok = ok && im->image;
    }
    if (!ok)
    {
        // #697 allocation policy: no crash -- the fog skips (retried on the next tier / toggle).
        LL_WARNS("VKVolFog") << "volumetric fog grid " << w << "x" << h << "x" << d
                             << " failed to allocate; effect skipped" << LL_ENDL;
        destroyImages();
        return false;
    }
    mW = w; mH = h; mD = d;
    mBindingDirty = true;
    // GENERAL for good; cleared (no NaN ever sits in a history; the integrated volume to the
    // identity: no in-scatter, T = 1), then visible to the compute / fragment readers.
    VkImageMemoryBarrier2 ib[4]{};
    for (int i = 0; i < 4; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].image = imgs[i]->image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 4; dep.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue zero{};
    VkClearColorValue ident{};
    ident.float32[3] = 1.f;
    for (int i = 0; i < 4; ++i)
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, i == 3 ? &ident : &zero, 1, &rng);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // The volume list ring (host-visible, written every running frame).
    for (GpuBuffer& lb : mList)
        if (!lb.buffer)
            lb = mCtx->createBuffer(16 + (VkDeviceSize)kVolCap * sizeof(VolPrimGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mList[0].buffer || !mList[1].buffer || !mList[0].mapped || !mList[1].mapped)
    {
        LL_WARNS("VKVolFog") << "volumetric fog volume list failed to allocate; effect skipped" << LL_ENDL;
        destroyImages();
        return false;
    }
    writeSets();
    return true;
}

void LLCleanVolFogVK::writeSets()
{
    if (!mCtx || !mDsl || !mInteg.image) return;
    VkDevice dev = mCtx->device();
    for (uint32_t p = 0; p < 2; ++p)
        for (uint32_t s = 0; s < 2; ++s)
        {
            // Parity p: this frame's history is mHist[p], last frame's mHist[p ^ 1].
            VkDescriptorImageInfo ii[4] = {{VK_NULL_HANDLE, mCur.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {mSampler, mHist[p ^ 1u].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mHist[p].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mInteg.view, VK_IMAGE_LAYOUT_GENERAL}};
            VkDescriptorBufferInfo bi{mList[s].buffer, 0, VK_WHOLE_SIZE};
            // #721: the particle bindings (stand-ins while the particles are off).
            const bool part = mPartPool && mPV.buffer && mEmit.view;
            VkDescriptorBufferInfo bp{part ? mPartPool : mList[s].buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo bv{part ? mPV.buffer : mList[s].buffer, 0, VK_WHOLE_SIZE};
            VkDescriptorImageInfo ie{VK_NULL_HANDLE, part ? mEmit.view : mCur.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr[8]{};
            for (uint32_t i = 0; i < 8; ++i)
            {
                wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[i].dstSet = mDset[p][s]; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
            }
            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          wr[0].pImageInfo = &ii[0];
            wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         wr[1].pBufferInfo = &bi;
            wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[2].pImageInfo = &ii[1];
            wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          wr[3].pImageInfo = &ii[2];
            wr[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          wr[4].pImageInfo = &ii[3];
            wr[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         wr[5].pBufferInfo = &bp;
            wr[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         wr[6].pBufferInfo = &bv;
            wr[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          wr[7].pImageInfo = &ie;
            vkUpdateDescriptorSets(dev, 8, wr, 0, nullptr);
        }
    const bool part = mPartPool && mPV.buffer && mEmit.view;
    mSetPool = part ? mPartPool : VK_NULL_HANDLE;
    mSetPV = part ? mPV.buffer : VK_NULL_HANDLE;
    mSetEmit = part ? mEmit.view : VK_NULL_HANDLE;
}

// DEBUG TEST VOLUMES (RenderVKVolumetricFogTestVolumes = N, 0..8): when N changes, N
// volumes are anchored in a row 14 m ahead of the camera (level), alternating noise-shaped
// ellipsoids and soft boxes with a few tints, scrolling slowly -- walk / fly around them.
// 0 removes them. Agent-frame positions: a region crossing shifts them (P1 debug aid).
void LLCleanVolFogVK::updateTestVolumes(const Frame& f, const float* fwd)
{
    static LLCachedControl<U32> s_test(gSavedSettings, "RenderVKVolumetricFogTestVolumes", 0);
    const uint32_t n = std::min<U32>(s_test, 8u);
    if (n == mTestCount) return;
    mTestCount = n;
    mTestVols.clear();
    if (n == 0) return;
    LLVector3 fw(fwd[0], fwd[1], 0.f);
    if (fw.normVec() < 1e-3f) fw.setVec(1.f, 0.f, 0.f);
    const LLVector3 side(-fw.mV[1], fw.mV[0], 0.f);
    const LLVector3 eye(f.camTime[0], f.camTime[1], f.camTime[2]);
    const float tints[4][3] = {{0.95f, 0.95f, 0.95f}, {0.95f, 0.75f, 0.55f}, {0.6f, 0.8f, 1.f}, {0.8f, 1.f, 0.7f}};
    for (uint32_t i = 0; i < n; ++i)
    {
        const float off = ((float)i - 0.5f * (float)(n - 1)) * 9.f;
        const LLVector3 c = eye + fw * 14.f + side * off + LLVector3(0.f, 0.f, 0.5f);
        const bool ell = (i & 1u) == 0u;
        const float hx = ell ? 4.f : 3.5f, hy = ell ? 3.f : 3.5f, hz = ell ? 2.5f : 3.f;
        VolPrimGPU v{};
        v.pos_shape[0] = c.mV[0]; v.pos_shape[1] = c.mV[1]; v.pos_shape[2] = c.mV[2]; v.pos_shape[3] = ell ? 1.f : 0.f;
        v.axis_x[0] = fw.mV[0]; v.axis_x[1] = fw.mV[1]; v.axis_x[2] = 0.f; v.axis_x[3] = hx;
        v.axis_y[0] = side.mV[0]; v.axis_y[1] = side.mV[1]; v.axis_y[2] = 0.f; v.axis_y[3] = hy;
        v.ext_z[0] = hz;
        v.ext_z[1] = ell ? 0.6f : 0.3f;     // edge softness
        v.ext_z[2] = ell ? 0.35f : 0.2f;    // core extinction (/m): ~5 m to 1/e
        v.ext_z[3] = 0.45f;                 // noise frequency (1/m)
        const float* t = tints[i % 4u];
        v.albedo[0] = t[0]; v.albedo[1] = t[1]; v.albedo[2] = t[2];
        v.albedo[3] = ell ? 0.7f : 0.4f;    // noise amount
        v.scroll[0] = 0.4f; v.scroll[1] = 0.15f; v.scroll[2] = 0.1f;
        v.scroll[3] = std::sqrt(hx * hx + hy * hy + hz * hz) + 0.01f;
        mTestVols.push_back(v);
    }
    LL_INFOS("VKVolFog") << "volumetric fog: " << n << " test volume(s) placed ahead of the camera" << LL_ENDL;
}

// VOLUMETRIC PARTICLES (#721): the emission grid (the fog grid's dims), the particle volume
// buffer (fixed size: the record cap's ceiling) and the stats readback exist only while the
// particles are wanted; bindings 5-7 of the own sets follow (rewritten behind a device wait:
// the sets of frames in flight name them). #697: a refused allocation retries ~300 frames
// later, the fog keeps running without the particles meanwhile.
bool LLCleanVolFogVK::ensureParticles(VkCommandBuffer cmd, bool want, VkBuffer pool)
{
    if (!mCtx || !mDsl || !mInteg.image) return false;
    if (!want)
    {
        if (mPV.buffer || mEmit.image || mPVRb[0].buffer || mPVRb[1].buffer)
        {
            vkDeviceWaitIdle(mCtx->device());
            destroyParticles();
            mPartPool = VK_NULL_HANDLE;
            writeSets();
            LL_INFOS("VKVolFog") << "volumetric particles off (buffers freed)" << LL_ENDL;
        }
        return false;
    }
    const bool have = mPV.buffer && mEmit.image && mPVRb[0].mapped && mPVRb[1].mapped;
    if (!have && mFrame < mPartRetryFrame) return false;
    if (!have)
    {
        VkMemScope memcat(VKMEM_TARGET);   // #407
        vkDeviceWaitIdle(mCtx->device());
        destroyParticles();
        const VkDeviceSize bytes = kPVRecOffset + (VkDeviceSize)kPVCapMax * sizeof(PVolGPU);
        mPV = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (GpuBuffer& rb : mPVRb)
            rb = mCtx->createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        mEmit = mCtx->createImage3D(mW, mH, mD, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!mPV.buffer || !mEmit.image || !mPVRb[0].mapped || !mPVRb[1].mapped)
        {
            LL_WARNS("VKVolFog") << "volumetric particle buffers failed to allocate; particles stay sprites (retry later)"
                                 << LL_ENDL;
            destroyParticles();
            mPartRetryFrame = mFrame + 300u;
            if (mSetPool || mSetPV || mSetEmit) { mPartPool = VK_NULL_HANDLE; writeSets(); }
            return false;
        }
        // GENERAL for good, cleared (no stale emission is ever read), the buffer zeroed.
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.image = mEmit.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, mEmit.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        vkCmdFillBuffer(cmd, mPV.buffer, 0, VK_WHOLE_SIZE, 0u);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                       | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        mPVRbValid[0] = mPVRbValid[1] = false;
        LL_INFOS("VKVolFog") << "volumetric particles on: emission grid " << mW << "x" << mH << "x" << mD << " ("
                             << ((uint64_t)mW * mH * mD * 8u) / (1024u * 1024u) << " MB) + particle volume buffer "
                             << bytes / 1024u << " KB" << LL_ENDL;
    }
    if (mPartPool != pool || mSetPool != pool || mSetPV != mPV.buffer || mSetEmit != mEmit.view)
    {
        vkDeviceWaitIdle(mCtx->device());   // (sets of frames in flight name the old bindings)
        mPartPool = pool;
        writeSets();
    }
    return true;
}

void LLCleanVolFogVK::destroyParticles()
{
    if (!mCtx) return;
    if (mEmit.image) mCtx->destroyImage(mEmit);
    mEmit = GpuImage{};
    if (mPV.buffer) mCtx->destroyBuffer(mPV);
    mPV = GpuBuffer{};
    for (GpuBuffer& rb : mPVRb)
    {
        if (rb.buffer) mCtx->destroyBuffer(rb);
        rb = GpuBuffer{};
    }
    mPVRbValid[0] = mPVRbValid[1] = false;
    mPartOn = false;
}

// The stats this frame slot's last recording copied (its frame has retired: the slot's fence
// was waited before this recording): hitch counters + a log line when the cap state changes.
void LLCleanVolFogVK::readParticleStats()
{
    if (!mPVRbValid[mSlot] || !mPVRb[mSlot].mapped) return;
    uint32_t h[4];
    std::memcpy(h, mPVRb[mSlot].mapped, sizeof(h));
    const uint32_t cap = (uint32_t)mPartPush.q[0][2];
    LLVKHitch::count("vk_volPartCandidates", h[3]);
    LLVKHitch::count("vk_volPartInjected", std::min(h[0], cap));
    LLVKHitch::count("vk_volPartTileOverflow", h[2]);
    const bool over = h[3] > cap || h[2] > 0u;
    if (over != mPartLastOverCap)
    {
        mPartLastOverCap = over;
        if (over)
            LL_INFOS("VKVolFog") << "volumetric particles over budget: " << h[3] << " candidates, " << std::min(h[0], cap)
                                 << " injected (cap " << cap << ", RenderVKVolumetricParticleMax), " << h[2]
                                 << " tile-list overflows -- the farthest keep their sprites" << LL_ENDL;
        else
            LL_INFOS("VKVolFog") << "volumetric particles within budget (" << h[3] << " candidates)" << LL_ENDL;
    }
}

double LLCleanVolFogVK::sOriginGlobal[2] = {0.0, 0.0};   // <OCOL>

bool LLCleanVolFogVK::prepare(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<U32>  s_on(gSavedSettings, "RenderVKVolumetricFog", 0);
    static LLCachedControl<U32>  s_quality(gSavedSettings, "RenderVKVolumetricFogQuality", 1);
    static LLCachedControl<F32>  s_range(gSavedSettings, "RenderVKVolumetricFogRange", 128.f);
    static LLCachedControl<F32>  s_density(gSavedSettings, "RenderVKVolumetricFogDensity", -1.f);
    static LLCachedControl<F32>  s_falloff(gSavedSettings, "RenderVKVolumetricFogHeightFalloff", -1.f);
    static LLCachedControl<F32>  s_base(gSavedSettings, "RenderVKVolumetricFogBaseOffset", 0.f);
    static LLCachedControl<F32>  s_aniso(gSavedSettings, "RenderVKVolumetricFogAnisotropy", -1.f);
    static LLCachedControl<LLVector3> s_color(gSavedSettings, "RenderVKVolumetricFogColor", LLVector3(0.9f, 0.9f, 0.9f));
    static LLCachedControl<F32>  s_sun(gSavedSettings, "RenderVKVolumetricFogSunScatter", 1.f);
    static LLCachedControl<F32>  s_amb(gSavedSettings, "RenderVKVolumetricFogAmbient", 1.f);
    static LLCachedControl<F32>  s_ll(gSavedSettings, "RenderVKVolumetricFogLocalLights", 1.f);
    static LLCachedControl<bool> s_lv(gSavedSettings, "RenderVKVolumetricFogLocalVolumes", true);
    static LLCachedControl<F32>  s_hist(gSavedSettings, "RenderVKVolumetricFogHistory", 0.9f);
    static LLCachedControl<bool> s_far(gSavedSettings, "RenderVKVolumetricFogFarAnalytic", true);
    static LLCachedControl<U32>  s_lsh(gSavedSettings, "RenderVKVolumetricFogLocalShadows", 1);
    static LLCachedControl<F32>  s_shd(gSavedSettings, "RenderVKVolumetricFogShadowDistance", 64.f);
    static LLCachedControl<U32>  s_shr(gSavedSettings, "RenderVKVolumetricFogShadowRays", 1);
    // Volumetric particles (#721).
    static LLCachedControl<U32>  s_vp(gSavedSettings, "RenderVKVolumetricParticles", 0);
    static LLCachedControl<F32>  s_vpSprite(gSavedSettings, "RenderVKVolumetricParticleSprite", 0.35f);
    static LLCachedControl<F32>  s_vpDensity(gSavedSettings, "RenderVKVolumetricParticleDensity", 0.65f);
    static LLCachedControl<F32>  s_vpDist(gSavedSettings, "RenderVKVolumetricParticleDistance", 48.f);
    static LLCachedControl<U32>  s_vpMax(gSavedSettings, "RenderVKVolumetricParticleMax", 1024);
    static LLCachedControl<F32>  s_vpMin(gSavedSettings, "RenderVKVolumetricParticleMinSize", 0.5f);
    static LLCachedControl<F32>  s_vpEmit(gSavedSettings, "RenderVKVolumetricParticleEmission", 1.f);
    static LLCachedControl<F32>  s_vpNoise(gSavedSettings, "RenderVKVolumetricParticleNoise", 0.35f);
    // <OCOL> OCOL mist: the OCOL viewer's rolling ground mist as a mode of this fog.
    static LLCachedControl<bool> s_ocol(gSavedSettings, "OCOLHeightFog", false);
    static LLCachedControl<F32>  s_ocolDensity(gSavedSettings, "OCOLHeightFogDensity", 0.015f);
    static LLCachedControl<F32>  s_ocolBase(gSavedSettings, "OCOLHeightFogBaseHeight", 2.f);
    static LLCachedControl<F32>  s_ocolHeight(gSavedSettings, "OCOLHeightFogFalloff", 8.f);
    static LLCachedControl<F32>  s_ocolBreakup(gSavedSettings, "OCOLHeightFogBreakup", 1.f);
    static LLCachedControl<F32>  s_ocolSize(gSavedSettings, "OCOLHeightFogBankSize", 158.f);
    static LLCachedControl<F32>  s_ocolRoll(gSavedSettings, "OCOLHeightFogRollHeight", 33.5f);
    static LLCachedControl<F32>  s_ocolWind(gSavedSettings, "OCOLHeightFogWindSpeed", 2.f);
    static LLCachedControl<F32>  s_ocolWindDir(gSavedSettings, "OCOLHeightFogWindDirection", 68.f);

    const bool wasRunning = mRunning;
    mRunning = false;
    if (s_on == 0 || !f.ctx)
    {
        if (!idle() && f.ctx) destroy(f.ctx->device());
        return false;
    }
    if (!f.viewProjRel || !f.camTime || !f.atmos || !f.dslDraw0 || !f.dslDraw1 || f.rw < 2 || f.rh < 2)
        return false;
    mCtx = f.ctx;
    if (!ensurePipelines(f)) return false;
    // Shadowed projectors in the fog: the ray-traced light variant is built (in the
    // background) once the renderer's projector shadows are ray traced; kept afterwards.
    mLocalShadows = s_lsh != 0;
    mShadowDist = llclamp((F32)s_shd, 0.f, 1024.f);
    mShadowPicks = std::clamp<U32>(s_shr, 1u, 4u);
    // The OCCLUSION TERMS (clean_vol_light.comp header: ambient sky visibility with GI, the sun's
    // shadow ray in ray-traced sun mode) ride the same ray-traced light variant: it is also
    // built when either may run on this RT-capable device.
    {
        static LLCachedControl<U32>  s_aocc(gSavedSettings, "RenderVKVolumetricFogAmbientOcclusion", 1);
        static LLCachedControl<F32>  s_alen(gSavedSettings, "RenderVKVolumetricFogAmbientRayLength", 32.f);
        static LLCachedControl<bool> s_sunrt(gSavedSettings, "RenderVKVolumetricFogSunRT", true);
        static LLCachedControl<F32>  s_odist(gSavedSettings, "RenderVKVolumetricFogOcclusionDistance", 128.f);
        mAmbOcc = std::min<U32>(s_aocc, 2u);
        mAmbLen = llclamp((F32)s_alen, 0.5f, 1024.f);
        mSunRt = s_sunrt;
        mOccDist = llclamp((F32)s_odist, 0.f, 1024.f);
    }
    // SHADOWED POINT LIGHTS (clean_vol_light.comp header): point lights join the ray-traced pick.
    // 1 = on except on the low quality tier (its budget: points unshadowed there), 2 = always
    // and attachment lights too. Only while the RT light variant runs (a TLAS this frame).
    {
        static LLCachedControl<U32> s_pts(gSavedSettings, "RenderVKVolumetricFogPointShadows", 1);
        static LLCachedControl<F32> s_fix(gSavedSettings, "RenderVKReSTIRFixtureSize", 1.5f);
        const U32 m = std::min<U32>(s_pts, 2u);
        mPointShadows = (m == 1u && std::min<U32>(s_quality, 2u) == 0u) ? 0u : m;
        mFixMax = llclamp((F32)s_fix, 0.f, 16.f);
    }
    mRtLayoutNow = f.rtLayout;
    const bool rtTermsWanted = mAmbOcc != 0u || (mSunRt && f.shadowMode > 1.5f) || mPointShadows != 0u;
    if (f.rtLayout && mDsl
        && ((mLocalShadows && f.rtWanted) || rtTermsWanted || mLightRtPipe || mRtJob.pending()))
        ensureRtPipeline(f);
    const Tier& t = kTiers[std::min<U32>(s_quality, 2u)];
    if (!ensureImages(cmd, t.w, t.h, t.d)) return false;
    // Volumetric particles (#721): their resources follow the setting (freed when off).
    static_assert(((192u + kPVTile - 1u) / kPVTile) * ((108u + kPVTile - 1u) / kPVTile) <= kPVMaxTiles,
                  "the high tier's screen tiles fit VP_MAX_TILES");
    mPartOn = ensureParticles(cmd, s_vp != 0 && f.partPool && f.partMax > 0, f.partPool) && mPartPipe != VK_NULL_HANDLE;
    mPartMax = mPartOn ? f.partMax : 0u;
    // Camera under water: the water fog owns the view (as the god rays). No passes; the
    // history restarts when the camera comes back up.
    if (f.underwater)
    {
        mHistValid = false;
        return false;
    }

    // ---- this recording's parameters ----
    const float* A = f.atmos;
    const bool atmosOn = A[6 * 4 + 2] > 0.5f;
    const float hazeMie = std::max(A[1 * 4 + 3], 0.f) * std::max(A[4 * 4 + 3], 0.f) * std::max(A[5 * 4 + 3], 0.f);
    const float maxY = A[3 * 4 + 3];
    const float fogFar = llclamp((F32)s_range, 32.f, 512.f);
    const float n = fogFar / 64.f;
    float sigma0 = s_density < 0.f ? kAutoFog * hazeMie : (F32)s_density;
    sigma0 = llclamp(sigma0 + SLWeather::instance().state().fog * kWeatherFog, 0.f, 1.f);
    const float falloff = s_falloff < 0.f ? 1.f / llclamp(maxY * kAutoScaleHeight, 10.f, 200.f)
                                          : llclamp((F32)s_falloff, 0.f, 10.f);
    const float g = s_aniso < 0.f ? kAutoAniso : llclamp((F32)s_aniso, -0.9f, 0.95f);
    // <OCOL> OCOL mist: its own layer (density at the base, base above the water, scale height)
    // in place of the sky-derived one, and the noise that breaks it into rolling banks.
    float baseZ = f.waterHeight + (F32)s_base;
    float fogFalloff = falloff;
    mOcolMist[0] = mOcolMist[1] = mOcolMist[2] = mOcolMist[3] = 0.f;
    if (s_ocol)
    {
        sigma0 = llclamp((F32)s_ocolDensity, 0.f, 1.f);
        baseZ = f.waterHeight + llclamp((F32)s_ocolBase, -20.f, 200.f);
        fogFalloff = 1.f / llclamp((F32)s_ocolHeight, 0.5f, 500.f);
        const float size = llclamp((F32)s_ocolSize, 1.f, 2000.f);
        mOcolMist[1] = llclamp((F32)s_ocolBreakup, 0.f, 1.f);
        if (mOcolMist[1] <= 0.f) mOcolMist[1] = 1e-6f;   // (0 = the layer without noise, still the OCOL layer)
        mOcolMist[2] = size;
        mOcolMist[3] = llclamp((F32)s_ocolRoll, 0.f, 500.f);
        // The noise is global (continuous across regions) and drifts on the wall clock (the same
        // for everyone), wrapped to the 64-cell tile in double so the shader sees small numbers.
        const double tile = 64.0 * (double)size;
        auto wrap = [tile](double v) { const double r = std::fmod(v, tile); return (float)(r < 0.0 ? r + tile : r); };
        const double* origin = sOriginGlobal;
        const double now = LLDate::now().secondsSinceEpoch();
        const double rad = (double)s_ocolWindDir * DEG_TO_RAD;   // compass heading it drifts toward: 0 north, 90 east
        const double wx = std::sin(rad) * (double)s_ocolWind, wy = std::cos(rad) * (double)s_ocolWind;
        mOcolOffsets[0] = wrap(origin[0] - wx * now);
        mOcolOffsets[1] = wrap(origin[1] - wy * now);
        mOcolOffsets[2] = wrap(origin[0] - wx * now * 1.7);   // the wisps drift faster
        mOcolOffsets[3] = wrap(origin[1] - wy * now * 1.7);
    }
    // </OCOL>
    const LLVector3 col = s_color;

    FrameInfo& I = mInfo;
    I = FrameInfo{};
    std::memcpy(I.vp, f.viewProjRel, sizeof(I.vp));
    invert4(f.viewProjRel, I.inv_vp);
    I.eye[0] = f.camTime[0]; I.eye[1] = f.camTime[1]; I.eye[2] = f.camTime[2]; I.eye[3] = 1.f;
    I.grid[0] = (float)t.w; I.grid[1] = (float)t.h; I.grid[2] = (float)t.d; I.grid[3] = fogFar;
    I.slice[0] = n; I.slice[1] = std::log(1.f + fogFar / n);
    I.slice[2] = atmosOn ? hazeMie * kScatterGain * llclamp((F32)s_sun, 0.f, 4.f) : 0.f;
    I.slice[3] = g;
    I.fog[0] = sigma0; I.fog[1] = fogFalloff; I.fog[2] = baseZ;   // <OCOL> (OCOL mist's layer when on)
    I.fog[3] = llclamp((F32)s_amb, 0.f, 4.f);
    I.albedo[0] = llclamp(col.mV[0], 0.f, 1.f); I.albedo[1] = llclamp(col.mV[1], 0.f, 1.f);
    I.albedo[2] = llclamp(col.mV[2], 0.f, 1.f); I.albedo[3] = s_far ? 1.f : 0.f;
    mLocalLights = llclamp((F32)s_ll, 0.f, 8.f);
    I.misc[0] = mLocalLights;
    I.misc[1] = std::max(f.drawDistance, fogFar);
    I.misc[2] = f.waterHeight;
    I.misc[3] = (f.shadowMode > 0.5f && f.shadowMode < 1.5f) ? 1.f : 0.f;
    mLocalVolumes = s_lv;
    mHistWeight = llclamp((F32)s_hist, 0.f, 0.98f);
    mSlot = f.frameSlot & 1u;
    mOffscreen = f.offscreen;
    readParticleStats();   // (#721: this slot's last copy; before mPartPush is refilled below)

    // Camera forward (world): the unjittered NDC centre's ray.
    float fwd[3] = {0.f, 0.f, 1.f};
    {
        const float* m = I.inv_vp;
        const float x = 0.5f * m[8] + m[12], y = 0.5f * m[9] + m[13], z = 0.5f * m[10] + m[14], w = 0.5f * m[11] + m[15];
        if (std::fabs(w) > 1e-12f)
        {
            float v[3] = {x / w, y / w, z / w};
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 1e-12f) { fwd[0] = v[0] / l; fwd[1] = v[1] / l; fwd[2] = v[2] / l; }
        }
    }
    updateTestVolumes(f, fwd);

    // Volumetric particles (#721): this recording's selection / binning parameters.
    float spriteKeep = 1.f;
    if (mPartOn)
    {
        PartPush& P = mPartPush;
        P = PartPush{};
        const float maxDist = llclamp((F32)s_vpDist, 4.f, fogFar);
        spriteKeep = llclamp((F32)s_vpSprite, 0.f, 1.f);
        P.q[0][0] = (float)mPartMax;
        P.q[0][1] = (float)std::min<U32>(s_vp, 2u);
        P.q[0][2] = (float)std::clamp<U32>(s_vpMax, 1u, kPVCapMax);
        P.q[0][3] = llclamp((F32)s_vpMin, 0.f, 16.f);
        P.q[1][0] = maxDist;
        P.q[1][1] = spriteKeep;
        P.q[1][2] = llclamp((F32)s_vpDensity, 0.f, 4.f);
        P.q[1][3] = llclamp((F32)s_vpEmit, 0.f, 8.f);
        P.q[2][0] = (float)((t.w + kPVTile - 1u) / kPVTile);
        P.q[2][1] = (float)((t.h + kPVTile - 1u) / kPVTile);
        // Froxel size per metre of view depth (vertical): 2 tan(fovy / 2) / H, tan(fovy / 2)
        // from the unjittered inverse (the NDC (0, 1) ray against the forward one).
        {
            const float* m = I.inv_vp;
            const float x = m[4] + 0.5f * m[8] + m[12], y = m[5] + 0.5f * m[9] + m[13];
            const float z = m[6] + 0.5f * m[10] + m[14], w = m[7] + 0.5f * m[11] + m[15];
            float tanH = 0.6f;
            if (std::fabs(w) > 1e-12f)
            {
                const float d[3] = {x / w, y / w, z / w};
                const float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                if (along > 1e-6f)
                {
                    const float u[3] = {d[0] / along - fwd[0], d[1] / along - fwd[1], d[2] / along - fwd[2]};
                    tanH = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                }
            }
            P.q[2][3] = 2.f * llclamp(tanH, 0.05f, 4.f) / (float)t.h;
        }
        P.q[3][0] = fwd[0]; P.q[3][1] = fwd[1]; P.q[3][2] = fwd[2];
        mPartNoise = llclamp((F32)s_vpNoise, 0.f, 1.f);
    }
    // vol_eye.w 2 = the lit particle VS fades injected sprites by their glow.z this frame.
    if (mPartOn && f.partSpriteFade && spriteKeep < 1.f) I.eye[3] = 2.f;

    // History: reset on the first / re-created grid, a camera cut, a jump, a range change,
    // and every snapshot (no history: it then also leaves the live one invalid).
    const double dx = (double)f.camTime[0] - mPrevEye[0];
    const double dy = (double)f.camTime[1] - mPrevEye[1];
    const double dz = (double)f.camTime[2] - mPrevEye[2];
    const bool jump = dx * dx + dy * dy + dz * dz > kCamJump * kCamJump;
    mReset = !mHistValid || !wasRunning || f.cameraCut || jump || f.offscreen || fogFar != mPrevFar;
    mPrevFar = fogFar;
    mHistCur ^= 1u;

    // Push (shared by the four passes; p0 / p1 set per pass at record time).
    mPush = VolPush{};
    if (!mReset)
    {
        // Reprojection: this frame's camera-relative world -> last frame's clip =
        // last vol_vp x translate(eye - last eye), in double.
        const double dl[3] = {dx, dy, dz};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                double v = (double)mPrevVP[c * 4 + r];
                if (c == 3)
                    for (int k = 0; k < 3; ++k) v += (double)mPrevVP[k * 4 + r] * dl[k];
                mPush.reproj[c * 4 + r] = (float)v;
            }
    }
    // #932: mFrame holds on offscreen recordings; a supersampled shot's recordings are
    // averaged, so each takes the next phase by its sample index (the live clock is untouched).
    const uint32_t nf = mFrame + f.snapSample;
    if (!f.offscreen || f.snapSample)
    {
        // Halton (2, 3, 5) over 16 frames, rotated every frame (never tied to TAA).
        const uint32_t k = (nf % 16u) + 1u;
        mPush.jitter[0] = halton(k, 2) - 0.5f;
        mPush.jitter[1] = halton(k, 3) - 0.5f;
        mPush.jitter[2] = halton(k, 5) - 0.5f;
    }
    mPush.jitter[3] = (float)(nf % 64u);
    mPush.fwd[0] = fwd[0]; mPush.fwd[1] = fwd[1]; mPush.fwd[2] = fwd[2];
    mPush.fwd[3] = (float)std::fmod((double)f.camTime[3], 4096.0);   // noise scroll time (bounded)

    // Apply push: render extent + this frame's jitter (the depth was rasterized jittered).
    mApplyPush = ApplyPush{};
    mApplyPush.dims[0] = (float)f.rw; mApplyPush.dims[1] = (float)f.rh;
    mApplyPush.dims[2] = 1.f / (float)f.rw; mApplyPush.dims[3] = 1.f / (float)f.rh;

    // Volume list for this slot: the CPU-side volumes (test volumes now; #720 marked prims
    // later). GPU appenders add after this (clean_vol_inject.comp header).
    {
        uint8_t* dst = (uint8_t*)mList[mSlot].mapped;
        const uint32_t cnt = mLocalVolumes ? std::min<uint32_t>((uint32_t)mTestVols.size(), kVolCap) : 0u;
        const uint32_t hdr[4] = {cnt, kVolCap, 0u, 0u};
        std::memcpy(dst, hdr, sizeof(hdr));
        if (cnt) std::memcpy(dst + 16, mTestVols.data(), cnt * sizeof(VolPrimGPU));
    }

    if (!f.offscreen)
    {
        std::memcpy(mPrevVP, f.viewProjRel, sizeof(mPrevVP));
        mPrevEye[0] = f.camTime[0]; mPrevEye[1] = f.camTime[1]; mPrevEye[2] = f.camTime[2];
        ++mFrame;
    }
    const uint32_t tier = std::min<U32>(s_quality, 2u);
    if (tier != mLogTier)
    {
        LL_INFOS("VKVolFog") << "volumetric fog: " << t.w << "x" << t.h << "x" << t.d << " froxels, far " << fogFar
                             << " m, fog sigma0 " << sigma0 << " /m" << (s_density < 0.f ? " (EEP auto)" : "")
                             << ", falloff " << falloff << " /m, haze sun scatter " << I.slice[2] << " /m" << LL_ENDL;
        mLogTier = tier;
    }
    mRunning = true;
    return true;
}

void LLCleanVolFogVK::fillFrameInfo(FrameInfo& out) const
{
    if (mRunning) out = mInfo;
    else out = FrameInfo{};
}

void LLCleanVolFogVK::recordComputeImpl(VkCommandBuffer cmd, float shadowMode, uint32_t lightCount,
                                        const VkDescriptorSet draw01[2], uint32_t projMode, VkDescriptorSet rtSet,
                                        bool partLive, bool giNow, const std::function<void(const char*)>& mark)
{
    if (!mRunning || !mInjectPipe || !mInteg.image || !draw01[0] || !draw01[1]) return;
    VkDescriptorSet own = mDset[mHistCur][mSlot];
    const uint32_t gx = (mW + 3u) / 4u, gy = (mH + 3u) / 4u, gz = (mD + 3u) / 4u;
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // Inputs -> these passes: the frame-info / light / cluster uploads and computes, the
    // cascades' depth writes (already in their read layout), last frame's history writes;
    // and WAR against last frame's readers of the grid images (temporal / integration /
    // apply / transparent FS).
    memBarrier(cmd,
               VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | C | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                   | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);

    // {draw 0, draw 1, own}: one layout for all four passes (the bindless set is only read by
    // the light pass's projector gobos).
    const VkDescriptorSet sets[3] = {draw01[0], draw01[1], own};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPassLayout, 0, 3, sets, 0, nullptr);
    auto push = [&](float a, float b, float c, float d)
    {
        mPush.p0[0] = a; mPush.p0[1] = b; mPush.p0[2] = c; mPush.p0[3] = d;
        VK_PUSH_BUDGET_CHECK(VolPush);
        vkCmdPushConstants(cmd, mPassLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolPush), &mPush);
    };

    // 0. Volumetric particles (#721): clear the per-frame prefix (header, histogram, tile
    //    counts; WAR against last frame's inject / copy), classify, bin, copy the header for
    //    the stats. Skipped when the pool is not current this frame (particle compute off /
    //    particles hidden): the inject then sees no particles and the VS no fade (vol_eye.w
    //    names the replacement, but no particle sprite is drawn then either).
    const bool partNow = mPartOn && partLive && mPartPipe != VK_NULL_HANDLE && mPV.buffer && mEmit.view && mPartMax > 0
                      && mSetPool == mPartPool && mSetPV == mPV.buffer;
    if (partNow)
    {
        memBarrier(cmd, C | VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, mPV.buffer, 0, kPVClearBytes, 0u);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, C, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPartPipe);
        const uint32_t groups = (mPartMax + 63u) / 64u;
        for (int pass = 0; pass < 2; ++pass)
        {
            mPartPush.q[2][2] = (float)pass;
            static_assert(sizeof(PartPush) == sizeof(VolPush), "one push range for every fog pass");
            VK_PUSH_BUDGET_CHECK(PartPush);
            vkCmdPushConstants(cmd, mPassLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PartPush), &mPartPush);
            vkCmdDispatch(cmd, groups, 1, 1);
            memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C | VK_PIPELINE_STAGE_2_COPY_BIT,
                       RW | VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        if (mPVRb[mSlot].buffer)
        {
            VkBufferCopy r{0, 0, 16};
            vkCmdCopyBuffer(cmd, mPV.buffer, mPVRb[mSlot].buffer, 1, &r);
            mPVRbValid[mSlot] = true;
        }
        mark("c_vol_part");
    }

    // 1. Inject. #954 the precipitation medium (clean_weather_veil.glsl, ssr.weather[16]): its motion
    //    field moves with the gusty wind and the mean fall (p1; rain ~7 m/s, snow ~1 m/s by share),
    //    its strength RenderVKVolumetricFogPrecipMotion (p0.w).
    {
        static LLCachedControl<F32> s_motion(gSavedSettings, "RenderVKVolumetricFogPrecipMotion", 1.f);
        const SLWeather::State& st = SLWeather::instance().state();
        const float precip = st.rain + st.snow;
        const float share = precip > 1e-4f ? st.snow / precip : (st.blowing > 0.f ? 1.f : 0.f);
        mPush.p1[0] = st.wind[0]; mPush.p1[1] = st.wind[1];
        mPush.p1[2] = 7.f + (1.f - 7.f) * share;
        mPush.p1[3] = 0.f;
        mPrecipMotion = llclamp((F32)s_motion, 0.f, 1.f);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mInjectPipe);
    // <OCOL> the mist rides the inject pass's push: reproj[0] the noise offsets (banks xy, wisps zw),
    // reproj[1] xy the bank size and roll height, z its breakup (> 0: on; p0.w is #954's precipitation motion). The inject
    // reads no reprojection (it is the temporal pass's), so it is borrowed and put back, as the
    // light pass does with its ambient terms.
    float keepReproj[8];
    std::memcpy(keepReproj, mPush.reproj, sizeof(keepReproj));
    std::memcpy(mPush.reproj, mOcolOffsets, sizeof(mOcolOffsets));
    mPush.reproj[4] = mOcolMist[2]; mPush.reproj[5] = mOcolMist[3]; mPush.reproj[6] = mOcolMist[1]; mPush.reproj[7] = 0.f;
    push(mLocalVolumes ? 1.f : 0.f, partNow ? 1.f : 0.f, partNow ? mPartNoise : 0.f, mPrecipMotion);
    std::memcpy(mPush.reproj, keepReproj, sizeof(keepReproj));
    // </OCOL>
    vkCmdDispatch(cmd, gx, gy, gz);
    mark("c_vol_inject");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);

    // 2. Light (in place). Shadowed projectors (p0.w): the records say map / ray traced; the
    // RT variant only on a ray-traced projector frame with a valid TLAS (rtSet) and the
    // variant built for this RT layout -- else the plain pass (RT records unshadowed).
    // OCCLUSION TERMS (the same RT variant; clean_vol_light.comp header): the ambient's per-cell sky
    // visibility while GI runs (RenderVKVolumetricFogAmbientOcclusion 1) or whenever the TLAS is
    // valid (2); the sun / moon shadow ray in ray-traced sun mode (no cascades to tap).
    const bool cascades = shadowMode > 0.5f && shadowMode < 1.5f;
    const bool shadows = mLocalShadows && mLocalLights > 0.f;
    const bool rtReady = rtSet && mLightRtPipe && mPassRtLayout && mRtLayoutNow && mPipeRtDsl[3] == mRtLayoutNow;
    const bool projRt = shadows && projMode == 2u;
    const bool ambRt = mAmbOcc == 2u || (mAmbOcc == 1u && giNow);
    const bool sunRt = mSunRt && shadowMode > 1.5f;
    // shadowed point lights: any local light this frame (the shader skips projectors / attachments)
    const bool pointRt = mPointShadows != 0u && mLocalLights > 0.f && lightCount > 0u;
    const bool rt = rtReady && (projRt || ambRt || sunRt || pointRt);
    const uint32_t terms = rt ? ((projRt ? 1u : 0u) | (ambRt ? 2u : 0u) | (sunRt ? 4u : 0u) | (pointRt ? 8u : 0u)) : 0u;
    if (rt != mLastRt || terms != mLastRtTerms)
    {
        LL_INFOS("VKVolFog") << "volumetric fog: ray-traced light pass " << (rt ? "ON" : "off")
                             << " (projector shadows " << ((terms & 1u) ? "on" : "off")
                             << ", ambient sky visibility " << ((terms & 2u) ? "on" : "off")
                             << ", sun shadow ray " << ((terms & 4u) ? "on" : "off")
                             << ", point light shadows " << ((terms & 8u) ? (mPointShadows >= 2u ? "on + attachments" : "on") : "off")
                             << ")" << LL_ENDL;
        mLastRt = rt;
        mLastRtTerms = terms;
    }
    mPush.p1[0] = mShadowDist;
    mPush.p1[1] = (float)mShadowPicks;
    mPush.p1[2] = (float)(mFrame & 0xFFFFu);
    mPush.p1[3] = partNow ? 1.f : 0.f;   // #721: add the particles' emission (uEmit)
    const float p0w = shadows ? 1.f : 0.f;
    if (rt)
    {
        // Sets 0..2 stay bound (mPassRtLayout is compatible with mPassLayout through set 2,
        // same push range): only the RT consumer set goes on top.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLightRtPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPassRtLayout, 3, 1, &rtSet, 0, nullptr);
        mPush.p0[0] = cascades ? 1.f : 0.f; mPush.p0[1] = (float)lightCount;
        mPush.p0[2] = mLocalLights > 0.f ? 1.f : 0.f; mPush.p0[3] = p0w;
        // reproj's first column carries the occlusion terms in this pass, its second the point-light
        // shadows (the temporal pass's reprojection is restored right after the push)
        float keep[8];
        std::memcpy(keep, mPush.reproj, sizeof(keep));
        mPush.reproj[0] = ambRt ? 1.f : 0.f;
        mPush.reproj[1] = mAmbLen;
        mPush.reproj[2] = sunRt ? 1.f : 0.f;
        mPush.reproj[3] = mOccDist;
        mPush.reproj[4] = pointRt ? (float)mPointShadows : 0.f;
        mPush.reproj[5] = mFixMax;
        mPush.reproj[6] = 0.f;
        mPush.reproj[7] = 0.f;
        VK_PUSH_BUDGET_CHECK(VolPush);
        vkCmdPushConstants(cmd, mPassRtLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolPush), &mPush);
        std::memcpy(mPush.reproj, keep, sizeof(keep));
    }
    else
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLightPipe);
        push(cascades ? 1.f : 0.f, (float)lightCount, mLocalLights > 0.f ? 1.f : 0.f, p0w);
    }
    vkCmdDispatch(cmd, gx, gy, gz);
    mark(rt ? "c_vol_light_rt" : "c_vol_light");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 3. Temporal.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTemporalPipe);
    push(mOffscreen ? 0.f : mHistWeight, mReset ? 1.f : 0.f, kClampWiden, 0.f);
    vkCmdDispatch(cmd, gx, gy, gz);
    mark("c_vol_temporal");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);

    // 4. Integrate (one invocation per column).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipe);
    push(0.f, 0.f, 0.f, 0.f);
    vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1);
    mark("c_vol_integrate");

    // The integrated volume -> the apply / transparent FS (and any compute consumer); the
    // particles' sprite fades (pool glow.z, #721) -> the particle VS and next frame's particle
    // compute. On the async queue the join semaphore carries it; the sanitizer drops the
    // graphics stages there.
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | C,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                   | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // A snapshot leaves no usable history for the live view.
    mHistValid = !mOffscreen;
}

void LLCleanVolFogVK::recordApply(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0)
{
    if (!mRunning || !mApplyPipe || !dsetDraw0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mApplyPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mApplyLayout, 0, 1, &dsetDraw0, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(ApplyPush);
    vkCmdPushConstants(cmd, mApplyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ApplyPush), &mApplyPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanVolFogVK::destroy(VkDevice dev)
{
    if (!dev) return;
    mJob.wait();
    for (VkPipeline& p : mStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    destroyRtPipeline(dev);   // (references mDsl: before it goes)
    mLastRt = false;
    vkDeviceWaitIdle(dev);
    const bool had = mInteg.image != VK_NULL_HANDLE;
    destroyImages();
    mBindingDirty = mBindingDirty || had;
    for (GpuBuffer& b : mList) if (b.buffer && mCtx) mCtx->destroyBuffer(b);
    destroyParticles();
    mPartPool = mSetPool = mSetPV = VK_NULL_HANDLE;
    mSetEmit = VK_NULL_HANDLE;
    mPartMax = 0;
    mPartRetryFrame = 0;
    mPartLastOverCap = false;
    for (VkPipeline* p : {&mInjectPipe, &mLightPipe, &mTemporalPipe, &mIntegratePipe, &mApplyPipe, &mPartPipe})
    {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (mPassLayout) vkDestroyPipelineLayout(dev, mPassLayout, nullptr);
    if (mApplyLayout) vkDestroyPipelineLayout(dev, mApplyLayout, nullptr);
    mPassLayout = mApplyLayout = VK_NULL_HANDLE;
    mPool.destroy();
    for (auto& row : mDset) for (VkDescriptorSet& s : row) s = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mSampler = VK_NULL_HANDLE;
    mPipeDsl[0] = mPipeDsl[1] = VK_NULL_HANDLE;
    for (VkFormat& fm : mPipeFormats) fm = VK_FORMAT_UNDEFINED;
    mPipeFailed = false;
    mRunning = false;
    mLogTier = ~0u;
    mTestCount = 0;
    mTestVols.clear();
}
