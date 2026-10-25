/**
 * @file slcachemigrate.cpp
 * @brief Folder -> segment store converter (#714). See slcachemigrate.h.
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

#include "slcachemigrate.h"

#include "fsyspath.h"
#include "llerror.h"
#include "llformat.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    constexpr F64 MB = 1024.0 * 1024.0;
    using Clock = std::chrono::steady_clock;

    F64 since(Clock::time_point t0) { return std::chrono::duration<F64>(Clock::now() - t0).count(); }

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

    bool stopRequested(const SLCacheMigrate::Options& opt)
    {
        return opt.stop && opt.stop->load(std::memory_order_relaxed);
    }

    void sleepFor(F64 seconds, const SLCacheMigrate::Options& opt)
    {
        const auto end = Clock::now() + std::chrono::duration<F64>(seconds);
        while (Clock::now() < end && !stopRequested(opt))
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<S64>(20, (S64)(seconds * 1000.0) + 1)));
    }

    bool accepted(LLCache::Status s) { return s == LLCache::Status::OK || s == LLCache::Status::SKIPPED; }
}

namespace SLCacheMigrate
{

bool hasWork(const std::string& dir, const Converter& conv)
{
    std::error_code ec;
    fs::directory_iterator it(fsyspath(dir), ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        const std::string name = fsyspath(it->path().filename()).string();
        if (conv.matches(name) || conv.isLeftover(name)) return true;
    }
    return false;
}

std::string resultLine(const Result& r)
{
    const F64 secs = std::max(r.seconds, 1e-6);
    return llformat("%u files: %u converted (%u records, %.1f MB), %u already in the store, %u stale, %u bad, "
                    "%u over budget (deleted), %u failed, %u left; read %.1f MB in %.1f s = %.1f files/s, %.1f MB/s",
                    r.files, r.converted, r.records, (F64)r.bytes_converted / MB, r.already, r.stale, r.bad,
                    r.over_budget, r.failed, r.left, (F64)r.bytes_read / MB, r.seconds,
                    (F64)(r.converted + r.already) / secs, (F64)r.bytes_read / MB / secs)
         + (r.stopped ? " (stopped)" : "") + (r.disk_full ? " (disk full: rest left for the next start)" : "")
         + (r.completed ? " (complete)" : "");
}

Result migrateDir(const std::string& dir, LLCache::Store& store, const Converter& conv, const Options& opt)
{
    Result res;
    const auto t0 = Clock::now();

    struct FileEntry { std::string name; U64 size; fs::file_time_type mtime; };
    std::vector<FileEntry> files;
    {
        std::error_code ec;
        fs::directory_iterator it(fsyspath(dir), ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            const std::string name = fsyspath(it->path().filename()).string();
            if (conv.isLeftover(name))
            {
                removeFile(joinPath(dir, name));   // a write the old cache never finished
                ++res.bad;
                continue;
            }
            if (!conv.matches(name)) continue;     // not ours: never touched
            std::error_code e2;
            FileEntry f;
            f.name = name;
            f.size = (U64)it->file_size(e2);
            if (e2) f.size = 0;
            f.mtime = it->last_write_time(e2);
            files.push_back(std::move(f));
        }
    }
    res.files = (U32)files.size();
    // Newest first: the hot set lands first, and a budget cut drops the coldest.
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.mtime > b.mtime; });

    struct Slot
    {
        LLCache::Key key;
        LLCache::Ticket<LLCache::Status> done;
        bool has_status = false;          // refused / skipped at once: status known now
        LLCache::Status status = LLCache::Status::OK;
        bool isDone() const { return has_status || done.isDone(); }
    };
    struct InFlight { std::string path; U64 bytes = 0; std::vector<Slot> slots; };
    std::deque<InFlight> inflight;
    U64 inflight_bytes = 0;

    auto settle = [&](InFlight& f)
    {
        inflight_bytes -= std::min(inflight_bytes, f.bytes);
        bool all_ok = true;
        bool no_space = false;
        for (Slot& s : f.slots)
        {
            if (!s.has_status)
            {
                s.status = s.done.take();
                s.has_status = true;
            }
            if (!accepted(s.status)) all_ok = false;
            if (s.status == LLCache::Status::NO_SPACE) no_space = true;
        }
        if (all_ok)
        {
            removeFile(f.path);
            ++res.converted;
            res.records += (U32)f.slots.size();
            res.bytes_converted += f.bytes;
            return;
        }
        // Keep the file whole for the next start: take back the records this file wrote
        // (never a SKIPPED one: that key holds a newer copy from the live session).
        for (Slot& s : f.slots)
            if (s.status == LLCache::Status::OK) store.remove(s.key);
        if (no_space) res.disk_full = true;
        ++res.failed;
    };
    auto frontDone = [](const InFlight& f)
    {
        for (const Slot& s : f.slots) if (!s.isDone()) return false;
        return true;
    };
    // Settle what has finished; block on the oldest while over the window.
    auto drain = [&](bool all)
    {
        while (!inflight.empty())
        {
            InFlight& f = inflight.front();
            const bool over = all || inflight.size() >= opt.max_files_in_flight || inflight_bytes >= opt.max_bytes_in_flight;
            if (!frontDone(f))
            {
                if (!over) break;
                for (Slot& s : f.slots)
                    if (!s.isDone()) { s.done.wait(std::chrono::milliseconds(200)); break; }
                continue;
            }
            settle(f);
            inflight.pop_front();
        }
    };

    const LLCache::Group group = conv.group();
    U64 live = store.stats().groups[(size_t)group].live_bytes;
    F64 last_log = 0.0;
    size_t i = 0;
    U32 processed = 0;
    for (; i < files.size(); ++i)
    {
        if (stopRequested(opt) || (opt.stop_after_files && processed >= opt.stop_after_files))
        {
            res.stopped = true;
            break;
        }
        if (res.disk_full) break;
        // Throttle on bytes read AND on files touched (every one, skipped ones included:
        // a delete costs too). Applied before the next file, so the window's writes keep
        // completing meanwhile.
        {
            F64 should = 0.0;
            if (opt.max_read_bytes_per_sec) should = (F64)res.bytes_read / (F64)opt.max_read_bytes_per_sec;
            if (opt.max_files_per_sec) should = std::max(should, (F64)i / (F64)opt.max_files_per_sec);
            const F64 elapsed = since(t0);
            if (should > elapsed) sleepFor(should - elapsed, opt);
            if (stopRequested(opt)) { res.stopped = true; break; }
        }
        ++processed;
        const FileEntry& f = files[i];
        const std::string path = joinPath(dir, f.name);

        if ((i & 15) == 0) live = std::max(live, store.stats().groups[(size_t)group].live_bytes);
        if (opt.live_cap_bytes && live >= opt.live_cap_bytes)
        {
            removeFile(path);
            ++res.over_budget;
            continue;
        }

        const Verdict pre = conv.precheck(f.name, store);
        if (pre == Verdict::DONE || pre == Verdict::STALE)
        {
            removeFile(path);
            ++(pre == Verdict::DONE ? res.already : res.stale);
            continue;
        }

        std::vector<Record> recs;
        const Verdict v = conv.convert(path, f.name, recs);
        res.bytes_read += f.size;
        if (v == Verdict::IO_ERROR)
        {
            ++res.failed;   // unreadable now (locked / vanished): leave it
            continue;
        }
        if (v == Verdict::STALE || v == Verdict::DONE)
        {
            removeFile(path);
            ++(v == Verdict::DONE ? res.already : res.stale);
            continue;
        }
        if (v != Verdict::OK || recs.empty())
        {
            LL_DEBUGS("CacheStore") << conv.label() << " migration: dropping invalid " << f.name << LL_ENDL;
            removeFile(path);
            ++res.bad;
            continue;
        }

        InFlight fl;
        fl.path = path;
        for (Record& rec : recs)
        {
            Slot slot;
            slot.key = rec.key;
            const U64 n = rec.payload.size();
            fl.bytes += n;
            LLCache::WriteOptions wo;
            wo.priority = LLCache::Priority::LOW;
            wo.placement = LLCache::PlacementHint::shared();   // the old files carry no region
            wo.meta = rec.meta;
            wo.meta_len = rec.meta_len;
            // Lent to the store (its writer copies it off, then drops the reference), so a
            // REFUSED_BUSY write can be retried from the same buffer.
            auto payload = std::make_shared<std::vector<U8>>(std::move(rec.payload));
            bool handled = false;
            for (int attempt = 0; attempt < 40 && !stopRequested(opt); ++attempt)
            {
                std::shared_ptr<std::vector<U8>> keep = payload;
                LLCache::Ticket<LLCache::Status> t;
                store.write(rec.key, LLCache::WriteSource::borrow(payload->data(), payload->size(), [keep]() {}), wo, &t);
                if (t.isDone())
                {
                    LLCache::Ticket<LLCache::Status> tc = t;
                    const LLCache::Status s = tc.take();
                    if (s == LLCache::Status::REFUSED_BUSY)
                    {
                        drain(true);            // let the store's queue empty, then retry
                        sleepFor(0.25, opt);
                        continue;
                    }
                    slot.has_status = true;
                    slot.status = s;
                }
                else
                {
                    slot.done = t;
                }
                handled = true;
                break;
            }
            if (!handled)
            {
                slot.has_status = true;
                slot.status = LLCache::Status::REFUSED_BUSY;
            }
            fl.slots.push_back(std::move(slot));
        }
        inflight_bytes += fl.bytes;
        live += fl.bytes;   // estimate until the next stats refresh
        inflight.push_back(std::move(fl));
        drain(false);

        if (opt.progress_log_interval_s > 0.0 && since(t0) - last_log >= opt.progress_log_interval_s)
        {
            last_log = since(t0);
            Result p = res;
            p.seconds = last_log;
            p.left = (U32)(files.size() - i - 1);
            LL_INFOS("CacheStore") << conv.label() << " migration progress: " << resultLine(p) << LL_ENDL;
        }
    }
    res.left = (U32)(files.size() - std::min(i, files.size()));
    // Push the LOW-priority runs out now instead of waiting for their flush timer.
    if (!inflight.empty()) store.flush();
    drain(true);
    res.seconds = since(t0);
    res.completed = !res.stopped && !res.disk_full && res.left == 0 && res.failed == 0;
    if (res.completed && opt.remove_dir_when_done)
    {
        std::error_code ec;
        fs::remove(fsyspath(dir), ec);   // only when empty (unknown files keep it)
    }
    return res;
}

// ------------------------------------------------------------------ Runner

Runner::~Runner()
{
    stop();
}

void Runner::start(const std::string& dir, LLCache::Store* store, std::shared_ptr<const Converter> conv,
                   const Options& opt, F64 start_delay_s, std::function<void(const Result&)> on_done)
{
    stop();
    if (!store || !conv) return;
    mStop = false;
    mDone = false;
    mThread = std::thread([this, dir, store, conv, opt, start_delay_s, on_done]()
    {
        LL_PROFILER_SET_THREAD_NAME("CacheMigrate");
        Options o = opt;
        o.stop = &mStop;
        // The store's index loads on its writer thread; then let login I/O go first.
        while (!mStop && store->isOpen() && !store->ready()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sleepFor(start_delay_s, o);
        if (mStop || !store->isOpen() || store->readOnly())
        {
            mDone = true;
            return;
        }
        LL_INFOS("CacheStore") << conv->label() << " migration: converting " << dir << " into the cache store" << LL_ENDL;
        const Result r = migrateDir(dir, *store, *conv, o);
        LL_INFOS("CacheStore") << conv->label() << " migration " << (r.completed ? "done" : "paused") << ": "
                               << resultLine(r) << LL_ENDL;
        if (on_done) on_done(r);
        mDone = true;
    });
}

void Runner::stop()
{
    mStop = true;
    if (mThread.joinable()) mThread.join();
}

} // namespace SLCacheMigrate
