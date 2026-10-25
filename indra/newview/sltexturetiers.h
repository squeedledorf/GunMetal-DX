/**
 * @file sltexturetiers.h
 * @brief Texture disk tiers in the segment store (#705, #715): the TEXTURES budget group
 *        shared by J2C codestreams and GPU-ready mip chains, the eviction order between
 *        them, and the placement region for texture writes.
 *
 * BUDGET. When J2C lives in the store (CacheStoreJ2C on and the store open), the TEXTURES
 * group budget is the J2C texture budget (the texture share of CacheSize) PLUS the
 * GPU-ready budget (SlipstreamGpuTexCacheSizeMB), and GPUTEX's share cap is the GPU-ready
 * budget's fraction of that (~22% with the defaults, #705's "~30%"). So GPU-ready can never
 * grow past its own setting, J2C may use any room GPU-ready leaves, and the total on disk
 * stays where the two separate caches had it. Without J2C in the store the group is the
 * GPU-ready budget alone, uncapped (as before). One user-facing texture budget and its
 * sizing are #711's.
 *
 * EVICTION ORDER (lowest tier goes first; inside a tier the store's keep score:
 * reuse x decode cost / bytes x recency):
 *   tier 0  J2C records whose texture the GPU-ready tier holds at full size (or at the
 *           2048 cap): duplicates, kept while there is room (#705 "Duplicates");
 *           also every J2C record while J2C is NOT served from the store (left over from
 *           a session with CacheStoreJ2C on: they must not squeeze GPU-ready out)
 *           (likewise every GPU-ready record while GPU-ready is not served from the store)
 *   tier 1  GPU-ready records (#865: a tail + one record per level above 256 px each;
 *           the keep score's 1 / bytes and every use touching the tail make a
 *           chain lose its top levels first and its tail last, see llgputexrecord.h)
 *   tier 2  other J2C records (the source of truth: never evicted for GPU-ready)
 * The duplicate set is rebuilt on this module's thread every 60 s (forEach over GPUTEX);
 * the store's eviction runs on its writer thread under an index lock and must not call
 * back into the store, so the policy only reads that snapshot.
 *
 * Not done yet (#705 follow-up): reuse-admitted GPU-ready captures (Normal vs Aggressive
 * strategy option). The index already carries what admission needs (prev_session_access,
 * reuse_count, on J2C records too, so "found in J2C with a last access from an earlier
 * session" is a lookup).
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

#ifndef SL_SLTEXTURETIERS_H
#define SL_SLTEXTURETIERS_H

#include "llcachestore.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class SLTextureTiers
{
public:
    static SLTextureTiers& instance();
    ~SLTextureTiers();

    // After the store opened (main thread): sets the TEXTURES budget / GPUTEX share cap
    // now, and starts the helper thread (installs the eviction policy once the index is
    // loaded, refreshes the duplicate set, registers region visits). j2c_in_store: J2C is served from the store.
    // gputex_in_store: GPU-ready is served from the store (CacheStoreGpuTex); when it is
    // not, the group is the J2C budget alone and leftover GPU-ready records go first.
    void start(LLCache::Store* store, bool j2c_in_store, bool gputex_in_store, U64 j2c_budget, U64 gputex_budget);
    // Before the store closes. Safe when never started.
    void stop();

    // Budget settings changed (any order; applied at once when started).
    void setGpuTexBudget(U64 bytes);
    void setJ2CBudget(U64 bytes);
    U64  j2cBudget() const { return mJ2CBudget.load(); }
    U64  gpuTexBudget() const { return mGpuTexBudget.load(); }
    bool j2cInStore() const { return mJ2CInStore.load(); }

    // Placement for texture writes (#708): the agent's current region. setRegion() from
    // the main thread when it changes (also registers the visit with the store, on the
    // helper thread); placement() from any thread.
    void setRegion(const LLUUID& region, U64 grid_handle);
    LLCache::PlacementHint placement() const;

    // Duplicate-set facts for stats lines.
    U32 duplicateCandidates() const { return mDupCount.load(); }

private:
    void applyBudgets();
    void run();
    void refreshDuplicates();

    class Policy;
    LLCache::Store* mStore = nullptr;
    std::shared_ptr<Policy> mPolicy;
    std::atomic<bool> mJ2CInStore{ false };
    std::atomic<bool> mGpuTexInStore{ true };
    std::atomic<U64> mJ2CBudget{ 0 };
    std::atomic<U64> mGpuTexBudget{ 0 };
    std::atomic<U32> mDupCount{ 0 };

    mutable std::mutex mRegionMutex;
    LLCache::PlacementHint mRegion;
    bool mRegionVisitPending = false;

    std::thread mThread;
    std::mutex mWakeMutex;
    std::condition_variable mWake;
    bool mStop = false;
};

#endif // SL_SLTEXTURETIERS_H
