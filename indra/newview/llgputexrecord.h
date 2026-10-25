/**
 * @file llgputexrecord.h
 * @brief GPU-ready texture records in the segment store (#715, #708, #710, #865): key and
 *        metadata layout, chain lookup / read planning / write splitting and the v1 -> v2
 *        rewriter, shared by the store-backed LLGpuTexCache, the .gtx migration (#714)
 *        and the Develop > Cache Store tools.
 *
 * Pure over an LLCache::Store (llcommon + llcachestore only, no viewer globals:
 * llgputexrecord.cpp has no viewer precompiled header), so it is unit-tested on a temp
 * store (tests/llgputexrecord_test.cpp, tests/slgputexmigrate_test.cpp).
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

#ifndef LL_LLGPUTEXRECORD_H
#define LL_LLGPUTEXRECORD_H

#include "llgputexcache.h"
#include "llcachestore.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// RECORD LAYOUT (kind GPUTEX, codec 1 = zstd through the store's kind policy).
//
// KEY = (texture UUID, GPUTEX, variant):
//   bit 0      mask use (LLGpuTexCache::Key::mask; separate chains for masked and
//              unmasked uses).
//   bits 1-7   reserved for a texture ROLE (colour / normal / data) should the streamer
//              ever keep separate chains per role; today a normal map and a colour use of
//              one UUID share one chain (the streamer's shared image has no role in its key
//              and t2InfoUsable filters BC5 per use), so role 0 = "any" is the only value.
//   bit 8      0 = layout v1 (one record, the whole chain), 1 = layout v2 (per level).
//   bits 9-13  v2 only: 0 = the TAIL record, else floor(log2(longer side)) of the one
//              level a LEVEL record holds (9 = 512 .. 15 = 32768).
//   bits 14-31 0.
// Level records are keyed by their SIZE, not by an index from a chain's top: a 1024
// capture's 512 level and a later 2048 capture's 512 level are the same key, so a finer
// capture of the same encoding only adds the levels above what the store holds.
//
// LAYOUT v2 (#865; every write since, the .gtx migration, and v1 records rewritten):
//   TAIL   every level whose longer side is <= TAIL_PX (256), tightly packed, largest
//          first (a chain whose top is <= TAIL_PX is a tail alone).
//   LEVEL  one record per larger level.
//   A chain = the tail + the contiguous run of level records above it, probed in the
//   index (no I/O): the next level up must exist, carry the tail's encoding facts and
//   halve to the level below. A missing / corrupt / foreign level ends the chain there
//   (the levels above it are unreachable until a capture rewrites them); no tail = no
//   chain. A read fetches only the records a request needs: the tail plus the levels
//   between the requested top and what is resident (an upgrade copies the resident levels
//   on the GPU). Every record carries the store's own header / payload / codec hashes, so
//   a bad level drops only that level.
//   EVICTION: records are evicted one by one by the store's keep score
//   ((1 + reuse) x decode cost / bytes x recency). A level record is larger than its
//   tail and every use touches the tail (a read touches / notes reuse on the tail and the
//   levels it uses; levels above that nobody asked for keep their older access), so
//   under budget a chain loses its TOP levels first and its tail last: what is left is
//   always a usable smaller chain, never levels without a tail. (A tail found bad on a
//   read takes its levels with it; orphans left by a crash between appends are never
//   touched, so they age out first.)
//   WRITE ORDER: level records largest first, the tail last; the tail is written only if
//   every level before it was accepted, so "tail present" implies its levels were.
//
// LAYOUT v1 (#715, read only now): one record, variant = mask bit; payload = the whole mip
//   chain, level 0 first (what a .gtx file held after its 128-byte header). A v1 record
//   that is read (fully decoded anyway) is rewritten as v2 in the background (Rewriter:
//   LOW priority, the migrations' bytes/s + records/s limits) and removed only once every
//   v2 record of it is confirmed appended. Unread v1 records age out under the budget.
//   Mixed v1 / v2: a lookup serves the larger of the two chains (v2 on a tie).
//
// META (opaque to the store, little-endian):
//   v1 (27 bytes)                                   v2 (28 bytes, per record)
//    0 u8  meta version (1)                          0 u8  meta version (2)
//    1 u32 encoder_version                           1 u32 encoder_version
//    5 u32 vk_format                                 5 u32 vk_format
//    9 u8  enc   10 u8 enc_level                     9 u8  enc   10 u8 enc_level
//   11 u8  comps 12 u8 flags                        11 u8  comps 12 u8 flags
//   13 u16 w     15 u16 h      (level 0)            13 u16 w     15 u16 h  (the record's first level)
//   17 u16 full_w 19 u16 full_h                     17 u16 full_w 19 u16 full_h
//   21 u16 levels                                   21 u8  levels in this record (level: 1)
//   23 u8  alpha_grad_pct 24 u8 cov_cut             22 u8  alpha_grad_pct 23 u8 cov_cut
//   25 u16 alpha_side                               24 u16 alpha_side
//                                                   26 u8  part (1 tail, 2 level)
//                                                   27 u8  log2 of the writing capture's top
//                                                          (informational: dumps)
// Integrity: the store hashes the record header + meta and the stored payload, and the
// codec hashes the decoded bytes, so the reconstructed LLGpuTexHeader carries no payload
// hash (payload_hash = 0); its header_hash is recomputed so the struct is self-consistent.
namespace LLGpuTexRecord
{
    constexpr U8  META_VERSION_V1 = 1;
    constexpr U8  META_BYTES_V1 = 27;
    constexpr U8  META_VERSION = 2;
    constexpr U8  META_BYTES = 28;
    static_assert(META_BYTES <= 32, "store meta is at most 32 bytes");

    constexpr U32 VARIANT_MASK = 1u;
    constexpr U32 VARIANT_V2 = 1u << 8;
    constexpr U32 LEVEL_SHIFT = 9;
    constexpr U32 LEVEL_FIELD = 0x1Fu << LEVEL_SHIFT;
    // The tail holds every level whose longer side is at most this. 256: a BC7 tail is
    // ~87 KB (one small read serves any first image up to 256 px and every residency
    // floor), while 2048 / 1024 / 512 -- 94% of a 2048 chain -- are read only on demand.
    constexpr U32 TAIL_PX = 256;
    constexpr U32 MAX_SIDE_LOG2 = 15;          // 32768 (LLGpuTexHeader w/h are U16)

    enum class Part : U8 { WHOLE = 0, TAIL = 1, LEVEL = 2, UNKNOWN = 3 };
    constexpr U8 PART_TAIL = 1, PART_LEVEL = 2;   // meta byte 26

    // ---- keys
    inline LLCache::Key wholeKey(const LLUUID& id, bool mask)
    {
        return LLCache::Key(id, LLCache::Kind::GPUTEX, mask ? VARIANT_MASK : 0u);
    }
    inline LLCache::Key tailKey(const LLUUID& id, bool mask)
    {
        return LLCache::Key(id, LLCache::Kind::GPUTEX, (mask ? VARIANT_MASK : 0u) | VARIANT_V2);
    }
    U32 log2Floor(U32 v);
    // The level record holding the level whose longer side is `side` (> TAIL_PX).
    inline LLCache::Key levelKey(const LLUUID& id, bool mask, U32 side)
    {
        return LLCache::Key(id, LLCache::Kind::GPUTEX,
                            (mask ? VARIANT_MASK : 0u) | VARIANT_V2 | (log2Floor(side) << LEVEL_SHIFT));
    }
    inline bool isMaskVariant(U32 variant) { return (variant & VARIANT_MASK) != 0; }
    Part partOf(U32 variant);
    // A level key's log2 of the longer side (0 for other parts).
    inline U32 levelLog2(U32 variant) { return (variant & LEVEL_FIELD) >> LEVEL_SHIFT; }

    // ---- chain geometry (VkFormat values are the Vulkan spec's: no Vulkan header here)
    U32 mipCount(U32 w, U32 h);                          // levels down to 1 x 1
    U64 levelBytes(U32 vk_format, U32 w, U32 h);         // 0 = a format this does not know
    U64 chainBytes(U32 vk_format, U32 w, U32 h, U32 levels);
    U64 headerHash(const LLGpuTexHeader& h);

    // ---- v1 meta (whole chain): still read; packMetaV1 for tests / tools
    U8 packMetaV1(const LLGpuTexHeader& h, U8* out);     // out holds META_BYTES_V1
    bool unpackMetaV1(const U8* meta, U8 meta_len, const LLUUID& id, U32 variant, U64 payload_bytes,
                      LLGpuTexHeader& h);

    // ---- v2 meta (one record)
    struct RecMeta
    {
        U32 encoder_version = 0, vk_format = 0;
        U8  enc = 0, enc_level = 0, comps = 0, flags = 0;
        U16 w = 0, h = 0;                  // the record's first level
        U16 full_w = 0, full_h = 0;
        U8  levels = 0;                    // in this record
        U8  alpha_grad_pct = 255, cov_cut = 0;
        U16 alpha_side = 0;
        U8  part = 0;                      // PART_TAIL / PART_LEVEL
        U8  top_log2 = 0;                  // the writing capture's top (informational)
    };
    U8 packMeta(const RecMeta& m, U8* out);              // out holds META_BYTES
    bool unpackMeta(const U8* meta, U8 meta_len, RecMeta& out);
    // Same encoder facts (records of one chain must agree; "finer version wins" compares
    // a capture against the store's chain by them).
    bool sameEncoding(const RecMeta& a, const RecMeta& b);
    RecMeta metaFromHeader(const LLGpuTexHeader& h);     // w / h / levels / part left for the caller

    // A fixed-capacity list (no heap: lookups run per texture on the residency scan). A
    // chain has at most one record per level log2 9..15 (7) above the tail, plus the tail.
    constexpr size_t MAX_RECS = 8;
    template <class T>
    struct SmallList
    {
        T items[MAX_RECS] = {};
        size_t n = 0;
        void push_back(const T& v) { if (n < MAX_RECS) items[n++] = v; }
        void clear() { n = 0; }
        size_t size() const { return n; }
        bool empty() const { return n == 0; }
        T& operator[](size_t i) { return items[i]; }
        const T& operator[](size_t i) const { return items[i]; }
        const T& front() const { return items[0]; }
        const T& back() const { return items[n - 1]; }
        T* begin() { return items; }
        T* end() { return items + n; }
        const T* begin() const { return items; }
        const T* end() const { return items + n; }
    };

    // ---- the chain the index holds for (UUID, mask)
    struct Chain
    {
        bool v1 = false;
        // The servable chain: w x h = its top, levels down to 1 x 1, the facts of its top
        // record, payload_bytes = every record's bytes.
        LLGpuTexHeader hdr;
        struct Rec
        {
            LLCache::Key key;
            U32 level = 0, levels = 0;     // levels of hdr's chain the record holds
            U32 raw_bytes = 0;
        };
        SmallList<Rec> recs;               // top first: level records, then the tail (v1: one)
        U32 tail_level = 0;                // first level the tail holds (v1: 0)
        U32 last_access = 0;               // the tail's / the v1 record's
        U32 v1_write_time = 0;             // v1: the record's write time (rewrite identity)
        U64 bytes() const;
    };
    // False when there is no usable chain. No I/O: index lookups only (the tail, then one
    // per level above it, then the v1 key).
    // with_v1 = false: skip the v1 probe (a store known to hold no v1 records).
    bool resolve(const LLCache::Store& store, const LLUUID& id, bool mask, Chain& out, bool with_v1 = true);
    void infoFrom(const Chain& c, LLGpuTexCache::Info& out);

    // ---- reads
    struct ReadPlan
    {
        SmallList<size_t> recs;            // indices into Chain::recs, top first
        U32 first_level = 0;               // what the result will hold: [first, end)
        U32 end_level = 0;
        U64 bytes = 0;                     // raw bytes of those records
    };
    // Levels [first_level, levels) of the chain, minus those with a longer side <=
    // resident_top (0 = none resident). A v1 chain is always read whole. False when the
    // request needs nothing.
    bool planRead(const Chain& c, U32 first_level, U32 resident_top, ReadPlan& out);
    // The plan's read results (same order) -> blob. False with `bad` = the index in the
    // plan of the first piece that failed (status not OK, or its meta / size disagree with
    // the chain the plan was made from).
    bool assemble(const Chain& c, const ReadPlan& plan, std::vector<LLCache::ReadResult>& results,
                  LLGpuTexCache::Blob& out, size_t& bad);

    // ---- writes
    struct PartPlan
    {
        LLCache::Key key;
        U64 offset = 0, bytes = 0;         // the record's span of the whole chain
        U8  meta[32] = {};
        U8  meta_len = 0;
        bool tail = false;
        U32 w = 0, h = 0;                  // its first level
    };
    // A whole chain (level 0 first, `payload_bytes` long, laid out as hdr says) as v2
    // records: level records largest first, then the tail. above_side > 0: only the
    // levels whose longer side is greater (a finer capture on top of a chain the store
    // holds), no tail when above_side >= TAIL_PX (a smaller above_side writes the tail too).
    // False when hdr / the size do not describe a full chain of a known format (nothing to
    // write). A chain's top may be below the asset's full size (full_w / full_h): a capture
    // at a higher discard is a complete chain from ITS top down to 1 x 1.
    bool planWrite(const LLGpuTexHeader& hdr, U64 payload_bytes, std::vector<PartPlan>& out, U32 above_side = 0);

    enum class WriteMode : U8 { SKIP, ABOVE, FULL };
    // Finer version wins: a capture of the store chain's encoding is written only when its
    // top is larger, and then only the levels above the chain's top (ABOVE, above_side
    // set) -- unless the chain's top is below TAIL_PX (#991: its tail is rewritten, FULL);
    // another encoding (or no chain) replaces it (FULL). same_top (optional) = the top of
    // the store's chain of the same encoding (0 = none / another encoding): a write with
    // same_top > 0 that is not SKIP upgrades a smaller entry in place.
    WriteMode writeMode(const LLCache::Store& store, const LLGpuTexHeader& hdr, U32& above_side,
                        U32* same_top = nullptr);

    // Hand planned parts to the store in order (src = the whole chain). Stops at the
    // first refused part (its status in `refused`; the parts before it stay queued). Each
    // handed part's release runs exactly once (when the store has copied it off, also on
    // refusal). per_part may adjust a part's options (completion) before its write.
    struct PartWrite
    {
        size_t queued = 0;
        LLCache::Status refused = LLCache::Status::OK;
        std::vector<LLCache::Ticket<LLCache::Status>> tickets;
    };
    PartWrite writeParts(LLCache::Store& store, const std::vector<PartPlan>& parts, const U8* src,
                         const LLCache::WriteOptions& base, const std::function<void()>& release_part,
                         const std::function<void(size_t, LLCache::WriteOptions&)>& per_part = nullptr);

    // After a FULL write: drop what the new chain supersedes -- the v1 record and level
    // records above the new top (another capture's larger levels).
    void removeStale(LLCache::Store& store, const LLUUID& id, bool mask, U32 top_side);
    // The level records above above_side (0 = all): e.g. the orphans of a tail found bad.
    void removeLevels(LLCache::Store& store, const LLUUID& id, bool mask, U32 above_side = 0);
    // Every record of (UUID, mask): v1, tail, levels (only keys that exist: no tombstones
    // for nothing).
    void removeAll(LLCache::Store& store, const LLUUID& id, bool mask);
    // Touch / note reuse on every record of a chain (their access data must not diverge:
    // the tail must outlive its levels under eviction).
    void touchChain(LLCache::Store& store, const Chain& c, bool reuse);

    // ---- inspection
    // " v2 tail 256x256 e.. l.. v.." style suffix for a record listing ("" = not GPUTEX).
    std::string describe(const LLCache::RecordInfo& r);
    // A record that holds a full-size top level (w x h >= full size or the 2048 cap): a v1
    // record, or a v2 tail / level record of that size (SLTextureTiers' duplicate test,
    // which must still check the chain resolves).
    bool holdsFullSizeTop(const LLCache::RecordInfo& r);

    // ---- v1 -> v2 rewrite (#865)
    // A v1 record that was read in full is offered here; pump() (main thread, every
    // texture pass) writes it as v2 at LOW priority within the migrations' never-lag limits
    // and removes the v1 record once every v2 record is confirmed appended. Bounded queue
    // (an offer that does not fit is dropped: the record is rewritten on a later read).
    // Interrupting it is safe at any point: until the v1 record is removed, lookups serve
    // whichever chain is larger.
    class Rewriter
    {
    public:
        struct Limits
        {
            U64 bytes_per_sec = 24ull << 20;   // SLGpuTexMigrate::Options defaults
            U32 records_per_sec = 400;
            U64 max_queued_bytes = 64ull << 20;
            U32 max_queued = 512;
            U64 max_in_flight_bytes = 32ull << 20;
        };
        struct Stats
        {
            U64 offered = 0, queued = 0, dropped_full = 0, rewritten = 0, failed = 0, stale = 0, cancelled = 0;
            U64 bytes_rewritten = 0;
            U32 queue = 0, in_flight = 0;
            U64 queue_bytes = 0;
        };

        void setStore(LLCache::Store* store) { mStore = store; }
        void setLimits(const Limits& l) { mLimits = l; }
        // c = the v1 chain the read was planned from; blob = its result (one whole piece).
        bool offer(const Chain& c, const std::shared_ptr<const LLGpuTexCache::Blob>& blob, bool mask);
        void cancel(const LLUUID& id, bool mask);
        void pump(F64 now_sec);
        // Drop the queue (shutdown); in-flight writes finish on the store's writer (their
        // bytes stay alive with them) and their v1 records stay (rewritten next session).
        void clear();
        Stats stats() const;
        bool idle() const { return mQueue.empty() && mInFlight.empty(); }

    private:
        struct Job
        {
            LLUUID id;
            bool mask = false;
            U32 v1_write_time = 0;
            U32 v1_raw = 0;
            LLGpuTexHeader hdr;
            std::shared_ptr<const LLGpuTexCache::Blob> blob;
            U64 bytes = 0;
            bool cancelled = false;
            std::vector<LLCache::Ticket<LLCache::Status>> tickets;
        };
        bool v1Current(const Job& j) const;
        void settle(Job& j);

        LLCache::Store* mStore = nullptr;
        Limits mLimits;
        std::deque<Job> mQueue, mInFlight;
        U64 mQueueBytes = 0, mInFlightBytes = 0;
        F64 mLast = -1.0;
        F64 mTokBytes = 0.0, mTokRecs = 0.0;
        Stats mStats;
    };
}

#endif // LL_LLGPUTEXRECORD_H
