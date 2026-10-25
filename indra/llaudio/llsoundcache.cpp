/**
 * @file llsoundcache.cpp
 * @brief On-disk cache of decoded sounds (<uuid>.dsf WAV files) with an LRU byte budget.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llsoundcache.h"

#include "llaudioengine.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llformat.h"
#include "llstring.h"

#include "boost/unordered_map.hpp"
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <list>
#include <vector>

namespace
{
    const std::string DSF_EXT(".dsf");
    const std::string PART_EXT(".part");
    // A decoded file is a 44-byte WAV header plus PCM; anything shorter is junk.
    constexpr U64 MIN_DSF_BYTES = 44;
    // Like LLFileSystem::updateFileAccessTime (SL-14582): only rewrite a file's
    // mtime on use when the stored one is over an hour old, to spare SSD writes.
    constexpr std::time_t MTIME_TOUCH_THRESHOLD = 60 * 60;
    constexpr F64 MB = 1024.0 * 1024.0;

    struct Entry
    {
        U64 mSize = 0;
        std::time_t mDiskTime = 0;           // mtime last written to the file
        std::list<LLUUID>::iterator mLRU;    // position in sLRU
    };

    std::string sDir;
    bool sInitialized = false;
    U64 sBudget = 1024ull * 1024 * 1024;
    U64 sUsed = 0;
    U64 sPeak = 0;
    U32 sEvictions = 0;
    // Front = least recently used.
    std::list<LLUUID> sLRU;
    boost::unordered_map<LLUUID, Entry> sEntries;

    boost::filesystem::path to_fs_path(const std::string& path)
    {
#if LL_WINDOWS
        return boost::filesystem::path(ll_convert<std::wstring>(path));
#else
        return boost::filesystem::path(path);
#endif
    }

    std::string strip_trailing_delimiters(std::string dir)
    {
        while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
        {
            dir.pop_back();
        }
        return dir;
    }

    // Create the folder (and any missing parents) and prove we can write a file in it.
    bool prepare_dir(const std::string& dir)
    {
        if (dir.empty())
        {
            return false;
        }
        boost::system::error_code ec;
        boost::filesystem::create_directories(to_fs_path(dir), ec);
        if (!LLFile::isdir(dir))
        {
            LL_WARNS("SoundCache") << "Cannot create sound cache folder " << dir
                                   << (ec.failed() ? ": " + ec.message() : std::string()) << LL_ENDL;
            return false;
        }
        const std::string probe = gDirUtilp->add(dir, "write_test.part");
        LLFILE* fp = LLFile::fopen(probe, "wb");
        if (!fp)
        {
            LL_WARNS("SoundCache") << "Sound cache folder " << dir << " is not writable" << LL_ENDL;
            return false;
        }
        const bool wrote = fputc('x', fp) != EOF;
        LLFile::close(fp);
        LLFile::remove(probe, ENOENT);
        if (!wrote)
        {
            LL_WARNS("SoundCache") << "Sound cache folder " << dir << " is not writable" << LL_ENDL;
        }
        return wrote;
    }

    void delete_file(const LLUUID& uuid)
    {
        LLFile::remove(LLSoundCache::getPath(uuid), ENOENT);
    }

    // Drop least recently used files until under budget. Never the one just added /
    // used (protect), so a single sound bigger than the whole budget cannot loop.
    void evict(const LLUUID& protect)
    {
        auto it = sLRU.begin();
        while (sUsed > sBudget && it != sLRU.end())
        {
            if (*it == protect)
            {
                ++it;
                continue;
            }
            const LLUUID uuid = *it;
            it = sLRU.erase(it);
            auto entry = sEntries.find(uuid);
            if (entry != sEntries.end())
            {
                sUsed -= llmin(sUsed, entry->second.mSize);
                sEntries.erase(entry);
            }
            delete_file(uuid);
            ++sEvictions;
            if (gAudiop)
            {
                gAudiop->handleDecodedFileEvicted(uuid);
            }
        }
    }
}

// static
std::string LLSoundCache::getDefaultDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "sounds");
}

// static
void LLSoundCache::init(const std::string& configured_dir, U64 budget_bytes, bool purge, bool read_only)
{
    sLRU.clear();
    sEntries.clear();
    sUsed = 0;
    sPeak = 0;
    sEvictions = 0;
    sBudget = budget_bytes;

    // Configured folder, else the default, else (last resort) the cache root, which
    // is where decoded sounds used to live.
    std::vector<std::string> candidates;
    if (!configured_dir.empty())
    {
        candidates.push_back(strip_trailing_delimiters(configured_dir));
    }
    candidates.push_back(strip_trailing_delimiters(getDefaultDir()));
    candidates.push_back(strip_trailing_delimiters(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "")));

    sDir.clear();
    for (const std::string& dir : candidates)
    {
        if (prepare_dir(dir))
        {
            sDir = dir;
            break;
        }
        LL_WARNS("SoundCache") << "Sound cache folder " << dir << " is unusable; falling back" << LL_ENDL;
    }
    if (sDir.empty())
    {
        // Nothing writable at all: decodes will fail to write and the sounds stay silent,
        // exactly as before this cache existed.
        sDir = candidates.back();
        LL_WARNS("SoundCache") << "No writable sound cache folder; using " << sDir << LL_ENDL;
    }

    if (purge && !read_only)
    {
        LL_INFOS("SoundCache") << "Clearing the sound cache in " << sDir << LL_ENDL;
        gDirUtilp->deleteFilesInDir(sDir, "*" + DSF_EXT);
    }
    if (!read_only)
    {
        // Temp files are decodes interrupted by a crash or kill; never trusted.
        gDirUtilp->deleteFilesInDir(sDir, "*" + DSF_EXT + PART_EXT);
    }

    // One scan to build the index; the mtime stands in for the last use.
    struct Found
    {
        std::time_t mTime;
        LLUUID mID;
        U64 mSize;
    };
    std::vector<Found> found;
    std::string name;
    LLDirIterator iter(sDir, "*" + DSF_EXT);
    while (iter.next(name))
    {
        if (name.size() != (size_t)(UUID_STR_LENGTH - 1) + DSF_EXT.size())
        {
            continue;
        }
        const std::string id_str = name.substr(0, UUID_STR_LENGTH - 1);
        if (!LLUUID::validate(id_str))
        {
            continue;
        }
        const std::string path = gDirUtilp->add(sDir, name);
        llstat st;
        if (LLFile::stat(path, &st) != 0)
        {
            continue;
        }
        const U64 size = (U64)st.st_size;
        if (size < MIN_DSF_BYTES)
        {
            LLFile::remove(path, ENOENT);
            continue;
        }
        found.push_back({ (std::time_t)st.st_mtime, LLUUID(id_str), size });
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mTime < b.mTime; });
    for (const Found& f : found)
    {
        if (sEntries.count(f.mID))
        {
            continue;
        }
        Entry& entry = sEntries[f.mID];
        entry.mSize = f.mSize;
        entry.mDiskTime = f.mTime;
        entry.mLRU = sLRU.insert(sLRU.end(), f.mID);
        sUsed += f.mSize;
    }
    sPeak = sUsed;
    sInitialized = true;

    evict(LLUUID::null);
    logStats("startup");
}

// static
bool LLSoundCache::isInitialized()
{
    return sInitialized;
}

// static
const std::string& LLSoundCache::getDir()
{
    return sDir;
}

// static
std::string LLSoundCache::getPath(const LLUUID& uuid)
{
    if (sDir.empty())
    {
        // Not initialised (a tool without the viewer's startup): the old location.
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, uuid.asString()) + DSF_EXT;
    }
    return gDirUtilp->add(sDir, uuid.asString() + DSF_EXT);
}

// static
std::string LLSoundCache::getTempPath(const LLUUID& uuid)
{
    return getPath(uuid) + PART_EXT;
}

// static
bool LLSoundCache::has(const LLUUID& uuid)
{
    if (!sInitialized)
    {
        return gDirUtilp->fileExists(getPath(uuid));
    }
    return sEntries.count(uuid) > 0;
}

// static
void LLSoundCache::touch(const LLUUID& uuid)
{
    auto it = sEntries.find(uuid);
    if (it == sEntries.end())
    {
        return;
    }
    Entry& entry = it->second;
    sLRU.splice(sLRU.end(), sLRU, entry.mLRU);

    // Carry the use over to the next session's scan, at most once an hour per file.
    const std::time_t now = std::time(nullptr);
    if (now - entry.mDiskTime > MTIME_TOUCH_THRESHOLD)
    {
        boost::system::error_code ec;
        boost::filesystem::last_write_time(to_fs_path(getPath(uuid)), now, ec);
        entry.mDiskTime = now;
    }
}

// static
void LLSoundCache::add(const LLUUID& uuid, U64 size_bytes)
{
    if (!sInitialized)
    {
        return;
    }
    auto it = sEntries.find(uuid);
    if (it != sEntries.end())
    {
        sUsed -= llmin(sUsed, it->second.mSize);
        it->second.mSize = size_bytes;
        it->second.mDiskTime = std::time(nullptr);
        sLRU.splice(sLRU.end(), sLRU, it->second.mLRU);
    }
    else
    {
        Entry& entry = sEntries[uuid];
        entry.mSize = size_bytes;
        entry.mDiskTime = std::time(nullptr);
        entry.mLRU = sLRU.insert(sLRU.end(), uuid);
    }
    sUsed += size_bytes;
    sPeak = llmax(sPeak, sUsed);
    evict(uuid);
}

// static
void LLSoundCache::remove(const LLUUID& uuid)
{
    auto it = sEntries.find(uuid);
    if (it != sEntries.end())
    {
        sUsed -= llmin(sUsed, it->second.mSize);
        sLRU.erase(it->second.mLRU);
        sEntries.erase(it);
    }
    delete_file(uuid);
}

// static
void LLSoundCache::setBudget(U64 budget_bytes)
{
    sBudget = budget_bytes;
    if (sInitialized)
    {
        evict(LLUUID::null);
    }
}

// static
void LLSoundCache::clear()
{
    if (sDir.empty())
    {
        return;
    }
    const std::list<LLUUID> dropped(std::move(sLRU));
    sLRU.clear();
    sEntries.clear();
    sUsed = 0;
    gDirUtilp->deleteFilesInDir(sDir, "*" + DSF_EXT);
    if (gAudiop)
    {
        for (const LLUUID& uuid : dropped)
        {
            gAudiop->handleDecodedFileEvicted(uuid);
        }
    }
}

// static
U64 LLSoundCache::getBytesUsed()
{
    return sUsed;
}

// static
U64 LLSoundCache::getBudget()
{
    return sBudget;
}

// static
U64 LLSoundCache::getPeakBytes()
{
    return sPeak;
}

// static
U32 LLSoundCache::getEntryCount()
{
    return (U32)sEntries.size();
}

// static
U32 LLSoundCache::getEvictionCount()
{
    return sEvictions;
}

// static
void LLSoundCache::logStats(const std::string& when)
{
    LL_INFOS("SoundCache") << "Sound cache (" << when << "): " << getEntryCount() << " files, "
                           << llformat("%.1f", (F64)sUsed / MB) << " MB used of "
                           << llformat("%.0f", (F64)sBudget / MB) << " MB budget, peak "
                           << llformat("%.1f", (F64)sPeak / MB) << " MB, " << sEvictions
                           << " evictions, folder " << sDir << LL_ENDL;
}
