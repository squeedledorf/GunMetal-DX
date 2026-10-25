#include "vk_offscreen.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <vector>

void OffscreenTarget::init(VkDeviceContext& ctx, uint32_t w, uint32_t h,
                           bool withDepth) {
    // RGBA8 UNORM so readback is a trivial byte copy into a PNG.
    mColor = ctx.createImage2D(
        w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    if (withDepth) {
        mDepth = ctx.createImage2D(
            w, h, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

void OffscreenTarget::destroy(VkDeviceContext& ctx) {
    ctx.destroyImage(mColor);
    if (mDepth.image) ctx.destroyImage(mDepth);
}

bool OffscreenTarget::readbackToPng(VkDeviceContext& ctx,
                                    const std::string& path) {
    const uint32_t w = width(), h = height();
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

    GpuBuffer host = ctx.createBuffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        // Transition color image from COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC.
        VkImageMemoryBarrier2 toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.image = mColor.image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toSrc;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, mColor.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               host.buffer, 1, &region);

        // Barrier so the host read sees the copy.
        VkBufferMemoryBarrier2 hostBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hostBarrier.buffer = host.buffer;
        hostBarrier.size = bytes;
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.bufferMemoryBarrierCount = 1;
        dep2.pBufferMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });

    int ok = stbi_write_png(path.c_str(), (int)w, (int)h, 4, host.mapped,
                            (int)(w * 4));
    ctx.destroyBuffer(host);
    if (!ok) {
        std::fprintf(stderr, "[vktest] stbi_write_png failed: %s\n",
                     path.c_str());
        return false;
    }
    VK_LOG("wrote %s (%ux%u RGBA8)", path.c_str(), w, h);
    return true;
}
