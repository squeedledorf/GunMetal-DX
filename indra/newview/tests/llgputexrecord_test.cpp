/**
 * @file llgputexrecord_test.cpp
 * @brief GPU-ready per-level records (#865): keys and geometry, writing a chain as a
 *        tail + level records, partial reads, corruption of one level, finer-version-wins,
 *        mixed v1 / v2 lookups and the throttled, interruptible v1 -> v2 rewrite.
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

#include "../llgputexrecord.h"

#include "llcachestore_platform.h"

#include <algorithm>
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
    namespace R = LLGpuTexRecord;
    constexpr U32 BC7 = 145;   // VK_FORMAT_BC7_UNORM_BLOCK
    constexpr U32 BC1 = 131;   // VK_FORMAT_BC1_RGB_UNORM_BLOCK

    std::atomic<U32> sDirCounter{ 0 };

    struct TempDir
    {
        std::string path;
        TempDir()
        {
            std::error_code ec;
            fs::path base = fs::temp_directory_path(ec);
            path = (base / ("llgputexrecord_test_" + std::to_string(LLCacheStorePlatform::processId()) + "_" +
                            std::to_string(sDirCounter++))).string();
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    LLUUID idFor(U32 n)
    {
        LLUUID id;
        for (int i = 0; i < 16; ++i) id.mData[i] = (U8)((n * 2654435761u) >> ((i % 4) * 8)) ^ (U8)(i * 7);
        id.mData[0] = (U8)n;
        id.mData[1] = (U8)(n >> 8);
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

    LLGpuTexHeader chainHdr(const LLUUID& id, bool mask, U16 w, U16 h, U32 fmt = BC7, U32 encver = 7)
    {
        LLGpuTexHeader hd;
        hd.magic = LLGpuTexHeader::kMagic;
        hd.version = LLGpuTexCache::FILE_VERSION;
        hd.encoder_version = encver;
        hd.header_bytes = sizeof(LLGpuTexHeader);
        memcpy(hd.id, id.mData, 16);
        hd.vk_format = fmt;
        hd.enc = 5; hd.enc_level = 2; hd.comps = 4;
        hd.flags = mask ? LLGpuTexHeader::F_MASK_VARIANT : 0;
        hd.w = w; hd.h = h; hd.full_w = w; hd.full_h = h;
        hd.levels = (U16)R::mipCount(w, h);
        hd.alpha_grad_pct = 12; hd.alpha_side = 64;
        hd.payload_bytes = R::chainBytes(fmt, w, h, hd.levels);
        return hd;
    }

    // Distinct bytes per level, so a slice from the wrong level never compares equal.
    std::vector<U8> chainBytes(const LLGpuTexHeader& hd, U32 seed)
    {
        std::vector<U8> v((size_t)hd.payload_bytes);
        size_t off = 0;
        for (U32 l = 0; l < hd.levels; ++l)
        {
            const size_t n = (size_t)R::levelBytes(hd.vk_format, std::max(hd.w >> l, 1), std::max(hd.h >> l, 1));
            for (size_t i = 0; i < n; ++i) v[off + i] = (U8)((i / 16) * 3 + seed + l * 41);
            off += n;
        }
        return v;
    }

    // Offset of level `l` in a whole chain.
    size_t levelOffset(const LLGpuTexHeader& hd, U32 l)
    {
        return (size_t)R::chainBytes(hd.vk_format, hd.w, hd.h, l);
    }

    // A chain as a capture writes it (no completions).
    bool writeChain(LLCache::Store& s, const LLGpuTexHeader& hd, const std::vector<U8>& bytes)
    {
        std::vector<R::PartPlan> parts;
        if (!R::planWrite(hd, bytes.size(), parts)) return false;
        LLCache::WriteOptions o;
        R::PartWrite pw = R::writeParts(s, parts, bytes.data(), o, nullptr);
        s.flush();
        return pw.refused == LLCache::Status::OK;
    }

    // An old (#715) whole-chain record.
    void writeV1(LLCache::Store& s, const LLGpuTexHeader& hd, const std::vector<U8>& bytes)
    {
        U8 meta[32] = {};
        LLCache::WriteOptions o;
        o.meta = meta;
        o.meta_len = R::packMetaV1(hd, meta);
        LLUUID id;
        memcpy(id.mData, hd.id, 16);
        s.write(R::wholeKey(id, (hd.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0),
                LLCache::WriteSource::copy(bytes.data(), bytes.size()), o);
        s.flush();
    }

    // Read a plan synchronously and assemble it.
    bool readPlan(LLCache::Store& s, const R::Chain& c, const R::ReadPlan& p, LLGpuTexCache::Blob& out, size_t& bad,
                  std::vector<LLCache::Status>* statuses = nullptr)
    {
        std::vector<LLCache::ReadResult> rr;
        for (size_t i : p.recs) rr.push_back(s.readSync(c.recs[i].key));
        if (statuses) for (const auto& r : rr) statuses->push_back(r.status);
        return R::assemble(c, p, rr, out, bad);
    }

    std::vector<U8> concat(const LLGpuTexCache::Blob& b)
    {
        std::vector<U8> v;
        for (const auto& p : b.pieces) v.insert(v.end(), p.bytes.begin(), p.bytes.end());
        return v;
    }

    std::vector<std::string> segmentFiles(const std::string& dir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string n = it->path().filename().string();
            if (n.rfind("seg-", 0) == 0) out.push_back(it->path().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    U32 get32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }

    // Flip a payload byte of the LAST record of `key` in the segments (header + meta stay
    // valid, so the store's payload hash catches it on read).
    bool corruptRecord(const std::string& store_dir, const LLCache::Key& key)
    {
        bool done = false;
        for (const std::string& seg : segmentFiles(store_dir))
        {
            std::vector<U8> bytes;
            {
                std::ifstream in(seg, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            size_t hit = 0;
            for (size_t off = 64; off + 96 <= bytes.size(); off += 16)
            {
                const U8* r = bytes.data() + off;
                if (memcmp(r, "SLCR", 4) != 0 || r[5] != (U8)key.kind || memcmp(r + 8, key.id.mData, 16) != 0
                    || get32(r + 24) != key.variant || (r[6] & 0x01))
                    continue;
                hit = off;
            }
            if (!hit) continue;
            const U8* r = bytes.data() + hit;
            const size_t meta_len = r[32];
            const size_t stored = get32(r + 36);
            bytes[hit + 96 + meta_len + stored / 2] ^= 0x5a;
            std::ofstream out(seg, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            done = true;
        }
        return done;
    }

    void settle(LLCache::Store& s, R::Rewriter& rw, F64& t)
    {
        for (int i = 0; i < 400 && !rw.idle(); ++i)
        {
            t += 0.1;
            rw.pump(t);
            s.flush();
        }
    }
}

namespace tut
{
    struct gputexrecord_data {};
    typedef test_group<gputexrecord_data> gputexrecord_test;
    typedef gputexrecord_test::object gputexrecord_object;
    tut::gputexrecord_test gputexrecord("LLGpuTexRecord");

    // Keys, variants and chain geometry.
    template<> template<>
    void gputexrecord_object::test<1>()
    {
        const LLUUID id = idFor(1);
        ensure_equals("v1 key", R::wholeKey(id, true).variant, 1u);
        ensure("v1 part", R::partOf(R::wholeKey(id, false).variant) == R::Part::WHOLE);
        ensure("tail part", R::partOf(R::tailKey(id, true).variant) == R::Part::TAIL);
        ensure("tail mask", R::isMaskVariant(R::tailKey(id, true).variant));
        const LLCache::Key k = R::levelKey(id, false, 1024);
        ensure("level part", R::partOf(k.variant) == R::Part::LEVEL);
        ensure_equals("level log2", R::levelLog2(k.variant), 10u);
        ensure("non-pow2 side keys by floor log2", R::levelKey(id, false, 1000).variant == R::levelKey(id, false, 512).variant);
        ensure("role bits unknown", R::partOf(2u) == R::Part::UNKNOWN);
        ensure("level <= tail size unknown", R::partOf(R::VARIANT_V2 | (8u << R::LEVEL_SHIFT)) == R::Part::UNKNOWN);
        ensure_equals("mips", R::mipCount(2048, 512), 12u);
        ensure_equals("bc7 4x4", R::levelBytes(BC7, 4, 4), (U64)16);
        ensure_equals("bc7 1x1 = a block", R::levelBytes(BC7, 1, 1), (U64)16);
        ensure_equals("bc1 8x8", R::levelBytes(BC1, 8, 8), (U64)32);
        ensure_equals("rgba8", R::levelBytes(37, 8, 4), (U64)128);
        ensure_equals("unknown format", R::levelBytes(999, 8, 8), (U64)0);
        ensure_equals("bc7 chain 8x8", R::chainBytes(BC7, 8, 8, 4), (U64)(64 + 16 + 16 + 16));
        ensure("meta fits", R::META_BYTES <= 32);
    }

    // A chain is written as level records (largest first) + the tail; the index resolves
    // it without I/O; a partial plan reads only what it needs; pieces reassemble exactly.
    template<> template<>
    void gputexrecord_object::test<2>()
    {
        TempDir td;
        LLCache::Store s;
        ensure("open", s.open(storeConfig(td.path)));
        const LLUUID id = idFor(2);
        const LLGpuTexHeader hd = chainHdr(id, false, 2048, 1024);
        const std::vector<U8> bytes = chainBytes(hd, 2);

        std::vector<R::PartPlan> parts;
        ensure("plan", R::planWrite(hd, bytes.size(), parts));
        ensure_equals("2048, 1024, 512 + tail", parts.size(), (size_t)4);
        ensure("largest first", parts[0].w == 2048 && parts[1].w == 1024 && parts[2].w == 512);
        ensure("tail last", parts[3].tail && parts[3].w == 256 && parts[3].h == 128);
        ensure("tail covers the rest", parts[3].offset + parts[3].bytes == bytes.size());
        std::vector<R::PartPlan> bad;
        ensure("wrong size refused", !R::planWrite(hd, bytes.size() - 16, bad));

        ensure("written", writeChain(s, hd, bytes));
        ensure("no v1 record", !s.exists(R::wholeKey(id, false)));
        R::Chain c;
        ensure("resolves", R::resolve(s, id, false, c));
        ensure("v2", !c.v1);
        ensure("top", c.hdr.w == 2048 && c.hdr.h == 1024 && c.hdr.levels == 12);
        ensure_equals("records", c.recs.size(), (size_t)4);
        ensure_equals("tail level", c.tail_level, 3u);
        ensure_equals("chain bytes", c.bytes(), (U64)bytes.size());
        LLGpuTexCache::Info info;
        R::infoFrom(c, info);
        ensure("info", info.w == 2048 && info.enc_level == 2 && info.encoder_version == 7);
        R::Chain cm;
        ensure("other mask variant is another chain", !R::resolve(s, id, true, cm));

        // Whole read.
        R::ReadPlan p;
        LLGpuTexCache::Blob b;
        size_t badi = 0;
        ensure("plan all", R::planRead(c, 0, 0, p) && p.recs.size() == 4 && p.first_level == 0 && p.end_level == 12);
        ensure("read all", readPlan(s, c, p, b, badi));
        ensure("all bytes", concat(b) == bytes);
        ensure("holds all", b.holds(0, 12));

        // First image at 1024: the 2048 record is not read.
        ensure("plan skip 1", R::planRead(c, 1, 0, p));
        ensure_equals("skip 1 records", p.recs.size(), (size_t)3);
        ensure_equals("skip 1 bytes", p.bytes, (U64)(bytes.size() - levelOffset(hd, 1)));
        ensure("read skip 1", readPlan(s, c, p, b, badi));
        ensure("skip 1 bytes match", concat(b) == std::vector<U8>(bytes.begin() + (std::ptrdiff_t)levelOffset(hd, 1), bytes.end()));
        ensure("skip 1 range", b.first_level == 1 && b.endLevel() == 12 && !b.holds(0, 12));

        // First image at the tail: one record.
        ensure("plan tail", R::planRead(c, 5, 0, p) && p.recs.size() == 1 && p.first_level == 3);

        // Upgrade 512 -> 2048: only 2048 and 1024 (the resident 512 and below are copied).
        ensure("plan upgrade", R::planRead(c, 0, 512, p));
        ensure_equals("upgrade records", p.recs.size(), (size_t)2);
        ensure("upgrade range", p.first_level == 0 && p.end_level == 2);
        ensure("read upgrade", readPlan(s, c, p, b, badi));
        ensure("upgrade bytes", concat(b) == std::vector<U8>(bytes.begin(), bytes.begin() + (std::ptrdiff_t)levelOffset(hd, 2)));
        // Upgrade 128 -> 512: the tail is needed (it holds 256 .. 1).
        ensure("plan small upgrade", R::planRead(c, 2, 128, p) && p.recs.size() == 2 && p.end_level == 12);
        // Nothing to read.
        ensure("resident covers it", !R::planRead(c, 1, 1024, p));

        // touch / remove act on every record.
        R::removeAll(s, id, false);
        ensure("all gone", !R::resolve(s, id, false, c) && !s.exists(R::levelKey(id, false, 2048)));
        ensure_equals("verifyAll", s.verifyAll(nullptr), 0u);
    }

    // Corruption of one level record drops that level only: the read reports it, the
    // chain then resolves to the levels below it, the tail and the others stay readable.
    template<> template<>
    void gputexrecord_object::test<3>()
    {
        TempDir td;
        const LLUUID id = idFor(3);
        const LLGpuTexHeader hd = chainHdr(id, true, 2048, 2048);
        const std::vector<U8> bytes = chainBytes(hd, 3);
        {
            LLCache::Store s;
            ensure("open", s.open(storeConfig(td.path)));
            ensure("written", writeChain(s, hd, bytes));
            s.close();
        }
        ensure("corrupted", corruptRecord(td.path, R::levelKey(id, true, 1024)));
        LLCache::Store s;
        ensure("reopen", s.open(storeConfig(td.path)));
        R::Chain c;
        ensure("indexed whole until read", R::resolve(s, id, true, c) && c.hdr.w == 2048);
        R::ReadPlan p;
        ensure("plan", R::planRead(c, 0, 0, p));
        LLGpuTexCache::Blob b;
        size_t badi = 9;
        std::vector<LLCache::Status> st;
        ensure("read fails", !readPlan(s, c, p, b, badi, &st));
        ensure_equals("the 1024 record is the bad one", badi, (size_t)1);
        ensure("1024 corrupt", st[1] == LLCache::Status::CORRUPT);
        ensure("others fine", st[0] == LLCache::Status::OK && st[2] == LLCache::Status::OK && st[3] == LLCache::Status::OK);
        ensure("1024 dropped", !s.exists(R::levelKey(id, true, 1024)));
        ensure("2048 kept (unreachable)", s.exists(R::levelKey(id, true, 2048)));
        ensure("re-resolves lower", R::resolve(s, id, true, c) && c.hdr.w == 512 && c.hdr.levels == 10);
        ensure("lower chain reads", R::planRead(c, 0, 0, p) && readPlan(s, c, p, b, badi));
        ensure("lower bytes", concat(b) == std::vector<U8>(bytes.begin() + (std::ptrdiff_t)levelOffset(hd, 2), bytes.end()));

        // A finer capture of the same encoding writes only the levels above the store's top
        // (and so repairs the gap).
        U32 above = 0;
        ensure("above", R::writeMode(s, hd, above) == R::WriteMode::ABOVE && above == 512);
        std::vector<R::PartPlan> parts;
        ensure("plan above", R::planWrite(hd, bytes.size(), parts, above) && parts.size() == 2 && !parts[1].tail);
        R::writeParts(s, parts, bytes.data(), LLCache::WriteOptions(), nullptr);
        s.flush();
        ensure("repaired", R::resolve(s, id, true, c) && c.hdr.w == 2048);
        ensure("repaired bytes", R::planRead(c, 0, 0, p) && readPlan(s, c, p, b, badi) && concat(b) == bytes);
    }

    // Finer version wins; another encoding replaces (stale larger levels removed); mixed
    // v1 / v2 serves the larger chain.
    template<> template<>
    void gputexrecord_object::test<4>()
    {
        TempDir td;
        LLCache::Store s;
        ensure("open", s.open(storeConfig(td.path)));
        const LLUUID id = idFor(4);
        const LLGpuTexHeader big = chainHdr(id, false, 1024, 1024);
        const LLGpuTexHeader smaller = chainHdr(id, false, 512, 512);
        U32 above = 0;
        ensure("nothing yet: full", R::writeMode(s, big, above) == R::WriteMode::FULL);
        ensure("write big", writeChain(s, big, chainBytes(big, 4)));
        ensure("same, not larger: skip", R::writeMode(s, smaller, above) == R::WriteMode::SKIP);
        ensure("same size: skip", R::writeMode(s, big, above) == R::WriteMode::SKIP);

        // Another encoder version at a smaller size replaces it; the old 1024 level goes.
        const LLGpuTexHeader other = chainHdr(id, false, 512, 512, BC7, 8);
        ensure("other encoding: full", R::writeMode(s, other, above) == R::WriteMode::FULL);
        ensure("write other", writeChain(s, other, chainBytes(other, 5)));
        R::removeStale(s, id, false, 512);
        R::Chain c;
        ensure("new chain", R::resolve(s, id, false, c) && c.hdr.w == 512 && c.hdr.encoder_version == 8);
        ensure("stale 1024 removed", !s.exists(R::levelKey(id, false, 1024)));

        // Mixed: a v1 record larger than the v2 chain wins the lookup; a v2 chain at least
        // as large wins over v1.
        const LLUUID id2 = idFor(40);
        const LLGpuTexHeader v1h = chainHdr(id2, false, 1024, 1024);
        const std::vector<U8> v1b = chainBytes(v1h, 6);
        writeV1(s, v1h, v1b);
        ensure("v1 alone", R::resolve(s, id2, false, c) && c.v1 && c.hdr.w == 1024 && c.recs.size() == 1);
        const LLGpuTexHeader v2h = chainHdr(id2, false, 512, 512);
        ensure("v2 smaller", writeChain(s, v2h, chainBytes(v2h, 7)));
        ensure("v1 larger wins", R::resolve(s, id2, false, c) && c.v1);
        R::ReadPlan p;
        LLGpuTexCache::Blob b;
        size_t badi = 0;
        ensure("v1 always read whole", R::planRead(c, 3, 256, p) && p.first_level == 0 && p.end_level == c.hdr.levels);
        ensure("v1 reads", readPlan(s, c, p, b, badi) && concat(b) == v1b && b.pieces.size() == 1);
        ensure("v2 full write", writeChain(s, v1h, v1b));
        ensure("v2 tie wins", R::resolve(s, id2, false, c) && !c.v1 && c.hdr.w == 1024);
        R::removeStale(s, id2, false, 1024);
        ensure("v1 removed by a full write", !s.exists(R::wholeKey(id2, false)));
    }

    // v1 -> v2 rewrite: a read v1 record is rewritten per level at LOW priority and the
    // v1 record removed only after every v2 record is appended; an interrupted rewrite
    // leaves both (lookups serve v2 on the tie) and the next offer just drops v1;
    // throttled by records / s.
    template<> template<>
    void gputexrecord_object::test<5>()
    {
        TempDir td;
        const LLUUID id = idFor(5);
        const LLGpuTexHeader hd = chainHdr(id, false, 1024, 512);
        const std::vector<U8> bytes = chainBytes(hd, 8);
        F64 t = 100.0;
        {
            LLCache::Store s;
            ensure("open", s.open(storeConfig(td.path)));
            writeV1(s, hd, bytes);
            R::Chain c;
            ensure("v1", R::resolve(s, id, false, c) && c.v1);
            R::ReadPlan p;
            auto blob = std::make_shared<LLGpuTexCache::Blob>();
            size_t badi = 0;
            ensure("read v1", R::planRead(c, 0, 0, p) && readPlan(s, c, p, *blob, badi));
            R::Rewriter rw;
            rw.setStore(&s);
            ensure("offered", rw.offer(c, blob, false));
            ensure("no duplicate offer", !rw.offer(c, blob, false));
            settle(s, rw, t);
            const R::Rewriter::Stats st = rw.stats();
            ensure_equals("rewritten", st.rewritten, (U64)1);
            ensure("v1 removed", !s.exists(R::wholeKey(id, false)));
            ensure("v2 now", R::resolve(s, id, false, c) && !c.v1 && c.hdr.w == 1024 && c.recs.size() == 3);
            ensure("v2 bytes", R::planRead(c, 0, 0, p) && readPlan(s, c, p, *blob, badi) && concat(*blob) == bytes);
            ensure("partial read now possible", R::planRead(c, 1, 0, p) && p.recs.size() == 2);
            s.close();
        }
        // Interrupted: the v2 records are queued, the session ends before the rewrite is
        // settled -> v1 stays (only removed after confirmation), lookups serve v2 (tie).
        const LLUUID id2 = idFor(6);
        const LLGpuTexHeader hd2 = chainHdr(id2, false, 2048, 2048);
        const std::vector<U8> bytes2 = chainBytes(hd2, 9);
        {
            LLCache::Store s;
            ensure("reopen", s.open(storeConfig(td.path)));
            writeV1(s, hd2, bytes2);
            R::Chain c;
            R::ReadPlan p;
            auto blob = std::make_shared<LLGpuTexCache::Blob>();
            size_t badi = 0;
            ensure("read v1 2", R::resolve(s, id2, false, c) && c.v1 && R::planRead(c, 0, 0, p) && readPlan(s, c, p, *blob, badi));
            R::Rewriter rw;
            rw.setStore(&s);
            ensure("offered 2", rw.offer(c, blob, false));
            t += 1.0;
            rw.pump(t);   // queued to the store, never settled
            rw.clear();
            s.close();    // the store appends what it accepted
        }
        {
            LLCache::Store s;
            ensure("reopen 2", s.open(storeConfig(td.path)));
            R::Chain c;
            ensure("v1 kept", s.exists(R::wholeKey(id2, false)));
            ensure("v2 served on the tie", R::resolve(s, id2, false, c) && !c.v1 && c.hdr.w == 2048);
            // A later read of ... the v1 record (e.g. resolved before the v2 landed) is offered
            // again: the rewriter sees the complete v2 chain and only drops v1.
            R::Chain v1c;
            v1c.v1 = true;
            LLGpuTexCache::Blob tmp;
            R::ReadPlan p;
            {
                LLCache::ReadResult rr = s.readSync(R::wholeKey(id2, false));
                ensure("v1 readable", rr.status == LLCache::Status::OK && rr.bytes == bytes2);
                ensure("v1 meta", R::unpackMetaV1(rr.info.meta, rr.info.meta_len, id2, 0, rr.bytes.size(), v1c.hdr));
                R::Chain::Rec rec;
                rec.key = R::wholeKey(id2, false); rec.levels = v1c.hdr.levels; rec.raw_bytes = (U32)rr.bytes.size();
                v1c.recs.push_back(rec);
                v1c.v1_write_time = rr.info.write_time;
                auto blob = std::make_shared<LLGpuTexCache::Blob>();
                blob->hdr = v1c.hdr;
                LLGpuTexCache::Blob::Piece pc;
                pc.levels = v1c.hdr.levels;
                pc.bytes = rr.bytes;
                blob->pieces.push_back(pc);
                R::Rewriter rw;
                rw.setStore(&s);
                ensure("offered 3", rw.offer(v1c, blob, false));
                settle(s, rw, t);
                ensure_equals("stale (v2 already there)", rw.stats().stale, (U64)1);
            }
            ensure("v1 dropped", !s.exists(R::wholeKey(id2, false)));
            ensure("v2 intact", R::resolve(s, id2, false, c) && !c.v1 && R::planRead(c, 0, 0, p));
            size_t badi = 0;
            ensure("v2 bytes 2", readPlan(s, c, p, tmp, badi) && concat(tmp) == bytes2);

            // Throttle: one record per second -> the queue drains one per second.
            R::Rewriter rw;
            rw.setStore(&s);
            R::Rewriter::Limits lim;
            lim.records_per_sec = 1;
            rw.setLimits(lim);
            for (U32 i = 0; i < 3; ++i)
            {
                const LLUUID idn = idFor(70 + i);
                const LLGpuTexHeader h = chainHdr(idn, false, 512, 512);
                const std::vector<U8> b = chainBytes(h, 10 + i);
                writeV1(s, h, b);
                R::Chain cc;
                R::ReadPlan pp;
                auto blob = std::make_shared<LLGpuTexCache::Blob>();
                ensure("v1 n", R::resolve(s, idn, false, cc) && cc.v1 && R::planRead(cc, 0, 0, pp)
                                   && readPlan(s, cc, pp, *blob, badi));
                ensure("offer n", rw.offer(cc, blob, false));
            }
            F64 tt = 1000.0;
            rw.pump(tt);   // first pump: 0.05 s of allowance -> nothing yet
            ensure_equals("none started", rw.stats().queue, 3u);
            tt += 1.0;
            rw.pump(tt);
            ensure_equals("one per second", rw.stats().queue, 2u);
            settle(s, rw, tt);
            ensure_equals("all rewritten", rw.stats().rewritten, (U64)3);
            ensure_equals("verifyAll", s.verifyAll(nullptr), 0u);
        }
    }

    // #991: partial chains (captured at a higher discard: top below the asset's size) are
    // saved and upgraded in place by larger captures of the same encoding. A top inside the
    // tail is upgraded by rewriting the tail (a 128 capture over a 64 chain used to be
    // refused as "not a full mip chain"; a 512 one over it wrote a level the old tail could
    // never reach); never replaced by a smaller capture.
    template<> template<>
    void gputexrecord_object::test<6>()
    {
        TempDir td;
        LLCache::Store s;
        ensure("open", s.open(storeConfig(td.path)));
        const LLUUID id = idFor(6);
        auto capture = [&](U16 side, U32 seed, R::WriteMode want, U32 want_same_top) -> std::vector<U8>
        {
            LLGpuTexHeader hd = chainHdr(id, false, side, side);
            hd.full_w = hd.full_h = 1024;   // the asset's size: anything smaller is partial
            const std::vector<U8> bytes = chainBytes(hd, seed);
            U32 above = 0, same_top = 0;
            const R::WriteMode m = R::writeMode(s, hd, above, &same_top);
            ensure_equals("mode", (U32)m, (U32)want);
            ensure_equals("same_top", same_top, want_same_top);
            if (m == R::WriteMode::SKIP) return bytes;
            std::vector<R::PartPlan> parts;
            ensure("plan", R::planWrite(hd, bytes.size(), parts, above));
            ensure("tail written unless ABOVE a 256 tail", parts.back().tail == (m == R::WriteMode::FULL));
            R::PartWrite pw = R::writeParts(s, parts, bytes.data(), LLCache::WriteOptions(), nullptr);
            s.flush();
            ensure("queued", pw.refused == LLCache::Status::OK);
            if (m == R::WriteMode::FULL) R::removeStale(s, id, false, side);
            return bytes;
        };
        auto readAll = [&](U32 want_top, const std::vector<U8>* want_bytes, bool want_partial)
        {
            R::Chain c;
            ensure("resolves", R::resolve(s, id, false, c));
            ensure_equals("top", (U32)c.hdr.w, want_top);
            LLGpuTexCache::Info inf;
            R::infoFrom(c, inf);
            ensure("full size known", inf.full_w == 1024 && inf.full_h == 1024);
            ensure("partial flag", inf.partial() == want_partial);
            R::ReadPlan p;
            LLGpuTexCache::Blob b;
            size_t badi = 0;
            ensure("reads", R::planRead(c, 0, 0, p) && readPlan(s, c, p, b, badi));
            ensure_equals("whole chain", b.endLevel(), (U32)c.hdr.levels);
            if (want_bytes) ensure("bytes", concat(b) == *want_bytes);
        };

        const std::vector<U8> b64 = capture(64, 1, R::WriteMode::FULL, 0);
        readAll(64, &b64, true);
        const std::vector<U8> b128 = capture(128, 2, R::WriteMode::FULL, 64);   // (was refused)
        readAll(128, &b128, true);
        capture(64, 3, R::WriteMode::SKIP, 128);                                // never smaller
        readAll(128, &b128, true);
        const std::vector<U8> b512 = capture(512, 4, R::WriteMode::FULL, 128);  // level + new tail
        readAll(512, &b512, true);
        capture(1024, 5, R::WriteMode::ABOVE, 512);                             // 256 tail kept
        readAll(1024, nullptr, false);
        ensure_equals("verifyAll", s.verifyAll(nullptr), 0u);
    }
}
