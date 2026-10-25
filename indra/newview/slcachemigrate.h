/**
 * @file slcachemigrate.h
 * @brief The resumable, throttled folder -> segment store converter shared by the asset
 *        cache and cluster LOD cache migrations (#714, #715). The design of the GPU-ready
 *        migration (slgputexmigrate.h), with a pluggable per-cache Converter and files that
 *        may become several records (a mesh file -> its parts).
 *
 * Pure over a folder and a Store (llcommon + llcachestore only, no viewer globals), so it
 * is unit-tested on a temp folder (tests/slassetmigrate_test.cpp).
 *
 * WHAT IT DOES: lists the folder (names only, on the caller's thread: the viewer's runner
 * thread), deletes the converter's leftovers (e.g. *.part), and converts the files the
 * converter matches NEWEST FIRST (the hot set is usable first; a budget cut drops the
 * coldest). Files it does not match are never touched. Per file:
 *   precheck  the store already has it, or the name is stale -> the file is deleted;
 *   convert   read + validate -> records; a bad file is deleted (it was a cache entry the
 *             old code would have dropped on read); an unreadable one is kept;
 *   write     each record at LOW priority; the file is deleted ONLY once every one of its
 *             records is confirmed (Status OK, or SKIPPED = a newer copy won). On any
 *             failure the records already written for that file are removed again and the
 *             file is kept, so the next start converts it whole.
 * Throttled (read bytes / s, files and bytes in flight); a stop request is honoured between
 * files; NO_SPACE stops the import (the rest waits for the next start). Once the target
 * budget group's live bytes reach `live_cap_bytes`, the remaining (older) files are deleted
 * instead of imported: the store would evict them at once.
 *
 * RESUME: there is no progress file. A crash or stop leaves exactly the unconverted files;
 * the next start lists the folder again and continues with them (converted files are gone,
 * a file whose records landed but whose delete did not is found by precheck and deleted).
 * When nothing is left the folder is removed if empty (remove_dir_when_done), so later
 * starts do not even list it.
 */

#ifndef SL_SLCACHEMIGRATE_H
#define SL_SLCACHEMIGRATE_H

#include "stdtypes.h"
#include "llcachestore.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace SLCacheMigrate
{
    struct Record
    {
        LLCache::Key    key;
        U8              meta[32] = {};
        U8              meta_len = 0;
        std::vector<U8> payload;
    };

    enum class Verdict : U8
    {
        CONVERT,   // precheck: read and convert it
        DONE,      // the store already has it: delete the file
        STALE,     // a name of another format version: delete the file
        OK,        // convert: records produced
        BAD,       // convert: invalid -> delete the file
        IO_ERROR,  // convert: unreadable now -> keep the file
    };

    class Converter
    {
    public:
        virtual ~Converter() = default;
        virtual const char* label() const = 0;                       // "Asset", "Cluster" (log lines)
        virtual bool matches(const std::string& name) const = 0;     // a cache file this converter owns
        virtual bool isLeftover(const std::string& name) const { return false; }   // deleted (e.g. *.part)
        virtual Verdict precheck(const std::string& name, const LLCache::Store& store) const = 0;
        virtual Verdict convert(const std::string& path, const std::string& name, std::vector<Record>& out) const = 0;
        virtual LLCache::Group group() const = 0;                    // budget group of the records
    };

    struct Options
    {
        U64 max_read_bytes_per_sec = 32ull << 20;   // 0 = no throttle
        // Files opened / read / deleted per second (0 = none): the CPU bound once files are small.
        U32 max_files_per_sec = 400;
        // Bounded by BYTES; the count is a sanity cap. A write completes only when the store
        // flushes its run (4 MiB or the 2 s run timer): a 64-file window of small files
        // waited ~2 s per 64 files (32 files/s whatever the throttle said).
        U32 max_files_in_flight = 4096;             // handed to the store, not yet appended
        U64 max_bytes_in_flight = 64ull << 20;
        U64 live_cap_bytes = 0;                     // stop importing at this many live bytes in group() (0 = none)
        const std::atomic<bool>* stop = nullptr;    // stop request (checked between files)
        bool remove_dir_when_done = true;
        F64 progress_log_interval_s = 30.0;         // 0 = no progress lines
        U32 stop_after_files = 0;                   // test hook: behave as stopped after this many files (0 = off)
    };

    struct Result
    {
        U32 files = 0;            // matching files found
        U32 converted = 0;        // files converted (records confirmed, file deleted)
        U32 records = 0;          // records written for them
        U32 already = 0;          // the store already had it (file deleted)
        U32 stale = 0;            // another format version (file deleted)
        U32 bad = 0;              // invalid / leftovers (file deleted)
        U32 over_budget = 0;      // past live_cap_bytes (file deleted, not imported)
        U32 failed = 0;           // write refused / failed / unreadable (file kept for the next start)
        U32 left = 0;             // not processed (stop / disk full)
        U64 bytes_read = 0;       // file bytes read
        U64 bytes_converted = 0;  // payload bytes of the confirmed records
        F64 seconds = 0.0;
        bool stopped = false, disk_full = false;
        bool completed = false;   // nothing left to do
    };

    // Any file in `dir` the converter matches or treats as a leftover (a listing, no open).
    bool hasWork(const std::string& dir, const Converter& conv);

    // Convert `dir` into `store` (open, ready, writable). Blocks; run it on a background thread.
    Result migrateDir(const std::string& dir, LLCache::Store& store, const Converter& conv, const Options& opt);

    std::string resultLine(const Result& r);

    // The viewer's background runner: waits for the store's index, lets login go first
    // (start_delay_s), then migrateDir() on its own thread; logs "<label> migration
    // done/paused: ..." and calls on_done (on the runner thread) with the result.
    class Runner
    {
    public:
        ~Runner();
        void start(const std::string& dir, LLCache::Store* store, std::shared_ptr<const Converter> conv,
                   const Options& opt, F64 start_delay_s, std::function<void(const Result&)> on_done = nullptr);
        void stop();   // request + join (before the store closes)
        bool running() const { return mThread.joinable() && !mDone.load(); }

    private:
        std::thread mThread;
        std::atomic<bool> mStop{ false };
        std::atomic<bool> mDone{ false };
    };
}

#endif // SL_SLCACHEMIGRATE_H
