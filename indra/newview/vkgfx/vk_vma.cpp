// vk_vma.cpp -- the Vulkan Memory Allocator implementation, compiled once (#411).
// See vk_vma.h for the configuration and third_party/vma/README.md for the version.
#define VMA_IMPLEMENTATION
#include "vk_vma.h"

#include <atomic>
#include <cstdio>

void vkVmaAssertFail(const char* expr, const char* file, int line)
{
    static std::atomic<unsigned> s_count{0};
    const unsigned n = s_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 32)
        std::fprintf(stderr, "[vkgfx] VMA check failed: %s (%s:%d)%s\n", expr, file, line,
                     n == 31 ? " -- further VMA checks not logged" : "");
}
