/**
 * @file llcleanterrainbakevk.cpp
 * @brief PER-REGION TERRAIN BAKE of the Vulkan world renderer (#811, RenderVKTerrainBake).
 *
 * Why: every terrain pixel re-fetched and blended all four terrain materials every frame
 * (PBR terrain: base / normal / ORM / emissive per material, up to 16 fetches, x3 in hex
 * tiling's blend bands; legacy: the 4 detail textures), so the terrain G-buffer FS needs 72
 * registers (112 with hex tiling) against 48 for everything else, on every terrain pixel out
 * to the horizon, and the same cost again in every probe / mirror capture face.
 *
 * What: each region's blended surface is rendered ONCE (clean_terrain_bake.comp, the live
 * FS's own material code, clean_terrain_mat.glsl) into region-local textures; far terrain
 * samples 1-3 textures through a slim G-buffer FS, near terrain keeps the live FS (detail
 * textures repeat every few metres: ~100-300 texels / m up close, which no per-region
 * texture holds), crossfaded over a band, and captures take the bake at every distance.
 *
 * NUMBERS (defaults; one 256 m region):
 *  - RenderVKTerrainBakeRes 2048 = 8 texels / m (0.125 m texels). A: RGBA8 sRGB (albedo +
 *    PBR occlusion), B (PBR only): RGBA8 (Ng-relative normal xy, roughness, metallic), E
 *    (only when a material emits): RGBA8 sRGB. With mips: 22.4 MB legacy, 44.7 MB PBR
 *    (67 MB with emissive). 4096 would be 89 / 179 MB: too much for the 8 GB cards a
 *    region ring has to fit on; 1024 (5.6 / 11 MB) is what the second ring needs (below).
 *  - Resolution per region follows its distance (RenderVKTerrainBakeAuto): the texels a
 *    region needs where it is first seen = size / (distance x pixel angle). 1080p at 60 deg
 *    (1.07 mrad / px): a region within ~120 m bakes at 2048, the next ring (~250 m) at 1024,
 *    ~500 m at 512 -- a 3 x 3 legacy ring ~22 x 3 + 6 x 6 ~ 100 MB, PBR twice that.
 *  - Crossfade start d0 = max(RenderVKTerrainBakeNear 48 m, Auto x texel / pixel angle):
 *    where a bake texel covers one screen pixel, so the live FS's mip there is no sharper
 *    than the bake's level 0 (anisotropic sampling picks its mip by the minor axis, the
 *    across-view footprint = distance x pixel angle). 2048 at 1080p: ~117 m; 4K: ~234 m
 *    (Auto 0.5 halves it, at most one mip softer at the switch). Band 16 m.
 *  - Cost: a texel runs the live material blend once -- PBR + hex ~8-24 fetches, legacy 4
 *    (x3 hex) -- 4.2 M texels for a 2048 bake: ~1-3 ms of GPU on a GTX 1080 for a PBR +
 *    hex region, spread over 16 frames by RenderVKTerrainBakeKTexels 256 (~0.1-0.2 ms per
 *    frame) plus ~0.1 ms of mip blits at the end. One job at a time, nearest region first.
 *
 * INVALIDATION. Per region, every frame (cheap: no per-texel work on the CPU):
 *  - CONTENT hash: LLCleanTerrainInputs::hash (every ingested patch's heights + composition),
 *    the region's terrain record bytes (material ids, UV transforms, legacy scale / offset,
 *    the legacy material id, mode, PBR detail, paint type), each used material's stored
 *    factors / flags + texture UUIDs, the hex setting, the grid geometry and the record
 *    index. A change clears the table row the same frame (live shading) and frees the bake;
 *    a job in flight is aborted. A new bake starts once the content has been quiet 1 s.
 *  - QUALITY: the source textures' resident texels (the resolved material slots' sizes).
 *    Growth (streaming finished) re-bakes after 2 s quiet while the old bake stays shown;
 *    shrinking never does (a bake is self-contained). A job aborts if it changes mid-job
 *    (its rows would mix two residencies). The wanted resolution changing (the camera
 *    moved) re-bakes after 3 s quiet, the old one shown meanwhile.
 *  - LIVENESS: a region that left the world list, or lies past the draw distance + 256 m,
 *    frees its bake (and job) at once.
 * No disk cache: a bake is ~1-3 GPU-ms from inputs the viewer already holds, a disk copy
 * would be 5-45 MB per region of I/O for the same result.
 *
 * MEMORY. RenderVKTerrainBakeMB (default 384 MB): nearest regions first; a farther region's
 * bake is freed for a nearer one, a candidate that still does not fit tries half the
 * resolution, then waits (counted "refused"). Soft pressure: the cap halves while the
 * texture pressure level or the VRAM ladder is engaged, and is 0 from the ladder's geometry
 * rung (STEP_GEOM_LOD) / texture level 2 -- every bake freed, the live FS draws all terrain,
 * settings untouched; it comes back when the pressure clears. Every allocation is soft:
 * refused -> retried 5 s, then 10 s later; the third failure drops the region for 60 s.
 *
 * TRANSITION. The G-buffer terrain list is drawn twice (clean_draw.vert CLEAN_VS_TERRAIN_SIDE):
 * the live terrain FS (crossfades to the bake over [d0, d0 + band], skips its own fetches
 * past it) for every patch whose box reaches nearer than d0 + band, and the slim bake-only FS
 * for the rest. A bake that lands sweeps its d0 in from the draw distance over 1.5 s instead
 * of switching at once (no pop).
 *
 * STAGE 2 (not built; design note): near terrain stays live because a region texture cannot
 * hold ~100-300 texels / m. The next step is a camera-centred clipmap of the same blend:
 * L levels (e.g. 6) of 1024^2 texels each at 2x the previous footprint (level 0: 0.03 m
 * texels over 32 m, level 5: ~1 m over 1 km), toroidally updated as the camera moves (only
 * the newly exposed L-shaped strips re-baked by this compute, a few thousand texels per level
 * per frame), sampled by a slim FS that picks the level from the pixel footprint
 * (log2(footprint / level-0 texel)) and blends two levels. That removes the live FS entirely
 * (and the d0 parity floor), at ~4 MB x 6 levels x 2 images. Needs: the bake compute's
 * per-texel world XY (region-crossing: a texel picks its region's record / grid), a toroidal
 * offset per level, and invalidation by region content hash of the covered regions.
 */

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_pipeline.h"

#include "llerror.h"
#include "llframetimer.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llviewercamera.h"
#  include "llviewerregion.h"
#  include "llsurface.h"
#  include "llworld.h"
#  include "llcleaningest.h"
#  include "llcleanmaterialstore.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
// = clean_terrain_bake.comp BakePush (48 B).
struct TerrBakePush
{
    uint32_t view, rec, mat, flags;
    uint32_t org[2], ext[2];
    uint32_t grid_n;
    float    mpg, texel_m, pad0;
};
static_assert(sizeof(TerrBakePush) == 48, "TerrBakePush = clean_terrain_bake.comp BakePush");
constexpr uint32_t kBkPbr = 1u, kBkHex = 2u, kBkEmis = 4u;   // BK_*
constexpr uint32_t kTbReady = 1u, kTbPbr = 2u;               // TB_*

constexpr double kContentQuiet = 1.0;   // s the content must be still before a bake starts
constexpr double kQualQuiet    = 2.0;   // s the source residency must be still (re-bake / start)
constexpr double kResQuiet     = 3.0;   // s a changed wanted resolution must hold
constexpr double kSweepSec     = 1.5;   // a landing bake's crossfade sweeps in over this
constexpr uint32_t kMinRes = 256, kMaxRes = 4096;

inline uint64_t mix64(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 31; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 29;
    return h;
}
inline uint64_t mixBytes(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t x = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { x ^= b[i]; x *= 0x100000001b3ull; }
    return mix64(h, x);
}
inline uint32_t mipLevels(uint32_t res)
{
    uint32_t l = 1;
    while ((res >> l) > 0) ++l;
    return l;
}
double nowSec() { return LLFrameTimer::getElapsedSeconds(); }
}

bool LLCleanRendererVK::terrBakeOn() const
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKTerrainBake", false);
    // (mGbufTerrBakePipe: the live terrain VS drops the patches a ready bake covers, so no row
    // may say "ready" unless the slim pipeline that draws them exists)
    // (the clamp sampler is made by the first tick that finds this true)
    return s_on && mTerrBakePipe && mGbufTerrBakePipe && mTerrBakeLayout && mTerrBakeTable.buffer
        && mDsetTerrBake[0] != VK_NULL_HANDLE;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------
void LLCleanRendererVK::terrBakeInit()
{
    static_assert(kTerrBakeSlots == kTexDescSets, "kTerrBakeSlots must equal kTexDescSets");
    static_assert(sizeof(TerrBakeRow) == 32, "TerrBakeRow = clean_terrain_bake.glsl TerrBake (32 B)");
    // (first: the table is zeroed by terrBakeRecord from these even when the rest fails --
    // the live terrain VS reads it unconditionally)
    mTerrBakeRows.assign(kTerrBakeCap, TerrBakeRow{});
    mTerrBakeRowsGpu.assign(kTerrBakeCap, TerrBakeRow{});
    VkDevice dev = mCtx->device();
    // set 2: 0 the job's grid (SSBO), 1..3 the A / B / E storage views (level 0).
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4; li.pBindings = b;
    if (vkdCreateSetLayout(dev, &li, &mDslTerrBake, "mDslTerrBake") != VK_SUCCESS) { mDslTerrBake = VK_NULL_HANDLE; return; }
    if (mTerrBakePool.create(dev, "terrbake", {{mDslTerrBake, kTerrBakeSlots}}) != VK_SUCCESS) return;
    for (uint32_t s = 0; s < kTerrBakeSlots; ++s)
        if (mTerrBakePool.allocate(mDslTerrBake, &mDsetTerrBake[s]) != VK_SUCCESS) { mDsetTerrBake[0] = VK_NULL_HANDLE; return; }
    const VkDescriptorSetLayout sets[3] = {mDslDraw0, mDslDraw1, mDslTerrBake};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TerrBakePush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 3; pl.pSetLayouts = sets; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &pl, &mTerrBakeLayout, "mTerrBakeLayout") != VK_SUCCESS)
        mTerrBakeLayout = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kTerrBakeSlots * 2u;
    if (vkCreateQueryPool(dev, &qi, nullptr, &mTerrBakeQuery) != VK_SUCCESS) mTerrBakeQuery = VK_NULL_HANDLE;
}

void LLCleanRendererVK::terrBakeDestroy()
{
    // Device idle (shutdown): slots / images / views / buffers go through the retire lists the
    // caller frees right after; the objects owned here are destroyed now.
    for (auto& kv : mTerrBakes)
    {
        terrBakeFreeSet(kv.second.cur);
        terrBakeAbortJob(kv.second);
    }
    mTerrBakes.clear();
    mTerrBakeLiveBytes = 0;
    VkDevice dev = mCtx->device();
    if (mTerrBakePipe) { vkDestroyPipeline(dev, mTerrBakePipe, nullptr); mTerrBakePipe = VK_NULL_HANDLE; }
    if (mTerrBakeLayout) { vkDestroyPipelineLayout(dev, mTerrBakeLayout, nullptr); mTerrBakeLayout = VK_NULL_HANDLE; }
    mTerrBakePool.destroy();
    for (VkDescriptorSet& s : mDsetTerrBake) s = VK_NULL_HANDLE;
    if (mDslTerrBake) { vkdDestroySetLayout(dev, mDslTerrBake); mDslTerrBake = VK_NULL_HANDLE; }
    if (mTerrBakeSampler) { vkDestroySampler(dev, mTerrBakeSampler, nullptr); mTerrBakeSampler = VK_NULL_HANDLE; }
    if (mTerrBakeQuery) { vkDestroyQueryPool(dev, mTerrBakeQuery, nullptr); mTerrBakeQuery = VK_NULL_HANDLE; }
    if (mTerrBakeTable.buffer) mCtx->destroyBuffer(mTerrBakeTable);
    mTerrBakeTable = GpuBuffer{};
    mTerrBakeTableValid = false;
    mTexSamplerOv.clear();
}

// ---------------------------------------------------------------------------
// bindless slots (slot-owned images, like the white / media slots)
// ---------------------------------------------------------------------------
uint32_t LLCleanRendererVK::terrBakeAllocSlot(const GpuImage& img)
{
    // Slot 0 is the white texture (seeded by the first texture pass): nothing before it.
    if (mTextures.empty() || mVkTexKey.size() != mTextures.size()) return 0;
    uint32_t idx;
    if (!mFreeTexSlots.empty())
    {
        idx = mFreeTexSlots.back();
        mFreeTexSlots.pop_back();
    }
    else
    {
        if (mTextures.size() >= mMaxBindless) return 0;
        mTextures.push_back(GpuImage{});
        mVkTexKey.push_back(0); mVkTexShared.push_back(0);
        mVkTexGen.push_back(0);
        mVkTexW.push_back(0); mVkTexH.push_back(0);
        mTexLastUsed.push_back(mTexPass);
        idx = (uint32_t)mTextures.size() - 1;
    }
    mTextures[idx] = img;
    mVkTexKey[idx] = 0; mVkTexShared[idx] = 0; mVkTexGen[idx] = 0;
    mVkTexW[idx] = img.extent.width; mVkTexH[idx] = img.extent.height;
    mTexLastUsed[idx] = mTexPass;
    if (mTexSamplerOv.size() <= idx) mTexSamplerOv.resize((size_t)idx + 1u, VK_NULL_HANDLE);
    mTexSamplerOv[idx] = mTerrBakeSampler;
    touchTexSlot(idx);
    return idx;
}

void LLCleanRendererVK::terrBakeFreeSet(TerrBakeSet& s)
{
    for (uint32_t k = 0; k < 3; ++k)
    {
        if (s.slot[k])
        {
            // Only a slot that still holds OUR image: one the texture eviction sweep took
            // (unstamped, e.g. a long offscreen run) was already released -- its image retired
            // -- and may hold another texture now: drop the reference, touch nothing.
            const uint32_t sl = s.slot[k];
            if (sl < mTextures.size() && mTextures[sl].image == s.img[k].image)
            {
                if (sl < mTexSamplerOv.size()) mTexSamplerOv[sl] = VK_NULL_HANDLE;
                releaseTexSlot(sl);   // retires the image + its view (freed after the frames in flight)
                if (sl < mVkTexKey.size()) { mVkTexKey[sl] = 0; mVkTexW[sl] = mVkTexH[sl] = 0; }
                mFreeTexSlots.push_back(sl);
            }
        }
        else if (s.img[k].image)
            retireImage(s.img[k]);
        if (s.stor[k]) retireView(s.stor[k]);
    }
    s = TerrBakeSet{};
}

void LLCleanRendererVK::terrBakeAbortJob(TerrBakeRegion& r)
{
    if (r.job.live()) ++mTerrBakeAborted;
    terrBakeFreeSet(r.job);
    if (r.job_grid.buffer) retireBuffer(r.job_grid);
    if (r.job_stage.buffer) retireBuffer(r.job_stage);
    r.job_grid = GpuBuffer{}; r.job_stage = GpuBuffer{};
    r.job_row = 0; r.job_started = false; r.job_done = false;
}

// ---------------------------------------------------------------------------
// job start (main thread, tick): images, storage views, the input grid
// ---------------------------------------------------------------------------
bool LLCleanRendererVK::terrBakeStartJob(TerrBakeRegion& r, uint32_t res)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    const LLCleanTerrainInputs* ti = LLCleanIngest::terrainInputs(r.handle);
    LLViewerRegion* rg = LLWorld::getInstance()->getRegionFromHandle(r.handle);
    if (!ti || !rg || ti->gpe < 2 || ti->comp.size() < (size_t)ti->gpe * (size_t)ti->gpe) return false;
    const LLSurface& surf = rg->getLand();
    if (surf.getGridsPerEdge() != ti->gpe) return false;

    TerrBakeSet& j = r.job;
    j = TerrBakeSet{};
    j.res = res; j.levels = mipLevels(res);
    j.pbr = r.pbr; j.emis = r.pbr && r.emis;
    j.content = r.content; j.qual = r.qual;
    bool ok = true;
    {
        VkMemScope memcat(VKMEM_TEXTURE);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // A / E: sRGB images (sampled sRGB, blitted in linear light) written through a UNORM
        // storage view (sRGB formats have no storage support): MUTABLE + EXTENDED_USAGE.
        static const VkFormat kSrgbPair[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
        VkImageExtra ex{};
        ex.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        ex.viewFormats = kSrgbPair; ex.viewFormatCount = 2;
        ex.defaultViewUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (uint32_t k = 0; k < 3 && ok; ++k)
        {
            if (k == 1 && !j.pbr) continue;
            if (k == 2 && !j.emis) continue;
            const bool srgb = k != 1;
            j.img[k] = mCtx->createImageLayers(res, res, 1, j.levels,
                srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                false, VK_IMAGE_VIEW_TYPE_2D, "terrainBake", srgb ? &ex : nullptr);
            if (!j.img[k].image) { ok = false; break; }
            j.stor[k] = mCtx->createViewSoft(j.img[k].image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, srgb ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
            if (!j.stor[k]) { ok = false; break; }
            j.bytes += std::max<uint64_t>(mCtx->allocBytes(j.img[k]), 1);
        }
        // The input grid: (comp, dz/dx, dz/dy, 0) per grid point -> device-local via staging.
        const VkDeviceSize gbytes = (VkDeviceSize)ti->gpe * (VkDeviceSize)ti->gpe * 16u;
        if (ok)
        {
            VkMemScope memstg(VKMEM_STAGING);
            r.job_stage = mCtx->createBuffer(gbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ok = r.job_stage.buffer && r.job_stage.mapped;
        }
        if (ok)
        {
            r.job_grid = mCtx->createBuffer(gbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ok = r.job_grid.buffer != VK_NULL_HANDLE;
        }
        if (ok) j.bytes += (uint64_t)gbytes;
    }
    if (!ok)
    {
        const bool counted = r.job.live();
        terrBakeAbortJob(r);   // (frees what was made; not counted as an abort: nothing ran)
        if (counted) --mTerrBakeAborted;
        ++r.fails;
        const double now = nowSec();
        r.fail_until = now + (r.fails >= 3 ? 60.0 : 5.0 * r.fails);
        LL_WARNS("VKTerrBake") << "terrain bake: allocation refused for region " << r.handle << " (" << res << "^2, "
            << (r.pbr ? "PBR" : "legacy") << "), " << (r.fails >= 3 ? "dropped for 60 s" : "retry later")
            << " (failure " << r.fails << ")" << LL_ENDL;
        if (r.fails >= 3) r.fails = 0;
        return false;
    }
    // Fill the grid (heights through the region surface: the slopes the ingest's normals use).
    {
        const S32 n = ti->gpe;
        const F32 inv2 = 0.5f / std::max(ti->mpg, 1e-3f);
        float* g = static_cast<float*>(r.job_stage.mapped);
        for (S32 y = 0; y < n; ++y)
            for (S32 x = 0; x < n; ++x)
            {
                const F32 zl = surf.getZ(llclamp(x - 1, 0, n - 1), y), zr = surf.getZ(llclamp(x + 1, 0, n - 1), y);
                const F32 zd = surf.getZ(x, llclamp(y - 1, 0, n - 1)), zu = surf.getZ(x, llclamp(y + 1, 0, n - 1));
                float* o = g + ((size_t)y * (size_t)n + (size_t)x) * 4u;
                o[0] = ti->comp[(size_t)y * (size_t)n + (size_t)x];
                o[1] = (zr - zl) * inv2;
                o[2] = (zu - zd) * inv2;
                o[3] = 0.f;
            }
    }
    r.fails = 0;
    r.job_row = 0; r.job_started = false; r.job_done = false;
    r.job_t = nowSec();
    return true;
#else
    (void)r; (void)res;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// tick (main thread, recordWorld before beginTextureFrame)
// ---------------------------------------------------------------------------
void LLCleanRendererVK::terrBakeTick(uint32_t rh)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (!mTerrBakeLayout) return;
    const uint32_t fslot = mFrameSlot % kTerrBakeSlots;
    // This slot's previous frame finished (acquire waited its fence): its bake timestamps.
    if (mTerrBakeTimed[fslot] && mTerrBakeQuery)
    {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(mCtx->device(), mTerrBakeQuery, fslot * 2u, 2u, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0])
        {
            const double ms = (double)(ts[1] - ts[0]) * mCtx->limits().timestampPeriod * 1e-6;
            mTerrBakeGpuMs += ms;
            mTerrBakeGpuMsMax = std::max(mTerrBakeGpuMsMax, ms);
            ++mTerrBakeGpuFrames;
        }
        mTerrBakeTimed[fslot] = false;
    }
    // Keep every registered bake slot stamped for the texture eviction sweep (every recording,
    // offscreen ones too: a long snapshot run must not let the sweep take them).
    for (auto& kv : mTerrBakes)
        for (uint32_t k = 0; k < 3; ++k)
        {
            const uint32_t sl = kv.second.cur.slot[k];
            if (sl && sl < mTexLastUsed.size()) mTexLastUsed[sl] = mTexPass;
        }
    if (mOffscreen) return;   // (snapshot recordings: the live view's state stays as it is)
    ++mTerrBakeTick;
    const double now = nowSec();
    const bool on = terrBakeOn();
    std::fill(mTerrBakeRows.begin(), mTerrBakeRows.end(), TerrBakeRow{});
    if (!on)
    {
        if (!mTerrBakes.empty())
        {
            for (auto& kv : mTerrBakes) { terrBakeFreeSet(kv.second.cur); terrBakeAbortJob(kv.second); }
            mTerrBakes.clear();
            LL_INFOS("VKTerrBake") << "terrain bake off: every bake freed" << LL_ENDL;
        }
        mTerrBakeLiveBytes = 0;
        return;
    }

    static LLCachedControl<U32> s_res(gSavedSettings, "RenderVKTerrainBakeRes", 2048);
    static LLCachedControl<F32> s_near(gSavedSettings, "RenderVKTerrainBakeNear", 48.f);
    static LLCachedControl<F32> s_band(gSavedSettings, "RenderVKTerrainBakeBand", 16.f);
    static LLCachedControl<F32> s_auto(gSavedSettings, "RenderVKTerrainBakeAuto", 1.f);
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKTerrainBakeMB", 384);
    static LLCachedControl<bool> s_hex(gSavedSettings, "SlipstreamRenderTerrainHexTiling", false);
    uint32_t resMax = kMinRes;
    while (resMax < std::min<uint32_t>((U32)s_res, kMaxRes)) resMax <<= 1;
    const float nearM = llclamp((F32)s_near, 0.f, 4096.f);
    const float band  = llclamp((F32)s_band, 1.f, 1024.f);
    const float autoK = llclamp((F32)s_auto, 0.f, 8.f);
    const bool  hex   = s_hex;

    // Soft pressure (settings untouched): half the cap while texture / VRAM pressure is on,
    // nothing from the ladder's geometry rung / texture level 2 (live shading everywhere).
    uint64_t cap = (uint64_t)(U32)s_mb << 20;
    const uint32_t rung = mBudget.engaged();
    if (rung >= (uint32_t)LLVramBudgetVK::STEP_GEOM_LOD || mTexPressLvl >= 2) cap = 0;
    else if (rung > 0 || mTexPressLvl >= 1) cap /= 2;
    if (cap != mTerrBakeCapBytes)
    {
        LL_INFOS("VKTerrBake") << "terrain bake budget " << (mTerrBakeCapBytes >> 20) << " -> " << (cap >> 20)
            << " MB (setting " << (U32)s_mb << " MB, VRAM rung " << rung << ", texture pressure " << mTexPressLvl << ")" << LL_ENDL;
        mTerrBakeCapBytes = cap;
    }

    // Clamp sampler (trilinear, the world sampler's anisotropy): rebuilt on an aniso change.
    if (!mTerrBakeSampler || mTerrBakeSamplerAniso != mSamplerAniso)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.minLod = 0.f; sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.anisotropyEnable = mSamplerAniso > 1.f ? VK_TRUE : VK_FALSE;
        sci.maxAnisotropy = std::max(mSamplerAniso, 1.f);
        VkSampler ns = VK_NULL_HANDLE;
        if (vkCreateSampler(mCtx->device(), &sci, nullptr, &ns) == VK_SUCCESS)
        {
            if (mTerrBakeSampler) mPendRetire.samplers.push_back(mTerrBakeSampler);
            mTerrBakeSampler = ns;
            mTerrBakeSamplerAniso = mSamplerAniso;
            for (uint32_t sl = 0; sl < (uint32_t)mTexSamplerOv.size(); ++sl)
                if (mTexSamplerOv[sl]) { mTexSamplerOv[sl] = ns; touchTexSlot(sl, false); }
        }
        if (!mTerrBakeSampler) return;
    }

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin();
    const float pixAng = 2.f * tanf(0.5f * cam->getView()) / (float)std::max(rh, 1u);
    const float farM = cam->getFar();
    LLCleanMaterialStore& ms = LLCleanMaterialStore::instance();
    const uint32_t nRec = (uint32_t)(mCpuTerrain.size() / sizeof(LLCleanMaterialStore::TerrainRecord));
    const uint32_t nMat = (uint32_t)(mCpuMaterials.size() / sizeof(LLCleanMaterialStore::Material));
    const uint8_t* rawMat = ms.cpuMaterialBytes();
    const uint32_t nRawMat = ms.cpuMaterialCount();

    // ---- every region in the world list: inputs, hashes, distance ----
    for (LLViewerRegion* rg : LLWorld::getInstance()->getRegionList())
    {
        if (!rg) continue;
        const uint64_t handle = rg->getHandle();
        const LLCleanTerrainInputs* ti = LLCleanIngest::terrainInputs(handle);
        if (!ti || !ti->complete() || ti->gpe < 2) continue;   // patches still arriving: live shading
        const uint32_t rec = ms.findTerrainRecord(handle);
        if (rec == LLCleanMaterialStore::INVALID || rec >= kTerrBakeCap || rec >= nRec) continue;
        TerrBakeRegion& r = mTerrBakes[handle];
        r.handle = handle;
        r.seen = mTerrBakeTick;
        r.rec = rec;
        r.size = (float)(ti->gpe - 1) * ti->mpg;
        {
            const LLVector3 o = rg->getOriginAgent();
            const LLSurface& land = rg->getLand();
            const float dx = std::max({o.mV[VX] - eye.mV[VX], 0.f, eye.mV[VX] - (o.mV[VX] + r.size)});
            const float dy = std::max({o.mV[VY] - eye.mV[VY], 0.f, eye.mV[VY] - (o.mV[VY] + r.size)});
            const float dz = std::max({land.getMinZ() - eye.mV[VZ], 0.f, eye.mV[VZ] - land.getMaxZ()});
            r.dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        LLCleanMaterialStore::TerrainRecord tr;
        std::memcpy(&tr, mCpuTerrain.data() + (size_t)rec * sizeof(tr), sizeof(tr));
        r.pbr = tr.mode == 1u;
        r.hex = hex;
        uint32_t legacyMat = 0;
        std::memcpy(&legacyMat, &tr.legacy[3], sizeof(uint32_t));
        r.mat = legacyMat;
        uint32_t mats[4] = {0, 0, 0, 0};
        const uint32_t nm = r.pbr ? 4u : 1u;
        if (r.pbr) for (uint32_t i = 0; i < 4; ++i) mats[i] = tr.mat[i]; else mats[0] = legacyMat;

        uint64_t h = mix64(0x7e44a1u, handle);
        h = mix64(h, rec);
        h = mix64(h, ti->hash);
        h = mix64(h, (uint64_t)ti->gpe);
        h = mixBytes(h, &ti->mpg, sizeof(ti->mpg));
        h = mix64(h, hex ? 1u : 0u);
        h = mixBytes(h, &tr, sizeof(tr));
        uint64_t qual = 0;
        bool emis = false, matOk = true;
        for (uint32_t i = 0; i < nm; ++i)
        {
            const uint32_t m = mats[i];
            if (m >= nRawMat || m >= nMat) { matOk = false; break; }
            LLCleanMaterialStore::Material raw;
            std::memcpy(&raw, rawMat + (size_t)m * sizeof(raw), sizeof(raw));
            h = mixBytes(h, &raw, sizeof(raw));
            LLUUID ids[4];
            if (ms.materialTextureIds(m, ids)) h = mixBytes(h, ids, sizeof(ids));
            if (raw.emissive_factor[0] > 0.f || raw.emissive_factor[1] > 0.f || raw.emissive_factor[2] > 0.f) emis = true;
            // Residency: the resolved slots' sizes (the renderer's mirror holds the bindless index).
            LLCleanMaterialStore::Material res;
            std::memcpy(&res, mCpuMaterials.data() + (size_t)m * sizeof(res), sizeof(res));
            const uint32_t slots[4] = {res.base_color_tex[0], res.normal_tex[0], res.mr_tex[0], res.emissive_tex[0]};
            for (uint32_t sl : slots)
                if (sl > 0 && sl < mVkTexW.size()) qual += (uint64_t)mVkTexW[sl] * (uint64_t)mVkTexH[sl];
            // (+1 per resolved map: a map arriving counts even at its placeholder size)
            for (uint32_t b = 0; b < 4; ++b) qual += (res.flags >> b) & 1u;
        }
        if (!matOk) continue;   // (materials not mirrored yet: the hashes keep their last values)
        r.emis = r.pbr && emis && tr.pbr_detail >= 0;
        if (h != r.content) { r.content = h; r.content_t = now; }
        if (qual != r.qual) { r.qual = qual; r.qual_t = now; }
        // Wanted resolution: what the region's nearest point needs (parity at Auto).
        uint32_t want = resMax;
        if (autoK > 0.f && pixAng > 0.f)
        {
            const float need = r.size / std::max(std::max(r.dist, nearM) * pixAng * autoK, 1e-3f);
            const uint32_t have = r.cur.live() ? r.cur.res : 0u;
            // hysteresis: up when the bake is coarser than needed, down only well below half
            if (have && need <= (float)have && need >= 0.35f * (float)have) want = have;
            else
            {
                want = kMinRes;
                while ((float)want < need && want < resMax) want <<= 1;
            }
        }
        want = std::min(want, resMax);
        if (want != r.want_res) { r.want_res = want; r.want_t = now; }
    }

    // ---- liveness: regions gone / out of range; stale content; finished jobs ----
    const float keepM = farM + 256.f;
    mTerrBakeLiveBytes = 0;
    for (auto it = mTerrBakes.begin(); it != mTerrBakes.end();)
    {
        TerrBakeRegion& r = it->second;
        const bool alive = r.seen == mTerrBakeTick && r.content != 0;
        if (!alive || r.dist > keepM)
        {
            terrBakeFreeSet(r.cur);
            terrBakeAbortJob(r);
            if (!alive) { it = mTerrBakes.erase(it); continue; }
        }
        if (r.cur.live() && r.cur.content != r.content) terrBakeFreeSet(r.cur);   // content changed: never shown stale
        if (r.cur.live())
            for (uint32_t k = 0; k < 3; ++k)   // a slot the eviction sweep took: re-bake (terrBakeFreeSet)
                if (r.cur.slot[k] && (r.cur.slot[k] >= mTextures.size() || mTextures[r.cur.slot[k]].image != r.cur.img[k].image))
                { terrBakeFreeSet(r.cur); break; }
        if (r.job.live() && (r.job.content != r.content || r.job.qual != r.qual)) terrBakeAbortJob(r);
        if (r.job.live() && r.job_done)
        {
            // Mips recorded last frame: register the images (this frame's set is written next).
            bool ok = true;
            for (uint32_t k = 0; k < 3 && ok; ++k)
                if (r.job.img[k].image)
                {
                    r.job.slot[k] = terrBakeAllocSlot(r.job.img[k]);
                    ok = r.job.slot[k] != 0;
                }
            if (!ok)
            {
                LL_WARNS("VKTerrBake") << "terrain bake: no bindless slot for region " << r.handle << "; dropped" << LL_ENDL;
                terrBakeAbortJob(r);
                r.fail_until = now + 30.0;
            }
            else
            {
                const bool fresh = !r.cur.live();
                terrBakeFreeSet(r.cur);
                r.cur = r.job;
                r.job = TerrBakeSet{};
                // The storage views and the input grid only served the bake: retired now
                // (freed with the frames that may still use them).
                for (uint32_t k = 0; k < 3; ++k)
                    if (r.cur.stor[k]) { retireView(r.cur.stor[k]); r.cur.stor[k] = VK_NULL_HANDLE; }
                const uint64_t gridBytes = (uint64_t)r.job_grid.size;
                r.cur.bytes -= std::min(r.cur.bytes, gridBytes);
                if (r.job_grid.buffer) retireBuffer(r.job_grid);
                if (r.job_stage.buffer) retireBuffer(r.job_stage);
                r.job_grid = GpuBuffer{}; r.job_stage = GpuBuffer{};
                r.job_row = 0; r.job_started = false; r.job_done = false;
                if (fresh) r.ready_t = now;   // (a re-bake replaces in place: no sweep)
                ++mTerrBakeDone;
                mTerrBakeJobMs = (now - r.job_t) * 1000.0;
                LL_DEBUGS("VKTerrBake") << "terrain bake landed: region " << r.handle << " " << r.cur.res << "^2 "
                    << (r.cur.pbr ? (r.cur.emis ? "PBR+emissive" : "PBR") : "legacy") << ", "
                    << (r.cur.bytes >> 20) << " MB, " << r.dist << " m" << LL_ENDL;
            }
        }
        mTerrBakeLiveBytes += r.cur.bytes + r.job.bytes;
        ++it;
    }

    // ---- budget: over the (possibly shrunk) cap -> free the farthest ----
    auto farthest = [&](float beyond) -> TerrBakeRegion*
    {
        TerrBakeRegion* f = nullptr;
        for (auto& kv : mTerrBakes)
        {
            TerrBakeRegion& r = kv.second;
            if (!r.cur.live() && !r.job.live()) continue;
            if (r.dist <= beyond) continue;
            if (!f || r.dist > f->dist) f = &r;
        }
        return f;
    };
    while (mTerrBakeLiveBytes > mTerrBakeCapBytes)
    {
        TerrBakeRegion* f = farthest(-1.f);
        if (!f) break;
        mTerrBakeLiveBytes -= f->cur.bytes + f->job.bytes;
        terrBakeFreeSet(f->cur);
        terrBakeAbortJob(*f);
        ++mTerrBakeEvicted;
    }

    // ---- start the next job (one at a time, nearest region that needs one) ----
    bool busy = false;
    for (auto& kv : mTerrBakes) if (kv.second.job.live()) { busy = true; break; }
    if (!busy && mTerrBakeCapBytes > 0)
    {
        TerrBakeRegion* best = nullptr;
        for (auto& kv : mTerrBakes)
        {
            TerrBakeRegion& r = kv.second;
            if (r.dist > farM + 64.f || now < r.fail_until) continue;
            if (now - r.content_t < kContentQuiet || now - r.qual_t < kQualQuiet) continue;
            bool need = !r.cur.live();
            if (!need && r.qual > r.cur.qual) need = true;                         // sharper sources arrived
            if (!need && r.want_res != r.cur.res && now - r.want_t >= kResQuiet) need = true;   // distance moved
            if (!need) continue;
            if (!best || r.dist < best->dist) best = &r;
        }
        if (best)
        {
            uint32_t res = best->want_res ? best->want_res : resMax;
            auto est = [&](uint32_t rs)
            {
                const uint64_t one = (uint64_t)rs * rs * 4u * 4u / 3u;
                return one * (1u + (best->pbr ? 1u : 0u) + (best->pbr && best->emis ? 1u : 0u));
            };
            // Fit: free farther regions' bakes; still over -> half the resolution; else wait.
            // (A re-bake's own current set stays until the new one lands: counted.)
            while (mTerrBakeLiveBytes + est(res) > mTerrBakeCapBytes)
            {
                TerrBakeRegion* f = farthest(best->dist);
                if (f && f != best)
                {
                    mTerrBakeLiveBytes -= f->cur.bytes + f->job.bytes;
                    terrBakeFreeSet(f->cur);
                    terrBakeAbortJob(*f);
                    ++mTerrBakeEvicted;
                    continue;
                }
                if (res > kMinRes) { res >>= 1; continue; }
                break;
            }
            if (mTerrBakeLiveBytes + est(res) <= mTerrBakeCapBytes)
            {
                if (terrBakeStartJob(*best, res)) mTerrBakeLiveBytes += best->job.bytes;
            }
            else
            {
                ++mTerrBakeRefused;
                best->fail_until = now + 10.0;   // (re-evaluated when the budget moves)
            }
        }
    }
    mTerrBakePeakBytes = std::max(mTerrBakePeakBytes, mTerrBakeLiveBytes);

    // ---- table rows: every region with a registered bake ----
    uint32_t nLegacy = 0, nPbr = 0;
    for (auto& kv : mTerrBakes)
    {
        TerrBakeRegion& r = kv.second;
        if (!r.cur.live() || !r.cur.slot[0] || r.rec >= kTerrBakeCap) continue;
        if (r.cur.slot[0] >= mTextures.size() || mTextures[r.cur.slot[0]].image != r.cur.img[0].image) continue;   // (taken: see terrBakeFreeSet)
        (r.cur.pbr ? nPbr : nLegacy)++;
        const float texel = r.size / (float)std::max(r.cur.res, 1u);
        float d0 = std::max(nearM, (autoK > 0.f && pixAng > 0.f) ? autoK * texel / pixAng : 0.f);
        const float t = (float)llclamp((now - r.ready_t) / kSweepSec, 0.0, 1.0);
        d0 += (1.f - t) * (farM + 256.f);   // sweep in from the draw distance
        TerrBakeRow& row = mTerrBakeRows[r.rec];
        row.tex[0] = r.cur.slot[0];
        row.tex[1] = r.cur.pbr ? r.cur.slot[1] : 0u;
        row.tex[2] = r.cur.emis ? r.cur.slot[2] : 0u;
        row.tex[3] = kTbReady | (r.cur.pbr ? kTbPbr : 0u);
        row.xf[0] = 1.f / std::max(r.size, 1.f);
        row.xf[1] = d0;
        row.xf[2] = 1.f / band;
        row.xf[3] = d0 + band;
    }

    // ---- stats (live / cap / peak, bakes, GPU ms) every 30 s while anything happens ----
    if (now - mTerrBakeLogT >= 30.0 && (!mTerrBakes.empty() || mTerrBakeDone))
    {
        mTerrBakeLogT = now;
        std::ostringstream os;
        os << "VKTerrBake: " << (nLegacy + nPbr) << " regions baked (" << nLegacy << " legacy, " << nPbr << " PBR) of "
           << mTerrBakes.size() << " | live " << (mTerrBakeLiveBytes >> 20) << " MB / cap " << (mTerrBakeCapBytes >> 20)
           << " MB / peak " << (mTerrBakePeakBytes >> 20) << " MB | bakes " << mTerrBakeDone << " (aborted "
           << mTerrBakeAborted << ", refused " << mTerrBakeRefused << ", evicted " << mTerrBakeEvicted << ")";
        if (mTerrBakeGpuFrames)
            os << " | bake GPU " << std::fixed << std::setprecision(3) << (mTerrBakeGpuMs / mTerrBakeGpuFrames)
               << " ms/frame avg, " << mTerrBakeGpuMsMax << " max over " << mTerrBakeGpuFrames << " frames";
        os << " | last job " << std::fixed << std::setprecision(0) << mTerrBakeJobMs << " ms wall";
        LL_INFOS("VKTerrBake") << os.str() << LL_ENDL;
        mTerrBakeGpuMs = 0.0; mTerrBakeGpuMsMax = 0.0; mTerrBakeGpuFrames = 0;
    }
#else
    (void)rh;
#endif
}

// ---------------------------------------------------------------------------
// record (recordWorld, after the material / terrain-record streams)
// ---------------------------------------------------------------------------
void LLCleanRendererVK::terrBakeRecord(VkCommandBuffer cmd)
{
    if (!mTerrBakeTable.buffer) return;
    if (mTerrBakeRows.size() != kTerrBakeCap || mTerrBakeRowsGpu.size() != kTerrBakeCap)
    {
        mTerrBakeRows.assign(kTerrBakeCap, TerrBakeRow{});
        mTerrBakeRowsGpu.assign(kTerrBakeCap, TerrBakeRow{});
        mTerrBakeTableValid = false;
    }
    // ---- the table: the changed row range (all of it the first time: zeros = no bake) ----
    {
        uint32_t lo = kTerrBakeCap, hi = 0;
        if (!mTerrBakeTableValid) { lo = 0; hi = kTerrBakeCap; }
        else
            for (uint32_t i = 0; i < kTerrBakeCap; ++i)
                if (std::memcmp(&mTerrBakeRows[i], &mTerrBakeRowsGpu[i], sizeof(TerrBakeRow)) != 0)
                { lo = std::min(lo, i); hi = i + 1; }
        if (lo < hi)
        {
            // WAR: earlier frames' reads (VS / FS / compute) -> this update.
            VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                           | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.buffer = mTerrBakeTable.buffer; b.size = VK_WHOLE_SIZE;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &d);
            const VkDeviceSize off = (VkDeviceSize)lo * sizeof(TerrBakeRow);
            const VkDeviceSize bytes = (VkDeviceSize)(hi - lo) * sizeof(TerrBakeRow);   // <= 32 KB (< 65536)
            vkCmdUpdateBuffer(cmd, mTerrBakeTable.buffer, off, bytes, mTerrBakeRows.data() + lo);
            std::copy(mTerrBakeRows.begin() + lo, mTerrBakeRows.begin() + hi, mTerrBakeRowsGpu.begin() + lo);
            mTerrBakeTableValid = true;
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                           | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            vkCmdPipelineBarrier2(cmd, &d);
        }
    }
    if (mOffscreen || !terrBakeOn()) return;

    // ---- this frame's band of the active job ----
    TerrBakeRegion* jr = nullptr;
    for (auto& kv : mTerrBakes)
        if (kv.second.job.live() && !kv.second.job_done) { jr = &kv.second; break; }
    if (!jr || !jr->job_grid.buffer || !mTerrBakePipe) return;
    TerrBakeRegion& r = *jr;
    TerrBakeSet& j = r.job;
    const uint32_t s = mFrameSlot % kTerrBakeSlots;
    uint32_t ktex = 256;
#if defined(LL_HAS_CLEAN_PIPELINE)
    {
        static LLCachedControl<U32> s_ktex(gSavedSettings, "RenderVKTerrainBakeKTexels", 256);
        ktex = llclamp((U32)s_ktex, 16u, 16384u);
    }
#endif
    uint32_t rows = std::max<uint32_t>(8u, (ktex * 1024u) / std::max(j.res, 1u));
    rows = std::min((rows + 7u) & ~7u, j.res - r.job_row);

    if (!r.job_started)
    {
        // Grid staging -> device-local; level 0 of every image -> GENERAL (compute writes).
        VkBufferCopy bc{0, 0, r.job_stage.size};
        vkCmdCopyBuffer(cmd, r.job_stage.buffer, r.job_grid.buffer, 1, &bc);
        VkImageMemoryBarrier2 ib[3]{};
        uint32_t n = 0;
        for (uint32_t k = 0; k < 3; ++k)
        {
            if (!j.img[k].image) continue;
            VkImageMemoryBarrier2& b = ib[n++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE; b.srcAccessMask = 0;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = j.img[k].image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &d);
        r.job_started = true;
    }
    {
        // Everything written before (the grid copy above; this or earlier frames' material /
        // record streams and texture uploads, whose own barriers name the fragment stage only)
        // -> visible to the bake compute. One per baking frame.
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    // This slot's set (its previous frame finished): the job's grid + storage views.
    {
        VkDescriptorBufferInfo gi{r.job_grid.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii[3]{};
        for (uint32_t k = 0; k < 3; ++k)
        {
            ii[k].imageView = j.stor[k] ? j.stor[k] : j.stor[0];   // (unused outputs alias A: never written)
            ii[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetTerrBake[s];
            w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (i == 0) w[i].pBufferInfo = &gi; else w[i].pImageInfo = &ii[i - 1];
        }
        vkUpdateDescriptorSets(mCtx->device(), 4, w, 0, nullptr);
    }
    if (mTerrBakeQuery)
    {
        vkCmdResetQueryPool(cmd, mTerrBakeQuery, s * 2u, 2u);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mTerrBakeQuery, s * 2u);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTerrBakePipe);
    const VkDescriptorSet sets[3] = {mDsetDraw0, mDsetDraw1, mDsetTerrBake[s]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTerrBakeLayout, 0, 3, sets, 0, nullptr);
    TerrBakePush p{};
    p.rec = r.rec; p.mat = r.mat;
    p.flags = (j.pbr ? kBkPbr : 0u) | (r.hex ? kBkHex : 0u) | (j.emis ? kBkEmis : 0u);
    p.org[0] = 0; p.org[1] = r.job_row;
    p.ext[0] = j.res; p.ext[1] = rows;
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (const LLCleanTerrainInputs* ti = LLCleanIngest::terrainInputs(r.handle))
    { p.grid_n = (uint32_t)ti->gpe; p.mpg = ti->mpg; }
#endif
    if (p.grid_n < 2) { terrBakeAbortJob(r); return; }   // (region inputs gone: the tick frees it)
    p.texel_m = r.size / (float)j.res;
    vkCmdPushConstants(cmd, mTerrBakeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (j.res + 7u) / 8u, (rows + 7u) / 8u, 1u);
    r.job_row += rows;

    if (r.job_row >= j.res)
    {
        // Last band: level 0 -> the mip chain (blits, linear light for the sRGB images), then
        // every level -> SHADER_READ_ONLY for the terrain FS (registered by the next tick).
        for (uint32_t k = 0; k < 3; ++k)
        {
            if (!j.img[k].image) continue;
            const VkImage im = j.img[k].image;
            auto barrier = [&](uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                               VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
            {
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
                b.oldLayout = from; b.newLayout = to; b.image = im;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
                VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &d);
            };
            barrier(0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            if (j.levels > 1)
                barrier(1, j.levels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            int32_t mw = (int32_t)j.res, mh = (int32_t)j.res;
            for (uint32_t i = 1; i < j.levels; ++i)
            {
                const int32_t nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
                VkImageBlit bl{};
                bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1}; bl.srcOffsets[1] = {mw, mh, 1};
                bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};     bl.dstOffsets[1] = {nw, nh, 1};
                vkCmdBlitImage(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &bl, VK_FILTER_LINEAR);
                barrier(i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                mw = nw; mh = nh;
            }
            barrier(0, j.levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        r.job_done = true;
    }
    if (mTerrBakeQuery)
    {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mTerrBakeQuery, s * 2u + 1u);
        mTerrBakeTimed[s] = true;
    }
}
