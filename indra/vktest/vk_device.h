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

#include <functional>

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;          // non-null if host-visible + mapped
    VkDeviceAddress address = 0;     // non-zero if created with device-address
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
};

class VkDeviceContext {
public:
    // enableValidation turns on the Khronos validation layer + a debug
    // messenger that routes messages through onDebugMessage(). Sync validation
    // is requested via VK_EXT_layer_settings when available.
    bool init(bool enableValidation);
    void shutdown();

    // ---- accessors ----
    VkInstance instance() const { return mInstance; }
    VkPhysicalDevice physicalDevice() const { return mPhysical; }
    VkDevice device() const { return mDevice; }
    VkQueue queue() const { return mQueue; }
    uint32_t queueFamily() const { return mQueueFamily; }
    VkCommandPool commandPool() const { return mCommandPool; }

    // ---- one-shot command submission (blocking) ----
    // Records via the callback into a primary command buffer, submits, waits.
    void submitImmediate(const std::function<void(VkCommandBuffer)>& record);

    // ---- resource creation ----
    GpuBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props,
                           bool deviceAddress = false);
    void destroyBuffer(GpuBuffer& b);

    GpuImage createImage2D(uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage,
                           VkImageAspectFlags aspect);
    void destroyImage(GpuImage& img);

    // Upload host data into a device-local buffer via a staging buffer.
    GpuBuffer createDeviceLocalWithData(const void* data, VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        bool deviceAddress = false);

    // Validation bookkeeping (public so main() can report).
    uint32_t validationErrorCount() const { return mValidationErrors; }
    uint32_t validationWarnCount() const { return mValidationWarnings; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    void createInstance(bool enableValidation);
    void pickPhysicalDevice();
    void createLogicalDevice();

    VkInstance mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mMemProps{};
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    uint32_t mQueueFamily = 0;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    bool mValidation = false;

    uint32_t mValidationErrors = 0;
    uint32_t mValidationWarnings = 0;

public:
    // called by the static debug callback
    void onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                        const char* msg);
};
