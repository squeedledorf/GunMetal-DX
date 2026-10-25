/**
 * @file slparcelspeakers.h
 * @brief #892: the registry of in-world speakers that play the agent's parcel stream.
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
 * $/LicenseInfo$
 *
 * doc/parcel_speakers_protocol.md has the full convention. In short:
 *
 * DISCOVERY. A speaker is any prim whose hover text holds a tag, set with alpha 0 so nobody sees
 * it (llSetText("[slspk grp=stage ch=L vol=0.8 r=40]", <1,1,1>, 0.0)). Hover text reaches every
 * viewer in range in ObjectUpdate, survives the setup script, and comes back from the object
 * cache, so nothing is scanned: LLViewerObject calls onHoverText() when a text changes.
 *
 * TRUST. Each tagged LINKSET costs one RequestObjectPropertiesFamily (rate-limited, sent for its
 * root prim -- as LL only ever asks for roots -- and shared by every tagged prim in it) for its owner
 * and group; the reply is tapped in LLSelectMgr::processObjectPropertiesFamily. A speaker counts
 * when its owner is the parcel owner, or it is deeded to the parcel's group, or its group is set
 * to the parcel's group (deeded or not). It must also stand on the agent's parcel or on another
 * parcel of the same owner in the same region (one ParcelPropertiesRequest per 4 m cell, with
 * our own sequence ids, tapped in LLViewerParcelMgr::processParcelProperties). Attachments never
 * count. A neighbour can copy the tag text but not the owner or group: those come from the sim.
 *
 * PARCEL POLICY. The parcel description may hold [slspk outside=silent|ambient]: what listeners
 * out of range of every speaker hear (phase B).
 *
 * STATE. Per-object entries are capped (kMaxCandidates), freed as soon as the object dies or loses
 * its tag, and reported as live / cap / peak. Everything is cleared when ParcelAudioSpeakers is off.
 *
 * OUTPUT (phase B): speakers() is the current validated set with fresh positions, generation()
 * changes whenever its membership or any tag changes. Main thread only.
 */

#ifndef SL_PARCELSPEAKERS_H
#define SL_PARCELSPEAKERS_H

#include "lluuid.h"
#include "llquaternion.h"
#include "v3dmath.h"

#include <string>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SLParcelSpeakers
{
public:
    enum ERole : U8
    {
        ROLE_MONO = 0,  // ch=M (default): (L+R)/2
        ROLE_LEFT,      // ch=L
        ROLE_RIGHT,     // ch=R
        ROLE_SUB,       // ch=S: low-passed mono
    };

    enum EOutside : U8
    {
        OUTSIDE_SILENT = 0,     // default: out of range of every speaker = silence
        OUTSIDE_AMBIENT,        // a quiet listener-relative stereo bed instead
    };

    // vol= ceiling: 2 = +6 dB over the stream level (an amplified stage speaker).
    static constexpr F32 kMaxVol = 2.f;

    // One parsed hover-text tag. Values are already validated and clamped.
    struct Tag
    {
        std::string group = "main";     // grp=: 1..24 of [a-z0-9_.-], lower case
        ERole role = ROLE_MONO;
        F32  vol = 1.f;                 // vol=: 0..kMaxVol (2), times the user's music volume
        F32  range = 30.f;              // r=: audible range in metres, 5..100
        bool hasDir = false;            // dir= given and < 360
        F32  dirDeg = 360.f;            // dir=: full cone angle in degrees around the prim's +X, 1..360
        bool hasT0 = false;
        S64  t0 = 0;                    // t0=: clip start, UTC unix seconds (non-live URLs)
        bool off = false;               // bare "off": tag kept, speaker silent

        bool operator==(const Tag& o) const
        {
            return group == o.group && role == o.role && vol == o.vol && range == o.range && hasDir == o.hasDir
                && dirDeg == o.dirDeg && hasT0 == o.hasT0 && t0 == o.t0 && off == o.off;
        }
        bool operator!=(const Tag& o) const { return !(*this == o); }
    };

    // A validated speaker, as phase B consumes it.
    struct Speaker
    {
        LLUUID       id;
        LLVector3d   posGlobal;
        LLQuaternion rotation;          // world rotation (cone axis = +X rotated by this)
        Tag          tag;               // never off
    };

    struct Stats
    {
        U32 live = 0;           // tagged objects tracked now
        U32 cap = 0;            // kMaxCandidates
        U32 peak = 0;           // highest `live` this session
        U32 verified = 0;       // in speakers() now
        U32 pending = 0;        // properties requests in flight
        U32 untrusted = 0;      // known owner / group, rule not met
        U32 offParcel = 0;      // trusted, but not on this parcel (or one of the same owner)
        // The rest of the tagged ones, by reason (every tagged entry is in exactly one bucket:
        // verified + pending + untrusted + offParcel + these = live).
        U32 notHere = 0;        // in another region than the agent
        U32 noIdentity = 0;     // owner / group not answered (no request possible now, or backing off)
        U32 placing = 0;        // trusted; its parcel lookup has not answered yet
        U32 off = 0;            // tag says off
        U32 culled = 0;         // object not in the scene right now (object-cache culling): last position kept
        U32 tagLost = 0;        // its tag just disappeared: kept a few seconds in case an update brings it back
        U32 overflow = 0;       // tagged objects ignored because the table was full (session)
        U32 requests = 0;       // properties requests sent (session)
        U32 parcelLookups = 0;  // parcel-by-position requests sent (session)
        U32 cells = 0;          // cached parcel-by-position answers now
    };

    static constexpr U32 kMaxCandidates = 256;

    // Main thread, once per frame (idle). Heavy work runs at ~4 Hz; positions every call.
    static void update();

    // True while the registry listens (ParcelAudioSpeakers on). LLViewerObject checks it first,
    // so the hook costs one load when the feature is off.
    static bool sWatching;
    // LLViewerObject: `obj`'s hover text was set, changed or cleared.
    static void onHoverText(LLViewerObject* obj);

    // LLSelectMgr::processObjectPropertiesFamily. true: the registry had asked for this object.
    static bool onPropertiesFamily(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& group_id);

    // LLViewerParcelMgr::processParcelProperties: our parcel-by-position sequence ids.
    static bool isSpeakerParcelSeq(S32 sequence_id);
    static void onParcelProperties(S32 sequence_id, const LLViewerRegion* region, S32 local_id,
                                   const LLUUID& owner_id, const LLUUID& group_id, bool group_owned);

    // The hover-text tag (first "[slspk" in any case, followed by a space or ']', up to the next ']').
    // false: no tag. `errors`: tokens not understood (the rest still applies).
    static bool parseTag(const std::string& text, Tag& out, std::string* errors = nullptr);
    // The parcel description's policy tag. false: no tag (the default policy holds).
    static bool parsePolicy(const std::string& desc, EOutside& out, std::string* errors = nullptr);
    // A cheap test for a tag start (case-insensitive), no allocation.
    static bool hasTagPrefix(const std::string& text);
    // Hover text as drawn: the [slspk ...] tag removed (the sim drops near-invisible text, so tags
    // are sent visibly; Slipstream hides them). Unchanged when there is no tag.
    static std::string displayText(const std::string& text);

    // Output for phase B.
    static const std::vector<Speaker>& speakers();
    static U32 generation();
    static EOutside outsidePolicy();
    static const Stats& stats();
};

#endif
