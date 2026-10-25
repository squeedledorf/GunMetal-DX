/**
 * @file llthreadaffinity.h
 * @brief Per-thread CPU placement on multi-L3 (multi-CCD) machines.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLTHREADAFFINITY_H
#define LL_LLTHREADAFFINITY_H

#include <string>

// On CPUs whose L3 is split into several domains (AMD CCDs), the main thread
// and the threads it trades data with every frame (the GPU driver's threads,
// created from the main thread) are kept on the FIRST L3 domain, and every
// worker thread (LL::ThreadPool pools, LLThread subclasses, the HTTP service)
// on the OTHER domains, so background decode/staging/fetch work never competes
// for the main thread's cores or evicts its cache. Replaces the launcher's old
// whole-process pin to one domain, which crammed ~60 threads onto 6 cores.
//
// No-op on non-Linux builds, on single-L3 machines, on small machines, when
// disabled by setting, or when SLIPSTREAM_NO_AFFINITY is set in the environment.
namespace LLThreadAffinity
{
    // Detect the layout, pin the CALLING thread (the main thread) to the
    // primary domain and enable worker placement. Call once, early, before
    // the worker pools and the graphics device are created.
    void init(bool enabled);

    // Put the calling thread on the worker domains (no-op when inactive).
    // Called at the start of every worker thread.
    void applyWorker();

    bool active();
    // Logical CPUs available to workers (0 when inactive).
    unsigned workerCpuCount();
    // Human-readable layout, for logging.
    std::string describe();
}

#endif // LL_LLTHREADAFFINITY_H
