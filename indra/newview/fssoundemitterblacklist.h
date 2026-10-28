/**
 * @file fssoundemitterblacklist.h
 * @brief Per-emitter sound blacklist (silence a specific noisy object).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, SkoomaStorm
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

#ifndef FS_SOUNDEMITTERBLACKLIST_H
#define FS_SOUNDEMITTERBLACKLIST_H

#include <map>
#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lldate.h"
#include "llsd.h"
#include "lluuid.h"

class LLViewerObject;

struct FSSoundEmitterBlacklistData
{
    // Owner of the emitter (for a worn attachment, the wearer). Stable across
    // re-rez, unlike the object UUID, so owner+name re-identifies the emitter.
    // Null on entries added before the name/owner were known.
    LLUUID      owner;
    std::string name;
    std::string region;
    LLDate      date;
    bool        permanent{ false };
    // GunMetal-DX: a worn attachment's root when it was added. Only these are
    // re-matched on owner+name; an in-world object keeps its UUID.
    bool        attachment{ false };

    LLSD toLLSD() const;
    static FSSoundEmitterBlacklistData fromLLSD(const LLSD& data);
};

using sound_emitter_blacklist_map_t = std::map<LLUUID, FSSoundEmitterBlacklistData>;

// Blocks sounds emitted by specific objects (the emitter), as opposed to the
// per-asset Sound Blacklist. Lets a user silence a noisy scripted attachment
// (e.g. a music gauntlet) without derendering the whole object. Two tiers:
// session-only (lost on logout) and permanent (persisted per account). Keyed on
// the emitter object's UUID; child-prim emitters are matched via the linkset
// root in the sound path (see isEmitterBlacklisted).
//
// A worn attachment is issued a new object UUID every time it is re-rezzed or
// detached and re-worn, so a permanent entry also records owner+name, which do
// survive. When an attachment arrives on an avatar that owns a permanent entry,
// LLVOAvatar::addChild probes it for properties and noteObjectProperties()
// re-points the entry at the new UUID. The probe is gated on wantsProperties()
// and throttled by GMDXAttachProbe, so a user with no permanent entries pays one
// empty-map test per attach.
//
// GunMetal-DX: attached sounds are gated in LLAudioSourceVO::updateMute (the
// same gate as the mute list and the Sound Explorer block), which covers sounds
// delivered by AttachedSound and by ObjectUpdate alike and stops a playing or
// queued clip on the next audio update, so the source is muted rather than torn
// down: removing an entry lets a looping sound resume. Triggered and preloaded
// sounds are gated in llviewermessage.cpp.
class FSSoundEmitterBlacklist : public LLSingleton<FSSoundEmitterBlacklist>
{
    LLSINGLETON_EMPTY_CTOR(FSSoundEmitterBlacklist);

public:
    // Loads the persisted permanent list. Call once after login.
    void init();

    bool isBlacklisted(const LLUUID& object_id) const;
    void addEmitter(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name, const std::string& region, bool permanent, bool attachment);
    void removeEmitters(const uuid_vec_t& ids);

    // Remember that we asked the sim for this object's properties so the reply can
    // fill in the owner/name an entry was added without (the avatar path knows the
    // wearer but not which attachment it tagged until the properties come back).
    void noteProbePending(const LLUUID& object_id);

    // True if this attachment of wearer_id is worth probing for properties: it is not
    // blacklisted, and a permanent attachment entry of theirs points at an object they
    // no longer wear (GMDXAttachProbe). Cheap: fails on an empty map.
    bool wantsProperties(const LLUUID& wearer_id, const LLUUID& object_id) const;

    // Fed by every ObjectProperties reply. If owner+name match a permanent
    // attachment entry whose object is no longer worn by that owner, and object_id
    // is the root of an attachment they wear, re-point the entry at object_id. The
    // mute gate silences it from the next audio update on.
    void noteObjectProperties(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name);

    // The sound gates. An object is blacklisted when it or its linkset root is.
    // The UUID form is for SoundTrigger, whose emitter may not be in gObjectList:
    // it checks the wire ParentID first and falls back to the object's root.
    // Both cost one empty-map test while nothing is blacklisted.
    bool isEmitterBlacklisted(const LLViewerObject* objectp) const;
    bool isEmitterBlacklisted(const LLUUID& object_id, const LLUUID& parent_id) const;

    // The right-click handlers (Object.BlacklistSoundEmitter[Permanent]): blacklist
    // the selected object, or every currently emitting attachment of a selected avatar.
    static void blacklistSelection(bool permanent);

    // Nothing blacklisted at all. Lets the sound path skip its object lookups
    // entirely for the common case, as FSAssetBlacklist::isBlacklisted does.
    bool isEmpty() const { return mData.empty(); }

    sound_emitter_blacklist_map_t getData() const { return mData; }

    using changed_signal_t = boost::signals2::signal<void()>;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

private:
    void load();
    void save();
    // Rebuilds mPermIndex from mData. Called after any change to the permanent
    // tier; the list is user-sized (a handful of entries), so O(n) is fine.
    void reindex();

    // owner -> object name -> the object UUIDs those entries currently point at
    // (one wearer can have several attachments with the same name). Only permanent
    // attachment entries that know both owner and name appear here.
    using perm_index_t = std::map<LLUUID, std::map<std::string, std::vector<LLUUID>>>;

    std::string                   mFileName;
    sound_emitter_blacklist_map_t mData;
    perm_index_t                  mPermIndex;
    std::map<LLUUID, F64>         mPendingProbe;   // object -> when we asked (dropped after a minute)
    changed_signal_t              mChangedSignal;
};

#endif // FS_SOUNDEMITTERBLACKLIST_H
