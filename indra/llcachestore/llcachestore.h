/**
 * @file llcachestore.h
 * @brief The segment store: one budgeted, crash-safe, append-only disk cache
 *        for textures, assets and cluster LOD pages (#715, #708, #709).
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

#ifndef LL_LLCACHESTORE_H
#define LL_LLCACHESTORE_H

#include "llpreprocessor.h"
#include "stdtypes.h"
#include "lluuid.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*
 * ON-DISK CONTRACT -- segment store, format version 1
 * ===================================================
 *
 * A store is one folder (<cache>/store):
 *
 *   lock                 Lock file. The instance holding an exclusive lock on
 *                        it (flock / LockFileEx) owns the store and is the only
 *                        writer. Its contents (the owner's pid, text) are
 *                        informational. A crash releases the lock.
 *   seg-NNNNNNNN.dat     Segment files, NNNNNNNN = sequence number (8 hex
 *                        digits, starting at 00000001, never reused). Append
 *                        only; records are never modified in place. A segment
 *                        is closed for appends at Config::segment_bytes
 *                        (default 256 MiB) and later deleted whole.
 *   index.snap           Index snapshot (optional: a cache, rebuildable from
 *   index.snap.prev      the segments). The previous snapshot is kept.
 *   overflow-<pid>-<n>/  Private overflow of a second (read-only) instance:
 *                        a complete store of its own, deleted when that
 *                        instance exits; deleted by the owner at startup when
 *                        no process holds its lock.
 *
 * Every multi-byte field is little-endian and written byte by byte. Hashes
 * are XXH3-64 with seed 0 (HBXXH64::digest), not the older XXH64.
 *
 * Segment header (64 bytes, at offset 0):
 *    0  4  magic 'S' 'L' 'C' 'S'
 *    4  2  segment format version (1)
 *    6  2  reserved (0)
 *    8  4  sequence number (matches the file name)
 *   12  4  creation time (unix seconds)
 *   16 40  reserved (0)
 *   56  8  XXH3-64 of bytes [0, 56)
 * Records follow from offset 64, each starting on a 16-byte boundary.
 *
 * Record (header 96 bytes + meta + payload, zero-padded to a multiple of 16):
 *    0  4  magic 'S' 'L' 'C' 'R'
 *    4  1  record format version (1)
 *    5  1  kind (LLCache::Kind). A TOMBSTONE carries the removed key's
 *          kind; a CLEAR_KIND marker has kind 0 and the cleared kind in
 *          `variant`
 *    6  1  flags: 0x01 TOMBSTONE  the key was removed (no payload)
 *                 0x02 CLEAR_KIND every earlier record of kind `variant` is
 *                                 dead (Clear Cache; uuid null, no payload)
 *                 0x04 SHARED     placement hint was SHARED
 *                 0x08 COMPACTED  re-appended by compaction (informational)
 *                 others reserved, 0
 *    7  1  codec: 0 = payload is the record bytes as is (required for kinds
 *                     read by range: J2C, CLUSTER)
 *                 1 = payload is one LLCacheCodec blob (llcachecodec.h:
 *                     24-byte 'SLCZ' header + raw or zstd data, its own
 *                     XXH3 of the decoded bytes)
 *    8 16  key UUID
 *   24  4  key variant
 *   28  4  kind format version (KindPolicy::format_version at write time;
 *          records of another version are ignored: per-kind invalidation,
 *          never a whole-store wipe)
 *   32  1  meta length (0..32)
 *   33  3  reserved (0)
 *   36  4  stored payload bytes (after the codec)
 *   40  4  raw bytes (decoded record size)
 *   44  4  write time (unix seconds)
 *   48 16  placement region UUID (null when SHARED / no hint)
 *   64  8  placement region grid handle (0 when unknown)
 *   72  8  XXH3-64 of the stored payload bytes
 *   80  8  reserved (0)
 *   88  8  XXH3-64 of bytes [0, 88) followed by the meta bytes
 *   96  -  meta (opaque per-kind metadata), then the payload
 * A record is valid only if its header hash matches; its payload is valid
 * only if the payload hash matches (and, for codec 1, the codec decodes it).
 * Later records win: order = (segment sequence, offset). A record past the
 * end of its file is a torn write. Readers resynchronise after a bad header
 * by searching forward on 16-byte boundaries for the next valid header.
 * Range reads (codec 0 only) verify the header hash, not the payload hash;
 * whole reads and verifyAll() verify both.
 *
 * Region manifest records (kind MANIFEST, key = region UUID, codec 1) hold:
 *   u32 version (1), uuid region, u64 grid handle, u32 last visit,
 *   u32 visit count, u32 n + n x {u32 segment, u32 offset, u32 length} runs,
 *   u32 n + n x {uuid, u8 kind, u32 variant} shared references,
 *   u32 n + n x {uuid, u8 kind, u32 variant} first-request order.
 * Manifests are advisory: dropping them never touches data records.
 *
 * Index snapshot: 'SLCI', u16 version (1), the per-segment "indexed through"
 * offsets, the region table, every on-disk entry (key, location, sizes,
 * access data, meta) and a trailing XXH3-64 of everything before it. Written
 * as index.snap.tmp, synced, then renamed over index.snap (the old one
 * becomes index.snap.prev). At open: snapshot -> .prev -> full scan; entries
 * pointing past the end of their segment are dropped; segment bytes past the
 * "indexed through" offset are scanned. A bad snapshot is never fatal.
 *
 * Crash safety: a crash loses at most the unflushed write buffers (a few MB,
 * re-downloaded) and the half-written tail of the active segment, which the
 * owner truncates at the next open. Nothing ever wipes the whole store: bad
 * records are dropped one by one.
 */

namespace LLCache
{
    // ------------------------------------------------------------------ keys

    // What a record holds. Values are on disk: append only, never renumber.
    enum class Kind : U8
    {
        NONE     = 0,  // control records (tombstones of no kind, clear markers)
        J2C      = 1,  // texture codestream prefix (variant 0); range reads
        GPUTEX   = 2,  // GPU-ready mip chain (variant bit 0 = mask use); zstd
        ASSET    = 3,  // any asset blob except mesh (asset type in meta)
        MESH     = 4,  // mesh asset part (variant = part: header, LOD0-3, skin, physics)
        CLUSTER  = 5,  // cluster LOD pages (variant = face); range reads
        SCRATCH  = 6,  // upload / temp staging: unbudgeted, dropped at the next start
        MANIFEST = 7,  // region manifest (key = region UUID)
        COUNT
    };
    const char* kindName(Kind kind);

    struct Key
    {
        LLUUID id;
        Kind   kind    = Kind::ASSET;
        U32    variant = 0;

        Key() = default;
        Key(const LLUUID& i, Kind k, U32 v = 0) : id(i), kind(k), variant(v) {}
        bool operator==(const Key& o) const { return id == o.id && kind == o.kind && variant == o.variant; }
        bool operator!=(const Key& o) const { return !(*this == o); }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            return (size_t)(k.id.getDigest64() ^ ((U64)k.kind << 56) ^ ((U64)k.variant * 0x9E3779B97F4A7C15ull));
        }
    };

    // Where a write's bytes should live (#708 placement). Advisory: a wrong or
    // missing hint costs locality, never correctness.
    struct PlacementHint
    {
        LLUUID region;          // null = SHARED (avatars, inventory, UI, multi-region)
        U64    grid_handle = 0; // kept for lookup; may change for the same UUID

        static PlacementHint shared() { return PlacementHint(); }
        static PlacementHint inRegion(const LLUUID& r, U64 handle = 0) { PlacementHint h; h.region = r; h.grid_handle = handle; return h; }
        bool isShared() const { return region.isNull(); }
    };

    // Budget groups (owner decision 2026-10-18). Sounds stay outside the store.
    enum class Group : U8
    {
        TEXTURES   = 0,  // J2C + GPU-ready (GPU-ready share capped, ~30%)
        ASSETS     = 1,  // assets + mesh (+ manifests)
        CLUSTER    = 2,  // cluster LOD pages
        UNBUDGETED = 3,  // scratch, control records
        COUNT
    };
    const char* groupName(Group group);

    // Per-kind storage policy.
    struct KindPolicy
    {
        bool  compress       = false;   // zstd via LLCacheCodec; stored as codec 1 either way unless ranged_reads
        S32   level          = 0;       // zstd level, 0 = LLCacheCodec::defaultLevel()
        U32   format_version = 1;       // bump = every record of this kind is ignored (<= 0xffff)
        bool  ranged_reads   = false;   // stored as codec 0 (never compressed): readRange = one pread
        Group group          = Group::ASSETS;
        F32   share_cap      = 1.f;     // max fraction of the group budget for this kind
        F32   decode_cost    = 1.f;     // relative cost to rebuild (eviction weight, #705)
    };

    // What the index knows without I/O.
    struct RecordInfo
    {
        Key    key;
        U32    stored_bytes        = 0;   // payload on disk (after the codec)
        U32    raw_bytes           = 0;   // decoded size
        U32    kind_version        = 0;
        U8     codec               = 0;   // 0 raw, 1 LLCacheCodec blob
        U32    write_time          = 0;   // unix seconds
        // Access data lives in RAM and the snapshot; reads never write to disk.
        // last_access:  last touch (this or an earlier session).
        // prev_session_access: last_access as loaded at open, i.e. the last
        //   access in an EARLIER session (0 = never accessed before this run);
        //   minute precision (#866: the index packs it).
        // reuse_count: +1 on the first touch in a session of a record with
        //   prev_session_access != 0, and on noteReuse() (#705 admission).
        U32    last_access         = 0;
        U32    prev_session_access = 0;
        U16    reuse_count         = 0;
        bool   shared              = false; // used from more than its placement region
        bool   queued              = false; // accepted, not on disk yet (served from RAM)
        bool   staged              = false; // open staged write (size = bytes so far)
        bool   pinned              = false; // WF_PIN_SESSION and not read yet
        LLUUID region;                      // placement region (null = shared)
        U8     meta_len            = 0;
        U8     meta[32]            = {};
    };

    enum class Status : U8
    {
        OK, MISSING, CORRUPT, IO_ERROR, NO_SPACE, REFUSED_BUSY, READ_ONLY, CANCELLED, SKIPPED,
    };
    const char* statusName(Status s);

    struct ReadResult
    {
        Status          status = Status::MISSING;
        RecordInfo      info;
        std::vector<U8> bytes;   // decoded; a range read returns [offset, offset+len) clipped
    };

    // Async reads are served strictly by priority (FIFO within one), in the I/O queue and
    // again in the decode queue. LOW = background (prefetch, migration, verify); NORMAL =
    // demand that can wait a frame or two; HIGH = visible / near now; URGENT = blocking
    // something on screen (self / HUD).
    enum class Priority : U8 { LOW = 0, NORMAL = 1, HIGH = 2, URGENT = 3 };
    constexpr size_t PRIORITY_COUNT = 4;

    // ----------------------------------------------------------- async

    // Runs a task on the caller's thread of choice (e.g. a WorkQueue: see
    // llcachestore_workqueue.h). Returns false if the queue is gone.
    using Poster = std::function<bool(std::function<void()>)>;

    // How a finished ticket tells its owner. The store never picks a thread:
    // `notify` runs through `post` when set, else inline on the store's I/O
    // thread (keep it cheap). `wake` (optional) always runs inline, e.g. to
    // wake a parked fetch worker that then polls its ticket.
    struct Completion
    {
        Poster                post;
        std::function<void()> notify;
        std::function<void()> wake;
    };

    namespace detail
    {
        template <class T>
        struct TicketState
        {
            std::mutex              mutex;
            std::condition_variable cv;
            bool                    done = false;
            bool                    taken = false;
            std::atomic<bool>       cancelled{ false };
            T                       value{};
            Completion              completion;
        };

        template <class T>
        void complete(const std::shared_ptr<TicketState<T>>& st, T value)
        {
            if (!st)
            {
                return;
            }
            Completion c;
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (st->done)
                {
                    return;
                }
                st->value = std::move(value);
                st->done = true;
                c = std::move(st->completion);
            }
            st->cv.notify_all();
            if (st->cancelled.load())
            {
                return;
            }
            if (c.wake)
            {
                c.wake();
            }
            if (c.notify)
            {
                if (c.post)
                {
                    c.post(c.notify);   // false = the queue is gone: nobody to tell
                }
                else
                {
                    c.notify();
                }
            }
        }
    }

    // A pending result. Poll isDone()/take(), wait(), or use a Completion.
    template <class T>
    class Ticket
    {
    public:
        Ticket() = default;
        explicit Ticket(std::shared_ptr<detail::TicketState<T>> st) : mState(std::move(st)) {}

        bool valid() const { return (bool)mState; }
        bool isDone() const
        {
            if (!mState) return true;
            std::lock_guard<std::mutex> lock(mState->mutex);
            return mState->done;
        }
        // Block until done or timeout; true when done.
        bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000)) const
        {
            if (!mState) return true;
            std::unique_lock<std::mutex> lock(mState->mutex);
            return mState->cv.wait_for(lock, timeout, [this] { return mState->done; });
        }
        // The result, once (later calls return a default T). Call when isDone().
        T take()
        {
            if (!mState) return T{};
            std::lock_guard<std::mutex> lock(mState->mutex);
            if (!mState->done || mState->taken) return T{};
            mState->taken = true;
            return std::move(mState->value);
        }
        // Best effort: a queued read is skipped, a finished one dropped; no
        // completion runs after cancel() returns... unless it was already running.
        void cancel() { if (mState) mState->cancelled = true; }
        bool cancelled() const { return mState && mState->cancelled.load(); }

    private:
        std::shared_ptr<detail::TicketState<T>> mState;
    };

    // ----------------------------------------------------------- writes

    // Where write bytes come from: owned bytes, or a borrowed span the store
    // copies off on its writer thread and then calls release() (e.g. a mapped
    // Vulkan staging buffer). release() runs exactly once, also on refusal.
    struct WriteSource
    {
        std::vector<U8>       owned;
        const U8*             borrowed   = nullptr;
        size_t                borrowed_n = 0;
        std::function<void()> release;

        static WriteSource copy(const void* p, size_t n)
        {
            WriteSource s; s.owned.assign((const U8*)p, (const U8*)p + n); return s;
        }
        static WriteSource take(std::vector<U8>&& v) { WriteSource s; s.owned = std::move(v); return s; }
        static WriteSource borrow(const void* p, size_t n, std::function<void()> rel)
        {
            WriteSource s; s.borrowed = (const U8*)p; s.borrowed_n = n; s.release = std::move(rel); return s;
        }
        size_t size() const { return borrowed ? borrowed_n : owned.size(); }
    };

    enum WriteFlags : U32
    {
        WF_NONE               = 0,
        WF_SUPERSEDE_PENDING  = 1,  // always on: a newer write replaces a queued one
        WF_SKIP_IF_NOT_LARGER = 2,  // skip (Status::SKIPPED) if the live record has >= raw bytes
        WF_PIN_SESSION        = 4,  // not evicted before its first read this session; on
                                    // a failed append held in RAM (bounded) until read
    };

    struct WriteOptions
    {
        U32           flags    = WF_NONE;
        Priority      priority = Priority::NORMAL;   // URGENT flushes its run now
        PlacementHint placement;                     // default SHARED
        const U8*     meta     = nullptr;            // RecordInfo::meta, copied
        U8            meta_len = 0;                  // <= 32
        Completion    completion;                    // for the `done` ticket
    };

    // ----------------------------------------------------------- policy

    // Score inputs for eviction (#705 / #709).
    struct EvictionContext
    {
        U32 now            = 0;   // unix seconds
        U32 region_recency = 0;   // most recent visit of a region using the record (0 = none)
        F32 decode_cost    = 1.f; // KindPolicy::decode_cost
    };

    // Pluggable per group. Records are marked dead in ascending (tier, score).
    struct EvictionPolicy
    {
        virtual ~EvictionPolicy() = default;
        // Lower tier goes first. Default: duplicates 0, GPUTEX 1, everything else 2
        // (GPU-ready before J2C; J2C is the source of truth).
        virtual int tier(const RecordInfo& r) const;
        // Higher = keep longer. Default: (1 + reuse) x decode_cost / bytes x region
        // recency weight (half-life 14 days on the newest of last access / region visit).
        virtual F32 keepScore(const RecordInfo& r, const EvictionContext& ctx) const;
        // J2C covered by a full-size GPU-ready record, etc. Default false.
        virtual bool isDuplicate(const RecordInfo&) const { return false; }
    };

    // SSD wear protection (#709) and HDD locality (#712).
    struct WearPolicy
    {
        bool ssd                       = true;   // from drive detection; unknown = SSD
        F32  reclaim_live_max          = 0.25f;  // reclaim only segments <= this live
        F32  daily_compaction_fraction = 0.10f;  // rescue cap: fraction of the day's new bytes
        U64  daily_compaction_cap_bytes = 0;     // if > 0, a fixed cap (max with the fraction)
        bool locality_rewrites         = false;  // HDD hot-set gathering; forced off on SSD
        U32  locality_min_segments     = 8;      // gather a region spread over more segments
    };

    enum class DriveType : U8 { UNKNOWN, SSD, HDD };
    DriveType detectDriveType(const std::string& path);

    // ----------------------------------------------------------- stats

    struct GroupStats
    {
        U64 budget_bytes = 0;      // configured
        U64 disk_bytes   = 0;      // on-disk bytes written for this group, live + dead
        U64 live_bytes   = 0;
        U32 records      = 0;
    };
    struct KindStats
    {
        U64 live_bytes = 0;            // on-disk record bytes (header + meta + stored payload) of live records
        U32 records    = 0;
        // additive (#715 measurement)
        U64 raw_bytes       = 0;       // decoded payload bytes of the live records (what consumers get)
        U64 disk_bytes      = 0;       // bytes of this kind in the segments, live + dead (exact: per-segment attribution)
        U32 segments        = 0;       // segment files holding at least one record of this kind (live or dead)
        U64 evicted_records = 0;       // budget / reclaim evictions (not superseded, not removed, not corrupt)
        U64 evicted_bytes   = 0;
    };
    struct Stats
    {
        bool ready = false, read_only = false;
        U64  disk_bytes = 0, live_bytes = 0, dead_bytes = 0, peak_disk_bytes = 0;
        U64  effective_limit = 0;      // min(sum of budgets, disk + free - reserve)
        U32  segments = 0, records = 0;
        F32  live_fraction = 1.f;
        // #709 daily write accounting (UTC day; survives restarts via the snapshot)
        U32  day = 0;
        U64  bytes_written_today_new = 0, bytes_written_today_compaction = 0, bytes_written_today_meta = 0;
        U64  bytes_written_total = 0, bytes_read = 0;
        U64  hits = 0, misses = 0, corrupt_dropped = 0, write_fail = 0, writes_refused = 0;
        U64  evicted_records = 0, evicted_bytes = 0, segments_reclaimed = 0, rescued_records = 0;
        U64  superseded = 0, skipped = 0, ram_fallback_bytes = 0;
        U32  reads_in_flight = 0, writes_queued = 0;   // reads: past the lookup, not finished (I/O + decode queue + decode)
        U64  write_bytes_queued = 0;
        U64  prefetch_bytes = 0, prefetch_hits = 0;
        DriveType drive = DriveType::UNKNOWN;
        GroupStats groups[(size_t)Group::COUNT];
        KindStats  kinds[(size_t)Kind::COUNT];
        U64  overflow_records = 0;     // second instance: records in the private overflow
        // additive (#715 measurement): time is steady-clock wall time on the thread doing
        // the work (writer for encode / append, readers for decode); the codec and the
        // pwrite are synchronous, so it is that thread's busy time.
        U64  encode_count = 0, encode_usec = 0, encode_in_bytes = 0, encode_out_bytes = 0;   // LLCacheCodec encode (zstd / identity)
        U64  decode_count = 0, decode_usec = 0, decode_in_bytes = 0, decode_out_bytes = 0;   // decode (disk + queued records)
        U64  append_count = 0, append_usec = 0, append_bytes = 0;    // segment pwrite calls (MB/s = bytes / usec)
        U64  index_load_usec = 0;      // open: snapshot load + tail scan (0 until ready)
        U32  index_load_time = 0;      // unix seconds when the index load finished
        // This run's writes by class (the *_today_* counters span sessions of one UTC day;
        // bytes_written_total is this run's sum of the three).
        U64  bytes_written_session_new = 0, bytes_written_session_compaction = 0, bytes_written_session_meta = 0;
        // additive (read scheduling): where async read latency goes (read() / readRange()
        // tickets; readSync runs on the caller and is not counted here). Steady-clock usec.
        U32  read_threads = 0, decode_threads = 0;   // pool sizes in use
        U64  read_jobs = 0;                 // async reads a reader picked up
        U64  read_wait_usec = 0;            // request -> a reader picked it up (I/O queue)
        U64  read_io_usec = 0;              // reader: lookup + pread + header / payload-hash checks
        U64  decode_handoffs = 0;           // reads whose decode ran on the decode pool
        U64  decode_wait_usec = 0;          // handoff -> a decode thread picked it up (decode queue)
        U64  decode_inline = 0;             // codec-1 decodes left on the reader (small, or queue full)
        U64  read_done_count = 0, read_done_usec = 0, read_done_max_usec = 0;   // request -> completion
        U64  reads_by_priority[PRIORITY_COUNT] = {};
        U32  read_queue_depth = 0, decode_queue_depth = 0;   // now
        U64  decode_queue_bytes = 0;        // now: decoded bytes waiting on the decode pool
        // additive (#866): RAM held by the in-memory index (capacity: records, lookup
        // arrays, meta, queued-write map) and its entries (records + queued-only keys).
        U64  index_bytes = 0;
        U32  index_records = 0;
    };

    struct RegionInfo
    {
        LLUUID region;
        U64    grid_handle = 0;
        U32    last_visit  = 0;
        U32    visit_count = 0;
        U32    own_records = 0;
        U64    own_bytes   = 0;
        U32    shared_refs = 0;
    };

    // ----------------------------------------------------------- the store

    class Store
    {
    public:
        struct Config
        {
            std::string dir;                        // <cache>/store (created)
            U64  segment_bytes      = 256ull << 20; // 128-256 MiB recommended (1 MiB .. 1 GiB)
            // Async reads run in two stages: reader threads do the pread and the header /
            // payload-hash checks (I/O bound, normal OS priority); a zstd decode of a
            // large record is handed to the decode pool (CPU bound, below-normal OS
            // priority), so a burst of big texture decodes never holds up small reads.
            // 0 = auto: readers from the drive (HDD 1, SSD / unknown cores/8 in 2..4),
            // decoders cores/4 in 1..4. Overrides: readers 1..16, decoders 1..max(1, cores/2).
            U32  read_threads       = 0;
            U32  decode_threads     = 0;
            U64  decode_queue_bytes = 64ull << 20;  // decoded bytes queued on the pool; beyond: readers decode inline
            bool read_only_if_locked = true;        // else open() fails when locked
            bool force_read_only    = false;        // open read-only even if unlocked
            bool overflow           = true;         // read-only: private overflow for writes
            std::string overflow_dir;               // default <dir>/overflow-<pid>-<n>
            U64  overflow_bytes     = 512ull << 20; // overflow budget (all groups)
            U32  run_flush_bytes    = 4u << 20;     // per-placement run buffer
            U32  run_flush_ms       = 2000;
            U64  max_queued_bytes   = 256ull << 20; // beyond: REFUSED_BUSY (assets exempt)
            U64  ram_fallback_bytes = 64ull << 20;  // pinned records held in RAM on write failure
            U32  snapshot_interval_s = 1800;        // periodic snapshot (if changed); always at close
            U32  refresh_interval_s = 60;           // read-only: rescan the owner's new records
            bool async_open         = false;        // load on the writer thread; lookups miss until ready()
            bool detect_drive       = true;
            // Prefetch: Linux default = page-cache hints; buffer mode = bounded
            // read-ahead buffer consulted by reads (Windows default).
            enum class PrefetchMode : U8 { AUTO, HINT, BUFFER } prefetch_mode = PrefetchMode::AUTO;
            U64  prefetch_buffer_bytes = 256ull << 20;
            U64  prefetch_max_bytes    = 512ull << 20; // per region request

            // Test / tool hooks. All optional.
            std::function<U32()> clock;                         // unix seconds
            std::function<S64()> free_space;                    // bytes free (-1 unknown)
            // Called before each append; return how many of `len` bytes the
            // "disk" accepts (< len = a failed / short write, e.g. ENOSPC).
            std::function<size_t(U32 segment, U64 offset, size_t len)> write_fault;
            // Called on the store thread at the start of each async read job: by a reader
            // (decode = false) and again by a decode thread when it takes a handed-off
            // decode (decode = true). Tests block on it to hold a stage busy.
            std::function<void(const Key& key, bool decode)> job_hook;
        };

        Store();
        ~Store();   // close()
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        // Lock, load snapshot, scan tails (synchronously unless async_open).
        // Locked by another instance: read-only + private overflow
        // (read_only_if_locked), else false. Never wipes anything.
        bool open(const Config& config);
        // Commit staged writes (SCRATCH dropped), flush runs, save region
        // manifests and the snapshot, stop threads. A read-only instance
        // deletes its overflow (its records are discarded; see #715).
        void close();
        bool isOpen() const;
        bool ready() const;
        bool waitReady(std::chrono::milliseconds timeout) const;
        bool readOnly() const;
        const std::string& dir() const;

        void       setKindPolicy(Kind kind, const KindPolicy& policy); // a format_version change drops that kind's records
        KindPolicy kindPolicy(Kind kind) const;

        // ---- index (any thread, no I/O)
        bool lookup(const Key& key, RecordInfo* out = nullptr) const;
        bool exists(const Key& key) const { return lookup(key); }
        void touch(const Key& key);                               // RAM only
        void noteReuse(const Key& key);                           // e.g. decoded again after VRAM eviction
        // The record was used from this placement (promotes to shared when it
        // differs from where it was written; feeds region manifests + recency).
        void noteUse(const Key& key, const PlacementHint& where);

        // ---- reads (any thread). Codec-1 payloads are decoded; corrupt records
        // are dropped from the index and reported as CORRUPT.
        Ticket<ReadResult> read(const Key& key, Priority prio = Priority::NORMAL, Completion done = {});
        Ticket<ReadResult> readRange(const Key& key, U64 offset, U64 len,
                                     Priority prio = Priority::NORMAL, Completion done = {});
        ReadResult readSync(const Key& key);
        ReadResult readRangeSync(const Key& key, U64 offset, U64 len);

        // ---- writes: queued to the writer thread, visible to lookup/read at
        // once (read-your-writes). False = refused now (Status in *done:
        // READ_ONLY without overflow, REFUSED_BUSY, NO_SPACE, SKIPPED...).
        bool write(const Key& key, WriteSource src, const WriteOptions& opts = WriteOptions(),
                   Ticket<Status>* done = nullptr);

        // ---- staged writes: a per-key table independent of any object's
        // lifetime (the asset transfer code opens one object per packet).
        // While staged, lookup / read / readRange see the staged bytes.
        bool stagedBegin(const Key& key);                     // WRITE-open: start (or restart) empty
        bool stagedAppend(const Key& key, const void* p, size_t n); // APPEND: continue (starts if absent)
        bool isStaged(const Key& key) const;
        // Finish: the staged bytes become one record under final_key (rename
        // or plain finish when final_key == key). Removes the staging entry.
        bool stagedCommit(const Key& key, const Key& final_key, const WriteOptions& opts = WriteOptions(),
                          Ticket<Status>* done = nullptr);
        void stagedAbort(const Key& key);                     // remove() also aborts

        // ---- removal
        bool remove(const Key& key);       // index + tombstone record
        // Re-key a committed record without copying data (LLFileSystem::renameFile).
        bool rename(const Key& from, const Key& to);
        void removeAll(Kind kind);         // Clear Cache for a kind; frees disk now (uncapped compaction)
        void clearAll();                   // Clear Cache: deletes every segment now

        // Push buffered runs to disk and wait (tests, shutdown, before a snapshot).
        void flush();
        // Write the index snapshot now (owner only).
        bool saveSnapshot();

        // ---- regions (#708 placement, #712 prefetch, #705 recency)
        void regionVisit(const LLUUID& region, U64 grid_handle);
        bool regionInfo(const LLUUID& region, RegionInfo* out) const;
        void saveRegionManifests();        // dirty manifests -> MANIFEST records
        // Read the region's runs (sorted by segment + offset), then its shared
        // references, in the background below demand reads. Cancellable.
        bool prefetchRegion(const LLUUID& region, Priority prio = Priority::LOW);
        void cancelPrefetch(const LLUUID& region);
        // #712 hook (HDD + locality_rewrites only): re-append a region's records
        // into one run when spread over many segments, within the daily cap.
        bool gatherRegion(const LLUUID& region);

        // ---- budgets, eviction, wear (#705 #709 #711)
        void setBudget(Group group, U64 bytes);
        U64  budget(Group group) const;
        void setEvictionPolicy(Group group, std::shared_ptr<EvictionPolicy> policy);
        void setWearPolicy(const WearPolicy& policy);
        WearPolicy wearPolicy() const;
        void setDiskReserve(U64 bytes);    // appends limited by min(budgets, disk + free - reserve)
        void maintain();                   // run a budget / reclaim pass now and wait

        Stats       stats() const;
        std::string statsLine() const;
        void        logStats(const char* when) const;

        // ---- inspection (#714)
        void forEach(Kind kind, const std::function<bool(const RecordInfo&)>& fn) const;
        // Read and verify every on-disk record; fn(key, status) for each.
        // Does not count as an access (no touch / reuse / unpin); bad records
        // are dropped like on any read.
        U32  verifyAll(const std::function<void(const Key&, Status)>& fn);

        struct Impl;
    private:
        std::unique_ptr<Impl> mImpl;
    };
}

#endif // LL_LLCACHESTORE_H
