/**
 * @file llcachecodec_test.cpp
 * @brief Tests for the cache record codec shim (llcachecodec.h).
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

#include <cstring>
#include <string>
#include <vector>

#include "linden_common.h"

#include "../llcachecodec.h"

#include "../test/lltut.h"

namespace
{
    std::vector<U8> makeCompressible(size_t size)
    {
        std::vector<U8> data(size);
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = (U8)("region object cache "[i % 20] + (U8)((i / 8192) & 3));
        }
        return data;
    }

    std::vector<U8> makeNoise(size_t size)
    {
        std::vector<U8> data(size);
        U64 x = 0x2545F4914F6CDD1Dull;
        for (U8& b : data)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b = (U8)(x >> 24);
        }
        return data;
    }
}

namespace tut
{
    struct cachecodec_data
    {
        // Tests change the global policy; put the defaults back afterwards.
        LLCacheCodec::Policy mSaved[(size_t)ECacheRecordKind::COUNT];

        cachecodec_data()
        {
            for (size_t i = 0; i < (size_t)ECacheRecordKind::COUNT; ++i)
            {
                mSaved[i] = LLCacheCodec::getPolicy((ECacheRecordKind)i);
            }
        }

        ~cachecodec_data()
        {
            for (size_t i = 0; i < (size_t)ECacheRecordKind::COUNT; ++i)
            {
                LLCacheCodec::setPolicy((ECacheRecordKind)i, mSaved[i]);
            }
        }
    };
    typedef test_group<cachecodec_data> cachecodec_test;
    typedef cachecodec_test::object cachecodec_object;
    tut::cachecodec_test cachecodec("LLCacheCodec");

    // Built-in self test (round trips + every corruption class).
    template<> template<>
    void cachecodec_object::test<1>()
    {
        std::string failure;
        bool ok = LLCacheCodec::selfTest(&failure);
        ensure("selfTest: " + failure, ok);
    }

    // Compressible object-cache-like data round trips through zstd; the
    // header carries the kind and sizes; peek agrees without decoding.
    template<> template<>
    void cachecodec_object::test<2>()
    {
        LLCacheCodec::setEnabled(ECacheRecordKind::OBJECT, true);
        LLCacheCodec::setLevel(ECacheRecordKind::OBJECT, 0);
        const std::vector<U8> rec = makeCompressible(512 * 1024);

        std::vector<U8> enc;
        ensure("encode", LLCacheCodec::encode(ECacheRecordKind::OBJECT, rec.data(),
                                              rec.size(), enc));
        ensure("smaller", enc.size() < rec.size() / 2);
        ensure("magic", enc.size() >= LLCacheCodec::HEADER_SIZE
                        && memcmp(enc.data(), "SLCZ", 4) == 0);

        LLCacheCodec::RecordInfo info;
        ensure("peek", LLCacheCodec::peek(enc.data(), enc.size(), info));
        ensure_equals("peek codec", (S32)info.codec, (S32)LLCacheCodec::CODEC_ZSTD);
        ensure_equals("peek kind", (S32)info.kind, (S32)ECacheRecordKind::OBJECT);
        ensure_equals("peek size", info.uncompressed_size, (U64)rec.size());
        ensure("peek not legacy", !info.legacy);

        std::vector<U8> dec;
        ensure("decode", LLCacheCodec::decode(ECacheRecordKind::OBJECT, enc.data(),
                                              enc.size(), dec));
        ensure("round trip", dec == rec);

        // Zero-copy path, sized from peek.
        std::vector<U8> buf((size_t)info.uncompressed_size);
        size_t written = 0;
        ensure("decode into buffer",
               LLCacheCodec::decode(ECacheRecordKind::OBJECT, enc.data(), enc.size(),
                                    buf.data(), buf.size(), written)
               && written == rec.size() && buf == rec);
    }

    // J2C (disabled by default) and incompressible data are stored raw.
    template<> template<>
    void cachecodec_object::test<3>()
    {
        const std::vector<U8> compressible = makeCompressible(64 * 1024);
        const std::vector<U8> noise        = makeNoise(64 * 1024);
        std::vector<U8> enc, dec;

        ensure("j2c disabled by default",
               !LLCacheCodec::getPolicy(ECacheRecordKind::TEXTURE_J2C).enabled);
        ensure("encode j2c", LLCacheCodec::encode(ECacheRecordKind::TEXTURE_J2C,
                                                  compressible.data(),
                                                  compressible.size(), enc));
        ensure_equals("j2c raw codec", (S32)enc[5], (S32)LLCacheCodec::CODEC_RAW);
        ensure_equals("j2c raw size", enc.size(),
                      LLCacheCodec::HEADER_SIZE + compressible.size());

        LLCacheCodec::setEnabled(ECacheRecordKind::ASSET, true);
        ensure("encode noise", LLCacheCodec::encode(ECacheRecordKind::ASSET, noise.data(),
                                                    noise.size(), enc));
        ensure_equals("noise raw codec", (S32)enc[5], (S32)LLCacheCodec::CODEC_RAW);
        ensure("decode noise", LLCacheCodec::decode(ECacheRecordKind::ASSET, enc.data(),
                                                    enc.size(), dec)
                               && dec == noise);
    }

    // Legacy (pre-shim) records pass through; corrupt records are rejected.
    template<> template<>
    void cachecodec_object::test<4>()
    {
        const std::vector<U8> legacy = makeNoise(1000);
        std::vector<U8> dec;
        LLCacheCodec::RecordInfo info;
        ensure("legacy passthrough",
               LLCacheCodec::decode(ECacheRecordKind::OBJECT, legacy.data(), legacy.size(),
                                    dec, &info)
               && dec == legacy && info.legacy);

        const std::vector<U8> rec = makeCompressible(100 * 1024);
        std::vector<U8> enc;
        ensure("encode", LLCacheCodec::encode(ECacheRecordKind::GENERIC, rec.data(),
                                              rec.size(), enc));
        std::vector<U8> bad = enc;
        bad.resize(bad.size() - 3);
        ensure("truncated rejected",
               !LLCacheCodec::decode(ECacheRecordKind::GENERIC, bad.data(), bad.size(), dec));
        ensure("truncated header rejected",
               !LLCacheCodec::decode(ECacheRecordKind::GENERIC, enc.data(), 12, dec));
        bad = enc;
        bad[bad.size() / 2] ^= 0xFF;
        ensure("corrupt rejected",
               !LLCacheCodec::decode(ECacheRecordKind::GENERIC, bad.data(), bad.size(), dec));
        ensure("rejected output cleared", dec.empty());

        // A kind that forbids legacy input rejects header-less bytes but
        // still decodes real records.
        LLCacheCodec::Policy strict = LLCacheCodec::getPolicy(ECacheRecordKind::TEXTURE_GPU);
        strict.allow_legacy = false;
        LLCacheCodec::setPolicy(ECacheRecordKind::TEXTURE_GPU, strict);
        ensure("legacy rejected when not allowed",
               !LLCacheCodec::decode(ECacheRecordKind::TEXTURE_GPU, legacy.data(),
                                     legacy.size(), dec));
        ensure("encode gpu", LLCacheCodec::encode(ECacheRecordKind::TEXTURE_GPU, rec.data(),
                                                  rec.size(), enc));
        ensure("real record decodes when legacy not allowed",
               LLCacheCodec::decode(ECacheRecordKind::TEXTURE_GPU, enc.data(), enc.size(), dec)
               && dec == rec);
    }

    // The record cap bounds both directions.
    template<> template<>
    void cachecodec_object::test<5>()
    {
        const U64 saved_cap = LLCacheCodec::getMaxRecordSize();
        const std::vector<U8> rec = makeCompressible(4096);
        std::vector<U8> enc, dec;
        ensure("encode", LLCacheCodec::encode(ECacheRecordKind::GENERIC, rec.data(),
                                              rec.size(), enc));
        LLCacheCodec::setMaxRecordSize(1024);
        bool decoded = LLCacheCodec::decode(ECacheRecordKind::GENERIC, enc.data(),
                                            enc.size(), dec);
        bool encoded = LLCacheCodec::encode(ECacheRecordKind::GENERIC, rec.data(),
                                            rec.size(), enc);
        LLCacheCodec::setMaxRecordSize(saved_cap);
        ensure("oversize decode rejected", !decoded);
        ensure("oversize encode refused", !encoded);
    }
}
