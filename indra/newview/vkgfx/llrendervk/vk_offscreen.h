// vktest — offscreen render target + host readback + PNG writing.
//
// An OffscreenTarget owns a color image (and optionally a depth image) that a
// scene renders into via dynamic rendering. After rendering, readbackToPng()
// copies the color image to a host-visible buffer and writes an RGBA8 PNG.
//
// This is the "verification harness": every scene's correctness is proven by
// the PNG this produces.
#pragma once

#include "vk_device.h"

#include <string>

class OffscreenTarget {
public:
    void init(VkDeviceContext& ctx, uint32_t w, uint32_t h, bool withDepth);
    void destroy(VkDeviceContext& ctx);

    uint32_t width() const { return mColor.extent.width; }
    uint32_t height() const { return mColor.extent.height; }
    const GpuImage& color() const { return mColor; }
    const GpuImage& depth() const { return mDepth; }
    bool hasDepth() const { return mDepth.image != VK_NULL_HANDLE; }

    VkFormat colorFormat() const { return mColor.format; }
    VkFormat depthFormat() const { return mDepth.format; }

    // Copies the color image (assumed left in TRANSFER_SRC layout by the caller,
    // or we transition it here) into a host buffer and writes an RGBA8 PNG.
    // Returns true on success. The image data is written top-to-bottom.
    bool readbackToPng(VkDeviceContext& ctx, const std::string& path);

private:
    GpuImage mColor{};
    GpuImage mDepth{};
};
