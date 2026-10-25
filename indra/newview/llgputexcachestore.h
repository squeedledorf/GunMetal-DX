/**
 * @file llgputexcachestore.h
 * @brief Private: the segment-store backend of LLGpuTexCache (#715 / #708 / #710).
 *        LLGpuTexCache's static API forwards here while it is active; the renderer
 *        never sees the difference. Main thread only, like LLGpuTexCache.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLGPUTEXCACHESTORE_H
#define LL_LLGPUTEXCACHESTORE_H

#include "llgputexcache.h"

namespace LLCache { class Store; }

namespace LLGpuTexStore
{
    using Key = LLGpuTexCache::Key;
    using Info = LLGpuTexCache::Info;
    using Blob = LLGpuTexCache::Blob;
    using EReadState = LLGpuTexCache::EReadState;

    bool active();
    void init(LLCache::Store* store, bool enabled, U64 budget_bytes);
    void shutdown();
    bool isEnabled();
    void setEnabled(bool enabled);
    void poll();
    bool lookup(const Key& id, Info& out);
    void touch(const Key& id);
    void remove(const Key& id);
    bool requestRead(const Key& id, LLGpuTexCache::EReadPriority prio = LLGpuTexCache::READ_PRIO_NORMAL,
                     U32 first_level = 0, U32 resident_top = 0);
    EReadState readState(const Key& id);
    std::shared_ptr<Blob> takeRead(const Key& id, bool& failed);
    void noteMiss();
    bool canWrite(U64 bytes);
    bool write(const LLGpuTexHeader& hdr, const U8* src, U64 bytes,
               const std::shared_ptr<std::atomic<bool>>& released);
    void setBudget(U64 budget_bytes);
    LLGpuTexCache::Stats getStats();
    const std::string& getDir();
    std::string statsLine();
    void logStats(const std::string& when);
}

#endif // LL_LLGPUTEXCACHESTORE_H
