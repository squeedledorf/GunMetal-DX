/**
 * @file slphotostudiohaze.h
 * @brief Local Photo Studio presentation of height-fog preferences.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License, version 2.1 or
 * (at your option) any later version. This library is distributed without
 * any warranty; without even the implied warranty of merchantability or
 * fitness for a particular purpose. See the GNU Lesser General Public
 * License for more details.
 * $/LicenseInfo$
 */
#ifndef SL_PHOTOSTUDIOHAZE_H
#define SL_PHOTOSTUDIOHAZE_H

#include <algorithm>
#include <cmath>

namespace SLPhotoStudioHaze
{
inline bool isManual(double preference)
{
    return std::isfinite(preference) && preference >= 0.0;
}

// Return false for invalid widget input; never turn it into a saved value.
inline bool preferenceFromDisplay(double display, double scale, double limit, float& preference)
{
    if (!std::isfinite(display))
    {
        return false;
    }
    preference = static_cast<float>(std::clamp(display / scale, 0.0, limit));
    return true;
}
}
#endif
