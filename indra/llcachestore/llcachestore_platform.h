/**
 * @file llcachestore_platform.h
 * @brief Internal: the few OS calls the segment store needs (positional file
 *        I/O, lock files, free space, drive type, read-ahead hints).
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

#ifndef LL_LLCACHESTORE_PLATFORM_H
#define LL_LLCACHESTORE_PLATFORM_H

#include "stdtypes.h"

#include <string>
#include <vector>

// Not part of the public API. Paths are UTF-8. Every call is safe from any
// thread; a File may be read concurrently by several threads (positional I/O
// only: there is no shared file position).
namespace LLCacheStorePlatform
{
    // A native file handle (fd on POSIX, HANDLE on Windows), closed on
    // destruction. Opened with share-read/write/delete on Windows so another
    // instance can read it and the store can delete a segment while a reader
    // still holds it.
    class File
    {
    public:
        File() = default;
        ~File();
        File(const File&) = delete;
        File& operator=(const File&) = delete;

        enum Mode
        {
            READ,           // existing file, read only
            READ_WRITE,     // existing file
            CREATE_RW,      // create if missing, keep contents
            CREATE_NEW_RW,  // create; fail if it exists
        };

        bool open(const std::string& path, Mode mode);
        void close();
        bool isOpen() const;

        // Positional I/O. Return bytes transferred, or -1 on error (errno-like
        // code in lastError()). A short read means end of file.
        S64 pread(void* dst, size_t len, U64 offset) const;
        S64 pwrite(const void* src, size_t len, U64 offset);

        S64  size() const;                 // -1 on error
        bool truncate(U64 size);
        bool sync();                       // data to stable storage (fdatasync / FlushFileBuffers)

        // Read-ahead hint for [offset, offset+len): POSIX_FADV_WILLNEED.
        // No-op (returns false) where unsupported.
        bool adviseWillNeed(U64 offset, U64 len) const;

        int lastError() const { return mLastError; }
        bool lastErrorWasNoSpace() const;

    private:
#if LL_WINDOWS
        void* mHandle = nullptr;    // HANDLE; INVALID_HANDLE_VALUE stored as nullptr
#else
        int mFd = -1;
#endif
        mutable int mLastError = 0;
    };

    // An exclusive, non-blocking, advisory lock tied to an open file handle
    // (flock on POSIX, LockFileEx on Windows). Two LockFile objects conflict
    // even inside one process. Released on destruction or process exit (so a
    // crash never leaves the store locked).
    class LockFile
    {
    public:
        LockFile() = default;
        ~LockFile();
        LockFile(const LockFile&) = delete;
        LockFile& operator=(const LockFile&) = delete;

        // True when the lock was acquired; false when another holder has it
        // (or on error: then error() is non-zero and not "busy").
        bool tryLock(const std::string& path);
        void unlock();
        bool locked() const { return mLocked; }
        bool busy() const { return mBusy; }

    private:
#if LL_WINDOWS
        void* mHandle = nullptr;    // HANDLE holding the byte-range lock
#else
        int mFd = -1;               // fd holding the flock
#endif
        bool mLocked = false;
        bool mBusy = false;
    };

    // Free bytes available to this user on the volume holding path; -1 if unknown.
    S64 freeSpace(const std::string& path);

    enum class DriveType : U8 { UNKNOWN, SSD, HDD };
    // Rotational vs solid state for the volume holding path. Linux:
    // /sys/dev/block/MAJ:MIN[/..]/queue/rotational (following dm / md slaves).
    // Windows: IOCTL_STORAGE_QUERY_PROPERTY StorageDeviceSeekPenaltyProperty.
    DriveType detectDriveType(const std::string& path);

    U32 processId();

    // The calling thread below normal OS priority (CPU-bound store helpers, so the
    // render / main threads win the cores). Linux: nice +5 on its own tid; Windows:
    // THREAD_PRIORITY_BELOW_NORMAL; elsewhere a no-op.
    void lowerCurrentThreadPriority();

    // Directory helpers (std::filesystem with UTF-8 paths, never throw).
    bool makeDirs(const std::string& path);
    bool removeFile(const std::string& path);
    bool removeTree(const std::string& path);
    bool renameFile(const std::string& from, const std::string& to);   // replaces "to"
    bool fileExists(const std::string& path);
    bool syncDir(const std::string& path);                              // POSIX: fsync the directory
    std::vector<std::string> listDir(const std::string& path);          // names only
}

#endif // LL_LLCACHESTORE_PLATFORM_H
