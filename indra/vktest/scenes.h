// vktest — scene registry. Each scene renders into an OffscreenTarget.
#pragma once

#include "vk_device.h"
#include "vk_offscreen.h"

#include <string>

// Renders the named scene into `target`. Returns true on success.
// Scenes leave the color image in COLOR_ATTACHMENT_OPTIMAL layout; the
// readback path transitions it to TRANSFER_SRC.
bool renderScene(const std::string& name, VkDeviceContext& ctx,
                 OffscreenTarget& target, const std::string& shaderDir);
