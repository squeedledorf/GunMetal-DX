/**
 * @file llcachestore_index.h
 * @brief Internal: the store's in-RAM index, one table per shard (#866).
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
 * Layout (#866: was std::unordered_map<Key, Entry> at ~150 B per record)
 * ------
 * - Records: 60-byte {uuid, variant, Entry} slots in fixed chunks of CHUNK
 *   records, dense (record i lives at chunk i / CHUNK). Growing never moves a
 *   record; erase moves the last record into the hole (swap-remove).
 * - Lookup: an open-addressed array of U32 (8-bit hash tag | record index + 1),
 *   linear probing, backward-shift deletion (no tombstones), any capacity
 *   (fastrange on the high hash bits), max load 85%, grows by 1.5x. A grow
 *   rebuilds only that U32 array from the records (no record moves).
 * - Meta (only GPU-ready records have it): a side pool keyed by record index.
 *
 * Pointer / iterator rules (unlike the node map this replaced):
 * - An insert never moves an existing record: Entry& and iterators stay valid.
 * - erase() moves the shard's last record into the erased position: every
 *   Entry& into the shard except the erased one stays valid EXCEPT the moved
 *   record's; erase(it) returns an iterator at the same position, which now holds
 *   the not-yet-visited former last record, so erase-while-iterating loops visit
 *   each record once.
 *
 * Precision (documented change, #866): prev_access and recency are kept at
 * minute precision (24-bit minutes since 2020-01-01, saturating in 2051; 0 =
 * never). write_time and last_access stay exact seconds. Eviction is not
 * affected: it uses max(last_access, recency, ...) and noteUse() sets recency
 * and last_access together, so recency <= last_access. kind_version is kept in
 * 16 bits (KindPolicy::format_version is clamped to 0xffff by setKindPolicy) and
 * region slots in 24 bits (slots beyond are stored as no region).
 */

#ifndef LL_LLCACHESTORE_INDEX_H
#define LL_LLCACHESTORE_INDEX_H

#include "llcachestore.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace LLCacheStoreIndex
{
    using LLCache::Key;
    using LLCache::Kind;

    constexpr U32 NO_REGION = 0xffffffffu;

    // Entry flags (RAM; EF_RENAMED and EF_SHARED persist in the snapshot).
    constexpr U8 EF_SHARED   = 0x01;
    constexpr U8 EF_RENAMED  = 0x02;   // disk header holds another key (rename())
    constexpr U8 EF_PINNED   = 0x04;   // WF_PIN_SESSION, unread
    constexpr U8 EF_TOUCHED  = 0x08;   // touched this session
    constexpr U8 EF_RAM_ONLY = 0x10;   // append failed; held in RAM until read
    constexpr U8 EF_PENDING  = 0x20;   // newer bytes queued (or RAM-only): see Shard::pend
    constexpr U8 EF_CODEC1   = 0x40;   // disk record codec = CODEC_SHIM (else CODEC_NONE)
    constexpr U8 EF_PERSIST  = EF_SHARED | EF_RENAMED;

    // Minute stamps (prev_access, recency).
    constexpr U32 STAMP_EPOCH = 1577836800u;   // 2020-01-01 00:00 UTC
    inline U32 stampEncode(U32 t)
    {
        if (!t) return 0;
        U32 m = t > STAMP_EPOCH ? (t - STAMP_EPOCH) / 60u + 1u : 1u;
        return std::min<U32>(m, 0xffffffu);
    }
    inline U32 stampDecode(U32 m) { return m ? STAMP_EPOCH + (m - 1u) * 60u : 0; }

    // One indexed record (the key lives beside it in the table). 40 bytes.
    struct Entry
    {
        U32 seg = 0;            // 0 = not on disk
        U32 offset = 0;
        U32 stored = 0;
        U32 raw = 0;
        U32 write_time = 0;
        U32 last_access = 0;
        U16 reuse = 0;
        U8  flags = 0;
        U8  meta_len = 0;       // set with Index::setMeta only
    private:
        U16 kind_version_ = 0;
        U8  region_[3] = { 0xff, 0xff, 0xff };
        U8  prev_[3] = {};
        U8  recency_[3] = {};
        U8  kind_ = 0;          // the key's kind (owned by the Index)

        static U32 get24(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16); }
        static void put24(U8* p, U32 v) { p[0] = (U8)v; p[1] = (U8)(v >> 8); p[2] = (U8)(v >> 16); }
        friend class Index;

    public:
        U32  kindVersion() const { return kind_version_; }
        void setKindVersion(U32 v) { kind_version_ = (U16)std::min<U32>(v, 0xffffu); }
        U32  region() const { U32 r = get24(region_); return r == 0xffffffu ? NO_REGION : r; }
        void setRegion(U32 slot) { put24(region_, slot >= 0xffffffu ? 0xffffffu : slot); }
        U32  prevAccess() const { return stampDecode(get24(prev_)); }
        void setPrevAccess(U32 t) { put24(prev_, stampEncode(t)); }
        U32  recency() const { return stampDecode(get24(recency_)); }
        void setRecency(U32 t) { put24(recency_, stampEncode(t)); }
        U8   codec() const { return (flags & EF_CODEC1) ? 1 : 0; }
        void setCodec(U8 c) { flags = c ? (U8)(flags | EF_CODEC1) : (U8)(flags & ~EF_CODEC1); }
        bool pending() const { return (flags & EF_PENDING) != 0; }

        // Copy another entry's record data; keeps this entry's key kind, meta_len
        // (Index::setMeta) and EF_PENDING (Shard::setPending).
        void assignFrom(const Entry& o)
        {
            U8 kind = kind_, ml = meta_len, pend = flags & EF_PENDING;
            *this = o;
            kind_ = kind;
            meta_len = ml;
            flags = (U8)((flags & ~EF_PENDING) | pend);
        }
    };
    static_assert(sizeof(Entry) == 40, "index entry layout");

    class Index
    {
    public:
        static constexpr U32 CHUNK = 256;                 // records per chunk
        static constexpr U32 MAX_RECORDS = 0xfffffeu;     // per shard (24-bit index)

        struct KV
        {
            Key    first;
            Entry& second;
            U32    pos;
        };
        class iterator
        {
        public:
            iterator() = default;
            iterator(Index* ix, U32 pos) : mIx(ix), mPos(pos) {}
            KV operator*() const { return mIx->kvAt(mPos); }
            struct Arrow
            {
                KV kv;
                KV* operator->() { return &kv; }
            };
            Arrow operator->() const { return Arrow{ mIx->kvAt(mPos) }; }
            iterator& operator++() { ++mPos; return *this; }
            bool operator==(const iterator& o) const { return mPos == o.mPos; }
            bool operator!=(const iterator& o) const { return mPos != o.mPos; }
            U32 pos() const { return mPos; }
        private:
            Index* mIx = nullptr;
            U32    mPos = 0;
        };

        Index() = default;
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        size_t size() const { return mSize; }
        bool   empty() const { return mSize == 0; }
        // One iterator type for const and non-const use (the shard's lock is what
        // guards the table; a const Shard only means "read under the lock").
        iterator begin() const { return iterator(const_cast<Index*>(this), 0); }
        iterator end() const { return iterator(const_cast<Index*>(this), mSize); }

        iterator find(const Key& k) const;
        const Entry* get(const Key& k) const;
        Entry* get(const Key& k) { return const_cast<Entry*>(static_cast<const Index*>(this)->get(k)); }
        size_t count(const Key& k) const { return get(k) ? 1 : 0; }
        // Find or insert a default entry. nullptr only when the shard is full
        // (MAX_RECORDS).
        Entry* insert(const Key& k, bool* inserted = nullptr);
        iterator erase(iterator it);
        void clear();
        // Size the lookup array for n records (no grow until past it).
        void reserve(size_t n);

        // Meta bytes of a record (nullptr when meta_len == 0).
        const U8* meta(const Key& k) const;
        const U8* metaAt(U32 pos) const;
        // Set (len > 0) or drop (len == 0) a record's meta; updates meta_len.
        void setMeta(const Key& k, const U8* bytes, U8 len);
        void setMetaAt(U32 pos, const U8* bytes, U8 len);

        KV kvAt(U32 pos) { Rec& r = rec(pos); return KV{ keyOf(r), r.e, pos }; }

        // RAM held by this table (capacity, not just use).
        U64 memoryBytes() const;
        // Testing: the lookup array is consistent with the records.
        bool checkConsistency() const;

    private:
        struct Rec
        {
            LLUUID id;
            U32    variant = 0;
            Entry  e;
        };
        static_assert(sizeof(Rec) == 60, "index record layout");

        Rec& rec(U32 i) { return mChunks[i / CHUNK][i % CHUNK]; }
        const Rec& rec(U32 i) const { return mChunks[i / CHUNK][i % CHUNK]; }
        static Key keyOf(const Rec& r) { return Key(r.id, (Kind)r.e.kind_, r.variant); }
        static U64 hashOf(const LLUUID& id, Kind kind, U32 variant);
        static U64 hashOf(const Key& k) { return hashOf(k.id, k.kind, k.variant); }
        U32 home(U64 h) const { return (U32)(((h >> 32) * (U64)mSlots.size()) >> 32); }
        U32 next(U32 s) const { return s + 1 == (U32)mSlots.size() ? 0 : s + 1; }
        static bool match(const Rec& r, const Key& k)
        {
            return r.variant == k.variant && r.e.kind_ == (U8)k.kind && r.id == k.id;
        }
        S64  findPos(const Key& k, U64 h) const;           // record index or -1
        U32  slotOfRecord(U32 pos) const;                    // lookup slot holding pos
        void rebuild(size_t capacity);
        void placeSlot(std::vector<U32>& slots, U64 h, U32 pos) const;
        void eraseSlot(U32 slot);

        // meta side table: record index -> pool slot (open addressing, power of two)
        S64  metaFind(U32 pos) const;                        // map slot or -1
        void metaErase(U32 pos);
        U32& metaInsert(U32 pos);                            // pool slot reference (new = ~0u)
        void metaGrow();

        std::vector<std::unique_ptr<Rec[]>> mChunks;
        U32 mSize = 0;
        std::vector<U32> mSlots;                             // 0 = empty
        struct MetaBlob { U8 b[32]; };
        std::vector<MetaBlob> mMetaPool;
        std::vector<U32> mMetaFree;
        struct MetaSlot { U32 pos = ~0u; U32 pool = 0; };
        std::vector<MetaSlot> mMetaMap;
        U32 mMetaCount = 0;
    };
}

#endif // LL_LLCACHESTORE_INDEX_H
