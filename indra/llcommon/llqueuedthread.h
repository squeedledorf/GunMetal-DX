/**
 * @file llqueuedthread.h
 * @brief
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
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

#ifndef LL_LLQUEUEDTHREAD_H
#define LL_LLQUEUEDTHREAD_H

#include <atomic>
#include <chrono>
#include <deque>
#include <queue>
#include <string>
#include <map>
#include <set>

#include "llatomic.h"

#include "llthread.h"
#include "llsimplehash.h"
#include "workqueue.h"

//============================================================================
// Note: ~LLQueuedThread is O(N) N=# of queued threads, assumed to be small
//   It is assumed that LLQueuedThreads are rarely created/destroyed.

class LL_COMMON_API LLQueuedThread : public LLThread
{
    //------------------------------------------------------------------------
public:
    enum status_t {
        STATUS_EXPIRED = -1,
        STATUS_UNKNOWN = 0,
        STATUS_QUEUED = 1,
        STATUS_INPROGRESS = 2,
        STATUS_COMPLETE = 3,
        STATUS_ABORTED = 4,
        STATUS_DELETE = 5
    };
    enum flags_t {
        FLAG_AUTO_COMPLETE = 1,
        FLAG_AUTO_DELETE = 2, // child-class dependent
        FLAG_ABORT = 4
    };

    typedef U32 handle_t;

    //------------------------------------------------------------------------
public:

    class LL_COMMON_API QueuedRequest : public LLSimpleHashEntry<handle_t>
    {
        friend class LLQueuedThread;

    protected:
        virtual ~QueuedRequest(); // use deleteRequest()

    public:
        QueuedRequest(handle_t handle, U32 flags = 0);

        status_t getStatus()
        {
            return mStatus;
        }
        U32 getFlags() const
        {
            return mFlags;
        }

    protected:
        status_t setStatus(status_t newstatus)
        {
            status_t oldstatus = mStatus;
            mStatus = newstatus;
            return oldstatus;
        }
        void setFlags(U32 flags)
        {
            // NOTE: flags are |'d
            mFlags |= flags;
        }

        virtual bool processRequest() = 0; // Return true when request has completed
        virtual void finishRequest(bool completed); // Always called from thread after request has completed or aborted
        virtual void deleteRequest(); // Only method to delete a request

        // #299/#306: called from processRequest() before it returns false: "I am waiting
        // for an event whose source will call LLQueuedThread::wakeRequest()". Instead of
        // re-running every RETRY_DELAY the request then sleeps until woken, or until
        // PARK_TIMEOUT as a safety net (a missed wake costs latency, never a stuck request).
        // 'reason' tags the wait (e.g. the worker's state): after a PARK_TIMEOUT re-run, a
        // re-park with the same reason means the event simply hasn't happened yet (a slow
        // event, not a missed wake); anything else means the wake was missed.
        void requestPark(U32 reason = 0) { mParkRequested = true; mRequestedParkReason = reason; }

        // #299: names a park reason for the missed-wake debug log; nullptr = unnamed
        virtual const char* getParkReasonName(U32 /*reason*/) const { return nullptr; }

    protected:
        LLAtomicBase<status_t> mStatus;
        U32 mFlags;

    private:
        // #299: all under the owning thread's data lock (lockData())
        U32  mRetryGen = 0;             // bumped per (re)post; a post carrying an older value is stale
        bool mParkRequested = false;    // set by requestPark() during processRequest()
        bool mParked = false;           // QUEUED and waiting for wakeRequest()
        bool mWakePending = false;      // woken while INPROGRESS: don't park on this return
        bool mParkExpired = false;      // this run was started by expireParked(), not by a wake
        U32  mRequestedParkReason = 0;  // set by requestPark() during processRequest()
        U32  mParkReason = 0;           // reason of the current / last park
    };

    //------------------------------------------------------------------------

public:
    static handle_t nullHandle() { return handle_t(0); }

public:
    LLQueuedThread(const std::string& name, bool threaded = true, bool should_pause = false);
    virtual ~LLQueuedThread();
    virtual void shutdown();

private:
    // No copy constructor or copy assignment
    LLQueuedThread(const LLQueuedThread&);
    LLQueuedThread& operator=(const LLQueuedThread&);

    virtual bool runCondition(void);
    virtual void run(void);
    virtual void startThread(void);
    virtual void endThread(void);
    virtual void threadedUpdate(void);
    // subclasses overriding threadedUpdate() must return true so updateQueue() schedules it
    virtual bool hasThreadedUpdate() const { return false; }

protected:
    handle_t generateHandle();
    bool addRequest(QueuedRequest* req);
    void processRequest(QueuedRequest* req);
    void incQueue();
    // #299: run the request 'handle' if it is still QUEUED and 'gen' is its current
    // post (every post goes through here, so a stale or duplicate post is a no-op).
    void retryRequest(handle_t handle, U32 gen);
    // Locks: data lock held. Make a QUEUED request's next post the current one (un-park
    // it); returns the generation to post with (post AFTER unlocking).
    U32 prepareRepostLocked(QueuedRequest* req);
    void postNow(handle_t handle, U32 gen);
    // #299: MAIN thread (updateQueue): re-run parked requests nobody woke in PARK_TIMEOUT
    void expireParked();

public:
    // #299: a request re-runs this long after returning false (it used to sleep the
    // whole thread for it), or after PARK_TIMEOUT if it parked (requestPark()).
    static constexpr std::chrono::milliseconds RETRY_DELAY{ 16 };
    static constexpr std::chrono::milliseconds PARK_TIMEOUT{ 250 };

    // #299: the event a parked request waits for has happened: run it now. Safe from any
    // thread. A request that is running gets "don't park this time" instead (no lost
    // wake). Returns true if a parked/queued request was re-posted.
    // parked_only: leave a request that is QUEUED for an ordinary RETRY_DELAY retry alone
    // (still wakes a parked one -- or one whose PARK_TIMEOUT re-run hasn't started -- and
    // tells a running one not to park).
    bool wakeRequest(handle_t handle, bool parked_only = false);

    // #299 counters (any thread): requests parked right now, wakes delivered, and what
    // the PARK_TIMEOUT re-runs found:
    //  - getParkTimeoutCount(): MISSED WAKES -- the re-run made progress (or parked for a
    //    different reason), so the event had happened and its source didn't wake the
    //    request. Should stay ~0; each one cost up to PARK_TIMEOUT of latency.
    //  - getParkRecheckCount(): the re-run found the event still pending and re-parked in
    //    the same wait (slow event: a long decode queue, HTTP waiters, slow replies). No
    //    latency cost; scales with (parked requests) x (wait time / PARK_TIMEOUT).
    S32 getParkedCount() const { return mParkedCount; }
    U64 getWakeCount() const { return mWakeCount; }
    U64 getParkTimeoutCount() const { return mParkTimeoutCount; }
    U64 getParkRecheckCount() const { return mParkRecheckCount; }

    bool waitForResult(handle_t handle, bool auto_complete = true);

    virtual size_t update(F32 max_time_ms);
    size_t updateQueue(F32 max_time_ms);

    void waitOnPending();
    void printQueueStats();

    virtual size_t getPending();
    bool getThreaded() { return mThreaded; }

    // Request accessors
    status_t getRequestStatus(handle_t handle);
    void abortRequest(handle_t handle, bool autocomplete);
    void setFlags(handle_t handle, U32 flags);
    bool completeRequest(handle_t handle);
    // This is public for support classes like LLWorkerThread,
    // but generally the methods above should be used.
    QueuedRequest* getRequest(handle_t handle);

    // debug (see source)
    bool check();

protected:
    bool mThreaded;  // if false, run on main thread and do updates during update()
    bool mStarted;  // required when mThreaded is false to call startThread() from update()
    LLAtomicBool mIdleThread; // request queue is empty (or we are quitting) and the thread is idle

    //typedef std::set<QueuedRequest*, queued_request_less> request_queue_t;
    //request_queue_t mRequestQueue;
    // #299: a WorkSchedule (time-ordered) so a retry can be posted for later without
    // sleeping the thread: new work posted "now" runs ahead of not-yet-due retries.
    LL::WorkSchedule mRequestQueue;
    LL::WorkQueue::weak_t mMainQueue;

    std::atomic<S32> mParkedCount{ 0 };
    std::atomic<U64> mWakeCount{ 0 };
    std::atomic<U64> mParkTimeoutCount{ 0 };   // missed wakes (see getParkTimeoutCount)
    std::atomic<U64> mParkRecheckCount{ 0 };   // safety-net re-runs that re-parked (slow event)

    // #299: queue thread only, never under the data lock. Counts a missed wake and logs
    // it (LL_DEBUGS("QueuedThread"), at most once a second). reason_name must be a
    // static string (getParkReasonName()), as the request may be gone by now.
    void noteMissedWake(const char* reason_name, U32 reason, const char* outcome);
    F64 mMissedWakeLogTime = 0.0;
    U32 mMissedWakeLogSuppressed = 0;

    // #299: parks in the order they happened (so due times are ascending), under the
    // data lock. Entries are checked lazily against the request's current generation,
    // so a woken request's entry just expires; bounded by parks per PARK_TIMEOUT.
    // (The safety timeout is NOT a timed post: ThreadSafeSchedule computes its wait
    // deadline from the head item when the wait starts, so a post due sooner than the
    // head would wait for the head. Only this thread posts future-dated work, all with
    // the same RETRY_DELAY, so a new one is never due before the current head.)
    struct ParkEntry
    {
        LL::WorkSchedule::TimePoint mDue;
        handle_t mHandle;
        U32 mGen;
    };
    std::deque<ParkEntry> mParkedList;

    enum { REQUEST_HASH_SIZE = 512 }; // must be power of 2
    typedef LLSimpleHash<handle_t, REQUEST_HASH_SIZE> request_hash_t;
    request_hash_t mRequestHash;

    handle_t mNextHandle;
};

#endif // LL_LLQUEUEDTHREAD_H
