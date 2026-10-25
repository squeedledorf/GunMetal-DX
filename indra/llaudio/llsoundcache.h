/**
 * @file llsoundcache.h
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

#ifndef LL_LLSOUNDCACHE_H
#define LL_LLSOUNDCACHE_H

#include "stdtypes.h"
#include "lluuid.h"

#include <string>

// Decoded sounds live in their own folder (default <cache>/sounds, or the
// SlipstreamSoundCacheLocation setting), one <uuid>.dsf WAV per sound (#176).
// Every .dsf path in the viewer comes from here.
//
// The index (uuid -> size, last use) is built by one directory scan in init()
// and maintained incrementally afterwards; nothing scans the folder per sound.
// All index calls (has/touch/add/remove/setBudget/clear) are main-thread only.
// getPath()/getTempPath() may be called from the decode worker threads: they
// only read the directory string, which is fixed by init() before any decode.
class LLSoundCache
{
public:
    // SlipstreamSoundCacheSizeMB is clamped to this range (the prefs spinner matches).
    static constexpr U32 MIN_BUDGET_MB = 32;
    static constexpr U32 MAX_BUDGET_MB = 4096;

    // Default folder: "sounds" under the current LL_PATH_CACHE.
    static std::string getDefaultDir();

    // Pick the folder (configured_dir empty = default), create it, fall back to the
    // default (and then the cache root) when it is unusable, then index what is on
    // disk. purge = delete every cached decoded sound first (Clear Cache).
    // read_only = a second viewer instance: no purge and no temp-file cleanup.
    static void init(const std::string& configured_dir, U64 budget_bytes, bool purge, bool read_only);
    static bool isInitialized();

    // The active folder, without a trailing delimiter.
    static const std::string& getDir();
    // Final path of a decoded sound (<dir>/<uuid>.dsf).
    static std::string getPath(const LLUUID& uuid);
    // Where a decode is written before it is renamed into place (<dir>/<uuid>.dsf.part),
    // so a crash mid-write never leaves a truncated .dsf behind.
    static std::string getTempPath(const LLUUID& uuid);

    // True if a complete decoded file for this sound is cached.
    static bool has(const LLUUID& uuid);
    // The sound was read / played: mark it most recently used.
    static void touch(const LLUUID& uuid);
    // A decode finished and its file is in place: index it and evict down to the budget.
    static void add(const LLUUID& uuid, U64 size_bytes);
    // Drop one sound (a bad / unreadable file): deletes the file and its index entry.
    static void remove(const LLUUID& uuid);
    // Change the budget at runtime; evicts if now over it.
    static void setBudget(U64 budget_bytes);
    // Delete every cached decoded sound (keep-between-sessions off, at exit).
    static void clear();

    static U64 getBytesUsed();
    static U64 getBudget();
    static U64 getPeakBytes();
    static U32 getEntryCount();
    static U32 getEvictionCount();

    // One log line with the live / cap / peak stats.
    static void logStats(const std::string& when);
};

#endif // LL_LLSOUNDCACHE_H
