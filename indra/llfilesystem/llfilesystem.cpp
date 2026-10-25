/**
 * @file filesystem.h
 * @brief Simulate local file system operations.
 * @Note The initial implementation does actually use standard C++
 *       file operations but eventually, there will be another
 *       layer that caches and manages file meta data too.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#include "linden_common.h"

#include "lldir.h"
#include "llfilesystem.h"
#include "llfasttimer.h"
#include "lldiskcache.h"
#include "llcachemetrics.h"
#include "llfilesystemstore.h"

#include "boost/filesystem.hpp"

#include <chrono>

#if LL_WINDOWS
#include <io.h>         // _chsize_s, _fileno
#else
#include <unistd.h>     // ftruncate, fileno
#endif

constexpr S32 LLFileSystem::READ        = 0x00000001;
constexpr S32 LLFileSystem::WRITE       = 0x00000002;
constexpr S32 LLFileSystem::READ_WRITE  = 0x00000003;  // LLFileSystem::READ & LLFileSystem::WRITE
constexpr S32 LLFileSystem::APPEND      = 0x00000006;  // 0x00000004 & LLFileSystem::WRITE

static LLTrace::BlockTimerStatHandle FTM_VFILE_WAIT("VFile Wait");

// #301: every LLFileSystem call used to build the path and open the file again (a
// typical "construct, getSize, read" was two stats, two opens and a seek-to-end). Now
// an object opens its file once and keeps the handle until it is destroyed (or
// renamed / removed); sizes come from the open handle or a single stat().
//
// Visibility: every write is flushed before write() returns, so another LLFileSystem
// (or thread) reading the same cache file sees it exactly as before.

namespace
{
    // Size of an open stream (flushed by the caller). Moves the stream position, which
    // every operation here sets explicitly before use.
    S32 stream_size(LLFILE* f)
    {
        if (fseek(f, 0, SEEK_END) != 0)
        {
            return 0;
        }
        const long sz = ftell(f);
        return sz > 0 ? (S32)llmin((S64)sz, (S64)S32_MAX) : 0;
    }

    U64 now_usec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    S32 path_size(const std::string& filename)
    {
        llstat st;
        if (LLFile::stat(filename, &st) != 0)
        {
            return 0;
        }
        return st.st_size > 0 ? (S32)llmin((S64)st.st_size, (S64)S32_MAX) : 0;
    }
}

LLFileSystem::LLFileSystem(const LLUUID& file_id, const LLAssetType::EType file_type, S32 mode)
{
    mFileType = file_type;
    mFileID = file_id;
    mPosition = 0;
    mBytesRead = 0;
    mMode = mode;
    mOpenUsec = now_usec();
    // #715: the cache store when the viewer attached one (CacheStoreAssets), else the files.
    mUseStore = LLFileSystemStore::active();

    // This block of code was originally called in the read() method but after comments here:
    // https://bitbucket.org/lindenlab/viewer/commits/e28c1b46e9944f0215a13cab8ee7dded88d7fc90#comment-10537114
    // we decided to follow Henri's suggestion and move the code to update the last access time here.
    if (mode == LLFileSystem::READ && !mUseStore)
    {
        // (#715: the store keeps access times in its index; a read never writes the disk.)
        // update the last access time for the file if it exists - this is required
        // even though we are reading and not writing because this is the
        // way the cache works - it relies on a valid "last accessed time" for
        // each file so it knows how to remove the oldest, unused files
        // (#301: updateFileAccessTime() does the existence check with its one stat)
        updateFileAccessTime(filename());
    }
}

LLFileSystem::~LLFileSystem()
{
    if (mStoreWriter)
    {
        // A WRITE / READ_WRITE object owned the whole content: commit it now if it was the
        // last writer. APPEND staging (one object per received packet) waits for rename()
        // or the idle grace (LLFileSystemStore::pump()).
        storeEndWrite(mMode != APPEND);
    }
    recordMetrics();
    closeFile();
}

void LLFileSystem::storeLoad() const
{
    // A writing object re-reads (it sees its own staged bytes); a reader loads once.
    if (mStoreLoaded && mMode == READ)
    {
        return;
    }
    const LLFileSystemStore::ReadFrom from = LLFileSystemStore::readAll(mFileID, mFileType, mStoreData);
    mStoreFromLegacy = from == LLFileSystemStore::ReadFrom::LEGACY;
    mStoreLoaded = true;
}

bool LLFileSystem::storeBeginWrite()
{
    if (!mStoreWriter)
    {
        // WRITE truncates on the object's first write, as the file version did.
        mStoreWriter = LLFileSystemStore::beginWrite(mFileID, mFileType, mMode == WRITE);
    }
    mStoreLoaded = false;
    return mStoreWriter;
}

void LLFileSystem::storeEndWrite(bool commit_now) const
{
    if (mStoreWriter)
    {
        LLFileSystemStore::endWrite(mFileID, commit_now);
        mStoreWriter = false;
    }
}

void LLFileSystem::recordMetrics() const
{
    if (mFileType == LLAssetType::AT_MESH)
    {
        return;     // counted per mesh part by llmeshdiskcache (newview)
    }
    using namespace LLCacheMetrics;
    if (mMode == READ)
    {
        const Backend b = (mUseStore && !mStoreFromLegacy) ? Backend::STORE : Backend::LEGACY;
        if (mReadBytesTotal > 0)
        {
            recordRead(Cache::ASSET, b, true, mReadBytesTotal, mFirstDataUsec > mOpenUsec ? mFirstDataUsec - mOpenUsec : 0);
        }
        else if (mProbedEmpty)
        {
            recordRead(Cache::ASSET, b, false, 0, 0);   // a miss: no bytes, no latency (metrics convention)
        }
    }
    else if (mWriteBytes || mWriteFailed)
    {
        if (!mUseStore)
        {
            recordWrite(Cache::ASSET, Backend::LEGACY, mWriteBytes, mWriteBytes, mWriteUsec, !mWriteFailed);
        }
        else if (mWriteFailed)
        {
            // Staging refused it (size cap / no memory); successes count when the record is
            // committed (LLFileSystemStore), once per asset.
            recordWrite(Cache::ASSET, Backend::STORE, mWriteBytes, 0, mWriteUsec, false);
        }
    }
}

const std::string& LLFileSystem::filename() const
{
    if (mFilename.empty())
    {
        mFilename = LLDiskCache::metaDataToFilepath(mFileID, mFileType);
    }
    return mFilename;
}

void LLFileSystem::closeFile() const
{
    if (mFile)
    {
        LLFile::close(mFile);
        mFile = nullptr;
        mFileWritable = false;
    }
}

LLFILE* LLFileSystem::openForRead() const
{
    if (!mFile)
    {
        // READ_WRITE objects read through the same handle they write with.
        mFile = LLFile::fopen(filename(), (mMode == READ_WRITE) ? "r+b" : "rb");
        mFileWritable = mFile && (mMode == READ_WRITE);
    }
    return mFile;
}

LLFILE* LLFileSystem::openForWrite()
{
    if (mFile && !mFileWritable)
    {
        closeFile();    // opened read-only by an earlier getSize()/read()
    }
    if (!mFile)
    {
        if (mMode == APPEND)
        {
            mFile = LLFile::fopen(filename(), "ab");
        }
        else if (mMode == READ_WRITE)
        {
            // Don't truncate if the file already exists
            mFile = LLFile::fopen(filename(), "r+b");
            if (!mFile)
            {
                mFile = LLFile::fopen(filename(), "w+b");
            }
        }
        else
        {
            // WRITE: truncate (once per object; it used to truncate on every write)
            mFile = LLFile::fopen(filename(), "wb");
        }
        mFileWritable = mFile != nullptr;
    }
    return mFile;
}

// static
bool LLFileSystem::getExists(const LLUUID& file_id, const LLAssetType::EType file_type)
{
    LL_PROFILE_ZONE_SCOPED;
    if (LLFileSystemStore::active())
    {
        return LLFileSystemStore::size(file_id) > 0;
    }
    // #301: one stat instead of open + seek-to-end
    return path_size(LLDiskCache::metaDataToFilepath(file_id, file_type)) > 0;
}

// static
bool LLFileSystem::removeFile(const LLUUID& file_id, const LLAssetType::EType file_type, int suppress_error /*= 0*/)
{
    if (LLFileSystemStore::active())
    {
        return LLFileSystemStore::remove(file_id);
    }
    const std::string filename = LLDiskCache::metaDataToFilepath(file_id, file_type);

    LLFile::remove(filename.c_str(), suppress_error);

    return true;
}

// static
bool LLFileSystem::renameFile(const LLUUID& old_file_id, const LLAssetType::EType old_file_type,
                              const LLUUID& new_file_id, const LLAssetType::EType new_file_type)
{
    if (LLFileSystemStore::active())
    {
        return LLFileSystemStore::rename(old_file_id, new_file_id, new_file_type);
    }
    const std::string old_filename = LLDiskCache::metaDataToFilepath(old_file_id, old_file_type);
    const std::string new_filename = LLDiskCache::metaDataToFilepath(new_file_id, new_file_type);

    // Rename needs the new file to not exist.
    LLFileSystem::removeFile(new_file_id, new_file_type, ENOENT);

    if (LLFile::rename(old_filename, new_filename) != 0)
    {
        // We would like to return false here indicating the operation
        // failed but the original code does not and doing so seems to
        // break a lot of things so we go with the flow...
        //return false;
        LL_WARNS() << "Failed to rename " << old_file_id << " to " << new_file_id << " reason: " << strerror(errno) << LL_ENDL;
    }

    return true;
}

// static
S32 LLFileSystem::getFileSize(const LLUUID& file_id, const LLAssetType::EType file_type)
{
    if (LLFileSystemStore::active())
    {
        return LLFileSystemStore::size(file_id);
    }
    // #301: one stat instead of open + seek-to-end
    return path_size(LLDiskCache::metaDataToFilepath(file_id, file_type));
}

bool LLFileSystem::read(U8* buffer, S32 bytes)
{
    bool success = false;
    mBytesRead = 0;
    mReadAttempted = true;

    if (mUseStore)
    {
        storeLoad();
        const S32 size = (S32)llmin(mStoreData.size(), (size_t)S32_MAX);
        if (bytes > 0 && mPosition >= 0 && mPosition < size)
        {
            mBytesRead = llmin(bytes, size - mPosition);
            memcpy(buffer, mStoreData.data() + mPosition, mBytesRead);
            mPosition += mBytesRead;
        }
    }
    else
    {
        LLFILE* f = openForRead();
        if (f && bytes > 0 && fseek(f, mPosition, SEEK_SET) == 0)
        {
            mBytesRead = (S32)fread(buffer, 1, bytes, f);
            mPosition += mBytesRead;
        }
    }
    if (mBytesRead)
    {
        success = true;
        if (!mFirstDataUsec)
        {
            mFirstDataUsec = now_usec();
        }
        mReadBytesTotal += mBytesRead;
    }
    else if (!mReadBytesTotal)
    {
        mProbedEmpty = true;
    }

    return success;
}

S32 LLFileSystem::getLastBytesRead() const
{
    return mBytesRead;
}

bool LLFileSystem::eof() const
{
    return mPosition >= getSize();
}

bool LLFileSystem::write(const U8* buffer, S32 bytes)
{
    const U64 t0 = now_usec();
    if (mUseStore)
    {
        S32 new_pos = mPosition;
        const bool ok = storeBeginWrite()
                     && LLFileSystemStore::write(mFileID, mMode == APPEND ? -1 : mPosition, buffer, bytes, new_pos);
        if (ok)
        {
            mPosition = new_pos;
            mWriteBytes += llmax(bytes, 0);
        }
        else
        {
            mWriteFailed = true;
        }
        mWriteUsec += now_usec() - t0;
        return ok;
    }

    LLFILE* f = openForWrite();
    if (!f)
    {
        return false;
    }

    bool success = false;
    if (mMode == APPEND)
    {
        // "ab": every write lands at the end whatever the position
        success = (bytes <= 0) || (fwrite(buffer, 1, bytes, f) == (size_t)bytes);
        success = (fflush(f) == 0) && success;
        mPosition = stream_size(f);
    }
    else
    {
        // READ_WRITE / WRITE: write at the current position (a position past the end
        // leaves a zero-filled gap, as the stream implementation did)
        if (fseek(f, mPosition, SEEK_SET) == 0)
        {
            success = (bytes <= 0) || (fwrite(buffer, 1, bytes, f) == (size_t)bytes);
            success = (fflush(f) == 0) && success;
            if (success)
            {
                mPosition += llmax(bytes, 0);
            }
        }
    }
    if (success)
    {
        mWriteBytes += llmax(bytes, 0);
    }
    else
    {
        mWriteFailed = true;
    }
    mWriteUsec += now_usec() - t0;

    return success;
}

bool LLFileSystem::setSize(S32 size)
{
    if (mMode == READ || size < 0)
    {
        return false;
    }
    if (mUseStore)
    {
        return storeBeginWrite() && LLFileSystemStore::setSize(mFileID, size);
    }
    LLFILE* f = openForWrite();
    if (!f || fflush(f) != 0)
    {
        return false;
    }
#if LL_WINDOWS
    return _chsize_s(_fileno(f), (__int64)size) == 0;
#else
    return ftruncate(fileno(f), (off_t)size) == 0;
#endif
}

bool LLFileSystem::seek(S32 offset, S32 origin)
{
    if (-1 == origin)
    {
        origin = mPosition;
    }

    S32 new_pos = origin + offset;

    S32 size = getSize();

    if (new_pos > size)
    {
        LL_WARNS() << "Attempt to seek past end of file" << LL_ENDL;

        mPosition = size;
        return false;
    }
    else if (new_pos < 0)
    {
        LL_WARNS() << "Attempt to seek past beginning of file" << LL_ENDL;

        mPosition = 0;
        return false;
    }

    mPosition = new_pos;
    return true;
}

S32 LLFileSystem::tell() const
{
    return mPosition;
}

S32 LLFileSystem::getSize() const
{
    if (mUseStore)
    {
        const S32 sz = (mStoreLoaded && mMode == READ) ? (S32)llmin(mStoreData.size(), (size_t)S32_MAX)
                                                       : LLFileSystemStore::size(mFileID);
        if (sz == 0 && mMode == READ)
        {
            mProbedEmpty = true;
        }
        return sz;
    }
    const S32 sz = getSizeLegacy();
    if (sz == 0 && mMode == READ)
    {
        mProbedEmpty = true;
    }
    return sz;
}

S32 LLFileSystem::getSizeLegacy() const
{
    // #301: from the open handle when there is one (writes are flushed), else one stat
    if (mFile)
    {
        return stream_size(mFile);
    }
    if (mMode == READ)
    {
        // A reader almost always reads next: open now and size from the handle.
        if (LLFILE* f = openForRead())
        {
            return stream_size(f);
        }
        return 0;
    }
    return path_size(filename());
}

S32 LLFileSystem::getMaxSize() const
{
    // offer up a huge size since we don't care what the max is
    return INT_MAX;
}

bool LLFileSystem::rename(const LLUUID& new_id, const LLAssetType::EType new_type)
{
    if (mUseStore)
    {
        // The staged bytes (or the record) move to the new id as one record: the download
        // flow's commit point.
        storeEndWrite(false);
        LLFileSystemStore::rename(mFileID, new_id, new_type);
        mFileID = new_id;
        mFileType = new_type;
        mFilename.clear();
        mStoreLoaded = false;
        return true;
    }
    closeFile();    // Windows can't rename an open file
    LLFileSystem::renameFile(mFileID, mFileType, new_id, new_type);

    mFileID = new_id;
    mFileType = new_type;
    mFilename.clear();

    return true;
}

bool LLFileSystem::remove() const
{
    if (mUseStore)
    {
        storeEndWrite(false);
        LLFileSystemStore::remove(mFileID);
        mStoreLoaded = false;
        mStoreData.clear();
        return true;
    }
    closeFile();    // Windows can't remove an open file
    LLFileSystem::removeFile(mFileID, mFileType);
    return true;
}

void LLFileSystem::updateFileAccessTime(const std::string& file_path)
{
    /**
     * Threshold in time_t units that is used to decide if the last access time
     * time of the file is updated or not. Added as a precaution for the concern
     * outlined in SL-14582  about frequent writes on older SSDs reducing their
     * lifespan. I think this is the right place for the threshold value - rather
     * than it being a pref - do comment on that Jira if you disagree...
     *
     * Let's start with 1 hour in time_t units and see how that unfolds
     */
    constexpr std::time_t time_threshold = 1 * 60 * 60;

    // current time
    const std::time_t cur_time = std::time(nullptr);

    boost::system::error_code ec;
#if LL_WINDOWS
    // file last write time (#301: a missing file is normal here; don't warn)
    if (!LLFile::isfile(file_path))
    {
        return;
    }
    const std::time_t last_write_time = boost::filesystem::last_write_time(ll_convert<std::wstring>(file_path), ec);
    if (ec.failed())
    {
        LL_WARNS() << "Failed to read last write time for cache file " << file_path << ": " << ec.message() << LL_ENDL;
        return;
    }

    // delta between cur time and last time the file was written
    const std::time_t delta_time = cur_time - last_write_time;

    // we only write the new value if the time in time_threshold has elapsed
    // before the last one
    if (delta_time > time_threshold)
    {
        boost::filesystem::last_write_time(ll_convert<std::wstring>(file_path), cur_time, ec);
    }
#else
    // file last write time (#301: one stat, which is also the existence check; a
    // missing file is normal here -- the caller is about to find out it isn't cached)
    llstat st;
    if (LLFile::stat(file_path, &st) != 0)
    {
        return;
    }
    const std::time_t last_write_time = st.st_mtime;

    // delta between cur time and last time the file was written
    const std::time_t delta_time = cur_time - last_write_time;

    // we only write the new value if the time in time_threshold has elapsed
    // before the last one
    if (delta_time > time_threshold)
    {
        boost::filesystem::last_write_time(file_path, cur_time, ec);
    }
#endif

    if (ec.failed())
    {
        LL_WARNS() << "Failed to update last write time for cache file " << file_path << ": " << ec.message() << LL_ENDL;
    }
}
