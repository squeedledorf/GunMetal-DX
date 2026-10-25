/**
 * @file slassetmigrate.cpp
 * @brief Old asset folder -> segment store (#714, #715). See slassetmigrate.h.
 *
 * No viewer precompiled header (SKIP_PRECOMPILE_HEADERS): llcommon + llcachestore only.
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

#include "slassetmigrate.h"

#include "fsyspath.h"
#include "llsd.h"
#include "llsdserialize.h"
#include "llcachecodec.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    const std::string PREFIX("sl_cache_");
    const std::string SUFFIX("_0.asset");
    constexpr U8 ASSET_META_VERSION = 1;        // = LLFileSystemStore::META_VERSION
    constexpr U8 ASSET_TYPE_UNKNOWN = 0xFF;     // (U8)(S8)LLAssetType::AT_NONE: the old name has no type
    constexpr S64 MAX_MESH_ASSET_BYTES = 64 * 1024 * 1024;   // LLMeshHeader::MAX_MESH_ASSET_BYTES
    constexpr S32 MAX_HEADER_BYTES = 1 << 20;

    U32 rdU32(const U8* p)
    {
        return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24);
    }

    // LLMeshHeader::sanitizeBlock
    void sanitize(S32& offset, S32& size)
    {
        if (offset < 0 || size < 0 || (S64)offset + (S64)size > MAX_MESH_ASSET_BYTES)
        {
            offset = 0;
            size = 0;
        }
    }
}

namespace SLMeshParts
{

U32 partFlag(Part p)
{
    switch (p)
    {
    case LOD0: case LOD1: case LOD2: case LOD3: return 1u << ((U32)p - (U32)LOD0);
    case SKIN:        return 1u << 4;
    case PHYS_CONVEX: return 1u << 5;
    case PHYS_MESH:   return 1u << 6;
    default:          return 0;
    }
}

const char* partName(Part p)
{
    switch (p)
    {
    case HEADER:      return "header";
    case LOD0:        return "lod0";
    case LOD1:        return "lod1";
    case LOD2:        return "lod2";
    case LOD3:        return "lod3";
    case SKIN:        return "skin";
    case PHYS_CONVEX: return "physics_convex";
    case PHYS_MESH:   return "physics_mesh";
    default:          return "?";
    }
}

bool splitOldMeshFile(const LLUUID& id, const U8* data, size_t size, Split& out)
{
    out = Split();
    if (!data || size <= (size_t)PREAMBLE_SIZE) return false;
    const U32 version = rdU32(data);
    const S32 header_size = (S32)rdU32(data + 4);
    const U32 flags = rdU32(data + 8);
    if (version != PREAMBLE_VERSION || header_size <= 0 || header_size > MAX_HEADER_BYTES
        || (size_t)PREAMBLE_SIZE + (size_t)header_size > size || (flags & ~0x7Fu) != 0)
    {
        return false;
    }
    const U8* body = data + PREAMBLE_SIZE;
    const size_t body_size = size - PREAMBLE_SIZE;

    // The header LLSD, parsed the way LLMeshRepoThread::headerReceived does.
    LLSD header;
    {
        std::string text((const char*)body, (size_t)header_size);
        llssize n = (llssize)text.size();
        char* p = strip_deprecated_header(text.data(), n);
        std::istringstream in(std::string(p, (size_t)n));
        if (LLSDSerialize::fromBinary(header, in, n) <= 0 || !header.isMap()) return false;
    }
    static const char* lod_names[4] = { "lowest_lod", "low_lod", "medium_lod", "high_lod" };
    bool any_block = false;
    for (const char* k : lod_names) any_block = any_block || header.has(k);
    if (!any_block && !header.has("404")) return false;   // a map, but not a mesh header

    SLCacheMigrate::Record hrec;
    hrec.key = key(id, HEADER);
    hrec.payload.assign(body, body + header_size);
    out.records.push_back(std::move(hrec));
    out.header_size = header_size;

    struct Block { Part part; const char* name; };
    static const Block blocks[] = {
        { LOD0, "lowest_lod" }, { LOD1, "low_lod" }, { LOD2, "medium_lod" }, { LOD3, "high_lod" },
        { SKIN, "skin" }, { PHYS_CONVEX, "physics_convex" }, { PHYS_MESH, "physics_mesh" },
    };
    for (const Block& b : blocks)
    {
        if (!(flags & partFlag(b.part))) continue;
        S32 offset = header[b.name]["offset"].asInteger();
        S32 bsize = header[b.name]["size"].asInteger();
        sanitize(offset, bsize);
        if (bsize <= 0) continue;
        const U64 at = (U64)header_size + (U64)offset;
        if (at + (U64)bsize > body_size) continue;   // reserved past the end: never written
        const U8* src = body + at;
        bool zero = true;
        for (S32 i = 0; i < std::min<S32>(bsize, 1024) && zero; ++i) zero = src[i] == 0;
        if (zero) continue;                          // reserved but not written
        SLCacheMigrate::Record r;
        r.key = key(id, b.part);
        r.payload.assign(src, src + bsize);
        out.records.push_back(std::move(r));
        out.flags |= partFlag(b.part);
    }
    return true;
}

} // namespace SLMeshParts

namespace SLAssetMigrate
{

bool parseName(const std::string& name, LLUUID& id)
{
    const size_t ul = UUID_STR_LENGTH - 1;
    if (name.size() != PREFIX.size() + ul + SUFFIX.size()) return false;
    if (name.compare(0, PREFIX.size(), PREFIX) != 0) return false;
    if (name.compare(name.size() - SUFFIX.size(), SUFFIX.size(), SUFFIX) != 0) return false;
    const std::string s = name.substr(PREFIX.size(), ul);
    if (!LLUUID::validate(s)) return false;
    id = LLUUID(s);
    return makeName(id) == name;
}

std::string makeName(const LLUUID& id)
{
    return PREFIX + id.asString() + SUFFIX;
}

bool Converter::matches(const std::string& name) const
{
    LLUUID id;
    return parseName(name, id);
}

SLCacheMigrate::Verdict Converter::precheck(const std::string& name, const LLCache::Store& store) const
{
    LLUUID id;
    if (!parseName(name, id)) return SLCacheMigrate::Verdict::BAD;
    // Fetched (or imported on demand) this session, under either layout: the store's copy stands.
    if (store.exists(LLCache::Key(id, LLCache::Kind::ASSET, 0)) || store.exists(SLMeshParts::key(id, SLMeshParts::HEADER)))
        return SLCacheMigrate::Verdict::DONE;
    return SLCacheMigrate::Verdict::CONVERT;
}

SLCacheMigrate::Verdict Converter::convert(const std::string& path, const std::string& name,
                                          std::vector<SLCacheMigrate::Record>& out) const
{
    out.clear();
    LLUUID id;
    if (!parseName(name, id)) return SLCacheMigrate::Verdict::BAD;
    std::error_code ec;
    const U64 size = (U64)fs::file_size(fsyspath(path), ec);
    if (ec) return SLCacheMigrate::Verdict::IO_ERROR;
    if (size == 0) return SLCacheMigrate::Verdict::BAD;          // read as absent by the old cache too
    if (size > LLCacheCodec::getMaxRecordSize()) return SLCacheMigrate::Verdict::BAD;
    std::vector<U8> bytes;
    try { bytes.resize((size_t)size); }
    catch (const std::bad_alloc&) { return SLCacheMigrate::Verdict::IO_ERROR; }
    {
        std::ifstream in(static_cast<const std::filesystem::path&>(fsyspath(path)), std::ios::binary);
        if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size()))
            return SLCacheMigrate::Verdict::IO_ERROR;
    }
    SLMeshParts::Split split;
    if (SLMeshParts::splitOldMeshFile(id, bytes.data(), bytes.size(), split))
    {
        out = std::move(split.records);
        return SLCacheMigrate::Verdict::OK;
    }
    SLCacheMigrate::Record r;
    r.key = LLCache::Key(id, LLCache::Kind::ASSET, 0);
    r.meta[0] = ASSET_META_VERSION;
    r.meta[1] = ASSET_TYPE_UNKNOWN;
    r.meta_len = 2;
    r.payload = std::move(bytes);
    out.push_back(std::move(r));
    return SLCacheMigrate::Verdict::OK;
}

} // namespace SLAssetMigrate
