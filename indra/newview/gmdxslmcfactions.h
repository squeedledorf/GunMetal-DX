/**
 * @file gmdxslmcfactions.h
 * @brief The SLMC faction table: a faction's main, militia and land groups are one side.
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: the SLMC faction table and its loader
// (skmapallies.cpp FactionTable, app_settings/sk_slmc_factions.xml, 86d86a029d). GunMetal-DX uses it
// for its group colours and the Group Viewer, not for Skooma's allies-only mini-map (nor map-share trust:
// a militia or land group can be open to anyone, so its tag proves nothing).

#pragma once

#include "lluuid.h"

#include <vector>
#include <string>

namespace GMDXSLMCFactions
{
    struct Faction
    {
        std::string mName;
        std::string mShort;
        std::vector<LLUUID> mGroups;   // main, militia, land, in that order, once each; civilian groups are not part of it
    };

    // The faction holding the group, or null (app_settings/gmdx_slmc_factions.xml, read on first use).
    const Faction* factionOf(const LLUUID& group);
}
