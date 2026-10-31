/**
 * @file gmdxghostwatch.cpp
 * @brief Physical temporary objects the simulator has forgotten: hidden, asked about, removed or restored.
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

// <GMDX> See gmdxghostwatch.h. From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: skUpdateGhostWatch and its
// helpers (llviewerobjectlist.cpp, eba54dea60). GunMetal-DX keeps the state here instead of in
// LLViewerObject / LLViewerObjectList members, hides through the Vulkan instance store (the renderer has
// no FORCE_INVISIBLE) and sends one select / deselect pair per region per sweep.

#include "llviewerprecompiledheaders.h"

#include "gmdxghostwatch.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llcleaningest.h"
#include "llcleaninstancestore.h"
#include "llframetimer.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworldclock.h"
#include "message.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Sampling a position four times a second is enough to tell moving from stopped.
    constexpr F64 SWEEP_INTERVAL = 0.25;
    // How long an object must hold still before it counts as stopped rather than slow. Only for objects
    // that came to rest away from a border, where there is nothing better to go on.
    constexpr F64 STILL_TIME = 0.5;
    // A simulator streaming a fast object speaks far more often than this, so silence for longer than one
    // of its own update intervals is already out of character.
    constexpr F64 UNANNOUNCED_STOP = 0.1;
    // What makes that silence evidence is the ground covered on nothing but a remembered velocity. Time
    // alone cannot say: a tenth of a second is 20 m of weapon fire and 10 cm of drifting prim.
    constexpr F32 UNCONFIRMED_COAST = 5.f; // metres
    // Ignore drift below this, so a residual trickle of prediction does not read as motion.
    constexpr F32 STILL_DISTANCE_SQ = 1e-6f; // 1 mm
    // How long to let a probe be answered. A ghost is killed well inside this; anything still here
    // afterwards was real and is put back.
    constexpr F64 CONFIRM_TIME = 1.0;
    // (GMDX) Local ids per select / deselect message.
    constexpr size_t MAX_PROBES_PER_MESSAGE = 64;

    struct Candidate
    {
        LLPointer<LLViewerObject> mObject;
        LLVector3d mLastPos;    // (GMDX: global, so a region crossing is no jump)
        F64 mLastMovedAt = 0.0;
        F64 mProbedAt = 0.0;    // zero until probed
        F32 mLastSpeed = 0.f;
        F32 mAudioGain = -1.f;  // gain before it was silenced (< 0: nothing silenced)
        bool mHalted = false;   // the viewer stopped it at a border, not the simulator
        bool mHidden = false;
    };
    std::unordered_map<const LLViewerObject*, Candidate> sCandidates;
    std::unordered_set<const LLViewerObject*> sHidden;
    F64 sNextSweep = 0.0;
    U32 sDropped = 0;

    // Weapon fire and the like: physical, temporary, a world root.
    bool eligible(const LLViewerObject* objectp)
    {
        return objectp && !objectp->isDead() && objectp->getRegion() && objectp->getLocalID() != 0
            && !objectp->getParent() && !objectp->isAvatar() && !objectp->isAttachment()
            && objectp->flagUsePhysics() && objectp->flagTemporaryOnRez();
    }

    Candidate* watch(LLViewerObject* objectp)
    {
        auto it = sCandidates.find(objectp);
        if (it != sCandidates.end())
        {
            return &it->second;
        }
        if (!eligible(objectp))
        {
            return nullptr;
        }
        Candidate& c = sCandidates[objectp];
        c.mObject = objectp;
        c.mLastPos = objectp->getPositionGlobal();
        c.mLastMovedAt = LLFrameTimer::getElapsedSeconds();
        return &c;
    }

    // Take a suspect off the screen while the simulator is asked about it. A ghost is nearly always what
    // this is, so hiding first means the common case disappears on sight; the rare survivor is restored.
    // Looping sounds go quiet the same way: a ghosted weapon hanging at a border is as audible as visible.
    void hide(Candidate& c)
    {
        LLViewerObject* objectp = c.mObject;
        c.mHidden = true;
        sHidden.insert(objectp);
        LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
        store.releaseObject(objectp->getID());
        for (LLViewerObject* child : objectp->getChildren())
        {
            store.releaseObject(child->getID());
        }
        if (gAudiop)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(objectp->getID()))
            {
                c.mAudioGain = source->getGain();
                objectp->adjustAudioGain(0.f);
            }
        }
    }

    void restore(Candidate& c)
    {
        LLViewerObject* objectp = c.mObject;
        c.mHidden = false;
        sHidden.erase(objectp);
        if (objectp->isDead())
        {
            return;
        }
        LLCleanIngest::instance().noteObjectChanged(objectp);   // registers its faces again
        for (LLViewerObject* child : objectp->getChildren())
        {
            LLCleanIngest::instance().noteObjectChanged(child);
        }
        if (c.mAudioGain >= 0.f)
        {
            // (GMDX) Only if still muted: the update that restores it may have set a gain of its own.
            LLAudioSource* source = gAudiop ? gAudiop->findAudioSource(objectp->getID()) : nullptr;
            if (source && source->getGain() <= 0.f)
            {
                objectp->adjustAudioGain(c.mAudioGain);
            }
            c.mAudioGain = -1.f;
        }
    }

    // Briefly select and deselect the objects server-side. A simulator answers a select for a local id it
    // no longer owns with KillObject, which is what culls a ghost when a user clicks one.
    // RequestMultipleObjects cannot do this: it only refreshes objects that still exist.
    void sendProbes(const LLHost& host, const std::vector<U32>& local_ids)
    {
        LLMessageSystem* msg = gMessageSystem;
        for (size_t start = 0; start < local_ids.size(); start += MAX_PROBES_PER_MESSAGE)
        {
            const size_t end = llmin(local_ids.size(), start + MAX_PROBES_PER_MESSAGE);
            for (const char* message_name : { _PREHASH_ObjectSelect, _PREHASH_ObjectDeselect })
            {
                msg->newMessageFast(message_name);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                for (size_t i = start; i < end; ++i)
                {
                    msg->nextBlockFast(_PREHASH_ObjectData);
                    msg->addU32Fast(_PREHASH_ObjectLocalID, local_ids[i]);
                }
                msg->sendReliable(host);
            }
        }
    }
}

void GMDXGhostWatch::onSimUpdate(LLViewerObject* objectp)
{
    if (Candidate* c = watch(objectp))
    {
        // The simulator still knows about this one, which answers any probe too (GMDX: so a later halt at a
        // border is judged afresh instead of ending with the probe's confirm second).
        c->mHalted = false;
        c->mProbedAt = 0.0;
        if (c->mHidden)
        {
            restore(*c);
        }
    }
}

void GMDXGhostWatch::onBoundaryStop(LLViewerObject* objectp)
{
    if (Candidate* c = watch(objectp))
    {
        c->mHalted = true;
    }
}

void GMDXGhostWatch::tick()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // Out of a frozen world (Freeze World, FreezeTime), where nothing moves and everything would read as stopped.
    if (sCandidates.empty() || now < sNextSweep || !gMessageSystem || LLWorldClock::isFrozen())
    {
        return;
    }
    sNextSweep = now + SWEEP_INTERVAL;

    std::map<LLHost, std::vector<U32>> probes;
    for (auto it = sCandidates.begin(); it != sCandidates.end();)
    {
        Candidate& c = it->second;
        LLViewerObject* objectp = c.mObject;
        bool done = true;
        bool probe = false;

        // Stop watching anything that died, lost its region, or is no longer the kind of object this is about.
        if (eligible(objectp))
        {
            const LLVector3d pos = objectp->getPositionGlobal();
            const F32 step_sq = (F32)(pos - c.mLastPos).magVecSquared();
            const F64 silence = c.mLastMovedAt - objectp->lastSimulatorUpdateSeconds();

            if (c.mProbedAt > 0.0)
            {
                // Already asked. A ghost is killed by the answer and never reaches this again.
                done = now - c.mProbedAt >= CONFIRM_TIME;
            }
            else if (objectp->isSelected())
            {
                // Deselecting it out from under the user would be worse than leaving a ghost.
                done = false;
            }
            else if (c.mHalted)
            {
                // The viewer stopped this one itself, at a border, for want of anything saying where it
                // went. That needs no corroborating. Hide it now and ask afterwards, so the answer only has
                // to undo something in the rare case where the object turns out to still exist.
                done = false;
                if (!c.mHidden)
                {
                    hide(c);
                }
                else
                {
                    probe = true;
                }
            }
            else if (step_sq > STILL_DISTANCE_SQ)
            {
                // Still moving. Remember how fast, so there is a speed to weigh the silence against once it
                // does stop.
                const F32 sampled = (F32)(now - c.mLastMovedAt);
                c.mLastSpeed = sampled > 0.f ? sqrtf(step_sq) / sampled : 0.f;
                c.mLastPos = pos;
                c.mLastMovedAt = now;
                done = false;
            }
            else if (now - c.mLastMovedAt < STILL_TIME || silence < UNANNOUNCED_STOP
                     || c.mLastSpeed * (F32)silence < UNCONFIRMED_COAST)
            {
                done = false;
            }
            else
            {
                probe = true;
            }
        }
        else if (objectp && objectp->isDead() && c.mProbedAt > 0.0)
        {
            ++sDropped;   // the probe's answer killed it
        }

        if (probe)
        {
            // A probe only means anything while the simulator can answer it. On a circuit that has stopped
            // talking the reply would never come, and the silence being read as evidence is the circuit's,
            // not the object's -- so wait for it to come back.
            LLViewerRegion* regionp = objectp->getRegion();
            LLCircuitData* cdp = gMessageSystem->mCircuitInfo.findCircuit(regionp->getHost());
            if (regionp->isAlive() && cdp && cdp->isAlive() && !cdp->isBlocked())
            {
                LL_DEBUGS("Interpolate") << "Probing possibly ghosted physical object " << objectp->getID()
                                         << ", unheard from for " << (now - objectp->lastSimulatorUpdateSeconds()) << "s"
                                         << (c.mHalted ? ", halted at a border" : "") << LL_ENDL;
                probes[regionp->getHost()].push_back(objectp->getLocalID());
                c.mProbedAt = now;
            }
            done = false;
        }

        // One probe settles it either way, so nothing is watched past its answer.
        if (done)
        {
            if (objectp && c.mHidden)
            {
                restore(c);
            }
            it = sCandidates.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& [host, local_ids] : probes)
    {
        sendProbes(host, local_ids);
    }
}

void GMDXGhostWatch::clear()
{
    sCandidates.clear();
    sHidden.clear();
    sNextSweep = 0.0;
}

bool GMDXGhostWatch::isHidden(const LLViewerObject* objectp)
{
    if (sHidden.empty() || !objectp)
    {
        return false;
    }
    const LLViewerObject* root = objectp->getRootEdit();
    return sHidden.count(root ? root : objectp) != 0;
}

U32 GMDXGhostWatch::numWatched()
{
    return (U32)sCandidates.size();
}

U32 GMDXGhostWatch::numDropped()
{
    return sDropped;
}
