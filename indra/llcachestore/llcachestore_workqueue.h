/**
 * @file llcachestore_workqueue.h
 * @brief Completion helper: deliver store ticket completions on an LL::WorkQueue.
 *
 * Kept out of llcachestore.h so the store's public header does not pull in
 * the WorkQueue machinery.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLCACHESTORE_WORKQUEUE_H
#define LL_LLCACHESTORE_WORKQUEUE_H

#include "llcachestore.h"
#include "workqueue.h"

namespace LLCache
{
    // A Poster that runs the completion on `queue`'s thread(s). If the queue
    // is gone or closed the completion is dropped (the ticket still holds the
    // result for anyone polling it).
    inline Poster postTo(LL::WorkQueueBase::weak_t queue)
    {
        return [queue](std::function<void()> fn) -> bool
        {
            return LL::WorkQueueBase::postMaybe(queue, std::move(fn));
        };
    }

    // Convenience: completion = notify on queue.
    inline Completion onQueue(LL::WorkQueueBase::weak_t queue, std::function<void()> notify,
                              std::function<void()> wake = {})
    {
        Completion c;
        c.post = postTo(queue);
        c.notify = std::move(notify);
        c.wake = std::move(wake);
        return c;
    }
}

#endif // LL_LLCACHESTORE_WORKQUEUE_H
