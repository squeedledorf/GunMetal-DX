/**
 * @file slspeedfov.h
 * @brief Speed-based field of view (#663): the main camera's FoV widens with the speed of
 *        the camera's anchor (your avatar, or the vehicle / object you ride) and eases
 *        back when it slows down. Off by default.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_SPEEDFOV_H
#define SL_SPEEDFOV_H

#include "stdtypes.h"

namespace SLSpeedFov
{
    // Per camera update (LLAgentCamera::updateCamera, before the FoV is applied): samples
    // the anchor's motion, advances the smoothing and returns the extra vertical FoV in
    // radians (0 when the feature is off or suppressed). Frame-rate independent (it
    // measures its own elapsed time between calls).
    F32 update();

    // Drop the speed / boost state.
    void reset();
}

#endif // SL_SPEEDFOV_H
