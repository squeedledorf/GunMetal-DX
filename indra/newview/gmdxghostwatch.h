/**
 * @file gmdxghostwatch.h
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: the ghosted physical object rules
// (workaround-ghosted-physical-objects, eba54dea60; his rewrite of the SoapStorm ghost projectile watch
// ee21d2505b that GunMetal-DX had as fsghostprojectile.cpp).
//
// A physical temporary object (weapon fire, mostly) that loses its simulator, at a region border or in
// a failed crossing, is left by the viewer at its last known velocity until prediction stops it, and then
// stands there for the rest of the session. The simulator answers a select of a local id it no longer has
// with KillObject, so a probe (select, deselect) clears a ghost. The rules:
// - The viewer stopping an object at a border (the clip at a void edge, a crossing predicted too long)
//   is evidence enough: the suspect is hidden at once, its looping sound muted, then probed; still there
//   a second after the probe, it is restored. Any update for it restores it at once.
// - Elsewhere silence is judged by distance, not time: an object that stopped is probed once its last
//   speed times the silence before it stopped comes to 5 m (0.1 s of weapon fire, not drifting prims).
// - No probes while the region's circuit is dead or blocked: that silence is the network's.
//
// This is the one ghost watch. It replaces the SoapStorm watch's time thresholds (fsghostprojectile.cpp
// keeps its projectile counts) and takes over Slipstream's boundary probe (sltempobjectedge.cpp, 3d8fe26f58)
// at the two places it was fed, which keeps its rules: a selected object is never probed, the region,
// circuit and identity are checked before each probe, and an object is never removed locally.

#pragma once

#include "stdtypes.h"

class LLViewerObject;

namespace GMDXGhostWatch
{
    // End of LLViewerObject::processUpdateMessage: the simulator still knows this object.
    void onSimUpdate(LLViewerObject* objectp);
    // LLViewerObject::interpolateLinearMotion, where the viewer itself stops an object at a region border.
    void onBoundaryStop(LLViewerObject* objectp);
    // Once per frame (LLViewerObjectList::update): four sweeps a second.
    void tick();
    // LLViewerObjectList::killAllObjects
    void clear();
    // LLCleanIngest::registerCleanFace: a suspect hidden while it is asked about draws nothing.
    bool isHidden(const LLViewerObject* objectp);

    U32 numWatched();
    U32 numDropped();   // probed and then killed by the simulator
}
