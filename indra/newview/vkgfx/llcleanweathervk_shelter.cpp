/**
 * @file llcleanweathervk_shelter.cpp
 * @brief Per-avatar shelter maps (#901): umbrellas and hats keep their wearer (and what is under
 *        them) dry, rain splashes on a canopy; M3+ the avatars' own snow. See llcleanweathervk.h
 *        (#901 PER-AVATAR SHELTER MAPS) and clean_weather_shelter.comp / .glsl.
 *
 * The GPU does the work: one chain per recording right after the main cull rasterizes the
 * visible avatars' skinned / worn geometry top-down into their layers. This side picks the
 * layers (a bounded list: the joint-table slots' avatars by distance), schedules which are
 * re-rastered (the own avatar every recording, the others round-robin), writes the record's
 * 384 B header, owns the atlas (soft allocation, tier configurations by VRAM pressure) and keeps
 * the stats from the record's readback.
 */
#include "linden_common.h"

#include "llcleanweathervk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llagent.h"
#include "llcleanskinstore.h"
#include "llerror.h"
#include "llmath.h"
#include "llmemtrend.h"
#include "llformat.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Tier configurations: 0 = 4 x 256^2 (the own avatar + the 3 closest) + 12 x 128^2 (atlas
// 1024 x 512), 1 = 16 x 128^2 (1024 x 256), 2 = 8 x 128^2 (1024 x 128), 3 = none. The atlas images
// are 1024 wide (+ kShSlotRows rows of per-slot frames / #901 wetness: one column per joint-table slot).
constexpr uint32_t kShAtlasW = 1024;
constexpr uint32_t kShBodyRows = 8;   // #783 (clean_weather_shelter.glsl WX_SH_BODY_ROWS: the raster target's frame rows)
constexpr uint32_t kShSlotRows = 7;   // (clean_weather_shelter.glsl WX_SH_SLOT_ROWS: frame x4, #901 wetness, exposure, layer directory)
constexpr uint32_t kShConfigNone = 3;
uint32_t shLayersOf(uint32_t config) { return config == 0 ? 16u : config == 1 ? 16u : config == 2 ? 8u : 0u; }
uint32_t shTilesH(uint32_t config) { return config == 0 ? 512u : config == 1 ? 256u : config == 2 ? 128u : 0u; }
// Layer L's tile in a configuration.
void shTile(uint32_t config, uint32_t L, uint32_t& res, uint32_t& x0, uint32_t& y0)
{
    if (config == 0 && L < 4)
    {
        res = 256; x0 = L * 256u; y0 = 0;
        return;
    }
    const uint32_t j = config == 0 ? L - 4u : L;
    res = 128; x0 = (j % 8u) * 128u; y0 = (config == 0 ? 256u : 0u) + (j / 8u) * 128u;
}
// The own avatar + this many closest take the 256^2 tiles (configuration 0).
constexpr uint32_t kShTierA = 4;
// Extent (m): the minimum (a person, arms out), the margin added to twice the reach.
constexpr float kShExtentMin = 2.5f;
// A frame-to-frame pelvis jump past this (m) is a teleport / sit jump: no history.
constexpr float kShTeleport = 4.f;
// The atlas lingers this long after the last recording that needed it (s): rain showers.
constexpr double kShLinger = 30.0;
// VRAM: a better configuration only after this much calm (s).
constexpr double kShCalm = 30.0;
constexpr double kShRetrySecs = 10.0;
// #901 AVATAR SNOW (clean_weather_shelter.comp -DWX_SH_PUBLISH): what melts per second fully under
// cover (#903: snowing or not), the speeds past which an avatar sheds it (llcleanweathervk.cpp's
// avatar thresholds), the mesh's lift against z-fighting and the depth a cell needs for triangles.
constexpr float kShCoverMelt = 0.0008f;   // m/s (~4 cm in under a minute indoors)
constexpr float kShShedSpeed = 3.6f, kShShedVSpeed = 1.6f;
constexpr float kShMeshLift = 0.004f, kShMeshLeast = 0.003f;
// #901 COVERED LAYERS: an avatar whose top lies under a roof (the PREP's exposure, read back) has
// nothing falling on it -- its map only needs refreshing this often (s; the own avatar / others),
// so a club full of avatars indoors costs a fraction of the raster.
constexpr double kShCoveredRefreshSelf = 0.25, kShCoveredRefreshOther = 1.0;
constexpr float kShCoveredExposure = 0.02f;

// #901 PER-AVATAR WETNESS, the CPU's side (static: it outlives the module -- a teleport's clean
// slate frees it, the own avatar's wetness carries on): per joint-table slot (fixed: 1024 entries,
// no growth) the avatar last seen there and its wetness as last read back; the atlas's rows hold
// the live state, these seed a re-created atlas and keep the module while someone is still wet.
struct ShWetEntry
{
    LLUUID id;
    float  top = 0.f, body = 0.f, expTop = 0.f, expBody = 0.f;
    F64    t = 0.0;        // when read back (LLTimer::getTotalSeconds)
    bool   known = false;
};
ShWetEntry s_wet[1024];
LLUUID     s_slotOwner[1024];   // who planShelter last saw in each slot
bool       s_seeded[1024] = {}; // this atlas's row got its seed / reset
F64        s_wetUntil = 0.0;    // the estimate of when the last tracked avatar is dry
F64        s_wetScanAt = 0.0;
float      s_dryPerSec = 1.f / 240.f;   // the last drying rate (no boost: the seed's decay)

// float 0..1 -> IEEE half (clean_weather_shelter.comp unpackHalf2x16; tiny values flush to 0).
uint32_t shHalf(float f)
{
    f = llclamp(f, 0.f, 1.f);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const int32_t he = (int32_t)((b >> 23) & 0xFFu) - 127 + 15;
    if (he <= 0) return 0u;
    if (he >= 31) return 0x7BFFu;
    return ((uint32_t)he << 10) | ((b >> 13) & 0x3FFu);
}

void shBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

VkShaderModule shModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

// clean_weather_shelter.comp ShPush (128 B).
struct ShPush
{
    uint32_t a[4];   // instance count, flags (1 skinned vertices, 2 skin meta, 4 snow, 8 mesh), the per-slot rows' row, unused
    float    b[4];   // the palettes' eye xyz, dt
    float    c[4];   // extent min, extent cap, teleport distance, unused
    float    d[4], e[4], f[4];   // (M3+ snow)
    float    g[4], h[4];         // #901 per-avatar wetness: rain, wet / s, dry / s, global wetness; fall shear xy, on, unused
};
static_assert(sizeof(ShPush) == 128, "ShPush = clean_weather_shelter.comp ShPush");
} // namespace

LLCleanWeatherVK::ShelterStats LLCleanWeatherVK::sShelterStats;

// MemTrend (#883): the shelter's VRAM and its last chain's work items.
void LLCleanWeatherVK::registerShelterProbe()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    LLMemTrend::registerProbe("vk-weather-shelter", []
    {
        const ShelterStats& s = sShelterStats;
        LLMemTrend::Sample out;
        out.count = s.layers;
        out.bytes = s.bytes;
        out.cap = 0;
        out.peak = s.peakBytes;
        return out;
    });
}

// The own set's layout {0 raster target, 1 atlas x2, 2 record, 3 candidates, 4 their meta, 5
// skinned vertices, 6 index words, 7 skin meta, 8 motion state, 9 / 10 / 11 the avatar snow
// mesh's vertices / indices / arguments, #901 12 / 13 / 14 the rest-pose accumulation's record /
// scratch / grids (llcleanweathervk_accum.cpp)}, a set per frame slot, the
// pipeline layout {draw set 0, own} + the 128 B push. With the snow layouts (first use).
void LLCleanWeatherVK::createShelterLayouts(VkDevice dev, VkDescriptorSetLayout dslDraw0)
{
    if (mShLayout || !dslDraw0) return;
    mShDslDraw0 = dslDraw0;
    VkDescriptorSetLayoutBinding b[kShSetBindings]{};
    for (uint32_t i = 0; i < kShSetBindings; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = (i < 2 || i == 14) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = i == 1 ? 2u : 1u;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // (#901 12 / 13 / 14 -- the accumulation record, scratch, grids -- are written only while its
    // pool exists, and only its kernels use them: partially bound)
    VkDescriptorBindingFlags bf[kShSetBindings] = {};
    bf[12] = bf[13] = bf[14] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = kShSetBindings; bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &bfi;
    lci.bindingCount = kShSetBindings; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mShDsl, "mDslWeatherShelter"));
    VK_CHECK(mShPool.create(dev, "weather shelter", {{mShDsl, 2}}));
    for (VkDescriptorSet& s : mShSet)
        VKD_ALLOC_SOFT(mShPool.allocate(mShDsl, &s), { s = VK_NULL_HANDLE; });
    const VkDescriptorSetLayout sl[2] = {dslDraw0, mShDsl};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = sl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mShLayout, "mWeatherShelterLayout"));
}

// Into the open pipeline batch (ensurePipelines), mStage[kShStage0 + k].
void LLCleanWeatherVK::buildShelterPipelines(VkDevice dev)
{
    if (!mShLayout) return;
    const struct { const uint32_t* code; size_t bytes; const char* sym; const char* what; } k[kShKernels] = {
        {kWorldWeatherShelterPrepSpv, kWorldWeatherShelterPrepSpv_bytes, "kWorldWeatherShelterPrepSpv", "weather shelter frames"},
        {kWorldWeatherShelterFilterSpv, kWorldWeatherShelterFilterSpv_bytes, "kWorldWeatherShelterFilterSpv", "weather shelter filter"},
        {kWorldWeatherShelterClearSpv, kWorldWeatherShelterClearSpv_bytes, "kWorldWeatherShelterClearSpv", "weather shelter clear"},
        {kWorldWeatherShelterRasterSpv, kWorldWeatherShelterRasterSpv_bytes, "kWorldWeatherShelterRasterSpv", "weather shelter raster"},
        {kWorldWeatherShelterPublishSpv, kWorldWeatherShelterPublishSpv_bytes, "kWorldWeatherShelterPublishSpv", "weather shelter publish + avatar snow"},
        {kWorldWeatherShelterMeshSpv, kWorldWeatherShelterMeshSpv_bytes, "kWorldWeatherShelterMeshSpv", "weather avatar snow mesh"},
    };
    for (uint32_t i = 0; i < kShKernels; ++i)
    {
        VkShaderModule m = shModule(dev, k[i].code, k[i].bytes, k[i].sym);
        createComputePipelineInto(dev, m, mShLayout, 0, nullptr, &mStage[kShStage0 + i], k[i].what);
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    // #901 REST-POSE ACCUMULATION (the shelter layout: its set + the 128 B push range)
    const struct { const uint32_t* code; size_t bytes; const char* sym; const char* what; } a[kAcKernels] = {
        {kWorldWeatherAccumPrepSpv, kWorldWeatherAccumPrepSpv_bytes, "kWorldWeatherAccumPrepSpv", "weather avatar accumulation prep"},
        {kWorldWeatherAccumSpv, kWorldWeatherAccumSpv_bytes, "kWorldWeatherAccumSpv", "weather avatar accumulation"},
        {kWorldWeatherAccumStageSpv, kWorldWeatherAccumStageSpv_bytes, "kWorldWeatherAccumStageSpv", "weather avatar accumulation resample"},
        {kWorldWeatherAccumResolveSpv, kWorldWeatherAccumResolveSpv_bytes, "kWorldWeatherAccumResolveSpv", "weather avatar accumulation resolve"},
    };
    for (uint32_t i = 0; i < kAcKernels; ++i)
    {
        VkShaderModule m = shModule(dev, a[i].code, a[i].bytes, a[i].sym);
        createComputePipelineInto(dev, m, mShLayout, 0, nullptr, &mStage[kAcStage0 + i], a[i].what);
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
}

void LLCleanWeatherVK::destroyShelterPipelines(VkDevice dev)
{
    // (mShPipe / mAcPipe alias mStage[kShStage0 ..] / [kAcStage0 ..], which destroy() frees first:
    // destroying them here too was a double vkDestroyPipeline on every module teardown)
    for (VkPipeline& p : mShPipe) p = VK_NULL_HANDLE;
    for (VkPipeline& p : mAcPipe) p = VK_NULL_HANDLE;
    // (#901 / #909 follow-up: the cluster kernels are their own -- not aliases of mStage)
    mShClJob.wait();
    for (uint32_t k = 0; k < kShClKernels; ++k)
    {
        if (mShClPipe[k]) vkDestroyPipeline(dev, mShClPipe[k], nullptr);
        if (mShClStage[k] && mShClStage[k] != mShClPipe[k]) vkDestroyPipeline(dev, mShClStage[k], nullptr);
        mShClPipe[k] = mShClStage[k] = VK_NULL_HANDLE;
    }
    if (mShClLayout) vkDestroyPipelineLayout(dev, mShClLayout, nullptr);
    mShClLayout = VK_NULL_HANDLE;
    mShClDslKey = VK_NULL_HANDLE;
    mShClFailed = false;
    mShDslDraw0 = VK_NULL_HANDLE;
    if (mShLayout) vkDestroyPipelineLayout(dev, mShLayout, nullptr);
    mShLayout = VK_NULL_HANDLE;
    mShPool.destroy();
    mShSet[0] = mShSet[1] = VK_NULL_HANDLE;
    if (mShDsl) vkdDestroySetLayout(dev, mShDsl);
    mShDsl = VK_NULL_HANDLE;
}

// Frees the atlas / record (the caller waited for the device; binding 49 and the simulation's
// sets are rewritten by it).
void LLCleanWeatherVK::destroyShelter()
{
    if (mCtx)
    {
        for (GpuImage* im : {&mShH, &mShS[0], &mShS[1]})
            if (im->image) mCtx->destroyImage(*im);
        for (GpuBuffer* b : {&mShBuf, &mShRead[0], &mShRead[1]})
            if (b->buffer) mCtx->destroyBuffer(*b);
    }
    mShH = mShS[0] = mShS[1] = GpuImage{};
    mShBuf = mShRead[0] = mShRead[1] = GpuBuffer{};
    mShReadWritten[0] = mShReadWritten[1] = false;
    // #946: the new record's simulation counters start at 0 -- a delta against the old record's
    // logged a wrapped 4294961600 "drops hidden" / 4294967290 "wetness resets" after each re-create.
    mShStatHave = false;
    destroyShelterMesh();
    for (auto& row : mShBound)
        for (VkBuffer& b : row) b = VK_NULL_HANDLE;
    mShConfig = kShConfigNone;
    mShTilesH = 0;
    mShPub = -1;
    mShDst = 0;
    mShDue = false;
    mShLayersUsed = 0;
    for (int32_t& s : mShLayerSlot) s = -1;
    for (bool& f : mShFresh) f = false;
    sShelterStats.bytes = 0;
    sShelterStats.config = kShConfigNone;
    sShelterStats.layers = sShelterStats.layerCap = sShelterStats.items = 0;
}

// The atlas / record for a configuration (device wait on a change; binding 49 and the
// simulation's sets rewritten). A refused allocation frees everything and retries later.
bool LLCleanWeatherVK::ensureShelter(VkCommandBuffer cmd, uint32_t config)
{
    if (config == mShConfig && (config == kShConfigNone || (mShH.image && mShBuf.buffer))) return config != kShConfigNone;
    if (config != kShConfigNone && mNow < mShRetryAt) return mShConfig != kShConfigNone && mShH.image;
    wxWaitIdle(mCtx->device(), "shelter atlas");   // (binding 49 / the sets name them in frames in flight)
    const bool had = mShH.image != VK_NULL_HANDLE;
    destroyShelter();
    if (config == kShConfigNone)
    {
        if (had)
        {
            mBindingDirty = true;
            writeSets();
            LL_INFOS("VKWeather") << "weather avatar shelter freed" << LL_ENDL;
        }
        return false;
    }
    const uint32_t tilesH = shTilesH(config);
    bool ok = true;
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // (#783 BODY CONTACT: + the bottom map under the top map + the layers' raster-time frame
        // rows -- clean_weather_shelter.glsl WX_SH_BODY_ROWS; the snow passes read it)
        mShH = mCtx->createImage2D(kShAtlasW, 2u * tilesH + kShBodyRows, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        for (GpuImage& im : mShS)
            im = mCtx->createImage2D(kShAtlasW, tilesH + kShSlotRows, VK_FORMAT_R32G32_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        mShBuf = mCtx->createBuffer(kShRecordBytes + (VkDeviceSize)kShItemCap * 8u, su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (GpuBuffer& r : mShRead)
            r = mCtx->createBuffer(kShRecordBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok = mShH.image && mShH.view && mShS[0].image && mShS[0].view && mShS[1].image && mShS[1].view && mShBuf.buffer
          && mShRead[0].mapped && mShRead[1].mapped;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather avatar shelter (configuration " << config << ") failed to allocate; skipped, retrying in "
                              << kShRetrySecs << " s" << LL_ENDL;
        destroyShelter();
        mShRetryAt = mNow + kShRetrySecs;
        mBindingDirty = true;
        writeSets();
        return false;
    }
    mShConfig = config;
    mShTilesH = tilesH;
    std::fill(s_seeded, s_seeded + 1024, false);   // (#901: the rows start empty -- seed the known wetness again)
    for (float& x : mShExpTop) x = -1.f;
    registerShelterProbe();
    const uint64_t bytes = (uint64_t)kShAtlasW * (2u * tilesH + kShBodyRows) * 4u + 2u * (uint64_t)kShAtlasW * (tilesH + kShSlotRows) * 8u
                         + kShRecordBytes + (uint64_t)kShItemCap * 8u + 32u;
    sShelterStats.bytes = bytes;
    sShelterStats.peakBytes = std::max(sShelterStats.peakBytes, bytes);
    sShelterStats.config = config;
    // Cleared: nothing drawn, no surface / snow, an empty record (no layer history); GENERAL.
    {
        GpuImage* ims[3] = {&mShH, &mShS[0], &mShS[1]};
        VkImageMemoryBarrier2 ib[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib[i].image = ims[i]->image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 3; dep.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, mShH.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
        VkClearColorValue none{};
        none.float32[0] = -1.0e4f;   // (clean_weather_shelter.glsl kShNone)
        for (uint32_t i = 1; i < 3; ++i)
            vkCmdClearColorImage(cmd, ims[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &rng);
        vkCmdFillBuffer(cmd, mShBuf.buffer, 0, VK_WHOLE_SIZE, 0u);
        for (uint32_t i = 0; i < 3; ++i)
        {
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier2(cmd, &dep);
        shBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // The renderer's buffers are written into each frame slot's set at record time.
    writeShelterSets(0, nullptr);
    writeShelterSets(1, nullptr);
    mBindingDirty = true;   // (binding 49 [8] / [9])
    writeSets();            // (the simulation's 9 / 10)
    LL_INFOS("VKWeather") << "weather avatar shelter: configuration " << config << " (" << shLayersOf(config) << " layers), atlas "
                          << kShAtlasW << " x " << (2u * tilesH + kShBodyRows) << " r32ui (top + bottom) + 2 x " << kShAtlasW << " x " << (tilesH + kShSlotRows)
                          << " rg32f, " << (bytes >> 10) << " KB" << LL_ENDL;
    return true;
}

void LLCleanWeatherVK::destroyShelterMesh()
{
    if (mShMeshV.buffer)
    {
        const uint64_t mb = (uint64_t)mShMeshIdxCap / 6u * 16u + (uint64_t)mShMeshIdxCap * 4u + 32u;
        sShelterStats.bytes -= std::min(sShelterStats.bytes, mb);
    }
    if (mCtx)
        for (GpuBuffer* b : {&mShMeshV, &mShMeshI, &mShMeshA})
            if (b->buffer) mCtx->destroyBuffer(*b);
    mShMeshV = mShMeshI = mShMeshA = GpuBuffer{};
    mShMeshIdxCap = 0;
    mShMeshBuilt = false;
}

// The avatar snow mesh's buffers for the allocated atlas while the avatars carry snow (device
// wait on a change; the sets rewritten). A refused allocation skips the mesh and retries later.
bool LLCleanWeatherVK::ensureShelterMesh(VkCommandBuffer cmd, bool want)
{
    const uint32_t verts = (kShAtlasW / 2u) * (mShTilesH / 2u);
    const uint32_t idxCap = verts * 6u;
    if (want && mShMeshV.buffer && mShMeshIdxCap == idxCap) return true;
    if (!want && !mShMeshV.buffer) return false;
    if (want && mNow < mShMeshRetryAt) return false;
    wxWaitIdle(mCtx->device(), "shelter snow mesh");
    destroyShelterMesh();
    if (want)
    {
        bool ok;
        {
            ++sSync.recreates;   // #946 (the "weather sync" line)
            VkMemScope memcat(VKMEM_PARTICLE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            mShMeshV = mCtx->createBuffer((VkDeviceSize)verts * 16u, su | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            mShMeshI = mCtx->createBuffer((VkDeviceSize)idxCap * 4u, su | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            mShMeshA = mCtx->createBuffer(32u, su | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ok = mShMeshV.buffer && mShMeshI.buffer && mShMeshA.buffer;
        }
        if (!ok)
        {
            LL_WARNS("VKWeather") << "weather avatar snow mesh failed to allocate; the avatars keep the coat, retrying in "
                                  << kShRetrySecs << " s" << LL_ENDL;
            destroyShelterMesh();
            mShMeshRetryAt = mNow + kShRetrySecs;
        }
        else
        {
            mShMeshIdxCap = idxCap;
            vkCmdFillBuffer(cmd, mShMeshA.buffer, 0, VK_WHOLE_SIZE, 0u);   // (no triangles until a chain builds them)
            shBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            sShelterStats.bytes += (uint64_t)verts * 16u + (uint64_t)idxCap * 4u + 32u;
            sShelterStats.peakBytes = std::max(sShelterStats.peakBytes, sShelterStats.bytes);
            LL_INFOS("VKWeather") << "weather avatar snow mesh: " << verts << " vertices, " << idxCap / 3u << " triangles cap" << LL_ENDL;
        }
    }
    writeShelterSets(0, nullptr);
    writeShelterSets(1, nullptr);
    return mShMeshV.buffer != VK_NULL_HANDLE;
}

// A frame slot's set: the atlas / record always, the renderer's buffers when given (null: the
// record stands in -- the chain does not run before record time writes them).
void LLCleanWeatherVK::writeShelterSets(uint32_t slot, const ShelterInputs* in)
{
    if (!mCtx || slot > 1 || !mShSet[slot] || !mShH.view || !mShS[0].view || !mShS[1].view || !mShBuf.buffer) return;
    const VkBuffer rec = mShBuf.buffer;
    VkBuffer want[6] = {rec, rec, rec, rec, rec, rec};
    if (in)
    {
        want[0] = in->cand ? in->cand : rec;
        want[1] = in->candMeta ? in->candMeta : rec;
        want[2] = in->skinVbo ? in->skinVbo : rec;
        want[3] = in->ibo ? in->ibo : rec;
        want[4] = in->skinMeta ? in->skinMeta : rec;
        want[5] = mAvState.buffer ? mAvState.buffer : rec;   // (M3+: the avatars' motion state)
    }
    // (rewritten every recording, never skipped on equal handles: a buffer the renderer / the snow
    // passes re-created may reuse the old handle value -- this slot's last use is fenced, 12 writes)
    const VkDescriptorImageInfo hi{VK_NULL_HANDLE, mShH.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo si[2] = {{VK_NULL_HANDLE, mShS[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                         {VK_NULL_HANDLE, mShS[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo bi[10];
    bi[0] = {rec, 0, VK_WHOLE_SIZE};
    for (uint32_t i = 0; i < 6; ++i) bi[1 + i] = {want[i], 0, VK_WHOLE_SIZE};
    // (#901 the avatar snow mesh; the record stands in without it -- never written then)
    bi[7] = {mShMeshV.buffer ? mShMeshV.buffer : rec, 0, VK_WHOLE_SIZE};
    bi[8] = {mShMeshI.buffer ? mShMeshI.buffer : rec, 0, VK_WHOLE_SIZE};
    bi[9] = {mShMeshA.buffer ? mShMeshA.buffer : rec, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[kShSetBindings]{};
    constexpr uint32_t kShOwn = 12;   // (0 .. 11: the shelter's own; 12 .. 14 the accumulation's, below)
    for (uint32_t i = 0; i < kShOwn; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = mShSet[slot]; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
        wr[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i >= 2) wr[i].pBufferInfo = &bi[i - 2];
    }
    wr[0].pImageInfo = &hi;
    wr[1].pImageInfo = si; wr[1].descriptorCount = 2;
    // #901 the rest-pose accumulation's record / scratch / grids while its pool exists
    uint32_t n = kShOwn;
    VkDescriptorBufferInfo abi[2];
    VkDescriptorImageInfo aii;
    writeAccumSets(slot, wr, abi, &aii, n);
    vkUpdateDescriptorSets(mCtx->device(), n, wr, 0, nullptr);
    std::copy(want, want + 6, mShBound[slot]);
}

// This recording's plan (prepare): which avatars have a layer, which are re-rastered, the record's
// header. true = the chain runs this recording (recordShelter); mShDst its atlas image.
bool LLCleanWeatherVK::planShelter(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKWeatherAvatarShelter", true);
    static LLCachedControl<U32>  s_count(gSavedSettings, "RenderVKWeatherAvatarShelterCount", 16);
    static LLCachedControl<F32>  s_range(gSavedSettings, "RenderVKWeatherAvatarShelterRange", 20.f);
    static LLCachedControl<U32>  s_per(gSavedSettings, "RenderVKWeatherAvatarShelterPerFrame", 3);
    static LLCachedControl<F32>  s_extMax(gSavedSettings, "RenderVKWeatherAvatarShelterExtentMax", 6.f);
    static LLCachedControl<bool> s_snow(gSavedSettings, "RenderVKWeatherSnowAvatars", true);
    static LLCachedControl<bool> s_mesh(gSavedSettings, "RenderVKWeatherAvatarSnowMesh", false);
    static LLCachedControl<bool> s_wetOn(gSavedSettings, "RenderVKWeatherAvatarWetness", true);
    static LLCachedControl<F32>  s_soak(gSavedSettings, "RenderVKWeatherAvatarSoakSeconds", 75.f);
    static LLCachedControl<F32>  s_dry(gSavedSettings, "RenderVKWeatherAvatarDrySeconds", 240.f);
    if (!mShInit)
    {
        for (int32_t& s : mShLayerSlot) s = -1;
        for (float& x : mShExpTop) x = -1.f;
        mShInit = true;
    }
    mShDue = false;
    const bool pipes = mShLayout && mShSet[0] && mShSet[1] && mShPipe[0] && mShPipe[1] && mShPipe[2] && mShPipe[3] && mShPipe[4];
    const SLWeather::State& st = SLWeather::instance().state();
    // Wanted while it rains or snows, or snow lies (the avatars' own snow), within kShLinger.
    const bool precip = st.rain > SLWeather::kEpsilon || st.snow > SLWeather::kEpsilon || st.snowCover > SLWeather::kEpsilon;
    if (precip) mShLastNeed = mNow;
    // (#901: and while an avatar it tracked is still wet -- it dries on its own clock)
    const bool want = s_on && pipes && f.camTime && ((mNow - mShLastNeed) < kShLinger || avatarsWetHold());
    // The avatars near the camera (the joint-table slots' avatars and animesh; a bounded list).
    struct Cand { float d; uint32_t slot; bool self; LLUUID id; };
    static std::vector<Cand> s_cands;
    s_cands.clear();
    if (want)
    {
        static std::vector<std::pair<LLUUID, U32>> s_slots;
        LLCleanSkinStore::instance().copyAvatarSlots(s_slots);
        const float range = llclamp((F32)s_range, 2.f, 64.f);
        // #901 PER-AVATAR WETNESS resets (at most 8 a recording; the rest the next): a new avatar in
        // a slot starts from the guess (the GPU: the global wetness x its exposure now), a known
        // one in a re-created atlas from its last value, dried for the time since.
        uint32_t nr = 0;
        const F64 wnow = LLTimer::getTotalSeconds();
        for (const auto& kv : s_slots)
        {
            if (f.offscreen || kv.second >= 1024u || nr >= 8u) continue;   // (a snapshot runs no chain)
            const uint32_t sl = kv.second;
            if (s_slotOwner[sl] != kv.first)
            {
                // (the same avatar known in another slot -- the skin store re-slotted it, e.g. after a
                // teleport's clean slate: its state moves with it, the own avatar keeps drying)
                ShWetEntry moved;
                for (ShWetEntry& o : s_wet)
                    if (o.known && o.id == kv.first) { moved = o; o = ShWetEntry{}; break; }
                for (LLUUID& ow : s_slotOwner)
                    if (ow == kv.first) ow.setNull();
                s_slotOwner[sl] = kv.first;
                s_wet[sl] = moved;
                s_wet[sl].id = kv.first;
                s_seeded[sl] = true;
                mShResets[4 + 2 * nr] = sl;
                if (moved.known)
                {
                    const float dried = (float)std::max(wnow - moved.t, 0.0) * s_dryPerSec;
                    mShResets[5 + 2 * nr] = shHalf(moved.top - dried) | (shHalf(moved.body - dried) << 16);
                }
                else
                    mShResets[5 + 2 * nr] = 0xFFFFFFFFu;   // (a new avatar: the GPU's guess)
                ++nr;
            }
            else if (!s_seeded[sl])
            {
                s_seeded[sl] = true;
                const ShWetEntry& we = s_wet[sl];
                if (we.known && we.id == kv.first)
                {
                    const float dried = (float)std::max(wnow - we.t, 0.0) * s_dryPerSec;
                    mShResets[4 + 2 * nr] = sl;
                    mShResets[5 + 2 * nr] = shHalf(we.top - dried) | (shHalf(we.body - dried) << 16);
                    ++nr;
                }
            }
        }
        mShResets[0] = nr; mShResets[1] = mShResets[2] = mShResets[3] = 0u;
        for (const auto& kv : s_slots)
        {
            if (kv.second >= 1024u) continue;
            LLViewerObject* obj = gObjectList.findObject(kv.first);
            LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
            if (!av || av->isDead()) continue;
            const LLVector3 p = av->getPositionAgent();
            const float dx = p.mV[VX] - f.camTime[0], dy = p.mV[VY] - f.camTime[1], dz = p.mV[VZ] - f.camTime[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool self = av->isSelf();
            if (!std::isfinite(d) || (d > range && !self)) continue;
            s_cands.push_back(Cand{self ? -1.f : d, kv.second, self, kv.first});
        }
    }
    // VRAM pressure: worse at once, better after kShCalm of calm.
    uint32_t cfgWant = f.vramMode >= 2 ? 2u : (f.vramMode >= 1 || f.vramRung > 0) ? 1u : 0u;
    // #946: the SMALL VRAM tier never takes configuration 0 (the 512-row atlas): a 4 GB card at
    // the edge of its budget flipped 0 -> 1 -> 0 as the ladder engaged / calmed, each flip a
    // device wait and the 12 MB atlas re-created while the regrowth re-added the pressure.
    if (sSmallVram) cfgWant = std::max(cfgWant, 1u);
    if (cfgWant > mShWant || mShWant == kShConfigNone) { mShWant = cfgWant; mShCalmSince = mNow; }
    else if (cfgWant < mShWant && mNow - mShCalmSince > kShCalm) { mShWant = cfgWant; mShCalmSince = mNow; }
    else if (cfgWant >= mShWant) mShCalmSince = mNow;
    const uint32_t config = (want && !s_cands.empty()) ? mShWant : (want && mShConfig != kShConfigNone ? mShConfig : kShConfigNone);
    mAcDue = false;
    for (bool& d : mAcLayerDue) d = false;
    if (!ensureShelter(cmd, config))
    {
        mShLayersUsed = 0;
        if (config == kShConfigNone) ensureAccum(cmd, false);   // (#901: the grids go with the atlas)
        return false;
    }
    if (s_cands.empty() || f.offscreen)
    {
        // Nobody near (the atlas lingers a while) / a snapshot: no chain; the last maps hold.
        if (s_cands.empty())
        {
            for (int32_t& s : mShLayerSlot) s = -1;
            mShLayersUsed = 0;
            mShMeshBuilt = false;
        }
        sShelterStats.layers = 0;
        return false;
    }
    // LAYERS: the own avatar first, then by distance; tier A (configuration 0: layers 0..3,
    // 256^2) the first kShTierA, tier B the rest -- STICKY: an avatar keeps its layer while it stays
    // in its tier (a new layer has no history: its map and snow start over).
    // (#946: the SMALL VRAM tier keeps at most 8 layers -- the own avatar first, then the closest)
    const uint32_t nLayers = std::min<uint32_t>({shLayersOf(mShConfig), llclamp((U32)s_count, 1u, kShLayers),
                                                 sSmallVram ? 8u : kShLayers});
    const uint32_t nA = mShConfig == 0 ? std::min(kShTierA, nLayers) : 0u;
    const size_t nKeep = std::min<size_t>(s_cands.size(), nLayers);
    std::partial_sort(s_cands.begin(), s_cands.begin() + nKeep, s_cands.end(),
                      [](const Cand& a, const Cand& b) { return a.d < b.d; });
    int32_t tierOf[1024];   // (the wanted tier per slot: -1 none, 0 A, 1 B)
    std::fill(tierOf, tierOf + 1024, -1);
    for (size_t i = 0; i < nKeep; ++i) tierOf[s_cands[i].slot] = i < nA ? 0 : 1;
    bool placed[kShLayers] = {};
    for (uint32_t L = 0; L < kShLayers; ++L)
    {
        const int32_t s = mShLayerSlot[L];
        if (s < 0) continue;
        const int32_t tier = L < nA ? 0 : 1;
        if (L >= nLayers || tierOf[s] != tier) { mShLayerSlot[L] = -1; continue; }
        placed[L] = true;
        tierOf[s] = 2;   // (has its layer)
    }
    for (size_t i = 0; i < nKeep; ++i)
    {
        const uint32_t slot = s_cands[i].slot;
        if (tierOf[slot] == 2) continue;
        const uint32_t lo = tierOf[slot] == 0 ? 0u : nA, hi = tierOf[slot] == 0 ? nA : nLayers;
        for (uint32_t L = lo; L < hi; ++L)
            if (mShLayerSlot[L] < 0)
            {
                mShLayerSlot[L] = (int32_t)slot;
                mShFresh[L] = true;
                mShExpTop[L] = -1.f;
                tierOf[slot] = 2;
                break;
            }
    }
    // DUE: new layers and the own avatar every recording; the others round-robin.
    uint32_t used = 0, live = 0;
    uint32_t selfSlot = 0xFFFFFFFFu;
    for (const Cand& c : s_cands)
        if (c.self) selfSlot = c.slot;
    mShSelfSlot = selfSlot;
    // #901 COST: the maps matter only while something falls on the avatars or snow lies on them
    // (the rain / snow tests, the avatar snow model). Otherwise nothing is re-rastered (the maps
    // hold; the chain's PREP still dries the avatars). A layer whose avatar is under a roof (its
    // top's exposure, read back) is refreshed only every kShCoveredRefresh* s.
    const bool anyFalling = st.rain > SLWeather::kEpsilon || st.snow > SLWeather::kEpsilon;
    const bool needMaps = anyFalling || (s_snow && st.snowCover > SLWeather::kEpsilon);
    bool due[kShLayers] = {};
    bool covered[kShLayers] = {};
    for (uint32_t L = 0; L < nLayers; ++L)
        if (mShLayerSlot[L] >= 0)
        {
            used = L + 1;
            ++live;
            const bool self = (uint32_t)mShLayerSlot[L] == selfSlot;
            covered[L] = mShExpTop[L] >= 0.f && mShExpTop[L] < kShCoveredExposure
                      && mNow - mShLastDue[L] < (self ? kShCoveredRefreshSelf : kShCoveredRefreshOther);
            due[L] = mShFresh[L] || (needMaps && self && !covered[L]);
        }
    if (used == 0) { mShLayersUsed = 0; mShMeshBuilt = false; sShelterStats.layers = 0; return false; }
    // #901 REST-POSE ACCUMULATION: each layer's avatar (the grids follow it by id); which grids
    // update this recording (their layers' bit 4 below: the filter lists their draws)
    for (uint32_t L = 0; L < kShLayers; ++L)
    {
        mShLayerId[L].setNull();
        mShLayerSelf[L] = false;
        if (L >= used || mShLayerSlot[L] < 0) continue;
        for (const Cand& c : s_cands)
            if (c.slot == (uint32_t)mShLayerSlot[L]) { mShLayerId[L] = c.id; mShLayerSelf[L] = c.self; break; }
    }
    planAccum(cmd, f, used);
    if (needMaps)
    {
        const uint32_t per = llclamp((U32)s_per, 1u, kShLayers);
        uint32_t picked = 0;
        for (uint32_t k = 0; k < used && picked < per; ++k)
        {
            const uint32_t L = (mShRound + k) % used;
            if (mShLayerSlot[L] < 0 || due[L] || covered[L]) continue;
            due[L] = true;
            ++picked;
        }
        mShRound = (mShRound + std::max(picked, 1u)) % used;
    }
    uint32_t nDue = 0;
    for (uint32_t L = 0; L < used; ++L)
        if (due[L]) { mShLastDue[L] = mNow; ++nDue; }
    sShelterStats.due = nDue;
    // The record's header (clean_weather_shelter.glsl WxShelter, the first kShHeadBytes).
    mShDst = mShPub >= 0 ? (mShPub ^ 1) : 0;
    std::memset(mShHead, 0, sizeof(mShHead));
    uint32_t* h = mShHead;
    h[0] = 0u; h[1] = kShItemCap; h[2] = 0u; h[3] = mShTilesH;           // head
    h[4] = 0u; h[5] = 1u; h[6] = 1u; h[7] = 0u;                          // disp
    h[8] = 0u; h[9] = 0u; h[10] = 0u; h[11] = 0u;                        // mesh (M3+)
    const float extMax = llclamp((F32)s_extMax, kShExtentMin, 12.f);
    h[12] = used; h[13] = (uint32_t)mShDst;
    std::memcpy(&h[14], &extMax, 4);
    std::memcpy(&h[15], &kShExtentMin, 4);
    for (uint32_t L = 0; L < kShLayers; ++L)
    {
        uint32_t* inf = &h[16 + 4 * L];
        if (L >= used || mShLayerSlot[L] < 0) { inf[0] = 0xFFFFFFFFu; continue; }
        uint32_t res = 0, x0 = 0, y0 = 0;
        shTile(mShConfig, L, res, x0, y0);
        inf[0] = (uint32_t)mShLayerSlot[L];
        inf[1] = res;
        inf[2] = x0 | (y0 << 16);
        inf[3] = (due[L] ? 1u : 0u) | (mShFresh[L] ? 2u : 0u) | (mAcLayerDue[L] ? 4u : 0u);
    }
    // (h[80..95]: the per-layer item counts, zero)
    for (bool& fr : mShFresh) fr = false;
    // AVATAR SNOW (the world's model at avatar scale): while snow falls or lies (the global cover)
    // and RenderVKWeatherSnowAvatars; the mesh while RenderVKWeatherAvatarSnowMesh and the world
    // snow mesh's pipelines exist.
    {
        const bool snowOn = s_snow && (st.snow > SLWeather::kEpsilon || st.snowCover > SLWeather::kEpsilon);
        const bool meshOn = snowOn && s_mesh && mMeshGbufPipe && ensureShelterMesh(cmd, true);
        if (!snowOn || !s_mesh) ensureShelterMesh(cmd, false);
        const float dt = mDt;
        float* d = mShSnowPush;
        std::memset(mShSnowPush, 0, sizeof(mShSnowPush));
        d[0] = st.snowDepth;
        // (#783: the state's rates -- the settings x the intensity, or a parcel tag's rate=)
        d[1] = dt * std::max(st.snowDepthBuild, 0.f);
        d[2] = dt * std::max(st.snowDepthMelt, 0.f);
        d[3] = dt * kShCoverMelt;
        d[4] = kShShedSpeed; d[5] = kShShedVSpeed; d[6] = kShMeshLift; d[7] = kShMeshLeast;
        d[8] = mShPub >= 0 ? (float)mShPub : -1.f;
        mShFlags = (snowOn ? 4u : 0u) | (meshOn ? 8u : 0u) | (mAvState.buffer ? 16u : 0u);
        mShMeshBuilt = false;   // (the chain builds it this recording)
    }
    {
        // PER-AVATAR WETNESS (clean_weather_shelter.comp PREP): wets in the soak time at full rain
        // fully exposed, dries in the drying time -- faster in wind and in sunshine (the cheap
        // signals: the mean wind, the sun's elevation x the clear sky).
        const float soak = llclamp((F32)s_soak, 5.f, 3600.f), dry = llclamp((F32)s_dry, 5.f, 7200.f);
        const float wind = std::sqrt(st.windMean[0] * st.windMean[0] + st.windMean[1] * st.windMean[1]);
        const float sun = f.sunDir ? llclamp(f.sunDir[2] * 3.f, 0.f, 1.f) : 0.f;
        const float boost = 1.f + 0.6f * std::min(wind / 10.f, 1.f) + 0.6f * sun * (1.f - llclamp(st.cloud, 0.f, 1.f));
        s_dryPerSec = 1.f / dry;
        float* w = mShWetPush;
        w[0] = llclamp(st.rain, 0.f, 1.f);
        w[1] = 1.f / soak;
        w[2] = boost / dry;
        w[3] = llclamp(st.wetness, 0.f, 1.f);
        w[4] = mTilt[0]; w[5] = mTilt[1];
        w[6] = s_wetOn ? 1.f : 0.f;
        w[7] = 0.f;
    }
    mShLayersUsed = used;
    sShelterStats.layers = live;
    sShelterStats.layerCap = nLayers;
    sShelterStats.layerPeak = std::max(sShelterStats.layerPeak, live);
    sShelterStats.itemCap = kShItemCap;
    mShDue = true;
    return true;
}

// #901 / #909 follow-up: the CLUSTER LOD kernels (header). Layout {draw set 0, shelter set, the cluster
// set} + the shelter push range: sets 0 / 1 and the push stay compatible with mShLayout, so one bind
// of the three sets serves the whole chain. First use (cluster LOD on, the chain running) queues the
// three pipelines in a background batch; the chain runs without them until they land (the cluster-
// drawn parts miss the maps / grids for those frames, as before). A new cluster module (cluster LOD
// toggled: a new set layout) rebuilds them behind a device wait.
bool LLCleanWeatherVK::ensureShelterCluster(const ShelterInputs& in)
{
    if (!in.clSet || !in.clDsl || !in.clArgs || !mShDsl || !mShDslDraw0 || !mShLayout || !mCtx) return false;
    VkDevice dev = mCtx->device();
    if (mShClJob.pending())
    {
        if (!mShClJob.poll()) return false;   // still compiling
        bool ok = true;
        for (uint32_t k = 0; k < kShClKernels; ++k)
        {
            mShClPipe[k] = mShClStage[k];
            mShClStage[k] = VK_NULL_HANDLE;
            ok = ok && mShClPipe[k] != VK_NULL_HANDLE;
        }
        if (!ok)
        {
            LL_WARNS("VKWeather") << "weather avatar shelter: the cluster LOD pipelines failed to build; cluster-drawn "
                                     "avatar parts stay out of the shelter maps / grids" << LL_ENDL;
            mShClFailed = true;
        }
        else
            LL_INFOS("VKWeather") << "weather avatar shelter: cluster LOD pipelines built (background)" << LL_ENDL;
    }
    if (mShClLayout && mShClDslKey != in.clDsl)
    {
        // A new cluster module (its set layout): the old pipelines / layout go (rare: LOD toggled).
        wxWaitIdle(dev, "shelter cluster pipelines");
        for (VkPipeline& p : mShClPipe)
            if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        vkDestroyPipelineLayout(dev, mShClLayout, nullptr);
        mShClLayout = VK_NULL_HANDLE;
        mShClDslKey = VK_NULL_HANDLE;
        mShClFailed = false;
    }
    if (mShClFailed) return false;
    if (mShClPipe[0] && mShClPipe[1] && mShClPipe[2]) return true;
    if (mShClLayout) return false;   // (latched: built and failed above, or still landing)
    const VkDescriptorSetLayout sl[3] = {mShDslDraw0, mShDsl, in.clDsl};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 3; pli.pSetLayouts = sl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &pli, &mShClLayout, "mWeatherShelterClusterLayout") != VK_SUCCESS)
    {
        mShClLayout = VK_NULL_HANDLE;
        mShClFailed = true;
        return false;
    }
    mShClDslKey = in.clDsl;
    {
        PipelineBatchScope batchScope(dev);
        const struct { const uint32_t* code; size_t bytes; const char* sym; const char* what; } k[kShClKernels] = {
            {kWorldWeatherShelterClFilterSpv, kWorldWeatherShelterClFilterSpv_bytes, "kWorldWeatherShelterClFilterSpv", "weather shelter cluster filter"},
            {kWorldWeatherShelterClRasterSpv, kWorldWeatherShelterClRasterSpv_bytes, "kWorldWeatherShelterClRasterSpv", "weather shelter cluster raster"},
            {kWorldWeatherAccumClSpv, kWorldWeatherAccumClSpv_bytes, "kWorldWeatherAccumClSpv", "weather avatar accumulation (cluster)"},
        };
        for (uint32_t i = 0; i < kShClKernels; ++i)
        {
            VkShaderModule m = shModule(dev, k[i].code, k[i].bytes, k[i].sym);
            createComputePipelineInto(dev, m, mShClLayout, 0, nullptr, &mShClStage[i], k[i].what);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
        PipelineJobOpts o;
        o.what = "weather shelter cluster";
        o.gate = false;   // (optional: never holds the login progress screen)
        mShClJob.start(pipelineBatchFlushAsync(dev, o));
    }
    return false;
}

void LLCleanWeatherVK::bindShelterSets(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in, bool cl,
                                       const void* push, uint32_t pushBytes)
{
    const VkDescriptorSet sets[3] = {dsetDraw0, mShSet[mSlot], in.clSet};
    const VkPipelineLayout layout = cl ? mShClLayout : mShLayout;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, cl ? 3u : 2u, sets, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
}

// The chain (header): after the main cull's candidates and the pre-skin, before the G-buffer.
void LLCleanWeatherVK::recordShelter(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in)
{
    if (!shelterDue() || !dsetDraw0 || !mShSet[mSlot] || !mShBuf.buffer || !in.cand || !in.candMeta || !in.ibo) return;
    mShDue = false;
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // #901 / #909 follow-up: the cluster-drawn avatar parts (this frame's skinned arena; the
    // cluster pre-skin's per-entry grid, already ordered for indirect reads by its own barriers)
    const bool cl = ensureShelterCluster(in);
    writeShelterSets(mSlot, &in);   // (this slot's last use is fenced: no wait)
    // Earlier readers (the lit FS / the simulation / the snow passes of earlier recordings, the
    // last raster's indirect read, the readback copy) -> the header's update.
    shBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT
                       | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | RW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdUpdateBuffer(cmd, mShBuf.buffer, 0, kShHeadBytes, mShHead);
    vkCmdUpdateBuffer(cmd, mShBuf.buffer, kShResetOffset, sizeof(mShResets), mShResets);   // (#901 the wetness resets)
    mShResets[0] = 0u;
    const bool mesh = (mShFlags & 8u) != 0u && mShMeshA.buffer && mShPipe[5];
    if (mesh)
    {
        // VkDrawIndexedIndirectCommand {0 indices, 1 instance, 0, 0, 0} + [5] the index capacity
        const uint32_t args[8] = {0u, 1u, 0u, 0u, 0u, mShMeshIdxCap, 0u, 0u};
        vkCmdUpdateBuffer(cmd, mShMeshA.buffer, 0, sizeof(args), args);
    }
    // The header + the main cull's candidates / the pre-skinned vertices -> the chain.
    shBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              C, RW);
    ShPush p{};
    p.a[0] = in.instanceCount;
    p.a[1] = (in.skinVbo ? 1u : 0u) | (in.skinMeta ? 2u : 0u) | mShFlags;
    std::memcpy(p.d, mShSnowPush, sizeof(mShSnowPush));   // (d, e, f)
    std::memcpy(p.g, mShWetPush, sizeof(mShWetPush));     // (g, h)
    p.a[2] = mShTilesH;
    p.b[0] = in.palEye[0]; p.b[1] = in.palEye[1]; p.b[2] = in.palEye[2]; p.b[3] = mDt;
    std::memcpy(&p.c[1], &mShHead[14], 4);
    p.c[0] = kShExtentMin;
    p.c[2] = kShTeleport;
    bindShelterSets(cmd, dsetDraw0, in, cl, &p, sizeof(p));
    const uint32_t gx = (kShAtlasW + 7u) / 8u, gy = (mShTilesH + 7u) / 8u;
    // PREP (the frames, the per-slot rows) and FILTER (the work list) are independent.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[0]);
    vkCmdDispatch(cmd, (1024u + 63u) / 64u, 1, 1);
    if (in.instanceCount)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[1]);
        vkCmdDispatch(cmd, (in.instanceCount + 63u) / 64u, 1, 1);
    }
    if (cl)
    {
        // (#901 / #909 follow-up) the cluster-drawn parts count as their layers' work (CLEAR)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShClPipe[0]);
        vkCmdDispatchIndirect(cmd, in.clArgs, in.clArgsOffset);
    }
    shBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
              RW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    // CLEAR the due tiles that found work, then RASTER (one workgroup per work item, indirect).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[2]);
    vkCmdDispatch(cmd, gx, gy, 1);
    shBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[3]);
    vkCmdDispatchIndirect(cmd, mShBuf.buffer, 16);
    if (cl)
    {
        // (#901 / #909 follow-up) and their triangles from the skinned arena (the same atomics)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShClPipe[1]);
        vkCmdDispatchIndirect(cmd, in.clArgs, in.clArgsOffset);
    }
    shBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
    // PUBLISH into this recording's atlas image.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[4]);
    vkCmdDispatch(cmd, gx, gy, 1);
    if (mesh)
    {
        // The avatar snow mesh from the atlas just written (every second texel a vertex).
        shBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShPipe[5]);
        vkCmdDispatch(cmd, (kShAtlasW / 2u + 7u) / 8u, (mShTilesH / 2u + 7u) / 8u, 1);
        mShMeshBuilt = true;
    }
    // -> the lit FS (binding 49), the simulation / snow passes, the next HUD / offscreen culls
    // (they overwrite the candidates this chain read: an execution dependency), the readback;
    // the avatar snow mesh's draws (indirect, vertex / index fetch).
    shBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                  | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | RW | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
                  | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    if (mShRead[mSlot].buffer)
    {
        const VkBufferCopy rc{0, 0, kShRecordBytes};   // (the head + every layer's frame / counts: the log)
        vkCmdCopyBuffer(cmd, mShBuf.buffer, mShRead[mSlot].buffer, 1, &rc);
        shBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
        mShReadWritten[mSlot] = true;
    }
    mShPub = mShDst;
}

// The record a frame slot's last chain copied back (kShRecordBytes; clean_weather_shelter.glsl
// WxShelter): head [0] work items appended (past the capacity: dropped), [1] the capacity; per
// layer info (16 + 4L), count (80 + L), frame (96 + 4L), ext (160 + 4L), reach (288 + L), #901
// wet (304 + 4L: top, body, top exposure, body exposure), attach (368 + L), stat (384: drops
// hidden, drops splashed, wetness resets -- cumulative).
void LLCleanWeatherVK::readShelterHead(const uint32_t* head)
{
    static LLCachedControl<bool> s_dbg(gSavedSettings, "RenderVKWeatherAvatarShelterDebug", false);
    const uint32_t cap = head[1];
    if (!cap) return;
    const uint32_t items = std::min(head[0], cap);
    sShelterStats.items = items;
    sShelterStats.itemDropped = head[0] - items;
    sShelterStats.itemPeak = std::max(sShelterStats.itemPeak, items);
    if (sShelterStats.itemDropped) ++sShelterStats.overflowSteps;
    const F64 now = LLTimer::getTotalSeconds();
    // #901 PER-AVATAR WETNESS: each layer's avatar's state into the CPU table (by slot: the seed of
    // a re-created atlas), its top's exposure (the covered-layer skip).
    for (uint32_t L = 0; L < kShLayers; ++L)
    {
        const uint32_t sl = head[16 + 4 * L];
        if (sl >= 1024u) continue;
        float w[4];
        std::memcpy(w, &head[304 + 4 * L], sizeof(w));
        if (!(w[0] > -1.f)) { mShExpTop[L] = -1.f; continue; }   // (wetness off: none)
        if (mShLayerSlot[L] == (int32_t)sl) mShExpTop[L] = w[2];
        ShWetEntry& we = s_wet[sl];
        if (s_slotOwner[sl].isNull()) continue;
        we.id = s_slotOwner[sl];
        we.top = w[0]; we.body = w[1]; we.expTop = w[2]; we.expBody = w[3];
        we.t = now;
        we.known = true;
    }
    // (at most once a second: when the last tracked avatar is dry, by the plain drying rate)
    if (now >= s_wetScanAt)
    {
        s_wetScanAt = now + 1.0;
        F64 until = 0.0;
        for (const ShWetEntry& we : s_wet)
            if (we.known && now - we.t < 120.0)
                until = std::max(until, we.t + (F64)(std::max(we.top, we.body) / std::max(s_dryPerSec, 1e-5f)));
        s_wetUntil = until;
    }
    static F64 s_next = 0.0;
    if (now >= s_next)
    {
        const F64 span = s_next > 0.0 ? now - (s_next - (s_dbg ? 2.0 : 10.0)) : 0.0;
        s_next = now + (s_dbg ? 2.0 : 10.0);
        // THE OWN AVATAR'S LAYER: what its last raster drew -- the draws (the umbrella is one of
        // them; of them worn rigid attachments), the reach / extent (an attachment off the tile
        // shows as a reach at the cap with nothing over the body), its frame, its wetness.
        if (mShSelfSlot != 0xFFFFFFFFu)
            for (uint32_t L = 0; L < kShLayers; ++L)
            {
                if (head[16 + 4 * L] != mShSelfSlot) continue;
                float fr[4], ex[4], w[4], reach = 0.f;
                std::memcpy(fr, &head[96 + 4 * L], sizeof(fr));
                std::memcpy(ex, &head[160 + 4 * L], sizeof(ex));
                std::memcpy(w, &head[304 + 4 * L], sizeof(w));
                std::memcpy(&reach, &head[288 + L], sizeof(reach));
                LL_INFOS("VKWeather") << "weather avatar shelter, own avatar: layer " << L << " (slot " << mShSelfSlot << ", "
                                      << head[16 + 4 * L + 1] << "^2, due " << (head[16 + 4 * L + 3] & 1u) << "), draws rasterized "
                                      << head[80 + L] << " (worn rigid attachments " << head[368 + L] << "), reach " << reach
                                      << " m, footprint " << ex[0] << " x " << ex[0] << " m, pelvis " << fr[0] << " " << fr[1]
                                      << " " << fr[2] << " yaw " << fr[3] << "; wetness top " << w[0] << " body " << w[1]
                                      << ", exposure top " << w[2] << " body " << w[3] << LL_ENDL;
            }
        // EVERY LAYER: slot, draws (attachments), footprint, wetness / exposure.
        std::string per;
        for (uint32_t L = 0; L < kShLayers; ++L)
        {
            const uint32_t sl = head[16 + 4 * L];
            if (sl == 0xFFFFFFFFu) continue;
            float ex = 0.f, w[4];
            std::memcpy(&ex, &head[160 + 4 * L], sizeof(ex));
            std::memcpy(w, &head[304 + 4 * L], sizeof(w));
            per += llformat(" [L%u s%u%s %u/%ua %.2fm w%.2f/%.2f e%.2f/%.2f]", L, sl, sl == mShSelfSlot ? "*" : "", head[80 + L],
                            head[368 + L], ex, w[0], w[1], w[2], w[3]);
        }
        // THE SIMULATION'S COUNTERS (cumulative, wrapping): the drops the shelter hid / splashed.
        const uint32_t st[3] = {head[384], head[385], head[386]};
        uint32_t d[3] = {0u, 0u, 0u};
        if (mShStatHave)
            for (int i = 0; i < 3; ++i) d[i] = st[i] - mShStatPrev[i];
        std::memcpy(mShStatPrev, st, sizeof(st));
        mShStatHave = true;
        const ShWetEntry* self = mShSelfSlot < 1024u ? &s_wet[mShSelfSlot] : nullptr;
        LL_INFOS("VKWeather") << "weather avatar shelter: configuration " << sShelterStats.config << ", layers "
                              << sShelterStats.layers << "/" << sShelterStats.layerCap << " (peak " << sShelterStats.layerPeak
                              << ", re-rastered " << sShelterStats.due << " last recording), raster items " << items << "/" << cap
                              << " (peak " << sShelterStats.itemPeak << ", dropped " << sShelterStats.itemDropped
                              << ", overflow steps " << sShelterStats.overflowSteps << "), " << (sShelterStats.bytes >> 10)
                              << " KB; drops hidden by shelter " << d[0] << " (" << (span > 0.0 ? (F64)d[0] / span : 0.0)
                              << "/s), splashed on one " << d[1] << ", wetness resets " << d[2] << "; own wetness "
                              << (self && self->known ? self->top : -1.f) << " top / " << (self && self->known ? self->body : -1.f)
                              << " body, exposure " << (self && self->known ? self->expTop : -1.f) << " / "
                              << (self && self->known ? self->expBody : -1.f) << ";" << per << LL_ENDL;
    }
}

// #901: while an avatar the chain tracked is still wet (the estimate from the last readbacks),
// the weather module stays / comes back so it dries on its own clock.
bool LLCleanWeatherVK::avatarsWetHold()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKWeatherAvatarShelter", true);
    static LLCachedControl<bool> s_wetOn(gSavedSettings, "RenderVKWeatherAvatarWetness", true);
    if (!SLWeather::masterEnabled())
    {
        // #904: the weather off clears the avatars' snow and wetness NOW -- what outlives the module
        // on the CPU side (the wetness seeds, the own avatar's accumulation grid) goes too, so
        // turning it back on does not bring them back
        for (ShWetEntry& we : s_wet) we.known = false;
        s_wetUntil = 0.0;
        clearAccumSnapshot();
        return false;
    }
    return s_on && ((s_wetOn && LLTimer::getTotalSeconds() < s_wetUntil) || avatarSnowHold());
}
