/**
 * @file vk_pipecache.h
 * @brief Persistent VkPipelineCache on disk (load / validate / save).
 *
 * The device context (VkDeviceContext) owns ONE process-wide VkPipelineCache, created
 * at device init from the file this module loads, and passed to every
 * vkCreateGraphicsPipelines / vkCreateComputePipelines call in the viewer (through
 * vkSharedPipelineCache(), vk_common.h). Without it every launch recompiles every
 * pipeline from scratch (~20 s of world-FS variants on a cold driver).
 *
 * FILE FORMAT (native endianness; a file from another machine fails the device check):
 *   VkPipeCacheFileHeader (below): magic "SLVKPCH\0", format version, header size,
 *     viewer build tag (channel + version), vendorID / deviceID / driverVersion /
 *     pipelineCacheUUID of the device that wrote it, payload size, payload CRC32.
 *   payload: exactly what vkGetPipelineCacheData returned. Its leading
 *     VkPipelineCacheHeaderVersionOne is validated too (header size / version,
 *     vendorID, deviceID, pipelineCacheUUID == this device) before the driver sees it:
 *     drivers can crash on bad cache data despite the spec.
 * Any mismatch / corruption: the file is ignored (and removed) and the cache starts
 * empty. Saves are atomic (temp file + rename). Payloads over kMaxBytes are neither
 * loaded nor saved.
 */
#pragma once

#include "vk_common.h"

#include <string>
#include <vector>

namespace vkpipecache
{
constexpr size_t kMaxBytes = 256u * 1024u * 1024u;

// Reads + validates `path` for this physical device / build tag. True with the driver
// payload in `out`; false (out empty) when absent, stale or corrupt (logged; a corrupt
// or stale file is deleted).
bool load(VkPhysicalDevice phys, const std::string& path, const std::string& buildTag,
          std::vector<uint8_t>& out);

// Writes the cache's current contents to `path` (atomic). Skipped when the payload
// size equals `lastBytes` (nothing new compiled since the last load / save); updates
// `lastBytes` on success. `why` is logged.
bool save(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& path,
          const std::string& buildTag, size_t& lastBytes, const char* why);

// Sets the process-wide cache returned by vkSharedPipelineCache() (vk_common.h).
void setShared(VkPipelineCache cache);

// LEARNED PIPELINE COMPILE COSTS (#471; vk_pipeline.h vkPipelineCostImport / Export),
// a small text file next to the cache. Unlike the cache it is NOT tied to the viewer
// build (a new release is exactly when everything compiles cold; changed shaders get new
// keys anyway), only to the device + driver (vendorID, deviceID, driverVersion,
// pipelineCacheUUID): a driver update starts fresh. Layout:
//   SLVKCOST 1
//   device <vendorID> <deviceID> <driverVersion> <pipelineCacheUUID hex>
//   crc <CRC32 of the body, hex>
//   <body: vkPipelineCostExport text>
// Bounded (kMaxCostBytes). Mismatch / corruption: deleted, start fresh.
constexpr size_t kMaxCostBytes = 1024u * 1024u;
// Reads + validates `path` into the vk_pipeline cost table (always starts a new session,
// empty if the file is missing or stale). Returns the entries loaded.
size_t loadCosts(VkPhysicalDevice phys, const std::string& path);
// Writes the vk_pipeline cost table to `path` (atomic). Any thread; serialized.
bool saveCosts(VkPhysicalDevice phys, const std::string& path, const char* why);
}
