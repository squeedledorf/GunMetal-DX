/**
 * @file llcleanrtomm.h
 * @brief Opacity micromaps (VK_EXT_opacity_micromap, #194) for the clean renderer's
 *        ray tracing: alpha-mask cut-outs resolved by the RT hardware instead of an
 *        any-hit style shader alpha test per candidate.
 *
 * This file: the micro-triangle ordering (the space-filling "bird curve" every OMM
 * implementation shares) and the start-up SELF-TEST that proves it against the
 * driver before any micromap is used.
 */
#ifndef LL_LLCLEANRTOMM_H
#define LL_LLCLEANRTOMM_H

#include "vk_device.h"

#include <cstdint>

// Micro-triangle `index` of a subdivision `level` triangle -> its three corners in the
// base triangle's barycentrics (b1, b2) = the weights of vertices 1 and 2 (ray-query
// barycentrics). Upright micro-triangles wind like the base; inverted ones are flipped.
void ommIndexToBary(uint32_t index, uint32_t level, float uv[3][2]);

// Builds a one-BLAS scene whose micromaps each mark ONE micro-triangle opaque, traces a
// ray through every micro-triangle centroid and checks that exactly the expected ones
// hit (levels 1..3, 2-state). Logs the outcome. false = micromaps must not be used on
// this device (ordering mismatch, ray queries ignoring them, or a build failure).
bool ommSelfTest(VkDeviceContext& ctx);

#endif
