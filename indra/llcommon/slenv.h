/**
 * @file slenv.h
 * @brief Slipstream environment variables, with their pre-1.2 ALCHEMY_* names
 *        still read as a fallback (#126).
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

#ifndef SL_SLENV_H
#define SL_SLENV_H

#include <cstdlib>
#include <string>

// Header only and dependency free: the Vulkan sources outside the viewer's
// precompiled header use it too.
//
// sl_getenv("VK_VALIDATION") returns the value of SLIPSTREAM_VK_VALIDATION, or,
// when that is not set, of the old name ALCHEMY_VK_VALIDATION (kept for a few
// releases so existing scripts keep working). Null when neither is set.
inline const char* sl_getenv(const char* suffix)
{
    const std::string name = std::string("SLIPSTREAM_") + suffix;
    if (const char* value = std::getenv(name.c_str()))
    {
        return value;
    }
    const std::string old_name = std::string("ALCHEMY_") + suffix;
    return std::getenv(old_name.c_str());
}

#endif // SL_SLENV_H
