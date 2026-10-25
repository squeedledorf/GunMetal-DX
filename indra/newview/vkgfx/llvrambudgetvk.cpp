/**
 * @file llvrambudgetvk.cpp
 * @brief One VRAM budget for the Vulkan renderer (#405 part A, #407).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#include "llvrambudgetvk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <cstdio>
#include <system_error>
#include <sstream>

namespace
{
unsigned long long vbMB(uint64_t b) { return (unsigned long long)((b + (1u << 19)) >> 20); }
}

std::string LLVramStatus::summaryText() const
{
    if (!valid) return "n/a (no Vulkan world renderer)";
    std::ostringstream o;
    o << "used " << vbMB(usage) << "/" << vbMB(budget) << " MB (cap " << (userCap ? std::to_string(vbMB(userCap)) : std::string("auto"))
      << ", OS " << vbMB(osBudget);
    if (osSmoothed && osSmoothed + (16u << 20) < osBudget) o << " (smoothed " << vbMB(osSmoothed) << ")";
    if (peers) o << ", shared with " << peers << " viewer" << (peers > 1 ? "s" : "") << " (" << vbMB(peerClaims) << " MB): " << vbMB(peerCap);
    o << ", reserve " << vbMB(reserve) << ") tracked " << vbMB(tracked) << " fixed " << vbMB(fixed)
      << " slack " << vbMB(slackDev) << " swap " << vbMB(swapchain) << " ovh " << vbMB(overhead)
      << " (unattributed " << vbMB(unattributed) << ")";
    if (scalable) o << " room " << vbMB(scalable);
    o << " | ladder " << rung << "/" << rungCount;
    if (step) o << " s" << step << (mode == 2 ? " reserve" : mode == 1 ? " soft" : "");
    if (hardSteps) o << " hard " << hardSteps;
    return o.str();
}

std::string LLVramStatus::categoryText() const
{
    if (!valid) return std::string();
    std::ostringstream o;
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        if (catDev[c] >= (1u << 20)) o << (o.tellp() > 0 ? " " : "") << vkMemCatName(c) << " " << vbMB(catDev[c]);
    return o.str();
}

std::string LLVramStatus::overheadText() const
{
    if (!valid) return std::string();
    std::ostringstream o;
    o << "ovh " << vbMB(overhead) << " MB = pad " << vbMB(pad) << " (" << allocCount << " allocs";
    if (allocLimit && allocLimit < 0xFFFFFFFFu) o << " of max " << allocLimit;
    o << " @" << (granularity >> 10) << "K"
      << (granularityMeasured ? "" : " assumed") << ") + driver " << vbMB(drvInternal);
    if (drvReport)
    {
        o << " [";
        bool any = false;
        for (uint32_t b = 1; b < VKDRV_COUNT; ++b)
            if (drv[b] >= (1u << 20)) { o << (any ? " " : "") << vkDrvBucketName(b) << " " << vbMB(drv[b]); any = true; }
        o << "]";
    }
    else o << " (no driver report)";
    o << " + untracked " << vbMB(untrackedDevMem) << " + ? " << vbMB(unattributed) << "; min " << vbMB(overheadMin);
    if (drvImport) o << "; import " << vbMB(drvImport);
    o << "; slack vram " << vbMB(slackDev) << " sys " << vbMB(slack > slackDev ? slack - slackDev : 0);
    return o.str();
}

std::string LLVramStatus::texPoolText() const
{
    if (!valid || !texPools) return std::string();
    std::ostringstream o;
    const double occ = texPoolBlockBytes ? 100.0 * (double)texPoolLive / (double)texPoolBlockBytes : 0.0;
    const uint64_t free_b = texPoolBlockBytes > texPoolRounded ? texPoolBlockBytes - texPoolRounded : 0;
    const uint64_t round_b = texPoolRounded > texPoolLive ? texPoolRounded - texPoolLive : 0;
    char rate[96];
    std::snprintf(rate, sizeof(rate), "%.1f/%.1f (sub %.0f/%.0f)", texAllocPerSec, texFreePerSec, texSubAllocPerSec,
                  texSubFreePerSec);
    o << "tex pools " << texPoolBlocks << " blocks " << vbMB(texPoolBlockBytes) << " MB, " << texPooledImages
      << " images " << vbMB(texPoolLive) << " MB live (" << (int)(occ + 0.5) << "% occupied), slack " << vbMB(texPoolSlack)
      << " (free " << vbMB(free_b) << " + rounding " << vbMB(round_b) << ")";
    // #587: the medium tier -- slab classes vs the TLSF fallback (occupancy, stranded =
    // free space in their blocks) and how many medium-range images each took (since start).
    uint64_t slabBlk = 0, slabLive = 0, slabFree = 0, fbBlk = 0, fbLive = 0, fbFree = 0;
    for (uint32_t i = 0; i < texPoolCount && i < VkDeviceContext::MemStats::kTexPoolMax; ++i)
    {
        const VkDeviceContext::MemStats::TexPool& p = texPool[i];
        const uint64_t fr = p.blockBytes > p.roundedBytes ? p.blockBytes - p.roundedBytes : 0;
        if (!p.classBytes) { fbBlk += p.blockBytes; fbLive += p.liveBytes; fbFree += fr; }
        else if (p.classBytes > texSmallMax) { slabBlk += p.blockBytes; slabLive += p.liveBytes; slabFree += fr; }
    }
    auto pct = [](uint64_t a, uint64_t b) { return b ? (int)(100.0 * (double)a / (double)b + 0.5) : 0; };
    o << "; medium classes " << vbMB(slabBlk) << " MB " << pct(slabLive, slabBlk) << "% stranded " << vbMB(slabFree)
      << ", fallback " << vbMB(fbBlk) << " MB " << pct(fbLive, fbBlk) << "% stranded " << vbMB(fbFree)
      << " (images hit/miss " << texClassHit << "/" << texClassMiss << ", max align " << texMedAlignMax << ")"
      << "; tex alloc/free per s " << rate;
    // #614: compaction -- what is left to reclaim, the empty blocks VMA keeps, and the work.
    char mbs[32];
    std::snprintf(mbs, sizeof(mbs), "%.1f", texCompactMBPerSec);
    o << "; compact " << (texCompactMode == 2u ? "PRESSURE" : texCompactMode == 1u ? "bg" : "off") << ": reclaimable "
      << vbMB(texPoolReclaim) << " MB, empty " << texPoolEmptyBlocks << " (" << vbMB(texPoolEmptyBytes) << " MB), moved "
      << vbMB(texCompactMoved) << " MB / " << texCompactMoves << " (" << mbs << " MB/s), blocks evacuated "
      << texCompactEvacuated << " released " << texCompactReleased << " (" << vbMB(texCompactReleasedBytes)
      << " MB), skipped busy " << texCompactSkipped << ", no room " << texCompactPlaceFail;
    return o.str();
}

std::string LLVramStatus::texPoolClassText() const
{
    if (!valid || !texPools) return std::string();
    std::ostringstream o;
    for (uint32_t i = 0; i < texPoolCount && i < VkDeviceContext::MemStats::kTexPoolMax; ++i)
    {
        const VkDeviceContext::MemStats::TexPool& p = texPool[i];
        if (!p.blocks) continue;   // (classes with no blocks: see the init line)
        if (!o.str().empty()) o << " ";
        // slab: <class>K <blocks>b <live>/<slots> <occupancy>% [f<stranded MB>]
        // fallback: fb <blocks>b <MB> <occupancy>% f<stranded MB>
        const uint64_t fr = p.blockBytes > p.roundedBytes ? p.blockBytes - p.roundedBytes : 0;
        if (p.classBytes)
        {
            o << ((p.classBytes + 1023) >> 10) << "K " << p.blocks << "b " << p.allocs << "/"
              << (uint64_t)p.blocks * p.slotsPerBlock;
        }
        else o << "fb " << p.blocks << "b " << vbMB(p.blockBytes);
        if (p.blockBytes) o << " " << (int)(100.0 * (double)p.liveBytes / (double)p.blockBytes + 0.5) << "%";
        if (fr >= (1ull << 20) || !p.classBytes) o << " f" << vbMB(fr);
    }
    return o.str();
}

std::string LLVramStatus::compactText() const
{
    std::string o = summaryText();
    if (!valid) return o;
    o += " | " + categoryText();
    if (!holding.empty()) o += " | holding: " + holding;
    if (!fitNotes.empty()) o += " | fit: " + fitNotes;
    o += " | " + overheadText();
    if (texPools) o += " | " + texPoolText();
    return o;
}

namespace
{
std::string vbText(uint64_t b)
{
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", (double)b / 1073741824.0);
    else std::snprintf(buf, sizeof(buf), "%llu MB", (unsigned long long)(b >> 20));
    return buf;
}
}

std::string LLVramStatus::readoutText() const
{
    // Short lines for the Advanced Graphics readout (~60 columns; the tooltip has the rest).
    if (!valid) return "Video memory: not measured yet";
    // "(capped)" only when "Maximum video memory" is what binds (#695: a 12 GB cap over a
    // 240 MB budget said "capped"); "(shared RAM)": the UMA budget counts system memory.
    const bool capped = userCap && userCap <= capacity && (!osBudget || userCap < osBudget);
    std::string tag;
    if (uma) tag = "shared RAM";
    if (capped) tag += tag.empty() ? "capped" : ", capped";
    std::string o = "In use " + vbText(usage) + " of a " + vbText(budget) + " budget" + (tag.empty() ? "" : " (" + tag + ")")
                  + ", reserve " + vbText(reserve);
    if (peers) o += ", shared with " + std::to_string(peers) + " other Slipstream" + (peers > 1 ? "s" : "");
    // Each group includes the driver page padding its allocations cause (#407: small
    // textures pad ~1 GB in a busy region, which used to land in "other").
    auto grp = [this](std::initializer_list<VkMemCat> cats)
    {
        uint64_t b = 0;
        for (VkMemCat c : cats) b += catDev[c] + catPad[c];
        return b;
    };
    // (+ the texture pools' slack: block space the pooled textures don't fill, #411)
    const uint64_t tex = grp({VKMEM_TEXTURE, VKMEM_TEXENC}) + texPoolSlack;
    const uint64_t geo = grp({VKMEM_GEOMETRY, VKMEM_SCENE, VKMEM_RIG, VKMEM_CULL});
    const uint64_t scr = grp({VKMEM_TARGET, VKMEM_UPSCALER, VKMEM_UI}) + swapchain;
    const uint64_t rt = grp({VKMEM_RT_AS, VKMEM_RT_AUX, VKMEM_RT_MASK, VKMEM_DENOISE});
    const uint64_t refl = grp({VKMEM_PROBE, VKMEM_MIRROR});
    const uint64_t named = tex + geo + scr + rt + refl;
    std::string pools;
    if (texPools && texPoolBlockBytes)
        pools = " (" + std::to_string((int)(100.0 * (double)texPoolLive / (double)texPoolBlockBytes + 0.5)) + "% packed)";
    o += "\nTextures " + vbText(tex) + pools + ", geometry " + vbText(geo) + ", screen " + vbText(scr) + ", RT " + vbText(rt)
       + ", reflections " + vbText(refl) + ", other " + vbText(usage > named ? usage - named : 0);
    // (#407: the measured fixed part, the room it leaves and the overhead breakdown are in
    // the tooltip -- compactText; this 52 px box has no room for a fourth line.)
    if (holding.empty()) o += "\nNothing reduced for memory.";
    else o += std::string(mode == 2 ? "\nReduced (memory nearly full): " : "\nReduced for memory: ") + holding;
    if (!fitNotes.empty()) o += "\nApplied up to what fits: " + fitNotes;
    return o;
}

// ---------------------------------------------------------------------------
// LLVramBudgetVK
// ---------------------------------------------------------------------------
namespace
{
// The ladder, lightest visual loss first (owner decision 2026-10-05, #405; mesh detail and
// far geometry added, far shadows ahead of the far geometry drop). Texture rungs
// map onto the renderer's #282 texture step: 1 over-demand, 2 off-screen, 3..8 demand
// classes < 128 .. < 4096 px, 9.. the global mip bias (texStepMin = sub).
const LLVramBudgetVK::Rung kRungTable[] = {
    {LLVramBudgetVK::STEP_TEX_OFFSCREEN, 1, "over-demand textures"},
    {LLVramBudgetVK::STEP_TEX_OFFSCREEN, 2, "off-screen textures"},
    {LLVramBudgetVK::STEP_PROBES, 1, "reflection probe resolution 1/2"},
    {LLVramBudgetVK::STEP_PROBES, 2, "reflection probe resolution 1/4"},
    {LLVramBudgetVK::STEP_PROBES, 3, "reflection probe count 1/2"},
    {LLVramBudgetVK::STEP_PROBES, 4, "reflection probe count 1/4"},
    {LLVramBudgetVK::STEP_TEX_FAR, 3, "far textures (< 128 px)"},
    {LLVramBudgetVK::STEP_TEX_FAR, 4, "far textures (< 256 px)"},
    {LLVramBudgetVK::STEP_TEX_FAR, 5, "far textures (< 512 px)"},
    // #613 rigged rungs, EARLY copy (cards above kRigSmallCardBytes; Caps::rigEarly): the
    // decimated steps (error-bounded, ~invisible) ahead of the mesh-detail rungs, creator
    // LODs and the smaller rigged budget right after them (where the old avatarLodScale
    // squeeze of the rigged budget began).
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_DECIMATE, "far avatars decimated"},
    // Mesh / terrain detail: sub = index into kLodScale (volume, avatar, terrain).
    {LLVramBudgetVK::STEP_GEOM_LOD, 1, "mesh detail reduced (3/4)"},
    {LLVramBudgetVK::STEP_GEOM_LOD, 2, "mesh detail reduced (1/2)"},
    {LLVramBudgetVK::STEP_GEOM_LOD, 3, "mesh detail reduced (1/3)"},
    {LLVramBudgetVK::STEP_GEOM_LOD, 4, "mesh detail reduced (1/4)"},
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_CREATOR, "far avatars at creator LODs"},
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_HALF, "avatar mesh memory 1/2"},
    {LLVramBudgetVK::STEP_FAR_SHADOWS, 1, "far shadows"},
    // Far geometry: sub = draw distance percent (floor kFarDrawFloorM).
    {LLVramBudgetVK::STEP_FAR_DRAW, 75, "draw distance 3/4"},
    {LLVramBudgetVK::STEP_FAR_DRAW, 50, "draw distance 1/2"},
    {LLVramBudgetVK::STEP_FAR_DRAW, 35, "draw distance 35%"},
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_BLAS_CAP, "RT geometry budget"},
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_RIGGED_OUT, "avatars in RT"},
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_MASK_3_4, "RT shadow resolution 3/4"},
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_MASK_HALF, "RT shadow resolution 1/2"},
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_SHORT_RANGE, "RT range 64 m"},
    {LLVramBudgetVK::STEP_MIRRORS, 1, "mirrors"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 6, "near textures (< 1024 px)"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 7, "near textures (< 2048 px)"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 8, "near textures (< 4096 px)"},
    // #613 rigged rungs, LATE copy (cards up to kRigSmallCardBytes; Caps::rigLate): an 8 GB
    // card gives up mid-texture detail (the demand classes, then half resolution) before far
    // bodies are decimated / capped -- blur reads better than bodies poking through clothes.
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_DECIMATE, "far avatars decimated"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 9, "texture mip bias 1"},
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_CREATOR, "far avatars at creator LODs"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 10, "texture mip bias 2"},
    {LLVramBudgetVK::STEP_RIGGED, LLVramBudgetVK::RIG_HALF, "avatar mesh memory 1/2"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 11, "texture mip bias 3"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 12, "texture mip bias 4"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 13, "texture mip bias 5"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 14, "texture mip bias 6"},
    {LLVramBudgetVK::STEP_TEX_NEAR, 15, "texture mip bias 7"},
    // #563: ray-traced probe capture -> raster faces costs GPU TIME (six raster scene
    // passes per capture), so it goes after every texture rung, not with the RT budget ones.
    {LLVramBudgetVK::STEP_RT, LLVramBudgetVK::RT_PROBE_TRACE_OFF, "ray-traced reflections"},
    {LLVramBudgetVK::STEP_PROJ_SHADOWS, 1, "projector shadows"},
    {LLVramBudgetVK::STEP_SUN_SHADOWS, 1, "sun shadows"},
    {LLVramBudgetVK::STEP_RENDER_SCALE, 85, "render scale 85%"},
    {LLVramBudgetVK::STEP_RENDER_SCALE, 70, "render scale 70%"},
    {LLVramBudgetVK::STEP_RENDER_SCALE, 50, "render scale 50%"},
};
constexpr uint32_t kRungN = (uint32_t)(sizeof(kRungTable) / sizeof(kRungTable[0]));
static_assert(sizeof(kRungTable) / sizeof(kRungTable[0]) <= 48, "LLVramBudgetVK::kMaxRungs");
// STEP_GEOM_LOD sub-levels: {volume (mesh / prim), avatar, terrain} LOD factor multipliers.
// The volume factor is the main lever (rigged attachments use it too); avatars and
// terrain follow a step behind so the nearest, most-looked-at geometry goes last.
const float kLodScale[5][3] = {
    {1.f, 1.f, 1.f}, {0.75f, 1.f, 1.f}, {0.5f, 0.75f, 0.75f}, {0.33f, 0.5f, 0.5f}, {0.25f, 0.35f, 0.35f}};
} // namespace

bool LLVramBudgetVK::scalableCategory(uint32_t cat)
{
    switch (cat)
    {
    case VKMEM_TEXTURE: case VKMEM_TEXENC:                       // texture steps 1 / 3 / 7
    case VKMEM_PROBE:                                            // step 2
    case VKMEM_SHADOW:                                           // steps 4 / 8 / 9
    case VKMEM_RT_AS: case VKMEM_RT_AUX: case VKMEM_RT_MASK:     // step 5
    case VKMEM_DENOISE:                                          // (#85: steps down to half extent itself)
    case VKMEM_MIRROR:                                           // step 6
    case VKMEM_GEOMETRY:                                         // mesh / terrain LOD rungs
    case VKMEM_RIG:                                              // rigged rungs (#613)
        return true;
    default:
        return false;
    }
}

uint64_t LLVramBudgetVK::fixedOf(const VkDeviceContext::MemStats& ms, uint64_t usage)
{
    uint64_t scal = ms.texPoolSlack;   // (#411: pool blocks empty as textures leave)
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        if (scalableCategory(c)) scal += ms.catDev[c] + ms.catPad[c];
    return usage > scal ? usage - scal : 0;
}

uint64_t LLVramBudgetVK::driverInternalBytes(const VkDeviceContext::MemStats& ms, uint32_t h)
{
    if (!ms.drvReport || h >= VK_MAX_MEMORY_HEAPS) return 0;
    const uint64_t* b = ms.drvHeap[h];
    uint64_t sum = std::max(b[VKDRV_DEVICE], b[VKDRV_PIPE]);
    for (uint32_t i = 0; i < VKDRV_COUNT; ++i)
        if (i != VKDRV_DEVMEM && i != VKDRV_SWAPCHAIN && i != VKDRV_DEVICE && i != VKDRV_PIPE) sum += b[i];
    return sum;
}

uint64_t LLVramBudgetVK::heapUsage(const VkDeviceContext::MemStats& ms, uint32_t h, uint64_t swapchain)
{
    if (h >= VK_MAX_MEMORY_HEAPS) return 0;
    if (ms.budgetExt) return std::max(ms.heapDriverUsage[h], ms.heapUsed[h]);
    // No VK_EXT_memory_budget: no process usage to read. Count what we know is in it
    // (the swapchain and padding live on the device-local heap).
    uint64_t u = ms.heapUsed[h];
    if (ms.heapDeviceLocal[h]) u += swapchain + ms.padBytes;
    return u + driverInternalBytes(ms, h);
}

uint64_t LLVramBudgetVK::budgetUsage(const VkDeviceContext::MemStats& ms, const VkDeviceContext::BudgetHeaps& bh,
                                     uint64_t swapchain)
{
    if (bh.heap >= VK_MAX_MEMORY_HEAPS) return 0;
    uint64_t u = heapUsage(ms, bh.heap, swapchain);
    if (bh.uma && bh.hostHeap < VK_MAX_MEMORY_HEAPS) u += heapUsage(ms, bh.hostHeap, 0);
    return u;
}

uint32_t LLVramBudgetVK::rungCount() { return kRungN; }
const LLVramBudgetVK::Rung& LLVramBudgetVK::rung(uint32_t i) { return kRungTable[i < kRungN ? i : kRungN - 1]; }

const char* LLVramBudgetVK::stepName(uint32_t step)
{
    static const char* kName[STEP_COUNT] = {"none", "off-screen textures", "reflection probes", "far textures",
                                            "mesh detail", "far shadows", "draw distance", "ray tracing budget",
                                            "mirrors", "near textures", "projector shadows", "sun shadows",
                                            "render scale", "far avatars"};
    return step < STEP_COUNT ? kName[step] : "?";
}

LLVramBudgetVK::Caps LLVramBudgetVK::capsAt(uint32_t engaged)
{
    Caps c;
    for (uint32_t i = 0; i < engaged && i < kRungN; ++i)
    {
        const Rung& r = kRungTable[i];
        switch (r.step)
        {
        case STEP_TEX_OFFSCREEN:
        case STEP_TEX_FAR:
        case STEP_TEX_NEAR: c.texStepMin = std::max<uint32_t>(c.texStepMin, r.sub); break;
        case STEP_PROBES:
            if (r.sub <= 2) c.probeResShift = std::max<uint32_t>(c.probeResShift, r.sub);
            else c.probeCountShift = std::max<uint32_t>(c.probeCountShift, r.sub - 2u);
            break;
        case STEP_GEOM_LOD:
        {
            const uint32_t k = std::min<uint32_t>(r.sub, 4u);
            c.meshLodScale = std::min(c.meshLodScale, kLodScale[k][0]);
            c.avatarLodScale = std::min(c.avatarLodScale, kLodScale[k][1]);
            c.terrainLodScale = std::min(c.terrainLodScale, kLodScale[k][2]);
            break;
        }
        case STEP_FAR_SHADOWS: c.farShadowsOff = true; break;
        case STEP_FAR_DRAW: c.drawDistScale = std::min(c.drawDistScale, (float)r.sub / 100.f); break;
        case STEP_RT: c.rtLevel = std::max<uint32_t>(c.rtLevel, r.sub); break;
        case STEP_MIRRORS: c.mirrorsOff = true; break;
        case STEP_PROJ_SHADOWS: c.projShadowsOff = true; break;
        case STEP_SUN_SHADOWS: c.sunShadowsOff = true; break;
        case STEP_RENDER_SCALE: c.renderScaleMax = std::min(c.renderScaleMax, (float)r.sub / 100.f); break;
        case STEP_RIGGED:
        {
            // (#613: the first copy in the table is the early one; see kRungTable)
            bool late = false;
            for (uint32_t j = 0; j < i; ++j)
                if (kRungTable[j].step == STEP_RIGGED && kRungTable[j].sub == r.sub) { late = true; break; }
            uint32_t& lv = late ? c.rigLate : c.rigEarly;
            lv = std::max<uint32_t>(lv, r.sub);
            break;
        }
        default: break;
        }
    }
    return c;
}

uint32_t LLVramBudgetVK::deepestStep() const
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < mRung && i < kRungN; ++i)
        if (mApplied[i]) s = kRungTable[i].step;
    return s;
}

std::string LLVramBudgetVK::holdingText(const std::function<std::string(uint32_t)>& label) const
{
    // The deepest APPLIED rung of each step, in ladder order.
    std::string o;
    for (uint32_t i = 0; i < mRung && i < kRungN; ++i)
    {
        if (!mApplied[i]) continue;
        bool deeper = false;
        for (uint32_t j = i + 1; j < mRung && j < kRungN; ++j)
            if (mApplied[j] && kRungTable[j].step == kRungTable[i].step) { deeper = true; break; }
        if (deeper) continue;
        if (!o.empty()) o += ", ";
        const std::string live = label ? label(i) : std::string();
        o += live.empty() ? std::string(kRungTable[i].what) : live;
    }
    return o;
}

bool LLVramBudgetVK::engageNext(double now, uint64_t usage, const std::function<bool(uint32_t)>& useful)
{
    uint32_t i = mRung;
    while (i < kRungN && !useful(i)) ++i;
    if (i >= kRungN)
    {
        mExhausted = true;
        return false;
    }
    for (uint32_t k = mRung; k < i; ++k) mApplied[k] = false;   // engaged, changes nothing now
    // #563: the previous applied rung's measurement window ends here -- whatever usage
    // falls from now on is (also) this rung's doing. Without this, a fast burst credited
    // every rung of it with the whole drop (6.6 GB for "texture mip bias 1" at PEAK
    // Lounge), and the restore check U + re-add < 85% could never pass again.
    for (int32_t j = (int32_t)mRung - 1; j >= 0; --j)
        if (mApplied[j])
        {
            if (!mFreedKnown[j])
            {
                mFreed[j] = mUsageAt[j] > usage ? mUsageAt[j] - usage : 0;
                mFreedKnown[j] = true;
            }
            break;
        }
    mApplied[i] = true;
    mUsageAt[i] = usage;
    mAppliedAt[i] = now;
    mFreedKnown[i] = false;
    mRung = i + 1;
    mCaps = capsAt(mRung);
    return true;
}

double LLVramBudgetVK::readdOf(uint32_t i, const std::function<uint64_t(uint32_t)>& readd) const
{
    // The renderer's own estimate when it has one (texture rungs: the measured bytes per
    // texture step), else the measured marginal drop. Unknown: 5% of the budget; measured:
    // at least 2% (usage that rose while the rung settled measures ~0 and would restore at
    // once); never more than a quarter of the budget (one rung never frees that much --
    // a larger number is a measurement that caught something else).
    const double B = (double)mBudget;
    double r = mFreedKnown[i] ? std::max((double)mFreed[i], 0.02 * B) : 0.05 * B;
    if (readd)
    {
        const uint64_t e = readd(i);
        if (e != ~0ull) r = std::max((double)e, 0.01 * B);
    }
    return std::min(r, 0.25 * B);
}

int LLVramBudgetVK::update(const Input& in, const std::function<bool(uint32_t)>& useful,
                           const std::function<uint64_t(uint32_t)>& readd)
{
    const double now = in.now;
    mNextCheck = now + (mMode == 2 ? 0.25 : 0.5);
    // ---- the budget ----
    uint64_t cap = in.osBudget;
    if (in.userCap) cap = cap ? std::min(cap, in.userCap) : in.userCap;
    if (in.debugCap) cap = cap ? std::min(cap, in.debugCap) : in.debugCap;
    if (in.shareCap) cap = cap ? std::min(cap, in.shareCap) : in.shareCap;   // #403
    if (!cap) return 0;   // nothing measured yet
    mCapacity = cap;
    mReserve = cap / 10u;
    mBudget = cap - mReserve;
    mUsage = in.usage;
    mOsBudget = in.osBudget;
    // ---- the measured fixed term (#407): exponential smoothing, fast up (~2 s) so new
    //      fixed costs are respected at once, slow down (~30 s) so a transient dip (a
    //      retired buffer, a recreate) does not hand room out that comes right back ----
    if (in.fixed)
    {
        if (!mFixedInit)
        {
            mFixedSm = (double)in.fixed;
            mFixedInit = true;
        }
        else
        {
            const double dt = std::clamp(now - mFixedAt, 0.0, 10.0);
            const double tau = (double)in.fixed > mFixedSm ? 2.0 : 30.0;
            mFixedSm += ((double)in.fixed - mFixedSm) * (1.0 - std::exp(-dt / tau));
        }
        mFixedAt = now;
    }
    const double B = (double)mBudget, U = (double)in.usage;

    // ---- mode ----
    if (!mFailInit) { mFailInit = true; mFailSeen = in.allocFailures; }
    if (in.allocFailures != mFailSeen)
    {
        // Failures recurring within 10 s (whatever their size: the driver keeps refusing
        // although the numbers say it fits) are answered like a request that does not fit.
        mFailRepeat = (now - mFailAt < 10.0) ? mFailRepeat + (in.allocFailures - mFailSeen) : 1u;
        mFailSeen = in.allocFailures;
        mFailUntil = now + 5.0;
        mFailAt = now;
        mFailBytes = in.failBytes;
        mNextDown = now;   // react at once
    }
    // #563: a failure is answered while the failed request would still not fit: usage +
    // its size + 3% of the capacity over the capacity (unknown size: one step). It used to
    // mean 5 s of fast mode whatever the usage -- one failed 2.4 GB rigged grow at 81%
    // stepped all 39 rungs in 4 s while usage fell to 40%.
    if (now < mFailUntil)
    {
        const double need = (double)in.usage + (double)mFailBytes + 0.03 * (double)mCapacity;
        mFailNeed = mFailRepeat >= 3u || (mFailBytes ? need > (double)mCapacity : (now - mFailAt < 0.3));
    }
    else mFailNeed = false;
    const bool failing = mFailNeed;
    // Crash risk only: usage within 3% of what the OS/driver grants this process. Being
    // over the user's "Maximum video memory" (or the debug cap) is not a crash risk and
    // must never trigger the hard step (it writes settings).
    if (in.osBudget && (double)in.usage > 0.97 * (double)in.osBudget)
    {
        if (mOverCapSince == 0.0) mOverCapSince = now;
    }
    else mOverCapSince = 0.0;
    const uint32_t prev_mode = mMode;
    // (a pending failure under the budget: soft mode at a faster pace, below)
    mMode = U > B ? 2u : ((failing || U > 0.95 * B) ? 1u : 0u);
    if (mMode != 0u && prev_mode == 0u)
    {
        // Pressure came back soon after a restore: wait longer before the next one (#268).
        if (now - mLastRestore < 60.0) mCalmNeed = std::min(mCalmNeed * 2.0, 160.0);
    }
    if (mMode == 0u && mCalmNeed > 10.0 && now - mLastRestore > 300.0) mCalmNeed = 10.0;

    // ---- engaged rungs that became relevant (e.g. mirrors / RT switched on while the
    //      ladder sat past them): their cap already applies, now they count as applied
    //      (readout, recovery order) ----
    for (uint32_t k = 0; k < mRung && k < kRungN; ++k)
        if (!mApplied[k] && useful(k))
        {
            mApplied[k] = true;
            mUsageAt[k] = in.usage;
            mAppliedAt[k] = now;
            mFreedKnown[k] = false;
        }

    // ---- what the last engaged rungs freed (measured once they settled; the geometry
    //      rungs free through the pressure GC + a repack, ~10 s later) ----
    for (uint32_t i = 0; i < mRung && i < kRungN; ++i)
        if (mApplied[i] && !mFreedKnown[i]
            && now - mAppliedAt[i] >= ((kRungTable[i].step == STEP_GEOM_LOD || kRungTable[i].step == STEP_FAR_DRAW
                                        || kRungTable[i].step == STEP_RIGGED) ? 12.0 : 3.0))
        {
            mFreed[i] = mUsageAt[i] > in.usage ? mUsageAt[i] - in.usage : 0;
            mFreedKnown[i] = true;
        }

    // ---- step down ----
    int moved = 0;
    if (mMode == 0u) mExhausted = false;
    // A geometry (or rigged, #613) rung frees through the pressure GC + a repack (~10-40 s). Without real
    // allocation failures, fast mode waits for the newest one to settle instead of
    // exhausting the whole ladder before it could take effect.
    bool settling = false;
    if (!failing)
        for (int32_t i = (int32_t)std::min<uint32_t>(mRung, kRungN) - 1; i >= 0; --i)
            if (mApplied[i])
            {
                const uint8_t st = kRungTable[i].step;
                // (#613: rigged rungs free through smaller demand + a rigged buffer re-size)
                settling = (st == STEP_GEOM_LOD || st == STEP_FAR_DRAW || st == STEP_RIGGED) && !mFreedKnown[i];
                break;
            }
    // #614: the compaction pre-step holds the step down (mNextDown is left alone, so the
    // ladder acts at once when it ends) -- never against a failure that still does not fit.
    const bool held = in.holdLadder && !failing;
    if (mMode == 2u && now >= mNextDown && !settling && !held)
    {
        for (int k = 0; k < 3 && engageNext(now, in.usage, useful); ++k) ++moved;
        mNextDown = now + 0.25;   // fast: no hysteresis wait inside the reserve
    }
    else if (mMode == 1u && now >= mNextDown && !settling && !held)
    {
        if (engageNext(now, in.usage, useful)) ++moved;
        // normal pacing: let the last move settle (a pending failure: 0.5 s, so the frees
        // of the step land in the usage before the next one is decided)
        mNextDown = now + (failing && U <= 0.95 * B ? 0.5 : 2.0);
    }
    if (mExhausted && mExhaustedAt > now) mExhaustedAt = now;
    if (!mExhausted) mExhaustedAt = 1.0e18;
    if (moved)
    {
        mCalmSince = 0.0;
        return moved;
    }

    // ---- recover (reverse order, one applied rung per calm period) ----
    int32_t last = (int32_t)mRung - 1;
    while (last >= 0 && !mApplied[last]) --last;
    mPendingReadd = last >= 0 ? (uint64_t)readdOf((uint32_t)last, readd) : 0;
    mPendingRung = last;
    if (mRung == 0 || mMode != 0u || U >= 0.80 * B)
    {
        mCalmSince = 0.0;
        return 0;
    }
    if (last < 0)
    {
        // Only "changes nothing" rungs engaged: drop them at once.
        const int was = (int)mRung;
        mRung = 0;
        mCaps = capsAt(0);
        return -was;
    }
    const double readd_last = (double)mPendingReadd;
    if (U + readd_last >= 0.85 * B)
    {
        mCalmSince = 0.0;
        return 0;
    }
    // Recovery pace follows the headroom: near the budget wait the full calm period and
    // restore one rung (no flapping); far below it (e.g. the cap was lifted) restore
    // quickly and several rungs at once, as long as their re-adds still fit well under.
    const double calm = U < 0.50 * B ? 1.0 : (U < 0.65 * B ? 3.0 : mCalmNeed);
    if (mCalmSince == 0.0) { mCalmSince = now; return 0; }
    if (now - mCalmSince < calm) return 0;
    const int was = (int)mRung;
    int32_t pop_to = last;
    if (U < 0.50 * B)
    {
        double proj = U + readd_last;
        for (int popped = 1; popped < 8; ++popped)
        {
            int32_t nxt = pop_to - 1;
            while (nxt >= 0 && !mApplied[nxt]) --nxt;
            if (nxt < 0) { pop_to = 0; break; }
            const double r = readdOf((uint32_t)nxt, readd);
            if (proj + r >= 0.60 * B) break;
            proj += r;
            pop_to = nxt;
        }
    }
    mRung = (uint32_t)pop_to;   // pops `pop_to` and everything above it (applied or not)
    mCaps = capsAt(mRung);
    mLastRestore = now;
    mCalmSince = 0.0;
    mExhausted = false;
    return (int)mRung - was;
}

bool LLVramBudgetVK::hardStepDue(double now) const
{
    if (!(mExhausted || mRung >= kRungN) || now < mNextHard) return false;
    // #563: only a failure that came AFTER the ladder ran out (every soft step taken and
    // allocations still fail), with usage + the failed request near the capacity -- a
    // real crash risk. The failure that started the descent does not count: it wrote RT
    // settings at 9.8 of 24 GB at PEAK Lounge (2026-10-08).
    const double exhausted_at = mExhaustedAt < 1.0e17 ? mExhaustedAt : now;
    // (Against the OS budget, never the user's "Maximum video memory": over that is a
    // preference, not a crash risk.)
    const double os = (double)(mOsBudget ? mOsBudget : mCapacity);
    const bool failing = mFailNeed && now < mFailUntil && mFailAt > exhausted_at
                      && (double)mUsage + (double)mFailBytes > 0.90 * os;
    const bool over_cap = mOverCapSince != 0.0 && now - mOverCapSince >= 5.0;
    return failing || over_cap;
}

void LLVramBudgetVK::noteHardStep(double now, bool done)
{
    mNextHard = now + 15.0;
    if (done) ++mHardSteps;
}

void LLVramBudgetVK::fastRecover(double now)
{
    // Focus regained (alt-tab): the OS budget came back; the next calm check may restore
    // without waiting out the whole calm period (one rung per check still).
    mCalmSince = now - mCalmNeed;
    mNextCheck = now;
}

// ---------------------------------------------------------------------------
// LLVramShareVK (#403; see the header)
// ---------------------------------------------------------------------------
namespace
{
constexpr double kShareIoPeriod = 1.0;      // publish + scan (s)
constexpr double kPeerStale = 30.0;         // a peer silent this long is not counted (s; a
                                            // hitching viewer can stall for several seconds)
constexpr double kPeerRemove = 120.0;       // ... and its file is removed (crashed viewer)
constexpr double kOsUpTau = 30.0;           // smoothed OS budget: rise time constant (s)
constexpr double kOsUpTauVolatile = 120.0;  // ... while the OS budget is volatile (#597 #599)
constexpr double kPeerUpTau = 20.0;         // peer share: rise time constant (s)
constexpr const char* kPeerMagic = "slipstream-vram";
constexpr int kPeerVersion = 1;

double wallSeconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A UTF-8 std::string as a path (std::filesystem::u8path is deprecated in C++20; a
// narrow std::string is the ANSI code page on Windows).
std::filesystem::path upath(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// Falls at once, rises with time constant tau.
void downFastUpSlow(double& sm, double& at, double target, double now, double tau)
{
    if (sm <= 0.0 || target <= sm) sm = target;
    else sm += (target - sm) * (1.0 - std::exp(-std::clamp(now - at, 0.0, 10.0) / tau));
    at = now;
}
} // namespace

void LLVramShareVK::init(const std::string& dir, const std::string& device_key, uint64_t heap_size, long pid)
{
    mInit = true;
    mDir = dir;
    mKey = device_key;
    mHeap = heap_size;
    mPid = pid;
    mFile.clear();
    if (mDir.empty() || mKey.empty()) { mDir.clear(); return; }
    std::error_code ec;
    std::filesystem::create_directories(upath(mDir), ec);
    if (ec) { mDir.clear(); return; }
    mFile = mDir + "/" + mKey + "-" + std::to_string(mPid) + ".vram";
}

uint64_t LLVramShareVK::shareOf(uint64_t pool, uint32_t n, uint64_t others)
{
    if (!pool || n < 2) return 0;
    const uint64_t fair = pool / n;
    const uint64_t take = others + pool / 20u;   // 5% left unclaimed between viewers
    const uint64_t borrow = pool > take ? pool - take : 0;
    return std::min(pool, std::max(fair, borrow));
}

void LLVramShareVK::publishAndScan(double wall, uint64_t usage, uint64_t claim)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    // ---- publish (temp + rename: a reader never sees half a line) ----
    if (!mFile.empty())
    {
        const std::string tmp = mFile + ".tmp";
        {
            std::ofstream f(upath(tmp), std::ios::out | std::ios::trunc);
            if (f)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s %d %ld %llu %llu %llu end\n", kPeerMagic, kPeerVersion, mPid,
                              (unsigned long long)usage, (unsigned long long)claim,
                              (unsigned long long)(wall * 1000.0));   // ms: integers only (locale-proof)
                f << buf;
            }
        }
        fs::rename(upath(tmp), upath(mFile), ec);   // (replaces the old file: POSIX and MSVC STL)
        if (ec) { std::error_code rec; fs::remove(upath(tmp), rec); }
    }
    // ---- scan the other viewers on this GPU ----
    uint32_t n = 0;
    uint64_t claims = 0;
    if (!mDir.empty())
    {
        const bool sweep = wall >= mNextSweep;
        if (sweep) mNextSweep = wall + 30.0;
        const std::string prefix = mKey + "-";
        std::error_code iec;
        fs::directory_iterator it(upath(mDir), iec), end;
        uint32_t seen = 0;
        for (; !iec && it != end && seen < 64; it.increment(iec), ++seen)
        {
            const fs::path p = it->path();
            const std::u8string name8 = p.filename().u8string();
            const std::string name(name8.begin(), name8.end());
            if (name.size() < 5 || name.compare(name.size() - 5, 5, ".vram") != 0) continue;
            std::string line;
            {
                std::ifstream f(p);
                if (!f || !std::getline(f, line)) continue;
            }
            char magic[32] = {}, tail[8] = {};
            int ver = 0;
            long pid = 0;
            unsigned long long u = 0, c = 0, t_ms = 0;
            const bool ok = std::sscanf(line.c_str(), "%31s %d %ld %llu %llu %llu %7s", magic, &ver, &pid, &u, &c, &t_ms, tail) == 7
                         && std::strcmp(magic, kPeerMagic) == 0 && ver == kPeerVersion && std::strcmp(tail, "end") == 0;
            const double age = ok ? std::fabs(wall - (double)t_ms / 1000.0) : 1.0e9;   // (clock stepped back: stale too)
            if (age > kPeerRemove)
            {
                if (sweep) { std::error_code rec; fs::remove(p, rec); }   // a crashed / killed viewer
                continue;
            }
            if (!ok || age > kPeerStale || pid == mPid) continue;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;   // another GPU
            ++n;
            claims += std::max(u, c);
        }
    }
    mPeers = n;
    mPeerClaims = claims;
}

uint64_t LLVramShareVK::update(const Input& in)
{
    if (!in.osBudget) return mCap;
    // ---- 1. the OS budget, smoothed ----
    // #597 #599 (AMD / Windows, another app taking and releasing VRAM: 4.6 .. 7.1 GB within
    // minutes): a drop of > 5% marks the budget VOLATILE for 5 min -- rises then take ~2 min
    // and a focus regain does not snap up -- so the ladder does not restore into room that
    // vanishes again (the sawtooth that ran into the wall twice). One smoothed value feeds
    // every decision (capacity = min(raw, this)).
    if (mOsSm > 0.0 && (double)in.osBudget < 0.95 * mOsSm) mOsDropAt = in.now;
    const bool vol = osVolatile(in.now);
    if (mOsSnap && vol && mOsSm > 0.0) mOsSnap = false;   // (volatile: no snap; rises slowly below)
    if (mOsSnap || mOsSm <= 0.0) { mOsSm = (double)in.osBudget; mOsAt = in.now; mOsSnap = false; }
    else downFastUpSlow(mOsSm, mOsAt, (double)in.osBudget, in.now, vol ? kOsUpTauVolatile : kOsUpTau);
    uint64_t cap = (uint64_t)mOsSm;

    // ---- 2. Slipstream peers on this GPU ----
    if (in.peersOn && mInit && !mDir.empty() && mHeap)
    {
        const uint64_t pool = mHeap - mHeap / 10u;   // 10% for the desktop / other apps
        // Our claim: what we use; under pressure at least our fair share (bounded by what
        // we could use at all) -- that is what makes a borrowing peer give room back.
        uint64_t claim = in.usage;
        if (in.pressured && mPeers)
        {
            uint64_t want = pool / (mPeers + 1u);
            if (in.ownCeil) want = std::min(want, in.ownCeil);
            claim = std::max(claim, want);
        }
        if (in.now >= mNextIo)
        {
            mNextIo = in.now + kShareIoPeriod;
            publishAndScan(wallSeconds(), in.usage, claim);
        }
        if (mPeers)
        {
            const uint64_t share = shareOf(pool, mPeers + 1u, mPeerClaims);
            downFastUpSlow(mPeerSm, mPeerAt, (double)share, in.now, kPeerUpTau);
            cap = std::min(cap, (uint64_t)mPeerSm);
        }
        else if (mPeerSm > 0.0)
        {
            // Alone again (the peer closed, or went silent -- a frozen viewer must not hand
            // its room out at once): the share rises slowly to the OS budget, then lets go.
            downFastUpSlow(mPeerSm, mPeerAt, mOsSm, in.now, kPeerUpTau);
            if (mPeerSm >= 0.98 * mOsSm) mPeerSm = 0.0;
            else cap = std::min(cap, (uint64_t)mPeerSm);
        }
    }
    else
    {
        mPeers = 0;
        mPeerClaims = 0;
        mPeerSm = 0.0;
        if (!in.peersOn && !mFile.empty() && in.now >= mNextIo)
        {
            // Switched off: stop being counted by the others.
            mNextIo = in.now + 5.0;
            std::error_code ec;
            std::filesystem::remove(upath(mFile), ec);
        }
    }
    mCap = cap;
    return mCap;
}

void LLVramShareVK::shutdown()
{
    if (!mFile.empty())
    {
        std::error_code ec;
        std::filesystem::remove(upath(mFile), ec);
    }
    *this = LLVramShareVK{};
}
