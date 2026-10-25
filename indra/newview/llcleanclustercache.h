/**
 * @file llcleanclustercache.h
 * @brief Cluster LOD (#642 / #744, OWNER: 1a): the on-disk cache of built cluster meshes
 *        (one .clod file per mesh asset + face + builder version), and the CPU-side
 *        container (LLClusterMeshData) the builder produces and the streamer consumes.
 *
 * File format: llcleanclusterformat.h "ON-DISK FORMAT". Pattern of the GPU texture cache
 * (#274, llgputexcache.h): <cache>/clusterlod/, writes to <name>.part + rename, *.part
 * deleted at startup, every read verified (magic, versions, header hash, sizes, page hash;
 * a mismatch deletes the file and reports a miss), a byte budget (RenderVKClusterCacheMB)
 * with LRU by file mtime, bounded reads / writes in flight, disk I/O on the cache's own
 * worker pool, results merged on the main thread by poll().
 *
 * RAM: the streamer keeps only the METADATA of a registered mesh (header + groups + page
 * directory + deps, a few KB); page payloads are read on demand (requestPages) and freed once
 * uploaded. A freshly built mesh carries its payload until store() has written it.
 *
 * THREADING: every public call is render-thread only unless noted.
 *
 * 1a notes: a mesh built from a creator LOD below the highest (header.source_lod < 3) is
 * served to the streamer but NOT written (a later session builds it from the full LOD);
 * files of another builder version are deleted by the startup scan; a page read verifies the
 * file still carries the header the metadata came from (header_hash), so a rebuilt file
 * never mixes with old metadata.
 */
#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "llcleanclusterformat.h"

#include <memory>
#include <string>
#include <vector>

namespace LLCache { class Store; }

// A cluster mesh = one face of one mesh asset (the DAG is built from its highest LOD).
struct LLClusterKey
{
    LLUUID mesh_id;
    S32    face = 0;

    bool operator==(const LLClusterKey& o) const { return mesh_id == o.mesh_id && face == o.face; }
    bool operator!=(const LLClusterKey& o) const { return !(*this == o); }
    // 64-bit hash (map key; low 32 bits go into ClusterPageHeader::mesh_key).
    U64 hash() const;
    // "<uuid>_f<face>_b<builder version>.clod"
    std::string fileName() const;
};
struct LLClusterKeyHash
{
    size_t operator()(const LLClusterKey& k) const { return (size_t)k.hash(); }
};

// One built (or loaded) cluster mesh. Immutable once published (shared_ptr<const>).
struct LLClusterMeshData
{
    LLClusterKey key;
    LLClusterFmt::ClusterFileHeader header{};
    std::vector<LLClusterFmt::ClusterGroup>   groups;   // header.group_count (incl. sentinels 0, 1)
    std::vector<LLClusterFmt::ClusterPageDir> pages;    // header.page_count, coarse -> fine
    std::vector<U32>                          deps;     // parent page lists (ClusterPageDir::dep_first)
    // Page payloads, laid out as on disk (pages[i].offset .. + stored_bytes). EMPTY when only
    // the metadata was loaded from the cache: pages then come from requestPages().
    std::vector<U8>                           payload;

    bool hasPayload() const { return !payload.empty(); }
    // 1a: fill the header's file-layout fields (groups_off .. payload_bytes, deps_count) from
    // the vectors and compute header_hash. Called by the builder once the mesh is complete;
    // the on-disk file is then header | groups | pagedir | deps | payload, byte for byte.
    void finalizeLayout();
    // 1a: header self-consistency (magic, versions, sizes, hash, layout offsets vs the vector
    // sizes when they are loaded). Any thread.
    bool checkHeader(std::string* why) const;
    U64  metaBytes() const;      // RAM held without the payload
    U64  totalBytes() const;     // + payload
    // Uncompressed page bytes of page i into dst (kPageBytes, zero tail). False on a bad
    // page (hash / size): the caller treats the page as unavailable (never crashes).
    bool copyPage(U32 page, U8* dst) const;
};

class LLCleanClusterCache
{
public:
    static LLCleanClusterCache& instance();

    // Main thread. dir = <cache>/clusterlod; cap = RenderVKClusterCacheMB << 20.
    // 1a: init only RECORDS the configuration: the folder, the worker pool and the index
    // scan start on first use (requestMeta / requestPages / store / drop from the streamer,
    // i.e. only once RenderVKClusterLOD is on), so with the toggle off there is no thread and
    // no disk I/O. purge = Clear Cache (every .clod deleted at the scan); read_only = a
    // second viewer instance (no writes, no deletes). cap 0 = no disk cache (every lookup
    // misses, store() is a no-op).
    // #715: store = the open cache store (CacheStoreCluster on): every call then forwards to
    // the store backend (llcleanclustercachestore.h) and `dir` is only the migration source.
    void init(const std::string& dir, U64 cap_bytes, bool purge = false, bool read_only = false,
              LLCache::Store* store = nullptr);
    static std::string defaultDir();         // <cache>/clusterlod
    void shutdown();                         // drains the workers
    void setCapBytes(U64 cap_bytes);         // live setting change (evicts down to it)
    bool ready() const;                      // startup scan merged

    enum class State : U8
    {
        Unknown = 0,   // never asked
        Missing,       // no valid file: build it (LLCleanClusterBuilder)
        Loading,       // metadata read in flight
        Ready,         // metadata available (out)
        Failed,        // unreadable / corrupt (deleted): treat as Missing next time
    };
    // Metadata (no payload) of `key`; starts the read on first ask. Cheap when known.
    State requestMeta(const LLClusterKey& key, std::shared_ptr<const LLClusterMeshData>& out);

    // Asynchronous page reads (uncompressed kPageBytes each into the result). `ticket` is the
    // caller's id, echoed back. False = refused now (bounded in-flight count / bytes): retry.
    struct PageRead
    {
        LLClusterKey key;
        U32  page = 0;
        U64  ticket = 0;
        bool ok = false;               // false: missing / corrupt (the file is dropped)
        std::vector<U8> bytes;         // kPageBytes when ok
    };
    bool requestPages(const LLClusterKey& key, const U32* pages, U32 n, U64 ticket);
    // Can page reads of `key` be served from disk for the metadata with `header_hash`?
    //   Ready   the file holds that build and a read can be posted now;
    //   Full    the file holds it, the bounded read slots are taken (retry next frame);
    //   Pending not known yet (index scan running, or its write in flight);
    //   None    never from disk: not persisted (a lower creator LOD build), evicted / dropped,
    //           another build's file, or no cache. The caller must keep / rebuild the payload.
    enum class PageSource { Ready = 0, Full, Pending, None };
    PageSource pageSource(const LLClusterKey& key, U32 header_hash);
    void takePages(std::vector<PageRead>& out);   // finished reads since the last call

    // Write a freshly built mesh (with payload) to disk, asynchronously.
    void store(const std::shared_ptr<const LLClusterMeshData>& mesh);
    // Forget a key (e.g. its source asset changed): deletes the file.
    void drop(const LLClusterKey& key);

    // Main thread, once per frame: merge worker results, evict to the cap.
    void poll();

    struct Stats
    {
        U64 disk_bytes = 0, disk_cap = 0, files = 0;
        U64 meta_hits = 0, meta_misses = 0, corrupt = 0;
        U64 page_reads = 0, page_read_bytes = 0, pages_inflight = 0;
        U64 writes = 0, write_bytes = 0, evicted = 0;
        // 1a extras
        U64 disk_peak = 0, write_fail = 0, write_refused = 0, not_persisted = 0, stale_deleted = 0;
        U64 meta_inflight = 0, write_bytes_inflight = 0;
        bool started = false, scanned = false, enabled = false, read_only = false;
    };
    Stats stats() const;
    std::string statsLine() const;

    void logStats(const char* when) const;

private:
    LLCleanClusterCache();
    ~LLCleanClusterCache();
    LLCleanClusterCache(const LLCleanClusterCache&) = delete;
    LLCleanClusterCache& operator=(const LLCleanClusterCache&) = delete;
    struct Impl;                       // index, LRU, worker pool, in-flight bookkeeping (.cpp)
    std::unique_ptr<Impl> mImpl;
};
