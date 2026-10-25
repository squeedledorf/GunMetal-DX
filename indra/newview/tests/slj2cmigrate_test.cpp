/**
 * @file slj2cmigrate_test.cpp
 * @brief The legacy J2C texture cache -> segment store converter (#714): parsing
 *        texture.entries / texture.cache / bodies, conversion, dropping legacy files only
 *        after a confirmed write, resume after failures, the budget cut, the version
 *        check, read-through and forget.
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

#include "../slj2cmigrate.h"
#include "../llj2crecord.h"

#include "llcachestore_platform.h"
#include "fsyspath.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../test/lltut.h"

namespace fs = std::filesystem;

namespace
{
    std::atomic<U32> sDirCounter{ 0 };

    struct TempDir
    {
        std::string path;
        TempDir()
        {
            std::error_code ec;
            fs::path base = fs::temp_directory_path(ec);
            path = (base / ("slj2cmigrate_test_" + std::to_string(LLCacheStorePlatform::processId()) + "_" +
                            std::to_string(sDirCounter++))).string();
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        std::string sub(const std::string& name) const { return path + "/" + name; }
    };

    LLUUID idFor(U32 n)
    {
        LLUUID id;
        for (int i = 0; i < 16; ++i) id.mData[i] = (U8)((n * 2654435761u) >> ((i % 4) * 8)) ^ (U8)(i * 7);
        id.mData[0] = (U8)n;
        id.mData[1] = (U8)(n >> 8);
        return id;
    }

    std::vector<U8> codestream(U32 seed, size_t n)
    {
        std::vector<U8> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (U8)(i * 31 + seed * 7 + (i >> 9));
        return v;
    }

    // One legacy texture as LLTextureCache wrote it.
    struct Tex
    {
        LLUUID id;
        std::vector<U8> prefix;   // the cached bytes
        S32 image_size = 0;
        U32 time = 0;
        bool free_slot = false;   // a freed slot (image size -1)
    };

    void put32(std::vector<U8>& v, size_t at, U32 x) { for (int i = 0; i < 4; ++i) v[at + i] = (U8)(x >> (8 * i)); }

    // Writes texture.entries + texture.cache + bodies into `dir` (version/address as given).
    void writeLegacy(const std::string& dir, const std::vector<Tex>& texs, F32 version = 1.71f, U32 address = 64)
    {
        fs::create_directories(dir);
        for (const char* d : { "0","1","2","3","4","5","6","7","8","9","a","b","c","d","e","f" })
            fs::create_directories(dir + "/" + d);
        std::vector<U8> entries(SLJ2CMigrate::ENTRIES_INFO_BYTES + texs.size() * SLJ2CMigrate::ENTRY_BYTES, 0);
        memcpy(entries.data(), &version, 4);
        put32(entries, 4, address);
        strcpy((char*)entries.data() + 8, "TestEngine 1.0");
        put32(entries, 40, (U32)texs.size());
        std::vector<U8> stripes(texs.size() * SLJ2CMigrate::STRIPE_BYTES, 0);
        for (size_t i = 0; i < texs.size(); ++i)
        {
            const Tex& t = texs[i];
            const size_t at = SLJ2CMigrate::ENTRIES_INFO_BYTES + i * SLJ2CMigrate::ENTRY_BYTES;
            memcpy(entries.data() + at, t.id.mData, 16);
            const S32 body = t.prefix.size() > (size_t)SLJ2CMigrate::STRIPE_BYTES ? (S32)t.prefix.size() - SLJ2CMigrate::STRIPE_BYTES : 0;
            put32(entries, at + 16, t.free_slot ? (U32)-1 : (U32)t.image_size);
            put32(entries, at + 20, t.free_slot ? 0 : (U32)body);
            put32(entries, at + 24, t.time);
            if (t.free_slot) continue;
            memcpy(stripes.data() + i * SLJ2CMigrate::STRIPE_BYTES, t.prefix.data(),
                   std::min(t.prefix.size(), (size_t)SLJ2CMigrate::STRIPE_BYTES));
            if (body > 0)
            {
                const std::string s = t.id.asString();
                std::ofstream out(dir + "/" + s.substr(0, 1) + "/" + s + ".texture", std::ios::binary);
                out.write((const char*)t.prefix.data() + SLJ2CMigrate::STRIPE_BYTES, body);
            }
        }
        { std::ofstream out(dir + "/texture.entries", std::ios::binary); out.write((const char*)entries.data(), entries.size()); }
        { std::ofstream out(dir + "/texture.cache", std::ios::binary); out.write((const char*)stripes.data(), stripes.size()); }
    }

    std::string bodyPath(const std::string& dir, const LLUUID& id)
    {
        const std::string s = id.asString();
        return dir + "/" + s.substr(0, 1) + "/" + s + ".texture";
    }

    Tex makeTex(U32 n, size_t bytes, bool complete = true)
    {
        Tex t;
        t.id = idFor(n);
        t.prefix = codestream(n, bytes);
        t.image_size = complete ? (S32)bytes : (S32)bytes * 4;
        t.time = 1000000 + n;
        return t;
    }

    LLCache::Store::Config storeConfig(const std::string& dir)
    {
        LLCache::Store::Config c;
        c.dir = dir;
        c.read_threads = 1;
        c.run_flush_ms = 20;
        c.detect_drive = false;
        c.snapshot_interval_s = 0;
        c.free_space = [] { return (S64)(1ull << 40); };
        return c;
    }

    SLJ2CMigrate::Options fastOptions()
    {
        SLJ2CMigrate::Options o;
        o.max_read_bytes_per_sec = 0;
        o.max_files_per_sec = 0;
        o.progress_log_interval_s = 0.0;
        return o;
    }

    bool exists(const std::string& p)
    {
        std::error_code ec;
        return fs::exists(p, ec);
    }

    void checkRecord(const LLCache::Store& cstore, const Tex& t, const char* what)
    {
        LLCache::Store& store = const_cast<LLCache::Store&>(cstore);
        LLCache::ReadResult rr = store.readSync(LLJ2CRecord::makeKey(t.id));
        tut::ensure(std::string(what) + ": present", rr.status == LLCache::Status::OK);
        tut::ensure(std::string(what) + ": bytes", rr.bytes == t.prefix);
        S32 isz = 0;
        tut::ensure(std::string(what) + ": meta", LLJ2CRecord::imageSizeFromRecord(rr.info, isz) && isz == t.image_size);
        tut::ensure(std::string(what) + ": raw (codec 0)", rr.info.codec == 0);
    }
}

namespace tut
{
    struct j2cmigrate_data {};
    typedef test_group<j2cmigrate_data> j2cmigrate_test;
    typedef j2cmigrate_test::object j2cmigrate_object;
    tut::j2cmigrate_test j2cmigrate("SLJ2CMigrate");

    // Record meta round trip.
    template<> template<>
    void j2cmigrate_object::test<1>()
    {
        U8 meta[LLJ2CRecord::META_BYTES];
        ensure_equals("len", LLJ2CRecord::packMeta(123457, meta), LLJ2CRecord::META_BYTES);
        S32 v = 0;
        ensure("unpack", LLJ2CRecord::unpackMeta(meta, LLJ2CRecord::META_BYTES, v) && v == 123457);
        meta[0] = 9;
        ensure("other version refused", !LLJ2CRecord::unpackMeta(meta, LLJ2CRecord::META_BYTES, v));
        ensure("short refused", !LLJ2CRecord::unpackMeta(meta, 2, v));
        ensure_equals("disk bytes aligned", LLJ2CRecord::diskBytes(1) % 16, 0ull);
    }

    // Full conversion: small (stripe only) and bodied textures, incomplete prefixes, a
    // free slot, a bad body; newest first; legacy folder removed at the end.
    template<> template<>
    void j2cmigrate_object::test<2>()
    {
        TempDir root;
        const std::string ldir = root.sub("texturecache");
        std::vector<Tex> texs;
        texs.push_back(makeTex(1, 300));            // smaller than a stripe
        texs.push_back(makeTex(2, 600));            // exactly one stripe
        texs.push_back(makeTex(3, 5000));           // stripe + body
        texs.push_back(makeTex(4, 20000, false));   // partial prefix (image size larger)
        Tex freed = makeTex(5, 900); freed.free_slot = true;
        texs.push_back(freed);
        Tex badbody = makeTex(6, 3000);
        texs.push_back(badbody);
        for (U32 i = 0; i < 30; ++i) texs.push_back(makeTex(100 + i, 700 + i * 333, (i & 1) != 0));
        writeLegacy(ldir, texs);
        // corrupt one body: wrong size
        { std::ofstream out(bodyPath(ldir, badbody.id), std::ios::binary | std::ios::trunc); out << "short"; }
        ensure("has work", SLJ2CMigrate::hasWork(ldir));

        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        SLJ2CMigrate::Legacy legacy;
        ensure("legacy open", legacy.open(ldir, fastOptions()));
        ensure_equals("live entries", legacy.liveCount(), (U32)texs.size() - 1);
        const std::vector<SLJ2CMigrate::Legacy::Entry> order = legacy.liveEntriesNewestFirst();
        ensure("newest first", order.front().time >= order.back().time && order.front().id == idFor(129));

        const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, fastOptions());
        ensure_equals("entries", r.entries, (U32)texs.size() - 1);
        ensure_equals("converted", r.converted, (U32)texs.size() - 2);
        ensure_equals("bad", r.bad, 1u);
        ensure_equals("failed", r.failed, 0u);
        ensure("complete", r.completed);
        ensure("folder removed", !exists(ldir));
        ensure("no more work", !SLJ2CMigrate::hasWork(ldir));
        for (const Tex& t : texs)
        {
            if (t.free_slot || t.id == badbody.id) continue;
            checkRecord(store, t, "converted");
        }
        ensure("free slot not imported", !store.exists(LLJ2CRecord::makeKey(freed.id)));
        ensure("bad body not imported", !store.exists(LLJ2CRecord::makeKey(badbody.id)));
        LLCache::ReadResult part = store.readRangeSync(LLJ2CRecord::makeKey(texs[3].id), 600, 1000);
        ensure("range read", part.status == LLCache::Status::OK && part.bytes.size() == 1000
                             && memcmp(part.bytes.data(), texs[3].prefix.data() + 600, 1000) == 0);
        ensure_equals("verifyAll", store.verifyAll(nullptr), 0u);
    }

    // Legacy files are dropped only after a confirmed store write: with every append
    // failing nothing is dropped; the next run (healthy disk) resumes and completes.
    // Free marks persist in texture.entries (a reopened Legacy sees them).
    template<> template<>
    void j2cmigrate_object::test<3>()
    {
        TempDir root;
        const std::string ldir = root.sub("texturecache");
        std::vector<Tex> texs;
        for (U32 i = 0; i < 12; ++i) texs.push_back(makeTex(200 + i, 1500 + i * 100));
        writeLegacy(ldir, texs);
        {
            LLCache::Store::Config c = storeConfig(root.sub("store_bad"));
            c.write_fault = [](U32, U64, size_t) -> size_t { return 0; };   // the disk takes nothing
            LLCache::Store bad;
            ensure("open faulty", bad.open(c));
            SLJ2CMigrate::Legacy legacy;
            ensure("legacy open", legacy.open(ldir, fastOptions()));
            const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, bad, fastOptions());
            ensure("not complete", !r.completed);
            ensure_equals("nothing converted", r.converted, 0u);
            ensure_equals("all failed", r.failed + r.left, 12u);
        }
        for (const Tex& t : texs) ensure("body kept after failed write", exists(bodyPath(ldir, t.id)));
        {
            SLJ2CMigrate::Legacy legacy;
            ensure("reopen", legacy.open(ldir, fastOptions()));
            ensure_equals("still all live", legacy.liveCount(), 12u);
            // Drop three by hand (as after confirmed writes), close, reopen: the marks are on disk.
            const auto v = legacy.liveEntriesNewestFirst();
            for (int i = 0; i < 3; ++i) legacy.drop(v[i]);
            legacy.close();
            SLJ2CMigrate::Legacy again;
            ensure("reopen 2", again.open(ldir, fastOptions()));
            ensure_equals("marks persisted", again.liveCount(), 9u);
            ensure("dropped body gone", !exists(bodyPath(ldir, v[0].id)));
        }
        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        // A crash after the record landed but before its free mark was written: the entry is
        // live in texture.entries and the store has the key -> "already", dropped.
        {
            const Tex& t = texs[5];
            U8 meta[LLJ2CRecord::META_BYTES];
            LLCache::WriteOptions o;
            o.meta = meta;
            o.meta_len = LLJ2CRecord::packMeta(t.image_size, meta);
            store.write(LLJ2CRecord::makeKey(t.id), LLCache::WriteSource::copy(t.prefix.data(), t.prefix.size()), o);
            store.flush();
        }
        SLJ2CMigrate::Legacy legacy;
        ensure("legacy open", legacy.open(ldir, fastOptions()));
        const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, fastOptions());
        ensure_equals("already", r.already, 1u);
        ensure_equals("resumed", r.converted, 8u);
        ensure("complete", r.completed);
        ensure("folder removed", !exists(ldir));
    }

    // Store already longer -> kept; store shorter -> replaced by the longer legacy prefix;
    // the budget cut drops the rest; a stop leaves entries; a foreign version drops all.
    template<> template<>
    void j2cmigrate_object::test<4>()
    {
        TempDir root;
        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        auto put = [&](const LLUUID& id, const std::vector<U8>& bytes, S32 isz)
        {
            U8 meta[LLJ2CRecord::META_BYTES];
            LLCache::WriteOptions o;
            o.meta = meta;
            o.meta_len = LLJ2CRecord::packMeta(isz, meta);
            store.write(LLJ2CRecord::makeKey(id), LLCache::WriteSource::copy(bytes.data(), bytes.size()), o);
            store.flush();
        };
        {
            TempDir d;
            const std::string ldir = d.sub("tc");
            Tex longer_in_store = makeTex(300, 2000, false);
            Tex shorter_in_store = makeTex(301, 8000);
            writeLegacy(ldir, { longer_in_store, shorter_in_store });
            const std::vector<U8> big = codestream(300, 6000);
            put(longer_in_store.id, big, 8000);
            put(shorter_in_store.id, std::vector<U8>(shorter_in_store.prefix.begin(), shorter_in_store.prefix.begin() + 600), 8000);
            SLJ2CMigrate::Legacy legacy;
            ensure("legacy open", legacy.open(ldir, fastOptions()));
            const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, fastOptions());
            ensure_equals("already", r.already, 1u);
            ensure_equals("converted", r.converted, 1u);
            LLCache::ReadResult a = store.readSync(LLJ2CRecord::makeKey(longer_in_store.id));
            ensure("store's longer kept", a.status == LLCache::Status::OK && a.bytes == big);
            checkRecord(store, shorter_in_store, "longer legacy won");
            ensure("complete", r.completed);
        }
        {
            TempDir d;
            const std::string ldir = d.sub("tc");
            std::vector<Tex> texs;
            for (U32 i = 0; i < 8; ++i) texs.push_back(makeTex(400 + i, 30000));
            writeLegacy(ldir, texs);
            SLJ2CMigrate::Options o = fastOptions();
            o.live_cap_bytes = 1;   // the store already holds more than this
            SLJ2CMigrate::Legacy legacy;
            ensure("legacy open", legacy.open(ldir, o));
            const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, o);
            ensure_equals("all over budget", r.over_budget, 8u);
            ensure("complete", r.completed);
            ensure("folder removed", !exists(ldir));
            ensure("not imported", !store.exists(LLJ2CRecord::makeKey(texs[0].id)));
        }
        {
            TempDir d;
            const std::string ldir = d.sub("tc");
            std::vector<Tex> texs;
            for (U32 i = 0; i < 5; ++i) texs.push_back(makeTex(500 + i, 1200));
            writeLegacy(ldir, texs);
            std::atomic<bool> stop{ true };
            SLJ2CMigrate::Options o = fastOptions();
            o.stop = &stop;
            {
                SLJ2CMigrate::Legacy legacy;
                ensure("legacy open", legacy.open(ldir, o));
                const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, o);
                ensure("stopped", r.stopped && !r.completed);
                ensure_equals("left", r.left, 5u);
            }
            ensure("still work", SLJ2CMigrate::hasWork(ldir));
            stop = false;
            SLJ2CMigrate::Legacy legacy;
            ensure("legacy open", legacy.open(ldir, o));
            const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, o);
            ensure_equals("resumed", r.converted, 5u);
            ensure("complete", r.completed);
        }
        {
            TempDir d;
            const std::string ldir = d.sub("tc");
            Tex t = makeTex(600, 4000);
            writeLegacy(ldir, { t }, 1.70f);
            SLJ2CMigrate::Legacy legacy;
            ensure("legacy open", legacy.open(ldir, fastOptions()));
            ensure("mismatch", legacy.versionMismatch());
            const SLJ2CMigrate::Result r = SLJ2CMigrate::migrate(legacy, store, fastOptions());
            ensure("dropped", r.version_mismatch && r.completed);
            ensure("folder removed", !exists(ldir));
            ensure("not imported", !store.exists(LLJ2CRecord::makeKey(t.id)));
        }
    }

    // Read-through serves an unconverted entry; forget() drops its files so it never
    // comes back.
    template<> template<>
    void j2cmigrate_object::test<5>()
    {
        TempDir root;
        const std::string ldir = root.sub("texturecache");
        Tex a = makeTex(700, 4321, false);
        Tex b = makeTex(701, 450);
        writeLegacy(ldir, { a, b });
        SLJ2CMigrate::Legacy legacy;
        ensure("legacy open", legacy.open(ldir, fastOptions()));
        std::vector<U8> out;
        S32 isz = 0;
        ensure("read a", legacy.read(a.id, out, isz) && out == a.prefix && isz == a.image_size);
        ensure("read b", legacy.read(b.id, out, isz) && out == b.prefix && isz == b.image_size);
        ensure("unknown", !legacy.read(idFor(999), out, isz));
        legacy.forget(a.id);
        ensure("forgotten", !legacy.read(a.id, out, isz));
        ensure("body deleted", !exists(bodyPath(ldir, a.id)));
        legacy.close();
        SLJ2CMigrate::Legacy again;
        ensure("reopen", again.open(ldir, fastOptions()));
        ensure_equals("forget persisted", again.liveCount(), 1u);
    }
}
