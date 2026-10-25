/**
 * @file llcleanclustervk.cpp
 * @brief Cluster LOD (#642 / #743): the GPU module's thin owner -- descriptor set layout +
 *        per-slot sets from registered buffers, tiers, core buffers, per-frame order.
 */
#include "linden_common.h"

#include "llcleanclustervk.h"
#include "llcleanclusterselectvk.h"
#include "llcleanclusterstreamvk.h"
#include "llcleanclusterskinvk.h"
#include "../llcleanclusterbuild.h"
#include "../llcleanclustercache.h"

#include "llerror.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
using namespace LLClusterFmt;
// Per-slot stats block stride: a multiple of every device's minStorageBufferOffsetAlignment
// (<= 256 by the spec).
constexpr VkDeviceSize kStatsStride = 256;
static_assert(CLS_COUNT * 4u <= kStatsStride, "stats block");
// Joint-stretch placeholder: {rows = 0, pad x3} -> clJointStretch returns 1.
constexpr VkDeviceSize kJointStretchDummyBytes = 16;

// Descriptor type per binding (clean_cluster_common.glsl). Every binding 0..CLB_COUNT-1
// exists; a reserved one that must not be a storage buffer gets its type here -- the only
// edit other modules make in this file.
VkDescriptorType bindingType(uint32_t b)
{
    return b == CLB_HIZ ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

int32_t forceTierSetting()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<S32> s_force(gSavedSettings, "RenderVKClusterForceTier", -1);
    return (S32)s_force;
#else
    return -1;
#endif
}
} // namespace

LLCleanClusterVK::LLCleanClusterVK() = default;
LLCleanClusterVK::~LLCleanClusterVK() = default;

bool LLCleanClusterVK::init(VkDeviceContext& ctx, const LLClusterRenderEnv& env)
{
    mCtx = &ctx;
    mEnv = env;
    mCaps = LLCleanClusterCaps::detect(ctx, forceTierSetting());
    LL_INFOS("Vulkan") << "Cluster LOD: module created. " << mCaps.logLine() << LL_ENDL;

    mStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
#ifdef VK_EXT_mesh_shader
    if (mCaps.feat.meshShader) mStages |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
#endif
    const VkDevice dev = ctx.device();
    VkDescriptorSetLayoutBinding b[kBindingCount]{};
    VkDescriptorBindingFlags fl[kBindingCount]{};
    for (uint32_t i = 0; i < kBindingCount; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = bindingType(i);
        b[i].descriptorCount = 1;
        b[i].stageFlags = mStages;
        // Every binding may be unwritten while its owner has no buffer yet (and the Hi-Z
        // while the main view has none): shaders only touch bindings their dispatch needs.
        fl[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = kBindingCount;
    bf.pBindingFlags = fl;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.bindingCount = kBindingCount;
    li.pBindings = b;
    if (vkdCreateSetLayout(dev, &li, &mDsl, "mDslCluster") != VK_SUCCESS)
    {
        mDsl = VK_NULL_HANDLE;
        LL_WARNS("Vulkan") << "Cluster LOD: set layout creation failed; module off" << LL_ENDL;
        destroy();
        return false;
    }
    if (mPool.create(dev, "cluster", {{mDsl, kFrameSlots}}) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Cluster LOD: descriptor pool creation failed; module off" << LL_ENDL;
        destroy();
        return false;
    }
    for (uint32_t s = 0; s < kFrameSlots; ++s)
    {
        if (mPool.allocate(mDsl, &mSets[s]) != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Cluster LOD: descriptor set allocation failed; module off" << LL_ENDL;
            destroy();
            return false;
        }
    }
    ensureCoreBuffers();
    if (!mStats.buffer || !mStatsRead.buffer || !mJointStretchDummy.buffer)
    {
        LL_WARNS("Vulkan") << "Cluster LOD: core buffers unavailable (VRAM); module off" << LL_ENDL;
        destroy();
        return false;
    }

    mStream = std::make_unique<LLCleanClusterStreamVK>(*this);
    mSelect = std::make_unique<LLCleanClusterSelectVK>(*this);
    mSkin = std::make_unique<LLCleanClusterSkinVK>(*this);   // 2a (#747)
    if (!mStream->init() || !mSelect->init() || !mSkin->init())
    {
        LL_WARNS("Vulkan") << "Cluster LOD: streaming / selection / skinning init failed; module off" << LL_ENDL;
        destroy();
        return false;
    }
    mReady = true;
    return true;
}

void LLCleanClusterVK::ensureCoreBuffers()
{
    VkMemScope scope(VKMEM_CULL);
    VkDeviceContext::SoftAllocScope soft(mCtx);
    if (!mStats.buffer)
    {
        mStats = mCtx->createBuffer(kStatsStride * kFrameSlots,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for (uint32_t s = 0; s < kFrameSlots && mStats.buffer; ++s)
            setBuffer(CLB_STATS, mStats, kStatsStride * s, kStatsStride, s);
    }
    if (!mStatsRead.buffer)
    {
        VkMemScope rscope(VKMEM_READBACK);
        mStatsRead = mCtx->createBuffer(kStatsStride * kFrameSlots, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mStatsRead.buffer && !mStatsRead.mapped) mCtx->destroyBuffer(mStatsRead);
        if (mStatsRead.mapped) std::memset(mStatsRead.mapped, 0, kStatsStride * kFrameSlots);
    }
    if (!mJointStretchDummy.buffer)
    {
        VkMemScope sscope(VKMEM_RIG);
        mJointStretchDummy = mCtx->createBuffer(kJointStretchDummyBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mJointStretchDummy.buffer && !mJointStretchDummy.mapped) mCtx->destroyBuffer(mJointStretchDummy);
        if (mJointStretchDummy.mapped)
        {
            std::memset(mJointStretchDummy.mapped, 0, kJointStretchDummyBytes);
            setBuffer(CLB_JOINT_STRETCH, mJointStretchDummy);
        }
    }
}

void LLCleanClusterVK::destroy()
{
    mReady = false;
    if (mSkin) { mSkin->destroy(); mSkin.reset(); }
    if (mSelect) { mSelect->destroy(); mSelect.reset(); }
    if (mStream) { mStream->destroy(); mStream.reset(); }
    if (!mCtx) return;
    freeRetired(true);
    if (mStats.buffer) mCtx->destroyBuffer(mStats);
    if (mStatsRead.buffer) mCtx->destroyBuffer(mStatsRead);
    if (mJointStretchDummy.buffer) mCtx->destroyBuffer(mJointStretchDummy);
    mPool.destroy();
    for (VkDescriptorSet& s : mSets) s = VK_NULL_HANDLE;
    if (mDsl) { vkdDestroySetLayout(mCtx->device(), mDsl); mDsl = VK_NULL_HANDLE; }
    for (uint32_t s = 0; s < kFrameSlots; ++s)
        for (uint32_t i = 0; i < kBindingCount; ++i) mWant[s][i] = mHave[s][i] = Bound{};
}

void LLCleanClusterVK::setBuffer(uint32_t binding, const GpuBuffer& buf, VkDeviceSize offset, VkDeviceSize range,
                                 uint32_t slot)
{
    if (binding >= kBindingCount || binding == CLB_HIZ) return;
    Bound w;
    w.buffer = buf.buffer;
    w.serial = buf.serial;
    w.offset = offset;
    w.range = range;
    for (uint32_t s = 0; s < kFrameSlots; ++s)
        if (slot == kAllSlots || slot == s) mWant[s][binding] = w;
}

void LLCleanClusterVK::retire(GpuBuffer& buf)
{
    if (!buf.buffer) return;
    mRetired.push_back({buf, mFrame});
    buf = GpuBuffer{};
}

void LLCleanClusterVK::freeRetired(bool all)
{
    if (!mCtx) return;
    size_t keep = 0;
    for (size_t i = 0; i < mRetired.size(); ++i)
    {
        if (all || mFrame >= mRetired[i].frame + kFrameSlots) mCtx->destroyBuffer(mRetired[i].buf);
        else mRetired[keep++] = mRetired[i];
    }
    mRetired.resize(keep);
}

// The slot's frame finished (its fence was waited): bring its set up to date.
void LLCleanClusterVK::writeSlotSet(uint32_t s)
{
    VkWriteDescriptorSet w[kBindingCount]{};
    VkDescriptorBufferInfo bi[kBindingCount]{};
    VkDescriptorImageInfo ii{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < kBindingCount; ++i)
    {
        if (i == CLB_HIZ) continue;
        const Bound& want = mWant[s][i];
        Bound& have = mHave[s][i];
        if (!want.buffer) continue;   // never written yet: PARTIALLY_BOUND
        if (want.buffer == have.buffer && want.serial == have.serial && want.offset == have.offset && want.range == have.range)
            continue;
        bi[n] = {want.buffer, want.offset, want.range};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = mSets[s];
        w[n].dstBinding = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n].pBufferInfo = &bi[n];
        have = want;
        ++n;
    }
    if (mHizWant && mHizSampler
        && (mHizView[s] != mHizWant || mHizSize[s] != mHizWantSize || mHizGenSet[s] != mHizWantGen))
    {
        // (#745: the renderer's Hi-Z pyramid lives in GENERAL -- it is a storage image its
        // build writes -- and is sampled in that layout)
        ii = {mHizSampler, mHizWant, VK_IMAGE_LAYOUT_GENERAL};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = mSets[s];
        w[n].dstBinding = CLB_HIZ;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &ii;
        mHizView[s] = mHizWant;
        mHizSize[s] = mHizWantSize;
        mHizGenSet[s] = mHizWantGen;
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mCtx->device(), n, w, 0, nullptr);
}

void LLCleanClusterVK::harvestStats(uint32_t s)
{
    if (!mStatsRead.mapped) return;
    // (round 1 copied an uninitialized block; from round 2 on it holds a zeroed + used one)
    if (mStatsRounds[s] >= 2)
        std::memcpy(mStatsLast, (const uint8_t*)mStatsRead.mapped + kStatsStride * s, sizeof(mStatsLast));
}

void LLCleanClusterVK::tick(const LLClusterFrameIn& in)
{
    if (!mReady) return;
    const auto t0 = std::chrono::steady_clock::now();
    // #749: the previous tick's GPU half was never recorded (a recording that ended between
    // the renderer's two hooks): its planned copies / table writes never ran -- the same
    // recovery as a dropped frame, so no page flips resident without its copy.
    if (mTicked)
    {
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD: a frame start was never recorded; residency restarts" << LL_ENDL;
        if (mStream) mStream->frameDropped();
        if (mSelect) mSelect->frameDropped();
        if (mSkin) mSkin->frameDropped();
        mTicked = false;
    }
    mSlot = in.slot % kFrameSlots;
    mFrame = in.frame;
    mIdleSince = 0;
    if (mCaps.applyForce(forceTierSetting()))
        LL_INFOS("Vulkan") << "Cluster LOD: " << mCaps.logLine() << LL_ENDL;
    freeRetired(false);
    harvestStats(mSlot);

    // Hi-Z import (the main view's pyramid; null = no occlusion test this frame).
    mHizWant = in.hiz_view;
    mHizSampler = in.hiz_sampler;
    mHizWantSize = (in.hiz_w & 0xFFFFu) | (in.hiz_h << 16);
    if (in.hiz_view) mHizWantGen = in.hiz_gen;

    // The controller (#749) first: the gatherer reads its avatar protection (tol_scale), the
    // views its tolerance / LOD camera.
    using Clock = std::chrono::steady_clock;
    const auto msBetween = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const Clock::time_point c0 = Clock::now();
    mControl.update(in.pressure, in.now, in.offscreen || in.snapshot);
    mControl.setMainView(in.main_cam, in.main_px_scale, in.main_h, in.znear);

    // CPU side first: streaming decides residency + uploads, then selection gathers the
    // instances (it asks streaming for meshes). A snapshot's 2nd+ recording changes no
    // residency, but selection still runs: its per-slot buffers (instances, views) live in
    // THIS slot, so it re-uploads its cached list without re-gathering (in.offscreen).
    // (#1011 PROFILE: each module's share of the tick, CLOD summary "cpu tick ... (ctl / stream /
    // select / skin)")
    const Clock::time_point c1 = Clock::now();
    if (!in.offscreen) mStream->beginFrame(in);
    const Clock::time_point c2 = Clock::now();
    mSelect->beginFrame(in);
    const Clock::time_point c3 = Clock::now();
    mSkin->beginFrame(in);   // 2a: after the gather (its per-avatar switch), before the set write
    const Clock::time_point c4 = Clock::now();
    mTickCtlMs += msBetween(c0, c1);
    mTickStreamMs += msBetween(c1, c2);
    mTickSelectMs += msBetween(c2, c3);
    mTickSkinMs += msBetween(c3, c4);
    writeSlotSet(mSlot);
    mOffscreen = in.offscreen;
    mTicked = true;
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        mTickMsAcc += ms;
        mTickMsMax = std::max(mTickMsMax, ms);
        ++mTickN;
    }
    logSummary(in);
}

bool LLCleanClusterVK::uploadsPlanned() const
{
    return mReady && mTicked && !mOffscreen && mStream && mStream->plannedCopies() != 0;
}

// #749 (frame pipelining): the GPU half of the frame start, recorded into the command buffer
// the cluster selection of this frame runs in (see the header). Every write below is read by
// this frame's selection / draws only after it in the same command buffer (or, for 1c's page
// copies on a forked segment, only after their fence-gated flip kFrameSlots frames later).
void LLCleanClusterVK::recordFrameStart(VkCommandBuffer cmd, bool cmd_async)
{
    if (!mReady || !mTicked || !cmd)
    {
        if (mStream) mStream->setUploadCmd(VK_NULL_HANDLE);
        return;
    }
    mTicked = false;
    (void)cmd_async;   // (1c reads the queue family of cmd / its upload buffer itself)
    // Stats: this slot's block (frame N - kFrameSlots) -> readback, then zero it.
    {
        VkBufferCopy c{kStatsStride * mSlot, kStatsStride * mSlot, kStatsStride};
        vkCmdCopyBuffer(cmd, mStats.buffer, mStatsRead.buffer, 1, &c);
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.buffer = mStats.buffer; b.offset = kStatsStride * mSlot; b.size = kStatsStride;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        vkCmdFillBuffer(cmd, mStats.buffer, kStatsStride * mSlot, kStatsStride, 0);
        mStatsRounds[mSlot] = (uint8_t)std::min(2, mStatsRounds[mSlot] + 1);
    }
    if (!mOffscreen) mStream->recordUploads(cmd);
    else mStream->setUploadCmd(VK_NULL_HANDLE);
    mSelect->recordBegin(cmd);
    {   // stats zero -> any shader stage that bumps them
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
}

uint32_t LLCleanClusterVK::recordCull(VkCommandBuffer cmd, const LLClusterViewIn& view)
{
    if (!mReady) return kNone;
    // FIXED-COST AUDIT (2026-10-14): a SECONDARY view (sun cascade, mirror) selects STATIC
    // instances drawn for real only -- rigged ones come from the main view's shared rig cut. With
    // none this frame (RenderVKClusterStatic 0, the default) its selection was a full dispatch over
    // every ClusterInstance + finalize + three barriers + a counters readback + an arena slice
    // reservation, per re-rendered cascade and for the mirror, for nothing. kNone = the pass draws
    // no cluster statics and its legacy cull runs without the exclusion: exactly what an empty
    // view gave (clusterStaticExclusion / viewDrawReady are false for kNone).
    if (!(view.flags & CLV_MAIN) && !(view.flags & CLV_RT) && mSelect && mSelect->staticDrawn() == 0)
    {
        ++mViewsSkipped;
        return kNone;
    }
    // 2a (#747): once per frame, before the first selection -- the joint-stretch table and the
    // rigged instances' posed-bound margins, in the same command buffer as the selection (the
    // main view is the first recordCull of a frame: in the front-end segment under frame
    // pipelining, after recordFrameStart). No-op on later calls.
    mSkin->recordBounds(cmd);
    // #749: the view's tolerance (role scale x the controller's knob) and its LOD camera
    // (lagged, with the conservative margin) + held px_scale. RT / other views as given.
    LLClusterViewIn v = view;
    v.lod_cam[0] = v.cam_pos[0]; v.lod_cam[1] = v.cam_pos[1]; v.lod_cam[2] = v.cam_pos[2]; v.lod_cam[3] = 0.f;
    if (v.role < CLROLE_OTHER && !(v.flags & CLV_RT))
        mControl.applyView(v.role, v.sub, v.cam_pos, (v.flags & CLV_ORTHO) != 0u, v.px_scale, v.tol_px, v.lod_cam);
    const uint32_t idx = mSelect->recordSelect(cmd, v);
    if ((v.flags & CLV_MAIN) && !(v.flags & CLV_ORTHO) && idx != kNone)
    {   // (shadowLiftBound: the projection this selection used)
        mMainTolPx = v.tol_px;
        mMainPxScale = v.px_scale;
        mMainZnear = v.znear;
        mMainViewFrame = mFrame;
        mMainView = idx;
    }
    return idx;
}

bool LLCleanClusterVK::shadowLiftBound(float& m_per_m, float& znear) const
{
    m_per_m = 0.f;
    znear = 0.1f;
    if (!mReady || !mSkin || mMainViewFrame != mFrame || mSkin->avatarsOnCluster() == 0) return false;
    if (!(mMainPxScale > 0.f) || !std::isfinite(mMainTolPx) || !(mMainTolPx > 0.f)) return false;
    m_per_m = mMainTolPx / mMainPxScale;
    znear = std::max(mMainZnear, 1.0e-3f);
    return std::isfinite(m_per_m);
}

void LLCleanClusterVK::frameDropped()
{
    if (!mReady) return;
    LL_WARNS("Vulkan") << "Cluster LOD: frame dropped (#697): residency restarts, the lost page copies and "
                          "table writes are re-streamed" << LL_ENDL;
    if (mStream) mStream->frameDropped();
    if (mSelect) mSelect->frameDropped();
    if (mSkin) mSkin->frameDropped();
    // The dropped slot's stats block was neither copied nor zeroed: its next readback would
    // carry two frames' counts. Every slot starts its rounds again.
    for (uint8_t& r : mStatsRounds) r = 0;
    mTicked = false;
}

bool LLCleanClusterVK::recordDraw(const LLClusterDrawIn& in)
{
    if (!mReady || in.view == kNone) return false;
    return mSelect->recordDraw(in);
}

// ---- #702 integration: secondary views, the shared rig cut, the GPU pick ----------------------
void LLCleanClusterVK::setRigLists(const uint32_t bits[2][kAvatarSlotWords], bool on, uint32_t emit)
{
    if (mReady && mSelect) mSelect->setRigLists(bits, on, emit);
}

bool LLCleanClusterVK::rigListReady(LLClusterDrawPass pass) const
{
    // (the skin module's pre-skin of THIS frame covered the main slice: its map is valid; the
    // captures draw rig list 1, the shadow passes list 0)
    // (rigDrawReady: every condition of the rig-list draw itself, so a pass that saw true draws)
    return mReady && mSelect && mSkin && mSelect->rigDrawReady(pass) && mSkin->preskinRecorded(mFrame);
}

bool LLCleanClusterVK::rigListWillDraw(LLClusterDrawPass pass) const
{
    return mReady && mSelect && mSkin && mSelect->rigDrawReady(pass) && mSkin->preskinWillRecord(mainView());
}

bool LLCleanClusterVK::recordAlphaOrder(VkCommandBuffer cmd, const float cam[3], float water, bool under)
{
    return mReady && mSelect && mSelect->recordAlphaOrder(cmd, mainView(), cam, water, under);
}

bool LLCleanClusterVK::alphaReady(LLClusterDrawPass pass) const
{
    // (secondary passes draw a rig list's alpha list: its skinning is the pre-skin of this frame)
    const bool rig = pass == CLPASS_SHADOW || pass == CLPASS_PROJ || pass == CLPASS_PROBE || pass == CLPASS_TRANS_DEPTH
                  || pass == CLPASS_TRANS_COLOR;
    return mReady && mSelect && mSkin && mSelect->alphaDrawReady(pass) && (!rig || mSkin->preskinRecorded(mFrame));
}

bool LLCleanClusterVK::viewDrawReady(uint32_t view, LLClusterDrawPass pass) const
{
    return mReady && mSelect && view != kNone && mSelect->viewSlice(view).sel_cap != 0 && mSelect->passReady(pass);
}

bool LLCleanClusterVK::pickReady(VkDescriptorSetLayout out_dsl)
{
    return mReady && mSelect && mSelect->pickReady(out_dsl);
}

bool LLCleanClusterVK::recordPickFilter(VkCommandBuffer cmd, const float org[3], const float dir[3], float t0, float t1,
                                        float cone)
{
    return mReady && mSelect && mSelect->recordPickFilter(cmd, org, dir, t0, t1, cone);
}

void LLCleanClusterVK::recordPickDraw(VkCommandBuffer cmd, const VkDescriptorSet draw_sets[2], VkDescriptorSet out_set,
                                      const uint32_t push[4], uint32_t round)
{
    if (mReady && mSelect) mSelect->recordPickDraw(cmd, draw_sets, out_set, push, round);
}

uint64_t LLCleanClusterVK::pickListAddress() const
{
    return mReady && mSelect ? mSelect->pickListAddress() : 0ull;
}

bool LLCleanClusterVK::idleRelease(uint64_t frame)
{
    // (1c) The first idle frame unregisters every mesh and hands the page pool / tables /
    // staging / readbacks to retire(); once they are freed (kFrameSlots frames) the renderer
    // deletes the module. Turned on again before that: tick() re-creates them lazily.
    mFrame = frame;
    if (!mIdleSince)
    {
        mIdleSince = frame ? frame : 1;
        if (mStream) mStream->releaseAll();
    }
    freeRetired(false);
    return frame >= mIdleSince + kFrameSlots && mRetired.empty();
}

// #749 diagnostics: one compact summary line + one A/B line against the old rigged LOD
// controller + one main-view LOD line (full-detail baseline, faces at the DAG floor), every
// 10 s while the module runs (never offscreen). Kept short: bug-report excerpts keep vkgfx
// lines, but long lines went missing there (#739).
void LLCleanClusterVK::logSummary(const LLClusterFrameIn& in)
{
    if (in.offscreen || in.snapshot || !mReady) return;
    if (mSummaryAt > 0.0 && in.now - mSummaryAt < 10.0) return;
    mSummaryAt = in.now;
    const auto ms = [](float v) -> std::string
    {
        if (v < 0.f) return "-";
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", v);
        return b;
    };
    std::string s = "CLOD t r" + std::to_string(mCaps.get(CLCAP_RASTER)) + "c" + std::to_string(mCaps.get(CLCAP_CMDGEN))
                  + "u" + std::to_string(mCaps.get(CLCAP_UPLOAD)) + "rt" + std::to_string(mCaps.get(CLCAP_RT)) + "a"
                  + std::to_string(mCaps.get(CLCAP_ATOMIC64)) + " | ";
    mControl.summary(s);
    s += " | sel ";
    if (mSelect) mSelect->roleSummary(s);
    {   // the CPU frame setup (main thread): tick avg / max, (#1011) its split by module, the 1b
        // gather's share and work
        char tb[160];
        const double n = mTickN ? (double)mTickN : 1.0;
        std::snprintf(tb, sizeof(tb), " | cpu tick %.3f/%.2f ms (ctl %.3f stream %.3f select %.3f skin %.3f) ",
                      mTickN ? mTickMsAcc / mTickN : 0.0, mTickMsMax, mTickCtlMs / n, mTickStreamMs / n, mTickSelectMs / n,
                      mTickSkinMs / n);
        s += tb;
        if (mSelect) mSelect->gatherSummary(s);
        mTickMsAcc = mTickMsMax = 0.0;
        mTickN = 0;
        mTickCtlMs = mTickStreamMs = mTickSelectMs = mTickSkinMs = 0.0;
    }
    s += " | ";
    if (mStream) mStream->summary(s);
    const LLCleanClusterBuilder::Stats bs = LLCleanClusterBuilder::instance().stats();
    // builder: queued, in flight / worker threads, finished per second and their mean worker ms
    // (over ~5 s), meshes waiting for their source data (no CPU mirror / LOD yet) and
    // registrations refused last frame (registry full)
    char bb[160];
    std::snprintf(bb, sizeof(bb), " | build q%u b%u/%u %.1f/s %.1fms wait %u", bs.queued, bs.building, bs.threads,
                  bs.per_sec, bs.avg_ms, mStream ? mStream->buildWaitSource() : 0u);
    s += bb;
    if (mStream && mStream->regRefused()) s += " regfull " + std::to_string(mStream->regRefused());
    if (mStream) mStream->ttfdSummary(s);   // 2026-10-14: time to drawable per mesh
    s += " | gpu ms preskin "
       + ms(in.diag.preskin_ms) + " sel " + ms(in.diag.select_ms) + " pre " + ms(in.diag.prepass_ms) + " draw "
       + ms(in.diag.draw_ms);
    LL_INFOS("Vulkan") << s << LL_ENDL;
    // A/B against the old controller on the same scene (#561 / #613: decimation level A,
    // perf / memory creator-LOD pressure, the VRAM ladder's rigged rung).
    // Legacy-skin skip: "skip <rows> <verts>v av <skipped>/<switched>" + the reasons switched
    // avatars kept their legacy pre-skin (only the non-zero ones).
    std::string skip = " skip " + std::to_string(in.diag.skip_insts) + " " + std::to_string(in.diag.skip_verts) + "v av "
                     + std::to_string(in.diag.skip_av) + "/" + std::to_string(in.diag.switched_av);
    {
        static const char* const kWhy[8] = {"sun", "proj", "mir", "prb", "pick", "rt", "sel", "off"};
        std::string keep;
        for (uint32_t k = 0; k < 8; ++k)
            if (in.diag.keep_why[k]) keep += " " + std::string(kWhy[k]) + std::to_string(in.diag.keep_why[k]);
        if (!keep.empty()) skip += " keep" + keep;
        if (in.diag.rt_role == 1u) skip += " (rt: not tracing)";
        else if (in.diag.rt_role == 2u) skip += " (rt: cluster-only, no legacy consumer)";
    }
    LL_INFOS("Vulkan") << "CLOD A/B old: skinned verts " << in.diag.skinned_verts << skip << " rig gpu "
                       << ms(in.diag.rig_gpu_ms) << " ms A " << in.diag.rig_slod << " perfP " << in.diag.rig_perf_p
                       << " memP " << in.diag.rig_pressure << " rung " << in.diag.rig_ladder << " | new: tris "
                       << mStatsLast[CLS_TRIANGLES] << " verts " << mStatsLast[CLS_VERTICES] << " cluster gpu "
                       << ms(in.pressure.cluster_gpu_ms) << " ms (preskin " << ms(in.diag.preskin_ms) << ")" << LL_ENDL;
    // The main view's LOD against full detail (instances drawn for real): faces with a cluster
    // on screen, how many sit at their DAG root (the floor), the triangles drawn, their cut
    // before culling, and the LOD0 triangles of those faces (% = cut / full: the LOD alone).
    const uint32_t full = mStatsLast[CLS_MAIN_FULL_TRIS], cut = mStatsLast[CLS_MAIN_CUT_TRIS];
    char pct[16] = "-";
    if (full) std::snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * (double)cut / (double)full);
    // Rigged ALPHA-BLEND: the cluster path's alpha faces / triangles drawn in the main view (and
    // its list sizes) vs what is still legacy (faces the main cull drew, pass 2 triangles).
    std::string al;
    if (mSelect) mSelect->alphaSummary(al);
    al += " vs legacy faces " + (in.diag.legacy_alpha_faces >= 0 ? std::to_string(in.diag.legacy_alpha_faces) : std::string("-"))
        + " tris " + (in.diag.legacy_alpha_tris >= 0 ? std::to_string(in.diag.legacy_alpha_tris) : std::string("-"));
    LL_INFOS("Vulkan") << "CLOD main: faces " << mStatsLast[CLS_MAIN_INST] << " (root " << mStatsLast[CLS_MAIN_ROOT]
                       << ") tris " << mStatsLast[CLS_MAIN_SEL_TRIS] << " cut " << cut << " of " << full << " full ("
                       << pct << ") | " << al << LL_ENDL;
    // What per-cluster culling saves (every view's selection that frame): clusters in the cut,
    // and how many of them frustum / backface cone / Hi-Z occlusion removed.
    LL_INFOS("Vulkan") << "CLOD cull: cut " << mStatsLast[CLS_CUT] << " frustum " << mStatsLast[CLS_CULL_FRUSTUM]
                       << " cone " << mStatsLast[CLS_CULL_CONE] << " occl " << mStatsLast[CLS_CULL_OCCLUSION]
                       << " drawn " << mStatsLast[CLS_SELECTED] << LL_ENDL;
    // FIXED-COST AUDIT (2026-10-14): what the module holds whatever it draws -- per buffer
    // "live/cap" MB (live = the demand) and the total cluster VRAM (stream + skin + select + RT),
    // then the cluster GPU sub-timers of the last 5 s window (avg/worst ms; needs RenderVKGpuTimers
    // and RenderVKGpuSubMarks). Short lines (bug-report excerpts drop long ones, #739).
    {
        const uint64_t st = mStream ? mStream->heldBytes() : 0, sk = mSkin ? mSkin->heldBytes() : 0,
                       se = mSelect ? mSelect->heldBytes() : 0;
        char tb[160];
        std::snprintf(tb, sizeof(tb), "CLOD mem: total %.1f MB = stream %.1f + skin %.1f + select %.1f + RT %.1f |",
                      (double)(st + sk + se + in.diag.rt_bytes) / 1048576.0, (double)st / 1048576.0, (double)sk / 1048576.0,
                      (double)se / 1048576.0, (double)in.diag.rt_bytes / 1048576.0);
        std::string m = tb;
        if (mStream) { m += " "; mStream->memSummary(m); }
        LL_INFOS("Vulkan") << m << LL_ENDL;
        // #1010: the skinned arena under crowds (clusters dropped -- expected 0 --, avatars hidden
        // whole / coarsened, pressure, live/cap/peak MB, limit). Every 10 s.
        if (mSkin)
        {
            std::string a = "CLOD skin arena: ";
            mSkin->arenaSummary(a);
            LL_INFOS("Vulkan") << a << LL_ENDL;
        }
        m = "CLOD mem2:";
        if (mSkin) { m += " "; mSkin->memSummary(m); }
        if (mSelect) { m += " | "; mSelect->memSummary(m); }
        LL_INFOS("Vulkan") << m << LL_ENDL;
        if (!in.diag.rt_mem.empty()) LL_INFOS("Vulkan") << "CLOD mem RT: " << in.diag.rt_mem << LL_ENDL;
        LL_INFOS("Vulkan") << "CLOD gpu sub (5 s avg/worst ms): "
                           << (in.diag.gpu_sub.empty() ? std::string("- (RenderVKGpuTimers / RenderVKGpuSubMarks off)")
                                                       : in.diag.gpu_sub)
                           << LL_ENDL;
    }
    // #702 integration: the secondary views (static records per role, the shared rig lists,
    // the passes that drew clusters and the picks of the last 10 s). Short (bug-report excerpts).
    if (mSelect)
    {
        std::string vl = "CLOD views: ";
        mSelect->viewSummary(vl);
        // (fixed-cost audit: secondary selections not recorded -- no static drawn for real)
        if (mViewsSkipped) vl += " | secondary sel skipped " + std::to_string(mViewsSkipped) + " (no static)";
        mViewsSkipped = 0;
        LL_INFOS("Vulkan") << vl << LL_ENDL;
    }
}

std::string LLCleanClusterVK::statsLine()
{
    std::string s = "[cluster LOD: tiers r" + std::to_string(mCaps.get(CLCAP_RASTER)) + " c"
                  + std::to_string(mCaps.get(CLCAP_CMDGEN)) + " u" + std::to_string(mCaps.get(CLCAP_UPLOAD)) + " rt"
                  + std::to_string(mCaps.get(CLCAP_RT)) + " a" + std::to_string(mCaps.get(CLCAP_ATOMIC64));
    s += "; gpu inst " + std::to_string(mStatsLast[CLS_INSTANCES]) + " tested " + std::to_string(mStatsLast[CLS_TESTED])
       + " cut " + std::to_string(mStatsLast[CLS_CUT]) + " (fallback " + std::to_string(mStatsLast[CLS_FALLBACK])
       + ") selected " + std::to_string(mStatsLast[CLS_SELECTED]) + " (overflow " + std::to_string(mStatsLast[CLS_SEL_OVERFLOW])
       + ") tris " + std::to_string(mStatsLast[CLS_TRIANGLES]) + " requests " + std::to_string(mStatsLast[CLS_REQUESTS])
       + " (overflow " + std::to_string(mStatsLast[CLS_REQ_OVERFLOW]) + ")";
    s += "; ";
    mControl.statsLine(s);
    if (mStream) { s += "; "; mStream->statsLine(s); }
    if (mSelect) { s += "; "; mSelect->statsLine(s); }
    if (mSkin) { s += "; "; mSkin->statsLine(s); }
    s += "; " + LLCleanClusterCache::instance().statsLine() + "; " + LLCleanClusterBuilder::instance().statsLine() + "]";
    return s;
}
