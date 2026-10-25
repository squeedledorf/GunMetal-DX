/**
 * @file slaoobjectimport.cpp
 * @brief Client-side animation overrider (#496): take animations out of an
 *        object's contents into an AO set (copyable items only).
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

#include "llviewerprecompiledheaders.h"

#include "slaoobjectimport.h"

#include <algorithm>
#include <sstream>

#include "llagent.h"
#include "llassetstorage.h"
#include "llcallbacklist.h"
#include "llfilesystem.h"
#include "llinventory.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "slaosets.h"

using namespace SLAO;

namespace
{
    constexpr F32 PUMP_SECS = 0.25f;
    constexpr size_t SENDS_PER_PUMP = 6;        // MoveTaskInventory messages per pump
    constexpr F64 NOTECARD_TIMEOUT_SECS = 10.0;
    constexpr F64 FOLDER_TIMEOUT_SECS = 30.0;
    constexpr F64 STALL_TIMEOUT_SECS = 20.0;    // no new copy for this long: give up on the rest
    constexpr size_t MAX_JOBS = 32;
    constexpr size_t MAX_ITEMS = 512;
    constexpr size_t MAX_NOTECARDS = 6;

    F64 now()
    {
        return LLTimer::getElapsedSeconds();
    }

    std::string lower(const std::string& in)
    {
        std::string out = in;
        LLStringUtil::toLower(out);
        LLStringUtil::trim(out);
        return out;
    }

    const LLInventoryItem* as_item(const LLPointer<LLInventoryObject>& obj)
    {
        return dynamic_cast<const LLInventoryItem*>(obj.get());
    }

    bool likely_ao_notecard(const std::string& name)
    {
        const std::string l = lower(name);
        return l.find("ao") != std::string::npos || l.find("zhao") != std::string::npos ||
               l.find("config") != std::string::npos || l.find("set") == 0 ||
               l.find("default") != std::string::npos || l.find("anim") != std::string::npos;
    }

    void collect_items(const LLUUID& folder_id, LLInventoryModel::item_array_t& out)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, cats, items);
        if (items)
        {
            out.insert(out.end(), items->begin(), items->end());
        }
    }
}

SLAOObjectImport::SLAOObjectImport()
{
}

SLAOObjectImport::~SLAOObjectImport()
{
}

// static
bool SLAOObjectImport::canCopy(const LLInventoryItem* item)
{
    return item && item->getPermissions().allowCopyBy(gAgent.getID(), gAgent.getGroupID());
}

std::string SLAOObjectImport::getStatusText() const
{
    if (mJobs.empty())
    {
        return std::string();
    }
    // Report the oldest job; mention the others
    const Job& job = mJobs.begin()->second;
    LLStringUtil::format_map_t args;
    args["[NAME]"] = job.mObjectName;
    size_t done = 0;
    size_t total = 0;
    for (const Item& item : job.mItems)
    {
        if (!item.mIsNotecard)
        {
            ++total;
            done += item.mCopyID.notNull() ? 1 : 0;
        }
    }
    args["[DONE]"] = llformat("%d", (S32)done);
    args["[TOTAL]"] = llformat("%d", (S32)total);
    args["[MORE]"] = (mJobs.size() > 1) ? llformat(" (+%d)", (S32)mJobs.size() - 1) : std::string();
    switch (job.mPhase)
    {
    case PHASE_READING:
        return LLTrans::getString("SLAOObjectReading", args);
    case PHASE_FOLDERS:
        return LLTrans::getString("SLAOObjectPreparing", args);
    case PHASE_COPYING:
    default:
        return LLTrans::getString("SLAOObjectCopying", args);
    }
}

//-----------------------------------------------------------------------------
// Whole object import
//-----------------------------------------------------------------------------

bool SLAOObjectImport::importObject(const LLUUID& object_id, const std::string& set_name,
                                    const LLUUID& preferred_notecard)
{
    LLViewerObject* object = gObjectList.findObject(object_id);
    LLSD args;
    args["NAME"] = set_name;
    if (!object || object->isDead() || !object->getRegion())
    {
        LLNotificationsUtil::add("SLAOObjectGone", args);
        return false;
    }
    if (mJobs.size() >= MAX_JOBS)
    {
        LLNotificationsUtil::add("SLAOImportBusy", args);
        return false;
    }

    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);

    LLUUID job_id;
    job_id.generate();
    Job job;
    job.mObjectID = object_id;
    job.mObjectName = set_name;
    job.mSetName = set_name;
    job.mNewSet = true;
    job.mStarted = now();
    job.mLastActivity = job.mStarted;

    std::vector<const LLInventoryItem*> notecards;
    for (const LLPointer<LLInventoryObject>& obj : contents)
    {
        const LLInventoryItem* item = as_item(obj);
        if (!item)
        {
            continue;
        }
        if (item->getType() == LLAssetType::AT_ANIMATION)
        {
            if (!canCopy(item))
            {
                ++job.mNoCopy; // stays in the object
                continue;
            }
            if (job.mItems.size() < MAX_ITEMS)
            {
                Item entry;
                entry.mTaskItemID = item->getUUID();
                entry.mName = item->getName();
                job.mItems.push_back(entry);
            }
        }
        else if (item->getType() == LLAssetType::AT_NOTECARD)
        {
            notecards.push_back(item);
        }
    }

    if (job.mItems.empty())
    {
        args["NOCOPY"] = job.mNoCopy;
        LLNotificationsUtil::add(job.mNoCopy ? "SLAOObjectAllNoCopy" : "SLAOObjectNoAnims", args);
        return false;
    }

    // Read the likely AO notecards straight from the object (no copy needed
    // to read them); the best match maps animations to states.
    std::stable_sort(notecards.begin(), notecards.end(), [&preferred_notecard](const LLInventoryItem* a, const LLInventoryItem* b)
        {
            const bool pa = a->getUUID() == preferred_notecard;
            const bool pb = b->getUUID() == preferred_notecard;
            if (pa != pb)
            {
                return pa;
            }
            return likely_ao_notecard(a->getName()) && !likely_ao_notecard(b->getName());
        });
    if (notecards.size() > MAX_NOTECARDS)
    {
        notecards.resize(MAX_NOTECARDS);
    }

    mJobs[job_id] = job;
    Job& stored = mJobs[job_id];
    for (const LLInventoryItem* notecard : notecards)
    {
        if (notecard->getAssetUUID().isNull() || !gAssetStorage)
        {
            continue;
        }
        ++stored.mPendingNotecards;
        const std::string name = notecard->getName();
        LLGetAssetCallback callback = [job_id, name](const LLUUID& asset_id, LLAssetType::EType type,
                                                     void*, S32 status, LLExtStat)
        {
            std::string text;
            if (status == 0)
            {
                LLFileSystem file(asset_id, type, LLFileSystem::READ);
                const S32 size = file.getSize();
                if (size > 0)
                {
                    std::string buffer(size, '\0');
                    file.read((U8*)&buffer[0], size);
                    if (buffer.compare(0, 19, "Linden text version") == 0)
                    {
                        LLNotecard parsed(LLNotecard::MAX_SIZE);
                        std::istringstream stream(buffer);
                        if (parsed.importStream(stream))
                        {
                            text = parsed.getText();
                        }
                    }
                    else
                    {
                        text = buffer;
                    }
                }
            }
            if (SLAOObjectImport::instanceExists())
            {
                SLAOObjectImport::instance().onNotecardText(job_id, name, text);
            }
        };
        gAssetStorage->getInvItemAsset(object->getRegion()->getHost(),
                                       gAgent.getID(),
                                       gAgent.getSessionID(),
                                       notecard->getPermissions().getOwner(),
                                       object_id,
                                       notecard->getUUID(),
                                       notecard->getAssetUUID(),
                                       LLAssetType::AT_NOTECARD,
                                       callback,
                                       nullptr,
                                       true);
        // Keep a copy of a copyable notecard with the set, for reference
        if (canCopy(notecard) && stored.mItems.size() < MAX_ITEMS)
        {
            Item entry;
            entry.mTaskItemID = notecard->getUUID();
            entry.mName = notecard->getName();
            entry.mIsNotecard = true;
            stored.mItems.push_back(entry);
        }
    }

    LL_INFOS() << "AO: importing " << stored.mItems.size() << " items from '" << set_name << "' ("
               << stored.mNoCopy << " no-copy animations stay)" << LL_ENDL;
    startPump(job_id);
    notifyChanged();
    return true;
}

void SLAOObjectImport::onNotecardText(const LLUUID& job_id, const std::string& name, const std::string& text)
{
    auto it = mJobs.find(job_id);
    if (it == mJobs.end())
    {
        return;
    }
    Job& job = it->second;
    if (!text.empty())
    {
        job.mNotecardTexts.emplace_back(name, text);
    }
    if (job.mPendingNotecards > 0)
    {
        --job.mPendingNotecards;
    }
}

void SLAOObjectImport::prepare(Job& job)
{
    // Pick the notecard that names the most of the object's animations
    std::map<std::string, size_t> by_name; // lower-case name -> item index
    for (size_t i = 0; i < job.mItems.size(); ++i)
    {
        if (!job.mItems[i].mIsNotecard)
        {
            by_name.emplace(lower(job.mItems[i].mName), i);
        }
    }

    S32 best_score = 0;
    SLAONotecard best;
    for (const auto& [name, text] : job.mNotecardTexts)
    {
        SLAONotecard notecard;
        if (!notecard.parse(text))
        {
            continue;
        }
        S32 score = 0;
        for (const auto& names : notecard.mNames)
        {
            for (const std::string& anim : names)
            {
                score += by_name.count(lower(anim)) ? 1 : 0;
            }
        }
        if (score > best_score)
        {
            best_score = score;
            best = notecard;
            job.mNotecardUsed = name;
        }
    }

    if (best_score > 0)
    {
        for (S32 state = 0; state < STATE_COUNT; ++state)
        {
            for (const std::string& anim : best.mNames[state])
            {
                auto found = by_name.find(lower(anim));
                if (found != by_name.end())
                {
                    std::vector<EState>& states = job.mItems[found->second].mStates;
                    if (std::find(states.begin(), states.end(), (EState)state) == states.end())
                    {
                        states.push_back((EState)state);
                    }
                }
            }
        }
    }

    // Names for whatever the notecard did not place
    for (Item& item : job.mItems)
    {
        if (!item.mIsNotecard && item.mStates.empty())
        {
            const EState guess = guessStateFromAnimName(item.mName);
            if (guess != STATE_NONE)
            {
                item.mStates.push_back(guess);
                ++job.mGuessed;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Single animation from an object's contents
//-----------------------------------------------------------------------------

bool SLAOObjectImport::copyToState(const LLUUID& object_id, const LLUUID& task_item_id,
                                   const LLUUID& set_id, EState state)
{
    LLViewerObject* object = gObjectList.findObject(object_id);
    if (!object || object->isDead() || set_id.isNull() || state <= STATE_NONE || state >= STATE_COUNT ||
        mJobs.size() >= MAX_JOBS)
    {
        return false;
    }
    const LLInventoryItem* item = dynamic_cast<const LLInventoryItem*>(object->getInventoryObject(task_item_id));
    if (!item || item->getType() != LLAssetType::AT_ANIMATION || !canCopy(item))
    {
        return false;
    }

    // Several items dropped together from one object share a job
    for (auto& [id, job] : mJobs)
    {
        if (!job.mNewSet && job.mObjectID == object_id && job.mSetID == set_id &&
            (job.mPhase == PHASE_FOLDERS || job.mPhase == PHASE_COPYING))
        {
            const bool present = std::any_of(job.mItems.begin(), job.mItems.end(),
                [&task_item_id](const Item& entry) { return entry.mTaskItemID == task_item_id; });
            if (!present && job.mItems.size() < MAX_ITEMS)
            {
                Item entry;
                entry.mTaskItemID = task_item_id;
                entry.mName = item->getName();
                entry.mStates.push_back(state);
                job.mItems.push_back(entry);
                notifyChanged();
            }
            return true;
        }
    }

    LLUUID job_id;
    job_id.generate();
    Job job;
    job.mObjectID = object_id;
    job.mObjectName = item->getName();
    job.mNewSet = false;
    job.mSetID = set_id;
    job.mPhase = PHASE_FOLDERS;
    job.mStarted = now();
    job.mLastActivity = job.mStarted;
    Item entry;
    entry.mTaskItemID = task_item_id;
    entry.mName = item->getName();
    entry.mStates.push_back(state);
    job.mItems.push_back(entry);
    mJobs[job_id] = job;

    openFolders(job_id);
    startPump(job_id);
    notifyChanged();
    return true;
}

//-----------------------------------------------------------------------------
// Job machinery
//-----------------------------------------------------------------------------

void SLAOObjectImport::startPump(const LLUUID& job_id)
{
    doPeriodically([job_id]() -> bool
        {
            if (!SLAOObjectImport::instanceExists())
            {
                return true;
            }
            return SLAOObjectImport::instance().pump(job_id);
        }, PUMP_SECS);
}

void SLAOObjectImport::openFolders(const LLUUID& job_id)
{
    auto it = mJobs.find(job_id);
    if (it == mJobs.end())
    {
        return;
    }
    Job& job = it->second;
    job.mPhase = PHASE_FOLDERS;

    auto with_set = [job_id](const LLUUID& set_id)
    {
        if (!SLAOObjectImport::instanceExists())
        {
            return;
        }
        SLAOObjectImport& self = SLAOObjectImport::instance();
        auto job_it = self.mJobs.find(job_id);
        if (job_it == self.mJobs.end())
        {
            return;
        }
        if (set_id.isNull())
        {
            LLSD args;
            args["NAME"] = job_it->second.mObjectName;
            LLNotificationsUtil::add("SLAOObjectFailed", args);
            self.mJobs.erase(job_it);
            self.notifyChanged();
            return;
        }
        job_it->second.mSetID = set_id;
        SLAOSets::instance().ensureCopiesFolder(set_id, [job_id](const LLUUID& folder_id)
            {
                if (!SLAOObjectImport::instanceExists())
                {
                    return;
                }
                SLAOObjectImport& importer = SLAOObjectImport::instance();
                auto found = importer.mJobs.find(job_id);
                if (found == importer.mJobs.end())
                {
                    return;
                }
                Job& ready = found->second;
                if (folder_id.isNull())
                {
                    LLSD args;
                    args["NAME"] = ready.mObjectName;
                    LLNotificationsUtil::add("SLAOObjectFailed", args);
                    importer.mJobs.erase(found);
                    importer.notifyChanged();
                    return;
                }
                ready.mFolderID = folder_id;
                // Whatever is already there is not ours
                LLInventoryModel::item_array_t existing;
                collect_items(ready.mFolderID, existing);
                collect_items(ready.mSetID, existing);
                for (const LLPointer<LLViewerInventoryItem>& item : existing)
                {
                    ready.mPreexisting.insert(item->getUUID());
                }
                ready.mPhase = PHASE_COPYING;
                ready.mLastActivity = now();
                importer.notifyChanged();
            });
    };

    if (job.mNewSet)
    {
        SLAOSets::instance().createSet(job.mSetName, with_set);
    }
    else
    {
        with_set(job.mSetID);
    }
}

bool SLAOObjectImport::pump(const LLUUID& job_id)
{
    auto it = mJobs.find(job_id);
    if (it == mJobs.end())
    {
        return true;
    }
    Job& job = it->second;
    const F64 t = now();

    if (job.mPhase == PHASE_READING)
    {
        if (job.mPendingNotecards > 0 && t - job.mStarted < NOTECARD_TIMEOUT_SECS)
        {
            return false;
        }
        prepare(job);
        openFolders(job_id);
        return false; // `job` may be gone after openFolders' callbacks; look again next pump
    }

    if (job.mPhase == PHASE_FOLDERS)
    {
        if (t - job.mStarted > NOTECARD_TIMEOUT_SECS + FOLDER_TIMEOUT_SECS)
        {
            LLSD args;
            args["NAME"] = job.mObjectName;
            LLNotificationsUtil::add("SLAOObjectFailed", args);
            mJobs.erase(it);
            notifyChanged();
            return true;
        }
        return false;
    }

    // PHASE_COPYING: send a few requests
    LLViewerObject* object = gObjectList.findObject(job.mObjectID);
    const bool object_gone = !object || object->isDead() || !object->getRegion();
    size_t sent_now = 0;
    while (job.mNextSend < job.mItems.size() && sent_now < SENDS_PER_PUMP)
    {
        Item& item = job.mItems[job.mNextSend++];
        if (object_gone)
        {
            ++job.mNotSent; // detached or derezzed before we got to it
            continue;
        }
        // Only ever for copyable items: for no-copy ones this would move the
        // original out of the object.
        const LLInventoryItem* task_item = dynamic_cast<const LLInventoryItem*>(object->getInventoryObject(item.mTaskItemID));
        if (!task_item || !canCopy(task_item))
        {
            ++job.mNotSent;
            continue;
        }
        object->moveInventory(item.mIsNotecard ? job.mSetID : job.mFolderID, item.mTaskItemID);
        item.mSent = true;
        ++sent_now;
    }
    if (sent_now)
    {
        job.mLastActivity = t;
    }

    // Match arrivals by name (new items only)
    LLInventoryModel::item_array_t arrived;
    collect_items(job.mFolderID, arrived);
    collect_items(job.mSetID, arrived);
    bool progress = false;
    for (const LLPointer<LLViewerInventoryItem>& inv_item : arrived)
    {
        const LLUUID& id = inv_item->getUUID();
        if (job.mPreexisting.count(id) || mClaimed.count(id) || inv_item->getIsLinkType())
        {
            continue;
        }
        for (Item& item : job.mItems)
        {
            if (item.mSent && item.mCopyID.isNull() && item.mName == inv_item->getName() &&
                (item.mIsNotecard ? inv_item->getType() == LLAssetType::AT_NOTECARD
                                  : inv_item->getType() == LLAssetType::AT_ANIMATION))
            {
                item.mCopyID = id;
                mClaimed.insert(id);
                progress = true;
                break;
            }
        }
    }
    if (progress)
    {
        job.mLastActivity = t;
        notifyChanged();
    }

    const bool all_sent = job.mNextSend >= job.mItems.size();
    const bool all_arrived = std::all_of(job.mItems.begin(), job.mItems.end(),
        [](const Item& item) { return !item.mSent || item.mCopyID.notNull(); });
    if (all_sent && (all_arrived || t - job.mLastActivity > STALL_TIMEOUT_SECS))
    {
        finish(job_id);
        return true;
    }
    return false;
}

void SLAOObjectImport::finish(const LLUUID& job_id)
{
    auto it = mJobs.find(job_id);
    if (it == mJobs.end())
    {
        return;
    }
    Job job = it->second;
    mJobs.erase(it);
    if (mJobs.empty())
    {
        mClaimed.clear();
    }

    // Link the copies into their states, in the object's order
    std::map<S32, uuid_vec_t> by_state;
    S32 copied = 0;
    S32 unsorted = 0;
    S32 failed = 0;
    for (const Item& item : job.mItems)
    {
        if (item.mIsNotecard)
        {
            continue;
        }
        if (item.mCopyID.isNull())
        {
            ++failed;
            continue;
        }
        ++copied;
        if (item.mStates.empty())
        {
            ++unsorted;
        }
        for (EState state : item.mStates)
        {
            by_state[state].push_back(item.mCopyID);
        }
    }
    for (const auto& [state, ids] : by_state)
    {
        SLAOSets::instance().addAnimations(job.mSetID, (EState)state, ids);
    }
    SLAOSets::instance().requestRescan();

    LLSD args;
    args["NAME"] = job.mObjectName;
    args["COPIED"] = copied;
    args["STATES"] = (S32)by_state.size();
    args["UNSORTED"] = unsorted;
    args["NOCOPY"] = job.mNoCopy;
    args["FAILED"] = failed + job.mNotSent;
    args["NOTECARD"] = job.mNotecardUsed;
    if (!job.mNewSet)
    {
        if (copied == 0)
        {
            LLNotificationsUtil::add("SLAOObjectCopyFailed", args);
        }
        notifyChanged();
        return;
    }

    std::string details;
    if (!job.mNotecardUsed.empty())
    {
        LLStringUtil::format_map_t fmt;
        fmt["[NOTECARD]"] = job.mNotecardUsed;
        details += LLTrans::getString("SLAOObjectUsedNotecard", fmt);
    }
    if (unsorted)
    {
        LLStringUtil::format_map_t fmt;
        fmt["[COUNT]"] = llformat("%d", unsorted);
        details += LLTrans::getString("SLAOObjectUnsorted", fmt);
    }
    if (job.mNoCopy)
    {
        LLStringUtil::format_map_t fmt;
        fmt["[COUNT]"] = llformat("%d", job.mNoCopy);
        details += LLTrans::getString("SLAOObjectNoCopyLeft", fmt);
    }
    if (failed + job.mNotSent)
    {
        LLStringUtil::format_map_t fmt;
        fmt["[COUNT]"] = llformat("%d", failed + job.mNotSent);
        details += LLTrans::getString("SLAOObjectNotCopied", fmt);
    }
    args["DETAILS"] = details;
    LLNotificationsUtil::add(copied ? "SLAOObjectImported" : "SLAOObjectCopyFailed", args);
    LL_INFOS() << "AO: import from '" << job.mObjectName << "' done: " << copied << " copied, " << unsorted
               << " unsorted, " << job.mNoCopy << " no-copy, " << (failed + job.mNotSent) << " failed" << LL_ENDL;
    notifyChanged();
}
