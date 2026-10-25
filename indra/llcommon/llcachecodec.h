/**
 * @file llcachecodec.h
 * @brief Cache record codec shim: bytes in, bytes out, optional zstd
 *        compression behind a small self-describing header (#713, #715).
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

#ifndef LL_LLCACHECODEC_H
#define LL_LLCACHECODEC_H

#include "llpreprocessor.h"
#include "stdtypes.h"

#include <cstddef>
#include <string>
#include <vector>

/*
 * ON-DISK CONTRACT -- cache record header, format version 1
 * =========================================================
 *
 * The shim sits between any cache's save/load and any storage backend. It
 * never touches files or streams: encode() turns a record's bytes into
 * header + payload, decode() turns them back. Storage backends keep the
 * encoded bytes as an opaque blob.
 *
 * Every encoded record starts with this fixed 24-byte header. All multi-byte
 * fields are little-endian and written byte by byte (no struct memcpy), so
 * the layout is identical on every platform and compiler.
 *
 *   offset size field
 *   ------ ---- -----------------------------------------------------------
 *        0    4 magic       'S' 'L' 'C' 'Z'  (0x53 0x4C 0x43 0x5A).
 *                           Distinct from the zstd frame magic
 *                           (28 B5 2F FD) and from J2C / KTX / PNG magics.
 *        4    1 version     Format version. This document is version 1.
 *                           A reader rejects versions newer than it knows.
 *        5    1 codec       0 = raw (payload is the record, uncompressed)
 *                           1 = zstd (payload is exactly one zstd frame
 *                               with the content size field set; no zstd
 *                               frame checksum -- see checksum below)
 *                           Anything else is rejected.
 *        6    1 kind        ECacheRecordKind of the writer. Informational
 *                           (inspect / dump tools, stats); decode does not
 *                           require it to match the caller's kind. Values
 *                           a reader does not know are accepted.
 *        7    1 flags       bit 0 (0x01): checksum present. REQUIRED in
 *                           version 1 (writers always set it, readers reject
 *                           a record without it), so a single flipped bit
 *                           cannot switch verification off; the bit exists so
 *                           a later version may make the checksum optional.
 *                           Bits 1-7 are reserved and must be 0 in version
 *                           1; a version 1 reader rejects a record with any
 *                           reserved bit set (a new optional feature needs a
 *                           new version).
 *        8    8 size        Uncompressed record size in bytes (u64).
 *                           For codec 0 the payload length must equal it;
 *                           for codec 1 the zstd frame's content size must
 *                           equal it, and so must the decoded length.
 *                           Readers reject sizes above their record cap
 *                           (getMaxRecordSize(), default 256 MiB) before
 *                           allocating anything (allocation bombs).
 *       16    8 checksum    XXH3-64 (seed 0) of the UNCOMPRESSED record
 *                           bytes, i.e. HBXXH64::digest() -- note XXH3-64,
 *                           not the older XXH64. Covers raw and zstd records
 *                           alike and is always verified on decode. With
 *                           it, every header field that matters is checked:
 *                           magic / version / codec / flags directly, size
 *                           against the payload or frame, the checksum field
 *                           against the data. Only the informational kind
 *                           byte is unprotected (harmless if wrong).
 *       24    - payload     Raw record bytes (codec 0) or one zstd frame
 *                           (codec 1), running to the end of the encoded
 *                           blob. There is no payload length field: the
 *                           blob length comes from the storage backend.
 *
 * Legacy records: input that does not begin with the 4-byte magic
 * (including anything shorter than 4 bytes) is a record written before the
 * shim existed. decode() passes it through unchanged (RecordInfo::legacy =
 * true), so existing caches keep working when compression is switched on
 * (unless the kind's Policy::allow_legacy is off: then it is rejected).
 * Input that begins with the magic but is shorter than 24 bytes is a
 * truncated record and is rejected. A legacy record that happens to begin
 * with "SLCZ" is parsed as a header and, failing validation, rejected -- an
 * accepted, astronomically unlikely loss of one cache entry (a cache drops
 * bad records individually, never the whole cache).
 *
 * Evolving the format: bump the version for any layout or semantic change;
 * readers keep decoding every older version they shipped with. Adding a
 * codec id is a new version as well (older readers reject unknown codecs
 * anyway, but the version records intent).
 *
 * Encoding policy: per ECacheRecordKind (enabled, zstd level, minimum
 * saving; plus allow_legacy on the decode side, see Policy). A record is stored as zstd only when compression is
 * enabled for its kind and the zstd payload is at least min_saving_pct
 * percent smaller than the raw record; otherwise it is stored raw (codec 0),
 * so readers never pay decompression for incompressible data such as J2C.
 *
 * Threading: every function is safe to call concurrently from any thread.
 * Each thread lazily creates its own zstd compression / decompression
 * context (thread_local, freed when the thread exits or on
 * releaseThreadContexts()), so encode/decode allocate nothing per call
 * beyond the output buffer.
 */

// What a record holds. Selects the encoding policy and the stats bucket, and
// is written into the header for inspection tools. Values are part of the
// on-disk contract: append only, never renumber.
enum class ECacheRecordKind : U8
{
    GENERIC     = 0,    // anything without its own policy
    OBJECT      = 1,    // per-region object cache (#713)
    TEXTURE_GPU = 2,    // GPU-ready (transcoded / uncompressed) textures (#710)
    TEXTURE_J2C = 3,    // J2C as received from the server (already compressed)
    ASSET       = 4,    // other assets (meshes, sounds, animations, ...)
    COUNT
};

namespace LLCacheCodec
{
    constexpr size_t HEADER_SIZE    = 24;
    constexpr U8     FORMAT_VERSION = 1;

    enum ECodec : U8
    {
        CODEC_RAW  = 0,
        CODEC_ZSTD = 1,
    };

    enum EFlags : U8
    {
        FLAG_CHECKSUM      = 0x01,
        FLAGS_KNOWN_V1     = FLAG_CHECKSUM,
    };

    // Default record size cap (declared uncompressed size): 256 MiB.
    constexpr U64 DEFAULT_MAX_RECORD_SIZE = 256ull * 1024ull * 1024ull;

    // ---------------------------------------------------------------- policy

    struct Policy
    {
        bool enabled        = true;  // try zstd for this kind
        S32  level          = 0;     // zstd level; 0 = auto (defaultLevel())
        U32  min_saving_pct = 5;     // keep zstd only if >= this % smaller
        // Decode passes header-less input through as a legacy record. Turn
        // off for kinds whose storage never held pre-shim records or whose
        // data has no inner format to catch a mistake: a record whose magic
        // got corrupted is otherwise handed back verbatim (header and zstd
        // frame bytes) as if it were the data. Harmless where the record has
        // its own validated header (object cache); garbage for raw pixels.
        bool allow_legacy   = true;
    };

    // Auto zstd level: 3, or 1 on machines with <= 4 hardware threads.
    LL_COMMON_API S32 defaultLevel();

    // Levels are clamped to [MIN_LEVEL, MAX_LEVEL] (negative = zstd "fast"
    // levels; above 19 needs far more memory per context).
    constexpr S32 MIN_LEVEL = -7;
    constexpr S32 MAX_LEVEL = 19;

    // Built-in defaults: enabled with auto level for every kind except
    // TEXTURE_J2C (already compressed; disabled). Newview overrides these
    // from saved settings at startup and on setting change -- llcommon does
    // not read settings itself. Safe to call while other threads encode;
    // each encode uses a consistent snapshot of the fields it reads.
    LL_COMMON_API void   setPolicy(ECacheRecordKind kind, const Policy& policy);
    LL_COMMON_API Policy getPolicy(ECacheRecordKind kind);
    LL_COMMON_API void   setEnabled(ECacheRecordKind kind, bool enabled);
    LL_COMMON_API void   setLevel(ECacheRecordKind kind, S32 level); // 0 = auto

    // Cap on a record's declared uncompressed size, for decode (reject) and
    // encode (refuse to write what could not be read back).
    LL_COMMON_API void setMaxRecordSize(U64 bytes);
    LL_COMMON_API U64  getMaxRecordSize();

    // ------------------------------------------------------------- records

    struct RecordInfo
    {
        bool             legacy            = false; // no header: raw passthrough
        U8               version           = 0;
        U8               codec             = CODEC_RAW;
        U8               kind              = 0;     // raw header byte
        U8               flags             = 0;
        U64              uncompressed_size = 0;
        U64              checksum          = 0;
        size_t           payload_size      = 0;     // bytes after the header
    };

    // Parse and validate the header only (no decompression, no checksum).
    // Legacy input returns true with info.legacy set and uncompressed_size =
    // len. Returns false for a header that is present but invalid (bad
    // version, codec, flags, size over the cap, raw size mismatch, or a zstd
    // frame whose content size disagrees). Use it to size a caller buffer
    // for the zero-copy decode below.
    LL_COMMON_API bool peek(const U8* src, size_t len, RecordInfo& info);

    // Worst-case encoded size for a record of len bytes: HEADER_SIZE + len.
    // (zstd output is only kept when smaller than the record, so the raw
    // fallback is always the upper bound.)
    constexpr size_t encodeBound(size_t len) { return HEADER_SIZE + len; }

    // Encode one record. Always writes a header (codec 0 when compression
    // is disabled for the kind or does not save enough). Returns false only
    // when the record exceeds the record cap, the caller buffer is smaller
    // than encodeBound(len), or src is null with len > 0; never throws.
    // src and dst must not overlap.
    LL_COMMON_API bool encode(ECacheRecordKind kind, const U8* src, size_t len,
                              std::vector<U8>& out);
    LL_COMMON_API bool encode(ECacheRecordKind kind, const U8* src, size_t len,
                              U8* dst, size_t dst_cap, size_t& written);

    // Decode one record (header + payload, or legacy raw bytes). kind is the
    // caller's record kind, used for stats only. Returns false, leaving the
    // output unspecified, for any corrupt / truncated / oversized record or a
    // checksum mismatch; never crashes or throws on bad input. The zero-copy
    // overload decodes straight into dst (use peek() for the size) and fails
    // if dst_cap is too small. src and dst must not overlap.
    LL_COMMON_API bool decode(ECacheRecordKind kind, const U8* src, size_t len,
                              std::vector<U8>& out, RecordInfo* info = nullptr);
    LL_COMMON_API bool decode(ECacheRecordKind kind, const U8* src, size_t len,
                              U8* dst, size_t dst_cap, size_t& written,
                              RecordInfo* info = nullptr);

    // Same as encode() / decode(), but with an explicit policy instead of the
    // global per-kind one (only enabled / level / min_saving_pct are used to
    // encode, only allow_legacy to decode). For storage backends that own
    // their per-kind policy, e.g. the segment store (llcachestore), so a
    // global setting can never change what they write or accept.
    LL_COMMON_API bool encodeWith(ECacheRecordKind kind, const Policy& policy,
                                  const U8* src, size_t len, std::vector<U8>& out);
    LL_COMMON_API bool decodeWith(ECacheRecordKind kind, const Policy& policy,
                                  const U8* src, size_t len, std::vector<U8>& out,
                                  RecordInfo* info = nullptr);
    LL_COMMON_API bool decodeWith(ECacheRecordKind kind, const Policy& policy,
                                  const U8* src, size_t len, U8* dst, size_t dst_cap,
                                  size_t& written, RecordInfo* info = nullptr);

    // --------------------------------------------------------------- stats

    struct KindStats
    {
        U64 encodes          = 0;  // successful encode calls
        U64 encoded_raw      = 0;  //   ... stored with codec 0
        U64 encoded_zstd     = 0;  //   ... stored with codec 1
        U64 encode_failures  = 0;  // refused (cap, buffer, bad args)
        U64 encode_bytes_in  = 0;  // record bytes handed to encode
        U64 encode_bytes_out = 0;  // encoded bytes incl. headers
        U64 encode_ns        = 0;  // wall time inside encode
        U64 decodes          = 0;  // successful decode calls
        U64 decoded_legacy   = 0;  //   ... of which legacy passthrough
        U64 decode_failures  = 0;  // rejected records
        U64 decode_bytes_in  = 0;  // encoded bytes handed to decode
        U64 decode_bytes_out = 0;  // record bytes produced
        U64 decode_ns        = 0;  // wall time inside decode

        // Net bytes saved by encoding (negative: header overhead on raw).
        S64 bytesSaved() const
        {
            return (S64)encode_bytes_in - (S64)encode_bytes_out;
        }
    };

    LL_COMMON_API KindStats   getStats(ECacheRecordKind kind);
    LL_COMMON_API void        resetStats();
    LL_COMMON_API std::string formatStats();  // multi-line, kinds with activity
    LL_COMMON_API void        logStats();     // formatStats() to the log

    // Live zstd contexts across all threads (each holds a few MB at most).
    LL_COMMON_API U32 liveContexts();
    // Free the calling thread's zstd contexts now (e.g. a worker going idle).
    LL_COMMON_API void releaseThreadContexts();

    LL_COMMON_API const char* kindName(ECacheRecordKind kind);

    // ---------------------------------------------------------- self test

    // Round trips and corruption cases (compressible, incompressible, empty,
    // legacy passthrough, truncation, bad version / codec / flags, oversize,
    // checksum mismatch, missing checksum flag, zero-copy buffers). Uses explicit policies, so it
    // does not touch the global policy; its records do count in the GENERIC
    // stats. Returns false and fills failure (if given) on the first failed
    // check. Cheap (a few MB of work); fine for a Develop menu item.
    LL_COMMON_API bool selfTest(std::string* failure = nullptr);
}

#endif // LL_LLCACHECODEC_H
