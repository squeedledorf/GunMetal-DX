/**
 * @file llcachecodec.cpp
 * @brief Cache record codec shim (see llcachecodec.h for the on-disk
 *        contract).
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

#include "llcachecodec.h"

#include "hbxxh.h"

// Stable zstd API only (no ZSTD_STATIC_LINKING_ONLY). The implementation is
// compiled in llzstd.cpp, with the same visibility overrides (hidden symbols).
#define ZSTDLIB_VISIBLE
#define ZSTDERRORLIB_VISIBLE
#include "third_party/zstd/zstd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <thread>

using namespace LLCacheCodec;

namespace
{
    constexpr U8     MAGIC[4]   = { 'S', 'L', 'C', 'Z' };
    constexpr size_t KIND_COUNT = (size_t)ECacheRecordKind::COUNT;

    size_t kindIndex(ECacheRecordKind kind)
    {
        size_t i = (size_t)kind;
        return i < KIND_COUNT ? i : 0;
    }

    S32 computeDefaultLevel()
    {
        // 0 = unknown: assume a normal desktop.
        unsigned threads = std::thread::hardware_concurrency();
        return (threads != 0 && threads <= 4) ? 1 : 3;
    }
    const S32 sDefaultLevel = computeDefaultLevel();

    // ------------------------------------------------------------- policy
    // Atomics with constexpr constructors: constant-initialized, so usable
    // from any static initializer and from any thread.

    struct PolicySlot
    {
        std::atomic<bool> enabled;
        std::atomic<S32>  level;
        std::atomic<U32>  min_saving_pct;
        std::atomic<bool> allow_legacy;
    };

    PolicySlot sPolicy[KIND_COUNT] = {
        { {true},  {0}, {5}, {true} },  // GENERIC
        { {true},  {0}, {5}, {true} },  // OBJECT
        { {true},  {0}, {5}, {true} },  // TEXTURE_GPU
        { {false}, {0}, {5}, {true} },  // TEXTURE_J2C: already compressed
        { {true},  {0}, {5}, {true} },  // ASSET
    };
    static_assert(KIND_COUNT == 5, "add a policy row for the new kind");

    std::atomic<U64> sMaxRecordSize{ DEFAULT_MAX_RECORD_SIZE };

    S32 resolveLevel(S32 level)
    {
        if (level == 0)
        {
            level = sDefaultLevel;
        }
        return std::clamp(level, MIN_LEVEL, MAX_LEVEL);
    }

    // -------------------------------------------------------------- stats

    struct StatSlot
    {
        std::atomic<U64> encodes{0};
        std::atomic<U64> encoded_raw{0};
        std::atomic<U64> encoded_zstd{0};
        std::atomic<U64> encode_failures{0};
        std::atomic<U64> encode_bytes_in{0};
        std::atomic<U64> encode_bytes_out{0};
        std::atomic<U64> encode_ns{0};
        std::atomic<U64> decodes{0};
        std::atomic<U64> decoded_legacy{0};
        std::atomic<U64> decode_failures{0};
        std::atomic<U64> decode_bytes_in{0};
        std::atomic<U64> decode_bytes_out{0};
        std::atomic<U64> decode_ns{0};
    };
    StatSlot sStats[KIND_COUNT];

    inline void bump(std::atomic<U64>& counter, U64 amount = 1)
    {
        counter.fetch_add(amount, std::memory_order_relaxed);
    }

    using Clock = std::chrono::steady_clock;

    inline U64 elapsedNs(Clock::time_point start)
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
    }

    // ------------------------------------------------- per-thread contexts

    std::atomic<U32> sLiveContexts{0};

    struct ThreadContexts
    {
        ZSTD_CCtx* mCCtx = nullptr;
        ZSTD_DCtx* mDCtx = nullptr;

        ~ThreadContexts() { release(); }

        void release()
        {
            if (mCCtx)
            {
                ZSTD_freeCCtx(mCCtx);
                mCCtx = nullptr;
                sLiveContexts.fetch_sub(1, std::memory_order_relaxed);
            }
            if (mDCtx)
            {
                ZSTD_freeDCtx(mDCtx);
                mDCtx = nullptr;
                sLiveContexts.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        // Null on allocation failure; callers degrade (raw store / reject).
        ZSTD_CCtx* compressor()
        {
            if (!mCCtx)
            {
                mCCtx = ZSTD_createCCtx();
                if (mCCtx)
                {
                    sLiveContexts.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return mCCtx;
        }

        ZSTD_DCtx* decompressor()
        {
            if (!mDCtx)
            {
                mDCtx = ZSTD_createDCtx();
                if (mDCtx)
                {
                    sLiveContexts.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return mDCtx;
        }
    };
    thread_local ThreadContexts tContexts;

    // ------------------------------------------------------------- header

    inline void putU64(U8* p, U64 v)
    {
        for (int i = 0; i < 8; ++i)
        {
            p[i] = (U8)(v >> (8 * i));
        }
    }

    inline U64 getU64(const U8* p)
    {
        U64 v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= (U64)p[i] << (8 * i);
        }
        return v;
    }

    void writeHeader(U8* dst, U8 codec, U8 kind, U8 flags, U64 size, U64 checksum)
    {
        memcpy(dst, MAGIC, 4);
        dst[4] = FORMAT_VERSION;
        dst[5] = codec;
        dst[6] = kind;
        dst[7] = flags;
        putU64(dst + 8, size);
        putU64(dst + 16, checksum);
    }

    U64 checksumOf(const U8* data, size_t len)
    {
        static const U8 empty = 0;
        return HBXXH64::digest(len ? (const void*)data : (const void*)&empty, len);
    }

    enum class EParse
    {
        LEGACY,
        OK,
        BAD,
    };

    EParse parseHeader(const U8* src, size_t len, U64 max_size, RecordInfo& info)
    {
        info = RecordInfo();
        if (!src || len < sizeof(MAGIC) || memcmp(src, MAGIC, sizeof(MAGIC)) != 0)
        {
            info.legacy            = true;
            info.uncompressed_size = len;
            info.payload_size      = len;
            return EParse::LEGACY;
        }
        if (len < HEADER_SIZE)
        {
            return EParse::BAD;     // truncated header
        }

        info.version           = src[4];
        info.codec             = src[5];
        info.kind              = src[6];
        info.flags             = src[7];
        info.uncompressed_size = getU64(src + 8);
        info.checksum          = getU64(src + 16);
        info.payload_size      = len - HEADER_SIZE;

        if (info.version == 0 || info.version > FORMAT_VERSION)
        {
            return EParse::BAD;
        }
        if ((info.flags & ~FLAGS_KNOWN_V1) || !(info.flags & FLAG_CHECKSUM))
        {
            return EParse::BAD;     // reserved bits set, or checksum missing
        }
        if (info.uncompressed_size > max_size
            || info.uncompressed_size > (U64)(SIZE_MAX - HEADER_SIZE))
        {
            return EParse::BAD;     // allocation bomb / over the record cap
        }

        switch (info.codec)
        {
        case CODEC_RAW:
            if ((U64)info.payload_size != info.uncompressed_size)
            {
                return EParse::BAD;
            }
            break;

        case CODEC_ZSTD:
        {
            if (info.payload_size == 0)
            {
                return EParse::BAD;
            }
            // Parses only the frame header: cheap, and rejects truncated or
            // non-zstd payloads and size mismatches before any allocation.
            unsigned long long frame_size =
                ZSTD_getFrameContentSize(src + HEADER_SIZE, info.payload_size);
            if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN
                || frame_size == ZSTD_CONTENTSIZE_ERROR
                || (U64)frame_size != info.uncompressed_size)
            {
                return EParse::BAD;
            }
            break;
        }

        default:
            return EParse::BAD;
        }
        return EParse::OK;
    }

    // ------------------------------------------------------ encode/decode

    bool encodeImpl(ECacheRecordKind kind, const Policy& policy, U64 max_size,
                    const U8* src, size_t len, U8* dst, size_t dst_cap,
                    size_t& written)
    {
        Clock::time_point start = Clock::now();
        StatSlot& stats = sStats[kindIndex(kind)];
        written = 0;

        if ((!src && len) || !dst || (U64)len > max_size
            || len > SIZE_MAX - HEADER_SIZE || dst_cap < encodeBound(len))
        {
            bump(stats.encode_failures);
            return false;
        }

        const U8  flags    = FLAG_CHECKSUM;     // required in version 1
        const U64 checksum = checksumOf(src, len);

        U8     codec   = CODEC_RAW;
        size_t payload = len;

        if (policy.enabled && len > 0)
        {
            // Give zstd only as much room as a worthwhile result may take:
            // if it does not fit, the record is not compressible enough and
            // zstd stops with dstSize_tooSmall; store raw instead. This also
            // keeps the output buffer at HEADER_SIZE + len.
            const U64 saving = std::min<U64>(policy.min_saving_pct, 100);
            const U64 limit  = (U64)len * (100 - saving) / 100;
            if (limit > 0)
            {
                ZSTD_CCtx* cctx = tContexts.compressor();
                if (cctx)
                {
                    size_t result = ZSTD_compressCCtx(cctx, dst + HEADER_SIZE,
                                                      (size_t)limit, src, len,
                                                      resolveLevel(policy.level));
                    if (!ZSTD_isError(result))
                    {
                        codec   = CODEC_ZSTD;
                        payload = result;
                    }
                    else if (ZSTD_getErrorCode(result) != ZSTD_error_dstSize_tooSmall)
                    {
                        LL_WARNS_ONCE("CacheCodec") << "zstd compression failed ("
                                                    << ZSTD_getErrorName(result)
                                                    << "); storing raw" << LL_ENDL;
                    }
                }
            }
        }

        if (codec == CODEC_RAW && len > 0)
        {
            memcpy(dst + HEADER_SIZE, src, len);
        }
        writeHeader(dst, codec, (U8)kind, flags, (U64)len, checksum);
        written = HEADER_SIZE + payload;

        bump(stats.encodes);
        bump(codec == CODEC_ZSTD ? stats.encoded_zstd : stats.encoded_raw);
        bump(stats.encode_bytes_in, len);
        bump(stats.encode_bytes_out, written);
        bump(stats.encode_ns, elapsedNs(start));
        return true;
    }

    bool encodeVecImpl(ECacheRecordKind kind, const Policy& policy, U64 max_size,
                       const U8* src, size_t len, std::vector<U8>& out)
    {
        if ((U64)len > max_size || len > SIZE_MAX - HEADER_SIZE)
        {
            bump(sStats[kindIndex(kind)].encode_failures);
            out.clear();
            return false;
        }
        out.resize(encodeBound(len));
        size_t written = 0;
        if (!encodeImpl(kind, policy, max_size, src, len, out.data(), out.size(), written))
        {
            out.clear();
            return false;
        }
        out.resize(written);
        return true;
    }

    bool decodeImpl(ECacheRecordKind kind, U64 max_size, const U8* src, size_t len,
                    U8* dst, size_t dst_cap, size_t& written, RecordInfo* info_out,
                    bool allow_legacy = true)
    {
        Clock::time_point start = Clock::now();
        StatSlot& stats = sStats[kindIndex(kind)];
        written = 0;

        RecordInfo info;
        const EParse parsed = parseHeader(src, len, max_size, info);
        bool ok = false;

        if (parsed == EParse::LEGACY)
        {
            if (allow_legacy && len <= dst_cap && (len == 0 || dst))
            {
                if (len > 0)
                {
                    memcpy(dst, src, len);
                }
                written = len;
                ok = true;
            }
        }
        else if (parsed == EParse::OK)
        {
            const size_t size    = (size_t)info.uncompressed_size;
            const U8*    payload = src + HEADER_SIZE;
            if (size <= dst_cap && (size == 0 || dst))
            {
                if (info.codec == CODEC_RAW)
                {
                    if (size > 0)
                    {
                        memcpy(dst, payload, size);
                    }
                    ok = true;
                }
                else // CODEC_ZSTD (parseHeader rejected everything else)
                {
                    ZSTD_DCtx* dctx = tContexts.decompressor();
                    if (dctx)
                    {
                        U8  dummy  = 0;
                        U8* target = size ? dst : &dummy;
                        // Decodes into the flat buffer (no window buffer); a
                        // frame claiming more than `size` fails with
                        // dstSize_tooSmall, trailing garbage fails too.
                        size_t result = ZSTD_decompressDCtx(dctx, target, size,
                                                            payload, info.payload_size);
                        ok = !ZSTD_isError(result) && result == size;
                    }
                    else
                    {
                        LL_WARNS_ONCE("CacheCodec") << "Could not allocate a zstd "
                                                    << "decompression context" << LL_ENDL;
                    }
                }
                if (ok) // FLAG_CHECKSUM is required (parseHeader)
                {
                    ok = checksumOf(dst, size) == info.checksum;
                }
                if (ok)
                {
                    written = size;
                }
            }
        }

        if (info_out)
        {
            *info_out = info;
        }
        if (ok)
        {
            bump(stats.decodes);
            if (info.legacy)
            {
                bump(stats.decoded_legacy);
            }
            bump(stats.decode_bytes_in, len);
            bump(stats.decode_bytes_out, written);
        }
        else
        {
            bump(stats.decode_failures);
            written = 0;
        }
        bump(stats.decode_ns, elapsedNs(start));
        return ok;
    }

    bool decodeVecImpl(ECacheRecordKind kind, U64 max_size, const U8* src, size_t len,
                       std::vector<U8>& out, RecordInfo* info_out,
                       bool allow_legacy = true)
    {
        RecordInfo pre;
        const EParse parsed = parseHeader(src, len, max_size, pre);
        // On BAD, decodeImpl fails again on its own and does the stats.
        const bool fails = parsed == EParse::BAD || (parsed == EParse::LEGACY && !allow_legacy);
        out.resize(fails ? 0 : (size_t)pre.uncompressed_size);
        size_t written = 0;
        if (!decodeImpl(kind, max_size, src, len, out.data(), out.size(), written, info_out,
                        allow_legacy))
        {
            out.clear();
            return false;
        }
        out.resize(written);
        return true;
    }

    Policy loadPolicy(ECacheRecordKind kind)
    {
        const PolicySlot& slot = sPolicy[kindIndex(kind)];
        Policy policy;
        policy.enabled        = slot.enabled.load(std::memory_order_relaxed);
        policy.level          = slot.level.load(std::memory_order_relaxed);
        policy.min_saving_pct = slot.min_saving_pct.load(std::memory_order_relaxed);
        policy.allow_legacy   = slot.allow_legacy.load(std::memory_order_relaxed);
        return policy;
    }

    std::string formatBytes(U64 bytes)
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(1);
        if (bytes >= 1024ull * 1024ull * 1024ull)
        {
            s << (F64)bytes / (1024.0 * 1024.0 * 1024.0) << " GiB";
        }
        else if (bytes >= 1024ull * 1024ull)
        {
            s << (F64)bytes / (1024.0 * 1024.0) << " MiB";
        }
        else if (bytes >= 1024ull)
        {
            s << (F64)bytes / 1024.0 << " KiB";
        }
        else
        {
            s << bytes << " B";
        }
        return s.str();
    }
} // anonymous namespace

// ------------------------------------------------------------------ policy

S32 LLCacheCodec::defaultLevel()
{
    return sDefaultLevel;
}

void LLCacheCodec::setPolicy(ECacheRecordKind kind, const Policy& policy)
{
    PolicySlot& slot = sPolicy[kindIndex(kind)];
    slot.enabled.store(policy.enabled, std::memory_order_relaxed);
    slot.level.store(policy.level, std::memory_order_relaxed);
    slot.min_saving_pct.store(std::min<U32>(policy.min_saving_pct, 100),
                              std::memory_order_relaxed);
    slot.allow_legacy.store(policy.allow_legacy, std::memory_order_relaxed);
}

LLCacheCodec::Policy LLCacheCodec::getPolicy(ECacheRecordKind kind)
{
    return loadPolicy(kind);
}

void LLCacheCodec::setEnabled(ECacheRecordKind kind, bool enabled)
{
    sPolicy[kindIndex(kind)].enabled.store(enabled, std::memory_order_relaxed);
}

void LLCacheCodec::setLevel(ECacheRecordKind kind, S32 level)
{
    sPolicy[kindIndex(kind)].level.store(level, std::memory_order_relaxed);
}

void LLCacheCodec::setMaxRecordSize(U64 bytes)
{
    sMaxRecordSize.store(std::min<U64>(bytes, (U64)(SIZE_MAX - HEADER_SIZE)),
                         std::memory_order_relaxed);
}

U64 LLCacheCodec::getMaxRecordSize()
{
    return sMaxRecordSize.load(std::memory_order_relaxed);
}

// ----------------------------------------------------------------- records

bool LLCacheCodec::peek(const U8* src, size_t len, RecordInfo& info)
{
    return parseHeader(src, len, getMaxRecordSize(), info) != EParse::BAD;
}

bool LLCacheCodec::encode(ECacheRecordKind kind, const U8* src, size_t len,
                          std::vector<U8>& out)
{
    return encodeVecImpl(kind, loadPolicy(kind), getMaxRecordSize(), src, len, out);
}

bool LLCacheCodec::encode(ECacheRecordKind kind, const U8* src, size_t len,
                          U8* dst, size_t dst_cap, size_t& written)
{
    return encodeImpl(kind, loadPolicy(kind), getMaxRecordSize(), src, len,
                      dst, dst_cap, written);
}

bool LLCacheCodec::decode(ECacheRecordKind kind, const U8* src, size_t len,
                          std::vector<U8>& out, RecordInfo* info)
{
    return decodeVecImpl(kind, getMaxRecordSize(), src, len, out, info,
                         loadPolicy(kind).allow_legacy);
}

bool LLCacheCodec::decode(ECacheRecordKind kind, const U8* src, size_t len,
                          U8* dst, size_t dst_cap, size_t& written, RecordInfo* info)
{
    return decodeImpl(kind, getMaxRecordSize(), src, len, dst, dst_cap, written, info,
                      loadPolicy(kind).allow_legacy);
}

bool LLCacheCodec::encodeWith(ECacheRecordKind kind, const Policy& policy,
                              const U8* src, size_t len, std::vector<U8>& out)
{
    return encodeVecImpl(kind, policy, getMaxRecordSize(), src, len, out);
}

bool LLCacheCodec::decodeWith(ECacheRecordKind kind, const Policy& policy,
                              const U8* src, size_t len, std::vector<U8>& out,
                              RecordInfo* info)
{
    return decodeVecImpl(kind, getMaxRecordSize(), src, len, out, info,
                         policy.allow_legacy);
}

bool LLCacheCodec::decodeWith(ECacheRecordKind kind, const Policy& policy,
                              const U8* src, size_t len, U8* dst, size_t dst_cap,
                              size_t& written, RecordInfo* info)
{
    return decodeImpl(kind, getMaxRecordSize(), src, len, dst, dst_cap, written, info,
                      policy.allow_legacy);
}

// ------------------------------------------------------------------- stats

LLCacheCodec::KindStats LLCacheCodec::getStats(ECacheRecordKind kind)
{
    const StatSlot& slot = sStats[kindIndex(kind)];
    KindStats s;
    s.encodes          = slot.encodes.load(std::memory_order_relaxed);
    s.encoded_raw      = slot.encoded_raw.load(std::memory_order_relaxed);
    s.encoded_zstd     = slot.encoded_zstd.load(std::memory_order_relaxed);
    s.encode_failures  = slot.encode_failures.load(std::memory_order_relaxed);
    s.encode_bytes_in  = slot.encode_bytes_in.load(std::memory_order_relaxed);
    s.encode_bytes_out = slot.encode_bytes_out.load(std::memory_order_relaxed);
    s.encode_ns        = slot.encode_ns.load(std::memory_order_relaxed);
    s.decodes          = slot.decodes.load(std::memory_order_relaxed);
    s.decoded_legacy   = slot.decoded_legacy.load(std::memory_order_relaxed);
    s.decode_failures  = slot.decode_failures.load(std::memory_order_relaxed);
    s.decode_bytes_in  = slot.decode_bytes_in.load(std::memory_order_relaxed);
    s.decode_bytes_out = slot.decode_bytes_out.load(std::memory_order_relaxed);
    s.decode_ns        = slot.decode_ns.load(std::memory_order_relaxed);
    return s;
}

void LLCacheCodec::resetStats()
{
    for (StatSlot& slot : sStats)
    {
        for (std::atomic<U64>* counter : { &slot.encodes, &slot.encoded_raw,
                                           &slot.encoded_zstd, &slot.encode_failures,
                                           &slot.encode_bytes_in, &slot.encode_bytes_out,
                                           &slot.encode_ns, &slot.decodes,
                                           &slot.decoded_legacy, &slot.decode_failures,
                                           &slot.decode_bytes_in, &slot.decode_bytes_out,
                                           &slot.decode_ns })
        {
            counter->store(0, std::memory_order_relaxed);
        }
    }
}

std::string LLCacheCodec::formatStats()
{
    std::ostringstream out;
    out << "Cache codec (zstd " << ZSTD_versionString() << ", default level "
        << sDefaultLevel << ", live contexts " << liveContexts()
        << ", record cap " << formatBytes(getMaxRecordSize()) << ")";

    bool any = false;
    for (size_t i = 0; i < KIND_COUNT; ++i)
    {
        const ECacheRecordKind kind = (ECacheRecordKind)i;
        const KindStats s = getStats(kind);
        if (!s.encodes && !s.encode_failures && !s.decodes && !s.decode_failures)
        {
            continue;
        }
        any = true;
        const Policy p = loadPolicy(kind);
        out << std::fixed << std::setprecision(2);
        out << "\n  " << kindName(kind) << " [" << (p.enabled ? "on" : "off")
            << ", level " << resolveLevel(p.level) << "]:";
        out << " encoded " << s.encodes << " (zstd " << s.encoded_zstd
            << ", raw " << s.encoded_raw << ", refused " << s.encode_failures << ") "
            << formatBytes(s.encode_bytes_in) << " -> " << formatBytes(s.encode_bytes_out);
        if (s.encode_bytes_out)
        {
            out << " (" << (F64)s.encode_bytes_in / (F64)s.encode_bytes_out << "x, saved ";
            const S64 saved = s.bytesSaved();
            out << (saved < 0 ? "-" : "") << formatBytes((U64)(saved < 0 ? -saved : saved))
                << ")";
        }
        out << " in " << (F64)s.encode_ns / 1.0e6 << " ms";
        out << "; decoded " << s.decodes << " (legacy " << s.decoded_legacy
            << ", rejected " << s.decode_failures << ") "
            << formatBytes(s.decode_bytes_in) << " -> " << formatBytes(s.decode_bytes_out)
            << " in " << (F64)s.decode_ns / 1.0e6 << " ms";
    }
    if (!any)
    {
        out << "\n  no records encoded or decoded";
    }
    return out.str();
}

void LLCacheCodec::logStats()
{
    LL_INFOS("CacheCodec") << formatStats() << LL_ENDL;
}

U32 LLCacheCodec::liveContexts()
{
    return sLiveContexts.load(std::memory_order_relaxed);
}

void LLCacheCodec::releaseThreadContexts()
{
    tContexts.release();
}

const char* LLCacheCodec::kindName(ECacheRecordKind kind)
{
    switch (kind)
    {
    case ECacheRecordKind::GENERIC:     return "generic";
    case ECacheRecordKind::OBJECT:      return "object";
    case ECacheRecordKind::TEXTURE_GPU: return "texture_gpu";
    case ECacheRecordKind::TEXTURE_J2C: return "texture_j2c";
    case ECacheRecordKind::ASSET:       return "asset";
    default:                            return "unknown";
    }
}

// --------------------------------------------------------------- self test

bool LLCacheCodec::selfTest(std::string* failure)
{
    const ECacheRecordKind kind = ECacheRecordKind::GENERIC;
    const U64 cap = DEFAULT_MAX_RECORD_SIZE;

#define SELFTEST_CHECK(cond, msg)   \
    if (!(cond))                    \
    {                               \
        return msg;                 \
    }

    // Returns the first failed check, or nullptr when everything passed.
    auto run = [&]() -> const char*
    {
        Policy on;
        on.enabled        = true;
        on.level          = 3;
        on.min_saving_pct = 5;
        Policy off        = on;
        off.enabled       = false;

        // Compressible: structured, repetitive bytes (like an object cache).
        std::vector<U8> text(256 * 1024);
        for (size_t i = 0; i < text.size(); ++i)
        {
            text[i] = (U8)("object cache entry "[i % 19] + (U8)((i / 4096) & 7));
        }
        // Incompressible: xorshift noise (like J2C).
        std::vector<U8> noise(64 * 1024);
        U64 x = 0x9E3779B97F4A7C15ull;
        for (U8& b : noise)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b = (U8)(x >> 32);
        }

        std::vector<U8> enc, dec;
        RecordInfo info;
        size_t written = 0;

        // 1. Compressible round trip stores zstd and shrinks.
        SELFTEST_CHECK(encodeVecImpl(kind, on, cap, text.data(), text.size(), enc),
                       "encode compressible");
        SELFTEST_CHECK(enc.size() >= HEADER_SIZE && enc[5] == CODEC_ZSTD,
                       "compressible record not stored as zstd");
        SELFTEST_CHECK(enc.size() < text.size() / 2, "compressible record barely shrank");
        SELFTEST_CHECK(decodeVecImpl(kind, cap, enc.data(), enc.size(), dec, &info),
                       "decode compressible");
        SELFTEST_CHECK(dec == text, "compressible round trip mismatch");
        SELFTEST_CHECK(!info.legacy && info.codec == CODEC_ZSTD
                       && info.kind == (U8)kind && (info.flags & FLAG_CHECKSUM)
                       && info.uncompressed_size == text.size(),
                       "compressible header fields");

        // 2. Zero-copy decode into an exact buffer; one byte short fails.
        {
            std::vector<U8> buf(text.size());
            SELFTEST_CHECK(decodeImpl(kind, cap, enc.data(), enc.size(), buf.data(),
                                      buf.size(), written, nullptr)
                           && written == text.size() && buf == text,
                           "zero-copy decode");
            SELFTEST_CHECK(!decodeImpl(kind, cap, enc.data(), enc.size(), buf.data(),
                                       buf.size() - 1, written, nullptr),
                           "zero-copy decode into short buffer accepted");
            std::vector<U8> small(encodeBound(text.size()) - 1);
            SELFTEST_CHECK(!encodeImpl(kind, on, cap, text.data(), text.size(),
                                       small.data(), small.size(), written),
                           "encode into buffer below encodeBound accepted");
        }

        // 3. Corrupt zstd records are rejected.
        {
            std::vector<U8> bad = enc;
            bad.pop_back();
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "truncated zstd payload accepted");
            bad = enc;
            bad.push_back(0);
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "zstd payload with trailing garbage accepted");
            bad = enc;
            bad[HEADER_SIZE + (bad.size() - HEADER_SIZE) / 2] ^= 0x5A;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "corrupted zstd payload accepted");
            bad = enc;
            putU64(bad.data() + 8, text.size() + 1);
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "zstd size mismatch accepted");
            SELFTEST_CHECK(!decodeVecImpl(kind, text.size() - 1, enc.data(), enc.size(),
                                          dec, nullptr),
                           "record over the size cap accepted");
            bad = enc;
            putU64(bad.data() + 8, 1ull << 40);
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "1 TiB declared size accepted");
            bad = enc;
            bad[4] = FORMAT_VERSION + 1;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "future version accepted");
            bad[4] = 0;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "version 0 accepted");
            bad = enc;
            bad[5] = 7;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "unknown codec accepted");
            bad = enc;
            bad[7] |= 0x80;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "reserved flag bit accepted");
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, enc.data(), 10, dec, nullptr),
                           "truncated header accepted");
        }

        // 4. Incompressible data is stored raw, byte for byte.
        SELFTEST_CHECK(encodeVecImpl(kind, on, cap, noise.data(), noise.size(), enc),
                       "encode incompressible");
        SELFTEST_CHECK(enc.size() == HEADER_SIZE + noise.size() && enc[5] == CODEC_RAW
                       && memcmp(enc.data() + HEADER_SIZE, noise.data(), noise.size()) == 0,
                       "incompressible record not stored raw");
        SELFTEST_CHECK(decodeVecImpl(kind, cap, enc.data(), enc.size(), dec, &info)
                       && dec == noise && info.codec == CODEC_RAW,
                       "incompressible round trip");
        {
            std::vector<U8> bad = enc;
            bad[HEADER_SIZE + 100] ^= 0x01;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "raw record checksum mismatch accepted");
            bad = enc;
            bad.pop_back();
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "truncated raw record accepted");
        }

        // 5. Disabled kind stores raw even when compressible.
        SELFTEST_CHECK(encodeVecImpl(kind, off, cap, text.data(), text.size(), enc)
                       && enc[5] == CODEC_RAW && enc.size() == HEADER_SIZE + text.size(),
                       "disabled policy did not store raw");
        SELFTEST_CHECK(decodeVecImpl(kind, cap, enc.data(), enc.size(), dec, nullptr)
                       && dec == text,
                       "disabled policy round trip");
        {
            // Clearing the checksum flag must not switch verification off.
            std::vector<U8> bad = enc;
            bad[7] = 0;
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, bad.data(), bad.size(), dec, nullptr),
                           "record without the checksum flag accepted");
        }

        // 6. Empty record: header only, decodes to nothing.
        SELFTEST_CHECK(encodeVecImpl(kind, on, cap, nullptr, 0, enc)
                       && enc.size() == HEADER_SIZE && enc[5] == CODEC_RAW,
                       "encode empty");
        SELFTEST_CHECK(decodeVecImpl(kind, cap, enc.data(), enc.size(), dec, &info)
                       && dec.empty() && !info.legacy,
                       "decode empty");

        // 7. Legacy (pre-shim) bytes pass through unchanged, short or long.
        {
            const char* legacy_text = "legacy cache record, no header";
            std::vector<U8> legacy(legacy_text, legacy_text + strlen(legacy_text));
            SELFTEST_CHECK(decodeVecImpl(kind, cap, legacy.data(), legacy.size(), dec, &info)
                           && dec == legacy && info.legacy,
                           "legacy passthrough");
            SELFTEST_CHECK(decodeVecImpl(kind, cap, legacy.data(), 2, dec, &info)
                           && dec.size() == 2 && info.legacy,
                           "short legacy passthrough");
            SELFTEST_CHECK(decodeVecImpl(kind, cap, nullptr, 0, dec, &info)
                           && dec.empty() && info.legacy,
                           "empty legacy passthrough");
            SELFTEST_CHECK(decodeVecImpl(kind, cap, noise.data(), noise.size(), dec, nullptr)
                           && dec == noise,
                           "legacy noise passthrough");
            // Kinds that forbid legacy input reject it (and still decode
            // real records).
            SELFTEST_CHECK(!decodeVecImpl(kind, cap, legacy.data(), legacy.size(), dec,
                                          nullptr, false),
                           "legacy record accepted with allow_legacy off");
            SELFTEST_CHECK(encodeVecImpl(kind, on, cap, text.data(), text.size(), enc)
                           && decodeVecImpl(kind, cap, enc.data(), enc.size(), dec, nullptr,
                                            false)
                           && dec == text,
                           "real record rejected with allow_legacy off");
        }

        // 8. Refuse to encode what could not be read back.
        SELFTEST_CHECK(!encodeVecImpl(kind, on, text.size() - 1, text.data(), text.size(), enc),
                       "record over the size cap encoded");
        return nullptr;
    };
#undef SELFTEST_CHECK

    const char* why = run();
    if (why)
    {
        LL_WARNS("CacheCodec") << "Self test FAILED: " << why << LL_ENDL;
        if (failure)
        {
            *failure = why;
        }
        return false;
    }
    LL_INFOS("CacheCodec") << "Self test passed (zstd " << ZSTD_versionString() << ")"
                           << LL_ENDL;
    return true;
}
