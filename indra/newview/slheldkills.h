/**
 * @file slheldkills.h
 * @brief Freeze World (#1172): the kills held until the thaw, and the ordering rule that
 *        voids one. No viewer objects here (LLCrossingKeep resolves and applies them), so
 *        the rule is unit-tested (tests/slheldkills_test.cpp).
 *
 * WHAT VOIDS A HELD KILL. Only proof that the killing sim sent the object again after the
 * kill: a live update (not one loaded from the object cache), accepted by the object (not
 * dropped as an older packet, LLViewerObject::processUpdateMessage), from the same sim
 * (circuit), in a packet the sim sent after the KillObject. A sim numbers the packets of a
 * circuit in send order (24-bit ids, wrapping), so an update sent before the kill but
 * received after it (reordered, or a resend, which keeps its id) is older and voids
 * nothing, even when it is newer than the object's previous update. An update from another
 * sim cannot be ordered against the kill: it re-keys the object (another region / local
 * id), and the thaw skips a kill whose object is no longer under the killing key. An
 * object outlives neither its region nor that region's circuit, so a held kill's packet
 * id is never compared across circuits.
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

#ifndef SL_SLHELDKILLS_H
#define SL_SLHELDKILLS_H

#include "lluuid.h"
#include "stdtypes.h"

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

// A kill: the killing sim (host), the object's local id there, and the packet id the sim
// sent the KillObject in. A deferred or held kill is applied only to an object still under
// (host, local id): one the sim re-keyed meanwhile is not that kill's object any more.
struct SLKillKey
{
    U32 local_id = 0, ip = 0, port = 0;
    U32 packet = 0;
};

// A full update as it reached an object.
struct SLObjectUpdateStamp
{
    bool from_cache = false;    // loaded from the object cache: no packet, no ordering
    bool accepted = false;      // the object applied it (not dropped as an older packet)
    U32 ip = 0, port = 0;       // the sending sim
    U32 packet = 0;
};

class SLHeldKills
{
public:
    // Packet ids count up per circuit and wrap at LL_MAX_OUT_PACKET_ID (llcircuit.h); the
    // circuit code takes a gap of more than half the range as a wrap, and so does this.
    static constexpr U32 PACKET_ID_RANGE = 0x01000000;

    // a was sent after b on the same circuit
    static bool packetAfter(U32 a, U32 b);
    // The update proves the killing sim sent the object again after the kill (above)
    static bool voids(const SLKillKey& kill, const SLObjectUpdateStamp& update);

    struct Held
    {
        SLKillKey key;
        F64 t_first = 0.0;          // the original kill (a deferral's, if it ran out frozen)
        // A worn object: its wearer, and since when it waits for the wearer's kill to join
        // its own (the kill, or the wearer's return); once the wearer's kill is held too
        // (with_wearer) it stays held.
        LLUUID wearer;
        F64 wait_from = 0.0;
        bool with_wearer = false;
    };
    using list_t = std::vector<std::pair<LLUUID, Held>>;

    bool empty() const { return mHeld.empty(); }
    size_t size() const { return mHeld.size(); }
    bool contains(const LLUUID& id) const { return mHeld.count(id) != 0; }

    // Hold id's kill (wearer: null, or the avatar a worn object is on). Killed again while
    // held: the kill the sim sent last replaces the held one (one from another sim or under
    // another local id: the latest received; the object was re-keyed). False = a new hold
    // refused because the table is at cap (the caller applies the kill).
    bool hold(const LLUUID& id, const SLKillKey& key, F64 t_first, const LLUUID& wearer, size_t cap);
    // A full update reached id. True = it voids the held kill (dropped); for an avatar, what
    // it wore waits for it again from t (what the sim does not send again was detached).
    bool onUpdate(const LLUUID& id, const SLObjectUpdateStamp& update, F64 t);
    // Worn kills whose wearer's kill has not joined them within wait_s: plain detaches, taken
    // out to be applied now. One joined by its wearer's kill stays held until the thaw.
    list_t takeDetaches(F64 t, F64 wait_s);
    // The thaw (or memory pressure): every held kill, taken out to be applied.
    list_t takeAll();
    // Drop the entries pred(id) selects (objects already gone)
    void eraseIf(const std::function<bool(const LLUUID&)>& pred);
    void clear() { mHeld.clear(); }

private:
    std::unordered_map<LLUUID, Held> mHeld;
};

#endif // SL_SLHELDKILLS_H
