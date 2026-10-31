/**
 * @file sltempobjectedge.cpp
 * @brief One-shot, identity-checked boundary existence probes; no local object expiry.
 *
 * Copyright (c) 2026 Slipstream contributors
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "llviewerprecompiledheaders.h"
#include "sltempobjectedge.h"
#include "gmdxghostwatch.h" // <GMDX>
#include "llagent.h"
#include "llframetimer.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "message.h"
#include <array>
#include <algorithm>

namespace
{
    constexpr size_t kCapacity = 128, kChecksPerFrame = 16;
    constexpr F64 kWatchSeconds = 20.0, kGlobalInterval = 1.0;
    struct Watch
    {
        LLUUID id;
        U64 region = 0;
        LLHost host;
        U32 local = 0;
        F64 update = 0.0, stopped = 0.0;
        bool sent = false;
    };
    std::array<Watch, kCapacity> sWatch;
    size_t sCursor = 0, sCount = 0;
    F64 sNextProbe = 0.0, sNextLog = 0.0;
    U64 sAdmitted = 0, sProbed = 0, sExpired = 0, sOverflow = 0;

    bool eligible(LLViewerObject* object)
    {
        return object && !object->getID().isNull() && !object->isDead() && object->flagTemporaryOnRez()
            && !object->isAvatar() && !object->getAvatarAncestor()
            && !object->getParent() && object->getLocalID() != 0 && object->getRegion();
    }
    void remove(Watch& entry)
    {
        entry = Watch{};
        --sCount;
    }
    void logCounts(F64 now)
    {
        if (now < sNextLog) return;
        sNextLog = now + 10.0;
        LL_INFOS("TempObjectEdge") << "boundary watch live=" << sCount << '/' << kCapacity
            << " admitted=" << sAdmitted << " one_shot_probes=" << sProbed
            << " watch_expired=" << sExpired << " capacity_refused=" << sOverflow << LL_ENDL;
    }
}

void SLTempObjectEdge::stopped(LLViewerObject* object)
{
    // <GMDX> One ghost watch: a boundary stop goes to gmdxghostwatch.cpp (Alchemy: Skooma's rules: hide at
    // once, probe, show it again if it is still there), which keeps this watch's selection, region and circuit checks.
    // This watch stays empty, so its tick() returns at once.
    GMDXGhostWatch::onBoundaryStop(object);
    return;
    // </GMDX>
    if (!eligible(object)) return;
    Watch* empty = nullptr;
    for (Watch& entry : sWatch)
    {
        if (entry.id == object->getID())
        {
            if (entry.update == object->lastSimulatorUpdateSeconds()
                && entry.region == object->getRegion()->getHandle()
                && entry.host == object->getRegion()->getHost()
                && entry.local == object->getLocalID()) return;
            // A new simulator update may lead to another actual boundary stop.
            remove(entry);
            empty = &entry;
            break;
        }
        if (!empty && entry.id.isNull()) empty = &entry;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!empty) { ++sOverflow; logCounts(now); return; }
    empty->id = object->getID();
    empty->region = object->getRegion()->getHandle();
    empty->host = object->getRegion()->getHost();
    empty->local = object->getLocalID();
    empty->update = object->lastSimulatorUpdateSeconds();
    empty->stopped = now;
    ++sCount;
    ++sAdmitted;
}

void SLTempObjectEdge::tick()
{
    if (sCount == 0 || !gMessageSystem) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 silence = std::max(5.0, LLViewerObject::maxMotionPredictionSeconds() + 2.0);
    for (size_t n = 0; n < kChecksPerFrame; ++n)
    {
        Watch& entry = sWatch[sCursor];
        sCursor = (sCursor + 1) % kCapacity;
        if (entry.id.isNull()) continue;
        LLViewerObject* object = gObjectList.findObject(entry.id);
        if (!eligible(object) || object->getLocalID() != entry.local
            || object->getRegion()->getHandle() != entry.region
            || object->getRegion()->getHost() != entry.host
            || object->lastSimulatorUpdateSeconds() != entry.update)
        { remove(entry); continue; }
        if (now - entry.stopped > kWatchSeconds)
        { ++sExpired; remove(entry); continue; } // expire bookkeeping, NEVER infer server deletion
        if (entry.sent || now < sNextProbe || now - entry.update < silence
            || now - entry.stopped < 2.0 || object->isSelected()
            || !object->getVelocity().isExactlyZero() || !object->getAcceleration().isExactlyZero()) continue;
        LLViewerRegion* region = object->getRegion();
        LLCircuitData* circuit = gMessageSystem->mCircuitInfo.findCircuit(region->getHost());
        if (!region->isAlive() || !circuit || !circuit->isAlive() || circuit->isBlocked()) continue;
        // The normal KillObject/update handlers remain authoritative. A silent
        // reply is not proof of death. Reliable transport is not an application retry.
        entry.sent = true;
        sNextProbe = now + kGlobalInterval;
        ++sProbed;
        for (const char* operation : {_PREHASH_ObjectSelect, _PREHASH_ObjectDeselect})
        {
            gMessageSystem->newMessageFast(operation);
            gMessageSystem->nextBlockFast(_PREHASH_AgentData);
            gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
            gMessageSystem->addU32Fast(_PREHASH_ObjectLocalID, entry.local);
            gMessageSystem->sendReliable(region->getHost());
        }
    }
    logCounts(now);
}

void SLTempObjectEdge::clear()
{
    sWatch = {};
    sCount = sCursor = 0;
    sNextProbe = sNextLog = 0.0;
}
