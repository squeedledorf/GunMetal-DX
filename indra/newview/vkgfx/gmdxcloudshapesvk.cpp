/**
 * @file gmdxcloudshapesvk.cpp
 * @brief GunMetal-DX: the volumetric clouds' sculpted cloud bodies (see gmdxcloudshapesvk.h).
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

#include "linden_common.h"

#include "gmdxcloudshapesvk.h"

#include "lldir.h"
#include "llerror.h"
#include "llfile.h"

#include <cstring>
#include <vector>

namespace
{
constexpr uint32_t kShape = 64;            // texels per shape edge (the file's size field)
constexpr uint32_t kBlock = kShape * 2;    // the 2 x 2 x 2 block
constexpr uint32_t kMaxShapes = 8;

// The packed block, or empty: file missing, wrong magic / size, short.
std::vector<uint8_t> loadBlock(uint32_t& count)
{
    count = 0;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "gmdx_cloud_shapes.bin");
    llifstream in(path.c_str(), std::ios::binary);
    char magic[8] = {};
    uint32_t hdr[2] = {};
    if (!in.read(magic, 8) || std::memcmp(magic, "GMDXCS01", 8) != 0 || !in.read((char*)hdr, sizeof(hdr))
        || hdr[1] != kShape || hdr[0] == 0)
    {
        LL_WARNS("VKVolClouds") << "cloud shapes: " << path << " missing or not a shape file; procedural clouds only" << LL_ENDL;
        return {};
    }
    const uint32_t n = std::min(hdr[0], kMaxShapes);
    std::vector<uint8_t> shape((size_t)kShape * kShape * kShape);
    std::vector<uint8_t> block((size_t)kBlock * kBlock * kBlock, 0);
    for (uint32_t k = 0; k < n; ++k)
    {
        if (!in.read((char*)shape.data(), (std::streamsize)shape.size()))
        {
            LL_WARNS("VKVolClouds") << "cloud shapes: " << path << " is short; procedural clouds only" << LL_ENDL;
            return {};
        }
        const uint32_t ox = (k & 1u) * kShape, oy = ((k >> 1) & 1u) * kShape, oz = (k >> 2) * kShape;
        for (uint32_t z = 0; z < kShape; ++z)
            for (uint32_t y = 0; y < kShape; ++y)
                std::memcpy(&block[((size_t)(oz + z) * kBlock + oy + y) * kBlock + ox],
                            &shape[((size_t)z * kShape + y) * kShape], kShape);
    }
    count = n;
    return block;
}
} // namespace

bool GMDXCloudShapesVK::create(VkDeviceContext* ctx)
{
    destroy(ctx);
    uint32_t n = 0;
    std::vector<uint8_t> block = loadBlock(n);
    if (block.empty()) block.assign(1, 0);
    // The bodies are optional: a refused allocation (out of video memory) falls back to the
    // 1-texel stand-in, procedural clouds only. false only when even that is refused.
    if (!upload(ctx, block, block.size() == 1 ? 1u : kBlock))
    {
        LL_WARNS("VKVolClouds") << "cloud shapes: allocation refused; procedural clouds only" << LL_ENDL;
        n = 0;
        block.assign(1, 0);
        if (!upload(ctx, block, 1u)) return false;
    }
    mCount = n;
    if (n) LL_INFOS("VKVolClouds") << "cloud shapes: " << n << " sculpted cloud bodies loaded" << LL_ENDL;
    return true;
}

bool GMDXCloudShapesVK::upload(VkDeviceContext* ctx, const std::vector<uint8_t>& block, uint32_t side)
{
    GpuBuffer staging;
    {
        VkDeviceContext::SoftAllocScope soft(ctx);
        mImage = ctx->createImage3D(side, side, side, VK_FORMAT_R8_UNORM,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (mImage.image)
            staging = ctx->createBuffer(block.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mImage.image || !staging.mapped)
    {
        if (staging.buffer) ctx->destroyBuffer(staging);
        destroy(ctx);
        return false;
    }
    std::memcpy(staging.mapped, block.data(), block.size());
    ctx->submitImmediate([&](VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = mImage.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {side, side, side};
        vkCmdCopyBufferToImage(cmd, staging.buffer, mImage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &d);
    });
    ctx->destroyBuffer(staging);
    ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mImage.image, "gmdxCloudShapes");
    return true;
}

void GMDXCloudShapesVK::destroy(VkDeviceContext* ctx)
{
    if (ctx && mImage.image) ctx->destroyImage(mImage);
    mImage = GpuImage{};
    mCount = 0;
}

void GMDXCloudShapesVK::write(VkDevice dev, VkDescriptorSet set, VkSampler clampLinear) const
{
    if (!set || !mImage.view) return;
    const VkDescriptorImageInfo ii{clampLinear, mImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = kBinding; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}
