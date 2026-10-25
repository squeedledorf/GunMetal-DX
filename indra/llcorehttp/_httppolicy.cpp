/**
 * @file _httppolicy.cpp
 * @brief Internal definitions of the Http policy thread
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2012-2014, Linden Research, Inc.
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

#include "_httppolicy.h"

#include "_httpoprequest.h"
#include "_httpservice.h"
#include "_httplibcurl.h"
#include "_httppolicyclass.h"

#include "lltimer.h"

#include <atomic>
#include "httpstats.h"

namespace
{

static const char * const LOG_CORE("CoreHttp");

// #485 busy back-off (PO_BUSY_BACKOFF_MAX_MS): lifetime count of back-offs and the
// time the current pause ends, per class (relaxed atomics, read by the viewer's stats).
std::atomic<U64> sClassBackoffs[LLCore::HTTP_POLICY_CLASS_LIMIT];
std::atomic<U64> sClassPausedUntil[LLCore::HTTP_POLICY_CLASS_LIMIT];

// The server (or the path to it) says "slow down": 429 Too Many Requests, 503 Service
// Unavailable, 502/504 from a gateway, or a connect / transfer timeout.
bool is_busy_status(const LLCore::HttpStatus& status)
{
    static const LLCore::HttpStatus too_many(429);
    static const LLCore::HttpStatus bad_gateway(502);
    static const LLCore::HttpStatus unavailable(503);
    static const LLCore::HttpStatus gateway_timeout(504);
    static const LLCore::HttpStatus timed_out(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_OPERATION_TIMEDOUT);
    static const LLCore::HttpStatus cant_connect(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_COULDNT_CONNECT);
    return status == too_many || status == bad_gateway || status == unavailable || status == gateway_timeout
        || status == timed_out || status == cant_connect;
}

} // end anonymous namespace


namespace LLCore
{


// Per-policy-class data for a running system.
// Collection of queues, options and other data
// for a single policy class.
//
// Threading:  accessed only by worker thread
struct HttpPolicy::ClassState
{
public:
    ClassState()
        : mTokenTime(0),
          mTokens(0.0),
          mRequestCount(0L),
          mBusyUntil(0),
          mBusyDelay(0)
        {}

    HttpReadyQueue      mReadyQueue;
    HttpRetryQueue      mRetryQueue;

    HttpPolicyClass     mOptions;
    // #485: PO_THROTTLE_RATE is a token bucket (smooth pacing of request starts,
    // at most ~100 ms of starts in one burst) -- it used to let a whole second's
    // worth start at once and then stall until the second ended.
    HttpTime            mTokenTime;
    F64                 mTokens;
    long                mRequestCount;
    // #485: PO_BUSY_BACKOFF_MAX_MS: no new request starts before mBusyUntil;
    // mBusyDelay doubles per busy reply and resets on a good one.
    HttpTime            mBusyUntil;
    HttpTime            mBusyDelay;
};


HttpPolicy::HttpPolicy(HttpService * service)
    : mService(service)
{
    // Create default class
    mClasses.push_back(new ClassState());
}


HttpPolicy::~HttpPolicy()
{
    shutdown();

    for (class_list_t::iterator it(mClasses.begin()); it != mClasses.end(); ++it)
    {
        delete (*it);
    }
    mClasses.clear();

    mService = NULL;
}


HttpRequest::policy_t HttpPolicy::createPolicyClass()
{
    const HttpRequest::policy_t policy_class(static_cast<HttpRequest::policy_t>(mClasses.size()));
    if (policy_class >= HTTP_POLICY_CLASS_LIMIT)
    {
        return HttpRequest::INVALID_POLICY_ID;
    }
    mClasses.push_back(new ClassState());
    return policy_class;
}


void HttpPolicy::shutdown()
{
    for (int policy_class(0); policy_class < mClasses.size(); ++policy_class)
    {
        ClassState & state(*mClasses[policy_class]);

        HttpRetryQueue & retryq(state.mRetryQueue);
        while (! retryq.empty())
        {
            HttpOpRequest::ptr_t op(retryq.top());
            retryq.pop();

            op->cancel();
        }

        HttpReadyQueue & readyq(state.mReadyQueue);
        while (! readyq.empty())
        {
            HttpOpRequest::ptr_t op(readyq.top());
            readyq.pop();

            op->cancel();
        }
    }
}


void HttpPolicy::start()
{
}


void HttpPolicy::addOp(const HttpOpRequest::ptr_t &op)
{
    const int policy_class(op->mReqPolicy);

    op->mPolicyRetries = 0;
    op->mPolicy503Retries = 0;
    mClasses[policy_class]->mReadyQueue.push(op);
}


void HttpPolicy::retryOp(const HttpOpRequest::ptr_t &op)
{
    static const HttpStatus error_503(503);

    const HttpTime now(totalTime());
    const int policy_class(op->mReqPolicy);

    HttpTime delta_min = op->mPolicyMinRetryBackoff;
    HttpTime delta_max = op->mPolicyMaxRetryBackoff;
    // mPolicyRetries limited to 100
    U32 delta_factor = op->mPolicyRetries <= 10 ? 1 << op->mPolicyRetries : 1024;
    HttpTime delta = llmin(delta_min * delta_factor, delta_max);
    bool external_delta(false);

    if (op->mReplyRetryAfter > 0 && op->mReplyRetryAfter < 30)
    {
        delta = op->mReplyRetryAfter * U64L(1000000);
        external_delta = true;
    }
    op->mPolicyRetryAt = now + delta;
    ++op->mPolicyRetries;
    if (error_503 == op->mStatus)
    {
        ++op->mPolicy503Retries;
    }
    LL_DEBUGS(LOG_CORE) << "HTTP request " << op->getHandle()
                        << " retry " << op->mPolicyRetries
                        << " scheduled in " << (delta / HttpTime(1000))
                        << " mS (" << (external_delta ? "external" : "internal")
                        << ").  Status:  " << op->mStatus.toTerseString()
                        << LL_ENDL;
    if (op->mTracing > HTTP_TRACE_OFF)
    {
        LL_INFOS(LOG_CORE) << "TRACE, ToRetryQueue, Handle:  "
                            << op->getHandle()
                            << ", Delta:  " << (delta / HttpTime(1000))
                            << ", Retries:  " << op->mPolicyRetries
                            << LL_ENDL;
    }
    HttpLibcurl::countRetry(policy_class);
    mClasses[policy_class]->mRetryQueue.push(op);
}


// Attempt to deliver requests to the transport layer.
//
// Tries to find HTTP requests for each policy class with
// available capacity.  Starts with the retry queue first
// looking for requests that have waited long enough then
// moves on to the ready queue.
//
// If all queues are empty, will return an indication that
// the worker thread may sleep hard otherwise will ask for
// normal polling frequency.
//
// Implements a client-side request rate throttle as well.
// This is intended to mimic and predict throttling behavior
// of grid services but that is difficult to do with different
// time bases.  This also represents a rigid coupling between
// viewer and server that makes it hard to change parameters
//
HttpService::ELoopSpeed HttpPolicy::processReadyQueue()
{
    const HttpTime now(totalTime());
    HttpService::ELoopSpeed result(HttpService::REQUEST_SLEEP);
    HttpLibcurl & transport(mService->getTransport());

    for (int policy_class(0); policy_class < mClasses.size(); ++policy_class)
    {
        ClassState & state(*mClasses[policy_class]);
        HttpRetryQueue & retryq(state.mRetryQueue);
        HttpReadyQueue & readyq(state.mReadyQueue);

        if (retryq.empty() && readyq.empty())
        {
            continue;
        }

        // #485: busy back-off -- the server said slow down (429 / 503 / timeouts):
        // start nothing new in this class (retries included) until the pause ends.
        if (state.mOptions.mBusyBackoffMaxMs > 0 && now < state.mBusyUntil)
        {
            result = HttpService::NORMAL;
            continue;
        }

        // #485: smooth start pacing (PO_THROTTLE_RATE starts per second, token
        // bucket holding at most ~100 ms of starts) so a burst of new work drains
        // at a steady rate instead of hitting the server all at once.
        const long rate(state.mOptions.mThrottleRate);
        if (rate > 0L)
        {
            const F64 burst((std::max)(1.0, F64(rate) / 10.0));
            if (state.mTokenTime == 0 || now < state.mTokenTime)
            {
                state.mTokens = burst;
            }
            else
            {
                state.mTokens = (std::min)(burst, state.mTokens + F64(now - state.mTokenTime) * F64(rate) / 1000000.0);
            }
            state.mTokenTime = now;
            if (state.mTokens < 1.0)
            {
                // Out of starts for now: don't serve this class but don't sleep hard.
                result = HttpService::NORMAL;
                continue;
            }
        }

        int active(transport.getActiveCountInClass(policy_class));
        // One request per connection (no HTTP/1.1 pipelining since curl 7.65,
        // and HTTP/2 multiplexing is not used): the class's connection limit
        // is also its in-flight request limit.
        int active_limit(state.mOptions.mConnectionLimit);
        int needed(active_limit - active);      // Expect negatives here
        if (rate > 0L)
        {
            needed = (std::min)(needed, int(state.mTokens));
        }

        if (needed > 0)
        {
            // First see if we have any retries...
            while (needed > 0 && ! retryq.empty())
            {
                HttpOpRequest::ptr_t op(retryq.top());
                if (op->mPolicyRetryAt > now)
                    break;

                retryq.pop();

                op->stageFromReady(mService);
                op.reset();

                ++state.mRequestCount;
                --needed;
                if (rate > 0L)
                {
                    state.mTokens -= 1.0;
                }
            }

            // Now go on to the new requests...
            while (needed > 0 && ! readyq.empty())
            {
                HttpOpRequest::ptr_t op(readyq.top());
                readyq.pop();

                op->stageFromReady(mService);
                op.reset();

                ++state.mRequestCount;
                --needed;
                if (rate > 0L)
                {
                    state.mTokens -= 1.0;
                }
            }
        }

        if (! readyq.empty() || ! retryq.empty())
        {
            // If anything is ready, continue looping...
            result = HttpService::NORMAL;
        }
    } // end foreach policy_class

    return result;
}

bool HttpPolicy::cancel(HttpHandle handle)
{
    for (int policy_class(0); policy_class < mClasses.size(); ++policy_class)
    {
        ClassState & state(*mClasses[policy_class]);

        // Scan retry queue
        HttpRetryQueue::container_type & c1(state.mRetryQueue.get_container());
        for (HttpRetryQueue::container_type::iterator iter(c1.begin()); c1.end() != iter;)
        {
            HttpRetryQueue::container_type::iterator cur(iter++);

            if ((*cur)->getHandle() == handle)
            {
                HttpOpRequest::ptr_t op(*cur);
                c1.erase(cur);                                  // All iterators are now invalidated
                op->cancel();
                return true;
            }
        }

        // Scan ready queue
        HttpReadyQueue::container_type & c2(state.mReadyQueue.get_container());
        for (HttpReadyQueue::container_type::iterator iter(c2.begin()); c2.end() != iter;)
        {
            HttpReadyQueue::container_type::iterator cur(iter++);

            if ((*cur)->getHandle() == handle)
            {
                HttpOpRequest::ptr_t op(*cur);
                c2.erase(cur);                                  // All iterators are now invalidated
                op->cancel();
                return true;
            }
        }
    }

    return false;
}


bool HttpPolicy::stageAfterCompletion(const HttpOpRequest::ptr_t &op)
{
    // #485: busy back-off for opted-in classes (the asset fetch classes). A "slow
    // down" reply pauses ALL new starts in the class -- not just this request's
    // retry -- for the server's Retry-After (0.5-30 s) or an exponential delay
    // (0.5 s doubling up to the class maximum); a good reply resets the delay.
    bool busy_retryable(false);
    const int pclass(op->mReqPolicy);
    if (pclass >= 0 && pclass < int(mClasses.size()) && mClasses[pclass]->mOptions.mBusyBackoffMaxMs > 0)
    {
        ClassState & state(*mClasses[pclass]);
        if (! op->mStatus && is_busy_status(op->mStatus))
        {
            const HttpTime now(totalTime());
            const HttpTime max_delay(HttpTime(state.mOptions.mBusyBackoffMaxMs) * 1000U);
            HttpTime delay;
            if (op->mReplyRetryAfter > 0)
            {
                delay = llclamp(HttpTime(op->mReplyRetryAfter) * 1000000U, HttpTime(500000U), HttpTime(30000000U));
            }
            else
            {
                state.mBusyDelay = state.mBusyDelay ? (std::min)(state.mBusyDelay * 2U, max_delay) : (std::min)(HttpTime(500000U), max_delay);
                delay = state.mBusyDelay;
            }
            if (now + delay > state.mBusyUntil)
            {
                state.mBusyUntil = now + delay;
                sClassPausedUntil[pclass].store(state.mBusyUntil, std::memory_order_relaxed);
            }
            sClassBackoffs[pclass].fetch_add(1, std::memory_order_relaxed);
            LL_DEBUGS(LOG_CORE) << "Policy class " << pclass << " backing off " << (delay / 1000U)
                                << " ms after " << op->mStatus.toTerseString() << LL_ENDL;
            // 429 isn't in HttpStatus::isRetryable() (it is for these classes)
            busy_retryable = true;
        }
        else if (op->mStatus)
        {
            state.mBusyDelay = 0;
        }
    }

    // Retry or finalize
    if (! op->mStatus)
    {
        // *DEBUG:  For "[curl:bugs] #1420" tests.  This will interfere
        // with unit tests due to allocation retention by logging code.
        // But you won't be checking this in enabled.
#if 0
        if (op->mStatus == HttpStatus(HttpStatus::EXT_CURL_EASY, CURLE_OPERATION_TIMEDOUT))
        {
            LL_WARNS(LOG_CORE) << "HTTP request " << op->getHandle()
                               << " timed out."
                               << LL_ENDL;
        }
#endif

        // If this failed, we might want to retry.
        if (op->mPolicyRetries < op->mPolicyRetryLimit && (op->mStatus.isRetryable() || busy_retryable))
        {
            // Okay, worth a retry.
            retryOp(op);
            return true;                // still active/ready
        }
    }

    // This op is done, finalize it delivering it to the reply queue...
    if (! op->mStatus)
    {
        LL_WARNS(LOG_CORE) << "HTTP request " << op->getHandle()
                           << " failed after " << op->mPolicyRetries
                           << " retries.  Reason:  " << op->mStatus.toString()
                           << " (" << op->mStatus.toTerseString() << ")"
                           << LL_ENDL;
    }
    else if (op->mPolicyRetries)
    {
        LL_DEBUGS(LOG_CORE) << "HTTP request " << op->getHandle()
                            << " succeeded on retry " << op->mPolicyRetries << "."
                            << LL_ENDL;
    }

    op->stageFromActive(mService);

    HTTPStats::instance().recordResultCode(op->mStatus.getType());
    return false;                       // not active
}


// static (#485)
void HttpRequest::getBackoffStats(policy_t policy_id, U64& backoffs, bool& paused_now)
{
    backoffs = 0;
    paused_now = false;
    if (policy_id < HttpRequest::policy_t(HTTP_POLICY_CLASS_LIMIT))
    {
        backoffs = sClassBackoffs[policy_id].load(std::memory_order_relaxed);
        paused_now = sClassPausedUntil[policy_id].load(std::memory_order_relaxed) > totalTime();
    }
}


HttpPolicyClass & HttpPolicy::getClassOptions(HttpRequest::policy_t pclass)
{
    llassert_always(pclass >= 0 && pclass < mClasses.size());

    return mClasses[pclass]->mOptions;
}


int HttpPolicy::getReadyCount(HttpRequest::policy_t policy_class) const
{
    if (policy_class < mClasses.size())
    {
        return static_cast<int>((mClasses[policy_class]->mReadyQueue.size()
            + mClasses[policy_class]->mRetryQueue.size()));
    }
    return 0;
}


}  // end namespace LLCore
