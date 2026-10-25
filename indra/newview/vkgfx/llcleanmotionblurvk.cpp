/**
 * @file llcleanmotionblurvk.cpp
 * @brief Motion blur (#118): see llcleanmotionblurvk.h and the shaders
 *        clean_mblur_{tile,neighbor,recon}.comp / clean_mblur_writeback.frag; plus the
 *        renderer's side (LLCleanRendererVK::motionBlurWanted / recordMotionBlur).
 *
 * VELOCITY: the depth pre-pass writes full motion vectors (temporal #84, stages 1 + 2:
 * camera reprojection, moved / animated prims through the previous instance rows,
 * skinned avatars through the previous skinned positions / joint table) into mMotion.
 * This feature keeps that plumbing alive with TAA off (ensureTemporal ORs
 * motionBlurWanted()) and reads it, with two camera reprojections next to it: the full
 * one (mTaaReproj, the resolve's) and the rotation-only one (mMbReprojRot: last frame's
 * rotation / projection, this frame's eye) -- their difference is the translation part.
 *
 * SHUTTER: RenderVKMotionBlurShutter is an exposure time in seconds; the per-frame
 * motion is scaled by shutter / frame time (capped at 8 frames), so the look does not
 * depend on the frame rate (1/60 s at 144 FPS blurs ~2.4 frames of motion).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanmotionblurvk.h"
#include "llcleanrenderervk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr uint32_t kTile = 16;            // clean_mblur_common.glsl kTile (= the 16x16 workgroups)
// Active-tile threshold: a NeighborMax radius under half a pixel cannot move a sample.
constexpr float    kActivePx = 0.5f;
// MaxRadius is given at a 1080-line render (the look does not change with resolution).
constexpr float    kRefLines = 1080.f;
constexpr uint32_t kMaxReach = 8;         // NeighborMax reach cap (tiles): 128 px radius
constexpr uint32_t kRetryFrames = 300;    // after an allocation failure

// Bindings (clean_mblur_common.glsl): 0 params; 1 depth, 2 motion, 3 water mask, 4 scene;
// 5 / 6 velocity (storage / sampled), 7 / 8 TileMax, 9 / 10 NeighborMax, 11 / 12 the
// reconstruction; 13 dynamic exposure (storage, read-only).
constexpr uint32_t kBindings = 14;
bool isStorage(uint32_t b) { return b == 5 || b == 7 || b == 9 || b == 11 || b == 13; }

VkShaderModule mbModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);   // (names the batch report / straggler / crash lines)
    return m;
}
} // namespace

// FIRST USE OFF THE RENDER THREAD: samplers, set / pipeline layouts and the set are made
// here; the four pipelines go to an async batch (vk_pipeline.h PipelineJobSlot, staged in
// mStage) -- false (no motion blur those frames) until they land.
bool LLCleanMotionBlurVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mTilePipe = mStage[0]; mNeighborPipe = mStage[1]; mReconPipe = mStage[2]; mWritebackPipe = mStage[3];
        for (VkPipeline& p : mStage) p = VK_NULL_HANDLE;
        if (!mTilePipe || !mNeighborPipe || !mReconPipe || !mWritebackPipe)
        {
            LL_WARNS("VKMotionBlur") << "motion blur pipelines failed to build; effect disabled" << LL_ENDL;
            const VkFormat fmt = mWritebackFormat;
            destroy(dev);
            mCtx = f.ctx;
            mWritebackFormat = fmt;
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VKMotionBlur") << "motion blur pipelines built (background)" << LL_ENDL;
    }
    if (mTilePipe && mWritebackFormat == f.sceneFormat) return true;
    if (mPipeFailed && mWritebackFormat == f.sceneFormat) return false;
    if (mTilePipe || mDsl) destroy(dev);   // the scene format changed (rare): rebuild
    mCtx = f.ctx;
    mWritebackFormat = f.sceneFormat;
    PipelineBatchScope batchScope(dev);   // (the pipelines below: queued, flushed async at the end)

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mLinear));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mNearest));

    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1;
        // Storage images are compute-only (the per-stage storage image minimum is 4); the
        // write-back fragment shader reads NeighborMax + the reconstruction (sampled).
        b[i].stageFlags = isStorage(i) ? VK_SHADER_STAGE_COMPUTE_BIT
                                       : (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                            : isStorage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = kBindings; lci.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslMotionBlur"));
    VK_CHECK(mPool.create(dev, "motionBlur", {{mDsl, 1}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mSet), { destroy(dev); return false; });

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
    VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mMotionBlurLayout"));
    {
        VkShaderModule m = mbModule(dev, kWorldMblurTileSpv, kWorldMblurTileSpv_bytes, "kWorldMblurTileSpv");
        createComputePipelineInto(dev, m, mLayout, 0, nullptr, &mStage[0], "motion blur tiles");
        destroyShaderModuleDeferred(dev, m, nullptr);
        m = mbModule(dev, kWorldMblurNeighborSpv, kWorldMblurNeighborSpv_bytes, "kWorldMblurNeighborSpv");
        createComputePipelineInto(dev, m, mLayout, 0, nullptr, &mStage[1], "motion blur neighbour");
        destroyShaderModuleDeferred(dev, m, nullptr);
        m = mbModule(dev, kWorldMblurReconSpv, kWorldMblurReconSpv_bytes, "kWorldMblurReconSpv");
        createComputePipelineInto(dev, m, mLayout, 0, nullptr, &mStage[2], "motion blur reconstruct");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    // Write-back: fullscreen triangle into the scene alone (opaque, no depth).
    {
        VkShaderModule vs = mbModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule fs = mbModule(dev, kWorldMblurWritebackFragSpv, kWorldMblurWritebackFragSpv_bytes, "kWorldMblurWritebackFragSpv");
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mLayout;
        d.colorFormat = f.sceneFormat;
        d.depthTest = false; d.depthWrite = false;
        d.cullMode = VK_CULL_MODE_NONE;
        createGraphicsPipelineInto(dev, d, &mStage[3], "motion blur write-back");
        destroyShaderModuleDeferred(dev, vs, nullptr);
        destroyShaderModuleDeferred(dev, fs, nullptr);
    }
    PipelineJobOpts o;
    o.what = "motion blur";
    mJob.start(pipelineBatchFlushAsync(dev, o));
    LL_INFOS("VKMotionBlur") << "motion blur pipelines compiling in the background (the effect starts when they land)" << LL_ENDL;
    return false;
}

bool LLCleanMotionBlurVK::warm(VkDeviceContext* ctx, VkFormat sceneFormat)
{
    if (!ctx) return false;
    Frame f;
    f.ctx = ctx;
    f.sceneFormat = sceneFormat;
    ensurePipelines(f);
    return mJob.pending();
}

void LLCleanMotionBlurVK::destroyTargets()
{
    if (mCtx)
    {
        for (GpuImage* im : {&mVel, &mTileMax, &mNeighbor, &mOut})
            if (im->image) mCtx->destroyImage(*im);
        if (mParams.buffer) mCtx->destroyBuffer(mParams);
    }
    mVel = mTileMax = mNeighbor = mOut = GpuImage{};
    mParams = GpuBuffer{};
    mW = mH = mTilesX = mTilesY = 0;
    ++mOwnGen;
}

bool LLCleanMotionBlurVK::ensureTargets(VkCommandBuffer cmd, const Frame& f)
{
    const uint32_t tx = (f.rw + kTile - 1) / kTile, ty = (f.rh + kTile - 1) / kTile;
    const bool have = mParams.buffer && mVel.image && mTileMax.image && mNeighbor.image && mOut.image;
    if (!have || f.rw != mW || f.rh != mH)
    {
        if (!have && (int32_t)(mFrameNo - mRetryFrame) < 0) return false;   // failed recently
        VkDevice dev = f.ctx->device();
        vkDeviceWaitIdle(dev);   // the set / images may be in use by frames in flight
        destroyTargets();
        {
            VkMemScope memcat(VKMEM_TARGET);   // #407
            VkDeviceContext::SoftAllocScope soft(mCtx);   // a failure turns the effect off, never aborts
            const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            mParams = mCtx->createBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mParams.buffer) mVel = mCtx->createImage2D(f.rw, f.rh, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
            if (mVel.image) mOut = mCtx->createImage2D(f.rw, f.rh, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
            if (mOut.image) mTileMax = mCtx->createImage2D(tx, ty, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
            if (mTileMax.image) mNeighbor = mCtx->createImage2D(tx, ty, VK_FORMAT_R16G16B16A16_SFLOAT, u, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (!mParams.buffer || !mVel.image || !mOut.image || !mTileMax.image || !mNeighbor.image)
        {
            LL_WARNS("VKMotionBlur") << "motion blur targets (" << f.rw << "x" << f.rh
                                     << ") failed to allocate; effect off, retry in " << kRetryFrames << " frames" << LL_ENDL;
            destroyTargets();
            mRetryFrame = mFrameNo + kRetryFrames;
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mVel.image, "mblurVelocity");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mOut.image, "mblurReconstruction");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mTileMax.image, "mblurTileMax");
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mNeighbor.image, "mblurNeighborMax");
        mW = f.rw; mH = f.rh; mTilesX = tx; mTilesY = ty;
        // GENERAL for good (compute writes; compute / fragment reads).
        VkImageMemoryBarrier2 ib[4]{};
        const VkImage imgs[4] = {mVel.image, mOut.image, mTileMax.image, mNeighbor.image};
        for (int i = 0; i < 4; ++i)
        {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib[i].image = imgs[i]; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 4; d.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    // The set: rewritten when an own resource or a renderer target was re-created (the
    // generations), or a renderer view differs (second check only).
    const VkImageView views[6] = {f.sceneView, f.depthView, f.motionView, f.reactView, f.exposureView, mOut.view};
    if (mSetOwnGen != mOwnGen || mSetTargetGen != f.targetGen || mSetDepthGen != f.depthGen
        || std::memcmp(views, mSetViews, sizeof(views)) != 0)
    {
        if (mSetOwnGen == mOwnGen) vkDeviceWaitIdle(f.ctx->device());   // (a re-create above already waited)
        mSceneView = f.sceneView;
        std::memcpy(mSetViews, views, sizeof(views));
        mSetOwnGen = mOwnGen; mSetTargetGen = f.targetGen; mSetDepthGen = f.depthGen;
        mBindDepth = f.depthView; mBindMotion = f.motionView; mBindReact = f.reactView; mBindExposure = f.exposureView;
        writeSet();
    }
    return true;
}

void LLCleanMotionBlurVK::writeSet()
{
    if (!mCtx || !mSet) return;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, G = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bi{mParams.buffer, 0, sizeof(Params)};
    VkDescriptorImageInfo ii[kBindings] = {
        {},
        {mNearest, mBindDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {mNearest, mBindMotion, RO},
        {mNearest, mBindReact, RO},
        {mLinear, mSceneView, RO},
        {VK_NULL_HANDLE, mVel.view, G},
        {mNearest, mVel.view, G},
        {VK_NULL_HANDLE, mTileMax.view, G},
        {mNearest, mTileMax.view, G},
        {VK_NULL_HANDLE, mNeighbor.view, G},
        {mNearest, mNeighbor.view, G},
        {VK_NULL_HANDLE, mOut.view, G},
        {mNearest, mOut.view, G},
        {VK_NULL_HANDLE, mBindExposure, G}};
    VkWriteDescriptorSet wr[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = mSet; wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i == 0)
        {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &bi;
        }
        else
        {
            wr[i].descriptorType = isStorage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &ii[i];
        }
    }
    vkUpdateDescriptorSets(mCtx->device(), kBindings, wr, 0, nullptr);
}

bool LLCleanMotionBlurVK::recordTiles(VkCommandBuffer cmd, const Frame& f)
{
    static LLCachedControl<F32> s_shutter(gSavedSettings, "RenderVKMotionBlurShutter", 1.f / 60.f);
    static LLCachedControl<F32> s_max(gSavedSettings, "RenderVKMotionBlurMaxRadius", 24.f);
    static LLCachedControl<U32> s_taps(gSavedSettings, "RenderVKMotionBlurSamples", 16U);
    static LLCachedControl<F32> s_rot(gSavedSettings, "RenderVKMotionBlurCameraRotation", 0.f);
    static LLCachedControl<F32> s_trans(gSavedSettings, "RenderVKMotionBlurCameraTranslation", 0.5f);
    static LLCachedControl<F32> s_obj(gSavedSettings, "RenderVKMotionBlurObject", 1.f);
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKMotionBlurDebug", 0U);
    // #836 A/B (both default on): the uniform fast path's neighbourhood check over the whole
    // reach instead of the 3x3, and Karis 1 / (1 + exposed luma) tap weights (clean_mblur_recon.comp).
    static LLCachedControl<bool> s_wide(gSavedSettings, "RenderVKMotionBlurWideUniformCheck", true);
    static LLCachedControl<bool> s_hdrw(gSavedSettings, "RenderVKMotionBlurHdrWeight", true);

    mArmed = false;
    ++mFrameNo;
    if (!f.ctx || f.rw < 2 || f.rh < 2 || !f.sceneImage || !f.sceneView || !f.depthImage || !f.depthView
        || !f.motionView || !f.reactImage || !f.reactView || !f.exposureView || !f.reprojCam || !f.reprojRot
        || !f.waterPlane || !f.lin)
        return false;
    const U32 dbg = std::min<U32>(s_dbg, 2u);
    // Shutter in frames of motion (frame-rate independent, capped at 8 frames).
    const float shutter = std::clamp((float)s_shutter / std::clamp(f.frameDt, 1.f / 1000.f, 0.25f), 0.f, 8.f);
    const float sr = std::clamp((float)s_rot, 0.f, 2.f);
    const float st = std::clamp((float)s_trans, 0.f, 2.f);
    const float so = std::clamp((float)s_obj, 0.f, 2.f);
    const float smax = std::max({sr, st, so});
    if ((shutter <= 0.f || smax <= 0.f) && dbg == 0) return false;
    if (!ensurePipelines(f)) return false;
    if (!ensureTargets(cmd, f)) return false;

    const float max_px = std::clamp((float)s_max, 1.f, 64.f) * (float)f.rh / kRefLines;
    const uint32_t reach = std::clamp((uint32_t)std::ceil(max_px / (float)kTile), 1u, kMaxReach);
    const float max_r = std::min(max_px, (float)(reach * kTile));
    if (f.rw != mLogW || f.rh != mLogH || reach != mLogReach)
    {
        LL_INFOS("VKMotionBlur") << "motion blur: " << f.rw << "x" << f.rh << ", " << mTilesX << "x" << mTilesY
                                 << " tiles, max radius " << max_r << " px (reach " << reach << " tiles)"
                                 << (f.offscreen ? " [snapshot]" : "") << LL_ENDL;
        mLogW = f.rw; mLogH = f.rh; mLogReach = reach;
    }

    Params p{};
    std::memcpy(p.reproj_cam, f.reprojCam, sizeof(p.reproj_cam));
    std::memcpy(p.reproj_rot, f.reprojRot, sizeof(p.reproj_rot));
    std::memcpy(p.wplane, f.waterPlane, sizeof(p.wplane));
    std::memcpy(p.lin, f.lin, sizeof(p.lin));
    p.scales[0] = sr; p.scales[1] = st; p.scales[2] = so; p.scales[3] = smax;
    p.p0[0] = shutter;
    p.p0[1] = max_r;
    // Taps: even (mirrored pairs). A single-frame snapshot has no resolve: twice the taps.
    uint32_t taps = std::clamp((U32)s_taps, 4u, 32u) & ~1u;
    if (f.offscreen) taps = std::min(taps * 2u, 64u);
    p.p0[2] = (float)taps;
    p.p0[3] = f.motionValid ? 1.f : 0.f;
    // Noise: rotated per frame only when a temporal resolve integrates it (Jimenez's IGN
    // frame offset, 64-frame cycle); a fixed pattern otherwise.
    if (f.temporalResolve && !f.offscreen) ++mNoiseFrame;
    p.p1[0] = (f.temporalResolve && !f.offscreen) ? (float)(1u + mNoiseFrame % 64u) : 0.f;
    // #932: a supersampled offscreen shot's recordings are averaged -- one pattern each
    // (the live clock above holds; a live shot's samples count as resolved, the caller).
    if (f.offscreen && f.snapSample) p.p1[0] = (float)(1u + (mNoiseFrame + f.snapSample) % 64u);
    p.p1[1] = (float)dbg;
    p.p1[2] = (float)f.rw; p.p1[3] = (float)f.rh;
    p.p2[0] = (float)kTile; p.p2[1] = (float)reach; p.p2[2] = (float)mTilesX; p.p2[3] = (float)mTilesY;
    p.p3[0] = f.staticExposure; p.p3[1] = f.dynExposure ? 1.f : 0.f; p.p3[2] = kActivePx; p.p3[3] = f.fovComp;
    p.p4[0] = s_wide ? 1.f : 0.f; p.p4[1] = s_hdrw ? 1.f : 0.f;
    static_assert(sizeof(Params) == 256, "LLCleanMotionBlurVK::Params must match clean_mblur_common.glsl MbParams");

    // Parameters: earlier reads (last recording's passes) -> this update -> this frame's.
    {
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        bb.srcAccessMask = 0;
        bb.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;   // (vkCmdUpdateBuffer is a transfer "clear" op)
        bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bb.buffer = mParams.buffer; bb.size = VK_WHOLE_SIZE;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &bb;
        vkCmdPipelineBarrier2(cmd, &d);
        vkCmdUpdateBuffer(cmd, mParams.buffer, 0, sizeof(p), &p);
    }
    // Pass 2's writes (scene / water mask attachments, depth from pass 1, the motion
    // target from pass 1a) and the parameter update -> this compute's reads; the own
    // images: last recording's reads -> these writes (WAR). Scene + mask -> sampled.
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT
                        | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                         | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
        VkImageMemoryBarrier2 ib[2]{};
        for (int i = 0; i < 2; ++i)
        {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ib[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ib[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib[i].image = i == 0 ? f.sceneImage : f.reactImage;
            ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        d.imageMemoryBarrierCount = 2; d.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    mSceneImage = f.sceneImage;
    mReactImage = f.reactImage;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTilePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
    vkCmdDispatch(cmd, mTilesX, mTilesY, 1);
    mArmed = true;
    return true;
}

namespace
{
// Compute writes -> the next pass's reads (compute; the write-back's fragment reads).
void computeToReads(VkCommandBuffer cmd, bool fragment)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | (fragment ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0);
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

void LLCleanMotionBlurVK::recordNeighbor(VkCommandBuffer cmd)
{
    if (!mArmed) return;
    computeToReads(cmd, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mNeighborPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
    vkCmdDispatch(cmd, (mTilesX + 7u) / 8u, (mTilesY + 7u) / 8u, 1);
}

void LLCleanMotionBlurVK::recordReconstruct(VkCommandBuffer cmd)
{
    if (!mArmed) return;
    mArmed = false;
    computeToReads(cmd, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReconPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
    vkCmdDispatch(cmd, mTilesX, mTilesY, 1);

    // Reconstruction writes -> the write-back's fragment reads; the scene: the gather's
    // sampled reads done -> colour attachment again (pass 2's layout, LOADed below); the
    // water mask back to pass 2's layout for the resolve's own prologue barrier.
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkImageMemoryBarrier2 ib[2]{};
        for (int i = 0; i < 2; ++i)
        {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            ib[i].srcAccessMask = 0;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ib[i].image = i == 0 ? mSceneImage : mReactImage;
            ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        d.imageMemoryBarrierCount = 2; d.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &d);
    }

    // Write-back: the reconstructed tiles into the scene (LOAD + STORE: the rest stays).
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = mSceneView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    bindGraphicsPipeline(cmd, mWritebackPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // The scene is COLOR_ATTACHMENT_OPTIMAL, last written by the colour-attachment stage:
    // exactly pass 2's exit state, so the resolve / SSR copy / blit barriers chain on.
}

void LLCleanMotionBlurVK::destroy(VkDevice dev)
{
    if (!dev) return;
    // A build in flight references the layout below (#782 audit): wait, free its result.
    mJob.wait();
    for (VkPipeline& p : mStage) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    vkDeviceWaitIdle(dev);
    destroyTargets();
    if (mTilePipe) vkDestroyPipeline(dev, mTilePipe, nullptr);
    if (mNeighborPipe) vkDestroyPipeline(dev, mNeighborPipe, nullptr);
    if (mReconPipe) vkDestroyPipeline(dev, mReconPipe, nullptr);
    if (mWritebackPipe) vkDestroyPipeline(dev, mWritebackPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    if (mLinear) vkDestroySampler(dev, mLinear, nullptr);
    if (mNearest) vkDestroySampler(dev, mNearest, nullptr);
    mTilePipe = mNeighborPipe = mReconPipe = mWritebackPipe = VK_NULL_HANDLE;
    mLayout = VK_NULL_HANDLE; mDsl = VK_NULL_HANDLE; mSet = VK_NULL_HANDLE;
    mLinear = mNearest = VK_NULL_HANDLE;
    mWritebackFormat = VK_FORMAT_UNDEFINED;
    mPipeFailed = false;
    mArmed = false;
    mSetOwnGen = 0;
    for (VkImageView& v : mSetViews) v = VK_NULL_HANDLE;
    mLogW = mLogH = mLogReach = 0;
}

// ===========================================================================
// Renderer side
// ===========================================================================
bool LLCleanRendererVK::motionBlurWanted() const
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKMotionBlur", false);
    return s_on;
#else
    return false;
#endif
}

bool LLCleanRendererVK::recordMotionBlur(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, bool mvNow)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    const bool wanted = motionBlurWanted();
    if (!wanted)
    {
        // Off: free the effect (not from a snapshot: the live view decides).
        if (mMotionBlur && !mOffscreen)
        {
            mMotionBlur->destroy(mCtx->device());
            delete mMotionBlur;
            mMotionBlur = nullptr;
        }
        return false;
    }
    // #870 / #962: DLSS (Ray Reconstruction and Super Resolution) must see the unblurred scene
    // (its guides: post effects after it); this blur runs before the resolve, so on DLSS frames
    // it stands down (logged once).
    if (mDlssFrame && !mOffscreen)
    {
        static bool s_noted = false;
        if (!s_noted)
        {
            s_noted = true;
            LL_INFOS("Vulkan") << "motion blur: off while DLSS runs (it must come after the upscale)" << LL_ENDL;
        }
        return false;
    }
    // Supersampled snapshot recordings (#417) freeze the world: their motion is not the
    // live view's (the first recording's camera reprojection would smear the sky once).
    if (!mTemporalOn || !mMotion.image || (mOffscreen && mSnapAccNow)) return false;
    if (!mMotionBlur) mMotionBlur = new LLCleanMotionBlurVK();
    const GpuImage& scene = sceneHdr();
    LLCleanMotionBlurVK::Frame f;
    f.ctx = mCtx;
    f.rw = rw; f.rh = rh;
    f.sceneFormat = mHdrFormat;
    f.sceneImage = scene.image; f.sceneView = scene.view;
    f.depthImage = mDepth.image; f.depthView = mDepth.view;
    f.motionView = mMotion.view;
    f.reactImage = mReact.image; f.reactView = mReact.view;
    f.exposureView = mExposureImg.view;
    f.targetGen = mTargetGen; f.depthGen = mDepthGen;
    f.reprojCam = mMbReprojCam;   // mTaaReproj less the speed-FoV change (#663)
    f.fovComp = mMbFovK;
    f.reprojRot = mMbReprojRot;
    f.waterPlane = mTaaWaterPlane;
    // View depth from NDC depth: rows 2 / 3 of the (unjittered) inverse projection.
    const float lin[4] = {mCurInvProj[10], mCurInvProj[14], mCurInvProj[11], mCurInvProj[15]};
    f.lin = lin;
    // Frame time of the motion the target holds: the exposure adaptation's dt (mAtmos
    // row 15: speed_target, dt, ...).
    f.frameDt = mAtmos[15 * 4 + 1];
    f.staticExposure = mAtmos[13 * 4 + 0];
    f.dynExposure = mAtmos[15 * 4 + 2] > 0.5f && mExposurePipe;
    f.motionValid = mvNow;
    // (#932: a live shot's accumulation samples are averaged: they integrate the rotation too)
    f.temporalResolve = (mTaaNow || mLiveCapNow) && !mOffscreen;
    f.offscreen = mOffscreen;
    f.snapSample = snapSample();
    if (!mMotionBlur->recordTiles(cmd, f)) return false;
    gpuSubMark(cmd, "p_mblur_tile");
    mMotionBlur->recordNeighbor(cmd);
    gpuSubMark(cmd, "p_mblur_nmax");
    mMotionBlur->recordReconstruct(cmd);
    gpuSubMark(cmd, "p_mblur");
    return true;
#else
    (void)cmd; (void)rw; (void)rh; (void)mvNow;
    return false;
#endif
}
