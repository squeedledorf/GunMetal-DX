/**
 * @file llcachestore_index.cpp
 * @brief Internal: the store's in-RAM index (#866). See llcachestore_index.h.
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

#include "llcachestore_index.h"

#include <cstring>

using namespace LLCacheStoreIndex;

namespace
{
    // Max load of the lookup array: 85% (17/20). A grow goes to 1.5x, i.e. ~57%.
    bool overLoad(size_t n, size_t cap) { return n * 20 > cap * 17; }
    size_t capacityFor(size_t n) { return std::max<size_t>(16, n * 20 / 17 + 1); }

    constexpr U32 TAG_SHIFT = 24;
    constexpr U32 POS_MASK = 0xffffffu;

    U32 tagOf(U64 h) { return (U32)(h >> 24) & 0xffu; }
    U32 slotPos(U32 s) { return (s & POS_MASK) - 1; }
}

// The shard was chosen by KeyHash % 64, so its low bits are constant here:
// remix so the tag (bits 24..31) and the home (bits 32..63) both vary.
U64 Index::hashOf(const LLUUID& id, Kind kind, U32 variant)
{
    U64 h = (U64)LLCache::KeyHash()(Key(id, kind, variant));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

S64 Index::findPos(const Key& k, U64 h) const
{
    if (mSlots.empty()) return -1;
    const U32 tag = tagOf(h);
    for (U32 s = home(h);; s = next(s))
    {
        const U32 v = mSlots[s];
        if (!v) return -1;
        if ((v >> TAG_SHIFT) == tag)
        {
            const U32 pos = slotPos(v);
            if (match(rec(pos), k)) return pos;
        }
    }
}

U32 Index::slotOfRecord(U32 pos) const
{
    const Rec& r = rec(pos);
    for (U32 s = home(hashOf(r.id, (Kind)r.e.kind_, r.variant));; s = next(s))
    {
        const U32 v = mSlots[s];
        if (v && slotPos(v) == pos) return s;
        llassert(v);
    }
}

Index::iterator Index::find(const Key& k) const
{
    S64 p = findPos(k, hashOf(k));
    return iterator(const_cast<Index*>(this), p < 0 ? mSize : (U32)p);
}

const Entry* Index::get(const Key& k) const
{
    S64 p = findPos(k, hashOf(k));
    return p < 0 ? nullptr : &rec((U32)p).e;
}

void Index::placeSlot(std::vector<U32>& slots, U64 h, U32 pos) const
{
    const U32 cap = (U32)slots.size();
    U32 s = (U32)(((h >> 32) * (U64)cap) >> 32);
    while (slots[s]) s = s + 1 == cap ? 0 : s + 1;
    slots[s] = (tagOf(h) << TAG_SHIFT) | (pos + 1);
}

void Index::rebuild(size_t capacity)
{
    std::vector<U32> slots(capacity, 0);
    for (U32 i = 0; i < mSize; ++i)
    {
        const Rec& r = rec(i);
        placeSlot(slots, hashOf(r.id, (Kind)r.e.kind_, r.variant), i);
    }
    mSlots.swap(slots);
}

void Index::reserve(size_t n)
{
    n = std::min<size_t>(n, MAX_RECORDS);
    if (overLoad(n, mSlots.size())) rebuild(capacityFor(n));
    mChunks.reserve((n + CHUNK - 1) / CHUNK);
}

Entry* Index::insert(const Key& k, bool* inserted)
{
    const U64 h = hashOf(k);
    S64 p = findPos(k, h);
    if (inserted) *inserted = p < 0;
    if (p >= 0) return &rec((U32)p).e;
    if (mSize >= MAX_RECORDS) return nullptr;
    if (overLoad(mSize + 1, mSlots.size()))
    {
        rebuild(std::max(capacityFor(mSize + 1), mSlots.size() + mSlots.size() / 2));
    }
    const U32 pos = mSize;
    if (pos / CHUNK >= mChunks.size()) mChunks.emplace_back(new Rec[CHUNK]);
    Rec& r = rec(pos);
    r = Rec();
    r.id = k.id;
    r.variant = k.variant;
    r.e.kind_ = (U8)k.kind;
    ++mSize;
    placeSlot(mSlots, h, pos);
    return &r.e;
}

// Backward-shift deletion: pull later members of the probe run into the hole
// when the hole lies on their path from home.
void Index::eraseSlot(U32 hole)
{
    const U32 cap = (U32)mSlots.size();
    U32 j = hole;
    for (;;)
    {
        j = next(j);
        const U32 v = mSlots[j];
        if (!v) break;
        const Rec& r = rec(slotPos(v));
        const U32 h = home(hashOf(r.id, (Kind)r.e.kind_, r.variant));
        // distance home -> j  >=  distance hole -> j: the hole is on its path
        const U32 dj = (j + cap - h) % cap;
        const U32 dh = (j + cap - hole) % cap;
        if (dj >= dh)
        {
            mSlots[hole] = v;
            hole = j;
        }
    }
    mSlots[hole] = 0;
}

Index::iterator Index::erase(iterator it)
{
    const U32 pos = it.pos();
    if (pos >= mSize) return end();
    eraseSlot(slotOfRecord(pos));
    if (rec(pos).e.meta_len) metaErase(pos);
    const U32 last = mSize - 1;
    if (pos != last)
    {
        const U32 s = slotOfRecord(last);
        mSlots[s] = (mSlots[s] & ~POS_MASK) | (pos + 1);
        if (rec(last).e.meta_len)
        {
            // Re-key: the pool slot goes to the free list and straight back (the
            // insert takes the most recently freed slot), so the bytes stay put.
            metaErase(last);
            metaInsert(pos);
        }
        rec(pos) = rec(last);
    }
    --mSize;
    // Free emptied chunks, keeping one spare against insert / erase churn at a boundary.
    while (mChunks.size() > (size_t)(mSize + CHUNK - 1) / CHUNK + 1) mChunks.pop_back();
    if (!mSize)
    {
        clear();
    }
    return iterator(this, pos);
}

void Index::clear()
{
    mChunks.clear();
    mChunks.shrink_to_fit();
    mSize = 0;
    std::vector<U32>().swap(mSlots);
    std::vector<MetaBlob>().swap(mMetaPool);
    std::vector<U32>().swap(mMetaFree);
    std::vector<MetaSlot>().swap(mMetaMap);
    mMetaCount = 0;
}

// ---------------------------------------------------------------- meta

namespace
{
    U32 metaHome(U32 pos, size_t cap) { return (U32)(((U64)(pos * 0x9E3779B1u) * (U64)cap) >> 32); }
}

S64 Index::metaFind(U32 pos) const
{
    const size_t cap = mMetaMap.size();
    if (!cap) return -1;
    for (U32 s = metaHome(pos, cap);; s = (U32)((s + 1) % cap))
    {
        if (mMetaMap[s].pos == pos) return s;
        if (mMetaMap[s].pos == ~0u) return -1;
    }
}

void Index::metaGrow()
{
    std::vector<MetaSlot> old;
    old.swap(mMetaMap);
    mMetaMap.assign(std::max<size_t>(16, old.size() * 2), MetaSlot());
    const size_t cap = mMetaMap.size();
    for (const MetaSlot& m : old)
    {
        if (m.pos == ~0u) continue;
        U32 s = metaHome(m.pos, cap);
        while (mMetaMap[s].pos != ~0u) s = (U32)((s + 1) % cap);
        mMetaMap[s] = m;
    }
}

U32& Index::metaInsert(U32 pos)
{
    S64 f = metaFind(pos);
    if (f >= 0) return mMetaMap[(size_t)f].pool;
    if ((mMetaCount + 1) * 4 > mMetaMap.size() * 3) metaGrow();
    const size_t cap = mMetaMap.size();
    U32 s = metaHome(pos, cap);
    while (mMetaMap[s].pos != ~0u) s = (U32)((s + 1) % cap);
    mMetaMap[s].pos = pos;
    if (!mMetaFree.empty())
    {
        mMetaMap[s].pool = mMetaFree.back();
        mMetaFree.pop_back();
    }
    else
    {
        mMetaMap[s].pool = (U32)mMetaPool.size();
        mMetaPool.emplace_back();
    }
    ++mMetaCount;
    return mMetaMap[s].pool;
}

void Index::metaErase(U32 pos)
{
    S64 f = metaFind(pos);
    if (f < 0) return;
    const size_t cap = mMetaMap.size();
    U32 hole = (U32)f;
    mMetaFree.push_back(mMetaMap[hole].pool);
    U32 j = hole;
    for (;;)
    {
        j = (U32)((j + 1) % cap);
        const MetaSlot m = mMetaMap[j];
        if (m.pos == ~0u) break;
        const U32 h = metaHome(m.pos, cap);
        if ((j + cap - h) % cap >= (j + cap - hole) % cap)
        {
            mMetaMap[hole] = m;
            hole = j;
        }
    }
    mMetaMap[hole] = MetaSlot();
    --mMetaCount;
}

const U8* Index::metaAt(U32 pos) const
{
    if (pos >= mSize || !rec(pos).e.meta_len) return nullptr;
    S64 f = metaFind(pos);
    return f < 0 ? nullptr : mMetaPool[mMetaMap[(size_t)f].pool].b;
}

const U8* Index::meta(const Key& k) const
{
    S64 p = findPos(k, hashOf(k));
    return p < 0 ? nullptr : metaAt((U32)p);
}

void Index::setMetaAt(U32 pos, const U8* bytes, U8 len)
{
    if (pos >= mSize) return;
    Entry& e = rec(pos).e;
    len = std::min<U8>(len, 32);
    if (!len || !bytes)
    {
        if (e.meta_len) metaErase(pos);
        e.meta_len = 0;
        return;
    }
    U32 pool = metaInsert(pos);
    memcpy(mMetaPool[pool].b, bytes, len);
    e.meta_len = len;
}

void Index::setMeta(const Key& k, const U8* bytes, U8 len)
{
    S64 p = findPos(k, hashOf(k));
    if (p >= 0) setMetaAt((U32)p, bytes, len);
}

// ---------------------------------------------------------------- accounting

U64 Index::memoryBytes() const
{
    return (U64)mChunks.size() * CHUNK * sizeof(Rec) + (U64)mChunks.capacity() * sizeof(void*) +
           (U64)mSlots.capacity() * sizeof(U32) + (U64)mMetaPool.capacity() * sizeof(MetaBlob) +
           (U64)mMetaFree.capacity() * sizeof(U32) + (U64)mMetaMap.capacity() * sizeof(MetaSlot);
}

bool Index::checkConsistency() const
{
    size_t used = 0;
    for (U32 v : mSlots)
        if (v) ++used;
    if (used != mSize) return false;
    U32 metas = 0;
    for (U32 i = 0; i < mSize; ++i)
    {
        const Rec& r = rec(i);
        if (findPos(keyOf(r), hashOf(keyOf(r))) != (S64)i) return false;
        if (r.e.meta_len)
        {
            ++metas;
            if (metaFind(i) < 0) return false;
        }
    }
    return metas == mMetaCount;
}
