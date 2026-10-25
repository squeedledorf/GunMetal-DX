/**
 * @file llcachestore_format.h
 * @brief Internal: byte layout of segments and records (the on-disk contract
 *        documented at the top of llcachestore.h).
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

#ifndef LL_LLCACHESTORE_FORMAT_H
#define LL_LLCACHESTORE_FORMAT_H

#include "llcachestore.h"

#include <cstring>
#include <string>
#include <vector>

namespace LLCacheStoreFormat
{
    constexpr size_t SEG_HEADER_SIZE = 64;
    constexpr size_t REC_HEADER_SIZE = 96;
    constexpr size_t REC_ALIGN       = 16;
    constexpr size_t MAX_META        = 32;
    constexpr U8     REC_VERSION     = 1;
    constexpr U16    SEG_VERSION     = 1;

    constexpr U8 FLAG_TOMBSTONE  = 0x01;
    constexpr U8 FLAG_CLEAR_KIND = 0x02;
    constexpr U8 FLAG_SHARED     = 0x04;
    constexpr U8 FLAG_COMPACTED  = 0x08;
    constexpr U8 FLAGS_KNOWN     = 0x0f;

    constexpr U8 CODEC_NONE  = 0;   // payload = record bytes
    constexpr U8 CODEC_SHIM  = 1;   // payload = LLCacheCodec blob

    inline U64 align16(U64 v) { return (v + (REC_ALIGN - 1)) & ~(U64)(REC_ALIGN - 1); }
    inline U64 recordSize(U32 meta_len, U32 stored) { return align16(REC_HEADER_SIZE + (U64)meta_len + stored); }

    // ---- little-endian byte helpers
    inline void put16(U8* p, U16 v) { p[0] = (U8)v; p[1] = (U8)(v >> 8); }
    inline void put32(U8* p, U32 v) { for (int i = 0; i < 4; ++i) p[i] = (U8)(v >> (8 * i)); }
    inline void put64(U8* p, U64 v) { for (int i = 0; i < 8; ++i) p[i] = (U8)(v >> (8 * i)); }
    inline U16 get16(const U8* p) { return (U16)(p[0] | (p[1] << 8)); }
    inline U32 get32(const U8* p) { U32 v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
    inline U64 get64(const U8* p) { U64 v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

    // Growable little-endian writer / bounds-checked reader (snapshots, manifests).
    struct Writer
    {
        std::vector<U8> buf;
        void u8(U8 v) { buf.push_back(v); }
        void u16(U16 v) { size_t n = buf.size(); buf.resize(n + 2); put16(&buf[n], v); }
        void u32(U32 v) { size_t n = buf.size(); buf.resize(n + 4); put32(&buf[n], v); }
        void u64(U64 v) { size_t n = buf.size(); buf.resize(n + 8); put64(&buf[n], v); }
        void bytes(const void* p, size_t n) { buf.insert(buf.end(), (const U8*)p, (const U8*)p + n); }
        void uuid(const LLUUID& id) { bytes(id.mData, UUID_BYTES); }
    };
    struct Reader
    {
        const U8* p;
        size_t    n;
        size_t    pos = 0;
        bool      ok  = true;
        Reader(const U8* data, size_t len) : p(data), n(len) {}
        bool need(size_t k) { if (!ok || n - pos < k || pos > n) { ok = false; return false; } return true; }
        U8  u8()  { if (!need(1)) return 0; return p[pos++]; }
        U16 u16() { if (!need(2)) return 0; U16 v = get16(p + pos); pos += 2; return v; }
        U32 u32() { if (!need(4)) return 0; U32 v = get32(p + pos); pos += 4; return v; }
        U64 u64() { if (!need(8)) return 0; U64 v = get64(p + pos); pos += 8; return v; }
        void bytes(void* d, size_t k) { if (!need(k)) { memset(d, 0, k); return; } memcpy(d, p + pos, k); pos += k; }
        LLUUID uuid() { LLUUID id; bytes(id.mData, UUID_BYTES); return id; }
    };

    // ---- record header
    struct RecordHeader
    {
        U8     kind         = 0;
        U8     flags        = 0;
        U8     codec        = CODEC_NONE;
        LLUUID id;
        U32    variant      = 0;
        U32    kind_version = 0;
        U8     meta_len     = 0;
        U32    stored       = 0;
        U32    raw          = 0;
        U32    write_time   = 0;
        LLUUID region;
        U64    grid_handle  = 0;
        U64    payload_hash = 0;

        U64 totalSize() const { return recordSize(meta_len, stored); }
    };

    // Serialise header + meta into out (REC_HEADER_SIZE + meta_len bytes).
    void encodeRecordHeader(const RecordHeader& h, const U8* meta, U8* out);

    enum class HeaderCheck { OK, NOT_A_HEADER, BAD };
    // Parse the fixed 96 bytes (no hash check yet: the hash covers the meta,
    // which follows). NOT_A_HEADER = wrong magic.
    HeaderCheck parseRecordHeaderFixed(const U8* buf, RecordHeader& h);
    // Verify the header hash given the fixed bytes and the meta bytes.
    bool verifyRecordHeader(const U8* fixed, const U8* meta, U8 meta_len);

    // ---- segment header
    void encodeSegmentHeader(U32 seq, U32 created, U8* out);
    bool parseSegmentHeader(const U8* buf, U32& seq, U32& created);

    std::string segmentFileName(U32 seq);              // "seg-0000002a.dat"
    bool        parseSegmentFileName(const std::string& name, U32& seq);
}

#endif // LL_LLCACHESTORE_FORMAT_H
