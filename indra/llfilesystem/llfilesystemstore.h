/**
 * @file llfilesystemstore.h
 * @brief The segment-store backend of LLFileSystem (#715 / #708 / #714): the asset cache
 *        (sounds, animations, notecards, scripts, gestures, settings, materials, GLTF,
 *        landmarks, wearables, upload scratch) as ASSET records in the viewer's one cache
 *        store instead of one sl_cache_<uuid>_0.asset file each.
 *
 * INJECTION: llfilesystem sits below newview, so the viewer hands the open store in at
 * startup (attach()) and takes it back before closing it (detach()). With nothing attached
 * LLFileSystem is the old file-per-asset cache, unchanged (CacheStoreAssets off, the store
 * failed to open). The choice is made per LLFileSystem object at construction.
 *
 * KEYS: Key(asset UUID, Kind::ASSET, 0) -- UUID only, as the old file name (the asset type is
 * not part of today's key, design doc 6.6); the asset type goes in the record meta
 * (META_VERSION, type). Meshes do not come through here when the store is active: the mesh
 * repository writes per-part MESH records itself (newview/llmeshdiskcache.h).
 *
 * WRITES: an asset is written as ONE record. LLFileSystem's write / append / seek+write /
 * setSize calls go to a per-UUID staging table (RAM), independent of any object's lifetime
 * (the UDP transfer and xfer code open one APPEND object per packet). The staged bytes are
 * committed as one record:
 *   - on rename() (the download flow: write a temp UUID, rename to the asset UUID);
 *   - when the last WRITE / READ_WRITE object of the key closes (it owned the whole content);
 *   - APPEND-only staging: once no writer has had it open for `commit_grace_s` (pump());
 *   - at detach() (shutdown), whatever is still staged.
 * Readers (READ objects, getExists, getFileSize) see the staged bytes at once, as they saw
 * every flushed write of the old files. remove() drops staging + record; a WRITE object's
 * first write truncates (restarts) the staging; an APPEND / READ_WRITE first write on a key
 * that is only committed copies the record into staging first (rare: notecard/gesture saves).
 * Commits are pinned for the session (WF_PIN_SESSION: the asset system delivers assets
 * THROUGH the cache, the callback reads the record back) and carry the current region as
 * their placement (setPlacement()).
 *
 * READS: one whole-record read per READ object (readAll), served from RAM afterwards (assets
 * are small; the Ogg decoder's seek/read callbacks then cost nothing). While the old folder
 * still holds unconverted files (legacy fallback, set by the migration), a miss looks for the
 * old file and imports it on the spot (the migration later finds the key and deletes the file).
 *
 * THREADING: every function is any-thread. The store pointer is held under a shared lock for
 * the duration of each call (Access); detach() takes it exclusively, so a call in flight on
 * the mesh / General / main thread finishes before the store closes.
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

#ifndef LL_LLFILESYSTEMSTORE_H
#define LL_LLFILESYSTEMSTORE_H

#include "llassettype.h"
#include "llcachestore.h"
#include "lluuid.h"

#include <string>
#include <vector>

namespace LLFileSystemStore
{
    constexpr U8 META_VERSION = 1;   // meta = { META_VERSION, (U8)asset type }

    struct Options
    {
        // The old asset folder still holds files the migration has not converted: a miss
        // looks there and imports the file (setLegacyFallback() turns it off when done).
        bool legacy_fallback = false;
        F32  commit_grace_s = 2.f;            // APPEND-only staging: commit after this idle time
        U64  max_staged_bytes = 256ull << 20; // above: closed staging is committed at once
        U32  ready_wait_ms = 10000;           // first uses wait up to this long (in total) for the index
    };

    // ---- viewer side (main thread)
    void attach(LLCache::Store* store, const Options& opt);
    // Commits everything still staged, then forgets the store (waits for calls in flight).
    void detach();
    bool active();
    void setLegacyFallback(bool on);
    bool legacyFallback();
    // Current region of the agent: the placement of every asset / mesh record written from
    // now on (assets fetched for a region belong to its run, #708). Null = shared.
    void setPlacement(const LLUUID& region, U64 grid_handle);
    LLCache::PlacementHint placement();
    // Commit APPEND-only staging that has been idle for commit_grace_s (any thread; the
    // viewer calls it every second, LLFileSystem calls it opportunistically).
    void pump();

    struct Stats
    {
        U32 staged = 0;
        U64 staged_bytes = 0, peak_staged_bytes = 0;
        U64 commits = 0, commit_bytes = 0, commit_fail = 0;
        U64 imported = 0, imported_bytes = 0;     // legacy files imported on a miss
        U64 corrupt = 0;
    };
    Stats stats();
    std::string statsLine();

    // Scoped use of the attached store (shared lock; never nest two in one thread).
    class Access
    {
    public:
        Access();
        ~Access();
        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;
        LLCache::Store* get() const { return mStore; }
        LLCache::Store* operator->() const { return mStore; }
        explicit operator bool() const { return mStore != nullptr; }
    private:
        LLCache::Store* mStore = nullptr;
    };
    // The store's index loads on its writer thread after open: the first uses wait for it
    // (bounded by Options::ready_wait_ms in total), so a login-time read is not a false miss.
    bool waitReady(LLCache::Store* store);

    // The old cache file of an asset (the legacy fallback / import source).
    std::string legacyPath(const LLUUID& id);

    // ---- the LLFileSystem backend (any thread)
    LLCache::Key key(const LLUUID& id);
    // Size: staged bytes, else the committed record, else (fallback) the old file. 0 = absent.
    S32  size(const LLUUID& id);
    enum class ReadFrom : U8 { NONE, STAGED, STORE, LEGACY, CORRUPT };
    // The whole content. NONE / CORRUPT = absent (CORRUPT: the record was bad and dropped).
    ReadFrom readAll(const LLUUID& id, LLAssetType::EType type, std::vector<U8>& out);

    // Writers. beginWrite() = an object's first write: registers it as a writer of `id`;
    // truncate (WRITE mode) restarts the staging, otherwise a committed record (or old file)
    // is copied in first. endWrite() = the object closes; commit_now (WRITE / READ_WRITE)
    // commits when it was the last writer, else the staging waits for pump().
    bool beginWrite(const LLUUID& id, LLAssetType::EType type, bool truncate);
    void endWrite(const LLUUID& id, bool commit_now);
    // Write at `pos` (zero-filling a gap), or append when pos < 0. Returns the new position.
    bool write(const LLUUID& id, S64 pos, const U8* p, S32 n, S32& new_pos);
    bool setSize(const LLUUID& id, S32 size);
    bool remove(const LLUUID& id);
    bool rename(const LLUUID& from, const LLUUID& to, LLAssetType::EType to_type);
}

#endif // LL_LLFILESYSTEMSTORE_H
