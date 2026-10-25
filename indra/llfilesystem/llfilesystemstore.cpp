/**
 * @file llfilesystemstore.cpp
 * @brief The segment-store backend of LLFileSystem (#715). See llfilesystemstore.h.
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

#include "linden_common.h"

#include "llfilesystemstore.h"

#include "llcachecodec.h"
#include "llcachemetrics.h"
#include "lldiskcache.h"
#include "llfile.h"
#include "llformat.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    F64 nowSec()
    {
        return std::chrono::duration<F64>(Clock::now().time_since_epoch()).count();
    }

    U64 nowUsec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    }

    // Store pointer: shared lock for every call, exclusive for attach / detach.
    std::shared_mutex sStoreMutex;
    LLCache::Store* sStore = nullptr;
    std::atomic<bool> sActive{ false };
    LLFileSystemStore::Options sOpt;
    std::atomic<bool> sLegacyFallback{ false };
    std::atomic<F64> sReadyDeadline{ 0.0 };   // waitReady() stops waiting after this (steady seconds)

    std::mutex sPlacementMutex;
    LLCache::PlacementHint sPlacement;

    struct Staged
    {
        std::vector<U8> bytes;
        LLAssetType::EType type = LLAssetType::AT_NONE;
        U32 writers = 0;          // objects that wrote and are still open
        F64 closed_at = 0.0;      // when writers dropped to 0 (pump() commits after the grace)
        U64 t0 = 0;               // first write (usec): commit metrics measure from here
    };
    std::mutex sStagingMutex;     // guards everything below
    std::unordered_map<LLUUID, Staged> sStaging;
    U64 sStagedBytes = 0;
    U64 sPeakStaged = 0;
    F64 sNextPump = 0.0;

    std::atomic<U64> sCommits{ 0 }, sCommitBytes{ 0 }, sCommitFail{ 0 };
    std::atomic<U64> sImported{ 0 }, sImportedBytes{ 0 }, sCorrupt{ 0 };

    LLCacheMetrics::Cache metricsCache(LLAssetType::EType type)
    {
        return type == LLAssetType::AT_MESH ? LLCacheMetrics::Cache::MESH : LLCacheMetrics::Cache::ASSET;
    }

    // On-disk bytes of an uncompressed ASSET record: 96-byte record header + meta + a
    // 24-byte LLCacheCodec frame + the payload, padded to 16 (llcachestore.h contract).
    U64 recordDiskBytes(U64 payload, U32 meta_len)
    {
        return (96 + meta_len + 24 + payload + 15) & ~15ull;
    }

    void account(S64 delta)
    {
        if (delta < 0) sStagedBytes -= std::min<U64>(sStagedBytes, (U64)(-delta));
        else sStagedBytes += (U64)delta;
        sPeakStaged = std::max(sPeakStaged, sStagedBytes);
    }

    void eraseStaged(std::unordered_map<LLUUID, Staged>::iterator it)
    {
        account(-(S64)it->second.bytes.size());
        sStaging.erase(it);
    }

    // Read a whole old cache file (empty when absent).
    bool readLegacyFile(const LLUUID& id, std::vector<U8>& out)
    {
        out.clear();
        const std::string path = LLFileSystemStore::legacyPath(id);
        LLFILE* fp = LLFile::fopen(path, "rb");
        if (!fp) return false;
        bool ok = false;
        if (fseek(fp, 0, SEEK_END) == 0)
        {
            const long n = ftell(fp);
            if (n > 0 && (U64)n <= LLCacheCodec::getMaxRecordSize() && fseek(fp, 0, SEEK_SET) == 0)
            {
                try { out.resize((size_t)n); }
                catch (const std::bad_alloc&) { out.clear(); }
                ok = !out.empty() && fread(out.data(), 1, out.size(), fp) == out.size();
            }
        }
        LLFile::close(fp);
        if (!ok) out.clear();
        return ok;
    }

    void writeRecord(LLCache::Store* store, const LLUUID& id, LLAssetType::EType type, std::vector<U8>&& bytes,
                     U32 flags, LLCache::Priority prio, U64 t0, bool count_metrics)
    {
        const U64 n = bytes.size();
        U8 meta[2] = { LLFileSystemStore::META_VERSION, (U8)(S8)type };
        LLCache::WriteOptions o;
        o.flags = flags;
        o.priority = prio;
        o.placement = LLFileSystemStore::placement();
        o.meta = meta;
        o.meta_len = sizeof(meta);
        auto done = std::make_shared<LLCache::Ticket<LLCache::Status>>();
        const LLCacheMetrics::Cache mc = metricsCache(type);
        // Inline on the store's writer thread once the record is on disk (or refused).
        o.completion.wake = [done, n, t0, mc, count_metrics]()
        {
            const LLCache::Status st = done->take();
            const bool ok = st == LLCache::Status::OK || st == LLCache::Status::SKIPPED;
            if (ok) { sCommits++; sCommitBytes += n; }
            else sCommitFail++;
            if (count_metrics && st != LLCache::Status::SKIPPED)   // skipped / superseded: not a write
            {
                const U64 t = nowUsec();
                LLCacheMetrics::recordWrite(mc, LLCacheMetrics::Backend::STORE, n, ok ? recordDiskBytes(n, 2) : 0,
                                            t > t0 ? t - t0 : 0, ok);
            }
        };
        store->write(LLFileSystemStore::key(id), LLCache::WriteSource::take(std::move(bytes)), o, done.get());
    }

    // The staged bytes of `s` become the record of `final_id` (empty = the asset is absent,
    // like an empty file). Staging mutex held.
    void commitLocked(LLCache::Store* store, Staged& s, const LLUUID& final_id, LLAssetType::EType final_type)
    {
        if (s.bytes.empty())
        {
            store->remove(LLFileSystemStore::key(final_id));
            return;
        }
        account(-(S64)s.bytes.size());
        writeRecord(store, final_id, final_type, std::move(s.bytes), LLCache::WF_PIN_SESSION, LLCache::Priority::NORMAL,
                    s.t0 ? s.t0 : nowUsec(), true);
        s.bytes.clear();
    }

    // Commit closed staging whose grace ran out (or all closed staging when over the RAM cap).
    void pumpLocked(LLCache::Store* store, bool force)
    {
        const F64 t = nowSec();
        if (!force && t < sNextPump) return;
        sNextPump = t + 0.5;
        const bool over = sStagedBytes > sOpt.max_staged_bytes;
        for (auto it = sStaging.begin(); it != sStaging.end();)
        {
            Staged& s = it->second;
            if (s.writers == 0 && (over || t - s.closed_at >= sOpt.commit_grace_s))
            {
                commitLocked(store, s, it->first, s.type);
                it = sStaging.erase(it);
            }
            else ++it;
        }
    }
}

namespace LLFileSystemStore
{

Access::Access()
{
    sStoreMutex.lock_shared();
    mStore = sStore;
    if (!mStore) sStoreMutex.unlock_shared();
}

Access::~Access()
{
    if (mStore) sStoreMutex.unlock_shared();
}

void attach(LLCache::Store* store, const Options& opt)
{
    detach();
    if (!store) return;
    std::unique_lock<std::shared_mutex> lock(sStoreMutex);
    sStore = store;
    sOpt = opt;
    sLegacyFallback = opt.legacy_fallback;
    sReadyDeadline = nowSec() + (F64)opt.ready_wait_ms / 1000.0;
    sActive = true;
    LL_INFOS("AssetCache") << "Asset cache: in the cache store " << store->dir() << " (kinds asset + mesh)"
                           << (store->readOnly() ? ", read-only + private overflow (second instance)" : "")
                           << (opt.legacy_fallback ? "; old files still served until migrated" : "") << LL_ENDL;
}

void detach()
{
    std::unique_lock<std::shared_mutex> lock(sStoreMutex);
    if (!sStore) return;
    {
        std::lock_guard<std::mutex> sl(sStagingMutex);
        U32 n = 0;
        for (auto& kv : sStaging)
        {
            if (kv.second.bytes.empty()) continue;
            commitLocked(sStore, kv.second, kv.first, kv.second.type);
            ++n;
        }
        sStaging.clear();
        sStagedBytes = 0;
        if (n) LL_INFOS("AssetCache") << "Asset cache: committed " << n << " staged writes at shutdown" << LL_ENDL;
    }
    LL_INFOS("AssetCache") << "Asset cache (store, shutdown): " << statsLine() << LL_ENDL;
    sStore = nullptr;
    sActive = false;
}

bool active()
{
    return sActive.load();
}

void setLegacyFallback(bool on)
{
    sLegacyFallback = on;
}

bool legacyFallback()
{
    return sLegacyFallback.load();
}

void setPlacement(const LLUUID& region, U64 grid_handle)
{
    std::lock_guard<std::mutex> lock(sPlacementMutex);
    sPlacement = region.isNull() ? LLCache::PlacementHint::shared() : LLCache::PlacementHint::inRegion(region, grid_handle);
}

LLCache::PlacementHint placement()
{
    std::lock_guard<std::mutex> lock(sPlacementMutex);
    return sPlacement;
}

void pump()
{
    Access a;
    if (!a) return;
    std::lock_guard<std::mutex> sl(sStagingMutex);
    pumpLocked(a.get(), false);
}

Stats stats()
{
    Stats s;
    {
        std::lock_guard<std::mutex> sl(sStagingMutex);
        s.staged = (U32)sStaging.size();
        s.staged_bytes = sStagedBytes;
        s.peak_staged_bytes = sPeakStaged;
    }
    s.commits = sCommits; s.commit_bytes = sCommitBytes; s.commit_fail = sCommitFail;
    s.imported = sImported; s.imported_bytes = sImportedBytes; s.corrupt = sCorrupt;
    return s;
}

std::string statsLine()
{
    const Stats s = stats();
    return llformat("staged %u (%.1f MB, peak %.1f MB), committed %llu (%.1f MB, %llu failed), imported old files %llu (%.1f MB), corrupt %llu",
                    s.staged, (F64)s.staged_bytes / 1048576.0, (F64)s.peak_staged_bytes / 1048576.0,
                    (unsigned long long)s.commits, (F64)s.commit_bytes / 1048576.0, (unsigned long long)s.commit_fail,
                    (unsigned long long)s.imported, (F64)s.imported_bytes / 1048576.0, (unsigned long long)s.corrupt);
}

bool waitReady(LLCache::Store* store)
{
    if (!store) return false;
    if (store->ready()) return true;
    const F64 left = sReadyDeadline.load() - nowSec();
    if (left <= 0.0) return false;
    return store->waitReady(std::chrono::milliseconds((S64)(left * 1000.0) + 1));
}

std::string legacyPath(const LLUUID& id)
{
    return LLDiskCache::metaDataToFilepath(id, LLAssetType::AT_NONE);
}

LLCache::Key key(const LLUUID& id)
{
    return LLCache::Key(id, LLCache::Kind::ASSET, 0);
}

S32 size(const LLUUID& id)
{
    Access a;
    if (!a) return 0;
    {
        std::lock_guard<std::mutex> sl(sStagingMutex);
        auto it = sStaging.find(id);
        if (it != sStaging.end()) return (S32)std::min<size_t>(it->second.bytes.size(), (size_t)S32_MAX);
    }
    waitReady(a.get());
    LLCache::RecordInfo ri;
    if (a->lookup(key(id), &ri)) return (S32)std::min<U32>(ri.raw_bytes, (U32)S32_MAX);
    if (sLegacyFallback)
    {
        llstat st;
        if (LLFile::stat(legacyPath(id), &st) == 0 && st.st_size > 0) return (S32)std::min<S64>((S64)st.st_size, S32_MAX);
    }
    return 0;
}

ReadFrom readAll(const LLUUID& id, LLAssetType::EType type, std::vector<U8>& out)
{
    out.clear();
    Access a;
    if (!a) return ReadFrom::NONE;
    {
        std::lock_guard<std::mutex> sl(sStagingMutex);
        auto it = sStaging.find(id);
        if (it != sStaging.end())
        {
            out = it->second.bytes;
            return out.empty() ? ReadFrom::NONE : ReadFrom::STAGED;
        }
    }
    waitReady(a.get());
    ReadFrom res = ReadFrom::NONE;
    LLCache::ReadResult r = a->readSync(key(id));
    if (r.status == LLCache::Status::OK && !r.bytes.empty())
    {
        out = std::move(r.bytes);
        return ReadFrom::STORE;
    }
    if (r.status == LLCache::Status::CORRUPT)
    {
        // The store dropped the bad record; the consumer fetches the asset again.
        sCorrupt++;
        LLCacheMetrics::recordCorrupt(metricsCache(type), LLCacheMetrics::Backend::STORE);
        LL_WARNS_ONCE("AssetCache") << "Dropped a bad asset record (hash mismatch): " << id << LL_ENDL;
        res = ReadFrom::CORRUPT;
    }
    if (sLegacyFallback && readLegacyFile(id, out))
    {
        // Not converted yet: serve it and import it now (the migration then finds the key
        // and deletes the file). A second instance imports into its private overflow.
        sImported++;
        sImportedBytes += out.size();
        std::vector<U8> copy = out;
        writeRecord(a.get(), id, type, std::move(copy), LLCache::WF_NONE, LLCache::Priority::NORMAL, nowUsec(), false);
        return ReadFrom::LEGACY;
    }
    return res;
}

bool beginWrite(const LLUUID& id, LLAssetType::EType type, bool truncate)
{
    Access a;
    if (!a) return false;
    std::lock_guard<std::mutex> sl(sStagingMutex);
    auto it = sStaging.find(id);
    const bool created = it == sStaging.end();
    Staged& s = created ? sStaging[id] : it->second;
    s.writers++;
    s.type = type;
    if (!s.t0) s.t0 = nowUsec();
    if (truncate)
    {
        account(-(S64)s.bytes.size());
        s.bytes.clear();
    }
    else if (created)
    {
        // Continue what is committed (READ_WRITE / APPEND on an existing asset).
        waitReady(a.get());
        LLCache::ReadResult r = a->readSync(key(id));
        if (r.status == LLCache::Status::OK) s.bytes = std::move(r.bytes);
        else if (sLegacyFallback) readLegacyFile(id, s.bytes);
        account((S64)s.bytes.size());
    }
    return true;
}

void endWrite(const LLUUID& id, bool commit_now)
{
    Access a;
    if (!a) return;
    std::lock_guard<std::mutex> sl(sStagingMutex);
    auto it = sStaging.find(id);
    if (it != sStaging.end())
    {
        Staged& s = it->second;
        if (s.writers) s.writers--;
        if (s.writers == 0)
        {
            if (commit_now)
            {
                commitLocked(a.get(), s, id, s.type);
                sStaging.erase(it);
            }
            else
            {
                s.closed_at = nowSec();
            }
        }
    }
    pumpLocked(a.get(), false);
}

bool write(const LLUUID& id, S64 pos, const U8* p, S32 n, S32& new_pos)
{
    std::lock_guard<std::mutex> sl(sStagingMutex);
    Staged& s = sStaging[id];
    const size_t before = s.bytes.size();
    const size_t at = pos < 0 ? before : (size_t)pos;
    const size_t len = n > 0 ? (size_t)n : 0;
    if ((U64)at + len > LLCacheCodec::getMaxRecordSize())
    {
        new_pos = (S32)std::min<size_t>(at, (size_t)S32_MAX);
        return false;
    }
    try
    {
        if (at + len > s.bytes.size()) s.bytes.resize(at + len, 0);
    }
    catch (const std::bad_alloc&)
    {
        new_pos = (S32)std::min<size_t>(at, (size_t)S32_MAX);
        return false;
    }
    if (len) memcpy(s.bytes.data() + at, p, len);
    account((S64)s.bytes.size() - (S64)before);
    new_pos = (S32)std::min<size_t>(at + len, (size_t)S32_MAX);
    return true;
}

bool setSize(const LLUUID& id, S32 size)
{
    if (size < 0) return false;
    std::lock_guard<std::mutex> sl(sStagingMutex);
    Staged& s = sStaging[id];
    const size_t before = s.bytes.size();
    try { s.bytes.resize((size_t)size, 0); }
    catch (const std::bad_alloc&) { return false; }
    account((S64)s.bytes.size() - (S64)before);
    return true;
}

bool remove(const LLUUID& id)
{
    Access a;
    if (!a) return false;
    {
        std::lock_guard<std::mutex> sl(sStagingMutex);
        auto it = sStaging.find(id);
        if (it != sStaging.end()) eraseStaged(it);
    }
    a->remove(key(id));
    if (sLegacyFallback && !a->readOnly())
    {
        // So the migration never imports what the viewer deleted.
        LLFile::remove(legacyPath(id), ENOENT);
    }
    return true;
}

bool rename(const LLUUID& from, const LLUUID& to, LLAssetType::EType to_type)
{
    Access a;
    if (!a) return false;
    if (from == to) return true;
    std::lock_guard<std::mutex> sl(sStagingMutex);
    // The destination goes first, as the old rename removed the target file.
    auto dst = sStaging.find(to);
    if (dst != sStaging.end()) eraseStaged(dst);
    auto it = sStaging.find(from);
    if (it != sStaging.end())
    {
        // The download flow: the staged bytes become the asset's record.
        Staged s = std::move(it->second);
        sStaging.erase(it);
        commitLocked(a.get(), s, to, to_type);
        a->remove(key(from));   // a record the staging was seeded from
        return true;
    }
    waitReady(a.get());
    if (a->lookup(key(from)))
    {
        if (a->rename(key(from), key(to))) return true;
        // Not re-keyable (e.g. RAM-only fallback): copy.
        LLCache::ReadResult r = a->readSync(key(from));
        if (r.status == LLCache::Status::OK)
        {
            writeRecord(a.get(), to, to_type, std::move(r.bytes), LLCache::WF_PIN_SESSION, LLCache::Priority::NORMAL,
                        nowUsec(), true);
            a->remove(key(from));
            return true;
        }
    }
    std::vector<U8> bytes;
    if (sLegacyFallback && readLegacyFile(from, bytes))
    {
        writeRecord(a.get(), to, to_type, std::move(bytes), LLCache::WF_PIN_SESSION, LLCache::Priority::NORMAL, nowUsec(),
                    true);
        if (!a->readOnly()) LLFile::remove(legacyPath(from), ENOENT);
        return true;
    }
    LL_DEBUGS("AssetCache") << "rename " << from << " -> " << to << ": nothing cached under the old id" << LL_ENDL;
    a->remove(key(to));
    return true;
}

} // namespace LLFileSystemStore
