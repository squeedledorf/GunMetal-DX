/**
 * @file slgputexmigrate_test.cpp
 * @brief The .gtx -> segment store converter (#714): validation, conversion, deletion
 *        after commit, the budget cut, resumability and the folder marker.
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

#include "../slgputexmigrate.h"
#include "../llgputexrecord.h"

#include "llcachestore_platform.h"
#include "fsyspath.h"
#include "hbxxh.h"

#include <atomic>
#include <chrono>
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
            path = (base / ("slgputexmigrate_test_" + std::to_string(LLCacheStorePlatform::processId()) + "_" +
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

    std::vector<U8> chain(U32 seed, size_t n)
    {
        std::vector<U8> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (U8)((i / 16) * 3 + seed);   // compressible, like BC blocks
        return v;
    }

    // A .gtx file exactly as the old cache's run_write made it.
    struct Gtx
    {
        SLGpuTexMigrate::ParsedName name;
        LLGpuTexHeader hdr;
        std::vector<U8> payload;
    };

    // A full BC7 mip chain of w x h (#865: the converter writes it per level, so it must
    // be one); bogus_bytes > 0: a payload of that size instead (not a chain).
    Gtx makeGtx(U32 n, bool mask, U16 w, U16 h, size_t bogus_bytes = 0)
    {
        Gtx g;
        g.name.id = idFor(n);
        g.name.mask = mask;
        g.name.w = w; g.name.h = h; g.name.enc = 3; g.name.enc_level = 2; g.name.encoder_version = 7;
        const U32 levels = LLGpuTexRecord::mipCount(w, h);
        g.payload = chain(n, bogus_bytes ? bogus_bytes : (size_t)LLGpuTexRecord::chainBytes(0x91, w, h, levels));
        LLGpuTexHeader& hd = g.hdr;
        hd.magic = LLGpuTexHeader::kMagic;
        hd.version = LLGpuTexCache::FILE_VERSION;
        hd.encoder_version = 7;
        hd.header_bytes = sizeof(LLGpuTexHeader);
        memcpy(hd.id, g.name.id.mData, 16);
        hd.vk_format = 0x91;   // VK_FORMAT_BC7_UNORM_BLOCK
        hd.enc = 3; hd.enc_level = 2; hd.comps = 4;
        hd.flags = mask ? (LLGpuTexHeader::F_ALPHA_MASK | LLGpuTexHeader::F_MASK_VARIANT) : 0;
        hd.w = w; hd.h = h; hd.full_w = (U16)(w * 2); hd.full_h = (U16)(h * 2);
        hd.levels = (U16)levels;
        hd.alpha_grad_pct = 12; hd.cov_cut = 128; hd.alpha_side = 64;
        hd.payload_bytes = g.payload.size();
        hd.payload_hash = HBXXH64::digest(g.payload.data(), g.payload.size());
        hd.header_hash = LLGpuTexRecord::headerHash(hd);
        return g;
    }

    std::string writeGtx(const TempDir& td, const Gtx& g)
    {
        const std::string path = td.sub(SLGpuTexMigrate::makeName(g.name));
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&g.hdr), sizeof(g.hdr));
        out.write(reinterpret_cast<const char*>(g.payload.data()), (std::streamsize)g.payload.size());
        return path;
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

    SLGpuTexMigrate::Options fastOptions()
    {
        SLGpuTexMigrate::Options o;
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

    // The store's chain for (id, mask), every level read and concatenated (level 0 first).
    bool readChain(LLCache::Store& store, const LLUUID& id, bool mask, LLGpuTexRecord::Chain& c, std::vector<U8>& out)
    {
        LLGpuTexRecord::ReadPlan p;
        if (!LLGpuTexRecord::resolve(store, id, mask, c) || !LLGpuTexRecord::planRead(c, 0, 0, p)) return false;
        std::vector<LLCache::ReadResult> rr;
        for (size_t i : p.recs) rr.push_back(store.readSync(c.recs[i].key));
        LLGpuTexCache::Blob b;
        size_t bad = 0;
        if (!LLGpuTexRecord::assemble(c, p, rr, b, bad)) return false;
        out.clear();
        for (const auto& pc : b.pieces) out.insert(out.end(), pc.bytes.begin(), pc.bytes.end());
        return true;
    }

    // A chain written straight to the store (a capture of this session).
    void writeChain(LLCache::Store& store, const LLGpuTexHeader& hd, const std::vector<U8>& bytes)
    {
        std::vector<LLGpuTexRecord::PartPlan> parts;
        LLGpuTexRecord::planWrite(hd, bytes.size(), parts);
        LLGpuTexRecord::writeParts(store, parts, bytes.data(), LLCache::WriteOptions(), nullptr);
        store.flush();
    }
}

namespace tut
{
    struct gtxmigrate_data {};
    typedef test_group<gtxmigrate_data> gtxmigrate_test;
    typedef gtxmigrate_test::object gtxmigrate_object;
    tut::gtxmigrate_test gtxmigrate("SLGpuTexMigrate");

    // Names round-trip; non-canonical names are refused (like the old parse_name).
    template<> template<>
    void gtxmigrate_object::test<1>()
    {
        SLGpuTexMigrate::ParsedName n;
        n.id = idFor(5); n.mask = true; n.w = 512; n.h = 256; n.enc = 4; n.enc_level = 1; n.encoder_version = 12;
        const std::string name = SLGpuTexMigrate::makeName(n);
        SLGpuTexMigrate::ParsedName p;
        ensure("parse", SLGpuTexMigrate::parseName(name, p));
        ensure("id", p.id == n.id);
        ensure("mask", p.mask);
        ensure_equals("w", (U32)p.w, 512u);
        ensure_equals("encver", p.encoder_version, 12u);
        ensure("not .gtx", !SLGpuTexMigrate::parseName(name + ".part", p));
        ensure("leading zero refused", !SLGpuTexMigrate::parseName(n.id.asString() + "_0512x256_e4_l1_v12_mask.gtx", p));
        ensure("zero size refused", !SLGpuTexMigrate::parseName(n.id.asString() + "_0x256_e4_l1_v12.gtx", p));
        ensure("garbage refused", !SLGpuTexMigrate::parseName("hello.gtx", p));
    }

    // readFile validates header, name agreement, size and payload hash.
    template<> template<>
    void gtxmigrate_object::test<2>()
    {
        TempDir td;
        Gtx g = makeGtx(1, true, 64, 32);
        const std::string path = writeGtx(td, g);
        SLGpuTexMigrate::Record rec;
        ensure("good file", SLGpuTexMigrate::readFile(path, SLGpuTexMigrate::makeName(g.name), rec) == SLGpuTexMigrate::FileStatus::OK);
        ensure("id", rec.id == g.name.id);
        ensure("mask", rec.mask);
        ensure("payload", rec.payload == g.payload);
        const LLGpuTexHeader& back = rec.hdr;
        ensure("header fields", back.vk_format == 0x91 && back.full_w == 128 && back.levels == 7 && back.cov_cut == 128
                                && back.alpha_side == 64 && back.alpha_grad_pct == 12 && back.encoder_version == 7);

        // Payload corruption.
        Gtx bad = makeGtx(2, false, 64, 32);
        bad.payload[100] ^= 0xFF;   // the header still carries the original hash
        const std::string p2 = writeGtx(td, bad);
        SLGpuTexMigrate::Record r2;
        ensure("bad hash", SLGpuTexMigrate::readFile(p2, SLGpuTexMigrate::makeName(bad.name), r2) == SLGpuTexMigrate::FileStatus::BAD_HASH);

        // Truncated.
        Gtx tr = makeGtx(3, false, 64, 32);
        tr.payload.resize(2000);
        const std::string p3 = writeGtx(td, tr);
        SLGpuTexMigrate::Record r3;
        ensure("truncated", SLGpuTexMigrate::readFile(p3, SLGpuTexMigrate::makeName(tr.name), r3) == SLGpuTexMigrate::FileStatus::BAD_HEADER);

        // #865: a payload that is not the chain its header describes.
        Gtx nc = makeGtx(5, false, 64, 32, 3000);
        const std::string p5 = writeGtx(td, nc);
        SLGpuTexMigrate::Record r5;
        ensure("not a chain", SLGpuTexMigrate::readFile(p5, SLGpuTexMigrate::makeName(nc.name), r5) == SLGpuTexMigrate::FileStatus::NOT_A_CHAIN);

        // Header disagrees with the name (renamed file).
        Gtx mm = makeGtx(4, false, 64, 32);
        writeGtx(td, mm);
        SLGpuTexMigrate::ParsedName other = mm.name;
        other.w = 128;
        const std::string p4 = td.sub(SLGpuTexMigrate::makeName(other));
        fs::rename(td.sub(SLGpuTexMigrate::makeName(mm.name)), p4);
        SLGpuTexMigrate::Record r4;
        ensure("name mismatch", SLGpuTexMigrate::readFile(p4, SLGpuTexMigrate::makeName(other), r4) == SLGpuTexMigrate::FileStatus::NAME_MISMATCH);
    }

    // A folder converts: good files become per-level (#865 v2) records -- a tail plus one
    // record per level above 256 px -- readable back to the same bytes with the header
    // facts in their meta; bad files and .part leftovers are deleted, the folder is removed
    // when done.
    template<> template<>
    void gtxmigrate_object::test<3>()
    {
        TempDir root;
        TempDir gdir;
        std::vector<Gtx> good;
        for (U32 i = 0; i < 20; ++i)
        {
            const U16 side = (i & 2) ? 512 : 128;
            good.push_back(makeGtx(100 + i, (i & 1) != 0, side, side));
            writeGtx(gdir, good.back());
        }
        Gtx bad = makeGtx(200, false, 32, 32);
        bad.payload[0] ^= 1;
        writeGtx(gdir, bad);
        { std::ofstream(gdir.sub("leftover.gtx.part")) << "x"; }
        { std::ofstream(gdir.sub("not-a-name.gtx")) << "x"; }
        ensure("has work", SLGpuTexMigrate::hasWork(gdir.path));

        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));
        const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, fastOptions());
        ensure_equals("files", r.files, 21u);   // 20 good + 1 bad hash (bad names are counted as bad up front)
        ensure_equals("converted", r.converted, 20u);
        ensure_equals("bad", r.bad, 3u);         // bad hash, .part, bad name
        ensure_equals("failed", r.failed, 0u);
        ensure("complete", r.completed);
        ensure("folder removed", !exists(gdir.path));
        ensure("no more work", !SLGpuTexMigrate::hasWork(gdir.path));

        for (const Gtx& g : good)
        {
            LLGpuTexRecord::Chain c;
            std::vector<U8> bytes;
            ensure("chain present", readChain(store, g.name.id, g.name.mask, c, bytes));
            ensure("written as v2", !c.v1 && !store.exists(LLGpuTexRecord::wholeKey(g.name.id, g.name.mask)));
            ensure("bytes", bytes == g.payload);
            ensure_equals("records", c.recs.size(), (size_t)(g.hdr.w > 256 ? 2 : 1));   // (512 + tail, or the tail)
            const LLGpuTexHeader& h = c.hdr;
            ensure("header facts", h.w == g.hdr.w && h.flags == g.hdr.flags && h.enc == g.hdr.enc && h.levels == g.hdr.levels
                                   && h.cov_cut == 128 && h.alpha_side == 64 && h.full_w == g.hdr.full_w);
            LLGpuTexCache::Info info;
            LLGpuTexRecord::infoFrom(c, info);
            ensure("info from index", info.w == g.hdr.w && info.enc_level == 2);
            for (const LLGpuTexRecord::Chain::Rec& rec : c.recs)
            {
                LLCache::RecordInfo ri;
                ensure("record compressed", store.lookup(rec.key, &ri) && ri.stored_bytes < ri.raw_bytes);
            }
        }
        LLGpuTexRecord::Chain none;
        ensure("bad not imported", !LLGpuTexRecord::resolve(store, bad.name.id, false, none));
        ensure_equals("verifyAll", store.verifyAll(nullptr), 0u);
    }

    // A key already in the store is not imported (its record is newer); a budget cut
    // deletes the remaining (oldest) files; a stop leaves files for the next run, which
    // resumes with them.
    template<> template<>
    void gtxmigrate_object::test<4>()
    {
        TempDir root;
        LLCache::Store store;
        ensure("open", store.open(storeConfig(root.sub("store"))));

        {
            TempDir gdir;
            Gtx g = makeGtx(300, false, 64, 64);
            writeGtx(gdir, g);
            Gtx newer = makeGtx(300, false, 64, 64);   // this session's capture of the same chain
            for (U8& b : newer.payload) b ^= 0x33;
            writeChain(store, newer.hdr, newer.payload);
            const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, fastOptions());
            ensure_equals("already", r.already, 1u);
            ensure_equals("not converted", r.converted, 0u);
            LLGpuTexRecord::Chain c;
            std::vector<U8> bytes;
            ensure("store copy kept", readChain(store, g.name.id, false, c, bytes) && bytes == newer.payload);
        }
        {
            // Finer version wins: the larger old file of the same encoding adds its levels
            // above the store's (smaller) chain; the store's tail stays.
            TempDir gdir;
            Gtx g = makeGtx(310, false, 1024, 1024);
            writeGtx(gdir, g);
            Gtx coarse = makeGtx(310, false, 256, 256);
            for (U8& b : coarse.payload) b ^= 0x55;
            writeChain(store, coarse.hdr, coarse.payload);
            const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, fastOptions());
            ensure_equals("finer imported", r.converted, 1u);
            LLGpuTexRecord::Chain c;
            std::vector<U8> bytes;
            ensure("finer chain", readChain(store, g.name.id, false, c, bytes) && !c.v1 && c.hdr.w == 1024);
            const size_t above = (size_t)LLGpuTexRecord::chainBytes(0x91, 1024, 1024, 2);   // 1024 + 512
            std::vector<U8> expect(g.payload.begin(), g.payload.begin() + (std::ptrdiff_t)above);
            expect.insert(expect.end(), coarse.payload.begin(), coarse.payload.end());
            ensure("file's levels on the store's tail", bytes == expect);

            // A finer file over an old (#715) whole-chain record replaces it with v2.
            TempDir gdir2;
            Gtx g2 = makeGtx(320, false, 512, 512);
            writeGtx(gdir2, g2);
            Gtx v1 = makeGtx(320, false, 128, 128);
            U8 meta[32];
            LLCache::WriteOptions o;
            o.meta = meta;
            o.meta_len = LLGpuTexRecord::packMetaV1(v1.hdr, meta);
            store.write(LLGpuTexRecord::wholeKey(v1.name.id, false), LLCache::WriteSource::copy(v1.payload.data(), v1.payload.size()), o);
            store.flush();
            ensure("v1 there", readChain(store, g2.name.id, false, c, bytes) && c.v1);
            const SLGpuTexMigrate::Result r2 = SLGpuTexMigrate::migrateDir(gdir2.path, store, fastOptions());
            ensure_equals("finer over v1", r2.converted, 1u);
            ensure("v2 now", readChain(store, g2.name.id, false, c, bytes) && !c.v1 && bytes == g2.payload);
            ensure("v1 removed", !store.exists(LLGpuTexRecord::wholeKey(g2.name.id, false)));
        }
        {
            TempDir gdir;
            for (U32 i = 0; i < 10; ++i) writeGtx(gdir, makeGtx(400 + i, false, 128, 128));
            SLGpuTexMigrate::Options o = fastOptions();
            o.live_cap_bytes = 1;   // the store already holds more than this
            const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, o);
            ensure_equals("all over budget", r.over_budget, 10u);
            ensure("complete", r.completed);
            ensure("folder removed", !exists(gdir.path));
        }
        {
            TempDir gdir;
            for (U32 i = 0; i < 6; ++i) writeGtx(gdir, makeGtx(500 + i, false, 64, 64));
            std::atomic<bool> stop{ true };
            SLGpuTexMigrate::Options o = fastOptions();
            o.stop = &stop;
            SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, o);
            ensure("stopped", r.stopped && !r.completed);
            ensure_equals("left", r.left, 6u);
            ensure("files kept", SLGpuTexMigrate::hasWork(gdir.path));
            stop = false;
            r = SLGpuTexMigrate::migrateDir(gdir.path, store, o);
            ensure_equals("resumed", r.converted, 6u);
            ensure("complete", r.completed);
        }
    }

    // Disk full: NO_SPACE stops the import and keeps the files.
    template<> template<>
    void gtxmigrate_object::test<5>()
    {
        TempDir root;
        TempDir gdir;
        for (U32 i = 0; i < 4; ++i) writeGtx(gdir, makeGtx(600 + i, false, 64, 64));
        LLCache::Store::Config c = storeConfig(root.sub("store"));
        c.free_space = [] { return (S64)(1ull << 20); };   // 1 MB free, under the 2 GB reserve
        LLCache::Store store;
        ensure("open", store.open(c));
        const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, fastOptions());
        ensure("disk full seen", r.disk_full);
        ensure("not complete", !r.completed);
        ensure_equals("nothing converted", r.converted, 0u);
        ensure("files kept", SLGpuTexMigrate::hasWork(gdir.path));
    }

    // #715 migration speed: small files with the store's REAL run timer (2 s) and the default
    // window. A write completes only when its run is flushed (4 MiB or the timer); the old
    // 32-file window waited one timer per 32 small files (16 files/s: 200 files ~12 s). The
    // byte-bounded window lets the run fill and the throttles alone pace it.
    template<> template<>
    void gtxmigrate_object::test<6>()
    {
        TempDir root;
        TempDir gdir;
        const U32 n = 200;
        for (U32 i = 0; i < n; ++i) writeGtx(gdir, makeGtx(700 + i, false, 128, 128));
        LLCache::Store::Config c = storeConfig(root.sub("store"));
        c.run_flush_ms = 2000;   // the production default
        LLCache::Store store;
        ensure("open", store.open(c));
        SLGpuTexMigrate::Options o = fastOptions();
        o.max_files_per_sec = 400;   // the default file cap stays on: 200 files >= 0.5 s
        const auto t0 = std::chrono::steady_clock::now();
        const SLGpuTexMigrate::Result r = SLGpuTexMigrate::migrateDir(gdir.path, store, o);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ensure_equals("converted", r.converted, n);
        ensure("complete", r.completed);
        ensure("file cap paced it", secs >= 0.45);
        ensure("no 2 s wait per small window", secs < 4.0);
    }
}
