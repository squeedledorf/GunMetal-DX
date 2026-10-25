/**
 * @file llfilesystemstore_test.cpp
 * @brief LLFileSystem on the segment store (#715): the download flow (write temp, rename),
 *        one APPEND object per packet, read-your-writes, seek/read, remove, the idle grace
 *        commit, the old-file read-through import and the commit at detach.
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

#include "../llfilesystem.h"
#include "../llfilesystemstore.h"
#include "../lldiskcache.h"

#include "llcachestore_platform.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
            path = (fs::temp_directory_path(ec) / ("llfilesystemstore_test_" + std::to_string(LLCacheStorePlatform::processId())
                                                  + "_" + std::to_string(sDirCounter++))).string();
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
        std::string sub(const std::string& n) const { return path + "/" + n; }
    };

    std::vector<U8> bytesFor(U32 seed, size_t n)
    {
        std::vector<U8> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (U8)(i * 13 + seed + 1);
        return v;
    }

    LLUUID idFor(U32 n)
    {
        LLUUID id;
        id.generate(std::to_string(n) + "-llfilesystemstore");
        return id;
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

    std::vector<U8> readAll(const LLUUID& id, LLAssetType::EType t)
    {
        LLFileSystem f(id, t, LLFileSystem::READ);
        std::vector<U8> out((size_t)f.getSize());
        if (!out.empty()) f.read(out.data(), (S32)out.size());
        return out;
    }

    // LLDiskCache is a singleton: one old-asset folder for the whole run.
    TempDir& legacyDir()
    {
        static TempDir sDir;
        static bool sInit = false;
        if (!sInit)
        {
            LLDiskCache::initParamSingleton(sDir.path, 1ull << 30, false);
            sInit = true;
        }
        return sDir;
    }

    struct Fixture
    {
        TempDir root;
        LLCache::Store store;
        Fixture() { legacyDir(); }
    };
}

namespace tut
{
    struct fsstore_data {};
    typedef test_group<fsstore_data> fsstore_test;
    typedef fsstore_test::object fsstore_object;
    tut::fsstore_test fsstore("LLFileSystemStore");

    // The HTTP download flow and the UDP / xfer flow (one APPEND object per packet), both
    // committed by rename and readable at once (before the record reaches the disk).
    template<> template<>
    void fsstore_object::test<1>()
    {
        Fixture fx;
        ensure("open", fx.store.open(storeConfig(fx.root.sub("store"))));
        LLFileSystemStore::Options o;
        LLFileSystemStore::attach(&fx.store, o);
        ensure("active", LLFileSystemStore::active());

        const LLUUID temp = idFor(1), asset = idFor(2);
        const std::vector<U8> body = bytesFor(1, 5000);
        {
            LLFileSystem vf(temp, LLAssetType::AT_NOTECARD, LLFileSystem::WRITE);
            ensure("write", vf.write(body.data(), (S32)body.size()));
            ensure("staged size", LLFileSystem::getFileSize(temp, LLAssetType::AT_NOTECARD) == (S32)body.size());
            ensure("rename", vf.rename(asset, LLAssetType::AT_NOTECARD));
        }
        ensure("temp gone", !LLFileSystem::getExists(temp, LLAssetType::AT_NOTECARD));
        ensure("asset exists", LLFileSystem::getExists(asset, LLAssetType::AT_NOTECARD));
        ensure("read back", readAll(asset, LLAssetType::AT_NOTECARD) == body);
        LLCache::RecordInfo ri;
        ensure("one record", fx.store.lookup(LLFileSystemStore::key(asset), &ri) && ri.raw_bytes == body.size());
        ensure("type in meta", ri.meta_len == 2 && ri.meta[0] == LLFileSystemStore::META_VERSION
                                && (S8)ri.meta[1] == (S8)LLAssetType::AT_NOTECARD);

        // One APPEND object per packet.
        const LLUUID temp2 = idFor(3), asset2 = idFor(4);
        const std::vector<U8> sound = bytesFor(2, 3000);
        for (size_t at = 0; at < sound.size(); at += 1000)
        {
            LLFileSystem pkt(temp2, LLAssetType::AT_SOUND, LLFileSystem::APPEND);
            ensure("append", pkt.write(sound.data() + at, 1000));
            ensure_equals("half received", LLFileSystem::getFileSize(temp2, LLAssetType::AT_SOUND), (S32)(at + 1000));
        }
        ensure("not committed yet", !fx.store.exists(LLFileSystemStore::key(temp2)));
        LLFileSystem::renameFile(temp2, LLAssetType::AT_SOUND, asset2, LLAssetType::AT_SOUND);
        ensure("appended asset", readAll(asset2, LLAssetType::AT_SOUND) == sound);

        // Seek + partial reads (the Ogg decoder's callbacks).
        {
            LLFileSystem f(asset2, LLAssetType::AT_SOUND);
            ensure("seek", f.seek(1500, 0));
            U8 buf[600];
            ensure("read", f.read(buf, 600) && f.getLastBytesRead() == 600);
            ensure("bytes", memcmp(buf, sound.data() + 1500, 600) == 0);
            ensure_equals("tell", f.tell(), 2100);
            ensure("seek end", f.seek(0, (S32)sound.size()) && f.eof());
        }

        fx.store.flush();
        ensure("on disk", fx.store.readSync(LLFileSystemStore::key(asset2)).bytes == sound);

        LLFileSystem::removeFile(asset, LLAssetType::AT_NOTECARD);
        ensure("removed", !LLFileSystem::getExists(asset, LLAssetType::AT_NOTECARD));
        ensure("missing reads empty", readAll(asset, LLAssetType::AT_NOTECARD).empty());
        LLFileSystemStore::detach();
        ensure("detached", !LLFileSystemStore::active());
    }

    // APPEND-only staging with no rename commits after the idle grace; a WRITE object
    // commits when it closes; detach commits the rest; an old file is read through and
    // imported.
    template<> template<>
    void fsstore_object::test<2>()
    {
        Fixture fx;
        ensure("open", fx.store.open(storeConfig(fx.root.sub("store"))));
        LLFileSystemStore::Options o;
        o.commit_grace_s = 0.05f;
        o.legacy_fallback = true;
        LLFileSystemStore::attach(&fx.store, o);

        const LLUUID up = idFor(10);
        const std::vector<U8> gesture = bytesFor(3, 800);
        {
            LLFileSystem f(up, LLAssetType::AT_GESTURE, LLFileSystem::APPEND);
            f.write(gesture.data(), (S32)gesture.size());
        }
        ensure("readable while staged", readAll(up, LLAssetType::AT_GESTURE) == gesture);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        LLFileSystemStore::pump();
        ensure("committed after the grace", fx.store.exists(LLFileSystemStore::key(up)));
        ensure_equals("nothing staged", LLFileSystemStore::stats().staged, 0u);

        const LLUUID w = idFor(11);
        {
            LLFileSystem f(w, LLAssetType::AT_GLTF, LLFileSystem::WRITE);
            f.write(gesture.data(), 100);
        }
        ensure("WRITE commits on close", fx.store.exists(LLFileSystemStore::key(w)));

        // Old file, not migrated yet.
        const LLUUID old = idFor(12);
        const std::vector<U8> anim = bytesFor(4, 1234);
        {
            std::ofstream out(LLFileSystemStore::legacyPath(old), std::ios::binary);
            out.write((const char*)anim.data(), (std::streamsize)anim.size());
        }
        ensure("old file exists", LLFileSystem::getExists(old, LLAssetType::AT_ANIMATION));
        ensure("old file read", readAll(old, LLAssetType::AT_ANIMATION) == anim);
        ensure("imported", fx.store.exists(LLFileSystemStore::key(old)));

        const LLUUID late = idFor(13);
        {
            LLFileSystem f(late, LLAssetType::AT_NOTECARD, LLFileSystem::APPEND);
            f.write(anim.data(), 10);
        }
        LLFileSystemStore::detach();
        ensure("detach commits", fx.store.exists(LLFileSystemStore::key(late)));
    }
}
