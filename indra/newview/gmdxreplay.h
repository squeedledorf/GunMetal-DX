/**
 * @file gmdxreplay.h
 * @brief GunMetal-DX instant replay: settings, saving, notifications (the encoder is vkgfx/gmdxreplayvk.cpp).
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

// <GMDX> INSTANT REPLAY (Combat > Instant Replay; Save Replay = Alt+F10). While GMDXReplayEnabled is
// on, the last GMDXReplaySeconds of the window (UI included) are kept H.264-encoded in memory; saving
// writes them to an MP4 in GMDXReplayFolder (default: the Videos folder, GunMetal-DX). Encoding runs on
// the GPU's video encoder; the frame only pays for one copy and a small conversion pass.

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

class VkDeviceContext;

namespace GMDXReplay
{
    // LLVulkanContext::endFrame: the finished image (COLOR_ATTACHMENT_OPTIMAL) on the frame's last
    // graphics command buffer, then right after the frame's graphics submission.
    void captureFrame(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkDeviceContext& dev);
    void afterSubmit(bool frameRan);   // frameRan false: the frame was dropped (its capture never ran)
    void shutdown(bool deviceLost);    // before the device is destroyed

    void save();       // menu / Alt+F10
}
