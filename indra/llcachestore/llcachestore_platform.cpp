/**
 * @file llcachestore_platform.cpp
 * @brief Internal OS layer of the segment store (see llcachestore_platform.h).
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

#include "llcachestore_platform.h"

#include "fsyspath.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <winioctl.h>
#else
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#if LL_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace LLCacheStorePlatform
{

// =================================================================== File

File::~File()
{
    close();
}

#if LL_WINDOWS

namespace
{
    std::wstring widen(const std::string& path)
    {
        return fsyspath(path).wstring();
    }

    HANDLE handleOf(void* h) { return h ? (HANDLE)h : INVALID_HANDLE_VALUE; }
}

bool File::open(const std::string& path, Mode mode)
{
    close();
    DWORD access = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    switch (mode)
    {
    case READ:          break;
    case READ_WRITE:    access |= GENERIC_WRITE; break;
    case CREATE_RW:     access |= GENERIC_WRITE; disposition = OPEN_ALWAYS; break;
    case CREATE_NEW_RW: access |= GENERIC_WRITE; disposition = CREATE_NEW; break;
    }
    HANDLE h = CreateFileW(widen(path).c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        mLastError = (int)GetLastError();
        return false;
    }
    mHandle = (void*)h;
    return true;
}

void File::close()
{
    if (mHandle)
    {
        CloseHandle(handleOf(mHandle));
        mHandle = nullptr;
    }
}

bool File::isOpen() const
{
    return mHandle != nullptr;
}

S64 File::pread(void* dst, size_t len, U64 offset) const
{
    size_t done = 0;
    while (done < len)
    {
        OVERLAPPED ov = {};
        U64 off = offset + done;
        ov.Offset = (DWORD)(off & 0xffffffffull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD chunk = (DWORD)std::min<size_t>(len - done, 1u << 30);
        DWORD got = 0;
        if (!ReadFile(handleOf(mHandle), (U8*)dst + done, chunk, &got, &ov))
        {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
            {
                break;
            }
            mLastError = (int)err;
            return -1;
        }
        if (got == 0)
        {
            break;
        }
        done += got;
    }
    return (S64)done;
}

S64 File::pwrite(const void* src, size_t len, U64 offset)
{
    size_t done = 0;
    while (done < len)
    {
        OVERLAPPED ov = {};
        U64 off = offset + done;
        ov.Offset = (DWORD)(off & 0xffffffffull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD chunk = (DWORD)std::min<size_t>(len - done, 1u << 30);
        DWORD put = 0;
        if (!WriteFile(handleOf(mHandle), (const U8*)src + done, chunk, &put, &ov))
        {
            mLastError = (int)GetLastError();
            return done ? (S64)done : -1;
        }
        if (put == 0)
        {
            break;
        }
        done += put;
    }
    return (S64)done;
}

S64 File::size() const
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(handleOf(mHandle), &sz))
    {
        mLastError = (int)GetLastError();
        return -1;
    }
    return (S64)sz.QuadPart;
}

bool File::truncate(U64 size)
{
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    if (!SetFileInformationByHandle(handleOf(mHandle), FileEndOfFileInfo, &info, sizeof(info)))
    {
        mLastError = (int)GetLastError();
        return false;
    }
    return true;
}

bool File::sync()
{
    if (!FlushFileBuffers(handleOf(mHandle)))
    {
        mLastError = (int)GetLastError();
        return false;
    }
    return true;
}

bool File::adviseWillNeed(U64, U64) const
{
    return false;   // Windows uses the store's read-ahead buffer instead
}

bool File::lastErrorWasNoSpace() const
{
    return mLastError == ERROR_DISK_FULL || mLastError == ERROR_HANDLE_DISK_FULL;
}

// --------------------------------------------------------------- LockFile

LockFile::~LockFile()
{
    unlock();
}

bool LockFile::tryLock(const std::string& path)
{
    unlock();
    mBusy = false;
    // We share everything, so opening never fails on another holder; the
    // byte-range lock below is what conflicts (also between two handles of
    // one process).
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    OVERLAPPED ov = {};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
    {
        DWORD err = GetLastError();
        CloseHandle(h);
        mBusy = (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING);
        return false;
    }
    // Keep the handle: the lock lives as long as it is open.
    mHandle = (void*)h;
    mLocked = true;
    return true;
}

void LockFile::unlock()
{
    if (mHandle)
    {
        OVERLAPPED ov = {};
        UnlockFileEx((HANDLE)mHandle, 0, 1, 0, &ov);
        CloseHandle((HANDLE)mHandle);
        mHandle = nullptr;
    }
    mLocked = false;
}

// ----------------------------------------------------------------- volume

S64 freeSpace(const std::string& path)
{
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(widen(path).c_str(), &avail, nullptr, nullptr))
    {
        return -1;
    }
    return (S64)avail.QuadPart;
}

DriveType detectDriveType(const std::string& path)
{
    // Volume of the path, e.g. "C:\" or "\\?\Volume{guid}\".
    wchar_t volume[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(widen(path).c_str(), volume, MAX_PATH))
    {
        return DriveType::UNKNOWN;
    }
    wchar_t guid_path[MAX_PATH + 1] = {};
    if (!GetVolumeNameForVolumeMountPointW(volume, guid_path, MAX_PATH))
    {
        return DriveType::UNKNOWN;
    }
    // "\\?\Volume{guid}\" -> "\\.\Volume{guid}" (no trailing slash: open the volume device).
    std::wstring dev(guid_path);
    if (!dev.empty() && dev.back() == L'\\')
    {
        dev.pop_back();
    }
    if (dev.size() > 4 && dev.compare(0, 4, L"\\\\?\\") == 0)
    {
        dev[2] = L'.';
    }
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return DriveType::UNKNOWN;
    }
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc = {};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                              &desc, sizeof(desc), &bytes, nullptr);
    CloseHandle(h);
    if (!ok || bytes < sizeof(desc))
    {
        return DriveType::UNKNOWN;
    }
    return desc.IncursSeekPenalty ? DriveType::HDD : DriveType::SSD;
}

U32 processId()
{
    return (U32)GetCurrentProcessId();
}

void lowerCurrentThreadPriority()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

bool syncDir(const std::string&)
{
    return true;    // NTFS metadata is journaled; no directory handle flush needed
}

#else // POSIX ==========================================================

bool File::open(const std::string& path, Mode mode)
{
    close();
    int flags = O_CLOEXEC;
    switch (mode)
    {
    case READ:          flags |= O_RDONLY; break;
    case READ_WRITE:    flags |= O_RDWR; break;
    case CREATE_RW:     flags |= O_RDWR | O_CREAT; break;
    case CREATE_NEW_RW: flags |= O_RDWR | O_CREAT | O_EXCL; break;
    }
    int fd;
    do
    {
        fd = ::open(path.c_str(), flags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
    {
        mLastError = errno;
        return false;
    }
    mFd = fd;
    return true;
}

void File::close()
{
    if (mFd >= 0)
    {
        ::close(mFd);
        mFd = -1;
    }
}

bool File::isOpen() const
{
    return mFd >= 0;
}

S64 File::pread(void* dst, size_t len, U64 offset) const
{
    size_t done = 0;
    while (done < len)
    {
        ssize_t got = ::pread(mFd, (U8*)dst + done, len - done, (off_t)(offset + done));
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            mLastError = errno;
            return -1;
        }
        if (got == 0)
        {
            break;
        }
        done += (size_t)got;
    }
    return (S64)done;
}

S64 File::pwrite(const void* src, size_t len, U64 offset)
{
    size_t done = 0;
    while (done < len)
    {
        ssize_t put = ::pwrite(mFd, (const U8*)src + done, len - done, (off_t)(offset + done));
        if (put < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            mLastError = errno;
            return done ? (S64)done : -1;
        }
        if (put == 0)
        {
            mLastError = ENOSPC;
            break;
        }
        done += (size_t)put;
    }
    return (S64)done;
}

S64 File::size() const
{
    struct stat st;
    if (fstat(mFd, &st) != 0)
    {
        mLastError = errno;
        return -1;
    }
    return (S64)st.st_size;
}

bool File::truncate(U64 size)
{
    if (ftruncate(mFd, (off_t)size) != 0)
    {
        mLastError = errno;
        return false;
    }
    return true;
}

bool File::sync()
{
#if LL_LINUX
    if (fdatasync(mFd) != 0)
#else
    if (fsync(mFd) != 0)
#endif
    {
        mLastError = errno;
        return false;
    }
    return true;
}

bool File::adviseWillNeed(U64 offset, U64 len) const
{
#if LL_LINUX
    return posix_fadvise(mFd, (off_t)offset, (off_t)len, POSIX_FADV_WILLNEED) == 0;
#else
    return false;
#endif
}

bool File::lastErrorWasNoSpace() const
{
    return mLastError == ENOSPC || mLastError == EDQUOT;
}

// --------------------------------------------------------------- LockFile

LockFile::~LockFile()
{
    unlock();
}

bool LockFile::tryLock(const std::string& path)
{
    unlock();
    mBusy = false;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return false;
    }
    // flock() locks belong to the open file description, so a second open
    // in the same process conflicts too (unlike fcntl record locks, which
    // are per process).
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        mBusy = (errno == EWOULDBLOCK);
        ::close(fd);
        return false;
    }
    mFd = fd;
    mLocked = true;
    return true;
}

void LockFile::unlock()
{
    if (mFd >= 0)
    {
        flock(mFd, LOCK_UN);
        ::close(mFd);
        mFd = -1;
    }
    mLocked = false;
}

// ----------------------------------------------------------------- volume

S64 freeSpace(const std::string& path)
{
    struct statvfs vfs;
    if (statvfs(path.c_str(), &vfs) != 0)
    {
        return -1;
    }
    return (S64)vfs.f_bavail * (S64)vfs.f_frsize;
}

namespace
{
    // 1 / 0 from <sysblock>/queue/rotational, -1 when absent.
    int readRotational(const std::string& sysblock)
    {
        std::ifstream in(sysblock + "/queue/rotational");
        int v = -1;
        if (in >> v)
        {
            return v ? 1 : 0;
        }
        return -1;
    }

    // Resolve a /sys/class/block-style device directory to rotational, following
    // device-mapper / md slaves (rotational if any underlying disk is).
    int rotationalOfSysDev(const std::string& sysdev, int depth = 0)
    {
        if (depth > 8)
        {
            return -1;
        }
        std::error_code ec;
        // Partitions have no queue/; their parent (the disk) does.
        int r = readRotational(sysdev);
        if (r < 0)
        {
            r = readRotational(sysdev + "/..");
        }
        // dm-* / md* report rotational=0 even on hard disks: ask the slaves.
        fs::path slaves = fs::path(sysdev) / "slaves";
        if (fs::is_directory(slaves, ec))
        {
            int any = -1;
            for (const auto& e : fs::directory_iterator(slaves, ec))
            {
                int s = rotationalOfSysDev(e.path().string(), depth + 1);
                if (s == 1)
                {
                    return 1;
                }
                if (s == 0)
                {
                    any = 0;
                }
            }
            if (any >= 0)
            {
                return any;
            }
        }
        return r;
    }

    // Device node backing the mount that holds path (from /proc/self/mountinfo).
    // Needed for btrfs / overlay, whose st_dev is an anonymous device number.
    std::string mountSourceFor(const std::string& path)
    {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(path), ec);
        std::string target = ec ? path : p.string();
        std::ifstream in("/proc/self/mountinfo");
        std::string line, best_src;
        size_t best_len = 0;
        while (std::getline(in, line))
        {
            // id parent maj:min root mountpoint opts ... - fstype source superopts
            std::istringstream ls(line);
            std::string id, parent, majmin, root, mnt;
            ls >> id >> parent >> majmin >> root >> mnt;
            size_t dash = line.find(" - ");
            if (dash == std::string::npos)
            {
                continue;
            }
            std::istringstream rest(line.substr(dash + 3));
            std::string fstype, source;
            rest >> fstype >> source;
            bool prefix = target.compare(0, mnt.size(), mnt) == 0 &&
                          (target.size() == mnt.size() || mnt == "/" || target[mnt.size()] == '/');
            if (prefix && mnt.size() >= best_len)
            {
                best_len = mnt.size();
                best_src = source;
            }
        }
        return best_src;
    }
}

DriveType detectDriveType(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        std::string sysdev = "/sys/dev/block/" + std::to_string(major(st.st_dev)) + ":" +
                             std::to_string(minor(st.st_dev));
        std::error_code ec;
        if (fs::exists(sysdev, ec))
        {
            int r = rotationalOfSysDev(fs::canonical(sysdev, ec).string());
            if (r >= 0)
            {
                return r ? DriveType::HDD : DriveType::SSD;
            }
        }
    }
    // Anonymous st_dev (btrfs, overlay): try the mount's source device.
    std::string src = mountSourceFor(path);
    if (src.compare(0, 5, "/dev/") == 0)
    {
        std::error_code ec;
        fs::path node = fs::canonical(src, ec);     // /dev/mapper/x -> /dev/dm-0
        if (!ec)
        {
            std::string sysdev = "/sys/class/block/" + node.filename().string();
            fs::path real = fs::canonical(sysdev, ec);
            if (!ec)
            {
                int r = rotationalOfSysDev(real.string());
                if (r >= 0)
                {
                    return r ? DriveType::HDD : DriveType::SSD;
                }
            }
        }
    }
    return DriveType::UNKNOWN;
}

U32 processId()
{
    return (U32)getpid();
}

void lowerCurrentThreadPriority()
{
#if LL_LINUX
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
#endif
}

bool syncDir(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    bool ok = fsync(fd) == 0;
    ::close(fd);
    return ok;
}

#endif // LL_WINDOWS / POSIX

// ======================================================= portable helpers

bool makeDirs(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(fsyspath(path), ec);
    return fs::is_directory(fsyspath(path), ec);
}

bool removeFile(const std::string& path)
{
    std::error_code ec;
    return fs::remove(fsyspath(path), ec) && !ec;
}

bool removeTree(const std::string& path)
{
    std::error_code ec;
    fs::remove_all(fsyspath(path), ec);
    return !ec;
}

bool renameFile(const std::string& from, const std::string& to)
{
    std::error_code ec;
    fs::rename(fsyspath(from), fsyspath(to), ec);
    return !ec;
}

bool fileExists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(fsyspath(path), ec);
}

std::vector<std::string> listDir(const std::string& path)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(fsyspath(path), ec), end; !ec && it != end; it.increment(ec))
    {
        out.push_back(fsyspath(it->path().filename()).string());
    }
    return out;
}

} // namespace LLCacheStorePlatform
