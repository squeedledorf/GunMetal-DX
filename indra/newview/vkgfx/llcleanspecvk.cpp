/**
 * @file llcleanspecvk.cpp
 * @brief SETTING SPECIALIZATION of the Vulkan world renderer (RenderVKSpecPipelines): the
 *        graphics settings the heavy lit shaders read become specialization constants, so
 *        the driver compiles each configuration without the paths it makes dead.
 *
 * Why: the lit FS / deferred lighting kernels carry every feature behind run-time tests of
 * the per-frame block (clean_frame_info.glsl ssr.*). The driver cannot drop a branch it
 * cannot prove dead, and NVIDIA's compiler sizes registers for the WORST path, then picks
 * an occupancy target and spills the rest (RenderVKPipelineStats on a 5090: forward lit FS
 * 168 regs + 128-144 B local, RT lit FS 255 + 224 B, deferred uber 128 + 96 B). A spec
 * constant per setting lets the pipeline for "cascades only, no SSR, no cel" lose the ray
 * queries, the SSR march and the toon terms outright (SPIR-V: the RT lit FS 166.6k -> 77.0k
 * words under the default settings).
 *
 * Design (see the header block at SpecId in llcleanrenderervk.h):
 *  - kSpecTable below is THE table: setting(s) -> constant id -> value, and its kind (EXACT:
 *    the value replaces the field; CAP: an upper bound, the run-time test stays).
 *  - CONFIGURATION KEY: the settings' spec values, 4 bits per SpecId. A SPEC SET = every
 *    pipeline of the specialized shaders for one key (lit FS both variants, deferred uber +
 *    split, water, particle FS + lighting compute), owned here; the generic pipelines stay
 *    owned by init / shutdown. The bind sites go through the accessors at the end.
 *  - specApply keeps the frame-info fields (and the CPU decisions behind them: cascades,
 *    ray-traced sun, projector mode, translucency layer, AO compute, SSR copy) on the BOUND
 *    key, so the folded code and the fields agree in every frame, whatever the settings
 *    already say. Generic (nothing bound): the live settings.
 *  - Builds run one KEY at a time in the background: LOADING (worker: the per-key
 *    VkPipelineCache from disk + the shader modules) -> COMPILING (main thread queues ONE
 *    async pipeline batch into that cache -- graphics and compute together, compiled wide by
 *    the shared scheduler: priority USER for a setting change, PREWARM class for the login key (after the login gate),
 *    PREWARM (one low-priority thread) for a speculative key) -> READY (bindable; bound at
 *    the next frame boundary when it is the wanted key) -> for a key that can trace, its
 *    RAY-QUERY slots as a second batch (lit FS ~16 s each cold vs ~8 s plain: the set binds
 *    on the plain slots, and the ray-query bind sites take the GENERIC ray-query pipeline
 *    until they land -- correct, the frame fields follow the key) -> its cache is saved
 *    (worker) and freed. A key that cannot trace (sun < 2, projectors < 2) never builds
 *    the ray-query slots.
 *  - Bounded: resident sets and per-key cache files are LRU'd within RenderVKSpecCacheMB
 *    each (never the bound / wanted / building key; plus a hard cap on resident sets);
 *    evicted pipelines go through the frame retire lists (destroyed once the frames that
 *    may bind them are done). VKSpec log lines carry live / cap / peak and per-key sizes.
 */

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_pipeline.h"
#include "vk_pipecache.h"
#include "vk_world_shaders_spv.h"
#include "../gmdxrezfx.h"   // <GMDX> rez effects: the lit FS wireframe twins

#include "llerror.h"
#include "llfile.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llnotificationsutil.h"
#  include "llcallbacklist.h"   // doAfterInterval: the notice leaves the render path
#  include "llagent.h"          // prewarm busy test: teleport state
#  include "llstartup.h"        // prewarm busy test: login
#  include "llenvironment.h"    // #818: the current sky's classic mode (SPEC_SKY)
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>

namespace
{
double specClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- THE TABLE: setting(s) -> specialization constant -> value ----
// Index = LLCleanRendererVK::SpecId; constId = clean_frame_info.glsl's constant_id. EXACT:
// the key's value replaces the frame-info field; CAP: the field is clamped to it (0 folds
// the feature away, higher keeps the run-time test). maxValue bounds the 4-bit key field.
struct SpecDesc
{
    const char* label;      // UI / log
    const char* settings;   // what the value is derived from (specKeyFromSettings)
    uint32_t    constId;
    bool        exact;
    uint32_t    maxValue;
};
const SpecDesc kSpecTable[LLCleanRendererVK::SPEC_COUNT] = {
    {"shadow filter",            "SlipstreamRenderShadowFilterQuality (0..3)",                    8,  true,  3},
    {"sun/moon shadows",         "RenderShadowDetail >= 1, RenderVKSunShadows (1 maps, 2 traced)", 9,  false, 2},
    {"projector shadows",        "RenderShadowDetail >= 2, RenderVKProjectorShadows (0..2)",      10, false, 2},
    {"translucent shadows",      "RenderVKTranslucentShadows",                                    11, false, 1},
    {"screen-space reflections", "RenderScreenSpaceReflections",                                  12, false, 1},
    {"mirrors",                  "RenderMirrors && RenderVKMirrors",                              13, false, 1},
    {"cel shading",              "RenderVKCel",                                                   14, false, 1},
    {"reflection probe limit",   "RenderVKProbeTopN (0..4)",                                      15, true,  4},
    {"ambient occlusion",        "RenderDeferredSSAO && RenderVKDepthPrepass",                    16, false, 1},
    {"AO mode",                  "RenderVKSpecularOcclusion (bit 0), RenderVKAOStandard (bit 1)", 17, true,  3},
    // (#818) Not a setting: the CURRENT SKY's classic (legacy, not auto-adjusted: LL's sRGB-space
    // lighting recombination) vs linear mode, as llappviewer fills atmos[8].w -- 0 linear, 1
    // classic, 2 = run time (RenderVKSpecClassicSky off: both paths compiled, the field tested).
    // A sky change re-keys like a setting change, except that the bound set is dropped AT ONCE
    // for the generic pipelines while the other mode's set is built (specFrameTick): the folded
    // code must never shade one sky with the other's math. Prewarm keeps the flip resident.
    {"sky mode",                 "the environment's sky (classic / linear), RenderVKSpecClassicSky", 18, true, 2},
    // (#818) the AO half-resolution upsample (clean_surface_light.glsl aoSample, ao_up.x): 0 full
    // resolution, 1 bilinear, 2 depth-aware -- RenderVKAOHalfRes, RenderVKAOUpsample; ensureAO
    // allocates the bound key's mode.
    {"AO upsample",              "RenderVKAOHalfRes, RenderVKAOUpsample",                         19, true,  2},
};
constexpr uint32_t kDebugConstId = 1;   // kDebugViews (clean_draw_common.glsl): FALSE in every set
constexpr size_t   kSpecMaxSets = 24;    // resident sets, whatever their size (sizes can be unknown)
constexpr uint64_t kSpecEstBytesPerPipe = 256ull << 10;   // until the set's cache size is known
constexpr double   kSpecNeighbourGapSec = 30.0;   // RenderVKSpecPrewarm 2: between neighbour builds
constexpr double   kSpecSlowFrameSec = 0.1;       // a frame this long makes prewarm wait (specPrewarmBusy)

uint32_t keyField(uint64_t key, uint32_t id) { return (uint32_t)((key >> (4u * id)) & 15u); }
uint64_t keyWith(uint64_t key, uint32_t id, uint32_t v)
{
    return (key & ~(15ull << (4u * id))) | ((uint64_t)(v & 15u) << (4u * id));
}
// #1075 LAYERED STEREO: the key's VIEW-COUNT field (N - 1; 0 at N = 1, so every desktop key and
// cache file is unchanged): a multiview set (viewMask, kViewCount) never shares a key -- or a
// per-key pipeline cache -- with a single-view one. Not a SpecId (no setting, no kSpecTable
// row): the session's view count, fixed at startup.
constexpr uint32_t kSpecViewsField = 15;   // bits 60-63 (the SpecIds use 0 .. 4 * SPEC_COUNT - 1)
static_assert(LLCleanRendererVK::SPEC_COUNT < kSpecViewsField, "the view-count field overlaps a SpecId");

// The UI snapshot (specStatusText): written by the render thread's tick, read by the prefs.
std::mutex  sStatusMutex;
std::string sStatusText;
bool        sStatusFallback = false;

// The modules a set needs (bit i = SpecMod i), created on the LOADING worker.
enum SpecMod : uint32_t { SM_VS, SM_RVS, SM_FRAG, SM_FRAG_BLEND, SM_FRAG_EZ, SM_FRAG_RT, SM_FRAG_BLEND_RT, SM_FRAG_RT_EZ,
                          SM_WATER, SM_WATER_CAP, SM_DEFER, SM_DSHADOW, SM_DREFLECT, SM_DCOMBINE, SM_PLIGHT, SM_PLIGHT_RT,
                          SM_DREFLECT_HALF, SM_DREFLECT_SHARP, SM_FRAG_CAP, SM_FRAG_CAP_RT, SM_DBASE, SM_DLOCAL,
                          SM_DSHADOW_CLS, SM_COUNT };   // (SM_DSHADOW_CLS: #831 shadow + tile classify)
struct SpecModSrc { const uint32_t* code; size_t bytes; const char* name; };
SpecModSrc specModSrc(uint32_t m, bool g11, bool clk)
{
    if (clk)   // RenderVKShaderClockLighting: the lighting kernels' instrumented twins
    {
        const int pass = m == SM_DEFER ? 0 : m == SM_DSHADOW ? 1 : m == SM_DREFLECT ? 2 : m == SM_DCOMBINE ? 3
                       : m == SM_DREFLECT_HALF ? 4 : m == SM_DREFLECT_SHARP ? 5 : m == SM_DBASE ? 7
                       : m == SM_DLOCAL ? 8 : m == SM_DSHADOW_CLS ? 10 : -1;
        if (pass >= 0)
        {
            const VKDeferSpv d = vkDeferLightingSpv((uint32_t)pass, g11, true);
            return {d.code, d.bytes, d.name};
        }
    }
    switch (m)
    {
    case SM_VS:            return {kWorldVertSpv, kWorldVertSpv_bytes, "Vert"};
    case SM_RVS:           return {kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes, "RiggedVert"};
    case SM_FRAG:          return {kWorldFragSpv, kWorldFragSpv_bytes, "Frag"};
    case SM_FRAG_BLEND:    return {kWorldFragBlendSpv, kWorldFragBlendSpv_bytes, "FragBlend"};
    case SM_FRAG_EZ:       return {kWorldFragEzSpv, kWorldFragEzSpv_bytes, "FragEz"};
    case SM_FRAG_RT:       return {kWorldFragRtSpv, kWorldFragRtSpv_bytes, "FragRt"};
    case SM_FRAG_BLEND_RT: return {kWorldFragBlendRtSpv, kWorldFragBlendRtSpv_bytes, "FragBlendRt"};
    case SM_FRAG_RT_EZ:    return {kWorldFragRtEzSpv, kWorldFragRtEzSpv_bytes, "FragRtEz"};
    case SM_WATER:         return {kWorldWaterFragSpv, kWorldWaterFragSpv_bytes, "WaterFrag"};
    case SM_WATER_CAP:     return {kWorldWaterCapFragSpv, kWorldWaterCapFragSpv_bytes, "WaterCapFrag"};
    case SM_DEFER:         return g11 ? SpecModSrc{kWorldDeferredLightG11Spv, kWorldDeferredLightG11Spv_bytes, "DeferredLightG11"}
                                      : SpecModSrc{kWorldDeferredLightSpv, kWorldDeferredLightSpv_bytes, "DeferredLight"};
    case SM_DSHADOW:       return {kWorldDeferShadowSpv, kWorldDeferShadowSpv_bytes, "DeferShadow"};
    case SM_DREFLECT:      return {kWorldDeferReflectSpv, kWorldDeferReflectSpv_bytes, "DeferReflect"};
    case SM_DCOMBINE:      return g11 ? SpecModSrc{kWorldDeferCombineG11Spv, kWorldDeferCombineG11Spv_bytes, "DeferCombineG11"}
                                      : SpecModSrc{kWorldDeferCombineSpv, kWorldDeferCombineSpv_bytes, "DeferCombine"};
    case SM_PLIGHT:        return {kWorldPartLightSpv, kWorldPartLightSpv_bytes, "PartLight"};
    case SM_PLIGHT_RT:     return {kWorldPartLightRtSpv, kWorldPartLightRtSpv_bytes, "PartLightRt"};
    case SM_DREFLECT_HALF: return {kWorldDeferReflectHalfSpv, kWorldDeferReflectHalfSpv_bytes, "DeferReflectHalf"};
    case SM_DREFLECT_SHARP:return {kWorldDeferReflectSharpSpv, kWorldDeferReflectSharpSpv_bytes, "DeferReflectSharp"};
    // (#829) the reflection-probe capture FS, plain / ray-query (LIT_CAP_*, frag kind LITF_CAP)
    case SM_FRAG_CAP:      return {kWorldFragCapSpv, kWorldFragCapSpv_bytes, "FragCap"};
    case SM_FRAG_CAP_RT:   return {kWorldFragCapRtSpv, kWorldFragCapRtSpv_bytes, "FragCapRt"};
    case SM_DBASE:         return g11 ? SpecModSrc{kWorldDeferBaseG11Spv, kWorldDeferBaseG11Spv_bytes, "DeferBaseG11"}
                                      : SpecModSrc{kWorldDeferBaseSpv, kWorldDeferBaseSpv_bytes, "DeferBase"};
    case SM_DLOCAL:        return g11 ? SpecModSrc{kWorldDeferLocalG11Spv, kWorldDeferLocalG11Spv_bytes, "DeferLocalG11"}
                                      : SpecModSrc{kWorldDeferLocalSpv, kWorldDeferLocalSpv_bytes, "DeferLocal"};
    case SM_DSHADOW_CLS:   return {kWorldDeferShadowClassifySpv, kWorldDeferShadowClassifySpv_bytes, "DeferShadowClassify"};
    default:               return {nullptr, 0, nullptr};
    }
}

VkShaderModule specModule(VkDevice dev, const SpecModSrc& src_in)
{
    SpecModSrc src = src_in;
    GMDXRezFx::wireModule(src.name, src.code, src.bytes);   // <GMDX> a lit FS's Tron-wireframe twin
    if (!src.code) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = src.bytes;
    ci.pCode = src.code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, src.code, src.bytes, src.name);   // (internally locked: any thread)
    return m;
}

std::string mb(uint64_t bytes)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return b;
}
}   // namespace

// The deferred lighting kernel's SPIR-V: pass = CLEAN_DEFER_PASS (0 uber, 1 shadow, 2 reflect,
// 3 combine, 4 reflect half, 5 reflect sharp, 6 light grid; #827 feature split: 7 base, 8 local,
// 9 tile classify; #831: 10 shadow + tile classify), g11 = the compact glow output (uber /
// combine / base / local), clock = the RenderVKShaderClockLighting twin (clean_shader_clock.glsl;
// only on a device with VK_KHR_shader_clock enabled). One table for every place that builds a lighting pipeline.
VKDeferSpv vkDeferLightingSpv(uint32_t pass, bool g11, bool clock)
{
#define DSPV(sym) VKDeferSpv{sym, sym##_bytes, #sym}
    switch (pass)
    {
    case 0:  return clock ? (g11 ? DSPV(kWorldDeferredLightG11ClockSpv) : DSPV(kWorldDeferredLightClockSpv))
                          : (g11 ? DSPV(kWorldDeferredLightG11Spv) : DSPV(kWorldDeferredLightSpv));
    case 1:  return clock ? DSPV(kWorldDeferShadowClockSpv) : DSPV(kWorldDeferShadowSpv);
    case 2:  return clock ? DSPV(kWorldDeferReflectClockSpv) : DSPV(kWorldDeferReflectSpv);
    case 3:  return clock ? (g11 ? DSPV(kWorldDeferCombineG11ClockSpv) : DSPV(kWorldDeferCombineClockSpv))
                          : (g11 ? DSPV(kWorldDeferCombineG11Spv) : DSPV(kWorldDeferCombineSpv));
    case 4:  return clock ? DSPV(kWorldDeferReflectHalfClockSpv) : DSPV(kWorldDeferReflectHalfSpv);
    case 5:  return clock ? DSPV(kWorldDeferReflectSharpClockSpv) : DSPV(kWorldDeferReflectSharpSpv);
    case 6:  return clock ? DSPV(kWorldLightGridClockSpv) : DSPV(kWorldLightGridSpv);
    case 7:  return clock ? (g11 ? DSPV(kWorldDeferBaseG11ClockSpv) : DSPV(kWorldDeferBaseClockSpv))
                          : (g11 ? DSPV(kWorldDeferBaseG11Spv) : DSPV(kWorldDeferBaseSpv));
    case 8:  return clock ? (g11 ? DSPV(kWorldDeferLocalG11ClockSpv) : DSPV(kWorldDeferLocalClockSpv))
                          : (g11 ? DSPV(kWorldDeferLocalG11Spv) : DSPV(kWorldDeferLocalSpv));
    case 9:  return clock ? DSPV(kWorldDeferTileClassifyClockSpv) : DSPV(kWorldDeferTileClassifySpv);
    case 10: return clock ? DSPV(kWorldDeferShadowClassifyClockSpv) : DSPV(kWorldDeferShadowClassifySpv);   // #831
    default: return VKDeferSpv{nullptr, 0, nullptr};
    }
#undef DSPV
}

// ---- one configuration's pipelines ----
struct LLCleanRendererVK::SpecSet
{
    enum Phase : uint8_t { LOADING, COMPILING, READY };
    uint64_t key = 0;
    Phase    phase = LOADING;
    const char* why = "";
    bool     rt = false;                       // the ray-query slots are built (the key can trace)
    // Specialization data: kDebugViews (FALSE) + one int per SpecId. The set lives behind a
    // unique_ptr, so `info` may point into it for the compute worker's whole run.
    int32_t  data[1 + SPEC_COUNT] = {};
    VkSpecializationMapEntry entries[1 + SPEC_COUNT] = {};
    VkSpecializationInfo info{};
    // #1075 LAYERED STEREO: the LAYERED lighting kernels' info -- `info` + kViewCount (constant 20)
    // = the key's view count. A one-view key: &info itself (layeredInfo). Graphics pipelines never
    // take it (GraphicsPipelineDesc::views merges their kViewCount).
    int32_t  vdata[2 + SPEC_COUNT] = {};
    VkSpecializationMapEntry ventries[2 + SPEC_COUNT] = {};
    VkSpecializationInfo vinfo{};
    bool     layered = false;
    const VkSpecializationInfo* layeredInfo() const { return layered ? &vinfo : &info; }
    VkPipelineCache cache = VK_NULL_HANDLE;   // per key, seeded from vk_spec_<key>.bin; freed once saved
    size_t   diskBytes = 0;                    // what the file held (0 = cold build)
    uint64_t bytes = 0;                        // the key's cache size once saved (its footprint)
    VkPipeline lit[2][LIT_COUNT] = {};
    VkPipeline defer = VK_NULL_HANDLE;
    VkPipeline lprobe[7] = {};                 // (RenderVKLightRegProbe: measurement only, never bound)
    VkPipeline split[DSP_COUNT] = {};
    VkPipeline feat[DFP_COUNT] = {};           // #827 BASE / LOCAL, #831 SHADOW_CLS (DFP_CLASSIFY stays generic: unused here)
    VkPipeline water = VK_NULL_HANDLE, waterCap = VK_NULL_HANDLE;
    VkPipeline partLight = VK_NULL_HANDLE, partLightRt = VK_NULL_HANDLE;
    VkPipeline part[PART_PIPE_LIT][NUM_PART_PAIRS] = {};   // MAIN / MAIN_RT / MIRROR (per-pixel lit FS)
    unsigned pipes = 0;
    // Background work.
    uint32_t modMask = 0;
    bool     g11 = false;
    struct Loaded { VkPipelineCache cache = VK_NULL_HANDLE; size_t diskBytes = 0; VkShaderModule mods[SM_COUNT] = {}; };
    std::future<Loaded> loadJob;
    VkShaderModule mods[SM_COUNT] = {};
    ::PipelineJob* gfxJob = nullptr;           // phase 1: plain lit + water + particles + compute
    // Phase 2 (rt): the ray-query slots, queued once the set is READY and the generic
    // ray-query variant exists (no generic slot: no twin). The workers write the STAGE
    // arrays; they are installed when the job is finished (the bind sites never see a
    // handle a worker is writing).
    ::PipelineJob* rtJob = nullptr;
    bool     rtQueued = false;
    VkPipeline litRtStage[LIT_COUNT] = {};
    VkPipeline partRtStage[NUM_PART_PAIRS] = {};
    VkPipeline partLightRtStage = VK_NULL_HANDLE;
    uint8_t  prio = VKPIPE_PRIO_LOGIN;         // vk_pipeline.h PipelineJobOpts (both phases)
    unsigned maxThreads = 0;
    bool     prewarm = false;
    std::future<uint64_t> saveJob;
    double   t0 = 0.0, tReady = 0.0, lastUse = 0.0;
    bool     busy() const { return saveJob.valid() || rtJob != nullptr || (rt && !rtQueued); }

    uint64_t footprint() const { return bytes ? bytes : (uint64_t)pipes * kSpecEstBytesPerPipe; }
    template <class F> void eachPipeline(F&& f)
    {
        for (auto& row : lit) for (VkPipeline& p : row) f(p);
        f(defer);
        for (VkPipeline& p : lprobe) f(p);
        for (VkPipeline& p : split) f(p);
        for (VkPipeline& p : feat) f(p);
        f(water); f(waterCap); f(partLight); f(partLightRt);
        for (auto& row : part) for (VkPipeline& p : row) f(p);
    }
};

void LLCleanRendererVK::SpecSetDeleter::operator()(SpecSet* s) const { delete s; }

// ---- the key ----
uint64_t LLCleanRendererVK::specKeyFromSettings() const
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKSpecPipelines", true);
    if (!s_on) return kSpecNone;
    static LLCachedControl<U32>  s_filt(gSavedSettings, "SlipstreamRenderShadowFilterQuality", 1);
    static LLCachedControl<S32>  s_detail(gSavedSettings, "RenderShadowDetail", 2);
    static LLCachedControl<U32>  s_sun(gSavedSettings, "RenderVKSunShadows", 1);
    static LLCachedControl<U32>  s_proj(gSavedSettings, "RenderVKProjectorShadows", 1);
    static LLCachedControl<bool> s_trans(gSavedSettings, "RenderVKTranslucentShadows", true);
    static LLCachedControl<bool> s_ssr(gSavedSettings, "RenderScreenSpaceReflections", false);
    static LLCachedControl<bool> s_mirrors(gSavedSettings, "RenderMirrors", false);
    static LLCachedControl<bool> s_vk_mirrors(gSavedSettings, "RenderVKMirrors", true);
    static LLCachedControl<bool> s_cel(gSavedSettings, "RenderVKCel", false);
    static LLCachedControl<U32>  s_topn(gSavedSettings, "RenderVKProbeTopN", 4);
    static LLCachedControl<bool> s_ao(gSavedSettings, "RenderDeferredSSAO", true);
    static LLCachedControl<bool> s_pre(gSavedSettings, "RenderVKDepthPrepass", true);
    static LLCachedControl<bool> s_spec_occ(gSavedSettings, "RenderVKSpecularOcclusion", true);
    static LLCachedControl<bool> s_ao_std(gSavedSettings, "RenderVKAOStandard", true);
    static LLCachedControl<bool> s_spec_sky(gSavedSettings, "RenderVKSpecClassicSky", true);   // #818
    static LLCachedControl<bool> s_sky_auto(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    static LLCachedControl<bool> s_ao_half(gSavedSettings, "RenderVKAOHalfRes", true);
    static LLCachedControl<U32>  s_ao_up(gSavedSettings, "RenderVKAOUpsample", 1U);
    const S32 detail = s_detail;
    // Without the RT foundation a "ray traced" setting falls back to maps (sunRtWanted /
    // selectProjShadows): the key folds the traced paths then.
    const bool rtOk = mRT != nullptr;
    uint32_t v[SPEC_COUNT];
    v[SPEC_FILTER] = std::min<U32>((U32)s_filt, 3u);
    v[SPEC_SUN]    = detail >= 1 ? (((U32)s_sun == 2u && rtOk) ? 2u : 1u) : 0u;
    v[SPEC_PROJ]   = detail >= 2 ? std::min<U32>((U32)s_proj, 2u) : 0u;
    if (v[SPEC_PROJ] == 2u && !rtOk) v[SPEC_PROJ] = 1u;
    v[SPEC_TRANS]  = s_trans ? 1u : 0u;
    v[SPEC_SSR]    = s_ssr ? 1u : 0u;
    v[SPEC_MIRROR] = (s_mirrors && s_vk_mirrors) ? 1u : 0u;
    v[SPEC_CEL]    = s_cel ? 1u : 0u;
    v[SPEC_TOPN]   = std::min<U32>((U32)s_topn, 4u);
    v[SPEC_AO]     = (s_ao && s_pre) ? 1u : 0u;
    v[SPEC_AOMODE] = (s_spec_occ ? 1u : 0u) | (s_ao_std ? 2u : 0u);
    // (#818) The sky mode exactly as llappviewer.cpp fills atmos[8].w for this frame (the current
    // sky can auto-adjust -- a legacy sky -- and RenderSkyAutoAdjustLegacy is off; no sky: linear).
    {
        bool classic = false;
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (sky) classic = sky->canAutoAdjust() && !(bool)s_sky_auto;
        v[SPEC_SKY] = s_spec_sky ? (classic ? 1u : 0u) : 2u;
    }
    // (#818) ensureAO's mode: half resolution -> 1 bilinear / 2 depth-aware (the meta target).
    v[SPEC_AOUP]   = s_ao_half ? ((U32)s_ao_up >= 1u ? 2u : 1u) : 0u;
    // Values the rest of the key makes dead are normalized (fewer distinct keys, same code):
    // no AO -> the AO mode and upsample are never read; no sun and no projector shadows ->
    // neither the filter nor the translucency layers are.
    if (!v[SPEC_AO]) { v[SPEC_AOMODE] = 0u; v[SPEC_AOUP] = 0u; }
    if (!v[SPEC_SUN] && !v[SPEC_PROJ]) { v[SPEC_FILTER] = 0u; v[SPEC_TRANS] = 0u; }
    uint64_t key = 0;
    for (uint32_t i = 0; i < SPEC_COUNT; ++i) key = keyWith(key, i, std::min(v[i], kSpecTable[i].maxValue));
    if (mViewCount > 1u) key = keyWith(key, kSpecViewsField, mViewCount - 1u);   // #1075 (N = 1: no change)
    return key;
#else
    return kSpecNone;
#endif
}

uint32_t LLCleanRendererVK::specApply(SpecId id, uint32_t live) const
{
    if (!mSpecLive || id >= SPEC_COUNT) return live;   // generic pipelines: the live values
    const uint32_t v = keyField(mSpecLive->key, id);
    return kSpecTable[id].exact ? v : std::min(live, v);
}

std::string LLCleanRendererVK::specKeyText(uint64_t key) const
{
    if (key == kSpecNone) return "generic";
    char b[128];
    std::snprintf(b, sizeof(b), "%05" PRIx64 " [filt %u sun %u proj %u trans %u ssr %u mirror %u cel %u topN %u ao %u/%u/%u sky %s]",
                  key, keyField(key, SPEC_FILTER), keyField(key, SPEC_SUN), keyField(key, SPEC_PROJ),
                  keyField(key, SPEC_TRANS), keyField(key, SPEC_SSR), keyField(key, SPEC_MIRROR), keyField(key, SPEC_CEL),
                  keyField(key, SPEC_TOPN), keyField(key, SPEC_AO), keyField(key, SPEC_AOMODE), keyField(key, SPEC_AOUP),
                  keyField(key, SPEC_SKY) == 0u ? "linear" : keyField(key, SPEC_SKY) == 1u ? "classic" : "runtime");
    return b;
}

std::string LLCleanRendererVK::specCachePath(uint64_t key) const
{
    if (!mCtx || mCtx->pipelineCacheFile().empty()) return std::string();
    const std::string& f = mCtx->pipelineCacheFile();
    const size_t slash = f.find_last_of("/\\");
    char name[48];
    std::snprintf(name, sizeof(name), "vk_spec_%05" PRIx64 ".bin", key);
    return (slash == std::string::npos ? std::string() : f.substr(0, slash + 1)) + name;
}

// ---- accessors (bind sites) ----
VkPipeline LLCleanRendererVK::litVar(int v, int i) const
{
    if (v < 0 || v > 1 || i < 0 || i >= LIT_COUNT) return VK_NULL_HANDLE;
    if (mSpecLive && mSpecLive->lit[v][i]) return mSpecLive->lit[v][i];
    return mLitVar[v][i];
}
VkPipeline LLCleanRendererVK::deferPipe() const
{
    return (mSpecLive && mSpecLive->defer) ? mSpecLive->defer : mDeferPipe;
}
VkPipeline LLCleanRendererVK::deferSplitPipe(uint32_t i) const
{
    if (i >= DSP_COUNT) return VK_NULL_HANDLE;
    return (mSpecLive && mSpecLive->split[i]) ? mSpecLive->split[i] : mDeferSplitPipe[i];
}
VkPipeline LLCleanRendererVK::deferFeatPipe(uint32_t i) const
{
    if (i >= DFP_COUNT) return VK_NULL_HANDLE;
    return (mSpecLive && mSpecLive->feat[i]) ? mSpecLive->feat[i] : mDeferFeatPipe[i];
}
VkPipeline LLCleanRendererVK::waterPipe() const
{
    return (mSpecLive && mSpecLive->water) ? mSpecLive->water : mWaterPipe;
}
VkPipeline LLCleanRendererVK::waterCapPipe() const
{
    return (mSpecLive && mSpecLive->waterCap) ? mSpecLive->waterCap : mWaterCapPipe;
}
VkPipeline LLCleanRendererVK::partLightPipe(bool rt) const
{
    if (rt) return (mSpecLive && mSpecLive->partLightRt) ? mSpecLive->partLightRt : mPartLightPipeRt;
    return (mSpecLive && mSpecLive->partLight) ? mSpecLive->partLight : mPartLightPipe;
}
VkPipeline LLCleanRendererVK::specPartPipe(uint32_t variant, uint32_t pair) const
{
    if (!mSpecLive || variant >= PART_PIPE_LIT || pair >= NUM_PART_PAIRS) return VK_NULL_HANDLE;
    return mSpecLive->part[variant][pair];
}

// ---- build ----
// LOADING: the per-key cache (disk) + the modules, on a worker; the next ticks go on.
// Two LANES, one key each: mSpecBuild (the login key / a setting change: the user is
// waiting) and mSpecPrewarmBuild (speculative, one low-priority thread) -- a setting change
// never waits behind a slow prewarm.
bool LLCleanRendererVK::specStartBuild(uint64_t key, const char* why)
{
    const bool prewarmLane = std::strncmp(why, "prewarm", 7) == 0;
    SpecSet*& lane = prewarmLane ? mSpecPrewarmBuild : mSpecBuild;
    if (!mCtx || lane || key == kSpecNone) return false;
    std::unique_ptr<SpecSet, SpecSetDeleter> set(new SpecSet());
    SpecSet& s = *set;
    s.key = key;
    s.why = why;
    s.t0 = specClock();
    s.lastUse = s.t0;
    s.entries[0] = {kDebugConstId, 0u, sizeof(int32_t)};
    s.data[0] = VK_FALSE;   // kDebugViews (VkBool32 = 4 bytes, as litDebugSpec)
    for (uint32_t i = 0; i < SPEC_COUNT; ++i)
    {
        s.entries[1 + i] = {kSpecTable[i].constId, (uint32_t)((1 + i) * sizeof(int32_t)), sizeof(int32_t)};
        s.data[1 + i] = (int32_t)keyField(key, i);
    }
    // (#818) the sky mode's 2 = run time: the shader's generic -1 (both paths compiled)
    if (keyField(key, SPEC_SKY) >= 2u) s.data[1 + SPEC_SKY] = -1;
    s.info.mapEntryCount = 1 + SPEC_COUNT;
    s.info.pMapEntries = s.entries;
    s.info.dataSize = sizeof(s.data);
    s.info.pData = s.data;
    // #1075: a stereo key's layered kernels also take kViewCount (the key's view-count field = N - 1)
    {
        const uint32_t views = keyField(key, kSpecViewsField) + 1u;
        s.layered = views > 1u;
        for (uint32_t i = 0; i < 1 + SPEC_COUNT; ++i) { s.ventries[i] = s.entries[i]; s.vdata[i] = s.data[i]; }
        s.ventries[1 + SPEC_COUNT] = {GraphicsPipelineDesc::kViewCountConstId, (uint32_t)((1 + SPEC_COUNT) * sizeof(int32_t)),
                                      sizeof(int32_t)};
        s.vdata[1 + SPEC_COUNT] = (int32_t)views;
        s.vinfo.mapEntryCount = 2 + SPEC_COUNT;
        s.vinfo.pMapEntries = s.ventries;
        s.vinfo.dataSize = sizeof(s.vdata);
        s.vinfo.pData = s.vdata;
    }
    // The ray-query slots only where the key can trace (cascades-only sun and mapped
    // projectors fold every ray query: selectLitVariant never picks that variant then, the
    // particle RT pipe / RT lighting compute are picked only with a traced sun or projector
    // -- a key that cannot trace binds the generic ones there, correct either way). Neither
    // their modules nor their pipelines are built for such a key. Built as phase 2, once the
    // generic ray-query variant exists (specPollBuild).
    s.rt = (keyField(key, SPEC_SUN) >= 2u || keyField(key, SPEC_PROJ) >= 2u) && mRT && mDrawLayoutRt;
    s.g11 = mDeferG11;
    // Scheduling (vk_pipeline.h PipelineJobOpts): a key the user is waiting for (USER) >
    // the login shader gate's batches (LOGIN: the world can draw without this set, the
    // generic pipelines do) > the login key and a speculative prewarm (PREWARM class; the
    // login key still compiles wide, a prewarm on one low-priority thread).
    s.prewarm = std::strncmp(why, "prewarm", 7) == 0;
    s.prio = (s.prewarm || std::strcmp(why, "login") == 0) ? VKPIPE_PRIO_PREWARM : VKPIPE_PRIO_USER;
    s.maxThreads = 1;
    if (!s.prewarm)
    {
        U32 threads = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_threads(gSavedSettings, "RenderVKSpecBuildThreads", 0);
        threads = s_threads;
#endif
        s.maxThreads = threads;   // (0 = the compile thread count)
    }
    uint32_t m = (1u << SM_VS) | (1u << SM_RVS) | (1u << SM_FRAG) | (1u << SM_FRAG_BLEND) | (1u << SM_FRAG_EZ) | (1u << SM_FRAG_CAP);
    if (s.rt) m |= (1u << SM_FRAG_RT) | (1u << SM_FRAG_BLEND_RT) | (1u << SM_FRAG_RT_EZ) | (1u << SM_FRAG_CAP_RT);
    if (mWaterDescValid && mWaterPipe) m |= 1u << SM_WATER;
    if (mWaterDescValid && mWaterCapPipe) m |= 1u << SM_WATER_CAP;
    if (mDeferPipe && mDeferLayout) m |= 1u << SM_DEFER;
    if (mDeferSplitOk && mDeferSplitLayout)
        m |= (1u << SM_DSHADOW) | (1u << SM_DREFLECT) | (1u << SM_DCOMBINE) | (1u << SM_DREFLECT_HALF) | (1u << SM_DREFLECT_SHARP);
    if (mDeferFeatOk && mDeferSplitLayout)   // #827 feature split (+ #831 its shadow + tile classify kernel)
        m |= (1u << SM_DBASE) | (1u << SM_DLOCAL) | (1u << SM_DSHADOW_CLS);
    if (mPartLightPipe && mPartLightLayout) m |= 1u << SM_PLIGHT;
    if (s.rt && mPartLightPipeRt && mPartLightLayoutRt) m |= 1u << SM_PLIGHT_RT;
    s.modMask = m;
    const VkDevice dev = mCtx->device();
    const VkPhysicalDevice phys = mCtx->physicalDevice();
    const std::string path = specCachePath(key);
    const std::string tag = mCtx->pipelineCacheTag();
    const bool g11 = s.g11;
    const bool clk = mClkOn;   // (RenderVKShaderClockLighting: fixed for the session)
    s.loadJob = std::async(std::launch::async, [dev, phys, path, tag, m, g11, clk]()
    {
        SpecSet::Loaded L;
        std::vector<uint8_t> blob;
        // (validated against this device / driver and the viewer + shader build tag; a stale
        // or corrupt file is deleted by load and the key compiles cold)
        if (!path.empty() && vkpipecache::load(phys, path, tag, blob)) L.diskBytes = blob.size();
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = blob.size();
        ci.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(dev, &ci, nullptr, &L.cache) != VK_SUCCESS)
        {
            L.diskBytes = 0;
            ci.initialDataSize = 0; ci.pInitialData = nullptr;
            if (vkCreatePipelineCache(dev, &ci, nullptr, &L.cache) != VK_SUCCESS) L.cache = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < SM_COUNT; ++i)
            if (m & (1u << i)) L.mods[i] = specModule(dev, specModSrc(i, g11, clk));
        return L;
    });
    lane = set.get();
    mSpecSets.push_back(std::move(set));
    LL_INFOS("VKSpec") << "VKSpec: building key " << specKeyText(key) << " (" << why
                       << (lane->rt ? ", ray-query slots after the plain ones" : ", no ray-query slots: the key cannot trace")
                       << (lane->prewarm ? "; 1 low-priority thread" : "") << ") in the background" << LL_ENDL;
    return true;
}

// Advances the builds (non-blocking; both lanes) and harvests finished cache saves.
void LLCleanRendererVK::specPollBuild()
{
    const VkDevice dev = mCtx->device();
    // Saves (any set): the size is the key's footprint; the cache goes.
    bool evict = false;
    for (auto& up : mSpecSets)
    {
        SpecSet& s = *up;
        if (!s.saveJob.valid() || s.saveJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        s.bytes = s.saveJob.get();
        if (s.cache) { vkDestroyPipelineCache(dev, s.cache, nullptr); s.cache = VK_NULL_HANDLE; }
        if (!specCachePath(s.key).empty() && s.bytes)
        {
            SpecDiskEntry& e = mSpecDisk[s.key];
            e.used = (int64_t)std::time(nullptr);
            e.bytes = s.bytes;
            mSpecDiskDirty = true;
        }
        LL_INFOS("VKSpec") << "VKSpec: key " << specKeyText(s.key) << " cache " << mb(s.bytes)
                           << (s.diskBytes ? (s.bytes > s.diskBytes ? " (grew on disk)" : " (from disk, unchanged)")
                                           : " (saved)") << LL_ENDL;
        evict = true;
    }
    if (evict) specEvict(false);
    specAdvanceBuild(mSpecBuild);
    specAdvanceBuild(mSpecPrewarmBuild);
}

// One lane's build, one step (lane = nullptr once every phase is done).
void LLCleanRendererVK::specAdvanceBuild(SpecSet*& lane)
{
    SpecSet* s = lane;
    if (!s) return;
    const VkDevice dev = mCtx->device();
    const double now = specClock();
    if (s->phase == SpecSet::LOADING)
    {
        if (s->loadJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        SpecSet::Loaded L = s->loadJob.get();
        s->cache = L.cache;
        s->diskBytes = L.diskBytes;
        for (uint32_t i = 0; i < SM_COUNT; ++i) s->mods[i] = L.mods[i];
        if (!L.diskBytes && mSpecDisk.erase(s->key)) mSpecDiskDirty = true;   // (stale / missing file)
        // COMPILING: phase 1 -- every plain pipeline (graphics AND compute) as ONE async batch
        // into the set's cache (the batch queue is main-thread state), compiled wide.
        {
            PipelineBatchScope batchScope(dev);
            static const uint32_t kFragMod[4] = {SM_FRAG, SM_FRAG_BLEND, SM_FRAG_EZ, SM_FRAG_CAP};   // (by LitFragKind)
            for (int i = 0; i < LIT_COUNT; ++i)
            {
                const LitDbgDesc& r = mLitDbgDesc[0][i];
                if (!r.valid || !mLitVar[0][i] || r.frag > LITF_CAP) continue;   // (no generic slot: no twin)
                GraphicsPipelineDesc d = r.desc;
                d.vert = s->mods[r.rigged ? SM_RVS : SM_VS];
                d.frag = s->mods[kFragMod[r.frag]];
                if (!d.vert || !d.frag) continue;
                d.fragSpec = &s->info;
                createGraphicsPipelineInto(dev, d, &s->lit[0][i], r.frag == LITF_CAP ? "spec capture lit (plain)" : "spec lit (plain)");
            }
            if (s->mods[SM_VS] && s->mods[SM_WATER])
            {
                GraphicsPipelineDesc d = mWaterDesc;
                d.vert = s->mods[SM_VS]; d.frag = s->mods[SM_WATER]; d.fragSpec = &s->info;
                createGraphicsPipelineInto(dev, d, &s->water, "spec water");
            }
            if (s->mods[SM_VS] && s->mods[SM_WATER_CAP])
            {
                GraphicsPipelineDesc d = mWaterCapDesc;
                d.vert = s->mods[SM_VS]; d.frag = s->mods[SM_WATER_CAP]; d.fragSpec = &s->info;
                createGraphicsPipelineInto(dev, d, &s->waterCap, "spec capture water");
            }
            // Per-pixel lit particles: the pairs that exist now (common pairs + any built on
            // demand so far; one built later keeps its generic pipeline for this key). The
            // ray-query variant goes with phase 2.
            for (uint32_t v = 0; v < PART_PIPE_LIT; ++v)
            {
                if (v == PART_PIPE_MAIN_RT) continue;
                for (uint32_t pair = 0; pair < NUM_PART_PAIRS; ++pair)
                {
                    GraphicsPipelineDesc d;
                    if (!mPartPipe[v][pair] || !partPipeDesc(v, pair, d)) continue;   // (renderer-owned modules)
                    d.fragSpec = &s->info;
                    createGraphicsPipelineInto(dev, d, &s->part[v][pair], "spec particles");
                }
            }
            // Compute (a failure leaves VK_NULL_HANDLE: that slot binds the generic one).
            // (#1075: the uber kernel lights a stereo frame's views as layers -- kViewCount on a stereo key)
            createComputePipelineInto(dev, s->mods[SM_DEFER], mDeferLayout, 0, s->layeredInfo(), &s->defer, "spec compute");
#if defined(LL_HAS_CLEAN_PIPELINE)
            {
                // LIGHTING REGISTER PROBES (RenderVKLightRegProbe, measurement only): the G11 uber
                // kernel with local lights / reflections / atmosphere (and all three) compiled
                // out, built with THIS key's constants so RenderVKPipelineStats compares them with
                // the live kernel; never bound.
                static LLCachedControl<bool> s_lprobe(gSavedSettings, "RenderVKLightRegProbe", false);
                if (s_lprobe && s->g11)
                {
                    const struct { const uint32_t* spv; size_t bytes; const char* name; } lp[7] = {
                        {kWorldLightProbeLocalSpv, kWorldLightProbeLocalSpv_bytes, "lightProbe NO_LOCAL"},
                        {kWorldLightProbeReflSpv,  kWorldLightProbeReflSpv_bytes,  "lightProbe NO_REFL"},
                        {kWorldLightProbeAtmosSpv, kWorldLightProbeAtmosSpv_bytes, "lightProbe NO_ATMOS"},
                        {kWorldLightProbeAllSpv,   kWorldLightProbeAllSpv_bytes,   "lightProbe NO_ALL"},
                        {kWorldLightProbeOnlyLocalSpv, kWorldLightProbeOnlyLocalSpv_bytes, "lightProbe ONLY_LOCAL"},
                        {kWorldLightProbeOnlyReflSpv,  kWorldLightProbeOnlyReflSpv_bytes,  "lightProbe ONLY_REFL"},
                        {kWorldLightProbeOnlyAtmosSpv, kWorldLightProbeOnlyAtmosSpv_bytes, "lightProbe ONLY_ATMOS"}};
                    for (uint32_t i = 0; i < 7; ++i)
                    {
                        VkShaderModule m = specModule(dev, SpecModSrc{lp[i].spv, lp[i].bytes, lp[i].name});
                        createComputePipelineInto(dev, m, mDeferLayout, 0, &s->info, &s->lprobe[i], lp[i].name);
                        destroyShaderModuleDeferred(dev, m, nullptr);
                    }
                }
            }
#endif
            createComputePipelineInto(dev, s->mods[SM_DSHADOW], mDeferSplitLayout, 0, &s->info, &s->split[DSP_SHADOW], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DREFLECT], mDeferSplitLayout, 0, &s->info, &s->split[DSP_REFLECT], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DCOMBINE], mDeferSplitLayout, 0, &s->info, &s->split[DSP_COMBINE], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DREFLECT_HALF], mDeferSplitLayout, 0, &s->info, &s->split[DSP_REFLECT_HALF], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DREFLECT_SHARP], mDeferSplitLayout, 0, &s->info, &s->split[DSP_REFLECT_SHARP], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DBASE], mDeferSplitLayout, 0, &s->info, &s->feat[DFP_BASE], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DLOCAL], mDeferSplitLayout, 0, &s->info, &s->feat[DFP_LOCAL], "spec compute");
            createComputePipelineInto(dev, s->mods[SM_DSHADOW_CLS], mDeferSplitLayout, deferTileStageFlags(), &s->info,
                                      &s->feat[DFP_SHADOW_CLS], "spec compute");   // (#831: the tile builds' flag)
            createComputePipelineInto(dev, s->mods[SM_PLIGHT], mPartLightLayout, 0, &s->info, &s->partLight, "spec compute");
            // The phase-1 modules go with the job (destroyed by pipelineJobFinish); the
            // vertex modules stay for phase 2 when the key traces.
            for (uint32_t i : {SM_FRAG, SM_FRAG_BLEND, SM_FRAG_EZ, SM_FRAG_CAP, SM_WATER, SM_WATER_CAP,
                               SM_DEFER, SM_DSHADOW, SM_DREFLECT, SM_DCOMBINE, SM_PLIGHT, SM_DREFLECT_HALF, SM_DREFLECT_SHARP,
                               SM_DBASE, SM_DLOCAL, SM_DSHADOW_CLS})
                if (s->mods[i]) { destroyShaderModuleDeferred(dev, s->mods[i], nullptr); s->mods[i] = VK_NULL_HANDLE; }
            if (!s->rt)
                for (uint32_t i : {SM_VS, SM_RVS})
                    if (s->mods[i]) { destroyShaderModuleDeferred(dev, s->mods[i], nullptr); s->mods[i] = VK_NULL_HANDLE; }
            PipelineJobOpts o;
            o.cache = s->cache;
            o.prio = s->prio;
            o.maxThreads = s->maxThreads;
            o.lowThreadPriority = s->prewarm;
            o.gate = false;   // (the generic set draws the world: not part of the login gate)
            o.what = s->prewarm ? "spec prewarm" : "spec";
            s->gfxJob = pipelineBatchFlushAsync(dev, o);
        }
        s->phase = SpecSet::COMPILING;
        return;
    }
    if (s->phase == SpecSet::COMPILING)
    {
        if (s->gfxJob && !pipelineJobDone(s->gfxJob)) return;
        if (s->gfxJob) { pipelineJobFinish(s->gfxJob); s->gfxJob = nullptr; }
        // The split deferred passes are part of every set (the world batch built the generic
        // ones before any set starts); a twin that failed to compile binds its generic pass
        // (deferSplitPipe; correct -- the fields follow the key).
        unsigned failed = 0;
        s->pipes = 0;
        s->eachPipeline([&](VkPipeline& p) { if (p) ++s->pipes; });
        for (int i = 0; i < LIT_COUNT; ++i) failed += (mLitDbgDesc[0][i].valid && mLitVar[0][i] && !s->lit[0][i]) ? 1u : 0u;
        s->phase = SpecSet::READY;   // BINDABLE from here (the ray-query slots: generic until phase 2 lands)
        s->tReady = now;
        LL_INFOS("VKSpec") << "VKSpec: key " << specKeyText(s->key) << " ready in " << (S32)((now - s->t0) * 1000.0) << " ms: "
                           << s->pipes << " pipelines" << (failed ? " (" + std::to_string(failed) + " lit slots failed: generic)" : std::string())
                           << ", cache " << (s->diskBytes ? "warm from disk (" + mb(s->diskBytes) + ")" : std::string("cold"))
                           << (s->rt ? "; ray-query slots next (generic ray-query pipelines until then)" : "") << LL_ENDL;
        if (s->rt) return;   // phase 2 below, from the next tick
    }
    if (s->rt && !s->rtQueued)
    {
        // Phase 2 needs the generic ray-query variant (its slots + descs): it compiles after
        // the world batch at login (mRtPipelineJob).
        if (mRtPipelineJob) return;
        s->rtQueued = true;
        unsigned want = 0;
        {
            PipelineBatchScope batchScope(dev);
            static const uint32_t kFragModRt[4] = {SM_FRAG_RT, SM_FRAG_BLEND_RT, SM_FRAG_RT_EZ, SM_FRAG_CAP_RT};
            for (int i = 0; i < LIT_COUNT; ++i)
            {
                const LitDbgDesc& r = mLitDbgDesc[1][i];
                if (!r.valid || !mLitVar[1][i] || r.frag > LITF_CAP) continue;   // (no generic slot: no twin)
                GraphicsPipelineDesc d = r.desc;
                d.vert = s->mods[r.rigged ? SM_RVS : SM_VS];
                d.frag = s->mods[kFragModRt[r.frag]];
                if (!d.vert || !d.frag) continue;
                d.fragSpec = &s->info;
                createGraphicsPipelineInto(dev, d, &s->litRtStage[i], r.frag == LITF_CAP ? "spec capture lit (rayquery)" : "spec lit (rayquery)");
                ++want;
            }
            for (uint32_t pair = 0; pair < NUM_PART_PAIRS; ++pair)
            {
                GraphicsPipelineDesc d;
                if (!mPartPipe[PART_PIPE_MAIN_RT][pair] || !partPipeDesc(PART_PIPE_MAIN_RT, pair, d)) continue;
                d.fragSpec = &s->info;
                createGraphicsPipelineInto(dev, d, &s->partRtStage[pair], "spec particles (rayquery)");
                ++want;
            }
            if (s->mods[SM_PLIGHT_RT] && mPartLightLayoutRt)
            {
                createComputePipelineInto(dev, s->mods[SM_PLIGHT_RT], mPartLightLayoutRt, 0, &s->info, &s->partLightRtStage,
                                          "spec compute (rayquery)");
                ++want;
            }
            for (uint32_t i : {SM_VS, SM_RVS, SM_FRAG_RT, SM_FRAG_BLEND_RT, SM_FRAG_RT_EZ, SM_FRAG_CAP_RT, SM_PLIGHT_RT})
                if (s->mods[i]) { destroyShaderModuleDeferred(dev, s->mods[i], nullptr); s->mods[i] = VK_NULL_HANDLE; }
            PipelineJobOpts o;
            o.cache = s->cache;
            o.prio = s->prio;
            o.maxThreads = s->maxThreads;
            o.lowThreadPriority = s->prewarm;
            o.gate = false;
            o.what = s->prewarm ? "spec prewarm, ray-query" : "spec, ray-query";
            s->rtJob = pipelineBatchFlushAsync(dev, o);   // (nullptr: nothing to build)
        }
        if (s->rtJob)
            LL_INFOS("VKSpec") << "VKSpec: key " << specKeyText(s->key) << ": " << want
                               << " ray-query pipelines compiling (bound on the generic ones until they land)" << LL_ENDL;
        return;
    }
    if (s->rtJob)
    {
        if (!pipelineJobDone(s->rtJob)) return;
        pipelineJobFinish(s->rtJob);
        s->rtJob = nullptr;
        unsigned made = 0;
        for (int i = 0; i < LIT_COUNT; ++i) { s->lit[1][i] = s->litRtStage[i]; s->litRtStage[i] = VK_NULL_HANDLE; made += s->lit[1][i] ? 1u : 0u; }
        for (uint32_t pair = 0; pair < NUM_PART_PAIRS; ++pair)
        {
            s->part[PART_PIPE_MAIN_RT][pair] = s->partRtStage[pair];
            s->partRtStage[pair] = VK_NULL_HANDLE;
            made += s->part[PART_PIPE_MAIN_RT][pair] ? 1u : 0u;
        }
        s->partLightRt = s->partLightRtStage;
        s->partLightRtStage = VK_NULL_HANDLE;
        made += s->partLightRt ? 1u : 0u;
        s->pipes += made;
        LL_INFOS("VKSpec") << "VKSpec: key " << specKeyText(s->key) << ": " << made << " ray-query pipelines landed "
                           << (S32)((now - s->t0) * 1000.0) << " ms after the build started" << LL_ENDL;
    }
    // Built (every phase): the lane's next key may start.
    lane = nullptr;
    if (s->prewarm) mSpecLastPrewarmEnd = now;
    // Persist (worker): the per-key cache file; its size is the set's footprint. (After the
    // last phase: no job compiles into the cache any more.)
    {
        const VkPhysicalDevice phys = mCtx->physicalDevice();
        const std::string path = specCachePath(s->key);
        const std::string tag = mCtx->pipelineCacheTag();
        const VkPipelineCache cache = s->cache;
        const size_t loaded = s->diskBytes;
        char why[64];
        std::snprintf(why, sizeof(why), "spec key %05" PRIx64, s->key);
        const std::string whyS = why;
        if (cache)
            s->saveJob = std::async(std::launch::async, [dev, phys, path, tag, cache, loaded, whyS]() -> uint64_t
            {
                size_t n = loaded;
                if (!path.empty()) vkpipecache::save(dev, phys, cache, path, tag, n, whyS.c_str());
                size_t sz = 0;
                if (vkGetPipelineCacheData(dev, cache, &sz, nullptr) == VK_SUCCESS && sz) n = sz;
                return (uint64_t)n;
            });
    }
    specEvict(false);   // (last: also bounds the resident count when a set never saves -- no per-key cache)
}

// LRU over resident sets (and, separately, the per-key cache files) within
// RenderVKSpecCacheMB each; never the bound, wanted or building key. force_disk: also
// rewrite the index.
void LLCleanRendererVK::specEvict(bool force_disk)
{
    uint64_t cap = 256ull << 20;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKSpecCacheMB", 256);
    cap = (uint64_t)std::max<U32>(s_mb, 16u) << 20;
#endif
    const uint64_t liveKey = mSpecLive ? mSpecLive->key : kSpecNone;
    auto pinned = [&](uint64_t k)
    {
        return k == liveKey || k == mSpecWanted || (mSpecBuild && k == mSpecBuild->key)
            || (mSpecPrewarmBuild && k == mSpecPrewarmBuild->key);
    };
    for (;;)
    {
        uint64_t total = 0;
        size_t n = 0;
        SpecSet* victim = nullptr;
        for (auto& up : mSpecSets)
        {
            SpecSet& s = *up;
            total += s.footprint();
            ++n;
            if (s.phase != SpecSet::READY || s.busy() || pinned(s.key)) continue;
            if (!victim || s.lastUse < victim->lastUse) victim = &s;
        }
        mSpecPeakBytes = std::max(mSpecPeakBytes, total);
        if ((total <= cap && n <= kSpecMaxSets) || !victim) break;
        LL_INFOS("VKSpec") << "VKSpec: evicting key " << specKeyText(victim->key) << " (" << mb(victim->footprint())
                           << ", unused " << (S32)(specClock() - victim->lastUse) << " s; resident " << mb(total) << " of "
                           << mb(cap) << ", " << n << " sets)" << LL_ENDL;
        // Frames in flight may still bind them: destroyed through the retire lists.
        victim->eachPipeline([&](VkPipeline& p) { retirePipeline(p); p = VK_NULL_HANDLE; });
        if (victim->cache) { vkDestroyPipelineCache(mCtx->device(), victim->cache, nullptr); victim->cache = VK_NULL_HANDLE; }
        mSpecSets.erase(std::remove_if(mSpecSets.begin(), mSpecSets.end(),
                                       [&](const std::unique_ptr<SpecSet, SpecSetDeleter>& up) { return up.get() == victim; }),
                        mSpecSets.end());
    }
    // Disk: the per-key cache files, oldest use first.
    for (;;)
    {
        uint64_t total = 0;
        auto victim = mSpecDisk.end();
        for (auto it = mSpecDisk.begin(); it != mSpecDisk.end(); ++it)
        {
            total += it->second.bytes;
            if (pinned(it->first)) continue;
            if (victim == mSpecDisk.end() || it->second.used < victim->second.used) victim = it;
        }
        mSpecDiskPeak = std::max(mSpecDiskPeak, total);
        if (total <= cap || victim == mSpecDisk.end()) break;
        const std::string path = specCachePath(victim->first);
        LL_INFOS("VKSpec") << "VKSpec: deleting the cache file of key " << specKeyText(victim->first) << " ("
                           << mb(victim->second.bytes) << "; on disk " << mb(total) << " of " << mb(cap) << ")" << LL_ENDL;
        if (!path.empty()) LLFile::remove(path, ENOENT);
        mSpecDisk.erase(victim);
        mSpecDiskDirty = true;
    }
    if (mSpecDiskDirty || force_disk) specSaveIndex();
}

// vk_spec_index.txt next to the cache files: "<key hex> <last use, unix s> <bytes>" per line.
void LLCleanRendererVK::specSaveIndex()
{
    mSpecDiskDirty = false;
    const std::string any = specCachePath(0);
    if (any.empty()) return;
    const std::string path = any.substr(0, any.find_last_of("/\\") + 1) + "vk_spec_index.txt";
    std::ostringstream o;
    o << "SLVKSPEC 1\n";
    for (const auto& kv : mSpecDisk)
    {
        char line[96];
        std::snprintf(line, sizeof(line), "%" PRIx64 " %lld %llu\n", kv.first, (long long)kv.second.used,
                      (unsigned long long)kv.second.bytes);
        o << line;
    }
    const std::string text = o.str();
    const std::string tmp = path + ".tmp";
    if (LLFILE* f = LLFile::fopen(tmp, "wb"))
    {
        const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        LLFile::close(f);
#if LL_WINDOWS
        if (ok) LLFile::remove(path, ENOENT);
#endif
        if (!ok || LLFile::rename(tmp, path) != 0) LLFile::remove(tmp, ENOENT);
    }
}

void LLCleanRendererVK::specLog(const char* what, const SpecSet* s) const
{
    uint64_t cap = 256ull << 20;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKSpecCacheMB", 256);
    cap = (uint64_t)std::max<U32>(s_mb, 16u) << 20;
#endif
    uint64_t total = 0, disk = 0;
    std::string keys;
    for (const auto& up : mSpecSets)
    {
        total += up->footprint();
        char b[48];
        std::snprintf(b, sizeof(b), " %05" PRIx64 "%s=%.1f%s", up->key, up.get() == mSpecLive ? "*" : "",
                      (double)up->footprint() / (1024.0 * 1024.0), up->bytes ? "" : "~");
        keys += b;
    }
    for (const auto& kv : mSpecDisk) disk += kv.second.bytes;
    LL_INFOS("VKSpec") << "VKSpec: " << what << (s ? " " + specKeyText(s->key) : std::string()) << " | resident "
                       << mSpecSets.size() << " sets " << mb(total) << " live / " << mb(cap) << " cap / "
                       << mb(mSpecPeakBytes) << " peak (MB per key, * bound, ~ estimate:" << keys << ") | disk "
                       << mSpecDisk.size() << " keys " << mb(disk) << " live / " << mb(cap) << " cap / "
                       << mb(mSpecDiskPeak) << " peak" << LL_ENDL;
}

// PREWARM BUSY: compiling ahead waits while the viewer is logging in or teleporting, and
// while frames run slow -- the smoothed frame time above the 60 Hz budget (+20%), or a frame
// over kSpecSlowFrameSec (a hitch: region crossing, a burst of loads) in the last 10 s.
bool LLCleanRendererVK::specPrewarmBusy(double now)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (LLStartUp::getStartupState() < STATE_STARTED) return true;
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) return true;
#endif
    if (mSpecFrameEma > (1.0 / 60.0) * 1.2) return true;
    return now - mSpecSlowAt < 10.0;
}

// ---- the frame tick ----
void LLCleanRendererVK::specFrameTick()
{
    if (!mCtx || !mReady) return;
    pollLitDebug(false);   // (debug twins: installed when their background job is done)
    // The world batch first (the sets copy its descs). The ray-query batch is NOT waited for
    // here: a set's plain phase overlaps it (~10 s of a cold login), its ray-query phase waits
    // for it (specPollBuild).
    if (mPipelineJob) return;
    const double now = specClock();
    // Frame time for prewarm's busy test (this tick runs once per world frame).
    if (mSpecTickLast > 0.0)
    {
        const double dt = now - mSpecTickLast;
        mSpecFrameEma = mSpecFrameEma > 0.0 ? mSpecFrameEma * 0.9 + dt * 0.1 : dt;
        if (dt > kSpecSlowFrameSec) mSpecSlowAt = now;
    }
    mSpecTickLast = now;
    if (!mSpecInit)
    {
        mSpecInit = true;
        // The index of the per-key cache files (entries whose file is gone are dropped).
        const std::string any = specCachePath(0);
        if (!any.empty())
        {
            const std::string path = any.substr(0, any.find_last_of("/\\") + 1) + "vk_spec_index.txt";
            if (LLFILE* f = LLFile::fopen(path, "rb"))
            {
                char line[128];
                bool header = true;
                while (fgets(line, sizeof(line), f))
                {
                    if (header) { header = false; if (std::strncmp(line, "SLVKSPEC 1", 10) != 0) break; continue; }
                    unsigned long long k = 0, b = 0;
                    long long u = 0;
                    if (std::sscanf(line, "%llx %lld %llu", &k, &u, &b) != 3) continue;
                    if (!LLFile::isfile(specCachePath((uint64_t)k))) { mSpecDiskDirty = true; continue; }
                    mSpecDisk[(uint64_t)k] = SpecDiskEntry{(int64_t)u, (uint64_t)b};
                }
                LLFile::close(f);
            }
        }
        specEvict(true);
    }
    specPollBuild();
    const uint64_t want = specKeyFromSettings();
    if (want != mSpecWanted)
    {
        if (mSpecWanted != kSpecNone && want != kSpecNone)
        {
            std::string what;
            for (uint32_t i = 0; i < SPEC_COUNT; ++i)
                if (keyField(want, i) != keyField(mSpecWanted, i))
                    what += std::string(what.empty() ? "" : ", ") + kSpecTable[i].label + " " + std::to_string(keyField(mSpecWanted, i))
                          + " -> " + std::to_string(keyField(want, i));
            LL_INFOS("VKSpec") << "VKSpec: settings changed (" << what << ")" << LL_ENDL;
        }
        mSpecPrev = mSpecWanted;
        mSpecWanted = want;
        mSpecPendSince = now;
        mSpecPrewarmDone = false;
        if (mSpecFallback) mSpecFallback = false;   // (re-armed for the new change)
    }
    bool pending = false;
    if (want == kSpecNone)
    {
        // RenderVKSpecPipelines off: the generic pipelines, live settings.
        if (mSpecLive) { mSpecLive = nullptr; LL_INFOS("VKSpec") << "VKSpec: off (RenderVKSpecPipelines): generic pipelines" << LL_ENDL; }
        mSpecPendSince = -1.0;
        mSpecFallback = false;
    }
    else
    {
        SpecSet* ws = nullptr;
        for (auto& up : mSpecSets) if (up->key == want) { ws = up.get(); break; }
        if (ws && ws->phase == SpecSet::READY)
        {
            ws->lastUse = now;
            if (mSpecLive != ws)
            {
                const bool fb = mSpecFallback;
                mSpecLive = ws;   // THE switch: this frame's state is filled with its key from here on
                char t[96];
                std::snprintf(t, sizeof(t), "bound (%s, %.0f ms after the change)",
                              fb ? "replacing the generic fallback" : (ws->tReady + 0.5 < mSpecPendSince ? "resident" : "built"),
                              mSpecPendSince >= 0.0 ? (now - mSpecPendSince) * 1000.0 : 0.0);
                specLog(t, ws);
            }
            mSpecPendSince = -1.0;
            mSpecFallback = false;
        }
        else
        {
            pending = true;
            if (!ws && !mSpecBuild) specStartBuild(want, mSpecPrev == kSpecNone ? "login" : "setting change");
            // (#818) SKY MODE CHANGE: the environment, not the user, flipped classic <-> linear (a
            // teleport, a region / parcel / personal sky). The bound set has the OTHER mode's math
            // folded in -- no grace period: the generic pipelines (run-time test of the frame's
            // atmos[8].w) from this frame until the new key's set lands. No notice (nothing the
            // user changed); the flip is prewarmed below, so this is normally a resident bind.
            if (mSpecLive && keyField(mSpecLive->key, SPEC_SKY) != keyField(want, SPEC_SKY)
                && keyField(mSpecLive->key, SPEC_SKY) < 2u)
            {
                LL_INFOS("VKSpec") << "VKSpec: sky mode changed to " << (keyField(want, SPEC_SKY) == 1u ? "classic" : "linear")
                                   << ": generic pipelines until key " << specKeyText(want) << " is built" << LL_ENDL;
                mSpecLive = nullptr;
                mSpecFallback = true;
            }
            // TIMEOUT: the old configuration has rendered long enough -- the new values apply
            // on the generic pipelines until the set lands (frame boundary: this tick).
            F32 timeout = 3.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
            static LLCachedControl<F32> s_timeout(gSavedSettings, "RenderVKSpecApplyTimeoutSec", 3.f);
            timeout = std::max((F32)s_timeout, 0.f);
#endif
            if (mSpecLive && mSpecPendSince >= 0.0 && now - mSpecPendSince >= timeout)
            {
                LL_INFOS("VKSpec") << "VKSpec: key " << specKeyText(want) << " still compiling after " << (S32)(now - mSpecPendSince)
                                   << " s: the new settings apply on the generic pipelines until it lands" << LL_ENDL;
                mSpecLive = nullptr;
                mSpecFallback = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
                // (one notice per 10 minutes at most: the prefs line says it every time)
                static double s_noticeNext = 0.0;
                if (now >= s_noticeNext)
                {
                    s_noticeNext = now + 600.0;
                    doAfterInterval([]() { LLNotificationsUtil::add("VKSpecAppliedGeneric"); }, 0.f);
                }
#endif
            }
        }
    }
    if (mSpecLive) mSpecLive->lastUse = now;

    // PREWARM (idle: nothing pending or building for a while, and the frame not busy --
    // specPrewarmBusy): RenderVKSpecPrewarm 1 (default) = only the previous key (a setting
    // flipped back binds at once); 2 = then one-flip neighbours of the wanted key in priority
    // order (filter quality, SSR, mirror, cel), at least kSpecNeighbourGapSec apart -- each
    // only within the budget. Its own lane: one low-priority compile thread (specStartBuild).
    if (pending || mSpecBuild) mSpecIdleSince = -1.0;
    else if (mSpecIdleSince < 0.0) mSpecIdleSince = now;
    U32 prewarm = 1;
    uint64_t cap = 256ull << 20;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_prewarm(gSavedSettings, "RenderVKSpecPrewarm", 1);
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKSpecCacheMB", 256);
    prewarm = s_prewarm;
    cap = (uint64_t)std::max<U32>(s_mb, 16u) << 20;
#endif
    if (want != kSpecNone && prewarm > 0 && !mSpecPrewarmDone && !mSpecBuild && !mSpecPrewarmBuild && mSpecIdleSince >= 0.0
        && now - mSpecIdleSince > 5.0 && !specPrewarmBusy(now))
    {
        std::vector<uint64_t> cands;
        if (mSpecPrev != kSpecNone && mSpecPrev != want) cands.push_back(mSpecPrev);
        // (#818) the other sky mode: environment-driven (a teleport between a classic and a
        // linear sky), so it is prewarmed like a flip back (same budget rules).
        if (keyField(want, SPEC_SKY) < 2u) cands.push_back(keyWith(want, SPEC_SKY, keyField(want, SPEC_SKY) ^ 1u));
        if (prewarm >= 2)
        {
            const uint32_t f = keyField(want, SPEC_FILTER);
            if (keyField(want, SPEC_SUN) || keyField(want, SPEC_PROJ))
            {
                if (f < 3u) cands.push_back(keyWith(want, SPEC_FILTER, f + 1u));
                if (f > 0u) cands.push_back(keyWith(want, SPEC_FILTER, f - 1u));
            }
            cands.push_back(keyWith(want, SPEC_SSR, keyField(want, SPEC_SSR) ^ 1u));
            cands.push_back(keyWith(want, SPEC_MIRROR, keyField(want, SPEC_MIRROR) ^ 1u));
            cands.push_back(keyWith(want, SPEC_CEL, keyField(want, SPEC_CEL) ^ 1u));
        }
        uint64_t total = 0, avg = 0;
        for (auto& up : mSpecSets) total += up->footprint();
        if (!mSpecSets.empty()) avg = total / mSpecSets.size();
        bool started = false;
        for (uint64_t k : cands)
        {
            bool have = false;
            for (auto& up : mSpecSets) if (up->key == k) { have = true; break; }
            if (have) continue;
            if (total + avg > cap || mSpecSets.size() + 1 > kSpecMaxSets) break;   // (budget full: no prewarm)
            // Neighbours are speculative: one per kSpecNeighbourGapSec at most.
            const bool skyFlip = keyField(k, SPEC_SKY) != keyField(want, SPEC_SKY);   // (#818: as urgent as a flip back)
            if (k != mSpecPrev && !skyFlip && now - mSpecLastPrewarmEnd < kSpecNeighbourGapSec) { started = true; break; }   // (later)
            started = specStartBuild(k, k == mSpecPrev ? "prewarm: flip back" : skyFlip ? "prewarm: sky mode" : "prewarm: neighbour");
            break;
        }
        if (!started) mSpecPrewarmDone = true;
    }

    // UI snapshot (graphics preferences): what is pending, by label, against the bound key.
    {
        std::string text;
        if (pending && mSpecPrev != kSpecNone)
        {
            const uint64_t from = mSpecLive ? mSpecLive->key : mSpecPrev;
            for (uint32_t i = 0; i < SPEC_COUNT; ++i)
                if (keyField(from, i) != keyField(want, i))
                    text += std::string(text.empty() ? "" : ", ") + kSpecTable[i].label;
            if (text.empty()) text = "graphics settings";
        }
        std::lock_guard<std::mutex> lk(sStatusMutex);
        sStatusText = text;
        sStatusFallback = pending && !mSpecLive;   // (the new values already apply: generic pipelines)
    }
    // Periodic summary while sets churn is covered by the event lines; a quiet minute's line.
    if (now >= mSpecLogNext && !mSpecSets.empty())
    {
        mSpecLogNext = now + 300.0;
        specLog("status", mSpecLive);
    }
}

void LLCleanRendererVK::specShutdown()
{
    if (!mCtx) return;
    const VkDevice dev = mCtx->device();
    pollLitDebug(true);
    mSpecLive = nullptr;
    for (auto& up : mSpecSets)
    {
        SpecSet& s = *up;
        if (s.loadJob.valid())
        {
            SpecSet::Loaded L = s.loadJob.get();
            if (L.cache) vkDestroyPipelineCache(dev, L.cache, nullptr);
            for (VkShaderModule m : L.mods) if (m) vkDestroyShaderModule(dev, m, nullptr);
        }
        // (waits; destroys its modules) -- every job compiling into the set's cache / with its
        // spec info is finished before either goes (#782 audit)
        if (s.gfxJob) { pipelineJobFinish(s.gfxJob); s.gfxJob = nullptr; }
        if (s.rtJob)
        {
            pipelineJobFinish(s.rtJob);
            s.rtJob = nullptr;
            for (int i = 0; i < LIT_COUNT; ++i) { if (!s.lit[1][i]) s.lit[1][i] = s.litRtStage[i]; else if (s.litRtStage[i]) vkDestroyPipeline(dev, s.litRtStage[i], nullptr); }
            for (uint32_t p = 0; p < NUM_PART_PAIRS; ++p)
            {
                if (!s.part[PART_PIPE_MAIN_RT][p]) s.part[PART_PIPE_MAIN_RT][p] = s.partRtStage[p];
                else if (s.partRtStage[p]) vkDestroyPipeline(dev, s.partRtStage[p], nullptr);
            }
            if (!s.partLightRt) s.partLightRt = s.partLightRtStage;
            else if (s.partLightRtStage) vkDestroyPipeline(dev, s.partLightRtStage, nullptr);
        }
        for (VkShaderModule& m : s.mods) if (m) { vkDestroyShaderModule(dev, m, nullptr); m = VK_NULL_HANDLE; }
        if (s.saveJob.valid()) s.bytes = s.saveJob.get();
        s.eachPipeline([&](VkPipeline& p) { if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; } });
        if (s.cache) { vkDestroyPipelineCache(dev, s.cache, nullptr); s.cache = VK_NULL_HANDLE; }
    }
    if (!mSpecSets.empty()) specLog("shutdown", nullptr);
    mSpecSets.clear();
    mSpecBuild = nullptr;
    mSpecPrewarmBuild = nullptr;
    if (mSpecDiskDirty) specSaveIndex();
    std::lock_guard<std::mutex> lk(sStatusMutex);
    sStatusText.clear();
    sStatusFallback = false;
}

std::string LLCleanRendererVK::specStatusText(bool* fallback)
{
    std::lock_guard<std::mutex> lk(sStatusMutex);
    if (fallback) *fallback = sStatusFallback;
    return sStatusText;
}
