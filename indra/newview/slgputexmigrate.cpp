/**
 * @file slgputexmigrate.cpp
 * @brief .gtx -> segment store migration (#714). See slgputexmigrate.h.
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

#include "slgputexmigrate.h"

#include "fsyspath.h"
#include "hbxxh.h"
#include "llerror.h"
#include "llformat.h"
#include "llgputexrecord.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    const std::string GTX_EXT(".gtx");
    const std::string PART_EXT(".gtx.part");
    constexpr U64 MAX_PAYLOAD = 512ull << 20;   // the old reader's sanity cap
    constexpr F64 MB = 1024.0 * 1024.0;

    bool endsWith(const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

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

    using Clock = std::chrono::steady_clock;
    F64 since(Clock::time_point t0) { return std::chrono::duration<F64>(Clock::now() - t0).count(); }

    bool stopRequested(const SLGpuTexMigrate::Options& opt)
    {
        return opt.stop && opt.stop->load(std::memory_order_relaxed);
    }

    // Sleep in small steps so a stop request is honoured quickly.
    void sleepFor(F64 seconds, const SLGpuTexMigrate::Options& opt)
    {
        const auto end = Clock::now() + std::chrono::duration<F64>(seconds);
        while (Clock::now() < end && !stopRequested(opt))
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<S64>(20, (S64)(seconds * 1000.0) + 1)));
    }
}

namespace SLGpuTexMigrate
{

std::string makeName(const ParsedName& n)
{
    return llformat("%s_%ux%u_e%u_l%u_v%u", n.id.asString().c_str(), (U32)n.w, (U32)n.h, (U32)n.enc,
                    (U32)n.enc_level, n.encoder_version) + (n.mask ? "_mask" : "") + GTX_EXT;
}

bool parseName(const std::string& name, ParsedName& out)
{
    const size_t ul = UUID_STR_LENGTH - 1;
    if (name.size() <= ul + GTX_EXT.size() || !endsWith(name, GTX_EXT)) return false;
    const std::string id_str = name.substr(0, ul);
    if (!LLUUID::validate(id_str)) return false;
    unsigned w = 0, h = 0, e = 0, l = 0, v = 0;
    if (sscanf(name.c_str() + ul, "_%ux%u_e%u_l%u_v%u", &w, &h, &e, &l, &v) != 5) return false;
    if (!w || !h || w > 0xFFFF || h > 0xFFFF || e > 255 || l > 255) return false;
    ParsedName n;
    n.id = LLUUID(id_str);
    n.mask = endsWith(name, "_mask" + GTX_EXT);
    n.w = (U16)w; n.h = (U16)h; n.enc = (U8)e; n.enc_level = (U8)l; n.encoder_version = v;
    if (makeName(n) != name) return false;
    out = n;
    return true;
}

const char* fileStatusName(FileStatus s)
{
    switch (s)
    {
    case FileStatus::OK:            return "ok";
    case FileStatus::BAD_NAME:      return "bad name";
    case FileStatus::TOO_SMALL:     return "too small";
    case FileStatus::BAD_HEADER:    return "bad header";
    case FileStatus::NAME_MISMATCH: return "header disagrees with name";
    case FileStatus::BAD_HASH:      return "payload hash mismatch";
    case FileStatus::IO_ERROR:      return "read error";
    case FileStatus::NOT_A_CHAIN:   return "not a full mip chain";
    }
    return "?";
}

FileStatus readFile(const std::string& path, const std::string& name, Record& out)
{
    ParsedName n;
    if (!parseName(name, n)) return FileStatus::BAD_NAME;
    std::error_code ec;
    const U64 size = (U64)fs::file_size(fsyspath(path), ec);
    if (ec) return FileStatus::IO_ERROR;
    if (size <= sizeof(LLGpuTexHeader)) return FileStatus::TOO_SMALL;

    std::ifstream in(static_cast<const std::filesystem::path&>(fsyspath(path)), std::ios::binary);
    if (!in) return FileStatus::IO_ERROR;
    LLGpuTexHeader h;
    if (!in.read(reinterpret_cast<char*>(&h), sizeof(h))) return FileStatus::IO_ERROR;
    const bool header_ok = h.magic == LLGpuTexHeader::kMagic && h.version == LLGpuTexCache::FILE_VERSION
                        && h.header_bytes == sizeof(LLGpuTexHeader) && h.header_hash == LLGpuTexRecord::headerHash(h)
                        && h.payload_bytes > 0 && h.payload_bytes <= MAX_PAYLOAD
                        && h.payload_bytes + sizeof(LLGpuTexHeader) == size;
    if (!header_ok) return FileStatus::BAD_HEADER;
    const bool hdr_mask = (h.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0;
    if (memcmp(h.id, n.id.mData, 16) != 0 || hdr_mask != n.mask || h.w != n.w || h.h != n.h || h.enc != n.enc
        || h.enc_level != n.enc_level || h.encoder_version != n.encoder_version)
    {
        return FileStatus::NAME_MISMATCH;
    }
    // (#865: written per level, so the payload must be exactly the chain the header says)
    if (h.levels != LLGpuTexRecord::mipCount(h.w, h.h)
        || LLGpuTexRecord::chainBytes(h.vk_format, h.w, h.h, h.levels) != h.payload_bytes)
    {
        return FileStatus::NOT_A_CHAIN;
    }
    try { out.payload.resize((size_t)h.payload_bytes); }
    catch (const std::bad_alloc&) { return FileStatus::IO_ERROR; }
    if (!in.read(reinterpret_cast<char*>(out.payload.data()), (std::streamsize)out.payload.size()))
        return FileStatus::IO_ERROR;
    if (HBXXH64::digest(out.payload.data(), out.payload.size()) != h.payload_hash)
    {
        out.payload.clear();
        return FileStatus::BAD_HASH;
    }
    out.id = n.id;
    out.mask = n.mask;
    out.hdr = h;
    return FileStatus::OK;
}

bool hasWork(const std::string& dir)
{
    std::error_code ec;
    fs::directory_iterator it(fsyspath(dir), ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        const std::string name = fsyspath(it->path().filename()).string();
        if (endsWith(name, GTX_EXT) || endsWith(name, PART_EXT)) return true;
    }
    return false;
}

std::string resultLine(const Result& r)
{
    const F64 secs = std::max(r.seconds, 1e-6);
    return llformat("%u files: %u converted (%.1f MB raw), %u superseded, %u already in the store, %u bad, "
                    "%u over budget (deleted), %u failed, %u left; read %.1f MB in %.1f s = %.1f files/s, %.1f MB/s",
                    r.files, r.converted, (F64)r.bytes_converted / MB, r.superseded, r.already, r.bad, r.over_budget,
                    r.failed, r.left, (F64)r.bytes_read / MB, r.seconds, (F64)(r.converted + r.superseded) / secs,
                    (F64)r.bytes_read / MB / secs)
         + (r.stopped ? " (stopped)" : "") + (r.disk_full ? " (disk full: rest left for the next start)" : "")
         + (r.completed ? " (complete)" : "");
}

Result migrateDir(const std::string& dir, LLCache::Store& store, const Options& opt)
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
            const std::string path = joinPath(dir, name);
            if (endsWith(name, PART_EXT))
            {
                // A write the old cache never finished.
                removeFile(path);
                ++res.bad;
                continue;
            }
            if (!endsWith(name, GTX_EXT)) continue;
            ParsedName pn;
            if (!parseName(name, pn))
            {
                removeFile(path);
                ++res.bad;
                continue;
            }
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

    // One file = its v2 records (#865): the file goes once every one of them is appended.
    struct InFlight { std::vector<LLCache::Ticket<LLCache::Status>> done; std::string path; U64 bytes; };
    std::deque<InFlight> inflight;
    U64 inflight_bytes = 0;
    auto settle = [&](InFlight& f)
    {
        LLCache::Status st = LLCache::Status::OK;
        for (LLCache::Ticket<LLCache::Status>& t : f.done)
        {
            const LLCache::Status s = t.take();
            if (s != LLCache::Status::OK && s != LLCache::Status::SKIPPED && st == LLCache::Status::OK) st = s;
        }
        inflight_bytes -= std::min(inflight_bytes, f.bytes);
        if (st == LLCache::Status::OK)
        {
            removeFile(f.path);
            ++res.converted;
            res.bytes_converted += f.bytes;
        }
        else
        {
            if (st == LLCache::Status::NO_SPACE) res.disk_full = true;
            ++res.failed;         // kept for the next start (its records already appended are rewritten in place)
        }
    };
    auto allDone = [](const InFlight& f)
    {
        for (const LLCache::Ticket<LLCache::Status>& t : f.done) if (!t.isDone()) return false;
        return true;
    };
    // Settle what has finished; block on the oldest while over the window.
    auto drain = [&](bool all)
    {
        while (!inflight.empty())
        {
            InFlight& f = inflight.front();
            const bool over = all || inflight.size() >= opt.max_files_in_flight || inflight_bytes >= opt.max_bytes_in_flight;
            if (!allDone(f))
            {
                if (!over) break;
                bool waited = true;
                for (const LLCache::Ticket<LLCache::Status>& t : f.done)
                    if (!t.wait(std::chrono::milliseconds(200))) { waited = false; break; }
                if (!waited) continue;   // re-check (no stop: the writes are queued anyway)
            }
            settle(f);
            inflight.pop_front();
        }
    };

    LLCache::Stats st0 = store.stats();
    U64 live = st0.kinds[(size_t)LLCache::Kind::GPUTEX].live_bytes;
    F64 last_log = 0.0;
    size_t i = 0;
    for (; i < files.size(); ++i)
    {
        if (stopRequested(opt)) { res.stopped = true; break; }
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
        const FileEntry& f = files[i];
        const std::string path = joinPath(dir, f.name);

        if ((i & 15) == 0)
        {
            live = store.stats().kinds[(size_t)LLCache::Kind::GPUTEX].live_bytes;
        }
        if (opt.live_cap_bytes && live >= opt.live_cap_bytes)
        {
            removeFile(path);
            ++res.over_budget;
            continue;
        }

        Record rec;
        ParsedName pn;
        LLGpuTexRecord::Chain have;
        if (parseName(f.name, pn) && LLGpuTexRecord::resolve(store, pn.id, pn.mask, have))
        {
            // The store already has a chain (a capture made this session). The adapter's
            // "finer version wins" rule: the old file still adds to it when it holds the
            // same encoding at a larger size (checked again on its header below); otherwise
            // the store's chain stands.
            const bool finer = have.hdr.encoder_version == pn.encoder_version && have.hdr.enc == pn.enc
                            && have.hdr.enc_level == pn.enc_level
                            && std::max(pn.w, pn.h) > std::max(have.hdr.w, have.hdr.h);
            if (!finer)
            {
                removeFile(path);
                ++res.already;
                continue;
            }
        }
        const FileStatus fst = readFile(path, f.name, rec);
        res.bytes_read += f.size;
        if (fst == FileStatus::IO_ERROR)
        {
            ++res.failed;   // unreadable now (locked / vanished): leave it
            continue;
        }
        if (fst != FileStatus::OK)
        {
            LL_DEBUGS("CacheStore") << "migrate: dropping " << f.name << ": " << fileStatusName(fst) << LL_ENDL;
            removeFile(path);
            ++res.bad;
            continue;
        }

        const U64 bytes = rec.payload.size();
        // #865: straight to the per-level layout. Finer version wins (against the store's
        // chain as it is now: a capture may have landed since the name check).
        U32 above = 0;
        const LLGpuTexRecord::WriteMode mode = LLGpuTexRecord::writeMode(store, rec.hdr, above);
        if (mode == LLGpuTexRecord::WriteMode::SKIP)
        {
            removeFile(path);   // a newer capture of the chain reached the store first
            ++res.superseded;
            continue;
        }
        std::vector<LLGpuTexRecord::PartPlan> parts;
        if (!LLGpuTexRecord::planWrite(rec.hdr, bytes, parts, above))
        {
            removeFile(path);   // (readFile checked the chain: not reached)
            ++res.bad;
            continue;
        }
        LLCache::WriteOptions wo;
        wo.priority = LLCache::Priority::LOW;
        wo.placement = LLCache::PlacementHint::shared();   // the old files carry no region
        // Lent to the store (its writer copies each record's span off, then drops the
        // reference), so a refused record can be retried from the same buffer.
        auto payload = std::make_shared<std::vector<U8>>(std::move(rec.payload));
        InFlight fl;
        fl.path = path;
        fl.bytes = bytes;
        size_t from = 0;
        LLCache::Status refused = LLCache::Status::OK;
        bool queued = false;
        for (int attempt = 0; attempt < 40 && !stopRequested(opt); ++attempt)
        {
            const std::vector<LLGpuTexRecord::PartPlan> rest(parts.begin() + (std::ptrdiff_t)from, parts.end());
            std::shared_ptr<std::vector<U8>> keep = payload;
            LLGpuTexRecord::PartWrite pw =
                LLGpuTexRecord::writeParts(store, rest, payload->data(), wo, [keep]() {});
            for (LLCache::Ticket<LLCache::Status>& t : pw.tickets) fl.done.push_back(t);
            refused = pw.refused;
            if (refused == LLCache::Status::OK) { queued = true; break; }
            if (refused != LLCache::Status::REFUSED_BUSY) break;
            from += pw.queued - 1;   // the refused record and the rest: once the queue has room
            drain(true);             // let the store's queue empty, then retry
            sleepFor(0.25, opt);
        }
        if (!queued)
        {
            // (records queued before the refusal are harmless without their tail and are
            // rewritten in place by the next start)
            if (refused == LLCache::Status::NO_SPACE) res.disk_full = true;
            ++res.failed;
        }
        else
        {
            if (mode == LLGpuTexRecord::WriteMode::FULL)
                LLGpuTexRecord::removeStale(store, rec.id, rec.mask, std::max<U32>(rec.hdr.w, rec.hdr.h));
            inflight_bytes += bytes;
            inflight.push_back(std::move(fl));
            live += bytes / 2;   // estimate until the next stats refresh (zstd ~50%, #710)
        }
        drain(false);

        if (opt.progress_log_interval_s > 0.0 && since(t0) - last_log >= opt.progress_log_interval_s)
        {
            last_log = since(t0);
            Result p = res;
            p.seconds = last_log;
            p.left = (U32)(files.size() - i - 1);
            LL_INFOS("CacheStore") << "GPU-ready migration progress: " << resultLine(p) << LL_ENDL;
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

void Runner::start(const std::string& dir, LLCache::Store* store, const Options& opt, F64 start_delay_s)
{
    stop();
    if (!store) return;
    mStop = false;
    mDone = false;
    mThread = std::thread([this, dir, store, opt, start_delay_s]()
    {
        LL_PROFILER_SET_THREAD_NAME("GpuTexMigrate");
        Options o = opt;
        o.stop = &mStop;
        // The store's index loads on its writer thread; then let login I/O go first.
        while (!mStop && !store->ready()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sleepFor(start_delay_s, o);
        if (mStop || !store->isOpen() || store->readOnly())
        {
            mDone = true;
            return;
        }
        LL_INFOS("CacheStore") << "GPU-ready migration: converting " << dir << " into the cache store" << LL_ENDL;
        const Result r = migrateDir(dir, *store, o);
        LL_INFOS("CacheStore") << "GPU-ready migration " << (r.completed ? "done" : "paused") << ": " << resultLine(r)
                               << LL_ENDL;
        mDone = true;
    });
}

void Runner::stop()
{
    mStop = true;
    if (mThread.joinable()) mThread.join();
}

} // namespace SLGpuTexMigrate
