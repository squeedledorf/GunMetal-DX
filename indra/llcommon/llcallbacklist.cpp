/**
 * @file llcallbacklist.cpp
 * @brief A simple list of callback functions to call.
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

#include "llcallbacklist.h"
#include "lleventtimer.h"
#include "llerrorlegacy.h"

// Globals
//
LLCallbackList gIdleCallbacks;

//
// Member functions
//

LLCallbackList::LLCallbackList()
{
    // nothing
}

LLCallbackList::~LLCallbackList()
{
}


void LLCallbackList::addFunction( callback_t func, void *data)
{
    if (!func)
    {
        return;
    }

    // only add one callback per func/data pair
    //
    if (containsFunction(func, data))
    {
        return;
    }

    callback_pair_t t(func, data);
    mCallbackList.push_back(t);
}

bool LLCallbackList::containsFunction( callback_t func, void *data)
{
    callback_pair_t t(func, data);
    callback_list_t::iterator iter = find(func,data);
    if (iter != mCallbackList.end())
    {
        return true;
    }
    else
    {
        return false;
    }
}


bool LLCallbackList::deleteFunction( callback_t func, void *data)
{
    callback_list_t::iterator iter = find(func,data);
    if (iter != mCallbackList.end())
    {
        if (mIterating)
        {
            iter->first = nullptr;
            mNeedsCompact = true;
        }
        else
        {
            mCallbackList.erase(iter);
        }
        return true;
    }
    else
    {
        return false;
    }
}

inline
LLCallbackList::callback_list_t::iterator
LLCallbackList::find(callback_t func, void *data)
{
    callback_pair_t t(func, data);
    return std::find(mCallbackList.begin(), mCallbackList.end(), t);
}

void LLCallbackList::deleteAllFunctions()
{
    if (mIterating)
    {
        for (callback_pair_t& entry : mCallbackList)
        {
            entry.first = nullptr;
        }
        mNeedsCompact = true;
        return;
    }
    mCallbackList.clear();
}


void LLCallbackList::callFunctions()
{
    struct IterationScope
    {
        LLCallbackList& mList;
        bool mWasIterating;
        IterationScope(LLCallbackList& list) : mList(list), mWasIterating(list.mIterating) { mList.mIterating = true; }
        ~IterationScope()
        {
            mList.mIterating = mWasIterating;
            if (!mList.mIterating && mList.mNeedsCompact)
            {
                callback_list_t& cbs = mList.mCallbackList;
                cbs.erase(std::remove_if(cbs.begin(), cbs.end(),
                                         [](const callback_pair_t& entry) { return entry.first == nullptr; }),
                          cbs.end());
                mList.mNeedsCompact = false;
            }
        }
    } scope(*this);

    // Entries appended during the walk are still called this pass, as before. Index,
    // not iterator: an append may reallocate. Copy the entry before calling it for the
    // same reason.
    for (size_t i = 0; i < mCallbackList.size(); ++i)
    {
        const callback_pair_t entry = mCallbackList[i];
        if (entry.first)
        {
            entry.first(entry.second);
        }
    }
}

// Shim class to allow arbitrary boost::bind
// expressions to be run as one-time idle callbacks.
class OnIdleCallbackOneTime
{
public:
    OnIdleCallbackOneTime(nullary_func_t callable):
        mCallable(callable)
    {
    }
    static void onIdle(void *data)
    {
        gIdleCallbacks.deleteFunction(onIdle, data);
        OnIdleCallbackOneTime* self = reinterpret_cast<OnIdleCallbackOneTime*>(data);
        self->call();
        delete self;
    }
    void call()
    {
        mCallable();
    }
private:
    nullary_func_t mCallable;
};

void doOnIdleOneTime(nullary_func_t callable)
{
    OnIdleCallbackOneTime* cb_functor = new OnIdleCallbackOneTime(callable);
    gIdleCallbacks.addFunction(&OnIdleCallbackOneTime::onIdle,cb_functor);
}

// Shim class to allow generic boost functions to be run as
// recurring idle callbacks.  Callable should return true when done,
// false to continue getting called.
class OnIdleCallbackRepeating
{
public:
    OnIdleCallbackRepeating(bool_func_t callable):
        mCallable(callable)
    {
    }
    // Will keep getting called until the callable returns true.
    static void onIdle(void *data)
    {
        OnIdleCallbackRepeating* self = reinterpret_cast<OnIdleCallbackRepeating*>(data);
        bool done = self->call();
        if (done)
        {
            gIdleCallbacks.deleteFunction(onIdle, data);
            delete self;
        }
    }
    bool call()
    {
        return mCallable();
    }
private:
    bool_func_t mCallable;
};

void doOnIdleRepeating(bool_func_t callable)
{
    OnIdleCallbackRepeating* cb_functor = new OnIdleCallbackRepeating(callable);
    gIdleCallbacks.addFunction(&OnIdleCallbackRepeating::onIdle,cb_functor);
}

class NullaryFuncEventTimer: public LLEventTimer
{
public:
    NullaryFuncEventTimer(nullary_func_t callable, F32 seconds):
        LLEventTimer(seconds),
        mCallable(callable)
    {
    }

private:
    bool tick()
    {
        mCallable();
        return true;
    }

    nullary_func_t mCallable;
};

// Call a given callable once after specified interval.
void doAfterInterval(nullary_func_t callable, F32 seconds)
{
    new NullaryFuncEventTimer(callable, seconds);
}

class BoolFuncEventTimer: public LLEventTimer
{
public:
    BoolFuncEventTimer(bool_func_t callable, F32 seconds):
        LLEventTimer(seconds),
        mCallable(callable)
    {
    }
private:
    bool tick()
    {
        return mCallable();
    }

    bool_func_t mCallable;
};

// Call a given callable every specified number of seconds, until it returns true.
void doPeriodically(bool_func_t callable, F32 seconds)
{
    new BoolFuncEventTimer(callable, seconds);
}
