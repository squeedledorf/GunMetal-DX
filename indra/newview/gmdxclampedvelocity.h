/**
 * @file gmdxclampedvelocity.h
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: clamped velocity recovery
// (LLViewerObject::skRecoverClampedVelocity, feature-motion/unclamp 656243faf8..5357bf3c69; doc/skooma/MOTION.md).
//
// A terse update clamps each velocity axis to +-128 m/s (and acceleration to +-64 m/s^2), so a 200 m/s
// bullet arrived at about 130 m/s and a few degrees off, and the viewer snapped it forward on every
// update. The fastest unclamped axis is still exact: divided by its share of the direction of travel
// (from the last two update positions) it gives the speed back. Skooma kept the last position in
// three LLViewerObject members; GunMetal-DX keeps them in a table here, for fast objects only, so
// llviewerobject.h (included everywhere) is untouched.

#pragma once

class LLVector3;
class LLViewerObject;

namespace GMDXClampedVelocity
{
    // LLViewerObject::processUpdateMessage, for a terse update, after the velocity is decoded and
    // before anything uses it. new_pos_parent: the update's position (region frame for a root).
    void onTerseUpdate(LLViewerObject* objectp, const LLVector3& new_pos_parent);
    // LLViewerObjectList::killAllObjects
    void clear();
}
