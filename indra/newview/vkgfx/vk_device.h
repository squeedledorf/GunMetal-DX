// vktest — reusable Vulkan RHI: instance + device + queue + allocation helpers.
//
// This is the foundation layer. It owns:
//   * VkInstance (with validation layer + debug messenger)
//   * physical device selection (prefers discrete GPU)
//   * a single graphics+compute+transfer queue (all-in-one on NVIDIA)
//   * a command pool
//   * low-level buffer / image creation with a simple memory-type picker
//
// Everything above (offscreen targets, pipelines, scenes) builds on this.
// The design deliberately exposes buffer-device-address, descriptor-indexing
// and dynamic-rendering features so a later GPU-driven stage (instance SSBO +
// compute cull + multi-draw-indirect-count + bindless) can turn them on without
// restructuring this layer.
#pragma once

#include "vk_common.h"
#include "vk_requirements.h"

#include <map>
#include <functional>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// Vulkan Memory Allocator handles (#411; vk_vma.h has the API). Repeating VMA's own
// VK_DEFINE_HANDLE typedefs keeps vk_mem_alloc.h out of every includer of this header.
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaPool_T* VmaPool;
typedef struct VmaAllocation_T* VmaAllocation;

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;          // non-null if host-visible + mapped
    VkDeviceAddress address = 0;     // non-zero if created with device-address
    // Unique per createBuffer (never reused). A freed buffer's VkBuffer handle and address
    // can come back for the next create, so a descriptor cache keyed on handles can miss a
    // re-create and keep pointing at freed memory: key such caches on `serial` too.
    uint64_t serial = 0;
};

// Optional image-create extras (#565, reflection-probe storage formats): create flags
// beyond CUBE_COMPATIBLE (MUTABLE_FORMAT / EXTENDED_USAGE / BLOCK_TEXEL_VIEW_COMPATIBLE),
// the view-format list (VkImageFormatListCreateInfo, keeps mutable images optimal) and the
// usage the DEFAULT view is restricted to (0 = the image's usage).
struct VkImageExtra {
    VkImageCreateFlags flags = 0;
    const VkFormat* viewFormats = nullptr;
    uint32_t viewFormatCount = 0;
    VkImageUsageFlags defaultViewUsage = 0;
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    // The image's memory: its own allocation, or (texture pools, #411) the VMA block it
    // is suballocated from -- then shared with other images, so never an identity: compare
    // `image`, size it with VkDeviceContext::allocBytes(const GpuImage&).
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
};

// ---- VRAM CATEGORIES (#407) ----
// Every device allocation VkDeviceContext makes carries exactly one category: no
// "other" bucket. The category comes from the innermost VkMemScope on the allocating
// thread; without one it is inferred from the usage / memory flags (vkMemInfer) and the
// allocation is counted as "inferred" too, so the log shows where a scope is missing.
// Buffers with acceleration-structure / micromap storage usage are always RT_AS.
enum VkMemCat : uint8_t
{
    VKMEM_TEXTURE = 0,  // bindless world textures (shared + slot-owned + media)
    VKMEM_GEOMETRY,     // packed vertex / index / weight buffers (skinned output: VKMEM_RIG)
    VKMEM_SCENE,        // instance / slot / material / animation tables
    VKMEM_CULL,         // cull + occlusion + indirect command buffers
    VKMEM_TARGET,       // render targets, G-buffer, history, post chains
    VKMEM_PROBE,        // reflection probe arrays + capture targets
    VKMEM_SHADOW,       // sun cascades + projector atlas
    VKMEM_MIRROR,       // hero / planar mirror targets
    VKMEM_RT_AS,        // BLAS / TLAS / micromap storage
    VKMEM_RT_AUX,       // RT scratch, instance, geometry, rig position buffers
    VKMEM_RT_MASK,      // RT shadow mask images
    VKMEM_STAGING,      // host-visible upload buffers and rings
    VKMEM_READBACK,     // host-visible GPU -> CPU buffers
    VKMEM_PARTICLE,     // GPU particle pools
    VKMEM_UI,           // LLRenderVK (UI) textures, targets, vertex rings
    VKMEM_UPSCALER,     // FSR 3 resources
    VKMEM_TEXENC,       // texture block-compression scratch / pool
    VKMEM_RIG,          // rigged skin output + its temporal copies, work / rig draw lists, palettes
    VKMEM_DENOISE,      // diffuse / specular lighting denoiser images (#85 phase 2, LLCleanDenoiserVK)
    VKMEM_COUNT
};
const char* vkMemCatName(uint32_t cat);
// The calling function of an allocation, recorded for INFERRED allocations only (#407):
// VkDeviceContext::inferredSitesText names where a VkMemScope is missing.
#if defined(__clang__) || defined(__GNUC__) || (defined(_MSC_VER) && _MSC_VER >= 1926)
#  define VK_MEM_SITE __builtin_FUNCTION()
#else
#  define VK_MEM_SITE nullptr
#endif
// ---- DRIVER-REPORTED MEMORY (#407, VK_EXT_device_memory_report) ----
// What the driver says it allocated, aggregated by the Vulkan object type that caused it
// (a fixed table, no per-allocation list kept beyond the live object map the FREE events
// need). VKDRV_DEVMEM = vkAllocateMemory (ours, and anything that bypasses the tracker);
// the rest is driver-internal. On NVIDIA the PIPELINE entries are suballocated from code
// heap chunks reported as DEVICE, so pipe overlaps device there (raw numbers, not a sum).
enum VkDrvMemBucket : uint8_t
{
    VKDRV_DEVMEM = 0,   // VK_OBJECT_TYPE_DEVICE_MEMORY
    VKDRV_DEVICE,       // device / instance / physical device (driver heaps, code heap)
    VKDRV_QUEUE,
    VKDRV_CMD,          // command pools / buffers
    VKDRV_DESC,         // descriptor pools / sets / layouts
    VKDRV_PIPE,         // pipelines / shader modules / pipeline cache / layouts
    VKDRV_QUERY,
    VKDRV_SWAPCHAIN,    // swapchain (when the WSI reports its images this way)
    VKDRV_RESOURCE,     // images / buffers / views / samplers (driver side data)
    VKDRV_ACCEL,        // acceleration structures / micromaps
    VKDRV_SYNC,         // fences / semaphores / events
    VKDRV_OTHER,
    VKDRV_COUNT
};
const char* vkDrvBucketName(uint32_t b);
// Scoped category for allocations made on this thread (nests; restores on exit).
struct VkMemScope
{
    explicit VkMemScope(VkMemCat c);
    ~VkMemScope();
    VkMemScope(const VkMemScope&) = delete;
    VkMemScope& operator=(const VkMemScope&) = delete;
private:
    int mPrev;
};

// ---- CLUSTER LOD DEVICE FEATURES (#743) ----
// What the device ENABLED for the cluster LOD path (each flag: extension listed AND feature
// bit supported AND enabled at vkCreateDevice), plus the limits the tiers check. Filled by
// createLogicalDevice; all false / 0 on a device without them. Enabled whenever present when
// RenderVKClusterLOD is on at startup; otherwise only those inert while unused (per
// RenderVKClusterDeviceFeatures, vk_device.cpp "CLUSTER LOD"), so cluster LOD turned on
// mid-session runs them without a restart. Never a device requirement:
// a missing extension only lowers a tier, and if vkCreateDevice refuses the device with
// them it is retried once without any of them (clusterFeatures().retried).
struct VkClusterFeatures
{
    bool meshShader = false;          // VK_EXT_mesh_shader: taskShader + meshShader
    bool indexTypeUint8 = false;      // VK_KHR_index_type_uint8 (or the EXT): VK_INDEX_TYPE_UINT8
    bool dgc = false;                 // VK_EXT_device_generated_commands (+ VK_KHR_maintenance5)
    bool dgcNVCompute = false;        // VK_NV_device_generated_commands_compute listed (info only)
    bool memDecompEXT = false;        // VK_EXT_memory_decompression
    bool memDecompNV = false;         // VK_NV_memory_decompression (when the EXT is absent)
    bool copyIndirectKHR = false;     // VK_KHR_copy_memory_indirect
    bool copyIndirectNV = false;      // VK_NV_copy_memory_indirect (when the KHR is absent)
    bool clusterAS = false;           // VK_NV_cluster_acceleration_structure (ray tracing on only)
    bool partitionedAS = false;       // VK_NV_partitioned_acceleration_structure (ray tracing on only)
    bool bufferInt64Atomics = false;  // shaderInt64 + shaderBufferInt64Atomics (core 1.2)
    bool imageInt64Atomics = false;   // VK_EXT_shader_image_atomic_int64
    bool retried = false;             // vkCreateDevice refused them: device created without
    bool lodAtStartup = false;        // RenderVKClusterLOD was on at startup: every one considered
    uint32_t deviceFeatures = 0;      // RenderVKClusterDeviceFeatures at startup (0 / 1 / 2)
    // limits (0 = not enabled)
    uint32_t meshMaxOutputVertices = 0, meshMaxOutputPrimitives = 0, meshMaxWorkGroupInvocations = 0;
    uint32_t meshMaxOutputMemory = 0, meshMaxSharedMemory = 0, taskMaxPayload = 0;
    uint32_t clasMaxVerticesPerCluster = 0, clasMaxTrianglesPerCluster = 0;
    uint64_t decompressionMethods = 0;  // VkMemoryDecompressionMethodFlags (GDeflate 1.0 = bit 0)
    // what was listed but not enabled, for the log ("mesh_shader: needs the 1.3 path", ...)
    std::string notes;
};

class VkDeviceContext {
public:
    // enableValidation turns on the Khronos validation layer + a debug
    // messenger that routes messages through onDebugMessage(). Sync validation
    // is requested via VK_EXT_layer_settings when available.
    bool init(bool enableValidation);

    // Viewer-integration variant. Extra instance extensions (e.g. the surface
    // extensions SDL_Vulkan_GetInstanceExtensions returns) are enabled on the
    // VkInstance, and VK_KHR_swapchain is added to the device so a swapchain can
    // be created. The physical device's single graphics/compute/transfer queue
    // family on NVIDIA also supports present on X11/Wayland; callers that need a
    // strict present-capability check can validate via
    // vkGetPhysicalDeviceSurfaceSupportKHR after creating their surface.
    bool initForPresent(bool enableValidation,
                        const std::vector<const char*>& extraInstanceExts,
                        bool wantSwapchain);
    void shutdown();

    // #526: what init*() found. When it returns false (no Vulkan driver, no device that
    // meets the requirements, or vkCreateInstance / vkCreateDevice failed) this holds
    // every device's evaluation and the failing step (report.failure); the caller shows
    // vkRequirementsUserMessage() and exits instead of crashing. Also filled on success
    // (the chosen device + why).
    const VkRequirementsReport& requirementsReport() const { return mReqReport; }
    // #675: true = the device runs on the Vulkan 1.2 + extensions path (a < 1.3 device, or
    // RenderVKForceApi12): KHR dynamic rendering / synchronization2 and EXT extended dynamic
    // state through vk_compat.h instead of the 1.3 core entry points.
    bool usesApi12() const { return mApi12; }

    // ---- PERSISTENT PIPELINE CACHE (vk_pipecache.h) ----
    // Set BEFORE init*(): the on-disk cache file (empty = no disk cache: an in-memory
    // cache is still shared by every pipeline creation of this run) and the viewer
    // build tag that must match for the file to load. init*() loads + validates the
    // file and creates THE process-wide VkPipelineCache (vkSharedPipelineCache()).
    void setPipelineCacheFile(const std::string& path, const std::string& buildTag)
    {
        mPipeCachePath = path;
        mPipeCacheTag = buildTag;
    }
    VkPipelineCache pipelineCache() const { return mPipeCache; }
    // The disk file + build tag above (empty path = no disk cache): the setting
    // specialization sets keep their per-key caches next to it, under the same tag.
    const std::string& pipelineCacheFile() const { return mPipeCachePath; }
    const std::string& pipelineCacheTag() const { return mPipeCacheTag; }
    // The cache was seeded from a valid disk file at init (#471: warm compiles expected).
    bool pipelineCacheWarm() const { return mPipeCacheWarm; }
    // Write the cache to disk if pipelines were created since the last load / save.
    // Call only while no pipeline compile is in flight (no async batch job).
    // shutdown() saves once more before destroying the cache.
    bool savePipelineCache(const char* why);

    // ---- accessors ----
    VkInstance instance() const { return mInstance; }
    // Debug name for validation / GPU-crash messages (VK_EXT_debug_utils; no-op
    // unless the instance enabled it, i.e. SLIPSTREAM_VK_VALIDATION).
    void setObjectName(VkObjectType type, uint64_t handle, const char* name) const;
    VkPhysicalDevice physicalDevice() const { return mPhysical; }
    bool depthClampEnabled() const { return mDepthClamp; }
    bool depthBiasClampEnabled() const { return mDepthBiasClamp; }   // <GMDX>
    // fragmentStoresAndAtomics enabled: storage writes from fragment shaders (the raster
    // cursor pick's fragment list, #702). A device requirement since #704: always true
    // on a created device.
    bool fragmentStoresEnabled() const { return mFragStores; }
    // #546: VK_EXT_pipeline_robustness is enabled; the best robust behaviours the device
    // supports, for pipelines built from user shaders (the effects stack).
    bool pipelineRobustnessEnabled() const { return mPipeRobust; }
    VkPipelineRobustnessBufferBehaviorEXT robustBufferBehavior() const { return mRobustBuffer; }
    VkPipelineRobustnessImageBehaviorEXT robustImageBehavior() const { return mRobustImage; }
    // shaderStorageImageExtendedFormats enabled (r8 / r16f / rg16f ... storage images).
    bool storageImageExtendedFormats() const { return mStorageExtFormats; }
    // textureCompressionBC enabled and every BC format the texture encoder writes is
    // sampleable + copyable (RenderVKTextureCompression; false = compression stays off).
    bool textureCompressionBC() const { return mTexCompBC; }
    // Rigged-cull subgroup scan (clean_wg_scan.glsl; rigged-cull crash, TheSlowly report):
    // subgroupSizeControl + computeFullSubgroups enabled, basic + arithmetic subgroup ops in
    // compute, max subgroup size divides 256. Only then are the probe / shadow rigged-cull
    // pipelines created with REQUIRE_FULL_SUBGROUPS and allowed the subgroup scan.
    bool wgScanFullSubgroups() const { return mWgScanFullSubgroups; }
    bool computeFullSubgroups() const { return mComputeFullSubgroups; }
    uint32_t subgroupSize() const { return mSubgroupSize; }
    uint32_t subgroupSizeMin() const { return mSubgroupSizeMin; }
    uint32_t subgroupSizeMax() const { return mSubgroupSizeMax; }
    uint32_t subgroupOps() const { return mSubgroupOps; }
    // shaderStorageImageArrayDynamicIndexing enabled (#565: the BC6H probe encoder indexes
    // its single-layer block-texel views by the GPU-chosen slot).
    bool storageImageArrayDynamicIndexing() const { return mStorageImgArrayDynIdx; }
    // wideLines enabled? Without it every line width must be exactly 1.0.
    bool wideLinesEnabled() const { return mWideLines; }
    float lineWidthMax() const { return mLineWidthMax; }
    // Device exposes VK_KHR_acceleration_structure + VK_KHR_ray_query +
    // VK_KHR_deferred_host_operations and the accelerationStructure / rayQuery /
    // bufferDeviceAddress feature bits (capability, independent of the request).
    bool rayQuerySupported() const { return mRayQuerySupported; }
    // Set BEFORE init*(): ask for the RT extensions to be enabled (RenderVKRayTracing).
    void setRayTracingRequested(bool on) { mRayTracingRequested = on; }
    bool rayTracingRequested() const { return mRayTracingRequested; }
    // RenderVKClusterLOD at startup: every cluster extension the device has is enabled when it
    // is on (incl. the startup-only ones: DGC + maintenance5, partitioned TLAS).
    void setClusterRequested(bool on) { mClusterRequested = on; }
    // RenderVKPipelineStats at startup: enable VK_KHR_pipeline_executable_properties
    // (pipelineExecutableInfo) when the device has it and hand vk_pipeline its entry points
    // (vkSetPipelineStatsCapture: per-pipeline register / instruction statistics in the log).
    void setPipelineStatsRequested(bool on) { mPipelineStatsRequested = on; }
    bool pipelineStatsEnabled() const { return mPipelineStatsEnabled; }
    // RenderVKShaderClockLighting at startup: enable VK_KHR_shader_clock (shaderSubgroupClock)
    // when the device has it -- the deferred lighting's per-stage cycle counters (the *ClockSpv
    // kernels, clean_shader_clock.glsl). Off: nothing changes.
    void setShaderClockRequested(bool on) { mShaderClockRequested = on; }
    bool shaderClockEnabled() const { return mShaderClockEnabled; }
    // One line of device-wide shader-core facts for the statistics: subgroup size(s), and
    // the AMD register file / SIMD shape or the NVIDIA SM / warp counts where listed.
    std::string shaderCoreSummary() const;
    // RenderVKClusterDeviceFeatures at startup: with RenderVKClusterLOD off, 0 = no cluster
    // extension (the device as before #743), 1 = those inert while unused, 2 = + mesh shaders.
    void setClusterDeviceFeatures(uint32_t level) { mClusterDeviceFeatures = level; }
    // Requested AND supported AND the entry points resolved: the RT extensions are
    // enabled on the device. Every RT code path must gate on this.
    bool rayTracingEnabled() const { return mRayTracingEnabled; }
    // Why RT is (not) on, e.g. "enabled", "off (RenderVKRayTracing=0)",
    // "unsupported: missing VK_KHR_ray_query". Shown in the log and About info.
    const std::string& rayTracingStatus() const { return mRTStatus; }
    VkDeviceSize asScratchAlignment() const { return mAsScratchAlign; }
    struct RTFuncs {
        PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
        PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR = nullptr;
        // VK_EXT_opacity_micromap (#194); null unless opacityMicromapEnabled().
        PFN_vkCreateMicromapEXT vkCreateMicromapEXT = nullptr;
        PFN_vkDestroyMicromapEXT vkDestroyMicromapEXT = nullptr;
        PFN_vkCmdBuildMicromapsEXT vkCmdBuildMicromapsEXT = nullptr;
        PFN_vkGetMicromapBuildSizesEXT vkGetMicromapBuildSizesEXT = nullptr;
    };
    // VK_EXT_opacity_micromap enabled with ray tracing (hardware alpha-tested rays, #194).
    bool opacityMicromapEnabled() const { return mOmmEnabled; }
    // VK_KHR_fragment_shading_rate with per-pipeline (dynamic) rates.
    bool shadingRateEnabled() const { return mVrsEnabled; }
    // Pipeline shading rate (w x h fragment area, 1 or 2 each) for the bound pipeline
    // (must carry the dynamic state, GraphicsPipelineDesc::dynamicShadingRate).
    void cmdSetShadingRate(VkCommandBuffer cmd, uint32_t w, uint32_t h) const
    {
        if (!mCmdSetShadingRate) return;
        const VkExtent2D e{w, h};
        const VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                                                           VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        mCmdSetShadingRate(cmd, &e, ops);
    }
    // VRS tier 2 (RenderVKOpaqueShadingRate): a per-tile shading-rate ATTACHMENT
    // (attachmentFragmentShadingRate, R8_UINT storage-writable and usable as the
    // attachment, a square texel size of 8..32 px). shadingRateTexel() = that tile size.
    bool shadingRateImageEnabled() const { return mVrsImageEnabled; }
    // VK_KHR_fragment_shader_barycentric enabled (#664: the RT terminator triangle pass).
    bool fragmentShaderBarycentricEnabled() const { return mFragBaryEnabled; }
    // CLUSTER LOD (#743): the optional extensions / features the cluster path uses, enabled
    // at device creation: all the device has when RenderVKClusterLOD is on at startup, else the
    // ones inert while unused (RenderVKClusterDeviceFeatures; see vk_device.cpp "CLUSTER LOD").
    // Tiers are derived from this in LLCleanClusterCaps (llcleanclustercaps.h).
    const VkClusterFeatures& clusterFeatures() const { return mCluster; }
    uint32_t shadingRateTexel() const { return mVrsTexel; }
    // For a pipeline with the dynamic rate: 1x1 pipeline rate, and the attachment's rate
    // REPLACEs it (replace = true) or is ignored (false: every pixel shaded).
    void cmdSetShadingRateAttachment(VkCommandBuffer cmd, bool replace) const
    {
        if (!mCmdSetShadingRate) return;
        const VkExtent2D e{1, 1};
        const VkFragmentShadingRateCombinerOpKHR ops[2] = {
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
            replace ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        mCmdSetShadingRate(cmd, &e, ops);
    }
    uint32_t ommMaxLevel4State() const { return mOmmMaxLevel4; }
    const RTFuncs& rt() const { return mRT; }
    VkDevice device() const { return mDevice; }
    VkQueue queue() const { return mQueue; }
    uint32_t queueFamily() const { return mQueueFamily; }
    VkCommandPool commandPool() const { return mCommandPool; }

    // ---- ASYNC COMPUTE QUEUE (roadmap phase 3a; RenderVKAsyncComputeQueue) ----
    // Set BEFORE init*(): 0 = no second queue (single-queue renderer, every resource
    // EXCLUSIVE), 1 = a dedicated compute-only family when the device has one (NVIDIA:
    // yes), else a second queue of the graphics family, 2 = always a second queue of the
    // graphics family (no CONCURRENT sharing, no barrier sanitizing, blits allowed).
    void setAsyncComputeMode(uint32_t m) { mAsyncMode = m; }
    // RenderVKAsyncCompute is -1 (auto) and RenderVKAsyncComputeQueue is at its default (the
    // user chose neither): the per-architecture default (asyncAutoDecide, vk_device.cpp) may
    // then turn the second queue off at device selection.
    void setAsyncComputeAuto(bool a) { mAsyncAuto = a; }
    // #845 / #917 diagnostic (RenderVKDebugRobustness, restart): set BEFORE init*(). On, the
    // device is created with robustBufferAccess + VK_EXT_robustness2 (robustBufferAccess2,
    // robustImageAccess2, nullDescriptor) where supported, so EVERY pipeline (the engine's
    // included, not only the effects stack) gets bounds-checked descriptor access: an
    // out-of-range / null descriptor read returns zero instead of faulting. If a device loss
    // stops with it on, a descriptor read was out of range or unbound; if it does not, the
    // fault is a raw device address (BDA / AS build input) or a freed allocation.
    void setDebugRobustness(bool on) { mDebugRobustReq = on; }
    // #919: optional device extensions a caller wants when the device lists them (e.g.
    // VK_EXT_full_screen_exclusive on Windows). Set BEFORE init*(); the ones the device
    // lists are enabled, the others are skipped (optionalExtensionEnabled says which).
    void requestOptionalExtension(const char* name) { if (name) mOptionalExtsRequested.emplace_back(name); }
    bool optionalExtensionEnabled(const char* name) const;
    // #870: a LATE optional-extension provider, asked after the GPU is picked (it needs the
    // instance + physical device: e.g. an optional neural reconstruction library's own
    // query) and before vkCreateDevice. Its list is all-or-nothing: enabled only when the
    // device lists every one; *why is set when it is not (or the provider declined). Never a
    // reason for device creation to fail: a refusal with them is retried without them.
    using LateExtProvider = std::function<bool(VkInstance, VkPhysicalDevice, std::vector<std::string>& exts,
                                               std::string& why)>;
    void setLateExtensionProvider(LateExtProvider p) { mLateExtProvider = std::move(p); }
    // The provider's extensions are enabled on the device (false: see lateExtensionsWhy()).
    bool lateExtensionsEnabled() const { return mLateExtsOn; }
    const std::vector<std::string>& lateExtensions() const { return mLateExts; }
    const std::string& lateExtensionsWhy() const { return mLateExtsWhy; }
    bool debugRobustnessActive() const { return mDebugRobustOn; }
    const std::string& debugRobustnessStatus() const { return mDebugRobustStatus; }
    bool asyncComputeAvailable() const { return mComputeQueue != VK_NULL_HANDLE; }
    VkQueue computeQueue() const { return mComputeQueue; }
    // <GMDX> instant replay: the video encode queue (null when the device has none)
    VkQueue encodeQueue() const { return mEncodeQueue; }
    uint32_t encodeFamily() const { return mEncodeFamily; }
    uint32_t computeFamily() const { return mComputeFamily; }
    // The async queue's family has no GRAPHICS bit: barriers recorded into its command
    // buffers are sanitized (vkqCmdPipelineBarrier2, vk_common.h), graphics-only commands
    // (blit, draw) cannot go there, and every resource is created CONCURRENT across the
    // two families (no queue-family ownership transfers).
    bool computeQueueComputeOnly() const { return mComputeOnly; }
    // The async family writes timestamps (timestampValidBits > 0).
    bool computeTimestamps() const { return mComputeTsBits != 0; }
    // e.g. "dedicated family 2", "graphics family 0 queue 1", "off (mode 0)".
    const std::string& asyncComputeStatus() const { return mAsyncStatus; }
    // How the async queue was decided: "auto -> off (NVIDIA pre-Turing ...)", "auto -> on
    // (...)" or "user-set (...)". Startup log, About info, device-lost report.
    const std::string& asyncComputeDecision() const { return mAsyncDecision; }

    // ---- one-shot command submission (blocking) ----
    // Records via the callback into a primary command buffer, submits, waits.
    // `file` / `line` default to the CALLER's location (#845 / #917): the submission is
    // breadcrumbed and logged under it ("imm <file>:<line>"), so a device loss between frames
    // names the out-of-frame submit that was running.
    void submitImmediate(const std::function<void(VkCommandBuffer)>& record,
                         const char* file = __builtin_FILE(), int line = __builtin_LINE());

    // ---- resource creation ----
    GpuBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props,
                           bool deviceAddress = false, const char* site = VK_MEM_SITE);
    void destroyBuffer(GpuBuffer& b);

    // mutableAltFormat != UNDEFINED: create with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and a
    // {format, mutableAltFormat} format list, so a second view in the other format can
    // alias the same memory (sRGB + UNORM views of one texture).
    GpuImage createImage2D(uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage,
                           VkImageAspectFlags aspect,
                           uint32_t mipLevels = 1,
                           VkFormat mutableAltFormat = VK_FORMAT_UNDEFINED, const char* site = VK_MEM_SITE);
    // 3D colour image (one mip), e.g. a colour-grading LUT.
    GpuImage createImage3D(uint32_t w, uint32_t h, uint32_t d, VkFormat format, VkImageUsageFlags usage,
                           const char* site = VK_MEM_SITE);
    void destroyImage(GpuImage& img);
    void destroyView(VkImageView& v);

    // ---- VRAM accounting (every vkAllocateMemory/vkFreeMemory goes through here) ----
    struct MemStats
    {
        uint64_t heapUsed[VK_MAX_MEMORY_HEAPS]  = {};   // our live bytes per heap
        uint64_t heapBudget[VK_MAX_MEMORY_HEAPS] = {};  // VK_EXT_memory_budget (else heap size)
        uint64_t heapDriverUsage[VK_MAX_MEMORY_HEAPS] = {}; // process-wide usage (budget ext)
        uint32_t heapCount = 0;
        bool     heapDeviceLocal[VK_MAX_MEMORY_HEAPS] = {};
        // #695 UMA (integrated GPU with a carve-out, VkUmaConfig): the carve-out heap and the
        // shared system-memory heap form ONE budget (budgetHeaps). heapBudget[umaDevHeap] is
        // already clamped by RenderVKDebugFakeCarveoutMB; umaHostCeil = the most of the host
        // heap the budget counts (the RAM share). On UMA catDev / devLocalBytes / the texture
        // pools' "dev" mean "in the budgeted heaps" (both), so host-heap textures still count
        // as textures for the ladder; heapDeviceLocal stays the raw heap flag.
        bool     uma = false;
        uint32_t umaDevHeap = ~0u, umaHostHeap = ~0u;
        uint64_t umaHostCeil = 0;
        uint64_t umaSpills = 0, umaSpillBytes = 0;   // device-local requests placed on the host heap (monotonic)
        uint64_t bufferBytes = 0, imageBytes = 0, hostVisibleBytes = 0;
        uint32_t allocCount = 0;
        bool     budgetExt = false;
        // #407: live bytes per category, split device-local heap / system heap, the
        // per-category high-water mark (since start), allocation counts, and how much
        // of the total was attributed by inference (no VkMemScope at the call).
        uint64_t catDev[VKMEM_COUNT] = {}, catHost[VKMEM_COUNT] = {}, catPeak[VKMEM_COUNT] = {};
        // per-category high-water since the last resetCatPeakInterval() (the VKMem
        // breakdown line resets it after printing: "peak since the last line")
        uint64_t catPeakInt[VKMEM_COUNT] = {};
        uint32_t catCount[VKMEM_COUNT] = {};
        uint64_t inferredBytes = 0;
        uint64_t devLocalBytes = 0, devLocalPeak = 0;   // all device-local heaps
        // #407: page padding the driver adds to each device-local allocation (it rounds
        // every vkAllocateMemory up to its granularity: 64 KiB on NVIDIA, measured), per
        // category and in total. Real VRAM that no category's bytes include.
        uint64_t catPad[VKMEM_COUNT] = {};
        uint64_t padBytes = 0;
        uint64_t allocGranularity = 0;    // bytes; measured at device creation, else assumed
        bool     granularityMeasured = false;
        // #407: VK_EXT_device_memory_report (driver-side view), live bytes per heap and
        // bucket, imported memory per heap, and allocation failures the driver reported.
        bool     drvReport = false;
        uint64_t drvHeap[VK_MAX_MEMORY_HEAPS][VKDRV_COUNT] = {};
        uint64_t drvImport[VK_MAX_MEMORY_HEAPS] = {};
        uint32_t drvObjects = 0, drvFailures = 0;
        // #411: TEXTURE SUBALLOCATION (VMA pools, texture + UI images). Pool blocks are
        // tracked allocations (heapUsed / allocCount / imageBytes; pad ~0) that belong to no
        // category; the categories count the LIVE suballocations (memory requirement
        // sizes), so texPoolSlack = block bytes - live bytes is the rest of `tracked`:
        // free space in the blocks (texPoolBlockBytes - texPoolRounded) + size-class
        // rounding (texPoolRounded - texPoolLive).
        static constexpr uint32_t kTexPoolMax = 32;
        struct TexPool
        {
            uint32_t classBytes = 0;          // slab slot size; 0 = the medium (TLSF) fallback pool
            uint64_t blockSize = 0;           // the pool's block size
            uint32_t slotsPerBlock = 0;       // slab slots in one block (0 = the fallback pool)
            uint32_t blocks = 0, allocs = 0;  // VMA: live blocks / suballocations
            uint64_t blockBytes = 0;          // VMA: bytes in its blocks
            uint64_t roundedBytes = 0;        // VMA: suballocated bytes (class-rounded)
            uint64_t liveBytes = 0;           // memory requirement bytes of its images
            // #614: blocks holding no image (VMA's hysteresis), and what a perfect packing of
            // the pool's suballocations would free (non-empty blocks beyond the minimum).
            uint32_t emptyBlocks = 0;
            uint64_t reclaimBytes = 0;
        };
        bool     texPools = false;            // VMA texture pools active
        uint32_t texPoolCount = 0;
        TexPool  texPool[kTexPoolMax] = {};
        uint64_t texSmallMax = 0, texMediumMax = 0;   // class bounds (small = granularity)
        // #587: images above the small tier (<= texMediumMax) that no medium class took
        // (rounding waste > 1/8 or alignment) and went to the TLSF fallback pool, and the
        // largest memory-requirement alignment seen above the small tier (monotonic).
        uint64_t texClassMiss = 0, texClassHit = 0;
        uint64_t texMedAlignMax = 0;
        uint32_t texPoolBlocks = 0, texPooledImages = 0;
        uint64_t texPoolBlockBytes = 0, texPoolRounded = 0, texPoolLive = 0, texPoolSlack = 0;
        uint32_t texDedicatedWanted = 0;      // poolable images the driver wants dedicated (monotonic)
        // Monotonic (hitch comparison, #411): vkAllocateMemory / vkFreeMemory calls for
        // texture + UI images (pool blocks and dedicated images), and pool suballocations
        // / frees (free-list operations).
        uint64_t texDevAllocCalls = 0, texDevFreeCalls = 0, texSubAllocCalls = 0, texSubFreeCalls = 0;
        // #614 TEXTURE POOL COMPACTION: reclaimable bytes over all pools (TexPool::reclaimBytes),
        // empty blocks VMA keeps, and the compaction counters (monotonic): images moved + their
        // bytes, sparse blocks skipped because an image was busy, source blocks fully queued
        // for evacuation, destinations refused (no room outside the sources), and empty
        // blocks released early (vmaReleasePoolEmptyBlocks). A move also counts as one
        // suballocation + one free in texSub*Calls (texCompactMoves tells them apart).
        uint64_t texPoolReclaim = 0;
        uint32_t texPoolEmptyBlocks = 0;
        uint64_t texPoolEmptyBytes = 0;
        uint64_t texCompactMoves = 0, texCompactMoved = 0, texCompactSkipped = 0, texCompactEvacuated = 0;
        uint64_t texCompactPlaceFail = 0, texCompactReleased = 0, texCompactReleasedBytes = 0;
        // #458 HOST-VISIBLE memory (HOST_VISIBLE memory types on any heap: ReBAR / BAR
        // VRAM included, unlike catHost which is "not a device-local heap"): live bytes and
        // allocation counts per category. And allocation TRAFFIC through createBuffer /
        // createImage* (VMA texture pool blocks excluded): monotonic vkAllocateMemory /
        // vkFreeMemory calls and bytes by kind ([0] device-only, [1] host-visible) and
        // calls per category -- diff two snapshots for a rate.
        uint64_t catHV[VKMEM_COUNT] = {};
        uint32_t catHVCount[VKMEM_COUNT] = {};
        uint32_t hostVisibleCount = 0;
        uint64_t allocCalls[2] = {}, freeCalls[2] = {}, allocBytesTotal[2] = {}, freeBytesTotal[2] = {};
        uint64_t catAllocCalls[VKMEM_COUNT] = {}, catFreeCalls[VKMEM_COUNT] = {};
    };
    MemStats memStats() const;
    // Restart the per-category interval peaks (MemStats::catPeakInt) at the live bytes.
    void resetCatPeakInterval();
    // #407: the largest live INFERRED allocation sites (calling function, MB, count,
    // the inferred category), e.g. "ensureFoo 120MB/4 targets, ..."; "" = none.
    std::string inferredSitesText(size_t top = 6) const;
    // World texture images (VKMEM_TEXTURE) by allocation-size band: count / MB / 64K-granularity
    // padding, pooled vs dedicated -- sizes the texture pool limit (RenderVKTexturePoolMediumMaxKB).
    std::string texSizeHistogramText() const;
    // The largest device-local heap's budget (VK_EXT_memory_budget, else its size); on UMA
    // (#695) the combined budget of budgetHeaps.
    uint64_t deviceLocalBudgetBytes() const;
    // #695 THE HEAP SET THE VRAM BUDGET MEASURES -- one definition for every caller (each
    // used to carry its own copy of the loop). Not UMA: exactly the old rule, the
    // device-local heap with the largest budget (heap ~0u / budget 0 when there is none;
    // NOT the highest usage/budget ratio -- the NVIDIA non-ReBAR 256 MB BAR heap). UMA: the
    // carve-out heap + the host heap, budget = carve-out budget + min(host budget, ceiling).
    struct BudgetHeaps
    {
        bool     uma = false;
        uint32_t heap = ~0u;       // the device-local (main / carve-out) heap
        uint32_t hostHeap = ~0u;   // UMA: the shared system-memory heap
        uint64_t budget = 0;       // what the VRAM budget measures against
        uint64_t devBudget = 0, hostBudget = 0;   // UMA: the two parts (host part already capped)
    };
    static BudgetHeaps budgetHeaps(const MemStats& m);
    bool uma() const { return mUma; }
    // #695: one line for the log: device type, every heap (size / flags / budget / usage)
    // and the budget mode chosen (UMA or the main device-local heap).
    std::string memLayoutText() const;
    // The physical device's limits (maxStorageBufferRange, maxComputeWorkGroupCount...).
    const VkPhysicalDeviceLimits& limits() const { return mLimits; }
    // Our tracked live bytes on one heap (cheap: no driver query).
    uint64_t heapUsedBytes(uint32_t heap) const;
    // SOFT ALLOCATION (VRAM stopgap): while enabled, createImage2D/createBuffer treat
    // VK_ERROR_OUT_OF_DEVICE_MEMORY / OUT_OF_HOST_MEMORY as a recoverable failure --
    // partial objects are destroyed and an EMPTY GpuImage/GpuBuffer is returned (the
    // caller must check .image/.buffer). Off by default: critical allocations keep
    // VK_CHECK abort semantics. allocFailures() counts soft failures (monotonic).
    // #697: the mode is PER THREAD (thread_local in vk_device.cpp, like VkMemScope's
    // category): it was one shared non-atomic bool, so a worker allocating while the main
    // thread had it on (or off) got the other thread's semantics. Prefer SoftAllocScope,
    // which restores the previous value (a bare set(false) cancelled an enclosing scope).
    void setSoftAllocFail(bool on);
    bool softAllocFail() const;
    uint32_t allocFailures() const { return mAllocFailures; }
    // Size of the latest soft-failed request (#563: the VRAM ladder answers a failure until
    // that much would fit again). Pre-flight refusals count as failures too.
    uint64_t lastFailBytes() const { return mLastFailBytes; }
    // HEAP BUDGET PRE-FLIGHT (#563). NVIDIA segfaults inside vkAllocateMemory when a large
    // request pushes the device-local heap past its VK_EXT_memory_budget budget instead of
    // returning OUT_OF_DEVICE_MEMORY. Every device-local buffer / non-pooled image request
    // >= 64 MB is checked against the live heap budget first: over it, the relief hook
    // (the renderer: device idle + free what earlier frames retired) runs once; still over,
    // a soft request fails softly, a hard one proceeds with a warning (it would abort
    // either way; the log then names it). The hook runs only on the thread that set it.
    void setPressureRelief(std::function<uint64_t()> fn);
    // RUNTIME OUT-OF-MEMORY POLICY (#595 #597 #599). Every vkAllocateMemory of
    // createBuffer / createImage* that returns OUT_OF_DEVICE / HOST_MEMORY runs the relief
    // hook (the renderer: device idle + free retired / idle pools, then the VRAM ladder's
    // fast step) and retries ONCE before the soft / hard decision. A soft request >= 4 MB
    // that would take the device-local heap past its CURRENT OS budget (VK_EXT_memory_budget;
    // AMD on Windows refuses over-commit there) is refused up front (relief first). Every
    // soft failure is counted per site ("<category>/<call>") and logged through the log
    // hook (one WARNING per site per 10 s, with size / budget / usage).
    void setAllocLog(std::function<void(const std::string&)> fn) { mAllocLog = std::move(fn); }
    // After the renderer's init: allocations from here on are "runtime" (fault injection).
    void setRuntimeAllocs(bool on) { mRuntimeAllocs = on; }
    // Debug (SLVkInjectAllocFail N, SLVkInjectAllocFailHard): every Nth runtime allocation
    // fails with OUT_OF_DEVICE_MEMORY (0 = off). Soft requests only unless `hard` -- a hard
    // injected failure aborts, which is the test that no hard runtime site is left.
    void setInjectAllocFail(uint32_t every, bool hard) { mInjectEvery = every; mInjectHard = hard; }
    // "N (sites geom/createBuffer x3, probes/createImageLayers x1)" ("0" when none).
    std::string softFailSummary() const;
    // Live driver numbers for the largest device-local heap (VK_EXT_memory_budget; false
    // without it): the process usage and the OS budget right now.
    bool liveDeviceBudget(uint64_t& usage, uint64_t& budget) const;
    // Tracked size of one allocation (0 if unknown, and 0 for a texture pool block: its
    // images are sized by the image overloads below).
    uint64_t allocBytes(VkDeviceMemory mem) const;
    // An image's bytes: its suballocation (memory requirement size) when pooled (#411),
    // else its own allocation's size.
    uint64_t allocBytes(VkImage image, VkDeviceMemory mem) const;
    uint64_t allocBytes(const GpuImage& img) const { return allocBytes(img.image, img.memory); }
    // The image is suballocated from a texture pool (#411): no page padding of its own.
    bool imagePooled(VkImage image) const;
    // e.g. "on: 11 slab classes 2-64 KiB (4 MiB blocks), medium <= 256 KiB (8 MiB blocks)".
    const std::string& texturePoolStatus() const { return mTexPoolStatus; }
    // VMA device-memory callbacks (vkAllocateMemory / vkFreeMemory of a pool block).
    void onVmaBlock(bool alloc, uint32_t memType, VkDeviceMemory mem, uint64_t bytes);

    // ---- TEXTURE POOL COMPACTION (#614) ----
    // Pool blocks free only when their last image goes; unloads and mip-downs leave sparse
    // blocks behind (an 8 GB card: 345 MB slack, 257 MB of it stranded in the medium
    // classes). The RENDERER moves images out of the emptiest blocks: a fresh image placed
    // in a fuller block (TexCompactScope), a GPU copy of every mip, the bindless views
    // repointed and the old image retired -- freed with the frames that may still sample
    // it. This layer picks the blocks and steers the placement; it never copies.
    struct TexCompactPick
    {
        std::vector<VkImage> images;          // to move, block by block (emptiest block first)
        std::vector<VkDeviceMemory> blocks;   // their source blocks (destinations must avoid them)
        std::vector<uint32_t> blockOf;        // per image: its index in `blocks`
        uint64_t bytes = 0;                   // the images' suballocation bytes
        uint32_t busyBlocks = 0;              // sparse blocks skipped: an image busy this pass
        uint32_t pinnedBlocks = 0;            // ... holding an image the caller cannot move
    };
    // Pick whole source blocks to evacuate: per pool, at most the blocks a perfect packing
    // frees (so the rest has room for their images), its emptiest ones, each at most
    // maxOccupancy full (live bytes / block); across pools the cheapest per byte freed
    // first (bytes copied + images moved); about maxBytes of images (at least one block).
    // movable(image): 1 = the caller can move it now; 2 = busy this pass (skip the block);
    // 0 = not the caller's (UI, media, trial candidates, retired) -- the block can never
    // empty. `avoid`: blocks still being evacuated (not picked again). Never calls VMA.
    // keepBlocks (background): per pool, that many blocks of the reclaimable slack stay
    // where they are -- the pool's allocation headroom; evacuating them would only make
    // the next allocations open a fresh (sparse) block that the next plan evacuates again.
    // minIdleSec (background): a block that received an allocation (a new image, or a
    // compaction destination) less than that long ago is not a source -- it is filling.
    void texCompactPlan(uint64_t maxBytes, float maxOccupancy, const std::vector<VkDeviceMemory>& avoid,
                        const std::function<int(VkImage)>& movable, TexCompactPick& out,
                        uint32_t keepBlocks = 0, double minIdleSec = 0.0) const;
    // Sum over pools of TexPool::reclaimBytes (cheap: the per-block table, no VMA call);
    // keepBlocks: per pool, that many blocks of slack are not counted (texCompactPlan).
    uint64_t texPoolReclaimable(uint32_t keepBlocks = 0) const;
    // The texture-pool block a pooled image lives in (VK_NULL_HANDLE: not pooled).
    VkDeviceMemory texPoolBlockOf(VkImage image) const;
    // While alive, createImage2D on THIS thread places a pooled image only in an existing
    // block that already holds an image and is not in `avoid` -- no new block, no
    // dedicated fallback: anything else returns an empty GpuImage (not a soft failure).
    struct TexCompactScope
    {
        explicit TexCompactScope(const std::vector<VkDeviceMemory>* avoid);
        ~TexCompactScope();
        TexCompactScope(const TexCompactScope&) = delete;
        TexCompactScope& operator=(const TexCompactScope&) = delete;
    private:
        const std::vector<VkDeviceMemory>* mPrev;
    };
    // Free the empty blocks VMA keeps (one per pool, its hysteresis) now. Nothing is bound
    // to them: images are destroyed only after the frames that use them. Bytes freed.
    uint64_t texPoolReleaseEmpty();
    // Renderer-side counters for MemStats (moved images / bytes, busy skips, evacuations).
    void noteTexCompact(uint32_t moves, uint64_t bytes, uint32_t skippedBusy, uint32_t evacuated);
    // The allocation lives on a device-local heap (false if unknown). Slack split (#407).
    bool allocDeviceLocal(VkDeviceMemory mem) const;
    // Scoped soft allocation (#226): soft while alive, the previous mode restored after.
    // Keep the scope tight. (#697: per thread -- a scope on the main thread never changes
    // a worker's allocations.)
    struct SoftAllocScope
    {
        VkDeviceContext* ctx; bool prev;
        // on = false: no change (a hard allocation in the same code path).
        explicit SoftAllocScope(VkDeviceContext* c, bool on = true) : ctx(on ? c : nullptr),
            prev(c ? c->softAllocFail() : false)
        { if (ctx) ctx->setSoftAllocFail(true); }
        ~SoftAllocScope() { if (ctx) ctx->setSoftAllocFail(prev); }
        SoftAllocScope(const SoftAllocScope&) = delete;
        SoftAllocScope& operator=(const SoftAllocScope&) = delete;
    };
    // Exact device memory an image created by createImage2D (layers 1, cube false) or
    // createImageLayers would take, without creating it (vkGetDeviceImageMemoryRequirements,
    // same create info incl. sharing). Pre-checks for setting-driven allocations (#226).
    uint64_t imageMemoryBytes(uint32_t w, uint32_t h, uint32_t layers, uint32_t mipLevels,
                              VkFormat format, VkImageUsageFlags usage, bool cube_compatible,
                              const struct VkImageExtra* extra = nullptr) const;

    // Layered / cube image: `layers` array layers, `mipLevels` mips; cube_compatible
    // sets VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT (layers a multiple of 6). The default
    // view has `viewType` over all layers + mips (e.g. CUBE_ARRAY for sampling).
    GpuImage createImageLayers(uint32_t w, uint32_t h, uint32_t layers, uint32_t mipLevels,
                               VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, bool cube_compatible,
                               VkImageViewType viewType, const char* site = VK_MEM_SITE,
                               const struct VkImageExtra* extra = nullptr);
    // Extra view over a sub-range of an image (caller destroys it). viewUsage != 0 chains
    // VkImageViewUsageCreateInfo: required for a view of a MUTABLE_FORMAT | EXTENDED_USAGE
    // image whose format lacks some of the image's usage (e.g. STORAGE on E5B9G9R9).
    VkImageView createView(VkImage image, VkFormat format, VkImageViewType type,
                           VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                           uint32_t baseLayer, uint32_t layerCount, VkImageUsageFlags viewUsage = 0);
    // #697: as createView, but out of memory (or SLVkInjectOomCalls) returns VK_NULL_HANDLE
    // (counted as a soft failure) instead of aborting. The caller skips the resource.
    VkImageView createViewSoft(VkImage image, VkFormat format, VkImageViewType type,
                               VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                               uint32_t baseLayer, uint32_t layerCount, VkImageUsageFlags viewUsage = 0);

    // ---- ALIASED TRANSIENT TARGETS (#615; the policy is VkAliasArena below) ----
    // An UNBOUND 2D image (1 mip, 1 layer, sharing as createImage2D) and its memory
    // requirements; null on failure (never aborts). `dedicated`: the driver REQUIRES a
    // dedicated allocation -- it cannot share memory. No VK_IMAGE_CREATE_ALIAS_BIT: that
    // bit is for aliases that must read each other's contents; ours never do (every first
    // use in a frame discards: oldLayout UNDEFINED).
    VkImage createAliasImage2D(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                               VkMemoryRequirements& req, bool& dedicated);
    // A shared device-local block (a memory type in `typeBits`). Always soft: null on
    // failure or when `typeBits` has no device-local type. Tracked under the calling
    // thread's VkMemScope; holds one reference (the caller's: releaseAliasBlock).
    VkDeviceMemory allocAliasBlock(VkDeviceSize bytes, uint32_t typeBits, uint32_t& typeIndex);
    // Binds `image` at `offset` in `block`: +1 reference on the block, dropped when
    // destroyImage destroys the image (the GpuImage keeps memory = VK_NULL_HANDLE, so
    // nothing frees the block through it). False: not bound (the caller destroys it).
    bool bindAliasImage(VkImage image, VkDeviceMemory block, VkDeviceSize offset, VkDeviceSize bytes,
                        uint32_t w, uint32_t h, VkFormat format, const void* caller);
    // Drops one reference; the block is freed with its last one (its images all destroyed).
    void releaseAliasBlock(VkDeviceMemory block);
    // The block a live aliased image is bound to (null: not aliased / destroyed).
    VkDeviceMemory aliasBlockOf(VkImage image) const;
    // Live alias blocks: count and bytes (current + retired-but-referenced).
    void aliasBlockStats(uint32_t& blocks, uint64_t& bytes) const;

    // Upload host data into a device-local buffer via a staging buffer.
    GpuBuffer createDeviceLocalWithData(const void* data, VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        bool deviceAddress = false, const char* site = VK_MEM_SITE);

    // Validation bookkeeping (public so main() can report).
    uint32_t validationErrorCount() const { return mValidationErrors; }
    uint32_t validationWarnCount() const { return mValidationWarnings; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    void trackAlloc(VkDeviceMemory mem, VkDeviceSize bytes, uint32_t typeIndex, bool image,
                    uint32_t usage, bool asStorage = false, const char* site = nullptr);
    void trackFree(VkDeviceMemory mem);
    void measureAllocGranularity();   // #407: one-time, right after vkCreateDevice
    // ---- TEXTURE SUBALLOCATION (#411) ----
    void initTexturePools();          // after measureAllocGranularity (classes follow it)
    void destroyTexturePools();       // shutdown, before vkDestroyDevice
    int  texPoolFor(uint64_t size, uint64_t alignment) const;   // pool index, -1 = dedicated
    // Suballocate + bind `img.image` (req = its requirements); false = use a dedicated
    // allocation (not poolable, pool full / out of memory).
    bool allocPooledImage(GpuImage& img, const VkMemoryRequirements& req, uint8_t cat, uint32_t w, uint32_t h,
                          VkFormat fmt, const void* caller);
    VmaAllocation releasePooled(VkImage image);   // drop the record + stats; caller frees
    // #614: bytes a perfect packing of `images` / `used` would free from `blocks` non-empty
    // blocks of pool `pool` (slab: by slots; fallback: by bytes).
    uint64_t texPoolReclaimOf(uint32_t pool, uint32_t blocks, uint32_t images, uint64_t used) const;
    VmaAllocator mVma = nullptr;
    struct TexPoolDef { VmaPool pool = nullptr; uint32_t classBytes = 0; uint64_t blockSize = 0; uint32_t slots = 0; };
    std::vector<TexPoolDef> mTexPools;   // slab classes ascending, then the medium pool
    uint32_t mTexPoolType = ~0u;         // the device-local type the pools live in
    bool     mTexPoolDev = true;         // ... on a device-local heap
    uint64_t mTexSmallMax = 0, mTexMediumMax = 0;
    std::string mTexPoolStatus = "off";
    // block / sub: the VMA block (its VkDeviceMemory) and the suballocation's size (the slab
    // class, or the request in the fallback pool) -- #614 compaction.
    struct PooledRec { VmaAllocation alloc; uint64_t bytes; uint32_t pool; uint8_t cat;
                       VkDeviceMemory block = VK_NULL_HANDLE; uint64_t sub = 0; };
    std::unordered_map<VkImage, PooledRec> mPooled;   // live pooled images (under mAllocMutex)
    // #614: every NON-EMPTY pool block -> its pool, image count, suballocated bytes (under
    // mAllocMutex; an entry goes when its last image does -- empty blocks are VMA's).
    // last_alloc: steady-clock seconds of the block's latest suballocation (#614: a block
    // that is filling is no compaction source).
    struct TexBlockRec { uint32_t pool = 0; uint32_t images = 0; uint64_t used = 0; double last_alloc = 0.0; };
    std::unordered_map<VkDeviceMemory, TexBlockRec> mTexBlocks;
    uint64_t mTexCompactMoves = 0, mTexCompactMoved = 0, mTexCompactSkipped = 0, mTexCompactEvacuated = 0;
    uint64_t mTexCompactPlaceFail = 0, mTexCompactReleased = 0, mTexCompactReleasedBytes = 0;
    uint64_t mTexPoolLive[MemStats::kTexPoolMax] = {};
    uint64_t mTexPoolLiveTotal = 0, mTexPoolBlockBytes = 0;
    uint32_t mTexPoolBlocks = 0, mTexDedicatedWanted = 0;
    uint64_t mTexClassMiss = 0, mTexClassHit = 0, mTexMedAlignMax = 0;   // #587 (under mAllocMutex)
    uint64_t mTexDevAllocCalls = 0, mTexDevFreeCalls = 0, mTexSubAllocCalls = 0, mTexSubFreeCalls = 0;
    // ---- ALIASED TRANSIENT TARGETS (#615) ---- (both under mAllocMutex)
    struct AliasBlockRec { uint32_t refs = 0; uint64_t va = 0; uint64_t bytes = 0; };
    std::unordered_map<VkDeviceMemory, AliasBlockRec> mAliasBlocks;
    std::unordered_map<VkImage, VkDeviceMemory> mAliasImages;   // bound image -> its block
    void aliasUnref(VkDeviceMemory block);   // (not under mAllocMutex: may free)
    // #526: each returns false (reason in mReqReport) instead of aborting.
    bool createInstance(bool enableValidation,
                        const std::vector<const char*>& extraInstanceExts);
    bool pickPhysicalDevice(bool wantSwapchain);
    bool createLogicalDevice(bool wantSwapchain);
    VkRequirementsReport mReqReport;
    bool mApi12 = false;     // #675 (see usesApi12)
    bool mMaint4 = false;    // #675: VK_KHR_maintenance4 enabled on the 1.2 path
    void createPipelineCache();
    void destroyPipelineCache();

    VkInstance mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mMemProps{};
    VkPhysicalDeviceLimits mLimits{};
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    uint32_t mQueueFamily = 0;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkPipelineCache mPipeCache = VK_NULL_HANDLE;
    std::string mPipeCachePath, mPipeCacheTag;
    size_t mPipeCacheBytes = 0;   // payload size at the last load / save (dirty check)
    bool mPipeCacheWarm = false;  // seeded from the disk file at init
    // Async compute (phase 3a): chosen in pickPhysicalDevice, created with the device.
    uint32_t mAsyncMode = 0;
    bool     mAsyncAuto = false;   // #845 (setAsyncComputeAuto)
    uint32_t mComputeFamily = ~0u;   // ~0u: no async queue
    uint32_t mComputeQueueIndex = 0; // 1 when it shares the graphics family
    bool mComputeOnly = false;
    uint32_t mComputeTsBits = 0;
    VkQueue mComputeQueue = VK_NULL_HANDLE;
    uint32_t mEncodeFamily = ~0u;              // <GMDX> instant replay
    VkQueue mEncodeQueue = VK_NULL_HANDLE;     // <GMDX>
    std::string mAsyncStatus = "off";
    std::string mAsyncDecision = "not decided";
    // RenderVKAsyncCompute -1 (auto): the per-architecture default; `reason` is the log text.
    bool asyncAutoDecide(const VkPhysicalDeviceProperties& p, std::string& reason) const;
    // CONCURRENT sharing for every buffer / image (dedicated compute family only).
    bool mConcurrent = false;
    uint32_t mShareFamilies[2] = {0, 0};
    void applySharing(VkSharingMode& mode, uint32_t& count, const uint32_t*& families, bool image,
                      VkImageUsageFlags usage) const;
    bool mValidation = false;
    bool mDepthClamp = false;
    bool mDepthBiasClamp = false;   // <GMDX>
    bool mFragStores = false;   // fragmentStoresAndAtomics enabled
    bool mPipeRobust = false;   // VK_EXT_pipeline_robustness enabled (#546)
    bool mDebugRobustReq = false;   // RenderVKDebugRobustness (setDebugRobustness)
    std::vector<std::string> mOptionalExtsRequested;   // #919 requestOptionalExtension
    std::vector<std::string> mOptionalExtsEnabled;     // the requested ones the device listed
    LateExtProvider mLateExtProvider;                  // #870 setLateExtensionProvider
    std::vector<std::string> mLateExts;                // its list (enabled when mLateExtsOn)
    bool mLateExtsOn = false;
    std::string mLateExtsWhy = "no provider";
    bool mDebugRobustOn = false;    // device created with robustBufferAccess (+ robustness2 where supported)
    std::string mDebugRobustStatus = "off";
    VkPipelineRobustnessBufferBehaviorEXT mRobustBuffer = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
    VkPipelineRobustnessImageBehaviorEXT mRobustImage = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
    bool mStorageExtFormats = false;
    bool mTexCompBC = false;
    bool mWgScanFullSubgroups = false;    // see wgScanFullSubgroups()
    bool mComputeFullSubgroups = false;
    uint32_t mSubgroupSize = 0, mSubgroupSizeMin = 0, mSubgroupSizeMax = 0, mSubgroupOps = 0;
    bool mStorageImgArrayDynIdx = false;   // shaderStorageImageArrayDynamicIndexing (#565)
    bool mWideLines = false;
    float mLineWidthMax = 1.0f;
    bool mRayQuerySupported = false;
    bool mRayTracingRequested = false;
    bool mClusterRequested = false;
    uint32_t mClusterDeviceFeatures = 0;   // RenderVKClusterDeviceFeatures (the viewer sets it)
    bool mRayTracingEnabled = false;
    bool mOmmEnabled = false;
    bool mVrsEnabled = false;
    bool mVrsImageEnabled = false;   // + the rate attachment (VRS tier 2)
    bool mFragBaryEnabled = false;   // VK_KHR_fragment_shader_barycentric (#664)
    bool mPipelineStatsRequested = false;   // RenderVKPipelineStats (set before init)
    bool mPipelineStatsEnabled = false;     // VK_KHR_pipeline_executable_properties enabled
    bool mShaderClockRequested = false;     // RenderVKShaderClockLighting (set before init)
    bool mShaderClockEnabled = false;       // VK_KHR_shader_clock (shaderSubgroupClock) enabled

    VkClusterFeatures mCluster;      // #743 cluster LOD extensions (clusterFeatures())
    uint32_t mVrsTexel = 0;          // its texel size (px, square)
    PFN_vkCmdSetFragmentShadingRateKHR mCmdSetShadingRate = nullptr;
    uint32_t mOmmMaxLevel4 = 0;
    std::string mRTStatus = "not initialized";
    VkDeviceSize mAsScratchAlign = 256;
    RTFuncs mRT;
    bool mMemBudgetExt = false;
    struct AllocRec { uint64_t bytes; uint32_t heap; bool image; bool host; uint8_t cat; bool inferred; bool dev;
                      uint32_t pad; const char* site;
                      bool pool = false;          // a texture pool block (#411): no category bytes
                      uint64_t va = 0; };         // its GPU VA (pool blocks; image registry)
    struct SiteRec { uint64_t bytes = 0; uint32_t count = 0; uint8_t cat = 0; };
    std::unordered_map<const char*, SiteRec> mInferredSites;   // live inferred bytes per call site (#407)
    // shutdown() resets via `*this = VkDeviceContext{}`: a copy/move of the
    // context must not try to copy the mutex, so wrap it in a no-op-copy holder.
    struct AllocMutex { std::mutex m; AllocMutex() = default; AllocMutex(const AllocMutex&) {}
                        AllocMutex& operator=(const AllocMutex&) { return *this; } };
    mutable AllocMutex mAllocMutex;
    uint32_t mAllocFailures = 0;
    bool softFail(VkResult r, const char* what, uint64_t bytes);
    uint64_t mLastFailBytes = 0;
    std::function<uint64_t()> mRelief;
    std::thread::id mReliefThread{};
    bool mInRelief = false;
    double mLastHardOverLog = -1.0e9;
    double mLastRelief = -1.0e9;
    // false = refuse (soft mode: softFail already counted); true = go ahead.
    bool heapBudgetPreflight(uint32_t memTypeIndex, uint64_t bytes, const char* what);
    // ---- #695 UMA (integrated GPU with a BIOS carve-out; VkUmaConfig) ----
    // detectUma: at device selection (memory properties + device type known). Every
    // helper below is the old behaviour when mUma is false.
    void detectUma(VkPhysicalDeviceType type);
    // The heap counts toward the VRAM budget: DEVICE_LOCAL (not UMA), carve-out or host heap (UMA).
    bool inBudgetHeap(uint32_t heap) const;
    // A type on the UMA host heap in typeBits with every flag of props (DEVICE_LOCAL
    // dropped), preferring uncached (write-combined: the GPU reads it without snooping),
    // never the AMD device-coherent / uncached debug types; ~0u = none.
    uint32_t umaHostType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    // The memory type for a DEVICE_LOCAL request: findMemoryType(typeBits, props) -- on
    // UMA the host-heap type instead when the carve-out has no room for `bytes` (spill).
    uint32_t pickDeviceLocalType(uint32_t typeBits, VkMemoryPropertyFlags props, uint64_t bytes);
    // UMA: `bytes` fit the carve-out's budget (live driver numbers, refreshed at most every
    // 250 ms, + our allocations since; the pre-flight's margins).
    bool umaCarveFits(uint64_t bytes);
    // allocateMemory, and on UMA one more try on the host heap when the carve-out type
    // ran out of memory (the driver's budget can be below what our snapshot said).
    VkResult allocateMemoryUma(VkMemoryAllocateInfo& ai, VkDeviceMemory* mem, const char* what, uint32_t typeBits,
                               VkMemoryPropertyFlags props);
    bool     mUma = false, mUmaForced = false;
    uint32_t mUmaDevHeap = ~0u, mUmaHostHeap = ~0u;
    uint64_t mUmaHostCeil = 0, mUmaFakeCarve = 0;
    VkPhysicalDeviceType mDeviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    mutable AllocMutex mUmaMutex;   // the carve-out snapshot + spill counters below
    double   mUmaSnapAt = -1.0e9;
    uint64_t mUmaSnapUsage = 0, mUmaSnapBudget = 0, mUmaSnapTracked = 0;
    uint64_t mUmaSpills = 0, mUmaSpillBytes = 0;
    // #595: vkAllocateMemory + fault injection + relief and one retry on out-of-memory.
    VkResult allocateMemory(const VkMemoryAllocateInfo& ai, VkDeviceMemory* mem, const char* what);
    // Runs the relief hook if allowed here (its thread, not re-entered, >= min_interval s
    // since the last run). True when it ran.
    bool runRelief(double min_interval, uint64_t& freed);
    void allocLog(const std::string& s) const;
    std::function<void(const std::string&)> mAllocLog;
    bool mRuntimeAllocs = false;
    uint32_t mInjectEvery = 0;
    bool mInjectHard = false;
    uint64_t mInjectCount = 0;
    struct SoftSite { uint32_t count = 0; uint32_t since = 0; uint64_t lastBytes = 0; double lastLog = -1.0e9; };
    std::map<std::string, SoftSite> mSoftSites;   // (under mAllocMutex)
    double mLastOomRetryLog = -1.0e9;
    std::unordered_map<VkDeviceMemory, AllocRec> mAllocs;
    uint64_t mHeapUsed[VK_MAX_MEMORY_HEAPS] = {};
    uint64_t mBufBytes = 0, mImgBytes = 0, mHostBytes = 0;
    // #458 (see MemStats::catHV / allocCalls)
    uint64_t mCatHV[VKMEM_COUNT] = {};
    uint32_t mCatHVCount[VKMEM_COUNT] = {};
    uint32_t mHostCount = 0;
    uint64_t mAllocCalls[2] = {}, mFreeCalls[2] = {}, mAllocBytesTotal[2] = {}, mFreeBytesTotal[2] = {};
    uint64_t mCatAllocCalls[VKMEM_COUNT] = {}, mCatFreeCalls[VKMEM_COUNT] = {};
    uint64_t mCatDev[VKMEM_COUNT] = {}, mCatHost[VKMEM_COUNT] = {}, mCatPeak[VKMEM_COUNT] = {};
    uint64_t mCatPeakInt[VKMEM_COUNT] = {};   // since resetCatPeakInterval()
    uint32_t mCatCount[VKMEM_COUNT] = {};
    uint64_t mInferredBytes = 0, mDevLocalBytes = 0, mDevLocalPeak = 0;
    uint64_t mCatPad[VKMEM_COUNT] = {}, mPadBytes = 0;
    uint64_t mAllocGran = 64ull << 10;   // driver allocation granularity (#407)
    bool     mAllocGranMeasured = false;
    // VK_EXT_device_memory_report (#407): live driver memory objects (id -> size / heap /
    // bucket; FREE events carry no size) and the per heap x bucket sums. Own mutex: the
    // callback runs inside vkAllocateMemory & co. on any thread, before trackAlloc.
    struct DrvRec { uint64_t bytes; uint32_t heap; uint8_t bucket; bool import; };
    bool     mDrvReport = false;
    mutable AllocMutex mDrvMutex;
    std::unordered_map<uint64_t, DrvRec> mDrvObjs;
    uint64_t mDrvHeap[VK_MAX_MEMORY_HEAPS][VKDRV_COUNT] = {};
    uint64_t mDrvImport[VK_MAX_MEMORY_HEAPS] = {};
    uint32_t mDrvFailures = 0;

    uint32_t mValidationErrors = 0;
    uint32_t mValidationWarnings = 0;

public:
    // called by the static debug callback
    void onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                        const char* msg);
    // called by the static device memory report callback (#407); never calls Vulkan
    void onDriverMemoryReport(const VkDeviceMemoryReportCallbackDataEXT& d);
};

// ---- ALIASED TRANSIENT TARGETS (#615) ----
// One shared device-memory block for render targets whose lifetimes never overlap. Each
// slot belongs to a PHASE: the slots of one phase get disjoint regions (they may be alive
// at once), and every phase starts at offset 0 -- block = the largest phase's packed size.
// The OWNER guarantees, for every frame on every queue: no image of one phase is accessed
// between another phase's first and last access; every image's first access in a frame
// starts from VK_IMAGE_LAYOUT_UNDEFINED behind a barrier whose source covers the other
// phases' stages / writes; nothing reads an aliased image's contents in a later frame.
// Images can't be re-bound, so the block never grows in place. A set that does not fit
// (bigger size, a slot without a region, another memory type) or a block far bigger than
// what its members need gets a NEW block, planned from that set + every other slot's last
// requirement (live, or dead and not forget()-ten);
// the old block stays alive (each image holds a reference, destroyImage drops it) until
// its last image is destroyed, and epoch() moves on so the owners of images left on it
// re-create them at their next ensure (the new block reserves their regions).
class VkAliasArena
{
public:
    static constexpr uint32_t kMaxSlots = 24;
    struct Desc
    {
        uint32_t slot = 0;
        uint32_t w = 0, h = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    };
    // `phase[s]` = slot s's phase (< 8). Once, before any create.
    void setup(const char* name, const uint8_t* phase, uint32_t slotCount);
    // RenderVKAliasTargets. A change moves the epoch (every owner re-creates its set);
    // off drops the arena's reference on its block (freed once its images are re-created).
    void setEnabled(VkDeviceContext& ctx, bool on);
    bool enabled() const { return mEnabled; }
    // The n images of one owner's set (created together, all alive at once or not at
    // all), bound into the block -- all or nothing. False: nothing was created; the caller
    // creates them the plain way (createImage2D). The images' memory is VK_NULL_HANDLE.
    bool create(VkDeviceContext& ctx, const Desc* d, uint32_t n, GpuImage* out);
    // Moves whenever an image may sit on a retired block or the setting changed: an owner
    // whose set was created at another epoch re-creates it (stale).
    uint64_t epoch() const { return mEpoch; }
    // Per frame: drop the block once no live image uses it.
    void trim(VkDeviceContext& ctx);
    // The slots' images were freed for good (a feature turned off): stop reserving their
    // last requirements in the next block's plan. Call after destroying the images.
    void forget(uint32_t first, uint32_t count);
    void shutdown(VkDeviceContext& ctx);
    // Live images on the current block, their summed requirements, the block's size and
    // how many phases hold a live image (the "N MB -> M MB" log).
    void stats(const VkDeviceContext& ctx, uint32_t& images, uint64_t& memberBytes, uint64_t& blockBytes,
               uint32_t& phases) const;
    // The last create()'s fallback reason ("" = none), for a one-line log.
    const char* lastFail() const { return mLastFail; }

private:
    struct Slot
    {
        VkImage image = VK_NULL_HANDLE;   // the last image created for the slot (may be dead)
        VkMemoryRequirements req{};
        VkDeviceSize off = 0, region = 0; // its region in the current block (region 0 = none)
    };
    const char* mName = "alias";
    uint8_t  mPhase[kMaxSlots] = {};
    uint32_t mSlotCount = 0;
    Slot     mSlots[kMaxSlots];
    VkDeviceMemory mBlock = VK_NULL_HANDLE;
    VkDeviceSize   mBlockSize = 0;
    uint32_t       mBlockType = 0;
    uint64_t       mEpoch = 1;
    bool           mEnabled = true;
    const char*    mLastFail = "";
    bool live(const VkDeviceContext& ctx, uint32_t s) const;
    void dropBlock(VkDeviceContext& ctx);
};
