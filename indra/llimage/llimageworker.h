/**
 * @file llimageworker.h
 * @brief Object for managing images and their textures.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

#ifndef LL_LLIMAGEWORKER_H
#define LL_LLIMAGEWORKER_H

#include "llimage.h"
#include "llpointer.h"
#include "threadpool_fwd.h"

#include <atomic>
#include <memory>

class LLImageDecodeThread
{
public:
    class Responder : public LLThreadSafeRefCount
    {
    protected:
        virtual ~Responder();
    public:
        virtual void completed(bool success, const std::string& error_message, LLImageRaw* raw, LLImageRaw* aux, U32 request_id) = 0;
    };

public:
    LLImageDecodeThread(bool threaded = true);
    virtual ~LLImageDecodeThread();

    // meant to resemble LLQueuedThread::handle_t
    typedef U32 handle_t;
    // #698: Responder::completed() error_message of a decode that ran out of memory (the
    // input is fine: keep it cached, retry later).
    static constexpr const char* OUT_OF_MEMORY_ERROR = "#698 out of memory";
    // #307: decodes start highest 'priority' first (ties: oldest first), not FIFO.
    handle_t decodeImage(const LLPointer<LLImageFormatted>& image,
                         S32 discard, bool needs_aux,
                         const LLPointer<Responder>& responder,
                         F32 priority = 0.f);
    // #307: drop a decode that hasn't started. Its responder is released WITHOUT being
    // called (so whatever it holds -- e.g. memory-governor bytes -- is freed now).
    // Returns false if it already started (or finished): its result will arrive.
    bool cancel(handle_t handle);
    size_t getPending();    // decodes waiting to start
    U64 getCancelledCount() const { return mCancelledCount; }
    size_t update(F32 max_time_ms);
    S32 getTotalDecodeCount() { return mDecodeCount; }
    void shutdown();

private:
    // As of SL-17483, LLImageDecodeThread is no longer itself an
    // LLQueuedThread - instead this is the API by which we submit work to the
    // "ImageDecode" ThreadPool.
    std::unique_ptr<LL::ThreadPool> mThreadPool;
    LLAtomicU32 mDecodeCount;

    // #307: the decodes waiting to start, by priority, behind their own mutex. Every
    // decodeImage() posts one runOne() to the pool; runOne() starts the best waiting
    // decode (a cancelled one leaves a spare runOne(), which finds nothing). Bounded by
    // the callers' requests in flight (one per texture fetch worker).
    struct PendingQueue;
    std::unique_ptr<PendingQueue> mPending;
    std::atomic<U64> mCancelledCount{ 0 };
    void runOne();
};

#endif
