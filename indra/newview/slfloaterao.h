/**
 * @file slfloaterao.h
 * @brief Client-side animation overrider (#496): the compact control and
 *        the full AO editor.
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

#ifndef SL_FLOATERAO_H
#define SL_FLOATERAO_H

#include <map>
#include <vector>

#include <boost/signals2.hpp>

#include "llfloater.h"
#include "llvoinventorylistener.h"
#include "slaoset.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;

// Small always-handy control: on/off, set, next/previous stand, sits,
// pause, and why the AO is paused if it is.
class SLFloaterAO : public LLFloater
{
    LOG_CLASS(SLFloaterAO);
public:
    SLFloaterAO(const LLSD& key);
    ~SLFloaterAO() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void refreshSets();
    void refreshStatus();

    LLCheckBoxCtrl* mEnabledCheck = nullptr;
    LLComboBox* mSetCombo = nullptr;
    LLCheckBoxCtrl* mSitCheck = nullptr;
    LLButton* mPauseBtn = nullptr;
    LLTextBox* mStatusText = nullptr;

    bool mSetsDirty = true;
    bool mStatusDirty = true;
    boost::signals2::scoped_connection mSetsConnection;
    boost::signals2::scoped_connection mEngineConnection;
};

// Full editor: sets, states, animations, ordering, cycling, options, import.
class SLFloaterAOEditor : public LLFloater
{
    LOG_CLASS(SLFloaterAOEditor);
public:
    SLFloaterAOEditor(const LLSD& key);
    ~SLFloaterAOEditor() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void refreshSets();
    void refreshStates();
    void refreshAnims();
    void refreshOptions();
    void refreshStatus();

    LLUUID getSelectedSetID() const;
    SLAO::EState getSelectedState() const;
    LLUUID getSelectedAnimItem() const;
    bool canEdit() const;
    bool isUnsortedSelected() const { return mSelectedState == SLAO::STATE_COUNT; }
    // Animations listed for the selected state (or the unsorted copies)
    const std::vector<SLAOAnim>* getShownAnims() const;

    void onSetSelected();
    void onStateSelected();
    void onAnimSelected();
    void onNewSet();
    void onRenameSet();
    void onDeleteSet();
    void onActivateSet();
    void onImport();
    void onMoveAnim(S32 delta);
    void onRemoveAnim();
    void onPreviewAnim();
    void onStateConfigChanged();
    void onSetOptionsChanged();
    void onAssignUnsorted();
    void onIdleOptionsChanged();

    LLScrollListCtrl* mSetList = nullptr;
    LLScrollListCtrl* mStateList = nullptr;
    LLScrollListCtrl* mAnimList = nullptr;
    LLCheckBoxCtrl* mCycleCheck = nullptr;
    LLCheckBoxCtrl* mRandomCheck = nullptr;
    LLCheckBoxCtrl* mPickOnEntryCheck = nullptr;
    LLSpinCtrl* mIntervalSpin = nullptr;
    LLCheckBoxCtrl* mSitOverrideCheck = nullptr;
    LLCheckBoxCtrl* mSmartSitCheck = nullptr;
    LLCheckBoxCtrl* mNoMouselookCheck = nullptr;
    LLCheckBoxCtrl* mNoTypingCheck = nullptr;
    LLCheckBoxCtrl* mStopSystemCheck = nullptr;
    LLCheckBoxCtrl* mAlwaysCheck = nullptr;
    LLCheckBoxCtrl* mIdleCheck = nullptr;
    LLSpinCtrl* mIdleMinSpin = nullptr;
    LLSpinCtrl* mIdleMaxSpin = nullptr;
    LLSpinCtrl* mIdleCapSpin = nullptr;
    LLCheckBoxCtrl* mIdleSitCheck = nullptr;
    LLButton* mPreviewBtn = nullptr;
    LLComboBox* mAssignCombo = nullptr;
    LLTextBox* mStatusText = nullptr;
    LLTextBox* mHintText = nullptr;

    LLUUID mSelectedSet;
    S32 mSelectedState = SLAO::STAND;
    S32 mMarkedState = SLAO::STATE_NONE;
    std::string mAnimMarkerKey;
    bool mSetsDirty = true;
    bool mStatusDirty = true;
    boost::signals2::scoped_connection mSetsConnection;
    boost::signals2::scoped_connection mEngineConnection;
    boost::signals2::scoped_connection mImportConnection;
};

// Pick a worn HUD or attachment and take its copyable animations into a new
// set (key: optional object id to preselect).
class SLFloaterAOObjectImport : public LLFloater, public LLVOInventoryListener
{
    LOG_CLASS(SLFloaterAOObjectImport);
public:
    SLFloaterAOObjectImport(const LLSD& key);
    ~SLFloaterAOObjectImport() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // LLVOInventoryListener
    void inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory,
                          S32 serial_num, void* user_data) override;

private:
    struct Entry
    {
        LLUUID      mObjectID;
        std::string mName;
        std::string mPoint;
        bool        mHUD = false;
        S32         mAnims = -1;    // -1: contents not loaded yet
        S32         mCopyable = 0;
        S32         mNotecards = 0;
    };

    void scanAttachments();
    void stopListening();
    void refreshList();
    void onImport();

    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mStatusText = nullptr;
    std::map<LLUUID, Entry> mEntries;
    LLUUID mPreselect;
    bool mListDirty = true;
    bool mStatusDirty = true;
    boost::signals2::scoped_connection mImportConnection;
};

// Manage the per-account list of animations that never pause the AO.
class SLFloaterAOIgnored : public LLFloater
{
    LOG_CLASS(SLFloaterAOIgnored);
public:
    SLFloaterAOIgnored(const LLSD& key);
    ~SLFloaterAOIgnored() override;

    bool postBuild() override;
    void draw() override;

private:
    LLScrollListCtrl* mList = nullptr;
    LLScrollListCtrl* mScriptedList = nullptr;
    bool mDirty = true;
    boost::signals2::scoped_connection mSetsConnection;
};

#endif // SL_FLOATERAO_H
