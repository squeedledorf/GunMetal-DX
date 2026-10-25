// Slipstream stage speaker -- one-shot setup (#892, doc/parcel_speakers_protocol.md)
//
// Edit TAG, then drop this script into the speaker prim. It writes the tag as faint hover
// text (Slipstream does not draw it) and deletes itself: no script keeps running. The hover text stays on the prim (also
// after a take / re-rez) and every Slipstream viewer in range reads it.
//
//   grp=<name>   speaker group (default main): 1-24 of a-z 0-9 _ . -
//   ch=M|L|R|S   mono (default), left, right, sub (low-passed mono)
//   vol=0..2     speaker level (default 1.0; 2 = +6 dB), times each listener's music volume
//   r=5..100     audible range in metres (default 30)
//   dir=<deg>    optional cone (full angle) around the prim's +X axis; omit = all round
//   t0=<unix>    optional clip start (UTC seconds) for non-live stream URLs
//   off          keep the tag but silence this speaker
//
// The speaker plays the parcel's music stream when its owner is the parcel owner, or it is
// deeded to the parcel's group, or its group is set to the parcel's group.
//
// To remove the tag later: put a script with llSetText("", ZERO_VECTOR, 0.0); in the prim, or
// use slipstream_speaker_live.lsl and say "clear".
//
// Full permissions; copy, change and give away freely.

string TAG = "[slspk grp=stage ch=L vol=0.8 r=40]";

default
{
    state_entry()
    {
        // The sim does not send near-invisible hover text to viewers (alpha 0.01 is dropped),
        // so the tag is set faintly visible: Slipstream hides it, other viewers show it faintly.
        llSetText(TAG, <1.0, 1.0, 1.0>, 0.1);
        llOwnerSay("Speaker tag set: " + TAG + " -- this setup script now removes itself.");
        llRemoveInventory(llGetScriptName());
    }
}
