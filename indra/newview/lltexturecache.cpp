/**
 * @file lltexturecache.cpp
 * @brief Object which handles local texture caching
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lltexturecache.h"

#include "llapr.h"
#include "llagent.h"
#include "llcachemetrics.h"
#include "llcachestore.h"
#include "lldir.h"
#include "llformat.h"
#include "llimage.h"
#include "llimagej2c.h" // for version control
#include "llj2crecord.h"
#include "lllfsthread.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "slj2cmigrate.h"
#include "sltexturetiers.h"

#include <chrono>

// Included to allow LLTextureCache::purgeTextures() to pause watchdog timeout
#include "llappviewer.h"
#include "llmemory.h"

// Cache organization:
// cache/texture.entries
//  Unordered array of Entry structs
// cache/texture.cache
//  First TEXTURE_CACHE_ENTRY_SIZE bytes of each texture in texture.entries in same order
// cache/textures/[0-F]/UUID.texture
//  Actual texture body files
//
// STORE backend (#715, CacheStoreJ2C): one J2C record per texture in the segment store
// (llj2crecord.h: key = UUID, payload = the cached prefix of the codestream, meta = the
// image size the fetcher passed). The worker thread, the handles and responders, and what
// LLTextureFetch sees are unchanged; doRead / doWrite call storeRead / storeWrite instead
// of the header / body code.
// - Reads: one range read (pread of [offset, offset + size) clipped to the record) on the
//   cache thread, copied into an aligned buffer the responder takes (LLImageFormatted
//   frees with ll_aligned_free_16). No stripe, no padding, no entry-file lookup.
// - Writes: the bytes are copied on the cache thread and handed to the store's writer
//   (read-your-writes: a read right after sees them while the append is still queued);
//   the responder is told at once. Durability (and the write metric) comes from the
//   store's ticket when the record is appended.
// - Prefix growth: a longer prefix (lower discard level) is written as a WHOLE NEW
//   RECORD that supersedes the shorter one; the shorter one becomes dead bytes, counted
//   against the TEXTURES budget until the store reclaims its segment (#709). Rejected:
//   appending only the new bytes as a continuation record. Why rewrite wins (#709 wear):
//   discard steps grow ~4x each, so rewriting the whole prefix costs at most ~1.33x the
//   final size (the same bytes the legacy cache wrote: it rewrote the whole body file per
//   step), while a continuation chain would leave one texture spread over several
//   segments (several preads per read, no single-pread range reads), keep superseded
//   bytes alive as long as any later piece is, and make compaction / eviction move chains
//   instead of records. A newer write of the same texture still queued replaces the queued
//   one (never appended at all), which collapses the burst of steps during a fetch.
//   A write that is not longer than the stored prefix is skipped, except when it changes
//   the image size at the same length (the fetcher's "datasize + 1 = not complete" flag
//   flipping to complete), which the legacy updateEntry() rewrote too.

//note: there is no good to define 1024 for TEXTURE_CACHE_ENTRY_SIZE while FIRST_PACKET_SIZE is 600 on sim side.
const S32 TEXTURE_CACHE_ENTRY_SIZE = FIRST_PACKET_SIZE;//1024;
const F32 TEXTURE_CACHE_PURGE_AMOUNT = .20f; // % amount to reduce the cache by when it exceeds its limit
const F32 TEXTURE_CACHE_LRU_SIZE = .10f; // % amount for LRU list (low overhead to regenerate)
// Per-entry share of the header budget once taken by the removed (write-only) fast cache.
// Kept so the entry count derived from the cache size does not change.
const S32 TEXTURE_FAST_CACHE_ENTRY_SIZE = 16 * 16 * 4 + sizeof(S32) * 4;
const F32 TEXTURE_LAZY_PURGE_TIME_LIMIT = .004f; // 4ms. Would be better to autoadjust, but there is a major cache rework in progress.
const F32 TEXTURE_PRUNING_MAX_TIME = 15.f;

class LLTextureCacheWorker : public LLWorkerClass
{
    friend class LLTextureCache;

private:
    class ReadResponder : public LLLFSThread::Responder
    {
    public:
        ReadResponder(LLTextureCache* cache, handle_t handle) : mCache(cache), mHandle(handle) {}
        ~ReadResponder() {}
        void completed(S32 bytes)
        {
            mCache->lockWorkers();
            LLTextureCacheWorker* reader = mCache->getReader(mHandle);
            if (reader) reader->ioComplete(bytes);
            mCache->unlockWorkers();
        }
        LLTextureCache* mCache;
        LLTextureCacheWorker::handle_t mHandle;
    };

    class WriteResponder : public LLLFSThread::Responder
    {
    public:
        WriteResponder(LLTextureCache* cache, handle_t handle) : mCache(cache), mHandle(handle) {}
        ~WriteResponder() {}
        void completed(S32 bytes)
        {
            mCache->lockWorkers();
            LLTextureCacheWorker* writer = mCache->getWriter(mHandle);
            if (writer) writer->ioComplete(bytes);
            mCache->unlockWorkers();
        }
        LLTextureCache* mCache;
        LLTextureCacheWorker::handle_t mHandle;
    };

public:
    LLTextureCacheWorker(LLTextureCache* cache, const LLUUID& id,
                         const U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLTextureCache::Responder* responder)
        : LLWorkerClass(cache, "LLTextureCacheWorker"),
          mID(id),
          mCache(cache),
          mReadData(NULL),
          mWriteData(data),
          mDataSize(datasize),
          mOffset(offset),
          mImageSize(imagesize),
          mImageFormat(IMG_CODEC_J2C),
          mImageLocal(false),
          mResponder(responder),
          mFileHandle(LLLFSThread::nullHandle()),
          mBytesToRead(0),
          mBytesRead(0),
          mStartUsec(totalTime()),
          mPayloadSize(datasize)
    {
    }
    ~LLTextureCacheWorker()
    {
        llassert_always(!haveWork());
        ll_aligned_free_16(mReadData);
    }

    // override this interface
    virtual bool doRead() = 0;
    virtual bool doWrite() = 0;

    virtual bool doWork(S32 param); // Called from LLWorkerThread::processRequest()

    handle_t read() { addWork(0); return mRequestHandle; }
    handle_t write() { addWork(1); return mRequestHandle; }
    bool complete() { return checkWork(); }
    void ioComplete(S32 bytes)
    {
        mBytesRead = bytes;
    }

private:
    virtual void startWork(S32 param); // called from addWork() (MAIN THREAD)
    virtual void finishWork(S32 param, bool completed); // called from finishRequest() (WORK THREAD)
    virtual void endWork(S32 param, bool aborted); // called from doWork() (MAIN THREAD)

protected:
    LLTextureCache* mCache;
    LLUUID  mID;

    U8* mReadData;
    const U8* mWriteData;
    S32 mDataSize;
    S32 mOffset;
    S32 mImageSize;
    EImageCodec mImageFormat;
    bool mImageLocal;
    LLPointer<LLTextureCache::Responder> mResponder;
    LLLFSThread::handle_t mFileHandle;
    S32 mBytesToRead;
    LLAtomicS32 mBytesRead;
    // #715 metrics (LLCacheMetrics, recorded in finishWork()).
    U64 mStartUsec;                 // request time
    S32 mPayloadSize;               // write: bytes asked to cache
    U64 mDiskBytes = 0;             // write: bytes written to disk for it
    bool mMetricsSkip = false;      // write skipped (already cached) / local file: not counted
    bool mStoreBackend = false;     // served by the segment store (else legacy files)
};

class LLTextureCacheLocalFileWorker : public LLTextureCacheWorker
{
public:
    LLTextureCacheLocalFileWorker(LLTextureCache* cache, const std::string& filename, const LLUUID& id,
                         U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLTextureCache::Responder* responder)
            : LLTextureCacheWorker(cache, id, data, datasize, offset, imagesize, responder),
            mFileName(filename)

    {
        mMetricsSkip = true;    // a file:// texture, not the cache
    }

    virtual bool doRead();
    virtual bool doWrite();

private:
    std::string mFileName;
};

bool LLTextureCacheLocalFileWorker::doRead()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 local_size = LLAPRFile::size(mFileName, mCache->getLocalAPRFilePool());

    if (local_size > 0 && mFileName.size() > 4)
    {
        mDataSize = local_size; // Only a complete file is valid

        std::string extension = mFileName.substr(mFileName.size() - 3, 3);

        mImageFormat = LLImageBase::getCodecFromExtension(extension);

        if (mImageFormat == IMG_CODEC_INVALID)
        {
//          LL_WARNS() << "Unrecognized file extension " << extension << " for local texture " << mFileName << LL_ENDL;
            mDataSize = 0; // no data
            return true;
        }
    }
    else
    {
        // file doesn't exist
        mDataSize = 0; // no data
        return true;
    }

    if (!mDataSize || mDataSize > local_size)
    {
        mDataSize = local_size;
    }
    mReadData = (U8*)ll_aligned_malloc_16(mDataSize);

    S32 bytes_read = LLAPRFile::readEx(mFileName, mReadData, mOffset, mDataSize, mCache->getLocalAPRFilePool());

    if (bytes_read != mDataSize)
    {
//      LL_WARNS() << "Error reading file from local cache: " << mFileName
//              << " Bytes: " << mDataSize << " Offset: " << mOffset
//              << " / " << mDataSize << LL_ENDL;
        mDataSize = 0;
        ll_aligned_free_16(mReadData);
        mReadData = NULL;
    }
    else
    {
        mImageSize = local_size;
        mImageLocal = true;
    }
    return true;
}

bool LLTextureCacheLocalFileWorker::doWrite()
{
    // no writes for local files
    return false;
}

class LLTextureCacheRemoteWorker : public LLTextureCacheWorker
{
public:
    LLTextureCacheRemoteWorker(LLTextureCache* cache, const LLUUID& id,
                         const U8* data, S32 datasize, S32 offset,
                         S32 imagesize, // for writes
                         LLPointer<LLImageRaw> raw, S32 discardlevel,
                         LLTextureCache::Responder* responder)
            : LLTextureCacheWorker(cache, id, data, datasize, offset, imagesize, responder),
            mState(INIT),
            mRawImage(raw),
            mRawDiscardLevel(discardlevel)
    {
    }

    virtual bool doRead();
    virtual bool doWrite();

private:
    enum e_state
    {
        INIT = 0,
        LOCAL = 1,
        CACHE = 2,
        HEADER = 3,
        BODY = 4
    };

    e_state mState;
    LLPointer<LLImageRaw> mRawImage;
    S32 mRawDiscardLevel;
};


//virtual
void LLTextureCacheWorker::startWork(S32 param)
{
}

// This is where a texture is read from the cache system (header and body)
// Current assumption are:
// - the whole data are in a raw form, will be stored at mReadData
// - the size of this raw data is mDataSize and can be smaller than TEXTURE_CACHE_ENTRY_SIZE (the size of a record in the header cache)
// - the code supports offset reading but this is actually never exercised in the viewer
bool LLTextureCacheRemoteWorker::doRead()
{
    LL_PROFILE_ZONE_SCOPED;
    if (mCache->usingStore())
    {
        // #715: one range read from the segment store
        mStoreBackend = true;
        U8* data = nullptr;
        S32 imagesize = 0;
        bool from_legacy = false;
        const S32 n = mCache->storeRead(mID, mOffset, mDataSize, &data, imagesize, &from_legacy);
        if (from_legacy)
        {
            mStoreBackend = false;   // the bytes came from the legacy files (read-through)
        }
        if (n > 0)
        {
            mReadData = data;
            mDataSize = n;
            mImageSize = imagesize;
        }
        else
        {
            mReadData = NULL;
            mDataSize = n < 0 ? -1 : 0;   // -1: endWork() drops the record
        }
        return true;
    }
    bool done = false;
    S32 idx = -1;

    S32 local_size = 0;
    std::string local_filename;

    // First state / stage : find out if the file is local
    if (mState == INIT)
    {
#if 0
        std::string filename = mCache->getLocalFileName(mID);
        // Is it a JPEG2000 file?
        {
            local_filename = filename + ".j2c";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_J2C;
            }
        }
        // If not, is it a jpeg file?
        if (local_size == 0)
        {
            local_filename = filename + ".jpg";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_JPEG;
                mDataSize = local_size; // Only a complete .jpg file is valid
            }
        }
        // Hmm... What about a targa file? (used for UI texture mostly)
        if (local_size == 0)
        {
            local_filename = filename + ".tga";
            local_size = LLAPRFile::size(local_filename, mCache->getLocalAPRFilePool());
            if (local_size > 0)
            {
                mImageFormat = IMG_CODEC_TGA;
                mDataSize = local_size; // Only a complete .tga file is valid
            }
        }
        // Determine the next stage: if we found a file, then LOCAL else CACHE
        mState = (local_size > 0 ? LOCAL : CACHE);

        llassert_always(mState == CACHE) ;
#else
        mState = CACHE;
#endif
    }

    // Second state / stage : if the file is local, load it and leave
    if (!done && (mState == LOCAL))
    {
        llassert(local_size != 0);  // we're assuming there is a non empty local file here...
        if (!mDataSize || mDataSize > local_size)
        {
            mDataSize = local_size;
        }
        // Allocate read buffer
        mReadData = (U8*)ll_aligned_malloc_16(mDataSize);

        if (mReadData)
        {
            S32 bytes_read = LLAPRFile::readEx( local_filename,
                                                mReadData,
                                                mOffset,
                                                mDataSize,
                                                mCache->getLocalAPRFilePool());

            if (bytes_read != mDataSize)
            {
                LL_WARNS() << "Error reading file from local cache: " << local_filename
                        << " Bytes: " << mDataSize << " Offset: " << mOffset
                    << " / " << mDataSize << LL_ENDL;
                mDataSize = 0;
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
            }
            else
            {
                mImageSize = local_size;
                mImageLocal = true;
            }
        }
        else
        {
            LL_WARNS() << "Error allocating memory for cache: " << local_filename
                    << " of size: " << mDataSize << LL_ENDL;
            mDataSize = 0;
        }
        // We're done...
        done = true;
    }

    // Second state / stage : identify the cache or not...
    if (!done && (mState == CACHE))
    {
        LLTextureCache::Entry entry ;
        idx = mCache->getHeaderCacheEntry(mID, entry);
        if (idx < 0)
        {
            // The texture is *not* cached. We're done here...
            mDataSize = 0; // no data
            done = true;
        }
        else
        {
            mImageSize = entry.mImageSize ;
            // If the read offset is bigger than the header cache, we read directly from the body
            // Note that currently, we *never* read with offset from the cache, so the result is *always* HEADER
            mState = mOffset < TEXTURE_CACHE_ENTRY_SIZE ? HEADER : BODY;
        }
    }

    // Third state / stage : read data from the header cache (texture.entries) file
    if (!done && (mState == HEADER))
    {
        llassert_always(idx >= 0);  // we need an entry here or reading the header makes no sense
        llassert_always(mOffset < TEXTURE_CACHE_ENTRY_SIZE);
        S32 offset = idx * TEXTURE_CACHE_ENTRY_SIZE + mOffset;
        // Compute the size we need to read (in bytes)
        S32 size = TEXTURE_CACHE_ENTRY_SIZE - mOffset;
        size = llmin(size, mDataSize);
        // Allocate the read buffer
        mReadData = (U8*)ll_aligned_malloc_16(size);
        if (mReadData)
        {
            S32 bytes_read = mCache->readHeaderData(offset, mReadData, size);   // #300: held handle
            if (bytes_read != size)
            {
                LL_WARNS() << "LLTextureCacheWorker: "  << mID
                        << " incorrect number of bytes read from header: " << bytes_read
                        << " / " << size << LL_ENDL;
                LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY);
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
                mDataSize = -1; // failed
                done = true;
            }
            // If we already read all we expected, we're actually done
            if (mDataSize <= bytes_read)
            {
                done = true;
            }
            else
            {
                mState = BODY;
            }
        }
        else
        {
            LL_WARNS() << "LLTextureCacheWorker: "  << mID
                << " failed to allocate memory for reading: " << mDataSize << LL_ENDL;
            mReadData = NULL;
            mDataSize = -1; // failed
            done = true;
        }
    }

    // Fourth state / stage : read the rest of the data from the UUID based cached file
    if (!done && (mState == BODY))
    {
        std::string filename = mCache->getTextureFileName(mID);
        // #300: one open for both the size and the read (was LLAPRFile::size, then
        // LLAPRFile::readEx opening it again)
        LLFILE* body_file = LLFile::fopen(filename, "rb");
        S32 filesize = 0;
        if (body_file && fseek(body_file, 0, SEEK_END) == 0)
        {
            const long end = ftell(body_file);
            filesize = end > 0 ? (S32)llmin((S64)end, (S64)S32_MAX) : 0;
        }

        if (filesize && (filesize + TEXTURE_CACHE_ENTRY_SIZE) > mOffset)
        {
            S32 max_datasize = TEXTURE_CACHE_ENTRY_SIZE + filesize - mOffset;
            mDataSize = llmin(max_datasize, mDataSize);

            S32 data_offset, file_size, file_offset;

            // Reserve the whole data buffer first
            U8* data = (U8*)ll_aligned_malloc_16(mDataSize);
            if (data)
            {
                // Set the data file pointers taking the read offset into account. 2 cases:
                if (mOffset < TEXTURE_CACHE_ENTRY_SIZE)
                {
                    // Offset within the header record. That means we read something from the header cache.
                    // Note: most common case is (mOffset = 0), so this is the "normal" code path.
                    data_offset = TEXTURE_CACHE_ENTRY_SIZE - mOffset;   // i.e. TEXTURE_CACHE_ENTRY_SIZE if mOffset nul (common case)
                    file_offset = 0;
                    file_size = mDataSize - data_offset;
                    // Copy the raw data we've been holding from the header cache into the new sized buffer
                    llassert_always(mReadData);
                    memcpy(data, mReadData, data_offset);
                    ll_aligned_free_16(mReadData);
                    mReadData = NULL;
                }
                else
                {
                    // Offset bigger than the header record. That means we haven't read anything yet.
                    data_offset = 0;
                    file_offset = mOffset - TEXTURE_CACHE_ENTRY_SIZE;
                    file_size = mDataSize;
                    // No data from header cache to copy in that case, we skipped it all
                }

                // Now use that buffer as the object read buffer
                llassert_always(mReadData == NULL);
                mReadData = data;

                // Read the data at last
                S32 bytes_read = 0;
                if (file_size > 0 && fseek(body_file, file_offset, SEEK_SET) == 0)
                {
                    bytes_read = (S32)fread(mReadData + data_offset, 1, file_size, body_file);
                }
                if (bytes_read != file_size)
                {
                    LL_WARNS() << "LLTextureCacheWorker: "  << mID
                            << " incorrect number of bytes read from body: " << bytes_read
                            << " / " << file_size << LL_ENDL;
                    LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY);
                    ll_aligned_free_16(mReadData);
                    mReadData = NULL;
                    mDataSize = -1; // failed
                    done = true;
                }
            }
            else
            {
                LL_WARNS() << "LLTextureCacheWorker: "  << mID
                    << " failed to allocate memory for reading: " << mDataSize << LL_ENDL;
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
                mDataSize = -1; // failed
                done = true;
            }
        }
        else
        {
            // No body, we're done.
            mDataSize = llmax(TEXTURE_CACHE_ENTRY_SIZE - mOffset, 0);
            LL_DEBUGS() << "No body file for: " << filename << LL_ENDL;
        }
        if (body_file)
        {
            LLFile::close(body_file);
        }
        // Nothing else to do at that point...
        done = true;
    }

    // Clean up and exit
    return done;
}

// This is where *everything* about a texture is written down in the cache system (entry map, header and body)
// Current assumption are:
// - the whole data are in a raw form, starting at mWriteData
// - the size of this raw data is mDataSize and can be smaller than TEXTURE_CACHE_ENTRY_SIZE (the size of a record in the header cache)
// - the code *does not* support offset writing so there are no difference between buffer addresses and start of data
bool LLTextureCacheRemoteWorker::doWrite()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    bool done = false;
    S32 idx = -1;

    // First state / stage : check that what we're trying to cache is in an OK shape
    if (mState == INIT)
    {
        if ((mOffset != 0) // We currently do not support write offsets
            || (mDataSize <= 0) // Things will go badly wrong if mDataSize is nul or negative...
            || (mImageSize < mDataSize)
            || (mRawDiscardLevel < 0)
            || (mRawImage->isBufferInvalid())) // decode failed or malfunctioned, don't write
        {
            LL_WARNS() << "INIT state check failed for image: " << mID << " Size: " << mImageSize << " DataSize: " << mDataSize << " Discard:" << mRawDiscardLevel << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            mState = CACHE;
        }
    }

    // No LOCAL state for write(): because it doesn't make much sense to cache a local file...

    if (!done && mState == CACHE && mCache->usingStore())
    {
        // #715: one record in the segment store. The store's ticket reports the write metric
        // when the record is appended (submit -> durable); not this worker's.
        mStoreBackend = true;
        mMetricsSkip = true;
        if (!mCache->storeWrite(mID, mWriteData, mDataSize, mImageSize))
        {
            // Refused (busy / disk full / no overflow) or failed: the record that may already
            // exist is intact (records are never modified in place), so unlike the legacy
            // path nothing is removed (mDataSize 0, not -1).
            mDataSize = 0;
        }
        mRawImage = NULL;
        return true;
    }

    // Second state / stage : set an entry in the headers entry (texture.entries) file
    if (!done && (mState == CACHE))
    {
        bool alreadyCached = false;
        LLTextureCache::Entry entry;

        // Checks if this image is already in the entry list
        idx = mCache->getHeaderCacheEntry(mID, entry);
        if(idx < 0)
        {
            idx = mCache->setHeaderCacheEntry(mID, entry, mImageSize, mDataSize); // create the new entry.
        }
        else
        {
            alreadyCached = mCache->updateEntry(idx, entry, mImageSize, mDataSize); // update the existing entry.
        }

        if (!done)
        {
            if (idx < 0)
            {
                LL_WARNS() << "LLTextureCacheWorker: " << mID
                    << " Unable to create header entry for writing!" << LL_ENDL;
                mDataSize = -1; // failed
                done = true;
            }
            else
            {
                if (alreadyCached && (mDataSize <= TEXTURE_CACHE_ENTRY_SIZE))
                {
                    // Small texture already cached case: we're done with writing
                    mMetricsSkip = true;
                    done = true;
                }
                else
                {
                    if (!alreadyCached)
                    {
                        mDiskBytes += sizeof(LLTextureCache::Entry);   // the entry slot written by setHeaderCacheEntry
                    }
                    // If the texture has already been cached, we don't resave the header and go directly to the body part
                    mState = alreadyCached ? BODY : HEADER;
                }
            }
        }
    }


    // Third stage / state : write the header record in the header file (texture.cache)
    if (!done && (mState == HEADER))
    {
        if (idx < 0) // we need an entry here or storing the header makes no sense
        {
            LL_WARNS() << "index check failed" << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            S32 offset = idx * TEXTURE_CACHE_ENTRY_SIZE;    // skip to the correct spot in the header file
            S32 size = TEXTURE_CACHE_ENTRY_SIZE;            // record size is fixed for the header
            S32 bytes_written;

            if (mDataSize < TEXTURE_CACHE_ENTRY_SIZE)
            {
                // We need to write a full record in the header cache so, if the amount of data is smaller
                // than a record, we need to transfer the data to a buffer padded with 0 and write that
                U8* padBuffer = (U8*)ll_aligned_malloc_16(TEXTURE_CACHE_ENTRY_SIZE);
                memset(padBuffer, 0, TEXTURE_CACHE_ENTRY_SIZE);     // Init with zeros
                memcpy(padBuffer, mWriteData, mDataSize);           // Copy the write buffer
                bytes_written = mCache->writeHeaderData(offset, padBuffer, size);   // #300: held handle
                ll_aligned_free_16(padBuffer);
            }
            else
            {
                // Write the header record (== first TEXTURE_CACHE_ENTRY_SIZE bytes of the raw file) in the header file
                bytes_written = mCache->writeHeaderData(offset, mWriteData, size);  // #300: held handle
            }

            if (bytes_written <= 0)
            {
                LL_WARNS() << "LLTextureCacheWorker: " << mID
                    << " Unable to write header entry!" << LL_ENDL;
                mDataSize = -1; // failed
                done = true;
            }
            else
            {
                mDiskBytes += (U64)bytes_written;
            }

            // If we wrote everything (may be more with padding) in the header cache,
            // we're done so we don't have a body to store
            if (mDataSize <= bytes_written)
            {
                done = true;
            }
            else
            {
                mState = BODY;
            }
        }
    }

    // Fourth stage / state : write the body file, i.e. the rest of the texture in a "UUID" file name
    if (!done && (mState == BODY))
    {
        if (mDataSize <= TEXTURE_CACHE_ENTRY_SIZE) // wouldn't make sense to be here otherwise...
        {
            LL_WARNS() << "mDataSize check failed" << LL_ENDL;
            mDataSize = -1; // failed
            done = true;
        }
        else
        {
            S32 file_size = mDataSize - TEXTURE_CACHE_ENTRY_SIZE;

            {
                // build the cache file name from the UUID
                std::string filename = mCache->getTextureFileName(mID);
                //          LL_INFOS() << "Writing Body: " << filename << " Bytes: " << file_offset+file_size << LL_ENDL;
                S32 bytes_written = LLAPRFile::writeEx(filename,
                                                       mWriteData + TEXTURE_CACHE_ENTRY_SIZE,
                                                       0, file_size,
                                                       mCache->getLocalAPRFilePool());
                if (bytes_written <= 0)
                {
                    LL_WARNS() << "LLTextureCacheWorker: " << mID
                        << " incorrect number of bytes written to body: " << bytes_written
                        << " / " << file_size << LL_ENDL;
                    mDataSize = -1; // failed
                    done = true;
                }
                else
                {
                    mDiskBytes += (U64)bytes_written;
                }
            }

            // Nothing else to do at that point...
            done = true;
        }
    }
    mRawImage = NULL;

    // Clean up and exit
    return done;
}

//virtual
bool LLTextureCacheWorker::doWork(S32 param)
{
    LL_PROFILE_ZONE_SCOPED;
    bool res = false;
    mStoreBackend = mCache->usingStore();   // #715 metrics: every request of this cache, also failed INIT checks
    if (param == 0) // read
    {
        res = doRead();
    }
    else if (param == 1) // write
    {
        res = doWrite();
    }
    else
    {
        llassert_always(0);
    }
    return res;
}

//virtual (WORKER THREAD)
void LLTextureCacheWorker::finishWork(S32 param, bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mResponder.notNull())
    {
        bool success = (completed && mDataSize > 0);
        if (!mMetricsSkip)
        {
            // #715: request -> backend finished (this worker), the same bracket for the
            // legacy files and the store (store writes report from the store's ticket).
            const U64 now = totalTime();
            const U64 usec = now > mStartUsec ? now - mStartUsec : 0;
            const LLCacheMetrics::Backend b = mStoreBackend ? LLCacheMetrics::Backend::STORE : LLCacheMetrics::Backend::LEGACY;
            if (param == 0)
            {
                LLCacheMetrics::recordRead(LLCacheMetrics::Cache::J2C, b, success, success ? (U64)mDataSize : 0, usec);
            }
            else
            {
                LLCacheMetrics::recordWrite(LLCacheMetrics::Cache::J2C, b, (U64)llmax(mPayloadSize, 0), mDiskBytes, usec, success);
            }
        }
        if (param == 0)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - read");
            // read
            if (success)
            {
                mResponder->setData(mReadData, mDataSize, mImageSize, mImageFormat, mImageLocal);
                mReadData = NULL; // responder owns data
                mDataSize = 0;
            }
            else
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - read fail");
                ll_aligned_free_16(mReadData);
                mReadData = NULL;
            }
        }
        else
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("tcwfw - write");
            // write
            mWriteData = NULL; // we never owned data
            mDataSize = 0;
        }
        mCache->addCompleted(mResponder, success);
    }
}

//virtual (MAIN THREAD)
void LLTextureCacheWorker::endWork(S32 param, bool aborted)
{
    LL_PROFILE_ZONE_SCOPED;
    if (aborted)
    {
        // Let the destructor handle any cleanup
        return;
    }
    switch(param)
    {
      default:
      case 0: // read
      case 1: // write
      {
          if (mDataSize < 0)
          {
              // failed
              mCache->removeFromCache(mID);
          }
          break;
      }
    }
}

//////////////////////////////////////////////////////////////////////////////

LLTextureCache::LLTextureCache(bool threaded)
    : LLWorkerThread("TextureCache", threaded),
      mWorkersMutex(),
      mHeaderMutex(),
      mListMutex(),
      mHeaderAPRFile(NULL),
      mReadOnly(true), //do not allow to change the texture cache until setReadOnly() is called.
      mTexturesSizeTotal(0),
      mDoPurge(false)
{
    mHeaderAPRFilePoolp = new LLVolatileAPRPool(); // is_local = true, because this pool is for headers, headers are under own mutex
}

LLTextureCache::~LLTextureCache()
{
    clearDeleteList() ;
    writeUpdatedEntries() ;
    closeHeaderDataFile();  // #300
    closeEntriesFile();     // #300
    delete mHeaderAPRFilePoolp;
}

// #300: any thread (the cache thread reads/writes; purges close it)
S32 LLTextureCache::readHeaderData(S32 offset, U8* buf, S32 size)
{
    LLMutexLock lock(&mHeaderDataMutex);
    if (!mHeaderDataFile)
    {
        mHeaderDataFile = LLFile::fopen(mHeaderDataFileName, "r+b");
        mHeaderDataWritable = mHeaderDataFile != nullptr;
        if (!mHeaderDataFile)
        {
            mHeaderDataFile = LLFile::fopen(mHeaderDataFileName, "rb"); // read-only cache dir
        }
        if (!mHeaderDataFile)
        {
            return 0;   // no texture.cache yet: nothing cached
        }
    }
    if (size <= 0 || fseek(mHeaderDataFile, offset, SEEK_SET) != 0)
    {
        return 0;
    }
    return (S32)fread(buf, 1, size, mHeaderDataFile);
}

// #300
S32 LLTextureCache::writeHeaderData(S32 offset, const U8* buf, S32 size)
{
    LLMutexLock lock(&mHeaderDataMutex);
    if (mHeaderDataFile && !mHeaderDataWritable)
    {
        LLFile::close(mHeaderDataFile);     // opened read-only by readHeaderData()
        mHeaderDataFile = nullptr;
    }
    if (!mHeaderDataFile)
    {
        mHeaderDataFile = LLFile::fopen(mHeaderDataFileName, "r+b");
        if (!mHeaderDataFile)
        {
            // create it (only when it doesn't exist: "w+b" truncates)
            mHeaderDataFile = LLFile::fopen(mHeaderDataFileName, "w+b");
        }
        mHeaderDataWritable = mHeaderDataFile != nullptr;
        if (!mHeaderDataFile)
        {
            return 0;
        }
    }
    if (size <= 0 || fseek(mHeaderDataFile, offset, SEEK_SET) != 0)
    {
        return 0;
    }
    const S32 written = (S32)fwrite(buf, 1, size, mHeaderDataFile);
    if (fflush(mHeaderDataFile) != 0)
    {
        return 0;
    }
    return written;
}

// #300: before texture.cache is deleted (purge / clear) or its path changes
void LLTextureCache::closeHeaderDataFile()
{
    LLMutexLock lock(&mHeaderDataMutex);
    if (mHeaderDataFile)
    {
        LLFile::close(mHeaderDataFile);
        mHeaderDataFile = nullptr;
    }
}

// #300: Locks: mHeaderMutex
void LLTextureCache::mirrorEntry(S32 idx, const Entry& entry)
{
    if (idx < 0)
    {
        return;
    }
    if ((size_t)idx >= mEntries.size())
    {
        mEntries.resize((size_t)idx + 1);   // a gap is never read: lookups go through mHeaderIDMap
    }
    mEntries[idx] = entry;
    mEntriesBytes = mEntries.size() * sizeof(Entry);
}

// #300: Locks: mHeaderMutex
void LLTextureCache::mirrorEntries(const std::vector<Entry>& entries)
{
    mEntries = entries;
    mEntriesBytes = mEntries.size() * sizeof(Entry);
}

//////////////////////////////////////////////////////////////////////////////

//virtual
size_t LLTextureCache::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static LLFrameTimer timer ;
    static const F32 MAX_TIME_INTERVAL = 300.f ; //seconds.

    size_t res;
    res = LLWorkerThread::update(max_time_ms);

    mListMutex.lock();
    handle_list_t priorty_list = mPrioritizeWriteList; // copy list
    mPrioritizeWriteList.clear();
    responder_list_t completed_list = mCompletedList; // copy list
    mCompletedList.clear();
    mListMutex.unlock();

    // call 'completed' with workers list unlocked (may call readComplete() or writeComplete()
    for (responder_list_t::iterator iter1 = completed_list.begin();
         iter1 != completed_list.end(); ++iter1)
    {
        Responder *responder = iter1->responder;
        bool success = iter1->success;
        responder->completed(success);
    }

    if(!res && timer.getElapsedTimeF32() > MAX_TIME_INTERVAL)
    {
        timer.reset() ;
        writeUpdatedEntries() ;
    }

    if (mUsingStore)
    {
        // #708 placement: J2C writes land in the current region's run (main thread here;
        // the cache thread reads it in storeWrite()). Not after detachStore(): the shutdown
        // drain still calls update() while the agent / regions are being torn down.
        LLViewerRegion* region = mStoreAttached.load() ? gAgent.getRegion() : nullptr;
        if (region)
        {
            SLTextureTiers::instance().setRegion(region->getRegionID(), region->getHandle());
        }
    }
    else
    {
        // #715: legacy footprint every ~60 s (from the RAM index; never a directory walk)
        static LLFrameTimer footprint_timer;
        if (footprint_timer.getElapsedTimeF32() > 60.f)
        {
            footprint_timer.reset();
            sampleLegacyFootprint();
        }
    }

    return res;
}

//////////////////////////////////////////////////////////////////////////////
// search for local copy of UUID-based image file
std::string LLTextureCache::getLocalFileName(const LLUUID& id)
{
    // Does not include extension
    std::string idstr = id.asString();
    // TODO: should we be storing cached textures in skin directory?
    std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_LOCAL_ASSETS, idstr);
    return filename;
}

std::string LLTextureCache::getTextureFileName(const LLUUID& id)
{
    std::string idstr = id.asString();
    std::string delem = gDirUtilp->getDirDelimiter();
    std::string filename = mTexturesDirName + delem + idstr[0] + delem + idstr + ".texture";
    return filename;
}

//debug
bool LLTextureCache::isInCache(const LLUUID& id)
{
    if (mUsingStore)
    {
        std::shared_lock<std::shared_mutex> slock(mStoreMutex);
        return mStore && mStore->exists(LLJ2CRecord::makeKey(id));
    }
    LLMutexLock lock(&mHeaderMutex);
    id_map_t::const_iterator iter = mHeaderIDMap.find(id);

    return (iter != mHeaderIDMap.end()) ;
}

//debug
bool LLTextureCache::isInLocal(const LLUUID& id)
{
    S32 local_size = 0;
    std::string local_filename;

    std::string filename = getLocalFileName(id);
    // Is it a JPEG2000 file?
    {
        local_filename = filename + ".j2c";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    // If not, is it a jpeg file?
    {
        local_filename = filename + ".jpg";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    // Hmm... What about a targa file? (used for UI texture mostly)
    {
        local_filename = filename + ".tga";
        local_size = LLAPRFile::size(local_filename, getLocalAPRFilePool());
        if (local_size > 0)
        {
            return true ;
        }
    }

    return false ;
}
//////////////////////////////////////////////////////////////////////////////

//static
F32 LLTextureCache::sHeaderCacheVersion = 1.71f;
U32 LLTextureCache::sCacheMaxEntries = 1024 * 1024; //~1 million textures.
S64 LLTextureCache::sCacheMaxTexturesSize = 0; // no limit
std::string LLTextureCache::sHeaderCacheEncoderVersion = LLImageJ2C::getEngineInfo();

#if defined(ADDRESS_SIZE)
U32 LLTextureCache::sHeaderCacheAddressSize = ADDRESS_SIZE;
#else
U32 LLTextureCache::sHeaderCacheAddressSize = 32;
#endif

const char* entries_filename = "texture.entries";
const char* cache_filename = "texture.cache";
const char* old_textures_dirname = "textures";
//change the location of the texture cache to prevent from being deleted by old version viewers.
const char* textures_dirname = "texturecache";
const char* fast_cache_filename = "FastCache.cache";

void LLTextureCache::setDirNames(ELLPath location)
{
    closeHeaderDataFile();  // #300: the held texture.cache may be about to change
    {
        LLMutexLock lock(&mHeaderMutex);
        closeEntriesFile(); // #300: ... and texture.entries
    }
    mHeaderEntriesFileName = gDirUtilp->getExpandedFilename(location, textures_dirname, entries_filename);
    mHeaderDataFileName = gDirUtilp->getExpandedFilename(location, textures_dirname, cache_filename);
    mTexturesDirName = gDirUtilp->getExpandedFilename(location, textures_dirname);
}

void LLTextureCache::purgeCache(ELLPath location, bool remove_dir)
{
    if (mUsingStore)
    {
        // #715: Clear Cache with the store backend: every J2C record, and the legacy files.
        // A migration still running holds the legacy files open: close them first (its
        // reads then find nothing, it completes and its folder removal is a no-op).
        {
            std::shared_ptr<SLJ2CMigrate::Legacy> legacy;
            {
                std::lock_guard<std::mutex> llock(mLegacySourceMutex);
                legacy = mLegacySource;
            }
            if (legacy)
            {
                legacy->close();
            }
        }
        setLegacySource(nullptr);
        {
            std::shared_lock<std::shared_mutex> slock(mStoreMutex);
            if (mStore && !mStore->readOnly())
            {
                LL_INFOS("TextureCache") << "Clear Cache: dropping every J2C record in the cache store" << LL_ENDL;
                mStore->removeAll(LLCache::Kind::J2C);
            }
        }
        if (!mReadOnly)
        {
            gDirUtilp->deleteDirAndContents(legacyDir(location));
            gDirUtilp->deleteDirAndContents(gDirUtilp->getExpandedFilename(location, old_textures_dirname));
        }
        return;
    }
    LLMutexLock lock(&mHeaderMutex);

    if (!mReadOnly)
    {
        setDirNames(location);
        llassert_always(mHeaderAPRFile == NULL);

        //remove the legacy cache if exists
        std::string texture_dir = mTexturesDirName ;
        mTexturesDirName = gDirUtilp->getExpandedFilename(location, old_textures_dirname);
        if(LLFile::isdir(mTexturesDirName))
        {
            std::string file_name = gDirUtilp->getExpandedFilename(location, entries_filename);
            // mHeaderAPRFilePoolp because we are under header mutex, and can be in main thread
            LLAPRFile::remove(file_name, mHeaderAPRFilePoolp);

            file_name = gDirUtilp->getExpandedFilename(location, cache_filename);
            LLAPRFile::remove(file_name, mHeaderAPRFilePoolp);

            purgeAllTextures(true);
        }
        mTexturesDirName = texture_dir ;
    }

    //remove the current texture cache.
    purgeAllTextures(remove_dir);
}

//is called in the main thread before initCache(...) is called.
void LLTextureCache::setReadOnly(bool read_only)
{
    mReadOnly = read_only ;
}

// Called in the main thread.
// Returns the unused amount of max_size if any
S64 LLTextureCache::initCache(ELLPath location, S64 max_size, bool texture_cache_mismatch, LLCache::Store* store)
{
    llassert_always(getPending() == 0) ; //should not start accessing the texture cache before initialized.

    if (store)
    {
        // #715 STORE backend. No legacy init at all: texture.entries / texture.cache / the
        // bodies belong to the migration from now on (slj2cmigrate.h); running the legacy
        // startup purge here would rewrite them under it. Budget: the whole texture share
        // (no entry-slot reserve), applied through SLTextureTiers.
        {
            std::unique_lock<std::shared_mutex> lock(mStoreMutex);
            mStore = store;
        }
        mUsingStore = true;
        mStoreAttached = true;
        setDirNames(location);
        sCacheMaxTexturesSize = max_size;
        if (texture_cache_mismatch && !mReadOnly && !store->readOnly())
        {
            // LocalCacheVersion bump / startup purge: the legacy cache would have wiped itself.
            LL_INFOS("TextureCache") << "Texture cache version mismatch: dropping J2C records and the legacy files" << LL_ENDL;
            store->removeAll(LLCache::Kind::J2C);
            gDirUtilp->deleteDirAndContents(mTexturesDirName);
        }
        LL_INFOS("TextureCache") << "J2C texture cache: in the cache store " << store->dir() << " (kind j2c, range reads), "
                                 << (max_size >> 20) << " MB" << (store->readOnly() ? ", read-only + private overflow (second instance)" : "")
                                 << (store->ready() ? "" : "; index loading on the store's writer") << LL_ENDL;
        return 0;
    }

    S64 entries_size = (max_size * 36) / 100; //0.36 * max_size
    S64 max_entries = entries_size / (TEXTURE_CACHE_ENTRY_SIZE + TEXTURE_FAST_CACHE_ENTRY_SIZE);
    sCacheMaxEntries = (S32)(llmin((S64)sCacheMaxEntries, max_entries));
    entries_size = sCacheMaxEntries * (TEXTURE_CACHE_ENTRY_SIZE + TEXTURE_FAST_CACHE_ENTRY_SIZE);
    max_size -= entries_size;
    if (sCacheMaxTexturesSize > 0)
        sCacheMaxTexturesSize = llmin(sCacheMaxTexturesSize, max_size);
    else
        sCacheMaxTexturesSize = max_size;
    max_size -= sCacheMaxTexturesSize;

    LL_INFOS("TextureCache") << "Headers: " << sCacheMaxEntries
            << " Textures size: " << sCacheMaxTexturesSize / (1024 * 1024) << " MB" << LL_ENDL;

    setDirNames(location);

    if(texture_cache_mismatch)
    {
        //if readonly, disable the texture cache,
        //otherwise wipe out the texture cache.
        purgeAllTextures(true);

        if(mReadOnly)
        {
            return max_size ;
        }
    }

    if (!mReadOnly)
    {
        // The fast cache was write-only and is gone; reclaim the old file.
        const std::string fast_cache = gDirUtilp->getExpandedFilename(location, textures_dirname, fast_cache_filename);
        if (LLFile::isfile(fast_cache))
        {
            LLFile::remove(fast_cache);
        }

        LLFile::mkdir(mTexturesDirName);

        const char* subdirs = "0123456789abcdef";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + gDirUtilp->getDirDelimiter() + subdirs[i];
            LLFile::mkdir(dirname);
        }
    }
    const U64 startup_t0 = totalTime();
    readHeaderCache();
    purgeTextures(true); // calc mTexturesSize and make some room in the texture cache if we need it
    LLCacheMetrics::recordStartup(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY, totalTime() - startup_t0);
    sampleLegacyFootprint();

    llassert_always(getPending() == 0) ; //should not start accessing the texture cache before initialized.

    return max_size; // unused cache space
}

//----------------------------------------------------------------------------
// mHeaderMutex must be locked for the following functions!

LLAPRFile* LLTextureCache::openHeaderEntriesFile(bool readonly, S32 offset)
{
    llassert_always(mHeaderAPRFile == NULL);
    apr_int32_t flags = readonly ? APR_READ|APR_BINARY : APR_READ|APR_WRITE|APR_BINARY;
    mHeaderAPRFile = new LLAPRFile(mHeaderEntriesFileName, flags, mHeaderAPRFilePoolp);
    if(offset > 0)
    {
        mHeaderAPRFile->seek(APR_SET, offset);
    }
    return mHeaderAPRFile;
}

void LLTextureCache::closeHeaderEntriesFile()
{
    if(!mHeaderAPRFile)
    {
        return ;
    }

    delete mHeaderAPRFile;
    mHeaderAPRFile = NULL;
}

void LLTextureCache::readEntriesHeader()
{
    // mHeaderEntriesInfo initializes to default values so safe not to read it
    llassert_always(mHeaderAPRFile == NULL);
    if (LLAPRFile::isExist(mHeaderEntriesFileName, mHeaderAPRFilePoolp))
    {
        LLAPRFile::readEx(mHeaderEntriesFileName, (U8*)&mHeaderEntriesInfo, 0, sizeof(EntriesInfo),
                          mHeaderAPRFilePoolp);
    }
    else //create an empty entries header.
    {
        setEntriesHeader();
        writeEntriesHeader() ;
    }
}

void LLTextureCache::setEntriesHeader()
{
    if (sHeaderEncoderStringSize < sHeaderCacheEncoderVersion.size() + 1)
    {
        // For simplicity we use predefined size of header, so if version string
        // doesn't fit, either getEngineInfo() returned malformed string or
        // sHeaderEncoderStringSize need to be increased.
        // Also take into accout that c_str() returns additional null character
        LL_ERRS() << "Version string doesn't fit in header" << LL_ENDL;
    }

    mHeaderEntriesInfo.mVersion = sHeaderCacheVersion;
    mHeaderEntriesInfo.mAdressSize = sHeaderCacheAddressSize;
    strcpy(mHeaderEntriesInfo.mEncoderVersion, sHeaderCacheEncoderVersion.c_str());
    mHeaderEntriesInfo.mEntries = 0;
}

void LLTextureCache::writeEntriesHeader()
{
    llassert_always(mHeaderAPRFile == NULL);
    if (!mReadOnly)
    {
        LLAPRFile::writeEx(mHeaderEntriesFileName, (U8*)&mHeaderEntriesInfo, 0, sizeof(EntriesInfo),
                           mHeaderAPRFilePoolp);
    }
}

//mHeaderMutex is locked before calling this.
S32 LLTextureCache::openAndReadEntry(const LLUUID& id, Entry& entry, bool create)
{
    S32 idx = -1;

    id_map_t::iterator iter1 = mHeaderIDMap.find(id);
    if (iter1 != mHeaderIDMap.end())
    {
        idx = iter1->second;
    }

    if (idx < 0)
    {
        if (create && !mReadOnly)
        {
            if (mHeaderEntriesInfo.mEntries < sCacheMaxEntries)
            {
                // Add an entry to the end of the list
                idx = mHeaderEntriesInfo.mEntries++;

            }
            else if (!mFreeList.empty())
            {
                idx = *(mFreeList.begin());
                mFreeList.erase(mFreeList.begin());
            }
            else
            {
                // Look for a still valid entry in the LRU, oldest first
                while (!mLRUOrder.empty())
                {
                    LLUUID oldid = mLRUOrder.front();
                    mLRUOrder.pop_front();
                    // Erase entry from LRU regardless; skip ids already used or accessed since
                    if (mLRU.erase(oldid) == 0)
                    {
                        continue;
                    }
                    // Look up entry and use it if it is valid
                    id_map_t::iterator iter3 = mHeaderIDMap.find(oldid);
                    if (iter3 != mHeaderIDMap.end() && iter3->second >= 0)
                    {
                        idx = iter3->second;
                        size_map_t::const_iterator sz = mTexturesSizeMap.find(oldid);
                        LLCacheMetrics::recordEvict(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY,
                                                    TEXTURE_CACHE_ENTRY_SIZE + (sz != mTexturesSizeMap.end() ? (U64)llmax(sz->second, 0) : 0));
                        removeCachedTexture(oldid) ;//remove the existing cached texture to release the entry index.
                        break;
                    }
                }
                // if (idx < 0) at this point, we will rebuild the LRU
                //  and retry if called from setHeaderCacheEntry(),
                //  otherwise this shouldn't happen and will trigger an error
            }
            if (idx >= 0)
            {
                entry.mID = id ;
                entry.mImageSize = -1 ; //mark it is a brand-new entry.
                entry.mBodySize = 0 ;
            }
        }
    }
    else
    {
        // Remove this entry from the LRU if it exists
        mLRU.erase(id);
        // Read the entry
        idx_entry_map_t::iterator iter = mUpdatedEntryMap.find(idx) ;
        if(iter != mUpdatedEntryMap.end())
        {
            entry = iter->second ;
        }
        else
        {
            readEntryFromHeaderImmediately(idx, entry) ;
        }
        if(entry.mImageSize <= entry.mBodySize)//it happens on 64-bit systems, do not know why
        {
            LL_WARNS() << "corrupted entry: " << id << " entry image size: " << entry.mImageSize << " entry body size: " << entry.mBodySize << LL_ENDL ;
            LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY);

            //erase this entry and the cached texture from the cache.
            std::string tex_filename = getTextureFileName(id);
            removeEntry(idx, entry, tex_filename) ;
            mUpdatedEntryMap.erase(idx) ;
            idx = -1 ;
        }
    }
    return idx;
}

//mHeaderMutex is locked before calling this.
void LLTextureCache::writeEntryToHeaderImmediately(S32& idx, Entry& entry, bool write_header)
{
    // #300: through the held texture.entries handle (was open + seek + write + close for
    // every new or changed entry -- once per newly cached texture)
    S32 bytes_written ;
    S32 offset = sizeof(EntriesInfo) + idx * sizeof(Entry);
    if(write_header)
    {
        bytes_written = writeEntriesFile(0, (const U8*)&mHeaderEntriesInfo, sizeof(EntriesInfo)) ;
        if(bytes_written != sizeof(EntriesInfo))
        {
            clearCorruptedCache() ; //clear the cache.
            idx = -1 ;//mark the idx invalid.
            return ;
        }
    }
    bytes_written = writeEntriesFile(offset, (const U8*)&entry, (S32)sizeof(Entry));
    if(bytes_written != sizeof(Entry))
    {
        clearCorruptedCache() ; //clear the cache.
        idx = -1 ;//mark the idx invalid.

        return ;
    }

    mUpdatedEntryMap.erase(idx) ;
    mirrorEntry(idx, entry);    // #300
}

// #300: Locks: mHeaderMutex. texture.entries held open for single-entry writes. The
// bulk paths (openAndReadEntries, writeEntriesAndClose, updatedHeaderEntriesFile, the
// EntriesInfo readEx/writeEx) still use their own unbuffered APR handles; this one never
// reads and is flushed after every write, so the two never see stale bytes.
S32 LLTextureCache::writeEntriesFile(S32 offset, const U8* buf, S32 size)
{
    if (!mEntriesFile)
    {
        // no create: texture.entries exists once readEntriesHeader() ran (the APR path
        // didn't create it either)
        mEntriesFile = LLFile::fopen(mHeaderEntriesFileName, "r+b");
        if (!mEntriesFile)
        {
            return 0;
        }
    }
    if (size <= 0 || fseek(mEntriesFile, offset, SEEK_SET) != 0)
    {
        return 0;
    }
    const S32 written = (S32)fwrite(buf, 1, size, mEntriesFile);
    if (fflush(mEntriesFile) != 0)
    {
        return 0;
    }
    return written;
}

// #300: Locks: mHeaderMutex
void LLTextureCache::closeEntriesFile()
{
    if (mEntriesFile)
    {
        LLFile::close(mEntriesFile);
        mEntriesFile = nullptr;
    }
}

//mHeaderMutex is locked before calling this.
void LLTextureCache::readEntryFromHeaderImmediately(S32& idx, Entry& entry)
{
    // #300: served from the RAM mirror of texture.entries (it holds every entry written)
    if (idx >= 0 && (size_t)idx < mEntries.size())
    {
        entry = mEntries[idx];
        ++mEntryRAMHits;
        return;
    }
    ++mEntryDiskReads;
    S32 offset = sizeof(EntriesInfo) + idx * sizeof(Entry);
    LLAPRFile* aprfile = openHeaderEntriesFile(true, offset);
    S32 bytes_read = aprfile->read((void*)&entry, (S32)sizeof(Entry));
    closeHeaderEntriesFile();

    if(bytes_read != sizeof(Entry))
    {
        clearCorruptedCache() ; //clear the cache.
        idx = -1 ;//mark the idx invalid.
    }
}

//mHeaderMutex is locked before calling this.
//update an existing entry time stamp, delay writing.
void LLTextureCache::updateEntryTimeStamp(S32 idx, Entry& entry)
{
    // Always stamp (in RAM; flushed in batches by writeUpdatedEntries) so purges evict by
    // last use rather than by first write.
    if (idx >= 0)
    {
        if (!mReadOnly)
        {
            entry.mTime = (U32)time(NULL);
            mUpdatedEntryMap[idx] = entry ;
        }
    }
}

//update an existing entry, write to header file immediately.
bool LLTextureCache::updateEntry(S32& idx, Entry& entry, S32 new_image_size, S32 new_data_size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    S32 new_body_size = llmax(0, new_data_size - TEXTURE_CACHE_ENTRY_SIZE) ;

    if(new_image_size == entry.mImageSize && new_body_size == entry.mBodySize)
    {
        return true ; //nothing changed.
    }
    else
    {
        bool purge = false ;

        lockHeaders() ;

        bool update_header = false ;
        if(entry.mImageSize < 0) //is a brand-new entry
        {
            mHeaderIDMap[entry.mID] = idx;
            mTexturesSizeMap[entry.mID] = new_body_size ;
            mTexturesSizeTotal += new_body_size ;

            // Update Header
            update_header = true ;
        }
        else if (entry.mBodySize != new_body_size)
        {
            //already in mHeaderIDMap.
            mTexturesSizeMap[entry.mID] = new_body_size ;
            mTexturesSizeTotal -= entry.mBodySize ;
            mTexturesSizeTotal += new_body_size ;
        }
        entry.mTime = (U32)time(NULL);
        entry.mImageSize = new_image_size ;
        entry.mBodySize = new_body_size ;

        writeEntryToHeaderImmediately(idx, entry, update_header) ;

        if (mTexturesSizeTotal > sCacheMaxTexturesSize)
        {
            purge = true;
        }

        unlockHeaders() ;

        if (purge)
        {
            mDoPurge = true;
        }
    }

    return false ;
}

U32 LLTextureCache::openAndReadEntries(std::vector<Entry>& entries)
{
    U32 num_entries = mHeaderEntriesInfo.mEntries;

    mHeaderIDMap.clear();
    mTexturesSizeMap.clear();
    mFreeList.clear();
    mTexturesSizeTotal = 0;

    LLAPRFile* aprfile = NULL;
    if(mUpdatedEntryMap.empty())
    {
        aprfile = openHeaderEntriesFile(true, (S32)sizeof(EntriesInfo));
    }
    else //update the header file first.
    {
        aprfile = openHeaderEntriesFile(false, 0);
        updatedHeaderEntriesFile() ;
        if(!aprfile)
        {
            return 0;
        }
        aprfile->seek(APR_SET, (S32)sizeof(EntriesInfo));
    }

    try
    {
        entries.resize(num_entries);
        mHeaderIDMap.reserve(num_entries);
        mTexturesSizeMap.reserve(num_entries);
        S32 total_entries_size = sizeof(Entry) * num_entries;
        S32 bytes_read = aprfile->read((void*)entries.data(), total_entries_size);
        if (bytes_read != total_entries_size)
        {
            LL_WARNS() << "Corrupted header entries, expected " << total_entries_size << " bytes but got " << bytes_read << " bytes" << LL_ENDL;
            closeHeaderEntriesFile();
            purgeAllTextures(false);
            return 0;
        }

        mirrorEntries(entries);     // #300: the RAM copy starts from what's on disk
        for (U32 idx = 0; idx < num_entries; idx++)
        {
            const Entry& entry = entries[idx];
//          LL_INFOS() << "ENTRY: " << entry.mTime << " TEX: " << entry.mID << " IDX: " << idx << " Size: " << entry.mImageSize << LL_ENDL;
            if(entry.mImageSize > entry.mBodySize)
            {
                mHeaderIDMap[entry.mID] = idx;
                mTexturesSizeMap[entry.mID] = entry.mBodySize;
                mTexturesSizeTotal += entry.mBodySize;
            }
            else
            {
                mFreeList.insert(idx);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        // Too little ram yet very large cache?
        // Should this actually crash viewer?
        entries.clear();
        LL_WARNS() << "Bad alloc trying to read texture entries from cache, mFreeList: " << (S32)mFreeList.size()
            << ", total entries: " << num_entries << LL_ENDL;
        closeHeaderEntriesFile();
        purgeAllTextures(false);
        return 0;
    }
    closeHeaderEntriesFile();
    return num_entries;
}

void LLTextureCache::writeEntriesAndClose(const std::vector<Entry>& entries)
{
    auto num_entries = entries.size();
    llassert_always(num_entries == mHeaderEntriesInfo.mEntries);

    if (!mReadOnly)
    {
        LLAPRFile* aprfile = openHeaderEntriesFile(false, (S32)sizeof(EntriesInfo));
        for (size_t idx=0; idx<num_entries; idx++)
        {
            S32 bytes_written = aprfile->write((void*)(&entries[idx]), (S32)sizeof(Entry));
            if(bytes_written != sizeof(Entry))
            {
                clearCorruptedCache() ; //clear the cache.
                return ;
            }
        }
        closeHeaderEntriesFile();
    }
    mirrorEntries(entries);     // #300
}

void LLTextureCache::writeUpdatedEntries()
{
    lockHeaders() ;
    if (!mReadOnly && !mUpdatedEntryMap.empty())
    {
        openHeaderEntriesFile(false, 0);
        updatedHeaderEntriesFile() ;
        closeHeaderEntriesFile();
    }
    unlockHeaders() ;
}

//mHeaderMutex is locked and mHeaderAPRFile is created before calling this.
void LLTextureCache::updatedHeaderEntriesFile()
{
    if (!mReadOnly && !mUpdatedEntryMap.empty() && mHeaderAPRFile)
    {
        //entriesInfo
        mHeaderAPRFile->seek(APR_SET, 0);
        S32 bytes_written = mHeaderAPRFile->write((U8*)&mHeaderEntriesInfo, sizeof(EntriesInfo)) ;
        if(bytes_written != sizeof(EntriesInfo))
        {
            clearCorruptedCache() ; //clear the cache.
            return ;
        }

        //write each updated entry
        S32 entry_size = (S32)sizeof(Entry) ;
        S32 prev_idx = -1 ;
        S32 delta_idx ;
        for (idx_entry_map_t::iterator iter = mUpdatedEntryMap.begin(); iter != mUpdatedEntryMap.end(); ++iter)
        {
            delta_idx = iter->first - prev_idx - 1;
            prev_idx = iter->first ;
            if(delta_idx)
            {
                mHeaderAPRFile->seek(APR_CUR, delta_idx * entry_size);
            }

            bytes_written = mHeaderAPRFile->write((void*)(&iter->second), entry_size);
            if(bytes_written != entry_size)
            {
                clearCorruptedCache() ; //clear the cache.
                return ;
            }
            mirrorEntry(iter->first, iter->second);  // #300
        }
        mUpdatedEntryMap.clear() ;
    }
}
//----------------------------------------------------------------------------

// Called from either the main thread or the worker thread
void LLTextureCache::readHeaderCache()
{
    mHeaderMutex.lock();

    mLRU.clear(); // always clear the LRU
    mLRUOrder.clear();

    readEntriesHeader();

    if (mHeaderEntriesInfo.mVersion != sHeaderCacheVersion
        || mHeaderEntriesInfo.mAdressSize != sHeaderCacheAddressSize
        || strcmp(mHeaderEntriesInfo.mEncoderVersion, sHeaderCacheEncoderVersion.c_str()) != 0)
    {
        if (!mReadOnly)
        {
            LL_INFOS() << "Texture Cache version mismatch, Purging." << LL_ENDL;
            purgeAllTextures(false);
        }
    }
    else
    {
        std::vector<Entry> entries;
        U32 num_entries = openAndReadEntries(entries);
        if (num_entries)
        {
            U32 empty_entries = 0;
            typedef std::pair<U32, S32> lru_data_t;
            std::vector<lru_data_t> lru;   // sorted below; a std::set is ~10x slower at 1M entries
            lru.reserve(num_entries);
            std::set<U32> purge_list;
            for (U32 i=0; i<num_entries; i++)
            {
                Entry& entry = entries[i];
                if (entry.mImageSize <= 0)
                {
                    // This will be in the Free List, don't put it in the LRU
                    ++empty_entries;
                }
                else
                {
                    lru.push_back(std::make_pair(entry.mTime, i));
                    if (entry.mBodySize > 0)
                    {
                        if (entry.mBodySize > entry.mImageSize)
                        {
                            // Shouldn't happen, failsafe only
                            LL_WARNS() << "Bad entry: " << i << ": " << entry.mID << ": BodySize: " << entry.mBodySize << LL_ENDL;
                            purge_list.insert(i);
                        }
                    }
                }
            }
            std::sort(lru.begin(), lru.end());
            if (num_entries - empty_entries > sCacheMaxEntries)
            {
                // Special case: cache size was reduced, need to remove entries
                U32 entries_to_purge = (num_entries - empty_entries) - sCacheMaxEntries;
                LL_INFOS() << "Texture Cache Entries: " << num_entries << " Max: " << sCacheMaxEntries << " Empty: " << empty_entries << " Purging: " << entries_to_purge << LL_ENDL;
                // We can exit the following loop with the given condition, since if we'd reach the end of the lru set we'd have:
                // purge_list.size() = lru.size() = num_entries - empty_entries = entries_to_purge + sCacheMaxEntries >= entries_to_purge
                // So, it's certain that iter will never reach lru.end() first.
                std::vector<lru_data_t>::iterator iter = lru.begin();
                while (purge_list.size() < entries_to_purge)
                {
                    purge_list.insert(iter->second);
                    ++iter;
                }
            }

            {
                S32 lru_entries = (S32)((F32)sCacheMaxEntries * TEXTURE_CACHE_LRU_SIZE);
                for (std::vector<lru_data_t>::iterator iter = lru.begin(); iter != lru.end(); ++iter)
                {
                    if (mLRU.insert(entries[iter->second].mID).second)
                    {
                        mLRUOrder.push_back(entries[iter->second].mID);
                    }
//                  LL_INFOS() << "LRU: " << iter->first << " : " << iter->second << LL_ENDL;
                    if (--lru_entries <= 0)
                        break;
                }
            }

            if (purge_list.size() > 0)
            {
                LLTimer timer;
                for (std::set<U32>::iterator iter = purge_list.begin(); iter != purge_list.end(); ++iter)
                {
                    std::string tex_filename = getTextureFileName(entries[*iter].mID);
                    removeEntry((S32)*iter, entries[*iter], tex_filename);

                    //make sure that pruning entries doesn't take too much time
                    if (timer.getElapsedTimeF32() > TEXTURE_PRUNING_MAX_TIME)
                    {
                        break;
                    }
                }
                writeEntriesAndClose(entries);
            }
            else
            {
                //entries are not changed, nothing here.
            }
        }
    }
    mHeaderMutex.unlock();
}

//////////////////////////////////////////////////////////////////////////////

//the header mutex is locked before calling this.
void LLTextureCache::clearCorruptedCache()
{
    LL_WARNS() << "the texture cache is corrupted, need to be cleared." << LL_ENDL ;

    closeHeaderEntriesFile();//close possible file handler
    purgeAllTextures(false) ; //clear the cache.

    if (!mReadOnly) //regenerate the directory tree if not exists.
    {
        LLFile::mkdir(mTexturesDirName);

        const char* subdirs = "0123456789abcdef";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + gDirUtilp->getDirDelimiter() + subdirs[i];
            LLFile::mkdir(dirname);
        }
    }

    return ;
}

void LLTextureCache::purgeAllTextures(bool purge_directories)
{
    closeHeaderDataFile();  // #300: about to delete texture.cache (Windows can't delete an open file)
    closeEntriesFile();     // #300: ... and texture.entries
    mEntries.clear();       // #300
    mEntriesBytes = 0;
    if (!mReadOnly)
    {
        const char* subdirs = "0123456789abcdef";
        std::string delem = gDirUtilp->getDirDelimiter();
        std::string mask = "*";
        for (S32 i=0; i<16; i++)
        {
            std::string dirname = mTexturesDirName + delem + subdirs[i];
            LL_INFOS() << "Deleting files in directory: " << dirname << LL_ENDL;
            if (purge_directories)
            {
                gDirUtilp->deleteDirAndContents(dirname);
            }
            else
            {
                gDirUtilp->deleteFilesInDir(dirname, mask);
            }
#if LL_WINDOWS
            // Texture cache can be large and can take a while to remove
            // assure OS that processes is alive and not hanging
            MSG msg;
            PeekMessage(&msg, 0, 0, 0, PM_NOREMOVE | PM_NOYIELD);
#endif
        }
        gDirUtilp->deleteFilesInDir(mTexturesDirName, mask); // headers, fast cache
        if (purge_directories)
        {
            LLFile::rmdir(mTexturesDirName);
        }
    }
    mHeaderIDMap.clear();
    mTexturesSizeMap.clear();
    mTexturesSizeTotal = 0;
    mFreeList.clear();
    mTexturesSizeTotal = 0;
    mUpdatedEntryMap.clear();

    // Info with 0 entries
    setEntriesHeader();
    writeEntriesHeader();

    LL_INFOS() << "The entire texture cache is cleared." << LL_ENDL ;
}

void LLTextureCache::purgeTexturesLazy(F32 time_limit_sec)
{
    if (mReadOnly)
    {
        return;
    }

    if (!mThreaded)
    {
        LLAppViewer::instance()->pauseMainloopTimeout();
    }

    // time_limit doesn't account for lock time
    LLMutexLock lock(&mHeaderMutex);

    if (mPurgeEntryList.empty())
    {
        // Read the entries list and form list of textures to purge
        std::vector<Entry> entries;
        U32 num_entries = openAndReadEntries(entries);
        if (!num_entries)
        {
            return; // nothing to purge
        }

        // Use mTexturesSizeMap to collect UUIDs of textures with bodies
        // Sorted vector, not std::set: forming the list is O(n log n) either way, but a
        // node-per-entry set costs ~10x more at the ~1M entries a large cache holds, all
        // under mHeaderMutex (which blocks cache reads).
        typedef std::vector<std::pair<U32, S32> > time_idx_set_t;
        time_idx_set_t time_idx_set;
        time_idx_set.reserve(mTexturesSizeMap.size());
        for (size_map_t::iterator iter1 = mTexturesSizeMap.begin();
            iter1 != mTexturesSizeMap.end(); ++iter1)
        {
            if (iter1->second > 0)
            {
                id_map_t::iterator iter2 = mHeaderIDMap.find(iter1->first);
                if (iter2 != mHeaderIDMap.end())
                {
                    S32 idx = iter2->second;
                    time_idx_set.push_back(std::make_pair(entries[idx].mTime, idx));
                }
                else
                {
                    LL_ERRS("TextureCache") << "mTexturesSizeMap / mHeaderIDMap corrupted." << LL_ENDL;
                }
            }
        }

        std::sort(time_idx_set.begin(), time_idx_set.end());
        S64 cache_size = mTexturesSizeTotal;
        S64 purged_cache_size = (llmax(cache_size, sCacheMaxTexturesSize) * (S64)((1.f - TEXTURE_CACHE_PURGE_AMOUNT) * 100)) / 100;
        for (time_idx_set_t::iterator iter = time_idx_set.begin();
            iter != time_idx_set.end(); ++iter)
        {
            S32 idx = iter->second;
            if (cache_size >= purged_cache_size)
            {
                cache_size -= entries[idx].mBodySize;
                mPurgeEntryList.push_back(std::pair<S32, Entry>(idx, entries[idx]));
            }
            else
            {
                break;
            }
        }
        LL_DEBUGS("TextureCache") << "Formed Purge list of " << mPurgeEntryList.size() << " entries" << LL_ENDL;
    }
    else
    {
        // Remove collected entried
        LLTimer timer;
        while (!mPurgeEntryList.empty() && timer.getElapsedTimeF32() < time_limit_sec)
        {
            S32 idx = mPurgeEntryList.back().first;
            Entry entry = mPurgeEntryList.back().second;
            mPurgeEntryList.pop_back();
            // make sure record is still valid
            id_map_t::iterator iter_header = mHeaderIDMap.find(entry.mID);
            if (iter_header != mHeaderIDMap.end() && iter_header->second == idx)
            {
                LLCacheMetrics::recordEvict(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY,
                                            (U64)llmax(entry.mBodySize, 0) + TEXTURE_CACHE_ENTRY_SIZE);
                std::string tex_filename = getTextureFileName(entry.mID);
                removeEntry(idx, entry, tex_filename);
                writeEntryToHeaderImmediately(idx, entry);
            }
        }
    }
}

void LLTextureCache::purgeTextures(bool validate)
{
    if (mReadOnly)
    {
        return;
    }

    if (!mThreaded)
    {
        // *FIX:Mani - watchdog off.
        LLAppViewer::instance()->pauseMainloopTimeout();
    }

    LLMutexLock lock(&mHeaderMutex);

    LL_INFOS() << "TEXTURE CACHE: Purging." << LL_ENDL;

    // Read the entries list
    std::vector<Entry> entries;
    U32 num_entries = openAndReadEntries(entries);
    if (!num_entries)
    {
        return; // nothing to purge
    }

    // Use mTexturesSizeMap to collect UUIDs of textures with bodies
    typedef std::vector<std::pair<U32,S32> > time_idx_set_t;   // sorted below (see purgeTexturesLazy)
    time_idx_set_t time_idx_set;
    time_idx_set.reserve(mTexturesSizeMap.size());
    for (size_map_t::iterator iter1 = mTexturesSizeMap.begin();
         iter1 != mTexturesSizeMap.end(); ++iter1)
    {
        if (iter1->second > 0)
        {
            id_map_t::iterator iter2 = mHeaderIDMap.find(iter1->first);
            if (iter2 != mHeaderIDMap.end())
            {
                S32 idx = iter2->second;
                time_idx_set.push_back(std::make_pair(entries[idx].mTime, idx));
//              LL_INFOS() << "TIME: " << entries[idx].mTime << " TEX: " << entries[idx].mID << " IDX: " << idx << " Size: " << entries[idx].mImageSize << LL_ENDL;
            }
            else
            {
                LL_ERRS() << "mTexturesSizeMap / mHeaderIDMap corrupted." << LL_ENDL ;
            }
        }
    }

    // Validate 1/256th of the files on startup
    U32 validate_idx = 0;
    if (validate)
    {
        validate_idx = gSavedSettings.getU32("CacheValidateCounter");
        U32 next_idx = (validate_idx + 1) % 256;
        gSavedSettings.setU32("CacheValidateCounter", next_idx);
        LL_DEBUGS("TextureCache") << "TEXTURE CACHE: Validating: " << validate_idx << LL_ENDL;
    }

    std::sort(time_idx_set.begin(), time_idx_set.end());
    S64 cache_size = mTexturesSizeTotal;
    S64 purged_cache_size = (llmax(cache_size, sCacheMaxTexturesSize) * (S64)((1.f - TEXTURE_CACHE_PURGE_AMOUNT) * 100)) / 100;
    S32 purge_count = 0;
    for (time_idx_set_t::iterator iter = time_idx_set.begin();
         iter != time_idx_set.end(); ++iter)
    {
        S32 idx = iter->second;
        bool purge_entry = false;

        if (cache_size >= purged_cache_size)
        {
            purge_entry = true;
            LLCacheMetrics::recordEvict(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY,
                                        (U64)llmax(entries[idx].mBodySize, 0) + TEXTURE_CACHE_ENTRY_SIZE);
        }
        else if (validate)
        {
            // make sure file exists and is the correct size
            U32 uuididx = entries[idx].mID.mData[0];
            if (uuididx == validate_idx)
            {
                std::string filename = getTextureFileName(entries[idx].mID);
                LL_DEBUGS("TextureCache") << "Validating: " << filename << "Size: " << entries[idx].mBodySize << LL_ENDL;
                // mHeaderAPRFilePoolp because this is under header mutex in main thread
                S32 bodysize = LLAPRFile::size(filename, mHeaderAPRFilePoolp);
                if (bodysize != entries[idx].mBodySize)
                {
                    LL_WARNS("TextureCache") << "TEXTURE CACHE BODY HAS BAD SIZE: " << bodysize << " != " << entries[idx].mBodySize << filename << LL_ENDL;
                    LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY);
                    purge_entry = true;
                }
            }
        }
        else
        {
            break;
        }

        if (purge_entry)
        {
            purge_count++;
            std::string filename = getTextureFileName(entries[idx].mID);
            LL_DEBUGS("TextureCache") << "PURGING: " << filename << LL_ENDL;
            cache_size -= entries[idx].mBodySize;
            removeEntry(idx, entries[idx], filename) ;
        }
    }

    LL_DEBUGS("TextureCache") << "TEXTURE CACHE: Writing Entries: " << num_entries << LL_ENDL;

    writeEntriesAndClose(entries);

    // *FIX:Mani - watchdog back on.
    LLAppViewer::instance()->resumeMainloopTimeout();

    LL_INFOS("TextureCache") << "TEXTURE CACHE:"
            << " PURGED: " << purge_count
            << " ENTRIES: " << num_entries
            << " CACHE SIZE: " << mTexturesSizeTotal / (1024 * 1024) << " MB"
            << LL_ENDL;
}

//////////////////////////////////////////////////////////////////////////////

// call lockWorkers() first!
LLTextureCacheWorker* LLTextureCache::getReader(handle_t handle)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureCacheWorker* res = NULL;
    handle_map_t::iterator iter = mReaders.find(handle);
    if (iter != mReaders.end())
    {
        res = iter->second;
    }
    return res;
}

LLTextureCacheWorker* LLTextureCache::getWriter(handle_t handle)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureCacheWorker* res = NULL;
    handle_map_t::iterator iter = mWriters.find(handle);
    if (iter != mWriters.end())
    {
        res = iter->second;
    }
    return res;
}

//////////////////////////////////////////////////////////////////////////////
// Called from work thread

// Reads imagesize from the header, updates timestamp
S32 LLTextureCache::getHeaderCacheEntry(const LLUUID& id, Entry& entry)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLMutexLock lock(&mHeaderMutex);
    S32 idx = openAndReadEntry(id, entry, false);
    if (idx >= 0)
    {
        updateEntryTimeStamp(idx, entry); // updates time
    }
    return idx;
}

// Writes imagesize to the header, updates timestamp
S32 LLTextureCache::setHeaderCacheEntry(const LLUUID& id, Entry& entry, S32 imagesize, S32 datasize)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    mHeaderMutex.lock();
    S32 idx = openAndReadEntry(id, entry, true); // read or create
    mHeaderMutex.unlock();

    if(idx < 0) // retry once
    {
        readHeaderCache(); // We couldn't write an entry, so refresh the LRU

        mHeaderMutex.lock();
        idx = openAndReadEntry(id, entry, true);
        mHeaderMutex.unlock();
    }

    if (idx >= 0)
    {
        updateEntry(idx, entry, imagesize, datasize);
    }
    else
    {
        LL_WARNS() << "Failed to set cache entry for image: " << id << LL_ENDL;
        // We couldn't write to file, switch to read only mode and clear data
        setReadOnly(true);
        clearCorruptedCache(); // won't remove files due to "read only"
    }

    return idx;
}

//////////////////////////////////////////////////////////////////////////////

// Calls from texture pipeline thread (i.e. LLTextureFetch)

LLTextureCache::handle_t LLTextureCache::readFromCache(const std::string& filename, const LLUUID& id,
                                                       S32 offset, S32 size, ReadResponder* responder)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Note: checking to see if an entry exists can cause a stall,
    //  so let the thread handle it
    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheLocalFileWorker(this, filename, id,
                                                                     NULL, size, offset, 0,
                                                                     responder);
    handle_t handle = worker->read();
    mReaders[handle] = worker;
    return handle;
}

LLTextureCache::handle_t LLTextureCache::readFromCache(const LLUUID& id,
                                                       S32 offset, S32 size, ReadResponder* responder)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Note: checking to see if an entry exists can cause a stall,
    //  so let the thread handle it
    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheRemoteWorker(this, id,
                                                                  NULL, size, offset,
                                                                  0, NULL, 0, responder);
    handle_t handle = worker->read();
    mReaders[handle] = worker;
    return handle;
}


bool LLTextureCache::readComplete(handle_t handle, bool abort)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    lockWorkers();
    handle_map_t::iterator iter = mReaders.find(handle);
    LLTextureCacheWorker* worker = NULL;
    bool complete = false;
    if (iter != mReaders.end())
    {
        worker = iter->second;
        complete = worker->complete();

        if(!complete && abort)
        {
            abortRequest(handle, true) ;
        }
    }
    if (worker && (complete || abort))
    {
        mReaders.erase(iter);
        unlockWorkers();
        worker->scheduleDelete();
    }
    else
    {
        unlockWorkers();
    }
    return (complete || abort);
}

LLTextureCache::handle_t LLTextureCache::writeToCache(const LLUUID& id,
                                                      const U8* data, S32 datasize, S32 imagesize,
                                                      LLPointer<LLImageRaw> rawimage, S32 discardlevel,
                                                      WriteResponder* responder)
{
    // A second instance (mReadOnly) writes nothing to the legacy files; with the store
    // backend its writes go to the store's private overflow (discarded at exit).
    if (mReadOnly && !mUsingStore)
    {
        delete responder;
        return LLWorkerThread::nullHandle();
    }
    if (mDoPurge && !mUsingStore)
    {
        // NOTE: Needs to be done on the control thread
        //  (i.e. here)
        purgeTexturesLazy(TEXTURE_LAZY_PURGE_TIME_LIMIT);
        mDoPurge = !mPurgeEntryList.empty();
    }
    LLMutexLock lock(&mWorkersMutex);
    LLTextureCacheWorker* worker = new LLTextureCacheRemoteWorker(this, id,
                                                                  data, datasize, 0,
                                                                  imagesize, rawimage, discardlevel, responder);
    handle_t handle = worker->write();
    mWriters[handle] = worker;
    return handle;
}

bool LLTextureCache::writeComplete(handle_t handle, bool abort)
{
    lockWorkers();
    handle_map_t::iterator iter = mWriters.find(handle);
    llassert(iter != mWriters.end());
    if (iter != mWriters.end())
    {
        LLTextureCacheWorker* worker = iter->second;
        if (worker->complete() || abort)
        {
            mWriters.erase(handle);
            unlockWorkers();
            worker->scheduleDelete();
            return true;
        }
    }
    unlockWorkers();
    return false;
}

void LLTextureCache::prioritizeWrite(handle_t handle)
{
    // Don't prioritize yet, we might be working on this now
    //   which could create a deadlock
    LLMutexLock lock(&mListMutex);
    mPrioritizeWriteList.push_back(handle);
}

void LLTextureCache::addCompleted(Responder* responder, bool success)
{
    LLMutexLock lock(&mListMutex);
    Completed c;
    c.responder = responder;
    c.success = success;
    mCompletedList.push_back(c);
}

// #715 metrics. Main thread. The legacy cache on disk = texture.entries (header + one
// Entry per slot) + texture.cache (one TEXTURE_CACHE_ENTRY_SIZE stripe per slot) + one
// body file per texture longer than the stripe; live payload = the cached prefixes.
void LLTextureCache::sampleLegacyFootprint()
{
    if (mReadOnly || mUsingStore || !mHeaderMutex.trylock())
    {
        return;
    }
    const U64 slots = mHeaderEntriesInfo.mEntries;
    const U64 live_entries = mHeaderIDMap.size();
    U64 bodies = 0;
    for (const auto& kv : mTexturesSizeMap)
    {
        if (kv.second > 0) ++bodies;
    }
    const U64 body_bytes = (U64)llmax(mTexturesSizeTotal, (S64)0);
    mHeaderMutex.unlock();
    const U64 disk = body_bytes + slots * (U64)TEXTURE_CACHE_ENTRY_SIZE + sizeof(EntriesInfo) + slots * sizeof(Entry);
    const U64 live = body_bytes + live_entries * (U64)TEXTURE_CACHE_ENTRY_SIZE;
    LLCacheMetrics::setFootprint(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::LEGACY, disk, live,
                                 bodies + (slots ? 2 : 0));
}

//////////////////////////////////////////////////////////////////////////////

//called after mHeaderMutex is locked.
void LLTextureCache::removeCachedTexture(const LLUUID& id)
{
    if(mTexturesSizeMap.find(id) != mTexturesSizeMap.end())
    {
        mTexturesSizeTotal -= mTexturesSizeMap[id] ;
        mTexturesSizeMap.erase(id);
    }
    mHeaderIDMap.erase(id);
    // We are inside header's mutex so mHeaderAPRFilePoolp is safe to use,
    // but getLocalAPRFilePool() is not safe, it might be in use by worker
    LLAPRFile::remove(getTextureFileName(id), mHeaderAPRFilePoolp);
}

//called after mHeaderMutex is locked.
void LLTextureCache::removeEntry(S32 idx, Entry& entry, std::string& filename)
{
    bool file_maybe_exists = true;  // Always attempt to remove when idx is invalid.

    if(idx >= 0) //valid entry
    {
        if (entry.mBodySize == 0)   // Always attempt to remove when mBodySize > 0.
        {
          // Sanity check. Shouldn't exist when body size is 0.
          // We are inside header's mutex so mHeaderAPRFilePoolp is safe to use,
          // but getLocalAPRFilePool() is not safe, it might be in use by worker
          if (LLAPRFile::isExist(filename, mHeaderAPRFilePoolp))
          {
              LL_WARNS("TextureCache") << "Entry has body size of zero but file " << filename << " exists. Deleting this file, too." << LL_ENDL;
          }
          else
          {
              file_maybe_exists = false;
          }
        }
        mTexturesSizeTotal -= entry.mBodySize;

        entry.mImageSize = -1;
        entry.mBodySize = 0;
        mHeaderIDMap.erase(entry.mID);
        mTexturesSizeMap.erase(entry.mID);
        mFreeList.insert(idx);
    }

    if (file_maybe_exists)
    {
        LLAPRFile::remove(filename, mHeaderAPRFilePoolp);
    }
}

bool LLTextureCache::removeFromCache(const LLUUID& id)
{
    //LL_WARNS() << "Removing texture from cache: " << id << LL_ENDL;
    if (mUsingStore)
    {
        return storeRemove(id);
    }
    bool ret = false ;
    if (!mReadOnly)
    {
        lockHeaders() ;

        Entry entry;
        S32 idx = openAndReadEntry(id, entry, false);
        std::string tex_filename = getTextureFileName(id);
        removeEntry(idx, entry, tex_filename) ;
        if (idx >= 0)
        {
            writeEntryToHeaderImmediately(idx, entry);
            ret = true;
        }

        unlockHeaders() ;
    }
    return ret ;
}

//////////////////////////////////////////////////////////////////////////////

LLTextureCache::ReadResponder::ReadResponder()
    : mImageSize(0),
      mImageLocal(false)
{
}

void LLTextureCache::ReadResponder::setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
{
    if (mFormattedImage.notNull())
    {
        llassert_always(mFormattedImage->getCodec() == imageformat);
        mFormattedImage->appendData(data, datasize);
    }
    else
    {
        mFormattedImage = LLImageFormatted::createFromType(imageformat);
        mFormattedImage->setData(data,datasize);
    }
    mImageSize = imagesize;
    mImageLocal = imagelocal;
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// #715 STORE backend

namespace
{
    constexpr std::chrono::milliseconds STORE_READY_WAIT(10000);
}

// static
std::string LLTextureCache::legacyDir(ELLPath location)
{
    return gDirUtilp->getExpandedFilename(location, textures_dirname);
}

void LLTextureCache::detachStore()
{
    mStoreAttached = false;
    setLegacySource(nullptr);
    logStoreStats("shutdown");
    std::unique_lock<std::shared_mutex> lock(mStoreMutex);   // waits for store calls in flight
    mStore = nullptr;
}

void LLTextureCache::setLegacySource(const std::shared_ptr<SLJ2CMigrate::Legacy>& legacy)
{
    std::lock_guard<std::mutex> lock(mLegacySourceMutex);
    mLegacySource = legacy;
}

S64Bytes LLTextureCache::getUsage()
{
    if (mUsingStore)
    {
        std::shared_lock<std::shared_mutex> lock(mStoreMutex);
        return S64Bytes(mStore ? (S64)mStore->stats().kinds[(size_t)LLCache::Kind::J2C].live_bytes : 0);
    }
    return S64Bytes(mTexturesSizeTotal);
}

S64Bytes LLTextureCache::getMaxUsage()
{
    if (mUsingStore)
    {
        return S64Bytes((S64)SLTextureTiers::instance().j2cBudget());
    }
    return S64Bytes(sCacheMaxTexturesSize);
}

U32 LLTextureCache::getEntries()
{
    if (mUsingStore)
    {
        std::shared_lock<std::shared_mutex> lock(mStoreMutex);
        return mStore ? mStore->stats().kinds[(size_t)LLCache::Kind::J2C].records : 0;
    }
    return mHeaderEntriesInfo.mEntries;
}

S32 LLTextureCache::storeRead(const LLUUID& id, S32 offset, S32 size, U8** data, S32& imagesize, bool* from_legacy)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    *data = nullptr;
    *from_legacy = false;
    if (offset < 0 || size <= 0)
    {
        return 0;
    }
    std::shared_lock<std::shared_mutex> lock(mStoreMutex);
    LLCache::Store* store = mStore;
    if (!store)
    {
        return 0;
    }
    if (!store->ready() && !store->waitReady(STORE_READY_WAIT) && !mStoreNotReadyLogged.exchange(true))
    {
        // The legacy cache loaded its index synchronously at startup; the store loads it on
        // its writer thread. Waiting here (cache thread only) keeps login reads hits.
        LL_WARNS("TextureCache") << "Cache store index still loading after " << STORE_READY_WAIT.count()
                                 << " ms: J2C reads miss until it is ready" << LL_ENDL;
    }
    const LLCache::Key key = LLJ2CRecord::makeKey(id);
    LLCache::RecordInfo info;
    if (!store->lookup(key, &info))
    {
        // Not in the store yet: the legacy cache may still hold it (migration pending).
        std::shared_ptr<SLJ2CMigrate::Legacy> legacy;
        {
            std::lock_guard<std::mutex> llock(mLegacySourceMutex);
            legacy = mLegacySource;
        }
        std::vector<U8> full;
        S32 legacy_imagesize = 0;
        if (!legacy || !legacy->read(id, full, legacy_imagesize) || (S32)full.size() <= offset)
        {
            return 0;
        }
        // Promote it into the store now (the migration then finds the key and drops the
        // legacy files when it gets there), and serve the range from the bytes in hand.
        if (!store->readOnly())
        {
            U8 meta[LLJ2CRecord::META_BYTES];
            LLCache::WriteOptions o;
            o.meta_len = LLJ2CRecord::packMeta(legacy_imagesize, meta);
            o.meta = meta;
            o.flags = LLCache::WF_SKIP_IF_NOT_LARGER;
            o.placement = SLTextureTiers::instance().placement();
            store->write(key, LLCache::WriteSource::copy(full.data(), full.size()), o);
        }
        const S32 n = llmin(size, (S32)full.size() - offset);
        U8* buf = (U8*)ll_aligned_malloc_16(n);
        if (!buf)
        {
            return -1;
        }
        memcpy(buf, full.data() + offset, n);
        *data = buf;
        imagesize = legacy_imagesize;
        *from_legacy = true;
        ++mReadThroughHits;
        return n;
    }
    S32 stored_imagesize = 0;
    if (!LLJ2CRecord::imageSizeFromRecord(info, stored_imagesize) || stored_imagesize <= 0)
    {
        // A meta version this build does not understand (or garbage): never served.
        LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::STORE);
        store->remove(key);
        return 0;
    }
    if ((U64)offset >= (U64)info.raw_bytes)
    {
        return 0;   // nothing cached past what the fetcher already holds (legacy: a failed read)
    }
    LLCache::ReadResult r = store->readRangeSync(key, (U64)offset, (U64)size);
    if (r.status == LLCache::Status::OK)
    {
        if (r.bytes.empty())
        {
            return 0;
        }
        const S32 n = (S32)llmin(r.bytes.size(), (size_t)size);
        U8* buf = (U8*)ll_aligned_malloc_16(n);
        if (!buf)
        {
            return -1;
        }
        memcpy(buf, r.bytes.data(), n);
        *data = buf;
        imagesize = stored_imagesize;
        return n;
    }
    if (r.status == LLCache::Status::CORRUPT)
    {
        // The store already dropped it from its index.
        LLCacheMetrics::recordCorrupt(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::STORE);
        return 0;
    }
    if (r.status == LLCache::Status::MISSING || r.status == LLCache::Status::CANCELLED)
    {
        return 0;
    }
    return -1;   // I/O error: endWork() drops it (never trusted again this session)
}

bool LLTextureCache::storeWrite(const LLUUID& id, const U8* data, S32 datasize, S32 imagesize)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!data || datasize <= 0)
    {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(mStoreMutex);
    LLCache::Store* store = mStore;
    if (!store)
    {
        return false;
    }
    if (!store->ready())
    {
        store->waitReady(STORE_READY_WAIT);   // lookups below must see the loaded index
    }
    const LLCache::Key key = LLJ2CRecord::makeKey(id);
    U32 flags = LLCache::WF_NONE;
    LLCache::RecordInfo info;
    if (store->lookup(key, &info) && !info.staged)
    {
        S32 stored_imagesize = 0;
        const bool meta_ok = LLJ2CRecord::imageSizeFromRecord(info, stored_imagesize);
        const bool same_image_size = meta_ok && stored_imagesize == imagesize;
        // A longer prefix of the same codestream is already there (the fetcher re-sent a
        // shorter one after reading part of the cached bytes back): keep the longer one.
        if (meta_ok && (S64)info.raw_bytes > (S64)datasize)
        {
            ++mStoreWritesSkipped;
            return true;
        }
        if (same_image_size && (S64)info.raw_bytes == (S64)datasize)
        {
            ++mStoreWritesSkipped;   // the legacy alreadyCached case
            return true;
        }
        if (same_image_size)
        {
            flags |= LLCache::WF_SKIP_IF_NOT_LARGER;   // a racing longer write wins
        }
    }

    U8 meta[LLJ2CRecord::META_BYTES];
    LLCache::WriteOptions o;
    o.meta_len = LLJ2CRecord::packMeta(imagesize, meta);
    o.meta = meta;
    o.flags = flags;
    o.priority = LLCache::Priority::NORMAL;
    o.placement = SLTextureTiers::instance().placement();
    // Metric: submit -> appended (or refused), on the store's thread. `wake` runs inline
    // there exactly once, also for an immediate refusal inside write().
    auto done = std::make_shared<LLCache::Ticket<LLCache::Status>>();
    const U64 start = totalTime();
    const U64 payload = (U64)datasize;
    o.completion.wake = [done, start, payload]()
    {
        const LLCache::Status st = done->take();
        if (st == LLCache::Status::SKIPPED)
        {
            return;   // a longer prefix got there first: not a write
        }
        const U64 now = totalTime();
        const bool ok = st == LLCache::Status::OK;
        LLCacheMetrics::recordWrite(LLCacheMetrics::Cache::J2C, LLCacheMetrics::Backend::STORE, payload,
                                    ok ? LLJ2CRecord::diskBytes(payload) : 0, now > start ? now - start : 0, ok);
    };
    const bool queued = store->write(key, LLCache::WriteSource::copy(data, (size_t)datasize), o, done.get());
    if (!queued)
    {
        ++mStoreWritesRefused;
    }
    return queued;
}

bool LLTextureCache::storeRemove(const LLUUID& id)
{
    bool removed = false;
    {
        std::shared_lock<std::shared_mutex> lock(mStoreMutex);
        if (mStore)
        {
            // A second instance only hides it from itself (and drops its overflow copy).
            removed = mStore->remove(LLJ2CRecord::makeKey(id));
        }
    }
    if (!mReadOnly)
    {
        // Found bad this session: the legacy copy (not migrated yet) must not come back.
        std::shared_ptr<SLJ2CMigrate::Legacy> legacy;
        {
            std::lock_guard<std::mutex> llock(mLegacySourceMutex);
            legacy = mLegacySource;
        }
        if (legacy)
        {
            legacy->forget(id);
        }
    }
    return removed;
}

void LLTextureCache::logStoreStats(const char* when)
{
    if (!mUsingStore)
    {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(mStoreMutex);
    if (!mStore)
    {
        return;
    }
    const LLCache::Stats s = mStore->stats();
    const LLCache::KindStats& k = s.kinds[(size_t)LLCache::Kind::J2C];
    LL_INFOS("TextureCache") << "J2C texture cache (store, " << when << "): " << k.records << " records, "
                             << llformat("%.1f", (F64)k.live_bytes / (1024.0 * 1024.0)) << " MB of "
                             << (SLTextureTiers::instance().j2cBudget() >> 20) << " MB; legacy read-through hits "
                             << mReadThroughHits.load() << ", writes skipped (not longer) " << mStoreWritesSkipped.load()
                             << ", refused " << mStoreWritesRefused.load() << ", duplicate J2C (GPU-ready full size) "
                             << SLTextureTiers::instance().duplicateCandidates() << LL_ENDL;
}
