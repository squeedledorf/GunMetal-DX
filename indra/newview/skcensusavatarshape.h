/**
 * @file skcensusavatarshape.h
 * @brief The census's avatar physics shapes: the sim's convex hull of an avatar, standing, sat on the ground or on an object.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: measured avatar hull (skcensusavatarshape, 8dd8b54a4f).
// GunMetal-DX converges the aim on it (fscombataim.cpp); it is never drawn.

#ifndef SK_CENSUSAVATARSHAPE_H
#define SK_CENSUSAVATARSHAPE_H

#include "v3math.h"

#include <vector>

class LLVOAvatar;

namespace skcensus
{
    enum class AvatarPose
    {
        STANDING,
        SIT_GROUND,
        SIT_OBJECT,
    };

    // The pose whose physics shape the sim gives an avatar now.
    AvatarPose avatarPose(LLVOAvatar* avatar);

    // The avatar's physics shape as outward-wound triangles, three corners each, in its own frame: x forward, y left,
    // z up, the origin at its position (its agent box's centre). The shapes were measured by casting rays at avatars
    // in-world; every ray agreed with these hulls to 2 mm. Standing follows the agent height (llGetAgentSize's z);
    // the sitting shapes were measured at one height only and are kept as measured.
    const std::vector<LLVector3>& avatarShape(AvatarPose pose, F32 agent_height);
}

#endif // SK_CENSUSAVATARSHAPE_H
