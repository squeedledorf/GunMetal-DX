// #GunMetal-DX Helper -- the GunMetal-DX viewer's on-demand bridge (Slipstream's
// "#Slipstream Helper", #500, under its own name and channels so the two viewers never
// answer for each other's helper).
//
// Slipstream Viewer Source Code, Copyright (C) 2026, Slipstream Viewer contributors.
// LGPL 2.1 (see the viewer's license). Generated into your inventory and uploaded by
// the viewer; edits are overwritten when the viewer updates it (the viewer versions the
// script by a hash of this file, so any change here updates every worn helper).
//
// A small message router between the viewer and the region:
//   viewer -> bridge : chat from the owner on a random private channel (said in hello)
//   bridge -> viewer : llOwnerSay lines prefixed "#slb:", swallowed by the viewer
//   bridge <-> others: the public Slipstream pose and consent channels, and the
//                      consent-confirm channel (avatar-spoken grant statements)
//   script info      : "scripts|<batch>|<key>,<key>,..." from the viewer is answered with
//                      the script counts, memory and time of those objects (#425)
//   region -> viewer : Combat 2.0 events from the region combat log, only while the
//                      viewer asks for them: every death (the kill feed), and the
//                      owner's own hits and kills (the hit marker)
// Identity always comes from the sim: llGetOwnerKey() of the speaking attachment, or the
// key of the avatar answering a dialog. The viewer never trusts a key in a message body.
// Only well-formed lines of the known kinds are forwarded, within per-speaker and overall
// rate budgets; everything else is dropped here.
//
// Silent unless driven: the helper never speaks first. The viewer asks for hello on a
// discovery channel derived from the owner's key (only the owner's own chat is heard
// there), and public-channel traffic is only forwarded while the viewer pinged within
// the last minute -- so a helper left worn under another viewer says nothing at all.
//
// Dialog answers are ordinary chat from the answering avatar at its position, so they
// are heard only by bridges within chat range (20 m) of that avatar. Slipstream targets
// re-state their consent as chat from the avatar itself (shout range, 100 m) on the
// confirm channel, which no attachment can imitate; bystanders further away than that
// cannot verify the grant and do not show the pose.

integer VERSION = [VERSION];
integer POSE_CHANNEL = [POSE_CHANNEL];
integer CONSENT_CHANNEL = [CONSENT_CHANNEL];
integer CONFIRM_CHANNEL = [CONFIRM_CHANNEL];
integer QUIET_AFTER = 60;       // seconds without a viewer command before we stop forwarding
integer MAX_DEATHS = 8;         // combat log deaths forwarded per second at most
integer MAX_HITS = 16;          // the owner's hits forwarded per second at most (kills get MAX_DEATHS more)

// Forwarding budgets (token buckets): per speaking owner, and for everything together.
float OWNER_BURST = 20.0;
float OWNER_RATE = 6.0;         // lines a second
float TOTAL_BURST = 40.0;
float TOTAL_RATE = 12.0;
integer MAX_TRACKED = 16;       // owners with a bucket; the least recently heard is dropped

key gOwner;
string gTag;            // first 8 hex digits of the owner's key: the consent button tag
integer gCmdChannel;
integer gHelloChannel;  // discovery: the viewer says "hello" here (same formula in slbridge.cpp)
integer gLastCmd;       // unix time of the last command from the viewer
list gAsked;            // avatars we sent a pose request to, awaiting their answer
list gPosing;           // targets we sent pose data for (stopped on detach)
integer gCombatWanted;  // the viewer's kill feed asked for the combat log (combat|1)
integer gHitsWanted;    // the viewer's hit marker asked for the owner's hits (hits|1)
integer gCombatOn;      // the combat log listen below exists (one, shared by both)
integer gCombatListen;  // its handle
integer gDeathSecond;   // unix second of the death budget
integer gDeathsSent;    // deaths forwarded in that second
integer gHitSecond;     // unix second of the hit budget
integer gHitsSent;      // hit lines forwarded in that second
integer gInfoWindow;    // unix time the current script-info rate window began
integer gInfoBatches;   // script-info batches answered in that window
integer gPoseListen;
integer gConsentListen;
integer gConfirmListen;
integer gPublicOn;      // public listens enabled (only while the viewer drives us)
list gBuckets;          // [owner, tokens, time] per tracked owner, least recently heard first
float gTotalTokens;
float gTotalTime;

integer INFO_MAX_KEYS = 25;     // keys per batch (the viewer sends at most this many)
integer INFO_WINDOW = 10;       // seconds
integer INFO_MAX_BATCHES = 20;  // batches per window; more are answered "busy"

// Where the worn helper sits, relative to its HUD point (#537). HUD space is y = left/right,
// z = up/down, 1 unit = the screen height: the view spans z -0.5..0.5 and y +-aspect/2
// (+-1.78 at 32:9). This is out of view at every aspect ratio, and inside the 3.5 m an
// attachment may sit from its point (viewer and sim clamp further offsets).
vector HIDDEN_POS = <0.0, 2.4, -2.4>;

hide()
{
    llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_SIZE, <0.01, 0.01, 0.01>,
        PRIM_COLOR, ALL_SIDES, <1.0, 1.0, 1.0>, 0.0, PRIM_TEXT, "", ZERO_VECTOR, 0.0]);
    // Off screen, so it never sits over the middle of the view (or catches clicks there).
    // Only while worn: on a rezzed object this would move it in the region.
    if (llGetAttached())
        llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_POS_LOCAL, HIDDEN_POS]);
}

hello()
{
    llOwnerSay("#slb:hello|" + (string)VERSION + "|" + (string)gCmdChannel + "|" + (string)llGetKey());
}

start()
{
    gOwner = llGetOwner();
    gTag = llGetSubString((string)gOwner, 0, 7);
    gLastCmd = 0;
    // Positive so the viewer can use plain ChatFromViewer (dialog replies cap at 255 bytes).
    // 1900000000..2139999999: never a Slipstream helper's (100000000..1099999999), never
    // a discovery channel, below COMBAT_CHANNEL and DEBUG_CHANNEL.
    gCmdChannel = 1900000000 + (integer)llFrand(240000000.0);
    // 0x60000000 + the first 7 hex digits of the owner's key: positive, and the viewer
    // computes the same number from its own agent id. Slipstream's helper uses 0x40000000,
    // so the same avatar's Slipstream helper never hears our hello (nor ours theirs).
    gHelloChannel = 0x60000000 + (integer)("0x" + llGetSubString((string)gOwner, 0, 6));
    llListen(gCmdChannel, "", gOwner, "");
    llListen(gHelloChannel, "", gOwner, "hello");
    gPoseListen = llListen(POSE_CHANNEL, "", NULL_KEY, "");
    gConsentListen = llListen(CONSENT_CHANNEL, "", NULL_KEY, "");
    gConfirmListen = llListen(CONFIRM_CHANNEL, "", NULL_KEY, "");
    gPublicOn = TRUE;
    publicListens(FALSE);   // until the viewer drives us
    gBuckets = [];
    gTotalTokens = TOTAL_BURST;
    gTotalTime = llGetTime();
}

// The public listens only run while the viewer drives us; an idle helper hears nothing.
publicListens(integer on)
{
    if (on == gPublicOn)
        return;
    gPublicOn = on;
    llListenControl(gPoseListen, on);
    llListenControl(gConsentListen, on);
    llListenControl(gConfirmListen, on);
    if (on)
        llSetTimerEvent(30.0);
    else
        llSetTimerEvent(0.0);
}

// One line from owner may be forwarded: both budgets have a token left.
integer allow(key owner)
{
    float now = llGetTime();
    gTotalTokens += (now - gTotalTime) * TOTAL_RATE;
    if (gTotalTokens > TOTAL_BURST)
        gTotalTokens = TOTAL_BURST;
    gTotalTime = now;
    if (gTotalTokens < 1.0)
        return FALSE;

    float tokens = OWNER_BURST;
    integer at = llListFindList(gBuckets, [owner]);
    if (at >= 0)
    {
        tokens = llList2Float(gBuckets, at + 1) + (now - llList2Float(gBuckets, at + 2)) * OWNER_RATE;
        if (tokens > OWNER_BURST)
            tokens = OWNER_BURST;
        gBuckets = llDeleteSubList(gBuckets, at, at + 2);
    }
    integer ok = (tokens >= 1.0);
    if (ok)
    {
        tokens -= 1.0;
        gTotalTokens -= 1.0;
    }
    gBuckets += [owner, tokens, now];
    if (llGetListLength(gBuckets) > MAX_TRACKED * 3)
        gBuckets = llDeleteSubList(gBuckets, 0, 2);
    return ok;
}

// "<prefix><key>" exactly, e.g. "G1|" + a 36-character key.
integer keyLine(string msg, string prefix)
{
    return llStringLength(msg) == 39 && llGetSubString(msg, 0, 2) == prefix;
}

// "<verb> <8-character tag>" exactly: a consent dialog answer.
integer answerLine(string msg, string verb)
{
    integer n = llStringLength(verb);
    return llStringLength(msg) == n + 9 && llGetSubString(msg, 0, n) == verb + " ";
}

// Speaker identity on the public channels: an attachment speaks for its owner; anything
// else (rezzed objects, avatars typing on the channel) is not an attachment.
key attachedOwner(key id)
{
    if (llList2Integer(llGetObjectDetails(id, [OBJECT_ATTACHED_POINT]), 0) == 0)
        return NULL_KEY;
    return llGetOwnerKey(id);
}

// Script counts, memory and time of objects in this region (#425). Commands come only
// from our owner on the private channel, and every field is still checked: the batch
// must be a plain number, keys must be valid 36-character keys, at most INFO_MAX_KEYS
// of them; anything else is skipped. Reply lines stay under ~950 bytes:
//   #slb:scripts|<batch>|<key>,<running>,<total>,<memory bytes>,<time us>|<key>,-|...
// where "<key>,-" means the object is not in this region.
scriptInfo(string arg)
{
    integer bar = llSubStringIndex(arg, "|");
    if (bar < 1 || bar > 9)
        return;
    string batch = llGetSubString(arg, 0, bar - 1);
    if ((string)((integer)batch) != batch)
        return;
    integer now = llGetUnixTime();
    if (now - gInfoWindow >= INFO_WINDOW)
    {
        gInfoWindow = now;
        gInfoBatches = 0;
    }
    if (++gInfoBatches > INFO_MAX_BATCHES)
    {
        llOwnerSay("#slb:scripts|" + batch + "|busy");
        return;
    }
    list ids = llParseString2List(llGetSubString(arg, bar + 1, -1), [","], []);
    integer n = llGetListLength(ids);
    if (n > INFO_MAX_KEYS)
        n = INFO_MAX_KEYS;
    string out = "";
    integer i;
    for (i = 0; i < n; ++i)
    {
        string id = llList2String(ids, i);
        if (llStringLength(id) == 36)
        {
            if ((key)id)
            {
                list d = llGetObjectDetails((key)id, [OBJECT_RUNNING_SCRIPT_COUNT, OBJECT_TOTAL_SCRIPT_COUNT,
                    OBJECT_SCRIPT_MEMORY, OBJECT_SCRIPT_TIME]);
                string e = id + ",-";
                if (llGetListLength(d) == 4)
                    e = id + "," + (string)llList2Integer(d, 0) + "," + (string)llList2Integer(d, 1) + ","
                        + (string)llList2Integer(d, 2) + "," + (string)((integer)(llList2Float(d, 3) * 1000000.0));
                if (llStringLength(out) + llStringLength(e) > 900)
                {
                    llOwnerSay("#slb:scripts|" + batch + out);
                    out = "";
                }
                out += "|" + e;
            }
        }
    }
    if (out != "")
        llOwnerSay("#slb:scripts|" + batch + out);
}

integer driven()
{
    return gLastCmd > 0 && llGetUnixTime() - gLastCmd < QUIET_AFTER;
}

// The region combat log (Combat 2.0): only sim events, from COMBAT_LOG_ID. It is busy in
// a fight, so it is heard only while the viewer's kill feed or hit marker asks for it.
combatListen(integer on)
{
    if (on && !gCombatOn)
    {
        gCombatListen = llListen(COMBAT_CHANNEL, "", COMBAT_LOG_ID, "");
        gCombatOn = TRUE;
    }
    else if (!on && gCombatOn)
    {
        llListenRemove(gCombatListen);
        gCombatOn = FALSE;
    }
}

// A key field of a combat event, NULL_KEY when missing or null.
key eventKey(string ev, string field)
{
    key k = (key)llJsonGetValue(ev, [field]);
    if (k)
        return k;
    return NULL_KEY;
}

// An object's name; "" for an avatar (OBJECT_NAME would give the avatar's name) or a
// missing object.
string objectName(key id)
{
    if (id)
        if (llGetAgentSize(id) == ZERO_VECTOR)
            return llList2String(llGetObjectDetails(id, [OBJECT_NAME]), 0);
    return "";
}

// One DEATH from the combat log, forwarded as
//   #slb:combat|<victim>|<killer>|<damage type>|<damage>|<weapon>
// (type and damage empty when unknown; the weapon last, as an object name may hold "|").
// A DEATH carries its type but no damage: the killing blow's DAMAGE normally arrives in the
// same message, and the caller passes its amount (or "").
combatDeath(string ev, string damage)
{
    key victim = eventKey(ev, "target");
    if (victim == NULL_KEY)
        return;
    string type = llJsonGetValue(ev, ["type"]);
    if (type == JSON_INVALID)
        type = "";

    // A mass kill must not flood the viewer: the feed shows a handful of lines anyway.
    integer now = llGetUnixTime();
    if (now != gDeathSecond)
    {
        gDeathSecond = now;
        gDeathsSent = 0;
    }
    if (++gDeathsSent > MAX_DEATHS)
        return;

    key source = eventKey(ev, "source");
    key killer = eventKey(ev, "owner");
    if (killer == NULL_KEY)
        killer = source;
    // Name the weapon while it still exists: a projectile usually dies on impact, so
    // fall back to what rezzed it (the gun, normally still worn by the killer).
    string weapon = objectName(source);
    if (weapon == "")
        weapon = objectName(eventKey(ev, "rezzer"));
    llOwnerSay("#slb:combat|" + (string)victim + "|" + (string)killer + "|"
        + llGetSubString(type, 0, 10) + "|" + llGetSubString(damage, 0, 15) + "|"
        + llGetSubString(weapon, 0, 63));
}

// One of the owner's own hits ("D", a DAMAGE; "d" when the same message holds the owner's
// kill of that target, so the viewer plays only the kill sound) or kills ("K", a DEATH),
// forwarded as
//   #slb:hit|<D, d or K>|<target>|<1 if the target is an avatar, else 0>|<damage type>|<damage>
// (type and damage empty when unknown). Anything not dealt by the owner, and the owner
// hurting themselves, stays here.
combatHit(string ev, string what)
{
    if (llJsonGetValue(ev, ["owner"]) != (string)gOwner)
        return;
    string target = llJsonGetValue(ev, ["target"]);
    if (target == JSON_INVALID || target == (string)gOwner)
        return;

    // A full-auto burst or a shotgun must not flood the viewer: it batches hits over 0.3 s
    // anyway. Kills get a little headroom over plain hits.
    integer now = llGetUnixTime();
    if (now != gHitSecond)
    {
        gHitSecond = now;
        gHitsSent = 0;
    }
    // Only lines sent count, so dropped hits never eat the kills' headroom.
    if (gHitsSent >= MAX_HITS + MAX_DEATHS || (gHitsSent >= MAX_HITS && what != "K"))
        return;
    ++gHitsSent;

    string type = llJsonGetValue(ev, ["type"]);
    if (type == JSON_INVALID)
        type = "";
    string damage = llJsonGetValue(ev, ["damage"]);
    if (damage == JSON_INVALID)
        damage = "";
    llOwnerSay("#slb:hit|" + what + "|" + llGetSubString(target, 0, 35) + "|"
        // A DEATH target is always an avatar, even once it has been sent home (the log
        // arrives up to a second late); a hit's target is asked.
        + (string)(what == "K" || llGetAgentSize((key)target) != ZERO_VECTOR) + "|"
        + llGetSubString(type, 0, 10) + "|" + llGetSubString(damage, 0, 15));
}

command(string msg)
{
    gLastCmd = llGetUnixTime();
    publicListens(TRUE);
    // The viewer is back after a quiet spell that dropped the combat listen: hear it again
    // (any command will do, so a lost combat|1 or hits|1 can't leave either feature dead).
    if (gCombatWanted || gHitsWanted)
        combatListen(TRUE);

    integer bar = llSubStringIndex(msg, "|");
    string cmd = msg;
    string arg = "";
    if (bar >= 0)
    {
        cmd = llGetSubString(msg, 0, bar - 1);
        arg = llGetSubString(msg, bar + 1, -1);
    }

    if (cmd == "say")
    {
        // Pose data, already encoded and size-capped by the viewer. Remember who we pose
        // (P1|seq|kind|part|target|...) so a detach can stop it for everyone.
        if (llGetSubString(arg, 0, 2) == "P1|")
        {
            list f = llParseStringKeepNulls(arg, ["|"], []);
            string target = llList2String(f, 4);
            if (target != "-" && target != "" && llListFindList(gPosing, [target]) < 0)
                gPosing = llList2List(gPosing + [target], -8, -1);
            if (llList2String(f, 2) == "X")
            {
                integer at = llListFindList(gPosing, [target]);
                if (at >= 0)
                    gPosing = llDeleteSubList(gPosing, at, at);
            }
        }
        llRegionSay(POSE_CHANNEL, arg);
    }
    else if (cmd == "scripts")
    {
        scriptInfo(arg);
    }
    else if (cmd == "ping")
    {
        // Keep-alive; nothing to say back.
    }
    else if (cmd == "hello")
    {
        hello();
    }
    else if (cmd == "ask")
    {
        key target = (key)arg;
        if (llGetAgentSize(target) == ZERO_VECTOR)
        {
            llOwnerSay("#slb:err|ask|" + arg);
            return;
        }
        // Announce the request (owner-authenticated) so every Slipstream bridge can tie
        // the target's "Accept <tag>" answer to us.
        llRegionSay(POSE_CHANNEL, "R1|" + arg);
        if (llListFindList(gAsked, [target]) < 0)
            gAsked = llList2List(gAsked + [target], -8, -1);
        llDialog(target, llGetDisplayName(gOwner) + " (" + llKey2Name(gOwner)
            + ") wants to pose your avatar.\n\nThe pose is only shown in Slipstream viewers, and you can stop it at any time.",
            ["Accept " + gTag, "Deny " + gTag], CONSENT_CHANNEL);
    }
    else if (cmd == "end")
    {
        // We stop posing someone: everyone drops our grant for them.
        llRegionSay(POSE_CHANNEL, "E1|" + arg);
        integer at = llListFindList(gPosing, [arg]);
        if (at >= 0)
            gPosing = llDeleteSubList(gPosing, at, at);
    }
    else if (cmd == "combat")
    {
        // Kill feed on (1) or off (0).
        gCombatWanted = (arg == "1");
        combatListen(gCombatWanted || gHitsWanted);
    }
    else if (cmd == "hits")
    {
        // Hit marker on (1) or off (0).
        gHitsWanted = (arg == "1");
        combatListen(gCombatWanted || gHitsWanted);
    }
    else if (cmd == "csay")
    {
        // Our owner's own consent statements (grant renewals, revocations), which other
        // bridges attribute to our owner by llGetOwnerKey.
        string kind = llGetSubString(arg, 0, 2);
        if (kind == "G1|" || kind == "S1|")
            llRegionSay(CONSENT_CHANNEL, arg);
    }
}

default
{
    state_entry()
    {
        hide();
        if (llGetAttached())
            start();
    }

    on_rez(integer param)
    {
        llResetScript();
    }

    timer()
    {
        if (!driven())
        {
            publicListens(FALSE);
            combatListen(FALSE);    // the busy combat log too, even in a quiet region
        }
    }

    attach(key id)
    {
        if (id != NULL_KEY)
            hide();
        if (id == NULL_KEY)
        {
            // Taken off: nobody should keep showing poses we were sending.
            llRegionSay(POSE_CHANNEL, "P1|0|X|1/1|-|");
            integer i;
            for (i = 0; i < llGetListLength(gPosing); ++i)
            {
                string target = llList2String(gPosing, i);
                llRegionSay(POSE_CHANNEL, "P1|0|X|1/1|" + target + "|");
                llRegionSay(POSE_CHANNEL, "E1|" + target);
            }
            gPosing = [];
        }
    }

    changed(integer change)
    {
        if (change & CHANGED_OWNER)
            llResetScript();
        if (change & (CHANGED_REGION | CHANGED_TELEPORT))
        {
            gAsked = [];
            gPosing = [];
        }
    }

    listen(integer channel, string name, key id, string msg)
    {
        if (channel == gCmdChannel)
        {
            command(msg);
            return;
        }
        if (channel == gHelloChannel)
        {
            hello();
            return;
        }
        if (!driven())
        {
            // The viewer is gone (closed, crashed, another viewer): stop hearing the busy
            // combat log rather than wait to be told.
            if (channel == COMBAT_CHANNEL)
                combatListen(FALSE);
            return;
        }

        if (channel == COMBAT_CHANNEL)
        {
            // Only deaths (kill feed) and events naming the owner (hit marker) can be
            // forwarded: skip everything else before any JSON work, so a busy fight can't
            // crowd the event queue.
            integer deaths = gCombatWanted && llSubStringIndex(msg, "\"DEATH\"") >= 0;
            integer mine = gHitsWanted && llSubStringIndex(msg, (string)gOwner) >= 0;
            if (!deaths && !mine)
                return;
            // The sim may batch several events into one JSON array; a killing blow's DAMAGE
            // comes before its DEATH.
            list events = [msg];
            if (llJsonValueType(msg, []) == JSON_ARRAY)
                events = llJson2List(msg);
            list blows = [];    // [target, damage] of this message's DAMAGE events, newest first
            integer n = llGetListLength(events);
            integer i;
            // Targets the owner kills in this message: their hits are part of the kill
            // (a one-shot kill plays the kill sound only).
            list killed = [];
            if (mine && llSubStringIndex(msg, "\"DEATH\"") >= 0)
                for (i = 0; i < n; ++i)
                {
                    string dev = llList2String(events, i);
                    if (llJsonGetValue(dev, ["event"]) == "DEATH")
                        if (llJsonGetValue(dev, ["owner"]) == (string)gOwner)
                            killed += [llJsonGetValue(dev, ["target"])];
                }
            for (i = 0; i < n; ++i)
            {
                string ev = llList2String(events, i);
                string kind = llJsonGetValue(ev, ["event"]);
                if (kind == "DAMAGE")
                {
                    if (deaths)
                        blows = [llJsonGetValue(ev, ["target"]), llJsonGetValue(ev, ["damage"])] + blows;
                    if (mine)
                    {
                        if (llListFindList(killed, [llJsonGetValue(ev, ["target"])]) >= 0)
                            combatHit(ev, "d");
                        else
                            combatHit(ev, "D");
                    }
                }
                else if (kind == "DEATH")
                {
                    if (deaths)
                    {
                        string damage = "";
                        integer at = llListFindList(blows, [llJsonGetValue(ev, ["target"])]);
                        if (at >= 0)
                            damage = llList2String(blows, at + 1);
                        if (damage == JSON_INVALID)
                            damage = "";
                        combatDeath(ev, damage);
                    }
                    if (mine)
                        combatHit(ev, "K");
                }
            }
            return;
        }

        if (channel == POSE_CHANNEL)
        {
            // P1 pose data, Q1 full-pose requests, R1 / E1 consent bookkeeping only.
            integer n = llStringLength(msg);
            string head = llGetSubString(msg, 0, 2);
            if (head == "P1|")
            {
                if (n > 970)
                    return;
            }
            else if (head == "Q1|")
            {
                if (msg != "Q1|*" && n != 39)
                    return;
            }
            else if (head != "R1|" && head != "E1|")
                return;
            else if (n != 39)
                return;
            key owner = llGetOwnerKey(id);
            if (owner == gOwner || !allow(owner))
                return;
            owner = attachedOwner(id);
            if (owner == NULL_KEY || owner == gOwner)
                return;
            llOwnerSay("#slb:pose|" + (string)owner + "|" + msg);
            return;
        }

        if (channel == CONFIRM_CHANNEL)
        {
            // A Slipstream target's own viewer re-stating (G1) or ending (S1) a grant, as
            // chat from the avatar itself: no attachment can say this for it.
            if (id == gOwner || !(keyLine(msg, "G1|") || keyLine(msg, "S1|")))
                return;
            if (llGetAgentSize(id) == ZERO_VECTOR || !allow(id))
                return;
            llOwnerSay("#slb:consent|" + (string)id + "|v|" + (string)id + "|" + msg);
            return;
        }

        if (channel == CONSENT_CHANNEL)
        {
            key who = NULL_KEY;
            string via = "";
            if (llGetAgentSize(id) != ZERO_VECTOR)
            {
                // A dialog answer, said by the avatar itself: only the three answers count.
                if (id != gOwner && (answerLine(msg, "Accept") || answerLine(msg, "Deny") || answerLine(msg, "Stop")))
                {
                    who = id;
                    via = "d";
                }
            }
            else if (keyLine(msg, "G1|") || keyLine(msg, "S1|"))
            {
                // An attachment speaking for its owner: an older Slipstream bridge, or any
                // other attachment. Forwarded with the object, so the viewer can tell (and,
                // for our own owner, report) one that is not a real bridge.
                who = attachedOwner(id);
                via = "a";
                if (who == gOwner && llGetSubString(msg, 0, 2) != "G1|")
                    who = NULL_KEY;
            }
            if (who == NULL_KEY)
                return;
            if (!allow(who))
                return;
            llOwnerSay("#slb:consent|" + (string)who + "|" + via + "|" + (string)id + "|" + msg);

            // Someone we asked said yes: give them a way to stop it from any viewer.
            integer at = llListFindList(gAsked, [who]);
            if (via == "d" && at >= 0 && msg == "Accept " + gTag)
            {
                gAsked = llDeleteSubList(gAsked, at, at);
                llDialog(who, llGetDisplayName(gOwner) + " is posing you. Press Stop at any time to end it.",
                    ["Stop " + gTag], CONSENT_CHANNEL);
            }
            else if (via == "d" && at >= 0 && msg == "Deny " + gTag)
            {
                gAsked = llDeleteSubList(gAsked, at, at);
            }
        }
    }
}
