/**
 * @file slaoobjectimport.h
 * @brief Client-side animation overrider (#496): take animations out of an
 *        object's contents (a worn AO HUD, any attachment, a rezzed box)
 *        into an AO set. Only copyable items are taken, as copies; no-copy
 *        items stay in the object.
 *
 * How the copy works: LLViewerObject::moveInventory() sends
 * MoveTaskInventory with the destination folder. For a copyable item the
 * simulator puts a copy in that folder and leaves the original in the
 * object (the same path as "Copy to Inventory" in an object's Contents).
 * The copies arrive asynchronously; each job watches its destination
 * folder and matches new items by name, then links them into states.
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

#ifndef SL_AOOBJECTIMPORT_H
#define SL_AOOBJECTIMPORT_H

#include <map>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lluuid.h"
#include "slaoset.h"

class LLInventoryItem;

class SLAOObjectImport : public LLSingleton<SLAOObjectImport>
{
    LLSINGLETON(SLAOObjectImport);
    ~SLAOObjectImport() override;
    LOG_CLASS(SLAOObjectImport);

public:
    // True if the agent may take a copy of this object inventory item.
    static bool canCopy(const LLInventoryItem* item);

    // Import every copyable animation of an object (whose inventory has been
    // loaded) as a new set named `set_name`. An AO notecard inside, if any,
    // maps animations to states; otherwise names are used; the rest stays in
    // the set's "Unsorted" list. Returns false if nothing can be taken.
    bool importObject(const LLUUID& object_id, const std::string& set_name,
                      const LLUUID& preferred_notecard = LLUUID::null);

    // Copy one animation from an object's contents into a set's state.
    bool copyToState(const LLUUID& object_id, const LLUUID& task_item_id,
                     const LLUUID& set_id, SLAO::EState state);

    bool isBusy() const { return !mJobs.empty(); }
    std::string getStatusText() const;

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

private:
    struct Item
    {
        LLUUID      mTaskItemID;
        std::string mName;
        bool        mIsNotecard = false;
        std::vector<SLAO::EState> mStates;  // empty: unsorted
        bool        mSent = false;
        LLUUID      mCopyID;                // the new item in the agent's inventory
    };

    enum EPhase { PHASE_READING, PHASE_FOLDERS, PHASE_COPYING };

    struct Job
    {
        LLUUID      mObjectID;
        std::string mObjectName;
        std::string mSetName;
        bool        mNewSet = true;
        LLUUID      mSetID;
        LLUUID      mFolderID;              // copies folder
        EPhase      mPhase = PHASE_READING;
        std::vector<Item> mItems;
        size_t      mNextSend = 0;
        uuid_set_t  mPreexisting;           // items already in the folders
        F64         mStarted = 0.0;
        F64         mLastActivity = 0.0;
        S32         mNoCopy = 0;
        S32         mNotSent = 0;           // object gone before sending
        // Notecard reading
        S32         mPendingNotecards = 0;
        std::vector<std::pair<std::string, std::string>> mNotecardTexts; // name, text
        std::string mNotecardUsed;
        S32         mGuessed = 0;
    };

    void startPump(const LLUUID& job_id);
    bool pump(const LLUUID& job_id);    // true when finished
    void prepare(Job& job);
    void openFolders(const LLUUID& job_id);
    void finish(const LLUUID& job_id);
    void onNotecardText(const LLUUID& job_id, const std::string& name, const std::string& text);
    void notifyChanged() { mChangedSignal(); }

    std::map<LLUUID, Job> mJobs;
    uuid_set_t mClaimed;    // copies already matched to an item (all jobs)
    changed_signal_t mChangedSignal;
};

#endif // SL_AOOBJECTIMPORT_H
