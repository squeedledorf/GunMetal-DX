/**
 * @file gmdxcloudshapesvk.h
 * @brief GunMetal-DX: the volumetric clouds' sculpted cloud bodies.
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

// <GMDX> app_settings/gmdx_cloud_shapes.bin (CorvaeOboro's CC0 cloud volumes, packed by
// scripts/clouds/corvae_shapes.py) as one R8 3D image: up to 8 shapes of 64^3 in a 2 x 2 x 2
// block of 128^3 (shape k at x = k & 1, y = (k >> 1) & 1, z = k >> 2). Every shape rests on
// its cube's floor and keeps one empty texel layer around it, so a clamped trilinear read
// never mixes two shapes. Without the file: a 1-texel empty image and count() 0, so the
// binding stays valid and the shader places no shapes. Owned by LLCleanVolCloudsVK, bound at
// its compute set's binding 13 (gmdx_cloud_common.glsl uVcShapes).

#pragma once

#include "vk_device.h"

#include <cstdint>
#include <vector>

class GMDXCloudShapesVK
{
public:
    // Device idle: reads the file, creates the image and uploads it (submitImmediate).
    // A refused 128^3 image falls back to the 1-texel stand-in; false only when that is refused too.
    bool create(VkDeviceContext* ctx);
    void destroy(VkDeviceContext* ctx);
    // Binding 13 of set `set` (the layout's binding must be a COMBINED_IMAGE_SAMPLER).
    void write(VkDevice dev, VkDescriptorSet set, VkSampler clampLinear) const;
    uint32_t count() const { return mCount; }

    static constexpr uint32_t kBinding = 13;

private:
    bool upload(VkDeviceContext* ctx, const std::vector<uint8_t>& block, uint32_t side);

    GpuImage mImage;
    uint32_t mCount = 0;
};
