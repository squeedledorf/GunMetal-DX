/**
 * @file llj2crecord.h
 * @brief J2C texture codestream records in the segment store (#715, #708): key and
 *        metadata layout shared by the store backend of LLTextureCache and the legacy
 *        texture cache migration (#714). Header only, llcommon + llcachestore.
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

#ifndef LL_LLJ2CRECORD_H
#define LL_LLJ2CRECORD_H

#include "llcachestore.h"

// RECORD LAYOUT (kind J2C, codec 0: stored as is so a read at an offset is one pread):
//   key     = (texture UUID, J2C, variant 0)
//   payload = a PREFIX of the texture's J2C codestream, from byte 0 (more bytes = a lower
//             discard level). Unlike the legacy cache there is no 600-byte header stripe
//             and no zero padding: the record holds exactly the bytes the fetcher wrote.
//   meta    = 5 bytes, little-endian:
//               0 u8  meta version (1)
//               1 s32 image size: the full asset size as the fetcher passed it to
//                     writeToCache(). The fetcher writes datasize + 1 to flag "not
//                     complete" (lltexturefetch.cpp, WRITE_TO_CACHE); that value round-trips
//                     unchanged so a read hands back the same mFileSize as before.
// PREFIX GROWTH: a longer prefix is written as a whole new record that supersedes the
// shorter one (never an append-continuation). See LLTextureCache's store backend notes.
namespace LLJ2CRecord
{
    constexpr U8 META_VERSION = 1;
    constexpr U8 META_BYTES = 5;
    constexpr U32 RECORD_HEADER_BYTES = 96;   // llcachestore.h on-disk contract

    inline LLCache::Key makeKey(const LLUUID& id)
    {
        return LLCache::Key(id, LLCache::Kind::J2C, 0);
    }

    // out must hold META_BYTES. Returns the meta length.
    inline U8 packMeta(S32 image_size, U8* out)
    {
        const U32 v = (U32)image_size;
        out[0] = META_VERSION;
        for (int i = 0; i < 4; ++i) out[1 + i] = (U8)(v >> (8 * i));
        return META_BYTES;
    }

    inline bool unpackMeta(const U8* meta, U8 meta_len, S32& image_size)
    {
        if (!meta || meta_len < META_BYTES || meta[0] != META_VERSION) return false;
        const U32 v = (U32)meta[1] | ((U32)meta[2] << 8) | ((U32)meta[3] << 16) | ((U32)meta[4] << 24);
        image_size = (S32)v;
        return true;
    }

    inline bool imageSizeFromRecord(const LLCache::RecordInfo& r, S32& image_size)
    {
        return unpackMeta(r.meta, r.meta_len, image_size);
    }

    // Bytes one record of `payload` bytes takes in a segment (codec 0): header + meta +
    // payload, padded to 16.
    inline U64 diskBytes(U64 payload)
    {
        return (RECORD_HEADER_BYTES + META_BYTES + payload + 15) & ~15ull;
    }
}

#endif // LL_LLJ2CRECORD_H
