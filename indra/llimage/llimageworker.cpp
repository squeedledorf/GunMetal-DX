/**
 * @file llimageworker.cpp
 * @brief Base class for images.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llimageworker.h"
#include "llimagedxt.h"
#include "threadpool.h"
#include "llmemorygovernor.h"
#include "llcachemetrics.h"

#include <chrono>
#include <map>
#include <mutex>
#include <unordered_map>

/*--------------------------------------------------------------------------*/
class ImageRequest
{
public:
    ImageRequest(const LLPointer<LLImageFormatted>& image,
                 S32 discard,
                 bool needs_aux,
                 const LLPointer<LLImageDecodeThread::Responder>& responder,
                 U32 request_id);
    virtual ~ImageRequest();

    /*virtual*/ bool processRequest();
    /*virtual*/ void finishRequest(bool completed);
    // #698: the decode ran out of memory -- drop the partial output, report
    // LLImageDecodeThread::OUT_OF_MEMORY_ERROR (the owner retries; the input is fine).
    void failOutOfMemory();
    bool isOutOfMemory() const { return mOutOfMemory; }
    U64 outputBytesEstimate() const;

private:
    // LLPointers stored in ImageRequest MUST be LLPointer instances rather
    // than references: we need to increment the refcount when storing these.
    // input
    LLPointer<LLImageFormatted> mFormattedImage;
    S32 mDiscardLevel;
    U32 mRequestId;
    bool mNeedsAux;
    // output
    LLPointer<LLImageRaw> mDecodedImageRaw;
    LLPointer<LLImageRaw> mDecodedImageAux;
    bool mDecodedRaw;
    bool mDecodedAux;
    LLPointer<LLImageDecodeThread::Responder> mResponder;
    std::string mErrorString;
    bool mOutOfMemory = false;
    // #715 metrics (J2C only): enqueue time and the decode's own time (usec, steady clock).
    U64 mRequestUsec = 0;
    U64 mWorkUsec = 0;
};

namespace
{
    U64 steady_usec()
    {
        return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}


//----------------------------------------------------------------------------

// #307: decodes waiting to start, best first
struct LLImageDecodeThread::PendingQueue
{
    struct Key
    {
        F32 mPriority;
        U32 mHandle;    // increasing: ties go to the older request
        bool operator<(const Key& rhs) const
        {
            if (mPriority != rhs.mPriority)
            {
                return mPriority > rhs.mPriority;
            }
            return mHandle < rhs.mHandle;
        }
    };
    std::mutex mMutex;
    std::map<Key, std::unique_ptr<ImageRequest> > mQueue;    // under mMutex
    std::unordered_map<U32, Key> mByHandle;                  // under mMutex
};

// MAIN THREAD
LLImageDecodeThread::LLImageDecodeThread(bool /*threaded*/)
    : mDecodeCount(0),
      mPending(new PendingQueue)
{
    mThreadPool.reset(new LL::ThreadPool("ImageDecode", 8));
    mThreadPool->start();
}

//virtual
LLImageDecodeThread::~LLImageDecodeThread()
{
    // #307: join the pool before mPending (declared after it) goes away under runOne()
    shutdown();
}

// MAIN THREAD
// virtual
size_t LLImageDecodeThread::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return getPending();
}

size_t LLImageDecodeThread::getPending()
{
    std::lock_guard<std::mutex> lock(mPending->mMutex);
    return mPending->mQueue.size();
}

// #307: any thread
bool LLImageDecodeThread::cancel(handle_t handle)
{
    std::unique_ptr<ImageRequest> dropped;
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        auto it = mPending->mByHandle.find(handle);
        if (it == mPending->mByHandle.end())
        {
            return false;   // started, finished, or unknown
        }
        auto qit = mPending->mQueue.find(it->second);
        if (qit != mPending->mQueue.end())
        {
            dropped = std::move(qit->second);
            mPending->mQueue.erase(qit);
        }
        mPending->mByHandle.erase(it);
    }
    // destroyed outside the lock: releasing the responder may do real work
    dropped.reset();
    ++mCancelledCount;
    return true;
}

// #307: pool thread -- start the best waiting decode
void LLImageDecodeThread::runOne()
{
    std::unique_ptr<ImageRequest> req;
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        auto qit = mPending->mQueue.begin();
        if (qit == mPending->mQueue.end())
        {
            return; // its request was cancelled
        }
        req = std::move(qit->second);
        mPending->mByHandle.erase(qit->first.mHandle);
        mPending->mQueue.erase(qit);
    }
    // #698: a decode that runs out of memory is reported to its owner as such (it retries
    // later and keeps the cached image), never forwarded to the main loop as fatal and
    // never a completion that silently never comes.
    bool done = true;
    try
    {
        LLMemoryGovernor::maybeInjectAllocFail();   // #698 test switch (SLMemInjectAllocFail)
        done = req->processRequest();
    }
    catch (const std::bad_alloc&)
    {
        req->failOutOfMemory();
    }
    if (req->isOutOfMemory())
    {
        LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_TEX_DECODE, req->outputBytesEstimate(), "image decode");
        done = true;
    }
    req->finishRequest(done);
}

LLImageDecodeThread::handle_t LLImageDecodeThread::decodeImage(
    const LLPointer<LLImageFormatted>& image,
    S32 discard,
    bool needs_aux,
    const LLPointer<LLImageDecodeThread::Responder>& responder,
    F32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 decode_id = ++mDecodeCount;
    if (decode_id == 0)
        decode_id = ++mDecodeCount;

    // #307: queue by priority, then post a "start the best one" to the pool (the pool's
    // own queue is FIFO and can't cancel). Queued before posting, so the runOne() this
    // posts always finds something unless a cancel() got there first.
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        const PendingQueue::Key key{ priority, decode_id };
        mPending->mQueue.emplace(key, std::make_unique<ImageRequest>(image, discard, needs_aux, responder, decode_id));
        mPending->mByHandle.emplace(decode_id, key);
    }
    bool posted = mThreadPool->getQueue().post([this]() { runOne(); });
    if (! posted)
    {
        LL_DEBUGS() << "Tried to start decoding on shutdown" << LL_ENDL;
        cancel(decode_id);  // nothing will run it
        return 0;
    }

    return decode_id;
}

void LLImageDecodeThread::shutdown()
{
    // #307: don't decode what nobody will use: drop what hasn't started (the closing
    // pool would otherwise run every queued decode before its threads exit)
    std::map<PendingQueue::Key, std::unique_ptr<ImageRequest> > dropped;
    {
        std::lock_guard<std::mutex> lock(mPending->mMutex);
        dropped.swap(mPending->mQueue);
        mPending->mByHandle.clear();
    }
    dropped.clear();
    if (mThreadPool)
    {
        mThreadPool->close();
    }
}

LLImageDecodeThread::Responder::~Responder()
{
}

//----------------------------------------------------------------------------

ImageRequest::ImageRequest(const LLPointer<LLImageFormatted>& image,
                           S32 discard,
                           bool needs_aux,
                           const LLPointer<LLImageDecodeThread::Responder>& responder,
                           U32 request_id)
    : mFormattedImage(image),
      mDiscardLevel(discard),
      mNeedsAux(needs_aux),
      mDecodedRaw(false),
      mDecodedAux(false),
      mResponder(responder),
      mRequestId(request_id),
      mRequestUsec(steady_usec())
{
}

ImageRequest::~ImageRequest()
{
    mDecodedImageRaw = NULL;
    mDecodedImageAux = NULL;
    mFormattedImage = NULL;
}

//----------------------------------------------------------------------------


// Returns true when done, whether or not decode was successful.
bool ImageRequest::processRequest()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mFormattedImage.isNull())
        return true;

    const F32 decode_time_slice = 0.f; //disable time slicing
    bool done = true;

    LLImageDataLock lockFormatted(mFormattedImage);
    LLImageDataLock lockDecodedRaw(mDecodedImageRaw);
    LLImageDataLock lockDecodedAux(mDecodedImageAux);
    // #715: the decode's own time (header parse + decode + aux), not the queue wait
    struct WorkTimer
    {
        U64& acc; const U64 t0 = steady_usec();
        ~WorkTimer() { const U64 t1 = steady_usec(); acc += t1 > t0 ? t1 - t0 : 0; }
    } work_timer{ mWorkUsec };

    if (!mDecodedRaw)
    {
        // Decode primary channels
        if (mDecodedImageRaw.isNull())
        {
            // parse formatted header
            if (!mFormattedImage->updateData())
            {
                return true; // done (failed)
            }
            if ((mFormattedImage->getWidth() * mFormattedImage->getHeight() * mFormattedImage->getComponents()) == 0)
            {
                return true; // done (failed)
            }
            if (mDiscardLevel >= 0)
            {
                mFormattedImage->setDiscardLevel(mDiscardLevel);
            }
            S32 width = mFormattedImage->getWidth();
            S32 height = mFormattedImage->getHeight();
            // RAM #262: a J2C decodes at the requested discard (OpenJPEG cp_reduce) and the
            // decoder resizes the output to that size -- don't allocate the full-size
            // buffer first (a 4096^2 RGBA texture decoded at discard 3 used to reserve
            // 64 MB for a 2 MB result).
            if (mFormattedImage->getCodec() == IMG_CODEC_J2C && mDiscardLevel > 0)
            {
                const S32 d = llmin(mDiscardLevel, 15);
                width = llmax(1, (width + (1 << d) - 1) >> d);
                height = llmax(1, (height + (1 << d) - 1) >> d);
            }
            mDecodedImageRaw = new LLImageRaw(width, height, mFormattedImage->getComponents());
            if (!mDecodedImageRaw->getData())
            {
                // #698: LLImageRaw's allocation fails soft (no data). This used to reach the
                // owner as "decode failed" -- and the texture fetcher deleted the (good)
                // cached image. Report it as out of memory instead.
                failOutOfMemory();
                return true;
            }
        }
        done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        // some decoders are removing data when task is complete and there were errors
        mDecodedRaw = done && mDecodedImageRaw->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }
    if (done && mNeedsAux && !mDecodedAux && mFormattedImage.notNull())
    {
        // Decode aux channel
        if (!mDecodedImageAux)
        {
            S32 width = mFormattedImage->getWidth();
            S32 height = mFormattedImage->getHeight();
            if (mFormattedImage->getCodec() == IMG_CODEC_J2C && mDiscardLevel > 0)
            {
                const S32 d = llmin(mDiscardLevel, 15);   // as above (RAM #262)
                width = llmax(1, (width + (1 << d) - 1) >> d);
                height = llmax(1, (height + (1 << d) - 1) >> d);
            }
            mDecodedImageAux = new LLImageRaw(width, height, 1);
            if (!mDecodedImageAux->getData())
            {
                failOutOfMemory();   // #698: as the primary channels above
                return true;
            }
        }
        done = mFormattedImage->decodeChannels(mDecodedImageAux, decode_time_slice, 4, 4);
        mDecodedAux = done && mDecodedImageAux->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }

    return done;
}

void ImageRequest::failOutOfMemory()
{
    mDecodedImageRaw = NULL;
    mDecodedImageAux = NULL;
    mDecodedRaw = false;
    mDecodedAux = false;
    mOutOfMemory = true;
    mErrorString = LLImageDecodeThread::OUT_OF_MEMORY_ERROR;
}

U64 ImageRequest::outputBytesEstimate() const
{
    if (mFormattedImage.isNull()) return 0;
    return (U64)llmax(mFormattedImage->getWidth(), 0) * (U64)llmax(mFormattedImage->getHeight(), 0)
         * (U64)llmax((S32)mFormattedImage->getComponents(), 0);
}

void ImageRequest::finishRequest(bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // #715 cache metrics: the J2C path's decode stage (request -> pixels, as the GPU-ready
    // cache's read latency is request -> decoded chain; plus the decode's own time).
    if (mFormattedImage.notNull() && mFormattedImage->getCodec() == IMG_CODEC_J2C)
    {
        const bool ok = completed && mDecodedRaw && (!mNeedsAux || mDecodedAux);
        const U64 now = steady_usec();
        U64 out = 0;
        if (ok && mDecodedImageRaw.notNull()) out += (U64)llmax(mDecodedImageRaw->getDataSize(), 0);
        if (ok && mDecodedImageAux.notNull()) out += (U64)llmax(mDecodedImageAux->getDataSize(), 0);
        LLCacheMetrics::recordStage(LLCacheMetrics::Stage::J2C_DECODE, ok, (U64)llmax(mFormattedImage->getDataSize(), 0), out,
                                    now > mRequestUsec ? now - mRequestUsec : 0, mWorkUsec);
    }
    if (mResponder.notNull())
    {
        bool success = completed && mDecodedRaw && (!mNeedsAux || mDecodedAux);
        mResponder->completed(success, mErrorString, mDecodedImageRaw, mDecodedImageAux, mRequestId);
    }
    // Will automatically be deleted
}
