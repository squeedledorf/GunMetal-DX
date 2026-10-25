/**
 * @file llqueuedthread.cpp
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

#include "linden_common.h"
#include "llqueuedthread.h"

#include <chrono>
#include <vector>

#include "llstl.h"
#include "lltimer.h"    // ms_sleep()
#include "llmutex.h"

//============================================================================

// MAIN THREAD
LLQueuedThread::LLQueuedThread(const std::string& name, bool threaded, bool should_pause) :
    LLThread(name),
    mIdleThread(true),
    mNextHandle(0),
    mStarted(false),
    mThreaded(threaded),
    mRequestQueue(name, 1024 * 1024)
{
    llassert(threaded); // not threaded implementation is deprecated
    mMainQueue = LL::WorkQueue::getInstance("mainloop");

    if (mThreaded)
    {
        if(should_pause)
        {
            pause() ; //call this before start the thread.
        }

        start();
    }
}

// MAIN THREAD
LLQueuedThread::~LLQueuedThread()
{
    if (!mThreaded)
    {
        endThread();
    }
    shutdown();
    // ~LLThread() will be called here
}

void LLQueuedThread::shutdown()
{
    setQuitting();

    unpause(); // MAIN THREAD
    if (mThreaded)
    {
        if (mRequestQueue.size() == 0)
        {
            mRequestQueue.close();
        }

        S32 timeout = 100;
        for ( ; timeout>0; timeout--)
        {
            if (isStopped())
            {
                break;
            }
            ms_sleep(100);
            LLThread::yield();
        }
        if (timeout == 0)
        {
            LL_WARNS() << "~LLQueuedThread (" << mName << ") timed out!" << LL_ENDL;
        }
    }
    else
    {
        mStatus = STOPPED;
    }

    QueuedRequest* req;
    S32 active_count = 0;
    while ( (req = (QueuedRequest*)mRequestHash.pop_element()) )
    {
        if (req->getStatus() == STATUS_QUEUED || req->getStatus() == STATUS_INPROGRESS)
        {
            ++active_count;
            req->setStatus(STATUS_ABORTED); // avoid assert in deleteRequest
        }
        req->deleteRequest();
    }
    if (active_count)
    {
        LL_WARNS() << "~LLQueuedThread() called with active requests: " << active_count << LL_ENDL;
    }
    lockData();
    mParkedList.clear();
    mParkedCount = 0;
    unlockData();

    mRequestQueue.close();
}

//----------------------------------------------------------------------------

// MAIN THREAD
// virtual
size_t LLQueuedThread::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mStarted)
    {
        if (!mThreaded)
        {
            startThread();
            mStarted = true;
        }
    }
    return updateQueue(max_time_ms);
}

size_t LLQueuedThread::updateQueue(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED;
    // Frame Update
    if (mThreaded)
    {
        expireParked();     // #299: the parked requests' safety net

        // schedule a call to threadedUpdate for every call to updateQueue,
        // only for subclasses that actually override it
        if (hasThreadedUpdate() && !isQuitting())
        {
            mRequestQueue.post([=, this]()
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_THREAD("qt - update");
                    mIdleThread = false;
                    threadedUpdate();
                    mIdleThread = true;
                }
            );
        }

        // Threads can be created paused (TextureFetch) and are only woken here:
        // keep the unpause, it is what starts them (#375 regression: startup hang).
        if (getPending() > 0)
        {
            unpause();
        }
    }
    else
    {
        mRequestQueue.runFor(std::chrono::microseconds((int) (max_time_ms*1000.f)));
        threadedUpdate();
    }
    return getPending();
}

void LLQueuedThread::incQueue()
{
    // Something has been added to the queue
    if (!isPaused())
    {
        if (mThreaded)
        {
            wake(); // Wake the thread up if necessary.
        }
    }
}

//virtual
// May be called from any thread
size_t LLQueuedThread::getPending()
{
    // #299: parked requests aren't in the queue but are still pending work
    return mRequestQueue.size() + (size_t)llmax(0, (S32)mParkedCount);
}

// MAIN thread
void LLQueuedThread::waitOnPending()
{
    while(1)
    {
        update(0);

        if (mIdleThread)
        {
            break;
        }
        if (mThreaded)
        {
            yield();
        }
    }
    return;
}

// MAIN thread
void LLQueuedThread::printQueueStats()
{
    auto size = mRequestQueue.size();
    if (size > 0)
    {
        LL_INFOS() << llformat("Pending Requests:%d ", mRequestQueue.size()) << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "Queued Thread Idle" << LL_ENDL;
    }
}

// MAIN thread
LLQueuedThread::handle_t LLQueuedThread::generateHandle()
{
    U32 res = ++mNextHandle;
    return res;
}

// MAIN thread
bool LLQueuedThread::addRequest(QueuedRequest* req)
{
    LL_PROFILE_ZONE_SCOPED;
    if (mStatus == QUITTING)
    {
        return false;
    }

    lockData();
    req->setStatus(STATUS_QUEUED);
    mRequestHash.insert(req);
#if _DEBUG
//  LL_INFOS() << llformat("LLQueuedThread::Added req [%08d]",handle) << LL_ENDL;
#endif
    unlockData();

    llassert(!mDataLock->isSelfLocked());
    // #299: by handle + generation like every other post, so a wake that re-posts the
    // request before this first post runs can't make it run twice (or after deletion).
    const handle_t handle = req->getHashKey();
    lockData();
    const U32 gen = ++req->mRetryGen;
    unlockData();
    mRequestQueue.post([this, handle, gen]() { retryRequest(handle, gen); });

    return true;
}

// Runs on its OWN thread (posted by addRequest / processRequest / wakeRequest)
void LLQueuedThread::retryRequest(handle_t handle, U32 gen)
{
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    if (!req || req->mRetryGen != gen || req->getStatus() != STATUS_QUEUED)
    {
        // stale: superseded by a newer post (wake), or completed / deleted meanwhile
        unlockData();
        return;
    }
    llassert(!req->mParked);   // parked requests have no live post (see expireParked)
    unlockData();
    // Only this thread moves a request out of QUEUED, and a QUEUED request is never
    // deleted (completeRequest asserts it isn't), so req stays valid.
    processRequest(req);
}

// Locks: data lock held
U32 LLQueuedThread::prepareRepostLocked(QueuedRequest* req)
{
    if (req->mParked)
    {
        req->mParked = false;
        --mParkedCount;
    }
    return ++req->mRetryGen;
}

// Any thread; never under the data lock (a full queue blocks the poster until the
// consumer -- which takes the data lock -- drains it)
void LLQueuedThread::postNow(handle_t handle, U32 gen)
{
    mRequestQueue.post([this, handle, gen]() { retryRequest(handle, gen); });
}

// MAIN thread
void LLQueuedThread::expireParked()
{
    const auto now = LL::WorkSchedule::TimePoint::clock::now();
    std::vector<std::pair<handle_t, U32> > expired;
    lockData();
    while (!mParkedList.empty() && mParkedList.front().mDue <= now)
    {
        const ParkEntry e = mParkedList.front();
        mParkedList.pop_front();
        QueuedRequest* req = (QueuedRequest*)mRequestHash.find(e.mHandle);
        if (req && req->mParked && req->mRetryGen == e.mGen && req->getStatus() == STATUS_QUEUED)
        {
            // nobody woke it: either the event hasn't happened yet (slow event) or its
            // source missed the wake. Run it anyway; processRequest() tells the two apart
            // by what the run does (see getParkTimeoutCount / getParkRecheckCount).
            req->mParkExpired = true;
            expired.emplace_back(e.mHandle, prepareRepostLocked(req));
        }
    }
    unlockData();
    for (const auto& h : expired)
    {
        postNow(h.first, h.second);
    }
}

// Queue thread; never under the data lock
void LLQueuedThread::noteMissedWake(const char* reason_name, U32 reason, const char* outcome)
{
    ++mParkTimeoutCount;
    // rate-limited: at most one line a second (with how many were folded into it)
    const F64 now = LLTimer::getTotalSeconds().value();
    if (now - mMissedWakeLogTime < 1.0)
    {
        ++mMissedWakeLogSuppressed;
        return;
    }
    mMissedWakeLogTime = now;
    const U32 suppressed = mMissedWakeLogSuppressed;
    mMissedWakeLogSuppressed = 0;
    LL_DEBUGS("QueuedThread") << mName << ": missed wake -- request parked in "
                              << (reason_name ? reason_name : "?") << " (" << reason << ")"
                              << " was only resumed by the " << PARK_TIMEOUT.count()
                              << " ms park timeout, then " << outcome
                              << " (+" << suppressed << " more since the last report)" << LL_ENDL;
}

// Any thread
bool LLQueuedThread::wakeRequest(handle_t handle, bool parked_only)
{
    if (handle == nullHandle())
    {
        return false;
    }
    bool posted = false;
    U32 gen = 0;
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    if (req)
    {
        const status_t status = req->getStatus();
        if (status == STATUS_QUEUED && (req->mParked || req->mParkExpired || !parked_only))
        {
            // parked, or waiting out RETRY_DELAY (or a PARK_TIMEOUT re-run that hasn't
            // started yet): either way run it now. The wake was delivered, so this run
            // is not a park-timeout re-run any more.
            gen = prepareRepostLocked(req);
            req->mParkExpired = false;
            posted = true;
        }
        else if (status == STATUS_INPROGRESS)
        {
            // it's running right now and may be about to park: tell it not to
            req->mWakePending = true;
        }
    }
    unlockData();
    if (posted)
    {
        postNow(handle, gen);
        ++mWakeCount;
    }
    return posted;
}

// MAIN thread
bool LLQueuedThread::waitForResult(LLQueuedThread::handle_t handle, bool auto_complete)
{
    LL_PROFILE_ZONE_SCOPED;
    llassert (handle != nullHandle());
    bool res = false;
    bool waspaused = isPaused();
    bool done = false;
    while(!done)
    {
        update(0); // unpauses
        lockData();
        QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
        if (!req)
        {
            done = true; // request does not exist
        }
        else if (req->getStatus() == STATUS_COMPLETE)
        {
            res = true;
            if (auto_complete)
            {
                mRequestHash.erase(handle);
                req->deleteRequest();
//              check();
            }
            done = true;
        }
        unlockData();

        if (!done && mThreaded)
        {
            yield();
        }
    }
    if (waspaused)
    {
        pause();
    }
    return res;
}

// MAIN thread
LLQueuedThread::QueuedRequest* LLQueuedThread::getRequest(handle_t handle)
{
    if (handle == nullHandle())
    {
        return 0;
    }
    lockData();
    QueuedRequest* res = (QueuedRequest*)mRequestHash.find(handle);
    unlockData();
    return res;
}

LLQueuedThread::status_t LLQueuedThread::getRequestStatus(handle_t handle)
{
    status_t res = STATUS_EXPIRED;
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    if (req)
    {
        res = req->getStatus();
    }
    unlockData();
    return res;
}

void LLQueuedThread::abortRequest(handle_t handle, bool autocomplete)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    bool repost = false;
    U32 gen = 0;
    if (req)
    {
        req->setFlags(FLAG_ABORT | (autocomplete ? FLAG_AUTO_COMPLETE : 0));
        if (req->mParked)
        {
            // #299: a parked request must run to see its abort
            gen = prepareRepostLocked(req);
            repost = true;
        }
    }
    unlockData();
    if (repost)
    {
        postNow(handle, gen);
    }
}

// MAIN thread
void LLQueuedThread::setFlags(handle_t handle, U32 flags)
{
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    if (req)
    {
        req->setFlags(flags);
    }
    unlockData();
}

bool LLQueuedThread::completeRequest(handle_t handle)
{
    LL_PROFILE_ZONE_SCOPED;
    bool res = false;
    lockData();
    QueuedRequest* req = (QueuedRequest*)mRequestHash.find(handle);
    if (req)
    {
        llassert_always(req->getStatus() != STATUS_QUEUED);
        llassert_always(req->getStatus() != STATUS_INPROGRESS);
#if _DEBUG
//      LL_INFOS() << llformat("LLQueuedThread::Completed req [%08d]",handle) << LL_ENDL;
#endif
        mRequestHash.erase(handle);
        req->deleteRequest();
//      check();
        res = true;
    }
    unlockData();
    return res;
}

bool LLQueuedThread::check()
{
#if 0 // not a reliable check once mNextHandle wraps, just for quick and dirty debugging
    for (int i=0; i<REQUEST_HASH_SIZE; i++)
    {
        LLSimpleHashEntry<handle_t>* entry = mRequestHash.get_element_at_index(i);
        while (entry)
        {
            if (entry->getHashKey() > mNextHandle)
            {
                LL_ERRS() << "Hash Error" << LL_ENDL;
                return false;
            }
            entry = entry->getNextEntry();
        }
    }
#endif
    return true;
}

//============================================================================
// Runs on its OWN thread

void LLQueuedThread::processRequest(LLQueuedThread::QueuedRequest* req)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;

    mIdleThread = false;
    //threadedUpdate();

    // Get next request from pool
    lockData();

    if ((req->getFlags() & FLAG_ABORT) || (mStatus == QUITTING))
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_THREAD("qtpr - abort");
        req->mParkExpired = false;  // aborted, not a missed wake
        req->setStatus(STATUS_ABORTED);
        req->finishRequest(false);
        if (req->getFlags() & FLAG_AUTO_COMPLETE)
        {
            mRequestHash.erase(req);
            req->deleteRequest();
//              check();
        }
        unlockData();
    }
    else
    {
        llassert_always(req->getStatus() == STATUS_QUEUED);

        if (req)
        {
            req->setStatus(STATUS_INPROGRESS);
        }
        unlockData();

        // This is the only place we will call req->setStatus() after
        // it has initially been seet to STATUS_QUEUED, so it is
        // safe to access req.
        if (req)
        {
            // process request
            bool complete = req->processRequest();

            if (complete)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_THREAD("qtpr - complete");
                lockData();
                // #299: a park-timeout re-run that completed: the event it was parked
                // for had happened and nobody woke it (unless a wake arrived meanwhile)
                const bool missed = req->mParkExpired && !req->mWakePending;
                const U32 missed_reason = req->mParkReason;
                const char* missed_name = missed ? req->getParkReasonName(missed_reason) : nullptr;
                req->mParkExpired = false;
                req->mParkRequested = false;
                req->mWakePending = false;
                req->setStatus(STATUS_COMPLETE);
                req->finishRequest(true);
                if (req->getFlags() & FLAG_AUTO_COMPLETE)
                {
                    mRequestHash.erase(req);
                    req->deleteRequest();
                    //              check();
                }
                unlockData();
                if (missed)
                {
                    noteMissedWake(missed_name, missed_reason, "completed");
                }
            }
            else
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_THREAD("qtpr - retry");
                // #299: back on the queue, due later -- the thread does NOT sleep for it
                // (it used to ms_sleep up to 16 ms here, stalling every other request,
                // including brand-new ones, behind each retry). Parked requests wait for
                // wakeRequest(), with PARK_TIMEOUT as the safety net.
                lockData();
                req->setStatus(STATUS_QUEUED);
                const bool woken = req->mWakePending;
                const bool aborting = (req->getFlags() & FLAG_ABORT) != 0;
                const bool park = req->mParkRequested && !woken && !aborting;
                // #299: was this run the PARK_TIMEOUT safety net's? Then sort it: re-parked
                // in the same wait = the event is just slow (recheck); progressed, or
                // parked for something else = its source missed the wake.
                bool missed = false;
                bool recheck = false;
                const U32 old_reason = req->mParkReason;
                if (req->mParkExpired && !woken && !aborting)
                {
                    if (park && req->mRequestedParkReason == old_reason)
                    {
                        recheck = true;
                    }
                    else
                    {
                        missed = true;
                    }
                }
                const char* missed_name = missed ? req->getParkReasonName(old_reason) : nullptr;
                const char* missed_outcome = !missed ? nullptr
                                           : park ? "parked for a different event"
                                                  : "returned to the retry poll";
                req->mParkExpired = false;
                if (park)
                {
                    req->mParkReason = req->mRequestedParkReason;
                }
                req->mParkRequested = false;
                req->mWakePending = false;
                req->mParked = park;
                const U32 gen = ++req->mRetryGen;
                const handle_t handle = req->getHashKey();
                const auto now = LL::WorkSchedule::TimePoint::clock::now();
                if (park)
                {
                    // no post: wakeRequest() posts it, or expireParked() after PARK_TIMEOUT
                    ++mParkedCount;
                    mParkedList.push_back(ParkEntry{ now + PARK_TIMEOUT, handle, gen });
                }
                unlockData();

                llassert(!mDataLock->isSelfLocked());

                if (recheck)
                {
                    ++mParkRecheckCount;
                }
                else if (missed)
                {
                    noteMissedWake(missed_name, old_reason, missed_outcome);
                }

                if (woken)
                {
                    postNow(handle, gen);
                }
                else if (!park)
                {
                    // Future-dated: only this thread posts those, all RETRY_DELAY out
                    // (see mParkedList's note on ThreadSafeSchedule).
                    mRequestQueue.post([this, handle, gen]() { retryRequest(handle, gen); },
                                       now + RETRY_DELAY);
                }


            }
        }
    }

    mIdleThread = true;
}

// virtual
bool LLQueuedThread::runCondition()
{
    // mRunCondition must be locked here
    if (mRequestQueue.size() == 0 && mIdleThread)
        return false;
    else
        return true;
}

// virtual
void LLQueuedThread::run()
{
    // call checPause() immediately so we don't try to do anything before the class is fully constructed
    checkPause();
    startThread();
    mStarted = true;


    /*while (1)
    {
        LL_PROFILE_ZONE_SCOPED;
        // this will block on the condition until runCondition() returns true, the thread is unpaused, or the thread leaves the RUNNING state.
        checkPause();

        mIdleThread = false;

        threadedUpdate();

        auto pending_work = processNextRequest();

        if (pending_work == 0)
        {
            //LL_PROFILE_ZONE_NAMED("LLQueuedThread - sleep");
            mIdleThread = true;
            //ms_sleep(1);
        }
        //LLThread::yield(); // thread should yield after each request
    }*/
    mRequestQueue.runUntilClose();

    endThread();
    LL_INFOS() << "LLQueuedThread " << mName << " EXITING." << LL_ENDL;


}

// virtual
void LLQueuedThread::startThread()
{
}

// virtual
void LLQueuedThread::endThread()
{
}

// virtual
void LLQueuedThread::threadedUpdate()
{
}

//============================================================================

LLQueuedThread::QueuedRequest::QueuedRequest(LLQueuedThread::handle_t handle, U32 flags) :
    LLSimpleHashEntry<LLQueuedThread::handle_t>(handle),
    mStatus(STATUS_UNKNOWN),
    mFlags(flags)
{
}

LLQueuedThread::QueuedRequest::~QueuedRequest()
{
    llassert_always(mStatus == STATUS_DELETE);
}

//virtual
void LLQueuedThread::QueuedRequest::finishRequest(bool completed)
{
}

//virtual
void LLQueuedThread::QueuedRequest::deleteRequest()
{
    llassert_always(mStatus != STATUS_INPROGRESS);
    setStatus(STATUS_DELETE);
    delete this;
}
