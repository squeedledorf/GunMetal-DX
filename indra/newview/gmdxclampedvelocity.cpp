/**
 * @file gmdxclampedvelocity.cpp
 * @brief Recovers object velocities that terse updates clamp to +-128 m/s per axis.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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
 * $/LicenseInfo$
 */

// <GMDX> See gmdxclampedvelocity.h. From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: the recovery below is
// skRecoverClampedVelocity; only where its state lives changed.

#include "llviewerprecompiledheaders.h"

#include "gmdxclampedvelocity.h"

#include "llframetimer.h"
#include "llviewerobject.h"

#include <unordered_map>

namespace
{
    constexpr F32 VEL_CLAMP = 127.99f;
    constexpr F32 ACC_CLAMP = 63.99f;
    // Objects slower than this on every axis are not tracked (GMDX: the table holds fast objects only).
    // Half the clamp, so an object speeding up past it already has a last position when it gets there.
    constexpr F32 TRACK_SPEED = 64.f;

    struct Track
    {
        LLVector3 mPos;             // the last terse update's position
        F64 mSecs = 0.0;            // and when it came
        const void* mRegion = nullptr;  // compared only, never dereferenced
        F32 mSpeedEWMA = 0.f;       // distance over time of recent clamped updates
    };
    std::unordered_map<LLUUID, Track> sTracks;
    F64 sNextPrune = 0.0;

    void prune(F64 now)
    {
        if (now < sNextPrune)
        {
            return;
        }
        sNextPrune = now + 2.0;
        for (auto it = sTracks.begin(); it != sTracks.end();)
        {
            it = (now - it->second.mSecs > 1.0) ? sTracks.erase(it) : std::next(it);
        }
    }
}

// Terse updates clamp each velocity axis to +-128 m/s; the fastest unclamped axis is exact, and
// divided by its share of the direction of travel it restores the speed.
void GMDXClampedVelocity::onTerseUpdate(LLViewerObject* objectp, const LLVector3& new_pos)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    prune(now);

    // A clamped acceleration points the wrong way, and the finite difference was no better than none.
    const LLVector3& acc = objectp->getAcceleration();
    if (fabsf(acc.mV[VX]) >= ACC_CLAMP || fabsf(acc.mV[VY]) >= ACC_CLAMP || fabsf(acc.mV[VZ]) >= ACC_CLAMP)
    {
        objectp->setAcceleration(LLVector3::zero);
    }

    const LLVector3 vel = objectp->getVelocity();
    if (fabsf(vel.mV[VX]) < TRACK_SPEED && fabsf(vel.mV[VY]) < TRACK_SPEED && fabsf(vel.mV[VZ]) < TRACK_SPEED)
    {
        sTracks.erase(objectp->getID());
        return;
    }

    const bool in_world = objectp->getRegion() && !objectp->getParent();
    Track& track = sTracks[objectp->getID()];
    const bool have_prev = in_world && track.mSecs > 0.0 && track.mRegion == objectp->getRegion() && now - track.mSecs <= 1.0;
    if (!have_prev)
    {
        track.mSpeedEWMA = 0.f;
    }
    const LLVector3 prev_pos = track.mPos;
    const F64 prev_secs = track.mSecs;
    track.mPos = new_pos;
    track.mSecs = now;
    track.mRegion = in_world ? objectp->getRegion() : nullptr;

    bool clamped[3];
    bool any_clamped = false;
    for (S32 i = 0; i < 3; ++i)
    {
        clamped[i] = fabsf(vel.mV[i]) >= VEL_CLAMP;
        any_clamped |= clamped[i];
    }
    if (!any_clamped || !have_prev)
    {
        return;
    }

    const LLVector3 delta = new_pos - prev_pos;
    const F32 dist = delta.length();
    if (dist < 0.5f)
    {
        return;
    }
    const LLVector3 dir = delta / dist;

    // A bounce or a teleport moves against the reported velocity; the ratio would be meaningless.
    if (dir * vel <= 0.f)
    {
        return;
    }
    for (S32 i = 0; i < 3; ++i)
    {
        if (clamped[i] && (dir.mV[i] > 0.f) != (vel.mV[i] > 0.f))
        {
            return;
        }
    }

    const F64 gap = now - prev_secs;
    if (gap >= 0.01 && gap <= 1.0)
    {
        const F32 rate = dist / (F32)gap;
        track.mSpeedEWMA = track.mSpeedEWMA > 0.f ? track.mSpeedEWMA + 0.3f * (rate - track.mSpeedEWMA) : rate;
    }

    const F32 reported = vel.length();
    S32 axis = -1;
    for (S32 i = 0; i < 3; ++i)
    {
        if (!clamped[i] && fabsf(vel.mV[i]) >= 0.5f && fabsf(dir.mV[i]) > 1e-6f && (dir.mV[i] > 0.f) == (vel.mV[i] > 0.f)
            && (axis < 0 || fabsf(vel.mV[i]) > fabsf(vel.mV[axis])))
        {
            axis = i;
        }
    }

    F32 speed = 0.f;
    if (axis >= 0)
    {
        const F32 ratio = fabsf(vel.mV[axis]) / fabsf(dir.mV[axis]);
        if (ratio >= reported - 1e-3f && ratio <= 2000.f)
        {
            speed = ratio;
        }
    }
    if (speed <= 0.f && track.mSpeedEWMA > 0.f)
    {
        speed = llmin(llmax(track.mSpeedEWMA, reported), 2000.f);   // (GMDX: capped as the axis ratio is)
    }
    if (speed <= 0.f)
    {
        return;
    }

    objectp->setVelocity(dir * speed);
}

void GMDXClampedVelocity::clear()
{
    sTracks.clear();
}
