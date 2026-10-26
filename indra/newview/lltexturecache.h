/**
 * @file lltexturecache.h
 * @brief Object for managing texture cachees.
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

#ifndef LL_LLTEXTURECACHE_H
#define LL_LLTEXTURECACHE_H

#include "lldir.h"
#include "llstl.h"
#include "llstring.h"
#include "lluuid.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "llfile.h"

#include "llworkerthread.h"

class LLImageFormatted;
class LLTextureCacheWorker;
class LLImageRaw;
namespace LLCache { class Store; }
namespace SLJ2CMigrate { class Legacy; }

// Two backends behind one contract (#715):
// - LEGACY: texture.entries + texture.cache (600-byte header stripe per texture) +
//   [0-f]/<uuid>.texture bodies in <cache>/texturecache (LL's design, below).
// - STORE (CacheStoreJ2C, default): one J2C record per texture in the segment store
//   (llj2crecord.h), chosen by passing an open store to initCache(). The worker thread,
//   handles, responders, readFromCache/writeToCache offsets and sizes, header-vs-body
//   semantics as seen by LLTextureFetch (a prefix of the codestream + the image size),
//   removeFromCache and purgeCache stay as they are; only doRead/doWrite change.
//   Reads are one range read (pread at the offset); a longer prefix is written as a new
//   record that supersedes the shorter one (see storeWrite()).

class LLTextureCache : public LLWorkerThread
{
    friend class LLTextureCacheWorker;
    friend class LLTextureCacheRemoteWorker;
    friend class LLTextureCacheLocalFileWorker;

private:

#if LL_WINDOWS
#pragma pack(push,1)
#endif

    // Entries
    static const U32 sHeaderEncoderStringSize = 32;
    struct EntriesInfo
    {
        EntriesInfo() : mVersion(0.f), mAdressSize(0), mEntries(0) { memset(mEncoderVersion, 0, sHeaderEncoderStringSize); }
        F32 mVersion;
        U32 mAdressSize;
        char mEncoderVersion[sHeaderEncoderStringSize];
        U32 mEntries;
    };
    struct Entry
    {
            Entry() :
                mBodySize(0),
            mImageSize(0),
            mTime(0)
        {
        }
        Entry(const LLUUID& id, S32 imagesize, S32 bodysize, U32 time) :
            mID(id), mImageSize(imagesize), mBodySize(bodysize), mTime(time) {}
        void init(const LLUUID& id, U32 time) { mID = id, mImageSize = 0; mBodySize = 0; mTime = time; }
        LLUUID mID; // 16 bytes
        S32 mImageSize; // total size of image if known
        S32 mBodySize; // size of body file in body cache
        U32 mTime; // seconds since 1/1/1970
    };

#if LL_WINDOWS
#pragma pack(pop)
#endif

public:

    class Responder : public LLResponder
    {
    public:
        virtual void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal) = 0;
    };

    class ReadResponder : public Responder
    {
    public:
        ReadResponder();
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal);
        void setImage(LLImageFormatted* image) { mFormattedImage = image; }
    protected:
        LLPointer<LLImageFormatted> mFormattedImage;
        S32 mImageSize;
        bool mImageLocal;
    };

    class WriteResponder : public Responder
    {
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
        {
            // not used
        }
    };

    LLTextureCache(bool threaded);
    ~LLTextureCache();

    /*virtual*/ size_t update(F32 max_time_ms);

    void purgeCache(ELLPath location, bool remove_dir = true);
    void setReadOnly(bool read_only) ;
    // store: an open segment store -> the STORE backend (the legacy files are left to the
    // migration, slj2cmigrate.h); null -> the legacy files, as before.
    S64 initCache(ELLPath location, S64 maxsize, bool texture_cache_mismatch, LLCache::Store* store = nullptr);
    bool usingStore() const { return mUsingStore; }
    // Before the store closes (shutdown): waits for any store call in flight; from then on
    // reads miss and writes are refused. Safe when the store backend is not in use.
    void detachStore();
    // STORE backend read-through to the not yet migrated legacy cache (null = none).
    void setLegacySource(const std::shared_ptr<SLJ2CMigrate::Legacy>& legacy);
    // The legacy folder (<cache>/texturecache) for `location`.
    static std::string legacyDir(ELLPath location);

    handle_t readFromCache(const std::string& local_filename, const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);

    handle_t readFromCache(const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);
    bool readComplete(handle_t handle, bool abort);
    handle_t writeToCache(const LLUUID& id, const U8* data, S32 datasize, S32 imagesize, LLPointer<LLImageRaw> rawimage, S32 discardlevel,
                          WriteResponder* responder);
    bool writeComplete(handle_t handle, bool abort = false);
    void prioritizeWrite(handle_t handle);

    bool removeFromCache(const LLUUID& id);

    // For LLTextureCacheWorker::Responder
    LLTextureCacheWorker* getReader(handle_t handle);
    LLTextureCacheWorker* getWriter(handle_t handle);
    void lockWorkers() { mWorkersMutex.lock(); }
    void unlockWorkers() { mWorkersMutex.unlock(); }

    // debug
    S32 getNumReads() { return static_cast<S32>(mReaders.size()); }
    S32 getNumWrites() { return static_cast<S32>(mWriters.size()); }
    S64Bytes getUsage();
    S64Bytes getMaxUsage();
    U32 getEntries();
    U32 getMaxEntries() { return sCacheMaxEntries; };
    // STORE backend counters (0 on the legacy backend)
    U64 getReadThroughHits() const { return mReadThroughHits.load(); }
    // #951 texture pipeline diagnostics: store reads by outcome, writes queued / refused /
    // skipped, and whether the store's index is loaded (-1 = legacy backend, 0 = still
    // loading, 1 = ready, 2 = detached or its lock busy).
    U64 getStoreReadHits() const { return mStoreReadHits.load(std::memory_order_relaxed); }
    U64 getStoreReadMisses() const { return mStoreReadMisses.load(std::memory_order_relaxed); }
    U64 getStoreReadErrors() const { return mStoreReadErrors.load(std::memory_order_relaxed); }
    U64 getStoreWritesQueued() const { return mStoreWritesQueued.load(std::memory_order_relaxed); }
    U64 getStoreWritesRefused() const { return mStoreWritesRefused.load(std::memory_order_relaxed); }
    U64 getStoreWritesSkipped() const { return mStoreWritesSkipped.load(std::memory_order_relaxed); }
    S32 getStoreState();
    void logStoreStats(const char* when);
    // #300 counters: RAM held by the entry mirror; entry lookups served from it vs from disk
    size_t getEntriesRAMBytes() const { return mEntriesBytes; }
    U64 getEntryRAMHits() const { return mEntryRAMHits; }
    U64 getEntryDiskReads() const { return mEntryDiskReads; }
    bool isInCache(const LLUUID& id) ;
    bool isInLocal(const LLUUID& id) ; //not thread safe at the moment

protected:
    // Accessed by LLTextureCacheWorker
    std::string getLocalFileName(const LLUUID& id);
    std::string getTextureFileName(const LLUUID& id);

    // STORE backend (worker thread). storeRead: > 0 = bytes read into a new
    // ll_aligned_malloc_16 buffer (*data; the responder takes it), 0 = not cached / nothing
    // at that offset, -1 = I/O failure (the worker's endWork then removes the entry).
    // *from_legacy = served by the legacy read-through (metrics: a LEGACY read).
    S32 storeRead(const LLUUID& id, S32 offset, S32 size, U8** data, S32& imagesize, bool* from_legacy);
    // false = refused / failed (any older record stands; nothing is removed).
    bool storeWrite(const LLUUID& id, const U8* data, S32 datasize, S32 imagesize);
    bool storeRemove(const LLUUID& id);

    void addCompleted(Responder* responder, bool success);

protected:
    //void setFileAPRPool(apr_pool_t* pool) { mFileAPRPool = pool ; }

private:
    void setDirNames(ELLPath location);
    void readHeaderCache();
    void clearCorruptedCache();
    void purgeAllTextures(bool purge_directories);
    void purgeTexturesLazy(F32 time_limit_sec);
    void purgeTextures(bool validate);
    LLAPRFile* openHeaderEntriesFile(bool readonly, S32 offset);
    void closeHeaderEntriesFile();
    void readEntriesHeader();
    void setEntriesHeader();
    void writeEntriesHeader();
    S32 openAndReadEntry(const LLUUID& id, Entry& entry, bool create);
    bool updateEntry(S32& idx, Entry& entry, S32 new_image_size, S32 new_body_size);
    void updateEntryTimeStamp(S32 idx, Entry& entry) ;
    U32 openAndReadEntries(std::vector<Entry>& entries);
    void writeEntriesAndClose(const std::vector<Entry>& entries);
    void readEntryFromHeaderImmediately(S32& idx, Entry& entry) ;
    void writeEntryToHeaderImmediately(S32& idx, Entry& entry, bool write_header = false) ;
    void removeEntry(S32 idx, Entry& entry, std::string& filename);
    void removeCachedTexture(const LLUUID& id) ;
    S32 getHeaderCacheEntry(const LLUUID& id, Entry& entry);
    S32 setHeaderCacheEntry(const LLUUID& id, Entry& entry, S32 imagesize, S32 datasize);
    void writeUpdatedEntries() ;
    void updatedHeaderEntriesFile() ;
    void lockHeaders() { mHeaderMutex.lock(); }
    void unlockHeaders() { mHeaderMutex.unlock(); }
    // #715 metrics: legacy footprint (bytes / files) from the in-RAM index, no directory
    // walk. Main thread; skipped when the header mutex is busy.
    void sampleLegacyFootprint();

    // #300: texture.cache (the first TEXTURE_CACHE_ENTRY_SIZE bytes of every cached
    // texture) held open for the session instead of opened, seeked and closed for every
    // header read and write. Positional I/O under mHeaderDataMutex; every write is
    // flushed. Closed (under the same mutex) before anything deletes or renames it.
    // Returns bytes transferred (0 on failure).
    S32 readHeaderData(S32 offset, U8* buf, S32 size);
    S32 writeHeaderData(S32 offset, const U8* buf, S32 size);
    void closeHeaderDataFile();
    // #300: the in-RAM copy of texture.entries. Locks: mHeaderMutex.
    void mirrorEntry(S32 idx, const Entry& entry);
    void mirrorEntries(const std::vector<Entry>& entries);
    // #300: texture.entries held open for single-entry writes. Locks: mHeaderMutex.
    S32 writeEntriesFile(S32 offset, const U8* buf, S32 size);
    void closeEntriesFile();

private:
    // Internal
    LLMutex mWorkersMutex;
    LLMutex mHeaderMutex;
    LLMutex mListMutex;
    LLAPRFile* mHeaderAPRFile;

    // #300 (see readHeaderData). Lock order: mHeaderMutex -> mHeaderDataMutex.
    LLMutex mHeaderDataMutex;
    LLFILE* mHeaderDataFile = nullptr;      // under mHeaderDataMutex
    bool mHeaderDataWritable = false;       // under mHeaderDataMutex
    LLFILE* mEntriesFile = nullptr;         // #300: texture.entries, under mHeaderMutex

    // #300: mirror of the entries in texture.entries (as written; pending timestamp
    // updates stay in mUpdatedEntryMap), so a cache lookup no longer opens texture.entries
    // and reads 28 bytes from it. Under mHeaderMutex. Bounded by sCacheMaxEntries
    // (28 bytes each: ~28 MB at the 1M-entry cap, a few MB for a typical cache).
    std::vector<Entry> mEntries;
    std::atomic<size_t> mEntriesBytes{ 0 };
    std::atomic<U64> mEntryRAMHits{ 0 };
    std::atomic<U64> mEntryDiskReads{ 0 };

    // mLocalAPRFilePoolp is not thread safe and is meant only for workers
    // howhever mHeaderEntriesFileName is accessed not from workers' threads
    // so it needs own pool (not thread safe by itself, relies onto header's mutex)
    LLVolatileAPRPool*   mHeaderAPRFilePoolp;

    typedef std::map<handle_t, LLTextureCacheWorker*> handle_map_t;
    handle_map_t mReaders;
    handle_map_t mWriters;

    typedef std::vector<handle_t> handle_list_t;
    handle_list_t mPrioritizeWriteList;

    struct Completed
    {
        LLPointer<Responder> responder;
        bool success = false;
    };
    typedef std::vector<Completed> responder_list_t;
    responder_list_t mCompletedList;

    bool mReadOnly;

    // STORE backend (#715). mStore is guarded by mStoreMutex: shared for every store call,
    // exclusive in detachStore(). mUsingStore is fixed by initCache().
    bool mUsingStore = false;
    std::atomic<bool> mStoreAttached{ false };   // cleared by detachStore() (main-thread checks)
    std::shared_mutex mStoreMutex;
    LLCache::Store* mStore = nullptr;
    std::mutex mLegacySourceMutex;
    std::shared_ptr<SLJ2CMigrate::Legacy> mLegacySource;
    std::atomic<U64> mReadThroughHits{ 0 };
    std::atomic<U64> mStoreWritesSkipped{ 0 };
    std::atomic<U64> mStoreWritesRefused{ 0 };
    std::atomic<U64> mStoreReadHits{ 0 };       // #951
    std::atomic<U64> mStoreReadMisses{ 0 };     // #951
    std::atomic<U64> mStoreReadErrors{ 0 };     // #951
    std::atomic<U64> mStoreWritesQueued{ 0 };   // #951
    std::atomic<bool> mStoreNotReadyLogged{ false };

    // HEADERS (Include first mip)
    std::string mHeaderEntriesFileName;
    std::string mHeaderDataFileName;
    EntriesInfo mHeaderEntriesInfo;
    std::set<S32> mFreeList; // deleted entries
    std::set<LLUUID> mLRU;
    std::deque<LLUUID> mLRUOrder; // mLRU ids, oldest first; ids no longer in mLRU are skipped
    // Hashed: every cache read/write looks up here under mHeaderMutex, and a 100+ GB
    // cache holds up to sCacheMaxEntries (~1M) entries. Nothing iterates it in order.
    typedef std::unordered_map<LLUUID, S32> id_map_t;
    id_map_t mHeaderIDMap;

    // BODIES (TEXTURES minus headers)
    std::string mTexturesDirName;
    typedef std::unordered_map<LLUUID,S32> size_map_t;   // (hashed, see id_map_t)
    size_map_t mTexturesSizeMap;
    S64 mTexturesSizeTotal;
    LLAtomicBool mDoPurge;

    typedef std::map<S32, Entry> idx_entry_map_t;
    idx_entry_map_t mUpdatedEntryMap;
    typedef std::vector<std::pair<S32, Entry> > idx_entry_vector_t;
    idx_entry_vector_t mPurgeEntryList;

    // Statics
    static F32 sHeaderCacheVersion;
    static U32 sHeaderCacheAddressSize;
    static std::string sHeaderCacheEncoderVersion;
    static U32 sCacheMaxEntries;
    static S64 sCacheMaxTexturesSize;
};

extern const S32 TEXTURE_CACHE_ENTRY_SIZE;

#endif // LL_LLTEXTURECACHE_H
