/**
 * @file sltexturetiers.cpp
 * @brief Texture disk tiers in the segment store (#705, #715). See sltexturetiers.h.
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

#include "llviewerprecompiledheaders.h"

#include "sltexturetiers.h"

#include "llgputexrecord.h"

#include <chrono>
#include <unordered_set>
#include <utility>
#include <vector>

// The TEXTURES group's eviction order (sltexturetiers.h). Called by the store's writer
// thread with an index shard locked: reads only its own snapshot.
class SLTextureTiers::Policy : public LLCache::EvictionPolicy
{
public:
    using IdSet = std::unordered_set<LLUUID>;

    int tier(const LLCache::RecordInfo& r) const override
    {
        if (r.key.kind == LLCache::Kind::J2C)
        {
            if (!mJ2CActive.load(std::memory_order_relaxed)) return 0;
            return isDuplicate(r) ? 0 : 2;
        }
        if (r.key.kind == LLCache::Kind::GPUTEX) return mGpuTexActive.load(std::memory_order_relaxed) ? 1 : 0;
        return 2;
    }

    bool isDuplicate(const LLCache::RecordInfo& r) const override
    {
        if (r.key.kind != LLCache::Kind::J2C) return false;
        std::shared_ptr<const IdSet> s;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            s = mFullSize;
        }
        return s && s->count(r.key.id) != 0;
    }

    void setFullSize(std::shared_ptr<const IdSet> s)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFullSize = std::move(s);
    }
    void setJ2CActive(bool on) { mJ2CActive = on; }
    void setGpuTexActive(bool on) { mGpuTexActive = on; }

private:
    mutable std::mutex mMutex;
    std::shared_ptr<const IdSet> mFullSize;
    std::atomic<bool> mJ2CActive{ false };
    std::atomic<bool> mGpuTexActive{ true };
};

// static
SLTextureTiers& SLTextureTiers::instance()
{
    static SLTextureTiers sInstance;
    return sInstance;
}

SLTextureTiers::~SLTextureTiers()
{
    stop();
}

void SLTextureTiers::start(LLCache::Store* store, bool j2c_in_store, bool gputex_in_store, U64 j2c_budget,
                           U64 gputex_budget)
{
    stop();
    mStore = store;
    mJ2CInStore = j2c_in_store && store;
    mGpuTexInStore = gputex_in_store;
    if (j2c_budget) mJ2CBudget = j2c_budget;
    if (gputex_budget) mGpuTexBudget = gputex_budget;
    if (!store) return;
    mPolicy = std::make_shared<Policy>();
    mPolicy->setJ2CActive(mJ2CInStore.load());
    mPolicy->setGpuTexActive(mGpuTexInStore.load());
    applyBudgets();
    {
        std::lock_guard<std::mutex> lock(mWakeMutex);
        mStop = false;
    }
    if (!store->readOnly())
    {
        mThread = std::thread([this]() { run(); });
    }
    LL_INFOS("CacheStore") << "Texture tiers: TEXTURES budget " << (store->budget(LLCache::Group::TEXTURES) >> 20)
                           << " MB (J2C " << (j2c_in_store ? std::to_string(mJ2CBudget.load() >> 20) + " MB" : std::string("not in the store"))
                           << ", GPU-ready cap " << (mGpuTexBudget.load() >> 20) << " MB); eviction: duplicate J2C, then GPU-ready, then J2C"
                           << LL_ENDL;
}

void SLTextureTiers::stop()
{
    {
        std::lock_guard<std::mutex> lock(mWakeMutex);
        mStop = true;
    }
    mWake.notify_all();
    if (mThread.joinable()) mThread.join();
    mStore = nullptr;
}

void SLTextureTiers::setGpuTexBudget(U64 bytes)
{
    if (!bytes) return;
    mGpuTexBudget = bytes;
    applyBudgets();
}

void SLTextureTiers::setJ2CBudget(U64 bytes)
{
    if (!bytes) return;
    mJ2CBudget = bytes;
    applyBudgets();
}

void SLTextureTiers::applyBudgets()
{
    LLCache::Store* store = mStore;
    if (!store) return;
    const U64 gpu = mGpuTexBudget.load();
    LLCache::KindPolicy gp = store->kindPolicy(LLCache::Kind::GPUTEX);
    if (mJ2CInStore && !mGpuTexInStore)
    {
        // GPU-ready on its .gtx files: the group is J2C's (leftover GPU-ready records are
        // tier 0 in the policy, so they go first without a cap forcing them out at once).
        const U64 group = mJ2CBudget.load();
        if (!group) return;
        store->setBudget(LLCache::Group::TEXTURES, group);
        gp.share_cap = 1.f;
    }
    else if (mJ2CInStore)
    {
        const U64 group = mJ2CBudget.load() + gpu;
        if (!group) return;
        store->setBudget(LLCache::Group::TEXTURES, group);
        gp.share_cap = (F32)std::min(1.0, (F64)gpu / (F64)group);
    }
    else
    {
        if (!gpu) return;
        store->setBudget(LLCache::Group::TEXTURES, gpu);
        gp.share_cap = 1.f;
    }
    store->setKindPolicy(LLCache::Kind::GPUTEX, gp);   // same format version: nothing dropped
}

void SLTextureTiers::setRegion(const LLUUID& region, U64 grid_handle)
{
    {
        std::lock_guard<std::mutex> lock(mRegionMutex);
        if (mRegion.region == region && mRegion.grid_handle == grid_handle) return;
        mRegion = region.isNull() ? LLCache::PlacementHint::shared() : LLCache::PlacementHint::inRegion(region, grid_handle);
        mRegionVisitPending = region.notNull();
    }
    mWake.notify_all();
}

LLCache::PlacementHint SLTextureTiers::placement() const
{
    std::lock_guard<std::mutex> lock(mRegionMutex);
    return mRegion;
}

void SLTextureTiers::refreshDuplicates()
{
    LLCache::Store* store = mStore;
    if (!store || !mJ2CInStore) return;
    // GPU-ready texture at full size: a record holding a full-size top level (or the 2048
    // cap the viewer never exceeds) -- and, #865, a chain that still resolves to it (a v2
    // level record without its tail / the levels below it serves nothing).
    std::vector<std::pair<LLUUID, bool>> cand;
    store->forEach(LLCache::Kind::GPUTEX, [&](const LLCache::RecordInfo& r) -> bool
    {
        if (!r.staged && LLGpuTexRecord::holdsFullSizeTop(r))
            cand.emplace_back(r.key.id, LLGpuTexRecord::isMaskVariant(r.key.variant));
        return true;
    });
    auto set = std::make_shared<Policy::IdSet>();
    for (const auto& c : cand)
    {
        if (set->count(c.first)) continue;
        LLGpuTexRecord::Chain ch;
        if (!LLGpuTexRecord::resolve(*store, c.first, c.second, ch)) continue;
        const LLGpuTexHeader& h = ch.hdr;
        if (h.w >= 2048 || h.h >= 2048 || (h.full_w && h.full_h && h.w >= h.full_w && h.h >= h.full_h))
            set->insert(c.first);
    }
    // Only J2C records that exist matter for the count shown in stats.
    U32 dups = 0;
    for (const LLUUID& id : *set)
        if (store->lookup(LLCache::Key(id, LLCache::Kind::J2C, 0))) ++dups;
    mDupCount = dups;
    mPolicy->setFullSize(std::move(set));
}

void SLTextureTiers::run()
{
    LL_PROFILER_SET_THREAD_NAME("TextureTiers");
    LLCache::Store* store = mStore;
    // The index loads on the store's writer thread; setEvictionPolicy() waits for it.
    while (!store->ready())
    {
        std::unique_lock<std::mutex> lock(mWakeMutex);
        if (mWake.wait_for(lock, std::chrono::milliseconds(100), [this] { return mStop; })) return;
    }
    store->setEvictionPolicy(LLCache::Group::TEXTURES, mPolicy);
    auto last_refresh = std::chrono::steady_clock::now() - std::chrono::hours(1);
    while (true)
    {
        bool visit = false;
        LLCache::PlacementHint where;
        {
            std::lock_guard<std::mutex> lock(mRegionMutex);
            if (mRegionVisitPending)
            {
                visit = true;
                where = mRegion;
                mRegionVisitPending = false;
            }
        }
        if (visit) store->regionVisit(where.region, where.grid_handle);   // may load the region's manifest

        const auto now = std::chrono::steady_clock::now();
        if (now - last_refresh >= std::chrono::seconds(60))
        {
            last_refresh = now;
            refreshDuplicates();
        }

        std::unique_lock<std::mutex> lock(mWakeMutex);
        mWake.wait_for(lock, std::chrono::seconds(5), [this] { return mStop; });
        if (mStop) return;
    }
}
