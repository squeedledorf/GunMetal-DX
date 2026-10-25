/**
 * @file llmeshdiskcache.h
 * @brief The mesh repository's disk cache, two backends (#715): the old sparse file per
 *        mesh (LLFileSystem, preamble + header + blocks patched in place) or MESH part
 *        records in the cache store (slassetmigrate.h "MESH LAYOUT" explains why per part).
 *        The backend follows the asset cache: the store when LLFileSystemStore is attached
 *        (CacheStoreAssets), the files otherwise. Every mesh cache read / write of the
 *        repository goes through here, so LLCacheMetrics counts Cache::MESH on both
 *        backends at the same points.
 *
 * Any thread (the mesh repo thread, the mesh thread pool, HTTP handlers).
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

#ifndef LL_LLMESHDISKCACHE_H
#define LL_LLMESHDISKCACHE_H

#include "slassetmigrate.h"

#include "lluuid.h"

#include <memory>
#include <vector>

class LLFileSystem;
class LLMeshHeader;

namespace LLMeshDiskCache
{
    using Part = SLMeshParts::Part;
    constexpr U32 CACHE_PREAMBLE_VERSION = SLMeshParts::PREAMBLE_VERSION;
    constexpr S32 CACHE_PREAMBLE_SIZE = SLMeshParts::PREAMBLE_SIZE;   // version, header_size, flags

    bool storeActive();

    // The cached header of `id`: out = the bytes after the preamble (old files: up to 8 KB,
    // possibly with blocks after the header) or the HEADER record; flags = cached parts
    // (LLMeshHeader::getFlags() bits). False = not cached (fetch it).
    bool readHeader(const LLUUID& id, std::vector<U8>& out, U32& flags);

    // Header receipt from the network: cache the header and every block wholly inside
    // `data` (the header's m*InCache flags, as headerReceived() set them).
    void writeHeader(const LLUUID& id, LLMeshHeader& header, const U8* data, S32 data_size, S32 reserve_bytes);

    // A cached block failed to parse: forget every block (the header stays).
    void markAllUncached(const LLUUID& id, S32 header_bytes, U32 flags);

    // One block read. disk_offset = CACHE_PREAMBLE_SIZE + header size + block offset (the
    // old file position); the store reads the part record whole. A request never served by
    // read() counts as a miss when the reader goes away.
    class BlockReader
    {
    public:
        BlockReader(const LLUUID& id, Part part, S32 disk_offset, S32 size);
        ~BlockReader();
        BlockReader(const BlockReader&) = delete;
        BlockReader& operator=(const BlockReader&) = delete;
        bool available();          // the block is on disk in full
        bool read(U8* dst);        // exactly `size` bytes; false (dst zeroed) on a short / bad read
    private:
        LLUUID mID;
        Part mPart;
        S32 mOffset, mSize;
        bool mStore;
        bool mDone = false;
        U64 mT0;
        std::unique_ptr<LLFileSystem> mFile;
    };

    // One block write after a network receipt.
    class BlockWriter
    {
    public:
        BlockWriter(const LLUUID& id, Part part, S32 disk_offset, S32 size);
        ~BlockWriter();
        BlockWriter(const BlockWriter&) = delete;
        BlockWriter& operator=(const BlockWriter&) = delete;
        bool reserved();                                  // the entry can take the block (its header is cached)
        bool write(const U8* data);                       // `size` bytes
        void markCached(S32 header_bytes, U32 flags);     // old files: rewrite the preamble flags
    private:
        LLUUID mID;
        Part mPart;
        S32 mOffset, mSize;
        bool mStore;
        std::unique_ptr<LLFileSystem> mFile;
    };
}

#endif // LL_LLMESHDISKCACHE_H
