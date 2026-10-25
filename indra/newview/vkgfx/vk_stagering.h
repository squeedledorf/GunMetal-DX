// vk_stagering.h -- a persistent host-visible STAGING RING, sub-allocated linearly.
//
// One persistently mapped buffer serves many uploads (per-upload vkAllocateMemory + free
// was a large part of streaming cost, and on some Windows drivers freed host-visible
// memory is not returned promptly: #458). Virtual offsets grow monotonically (physical =
// v % cap); the owner records head_v as a mark after the frame that used the bytes is
// recorded, and moves tail_v up to a mark once that frame's fence has been waited.
// alloc() refuses a request larger than cap / 4 (one upload never monopolises the ring)
// or one that does not fit in the free space: the caller then uses a one-off buffer.
//
// Users: LLCleanRendererVK (world texture uploads, TexStageRing) and LLRenderVK (UI /
// media texture uploads).
#pragma once

#include "vk_device.h"

#include <cstdint>

struct VkStageRing
{
    GpuBuffer buf{};
    uint64_t  cap = 0, head_v = 0, tail_v = 0;
    bool alloc(uint64_t size, uint64_t& off)
    {
        if (!buf.buffer || !buf.mapped || size == 0 || size > cap / 4) return false;
        size = (size + 255ull) & ~255ull;
        uint64_t pad = 0;
        const uint64_t phys = head_v % cap;
        if (phys + size > cap) pad = cap - phys;   // don't straddle the wrap
        if ((head_v - tail_v) + pad + size > cap) return false;
        head_v += pad;
        off = head_v % cap;
        head_v += size;
        return true;
    }
    // Would alloc(size) succeed now? (no state change)
    bool fits(uint64_t size) const
    {
        if (!buf.buffer || !buf.mapped || size == 0 || size > cap / 4) return false;
        size = (size + 255ull) & ~255ull;
        const uint64_t phys = head_v % cap;
        const uint64_t pad = phys + size > cap ? cap - phys : 0;
        return (head_v - tail_v) + pad + size <= cap;
    }
    uint64_t used() const { return head_v - tail_v; }
};
