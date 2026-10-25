/**
 * @file llcleanclustercache.cpp
 * @brief Cluster LOD disk cache (#744, OWNER: 1a). See llcleanclustercache.h.
 *
 * Pattern of the GPU-ready texture cache (#274, llgputexcache.cpp): one file per key in its
 * own folder, writes to <name>.part + rename, *.part deleted by the startup scan, every read
 * verified (a bad file is deleted and reported as a miss), a byte budget with LRU by file
 * mtime (refreshed at most hourly), bounded reads / writes in flight, disk I/O on the cache's
 * own worker pool, results merged on the render thread by poll().
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanclustercache.h"
#include "llcleanclustercachestore.h"
#include "llcleanclusterbuild.h"

#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llformat.h"
#include "llviewercontrol.h"
#include "llcachemetrics.h"
#include "threadpool.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace LLClusterFmt;

// ============================================================================================
// LLClusterKey / LLClusterMeshData
// ============================================================================================
U64 LLClusterKey::hash() const
{
    // FNV-1a 64 over the UUID bytes and the face.
    U64 h = 1469598103934665603ull;
    for (U32 i = 0; i < UUID_BYTES; ++i) { h ^= mesh_id.mData[i]; h *= 1099511628211ull; }
    const U32 f = (U32)face;
    for (U32 i = 0; i < 4; ++i) { h ^= (f >> (8 * i)) & 0xFFu; h *= 1099511628211ull; }
    return h;
}

std::string LLClusterKey::fileName() const
{
    return mesh_id.asString() + "_f" + std::to_string(face) + "_b" + std::to_string(LLClusterFmt::kBuilderVersion) + ".clod";
}

U64 LLClusterMeshData::metaBytes() const
{
    return sizeof(*this) + groups.capacity() * sizeof(LLClusterFmt::ClusterGroup)
         + pages.capacity() * sizeof(LLClusterFmt::ClusterPageDir) + deps.capacity() * sizeof(U32);
}

U64 LLClusterMeshData::totalBytes() const { return metaBytes() + payload.capacity(); }

bool LLClusterMeshData::copyPage(U32 page, U8* dst) const
{
    if (!dst || page >= pages.size() || payload.empty()) return false;
    const ClusterPageDir& d = pages[page];
    if (header.compression != kCompNone || d.used_bytes > kPageBytes || d.used_bytes < kPageHeaderBytes
        || d.stored_bytes != d.used_bytes || (U64)d.offset + d.stored_bytes > payload.size())
        return false;
    if (fnv1a32(payload.data() + d.offset, d.stored_bytes) != d.hash) return false;
    std::memcpy(dst, payload.data() + d.offset, d.used_bytes);
    std::memset(dst + d.used_bytes, 0, kPageBytes - d.used_bytes);
    return true;
}

namespace
{
U32 headerHash(const ClusterFileHeader& h)
{
    ClusterFileHeader c = h;
    c.header_hash = 0;
    return fnv1a32(&c, sizeof(c));
}
}

void LLClusterMeshData::finalizeLayout()
{
    ClusterFileHeader& h = header;
    h.magic = kFileMagic;
    h.file_version = kFileVersion;
    h.format_version = kFormatVersion;
    h.builder_version = kBuilderVersion;
    h.header_bytes = sizeof(ClusterFileHeader);
    std::memcpy(h.mesh_id, key.mesh_id.mData, 16);
    h.face = key.face;
    h.page_bytes = kPageBytes;
    h.page_count = (U32)pages.size();
    h.group_count = (U32)groups.size();
    h.groups_off = sizeof(ClusterFileHeader);
    h.pagedir_off = h.groups_off + (U32)(groups.size() * sizeof(ClusterGroup));
    h.deps_off = h.pagedir_off + (U32)(pages.size() * sizeof(ClusterPageDir));
    h.deps_count = (U32)deps.size();
    h.payload_off = h.deps_off + (U32)(deps.size() * sizeof(U32));
    h.payload_bytes = payload.size();
    h.pad0 = 0;
    h.pad1 = 0;
    h.header_hash = headerHash(h);
}

bool LLClusterMeshData::checkHeader(std::string* why) const
{
    const ClusterFileHeader& h = header;
    const char* bad = nullptr;
    if (h.magic != kFileMagic) bad = "file magic";
    else if (h.file_version != kFileVersion) bad = "file version";
    else if (h.format_version != kFormatVersion) bad = "format version";
    else if (h.builder_version != kBuilderVersion) bad = "builder version";
    else if (h.header_bytes != sizeof(ClusterFileHeader)) bad = "header size";
    else if (h.header_hash != headerHash(h)) bad = "header hash";
    else if (std::memcmp(h.mesh_id, key.mesh_id.mData, 16) != 0 || h.face != key.face) bad = "key mismatch";
    else if (h.page_bytes != kPageBytes) bad = "page size";
    else if (h.compression != kCompNone && h.compression != kCompGDeflate) bad = "compression";
    else if (h.group_count < kFirstRealGroup || h.group_count > kMaxGroupsTotal) bad = "group count";
    else if (h.page_count == 0 || h.page_count > kMaxVirtualPages || h.root_pages == 0 || h.root_pages > h.page_count)
        bad = "page count";
    else if (h.groups_off != sizeof(ClusterFileHeader)
             || h.pagedir_off != h.groups_off + (U64)h.group_count * sizeof(ClusterGroup)
             || h.deps_off != h.pagedir_off + (U64)h.page_count * sizeof(ClusterPageDir)
             || h.payload_off != h.deps_off + (U64)h.deps_count * sizeof(U32))
        bad = "layout offsets";
    else if (!groups.empty() && groups.size() != h.group_count) bad = "group vector";
    else if (!pages.empty() && pages.size() != h.page_count) bad = "page vector";
    else if (!pages.empty() && deps.size() != h.deps_count) bad = "deps vector";
    else if (!payload.empty() && payload.size() != h.payload_bytes) bad = "payload size";
    if (bad && why) *why = bad;
    return bad == nullptr;
}

// ============================================================================================
// LLCleanClusterCache
// ============================================================================================
namespace
{
const std::string kExt(".clod");
const std::string kPartExt(".part");
constexpr std::time_t kMtimeTouchSec = 60 * 60;   // like the GPU texture cache
constexpr U32 kMaxMetaInFlight = 16;
constexpr U32 kMaxPageReads = 96;                  // pages in flight + finished not yet taken (~7.5 MB)
constexpr U64 kMaxWriteBytes = 64ull << 20;        // copies queued for disk
constexpr F64 kMetaExpireSec = 30.0;               // a Ready / Failed result nobody asked for again
constexpr U64 kMaxFileBytes = 1ull << 30;          // sanity bound on one .clod

boost::filesystem::path fsPath(const std::string& path)
{
#if LL_WINDOWS
    return boost::filesystem::path(ll_convert<std::wstring>(path));
#else
    return boost::filesystem::path(path);
#endif
}

F64 nowSec()
{
    return std::chrono::duration<F64>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A .clod file through the generic reader (#715: the same verification serves the files and
// the store's CLUSTER records, llcleanclustercachestore.h).
class FileReader : public LLClusterCacheIO::Reader
{
public:
    explicit FileReader(const std::string& path) : mFp(LLFile::fopen(path, "rb"))
    {
        if (mFp && fseek(mFp, 0, SEEK_END) == 0)
        {
            const long n = ftell(mFp);
            mSize = n > 0 ? (U64)n : 0;
        }
    }
    ~FileReader() override { if (mFp) LLFile::close(mFp); }
    bool ok() const { return mFp != nullptr; }
    U64 size() override { return mSize; }
    bool readAt(U64 off, void* dst, size_t n) override
    {
        if (!mFp || off + n > mSize) return false;
        if (n == 0) return true;
        return fseek(mFp, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, mFp) == n;
    }
private:
    LLFILE* mFp = nullptr;
    U64 mSize = 0;
};

using LLClusterCacheIO::parseName;

// Worker: read + verify the metadata of one file.
std::shared_ptr<LLClusterMeshData> runReadMeta(const LLClusterKey& key, const std::string& path, std::string& why)
{
    FileReader r(path);
    if (!r.ok()) { why = "open"; return nullptr; }
    return LLClusterCacheIO::readMeta(r, key, why);
}

// Worker: read + verify pages of one file whose header hash must still be `expect_hash`.
void runReadPages(const LLClusterKey& key, const std::string& path, U32 expect_hash, const std::vector<U32>& pages,
                  U64 ticket, std::vector<LLCleanClusterCache::PageRead>& out, bool& corrupt)
{
    FileReader r(path);
    if (!r.ok())
    {
        corrupt = false;
        out.clear();
        for (U32 p : pages)
        {
            LLCleanClusterCache::PageRead pr;
            pr.key = key; pr.page = p; pr.ticket = ticket;
            out.push_back(std::move(pr));
        }
        return;
    }
    LLClusterCacheIO::readPages(r, key, expect_hash, pages, ticket, out, corrupt);
}
} // namespace

// ============================================================================================
// LLClusterCacheIO: verification shared by the file and store backends
// ============================================================================================
namespace LLClusterCacheIO
{

// "<uuid>_f<face>_b<ver>.clod" -> key + version. False = not one of ours.
bool parseName(const std::string& name, LLClusterKey& key, U32& ver)
{
    const size_t ul = UUID_STR_LENGTH - 1;
    if (name.size() <= ul + kExt.size() || name.compare(name.size() - kExt.size(), kExt.size(), kExt) != 0)
        return false;
    const std::string id = name.substr(0, ul);
    if (!LLUUID::validate(id)) return false;
    int face = 0;
    unsigned v = 0;
    if (sscanf(name.c_str() + ul, "_f%d_b%u", &face, &v) != 2 || face < 0 || face > 4096) return false;
    key.mesh_id = LLUUID(id);
    key.face = face;
    ver = v;
    return id + "_f" + std::to_string(face) + "_b" + std::to_string(v) + kExt == name;
}

// Read + verify the header against the key and the container size.
bool readHeader(Reader& r, const LLClusterKey& key, ClusterFileHeader& h, std::string& why)
{
    const U64 total = r.size();
    if (total <= sizeof(ClusterFileHeader) || total > kMaxFileBytes) { why = "file size"; return false; }
    if (!r.readAt(0, &h, sizeof(h))) { why = "short header"; return false; }
    LLClusterMeshData probe;
    probe.key = key;
    probe.header = h;
    if (!probe.checkHeader(&why)) return false;
    if (h.compression != kCompNone) { why = "compressed (no CPU decoder)"; return false; }
    if ((U64)h.payload_off + h.payload_bytes != total) { why = "file size"; return false; }
    return true;
}

std::shared_ptr<LLClusterMeshData> readMeta(Reader& r, const LLClusterKey& key, std::string& why)
{
    std::shared_ptr<LLClusterMeshData> m = std::make_shared<LLClusterMeshData>();
    m->key = key;
    if (!readHeader(r, key, m->header, why)) return nullptr;
    const ClusterFileHeader& h = m->header;
    try
    {
        m->groups.resize(h.group_count);
        m->pages.resize(h.page_count);
        m->deps.resize(h.deps_count);
    }
    catch (const std::bad_alloc&) { why = "alloc"; return nullptr; }
    if (!r.readAt(h.groups_off, m->groups.data(), sizeof(ClusterGroup) * h.group_count)) { why = "groups"; return nullptr; }
    if (!r.readAt(h.pagedir_off, m->pages.data(), sizeof(ClusterPageDir) * h.page_count)) { why = "pagedir"; return nullptr; }
    if (h.deps_count && !r.readAt(h.deps_off, m->deps.data(), sizeof(U32) * h.deps_count)) { why = "deps"; return nullptr; }
    for (const ClusterPageDir& d : m->pages)
    {
        if ((U64)d.offset + d.stored_bytes > h.payload_bytes || d.used_bytes > kPageBytes
            || d.used_bytes < kPageHeaderBytes || d.stored_bytes != d.used_bytes)
        {
            why = "page directory range";
            return nullptr;
        }
    }
    if (!LLCleanClusterBuilder::validate(*m, &why)) return nullptr;
    return m;
}

void readPages(Reader& r, const LLClusterKey& key, U32 expect_hash, const std::vector<U32>& pages, U64 ticket,
               std::vector<LLCleanClusterCache::PageRead>& out, bool& corrupt)
{
    corrupt = false;
    out.clear();
    for (U32 p : pages)
    {
        LLCleanClusterCache::PageRead pr;
        pr.key = key;
        pr.page = p;
        pr.ticket = ticket;
        out.push_back(std::move(pr));
    }
    std::string why;
    ClusterFileHeader h{};
    if (!readHeader(r, key, h, why))
    {
        corrupt = true;
        return;
    }
    if (h.header_hash != expect_hash)
    {
        // Rewritten since the metadata was served (a rebuild): not corrupt, just not ours.
        return;
    }
    for (LLCleanClusterCache::PageRead& pr : out)
    {
        if (pr.page >= h.page_count) continue;
        ClusterPageDir d{};
        if (!r.readAt(h.pagedir_off + (U64)pr.page * sizeof(ClusterPageDir), &d, sizeof(d))) { corrupt = true; break; }
        if (d.used_bytes > kPageBytes || d.used_bytes < kPageHeaderBytes || d.stored_bytes != d.used_bytes
            || (U64)d.offset + d.stored_bytes > h.payload_bytes)
        { corrupt = true; break; }
        try { pr.bytes.assign(kPageBytes, 0); }
        catch (const std::bad_alloc&) { pr.bytes.clear(); continue; }
        if (!r.readAt(h.payload_off + d.offset, pr.bytes.data(), d.stored_bytes)
            || fnv1a32(pr.bytes.data(), d.stored_bytes) != d.hash)
        { corrupt = true; pr.bytes.clear(); break; }
        ClusterPageHeader ph;
        std::memcpy(&ph, pr.bytes.data(), sizeof(ph));
        if (ph.magic != kPageMagic || ph.page_index != pr.page || ph.used_bytes != d.used_bytes
            || (ph.version_count >> 16) != kFormatVersion
            || fnv1a32(pr.bytes.data() + kPageHeaderBytes, d.used_bytes - kPageHeaderBytes) != ph.hash)
        { corrupt = true; pr.bytes.clear(); break; }
        pr.ok = true;
    }
    if (corrupt)
        for (LLCleanClusterCache::PageRead& pr : out) { pr.ok = false; pr.bytes.clear(); }
}

bool verifyPayload(Reader& r, const LLClusterMeshData& meta)
{
    // In chunks: readPages holds one kPageBytes buffer per page asked for.
    constexpr U32 CHUNK = 32;
    const U32 n = (U32)meta.pages.size();
    std::vector<U32> list;
    std::vector<LLCleanClusterCache::PageRead> out;
    for (U32 first = 0; first < n; first += CHUNK)
    {
        list.clear();
        for (U32 i = first; i < std::min(n, first + CHUNK); ++i) list.push_back(i);
        bool corrupt = false;
        readPages(r, meta.key, meta.header.header_hash, list, 0, out, corrupt);
        if (corrupt) return false;
        for (const LLCleanClusterCache::PageRead& pr : out) if (!pr.ok) return false;
    }
    return true;
}

bool serialize(const LLClusterMeshData& m, std::vector<U8>& out)
{
    const U64 total = (U64)m.header.payload_off + m.payload.size();
    if (total > kMaxFileBytes) return false;
    try { out.resize((size_t)total); }
    catch (const std::bad_alloc&) { out.clear(); return false; }
    U8* p = out.data();
    std::memcpy(p, &m.header, sizeof(m.header));
    if (!m.groups.empty()) std::memcpy(p + m.header.groups_off, m.groups.data(), m.groups.size() * sizeof(ClusterGroup));
    if (!m.pages.empty()) std::memcpy(p + m.header.pagedir_off, m.pages.data(), m.pages.size() * sizeof(ClusterPageDir));
    if (!m.deps.empty()) std::memcpy(p + m.header.deps_off, m.deps.data(), m.deps.size() * sizeof(U32));
    if (!m.payload.empty()) std::memcpy(p + m.header.payload_off, m.payload.data(), m.payload.size());
    return true;
}

} // namespace LLClusterCacheIO

namespace
{

bool runWrite(const std::string& final_path, const LLClusterMeshData& m)
{
    const std::string part = final_path + kPartExt;
    LLFILE* fp = LLFile::fopen(part, "wb");
    if (!fp) return false;
    bool ok = fwrite(&m.header, sizeof(m.header), 1, fp) == 1
           && fwrite(m.groups.data(), sizeof(ClusterGroup), m.groups.size(), fp) == m.groups.size()
           && fwrite(m.pages.data(), sizeof(ClusterPageDir), m.pages.size(), fp) == m.pages.size()
           && (m.deps.empty() || fwrite(m.deps.data(), sizeof(U32), m.deps.size(), fp) == m.deps.size())
           && fwrite(m.payload.data(), 1, m.payload.size(), fp) == m.payload.size();
    ok = (LLFile::close(fp) == 0) && ok;
    if (ok) ok = LLFile::replace(part, final_path) == 0;
    if (!ok) LLFile::remove(part, ENOENT);
    return ok;
}
} // namespace

struct LLCleanClusterCache::Impl
{
    struct Entry
    {
        U64 bytes = 0;
        std::time_t disk_time = 0;
        U32 header_hash = 0;          // of the last metadata served (0 = not read this session)
        std::list<LLClusterKey>::iterator lru;
    };
    struct MetaSlot
    {
        State state = State::Loading;
        std::shared_ptr<const LLClusterMeshData> meta;
        F64 at = 0.0;
    };
    // worker -> render thread
    struct MetaDone { LLClusterKey key; std::shared_ptr<LLClusterMeshData> meta; std::string why; U64 usec = 0; };
    struct PagesDone { LLClusterKey key; std::vector<PageRead> reads; bool corrupt = false; U64 usec = 0; };
    struct WriteDone { LLClusterKey key; U64 bytes = 0; U32 header_hash = 0; bool ok = false; U64 usec = 0; };
    struct ScanResult
    {
        struct Found { LLClusterKey key; U64 bytes; std::time_t t; };
        std::vector<Found> found;
        U64 stale = 0;
        U64 usec = 0;     // scan time (LLCacheMetrics startup)
    };
    F64 next_footprint = 0.0;   // LLCacheMetrics footprint sample (every 60 s, from the index)

    // configuration
    std::string dir;
    U64 cap = 0;
    bool purge = false, read_only = false, configured = false;
    // state
    bool started = false, start_failed = false, scanned = false;
    std::unique_ptr<LL::ThreadPool> pool;
    std::unordered_map<LLClusterKey, Entry, LLClusterKeyHash> index;
    std::list<LLClusterKey> lru;                 // front = least recently used
    std::unordered_map<LLClusterKey, MetaSlot, LLClusterKeyHash> metas;
    std::unordered_set<LLClusterKey, LLClusterKeyHash> writing, dropped_while_writing;
    std::vector<PageRead> pages_ready;
    U32 meta_inflight = 0, pages_inflight = 0;
    U64 write_bytes_inflight = 0;
    U64 used = 0, peak = 0;
    Stats st;

    std::mutex done_mutex;
    std::unique_ptr<ScanResult> scan_done;
    std::deque<MetaDone> meta_done;
    std::deque<PagesDone> pages_done;
    std::deque<WriteDone> write_done;

    bool enabled() const { return configured && cap > 0 && !start_failed; }
    std::string pathOf(const LLClusterKey& k) const { return gDirUtilp->add(dir, k.fileName()); }

    bool post(std::function<void()> fn)
    {
        if (!pool) return false;
        try { return pool->getQueue().post(std::move(fn)); }
        catch (...) { return false; }
    }

    void forget(std::unordered_map<LLClusterKey, Entry, LLClusterKeyHash>::iterator it)
    {
        used -= std::min(used, it->second.bytes);
        lru.erase(it->second.lru);
        index.erase(it);
    }

    void addEntry(const LLClusterKey& k, U64 bytes, std::time_t t, U32 hh)
    {
        auto it = index.find(k);
        if (it != index.end()) forget(it);
        Entry& e = index[k];
        e.bytes = bytes;
        e.disk_time = t;
        e.header_hash = hh;
        e.lru = lru.insert(lru.end(), k);
        used += bytes;
        peak = std::max(peak, used);
    }

    void removeFile(const LLClusterKey& k)
    {
        auto it = index.find(k);
        if (it != index.end()) forget(it);
        if (!read_only) LLFile::remove(pathOf(k), ENOENT);
    }

    void touch(const LLClusterKey& k)
    {
        auto it = index.find(k);
        if (it == index.end()) return;
        lru.splice(lru.end(), lru, it->second.lru);
        const std::time_t now = std::time(nullptr);
        if (!read_only && now - it->second.disk_time > kMtimeTouchSec)
        {
            boost::system::error_code ec;
            boost::filesystem::last_write_time(fsPath(pathOf(k)), now, ec);
            it->second.disk_time = now;
        }
    }

    // Least recently used out until under the cap (never `protect`).
    void evict(const LLClusterKey* protect)
    {
        if (read_only || cap == 0) return;
        auto it = lru.begin();
        while (used > cap && it != lru.end())
        {
            const LLClusterKey k = *it;
            ++it;
            if (protect && k == *protect) continue;
            if (writing.count(k)) continue;
            auto ie = index.find(k);
            if (ie != index.end())
                LLCacheMetrics::recordEvict(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, ie->second.bytes);
            removeFile(k);
            metas.erase(k);
            ++st.evicted;
        }
    }

    // Lazy start: folder, pool, scan. Only reached when the streamer asks (toggle on).
    bool ensureStarted()
    {
        if (started) return !start_failed;
        if (!configured || cap == 0) return false;
        started = true;
        boost::system::error_code ec;
        boost::filesystem::create_directories(fsPath(dir), ec);
        if (!LLFile::isdir(dir))
        {
            LL_WARNS("VKClusterCache") << "Cluster LOD cache: cannot create " << dir
                                       << (ec.failed() ? ": " + ec.message() : std::string())
                                       << " -- running without a disk cache" << LL_ENDL;
            start_failed = true;
            return false;
        }
        pool.reset(new LL::ThreadPool("CleanClusterCache", 1));
        pool->start();
        const std::string d = dir;
        const bool pg = purge, ro = read_only;
        auto scan = [d, pg, ro]()
        {
            const auto scan_t0 = std::chrono::steady_clock::now();
            std::unique_ptr<ScanResult> r(new ScanResult);
            try
            {
                if (pg && !ro) gDirUtilp->deleteFilesInDir(d, "*" + kExt);
                if (!ro) gDirUtilp->deleteFilesInDir(d, "*" + kExt + kPartExt);
                std::string name;
                LLDirIterator iter(d, "*" + kExt);
                while (iter.next(name))
                {
                    const std::string path = gDirUtilp->add(d, name);
                    LLClusterKey k;
                    U32 ver = 0;
                    if (!parseName(name, k, ver) || ver != kBuilderVersion)
                    {
                        // Another builder version (or not ours): its key can never be asked
                        // for again -- free the space.
                        if (!ro) { LLFile::remove(path, ENOENT); ++r->stale; }
                        continue;
                    }
                    llstat s;
                    if (LLFile::stat(path, &s) != 0) continue;
                    if ((U64)s.st_size <= sizeof(ClusterFileHeader))
                    {
                        if (!ro) LLFile::remove(path, ENOENT);
                        continue;
                    }
                    r->found.push_back({k, (U64)s.st_size, (std::time_t)s.st_mtime});
                }
            }
            catch (...) {}
            r->usec = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - scan_t0).count();
            return r;
        };
        Impl* self = this;
        const bool posted = post([self, scan]()
        {
            std::unique_ptr<ScanResult> r = scan();
            std::lock_guard<std::mutex> lk(self->done_mutex);
            self->scan_done = std::move(r);
        });
        if (!posted)
        {
            std::unique_ptr<ScanResult> r = scan();
            std::lock_guard<std::mutex> lk(done_mutex);
            scan_done = std::move(r);
        }
        LL_INFOS("VKClusterCache") << "Cluster LOD cache: folder " << dir << ", cap " << (cap >> 20) << " MB"
                                   << (purge ? ", purging" : "") << (read_only ? ", read-only (second instance)" : "")
                                   << "; indexing on a worker" << LL_ENDL;
        return true;
    }
};

LLCleanClusterCache::LLCleanClusterCache() : mImpl(new Impl) {}
LLCleanClusterCache::~LLCleanClusterCache() = default;

LLCleanClusterCache& LLCleanClusterCache::instance()
{
    static LLCleanClusterCache s;
    return s;
}

// static
std::string LLCleanClusterCache::defaultDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "clusterlod");
}

void LLCleanClusterCache::init(const std::string& dir, U64 cap_bytes, bool purge, bool read_only, LLCache::Store* store)
{
    if (store)
    {
        // #715: records in the cache store; the old folder is left to the migration (Clear
        // Cache: SLCacheStore empties the store, the caller deletes the folder).
        LLCleanClusterStore::init(store, cap_bytes, read_only);
        return;
    }
    LLCleanClusterStore::init(nullptr, 0, read_only);
    Impl& d = *mImpl;
    d.dir = dir;
    while (d.dir.size() > 1 && (d.dir.back() == '/' || d.dir.back() == '\\')) d.dir.pop_back();
    d.cap = cap_bytes;
    d.purge = purge;
    d.read_only = read_only;
    d.configured = !d.dir.empty();
    // Clear Cache is honoured now, toggle or not (a user request; no folder is created).
    if (purge && !read_only && d.configured && LLFile::isdir(d.dir))
    {
        gDirUtilp->deleteFilesInDir(d.dir, "*" + kExt);
        gDirUtilp->deleteFilesInDir(d.dir, "*" + kExt + kPartExt);
        d.purge = false;
    }
    // Nothing else: the pool / folder / scan start on first use (ensureStarted).
}

void LLCleanClusterCache::shutdown()
{
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::shutdown(); return; }
    Impl& d = *mImpl;
    if (d.started && !d.start_failed) logStats("shutdown");
    if (d.pool)
    {
        d.pool->close();   // joins the worker; queued jobs are dropped (a partial file is a *.part)
        d.pool.reset();
    }
    d.metas.clear();
    d.pages_ready.clear();
    d.writing.clear();
    d.dropped_while_writing.clear();
    d.meta_inflight = d.pages_inflight = 0;
    d.write_bytes_inflight = 0;
    d.configured = false;
}

void LLCleanClusterCache::setCapBytes(U64 cap_bytes)
{
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::setCapBytes(cap_bytes); return; }
    Impl& d = *mImpl;
    if (cap_bytes == d.cap) return;
    d.cap = cap_bytes;
    // 0 = no disk cache from now on (existing files are left for a later session's cap).
    if (d.started && d.scanned) d.evict(nullptr);
}

bool LLCleanClusterCache::ready() const
{
    if (LLCleanClusterStore::active()) return LLCleanClusterStore::ready();
    const Impl& d = *mImpl;
    return d.enabled() && d.started && d.scanned;
}

LLCleanClusterCache::State LLCleanClusterCache::requestMeta(const LLClusterKey& key,
                                                           std::shared_ptr<const LLClusterMeshData>& out)
{
    if (LLCleanClusterStore::active()) return LLCleanClusterStore::requestMeta(key, out);
    Impl& d = *mImpl;
    out.reset();
    if (!d.enabled())
    {
        ++d.st.meta_misses;
        return State::Missing;
    }
    if (!d.ensureStarted())
    {
        ++d.st.meta_misses;
        return State::Missing;
    }
    if (!d.scanned) return State::Loading;
    auto ms = d.metas.find(key);
    if (ms != d.metas.end())
    {
        const State s = ms->second.state;
        if (s == State::Ready)
        {
            out = ms->second.meta;
            d.metas.erase(ms);
            ++d.st.meta_hits;
            return State::Ready;
        }
        if (s == State::Failed)
        {
            d.metas.erase(ms);
            return State::Failed;
        }
        return State::Loading;
    }
    auto it = d.index.find(key);
    if (it == d.index.end())
    {
        ++d.st.meta_misses;
        LLCacheMetrics::recordRead(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, false, 0, 0);
        return State::Missing;
    }
    if (d.meta_inflight >= kMaxMetaInFlight) return State::Loading;   // (no slot: asked again)
    const std::string path = d.pathOf(key);
    Impl* self = &d;
    const auto t0 = std::chrono::steady_clock::now();
    const bool posted = d.post([self, key, path, t0]()
    {
        Impl::MetaDone r;
        r.key = key;
        try { r.meta = runReadMeta(key, path, r.why); }
        catch (...) { r.meta.reset(); r.why = "exception"; }
        r.usec = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(self->done_mutex);
        self->meta_done.push_back(std::move(r));
    });
    if (!posted) return State::Loading;
    Impl::MetaSlot& slot = d.metas[key];
    slot.state = State::Loading;
    slot.at = nowSec();
    ++d.meta_inflight;
    d.touch(key);
    return State::Loading;
}

bool LLCleanClusterCache::requestPages(const LLClusterKey& key, const U32* pages, U32 n, U64 ticket)
{
    if (LLCleanClusterStore::active()) return LLCleanClusterStore::requestPages(key, pages, n, ticket);
    Impl& d = *mImpl;
    if (!pages || n == 0 || !d.enabled() || !d.ensureStarted() || !d.scanned) return false;
    auto it = d.index.find(key);
    if (it == d.index.end() || it->second.header_hash == 0) return false;   // metadata first
    if (d.pages_inflight + (U32)d.pages_ready.size() + n > kMaxPageReads) return false;
    std::vector<U32> list(pages, pages + n);
    const std::string path = d.pathOf(key);
    const U32 hh = it->second.header_hash;
    Impl* self = &d;
    const auto t0 = std::chrono::steady_clock::now();
    const bool posted = d.post([self, key, path, hh, list, ticket, t0]()
    {
        Impl::PagesDone r;
        r.key = key;
        try { runReadPages(key, path, hh, list, ticket, r.reads, r.corrupt); }
        catch (...)
        {
            r.reads.clear();
            for (U32 p : list) { PageRead pr; pr.key = key; pr.page = p; pr.ticket = ticket; r.reads.push_back(std::move(pr)); }
        }
        r.usec = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(self->done_mutex);
        self->pages_done.push_back(std::move(r));
    });
    if (!posted) return false;
    d.pages_inflight += n;
    d.touch(key);
    return true;
}

LLCleanClusterCache::PageSource LLCleanClusterCache::pageSource(const LLClusterKey& key, U32 header_hash)
{
    if (LLCleanClusterStore::active()) return LLCleanClusterStore::pageSource(key, header_hash);
    Impl& d = *mImpl;
    if (!d.enabled() || !d.ensureStarted()) return PageSource::None;
    if (!d.scanned || d.writing.count(key)) return PageSource::Pending;
    auto it = d.index.find(key);
    if (it == d.index.end() || it->second.header_hash == 0 || it->second.header_hash != header_hash)
        return PageSource::None;
    return d.pages_inflight + (U32)d.pages_ready.size() >= kMaxPageReads ? PageSource::Full : PageSource::Ready;
}

void LLCleanClusterCache::takePages(std::vector<PageRead>& out)
{
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::takePages(out); return; }
    out.clear();
    out.swap(mImpl->pages_ready);
}

void LLCleanClusterCache::store(const std::shared_ptr<const LLClusterMeshData>& mesh)
{
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::store(mesh); return; }
    Impl& d = *mImpl;
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
    if (bytes > d.cap || bytes > kMaxFileBytes || d.writing.count(mesh->key)
        || (d.write_bytes_inflight && d.write_bytes_inflight + bytes > kMaxWriteBytes))
    {
        ++d.st.write_refused;
        return;
    }
    const LLClusterKey key = mesh->key;
    const std::string path = d.pathOf(key);
    Impl* self = &d;
    std::shared_ptr<const LLClusterMeshData> keep = mesh;   // immutable: no copy
    const auto t0 = std::chrono::steady_clock::now();
    const bool posted = d.post([self, key, path, keep, bytes, t0]()
    {
        Impl::WriteDone r;
        r.key = key;
        r.bytes = bytes;
        r.header_hash = keep->header.header_hash;
        try { r.ok = runWrite(path, *keep); }
        catch (...) { r.ok = false; }
        r.usec = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(self->done_mutex);
        self->write_done.push_back(std::move(r));
    });
    if (!posted) { ++d.st.write_refused; return; }
    d.writing.insert(key);
    d.write_bytes_inflight += bytes;
}

void LLCleanClusterCache::drop(const LLClusterKey& key)
{
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::drop(key); return; }
    Impl& d = *mImpl;
    d.metas.erase(key);
    if (!d.started || d.start_failed) return;
    if (d.writing.count(key)) d.dropped_while_writing.insert(key);
    d.removeFile(key);
}

void LLCleanClusterCache::poll()
{
    Impl& d = *mImpl;
    {
        static LLCachedControl<U32> s_cap_mb(gSavedSettings, "RenderVKClusterCacheMB", 2048);
        setCapBytes((U64)(U32)s_cap_mb << 20);
    }
    if (LLCleanClusterStore::active()) { LLCleanClusterStore::poll(); return; }
    if (!d.started || d.start_failed) return;
    std::unique_ptr<Impl::ScanResult> scan;
    std::deque<Impl::MetaDone> metas;
    std::deque<Impl::PagesDone> pages;
    std::deque<Impl::WriteDone> writes;
    {
        std::lock_guard<std::mutex> lk(d.done_mutex);
        if (!d.scanned && d.scan_done) scan = std::move(d.scan_done);
        metas.swap(d.meta_done);
        pages.swap(d.pages_done);
        // Writes that finish before the index exists wait for it (it would overwrite them).
        if (d.scanned || scan) writes.swap(d.write_done);
    }
    if (scan)
    {
        std::sort(scan->found.begin(), scan->found.end(),
                  [](const Impl::ScanResult::Found& a, const Impl::ScanResult::Found& b) { return a.t < b.t; });
        for (const Impl::ScanResult::Found& f : scan->found) d.addEntry(f.key, f.bytes, f.t, 0);
        d.st.stale_deleted += scan->stale;
        d.peak = d.used;
        d.scanned = true;
        d.evict(nullptr);
        LLCacheMetrics::recordStartup(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, scan->usec);
        logStats("startup");
    }
    for (Impl::MetaDone& r : metas)
    {
        if (d.meta_inflight) --d.meta_inflight;
        auto ms = d.metas.find(r.key);
        if (ms == d.metas.end()) continue;   // dropped meanwhile
        if (r.meta)
            LLCacheMetrics::recordRead(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, true, r.meta->metaBytes(), r.usec);
        else
        {
            LLCacheMetrics::recordRead(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, false, 0, r.usec);
            LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY);
        }
        if (r.meta)
        {
            auto it = d.index.find(r.key);
            if (it != d.index.end()) it->second.header_hash = r.meta->header.header_hash;
            ms->second.state = State::Ready;
            ms->second.meta = r.meta;
        }
        else
        {
            ++d.st.corrupt;
            LL_WARNS_ONCE("VKClusterCache") << "Discarding bad cluster LOD cache file " << r.key.fileName() << " ("
                                            << r.why << ")" << LL_ENDL;
            ms->second.state = State::Failed;
            ms->second.meta.reset();
            if (!d.writing.count(r.key)) d.removeFile(r.key);
        }
        ms->second.at = nowSec();
    }
    for (Impl::PagesDone& r : pages)
    {
        d.pages_inflight -= std::min<U32>(d.pages_inflight, (U32)r.reads.size());
        if (r.corrupt)
        {
            ++d.st.corrupt;
            LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY);
            LL_WARNS_ONCE("VKClusterCache") << "Discarding cluster LOD cache file with a bad page: " << r.key.fileName()
                                            << LL_ENDL;
            if (!d.writing.count(r.key)) d.removeFile(r.key);
            d.metas.erase(r.key);
        }
        for (PageRead& p : r.reads)
        {
            if (p.ok) { ++d.st.page_reads; d.st.page_read_bytes += p.bytes.size(); }
            LLCacheMetrics::recordRead(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, p.ok,
                                       p.ok ? p.bytes.size() : 0, (p.ok || r.corrupt) ? r.usec : 0);
            d.pages_ready.push_back(std::move(p));
        }
    }
    for (Impl::WriteDone& w : writes)
    {
        d.write_bytes_inflight -= std::min(d.write_bytes_inflight, w.bytes);
        d.writing.erase(w.key);
        if (d.dropped_while_writing.erase(w.key))
        {
            if (w.ok && !d.read_only) LLFile::remove(d.pathOf(w.key), ENOENT);
            continue;
        }
        LLCacheMetrics::recordWrite(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, w.bytes, w.ok ? w.bytes : 0,
                                    w.usec, w.ok);
        if (!w.ok) { ++d.st.write_fail; continue; }
        ++d.st.writes;
        d.st.write_bytes += w.bytes;
        // The streamer's metadata IS this mesh (the build), so its page reads may go straight
        // to the file: they are verified against this header hash.
        d.addEntry(w.key, w.bytes, std::time(nullptr), w.header_hash);
        d.evict(&w.key);
    }
    // Results nobody asked for again.
    if (!d.metas.empty())
    {
        const F64 t = nowSec();
        for (auto it = d.metas.begin(); it != d.metas.end();)
        {
            if (it->second.state != State::Loading && t - it->second.at > kMetaExpireSec) it = d.metas.erase(it);
            else ++it;
        }
    }
    // Page results the streamer did not take (it takes them every frame): bounded.
    if (d.pages_ready.size() > kMaxPageReads)
        d.pages_ready.erase(d.pages_ready.begin(), d.pages_ready.begin() + (d.pages_ready.size() - kMaxPageReads));
    // LLCacheMetrics footprint from the in-RAM index (no directory walk), every 60 s.
    if (d.scanned && nowSec() >= d.next_footprint)
    {
        d.next_footprint = nowSec() + 60.0;
        LLCacheMetrics::setFootprint(LLCacheMetrics::Cache::CLUSTER, LLCacheMetrics::Backend::LEGACY, d.used, d.used,
                                     d.index.size());
    }
}

LLCleanClusterCache::Stats LLCleanClusterCache::stats() const
{
    if (LLCleanClusterStore::active()) return LLCleanClusterStore::stats();
    const Impl& d = *mImpl;
    Stats s = d.st;
    s.disk_bytes = d.used;
    s.disk_cap = d.cap;
    s.disk_peak = d.peak;
    s.files = d.index.size();
    s.pages_inflight = d.pages_inflight;
    s.meta_inflight = d.meta_inflight;
    s.write_bytes_inflight = d.write_bytes_inflight;
    s.started = d.started && !d.start_failed;
    s.scanned = d.scanned;
    s.enabled = d.enabled();
    s.read_only = d.read_only;
    return s;
}

std::string LLCleanClusterCache::statsLine() const
{
    const Stats s = stats();
    if (!s.enabled) return "cache off";
    if (!s.started) return "cache idle";
    if (!s.scanned) return "cache indexing";
    return llformat("cache %llu/%llu MB (peak %llu) %llu files, meta hit %llu miss %llu, pages rd %llu (%llu MB) fly %llu, "
                    "wr %llu (fail %llu refused %llu low-lod %llu), evict %llu bad %llu stale %llu",
                    (unsigned long long)(s.disk_bytes >> 20), (unsigned long long)(s.disk_cap >> 20),
                    (unsigned long long)(s.disk_peak >> 20), (unsigned long long)s.files,
                    (unsigned long long)s.meta_hits, (unsigned long long)s.meta_misses,
                    (unsigned long long)s.page_reads, (unsigned long long)(s.page_read_bytes >> 20),
                    (unsigned long long)s.pages_inflight, (unsigned long long)s.writes,
                    (unsigned long long)s.write_fail, (unsigned long long)s.write_refused,
                    (unsigned long long)s.not_persisted, (unsigned long long)s.evicted,
                    (unsigned long long)s.corrupt, (unsigned long long)s.stale_deleted)
         + (s.read_only ? " (read-only)" : "");
}

void LLCleanClusterCache::logStats(const char* when) const
{
    LL_INFOS("VKClusterCache") << "Cluster LOD cache (" << when << "): " << statsLine()
                               << (LLCleanClusterStore::active() ? ", cache store " + LLCleanClusterStore::dir()
                                                                 : ", folder " + mImpl->dir)
                               << LL_ENDL;
}
