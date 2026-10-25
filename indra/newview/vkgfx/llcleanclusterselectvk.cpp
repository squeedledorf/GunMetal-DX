/**
 * @file llcleanclusterselectvk.cpp
 * @brief Cluster LOD selection / culling / emission and the cluster draws (#745, OWNER: 1b).
 *        Layouts, tiers and scope: llcleanclusterselectvk.h. Shaders:
 *        indra/vkclean/shaders/clean_cluster_select*.comp, clean_cluster_draw*.{task,mesh,frag},
 *        clean_draw.vert -DCLEAN_CLUSTER.
 *
 * Method: published material only (SIGGRAPH 2021 Nanite talk; meshoptimizer's MIT demo).
 */
#include "linden_common.h"

#include "llcleanclusterselectvk.h"
#include "llcleanclustervk.h"
#include "llcleanclusterstreamvk.h"
#include "llcleanclusterskinvk.h"
#include "vk_pipeline.h"
#include "vk_descpool.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "../llvkhitch.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llviewerobjectlist.h"
#  include "llviewerobject.h"
#  include "llvovolume.h"
#  include "llvoavatar.h"
#  include "llviewercamera.h"
#  include "llmodel.h"
#  include "llvolume.h"
#  include "m3math.h"
#  include "llquaternion.h"
#  include "../llcleaninstancestore.h"
#  include "../llcleanmeshasset.h"
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
using namespace LLClusterFmt;
constexpr uint32_t kSlots = LLCleanClusterVK::kFrameSlots;
constexpr VkDeviceSize kViewsBytes = LLCleanClusterSelectVK::kViewsTailOffset + 512;   // views + 1b tail (ClSelectAux 400 B)
constexpr VkDeviceSize kReadbackBytes = (VkDeviceSize)kMaxViews * LLCleanClusterSelectVK::kViewWords * 4;
constexpr uint32_t kResolvesPerFrame = 256;   // new instance-row key resolutions per frame
constexpr uint32_t kRetryFrames = 120;        // a failed resolution retries after this many frames

// Push blocks (16 B each; clean_cluster_select*.comp, the draw push of the mesh tier).
struct SelectPush { uint32_t view, instance_count, tier, pad; };
struct FinalizePush { uint32_t view, pad0, pad1, pad2; };
struct DrawPush16 { uint32_t view, f8, cl_view, bucket; };   // = VKDrawViewPush (pad0 / pad1 reused)
static_assert(sizeof(SelectPush) == 16 && sizeof(FinalizePush) == 16 && sizeof(DrawPush16) == 16, "push");
// The ClusterView records the 1b tail follows (clean_cluster_select_common.glsl ClSelectAux).
struct SelectAuxCPU
{
    float    occ_view_proj[16];
    float    occ_origin[4];
    uint32_t hiz[4];          // depth w, depth h, valid, mips
    uint32_t debug_mode;
    uint32_t aux_base;
    uint32_t tier;
    uint32_t flags;
    uint32_t rig[4];          // #702: first rig record, records per rig list and bucket, bit 0 on, pad
    uint32_t rig_bits[2 * kAvatarSlotWords];   // #702: per rig list kAvatarSlotCap avatar bits (rig_bits[L * 32 + slot / 32])
    uint32_t alpha[4];        // rigged alpha lists: first alpha record, records per list, entry cap, bit 0 on
};
constexpr size_t kAuxRigOffset = offsetof(SelectAuxCPU, rig);
// clean_cluster_pick.comp PickPush.
struct PickPush { float org[4]; float dir[4]; uint32_t view, cap; float cone; uint32_t pad; };
static_assert(sizeof(PickPush) == 48, "PickPush");
// clean_cluster_rigcull.comp RigCullPush (#748 perf pass: the MDI tiers' per-view rig-list cull).
struct RigCullPush { uint32_t view_lo, view_hi, list, main_view, out_base, pad0, pad1, pad2; };
static_assert(sizeof(RigCullPush) == 32, "RigCullPush");
// clean_cluster_alpha.comp AlphaPush (rigged alpha lists: prep / order / scatter).
struct AlphaPush { float cam[4]; uint32_t mode, view, under, pad; };
static_assert(sizeof(AlphaPush) == 32, "AlphaPush");
constexpr uint32_t kMinRigCap = 4096;   // records per rig list and bucket (grown from the read-back demand)
static_assert(sizeof(SelectAuxCPU) == LLCleanClusterSelectVK::kSelectAuxBytes, "ClSelectAux");
static_assert(kAuxRigOffset == 112, "ClSelectAux::rig");
static_assert(LLCleanClusterSelectVK::kViewsTailOffset == kMaxViews * sizeof(ClusterView), "views tail");

VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v && p < 0x80000000u) p <<= 1;
    return p;
}

// sigma_max of a 3x3 (row-major a[r * 3 + c]): sqrt of the largest eigenvalue of A^T A by
// power iteration, never below the largest column norm (a lower bound of sigma_max).
float sigmaMax3(const float a[9])
{
    float b[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            b[r * 3 + c] = a[0 * 3 + r] * a[0 * 3 + c] + a[1 * 3 + r] * a[1 * 3 + c] + a[2 * 3 + r] * a[2 * 3 + c];
    float col = 0.f;
    for (int c = 0; c < 3; ++c) col = std::max(col, b[c * 3 + c]);
    float v[3] = {1.f, 0.7f, 0.4f}, lambda = 0.f;
    for (int it = 0; it < 24; ++it)
    {
        float w[3];
        for (int r = 0; r < 3; ++r) w[r] = b[r * 3 + 0] * v[0] + b[r * 3 + 1] * v[1] + b[r * 3 + 2] * v[2];
        const float n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (!(n > 0.f)) break;
        lambda = n;
        for (int r = 0; r < 3; ++r) v[r] = w[r] / n;
    }
    const float s = std::sqrt(std::max(lambda, col));
    return std::isfinite(s) ? s : 0.f;
}

#if defined(LL_HAS_CLEAN_PIPELINE)
U32 settingDebug()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterDebug", 0);
    return s;
}
U32 settingStatic()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterStatic", 0);
    return s;
}
// #1011: the main view writes the RT cut summary + ranks (2b's ranked shared cut; its A/B)
bool settingRtRank()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterRTRigRank", 1);
    return (U32)s != 0u;
}
bool settingOcclusion()
{
    static LLCachedControl<bool> s(gSavedSettings, "RenderVKClusterOcclusion", true);
    return s;
}
// The depth passes' reduced cluster stages (the renderer's own depth-only pipes read it too).
bool settingDepthVs()
{
    static LLCachedControl<bool> s(gSavedSettings, "RenderVKDepthOnlyVS", true);
    return s;
}
// Rigged alpha-blend on the cluster path: the pixel tolerance cap of alpha instances (hair cards,
// lashes, sheer layers: thin, many small disconnected parts the DAG prunes whole), applied as
// tol_scale = min(tol_scale, cap / the main view's tolerance) -- the pressure controller may
// raise the opaque tolerance to 8+ px, alpha stays at <= cap. 0 = no cap.
F32 settingBlendTolMaxPx()
{
    static LLCachedControl<F32> s(gSavedSettings, "RenderVKClusterBlendTolMaxPx", 2.f);
    return s;
}
#endif
// #702 integration: the secondary pass kind of a draw pass (-1 = the main view's G-buffer /
// pre-pass): 0 sun cascades, 1 projector tiles, 2 captures (mirror / probe faces).
int passKind(uint32_t pass)
{
    return pass == CLPASS_SHADOW ? 0 : pass == CLPASS_PROJ ? 1 : pass == CLPASS_PROBE ? 2 : -1;
}
} // namespace

// =============================================================================================
// init / destroy
// =============================================================================================
bool LLCleanClusterSelectVK::init()
{
    VkDeviceContext& ctx = mOwner.ctx();
    const VkDevice dev = ctx.device();
    const LLClusterRenderEnv& env = mOwner.env();
#ifdef VK_EXT_mesh_shader
    mMesh = mOwner.caps().feat.meshShader && (mOwner.setStages() & VK_SHADER_STAGE_MESH_BIT_EXT) != 0;
#else
    mMesh = false;
#endif
    const VkDescriptorSetLayout cl = mOwner.setLayout();

    // Compute: {cluster set} + 16 B push.
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &cl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkdCreatePipelineLayout(dev, &pli, &mCompLayout, "mClusterSelectLayout") != VK_SUCCESS)
        {
            mCompLayout = VK_NULL_HANDLE;
            LL_WARNS("Vulkan") << "Cluster LOD select: compute layout failed" << LL_ENDL;
            return false;
        }
        VkShaderModule ms = makeModule(dev, kWorldClusterSelectSpv, kWorldClusterSelectSpv_bytes, "kWorldClusterSelectSpv");
        VkShaderModule mf = makeModule(dev, kWorldClusterFinalizeSpv, kWorldClusterFinalizeSpv_bytes, "kWorldClusterFinalizeSpv");
        if (ms) mSelectPipe = createComputePipeline(dev, ms, mCompLayout);
        if (mf) mFinalizePipe = createComputePipeline(dev, mf, mCompLayout);
        if (ms) vkDestroyShaderModule(dev, ms, nullptr);
        if (mf) vkDestroyShaderModule(dev, mf, nullptr);
        if (!mSelectPipe || !mFinalizePipe)
        {
            LL_WARNS("Vulkan") << "Cluster LOD select: compute pipelines failed" << LL_ENDL;
            return false;
        }
        // #702 integration: the GPU pick filter ({cluster set} + 48 B push). Optional: without
        // it the pick keeps the legacy candidates only.
        VkPushConstantRange ppr{VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(PickPush)};
        pli.pPushConstantRanges = &ppr;
        if (vkdCreatePipelineLayout(dev, &pli, &mPickCompLayout, "mClusterPickCompLayout") == VK_SUCCESS)
        {
            VkShaderModule mp = makeModule(dev, kWorldClusterPickSpv, kWorldClusterPickSpv_bytes, "kWorldClusterPickSpv");
            if (mp)
            {
                mPickFilterPipe = createComputePipeline(dev, mp, mPickCompLayout);
                vkDestroyShaderModule(dev, mp, nullptr);
            }
        }
        else
            mPickCompLayout = VK_NULL_HANDLE;
        // #748 perf pass: the MDI tiers' per-view rig-list cull ({cluster set} + 32 B push).
        // Optional: without it an MDI-tier secondary view draws the whole rig list (as before).
        VkPushConstantRange rpr{VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(RigCullPush)};
        pli.pPushConstantRanges = &rpr;
        if (vkdCreatePipelineLayout(dev, &pli, &mRigCullLayout, "mClusterRigCullLayout") == VK_SUCCESS)
        {
            VkShaderModule mr = makeModule(dev, kWorldClusterRigCullSpv, kWorldClusterRigCullSpv_bytes,
                                           "kWorldClusterRigCullSpv");
            if (mr)
            {
                mRigCullPipe = createComputePipeline(dev, mr, mRigCullLayout);
                vkDestroyShaderModule(dev, mr, nullptr);
            }
        }
        else
            mRigCullLayout = VK_NULL_HANDLE;
        // RIGGED ALPHA LISTS: the order compute ({cluster set} + 32 B push; prep / scatter and the
        // one-workgroup order). Optional: without it blend instances are never admitted.
        VkPushConstantRange apr{VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(AlphaPush)};
        pli.pPushConstantRanges = &apr;
        if (vkdCreatePipelineLayout(dev, &pli, &mAlphaCompLayout, "mClusterAlphaLayout") == VK_SUCCESS)
        {
            VkShaderModule ma = makeModule(dev, kWorldClusterAlphaSpv, kWorldClusterAlphaSpv_bytes, "kWorldClusterAlphaSpv");
            VkShaderModule mo = makeModule(dev, kWorldClusterAlphaOrderSpv, kWorldClusterAlphaOrderSpv_bytes,
                                           "kWorldClusterAlphaOrderSpv");
            if (ma) mAlphaPassPipe = createComputePipeline(dev, ma, mAlphaCompLayout);
            if (mo) mAlphaOrderPipe = createComputePipeline(dev, mo, mAlphaCompLayout);
            if (ma) vkDestroyShaderModule(dev, ma, nullptr);
            if (mo) vkDestroyShaderModule(dev, mo, nullptr);
        }
        else
            mAlphaCompLayout = VK_NULL_HANDLE;
    }
    // Draw layouts: {draw0, draw1, cluster}. Tiers 0 / 1 keep the renderer's push range
    // (VS|FS, 16 B): layout-compatible with its draw layout for sets 0 / 1 and the push.
    if (env.drawSet0 && env.drawSet1)
    {
        const VkDescriptorSetLayout sets[3] = {env.drawSet0, env.drawSet1, cl};
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, env.drawPushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3;
        pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkdCreatePipelineLayout(dev, &pli, &mDrawLayout, "mClusterDrawLayout") != VK_SUCCESS)
            mDrawLayout = VK_NULL_HANDLE;
        // Legacy-skin skip: the captures' ray-query lit variant, {draw0, draw1, RT set, cluster}
        // (push-compatible with the renderer's mDrawLayoutRt for sets 0..2 and the push).
        const VkDescriptorSetLayout rsets[4] = {env.drawSet0, env.drawSet1, env.rtSetLayout, cl};
        VkPipelineLayoutCreateInfo rli = pli;
        rli.setLayoutCount = 4;
        rli.pSetLayouts = rsets;
        if (env.rtSetLayout && vkdCreatePipelineLayout(dev, &rli, &mDrawLayoutRt, "mClusterDrawLayoutRt") != VK_SUCCESS)
            mDrawLayoutRt = VK_NULL_HANDLE;
#ifdef VK_EXT_mesh_shader
        if (mMesh)
        {
            pr.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
            if (vkdCreatePipelineLayout(dev, &pli, &mMeshLayout, "mClusterMeshLayout") != VK_SUCCESS)
                mMeshLayout = VK_NULL_HANDLE;
            if (env.rtSetLayout && vkdCreatePipelineLayout(dev, &rli, &mMeshLayoutRt, "mClusterMeshLayoutRt") != VK_SUCCESS)
                mMeshLayoutRt = VK_NULL_HANDLE;
            mDrawMeshTasksIndirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(
                vkGetDeviceProcAddr(dev, "vkCmdDrawMeshTasksIndirectEXT"));
        }
#endif
    }

    // Fixed buffers: the counters (+ dedupe stamps), the per-slot views and readbacks, and
    // the smallest arena.
    bool ok = ensureCounters() && ensureArena(kMinArena, kMinRigCap);
    for (uint32_t s = 0; s < kSlots && ok; ++s)
    {
        VkDeviceContext::SoftAllocScope soft(&ctx);
        {
            VkMemScope mem(VKMEM_CULL);
            mViews[s] = ctx.createBuffer(kViewsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        {
            VkMemScope mem(VKMEM_READBACK);
            mReadback[s] = ctx.createBuffer(kReadbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        ok = mViews[s].mapped && mReadback[s].mapped;
        if (ok)
        {
            std::memset(mViews[s].mapped, 0, kViewsBytes);
            std::memset(mReadback[s].mapped, 0, kReadbackBytes);
            mOwner.setBuffer(CLB_VIEWS, mViews[s], 0, VK_WHOLE_SIZE, s);
        }
    }
    if (!ok)
    {
        LL_WARNS("Vulkan") << "Cluster LOD select: buffers unavailable (VRAM)" << LL_ENDL;
        return false;
    }
    // #702 integration: the pick list exists from the start (80 KB), so every slot's set has
    // binding 14 before any pick reads it (a failure retries at the next pick, kFrameSlots later).
    if (mPickFilterPipe) ensurePickList();
    createDrawPipelines();
    mReady = true;
    LL_INFOS("Vulkan") << "Cluster LOD select: ready (mesh-shader stages " << (mMesh ? "yes" : "no")
                       << ", draw layouts " << (mDrawLayout ? "yes" : "no") << ", G-buffer template "
                       << (env.gbufValid ? "yes" : "no: selection only") << ")" << LL_ENDL;
    return true;
}

// Every raster tier the device has (so RenderVKClusterForceTier switches live) x {material,
// debug} FS x {cull back, cull none}, against the renderer's G-buffer template, compiled on
// background threads (vk_pipeline.h batch): the draws start once pollPipelines sees them.
void LLCleanClusterSelectVK::createDrawPipelines()
{
    const LLClusterRenderEnv& env = mOwner.env();
    if (!env.gbufValid || !mDrawLayout) return;   // no deferred lighting: selection only
    const VkDevice dev = mOwner.ctx().device();
    const int32_t native = mOwner.caps().native[CLCAP_RASTER];
    VkShaderModule vPull = makeModule(dev, kWorldClusterVertPullSpv, kWorldClusterVertPullSpv_bytes, "kWorldClusterVertPullSpv");
    VkShaderModule vU8 = native >= CLT_RASTER_MDI_U8
                       ? makeModule(dev, kWorldClusterVertU8Spv, kWorldClusterVertU8Spv_bytes, "kWorldClusterVertU8Spv")
                       : VK_NULL_HANDLE;
    VkShaderModule task = VK_NULL_HANDLE, mesh = VK_NULL_HANDLE;
#ifdef VK_EXT_mesh_shader
    if (native >= CLT_RASTER_MESH && mMesh && mMeshLayout && mDrawMeshTasksIndirect)
#else
    if (false)
#endif
    {
        task = makeModule(dev, kWorldClusterTaskSpv, kWorldClusterTaskSpv_bytes, "kWorldClusterTaskSpv");
        mesh = makeModule(dev, kWorldClusterMeshSpv, kWorldClusterMeshSpv_bytes, "kWorldClusterMeshSpv");
    }
    // RenderVKDepthOnlyVS: the depth flavour of each tier's stage (gl_Position + the alpha test's
    // inputs; every cluster depth pass alpha-tests). The task stage is shared.
    VkShaderModule vPullD = vPull ? makeModule(dev, kWorldClusterVertPullDepthSpv, kWorldClusterVertPullDepthSpv_bytes,
                                               "kWorldClusterVertPullDepthSpv")
                                  : VK_NULL_HANDLE;
    VkShaderModule vU8D = vU8 ? makeModule(dev, kWorldClusterVertU8DepthSpv, kWorldClusterVertU8DepthSpv_bytes,
                                           "kWorldClusterVertU8DepthSpv")
                              : VK_NULL_HANDLE;
    VkShaderModule meshD = mesh ? makeModule(dev, kWorldClusterMeshDepthSpv, kWorldClusterMeshDepthSpv_bytes,
                                             "kWorldClusterMeshDepthSpv")
                                : VK_NULL_HANDLE;
    // The SLIM pre-pass flavours (the normal / motion pre-pass twins, same switch): [0] + the
    // normal frame, [1] + normal frame + motion, [2] + motion only (pre-pass variants 1 / 2 / 3).
    VkShaderModule vPullS[3] = {}, vU8S[3] = {}, meshS[3] = {};
    {
        static const uint32_t* const kPull[3] = {kWorldClusterVertPullNormalSpv, kWorldClusterVertPullNormalMotionSpv,
                                                 kWorldClusterVertPullMotionSpv};
        static const size_t kPullB[3] = {kWorldClusterVertPullNormalSpv_bytes, kWorldClusterVertPullNormalMotionSpv_bytes,
                                         kWorldClusterVertPullMotionSpv_bytes};
        static const uint32_t* const kU8[3] = {kWorldClusterVertU8NormalSpv, kWorldClusterVertU8NormalMotionSpv,
                                               kWorldClusterVertU8MotionSpv};
        static const size_t kU8B[3] = {kWorldClusterVertU8NormalSpv_bytes, kWorldClusterVertU8NormalMotionSpv_bytes,
                                       kWorldClusterVertU8MotionSpv_bytes};
        static const uint32_t* const kMesh[3] = {kWorldClusterMeshNormalSpv, kWorldClusterMeshNormalMotionSpv,
                                                 kWorldClusterMeshMotionSpv};
        static const size_t kMeshB[3] = {kWorldClusterMeshNormalSpv_bytes, kWorldClusterMeshNormalMotionSpv_bytes,
                                         kWorldClusterMeshMotionSpv_bytes};
        static const char* const kNm[3][3] = {
            {"kWorldClusterVertPullNormalSpv", "kWorldClusterVertPullNormalMotionSpv", "kWorldClusterVertPullMotionSpv"},
            {"kWorldClusterVertU8NormalSpv", "kWorldClusterVertU8NormalMotionSpv", "kWorldClusterVertU8MotionSpv"},
            {"kWorldClusterMeshNormalSpv", "kWorldClusterMeshNormalMotionSpv", "kWorldClusterMeshMotionSpv"}};
        for (int k = 0; k < 3; ++k)
        {
            if (vPull) vPullS[k] = makeModule(dev, kPull[k], kPullB[k], kNm[0][k]);
            if (vU8)   vU8S[k]   = makeModule(dev, kU8[k], kU8B[k], kNm[1][k]);
            if (mesh)  meshS[k]  = makeModule(dev, kMesh[k], kMeshB[k], kNm[2][k]);
        }
    }
    // A depth pipeline's twin: the same desc with its stage swapped for the depth flavour
    // (named "+depthVS" for the batch report / RenderVKPipelineStats), or (slim 0..2) for a
    // slim pre-pass flavour ("+normalVS" / "+normalMotionVS" / "+motionVS").
    const auto depthTwin = [&](const GraphicsPipelineDesc& d, VkPipeline* out, const char* nm, int slim = -1)
    {
        static const char* const kSuffix[3] = {"+normalVS", "+normalMotionVS", "+motionVS"};
        GraphicsPipelineDesc t = d;
        if (t.mesh) t.mesh = t.mesh == mesh ? (slim < 0 ? meshD : meshS[slim]) : VK_NULL_HANDLE;
        else        t.vert = t.vert == vPull ? (slim < 0 ? vPullD : vPullS[slim])
                           : t.vert == vU8 ? (slim < 0 ? vU8D : vU8S[slim]) : VK_NULL_HANDLE;
        if (!(t.mesh ? t.mesh : t.vert)) return;
        const std::string tn = std::string(nm) + (slim < 0 ? "+depthVS" : kSuffix[slim]);
        createGraphicsPipelineInto(dev, t, out, tn.c_str());
    };
    VkShaderModule fs[2] = {
        makeModule(dev, kWorldFragGbufClusterSpv, kWorldFragGbufClusterSpv_bytes, "kWorldFragGbufClusterSpv"),
        makeModule(dev, kWorldClusterDebugFragSpv, kWorldClusterDebugFragSpv_bytes, "kWorldClusterDebugFragSpv")};
    static const char* const kNames[3][2][2] = {
        {{"clusterPull", "clusterPullDS"}, {"clusterPullDbg", "clusterPullDbgDS"}},
        {{"clusterU8", "clusterU8DS"}, {"clusterU8Dbg", "clusterU8DbgDS"}},
        {{"clusterMesh", "clusterMeshDS"}, {"clusterMeshDbg", "clusterMeshDbgDS"}}};
    {
        PipelineBatchScope batch(dev);
        for (int32_t t = 0; t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
        {
            GraphicsPipelineDesc d = env.gbufDesc;
            d.bindings.clear();
            d.attributes.clear();
            d.vert = d.task = d.mesh = VK_NULL_HANDLE;
            d.layout = mDrawLayout;
            if (t == CLT_RASTER_MDI_PULL)
            {
                if (!vPull) continue;
                d.vert = vPull;
            }
            else if (t == CLT_RASTER_MDI_U8)
            {
                if (!vU8) continue;
                d.vert = vU8;
                // The page pool as the vertex stream: the static path's quantized QVertex (20 B).
                d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
            }
            else
            {
                if (!task || !mesh) continue;
                d.task = task;
                d.mesh = mesh;
                d.layout = mMeshLayout;
            }
            // The cluster draws are not in the depth pre-pass: they test AND write depth
            // (reversed-Z GEQUAL via reverseZ), and the material FS alpha-tests itself.
            d.depthTest = true;
            d.depthWrite = true;
            d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
            d.reverseZ = env.reverseZ;
            d.dynamicShadingRate = false;
            d.fragSpec = nullptr;   // (neither FS has specialization constants; never inherit a pointer)
            for (uint32_t f = 0; f < 2; ++f)
            {
                if (!fs[f]) continue;
                d.frag = fs[f];
                for (uint32_t b = 0; b < 2; ++b)
                {
                    d.cullMode = b == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                    createGraphicsPipelineInto(dev, d, &mDrawPipe[t][f][b], kNames[t][f][b]);
                }
            }
        }
        // #749 DEPTH PRE-PASS twins: the renderer's four pre-pass attachment sets (depth only
        // in pass 1's scope / + AO normal / + motion + no-history mask / motion + no-history
        // mask without normals: RenderVKPrepassDepthOnlyDeferred), same VS modules as the
        // G-buffer draws (invariant gl_Position: the G-buffer draw's LEQUAL keeps its depth).
        // The mask FS variants for every cluster (no opaque / mask bucket split): the lit FS's
        // exact discards; an opaque material discards nothing.
        if (env.preValid)
        {
            VkShaderModule pfs[4] = {
                makeModule(dev, kWorldPrepassMaskFragSpv, kWorldPrepassMaskFragSpv_bytes, "kWorldPrepassMaskFragSpv"),
                makeModule(dev, kWorldPrepassNormalMaskFragSpv, kWorldPrepassNormalMaskFragSpv_bytes,
                           "kWorldPrepassNormalMaskFragSpv"),
                makeModule(dev, kWorldPrepassMotionMaskFragSpv, kWorldPrepassMotionMaskFragSpv_bytes,
                           "kWorldPrepassMotionMaskFragSpv"),
                makeModule(dev, kWorldPrepassMotionOnlyMaskFragSpv, kWorldPrepassMotionOnlyMaskFragSpv_bytes,
                           "kWorldPrepassMotionOnlyMaskFragSpv")};
            static const char* const kPreNames[3][4][2] = {
                {{"clPrePull", "clPrePullDS"}, {"clPreNrmPull", "clPreNrmPullDS"}, {"clPreMvPull", "clPreMvPullDS"},
                 {"clPreMoPull", "clPreMoPullDS"}},
                {{"clPreU8", "clPreU8DS"}, {"clPreNrmU8", "clPreNrmU8DS"}, {"clPreMvU8", "clPreMvU8DS"},
                 {"clPreMoU8", "clPreMoU8DS"}},
                {{"clPreMesh", "clPreMeshDS"}, {"clPreNrmMesh", "clPreNrmMeshDS"}, {"clPreMvMesh", "clPreMvMeshDS"},
                 {"clPreMoMesh", "clPreMoMeshDS"}}};
            for (int32_t t = 0; t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
                for (uint32_t pv = 0; pv < 4; ++pv)
                {
                    if (!pfs[pv]) continue;
                    GraphicsPipelineDesc d = env.preDesc[pv];
                    d.bindings.clear();
                    d.attributes.clear();
                    d.vert = d.task = d.mesh = VK_NULL_HANDLE;
                    d.layout = mDrawLayout;
                    if (t == CLT_RASTER_MDI_PULL)
                    {
                        if (!vPull) continue;
                        d.vert = vPull;
                    }
                    else if (t == CLT_RASTER_MDI_U8)
                    {
                        if (!vU8) continue;
                        d.vert = vU8;
                        d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                        d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                        {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                        {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                        {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
                    }
                    else
                    {
                        if (!task || !mesh) continue;
                        d.task = task;
                        d.mesh = mesh;
                        d.layout = mMeshLayout;
                    }
                    d.reverseZ = env.reverseZ;
                    d.fragSpec = nullptr;
                    d.frag = pfs[pv];
                    for (uint32_t b = 0; b < 2; ++b)
                    {
                        d.cullMode = b == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                        createGraphicsPipelineInto(dev, d, &mPrePipe[t][pv][b], kPreNames[t][pv][b]);
                        // (the depth flavour / the slim normal / normal + motion / motion flavour)
                        depthTwin(d, &mPreDvPipe[t][pv][b], kPreNames[t][pv][b], (int)pv - 1);
                    }
                }
            for (VkShaderModule m : pfs)
                if (m) destroyShaderModuleDeferred(dev, m);
        }
        // #702 integration: the SECONDARY passes (sun cascades, projector tiles, captures),
        // against the renderer's templates of those passes. Shadows: the shadow alpha-test FS
        // for every cluster (no opaque / mask split, as the pre-pass); projector tiles cast both
        // faces; captures: the plain lit FS.
        {
            const GraphicsPipelineDesc* tmpl[3] = {env.shadowValid ? &env.shadowDesc : nullptr,
                                                   env.projValid ? &env.projDesc : nullptr,
                                                   env.captureValid ? &env.captureDesc : nullptr};
            VkShaderModule sfs = (tmpl[0] || tmpl[1])
                               ? makeModule(dev, kWorldShadowMaskFragSpv, kWorldShadowMaskFragSpv_bytes, "kWorldShadowMaskFragSpv")
                               : VK_NULL_HANDLE;
            VkShaderModule lfs = tmpl[2] ? makeModule(dev, kWorldFragSpv, kWorldFragSpv_bytes, "kWorldFragSpv") : VK_NULL_HANDLE;
            static const char* const kPassNames[3][3][2] = {
                {{"clSunPull", "clSunPullDS"}, {"clSunU8", "clSunU8DS"}, {"clSunMesh", "clSunMeshDS"}},
                {{"clProjPull", "clProjPullDS"}, {"clProjU8", "clProjU8DS"}, {"clProjMesh", "clProjMeshDS"}},
                {{"clCapPull", "clCapPullDS"}, {"clCapU8", "clCapU8DS"}, {"clCapMesh", "clCapMeshDS"}}};
            for (uint32_t k = 0; k < 3; ++k)
            {
                const VkShaderModule f = k < 2 ? sfs : lfs;
                if (!tmpl[k] || !f) continue;
                for (int32_t t = 0; t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
                {
                    GraphicsPipelineDesc d = *tmpl[k];
                    d.bindings.clear();
                    d.attributes.clear();
                    d.vert = d.task = d.mesh = VK_NULL_HANDLE;
                    d.layout = mDrawLayout;
                    if (t == CLT_RASTER_MDI_PULL)
                    {
                        if (!vPull) continue;
                        d.vert = vPull;
                    }
                    else if (t == CLT_RASTER_MDI_U8)
                    {
                        if (!vU8) continue;
                        d.vert = vU8;
                        d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                        d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                        {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                        {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                        {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
                    }
                    else
                    {
                        if (!task || !mesh) continue;
                        d.task = task;
                        d.mesh = mesh;
                        d.layout = mMeshLayout;
                    }
                    d.fragSpec = nullptr;
                    d.frag = f;
                    for (uint32_t b = 0; b < 2; ++b)
                    {
                        d.cullMode = (b == 0 && k != 1) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                        createGraphicsPipelineInto(dev, d, &mPassPipe[k][t][b], kPassNames[k][t][b]);
                        if (k < 2) depthTwin(d, &mPassDvPipe[k][t][b], kPassNames[k][t][b]);   // (captures shade)
                    }
                }
            }
            for (VkShaderModule m : {sfs, lfs})
                if (m) destroyShaderModuleDeferred(dev, m);
            // Legacy-skin skip: the captures' RAY-QUERY lit variant (cluster set at 3).
            if (tmpl[2] && env.rtSetLayout && mDrawLayoutRt)
            {
                VkShaderModule rfs = makeModule(dev, kWorldFragRtSpv, kWorldFragRtSpv_bytes, "kWorldFragRtSpv");
                VkShaderModule rPull = makeModule(dev, kWorldClusterVertPullRtSpv, kWorldClusterVertPullRtSpv_bytes,
                                                  "kWorldClusterVertPullRtSpv");
                VkShaderModule rU8 = native >= CLT_RASTER_MDI_U8
                                   ? makeModule(dev, kWorldClusterVertU8RtSpv, kWorldClusterVertU8RtSpv_bytes,
                                                "kWorldClusterVertU8RtSpv")
                                   : VK_NULL_HANDLE;
                VkShaderModule rTask = VK_NULL_HANDLE, rMesh = VK_NULL_HANDLE;
                if (task && mesh && mMeshLayoutRt)
                {
                    rTask = makeModule(dev, kWorldClusterTaskRtSpv, kWorldClusterTaskRtSpv_bytes, "kWorldClusterTaskRtSpv");
                    rMesh = makeModule(dev, kWorldClusterMeshRtSpv, kWorldClusterMeshRtSpv_bytes, "kWorldClusterMeshRtSpv");
                }
                static const char* const kCapRtNames[3][2] = {
                    {"clCapRtPull", "clCapRtPullDS"}, {"clCapRtU8", "clCapRtU8DS"}, {"clCapRtMesh", "clCapRtMeshDS"}};
                for (int32_t t = 0; rfs && t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
                {
                    GraphicsPipelineDesc d = *tmpl[2];
                    d.bindings.clear();
                    d.attributes.clear();
                    d.vert = d.task = d.mesh = VK_NULL_HANDLE;
                    d.layout = mDrawLayoutRt;
                    if (t == CLT_RASTER_MDI_PULL)
                    {
                        if (!rPull) continue;
                        d.vert = rPull;
                    }
                    else if (t == CLT_RASTER_MDI_U8)
                    {
                        if (!rU8) continue;
                        d.vert = rU8;
                        d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                        d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                        {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                        {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                        {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
                    }
                    else
                    {
                        if (!rTask || !rMesh) continue;
                        d.task = rTask;
                        d.mesh = rMesh;
                        d.layout = mMeshLayoutRt;
                    }
                    d.fragSpec = nullptr;
                    d.frag = rfs;
                    for (uint32_t b = 0; b < 2; ++b)
                    {
                        d.cullMode = b == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                        createGraphicsPipelineInto(dev, d, &mCapRtPipe[t][b], kCapRtNames[t][b]);
                    }
                }
                for (VkShaderModule m : {rfs, rPull, rU8, rTask, rMesh})
                    if (m) destroyShaderModuleDeferred(dev, m);
            }
        }
        // CLPASS_FORWARD (2026-10-14, snapshot fix): the main view's FORWARD lit pass 1, against the
        // renderer's LIT_RIG template. A frame that does not go deferred (a supersampled snapshot
        // with RT shadows: no RT mask -> forward; RenderVKSnapshotDeferred off; pass 1 not split;
        // debug views) drew NO cluster-path mesh before -- and in the cluster-owns-rigged mode a
        // rigged mesh has no legacy copy, so snapshots lost every avatar. Plain variant = the lit
        // FS with the cluster set at 2; ray-query variant = the RT cluster stages + the ray-query
        // lit FS (RT set at 2, cluster set at 3), exactly as the captures' pair. Depth test + write
        // LEQUAL (reversed-Z): the same invariant VS as the cluster pre-pass, so after it the draw
        // passes its own depth (as the G-buffer twins), and without it it lays depth itself.
        if (env.forwardValid)
        {
            static const char* const kFwdNames[2][3][2] = {
                {{"clFwdPull", "clFwdPullDS"}, {"clFwdU8", "clFwdU8DS"}, {"clFwdMesh", "clFwdMeshDS"}},
                {{"clFwdRtPull", "clFwdRtPullDS"}, {"clFwdRtU8", "clFwdRtU8DS"}, {"clFwdRtMesh", "clFwdRtMeshDS"}}};
            const bool rt_ok = env.rtSetLayout && mDrawLayoutRt;
            VkShaderModule ffs[2] = {
                makeModule(dev, kWorldFragSpv, kWorldFragSpv_bytes, "kWorldFragSpv"),
                rt_ok ? makeModule(dev, kWorldFragRtSpv, kWorldFragRtSpv_bytes, "kWorldFragRtSpv") : VK_NULL_HANDLE};
            VkShaderModule fPullR = VK_NULL_HANDLE, fU8R = VK_NULL_HANDLE, fTaskR = VK_NULL_HANDLE, fMeshR = VK_NULL_HANDLE;
            if (ffs[1])
            {
                fPullR = makeModule(dev, kWorldClusterVertPullRtSpv, kWorldClusterVertPullRtSpv_bytes,
                                    "kWorldClusterVertPullRtSpv");
                if (native >= CLT_RASTER_MDI_U8)
                    fU8R = makeModule(dev, kWorldClusterVertU8RtSpv, kWorldClusterVertU8RtSpv_bytes,
                                      "kWorldClusterVertU8RtSpv");
                if (task && mesh && mMeshLayoutRt)
                {
                    fTaskR = makeModule(dev, kWorldClusterTaskRtSpv, kWorldClusterTaskRtSpv_bytes, "kWorldClusterTaskRtSpv");
                    fMeshR = makeModule(dev, kWorldClusterMeshRtSpv, kWorldClusterMeshRtSpv_bytes, "kWorldClusterMeshRtSpv");
                }
            }
            for (uint32_t rv = 0; rv < 2; ++rv)
            {
                if (!ffs[rv]) continue;
                for (int32_t t = 0; t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
                {
                    GraphicsPipelineDesc d = env.forwardDesc;
                    d.bindings.clear();
                    d.attributes.clear();
                    d.vert = d.task = d.mesh = VK_NULL_HANDLE;
                    d.layout = rv ? mDrawLayoutRt : mDrawLayout;
                    if (t == CLT_RASTER_MDI_PULL)
                    {
                        d.vert = rv ? fPullR : vPull;
                        if (!d.vert) continue;
                    }
                    else if (t == CLT_RASTER_MDI_U8)
                    {
                        d.vert = rv ? fU8R : vU8;
                        if (!d.vert) continue;
                        d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                        d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                        {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                        {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                        {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
                    }
                    else
                    {
                        d.task = rv ? fTaskR : task;
                        d.mesh = rv ? fMeshR : mesh;
                        if (!d.task || !d.mesh) continue;
                        d.layout = rv ? mMeshLayoutRt : mMeshLayout;
                        if (!d.layout) continue;
                    }
                    d.depthTest = true;
                    d.depthWrite = true;
                    d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
                    d.reverseZ = env.reverseZ;
                    d.blend = false;
                    d.dynamicShadingRate = false;
                    d.fragSpec = nullptr;
                    d.frag = ffs[rv];
                    for (uint32_t b = 0; b < 2; ++b)
                    {
                        d.cullMode = b == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                        createGraphicsPipelineInto(dev, d, &mFwdPipe[rv][t][b], kFwdNames[rv][t][b]);
                    }
                }
            }
            for (VkShaderModule m : {ffs[0], ffs[1], fPullR, fU8R, fTaskR, fMeshR})
                if (m) destroyShaderModuleDeferred(dev, m);
        }
        // RIGGED ALPHA-BLEND (the alpha lists): pass 2's blend (plain + ray-query lit FS: the
        // latter with the cluster set at 3), the alpha-blend depth pre-pass, the cascades'
        // translucency layer (depth, colour) and the captures' blend, against the renderer's
        // templates of the legacy rigged alpha pipelines. Each keeps its template's cull state
        // (dynamic: the blend draws set FRONT then BACK, as the legacy two-pass blend).
        {
            const bool rtl = env.rtSetLayout && mDrawLayoutRt && (!mMesh || mMeshLayoutRt);
            // The plain lit FS, not the #731 -DCLEAN_BLEND_TOL twin: avatar alpha keeps the exact
            // hardware depth test of its template (LIT_RIG_ALPHA, #742 / #750).
            VkShaderModule bfs = env.alphaValid ? makeModule(dev, kWorldFragSpv, kWorldFragSpv_bytes, "kWorldFragSpv")
                                                : VK_NULL_HANDLE;
            VkShaderModule bfr = (env.alphaValid && rtl)
                               ? makeModule(dev, kWorldFragRtSpv, kWorldFragRtSpv_bytes, "kWorldFragRtSpv")
                               : VK_NULL_HANDLE;
            VkShaderModule pbf = env.alphaDepthValid
                               ? makeModule(dev, kWorldPrepassBlendFragSpv, kWorldPrepassBlendFragSpv_bytes, "kWorldPrepassBlendFragSpv")
                               : VK_NULL_HANDLE;
            VkShaderModule tdf = env.transValid
                               ? makeModule(dev, kWorldShadowTransDepthFragSpv, kWorldShadowTransDepthFragSpv_bytes,
                                            "kWorldShadowTransDepthFragSpv")
                               : VK_NULL_HANDLE;
            VkShaderModule tcf = env.transValid
                               ? makeModule(dev, kWorldShadowTransColorFragSpv, kWorldShadowTransColorFragSpv_bytes,
                                            "kWorldShadowTransColorFragSpv")
                               : VK_NULL_HANDLE;
            VkShaderModule cfs = env.captureAlphaValid ? makeModule(dev, kWorldFragSpv, kWorldFragSpv_bytes, "kWorldFragSpv")
                                                       : VK_NULL_HANDLE;
            VkShaderModule aPullR = VK_NULL_HANDLE, aU8R = VK_NULL_HANDLE, aTaskR = VK_NULL_HANDLE, aMeshR = VK_NULL_HANDLE;
            if (bfr)
            {
                aPullR = makeModule(dev, kWorldClusterVertPullRtSpv, kWorldClusterVertPullRtSpv_bytes, "kWorldClusterVertPullRtSpv");
                if (native >= CLT_RASTER_MDI_U8)
                    aU8R = makeModule(dev, kWorldClusterVertU8RtSpv, kWorldClusterVertU8RtSpv_bytes, "kWorldClusterVertU8RtSpv");
                if (task && mesh && mMeshLayoutRt)
                {
                    aTaskR = makeModule(dev, kWorldClusterTaskRtSpv, kWorldClusterTaskRtSpv_bytes, "kWorldClusterTaskRtSpv");
                    aMeshR = makeModule(dev, kWorldClusterMeshRtSpv, kWorldClusterMeshRtSpv_bytes, "kWorldClusterMeshRtSpv");
                }
            }
            // The cluster stages of tier t into d (rt: the cluster set at 3). False = missing.
            const auto stages = [&](GraphicsPipelineDesc& d, int32_t t, bool rt) -> bool
            {
                d.bindings.clear();
                d.attributes.clear();
                d.vert = d.task = d.mesh = VK_NULL_HANDLE;
                d.layout = rt ? mDrawLayoutRt : mDrawLayout;
                d.fragSpec = nullptr;
                if (t == CLT_RASTER_MDI_PULL)
                {
                    d.vert = rt ? aPullR : vPull;
                    return d.vert != VK_NULL_HANDLE;
                }
                if (t == CLT_RASTER_MDI_U8)
                {
                    d.vert = rt ? aU8R : vU8;
                    d.bindings = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
                    d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                                    {1, 0, VK_FORMAT_R16G16_SINT, 8u},
                                    {2, 0, VK_FORMAT_R16G16_SNORM, 12u},
                                    {3, 0, VK_FORMAT_R16G16_SNORM, 16u}};
                    return d.vert != VK_NULL_HANDLE;
                }
                d.task = rt ? aTaskR : task;
                d.mesh = rt ? aMeshR : mesh;
                d.layout = rt ? mMeshLayoutRt : mMeshLayout;
                return d.task && d.mesh && d.layout;
            };
            static const char* const kAlNames[3][2] = {{"clAlphaPull", "clAlphaPullRt"}, {"clAlphaU8", "clAlphaU8Rt"},
                                                       {"clAlphaMesh", "clAlphaMeshRt"}};
            static const char* const kAlDepth[3] = {"clAlphaDepthPull", "clAlphaDepthU8", "clAlphaDepthMesh"};
            static const char* const kTrans[3][2] = {{"clTransDPull", "clTransCPull"}, {"clTransDU8", "clTransCU8"},
                                                     {"clTransDMesh", "clTransCMesh"}};
            static const char* const kCapAl[3] = {"clCapAlphaPull", "clCapAlphaU8", "clCapAlphaMesh"};
            for (int32_t t = 0; t <= std::min<int32_t>(native, CLT_RASTER_MESH); ++t)
            {
                for (uint32_t rt = 0; rt < 2; ++rt)
                {
                    const VkShaderModule f = rt ? bfr : bfs;
                    if (!f) continue;
                    GraphicsPipelineDesc d = env.alphaDesc;
                    if (!stages(d, t, rt != 0)) continue;
                    d.frag = f;
                    createGraphicsPipelineInto(dev, d, &mAlphaPipe[t][rt], kAlNames[t][rt]);
                }
                if (pbf)
                {
                    GraphicsPipelineDesc d = env.alphaDepthDesc;
                    if (stages(d, t, false))
                    {
                        d.frag = pbf;
                        createGraphicsPipelineInto(dev, d, &mAlphaDepthPipe[t], kAlDepth[t]);
                        depthTwin(d, &mAlphaDepthDvPipe[t], kAlDepth[t]);
                    }
                }
                for (uint32_t c = 0; c < 2 && tdf && tcf; ++c)
                {
                    GraphicsPipelineDesc d = env.transDesc[c];
                    if (!stages(d, t, false)) continue;
                    d.frag = c ? tcf : tdf;
                    createGraphicsPipelineInto(dev, d, &mTransPipe[t][c], kTrans[t][c]);
                    depthTwin(d, &mTransDvPipe[t][c], kTrans[t][c]);
                }
                if (cfs)
                {
                    GraphicsPipelineDesc d = env.captureAlphaDesc;
                    if (stages(d, t, false))
                    {
                        d.frag = cfs;
                        createGraphicsPipelineInto(dev, d, &mCapAlphaPipe[t], kCapAl[t]);
                    }
                }
            }
            for (VkShaderModule m : {bfs, bfr, pbf, tdf, tcf, cfs, aPullR, aU8R, aTaskR, aMeshR})
                if (m) destroyShaderModuleDeferred(dev, m);
        }
        for (VkShaderModule m : {vPull, vU8, task, mesh, fs[0], fs[1], vPullD, vU8D, meshD, vPullS[0], vPullS[1], vPullS[2],
                                 vU8S[0], vU8S[1], vU8S[2], meshS[0], meshS[1], meshS[2]})
            if (m) destroyShaderModuleDeferred(dev, m);
        mPipeJob = pipelineBatchFlushAsync(dev);
    }
    if (!mPipeJob) pollPipelines();
}

void LLCleanClusterSelectVK::pollPipelines()
{
    if (mPipeJob)
    {
        if (!pipelineJobDone(mPipeJob)) return;
        pipelineJobFinish(mPipeJob);
        mPipeJob = nullptr;
    }
    if (mPipesReady) return;
    mPipesReady = true;
    std::string have;
    for (int32_t t = 0; t < 3; ++t)
        if (mDrawPipe[t][0][0] && mDrawPipe[t][0][1] && mDrawPipe[t][1][0] && mDrawPipe[t][1][1])
            have += (have.empty() ? "" : ", ") + std::string(LLCleanClusterCaps::tierName(CLCAP_RASTER, t));
    std::string passes;
    static const char* const kPassName[3] = {"sun", "proj", "capture"};
    for (uint32_t k = 0; k < 3; ++k)
        for (int32_t t = 2; t >= 0; --t)
            if (mPassPipe[k][t][0] && mPassPipe[k][t][1])
            {
                passes += (passes.empty() ? "" : ", ") + std::string(kPassName[k]) + " t" + std::to_string(t);
                break;
            }
    for (int32_t t = 2; t >= 0; --t)
        if (mCapRtPipe[t][0] && mCapRtPipe[t][1])
        {
            passes += (passes.empty() ? "" : ", ") + std::string("capture-rt t") + std::to_string(t);
            break;
        }
    for (int32_t t = 2; t >= 0; --t)
        if (mFwdPipe[0][t][0] && mFwdPipe[0][t][1])
        {
            passes += (passes.empty() ? "" : ", ") + std::string("forward t") + std::to_string(t)
                    + (mFwdPipe[1][t][0] && mFwdPipe[1][t][1] ? "+rt" : "");
            break;
        }
    for (int32_t t = 2; t >= 0; --t)
        if (mAlphaPipe[t][0])
        {
            passes += (passes.empty() ? "" : ", ") + std::string("alpha t") + std::to_string(t)
                    + (mAlphaPipe[t][1] ? "+rt" : "") + (mAlphaDepthPipe[t] ? " depth" : "")
                    + (mTransPipe[t][0] && mTransPipe[t][1] ? " trans" : "") + (mCapAlphaPipe[t] ? " capture" : "")
                    + (mAlphaPassPipe && mAlphaOrderPipe ? " order" : " NO ORDER");
            break;
        }
    LL_INFOS("Vulkan") << "Cluster LOD select: draw pipelines ready for raster tiers: "
                       << (have.empty() ? std::string("none (selection only)") : have) << "; secondary passes: "
                       << (passes.empty() ? std::string("none") : passes) << LL_ENDL;
}

void LLCleanClusterSelectVK::destroy()
{
    mReady = false;
    VkDeviceContext& ctx = mOwner.ctx();
    const VkDevice dev = ctx.device();
    if (mPipeJob) { pipelineJobFinish(mPipeJob); mPipeJob = nullptr; }
    mPickJob.wait();   // (#782 audit: it references mPickLayout, destroyed below)
    if (mPickStage) { vkDestroyPipeline(dev, mPickStage, nullptr); mPickStage = VK_NULL_HANDLE; }
    for (auto& a : mDrawPipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mPrePipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mPassPipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mCapRtPipe)
        for (VkPipeline& p : a)
            if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mFwdPipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mAlphaPipe)
        for (VkPipeline& p : a)
            if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mTransPipe)
        for (VkPipeline& p : a)
            if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mAlphaDepthPipe)
        if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mCapAlphaPipe)
        if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    // RenderVKDepthOnlyVS twins.
    for (auto& a : mPreDvPipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mPassDvPipe)
        for (auto& b : a)
            for (VkPipeline& p : b)
                if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mAlphaDepthDvPipe)
        if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (auto& a : mTransDvPipe)
        for (VkPipeline& p : a)
            if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline* p : {&mPickFilterPipe, &mPickDrawPipe, &mRigCullPipe, &mAlphaPassPipe, &mAlphaOrderPipe})
        if (*p) { vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&mPickCompLayout, &mPickLayout, &mRigCullLayout, &mAlphaCompLayout})
        if (*l) { vkDestroyPipelineLayout(dev, *l, nullptr); *l = VK_NULL_HANDLE; }
    mPickOutDsl = VK_NULL_HANDLE;
    if (mSelectPipe) { vkDestroyPipeline(dev, mSelectPipe, nullptr); mSelectPipe = VK_NULL_HANDLE; }
    if (mFinalizePipe) { vkDestroyPipeline(dev, mFinalizePipe, nullptr); mFinalizePipe = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&mCompLayout, &mDrawLayout, &mMeshLayout, &mDrawLayoutRt, &mMeshLayoutRt})
        if (*l) { vkDestroyPipelineLayout(dev, *l, nullptr); *l = VK_NULL_HANDLE; }
    for (uint32_t s = 0; s < kSlots; ++s)
        for (GpuBuffer* b : {&mInst[s], &mViews[s], &mOwned[s], &mReadback[s]})
            if (b->buffer) ctx.destroyBuffer(*b);
    for (GpuBuffer* b : {&mSelected, &mDraws, &mCounters, &mPickList, &mInstCut})
        if (b->buffer) ctx.destroyBuffer(*b);
    mInstCutCap = 0;
    mRtSummary = false;
    mPipesReady = false;
    dropCandidates();
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (mChangeLogOn) LLCleanInstanceStore::instance().setChangeLog(false);
#endif
    mChangeLogOn = false;
    mCpuInst.clear();
    mCpuInst.shrink_to_fit();
    mCpuOwned.clear();
    mCpuOwned.shrink_to_fit();
}

// =============================================================================================
// buffers
// =============================================================================================
bool LLCleanClusterSelectVK::ensureCounters()
{
    if (mCounters.buffer) return true;
    VkDeviceContext& ctx = mOwner.ctx();
    VkMemScope mem(VKMEM_CULL);
    VkDeviceContext::SoftAllocScope soft(&ctx);
    const VkDeviceSize bytes = ((VkDeviceSize)kDedupBase + kMaxVirtualPages) * 4;
    mCounters = ctx.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mCounters.buffer) return false;
    mCountersFresh = true;
    mOwner.setBuffer(CLB_COUNTERS, mCounters);
    return true;
}

// The draw buffer's bytes: the per-view records + aux words + the rig lists + the culled rig list
// + (rigged alpha lists) the staging region and kAlOuts output lists of alpha_cap records, the
// staged tags (2 words each), the entry table and its sort keys (alpha_inst entries).
VkDeviceSize LLCleanClusterSelectVK::drawsBytes(uint32_t arena, uint32_t rig_cap, uint32_t alpha_cap, uint32_t alpha_inst)
{
    const VkDeviceSize rig_base = ((VkDeviceSize)arena * 2 * kDrawWords + arena + kDrawWords - 1) / kDrawWords;
    const VkDeviceSize alpha_base = rig_base + (VkDeviceSize)(kRigLists + 1) * 2 * rig_cap;
    const VkDeviceSize records = alpha_base + (VkDeviceSize)(1 + kAlOuts) * alpha_cap;
    const VkDeviceSize words = records * kDrawWords + 2ull * alpha_cap + (VkDeviceSize)(kAlEntryWords + 4) * alpha_inst;
    return words * 4;
}

// The selected arena + the draw records (2 buckets x arena x 5 words) + the aux words + (#702
// integration) the shared rig lists (4 x rig_cap records, from record rigBaseRecords()) + (#748
// perf pass) the MDI tiers' per-view culled rig list (2 x rig_cap records, rigCulledRecords()) +
// the rigged alpha lists (alphaBaseRecords(), drawsBytes). 0 = keep the current alpha sizes.
bool LLCleanClusterSelectVK::ensureArena(uint32_t entries, uint32_t rig_cap, uint32_t alpha_cap, uint32_t alpha_inst)
{
    entries = std::clamp(nextPow2(entries), kMinArena, kMaxArena);
    rig_cap = std::clamp(nextPow2(rig_cap), kMinRigCap, kMaxArena);
    alpha_cap = std::clamp(nextPow2(alpha_cap ? alpha_cap : mAlphaCap), kMinAlphaCap, kMaxArena);
    alpha_inst = std::clamp(nextPow2(alpha_inst ? alpha_inst : mAlphaInst), kMinAlphaInst, kMaxAlphaInst);
    if (entries == mArena && rig_cap == mRigCap && alpha_cap == mAlphaCap && alpha_inst == mAlphaInst && mSelected.buffer
        && mDraws.buffer)
        return true;
    VkDeviceContext& ctx = mOwner.ctx();
    GpuBuffer sel, draws;
    {
        VkMemScope mem(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        sel = ctx.createBuffer((VkDeviceSize)entries * sizeof(ClusterSelected), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (sel.buffer)
            draws = ctx.createBuffer(drawsBytes(entries, rig_cap, alpha_cap, alpha_inst),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!sel.buffer || !draws.buffer)
    {   // soft failure: keep what we have (overflow is counted; retried on the next resize)
        if (sel.buffer) ctx.destroyBuffer(sel);
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD select: arena of " << entries << " entries (rig lists " << rig_cap
                                << ") unavailable (VRAM); keeping " << mArena << " / " << mRigCap << LL_ENDL;
        return mSelected.buffer && mDraws.buffer;
    }
    mOwner.retire(mSelected);
    mOwner.retire(mDraws);
    mSelected = sel;
    mDraws = draws;
    mArena = entries;
    mRigCap = rig_cap;
    mAlphaCap = alpha_cap;
    mAlphaInst = alpha_inst;
    mOwner.setBuffer(CLB_SELECTED, mSelected);
    mOwner.setBuffer(CLB_DRAWS, mDraws);
    return true;
}

bool LLCleanClusterSelectVK::ensureInstances(uint32_t s, uint32_t instances)
{
    VkDeviceContext& ctx = mOwner.ctx();
    if (instances > mInstCap[s] || !mInst[s].buffer)
    {
        const uint32_t cap = std::min(kMaxInstances, std::max(1024u, nextPow2(instances)));
        GpuBuffer b;
        {
            VkMemScope mem(VKMEM_SCENE);
            VkDeviceContext::SoftAllocScope soft(&ctx);
            b = ctx.createBuffer((VkDeviceSize)cap * sizeof(ClusterInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (b.buffer && !b.mapped) ctx.destroyBuffer(b);
        if (!b.buffer) return false;
        mOwner.retire(mInst[s]);
        mInst[s] = b;
        mInstCap[s] = cap;
        mOwner.setBuffer(CLB_INSTANCES, mInst[s], 0, VK_WHOLE_SIZE, s);
    }
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

// #1011 RT CUT SUMMARY: CL_B_INST_CUT, CL_INST_CUT_WORDS words per ClusterInstance row (device;
// written by the main view's selection, read by the cluster RT build of the same frame -- the same
// lifetime as CL_B_SELECTED, so one buffer). Grows in pow2 steps up to kMaxInstances rows (4 MiB),
// never past it; created here, in the CPU frame setup, before any recording. False = none this
// frame (the summary is off: 2b gathers with its mode 11).
bool LLCleanClusterSelectVK::ensureInstCut(uint32_t instances)
{
    if (instances <= mInstCutCap && mInstCut.buffer) return true;
    VkDeviceContext& ctx = mOwner.ctx();
    const uint32_t cap = std::min(kMaxInstances, std::max(1024u, nextPow2(instances)));
    GpuBuffer b;
    {
        VkMemScope mem(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        b = ctx.createBuffer((VkDeviceSize)cap * 8u * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!b.buffer) return false;   // (soft: retried next frame)
    mOwner.retire(mInstCut);
    mInstCut = b;
    mInstCutCap = cap;
    mOwner.setBuffer(CLB_INST_CUT, mInstCut);
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

// The exclusion bitset of a slot (host-visible, read by the main cull by device address).
// False = none: the caller then runs this frame without the exclusion (and draws nothing for
// real), never with a half-built one.
bool LLCleanClusterSelectVK::ensureOwned(uint32_t s, uint32_t bitset_words)
{
    if (bitset_words <= mOwnedWords[s] && mOwned[s].buffer) return true;
    VkDeviceContext& ctx = mOwner.ctx();
    const uint32_t cap = std::max(256u, nextPow2(bitset_words));
    GpuBuffer b;
    {
        VkMemScope mem(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        b = ctx.createBuffer((VkDeviceSize)cap * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             /*deviceAddress*/ true);
    }
    if (b.buffer && (!b.mapped || !b.address)) ctx.destroyBuffer(b);
    if (!b.buffer) return false;
    mOwner.retire(mOwned[s]);
    mOwned[s] = b;
    mOwnedWords[s] = cap;
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

// =============================================================================================
// per frame (CPU)
// =============================================================================================
void LLCleanClusterSelectVK::frameDropped()
{
    mCountersFresh = true;   // the whole-buffer zero fill (dedupe stamps) may never have run
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        mReadbackValid[s] = false;
        mViewsRecorded[s] = 0;
    }
}

bool LLCleanClusterSelectVK::prepassReady() const
{
    const int32_t t = drawTier();
    return mReady && t >= 0 && mPrePipe[t][0][0] && mPrePipe[t][0][1] && mPrePipe[t][1][0] && mPrePipe[t][1][1]
        && mPrePipe[t][2][0] && mPrePipe[t][2][1] && mPrePipe[t][3][0] && mPrePipe[t][3][1];
}

bool LLCleanClusterSelectVK::forwardReady(bool rt) const
{
    const int32_t t = drawTier();
    const uint32_t v = rt ? 1u : 0u;
    return mReady && t >= 0 && mFwdPipe[v][t][0] && mFwdPipe[v][t][1];
}

bool LLCleanClusterSelectVK::peekMainCounts(uint32_t& draws, uint32_t& selected) const
{
    draws = selected = 0;
    if (!mReady || mMainView == kNoView || mMainView >= kMaxViews || !mReadbackValid[mSlot] || !mReadback[mSlot].mapped)
        return false;
    const uint32_t* c = static_cast<const uint32_t*>(mReadback[mSlot].mapped) + (size_t)mMainView * kViewWords;
    draws = c[kCntDraws0] + c[kCntDraws1];
    selected = c[kCntSelClamped];
    return true;
}

void LLCleanClusterSelectVK::roleSummary(std::string& out) const
{
    static const char* const k[CLROLE_COUNT] = {"m", "s", "p", "r", "o"};
    for (uint32_t r = 0; r < CLROLE_COUNT; ++r)
    {
        if (r) out += " ";
        out += std::string(k[r]) + std::to_string(mRoleSel[r]);
        if (mRoleViews[r] > 1) out += "/" + std::to_string(mRoleViews[r]) + "v";
    }
    out += " arena " + std::to_string(mArena);
}

// #702 integration: "st s123/4v r45 p0 | rig sh 6789 cap 2345 (3/2 av, cap 8192) | drew sun 10/600 proj 0
// cap 60/420 | pick 40" -- static draw records per secondary role (sun cascades, mirror, probe
// faces; the cascades' view count), the shared rig lists' records (shadow, capture; avatars in
// each; records per list and bucket), the secondary draws recorded in the last 10 s (sun
// static / rigged, projector tiles rigged, captures = mirror + probe faces static / rigged) and
// the cluster pick filters run.
void LLCleanClusterSelectVK::viewSummary(std::string& out)
{
    out += "st s" + std::to_string(mRoleDraws[CLROLE_SHADOW]);
    if (mRoleViews[CLROLE_SHADOW] > 1) out += "/" + std::to_string(mRoleViews[CLROLE_SHADOW]) + "v";
    out += " r" + std::to_string(mRoleDraws[CLROLE_MIRROR]) + " p" + std::to_string(mRoleDraws[CLROLE_PROBE]);
    out += " | rig sh " + std::to_string(mRigLast[0] + mRigLast[1]) + " cap " + std::to_string(mRigLast[2] + mRigLast[3])
         + " (" + std::to_string(mRigAvatars[0]) + "/" + std::to_string(mRigAvatars[1]) + " av, cap " + std::to_string(mRigCap)
         + ")";
    out += " | drew sun " + std::to_string(mPassDraws[0]) + "/" + std::to_string(mPassDraws[1]) + " proj "
         + std::to_string(mPassDraws[3]) + " cap " + std::to_string(mPassDraws[4]) + "/" + std::to_string(mPassDraws[5])
         + " | pick " + std::to_string(mPickRuns);
    // (#748 perf pass: the MDI tiers' per-view rig-list culls recorded; the mesh tier culls in
    // its task stage and never counts here)
    if (mRigCulls) out += " | rigcull " + std::to_string(mRigCulls);
    mRigCulls = 0;
    std::memcpy(mPassDrawsLast, mPassDraws, sizeof(mPassDraws));
    std::memset(mPassDraws, 0, sizeof(mPassDraws));
    mPickRunsLast = mPickRuns;
    mPickRuns = 0;
}

void LLCleanClusterSelectVK::harvest(uint32_t s)
{
    if (!mReadbackValid[s] || !mReadback[s].mapped) return;
    mReadbackValid[s] = false;
    uint32_t* w = static_cast<uint32_t*>(mReadback[s].mapped);
    uint32_t demand = 0;
    for (uint32_t v = 0; v < kMaxViews; ++v) demand += w[v * kViewWords + kCntSelected];
    // #749: per role -- the largest view's raw demand (slice sizing), views, selected entries.
    // #702 integration: + the draw records per role, the shared rig lists (main view).
    uint32_t rig_demand = 0;
    {
        uint32_t dem[8] = {}, views[8] = {}, sel[8] = {}, draws[8] = {}, rig[4] = {};
        for (uint32_t v = 0; v < mViewsRecorded[s] && v < kMaxViews; ++v)
        {
            const uint32_t* c = w + v * kViewWords;
            const uint32_t r = std::min<uint32_t>(mViewRole[s][v], CLROLE_COUNT - 1);
            dem[r] = std::max(dem[r], c[kCntSelected]);
            sel[r] += c[kCntSelClamped];
            draws[r] += c[kCntDraws0] + c[kCntDraws1];
            ++views[r];
            for (uint32_t k = 0; k < 4; ++k)
            {
                rig[k] += c[kCntRigDraws + k];
                rig_demand = std::max(rig_demand, c[kCntRigDraws + k]);
            }
        }
        for (uint32_t r = 0; r < CLROLE_COUNT; ++r)
        {
            mRoleDemand[r] = dem[r];
            mRoleViews[r] = views[r];
            mRoleSel[r] = sel[r];
            mRoleDraws[r] = draws[r];
        }
        std::memcpy(mRigLast, rig, sizeof(mRigLast));
    }
    mRigDemand = rig_demand;
    // Rigged alpha lists (main view only: the sums are its values).
    {
        uint32_t st = 0, en = 0, ov = 0, mi = 0, mt = 0, c[kAlOuts] = {};
        for (uint32_t v = 0; v < mViewsRecorded[s] && v < kMaxViews; ++v)
        {
            const uint32_t* c0 = w + v * kViewWords;
            st += c0[kCntAlStaged];
            en += c0[kCntAlInst];
            ov += c0[kCntAlOverflow];
            mi += c0[kCntAlMainInst];
            mt += c0[kCntAlMainTris];
            for (uint32_t k = 0; k < kAlOuts; ++k) c[k] += c0[kCntAlCount + k];
        }
        mAlStaged = st;
        mAlEntries = en;
        mAlOverflow = ov;
        mAlMainInst = mi;
        mAlMainTris = mt;
        std::memcpy(mAlCounts, c, sizeof(mAlCounts));
    }
    mDemand = demand;
    mDemandPeak = std::max(mDemandPeak, demand);
    mLastSel = w[kCntSelClamped];   // main view (view 0): valid entries
    mLastDraws[0] = w[kCntDraws0];
    mLastDraws[1] = w[kCntDraws1];
    std::memset(w, 0, kReadbackBytes);   // views not recorded next time read 0
    // Arena sizing: grow at once to the demand (+25 %), shrink by half after ~5 s under a
    // quarter of it (no unbounded growth: kMinArena .. kMaxArena).
    // The shared rig lists (#702 integration) the same way, per list and bucket, from the
    // largest raw rig count (kMinRigCap .. kMaxArena records each).
    const uint64_t want = (uint64_t)demand + demand / 4 + 4096;
    const uint64_t rig_want = (uint64_t)rig_demand + rig_demand / 4 + 1024;
    // (rigged alpha lists: the staged records -- the largest list is at most all of them -- and
    // the alpha entries, +25 %)
    const uint64_t al_want = (uint64_t)mAlStaged + mAlStaged / 4 + 1024;
    const uint64_t ai_want = (uint64_t)mAlEntries + mAlEntries / 4 + 256;
    if ((want > mArena && mArena < kMaxArena) || (rig_want > mRigCap && mRigCap < kMaxArena)
        || (al_want > mAlphaCap && mAlphaCap < kMaxArena) || (ai_want > mAlphaInst && mAlphaInst < kMaxAlphaInst))
    {
        ensureArena((uint32_t)std::min<uint64_t>(std::max<uint64_t>(want, mArena), kMaxArena),
                    (uint32_t)std::min<uint64_t>(std::max<uint64_t>(rig_want, mRigCap), kMaxArena),
                    (uint32_t)std::min<uint64_t>(std::max<uint64_t>(al_want, mAlphaCap), kMaxArena),
                    (uint32_t)std::min<uint64_t>(std::max<uint64_t>(ai_want, mAlphaInst), kMaxAlphaInst));
        mShrinkSince = 0;
    }
    else if ((mArena > kMinArena && (uint64_t)demand * 4 < mArena) || (mRigCap > kMinRigCap && rig_want * 4 < mRigCap)
             || (mAlphaCap > kMinAlphaCap && al_want * 4 < mAlphaCap) || (mAlphaInst > kMinAlphaInst && ai_want * 4 < mAlphaInst))
    {
        if (!mShrinkSince) mShrinkSince = mFrame ? mFrame : 1;
        else if (mFrame > mShrinkSince + 300)
        {
            ensureArena((uint64_t)demand * 4 < mArena ? mArena / 2 : mArena,
                        rig_want * 4 < mRigCap ? mRigCap / 2 : mRigCap,
                        al_want * 4 < mAlphaCap ? mAlphaCap / 2 : mAlphaCap,
                        ai_want * 4 < mAlphaInst ? mAlphaInst / 2 : mAlphaInst);
            mShrinkSince = 0;
        }
    }
    else
        mShrinkSince = 0;
}

void LLCleanClusterSelectVK::beginFrame(const LLClusterFrameIn& in)
{
    mSlot = in.slot % kSlots;
    mFrame = in.frame;
    mViewCount = 0;
    mArenaUsed = 0;
    mMainView = kNoView;
    mSnapshotFrame = in.offscreen || in.snapshot;
    for (ViewRec& v : mViewRecs) v.valid = false;
    if (!mReady) { mInstanceCount = 0; return; }
    pollPipelines();
#if defined(LL_HAS_CLEAN_PIPELINE)
    mDebug = settingDebug();
    mStatic = settingStatic();
    mOcclusion = settingOcclusion();
    mDepthVs = settingDepthVs();
#endif
    harvest(mSlot);
    mViewsRecorded[mSlot] = 0;   // (#749: after the harvest read the last round's roles)
    const int32_t tier = drawTier();
    if (tier != mLastTier)
    {
        if (mPipesReady)
            LL_INFOS("Vulkan") << "Cluster LOD select: raster tier " << tier << " ("
                               << (tier >= 0 ? LLCleanClusterCaps::tierName(CLCAP_RASTER, tier) : "no draw pipelines")
                               << ")" << LL_ENDL;
        mLastTier = tier;
    }

    // Hi-Z (CLV_OCCLUSION): last frame's pyramid pairs with last frame's main view.
    mHizOk = mOcclusion && in.hiz_view && in.hiz_w && in.hiz_h && mOccFrame != ~0ull && mOccFrame + 1 == in.frame;
    if (mHizOk)
    {
        mHizSize[0] = in.hiz_w * 2;
        mHizSize[1] = in.hiz_h * 2;
        uint32_t mips = 1;
        while (mips < 16 && (std::max(in.hiz_w, in.hiz_h) >> mips) > 0) ++mips;
        mHizMips = mips;
    }

    // LEGACY EXCLUSION gate: only while the G-buffer cluster draw demonstrably runs (last
    // frame or the one before), so an excluded instance always has a draw. Static with
    // RenderVKClusterStatic 2; rigged per avatar when 2a switches it (#747, gather).
    const bool gate = tier >= 0 && mLastGbufDraw != 0 && mLastGbufDraw + 2 >= in.frame;
    mExcludeStatic = mStatic >= 2 && gate;
    // Rigged (2026-10-14): in the renderer's cluster-owns-rigged mode the exclusion is not gated
    // on the cluster G-buffer draw having run -- there is no legacy copy to fall back to (a frame
    // without the cluster draw simply draws no cluster-path mesh).
    mOwnsRigged = in.owns_rigged && mOwner.skin().riggedWanted();
    // Rigged alpha-blend rows are admitted only while their lists can be ordered and drawn.
    mAlphaOn = alphaPipesReady() && mAlphaCap != 0 && mAlphaInst != 0;
    mExclude = (gate && (mExcludeStatic || mOwner.skin().riggedWanted())) || mOwnsRigged;
    if (!in.offscreen)
    {
        VK_HITCH_SCOPE("vk_clusterGather");
        gather(in.frame);
    }
    mInstanceCount = (uint32_t)mCpuInst.size();

    if (!ensureInstances(mSlot, mInstanceCount))
    {   // (soft failure: nothing selected this frame -- every instance's legacy draw stays,
        // the exclusion is off with it; retried next frame)
        mInstanceCount = 0;
        mOwnedAny = false;
    }
    if (mInstanceCount)
        std::memcpy(mInst[mSlot].mapped, mCpuInst.data(), (size_t)mInstanceCount * sizeof(ClusterInstance));
    // #1011 RT CUT SUMMARY (its buffer before any recording of this frame)
    bool rt_rank = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    rt_rank = settingRtRank();
#endif
    mRtSummary = rt_rank && mInstanceCount != 0 && ensureInstCut(mInstanceCount);
    if (mOwnedAny && mOwned[mSlot].mapped && mCpuOwned.size() <= mOwnedWords[mSlot])
        std::memcpy(mOwned[mSlot].mapped, mCpuOwned.data(), mCpuOwned.size() * 4);
    else
        mOwnedAny = false;
    // The 1b tail of the views buffer (this slot).
    if (mViews[mSlot].mapped)
    {
        SelectAuxCPU a{};
        std::memcpy(a.occ_view_proj, mOccViewProj, sizeof(a.occ_view_proj));
        a.occ_origin[0] = mOccOrigin[0]; a.occ_origin[1] = mOccOrigin[1]; a.occ_origin[2] = mOccOrigin[2];
        a.hiz[0] = mHizSize[0];
        a.hiz[1] = mHizSize[1];
        a.hiz[2] = mHizOk ? 1u : 0u;
        a.hiz[3] = mHizMips;
        a.debug_mode = mDebug;
        a.flags = mRtSummary ? 1u : 0u;   // CLA_FLAG_RT_SUMMARY (#1011)
        a.aux_base = mArena * 2 * kDrawWords;
        a.tier = (uint32_t)std::max(tier, 0);
        a.rig[0] = rigBaseRecords();   // (#702: rig[2] = 0, no shared cut until the main view's)
        a.rig[1] = mRigCap;
        // Rigged alpha lists (CLI1B_BLEND): their place in CL_B_DRAWS and whether they run.
        a.alpha[0] = alphaBaseRecords();
        a.alpha[1] = mAlphaCap;
        a.alpha[2] = mAlphaInst;
        a.alpha[3] = mAlphaOn ? 1u : 0u;
        std::memcpy(static_cast<uint8_t*>(mViews[mSlot].mapped) + kViewsTailOffset, &a, sizeof(a));
    }
}

// =============================================================================================
// INCREMENTAL GATHER
// =============================================================================================
// The old gather walked EVERY instance-store row every frame (O(store capacity), 224 B rows)
// and, for every eligible row, hashed its cluster key (a per-frame mesh map), asked the mesh
// asset for its best LOD slot (mutex + (uuid, face) hash), the stream for its mesh (key hash),
// looked the object up in gObjectList twice, and recomputed the rest-pose xform, its sigma_max
// (24 power iterations) and the avatar protection per instance. Now:
//  - CANDIDATES: the rows that can take the cluster path (the old per-row filter, row content
//    only: live, not kNever, rigged with a skin or -- with RenderVKClusterStatic -- static and
//    not spinning), walked in row order (the old instance order). Updated from the instance
//    store's row change log (the runs copyDirtyVK consumed: exactly the rows the GPU mirror
//    took, before this frame records); a full rescan on the first frame, a store size change,
//    a log overflow or a RenderVKClusterStatic on / off switch.
//  - Per candidate, cached until its row changes: the resolved key (budgeted, retried, as
//    before), the shared key record (MeshUse: stream directory hint + the groupBestSlot answer
//    while LLCleanMeshAsset::bestSlotGeneration holds), the wearer (AvatarUse) + bind shape
//    (rigged), the static xform / sigma_max / det (rebuilt on a row or render-offset change).
//  - Per AVATAR per frame: one gObjectList lookup, its pose (a version bumped when the position
//    / rotation change: an avatar that did not move reuses every rigged xform as is), its build
//    priority and the controller's tol_scale (lazily, per self flag).
//  - Per KEY per frame: one stream acquire by directory index (no hash): liveness + priority
//    exactly as before.
// What a frame produces (ClusterInstance rows, their order, flags, exclusion bits, the 2a
// hooks) is the old gather's, field for field. Differences: a rigged row's object / skin /
// wearer are re-checked by a rolling revalidation (kRevalPerFrame rows per frame) instead of
// every frame -- rows are released when their object dies, so this only matters for leaked
// rows; a row that leaves and re-enters the candidate set re-resolves its key; an id released
// and reused between two frames with the same LOD group (the same mesh re-attached) now
// re-resolves its key (LLCleanInstanceStore::rowGenerations) -- the old gather kept the dead
// object's key, so such a row never became an instance and held its avatar on legacy.
#if defined(LL_HAS_CLEAN_PIPELINE)
namespace
{
using StoreRow = LLCleanInstanceStore::Instance;
// Rows that never take the cluster path (llcleanclusterselectvk.h "WHAT IS GATHERED").
constexpr U32 kNever = LLCleanClusterSelectVK::kRowNever;
constexpr uint32_t kRevalPerFrame = 32;   // resolved rigged rows re-checked per frame

inline const StoreRow& rowAt(const uint8_t* rows, uint32_t id)
{
    return reinterpret_cast<const StoreRow*>(rows)[id];
}

// 0 not a candidate, 1 rigged, 2 static
inline uint32_t candKind(const StoreRow& r, bool statics)
{
    if (LLCleanClusterSelectVK::rowClusterRoutable(r.flags, r.skin_id, r.lod_group_id)) return 1;
    if (r.lod_group_id == 0xFFFFFFFFu || (r.flags & kNever)) return 0;
    if (r.flags & 1u) return 0;
    if (!statics) return 0;
    if (r.spin_axis[0] != 0.f || r.spin_axis[1] != 0.f || r.spin_axis[2] != 0.f) return 0;
    return 2;
}

// det sign / finiteness / sigma_max of a 3x4 xform (the old per-instance tail, unchanged).
inline void xformFacts(const float xf[12], bool& finite, bool& neg_det, float* lod_scale)
{
    const float m3[9] = {xf[0], xf[1], xf[2], xf[4], xf[5], xf[6], xf[8], xf[9], xf[10]};
    const float det = m3[0] * (m3[4] * m3[8] - m3[5] * m3[7]) - m3[1] * (m3[3] * m3[8] - m3[5] * m3[6])
                    + m3[2] * (m3[3] * m3[7] - m3[4] * m3[6]);
    finite = std::isfinite(det);
    for (int i = 0; i < 12; ++i) finite = finite && std::isfinite(xf[i]);
    neg_det = det < 0.f;
    if (lod_scale) *lod_scale = sigmaMax3(m3);
}

inline bool same3x3(const float a[12], const float b[12])
{
    for (int row = 0; row < 3; ++row)
        if (std::memcmp(&a[row * 4], &b[row * 4], 3 * sizeof(float)) != 0) return false;
    return true;
}
} // namespace

void LLCleanClusterSelectVK::releaseMesh(Cand& c)
{
    if (c.mesh == kNoIdx) return;
    MeshUse& m = mMeshUse[c.mesh];
    if (m.refs && --m.refs == 0)
    {
        mMeshUseIdx.erase(m.key);
        m = MeshUse{};
        mMeshUseFree.push_back(c.mesh);
    }
    c.mesh = kNoIdx;
}

void LLCleanClusterSelectVK::releaseRig(Cand& c)
{
    if (c.avatar != kNoIdx)
    {
        AvatarUse& a = mAvatarUse[c.avatar];
        if (a.refs && --a.refs == 0)
        {
            mAvatarUseIdx.erase(a.id);
            a = AvatarUse{};
            mAvatarUseFree.push_back(c.avatar);
        }
        c.avatar = kNoIdx;
    }
    if (c.bind != kNoIdx)
    {
        mBindFree.push_back(c.bind);
        c.bind = kNoIdx;
    }
    if (c.rigged) c.rec = false;
}

void LLCleanClusterSelectVK::buildStaticRec(Cand& c, const uint8_t* row, const float d[3])
{
    const StoreRow& r = *reinterpret_cast<const StoreRow*>(row);
    // model_matrix: column-major 4x4; translation in the store frame (agent + d).
    float xf[12];
    for (int rr = 0; rr < 3; ++rr)
        for (int k = 0; k < 4; ++k) xf[rr * 4 + k] = r.model_matrix[k * 4 + rr];
    for (int rr = 0; rr < 3; ++rr) xf[rr * 4 + 3] -= d[rr];
    const bool keep_sigma = c.rec && same3x3(xf, c.xform);   // (a pure move keeps sigma_max)
    std::memcpy(c.xform, xf, sizeof(xf));
    xformFacts(c.xform, c.finite, c.neg_det, keep_sigma ? nullptr : &c.lod_scale);
    // build priority numerator: the largest column of the model matrix (the old formula)
    F32 sc = 0.f;
    for (int k = 0; k < 3; ++k)
        sc = std::max(sc, r.model_matrix[k * 4] * r.model_matrix[k * 4] + r.model_matrix[k * 4 + 1] * r.model_matrix[k * 4 + 1]
                              + r.model_matrix[k * 4 + 2] * r.model_matrix[k * 4 + 2]);
    c.size = std::sqrt(sc);
    c.rec = true;
}

void LLCleanClusterSelectVK::markRemove(uint32_t idx)
{
    Cand& c = mCands[idx];
    if (c.dead) return;
    releaseMesh(c);
    releaseRig(c);
    c.dead = true;
    if (c.row < mRowCand.size() && mRowCand[c.row] == idx) mRowCand[c.row] = kNoIdx;
    mRemoved.push_back(idx);
}

// One changed row: membership + the cached snapshot (what the old gather read every frame).
void LLCleanClusterSelectVK::visitRow(const uint8_t* rows, uint32_t id, const float d[3])
{
    const StoreRow& r = rowAt(rows, id);
    const uint32_t want = candKind(r, mStaticCands);
    const uint32_t slot = mRowCand[id];
    if (slot == kPendingAdd)
    {
        if (!want) mRowCand[id] = kNoIdx;   // (applyMembership skips it)
        return;
    }
    if (slot == kNoIdx)
    {
        if (want) { mRowCand[id] = kPendingAdd; mAdds.push_back(id); }
        return;
    }
    Cand& c = mCands[slot];
    if (!want || (want == 1) != c.rigged)
    {
        markRemove(slot);
        if (want) { mRowCand[id] = kPendingAdd; mAdds.push_back(id); }
        return;
    }
    const uint32_t gen = mRowGens ? mRowGens[id] : 0u;
    if (r.lod_group_id != c.lod_group || gen != c.row_gen)
    {   // another geometry (the old per-frame lod_group test) or another key in this id (a
        // release + reuse since the last look): re-resolve the key
        releaseMesh(c);
        releaseRig(c);
        c.lod_group = r.lod_group_id;
        c.row_gen = gen;
        c.state = 0;
    }
    if (c.rigged && r.skin_id != c.skin_id) releaseRig(c);   // (another bind shape / wearer)
    c.flags = r.flags;
    c.skin_id = r.skin_id;
    if (!c.rigged) buildStaticRec(c, reinterpret_cast<const uint8_t*>(&r), d);
}

void LLCleanClusterSelectVK::applyMembership(const uint8_t* rows, const float d[3])
{
    if (!mRemoved.empty())
    {
        mOrder.erase(std::remove_if(mOrder.begin(), mOrder.end(),
                                    [this](const std::pair<uint32_t, uint32_t>& p) { return mCands[p.second].dead; }),
                     mOrder.end());
        for (const uint32_t idx : mRemoved)
        {
            mCands[idx] = Cand{};
            mCandFree.push_back(idx);
        }
        mRemoved.clear();
    }
    if (mAdds.empty()) return;
    std::sort(mAdds.begin(), mAdds.end());
    mAdds.erase(std::unique(mAdds.begin(), mAdds.end()), mAdds.end());
    const size_t old = mOrder.size();
    for (const uint32_t id : mAdds)
    {
        if (id >= mRowCand.size() || mRowCand[id] != kPendingAdd) continue;   // (cancelled)
        uint32_t idx;
        if (!mCandFree.empty()) { idx = mCandFree.back(); mCandFree.pop_back(); }
        else { idx = (uint32_t)mCands.size(); mCands.emplace_back(); }
        const StoreRow& r = rowAt(rows, id);
        Cand& c = mCands[idx];
        c = Cand{};
        c.row = id;
        c.row_gen = mRowGens ? mRowGens[id] : 0u;
        c.lod_group = r.lod_group_id;
        c.flags = r.flags;
        c.skin_id = r.skin_id;
        c.rigged = (r.flags & 1u) != 0u;
        if (!c.rigged) buildStaticRec(c, reinterpret_cast<const uint8_t*>(&r), d);
        mRowCand[id] = idx;
        mOrder.emplace_back(id, idx);
    }
    mAdds.clear();
    // (the adds are sorted by row and unique: one merge keeps the list in row order)
    std::inplace_merge(mOrder.begin(), mOrder.begin() + (std::ptrdiff_t)old, mOrder.end());
}

void LLCleanClusterSelectVK::intake(const uint8_t* rows, uint32_t n, bool full, const float d[3], uint32_t& visited)
{
    visited = 0;
    if (full)
    {
        // rows past the store's end go (trimTail / shutdown), then every row is visited
        for (const auto& p : mOrder)
            if (p.first >= n) markRemove(p.second);
        mRowCand.resize(n, kNoIdx);
        if (mRowCand.capacity() > 2 * (size_t)n + 4096) mRowCand.shrink_to_fit();
        for (uint32_t id = 0; id < n; ++id) visitRow(rows, id, d);
        visited = n;
        ++mGatherFull;
    }
    else
    {
        for (const auto& run : mRuns)
        {
            const uint32_t b = std::min(run.second, n);
            for (uint32_t id = run.first; id < b; ++id) visitRow(rows, id, d);
            visited += b > run.first ? b - run.first : 0u;
        }
    }
    mRuns.clear();
    applyMembership(rows, d);
}

// Key resolution (budgeted by the caller; the old per-row code). False: not resolved (state 2,
// retried after c.retry).
bool LLCleanClusterSelectVK::resolveKey(Cand& c, uint32_t frame32)
{
    c.state = 2;
    c.retry = frame32 + kRetryFrames;
    releaseMesh(c);
    releaseRig(c);
    LLUUID obj;
    S32 face = 0;
    if (!LLCleanInstanceStore::instance().keyForId(c.row, obj, face)) return false;
    LLViewerObject* o = gObjectList.findObject(obj);
    if (!o || o->isDead() || !o->getVolume()) return false;
    const LLVolumeParams& vp = o->getVolume()->getParams();
    if ((vp.getSculptType() & LL_SCULPT_TYPE_MASK) != LL_SCULPT_TYPE_MESH)
    {
        c.retry = frame32 + kRetryFrames * 10;   // prims / image sculpts: not cluster meshes
        return false;
    }
    const LLUUID mesh_id = LLCleanMeshAsset::geometryKey(vp);
    if (LLCleanMeshAsset::instance().getLodGroupId(mesh_id, face) != c.lod_group) return false;   // row of another face
    LLClusterKey key;
    key.mesh_id = mesh_id;
    key.face = face;
    uint32_t mi;
    auto it = mMeshUseIdx.find(key);
    if (it != mMeshUseIdx.end()) mi = it->second;
    else
    {
        if (!mMeshUseFree.empty()) { mi = mMeshUseFree.back(); mMeshUseFree.pop_back(); }
        else { mi = (uint32_t)mMeshUse.size(); mMeshUse.emplace_back(); }
        mMeshUse[mi] = MeshUse{};
        mMeshUse[mi].key = key;
        mMeshUseIdx.emplace(key, mi);
    }
    ++mMeshUse[mi].refs;
    c.mesh = mi;
    c.object = obj;
    c.state = 1;
    return true;
}

// Rigged: the wearer + bind shape of a resolved row (the old per-frame findObject / getAvatar /
// getSkinInfo). The wearer alone gives the build priority; the record needs both.
void LLCleanClusterSelectVK::resolveRig(Cand& c)
{
    releaseRig(c);
    LLViewerObject* o = gObjectList.findObject(c.object);
    if (!o || o->isDead()) return;
    LLVOAvatar* av = o->getAvatar();
    if (!av || av->isDead()) return;
    const LLUUID& aid = av->getID();
    uint32_t ai;
    auto it = mAvatarUseIdx.find(aid);
    if (it != mAvatarUseIdx.end()) ai = it->second;
    else
    {
        if (!mAvatarUseFree.empty()) { ai = mAvatarUseFree.back(); mAvatarUseFree.pop_back(); }
        else { ai = (uint32_t)mAvatarUse.size(); mAvatarUse.emplace_back(); }
        mAvatarUse[ai] = AvatarUse{};
        mAvatarUse[ai].id = aid;
        mAvatarUseIdx.emplace(aid, ai);
    }
    ++mAvatarUse[ai].refs;
    c.avatar = ai;
    if (o->getPCode() != LL_PCODE_VOLUME) return;
    const LLMeshSkinInfo* skin = static_cast<LLVOVolume*>(o)->getSkinInfo();
    if (!skin) return;
    uint32_t bi;
    if (!mBindFree.empty()) { bi = mBindFree.back(); mBindFree.pop_back(); }
    else { bi = (uint32_t)mBinds.size(); mBinds.emplace_back(); }
    std::memcpy(mBinds[bi].data(), skin->mBindShapeMatrix.getF32ptr(), sizeof(float) * 16);   // column-major
    c.bind = bi;
    c.rec = false;
}

// The wearer's per-frame values (once per avatar per frame). Null: not found / dead.
LLCleanClusterSelectVK::AvatarUse* LLCleanClusterSelectVK::avatarPose(uint32_t idx, uint64_t frame, const float cam[3])
{
    AvatarUse& a = mAvatarUse[idx];
    if (a.frame != frame)
    {
        a.frame = frame;
        a.ok = false;
        LLViewerObject* vo = gObjectList.findObject(a.id);
        LLVOAvatar* av = (vo && !vo->isDead()) ? vo->asAvatar() : nullptr;
        if (av)
        {
            const LLVector3 p = av->getPositionAgent();
            const LLQuaternion q = av->getRotation();
            if (!a.posed || std::memcmp(a.pos, p.mV, sizeof(a.pos)) != 0 || std::memcmp(a.quat, q.mQ, sizeof(a.quat)) != 0)
            {
                std::memcpy(a.pos, p.mV, sizeof(a.pos));
                std::memcpy(a.quat, q.mQ, sizeof(a.quat));
                const LLMatrix3 rm(q);
                for (int k = 0; k < 3; ++k)
                    for (int r = 0; r < 3; ++r) a.rm[k * 3 + r] = rm.mMatrix[k][r];
                a.posed = true;
                ++a.pose;
            }
            a.height = av->mBodySize.mV[VZ];
            const F32 dist = std::max(dist_vec(p, LLVector3(cam[0], cam[1], cam[2])), 1.f);
            a.importance = (std::max(a.height, 0.5f) + 0.5f) / dist;
            a.tol_frame[0] = a.tol_frame[1] = ~0ull;
            a.ok = true;
        }
    }
    return a.ok ? &a : nullptr;
}

// Rolling re-check of resolved rigged rows (object alive, same wearer, same bind shape): what
// the old gather looked up every frame. A row that fails drops its wearer / bind (re-resolved on
// its next walk; until it resolves again it is no instance, as before).
void LLCleanClusterSelectVK::revalidate(uint32_t budget)
{
    const uint32_t n = (uint32_t)mOrder.size();
    if (!n) { mRevalCursor = 0; return; }
    for (uint32_t done = 0, seen = 0; done < budget && seen < n; ++seen)
    {
        if (mRevalCursor >= n) mRevalCursor = 0;
        Cand& c = mCands[mOrder[mRevalCursor++].second];
        if (!c.rigged || c.state != 1 || (c.avatar == kNoIdx && c.bind == kNoIdx)) continue;
        ++done;
        bool ok = false;
        LLViewerObject* o = gObjectList.findObject(c.object);
        if (o && !o->isDead() && c.avatar != kNoIdx && c.bind != kNoIdx && o->getPCode() == LL_PCODE_VOLUME)
        {
            LLVOAvatar* av = o->getAvatar();
            const LLMeshSkinInfo* skin = static_cast<LLVOVolume*>(o)->getSkinInfo();
            ok = av && skin && av->getID() == mAvatarUse[c.avatar].id
              && std::memcmp(mBinds[c.bind].data(), skin->mBindShapeMatrix.getF32ptr(), sizeof(float) * 16) == 0;
        }
        if (!ok) releaseRig(c);
    }
}

#endif // LL_HAS_CLEAN_PIPELINE

void LLCleanClusterSelectVK::dropCandidates()
{
    mCands.clear(); mCands.shrink_to_fit();
    mCandFree.clear(); mCandFree.shrink_to_fit();
    mOrder.clear(); mOrder.shrink_to_fit();
    mRowCand.clear(); mRowCand.shrink_to_fit();
    mMeshUse.clear(); mMeshUse.shrink_to_fit();
    mMeshUseFree.clear(); mMeshUseFree.shrink_to_fit();
    mMeshUseIdx.clear();
    mAvatarUse.clear(); mAvatarUse.shrink_to_fit();
    mAvatarUseFree.clear(); mAvatarUseFree.shrink_to_fit();
    mAvatarUseIdx.clear();
    mBinds.clear(); mBinds.shrink_to_fit();
    mBindFree.clear(); mBindFree.shrink_to_fit();
    mRuns.clear(); mRuns.shrink_to_fit();
    mAdds.clear(); mRemoved.clear();
    mRows = 0;
    mRevalCursor = 0;
}

// The instance store's live rows -> this frame's ClusterInstance list (+ the exclusion bitset).
void LLCleanClusterSelectVK::gather(uint64_t frame)
{
    const auto t0 = std::chrono::steady_clock::now();
    mCpuInst.clear();
    mOwnedAny = false;
    mGathered[0] = mGathered[1] = 0;
    mAlGathered = 0;
    mPending = 0;
    mResolvedThisFrame = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    uint32_t walked = 0, visited = 0;
    const auto account = [&]()
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        mGatherMsAcc += ms;
        mGatherMsMax = std::max(mGatherMsMax, ms);
        ++mGatherFrames;
        mGatherVisitAcc += visited;
        mGatherWalkAcc += walked;
        mGatherResAcc += mResolvedThisFrame;
        LLVKHitch::count("vk_clusterGatherWalk", walked);
        LLVKHitch::count("vk_clusterGatherVisit", visited);
    };
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    if (!mChangeLogOn) { store.setChangeLog(true); mChangeLogOn = true; }   // (starts "full")
    bool full = false;
    store.takeChangeLog(mRuns, full);
    const uint8_t* rows = store.cpuInstanceBytes();
    const U32 n = store.cpuInstanceCount();
    mRowGens = store.rowGenerations();   // (n entries, like rows)
    if (!rows || !n)
    {
        if (mRows || !mOrder.empty()) dropCandidates();
        mRuns.clear();
        mCpuOwned.clear();
        mOwner.skin().gatherBegin(0);
        mOwner.skin().admit(false, mAdmitted);   // (no rows: no avatar on the cluster path)
        account();
        return;
    }
    F32 d[3] = {0.f, 0.f, 0.f};
    store.renderOffset(d);   // store = agent + d: agent-space lanes are R - d
    const bool statics = mStatic != 0;
    if (n != mRows || statics != mStaticCands) full = true;
    mStaticCands = statics;
    mRows = n;
    if (std::memcmp(d, mRowD, sizeof(mRowD)) != 0)
    {   // (a crossing moved the agent frame: every static xform moves with it, no row changed)
        std::memcpy(mRowD, d, sizeof(mRowD));
        for (const auto& p : mOrder)
            if (!mCands[p.second].rigged) mCands[p.second].rec = false;
    }
    // #1011 PROFILE (header: kGatherSampleEvery): this gather split by phase on a sampled frame
    using Clock = std::chrono::steady_clock;
    const bool gs = (frame % kGatherSampleEvery) == 0;
    const auto gms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const Clock::time_point g0 = gs ? Clock::now() : Clock::time_point{};
    double g_keys = 0.0, g_note = 0.0;
    intake(rows, n, full, d, visited);
    revalidate(kRevalPerFrame);
    const Clock::time_point g1 = gs ? Clock::now() : Clock::time_point{};

    // The exclusion bitset must exist BEFORE any instance is marked as drawn for real: an
    // instance flagged kInstDraw whose legacy draw is not excluded would draw twice.
    if (mExclude && !ensureOwned(mSlot, (n + 31) / 32)) mExclude = false;
    mCpuOwned.assign(mExclude ? (n + 31) / 32 : 0, 0u);
    mStaticOwned.assign(mExcludeStatic ? (n + 31) / 32 : 0, 0u);   // (#748 static pass: ownership flips)
    // 2a (#747): the per-avatar switch tallies every eligible rigged row and instance.
    LLCleanClusterSkinVK& skin = mOwner.skin();
    skin.gatherBegin(n);
    LLCleanMeshAsset& assets = LLCleanMeshAsset::instance();
    LLCleanClusterStreamVK& stream = mOwner.stream();
    LLCleanClusterControlVK& control = mOwner.control();
    const U32 frame32 = (U32)frame;
    const U32 best_gen = assets.bestSlotGeneration();   // (before any groupBestSlot below)
    // BUILD PRIORITY of a cache miss (1c / 1a, LLCleanClusterBuilder PRIORITY): an avatar switches
    // only when EVERY rigged mesh of it is on the cluster path, so its faces build as one set,
    // nearest / largest avatar first (our own first); statics by size over distance.
    const LLVector3 cam_v = LLViewerCamera::getInstance()->getOrigin();   // agent space
    const float cam[3] = {cam_v.mV[0], cam_v.mV[1], cam_v.mV[2]};
    // (the main view's post-controller tolerance this frame: the blend instances' cap is relative to it)
    const float main_tol = std::max(control.roleTolPx(CLROLE_MAIN), 0.05f);

    for (const auto& p : mOrder)
    {
        Cand& c = mCands[p.second];
        const bool rigged = c.rigged;
        if (!rigged && mStatic == 0) continue;   // (never: statics are candidates only while on)
        ++walked;
        // 2a: every eligible rigged row counts toward its avatar BEFORE any reason it may not
        // become an instance this frame (an avatar switches only when all of them did).
        const uint32_t av = rigged ? skin.avatarOfSkin(c.skin_id) : kNone;
        if (rigged)
        {
            const Clock::time_point n0 = gs ? Clock::now() : Clock::time_point{};
            skin.noteRow(av);
            if (gs) g_note += gms(n0, Clock::now());
        }
        // CLUSTER-OWNED ROW (2026-10-14): every eligible rigged row belongs to the cluster path,
        // drawn by it or not yet -- the legacy passes never draw it (its legacy geometry is not
        // even uploaded: LLCleanRendererVK::fillFromCleanStores, rowClusterRoutable).
        if (rigged && mOwnsRigged && c.row / 32 < mCpuOwned.size())
        {
            mCpuOwned[c.row / 32] |= 1u << (c.row & 31u);
            mOwnedAny = true;
        }

        // Key resolution (budgeted, cached per row; redone when the row's LOD group changes).
        if (c.state == 0 || (c.state == 2 && frame32 >= c.retry))
        {
            if (mResolvedThisFrame >= kResolvesPerFrame) continue;
            ++mResolvedThisFrame;
            if (!resolveKey(c, frame32)) continue;
        }
        if (c.state != 1) continue;
        // rigged: the wearer (cached per row; its values once per avatar per frame)
        AvatarUse* A = nullptr;
        if (rigged)
        {
            if (c.avatar == kNoIdx || c.bind == kNoIdx) resolveRig(c);   // (every frame while it fails, as before)
            if (c.avatar != kNoIdx)
            {
                A = avatarPose(c.avatar, frame, cam);
                if (!A) releaseRig(c);   // (wearer gone: re-resolved from the object next time)
            }
        }
        else if (!c.rec)
            buildStaticRec(c, reinterpret_cast<const uint8_t*>(&rowAt(rows, c.row)), d);

        // The registered cluster mesh (1c), once per key per frame.
        MeshUse& m = mMeshUse[c.mesh];
        if (m.frame != frame)
        {
            const Clock::time_point k0 = gs ? Clock::now() : Clock::time_point{};
            m.frame = frame;
            if (m.src_gen != best_gen || m.src_gen == 0)
            {
                m.src_slot = assets.groupBestSlot(m.key.mesh_id, m.key.face);
                m.src_gen = best_gen;
            }
            LLCleanClusterStreamVK::MeshRef ref;
            if (m.src_slot != LLCleanMeshAsset::LOD_SENTINEL)
            {
                float importance = 0.f;
                uint32_t group = kNone;
                if (rigged)
                {
                    // the avatar's height over its distance; its joint-table slot groups its faces
                    if (A)
                    {
                        importance = (c.flags & 8192u) ? 1.0e6f : A->importance;
                        group = av;
                    }
                }
                else
                {
                    // the object's scale (largest column of the model matrix) over its distance
                    const LLVector3 at(c.xform[3], c.xform[7], c.xform[11]);
                    importance = c.size / std::max(dist_vec(at, cam_v), 1.f);
                }
                ref = stream.acquireMesh(m.key, m.src_slot, frame, std::isfinite(importance) ? importance : 0.f, group,
                                         &m.dir);
            }
            m.ref_mesh = ref.mesh;
            m.ref_gen = ref.generation;
            if (gs) g_keys += gms(k0, Clock::now());
        }
        if (m.ref_mesh == kNone) { ++mPending; continue; }
        if (mCpuInst.size() >= kMaxInstances)
        {
            // (#1010 audit: a broken-off gather holds EVERY rigged instance back this frame -- admit()
            // WHY_PENDING: no avatar is drawn by the cluster path, which owns them -- and the rows past
            // the cap are not drawn either. Counted on the 10 s gather summary ("capped"), warned once.)
            ++mGatherCapped;
            LL_WARNS_ONCE("Vulkan") << "Cluster LOD: the gather reached its instance cap (" << kMaxInstances
                                    << "): no rigged mesh is drawn by the cluster path this frame, nor the rows past "
                                       "the cap" << LL_ENDL;
            skin.gatherIncomplete();
            break;
        }

        ClusterInstance ci{};
        float tol_scale = 1.f;   // #749: static instances: the view's tolerance as is
        float arena_scale = 1.f; // #1010: the skinned arena's per-avatar coarsening (>= 1)
        if (rigged)
        {
            // REST POSE (debug overlay until 2a): avatar world x bind shape.
            if (!A || c.bind == kNoIdx) continue;
            {   // #749 PROTECTION by a lower tolerance (your own avatar, then smooth in its
                // on-screen size): the controller's tol_scale (llcleanclustercontrolvk.h).
                const uint32_t self = (c.flags & 8192u) != 0u ? 1u : 0u;
                if (A->tol_frame[self] != frame)
                {
                    A->tol[self] = control.avatarTolScale(self != 0u, A->pos, A->height, &A->arena[self]);
                    A->tol_frame[self] = frame;
                }
                tol_scale = A->tol[self];
                arena_scale = A->arena[self];
            }
            if (!c.rec || c.pose != A->pose)
            {   // the wearer moved / turned (or a new bind shape): the xform again
                const F32* b = mBinds[c.bind].data();   // column-major, translation 12..14
                for (int row = 0; row < 3; ++row)
                    for (int k = 0; k < 4; ++k)
                    {
                        // world = R (row-vector LLMatrix3: A[row][j] = rm.mMatrix[j][row]) * B + p * B[3][k]
                        F32 s = 0.f;
                        for (int j = 0; j < 3; ++j) s += A->rm[j * 3 + row] * b[k * 4 + j];
                        c.xform[row * 4 + k] = s + A->pos[row] * b[k * 4 + 3];
                    }
                xformFacts(c.xform, c.finite, c.neg_det, &c.lod_scale);
                c.pose = A->pose;
                c.rec = true;
            }
            std::memcpy(ci.xform, c.xform, sizeof(ci.xform));
            ci.flags |= CLI_RIGGED;
            ci.skin_id = c.skin_id;
            ++mGathered[0];
        }
        else
        {
            std::memcpy(ci.xform, c.xform, sizeof(ci.xform));
            ci.skin_id = kNone;
            ++mGathered[1];
        }
        if (!c.finite) continue;
        const uint32_t id = c.row;
        ci.render_instance = id;
        ci.mesh = m.ref_mesh;
        ci.mesh_generation = m.ref_gen;
        if (c.flags & 8192u) ci.flags |= CLI_SELF;
        if (c.flags & 256u) ci.flags |= kInstDoubleSided;
        // RIGGED ALPHA-BLEND (row flag 2): the alpha lists only (ordered back to front, blended);
        // a tighter tolerance cap -- thin cards / lashes are what a coarse level deletes first.
        if (rigged && (c.flags & 2u))
        {
            ci.flags |= kInstBlend;
            ++mAlGathered;
            const float cap = settingBlendTolMaxPx();
            if (cap > 0.f && main_tol > cap) tol_scale = std::min(tol_scale, cap / main_tol);
        }
        // #702 integration: the wearer's joint-table slot (the shared rig lists are per avatar)
        // and the moving-caster bit (the cascades' #567 static / dynamic split).
        if (rigged && llAvatarSlotOk(av, "cluster gather")) ci.flags |= kInstAvatar | (av << kInstAvShift);
        if (!rigged && (c.flags & 2097152u)) ci.flags |= kInstDynCaster;
        if (c.neg_det) ci.flags |= kInstNoCone;
        // #1010: x the ARENA term after the alpha cap (a crowd's hair coarsens with its avatar, every
        // mesh of an avatar by the same factor) -- tol_scale may now exceed 1 (clean_cluster_common
        // .glsl clTolerance has no bound; the rigged RT cut reads the same value)
        if (std::isfinite(arena_scale) && arena_scale > 1.f) tol_scale *= arena_scale;
        ci.tol_scale = std::isfinite(tol_scale) ? std::clamp(tol_scale, 0.01f, kMaxTolScale) : 1.f;   // #749 (2c)
        ci.lod_scale = c.lod_scale;
        ci.anim_margin_m = 0.f;
        if (!rigged && mExcludeStatic && id / 32 < mCpuOwned.size())
        {
            ci.flags |= kInstDraw;
            mCpuOwned[id / 32] |= 1u << (id & 31u);
            if (id / 32 < mStaticOwned.size()) mStaticOwned[id / 32] |= 1u << (id & 31u);
            mOwnedAny = true;
        }
        if (rigged)
        {
            const Clock::time_point n0 = gs ? Clock::now() : Clock::time_point{};
            skin.noteInstance(av, (uint32_t)mCpuInst.size(), ci.mesh, c.skin_id, (c.flags & 8192u) != 0u,
                              m.key.mesh_id, m.key.face, m.src_slot,   // (+ #798 weld probe)
                              (c.flags & 8192u) ? 1.0e6f : (A ? A->importance : 0.f), arena_scale);   // (+ #1010)
            if (gs) g_note += gms(n0, Clock::now());
        }
        mCpuInst.push_back(ci);
    }
    const Clock::time_point g2 = gs ? Clock::now() : Clock::time_point{};
    // 2a (#747): the avatars whose every rigged mesh is on the cluster path this frame are drawn
    // by it for real (CLI1B_DRAW + their exclusion bits); the rest stay legacy.
    skin.admit(mOwnsRigged || (mExclude && !mCpuOwned.empty()), mAdmitted);
    for (const uint32_t k : mAdmitted)
    {
        if (k >= mCpuInst.size()) continue;
        ClusterInstance& ci = mCpuInst[k];
        // (rigged alpha-blend: drawn only from the alpha lists -- not drawn at all while they
        // cannot run; still owned: never a legacy copy)
        if ((ci.flags & kInstBlend) && !mAlphaOn) continue;
        const uint32_t rid = ci.render_instance;
        if (rid / 32 >= mCpuOwned.size()) continue;   // (never: the bitset covers every row)
        ci.flags |= kInstDraw;
        mCpuOwned[rid / 32] |= 1u << (rid & 31u);
        mOwnedAny = true;
    }
    if (mCpuInst.capacity() > 2 * mCpuInst.size() + 4096) mCpuInst.shrink_to_fit();
    // #748 static pass: statics drawn for real this frame, and the rows whose ownership flipped
    // since the last one (owned <-> legacy: a mesh rebuilding / evicted, the exclusion gate)
    {
        uint32_t owned = 0;
        uint64_t flips = 0;
        const size_t nw = std::max(mStaticOwned.size(), mStaticOwnedPrev.size());
        for (size_t w = 0; w < nw; ++w)
        {
            const uint32_t a = w < mStaticOwned.size() ? mStaticOwned[w] : 0u;
            const uint32_t b = w < mStaticOwnedPrev.size() ? mStaticOwnedPrev[w] : 0u;
            owned += (uint32_t)std::popcount(a);
            flips += (uint64_t)std::popcount(a ^ b);
        }
        mStaticOwnedN = owned;
        mStaticFlipAcc += flips;
        mStaticOwned.swap(mStaticOwnedPrev);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (mStaticFlipStart <= 0.0) mStaticFlipStart = now;
        else if (now - mStaticFlipStart >= 10.0)
        {
            mStaticFlipsPerSec = (double)mStaticFlipAcc / (now - mStaticFlipStart);
            mStaticFlipAcc = 0;
            mStaticFlipStart = now;
        }
    }
    if (gs)
    {
        mGsIntake += gms(g0, g1);
        mGsLoop += gms(g1, g2);
        mGsKeys += g_keys;
        mGsNote += g_note;
        mGsTail += gms(g2, Clock::now());
        ++mGsFrames;
    }
    account();
#else
    (void)frame;
    (void)t0;
#endif
}

void LLCleanClusterSelectVK::gatherSummary(std::string& out)
{
    char b[128];
    const double f = mGatherFrames ? (double)mGatherFrames : 1.0;
    std::snprintf(b, sizeof(b), "gather %.3f/%.2f ms walk %.0f chg %.0f res %.1f full %u",
                  mGatherMsAcc / f, mGatherMsMax, (double)mGatherWalkAcc / f, (double)mGatherVisitAcc / f,
                  (double)mGatherResAcc / f, mGatherFull);
    out += b;
    if (mGatherCapped) out += " capped " + std::to_string(mGatherCapped);   // (#1010 audit: frames at kMaxInstances)
    // #1011 PROFILE: the sampled split (ms per sampled gather; the inner timers add clock reads)
    if (mGsFrames)
    {
        const double g = (double)mGsFrames;
        char p[160];
        std::snprintf(p, sizeof(p), " (split: intake %.3f loop %.3f [keys %.3f 2a notes %.3f] tail %.3f)", mGsIntake / g,
                      mGsLoop / g, mGsKeys / g, mGsNote / g, mGsTail / g);
        out += p;
    }
    mGsIntake = mGsLoop = mGsKeys = mGsNote = mGsTail = 0.0;
    mGsFrames = 0;
    mGatherMsAcc = mGatherMsMax = 0.0;
    mGatherFrames = mGatherFull = mGatherCapped = 0;
    mGatherVisitAcc = mGatherWalkAcc = mGatherResAcc = 0;
}

// =============================================================================================
// recording
// =============================================================================================
void LLCleanClusterSelectVK::barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                                     VkPipelineStageFlags2 ds, VkAccessFlags2 da) const
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanClusterSelectVK::recordBegin(VkCommandBuffer cmd)
{
    if (!mReady || !mCounters.buffer) return;
    // WAR / WAW: last frame's indirect + shader reads of the counters / arena / draw records
    // and its selection writes, before this frame's zero fill and selection writes.
    VkPipelineStageFlags2 src = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_COPY_BIT;
#ifdef VK_EXT_mesh_shader
    if (mMesh) src |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#endif
    barrier(cmd, src, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // The view counters every frame; the whole buffer (dedupe stamps) once.
    vkCmdFillBuffer(cmd, mCounters.buffer, 0, mCountersFresh ? VK_WHOLE_SIZE : (VkDeviceSize)kDedupBase * 4, 0);
    mCountersFresh = false;
    // (LLCleanClusterVK::tick records the TRANSFER -> shader barrier after this.)
}

uint32_t LLCleanClusterSelectVK::recordSelect(VkCommandBuffer cmd, const LLClusterViewIn& vin)
{
    if (!mReady || !mSelectPipe || !mFinalizePipe || mInstanceCount == 0 || mViewCount >= kMaxViews
        || !mViews[mSlot].mapped || !mSelected.buffer || !mDraws.buffer || !mCounters.buffer || !mInst[mSlot].buffer)
        return kNone;
    const uint32_t remaining = mArena > mArenaUsed ? mArena - mArenaUsed : 0u;
    if (remaining < 256) return kNone;
    // #749 ARENA SLICES PER ROLE: a view takes its role's want (the largest read-back demand
    // of the role + 25 % + 1024); the main view (recorded first) takes what is left after
    // reserving the other roles' views of the last read-back frame, never less than its want
    // while the arena has it. Overflow is counted; the arena grows from the total demand.
    const uint32_t role = std::min<uint32_t>(vin.role, CLROLE_COUNT - 1);
    const auto want = [&](uint32_t r) -> uint32_t
    {
        const uint64_t w = (uint64_t)mRoleDemand[r] + mRoleDemand[r] / 4 + 1024;
        return (uint32_t)std::min<uint64_t>(w, mArena);
    };
    uint32_t cap = std::min(remaining, want(role));
    if (role == CLROLE_MAIN)
    {
        uint64_t reserve = 0;
        for (uint32_t r = 0; r < CLROLE_COUNT; ++r)
            if (r != CLROLE_MAIN) reserve += (uint64_t)mRoleViews[r] * want(r);
        const uint32_t rest = remaining > reserve ? remaining - (uint32_t)reserve : 0u;
        cap = std::max(rest, cap);
    }
    if (cap < 256) return kNone;
    const uint32_t v = mViewCount++;
    const bool main = (vin.flags & CLV_MAIN) != 0u;
    mViewRole[mSlot][v] = (uint8_t)role;
    mViewsRecorded[mSlot] = mViewCount;

    ClusterView cv{};
    std::memcpy(cv.view_proj, vin.view_proj, sizeof(cv.view_proj));
    std::memcpy(cv.planes, vin.planes, sizeof(cv.planes));
    cv.cam_pos[0] = vin.cam_pos[0]; cv.cam_pos[1] = vin.cam_pos[1]; cv.cam_pos[2] = vin.cam_pos[2];
    cv.cam_pos[3] = std::max(vin.znear, 1e-3f);
    cv.origin[0] = vin.origin[0]; cv.origin[1] = vin.origin[1]; cv.origin[2] = vin.origin[2];
    cv.px_scale = vin.px_scale;
    cv.tol_px = vin.tol_px;
    cv.flags = vin.flags & ~CLV_OCCLUSION;
    if (main && mHizOk) cv.flags |= CLV_OCCLUSION;
    cv.sel_base = mArenaUsed;
    cv.sel_cap = cap;
    cv.draw_base = mArenaUsed * 2;
    cv.draw_cap = cap;
    cv.hiz_size = (cv.flags & CLV_OCCLUSION) ? ((mHizSize[0] / 2) & 0xFFFFu) | ((mHizSize[1] / 2) << 16) : 0u;
    cv.frame = (uint32_t)mFrame;
    cv.view_index = v;
    cv.viewport_px[0] = vin.viewport_px[0];
    cv.viewport_px[1] = vin.viewport_px[1];
    // #749: the LOD camera of the error projection (the core filled it from the controller;
    // a caller that did not: the live camera, no margin).
    const bool lod_set = std::isfinite(vin.lod_cam[0]) && std::isfinite(vin.lod_cam[1]) && std::isfinite(vin.lod_cam[2])
                      && std::isfinite(vin.lod_cam[3]) && vin.lod_cam[3] >= 0.f
                      && (vin.lod_cam[0] != 0.f || vin.lod_cam[1] != 0.f || vin.lod_cam[2] != 0.f || vin.lod_cam[3] != 0.f);
    for (int k = 0; k < 3; ++k) cv.lod_cam[k] = lod_set ? vin.lod_cam[k] : vin.cam_pos[k];
    cv.lod_cam[3] = lod_set ? vin.lod_cam[3] : 0.f;
    std::memcpy(static_cast<uint8_t*>(mViews[mSlot].mapped) + (size_t)v * sizeof(ClusterView), &cv, sizeof(cv));
    mArenaUsed += cap;
    if (main)
    {   // #702 integration: the SHARED RIG CUT of this frame (the views tail, this slot): the rig
        // lists' place and the per-avatar list bits; read by this selection, its finalize, the
        // rig-list draws and the pick (host-coherent, written before the submit).
        uint32_t rig[4 + 2 * kAvatarSlotWords] = {};
        rig[0] = rigBaseRecords();
        rig[1] = mRigCap;
        const bool on = mRigWant && mRigCap != 0;
        rig[2] = on ? 1u : 0u;
        rig[3] = on ? mRigEmit : 0u;   // (#748 perf pass) the lists whose records are emitted
        if (on) std::memcpy(rig + 4, mRigBits, sizeof(mRigBits));
        std::memcpy(static_cast<uint8_t*>(mViews[mSlot].mapped) + kViewsTailOffset + kAuxRigOffset, rig, sizeof(rig));
        mRigFrame = on ? mFrame : ~0ull;
        mMainView = v;
        if (!mSnapshotFrame)
        {   // (the pick answers for the live view: a snapshot's recordings leave it alone)
            mPickFrame = mFrame;
            mPickSlot = mSlot;
            mPickView = v;
        }
    }
    ViewRec& rec = mViewRecs[v];
    rec.slice.sel_base = cv.sel_base;
    rec.slice.sel_cap = cap;
    rec.slice.flags = cv.flags;
    rec.draw_base = cv.draw_base;
    rec.draw_cap = cap;
    rec.valid = true;

    // Earlier compute writes (the legacy main cull, last frame's Hi-Z build, 1c's table
    // updates through their own barrier) -> the selection's reads.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet set = mOwner.set(mSlot);
    const int32_t tier = std::max(drawTier(), 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSelectPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompLayout, 0, 1, &set, 0, nullptr);
    // pad = 2a (#747): the rigged posed-bound margins of this frame are written (recordBounds).
    // bit 1 (#702): this secondary view skips moving casters (the cascades' #567 split).
    const SelectPush sp{v, mInstanceCount, (uint32_t)tier,
                        (mOwner.skin().boundsValid() ? 1u : 0u) | (!main && vin.skip_dyn ? 2u : 0u)};
    vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
    const uint32_t gx = std::min(mInstanceCount, kGridX);
    vkCmdDispatch(cmd, gx, (mInstanceCount + gx - 1) / gx, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFinalizePipe);
    const FinalizePush fp{v, 0u, 0u, 0u};
    vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fp), &fp);
    vkCmdDispatch(cmd, 1, 1, 1);
    // -> the draws (indirect args / counts, selected entries, aux words), the 2a / 2b
    // consumers (compute) and the counters readback copy.
    VkPipelineStageFlags2 dst = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_COPY_BIT;
#ifdef VK_EXT_mesh_shader
    if (mMesh) dst |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#endif
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, dst,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    if (mReadback[mSlot].buffer)
    {
        const VkBufferCopy c{(VkDeviceSize)v * kViewWords * 4, (VkDeviceSize)v * kViewWords * 4, kViewWords * 4};
        vkCmdCopyBuffer(cmd, mCounters.buffer, mReadback[mSlot].buffer, 1, &c);
        mReadbackValid[mSlot] = true;
    }
    if (main)
    {   // the Hi-Z this frame builds (after its pre-pass) pairs with this view next frame
        std::memcpy(mOccViewProj, vin.view_proj, sizeof(mOccViewProj));
        mOccOrigin[0] = vin.origin[0]; mOccOrigin[1] = vin.origin[1]; mOccOrigin[2] = vin.origin[2];
        mOccFrame = mFrame;
    }
    return v;
}

// The raster tier the draws use: the caps' tier in force, lowered to the best one whose
// pipelines exist (-1 = none yet / no G-buffer template).
int32_t LLCleanClusterSelectVK::drawTier() const
{
    if (!mPipesReady) return -1;
    for (int32_t t = std::min<int32_t>(mOwner.caps().get(CLCAP_RASTER), CLT_RASTER_MESH); t >= 0; --t)
        if (mDrawPipe[t][0][0] && mDrawPipe[t][0][1] && mDrawPipe[t][1][0] && mDrawPipe[t][1][1])
            return t;
    return -1;
}

bool LLCleanClusterSelectVK::passReady(LLClusterDrawPass pass) const
{
    const int k = passKind(pass);
    const int32_t t = drawTier();
    if (!mReady || k < 0 || t < 0 || !mPassPipe[k][t][0] || !mPassPipe[k][t][1]) return false;
    // (captures with RT: the ray-query lit variant too -- whichever lit variant the frame
    // selects later, the capture can draw; the legacy-skin skip commits before that choice)
    if (k == 2 && mOwner.env().rtSetLayout && (!mCapRtPipe[t][0] || !mCapRtPipe[t][1])) return false;
    return true;
}

bool LLCleanClusterSelectVK::rigDrawReady(LLClusterDrawPass pass) const
{
    // (= recordDraw's checks for a rig-list draw with view = the main view)
    const uint32_t list = pass == CLPASS_PROBE ? 1u : 0u;
    return mReady && passKind(pass) >= 0 && rigListShared(list) && mMainView < mViewCount && mViewRecs[mMainView].valid
        && passReady(pass) && mOwner.registeredBuffer(CLB_POOL) != VK_NULL_HANDLE;
}

void LLCleanClusterSelectVK::setRigLists(const uint32_t bits[2][kAvatarSlotWords], bool on, uint32_t emit)
{
    std::memcpy(mRigBits, bits, sizeof(mRigBits));
    mRigWant = on;
    mRigEmit = emit & 3u;
    for (uint32_t l = 0; l < kRigLists; ++l)
    {
        uint32_t n = 0;
        for (uint32_t w = 0; w < kAvatarSlotWords; ++w)
            for (uint32_t x = bits[l][w]; x; x &= x - 1u) ++n;
        mRigAvatars[l] = on ? n : 0u;
        mRigListOn[l] = on && n != 0 && ((mRigEmit >> l) & 1u) != 0u;   // (drawable: its records exist)
    }
}

// #748 perf pass: the MDI tiers' RIG-LIST CULL of ONE secondary view (clean_cluster_rigcull.comp):
// rig list `list` -> the culled region, only the records whose avatar / cluster skinned box
// touches the side planes of the draw view at `draw_view_addr` (the renderer's param ring entry
// the view's rig-list draw pushes). Outside any rendering scope, right before the view's scope;
// recordDraw then takes the culled copy (LLClusterDrawIn::rig_culled). One region: each call
// waits for the previous view's draws of it (the views record one after another anyway).
// False = nothing recorded (mesh tier: its task stage culls; no pipeline; the list is not
// drawable this frame): the caller draws the whole list.
bool LLCleanClusterSelectVK::recordRigCull(VkCommandBuffer cmd, uint32_t list, uint64_t draw_view_addr)
{
    if (!mReady || !mRigCullPipe || !mRigCullLayout || !draw_view_addr || list >= kRigLists || !rigListShared(list)
        || mMainView >= mViewCount || !mViewRecs[mMainView].valid || !mCounters.buffer || !mDraws.buffer)
        return false;
    const int32_t t = drawTier();
    if (t < 0 || t >= CLT_RASTER_MESH) return false;
    const VkPipelineStageFlags2 draws = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                      | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    // WAR: the previous view's draws of the culled region / its counts; RAW: the selection's lists.
    barrier(cmd, draws | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdFillBuffer(cmd, mCounters.buffer, ((VkDeviceSize)mMainView * kViewWords + kCntRigCulled) * 4, 8, 0u);
    barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    RigCullPush p{};
    p.view_lo = (uint32_t)(draw_view_addr & 0xFFFFFFFFull);
    p.view_hi = (uint32_t)(draw_view_addr >> 32);
    p.list = list;
    p.main_view = mMainView;
    p.out_base = rigCulledRecords();
    const VkDescriptorSet cset = mOwner.set(mSlot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigCullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigCullLayout, 0, 1, &cset, 0, nullptr);
    vkCmdPushConstants(cmd, mRigCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, std::max<uint32_t>((mRigCap + 63) / 64, 1u), 2, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, draws,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    mRigCullFrame = mFrame;
    mRigCullList = list;
    ++mRigCulls;
    return true;
}

bool LLCleanClusterSelectVK::recordDraw(const LLClusterDrawIn& in)
{
    // CLPASS_GBUFFER, (#749) CLPASS_PREPASS, CLPASS_FORWARD (the main view's forward lit pass 1:
    // the same records as the G-buffer draw), (#702 integration) the secondary passes: a
    // secondary view's own records (statics) or the main view's shared rig lists (rigged).
    if (in.alpha || in.pass >= CLPASS_ALPHA) return recordAlphaDraw(in);
    const bool pre = in.pass == CLPASS_PREPASS;
    const bool fwd = in.pass == CLPASS_FORWARD;
    const int pk = passKind(in.pass);
    const bool rig = in.list != CLLIST_VIEW;
    if (!mReady || (in.pass != CLPASS_GBUFFER && !pre && !fwd && pk < 0) || in.view >= mViewCount
        || !mViewRecs[in.view].valid)
        return false;
    if (rig && (pk < 0 || in.list > kRigLists || !rigListShared(in.list - 1) || in.view != mMainView)) return false;
    int32_t t = drawTier();
    if (t < 0) return false;
    // #748 perf pass: an MDI tier draws the per-view culled copy recordRigCull made of this list
    // (just before this scope); the mesh tier culls in its task stage.
    const bool culled = rig && in.rig_culled && t < CLT_RASTER_MESH && mRigCullFrame == mFrame
                     && mRigCullList == in.list - 1;
    if (pre && (in.pre_variant > 3 || !prepassReady())) return false;
    if (pk >= 0 && !passReady(in.pass)) return false;
    const uint32_t pv = pre ? in.pre_variant : 0u;
    // Legacy-skin skip: a capture in a ray-query lit frame -> the RT variant (cluster set at 3);
    // the forward lit pass of a ray-query lit frame likewise.
    const bool rtv = (pk == 2 || fwd) && in.rt_set != VK_NULL_HANDLE;
    if (rtv && pk == 2 && (!mCapRtPipe[t][0] || !mCapRtPipe[t][1])) return false;
    if (fwd && !forwardReady(rtv)) return false;
    // (the static exclusion gate: the main view's material draw ran -- G-buffer or forward)
    if (in.pass == CLPASS_GBUFFER || fwd) mLastGbufDraw = mFrame;
    const auto pipeOf = [&](uint32_t fsel, uint32_t b) -> VkPipeline
    {
        if (fwd) return mFwdPipe[rtv ? 1 : 0][t][b];
        // RenderVKDepthOnlyVS: the depth-stage twin of the pre-pass depth variant / sun / projector.
        if (pre) return dvOr(mPreDvPipe[t][pv][b], mPrePipe[t][pv][b]);
        if (!rtv && (pk == 0 || pk == 1)) return dvOr(mPassDvPipe[pk][t][b], mPassPipe[pk][t][b]);
        return rtv ? mCapRtPipe[t][b] : pk >= 0 ? mPassPipe[pk][t][b] : mDrawPipe[t][fsel][b];
    };
    VkBuffer pool = mOwner.registeredBuffer(CLB_POOL);
    if (!pool) return false;   // (never registered: no instance could have been gathered)
    const ViewRec& vr = mViewRecs[in.view];
    const uint32_t fs = (pk < 0 && ((mDebug >= 1 && mDebug <= 3) || mDebug == 5 || mDebug == 6)) ? 1u : 0u;   // (= clDebugTinted)
    // The records: the view's buckets, or rig list r = (list - 1) * 2 + bucket.
    const auto recOff = [&](uint32_t b) -> VkDeviceSize
    {
        if (!rig) return ((VkDeviceSize)vr.draw_base + (VkDeviceSize)b * vr.draw_cap) * kDrawWords * 4;
        if (culled) return ((VkDeviceSize)rigCulledRecords() + (VkDeviceSize)b * mRigCap) * kDrawWords * 4;
        return ((VkDeviceSize)rigBaseRecords() + (VkDeviceSize)((in.list - 1) * 2 + b) * mRigCap) * kDrawWords * 4;
    };
    const auto cntWord = [&](uint32_t b) -> uint32_t
    {
        if (culled) return kCntRigCulled + b;
        return rig ? kCntRigDraws + (in.list - 1) * 2 + b : (b ? kCntDraws1 : kCntDraws0);
    };
    const auto taskWord = [&](uint32_t b) -> uint32_t
    { return rig ? kCntRigTasks + ((in.list - 1) * 2 + b) * 4 : (b ? kCntTasks1 : kCntTasks0); };
    const uint32_t recCap = rig ? mRigCap : vr.draw_cap;
    if (pk >= 0) ++mPassDraws[pk * 2 + (rig ? 1 : 0)];
    const VkDescriptorSet cset = mOwner.set(mSlot);
    const VkDeviceSize cntOff = (VkDeviceSize)in.view * kViewWords * 4;
    VkCommandBuffer cmd = in.cmd;
#ifdef VK_EXT_mesh_shader
    if (t == CLT_RASTER_MESH)
    {
        // Mesh layout (push TASK|MESH|FS): not push-compatible with the renderer's draw
        // layout -> bind sets 0..2 and push here, restore the renderer's state after.
        const VkPipelineLayout ml = rtv ? mMeshLayoutRt : mMeshLayout;
        const VkDescriptorSet sets[4] = {in.draw_sets[0], in.draw_sets[1], rtv ? in.rt_set : cset, cset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ml, 0, rtv ? 4u : 3u, sets, 0, nullptr);
        const PFN_vkCmdDrawMeshTasksIndirectEXT drawTasks = mDrawMeshTasksIndirect;
        for (uint32_t b = 0; b < 2; ++b)
        {
            bindGraphicsPipeline(cmd, pipeOf(fs, b));
            const DrawPush16 p{in.draw_view, 0u, in.view, b | (in.list << 4)};
            vkCmdPushConstants(cmd, ml,
                               VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(p), &p);
            drawTasks(cmd, mCounters.buffer, cntOff + taskWord(b) * 4, 1, 12);
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayout, 0, 2, in.draw_sets, 0, nullptr);
        const DrawPush16 restore{in.draw_view, 0u, 0u, 0u};
        vkCmdPushConstants(cmd, mDrawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(restore), &restore);
        return true;
    }
#endif
    // MDI tiers: the renderer's sets 0 / 1 and pushed view stay valid (compatible layout).
    // (ray-query lit captures: the RT set at 2 + the cluster set at 3, mDrawLayoutRt)
    if (rtv)
    {
        const VkDescriptorSet s2[2] = {in.rt_set, cset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayoutRt, 2, 2, s2, 0, nullptr);
    }
    else
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayout, 2, 1, &cset, 0, nullptr);
    if (t == CLT_RASTER_MDI_U8)
    {
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &pool, &zero);
        vkCmdBindIndexBuffer(cmd, pool, 0, VK_INDEX_TYPE_UINT8_EXT);
    }
    for (uint32_t b = 0; b < 2; ++b)
    {
        bindGraphicsPipeline(cmd, pipeOf(fs, b));
        const VkDeviceSize off = recOff(b);
        const VkDeviceSize cnt = cntOff + (VkDeviceSize)cntWord(b) * 4;
        if (t == CLT_RASTER_MDI_U8)
            vkCmdDrawIndexedIndirectCount(cmd, mDraws.buffer, off, mCounters.buffer, cnt, recCap, kDrawWords * 4);
        else
            vkCmdDrawIndirectCount(cmd, mDraws.buffer, off, mCounters.buffer, cnt, recCap, kDrawWords * 4);
    }
    // TODO(1b) CMDGEN DGC (VK_EXT_device_generated_commands): one indirect execution set per
    // view with the two bucket pipelines (+ the debug pair) as a shader-object / pipeline set,
    // the selection writing one sequence per bucket switch -- replaces the per-bucket binds
    // above once more buckets exist (material classes, pre-pass twins, shadow views). With two
    // buckets per pass the CPU-side binds cost nothing measurable, so it waits for 2c's views.
    return true;
}

// =============================================================================================
// RIGGED ALPHA-BLEND on the cluster path (CLI1B_BLEND; clean_cluster_alpha.comp)
// =============================================================================================
// DESIGN. LL orders alpha back to front BETWEEN faces (per drawable distance, split at the water
// plane) and draws each face in its index order (no per-triangle sort). The cluster path keeps
// exactly that contract: a blend instance is selected like any rigged instance (the cut, the
// shared un-culled cut of the rig lists, the cluster pre-skin, the derived RT cut), but its draw
// records never enter the opaque buckets; the selection STAGES them with a deterministic rank
// inside their instance, and the order pass (after the pre-skin: the key is the POSED position,
// the union of the instance's skinned cluster boxes) sorts the instances on the legacy alpha
// sort's key and scatters every record to its place: the main view's pre-water / post-water
// lists (pass 2a / 2b), rig list 0's alpha list (shadow casters), rig list 1's (captures, back
// to front). All on the GPU, no per-object CPU work.
bool LLCleanClusterSelectVK::alphaPipesReady() const
{
    const int32_t t = drawTier();
    return mReady && t >= 0 && mAlphaPassPipe && mAlphaOrderPipe && mAlphaPipe[t][0]
        && (!mOwner.env().rtSetLayout || mAlphaPipe[t][1]);
}

bool LLCleanClusterSelectVK::alphaDrawReady(LLClusterDrawPass pass) const
{
    const int32_t t = drawTier();
    if (!mReady || t < 0 || mAlphaOrderFrame != mFrame || mMainView >= mViewCount || !mViewRecs[mMainView].valid)
        return false;
    switch (pass)
    {
        case CLPASS_ALPHA:       return mAlphaPipe[t][0] != VK_NULL_HANDLE;
        case CLPASS_ALPHA_DEPTH: return mAlphaDepthPipe[t] != VK_NULL_HANDLE;
        case CLPASS_TRANS_DEPTH:
        case CLPASS_TRANS_COLOR: return mTransPipe[t][0] && mTransPipe[t][1] && rigListShared(0);
        case CLPASS_SHADOW:
        case CLPASS_PROJ:        return passReady(pass) && rigListShared(0);
        case CLPASS_PROBE:       return mCapAlphaPipe[t] != VK_NULL_HANDLE && rigListShared(1);
        default:                 return false;
    }
}

bool LLCleanClusterSelectVK::recordAlphaOrder(VkCommandBuffer cmd, uint32_t main_view, const float cam[3], float water,
                                              bool under)
{
    if (!mReady || !mAlphaOn || main_view != mMainView || main_view >= mViewCount || !mViewRecs[main_view].valid
        || !mAlphaPassPipe || !mAlphaOrderPipe || !mCounters.buffer || !mDraws.buffer)
        return false;
    // FIXED-COST AUDIT (2026-10-14): no rigged alpha-blend instance gathered this frame -> no
    // staged record exists (only CLI1B_BLEND instances stage): the three order dispatches, their
    // barriers and the alpha counters' readback copy order nothing. The alpha lists stay empty
    // (counts zeroed by recordBegin) and alphaDrawReady is false: no alpha draw is recorded.
    if (mAlGathered == 0) return false;
    const VkDescriptorSet set = mOwner.set(mSlot);
    AlphaPush p{};
    p.cam[0] = cam[0]; p.cam[1] = cam[1]; p.cam[2] = cam[2]; p.cam[3] = water;
    p.view = main_view;
    p.under = under ? 1u : 0u;
    const VkDeviceSize disp = ((VkDeviceSize)main_view * kViewWords + kCntAlDispatch) * 4;
    // The selection / finalize (counters, staged records, entries) and the pre-skin (skinned
    // boxes, map, scratch header) -> the prep's reads.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAlphaCompLayout, 0, 1, &set, 0, nullptr);
    // 0 PREP: the posed box per entry.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAlphaPassPipe);
    p.mode = 0;
    vkCmdPushConstants(cmd, mAlphaCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatchIndirect(cmd, mCounters.buffer, disp);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // 1 ORDER: keys, sort, bases, counts, task commands (one workgroup).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAlphaOrderPipe);
    p.mode = 1;
    vkCmdPushConstants(cmd, mAlphaCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // 2 SCATTER: the staged records to their ordered places.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAlphaPassPipe);
    p.mode = 2;
    vkCmdPushConstants(cmd, mAlphaCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatchIndirect(cmd, mCounters.buffer, disp);
    // -> the alpha draws (indirect records / counts / task commands, the mesh / VS reads) and the
    // counters' readback copy (recorded at the selection; it ran before: the alpha counts it
    // reads are the previous slot's -- harvest sums whatever the copy saw).
    VkPipelineStageFlags2 dst = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
#ifdef VK_EXT_mesh_shader
    if (mMesh) dst |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#endif
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, dst,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    if (mReadback[mSlot].buffer)
    {   // the alpha words of the main view's block, after the order wrote them
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkDeviceSize o = ((VkDeviceSize)main_view * kViewWords + kCntAlStaged) * 4;
        const VkBufferCopy c{o, o, (VkDeviceSize)(kViewWords - kCntAlStaged) * 4};
        vkCmdCopyBuffer(cmd, mCounters.buffer, mReadback[mSlot].buffer, 1, &c);
    }
    mAlphaOrderFrame = mFrame;
    return true;
}

void LLCleanClusterSelectVK::alphaSummary(std::string& out) const
{
    char b[224];
    std::snprintf(b, sizeof(b),
                  "alpha cl faces %u tris %u (gathered %u, entries %u, recs pre %u post %u shadow %u cap %u, overflow %u, "
                  "cap %u/%u%s)",
                  mAlMainInst, mAlMainTris, mAlGathered, mAlEntries, mAlCounts[kAlOutPre], mAlCounts[kAlOutPost],
                  mAlCounts[kAlOutRig0], mAlCounts[kAlOutRig1], mAlOverflow, mAlphaCap, mAlphaInst,
                  mAlphaOn ? "" : ", OFF: pipelines");
    out += b;
}

// The ALPHA lists' draws (LLClusterDrawIn::alpha / CLPASS_ALPHA*, CLPASS_TRANS_*): the ordered
// records of the main view (pre / post-water) or a rig list's alpha list, with the pass's alpha
// pipeline. Blend passes with a dynamic cull draw twice -- cull FRONT (push f8 = 1: single-sided
// instances' clusters collapse / launch nothing), then BACK -- exactly the legacy drawListBlend
// (inside-out double-sided blend meshes); every triangle still rasterizes once.
bool LLCleanClusterSelectVK::recordAlphaDraw(const LLClusterDrawIn& in)
{
    const int32_t t = drawTier();
    if (t < 0 || !alphaDrawReady(in.pass) || in.view != mMainView) return false;
    const bool rig = in.list != CLLIST_VIEW;
    if (rig && (in.list > kRigLists || !rigListShared(in.list - 1))) return false;
    // Which list (kAlOut*) and pipeline.
    uint32_t k = 0;
    if (in.pass == CLPASS_ALPHA || in.pass == CLPASS_ALPHA_DEPTH)
    {
        if (rig) return false;
        k = in.alpha_list ? kAlOutPost : kAlOutPre;
    }
    else
    {
        if (!rig) return false;
        k = in.list - 1 == 0 ? kAlOutRig0 : kAlOutRig1;
    }
    const bool rtv = in.pass == CLPASS_ALPHA && in.rt_set != VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    switch (in.pass)
    {
        case CLPASS_ALPHA:       pipe = mAlphaPipe[t][rtv ? 1 : 0]; break;
        // (dvOr: the RenderVKDepthOnlyVS twin of the depth passes, when built)
        case CLPASS_ALPHA_DEPTH: pipe = dvOr(mAlphaDepthDvPipe[t], mAlphaDepthPipe[t]); break;
        case CLPASS_TRANS_DEPTH: pipe = dvOr(mTransDvPipe[t][0], mTransPipe[t][0]); break;
        case CLPASS_TRANS_COLOR: pipe = dvOr(mTransDvPipe[t][1], mTransPipe[t][1]); break;
        case CLPASS_SHADOW:
        case CLPASS_PROJ:        pipe = dvOr(mPassDvPipe[passKind(in.pass)][t][1], mPassPipe[passKind(in.pass)][t][1]); break;   // alpha-tested, cull none
        case CLPASS_PROBE:       pipe = mCapAlphaPipe[t]; break;
        default: break;
    }
    if (!pipe) return false;
    VkBuffer pool = mOwner.registeredBuffer(CLB_POOL);
    if (!pool) return false;
    // two-pass blend: pass 2's blend with a dynamic cull (the captures draw once, cull none, as
    // the legacy mMirrorRigAlphaPipe)
    const bool twoPass = in.pass == CLPASS_ALPHA && pipelineHasDynamicRaster(pipe);
    const VkDescriptorSet cset = mOwner.set(mSlot);
    const VkDeviceSize cntOff = (VkDeviceSize)in.view * kViewWords * 4;
    const VkDeviceSize recOff = ((VkDeviceSize)alphaBaseRecords() + (VkDeviceSize)(1 + k) * mAlphaCap) * kDrawWords * 4;
    const VkDeviceSize cnt = cntOff + (VkDeviceSize)(kCntAlCount + k) * 4;
    const VkDeviceSize tasks = cntOff + (VkDeviceSize)(kCntAlTasks + k * 4) * 4;
    // mesh-tier push: bucket field 2 (pre / rig alpha) or 3 (post), list = in.list
    const uint32_t pad1 = (k == kAlOutPost ? 3u : 2u) | (in.list << 4);
    if (in.pass >= CLPASS_SHADOW && in.pass <= CLPASS_PROJ) ++mPassDraws[passKind(in.pass) * 2 + 1];
    VkCommandBuffer cmd = in.cmd;
    const uint32_t passes = twoPass ? 2u : 1u;
#ifdef VK_EXT_mesh_shader
    if (t == CLT_RASTER_MESH)
    {
        const VkPipelineLayout ml = rtv ? mMeshLayoutRt : mMeshLayout;
        if (!ml || !mDrawMeshTasksIndirect) return false;
        const VkDescriptorSet sets[4] = {in.draw_sets[0], in.draw_sets[1], rtv ? in.rt_set : cset, cset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ml, 0, rtv ? 4u : 3u, sets, 0, nullptr);
        bindGraphicsPipeline(cmd, pipe);
        for (uint32_t ps = 0; ps < passes; ++ps)
        {
            if (twoPass) vkCmdSetCullMode(cmd, ps == 0 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
            const DrawPush16 p{in.draw_view, (twoPass && ps == 0) ? 1u : 0u, in.view, pad1};
            vkCmdPushConstants(cmd, ml, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(p), &p);
            mDrawMeshTasksIndirect(cmd, mCounters.buffer, tasks, 1, 12);
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayout, 0, 2, in.draw_sets, 0, nullptr);
        const DrawPush16 restore{in.draw_view, 0u, 0u, 0u};
        vkCmdPushConstants(cmd, mDrawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(restore),
                           &restore);
        return true;
    }
#endif
    const VkPipelineLayout dl = rtv ? mDrawLayoutRt : mDrawLayout;
    if (!dl) return false;
    if (rtv)
    {
        const VkDescriptorSet s2[2] = {in.rt_set, cset};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayoutRt, 2, 2, s2, 0, nullptr);
    }
    else
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayout, 2, 1, &cset, 0, nullptr);
    if (t == CLT_RASTER_MDI_U8)
    {
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &pool, &zero);
        vkCmdBindIndexBuffer(cmd, pool, 0, VK_INDEX_TYPE_UINT8_EXT);
    }
    bindGraphicsPipeline(cmd, pipe);
    for (uint32_t ps = 0; ps < passes; ++ps)
    {
        if (twoPass)
        {
            vkCmdSetCullMode(cmd, ps == 0 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
            const uint32_t f8 = ps == 0 ? 1u : 0u;   // (the renderer's push word: VKDrawViewPush::f8)
            vkCmdPushConstants(cmd, dl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 4, 4, &f8);
        }
        if (t == CLT_RASTER_MDI_U8)
            vkCmdDrawIndexedIndirectCount(cmd, mDraws.buffer, recOff, mCounters.buffer, cnt, mAlphaCap, kDrawWords * 4);
        else
            vkCmdDrawIndirectCount(cmd, mDraws.buffer, recOff, mCounters.buffer, cnt, mAlphaCap, kDrawWords * 4);
    }
    return true;
}

// =============================================================================================
// GPU pick of cluster-drawn objects (#702 integration; clean_cluster_pick.comp, llcleanpickvk.h)
// =============================================================================================
bool LLCleanClusterSelectVK::ensurePickList()
{
    if (mPickList.buffer) return true;
    VkDeviceContext& ctx = mOwner.ctx();
    {
        VkMemScope mem(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        mPickList = ctx.createBuffer((VkDeviceSize)(4 + kPickCap * kDrawWords) * 4,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                         | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*deviceAddress*/ true);
    }
    if (mPickList.buffer && !mPickList.address) mOwner.ctx().destroyBuffer(mPickList);
    if (!mPickList.buffer) return false;   // (soft: retried on the next pick)
    // (every slot's set: the pick may run between frames against the last frame's slot)
    mOwner.setBuffer(CLB_PICK, mPickList);
    mPickListFrame = mFrame;
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

bool LLCleanClusterSelectVK::pickReady(VkDescriptorSetLayout out_dsl)
{
    // (the conditions recordPickFilter checks: a caller that saw true gets the filter)
    if (!mReady || mPickFailed || !mPickFilterPipe || !out_dsl || mPickFrame != mFrame || mPickView == kNoView
        || !mCounters.buffer || !ensurePickList() || mFrame < mPickListFrame + kSlots)
        return false;
    VkDevice dev = mOwner.ctx().device();
    // The draw pipeline compiles in the background (first use off the render thread): the
    // pick keeps the legacy candidates until it lands.
    if (mPickJob.pending())
    {
        if (!mPickJob.poll()) return false;
        mPickDrawPipe = mPickStage;
        mPickStage = VK_NULL_HANDLE;
        if (!mPickDrawPipe)
        {
            mPickFailed = true;
            LL_WARNS("Vulkan") << "Cluster LOD select: pick pipeline failed; cluster-drawn objects pick legacy only" << LL_ENDL;
            return false;
        }
        LL_INFOS("Vulkan") << "Cluster LOD select: GPU pick of cluster-drawn objects ready" << LL_ENDL;
    }
    if (mPickDrawPipe && mPickOutDsl == out_dsl) return true;
    const LLClusterRenderEnv& env = mOwner.env();
    if (!env.drawSet0 || !env.drawSet1) return false;
    if (mPickDrawPipe || mPickLayout)
    {
        vkDeviceWaitIdle(dev);   // the pick's out-set layout changed under it (rare): rebuild
        if (mPickDrawPipe) vkDestroyPipeline(dev, mPickDrawPipe, nullptr);
        if (mPickLayout) vkDestroyPipelineLayout(dev, mPickLayout, nullptr);
        mPickDrawPipe = VK_NULL_HANDLE;
        mPickLayout = VK_NULL_HANDLE;
    }
    const VkDescriptorSetLayout sets[4] = {env.drawSet0, env.drawSet1, mOwner.setLayout(), out_dsl};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, env.drawPushBytes};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 4;
    li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mPickLayout, "mClusterPickLayout") != VK_SUCCESS)
    {
        mPickLayout = VK_NULL_HANDLE;
        mPickFailed = true;
        LL_WARNS("Vulkan") << "Cluster LOD select: pick layout failed; cluster-drawn objects pick legacy only" << LL_ENDL;
        return false;
    }
    VkShaderModule vs = makeModule(dev, kWorldClusterVertPickSpv, kWorldClusterVertPickSpv_bytes, "kWorldClusterVertPickSpv");
    VkShaderModule fs = makeModule(dev, kWorldPickFragClusterSpv, kWorldPickFragClusterSpv_bytes, "kWorldPickFragClusterSpv");
    if (vs && fs)
    {
        // = the pick's own draw state (llcleanpickvk.cpp ensureDraw): 1x1 R8 target, no colour
        // write, no depth, cull none (facing decided in the FS).
        GraphicsPipelineDesc d;
        d.layout = mPickLayout;
        d.vert = vs;
        d.frag = fs;
        d.colorFormat = VK_FORMAT_R8_UNORM;
        d.colorWrite = false;
        d.depthFormat = VK_FORMAT_UNDEFINED;
        d.depthTest = false; d.depthWrite = false; d.blend = false;
        d.cullMode = VK_CULL_MODE_NONE;
        PipelineBatchScope batchScope(dev);
        createGraphicsPipelineInto(dev, d, &mPickStage, "cluster pick");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
        vs = fs = VK_NULL_HANDLE;
        PipelineJobOpts o;
        o.what = "cluster pick";
        mPickJob.start(pipelineBatchFlushAsync(dev, o));
    }
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    if (!mPickJob.pending())
    {
        mPickFailed = true;
        LL_WARNS("Vulkan") << "Cluster LOD select: pick pipeline failed; cluster-drawn objects pick legacy only" << LL_ENDL;
        return false;
    }
    mPickOutDsl = out_dsl;
    return false;   // (true once the job landed: the next call)
}

bool LLCleanClusterSelectVK::recordPickFilter(VkCommandBuffer cmd, const float org[3], const float dir[3], float t0,
                                              float t1, float cone)
{
    // The last main view (this frame's, or -- between frames, pickNow -- the last recorded
    // frame's: its slot's set, the single arena / counters / skinned arena hold its data).
    if (!mReady || !mPickFilterPipe || mPickFrame != mFrame || mPickView == kNoView || !mCounters.buffer
        || !ensurePickList() || mFrame < mPickListFrame + kSlots)
        return false;
    VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // (earlier pick draws / filters of the list -> the reset; the selection's counters and
    // entries are ordered by its own final barrier, which includes compute)
    barrier(cmd, readers, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, mPickList.buffer, 0, 16, 0u);
    barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet set = mOwner.set(mPickSlot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPickFilterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPickCompLayout, 0, 1, &set, 0, nullptr);
    PickPush p{};
    p.org[0] = org[0]; p.org[1] = org[1]; p.org[2] = org[2]; p.org[3] = t0;
    p.dir[0] = dir[0]; p.dir[1] = dir[1]; p.dir[2] = dir[2]; p.dir[3] = t1;
    p.view = mPickView;
    p.cap = kPickCap;
    p.cone = std::isfinite(cone) ? std::clamp(cone, 0.f, 0.1f) : 0.f;
    vkCmdPushConstants(cmd, mPickCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatchIndirect(cmd, mCounters.buffer, ((VkDeviceSize)mPickView * kViewWords + kCntDispatch) * 4);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    ++mPickRuns;
    return true;
}

void LLCleanClusterSelectVK::recordPickDraw(VkCommandBuffer cmd, const VkDescriptorSet draw_sets[2],
                                            VkDescriptorSet out_set, const uint32_t push[4], uint32_t round)
{
    if (!mPickDrawPipe || !mPickLayout || !mPickList.buffer || !mOwner.registeredBuffer(CLB_POOL)) return;
    const VkDescriptorSet sets[4] = {draw_sets[0], draw_sets[1], mOwner.set(mPickSlot), out_set};
    bindGraphicsPipeline(cmd, mPickDrawPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPickLayout, 0, 4, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mPickLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, push);
    // tier-0 records (VkDrawIndirectCommand + 1 spare word): the vertex-pulling VS.
    // (count word 0 = round 1; word 1 = round 2, armed by the pick's window pass on a world overflow)
    vkCmdDrawIndirectCount(cmd, mPickList.buffer, 16, mPickList.buffer, round ? 4 : 0, kPickCap, kDrawWords * 4);
}

LLCleanClusterSelectVK::ViewSlice LLCleanClusterSelectVK::viewSlice(uint32_t view) const
{
    return (view < mViewCount && mViewRecs[view].valid) ? mViewRecs[view].slice : ViewSlice{};
}

bool LLCleanClusterSelectVK::legacyExclusion(uint64_t& address, uint32_t& words) const
{
    if (!mReady || !mExclude || !mOwnedAny || !mOwned[mSlot].address) return false;
    address = mOwned[mSlot].address;
    words = std::min<uint32_t>((uint32_t)mCpuOwned.size(), mOwnedWords[mSlot]);
    return words != 0;
}

const char* LLCleanClusterSelectVK::selectMarkName() const
{
    static const char* const k[4] = {"c_cluster", "c_cluster_t0", "c_cluster_t1", "c_cluster_t2"};
    return k[std::clamp(drawTier(), -1, 2) + 1];
}

const char* LLCleanClusterSelectVK::drawMarkName() const
{
    static const char* const k[4] = {"g_gbuf_cluster", "g_gbuf_cluster_t0", "g_gbuf_cluster_t1", "g_gbuf_cluster_t2"};
    return k[std::clamp(drawTier(), -1, 2) + 1];
}


namespace
{
// "name live/cap" in MB with one decimal ("name cap" when live is not tracked: live < 0)
void memItem(std::string& out, const char* name, double live, uint64_t cap)
{
    char b[96];
    if (live < 0.0) std::snprintf(b, sizeof(b), " %s %.1f", name, (double)cap / 1048576.0);
    else std::snprintf(b, sizeof(b), " %s %.1f/%.1f", name, live / 1048576.0, (double)cap / 1048576.0);
    out += b;
}
} // namespace

void LLCleanClusterSelectVK::memSummary(std::string& out) const
{
    out += "select";
    // arena: the read-back demand (entries, all views) / capacity; draws follow the arena + rig /
    // alpha list caps; counters = 16 views x 96 words + the kMaxVirtualPages dedupe stamps (fixed)
    memItem(out, "sel", (double)mDemand * sizeof(ClusterSelected), mSelected.buffer ? mSelected.size : 0);
    memItem(out, "draws", -1.0, mDraws.buffer ? mDraws.size : 0);
    memItem(out, "counters", -1.0, mCounters.buffer ? mCounters.size : 0);
    uint64_t inst = 0, views = 0, owned = 0, rb = 0;
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        inst += mInst[s].buffer ? mInst[s].size : 0;
        views += mViews[s].buffer ? mViews[s].size : 0;
        owned += mOwned[s].buffer ? mOwned[s].size : 0;
        rb += mReadback[s].buffer ? mReadback[s].size : 0;
    }
    memItem(out, "inst", (double)mInstanceCount * sizeof(ClusterInstance) * kSlots, inst);
    memItem(out, "views+owned+rb", -1.0, views + owned + rb);
    memItem(out, "pick", -1.0, mPickList.buffer ? mPickList.size : 0);
    memItem(out, "instcut", (double)mInstanceCount * 8 * 4, mInstCut.buffer ? mInstCut.size : 0);
    char b[48];
    std::snprintf(b, sizeof(b), " (peak %.1f)", (double)mPeakBytes / 1048576.0);
    out += b;
}

void LLCleanClusterSelectVK::statsLine(std::string& out) const
{
    const int32_t t = drawTier();
    char flips[32];
    std::snprintf(flips, sizeof(flips), "%.1f", mStaticFlipsPerSec);
    out += "select inst " + std::to_string(mInstanceCount) + " (rigged " + std::to_string(mGathered[0]) + ", static "
         + std::to_string(mGathered[1]) + " [owned " + std::to_string(mStaticOwnedN) + ", flips " + flips
         + " / s], waiting for mesh " + std::to_string(mPending) + ") views "
         + std::to_string(mViewCount) + " draw tier " + (t >= 0 ? LLCleanClusterCaps::tierName(CLCAP_RASTER, t) : "none")
         + (mPipeJob ? " (compiling)" : "") + " main sel " + std::to_string(mLastSel) + " draws "
         + std::to_string(mLastDraws[0]) + "/" + std::to_string(mLastDraws[1]) + " arena " + std::to_string(mArena)
         + " demand " + std::to_string(mDemand) + " (peak " + std::to_string(mDemandPeak) + ") occl "
         + (mHizOk ? "on" : "off") + " excl " + (mExclude && mOwnedAny ? "on" : "off") + " rig "
         + (mRigFrame == mFrame ? "on" : "off") + " (cap " + std::to_string(mRigCap) + ") debug "
         + std::to_string(mDebug) + " held " + std::to_string(heldBytes() >> 10) + " KiB (peak "
         + std::to_string(mPeakBytes >> 10) + ")";
}

uint64_t LLCleanClusterSelectVK::heldBytes() const
{
    uint64_t b = mSelected.size + mDraws.size + mCounters.size + mPickList.size + mInstCut.size;
    for (uint32_t s = 0; s < kSlots; ++s) b += mInst[s].size + mViews[s].size + mOwned[s].size + mReadback[s].size;
    return b;
}
