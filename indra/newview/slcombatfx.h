/**
 * @file slcombatfx.h
 * @brief CombatFX master gate and the combat bridge message types
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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

#ifndef SL_COMBATFX_H
#define SL_COMBATFX_H

#include "llviewercontrol.h"

#include <string>

// The combat features ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4
// (shoulder collision, ADS, true aim, Combat 2.0 hit markers / kill feed, ghost
// projectiles, rez queue, map share / pings, rez effects, loadouts) all sit behind ONE
// preference, CombatFX (Preferences > Combat), off by default. With it off there is no
// combat UI, no combat helper traffic and no combat-only per-frame work.
// TPS-owned framing remains available. Combat hooks return early on enabled() first.
namespace SLCombatFX
{
    inline bool enabled()
    {
        static LLCachedControl<bool> combat_fx(gSavedSettings, "CombatFX", false);
        return combat_fx;
    }

    // Helper bridge messages for combat (slbridge.cpp <-> slipstream_helper.lsl).
    // All of them live here, named "combat/<name>/<version>", so the planned bridge
    // protocol (#1213 / #1217: "#slb:<type>/<ver>|...", type registry, capability
    // negotiation) can take them over mechanically; the helper script spells the same
    // strings. A command is "<type>|<args>" on the helper's private channel; an event is
    // "#slb:<type>|<fields>" (llOwnerSay), routed by SLBridge::registerHandler(type).
    // The helper hears the region combat log only after CMD_FEED or CMD_HITS "|1" (sent
    // only with CombatFX on) and drops it on "|0" or when the viewer stops pinging.
    namespace Bridge
    {
        // viewer -> helper
        constexpr const char CMD_FEED[]    = "combat/feed/1";     // |1 / |0: forward every DEATH (kill feed)
        constexpr const char CMD_HITS[]    = "combat/hits/1";     // |1 / |0: forward the owner's hits and kills (hit marker)
        constexpr const char CMD_MAP_TO[]  = "combat/mapto/1";    // |<seq>|<key>,<key>,...: map share recipients
        constexpr const char CMD_MAP_SAY[] = "combat/mapsay/1";   // |<seq>|<line>: say a map share to them (llRegionSayTo)
        // helper -> viewer
        constexpr const char EVT_DEATH[]   = "combat/death/1";    // |<victim>|<killer>|<type>|<damage>|<weapon>
        constexpr const char EVT_HIT[]     = "combat/hit/1";      // |<D, d or K>|<target>|<agent 0/1>|<type>|<damage>
        constexpr const char EVT_MEM[]     = "combat/mem/1";      // |<script or lsd>|<free bytes>: map shares stopped

        // "<cmd>|<arg>"
        inline std::string command(const char* cmd, const std::string& arg) { return std::string(cmd) + "|" + arg; }
    }
}

#endif // SL_COMBATFX_H
