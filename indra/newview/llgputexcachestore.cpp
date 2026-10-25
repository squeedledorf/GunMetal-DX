/**
 * @file llgputexcachestore.cpp
 * @brief The segment-store backend of LLGpuTexCache (#715 / #708 / #710 / #865). See
 *        llgputexcachestore.h and llgputexrecord.h (record layout).
 *
 * What changes against the file backend (llgputexcache.cpp):
 * - per (UUID, mask variant) a tail record plus one record per level above 256 px in
 *   <cache>/store instead of one .gtx file (#865 layout v2; v1 whole-chain records are
 *   still read and rewritten as v2 in the background); payload zstd-compressed by the
 *   store's GPUTEX kind policy (#710);
 * - a read fetches only the records the request needs (the levels from the requested
 *   top down to what is resident, the tail only if it is needed), concurrently at the
 *   renderer's priority; the result is handed over as pieces (no reassembly copy);
 * - the index lives in the store (loaded on its writer thread, snapshot + tail scan): no
 *   folder scan, no file-name parsing, no mtime writes on use (touch is RAM only and
 *   saved with the index snapshot, #709);
 * - eviction is the store's (TEXTURES group budget = SlipstreamGpuTexCacheSizeMB for now,
 *   GPU-ready evicted before J2C once J2C joins, #705; per record, so a chain loses its
 *   top levels first -- see llgputexrecord.h);
 * - the capture's mapped staging buffer is handed over as borrowed spans (one per
 *   record): the store's writer copies them off and the renderer's `released` flag is set
 *   once every span has been copied (as the old worker did), so the render thread never
 *   copies.
 * What stays: the static API, main-thread use, the in-flight bounds (reads 24 / 96 MB,
 * ready results expire after 10 s, write copies 64 MB), a second instance never writes.
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

#include "llgputexcachestore.h"

#include "llcachemetrics.h"
#include "llcachestore.h"
#include "llcachestore_format.h"
#include "llcachestore_workqueue.h"
#include "llformat.h"
#include "llgputexrecord.h"
#include "sltexturetiers.h"
#include "workqueue.h"

#include "boost/unordered_map.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

namespace
{
    using LLGpuTexStore::Key;
    using LLGpuTexStore::Info;
    using LLGpuTexStore::Blob;

    constexpr F64 MB = 1024.0 * 1024.0;
    // Same bounds as the file backend (bounded RAM: every read result / write copy is a
    // transient buffer).
    constexpr U32 MAX_READS_IN_FLIGHT = 24;
    constexpr U64 MAX_READ_BYTES = 96ull << 20;     // in flight + ready, not yet taken
    constexpr U64 MAX_WRITE_BYTES = 64ull << 20;    // handed to the store, not yet on disk
    constexpr F64 READY_EXPIRE_SEC = 10.0;          // a result nobody took is dropped

    struct ReadSlot
    {
        LLGpuTexCache::EReadState mState = LLGpuTexCache::READ_PENDING;
        // The chain the read was planned from and the records it reads (one ticket each).
        LLGpuTexRecord::Chain mChain;
        LLGpuTexRecord::ReadPlan mPlan;
        std::vector<LLCache::Ticket<LLCache::ReadResult>> mTickets;
        std::shared_ptr<Blob> mBlob;
        U64 mBytes = 0;          // reserved (pending: the planned records; ready: the pieces)
        F64 mReadyAt = 0.0;
        // #715 metrics: request time, and the store's completion time of the last record
        // (stamped inline on its reader thread by the tickets' `wake`, usec since the
        // steady epoch).
        U64 mRequestUsec = 0;
        std::shared_ptr<std::atomic<U64>> mDoneUsec;
    };

    using LLCacheMetrics::Cache;
    using LLCacheMetrics::Backend;

    U64 steady_usec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Request -> the store finished the read (the main thread's pickup delay is
    // frame-coupled and the same for both backends).
    U64 read_usec(const ReadSlot& s)
    {
        const U64 done = s.mDoneUsec ? s.mDoneUsec->load(std::memory_order_relaxed) : 0;
        const U64 end = done ? done : steady_usec();
        return end > s.mRequestUsec ? end - s.mRequestUsec : 0;
    }

    LLCache::Store* sStore = nullptr;
    bool sActive = false;
    bool sEnabled = true;
    U64 sBudget = 3072ull << 20;
    U32 sSessionStart = 0;
    U32 sGeneration = 0;         // completions of an earlier init / shutdown are ignored
    std::string sDir;
    U64 sPeak = 0;
    U64 sHits = 0, sMisses = 0, sCorrupt = 0, sExpired = 0, sBytesRead = 0;
    U64 sWrites = 0, sWriteFail = 0, sWriteSkipped = 0, sBytesWritten = 0;
    // #991: writes handed to the store by kind: top below the asset's full size (partial)
    // or at it (full); upgraded = over a smaller chain of the same encoding (either kind).
    U64 sWritePartial = 0, sWriteFull = 0, sWriteUpgraded = 0;
    bool sLoggedPartial = false;
    U64 sReadBytes = 0;          // pending + ready reservations
    U32 sReadsPending = 0;
    // Bytes handed to the store and not yet appended. Decremented by the store's
    // completion `wake` (inline on its writer thread), so it can never leak even if the
    // main loop's queue is gone.
    std::atomic<U64> sWriteBytes{ 0 };
    boost::unordered_map<Key, ReadSlot> sReads;
    // #865: v1 (whole-chain) records that were read are rewritten as v2 in the background.
    LLGpuTexRecord::Rewriter sRewriter;

    F64 now_sec()
    {
        return std::chrono::duration<F64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Whether the store holds any v1 (whole-chain) record: decided once per init by a scan
    // of the index on a helper thread (nothing writes v1 any more), so a store converted
    // from .gtx files or fully rewritten never pays the v1 probe on its lookups. Until the
    // scan is done every lookup probes v1 too.
    std::atomic<bool> sV1Absent{ false };
    std::atomic<bool> sV1ScanStop{ false };
    struct ScanThread
    {
        std::thread t;
        bool joinable() const { return t.joinable(); }
        void join() { t.join(); }
        ScanThread& operator=(std::thread&& o) { t = std::move(o); return *this; }
        ~ScanThread() { sV1ScanStop = true; if (t.joinable()) t.join(); }   // (exit without shutdown())
    };
    ScanThread sV1Scan;

    void startV1Scan(LLCache::Store* store)
    {
        sV1Absent = false;
        sV1ScanStop = false;
        sV1Scan = std::thread([store]()
        {
            LL_PROFILER_SET_THREAD_NAME("GpuTexV1Scan");
            while (!sV1ScanStop && !store->ready()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (sV1ScanStop) return;
            bool any = false;
            store->forEach(LLCache::Kind::GPUTEX, [&any](const LLCache::RecordInfo& r) -> bool
            {
                any = LLGpuTexRecord::partOf(r.key.variant) == LLGpuTexRecord::Part::WHOLE;
                return !any && !sV1ScanStop;   // (stop at the first)
            });
            if (sV1ScanStop) return;
            sV1Absent = !any;
            LL_INFOS("GpuTexCache") << "GPU-ready store: " << (any ? "v1 whole-chain records present (rewritten per level as they are read)"
                                                                   : "per-level records only") << LL_ENDL;
        });
    }

    void stopV1Scan()
    {
        sV1ScanStop = true;
        if (sV1Scan.joinable()) sV1Scan.join();
    }

    bool resolveChain(const Key& id, LLGpuTexRecord::Chain& c)
    {
        return sStore && LLGpuTexRecord::resolve(*sStore, id.id, id.mask, c, !sV1Absent.load(std::memory_order_relaxed));
    }

    // Touch (and, for a reuse, note) the records a read uses: the tail and every level
    // from the first one read down. Unread levels above keep their older access, so the
    // eviction score drops the levels nobody needed first.
    void touchUsed(const LLGpuTexRecord::Chain& c, U32 from_level, bool reuse)
    {
        for (const LLGpuTexRecord::Chain::Rec& r : c.recs)
        {
            if (r.level + r.levels <= from_level) continue;
            if (reuse) sStore->noteReuse(r.key);
            sStore->touch(r.key);
        }
    }

    void noteBadRead(const Key& id, const char* why)
    {
        LL_WARNS_ONCE("GpuTexCache") << "Discarding bad GPU-ready record for " << id.id << " (" << why << ")" << LL_ENDL;
        ++sCorrupt;
        LLCacheMetrics::recordCorrupt(Cache::GPUTEX, Backend::STORE);
    }

    // A finished read -> READY / FAILED (main thread). Every record of the plan must be
    // done; one bad record fails the read (the requester falls back to the J2C path) and
    // drops only that record -- a bad tail takes its (now unreachable) levels with it.
    void finishRead(const Key& id, ReadSlot& s)
    {
        if (s.mState != LLGpuTexCache::READ_PENDING) return;
        for (const auto& t : s.mTickets) if (!t.isDone()) return;
        std::vector<LLCache::ReadResult> results;
        results.reserve(s.mTickets.size());
        for (auto& t : s.mTickets) results.push_back(t.take());
        s.mTickets.clear();
        const U64 lat = read_usec(s);
        if (sReadsPending) --sReadsPending;
        sReadBytes -= llmin(sReadBytes, s.mBytes);
        s.mBytes = 0;
        s.mReadyAt = now_sec();

        std::shared_ptr<Blob> blob = std::make_shared<Blob>();
        size_t bad = 0;
        if (LLGpuTexRecord::assemble(s.mChain, s.mPlan, results, *blob, bad))
        {
            s.mState = LLGpuTexCache::READ_READY;
            s.mBytes = blob->bytes();
            sReadBytes += s.mBytes;
            sBytesRead += s.mBytes;
            LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, true, s.mBytes, lat);
            // A whole-chain v1 record, fully decoded anyway: rewrite it per level.
            if (s.mChain.v1) sRewriter.offer(s.mChain, blob, id.mask);
            s.mBlob = std::move(blob);
            return;
        }
        s.mState = LLGpuTexCache::READ_FAILED;
        if (bad >= results.size() || bad >= s.mPlan.recs.size())
        {
            LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, false, 0, lat);
            return;
        }
        const LLGpuTexRecord::Chain::Rec& rec = s.mChain.recs[s.mPlan.recs[bad]];
        const bool tail = !s.mChain.v1 && rec.level == s.mChain.tail_level;
        const LLCache::Status st = results[bad].status;
        LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, false, 0, lat);
        if (st == LLCache::Status::OK)
        {
            // Read fine but not what the plan expected: a v1 record of another meta version /
            // mask is dropped; a v2 record was replaced since the lookup (a newer capture), so
            // it is left alone and the next lookup sees the new chain.
            if (s.mChain.v1)
            {
                noteBadRead(id, "metadata version / mask variant mismatch");
                sStore->remove(rec.key);
            }
        }
        else if (st == LLCache::Status::CORRUPT)
        {
            noteBadRead(id, "hash / codec mismatch");   // the store already dropped that record
            if (tail) LLGpuTexRecord::removeLevels(*sStore, id.id, id.mask);   // (unreachable without it)
        }
        else if (st != LLCache::Status::MISSING && st != LLCache::Status::CANCELLED)
        {
            // I/O error: never trusted again this session (like a bad .gtx file).
            noteBadRead(id, LLCache::statusName(st));
            sStore->remove(rec.key);
            if (tail) LLGpuTexRecord::removeLevels(*sStore, id.id, id.mask);
        }
        // (MISSING: evicted / removed since the lookup -- a miss; the next lookup sees what is left)
    }
}

namespace LLGpuTexStore
{

bool active()
{
    return sActive;
}

void init(LLCache::Store* store, bool enabled, U64 budget_bytes)
{
    sReads.clear();
    sReadBytes = 0;
    sReadsPending = 0;
    sStore = store;
    sActive = store != nullptr;
    sEnabled = enabled;
    sSessionStart = (U32)std::time(nullptr);
    ++sGeneration;
    sDir = store ? store->dir() : std::string();
    sRewriter.clear();
    sRewriter.setStore(store);
    stopV1Scan();
    if (!sActive) return;
    startV1Scan(store);
    setBudget(budget_bytes);
    LL_INFOS("GpuTexCache") << "GPU texture cache: in the cache store " << sDir << " (kind gputex, zstd, per-level records), budget "
                            << (budget_bytes >> 20) << " MB" << (enabled ? "" : " (disabled)")
                            << (store->readOnly() ? ", read-only (second instance)" : "")
                            << (store->ready() ? "" : "; index loading on the store's writer") << LL_ENDL;
}

void shutdown()
{
    if (!sActive) return;
    logStats("shutdown");
    for (auto& kv : sReads)
        for (auto& t : kv.second.mTickets) t.cancel();
    sReads.clear();
    sReadBytes = 0;
    sReadsPending = 0;
    sRewriter.clear();
    sRewriter.setStore(nullptr);
    stopV1Scan();
    ++sGeneration;
    sActive = false;
    sStore = nullptr;
}

bool isEnabled()
{
    return sActive && sEnabled && sStore && sStore->ready();
}

void setEnabled(bool enabled)
{
    sEnabled = enabled;
}

void poll()
{
    if (!sActive) return;
    const F64 t = now_sec();
    if (sStore && sStore->ready()) sRewriter.pump(t);
    if (sReads.empty()) return;
    for (auto it = sReads.begin(); it != sReads.end(); )
    {
        ReadSlot& s = it->second;
        finishRead(it->first, s);
        // Results nobody took (the texture went away, or was served another way).
        const bool stale = (s.mState == LLGpuTexCache::READ_READY || s.mState == LLGpuTexCache::READ_FAILED)
                        && t - s.mReadyAt > READY_EXPIRE_SEC;
        if (stale)
        {
            if (s.mState == LLGpuTexCache::READ_READY) { sReadBytes -= llmin(sReadBytes, s.mBytes); ++sExpired; }
            it = sReads.erase(it);
        }
        else ++it;
    }
}

bool lookup(const Key& id, Info& out)
{
    if (!isEnabled()) return false;
    LLGpuTexRecord::Chain c;
    if (!resolveChain(id, c)) return false;
    LLGpuTexRecord::infoFrom(c, out);
    return out.w && out.h;
}

void touch(const Key& id)
{
    LLGpuTexRecord::Chain c;
    if (sActive && resolveChain(id, c)) LLGpuTexRecord::touchChain(*sStore, c, false);
}

void remove(const Key& id)
{
    if (!sActive || !sStore) return;
    sRewriter.cancel(id.id, id.mask);
    LLGpuTexRecord::removeAll(*sStore, id.id, id.mask);
}

bool requestRead(const Key& id, LLGpuTexCache::EReadPriority prio, U32 first_level, U32 resident_top)
{
    if (!isEnabled()) return false;
    if (sReads.count(id)) return false;
    LLGpuTexRecord::Chain c;
    LLGpuTexRecord::ReadPlan plan;
    if (!resolveChain(id, c) || !LLGpuTexRecord::planRead(c, first_level, resident_top, plan)) return false;
    const U64 bytes = plan.bytes + sizeof(LLGpuTexHeader);
    // Slots held back for more urgent reads: a burst of NORMAL upgrades must not fill
    // every slot and refuse the visible / self reads the store would serve first.
    const U32 slots = prio >= LLGpuTexCache::READ_PRIO_URGENT ? MAX_READS_IN_FLIGHT
                    : prio == LLGpuTexCache::READ_PRIO_HIGH   ? MAX_READS_IN_FLIGHT - 4
                    : prio == LLGpuTexCache::READ_PRIO_NORMAL ? MAX_READS_IN_FLIGHT - 8
                                                              : MAX_READS_IN_FLIGHT / 2;
    if (sReadsPending >= slots) return false;
    if (sReadBytes > 0 && sReadBytes + bytes > MAX_READ_BYTES) return false;
    // #705 admission data: a second read in one session (the image left VRAM and is
    // needed again) is a reuse. The first touch of a record from an earlier session is
    // counted by the store itself.
    touchUsed(c, plan.first_level, c.last_access >= sSessionStart);
    // TODO(#708 placement): the texture's region when cheaply known (the streamer does
    // not carry one today); placement is only a write-side hint, reads are by key.
    ReadSlot& s = sReads[id];
    s.mState = LLGpuTexCache::READ_PENDING;
    s.mRequestUsec = steady_usec();
    s.mDoneUsec = std::make_shared<std::atomic<U64>>(0);
    const LLCache::Priority p = (LLCache::Priority)std::min<U8>((U8)prio, (U8)LLCache::Priority::URGENT);
    // Every record of the plan at once (the store's readers serve them in parallel, by
    // priority); the last one to finish stamps the completion time.
    s.mTickets.reserve(plan.recs.size());
    for (size_t i : plan.recs)
    {
        LLCache::Completion cpl;
        cpl.wake = [done = s.mDoneUsec]() { done->store(steady_usec(), std::memory_order_relaxed); };
        s.mTickets.push_back(sStore->read(c.recs[i].key, p, std::move(cpl)));
    }
    s.mChain = std::move(c);
    s.mPlan = std::move(plan);
    s.mBytes = bytes;
    sReadBytes += bytes;
    ++sReadsPending;
    return true;
}

LLGpuTexCache::EReadState readState(const Key& id)
{
    auto it = sReads.find(id);
    if (it == sReads.end()) return LLGpuTexCache::READ_NONE;
    finishRead(id, it->second);   // a read that finished since poll() is served this pass
    return it->second.mState;
}

std::shared_ptr<Blob> takeRead(const Key& id, bool& failed)
{
    failed = false;
    auto it = sReads.find(id);
    if (it == sReads.end()) return nullptr;
    ReadSlot& s = it->second;
    finishRead(id, s);
    if (s.mState == LLGpuTexCache::READ_FAILED)
    {
        failed = true;
        sReads.erase(it);
        return nullptr;
    }
    if (s.mState != LLGpuTexCache::READ_READY) return nullptr;
    std::shared_ptr<Blob> b = std::move(s.mBlob);
    sReadBytes -= llmin(sReadBytes, s.mBytes);
    sReads.erase(it);
    ++sHits;
    return b;
}

void noteMiss()
{
    ++sMisses;
    // An index miss: no I/O, ~0 latency (compare backends on hit latency).
    LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, false, 0, 0);
}

bool canWrite(U64 bytes)
{
    // A second instance (read-only store) writes nothing, as before: its private
    // overflow is discarded at exit, so GPU-ready writes there would be wasted wear.
    if (!isEnabled() || sStore->readOnly()) return false;
    if (bytes == 0 || bytes > sBudget) return false;
    const U64 inflight = sWriteBytes.load();
    return inflight == 0 || inflight + bytes <= MAX_WRITE_BYTES;
}

bool write(const LLGpuTexHeader& hdr, const U8* src, U64 bytes, const std::shared_ptr<std::atomic<bool>>& released)
{
    if (!src || !canWrite(bytes)) return false;
    LLUUID uuid;
    memcpy(uuid.mData, hdr.id, 16);
    const bool mask = (hdr.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0;

    // "Finer version wins": a capture of the store chain's encoding is written only when
    // it is bigger, and then only its levels above the chain's top. Another encoder
    // version, level or alpha-mip state always replaces: the old chain could not be served
    // any more.
    // #991: ALWAYS SAVE WHAT WE HAVE. A capture is the resident image's chain from its top
    // down to 1 x 1; its top may be below the asset's full size (a higher discard: PARTIAL).
    // It is written all the same, and a later, larger capture upgrades the entry in place.
    U32 above = 0, same_top = 0;
    const LLGpuTexRecord::WriteMode mode = LLGpuTexRecord::writeMode(*sStore, hdr, above, &same_top);
    if (mode == LLGpuTexRecord::WriteMode::SKIP)
    {
        ++sWriteSkipped;   // nothing written: not a write, not a failure
        return false;
    }
    std::vector<LLGpuTexRecord::PartPlan> parts;
    if (!LLGpuTexRecord::planWrite(hdr, bytes, parts, above))
    {
        // (the renderer always captures whole chains of known formats: not expected)
        LL_WARNS_ONCE("GpuTexCache") << "GPU-ready capture of " << uuid << " not written: " << hdr.w << "x" << hdr.h
                                     << ", " << hdr.levels << " levels (expected "
                                     << LLGpuTexRecord::mipCount(hdr.w, hdr.h) << "), format " << hdr.vk_format << ", "
                                     << bytes << " bytes (expected "
                                     << LLGpuTexRecord::chainBytes(hdr.vk_format, hdr.w, hdr.h, hdr.levels)
                                     << "), mode " << (U32)mode << " above " << above << LL_ENDL;
        ++sWriteFail;
        return false;
    }
    const bool partial = hdr.full_w && hdr.full_h && (hdr.w < hdr.full_w || hdr.h < hdr.full_h);
    const bool upgrade = same_top > 0;   // a smaller chain of this encoding grows in place

    // One logical write: the stats / metrics count it once, when its last record is done.
    struct Agg
    {
        size_t handed = 0, done = 0;
        bool fail = false;
        U64 bytes = 0, disk = 0, t_submit = 0;
        std::shared_ptr<std::atomic<U64>> t_done;
        std::vector<LLCache::Ticket<LLCache::Status>> tickets;
        std::vector<LLCache::Key> keys;
    };
    auto agg = std::make_shared<Agg>();
    agg->bytes = bytes;
    agg->t_submit = steady_usec();   // #715 metrics: submit -> the last record is appended
    agg->t_done = std::make_shared<std::atomic<U64>>(0);
    // The renderer's buffer is free once the store has copied every span off it.
    auto pending = std::make_shared<std::atomic<U32>>(1);   // + one per handed span; 1 = this call
    std::function<void()> release_part = [pending, released]()
    {
        if (pending->fetch_sub(1) == 1 && released) released->store(true, std::memory_order_release);
    };
    const U32 gen = sGeneration;
    LLCache::WriteOptions o;
    o.priority = LLCache::Priority::NORMAL;
    // TODO(#708 placement): the region of the objects using this texture when cheaply
    // known; SHARED for now (a wrong / missing hint only costs locality).
    o.placement = LLCache::PlacementHint::shared();
    o.completion.post = LLCache::postTo(LL::WorkQueue::getInstance("mainloop"));   // stats on the main loop
    auto per_part = [&parts, agg, pending, gen](size_t i, LLCache::WriteOptions& wo)
    {
        const U64 pb = parts[i].bytes;
        pending->fetch_add(1);
        agg->keys.push_back(parts[i].key);
        sWriteBytes += pb;   // before write(): a refusal runs `wake` inline
        auto t_done = agg->t_done;
        wo.completion.wake = [pb, t_done]()
        {
            sWriteBytes.fetch_sub(pb);
            t_done->store(steady_usec(), std::memory_order_relaxed);
        };
        wo.completion.notify = [agg, i, gen]()
        {
            if (gen != sGeneration) return;
            // (a refused record has no ticket: the write refused it inline)
            const LLCache::Status st = i < agg->tickets.size() ? agg->tickets[i].take() : LLCache::Status::REFUSED_BUSY;
            if (st == LLCache::Status::OK)
            {
                // disk = the record as appended (header + meta + zstd payload, 16-byte
                // aligned), from the index; 0 when a newer write already replaced it.
                LLCache::RecordInfo ri;
                if (sStore && sStore->lookup(agg->keys[i], &ri) && !ri.queued && !ri.staged)
                    agg->disk += LLCacheStoreFormat::recordSize(ri.meta_len, ri.stored_bytes);
            }
            else if (st != LLCache::Status::SKIPPED) agg->fail = true;
            if (++agg->done < agg->handed) return;
            const U64 end = agg->t_done->load(std::memory_order_relaxed);
            const U64 usec = end > agg->t_submit ? end - agg->t_submit : 0;
            if (!agg->fail)
            {
                ++sWrites; sBytesWritten += agg->bytes;
                LLCacheMetrics::recordWrite(Cache::GPUTEX, Backend::STORE, agg->bytes, agg->disk, usec, true);
            }
            else
            {
                ++sWriteFail;
                LLCacheMetrics::recordWrite(Cache::GPUTEX, Backend::STORE, agg->bytes, 0, usec, false);
            }
        };
    };
    // Levels largest first, the tail last (only if every level was accepted). The store
    // copies each span off on its writer thread, then runs its release (exactly once, also
    // when refused).
    LLGpuTexRecord::PartWrite pw = LLGpuTexRecord::writeParts(*sStore, parts, src, o, release_part, per_part);
    agg->handed = pw.queued;
    agg->tickets = std::move(pw.tickets);
    release_part();   // this call's reference: `released` is set once every span is copied
    if (pw.refused == LLCache::Status::OK)
    {
        // #991 counters: every record of the chain was accepted (they land in the store's
        // order; a failure on the writer still shows as "fail").
        if (upgrade) ++sWriteUpgraded;
        if (partial) ++sWritePartial; else ++sWriteFull;
        if (!sLoggedPartial && partial && upgrade)
        {
            sLoggedPartial = true;
            LL_INFOS("GpuTexCache") << "GPU-ready cache: saving partial chains (#991), e.g. " << uuid << " " << same_top
                                    << " px -> " << hdr.w << "x" << hdr.h << " of " << hdr.full_w << "x" << hdr.full_h
                                    << " (" << (mode == LLGpuTexRecord::WriteMode::ABOVE ? "levels above" : "whole chain")
                                    << "); counted in the t2 stats line (wr part / up / full)" << LL_ENDL;
        }
    }
    if (pw.refused == LLCache::Status::OK && mode == LLGpuTexRecord::WriteMode::FULL)
    {
        // The new chain supersedes a v1 record and any larger levels of another capture.
        sRewriter.cancel(uuid, mask);
        LLGpuTexRecord::removeStale(*sStore, uuid, mask, std::max<U32>(hdr.w, hdr.h));
    }
    return pw.queued > 0;
}

void setBudget(U64 budget_bytes)
{
    sBudget = budget_bytes;
    // #705: the TEXTURES group is shared with J2C; SLTextureTiers turns this into the GPUTEX
    // share cap (or the whole group budget while J2C is not in the store).
    if (sActive && sStore && budget_bytes) SLTextureTiers::instance().setGpuTexBudget(budget_bytes);
}

LLGpuTexCache::Stats getStats()
{
    LLGpuTexCache::Stats s;
    if (!sActive || !sStore) return s;
    const LLCache::Stats st = sStore->stats();
    const LLCache::KindStats& ks = st.kinds[(size_t)LLCache::Kind::GPUTEX];
    s.used = ks.live_bytes;
    s.budget = sBudget;
    sPeak = llmax(sPeak, s.used);
    s.peak = sPeak;
    s.entries = ks.records;   // records, not textures (#865: a tail + one per level above 256 px)
    s.hits = sHits; s.misses = sMisses; s.evictions = st.evicted_records; s.corrupt = sCorrupt;
    s.writes = sWrites; s.write_fail = sWriteFail; s.bytes_written = sBytesWritten; s.bytes_read = sBytesRead;
    s.reads_in_flight = sReadsPending;
    U32 ready = 0;
    U64 ready_bytes = 0;
    for (const auto& kv : sReads)
        if (kv.second.mState == LLGpuTexCache::READ_READY) { ++ready; ready_bytes += kv.second.mBytes; }
    s.ready = ready;
    s.ready_bytes = ready_bytes;
    s.read_bytes_in_flight = sReadBytes - llmin(sReadBytes, ready_bytes);
    s.write_bytes_in_flight = sWriteBytes.load();
    s.expired = sExpired;
    s.scanned = st.ready;
    s.enabled = isEnabled();
    return s;
}

const std::string& getDir()
{
    return sDir;
}

std::string statsLine()
{
    if (!sActive || !sStore) return "t2 off";
    if (!sStore->ready()) return "t2 store indexing";
    const LLGpuTexCache::Stats s = getStats();
    const LLGpuTexRecord::Rewriter::Stats rw = sRewriter.stats();
    return llformat("t2 store %.0f/%.0fMB peak %.0f %u rec hit %llu miss %llu bad %llu wr %llu (part %llu up %llu full %llu skip %llu fail %llu) rd-fly %u exp %llu v1->v2 %llu q %u",
                    (F64)s.used / MB, (F64)s.budget / MB, (F64)s.peak / MB, s.entries, (unsigned long long)sHits,
                    (unsigned long long)sMisses, (unsigned long long)sCorrupt, (unsigned long long)sWrites,
                    (unsigned long long)sWritePartial, (unsigned long long)sWriteUpgraded, (unsigned long long)sWriteFull,
                    (unsigned long long)sWriteSkipped, (unsigned long long)sWriteFail, sReadsPending,
                    (unsigned long long)sExpired, (unsigned long long)rw.rewritten, rw.queue + rw.in_flight)
         + (sEnabled ? "" : " (disabled)");
}

void logStats(const std::string& when)
{
    if (!sActive || !sStore) return;
    const LLGpuTexCache::Stats s = getStats();
    const LLGpuTexRecord::Rewriter::Stats rw = sRewriter.stats();
    LL_INFOS("GpuTexCache") << "GPU texture cache (store, " << when << "): " << s.entries << " records, "
                            << llformat("%.1f", (F64)s.used / MB) << " MB stored of "
                            << llformat("%.0f", (F64)s.budget / MB) << " MB budget, peak "
                            << llformat("%.1f", (F64)s.peak / MB) << " MB; hits " << sHits << ", misses " << sMisses
                            << ", discarded bad " << sCorrupt << ", writes " << sWrites << " ("
                            << llformat("%.1f", (F64)sBytesWritten / MB) << " MB raw; handed: " << sWritePartial
                            << " partial (top below the asset's size), " << sWriteFull << " full, " << sWriteUpgraded
                            << " upgrading a smaller entry; " << sWriteSkipped
                            << " skipped as not finer, " << sWriteFail << " failed), read "
                            << llformat("%.1f", (F64)sBytesRead / MB) << " MB, expired " << sExpired
                            << "; v1 -> v2 rewrite: " << rw.rewritten << " done ("
                            << llformat("%.1f", (F64)rw.bytes_rewritten / MB) << " MB), " << rw.stale << " stale, "
                            << rw.failed << " failed, " << rw.dropped_full << " dropped (queue full), " << rw.queue
                            << " queued, " << rw.in_flight << " in flight" << LL_ENDL;
    sStore->logStats(("gputex " + when).c_str());
}

} // namespace LLGpuTexStore
