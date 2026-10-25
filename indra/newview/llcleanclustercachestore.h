/**
 * @file llcleanclustercachestore.h
 * @brief Cluster LOD cache on the segment store (#715, #744). Private to the cluster cache:
 *        LLCleanClusterCache's public calls forward here while it is active (CacheStoreCluster
 *        on and the store open), exactly as LLGpuTexCache forwards to llgputexcachestore.
 *
 * RECORDS: one CLUSTER record per (mesh UUID, face): Key(mesh_id, CLUSTER, face), the .clod
 * container byte for byte (header | groups | page directory | deps | payload), stored raw
 * (CLUSTER is a ranged-read kind): metadata = readRange(0, payload_off), a page =
 * readRange(payload_off + offset, stored_bytes). Every read is verified exactly as a file
 * read (LLClusterCacheIO: header hash + layout + builder validate; per page FNV hashes).
 *
 * VERSIONING: the record meta carries kFileVersion / kFormatVersion / kBuilderVersion and the
 * header hash. A record of another version is never served: its lookup removes it (the old
 * .clod scan deleted files of another builder version). Not the store's per-kind
 * format_version: the store's index loads on its writer as soon as it opens, before any
 * consumer could set a policy, so a policy bump would race the load. A page read also checks
 * that the record still carries the header hash the metadata came from, so a rebuilt record
 * never mixes with old metadata.
 *
 * BUDGET: Group::CLUSTER = RenderVKClusterCacheMB, evicted by the store. A second instance
 * (read-only store) writes nothing, as the file cache did.
 *
 * THREADING: render thread, like LLCleanClusterCache; reads on the cache's one worker thread
 * (started on first use: no thread while RenderVKClusterLOD is off), writes on the store's.
 */
#pragma once

#include "llcleanclustercache.h"
#include "llcachestore.h"
#include "slcachemigrate.h"

#include <memory>
#include <string>
#include <vector>

// Verification shared by the file and store backends (llcleanclustercache.cpp).
namespace LLClusterCacheIO
{
    // Random access to one container (a .clod file or a CLUSTER record). Any thread.
    struct Reader
    {
        virtual ~Reader() = default;
        virtual U64  size() = 0;
        virtual bool readAt(U64 off, void* dst, size_t n) = 0;
    };
    bool parseName(const std::string& name, LLClusterKey& key, U32& ver);
    bool readHeader(Reader& r, const LLClusterKey& key, LLClusterFmt::ClusterFileHeader& h, std::string& why);
    std::shared_ptr<LLClusterMeshData> readMeta(Reader& r, const LLClusterKey& key, std::string& why);
    void readPages(Reader& r, const LLClusterKey& key, U32 expect_hash, const std::vector<U32>& pages, U64 ticket,
                   std::vector<LLCleanClusterCache::PageRead>& out, bool& corrupt);
    bool verifyPayload(Reader& r, const LLClusterMeshData& meta);   // every page hash
    bool serialize(const LLClusterMeshData& m, std::vector<U8>& out);
}

namespace LLCleanClusterStore
{
    constexpr U8 META_LEN = 24;   // {1, 0,0,0, file ver, format ver, builder ver, header hash, payload_off}
    LLCache::Key key(const LLClusterKey& k);
    U8   packMeta(const LLClusterFmt::ClusterFileHeader& h, U8* meta);
    // Current versions? (header_hash / payload_off out)
    bool metaCurrent(const LLCache::RecordInfo& ri, U32* header_hash = nullptr, U32* payload_off = nullptr);

    bool active();
    void init(LLCache::Store* store, U64 cap_bytes, bool read_only);
    void shutdown();
    void setCapBytes(U64 cap_bytes);
    bool ready();
    LLCleanClusterCache::State requestMeta(const LLClusterKey& key, std::shared_ptr<const LLClusterMeshData>& out);
    bool requestPages(const LLClusterKey& key, const U32* pages, U32 n, U64 ticket);
    LLCleanClusterCache::PageSource pageSource(const LLClusterKey& key, U32 header_hash);
    void takePages(std::vector<LLCleanClusterCache::PageRead>& out);
    void store(const std::shared_ptr<const LLClusterMeshData>& mesh);
    void drop(const LLClusterKey& key);
    void poll();
    LLCleanClusterCache::Stats stats();
    const std::string& dir();

    // #714: <cache>/clusterlod/*.clod -> CLUSTER records (*.clod.part deleted, other builder
    // versions deleted, every file verified whole before it is imported).
    class Converter : public SLCacheMigrate::Converter
    {
    public:
        const char* label() const override { return "Cluster"; }
        bool matches(const std::string& name) const override;
        bool isLeftover(const std::string& name) const override;
        SLCacheMigrate::Verdict precheck(const std::string& name, const LLCache::Store& store) const override;
        SLCacheMigrate::Verdict convert(const std::string& path, const std::string& name,
                                        std::vector<SLCacheMigrate::Record>& out) const override;
        LLCache::Group group() const override { return LLCache::Group::CLUSTER; }
    };
}
