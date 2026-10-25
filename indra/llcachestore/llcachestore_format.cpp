/**
 * @file llcachestore_format.cpp
 * @brief Internal: byte layout of segments and records.
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

#include "llcachestore_format.h"

#include "hbxxh.h"

#include <cstdio>

namespace LLCacheStoreFormat
{

namespace
{
    const U8 REC_MAGIC[4] = { 'S', 'L', 'C', 'R' };
    const U8 SEG_MAGIC[4] = { 'S', 'L', 'C', 'S' };

    U64 headerHash(const U8* fixed, const U8* meta, U8 meta_len)
    {
        HBXXH64 h;
        h.update(fixed, 88);
        if (meta_len)
        {
            h.update(meta, meta_len);
        }
        return h.digest();
    }
}

void encodeRecordHeader(const RecordHeader& h, const U8* meta, U8* out)
{
    memset(out, 0, REC_HEADER_SIZE);
    memcpy(out, REC_MAGIC, 4);
    out[4] = REC_VERSION;
    out[5] = h.kind;
    out[6] = h.flags;
    out[7] = h.codec;
    memcpy(out + 8, h.id.mData, 16);
    put32(out + 24, h.variant);
    put32(out + 28, h.kind_version);
    out[32] = h.meta_len;
    put32(out + 36, h.stored);
    put32(out + 40, h.raw);
    put32(out + 44, h.write_time);
    memcpy(out + 48, h.region.mData, 16);
    put64(out + 64, h.grid_handle);
    put64(out + 72, h.payload_hash);
    if (h.meta_len)
    {
        memcpy(out + REC_HEADER_SIZE, meta, h.meta_len);
    }
    put64(out + 88, headerHash(out, meta, h.meta_len));
}

HeaderCheck parseRecordHeaderFixed(const U8* buf, RecordHeader& h)
{
    if (memcmp(buf, REC_MAGIC, 4) != 0)
    {
        return HeaderCheck::NOT_A_HEADER;
    }
    if (buf[4] != REC_VERSION || (buf[6] & ~FLAGS_KNOWN) || buf[7] > CODEC_SHIM ||
        buf[32] > MAX_META || buf[5] >= (U8)LLCache::Kind::COUNT)
    {
        return HeaderCheck::BAD;
    }
    h.kind = buf[5];
    h.flags = buf[6];
    h.codec = buf[7];
    memcpy(h.id.mData, buf + 8, 16);
    h.variant = get32(buf + 24);
    h.kind_version = get32(buf + 28);
    h.meta_len = buf[32];
    h.stored = get32(buf + 36);
    h.raw = get32(buf + 40);
    h.write_time = get32(buf + 44);
    memcpy(h.region.mData, buf + 48, 16);
    h.grid_handle = get64(buf + 64);
    h.payload_hash = get64(buf + 72);
    return HeaderCheck::OK;
}

bool verifyRecordHeader(const U8* fixed, const U8* meta, U8 meta_len)
{
    return get64(fixed + 88) == headerHash(fixed, meta, meta_len);
}

void encodeSegmentHeader(U32 seq, U32 created, U8* out)
{
    memset(out, 0, SEG_HEADER_SIZE);
    memcpy(out, SEG_MAGIC, 4);
    put16(out + 4, SEG_VERSION);
    put32(out + 8, seq);
    put32(out + 12, created);
    put64(out + 56, HBXXH64::digest(out, 56));
}

bool parseSegmentHeader(const U8* buf, U32& seq, U32& created)
{
    if (memcmp(buf, SEG_MAGIC, 4) != 0 || get16(buf + 4) != SEG_VERSION ||
        get64(buf + 56) != HBXXH64::digest(buf, 56))
    {
        return false;
    }
    seq = get32(buf + 8);
    created = get32(buf + 12);
    return true;
}

std::string segmentFileName(U32 seq)
{
    char name[32];
    snprintf(name, sizeof(name), "seg-%08x.dat", seq);
    return name;
}

bool parseSegmentFileName(const std::string& name, U32& seq)
{
    if (name.size() != 16 || name.compare(0, 4, "seg-") != 0 || name.compare(12, 4, ".dat") != 0)
    {
        return false;
    }
    U32 v = 0;
    for (size_t i = 4; i < 12; ++i)
    {
        char c = name[i];
        U32 d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else return false;
        v = (v << 4) | d;
    }
    if (v == 0)
    {
        return false;
    }
    seq = v;
    return true;
}

} // namespace LLCacheStoreFormat
