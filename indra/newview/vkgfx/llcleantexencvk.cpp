/**
 * @file llcleantexencvk.cpp
 * @brief GPU block compression of sampled textures (#263). See llcleantexencvk.h.
 */
#include "linden_common.h"

#include "llcleantexencvk.h"
#include "llcachemetrics.h"
#include "llmemorygovernor.h"
#include "llthread.h"   // ll_set_current_thread_name (#736)
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "llapp.h"
#include "llfile.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace
{
// clean_texenc.comp EncPush (36 B).
struct EncPush
{
    uint32_t lod, w, h, bx;
    uint32_t by, out_word, mode, flags;
    uint32_t stat_rec;
};
static_assert(sizeof(EncPush) == 36, "EncPush");

uint32_t shaderMode(uint8_t enc, bool raw)
{
    if (raw) return 0u;
    switch (enc)
    {
    case TEXENC_BC1:   return 1u;
    case TEXENC_BC3:   return 2u;
    case TEXENC_BC4:   return 3u;
    case TEXENC_BC5:   return 4u;
    case TEXENC_BC7F:  return 5u;
    case TEXENC_BC7HQ: return 6u;
    default:           return 5u;
    }
}
// Channels the encoder's error covers (PSNR denominator).
uint32_t encChannels(uint8_t enc)
{
    switch (enc)
    {
    case TEXENC_BC1: return 3u;
    case TEXENC_BC4: return 1u;
    case TEXENC_BC5: return 2u;
    default:         return 4u;
    }
}
uint64_t rgbaChainBytes(uint32_t w, uint32_t h)
{
    return texChainBytesFmt(w, h, VK_FORMAT_R8G8B8A8_UNORM);
}
} // namespace

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------
VkFormat texEncFormat(uint8_t enc, bool srgb)
{
    switch (enc)
    {
    case TEXENC_BC1:   return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case TEXENC_BC3:   return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case TEXENC_BC4:   return VK_FORMAT_BC4_UNORM_BLOCK;
    case TEXENC_BC5:   return VK_FORMAT_BC5_UNORM_BLOCK;
    case TEXENC_BC7F:
    case TEXENC_BC7HQ: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    default:           return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkFormat texViewFormat(VkFormat base, bool srgb)
{
    switch (base)
    {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM:
        return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
        return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        return base;   // BC4 / BC5: UNORM only (no sRGB variant)
    }
}

VkFormat texAltFormat(VkFormat base)
{
    const VkFormat s = texViewFormat(base, true), u = texViewFormat(base, false);
    if (s == u) return VK_FORMAT_UNDEFINED;
    return base == s ? u : s;
}

VkFormat texUnormFormat(VkFormat base) { return texViewFormat(base, false); }

bool texFmtIsSrgb(VkFormat f)
{
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK
        || f == VK_FORMAT_BC7_SRGB_BLOCK;
}

bool texFmtIsBC(VkFormat f)
{
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}

uint64_t texLevelBytes(uint32_t w, uint32_t h, VkFormat f)
{
    if (!texFmtIsBC(f)) return (uint64_t)w * h * 4u;
    const uint64_t blocks = (uint64_t)((w + 3u) / 4u) * ((h + 3u) / 4u);
    const bool half = f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK
                   || f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK
                   || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK;
    return blocks * (half ? 8u : 16u);
}

uint64_t texChainBytesFmt(uint32_t w, uint32_t h, VkFormat f, uint32_t levels)
{
    uint64_t b = 0;
    w = std::max(w, 1u); h = std::max(h, 1u);
    for (uint32_t l = 0; l < levels; ++l)
    {
        b += texLevelBytes(w, h, f);
        if (w == 1 && h == 1) break;
        w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
    }
    return b;
}

uint32_t texFmtCode(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return 1u;
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: return 2u;
    case VK_FORMAT_BC4_UNORM_BLOCK: return 3u;
    case VK_FORMAT_BC5_UNORM_BLOCK: return 4u;
    case VK_FORMAT_BC7_SRGB_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: return 5u;
    default: return 0u;
    }
}

uint64_t texUuidKey(const uint8_t* id16)
{
    // Full-UUID mix of all 16 bytes (keying on 8 bytes could alias two textures).
    uint64_t klo = 0, khi = 0;
    std::memcpy(&klo, id16, sizeof(uint64_t));
    std::memcpy(&khi, id16 + sizeof(uint64_t), sizeof(uint64_t));
    return klo ^ (khi + 0x9e3779b97f4a7c15ull + (klo << 6) + (klo >> 2));
}

const char* texEncName(uint8_t enc)
{
    static const char* n[TEXENC_COUNT] = {"rgba8", "bc1", "bc3", "bc4", "bc5", "bc7f", "bc7hq"};
    return enc < TEXENC_COUNT ? n[enc] : "?";
}

// ---------------------------------------------------------------------------
bool LLCleanTexEncVK::init(VkDeviceContext* ctx)
{
    VkMemScope memcat(VKMEM_TEXENC);   // #407
    if (mPipe) return true;
    if (mInitFailed || !ctx || !ctx->textureCompressionBC()) return false;
    mCtx = ctx;
    VkDevice dev = ctx->device();
    U32 scratch_mb = 64, pool_mb = 96;
#if defined(LL_HAS_CLEAN_PIPELINE)
    scratch_mb = gSavedSettings.getU32("RenderVKTexEncodeScratchMB");
    pool_mb = gSavedSettings.getU32("RenderVKTexEncodePoolMB");
#endif
    mScratchCap = (uint64_t)std::clamp<U32>(scratch_mb, 8u, 1024u) << 20;
    mPoolCap = (uint64_t)std::clamp<U32>(pool_mb, 16u, 2048u) << 20;
    // (the block scratch ring is allocated on the first encode and released again once
    // compression is off and idle -- see ensureScratch / releaseIfIdle)
    mStats = ctx->createBuffer((VkDeviceSize)kSlots * kStatRecs * kStatWords * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mStats.buffer || !mStats.mapped)
    {
        LL_WARNS("Vulkan") << "texture compression: stats buffer unavailable; compression off" << LL_ENDL;
        destroy();
        mInitFailed = true;
        return false;
    }
    std::memset(mStats.mapped, 0, (size_t)mStats.size);

    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (auto& x : b) { x.descriptorCount = 1; x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3; li.pBindings = b;
    if (vkdCreateSetLayout(dev, &li, &mDsl, "mDslTexEnc") != VK_SUCCESS) { mDsl = VK_NULL_HANDLE; destroy(); mInitFailed = true; return false; }
    for (uint32_t s = 0; s < kSlots; ++s)
        if (mPool[s].create(dev, "texenc", {{mDsl, kMaxPerFrame}}) != VK_SUCCESS) { destroy(); mInitFailed = true; return false; }
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EncPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &mDsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &pl, &mLayout, "mTexEncLayout") != VK_SUCCESS) { mLayout = VK_NULL_HANDLE; destroy(); mInitFailed = true; return false; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = kWorldTexEncSpv_bytes; mi.pCode = kWorldTexEncSpv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, nullptr, &m) == VK_SUCCESS)
    {
        mPipe = createComputePipeline(dev, m, mLayout);
        vkDestroyShaderModule(dev, m, nullptr);
    }
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * 2u;
    if (vkCreateQueryPool(dev, &qi, nullptr, &mQuery) != VK_SUCCESS) mQuery = VK_NULL_HANDLE;
    mTsPeriod = ctx->limits().timestampPeriod;
    if (!mPipe)
    {
        LL_WARNS("Vulkan") << "texture compression: encoder pipeline creation failed; compression off" << LL_ENDL;
        destroy();
        mInitFailed = true;
        return false;
    }
    for (auto& v : mStatMeta) v.assign(kStatRecs, StatMeta{});
    LL_INFOS("Vulkan") << "texture compression encoder ready (scratch " << (mScratchCap >> 20) << " MB on first use, transient pool cap "
                       << (mPoolCap >> 20) << " MB)" << LL_ENDL;
    return true;
}

bool LLCleanTexEncVK::ensureScratch()
{
    VkMemScope memcat(VKMEM_TEXENC);   // #407
    if (mScratch.buffer) return true;
    if (!mCtx) return false;
    mScratch = mCtx->createBuffer(mScratchCap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // soft under the streamer's soft-alloc
    if (!mScratch.buffer) { mScratch = GpuBuffer{}; return false; }
    mHeadV = mTailV = 0;
    for (uint64_t& m : mMark) m = 0;
    return true;
}

uint64_t LLCleanTexEncVK::releaseIfIdle()
{
    // Everything admitted was recorded and every frame that used the ring has finished.
    if (!mCtx || mPending || !mQueued.empty() || mHeadV != mTailV) return 0;
    for (const auto& v : mInFlight) if (!v.empty()) return 0;
    uint64_t freed = 0;
    if (mScratch.buffer)
    {
        freed += mScratch.size;
        mCtx->destroyBuffer(mScratch);
        mScratch = GpuBuffer{};
        LL_INFOS("Vulkan") << "texture compression idle: block scratch and transient pool released" << LL_ENDL;
    }
    for (PoolImg& p : mFree) { freed += p.bytes; destroyPoolImg(p); }
    mFree.clear();
    return freed;
}

void LLCleanTexEncVK::destroyPoolImg(PoolImg& p)
{
    if (!mCtx) return;
    mPoolBytes -= std::min(mPoolBytes, p.bytes);
    mCtx->destroyImage(p.img);
}

void LLCleanTexEncVK::destroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (PoolImg& p : mFree) destroyPoolImg(p);
    mFree.clear();
    for (auto& v : mInFlight) { for (PoolImg& p : v) destroyPoolImg(p); v.clear(); }
    if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mPipe = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE;
    for (VkDescPool& p : mPool) p.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE;
    if (mQuery) vkDestroyQueryPool(dev, mQuery, nullptr);
    mQuery = VK_NULL_HANDLE;
    if (mScratch.buffer) mCtx->destroyBuffer(mScratch);
    if (mStats.buffer) mCtx->destroyBuffer(mStats);
    mScratch = GpuBuffer{}; mStats = GpuBuffer{};
    mHeadV = mTailV = 0;
    for (uint64_t& m : mMark) m = 0;
    mQueued.clear();
    mPending = 0;
    mCtx = nullptr;
}

// ---------------------------------------------------------------------------
void LLCleanTexEncVK::beginCall()
{
    U32 min_dim = 32;
    F32 mp = 4.f, target = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_min(gSavedSettings, "RenderVKTexCompressMinDim", 32);
    static LLCachedControl<F32> s_mp(gSavedSettings, "RenderVKTexEncodeBudgetMP", 4.f);
    static LLCachedControl<F32> s_target(gSavedSettings, "RenderVKTexEncodeTargetMs", 0.5f);
    min_dim = s_min; mp = s_mp; target = s_target;
#endif
    mMinDim = std::max<U32>(min_dim, 4u);
    mBudgetMP = std::max(mp, 0.25f);
    mTargetMs = std::max(target, 0.f);
    if (mTargetMs <= 0.f) mBudgetScale = 1.f;   // adaptation off: the caps as configured
    mCallTexelBudget = (uint64_t)(std::max(mBudgetMP * mBudgetScale, 0.25f) * 1048576.0);
    mCallTexels = 0;
}

// Budget auto-scaling (see budgetScale()). The caps were tuned on an RTX 5090 (4 MP-equivalent
// ~ 0.5 ms); a slower GPU spending several ms per frame on encodes during a texture burst is
// a visible hitch, so the per-frame budget follows the measured GPU time.
//  - Evaluated every 8 frames that recorded encodes (frames without encodes say nothing
//    about the cost), on a TRIMMED mean (the window's largest frame dropped): one spike --
//    a single 4K texture, which always fits as the call's first encode -- can't drag it down.
//  - A decrease is at most a halving per window; an increase is x1.25 while the budget is
//    what holds encodes back.
//  - Quiet recovery (budgetRecover): after 3 s with no over-target window and no refused
//    encode, x1.25 per second back toward the cap -- a scale lowered during one burst no
//    longer throttles the next one (a teleport) for the rest of the session.
static double steadySec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LLCleanTexEncVK::budgetControl(double frame_ms)
{
    if (mTargetMs <= 0.f) { mBudgetScale = 1.f; mCtlFrames = 0; return; }
    if (mCtlFrames == 0) mCtlRefused0 = mTot.refused_budget;
    mCtlMs[mCtlFrames] = frame_ms;
    if (++mCtlFrames < 8u) return;
    double sum = 0.0, mx = 0.0;
    for (double v : mCtlMs) { sum += v; mx = std::max(mx, v); }
    const double avg = (sum - mx) / 7.0;   // trimmed mean
    const bool binding = mTot.refused_budget != mCtlRefused0;   // encodes were held back by the budget
    const float before = mBudgetScale;
    constexpr float kMinScale = 1.f / 16.f;
    if (avg > 1.15 * mTargetMs)
    {
        mBudgetScale = std::max(kMinScale, mBudgetScale * (float)std::clamp((double)mTargetMs / avg, 0.5, 0.9));
        mCtlQuietSince = steadySec();
    }
    else if (avg < 0.6 * mTargetMs && binding)
        mBudgetScale = std::min(1.f, mBudgetScale * 1.25f);
    if (mBudgetScale != before)
        LL_DEBUGS("VKTex") << "texture encode budget scale " << before << " -> " << mBudgetScale << " (encode "
                           << avg << " ms/frame trimmed, max " << mx << ", target " << mTargetMs << ")" << LL_ENDL;
    mCtlFrames = 0;
}

void LLCleanTexEncVK::budgetRecover()
{
    if (mTargetMs <= 0.f || mBudgetScale >= 1.f) return;
    const double now = steadySec();
    if (mTot.refused_budget != mCtlRefusedSeen)
    {
        // refusals while the scale is low: the controller (binding) raises it if the cost
        // allows; quiet recovery waits until the burst is over
        mCtlRefusedSeen = mTot.refused_budget;
        mCtlQuietSince = now;
        return;
    }
    if (mCtlQuietSince == 0.0) mCtlQuietSince = now;
    if (now - mCtlQuietSince < 3.0 || now < mCtlNextRecover) return;
    mCtlNextRecover = now + 1.0;
    mBudgetScale = std::min(1.f, mBudgetScale * 1.25f);
}

// Encoder cost weight per texel against the per-frame budget (measured on an RTX 5090,
// 1024^2 + mips: BC1/BC3/BC5/BC7-fast ~0.15-0.2 ms, BC7 HQ ~0.6 ms).
static uint64_t encCostTexels(uint32_t w, uint32_t h, uint8_t enc)
{
    const uint64_t t = (uint64_t)w * h + (uint64_t)w * h / 3u;
    return enc == TEXENC_BC7HQ ? t * 3u : t;
}

bool LLCleanTexEncVK::check(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, uint8_t enc, uint64_t* off_out, bool count)
{
    if (!mPipe) return false;
    if (mPending >= kMaxPerFrame) { if (count) ++mTot.refused_cap; return false; }
    const uint64_t texels = encCostTexels(w, h, enc);
    // (the first encode of a call always fits the texel budget: a single huge texture
    // must not be refused forever)
    if (mCallTexels > 0 && mCallTexels + texels > mCallTexelBudget) { if (count) ++mTot.refused_budget; return false; }
    const uint64_t bytes = (texChainBytesFmt(w, h, fmt, levels) + 255ull) & ~255ull;
    // ring space (virtual offsets; an allocation never straddles the wrap)
    if (bytes == 0 || bytes > mScratchCap / 2 || !ensureScratch()) { if (count) ++mTot.refused_scratch; return false; }
    uint64_t pad = 0;
    const uint64_t phys = mHeadV % mScratchCap;
    if (phys + bytes > mScratchCap) pad = mScratchCap - phys;
    if ((mHeadV - mTailV) + pad + bytes > mScratchCap) { if (count) ++mTot.refused_scratch; return false; }
    if (off_out) *off_out = pad;
    return true;
}

bool LLCleanTexEncVK::admit(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, uint8_t enc, Job& job)
{
    uint64_t pad = 0;
    if (!check(w, h, levels, fmt, enc, &pad)) return false;
    const uint64_t bytes = (texChainBytesFmt(w, h, fmt, levels) + 255ull) & ~255ull;
    mHeadV += pad;
    job.scratch_off = mHeadV % mScratchCap;
    job.scratch_bytes = bytes;
    mHeadV += bytes;
    mCallTexels += encCostTexels(w, h, enc);
    ++mPending;
    return true;
}

GpuImage LLCleanTexEncVK::acquireTransient(uint32_t w, uint32_t h, bool srgb)
{
    VkMemScope memcat(VKMEM_TEXENC);   // #407
    if (!mCtx) return GpuImage{};
    for (size_t i = 0; i < mFree.size(); ++i)
    {
        if (mFree[i].w != w || mFree[i].h != h || mFree[i].srgb != srgb) continue;
        GpuImage g = mFree[i].img;
        mFree[i] = mFree.back(); mFree.pop_back();
        return g;
    }
    const uint64_t bytes = rgbaChainBytes(w, h);
    // over the cap: drop idle pooled images (oldest first), else refuse
    while (mPoolBytes + bytes > mPoolCap && !mFree.empty())
    {
        destroyPoolImg(mFree.front());
        mFree.erase(mFree.begin());
    }
    if (mPoolBytes + bytes > mPoolCap) { ++mTot.refused_pool; return GpuImage{}; }
    const VkFormat f = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t levels = 32u - (uint32_t)__builtin_clz(std::max(std::max(w, h), 1u));
    GpuImage img = mCtx->createImage2D(w, h, f,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, levels, srgb ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB);
    if (!img.image) { ++mTot.refused_pool; return GpuImage{}; }
    if (srgb)
    {
        // the encoder reads stored values: replace the sRGB default view with a UNORM one
        mCtx->destroyView(img.view);
        img.view = mCtx->createView(img.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1);
    }
    mPoolBytes += bytes;
    return img;
}

void LLCleanTexEncVK::giveBackTransient(const GpuImage& img)
{
    if (!img.image) return;
    PoolImg p; p.img = img; p.w = img.extent.width; p.h = img.extent.height;
    p.srgb = img.format == VK_FORMAT_R8G8B8A8_SRGB; p.bytes = rgbaChainBytes(p.w, p.h);
    mFree.push_back(p);
}

void LLCleanTexEncVK::releaseTransient(uint32_t slot, const GpuImage& img)
{
    if (!img.image) return;
    PoolImg p; p.img = img; p.w = img.extent.width; p.h = img.extent.height;
    p.srgb = img.format == VK_FORMAT_R8G8B8A8_SRGB; p.bytes = rgbaChainBytes(p.w, p.h);
    mInFlight[slot % kSlots].push_back(p);
}

// ---------------------------------------------------------------------------
void LLCleanTexEncVK::beginFrame(uint32_t slot, uint64_t frame_no)
{
    (void)frame_no;
    if (!mPipe) return;
    slot %= kSlots;
    mTailV = std::max(mTailV, std::min(mMark[slot], mHeadV));
    for (PoolImg& p : mInFlight[slot]) mFree.push_back(p);
    mInFlight[slot].clear();
    // Bounded: anything over the cap goes now (oldest first); a pool that went idle
    // shrinks to a quarter of the cap (VRAM back to the textures between bursts).
    const uint64_t keep = mPending == 0 ? mPoolCap / 4 : mPoolCap;
    while (mPoolBytes > keep && !mFree.empty())
    {
        destroyPoolImg(mFree.front());
        mFree.erase(mFree.begin());
    }
    readStats(slot);
    uint64_t enc_usec = 0;   // #715: this slot's frame of encodes, 0 = untimed
    if (mTimed[slot] && mQuery)
    {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(mCtx->device(), mQuery, slot * 2u, 2u, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0])
        {
            const double ms = (double)(ts[1] - ts[0]) * mTsPeriod * 1e-6;
            enc_usec = std::max<uint64_t>((uint64_t)(ms * 1000.0 + 0.5), 1);
            mTot.gpu_ms_window += ms;
            mTot.gpu_ms_max = std::max(mTot.gpu_ms_max, ms);
            ++mTot.timed_window;
            budgetControl(ms);
        }
        mTimed[slot] = false;
    }
    if (mSlotEncN[slot])
        LLCacheMetrics::recordStageBatch(LLCacheMetrics::Stage::BC_ENCODE, mSlotEncN[slot], mSlotEncIn[slot],
                                         mSlotEncOut[slot], enc_usec);
    mSlotEncN[slot] = 0; mSlotEncIn[slot] = mSlotEncOut[slot] = 0;
    ++mTot.frames_window;
    budgetRecover();
    mPool[slot].reset();
}

void LLCleanTexEncVK::readStats(uint32_t slot)
{
    const uint32_t n = mStatUsed[slot];
    if (!n) return;
    uint32_t* rec = reinterpret_cast<uint32_t*>(mStats.mapped) + (size_t)slot * kStatRecs * kStatWords;
    for (uint32_t i = 0; i < n; ++i)
    {
        const StatMeta& m = mStatMeta[slot][i];
        const uint32_t* r = rec + (size_t)i * kStatWords;
        const uint64_t sse = (uint64_t)r[0] | ((uint64_t)r[1] << 32);
        const double denom = (double)m.pixels * encChannels(m.enc);
        const double mse = denom > 0.0 ? (double)sse / denom : 0.0;
        const double psnr = mse > 0.0 ? 10.0 * std::log10(65025.0 / mse) : 99.0;
        PsnrAcc& a = mPsnr[m.enc < TEXENC_COUNT ? m.enc : 0];
        const int b = psnr < 30.0 ? 0 : psnr < 35.0 ? 1 : psnr < 40.0 ? 2 : psnr < 45.0 ? 3 : psnr < 50.0 ? 4 : psnr < 60.0 ? 5 : 6;
        ++a.hist[b]; ++a.n; a.sum += std::min(psnr, 99.0);
        if (psnr < a.worst) { a.worst = psnr; a.worst_key = m.key; }
        if (m.tag)
        {
            // adaptive measures: smooth-area PSNR (16 texels per block), smooth / bad shares
            const uint32_t blocks = std::max(r[3], 1u);
            const uint64_t ssse = (uint64_t)r[4] | ((uint64_t)r[5] << 32);
            const double sden = (double)r[6] * 16.0 * encChannels(m.enc);
            const double smse = sden > 0.0 ? (double)ssse / sden : 0.0;
            Result res;
            res.tag = m.tag; res.enc = m.enc;
            res.psnr = (float)std::min(psnr, 99.0);
            res.smooth_psnr = smse > 0.0 ? (float)std::min(10.0 * std::log10(65025.0 / smse), 99.0) : 99.f;
            res.smooth_frac = (float)r[6] / (float)blocks;
            res.bad_frac = (float)r[7] / (float)blocks;
            if (mResults.size() >= kMaxResults) mResults.erase(mResults.begin());   // undrained: oldest go (their trials time out)
            mResults.push_back(res);
        }
    }
    std::memset(rec, 0, (size_t)n * kStatWords * 4u);
    mStatUsed[slot] = 0;
}

void LLCleanTexEncVK::queueRecord(const Job& job, VkImage dst, VkFormat dst_fmt, uint32_t w, uint32_t h, uint32_t levels)
{
    mQueued.push_back({job, dst, dst_fmt, w, h, levels});
}

void LLCleanTexEncVK::flush(VkCommandBuffer cmd, uint32_t slot)
{
    if (mQueued.empty()) return;
    slot %= kSlots;
    VkDevice dev = mCtx->device();
    // 1. sources written by this command buffer's uploads / mip cascades (their last
    //    barrier ended at the fragment stage) -> compute reads.
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT
                        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    if (mQuery)
    {
        vkCmdResetQueryPool(cmd, mQuery, slot * 2u, 2u);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQuery, slot * 2u);
        mTimed[slot] = true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe);
    // 2. dispatches
    for (size_t qi = 0; qi < mQueued.size(); ++qi)
    {
        const Queued& q = mQueued[qi];
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (mPool[slot].allocate(mDsl, &set) != VK_SUCCESS || !set)
        {
            LL_WARNS_ONCE("Vulkan") << "texture compression: descriptor pool exhausted (encode skipped)" << LL_ENDL;
            continue;
        }
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, q.job.src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo b1{mScratch.buffer, 0, VK_WHOLE_SIZE}, b2{mStats.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr[3]{};
        for (int k = 0; k < 3; ++k)
        {
            wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = set;
            wr[k].dstBinding = (uint32_t)k; wr[k].descriptorCount = 1;
        }
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[0].pImageInfo = &ii;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &b1;
        wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].pBufferInfo = &b2;
        vkUpdateDescriptorSets(dev, 3, wr, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &set, 0, nullptr);
        // error stats on level 0 when its blocks are whole (no clamped padding texels)
        uint32_t rec = 0, flags = 0;
        if (!q.job.raw_decode && (q.w & 3u) == 0 && (q.h & 3u) == 0 && mStatUsed[slot] < kStatRecs)
        {
            rec = mStatUsed[slot]++;
            mStatMeta[slot][rec] = StatMeta{q.job.enc, q.w * q.h, q.job.key, q.job.tag};
            flags = 1u;
        }
        uint64_t off = q.job.scratch_off;
        for (uint32_t l = 0; l < q.levels; ++l)
        {
            const uint32_t lw = std::max(q.w >> l, 1u), lh = std::max(q.h >> l, 1u);
            EncPush p{};
            p.lod = q.job.src_level + l; p.w = lw; p.h = lh;
            p.bx = (lw + 3u) / 4u; p.by = (lh + 3u) / 4u;
            p.out_word = (uint32_t)(off / 4u);
            p.mode = shaderMode(q.job.enc, q.job.raw_decode);
            p.flags = (l == 0 ? flags : 0u) | (q.job.raw_decode ? 0u : (uint32_t)q.job.mask_cut << 8);   // #840: bits [8,16)
            p.stat_rec = slot * kStatRecs + rec;
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cmd, (p.bx * p.by + 63u) / 64u, 1, 1);
            off += texLevelBytes(lw, lh, q.fmt);
        }
        ++mTot.encodes[q.job.raw_decode ? 0 : q.job.enc];
        ++mSlotEncN[slot]; mSlotEncIn[slot] += rgbaChainBytes(q.w, q.h); mSlotEncOut[slot] += off - q.job.scratch_off;
        {
            const uint64_t cost = encCostTexels(q.w, q.h, q.job.enc);
            mTot.all_cost += cost;
            if (q.job.trial) { ++mTot.trial_encodes; mTot.trial_cost += cost; }
        }
    }
    // 3. blocks -> transfer reads; destinations UNDEFINED -> TRANSFER_DST
    std::vector<VkImageMemoryBarrier2> ib;
    ib.reserve(mQueued.size());
    for (const Queued& q : mQueued)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; b.srcAccessMask = 0;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = q.dst; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, q.levels, 0, 1};
        ib.push_back(b);
    }
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        d.imageMemoryBarrierCount = (uint32_t)ib.size(); d.pImageMemoryBarriers = ib.data();
        vkCmdPipelineBarrier2(cmd, &d);
    }
    // 4. copies (a skipped encode still gets its copy: defined, if wrong, content beats
    //    sampling an uninitialized image; it never happens below kMaxPerFrame)
    std::vector<VkBufferImageCopy> regs;
    for (const Queued& q : mQueued)
    {
        regs.clear();
        uint64_t off = q.job.scratch_off;
        for (uint32_t l = 0; l < q.levels; ++l)
        {
            const uint32_t lw = std::max(q.w >> l, 1u), lh = std::max(q.h >> l, 1u);
            VkBufferImageCopy r{};
            r.bufferOffset = off;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            r.imageExtent = {lw, lh, 1};
            regs.push_back(r);
            off += texLevelBytes(lw, lh, q.fmt);
        }
        vkCmdCopyBufferToImage(cmd, mScratch.buffer, q.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (uint32_t)regs.size(), regs.data());
    }
    // 5. destinations -> sampled
    for (VkImageMemoryBarrier2& b : ib)
    {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    {
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = (uint32_t)ib.size(); d.pImageMemoryBarriers = ib.data();
        vkCmdPipelineBarrier2(cmd, &d);
    }
    if (mQuery) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQuery, slot * 2u + 1u);
    mPending -= std::min<uint32_t>(mPending, (uint32_t)mQueued.size());
    mQueued.clear();
}

void LLCleanTexEncVK::endFrame(uint32_t slot)
{
    mMark[slot % kSlots] = mHeadV;
}

// ---------------------------------------------------------------------------
std::string LLCleanTexEncVK::statsLine()
{
    std::ostringstream o;
    auto MB = [](uint64_t b) { return (unsigned long long)((b + (1u << 19)) >> 20); };
    o << "encodes";
    bool any = false;
    for (uint32_t e = 1; e < TEXENC_COUNT; ++e)
        if (mTot.encodes[e]) { o << " " << texEncName(e) << " " << mTot.encodes[e]; any = true; }
    if (mTot.encodes[0]) { o << " decode " << mTot.encodes[0]; any = true; }
    if (!any) o << " 0";
    const double avg = mTot.frames_window ? mTot.gpu_ms_window / mTot.frames_window : 0.0;
    const double avg_enc = mTot.timed_window ? mTot.gpu_ms_window / mTot.timed_window : 0.0;
    o << std::fixed << std::setprecision(3) << "; encode gpu " << avg << " ms/frame (max " << mTot.gpu_ms_max
      << ", " << avg_enc << " per encoding frame, target " << std::setprecision(2) << mTargetMs << ")"
      << "; budget " << mBudgetMP * mBudgetScale << "/" << mBudgetMP << " MP (scale " << mBudgetScale << ")"
      << std::setprecision(3)
      << std::setprecision(0) << "; pool " << MB(mPoolBytes) << "/" << MB(mPoolCap) << "MB scratch "
      << MB(mHeadV - mTailV) << "/" << MB(mScratchCap) << "MB";
    if (mTot.refused_budget || mTot.refused_scratch || mTot.refused_pool || mTot.refused_cap)
        o << "; deferred budget " << mTot.refused_budget << " scratch " << mTot.refused_scratch
          << " pool " << mTot.refused_pool << " cap " << mTot.refused_cap;
    mTot.gpu_ms_window = 0.0; mTot.gpu_ms_max = 0.0; mTot.frames_window = 0; mTot.timed_window = 0;
    return o.str();
}

std::string LLCleanTexEncVK::psnrReport(const std::function<std::string(uint64_t)>& name)
{
    std::ostringstream o;
    bool any = false;
    for (uint32_t e = 1; e < TEXENC_COUNT; ++e)
    {
        PsnrAcc& a = mPsnr[e];
        if (!a.n) continue;
        if (any) o << " | ";
        any = true;
        o << texEncName(e) << " n=" << a.n << std::fixed << std::setprecision(1) << " mean " << a.sum / a.n
          << " dB [<30:" << a.hist[0] << " 30-35:" << a.hist[1] << " 35-40:" << a.hist[2] << " 40-45:" << a.hist[3]
          << " 45-50:" << a.hist[4] << " 50-60:" << a.hist[5] << " 60+:" << a.hist[6] << "] worst " << a.worst
          << " dB (" << name(a.worst_key) << ")";
        a = PsnrAcc{};
    }
    return any ? o.str() : std::string();
}

// ===========================================================================
// LLCleanTexFacts (see the header)
// ===========================================================================
namespace
{
constexpr char     kFactsMagic[8]   = {'S', 'L', 'V', 'K', 'T', 'C', 'D', '\0'};
constexpr uint32_t kFactsFormat     = 1;
struct FactsHeader
{
    char     magic[8];
    uint32_t formatVersion;
    uint32_t headerBytes;
    uint32_t encoderVersion;
    uint32_t recBytes;
    uint64_t count;
    uint32_t crc;
    uint32_t pad;
};
static_assert(sizeof(FactsHeader) == 40, "FactsHeader");
static_assert(sizeof(LLCleanTexFacts::Rec) == 40, "LLCleanTexFacts::Rec");

struct CrcTable
{
    uint32_t t[256];
    CrcTable()
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
uint32_t factsCrc(const uint8_t* p, size_t n)
{
    static const CrcTable tab;   // thread-safe initialization (magic static)
    const uint32_t* t = tab.t;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
} // namespace

LLCleanTexFacts::~LLCleanTexFacts()
{
    if (mLoadThread.joinable()) mLoadThread.join();
    if (mSaveThread.joinable()) mSaveThread.join();
}

void LLCleanTexFacts::startLoad(const std::string& path)
{
    if (mLoadThread.joinable() || mMerged) return;
    mPath = path;
    if (path.empty()) { mMerged = true; mState = "memory only"; return; }
    mState = "loading";
    mLoadThread = std::thread([this, path]()
    {
        // #698: a raw std::thread -- out of memory must not std::terminate. The facts are
        // an optimisation: give up the load (memory-only facts this session).
        ll_set_current_thread_name("VK texfacts load");
        try
        {
            std::vector<Rec> recs;
            std::string st;
            LLFILE* f = LLFile::fopen(path, "rb");
            if (!f) st = "no file (first run or cleared)";
            else
            {
                FactsHeader h{};
                std::string bad;
                if (fread(&h, 1, sizeof(h), f) != sizeof(h))                    bad = "truncated header";
                else if (std::memcmp(h.magic, kFactsMagic, sizeof(kFactsMagic)) != 0) bad = "bad magic";
                else if (h.formatVersion != kFactsFormat || h.headerBytes != sizeof(h) || h.recBytes != sizeof(Rec))
                    bad = "file format version";
                else if (h.encoderVersion != kEncoderVersion)                   bad = "encoder version changed";
                else if (h.count > kMaxEntries)                                 bad = "over the entry cap";
                if (bad.empty())
                {
                    recs.resize((size_t)h.count);
                    const size_t bytes = recs.size() * sizeof(Rec);
                    if (bytes && fread(recs.data(), 1, bytes, f) != bytes)       bad = "truncated payload";
                    else if (fgetc(f) != EOF)                                   bad = "trailing bytes";
                    else if (factsCrc(reinterpret_cast<const uint8_t*>(recs.data()), bytes) != h.crc) bad = "CRC mismatch";
                }
                LLFile::close(f);
                if (!bad.empty())
                {
                    recs.clear();
                    LLFile::remove(path, ENOENT);   // rewritten from this session's facts
                    st = "discarded (" + bad + ")";
                }
                else st = "loaded";
            }
            std::unordered_map<uint64_t, Rec> m;
            m.reserve(recs.size() + 4096);
            uint32_t max_stamp = 0;
            for (const Rec& r : recs)
            {
                max_stamp = std::max(max_stamp, r.stamp);
                m.emplace(texUuidKey(r.id) | 1ull, r);
            }
            mLoadMap.swap(m);
            mLoadMaxStamp = max_stamp;
            mLoadState = st;
            mLoadDone.store(true, std::memory_order_release);
        }
        catch (const std::bad_alloc&)
        {
            LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_THREAD, 0, "texture facts load");
            mLoadMap.clear();
            mLoadMaxStamp = 0;
            mLoadState = "not loaded (out of memory)";
            mLoadDone.store(true, std::memory_order_release);
        }
    });
}

void LLCleanTexFacts::poll(double now_s)
{
    if (!mMerged && mLoadDone.load(std::memory_order_acquire))
    {
        if (mLoadThread.joinable()) mLoadThread.join();
        // The worker built the map; facts learned this session before the load finished
        // (few) are moved over it and take precedence, then the maps swap.
        mLoadedCount = mLoadMap.size();
        const uint32_t base = mLoadMaxStamp + 1u;
        for (auto& kv : mMap)
        {
            kv.second.stamp += base;   // this session's uses stay the most recent
            mLoadMap[kv.first] = kv.second;
        }
        mClock = std::max(mClock + base, base);
        mMap.swap(mLoadMap);
        std::unordered_map<uint64_t, Rec>().swap(mLoadMap);
        mState = mLoadState;
        mMerged = true;
        evict();
        LL_INFOS("VKTex") << "texture facts (" << mPath << "): " << mState << ", " << mLoadedCount << " textures" << LL_ENDL;
        if (mNextSave == 0.0) mNextSave = now_s + 300.0;
    }
    if (mNextSave == 0.0) mNextSave = now_s + 300.0;
    // Periodic save: every 5 minutes when something changed, written on a worker from a
    // snapshot (a crash then loses at most a few minutes of measures).
    if (mMerged && mDirty && !mPath.empty() && now_s >= mNextSave && !mSaveBusy.load(std::memory_order_acquire))
    {
        mNextSave = now_s + 300.0;
        if (mSaveThread.joinable()) mSaveThread.join();
        std::vector<Rec> snap;
        snap.reserve(mMap.size());
        for (const auto& kv : mMap) snap.push_back(kv.second);
        mDirty = false;
        mSaveBusy.store(true, std::memory_order_release);
        mSaveThread = std::thread([this, snap = std::move(snap)]()
        {
            // #698: a raw std::thread -- out of memory skips this save (retried in 300 s)
            ll_set_current_thread_name("VK texfacts save");
            try { writeFile(snap); }
            catch (const std::bad_alloc&) { LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_THREAD, 0, "texture facts save"); }
            mSaveBusy.store(false, std::memory_order_release);
        });
    }
}

void LLCleanTexFacts::shutdown()
{
    if (mLoadThread.joinable()) mLoadThread.join();
    if (!mMerged && mLoadDone.load(std::memory_order_acquire)) poll(0.0);   // merge, so the save keeps the file's facts
    if (mSaveThread.joinable()) mSaveThread.join();
    if (mMerged && mDirty && !mPath.empty())
    {
        std::vector<Rec> snap;
        snap.reserve(mMap.size());
        for (const auto& kv : mMap) snap.push_back(kv.second);
        if (writeFile(snap)) mDirty = false;
    }
}

bool LLCleanTexFacts::writeFile(const std::vector<Rec>& recs) const
{
    FactsHeader h{};
    std::memcpy(h.magic, kFactsMagic, sizeof(kFactsMagic));
    h.formatVersion = kFactsFormat;
    h.headerBytes = sizeof(h);
    h.encoderVersion = kEncoderVersion;
    h.recBytes = sizeof(Rec);
    h.count = recs.size();
    const size_t bytes = recs.size() * sizeof(Rec);
    h.crc = factsCrc(reinterpret_cast<const uint8_t*>(recs.data()), bytes);
    const std::string tmp = mPath + ".tmp" + std::to_string(LLApp::getPid());
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f) { LL_WARNS("VKTex") << "texture facts: cannot write " << tmp << LL_ENDL; return false; }
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h) && (bytes == 0 || fwrite(recs.data(), 1, bytes, f) == bytes);
    ok = (fflush(f) == 0) && ok;
    ok = (LLFile::close(f) == 0) && ok;
    if (ok)
    {
#if LL_WINDOWS
        LLFile::remove(mPath, ENOENT);   // _wrename does not replace an existing file
#endif
        ok = LLFile::rename(tmp, mPath) == 0;
    }
    if (!ok)
    {
        LLFile::remove(tmp, ENOENT);   // never leave a partial file behind
        LL_WARNS("VKTex") << "texture facts: save to " << mPath << " failed" << LL_ENDL;
        return false;
    }
    LL_DEBUGS("VKTex") << "texture facts: saved " << recs.size() << " textures" << LL_ENDL;
    return true;
}

const LLCleanTexFacts::Rec* LLCleanTexFacts::find(uint64_t key) const
{
    auto it = mMap.find(key);
    return it == mMap.end() ? nullptr : &it->second;
}

LLCleanTexFacts::Rec& LLCleanTexFacts::edit(uint64_t key, const uint8_t* id16)
{
    if (mMap.size() >= kMaxEntries && mMap.find(key) == mMap.end()) evict();
    Rec& r = mMap[key];
    std::memcpy(r.id, id16, 16);
    r.stamp = mClock++;
    mDirty = true;
    return r;
}

void LLCleanTexFacts::touch(uint64_t key)
{
    auto it = mMap.find(key);
    if (it == mMap.end()) return;
    it->second.stamp = mClock++;
    // (a touch alone doesn't schedule a save: LRU order rides along with real changes)
}

// Least recently used out: down to 7/8 of the cap in one go (amortized O(1) per insert).
void LLCleanTexFacts::evict()
{
    if (mMap.size() < kMaxEntries) return;
    const size_t keep = kMaxEntries - kMaxEntries / 8;
    const size_t drop = mMap.size() - keep;
    std::vector<uint32_t> st;
    st.reserve(mMap.size());
    for (const auto& kv : mMap) st.push_back(kv.second.stamp);
    std::nth_element(st.begin(), st.begin() + (drop - 1), st.end());
    const uint32_t cut = st[drop - 1];
    size_t n = 0;
    for (auto it = mMap.begin(); it != mMap.end() && n < drop; )
    {
        if (it->second.stamp <= cut) { it = mMap.erase(it); ++n; }
        else ++it;
    }
    mDirty = true;
}
