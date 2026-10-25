/**
 * @file llcachestore.cpp
 * @brief The segment store (#715, #708, #709). See llcachestore.h.
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

/*
 * Threads
 * -------
 * - Callers (any thread): lookup / touch / write enqueue / readSync / staged.
 * - One writer thread (owner only): drains the write intake into per-placement
 *   run buffers, appends runs, lands records in the index, runs budget
 *   enforcement + compaction + snapshots. Every append and every segment
 *   create / delete happens here, so the segment set only changes on it.
 *   A read-only instance runs its tail refresh on this thread instead.
 * - Reader pool: async read tickets, demand before prefetch, by priority: lookup,
 *   pread, header + stored-payload hash checks (I/O; normal OS priority).
 * - Decode pool: the zstd decode of large codec-1 records handed off by a reader,
 *   by priority, then the index bookkeeping + completion (CPU; below-normal OS
 *   priority). Small decodes stay on the reader. Sizes: resolveThreadCounts().
 * - Prefetch thread: region read-ahead, paused while demand reads run.
 *
 * Locks (always taken in this order, never the reverse):
 *   shard mutex  ->  mSegMutex (shared)  ->  (atomics)
 *   mRegionMutex, mStagingMutex, mPolicyMutex, mHiddenMutex, mDayMutex are
 *   leaves: never held while taking another lock.
 *
 * Accounting: record sizes on disk are recordSize(meta_len, stored), i.e.
 * header + meta + payload + padding. Segment::live = bytes of records the
 * index points at; a segment's other bytes are dead. Group "disk" bytes = all
 * bytes written for that group in segments that still exist (live + dead),
 * which is what budgets compare against (#709: dead records stay until the
 * space is needed).
 */

#include "linden_common.h"

#include "llcachestore.h"
#include "llcachestore_format.h"
#include "llcachestore_index.h"
#include "llcachestore_platform.h"

#include "hbxxh.h"
#include "llcachecodec.h"
#include "llthread.h"   // ll_set_current_thread_name

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace LLCache;
namespace F = LLCacheStoreFormat;
namespace P = LLCacheStorePlatform;
using LLCacheStoreIndex::Entry;
using LLCacheStoreIndex::Index;
using LLCacheStoreIndex::NO_REGION;
using LLCacheStoreIndex::EF_SHARED;
using LLCacheStoreIndex::EF_RENAMED;
using LLCacheStoreIndex::EF_PINNED;
using LLCacheStoreIndex::EF_TOUCHED;
using LLCacheStoreIndex::EF_RAM_ONLY;
using LLCacheStoreIndex::EF_PENDING;
using LLCacheStoreIndex::EF_PERSIST;

namespace
{
    constexpr size_t SHARDS      = 64;
    constexpr size_t KIND_COUNT      = (size_t)Kind::COUNT;
    constexpr size_t GROUP_COUNT     = (size_t)Group::COUNT;
    constexpr U32    MAX_REGION_REFS = 16384;
    constexpr U32    REGION_MAX_AGE_S = 90u * 86400u;     // manifests dropped after this
    constexpr U32    DISK_FULL_BACKOFF_S = 5;

    const char* SNAP_NAME = "index.snap";
    const char* SNAP_PREV = "index.snap.prev";
    const char* SNAP_TMP  = "index.snap.tmp";

    size_t ki(Kind k) { return (size_t)k; }

    ECacheRecordKind codecKind(Kind k)
    {
        switch (k)
        {
        case Kind::J2C:     return ECacheRecordKind::TEXTURE_J2C;
        case Kind::GPUTEX:  return ECacheRecordKind::TEXTURE_GPU;
        case Kind::ASSET:
        case Kind::MESH:
        case Kind::SCRATCH: return ECacheRecordKind::ASSET;
        default:            return ECacheRecordKind::GENERIC;
        }
    }

    // Kinds whose writes deliver data to consumers (#711: must keep working).
    bool isDeliveryKind(Kind k)
    {
        return k == Kind::ASSET || k == Kind::MESH || k == Kind::SCRATCH || k == Kind::MANIFEST;
    }

    U32 wallNow()
    {
        return (U32)time(nullptr);
    }

    // Reader / decoder pool sizes (0 in the config = auto). Readers are I/O bound: an
    // HDD gets one (parallel random reads only add seeks), an SSD a few for queue depth
    // (unknown = SSD, as WearPolicy). Decoders are CPU bound and run below normal OS
    // priority; auto stays at cores/4 (1..4) so the render / main threads keep the cores,
    // an override at most cores/2.
    void resolveThreadCounts(Store::Config& cfg, DriveType drive)
    {
        U32 cores = std::thread::hardware_concurrency();
        if (!cores) cores = 4;
        if (!cfg.read_threads)
            cfg.read_threads = drive == DriveType::HDD ? 1u : std::clamp<U32>(cores / 8, 2, 4);
        cfg.read_threads = std::clamp<U32>(cfg.read_threads, 1, 16);
        if (!cfg.decode_threads)
            cfg.decode_threads = std::clamp<U32>(cores / 4, 1, 4);
        cfg.decode_threads = std::clamp<U32>(cfg.decode_threads, 1, std::max<U32>(1, cores / 2));
    }
}

// ======================================================================
// names

const char* LLCache::kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::NONE:     return "none";
    case Kind::J2C:      return "j2c";
    case Kind::GPUTEX:   return "gputex";
    case Kind::ASSET:    return "asset";
    case Kind::MESH:     return "mesh";
    case Kind::CLUSTER:  return "cluster";
    case Kind::SCRATCH:  return "scratch";
    case Kind::MANIFEST: return "manifest";
    default:             return "?";
    }
}

const char* LLCache::groupName(Group group)
{
    switch (group)
    {
    case Group::TEXTURES:   return "textures";
    case Group::ASSETS:     return "assets";
    case Group::CLUSTER:    return "cluster";
    case Group::UNBUDGETED: return "unbudgeted";
    default:                return "?";
    }
}

const char* LLCache::statusName(Status s)
{
    switch (s)
    {
    case Status::OK:           return "OK";
    case Status::MISSING:      return "MISSING";
    case Status::CORRUPT:      return "CORRUPT";
    case Status::IO_ERROR:     return "IO_ERROR";
    case Status::NO_SPACE:     return "NO_SPACE";
    case Status::REFUSED_BUSY: return "REFUSED_BUSY";
    case Status::READ_ONLY:    return "READ_ONLY";
    case Status::CANCELLED:    return "CANCELLED";
    case Status::SKIPPED:      return "SKIPPED";
    default:                   return "?";
    }
}

LLCache::DriveType LLCache::detectDriveType(const std::string& path)
{
    switch (P::detectDriveType(path))
    {
    case P::DriveType::SSD: return DriveType::SSD;
    case P::DriveType::HDD: return DriveType::HDD;
    default:                return DriveType::UNKNOWN;
    }
}

// ======================================================================
// default eviction policy

int EvictionPolicy::tier(const RecordInfo& r) const
{
    if (isDuplicate(r))
    {
        return 0;
    }
    return r.key.kind == Kind::GPUTEX ? 1 : 2;
}

F32 EvictionPolicy::keepScore(const RecordInfo& r, const EvictionContext& ctx) const
{
    // reuse x decode cost saved / bytes, weighted by recency of use (#705):
    // the newest of the record's last access and the last visit of a region
    // that uses it, half-life 14 days.
    U32 recent = std::max(r.last_access, ctx.region_recency);
    recent = std::max(recent, r.write_time);
    F32 age_days = ctx.now > recent ? (F32)(ctx.now - recent) / 86400.f : 0.f;
    F32 recency = std::pow(0.5f, age_days / 14.f);
    F32 bytes = (F32)std::max<U32>(r.stored_bytes, 1u) + 96.f;
    return (1.f + (F32)r.reuse_count) * ctx.decode_cost * recency * 1e6f / bytes;
}

// ======================================================================
// internals

namespace
{
    struct Pending
    {
        Key  key;                       // guarded by mutex (rename() can change it)
        std::mutex mutex;
        // source, until materialised on the writer thread
        std::vector<U8>       raw;
        const U8*             borrowed   = nullptr;
        size_t                borrowed_n = 0;
        std::function<void()> release;
        // after intake
        std::vector<U8>       payload;
        bool                  encoded    = false;
        U8                    codec      = F::CODEC_NONE;
        U64                   payload_hash = 0;

        U32  raw_size     = 0;
        U32  kind_version = 1;
        U8   meta_len     = 0;
        U8   meta[32]     = {};
        U32  write_time   = 0;
        U32  region_slot  = NO_REGION;
        LLUUID region;
        U64  grid_handle  = 0;
        U32  wflags       = 0;
        Priority prio     = Priority::NORMAL;
        bool control      = false;      // tombstone / clear marker (no index entry)
        U8   control_flags = 0;
        std::shared_ptr<detail::TicketState<Status>> done;

        void releaseBorrowed()
        {
            std::function<void()> rel;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (borrowed)
                {
                    raw.assign(borrowed, borrowed + borrowed_n);
                    borrowed = nullptr;
                    borrowed_n = 0;
                }
                rel = std::move(release);
                release = nullptr;
            }
            if (rel)
            {
                rel();
            }
        }
    };
    using PendingPtr = std::shared_ptr<Pending>;

    // One index shard (#866): the flat record table (llcachestore_index.h) plus the
    // queued / RAM-only writes of its records. A record has an entry in `pend` iff
    // its EF_PENDING flag is set; erase through erase() / clear() to keep that.
    struct Shard
    {
        mutable std::mutex mutex;
        Index map;
        std::unordered_map<Key, PendingPtr, KeyHash> pend;

        // The record's queued bytes (null when none).
        PendingPtr pending(const Key& k, const Entry& e) const
        {
            if (!e.pending()) return PendingPtr();
            auto it = pend.find(k);
            return it == pend.end() ? PendingPtr() : it->second;
        }
        // True if the record's queued bytes are exactly p.
        bool isPending(const Key& k, const Entry& e, const PendingPtr& p) const
        {
            if (!e.pending()) return !p;
            auto it = pend.find(k);
            return it != pend.end() && it->second == p;
        }
        void setPending(const Key& k, Entry& e, PendingPtr p)
        {
            if (p)
            {
                pend[k] = std::move(p);
                e.flags |= EF_PENDING;
            }
            else
            {
                if (e.pending()) pend.erase(k);
                e.flags &= ~EF_PENDING;
            }
        }
        Index::iterator erase(Index::iterator it)
        {
            if (it->second.pending()) pend.erase(it->first);
            return map.erase(it);
        }
        void clear()
        {
            map.clear();
            pend.clear();
        }
        U64 memoryBytes() const
        {
            // pending map nodes approximated as key + pointer + two links
            return map.memoryBytes() + (U64)pend.size() * (sizeof(Key) + sizeof(PendingPtr) + 2 * sizeof(void*)) +
                   (U64)pend.bucket_count() * sizeof(void*);
        }
    };

    struct Segment
    {
        U32         seq = 0;
        std::string path;
        P::File     file;
        std::atomic<U64> size{ 0 };            // valid bytes (append end)
        std::atomic<U64> live{ 0 };
        std::atomic<U32> live_records{ 0 };
        std::array<std::atomic<U64>, GROUP_COUNT> group_bytes{};
        std::array<std::atomic<U64>, KIND_COUNT>  kind_bytes{};
        U64  indexed_through = 0;              // scan watermark (read-only refresh / snapshot)
        bool header_ok = true;
    };
    using SegmentPtr = std::shared_ptr<Segment>;

    struct RegionState
    {
        LLUUID id;
        U64    handle = 0;
        U32    last_visit = 0;
        U32    visit_count = 0;
        bool   manifest_loaded = false;
        bool   dirty = false;
        std::vector<Key> shared_refs;
        std::unordered_set<Key, KeyHash> shared_set;
        std::vector<Key> order;
        std::unordered_set<Key, KeyHash> order_set;
    };

    struct ReadJob
    {
        Key  key;
        bool ranged = false;
        U64  offset = 0, len = 0;
        Priority prio = Priority::NORMAL;
        U64  seq = 0;
        std::shared_ptr<detail::TicketState<ReadResult>> state;
        std::chrono::steady_clock::time_point t_req;
    };
    struct ReadJobOrder
    {
        bool operator()(const ReadJob& a, const ReadJob& b) const
        {
            if (a.prio != b.prio) return (int)a.prio < (int)b.prio;
            return a.seq > b.seq;
        }
    };

    // One read between its stages: readBegin (lookup + I/O + hash checks), readDecode
    // (codec 1), readFinish (index bookkeeping: touch / unpin / corrupt drop).
    struct ReadCtx
    {
        Key  key;
        bool ranged = false;
        U64  roff = 0, rlen = 0;
        bool count_access = true;
        ReadResult res;
        bool done = false;          // res is final (overflow / staging / not indexed): no finish
        Status st = Status::MISSING;
        PendingPtr pend;            // read from the queued copy (else the disk copy below)
        U32  seq = 0, off = 0, raw = 0;
        // codec-1 disk payload still to decode: buf[pl_off, pl_off + stored)
        bool need_decode = false;
        bool zstd = false;
        std::vector<U8> buf;
        size_t pl_off = 0;
        U32  stored = 0;
    };

    // A read whose decode was handed from a reader to the decode pool.
    struct DecodeJob
    {
        ReadCtx  ctx;
        Priority prio = Priority::NORMAL;
        U64      seq = 0;           // the read's request order (same tie-break as the I/O queue)
        std::shared_ptr<detail::TicketState<ReadResult>> state;
        std::chrono::steady_clock::time_point t_req, t_handoff;
    };
    struct DecodeJobOrder
    {
        bool operator()(const DecodeJob& a, const DecodeJob& b) const
        {
            if (a.prio != b.prio) return (int)a.prio < (int)b.prio;
            return a.seq > b.seq;
        }
    };

    // Below this decoded size a decode stays on the reader: the handoff would cost more
    // than it saves, and small records are the latency-sensitive kinds.
    constexpr U32 HANDOFF_MIN_RAW = 64u << 10;

    struct Extent
    {
        U32 seg; U32 off; U32 len;
        bool operator<(const Extent& o) const { return seg != o.seg ? seg < o.seg : off < o.off; }
    };

    struct PrefetchJob
    {
        LLUUID region;
        std::vector<std::vector<Extent>> ranges;  // each = contiguous records, sorted
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    // Buffered sequential reader for scans.
    struct Window
    {
        const P::File* f = nullptr;
        U64 base = 0;
        std::vector<U8> buf;
        size_t len = 0;
        const U8* get(U64 off, size_t n)
        {
            if (off >= base && off + n <= base + len)
            {
                return buf.data() + (off - base);
            }
            size_t want = std::max<size_t>(n, 1u << 20);
            buf.resize(want);
            S64 got = f->pread(buf.data(), want, off);
            if (got < 0 || (size_t)got < n)
            {
                len = 0;
                return nullptr;
            }
            base = off;
            len = (size_t)got;
            return buf.data();
        }
    };
}

struct Store::Impl
{
    Config      cfg;
    std::string dir;
    bool        is_open = false;
    bool        ro = false;
    std::atomic<bool> is_ready{ false };
    P::LockFile lock;
    std::unique_ptr<Store> overflow;
    std::string overflow_path;

    mutable std::mutex policy_mutex;
    std::array<KindPolicy, KIND_COUNT> policies;

    std::array<Shard, SHARDS> shards;

    mutable std::shared_mutex seg_mutex;
    std::map<U32, SegmentPtr> segments;
    U32        next_seq = 1;
    SegmentPtr active;                       // writer thread only

    // accounting
    std::array<std::atomic<U64>, GROUP_COUNT> group_live{};
    std::array<std::atomic<U64>, GROUP_COUNT> group_disk{};
    std::array<std::atomic<U32>, GROUP_COUNT> group_records{};
    std::array<std::atomic<U64>, GROUP_COUNT> budgets{};
    std::array<std::atomic<U64>, KIND_COUNT>  kind_live{};
    std::array<std::atomic<U32>, KIND_COUNT>  kind_records{};
    std::array<std::atomic<U64>, KIND_COUNT>  kind_raw{};          // decoded bytes of the live records
    std::array<std::atomic<U64>, KIND_COUNT>  kind_evicted{};
    std::array<std::atomic<U64>, KIND_COUNT>  kind_evicted_bytes{};
    std::atomic<U64> disk_bytes{ 0 }, peak_disk{ 0 };

    // cost counters (#715 measurement): CPU time in the codec, time in the append pwrite,
    // index load time. Steady-clock wall time of the calling thread (the codec and the
    // pwrite are synchronous, so it is that thread's busy time).
    std::atomic<U64> encode_count{ 0 }, encode_usec{ 0 }, encode_in{ 0 }, encode_out{ 0 };
    std::atomic<U64> decode_count{ 0 }, decode_usec{ 0 }, decode_in{ 0 }, decode_out{ 0 };
    std::atomic<U64> append_count{ 0 }, append_usec{ 0 }, append_bytes{ 0 };
    std::atomic<U64> index_load_usec{ 0 };
    std::atomic<U32> index_load_time{ 0 };   // unix seconds when the load finished
    std::atomic<U64> session_new{ 0 }, session_comp{ 0 }, session_meta{ 0 };   // this run's writes by class

    // counters
    std::atomic<U64> hits{ 0 }, misses{ 0 }, corrupt_dropped{ 0 }, write_fail{ 0 }, writes_refused{ 0 };
    std::atomic<U64> evicted_records{ 0 }, evicted_bytes{ 0 }, segments_reclaimed{ 0 }, rescued_records{ 0 };
    std::atomic<U64> superseded{ 0 }, skipped{ 0 }, ram_fallback{ 0 };
    std::atomic<U64> bytes_written_total{ 0 }, bytes_read{ 0 };
    std::atomic<U64> prefetch_bytes{ 0 }, prefetch_hits{ 0 };
    // reads_in_flight: started (past the index lookup), not finished (I/O + decode, incl.
    // the decode queue). demand_in_flight: inside a demand pread only (pauses the prefetcher).
    std::atomic<U32> reads_in_flight{ 0 }, demand_in_flight{ 0 };

    // daily (#709)
    mutable std::mutex day_mutex;
    U32 day = 0;
    U64 day_new = 0, day_comp = 0, day_meta = 0;

    // writer
    std::thread writer;
    std::atomic<std::thread::id> writer_id{};
    std::mutex wmutex;
    std::condition_variable wcv;
    std::deque<PendingPtr> intake;
    std::deque<std::function<void()>> wtasks;
    bool stopping = false;
    std::atomic<U64> queued_bytes{ 0 };
    std::atomic<U32> queued_count{ 0 };
    struct Bucket
    {
        std::vector<PendingPtr> recs;
        U64 bytes = 0;
        std::chrono::steady_clock::time_point first;
    };
    std::unordered_map<U32, Bucket> buckets;  // writer only
    U32 last_snapshot = 0;
    // > 0 while a budget pass or a compaction re-append runs (writer thread
    // only): appends made by them must not start another budget pass, which
    // could pick the segment being reclaimed and delete it twice.
    U32 no_enforce = 0;
    struct NoEnforce
    {
        U32& n;
        explicit NoEnforce(U32& c) : n(c) { ++n; }
        ~NoEnforce() { --n; }
    };
    std::atomic<bool> index_dirty{ false };
    std::atomic<U32> disk_full_until{ 0 };
    std::mutex free_mutex;
    S64 cached_free = -1;
    std::chrono::steady_clock::time_point free_checked;
    U64 free_checked_disk = 0;

    // readers
    std::vector<std::thread> readers;
    mutable std::mutex rmutex;
    std::condition_variable rcv;
    std::priority_queue<ReadJob, std::vector<ReadJob>, ReadJobOrder> rqueue;
    U64 rseq = 0;
    bool rstop = false;

    // decode pool (heap ordered by DecodeJobOrder; a vector so jobs move out)
    std::vector<std::thread> decoders;
    mutable std::mutex dmutex;
    std::condition_variable dcv;
    std::vector<DecodeJob> dqueue;
    U64 dqueue_bytes = 0;                    // guarded by dmutex
    bool dstop = false;

    // read scheduling stats (async reads)
    std::atomic<U64> read_jobs{ 0 }, read_wait_usec{ 0 }, read_io_usec{ 0 };
    std::atomic<U64> decode_handoffs{ 0 }, decode_wait_usec{ 0 }, decode_inline{ 0 };
    std::atomic<U64> read_done_count{ 0 }, read_done_usec{ 0 }, read_done_max{ 0 };
    std::array<std::atomic<U64>, PRIORITY_COUNT> reads_by_prio{};

    // prefetch
    std::thread prefetcher;
    std::mutex pmutex;
    std::condition_variable pcv;
    std::deque<PrefetchJob> pqueue;
    std::unordered_map<LLUUID, std::shared_ptr<std::atomic<bool>>> pactive;
    bool pstop = false;
    bool buffer_mode = false;
    mutable std::mutex ra_mutex;
    std::map<std::pair<U32, U32>, std::vector<U8>> ra_chunks;   // (seg, off) -> bytes
    std::deque<std::pair<U32, U32>> ra_fifo;
    U64 ra_bytes = 0;

    // staging
    mutable std::mutex staging_mutex;
    std::unordered_map<Key, std::vector<U8>, KeyHash> staging;

    // regions
    mutable std::mutex region_mutex;
    std::vector<RegionState> regions;
    std::unordered_map<LLUUID, U32> region_slots;

    // policies
    std::array<std::shared_ptr<EvictionPolicy>, GROUP_COUNT> evict_policy;
    WearPolicy wear;
    std::atomic<U64> disk_reserve{ 2ull << 30 };
    DriveType drive = DriveType::UNKNOWN;

    // read-only instance
    mutable std::mutex hidden_mutex;
    std::unordered_set<Key, KeyHash> hidden;
    std::array<bool, KIND_COUNT> hidden_kinds{};
    U32 last_refresh = 0;

    // async open
    std::mutex load_mutex;
    bool loading = false;
    std::unordered_set<Key, KeyHash> removed_during_load;
    std::array<bool, KIND_COUNT> cleared_during_load{};

    U32 session_start = 0;

    // ------------------------------------------------------------ helpers

    U32 now() const { return cfg.clock ? cfg.clock() : wallNow(); }

    static U64 usecSince(std::chrono::steady_clock::time_point t0)
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    void countEvict(Kind k, U64 bytes)
    {
        evicted_records++;
        evicted_bytes += bytes;
        kind_evicted[ki(k)]++;
        kind_evicted_bytes[ki(k)] += bytes;
    }
    // loadIndex() + its duration (owner and read-only, sync and async open).
    void timedLoadIndex()
    {
        const auto t0 = std::chrono::steady_clock::now();
        loadIndex();
        index_load_usec = usecSince(t0);
        index_load_time = wallNow();
    }

    Shard& shardFor(const Key& k) { return shards[KeyHash()(k) % SHARDS]; }
    const Shard& shardFor(const Key& k) const { return shards[KeyHash()(k) % SHARDS]; }

    WearPolicy wearCopy() const
    {
        std::lock_guard<std::mutex> lock(policy_mutex);
        return wear;
    }

    KindPolicy policy(Kind k) const
    {
        std::lock_guard<std::mutex> lock(policy_mutex);
        return policies[ki(k)];
    }
    Group groupOf(Kind k) const { return policy(k).group; }

    SegmentPtr findSeg(U32 seq) const
    {
        std::shared_lock<std::shared_mutex> lock(seg_mutex);
        auto it = segments.find(seq);
        return it == segments.end() ? SegmentPtr() : it->second;
    }

    std::string segPath(U32 seq) const { return dir + "/" + F::segmentFileName(seq); }

    void noteDay(U32 t)
    {
        U32 d = t / 86400u;
        if (d != day)
        {
            day = d;
            day_new = day_comp = day_meta = 0;
        }
    }
    enum class WriteClass { NEW, COMPACTION, META };
    void countWrite(U64 n, WriteClass c)
    {
        std::lock_guard<std::mutex> lock(day_mutex);
        noteDay(now());
        if (c == WriteClass::NEW) { day_new += n; session_new += n; }
        else if (c == WriteClass::COMPACTION) { day_comp += n; session_comp += n; }
        else { day_meta += n; session_meta += n; }
        bytes_written_total += n;
    }
    U64 compactionAllowance()
    {
        std::lock_guard<std::mutex> lock(day_mutex);
        noteDay(now());
        WearPolicy w = wearCopy();
        U64 cap = (U64)((F64)day_new * (F64)w.daily_compaction_fraction);
        cap = std::max(cap, w.daily_compaction_cap_bytes);
        return cap > day_comp ? cap - day_comp : 0;
    }

    U64 recSize(const Entry& e) const { return F::recordSize(e.meta_len, e.stored); }

    // Live accounting for an entry's disk record. Caller holds the shard lock.
    void addLive(const Key& k, const Entry& e)
    {
        if (!e.seg) return;
        U64 sz = recSize(e);
        if (SegmentPtr s = findSeg(e.seg))
        {
            s->live += sz;
            s->live_records++;
        }
        size_t g = (size_t)groupOf(k.kind);
        group_live[g] += sz;
        group_records[g]++;
        kind_live[ki(k.kind)] += sz;
        kind_records[ki(k.kind)]++;
        kind_raw[ki(k.kind)] += e.raw;
    }
    void subLive(const Key& k, Entry& e)
    {
        if (!e.seg) return;
        U64 sz = recSize(e);
        if (SegmentPtr s = findSeg(e.seg))
        {
            s->live -= std::min<U64>(sz, s->live.load());
            if (s->live_records) s->live_records--;
        }
        size_t g = (size_t)groupOf(k.kind);
        group_live[g] -= std::min<U64>(sz, group_live[g].load());
        if (group_records[g]) group_records[g]--;
        kind_live[ki(k.kind)] -= std::min<U64>(sz, kind_live[ki(k.kind)].load());
        if (kind_records[ki(k.kind)]) kind_records[ki(k.kind)]--;
        kind_raw[ki(k.kind)] -= std::min<U64>(e.raw, kind_raw[ki(k.kind)].load());
        e.seg = 0;
        e.offset = 0;
        index_dirty = true;
    }

    // Caller holds sh's lock; e is sh's entry for k.
    RecordInfo infoOf(const Shard& sh, const Key& k, const Entry& e) const
    {
        RecordInfo r;
        r.key = k;
        r.last_access = e.last_access;
        r.prev_session_access = e.prevAccess();
        r.reuse_count = e.reuse;
        r.shared = (e.flags & EF_SHARED) != 0;
        r.pinned = (e.flags & EF_PINNED) != 0;
        U32 slot = e.region();
        if (PendingPtr pp = sh.pending(k, e))
        {
            Pending& p = *pp;
            std::lock_guard<std::mutex> plock(p.mutex);
            r.queued = true;
            r.raw_bytes = p.raw_size;
            r.stored_bytes = p.encoded ? (U32)p.payload.size() : p.raw_size;
            r.codec = p.codec;
            r.kind_version = p.kind_version;
            r.write_time = p.write_time;
            r.meta_len = p.meta_len;
            memcpy(r.meta, p.meta, p.meta_len);
            slot = p.region_slot;
        }
        else
        {
            r.raw_bytes = e.raw;
            r.stored_bytes = e.stored;
            r.codec = e.codec();
            r.kind_version = e.kindVersion();
            r.write_time = e.write_time;
            if (const U8* meta = e.meta_len ? sh.map.meta(k) : nullptr)
            {
                r.meta_len = e.meta_len;
                memcpy(r.meta, meta, e.meta_len);
            }
        }
        if (slot != NO_REGION)
        {
            std::lock_guard<std::mutex> lock(region_mutex);
            if (slot < regions.size()) r.region = regions[slot].id;
        }
        return r;
    }

    // ------------------------------------------------------------ regions

    U32 internRegion(const LLUUID& id, U64 handle)
    {
        if (id.isNull()) return NO_REGION;
        std::lock_guard<std::mutex> lock(region_mutex);
        auto it = region_slots.find(id);
        if (it != region_slots.end())
        {
            RegionState& r = regions[it->second];
            if (handle && r.handle != handle)
            {
                r.handle = handle;      // grid handle changed for the same UUID
                r.dirty = true;
            }
            return it->second;
        }
        U32 slot = (U32)regions.size();
        regions.emplace_back();
        regions.back().id = id;
        regions.back().handle = handle;
        region_slots[id] = slot;
        return slot;
    }
    S32 regionSlot(const LLUUID& id) const
    {
        std::lock_guard<std::mutex> lock(region_mutex);
        auto it = region_slots.find(id);
        return it == region_slots.end() ? -1 : (S32)it->second;
    }
    std::vector<U32> regionVisitTimes() const
    {
        std::lock_guard<std::mutex> lock(region_mutex);
        std::vector<U32> v(regions.size());
        for (size_t i = 0; i < regions.size(); ++i) v[i] = regions[i].last_visit;
        return v;
    }
    void addRegionUse(U32 slot, const Key& k, bool shared_ref)
    {
        std::lock_guard<std::mutex> lock(region_mutex);
        if (slot >= regions.size()) return;
        RegionState& r = regions[slot];
        if (r.order.size() < MAX_REGION_REFS && r.order_set.insert(k).second)
        {
            r.order.push_back(k);
            r.dirty = true;
        }
        if (shared_ref && r.shared_refs.size() < MAX_REGION_REFS && r.shared_set.insert(k).second)
        {
            r.shared_refs.push_back(k);
            r.dirty = true;
        }
    }

    // ------------------------------------------------------------ open / load

    struct SnapSeg
    {
        U32 seq; U64 through;
        std::array<U64, GROUP_COUNT> group_bytes;
        std::array<U64, KIND_COUNT> kind_bytes;
    };
    struct SnapRegion
    {
        LLUUID id; U64 handle; U32 last_visit, visit_count;
    };
    // Snapshot entry (v1 wire layout), 64 bytes + meta:
    //   0 uuid | 16 kind | 17 codec | 18 flags | 19 meta_len | 20 variant | 24 kind_version |
    //   28 seg | 32 offset | 36 stored | 40 raw | 44 write_time | 48 last_access |
    //   52 reuse | 54 (0) | 56 region index | 60 recency | 64 meta
    static constexpr size_t SNAP_ENTRY_FIXED = 64;
    struct Snapshot
    {
        U32 created = 0, day = 0;
        U64 day_new = 0, day_comp = 0, day_meta = 0;
        std::vector<SnapSeg> segs;
        std::vector<SnapRegion> regions;
        // #866: entries are applied straight from the file bytes, shard by shard
        // (no per-entry copy): offsets of the entries in buf, per index shard.
        std::vector<U8> buf;
        std::vector<std::vector<U32>> by_shard;
    };

    bool readSnapshotFile(const std::string& path, Snapshot& snap)
    {
        P::File f;
        if (!f.open(path, P::File::READ)) return false;
        S64 sz = f.size();
        if (sz < 16 || sz > (S64)(4ull << 30)) return false;
        std::vector<U8>& buf = snap.buf;
        buf.resize((size_t)sz);
        if (f.pread(buf.data(), buf.size(), 0) != sz) return false;
        U64 want = F::get64(&buf[buf.size() - 8]);
        if (HBXXH64::digest(buf.data(), buf.size() - 8) != want) return false;
        F::Reader r(buf.data(), buf.size() - 8);
        U8 magic[4];
        r.bytes(magic, 4);
        if (memcmp(magic, "SLCI", 4) != 0 || r.u16() != 1) return false;
        r.u16();
        snap.created = r.u32();
        snap.day = r.u32();
        snap.day_new = r.u64();
        snap.day_comp = r.u64();
        snap.day_meta = r.u64();
        U32 nseg = r.u32(), nreg = r.u32(), nent = r.u32();
        if (!r.ok) return false;
        for (U32 i = 0; i < nseg && r.ok; ++i)
        {
            SnapSeg s;
            s.seq = r.u32();
            s.through = r.u64();
            for (auto& g : s.group_bytes) g = r.u64();
            for (auto& k : s.kind_bytes) k = r.u64();
            snap.segs.push_back(s);
        }
        for (U32 i = 0; i < nreg && r.ok; ++i)
        {
            SnapRegion g;
            g.id = r.uuid();
            g.handle = r.u64();
            g.last_visit = r.u32();
            g.visit_count = r.u32();
            snap.regions.push_back(g);
        }
        if (!r.ok) return false;
        snap.by_shard.assign(SHARDS, std::vector<U32>());
        for (auto& v : snap.by_shard) v.reserve(std::min<U32>(nent, 64u << 20) / SHARDS * 9 / 8 + 16);
        size_t pos = r.pos;
        const size_t end = r.n;
        for (U32 i = 0; i < nent; ++i)
        {
            if (end - pos < SNAP_ENTRY_FIXED) return false;
            const U8* e = &buf[pos];
            const size_t ml = std::min<size_t>(e[19], F::MAX_META);
            if (end - pos - SNAP_ENTRY_FIXED < ml) return false;
            Key key;
            memcpy(key.id.mData, e, UUID_BYTES);
            key.kind = (Kind)e[16];
            key.variant = F::get32(e + 20);
            snap.by_shard[KeyHash()(key) % SHARDS].push_back((U32)pos);
            pos += SNAP_ENTRY_FIXED + ml;
        }
        return pos == end;
    }

    // True if the entry's disk record comes after (seq, off) in the log.
    static bool laterThan(const Entry& e, U32 seq, U64 off)
    {
        return e.seg > seq || (e.seg == seq && e.offset > off);
    }

    // Apply one record found by a scan. Only records later in the log than
    // what the index holds take effect (a snapshot entry may already be newer).
    void applyScanned(const F::RecordHeader& h, const U8* meta, U32 seq, U64 off, bool owner_load)
    {
        Kind kind = (Kind)h.kind;
        if (h.flags & F::FLAG_CLEAR_KIND)
        {
            Kind cleared = (Kind)std::min<U32>(h.variant, KIND_COUNT - 1);
            for (Shard& sh : shards)
            {
                std::lock_guard<std::mutex> lock(sh.mutex);
                for (auto it = sh.map.begin(); it != sh.map.end();)
                {
                    if (it->first.kind == cleared && !it->second.pending() && it->second.seg &&
                        !laterThan(it->second, seq, off))
                    {
                        subLive(it->first, it->second);
                        it = sh.erase(it);
                    }
                    else ++it;
                }
            }
            return;
        }
        Key key(h.id, kind, h.variant);
        if (h.flags & F::FLAG_TOMBSTONE)
        {
            Shard& sh = shardFor(key);
            std::lock_guard<std::mutex> lock(sh.mutex);
            auto it = sh.map.find(key);
            if (it != sh.map.end() && !it->second.pending() && it->second.seg && !laterThan(it->second, seq, off))
            {
                subLive(key, it->second);
                sh.erase(it);
            }
            return;
        }
        if (kind == Kind::NONE) return;
        if (kind == Kind::SCRATCH && owner_load) return;      // scratch is dropped at the next start
        if (h.kind_version != policy(kind).format_version) return;
        {
            std::lock_guard<std::mutex> lock(load_mutex);
            if (loading && (removed_during_load.count(key) || cleared_during_load[ki(kind)])) return;
        }
        U32 slot = internRegion(h.region, h.grid_handle);
        Shard& sh = shardFor(key);
        std::lock_guard<std::mutex> lock(sh.mutex);
        Entry* ep = sh.map.insert(key);
        if (!ep) return;
        Entry& e = *ep;
        if (e.seg && laterThan(e, seq, off)) return;
        subLive(key, e);
        e.seg = seq;
        e.offset = (U32)off;
        e.stored = h.stored;
        e.raw = h.raw;
        e.setKindVersion(h.kind_version);
        e.write_time = h.write_time;
        e.setCodec(h.codec);
        sh.map.setMeta(key, meta, h.meta_len);
        e.setRegion(slot);
        e.flags &= ~EF_RENAMED;
        if (!e.last_access)
        {
            e.last_access = h.write_time;   // the write was the last access we know of
            e.setPrevAccess(h.write_time);
        }
        addLive(key, e);
    }

    // Scan [start, file end) of a segment. Returns the end of the last good
    // record. verify_payload hashes payloads; stop_on_bad stops at the first
    // bad / incomplete record instead of resynchronising (read-only refresh
    // of the owner's in-progress tail).
    U64 scanSegment(Segment& s, U64 start, bool verify_payload, bool stop_on_bad, bool owner_load,
                    bool count_bytes)
    {
        S64 fsz = s.file.size();
        if (fsz < 0) return start;
        U64 size = (U64)fsz;
        Window w;
        w.f = &s.file;
        U64 pos = std::max<U64>(F::align16(start), F::SEG_HEADER_SIZE);
        U64 good_end = pos;
        while (pos + F::REC_HEADER_SIZE <= size)
        {
            const U8* fixed = w.get(pos, F::REC_HEADER_SIZE + F::MAX_META);
            if (!fixed)
            {
                fixed = w.get(pos, F::REC_HEADER_SIZE);
                if (!fixed) break;
            }
            F::RecordHeader h;
            F::HeaderCheck hc = F::parseRecordHeaderFixed(fixed, h);
            bool good = hc == F::HeaderCheck::OK && pos + F::REC_HEADER_SIZE + h.meta_len <= size;
            if (good)
            {
                fixed = w.get(pos, F::REC_HEADER_SIZE + h.meta_len);
                good = fixed && F::verifyRecordHeader(fixed, fixed + F::REC_HEADER_SIZE, h.meta_len);
            }
            if (!good)
            {
                if (stop_on_bad) break;
                pos += F::REC_ALIGN;    // resync
                continue;
            }
            U64 total = h.totalSize();
            if (pos + total > size)
            {
                break;                  // torn: header written, payload not
            }
            U8 meta[32];
            memcpy(meta, fixed + F::REC_HEADER_SIZE, h.meta_len);
            if (verify_payload && h.stored)
            {
                const U8* pl = w.get(pos + F::REC_HEADER_SIZE + h.meta_len, h.stored);
                if (!pl || HBXXH64::digest(pl, h.stored) != h.payload_hash)
                {
                    if (stop_on_bad) break;
                    pos += total;       // complete but damaged: skip it, keep going
                    good_end = pos;
                    continue;
                }
            }
            applyScanned(h, meta, s.seq, pos, owner_load);
            if (count_bytes)
            {
                bool control = (h.flags & (F::FLAG_TOMBSTONE | F::FLAG_CLEAR_KIND)) || h.kind == 0;
                size_t g = control ? (size_t)Group::UNBUDGETED : (size_t)groupOf((Kind)h.kind);
                s.group_bytes[g] += total;
                s.kind_bytes[std::min<size_t>(h.kind, KIND_COUNT - 1)] += total;
            }
            pos += total;
            good_end = pos;
        }
        return good_end;
    }

    void loadIndex()
    {
        const bool owner = !ro;
        // 1. segments on disk
        std::map<U32, SegmentPtr> segs;
        for (const std::string& name : P::listDir(dir))
        {
            U32 seq;
            if (!F::parseSegmentFileName(name, seq)) continue;
            auto s = std::make_shared<Segment>();
            s->seq = seq;
            s->path = dir + "/" + name;
            if (!s->file.open(s->path, owner ? P::File::READ_WRITE : P::File::READ))
            {
                LL_WARNS("CacheStore") << "cannot open " << s->path << " (" << s->file.lastError() << ")" << LL_ENDL;
                continue;
            }
            S64 sz = s->file.size();
            if (sz < (S64)F::SEG_HEADER_SIZE)
            {
                // Crash while creating it: nothing in it.
                s->file.close();
                if (owner) P::removeFile(s->path);
                continue;
            }
            U8 hdr[F::SEG_HEADER_SIZE];
            U32 hseq = 0, created = 0;
            if (s->file.pread(hdr, sizeof(hdr), 0) != (S64)sizeof(hdr) ||
                !F::parseSegmentHeader(hdr, hseq, created) || hseq != seq)
            {
                // Keep it: its records carry their own hashes.
                LL_WARNS("CacheStore") << "bad segment header in " << name << "; scanning records anyway" << LL_ENDL;
                s->header_ok = false;
            }
            s->size = (U64)sz;
            segs[seq] = s;
            next_seq = std::max(next_seq, seq + 1);
        }
        {
            std::unique_lock<std::shared_mutex> lock(seg_mutex);
            segments = segs;
        }

        // 2. snapshot -> .prev -> none
        Snapshot snap;
        bool have_snap = readSnapshotFile(dir + "/" + SNAP_NAME, snap);
        if (!have_snap)
        {
            snap = Snapshot();
            have_snap = readSnapshotFile(dir + "/" + SNAP_PREV, snap);
            if (have_snap)
            {
                LL_INFOS("CacheStore") << "using previous index snapshot" << LL_ENDL;
            }
            else
            {
                snap = Snapshot();
            }
        }
        std::unordered_map<U32, const SnapSeg*> snap_segs;
        if (have_snap)
        {
            for (const SnapSeg& s : snap.segs) snap_segs[s.seq] = &s;
            std::vector<U32> slot_of(snap.regions.size());
            for (size_t i = 0; i < snap.regions.size(); ++i)
            {
                const SnapRegion& g = snap.regions[i];
                slot_of[i] = internRegion(g.id, g.handle);
                std::lock_guard<std::mutex> lock(region_mutex);
                RegionState& r = regions[slot_of[i]];
                r.last_visit = std::max(r.last_visit, g.last_visit);
                r.visit_count = std::max(r.visit_count, g.visit_count);
            }
            {
                std::lock_guard<std::mutex> lock(day_mutex);
                noteDay(now());
                if (snap.day == day)
                {
                    day_new += snap.day_new;
                    day_comp += snap.day_comp;
                    day_meta += snap.day_meta;
                }
            }
            // Where each segment's snapshot coverage ends (an entry must lie within it).
            struct SegLimit { U64 limit; size_t idx; };
            std::unordered_map<U32, SegLimit> seg_limit;
            std::vector<Segment*> seg_of;
            for (const auto& kv : snap_segs)
            {
                auto sit = segs.find(kv.first);
                if (sit == segs.end()) continue;
                seg_limit[kv.first] = { std::min<U64>(sit->second->size, kv.second->through), seg_of.size() };
                seg_of.push_back(sit->second.get());
            }
            // Live accounting of the loaded entries (addLive), summed per shard batch and
            // added before the shard lock is released.
            std::vector<std::pair<U64, U32>> seg_add(seg_of.size());
            U32 dropped = 0;
            for (size_t si = 0; si < SHARDS; ++si)
            {
                const std::vector<U32>& offs = snap.by_shard[si];
                if (offs.empty()) continue;
                U32 fmt[KIND_COUNT];
                size_t grp[KIND_COUNT];
                for (size_t k = 0; k < KIND_COUNT; ++k)
                {
                    KindPolicy pol = policy((Kind)k);
                    fmt[k] = pol.format_version;
                    grp[k] = (size_t)pol.group;
                }
                U64 k_live[KIND_COUNT] = {}, k_raw[KIND_COUNT] = {};
                U32 k_records[KIND_COUNT] = {};
                bool check_load;
                {
                    std::lock_guard<std::mutex> lock(load_mutex);
                    check_load = loading;
                }
                Shard& sh = shards[si];
                std::lock_guard<std::mutex> lock(sh.mutex);
                sh.map.reserve(sh.map.size() + offs.size());
                for (U32 off : offs)
                {
                    const U8* p = &snap.buf[off];
                    Key key;
                    memcpy(key.id.mData, p, UUID_BYTES);
                    key.kind = (Kind)p[16];
                    key.variant = F::get32(p + 20);
                    const U8 meta_len = std::min<U8>(p[19], (U8)F::MAX_META);
                    Entry se;
                    se.seg = F::get32(p + 28);
                    se.offset = F::get32(p + 32);
                    se.stored = F::get32(p + 36);
                    auto lit = seg_limit.find(se.seg);
                    if (lit == seg_limit.end() || (U64)se.offset + F::recordSize(meta_len, se.stored) > lit->second.limit ||
                        key.kind == Kind::SCRATCH || key.kind == Kind::NONE || key.kind >= Kind::COUNT ||
                        F::get32(p + 24) != fmt[ki(key.kind)])
                    {
                        ++dropped;
                        continue;
                    }
                    if (check_load)
                    {
                        std::lock_guard<std::mutex> lock(load_mutex);
                        if (removed_during_load.count(key) || cleared_during_load[ki(key.kind)]) continue;
                    }
                    se.flags = p[18] & EF_PERSIST;
                    se.setCodec(p[17]);
                    se.setKindVersion(F::get32(p + 24));
                    se.raw = F::get32(p + 40);
                    se.write_time = F::get32(p + 44);
                    se.last_access = F::get32(p + 48);
                    se.setPrevAccess(se.last_access);    // the previous session's last access
                    se.reuse = F::get16(p + 52);
                    const U32 region_index = F::get32(p + 56);
                    se.setRegion(region_index < slot_of.size() ? slot_of[region_index] : NO_REGION);
                    se.setRecency(F::get32(p + 60));
                    Entry* ep = sh.map.insert(key);
                    if (!ep)
                    {
                        ++dropped;
                        continue;
                    }
                    Entry& e = *ep;
                    U8 keep_flags = e.flags & (EF_PINNED | EF_TOUCHED);
                    subLive(key, e);
                    e.assignFrom(se);       // keeps a queued write (EF_PENDING) as before
                    e.flags |= keep_flags;
                    if (meta_len || e.meta_len) sh.map.setMeta(key, p + SNAP_ENTRY_FIXED, meta_len);
                    // addLive(key, e), batched
                    const U64 sz = recSize(e);
                    seg_add[lit->second.idx].first += sz;
                    seg_add[lit->second.idx].second++;
                    k_live[ki(key.kind)] += sz;
                    k_records[ki(key.kind)]++;
                    k_raw[ki(key.kind)] += e.raw;
                }
                for (size_t i = 0; i < seg_add.size(); ++i)
                {
                    if (!seg_add[i].second) continue;
                    seg_of[i]->live += seg_add[i].first;
                    seg_of[i]->live_records += seg_add[i].second;
                    seg_add[i] = { 0, 0 };
                }
                for (size_t k = 0; k < KIND_COUNT; ++k)
                {
                    if (!k_records[k]) continue;
                    group_live[grp[k]] += k_live[k];
                    group_records[grp[k]] += k_records[k];
                    kind_live[k] += k_live[k];
                    kind_records[k] += k_records[k];
                    kind_raw[k] += k_raw[k];
                }
            }
            if (dropped)
            {
                LL_INFOS("CacheStore") << "snapshot: dropped " << dropped << " stale entries" << LL_ENDL;
            }
            std::vector<U8>().swap(snap.buf);
            std::vector<std::vector<U32>>().swap(snap.by_shard);
        }

        // 3. scan what the snapshot does not cover, in log order
        U32 last_seq = segs.empty() ? 0 : segs.rbegin()->first;
        for (auto& kv : segs)
        {
            Segment& s = *kv.second;
            U64 start = F::SEG_HEADER_SIZE;
            auto wit = snap_segs.find(s.seq);
            if (wit != snap_segs.end() && wit->second->through <= s.size)
            {
                start = wit->second->through;
                for (size_t g = 0; g < GROUP_COUNT; ++g) s.group_bytes[g] = wit->second->group_bytes[g];
                for (size_t k = 0; k < KIND_COUNT; ++k) s.kind_bytes[k] = wit->second->kind_bytes[k];
            }
            // else: no snapshot info, or the file is shorter than the snapshot
            // says (lost tail): rescan the whole segment; the log-order rule
            // keeps newer snapshot entries.
            bool is_last = s.seq == last_seq;
            U64 good_end = scanSegment(s, start, /*verify*/ is_last, /*stop_on_bad*/ false, owner, true);
            if (is_last && owner && good_end < s.size)
            {
                // The half-written tail of the active segment (only the owner
                // truncates: for a reader it is the owner's append in progress).
                LL_INFOS("CacheStore") << "truncating torn tail of " << F::segmentFileName(s.seq) << ": "
                                       << s.size << " -> " << good_end << LL_ENDL;
                if (s.file.truncate(good_end))
                {
                    s.size = good_end;
                }
            }
            s.indexed_through = owner ? s.size.load() : good_end;
            // Bytes no scan attributed (segment header, garbage, torn tails).
            U64 attributed = 0;
            for (size_t g = 0; g < GROUP_COUNT; ++g) attributed += s.group_bytes[g];
            if (attributed < s.size) s.group_bytes[(size_t)Group::UNBUDGETED] += s.size - attributed;
        }

        // 4. totals
        U64 disk = 0;
        for (auto& kv : segs)
        {
            disk += kv.second->size;
            for (size_t g = 0; g < GROUP_COUNT; ++g) group_disk[g] += kv.second->group_bytes[g];
        }
        disk_bytes = disk;
        peak_disk = std::max(peak_disk.load(), disk);

        // 5. reopen the last segment for appends if it has room
        if (owner && !segs.empty())
        {
            SegmentPtr last = segs.rbegin()->second;
            if (last->header_ok && last->size < cfg.segment_bytes)
            {
                active = last;
            }
        }
        last_snapshot = now();
        index_dirty = !have_snap;
    }

    // ------------------------------------------------------------ appends (writer thread)

    S64 freeBytes()
    {
        if (cfg.free_space) return cfg.free_space();
        std::lock_guard<std::mutex> lock(free_mutex);
        auto t = std::chrono::steady_clock::now();
        U64 d = disk_bytes.load();
        if (cached_free < 0 || t - free_checked > std::chrono::seconds(2) ||
            (d > free_checked_disk ? d - free_checked_disk : free_checked_disk - d) > (32ull << 20))
        {
            cached_free = P::freeSpace(dir);
            free_checked = t;
            free_checked_disk = d;
        }
        return cached_free;
    }

    U64 budgetTotal() const
    {
        U64 sum = 0;
        for (size_t g = 0; g < (size_t)Group::UNBUDGETED; ++g) sum += budgets[g].load();
        return sum + group_disk[(size_t)Group::UNBUDGETED].load();
    }

    // min(sum of budgets, disk + free - reserve)
    U64 effectiveLimit()
    {
        U64 limit = budgetTotal();
        S64 fr = freeBytes();
        if (fr >= 0)
        {
            U64 res = disk_reserve.load();
            U64 room = (U64)fr > res ? (U64)fr - res : 0;
            limit = std::min(limit, disk_bytes.load() + room);
        }
        return limit;
    }

    // use_reserve: compaction re-appends may dip into the reserve -- they are
    // at most reclaim_live_max of a segment that is deleted right after.
    bool roomOnDisk(U64 need, bool use_reserve = false)
    {
        S64 fr = freeBytes();
        if (fr < 0) return true;
        U64 res = use_reserve ? 0 : disk_reserve.load();
        return (U64)fr > res && (U64)fr - res >= need;
    }

    void sealActive() { active.reset(); }

    SegmentPtr newSegment()
    {
        U32 seq = next_seq++;
        auto s = std::make_shared<Segment>();
        s->seq = seq;
        s->path = segPath(seq);
        if (!s->file.open(s->path, P::File::CREATE_NEW_RW))
        {
            LL_WARNS("CacheStore") << "cannot create " << s->path << " (" << s->file.lastError() << ")" << LL_ENDL;
            return SegmentPtr();
        }
        U8 hdr[F::SEG_HEADER_SIZE];
        F::encodeSegmentHeader(seq, now(), hdr);
        size_t accept = cfg.write_fault ? std::min(cfg.write_fault(seq, 0, sizeof(hdr)), sizeof(hdr)) : sizeof(hdr);
        if (accept < sizeof(hdr) || s->file.pwrite(hdr, sizeof(hdr), 0) != (S64)sizeof(hdr))
        {
            s->file.close();
            P::removeFile(s->path);
            return SegmentPtr();
        }
        s->size = F::SEG_HEADER_SIZE;
        s->group_bytes[(size_t)Group::UNBUDGETED] = F::SEG_HEADER_SIZE;
        {
            std::unique_lock<std::shared_mutex> lock(seg_mutex);
            segments[seq] = s;
        }
        disk_bytes += F::SEG_HEADER_SIZE;
        group_disk[(size_t)Group::UNBUDGETED] += F::SEG_HEADER_SIZE;
        countWrite(F::SEG_HEADER_SIZE, WriteClass::META);
        return s;
    }

    // Append one contiguous run. On success *seg / *base get the location.
    Status appendRun(const std::vector<U8>& buf, WriteClass wc, SegmentPtr* seg_out, U64* base_out)
    {
        U64 need = buf.size();
        if (!no_enforce && disk_bytes.load() + need > effectiveLimit())
        {
            enforceBudgets(need);
        }
        if (!roomOnDisk(need, wc == WriteClass::COMPACTION))
        {
            if (wc != WriteClass::COMPACTION) disk_full_until = now() + DISK_FULL_BACKOFF_S;
            return Status::NO_SPACE;
        }
        if (active && active->size + need > cfg.segment_bytes && active->size > F::SEG_HEADER_SIZE)
        {
            sealActive();
        }
        if (!active)
        {
            active = newSegment();
            if (!active)
            {
                disk_full_until = now() + DISK_FULL_BACKOFF_S;
                return Status::IO_ERROR;
            }
        }
        Segment& s = *active;
        U64 off = s.size;
        size_t accept = cfg.write_fault ? std::min(cfg.write_fault(s.seq, off, buf.size()), buf.size()) : buf.size();
        const auto t_put = std::chrono::steady_clock::now();
        S64 put = accept ? s.file.pwrite(buf.data(), accept, off) : 0;
        append_usec += usecSince(t_put);
        append_count++;
        if (accept < buf.size() || put != (S64)buf.size())
        {
            bool nospace = accept < buf.size() || s.file.lastErrorWasNoSpace();
            // Remove the partial bytes; a failure to truncate leaves a torn
            // record the scan skips (header hash / bounds).
            s.file.truncate(off);
            write_fail++;
            if (nospace) disk_full_until = now() + DISK_FULL_BACKOFF_S;
            LL_WARNS("CacheStore") << "append of " << buf.size() << " bytes to " << F::segmentFileName(s.seq)
                                   << " failed" << (nospace ? " (disk full)" : "") << LL_ENDL;
            sealActive();   // start fresh next time
            return nospace ? Status::NO_SPACE : Status::IO_ERROR;
        }
        s.size = off + buf.size();
        append_bytes += buf.size();
        disk_bytes += buf.size();
        peak_disk = std::max(peak_disk.load(), disk_bytes.load());
        countWrite(buf.size(), wc);
        *seg_out = active;
        *base_out = off;
        return Status::OK;
    }

    static void addRecordBytes(std::vector<U8>& run, const F::RecordHeader& h, const U8* meta, const U8* payload)
    {
        size_t at = run.size();
        U64 total = h.totalSize();
        run.resize(at + total, 0);
        F::encodeRecordHeader(h, meta, &run[at]);
        if (h.stored)
        {
            memcpy(&run[at + F::REC_HEADER_SIZE + h.meta_len], payload, h.stored);
        }
    }

    void attribute(Segment& s, Kind kind, bool control, U64 bytes)
    {
        size_t g = control ? (size_t)Group::UNBUDGETED : (size_t)groupOf(kind);
        s.group_bytes[g] += bytes;
        s.kind_bytes[std::min<size_t>((size_t)kind, KIND_COUNT - 1)] += bytes;
        group_disk[g] += bytes;
    }

    // Materialise + encode a queued write (writer thread).
    bool encodePending(Pending& p)
    {
        p.releaseBorrowed();
        if (p.control) { p.encoded = true; return true; }
        KindPolicy pol = policy(p.key.kind);
        std::vector<U8> out;
        U8 codec;
        if (pol.ranged_reads)
        {
            codec = F::CODEC_NONE;
            std::lock_guard<std::mutex> lock(p.mutex);
            out = p.raw;
        }
        else
        {
            LLCacheCodec::Policy cp;
            cp.enabled = pol.compress;
            cp.level = pol.level;
            cp.allow_legacy = false;
            codec = F::CODEC_SHIM;
            // raw is not modified after materialisation; reading it unlocked
            // alongside readers (who also only read it) is safe.
            const auto t_enc = std::chrono::steady_clock::now();
            const bool enc_ok = LLCacheCodec::encodeWith(codecKind(p.key.kind), cp, p.raw.data(), p.raw.size(), out);
            encode_usec += usecSince(t_enc);
            encode_count++;
            if (!enc_ok)
            {
                return false;
            }
            encode_in += p.raw.size();
            encode_out += out.size();
        }
        U64 hash = HBXXH64::digest(out.data(), out.size());
        std::lock_guard<std::mutex> lock(p.mutex);
        p.payload = std::move(out);
        p.codec = codec;
        p.payload_hash = hash;
        p.encoded = true;
        p.raw.clear();
        p.raw.shrink_to_fit();
        return true;
    }

    bool isCurrent(const PendingPtr& p)
    {
        if (p->control) return true;
        Key k;
        {
            std::lock_guard<std::mutex> lock(p->mutex);
            k = p->key;
        }
        Shard& sh = shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        const Entry* e = sh.map.get(k);
        return e && e->pending() && sh.isPending(k, *e, p);
    }

    void finishPending(const PendingPtr& p, Status st)
    {
        queued_bytes -= std::min<U64>(queued_bytes.load(), p->raw_size);
        if (queued_count) queued_count--;
        detail::complete(p->done, st);
    }

    void intakeOne(const PendingPtr& p)
    {
        if (!isCurrent(p))
        {
            p->releaseBorrowed();
            superseded++;
            finishPending(p, Status::SKIPPED);
            return;
        }
        if (!encodePending(*p))
        {
            dropPending(p);
            write_fail++;
            finishPending(p, Status::IO_ERROR);
            return;
        }
        Bucket& b = buckets[p->region_slot];
        if (b.recs.empty()) b.first = std::chrono::steady_clock::now();
        b.recs.push_back(p);
        b.bytes += F::recordSize(p->meta_len, (U32)p->payload.size());
        if (p->prio == Priority::URGENT || b.bytes >= cfg.run_flush_bytes)
        {
            flushBucket(p->region_slot);
        }
    }

    // Remove a pending write's index entry if it is still the current one.
    void dropPending(const PendingPtr& p)
    {
        if (p->control) return;
        Key k;
        {
            std::lock_guard<std::mutex> lock(p->mutex);
            k = p->key;
        }
        Shard& sh = shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(k);
        if (it != sh.map.end() && it->second.pending() && sh.isPending(k, it->second, p))
        {
            sh.setPending(k, it->second, nullptr);
            if (!it->second.seg) sh.erase(it);
        }
    }

    void flushBucket(U32 slot)
    {
        auto bit = buckets.find(slot);
        if (bit == buckets.end()) return;
        std::vector<PendingPtr> recs = std::move(bit->second.recs);
        buckets.erase(bit);
        writeRun(recs, WriteClass::NEW);
    }

    void flushDue(bool all)
    {
        auto t = std::chrono::steady_clock::now();
        std::vector<U32> due;
        for (auto& kv : buckets)
        {
            if (all || kv.second.bytes >= cfg.run_flush_bytes ||
                t - kv.second.first >= std::chrono::milliseconds(cfg.run_flush_ms))
            {
                due.push_back(kv.first);
            }
        }
        for (U32 slot : due) flushBucket(slot);
    }

    void writeRun(std::vector<PendingPtr>& recs, WriteClass wc)
    {
        std::vector<U8> run;
        struct Placed { PendingPtr p; Key key; U64 at; U64 size; };
        std::vector<Placed> placed;
        for (PendingPtr& p : recs)
        {
            if (!isCurrent(p))
            {
                superseded++;
                finishPending(p, Status::SKIPPED);
                continue;
            }
            F::RecordHeader h;
            std::lock_guard<std::mutex> lock(p->mutex);
            h.kind = (U8)p->key.kind;
            h.id = p->key.id;
            h.variant = p->key.variant;
            h.write_time = p->write_time;
            if (p->control)
            {
                h.flags = p->control_flags;
            }
            else
            {
                h.flags = p->region.isNull() ? F::FLAG_SHARED : 0;
                h.codec = p->codec;
                h.kind_version = p->kind_version;
                h.meta_len = p->meta_len;
                h.stored = (U32)p->payload.size();
                h.raw = p->raw_size;
                h.region = p->region;
                h.grid_handle = p->grid_handle;
                h.payload_hash = p->payload_hash;
            }
            U64 at = run.size();
            addRecordBytes(run, h, p->meta, p->payload.data());
            placed.push_back({ p, p->key, at, h.totalSize() });
        }
        if (placed.empty()) return;
        SegmentPtr seg;
        U64 base = 0;
        Status st = appendRun(run, wc, &seg, &base);
        if (st != Status::OK)
        {
            for (Placed& pl : placed) failPending(pl.p, st);
            return;
        }
        for (Placed& pl : placed)
        {
            attribute(*seg, pl.key.kind, pl.p->control, pl.size);
            landPending(pl.p, seg->seq, base + pl.at, pl.key);
            finishPending(pl.p, Status::OK);
        }
    }

    void landPending(const PendingPtr& p, U32 seq, U64 off, const Key& written_key)
    {
        if (p->control) return;
        Key k;
        {
            std::lock_guard<std::mutex> lock(p->mutex);
            k = p->key;
        }
        Shard& sh = shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(k);
        if (it == sh.map.end() || !it->second.pending() || !sh.isPending(k, it->second, p))
        {
            superseded++;       // replaced or removed meanwhile: born dead
            return;
        }
        Entry& e = it->second;
        subLive(k, e);
        std::lock_guard<std::mutex> plock(p->mutex);
        e.seg = seq;
        e.offset = (U32)off;
        e.stored = (U32)p->payload.size();
        e.raw = p->raw_size;
        e.setKindVersion(p->kind_version);
        e.write_time = p->write_time;
        e.setCodec(p->codec);
        sh.map.setMetaAt(it.pos(), p->meta, p->meta_len);
        e.setRegion(p->region_slot);
        if (written_key != k) e.flags |= EF_RENAMED;   // renamed while queued
        else e.flags &= ~EF_RENAMED;
        sh.setPending(k, e, nullptr);
        addLive(k, e);
    }

    void failPending(const PendingPtr& p, Status st)
    {
        if (!p->control)
        {
            Key k;
            {
                std::lock_guard<std::mutex> lock(p->mutex);
                k = p->key;
            }
            Shard& sh = shardFor(k);
            std::lock_guard<std::mutex> lock(sh.mutex);
            auto it = sh.map.find(k);
            if (it != sh.map.end() && it->second.pending() && sh.isPending(k, it->second, p))
            {
                Entry& e = it->second;
                U64 sz = p->payload.size();
                // Asset delivery (#711): a pinned record stays readable from
                // RAM until its consumer reads it, within a bound.
                if ((e.flags & EF_PINNED) && ram_fallback.load() + sz <= cfg.ram_fallback_bytes)
                {
                    e.flags |= EF_RAM_ONLY;
                    ram_fallback += sz;
                }
                else
                {
                    sh.setPending(k, e, nullptr);
                    if (!e.seg) sh.erase(it);
                }
            }
        }
        finishPending(p, st);
    }

    // ------------------------------------------------------------ eviction (writer thread)

    struct Candidate
    {
        Key key; U32 seg; U32 off; U64 size; int tier; F32 score;
    };

    // Mark dead the lowest (tier, score) records matching the filter until
    // `bytes` live bytes are freed. Never pinned or queued records.
    U64 markDeadLowest(Group g, Kind only_kind, U64 bytes)
    {
        if (!bytes) return 0;
        std::shared_ptr<EvictionPolicy> pol = evict_policy[(size_t)g];
        if (!pol) pol = std::make_shared<EvictionPolicy>();
        std::vector<U32> visits = regionVisitTimes();
        U32 t = now();
        std::vector<Candidate> cands;
        for (Shard& sh : shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
            {
                const Entry& e = kv.second;
                if (!e.seg || e.pending() || (e.flags & EF_PINNED)) continue;
                if (only_kind != Kind::NONE && kv.first.kind != only_kind) continue;
                if (groupOf(kv.first.kind) != g) continue;
                RecordInfo r;
                r.key = kv.first;
                r.stored_bytes = e.stored;
                r.raw_bytes = e.raw;
                r.write_time = e.write_time;
                r.last_access = e.last_access;
                r.prev_session_access = e.prevAccess();
                r.reuse_count = e.reuse;
                r.shared = (e.flags & EF_SHARED) != 0;
                if (const U8* meta = sh.map.metaAt(kv.pos))
                {
                    r.meta_len = e.meta_len;
                    memcpy(r.meta, meta, e.meta_len);
                }
                EvictionContext ctx;
                ctx.now = t;
                ctx.region_recency = e.recency();
                const U32 region = e.region();
                if (region != NO_REGION && region < visits.size())
                    ctx.region_recency = std::max(ctx.region_recency, visits[region]);
                ctx.decode_cost = policy(kv.first.kind).decode_cost;
                cands.push_back({ kv.first, e.seg, e.offset, recSize(e), pol->tier(r), pol->keepScore(r, ctx) });
            }
        }
        // Ties (same tier and score) go oldest first, in log order.
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b)
                  {
                      if (a.tier != b.tier) return a.tier < b.tier;
                      if (a.score != b.score) return a.score < b.score;
                      return a.seg != b.seg ? a.seg < b.seg : a.off < b.off;
                  });
        U64 freed = 0;
        for (const Candidate& c : cands)
        {
            if (freed >= bytes) break;
            if (killAt(c.key, c.seg, c.off))
            {
                freed += c.size;
                countEvict(c.key.kind, c.size);
            }
        }
        return freed;
    }

    // Drop an entry if it still points at (seg, off) and has no newer bytes.
    bool killAt(const Key& k, U32 seg, U32 off)
    {
        Shard& sh = shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(k);
        if (it == sh.map.end() || it->second.seg != seg || it->second.offset != off) return false;
        subLive(k, it->second);
        if (!it->second.pending()) sh.erase(it);
        return true;
    }

    void deleteSegment(const SegmentPtr& s)
    {
        if (s == active) sealActive();
        {
            std::unique_lock<std::shared_mutex> lock(seg_mutex);
            if (!segments.erase(s->seq)) return;    // already gone: never count twice
        }
        disk_bytes -= std::min<U64>(disk_bytes.load(), s->size.load());
        for (size_t g = 0; g < GROUP_COUNT; ++g)
            group_disk[g] -= std::min<U64>(group_disk[g].load(), s->group_bytes[g].load());
        {
            std::lock_guard<std::mutex> lock(ra_mutex);
            for (auto it = ra_chunks.lower_bound({ s->seq, 0 }); it != ra_chunks.end() && it->first.first == s->seq;)
            {
                ra_bytes -= it->second.size();
                it = ra_chunks.erase(it);
            }
        }
        // Readers holding the SegmentPtr keep their handle (POSIX unlink /
        // Windows FILE_SHARE_DELETE); the name is never reused.
        P::removeFile(s->path);
        {
            std::lock_guard<std::mutex> lock(free_mutex);
            cached_free = -1;      // space came back
        }
        segments_reclaimed++;
        index_dirty = true;
    }

    std::vector<SegmentPtr> segmentList()
    {
        std::shared_lock<std::shared_mutex> lock(seg_mutex);
        std::vector<SegmentPtr> v;
        for (auto& kv : segments) v.push_back(kv.second);
        return v;
    }

    U32 deleteDeadSegments()
    {
        U32 n = 0;
        for (SegmentPtr& s : segmentList())
        {
            if (s != active && s->live_records.load() == 0)
            {
                deleteSegment(s);
                ++n;
            }
        }
        return n;
    }

    struct LiveRec { Key key; U32 off; U64 size; U16 reuse; U8 flags; U32 region; bool queued; };

    std::vector<LiveRec> liveRecordsIn(U32 seq)
    {
        std::vector<LiveRec> v;
        for (Shard& sh : shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
            {
                const Entry& e = kv.second;
                if (e.seg == seq)
                    v.push_back({ kv.first, e.offset, recSize(e), e.reuse, e.flags, e.region(), e.pending() });
            }
        }
        std::sort(v.begin(), v.end(), [](const LiveRec& a, const LiveRec& b)
                  { return a.region != b.region ? a.region < b.region : a.off < b.off; });
        return v;
    }

    // Re-append records (verbatim, flagged COMPACTED) grouped by region;
    // move index entries still pointing at the old copies.
    U32 reappend(const SegmentPtr& src, const std::vector<LiveRec>& recs)
    {
        NoEnforce guard(no_enforce);    // rescue appends are net-negative on disk
        U32 moved = 0;
        size_t i = 0;
        while (i < recs.size())
        {
            std::vector<U8> run;
            struct Item { const LiveRec* r; U64 at; U64 size; Kind kind; };
            std::vector<Item> items;
            U32 region = recs[i].region;
            while (i < recs.size() && recs[i].region == region && run.size() < cfg.run_flush_bytes)
            {
                const LiveRec& r = recs[i++];
                std::vector<U8> rec((size_t)r.size);
                if (src->file.pread(rec.data(), rec.size(), r.off) != (S64)rec.size()) continue;
                F::RecordHeader h;
                if (F::parseRecordHeaderFixed(rec.data(), h) != F::HeaderCheck::OK ||
                    !F::verifyRecordHeader(rec.data(), rec.data() + F::REC_HEADER_SIZE, h.meta_len) ||
                    h.totalSize() != r.size)
                {
                    continue;   // the old copy is bad; it dies with the segment
                }
                h.flags |= F::FLAG_COMPACTED;
                F::encodeRecordHeader(h, rec.data() + F::REC_HEADER_SIZE, rec.data());
                items.push_back({ &r, run.size(), r.size, r.key.kind });
                run.insert(run.end(), rec.begin(), rec.end());
            }
            if (items.empty()) continue;
            SegmentPtr seg;
            U64 base = 0;
            if (appendRun(run, WriteClass::COMPACTION, &seg, &base) != Status::OK) return moved;
            for (Item& it : items)
            {
                attribute(*seg, it.kind, false, it.size);
                Shard& sh = shardFor(it.r->key);
                std::lock_guard<std::mutex> lock(sh.mutex);
                auto eit = sh.map.find(it.r->key);
                if (eit == sh.map.end() || eit->second.seg != src->seq || eit->second.offset != it.r->off) continue;
                Entry& e = eit->second;
                subLive(it.r->key, e);
                e.seg = seg->seq;
                e.offset = (U32)(base + it.at);
                addLive(it.r->key, e);
                ++moved;
            }
        }
        return moved;
    }

    // Reclaim one segment: rescue what the rules allow, evict the rest, delete.
    void reclaimSegment(const SegmentPtr& s, bool rescue_all, bool allow_rescue)
    {
        if (s == active) sealActive();
        std::vector<LiveRec> recs = liveRecordsIn(s->seq);
        std::vector<LiveRec> rescue;
        U64 allowance = rescue_all ? ~0ull : (allow_rescue ? compactionAllowance() : 0);
        for (const LiveRec& r : recs)
        {
            bool must = (r.flags & EF_PINNED) || r.key.kind == Kind::SCRATCH;
            bool want = rescue_all || must || (r.reuse > 0 && !r.queued && r.size <= allowance);
            if (want && !r.queued)
            {
                rescue.push_back(r);
                if (!rescue_all && !must) allowance -= r.size;
            }
        }
        U32 moved = reappend(s, rescue);
        rescued_records += moved;
        // Everything still pointing here dies with the segment.
        for (const LiveRec& r : liveRecordsIn(s->seq))
        {
            if (killAt(r.key, s->seq, r.off))
            {
                countEvict(r.key.kind, r.size);
            }
        }
        deleteSegment(s);
    }

    bool groupOver(size_t g) const
    {
        U64 b = budgets[g].load();
        return g < (size_t)Group::UNBUDGETED && group_disk[g].load() > b;
    }

    // Budget / disk-guard pass (#709 rules). incoming = bytes about to be appended.
    void enforceBudgets(U64 incoming)
    {
        if (ro || no_enforce) return;
        NoEnforce guard(no_enforce);
        // share caps (#705: GPU-ready share of Textures)
        for (size_t k = 1; k < KIND_COUNT; ++k)
        {
            KindPolicy pol = policy((Kind)k);
            if (pol.share_cap >= 1.f || pol.group == Group::UNBUDGETED) continue;
            U64 cap = (U64)((F64)budgets[(size_t)pol.group].load() * pol.share_cap);
            U64 live = kind_live[k].load();
            if (live > cap) markDeadLowest(pol.group, (Kind)k, live - cap);
        }
        // The disk-guard limit is fixed for the pass: bytes freed by deleting
        // segments become free space, so they do not lower it.
        const U64 limit = effectiveLimit();
        for (int iter = 0; iter < 64; ++iter)
        {
            bool total_over = disk_bytes.load() + incoming > limit;
            int worst = -1;
            F64 worst_ratio = 1.0;
            for (size_t g = 0; g < (size_t)Group::UNBUDGETED; ++g)
            {
                U64 b = std::max<U64>(budgets[g].load(), 1);
                F64 ratio = (F64)group_disk[g].load() / (F64)b;
                if (groupOver(g) && ratio > worst_ratio) { worst_ratio = ratio; worst = (int)g; }
            }
            if (worst < 0 && total_over)
            {
                // Over the disk guard but no group over its own budget: take the fullest.
                F64 best = -1;
                for (size_t g = 0; g < (size_t)Group::UNBUDGETED; ++g)
                {
                    U64 b = std::max<U64>(budgets[g].load(), 1);
                    F64 ratio = (F64)group_disk[g].load() / (F64)b;
                    if (group_disk[g].load() && ratio > best) { best = ratio; worst = (int)g; }
                }
            }
            if (worst < 0) return;
            size_t g = (size_t)worst;

            // 1. fully dead segments go first (always, whatever the cap)
            if (deleteDeadSegments()) continue;

            // 2. mostly dead segments (<= reclaim_live_max live), most of this group first
            const F32 reclaim_max = wearCopy().reclaim_live_max;
            SegmentPtr best;
            U64 best_bytes = 0;
            for (SegmentPtr& s : segmentList())
            {
                if (s == active && s->size < cfg.segment_bytes / 2) continue;
                U64 sz = std::max<U64>(s->size.load(), 1);
                if ((F64)s->live.load() / (F64)sz > reclaim_max) continue;
                U64 gb = s->group_bytes[g].load();
                if (gb > best_bytes) { best = s; best_bytes = gb; }
            }
            if (best)
            {
                reclaimSegment(best, false, true);
                continue;
            }

            // 3. mark this group's least valuable records dead: live down to
            //    80% of its budget, and at least the disk-guard overage, so
            //    step 4 below finds segments made mostly of low-value records.
            U64 b = budgets[g].load();
            U64 target = (U64)((F64)b * 0.8);
            U64 live = group_live[g].load();
            U64 want = live > target ? live - target : 0;
            if (total_over)
            {
                U64 over = disk_bytes.load() + incoming - limit;
                want = std::max(want, std::min(live, over + over / 8));
            }
            if (want && markDeadLowest((Group)g, Kind::NONE, want)) continue;

            // 4. still over: evict whole segments without rescue (past the
            //    rules: extra misses, never extra wear), least live first.
            SegmentPtr victim;
            F64 victim_live = 2.0;
            for (SegmentPtr& s : segmentList())
            {
                if (s->group_bytes[g].load() == 0) continue;
                U64 sz = std::max<U64>(s->size.load(), 1);
                F64 lf = (F64)s->live.load() / (F64)sz;
                if (lf < victim_live) { victim = s; victim_live = lf; }
            }
            if (!victim) return;
            reclaimSegment(victim, false, false);
        }
    }

    // ------------------------------------------------------------ snapshot (writer thread)

    bool writeSnapshot()
    {
        if (ro) return false;
        if (active) active->file.sync();
        F::Writer w;
        w.bytes("SLCI", 4);
        w.u16(1);
        w.u16(0);
        w.u32(now());
        {
            std::lock_guard<std::mutex> lock(day_mutex);
            noteDay(now());
            w.u32(day);
            w.u64(day_new);
            w.u64(day_comp);
            w.u64(day_meta);
        }
        std::vector<SegmentPtr> segs = segmentList();
        std::vector<RegionState> regs;
        {
            std::lock_guard<std::mutex> lock(region_mutex);
            for (const RegionState& r : regions)
            {
                RegionState c;
                c.id = r.id;
                c.handle = r.handle;
                c.last_visit = r.last_visit;
                c.visit_count = r.visit_count;
                regs.push_back(std::move(c));
            }
        }
        size_t count_at = w.buf.size();
        w.u32((U32)segs.size());
        w.u32((U32)regs.size());
        w.u32(0);   // entry count, patched below
        for (SegmentPtr& s : segs)
        {
            w.u32(s->seq);
            w.u64(s->size.load());
            for (auto& g : s->group_bytes) w.u64(g.load());
            for (auto& k : s->kind_bytes) w.u64(k.load());
        }
        for (RegionState& r : regs)
        {
            w.uuid(r.id);
            w.u64(r.handle);
            w.u32(r.last_visit);
            w.u32(r.visit_count);
        }
        U32 n = 0;
        {
            size_t records = 0;
            for (const Shard& sh : shards)
            {
                std::lock_guard<std::mutex> lock(sh.mutex);
                records += sh.map.size();
            }
            w.buf.reserve(w.buf.size() + records * (SNAP_ENTRY_FIXED + 4) + 4096);
        }
        for (Shard& sh : shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
            {
                const Entry& e = kv.second;
                if (!e.seg || kv.first.kind == Kind::SCRATCH) continue;
                const U8* meta = e.meta_len ? sh.map.metaAt(kv.pos) : nullptr;
                const U8 meta_len = meta ? e.meta_len : 0;
                const size_t at = w.buf.size();
                w.buf.resize(at + SNAP_ENTRY_FIXED + meta_len);
                U8* o = &w.buf[at];
                memcpy(o, kv.first.id.mData, UUID_BYTES);
                o[16] = (U8)kv.first.kind;
                o[17] = e.codec();
                o[18] = e.flags & EF_PERSIST;
                o[19] = meta_len;
                F::put32(o + 20, kv.first.variant);
                F::put32(o + 24, e.kindVersion());
                F::put32(o + 28, e.seg);
                F::put32(o + 32, e.offset);
                F::put32(o + 36, e.stored);
                F::put32(o + 40, e.raw);
                F::put32(o + 44, e.write_time);
                F::put32(o + 48, std::max(e.last_access, e.prevAccess()));
                F::put16(o + 52, e.reuse);
                F::put16(o + 54, 0);
                F::put32(o + 56, e.region());
                F::put32(o + 60, e.recency());
                if (meta_len) memcpy(o + SNAP_ENTRY_FIXED, meta, meta_len);
                ++n;
            }
        }
        F::put32(&w.buf[count_at + 8], n);
        w.u64(HBXXH64::digest(w.buf.data(), w.buf.size()));

        std::string tmp = dir + "/" + SNAP_TMP;
        P::removeFile(tmp);
        P::File f;
        if (!f.open(tmp, P::File::CREATE_NEW_RW) ||
            f.pwrite(w.buf.data(), w.buf.size(), 0) != (S64)w.buf.size() || !f.sync())
        {
            f.close();
            P::removeFile(tmp);
            LL_WARNS("CacheStore") << "index snapshot write failed" << LL_ENDL;
            return false;
        }
        f.close();
        std::string cur = dir + "/" + SNAP_NAME;
        if (P::fileExists(cur)) P::renameFile(cur, dir + "/" + SNAP_PREV);
        bool ok = P::renameFile(tmp, cur);
        P::syncDir(dir);
        countWrite(w.buf.size(), WriteClass::META);
        last_snapshot = now();
        index_dirty = false;
        return ok;
    }

    // ------------------------------------------------------------ read-only refresh

    void refresh()
    {
        std::set<U32> on_disk;
        for (const std::string& name : P::listDir(dir))
        {
            U32 seq;
            if (F::parseSegmentFileName(name, seq)) on_disk.insert(seq);
        }
        // segments the owner deleted
        for (SegmentPtr& s : segmentList())
        {
            if (on_disk.count(s->seq)) continue;
            for (Shard& sh : shards)
            {
                std::lock_guard<std::mutex> lock(sh.mutex);
                for (auto it = sh.map.begin(); it != sh.map.end();)
                {
                    if (it->second.seg == s->seq)
                    {
                        subLive(it->first, it->second);
                        it = sh.erase(it);
                    }
                    else ++it;
                }
            }
            std::unique_lock<std::shared_mutex> lock(seg_mutex);
            disk_bytes -= std::min<U64>(disk_bytes.load(), s->size.load());
            segments.erase(s->seq);
        }
        // new segments and grown tails, in log order
        for (U32 seq : on_disk)
        {
            SegmentPtr s = findSeg(seq);
            if (!s)
            {
                s = std::make_shared<Segment>();
                s->seq = seq;
                s->path = segPath(seq);
                if (!s->file.open(s->path, P::File::READ)) continue;
                s->indexed_through = F::SEG_HEADER_SIZE;
                std::unique_lock<std::shared_mutex> lock(seg_mutex);
                segments[seq] = s;
            }
            S64 sz = s->file.size();
            if (sz < 0 || (U64)sz <= s->indexed_through) continue;
            U64 old = s->size.load();
            s->size = (U64)sz;
            disk_bytes += (U64)sz > old ? (U64)sz - old : 0;
            s->indexed_through = scanSegment(*s, s->indexed_through, true, true, false, true);
        }
    }

    // ------------------------------------------------------------ reads

    bool readAheadLookup(U32 seq, U32 off, U64 len, std::vector<U8>& out)
    {
        if (!buffer_mode) return false;
        std::lock_guard<std::mutex> lock(ra_mutex);
        if (ra_chunks.empty()) return false;
        auto it = ra_chunks.upper_bound({ seq, off });
        if (it == ra_chunks.begin()) return false;
        --it;
        if (it->first.first != seq || it->first.second > off) return false;
        U64 rel = off - it->first.second;
        if (rel + len > it->second.size()) return false;
        out.assign(it->second.begin() + rel, it->second.begin() + rel + len);
        prefetch_hits++;
        return true;
    }

    // Stage 1 of an on-disk read (reader thread, or the caller for readSync): pread,
    // then everything that needs no decode -- the header (hash, key, sizes) and the
    // stored payload's hash. Codec 0 ends here (c.res.bytes filled). Codec 1 leaves the
    // blob in c.buf (need_decode) after a structural peek; readDecode finishes it.
    // ranged: [roff, roff+rlen) of the decoded bytes.
    Status readDiskIO(ReadCtx& c, U32 stored, U8 meta_len, U8 codec, bool renamed)
    {
        const Key& k = c.key;
        SegmentPtr s = findSeg(c.seq);
        if (!s) return Status::MISSING;
        demand_in_flight++;
        struct Done { std::atomic<U32>& c; ~Done() { c--; } } done{ demand_in_flight };
        size_t hdr_len = F::REC_HEADER_SIZE + meta_len;
        bool partial = c.ranged && codec == F::CODEC_NONE;
        size_t want = partial ? hdr_len : hdr_len + stored;
        std::vector<U8>& buf = c.buf;
        if (!readAheadLookup(c.seq, c.off, want, buf))
        {
            buf.resize(want);
            S64 got = s->file.pread(buf.data(), want, c.off);
            if (got < 0) return Status::IO_ERROR;
            if ((size_t)got < want) return Status::CORRUPT;
        }
        bytes_read += want;
        F::RecordHeader h;
        if (F::parseRecordHeaderFixed(buf.data(), h) != F::HeaderCheck::OK ||
            h.meta_len != meta_len || h.stored != stored || h.codec != codec ||
            (h.flags & (F::FLAG_TOMBSTONE | F::FLAG_CLEAR_KIND)) ||
            !F::verifyRecordHeader(buf.data(), buf.data() + F::REC_HEADER_SIZE, meta_len))
        {
            return Status::CORRUPT;
        }
        if (!renamed && (h.id != k.id || h.kind != (U8)k.kind || h.variant != k.variant))
        {
            return Status::CORRUPT;
        }
        std::vector<U8>& out = c.res.bytes;
        if (partial)
        {
            U64 a = std::min<U64>(c.roff, c.raw);
            U64 n = std::min<U64>(c.rlen, c.raw - a);
            out.resize((size_t)n);
            if (n)
            {
                S64 got = s->file.pread(out.data(), (size_t)n, c.off + hdr_len + a);
                if (got < 0) return Status::IO_ERROR;
                if ((U64)got < n) return Status::CORRUPT;
                bytes_read += n;
            }
            return Status::OK;
        }
        const U8* pl = buf.data() + hdr_len;
        if (HBXXH64::digest(pl, stored) != h.payload_hash) return Status::CORRUPT;
        if (codec == F::CODEC_NONE)
        {
            if (stored != c.raw) return Status::CORRUPT;
            out.assign(pl, pl + stored);
            return Status::OK;
        }
        LLCacheCodec::RecordInfo ci;
        if (!LLCacheCodec::peek(pl, stored, ci) || ci.legacy || ci.uncompressed_size != c.raw)
            return Status::CORRUPT;
        c.need_decode = true;
        c.zstd = ci.codec == LLCacheCodec::CODEC_ZSTD;
        c.pl_off = hdr_len;
        c.stored = stored;
        return Status::OK;
    }

    // Stage 2 (codec 1): decode the blob readDiskIO verified. A decode failure (the
    // codec's own hash of the decoded bytes, a bad zstd frame) is CORRUPT like any other.
    void readDecode(ReadCtx& c)
    {
        c.need_decode = false;
        std::vector<U8>& out = c.res.bytes;
        LLCacheCodec::Policy cp;
        cp.allow_legacy = false;
        const auto t_dec = std::chrono::steady_clock::now();
        const bool dec_ok = LLCacheCodec::decodeWith(codecKind(c.key.kind), cp, c.buf.data() + c.pl_off, c.stored, out);
        decode_usec += usecSince(t_dec);
        decode_count++;
        std::vector<U8>().swap(c.buf);   // the blob is not needed any more
        if (!dec_ok)
        {
            out.clear();
            c.st = Status::CORRUPT;
            return;
        }
        decode_in += c.stored;
        decode_out += out.size();
        if (c.ranged)
        {
            U64 a = std::min<U64>(c.roff, out.size());
            U64 n = std::min<U64>(c.rlen, out.size() - a);
            std::vector<U8> slice(out.begin() + a, out.begin() + a + n);
            out.swap(slice);
        }
    }

    Status readPending(const Key& k, Pending& p, bool ranged, U64 roff, U64 rlen, std::vector<U8>& out)
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (!p.encoded)
        {
            const U8* src = p.borrowed ? p.borrowed : p.raw.data();
            size_t n = p.borrowed ? p.borrowed_n : p.raw.size();
            out.assign(src, src + n);
        }
        else if (p.codec == F::CODEC_NONE)
        {
            out = p.payload;
        }
        else
        {
            LLCacheCodec::Policy cp;
            cp.allow_legacy = false;
            const auto t_dec = std::chrono::steady_clock::now();
            const bool dec_ok = LLCacheCodec::decodeWith(codecKind(k.kind), cp, p.payload.data(), p.payload.size(), out);
            decode_usec += usecSince(t_dec);
            decode_count++;
            if (!dec_ok) return Status::CORRUPT;
            decode_in += p.payload.size();
            decode_out += out.size();
        }
        if (ranged)
        {
            U64 a = std::min<U64>(roff, out.size());
            U64 n = std::min<U64>(rlen, out.size() - a);
            std::vector<U8> slice(out.begin() + a, out.begin() + a + n);
            out.swap(slice);
        }
        return Status::OK;
    }

    void touchLocked(Entry& e, U32 t)
    {
        if (!(e.flags & EF_TOUCHED))
        {
            e.flags |= EF_TOUCHED;
            if (e.prevAccess() && e.reuse < 0xffff) e.reuse++;
        }
        e.last_access = t;
        index_dirty = true;
    }

    // A whole read on this thread (readSync / readRangeSync / verifyAll).
    ReadResult readImpl(const Key& k, bool ranged, U64 roff, U64 rlen, bool count_access = true)
    {
        ReadCtx c;
        c.key = k;
        c.ranged = ranged;
        c.roff = roff;
        c.rlen = rlen;
        c.count_access = count_access;
        readBegin(c);
        if (c.done) return std::move(c.res);
        if (c.need_decode) readDecode(c);
        return readFinish(c);
    }

    // Stage 1: lookup, then the read (a queued copy: copied / decoded here; a disk copy:
    // readDiskIO). Sets c.done when the result is already final.
    void readBegin(ReadCtx& c)
    {
        const Key& k = c.key;
        const bool ranged = c.ranged;
        const U64 roff = c.roff, rlen = c.rlen;
        ReadResult& res = c.res;
        c.done = true;
        if (ro && overflow && overflow->lookup(k))
        {
            res = ranged ? overflow->readRangeSync(k, roff, rlen) : overflow->readSync(k);
            if (res.status == Status::OK) return;
        }
        if (ro && isHidden(k))
        {
            misses++;
            res.status = Status::MISSING;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(staging_mutex);
            auto it = staging.find(k);
            if (it != staging.end())
            {
                const std::vector<U8>& v = it->second;
                U64 a = ranged ? std::min<U64>(roff, v.size()) : 0;
                U64 n = ranged ? std::min<U64>(rlen, v.size() - a) : v.size();
                res.bytes.assign(v.begin() + a, v.begin() + a + n);
                res.info.key = k;
                res.info.staged = true;
                res.info.raw_bytes = res.info.stored_bytes = (U32)v.size();
                res.status = Status::OK;
                hits++;
                return;
            }
        }
        U32 stored = 0;
        U8 meta_len = 0, codec = 0;
        bool renamed = false;
        {
            Shard& sh = shardFor(k);
            std::lock_guard<std::mutex> lock(sh.mutex);
            auto it = sh.map.find(k);
            if (it == sh.map.end())
            {
                misses++;
                res.status = Status::MISSING;
                return;
            }
            const Entry& e = it->second;
            res.info = infoOf(sh, k, e);
            c.pend = sh.pending(k, e);
            c.seq = e.seg; c.off = e.offset; stored = e.stored; c.raw = e.raw;
            meta_len = e.meta_len; codec = e.codec();
            renamed = (e.flags & EF_RENAMED) != 0;
        }
        c.done = false;
        reads_in_flight++;
        res.bytes.clear();
        if (c.pend) c.st = readPending(k, *c.pend, ranged, roff, rlen, res.bytes);
        else if (c.seq) c.st = readDiskIO(c, stored, meta_len, codec, renamed);
        else c.st = Status::MISSING;
        if (c.st != Status::OK)
        {
            c.need_decode = false;
            c.buf.clear();
        }
    }

    // Stage 3: the result -> the index (touch / unpin / RAM-only release on OK; drop
    // exactly the copy that failed on CORRUPT / MISSING).
    ReadResult readFinish(ReadCtx& c)
    {
        const Key& k = c.key;
        const Status st = c.st;
        const PendingPtr& pend = c.pend;
        ReadResult& res = c.res;
        reads_in_flight--;
        res.status = st;
        if (st != Status::OK) res.bytes.clear();
        Shard& sh = shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(k);
        if (st == Status::OK)
        {
            hits++;
            if (it != sh.map.end() && c.count_access)
            {
                Entry& e = it->second;
                touchLocked(e, now());
                bool was_ram_only = (e.flags & EF_RAM_ONLY) && pend && sh.isPending(k, e, pend);
                e.flags &= ~EF_PINNED;
                if (was_ram_only)
                {
                    // Held in RAM only until this first read (#711 fallback).
                    ram_fallback -= std::min<U64>(ram_fallback.load(), pend->payload.size());
                    sh.setPending(k, e, nullptr);
                    if (!e.seg) sh.erase(it);
                }
            }
        }
        else
        {
            if (st == Status::CORRUPT) corrupt_dropped++;
            if (st == Status::CORRUPT || st == Status::MISSING)
            {
                // drop exactly the copy that failed, never a newer one
                if (it != sh.map.end() && !pend && it->second.seg == c.seq && it->second.offset == c.off)
                {
                    subLive(k, it->second);
                    if (!it->second.pending()) sh.erase(it);
                }
                LL_DEBUGS("CacheStore") << "dropped " << kindName(k.kind) << " " << k.id << ": " << statusName(st) << LL_ENDL;
            }
            misses++;
        }
        return std::move(res);
    }

    // A started read whose ticket was cancelled (or the store is closing) before its
    // decode: no decode, no index effect.
    ReadResult readAbandon(ReadCtx&)
    {
        reads_in_flight--;
        ReadResult r;
        r.status = Status::CANCELLED;
        return r;
    }

    bool isHidden(const Key& k) const
    {
        std::lock_guard<std::mutex> lock(hidden_mutex);
        return hidden_kinds[ki(k.kind)] || hidden.count(k);
    }

    // ------------------------------------------------------------ threads

    void noteReadDone(std::chrono::steady_clock::time_point t_req)
    {
        const U64 us = usecSince(t_req);
        read_done_count++;
        read_done_usec += us;
        U64 m = read_done_max.load();
        while (us > m && !read_done_max.compare_exchange_weak(m, us)) {}
    }

    // Async reads, stage 1 (I/O): highest priority first, FIFO within one. A large zstd
    // decode goes to the decode pool; everything else (codec 0, small or identity codec
    // 1, queued copies) completes here.
    void readerLoop()
    {
        ll_set_current_thread_name("CacheStoreRead");
        while (true)
        {
            ReadJob job;
            {
                std::unique_lock<std::mutex> lock(rmutex);
                rcv.wait(lock, [this] { return rstop || !rqueue.empty(); });
                if (rstop) return;   // close(): what is still queued completes CANCELLED
                job = rqueue.top();
                rqueue.pop();
            }
            read_jobs++;
            read_wait_usec += usecSince(job.t_req);
            reads_by_prio[std::min<size_t>((size_t)job.prio, PRIORITY_COUNT - 1)]++;
            if (job.state->cancelled.load())
            {
                ReadResult r;
                r.status = Status::CANCELLED;
                detail::complete(job.state, std::move(r));
                continue;
            }
            if (cfg.job_hook) cfg.job_hook(job.key, false);
            ReadCtx c;
            c.key = job.key;
            c.ranged = job.ranged;
            c.roff = job.offset;
            c.rlen = job.len;
            const auto t_io = std::chrono::steady_clock::now();
            readBegin(c);
            read_io_usec += usecSince(t_io);
            if (c.done)
            {
                noteReadDone(job.t_req);
                detail::complete(job.state, std::move(c.res));
                continue;
            }
            if (c.need_decode && c.zstd && c.raw >= HANDOFF_MIN_RAW && handOff(job, c)) continue;
            if (c.need_decode)
            {
                decode_inline++;
                readDecode(c);
            }
            ReadResult r = readFinish(c);
            noteReadDone(job.t_req);
            detail::complete(job.state, std::move(r));
        }
    }

    // Reader -> decode pool. False (the reader decodes inline) when the pool is gone or
    // already holds decode_queue_bytes of decoded output (bounded RAM, and a reader that
    // decodes is the backpressure).
    bool handOff(const ReadJob& job, ReadCtx& c)
    {
        {
            std::lock_guard<std::mutex> lock(dmutex);
            if (dstop || decoders.empty()) return false;
            if (dqueue_bytes && dqueue_bytes + c.raw > cfg.decode_queue_bytes) return false;
            DecodeJob d;
            d.prio = job.prio;
            d.seq = job.seq;
            d.state = job.state;
            d.t_req = job.t_req;
            d.t_handoff = std::chrono::steady_clock::now();
            d.ctx = std::move(c);
            dqueue_bytes += d.ctx.raw;
            dqueue.push_back(std::move(d));
            std::push_heap(dqueue.begin(), dqueue.end(), DecodeJobOrder());
        }
        decode_handoffs++;
        dcv.notify_one();
        return true;
    }

    // Async reads, stage 2 (decode pool, below-normal OS priority): decode + index
    // bookkeeping + completion, highest priority first.
    void decoderLoop()
    {
        ll_set_current_thread_name("CacheStoreDec");
        P::lowerCurrentThreadPriority();
        while (true)
        {
            DecodeJob j;
            {
                std::unique_lock<std::mutex> lock(dmutex);
                dcv.wait(lock, [this] { return dstop || !dqueue.empty(); });
                if (dstop) return;   // close(): what is still queued completes CANCELLED
                std::pop_heap(dqueue.begin(), dqueue.end(), DecodeJobOrder());
                j = std::move(dqueue.back());
                dqueue.pop_back();
                dqueue_bytes -= std::min<U64>(dqueue_bytes, j.ctx.raw);
            }
            decode_wait_usec += usecSince(j.t_handoff);
            if (j.state->cancelled.load())
            {
                detail::complete(j.state, readAbandon(j.ctx));
                continue;
            }
            if (cfg.job_hook) cfg.job_hook(j.ctx.key, true);
            readDecode(j.ctx);
            ReadResult r = readFinish(j.ctx);
            noteReadDone(j.t_req);
            detail::complete(j.state, std::move(r));
        }
    }

    void runTask(std::function<void()> fn, bool wait)
    {
        if (std::this_thread::get_id() == writer_id.load() || !writer.joinable())
        {
            fn();
            return;
        }
        auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
        auto flag = std::make_shared<bool>(false);
        {
            std::lock_guard<std::mutex> lock(wmutex);
            wtasks.push_back([fn, done, flag]
            {
                fn();
                std::lock_guard<std::mutex> l(done->first);
                *flag = true;
                done->second.notify_all();
            });
        }
        wcv.notify_one();
        if (wait)
        {
            std::unique_lock<std::mutex> l(done->first);
            done->second.wait(l, [&] { return *flag; });
        }
    }

    void writerLoop()
    {
        if (loading)
        {
            timedLoadIndex();
            {
                std::lock_guard<std::mutex> lock(load_mutex);
                loading = false;
                removed_during_load.clear();
            }
            is_ready = true;
        }
        while (true)
        {
            std::deque<PendingPtr> in;
            std::deque<std::function<void()>> tasks;
            bool stop;
            {
                std::unique_lock<std::mutex> lock(wmutex);
                auto wait = std::chrono::milliseconds(ro ? 1000 : (buckets.empty() ? 1000 : 50));
                wcv.wait_for(lock, wait, [this] { return stopping || !intake.empty() || !wtasks.empty(); });
                in.swap(intake);
                tasks.swap(wtasks);
                stop = stopping;
            }
            if (ro)
            {
                for (auto& t : tasks) t();
                if (stop) break;
                if (now() - last_refresh >= cfg.refresh_interval_s)
                {
                    refresh();
                    last_refresh = now();
                }
                continue;
            }
            for (PendingPtr& p : in) intakeOne(p);
            for (auto& t : tasks) t();
            flushDue(stop);
            maintenanceTick();
            if (stop)
            {
                std::lock_guard<std::mutex> lock(wmutex);
                if (intake.empty() && wtasks.empty()) break;
            }
        }
        if (!ro)
        {
            flushDue(true);
            writeSnapshot();
        }
    }

    void maintenanceTick()
    {
        bool over = disk_bytes.load() > effectiveLimit();
        for (size_t g = 0; g < (size_t)Group::UNBUDGETED && !over; ++g) over = groupOver(g);
        for (size_t k = 1; k < KIND_COUNT && !over; ++k)
        {
            KindPolicy pol = policy((Kind)k);
            if (pol.share_cap < 1.f && pol.group != Group::UNBUDGETED)
                over = kind_live[k].load() > (U64)((F64)budgets[(size_t)pol.group].load() * pol.share_cap);
        }
        if (over) enforceBudgets(0);
        if (index_dirty && cfg.snapshot_interval_s && now() - last_snapshot >= cfg.snapshot_interval_s)
        {
            writeSnapshot();
        }
    }

    void prefetchLoop()
    {
        while (true)
        {
            PrefetchJob job;
            {
                std::unique_lock<std::mutex> lock(pmutex);
                pcv.wait(lock, [this] { return pstop || !pqueue.empty(); });
                if (pstop) return;
                job = std::move(pqueue.front());
                pqueue.pop_front();
            }
            for (const std::vector<Extent>& range : job.ranges)
            {
                if (job.cancel->load() || pstop) break;
                // Demand reads always win.
                while (demand_in_flight.load() > 0 && !job.cancel->load() && !pstop)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (range.empty()) continue;
                SegmentPtr s = findSeg(range.front().seg);
                if (!s) continue;
                if (!buffer_mode)
                {
                    U64 a = range.front().off;
                    U64 b = (U64)range.back().off + range.back().len;
                    s->file.adviseWillNeed(a, b - a);
                    prefetch_bytes += b - a;
                    continue;
                }
                // Buffer mode: chunks of whole records, ~1 MiB each.
                size_t i = 0;
                while (i < range.size() && !job.cancel->load())
                {
                    U32 a = range[i].off;
                    U64 end = a;
                    size_t j = i;
                    while (j < range.size() && (end - a) < (1u << 20) && range[j].off == end)
                    {
                        end += range[j].len;
                        ++j;
                    }
                    if (j == i) { end = (U64)range[i].off + range[i].len; j = i + 1; }
                    std::vector<U8> chunk((size_t)(end - a));
                    if (s->file.pread(chunk.data(), chunk.size(), a) == (S64)chunk.size())
                    {
                        U64 got = chunk.size();
                        struct Count { std::atomic<U64>& c; U64 n; ~Count() { c += n; } } count{ prefetch_bytes, got };
                        std::lock_guard<std::mutex> lock(ra_mutex);   // released before count runs
                        auto key = std::make_pair(s->seq, a);
                        if (!ra_chunks.count(key))
                        {
                            ra_bytes += chunk.size();
                            ra_chunks[key] = std::move(chunk);
                            ra_fifo.push_back(key);
                            while (ra_bytes > cfg.prefetch_buffer_bytes && !ra_fifo.empty())
                            {
                                auto old = ra_chunks.find(ra_fifo.front());
                                ra_fifo.pop_front();
                                if (old != ra_chunks.end())
                                {
                                    ra_bytes -= old->second.size();
                                    ra_chunks.erase(old);
                                }
                            }
                        }
                    }
                    i = j;
                }
            }
            std::lock_guard<std::mutex> lock(pmutex);
            auto it = pactive.find(job.region);
            if (it != pactive.end() && it->second == job.cancel) pactive.erase(it);
        }
    }

    // ------------------------------------------------------------ manifests

    std::vector<Extent> regionExtents(U32 slot)
    {
        std::vector<Extent> v;
        for (Shard& sh : shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
            {
                const Entry& e = kv.second;
                if (e.seg && e.region() == slot) v.push_back({ e.seg, e.offset, (U32)recSize(e) });
            }
        }
        std::sort(v.begin(), v.end());
        return v;
    }

    static std::vector<Extent> mergeRuns(const std::vector<Extent>& ext)
    {
        std::vector<Extent> runs;
        for (const Extent& e : ext)
        {
            if (!runs.empty() && runs.back().seg == e.seg && (U64)runs.back().off + runs.back().len == e.off)
                runs.back().len += e.len;
            else
                runs.push_back(e);
        }
        return runs;
    }

    void loadManifest(U32 slot, Store& self)
    {
        LLUUID id;
        {
            std::lock_guard<std::mutex> lock(region_mutex);
            if (slot >= regions.size() || regions[slot].manifest_loaded) return;
            regions[slot].manifest_loaded = true;
            id = regions[slot].id;
        }
        ReadResult r = self.readSync(Key(id, Kind::MANIFEST));
        if (r.status != Status::OK) return;
        F::Reader rd(r.bytes.data(), r.bytes.size());
        if (rd.u32() != 1) return;
        LLUUID rid = rd.uuid();
        U64 handle = rd.u64();
        U32 last_visit = rd.u32(), visits = rd.u32();
        U32 nruns = rd.u32();
        for (U32 i = 0; i < nruns && rd.ok; ++i) { rd.u32(); rd.u32(); rd.u32(); }  // recomputed from the index
        auto readKeys = [&rd](std::vector<Key>& out)
        {
            U32 n = rd.u32();
            for (U32 i = 0; i < n && rd.ok && i < MAX_REGION_REFS; ++i)
            {
                Key k;
                k.id = rd.uuid();
                k.kind = (Kind)std::min<U8>(rd.u8(), (U8)Kind::COUNT - 1);
                k.variant = rd.u32();
                out.push_back(k);
            }
        };
        std::vector<Key> shared, order;
        readKeys(shared);
        readKeys(order);
        if (!rd.ok || rid != id) return;
        std::lock_guard<std::mutex> lock(region_mutex);
        RegionState& g = regions[slot];
        if (!g.handle) g.handle = handle;
        g.last_visit = std::max(g.last_visit, last_visit);
        g.visit_count = std::max(g.visit_count, visits);
        for (const Key& k : shared)
            if (g.shared_refs.size() < MAX_REGION_REFS && g.shared_set.insert(k).second) g.shared_refs.push_back(k);
        // manifest order first, then this session's additions
        std::vector<Key> merged;
        std::unordered_set<Key, KeyHash> seen;
        for (const Key& k : order) if (seen.insert(k).second) merged.push_back(k);
        for (const Key& k : g.order) if (seen.insert(k).second) merged.push_back(k);
        if (merged.size() > MAX_REGION_REFS) merged.resize(MAX_REGION_REFS);
        g.order = std::move(merged);
        g.order_set = std::move(seen);
    }
};

// ======================================================================
// Store

Store::Store() : mImpl(new Impl)
{
}

Store::~Store()
{
    close();
}

bool Store::open(const Config& config)
{
    if (mImpl->is_open) return false;
    // Re-arm (a Store may be reopened after close()).
    mImpl.reset(new Impl);
    Impl& m = *mImpl;
    m.cfg = config;
    m.cfg.segment_bytes = std::clamp<U64>(m.cfg.segment_bytes, 1ull << 20, 1ull << 30);
    m.dir = config.dir;
    while (m.dir.size() > 1 && (m.dir.back() == '/' || m.dir.back() == '\\')) m.dir.pop_back();
    if (m.dir.empty() || !P::makeDirs(m.dir))
    {
        LL_WARNS("CacheStore") << "cannot create store folder " << m.dir << LL_ENDL;
        return false;
    }

    // Default kind policies.
    auto& pol = m.policies;
    pol[ki(Kind::J2C)]      = KindPolicy{ false, 0, 1, true,  Group::TEXTURES,   1.f, 4.f };
    pol[ki(Kind::GPUTEX)]   = KindPolicy{ true,  0, 1, false, Group::TEXTURES,   0.3f, 2.f };
    pol[ki(Kind::ASSET)]    = KindPolicy{ false, 0, 1, false, Group::ASSETS,     1.f, 1.f };
    pol[ki(Kind::MESH)]     = KindPolicy{ false, 0, 1, false, Group::ASSETS,     1.f, 1.f };
    pol[ki(Kind::CLUSTER)]  = KindPolicy{ false, 0, 1, true,  Group::CLUSTER,    1.f, 3.f };
    pol[ki(Kind::SCRATCH)]  = KindPolicy{ false, 0, 1, false, Group::UNBUDGETED, 1.f, 1.f };
    pol[ki(Kind::MANIFEST)] = KindPolicy{ true,  0, 1, false, Group::ASSETS,     1.f, 1.f };
    pol[ki(Kind::NONE)]     = KindPolicy{ false, 0, 1, false, Group::UNBUDGETED, 1.f, 1.f };
    m.budgets[(size_t)Group::TEXTURES] = 10ull << 30;
    m.budgets[(size_t)Group::ASSETS] = 6ull << 30;
    m.budgets[(size_t)Group::CLUSTER] = 2ull << 30;
    m.budgets[(size_t)Group::UNBUDGETED] = 0;

    // Owner or reader.
    m.ro = config.force_read_only;
    if (!m.ro && !m.lock.tryLock(m.dir + "/lock"))
    {
        if (!m.lock.busy() || !config.read_only_if_locked)
        {
            LL_WARNS("CacheStore") << "store " << m.dir << " is " << (m.lock.busy() ? "in use" : "not lockable") << LL_ENDL;
            return false;
        }
        m.ro = true;
    }
    m.session_start = m.now();
    if (config.detect_drive)
    {
        m.drive = detectDriveType(m.dir);
        m.wear.ssd = m.drive != DriveType::HDD;
    }
#if LL_WINDOWS
    m.buffer_mode = config.prefetch_mode != Config::PrefetchMode::HINT;
#else
    m.buffer_mode = config.prefetch_mode == Config::PrefetchMode::BUFFER;
#endif

    if (m.ro)
    {
        if (config.overflow)
        {
            static std::atomic<U32> counter{ 0 };
            m.overflow_path = config.overflow_dir.empty()
                ? m.dir + "/overflow-" + std::to_string(P::processId()) + "-" + std::to_string(counter++)
                : config.overflow_dir;
            Config oc;
            oc.dir = m.overflow_path;
            oc.segment_bytes = std::min<U64>(m.cfg.segment_bytes, 64ull << 20);
            oc.read_threads = 1;
            oc.decode_threads = 1;
            oc.read_only_if_locked = false;
            oc.overflow = false;
            oc.detect_drive = false;
            oc.snapshot_interval_s = 0;
            oc.clock = config.clock;
            oc.free_space = config.free_space;
            oc.write_fault = config.write_fault;
            oc.run_flush_bytes = config.run_flush_bytes;
            oc.run_flush_ms = config.run_flush_ms;
            m.overflow.reset(new Store);
            if (!m.overflow->open(oc))
            {
                m.overflow.reset();
            }
            else
            {
                m.overflow->setBudget(Group::TEXTURES, config.overflow_bytes / 4);
                m.overflow->setBudget(Group::ASSETS, config.overflow_bytes / 2);
                m.overflow->setBudget(Group::CLUSTER, config.overflow_bytes / 4);
            }
        }
        LL_INFOS("CacheStore") << "store " << m.dir << " is in use by another instance: read-only"
                               << (m.overflow ? " + private overflow " + m.overflow_path : std::string()) << LL_ENDL;
    }
    else
    {
        // Overflow folders of dead second instances.
        for (const std::string& name : P::listDir(m.dir))
        {
            if (name.compare(0, 9, "overflow-") != 0) continue;
            std::string path = m.dir + "/" + name;
            P::LockFile probe;
            if (probe.tryLock(path + "/lock"))
            {
                probe.unlock();
                P::removeTree(path);
            }
        }
    }

    resolveThreadCounts(m.cfg, m.drive);
    m.is_open = true;
    for (U32 i = 0; i < m.cfg.decode_threads; ++i)
        m.decoders.emplace_back([&m] { m.decoderLoop(); });
    for (U32 i = 0; i < m.cfg.read_threads; ++i)
        m.readers.emplace_back([&m] { m.readerLoop(); });
    m.prefetcher = std::thread([&m] { m.prefetchLoop(); });

    if (config.async_open)
    {
        m.loading = true;
        m.writer = std::thread([&m] { m.writerLoop(); });
        m.writer_id = m.writer.get_id();
    }
    else
    {
        m.timedLoadIndex();
        m.is_ready = true;
        m.last_refresh = m.now();
        m.writer = std::thread([&m] { m.writerLoop(); });
        m.writer_id = m.writer.get_id();
    }
    LL_INFOS("CacheStore") << "opened " << m.dir << (m.ro ? " (read-only)" : "") << " drive="
                           << (m.drive == DriveType::HDD ? "hdd" : m.drive == DriveType::SSD ? "ssd" : "unknown")
                           << " readers=" << m.cfg.read_threads << " decoders=" << m.cfg.decode_threads
                           << (config.read_threads || config.decode_threads ? " (override)" : " (auto)") << LL_ENDL;
    return true;
}

void Store::close()
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    // Staged writes still open become records (scratch is dropped).
    std::vector<std::pair<Key, std::vector<U8>>> staged;
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        for (auto& kv : m.staging) staged.emplace_back(kv.first, std::move(kv.second));
        m.staging.clear();
    }
    for (auto& kv : staged)
    {
        if (kv.first.kind != Kind::SCRATCH) write(kv.first, WriteSource::take(std::move(kv.second)));
    }
    if (!m.ro && m.is_ready) saveRegionManifests();
    {
        std::lock_guard<std::mutex> lock(m.pmutex);
        m.pstop = true;
    }
    m.pcv.notify_all();
    if (m.prefetcher.joinable()) m.prefetcher.join();
    {
        std::lock_guard<std::mutex> lock(m.wmutex);
        m.stopping = true;
    }
    m.wcv.notify_all();
    if (m.writer.joinable()) m.writer.join();
    {
        std::lock_guard<std::mutex> lock(m.rmutex);
        m.rstop = true;
    }
    m.rcv.notify_all();
    for (std::thread& t : m.readers) if (t.joinable()) t.join();
    m.readers.clear();
    // Readers first (no new handoffs), then the decode pool. Each finishes the job in
    // hand and exits; queued work completes CANCELLED (nobody waits on a closing store).
    {
        std::lock_guard<std::mutex> lock(m.dmutex);
        m.dstop = true;
    }
    m.dcv.notify_all();
    for (std::thread& t : m.decoders) if (t.joinable()) t.join();
    m.decoders.clear();
    // (read() refuses new jobs once rstop is set; completions run outside the locks)
    std::priority_queue<ReadJob, std::vector<ReadJob>, ReadJobOrder> rleft;
    std::vector<DecodeJob> dleft;
    {
        std::lock_guard<std::mutex> lock(m.rmutex);
        rleft.swap(m.rqueue);
    }
    {
        std::lock_guard<std::mutex> lock(m.dmutex);
        dleft.swap(m.dqueue);
        m.dqueue_bytes = 0;
    }
    while (!rleft.empty())
    {
        ReadResult r;
        r.status = Status::CANCELLED;
        detail::complete(rleft.top().state, std::move(r));
        rleft.pop();
    }
    for (DecodeJob& j : dleft) detail::complete(j.state, m.readAbandon(j.ctx));
    if (m.overflow)
    {
        // Phase 1 policy: the second instance's private records are discarded.
        m.overflow->close();
        m.overflow.reset();
        P::removeTree(m.overflow_path);
    }
    {
        std::unique_lock<std::shared_mutex> lock(m.seg_mutex);
        m.segments.clear();
    }
    m.active.reset();
    m.lock.unlock();
    m.is_open = false;
    m.is_ready = false;
    LL_INFOS("CacheStore") << "closed " << m.dir << LL_ENDL;
}

bool Store::isOpen() const { return mImpl->is_open; }
bool Store::ready() const { return mImpl->is_ready.load(); }
bool Store::readOnly() const { return mImpl->ro; }
const std::string& Store::dir() const { return mImpl->dir; }

bool Store::waitReady(std::chrono::milliseconds timeout) const
{
    auto end = std::chrono::steady_clock::now() + timeout;
    while (!mImpl->is_ready.load())
    {
        if (!mImpl->is_open || std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void Store::setKindPolicy(Kind kind, const KindPolicy& policy)
{
    Impl& m = *mImpl;
    if (kind == Kind::NONE || kind >= Kind::COUNT) return;
    KindPolicy p = policy;
    if (p.ranged_reads) p.compress = false;
    p.format_version = std::min<U32>(p.format_version, 0xffffu);    // the index keeps 16 bits (#866)
    p.share_cap = std::clamp(p.share_cap, 0.f, 1.f);
    U32 old_version;
    {
        std::lock_guard<std::mutex> lock(m.policy_mutex);
        old_version = m.policies[ki(kind)].format_version;
        // A group change would orphan accounting; groups are fixed per kind
        // once records exist, so only allow it on an empty kind.
        if (m.kind_records[ki(kind)].load() && p.group != m.policies[ki(kind)].group)
            p.group = m.policies[ki(kind)].group;
        m.policies[ki(kind)] = p;
    }
    if (m.overflow) m.overflow->setKindPolicy(kind, policy);
    if (old_version != p.format_version && m.is_open)
    {
        // Every record of the old version is now invalid: drop them (dead
        // bytes until reclaimed; a rebuild ignores them by version).
        for (Shard& sh : m.shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto it = sh.map.begin(); it != sh.map.end();)
            {
                if (it->first.kind == kind && !it->second.pending())
                {
                    m.subLive(it->first, it->second);
                    it = sh.erase(it);
                }
                else ++it;
            }
        }
    }
}

KindPolicy Store::kindPolicy(Kind kind) const
{
    return mImpl->policy(std::min(kind, (Kind)((U8)Kind::COUNT - 1)));
}

bool Store::lookup(const Key& key, RecordInfo* out) const
{
    const Impl& m = *mImpl;
    if (!m.is_open) return false;
    if (m.ro && m.overflow && m.overflow->lookup(key, out)) return true;
    if (m.ro && m.isHidden(key)) return false;
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        auto it = m.staging.find(key);
        if (it != m.staging.end())
        {
            if (out)
            {
                *out = RecordInfo();
                out->key = key;
                out->staged = true;
                out->raw_bytes = out->stored_bytes = (U32)it->second.size();
            }
            return true;
        }
    }
    const Shard& sh = m.shardFor(key);
    std::lock_guard<std::mutex> lock(sh.mutex);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;
    if (out) *out = m.infoOf(sh, key, it->second);
    return true;
}

void Store::touch(const Key& key)
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    if (m.ro && m.overflow) m.overflow->touch(key);
    Shard& sh = m.shardFor(key);
    std::lock_guard<std::mutex> lock(sh.mutex);
    auto it = sh.map.find(key);
    if (it != sh.map.end()) m.touchLocked(it->second, m.now());
}

void Store::noteReuse(const Key& key)
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    Shard& sh = m.shardFor(key);
    std::lock_guard<std::mutex> lock(sh.mutex);
    auto it = sh.map.find(key);
    if (it != sh.map.end() && it->second.reuse < 0xffff)
    {
        it->second.reuse++;
        m.index_dirty = true;
    }
}

void Store::noteUse(const Key& key, const PlacementHint& where)
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    U32 slot = m.internRegion(where.region, where.grid_handle);
    U32 t = m.now();
    bool shared_ref = false;
    {
        Shard& sh = m.shardFor(key);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(key);
        if (it == sh.map.end()) return;
        Entry& e = it->second;
        m.touchLocked(e, t);
        e.setRecency(t);
        PendingPtr pp = sh.pending(key, e);
        U32 placed = pp ? pp->region_slot : e.region();
        if (placed != slot)
        {
            // Promotion without copying: it stays where it is, no longer tied
            // to its first region's lifetime.
            e.flags |= EF_SHARED;
            shared_ref = true;
        }
    }
    if (slot != NO_REGION) m.addRegionUse(slot, key, shared_ref);
}

Ticket<ReadResult> Store::read(const Key& key, Priority prio, Completion done)
{
    return readRange(key, 0, ~0ull, prio, std::move(done));
}

Ticket<ReadResult> Store::readRange(const Key& key, U64 offset, U64 len, Priority prio, Completion done)
{
    Impl& m = *mImpl;
    auto st = std::make_shared<detail::TicketState<ReadResult>>();
    st->completion = std::move(done);
    Ticket<ReadResult> t(st);
    if (!m.is_open)
    {
        ReadResult r;
        r.status = Status::MISSING;
        detail::complete(st, std::move(r));
        return t;
    }
    ReadJob job;
    job.key = key;
    job.ranged = !(offset == 0 && len == ~0ull);
    job.offset = offset;
    job.len = len;
    job.prio = prio;
    job.state = st;
    job.t_req = std::chrono::steady_clock::now();
    bool closing;
    {
        std::lock_guard<std::mutex> lock(m.rmutex);
        closing = m.rstop;
        if (!closing)
        {
            job.seq = m.rseq++;
            m.rqueue.push(std::move(job));
        }
    }
    if (closing)
    {
        ReadResult r;
        r.status = Status::CANCELLED;
        detail::complete(st, std::move(r));
        return t;
    }
    m.rcv.notify_one();
    return t;
}

ReadResult Store::readSync(const Key& key)
{
    Impl& m = *mImpl;
    if (!m.is_open) return ReadResult();
    return m.readImpl(key, false, 0, 0);
}

ReadResult Store::readRangeSync(const Key& key, U64 offset, U64 len)
{
    Impl& m = *mImpl;
    if (!m.is_open) return ReadResult();
    return m.readImpl(key, true, offset, len);
}

bool Store::write(const Key& key, WriteSource src, const WriteOptions& opts, Ticket<Status>* done)
{
    Impl& m = *mImpl;
    std::shared_ptr<detail::TicketState<Status>> st;
    if (done)
    {
        st = std::make_shared<detail::TicketState<Status>>();
        st->completion = opts.completion;
        *done = Ticket<Status>(st);
    }
    auto refuse = [&](Status s) -> bool
    {
        if (src.release) src.release();
        if (s != Status::SKIPPED) m.writes_refused++;
        detail::complete(st, s);
        return s == Status::SKIPPED;
    };
    if (!m.is_open || key.kind == Kind::NONE || key.kind >= Kind::COUNT || opts.meta_len > 32 ||
        (opts.meta_len && !opts.meta))
    {
        return refuse(Status::IO_ERROR);
    }
    size_t n = src.size();
    if ((U64)n > std::min<U64>(LLCacheCodec::getMaxRecordSize(), 0xffff0000ull))
    {
        return refuse(Status::IO_ERROR);
    }
    if (m.ro)
    {
        if (!m.overflow) return refuse(Status::READ_ONLY);
        {
            std::lock_guard<std::mutex> lock(m.hidden_mutex);
            m.hidden.erase(key);
        }
        if (done)
        {
            // Mirror the overflow's result into our ticket. write() sets
            // *done before it queues anything, so `inner` is valid by the
            // time the inline notify runs.
            auto inner = std::make_shared<Ticket<Status>>();
            WriteOptions o = opts;
            o.completion = Completion();
            o.completion.notify = [inner, st] { detail::complete(st, inner->take()); };
            return m.overflow->write(key, std::move(src), o, inner.get());
        }
        return m.overflow->write(key, std::move(src), opts, nullptr);
    }
    bool delivery = isDeliveryKind(key.kind);
    if (!delivery && m.now() < m.disk_full_until.load())
    {
        return refuse(Status::NO_SPACE);
    }
    if (!delivery && m.queued_bytes.load() + n > m.cfg.max_queued_bytes)
    {
        return refuse(Status::REFUSED_BUSY);
    }
    KindPolicy pol = m.policy(key.kind);
    if (opts.flags & WF_SKIP_IF_NOT_LARGER)
    {
        Shard& sh = m.shardFor(key);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(key);
        if (it != sh.map.end())
        {
            const Entry& e = it->second;
            PendingPtr pp = sh.pending(key, e);
            U32 have = pp ? pp->raw_size : e.raw;
            if (have >= n)
            {
                m.skipped++;
                return refuse(Status::SKIPPED);
            }
        }
    }
    // A full write replaces an open staged write of the same key.
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        m.staging.erase(key);
    }

    auto p = std::make_shared<Pending>();
    p->key = key;
    if (src.borrowed)
    {
        p->borrowed = src.borrowed;
        p->borrowed_n = src.borrowed_n;
    }
    else
    {
        p->raw = std::move(src.owned);
    }
    p->release = std::move(src.release);
    p->raw_size = (U32)n;
    p->kind_version = pol.format_version;
    p->meta_len = opts.meta_len;
    if (opts.meta_len) memcpy(p->meta, opts.meta, opts.meta_len);
    p->write_time = m.now();
    p->region = opts.placement.region;
    p->grid_handle = opts.placement.grid_handle;
    p->region_slot = m.internRegion(opts.placement.region, opts.placement.grid_handle);
    p->wflags = opts.flags;
    p->prio = opts.priority;
    p->done = st;

    {
        Shard& sh = m.shardFor(key);
        std::lock_guard<std::mutex> lock(sh.mutex);
        Entry* ep = sh.map.insert(key);
        if (!ep)
        {
            // the shard is full (Index::MAX_RECORDS)
            p->releaseBorrowed();
            m.writes_refused++;
            detail::complete(st, Status::NO_SPACE);
            return false;
        }
        Entry& e = *ep;
        if (e.pending()) m.superseded++;
        sh.setPending(key, e, p);
        e.last_access = p->write_time;     // a write is an access (#705 reuse across sessions)
        e.flags &= ~EF_RAM_ONLY;
        if (opts.flags & WF_PIN_SESSION) e.flags |= EF_PINNED;
    }
    if (p->region_slot != NO_REGION) m.addRegionUse(p->region_slot, key, false);
    m.queued_bytes += n;
    m.queued_count++;
    m.index_dirty = true;
    {
        std::lock_guard<std::mutex> lock(m.wmutex);
        m.intake.push_back(p);
    }
    m.wcv.notify_one();
    return true;
}

bool Store::stagedBegin(const Key& key)
{
    Impl& m = *mImpl;
    if (!m.is_open || key.kind == Kind::NONE || key.kind >= Kind::COUNT) return false;
    std::lock_guard<std::mutex> lock(m.staging_mutex);
    m.staging[key].clear();
    return true;
}

bool Store::stagedAppend(const Key& key, const void* p, size_t n)
{
    Impl& m = *mImpl;
    if (!m.is_open || key.kind == Kind::NONE || key.kind >= Kind::COUNT) return false;
    std::lock_guard<std::mutex> lock(m.staging_mutex);
    std::vector<U8>& v = m.staging[key];
    if ((U64)v.size() + n > LLCacheCodec::getMaxRecordSize()) return false;
    v.insert(v.end(), (const U8*)p, (const U8*)p + n);
    return true;
}

bool Store::isStaged(const Key& key) const
{
    std::lock_guard<std::mutex> lock(mImpl->staging_mutex);
    return mImpl->staging.count(key) != 0;
}

bool Store::stagedCommit(const Key& key, const Key& final_key, const WriteOptions& opts, Ticket<Status>* done)
{
    Impl& m = *mImpl;
    std::vector<U8> bytes;
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        auto it = m.staging.find(key);
        if (it == m.staging.end()) return false;
        bytes = std::move(it->second);
        m.staging.erase(it);
        m.staging.erase(final_key);
    }
    return write(final_key, WriteSource::take(std::move(bytes)), opts, done);
}

void Store::stagedAbort(const Key& key)
{
    std::lock_guard<std::mutex> lock(mImpl->staging_mutex);
    mImpl->staging.erase(key);
}

bool Store::remove(const Key& key)
{
    Impl& m = *mImpl;
    if (!m.is_open) return false;
    stagedAbort(key);
    if (m.ro)
    {
        bool had = m.overflow && m.overflow->remove(key);
        bool in_main;
        {
            const Shard& sh = m.shardFor(key);
            std::lock_guard<std::mutex> lock(sh.mutex);
            in_main = sh.map.count(key) != 0;
        }
        std::lock_guard<std::mutex> lock(m.hidden_mutex);
        m.hidden.insert(key);
        return had || in_main;
    }
    bool had = false, on_disk = false;
    {
        Shard& sh = m.shardFor(key);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(key);
        if (it != sh.map.end())
        {
            had = true;
            on_disk = it->second.seg != 0 || it->second.pending();
            if (it->second.flags & EF_RAM_ONLY)
            {
                if (PendingPtr pp = sh.pending(key, it->second))
                    m.ram_fallback -= std::min<U64>(m.ram_fallback.load(), pp->payload.size());
            }
            m.subLive(key, it->second);
            sh.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m.load_mutex);
        if (m.loading)
        {
            m.removed_during_load.insert(key);
            on_disk = true;
        }
    }
    if (on_disk)
    {
        // Tombstone so a rebuild without a snapshot does not resurrect it.
        auto p = std::make_shared<Pending>();
        p->key = key;
        p->control = true;
        p->control_flags = F::FLAG_TOMBSTONE;
        p->write_time = m.now();
        p->encoded = true;
        {
            std::lock_guard<std::mutex> lock(m.wmutex);
            m.intake.push_back(p);
        }
        m.queued_count++;
        m.wcv.notify_one();
    }
    return had;
}

bool Store::rename(const Key& from, const Key& to)
{
    Impl& m = *mImpl;
    if (!m.is_open || from == to || m.ro) return m.ro && m.overflow ? m.overflow->rename(from, to) : false;
    Entry moved;
    PendingPtr moved_pending;
    U8 moved_meta[32];
    U8 moved_meta_len = 0;
    {
        Shard& sh = m.shardFor(from);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(from);
        if (it == sh.map.end()) return false;
        if (from.kind != to.kind && it->second.seg)
        {
            return false;   // accounting is per kind: re-key within a kind only
        }
        moved = it->second;
        moved_pending = sh.pending(from, it->second);
        if (const U8* meta = sh.map.metaAt(it.pos()))
        {
            moved_meta_len = it->second.meta_len;
            memcpy(moved_meta, meta, moved_meta_len);
        }
        m.subLive(from, it->second);
        sh.erase(it);
    }
    remove(to);
    if (moved_pending)
    {
        std::lock_guard<std::mutex> lock(moved_pending->mutex);
        moved_pending->key = to;
    }
    if (moved.seg) moved.flags |= EF_RENAMED;
    {
        Shard& sh = m.shardFor(to);
        std::lock_guard<std::mutex> lock(sh.mutex);
        Entry* ep = sh.map.insert(to);
        if (ep)
        {
            Entry& e = *ep;
            e.assignFrom(moved);
            sh.map.setMeta(to, moved_meta, moved_meta_len);
            sh.setPending(to, e, moved_pending);
            m.addLive(to, e);
        }
    }
    // Tombstone the old key (the snapshot carries the new one).
    auto p = std::make_shared<Pending>();
    p->key = from;
    p->control = true;
    p->control_flags = F::FLAG_TOMBSTONE;
    p->write_time = m.now();
    p->encoded = true;
    {
        std::lock_guard<std::mutex> lock(m.wmutex);
        m.intake.push_back(p);
    }
    m.queued_count++;
    m.wcv.notify_one();
    m.index_dirty = true;
    return true;
}

void Store::removeAll(Kind kind)
{
    Impl& m = *mImpl;
    if (!m.is_open || kind == Kind::NONE || kind >= Kind::COUNT) return;
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        for (auto it = m.staging.begin(); it != m.staging.end();)
            it = it->first.kind == kind ? m.staging.erase(it) : std::next(it);
    }
    if (m.ro)
    {
        if (m.overflow) m.overflow->removeAll(kind);
        std::lock_guard<std::mutex> lock(m.hidden_mutex);
        m.hidden_kinds[ki(kind)] = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m.load_mutex);
        if (m.loading) m.cleared_during_load[ki(kind)] = true;
    }
    m.runTask([&m, kind]
    {
        for (Shard& sh : m.shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto it = sh.map.begin(); it != sh.map.end();)
            {
                if (it->first.kind == kind)
                {
                    m.subLive(it->first, it->second);
                    it = sh.erase(it);
                }
                else ++it;
            }
        }
        // Marker first (in a new segment), so a crash mid-way never brings
        // the cleared records back; then free the space now, uncapped.
        m.sealActive();
        auto p = std::make_shared<Pending>();
        p->key = Key(LLUUID::null, Kind::NONE, (U32)kind);
        p->control = true;
        p->control_flags = F::FLAG_CLEAR_KIND;
        p->write_time = m.now();
        p->encoded = true;
        std::vector<PendingPtr> one{ p };
        m.queued_count++;
        m.writeRun(one, Impl::WriteClass::META);
        SegmentPtr marker_seg = m.active;
        for (SegmentPtr& s : m.segmentList())
        {
            if (s == marker_seg) continue;
            if (s->live_records.load() == 0) m.deleteSegment(s);
            else if (s->kind_bytes[ki(kind)].load()) m.reclaimSegment(s, true, true);
        }
        m.writeSnapshot();
    }, true);
}

void Store::clearAll()
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    {
        std::lock_guard<std::mutex> lock(m.staging_mutex);
        m.staging.clear();
    }
    if (m.ro)
    {
        if (m.overflow) m.overflow->clearAll();
        std::lock_guard<std::mutex> lock(m.hidden_mutex);
        for (auto& h : m.hidden_kinds) h = true;
        return;
    }
    m.runTask([&m]
    {
        for (auto& kv : m.buckets)
            for (PendingPtr& p : kv.second.recs) m.finishPending(p, Status::CANCELLED);
        m.buckets.clear();
        for (Shard& sh : m.shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map) m.subLive(kv.first, kv.second);
            sh.clear();
        }
        m.ram_fallback = 0;
        for (SegmentPtr& s : m.segmentList()) m.deleteSegment(s);
        P::removeFile(m.dir + "/" + SNAP_NAME);
        P::removeFile(m.dir + "/" + SNAP_PREV);
        {
            std::lock_guard<std::mutex> lock(m.region_mutex);
            m.regions.clear();
            m.region_slots.clear();
        }
        m.index_dirty = false;
    }, true);
}

void Store::flush()
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    if (m.ro)
    {
        if (m.overflow) m.overflow->flush();
        return;
    }
    // Make sure the intake is drained first: a task runs after the intake
    // batch that was queued before it.
    m.runTask([&m]
    {
        std::deque<PendingPtr> in;
        {
            std::lock_guard<std::mutex> lock(m.wmutex);
            in.swap(m.intake);
        }
        for (PendingPtr& p : in) m.intakeOne(p);
        m.flushDue(true);
    }, true);
}

bool Store::saveSnapshot()
{
    Impl& m = *mImpl;
    if (!m.is_open || m.ro) return false;
    bool ok = false;
    m.runTask([&m, &ok] { ok = m.writeSnapshot(); }, true);
    return ok;
}

void Store::regionVisit(const LLUUID& region, U64 grid_handle)
{
    Impl& m = *mImpl;
    if (!m.is_open || region.isNull()) return;
    U32 slot = m.internRegion(region, grid_handle);
    m.loadManifest(slot, *this);
    std::lock_guard<std::mutex> lock(m.region_mutex);
    RegionState& r = m.regions[slot];
    r.last_visit = m.now();
    r.visit_count++;
    r.dirty = true;
    m.index_dirty = true;
}

bool Store::regionInfo(const LLUUID& region, RegionInfo* out) const
{
    const Impl& m = *mImpl;
    S32 slot = m.regionSlot(region);
    if (slot < 0) return false;
    if (!out) return true;
    *out = RegionInfo();
    out->region = region;
    for (const Shard& sh : m.shards)
    {
        std::lock_guard<std::mutex> lock(sh.mutex);
        for (auto kv : sh.map)
        {
            if (kv.second.seg && kv.second.region() == (U32)slot)
            {
                out->own_records++;
                out->own_bytes += m.recSize(kv.second);
            }
        }
    }
    std::lock_guard<std::mutex> lock(m.region_mutex);
    const RegionState& r = m.regions[slot];
    out->grid_handle = r.handle;
    out->last_visit = r.last_visit;
    out->visit_count = r.visit_count;
    out->shared_refs = (U32)r.shared_refs.size();
    return true;
}

void Store::saveRegionManifests()
{
    Impl& m = *mImpl;
    if (!m.is_open || m.ro) return;
    std::vector<U32> dirty;
    {
        std::lock_guard<std::mutex> lock(m.region_mutex);
        for (U32 i = 0; i < m.regions.size(); ++i)
            if (m.regions[i].dirty) dirty.push_back(i);
    }
    U32 t = m.now();
    for (U32 slot : dirty)
    {
        m.loadManifest(slot, *this);
        std::vector<Extent> runs = Impl::mergeRuns(m.regionExtents(slot));
        F::Writer w;
        LLUUID id;
        {
            std::lock_guard<std::mutex> lock(m.region_mutex);
            RegionState& r = m.regions[slot];
            r.dirty = false;
            id = r.id;
            if (r.last_visit && t > r.last_visit && t - r.last_visit > REGION_MAX_AGE_S)
            {
                w.buf.clear();
            }
            else
            {
            w.u32(1);
            w.uuid(r.id);
            w.u64(r.handle);
            w.u32(r.last_visit);
            w.u32(r.visit_count);
            w.u32((U32)runs.size());
            for (const Extent& e : runs) { w.u32(e.seg); w.u32(e.off); w.u32(e.len); }
            for (const std::vector<Key>* keys : { &r.shared_refs, &r.order })
            {
                w.u32((U32)keys->size());
                for (const Key& k : *keys) { w.uuid(k.id); w.u8((U8)k.kind); w.u32(k.variant); }
            }
            }
        }
        if (w.buf.empty())
        {
            remove(Key(id, Kind::MANIFEST));   // the region aged out (#705)
            continue;
        }
        write(Key(id, Kind::MANIFEST), WriteSource::take(std::move(w.buf)));
    }
}

bool Store::prefetchRegion(const LLUUID& region, Priority prio)
{
    Impl& m = *mImpl;
    if (!m.is_open || !m.is_ready) return false;
    S32 slot = m.regionSlot(region);
    if (slot < 0) return false;
    m.loadManifest((U32)slot, *this);
    std::vector<Extent> own = m.regionExtents((U32)slot);
    std::vector<Key> refs;
    {
        std::lock_guard<std::mutex> lock(m.region_mutex);
        refs = m.regions[slot].shared_refs;
    }
    std::vector<Extent> shared;
    for (const Key& k : refs)
    {
        const Shard& sh = m.shardFor(k);
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto it = sh.map.find(k);
        if (it != sh.map.end() && it->second.seg && it->second.region() != (U32)slot)
            shared.push_back({ it->second.seg, it->second.offset, (U32)m.recSize(it->second) });
    }
    std::sort(shared.begin(), shared.end());
    PrefetchJob job;
    job.region = region;
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    U64 budget = m.cfg.prefetch_max_bytes;
    // own runs first, then shared references; ranges = contiguous record lists
    for (const std::vector<Extent>* list : { &own, &shared })
    {
        std::vector<Extent> cur;
        for (const Extent& e : *list)
        {
            if (budget < e.len) break;
            budget -= e.len;
            if (!cur.empty() && (cur.back().seg != e.seg || (U64)cur.back().off + cur.back().len != e.off))
            {
                job.ranges.push_back(std::move(cur));
                cur.clear();
            }
            cur.push_back(e);
        }
        if (!cur.empty()) job.ranges.push_back(std::move(cur));
    }
    if (job.ranges.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(m.pmutex);
        auto it = m.pactive.find(region);
        if (it != m.pactive.end()) it->second->store(true);
        m.pactive[region] = job.cancel;
        if (prio >= Priority::HIGH) m.pqueue.push_front(std::move(job));
        else m.pqueue.push_back(std::move(job));
    }
    m.pcv.notify_one();
    return true;
}

void Store::cancelPrefetch(const LLUUID& region)
{
    Impl& m = *mImpl;
    std::lock_guard<std::mutex> lock(m.pmutex);
    auto it = m.pactive.find(region);
    if (it != m.pactive.end())
    {
        it->second->store(true);
        m.pactive.erase(it);
    }
}

bool Store::gatherRegion(const LLUUID& region)
{
    Impl& m = *mImpl;
    WearPolicy wp = m.wearCopy();
    if (!m.is_open || m.ro || wp.ssd || !wp.locality_rewrites) return false;
    S32 slot = m.regionSlot(region);
    if (slot < 0) return false;
    bool done = false;
    m.runTask([&m, slot, &done]
    {
        std::map<U32, std::vector<Impl::LiveRec>> by_seg;
        U64 total = 0;
        for (Shard& sh : m.shards)
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
            {
                const Entry& e = kv.second;
                if (e.seg && e.region() == (U32)slot && !e.pending())
                {
                    by_seg[e.seg].push_back({ kv.first, e.offset, m.recSize(e), e.reuse, e.flags, e.region(), false });
                    total += m.recSize(e);
                }
            }
        }
        if (by_seg.size() < m.wearCopy().locality_min_segments || total > m.compactionAllowance()) return;
        m.sealActive();     // the gathered set gets a fresh run
        for (auto& kv : by_seg)
        {
            SegmentPtr s = m.findSeg(kv.first);
            if (s) m.rescued_records += m.reappend(s, kv.second);
        }
        done = true;
    }, true);
    return done;
}

void Store::setBudget(Group group, U64 bytes)
{
    if (group >= Group::COUNT) return;
    mImpl->budgets[(size_t)group] = bytes;
}

U64 Store::budget(Group group) const
{
    return group < Group::COUNT ? mImpl->budgets[(size_t)group].load() : 0;
}

void Store::setEvictionPolicy(Group group, std::shared_ptr<EvictionPolicy> policy)
{
    Impl& m = *mImpl;
    if (group >= Group::COUNT) return;
    m.runTask([&m, group, policy] { m.evict_policy[(size_t)group] = policy; }, true);
}

void Store::setWearPolicy(const WearPolicy& policy)
{
    Impl& m = *mImpl;
    WearPolicy p = policy;
    if (p.ssd) p.locality_rewrites = false;    // never on SSD (#709 / #712)
    p.reclaim_live_max = std::clamp(p.reclaim_live_max, 0.f, 0.9f);
    std::lock_guard<std::mutex> lock(m.policy_mutex);
    m.wear = p;
}

WearPolicy Store::wearPolicy() const
{
    return mImpl->wearCopy();
}

void Store::setDiskReserve(U64 bytes)
{
    mImpl->disk_reserve = bytes;
    if (mImpl->overflow) mImpl->overflow->setDiskReserve(bytes);
}

void Store::maintain()
{
    Impl& m = *mImpl;
    if (!m.is_open) return;
    if (m.ro)
    {
        if (m.overflow) m.overflow->maintain();
        m.runTask([&m] { m.refresh(); }, true);
        return;
    }
    m.runTask([&m]
    {
        m.deleteDeadSegments();
        m.enforceBudgets(0);
    }, true);
}

Stats Store::stats() const
{
    const Impl& m = *mImpl;
    Stats s;
    s.ready = m.is_ready.load();
    s.read_only = m.ro;
    s.disk_bytes = m.disk_bytes.load();
    s.peak_disk_bytes = m.peak_disk.load();
    for (size_t g = 0; g < GROUP_COUNT; ++g)
    {
        s.groups[g].budget_bytes = m.budgets[g].load();
        s.groups[g].disk_bytes = m.group_disk[g].load();
        s.groups[g].live_bytes = m.group_live[g].load();
        s.groups[g].records = m.group_records[g].load();
        s.live_bytes += s.groups[g].live_bytes;
        s.records += s.groups[g].records;
    }
    for (size_t k = 0; k < KIND_COUNT; ++k)
    {
        s.kinds[k].live_bytes = m.kind_live[k].load();
        s.kinds[k].records = m.kind_records[k].load();
        s.kinds[k].raw_bytes = m.kind_raw[k].load();
        s.kinds[k].evicted_records = m.kind_evicted[k].load();
        s.kinds[k].evicted_bytes = m.kind_evicted_bytes[k].load();
    }
    s.dead_bytes = s.disk_bytes > s.live_bytes ? s.disk_bytes - s.live_bytes : 0;
    s.live_fraction = s.disk_bytes ? (F32)((F64)s.live_bytes / (F64)s.disk_bytes) : 1.f;
    s.effective_limit = const_cast<Impl&>(m).effectiveLimit();
    {
        std::shared_lock<std::shared_mutex> lock(m.seg_mutex);
        s.segments = (U32)m.segments.size();
        // Per-kind disk bytes: every appended record is attributed to its kind in its
        // segment (live + dead, until the segment is deleted), so this is exact.
        for (const auto& kv : m.segments)
        {
            for (size_t k = 0; k < KIND_COUNT; ++k)
            {
                const U64 b = kv.second->kind_bytes[k].load();
                if (!b) continue;
                s.kinds[k].disk_bytes += b;
                s.kinds[k].segments++;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m.day_mutex);
        s.day = m.day;
        s.bytes_written_today_new = m.day_new;
        s.bytes_written_today_compaction = m.day_comp;
        s.bytes_written_today_meta = m.day_meta;
    }
    s.bytes_written_total = m.bytes_written_total.load();
    s.bytes_read = m.bytes_read.load();
    s.hits = m.hits.load();
    s.misses = m.misses.load();
    s.corrupt_dropped = m.corrupt_dropped.load();
    s.write_fail = m.write_fail.load();
    s.writes_refused = m.writes_refused.load();
    s.evicted_records = m.evicted_records.load();
    s.evicted_bytes = m.evicted_bytes.load();
    s.segments_reclaimed = m.segments_reclaimed.load();
    s.rescued_records = m.rescued_records.load();
    s.superseded = m.superseded.load();
    s.skipped = m.skipped.load();
    s.ram_fallback_bytes = m.ram_fallback.load();
    s.reads_in_flight = m.reads_in_flight.load();
    s.writes_queued = m.queued_count.load();
    s.write_bytes_queued = m.queued_bytes.load();
    s.prefetch_bytes = m.prefetch_bytes.load();
    s.prefetch_hits = m.prefetch_hits.load();
    s.drive = m.drive;
    if (m.overflow) s.overflow_records = m.overflow->stats().records;
    s.encode_count = m.encode_count.load();
    s.encode_usec = m.encode_usec.load();
    s.encode_in_bytes = m.encode_in.load();
    s.encode_out_bytes = m.encode_out.load();
    s.decode_count = m.decode_count.load();
    s.decode_usec = m.decode_usec.load();
    s.decode_in_bytes = m.decode_in.load();
    s.decode_out_bytes = m.decode_out.load();
    s.append_count = m.append_count.load();
    s.append_usec = m.append_usec.load();
    s.append_bytes = m.append_bytes.load();
    s.index_load_usec = m.index_load_usec.load();
    s.index_load_time = m.index_load_time.load();
    s.bytes_written_session_new = m.session_new.load();
    s.bytes_written_session_compaction = m.session_comp.load();
    s.bytes_written_session_meta = m.session_meta.load();
    s.read_threads = m.is_open ? m.cfg.read_threads : 0;
    s.decode_threads = m.is_open ? m.cfg.decode_threads : 0;
    s.read_jobs = m.read_jobs.load();
    s.read_wait_usec = m.read_wait_usec.load();
    s.read_io_usec = m.read_io_usec.load();
    s.decode_handoffs = m.decode_handoffs.load();
    s.decode_wait_usec = m.decode_wait_usec.load();
    s.decode_inline = m.decode_inline.load();
    s.read_done_count = m.read_done_count.load();
    s.read_done_usec = m.read_done_usec.load();
    s.read_done_max_usec = m.read_done_max.load();
    for (size_t p = 0; p < PRIORITY_COUNT; ++p) s.reads_by_priority[p] = m.reads_by_prio[p].load();
    {
        std::lock_guard<std::mutex> lock(m.rmutex);
        s.read_queue_depth = (U32)m.rqueue.size();
    }
    {
        std::lock_guard<std::mutex> lock(m.dmutex);
        s.decode_queue_depth = (U32)m.dqueue.size();
        s.decode_queue_bytes = m.dqueue_bytes;
    }
    for (const Shard& sh : m.shards)
    {
        std::lock_guard<std::mutex> lock(sh.mutex);
        s.index_bytes += sh.memoryBytes();
        s.index_records += (U32)sh.map.size();
    }
    s.index_bytes += sizeof(m.shards);
    return s;
}

std::string Store::statsLine() const
{
    Stats s = stats();
    std::ostringstream o;
    auto mb = [](U64 b) { return (F64)b / (1024.0 * 1024.0); };
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "store " << (s.read_only ? "RO " : "") << s.records << " rec, disk " << mb(s.disk_bytes) << " MB ("
      << (int)(s.live_fraction * 100.f) << "% live) in " << s.segments << " seg, limit " << mb(s.effective_limit)
      << " MB; today new " << mb(s.bytes_written_today_new) << " / compaction " << mb(s.bytes_written_today_compaction)
      << " / meta " << mb(s.bytes_written_today_meta) << " MB; hits " << s.hits << " misses " << s.misses
      << " corrupt " << s.corrupt_dropped << " evicted " << s.evicted_records << " reclaimed " << s.segments_reclaimed;
    for (size_t g = 0; g < (size_t)Group::UNBUDGETED; ++g)
    {
        o << "; " << groupName((Group)g) << " " << mb(s.groups[g].disk_bytes) << "/" << mb(s.groups[g].budget_bytes) << " MB";
    }
    // Async read latency split (averages in ms): I/O queue wait, reader I/O, decode queue
    // wait (handed-off reads only), whole request -> completion.
    auto avg = [](U64 usec, U64 n) { return n ? (F64)usec / (F64)n / 1000.0 : 0.0; };
    o.precision(2);
    o << "; reads " << s.read_jobs << " (" << s.reads_by_priority[3] << "/" << s.reads_by_priority[2] << "/"
      << s.reads_by_priority[1] << "/" << s.reads_by_priority[0] << " urgent/high/normal/low) wait "
      << avg(s.read_wait_usec, s.read_jobs) << " io " << avg(s.read_io_usec, s.read_jobs) << " decode-wait "
      << avg(s.decode_wait_usec, s.decode_handoffs) << " (" << s.decode_handoffs << " handed off, " << s.decode_inline
      << " inline) decode " << avg(s.decode_usec, s.decode_count) << " total " << avg(s.read_done_usec, s.read_done_count)
      << " max " << (F64)s.read_done_max_usec / 1000.0 << " ms; threads " << s.read_threads << " read / "
      << s.decode_threads << " decode, queued " << s.read_queue_depth << " / " << s.decode_queue_depth;
    o.precision(1);
    o << "; index " << mb(s.index_bytes) << " MB for " << s.index_records << " entries ("
      << (s.index_records ? (F64)s.index_bytes / (F64)s.index_records : 0.0) << " B/entry)";
    return o.str();
}

void Store::logStats(const char* when) const
{
    LL_INFOS("CacheStore") << (when ? when : "") << ": " << statsLine() << LL_ENDL;
}

void Store::forEach(Kind kind, const std::function<bool(const RecordInfo&)>& fn) const
{
    const Impl& m = *mImpl;
    for (const Shard& sh : m.shards)
    {
        std::vector<RecordInfo> batch;
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            for (auto kv : sh.map)
                if (kind == Kind::NONE || kv.first.kind == kind) batch.push_back(m.infoOf(sh, kv.first, kv.second));
        }
        for (const RecordInfo& r : batch)
            if (!fn(r)) return;
    }
}

U32 Store::verifyAll(const std::function<void(const Key&, Status)>& fn)
{
    Impl& m = *mImpl;
    if (!m.is_open) return 0;
    flush();
    std::vector<Key> keys;
    for (Shard& sh : m.shards)
    {
        std::lock_guard<std::mutex> lock(sh.mutex);
        for (auto kv : sh.map)
            if (kv.second.seg && !kv.second.pending()) keys.push_back(kv.first);
    }
    U32 bad = 0;
    for (const Key& k : keys)
    {
        ReadResult r = m.readImpl(k, false, 0, 0, /*count_access*/ false);   // no #705 side effects
        if (r.status != Status::OK) ++bad;
        if (fn) fn(k, r.status);
    }
    return bad;
}
