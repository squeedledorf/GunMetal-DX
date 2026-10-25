/**
 * @file llcleanpartfogvk.cpp
 * @brief Particle fog (#1003): see llcleanpartfogvk.h and the shaders clean_pfog_*.
 */
#include "linden_common.h"

#include "llcleanpartfogvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llcleanparticlestore.h"
#include "llerror.h"
#include "llframetimer.h"
#include "llpartdata.h"
#include "llviewercontrol.h"
#include "v4coloru.h"
#include "llworld.h"   // (resolveLandHeightAgent: the heavy fog's fallback floor)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>

namespace
{
// Kernel growth over a particle's life (R = half its size x (1 + kSpread x age fraction)),
// the fresh-particle fade-in (s), the smallest kernel (voxels), the runaway clamp (/m).
constexpr float kSpread = 0.6f;
constexpr float kFadeIn = 0.25f;
constexpr float kMinKernel = 0.9f;
constexpr float kSigmaClamp = 64.f;
// The blend moves toward its target at 1 / kBlendTime per second (no pops).
constexpr float kBlendTime = 0.6f;
// Billow noise: warp amplitude (x the base scale), flow period (s); the base scale = the
// sprite size x kBaseScaleK, clamped to [kBaseScaleMin, kBaseScaleMax] m, x BillowScale.
constexpr float kNoiseWarp = 1.3f;
constexpr float kFlowPeriod = 3.f;
constexpr float kBaseScaleK = 0.3f;
constexpr float kBaseScaleMin = 0.3f;
constexpr float kBaseScaleMax = 1.0f;
// Per-puff lumpiness (kernel radius displacement) per unit of RenderVKParticleFogErosion.
constexpr float kLumpPerErosion = 0.35f;
// HG anisotropy of the medium (water droplets scatter forward) and the back lobe's share;
// beer-powder strength; local-light shadow strength (x the optical depth through the medium).
constexpr float kPhaseG = 0.6f;
constexpr float kBackLobe = 0.2f;
constexpr float kPowder = 0.6f;
constexpr float kLocalShadowK = 1.f;
// Heavy settling: speed at RenderVKParticleFogHeavy 1 (m/s), the delay after spawn (s), the
// roll-out gain (horizontal m per m of blocked descent), the settled layer's height squash,
// the fallback floor's drop below the source (m), the floor reference above it (m), how
// often a floor map forgets (s: a moved prop / an avatar that walked off).
constexpr float kSettleMax = 0.2f;
constexpr float kSettleDelay = 0.4f;
constexpr float kRollGain = 1.2f;
constexpr float kSquashMin = 0.45f;
constexpr float kFloorDrop = 1.5f;
constexpr float kFloorRef = 0.3f;
constexpr double kFloorForget = 4.0;
// Near-fog resolution: on when a converted brick is within kNearOn m, off after kNearHold s
// with none within kNearOff m.
constexpr float kNearOn = 4.f;
constexpr float kNearOff = 7.f;
constexpr double kNearHold = 2.0;
// Camera jump (m) past which the history drops.
constexpr double kCamJump = 16.0;
// A slot costs about this much VRAM (fine 2 MB + 4 coarse 0.25 MB + the 5 MB accumulator).
constexpr double kSlotMB = ((double)LLCleanPartFogVK::kN * LLCleanPartFogVK::kN * LLCleanPartFogVK::kN * 8.0
                            + 4.0 * LLCleanPartFogVK::kNC * LLCleanPartFogVK::kNC * LLCleanPartFogVK::kNC * 8.0
                            + (double)LLCleanPartFogVK::kSlotWords * 4.0) / (1024.0 * 1024.0);

const char* const kReasonName[LLCleanPartFogVK::R_COUNT] = {
    "none", "hud", "attachment", "additive/emissive/glow", "ribbon/target/follow-velocity", "small", "fast",
    "short-lived", "shaped texture", "texture stats pending", "low score", "distance", "brick budget"};

VkShaderModule pfModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
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

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src; mb.srcAccessMask = srcA;
    mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

VkSampler makeSampler(VkDevice dev, VkFilter filter, VkSamplerAddressMode mode)
{
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = mode;
    sci.maxLod = 0.f;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &s));
    return s;
}

// UNDEFINED -> GENERAL, then a clear (every image here lives in GENERAL for good).
void clearToGeneral(VkCommandBuffer cmd, const GpuImage* const* imgs, const VkClearColorValue* vals, uint32_t n)
{
    std::vector<VkImageMemoryBarrier2> b(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        b[i] = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].image = imgs[i]->image; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b.data();
    vkCmdPipelineBarrier2(cmd, &d);
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < n; ++i)
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, &vals[i], 1, &rng);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// The own set's binding count (0 - 28).
constexpr uint32_t kBindings = 29;

inline float pfClamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline float pfRamp(float x, float a, float b) { return pfClamp01((x - a) / (b - a)); }
inline float pfStep(float a, float b, float x) { const float t = pfRamp(x, a, b); return t * t * (3.f - 2.f * t); }

uint32_t orderedToBits(float f)
{
    uint32_t b; std::memcpy(&b, &f, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}
float bitsToOrdered(uint32_t u)
{
    const uint32_t b = (u & 0x80000000u) ? (u & 0x7FFFFFFFu) : ~u;
    float f; std::memcpy(&f, &b, 4);
    return f;
}

// Box (agent frame) vs the camera-relative unjittered view-projection: false = surely off screen.
bool boxVisible(const float* vp, const float* eye, const float* mn, const float* mx)
{
    int out[5] = {0, 0, 0, 0, 0};
    for (int k = 0; k < 8; ++k)
    {
        const float p[3] = {((k & 1) ? mx[0] : mn[0]) - eye[0], ((k & 2) ? mx[1] : mn[1]) - eye[1],
                            ((k & 4) ? mx[2] : mn[2]) - eye[2]};
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = vp[r] * p[0] + vp[4 + r] * p[1] + vp[8 + r] * p[2] + vp[12 + r];
        if (c[0] < -c[3]) ++out[0];
        if (c[0] > c[3]) ++out[1];
        if (c[1] < -c[3]) ++out[2];
        if (c[1] > c[3]) ++out[3];
        if (c[3] <= 0.f) ++out[4];
    }
    for (int i = 0; i < 5; ++i) if (out[i] == 8) return false;
    return true;
}

float boxDistance(const float* p, const float* mn, const float* mx)
{
    float d2 = 0.f;
    for (int a = 0; a < 3; ++a)
    {
        const float e = p[a] < mn[a] ? mn[a] - p[a] : (p[a] > mx[a] ? p[a] - mx[a] : 0.f);
        d2 += e * e;
    }
    return std::sqrt(d2);
}
} // namespace

// ---------------------------------------------------------------------------
// pipelines (first use: layouts here, the pipelines in a background batch)
// ---------------------------------------------------------------------------
bool LLCleanPartFogVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;
        bool ok = true;
        for (int i = 0; i < P_COUNT; ++i) { mPipe[i] = mStage[i]; mStage[i] = VK_NULL_HANDLE; ok = ok && mPipe[i]; }
        if (!ok)
        {
            LL_WARNS("VKPartFog") << "particle fog pipelines failed to build; effect disabled" << LL_ENDL;
            for (VkPipeline& p : mPipe) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
            mPipeFailed = true;
            return false;
        }
        mPipeReady = true;
        LL_INFOS("VKPartFog") << "particle fog pipelines built (background)" << LL_ENDL;
    }
    const bool same = mPipeDsl[0] == f.dslDraw0 && mPipeDsl[1] == f.dslDraw1 && mPipeFormats[0] == f.hdrFormat
                   && mPipeFormats[1] == f.glowFormat && mPipeFormats[2] == f.reactFormat && mPipeFormats[3] == f.depthFormat;
    if (mPipeReady && same) return true;
    if (mPipeFailed && same) return false;
    if (mPipeReady || mDsl)
    {
        // A pass format / set layout change (rare): rebuild from scratch.
        destroy(dev);
    }
    mCtx = f.ctx;
    mPipeFailed = false;

    mSampLin  = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    mSampRep  = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    mSampNear = makeSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Own set (clean_pfog_common.glsl): 0 params (UBO), 1 table, 2 particle pool, 3 particle
    // lighting records, 4 splat list, 5 accumulator, 6 results (SSBOs); 7-10 fine / coarse /
    // velocity / light atlases (storage), 11-14 the same sampled; 15 noise (storage), 16 noise
    // (sampled, repeat); 17 depth; 18 / 19 march A / B (storage); 20 / 21 history A / B of last
    // frame (sampled), 22 / 23 this frame's (storage); 24 / 25 the light-direction atlas
    // (storage / sampled); 26 / 27 the detail noise (storage / sampled, repeat); 28 the floor
    // maps (SSBO).
    {
        VkDescriptorSetLayoutBinding b[kBindings]{};
        for (uint32_t i = 0; i < kBindings; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (i == 0) t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else if (i <= 6 || i == 28) t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if ((i >= 11 && i <= 14) || i == 16 || i == 17 || i == 20 || i == 21 || i == 25 || i == 27)
                t = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorType = t;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = kBindings; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslPartFog"));
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
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mCompDsl, "mDslPartFogComp"));
    }
    VK_CHECK(mPool.create(dev, "partfog", {{mDsl, 4}, {mCompDsl, 6}}));
    for (auto& row : mDset)
        for (VkDescriptorSet& s : row)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl, &s), { destroy(dev); return false; });
    for (auto& row : mCompSet)
        for (VkDescriptorSet& s : row)
            VKD_ALLOC_SOFT(mPool.allocate(mCompDsl, &s), { destroy(dev); return false; });

    {
        const VkDescriptorSetLayout sets[3] = {f.dslDraw0, f.dslDraw1, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mPartFogLayout"));
        const VkDescriptorSetLayout cs[2] = {f.dslDraw0, mCompDsl};
        VkPipelineLayoutCreateInfo cli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cli.setLayoutCount = 2; cli.pSetLayouts = cs;
        VK_CHECK(vkdCreatePipelineLayout(dev, &cli, &mCompLayout, "mPartFogCompLayout"));
    }

    {
        PipelineBatchScope batchScope(dev);
        struct C { const uint32_t* spv; size_t bytes; const char* name; int slot; };
        const C cs[8] = {
            {kWorldPFogTexStatsSpv, kWorldPFogTexStatsSpv_bytes, "kWorldPFogTexStatsSpv", P_TEX},
            {kWorldPFogBinSpv,      kWorldPFogBinSpv_bytes,      "kWorldPFogBinSpv",      P_BIN},
            {kWorldPFogSplatSpv,    kWorldPFogSplatSpv_bytes,    "kWorldPFogSplatSpv",    P_SPLAT},
            {kWorldPFogResolveSpv,  kWorldPFogResolveSpv_bytes,  "kWorldPFogResolveSpv",  P_RESOLVE},
            {kWorldPFogLightSpv,    kWorldPFogLightSpv_bytes,    "kWorldPFogLightSpv",    P_LIGHT},
            {kWorldPFogNoiseSpv,    kWorldPFogNoiseSpv_bytes,    "kWorldPFogNoiseSpv",    P_NOISE},
            {kWorldPFogMarchSpv,    kWorldPFogMarchSpv_bytes,    "kWorldPFogMarchSpv",    P_MARCH},
            {kWorldPFogTemporalSpv, kWorldPFogTemporalSpv_bytes, "kWorldPFogTemporalSpv", P_TEMPORAL}};
        for (const C& c : cs)
        {
            VkShaderModule m = pfModule(dev, c.spv, c.bytes, c.name);
            createComputePipelineInto(dev, m, mLayout, 0, nullptr, &mStage[c.slot], c.name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
        // Composite: render pass 2's attachments, scene' = src + dst x src.a (its alpha kept),
        // glow / reactive write-masked, no depth test (the froxel apply's state).
        VkShaderModule vs = pfModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = pfModule(dev, kWorldPFogCompFragSpv, kWorldPFogCompFragSpv_bytes, "kWorldPFogCompFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mCompLayout;
        d.colorFormat = f.hdrFormat;
        d.glowFormat = f.glowFormat; d.glowWrite = false;
        d.reactFormat = f.reactFormat; d.reactWriteMask = 0;
        d.depthFormat = f.depthFormat; d.depthTest = false; d.depthWrite = false;
        d.customBlend = true;
        d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[P_COMP], "particle fog composite");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
        PipelineJobOpts o;
        o.what = "particle fog";
        o.gate = false;   // optional effect: never holds the login screen
        mJob.start(pipelineBatchFlushAsync(dev, o));
    }
    mPipeDsl[0] = f.dslDraw0; mPipeDsl[1] = f.dslDraw1;
    mPipeFormats[0] = f.hdrFormat; mPipeFormats[1] = f.glowFormat;
    mPipeFormats[2] = f.reactFormat; mPipeFormats[3] = f.depthFormat;
    LL_INFOS("VKPartFog") << "particle fog pipelines compiling in the background (the effect starts when they land)" << LL_ENDL;
    return false;
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------
void LLCleanPartFogVK::destroyFixed()
{
    if (!mCtx) return;
    for (GpuBuffer* b : {&mParams[0], &mParams[1], &mTable[0], &mTable[1], &mReadback[0], &mReadback[1], &mList, &mResults,
                         &mFloor})
    {
        if (b->buffer) mCtx->destroyBuffer(*b);
        *b = GpuBuffer{};
    }
    for (GpuImage* im : {&mNoise, &mNoiseD})
    {
        if (im->image) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    mNoiseDone = false;
    mListCap = 0;
    mRbMeta[0].valid = mRbMeta[1].valid = false;
    mSetsDirty = true;
}

// The host rings, the splat list (sized for the pool), the results, the noise volumes, the
// floor maps.
bool LLCleanPartFogVK::ensureFixed(VkCommandBuffer cmd)
{
    const bool have = mParams[0].mapped && mParams[1].mapped && mTable[0].mapped && mTable[1].mapped
                   && mReadback[0].mapped && mReadback[1].mapped && mResults.buffer && mNoise.image && mNoiseD.image
                   && mFloor.buffer && mList.buffer && mListCap == mPartMax;
    if (have) return true;
    vkDeviceWaitIdle(mCtx->device());   // (the list / sets may be in flight)
    destroyFixed();
    {
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        {
            VkMemScope memcat(VKMEM_STAGING);   // #407
            for (int s = 0; s < 2; ++s)
            {
                mParams[s] = mCtx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hv);
                mTable[s] = mCtx->createBuffer(sizeof(TableGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv);
            }
        }
        {
            VkMemScope memcat(VKMEM_READBACK);
            for (int s = 0; s < 2; ++s)
                mReadback[s] = mCtx->createBuffer(kResultsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv);
        }
        VkMemScope memcat(VKMEM_PARTICLE);
        mResults = mCtx->createBuffer(kResultsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                         | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mList = mCtx->createBuffer(16 + (VkDeviceSize)std::max(mPartMax, 1u) * kRecBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mNoise = mCtx->createImage3D(64, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                                               | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        mNoiseD = mCtx->createImage3D(32, 32, 32, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        mFloor = mCtx->createBuffer((VkDeviceSize)kMaxBricks * kFloorWords * 4u,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    bool ok = mResults.buffer && mList.buffer && mNoise.image && mNoiseD.image && mFloor.buffer;
    for (int s = 0; s < 2; ++s) ok = ok && mParams[s].mapped && mTable[s].mapped && mReadback[s].mapped;
    if (!ok)
    {
        LL_WARNS("VKPartFog") << "particle fog buffers refused (out of memory); effect skipped, retrying later" << LL_ENDL;
        destroyFixed();
        return false;
    }
    mListCap = mPartMax;
    const GpuImage* imgs[2] = {&mNoise, &mNoiseD};
    const VkClearColorValue zero[2] = {};
    clearToGeneral(cmd, imgs, zero, 2);
    vkCmdFillBuffer(cmd, mResults.buffer, 0, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, mList.buffer, 0, VK_WHOLE_SIZE, 0u);
    // floor maps: every slot empty (header / stats 0, columns all ones = none seen)
    for (uint32_t i = 0; i < kMaxBricks; ++i)
    {
        const VkDeviceSize o = (VkDeviceSize)i * kFloorWords * 4u;
        vkCmdFillBuffer(cmd, mFloor.buffer, o, 32u, 0u);
        vkCmdFillBuffer(cmd, mFloor.buffer, o + 32u, (VkDeviceSize)kNC * kNC * 4u, 0xFFFFFFFFu);
    }
    for (auto& kv : mSys) kv.second.floorVox = 0.f;   // (stamps refer to the old buffer)
    LL_INFOS("VKPartFog") << "particle fog noise volumes: base 64^3 + detail 32^3 RGBA8 ("
                          << (64u * 64u * 64u * 4u + 32u * 32u * 32u * 4u) / 1024u << " KB), floor maps "
                          << (uint32_t)(kMaxBricks * kFloorWords * 4u / 1024u) << " KB" << LL_ENDL;
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
                   | VK_ACCESS_2_TRANSFER_READ_BIT);
    mNoiseDone = false;
    mSetsDirty = true;
    return true;
}

void LLCleanPartFogVK::destroyAtlas()
{
    if (!mCtx) return;
    for (GpuImage* im : {&mDens, &mCoarse, &mVel, &mLight, &mLDir})
    {
        if (im->image) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    if (mAccum.buffer) mCtx->destroyBuffer(mAccum);
    mAccum = GpuBuffer{};
    mSlots = 0;
    mCols = mRows = 1;
    mSetsDirty = true;
}

// The atlas at `slots` capacity (0: freed). Device wait when it changes (frames in flight
// name the old images through the sets).
bool LLCleanPartFogVK::ensureAtlas(VkCommandBuffer cmd, uint32_t slots)
{
    if (slots == mSlots && (slots == 0 || mDens.image)) return slots > 0;
    vkDeviceWaitIdle(mCtx->device());
    destroyAtlas();
    if (slots == 0) return false;
    const uint32_t maxCols = std::max(1u, (uint32_t)std::min<uint64_t>(mCtx->limits().maxImageDimension3D, 16384u) / kN);
    const uint32_t cols = std::min(slots, maxCols);
    const uint32_t rows = (slots + cols - 1) / cols;
    if (rows > maxCols) return false;
    {
        VkMemScope memcat(VKMEM_PARTICLE);   // #407
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        mDens   = mCtx->createImage3D(cols * kN, kN, rows * kN, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        mCoarse = mCtx->createImage3D(cols * kNC, kNC, rows * kNC, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        mVel    = mCtx->createImage3D(cols * kNC, kNC, rows * kNC, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        mLight  = mCtx->createImage3D(cols * kNC, kNC, rows * kNC, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        mLDir   = mCtx->createImage3D(cols * kNC, kNC, rows * kNC, VK_FORMAT_R16G16B16A16_SFLOAT, u);
        mAccum  = mCtx->createBuffer((VkDeviceSize)slots * kSlotWords * 4u,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mDens.image || !mCoarse.image || !mVel.image || !mLight.image || !mLDir.image || !mAccum.buffer)
    {
        LL_WARNS("VKPartFog") << "particle fog atlas of " << slots << " bricks (" << (uint32_t)(slots * kSlotMB)
                              << " MB) refused (out of video memory); effect skipped, retrying later" << LL_ENDL;
        destroyAtlas();
        return false;
    }
    mSlots = slots; mCols = cols; mRows = rows;
    mPeakSlots = std::max(mPeakSlots, slots);
    const GpuImage* imgs[5] = {&mDens, &mCoarse, &mVel, &mLight, &mLDir};
    const VkClearColorValue zero[5] = {};
    clearToGeneral(cmd, imgs, zero, 5);
    vkCmdFillBuffer(cmd, mAccum.buffer, 0, VK_WHOLE_SIZE, 0u);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    LL_INFOS("VKPartFog") << "particle fog atlas: " << slots << " brick slots (" << cols << "x" << rows << ", "
                          << (uint32_t)(slots * kSlotMB + 0.5) << " MB)" << LL_ENDL;
    mSetsDirty = true;
    return true;
}

void LLCleanPartFogVK::destroyTargets()
{
    if (!mCtx) return;
    for (GpuImage* im : {&mMarchA, &mMarchB, &mHistA[0], &mHistA[1], &mHistB[0], &mHistB[1]})
    {
        if (im->image) mCtx->destroyImage(*im);
        *im = GpuImage{};
    }
    mMW = mMH = 0;
    mHistValid = false;
    mSetsDirty = true;
}

bool LLCleanPartFogVK::ensureTargets(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (mMarchA.image && w == mMW && h == mMH) return true;
    vkDeviceWaitIdle(mCtx->device());
    destroyTargets();
    {
        VkMemScope memcat(VKMEM_TARGET);   // #407
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (GpuImage* im : {&mMarchA, &mMarchB, &mHistA[0], &mHistA[1]})
            *im = mCtx->createImage2D(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        // (the history B keeps the length alone: the composite reads the march's own B)
        for (GpuImage* im : {&mHistB[0], &mHistB[1]})
            *im = mCtx->createImage2D(w, h, VK_FORMAT_R32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mMarchA.image || !mMarchB.image || !mHistA[0].image || !mHistA[1].image || !mHistB[0].image || !mHistB[1].image)
    {
        LL_WARNS("VKPartFog") << "particle fog " << w << "x" << h << " march targets refused (out of video memory); "
                                 "effect skipped, retrying later" << LL_ENDL;
        destroyTargets();
        return false;
    }
    mMW = w; mMH = h;
    // No NaN in a history; the identity (no medium, T = 1) everywhere.
    const GpuImage* imgs[6] = {&mMarchA, &mHistA[0], &mHistA[1], &mMarchB, &mHistB[0], &mHistB[1]};
    VkClearColorValue v[6]{};
    for (int i = 0; i < 3; ++i) v[i].float32[3] = 1.f;
    v[3].float32[0] = v[3].float32[1] = 65000.f; v[3].float32[2] = -65000.f;   // (history B: length 0)
    clearToGeneral(cmd, imgs, v, 6);
    mHistValid = false;
    mSetsDirty = true;
    return true;
}

void LLCleanPartFogVK::writeSets()
{
    VkDevice dev = mCtx->device();
    for (uint32_t p = 0; p < 2; ++p)
        for (uint32_t s = 0; s < 2; ++s)
        {
            const uint32_t q = p ^ 1u;
            const VkDescriptorBufferInfo bi[7] = {{mParams[s].buffer, 0, sizeof(Params)},
                                                  {mTable[s].buffer, 0, sizeof(TableGPU)},
                                                  {mPartPool, 0, VK_WHOLE_SIZE},
                                                  {mPartLight, 0, VK_WHOLE_SIZE},
                                                  {mList.buffer, 0, VK_WHOLE_SIZE},
                                                  {mAccum.buffer, 0, VK_WHOLE_SIZE},
                                                  {mResults.buffer, 0, VK_WHOLE_SIZE}};
            const VkDescriptorBufferInfo floorInfo{mFloor.buffer, 0, VK_WHOLE_SIZE};   // 28
            const VkDescriptorImageInfo ii[21] = {
                {VK_NULL_HANDLE, mDens.view, VK_IMAGE_LAYOUT_GENERAL},                 // 7
                {VK_NULL_HANDLE, mCoarse.view, VK_IMAGE_LAYOUT_GENERAL},               // 8
                {VK_NULL_HANDLE, mVel.view, VK_IMAGE_LAYOUT_GENERAL},                  // 9
                {VK_NULL_HANDLE, mLight.view, VK_IMAGE_LAYOUT_GENERAL},                // 10
                {mSampLin, mDens.view, VK_IMAGE_LAYOUT_GENERAL},                       // 11
                {mSampLin, mCoarse.view, VK_IMAGE_LAYOUT_GENERAL},                     // 12
                {mSampLin, mVel.view, VK_IMAGE_LAYOUT_GENERAL},                        // 13
                {mSampLin, mLight.view, VK_IMAGE_LAYOUT_GENERAL},                      // 14
                {VK_NULL_HANDLE, mNoise.view, VK_IMAGE_LAYOUT_GENERAL},                // 15
                {mSampRep, mNoise.view, VK_IMAGE_LAYOUT_GENERAL},                      // 16
                {mSampNear, mDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},      // 17
                {VK_NULL_HANDLE, mMarchA.view, VK_IMAGE_LAYOUT_GENERAL},               // 18
                {VK_NULL_HANDLE, mMarchB.view, VK_IMAGE_LAYOUT_GENERAL},               // 19
                {mSampLin, mHistA[q].view, VK_IMAGE_LAYOUT_GENERAL},                   // 20 last frame
                {mSampNear, mHistB[q].view, VK_IMAGE_LAYOUT_GENERAL},                  // 21 (r32f: no linear filter guaranteed)
                {VK_NULL_HANDLE, mHistA[p].view, VK_IMAGE_LAYOUT_GENERAL},             // 22 this frame
                {VK_NULL_HANDLE, mHistB[p].view, VK_IMAGE_LAYOUT_GENERAL},             // 23
                {VK_NULL_HANDLE, mLDir.view, VK_IMAGE_LAYOUT_GENERAL},                 // 24
                {mSampLin, mLDir.view, VK_IMAGE_LAYOUT_GENERAL},                       // 25
                {VK_NULL_HANDLE, mNoiseD.view, VK_IMAGE_LAYOUT_GENERAL},               // 26
                {mSampRep, mNoiseD.view, VK_IMAGE_LAYOUT_GENERAL}};                    // 27
            // Only what exists: the texture-stats / bin passes need the buffers alone (the atlas
            // and the march targets come and go with the bricks; a pass that reads them only
            // runs while they exist, and every rewrite happens behind a device wait).
            VkWriteDescriptorSet w[kBindings]{};
            uint32_t nw = 0;
            for (uint32_t i = 0; i < kBindings; ++i)
            {
                VkWriteDescriptorSet& x = w[nw];
                x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = mDset[p][s];
                x.dstBinding = i; x.descriptorCount = 1;
                if (i <= 6 || i == 28)
                {
                    const VkDescriptorBufferInfo& bf = i == 28 ? floorInfo : bi[i];
                    if (!bf.buffer) continue;
                    x.descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    x.pBufferInfo = &bf;
                }
                else
                {
                    const VkDescriptorImageInfo& im = ii[i - 7];
                    if (!im.imageView) continue;
                    x.descriptorType = im.sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    x.pImageInfo = &im;
                }
                ++nw;
            }
            vkUpdateDescriptorSets(dev, nw, w, 0, nullptr);
        }
    // Composite: [0] the raw march, [1 + p] history parity p; per frame slot (its params). B
    // is always the march's own (the temporal pass passed it through unchanged).
    for (uint32_t src = 0; src < 3 && mMarchA.view && mDepthView; ++src)
        for (uint32_t s = 0; s < 2; ++s)
        {
            const GpuImage& a = src == 0 ? mMarchA : mHistA[src - 1];
            const GpuImage& b = mMarchB;
            const VkDescriptorBufferInfo ub{mParams[s].buffer, 0, sizeof(Params)};
            const VkDescriptorImageInfo ii[3] = {{mSampNear, mDepthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                 {mSampNear, a.view, VK_IMAGE_LAYOUT_GENERAL},
                                                 {mSampNear, b.view, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet w[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mCompSet[src][s];
                w[i].dstBinding = i; w[i].descriptorCount = 1;
                w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                if (i == 0) w[i].pBufferInfo = &ub; else w[i].pImageInfo = &ii[i - 1];
            }
            vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        }
    mSetPool = mPartPool; mSetLight = mPartLight;
    mSetDepth = mDepthView; mSetDepthGen = mDepthGen;
    mSetsDirty = false;
}

// ---------------------------------------------------------------------------
// readback (this frame slot's last copy: its frame has retired -- the slot's fence was
// waited before this recording)
// ---------------------------------------------------------------------------
void LLCleanPartFogVK::readBack()
{
    RbMeta& m = mRbMeta[mSlot];
    if (!m.valid || !mReadback[mSlot].mapped) return;
    m.valid = false;
    const double now = LLFrameTimer::getElapsedSeconds();
    const uint32_t* u = static_cast<const uint32_t*>(mReadback[mSlot].mapped);
    for (uint32_t i = 0; i < m.nSys && i < kMaxSys; ++i)
    {
        auto it = mSys.find(m.em[i]);
        if (it == mSys.end() || it->second.source != m.src[i]) continue;
        const uint32_t* b = u + i * 8u;
        Sys& s = it->second;
        s.alive = b[6];
        if (b[6] == 0u) continue;
        bool ok = true;
        for (int a = 0; a < 3; ++a)
        {
            s.gmin[a] = bitsToOrdered(b[a]);
            s.gmax[a] = bitsToOrdered(b[3 + a]);
            ok = ok && std::isfinite(s.gmin[a]) && std::isfinite(s.gmax[a]) && s.gmax[a] >= s.gmin[a];
        }
        if (ok) s.gAt = now;
    }
    const float* tx = reinterpret_cast<const float*>(u + kMaxSys * 8u);
    for (uint32_t j = 0; j < m.nTex && j < kMaxTexJobs; ++j)
    {
        auto it = mTex.find(m.tex[j]);
        if (it == mTex.end()) continue;
        TexStats& t = it->second;
        t.inflight = false;
        const float* r = tx + j * 8u;
        const uint32_t w = (uint32_t)std::max(r[6], 0.f), h = (uint32_t)std::max(r[7], 0.f);
        ++t.tries;
        // A texture still at a low placeholder mip measures wrong: wait for >= 32 texels (or
        // accept after a few tries -- natively small textures exist). The first accepted
        // measurement is confirmed once more 10 s later (it may have been a placeholder's), and
        // a texture whose bindless index changed (re-resolved / sharpened) is measured again;
        // the old values stay in use meanwhile.
        if ((w >= 32 && h >= 32) || t.tries >= 6)
        {
            t.known = true;
            t.idx = m.texIdx[j];
            ++t.passes;
            t.recheckAt = t.passes == 1 ? now + 10.0 : 0.0;
            t.meanA = pfClamp01(r[0]);
            t.rgb[0] = pfClamp01(r[1]); t.rgb[1] = pfClamp01(r[2]); t.rgb[2] = pfClamp01(r[3]);
            t.sharp = std::isfinite(r[4]) ? r[4] : 1.f;
            t.hf = std::isfinite(r[5]) ? r[5] : 1.f;
            t.w = w; t.h = h;
        }
        else
            t.retryAt = now + 1.0;
    }
}

// ---------------------------------------------------------------------------
// classification (per system, CPU)
// ---------------------------------------------------------------------------
void LLCleanPartFogVK::classifyAll(const Frame& f, double now, float dt)
{
    static LLCachedControl<F32>  s_thr(gSavedSettings, "RenderVKParticleFogThreshold", 0.7f);
    static LLCachedControl<F32>  s_dist(gSavedSettings, "RenderVKParticleFogDistance", 96.f);
    static LLCachedControl<bool> s_att(gSavedSettings, "RenderVKParticleFogAttachments", false);
    static LLCachedControl<F32>  s_heavy(gSavedSettings, "RenderVKParticleFogHeavy", 0.5f);
    const float heavy = llclamp((F32)s_heavy, 0.f, 1.f);
    const float thr = llclamp((F32)s_thr, 0.1f, 1.f);
    const float rev = std::max(thr - 0.2f, 0.05f);
    const float maxDist = llclamp((F32)s_dist, 8.f, 512.f);
    const float eye[3] = {f.camTime[0], f.camTime[1], f.camTime[2]};

    std::fill(std::begin(mReasonCount), std::end(mReasonCount), 0u);
    mSeen = 0;
    mJobsNow.clear();
    mJobIdx.clear();
    const LLCleanParticleStore& ps = LLCleanParticleStore::instance();
    const uint32_t n = ps.emitterCount();
    for (uint32_t slot = 0; slot < n; ++slot)
    {
        const CleanPartFogDesc* d = ps.emitterFog(slot);
        if (!d || now - ps.emitterLastFed(slot) > 1.0) continue;
        Sys& s = mSys[slot];
        if (s.source != d->source_id || s.lastSeen <= 0.0)
        {
            const int32_t keep = s.slot;   // (a slot the old source held is released below)
            s = Sys{};
            s.slot = keep;
            s.evict = keep >= 0;
            s.source = d->source_id;
        }
        s.lastSeen = now;
        s.pos[0] = d->pos[0]; s.pos[1] = d->pos[1]; s.pos[2] = d->pos[2];
        s.texture = d->texture;
        ++mSeen;

        // ---- hard exclusions ----
        const float maxScale = std::max(std::max(std::fabs(d->start_scale[0]), std::fabs(d->start_scale[1])),
                                        std::max(std::fabs(d->end_scale[0]), std::fabs(d->end_scale[1])));
        const float spd = std::max(std::fabs(d->speed_min), std::fabs(d->speed_max));
        const uint32_t dst = d->blend_pair % LLCleanParticleStore::NUM_BLEND_FACTORS;
        uint8_t reason = R_NONE;
        if (d->hud) reason = R_HUD;
        else if (d->attachment && !s_att) reason = R_ATTACH;
        // (LL_PART_EMISSIVE_MASK is SL's "fullbright", not light: fog machines set it so the fog
        //  shows at night -- the converted medium is lit properly instead. Additive blending and
        //  real glow stay particles.)
        // Additive ONE,ONE (sparks, fire cores) and real glow stay particles. SRC_ALPHA,ONE on a big,
        // slow, long-lived system is a creator's "glowy fog": it converts (scored lower below).
        else if (d->glow[0] > 0.02f || d->glow[1] > 0.02f
                 || (dst == LLCleanParticleStore::BLEND_FACTOR_ONE
                     && !(d->blend_pair / LLCleanParticleStore::NUM_BLEND_FACTORS == LLCleanParticleStore::BLEND_FACTOR_SOURCE_ALPHA
                          && d->part_age >= 4.f && maxScale >= 1.f)))
            reason = R_BLEND;
        else if (d->flags & (LLPartData::LL_PART_RIBBON_MASK | LLPartData::LL_PART_TARGET_POS_MASK
                             | LLPartData::LL_PART_TARGET_LINEAR_MASK | LLPartData::LL_PART_FOLLOW_VELOCITY_MASK
                             | LLPartData::LL_PART_BEAM_MASK))
            reason = R_FLAGS;
        else if (maxScale < 0.5f) reason = R_SMALL;
        else if (spd > 1.f) reason = R_FAST;
        else if (d->part_age < 1.f) reason = R_SHORT;

        // ---- texture stats (queued only for systems past the cheap tests) ----
        float softTex = 0.f;
        s.texA = 0.f;
        if (reason == R_NONE)
        {
            if (d->texture.isNull()) reason = R_TEXTURE;   // untextured: a white square
            else
            {
                TexStats& t = mTex[d->texture];
                t.lastUse = now;
                if (t.inflight && now - t.jobAt > 2.0) t.inflight = false;   // (its recording never ran: again)
                if (!t.inflight && mJobsNow.size() < kMaxTexJobs && f.texIndex)
                {
                    const int32_t idx = f.texIndex(d->texture);
                    const bool due = !t.known ? now >= t.retryAt
                                              : ((t.recheckAt > 0.0 && now >= t.recheckAt) || (idx >= 0 && idx != t.idx));
                    if (due && idx >= 0 && std::find(mJobsNow.begin(), mJobsNow.end(), d->texture) == mJobsNow.end())
                    {
                        mJobsNow.push_back(d->texture);
                        mJobIdx.push_back(idx);
                        t.inflight = true;
                        t.jobAt = now;
                    }
                }
                if (!t.known) reason = R_TEXPEND;
                else
                {
                    // Shape = hard EDGES (hearts, flakes, petals). Fine internal detail is what cloud /
                    // smoke textures are made of, so it only lowers the score a little.
                    softTex = (1.f - pfStep(0.25f, 0.50f, t.sharp)) * (1.f - 0.35f * pfStep(0.15f, 0.45f, t.hf));
                    if (t.meanA > 0.85f || t.meanA < 0.01f || softTex < 0.25f) reason = R_TEXTURE;
                    s.texA = t.meanA;
                    std::memcpy(s.texRGB, t.rgb, sizeof(s.texRGB));
                }
            }
        }

        // ---- soft score ----
        float score = 0.f;
        if (reason == R_NONE)
        {
            const float big = maxScale < 1.f ? 0.5f * pfRamp(maxScale, 0.5f, 1.f)
                                             : 0.5f + 0.5f * std::sqrt(pfRamp(maxScale, 1.f, 2.5f));
            const float peakA = std::max(d->start_color[3], d->end_color[3]);
            const float cover = pfClamp01(peakA) * std::min(1.f, s.texA * 2.f);
            const float faint = 1.f - pfStep(0.35f, 0.8f, cover);
            const float acc = std::sqrt(d->accel[0] * d->accel[0] + d->accel[1] * d->accel[1] + d->accel[2] * d->accel[2]);
            // (acceleration only counts past a strong push: rising smoke / settling fog is still fog)
            const float veff = std::max(spd, 0.25f * acc * d->part_age);
            const float slow = 1.f - pfStep(0.6f, 1.2f, veff);
            const float longLived = pfStep(0.9f, 2.f, d->part_age);
            const float blendOK = d->blend_pair == LLCleanParticleStore::BLEND_PAIR_ALPHA ? 1.f
                                : (dst == LLCleanParticleStore::BLEND_FACTOR_ONE ? 1.f : 0.f);   // (SRC_ALPHA,ONE glowy fog: gated above)
            const float alive = (float)d->burst_count * d->part_age / std::max(d->burst_rate, 1.f / 30.f);
            const float enough = pfStep(5.f, 20.f, alive);
            score = big * faint * slow * longLived * blendOK * softTex * enough;
        }
        s.score = score;
        // (debug view: say why each system did or didn't convert -- once per system per 10 s)
        {
            static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKParticleFogDebug", 0);
            static std::unordered_map<U32, double> s_logged;
            if (s_dbg && now - s_logged[d->source_id] > 10.0)
            {
                s_logged[d->source_id] = now;
                if (s_logged.size() > 4096) s_logged.clear();
                const float eyeD = std::sqrt((d->pos[0] - eye[0]) * (d->pos[0] - eye[0]) + (d->pos[1] - eye[1]) * (d->pos[1] - eye[1])
                                             + (d->pos[2] - eye[2]) * (d->pos[2] - eye[2]));
                LL_INFOS("VKPartFog") << "system " << d->source_id << " at " << (int)eyeD << " m: "
                    << (reason < R_COUNT ? kReasonName[reason] : "?") << " score " << score
                    << " | scale " << d->start_scale[0] << "x" << d->start_scale[1] << " -> " << d->end_scale[0] << "x" << d->end_scale[1]
                    << " alpha " << d->start_color[3] << "->" << d->end_color[3] << " speed " << d->speed_min << ".." << d->speed_max
                    << " age " << d->part_age << " burst " << (U32)d->burst_count << "/" << d->burst_rate << " s"
                    << " accel " << std::sqrt(d->accel[0] * d->accel[0] + d->accel[1] * d->accel[1] + d->accel[2] * d->accel[2])
                    << " softTex " << softTex << " texA " << s.texA
                    << " sharp " << (d->texture.notNull() && mTex.count(d->texture) ? mTex[d->texture].sharp : -1.f)
                    << " hf " << (d->texture.notNull() && mTex.count(d->texture) ? mTex[d->texture].hf : -1.f)
                    << " flags 0x" << std::hex << d->flags << std::dec << " blend " << d->blend_pair
                    << " glow " << d->glow[0] << "/" << d->glow[1] << " tex " << d->texture
                    << (d->attachment ? " (attachment)" : "") << LL_ENDL;
            }
        }

        // ---- billow scale, heavy settling (rising smoke / steam -- pushed up -- does not) ----
        s.puff = maxScale;
        s.settleV = heavy * kSettleMax * (1.f - pfStep(0.02f, 0.2f, d->accel[2]));
        {
            float land = s.pos[2] - kFloorDrop;
            if (LLWorld::instanceExists())
                land = LLWorld::getInstance()->resolveLandHeightAgent(LLVector3(s.pos[0], s.pos[1], s.pos[2]));
            s.floorFb = std::min(std::max(land, s.pos[2] - kFloorDrop), s.pos[2]);
        }

        // ---- bounds: the particles' real extent (GPU readback) or a CPU estimate ----
        {
            const float age = std::max(d->part_age, 0.f);
            const float acc = std::sqrt(d->accel[0] * d->accel[0] + d->accel[1] * d->accel[1] + d->accel[2] * d->accel[2]);
            float r = d->burst_radius + spd * age + 0.5f * acc * age * age
                    + ((d->flags & LLPartData::LL_PART_WIND_MASK) ? age : 0.f) + 0.5f * maxScale * (1.f + kSpread);
            const float roll = s.settleV * age * kRollGain;   // (settled fog rolls out this far)
            r = llclamp(r + roll, 0.25f, 24.f);
            for (int a = 0; a < 3; ++a) { s.emin[a] = s.pos[a] - r; s.emax[a] = s.pos[a] + r; }
            if (s.settleV > 0.f) s.emin[2] = std::min(s.emin[2], s.floorFb);
            const bool gpu = now - s.gAt < 0.6 && s.alive > 0;
            const float startR = 0.5f * maxScale;
            for (int a = 0; a < 3; ++a)
            {
                float lo = gpu ? s.gmin[a] : s.emin[a], hi = gpu ? s.gmax[a] : s.emax[a];
                // the source itself (new particles appear there before the readback sees them)
                lo = std::min(lo, s.pos[a] - startR - d->burst_radius);
                hi = std::max(hi, s.pos[a] + startR + d->burst_radius);
                const float m = 0.15f + 0.05f * (hi - lo) + spd * 0.1f;
                lo -= m; hi += m;
                if (hi - lo > 48.f) { const float c = 0.5f * (lo + hi); lo = c - 24.f; hi = c + 24.f; }
                s.bmin[a] = lo; s.bmax[a] = hi;
            }
        }
        s.dist = boxDistance(eye, s.bmin, s.bmax);
        s.visible = boxVisible(f.viewProjRel, eye, s.bmin, s.bmax);
        if (s.visible) s.lastVisible = now;

        // ---- hysteresis + blend target ----
        if (reason != R_NONE) { s.converted = false; s.target = 0.f; }
        else
        {
            if (score >= thr) s.converted = true;
            else if (score < rev) s.converted = false;
            s.target = s.converted ? 1.f : (score > rev ? pfStep(rev, thr, score) : 0.f);
            if (s.target <= 0.f) reason = R_SCORE;
        }
        if (reason == R_NONE && s.dist >= maxDist) reason = R_DIST;
        const float distFade = 1.f - pfStep(0.8f * maxDist, maxDist, s.dist);
        s.want = reason == R_NONE ? s.target * distFade : 0.f;
        if (s.evict) s.want = 0.f;
        s.reason = reason;
        // priority: blend x apparent size
        const float ext = std::max(std::max(s.bmax[0] - s.bmin[0], s.bmax[1] - s.bmin[1]), s.bmax[2] - s.bmin[2]);
        s.prio = s.want * ext / std::max(s.dist, 1.f);
    }
    (void)dt;

    // Systems gone (source dead / derezzed / no longer fed): drop (a held slot is released).
    for (auto it = mSys.begin(); it != mSys.end(); )
    {
        if (now - it->second.lastSeen > 3.0) it = mSys.erase(it);
        else ++it;
    }
    // Texture stats nobody used for 2 minutes (bounded cache).
    for (auto it = mTex.begin(); it != mTex.end(); )
    {
        if (now - it->second.lastUse > 120.0 && !it->second.inflight) it = mTex.erase(it);
        else ++it;
    }
}

// This recording's texture jobs will not run: back to unknown (measured another frame).
void LLCleanPartFogVK::abortJobs()
{
    for (const LLUUID& id : mJobsNow)
    {
        auto it = mTex.find(id);
        if (it != mTex.end()) it->second.inflight = false;
    }
    mJobsNow.clear();
    mJobIdx.clear();
}

// Brick slots by priority; a holder fades out before its slot changes hands.
void LLCleanPartFogVK::allocateBricks(const Frame& f, double now)
{
    static LLCachedControl<U32> s_budget(gSavedSettings, "RenderVKParticleFogBudgetMB", 128);
    uint32_t cap = (uint32_t)std::max(1.0, std::floor((double)llclamp((U32)s_budget, 16u, 4096u) / kSlotMB));
    cap = std::min(cap, kMaxBricks);
    if (f.pressured) cap = std::max(1u, cap / 2u);   // soft: restored when the pressure eases
    mCapSlots = cap;
    const float dt = (float)llclamp(now - mLastPrepare, 0.0, 0.1);

    // Release: invisible for 2 s, faded out, or past the (shrunk) cap.
    std::vector<uint8_t> used(kMaxBricks, 0);
    for (auto& kv : mSys)
    {
        Sys& s = kv.second;
        if (s.slot < 0) continue;
        if ((uint32_t)s.slot >= cap) s.evict = true;
        if (s.evict) s.want = 0.f;
        if (now - s.lastVisible > 2.0 || (s.want <= 0.f && s.cur <= 0.005f))
        {
            s.slot = -1; s.cur = 0.f; s.evict = false;
            continue;
        }
        used[s.slot] = 1;
    }
    // Candidates by priority.
    std::vector<Sys*> cand;
    for (auto& kv : mSys)
    {
        Sys& s = kv.second;
        if (s.slot < 0 && s.want > 0.f && s.visible) cand.push_back(&s);
    }
    std::sort(cand.begin(), cand.end(), [](const Sys* a, const Sys* b) { return a->prio > b->prio; });
    for (Sys* c : cand)
    {
        int32_t freeSlot = -1;
        for (uint32_t i = 0; i < cap; ++i) if (!used[i]) { freeSlot = (int32_t)i; break; }
        if (freeSlot >= 0)
        {
            c->slot = freeSlot; c->cur = 0.f;
            used[freeSlot] = 1;
            continue;
        }
        // Full: the weakest holder fades out (its slot frees once it reached 0) if this one
        // matters clearly more.
        Sys* weakest = nullptr;
        for (auto& kv : mSys)
        {
            Sys& h = kv.second;
            if (h.slot < 0 || h.evict) continue;
            if (!weakest || h.prio < weakest->prio) weakest = &h;
        }
        if (weakest && c->prio > 1.5f * weakest->prio) { weakest->evict = true; weakest->want = 0.f; }
        c->reason = R_BUDGET;
        c->want = 0.f;
    }
    // Blend toward the target; no slot -> whole billboards.
    mLiveSlots = 0; mConverted = 0; mBlended = 0;
    uint32_t hiSlot = 0;
    for (auto& kv : mSys)
    {
        Sys& s = kv.second;
        if (s.slot < 0) { s.cur = 0.f; continue; }
        const float step = dt / kBlendTime;
        s.cur += llclamp(s.want - s.cur, -step, step);
        s.cur = pfClamp01(s.cur);
        ++mLiveSlots;
        hiSlot = std::max(hiSlot, (uint32_t)s.slot + 1u);
        if (s.cur >= 0.99f) ++mConverted; else if (s.cur > 0.f) ++mBlended;
    }
    if (mLiveSlots > 0) mLastBricksAt = now;
    (void)hiSlot;
    for (auto& kv : mSys) if (kv.second.reason < R_COUNT) ++mReasonCount[kv.second.reason];
}

// The brick geometry of a system: quantised voxel size (2^(k/4) m), origin snapped to it,
// kept while it still fits (no shimmer from a box that breathes every frame).
static void brickGeometry(float* org, float& vox, uint32_t* dims, bool& have, const float* bmin, const float* bmax,
                          float dist)
{
    const uint32_t N = LLCleanPartFogVK::kN;
    float ext = 0.f;
    for (int a = 0; a < 3; ++a) ext = std::max(ext, bmax[a] - bmin[a]);
    // ~5 cm near the camera, coarser with distance, and whatever the system's size needs.
    float v = std::max(std::max(0.05f, 0.006f * dist), ext / (float)(N - 2));
    v = std::exp2(std::ceil(std::log2(v) * 4.f) / 4.f);
    if (have && std::fabs(vox - v) < 1e-6f * v)
    {
        bool fits = true;
        for (int a = 0; a < 3; ++a)
        {
            const float hi = org[a] + (float)dims[a] * vox;
            const uint32_t need = (uint32_t)std::ceil((bmax[a] - bmin[a]) / vox) + 2u;
            fits = fits && org[a] <= bmin[a] && hi >= bmax[a] && dims[a] <= need + 4u;
        }
        if (fits) return;
    }
    for (int tries = 0; tries < 8; ++tries)
    {
        bool ok = true;
        for (int a = 0; a < 3; ++a)
        {
            org[a] = std::floor(bmin[a] / v) * v;
            const float d = std::ceil((bmax[a] - org[a]) / v);
            dims[a] = (uint32_t)std::max(1.f, d);
            ok = ok && dims[a] <= N;
        }
        if (ok) break;
        v *= 1.18920712f;   // 2^(1/4)
    }
    for (int a = 0; a < 3; ++a) dims[a] = std::min(dims[a], N);
    vox = v;
    have = true;
}

// ---------------------------------------------------------------------------
// per recording
// ---------------------------------------------------------------------------
bool LLCleanPartFogVK::prepare(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<U32> s_on(gSavedSettings, "RenderVKParticleFog", 0);
    static LLCachedControl<F32> s_density(gSavedSettings, "RenderVKParticleFogDensity", 10.f);
    static LLCachedControl<F32> s_billow(gSavedSettings, "RenderVKParticleFogBillow", 0.7f);
    static LLCachedControl<U32> s_debug(gSavedSettings, "RenderVKParticleFogDebug", 0);
    static LLCachedControl<U32> s_steps(gSavedSettings, "RenderVKParticleFogSteps", 64);
    static LLCachedControl<F32> s_res(gSavedSettings, "RenderVKParticleFogResolution", 0.5f);
    static LLCachedControl<U32> s_hist(gSavedSettings, "RenderVKParticleFogHistory", 4);
    static LLCachedControl<F32> s_nearRes(gSavedSettings, "RenderVKParticleFogNearResolution", 0.75f);
    static LLCachedControl<F32> s_erosion(gSavedSettings, "RenderVKParticleFogErosion", 1.f);
    static LLCachedControl<F32> s_bscale(gSavedSettings, "RenderVKParticleFogBillowScale", 1.f);
    static LLCachedControl<U32> s_shadowL(gSavedSettings, "RenderVKParticleFogShadowLights", 2);
    static LLCachedControl<U32> s_quality(gSavedSettings, "RenderVKParticleFogQuality", 1);
    // Quality preset over the tuning settings (which are the medium values): low halves the march
    // steps, renders at 3/4 resolution and shadows one light; high doubles the steps, renders at
    // 1.5x resolution (full near the camera) and shadows two more lights.
    const U32 quality = std::min<U32>(s_quality, 2u);
    const float qStep = quality == 0 ? 0.5f : quality == 2 ? 2.f : 1.f;
    const float qRes = quality == 0 ? 0.75f : quality == 2 ? 1.5f : 1.f;

    const bool ranPrev = mRanNow;   // (the last recording marched: the history is continuous)
    mRunning = false;
    mRanNow = false;
    if (s_on == 0 || !f.ctx)
    {
        if (!idle() && f.ctx) destroy(f.ctx->device());
        return false;
    }
    if (!f.viewProjRel || !f.camTime || !f.dslDraw0 || !f.dslDraw1 || f.rw < 2 || f.rh < 2) return false;
    mCtx = f.ctx;
    if (!ensurePipelines(f)) return false;
    const double now = LLFrameTimer::getElapsedSeconds();
    mSlot = f.frameSlot & 1u;
    mOffscreen = f.offscreen;
    readBack();

    // Prerequisites: the froxel fog (the composite orders against it), particles running and
    // shown, the lit particle VS (the only one that applies the cross-fade), the pool.
    const bool can = f.volFogNow && f.particlesOn && f.partLighting && f.partPool && f.partLight && f.partMax > 0
                  && !f.underwater && f.depthView;
    if (!can)
    {
        // Nothing to do this recording; the atlas goes after 10 s of it.
        if (mSlots && now - mLastBricksAt > 10.0) { ensureAtlas(cmd, 0); }
        return false;
    }
    mPartMax = f.partMax;

    // ---- classification + slots (live frames only: snapshots render the live state) ----
    if (!f.offscreen)
    {
        const float dt = (float)llclamp(now - mLastPrepare, 0.0, 0.1);
        classifyAll(f, now, dt);
        allocateBricks(f, now);
        mLastPrepare = now;
    }
    else
    {
        mJobsNow.clear();
        mJobIdx.clear();
    }

    // ---- resources ----
    // The fixed set (rings, splat list, results, noise) is all the texture stats and the
    // bounds need; the atlas + march targets exist only while systems hold bricks.
    if (now < mRetryAt || !ensureFixed(cmd))
    {
        if (now >= mRetryAt) mRetryAt = now + 5.0;
        abortJobs();
        return false;
    }
    // Atlas capacity: the highest held slot, in steps of 4; shrinks after 15 s of a smaller
    // need, goes after 10 s without bricks.
    uint32_t need = 0;
    for (const auto& kv : mSys) if (kv.second.slot >= 0) need = std::max(need, (uint32_t)kv.second.slot + 1u);
    uint32_t want = need ? std::min(((need + 3u) / 4u) * 4u, std::max(mCapSlots, need)) : 0u;
    if (want < mSlots && need > 0)
    {
        if (mShrinkSince <= 0.0) mShrinkSince = now;
        if (now - mShrinkSince < 15.0) want = mSlots;
    }
    else mShrinkSince = 0.0;
    if (need == 0) want = (mSlots && now - mLastBricksAt < 10.0) ? mSlots : 0u;
    if (want != mSlots && (want == 0 || now >= mAtlasRetryAt))
    {
        if (!ensureAtlas(cmd, want) && want > 0) mAtlasRetryAt = now + 5.0;
    }
    bool bricksOk = false;
    // March resolution: the setting, or the near one while a converted brick is close (the
    // targets are sized for the near one the first time it is needed -- one device wait --
    // and stay so while bricks exist; the march uses a sub-rect of them otherwise).
    const float res = llclamp((F32)s_res * qRes, 0.25f, 1.f);
    const float nearRes = std::max(res, quality == 2 ? 1.f : llclamp((F32)s_nearRes * qRes, 0.25f, 1.f));
    if (!f.offscreen)
    {
        float nearest = 1e9f;
        for (const auto& kv : mSys)
            if (kv.second.slot >= 0 && kv.second.cur > 0.f && kv.second.visible) nearest = std::min(nearest, kv.second.dist);
        if (nearest < kNearOn) { mNear = true; mNearOffSince = 0.0; }
        else if (mNear && nearest > kNearOff)
        {
            if (mNearOffSince <= 0.0) mNearOffSince = now;
            if (now - mNearOffSince > kNearHold) mNear = false;
        }
        else mNearOffSince = 0.0;
        if (mNear && nearRes > res) mNearAlloc = true;
    }
    const float allocRes = mNearAlloc ? nearRes : res;
    const float actRes = mNear ? nearRes : res;
    const uint32_t mw = std::max(1u, (uint32_t)std::lround(f.rw * allocRes)), mh = std::max(1u, (uint32_t)std::lround(f.rh * allocRes));
    if (mSlots > 0 && now >= mAtlasRetryAt)
    {
        bricksOk = ensureTargets(cmd, mw, mh);
        if (!bricksOk) mAtlasRetryAt = now + 5.0;
    }
    else if (mSlots == 0 && mMarchA.image)
    {
        vkDeviceWaitIdle(mCtx->device());
        destroyTargets();
        mNearAlloc = false;
    }
    mActW = std::min(mMW, std::max(1u, (uint32_t)std::lround(f.rw * actRes)));
    mActH = std::min(mMH, std::max(1u, (uint32_t)std::lround(f.rh * actRes)));
    mPartPool = f.partPool; mPartLight = f.partLight;
    mDepthView = f.depthView; mDepthGen = f.depthGen;
    if (mSetsDirty || mSetPool != mPartPool || mSetLight != mPartLight || mSetDepth != mDepthView || mSetDepthGen != mDepthGen)
    {
        vkDeviceWaitIdle(mCtx->device());   // (sets of frames in flight)
        writeSets();
    }

    // ---- this recording's bricks + table ----
    Params& P = *static_cast<Params*>(mParams[mSlot].mapped);
    TableGPU& T = *static_cast<TableGPU*>(mTable[mSlot].mapped);
    std::memset(&P, 0, sizeof(P));
    std::memset(&T, 0, sizeof(T));
    RbMeta& meta = mRbMeta[mSlot];
    meta = RbMeta{};
    const float eye[3] = {f.camTime[0], f.camTime[1], f.camTime[2]};
    const float density = llclamp((F32)s_density, 0.f, 16.f);
    mNumBricks = 0; mNumSys = 0;
    auto addSys = [&](uint32_t emSlot, Sys& s, uint32_t brick, uint32_t flags)
    {
        if (mNumSys >= kMaxSys) return -1;
        SysGPU& g = T.sys[mNumSys];
        g.id[0] = emSlot; g.id[1] = s.source; g.id[2] = brick; g.id[3] = flags;
        g.tex[0] = s.texA; g.tex[1] = density; g.tex[2] = (flags & 2u) ? s.cur : 0.f;
        g.col[0] = s.texRGB[0]; g.col[1] = s.texRGB[1]; g.col[2] = s.texRGB[2];
        g.heavy[0] = s.pos[0]; g.heavy[1] = s.pos[1]; g.heavy[2] = s.floorFb; g.heavy[3] = s.settleV;
        meta.em[mNumSys] = emSlot; meta.src[mNumSys] = s.source;
        s.inTable = true;
        return (int)mNumSys++;
    };
    for (auto& kv : mSys) kv.second.inTable = false;
    const float bscale = llclamp((F32)s_bscale, 0.25f, 4.f);
    // 1. holders: visible ones get a brick this recording (splat + march), all take the fade
    for (auto& kv : mSys)
    {
        Sys& s = kv.second;
        if (s.slot < 0 || s.cur <= 0.f || (uint32_t)s.slot >= mSlots || !bricksOk) continue;
        uint32_t brick = 0xFFFFFFFFu;
        if (s.visible && mNumBricks < kMaxBricks && mNumSys < kMaxSys)
        {
            brickGeometry(s.org, s.vox, s.dims, s.geom, s.bmin, s.bmax, s.dist);
            BrickGPU& b = P.bricks[mNumBricks];
            b.org_vox[0] = s.org[0]; b.org_vox[1] = s.org[1]; b.org_vox[2] = s.org[2]; b.org_vox[3] = s.vox;
            b.dims_slot[0] = (float)s.dims[0]; b.dims_slot[1] = (float)s.dims[1]; b.dims_slot[2] = (float)s.dims[2];
            b.dims_slot[3] = (float)s.slot;
            b.atlas[0] = (float)((s.slot % mCols) * kN); b.atlas[1] = 0.f; b.atlas[2] = (float)((s.slot / mCols) * kN);
            b.atlas[3] = s.cur;
            b.look[0] = (float)mNumSys; b.look[1] = s.cur;
            b.shape[0] = llclamp(kBaseScaleK * s.puff, kBaseScaleMin, kBaseScaleMax) * bscale;
            b.shape[1] = s.pos[2] + kFloorRef;
            b.shape[2] = (float)((s.source * 2654435761u) >> 8) / 16777216.f;
            // the floor map forgets when the brick moved / changed hands, and every few seconds
            const bool moved = s.floorVox != s.vox || s.floorOrg[0] != s.org[0] || s.floorOrg[1] != s.org[1]
                            || s.floorOrg[2] != s.org[2];
            mFloorClear[mNumBricks] = moved || now - s.floorAt > kFloorForget;   // (stamped when recorded)
            mBrickSys[mNumBricks] = kv.first;
            mBrickSlot[mNumBricks] = (uint32_t)s.slot;
            brick = mNumBricks++;
        }
        addSys(kv.first, s, brick, 1u | 2u);
    }
    // 2. candidates (bounds only: the brick is sized from them once converted)
    for (auto& kv : mSys)
    {
        Sys& s = kv.second;
        if (s.inTable || mNumSys >= kMaxSys) continue;
        if ((s.reason == R_NONE || s.reason == R_BUDGET || s.reason == R_SCORE) && s.score > 0.f && s.visible) addSys(kv.first, s, 0xFFFFFFFFu, 1u);
    }
    T.hdr[0] = mNumSys;
    meta.nSys = mNumSys;
    // texture jobs
    mNumJobs = (uint32_t)std::min<size_t>(mJobsNow.size(), kMaxTexJobs);
    for (uint32_t j = 0; j < mNumJobs; ++j) { T.job[j][0] = (uint32_t)mJobIdx[j]; meta.tex[j] = mJobsNow[j]; meta.texIdx[j] = mJobIdx[j]; }
    T.hdr[1] = mNumJobs;
    meta.nTex = mNumJobs;
    if (mNumSys == 0 && mNumJobs == 0)
    {
        logStats(now);   // (nothing to bin this recording)
        return false;
    }

    // ---- params ----
    invert4(f.viewProjRel, P.inv_vp);
    const double dx = (double)f.camTime[0] - mPrevEye[0];
    const double dy = (double)f.camTime[1] - mPrevEye[1];
    const double dz = (double)f.camTime[2] - mPrevEye[2];
    const bool jump = dx * dx + dy * dy + dz * dz > kCamJump * kCamJump;
    mReset = !mHistValid || !ranPrev || f.cameraCut || jump || f.offscreen || mActW != mPrevActW || mActH != mPrevActH;
    if (!mReset)
    {
        const double dl[3] = {dx, dy, dz};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                double v = (double)mPrevVP[c * 4 + r];
                if (c == 3)
                    for (int k = 0; k < 3; ++k) v += (double)mPrevVP[k * 4 + r] * dl[k];
                P.reproj[c * 4 + r] = (float)v;
            }
    }
    const bool dbg = s_debug != 0;
    P.dims[0] = (float)f.rw; P.dims[1] = (float)f.rh; P.dims[2] = (float)mActW; P.dims[3] = (float)mActH;
    P.cam[0] = eye[0]; P.cam[1] = eye[1]; P.cam[2] = eye[2];
    P.cam[3] = (float)std::fmod((double)f.camTime[3], 3600.0);
    P.march[0] = (float)std::clamp<U32>((U32)(s_steps * qStep), 8u, 256u);
    P.march[1] = (float)(mFrame & 0xFFFFu);
    P.march[2] = dbg ? 1.f : (float)std::clamp<U32>(s_hist, 1u, 16u);
    P.march[3] = mReset ? 1.f : 0.f;
    P.look[0] = 1.f;                                 // multi-scatter strength
    P.look[1] = llclamp((F32)s_billow, 0.f, 2.f);
    P.look[2] = 1.f;                                 // self-shadow strength
    P.look[3] = 1.f;                                 // ambient scale
    P.noise[0] = 0.f; P.noise[1] = kNoiseWarp; P.noise[2] = kFlowPeriod; P.noise[3] = (float)std::min<U32>(s_debug, 4u);
    P.counts[0] = (float)mNumBricks; P.counts[1] = (float)mNumSys; P.counts[2] = (float)mNumJobs; P.counts[3] = (float)mPartMax;
    P.atlas[0] = (float)(mCols * kN); P.atlas[1] = (float)kN; P.atlas[2] = (float)(mRows * kN); P.atlas[3] = (float)mMW;
    P.atlasC[0] = (float)(mCols * kNC); P.atlasC[1] = (float)kNC; P.atlasC[2] = (float)(mRows * kNC); P.atlasC[3] = (float)mMH;
    P.light[0] = 1.f;   // local-light scale (light count / cascades at record time)
    P.light[3] = kPhaseG;
    P.misc[0] = kSpread; P.misc[1] = kFadeIn; P.misc[2] = kMinKernel; P.misc[3] = kSigmaClamp;
    const float erosion = llclamp((F32)s_erosion, 0.f, 2.f);
    P.erode[0] = erosion; P.erode[1] = kLumpPerErosion * std::min(erosion, 1.5f); P.erode[2] = kPowder; P.erode[3] = kBackLobe;
    P.settle[0] = kRollGain; P.settle[1] = kSettleDelay; P.settle[2] = kSquashMin; P.settle[3] = kLocalShadowK;
    mShadowLights = std::min<U32>(quality == 0 ? std::min<U32>(s_shadowL, 1u) : quality == 2 ? s_shadowL + 2u : (U32)s_shadowL, 4u);

    if (!f.offscreen)
    {
        std::memcpy(mPrevVP, f.viewProjRel, sizeof(mPrevVP));
        mPrevEye[0] = f.camTime[0]; mPrevEye[1] = f.camTime[1]; mPrevEye[2] = f.camTime[2];
        mPrevActW = mActW; mPrevActH = mActH;
        mHistCur ^= 1u;
        ++mFrame;
    }
    logStats(now);
    mRunning = true;
    return true;
}

void LLCleanPartFogVK::recordComputeImpl(VkCommandBuffer cmd, const VkDescriptorSet draw01[2], bool partLitNow,
                                         float shadowMode, uint32_t lightCount, const std::function<void(const char*)>& mark)
{
    mRanNow = false;
    if (!mRunning || !mPipeReady || !draw01[0] || !draw01[1] || mSetsDirty) return;
    if (!partLitNow)
    {
        // No lighting records this frame: the billboards draw whole -- no volume either, and
        // the jobs' textures are measured another frame.
        abortJobs();
        mRbMeta[mSlot].valid = false;
        return;
    }
    Params& P = *static_cast<Params*>(mParams[mSlot].mapped);
    P.light[1] = (float)lightCount;
    P.light[2] = (shadowMode > 0.5f && shadowMode < 1.5f) ? 1.f : 0.f;

    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // Inputs -> these passes: the particle sim / lighting records (compute), pass 1's depth
    // (late fragment tests), the frame-info / light uploads; WAR against last frame's readers
    // (the march / temporal / composite, the copy) of everything written below.
    memBarrier(cmd,
               C | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                   | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                   | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               C | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkDescriptorSet sets[3] = {draw01[0], draw01[1], mDset[mHistCur][mSlot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 3, sets, 0, nullptr);
    auto push = [&](uint32_t a, uint32_t b = 0u)
    {
        const uint32_t q[4] = {a, b, 0u, 0u};
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, q);
    };
    push(0u);

    // The noise volumes, once (the 64^3 dispatch writes the 32^3 detail too).
    if (!mNoiseDone)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_NOISE]);
        vkCmdDispatch(cmd, 16, 16, 16);
        mNoiseDone = true;
        // (read by the march, many barriers below)
    }

    // Per-frame resets: the splat list header (indirect x = 0, y = z = 1), the bounds
    // (min = all ones, max = 0, counts 0), the accumulator of every brick slot in use.
    {
        const uint32_t hdr[4] = {0u, 1u, 1u, 0u};
        vkCmdUpdateBuffer(cmd, mList.buffer, 0, 16, hdr);
        uint32_t init[kMaxSys * 8u];
        for (uint32_t i = 0; i < kMaxSys; ++i)
        {
            for (int a = 0; a < 3; ++a) { init[i * 8u + a] = 0xFFFFFFFFu; init[i * 8u + 3u + a] = 0u; }
            init[i * 8u + 6u] = 0u; init[i * 8u + 7u] = 0u;
        }
        vkCmdUpdateBuffer(cmd, mResults.buffer, 0, sizeof(init), init);
        for (uint32_t b = 0; b < mNumBricks; ++b)
            vkCmdFillBuffer(cmd, mAccum.buffer, (VkDeviceSize)mBrickSlot[b] * kSlotWords * 4u, kSlotWords * 4u, 0u);
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    if (mNumJobs)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_TEX]);
        vkCmdDispatch(cmd, mNumJobs, 1, 1);
        mark("c_pfog_tex");
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_BIN]);
    vkCmdDispatch(cmd, (mPartMax + 63u) / 64u, 1, 1);
    mark("c_pfog_bin");
    // Bin -> the splat (indirect count + records), the copy (results), the particle VS (the
    // cross-faded lighting records, pass 2).
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
               RW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!mOffscreen && mReadback[mSlot].buffer)
    {
        VkBufferCopy r{0, 0, kResultsBytes};
        vkCmdCopyBuffer(cmd, mResults.buffer, mReadback[mSlot].buffer, 1, &r);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
        mRbMeta[mSlot].valid = true;
    }
    if (mNumBricks == 0 || !mDens.image || !mMarchA.image) return;

    // Floor maps: the bin read last frame's; this frame's density stats start at 0, and a brick
    // that moved / is due forgets its columns (the march fills them again below).
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t b = 0; b < mNumBricks; ++b)
    {
        const VkDeviceSize o = (VkDeviceSize)mBrickSlot[b] * kFloorWords * 4u;
        if (mFloorClear[b])
        {
            vkCmdFillBuffer(cmd, mFloor.buffer, o, 32u, 0u);
            vkCmdFillBuffer(cmd, mFloor.buffer, o + 32u, (VkDeviceSize)kNC * kNC * 4u, 0xFFFFFFFFu);
            auto it = mSys.find(mBrickSys[b]);
            if (it != mSys.end())
            {
                Sys& s = it->second;
                s.floorVox = s.vox;
                std::memcpy(s.floorOrg, s.org, sizeof(s.floorOrg));
                s.floorAt = LLFrameTimer::getElapsedSeconds();
            }
        }
        else vkCmdFillBuffer(cmd, mFloor.buffer, o + 16u, 8u, 0u);
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, C, RW);

    // Splat (one workgroup per record).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_SPLAT]);
    vkCmdDispatchIndirect(cmd, mList.buffer, 0);
    mark("c_pfog_splat");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);

    // Resolve: coarse, then fine (samples the coarse level).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_RESOLVE]);
    push(0u);
    vkCmdDispatch(cmd, kNC / 4u, kNC / 4u, (kNC / 4u) * mNumBricks);
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    push(1u);
    vkCmdDispatch(cmd, kN / 4u, kN / 4u, (kN / 4u) * mNumBricks);
    mark("c_pfog_resolve");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Light grid (push y: the self-shadowed local lights).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_LIGHT]);
    push(0u, mShadowLights);
    vkCmdDispatch(cmd, kNC / 4u, kNC / 4u, (kNC / 4u) * mNumBricks);
    mark("c_pfog_light");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // March.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_MARCH]);
    vkCmdDispatch(cmd, (mActW + 7u) / 8u, (mActH + 7u) / 8u, 1);
    mark("c_pfog_march");
    memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Temporal (live frames with a history length > 1).
    const bool temporal = !mOffscreen && P.march[2] > 1.5f;
    if (temporal)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[P_TEMPORAL]);
        vkCmdDispatch(cmd, (mActW + 7u) / 8u, (mActH + 7u) / 8u, 1);
        mark("c_pfog_temporal");
        memBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    mHistValid = temporal;   // (a snapshot / no-history frame leaves no usable history)
    mRanNow = true;
}

void LLCleanPartFogVK::markConverted(CleanEmitter* em, uint32_t n) const
{
    if (!em) return;
    for (const auto& kv : mSys)
        if (kv.first < n && kv.second.slot >= 0 && kv.second.cur > 0.f && kv.second.source == em[kv.first].misc[3])
            em[kv.first].misc[1] |= kEmitterFogFlag;
}

void LLCleanPartFogVK::recordComposite(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0)
{
    if (!mRanNow || !mPipe[P_COMP] || !dsetDraw0) return;
    bindGraphicsPipeline(cmd, mPipe[P_COMP]);
    const VkDescriptorSet sets[2] = {dsetDraw0, mCompSet[mHistValid ? 1u + mHistCur : 0u][mSlot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanPartFogVK::logStats(double now)
{
    static LLCachedControl<U32> s_debug(gSavedSettings, "RenderVKParticleFogDebug", 0);
    uint64_t sig = (uint64_t)mSeen | ((uint64_t)mConverted << 12) | ((uint64_t)mBlended << 24) | ((uint64_t)mLiveSlots << 36)
                 | ((uint64_t)mSlots << 44) | ((uint64_t)mCapSlots << 52);
    const double every = s_debug ? 2.0 : 10.0;
    if (now < mLogAt || (sig == mLogSig && !s_debug)) return;
    mLogAt = now + every;
    mLogSig = sig;
    std::ostringstream o;
    o << "VKPartFog: systems seen " << mSeen << ", converted " << mConverted << ", blended " << mBlended << "; excluded:";
    bool any = false;
    for (uint32_t r = R_HUD; r < R_COUNT; ++r)
        if (mReasonCount[r]) { o << " " << kReasonName[r] << " " << mReasonCount[r] << ","; any = true; }
    if (!any) o << " none,";
    o << " bricks live " << mLiveSlots << " / cap " << mCapSlots << " (atlas " << mSlots << " slots, "
      << (uint32_t)(mSlots * kSlotMB + 0.5) << " MB, peak " << mPeakSlots << "), textures measured " << mTex.size()
      << ", march " << mActW << "x" << mActH << " of " << mMW << "x" << mMH << (mNear ? " (near)" : "")
      << " (GPU: c_pfog_splat / c_pfog_light / c_pfog_march / g_pfog_comp in VKGpu)";
    LL_INFOS("VKPartFog") << o.str() << LL_ENDL;
}

bool LLCleanPartFogVK::idle() const
{
    return !mDsl && !mCompDsl && !mJob.pending() && !mPipeReady && !mDens.image && !mMarchA.image && !mList.buffer
        && !mParams[0].buffer;
}

void LLCleanPartFogVK::destroy(VkDevice dev)
{
    if (!dev) return;
    mJob.wait();
    for (VkPipeline& p : mStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    vkDeviceWaitIdle(dev);
    for (VkPipeline& p : mPipe) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    destroyTargets();
    destroyAtlas();
    destroyFixed();
    for (VkPipelineLayout* l : {&mLayout, &mCompLayout})
    {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    mPool.destroy();
    for (auto& row : mDset) for (VkDescriptorSet& s : row) s = VK_NULL_HANDLE;
    for (auto& row : mCompSet) for (VkDescriptorSet& s : row) s = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout* l : {&mDsl, &mCompDsl})
    {
        if (*l) vkdDestroySetLayout(dev, *l);
        *l = VK_NULL_HANDLE;
    }
    for (VkSampler* s : {&mSampLin, &mSampRep, &mSampNear})
    {
        if (*s) vkDestroySampler(dev, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    mPipeDsl[0] = mPipeDsl[1] = VK_NULL_HANDLE;
    for (VkFormat& fm : mPipeFormats) fm = VK_FORMAT_UNDEFINED;
    mPipeReady = mPipeFailed = false;
    mRunning = mRanNow = false;
    mSetPool = mSetLight = VK_NULL_HANDLE;
    mSetDepth = VK_NULL_HANDLE;
    mSetsDirty = true;
    mSys.clear();
    mTex.clear();
    mRbMeta[0].valid = mRbMeta[1].valid = false;
    mPeakSlots = 0;
    mLogSig = 0;
}
