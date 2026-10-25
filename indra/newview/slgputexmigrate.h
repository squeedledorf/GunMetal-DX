/**
 * @file slgputexmigrate.h
 * @brief Migration of the old GPU-ready texture cache (the .gtx files in <cache>/gputex, #274) into
 *        the segment store (#714, #715).
 *
 * Pure over a folder and a Store (llcommon + llcachestore only, no viewer globals), so
 * the converter is unit-tested on a temp folder (tests/slgputexmigrate_test.cpp).
 *
 * WHAT IT DOES: lists the folder (names only), deletes *.gtx.part leftovers and names
 * that do not parse, and converts the .gtx files newest first (the hot set is usable
 * first). Each file is validated exactly like the old cache's read (magic, version,
 * header hash, UUID + mask flag against the name, size against the file, payload hash)
 * and must agree with its name; a bad file is deleted (it was rebuildable and the old
 * cache would have deleted it on read), and so is one whose payload is not the full mip
 * chain its header describes. A good one is written directly in the per-level layout
 * (#865 v2: level records above 256 px, then the tail; llgputexrecord.h) at LOW priority;
 * the file is deleted once the store reports every record appended (OK). A chain the
 * store already has is not imported (the store's records are newer) unless the file is a
 * finer version of the same encoding -- then only its levels above the store's top are
 * written. Throttled (read bytes / s, bytes and files
 * in flight); stops at once on a stop request; stops importing on NO_SPACE (the store's
 * disk-full guard) and leaves the remaining files for the next start. Once the store's
 * GPU-ready bytes reach `live_cap_bytes` (its budget), the remaining (older) files are
 * deleted instead of imported: the store would evict them straight away.
 *
 * COMPLETION MARKER: the folder itself. When every .gtx file is gone the folder is
 * removed, so the next start finds nothing to do without scanning (hasWork() is false);
 * a crash mid-way leaves only unconverted files, so the next start resumes with them.
 * If the file backend is ever selected again (CacheStoreGpuTex off), it recreates the
 * folder and its files are migrated the next time the store backend runs.
 */

#ifndef SL_SLGPUTEXMIGRATE_H
#define SL_SLGPUTEXMIGRATE_H

#include "stdtypes.h"
#include "lluuid.h"
#include "llcachestore.h"
#include "llgputexcache.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace SLGpuTexMigrate
{
    // "<uuid>_<w>x<h>_e<enc>_l<level>_v<encoder version>[_mask].gtx" (the old file name;
    // must round-trip exactly, like the old cache's parse_name).
    struct ParsedName
    {
        LLUUID id;
        bool mask = false;
        U16 w = 0, h = 0;
        U8 enc = 0, enc_level = 0;
        U32 encoder_version = 0;
    };
    bool parseName(const std::string& name, ParsedName& out);
    std::string makeName(const ParsedName& n);

    // NOT_A_CHAIN: the payload is not the full mip chain its header describes in a format
    // the record layout knows (the streamer would have rejected it on read anyway).
    enum class FileStatus : U8 { OK, BAD_NAME, TOO_SMALL, BAD_HEADER, NAME_MISMATCH, BAD_HASH, IO_ERROR, NOT_A_CHAIN };
    const char* fileStatusName(FileStatus s);

    struct Record
    {
        LLUUID id;
        bool mask = false;
        LLGpuTexHeader hdr;        // the file's header (validated)
        std::vector<U8> payload;   // the mip chain (the file minus its 128-byte header)
    };
    // Read + validate one .gtx file (`name` is its file name). Written to the store as
    // per-level (#865 v2) records: LLGpuTexRecord::planWrite / writeParts.
    FileStatus readFile(const std::string& path, const std::string& name, Record& out);

    struct Options
    {
        // Throttle (0 = none). The store zstd-encodes on its one writer thread (~50 MB/s
        // raw measured on real .gtx files): half of it is left for live captures.
        U64 max_read_bytes_per_sec = 24ull << 20;
        // Files opened / read / deleted per second (0 = none): the CPU bound once files are
        // small (~90-140 us of CPU per file measured, so 400/s stays at ~5% of one core,
        // what a 24 MB/s run of large files costs).
        U32 max_files_per_sec = 400;
        // The window is bounded by BYTES (RAM); the file count is only a sanity cap. It must
        // not be small: a write completes when the store flushes its run (4 MiB of records,
        // or the 2 s run timer), so a 32-file window of small files waited ~2 s per 32 files
        // (16 files/s whatever the throttle said: the 2026-10-19 migration ran at 5.4 MB/s).
        U32 max_files_in_flight = 4096;             // handed to the store, not yet appended
        U64 max_bytes_in_flight = 64ull << 20;
        U64 live_cap_bytes = 0;                     // stop importing at this many GPUTEX live bytes (0 = no cap)
        const std::atomic<bool>* stop = nullptr;    // stop request (checked between files)
        bool remove_dir_when_done = true;
        F64 progress_log_interval_s = 30.0;         // 0 = no progress lines
    };

    struct Result
    {
        U32 files = 0;            // .gtx files found
        U32 converted = 0;        // records written (file deleted)
        U32 superseded = 0;       // the store got a newer capture meanwhile (file deleted)
        U32 already = 0;          // the store already had the key (file deleted)
        U32 bad = 0;              // invalid / unparseable / leftover .part (file deleted)
        U32 over_budget = 0;      // past live_cap_bytes (file deleted, not imported)
        U32 failed = 0;           // write refused / failed (file kept for the next start)
        U32 left = 0;             // not processed (stop / disk full)
        U64 bytes_read = 0;       // .gtx bytes read
        U64 bytes_converted = 0;  // payload bytes of the converted records (raw)
        F64 seconds = 0.0;
        bool stopped = false, disk_full = false;
        bool completed = false;   // nothing left to do; folder removed (if asked)
    };

    // Any .gtx / .gtx.part file in `dir` (a directory listing, no file opened).
    bool hasWork(const std::string& dir);

    // Convert `dir` into `store` (which must be open, ready and writable). Blocks; run it
    // on a background thread.
    Result migrateDir(const std::string& dir, LLCache::Store& store, const Options& opt);

    std::string resultLine(const Result& r);

    // The viewer's background runner: waits for the store's index, lets login go first
    // (start_delay_s), then migrateDir() on its own thread; logs the result.
    class Runner
    {
    public:
        ~Runner();
        void start(const std::string& dir, LLCache::Store* store, const Options& opt, F64 start_delay_s);
        void stop();   // request + join (before the store closes)
        bool running() const { return mThread.joinable() && !mDone.load(); }

    private:
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::atomic<bool> mDone{ false };
    };
}

#endif // SL_SLGPUTEXMIGRATE_H
