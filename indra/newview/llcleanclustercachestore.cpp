/**
 * @file llcleanclustercachestore.cpp
 * @brief Cluster LOD cache on the segment store (#715, #744). See llcleanclustercachestore.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanclustercachestore.h"

#include "llcachemetrics.h"
#include "llfilesystemstore.h"
#include "fsyspath.h"
#include "threadpool.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace LLClusterFmt;
using LLCacheMetrics::Cache;
using LLCacheMetrics::Backend;

namespace
{
    constexpr U32 kMaxMetaInFlight = 16;
    constexpr U32 kMaxPageReads = 96;              // pages in flight + finished not yet taken
    constexpr U64 kMaxWriteBytes = 64ull << 20;    // serialized copies handed to the store
    constexpr F64 kMetaExpireSec = 30.0;
    constexpr U64 kMaxRecordBytes = 1ull << 30;

    F64 nowSec()
    {
        return std::chrono::duration<F64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    U64 nowUsec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    U64 since(U64 t0) { const U64 t = nowUsec(); return t > t0 ? t - t0 : 0; }

    void put32(U8* p, U32 v) { for (int i = 0; i < 4; ++i) p[i] = (U8)(v >> (8 * i)); }
    U32 get32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }

    // On-disk bytes of a raw CLUSTER record: 96-byte header + meta + payload, padded to 16.
    U64 recordDiskBytes(U64 payload) { return (96 + LLCleanClusterStore::META_LEN + payload + 15) & ~15ull; }

    // A CLUSTER record through the generic reader (range reads: one pread each).
    class StoreReader : public LLClusterCacheIO::Reader
    {
    public:
        StoreReader(LLCache::Store* s, const LLCache::Key& k) : mStore(s), mKey(k)
        {
            LLCache::RecordInfo ri;
            if (mStore && mStore->lookup(mKey, &ri) && !ri.staged) mSize = ri.raw_bytes;
        }
        U64 size() override { return mSize; }
        bool readAt(U64 off, void* dst, size_t n) override
        {
            if (!mStore || off + n > mSize) return false;
            if (n == 0) return true;
            LLCache::ReadResult r = mStore->readRangeSync(mKey, off, n);
            if (r.status == LLCache::Status::CORRUPT) mCorrupt = true;
            if (r.status != LLCache::Status::OK || r.bytes.size() != n) return false;
            memcpy(dst, r.bytes.data(), n);
            return true;
        }
        bool storeCorrupt() const { return mCorrupt; }
    private:
        LLCache::Store* mStore;
        LLCache::Key mKey;
        U64 mSize = 0;
        bool mCorrupt = false;
    };

    struct MetaSlot
    {
        LLCleanClusterCache::State state = LLCleanClusterCache::State::Loading;
        std::shared_ptr<const LLClusterMeshData> meta;
        F64 at = 0.0;
    };
    struct MetaDone { LLClusterKey key; std::shared_ptr<LLClusterMeshData> meta; std::string why; U64 usec = 0; bool store_corrupt = false; };
    struct PagesDone { LLClusterKey key; std::vector<LLCleanClusterCache::PageRead> reads; bool corrupt = false; U64 usec = 0; };
    struct WriteDone { LLClusterKey key; U64 bytes = 0; bool ok = false; U32 gen = 0; };

    struct State
    {
        LLCache::Store* store = nullptr;
        bool active = false, read_only = false, started = false;
        U64 cap = 0;
        U32 gen = 0;
        std::string dir;
        std::unique_ptr<LL::ThreadPool> pool;
        std::unordered_map<LLClusterKey, MetaSlot, LLClusterKeyHash> metas;
        std::unordered_set<LLClusterKey, LLClusterKeyHash> writing;
        std::vector<LLCleanClusterCache::PageRead> pages_ready;
        U32 meta_inflight = 0, pages_inflight = 0;
        U64 write_bytes_inflight = 0;      // render thread
        LLCleanClusterCache::Stats st;
        U64 peak = 0;

        std::mutex done_mutex;
        std::deque<MetaDone> meta_done;
        std::deque<PagesDone> pages_done;
        std::deque<WriteDone> write_done;

        bool post(std::function<void()> fn)
        {
            if (!pool) return false;
            try { return pool->getQueue().post(std::move(fn)); }
            catch (...) { return false; }
        }
        bool ensureStarted()
        {
            if (!active) return false;
            if (!started)
            {
                started = true;
                pool.reset(new LL::ThreadPool("CleanClusterStore", 1));
                pool->start();
                LL_INFOS("VKClusterCache") << "Cluster LOD cache: in the cache store " << dir << " (kind cluster), cap "
                                           << (cap >> 20) << " MB" << (read_only ? ", read-only (second instance)" : "")
                                           << LL_ENDL;
            }
            return true;
        }
        bool enabled() const { return active && cap > 0; }
        bool storeReady() const { return store && store->ready(); }
    };
    State& S()
    {
        static State s;
        return s;
    }

    // The record of `key` when it is of the current versions; a stale one is removed.
    bool currentRecord(State& d, const LLClusterKey& key, U32* header_hash)
    {
        LLCache::RecordInfo ri;
        if (!d.store->lookup(LLCleanClusterStore::key(key), &ri) || ri.staged) return false;
        if (LLCleanClusterStore::metaCurrent(ri, header_hash)) return true;
        if (!d.read_only) d.store->remove(LLCleanClusterStore::key(key));
        ++d.st.stale_deleted;
        return false;
    }
}

namespace LLCleanClusterStore
{

LLCache::Key key(const LLClusterKey& k)
{
    return LLCache::Key(k.mesh_id, LLCache::Kind::CLUSTER, (U32)k.face);
}

U8 packMeta(const ClusterFileHeader& h, U8* meta)
{
    memset(meta, 0, META_LEN);
    meta[0] = 1;
    put32(meta + 4, kFileVersion);
    put32(meta + 8, kFormatVersion);
    put32(meta + 12, kBuilderVersion);
    put32(meta + 16, h.header_hash);
    put32(meta + 20, h.payload_off);
    return META_LEN;
}

bool metaCurrent(const LLCache::RecordInfo& ri, U32* header_hash, U32* payload_off)
{
    if (ri.meta_len < META_LEN || ri.meta[0] != 1) return false;
    if (get32(ri.meta + 4) != kFileVersion || get32(ri.meta + 8) != kFormatVersion || get32(ri.meta + 12) != kBuilderVersion)
        return false;
    if (header_hash) *header_hash = get32(ri.meta + 16);
    if (payload_off) *payload_off = get32(ri.meta + 20);
    return true;
}

bool active()
{
    return S().active;
}

void init(LLCache::Store* store, U64 cap_bytes, bool read_only)
{
    State& d = S();
    shutdown();
    d.store = store;
    d.active = store != nullptr;
    d.read_only = read_only || (store && store->readOnly());
    d.cap = cap_bytes;
    d.dir = store ? store->dir() : std::string();
    ++d.gen;
    if (d.active && cap_bytes) store->setBudget(LLCache::Group::CLUSTER, cap_bytes);
    // Nothing else: the worker starts on first use (RenderVKClusterLOD on).
}

void shutdown()
{
    State& d = S();
    if (!d.active) return;
    if (d.started) LLCleanClusterCache::instance().logStats("shutdown");
    if (d.pool)
    {
        d.pool->close();   // joins the worker (its reads are synchronous store reads)
        d.pool.reset();
    }
    d.metas.clear();
    d.pages_ready.clear();
    d.writing.clear();
    d.meta_inflight = d.pages_inflight = 0;
    d.write_bytes_inflight = 0;
    {
        std::lock_guard<std::mutex> lk(d.done_mutex);
        d.meta_done.clear();
        d.pages_done.clear();
        d.write_done.clear();
    }
    ++d.gen;   // store write completions still in flight are ignored
    d.started = false;
    d.active = false;
    d.store = nullptr;
}

void setCapBytes(U64 cap_bytes)
{
    State& d = S();
    if (cap_bytes == d.cap) return;
    d.cap = cap_bytes;
    // 0 = no disk cache from now on (records stay for a later session's cap).
    if (d.active && cap_bytes) d.store->setBudget(LLCache::Group::CLUSTER, cap_bytes);
}

bool ready()
{
    State& d = S();
    return d.enabled() && d.started && d.storeReady();
}

LLCleanClusterCache::State requestMeta(const LLClusterKey& key, std::shared_ptr<const LLClusterMeshData>& out)
{
    using CState = LLCleanClusterCache::State;
    State& d = S();
    out.reset();
    if (!d.enabled() || !d.ensureStarted())
    {
        ++d.st.meta_misses;
        return CState::Missing;
    }
    if (!d.storeReady()) return CState::Loading;
    auto ms = d.metas.find(key);
    if (ms != d.metas.end())
    {
        const CState s = ms->second.state;
        if (s == CState::Ready)
        {
            out = ms->second.meta;
            d.metas.erase(ms);
            ++d.st.meta_hits;
            return CState::Ready;
        }
        if (s == CState::Failed)
        {
            d.metas.erase(ms);
            return CState::Failed;
        }
        return CState::Loading;
    }
    if (!currentRecord(d, key, nullptr))
    {
        ++d.st.meta_misses;
        LLCacheMetrics::recordRead(Cache::CLUSTER, Backend::STORE, false, 0, 0);
        return CState::Missing;
    }
    if (d.meta_inflight >= kMaxMetaInFlight) return CState::Loading;
    LLCache::Store* store = d.store;
    const U64 t0 = nowUsec();
    const bool posted = d.post([&d, store, key, t0]()
    {
        MetaDone r;
        r.key = key;
        StoreReader rd(store, LLCleanClusterStore::key(key));
        try { r.meta = LLClusterCacheIO::readMeta(rd, key, r.why); }
        catch (...) { r.meta.reset(); r.why = "exception"; }
        r.store_corrupt = rd.storeCorrupt();
        r.usec = since(t0);
        std::lock_guard<std::mutex> lk(d.done_mutex);
        d.meta_done.push_back(std::move(r));
    });
    if (!posted) return CState::Loading;
    MetaSlot& slot = d.metas[key];
    slot.state = CState::Loading;
    slot.at = nowSec();
    ++d.meta_inflight;
    store->touch(LLCleanClusterStore::key(key));
    return CState::Loading;
}

bool requestPages(const LLClusterKey& key, const U32* pages, U32 n, U64 ticket)
{
    State& d = S();
    if (!pages || n == 0 || !d.enabled() || !d.ensureStarted() || !d.storeReady()) return false;
    U32 hh = 0;
    if (!currentRecord(d, key, &hh)) return false;
    if (d.pages_inflight + (U32)d.pages_ready.size() + n > kMaxPageReads) return false;
    std::vector<U32> list(pages, pages + n);
    LLCache::Store* store = d.store;
    const U64 t0 = nowUsec();
    const bool posted = d.post([&d, store, key, hh, list, ticket, t0]()
    {
        PagesDone r;
        r.key = key;
        StoreReader rd(store, LLCleanClusterStore::key(key));
        try { LLClusterCacheIO::readPages(rd, key, hh, list, ticket, r.reads, r.corrupt); }
        catch (...)
        {
            r.reads.clear();
            for (U32 p : list)
            {
                LLCleanClusterCache::PageRead pr;
                pr.key = key; pr.page = p; pr.ticket = ticket;
                r.reads.push_back(std::move(pr));
            }
        }
        r.usec = since(t0);
        std::lock_guard<std::mutex> lk(d.done_mutex);
        d.pages_done.push_back(std::move(r));
    });
    if (!posted) return false;
    d.pages_inflight += n;
    store->touch(LLCleanClusterStore::key(key));
    return true;
}

LLCleanClusterCache::PageSource pageSource(const LLClusterKey& key, U32 header_hash)
{
    using PS = LLCleanClusterCache::PageSource;
    State& d = S();
    if (!d.enabled() || !d.ensureStarted()) return PS::None;
    if (!d.storeReady() || d.writing.count(key)) return PS::Pending;
    U32 hh = 0;
    if (!currentRecord(d, key, &hh) || hh != header_hash) return PS::None;
    return d.pages_inflight + (U32)d.pages_ready.size() >= kMaxPageReads ? PS::Full : PS::Ready;
}

void takePages(std::vector<LLCleanClusterCache::PageRead>& out)
{
    out.clear();
    out.swap(S().pages_ready);
}

void store(const std::shared_ptr<const LLClusterMeshData>& mesh)
{
    State& d = S();
    if (!mesh || !mesh->hasPayload() || !d.enabled() || d.read_only) return;
    if (mesh->header.source_lod != 3)
    {
        // Built from a lower creator LOD: served this session, never persisted.
        ++d.st.not_persisted;
        return;
    }
    if (!d.ensureStarted()) return;
    std::string why;
    if (!mesh->checkHeader(&why)) { ++d.st.write_fail; return; }
    const U64 bytes = (U64)mesh->header.payload_off + mesh->payload.size();
    if (bytes > d.cap || bytes > kMaxRecordBytes || d.writing.count(mesh->key)
        || (d.write_bytes_inflight && d.write_bytes_inflight + bytes > kMaxWriteBytes))
    {
        ++d.st.write_refused;
        return;
    }
    const LLClusterKey key = mesh->key;
    LLCache::Store* store = d.store;
    const U32 gen = d.gen;
    const LLCache::PlacementHint where = LLFileSystemStore::placement();
    std::shared_ptr<const LLClusterMeshData> keep = mesh;   // immutable: no copy until the worker serializes
    const U64 t0 = nowUsec();
    // Serialize on the worker (the payload can be MBs), then hand it to the store's writer.
    const bool posted = d.post([&d, store, key, keep, bytes, gen, where, t0]()
    {
        std::vector<U8> buf;
        auto finish = [&d, key, bytes, gen](bool ok)
        {
            WriteDone w;
            w.key = key; w.bytes = bytes; w.ok = ok; w.gen = gen;
            std::lock_guard<std::mutex> lk(d.done_mutex);
            d.write_done.push_back(w);
        };
        if (!LLClusterCacheIO::serialize(*keep, buf))
        {
            finish(false);
            return;
        }
        U8 meta[META_LEN];
        LLCache::WriteOptions o;
        o.meta_len = packMeta(keep->header, meta);
        o.meta = meta;
        o.placement = where;
        auto done = std::make_shared<LLCache::Ticket<LLCache::Status>>();
        // Inline on the store's writer thread once the record is on disk (or refused).
        o.completion.wake = [&d, done, key, bytes, gen, t0]()
        {
            const LLCache::Status st = done->take();
            const bool ok = st == LLCache::Status::OK || st == LLCache::Status::SKIPPED;
            if (st != LLCache::Status::SKIPPED)
                LLCacheMetrics::recordWrite(Cache::CLUSTER, Backend::STORE, bytes, ok ? recordDiskBytes(bytes) : 0, since(t0), ok);
            WriteDone w;
            w.key = key; w.bytes = bytes; w.ok = ok; w.gen = gen;
            std::lock_guard<std::mutex> lk(d.done_mutex);
            d.write_done.push_back(w);
        };
        store->write(LLCleanClusterStore::key(key), LLCache::WriteSource::take(std::move(buf)), o, done.get());
    });
    if (!posted) { ++d.st.write_refused; return; }
    d.writing.insert(key);
    d.write_bytes_inflight += bytes;
}

void drop(const LLClusterKey& key)
{
    State& d = S();
    d.metas.erase(key);
    if (!d.active) return;
    // A queued write of the key is dropped with it (the store's remove covers pending records).
    // A read-only instance hides the key (the store's private hidden set) without writing,
    // so it is rebuilt instead of served stale, as the file cache forgot its index entry.
    d.store->remove(LLCleanClusterStore::key(key));
}

void poll()
{
    State& d = S();
    if (!d.active || !d.started) return;
    std::deque<MetaDone> metas;
    std::deque<PagesDone> pages;
    std::deque<WriteDone> writes;
    {
        std::lock_guard<std::mutex> lk(d.done_mutex);
        metas.swap(d.meta_done);
        pages.swap(d.pages_done);
        writes.swap(d.write_done);
    }
    for (MetaDone& r : metas)
    {
        if (d.meta_inflight) --d.meta_inflight;
        if (r.meta)
            LLCacheMetrics::recordRead(Cache::CLUSTER, Backend::STORE, true, r.meta->metaBytes(), r.usec);
        else
        {
            LLCacheMetrics::recordRead(Cache::CLUSTER, Backend::STORE, false, 0, r.usec);
            LLCacheMetrics::recordCorrupt(Cache::CLUSTER, Backend::STORE);
        }
        auto ms = d.metas.find(r.key);
        if (ms == d.metas.end()) continue;   // dropped meanwhile
        if (r.meta)
        {
            ms->second.state = LLCleanClusterCache::State::Ready;
            ms->second.meta = r.meta;
        }
        else
        {
            ++d.st.corrupt;
            LL_WARNS_ONCE("VKClusterCache") << "Discarding bad cluster LOD record " << r.key.mesh_id << " face "
                                            << r.key.face << " (" << r.why << ")" << LL_ENDL;
            ms->second.state = LLCleanClusterCache::State::Failed;
            ms->second.meta.reset();
            if (!d.writing.count(r.key) && !d.read_only) d.store->remove(LLCleanClusterStore::key(r.key));
        }
        ms->second.at = nowSec();
    }
    for (PagesDone& r : pages)
    {
        d.pages_inflight -= std::min<U32>(d.pages_inflight, (U32)r.reads.size());
        if (r.corrupt)
        {
            ++d.st.corrupt;
            LLCacheMetrics::recordCorrupt(Cache::CLUSTER, Backend::STORE);
            LL_WARNS_ONCE("VKClusterCache") << "Discarding cluster LOD record with a bad page: " << r.key.mesh_id << " face "
                                            << r.key.face << LL_ENDL;
            if (!d.writing.count(r.key) && !d.read_only) d.store->remove(LLCleanClusterStore::key(r.key));
            d.metas.erase(r.key);
        }
        for (LLCleanClusterCache::PageRead& p : r.reads)
        {
            if (p.ok)
            {
                ++d.st.page_reads;
                d.st.page_read_bytes += p.bytes.size();
                LLCacheMetrics::recordRead(Cache::CLUSTER, Backend::STORE, true, p.bytes.size(), r.usec);
            }
            else
            {
                LLCacheMetrics::recordRead(Cache::CLUSTER, Backend::STORE, false, 0, r.corrupt ? r.usec : 0);
            }
            d.pages_ready.push_back(std::move(p));
        }
    }
    for (WriteDone& w : writes)
    {
        if (w.gen != d.gen) continue;
        d.write_bytes_inflight -= std::min(d.write_bytes_inflight, w.bytes);
        d.writing.erase(w.key);
        if (!w.ok) { ++d.st.write_fail; continue; }
        ++d.st.writes;
        d.st.write_bytes += w.bytes;
    }
    if (!d.metas.empty())
    {
        const F64 t = nowSec();
        for (auto it = d.metas.begin(); it != d.metas.end();)
        {
            if (it->second.state != LLCleanClusterCache::State::Loading && t - it->second.at > kMetaExpireSec) it = d.metas.erase(it);
            else ++it;
        }
    }
    if (d.pages_ready.size() > kMaxPageReads)
        d.pages_ready.erase(d.pages_ready.begin(), d.pages_ready.begin() + (d.pages_ready.size() - kMaxPageReads));
}

LLCleanClusterCache::Stats stats()
{
    State& d = S();
    LLCleanClusterCache::Stats s = d.st;
    if (d.store && d.active)
    {
        const LLCache::Stats st = d.store->stats();
        s.disk_bytes = st.kinds[(size_t)LLCache::Kind::CLUSTER].live_bytes;
        s.files = st.kinds[(size_t)LLCache::Kind::CLUSTER].records;
    }
    d.peak = std::max(d.peak, s.disk_bytes);
    s.disk_cap = d.cap;
    s.disk_peak = d.peak;
    s.pages_inflight = d.pages_inflight;
    s.meta_inflight = d.meta_inflight;
    s.write_bytes_inflight = d.write_bytes_inflight;
    s.started = d.started;
    s.scanned = d.storeReady();
    s.enabled = d.enabled();
    s.read_only = d.read_only;
    return s;
}

const std::string& dir()
{
    return S().dir;
}

// ------------------------------------------------------------------ migration

namespace
{
    const std::string kExt(".clod");
    const std::string kPart(".clod.part");
    bool endsWith(const std::string& s, const std::string& x)
    {
        return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
    }

    class FileReader : public LLClusterCacheIO::Reader
    {
    public:
        explicit FileReader(const std::vector<U8>& b) : mBytes(b) {}
        U64 size() override { return mBytes.size(); }
        bool readAt(U64 off, void* dst, size_t n) override
        {
            if (off + n > mBytes.size()) return false;
            if (n) memcpy(dst, mBytes.data() + off, n);
            return true;
        }
    private:
        const std::vector<U8>& mBytes;
    };
}

bool Converter::matches(const std::string& name) const
{
    return endsWith(name, kExt);
}

bool Converter::isLeftover(const std::string& name) const
{
    return endsWith(name, kPart);
}

SLCacheMigrate::Verdict Converter::precheck(const std::string& name, const LLCache::Store& store) const
{
    LLClusterKey k;
    U32 ver = 0;
    if (!LLClusterCacheIO::parseName(name, k, ver)) return SLCacheMigrate::Verdict::STALE;   // not a key we can ask for
    if (ver != kBuilderVersion) return SLCacheMigrate::Verdict::STALE;
    LLCache::RecordInfo ri;
    if (store.lookup(key(k), &ri) && metaCurrent(ri)) return SLCacheMigrate::Verdict::DONE;
    return SLCacheMigrate::Verdict::CONVERT;
}

SLCacheMigrate::Verdict Converter::convert(const std::string& path, const std::string& name,
                                          std::vector<SLCacheMigrate::Record>& out) const
{
    out.clear();
    LLClusterKey k;
    U32 ver = 0;
    if (!LLClusterCacheIO::parseName(name, k, ver) || ver != kBuilderVersion) return SLCacheMigrate::Verdict::STALE;
    std::error_code ec;
    const U64 size = (U64)std::filesystem::file_size(fsyspath(path), ec);
    if (ec) return SLCacheMigrate::Verdict::IO_ERROR;
    if (size <= sizeof(ClusterFileHeader) || size > kMaxRecordBytes) return SLCacheMigrate::Verdict::BAD;
    SLCacheMigrate::Record r;
    try { r.payload.resize((size_t)size); }
    catch (const std::bad_alloc&) { return SLCacheMigrate::Verdict::IO_ERROR; }
    LLFILE* fp = LLFile::fopen(path, "rb");
    if (!fp) return SLCacheMigrate::Verdict::IO_ERROR;
    const bool read_ok = fread(r.payload.data(), 1, r.payload.size(), fp) == r.payload.size();
    LLFile::close(fp);
    if (!read_ok) return SLCacheMigrate::Verdict::IO_ERROR;
    // Verified whole (metadata + every page) before it may be served from the store.
    FileReader rd(r.payload);
    std::string why;
    std::shared_ptr<LLClusterMeshData> meta = LLClusterCacheIO::readMeta(rd, k, why);
    if (!meta || !LLClusterCacheIO::verifyPayload(rd, *meta)) return SLCacheMigrate::Verdict::BAD;
    r.key = key(k);
    r.meta_len = packMeta(meta->header, r.meta);
    out.push_back(std::move(r));
    return SLCacheMigrate::Verdict::OK;
}

} // namespace LLCleanClusterStore
