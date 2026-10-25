// vktest — Vulkan de-risking spike for a possible Alchemy Viewer Vulkan port.
// vk_common.h — shared helpers, error checking, small utilities.
//
// This file is intentionally standalone: it depends only on the Vulkan headers,
// the C++ standard library, and glm. Nothing here touches the viewer.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- error checking ---------------------------------------------------------

const char* vkResultString(VkResult r);

#define VK_CHECK(expr)                                                          \
    do {                                                                        \
        VkResult _vk_res = (expr);                                              \
        if (_vk_res != VK_SUCCESS) {                                            \
            std::fprintf(stderr, "[vktest] VK_CHECK failed: %s = %s (%d)\n"     \
                         "  at %s:%d\n", #expr, vkResultString(_vk_res),        \
                         (int)_vk_res, __FILE__, __LINE__);                     \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

// ---- logging ----------------------------------------------------------------

#define VK_LOG(...)                                                             \
    do {                                                                        \
        std::fprintf(stdout, "[vktest] " __VA_ARGS__);                          \
        std::fprintf(stdout, "\n");                                             \
        std::fflush(stdout);                                                    \
    } while (0)
