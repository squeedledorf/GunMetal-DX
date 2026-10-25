// vktest — Stage 5 GPU-driven scenes (declared separately from scenes.cpp so
// the GPU-driven path + #29 sync stress test live in their own translation unit).
#pragma once

#include "vk_device.h"
#include "vk_offscreen.h"

#include <cstdint>
#include <string>

// PART A — persistent instance SSBO -> compute cull -> indirect-count draw ->
// bindless textures. Renders into `t`. Returns false if the compute draw count
// disagrees with the CPU reference (a real culling correctness failure).
bool sceneGpuCubes(VkDeviceContext& ctx, OffscreenTarget& t,
                   const std::string& dir);

// PART B — synchronization stress test. Fills `out` with per-phase validation
// tallies; phase-1 (unsynchronized) warnings are the expected experimental
// result, not a harness failure. Returns true if the experiment ran.
struct GpuStressResult {
    uint32_t hazardErrors = 0;
    uint32_t hazardWarnings = 0;
    uint32_t correctErrors = 0;
    uint32_t correctWarnings = 0;
    uint32_t finalDrawCount = 0;
    uint32_t expectedVisible = 0;
};
bool sceneGpuCubesStress(VkDeviceContext& ctx, OffscreenTarget& t,
                         const std::string& dir, GpuStressResult& out);
