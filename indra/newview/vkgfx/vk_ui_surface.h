// UI target contract: no native window ownership, toolkit or world-renderer types.
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>

using NativeSurfaceId = std::uint64_t; // 0 main; nonzero stable auxiliary/RTT identity

inline constexpr NativeSurfaceId NativeSurfaceMain = 0;
inline constexpr NativeSurfaceId NativeSurfaceVRUI = NativeSurfaceId(1) << 63;

struct VkUISurfaceTarget
{
    NativeSurfaceId surfaceID = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage colorImage = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{}; // framebuffer pixels
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    float dpiScaleX = 1.f, dpiScaleY = 1.f;
    // Exact common graphics-queue serial. May be shared by targets recorded in
    // the SAME submission only. completedSerial is queried from its GPU timeline;
    // a skipped/failed submit never authorizes an early release.
    std::uint64_t frameSerial = 0, completedSerial = 0;
    // Optional backdrop source. Absent for UI-only native windows. Sampling or
    // copying requires provider-granted usage; colorImage alone is not a grant.
    VkImage sceneImage = VK_NULL_HANDLE;
    VkImageView sceneView = VK_NULL_HANDLE;
    VkFormat sceneFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D sceneExtent{};
    VkImageLayout sceneLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags sceneUsage = 0; // explicit grants; never assume SAMPLED for a swapchain
};

// Called outside dynamic rendering. Record only; never submit/present, never
// retain target handles, leave color in colorLayout, and handle allocation
// refusal without throwing. The provider owns synchronization and scheduling.
using VkUISurfaceRecorder = std::function<void(const VkUISurfaceTarget&)>;
