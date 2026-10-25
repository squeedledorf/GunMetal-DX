/**
 * @file slcachestore.cpp
 * @brief The viewer's one segment store instance (#715). See slcachestore.h.
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

#include "llviewerprecompiledheaders.h"

#include "slcachestore.h"

#include "lldir.h"
#include "llfile.h"
#include "llformat.h"
#include "llgputexrecord.h"

#include <chrono>
#include <cstdlib>
#include <sstream>

// static
SLCacheStore& SLCacheStore::instance()
{
    static SLCacheStore sInstance;
    return sInstance;
}

SLCacheStore::~SLCacheStore()
{
    // close() at app shutdown normally did all of this; never leave a joinable thread.
    if (mVerify.joinable()) mVerify.join();
}

// static
std::string SLCacheStore::defaultDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "store");
}

bool SLCacheStore::open(const Options& opt)
{
    close();
    LLCache::Store::Config cfg;
    cfg.dir = defaultDir();
    cfg.force_read_only = opt.read_only;
    cfg.read_only_if_locked = true;   // locked by another process: read-only + overflow
    cfg.overflow = true;              // load-bearing once assets join (#715); harmless now
    cfg.snapshot_interval_s = opt.snapshot_interval_s;
    cfg.read_threads = opt.read_threads;
    cfg.decode_threads = opt.decode_threads;
    // The index (snapshot + tail scan, or a full scan without a snapshot) loads on the
    // store's writer thread: login never waits for it, every lookup misses until ready
    // (the same rule as the old GPU-ready cache's worker scan).
    cfg.async_open = true;

    std::unique_ptr<LLCache::Store> store(new LLCache::Store);
    if (!store->open(cfg))
    {
        LL_WARNS("CacheStore") << "Cannot open the cache store at " << cfg.dir
                               << "; the caches that use it fall back to their old files" << LL_ENDL;
        return false;
    }

    // GPU-ready share of the TEXTURES group: uncapped here; SLTextureTiers::start() (run
    // right after open) sets the real TEXTURES budget and GPUTEX share cap (#705: capped
    // at the GPU-ready budget when J2C shares the group).
    LLCache::KindPolicy gp = store->kindPolicy(LLCache::Kind::GPUTEX);
    gp.share_cap = 1.f;
    gp.compress = true;   // zstd (#710); level 0 = auto (3, or 1 on <= 4 threads)
    store->setKindPolicy(LLCache::Kind::GPUTEX, gp);
    if (opt.textures_budget) store->setBudget(LLCache::Group::TEXTURES, opt.textures_budget);
    if (opt.assets_budget) store->setBudget(LLCache::Group::ASSETS, opt.assets_budget);
    if (opt.cluster_budget) store->setBudget(LLCache::Group::CLUSTER, opt.cluster_budget);

    mStore = std::move(store);
    if (opt.purge && !mStore->readOnly())
    {
        // Clear Cache: waits for the index load, then deletes every segment.
        LL_INFOS("CacheStore") << "Clear Cache: emptying the cache store" << LL_ENDL;
        mStore->clearAll();
    }
    LL_INFOS("CacheStore") << "Cache store " << cfg.dir << (mStore->readOnly() ? " opened read-only" : " opened")
                           << ", textures budget " << (mStore->budget(LLCache::Group::TEXTURES) >> 20)
                           << " MB, assets budget " << (mStore->budget(LLCache::Group::ASSETS) >> 20)
                           << " MB, cluster budget " << (mStore->budget(LLCache::Group::CLUSTER) >> 20)
                           << " MB, snapshot every " << opt.snapshot_interval_s << " s" << LL_ENDL;
    return true;
}

void SLCacheStore::close()
{
    if (mVerify.joinable()) mVerify.join();
    if (mStore)
    {
        if (mStore->isOpen())
        {
            mStore->logStats("shutdown");
            mStore->close();
        }
        mStore.reset();
    }
}

void SLCacheStore::setTexturesBudget(U64 bytes)
{
    if (isOpen() && bytes) mStore->setBudget(LLCache::Group::TEXTURES, bytes);
}

void SLCacheStore::logStats(const char* when)
{
    if (!isOpen())
    {
        LL_INFOS("CacheStore") << "Cache store not open" << LL_ENDL;
        return;
    }
    mStore->logStats(when);
    const LLCache::Stats s = mStore->stats();
    std::ostringstream o;
    for (size_t k = 1; k < (size_t)LLCache::Kind::COUNT; ++k)
    {
        if (!s.kinds[k].records) continue;
        o << " " << LLCache::kindName((LLCache::Kind)k) << " " << s.kinds[k].records << " rec / "
          << llformat("%.1f", (F64)s.kinds[k].live_bytes / (1024.0 * 1024.0)) << " MB;";
    }
    LL_INFOS("CacheStore") << "Cache store kinds:" << (o.str().empty() ? std::string(" (empty)") : o.str())
                           << " ready " << s.ready << ", read-only " << s.read_only << ", queued writes "
                           << s.writes_queued << " (" << (s.write_bytes_queued >> 20) << " MB), refused "
                           << s.writes_refused << ", write failures " << s.write_fail << ", skipped " << s.skipped
                           << ", superseded " << s.superseded << ", overflow records " << s.overflow_records
                           << ", read " << (s.bytes_read >> 20) << " MB, written " << (s.bytes_written_total >> 20)
                           << " MB total" << LL_ENDL;
}

bool SLCacheStore::verifyAllAsync()
{
    if (!isOpen() || mVerifying.exchange(true)) return false;
    if (mVerify.joinable()) mVerify.join();
    LLCache::Store* store = mStore.get();
    LL_INFOS("CacheStore") << "Cache store verify started (reads every record)" << LL_ENDL;
    mVerify = std::thread([this, store]()
    {
        LL_PROFILER_SET_THREAD_NAME("CacheStoreVerify");
        const auto t0 = std::chrono::steady_clock::now();
        U32 n = 0;
        U32 by_status[16] = {};
        const U32 bad = store->verifyAll([&](const LLCache::Key& k, LLCache::Status st)
        {
            ++n;
            by_status[std::min<size_t>((size_t)st, 15)]++;
            if (st != LLCache::Status::OK)
            {
                LL_INFOS("CacheStore") << "verify: " << LLCache::kindName(k.kind) << " " << k.id << " v" << k.variant
                                       << ": " << LLCache::statusName(st) << LL_ENDL;
            }
        });
        const F64 secs = std::chrono::duration<F64>(std::chrono::steady_clock::now() - t0).count();
        std::ostringstream o;
        for (size_t i = 0; i < 16; ++i)
            if (by_status[i]) o << " " << LLCache::statusName((LLCache::Status)i) << "=" << by_status[i];
        LL_INFOS("CacheStore") << "Cache store verify done: " << n << " records, " << bad << " bad (dropped) in "
                               << llformat("%.1f", secs) << " s;" << o.str() << LL_ENDL;
        store->logStats("after verify");
        mVerifying = false;
    });
    return true;
}

U32 SLCacheStore::listRecords(LLCache::Kind kind, const std::string& path)
{
    if (!isOpen()) return 0;
    LLFILE* fp = LLFile::fopen(path, "wb");
    if (!fp)
    {
        LL_WARNS("CacheStore") << "Cannot write " << path << LL_ENDL;
        return 0;
    }
    fprintf(fp, "# kind uuid variant stored_bytes raw_bytes codec write_time last_access prev_session_access reuse"
                " flags region [gputex: layout part w x h levels enc level encver]\n"
                "# gputex variant: bit 0 mask, bit 8 layout v2, bits 9-13 level log2 (0 = tail); see llgputexrecord.h\n");
    U32 n = 0;
    mStore->forEach(kind, [&](const LLCache::RecordInfo& r) -> bool
    {
        const std::string extra = LLGpuTexRecord::describe(r);
        const std::string flags = std::string(r.queued ? "Q" : "") + (r.staged ? "S" : "") + (r.pinned ? "P" : "")
                                + (r.shared ? "H" : "");
        fprintf(fp, "%s %s %u %u %u %u %u %u %u %u %s %s%s\n", LLCache::kindName(r.key.kind),
                r.key.id.asString().c_str(), r.key.variant, r.stored_bytes, r.raw_bytes, (U32)r.codec, r.write_time,
                r.last_access, r.prev_session_access, (U32)r.reuse_count, flags.empty() ? "-" : flags.c_str(),
                r.region.isNull() ? "shared" : r.region.asString().c_str(), extra.c_str());
        ++n;
        return true;
    });
    LLFile::close(fp);
    LL_INFOS("CacheStore") << "Listed " << n << " " << (kind == LLCache::Kind::NONE ? "" : LLCache::kindName(kind))
                           << " records to " << path << LL_ENDL;
    return n;
}

bool SLCacheStore::extractGpuTexChain(const LLUUID& id, bool mask, const std::string& path)
{
    if (!isOpen()) return false;
    LLGpuTexRecord::Chain c;
    LLGpuTexRecord::ReadPlan plan;
    if (!LLGpuTexRecord::resolve(*mStore, id, mask, c) || !LLGpuTexRecord::planRead(c, 0, 0, plan))
    {
        LL_INFOS("CacheStore") << "Extract gputex chain " << id << (mask ? " (mask)" : "") << ": no chain" << LL_ENDL;
        return false;
    }
    std::vector<LLCache::ReadResult> results;
    for (size_t i : plan.recs) results.push_back(mStore->readSync(c.recs[i].key));
    LLGpuTexCache::Blob blob;
    size_t bad = 0;
    if (!LLGpuTexRecord::assemble(c, plan, results, blob, bad))
    {
        LL_INFOS("CacheStore") << "Extract gputex chain " << id << ": record " << bad << " of " << plan.recs.size()
                               << " failed (" << (bad < results.size() ? LLCache::statusName(results[bad].status) : "?")
                               << ")" << LL_ENDL;
        return false;
    }
    // The whole chain, level 0 first (as a .gtx payload), from its records in order.
    LLFILE* fp = LLFile::fopen(path, "wb");
    bool ok = fp != nullptr;
    for (const LLGpuTexCache::Blob::Piece& p : blob.pieces)
        ok = ok && fwrite(p.bytes.data(), 1, p.bytes.size(), fp) == p.bytes.size();
    if (fp) ok = LLFile::close(fp) == 0 && ok;
    LL_INFOS("CacheStore") << "Extract gputex chain " << id << (mask ? " (mask)" : "") << ": " << (c.v1 ? "v1" : "v2")
                           << " " << c.hdr.w << "x" << c.hdr.h << ", " << c.hdr.levels << " levels from "
                           << plan.recs.size() << " record(s), " << blob.bytes() << " bytes -> " << path
                           << (ok ? "" : " FAILED") << LL_ENDL;
    return ok;
}

bool SLCacheStore::extractRecord(const LLCache::Key& key, const std::string& path)
{
    if (!isOpen()) return false;
    LLCache::ReadResult r = mStore->readSync(key);
    if (r.status != LLCache::Status::OK)
    {
        LL_INFOS("CacheStore") << "Extract " << LLCache::kindName(key.kind) << " " << key.id << " v" << key.variant
                               << ": " << LLCache::statusName(r.status) << LL_ENDL;
        return false;
    }
    LLFILE* fp = LLFile::fopen(path, "wb");
    bool ok = fp && fwrite(r.bytes.data(), 1, r.bytes.size(), fp) == r.bytes.size();
    if (fp) ok = LLFile::close(fp) == 0 && ok;
    LL_INFOS("CacheStore") << "Extract " << LLCache::kindName(key.kind) << " " << key.id << " v" << key.variant << ": "
                           << r.bytes.size() << " bytes (stored " << r.info.stored_bytes << ", codec "
                           << (U32)r.info.codec << ", meta " << (U32)r.info.meta_len << " bytes) -> " << path
                           << (ok ? "" : " FAILED") << LL_ENDL;
    return ok;
}

// static
bool SLCacheStore::parseKind(const std::string& text, LLCache::Kind& out)
{
    for (U32 k = 0; k < (U32)LLCache::Kind::COUNT; ++k)
    {
        if (text == LLCache::kindName((LLCache::Kind)k) || text == std::to_string(k))
        {
            out = (LLCache::Kind)k;
            return true;
        }
    }
    return false;
}

// static
bool SLCacheStore::parseKey(const std::string& text, LLCache::Key& out, bool* gputex_chain)
{
    std::istringstream in(text);
    std::string id, kind, variant;
    in >> id >> kind >> variant;
    if (!LLUUID::validate(id)) return false;
    out = LLCache::Key(LLUUID(id), LLCache::Kind::GPUTEX, 0);
    if (!kind.empty() && !parseKind(kind, out.kind)) return false;
    // #865: a GPU-ready texture is several records; with no variant (or "chain" /
    // "chain-mask") the request is its whole chain, out.variant = the mask bit.
    const bool chain = out.kind == LLCache::Kind::GPUTEX && (variant.empty() || variant == "chain" || variant == "chain-mask");
    if (gputex_chain) *gputex_chain = chain;
    if (chain)
    {
        out.variant = variant == "chain-mask" ? LLGpuTexRecord::VARIANT_MASK : 0u;
        return true;
    }
    if (!variant.empty())
    {
        char* end = nullptr;
        const unsigned long v = strtoul(variant.c_str(), &end, 0);
        if (!end || *end) return false;
        out.variant = (U32)v;
    }
    return true;
}
