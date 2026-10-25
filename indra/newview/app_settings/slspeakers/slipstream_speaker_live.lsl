// Slipstream stage speaker -- live control (#892, doc/parcel_speakers_protocol.md)
//
// Drop into the speaker prim. It keeps the speaker's invisible hover-text tag up to date and
// takes commands from the prim's owner on channel CH, for example during a DJ set:
//
//   /8920 vol 0.5        speaker level 0..2 (2 = +6 dB)
//   /8920 r 40           audible range 5..100 m
//   /8920 ch L           role: M (mono), L, R, S (sub)
//   /8920 grp stage      speaker group: 1-24 of a-z 0-9 _ . -
//   /8920 off | on       silence / restore this speaker (the tag stays)
//   /8920 status         say the current tag
//   /8920 clear          remove the tag and delete this script
//
// Viewers pick up a change with the next object update (about 0.1 to 1 s).
// The speaker plays the parcel's music stream when its owner is the parcel owner, or it is
// deeded to the parcel's group, or its group is set to the parcel's group.
//
// Full permissions; copy, change and give away freely.

integer CH  = 8920;         // owner command channel
string  grp = "stage";
string  ch  = "M";
float   vol = 1.0;
integer r   = 30;
integer on  = TRUE;

string GROUP_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_.-";

// 0.8 -> "0.80" (what the viewer reads back exactly).
string fmtVol(float v)
{
    integer c = llRound(v * 100.0);
    return (string)(c / 100) + "." + llGetSubString("0" + (string)(c % 100), -2, -1);
}

string tag()
{
    string t = "[slspk grp=" + grp + " ch=" + ch + " vol=" + fmtVol(vol) + " r=" + (string)r;
    if (!on) t += " off";
    return t + "]";
}

integer validGroup(string g)
{
    integer n = llStringLength(g);
    if (n < 1 || n > 24) return FALSE;
    integer i;
    for (i = 0; i < n; ++i)
    {
        if (llSubStringIndex(GROUP_CHARS, llGetSubString(g, i, i)) == -1) return FALSE;
    }
    return TRUE;
}

publish()
{
    llSetText(tag(), <1.0, 1.0, 1.0>, 0.1);   // faint: the sim drops near-invisible text; Slipstream hides the tag
}

default
{
    state_entry()
    {
        publish();
        llListen(CH, "", llGetOwner(), "");
        llOwnerSay("Speaker: " + tag() + " -- commands on /" + (string)CH + " (say /" + (string)CH + " help)");
    }

    on_rez(integer p)
    {
        llResetScript();
    }

    changed(integer c)
    {
        if (c & CHANGED_OWNER) llResetScript();
    }

    listen(integer c, string n, key id, string m)
    {
        list a = llParseString2List(llToLower(llStringTrim(m, STRING_TRIM)), [" "], []);
        string k = llList2String(a, 0);
        string v = llList2String(a, 1);
        if (k == "vol")
        {
            float f = (float)v;
            if (f < 0.0) f = 0.0;
            if (f > 2.0) f = 2.0;
            vol = f;
        }
        else if (k == "r")
        {
            integer i = (integer)v;
            if (i < 5) i = 5;
            if (i > 100) i = 100;
            r = i;
        }
        else if (k == "ch")
        {
            string u = llToUpper(v);
            if (llListFindList(["M", "L", "R", "S"], [u]) == -1)
            {
                llOwnerSay("ch must be M, L, R or S");
                return;
            }
            ch = u;
        }
        else if (k == "grp")
        {
            if (!validGroup(v))
            {
                llOwnerSay("grp: 1-24 characters of a-z 0-9 _ . -");
                return;
            }
            grp = v;
        }
        else if (k == "off") on = FALSE;
        else if (k == "on") on = TRUE;
        else if (k == "status")
        {
            llOwnerSay(tag());
            return;
        }
        else if (k == "clear")
        {
            llSetText("", ZERO_VECTOR, 0.0);
            llOwnerSay("Speaker tag removed; this script deletes itself.");
            llRemoveInventory(llGetScriptName());
            return;
        }
        else
        {
            llOwnerSay("Commands: vol 0..2, r 5..100, ch M|L|R|S, grp NAME, on, off, status, clear");
            return;
        }
        publish();
        llOwnerSay(tag());
    }
}
