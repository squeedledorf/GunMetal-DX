/**
 * @file slassetcachestore.cpp
 * @brief Viewer glue for the asset / cluster LOD caches on the cache store (#715). See
 *        slassetcachestore.h.
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

#include "slassetcachestore.h"

#include "llagent.h"
#include "llcachestore.h"
#include "llcallbacklist.h"
#include "llcleanclustercachestore.h"
#include "llfilesystemstore.h"
#include "llviewerregion.h"
#include "slassetmigrate.h"
#include "slcachemigrate.h"

#include <atomic>

namespace
{
    SLCacheMigrate::Runner sAssetMigration;
    SLCacheMigrate::Runner sClusterMigration;
    boost::signals2::connection sRegionConn;
    LLCache::Store* sRegionStore = nullptr;
    std::shared_ptr<std::atomic<bool>> sPumpAlive;
    LLUUID sPlacedRegion;   // last region id handed to setPlacement (main thread)

    // After login: the store's index load and the login burst go first.
    constexpr F64 MIGRATION_DELAY_S = 15.0;

    void onRegionChanged()
    {
        LLViewerRegion* region = gAgent.getRegion();
        const LLUUID id = region ? region->getRegionID() : LLUUID::null;
        const U64 handle = region ? region->getHandle() : 0;
        LLFileSystemStore::setPlacement(id, handle);
        sPlacedRegion = id;
        if (sRegionStore && sRegionStore->isOpen() && id.notNull()) sRegionStore->regionVisit(id, handle);
    }
}

namespace SLAssetCacheStore
{

void startAssets(LLCache::Store* store, const std::string& asset_dir, U64 budget, bool read_only)
{
    if (!store) return;
    SLAssetMigrate::Converter probe;
    const bool old_files = SLCacheMigrate::hasWork(asset_dir, probe);
    LLFileSystemStore::Options fo;
    fo.legacy_fallback = old_files;
    LLFileSystemStore::attach(store, fo);

    // Commit APPEND-only staging once idle (LLFileSystem also pumps on its own calls).
    sPumpAlive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> alive = sPumpAlive;
    doPeriodically([alive]()
    {
        if (!alive->load()) return true;   // stopped
        LLFileSystemStore::pump();
        // The login region's id can arrive after the region-change signal: keep the
        // placement on the region the agent is really in.
        LLViewerRegion* region = gAgent.getRegion();
        if (region && region->getRegionID() != sPlacedRegion) onRegionChanged();
        return false;
    }, 1.f);

    if (old_files && !read_only && !store->readOnly())
    {
        // #714: convert the old folder in the background (throttled, newest first, each file
        // deleted once its records are on disk; the folder goes when empty). Until then a miss
        // reads + imports the old file, so nothing is downloaded again.
        SLCacheMigrate::Options mo;
        mo.live_cap_bytes = budget;
        sAssetMigration.start(asset_dir, store, std::make_shared<SLAssetMigrate::Converter>(), mo, MIGRATION_DELAY_S,
                              [](const SLCacheMigrate::Result& r)
                              {
                                  if (r.completed) LLFileSystemStore::setLegacyFallback(false);
                              });
    }
}

void startClusterMigration(LLCache::Store* store, const std::string& cluster_dir, U64 budget, bool read_only)
{
    if (!store || read_only || store->readOnly()) return;
    LLCleanClusterStore::Converter probe;
    if (!SLCacheMigrate::hasWork(cluster_dir, probe)) return;
    SLCacheMigrate::Options mo;
    mo.live_cap_bytes = budget;
    mo.max_read_bytes_per_sec = 24ull << 20;
    sClusterMigration.start(cluster_dir, store, std::make_shared<LLCleanClusterStore::Converter>(), mo,
                            MIGRATION_DELAY_S + 5.0);
}

void hookRegionChanges(LLCache::Store* store)
{
    sRegionStore = store;
    if (!sRegionConn.connected())
    {
        sRegionConn = gAgent.addRegionChangedCallback([]() { onRegionChanged(); });
    }
    onRegionChanged();
}

void stop()
{
    sAssetMigration.stop();
    sClusterMigration.stop();
    if (sPumpAlive) sPumpAlive->store(false);
    sRegionConn.disconnect();
    sRegionStore = nullptr;
    LLFileSystemStore::detach();
}

} // namespace SLAssetCacheStore
