/**
 * @file llgputexcache.cpp
 * @brief GPU-ready texture cache (tier 2, #274). See llgputexcache.h.
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

#include "llviewerprecompiledheaders.h"

#include "llgputexcache.h"
#include "llgputexcachestore.h"

#include "hbxxh.h"
#include "llcachemetrics.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llformat.h"
#include "llstring.h"
#include "llviewercontrol.h"
#include "threadpool.h"

#include "boost/unordered_map.hpp"
#include <boost/filesystem.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <list>
#include <mutex>

namespace
{
    using Key = LLGpuTexCache::Key;

    const std::string GTX_EXT(".gtx");
    const std::string PART_EXT(".part");
    constexpr std::time_t MTIME_TOUCH_THRESHOLD = 60 * 60;   // like LLSoundCache
    constexpr F64 MB = 1024.0 * 1024.0;
    // In-flight limits (bounded RAM: every read result / write copy is a transient buffer).
    constexpr U32 MAX_READS_IN_FLIGHT = 24;
    constexpr U64 MAX_READ_BYTES = 96ull << 20;     // in flight + ready, not yet taken
    constexpr U64 MAX_WRITE_BYTES = 64ull << 20;    // copied, not yet on disk
    constexpr F64 READY_EXPIRE_SEC = 10.0;          // a result nobody took is dropped

    struct Entry
    {
        LLGpuTexCache::Info mInfo;
        std::time_t mDiskTime = 0;
        std::list<Key>::iterator mLRU;
    };

    struct ReadSlot
    {
        LLGpuTexCache::EReadState mState = LLGpuTexCache::READ_PENDING;
        std::shared_ptr<LLGpuTexCache::Blob> mBlob;
        U64 mBytes = 0;          // reserved (pending: the index size; ready: the payload)
        F64 mReadyAt = 0.0;
    };

    // Worker -> main thread. mUsec (#715 metrics): request / submit -> the worker finished
    // (the main thread's pickup delay is frame-coupled and the same for both backends).
    struct ReadDone { Key mID; std::shared_ptr<LLGpuTexCache::Blob> mBlob; bool mOK = false; U64 mUsec = 0; };
    struct WriteDone { Key mID; LLGpuTexCache::Info mInfo; U64 mReserved = 0; bool mOK = false; U64 mUsec = 0; };
    struct ScanResult
    {
        struct Found { std::time_t mTime; Key mID; LLGpuTexCache::Info mInfo; };
        std::vector<Found> mFound;
        U64 mUsec = 0;           // folder scan (list + stat every file)
    };

    using LLCacheMetrics::Cache;
    using LLCacheMetrics::Backend;
    constexpr U64 FS_BLOCK = 4096;               // allocation unit assumed for the footprint
    constexpr F64 FOOTPRINT_INTERVAL_SEC = 10.0;
    U64 sAlloc = 0;                              // sum of the files' sizes rounded up to FS_BLOCK
    F64 sFootprintAt = 0.0;

    U64 usec_between(F64 t0, F64 t1) { return t1 > t0 ? (U64)((t1 - t0) * 1e6) : 0; }
    U64 alloc_bytes(U64 bytes) { return (bytes + FS_BLOCK - 1) / FS_BLOCK * FS_BLOCK; }

    std::string sDir;
    bool sInitialized = false;
    bool sEnabled = true;
    bool sReadOnly = false;
    bool sScanned = false;
    U64 sBudget = 3072ull << 20;
    U64 sUsed = 0, sPeak = 0;
    U64 sHits = 0, sMisses = 0, sEvictions = 0, sCorrupt = 0, sExpired = 0;
    U64 sWrites = 0, sWriteFail = 0, sBytesWritten = 0, sBytesRead = 0;
    U64 sReadBytes = 0;          // pending + ready reservations
    U64 sWriteBytes = 0;         // write copies in flight
    U32 sReadsPending = 0;
    std::list<Key> sLRU;      // front = least recently used
    boost::unordered_map<Key, Entry> sEntries;
    boost::unordered_map<Key, ReadSlot> sReads;

    std::unique_ptr<LL::ThreadPool> sPool;
    std::mutex sDoneMutex;
    std::deque<ReadDone> sReadDone;
    std::deque<WriteDone> sWriteDone;
    std::unique_ptr<ScanResult> sScanDone;
    bool sScanPosted = false;

    boost::filesystem::path to_fs_path(const std::string& path)
    {
#if LL_WINDOWS
        return boost::filesystem::path(ll_convert<std::wstring>(path));
#else
        return boost::filesystem::path(path);
#endif
    }

    F64 now_sec()
    {
        return std::chrono::duration<F64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string file_name(const Key& id, const LLGpuTexCache::Info& i)
    {
        return llformat("%s_%ux%u_e%u_l%u_v%u", id.id.asString().c_str(), (U32)i.w, (U32)i.h, (U32)i.enc,
                        (U32)i.enc_level, i.encoder_version) + (id.mask ? "_mask" : "") + GTX_EXT;
    }

    std::string file_path(const Key& id, const LLGpuTexCache::Info& i)
    {
        return gDirUtilp->add(sDir, file_name(id, i));
    }

    // Parse "<uuid>_<w>x<h>_e<enc>_l<level>_v<ver>[_mask].gtx"; the name must round-trip exactly
    bool parse_name(const std::string& name, Key& id, LLGpuTexCache::Info& i)
    {
        const size_t ul = UUID_STR_LENGTH - 1;
        if (name.size() <= ul + GTX_EXT.size() || name.compare(name.size() - GTX_EXT.size(), GTX_EXT.size(), GTX_EXT) != 0)
            return false;
        const std::string id_str = name.substr(0, ul);
        if (!LLUUID::validate(id_str)) return false;
        unsigned w = 0, h = 0, e = 0, l = 0, v = 0;
        if (sscanf(name.c_str() + ul, "_%ux%u_e%u_l%u_v%u", &w, &h, &e, &l, &v) != 5) return false;
        if (!w || !h || w > 0xFFFF || h > 0xFFFF || e > 255 || l > 255) return false;
        id.id = LLUUID(id_str);
        id.mask = name.size() >= 9 && name.compare(name.size() - 9, 9, "_mask.gtx") == 0;
        i.w = (U16)w; i.h = (U16)h; i.enc = (U8)e; i.enc_level = (U8)l; i.encoder_version = v;
        return file_name(id, i) == name;
    }

    U64 header_hash(const LLGpuTexHeader& h)
    {
        return HBXXH64::digest(&h, offsetof(LLGpuTexHeader, header_hash));
    }

    bool prepare_dir(const std::string& dir)
    {
        if (dir.empty()) return false;
        boost::system::error_code ec;
        boost::filesystem::create_directories(to_fs_path(dir), ec);
        if (!LLFile::isdir(dir))
        {
            LL_WARNS("GpuTexCache") << "Cannot create GPU texture cache folder " << dir
                                    << (ec.failed() ? ": " + ec.message() : std::string()) << LL_ENDL;
            return false;
        }
        return true;
    }

    void forget_entry(boost::unordered_map<Key, Entry>::iterator it)
    {
        sUsed -= llmin(sUsed, it->second.mInfo.bytes);
        sAlloc -= llmin(sAlloc, alloc_bytes(it->second.mInfo.bytes));
        sLRU.erase(it->second.mLRU);
        sEntries.erase(it);
    }

    // Least recently used out until under budget (never `protect`, so one file bigger than
    // the whole budget cannot loop). A file a pending read has open may fail to delete on
    // Windows: it is then untracked and the next startup scan picks it up again.
    void evict(const Key& protect)
    {
        if (sReadOnly) return;   // a second instance never deletes the first one's files
        auto it = sLRU.begin();
        while (sUsed > sBudget && it != sLRU.end())
        {
            if (*it == protect) { ++it; continue; }
            const Key id = *it;
            ++it;
            auto e = sEntries.find(id);
            if (e == sEntries.end()) continue;
            const std::string path = file_path(id, e->second.mInfo);
            LLCacheMetrics::recordEvict(Cache::GPUTEX, Backend::LEGACY, e->second.mInfo.bytes);
            forget_entry(e);
            LLFile::remove(path, ENOENT);
            ++sEvictions;
        }
    }

    void add_entry(const Key& id, const LLGpuTexCache::Info& info, std::time_t t)
    {
        auto it = sEntries.find(id);
        if (it != sEntries.end())
        {
            // A new capture of the same texture (bigger, or another encoding): the old
            // file goes when its name differs.
            const std::string old_name = file_name(id, it->second.mInfo);
            if (!sReadOnly && old_name != file_name(id, info))
                LLFile::remove(gDirUtilp->add(sDir, old_name), ENOENT);
            forget_entry(it);
        }
        Entry& e = sEntries[id];
        e.mInfo = info;
        e.mDiskTime = t;
        e.mLRU = sLRU.insert(sLRU.end(), id);
        sUsed += info.bytes;
        sAlloc += alloc_bytes(info.bytes);
        sPeak = llmax(sPeak, sUsed);
    }

    // #715 metrics: the .gtx footprint from the in-RAM index (no stat()): disk = file sizes
    // rounded up to a 4 KiB allocation unit, live = payload bytes (file minus the 128-byte
    // header), files = entries.
    void push_footprint()
    {
        const U64 n = (U64)sEntries.size();
        const U64 hdr = n * sizeof(LLGpuTexHeader);
        LLCacheMetrics::setFootprint(Cache::GPUTEX, Backend::LEGACY, sAlloc, sUsed - llmin(sUsed, hdr), n);
        sFootprintAt = now_sec();
    }

    // ---- worker jobs ----
    std::unique_ptr<ScanResult> run_scan(const std::string& dir, bool purge, bool read_only)
    {
        const F64 t0 = now_sec();
        std::unique_ptr<ScanResult> res(new ScanResult);
        if (purge && !read_only) gDirUtilp->deleteFilesInDir(dir, "*" + GTX_EXT);
        if (!read_only) gDirUtilp->deleteFilesInDir(dir, "*" + GTX_EXT + PART_EXT);
        std::string name;
        LLDirIterator iter(dir, "*" + GTX_EXT);
        while (iter.next(name))
        {
            ScanResult::Found f;
            const std::string path = gDirUtilp->add(dir, name);
            if (!parse_name(name, f.mID, f.mInfo))
            {
                if (!read_only) LLFile::remove(path, ENOENT);
                continue;
            }
            llstat st;
            if (LLFile::stat(path, &st) != 0) continue;
            f.mInfo.bytes = (U64)st.st_size;
            if (f.mInfo.bytes <= sizeof(LLGpuTexHeader))
            {
                if (!read_only) LLFile::remove(path, ENOENT);
                continue;
            }
            f.mTime = (std::time_t)st.st_mtime;
            res->mFound.push_back(f);
        }
        res->mUsec = usec_between(t0, now_sec());
        return res;
    }

    // Read + validate one file. Any thread.
    std::shared_ptr<LLGpuTexCache::Blob> run_read(const Key& id, const std::string& path, U64 expect_bytes)
    {
        std::shared_ptr<LLGpuTexCache::Blob> blob = std::make_shared<LLGpuTexCache::Blob>();
        LLFILE* fp = LLFile::fopen(path, "rb");
        if (!fp) return nullptr;
        bool ok = fread(&blob->hdr, sizeof(LLGpuTexHeader), 1, fp) == 1;
        const LLGpuTexHeader& h = blob->hdr;
        ok = ok && h.magic == LLGpuTexHeader::kMagic && h.version == LLGpuTexCache::FILE_VERSION
                && h.header_bytes == sizeof(LLGpuTexHeader) && h.header_hash == header_hash(h)
                && memcmp(h.id, id.id.mData, 16) == 0
                && (((h.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0) == id.mask)
                && h.payload_bytes > 0 && h.payload_bytes + sizeof(LLGpuTexHeader) == expect_bytes
                && h.payload_bytes <= (512ull << 20);
        // The whole chain as one piece from level 0.
        blob->pieces.resize(1);
        LLGpuTexCache::Blob::Piece& piece = blob->pieces[0];
        piece.level = 0;
        piece.levels = h.levels;
        std::vector<U8>& data = piece.bytes;
        if (ok)
        {
            try { data.resize((size_t)h.payload_bytes); }
            catch (const std::bad_alloc&) { ok = false; }
        }
        ok = ok && fread(data.data(), 1, data.size(), fp) == data.size();
        if (ok)
        {
            // nothing after the payload
            U8 extra;
            ok = fread(&extra, 1, 1, fp) == 0;
        }
        LLFile::close(fp);
        ok = ok && HBXXH64::digest(data.data(), data.size()) == h.payload_hash;
        return ok ? blob : nullptr;
    }

    bool run_write(const std::string& final_path, LLGpuTexHeader hdr, const std::vector<U8>& data)
    {
        hdr.magic = LLGpuTexHeader::kMagic;
        hdr.version = LLGpuTexCache::FILE_VERSION;
        hdr.header_bytes = sizeof(LLGpuTexHeader);
        hdr.payload_bytes = data.size();
        hdr.payload_hash = HBXXH64::digest(data.data(), data.size());
        hdr.header_hash = header_hash(hdr);
        const std::string part = final_path + PART_EXT;
        LLFILE* fp = LLFile::fopen(part, "wb");
        if (!fp) return false;
        bool ok = fwrite(&hdr, sizeof(hdr), 1, fp) == 1
               && fwrite(data.data(), 1, data.size(), fp) == data.size();
        ok = (LLFile::close(fp) == 0) && ok;
        if (ok) ok = LLFile::replace(part, final_path) == 0;
        if (!ok) LLFile::remove(part, ENOENT);
        return ok;
    }

    bool post(std::function<void()> fn)
    {
        if (!sPool) return false;
        try { return sPool->getQueue().post(std::move(fn)); }
        catch (...) { return false; }
    }
}

namespace
{
    bool file_request_read(const Key& id);
    std::shared_ptr<LLGpuTexCache::Blob> file_take_read(const Key& id, bool& failed);
}

// ------------------------------------------------------------------ #715 reread diagnostics
// Main thread only (like the whole static API). Off: one cached-control read per poll and a
// bool test per call.
namespace
{
    constexpr F64 DIAG_INTERVAL_SEC = 60.0;
    constexpr size_t DIAG_MAX_KEYS = 65536;
    constexpr U32 DIAG_TIMES = 4;   // last read times kept per texture

    struct DiagRec
    {
        F64 mTouch = 0.0;             // last event (pruning)
        F64 mLastRead = 0.0;          // last read request (0 = never)
        F64 mLastDrop = 0.0;          // last drop since that read (0 = none)
        F64 mTimes[DIAG_TIMES] = {};  // last read request times (ring)
        U32 mReads = 0;               // read requests, session
        U32 mWinReads = 0;            // read requests, this window
        U32 mDrops = 0;               // drops, session
        U32 mDropsSinceRead = 0;
        bool mPressureDrop = false;   // a drop since the last read happened under pressure
        bool mFreedSinceRead = false; // the image was freed (not only shrunk) since the last read
        bool mRereadNoDrop = false;   // the last read was a reread with nothing dropped before it
        U16 mLastBefore = 0, mLastAfter = 0;   // tops of the last use
        U8 mTi = 0;
    };

    struct DiagWin
    {
        U64 reads = 0, taken = 0, failed = 0;
        U64 uses[LLGpuTexCache::USE_COUNT] = {};
        U64 blob_bytes = 0, used_bytes = 0;
        U64 no_growth = 0;            // a use whose image did not get bigger
        U64 no_drop_no_growth = 0;    // ...and the read was a reread with nothing dropped before it (pure waste)
        U64 rereads = 0;              // reads of a texture read before this session
        U64 after_down = 0, after_free = 0, after_pressure = 0, no_drop = 0;
        F64 gap_sum = 0.0;            // read -> next read of the same texture (rereads)
        F64 drop_gap_sum = 0.0;       // last drop -> reread (rereads after a drop)
        U64 no_drop_fast = 0;         // rereads with no drop, within 10 s of the previous read
        U64 downs = 0, frees = 0, pressure_drops = 0;
    };

    bool sDiagOn = false;
    F64 sDiagWinStart = 0.0;
    DiagWin sDiagWin;
    boost::unordered_map<Key, DiagRec> sDiag;

    void diag_prune(F64 t)
    {
        if (sDiag.size() < DIAG_MAX_KEYS) return;
        for (auto it = sDiag.begin(); it != sDiag.end(); )
        {
            if (t - it->second.mTouch > 2.0 * DIAG_INTERVAL_SEC) it = sDiag.erase(it);
            else ++it;
        }
        if (sDiag.size() >= DIAG_MAX_KEYS) sDiag.clear();   // still full: start over (debug data)
    }

    void diag_read(const Key& id)
    {
        const F64 t = now_sec();
        diag_prune(t);
        DiagRec& r = sDiag[id];
        ++sDiagWin.reads;
        if (r.mReads)
        {
            ++sDiagWin.rereads;
            sDiagWin.gap_sum += t - r.mLastRead;
            if (r.mDropsSinceRead)
            {
                if (r.mFreedSinceRead) ++sDiagWin.after_free;
                else ++sDiagWin.after_down;
                if (r.mPressureDrop) ++sDiagWin.after_pressure;
                sDiagWin.drop_gap_sum += t - r.mLastDrop;
            }
            else
            {
                ++sDiagWin.no_drop;
                if (t - r.mLastRead < 10.0) ++sDiagWin.no_drop_fast;
            }
        }
        r.mRereadNoDrop = r.mReads && !r.mDropsSinceRead;
        ++r.mReads;
        ++r.mWinReads;
        r.mLastRead = t;
        r.mTouch = t;
        r.mTimes[r.mTi] = t;
        r.mTi = (U8)((r.mTi + 1) % DIAG_TIMES);
        r.mDropsSinceRead = 0;
        r.mPressureDrop = false;
        r.mFreedSinceRead = false;
        r.mLastDrop = 0.0;
    }

    void diag_summary(F64 t)
    {
        const F64 win = std::max(t - sDiagWinStart, 1e-3);
        const DiagWin& w = sDiagWin;
        if (w.reads || w.downs || w.frees)
        {
            // Reads per texture this window, and the top rereaders.
            U64 hist[5] = {};   // 1, 2, 3-4, 5-8, 9+
            U64 keys = 0;
            std::vector<std::pair<U32, const std::pair<const Key, DiagRec>*>> top;
            for (const auto& kv : sDiag)
            {
                const U32 n = kv.second.mWinReads;
                if (!n) continue;
                ++keys;
                ++hist[n == 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : 4];
                if (n >= 2) top.emplace_back(n, &kv);
            }
            const size_t ntop = std::min<size_t>(top.size(), 8);
            std::partial_sort(top.begin(), top.begin() + ntop, top.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            const U64 untaken = w.reads > w.taken + w.failed ? w.reads - w.taken - w.failed : 0;
            const U64 used = w.uses[LLGpuTexCache::USE_FIRST] + w.uses[LLGpuTexCache::USE_UP] + w.uses[LLGpuTexCache::USE_REFRESH];
            const U64 taken_unused = w.taken > used ? w.taken - used : 0;   // rejected / OOM after the read
            LL_INFOS("GpuTexReadDiag") << llformat(
                "GpuTexReadDiag %.0fs: reads %llu (%.1f/s) over %llu textures [1:%llu 2:%llu 3-4:%llu 5-8:%llu 9+:%llu]; "
                "taken %llu failed %llu untaken/pending %llu; uses first %llu up %llu refresh %llu (taken unused %llu), no-growth %llu (after no drop %llu); "
                "bytes read %.1f MB uploaded %.1f MB (%.0f%%); rereads %llu: after downgrade %llu, after free %llu "
                "(under pressure %llu), NO drop %llu (within 10 s %llu); mean gap read->reread %.1f s, drop->reread %.1f s; "
                "drops: downgrades %llu frees %llu (under pressure %llu); tracked %u",
                win, (unsigned long long)w.reads, (F64)w.reads / win, (unsigned long long)keys,
                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)w.taken,
                (unsigned long long)w.failed, (unsigned long long)untaken,
                (unsigned long long)w.uses[LLGpuTexCache::USE_FIRST], (unsigned long long)w.uses[LLGpuTexCache::USE_UP],
                (unsigned long long)w.uses[LLGpuTexCache::USE_REFRESH], (unsigned long long)taken_unused,
                (unsigned long long)w.no_growth, (unsigned long long)w.no_drop_no_growth,
                (F64)w.blob_bytes / MB, (F64)w.used_bytes / MB,
                w.blob_bytes ? 100.0 * (F64)w.used_bytes / (F64)w.blob_bytes : 0.0, (unsigned long long)w.rereads,
                (unsigned long long)w.after_down, (unsigned long long)w.after_free, (unsigned long long)w.after_pressure,
                (unsigned long long)w.no_drop, (unsigned long long)w.no_drop_fast,
                w.rereads ? w.gap_sum / (F64)w.rereads : 0.0,
                (w.after_down + w.after_free) ? w.drop_gap_sum / (F64)(w.after_down + w.after_free) : 0.0,
                (unsigned long long)w.downs, (unsigned long long)w.frees, (unsigned long long)w.pressure_drops,
                (U32)sDiag.size()) << LL_ENDL;
            for (size_t i = 0; i < ntop; ++i)
            {
                const Key& k = top[i].second->first;
                const DiagRec& r = top[i].second->second;
                std::string times;
                for (U32 j = 0; j < DIAG_TIMES; ++j)
                {
                    const F64 rt = r.mTimes[(r.mTi + j) % DIAG_TIMES];
                    if (rt > 0.0) times += llformat(" -%.0f", t - rt);
                }
                LL_INFOS("GpuTexReadDiag") << "GpuTexReadDiag top " << (i + 1) << ": " << k.id << (k.mask ? " mask" : "")
                                           << " reads " << top[i].first << " (session " << r.mReads << "), drops "
                                           << r.mDrops << ", last use " << r.mLastBefore << "->" << r.mLastAfter
                                           << " px, last reads (s ago)" << times << LL_ENDL;
            }
        }
        for (auto& kv : sDiag) kv.second.mWinReads = 0;
        sDiagWin = DiagWin();
        sDiagWinStart = t;
    }

    void diag_tick()
    {
        static LLCachedControl<bool> s_diag(gSavedSettings, "SlipstreamGpuTexReadDiag", false);
        const bool on = s_diag;
        const F64 t = now_sec();
        if (on != sDiagOn)
        {
            sDiagOn = on;
            sDiag.clear();
            sDiagWin = DiagWin();
            sDiagWinStart = t;
            LL_INFOS("GpuTexReadDiag") << "GPU-ready cache read diagnostics " << (on ? "on" : "off") << LL_ENDL;
            return;
        }
        if (on && t - sDiagWinStart >= DIAG_INTERVAL_SEC) diag_summary(t);
    }
}

// static
bool LLGpuTexCache::diagOn()
{
    return sDiagOn;
}

// static
void LLGpuTexCache::diagNoteUse(const Key& id, EDiagUse cause, U64 blob_bytes, U64 used_bytes, U32 top_before,
                                U32 top_after)
{
    if (!sDiagOn) return;
    ++sDiagWin.uses[cause < USE_COUNT ? cause : USE_UP];
    sDiagWin.blob_bytes += blob_bytes;
    sDiagWin.used_bytes += used_bytes;
    DiagRec& r = sDiag[id];
    if (top_after <= top_before)
    {
        ++sDiagWin.no_growth;
        if (r.mRereadNoDrop) ++sDiagWin.no_drop_no_growth;
    }
    r.mTouch = now_sec();
    r.mLastBefore = (U16)llmin(top_before, 0xFFFFu);
    r.mLastAfter = (U16)llmin(top_after, 0xFFFFu);
}

// static
void LLGpuTexCache::diagNoteDrop(const Key& id, U32 top_before, U32 top_after, bool pressure)
{
    if (!sDiagOn) return;
    if (top_after) ++sDiagWin.downs;
    else ++sDiagWin.frees;
    if (pressure) ++sDiagWin.pressure_drops;
    auto it = sDiag.find(id);
    if (it == sDiag.end()) return;   // never read from the cache: not part of a reread story
    DiagRec& r = it->second;
    const F64 t = now_sec();
    r.mTouch = t;
    r.mLastDrop = t;
    ++r.mDrops;
    ++r.mDropsSinceRead;
    if (!top_after) r.mFreedSinceRead = true;
    if (pressure) r.mPressureDrop = true;
    (void)top_before;
}

// static
std::string LLGpuTexCache::getDefaultDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "gputex");
}

// static
void LLGpuTexCache::init(bool enabled, U64 budget_bytes, bool purge, bool read_only, LLCache::Store* store)
{
    if (store)
    {
        // Segment store backend (#715): the file backend stays uninitialised (no folder
        // scan; the .gtx files are the migration's).
        sInitialized = false;
        LLGpuTexStore::init(store, enabled, budget_bytes);
        return;
    }
    LLGpuTexStore::init(nullptr, enabled, budget_bytes);
    sLRU.clear();
    sEntries.clear();
    sReads.clear();
    sUsed = sPeak = 0;
    sAlloc = 0;
    sReadBytes = sWriteBytes = 0;
    sReadsPending = 0;
    sBudget = budget_bytes;
    sEnabled = enabled;
    sReadOnly = read_only;
    sScanned = false;
    sDir = getDefaultDir();
    while (sDir.size() > 1 && (sDir.back() == '/' || sDir.back() == '\\')) sDir.pop_back();
    if (!prepare_dir(sDir))
    {
        // No folder: the cache stays off (tier 3 serves everything, as before it existed).
        sInitialized = false;
        return;
    }
    if (!sPool)
    {
        // Own workers: "General" is one thread and runs the CPU-copy downscales and alpha
        // analysis; blocking disk I/O there would stall them.
        sPool.reset(new LL::ThreadPool("GpuTexCache", 2));
        sPool->start();
    }
    sInitialized = true;
    const std::string dir = sDir;
    sScanPosted = post([dir, purge, read_only]()
    {
        std::unique_ptr<ScanResult> r;
        try { r = run_scan(dir, purge, read_only); }
        catch (...) { r.reset(new ScanResult); }
        std::lock_guard<std::mutex> lk(sDoneMutex);
        sScanDone = std::move(r);
    });
    if (!sScanPosted)
    {
        // No worker: scan here (startup; better slow than no cache).
        std::unique_ptr<ScanResult> r = run_scan(dir, purge, read_only);
        std::lock_guard<std::mutex> lk(sDoneMutex);
        sScanDone = std::move(r);
    }
    LL_INFOS("GpuTexCache") << "GPU texture cache: folder " << sDir << ", budget "
                            << (U64)(budget_bytes >> 20) << " MB" << (enabled ? "" : " (disabled)")
                            << (purge ? ", purging" : "") << (read_only ? ", read-only (second instance)" : "")
                            << "; indexing on a worker" << LL_ENDL;
}

// static
void LLGpuTexCache::shutdown()
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::shutdown(); return; }
    if (!sInitialized) return;
    logStats("shutdown");
    if (sPool)
    {
        sPool->close();
        sPool.reset();
    }
    sInitialized = false;
    sReads.clear();
}

// static
bool LLGpuTexCache::usingStore()
{
    return LLGpuTexStore::active();
}

// static
bool LLGpuTexCache::isEnabled()
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::isEnabled();
    return sInitialized && sEnabled && sScanned;
}

// static
void LLGpuTexCache::setEnabled(bool enabled)
{
    LLGpuTexStore::setEnabled(enabled);
    sEnabled = enabled;
}

// static
void LLGpuTexCache::poll()
{
    diag_tick();
    if (LLGpuTexStore::active()) { LLGpuTexStore::poll(); return; }
    if (!sInitialized) return;
    std::unique_ptr<ScanResult> scan;
    std::deque<ReadDone> reads;
    std::deque<WriteDone> writes;
    {
        std::lock_guard<std::mutex> lk(sDoneMutex);
        if (!sScanned && sScanDone) scan = std::move(sScanDone);
        reads.swap(sReadDone);
        // Writes finished before the index exists wait for it (they would be overwritten).
        if (sScanned || scan) writes.swap(sWriteDone);
    }
    if (scan)
    {
        // Oldest first, so the LRU front is the least recently used; a duplicate UUID
        // (an older capture whose rename raced a crash) keeps the newest file.
        std::sort(scan->mFound.begin(), scan->mFound.end(),
                  [](const ScanResult::Found& a, const ScanResult::Found& b) { return a.mTime < b.mTime; });
        for (const ScanResult::Found& f : scan->mFound)
            add_entry(f.mID, f.mInfo, f.mTime);
        sPeak = sUsed;
        sScanned = true;
        evict(LLUUID::null);
        LLCacheMetrics::recordStartup(Cache::GPUTEX, Backend::LEGACY, scan->mUsec);
        push_footprint();
        logStats("startup");
    }
    for (ReadDone& r : reads)
    {
        auto it = sReads.find(r.mID);
        if (sReadsPending) --sReadsPending;
        if (it == sReads.end()) continue;   // cleared meanwhile
        ReadSlot& s = it->second;
        sReadBytes -= llmin(sReadBytes, s.mBytes);
        if (r.mOK && r.mBlob)
        {
            s.mState = READ_READY;
            s.mBlob = r.mBlob;
            s.mBytes = r.mBlob->bytes();
            s.mReadyAt = now_sec();
            sReadBytes += s.mBytes;
            sBytesRead += s.mBytes + sizeof(LLGpuTexHeader);
            LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::LEGACY, true, s.mBytes, r.mUsec);
        }
        else
        {
            // Unreadable / corrupt / truncated: never trusted again.
            s.mState = READ_FAILED;
            s.mBytes = 0;
            s.mReadyAt = now_sec();
            ++sCorrupt;
            LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::LEGACY, false, 0, r.mUsec);
            LLCacheMetrics::recordCorrupt(Cache::GPUTEX, Backend::LEGACY);
            auto e = sEntries.find(r.mID);
            if (e != sEntries.end())
            {
                LL_WARNS_ONCE("GpuTexCache") << "Discarding bad GPU texture cache file for " << r.mID.id
                                             << " (header / size / hash mismatch)" << LL_ENDL;
                const std::string path = file_path(r.mID, e->second.mInfo);
                forget_entry(e);
                if (!sReadOnly) LLFile::remove(path, ENOENT);
            }
        }
    }
    for (WriteDone& w : writes)
    {
        sWriteBytes -= llmin(sWriteBytes, w.mReserved);
        // payload = the mip chain; disk = the file (header + chain; the 4 KiB tail slack is
        // in the footprint, not here).
        LLCacheMetrics::recordWrite(Cache::GPUTEX, Backend::LEGACY, w.mReserved, w.mOK ? w.mInfo.bytes : 0, w.mUsec, w.mOK);
        if (!w.mOK) { ++sWriteFail; continue; }
        ++sWrites;
        sBytesWritten += w.mInfo.bytes;
        add_entry(w.mID, w.mInfo, std::time(nullptr));
        evict(w.mID);
    }
    if (sScanned && now_sec() - sFootprintAt > FOOTPRINT_INTERVAL_SEC) push_footprint();
    // Results nobody took (the texture went away, or was served another way).
    if (!sReads.empty())
    {
        const F64 t = now_sec();
        for (auto it = sReads.begin(); it != sReads.end(); )
        {
            const ReadSlot& s = it->second;
            const bool stale = (s.mState == READ_READY && t - s.mReadyAt > READY_EXPIRE_SEC)
                            || (s.mState == READ_FAILED && t - s.mReadyAt > READY_EXPIRE_SEC);
            if (stale)
            {
                if (s.mState == READ_READY) { sReadBytes -= llmin(sReadBytes, s.mBytes); ++sExpired; }
                it = sReads.erase(it);
            }
            else ++it;
        }
    }
}

// static
bool LLGpuTexCache::lookup(const Key& id, Info& out)
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::lookup(id, out);
    if (!isEnabled()) return false;
    auto it = sEntries.find(id);
    if (it == sEntries.end()) return false;
    out = it->second.mInfo;
    return true;
}

// static
void LLGpuTexCache::touch(const Key& id)
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::touch(id); return; }
    auto it = sEntries.find(id);
    if (it == sEntries.end()) return;
    Entry& e = it->second;
    sLRU.splice(sLRU.end(), sLRU, e.mLRU);
    const std::time_t now = std::time(nullptr);
    if (!sReadOnly && now - e.mDiskTime > MTIME_TOUCH_THRESHOLD)
    {
        boost::system::error_code ec;
        boost::filesystem::last_write_time(to_fs_path(file_path(id, e.mInfo)), now, ec);
        e.mDiskTime = now;
    }
}

// static
void LLGpuTexCache::remove(const Key& id)
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::remove(id); return; }
    auto it = sEntries.find(id);
    if (it == sEntries.end()) return;
    const std::string path = file_path(id, it->second.mInfo);
    forget_entry(it);
    if (!sReadOnly) LLFile::remove(path, ENOENT);
}

// static
bool LLGpuTexCache::requestRead(const Key& id, EReadPriority prio, U32 first_level, U32 resident_top)
{
    // (the .gtx backend reads whole files)
    const bool ok = LLGpuTexStore::active() ? LLGpuTexStore::requestRead(id, prio, first_level, resident_top)
                                            : file_request_read(id);
    if (ok && sDiagOn) diag_read(id);
    return ok;
}

namespace
{
// The .gtx backend's requestRead.
bool file_request_read(const Key& id)
{
    if (!LLGpuTexCache::isEnabled()) return false;
    if (sReads.count(id)) return false;
    auto it = sEntries.find(id);
    if (it == sEntries.end()) return false;
    const U64 bytes = it->second.mInfo.bytes;
    if (sReadsPending >= MAX_READS_IN_FLIGHT) return false;
    if (sReadBytes > 0 && sReadBytes + bytes > MAX_READ_BYTES) return false;
    const std::string path = file_path(id, it->second.mInfo);
    const F64 t_req = now_sec();
    const bool posted = post([id, path, bytes, t_req]()
    {
        ReadDone d;
        d.mID = id;
        try { d.mBlob = run_read(id, path, bytes); }
        catch (...) { d.mBlob.reset(); }
        d.mOK = d.mBlob != nullptr;
        d.mUsec = usec_between(t_req, now_sec());
        std::lock_guard<std::mutex> lk(sDoneMutex);
        sReadDone.push_back(std::move(d));
    });
    if (!posted) return false;
    ReadSlot& s = sReads[id];
    s.mState = LLGpuTexCache::READ_PENDING;
    s.mBytes = bytes;
    sReadBytes += bytes;
    ++sReadsPending;
    LLGpuTexCache::touch(id);
    return true;
}
}

// static
LLGpuTexCache::EReadState LLGpuTexCache::readState(const Key& id)
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::readState(id);
    auto it = sReads.find(id);
    return it == sReads.end() ? READ_NONE : it->second.mState;
}

// static
std::shared_ptr<LLGpuTexCache::Blob> LLGpuTexCache::takeRead(const Key& id, bool& failed)
{
    std::shared_ptr<Blob> b = LLGpuTexStore::active() ? LLGpuTexStore::takeRead(id, failed) : file_take_read(id, failed);
    if (sDiagOn)
    {
        if (b) ++sDiagWin.taken;
        else if (failed) ++sDiagWin.failed;
    }
    return b;
}

namespace
{
// The .gtx backend's takeRead.
std::shared_ptr<LLGpuTexCache::Blob> file_take_read(const Key& id, bool& failed)
{
    failed = false;
    auto it = sReads.find(id);
    if (it == sReads.end()) return nullptr;
    ReadSlot& s = it->second;
    if (s.mState == LLGpuTexCache::READ_FAILED)
    {
        failed = true;
        sReads.erase(it);
        return nullptr;
    }
    if (s.mState != LLGpuTexCache::READ_READY) return nullptr;
    std::shared_ptr<LLGpuTexCache::Blob> b = std::move(s.mBlob);
    sReadBytes -= llmin(sReadBytes, s.mBytes);
    sReads.erase(it);
    ++sHits;
    return b;
}
}

// static
void LLGpuTexCache::noteMiss()
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::noteMiss(); return; }
    ++sMisses;
    // An index miss: no I/O, ~0 latency (compare backends on hit latency).
    LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::LEGACY, false, 0, 0);
}

// static
bool LLGpuTexCache::canWrite(U64 bytes)
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::canWrite(bytes);
    if (!isEnabled() || sReadOnly) return false;
    if (bytes == 0 || bytes > sBudget) return false;
    return sWriteBytes == 0 || sWriteBytes + bytes <= MAX_WRITE_BYTES;
}

// static
bool LLGpuTexCache::write(const LLGpuTexHeader& hdr, const U8* src, U64 bytes,
                          const std::shared_ptr<std::atomic<bool>>& released)
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::write(hdr, src, bytes, released);
    if (!src || !canWrite(bytes)) return false;
    Key id;
    memcpy(id.id.mData, hdr.id, 16);
    id.mask = (hdr.flags & LLGpuTexHeader::F_MASK_VARIANT) != 0;
    Info info;
    info.w = hdr.w; info.h = hdr.h; info.enc = hdr.enc; info.enc_level = hdr.enc_level;
    info.encoder_version = hdr.encoder_version;
    info.bytes = bytes + sizeof(LLGpuTexHeader);
    const std::string path = file_path(id, info);
    const F64 t_submit = now_sec();
    const bool posted = post([hdr, src, bytes, released, id, info, path, t_submit]()
    {
        WriteDone d;
        d.mID = id; d.mInfo = info; d.mReserved = bytes;
        std::vector<U8> data;
        try { data.assign(src, src + bytes); }   // the only read of the (mapped) source
        catch (...) { data.clear(); }
        if (released) released->store(true, std::memory_order_release);
        try { d.mOK = data.size() == bytes && run_write(path, hdr, data); }
        catch (...) { d.mOK = false; }
        d.mUsec = usec_between(t_submit, now_sec());
        std::lock_guard<std::mutex> lk(sDoneMutex);
        sWriteDone.push_back(std::move(d));
    });
    if (!posted) return false;
    sWriteBytes += bytes;
    return true;
}

// static
void LLGpuTexCache::setBudget(U64 budget_bytes)
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::setBudget(budget_bytes); return; }
    sBudget = budget_bytes;
    if (sInitialized && sScanned) evict(LLUUID::null);
}

// static
void LLGpuTexCache::sampleMetrics()
{
    if (LLGpuTexStore::active()) return;   // the store's footprint is sampled from its stats (SLCacheMetrics)
    if (sInitialized && sScanned) push_footprint();
}

// static
LLGpuTexCache::Stats LLGpuTexCache::getStats()
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::getStats();
    Stats s;
    s.used = sUsed; s.budget = sBudget; s.peak = sPeak; s.entries = (U32)sEntries.size();
    s.hits = sHits; s.misses = sMisses; s.evictions = sEvictions; s.corrupt = sCorrupt;
    s.writes = sWrites; s.write_fail = sWriteFail; s.bytes_written = sBytesWritten; s.bytes_read = sBytesRead;
    s.reads_in_flight = sReadsPending;
    U32 ready = 0;
    U64 ready_bytes = 0;
    for (const auto& kv : sReads)
        if (kv.second.mState == READ_READY) { ++ready; ready_bytes += kv.second.mBytes; }
    s.ready = ready;
    s.ready_bytes = ready_bytes;
    s.read_bytes_in_flight = sReadBytes - llmin(sReadBytes, ready_bytes);
    s.write_bytes_in_flight = sWriteBytes;
    s.expired = sExpired;
    s.scanned = sScanned;
    s.enabled = isEnabled();
    return s;
}

// static
const std::string& LLGpuTexCache::getDir()
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::getDir();
    return sDir;
}

// static
std::string LLGpuTexCache::statsLine()
{
    if (LLGpuTexStore::active()) return LLGpuTexStore::statsLine();
    if (!sInitialized) return "t2 off";
    if (!sScanned) return "t2 indexing";
    return llformat("t2 %.0f/%.0fMB peak %.0f %u files hit %llu miss %llu evict %llu bad %llu wr %llu (fail %llu) rd-fly %u exp %llu",
                    (F64)sUsed / MB, (F64)sBudget / MB, (F64)sPeak / MB, (U32)sEntries.size(),
                    (unsigned long long)sHits, (unsigned long long)sMisses, (unsigned long long)sEvictions,
                    (unsigned long long)sCorrupt, (unsigned long long)sWrites, (unsigned long long)sWriteFail,
                    sReadsPending, (unsigned long long)sExpired)
         + (sEnabled ? "" : " (disabled)");
}

// static
void LLGpuTexCache::logStats(const std::string& when)
{
    if (LLGpuTexStore::active()) { LLGpuTexStore::logStats(when); return; }
    LL_INFOS("GpuTexCache") << "GPU texture cache (" << when << "): " << (U32)sEntries.size() << " files, "
                            << llformat("%.1f", (F64)sUsed / MB) << " MB used of "
                            << llformat("%.0f", (F64)sBudget / MB) << " MB budget, peak "
                            << llformat("%.1f", (F64)sPeak / MB) << " MB; hits " << sHits << ", misses " << sMisses
                            << ", evictions " << sEvictions << ", discarded bad " << sCorrupt << ", writes "
                            << sWrites << " (" << llformat("%.1f", (F64)sBytesWritten / MB) << " MB, " << sWriteFail
                            << " failed), read " << llformat("%.1f", (F64)sBytesRead / MB) << " MB, expired "
                            << sExpired << ", folder " << sDir << LL_ENDL;
}
