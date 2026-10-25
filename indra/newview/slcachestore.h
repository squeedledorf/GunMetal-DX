/**
 * @file slcachestore.h
 * @brief The viewer's one segment store instance (#715): <cache>/store, opened at
 *        startup, closed at shutdown. Consumers (GPU-ready textures first; J2C, assets,
 *        cluster LOD later) get it through SLCacheStore::instance().store().
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

#ifndef SL_SLCACHESTORE_H
#define SL_SLCACHESTORE_H

#include "llcachestore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class SLCacheStore
{
public:
    static SLCacheStore& instance();
    ~SLCacheStore();

    // <cache>/store (LL_PATH_CACHE must be set).
    static std::string defaultDir();

    struct Options
    {
        bool read_only = false;           // a second viewer instance (LLAppViewer::mSecondInstance)
        bool purge = false;               // Clear Cache: drop every record after opening
        U32  snapshot_interval_s = 1800;  // CacheStoreSnapshotIntervalSec
        U64  textures_budget = 0;         // TEXTURES group budget (0 = the store's default)
        U64  assets_budget = 0;           // ASSETS group (assets + mesh parts; 0 = default)
        U64  cluster_budget = 0;          // CLUSTER group (cluster LOD records; 0 = default)
        U32  read_threads = 0;            // CacheStoreReadThreads (0 = auto: drive + cores)
        U32  decode_threads = 0;          // CacheStoreDecodeThreads (0 = auto: cores / 4, 1..4)
    };
    // Opens the store (the index loads on the store's writer thread: lookups miss until
    // ready()). A second instance, or any instance that finds the store locked, opens it
    // read-only with a private overflow. False (and logged) when it cannot be opened: the
    // consumers then fall back to their old caches.
    bool open(const Options& opt);
    // Stops the background helpers (verify), then closes the store (flushes runs, writes
    // the index snapshot). Safe when never opened.
    void close();

    bool isOpen() const { return mStore && mStore->isOpen(); }
    bool readOnly() const { return mStore && mStore->readOnly(); }
    LLCache::Store* store() { return isOpen() ? mStore.get() : nullptr; }

    void setTexturesBudget(U64 bytes);

    // ---- inspection (#714): Develop > Cache
    void logStats(const char* when);
    // Reads and verifies every record on a background thread; logs the result. False
    // when not open or a verify is already running.
    bool verifyAllAsync();
    // One line per record of `kind` (NONE = every kind) into `path`; returns the count.
    U32 listRecords(LLCache::Kind kind, const std::string& path);
    // The decoded bytes of one record into `path`.
    bool extractRecord(const LLCache::Key& key, const std::string& path);
    // #865: a GPU-ready texture's whole chain (v1 record or v2 tail + levels, resolved like
    // a lookup), level 0 first, into `path`.
    bool extractGpuTexChain(const LLUUID& id, bool mask, const std::string& path);
    // "<uuid>[ <kind name or number>[ <variant>]]" (kind defaults to gputex, variant 0).
    // gputex_chain (optional): set for a gputex key with no variant / "chain" / "chain-mask"
    // (the whole chain; out.variant = the mask bit).
    static bool parseKey(const std::string& text, LLCache::Key& out, bool* gputex_chain = nullptr);
    static bool parseKind(const std::string& text, LLCache::Kind& out);

private:
    std::unique_ptr<LLCache::Store> mStore;
    std::thread mVerify;
    std::atomic<bool> mVerifying{ false };
};

#endif // SL_SLCACHESTORE_H
