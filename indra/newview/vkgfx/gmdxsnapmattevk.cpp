/**
 * @file gmdxsnapmattevk.cpp
 * @brief GunMetal-DX avatar matte snapshots for the Vulkan clean renderer.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// <GMDX> AVATAR MATTE SNAPSHOTS: the renderer half of gmdxsnapshotavatars.h (SoapStorm's GL
// transparent-avatar / holdout-matte capture, re-done on the Vulkan path). Defines the
// LLCleanRendererVK members of its "AVATAR MATTE SNAPSHOTS" block; renderOffscreen calls them
// around each recording of an avatar layer's offscreen sequence:
//  - recording 0 (the warm-up every offscreen sequence has) renders the normal view; its avatar-id
//    pass (llcelshadevk.h, requested for the whole sequence) is read back and the avatars it shows
//    become the only other avatars the layer may contain (gmdxSnapshotMatteSetSeen) -- nobody
//    behind a wall. Its depth is kept too, as the WORLD OCCLUDER of each pixel (mSnapOcc): the
//    world's depth where the id pass shows no avatar, "nothing in front" where it shows one;
//  - recordings 1..N have mSnapMatte: the main view keeps avatars only, the world still shadows
//    and lights them. Each one's coverage -- avatar id != 0 (rigged, system body, worn rigid
//    attachments, alpha-blended hair at >= 50 % texel alpha), or the depth when the id pass is
//    unavailable -- is copied to the host with its depth and counted per pixel where the avatar
//    is not behind the world occluder: a seen avatar's parts behind a wall stay out of the matte,
//    so out of the transparent layer too (its colour is dropped where the matte is 0). N > 1 when
//    the snapshot is supersampled (jittered recordings), so the matte is antialiased like the
//    colour; the occluder is per pixel (recording 0 is not jittered), so the edge of a wall that
//    cuts an avatar is pixel-sharp;
//  - the matte is coverage / N in 8 bits, independent of the tonemap, bloom or exposure (the
//    colour chain never carries it).

#include "linden_common.h"

#include "llcelshadevk.h"
#include "llcleanrenderervk.h"

#include "gmdxsnapshotavatars.h"
#include "llcleanskinstore.h"
#include "lluuid.h"

#include <new>

namespace
{
void snapBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

void LLCleanRendererVK::snapMatteBegin(bool on, uint32_t w, uint32_t h)
{
    mSnapMatte = false;
    mSnapMatteRec = 0;
    mSnapMatteSamples = 0;
    mSnapMatteFrameNo = 0;
    mSnapAlpha.clear();
    std::vector<uint16_t>().swap(mSnapCov);
    std::vector<float>().swap(mSnapOcc);
    mSnapMatteSeq = on && w > 0 && h > 0;
    if (!mSnapMatteSeq) return;
    try
    {
        mSnapCov.assign((size_t)w * h, 0);
    }
    catch (const std::bad_alloc&)
    {
        LL_WARNS("Snapshot") << "avatar matte " << w << "x" << h << ": no memory for the coverage counts" << LL_ENDL;
        mSnapMatteSeq = false;
        return;
    }
    mSnapMatteW = w;
    mSnapMatteH = h;
    // The avatar-id pass in every recording of the sequence (recordCelFrameStart reads it).
    mSnapMaskWas = mCelFxWantsMask;
    requestAvatarMask(true);
}

void LLCleanRendererVK::snapMatteFrame(uint32_t frame)
{
    mSnapMatteFrameNo = frame;
    mSnapMatte = mSnapMatteSeq && frame > 0;   // recording 0: the normal view (who is in sight)
    mSnapMatteRec = 0;
}

void LLCleanRendererVK::snapMatteRecord(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    mSnapMatteRec = 0;
    if (!mSnapMatteSeq || w != mSnapMatteW || h != mSnapMatteH) return;
    // The avatar ids of this recording (render extent = the shot's offscreen; jittered like the
    // colour when supersampled), left by the id pass in SHADER_READ_ONLY_OPTIMAL.
    const bool ids = mCel && mCel->mMaskValid && mCel->mId.image && mCel->mIdW == w && mCel->mIdH == h;
    if (ids)
    {
        const VkDeviceSize need = (VkDeviceSize)w * h * sizeof(uint16_t);
        if (!mSnapIdBuf.buffer || mSnapIdBuf.size < need)
        {
            VkMemScope memcat(VKMEM_READBACK);   // #407
            if (mSnapIdBuf.buffer)
            {
                vkDeviceWaitIdle(mCtx->device());
                mCtx->destroyBuffer(mSnapIdBuf);
                mSnapIdBuf = GpuBuffer{};
            }
            VkDeviceContext::SoftAllocScope soft(mCtx);   // (out of memory: the depth below)
            mSnapIdBuf = mCtx->createBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (mSnapIdBuf.buffer && mSnapIdBuf.mapped)
        {
            snapBarrier(cmd, mCel->mId.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, mCel->mId.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mSnapIdBuf.buffer, 1, &r);
            snapBarrier(cmd, mCel->mId.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            hd.memoryBarrierCount = 1; hd.pMemoryBarriers = &hb;
            vkCmdPipelineBarrier2(cmd, &hd);
            mSnapMatteRec = 1;
        }
    }
    // The depth: the world occluder (recording 0), the avatars' own depth for the occluder test,
    // and the coverage itself when there are no ids (only avatars are in the main view then).
    if (mDepthW == w && mDepthH == h)
    {
        recordDepthCapture(cmd, w, h);
        if (mDepthCapValid) mSnapMatteRec |= 2;
    }
}

void LLCleanRendererVK::snapMatteCollect()
{
    if (!mSnapMatteSeq) return;
    const uint32_t rec = mSnapMatteRec;
    mSnapMatteRec = 0;
    const size_t n = (size_t)mSnapMatteW * mSnapMatteH;
    const uint16_t* ids = (rec & 1) ? static_cast<const uint16_t*>(mSnapIdBuf.mapped) : nullptr;
    std::vector<float> depth;
    if (rec & 2)
    {
        uint32_t dw = 0, dh = 0;
        try
        {
            if (!readDepthCapture(depth, dw, dh) || dw != mSnapMatteW || dh != mSnapMatteH || depth.size() < n) depth.clear();
        }
        catch (const std::bad_alloc&)
        {
            depth.clear();
        }
    }
    const bool reverse = worldReverseZ();   // the clear value is the far plane: 0 reversed, else 1
    const float far_z = reverse ? 0.f : 1.f;
    if (mSnapMatteFrameNo == 0)
    {
        // The world occluder per pixel, from the view with the world: needs the ids (to tell an
        // avatar's own depth from the world's) and the depth; without them no occluder test.
        if (ids && !depth.empty())
        {
            try
            {
                mSnapOcc.assign(depth.begin(), depth.begin() + n);
                for (size_t p = 0; p < n; ++p)
                    if (ids[p] != 0u) mSnapOcc[p] = far_z;
            }
            catch (const std::bad_alloc&)
            {
                std::vector<float>().swap(mSnapOcc);
                LL_WARNS("Snapshot") << "avatar matte: no memory for the world occluder, walls will not hide avatar parts" << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS("Snapshot") << "avatar matte: the warm-up read back no ids / depth, walls will not hide avatar parts" << LL_ENDL;
        }
        // The warm-up: the avatars the view with the world shows (none without ids: then the
        // layer holds only your own avatar).
        std::vector<LLUUID> seen;
        if (ids)
        {
            uint32_t bits[kAvatarSlotWords] = {};
            for (size_t p = 0; p < n; ++p)
            {
                const uint32_t v = ids[p];
                if (v != 0u && v - 1u < kAvatarSlotCap) bits[(v - 1u) >> 5] |= 1u << ((v - 1u) & 31u);
            }
            std::vector<std::pair<LLUUID, U32>> slots;
            LLCleanSkinStore::instance().copyAvatarSlots(slots);
            for (const auto& sl : slots)
                if (sl.second < kAvatarSlotCap && ((bits[sl.second >> 5] >> (sl.second & 31u)) & 1u)) seen.push_back(sl.first);
        }
        gmdxSnapshotMatteSetSeen(seen);
        return;
    }
    if (mSnapCov.size() != n) return;
    const bool occ = mSnapOcc.size() == n;
    if (depth.empty() && (!ids || occ)) return;   // (this recording read too little back: it does not count)
    // Not behind the world occluder. The depth is ~1/distance (reversed) or 1 - depth is
    // (conventional), so the tolerance is relative to the distance: 0.5 %, for the jitter between
    // this recording's samples and the occluder's pixel centres where an avatar meets the world.
    // Alpha-blended parts (hair) write no depth: over nothing opaque they read the far plane, so
    // they count only where recording 0 showed an avatar at the pixel (occluder = far) -- never
    // through a wall, at the price of a pixel-sharp hair fringe against the world.
    constexpr float kOccTol = 0.005f;
    for (size_t p = 0; p < n; ++p)
    {
        const bool covered = ids ? ids[p] != 0u : (reverse ? depth[p] > 0.f : depth[p] < 1.f);
        if (!covered) continue;
        if (occ && (reverse ? depth[p] < mSnapOcc[p] * (1.f - kOccTol)
                            : 1.f - depth[p] < (1.f - mSnapOcc[p]) * (1.f - kOccTol)))
            continue;   // behind the world: hidden in the normal view
        ++mSnapCov[p];
    }
    ++mSnapMatteSamples;
}

void LLCleanRendererVK::snapMatteEnd()
{
    mSnapMatte = false;
    if (!mSnapMatteSeq) return;
    mSnapMatteSeq = false;
    const uint32_t w = mSnapMatteW, h = mSnapMatteH;
    const size_t n = (size_t)w * h;
    if (mSnapMatteSamples > 0 && mSnapCov.size() == n)
    {
        try
        {
            mSnapAlpha.resize(n);
            const uint32_t s = mSnapMatteSamples;
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint16_t* row = &mSnapCov[(size_t)y * w];
                uint8_t* dst = &mSnapAlpha[(size_t)(h - 1 - y) * w];   // LLImageRaw rows are bottom-up
                for (uint32_t x = 0; x < w; ++x) dst[x] = (uint8_t)((row[x] * 255u + s / 2u) / s);
            }
        }
        catch (const std::bad_alloc&)
        {
            mSnapAlpha.clear();
        }
    }
    else
    {
        LL_WARNS("Snapshot") << "avatar matte " << w << "x" << h << ": no recording read its coverage back" << LL_ENDL;
    }
    std::vector<uint16_t>().swap(mSnapCov);
    std::vector<float>().swap(mSnapOcc);
    if (mSnapIdBuf.buffer)
    {
        vkDeviceWaitIdle(mCtx->device());
        mCtx->destroyBuffer(mSnapIdBuf);
        mSnapIdBuf = GpuBuffer{};
    }
    requestAvatarMask(mSnapMaskWas);
}

bool LLCleanRendererVK::snapMatteTake(std::vector<uint8_t>& alpha, uint32_t& w, uint32_t& h)
{
    if (mSnapAlpha.empty()) return false;
    alpha.swap(mSnapAlpha);
    std::vector<uint8_t>().swap(mSnapAlpha);
    w = mSnapMatteW;
    h = mSnapMatteH;
    return true;
}
