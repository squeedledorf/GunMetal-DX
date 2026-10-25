/**
 * @file lleventtimer.cpp
 * @brief Cross-platform objects for doing timing
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

#include "linden_common.h"

#include "lleventtimer.h"

#include "u64.h"
#include "llprofiler.h"
#include <mutex>
#include <vector>


//////////////////////////////////////////////////////////////////////////////
//
//      LLEventTimer Implementation
//
//////////////////////////////////////////////////////////////////////////////

namespace
{
// Flat registry of live timers. updateClass() walks it by index without
// allocating; removals during a walk leave a tombstone that is compacted
// once the outermost walk finishes.
struct TimerRegistry
{
    std::mutex mMutex;
    std::vector<LLEventTimer*> mTimers;
    S32 mWalkDepth = 0;
    bool mHasTombstones = false;
};

TimerRegistry& registry()
{
    static TimerRegistry sRegistry;
    return sRegistry;
}
}

LLEventTimer::LLEventTimer(F32 period)
: mEventTimer()
{
    mPeriod = period;
    registerTimer();
}

LLEventTimer::LLEventTimer(const LLDate& time)
: mEventTimer()
{
    mPeriod = (F32)(time.secondsSinceEpoch() - LLDate::now().secondsSinceEpoch());
    registerTimer();
}


LLEventTimer::~LLEventTimer()
{
    TimerRegistry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mMutex);
    if (reg.mWalkDepth > 0)
    {
        reg.mTimers[mRegistryIndex] = nullptr;
        reg.mHasTombstones = true;
    }
    else
    {
        LLEventTimer* last = reg.mTimers.back();
        reg.mTimers[mRegistryIndex] = last;
        last->mRegistryIndex = mRegistryIndex;
        reg.mTimers.pop_back();
    }
}

void LLEventTimer::registerTimer()
{
    TimerRegistry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mMutex);
    mRegistryIndex = reg.mTimers.size();
    reg.mTimers.push_back(this);
}

//static
void LLEventTimer::updateClass()
{
    LL_PROFILE_ZONE_SCOPED;
    TimerRegistry& reg = registry();
    size_t count;
    {
        std::lock_guard<std::mutex> lock(reg.mMutex);
        ++reg.mWalkDepth;
        count = reg.mTimers.size();
    }
    // timers added during the walk land past 'count' and run next frame
    for (size_t i = 0; i < count; ++i)
    {
        LLEventTimer* timer;
        {
            std::lock_guard<std::mutex> lock(reg.mMutex);
            timer = reg.mTimers[i];
        }
        if (!timer || !timer->mEventTimer.getStarted())
        {
            continue;
        }
        if (timer->mEventTimer.getElapsedTimeF32() > timer->mPeriod)
        {
            timer->mEventTimer.reset();
            if (timer->tick())
            {
                delete timer;
            }
        }
    }
    std::lock_guard<std::mutex> lock(reg.mMutex);
    if (--reg.mWalkDepth == 0 && reg.mHasTombstones)
    {
        auto& v = reg.mTimers;
        size_t out = 0;
        for (size_t in = 0; in < v.size(); ++in)
        {
            if (v[in])
            {
                v[out] = v[in];
                v[out]->mRegistryIndex = out;
                ++out;
            }
        }
        v.resize(out);
        reg.mHasTombstones = false;
    }
}
