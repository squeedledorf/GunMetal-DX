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

#ifndef LL_FILESYSTEM_H
#define LL_FILESYSTEM_H

#include "lluuid.h"
#include "llassettype.h"
#include "lldiskcache.h"
#include "llfile.h"

#include <vector>

class LLFileSystem
{
    public:
        LLFileSystem(const LLUUID& file_id, const LLAssetType::EType file_type, S32 mode = LLFileSystem::READ);
        ~LLFileSystem();

        // #301: an LLFileSystem owns an open file handle (opened on first use, closed
        // by the destructor, rename() and remove()), so it can't be copied.
        LLFileSystem(const LLFileSystem&) = delete;
        LLFileSystem& operator=(const LLFileSystem&) = delete;

        bool read(U8* buffer, S32 bytes);
        S32  getLastBytesRead() const;
        bool eof() const;

        bool write(const U8* buffer, S32 bytes);
        bool seek(S32 offset, S32 origin = -1);
        S32  tell() const;

        S32 getSize() const;
        S32 getMaxSize() const;
        bool rename(const LLUUID& new_id, const LLAssetType::EType new_type);
        bool remove() const;

        // #303: grow (or shrink) the file to exactly 'size' bytes without writing the
        // gap: the new bytes read back as zeros (a sparse extension on ext4/NTFS).
        // Not for READ mode. Returns false if the file can't be opened or resized.
        bool setSize(S32 size);

        /**
         * Update the "last write time" of a file to "now". This must be called whenever a
         * file in the cache is read (not written) so that the last time the file was
         * accessed is up to date (This is used in the mechanism for purging the cache)
         */
        void updateFileAccessTime(const std::string& file_path);

        static bool getExists(const LLUUID& file_id, const LLAssetType::EType file_type);
        static bool removeFile(const LLUUID& file_id, const LLAssetType::EType file_type, int suppress_error = 0);
        static bool renameFile(const LLUUID& old_file_id, const LLAssetType::EType old_file_type,
                               const LLUUID& new_file_id, const LLAssetType::EType new_file_type);
        static S32 getFileSize(const LLUUID& file_id, const LLAssetType::EType file_type);

    public:
        static const S32 READ;
        static const S32 WRITE;
        static const S32 READ_WRITE;
        static const S32 APPEND;

    protected:
        // #301: open the file once per object instead of once per call. READ opens "rb";
        // the writing modes open on the first write/setSize (WRITE truncates then, as it
        // always did, but only once per object). Returns the handle or nullptr.
        LLFILE* openForRead() const;
        S32     getSizeLegacy() const;
        LLFILE* openForWrite();
        void    closeFile() const;

        LLAssetType::EType mFileType;
        LLUUID  mFileID;
        S32     mPosition;
        S32     mMode;
        S32     mBytesRead;
        mutable LLFILE* mFile = nullptr;      // open handle (see openForRead/openForWrite)
        mutable bool    mFileWritable = false; // mFile was opened by openForWrite()
        mutable std::string mFilename;         // cached metaDataToFilepath() result
        const std::string& filename() const;

        // #715: the cache store backend (llfilesystemstore.h), chosen at construction.
        // READ objects hold the whole record after their first read; writers stage.
        bool    mUseStore = false;
        mutable bool mStoreLoaded = false;
        mutable std::vector<U8> mStoreData;
        mutable bool mStoreWriter = false;     // registered with LLFileSystemStore::beginWrite
        mutable bool mStoreFromLegacy = false; // the READ was served by an old file (import)
        void    storeLoad() const;
        bool    storeBeginWrite();
        void    storeEndWrite(bool commit_now) const;

        // LLCacheMetrics (#715), both backends. A READ object counts once: a hit when it
        // read data, a miss when it found nothing (size 0 / a failed read); an existence
        // probe (getSize() > 0 without a read) is not a read. Meshes are counted by the mesh
        // cache helper (llmeshdiskcache), not here.
        U64     mOpenUsec = 0;                 // construction (the consumer's request)
        mutable U64 mFirstDataUsec = 0;        // first read() that returned bytes
        mutable bool mProbedEmpty = false;     // getSize()/read() found nothing
        mutable bool mReadAttempted = false;
        mutable U64 mReadBytesTotal = 0;
        U64     mWriteBytes = 0;
        U64     mWriteUsec = 0;
        bool    mWriteFailed = false;
        void    recordMetrics() const;
};

#endif  // LL_FILESYSTEM_H
