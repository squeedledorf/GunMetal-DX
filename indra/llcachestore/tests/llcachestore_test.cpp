/**
 * @file llcachestore_test.cpp
 * @brief Integration tests for the segment store (llcachestore.h): crash at
 *        every write step, corrupt / truncated records, disk full, two
 *        instances, staged writes, read-your-writes, snapshot loss, eviction
 *        and compaction under budget pressure, the #709 wear cap, regions.
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

#include "../llcachestore.h"
#include "../llcachestore_format.h"
#include "../llcachestore_index.h"
#include "../llcachestore_platform.h"

#include "fsyspath.h"
#include "hbxxh.h"
#include "llstring.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>     // mallinfo2 (#866 benchmark)
#endif

#include "../test/lltut.h"

using namespace LLCache;
namespace fs = std::filesystem;

namespace
{
    std::vector<U8> makeData(U32 seed, size_t size, bool compressible = false)
    {
        std::vector<U8> v(size);
        U64 x = 0x9E3779B97F4A7C15ull ^ ((U64)seed * 0x2545F4914F6CDD1Dull) ^ size;
        for (size_t i = 0; i < size; ++i)
        {
            if (compressible)
            {
                v[i] = (U8)("gpu ready mip chain "[i % 20] + (seed & 7));
            }
            else
            {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                v[i] = (U8)(x >> 32);
            }
        }
        return v;
    }

    LLUUID idFor(U32 n)
    {
        LLUUID id;
        for (int i = 0; i < 16; ++i) id.mData[i] = (U8)((n * 2654435761u) >> ((i % 4) * 8)) ^ (U8)i;
        id.mData[0] = (U8)n;
        id.mData[1] = (U8)(n >> 8);
        return id;
    }

    std::atomic<U32> sDirCounter{ 0 };

    // A fresh folder per test, removed afterwards.
    struct TempDir
    {
        std::string path;
        TempDir()
        {
            std::error_code ec;
            fs::path base = fs::temp_directory_path(ec);
            path = (base / ("llcachestore_test_" + std::to_string(LLCacheStorePlatform::processId()) +
                            "_" + std::to_string(sDirCounter++))).string();
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

    Store::Config baseConfig(const std::string& dir)
    {
        Store::Config c;
        c.dir = dir;
        c.read_threads = 1;
        c.run_flush_ms = 20;
        c.detect_drive = false;
        c.snapshot_interval_s = 0;
        c.free_space = [] { return (S64)(1ull << 40); };
        return c;
    }

    std::vector<std::string> segmentFiles(const std::string& dir)
    {
        std::vector<std::string> v;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            U32 seq;
            if (LLCacheStoreFormat::parseSegmentFileName(e.path().filename().string(), seq)) v.push_back(e.path().string());
        }
        std::sort(v.begin(), v.end());
        return v;
    }

    std::vector<U8> readFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<U8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeFile(const std::string& path, const U8* p, size_t n)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write((const char*)p, (std::streamsize)n);
    }

    void copyDir(const std::string& from, const std::string& to)
    {
        std::error_code ec;
        fs::remove_all(to, ec);
        fs::create_directories(to, ec);
        for (auto& e : fs::directory_iterator(from, ec))
        {
            if (e.is_regular_file())
                fs::copy_file(e.path(), fs::path(to) / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }

    bool readEquals(Store& s, const Key& k, const std::vector<U8>& want)
    {
        ReadResult r = s.readSync(k);
        return r.status == Status::OK && r.bytes == want;
    }

    // Records written in one run land in submission order: their boundaries
    // in a fresh segment are 64 + cumulative record sizes.
    U64 recSize(size_t meta, size_t raw, bool shim)
    {
        size_t stored = shim ? raw + 24 : raw;   // LLCacheCodec raw blob = 24-byte header + bytes
        return LLCacheStoreFormat::recordSize((U32)meta, (U32)stored);
    }

    // Simple single-thread work queue for completion tests.
    struct TestQueue
    {
        std::mutex m;
        std::deque<std::function<void()>> q;
        Poster poster()
        {
            return [this](std::function<void()> f) { std::lock_guard<std::mutex> l(m); q.push_back(std::move(f)); return true; };
        }
        size_t run()
        {
            std::deque<std::function<void()>> todo;
            {
                std::lock_guard<std::mutex> l(m);
                todo.swap(q);
            }
            for (auto& f : todo) f();
            return todo.size();
        }
    };
}

namespace tut
{
    struct cachestore_data
    {
    };
    typedef test_group<cachestore_data> cachestore_test;
    typedef cachestore_test::object cachestore_object;
    tut::cachestore_test cachestore("LLCacheStore");

    // Disk accounting matches the files: catches double counting / drift.
    void checkInvariants(Store& s, const std::string& dir, const std::string& when)
    {
        s.flush();
        Stats st = s.stats();
        U64 files = 0;
        std::vector<std::string> segs = segmentFiles(dir);
        for (const std::string& f : segs) files += fs::file_size(f);
        ensure_equals(when + ": disk bytes == segment file bytes", st.disk_bytes, files);
        ensure_equals(when + ": segments == files", (size_t)st.segments, segs.size());
        U64 groups = 0;
        for (const GroupStats& g : st.groups) groups += g.disk_bytes;
        ensure_equals(when + ": group disk bytes add up", groups, st.disk_bytes);
        ensure(when + ": live <= disk", st.live_bytes <= st.disk_bytes);
    }

    // ------------------------------------------------------------------
    // Round trips for every storage path, meta, persistence across reopen.
    template<> template<>
    void cachestore_object::test<1>()
    {
        TempDir td;
        std::vector<U8> j2c = makeData(1, 5000);
        std::vector<U8> gpu = makeData(2, 70000, true);
        std::vector<U8> asset = makeData(3, 1234);
        U8 meta[5] = { 1, 2, 3, 4, 5 };
        Key kj(idFor(1), Kind::J2C), kg(idFor(2), Kind::GPUTEX, 1), ka(idFor(3), Kind::ASSET);
        {
            Store s;
            ensure("open", s.open(baseConfig(td.path)));
            ensure("owner", !s.readOnly());
            WriteOptions o;
            o.meta = meta;
            o.meta_len = 5;
            ensure("write j2c", s.write(kj, WriteSource::copy(j2c.data(), j2c.size()), o));
            ensure("write gpu", s.write(kg, WriteSource::copy(gpu.data(), gpu.size())));
            ensure("write asset", s.write(ka, WriteSource::copy(asset.data(), asset.size())));
            s.flush();
            RecordInfo ri;
            ensure("lookup j2c", s.lookup(kj, &ri));
            ensure_equals("j2c raw", ri.raw_bytes, (U32)j2c.size());
            ensure_equals("j2c meta len", (int)ri.meta_len, 5);
            ensure("j2c meta", memcmp(ri.meta, meta, 5) == 0);
            ensure_equals("j2c codec raw (range kind)", (int)ri.codec, 0);
            ensure("lookup gpu", s.lookup(kg, &ri));
            ensure("gpu compressed", ri.stored_bytes < gpu.size() / 2);
            ensure("read j2c", readEquals(s, kj, j2c));
            ensure("read gpu", readEquals(s, kg, gpu));
            ensure("read asset", readEquals(s, ka, asset));
            ensure("missing", s.readSync(Key(idFor(99), Kind::ASSET)).status == Status::MISSING);
            ensure("variant matters", !s.exists(Key(idFor(2), Kind::GPUTEX, 0)));
            ensure("kind matters", !s.exists(Key(idFor(3), Kind::MESH)));
            s.close();
        }
        {
            Store s;
            ensure("reopen", s.open(baseConfig(td.path)));
            ensure("j2c after reopen", readEquals(s, kj, j2c));
            ensure("gpu after reopen", readEquals(s, kg, gpu));
            ensure("asset after reopen", readEquals(s, ka, asset));
            RecordInfo ri;
            ensure("meta after reopen", s.lookup(kj, &ri) && ri.meta_len == 5 && memcmp(ri.meta, meta, 5) == 0);
            ensure("prev session access carried", ri.prev_session_access != 0);
            s.touch(kj);
            ensure("reuse counted on first touch of the session", s.lookup(kj, &ri) && ri.reuse_count >= 1);
            ensure_equals("verifyAll", s.verifyAll(nullptr), 0u);
        }
    }

    // ------------------------------------------------------------------
    // Read-your-writes before the append; borrowed sources; supersede; skip.
    template<> template<>
    void cachestore_object::test<2>()
    {
        TempDir td;
        Store::Config c = baseConfig(td.path);
        c.run_flush_ms = 60000;               // keep records queued
        c.run_flush_bytes = 64u << 20;
        Store s;
        ensure("open", s.open(c));
        Key k(idFor(10), Kind::ASSET);
        std::vector<U8> a = makeData(10, 3000), b = makeData(11, 4000);
        std::atomic<int> released{ 0 };
        std::vector<U8> borrowed = a;
        ensure("write borrowed", s.write(k, WriteSource::borrow(borrowed.data(), borrowed.size(), [&] { released++; })));
        RecordInfo ri;
        ensure("visible at once", s.lookup(k, &ri) && ri.queued);
        ensure("read queued", readEquals(s, k, a));
        ReadResult rr = s.readRangeSync(k, 100, 50);
        ensure("range of queued", rr.status == Status::OK && rr.bytes.size() == 50 && memcmp(rr.bytes.data(), a.data() + 100, 50) == 0);
        // Replace before it hits disk.
        Ticket<Status> t;
        ensure("write b", s.write(k, WriteSource::copy(b.data(), b.size()), WriteOptions(), &t));
        ensure("reads newest", readEquals(s, k, b));
        s.flush();
        ensure("released once", released.load() == 1);
        ensure("ticket done", t.wait() && t.take() == Status::OK);
        ensure("superseded counted", s.stats().superseded >= 1);
        ensure("newest on disk", readEquals(s, k, b));
        // Skip-if-not-larger.
        Ticket<Status> t2;
        std::vector<U8> small = makeData(12, 100);
        ensure("skip returns true", s.write(k, WriteSource::copy(small.data(), small.size()),
                                            [] { WriteOptions o; o.flags = WF_SKIP_IF_NOT_LARGER; return o; }(), &t2));
        ensure("skip status", t2.isDone() && t2.take() == Status::SKIPPED);
        ensure("still b", readEquals(s, k, b));
        // Only the newest copy is live on disk.
        s.close();
        Store s2;
        ensure("reopen", s2.open(baseConfig(td.path)));
        ensure("b after reopen", readEquals(s2, k, b));
        Stats st = s2.stats();
        ensure_equals("one record", st.records, 1u);
    }

    // ------------------------------------------------------------------
    // Range reads: pread for range kinds, decode + slice for codec kinds.
    template<> template<>
    void cachestore_object::test<3>()
    {
        TempDir td;
        Store s;
        ensure("open", s.open(baseConfig(td.path)));
        std::vector<U8> j = makeData(20, 100000);
        std::vector<U8> g = makeData(21, 50000, true);
        Key kj(idFor(20), Kind::J2C), kg(idFor(21), Kind::GPUTEX);
        s.write(kj, WriteSource::copy(j.data(), j.size()));
        s.write(kg, WriteSource::copy(g.data(), g.size()));
        s.flush();
        U64 before = s.stats().bytes_read;
        ReadResult r = s.readRangeSync(kj, 600, 1000);
        ensure("j2c range", r.status == Status::OK && r.bytes.size() == 1000 && memcmp(r.bytes.data(), j.data() + 600, 1000) == 0);
        ensure("j2c range reads only the range", s.stats().bytes_read - before < 2000);
        r = s.readRangeSync(kj, 99990, 1000);
        ensure("clipped at end", r.status == Status::OK && r.bytes.size() == 10);
        r = s.readRangeSync(kj, 200000, 10);
        ensure("past end = empty", r.status == Status::OK && r.bytes.empty());
        r = s.readRangeSync(kg, 1234, 5000);
        ensure("gpu range", r.status == Status::OK && r.bytes.size() == 5000 && memcmp(r.bytes.data(), g.data() + 1234, 5000) == 0);
        // async + completion on a caller queue
        TestQueue q;
        std::atomic<int> notified{ 0 };
        Completion c;
        c.post = q.poster();
        c.notify = [&] { notified++; };
        Ticket<ReadResult> t = s.readRange(kj, 10, 20, Priority::HIGH, c);
        ensure("async done", t.wait());
        // The completion is posted right after the result is published.
        for (int i = 0; i < 1000 && notified.load() == 0; ++i)
        {
            q.run();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ensure("notified on caller queue", notified.load() == 1);
        ReadResult ar = t.take();
        ensure("async bytes", ar.status == Status::OK && ar.bytes.size() == 20 && memcmp(ar.bytes.data(), j.data() + 10, 20) == 0);
        Ticket<ReadResult> miss = s.read(Key(idFor(77), Kind::J2C));
        ensure("async miss", miss.wait() && miss.take().status == Status::MISSING);
    }

    // ------------------------------------------------------------------
    // Crash at every write step: truncate the segment at every byte boundary
    // of the tail; with no snapshot and with a stale one.
    template<> template<>
    void cachestore_object::test<4>()
    {
        TempDir td;
        const int N = 6;
        std::vector<std::vector<U8>> data;
        std::vector<U64> ends;
        std::string src = td.sub("src");
        {
            Store s;
            Store::Config c = baseConfig(src);
            ensure("open", s.open(c));
            U64 pos = LLCacheStoreFormat::SEG_HEADER_SIZE;
            for (int i = 0; i < N; ++i)
            {
                data.push_back(makeData(100 + i, 40 + i * 13));
                s.write(Key(idFor(100 + i), Kind::J2C), WriteSource::copy(data[i].data(), data[i].size()));
                pos += recSize(0, data[i].size(), false);
                ends.push_back(pos);
            }
            s.flush();
            s.close();
        }
        std::vector<std::string> segs = segmentFiles(src);
        ensure_equals("one segment", segs.size(), (size_t)1);
        std::vector<U8> full = readFile(segs[0]);
        ensure_equals("expected layout", (U64)full.size(), ends.back());
        std::vector<U8> snap = readFile(src + "/index.snap");
        ensure("snapshot written", !snap.empty());
        std::string segname = fs::path(segs[0]).filename().string();

        for (int with_snap = 0; with_snap < 2; ++with_snap)
        {
            for (U64 t = 0; t <= full.size(); ++t)
            {
                std::string d = td.sub("t");
                std::error_code ec;
                fs::remove_all(d, ec);
                fs::create_directories(d, ec);
                writeFile(d + "/" + segname, full.data(), (size_t)t);
                if (with_snap) writeFile(d + "/index.snap", snap.data(), snap.size());
                Store s;
                Store::Config c = baseConfig(d);
                ensure("open truncated", s.open(c));
                for (int i = 0; i < N; ++i)
                {
                    Key k(idFor(100 + i), Kind::J2C);
                    bool complete = ends[i] <= t;
                    if (complete != s.exists(k))
                    {
                        std::string msg = "record " + std::to_string(i) + " at t=" + std::to_string(t) +
                                          (with_snap ? " (stale snapshot)" : "") + (complete ? " lost" : " phantom");
                        fail(msg);
                    }
                    if (complete && !readEquals(s, k, data[i]))
                    {
                        fail("record " + std::to_string(i) + " unreadable at t=" + std::to_string(t));
                    }
                }
                s.close();
                // The owner truncated the torn tail to the last whole record
                // (a segment shorter than its header is removed).
                U64 good = 0;
                for (U64 e : ends) if (e <= t) good = e;
                std::vector<std::string> left = segmentFiles(d);
                if (t < LLCacheStoreFormat::SEG_HEADER_SIZE)
                {
                    if (!left.empty() && fs::file_size(left[0]) != 0 && fs::file_size(left[0]) < LLCacheStoreFormat::SEG_HEADER_SIZE)
                        fail("short segment kept at t=" + std::to_string(t));
                }
                else
                {
                    U64 want = good ? good : LLCacheStoreFormat::SEG_HEADER_SIZE;
                    if (left.empty() || fs::file_size(left[0]) != want)
                        fail("tail not truncated to " + std::to_string(want) + " at t=" + std::to_string(t));
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Corrupt records are dropped one by one; a bad header resynchronises.
    template<> template<>
    void cachestore_object::test<5>()
    {
        TempDir td;
        const int N = 8;
        std::vector<std::vector<U8>> data;
        std::vector<U64> starts;
        {
            Store s;
            ensure("open", s.open(baseConfig(td.path)));
            U64 pos = LLCacheStoreFormat::SEG_HEADER_SIZE;
            for (int i = 0; i < N; ++i)
            {
                data.push_back(makeData(200 + i, 300));
                starts.push_back(pos);
                s.write(Key(idFor(200 + i), Kind::ASSET), WriteSource::copy(data[i].data(), data[i].size()));
                pos += recSize(0, 300, true);
            }
            s.flush();
            s.close();
        }
        std::string seg = segmentFiles(td.path)[0];
        std::vector<U8> bytes = readFile(seg);
        bytes[starts[2] + 96 + 24 + 10] ^= 0x40;   // payload of record 2
        bytes[starts[5] + 30] ^= 0x01;             // header of record 5
        writeFile(seg, bytes.data(), bytes.size());

        // With the snapshot: lookups succeed, the read detects the damage.
        {
            Store s;
            ensure("open with snapshot", s.open(baseConfig(td.path)));
            ensure("record 2 indexed", s.exists(Key(idFor(202), Kind::ASSET)));
            ensure("record 2 corrupt", s.readSync(Key(idFor(202), Kind::ASSET)).status == Status::CORRUPT);
            ensure("record 2 dropped", !s.exists(Key(idFor(202), Kind::ASSET)));
            ensure("record 5 corrupt", s.readSync(Key(idFor(205), Kind::ASSET)).status == Status::CORRUPT);
            for (int i : { 0, 1, 3, 4, 6, 7 })
                ensure("neighbour intact " + std::to_string(i), readEquals(s, Key(idFor(200 + i), Kind::ASSET), data[i]));
            ensure("corrupt counted", s.stats().corrupt_dropped == 2);
            s.close();
        }
        // Without any snapshot: the scan resyncs after record 5's bad header.
        fs::remove(td.sub("index.snap"));
        fs::remove(td.sub("index.snap.prev"));
        {
            Store s;
            ensure("open rebuild", s.open(baseConfig(td.path)));
            ensure("bad header skipped", !s.exists(Key(idFor(205), Kind::ASSET)));
            for (int i : { 0, 1, 3, 4, 6, 7 })
                ensure("found after resync " + std::to_string(i), readEquals(s, Key(idFor(200 + i), Kind::ASSET), data[i]));
            ReadResult r = s.readSync(Key(idFor(202), Kind::ASSET));
            ensure("bad payload never served", r.status != Status::OK);
            ensure("store not wiped", s.stats().records >= 6);
        }
        // A garbage snapshot is ignored (scan), never fatal.
        {
            std::vector<U8> junk = makeData(9, 500);
            writeFile(td.sub("index.snap"), junk.data(), junk.size());
            Store s;
            ensure("open junk snapshot", s.open(baseConfig(td.path)));
            ensure("data still there", readEquals(s, Key(idFor(200), Kind::ASSET), data[0]));
        }
    }

    // ------------------------------------------------------------------
    // Disk full: failed appends, the free-space guard, the RAM fallback for
    // pinned (delivery) records, recovery.
    template<> template<>
    void cachestore_object::test<6>()
    {
        TempDir td;
        std::atomic<bool> fail_writes{ false };
        std::atomic<S64> free_bytes{ (S64)(1ull << 40) };
        Store::Config c = baseConfig(td.path);
        c.write_fault = [&](U32, U64, size_t len) -> size_t { return fail_writes.load() ? len / 2 : len; };
        c.free_space = [&] { return free_bytes.load(); };
        Store s;
        ensure("open", s.open(c));
        s.setDiskReserve(1 << 20);
        std::vector<U8> a = makeData(300, 5000);
        Key ka(idFor(300), Kind::ASSET);
        s.write(ka, WriteSource::copy(a.data(), a.size()));
        s.flush();
        U64 seg_size_before = fs::file_size(segmentFiles(td.path).back());

        fail_writes = true;
        std::vector<U8> b = makeData(301, 6000), p = makeData(302, 7000);
        Key kb(idFor(301), Kind::GPUTEX), kp(idFor(302), Kind::ASSET);
        Ticket<Status> tb, tp;
        s.write(kb, WriteSource::copy(b.data(), b.size()), WriteOptions(), &tb);
        WriteOptions pin;
        pin.flags = WF_PIN_SESSION;
        s.write(kp, WriteSource::copy(p.data(), p.size()), pin, &tp);
        s.flush();
        ensure("failed write reported", tb.wait() && tb.take() == Status::NO_SPACE);
        ensure("pinned write reported", tp.wait() && tp.take() == Status::NO_SPACE);
        ensure("failed record gone", !s.exists(kb));
        ensure("pinned held in RAM", s.exists(kp) && s.stats().ram_fallback_bytes > 0);
        ensure("pinned served once", readEquals(s, kp, p));
        ensure("released after the read", !s.exists(kp) && s.stats().ram_fallback_bytes == 0);
        ensure("partial bytes removed", segmentFiles(td.path).empty() ||
                                        fs::file_size(segmentFiles(td.path).front()) == seg_size_before);
        ensure("old data fine", readEquals(s, ka, a));
        // Backoff: non-delivery writes are refused quickly for a few seconds.
        Ticket<Status> tr;
        bool accepted = s.write(Key(idFor(303), Kind::GPUTEX), WriteSource::copy(b.data(), b.size()), WriteOptions(), &tr);
        ensure("refused during backoff", !accepted && tr.take() == Status::NO_SPACE);

        // Free space below the reserve: guard refuses without touching the disk.
        fail_writes = false;
        free_bytes = 1000;
        Ticket<Status> ta;
        s.write(Key(idFor(304), Kind::ASSET), WriteSource::copy(a.data(), a.size()), WriteOptions(), &ta);
        s.flush();
        ensure("guard refuses", ta.wait() && ta.take() == Status::NO_SPACE);
        // (#711: near a full disk the store shrinks instead of growing, so
        // older records may have been evicted to honour the reserve.)
        ensure("write failures counted", s.stats().write_fail >= 1);

        // Space back: writes work again (after the backoff window).
        free_bytes = (S64)(1ull << 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Ticket<Status> tok;
        s.write(Key(idFor(305), Kind::ASSET), WriteSource::copy(a.data(), a.size()), WriteOptions(), &tok);
        s.flush();
        ensure("recovers", tok.wait() && tok.take() == Status::OK);
        checkInvariants(s, td.path, "disk full");
        s.close();
        Store s2;
        ensure("reopen after failures", s2.open(baseConfig(td.path)));
        ensure("data survives", readEquals(s2, Key(idFor(305), Kind::ASSET), a));
        ensure_equals("verify", s2.verifyAll(nullptr), 0u);
    }

    // ------------------------------------------------------------------
    // Two instances: owner + read-only with a private overflow.
    template<> template<>
    void cachestore_object::test<7>()
    {
        TempDir td;
        Store a;
        ensure("owner opens", a.open(baseConfig(td.path)));
        std::vector<U8> d1 = makeData(400, 2000), d2 = makeData(401, 3000), d3 = makeData(402, 1500);
        Key k1(idFor(400), Kind::ASSET), k2(idFor(401), Kind::ASSET), k3(idFor(402), Kind::ASSET);
        a.write(k1, WriteSource::copy(d1.data(), d1.size()));
        a.flush();
        // Simulate the owner's append in progress: a torn tail.
        std::string seg = segmentFiles(td.path).back();
        U64 owner_size = fs::file_size(seg);
        {
            std::ofstream out(seg, std::ios::binary | std::ios::app);
            out.write("SLCR\x01garbage-in-progress", 24);
        }
        U64 torn_size = fs::file_size(seg);

        Store b;
        Store::Config bc = baseConfig(td.path);
        bc.refresh_interval_s = 3600;
        ensure("second opens", b.open(bc));
        ensure("second is read-only", b.readOnly());
        ensure("reader never truncates the owner's tail", fs::file_size(seg) == torn_size);
        ensure("reader sees owner's record", readEquals(b, k1, d1));
        // Writes from the reader go to its overflow.
        Ticket<Status> tw;
        ensure("reader write accepted", b.write(k2, WriteSource::copy(d2.data(), d2.size()), WriteOptions(), &tw));
        b.flush();
        ensure("reader write ok", tw.wait() && tw.take() == Status::OK);
        ensure("reader reads its overflow", readEquals(b, k2, d2));
        ensure("owner does not see overflow", !a.exists(k2));
        ensure("overflow counted", b.stats().overflow_records == 1);
        // Remove in the reader hides the owner's record for the reader only.
        ensure("reader remove", b.remove(k1));
        ensure("hidden in reader", !b.exists(k1));
        ensure("owner keeps it", readEquals(a, k1, d1));
        // Staged write in the reader commits to the overflow.
        ensure("reader staged", b.stagedAppend(Key(idFor(9), Kind::ASSET), d3.data(), d3.size()));
        ensure("reader commit", b.stagedCommit(Key(idFor(9), Kind::ASSET), k3));
        b.flush();
        ensure("reader committed", readEquals(b, k3, d3));
        // Owner's new records become visible after a refresh.
        // (Repair the fake torn tail first, as the owner would on restart.)
        {
            std::error_code ec;
            fs::resize_file(seg, owner_size, ec);
        }
        std::vector<U8> d4 = makeData(403, 2500);
        Key k4(idFor(403), Kind::ASSET);
        a.write(k4, WriteSource::copy(d4.data(), d4.size()));
        a.flush();
        b.maintain();
        ensure("refresh sees owner's new record", readEquals(b, k4, d4));
        // A third instance is read-only too.
        Store c3;
        ensure("third opens", c3.open(bc));
        ensure("third read-only", c3.readOnly());
        c3.close();
        std::vector<std::string> overflow_dirs;
        for (auto& e : fs::directory_iterator(td.path))
            if (e.path().filename().string().rfind("overflow-", 0) == 0) overflow_dirs.push_back(e.path().string());
        ensure("reader overflow exists", !overflow_dirs.empty());
        b.close();
        bool left = false;
        for (auto& e : fs::directory_iterator(td.path))
            if (e.path().filename().string().rfind("overflow-", 0) == 0) left = true;
        ensure("overflow discarded on exit", !left);
        // Owner exits; a stale overflow folder of a crashed reader is cleaned at the next owner start.
        a.close();
        fs::create_directories(td.sub("overflow-424242-0"));
        writeFile(td.sub("overflow-424242-0/lock"), (const U8*)"x", 1);
        Store a2;
        ensure("owner reopens", a2.open(baseConfig(td.path)));
        ensure("owner again", !a2.readOnly());
        ensure("stale overflow removed", !fs::exists(td.sub("overflow-424242-0")));
        ensure("owner data kept", readEquals(a2, k1, d1) && readEquals(a2, k4, d4));
        ensure("reader's records discarded", !a2.exists(k2));
        // Without overflow, a reader refuses writes.
        Store r2;
        Store::Config nc = baseConfig(td.path);
        nc.overflow = false;
        ensure("reader no overflow", r2.open(nc) && r2.readOnly());
        Ticket<Status> tro;
        ensure("refused", !r2.write(k2, WriteSource::copy(d2.data(), d2.size()), WriteOptions(), &tro));
        ensure("READ_ONLY", tro.take() == Status::READ_ONLY);
        Store::Config strict = baseConfig(td.path);
        strict.read_only_if_locked = false;
        Store r3;
        ensure("strict open fails when locked", !r3.open(strict));
    }

    // ------------------------------------------------------------------
    // Staged writes (per-key table semantics).
    template<> template<>
    void cachestore_object::test<8>()
    {
        TempDir td;
        Key tmp(idFor(500), Kind::ASSET), fin(idFor(501), Kind::ASSET), keep(idFor(502), Kind::ASSET), scratch(idFor(503), Kind::SCRATCH);
        std::vector<U8> d = makeData(500, 10000);
        {
            Store s;
            ensure("open", s.open(baseConfig(td.path)));
            // APPEND per packet, no object lifetime involved
            for (size_t off = 0; off < d.size(); off += 1000)
                ensure("append", s.stagedAppend(tmp, d.data() + off, 1000));
            RecordInfo ri;
            ensure("staged visible", s.lookup(tmp, &ri) && ri.staged && ri.raw_bytes == d.size());
            ensure("isStaged", s.isStaged(tmp));
            ReadResult r = s.readRangeSync(tmp, 2000, 10);
            ensure("staged read", r.status == Status::OK && r.bytes.size() == 10 && memcmp(r.bytes.data(), d.data() + 2000, 10) == 0);
            // WRITE-open restarts it
            ensure("restart", s.stagedBegin(tmp));
            ensure("restarted empty", s.lookup(tmp, &ri) && ri.raw_bytes == 0);
            s.stagedAppend(tmp, d.data(), d.size());
            // rename = commit under the final key
            ensure("commit", s.stagedCommit(tmp, fin));
            ensure("staging gone", !s.isStaged(tmp) && !s.exists(tmp));
            ensure("committed visible at once", readEquals(s, fin, d));
            // remove aborts
            s.stagedAppend(Key(idFor(504), Kind::ASSET), d.data(), 10);
            s.remove(Key(idFor(504), Kind::ASSET));
            ensure("remove aborts staged", !s.exists(Key(idFor(504), Kind::ASSET)));
            s.stagedAppend(Key(idFor(505), Kind::ASSET), d.data(), 10);
            s.stagedAbort(Key(idFor(505), Kind::ASSET));
            ensure("abort", !s.exists(Key(idFor(505), Kind::ASSET)));
            // left open at close: committed (scratch dropped)
            s.stagedAppend(keep, d.data(), 777);
            s.stagedAppend(scratch, d.data(), 55);
            s.close();
        }
        {
            Store s;
            ensure("reopen", s.open(baseConfig(td.path)));
            ensure("final persisted", readEquals(s, fin, d));
            ReadResult r = s.readSync(keep);
            ensure("open staged committed at close", r.status == Status::OK && r.bytes.size() == 777);
            ensure("scratch dropped", !s.exists(scratch));
            // A committed scratch record lives for the session only.
            s.write(scratch, WriteSource::copy(d.data(), 100));
            s.flush();
            ensure("scratch this session", s.exists(scratch));
            s.close();
        }
        {
            Store s;
            ensure("reopen 2", s.open(baseConfig(td.path)));
            ensure("scratch gone next session", !s.exists(scratch));
        }
    }

    // ------------------------------------------------------------------
    // Snapshot loss / staleness -> rebuild by scan; async open.
    template<> template<>
    void cachestore_object::test<9>()
    {
        TempDir td;
        std::string live = td.sub("live"), crash = td.sub("crash");
        std::vector<U8> a = makeData(600, 4000), b = makeData(601, 5000);
        Key ka(idFor(600), Kind::ASSET), kb(idFor(601), Kind::J2C), kr(idFor(602), Kind::ASSET);
        {
            Store s;
            ensure("open", s.open(baseConfig(live)));
            s.write(ka, WriteSource::copy(a.data(), a.size()));
            s.write(kr, WriteSource::copy(a.data(), 100));
            s.flush();
            ensure("snapshot", s.saveSnapshot());
            // after the snapshot: one new record, one removal (tombstone)
            s.write(kb, WriteSource::copy(b.data(), b.size()));
            s.remove(kr);
            s.flush();
            // "crash": copy the files as they are now (stale snapshot)
            copyDir(live, crash);
            fs::remove(crash + "/lock");
            s.close();
        }
        {
            Store s;
            ensure("open crashed copy", s.open(baseConfig(crash)));
            ensure("pre-snapshot record", readEquals(s, ka, a));
            ensure("tail record found by scan", readEquals(s, kb, b));
            ensure("tombstone after snapshot applied", !s.exists(kr));
            s.close();
        }
        // Snapshot gone entirely.
        fs::remove(live + "/index.snap");
        fs::remove(live + "/index.snap.prev");
        {
            Store s;
            ensure("open no snapshot", s.open(baseConfig(live)));
            ensure("rebuilt a", readEquals(s, ka, a));
            ensure("rebuilt b", readEquals(s, kb, b));
            ensure("rebuilt keeps removal", !s.exists(kr));
            s.close();
        }
        // Damaged current snapshot -> previous one.
        {
            std::vector<U8> snap = readFile(live + "/index.snap");
            ensure("snapshot rewritten at close", !snap.empty());
            snap[snap.size() / 2] ^= 0xff;
            writeFile(live + "/index.snap", snap.data(), snap.size());
            Store s;
            ensure("open damaged snapshot", s.open(baseConfig(live)));
            ensure("still a", readEquals(s, ka, a));
            ensure("still b", readEquals(s, kb, b));
            s.close();
        }
        // Async open: writes during the load win over older disk records.
        {
            Store s;
            Store::Config c = baseConfig(live);
            c.async_open = true;
            ensure("async open", s.open(c));
            std::vector<U8> a2 = makeData(603, 999);
            s.write(ka, WriteSource::copy(a2.data(), a2.size()));
            ensure("ready", s.waitReady(std::chrono::seconds(10)));
            ensure("newer write wins", readEquals(s, ka, a2));
            ensure("others loaded", readEquals(s, kb, b));
            s.flush();
            s.close();
            Store s2;
            ensure("reopen", s2.open(baseConfig(live)));
            ensure("newer persisted", readEquals(s2, ka, a2));
        }
    }

    // ------------------------------------------------------------------
    // Eviction + reclaim under budget pressure; GPU-ready share cap.
    template<> template<>
    void cachestore_object::test<10>()
    {
        TempDir td;
        Store::Config c = baseConfig(td.path);
        c.segment_bytes = 1 << 20;
        c.run_flush_bytes = 64 << 10;
        Store s;
        ensure("open", s.open(c));
        s.setBudget(Group::ASSETS, 3u << 20);
        s.setBudget(Group::TEXTURES, 4u << 20);
        WearPolicy wp;
        wp.daily_compaction_fraction = 1.f;      // rescue allowed for this test
        s.setWearPolicy(wp);
        const int N = 400;                       // ~8 MB of assets, 20 KB each
        for (int i = 0; i < N; ++i)
        {
            std::vector<U8> d = makeData(1000 + i, 20000);
            s.write(Key(idFor(1000 + i), Kind::ASSET), WriteSource::take(std::move(d)));
            if (i % 50 == 49) s.flush();
        }
        s.flush();
        s.maintain();
        Stats st = s.stats();
        ensure("assets within budget (+1 segment slack)",
               st.groups[(size_t)Group::ASSETS].disk_bytes <= (3u << 20) + (1u << 20));
        ensure("segments reclaimed", st.segments_reclaimed > 0);
        ensure("not wiped", st.groups[(size_t)Group::ASSETS].records > 50);
        ensure("newest survive", readEquals(s, Key(idFor(1000 + N - 1), Kind::ASSET), makeData(1000 + N - 1, 20000)));
        U32 bad = s.verifyAll(nullptr);
        ensure_equals("all remaining readable", bad, 0u);
        // Index and disk agree after eviction.
        U64 live = 0;
        s.forEach(Kind::ASSET, [&](const RecordInfo& r) { live += LLCacheStoreFormat::recordSize(r.meta_len, r.stored_bytes); return true; });
        ensure_equals("live accounting", live, s.stats().kinds[(size_t)Kind::ASSET].live_bytes);

        // Share cap: GPU-ready limited to 30% of Textures; J2C is not evicted for it.
        for (int i = 0; i < 40; ++i)
        {
            std::vector<U8> j = makeData(2000 + i, 30000);
            s.write(Key(idFor(2000 + i), Kind::J2C), WriteSource::take(std::move(j)));
        }
        for (int i = 0; i < 80; ++i)
        {
            std::vector<U8> g = makeData(3000 + i, 30000);     // incompressible
            s.write(Key(idFor(3000 + i), Kind::GPUTEX), WriteSource::take(std::move(g)));
        }
        s.flush();
        s.maintain();
        st = s.stats();
        ensure("gpu-ready within its share", st.kinds[(size_t)Kind::GPUTEX].live_bytes <= (U64)((4u << 20) * 0.3) + 40000);
        ensure_equals("all J2C kept", st.kinds[(size_t)Kind::J2C].records, 40u);
        checkInvariants(s, td.path, "eviction");
    }

    // ------------------------------------------------------------------
    // #709 wear rules: no compaction under budget; reclaim only <= 25% live;
    // rescue only reused records within the daily cap; past it, no rescue.
    template<> template<>
    void cachestore_object::test<11>()
    {
        TempDir td;
        std::atomic<U32> clock{ 1700000000u };
        Store::Config c = baseConfig(td.path);
        c.segment_bytes = 1 << 20;
        c.run_flush_bytes = 64 << 10;
        c.clock = [&] { return clock.load(); };
        const int N = 160;                    // 160 x 20 KB = 3.2 MB -> 4 segments
        {
            Store s;
            ensure("open", s.open(c));
            s.setBudget(Group::ASSETS, 64u << 20);
            for (int i = 0; i < N; ++i)
                s.write(Key(idFor(5000 + i), Kind::ASSET), WriteSource::take(makeData(5000 + i, 20000)));
            s.flush();
            s.close();
        }
        clock += 3600;
        Store s;
        ensure("reopen", s.open(c));
        // Reuse: records 0..9 touched in this (later) session.
        for (int i = 0; i < 10; ++i) s.touch(Key(idFor(5000 + i), Kind::ASSET));
        RecordInfo ri;
        ensure("reuse counted", s.lookup(Key(idFor(5000), Kind::ASSET), &ri) && ri.reuse_count == 1);
        // Kill most of segment 1's records (it holds the first ~50).
        for (int i = 10; i < 50; ++i) s.remove(Key(idFor(5000 + i), Kind::ASSET));   // segment 1 -> 12/52 live
        s.flush();
        Stats before = s.stats();
        s.maintain();
        Stats st = s.stats();
        ensure_equals("no compaction while under budget", st.bytes_written_today_compaction, before.bytes_written_today_compaction);
        ensure_equals("nothing reclaimed under budget", st.segments_reclaimed, before.segments_reclaimed);
        // Now press: budget below usage.
        WearPolicy wp;
        wp.daily_compaction_cap_bytes = 1u << 20;    // plenty for 10 x 20 KB
        wp.daily_compaction_fraction = 0.f;
        s.setWearPolicy(wp);
        s.setBudget(Group::ASSETS, st.groups[(size_t)Group::ASSETS].disk_bytes - 200000);
        s.maintain();
        st = s.stats();
        ensure("a mostly-dead segment was reclaimed", st.segments_reclaimed > before.segments_reclaimed);
        ensure("reused records rescued", st.rescued_records >= 10);
        for (int i = 0; i < 10; ++i)
            ensure("rescued readable " + std::to_string(i), readEquals(s, Key(idFor(5000 + i), Kind::ASSET), makeData(5000 + i, 20000)));
        ensure("compaction counted", st.bytes_written_today_compaction > 0);
        ensure("compaction within cap", st.bytes_written_today_compaction <= (1u << 20));
        ensure("only reused rescued (no unreused survivors from the reclaimed segment)", st.rescued_records <= 10);
        s.close();

        // Past the cap: whole segments go without rescue.
        clock += 86400;           // new day: the day's new bytes are 0 -> cap 0
        Store s2;
        ensure("reopen 2", s2.open(c));
        for (int i = 0; i < 10; ++i) s2.touch(Key(idFor(5000 + i), Kind::ASSET));
        WearPolicy none;
        none.daily_compaction_fraction = 0.1f;   // 10% of 0 new bytes today
        s2.setWearPolicy(none);
        Stats b2 = s2.stats();
        ensure("new day resets counters", b2.bytes_written_today_compaction == 0);
        s2.setBudget(Group::ASSETS, 1u << 20);
        s2.maintain();
        Stats st2 = s2.stats();
        ensure("no rescue past the cap", st2.rescued_records == 0 && st2.bytes_written_today_compaction == 0);
        ensure("but space still reclaimed", st2.groups[(size_t)Group::ASSETS].disk_bytes <= (2u << 20));
        checkInvariants(s2, td.path, "wear");
    }

    // ------------------------------------------------------------------
    // Clear Cache frees disk now; per-kind clear survives a rebuild.
    template<> template<>
    void cachestore_object::test<12>()
    {
        TempDir td;
        Store::Config c = baseConfig(td.path);
        c.segment_bytes = 1 << 20;
        c.run_flush_bytes = 32 << 10;
        {
            Store s;
            ensure("open", s.open(c));
            for (int i = 0; i < 120; ++i)
            {
                Kind k = (i % 2) ? Kind::J2C : Kind::ASSET;
                s.write(Key(idFor(6000 + i), k), WriteSource::take(makeData(6000 + i, 20000)));
            }
            s.flush();
            U64 disk_before = s.stats().disk_bytes;
            s.removeAll(Kind::J2C);
            Stats st = s.stats();
            ensure("disk freed at once", st.disk_bytes < disk_before * 6 / 10);
            ensure("j2c gone", st.kinds[(size_t)Kind::J2C].records == 0 && !s.exists(Key(idFor(6001), Kind::J2C)));
            ensure_equals("assets kept", st.kinds[(size_t)Kind::ASSET].records, 60u);
            ensure("asset readable", readEquals(s, Key(idFor(6000), Kind::ASSET), makeData(6000, 20000)));
            checkInvariants(s, td.path, "removeAll");
            s.close();
        }
        fs::remove(td.sub("index.snap"));
        fs::remove(td.sub("index.snap.prev"));
        {
            Store s;
            ensure("rebuild", s.open(c));
            ensure("clear survives rebuild", !s.exists(Key(idFor(6001), Kind::J2C)));
            ensure("assets after rebuild", readEquals(s, Key(idFor(6002), Kind::ASSET), makeData(6002, 20000)));
            s.clearAll();
            Stats st = s.stats();
            ensure("clearAll deletes segments", st.segments == 0 && st.disk_bytes == 0 && st.records == 0);
            ensure("no segment files", segmentFiles(td.path).empty());
            // still usable
            s.write(Key(idFor(1), Kind::ASSET), WriteSource::take(makeData(1, 10)));
            s.flush();
            ensure("usable after clear", readEquals(s, Key(idFor(1), Kind::ASSET), makeData(1, 10)));
        }
    }

    // ------------------------------------------------------------------
    // Regions: runs, promotion to shared, manifests, prefetch, handle change.
    template<> template<>
    void cachestore_object::test<13>()
    {
        TempDir td;
        LLUUID ra = idFor(70001), rb = idFor(70002);
        Store::Config c = baseConfig(td.path);
        c.run_flush_ms = 60000;
        c.prefetch_mode = Store::Config::PrefetchMode::BUFFER;
        {
            Store s;
            ensure("open", s.open(c));
            s.regionVisit(ra, 1000);
            // Interleaved arrivals from two regions + shared.
            for (int i = 0; i < 60; ++i)
            {
                WriteOptions o;
                o.placement = (i % 3 == 0) ? PlacementHint::inRegion(ra, 1000)
                            : (i % 3 == 1) ? PlacementHint::inRegion(rb, 2000) : PlacementHint::shared();
                s.write(Key(idFor(7000 + i), Kind::J2C), WriteSource::take(makeData(7000 + i, 3000)), o);
            }
            s.flush();
            RegionInfo info;
            ensure("region a known", s.regionInfo(ra, &info));
            ensure_equals("a owns its records", info.own_records, 20u);
            // A record written in A, later used from B, becomes shared (no copy).
            Key ka0(idFor(7000), Kind::J2C);
            s.noteUse(ka0, PlacementHint::inRegion(rb, 2000));
            RecordInfo ri;
            ensure("promoted", s.lookup(ka0, &ri) && ri.shared && ri.region == ra);
            ensure("still served", readEquals(s, ka0, makeData(7000, 3000)));
            // Wrong hint: still correct.
            WriteOptions wrong;
            wrong.placement = PlacementHint::inRegion(idFor(1234567), 9);
            s.write(Key(idFor(7999), Kind::J2C), WriteSource::take(makeData(7999, 100)), wrong);
            s.flush();
            ensure("wrong hint served", readEquals(s, Key(idFor(7999), Kind::J2C), makeData(7999, 100)));
            s.regionVisit(rb, 2000);
            s.close();   // manifests saved
        }
        {
            Store s;
            ensure("reopen", s.open(c));
            ensure("manifest stored", s.exists(Key(rb, Kind::MANIFEST)));
            // Grid handle change for the same region UUID.
            s.regionVisit(rb, 3000);
            RegionInfo info;
            ensure("info b", s.regionInfo(rb, &info));
            ensure_equals("handle follows the UUID", info.grid_handle, (U64)3000);
            ensure("visits counted", info.visit_count >= 2);
            ensure("shared refs from manifest", info.shared_refs >= 1);
            // A manifest reference to a record that is gone is harmless.
            s.remove(Key(idFor(7000), Kind::J2C));
            ensure("prefetch b", s.prefetchRegion(rb));
            for (int i = 0; i < 200 && s.stats().prefetch_bytes == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ensure("prefetched", s.stats().prefetch_bytes > 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ensure("prefetched record served", readEquals(s, Key(idFor(7001), Kind::J2C), makeData(7001, 3000)));
            ensure("served from the read-ahead buffer", s.stats().prefetch_hits > 0);
            s.cancelPrefetch(rb);
            ensure("prefetch unknown region", !s.prefetchRegion(idFor(4242)));
        }
    }

    // ------------------------------------------------------------------
    // Rename (re-key without copy), format_version bump, tickets, drive type.
    template<> template<>
    void cachestore_object::test<14>()
    {
        TempDir td;
        Store s;
        ensure("open", s.open(baseConfig(td.path)));
        std::vector<U8> d = makeData(800, 4000);
        Key from(idFor(800), Kind::ASSET), to(idFor(801), Kind::ASSET);
        s.write(from, WriteSource::copy(d.data(), d.size()));
        s.flush();
        ensure("rename", s.rename(from, to));
        ensure("old gone", !s.exists(from));
        ensure("new reads", readEquals(s, to, d));
        s.close();
        ensure("reopen", s.open(baseConfig(td.path)));
        ensure("rename persisted via snapshot", readEquals(s, to, d) && !s.exists(from));
        // format version bump drops only that kind
        s.write(Key(idFor(802), Kind::GPUTEX), WriteSource::take(makeData(802, 1000, true)));
        s.flush();
        KindPolicy p = s.kindPolicy(Kind::GPUTEX);
        p.format_version = 2;
        s.setKindPolicy(Kind::GPUTEX, p);
        ensure("old version dropped", !s.exists(Key(idFor(802), Kind::GPUTEX)));
        ensure("other kind kept", s.exists(to));
        // A cancelled queued read completes CANCELLED without notifying.
        std::vector<Ticket<ReadResult>> busy;
        for (int i = 0; i < 300; ++i) busy.push_back(s.read(to, Priority::HIGH));
        std::atomic<int> notified{ 0 };
        Completion c;
        c.notify = [&] { notified++; };
        Ticket<ReadResult> t = s.read(to, Priority::LOW, c);
        t.cancel();
        ensure("cancelled ticket finishes", t.wait());
        if (t.take().status == Status::CANCELLED)
            ensure("no notify after cancel", notified.load() == 0);
        for (auto& b : busy) b.wait();
        DriveType dt = detectDriveType(td.path);
        ensure("drive detection does not crash", dt == DriveType::SSD || dt == DriveType::HDD || dt == DriveType::UNKNOWN);
        ensure("stats line", !s.statsLine().empty());
    }

    // ------------------------------------------------------------------
    // Concurrency smoke: writers + readers on many threads.
    template<> template<>
    void cachestore_object::test<15>()
    {
        TempDir td;
        Store::Config c = baseConfig(td.path);
        c.read_threads = 3;
        c.segment_bytes = 1 << 20;
        Store s;
        ensure("open", s.open(c));
        std::atomic<int> bad{ 0 };
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
        {
            th.emplace_back([&, t]
            {
                for (int i = 0; i < 200; ++i)
                {
                    U32 n = 9000 + t * 1000 + i;
                    std::vector<U8> d = makeData(n, 500 + (n % 7) * 300);
                    Key k(idFor(n), (n & 1) ? Kind::J2C : Kind::ASSET);
                    s.write(k, WriteSource::copy(d.data(), d.size()));
                    ReadResult r = s.readSync(k);
                    if (r.status != Status::OK || r.bytes != d) bad++;
                    if (i % 10 == 0)
                    {
                        Ticket<ReadResult> tk = s.read(k);
                        tk.wait();
                        ReadResult ar = tk.take();
                        if (ar.status != Status::OK || ar.bytes != d) bad++;
                    }
                }
            });
        }
        for (auto& t : th) t.join();
        s.flush();
        ensure_equals("all reads correct", bad.load(), 0);
        ensure_equals("verify", s.verifyAll(nullptr), 0u);
        ensure_equals("records", s.stats().records, 800u);
    }

    // ------------------------------------------------------------------
    // Disk-full guard against a modelled disk (free = capacity - files), with
    // mostly dead segments so compaction rescue runs under the guard.
    template<> template<>
    void cachestore_object::test<16>()
    {
        TempDir td;
        U64 capacity = 9u << 20;
        std::string dir = td.path;
        auto used = [dir]
        {
            U64 n = 0;
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file(ec)) n += e.file_size(ec);
            return n;
        };
        Store::Config c = baseConfig(dir);
        c.segment_bytes = 1 << 20;
        c.run_flush_bytes = 64 << 10;
        c.free_space = [capacity, used] { U64 u = used(); return (S64)(u < capacity ? capacity - u : 0); };
        const int N = 200;                     // 200 x 20 KB = ~4 MB
        {
            Store s;
            ensure("open", s.open(c));
            s.setDiskReserve(1u << 20);
            for (int i = 0; i < N; ++i)
                s.write(Key(idFor(20000 + i), Kind::ASSET), WriteSource::take(makeData(20000 + i, 20000)));
            s.flush();
            s.close();
        }
        Store s;
        ensure("reopen", s.open(c));
        s.setDiskReserve(1u << 20);
        WearPolicy wp;
        wp.daily_compaction_cap_bytes = 64u << 20;
        s.setWearPolicy(wp);
        // reuse on every 5th record; all others die (segments ~20% live)
        for (int i = 0; i < N; i += 5) s.touch(Key(idFor(20000 + i), Kind::ASSET));
        for (int i = 0; i < N; ++i)
            if (i % 5) s.remove(Key(idFor(20000 + i), Kind::ASSET));
        s.flush();
        checkInvariants(s, dir, "before pressure");
        // Fill past the modelled disk: the guard binds on every append.
        int ok = 0, refused = 0;
        for (int i = 0; i < 300; ++i)
        {
            Ticket<Status> t;
            s.write(Key(idFor(30000 + i), Kind::ASSET), WriteSource::take(makeData(30000 + i, 20000)), WriteOptions(), &t);
            if (i % 20 == 19) s.flush();
            if (t.isDone() && t.take() != Status::OK) ++refused;
        }
        s.flush();
        s.maintain();
        checkInvariants(s, dir, "under the guard");
        Stats st = s.stats();
        ensure("reserve honoured", used() <= capacity - (1u << 20) + (2u << 20));
        ensure("compaction rescued reused records", st.rescued_records > 0);
        ensure("segments reclaimed", st.segments_reclaimed > 0);
        U32 bad = s.verifyAll(nullptr);
        ensure_equals("everything indexed is readable", bad, 0u);
        for (int i = 0; i < 300; ++i)
            if (s.exists(Key(idFor(30000 + i), Kind::ASSET))) ++ok;
        ensure("newest writes kept", ok > 0);
        ensure("delivery writes not refused under the guard", refused == 0);
        s.close();
        Store s2;
        ensure("reopen after pressure", s2.open(c));
        checkInvariants(s2, dir, "after reopen");
        ensure_equals("verify after reopen", s2.verifyAll(nullptr), 0u);
    }

    // ------------------------------------------------------------------
    // Read scheduling helpers: a big record whose zstd decode is handed to the decode
    // pool (compresses, but not to nothing), a gate a store thread blocks on (via
    // Config::job_hook), and an ordered completion log.

    std::vector<U8> textureLike(U32 seed, size_t size)
    {
        std::vector<U8> v = makeData(seed, size);
        for (size_t i = 0; i < size; ++i)
            if (i % 4) v[i] = (U8)('a' + (i % 7));
        return v;
    }

    struct Gate
    {
        std::mutex m;
        std::condition_variable cv;
        bool entered = false, open = false;
        void enter()
        {
            std::unique_lock<std::mutex> l(m);
            entered = true;
            cv.notify_all();
            cv.wait(l, [this] { return open; });
        }
        bool waitEntered()
        {
            std::unique_lock<std::mutex> l(m);
            return cv.wait_for(l, std::chrono::seconds(10), [this] { return entered; });
        }
        void release()
        {
            std::lock_guard<std::mutex> l(m);
            open = true;
            cv.notify_all();
        }
    };

    struct OrderLog
    {
        std::mutex m;
        std::vector<int> order;
        Completion tag(int n)
        {
            Completion c;
            c.wake = [this, n] { std::lock_guard<std::mutex> l(m); order.push_back(n); };
            return c;
        }
        std::vector<int> get() { std::lock_guard<std::mutex> l(m); return order; }
    };

    bool waitFor(const std::function<bool()>& cond)
    {
        for (int i = 0; i < 5000; ++i)
        {
            if (cond()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return cond();
    }

    // ------------------------------------------------------------------
    // Decode handoff: big zstd records decode on the pool, small / codec-0 ones on the
    // reader; whole and ranged reads return the right bytes either way, also with a
    // decode queue so small that readers mostly decode inline (backpressure).
    template<> template<>
    void cachestore_object::test<17>()
    {
        for (U64 qbytes : { 64ull << 20, 1ull })
        {
            TempDir td;
            Store::Config c = baseConfig(td.path);
            c.read_threads = 2;
            c.decode_threads = 2;
            c.decode_queue_bytes = qbytes;
            Store s;
            ensure("open", s.open(c));
            const int N = 24;
            std::vector<std::vector<U8>> big, small;
            for (int i = 0; i < N; ++i)
            {
                big.push_back(textureLike(40000 + i, 200000 + i * 1000));
                small.push_back(makeData(41000 + i, 700 + i));
                s.write(Key(idFor(40000 + i), Kind::GPUTEX), WriteSource::copy(big[i].data(), big[i].size()));
                s.write(Key(idFor(41000 + i), (i & 1) ? Kind::J2C : Kind::ASSET), WriteSource::copy(small[i].data(), small[i].size()));
            }
            s.flush();
            RecordInfo ri;
            ensure("big record compressed", s.lookup(Key(idFor(40000), Kind::GPUTEX), &ri) && ri.codec == 1
                                            && ri.stored_bytes < ri.raw_bytes / 2);
            std::vector<Ticket<ReadResult>> tb, ts, tr;
            for (int i = 0; i < N; ++i)
            {
                tb.push_back(s.read(Key(idFor(40000 + i), Kind::GPUTEX), (Priority)(i % 4)));
                ts.push_back(s.read(Key(idFor(41000 + i), (i & 1) ? Kind::J2C : Kind::ASSET), Priority::HIGH));
                tr.push_back(s.readRange(Key(idFor(40000 + i), Kind::GPUTEX), 1000 + i, 5000));
            }
            int bad = 0;
            for (int i = 0; i < N; ++i)
            {
                tb[i].wait(); ts[i].wait(); tr[i].wait();
                ReadResult a = tb[i].take(), b = ts[i].take(), r = tr[i].take();
                if (a.status != Status::OK || a.bytes != big[i]) ++bad;
                if (b.status != Status::OK || b.bytes != small[i]) ++bad;
                if (r.status != Status::OK || r.bytes.size() != 5000
                    || memcmp(r.bytes.data(), big[i].data() + 1000 + i, 5000) != 0) ++bad;
            }
            ensure_equals("handoff reads correct", bad, 0);
            Stats st = s.stats();
            ensure_equals("read jobs", st.read_jobs, (U64)(3 * N));
            ensure_equals("all completed", st.read_done_count, (U64)(3 * N));
            ensure("decodes handed off", st.decode_handoffs > 0);
            ensure("every codec-1 decode accounted", st.decode_handoffs + st.decode_inline >= (U64)(2 * N));
            ensure("pool sizes", st.read_threads == 2 && st.decode_threads >= 1);
            ensure("queues drained", st.read_queue_depth == 0 && st.decode_queue_depth == 0 && st.decode_queue_bytes == 0);
            ensure("no reads left in flight", st.reads_in_flight == 0);
            ensure("stats line has the split", s.statsLine().find("decode-wait") != std::string::npos);
            ensure_equals("verify", s.verifyAll(nullptr), 0u);
        }
    }

    // ------------------------------------------------------------------
    // Integrity is unchanged by the handoff: a bad stored payload is caught on the
    // reader (payload hash), a payload whose hash was re-signed but whose zstd data
    // is wrong is caught on the decode pool (the codec's hash of the decoded bytes);
    // both report CORRUPT and drop the record from the index.
    template<> template<>
    void cachestore_object::test<18>()
    {
        TempDir td;
        Key k1(idFor(50001), Kind::GPUTEX), k2(idFor(50002), Kind::GPUTEX), k3(idFor(50003), Kind::GPUTEX);
        std::vector<U8> d1 = textureLike(50001, 300000), d2 = textureLike(50002, 300000), d3 = textureLike(50003, 300000);
        U32 st1 = 0, st2 = 0;
        Store::Config c = baseConfig(td.path);
        c.read_threads = 1;
        c.decode_threads = 1;
        {
            Store s;
            ensure("open", s.open(c));
            s.write(k1, WriteSource::copy(d1.data(), d1.size()));
            s.write(k2, WriteSource::copy(d2.data(), d2.size()));
            s.write(k3, WriteSource::copy(d3.data(), d3.size()));
            s.flush();
            RecordInfo ri;
            ensure("k1", s.lookup(k1, &ri) && ri.codec == 1 && ri.meta_len == 0);
            st1 = ri.stored_bytes;
            ensure("k2", s.lookup(k2, &ri));
            st2 = ri.stored_bytes;
            s.close();
        }
        namespace F = LLCacheStoreFormat;
        std::string seg = segmentFiles(td.path)[0];
        std::vector<U8> bytes = readFile(seg);
        const U64 r1 = F::SEG_HEADER_SIZE;
        const U64 r2 = r1 + F::recordSize(0, st1);
        // k1: flip a payload byte only -> the stored-payload hash fails on the reader.
        bytes[r1 + F::REC_HEADER_SIZE + 24 + st1 / 2] ^= 0x5a;
        // k2: flip a byte in the zstd data, then re-sign the payload hash and the header
        // hash -> only the decode can tell.
        U8* rec = bytes.data() + r2;
        rec[F::REC_HEADER_SIZE + 24 + st2 / 2] ^= 0x5a;
        F::put64(rec + 72, HBXXH64::digest(rec + F::REC_HEADER_SIZE, st2));
        HBXXH64 hh;
        hh.update(rec, 88);
        F::put64(rec + 88, hh.digest());
        writeFile(seg, bytes.data(), bytes.size());

        Store s;
        ensure("reopen", s.open(c));
        ensure("both indexed", s.exists(k1) && s.exists(k2));
        Ticket<ReadResult> t1 = s.read(k1), t2 = s.read(k2), t3 = s.read(k3);
        ensure("done", t1.wait() && t2.wait() && t3.wait());
        ensure("payload damage: CORRUPT", t1.take().status == Status::CORRUPT);
        ensure("decode damage: CORRUPT", t2.take().status == Status::CORRUPT);
        ReadResult r3 = t3.take();
        ensure("neighbour intact", r3.status == Status::OK && r3.bytes == d3);
        ensure("k1 dropped", !s.exists(k1));
        ensure("k2 dropped", !s.exists(k2));
        Stats st = s.stats();
        ensure_equals("corrupt counted", st.corrupt_dropped, (U64)2);
        ensure_equals("k2 + k3 decoded on the pool (k1 never got that far)", st.decode_handoffs, (U64)2);
        ensure("sync read sees the drop", s.readSync(k2).status == Status::MISSING);
    }

    // ------------------------------------------------------------------
    // Priorities: the I/O queue and the decode queue each serve URGENT > HIGH >
    // NORMAL > LOW (FIFO within one), and a small read is not held up by a decode.
    template<> template<>
    void cachestore_object::test<19>()
    {
        TempDir td;
        Gate io_gate, dec_gate;
        OrderLog io_log, dec_log;
        const Key io_block(idFor(60000), Kind::ASSET), dec_block(idFor(60001), Kind::GPUTEX);
        Store::Config c = baseConfig(td.path);
        c.read_threads = 1;
        c.decode_threads = 1;
        c.job_hook = [&](const Key& k, bool decode)
        {
            if (!decode && k == io_block) io_gate.enter();
            if (decode && k == dec_block) dec_gate.enter();
        };
        Store s;
        ensure("open", s.open(c));
        s.write(io_block, WriteSource::take(makeData(60000, 500)));
        s.write(dec_block, WriteSource::take(textureLike(60001, 200000)));
        for (int p = 0; p < 4; ++p)
        {
            s.write(Key(idFor(60100 + p), Kind::ASSET), WriteSource::take(makeData(60100 + p, 800)));
            s.write(Key(idFor(60200 + p), Kind::GPUTEX), WriteSource::take(textureLike(60200 + p, 150000)));
        }
        s.write(Key(idFor(60300), Kind::MESH), WriteSource::take(makeData(60300, 900)));
        s.flush();

        // I/O queue: the only reader is held; LOW, NORMAL, HIGH, URGENT queue behind it.
        Ticket<ReadResult> held = s.read(io_block);
        ensure("reader held", io_gate.waitEntered());
        std::vector<Ticket<ReadResult>> t;
        for (int p = 0; p < 4; ++p) t.push_back(s.read(Key(idFor(60100 + p), Kind::ASSET), (Priority)p, io_log.tag(p)));
        ensure("queued", waitFor([&] { return s.stats().read_queue_depth == 4; }));
        io_gate.release();
        for (auto& x : t) ensure("io read done", x.wait() && x.take().status == Status::OK);
        ensure("held done", held.wait());
        ensure("I/O queue by priority", io_log.get() == std::vector<int>({ 3, 2, 1, 0 }));

        // Decode queue: the only decoder is held; four big decodes queue behind it.
        Ticket<ReadResult> dheld = s.read(dec_block);
        ensure("decoder held", dec_gate.waitEntered());
        t.clear();
        for (int p = 0; p < 4; ++p) t.push_back(s.read(Key(idFor(60200 + p), Kind::GPUTEX), (Priority)p, dec_log.tag(p)));
        ensure("handed off", waitFor([&] { return s.stats().decode_queue_depth == 4; }));
        ensure("decode queue bytes", s.stats().decode_queue_bytes == 4ull * 150000);
        // A small read completes on the reader while every decode waits.
        Ticket<ReadResult> mesh = s.read(Key(idFor(60300), Kind::MESH), Priority::NORMAL);
        ensure("small read not behind decodes", mesh.wait(std::chrono::milliseconds(10000)) && mesh.take().status == Status::OK);
        ensure("decodes still queued", s.stats().decode_queue_depth == 4);
        dec_gate.release();
        for (auto& x : t) ensure("decode read done", x.wait() && x.take().status == Status::OK);
        ensure("dheld done", dheld.wait() && dheld.take().status == Status::OK);
        ensure("decode queue by priority", dec_log.get() == std::vector<int>({ 3, 2, 1, 0 }));
        Stats st = s.stats();
        ensure("reads counted by priority", st.reads_by_priority[0] == 2 && st.reads_by_priority[3] == 2);
        ensure("wait time recorded", st.read_wait_usec > 0 && st.decode_wait_usec > 0);
    }

    // ------------------------------------------------------------------
    // Shutdown with work queued in both stages: close() returns, every ticket completes
    // exactly once (OK, or CANCELLED for what never ran), nothing hangs.
    template<> template<>
    void cachestore_object::test<20>()
    {
        TempDir td;
        Gate dec_gate;
        const Key first(idFor(70000), Kind::GPUTEX);
        Store::Config c = baseConfig(td.path);
        c.read_threads = 1;
        c.decode_threads = 1;
        // The decoder is held on `first`; the reader takes ~1 ms per job, so a burst
        // leaves work in both queues when close() starts.
        c.job_hook = [&](const Key& k, bool decode)
        {
            if (!decode) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (decode && k == first) dec_gate.enter();
        };
        const int N = 20, R = 200;
        std::atomic<int> wakes{ 0 };
        std::vector<Ticket<ReadResult>> t;
        {
            Store s;
            ensure("open", s.open(c));
            for (int i = 0; i < N; ++i) s.write(Key(idFor(70000 + i), Kind::GPUTEX), WriteSource::take(textureLike(70000 + i, 100000)));
            s.flush();
            Ticket<ReadResult> held = s.read(first, Priority::URGENT);
            ensure("decoder held", dec_gate.waitEntered());
            for (int i = 0; i < R; ++i)
            {
                Completion cp;
                cp.wake = [&wakes] { wakes++; };
                t.push_back(s.read(Key(idFor(70000 + (i % N)), Kind::GPUTEX), (Priority)(i % 4), cp));
            }
            ensure("work queued in both stages", waitFor([&] { Stats st = s.stats(); return st.decode_queue_depth > 0; })
                                                 && s.stats().read_queue_depth > 0);
            std::thread releaser([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); dec_gate.release(); });
            const auto t0 = std::chrono::steady_clock::now();
            s.close();
            releaser.join();
            ensure("close is prompt", std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
            int ok = 0, cancelled = 0, other = 0;
            for (auto& x : t)
            {
                if (!x.isDone()) { ++other; continue; }
                Status st = x.take().status;
                if (st == Status::OK) ++ok;
                else if (st == Status::CANCELLED) ++cancelled;
                else ++other;
            }
            ensure_equals("every ticket finished OK or CANCELLED", other, 0);
            ensure_equals("all accounted", ok + cancelled, R);
            ensure("queued work cancelled", cancelled > 0);
            ensure_equals("each completion ran once", wakes.load(), R);
            ensure("the job in hand finished", held.isDone() && held.take().status == Status::OK);
            ensure("read after close", s.read(first).wait() && !s.isOpen());
        }
        // Reopen: nothing lost, nothing in flight.
        Store s;
        ensure("reopen", s.open(c));
        ensure_equals("records kept", s.stats().records, (U32)N);
        ensure_equals("verify", s.verifyAll(nullptr), 0u);
    }
}

namespace
{
    U64 splitmix(U64& x)
    {
        U64 z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Heap bytes in use (glibc); 0 elsewhere.
    U64 heapInUse()
    {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
        struct mallinfo2 mi = mallinfo2();
        return (U64)mi.uordblks + (U64)mi.hblkhd;
#else
        return 0;
#endif
    }

    // The #866 benchmark mix: ~80% mesh parts, ~10% j2c, ~7% assets, ~3% GPU-ready
    // with 27 bytes of meta.
    Key benchKey(U32 i, U64 rnd, U8* meta, U8& meta_len)
    {
        U64 x = 0x1234567ull + i;
        LLUUID id;
        U64 a = splitmix(x), b = splitmix(x);
        memcpy(id.mData, &a, 8);
        memcpy(id.mData + 8, &b, 8);
        meta_len = 0;
        U32 r = (U32)(rnd % 100);
        if (r < 80) return Key(id, Kind::MESH, (U32)(rnd >> 8) % 12);
        if (r < 90) return Key(id, Kind::J2C);
        if (r < 97) return Key(id, Kind::ASSET);
        meta_len = 27;
        for (U8 k = 0; k < 27; ++k) meta[k] = (U8)((U8)(a >> (k % 8)) + k);
        return Key(id, Kind::GPUTEX, (U32)(rnd >> 16) % 3);
    }
}

namespace tut
{
    // ------------------------------------------------------------------
    // #866 measurement: index RAM per record, snapshot save / load time, and the
    // worst single write() (an index grow happens under the shard lock). Record
    // count from LLCACHESTORE_BENCH_RECORDS (default 30000; the ticket's case is
    // 1200000).
    template<> template<>
    void cachestore_object::test<21>()
    {
        U32 n = 30000;
        if (auto e = LLStringUtil::getoptenv("LLCACHESTORE_BENCH_RECORDS")) n = std::max<U32>((U32)atoi(e->c_str()), 1000);
        TempDir td;
        Store::Config c = baseConfig(td.path);
        c.max_queued_bytes = 4ull << 30;
        c.run_flush_bytes = 16u << 20;
        std::vector<U8> payload(16);
        U64 write_max_usec = 0;
        {
            Store s;
            ensure("open", s.open(c));
            for (size_t g = 0; g < (size_t)Group::UNBUDGETED; ++g) s.setBudget((Group)g, 1ull << 40);
            U64 rs = 99;
            for (U32 i = 0; i < n; ++i)
            {
                U8 meta[32];
                U8 ml;
                Key k = benchKey(i, splitmix(rs), meta, ml);
                memcpy(payload.data(), &i, 4);
                WriteOptions o;
                o.meta = ml ? meta : nullptr;
                o.meta_len = ml;
                const auto t0 = std::chrono::steady_clock::now();
                bool ok = s.write(k, WriteSource::copy(payload.data(), payload.size()), o);
                U64 us = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                write_max_usec = std::max(write_max_usec, us);
                if (!ok)
                {
                    s.flush();
                    ensure("write after flush", s.write(k, WriteSource::copy(payload.data(), payload.size()), o));
                }
                if ((i & 0xffff) == 0xffff) s.flush();
            }
            s.flush();
            ensure_equals("records", s.stats().records, n);
            {
                Stats st = s.stats();
                fprintf(stderr, "\n[#866 bench] grown by writes: index %.1f MB = %.1f B/record",
                        (F64)st.index_bytes / 1048576.0, st.index_records ? (F64)st.index_bytes / st.index_records : 0.0);
            }
            const auto t0 = std::chrono::steady_clock::now();
            ensure("save", s.saveSnapshot());
            F64 save_ms = (F64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 1000.0;
            fprintf(stderr, "\n[#866 bench] records %u: snapshot save %.1f ms, worst write() %llu us\n", n, save_ms,
                    (unsigned long long)write_max_usec);
            s.close();
        }
        {
            Store s;
            U64 heap_before = heapInUse();
            ensure("reopen", s.open(c));
            U64 heap_after = heapInUse();
            Stats st = s.stats();
            ensure_equals("records after reopen", st.records, n);
            F64 heap_per = heap_after > heap_before ? (F64)(heap_after - heap_before) / n : 0.0;
            fprintf(stderr, "[#866 bench] reopen: index load %.1f ms; heap delta %.1f MB = %.1f B/record\n",
                    (F64)st.index_load_usec / 1000.0, (F64)(heap_after - heap_before) / 1048576.0, heap_per);
            ensure_equals("index entries", st.index_records, n);
            F64 index_per = (F64)st.index_bytes / (F64)st.index_records;
            fprintf(stderr, "[#866 bench] index accounting: %.1f MB = %.1f B/record\n%s\n",
                    (F64)st.index_bytes / 1048576.0, index_per, s.statsLine().c_str());
            // The ticket's target (<= 70 B/record at 1.2M); small runs are dominated
            // by the per-shard fixed cost.
            if (n >= 1000000) ensure("index <= 70 B/record", index_per <= 70.0);
            s.close();
        }
    }

    // ------------------------------------------------------------------
    // #866: the flat index against a reference map: random insert / erase / meta,
    // erase-while-iterating visits each record once, swap-remove moves meta along.
    // Also times the worst single insert (a grow rebuilds the lookup array under
    // the shard lock) at the per-shard size of 1.2M records (64 shards) and for one
    // 1.2M-record table.
    template<> template<>
    void cachestore_object::test<22>()
    {
        using LLCacheStoreIndex::Entry;
        using LLCacheStoreIndex::Index;
        struct Ref { U32 seg; std::vector<U8> meta; };
        auto keyFor = [](U32 i) -> Key
        {
            U64 x = 0xabcdefull + i;
            LLUUID id;
            U64 a = splitmix(x), b = splitmix(x);
            memcpy(id.mData, &a, 8);
            memcpy(id.mData + 8, &b, 8);
            // KeyHash % 64 picks a shard; keep one shard's keys like the store does
            return Key(id, (Kind)(1 + i % 6), i % 5);
        };
        Index ix;
        std::unordered_map<Key, Ref, KeyHash> ref;
        U64 rs = 7;
        auto verifyAll = [&](const char* when)
        {
            ensure(std::string(when) + ": consistent", ix.checkConsistency());
            ensure_equals(std::string(when) + ": size", ix.size(), ref.size());
            for (auto& kv : ref)
            {
                const Entry* e = ix.get(kv.first);
                ensure(std::string(when) + ": found", e != nullptr);
                ensure_equals(std::string(when) + ": seg", e->seg, kv.second.seg);
                ensure_equals(std::string(when) + ": meta len", (size_t)e->meta_len, kv.second.meta.size());
                if (!kv.second.meta.empty())
                    ensure(std::string(when) + ": meta", memcmp(ix.meta(kv.first), kv.second.meta.data(), kv.second.meta.size()) == 0);
            }
        };
        for (int round = 0; round < 4; ++round)
        {
            for (int op = 0; op < 60000; ++op)
            {
                U64 r = splitmix(rs);
                Key k = keyFor((U32)(r % 9000));
                switch ((r >> 20) % 4)
                {
                case 0:
                case 1:
                {
                    bool inserted = false;
                    Entry* e = ix.insert(k, &inserted);
                    ensure("insert", e != nullptr);
                    ensure_equals("inserted iff new", inserted, ref.count(k) == 0);
                    e->seg = (U32)(r >> 32);
                    Ref& rf = ref[k];
                    rf.seg = e->seg;
                    if ((r >> 40) % 3 == 0)
                    {
                        U8 m[32];
                        U8 len = (U8)(1 + (r >> 44) % 32);
                        for (U8 b = 0; b < len; ++b) m[b] = (U8)(r >> (b % 56)) ^ b;
                        ix.setMeta(k, m, len);
                        rf.meta.assign(m, m + len);
                    }
                    else if ((r >> 40) % 3 == 1)
                    {
                        ix.setMeta(k, nullptr, 0);
                        rf.meta.clear();
                    }
                    break;
                }
                case 2:
                {
                    auto it = ix.find(k);
                    ensure_equals("find iff present", it != ix.end(), ref.count(k) != 0);
                    if (it != ix.end())
                    {
                        ix.erase(it);
                        ref.erase(k);
                    }
                    break;
                }
                default:
                {
                    const Entry* e = ix.get(k);
                    auto rit = ref.find(k);
                    ensure_equals("lookup iff present", e != nullptr, rit != ref.end());
                    if (e) ensure_equals("lookup seg", e->seg, rit->second.seg);
                }
                }
            }
            verifyAll("random ops");
            // erase while iterating: every record visited exactly once
            size_t before = ix.size(), visits = 0, erased = 0;
            std::unordered_set<Key, KeyHash> seen;
            for (auto it = ix.begin(); it != ix.end();)
            {
                ++visits;
                ensure("visited once", seen.insert(it->first).second);
                if (it->second.seg % 3 == 0)
                {
                    ref.erase(it->first);
                    it = ix.erase(it);
                    ++erased;
                }
                else ++it;
            }
            ensure_equals("each record visited once", visits, before);
            ensure_equals("erased", ix.size(), before - erased);
            verifyAll("erase while iterating");
        }
        // drain to empty and refill
        for (auto it = ix.begin(); it != ix.end();) it = ix.erase(it);
        ref.clear();
        verifyAll("drained");
        ensure("drained memory released", ix.memoryBytes() < 1024);

        // worst single insert (grow) at the store's shard size for 1.2M records, and
        // in one 1.2M-record table
        for (U32 count : { 1200000u / 64u, 1200000u })
        {
            Index big;
            U64 worst = 0, total = 0;
            for (U32 i = 0; i < count; ++i)
            {
                Key k = keyFor(1000000 + i);
                const auto t0 = std::chrono::steady_clock::now();
                big.insert(k)->seg = i + 1;
                U64 ns = (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                worst = std::max(worst, ns);
                total += ns;
            }
            fprintf(stderr, "\n[#866 index] %u records in one table: worst insert %.1f us (mean %.0f ns), %.1f B/record\n",
                    count, (F64)worst / 1000.0, (F64)total / count, (F64)big.memoryBytes() / count);
            ensure("consistent", big.checkConsistency());
        }
    }

    // ------------------------------------------------------------------
    // #866: a snapshot built by hand from the documented v1 layout (the format the
    // node-map index wrote) loads as is: no scan, every field carried.
    template<> template<>
    void cachestore_object::test<23>()
    {
        namespace F = LLCacheStoreFormat;
        TempDir td;
        const U8 meta[27] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 };
        std::vector<Key> keys = { Key(idFor(801), Kind::J2C), Key(idFor(802), Kind::J2C), Key(idFor(803), Kind::J2C) };
        std::vector<std::vector<U8>> data = { makeData(801, 3000), makeData(802, 1000), makeData(803, 2000) };
        std::vector<U32> write_time(3);
        {
            Store s;
            ensure("open", s.open(baseConfig(td.path)));
            for (size_t i = 0; i < 3; ++i)
            {
                WriteOptions o;
                if (i == 1)
                {
                    o.meta = meta;
                    o.meta_len = 27;
                }
                ensure("write", s.write(keys[i], WriteSource::copy(data[i].data(), data[i].size()), o));
            }
            s.flush();
            for (size_t i = 0; i < 3; ++i)
            {
                RecordInfo ri;
                ensure("lookup", s.lookup(keys[i], &ri));
                ensure_equals("raw codec", (int)ri.codec, 0);
                write_time[i] = ri.write_time;
            }
            s.close();
        }
        std::vector<std::string> segs = segmentFiles(td.path);
        ensure_equals("one segment", segs.size(), (size_t)1);
        U32 seq = 0;
        ensure("seq", F::parseSegmentFileName(fs::path(segs[0]).filename().string(), seq));
        const U64 seg_size = fs::file_size(segs[0]);
        fs::remove(td.sub("index.snap"));
        fs::remove(td.sub("index.snap.prev"));

        // v1: header, segments, regions, entries (64 bytes + meta each), xxh64
        const LLUUID region = idFor(4242);
        const U32 last_access[3] = { 1700000000u, 1700003600u, 1700007200u };
        const U32 recency[3] = { 0, 1700003000u, 1700007000u };
        F::Writer w;
        w.bytes("SLCI", 4);
        w.u16(1);
        w.u16(0);
        w.u32(1700010000u);
        w.u32(0);   // day (another day: no carried write counters)
        w.u64(0);
        w.u64(0);
        w.u64(0);
        w.u32(1);   // segments
        w.u32(1);   // regions
        w.u32(3);   // entries
        w.u32(seq);
        w.u64(seg_size);
        U64 rec_bytes = 0;
        U64 offs[3];
        {
            U64 at = F::SEG_HEADER_SIZE;
            for (size_t i = 0; i < 3; ++i)
            {
                offs[i] = at;
                at += F::recordSize(i == 1 ? 27 : 0, (U32)data[i].size());
            }
            rec_bytes = at - F::SEG_HEADER_SIZE;
        }
        for (size_t g = 0; g < (size_t)Group::COUNT; ++g) w.u64(g == (size_t)Group::TEXTURES ? rec_bytes : 0);
        for (size_t k = 0; k < (size_t)Kind::COUNT; ++k) w.u64(k == (size_t)Kind::J2C ? rec_bytes : 0);
        w.uuid(region);
        w.u64(0x1234);
        w.u32(1700000500u);
        w.u32(3);
        for (size_t i = 0; i < 3; ++i)
        {
            w.uuid(keys[i].id);
            w.u8((U8)keys[i].kind);
            w.u8(0);                            // codec
            w.u8(i == 2 ? 0x01 : 0);            // flags: shared
            w.u8(i == 1 ? 27 : 0);              // meta_len
            w.u32(keys[i].variant);
            w.u32(1);                           // kind version
            w.u32(seq);
            w.u32((U32)offs[i]);
            w.u32((U32)data[i].size());         // stored
            w.u32((U32)data[i].size());         // raw
            w.u32(write_time[i]);
            w.u32(last_access[i]);
            w.u16((U16)(5 + i));                // reuse
            w.u16(0);
            w.u32(i == 0 ? 0u : 0xffffffffu);   // region index
            w.u32(recency[i]);
            if (i == 1) w.bytes(meta, 27);
        }
        w.u64(HBXXH64::digest(w.buf.data(), w.buf.size()));
        writeFile(td.sub("index.snap"), w.buf.data(), w.buf.size());

        Store s;
        ensure("open with the v1 snapshot", s.open(baseConfig(td.path)));
        for (size_t i = 0; i < 3; ++i)
        {
            RecordInfo ri;
            ensure("lookup", s.lookup(keys[i], &ri));
            // a scan would have given reuse 0 and last_access = write_time
            ensure_equals("reuse from the snapshot", (int)ri.reuse_count, (int)(5 + i));
            ensure_equals("last access exact", ri.last_access, last_access[i]);
            ensure_equals("prev session access (minute precision)", ri.prev_session_access, last_access[i] / 60 * 60);
            ensure_equals("write time", ri.write_time, write_time[i]);
            ensure_equals("raw", ri.raw_bytes, (U32)data[i].size());
            ensure_equals("shared", ri.shared, i == 2);
            ensure_equals("meta len", (int)ri.meta_len, i == 1 ? 27 : 0);
            if (i == 1) ensure("meta", memcmp(ri.meta, meta, 27) == 0);
            ensure("region", i == 0 ? ri.region == region : ri.region.isNull());
            ensure("read", readEquals(s, keys[i], data[i]));
        }
        RegionInfo rgi;
        ensure("region carried", s.regionInfo(region, &rgi) && rgi.visit_count == 3 && rgi.own_records == 1);
        ensure_equals("verify", s.verifyAll(nullptr), 0u);
        s.close();
        // ...and the snapshot this build writes reads back the same way.
        Store s2;
        ensure("reopen", s2.open(baseConfig(td.path)));
        RecordInfo ri;
        ensure("meta after rewrite", s2.lookup(keys[1], &ri) && ri.meta_len == 27 && memcmp(ri.meta, meta, 27) == 0);
        ensure("region after rewrite", s2.lookup(keys[0], &ri) && ri.region == region);
    }
}
