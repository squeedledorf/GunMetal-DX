// vkclean — native Vulkan clean-renderer prototype against the REAL clean
// pipeline data layouts. See clean_scene.cpp for the full description.
#pragma once

#include "vk_device.h"
#include "vk_offscreen.h"

#include <cstdint>
#include <string>

struct CleanResult {
    // Per-bucket emitted draw counts read back from the compute (GPU) vs a
    // CPU reference that runs the identical cull. All six must match.
    uint32_t gpu_bucket[6]  = {0,0,0,0,0,0};
    uint32_t cpu_bucket[6]  = {0,0,0,0,0,0};
    uint32_t instanceCount  = 0;
    bool     cullMatch      = false;   // gpu_bucket == cpu_bucket for all 6
    bool     rendered       = false;   // PNG written
    bool     nonEmpty       = false;   // PNG has coverage beyond the clear color
    uint32_t validationErrors   = 0;
    uint32_t validationWarnings = 0;
};

// Build the synthetic-but-representative scene in the REAL Instance / SlotInfo /
// LodGroup / Material layouts, run the compute cull that mirrors cleanCullC.glsl,
// draw all six pass buckets via vkCmdDrawIndexedIndirectCount with bindless
// per-material textures, and write `out_png`. Fills `out`.
bool sceneClean(VkDeviceContext& ctx, OffscreenTarget& t,
                const std::string& shaderDir, const std::string& out_png,
                CleanResult& out);
