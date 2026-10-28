/**
 * @file fssoundemitterblacklist.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fssoundemitterblacklist.h"

#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llsdserialize.h"

#include "llselectmgr.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"

LLSD FSSoundEmitterBlacklistData::toLLSD() const
{
    LLSD sd;
    sd["owner"]     = owner;
    sd["name"]      = name;
    sd["region"]    = region;
    sd["date"]      = date;
    sd["permanent"] = permanent;
    sd["attachment"] = attachment;
    return sd;
}

FSSoundEmitterBlacklistData FSSoundEmitterBlacklistData::fromLLSD(const LLSD& sd)
{
    FSSoundEmitterBlacklistData d;
    d.owner     = sd["owner"].asUUID();
    d.name      = sd["name"].asString();
    d.region    = sd["region"].asString();
    d.date      = sd["date"].asDate();
    d.permanent = sd["permanent"].asBoolean();
    d.attachment = sd["attachment"].asBoolean();
    return d;
}

void FSSoundEmitterBlacklist::init()
{
    mFileName = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "sound_emitter_blacklist.xml");
    load();
}

bool FSSoundEmitterBlacklist::isBlacklisted(const LLUUID& object_id) const
{
    if (mData.empty() || object_id.isNull())
    {
        return false;
    }
    return mData.find(object_id) != mData.end();
}

void FSSoundEmitterBlacklist::addEmitter(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name, const std::string& region, bool permanent, bool attachment)
{
    if (object_id.isNull())
    {
        return;
    }

    FSSoundEmitterBlacklistData entry;
    entry.owner     = owner_id;
    entry.name      = name;
    entry.region    = region;
    entry.date      = LLDate::now();
    entry.permanent = permanent;
    entry.attachment = attachment;

    auto it = mData.find(object_id);
    if (it != mData.end())
    {
        // Keep the original add time; only ever upgrade session -> permanent.
        entry.date      = it->second.date;
        entry.permanent = permanent || it->second.permanent;
        if (entry.owner.isNull()) entry.owner  = it->second.owner;
        if (entry.name.empty())   entry.name   = it->second.name;
        if (entry.region.empty()) entry.region = it->second.region;
    }

    mData[object_id] = entry;

    if (entry.permanent)
    {
        reindex();
        save();
    }
    mChangedSignal();
}

void FSSoundEmitterBlacklist::noteProbePending(const LLUUID& object_id)
{
    if (object_id.isNull())
    {
        return;
    }
    // A reply that never came would leave its row behind for good: forget old asks.
    constexpr F64 PENDING_PROBE_TIMEOUT = 60.0;
    const F64 now = LLFrameTimer::getTotalSeconds();
    for (auto it = mPendingProbe.begin(); it != mPendingProbe.end(); )
    {
        it = (now - it->second > PENDING_PROBE_TIMEOUT) ? mPendingProbe.erase(it) : std::next(it);
    }
    mPendingProbe[object_id] = now;
}

// An attachment worn by owner_id right now (root_only: and it is the attachment's root).
static bool is_worn_by(LLViewerObject* objectp, const LLUUID& owner_id, bool root_only)
{
    if (!objectp || objectp->isDead() || !objectp->isAttachment() || (root_only && objectp->getRootEdit() != objectp))
    {
        return false;
    }
    const LLVOAvatar* avatarp = objectp->getAvatarAncestor();
    return avatarp && avatarp->getID() == owner_id;
}

bool FSSoundEmitterBlacklist::wantsProperties(const LLUUID& wearer_id, const LLUUID& object_id) const
{
    if (mPermIndex.empty() || wearer_id.isNull())
    {
        return false;
    }
    auto owner_it = mPermIndex.find(wearer_id);
    if (owner_it == mPermIndex.end() || isBlacklisted(object_id))
    {
        return false;
    }
    for (const auto& name_ids : owner_it->second)
    {
        for (const LLUUID& id : name_ids.second)
        {
            if (!is_worn_by(gObjectList.findObject(id), wearer_id, false))
            {
                return true; // not matched yet this session
            }
        }
    }
    return false;
}

void FSSoundEmitterBlacklist::noteObjectProperties(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name)
{
    if (object_id.isNull())
    {
        return;
    }

    // A reply to a probe we fired when the entry was added: record what the entry
    // will be re-identified by from now on. Nothing to silence: the gate already has it.
    // The ask is answered even when the object has no name.
    auto pending_it = mPendingProbe.find(object_id);
    if (pending_it != mPendingProbe.end())
    {
        mPendingProbe.erase(pending_it);
        auto data_it = mData.find(object_id);
        if (data_it != mData.end() && (owner_id.notNull() || !name.empty()))
        {
            if (owner_id.notNull()) data_it->second.owner = owner_id;
            if (!name.empty())      data_it->second.name  = name;
            if (data_it->second.permanent)
            {
                reindex();
                save();
            }
            mChangedSignal();
        }
        return;
    }

    if (mPermIndex.empty() || owner_id.isNull() || name.empty())
    {
        return;
    }

    auto owner_it = mPermIndex.find(owner_id);
    if (owner_it == mPermIndex.end())
    {
        return;
    }
    auto name_it = owner_it->second.find(name);
    if (name_it == owner_it->second.end())
    {
        return;
    }

    if (mData.find(object_id) != mData.end())
    {
        return; // already blacklisted; the sound path has it
    }

    // GunMetal-DX: only the root of an attachment this owner wears takes an entry
    // over. Any ObjectProperties reply lands here (the user selecting things, area
    // search), and without this an in-world object, or a child prim, that happens
    // to share the owner and name would pull the entry off the object it was made for.
    if (!is_worn_by(gObjectList.findObject(object_id), owner_id, true))
    {
        return;
    }

    for (LLUUID& old_id : name_it->second)
    {
        // That emitter is still worn: a second attachment with the same name is
        // another object, not this one come back.
        if (is_worn_by(gObjectList.findObject(old_id), owner_id, false))
        {
            continue;
        }
        auto data_it = mData.find(old_id);
        if (data_it == mData.end())
        {
            continue; // index is stale; reindex() will drop it
        }

        // Same emitter, new UUID: move the entry over so the hot path keeps working
        // as a plain UUID lookup, and remember the new UUID for the next session.
        FSSoundEmitterBlacklistData entry = data_it->second;
        mData.erase(data_it);
        mData[object_id] = entry;
        old_id = object_id;

        save();
        mChangedSignal();
        return;
    }
}

bool FSSoundEmitterBlacklist::isEmitterBlacklisted(const LLViewerObject* objectp) const
{
    if (mData.empty() || !objectp)
    {
        return false;
    }
    if (isBlacklisted(objectp->getID()))
    {
        return true;
    }
    // A child prim emitting: match the linkset root (an attachment's root edit is
    // the attachment root, never the avatar).
    const LLViewerObject* rootp = objectp->getRootEdit();
    return rootp && rootp != objectp && isBlacklisted(rootp->getID());
}

bool FSSoundEmitterBlacklist::isEmitterBlacklisted(const LLUUID& object_id, const LLUUID& parent_id) const
{
    // Every incoming sound reaches here, so do nothing at all when nobody has
    // blacklisted an emitter: the findObject below is a lookup per sound that
    // only the root fallback needs.
    if (mData.empty())
    {
        return false;
    }
    // SoundTrigger carries the root on the wire (ParentID) and plays even when the
    // emitter is not in gObjectList, so prefer that over the findObject fallback.
    if (isBlacklisted(object_id) || isBlacklisted(parent_id))
    {
        return true;
    }
    return isEmitterBlacklisted(gObjectList.findObject(object_id));
}

// The right-click handlers. SkoomaStorm had these in llviewermenu.cpp; they live
// here in GunMetal-DX so the menu file only registers them.
namespace
{
    // True if this object or any of its child prims currently has an active sound.
    bool object_or_child_emits_sound(LLViewerObject* o)
    {
        if (!o)
        {
            return false;
        }
        if (o->isAudioSource())
        {
            return true;
        }
        for (LLViewerObject* child : o->getChildren())
        {
            if (child && child->isAudioSource())
            {
                return true;
            }
        }
        return false;
    }

    // Add one emitter object to the blacklist. The mute gate silences it on the next
    // audio update, current and queued clips alike.
    // owner_id is what makes a permanent entry outlive the object UUID, which a worn
    // attachment is reissued on every re-rez. An empty name means we do not know what
    // the object is called yet, so ask: the reply completes the entry (see
    // FSSoundEmitterBlacklist::noteObjectProperties).
    void blacklist_one_emitter(LLViewerObject* o, const LLUUID& owner_id, const std::string& name, bool permanent)
    {
        if (!o)
        {
            return;
        }
        std::string region_name;
        if (LLViewerRegion* region = o->getRegion())
        {
            region_name = region->getName();
        }

        FSSoundEmitterBlacklist& blacklist = FSSoundEmitterBlacklist::instance();
        blacklist.addEmitter(o->getID(), owner_id, name, region_name, permanent, o->isAttachment());

        if (name.empty())
        {
            blacklist.noteProbePending(o->getID());
            // A selected object gets its properties with the selection, and the probe's
            // deselect would drop it from the user's selection on the sim: wait for those.
            if (!LLSelectMgr::getInstance()->getSelection()->findNode(o))
            {
                LLSelectMgr::getInstance()->requestObjectPropertiesViaSelect(o);
            }
        }
    }
}

// static
void FSSoundEmitterBlacklist::blacklistSelection(bool permanent)
{
    LLSelectMgr* select_mgr = LLSelectMgr::getInstance();
    LLViewerObject* objp = select_mgr->getSelection()->getFirstRootObject(true);
    if (!objp)
    {
        return;
    }

    if (LLVOAvatar* av = objp->asAvatar())
    {
        // Right-clicked an avatar (e.g. a teammate's worn emitter): blacklist whichever
        // of their attachments is currently emitting sound. The wearer owns the
        // attachment, so recording their UUID plus the object's name (fetched by the
        // probe below) survives the re-rez that changes the object's own UUID.
        std::string av_name = av->getFullname();

        S32 tagged = 0;
        for (const auto& ap : av->mAttachmentPoints)
        {
            LLViewerJointAttachment* attach = ap.second;
            if (!attach)
            {
                continue;
            }
            for (LLViewerObject* attached : attach->mAttachedObjects)
            {
                if (object_or_child_emits_sound(attached))
                {
                    blacklist_one_emitter(attached, av->getID(), std::string(), permanent);
                    ++tagged;
                }
            }
        }
        LL_INFOS() << "Sound emitter blacklist: tagged " << tagged << " emitting attachment(s) on "
                   << (av_name.empty() ? objp->getID().asString() : av_name) << LL_ENDL;
        return;
    }

    std::string entry_name;
    LLUUID      owner_id;
    if (LLSelectNode* nodep = select_mgr->getSelection()->getFirstRootNode())
    {
        entry_name = nodep->mName;
        if (nodep->mValid && nodep->mPermissions) // filled in once properties arrived
        {
            owner_id = nodep->mPermissions->getOwner();
        }
    }
    blacklist_one_emitter(objp, owner_id, entry_name, permanent);
}

void FSSoundEmitterBlacklist::removeEmitters(const uuid_vec_t& ids)
{
    bool removed_permanent = false;
    bool removed_any = false;
    for (const LLUUID& id : ids)
    {
        auto it = mData.find(id);
        if (it == mData.end())
        {
            continue;
        }
        removed_permanent |= it->second.permanent;
        mData.erase(it);
        removed_any = true;
    }
    if (removed_permanent)
    {
        reindex();
        save();
    }
    if (removed_any)
    {
        mChangedSignal();
    }
}

void FSSoundEmitterBlacklist::load()
{
    // init() runs per account, so start from empty rather than merging into
    // whatever the previous session left behind. This has to happen before the
    // early returns below: a missing or unreadable file is exactly the case that
    // would otherwise hand the next account the previous one's entries whole.
    mData.clear();
    mPermIndex.clear();
    mPendingProbe.clear();

    llifstream file(mFileName.c_str());
    if (!file.is_open())
    {
        return;
    }

    LLSD data;
    if (LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE)
    {
        file.close();
        LL_WARNS() << "Failed to parse sound emitter blacklist file." << LL_ENDL;
        return;
    }
    file.close();

    if (!data.isMap())
    {
        return;
    }

    for (LLSD::map_const_iterator it = data.beginMap(); it != data.endMap(); ++it)
    {
        LLUUID id(it->first);
        if (id.isNull())
        {
            continue;
        }
        FSSoundEmitterBlacklistData entry = FSSoundEmitterBlacklistData::fromLLSD(it->second);
        entry.permanent = true; // only permanent entries are ever persisted
        mData[id] = entry;
    }

    reindex();
}

// Entries written before owner/name were recorded have no index row, so they
// keep the old behaviour: matched by object UUID only, which still holds for
// in-world objects (their UUID is stable) but lapses when an attachment re-rezzes.
void FSSoundEmitterBlacklist::reindex()
{
    mPermIndex.clear();
    for (const auto& entry : mData)
    {
        const FSSoundEmitterBlacklistData& d = entry.second;
        if (d.permanent && d.attachment && d.owner.notNull() && !d.name.empty())
        {
            mPermIndex[d.owner][d.name].push_back(entry.first);
        }
    }
}

void FSSoundEmitterBlacklist::save()
{
    LLSD data;
    for (const auto& entry : mData)
    {
        if (entry.second.permanent)
        {
            data[entry.first.asString()] = entry.second.toLLSD();
        }
    }

    llofstream file(mFileName.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Unable to open sound emitter blacklist file for writing." << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
    file.close();
}
