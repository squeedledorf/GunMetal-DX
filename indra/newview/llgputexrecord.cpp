/**
 * @file llgputexrecord.cpp
 * @brief GPU-ready texture records in the segment store (#715, #865). See llgputexrecord.h.
 *
 * No viewer precompiled header (SKIP_PRECOMPILE_HEADERS): llcommon + llcachestore only,
 * so the unit tests link it without the viewer.
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

#include "llgputexrecord.h"

#include "hbxxh.h"
#include "llformat.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
    void put16(U8* p, U16 v) { p[0] = (U8)v; p[1] = (U8)(v >> 8); }
    void put32(U8* p, U32 v) { for (int i = 0; i < 4; ++i) p[i] = (U8)(v >> (8 * i)); }
    U16 get16(const U8* p) { return (U16)(p[0] | (p[1] << 8)); }
    U32 get32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }

    U32 side(U32 w, U32 h) { return std::max(std::max(w, 1u), std::max(h, 1u)); }
    U32 lvlDim(U32 d, U32 l) { return std::max(l < 32 ? d >> l : 0u, 1u); }

    LLCache::Key levelKeyLog2(const LLUUID& id, bool mask, U32 lg)
    {
        return LLCache::Key(id, LLCache::Kind::GPUTEX,
                            (mask ? LLGpuTexRecord::VARIANT_MASK : 0u) | LLGpuTexRecord::VARIANT_V2
                                | (lg << LLGpuTexRecord::LEVEL_SHIFT));
    }

    bool live(const LLCache::Store& store, const LLCache::Key& k, LLCache::RecordInfo& ri)
    {
        return store.lookup(k, &ri) && !ri.staged;
    }

    // The chain-wide encoding facts of a header vs a record (vk_format is not one: the
    // blocks do not depend on the sRGB / UNORM base format a capture happened to have).
    bool sameFacts(const LLGpuTexHeader& h, const LLGpuTexRecord::RecMeta& m)
    {
        return h.encoder_version == m.encoder_version && h.enc == m.enc && h.enc_level == m.enc_level
            && h.flags == m.flags && h.cov_cut == m.cov_cut;
    }

    void headerFromMeta(const LLGpuTexRecord::RecMeta& m, const LLUUID& id, LLGpuTexHeader& h)
    {
        h = LLGpuTexHeader();
        h.magic = LLGpuTexHeader::kMagic;
        h.version = LLGpuTexCache::FILE_VERSION;
        h.header_bytes = sizeof(LLGpuTexHeader);
        memcpy(h.id, id.mData, 16);
        h.encoder_version = m.encoder_version;
        h.vk_format = m.vk_format;
        h.enc = m.enc; h.enc_level = m.enc_level; h.comps = m.comps; h.flags = m.flags;
        h.w = m.w; h.h = m.h;
        h.full_w = m.full_w; h.full_h = m.full_h;
        h.levels = (U16)LLGpuTexRecord::mipCount(m.w, m.h);
        h.alpha_grad_pct = m.alpha_grad_pct; h.cov_cut = m.cov_cut;
        h.alpha_side = m.alpha_side;
    }

    bool resolveV1(const LLCache::Store& store, const LLUUID& id, bool mask, LLGpuTexRecord::Chain& c)
    {
        LLCache::RecordInfo ri;
        const LLCache::Key k = LLGpuTexRecord::wholeKey(id, mask);
        if (!live(store, k, ri)) return false;
        if (!LLGpuTexRecord::unpackMetaV1(ri.meta, ri.meta_len, id, k.variant, ri.raw_bytes, c.hdr)) return false;
        c.v1 = true;
        c.recs.clear();
        LLGpuTexRecord::Chain::Rec r;
        r.key = k; r.level = 0; r.levels = c.hdr.levels; r.raw_bytes = ri.raw_bytes;
        c.recs.push_back(r);
        c.tail_level = 0;
        c.last_access = ri.last_access;
        c.v1_write_time = ri.write_time;
        return true;
    }

    bool resolveV2(const LLCache::Store& store, const LLUUID& id, bool mask, LLGpuTexRecord::Chain& c)
    {
        using namespace LLGpuTexRecord;
        LLCache::RecordInfo ti;
        RecMeta tm;
        if (!live(store, tailKey(id, mask), ti) || !unpackMeta(ti.meta, ti.meta_len, tm) || tm.part != PART_TAIL)
            return false;
        if (((tm.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0) != mask) return false;
        if (!tm.w || !tm.h || side(tm.w, tm.h) > TAIL_PX || tm.levels != mipCount(tm.w, tm.h)) return false;
        const U64 tail_bytes = chainBytes(tm.vk_format, tm.w, tm.h, tm.levels);
        if (!tail_bytes || tail_bytes != ti.raw_bytes) return false;

        // The contiguous run of level records above the tail, smallest first.
        struct Up { LLCache::Key key; RecMeta m; U32 raw; };
        SmallList<Up> ups;
        U32 cw = tm.w, ch = tm.h;
        for (U32 lg = log2Floor(side(cw, ch)) + 1; lg <= MAX_SIDE_LOG2; ++lg)
        {
            const LLCache::Key k = levelKeyLog2(id, mask, lg);
            LLCache::RecordInfo ri;
            RecMeta m;
            if (!live(store, k, ri) || !unpackMeta(ri.meta, ri.meta_len, m)) break;
            if (m.part != PART_LEVEL || m.levels != 1 || !sameEncoding(m, tm)) break;
            if (lvlDim(m.w, 1) != cw || lvlDim(m.h, 1) != ch || log2Floor(side(m.w, m.h)) != lg) break;
            const U64 lb = levelBytes(m.vk_format, m.w, m.h);
            if (!lb || lb != ri.raw_bytes) break;
            ups.push_back({ k, m, ri.raw_bytes });
            cw = m.w; ch = m.h;
        }
        const RecMeta& top = ups.empty() ? tm : ups.back().m;
        headerFromMeta(top, id, c.hdr);
        const U32 n_up = (U32)ups.size();
        if (c.hdr.levels != n_up + tm.levels) return false;   // (halving above keeps this; belt and braces)
        c.v1 = false;
        c.recs.clear();
        U64 total = 0;
        for (U32 i = 0; i < n_up; ++i)
        {
            const Up& u = ups[n_up - 1 - i];
            Chain::Rec r;
            r.key = u.key; r.level = i; r.levels = 1; r.raw_bytes = u.raw;
            c.recs.push_back(r);
            total += u.raw;
        }
        Chain::Rec tr;
        tr.key = tailKey(id, mask); tr.level = n_up; tr.levels = tm.levels; tr.raw_bytes = ti.raw_bytes;
        c.recs.push_back(tr);
        total += ti.raw_bytes;
        c.tail_level = n_up;
        c.last_access = ti.last_access;
        c.v1_write_time = 0;
        c.hdr.payload_bytes = total;
        c.hdr.header_hash = headerHash(c.hdr);
        return true;
    }
}

namespace LLGpuTexRecord
{

U32 log2Floor(U32 v)
{
    U32 r = 0;
    while (v > 1) { v >>= 1; ++r; }
    return r;
}

Part partOf(U32 variant)
{
    if (variant & ~(VARIANT_MASK | VARIANT_V2 | LEVEL_FIELD)) return Part::UNKNOWN;   // role bits / high bits
    if (!(variant & VARIANT_V2)) return (variant & LEVEL_FIELD) ? Part::UNKNOWN : Part::WHOLE;
    const U32 lg = levelLog2(variant);
    if (lg == 0) return Part::TAIL;
    return lg > log2Floor(TAIL_PX) && lg <= MAX_SIDE_LOG2 ? Part::LEVEL : Part::UNKNOWN;
}

U32 mipCount(U32 w, U32 h)
{
    return 1u + log2Floor(side(w, h));
}

U64 levelBytes(U32 vk_format, U32 w, U32 h)
{
    w = std::max(w, 1u); h = std::max(h, 1u);
    const U64 blocks = (U64)((w + 3u) / 4u) * ((h + 3u) / 4u);
    switch (vk_format)
    {
    case 37: case 38: case 39: case 40: case 41: case 42: case 43:   // R8G8B8A8_*
        return (U64)w * h * 4u;
    case 131: case 132: case 133: case 134:                          // BC1_*
    case 139: case 140:                                              // BC4_*
        return blocks * 8u;
    case 135: case 136: case 137: case 138:                          // BC2_*, BC3_*
    case 141: case 142: case 143: case 144: case 145: case 146:      // BC5_*, BC6H_*, BC7_*
        return blocks * 16u;
    default:
        return 0;
    }
}

U64 chainBytes(U32 vk_format, U32 w, U32 h, U32 levels)
{
    U64 b = 0;
    for (U32 l = 0; l < levels; ++l)
    {
        const U64 n = levelBytes(vk_format, lvlDim(w, l), lvlDim(h, l));
        if (!n) return 0;
        b += n;
    }
    return b;
}

U64 headerHash(const LLGpuTexHeader& h)
{
    return HBXXH64::digest(&h, offsetof(LLGpuTexHeader, header_hash));
}

U8 packMetaV1(const LLGpuTexHeader& h, U8* out)
{
    out[0] = META_VERSION_V1;
    put32(out + 1, h.encoder_version);
    put32(out + 5, h.vk_format);
    out[9] = h.enc; out[10] = h.enc_level; out[11] = h.comps; out[12] = h.flags;
    put16(out + 13, h.w); put16(out + 15, h.h);
    put16(out + 17, h.full_w); put16(out + 19, h.full_h);
    put16(out + 21, h.levels);
    out[23] = h.alpha_grad_pct; out[24] = h.cov_cut;
    put16(out + 25, h.alpha_side);
    return META_BYTES_V1;
}

bool unpackMetaV1(const U8* meta, U8 meta_len, const LLUUID& id, U32 variant, U64 payload_bytes, LLGpuTexHeader& h)
{
    if (!meta || meta_len < META_BYTES_V1 || meta[0] != META_VERSION_V1) return false;
    h = LLGpuTexHeader();
    h.magic = LLGpuTexHeader::kMagic;
    h.version = LLGpuTexCache::FILE_VERSION;
    h.header_bytes = sizeof(LLGpuTexHeader);
    memcpy(h.id, id.mData, 16);
    h.encoder_version = get32(meta + 1);
    h.vk_format = get32(meta + 5);
    h.enc = meta[9]; h.enc_level = meta[10]; h.comps = meta[11]; h.flags = meta[12];
    h.w = get16(meta + 13); h.h = get16(meta + 15);
    h.full_w = get16(meta + 17); h.full_h = get16(meta + 19);
    h.levels = get16(meta + 21);
    h.alpha_grad_pct = meta[23]; h.cov_cut = meta[24];
    h.alpha_side = get16(meta + 25);
    h.payload_bytes = payload_bytes;
    h.payload_hash = 0;
    h.header_hash = headerHash(h);
    if (((h.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0) != isMaskVariant(variant)) return false;
    return h.w && h.h && payload_bytes > 0;
}

U8 packMeta(const RecMeta& m, U8* out)
{
    out[0] = META_VERSION;
    put32(out + 1, m.encoder_version);
    put32(out + 5, m.vk_format);
    out[9] = m.enc; out[10] = m.enc_level; out[11] = m.comps; out[12] = m.flags;
    put16(out + 13, m.w); put16(out + 15, m.h);
    put16(out + 17, m.full_w); put16(out + 19, m.full_h);
    out[21] = m.levels;
    out[22] = m.alpha_grad_pct; out[23] = m.cov_cut;
    put16(out + 24, m.alpha_side);
    out[26] = m.part;
    out[27] = m.top_log2;
    return META_BYTES;
}

bool unpackMeta(const U8* meta, U8 meta_len, RecMeta& m)
{
    if (!meta || meta_len < META_BYTES || meta[0] != META_VERSION) return false;
    m.encoder_version = get32(meta + 1);
    m.vk_format = get32(meta + 5);
    m.enc = meta[9]; m.enc_level = meta[10]; m.comps = meta[11]; m.flags = meta[12];
    m.w = get16(meta + 13); m.h = get16(meta + 15);
    m.full_w = get16(meta + 17); m.full_h = get16(meta + 19);
    m.levels = meta[21];
    m.alpha_grad_pct = meta[22]; m.cov_cut = meta[23];
    m.alpha_side = get16(meta + 24);
    m.part = meta[26];
    m.top_log2 = meta[27];
    return m.w && m.h && m.levels && (m.part == PART_TAIL || m.part == PART_LEVEL);
}

bool sameEncoding(const RecMeta& a, const RecMeta& b)
{
    return a.encoder_version == b.encoder_version && a.enc == b.enc && a.enc_level == b.enc_level
        && a.flags == b.flags && a.cov_cut == b.cov_cut && levelBytes(a.vk_format, 4, 4) == levelBytes(b.vk_format, 4, 4);
}

RecMeta metaFromHeader(const LLGpuTexHeader& h)
{
    RecMeta m;
    m.encoder_version = h.encoder_version;
    m.vk_format = h.vk_format;
    m.enc = h.enc; m.enc_level = h.enc_level; m.comps = h.comps; m.flags = h.flags;
    m.w = h.w; m.h = h.h;
    m.full_w = h.full_w; m.full_h = h.full_h;
    m.levels = (U8)std::min<U32>(h.levels, 255u);
    m.alpha_grad_pct = h.alpha_grad_pct; m.cov_cut = h.cov_cut;
    m.alpha_side = h.alpha_side;
    m.top_log2 = (U8)log2Floor(side(h.w, h.h));
    return m;
}

U64 Chain::bytes() const
{
    U64 n = 0;
    for (const Rec& r : recs) n += r.raw_bytes;
    return n;
}

bool resolve(const LLCache::Store& store, const LLUUID& id, bool mask, Chain& out, bool with_v1)
{
    Chain v2, v1;
    const bool have2 = resolveV2(store, id, mask, v2);
    const bool have1 = with_v1 && resolveV1(store, id, mask, v1);
    if (have2 && (!have1 || side(v2.hdr.w, v2.hdr.h) >= side(v1.hdr.w, v1.hdr.h)))
    {
        out = std::move(v2);
        return true;
    }
    if (have1)
    {
        out = std::move(v1);
        return true;
    }
    return false;
}

void infoFrom(const Chain& c, LLGpuTexCache::Info& out)
{
    out.w = c.hdr.w;
    out.h = c.hdr.h;
    out.enc = c.hdr.enc;
    out.enc_level = c.hdr.enc_level;
    out.encoder_version = c.hdr.encoder_version;
    out.full_w = c.hdr.full_w;
    out.full_h = c.hdr.full_h;
    out.bytes = c.bytes() + sizeof(LLGpuTexHeader);   // what a whole read holds in RAM (header + chain)
}

bool planRead(const Chain& c, U32 first_level, U32 resident_top, ReadPlan& out)
{
    out = ReadPlan();
    const U32 L = c.hdr.levels;
    if (!L || c.recs.empty()) return false;
    first_level = std::min(first_level, L - 1);
    U32 end = L;
    if (!c.v1 && resident_top)
    {
        end = first_level;
        while (end < L && side(lvlDim(c.hdr.w, end), lvlDim(c.hdr.h, end)) > resident_top) ++end;
    }
    if (end <= first_level) return false;
    for (size_t i = 0; i < c.recs.size(); ++i)
    {
        const Chain::Rec& r = c.recs[i];
        if (c.v1 || (r.level < end && r.level + r.levels > first_level))
        {
            out.recs.push_back(i);
            out.bytes += r.raw_bytes;
        }
    }
    if (out.recs.empty()) return false;
    out.first_level = c.recs[out.recs.front()].level;
    const Chain::Rec& last = c.recs[out.recs.back()];
    out.end_level = last.level + last.levels;
    return true;
}

bool assemble(const Chain& c, const ReadPlan& plan, std::vector<LLCache::ReadResult>& results, LLGpuTexCache::Blob& out,
              size_t& bad)
{
    bad = 0;
    if (results.size() != plan.recs.size()) return false;
    LLUUID id;
    memcpy(id.mData, c.hdr.id, 16);
    out = LLGpuTexCache::Blob();
    out.hdr = c.hdr;
    out.first_level = plan.first_level;
    out.pieces.reserve(plan.recs.size());
    for (size_t k = 0; k < plan.recs.size(); ++k)
    {
        bad = k;
        const Chain::Rec& r = c.recs[plan.recs[k]];
        LLCache::ReadResult& rr = results[k];
        if (rr.status != LLCache::Status::OK || rr.bytes.size() != (size_t)r.raw_bytes || rr.info.key != r.key) return false;
        if (c.v1)
        {
            LLGpuTexHeader h;
            if (!unpackMetaV1(rr.info.meta, rr.info.meta_len, id, r.key.variant, rr.bytes.size(), h)) return false;
            if (h.w != c.hdr.w || h.h != c.hdr.h || h.levels != c.hdr.levels || h.enc != c.hdr.enc
                || h.encoder_version != c.hdr.encoder_version)
                return false;
        }
        else
        {
            RecMeta m;
            if (!unpackMeta(rr.info.meta, rr.info.meta_len, m) || !sameFacts(c.hdr, m)) return false;
            if (m.w != lvlDim(c.hdr.w, r.level) || m.h != lvlDim(c.hdr.h, r.level) || m.levels != r.levels) return false;
        }
        LLGpuTexCache::Blob::Piece p;
        p.level = r.level;
        p.levels = r.levels;
        p.bytes = std::move(rr.bytes);
        out.pieces.push_back(std::move(p));
    }
    bad = 0;
    return true;
}

bool planWrite(const LLGpuTexHeader& hdr, U64 payload_bytes, std::vector<PartPlan>& out, U32 above_side)
{
    out.clear();
    const U32 w = hdr.w, h = hdr.h, L = hdr.levels;
    if (!w || !h || L != mipCount(w, h) || side(w, h) > (1u << MAX_SIDE_LOG2)) return false;
    const U64 total = chainBytes(hdr.vk_format, w, h, L);
    if (!total || total != payload_bytes) return false;
    LLUUID id;
    memcpy(id.mData, hdr.id, 16);
    const bool mask = (hdr.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0;
    const RecMeta base = metaFromHeader(hdr);
    U64 off = 0;
    for (U32 l = 0; l < L; ++l)
    {
        const U32 lw = lvlDim(w, l), lh = lvlDim(h, l), s = side(lw, lh);
        PartPlan p;
        RecMeta m = base;
        m.w = (U16)lw; m.h = (U16)lh;
        p.offset = off; p.w = lw; p.h = lh;
        if (s <= TAIL_PX)
        {
            // The store's tail is kept only when it is a complete one (256 px top); over a
            // smaller tail the new one is written (#991: writeMode asks FULL for that anyway).
            if (above_side >= TAIL_PX) break;
            m.levels = (U8)(L - l);
            m.part = PART_TAIL;
            p.key = tailKey(id, mask);
            p.bytes = payload_bytes - off;
            p.tail = true;
            p.meta_len = packMeta(m, p.meta);
            out.push_back(p);
            break;
        }
        const U64 n = levelBytes(hdr.vk_format, lw, lh);
        if (s > above_side)
        {
            m.levels = 1;
            m.part = PART_LEVEL;
            p.key = levelKey(id, mask, s);
            p.bytes = n;
            p.meta_len = packMeta(m, p.meta);
            out.push_back(p);
        }
        off += n;
    }
    return !out.empty();
}

WriteMode writeMode(const LLCache::Store& store, const LLGpuTexHeader& hdr, U32& above_side, U32* same_top)
{
    above_side = 0;
    if (same_top) *same_top = 0;
    LLUUID id;
    memcpy(id.mData, hdr.id, 16);
    const bool mask = (hdr.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0;
    Chain c;
    if (!resolve(store, id, mask, c)) return WriteMode::FULL;
    const bool same = sameFacts(c.hdr, metaFromHeader(hdr))
                   && levelBytes(c.hdr.vk_format, 4, 4) == levelBytes(hdr.vk_format, 4, 4);
    const U32 top_new = side(hdr.w, hdr.h), top_old = side(c.hdr.w, c.hdr.h);
    if (!same) return WriteMode::FULL;
    if (same_top) *same_top = top_old;
    if (top_new <= top_old) return WriteMode::SKIP;
    // #991: a chain whose top is inside the tail (captured at a higher discard, e.g. 64 x 64)
    // is upgraded by rewriting its tail -- the new capture's tail holds levels the old one
    // does not -- so the whole new chain is written (the tail key is replaced). ABOVE wrote
    // only the level records above 256 px: none for a capture <= 256 (refused as "not a
    // full mip chain", every time, every session), and for a larger one level records the
    // resolver could never reach from the old tail. A 256 tail is the same in both: ABOVE.
    if (c.v1 || top_old < TAIL_PX) return WriteMode::FULL;
    // The new chain must reach the store's top exactly (same dims), or it is another chain.
    for (U32 l = 0; l < hdr.levels; ++l)
    {
        const U32 lw = lvlDim(hdr.w, l), lh = lvlDim(hdr.h, l);
        if (side(lw, lh) != top_old) continue;
        if (lw != c.hdr.w || lh != c.hdr.h) break;
        above_side = top_old;
        return WriteMode::ABOVE;
    }
    return WriteMode::FULL;
}

PartWrite writeParts(LLCache::Store& store, const std::vector<PartPlan>& parts, const U8* src,
                     const LLCache::WriteOptions& base, const std::function<void()>& release_part,
                     const std::function<void(size_t, LLCache::WriteOptions&)>& per_part)
{
    PartWrite pw;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const PartPlan& p = parts[i];
        LLCache::WriteOptions o = base;
        if (per_part) per_part(i, o);
        o.meta = p.meta;
        o.meta_len = p.meta_len;
        LLCache::Ticket<LLCache::Status> t;
        const bool ok = store.write(p.key, LLCache::WriteSource::borrow(src + p.offset, (size_t)p.bytes, release_part), o, &t);
        ++pw.queued;   // handed over: its release runs once, accepted or not
        if (!ok)
        {
            const LLCache::Status s = t.isDone() ? t.take() : LLCache::Status::REFUSED_BUSY;
            pw.refused = s == LLCache::Status::OK ? LLCache::Status::REFUSED_BUSY : s;
            break;
        }
        pw.tickets.push_back(t);
    }
    return pw;
}

void removeStale(LLCache::Store& store, const LLUUID& id, bool mask, U32 top_side)
{
    const LLCache::Key wk = wholeKey(id, mask);
    if (store.exists(wk)) store.remove(wk);
    removeLevels(store, id, mask, top_side);
}

void removeLevels(LLCache::Store& store, const LLUUID& id, bool mask, U32 above_side)
{
    for (U32 lg = std::max(log2Floor(std::max(above_side, 1u)) + 1, log2Floor(TAIL_PX) + 1); lg <= MAX_SIDE_LOG2; ++lg)
    {
        const LLCache::Key k = levelKeyLog2(id, mask, lg);
        if (store.exists(k)) store.remove(k);
    }
}

void removeAll(LLCache::Store& store, const LLUUID& id, bool mask)
{
    const LLCache::Key tk = tailKey(id, mask);
    if (store.exists(tk)) store.remove(tk);
    removeStale(store, id, mask, 0);
}

void touchChain(LLCache::Store& store, const Chain& c, bool reuse)
{
    for (const Chain::Rec& r : c.recs)
    {
        if (reuse) store.noteReuse(r.key);
        store.touch(r.key);
    }
}

std::string describe(const LLCache::RecordInfo& r)
{
    if (r.key.kind != LLCache::Kind::GPUTEX) return std::string();
    const Part part = partOf(r.key.variant);
    if (part == Part::WHOLE)
    {
        if (r.meta_len < META_BYTES_V1 || r.meta[0] != META_VERSION_V1) return " v1 whole (bad meta)";
        return llformat(" v1 whole %ux%u levels %u e%u l%u v%u", (U32)get16(r.meta + 13), (U32)get16(r.meta + 15),
                        (U32)get16(r.meta + 21), (U32)r.meta[9], (U32)r.meta[10], get32(r.meta + 1));
    }
    if (part == Part::UNKNOWN) return " (unknown variant)";
    RecMeta m;
    if (!unpackMeta(r.meta, r.meta_len, m)) return " v2 (bad meta)";
    return llformat(" v2 %s %ux%u levels %u e%u l%u v%u (capture top %u)",
                    part == Part::TAIL ? "tail" : llformat("level %u", 1u << levelLog2(r.key.variant)).c_str(),
                    (U32)m.w, (U32)m.h, (U32)m.levels, (U32)m.enc, (U32)m.enc_level, m.encoder_version,
                    1u << std::min<U32>(m.top_log2, 31u));
}

bool holdsFullSizeTop(const LLCache::RecordInfo& r)
{
    if (r.key.kind != LLCache::Kind::GPUTEX) return false;
    U32 w = 0, h = 0, fw = 0, fh = 0;
    const Part part = partOf(r.key.variant);
    if (part == Part::WHOLE)
    {
        if (r.meta_len < META_BYTES_V1 || r.meta[0] != META_VERSION_V1) return false;
        w = get16(r.meta + 13); h = get16(r.meta + 15);
        fw = get16(r.meta + 17); fh = get16(r.meta + 19);
    }
    else if (part == Part::TAIL || part == Part::LEVEL)
    {
        RecMeta m;
        if (!unpackMeta(r.meta, r.meta_len, m)) return false;
        w = m.w; h = m.h; fw = m.full_w; fh = m.full_h;
    }
    else return false;
    if (!w || !h) return false;
    if (w >= 2048 || h >= 2048) return true;
    return fw && fh && w >= fw && h >= fh;
}

// ------------------------------------------------------------------ Rewriter

bool Rewriter::offer(const Chain& c, const std::shared_ptr<const LLGpuTexCache::Blob>& blob, bool mask)
{
    ++mStats.offered;
    if (!mStore || mStore->readOnly() || !c.v1 || !blob || blob->pieces.size() != 1 || c.recs.size() != 1) return false;
    LLUUID id;
    memcpy(id.mData, c.hdr.id, 16);
    for (const Job& j : mQueue) if (j.id == id && j.mask == mask) return false;
    for (const Job& j : mInFlight) if (j.id == id && j.mask == mask) return false;
    const U64 bytes = blob->bytes();
    if (mQueue.size() >= mLimits.max_queued || (mQueueBytes > 0 && mQueueBytes + bytes > mLimits.max_queued_bytes))
    {
        ++mStats.dropped_full;
        return false;
    }
    Job j;
    j.id = id;
    j.mask = mask;
    j.v1_write_time = c.v1_write_time;
    j.v1_raw = c.recs[0].raw_bytes;
    j.hdr = c.hdr;
    j.blob = blob;
    j.bytes = bytes;
    mQueueBytes += bytes;
    mQueue.push_back(std::move(j));
    ++mStats.queued;
    return true;
}

void Rewriter::cancel(const LLUUID& id, bool mask)
{
    for (auto it = mQueue.begin(); it != mQueue.end(); )
    {
        if (it->id == id && it->mask == mask)
        {
            mQueueBytes -= std::min(mQueueBytes, it->bytes);
            ++mStats.cancelled;
            it = mQueue.erase(it);
        }
        else ++it;
    }
    for (Job& j : mInFlight)
        if (j.id == id && j.mask == mask) j.cancelled = true;
}

void Rewriter::clear()
{
    mQueue.clear();
    mQueueBytes = 0;
    mInFlight.clear();   // (their tickets / bytes are owned by the store's write sources now)
    mInFlightBytes = 0;
}

bool Rewriter::v1Current(const Job& j) const
{
    LLCache::RecordInfo ri;
    return mStore && live(*mStore, wholeKey(j.id, j.mask), ri) && ri.write_time == j.v1_write_time
        && ri.raw_bytes == j.v1_raw;
}

void Rewriter::settle(Job& j)
{
    mInFlightBytes -= std::min(mInFlightBytes, j.bytes);
    bool ok = !j.tickets.empty();
    for (LLCache::Ticket<LLCache::Status>& t : j.tickets)
        if (t.take() != LLCache::Status::OK) ok = false;
    if (j.cancelled) { ++mStats.cancelled; return; }
    if (!ok) { ++mStats.failed; return; }   // the v1 record stays (lookups serve the larger chain)
    // Every v2 record is appended: the v1 record goes, but only while the v2 chain still
    // resolves at least as large (a level evicted meanwhile: keep v1, rewrite again later).
    Chain c;
    if (v1Current(j) && resolve(*mStore, j.id, j.mask, c) && !c.v1) mStore->remove(wholeKey(j.id, j.mask));
    ++mStats.rewritten;
    mStats.bytes_rewritten += j.bytes;
}

void Rewriter::pump(F64 now_sec)
{
    if (!mStore) return;
    for (auto it = mInFlight.begin(); it != mInFlight.end(); )
    {
        bool done = true;
        for (const LLCache::Ticket<LLCache::Status>& t : it->tickets) if (!t.isDone()) { done = false; break; }
        if (done)
        {
            settle(*it);
            it = mInFlight.erase(it);
        }
        else ++it;
    }
    // Token buckets (at most one second of allowance banked: no burst after an idle spell).
    const F64 rate = mLimits.bytes_per_sec ? (F64)mLimits.bytes_per_sec : 1e18;
    const F64 rps = mLimits.records_per_sec ? (F64)mLimits.records_per_sec : 1e9;
    const F64 dt = mLast < 0.0 ? 0.05 : std::clamp(now_sec - mLast, 0.0, 1.0);
    mLast = now_sec;
    mTokBytes = std::min(mTokBytes + dt * rate, rate);
    mTokRecs = std::min(mTokRecs + dt * rps, rps);
    if (mStore->readOnly()) return;
    while (!mQueue.empty() && mTokRecs >= 1.0 && mTokBytes > 0.0)
    {
        Job& front = mQueue.front();
        if (!mInFlight.empty() && mInFlightBytes + front.bytes > mLimits.max_in_flight_bytes) break;
        Job j = std::move(front);
        mQueue.pop_front();
        mQueueBytes -= std::min(mQueueBytes, j.bytes);
        mTokRecs -= 1.0;
        if (!v1Current(j)) { ++mStats.stale; continue; }   // evicted / removed / replaced since the read
        Chain c;
        if (resolve(*mStore, j.id, j.mask, c) && !c.v1)
        {
            // A v2 chain at least as large already exists (an interrupted rewrite, a new
            // capture): only the v1 record is left to drop.
            mStore->remove(wholeKey(j.id, j.mask));
            ++mStats.stale;
            continue;
        }
        std::vector<PartPlan> parts;
        if (!planWrite(j.hdr, j.bytes, parts))
        {
            ++mStats.failed;   // not a chain this layout can split (the renderer rejects it anyway)
            continue;
        }
        LLCache::WriteOptions o;
        o.priority = LLCache::Priority::LOW;
        o.placement = LLCache::PlacementHint::shared();
        std::shared_ptr<const LLGpuTexCache::Blob> keep = j.blob;
        PartWrite pw = writeParts(*mStore, parts, j.blob->pieces[0].bytes.data(), o, [keep]() {});
        mTokBytes -= (F64)j.bytes;
        if (pw.refused != LLCache::Status::OK)
        {
            // (the levels queued before the refusal are harmless without their tail and are
            // rewritten in place next time)
            ++mStats.failed;
            if (pw.refused == LLCache::Status::REFUSED_BUSY) break;
            continue;
        }
        j.tickets = std::move(pw.tickets);
        mInFlightBytes += j.bytes;
        mInFlight.push_back(std::move(j));
    }
}

Rewriter::Stats Rewriter::stats() const
{
    Stats s = mStats;
    s.queue = (U32)mQueue.size();
    s.in_flight = (U32)mInFlight.size();
    s.queue_bytes = mQueueBytes;
    return s;
}

} // namespace LLGpuTexRecord
