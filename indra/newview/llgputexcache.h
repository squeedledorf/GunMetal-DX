/**
 * @file llgputexcache.h
 * @brief GPU-ready texture cache (tier 2, #274): the block-compressed mip chains the
 *        Vulkan texture streamer produced, on disk, so a texture seen before reloads
 *        without a full J2C decode or a BC re-encode.
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

#ifndef LL_LLGPUTEXCACHE_H
#define LL_LLGPUTEXCACHE_H

#include "stdtypes.h"
#include "lluuid.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace LLCache { class Store; }

// TEXTURE STORAGE TIERS (#405): 0 = GPU resident, 1 = decoded copy in RAM, 2 = THIS
// cache (disk read + upload), 3 = LL's J2C cache (read + decode + BC encode + upload).
//
// LAYOUT: one file per texture UUID and mip policy in <cache>/gputex:
//     <uuid>_<w>x<h>_e<enc>_l<level>_v<encoder version>[_mask].gtx
// The name carries everything the index needs (top-level size, encoding, compression
// level, encoder version, mip policy), so the startup scan never opens a file:
// block bytes don't depend on the sRGB / UNORM view (the encoder samples through a UNORM
// view and the streamer's images are mutable-format), so both views share it.
// Masked and unmasked uses have separate entries
// A file is a 128-byte LLGpuTexHeader followed by the mip chain, level 0 first, each
// level tightly packed (BC: 4x4 blocks row-major; RGBA8: rows of texels).
//
// SAFETY: writes go to <name>.part and are renamed into place; *.part files are deleted
// at startup. A read checks magic, file version, header size + hash, UUID, payload size
// (against the file size and the header) and the payload hash; any mismatch deletes the
// file and drops the entry, and the caller falls back to tier 3. The streamer checks the
// header's format facts against what it would make before using it.
//
// BOUNDED: disk bytes by a budget (LRU: least recently used out first; the file mtime
// carries the use to the next session's scan), reads in flight (count + bytes), read
// results waiting to be taken (bytes + age), writes in flight (bytes).
//
// THREADING: all static calls are main-thread only. Disk I/O (scan, reads, writes) runs
// on the cache's own two-thread pool ("GpuTexCache"); results come back through queues
// merged by poll(). Until the startup scan is merged every lookup misses.
//
// TWO BACKENDS (#715): the static API below is served either by the segment store
// (<cache>/store, kind GPUTEX, zstd, one record per level above 256 px + a tail, #865;
// llgputexcachestore.cpp, record layout in
// llgputexrecord.h) or by the .gtx files described above (this file's implementation).
// init() picks the store when it is handed an open one (CacheStoreGpuTex on, the store
// opened); otherwise the files, as before. Callers never see the difference. Existing
// .gtx files are converted into store records in the background (slgputexmigrate.h).

// Every field sits at its natural alignment (no packing needed; checked below).
struct LLGpuTexHeader
{
    U32 magic = 0;            // kMagic
    U32 version = 0;          // LLGpuTexCache::FILE_VERSION
    U32 encoder_version = 0;  // the streamer's encoder version at capture
    U32 header_bytes = 0;     // sizeof(LLGpuTexHeader)
    U8  id[16] = {};          // texture UUID
    U32 vk_format = 0;        // base VkFormat of the captured image
    U8  enc = 0;              // LLTexEnc
    U8  enc_level = 0;        // RenderVKTextureCompression level it was encoded under
    U8  comps = 0;            // channels the image represents (3 = opaque)
    U8  flags = 0;            // F_* below
    U16 w = 0, h = 0;         // level 0 size
    U16 full_w = 0, full_h = 0;   // the asset's full size (under the resolution cap at capture)
    U16 levels = 0;           // mips stored (a full chain from level 0)
    U8  alpha_grad_pct = 255; // LLTextureData alpha analysis: % partial-alpha texels (255 = none)
    U8  cov_cut = 0;          // F_COVERAGE_MIPS: the 8-bit alpha cutoff the mips preserve coverage at (was padding: old files 0)
    U16 alpha_side = 0;       // short side of the decode that analysis was committed from
    U16 pad1 = 0;
    U64 payload_bytes = 0;
    U64 payload_hash = 0;     // HBXXH64 of the payload
    U64 header_hash = 0;      // HBXXH64 of the bytes before this field
    U8  reserved[48] = {};

    // F_COVERAGE_MIPS: an alpha-mask image whose mips were built coverage-preserving
    // (RenderVKAlphaCoverageMips); the streamer serves a chain only for the same state.
    enum : U8 { F_ALPHA_MASK = 1, F_COVERAGE_MIPS = 2, F_MASK_VARIANT = 4 };
    static constexpr U32 kMagic = 0x31585447u;   // "GTX1"
};
static_assert(sizeof(LLGpuTexHeader) == 128, "LLGpuTexHeader is 128 bytes on disk");
static_assert(alignof(LLGpuTexHeader) == 8, "LLGpuTexHeader layout");

class LLGpuTexCache
{
public:
    struct Key
    {
        LLUUID id;
        bool mask = false;

        Key(const LLUUID& asset = LLUUID::null, bool mask_use = false) : id(asset), mask(mask_use) {}
        bool operator==(const Key& other) const { return id == other.id && mask == other.mask; }
        friend std::size_t hash_value(const Key& key) noexcept
        {
            return hash_value(key.id) ^ (key.mask ? 0x9e3779b9u : 0u);
        }
    };

    static constexpr U32 FILE_VERSION = 1;
    // SlipstreamGpuTexCacheSizeMB is clamped to this range (the prefs spinner matches).
    static constexpr U32 MIN_BUDGET_MB = 256;
    static constexpr U32 MAX_BUDGET_MB = 65536;

    // What the index knows about a UUID's file (from its name).
    struct Info
    {
        U16 w = 0, h = 0;
        U8  enc = 0, enc_level = 0;
        U32 encoder_version = 0;
        U64 bytes = 0;            // file size
        // #991: the asset's size (under the resolution cap at capture) the chain was captured
        // from; w x h below it = a PARTIAL entry (captured at a higher discard: a complete
        // chain from its own top down, missing the levels above). 0 = unknown (.gtx backend:
        // not in the file name) -- treat as full, as before.
        U16 full_w = 0, full_h = 0;
        bool partial() const { return full_w && full_h && (w < full_w || h < full_h); }
    };
    // A finished read (validated): the chain's header and the levels the read held.
    // hdr describes the servable chain (w x h = its top level, `levels` down to 1 x 1);
    // the pieces hold levels [first_level, endLevel()) in order, each piece a whole number
    // of levels tightly packed (BC blocks row-major / RGBA8 rows). A whole-chain read (a
    // .gtx file, a v1 store record) is one piece from level 0; a per-level read (#865 v2
    // records) is one piece per level record plus the tail, only the levels asked for. The
    // renderer stages the pieces as they are (no reassembly copy).
    struct Blob
    {
        struct Piece
        {
            U32 level = 0;            // first level of hdr's chain this piece holds
            U32 levels = 0;           // levels it holds
            std::vector<U8> bytes;
        };
        LLGpuTexHeader hdr;
        U32 first_level = 0;
        std::vector<Piece> pieces;

        U64 bytes() const
        {
            U64 n = 0;
            for (const Piece& p : pieces) n += p.bytes.size();
            return n;
        }
        U32 endLevel() const { return pieces.empty() ? first_level : pieces.back().level + pieces.back().levels; }
        bool holds(U32 from, U32 to) const { return !pieces.empty() && from >= first_level && to <= endLevel() && from < to; }
    };
    enum EReadState { READ_NONE = 0, READ_PENDING, READ_READY, READ_FAILED };
    // How soon a read is needed (the store backend serves reads by it; same values as
    // LLCache::Priority). The .gtx backend ignores it.
    enum EReadPriority : U8 { READ_PRIO_LOW = 0, READ_PRIO_NORMAL, READ_PRIO_HIGH, READ_PRIO_URGENT };

    static std::string getDefaultDir();

    // Create the folder and start the index scan on the worker. purge = delete every
    // cached file first (Clear Cache); read_only = a second viewer instance: no purge, no
    // temp-file cleanup, no writes. enabled = false: every lookup misses, nothing is written.
    // store = an open segment store: serve everything from it instead (the .gtx folder is
    // left to the migration; purge / read-only are the store's business: SLCacheStore).
    static void init(bool enabled, U64 budget_bytes, bool purge, bool read_only,
                     LLCache::Store* store = nullptr);
    // The store backend is serving (false: the .gtx files, or not initialised).
    static bool usingStore();
    // App shutdown: stop the workers (queued jobs are dropped) and log the stats.
    static void shutdown();
    static bool isEnabled();        // initialised, enabled, scan merged
    static void setEnabled(bool enabled);

    // Main thread, every texture pass: merge the scan, read / write completions, expire
    // results nobody took.
    static void poll();

    // Index lookup (no I/O). False when there is no entry.
    static bool lookup(const Key& id, Info& out);
    // Mark the entry used (LRU; the mtime is refreshed at most hourly).
    static void touch(const Key& id);
    // Drop an entry and its file (e.g. its header disagreed with what the streamer needs).
    static void remove(const Key& id);

    // Start reading id's file on the worker. False when there is no entry, a read for it
    // is already pending / ready, or the in-flight limits say not now.
    // #865 (store backend, v2 records): only the levels a request needs are read --
    // first_level = the levels below the lookup's top (Info w x h) it skips; resident_top =
    // the longer side of the image already resident (0 = none): levels at or below it are
    // not read (the caller copies them from that image). A result may hold MORE than asked
    // (a whole-chain record, the tail's other levels): check Blob::holds(). The .gtx
    // backend always reads the whole chain.
    static bool requestRead(const Key& id, EReadPriority prio = READ_PRIO_NORMAL, U32 first_level = 0,
                            U32 resident_top = 0);
    static EReadState readState(const Key& id);
    // A READY result, moved out (counted as a hit); null otherwise. A FAILED state is
    // cleared by this call too (failed = true).
    static std::shared_ptr<Blob> takeRead(const Key& id, bool& failed);
    // The streamer looked for a texture and the index had nothing usable.
    static void noteMiss();

    // Room for a write of `bytes` now (in-flight write bytes under the cap, enabled, not
    // read-only)?
    static bool canWrite(U64 bytes);
    // Write `bytes` of payload from `src` under `hdr` (the hashes and sizes are filled in
    // here). `src` is read on the worker: it must stay valid until *released is true
    // (set as soon as the bytes are copied off it). A job the pool drops at shutdown
    // never sets it, so the caller must bound that wait. False = not queued: src is the
    // caller's to free at once.
    static bool write(const LLGpuTexHeader& hdr, const U8* src, U64 bytes,
                      const std::shared_ptr<std::atomic<bool>>& released);

    static void setBudget(U64 budget_bytes);

    // ---- #715 reread diagnostics (SlipstreamGpuTexReadDiag, off by default) ----
    // Reads are counted here (both backends); the renderer reports what it did with each
    // result and every residency drop of a texture's image, so the 60 s summary
    // ("GpuTexReadDiag" log lines) can tell streaming (the image shrank / was freed between
    // two reads) from waste (read again with nothing dropped, or a read whose upload did not
    // grow the image). Bounded: at most 65536 tracked textures.
    enum EDiagUse : U8
    {
        USE_FIRST = 0,   // the first image of a texture made from the chain
        USE_UP,          // a residency upgrade from the chain
        USE_REFRESH,     // an equal-size refresh (transcoded / lossy image) from the chain
        USE_COUNT
    };
    static bool diagOn();
    // A taken read result was uploaded: the chain's bytes, the bytes uploaded from it, the
    // image's top size before (0 = none) and after.
    static void diagNoteUse(const Key& id, EDiagUse cause, U64 blob_bytes, U64 used_bytes, U32 top_before,
                            U32 top_after);
    // The texture's resident image shrank (top_after > 0: residency downgrade) or was freed
    // (top_after 0). pressure = texture pressure (budget ladder) at the time.
    static void diagNoteDrop(const Key& id, U32 top_before, U32 top_after, bool pressure);

    // ---- stats (live / cap / peak) ----
    struct Stats
    {
        U64 used = 0, budget = 0, peak = 0;
        U32 entries = 0;
        U64 hits = 0, misses = 0, evictions = 0, corrupt = 0;
        U64 writes = 0, write_fail = 0, bytes_written = 0, bytes_read = 0;
        U32 reads_in_flight = 0, ready = 0;
        U64 read_bytes_in_flight = 0, ready_bytes = 0, write_bytes_in_flight = 0;
        U64 expired = 0;
        bool scanned = false, enabled = false;
    };
    static Stats getStats();
    // #715 metrics: push the .gtx backend's footprint into LLCacheMetrics now (poll() does
    // it every 10 s; no-op on the store backend, whose footprint comes from its stats).
    static void sampleMetrics();
    static const std::string& getDir();
    // Short fragment for the VKMem line.
    static std::string statsLine();
    static void logStats(const std::string& when);
};

#endif // LL_LLGPUTEXCACHE_H
