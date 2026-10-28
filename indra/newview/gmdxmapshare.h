/**
 * @file gmdxmapshare.h
 * @brief GunMetal-DX: sharing mini-map marks and zones, and pings, with people in the same region.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> Shared map items, between people in the same region only. Everything goes as one ordinary,
// readable line (gmdxmapsharetext.h: what, a name, a map link, a visible "(gmdx:...)" tail) that the
// sender's GunMetal-DX Helper (slbridge.h) says privately to each recipient in the region
// (llRegionSayTo on channel 0, under the name "Ping from <username>" / "Mark from" / "Zone from"):
// any viewer shows it as object chat with its map link, GMDX draws it. Nobody outside the region is
// ever sent anything, and nothing goes over IM, group chat, conferences or local chat. Nothing is sent
// unless the user asks (Share, Ping, the ping keys), with rate limits (shares: 1 a second, 10 in 30 s;
// pings: 1 in 2 s, 5 in 30 s; the helper's own: 4 at once, 0.6 a second), and only while sending is on
// (GMDXMapShareSend, which keeps the helper worn).
//
// Who gets them (the targets, "my list"): my group, the one whose tag I wear (GMDXMapShareMyGroup),
// and the groups and people I added and ticked. Added ones last the session; Keep makes them stay
// (GMDXMapShareTargetGroups / GMDXMapShareTargetPeople). Each send goes to those of them in my region
// at that moment (a group: its members here, from the group's member list or their tag), at most 50
// people; a mark's or zone's Share to Group goes only to the members here of the group whose tag I
// wear. No targets here means nobody: nothing ever goes to everyone in the region. An added person
// who is not kept is dropped once they have been gone from my region for a few seconds (a kept one
// stays listed and gets them again when back).
//
// Receiving (llviewermessage's object chat hook, consumeObjectChat): a well-formed line said to me by
// an object named for a share, whose owner is an avatar in my region (and, when I can see the object,
// worn by them) counts as from that owner. It is taken out of the chat (no nearby chat line, toast or
// log) and, when the owner is in my list (any person in it, ticked or not) or a member of a group I
// send to (my group while GMDXMapShareMyGroup, the ticked groups) and not blocked
// (GMDXMapShareBlocked), drawn on the mini-map in a "shared" layer under the owner's name, and a ping
// also over the world for GMDXPingSeconds. Friends are not trusted for being friends. Anyone else's
// line stays the readable chat line it is; a mark or zone asks (Accept / Always: add to my list, kept,
// not ticked / Deny / Mute: GMDXMapShareBlocked), a ping does not (no marker, no sound); a blocked
// or muted owner's stays text. Anything else said by an object stays ordinary chat. What was shared
// lives only while both are in the region: a sender's items go when they leave it, and all of them
// when I do. GMDXMapShareShow off: lines stay chat text and are not drawn; GMDXMapShareShowAsText: they
// are drawn and also stay chat text (for checking). Right-click a shared item to keep it (a copy in
// my marks / zones), remove it, or block its sender.

#pragma once

#include "gmdxminimapmarks.h"
#include "gmdxmapsharetext.h"

class LLMenuGL;
class LLWorldMapView;

struct GMDXSharedItem
{
    LLUUID id;
    GMDXShareText::Type type = GMDXShareText::Type::Mark;
    LLUUID sender;
    std::string sender_name;    // from the helper's name, cleaned: shown until the name cache has the avatar
    GMDXMapMark mark;           // marks and pings: the position, name and colour
    GMDXMapZone zone;           // zones
    S32 ping = 0;               // 0 ping, 1 enemy, 2 rally
    F64 arrived = 0.0;          // LLFrameTimer::getTotalSeconds
    F64 expires = 0.0;          // pings
    LLUUID anchor;              // pings: the object / avatar it is on, followed while this viewer has it
    std::string on;             // pings: that one's name as sent ("" unknown)
    LLVector3 anchor_off;       // pings: the point picked on it, in its own frame (zero: its origin)
    bool map_only = false;      // pings placed on a map: on maps only, never over the world
};

class GMDXMapShare
{
public:
    // After login (llstartup): the helper while sending is on, the targets kept from before, the watch
    // on my region.
    static void init();

    // Where my shares and pings go, besides my group (GMDXMapShareMyGroup).
    struct Target
    {
        enum Kind { GROUP, PERSON } kind = GROUP;
        LLUUID id;
        bool on = true;     // ticked: gets my shares and pings
        bool kept = false;  // stays for the next login
        F64 gone_since = 0.0;   // a person: when they were last seen leaving my region (0 = here)
    };
    static const std::vector<Target>& targets();
    static void addTarget(Target::Kind kind, const LLUUID& id);   // for this session, ticked
    static void removeTarget(LLUUID id);
    static void setTargetOn(const LLUUID& id, bool on);
    static void setTargetKept(const LLUUID& id, bool kept);
    static std::string targetName(const Target& t);

    // Share one of my marks / zones (group_only: Share to Group, my worn group's members only), or
    // ping a place (the ping keys: under the cursor / at the crosshair; a ping that lands on an
    // object or avatar is on it, at the picked point, and follows it on receivers' maps), to the
    // targets in my region. A ping placed on the mini-map (ping) shows on maps only, for me and for
    // them; one from the world (pingAtCursor) also over the world.
    static void shareMark(const LLUUID& mark_id, bool group_only = false);
    static void shareZone(const LLUUID& zone_id, bool group_only = false);
    static void ping(const LLVector3d& pos_global, S32 kind);
    static void pingAtCursor(S32 kind);
    // pingAtCursor in two steps (the ping wheel: the spot when its key goes down, the kind when it
    // comes up): where (false: nothing there, and a tip says so), then send to it.
    struct PingTarget
    {
        LLVector3d pos;
        LLUUID on;          // the object / avatar it landed on (null: none)
        LLVector3 off;      // the spot on it, in its own frame
    };
    static bool pingTarget(PingTarget& target);
    static void pingTo(const PingTarget& target, S32 kind);
    static LLColor4 pingColor(S32 kind);   // ping yellow, enemy red, rally green

    // An object's line said to me alone (llRegionSayTo, chat type DIRECT): true = a share from
    // someone's helper, taken out of the chat (and drawn when accepted).
    static bool consumeObjectChat(const LLUUID& object_id, const LLUUID& owner_id, const std::string& object_name,
                                  const std::string& text);

    // What others shared (expired pings gone), drawn by the mini-map.
    static const std::vector<GMDXSharedItem>& items();
    static const GMDXSharedItem* find(const LLUUID& id);
    static std::string senderName(const GMDXSharedItem& item);
    static LLColor4 senderColor(const GMDXSharedItem& item);   // the sender's group colour, else light grey
    static std::string anchorName(const GMDXSharedItem& item); // what a ping is on, as I may see it ("" none)
    static void keep(const LLUUID& id);         // a copy in my marks / zones
    static void remove(LLUUID id);              // (by value: callers pass ids from the items)
    static void removeFrom(LLUUID sender);
    // GMDXMapShareBlocked: their items gone, and from now on nothing of theirs is drawn
    static void block(LLUUID sender);
    static void unblockAll();
    static void clear();

    // The mini-map menu's Share and Ping > Send To submenus, Share to <my group>, and Show > Unblock
    // All Map Shares (its count).
    static void fillShareMenus(LLMenuGL* menu);

    // Pings over the world (LLHUDView::draw), and on the World Map (LLWorldMapView::draw).
    static void drawWorld();
    static void drawWorldMap(LLWorldMapView* view);
    // A ping's radar blip at (x, y) (GMDXPingBlip: a flash and three rings sonar-style out to
    // max_radius px on arrival; off: one ring a second for 2 s), and how faded its dot is now (on a
    // map with the blip: over its whole life; else over its last 1.5 s).
    static void drawBlip(F32 x, F32 y, const GMDXSharedItem& item, F32 max_radius);
    static F32 pingAlpha(const GMDXSharedItem& item, bool on_map);
};
