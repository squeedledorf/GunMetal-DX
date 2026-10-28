/**
 * @file fsghostprojectile.cpp
 * @brief Ghost projectile watch: probes projectiles the simulator stopped updating.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#include "llviewerprecompiledheaders.h"

#include "fsghostprojectile.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "message.h"

#include <vector>

// <SS:Nexii> Projectile ghost probing

static const F32        PROJECTILE_MIN_LENGTH_M = 4.0f;
static const F32        PROJECTILE_MAX_THICKNESS_M = 0.2f;
static const F64Seconds PROBE_INTERVAL(3.0);
static const U32        MAX_WATCH_PROBES = 3;

static std::vector<LLPointer<LLViewerObject> > sWatchList;
static U32 sNumLikely = 0;
static U32 sNumDropped = 0;

static void track_likely(bool was_likely, bool is_likely)
{
    if (was_likely == is_likely) return;

    if (is_likely) ++sNumLikely;
    else if (sNumLikely > 0) --sNumLikely;
}

// static
void FSGhostProjectile::onVelocityUpdate(LLViewerObject* objectp)
{
    FSGhostProjectileState& s = objectp->mGhostProjectile;
    if (s.mTagged) return;
    s.mTagged = true;

    const bool was_likely = s.mLikely;

    if (objectp->isAvatar() || objectp->isAttachment())
    {
        s.mLikely = false;
        s.mGhosted = false;
        track_likely(was_likely, false);
        return;
    }

    const LLVector3 scale = objectp->getScale();
    const F32 x = llmax(0.0f, scale.mV[VX]);
    const F32 y = llmax(0.0f, scale.mV[VY]);
    const F32 z = llmax(0.0f, scale.mV[VZ]);

    const F32 longest = llmax(x, llmax(y, z));
    const F32 shortest = llmin(x, llmin(y, z));
    const F32 middle = x + y + z - longest - shortest;

    s.mLikely = (longest >= PROJECTILE_MIN_LENGTH_M) &&
                (middle <= PROJECTILE_MAX_THICKNESS_M) &&
                (shortest <= PROJECTILE_MAX_THICKNESS_M);
    if (!s.mLikely)
    {
        s.mGhosted = false;
    }
    track_likely(was_likely, s.mLikely);
}

// A likely projectile that WAS moving but is now static may have crossed a region boundary
// and become a ghost. Going static stops interpolateLinearMotion() from ever running for it
// again (and, for LLVOVolume, takes it off the active list), so register it for ghost probing
// before that happens.
// static
void FSGhostProjectile::onGoingStatic(LLViewerObject* objectp)
{
    // <GMDX> and mSeenMoving: mStatic starts out false, so SoapStorm also watched (and probed
    // three times) every long thin prim whose very first update was static.
    if (objectp->mGhostProjectile.mLikely && !objectp->mStatic && objectp->mGhostProjectile.mSeenMoving)
    {
        LL_DEBUGS("Projectile") << "Object " << objectp->getID() << " was a likely projectile bullet, but is now static. It may have crossed a region boundary and become a ghost." << LL_ENDL;
        beginWatch(objectp);
    }
}

// static
void FSGhostProjectile::onSimUpdate(LLViewerObject* objectp)
{
    objectp->mGhostProjectile.mGhosted = false;
    objectp->mGhostProjectile.mProbeCount = 0;
    if (!objectp->mStatic)
    {
        objectp->mGhostProjectile.mSeenMoving = true; // <GMDX>
    }
}

// Static projectiles are skipped by interpolateLinearMotion(), so a ghost one has to be
// probed from here. Objects whose isActive() drops with mStatic are off the active list and
// never reach this; the watch list covers them.
// static
void FSGhostProjectile::onIdleStatic(LLViewerObject* objectp)
{
    const FSGhostProjectileState& s = objectp->mGhostProjectile;
    // <GMDX> Only objects seen moving: a static thin prim that never moved is no projectile.
    if (s.mLikely && s.mSeenMoving && !s.mOnWatch && s.mProbeCount == 0 && !objectp->isSelected())
    {
        beginWatch(objectp);
    }
}

// Bullets legitimately go a long time between updates: the simulator sends one terse
// update and the viewer interpolates the rest of the flight. Anything inside
// sMaxUpdateInterpolationTime is normal traffic, so the ghost threshold starts where the
// viewer's own interpolation gives up and adds a tunable grace period on top.
// static
F64Seconds FSGhostProjectile::getProbeThreshold()
{
    static LLCachedControl<F32> probe_delay(gSavedSettings, "FSSoapstormGhostProjectileProbeDelay", 2.f);
    return LLViewerObject::sMaxUpdateInterpolationTime + F64Seconds(llmax(0.f, (F32)probe_delay));
}

// static
void FSGhostProjectile::onInterpolate(LLViewerObject* objectp, F64Seconds frame_time, F64Seconds time_since_last_update)
{
    FSGhostProjectileState& s = objectp->mGhostProjectile;
    if (!s.mLikely || !objectp->mRegionp || objectp->mDead || objectp->mLocalID == 0) return;

    if (time_since_last_update < getProbeThreshold()) return;

    if (frame_time - s.mLastProbe < PROBE_INTERVAL) return;

    LL_DEBUGS("Projectile") << "Probing ghost projectile " << objectp->getID() << " ("
                            << time_since_last_update.value() << "s since last update)" << LL_ENDL;

    s.mLastProbe = frame_time;
    sendProbe(objectp);
}

// static
void FSGhostProjectile::beginWatch(LLViewerObject* objectp)
{
    FSGhostProjectileState& s = objectp->mGhostProjectile;
    if (s.mOnWatch || objectp->mDead || !objectp->mRegionp || objectp->mLocalID == 0) return;

    s.mWatchStart = F64Seconds(LLFrameTimer::getElapsedSeconds());
    // mProbeCount is deliberately not reset here - it is reset only when the simulator sends
    // a fresh update, so an object that already exhausted its probe budget cannot be re-armed
    // frame after frame.
    s.mOnWatch = true;
    sWatchList.push_back(objectp);
}

// static
bool FSGhostProjectile::idleUpdateWatched(LLViewerObject* objectp, F64Seconds frame_time)
{
    FSGhostProjectileState& s = objectp->mGhostProjectile;

    // Stop watching once the object is gone, stopped looking like a projectile, or started
    // moving again (in which case interpolateLinearMotion() covers it once more).
    if (objectp->mDead || !objectp->mRegionp || objectp->mLocalID == 0 || !s.mLikely || !objectp->mStatic)
    {
        s.mGhosted = false;
        return false;
    }

    const F64Seconds threshold = getProbeThreshold();

    // Give the object the full probe budget after it goes silent before abandoning the watch.
    if (frame_time - s.mWatchStart > threshold + PROBE_INTERVAL * (F64)(MAX_WATCH_PROBES + 1))
    {
        s.mGhosted = false;
        return false;
    }

    // Still hearing from the simulator about this object, so it is not a ghost yet. Keep
    // watching - it only becomes interesting once the updates stop for long enough.
    const F64Seconds time_since_last_update = frame_time - objectp->mLastMessageUpdateSecs;
    if (time_since_last_update < threshold) return true;

    if (frame_time - s.mLastProbe < PROBE_INTERVAL) return true;

    if (s.mProbeCount >= MAX_WATCH_PROBES)
    {
        // Probed repeatedly without the simulator killing it or updating it. Stop generating
        // traffic for it.
        LL_DEBUGS("Projectile") << "Object " << objectp->getID() << " survived " << s.mProbeCount
                                << " existence probes, dropping ghost projectile watch." << LL_ENDL;
        s.mGhosted = false;
        return false;
    }

    LL_DEBUGS("Projectile") << "Probing static ghost projectile " << objectp->getID() << " (probe "
                            << (s.mProbeCount + 1) << ", "
                            << time_since_last_update.value() << "s since last update)" << LL_ENDL;

    s.mLastProbe = frame_time;
    ++s.mProbeCount;
    sendProbe(objectp);
    return true;
}

// Briefly select and deselect the object server-side. An object the simulator no longer
// knows about answers with a KillObject, which is what culls ghosts when a user clicks one.
// RequestMultipleObjects only refreshes objects that still exist, so it cannot cull. The
// selection never enters the viewer-side selection list.
// static
void FSGhostProjectile::sendProbe(LLViewerObject* objectp)
{
    LLViewerRegion* regionp = objectp->getRegion();
    // <GMDX> Never probe an object the user has selected: the deselect would drop it server-side.
    if (!regionp || objectp->isSelected()) return;

    objectp->mGhostProjectile.mGhosted = true;

    LLMessageSystem* msg = gMessageSystem;

    msg->newMessageFast(_PREHASH_ObjectSelect);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, objectp->getLocalID());
    msg->sendReliable(regionp->getHost());

    msg->newMessageFast(_PREHASH_ObjectDeselect);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, objectp->getLocalID());
    msg->sendReliable(regionp->getHost());
}

// static
void FSGhostProjectile::removeFromWatch(LLViewerObject* objectp)
{
    if (!objectp->mGhostProjectile.mOnWatch) return;

    objectp->mGhostProjectile.mOnWatch = false;
    for (auto iter = sWatchList.begin(); iter != sWatchList.end(); ++iter)
    {
        if (iter->get() == objectp)
        {
            sWatchList.erase(iter);
            break;
        }
    }
}

// Probes projectiles that went static. They are no longer interpolated and, for LLVOVolume,
// are no longer on the active list at all, so idleUpdate() cannot reach them.
// static
void FSGhostProjectile::idleUpdate(F64 frame_time)
{
    if (sWatchList.empty()) return;

    // Iterate a copy: probes can kill objects, which removes them from the watch list.
    const std::vector<LLPointer<LLViewerObject> > watch_list = sWatchList;
    for (const LLPointer<LLViewerObject>& objectp : watch_list)
    {
        if (objectp.isNull() || !objectp->mGhostProjectile.mOnWatch) continue; // already removed this frame

        if (objectp->isDead() || !idleUpdateWatched(objectp, F64Seconds(frame_time)))
        {
            removeFromWatch(objectp);
        }
    }
}

// static
void FSGhostProjectile::onCleanup(LLViewerObject* objectp)
{
    FSGhostProjectileState& s = objectp->mGhostProjectile;
    if (s.mGhosted)
    {
        ++sNumDropped;
        LL_DEBUGS("Projectile") << "Dropped projectile " << objectp->getID() << " was ghosted" << LL_ENDL;
    }
    track_likely(s.mLikely, false);
    // <GMDX> cleanupReferences can run twice for one object; count it once
    s.mLikely = false;
    s.mGhosted = false;
    // </GMDX>
    removeFromWatch(objectp);
}

// static
void FSGhostProjectile::clear()
{
    for (LLPointer<LLViewerObject>& objectp : sWatchList)
    {
        if (objectp.notNull())
        {
            objectp->mGhostProjectile.mOnWatch = false;
        }
    }
    sWatchList.clear();
}

// static
U32 FSGhostProjectile::getNumLikely() { return sNumLikely; }
// static
U32 FSGhostProjectile::getNumDropped() { return sNumDropped; }
// static
U32 FSGhostProjectile::getNumWatched() { return (U32)sWatchList.size(); }

// </SS:Nexii>
