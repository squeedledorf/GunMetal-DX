/**
 * @file llmeshdiskcache.cpp
 * @brief The mesh repository's disk cache, old files or store parts (#715). See
 *        llmeshdiskcache.h.
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

#include "llmeshdiskcache.h"

#include "llcachemetrics.h"
#include "llfilesystem.h"
#include "llfilesystemstore.h"
#include "llmeshrepository.h"

#include <chrono>

namespace
{
    using namespace LLCacheMetrics;
    using SLMeshParts::Part;

    U64 nowUsec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    U64 since(U64 t0)
    {
        const U64 t = nowUsec();
        return t > t0 ? t - t0 : 0;
    }

    // On-disk bytes of an uncompressed MESH record (96-byte header, 24-byte codec frame, pad 16).
    U64 recordDiskBytes(U64 payload)
    {
        return (96 + 24 + payload + 15) & ~15ull;
    }

    bool firstKbZero(const U8* p, S32 size)
    {
        for (S32 i = 0; i < llmin(size, 1024); ++i)
            if (p[i]) return false;
        return true;
    }

    // Old files: the preamble is the file's first bytes.
    void write_preamble(LLFileSystem& file, S32 header_bytes, S32 flags)
    {
        LLMeshRepository::sCacheBytesWritten += LLMeshDiskCache::CACHE_PREAMBLE_SIZE;
        file.seek(0, 0);    // callers may have written a block first
        file.write((U8*)&LLMeshDiskCache::CACHE_PREAMBLE_VERSION, sizeof(U32));
        file.write((U8*)&header_bytes, sizeof(U32));
        file.write((U8*)&flags, sizeof(U32));
    }

    // Store: one part record (placement = the agent's region; not pinned: the repository
    // parses the network bytes, nobody reads the record back this session).
    bool writePart(LLCache::Store* store, const LLUUID& id, Part part, const U8* data, S32 size)
    {
        if (!store || !data || size <= 0) return false;
        LLCache::WriteOptions o;
        o.placement = LLFileSystemStore::placement();
        auto done = std::make_shared<LLCache::Ticket<LLCache::Status>>();
        const U64 t0 = nowUsec();
        const U64 n = (U64)size;
        o.completion.wake = [done, t0, n]()
        {
            const LLCache::Status st = done->take();
            if (st == LLCache::Status::SKIPPED) return;   // superseded: not a write
            const bool ok = st == LLCache::Status::OK;
            recordWrite(Cache::MESH, Backend::STORE, n, ok ? recordDiskBytes(n) : 0, since(t0), ok);
        };
        return store->write(SLMeshParts::key(id, part), LLCache::WriteSource::copy(data, (size_t)size), o, done.get());
    }

    U32 presentFlags(LLCache::Store* store, const LLUUID& id)
    {
        U32 flags = 0;
        for (U32 p = SLMeshParts::LOD0; p < SLMeshParts::PART_COUNT; ++p)
            if (store->exists(SLMeshParts::key(id, (Part)p))) flags |= SLMeshParts::partFlag((Part)p);
        return flags;
    }

    // While the old folder still has unconverted files: split + import the old mesh file.
    bool importOldMesh(LLCache::Store* store, const LLUUID& id, SLMeshParts::Split& split)
    {
        if (!LLFileSystemStore::legacyFallback()) return false;
        const std::string path = LLFileSystemStore::legacyPath(id);
        LLFILE* fp = LLFile::fopen(path, "rb");
        if (!fp) return false;
        std::vector<U8> bytes;
        bool ok = false;
        if (fseek(fp, 0, SEEK_END) == 0)
        {
            const long n = ftell(fp);
            if (n > 0 && n <= (long)LLMeshHeader::MAX_MESH_ASSET_BYTES + (1 << 20) && fseek(fp, 0, SEEK_SET) == 0)
            {
                bytes.resize((size_t)n);
                ok = fread(bytes.data(), 1, bytes.size(), fp) == bytes.size();
            }
        }
        LLFile::close(fp);
        if (!ok || !SLMeshParts::splitOldMeshFile(id, bytes.data(), bytes.size(), split)) return false;
        for (SLCacheMigrate::Record& r : split.records)
        {
            LLCache::WriteOptions o;
            o.placement = LLFileSystemStore::placement();
            store->write(r.key, LLCache::WriteSource::copy(r.payload.data(), r.payload.size()), o);
        }
        // The migration finds the HEADER record and deletes the file.
        return true;
    }
}

namespace LLMeshDiskCache
{

bool storeActive()
{
    return LLFileSystemStore::active();
}

bool readHeader(const LLUUID& id, std::vector<U8>& out, U32& flags)
{
    out.clear();
    flags = 0;
    const U64 t0 = nowUsec();
    {
        LLFileSystemStore::Access store;
        if (store)
        {
            LLFileSystemStore::waitReady(store.get());
            LLCache::ReadResult r = store->readSync(SLMeshParts::key(id, SLMeshParts::HEADER));
            if (r.status == LLCache::Status::OK && !r.bytes.empty())
            {
                out = std::move(r.bytes);
                flags = presentFlags(store.get(), id);
                LLMeshRepository::sCacheBytesRead += (U32)out.size();
                ++LLMeshRepository::sCacheReads;
                recordRead(Cache::MESH, Backend::STORE, true, out.size(), since(t0));
                return true;
            }
            if (r.status == LLCache::Status::CORRUPT)
            {
                recordCorrupt(Cache::MESH, Backend::STORE);
            }
            SLMeshParts::Split split;
            if (importOldMesh(store.get(), id, split))
            {
                out = std::move(split.records.front().payload);
                flags = split.flags;
                recordRead(Cache::MESH, Backend::LEGACY, true, out.size(), since(t0));
                return true;
            }
            recordRead(Cache::MESH, Backend::STORE, false, 0, 0);
            return false;
        }
    }

    // Old file: preamble + header (+ whatever follows, up to 8 KB).
    LLFileSystem file(id, LLAssetType::AT_MESH);
    const S32 size = file.getSize();
    // A cache file too short to hold the preamble is ignored (fetched from the simulator).
    if (size > CACHE_PREAMBLE_SIZE)
    {
        // *NOTE: if the header size is ever more than 4KB, this will break
        constexpr S32 DISK_MINIMAL_READ = 4096;
        U8 buffer[DISK_MINIMAL_READ * 2];
        S32 bytes = llmin(size, DISK_MINIMAL_READ);
        LLMeshRepository::sCacheBytesRead += bytes;
        ++LLMeshRepository::sCacheReads;
        file.read(buffer, bytes);

        U32 version = 0;
        memcpy(&version, buffer, sizeof(U32));
        S32 header_size = 0;
        memcpy(&header_size, buffer + sizeof(U32), sizeof(S32));
        // The preamble's header size must fit the read buffer.
        const bool header_size_ok = header_size >= 0 && (S64)header_size + (S64)CACHE_PREAMBLE_SIZE <= (S64)sizeof(buffer);
        if (version == CACHE_PREAMBLE_VERSION && header_size_ok)
        {
            if (header_size + CACHE_PREAMBLE_SIZE > DISK_MINIMAL_READ && size > DISK_MINIMAL_READ)
            {
                bytes = llmin(size, DISK_MINIMAL_READ * 2);
                file.read(buffer + DISK_MINIMAL_READ, bytes - DISK_MINIMAL_READ);
            }
            memcpy(&flags, buffer + 2 * sizeof(U32), sizeof(U32));
            out.assign(buffer + CACHE_PREAMBLE_SIZE, buffer + bytes);
            recordRead(Cache::MESH, Backend::LEGACY, true, out.size(), since(t0));
            return true;
        }
        recordCorrupt(Cache::MESH, Backend::LEGACY);
    }
    recordRead(Cache::MESH, Backend::LEGACY, false, 0, 0);
    return false;
}

void writeHeader(const LLUUID& id, LLMeshHeader& header, const U8* data, S32 data_size, S32 reserve_bytes)
{
    const S32 header_bytes = header.mHeaderSize;
    if (!data || data_size <= 0 || header_bytes <= 0) return;
    {
        LLFileSystemStore::Access store;
        if (store)
        {
            if (data_size < header_bytes) return;
            LLMeshRepository::sCacheBytesWritten += data_size;
            ++LLMeshRepository::sCacheWrites;
            // Header first (its presence is what "cached" means), then every block that came
            // whole with it (a small mesh arrives complete in its first 4 KB).
            writePart(store.get(), id, SLMeshParts::HEADER, data, header_bytes);
            struct B { Part part; S32 off, size; bool cached; };
            const B blocks[] = {
                { SLMeshParts::LOD0, header.mLodOffset[0], header.mLodSize[0], header.mLodInCache[0] },
                { SLMeshParts::LOD1, header.mLodOffset[1], header.mLodSize[1], header.mLodInCache[1] },
                { SLMeshParts::LOD2, header.mLodOffset[2], header.mLodSize[2], header.mLodInCache[2] },
                { SLMeshParts::LOD3, header.mLodOffset[3], header.mLodSize[3], header.mLodInCache[3] },
                { SLMeshParts::SKIN, header.mSkinOffset, header.mSkinSize, header.mSkinInCache },
                { SLMeshParts::PHYS_CONVEX, header.mPhysicsConvexOffset, header.mPhysicsConvexSize, header.mPhysicsConvexInCache },
                { SLMeshParts::PHYS_MESH, header.mPhysicsMeshOffset, header.mPhysicsMeshSize, header.mPhysicsMeshInCache },
            };
            for (const B& b : blocks)
            {
                if (!b.cached || b.off < 0 || b.size <= 0) continue;
                const S64 at = (S64)header_bytes + b.off;
                if (at + b.size > data_size) continue;
                writePart(store.get(), id, b.part, data + at, b.size);
            }
            return;
        }
    }

    const U64 t0 = nowUsec();
    LLFileSystem file(id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);
    if (file.getMaxSize() >= reserve_bytes)
    {
        LLMeshRepository::sCacheBytesWritten += data_size;
        ++LLMeshRepository::sCacheWrites;

        write_preamble(file, header_bytes, header.getFlags());
        const bool ok = file.write(data, data_size);

        // #303: reserve the rest of the entry (LODs, skin, physics land there later) by
        // growing the file -- the gap reads back as zeros, which the readers' "first 1 KB
        // all zero = not written" check relies on.
        if (reserve_bytes > file.tell() && !file.setSize(reserve_bytes))
        {
            LL_WARNS_ONCE("Mesh") << "Could not reserve mesh cache entry (disk full?): " << id << LL_ENDL;
        }
        recordWrite(Cache::MESH, Backend::LEGACY, data_size, data_size + CACHE_PREAMBLE_SIZE, since(t0), ok);
    }
}

void markAllUncached(const LLUUID& id, S32 header_bytes, U32 flags)
{
    {
        LLFileSystemStore::Access store;
        if (store)
        {
            for (U32 p = SLMeshParts::LOD0; p < SLMeshParts::PART_COUNT; ++p)
                store->remove(SLMeshParts::key(id, (Part)p));
            return;
        }
    }
    LLFileSystem file(id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);
    if (file.getMaxSize() >= CACHE_PREAMBLE_SIZE)
    {
        write_preamble(file, header_bytes, flags);
    }
}

// ------------------------------------------------------------------ BlockReader

BlockReader::BlockReader(const LLUUID& id, Part part, S32 disk_offset, S32 size)
    : mID(id), mPart(part), mOffset(disk_offset), mSize(size), mStore(storeActive()), mT0(nowUsec())
{
    if (!mStore)
    {
        mFile.reset(new LLFileSystem(id, LLAssetType::AT_MESH));
    }
}

BlockReader::~BlockReader()
{
    if (!mDone)
    {
        // Asked for, not served from disk: the repository fetches it.
        recordRead(Cache::MESH, mStore ? Backend::STORE : Backend::LEGACY, false, 0, 0);
    }
}

bool BlockReader::available()
{
    if (mSize <= 0) return false;
    if (!mStore) return mFile && mFile->getSize() >= mOffset + mSize;
    LLFileSystemStore::Access store;
    if (!store) return false;
    LLCache::RecordInfo ri;
    return store->lookup(SLMeshParts::key(mID, mPart), &ri) && !ri.staged && (S32)ri.raw_bytes == mSize;
}

bool BlockReader::read(U8* dst)
{
    if (!dst || mSize <= 0) return false;
    mDone = true;
    bool ok = false;
    if (!mStore)
    {
        if (mFile)
        {
            mFile->seek(mOffset);
            ok = mFile->read(dst, mSize) && mFile->getLastBytesRead() == mSize;
        }
        // A reserved block that was never written reads back as zeros: not a hit.
        const bool hit = ok && !firstKbZero(dst, mSize);
        recordRead(Cache::MESH, Backend::LEGACY, hit, hit ? (U64)mSize : 0, hit ? since(mT0) : 0);
        return ok;
    }
    {
        LLFileSystemStore::Access store;
        if (store)
        {
            LLCache::ReadResult r = store->readSync(SLMeshParts::key(mID, mPart));
            if (r.status == LLCache::Status::OK && (S32)r.bytes.size() == mSize)
            {
                memcpy(dst, r.bytes.data(), mSize);
                ok = true;
            }
            else if (r.status == LLCache::Status::CORRUPT)
            {
                recordCorrupt(Cache::MESH, Backend::STORE);
            }
        }
    }
    if (!ok) memset(dst, 0, mSize);   // the caller's zero check then fetches it
    recordRead(Cache::MESH, Backend::STORE, ok, ok ? (U64)mSize : 0, ok ? since(mT0) : 0);
    return ok;
}

// ------------------------------------------------------------------ BlockWriter

BlockWriter::BlockWriter(const LLUUID& id, Part part, S32 disk_offset, S32 size)
    : mID(id), mPart(part), mOffset(disk_offset), mSize(size), mStore(storeActive())
{
    if (!mStore)
    {
        mFile.reset(new LLFileSystem(id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE));
    }
}

BlockWriter::~BlockWriter() = default;

bool BlockWriter::reserved()
{
    if (mSize <= 0) return false;
    if (!mStore) return mFile && mFile->getSize() >= mOffset + mSize;
    // Store: the header must be cached (as the old entry had to be reserved by it).
    LLFileSystemStore::Access store;
    return store && store->exists(SLMeshParts::key(mID, SLMeshParts::HEADER));
}

bool BlockWriter::write(const U8* data)
{
    if (!data || mSize <= 0) return false;
    if (mStore)
    {
        LLFileSystemStore::Access store;
        return store && writePart(store.get(), mID, mPart, data, mSize);
    }
    const U64 t0 = nowUsec();
    mFile->seek(mOffset, 0);
    const bool ok = mFile->write(data, mSize);
    recordWrite(Cache::MESH, Backend::LEGACY, mSize, mSize, since(t0), ok);
    return ok;
}

void BlockWriter::markCached(S32 header_bytes, U32 flags)
{
    if (!mStore && mFile)
    {
        write_preamble(*mFile, header_bytes, flags);
    }
}

} // namespace LLMeshDiskCache
