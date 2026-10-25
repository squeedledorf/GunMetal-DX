/**
 * @file slj2cmigrate.h
 * @brief Migration of the legacy J2C texture cache (<cache>/texturecache: texture.entries,
 *        texture.cache and the [0-f]/<uuid>.texture bodies) into the segment store
 *        (#714, #715).
 *
 * Pure over a folder and a Store (llcommon + llcachestore only, no viewer globals), so
 * the converter is unit-tested on a temp folder (tests/slj2cmigrate_test.cpp).
 *
 * LEGACY LAYOUT (lltexturecache.cpp): texture.entries = a 44-byte EntriesInfo {F32
 * version, U32 address size, char[32] J2C engine string, U32 entry count} followed by
 * 28-byte entries {UUID, S32 image size, S32 body size, U32 last access}; an entry is
 * live when image size > body size and image size > 0. texture.cache = one 600-byte
 * stripe per entry slot (the first 600 bytes of the codestream, zero padded). Bodies =
 * bytes 600.. in <dir>/<first hex digit>/<uuid>.texture. The cached prefix is
 * stripe + body when the body size is > 0, else min(600, image size) bytes of the stripe.
 *
 * WHAT IT DOES: loads texture.entries (Legacy::open), then converts the live entries
 * newest first (by last access: the hot set is usable first). Each prefix (stripe +
 * body, the body's size must match the entry) is written as one J2C record at LOW
 * priority with the entry's image size as meta. Once the store reports the record
 * appended (OK), or superseded by a longer prefix written meanwhile (SKIPPED), the body
 * file is deleted and the entry is marked free in texture.entries (image size -1, the
 * legacy cache's own convention; written in batches). A key the store already holds with
 * at least as many bytes is not imported (its legacy files are dropped); a shorter one is
 * replaced by the legacy prefix (WF_SKIP_IF_NOT_LARGER guards a race). A bad entry (body
 * missing / wrong size, stripe short, sizes inconsistent) is dropped. Throttled (read
 * bytes / s, files and bytes in flight); stops at once on a stop request; stops importing
 * on NO_SPACE (the store's disk-full guard) and leaves the rest for the next start. Once
 * the store's J2C live bytes reach `live_cap_bytes` (the J2C budget) the remaining,
 * older entries are dropped instead of imported: the store would evict them anyway.
 * A version or address-size mismatch in EntriesInfo (a format this build never wrote)
 * drops the whole legacy cache, as the legacy cache itself would have. A J2C engine
 * string mismatch does NOT: the cache holds the server's codestream bytes, which do not
 * depend on the local decoder (the legacy cache wiped itself on that change).
 *
 * RESUME: texture.entries is the progress record. A converted entry is marked free (and
 * its body deleted) only after the store confirmed the write, so a crash or a stop
 * leaves only unconverted entries live; the next start resumes with them. An entry
 * whose free mark was lost in a crash (batch not yet written) is still live but the
 * store already holds its key: it is recognised as "already" and dropped. Because the
 * marks use the legacy format, turning CacheStoreJ2C off mid-migration leaves a
 * consistent (smaller) legacy cache for the legacy backend.
 *
 * COMPLETION MARKER: the folder. When no live entry is left, texture.entries,
 * texture.cache and the body folders are deleted (the whole <dir>), so later starts find
 * nothing to do without reading anything (hasWork() is false).
 *
 * READ-THROUGH: while the migration has not reached an entry, Legacy::read() serves it
 * to LLTextureCache's store backend on a store miss (which then writes it into the store),
 * so the first session after the switch does not refetch the unconverted part of the
 * cache from the network.
 */

#ifndef SL_SLJ2CMIGRATE_H
#define SL_SLJ2CMIGRATE_H

#include "stdtypes.h"
#include "lluuid.h"
#include "llcachestore.h"
#include "llfile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SLJ2CMigrate
{
    constexpr S32 STRIPE_BYTES = 600;           // TEXTURE_CACHE_ENTRY_SIZE == FIRST_PACKET_SIZE
    constexpr U32 ENTRIES_INFO_BYTES = 44;      // sizeof(LLTextureCache::EntriesInfo)
    constexpr U32 ENTRY_BYTES = 28;             // sizeof(LLTextureCache::Entry)

    struct Options
    {
        // The legacy header this build writes (LLTextureCache::sHeaderCacheVersion /
        // sHeaderCacheAddressSize). Anything else is dropped, never imported.
        F32 expected_version = 1.71f;
        U32 expected_address_size = 64;
        // Throttle (0 = none). J2C records are stored as is (no codec): the store's writer
        // only copies, so this mainly keeps the disk free for live reads.
        U64 max_read_bytes_per_sec = 48ull << 20;
        // Entries read / dropped per second (0 = none): the CPU bound once entries are small
        // (per-entry body open / read / delete + free-mark bookkeeping).
        U32 max_files_per_sec = 400;
        // Bounded by BYTES; the count is a sanity cap. A write completes only when the store
        // flushes its run (4 MiB or the 2 s run timer): a 64-entry window of small entries
        // waited ~2 s per 64 entries (32 entries/s whatever the throttle said).
        U32 max_files_in_flight = 4096;             // handed to the store, not yet appended
        U64 max_bytes_in_flight = 64ull << 20;
        U64 live_cap_bytes = 0;                     // stop importing at this many J2C live bytes (0 = no cap)
        const std::atomic<bool>* stop = nullptr;    // stop request (checked between entries)
        bool remove_dir_when_done = true;
        U32 free_mark_batch = 256;                  // entries marked free per texture.entries write
        F64 progress_log_interval_s = 30.0;         // 0 = no progress lines
    };

    struct Result
    {
        U32 entries = 0;          // live legacy entries found
        U32 converted = 0;        // records written (legacy files dropped)
        U32 superseded = 0;       // the store got a longer prefix meanwhile (dropped)
        U32 already = 0;          // the store already had >= the legacy prefix (dropped)
        U32 bad = 0;              // inconsistent / unreadable entry (dropped)
        U32 over_budget = 0;      // past live_cap_bytes (dropped, not imported)
        U32 failed = 0;           // write refused / failed (kept for the next start)
        U32 left = 0;             // not processed (stop / disk full)
        U64 bytes_read = 0;       // legacy bytes read (stripes + bodies)
        U64 bytes_converted = 0;  // payload bytes of the converted records
        F64 seconds = 0.0;
        bool stopped = false, disk_full = false;
        bool version_mismatch = false;   // legacy header from another format: all dropped
        bool completed = false;   // nothing left to do; folder removed (if asked)
    };

    // texture.entries exists in `dir` (one stat, nothing opened).
    bool hasWork(const std::string& dir);
    std::string resultLine(const Result& r);

    // The opened legacy cache. Thread-safe: the migration and LLTextureCache's read-through
    // use it concurrently (one mutex around every legacy file operation).
    class Legacy
    {
    public:
        ~Legacy();

        // Read texture.entries. False when it is missing or unreadable. A header of
        // another version / address size opens with no live entries and
        // versionMismatch() true.
        bool open(const std::string& dir, const Options& opt);
        void close();
        bool isOpen() const;
        bool versionMismatch() const { return mVersionMismatch; }
        const std::string& dir() const { return mDir; }

        // Read-through (any thread): the whole cached prefix of `id` and its image size.
        // False when the id is not (or no longer) a live legacy entry, or its files are bad.
        bool read(const LLUUID& id, std::vector<U8>& out, S32& image_size);
        // LLTextureCache::removeFromCache(): the texture was found bad (or removed) this
        // session; drop its legacy files too, so the migration never re-imports it.
        void forget(const LLUUID& id);

        // ---- migration side
        struct Entry
        {
            LLUUID id;
            S32 image_size = 0;
            S32 body_size = 0;
            U32 time = 0;
            U32 idx = 0;
            S32 prefixBytes() const { return body_size > 0 ? STRIPE_BYTES + body_size : std::min(STRIPE_BYTES, image_size); }
        };
        // The live entries, newest first.
        std::vector<Entry> liveEntriesNewestFirst() const;
        enum class ReadStatus : U8 { OK, GONE, BAD, IO_ERROR };
        ReadStatus readEntry(const Entry& e, std::vector<U8>& out);
        // Delete the body (if any) and mark the slot free (batched). Idempotent.
        void drop(const Entry& e);
        // Write the batched free marks now.
        void flushFreeMarks();
        U32 liveCount() const;
        // Delete the whole legacy cache folder (closes it first).
        bool removeAll();

    private:
        ReadStatus readLocked(const Entry& e, std::vector<U8>& out);
        void dropLocked(const Entry& e);
        void flushLocked();
        std::string bodyPath(const LLUUID& id) const;

        mutable std::mutex mMutex;
        std::string mDir;
        bool mOpen = false;
        bool mVersionMismatch = false;
        U32 mFreeBatch = 256;
        std::unordered_map<LLUUID, Entry> mLive;
        std::vector<U32> mPendingFree;
        LLFILE* mStripes = nullptr;   // texture.cache, read only
        LLFILE* mEntries = nullptr;   // texture.entries, r+b (free marks)
    };

    // Convert `legacy` into `store` (open, ready, writable). Blocks; run it on a
    // background thread.
    Result migrate(Legacy& legacy, LLCache::Store& store, const Options& opt);

    // The viewer's background runner: opens the legacy cache at once (on its own thread)
    // and hands it to `on_open` for the read-through, waits for the store's index, lets
    // login go first (start_delay_s), then migrate(); logs the result. on_open(nullptr)
    // runs when the legacy cache is closed (done, stopped).
    class Runner
    {
    public:
        using OnOpen = std::function<void(const std::shared_ptr<Legacy>&)>;
        ~Runner();
        void start(const std::string& dir, LLCache::Store* store, const Options& opt, F64 start_delay_s,
                   OnOpen on_open);
        void stop();   // request + join (before the store closes)
        bool running() const { return mThread.joinable() && !mDone.load(); }

    private:
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::atomic<bool> mDone{ false };
    };
}

#endif // SL_SLJ2CMIGRATE_H
