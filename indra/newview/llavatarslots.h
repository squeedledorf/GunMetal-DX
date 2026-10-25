/**
 * @file llavatarslots.h
 * @brief The avatar (joint-table) slot cap: ONE constant for every per-avatar table of the VK
 *        renderer -- the skin store's joint table and slot allocator, av_vis, the per-slot CPU
 *        arrays, the cluster LOD rig lists / per-avatar rows / weld words, the cluster RT masks,
 *        the cel selection bitset. GLSL twin: indra/vkclean/shaders/clean_av_slots.glsl.
 *
 * 2026-10-14: 256 -> 1024 (avatars, animesh control avatars and flexi pseudo-slots share it; 63
 * animesh at Cloudsdale filled 256 with a crowd). Right-sizing to demand is #762.
 * Memory at 1024: joint table 1024 x 13.8 KB = 14.2 MB on the GPU (+ the same CPU mirror), GPU
 * animation local state 10.6 MB, av_vis ~48 KB.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "llerror.h"

constexpr uint32_t kAvatarSlotCap = 1024;            // = clean_av_slots.glsl AV_SLOT_CAP
constexpr uint32_t kAvatarSlotWords = kAvatarSlotCap / 32;   // one bit per slot
constexpr uint32_t kAvatarSlotMask = kAvatarSlotCap - 1;     // a slot field (10 bits)
static_assert((kAvatarSlotCap & (kAvatarSlotCap - 1)) == 0, "slot cap: a power of two (masks)");

// Always-on guard for a CPU-side slot index read from data (skin meta, cluster instance flags,
// owner keys) before it indexes a per-slot table: true = valid; false = never indexed, logged
// rate-limited (the first time, then every 4096th) with the site. kNone (0xFFFFFFFF, "no slot")
// is a normal value: false without a log.
inline bool llAvatarSlotOk(uint32_t slot, const char* site)
{
    if (slot < kAvatarSlotCap) return true;
    if (slot == 0xFFFFFFFFu) return false;
    static std::atomic<uint32_t> s_bad{0};
    const uint32_t n = s_bad.fetch_add(1, std::memory_order_relaxed);
    if ((n & 4095u) == 0u)
        LL_WARNS("Vulkan") << "avatar slot " << slot << " >= the cap " << kAvatarSlotCap << " at " << (site ? site : "?")
                           << " (" << (n + 1) << " so far): skipped" << LL_ENDL;
    return false;
}
