/**
 * @file slheldkills_test.cpp
 * @brief Freeze World (#1172) held kills: only a live, accepted update that the killing sim
 *        sent after the kill voids one (stale, reordered, resent, cached, rejected and other
 *        sims' updates do not), packet ids wrapping, re-kills, worn detaches and the thaw.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slheldkills.h"

#include "../test/lltut.h"

namespace
{
    const U32 SIM_A_IP = 0x0a000001, SIM_B_IP = 0x0a000002;
    const U32 PORT = 13000;
    const size_t CAP = 16;

    LLUUID idFor(U32 n)
    {
        LLUUID id;
        id.mData[0] = (U8)n;
        id.mData[15] = 0x5a;
        return id;
    }

    SLKillKey killFromA(U32 local_id, U32 packet) { return SLKillKey{ local_id, SIM_A_IP, PORT, packet }; }

    SLObjectUpdateStamp live(U32 ip, U32 packet, bool accepted = true)
    {
        SLObjectUpdateStamp u;
        u.accepted = accepted;
        u.ip = ip;
        u.port = PORT;
        u.packet = packet;
        return u;
    }

    SLObjectUpdateStamp cached()
    {
        SLObjectUpdateStamp u;
        u.from_cache = true;
        u.accepted = true;
        return u;
    }
}

namespace tut
{
    struct heldkills_data {};
    typedef test_group<heldkills_data> heldkills_test;
    typedef heldkills_test::object heldkills_object;
    tut::heldkills_test heldkills("SLHeldKills");

    // Packet order on a circuit: later, earlier, equal, and across the 24-bit wrap.
    template<> template<>
    void heldkills_object::test<1>()
    {
        const U32 top = SLHeldKills::PACKET_ID_RANGE - 1;
        ensure("later", SLHeldKills::packetAfter(106, 105));
        ensure("earlier", !SLHeldKills::packetAfter(104, 105));
        ensure("same packet", !SLHeldKills::packetAfter(105, 105));
        ensure("just past the wrap is later", SLHeldKills::packetAfter(2, top - 3));
        ensure("just before the wrap is earlier", !SLHeldKills::packetAfter(top - 3, 2));
        ensure("half the range away is not later", !SLHeldKills::packetAfter(SLHeldKills::PACKET_ID_RANGE / 2, 0));
    }

    // The ordering rule itself: the kill at packet 105 from sim A.
    template<> template<>
    void heldkills_object::test<2>()
    {
        const SLKillKey kill = killFromA(42, 105);
        ensure("a live, accepted update from A sent after the kill voids it", SLHeldKills::voids(kill, live(SIM_A_IP, 107)));
        ensure("sent before the kill, received after it: no", !SLHeldKills::voids(kill, live(SIM_A_IP, 103)));
        ensure("dropped by the object as older: no", !SLHeldKills::voids(kill, live(SIM_A_IP, 107, false)));
        ensure("loaded from the cache: no", !SLHeldKills::voids(kill, cached()));
        ensure("from another sim: no (cannot be ordered)", !SLHeldKills::voids(kill, live(SIM_B_IP, 107)));
        SLObjectUpdateStamp other_port = live(SIM_A_IP, 107);
        other_port.port = PORT + 1;
        ensure("another sim on the same address: no", !SLHeldKills::voids(kill, other_port));
        ensure("after the kill across the wrap",
               SLHeldKills::voids(killFromA(42, SLHeldKills::PACKET_ID_RANGE - 2), live(SIM_A_IP, 1)));
    }

    // Frozen: an avatar's kill is held; a reordered update from before the kill arrives after
    // it (accepted, being newer than the avatar's last update) and must not void the hold. The
    // thaw still applies the kill.
    template<> template<>
    void heldkills_object::test<3>()
    {
        SLHeldKills held;
        const LLUUID avatar = idFor(1);
        ensure("held", held.hold(avatar, killFromA(42, 105), 10.0, LLUUID::null, CAP));
        ensure("stale update keeps it", !held.onUpdate(avatar, live(SIM_A_IP, 104), 10.1));
        ensure("rejected update keeps it", !held.onUpdate(avatar, live(SIM_A_IP, 108, false), 10.2));
        ensure("cache load keeps it", !held.onUpdate(avatar, cached(), 10.3));
        ensure("other sim keeps it", !held.onUpdate(avatar, live(SIM_B_IP, 900), 10.4));
        ensure("still held", held.contains(avatar));

        const SLHeldKills::list_t thaw = held.takeAll();
        ensure_equals("the thaw applies it", thaw.size(), (size_t)1);
        ensure("the kill's key", thaw[0].first == avatar && thaw[0].second.key.packet == 105 && thaw[0].second.key.local_id == 42);
        ensure("nothing left", held.empty());
    }

    // The avatar comes back while frozen: the sim's later update voids the hold, and the thaw
    // applies nothing.
    template<> template<>
    void heldkills_object::test<4>()
    {
        SLHeldKills held;
        const LLUUID avatar = idFor(2);
        held.hold(avatar, killFromA(42, 105), 10.0, LLUUID::null, CAP);
        ensure("returned", held.onUpdate(avatar, live(SIM_A_IP, 140), 11.0));
        ensure("not held", !held.contains(avatar));
        ensure("thaw applies nothing", held.takeAll().empty());
        ensure("an update with nothing held voids nothing", !held.onUpdate(avatar, live(SIM_A_IP, 150), 12.0));
    }

    // Killed again while held: the later kill replaces the held one, an older (reordered)
    // kill does not; an update between the two kills voids nothing.
    template<> template<>
    void heldkills_object::test<5>()
    {
        SLHeldKills held;
        const LLUUID avatar = idFor(3);
        held.hold(avatar, killFromA(42, 105), 10.0, LLUUID::null, CAP);
        ensure("re-kill held", held.hold(avatar, killFromA(42, 120), 10.5, LLUUID::null, CAP));
        ensure("reordered older kill held", held.hold(avatar, killFromA(42, 110), 10.6, LLUUID::null, CAP));
        ensure("update between the kills keeps it", !held.onUpdate(avatar, live(SIM_A_IP, 115), 10.7));
        ensure("update after the last kill voids it", held.onUpdate(avatar, live(SIM_A_IP, 121), 10.8));

        // A kill from another sim (the object was re-keyed there): the latest received wins.
        held.hold(avatar, killFromA(42, 200), 11.0, LLUUID::null, CAP);
        held.hold(avatar, SLKillKey{ 7, SIM_B_IP, PORT, 5 }, 11.1, LLUUID::null, CAP);
        const SLHeldKills::list_t thaw = held.takeAll();
        ensure("one entry", thaw.size() == 1);
        ensure("sim B's kill", thaw[0].second.key.ip == SIM_B_IP && thaw[0].second.key.local_id == 7);
        ensure("first kill's time kept", thaw[0].second.t_first == 11.0);
    }

    // Worn objects: held only along with the wearer's kill; alone they are plain detaches
    // after the wait. When the wearer comes back, what it wore waits for it again.
    template<> template<>
    void heldkills_object::test<6>()
    {
        const F64 WAIT = 1.0;
        SLHeldKills held;
        const LLUUID avatar = idFor(4), hat = idFor(5), shoe = idFor(6);

        held.hold(shoe, killFromA(61, 100), 5.0, avatar, CAP);   // a detach: the wearer stays
        ensure("waits", held.takeDetaches(5.5, WAIT).empty());
        SLHeldKills::list_t detached = held.takeDetaches(6.0, WAIT);
        ensure("a detach applies after the wait", detached.size() == 1 && detached[0].first == shoe);

        held.hold(hat, killFromA(60, 200), 7.0, avatar, CAP);
        held.hold(avatar, killFromA(42, 201), 7.0, LLUUID::null, CAP);
        ensure("joined by its wearer's kill", held.takeDetaches(7.1, WAIT).empty());
        ensure("stays held past the wait", held.takeDetaches(20.0, WAIT).empty() && held.contains(hat));

        // the wearer is sent again: the hat waits for its own return from then on
        ensure("wearer back", held.onUpdate(avatar, live(SIM_A_IP, 230), 21.0));
        ensure("waits again", held.takeDetaches(21.5, WAIT).empty());
        detached = held.takeDetaches(22.0, WAIT);
        ensure("not sent again: applies as a detach", detached.size() == 1 && detached[0].first == hat);
    }

    // The cap refuses a new hold (the caller applies the kill) but not a re-kill; eraseIf
    // drops entries of objects already gone.
    template<> template<>
    void heldkills_object::test<7>()
    {
        SLHeldKills held;
        ensure("first", held.hold(idFor(10), killFromA(1, 1), 1.0, LLUUID::null, 2));
        ensure("second", held.hold(idFor(11), killFromA(2, 2), 1.0, LLUUID::null, 2));
        ensure("over the cap", !held.hold(idFor(12), killFromA(3, 3), 1.0, LLUUID::null, 2));
        ensure("re-kill at the cap", held.hold(idFor(10), killFromA(1, 4), 1.0, LLUUID::null, 2));
        held.eraseIf([](const LLUUID& id) { return id == idFor(11); });
        ensure_equals("one left", held.size(), (size_t)1);
        ensure("the right one", held.contains(idFor(10)));
    }
}
