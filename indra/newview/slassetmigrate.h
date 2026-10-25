/**
 * @file slassetmigrate.h
 * @brief The asset cache in the segment store (#715 / #714): the mesh part layout and the
 *        converter of the old asset folder (<cache>/cache/sl_cache_<uuid>_0.asset files).
 *
 * Pure (llcommon + llcachestore), no viewer precompiled header: unit-tested on a temp folder
 * (tests/slassetmigrate_test.cpp) and used by llmeshdiskcache.cpp for an on-demand import.
 *
 * MESH LAYOUT (why per part): the old cache kept a mesh as ONE sparse file -- a 12-byte
 * preamble {version 1, header size, cached-part flags}, the header, then each LOD / skin /
 * physics block patched in place at 12 + header size + its offset as it arrived (READ_WRITE
 * + setSize; "first KB all zero = not written"). An append-only store cannot patch: one
 * record per mesh would re-append the whole asset on every block arrival and store the
 * zero gaps. So a mesh is one MESH record per part, variant = Part: the header (exactly its
 * header-size bytes) and each block that arrived. Presence of a part record replaces the
 * preamble flags; each arrival is one append; a read is one whole-record read of one part.
 *
 * OTHER ASSETS: one ASSET record per file (key = UUID; the old name carries no type, so the
 * meta type is AT_NONE for imported files).
 *
 * WHAT IS MIGRATED: only files named sl_cache_<uuid>_0.asset in the asset folder
 * (DiskCacheDirName). Nothing else in <cache> is touched; app_settings, skins and the
 * read-only install tree are never looked at.
 */

#ifndef SL_SLASSETMIGRATE_H
#define SL_SLASSETMIGRATE_H

#include "slcachemigrate.h"

#include "lluuid.h"

namespace SLMeshParts
{
    // MESH record variants. Values are on disk: never renumber.
    enum Part : U32
    {
        HEADER      = 0,
        LOD0        = 1,   // LOD0..LOD3 = 1..4 (lowest .. high)
        LOD1        = 2,
        LOD2        = 3,
        LOD3        = 4,
        SKIN        = 5,
        PHYS_CONVEX = 6,
        PHYS_MESH   = 7,
        PART_COUNT  = 8
    };
    inline Part lodPart(S32 lod) { return (Part)(LOD0 + lod); }
    // The part's bit in LLMeshHeader::getFlags() (the old preamble flags): LOD i = 1 << i,
    // skin 1 << 4, physics convex 1 << 5, physics mesh 1 << 6.
    U32 partFlag(Part p);
    const char* partName(Part p);
    inline LLCache::Key key(const LLUUID& id, Part p) { return LLCache::Key(id, LLCache::Kind::MESH, (U32)p); }

    constexpr U32 PREAMBLE_VERSION = 1;
    constexpr S32 PREAMBLE_SIZE = 12;   // version, header size, flags (U32 each)

    struct Split
    {
        S32 header_size = 0;
        U32 flags = 0;                    // preamble flags of the parts actually kept
        std::vector<SLCacheMigrate::Record> records;   // HEADER first, then the cached blocks
    };
    // Split an old mesh cache file (preamble + header + blocks) into part records. False =
    // not a mesh cache file (no valid preamble / header LLSD). A block is kept only when the
    // preamble flags it cached, it lies inside the file and its first KB is not all zero (a
    // reserved but never written block of a crashed session).
    bool splitOldMeshFile(const LLUUID& id, const U8* data, size_t size, Split& out);
}

namespace SLAssetMigrate
{
    // "sl_cache_<uuid>_0.asset" -> uuid.
    bool parseName(const std::string& name, LLUUID& id);
    std::string makeName(const LLUUID& id);

    // The old asset folder -> ASSET / MESH records.
    class Converter : public SLCacheMigrate::Converter
    {
    public:
        const char* label() const override { return "Asset"; }
        bool matches(const std::string& name) const override;
        SLCacheMigrate::Verdict precheck(const std::string& name, const LLCache::Store& store) const override;
        SLCacheMigrate::Verdict convert(const std::string& path, const std::string& name,
                                        std::vector<SLCacheMigrate::Record>& out) const override;
        LLCache::Group group() const override { return LLCache::Group::ASSETS; }
    };
}

#endif // SL_SLASSETMIGRATE_H
