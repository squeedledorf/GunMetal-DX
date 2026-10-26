/**
 * @file lltexturefetch.h
 * @brief Object for managing texture fetches.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2012-2013, Linden Research, Inc.
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

#ifndef LL_LLTEXTUREFETCH_H
#define LL_LLTEXTUREFETCH_H

#include <vector>
#include <atomic>
#include <set>
#include <map>

#include "lldir.h"
#include "llimage.h"
#include "lluuid.h"
#include "llworkerthread.h"
#include "lltextureinfo.h"
#include "llimageworker.h"
#include "httprequest.h"
#include "httpoptions.h"
#include "httpheaders.h"
#include "httphandler.h"
#include "lltrace.h"
#include "llviewertexture.h"

class LLViewerTexture;
class LLTextureFetchWorker;
class LLImageDecodeThread;
class LLHost;
class LLViewerAssetStats;
class LLTextureCache;
class LLTextureFetchTester;

// Interface class

class LLTextureFetch : public LLWorkerThread
{
    friend class LLTextureFetchWorker;

public:
    static std::string getStateString(S32 state);

    LLTextureFetch(LLTextureCache* cache, bool threaded, bool qa_mode);
    ~LLTextureFetch();

    class TFRequest;

    // Threads:  Tmain
    /*virtual*/ size_t update(F32 max_time_ms);

    // called in the main thread after the TextureCacheThread shuts down.
    // Threads:  Tmain
    void shutDownTextureCacheThread();

    //called in the main thread after the ImageDecodeThread shuts down.
    // Threads:  Tmain
    void shutDownImageDecodeThread();

    enum e_crete_request_errors
    {
        CREATE_REQUEST_ERROR_DEFAULT = -1,
        CREATE_REQUEST_ERROR_MHOSTS = -2,
        CREATE_REQUEST_ERROR_ABORTED = -3,
        CREATE_REQUEST_ERROR_TRANSITION = -4,
    };

    // Threads:  T* (but Tmain mostly)
    // returns discard on success, fail code otherwise
    S32 createRequest(FTType f_type, const std::string& url, const LLUUID& id, const LLHost& host, F32 priority,
                      S32 w, S32 h, S32 c, S32 discard, bool needs_aux, bool can_use_http);

    // Requests that a fetch operation be deleted from the queue.
    // If @cancel is true, also stops any I/O operations pending.
    // Actual delete will be scheduled and performed later.
    //
    // Note:  This *looks* like an override/variant of the
    // base class's deleteRequest() but is functionally quite
    // different.
    //
    // Threads:  T*
    void deleteRequest(const LLUUID& id, bool cancel);

    void deleteAllRequests();

    // #305: what getFetchState() reports, gathered by getRequestFinished() in the same
    // worker lookup when the request isn't finished (state == INVALID if there was none).
    struct FetchStateInfo
    {
        S32 mState = 0;
        F32 mDataProgress = 0.f;
        F32 mRequestedPriority = 0.f;
        U32 mFetchPriority = 0;
        F32 mFetchDeltaTime = 999999.f;
        F32 mRequestDeltaTime = 999999.f;
        bool mCanUseHTTP = true;
        bool mValid = false;    // filled (the request isn't finished and has work)
    };

    // Threads:  T*
    // keep in mind that if fetcher isn't done, it still might need original raw image
    // #305: pass state_out to also get getFetchState()'s answer from the same lookup.
    bool getRequestFinished(const LLUUID& id, S32& discard_level, S32& worker_state,
                            LLPointer<LLImageRaw>& raw, LLPointer<LLImageRaw>& aux,
                            LLCore::HttpStatus& last_http_get_status,
                            FetchStateInfo* state_out = nullptr);

    // Threads:  Tmain only (#305: stores the worker's priority directly -- workers are
    // only deleted on the main thread, so the looked-up pointer stays valid). It used
    // to post a lambda to the fetch thread for every fetching texture every frame.
    bool updateRequestPriority(const LLUUID& id, F32 priority);

    // Threads:  T* (but not safe)
    void setTextureBandwidth(F32 bandwidth) { mTextureBandwidth = bandwidth; }

    // Threads:  T* (but not safe)
    F32 getTextureBandwidth() { return mTextureBandwidth; }

    // Threads:  T*
    bool isFromLocalCache(const LLUUID& id);

    // #305: getFetchState()'s answer for a worker already in hand
    void fillFetchState(LLTextureFetchWorker* worker, FetchStateInfo* out);

    // get the current fetch state, if any, from the given UUID
    S32 getFetchState(const LLUUID& id);

    // @return  Fetch state of an active given image and associates statistics
    //          See also getStateString
    // Threads:  T*
    S32 getFetchState(const LLUUID& id, F32& decode_progress_p, F32& requested_priority_p,
                      U32& fetch_priority_p, F32& fetch_dtime_p, F32& request_dtime_p, bool& can_use_http);

    // @return  Fetch last state of given image
    // Threads:  T*
    S32 getLastFetchState(const LLUUID& id, S32& requested_discard, S32 &decoded_discard, bool &decoded);

    // @return  Fetch last raw image
    // Threads:  T*
    S32 getLastRawImage(const LLUUID& id, LLPointer<LLImageRaw>& raw, LLPointer<LLImageRaw>& aux);

    // Debug utility - generally not safe
    void dump();

    // Threads:  T*
    S32 getNumRequests();

    // Threads:  T*
    S32 getNumHTTPRequests();

    // Threads:  T*
    U32 getTotalNumHTTPRequests();

    // Threads:  T*
    size_t getPending();

    // Threads:  T*
    void lockQueue() { mQueueMutex.lock(); }

    // Threads:  T*
    void unlockQueue() { mQueueMutex.unlock(); }

    // Threads:  T*
    LLTextureFetchWorker* getWorker(const LLUUID& id);

    // Threads:  T*
    // Locks:  Mfq
    LLTextureFetchWorker* getWorkerAfterLock(const LLUUID& id);

    // Commands available to other threads to control metrics gathering operations.

    // Threads:  T*
    void commandSetRegion(U64 region_handle);

    // Threads:  T*
    void commandSendMetrics(const std::string & caps_url,
                            const LLUUID & session_id,
                            const LLUUID & agent_id,
                            LLSD& stats_sd);

    // Threads:  T*
    void commandDataBreak();

    // Threads:  T*
    LLCore::HttpRequest & getHttpRequest()  { return *mHttpRequest; }

    // Threads:  T*
    LLCore::HttpRequest::policy_t getPolicyClass() const { return mHttpPolicyClass; }

    // Return a pointer to the shared metrics headers definition.
    // Does not increment the reference count, caller is required
    // to do that to hold a reference for any length of time.
    //
    // Threads:  T*
    LLCore::HttpHeaders::ptr_t getMetricsHeaders() const    { return mHttpMetricsHeaders; }

    // Threads:  T*
    LLCore::HttpRequest::policy_t getMetricsPolicyClass() const { return mHttpMetricsPolicyClass; }

    bool isQAMode() const               { return mQAMode; }

    // ----------------------------------
    // HTTP resource waiting methods

    // Threads:  T*
    void addHttpWaiter(const LLUUID & tid);

    // Threads:  T*
    void removeHttpWaiter(const LLUUID & tid);

    // Threads:  T*
    bool isHttpWaiter(const LLUUID & tid);

    // If there are slots, release one or more LLTextureFetchWorker
    // requests from resource wait state (WAIT_HTTP_RESOURCE) to
    // active (SEND_HTTP_REQ).
    //
    // Because this will modify state of many workers, you may not
    // hold any Mw lock while calling.  This makes it a little
    // inconvenient to use but that's the rule.
    //
    // Threads:  T*
    // Locks:  -Mw (must not hold any worker when called)
    void releaseHttpWaiters();

    // Threads:  T*
    void cancelHttpWaiters();

    // Threads:  T*
    int getHttpWaitersCount();

    // ----------------------------------
    // #485 fetch gate: decode-admission waiters (parked on the memory governor, woken
    // by priority when it frees bytes -- they used to re-try every 16 ms)

    // Threads:  T*
    void addGovWaiter(const LLUUID& tid);
    void removeGovWaiter(const LLUUID& tid);
    bool isGovWaiter(const LLUUID& tid);

    // Threads:  Ttf. Locks: -Mw
    void releaseGovWaiters();

    // #485 stats, any thread (relaxed): see LLTextureFetch::FetchGateStats
    struct FetchGateStats
    {
        S32 mInFlight = 0;          // CDN texture requests handed to HTTP (sent or about to be)
        S32 mInFlightAux = 0;       // bakes / map tiles / other-URL textures
        S32 mInFlightLimit = 0;
        S32 mQueued = 0;            // waiting to be sent (no data held)
        S32 mGovWaiting = 0;        // downloaded, parked on decode admission
        U64 mSent = 0;              // lifetime: requests handed to HTTP
        U64 mDroppedUnsent = 0;     // lifetime: dropped from the queue before sending
        U64 mDroppedCap = 0;        //   ...of which by the SLFetchMaxQueued cap
        F32 mQueueAgeP50 = 0.f;     // seconds waiting, current queue
        F32 mQueueAgeP95 = 0.f;
    };
    void getFetchGateStats(FetchGateStats& out);

    // #951: why a LOAD_FROM_NETWORK worker did not move on (set where doWork returns
    // without leaving the state; cleared by any state change).
    enum ENetWhy
    {
        NET_WHY_NONE = 0,           // not evaluated yet / moving on
        NET_WHY_RETRY_WAIT,         // the retry policy's back-off after a server failure
        NET_WHY_NO_REGION,          // no region for the worker's host (or no agent region)
        NET_WHY_NO_URL,             // the region has no ViewerAsset capability (yet)
        NET_WHY_CAP_RETRY,          // waiting out CAP_MISSING_EXPIRATION_DELAY before asking again
        NET_WHY_NO_HTTP,            // ImagePipelineUseHTTP off / HTTP not allowed for this request
        NET_WHY_COUNT
    };
    // #951: worker kinds for the per-stage ages (WorkerStats::mOldest / mStuck)
    enum EStage { STAGE_CACHE = 0, STAGE_NET, STAGE_HTTP, STAGE_DECODE, STAGE_WRITE, STAGE_UNCOLLECTED, STAGE_COUNT };

    // #485 RAM: every fetch worker by state and the bytes each kind holds, sampled by
    // the worker sweep (sweepWorkers, main thread, ~1 Hz), plus what the sweep evicted.
    // A worker outlives its fetch until its texture deletes it; a finished one keeps
    // its decoded raw (and, until collected, the encoded data) -- 64k of them were the
    // invisible part of RSS. Main thread only.
    struct WorkerStats
    {
        S32 mTotal = 0, mPeak = 0;
        S32 mCache = 0;             // reading the disk cache
        S32 mNetWait = 0;           // waiting to be sent (holds no data)
        S32 mInFlight = 0;          // HTTP request out
        S32 mDecode = 0;            // decode admission / decoding
        S32 mWrite = 0;             // writing the disk cache
        S32 mUncollected = 0;       // finished, the texture hasn't taken the result yet
        S32 mIdle = 0;              // no work: result taken (or dropped unsent); waiting for deletion
        S32 mBusy = 0;              // locked by the fetch thread at sample time (not counted above)
        U64 mFormattedBytes = 0;    // encoded data held by workers
        U64 mRawBytes = 0;          // decoded raw + aux held by workers (shared with textures or not)
        U64 mRawOnlyBytes = 0;      //   ...of which only the worker still references (pure waste)
        U64 mHttpBytes = 0;         // HTTP reply buffers held
        U64 mIdleRawBytes = 0;      // raw held by idle + uncollected workers (what eviction frees)
        U64 mEvictedIdle = 0;       // lifetime: idle past SLFetchWorkerIdleSec
        U64 mEvictedUncollected = 0;// lifetime: finished but not collected for SLFetchWorkerUncollectedSec
        U64 mEvictedCap = 0;        // lifetime: over SLFetchMaxWorkers / SLFetchWorkerHeldMB
        F32 mSweepMs = 0.f;
        // #951 texture pipeline diagnostics (logPipelineState): mNetWait split by state, why
        // LOAD_FROM_NETWORK workers are not sending, decode waits by cause, and how long the
        // oldest worker of each kind has been in its state.
        S32 mLoadNet = 0;           // LOAD_FROM_NETWORK (looking for a URL / capability)
        S32 mWaitRes = 0;           // WAIT_HTTP_RESOURCE + WAIT_HTTP_RESOURCE2 (send queue)
        S32 mNetWhy[NET_WHY_COUNT] = {};   // LOAD_FROM_NETWORK workers by ENetWhy
        S32 mDecodeGov = 0;         // DECODE_IMAGE parked on the memory budget
        S32 mDecodeOOM = 0;         // DECODE_IMAGE in the #698 out-of-memory backoff
        S32 mDecodeRun = 0;         // DECODE_IMAGE_UPDATE (queued on / running in the decode pool)
        F32 mOldest[STAGE_COUNT] = {};     // seconds the oldest worker of each EStage has been in its state
        S32 mStuck[STAGE_COUNT] = {};      // workers of each EStage in their state > 30 s
        F32 mOldestLoadNet = 0.f;   // seconds the oldest LOAD_FROM_NETWORK worker has been looking for a URL
        LLUUID mOldestId;           // the oldest stuck worker (a texture id, no PII)
        S32 mOldestState = -1;      //   ...its state
    };
    static const char* netWhyName(S32 why);
    static const char* stageName(S32 stage);
    // #951: total worker state changes (any worker), so a stall (no change at all) can be
    // told from a slow queue. Threads: T*
    void noteStateChange() { mStateChanges.fetch_add(1, std::memory_order_relaxed); }
    // Threads: Tmain
    const WorkerStats& getWorkerStats() const { return mWorkerStats; }
    // ----------------------------------
    // Stats management

    // Add given counts to the global totals for the states/requests
    // Threads:  T*
    void updateStateStats(U32 cache_read, U32 cache_write, U32 res_wait);

    // Return the global counts
    // Threads:  T*
    void getStateStats(U32 * cache_read, U32 * cache_write, U32 * res_wait);

    // ----------------------------------

protected:
    // Threads:  T*
    void addToHTTPQueue(const LLUUID& id);

    // XXX possible delete
    // Threads:  T*
    void removeFromHTTPQueue(const LLUUID& id, S32Bytes received_size);

    // Identical to @deleteRequest but with different arguments
    // (caller already has the worker pointer).
    //
    // Threads:  T*
    void removeRequest(LLTextureFetchWorker* worker, bool cancel);

    // Overrides from the LLThread tree
    // Locks:  Ct
    bool runCondition();

private:
    // Threads:  Ttf
    /*virtual*/ void startThread(void);

    // Threads:  Ttf
    /*virtual*/ void endThread(void);

    // Threads:  Ttf
    /*virtual*/ void threadedUpdate(void);
    /*virtual*/ bool hasThreadedUpdate() const { return true; }

    // Threads:  Ttf
    void commonUpdate();

    // #485 Threads: Ttf. Deliver HTTP completions and refill freed slots at once (run
    // from the reply notifier, not only once per frame).
    void pumpHttp();

    // #485 Threads: Ttf. Drop unsent requests nobody wants any more (priority 0) and
    // the lowest ones past SLFetchMaxQueued; sample the queue ages. Locks: -Mw
    void pruneHttpWaiters();

    // #485: in-flight limits for the CDN and the aux (non-CDN) texture classes
    void updateHttpLimits();

    // Metrics command helpers
    /**
     * Enqueues a command request at the end of the command queue
     * and wakes up the thread as needed.
     *
     * Takes ownership of the TFRequest object.
     *
     * Method locks the command queue.
     *
     * Threads:  T*
     */
    void cmdEnqueue(TFRequest *);

    /**
     * Returns the first TFRequest object in the command queue or
     * NULL if none is present.
     *
     * Caller acquires ownership of the object and must dispose of it.
     *
     * Method locks the command queue.
     *
     * Threads:  T*
     */
    TFRequest * cmdDequeue();

    /**
     * Processes the first command in the queue disposing of the
     * request on completion.  Successive calls are needed to perform
     * additional commands.
     *
     * Method locks the command queue.
     *
     * Threads:  Ttf
     */
    void cmdDoWork();

public:
    LLUUID mDebugID;
    S32 mDebugCount;
    bool mDebugPause;

    static LLTrace::CountStatHandle<F64>        sCacheHit;
    static LLTrace::CountStatHandle<F64>        sCacheAttempt;
    static LLTrace::SampleStatHandle<F32Seconds> sCacheReadLatency;
    static LLTrace::SampleStatHandle<F32Seconds> sTexDecodeLatency;
    static LLTrace::SampleStatHandle<F32Seconds> sCacheWriteLatency;
    static LLTrace::SampleStatHandle<F32Seconds> sTexFetchLatency;
    static LLTrace::EventStatHandle<LLUnit<F32, LLUnits::Percent> > sCacheHitRate;

private:
    LLMutex mQueueMutex;        //to protect mRequestMap and mCommands only
    LLMutex mNetworkQueueMutex; //to protect mHTTPTextureQueue

    LLTextureCache* mTextureCache;

    // Map of all requests by UUID
    typedef boost::unordered_map<LLUUID,LLTextureFetchWorker*> map_t;
    map_t mRequestMap;                                                  // Mfq

    // Set of requests that require network data
    typedef std::set<LLUUID> queue_t;
    queue_t mHTTPTextureQueue;                                          // Mfnq
    typedef std::map<LLHost,std::set<LLUUID> > cancel_queue_t;
    F32 mTextureBandwidth;                                              // <none>
    F32 mMaxBandwidth;                                                  // Mfnq
    LLTextureInfo mTextureInfo;
    LLTextureInfo mTextureInfoMainThread;

    // XXX possible delete
    U32Bits mHTTPTextureBits;                                               // Mfnq

    // XXX possible delete
    //debug use
    U32 mTotalHTTPRequests;

    // Out-of-band cross-thread command queue.  This command queue
    // is logically tied to LLQueuedThread's list of
    // QueuedRequest instances and so must be covered by the
    // same locks.
    typedef std::vector<TFRequest *> command_queue_t;
    command_queue_t mCommands;                                          // Mfq

    // If true, modifies some behaviors that help with QA tasks.
    const bool mQAMode;

    // Interfaces and objects into the core http library used
    // to make our HTTP requests.  These replace the various
    // LLCurl interfaces used in the past.
    LLCore::HttpRequest *               mHttpRequest;                   // Ttf
    LLCore::HttpOptions::ptr_t          mHttpOptions;                   // Ttf
    LLCore::HttpOptions::ptr_t          mHttpOptionsWithHeaders;        // Ttf
    LLCore::HttpHeaders::ptr_t          mHttpHeaders;                   // Ttf
    LLCore::HttpRequest::policy_t       mHttpPolicyClass;               // T*
    LLCore::HttpHeaders::ptr_t          mHttpMetricsHeaders;            // Ttf
    LLCore::HttpRequest::policy_t       mHttpMetricsPolicyClass;        // T*
    S32                                 mHttpHighWater;                 // Ttf
    S32                                 mHttpLowWater;                  // Ttf

    // We use a resource semaphore to keep HTTP requests in
    // WAIT_HTTP_RESOURCE2 if there aren't sufficient slots in the
    // transport.  This keeps them near where they can be cheaply
    // reprioritized rather than dumping them all across a thread
    // where it's more expensive to get at them.  Requests in either
    // SEND_HTTP_REQ or WAIT_HTTP_REQ charge against the semaphore
    // and tracking state transitions is critical to liveness.
    //
    // Originally implemented as a traditional semaphore (heading towards
    // zero), it now is an outstanding request count that is allowed to
    // exceed the high water level (but not go below zero).
    LLAtomicS32                         mHttpSemaphore;                 // Ttf
    // #485: the same for the aux class (bakes, map tiles, other URLs: other hosts)
    LLCore::HttpRequest::policy_t       mHttpAuxPolicyClass;            // T*
    LLAtomicS32                         mHttpAuxSemaphore;              // Ttf
    S32                                 mHttpAuxHighWater;              // Ttf

    typedef std::set<LLUUID> wait_http_res_queue_t;
    wait_http_res_queue_t               mHttpWaitResource;              // Mfnq
    wait_http_res_queue_t               mGovWaiters;                    // Mfnq (#485)

    // #485 RAM: worker sweep (Tmain)
    void sweepWorkers();
    WorkerStats                         mWorkerStats;                   // Tmain
    F64                                 mNextSweepTime = 0.0;           // Tmain
    // #951 texture pipeline diagnostics (Tmain, from sweepWorkers)
    void logPipelineState(F64 now);
    std::atomic<U64>                    mStateChanges{ 0 };
    U64                                 mDiagLastChanges = 0;           // Tmain
    F64                                 mDiagLastChangeTime = 0.0;      // Tmain: when mStateChanges last moved
    F64                                 mDiagNextLog = 0.0;             // Tmain
    F64                                 mDiagNextWarn = 0.0;            // Tmain
    bool                                mDiagEverActive = false;        // Tmain

    // #485 fetch gate
    std::atomic<bool>                   mPumpPosted{ false };
    F64                                 mNextPruneTime = 0.0;           // Ttf
    U64                                 mLastGovReleases = 0;           // Ttf
    F64                                 mLastGovWake = 0.0;             // Ttf
    std::atomic<U64>                    mSentCount{ 0 };
    std::atomic<U64>                    mDroppedUnsent{ 0 };
    std::atomic<U64>                    mDroppedCap{ 0 };
    std::atomic<S32>                    mQueuedCount{ 0 };
    std::atomic<S32>                    mGovWaitCount{ 0 };
    std::atomic<F32>                    mQueueAgeP50{ 0.f };
    std::atomic<F32>                    mQueueAgeP95{ 0.f };

    // Cumulative stats on the states/requests issued by
    // textures running through here.
    U32 mTotalCacheReadCount;                                           // Mfq
    U32 mTotalCacheWriteCount;                                          // Mfq
    U32 mTotalResourceWaitCount;                                        // Mfq

public:
    // A probabilistically-correct indicator that the current
    // attempt to log metrics follows a break in the metrics stream
    // reporting due to either startup or a problem POSTing data.
    static volatile bool svMetricsDataBreak;

public:
    //debug use
    enum e_tex_source
    {
        FROM_ALL = 0,
        FROM_HTTP_ONLY,
        INVALID_SOURCE
    };

    static LLTextureFetchTester* sTesterp;

private:
    e_tex_source mFetchSource;
    e_tex_source mOriginFetchSource;

    // Retry logic
    //LLAdaptiveRetryPolicy mFetchRetryPolicy;

public:
    void setLoadSource(e_tex_source source) {mFetchSource = source;}
    void resetLoadSource() {mFetchSource = mOriginFetchSource;}
    bool canLoadFromCache() { return mFetchSource != FROM_HTTP_ONLY;}
};

//debug use
class LLViewerFetchedTexture;

class LLTextureFetchTester : public LLMetricPerformanceTesterBasic
{
public:
    LLTextureFetchTester();
    ~LLTextureFetchTester();

    void updateStats(const std::map<S32, F32> states_timers, const F32 fetch_time, const F32 other_states_time, const S32 file_size);

protected:
    /*virtual*/ void outputTestRecord(LLSD* sd);

private:

    F32 mTextureFetchTime;
    F32 mSkippedStatesTime;
    S32 mFileSize;

    std::map<S32, F32> mStateTimersMap;
};
#endif // LL_LLTEXTUREFETCH_H

