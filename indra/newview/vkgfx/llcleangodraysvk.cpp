/**
 * @file llcleangodraysvk.cpp
 * @brief God rays / volumetric sun shafts (#208): see llcleangodraysvk.h and the
 *        shaders clean_godrays.comp / clean_godrays_composite.frag.
 */
#include "linden_common.h"

#include "llcleangodraysvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Scattering gain over the physical haze coefficient. WindLight's haze extinction is
// ~1e-4 /m at the default sky (haze 0.7 x density 0.00018 x distance 0.8): a fully lit
// 256 m march toward the sun then adds only ~2% of the beam -- invisible as shafts.
// The gain lifts in-scatter (not extinction) to where shafts read at Intensity 1,
// comparable to the haze's own additive term.
constexpr float kScatterGain = 8.f;

// Tiers (RenderVKGodRays 1-3): resolution scale per axis and steps per ray. The march
// is a stochastic estimate that the temporal accumulation (clean_godrays_temporal.comp)
// integrates over ~12 frames, with the start offset rotated per frame, so few steps per
// frame still converge to many effective steps.
struct Tier { float scale; uint32_t steps; };
// Cascades: one bilinear compare tap per step.
constexpr Tier kTiers[4] = {{0.f, 0u}, {0.25f, 12u}, {0.5f, 16u}, {0.5f, 24u}};
// Ray-traced sun: one shadow ray per step (any-hit alpha tests through foliage), ~0.4 ns
// each on an RTX 5090 in a dense foliage sim -- the old half-res x 24-step High was
// ~50 M rays, ~20 ms at 4K. Quarter resolution with 4 / 6 / 8 rays per texel per frame:
// 4K = 960x540 texels -> ~2.1 / 3.1 / 4.1 M rays (~0.8 / 1.2 / 1.6 ms before the
// opaque-only cut past RenderVKGodRaysRTAlphaDist).
constexpr Tier kRtTiers[4] = {{0.f, 0u}, {0.25f, 4u}, {0.25f, 6u}, {0.25f, 8u}};

// Temporal accumulation (clean_godrays_temporal.comp): history weight per frame (~12
// frames to converge; the variance clamp bounds the trail), and the camera move
// between two frames past which the history is dropped (teleport / origin shift /
// region crossing the renderer did not flag -- agent-local coordinates jump).
constexpr float  kHistWeight = 0.92f;
constexpr double kCamJump    = 32.0;

VkShaderModule grModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);   // (names the batch report / straggler / crash lines)
    return m;
}

// Inverse of a column-major 4x4 in double (the camera-relative view-projection: its
// float inverse is precise; the double keeps it so at grazing projections).
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
} // namespace

// FIRST USE OFF THE RENDER THREAD: the set layouts, pool and pipeline layouts are made
// here (cheap); the five pipelines go to an async batch (vk_pipeline.h PipelineJobSlot,
// staged in mStage) and land on a later call -- false (the effect skips its frames) until
// then. A failed build is latched for these formats / layout (no rebuild every frame).
bool LLCleanGodRaysVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mTempPipe = mStage[0]; mDnTempPipe = mStage[1]; mAtPipe = mStage[2]; mMarchPipe = mStage[3]; mCompPipe = mStage[4];
        for (VkPipeline& p : mStage) p = VK_NULL_HANDLE;
        if (!mDnTempPipe || !mAtPipe)
            LL_WARNS("VKGodRays") << "god rays denoiser pipelines failed to build; RenderVKGodRaysDenoise falls back to the plain temporal pass" << LL_ENDL;
        if (!mMarchPipe || !mCompPipe || !mTempPipe)
        {
            LL_WARNS("VKGodRays") << "god rays pipelines failed to build; effect disabled" << LL_ENDL;
            VkFormat ff[4] = {mCompFormats[0], mCompFormats[1], mCompFormats[2], mCompFormats[3]};
            const VkDescriptorSetLayout fd = mPipeDslDraw0;
            destroy(dev);
            for (int i = 0; i < 4; ++i) mFailFormats[i] = ff[i];
            mFailDsl = fd;
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VKGodRays") << "god rays pipelines built (background)" << LL_ENDL;
    }
    const bool fmtChanged = mCompPipe && (mCompFormats[0] != f.hdrFormat || mCompFormats[1] != f.glowFormat
                                          || mCompFormats[2] != f.depthFormat || mCompFormats[3] != f.reactFormat);
    if (mMarchPipe && mCompPipe && !fmtChanged && mPipeDslDraw0 == f.dslDraw0) return true;
    if (mPipeFailed && mFailFormats[0] == f.hdrFormat && mFailFormats[1] == f.glowFormat && mFailFormats[2] == f.depthFormat
        && mFailFormats[3] == f.reactFormat && mFailDsl == f.dslDraw0)
        return false;
    mPipeFailed = false;
    if (mMarchPipe || mCompPipe || mDsl)
    {
        // A pass format / set 0 layout change (rare): rebuild from scratch.
        destroy(dev);
    }
    mCtx = f.ctx;
    PipelineBatchScope batchScope(dev);   // (the pipelines below: queued, flushed async at the end)

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));

    VkDescriptorSetLayoutBinding b[5]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // #657 denoiser: 3 the march's Lmax output (storage), 4 Lmax for the composite's
    // reactive (sampled). Placeholders (mOut) without the denoiser.
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 5; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslGodRays"));
    // Temporal: 0 march output, 1 history (both texelFetch), 2 history out (storage).
    VkDescriptorSetLayoutBinding tb[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        tb[i].binding = i; tb[i].descriptorCount = 1; tb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tb[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo tlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tlci.bindingCount = 3; tlci.pBindings = tb;
    VK_CHECK(vkdCreateSetLayout(dev, &tlci, &mTempDsl, "mDslGodRaysTemporal"));
    // Denoiser temporal (#657): 0 march, 1 last acc, 2 last meta (texelFetch), 3 acc out,
    // 4 meta out (storage).
    VkDescriptorSetLayoutBinding db[5]{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        db[i].binding = i; db[i].descriptorCount = 1; db[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        db[i].descriptorType = i >= 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 5; dlci.pBindings = db;
    VK_CHECK(vkdCreateSetLayout(dev, &dlci, &mDnTempDsl, "mDslGodRaysDnTemporal"));
    // Denoiser a-trous (#657): 0 acc, 1 meta, 2 / 3 scratch, 4 Lmax, 5 march target (storage).
    VkDescriptorSetLayoutBinding ab[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        ab[i].binding = i; ab[i].descriptorCount = 1; ab[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ab[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo alci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    alci.bindingCount = 6; alci.pBindings = ab;
    VK_CHECK(vkdCreateSetLayout(dev, &alci, &mAtDsl, "mDslGodRaysAtrous"));
    VK_CHECK(mPool.create(dev, "godrays", {{mDsl, 3}, {mTempDsl, 2}, {mDnTempDsl, 2}, {mAtDsl, 2}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mCompDset[0]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mCompDset[1]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mTempDsl, &mTempDset[0]), { destroy(dev); return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mTempDsl, &mTempDset[1]), { destroy(dev); return false; });
    for (int i = 0; i < 2; ++i)
    {
        VKD_ALLOC_SOFT(mPool.allocate(mDnTempDsl, &mDnTempDset[i]), { destroy(dev); return false; });
        VKD_ALLOC_SOFT(mPool.allocate(mAtDsl, &mAtDset[i]), { destroy(dev); return false; });
    }

    // Temporal accumulation: {mTempDsl}, 128 B push.
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TempPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mTempDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mTempLayout, "mGodRaysTemporalLayout"));
        VkShaderModule cm = grModule(dev, kWorldGodRaysTemporalSpv, kWorldGodRaysTemporalSpv_bytes, "kWorldGodRaysTemporalSpv");
        createComputePipelineInto(dev, cm, mTempLayout, 0, nullptr, &mStage[0], "god rays temporal");
        destroyShaderModuleDeferred(dev, cm, nullptr);
    }

    // Denoiser (#657): temporal {mDnTempDsl} (128 B push, TempPush's layout) and a-trous
    // {mAtDsl} (32 B push). A failed build only disables the denoiser (the old temporal
    // pass stays): see the availability test in recordMarch.
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TempPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDnTempDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mDnTempLayout, "mGodRaysDnTemporalLayout"));
        VkShaderModule cm = grModule(dev, kWorldGodRaysDnTemporalSpv, kWorldGodRaysDnTemporalSpv_bytes, "kWorldGodRaysDnTemporalSpv");
        createComputePipelineInto(dev, cm, mDnTempLayout, 0, nullptr, &mStage[1], "god rays denoise temporal");
        destroyShaderModuleDeferred(dev, cm, nullptr);
    }
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AtPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mAtDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mAtLayout, "mGodRaysAtrousLayout"));
        VkShaderModule cm = grModule(dev, kWorldGodRaysAtrousSpv, kWorldGodRaysAtrousSpv_bytes, "kWorldGodRaysAtrousSpv");
        createComputePipelineInto(dev, cm, mAtLayout, 0, nullptr, &mStage[2], "god rays a-trous");
        destroyShaderModuleDeferred(dev, cm, nullptr);
    }

    // March: {draw set 0 (frame info, cascades), own set}, 128 B push.
    {
        const VkDescriptorSetLayout sets[2] = {f.dslDraw0, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mMarchLayout, "mGodRaysMarchLayout"));
        VkShaderModule cm = grModule(dev, kWorldGodRaysSpv, kWorldGodRaysSpv_bytes, "kWorldGodRaysSpv");
        createComputePipelineInto(dev, cm, mMarchLayout, 0, nullptr, &mStage[3], "god rays march");
        destroyShaderModuleDeferred(dev, cm, nullptr);
    }
    // Composite: {own set}, fragment push; pass 2's attachment set (HDR + glow masked
    // off + read-only depth, no test), additive.
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mCompLayout, "mGodRaysCompLayout"));
        VkShaderModule vs = grModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = grModule(dev, kWorldGodRaysCompFragSpv, kWorldGodRaysCompFragSpv_bytes, "kWorldGodRaysCompFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mCompLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowWrite = false;
        // Pass 2's reactive / water mask: R only, MAX-blended -- the denoiser's moving-caster
        // shading change (#657); every other case outputs an exact 0 (a no-op under MAX).
        d.reactFormat = f.reactFormat; d.reactWriteMask = VK_COLOR_COMPONENT_R_BIT;
        d.depthFormat = f.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.additive = true;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[4], "god rays composite");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
    }
    mCompFormats[0] = f.hdrFormat; mCompFormats[1] = f.glowFormat; mCompFormats[2] = f.depthFormat;
    mCompFormats[3] = f.reactFormat;
    mPipeDslDraw0 = f.dslDraw0;
    PipelineJobOpts o;
    o.what = "god rays";
    mJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKGodRays") << "god rays pipelines compiling in the background (the effect starts when they land)" << LL_ENDL;
    return false;
}

// The ray-traced march (GODRAYS_RT). Separate from ensurePipelines: it needs the RT
// consumer layout, which exists only while ray tracing is up, and is built lazily on the
// first traced frame. Compute only, so it can be (re)built at any time.
bool LLCleanGodRaysVK::ensureRtPipeline(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    const VkDescriptorSetLayout want[3] = {f.dslDraw0, f.dslDraw1, f.rtLayout};
    const bool same = mPipeRtDsl[0] == want[0] && mPipeRtDsl[1] == want[1] && mPipeRtDsl[2] == want[2];
    if (mRtJob.pending())
    {
        if (!mRtJob.poll()) return false;   // still compiling (the cascade-less RT frame has no god rays meanwhile)
        mMarchRtPipe = mRtStage[0]; mMarchRtDnPipe = mRtStage[1];
        mRtStage[0] = mRtStage[1] = VK_NULL_HANDLE;
        if (mMarchRtPipe && !mMarchRtDnPipe)
            LL_WARNS("VKGodRays") << "ray-traced god rays denoiser march failed to build; the RT march keeps the plain temporal pass" << LL_ENDL;
        if (!mMarchRtPipe)
        {
            LL_WARNS("VKGodRays") << "ray-traced god rays pipeline failed to build; RT sun mode has no god rays" << LL_ENDL;
            if (mMarchRtDnPipe) { vkDestroyPipeline(dev, mMarchRtDnPipe, nullptr); mMarchRtDnPipe = VK_NULL_HANDLE; }
            if (mMarchRtLayout) vkDestroyPipelineLayout(dev, mMarchRtLayout, nullptr);
            mMarchRtLayout = VK_NULL_HANDLE;
            mRtFailed = true;
        }
        else
            LL_INFOS("VKGodRays") << "ray-traced god rays pipeline built (background)" << LL_ENDL;
    }
    if (mMarchRtPipe && same) return true;
    if (mRtFailed && same) return false;
    if (mMarchRtPipe || mMarchRtLayout) destroyRtPipeline(dev);
    mRtFailed = false;
    mPipeRtDsl[0] = want[0]; mPipeRtDsl[1] = want[1]; mPipeRtDsl[2] = want[2];
    PipelineBatchScope batchScope(dev);   // (queued, flushed async at the end: ~5 s cold on a 5090)

    const VkDescriptorSetLayout sets[4] = {f.dslDraw0, f.dslDraw1, f.rtLayout, mDsl};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4; pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mMarchRtLayout, "mGodRaysMarchRtLayout"));
    VkShaderModule cm = grModule(dev, kWorldGodRaysRtSpv, kWorldGodRaysRtSpv_bytes, "kWorldGodRaysRtSpv");
    createComputePipelineInto(dev, cm, mMarchRtLayout, 0, nullptr, &mRtStage[0], "god rays march (ray traced)");
    destroyShaderModuleDeferred(dev, cm, nullptr);
    // The denoiser's variant (#657): + moving-caster tracking on every shadow ray. Same
    // layout; a failed build only disables the denoiser for the RT march.
    VkShaderModule dm = grModule(dev, kWorldGodRaysRtDnSpv, kWorldGodRaysRtDnSpv_bytes, "kWorldGodRaysRtDnSpv");
    createComputePipelineInto(dev, dm, mMarchRtLayout, 0, nullptr, &mRtStage[1], "god rays march (ray traced, denoiser)");
    destroyShaderModuleDeferred(dev, dm, nullptr);
    PipelineJobOpts o;
    o.what = "god rays (ray traced)";
    mRtJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKGodRays") << "ray-traced god rays pipelines compiling in the background" << LL_ENDL;
    return false;
}

bool LLCleanGodRaysVK::warm(const Frame& f)
{
    if (!f.ctx || !f.dslDraw0) return false;
    mCtx = f.ctx;
    ensurePipelines(f);
    // The RT march needs mDsl (made by ensurePipelines above, synchronously).
    if (f.rtLayout && f.dslDraw1 && mDsl) ensureRtPipeline(f);
    return pipelinesPending();
}

void LLCleanGodRaysVK::destroyRtPipeline(VkDevice dev)
{
    // A build in flight writes the stage and references the layout: wait for it, free its result.
    mRtJob.wait();
    for (VkPipeline& p : mRtStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mMarchRtPipe || mMarchRtLayout || mMarchRtDnPipe) vkDeviceWaitIdle(dev);   // may be in flight
    if (mMarchRtPipe) vkDestroyPipeline(dev, mMarchRtPipe, nullptr);
    if (mMarchRtDnPipe) vkDestroyPipeline(dev, mMarchRtDnPipe, nullptr);
    mMarchRtDnPipe = VK_NULL_HANDLE;
    if (mMarchRtLayout) vkDestroyPipelineLayout(dev, mMarchRtLayout, nullptr);
    mMarchRtPipe = VK_NULL_HANDLE;
    mMarchRtLayout = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout& l : mPipeRtDsl) l = VK_NULL_HANDLE;
    mRtFailed = false;
}

void LLCleanGodRaysVK::destroyTarget()
{
    if (mOut.image && mCtx) mCtx->destroyImage(mOut);
    mOut = GpuImage{};
    mW = mH = 0;
    mBoundDepthView = VK_NULL_HANDLE;
    mBoundDepthGen = 0;
}

void LLCleanGodRaysVK::ensureTarget(VkCommandBuffer cmd, const Frame& f, uint32_t w, uint32_t h)
{
    VkMemScope memcat(VKMEM_TARGET);   // #407
    VkDevice dev = f.ctx->device();
    const bool resize = !mOut.image || w != mW || h != mH;
    if (!resize && mBoundDepthView == f.depthView && mBoundDepthGen == f.depthGen) return;
    vkDeviceWaitIdle(dev);   // the set / image may be in use by frames in flight
    if (resize)
    {
        destroyTarget();
        mOut = mCtx->createImage2D(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        mW = w; mH = h;
        // GENERAL for good (compute writes, fragment reads).
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.image = mOut.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    mBoundDepthView = f.depthView;
    mBoundDepthGen = f.depthGen;
    writeSets();
}

void LLCleanGodRaysVK::writeSets()
{
    if (!mCtx || !mDset || !mOut.image || !mBoundDepthView) return;
    VkDevice dev = mCtx->device();
    // Composite sets: 0 depth, 1 march output (storage; the march's), 2 what the
    // composite upsamples -- the raw march (mDset) or a history parity (mCompDset).
    const bool hist = mHist[0].image && mHist[1].image;
    // #657 denoiser images (with the history): Lmax at 3 / 4 and the denoiser's own sets.
    const bool dn = hist && mMeta[0].image && mMeta[1].image && mLmax.image;
    const VkImageView lmax = dn ? mLmax.view : mOut.view;   // placeholder: never used then
    const VkDescriptorSet comp[3] = {mDset, mCompDset[0], mCompDset[1]};
    for (int s = 0; s < (hist ? 3 : 1); ++s)
    {
        const VkImageView src = s == 0 ? mOut.view : mHist[s - 1].view;
        VkDescriptorImageInfo ii[5] = {{mSampler, mBoundDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                       {VK_NULL_HANDLE, mOut.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {mSampler, src, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, lmax, VK_IMAGE_LAYOUT_GENERAL},
                                       {mSampler, lmax, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[5]{};
        for (int i = 0; i < 5; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = comp[s]; wr[i].dstBinding = (uint32_t)i;
            wr[i].descriptorCount = 1; wr[i].pImageInfo = &ii[i];
            wr[i].descriptorType = (i == 1 || i == 3) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        vkUpdateDescriptorSets(dev, 5, wr, 0, nullptr);
    }
    if (!hist) return;
    if (dn)
    {
        // Denoiser temporal, parity p: march + last frame's acc / meta (p ^ 1) -> acc /
        // meta p. A-trous, parity p: acc / meta p, scratch = last frame's pair (p ^ 1,
        // dead once the temporal pass read it), Lmax, the march target (finalize).
        for (uint32_t p = 0; p < 2; ++p)
        {
            const uint32_t q = p ^ 1u;
            VkDescriptorImageInfo ti[5] = {{mSampler, mOut.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {mSampler, mHist[q].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {mSampler, mMeta[q].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mHist[p].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mMeta[p].view, VK_IMAGE_LAYOUT_GENERAL}};
            VkDescriptorImageInfo ai[6] = {{VK_NULL_HANDLE, mHist[p].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mMeta[p].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mHist[q].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mMeta[q].view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mLmax.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, mOut.view, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet wr[11]{};
            for (int i = 0; i < 11; ++i)
            {
                const bool t = i < 5;
                const int b = t ? i : i - 5;
                wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[i].dstSet = t ? mDnTempDset[p] : mAtDset[p];
                wr[i].dstBinding = (uint32_t)b;
                wr[i].descriptorCount = 1;
                wr[i].pImageInfo = t ? &ti[b] : &ai[b];
                wr[i].descriptorType = (t && b < 3) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            vkUpdateDescriptorSets(dev, 11, wr, 0, nullptr);
        }
    }
    // Temporal sets: parity p reads the march + mHist[p ^ 1] (last frame), writes mHist[p].
    for (uint32_t p = 0; p < 2; ++p)
    {
        VkDescriptorImageInfo ii[3] = {{mSampler, mOut.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {mSampler, mHist[p ^ 1u].view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, mHist[p].view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[3]{};
        for (int i = 0; i < 3; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = mTempDset[p]; wr[i].dstBinding = (uint32_t)i;
            wr[i].descriptorCount = 1; wr[i].pImageInfo = &ii[i];
            wr[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        vkUpdateDescriptorSets(dev, 3, wr, 0, nullptr);
    }
}

void LLCleanGodRaysVK::destroyDenoiseImages()
{
    for (GpuImage* im : {&mMeta[0], &mMeta[1], &mLmax})
    {
        if (im->image && mCtx) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
}

void LLCleanGodRaysVK::destroyHistory()
{
    for (GpuImage& im : mHist)
    {
        if (im.image && mCtx) mCtx->destroyImage(im);
        im = GpuImage{};
    }
    destroyDenoiseImages();
    mHistW = mHistH = 0;
    mHistValid = false;
}

bool LLCleanGodRaysVK::ensureHistory(VkCommandBuffer cmd, uint32_t w, uint32_t h, bool dn)
{
    VkMemScope memcat(VKMEM_TARGET);   // #407
    const bool haveDn = mMeta[0].image && mMeta[1].image && mLmax.image;
    if (mHist[0].image && mHist[1].image && w == mHistW && h == mHistH && haveDn == dn) return false;
    vkDeviceWaitIdle(mCtx->device());   // the sets / images may be in use by frames in flight
    destroyHistory();
    // The history pair; with the denoiser (#657) also its meta pair and Lmax (the march's
    // lit in-scatter) -- 3 more RGBA16F images at the march resolution, freed with it.
    GpuImage* imgs[5] = {&mHist[0], &mHist[1], &mMeta[0], &mMeta[1], &mLmax};
    const int nimg = dn ? 5 : 2;
    bool ok = true;
    for (int i = 0; i < nimg; ++i)
    {
        *imgs[i] = mCtx->createImage2D(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        ok = ok && imgs[i]->image;
    }
    if (!ok && dn)
    {
        // The denoiser's extras did not fit: keep the plain history.
        destroyDenoiseImages();
        ok = mHist[0].image && mHist[1].image;
        mDnAllocFailed = true;   // do not retry every frame (cleared by destroy())
        LL_WARNS("VKGodRays") << "god rays denoiser images failed to allocate; plain temporal pass" << LL_ENDL;
    }
    if (!ok)
    {
        destroyHistory();
        writeSets();
        return true;
    }
    const int ncl = (mMeta[0].image && mLmax.image) ? 5 : 2;
    mHistW = w; mHistH = h;
    // GENERAL for good, cleared (a reset frame never reads it, but no NaN may ever sit
    // in a history), then made visible to the temporal compute.
    VkImageMemoryBarrier2 ib[5]{};
    for (int i = 0; i < ncl; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].image = imgs[i]->image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = (uint32_t)ncl; d.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < ncl; ++i)
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d2.memoryBarrierCount = 1; d2.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d2);
    writeSets();
    return true;
}

bool LLCleanGodRaysVK::recordMarch(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<U32> s_tier(gSavedSettings, "RenderVKGodRays", 0);
    static LLCachedControl<F32> s_intensity(gSavedSettings, "RenderVKGodRaysIntensity", 1.f);
    static LLCachedControl<F32> s_aniso(gSavedSettings, "RenderVKGodRaysAnisotropy", 0.6f);
    static LLCachedControl<F32> s_maxdist(gSavedSettings, "RenderVKGodRaysMaxDist", 256.f);
    static LLCachedControl<U32> s_steps(gSavedSettings, "RenderVKGodRaysSteps", 0);
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKGodRaysScale", 0.f);
    static LLCachedControl<bool> s_temporal(gSavedSettings, "RenderVKGodRaysTemporal", true);
    static LLCachedControl<F32> s_rt_alpha(gSavedSettings, "RenderVKGodRaysRTAlphaDist", 32.f);
    // Denoiser (#657; see the header and settings.xml for each).
    static LLCachedControl<U32> s_dn(gSavedSettings, "RenderVKGodRaysDenoise", 1);
    static LLCachedControl<F32> s_dn_hist(gSavedSettings, "RenderVKGodRaysHistory", 32.f);
    static LLCachedControl<F32> s_dn_clamp(gSavedSettings, "RenderVKGodRaysClamp", 1.5f);
    static LLCachedControl<bool> s_dn_antilag(gSavedSettings, "RenderVKGodRaysAntiLag", true);
    static LLCachedControl<F32> s_dn_dynhist(gSavedSettings, "RenderVKGodRaysDynamicHistory", 3.f);
    static LLCachedControl<bool> s_dn_svgf(gSavedSettings, "RenderVKGodRaysSvgf", true);
    static LLCachedControl<U32> s_dn_atrous(gSavedSettings, "RenderVKGodRaysAtrous", 2);
    static LLCachedControl<F32> s_dn_react(gSavedSettings, "RenderVKGodRaysReactive", 2.f);
    static LLCachedControl<U32> s_dn_debug(gSavedSettings, "RenderVKGodRaysDebug", 0);

    // History bookkeeping: a main-view frame that does not accumulate (skipped, setting
    // off) breaks the history; snapshots (offscreen) leave it alone.
    mTemporalNow = false;
    mCompHist = false;
    mDnNow = false;
    mDnRan = false;
    mSpatialNow = false;
    mCompReact = false;
    mDeferRelease = f.deferRelease;
    mReleaseOwed = false;
    const bool histWas = mHistValid;
    if (!f.offscreen) mHistValid = false;

    const U32 tier = std::min<U32>(s_tier, 3u);
    if (tier == 0 || !f.ctx)
    {
        if (!idle() && f.ctx) destroy(f.ctx->device());
        return false;
    }
    // Shadow-map sun (shadow_params.w 1): march the cascades. Ray-traced sun (2): no
    // cascades are rendered, so march with shadow rays -- only when this frame traces
    // (TLAS + consumer set valid, lit variant RT); otherwise there is no occlusion
    // source and the effect is skipped. Shadows off -> nothing to shaft. Under water:
    // the water medium is #208 M4. No light: nothing.
    const bool rt = f.shadowMode > 1.5f && f.rtTrace && f.rtSet && f.rtLayout && f.dsetDraw1 && f.dslDraw1;
    if (f.shadowMode < 0.5f || (f.shadowMode > 1.5f && !rt) || f.underwater || f.lightLuma <= 1e-5f
        || !f.dsetDraw0 || !f.depthView || !f.viewProjRel || !f.camTime || f.rw < 2 || f.rh < 2)
        return false;
    if (!ensurePipelines(f)) return false;
    if (rt && !ensureRtPipeline(f)) return false;

    // Resolution and steps: the tier's (kTiers / kRtTiers), unless overridden.
    const Tier& tt = rt ? kRtTiers[tier] : kTiers[tier];
    float scale = tt.scale;
    if (s_scale > 0.f) scale = (F32)s_scale >= 0.75f ? 1.f : ((F32)s_scale >= 0.375f ? 0.5f : 0.25f);
    uint32_t steps = tt.steps;
    // RT: down to 2 steps (the denoiser's A/B for fewer rays per texel, #657).
    if (s_steps > 0) steps = llclamp((U32)s_steps, rt ? 2u : 4u, 96u);
    // Snapshots have no history to integrate the dither: cascades at full resolution
    // with 4x the steps; ray-traced at half resolution with 3x, at most 24 (a full-res
    // ray march would be hundreds of millions of rays at snapshot sizes).
    if (f.offscreen)
    {
        if (rt) { scale = std::max(scale, 0.5f); steps = std::min(steps * 3u, 24u); }
        else    { scale = 1.f; steps = std::min(steps * 4u, 192u); }
    }
    const uint32_t w = std::max(1u, (uint32_t)std::ceil(f.rw * scale));
    const uint32_t h = std::max(1u, (uint32_t)std::ceil(f.rh * scale));
    ensureTarget(cmd, f, w, h);

    // Temporal accumulation (main view only): the history pair at the march resolution.
    // Off: free it (the setting toggles rarely; ~2 x 8 B / texel).
    const bool temporal = s_temporal && !f.offscreen && mTempPipe;
    // DENOISER (#657): replaces the plain temporal pass for the RT march (Denoise 1) or
    // every march (2), when its pipelines (and, RT, the tracking march) exist.
    const U32 dnMode = std::min<U32>(s_dn, 2u);
    bool dn = temporal && dnMode > 0 && (rt || dnMode == 2) && mDnTempPipe && mAtPipe
              && (!rt || mMarchRtDnPipe) && !mDnAllocFailed;
    bool histReset = true;
    if (temporal)
    {
        const bool created = ensureHistory(cmd, w, h, dn);
        dn = dn && mMeta[0].image && mMeta[1].image && mLmax.image;
        if (mHist[0].image)
        {
            const double dx = (double)f.camTime[0] - mPrevCam[0];
            const double dy = (double)f.camTime[1] - mPrevCam[1];
            const double dz = (double)f.camTime[2] - mPrevCam[2];
            const bool jump = dx * dx + dy * dy + dz * dz > kCamJump * kCamJump;
            // (The denoiser's history holds v, not HDR in-scatter: a mode switch resets.)
            histReset = created || !histWas || f.cameraCut || jump
                        || rt != mPrevRt || tier != mPrevTier || steps != mPrevSteps || dn != mPrevDn;
            mHistCur ^= 1u;   // write the other parity; read last frame's
            mTemporalNow = true;
            mDnNow = dn;
        }
    }
    else if (!f.offscreen && mHist[0].image)
    {
        vkDeviceWaitIdle(f.ctx->device());
        destroyHistory();
        writeSets();
    }
    if (tier != mLogTier || w != mLogW || h != mLogH || steps != mLogSteps || rt != mLogRt || dn != mLogDn)
    {
        LL_INFOS("VKGodRays") << "god rays: tier " << tier << ", " << w << "x" << h << " (render "
                              << f.rw << "x" << f.rh << "), " << steps << " steps"
                              << (rt ? ", ray-traced sun" : ", cascades")
                              << (dn ? ", denoiser" : (mTemporalNow ? ", temporal" : ""))
                              << (f.offscreen ? " [snapshot]" : "") << LL_ENDL;
        mLogTier = tier; mLogW = w; mLogH = h; mLogSteps = steps; mLogRt = rt; mLogDn = dn;
    }

    // Frame info (updated by transfer earlier this frame) -> this compute's reads; world
    // depth (pass 1's writes, already DEPTH_READ_ONLY) -> sampled here; the in-scatter
    // target: last frame's composite reads -> these writes
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkImageMemoryBarrier2 ib[3]{};
    ib[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ib[0].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ib[0].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ib[0].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ib[0].oldLayout = ib[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    ib[0].image = f.depthImage; ib[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    ib[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    // (and last frame's temporal pass's compute reads: WAR, execution only).
    ib[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib[1].srcAccessMask = 0;
    ib[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ib[1].oldLayout = ib[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib[1].image = mOut.image; ib[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Denoiser (#657): last frame's a-trous finalize WROTE the march target and Lmax
    // (WAW: a memory dependency), the composite read them (WAR).
    if (dn)
    {
        ib[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        ib[2] = ib[1];
        ib[2].image = mLmax.image;
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
    dep.imageMemoryBarrierCount = dn ? 3u : 2u; dep.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &dep);

    struct { float inv_view_proj[16]; float cam[4]; float dims[4]; float params[4]; float params2[4]; } mp{};
    static_assert(sizeof(mp) == 128, "god rays march push must stay 128 B");
    invert4(f.viewProjRel, mp.inv_view_proj);
    mp.cam[0] = f.camTime[0]; mp.cam[1] = f.camTime[1]; mp.cam[2] = f.camTime[2];
    mp.cam[3] = (float)((mFrame + f.snapSample) % 64u);   // (#932: + a supersampled shot's sample index)
    mp.dims[0] = (float)f.rw; mp.dims[1] = (float)f.rh; mp.dims[2] = (float)w; mp.dims[3] = (float)h;
    mp.params[0] = kScatterGain * llclamp((F32)s_intensity, 0.f, 4.f);
    mp.params[1] = llclamp((F32)s_aniso, 0.f, 0.95f);
    mp.params[2] = llclamp((F32)s_maxdist, 16.f, 512.f);
    mp.params[3] = (float)steps;
    mp.params2[0] = 1.f;   // water-plane clamp
    mp.params2[1] = f.camTime[3];   // seconds: RT alpha test's texture animation
    // Rotate the noise: the temporal pass integrates it; 2 = the denoiser's v / Lmax
    // split + moving-caster sign (#657).
    mp.params2[2] = dn ? 2.f : (mTemporalNow ? 1.f : 0.f);
    mp.params2[3] = std::max((F32)s_rt_alpha, 0.f);   // RT: opaque-only shadow rays past this (m; 0 = off)

    // The TLAS / RT SSBOs need no barrier here: LLCleanRTVK's build releases them to
    // fragment + compute consumers, and this runs after pass 1 in the same submission.
    if (rt)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dn ? mMarchRtDnPipe : mMarchRtPipe);
        const VkDescriptorSet sets[4] = {f.dsetDraw0, f.dsetDraw1, f.rtSet, mDset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMarchRtLayout, 0, 4, sets, 0, nullptr);
        VK_PUSH_BUDGET_CHECK(mp);
        vkCmdPushConstants(cmd, mMarchRtLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    }
    else
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMarchPipe);
        const VkDescriptorSet sets[2] = {f.dsetDraw0, mDset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMarchLayout, 0, 2, sets, 0, nullptr);
        VK_PUSH_BUDGET_CHECK(mp);
        vkCmdPushConstants(cmd, mMarchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    }
    mLastRt = rt;
    vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1);

    // In-scatter writes -> the temporal pass's / pass 2's composite reads (recorded
    // before the rendering scope). Deferred (phase 2): compute only here -- a FRAGMENT
    // destination would hold pass 2's raster behind the march; the composite's
    // release is recordCompositeRelease's.
    ib[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ib[1].dstStageMask = mDeferRelease ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                       : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    ib[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    mReleaseOwed = mDeferRelease;
    // Denoiser: Lmax -> the a-trous finalize (compute; the composite reads it only after
    // the finalize rewrote its alpha, released there / by recordCompositeRelease).
    ib[2] = ib[1];
    ib[2].image = mLmax.image;
    ib[2].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib[2].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = dn ? 2u : 1u; dep2.pImageMemoryBarriers = &ib[1];
    vkCmdPipelineBarrier2(cmd, &dep2);

    std::memcpy(mCompPush.inv_view_proj, mp.inv_view_proj, sizeof(mCompPush.inv_view_proj));
    mCompPush.dims[0] = mp.dims[0]; mCompPush.dims[1] = mp.dims[1];
    mCompPush.dims[2] = mp.dims[2]; mCompPush.dims[3] = mp.dims[3];
    mCompPush.jitter[0] = f.jitterNdc ? f.jitterNdc[0] : 0.f;
    mCompPush.jitter[1] = f.jitterNdc ? f.jitterNdc[1] : 0.f;
    mCompPush.jitter[2] = 0.f;   // reactive: per composite site (recordComposite)

    if (mTemporalNow)
    {
        // Reprojection: current camera-relative world -> last frame's clip = last frame's
        // camera-relative view-proj x translate(current eye - last eye), in double.
        const double dl[3] = {(double)f.camTime[0] - mPrevCam[0], (double)f.camTime[1] - mPrevCam[1],
                              (double)f.camTime[2] - mPrevCam[2]};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                double v = (double)mPrevViewProjRel[c * 4 + r];
                if (c == 3)
                    for (int k = 0; k < 3; ++k) v += (double)mPrevViewProjRel[k * 4 + r] * dl[k];
                mTempPush.reproj[c * 4 + r] = (float)v;
            }
        // View ray of an unjittered NDC: inverse(view_proj_rel) x (x, y, 0.5, 1) =
        // col0 x + col1 y + (0.5 col2 + col3); its w is constant over x, y (perspective,
        // rotation-only view), so pre-divide and keep xyz. w slots: the eye's move.
        const float* iv = mp.inv_view_proj;
        float cw = 0.5f * iv[2 * 4 + 3] + iv[3 * 4 + 3];
        if (std::fabs(cw) < 1e-12f) cw = 1.f;
        for (int r = 0; r < 3; ++r)
        {
            mTempPush.ray_a[r] = iv[0 * 4 + r] / cw;
            mTempPush.ray_b[r] = iv[1 * 4 + r] / cw;
            mTempPush.ray_c[r] = (0.5f * iv[2 * 4 + r] + iv[3 * 4 + r]) / cw;
        }
        mTempPush.ray_a[3] = (float)dl[0];
        mTempPush.ray_b[3] = (float)dl[1];
        mTempPush.ray_c[3] = (float)dl[2];
        mTempPush.dims[0] = (float)w; mTempPush.dims[1] = (float)h;
        mTempPush.dims[2] = kHistWeight;
        mTempPush.dims[3] = histReset ? 1.f : 0.f;
        if (dn)
        {
            // Denoiser temporal (clean_godrays_dn_temporal.comp): the same 128 B, dims
            // reinterpreted as p -- x max history, y clamp sigmas, z flags (exact integer:
            // bit 0 anti-lag, bit 1 SVGF clamp, bits 4-7 moving-caster history, bits 8-15
            // reactive gain x 16), w 1 = no history. Its size comes from textureSize.
            const U32 dyn = (U32)llclamp((F32)s_dn_dynhist, 1.f, 15.f);
            const U32 gain = (U32)llclamp((F32)s_dn_react * 16.f + 0.5f, 0.f, 255.f);
            const U32 flags = (s_dn_antilag ? 1u : 0u) | (s_dn_svgf ? 2u : 0u) | (dyn << 4u) | (gain << 8u);
            mTempPush.dims[0] = llclamp((F32)s_dn_hist, 1.f, 64.f);
            mTempPush.dims[1] = llclamp((F32)s_dn_clamp, 0.5f, 8.f);
            mTempPush.dims[2] = (float)flags;
            // A-trous / finalize (clean_godrays_atrous.comp): N iterations, at least the
            // finalize-only dispatch (it writes v x Lmax into the march target).
            const U32 iters = std::min<U32>(s_dn_atrous, 4u);
            const U32 dbg = std::min<U32>(s_dn_debug, 5u);
            mAtIters = std::max(iters, 1u);
            mAtPush = AtPush{};
            mAtPush.p0[1] = (float)mAtIters;
            mAtPush.p0[2] = (float)((s_dn_svgf ? 1u : 0u) | (iters > 0 ? 2u : 0u) | (dbg << 4u));
            mAtPush.p0[3] = mTempPush.dims[0];
            mAtPush.p1[0] = (float)steps;
            mSpatialNow = true;
            mCompReact = s_dn_react > 0.f;
        }
        static_assert(sizeof(TempPush) == 128, "god rays temporal push must stay 128 B");
        // The renderer records recordTemporal() right after every recordMarch() that
        // returned true, so this frame's history parity is written.
        mHistValid = true;
    }
    // This main-view frame is the next one's "previous" (snapshots never are).
    if (!f.offscreen)
    {
        std::memcpy(mPrevViewProjRel, f.viewProjRel, sizeof(mPrevViewProjRel));
        mPrevCam[0] = f.camTime[0]; mPrevCam[1] = f.camTime[1]; mPrevCam[2] = f.camTime[2];
        mPrevRt = rt; mPrevTier = tier; mPrevSteps = steps; mPrevDn = dn;
        ++mFrame;
    }
    return true;
}

bool LLCleanGodRaysVK::recordTemporal(VkCommandBuffer cmd)
{
    if (!mTemporalNow || !mTempPipe || !mHist[0].image || !mHist[1].image) return false;
    mTemporalNow = false;
    const uint32_t p = mHistCur;
    // The march's writes were released to compute reads at its end. The history pair:
    // last frame's compute writes (mHist[p ^ 1]) -> these reads; earlier frames' compute
    // / composite reads of mHist[p] -> these writes (WAR: execution only).
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    mDnRan = mDnNow;
    if (mDnNow)
    {
        // DENOISER (#657): the RT-shadow-style temporal pass -> acc / meta parity p. The
        // a-trous finalize (recordSpatial) then writes the march target the composite
        // reads (mDset), so neither the history nor its release to fragment is needed.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDnTempPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDnTempLayout, 0, 1, &mDnTempDset[p], 0, nullptr);
        vkCmdPushConstants(cmd, mDnTempLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TempPush), &mTempPush);
        vkCmdDispatch(cmd, (mHistW + 7u) / 8u, (mHistH + 7u) / 8u, 1);
        return true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTempPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTempLayout, 0, 1, &mTempDset[p], 0, nullptr);
    VK_PUSH_BUDGET_CHECK(TempPush);
    vkCmdPushConstants(cmd, mTempLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TempPush), &mTempPush);
    vkCmdDispatch(cmd, (mHistW + 7u) / 8u, (mHistH + 7u) / 8u, 1);

    mCompHist = true;
    // Deferred (phase 2): recordCompositeRelease releases the history to the composite.
    if (mDeferRelease) return true;
    // History writes -> pass 2's composite reads.
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    ib.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ib.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = mHist[p].image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep2.imageMemoryBarrierCount = 1; dep2.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd, &dep2);
    return true;
}

bool LLCleanGodRaysVK::recordSpatial(VkCommandBuffer cmd)
{
    if (!mSpatialNow || !mDnRan || !mAtPipe || !mLmax.image || !mHist[0].image) return false;
    mSpatialNow = false;
    const uint32_t p = mHistCur;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAtPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAtLayout, 0, 1, &mAtDset[p], 0, nullptr);
    for (uint32_t k = 0; k < mAtIters; ++k)
    {
        // The previous dispatch's writes (temporal acc / meta; iteration k - 1's scratch;
        // last frame's composite reads of the target / Lmax were ordered by the march's
        // barrier) -> this dispatch's reads / writes. Scratch k + 1 overwrites what
        // iteration k - 1 read (WAR, covered by the same execution dependency).
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        mAtPush.p0[0] = (float)k;
        VK_PUSH_BUDGET_CHECK(AtPush);
        vkCmdPushConstants(cmd, mAtLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AtPush), &mAtPush);
        vkCmdDispatch(cmd, (mHistW + 7u) / 8u, (mHistH + 7u) / 8u, 1);
    }
    // Deferred (phase 2): recordCompositeRelease releases the target + Lmax.
    if (mDeferRelease) return true;
    // The finalize's writes (target, Lmax.a) -> pass 2's composite reads.
    VkImageMemoryBarrier2 ib[2]{};
    for (int i = 0; i < 2; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        ib[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        ib[i].oldLayout = ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].image = i == 0 ? mOut.image : mLmax.image;
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep2.imageMemoryBarrierCount = 2; dep2.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &dep2);
    return true;
}

void LLCleanGodRaysVK::recordCompositeRelease(VkCommandBuffer cmd)
{
    if (!mReleaseOwed) return;
    mReleaseOwed = false;
    // March (mOut) and, when the temporal pass ran, its history parity: compute writes
    // -> the composite's fragment reads. Both stay GENERAL. mOut is released either way
    // (the march wrote it this frame; cheap, and correct whichever image is bound).
    VkImageMemoryBarrier2 ib[3]{};
    uint32_t n = 0;
    auto add = [&](VkImage img)
    {
        if (!img) return;
        VkImageMemoryBarrier2& b = ib[n++];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    };
    add(mOut.image);
    if (mCompHist) add(mHist[mHistCur].image);
    if (mDnRan && mLmax.image) add(mLmax.image);   // denoiser: the finalize's Lmax.a (reactive)
    if (n == 0) return;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = n; dep.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LLCleanGodRaysVK::recordComposite(VkCommandBuffer cmd, bool reactive)
{
    if (!mCompPipe || !mDset) return;
    // The denoiser's moving-caster shading change -> mReact.R, only where the resolve
    // reads it (the caller) and the denoiser ran this frame (else binding 4 is a
    // placeholder). The march target holds the finalized in-scatter: mDset (mCompHist
    // is false then).
    mCompPush.jitter[2] = (reactive && mCompReact && mDnRan) ? 1.f : 0.f;
    // Plain bind: the pipeline has no dynamic cull / depth state (viewport + scissor
    // come from the pass).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompPipe);
    // The accumulated history when the temporal pass ran this frame, else the raw march.
    const VkDescriptorSet set = mCompHist ? mCompDset[mHistCur] : mDset;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompLayout, 0, 1, &set, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(mCompPush);
    vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mCompPush), &mCompPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanGodRaysVK::destroy(VkDevice dev)
{
    if (!dev) return;
    // A build in flight references the layouts below (#782 audit): wait, free its result.
    mJob.wait();
    for (VkPipeline& p : mStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    mPipeFailed = false;
    vkDeviceWaitIdle(dev);
    destroyRtPipeline(dev);
    destroyTarget();
    destroyHistory();
    if (mTempPipe) vkDestroyPipeline(dev, mTempPipe, nullptr);
    if (mDnTempPipe) vkDestroyPipeline(dev, mDnTempPipe, nullptr);
    if (mDnTempLayout) vkDestroyPipelineLayout(dev, mDnTempLayout, nullptr);
    if (mAtPipe) vkDestroyPipeline(dev, mAtPipe, nullptr);
    if (mAtLayout) vkDestroyPipelineLayout(dev, mAtLayout, nullptr);
    if (mDnTempDsl) vkdDestroySetLayout(dev, mDnTempDsl);
    if (mAtDsl) vkdDestroySetLayout(dev, mAtDsl);
    mDnTempPipe = mAtPipe = VK_NULL_HANDLE;
    mDnTempLayout = mAtLayout = VK_NULL_HANDLE;
    mDnTempDsl = mAtDsl = VK_NULL_HANDLE;
    for (int i = 0; i < 2; ++i) mDnTempDset[i] = mAtDset[i] = VK_NULL_HANDLE;
    mDnNow = mDnRan = mSpatialNow = mCompReact = mPrevDn = false;
    mDnAllocFailed = false;
    if (mTempLayout) vkDestroyPipelineLayout(dev, mTempLayout, nullptr);
    if (mMarchPipe) vkDestroyPipeline(dev, mMarchPipe, nullptr);
    if (mCompPipe) vkDestroyPipeline(dev, mCompPipe, nullptr);
    if (mMarchLayout) vkDestroyPipelineLayout(dev, mMarchLayout, nullptr);
    if (mCompLayout) vkDestroyPipelineLayout(dev, mCompLayout, nullptr);
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    if (mTempDsl) vkdDestroySetLayout(dev, mTempDsl);
    mTempPipe = VK_NULL_HANDLE; mTempLayout = VK_NULL_HANDLE; mTempDsl = VK_NULL_HANDLE;
    mTempDset[0] = mTempDset[1] = VK_NULL_HANDLE;
    mCompDset[0] = mCompDset[1] = VK_NULL_HANDLE;
    mTemporalNow = mCompHist = false;
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mMarchPipe = mCompPipe = VK_NULL_HANDLE;
    mMarchLayout = mCompLayout = VK_NULL_HANDLE;
    mDsl = VK_NULL_HANDLE; mDset = VK_NULL_HANDLE; mSampler = VK_NULL_HANDLE;
    mPipeDslDraw0 = VK_NULL_HANDLE;
    for (VkFormat& fm : mCompFormats) fm = VK_FORMAT_UNDEFINED;
    mLogTier = ~0u;
    mLastRt = false;
}
