/**
 * @file slassetmigrate_test.cpp
 * @brief The old asset folder -> segment store converter (#714 / #715): names, the mesh
 *        part split, conversion, deletion only after a confirmed write, foreign files left
 *        alone, the budget cut, failure keeps the file, and resume after a stop.
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

#include "../slassetmigrate.h"

#include "llcachestore_platform.h"
#include "llsd.h"
#include "llsdserialize.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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
            path = (base / ("slassetmigrate_test_" + std::to_string(LLCacheStorePlatform::processId()) + "_" +
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

    std::vector<U8> bytesFor(U32 seed, size_t n)
    {
        std::vector<U8> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (U8)(i * 31 + seed * 7 + 1);
        return v;
    }

    void writeFile(const std::string& path, const std::vector<U8>& b)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
    }

    bool exists(const std::string& p)
    {
        std::error_code ec;
        return fs::exists(p, ec);
    }

    void putU32(std::vector<U8>& v, size_t at, U32 x)
    {
        for (int i = 0; i < 4; ++i) v[at + i] = (U8)(x >> (8 * i));
    }

    // An old mesh cache file: preamble {1, header size, flags} + binary LLSD header + blocks
    // at 12 + header size + offset (a sparse gap reads as zeros).
    struct OldMesh
    {
        std::vector<U8> file;
        std::vector<U8> header;
        std::vector<U8> lod0, lod1, lod3, skin;
    };
    OldMesh makeOldMesh(U32 seed, U32 flags, bool lod1_unwritten)
    {
        OldMesh m;
        m.lod0 = bytesFor(seed, 100);
        m.lod1 = bytesFor(seed + 1, 200);
        m.lod3 = bytesFor(seed + 3, 50);
        m.skin = bytesFor(seed + 5, 40);
        LLSD h;
        h["version"] = 1;
        auto blk = [](S32 off, S32 sz) { LLSD b; b["offset"] = off; b["size"] = sz; return b; };
        h["lowest_lod"] = blk(0, 100);
        h["low_lod"] = blk(100, 200);
        h["high_lod"] = blk(300, 50);
        h["skin"] = blk(350, 40);
        std::ostringstream os;
        LLSDSerialize::toBinary(h, os);
        const std::string hs = os.str();
        m.header.assign(hs.begin(), hs.end());
        const size_t hsz = m.header.size();
        m.file.assign(12 + hsz + 390, 0);
        putU32(m.file, 0, 1);
        putU32(m.file, 4, (U32)hsz);
        putU32(m.file, 8, flags);
        memcpy(m.file.data() + 12, m.header.data(), hsz);
        const size_t base = 12 + hsz;
        memcpy(m.file.data() + base + 0, m.lod0.data(), 100);
        if (!lod1_unwritten) memcpy(m.file.data() + base + 100, m.lod1.data(), 200);
        memcpy(m.file.data() + base + 300, m.lod3.data(), 50);
        memcpy(m.file.data() + base + 350, m.skin.data(), 40);
        return m;
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

    SLCacheMigrate::Options fastOptions()
    {
        SLCacheMigrate::Options o;
        o.max_read_bytes_per_sec = 0;
        o.max_files_per_sec = 0;
        o.progress_log_interval_s = 0.0;
        return o;
    }

    LLCache::Key assetKey(const LLUUID& id) { return LLCache::Key(id, LLCache::Kind::ASSET, 0); }
}

namespace tut
{
    struct assetmigrate_data {};
    typedef test_group<assetmigrate_data> assetmigrate_test;
    typedef assetmigrate_test::object assetmigrate_object;
    tut::assetmigrate_test assetmigrate("SLAssetMigrate");

    // Names round-trip; anything else is not ours.
    template<> template<>
    void assetmigrate_object::test<1>()
    {
        const LLUUID id = idFor(7);
        LLUUID back;
        ensure("parse", SLAssetMigrate::parseName(SLAssetMigrate::makeName(id), back) && back == id);
        ensure("upper-case refused", !SLAssetMigrate::parseName("SL_CACHE_" + id.asString() + "_0.asset", back));
        ensure("other suffix refused", !SLAssetMigrate::parseName("sl_cache_" + id.asString() + "_1.asset", back));
        ensure("part refused", !SLAssetMigrate::parseName(SLAssetMigrate::makeName(id) + ".part", back));
        ensure("garbage refused", !SLAssetMigrate::parseName("sl_cache_hello_0.asset", back));
        ensure("flags", SLMeshParts::partFlag(SLMeshParts::LOD0) == 1 && SLMeshParts::partFlag(SLMeshParts::LOD3) == 8
                            && SLMeshParts::partFlag(SLMeshParts::SKIN) == 16 && SLMeshParts::partFlag(SLMeshParts::PHYS_MESH) == 64);
    }

    // The mesh split keeps exactly the header and the flagged, written blocks.
    template<> template<>
    void assetmigrate_object::test<2>()
    {
        const LLUUID id = idFor(11);
        const U32 flags = SLMeshParts::partFlag(SLMeshParts::LOD0) | SLMeshParts::partFlag(SLMeshParts::LOD1)
                        | SLMeshParts::partFlag(SLMeshParts::SKIN);   // high LOD not flagged
        const OldMesh m = makeOldMesh(3, flags, true);                // LOD1 flagged but never written
        SLMeshParts::Split s;
        ensure("is a mesh", SLMeshParts::splitOldMeshFile(id, m.file.data(), m.file.size(), s));
        ensure_equals("header size", s.header_size, (S32)m.header.size());
        ensure_equals("records", s.records.size(), (size_t)3);   // header, lod0, skin
        ensure("header first", s.records[0].key == SLMeshParts::key(id, SLMeshParts::HEADER) && s.records[0].payload == m.header);
        ensure("lod0", s.records[1].key == SLMeshParts::key(id, SLMeshParts::LOD0) && s.records[1].payload == m.lod0);
        ensure("skin", s.records[2].key == SLMeshParts::key(id, SLMeshParts::SKIN) && s.records[2].payload == m.skin);
        ensure_equals("kept flags", s.flags, SLMeshParts::partFlag(SLMeshParts::LOD0) | SLMeshParts::partFlag(SLMeshParts::SKIN));

        // An animation starts with U16 1, U16 0 (= U32 1 too): not a mesh.
        std::vector<U8> anim = bytesFor(9, 300);
        putU32(anim, 0, 1);
        putU32(anim, 4, 4);
        ensure("anim is not a mesh", !SLMeshParts::splitOldMeshFile(id, anim.data(), anim.size(), s));
        std::vector<U8> tiny(5, 1);
        ensure("tiny is not a mesh", !SLMeshParts::splitOldMeshFile(id, tiny.data(), tiny.size(), s));
    }

    // Conversion: assets and meshes land, files go, foreign files stay, empty files are bad.
    template<> template<>
    void assetmigrate_object::test<3>()
    {
        TempDir root;
        TempDir adir;
        std::vector<std::pair<LLUUID, std::vector<U8>>> assets;
        for (U32 i = 0; i < 12; ++i)
        {
            assets.emplace_back(idFor(100 + i), bytesFor(i, 500 + i * 37));
            writeFile(adir.sub(SLAssetMigrate::makeName(assets.back().first)), assets.back().second);
        }
        const LLUUID mesh_id = idFor(300);
        const U32 all = SLMeshParts::partFlag(SLMeshParts::LOD0) | SLMeshParts::partFlag(SLMeshParts::LOD1)
                      | SLMeshParts::partFlag(SLMeshParts::LOD3) | SLMeshParts::partFlag(SLMeshParts::SKIN);
        const OldMesh m = makeOldMesh(5, all, false);
        writeFile(adir.sub(SLAssetMigrate::makeName(mesh_id)), m.file);
        const LLUUID empty_id = idFor(301);
        writeFile(adir.sub(SLAssetMigrate::makeName(empty_id)), {});
        writeFile(adir.sub("not_ours.txt"), bytesFor(1, 10));
        writeFile(adir.sub("sl_cache_hello_0.asset"), bytesFor(2, 10));

        SLAssetMigrate::Converter conv;
        ensure("has work", SLCacheMigrate::hasWork(adir.path, conv));
        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        const SLCacheMigrate::Result r = SLCacheMigrate::migrateDir(adir.path, store, conv, fastOptions());
        ensure_equals("files", r.files, 14u);
        ensure_equals("converted", r.converted, 13u);
        ensure_equals("records", r.records, 12u + 5u);   // 12 assets + mesh header, lod0, lod1, lod3, skin
        ensure_equals("bad (empty)", r.bad, 1u);
        ensure_equals("failed", r.failed, 0u);
        ensure("complete", r.completed);
        ensure("foreign file kept", exists(adir.sub("not_ours.txt")));
        ensure("unparseable name kept", exists(adir.sub("sl_cache_hello_0.asset")));
        ensure("folder kept (not empty)", exists(adir.path));
        ensure("no more work", !SLCacheMigrate::hasWork(adir.path, conv));

        for (const auto& a : assets)
        {
            LLCache::ReadResult rr = store.readSync(assetKey(a.first));
            ensure("asset present", rr.status == LLCache::Status::OK && rr.bytes == a.second);
            ensure("asset meta", rr.info.meta_len == 2 && rr.info.meta[0] == 1 && rr.info.meta[1] == 0xFF);
            ensure("file gone", !exists(adir.sub(SLAssetMigrate::makeName(a.first))));
        }
        ensure("mesh is not an asset", !store.exists(assetKey(mesh_id)));
        LLCache::ReadResult h = store.readSync(SLMeshParts::key(mesh_id, SLMeshParts::HEADER));
        ensure("mesh header", h.status == LLCache::Status::OK && h.bytes == m.header);
        ensure("mesh lod3", store.readSync(SLMeshParts::key(mesh_id, SLMeshParts::LOD3)).bytes == m.lod3);
        ensure("mesh skin", store.readSync(SLMeshParts::key(mesh_id, SLMeshParts::SKIN)).bytes == m.skin);
        ensure("no physics", !store.exists(SLMeshParts::key(mesh_id, SLMeshParts::PHYS_CONVEX)));
        ensure("empty not imported", !store.exists(assetKey(empty_id)));
        ensure_equals("verifyAll", store.verifyAll(nullptr), 0u);
    }

    // Already in the store (fetched this session) -> file deleted, store copy kept; the
    // budget cut deletes the rest; a failed write keeps the file and takes its records back.
    template<> template<>
    void assetmigrate_object::test<4>()
    {
        TempDir root;
        SLAssetMigrate::Converter conv;
        {
            LLCache::Store store;
            ensure("open", store.open(storeConfig(root.sub("store"))));
            TempDir adir;
            const LLUUID a = idFor(400), m = idFor(401);
            writeFile(adir.sub(SLAssetMigrate::makeName(a)), bytesFor(1, 100));
            writeFile(adir.sub(SLAssetMigrate::makeName(m)), makeOldMesh(2, 1, false).file);
            const std::vector<U8> newer(77, 9);
            store.write(assetKey(a), LLCache::WriteSource::copy(newer.data(), newer.size()));
            store.write(SLMeshParts::key(m, SLMeshParts::HEADER), LLCache::WriteSource::copy(newer.data(), newer.size()));
            store.flush();
            const SLCacheMigrate::Result r = SLCacheMigrate::migrateDir(adir.path, store, conv, fastOptions());
            ensure_equals("already", r.already, 2u);
            ensure_equals("converted", r.converted, 0u);
            ensure("store copy kept", store.readSync(assetKey(a)).bytes == newer);
            ensure("folder removed", !exists(adir.path));

            TempDir bdir;
            for (U32 i = 0; i < 8; ++i) writeFile(bdir.sub(SLAssetMigrate::makeName(idFor(500 + i))), bytesFor(i, 4000));
            SLCacheMigrate::Options o = fastOptions();
            o.live_cap_bytes = 1;   // the store already holds more
            const SLCacheMigrate::Result r2 = SLCacheMigrate::migrateDir(bdir.path, store, conv, o);
            ensure_equals("over budget", r2.over_budget, 8u);
            ensure("complete", r2.completed);
        }
        {
            // Every append fails: nothing is deleted, nothing is left in the store.
            LLCache::Store::Config c = storeConfig(root.sub("store2"));
            c.write_fault = [](U32, U64, size_t) -> size_t { return 0; };
            LLCache::Store store;
            ensure("open faulty", store.open(c));
            TempDir adir;
            const LLUUID a = idFor(600), m = idFor(601);
            writeFile(adir.sub(SLAssetMigrate::makeName(a)), bytesFor(3, 1000));
            writeFile(adir.sub(SLAssetMigrate::makeName(m)), makeOldMesh(4, 3, false).file);
            const SLCacheMigrate::Result r = SLCacheMigrate::migrateDir(adir.path, store, conv, fastOptions());
            ensure_equals("failed", r.failed, 2u);
            ensure_equals("converted", r.converted, 0u);
            ensure("not complete", !r.completed);
            ensure("asset file kept", exists(adir.sub(SLAssetMigrate::makeName(a))));
            ensure("mesh file kept", exists(adir.sub(SLAssetMigrate::makeName(m))));
            ensure("asset not in store", !store.exists(assetKey(a)));
            ensure("mesh header not in store", !store.exists(SLMeshParts::key(m, SLMeshParts::HEADER)));
        }
    }

    // Resume: a stop after 4 files leaves exactly the other 6; the next run converts them,
    // newest first both times, and every record is there at the end.
    template<> template<>
    void assetmigrate_object::test<5>()
    {
        TempDir root;
        TempDir adir;
        SLAssetMigrate::Converter conv;
        std::vector<LLUUID> ids;
        const auto now = fs::file_time_type::clock::now();
        for (U32 i = 0; i < 10; ++i)
        {
            ids.push_back(idFor(700 + i));
            const std::string p = adir.sub(SLAssetMigrate::makeName(ids.back()));
            writeFile(p, bytesFor(i, 2000 + i));
            std::error_code ec;
            fs::last_write_time(p, now - std::chrono::hours(10 - i), ec);   // ids[9] is the newest
        }
        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        SLCacheMigrate::Options o = fastOptions();
        o.stop_after_files = 4;
        SLCacheMigrate::Result r = SLCacheMigrate::migrateDir(adir.path, store, conv, o);
        ensure("stopped", r.stopped && !r.completed);
        ensure_equals("first run converted", r.converted, 4u);
        ensure_equals("left", r.left, 6u);
        for (U32 i = 0; i < 10; ++i)
        {
            const bool newest4 = i >= 6;
            ensure("newest converted first", store.exists(assetKey(ids[i])) == newest4);
            ensure("only converted files deleted", exists(adir.sub(SLAssetMigrate::makeName(ids[i]))) == !newest4);
        }

        // A crash between "record landed" and "file deleted": the file is still there, the
        // next run finds the key and deletes it without importing it again.
        writeFile(adir.sub(SLAssetMigrate::makeName(ids[9])), bytesFor(9, 2009));

        o.stop_after_files = 0;
        r = SLCacheMigrate::migrateDir(adir.path, store, conv, o);
        ensure_equals("resumed", r.converted, 6u);
        ensure_equals("already", r.already, 1u);
        ensure("complete", r.completed);
        ensure("folder removed", !exists(adir.path));
        for (U32 i = 0; i < 10; ++i)
        {
            LLCache::ReadResult rr = store.readSync(assetKey(ids[i]));
            ensure("all present", rr.status == LLCache::Status::OK && rr.bytes == bytesFor(i, 2000 + i));
        }
    }
}
