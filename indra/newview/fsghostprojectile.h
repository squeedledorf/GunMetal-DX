/**
 * @file fsghostprojectile.h
 * @brief Ghost projectile watch: spots projectiles the simulator has
 *        forgotten (stuck at a region edge, left behind by a failed
 *        crossing or derez) and asks the simulator whether they still exist.
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

#ifndef FS_GHOSTPROJECTILE_H
#define FS_GHOSTPROJECTILE_H

#include "llunits.h"

class LLViewerObject;

// <SS:Nexii> Projectile ghost probing
//
// SoapStorm keeps this inside LLViewerObject / LLViewerObjectList; GMDX keeps
// the logic here and leaves small hooks in those files.
//
// An object is tagged a likely projectile once, from its first update that
// carries a velocity: long (>= 4 m) and thin (<= 0.2 m on the other two axes),
// not an avatar or attachment. A likely projectile that goes without a
// simulator update for longer than the viewer interpolates
// (InterpolationTime) plus FSSoapstormGhostProjectileProbeDelay is probed:
// a brief server-side select and deselect, to which the simulator answers a
// KillObject when it no longer knows the object. That is what clicking a
// ghost does by hand.

// Per-object state, a member of LLViewerObject.
struct FSGhostProjectileState
{
    F64Seconds  mLastProbe{ 0.0 };   // last existence probe
    F64Seconds  mWatchStart{ 0.0 };  // when the object went on the watch list
    U32         mProbeCount = 0;     // probes on the watch list since the last simulator update
    bool        mOnWatch = false;    // on the watch list
    bool        mLikely = false;     // looks like a projectile
    bool        mTagged = false;     // the shape test has run
    bool        mGhosted = false;    // probed and not heard from since
    bool        mSeenMoving = false; // <GMDX> a simulator update has shown it moving
};

class FSGhostProjectile
{
public:
    // LLViewerObject::processUpdateMessage: the first update carrying a velocity tags the object.
    static void onVelocityUpdate(LLViewerObject* objectp);
    // LLViewerObject::processUpdateMessage, just before the object is marked static.
    static void onGoingStatic(LLViewerObject* objectp);
    // End of LLViewerObject::processUpdateMessage: a fresh update re-arms the probe budget.
    static void onSimUpdate(LLViewerObject* objectp);
    // LLViewerObject::idleUpdate, for a static object that is still on the active list.
    static void onIdleStatic(LLViewerObject* objectp);
    // LLViewerObject::interpolateLinearMotion: covers moving, phased-out and stalled projectiles.
    static void onInterpolate(LLViewerObject* objectp, F64Seconds frame_time, F64Seconds time_since_last_update);

    // LLViewerObjectList::update: probes static projectiles, which are off the active list.
    static void idleUpdate(F64 frame_time);
    // LLViewerObjectList::cleanupReferences
    static void onCleanup(LLViewerObject* objectp);
    // LLViewerObjectList::killAllObjects
    static void clear();

    // Counts for the debug text (DebugShowSoapstormProjectilesInfo).
    static U32 getNumLikely();
    static U32 getNumDropped();
    static U32 getNumWatched();

private:
    static void beginWatch(LLViewerObject* objectp);
    static bool idleUpdateWatched(LLViewerObject* objectp, F64Seconds frame_time); // false: drop from the watch
    static void sendProbe(LLViewerObject* objectp);
    static void removeFromWatch(LLViewerObject* objectp);
    static F64Seconds getProbeThreshold();
};
// </SS:Nexii>

#endif // FS_GHOSTPROJECTILE_H
