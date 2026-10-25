/**
 * @file slj2cmigrate.cpp
 * @brief Legacy J2C texture cache -> segment store migration (#714). See slj2cmigrate.h.
 *
 * No viewer precompiled header (SKIP_PRECOMPILE_HEADERS): llcommon + llcachestore only,
 * so the unit test links it without the viewer.
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

#include "slj2cmigrate.h"

#include "fsyspath.h"
#include "llerror.h"
#include "llformat.h"
#include "llj2crecord.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    const char* ENTRIES_FILE = "texture.entries";
    const char* STRIPES_FILE = "texture.cache";
    constexpr F64 MB = 1024.0 * 1024.0;
    constexpr S32 MAX_PREFIX = 64 << 20;   // sanity cap on one legacy prefix

    std::string joinPath(const std::string& dir, const std::string& name)
    {
        if (dir.empty()) return name;
        const char last = dir.back();
        return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
    }

    bool removeFile(const std::string& path)
    {
        std::error_code ec;
        return fs::remove(fsyspath(path), ec) && !ec;
    }

    U32 get32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }
    void put32(U8* p, U32 v) { for (int i = 0; i < 4; ++i) p[i] = (U8)(v >> (8 * i)); }

    using Clock = std::chrono::steady_clock;
    F64 since(Clock::time_point t0) { return std::chrono::duration<F64>(Clock::now() - t0).count(); }

    bool stopRequested(const SLJ2CMigrate::Options& opt)
    {
        return opt.stop && opt.stop->load(std::memory_order_relaxed);
    }

    // Sleep in small steps so a stop request is honoured quickly.
    void sleepFor(F64 seconds, const SLJ2CMigrate::Options& opt)
    {
        const auto end = Clock::now() + std::chrono::duration<F64>(seconds);
        while (Clock::now() < end && !stopRequested(opt))
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<S64>(20, (S64)(seconds * 1000.0) + 1)));
    }

    // Positional read on a stdio handle (64-bit offsets everywhere).
    bool preadAt(LLFILE* fp, U64 offset, U8* buf, size_t n)
    {
#if LL_WINDOWS
        if (_fseeki64(fp, (__int64)offset, SEEK_SET) != 0) return false;
#else
        if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) return false;
#endif
        return fread(buf, 1, n, fp) == n;
    }

    bool pwriteAt(LLFILE* fp, U64 offset, const U8* buf, size_t n)
    {
#if LL_WINDOWS
        if (_fseeki64(fp, (__int64)offset, SEEK_SET) != 0) return false;
#else
        if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) return false;
#endif
        return fwrite(buf, 1, n, fp) == n;
    }
}

namespace SLJ2CMigrate
{

bool hasWork(const std::string& dir)
{
    std::error_code ec;
    return fs::is_regular_file(fsyspath(joinPath(dir, ENTRIES_FILE)), ec) && !ec;
}

std::string resultLine(const Result& r)
{
    const F64 secs = std::max(r.seconds, 1e-6);
    return llformat("%u entries: %u converted (%.1f MB), %u superseded, %u already in the store, %u bad, "
                    "%u over budget (dropped), %u failed, %u left; read %.1f MB in %.1f s = %.1f files/s, %.1f MB/s",
                    r.entries, r.converted, (F64)r.bytes_converted / MB, r.superseded, r.already, r.bad, r.over_budget,
                    r.failed, r.left, (F64)r.bytes_read / MB, r.seconds, (F64)(r.converted + r.superseded) / secs,
                    (F64)r.bytes_read / MB / secs)
         + (r.version_mismatch ? " (legacy cache of another format: dropped)" : "")
         + (r.stopped ? " (stopped)" : "") + (r.disk_full ? " (disk full: rest left for the next start)" : "")
         + (r.completed ? " (complete)" : "");
}

// ------------------------------------------------------------------ Legacy

Legacy::~Legacy()
{
    close();
}

bool Legacy::open(const std::string& dir, const Options& opt)
{
    close();
    std::lock_guard<std::mutex> lock(mMutex);
    mDir = dir;
    mFreeBatch = std::max<U32>(opt.free_mark_batch, 1);
    mVersionMismatch = false;
    mLive.clear();
    mPendingFree.clear();

    const std::string entries_path = joinPath(dir, ENTRIES_FILE);
    LLFILE* fp = LLFile::fopen(entries_path, "rb");
    if (!fp) return false;
    std::vector<U8> data;
    {
        U8 buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) data.insert(data.end(), buf, buf + n);
    }
    LLFile::close(fp);

    mOpen = true;
    if (data.size() < ENTRIES_INFO_BYTES)
    {
        return true;   // empty / torn header: nothing to import
    }
    F32 version = 0.f;
    memcpy(&version, data.data(), sizeof(F32));
    const U32 address_size = get32(data.data() + 4);
    if (version != opt.expected_version || address_size != opt.expected_address_size)
    {
        mVersionMismatch = true;
        return true;
    }
    const U32 count = get32(data.data() + 40);
    const U64 avail = (data.size() - ENTRIES_INFO_BYTES) / ENTRY_BYTES;
    const U32 n = (U32)std::min<U64>(count, avail);
    mLive.reserve(n);
    for (U32 i = 0; i < n; ++i)
    {
        const U8* p = data.data() + ENTRIES_INFO_BYTES + (size_t)i * ENTRY_BYTES;
        Entry e;
        memcpy(e.id.mData, p, 16);
        e.image_size = (S32)get32(p + 16);
        e.body_size = (S32)get32(p + 20);
        e.time = get32(p + 24);
        e.idx = i;
        if (e.image_size <= 0 || e.image_size <= e.body_size || e.body_size < 0 || e.id.isNull()) continue;
        auto it = mLive.find(e.id);
        if (it != mLive.end())
        {
            // One UUID in two slots (never written by a sane legacy cache): keep the newest,
            // free the other slot (the body file is shared by name, so it stays).
            if (it->second.time >= e.time)
            {
                mPendingFree.push_back(e.idx);
                continue;
            }
            mPendingFree.push_back(it->second.idx);
        }
        mLive[e.id] = e;
    }
    mStripes = LLFile::fopen(joinPath(dir, STRIPES_FILE), "rb");
    mEntries = LLFile::fopen(entries_path, "r+b");   // free marks (null on a read-only folder: no marks)
    return true;
}

void Legacy::close()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mOpen) flushLocked();
    if (mStripes) { LLFile::close(mStripes); mStripes = nullptr; }
    if (mEntries) { LLFile::close(mEntries); mEntries = nullptr; }
    mOpen = false;
    mLive.clear();
}

bool Legacy::isOpen() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mOpen;
}

std::string Legacy::bodyPath(const LLUUID& id) const
{
    const std::string s = id.asString();
    return joinPath(joinPath(mDir, s.substr(0, 1)), s + ".texture");
}

Legacy::ReadStatus Legacy::readLocked(const Entry& e, std::vector<U8>& out)
{
    auto it = mLive.find(e.id);
    if (!mOpen || it == mLive.end() || it->second.idx != e.idx) return ReadStatus::GONE;
    const S32 n = e.prefixBytes();
    if (n <= 0 || n > MAX_PREFIX) return ReadStatus::BAD;
    if (e.body_size > 0 && e.image_size < STRIPE_BYTES + e.body_size) return ReadStatus::BAD;
    if (!mStripes) return ReadStatus::BAD;
    try { out.resize((size_t)n); }
    catch (const std::bad_alloc&) { return ReadStatus::IO_ERROR; }
    const size_t head = (size_t)std::min(n, STRIPE_BYTES);
    if (!preadAt(mStripes, (U64)e.idx * STRIPE_BYTES, out.data(), head)) return ReadStatus::BAD;
    if (e.body_size > 0)
    {
        const std::string path = bodyPath(e.id);
        std::error_code ec;
        const U64 size = (U64)fs::file_size(fsyspath(path), ec);
        if (ec || size != (U64)e.body_size) return ReadStatus::BAD;
        LLFILE* fp = LLFile::fopen(path, "rb");
        if (!fp) return ReadStatus::IO_ERROR;
        const bool ok = fread(out.data() + STRIPE_BYTES, 1, (size_t)e.body_size, fp) == (size_t)e.body_size;
        LLFile::close(fp);
        if (!ok) return ReadStatus::IO_ERROR;
    }
    return ReadStatus::OK;
}

Legacy::ReadStatus Legacy::readEntry(const Entry& e, std::vector<U8>& out)
{
    std::lock_guard<std::mutex> lock(mMutex);
    return readLocked(e, out);
}

bool Legacy::read(const LLUUID& id, std::vector<U8>& out, S32& image_size)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mLive.find(id);
    if (!mOpen || it == mLive.end()) return false;
    const Entry e = it->second;
    if (readLocked(e, out) != ReadStatus::OK)
    {
        out.clear();
        return false;
    }
    image_size = e.image_size;
    return true;
}

void Legacy::dropLocked(const Entry& e)
{
    auto it = mLive.find(e.id);
    if (it == mLive.end() || it->second.idx != e.idx) return;
    if (it->second.body_size > 0) removeFile(bodyPath(e.id));
    mLive.erase(it);
    mPendingFree.push_back(e.idx);
    if (mPendingFree.size() >= mFreeBatch) flushLocked();
}

void Legacy::drop(const Entry& e)
{
    std::lock_guard<std::mutex> lock(mMutex);
    dropLocked(e);
}

void Legacy::forget(const LLUUID& id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mLive.find(id);
    if (it == mLive.end()) return;
    const Entry e = it->second;
    dropLocked(e);
    flushLocked();
}

void Legacy::flushLocked()
{
    if (mPendingFree.empty()) return;
    if (mEntries)
    {
        // The legacy cache's free slot: image size -1, body 0 (removeEntry()).
        std::sort(mPendingFree.begin(), mPendingFree.end());
        U8 rec[ENTRY_BYTES] = {};
        put32(rec + 16, (U32)-1);
        for (U32 idx : mPendingFree)
        {
            if (!pwriteAt(mEntries, ENTRIES_INFO_BYTES + (U64)idx * ENTRY_BYTES, rec, sizeof(rec))) break;
        }
        fflush(mEntries);
    }
    mPendingFree.clear();
}

void Legacy::flushFreeMarks()
{
    std::lock_guard<std::mutex> lock(mMutex);
    flushLocked();
}

U32 Legacy::liveCount() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return (U32)mLive.size();
}

std::vector<Legacy::Entry> Legacy::liveEntriesNewestFirst() const
{
    std::vector<Entry> v;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        v.reserve(mLive.size());
        for (const auto& kv : mLive) v.push_back(kv.second);
    }
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b)
              { return a.time != b.time ? a.time > b.time : a.idx < b.idx; });
    return v;
}

bool Legacy::removeAll()
{
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPendingFree.clear();   // the file goes anyway
        dir = mDir;
    }
    close();
    if (dir.empty()) return false;
    std::error_code ec;
    fs::remove_all(fsyspath(dir), ec);
    return !ec;
}

// ------------------------------------------------------------------ migrate

Result migrate(Legacy& legacy, LLCache::Store& store, const Options& opt)
{
    Result res;
    const auto t0 = Clock::now();

    if (legacy.versionMismatch())
    {
        res.version_mismatch = true;
        res.completed = legacy.removeAll();
        res.seconds = since(t0);
        return res;
    }

    const std::vector<Legacy::Entry> entries = legacy.liveEntriesNewestFirst();
    res.entries = (U32)entries.size();

    struct InFlight { LLCache::Ticket<LLCache::Status> done; Legacy::Entry entry; U64 bytes; };
    std::deque<InFlight> inflight;
    U64 inflight_bytes = 0;
    auto settle = [&](InFlight& f)
    {
        const LLCache::Status st = f.done.take();
        inflight_bytes -= std::min(inflight_bytes, f.bytes);
        if (st == LLCache::Status::OK)
        {
            legacy.drop(f.entry);
            ++res.converted;
            res.bytes_converted += f.bytes;
        }
        else if (st == LLCache::Status::SKIPPED)
        {
            legacy.drop(f.entry);   // a longer prefix of the key reached the store first
            ++res.superseded;
        }
        else
        {
            if (st == LLCache::Status::NO_SPACE) res.disk_full = true;
            ++res.failed;           // kept for the next start
        }
    };
    // Settle what has finished; block on the oldest while over the window.
    auto drain = [&](bool all)
    {
        while (!inflight.empty())
        {
            InFlight& f = inflight.front();
            const bool over = all || inflight.size() >= opt.max_files_in_flight || inflight_bytes >= opt.max_bytes_in_flight;
            if (!f.done.isDone())
            {
                if (!over) break;
                if (!f.done.wait(std::chrono::milliseconds(200))) continue;   // re-check (the write is queued anyway)
            }
            settle(f);
            inflight.pop_front();
        }
    };

    U64 live = store.stats().kinds[(size_t)LLCache::Kind::J2C].live_bytes;
    F64 last_log = 0.0;
    size_t i = 0;
    std::vector<U8> buf;
    for (; i < entries.size(); ++i)
    {
        if (stopRequested(opt)) { res.stopped = true; break; }
        if (res.disk_full) break;
        // Throttle on bytes read AND on entries touched (every one, skipped ones included:
        // a delete costs too). Applied before the next entry, so the window's writes keep
        // completing meanwhile.
        {
            F64 should = 0.0;
            if (opt.max_read_bytes_per_sec) should = (F64)res.bytes_read / (F64)opt.max_read_bytes_per_sec;
            if (opt.max_files_per_sec) should = std::max(should, (F64)i / (F64)opt.max_files_per_sec);
            const F64 elapsed = since(t0);
            if (should > elapsed) sleepFor(should - elapsed, opt);
            if (stopRequested(opt)) { res.stopped = true; break; }
        }
        const Legacy::Entry& e = entries[i];
        const S32 prefix = e.prefixBytes();

        if ((i & 15) == 0) live = store.stats().kinds[(size_t)LLCache::Kind::J2C].live_bytes;
        if (opt.live_cap_bytes && live >= opt.live_cap_bytes)
        {
            legacy.drop(e);
            ++res.over_budget;
            continue;
        }

        const LLCache::Key key = LLJ2CRecord::makeKey(e.id);
        LLCache::RecordInfo have;
        bool longer_only = false;
        if (store.lookup(key, &have) && !have.staged)
        {
            if (prefix <= 0 || (S64)have.raw_bytes >= (S64)prefix)
            {
                legacy.drop(e);   // the store's record (written this or an earlier session) stands
                ++res.already;
                continue;
            }
            longer_only = true;   // the legacy prefix is longer: it replaces the store's
        }

        const Legacy::ReadStatus rs = legacy.readEntry(e, buf);
        if (rs == Legacy::ReadStatus::GONE) { ++res.already; continue; }   // forgotten meanwhile
        if (rs == Legacy::ReadStatus::IO_ERROR) { ++res.failed; continue; } // leave it
        res.bytes_read += (U64)std::max(prefix, 0);
        if (rs == Legacy::ReadStatus::BAD)
        {
            LL_DEBUGS("CacheStore") << "J2C migrate: dropping bad legacy entry " << e.id << " (image " << e.image_size
                                    << ", body " << e.body_size << ")" << LL_ENDL;
            legacy.drop(e);
            ++res.bad;
            continue;
        }

        const U64 bytes = buf.size();
        U8 meta[LLJ2CRecord::META_BYTES];
        LLCache::WriteOptions wo;
        wo.priority = LLCache::Priority::LOW;
        wo.placement = LLCache::PlacementHint::shared();   // the legacy cache carries no region
        wo.meta_len = LLJ2CRecord::packMeta(e.image_size, meta);
        wo.meta = meta;
        if (longer_only) wo.flags |= LLCache::WF_SKIP_IF_NOT_LARGER;   // a longer write may land first
        // Lent to the store (its writer copies it off, then drops the reference), so a
        // refused write can be retried from the same buffer.
        auto payload = std::make_shared<std::vector<U8>>(std::move(buf));
        buf.clear();
        bool queued = false;
        for (int attempt = 0; attempt < 40 && !stopRequested(opt); ++attempt)
        {
            InFlight fl;
            fl.entry = e;
            fl.bytes = bytes;
            std::shared_ptr<std::vector<U8>> keep = payload;
            store.write(key, LLCache::WriteSource::borrow(payload->data(), payload->size(), [keep]() {}), wo, &fl.done);
            if (fl.done.isDone())
            {
                // Refused at once (the ticket already holds the status) or skipped.
                LLCache::Ticket<LLCache::Status> t = fl.done;
                const LLCache::Status s = t.take();
                if (s == LLCache::Status::REFUSED_BUSY)
                {
                    drain(true);           // let the store's queue empty, then retry
                    sleepFor(0.25, opt);
                    continue;
                }
                if (s == LLCache::Status::SKIPPED) { legacy.drop(e); ++res.superseded; }
                else if (s == LLCache::Status::OK) { legacy.drop(e); ++res.converted; res.bytes_converted += bytes; }
                else
                {
                    if (s == LLCache::Status::NO_SPACE) res.disk_full = true;
                    ++res.failed;
                }
                queued = true;   // handled
                break;
            }
            inflight_bytes += bytes;
            inflight.push_back(std::move(fl));
            live += LLJ2CRecord::diskBytes(bytes);   // estimate until the next stats refresh
            queued = true;
            break;
        }
        if (!queued) ++res.failed;
        drain(false);

        if (opt.progress_log_interval_s > 0.0 && since(t0) - last_log >= opt.progress_log_interval_s)
        {
            last_log = since(t0);
            Result p = res;
            p.seconds = last_log;
            p.left = (U32)(entries.size() - i - 1);
            LL_INFOS("CacheStore") << "J2C migration progress: " << resultLine(p) << LL_ENDL;
        }
    }
    res.left = (U32)(entries.size() - std::min(i, entries.size()));
    // Push the LOW-priority runs out now instead of waiting for their flush timer.
    if (!inflight.empty()) store.flush();
    drain(true);
    legacy.flushFreeMarks();
    res.seconds = since(t0);
    res.completed = !res.stopped && !res.disk_full && res.left == 0 && res.failed == 0 && legacy.liveCount() == 0;
    if (res.completed && opt.remove_dir_when_done)
    {
        legacy.removeAll();
    }
    return res;
}

// ------------------------------------------------------------------ Runner

Runner::~Runner()
{
    stop();
}

void Runner::start(const std::string& dir, LLCache::Store* store, const Options& opt, F64 start_delay_s,
                   OnOpen on_open)
{
    stop();
    if (!store) return;
    mStop = false;
    mDone = false;
    mThread = std::thread([this, dir, store, opt, start_delay_s, on_open]()
    {
        LL_PROFILER_SET_THREAD_NAME("J2CMigrate");
        Options o = opt;
        o.stop = &mStop;
        auto legacy = std::make_shared<Legacy>();
        const auto t_open = Clock::now();
        if (!legacy->open(dir, o))
        {
            mDone = true;
            return;
        }
        LL_INFOS("CacheStore") << "J2C migration: legacy cache " << dir << " opened in "
                               << llformat("%.1f", since(t_open) * 1000.0) << " ms, " << legacy->liveCount()
                               << " live entries" << (legacy->versionMismatch() ? " (another format: will be dropped)" : "")
                               << LL_ENDL;
        if (on_open && !legacy->versionMismatch()) on_open(legacy);   // read-through until converted

        // The store's index loads on its writer thread; then let login I/O go first.
        while (!mStop && !store->ready()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sleepFor(start_delay_s, o);
        Result r;
        bool ran = false;
        if (!mStop && store->isOpen() && !store->readOnly())
        {
            LL_INFOS("CacheStore") << "J2C migration: converting " << dir << " into the cache store" << LL_ENDL;
            r = migrate(*legacy, *store, o);
            ran = true;
            LL_INFOS("CacheStore") << "J2C migration " << (r.completed ? "done" : "paused") << ": " << resultLine(r)
                                   << LL_ENDL;
        }
        // Not finished (disk full / failures): keep serving the rest by read-through
        // until shutdown.
        while (ran && !r.completed && !mStop) sleepFor(1.0, o);
        if (on_open) on_open(nullptr);
        legacy->close();
        mDone = true;
    });
}

void Runner::stop()
{
    mStop = true;
    if (mThread.joinable()) mThread.join();
}

} // namespace SLJ2CMigrate
