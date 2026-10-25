/**
 * @file slassetcachestore.h
 * @brief Viewer glue for the asset and cluster LOD caches on the cache store (#715, #714):
 *        attaches the store to LLFileSystem (assets + mesh parts), keeps the write placement
 *        on the agent's region, pumps idle staged writes, and runs the background migrations
 *        of the old asset folder and the old cluster LOD folder.
 *
 * Main thread.
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

#ifndef SL_SLASSETCACHESTORE_H
#define SL_SLASSETCACHESTORE_H

#include "stdtypes.h"

#include <string>

namespace LLCache { class Store; }

namespace SLAssetCacheStore
{
    // Assets (CacheStoreAssets): attach `store` to LLFileSystem. asset_dir = the old asset
    // folder (<cache>/<DiskCacheDirName>): while it holds unconverted files a miss imports the
    // old file; the owner converts it in the background (after login, throttled, newest
    // first). budget = the ASSETS group budget (older files beyond it are dropped).
    void startAssets(LLCache::Store* store, const std::string& asset_dir, U64 budget, bool read_only);

    // Cluster LOD (CacheStoreCluster): migrate the old .clod folder in the background.
    void startClusterMigration(LLCache::Store* store, const std::string& cluster_dir, U64 budget, bool read_only);

    // The region the agent is in: placement of the records written from now on, and a
    // region visit for the store's recency / manifests. Hooked to LLAgent's region change.
    void hookRegionChanges(LLCache::Store* store);

    // Shutdown, before the store closes: stop the migrations (a file in flight finishes),
    // commit staged asset writes and detach LLFileSystem.
    void stop();
}

#endif // SL_SLASSETCACHESTORE_H
