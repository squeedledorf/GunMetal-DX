// vk_vma.h -- the one way to include the vendored Vulkan Memory Allocator (#411).
//
// VMA's header is third-party code: its warnings (unused parameters, missing field
// initializers, clang nullability annotations) are silenced here so the viewer's own
// warnings stay readable. The implementation lives in vk_vma.cpp; the configuration
// macros below must match in every TU that includes this header.
#pragma once

#include <vulkan/vulkan.h>

// Function loading: the viewer links the Vulkan loader, but VMA fetches every entry
// point it needs through vkGetInstanceProcAddr / vkGetDeviceProcAddr (passed in
// VmaVulkanFunctions), so it never depends on which prototypes a header version exports.
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif

// VMA's internal consistency checks (leaks at pool / allocator destruction, misuse):
// logged to stderr instead of assert() -- a texture leaked at shutdown must not abort.
void vkVmaAssertFail(const char* expr, const char* file, int line);
#ifndef VMA_ASSERT
#define VMA_ASSERT(expr) ((expr) ? (void)0 : vkVmaAssertFail(#expr, __FILE__, __LINE__))
#endif

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunknown-warning-option"
#  pragma clang diagnostic ignored "-Wnullability-completeness"
#  pragma clang diagnostic ignored "-Wnullability-extension"
#  pragma clang diagnostic ignored "-Wunused-parameter"
#  pragma clang diagnostic ignored "-Wunused-variable"
#  pragma clang diagnostic ignored "-Wunused-private-field"
#  pragma clang diagnostic ignored "-Wunused-function"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#  pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#  pragma clang diagnostic ignored "-Wsign-compare"
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Wdeprecated-copy"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wclass-memaccess"
#  pragma GCC diagnostic ignored "-Wparentheses"
#elif defined(_MSC_VER)
#  pragma warning(push, 0)
#endif

#include "third_party/vma/vk_mem_alloc.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
