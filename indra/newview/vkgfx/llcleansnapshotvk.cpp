/**
 * @file llcleansnapshotvk.cpp
 * @brief LIVE SNAPSHOTS (#873) -- see llcleansnapshotvk.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llcleansnapshotvk.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
// clean_snap_live.comp Push (96 B).
struct SnapPush
{
    uint32_t size[4];   // source w, h; output w, h
    uint32_t aux[4];    // flags, motion w, h, -
    uint32_t rows[4];   // first work row, work rows
    float    crop[4];   // source px x0, y0, w, h
    float    p0[4];
    float    p1[4];
};
VK_PUSH_BUDGET_CHECK(SnapPush);

constexpr uint32_t kFlagBgra = 1u, kFlagMotion = 2u, kFlagFirst = 4u, kFlagDither = 8u;
constexpr float    kAccSigma = 0.5f;      // reconstruction Gaussian, output px
constexpr float    kMotionPx = 0.25f;     // moved more than this (display px) this frame: not accumulated
constexpr uint64_t kFenceTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;

enum { PIPE_H = 0, PIPE_V, PIPE_DEPTH, PIPE_ACC, PIPE_RESOLVE, PIPE_COUNT };

VkShaderModule snapModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

VkDeviceSize packedBytes(uint32_t w, uint32_t h)
{
    const uint64_t px = (uint64_t)w * h;
    return (VkDeviceSize)(((px + 3u) / 4u) * 12u);   // 4 px per 3 words (clean_snap_live.comp packOut)
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = srcStage; m.srcAccessMask = srcAccess;
    m.dstStageMask = dstStage; m.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

void imageToBuffer(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout layout,
                   VkPipelineStageFlags2 useStages, VkAccessFlags2 useWrite, VkAccessFlags2 useAccess,
                   VkBuffer dst, uint32_t w, uint32_t h)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = useStages; b.srcAccessMask = useWrite;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = img; b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo d0{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d0.imageMemoryBarrierCount = 1; d0.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d0);
    VkBufferImageCopy r{};
    r.imageSubresource = {aspect, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
    VkImageMemoryBarrier2 a = b;
    a.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; a.srcAccessMask = 0;
    a.dstStageMask = useStages; a.dstAccessMask = useAccess;
    a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; a.newLayout = layout;
    VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d1.imageMemoryBarrierCount = 1; d1.pImageMemoryBarriers = &a;
    vkCmdPipelineBarrier2(cmd, &d1);
}

double nowSec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool LLCleanSnapshotVK::sourceFormatSupported(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

bool LLCleanSnapshotVK::ensurePipelines(VkDevice dev)
{
    if (mPipe[0]) return true;
    if (mPipeFailed) return false;
    mPipeFailed = true;   // until everything below exists
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4; dl.pBindings = b;
    if (vkdCreateSetLayout(dev, &dl, &mDsl, "mDslSnapLive") != VK_SUCCESS) return false;
    if (mPool.create(dev, "snapLive", {{mDsl, 1}}) != VK_SUCCESS) return false;
    if (mPool.allocate(mDsl, &mSet) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SnapPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &mDsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mLayout, "mSnapLiveLayout") != VK_SUCCESS) return false;
    struct Src { const uint32_t* code; size_t bytes; const char* name; };
    const Src srcs[PIPE_COUNT] = {
        {kWorldSnapLiveHSpv,       kWorldSnapLiveHSpv_bytes,       "SnapLiveH"},
        {kWorldSnapLiveVSpv,       kWorldSnapLiveVSpv_bytes,       "SnapLiveV"},
        {kWorldSnapLiveDepthSpv,   kWorldSnapLiveDepthSpv_bytes,   "SnapLiveDepth"},
        {kWorldSnapLiveAccSpv,     kWorldSnapLiveAccSpv_bytes,     "SnapLiveAcc"},
        {kWorldSnapLiveResolveSpv, kWorldSnapLiveResolveSpv_bytes, "SnapLiveResolve"}};
    for (uint32_t i = 0; i < PIPE_COUNT; ++i)
    {
        VkShaderModule m = snapModule(dev, srcs[i].code, srcs[i].bytes, srcs[i].name);
        if (!m) return false;
        mPipe[i] = createComputePipeline(dev, m, mLayout);
        destroyShaderModuleDeferred(dev, m, nullptr);
        if (!mPipe[i]) return false;
    }
    mPipeFailed = false;
    return true;
}

void LLCleanSnapshotVK::fail(const std::string& why)
{
    if (mState != ST_FAILED)
        LL_WARNS("Snapshot") << "live snapshot failed: " << why << LL_ENDL;
    mWhy = why;
    mState = ST_FAILED;
}

void LLCleanSnapshotVK::freeBuffers()
{
    if (!mCtx) return;
    for (GpuBuffer* b : {&mSrc, &mWork, &mOut, &mMotion})
        if (b->buffer) mCtx->destroyBuffer(*b);
    mSrc = mWork = mOut = mMotion = GpuBuffer{};
    mSetDirty = true;
}

uint64_t LLCleanSnapshotVK::deviceBytes() const
{
    return (uint64_t)mSrc.size + mWork.size + mMotion.size;
}

bool LLCleanSnapshotVK::arm(VkDeviceContext* ctx, const Request& r, uint32_t srcW, uint32_t srcH, std::string& why)
{
    if (!ctx) { why = "no device"; return false; }
    if (mState == ST_ARMED || mState == ST_RECORDED || mState == ST_SUBMITTED)
    {
        LL_WARNS("Snapshot") << "live snapshot: a previous shot was not read; abandoned" << LL_ENDL;
        cancel();
    }
    mCtx = ctx;
    mState = ST_IDLE;
    mWhy.clear();
    if (!r.outW || !r.outH || r.crop[2] <= 0.f || r.crop[3] <= 0.f) { why = "empty shot"; return false; }
    if (r.mode != MODE_DEPTH && (!srcW || !srcH)) { why = "no live frame"; return false; }
    VkDevice dev = ctx->device();
    if (!ensurePipelines(dev)) { why = "the snapshot compute pipelines could not be created"; return false; }
    freeBuffers();
    mReq = r;
    mReq.frames = std::clamp(r.frames, 1u, 64u);
    mSrcW = srcW; mSrcH = srcH;
    mFrame = 0;
    mMotionNow = false;
    mFence = VK_NULL_HANDLE;
    mPendingInFlight = false;

    // MODE_RESAMPLE: the source rows the vertical kernel reaches (the crop's rows plus the
    // Lanczos support), so the work buffer is shot width x those rows, never the window.
    mWorkRow0 = mWorkRows = 0;
    if (r.mode == MODE_RESAMPLE)
    {
        const double y0 = (double)r.crop[1] * srcH, ch = (double)r.crop[3] * srcH;
        const double fs = std::max(ch / (double)r.outH, 1.0);
        const int64_t a = std::max<int64_t>(0, (int64_t)std::floor(y0 - 3.0 * fs - 1.0));
        const int64_t b = std::min<int64_t>((int64_t)srcH - 1, (int64_t)std::ceil(y0 + ch + 3.0 * fs + 1.0));
        mWorkRow0 = (uint32_t)a;
        mWorkRows = (uint32_t)std::max<int64_t>(1, b - a + 1);
    }

    {
        // Soft (#697 / #730): out of memory is a "no", never a crash; the caller falls back.
        VkDeviceContext::SoftAllocScope soft(ctx, true);
        {
            VkMemScope memcat(VKMEM_READBACK);
            const VkDeviceSize ob = packedBytes(r.outW, r.outH);
            mOut = ctx->createBuffer(ob, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (!mOut.buffer)   // (no cached coherent type: plain coherent)
                mOut = ctx->createBuffer(ob, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        VkMemScope memcat(VKMEM_TARGET);
        if (mOut.buffer && mOut.mapped)
        {
            VkDeviceSize wb = 0;
            if (r.mode == MODE_RESAMPLE) wb = (VkDeviceSize)r.outW * mWorkRows * 8u;
            else if (r.mode == MODE_ACCUM) wb = (VkDeviceSize)r.outW * r.outH * 8u;
            if (wb) mWork = ctx->createBuffer(wb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (r.mode != MODE_DEPTH && (!wb || mWork.buffer))
                ensureSource((VkDeviceSize)srcW * srcH * 4u);
        }
    }
    const bool ok = mOut.buffer && mOut.mapped
                 && (r.mode == MODE_DEPTH || (mSrc.buffer && (r.mode == MODE_RESAMPLE || r.mode == MODE_ACCUM) && mWork.buffer));
    if (!ok)
    {
        why = "not enough memory for the shot's buffers";
        LL_WARNS("Snapshot") << "live snapshot " << r.outW << "x" << r.outH << ": " << why << " (output "
                             << (packedBytes(r.outW, r.outH) >> 20) << " MB host, work "
                             << ((r.mode == MODE_ACCUM ? (uint64_t)r.outW * r.outH * 8u
                                                       : (uint64_t)r.outW * mWorkRows * 8u) >> 20)
                             << " MB device)" << LL_ENDL;
        freeBuffers();
        return false;
    }
    mSetDirty = true;
    mState = ST_ARMED;
    mArmTime = nowSec();
    static const char* const kModes[3] = {"resample", "depth", "accumulate"};
    LL_INFOS("Snapshot") << "live snapshot armed: " << r.outW << "x" << r.outH << " " << kModes[std::min<uint32_t>(r.mode, 2u)]
                         << " at capture point " << r.point << " from " << srcW << "x" << srcH << " crop "
                         << r.crop[0] << "," << r.crop[1] << " " << r.crop[2] << "x" << r.crop[3]
                         << (r.mode == MODE_ACCUM ? " frames " + std::to_string(mReq.frames) : std::string())
                         << (r.mode == MODE_ACCUM && r.motionReject ? " (motion rejection)" : "")
                         << "; VRAM: source " << (mSrc.size >> 10) << " KB, "
                         << (r.mode == MODE_ACCUM ? "accumulator " : "work ") << (mWork.size >> 10)
                         << " KB; host output " << (mOut.size >> 10) << " KB" << LL_ENDL;
    return true;
}

bool LLCleanSnapshotVK::ensureSource(VkDeviceSize bytes)
{
    if (mSrc.buffer && mSrc.size >= bytes) return true;
    if (mSrc.buffer) return false;   // (sized at arm / the first record; a shot never re-sizes it)
    VkMemScope memcat(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mCtx, true);
    mSrc = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mSetDirty = true;
    return mSrc.buffer != VK_NULL_HANDLE;
}

bool LLCleanSnapshotVK::ensureMotion(VkDeviceSize bytes)
{
    if (mMotion.buffer) return mMotion.size >= bytes;
    VkMemScope memcat(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mCtx, true);
    mMotion = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mSetDirty = true;
    return mMotion.buffer != VK_NULL_HANDLE;
}

// Bindings the mode does not read name the source (a valid buffer; never accessed).
void LLCleanSnapshotVK::writeSet()
{
    if (!mSetDirty || !mSet) return;
    const VkBuffer src = mSrc.buffer;
    const VkDescriptorBufferInfo bi[4] = {
        {src, 0, VK_WHOLE_SIZE},
        {mWork.buffer ? mWork.buffer : src, 0, VK_WHOLE_SIZE},
        {mOut.buffer, 0, VK_WHOLE_SIZE},
        {mMotion.buffer ? mMotion.buffer : src, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = mSet; wr[i].dstBinding = i;
        wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 4, wr, 0, nullptr);
    mSetDirty = false;
}

void LLCleanSnapshotVK::record(VkCommandBuffer cmd, const Source& s)
{
    if (mState != ST_ARMED || s.point != mReq.point || !cmd) return;
    const Request& r = mReq;
    SnapPush pc{};
    pc.size[2] = r.outW; pc.size[3] = r.outH;
    uint32_t srcW = 0, srcH = 0;
    if (r.mode == MODE_DEPTH)
    {
        if (!s.depthImage || s.depthFormat != VK_FORMAT_D32_SFLOAT || !s.depthW || !s.depthH)
        {
            fail("no D32 world depth at the capture point");
            return;
        }
        srcW = s.depthW; srcH = s.depthH;
        // The set may name a buffer an earlier, unread shot freed: the first record writes it.
        if (!ensureSource((VkDeviceSize)srcW * srcH * 4u)) { fail("not enough memory for the depth copy"); return; }
    }
    else
    {
        if (!s.swapImage || !sourceFormatSupported(s.swapFormat)) { fail("unsupported swapchain format"); return; }
        if (s.width != mSrcW || s.height != mSrcH)
        {
            fail("the window changed size during the shot");
            return;
        }
        srcW = s.width; srcH = s.height;
    }
    const bool first = (mFrame == 0);
    if (r.mode == MODE_ACCUM && first)
    {
        mMotionNow = r.motionReject && s.motionImage && s.motionW && s.motionH
                  && ensureMotion((VkDeviceSize)s.motionW * s.motionH * 4u);
        if (r.motionReject && !mMotionNow)
            LL_INFOS("Snapshot") << "live snapshot: no motion vectors this frame (temporal AA off?); "
                                    "moving content is accumulated as is" << LL_ENDL;
    }
    writeSet();

    vkBreadcrumb(cmd, "snap_live");
    // WAR on the shot's buffers (the previous frame's dispatches of this shot) -> copies.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (r.mode == MODE_DEPTH)
        imageToBuffer(cmd, s.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                      mSrc.buffer, srcW, srcH);
    else
        imageToBuffer(cmd, s.swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                      mSrc.buffer, srcW, srcH);
    if (r.mode == MODE_ACCUM && mMotionNow)
        imageToBuffer(cmd, s.motionImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mMotion.buffer, s.motionW, s.motionH);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    pc.size[0] = srcW; pc.size[1] = srcH;
    pc.crop[0] = r.crop[0] * (float)srcW; pc.crop[1] = r.crop[1] * (float)srcH;
    pc.crop[2] = r.crop[2] * (float)srcW; pc.crop[3] = r.crop[3] * (float)srcH;
    const bool bgra = s.swapFormat == VK_FORMAT_B8G8R8A8_UNORM || s.swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    pc.aux[0] = (r.mode != MODE_DEPTH && bgra) ? kFlagBgra : 0u;
    if (r.mode == MODE_ACCUM && r.dither) pc.aux[0] |= kFlagDither;   // #932: the resolve dithers the mean once
    const uint32_t packGroups = (uint32_t)((((uint64_t)r.outW * r.outH + 3u) / 4u + 63u) / 64u);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
    auto push = [&]() { vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc); };
    auto computeToCompute = [&]() {
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    };
    bool last = true;
    switch (r.mode)
    {
    case MODE_RESAMPLE:
        pc.rows[0] = mWorkRow0; pc.rows[1] = mWorkRows;
        push();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[PIPE_H]);
        vkCmdDispatch(cmd, (r.outW + 7u) / 8u, (mWorkRows + 7u) / 8u, 1);
        computeToCompute();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[PIPE_V]);
        vkCmdDispatch(cmd, packGroups, 1, 1);
        break;
    case MODE_DEPTH:
        pc.p0[0] = r.depthRange[0]; pc.p0[1] = r.depthRange[1];
        std::memcpy(pc.p1, s.invProj, sizeof(pc.p1));
        push();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[PIPE_DEPTH]);
        vkCmdDispatch(cmd, packGroups, 1, 1);
        break;
    case MODE_ACCUM:
    {
        last = (mFrame + 1u >= r.frames);
        if (first) pc.aux[0] |= kFlagFirst;
        if (mMotionNow) { pc.aux[0] |= kFlagMotion; pc.aux[1] = s.motionW; pc.aux[2] = s.motionH; }
        pc.p0[0] = s.jitterPx[0]; pc.p0[1] = s.jitterPx[1];
        pc.p0[2] = kAccSigma; pc.p0[3] = kMotionPx;
        push();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[PIPE_ACC]);
        vkCmdDispatch(cmd, (r.outW + 7u) / 8u, (r.outH + 7u) / 8u, 1);
        if (last)
        {
            computeToCompute();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe[PIPE_RESOLVE]);
            vkCmdDispatch(cmd, packGroups, 1, 1);
        }
        break;
    }
    }
    // The packed output -> the host (read() waits the frame's fence first).
    if (last)
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    ++mFrame;
    mPendingInFlight = true;
    if (last) mState = ST_RECORDED;
}

void LLCleanSnapshotVK::frameSubmitted(VkFence fence)
{
    if (mState == ST_RECORDED)
    {
        mFence = fence;
        mState = fence ? ST_SUBMITTED : ST_FAILED;
        if (!fence) mWhy = "no frame fence";
    }
}

void LLCleanSnapshotVK::frameDropped()
{
    // The dropped frame's recording never ran: an accumulation lost a sample (or its first
    // frame's initialisation), a single-frame shot its only copy.
    if (mState == ST_ARMED && mFrame > 0) fail("a captured frame was dropped (out of memory)");
    else if (mState == ST_RECORDED) fail("the captured frame was dropped (out of memory)");
}

bool LLCleanSnapshotVK::read(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h)
{
    if (mState != ST_SUBMITTED || !mCtx)
    {
        if (mState == ST_FAILED || mState == ST_ARMED || mState == ST_RECORDED) cancel();
        return false;
    }
    VkDevice dev = mCtx->device();
    const VkResult wr = vkWaitForFences(dev, 1, &mFence, VK_TRUE, kFenceTimeoutNs);
    if (wr != VK_SUCCESS)
    {
        LL_WARNS("Snapshot") << "live snapshot: the frame did not complete (" << (int)wr << ")" << LL_ENDL;
        cancel();
        return false;
    }
    w = mReq.outW; h = mReq.outH;
    const size_t bytes = (size_t)w * h * 3u;
    rgb.resize(bytes);
    std::memcpy(rgb.data(), mOut.mapped, bytes);
    LL_INFOS("Snapshot") << "live snapshot " << w << "x" << h << " read back after " << mFrame << " frame(s), "
                         << (int)((nowSec() - mArmTime) * 1000.0) << " ms; peak VRAM " << (deviceBytes() >> 20)
                         << " MB device + " << (hostBytes() >> 20) << " MB host" << LL_ENDL;
    // The fence covers every recording of this shot (all submitted before it on this queue).
    freeBuffers();
    mPendingInFlight = false;
    mState = ST_IDLE;
    return true;
}

void LLCleanSnapshotVK::cancel()
{
    if (!mCtx) { mState = ST_IDLE; return; }
    if (mPendingInFlight) vkDeviceWaitIdle(mCtx->device());   // (error path only)
    freeBuffers();
    mPendingInFlight = false;
    mState = ST_IDLE;
}

void LLCleanSnapshotVK::destroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    vkDeviceWaitIdle(dev);
    freeBuffers();
    for (VkPipeline& p : mPipe) if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mLayout) { vkDestroyPipelineLayout(dev, mLayout, nullptr); mLayout = VK_NULL_HANDLE; }
    mPool.destroy();
    mSet = VK_NULL_HANDLE;
    if (mDsl) { vkdDestroySetLayout(dev, mDsl); mDsl = VK_NULL_HANDLE; }
    mPipeFailed = false;
    mState = ST_IDLE;
    mCtx = nullptr;
}
