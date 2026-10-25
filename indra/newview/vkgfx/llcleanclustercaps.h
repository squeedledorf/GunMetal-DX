/**
 * @file llcleanclustercaps.h
 * @brief Cluster LOD (#743): per-capability TIERS chosen from what the device enabled
 *        (VkDeviceContext::clusterFeatures()), capped by RenderVKClusterForceTier.
 *
 * Every capability uses the best path the running GPU supports; slower paths are additive
 * fallbacks, chosen by EXTENSION / FEATURE presence at runtime -- never by GPU name. Every
 * fallback is reachable on the newest card: RenderVKClusterForceTier = N caps each capability
 * at min(native, N) (-1 = native). Capabilities and tiers (higher = newer):
 *
 *   RASTER   0 MDI_PULL   compute selection + vkCmdDrawIndirectCount, non-indexed, the VS
 *                         pulls the uint8 index from the page pool (core Vulkan; the floor)
 *            1 MDI_U8     same, indexed: the pool bound as a VK_INDEX_TYPE_UINT8 index buffer
 *                         (VK_KHR / EXT_index_type_uint8)
 *            2 MESH       task + mesh shaders (VK_EXT_mesh_shader; Vulkan 1.3 path only)
 *   CMDGEN   0 COUNT      per-bucket indirect draws with a GPU count (core 1.2 drawIndirectCount)
 *            1 DGC        VK_EXT_device_generated_commands (pipeline / bucket switches generated
 *                         on the GPU)
 *   UPLOAD   0 COPY       host staging ring + vkCmdCopyBuffer per page
 *            1 INDIRECT   GPU-built copy lists: VK_KHR (or NV)_copy_memory_indirect
 *            2 DECOMPRESS GDeflate pages decompressed on the GPU: VK_EXT (or NV)_memory_decompression
 *                         (needs GDeflate payloads in the cache -- see the contract; never
 *                         native yet: held at INDIRECT, "held lower" says so, until an
 *                         encoder is in the tree)
 *   RT       -1 NONE      ray tracing off (RenderVKRayTracing / no hardware): no RT cut
 *            0 BLAS       per-cut BLAS rebuilds (KHR acceleration structures)
 *            1 CLAS       VK_NV_cluster_acceleration_structure: cluster AS per resident cluster
 *            2 CLAS_PTLAS + VK_NV_partitioned_acceleration_structure (partitioned TLAS)
 *   ATOMIC64 0 ATOMIC32   32-bit atomics only (request priority quantized into the key)
 *            1 ATOMIC64   shaderBufferInt64Atomics (+ image int64 atomics when listed)
 *
 * A capability whose extension is present but whose LIMITS do not fit the format (mesh
 * shader output / invocations, CLAS max vertices / triangles below kMaxClusterVerts / Tris)
 * is held one tier lower and says why in the log line.
 */
#pragma once

#include "vk_device.h"

#include <cstdint>
#include <string>

enum LLClusterCap : uint32_t
{
    CLCAP_RASTER = 0,
    CLCAP_CMDGEN,
    CLCAP_UPLOAD,
    CLCAP_RT,
    CLCAP_ATOMIC64,
    CLCAP_COUNT
};

enum : int32_t
{
    CLT_RASTER_MDI_PULL = 0, CLT_RASTER_MDI_U8 = 1, CLT_RASTER_MESH = 2,
    CLT_CMDGEN_COUNT = 0, CLT_CMDGEN_DGC = 1,
    CLT_UPLOAD_COPY = 0, CLT_UPLOAD_INDIRECT = 1, CLT_UPLOAD_DECOMPRESS = 2,
    CLT_RT_NONE = -1, CLT_RT_BLAS = 0, CLT_RT_CLAS = 1, CLT_RT_CLAS_PTLAS = 2,
    CLT_ATOMIC32 = 0, CLT_ATOMIC64 = 1,
};

struct LLCleanClusterCaps
{
    int32_t native[CLCAP_COUNT] = {};   // best tier the device supports
    int32_t tier[CLCAP_COUNT] = {};     // in force: min(native, force) (RT_NONE stays NONE)
    int32_t force = -1;                 // RenderVKClusterForceTier (-1 = native)
    bool    asyncCompute = false;       // a second (compute) queue exists: page copies may run there
    bool    asyncComputeOnly = false;   // ... in a compute-only family (CONCURRENT sharing)
    uint64_t maxStorageBufferRange = 0; // bounds the page pool binding
    VkClusterFeatures feat;             // what the device enabled (+ limits)
    std::string why;                    // tiers held below their extension's level, and why

    // From the created device. `force` = RenderVKClusterForceTier.
    static LLCleanClusterCaps detect(const VkDeviceContext& ctx, int32_t force);
    // Re-cap the tiers (live RenderVKClusterForceTier change). True when a tier or the setting
    // changed (the caller logs logLine()). Every module picks its tier per frame by get() from
    // pipelines / buffers made for every NATIVE tier, so a change takes effect the next frame.
    bool applyForce(int32_t force);
    int32_t get(LLClusterCap c) const { return tier[c]; }

    // One line: every capability with its chosen tier (and native when forced lower).
    std::string logLine() const;
    static const char* capName(uint32_t cap);
    static const char* tierName(uint32_t cap, int32_t tier);
};
