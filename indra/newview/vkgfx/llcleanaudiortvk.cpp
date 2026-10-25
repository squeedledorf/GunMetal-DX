/**
 * @file llcleanaudiortvk.cpp
 * @brief Acoustics probe (#683 phase 0). See llcleanaudiortvk.h.
 */
#include "linden_common.h"

#include "llcleanaudiortvk.h"
#include "llcleanrenderervk.h"
#include "llcleanweathervk.h"   // #930: the published snow grids (acousticsSnow)
#include "vk_common.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
constexpr uint32_t kRoomRays = LLVKAcousticsResult::kRoomRays;
constexpr uint32_t kMaxSources = LLVKAcousticsRequest::kMaxSources;

// clean_audio_rt.comp AcRoomHit (std430, 32 B).
struct AcRoomHit
{
    float    dist;       // hit distance (< 0 miss)
    float    dir_z;      // the ray's world z
    uint32_t inst;       // clean Instance id (0xFFFFFFFF: miss / unresolvable)
    float    hint;       // surface hardness hint 0..1, -1 terrain, -2 water (#953)
    float    clutter;    // geometric clutter score 0..1 (< 0: none)
    float    kind;       // 1 = alpha-mask (foliage) hit, 2 = alpha-blend (glass) hit, 0 = other / miss
    float    dir_x, dir_y;   // the ray's world x / y (#858)
};
static_assert(sizeof(AcRoomHit) == 32, "AcRoomHit must match clean_audio_rt.comp (std430)");
constexpr uint32_t kMaxDiff = LLVKAcousticsRequest::kMaxDiffSources;
constexpr uint32_t kMaxQueries = LLVKAcousticsRequest::kMaxQueries;
static_assert(kMaxDiff == 4 && kMaxQueries == 128,
              "clean_audio_rt.comp AC_MAX_DIFF (uvec4 dsrc) / AC_MAX_QUERIES (2 workgroups)");
// clean_audio_rt.comp AcQuery (std430, 64 B, in) / AcQueryHit (32 B, out): #857 / #858 path queries.
static_assert(sizeof(LLVKAcousticsQuery) == 64, "AcQuery must match clean_audio_rt.comp (std430)");
struct AcQueryHit
{
    float    dist;       // RAY hit distance (< 0 miss); SEGMENT -1
    float    vis;        // |P - b| when clear, < 0 blocked
    uint32_t inst;
    float    hint;
    float    kind;
    float    p[3];       // the tested point
};
static_assert(sizeof(AcQueryHit) == 32, "AcQueryHit must match clean_audio_rt.comp (std430)");
// #683 phase 1 bounce pass (clean_audio_rt.comp AC_* / ACP_*).
constexpr uint32_t kMaxSets = LLVKAcousticsRequest::kMaxBounceSets;
constexpr uint32_t kMaxBounceWG = LLVKAcousticsRequest::kMaxBounceWorkgroups;
constexpr uint32_t kClasses = LLVKAcousticsRequest::kBounceClasses;
constexpr uint32_t kMatSlots = 256;     // AC_MAT_SLOTS
constexpr uint32_t kPartWords = 1056;   // AC_PART_WORDS
constexpr uint32_t kPHist = 0, kPEarly = 768, kPLate = 896, kPCap = 960, kPEdir = 1024, kPLdir = 1027,
                   kPEpre = 1030, kPEabs = 1033, kPEesc = 1036, kPEair = 1039, kPCnt = 1042;
constexpr double   kFix = 1048576.0;    // AC_FIX
using BSet = LLVKAcousticsBounceSet;
static_assert(kPHist + 3 * BSet::kBins == kPEarly && kPEarly + BSet::kEarlyBins == kPLate
              && kPLate + BSet::kLateBins == kPCap && kPCap + BSet::kCapBins == kPEdir && kPEdir + 3 == kPLdir
              && kPLdir + 3 == kPEpre && kPCnt + 7 <= kPartWords,
              "clean_audio_rt.comp ACP_* partial layout");
static_assert(kMaxSets == 5 && kMaxBounceWG == 32 && kClasses == 16,
              "clean_audio_rt.comp AC_MAX_BOUNCE_SETS / AC_MAX_BOUNCE_WG / AC_CLASSES");
static_assert(LLVKAcousticsRequest::kMaxBounceListenerRays + (kMaxSets - 1) * LLVKAcousticsRequest::kMaxBounceSourceRays
                  <= kMaxBounceWG * 64,
              "the bounce sets must fit the workgroup budget");
// clean_audio_rt.comp AcBounceSet (std430, 32 B).
struct AcBounceSet
{
    float    origin[4];   // xyz origin (= receiver unless cfg[2] bit 0), w first segment's tmin
    uint32_t cfg[4];      // rays, first workgroup, set flags (below), receiver end margin (float bits)
};
// cfg[2] (clean_audio_rt.comp AC_SET_*): bit 0 = the receiver is the LISTENER (pc.listener; an
// early-gain set, #683), bits 8..15 = this set's bounce cap (0 = binfo.z), bits 16..31 = its receiver
// radius in cm (0 = bparam.x).
constexpr uint32_t kSetToListener = 1u;
static_assert(sizeof(AcBounceSet) == 32, "AcBounceSet must match clean_audio_rt.comp (std430)");

uint32_t acHash(uint32_t x)   // = clean_audio_rt.comp acHash
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// clean_audio_rt.comp AcSlotRef (std430).
struct AcSlot
{
    float     src[kMaxSources][4];    // in : xyz agent space, w end margin
    AcRoomHit room[kRoomRays];        // out
    float     occ[kMaxSources];       // out: blocking hit distance (< 0 clear)
    uint32_t  hdr[4];                 // out: serial, room rays, sources, 1
    uint32_t  occ_inst[kMaxSources][2];   // out: nearest visible alpha-blend / alpha-mask surface on the source ray
    uint32_t  dsrc[kMaxDiff];         // in : #858 diffraction sources (indices into src, 0xFFFFFFFF unused)
    uint32_t  qinfo[4];               // in : x = path queries listed
    float     nee[kRoomRays][kMaxDiff];   // out: per room ray, |hit - dsrc[k]| when visible, < 0 not
    LLVKAcousticsQuery q[kMaxQueries];    // in
    AcQueryHit qh[kMaxQueries];       // out
    // ---- #683 phase 1 bounce pass (appended; clean_audio_rt.comp BOUNCE PASS) ----
    uint32_t  binfo[4];               // in : sets, workgroups, bounces, -
    uint32_t  bhdr[4];                // out: serial, sets, workgroups, 1
    float     bparam[4];              // in : receiver radius, range, hard absorption, polished scattering
    float     bair[4];                // in : air absorption per band (energy, 1/m), speed of sound
    float     bclass[kClasses][4];    // in : absorption low / mid / high, scattering
    uint32_t  bmat[kMatSlots][2];     // in : instance -> class hash (0xFFFFFFFF empty)
    AcBounceSet bset[kMaxSets];       // in
    uint32_t  bpart[kMaxBounceWG * kPartWords];   // out: one partial record per workgroup
    // ---- #930 lying snow (appended; the AC_SNOW variant only) ----
    float     sfr[4][4];              // in : inner grid frame (corner, 1 / extent, top; shear, res, tol), ring the same (res 0: none)
    float     sprm[4];                // in : strength, global depth (m), -, -
    float     rsnow[kRoomRays][2];    // out: per room ray (snow share, packed)
    float     qsnow[kMaxQueries][2];  // out: per path query's RAY hit
};
// (kMaxSources = 64 = the shader's 64 lanes: 30 channel voices + up to 34 channel-less sources,
// #892 speakers (ParcelAudioMaxSpeakers up to 32) / #783 thunder.)
static_assert(kMaxSources == 64, "the offsets below are for AC_MAX_SOURCES 64");
static_assert(kMaxSources <= 64, "one occlusion ray per lane of workgroup 0 (local_size_x 64)");
static_assert(offsetof(AcSlot, room) == 1024 && offsetof(AcSlot, occ) == 3072 && offsetof(AcSlot, hdr) == 3328
              && offsetof(AcSlot, occ_inst) == 3344 && offsetof(AcSlot, dsrc) == 3856 && offsetof(AcSlot, qinfo) == 3872
              && offsetof(AcSlot, nee) == 3888 && offsetof(AcSlot, q) == 4912 && offsetof(AcSlot, qh) == 13104
              && offsetof(AcSlot, binfo) == 17200,
              "AcSlot must match clean_audio_rt.comp AcSlotRef (std430)");
static_assert(offsetof(AcSlot, bhdr) == 17216 && offsetof(AcSlot, bparam) == 17232 && offsetof(AcSlot, bair) == 17248
              && offsetof(AcSlot, bclass) == 17264 && offsetof(AcSlot, bmat) == 17520 && offsetof(AcSlot, bset) == 19568
              && offsetof(AcSlot, bpart) == 19728,
              "AcSlot bounce region must match clean_audio_rt.comp AcSlotRef (std430)");
static_assert(offsetof(AcSlot, sfr) == 154896 && offsetof(AcSlot, sprm) == 154960 && offsetof(AcSlot, rsnow) == 154976
              && offsetof(AcSlot, qsnow) == 155488 && sizeof(AcSlot) == 156512,
              "AcSlot snow region (#930) must match clean_audio_rt.comp AcSlotRef (std430)");
constexpr VkDeviceSize kSlotStride = 156672;  // >= sizeof(AcSlot), 256-aligned records
static_assert(kSlotStride % 256 == 0, "AcSlot stride: 256-aligned records");
static_assert(sizeof(AcSlot) <= kSlotStride, "AcSlot stride");

// clean_audio_rt.comp AcPush (64 B).
struct AcPush
{
    float    listener[4];   // xyz agent space, w room ray length
    float    seed[4];       // azimuth turns, z-stratum jitter, jitter angle, foliage alpha-test distance
    uint32_t info[4];       // serial, source count, frame counter, bit 0 room rays | bit 1 alpha blocks
    uint32_t slot[2];       // device address of the frame slot's record
    uint32_t pad[2];
};
static_assert(sizeof(AcPush) == 64 && sizeof(AcPush) <= kVkPushConstantBudget, "AcPush");

// Cheap per-frame sequence in [0, 1): R2-style additive recurrence (well spread over frames).
float acSeq(uint32_t n, float alpha)
{
    const double v = 0.5 + (double)n * (double)alpha;
    return (float)(v - (double)(uint64_t)v);
}
} // namespace

LLCleanAudioRTVK::LLCleanAudioRTVK(LLCleanRendererVK& r) : mR(r)
{
}

uint32_t LLCleanAudioRTVK::slotIndex() const
{
    return mR.mFrameSlot % kSlots;
}

void LLCleanAudioRTVK::destroy(VkDevice dev)
{
    // #930 snow variant
    if (mPipeSnow) vkDestroyPipeline(dev, mPipeSnow, nullptr);
    if (mLayoutSnow) vkDestroyPipelineLayout(dev, mLayoutSnow, nullptr);
    mSnowPool.destroy();
    if (mDslSnow) vkdDestroySetLayout(dev, mDslSnow);
    mPipeSnow = VK_NULL_HANDLE;
    mLayoutSnow = VK_NULL_HANDLE;
    mDslSnow = VK_NULL_HANDLE;
    for (VkDescriptorSet& d : mSnowSet) d = VK_NULL_HANDLE;
    mSnowFrame = 0xFFFFFFFFu;
    mSnowNow = false;
    if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mPipe = VK_NULL_HANDLE;
    mLayout = VK_NULL_HANDLE;
    mLayoutRt = VK_NULL_HANDLE;
    if (mCtx && mBuf.buffer) mCtx->destroyBuffer(mBuf);
    mBuf = GpuBuffer{};
    for (SlotState& s : mSlot) s.pending = s.bounce_pending = false;
    mLatest = LLVKAcousticsResult{};
    mLatestBounce = LLVKAcousticsBounce{};
    mLastBounceDispatch = 0;
    mCtx = nullptr;
}

bool LLCleanAudioRTVK::ensure(VkDescriptorSetLayout rtLayout)
{
    if (mFailed || !mR.mCtx || !rtLayout || !mR.mDslDraw1) return false;
    if (mPipe && mLayoutRt == rtLayout) return true;
    if (mPipe)
    {
        // The RT module lives as long as the renderer, so its consumer layout never changes
        // under us; if it ever does, stop rather than destroy a pipeline still in flight.
        LL_WARNS("Vulkan") << "acoustics: RT consumer layout changed; probe disabled" << LL_ENDL;
        mFailed = true;
        return false;
    }
    mCtx = mR.mCtx;
    VkDevice dev = mCtx->device();
    if (!mBuf.buffer)
    {
        VkMemScope memcat(VKMEM_READBACK);
        // HOST_CACHED preferred (createBuffer falls back): the CPU reads back up to ~135 KB of
        // bounce partials per bounce frame (#683 phase 1), and uncached reads stall per load.
        mBuf = mCtx->createBuffer(kSlotStride * kSlots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                  true);
        if (!mBuf.buffer || !mBuf.mapped || !mBuf.address)
        {
            // #698 policy: no crash -- the probe answers nothing (acoustics stays dry).
            LL_WARNS("Vulkan") << "acoustics: result buffer allocation failed; probe disabled" << LL_ENDL;
            if (mBuf.buffer) mCtx->destroyBuffer(mBuf);
            mBuf = GpuBuffer{};
            mFailed = true;
            return false;
        }
        std::memset(mBuf.mapped, 0, (size_t)mBuf.size);   // serial 0 = never answered
    }
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AcPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    // set 0 = the RT consumer set (TLAS, instances, materials); set 1 = the draw set 1
    // (bindless textures: one smallest-mip read of a hit's MR / specular map).
    const VkDescriptorSetLayout sl[2] = {rtLayout, mR.mDslDraw1};
    li.setLayoutCount = 2; li.pSetLayouts = sl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mLayout, "mAudioRtLayout") != VK_SUCCESS)
    {
        mLayout = VK_NULL_HANDLE;
        mFailed = true;
        LL_WARNS("Vulkan") << "acoustics: pipeline layout creation failed; probe disabled" << LL_ENDL;
        return false;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = kWorldAudioRtSpv_bytes;
    ci.pCode = kWorldAudioRtSpv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    mPipe = m ? createComputePipeline(dev, m, mLayout) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mPipe)
    {
        LL_WARNS("Vulkan") << "acoustics: probe pipeline creation failed; probe disabled" << LL_ENDL;
        vkDestroyPipelineLayout(dev, mLayout, nullptr);
        mLayout = VK_NULL_HANDLE;
        mFailed = true;
        return false;
    }
    mLayoutRt = rtLayout;
    LL_INFOS("Vulkan") << "acoustics: probe ready (" << kRoomRays << " room rays + up to " << kMaxSources
                       << " occlusion rays per frame against the TLAS; + up to " << kRoomRays << " x " << kMaxDiff
                       << " diffraction shadow rays on room frames and up to " << kMaxQueries
                       << " path queries in extra workgroups)" << LL_ENDL;
    return true;
}

// #930: the AC_SNOW variant (same consumer layout + draw set 1, + set 2: the snow grids).
bool LLCleanAudioRTVK::ensureSnow()
{
    if (mSnowFailed || !mPipe || !mLayoutRt || !mCtx) return false;
    if (mPipeSnow) return true;
    if ((int32_t)(mFrame - mSnowRetry) < 0) return false;   // (#697: an out-of-memory set allocation retries later)
    VkDevice dev = mCtx->device();
    auto fail = [&](const char* what, bool latch = true)
    {
        LL_WARNS("Vulkan") << "acoustics: " << what << "; lying snow (#930) ignored" << (latch ? "" : " for now (retried)") << LL_ENDL;
        if (mPipeSnow) vkDestroyPipeline(dev, mPipeSnow, nullptr);
        if (mLayoutSnow) vkDestroyPipelineLayout(dev, mLayoutSnow, nullptr);
        mSnowPool.destroy();
        if (mDslSnow) vkdDestroySetLayout(dev, mDslSnow);
        mPipeSnow = VK_NULL_HANDLE; mLayoutSnow = VK_NULL_HANDLE; mDslSnow = VK_NULL_HANDLE;
        for (VkDescriptorSet& d : mSnowSet) d = VK_NULL_HANDLE;
        if (latch) mSnowFailed = true;
        else mSnowRetry = mFrame + 600u;
        return false;
    };
    // 0 the inner cover grid, 1 its filtered base, 2 the outer ring (storage images, read only).
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3; lci.pBindings = b;
    if (vkdCreateSetLayout(dev, &lci, &mDslSnow, "mDslAudioSnow") != VK_SUCCESS)
    {
        mDslSnow = VK_NULL_HANDLE;
        return fail("snow set layout creation failed");
    }
    if (mSnowPool.create(dev, "acoustics snow", {{mDslSnow, kSlots}}) != VK_SUCCESS) return fail("snow descriptor pool creation failed");
    for (uint32_t k = 0; k < kSlots; ++k)
        VKD_ALLOC_SOFT(mSnowPool.allocate(mDslSnow, &mSnowSet[k]), { return fail("snow descriptor set allocation failed", false); });
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AcPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout sl[3] = {mLayoutRt, mR.mDslDraw1, mDslSnow};
    li.setLayoutCount = 3; li.pSetLayouts = sl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mLayoutSnow, "mAudioRtSnowLayout") != VK_SUCCESS)
    {
        mLayoutSnow = VK_NULL_HANDLE;
        return fail("snow pipeline layout creation failed");
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = kWorldAudioRtSnowSpv_bytes;
    ci.pCode = kWorldAudioRtSnowSpv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    mPipeSnow = m ? createComputePipeline(dev, m, mLayoutSnow) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mPipeSnow) return fail("snow pipeline creation failed");
    LL_INFOS("Vulkan") << "acoustics: lying-snow variant ready (#930: one snow-grid column lookup per hit while snow lies)" << LL_ENDL;
    return true;
}

bool LLCleanAudioRTVK::snowFor(uint32_t slot, uint8_t* recBytes)
{
    AcSlot* rec = reinterpret_cast<AcSlot*>(recBytes);
    if (mSnowFrame != mFrame)
    {
        // Once per recording: this frame slot's set is not in flight (its fence was waited), and
        // record + recordBounce share it -- so it is written once, before either binds it.
        mSnowFrame = mFrame;
        mSnowNow = false;
        LLCleanWeatherVK::AcousticsSnow v;
        if (mReq.snow_strength > 0.f && mR.mWeather && mR.mWeatherNow && mR.mWeather->acousticsSnow(v) && ensureSnow()
            && slot < kSlots && mSnowSet[slot])
        {
            const bool ring = v.ring != VK_NULL_HANDLE && v.ringRes > 0u && v.ringF[2] > 0.f;
            VkDescriptorImageInfo ii[3]{};
            ii[0].imageView = v.cover; ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ii[1].imageView = v.base;  ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ii[2].imageView = ring ? v.ring : v.cover;   // (no ring: a valid rgba32f stand-in, never read: res 0)
            ii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet wr[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[i].dstSet = mSnowSet[slot]; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                wr[i].pImageInfo = &ii[i];
            }
            vkUpdateDescriptorSets(mCtx->device(), 3, wr, 0, nullptr);
            // Frames {x0, y0, extent, top plane, -, texel, shear x, shear y} -> sfr pairs.
            const float tol = std::max(v.tol, 0.05f);
            auto frame = [this](uint32_t row, const float* F, uint32_t res, float t)
            {
                mSnowIn[row][0] = F[0]; mSnowIn[row][1] = F[1]; mSnowIn[row][2] = 1.f / std::max(F[2], 1e-3f); mSnowIn[row][3] = F[3];
                mSnowIn[row + 1][0] = F[6]; mSnowIn[row + 1][1] = F[7]; mSnowIn[row + 1][2] = (float)res; mSnowIn[row + 1][3] = t;
            };
            std::memset(mSnowIn, 0, sizeof(mSnowIn));
            frame(0, v.coverF, v.coverRes, tol);
            if (ring) frame(2, v.ringF, v.ringRes, std::max(tol, 0.5f * v.ringF[2] / (float)v.ringRes));
            mSnowIn[4][0] = std::clamp(mReq.snow_strength, 0.f, 1.f);
            mSnowIn[4][1] = std::max(mReq.snow_depth, 0.f);
            mSnowNow = true;
        }
    }
    if (mSnowNow)
    {
        std::memcpy(rec->sfr, mSnowIn, sizeof(rec->sfr));
        std::memcpy(rec->sprm, mSnowIn[4], sizeof(rec->sprm));
    }
    return mSnowNow;
}

void LLCleanAudioRTVK::beginFrame()
{
    ++mFrame;
    const uint32_t s = slotIndex();
    SlotState& st = mSlot[s];
    if (st.bounce_pending && mBuf.mapped) harvestBounce(s, st);   // #683 phase 1
    st.bounce_pending = false;
    if (!st.pending || !mBuf.mapped) return;
    st.pending = false;
    const AcSlot* rec = reinterpret_cast<const AcSlot*>(static_cast<const uint8_t*>(mBuf.mapped) + s * kSlotStride);
    if (rec->hdr[0] != st.req.serial || rec->hdr[0] == 0u || rec->hdr[3] != 1u) return;   // never ran / stale
    LLVKAcousticsResult r;
    r.valid = true;
    r.serial = st.req.serial;
    r.frames_old = mFrame - st.frame;
    if (st.room)
    {
    r.room_serial = st.req.serial;
    std::memcpy(r.listener, st.req.listener, sizeof(r.listener));
    r.room_range = st.req.room_range;
    for (uint32_t i = 0; i < kRoomRays; ++i)
    {
        r.room_dist[i] = rec->room[i].dist;
        r.room_dir_z[i] = rec->room[i].dir_z;
        r.room_inst[i] = rec->room[i].inst;
        r.room_hint[i] = rec->room[i].hint;
        r.room_clutter[i] = rec->room[i].clutter;
        const float k = rec->room[i].kind;
        r.room_kind[i] = k > 1.5f ? 2 : k > 0.5f ? 1 : 0;
        r.room_dir_x[i] = rec->room[i].dir_x;
        r.room_dir_y[i] = rec->room[i].dir_y;
        for (uint32_t d = 0; d < kMaxDiff; ++d) r.room_nee[i][d] = rec->nee[i][d];
        r.room_snow[i] = st.snow ? rec->rsnow[i][0] : 0.f;   // (#930)
        r.room_snow_pack[i] = st.snow ? rec->rsnow[i][1] : 0.f;
    }
    for (uint32_t d = 0; d < kMaxDiff; ++d)   // (as record() passed them)
        r.diff_src[d] = st.req.diff_src[d] < std::min(st.req.source_count, kMaxSources) ? st.req.diff_src[d]
                                                                                       : LLVKAcousticsRequest::kNone;
    }
    else if (mLatest.valid)
    {
        // Occlusion-only record: carry the newest room rays forward (the CPU keys on room_serial).
        std::memcpy(r.listener, mLatest.listener, sizeof(r.listener));
        r.room_range = mLatest.room_range;
        r.room_serial = mLatest.room_serial;
        std::memcpy(r.room_dist, mLatest.room_dist, sizeof(r.room_dist));
        std::memcpy(r.room_dir_z, mLatest.room_dir_z, sizeof(r.room_dir_z));
        std::memcpy(r.room_inst, mLatest.room_inst, sizeof(r.room_inst));
        std::memcpy(r.room_hint, mLatest.room_hint, sizeof(r.room_hint));
        std::memcpy(r.room_clutter, mLatest.room_clutter, sizeof(r.room_clutter));
        std::memcpy(r.room_kind, mLatest.room_kind, sizeof(r.room_kind));
        std::memcpy(r.room_dir_x, mLatest.room_dir_x, sizeof(r.room_dir_x));
        std::memcpy(r.room_dir_y, mLatest.room_dir_y, sizeof(r.room_dir_y));
        std::memcpy(r.room_nee, mLatest.room_nee, sizeof(r.room_nee));
        std::memcpy(r.diff_src, mLatest.diff_src, sizeof(r.diff_src));
        std::memcpy(r.room_snow, mLatest.room_snow, sizeof(r.room_snow));
        std::memcpy(r.room_snow_pack, mLatest.room_snow_pack, sizeof(r.room_snow_pack));
    }
    else
    {
        for (uint32_t d = 0; d < kMaxDiff; ++d) r.diff_src[d] = LLVKAcousticsRequest::kNone;
    }
    r.source_count = std::min<uint32_t>(rec->hdr[2], std::min(st.req.source_count, kMaxSources));
    for (uint32_t i = 0; i < r.source_count; ++i)
    {
        r.occ_dist[i] = rec->occ[i];
        r.occ_blend_inst[i] = rec->occ_inst[i][0];
        r.occ_mask_inst[i] = rec->occ_inst[i][1];
    }
    // #857 / #858 path queries (this record only).
    r.query_count = st.queries;
    for (uint32_t i = 0; i < st.queries; ++i)
    {
        const AcQueryHit& h = rec->qh[i];
        LLVKAcousticsQueryHit& o = r.query[i];
        o.dist = h.dist;
        o.vis = h.vis;
        o.inst = h.inst;
        o.hint = h.hint;
        o.kind = h.kind > 1.5f ? 2 : h.kind > 0.5f ? 1 : 0;
        o.p[0] = h.p[0]; o.p[1] = h.p[1]; o.p[2] = h.p[2];
        o.snow = st.snow ? rec->qsnow[i][0] : 0.f;   // (#930)
        o.snow_pack = st.snow ? rec->qsnow[i][1] : 0.f;
    }
    if (!mLatest.valid || (int32_t)(r.serial - mLatest.serial) > 0) mLatest = r;
}

bool LLCleanAudioRTVK::latest(LLVKAcousticsResult& out) const
{
    if (!mLatest.valid) return false;
    out = mLatest;
    return true;
}

bool LLCleanAudioRTVK::latestBounce(LLVKAcousticsBounce& out, uint32_t seen) const
{
    if (!mLatestBounce.valid || mLatestBounce.serial == seen) return false;
    out = mLatestBounce;
    return true;
}

// #683 phase 1: sum each set's workgroup partials (fixed point -> ray energy units).
void LLCleanAudioRTVK::harvestBounce(uint32_t slot, SlotState& st)
{
    const AcSlot* rec = reinterpret_cast<const AcSlot*>(static_cast<const uint8_t*>(mBuf.mapped) + slot * kSlotStride);
    if (rec->bhdr[0] != st.bounce_serial || rec->bhdr[0] == 0u || rec->bhdr[3] != 1u
        || rec->bhdr[2] != st.bounce_wg || st.bounce_sets == 0u)
        return;   // never ran / stale
    if (mLatestBounce.valid && (int32_t)(st.bounce_serial - mLatestBounce.serial) <= 0) return;
    LLVKAcousticsBounce& b = mLatestBounce;
    b.valid = false;
    b.serial = st.bounce_serial;
    b.frames_old = mFrame - st.bounce_frame;
    b.bounces = st.bounce_max;
    b.workgroups = st.bounce_wg;
    b.range = st.bounce_range;
    b.receiver_radius = st.bounce_rr;
    b.speed = st.bounce_speed;
    std::memcpy(b.air, st.bounce_air, sizeof(b.air));
    b.set_count = std::min(st.bounce_sets, kMaxSets);
    const double inv = 1.0 / kFix;
    for (uint32_t k = 0; k < b.set_count; ++k)
    {
        BSet& o = b.set[k];
        o = BSet{};
        o.src = st.bounce_src[k];
        std::memcpy(o.origin, st.bounce_origin[k], sizeof(o.origin));
        o.to_listener = st.bounce_to_l[k];
        std::memcpy(o.recv, st.bounce_recv[k], sizeof(o.recv));
        o.recv_r = st.bounce_set_rr[k];
        o.bounces = st.bounce_set_max[k];
        // Sum in 64 bits (unsigned energies / counters, signed directions), then scale.
        uint64_t acc[kPartWords] = {};
        int64_t dir[6] = {};
        const uint32_t w0 = st.bounce_wg0[k];
        const uint32_t w1 = std::min(w0 + st.bounce_nwg[k], std::min(st.bounce_wg, kMaxBounceWG));
        for (uint32_t w = w0; w < w1; ++w)
        {
            const uint32_t* part = rec->bpart + (size_t)w * kPartWords;
            for (uint32_t i = 0; i < kPEdir; ++i) acc[i] += part[i];
            for (uint32_t i = 0; i < 6; ++i) dir[i] += (int32_t)part[kPEdir + i];
            for (uint32_t i = kPEpre; i < kPCnt + 7; ++i) acc[i] += part[i];
        }
        for (uint32_t bd = 0; bd < 3; ++bd)
            for (uint32_t i = 0; i < BSet::kBins; ++i) o.hist[bd][i] = (float)((double)acc[kPHist + bd * BSet::kBins + i] * inv);
        for (uint32_t i = 0; i < BSet::kEarlyBins; ++i) o.early[i] = (float)((double)acc[kPEarly + i] * inv);
        for (uint32_t i = 0; i < BSet::kLateBins; ++i) o.late[i] = (float)((double)acc[kPLate + i] * inv);
        for (uint32_t i = 0; i < BSet::kCapBins; ++i) o.cap[i] = (float)acc[kPCap + i];   // (ray counts)
        for (uint32_t i = 0; i < 3; ++i)
        {
            o.early_dir[i] = (float)((double)dir[i] * inv);
            o.late_dir[i] = (float)((double)dir[3 + i] * inv);
            o.e_pre[i] = (float)((double)acc[kPEpre + i] * inv);
            o.e_abs[i] = (float)((double)acc[kPEabs + i] * inv);
            o.e_esc[i] = (float)((double)acc[kPEesc + i] * inv);
            o.e_air[i] = (float)((double)acc[kPEair + i] * inv);
        }
        o.rays = (uint32_t)acc[kPCnt + 0];
        o.hits = (float)acc[kPCnt + 1];
        o.escapes = (float)acc[kPCnt + 2];
        o.capped = (float)acc[kPCnt + 3];
        o.nee_vis = (float)acc[kPCnt + 4];
        o.spec = (float)acc[kPCnt + 5];
        o.seg_len = (float)acc[kPCnt + 6] * (1.f / 16.f);
    }
    b.valid = true;
}

void LLCleanAudioRTVK::record(VkCommandBuffer cmd, VkDescriptorSetLayout rtLayout, VkDescriptorSet rtSet)
{
    if (!wanted() || !cmd || !rtSet || !mR.mDsetDraw1 || mReq.serial == 0u) return;
    if (!ensure(rtLayout)) return;
    const uint32_t s = slotIndex();
    SlotState& st = mSlot[s];
    // (This slot's previous record was harvested in beginFrame; its frame has completed.)
    AcSlot* rec = reinterpret_cast<AcSlot*>(static_cast<uint8_t*>(mBuf.mapped) + s * kSlotStride);
    const uint32_t n = std::min(mReq.source_count, kMaxSources);
    // Room rays every room_interval-th frame; the other frames trace the source rays only (and
    // nothing at all when there are no sources).
    const uint32_t interval = std::clamp<uint32_t>(mReq.room_interval, 1u, 600u);
    const bool room = (mFrame % interval) == 0u;
    const uint32_t nq = std::min(mReq.query_count, kMaxQueries);
    if (!room && n == 0u && nq == 0u) return;
    std::memset(rec, 0, offsetof(AcSlot, q));   // (inputs + header; query answers are all written)
    std::memcpy(rec->src, mReq.sources, n * 4 * sizeof(float));
    // #858 diffraction sources (room frames only: the shadow rays start at the room hits);
    // #857 / #858 path queries (extra workgroups, only as many as listed).
    for (uint32_t k = 0; k < kMaxDiff; ++k)
        rec->dsrc[k] = (room && mReq.diff_src[k] < n) ? mReq.diff_src[k] : LLVKAcousticsRequest::kNone;
    rec->qinfo[0] = nq;
    if (nq) std::memcpy(rec->q, mReq.queries, nq * sizeof(LLVKAcousticsQuery));

    const bool snow = snowFor(s, reinterpret_cast<uint8_t*>(rec));   // #930

    AcPush pc{};
    pc.listener[0] = mReq.listener[0]; pc.listener[1] = mReq.listener[1]; pc.listener[2] = mReq.listener[2];
    pc.listener[3] = std::clamp(mReq.room_range, 1.f, 256.f);
    pc.seed[0] = acSeq(mFrame, 0.7548776662f);   // (R2 sequence constants)
    pc.seed[1] = acSeq(mFrame, 0.5698402910f);
    pc.seed[2] = acSeq(mFrame, 0.6180339887f);
    pc.seed[3] = std::max(mReq.foliage_test_dist, 0.f);
    pc.info[0] = mReq.serial;
    pc.info[1] = n;
    pc.info[2] = mFrame;
    pc.info[3] = (room ? 1u : 0u) | (mReq.alpha_blocks ? 2u : 0u);
    const VkDeviceAddress addr = mBuf.address + (VkDeviceAddress)s * kSlotStride;
    pc.slot[0] = (uint32_t)addr;
    pc.slot[1] = (uint32_t)(addr >> 32);

    const VkPipelineLayout lay = snow ? mLayoutSnow : mLayout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, snow ? mPipeSnow : mPipe);
    const VkDescriptorSet sets[3] = {rtSet, mR.mDsetDraw1, snow ? mSnowSet[s] : VK_NULL_HANDLE};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, snow ? 3u : 2u, sets, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(pc);
    vkCmdPushConstants(cmd, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, 1u + (nq + 63u) / 64u, 1, 1);
    // The record -> the host read after this slot's fence.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);

    st.pending = true;
    st.frame = mFrame;
    st.room = room;
    st.queries = nq;
    st.snow = snow;
    st.req = mReq;
}

bool LLCleanAudioRTVK::recordBounce(VkCommandBuffer cmd, VkDescriptorSetLayout rtLayout, VkDescriptorSet rtSet)
{
    if (!wanted() || !mReq.bounce || !cmd || !rtSet || !mR.mDsetDraw1 || mReq.serial == 0u) return false;
    // On the room cadence, one frame after the room rays (spreads the cost; every frame at interval 1).
    const uint32_t interval = std::clamp<uint32_t>(mReq.room_interval, 1u, 600u);
    if ((mFrame % interval) != (interval >= 2u ? 1u : 0u)) return false;
    if (!ensure(rtLayout)) return false;
    const uint32_t s = slotIndex();
    SlotState& st = mSlot[s];
    AcSlot* rec = reinterpret_cast<AcSlot*>(static_cast<uint8_t*>(mBuf.mapped) + s * kSlotStride);

    // The sets: the listener, then the requested sources, each a whole number of 64-ray workgroups.
    uint32_t nsets = 0, nwg = 0;
    const uint32_t bounces = std::clamp<uint32_t>(mReq.bounce_max, 1u, LLVKAcousticsRequest::kMaxBounces);
    const float rr_global = std::clamp(mReq.receiver_radius, 0.1f, 5.f);
    auto addSet = [&](const float pos[3], float tmin, float margin, uint32_t rays, uint32_t src, bool to_l, float recv_r,
                      uint32_t set_bounces)
    {
        if (nsets >= kMaxSets) return;
        const uint32_t wgs = std::min((rays + 63u) / 64u, kMaxBounceWG - nwg);
        if (wgs == 0u) return;
        AcBounceSet& b = rec->bset[nsets];
        b.origin[0] = pos[0]; b.origin[1] = pos[1]; b.origin[2] = pos[2];
        b.origin[3] = tmin;
        b.cfg[0] = wgs * 64u;
        b.cfg[1] = nwg;
        const uint32_t cap = set_bounces ? std::clamp<uint32_t>(set_bounces, 1u, LLVKAcousticsRequest::kMaxBounces) : 0u;
        const uint32_t rr_cm = recv_r > 0.f ? (uint32_t)std::clamp(recv_r * 100.f + 0.5f, 10.f, 500.f) : 0u;
        b.cfg[2] = (to_l ? kSetToListener : 0u) | (cap << 8) | (rr_cm << 16);
        std::memcpy(&b.cfg[3], &margin, sizeof(float));
        st.bounce_wg0[nsets] = nwg;
        st.bounce_nwg[nsets] = wgs;
        st.bounce_src[nsets] = src;
        std::memcpy(st.bounce_origin[nsets], pos, 3 * sizeof(float));
        std::memcpy(st.bounce_recv[nsets], to_l ? mReq.listener : pos, 3 * sizeof(float));
        st.bounce_set_rr[nsets] = rr_cm ? (float)rr_cm * 0.01f : rr_global;
        st.bounce_set_max[nsets] = cap ? cap : bounces;
        st.bounce_to_l[nsets] = to_l;
        nwg += wgs;
        ++nsets;
    };
    if (mReq.bounce_listener)
        addSet(mReq.listener, 0.05f, 0.f,
               std::clamp<uint32_t>(mReq.bounce_rays, 64u, LLVKAcousticsRequest::kMaxBounceListenerRays),
               LLVKAcousticsRequest::kNone, false, 0.f, 0u);
    const uint32_t nsrc = std::min(mReq.source_count, kMaxSources);
    for (uint32_t k = 0; k < std::min(mReq.bounce_src_count, kMaxSets - 1u); ++k)
    {
        const LLVKAcousticsRequest::BounceSource& b = mReq.bounce_src[k];
        if (b.src >= nsrc) continue;
        const float m = std::clamp(b.margin, 0.05f, 1.5f);
        // Early gain: the receiver is the listener (no prim of its own: a small end margin).
        addSet(b.pos, m, b.to_listener ? 0.05f : m,
               std::clamp<uint32_t>(b.rays, 64u, LLVKAcousticsRequest::kMaxBounceSourceRays), b.src, b.to_listener,
               b.recv_r, b.bounces);
    }
    if (nsets == 0u || nwg == 0u) return false;
    rec->binfo[0] = nsets; rec->binfo[1] = nwg; rec->binfo[2] = bounces; rec->binfo[3] = 0u;
    rec->bparam[0] = rr_global;
    rec->bparam[1] = std::clamp(mReq.bounce_range, 5.f, 256.f);
    rec->bparam[2] = std::clamp(mReq.hard_abs, 0.f, 1.f);
    rec->bparam[3] = std::clamp(mReq.polished_scatter, 0.f, 1.f);
    for (uint32_t i = 0; i < 3; ++i) rec->bair[i] = std::max(mReq.air[i], 0.f);
    rec->bair[3] = std::clamp(mReq.speed, 100.f, 2000.f);
    std::memcpy(rec->bclass, mReq.bounce_class, sizeof(rec->bclass));
    // The instance -> class hash (= the shader's acMatClass probe: 8 slots from acHash(id)).
    for (uint32_t i = 0; i < kMatSlots; ++i) { rec->bmat[i][0] = 0xFFFFFFFFu; rec->bmat[i][1] = 0u; }
    for (uint32_t k = 0; k < std::min(mReq.bounce_mat_count, LLVKAcousticsRequest::kBounceMatMax); ++k)
    {
        const uint32_t id = mReq.bounce_mat_inst[k];
        if (id >= 0xFFFFFFFEu) continue;
        const uint32_t h = acHash(id) & (kMatSlots - 1u);
        for (uint32_t j = 0; j < 8u; ++j)
        {
            uint32_t* e = rec->bmat[(h + j) & (kMatSlots - 1u)];
            if (e[0] == id) break;   // (listed twice: the first wins)
            if (e[0] == 0xFFFFFFFFu) { e[0] = id; e[1] = mReq.bounce_mat_class[k]; break; }
        }
    }
    rec->bhdr[0] = 0u; rec->bhdr[3] = 0u;   // (written by the dispatch)
    const bool snow = snowFor(s, reinterpret_cast<uint8_t*>(rec));   // #930 (classes 10 / 11: the request's)

    AcPush pc{};
    pc.listener[0] = mReq.listener[0]; pc.listener[1] = mReq.listener[1]; pc.listener[2] = mReq.listener[2];
    pc.listener[3] = rec->bparam[1];
    pc.seed[0] = acSeq(mFrame, 0.7548776662f);
    pc.seed[1] = acSeq(mFrame, 0.5698402910f);
    pc.seed[2] = acSeq(mFrame, 0.6180339887f);
    pc.seed[3] = std::max(mReq.foliage_test_dist, 0.f);
    pc.info[0] = mReq.serial;
    pc.info[1] = 0u;
    pc.info[2] = mFrame;
    pc.info[3] = 4u | (mReq.alpha_blocks ? 2u : 0u);   // bit 2: the bounce pass
    const VkDeviceAddress addr = mBuf.address + (VkDeviceAddress)s * kSlotStride;
    pc.slot[0] = (uint32_t)addr;
    pc.slot[1] = (uint32_t)(addr >> 32);

    const VkPipelineLayout lay = snow ? mLayoutSnow : mLayout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, snow ? mPipeSnow : mPipe);
    const VkDescriptorSet sets[3] = {rtSet, mR.mDsetDraw1, snow ? mSnowSet[s] : VK_NULL_HANDLE};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, snow ? 3u : 2u, sets, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(pc);
    vkCmdPushConstants(cmd, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, nwg, 1, 1);
    // The record -> the host read after this slot's fence.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);

    st.bounce_pending = true;
    mLastBounceDispatch = mReq.serial;
    st.bounce_serial = mReq.serial;
    st.bounce_frame = mFrame;
    st.bounce_sets = nsets;
    st.bounce_wg = nwg;
    st.bounce_max = bounces;
    st.bounce_range = rec->bparam[1];
    st.bounce_rr = rec->bparam[0];
    st.bounce_speed = rec->bair[3];
    for (uint32_t i = 0; i < 3; ++i) st.bounce_air[i] = rec->bair[i];
    return true;
}
