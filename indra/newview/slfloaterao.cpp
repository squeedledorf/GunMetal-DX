/**
 * @file slfloaterao.cpp
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

#include "llviewerprecompiledheaders.h"

#include "slfloaterao.h"

#include <algorithm>

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lltooldraganddrop.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "llfolderview.h"
#include "llfolderviewmodelinventory.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "slaoengine.h"
#include "slaoobjectimport.h"
#include "slaosets.h"

using namespace SLAO;

namespace
{
    constexpr size_t MAX_SET_NAME = 60;

    void prompt_name(const std::string& prompt, const std::string& default_name,
                     std::function<void(const std::string&)> cb)
    {
        LLSD args;
        args["PROMPT"] = prompt;
        args["NAME"] = default_name;
        LLNotificationsUtil::add("SLAONamePrompt", args, LLSD(),
            [cb](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                {
                    return;
                }
                std::string name = response["new_name"].asString();
                LLStringUtil::trim(name);
                if (name.size() > MAX_SET_NAME)
                {
                    name.resize(MAX_SET_NAME);
                }
                if (!name.empty())
                {
                    cb(name);
                }
            });
    }

    // The agent's own inventory item behind a drag, or null (object contents,
    // library, ...).
    LLViewerInventoryItem* own_item(void* cargo_data)
    {
        const LLInventoryObject* obj = static_cast<const LLInventoryObject*>(cargo_data);
        return obj ? gInventory.getItem(obj->getUUID()) : nullptr;
    }

    // The object whose Contents a drag comes from, if it does
    const LLViewerObject* contents_source()
    {
        LLToolDragAndDrop* dnd = LLToolDragAndDrop::getInstance();
        if (dnd->getSource() != LLToolDragAndDrop::SOURCE_WORLD)
        {
            return nullptr;
        }
        const LLViewerObject* object = gObjectList.findObject(dnd->getObjectID());
        return (object && !object->isDead()) ? object : nullptr;
    }

    // Name for a set taken from an object: its inventory name if worn
    std::string object_label(const LLViewerObject* object)
    {
        const LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        if (root && root->isAttachment())
        {
            if (const LLViewerInventoryItem* item = gInventory.getItem(root->getAttachmentItemID()))
            {
                return item->getName();
            }
        }
        return LLTrans::getString("SLAOObjectDefaultName");
    }

    // What the state list marks as playing, packed into one number:
    // posture state + 100 if an idle plays + 200 if Always animations run.
    S32 marker_key(const LLUUID& selected_set)
    {
        SLAOEngine& engine = SLAOEngine::instance();
        if (selected_set != engine.getCurrentSetID())
        {
            return -1;
        }
        return (S32)engine.getCurrentState() + 1 + (engine.isIdlePlaying() ? 100 : 0) +
               (engine.getAlwaysCount() > 0 ? 200 : 0);
    }

    bool marker_shows(S32 key, S32 state)
    {
        if (key < 0)
        {
            return false;
        }
        if (state == IDLE)
        {
            return (key % 200) >= 100;
        }
        if (state == ALWAYS)
        {
            return key >= 200;
        }
        // During an idle the stand is not what plays
        return (key % 200) < 100 && (key % 100) - 1 == state;
    }

    // Shared drop handling. With a set and state, animations are added to it;
    // notecards and folders are imported either way.
    bool handle_drop(bool drop, EDragAndDropType cargo_type, void* cargo_data, EAcceptance* accept,
                     std::string& tooltip_msg, const LLUUID& set_id, EState state, bool can_edit)
    {
        switch (cargo_type)
        {
        case DAD_ANIMATION:
        case DAD_LINK:
        {
            if (const LLViewerObject* source = contents_source())
            {
                // From an object's Contents: copy it out first (copyable only)
                const LLInventoryItem* task_item = static_cast<const LLInventoryItem*>(cargo_data);
                if (!task_item || task_item->getType() != LLAssetType::AT_ANIMATION)
                {
                    *accept = ACCEPT_NO;
                    return true;
                }
                if (!SLAOObjectImport::canCopy(task_item))
                {
                    *accept = ACCEPT_NO;
                    tooltip_msg = LLTrans::getString("SLAODropNoCopy");
                    return true;
                }
                if (set_id.isNull() || state == STATE_NONE || !can_edit)
                {
                    *accept = ACCEPT_NO;
                    tooltip_msg = LLTrans::getString("SLAODropPickState");
                    return true;
                }
                *accept = ACCEPT_YES_COPY_MULTI;
                tooltip_msg = LLTrans::getString("SLAODropCopy");
                if (drop)
                {
                    SLAOObjectImport::instance().copyToState(source->getID(), task_item->getUUID(), set_id, state);
                }
                return true;
            }
            LLViewerInventoryItem* item = own_item(cargo_data);
            if (!item || item->getType() != LLAssetType::AT_ANIMATION || set_id.isNull() ||
                state == STATE_NONE || !can_edit)
            {
                *accept = ACCEPT_NO;
                if (item && item->getType() == LLAssetType::AT_ANIMATION)
                {
                    tooltip_msg = LLTrans::getString("SLAODropPickState");
                }
                return true;
            }
            *accept = ACCEPT_YES_MULTI;
            if (drop)
            {
                SLAOSets::instance().addAnimations(set_id, state, { item->getUUID() });
            }
            return true;
        }
        case DAD_NOTECARD:
        {
            if (const LLViewerObject* source = contents_source())
            {
                // A notecard in an object's Contents: import the whole object,
                // using this notecard for the states
                const LLInventoryItem* task_item = static_cast<const LLInventoryItem*>(cargo_data);
                *accept = task_item ? ACCEPT_YES_SINGLE : ACCEPT_NO;
                tooltip_msg = LLTrans::getString("SLAODropObjectNotecard");
                if (drop && task_item)
                {
                    SLAOObjectImport::instance().importObject(source->getID(), object_label(source),
                                                              task_item->getUUID());
                }
                return true;
            }
            LLViewerInventoryItem* item = own_item(cargo_data);
            if (!item)
            {
                *accept = ACCEPT_NO;
                return true;
            }
            *accept = ACCEPT_YES_SINGLE;
            tooltip_msg = LLTrans::getString("SLAODropImport");
            if (drop)
            {
                SLAOSets::instance().importNotecard(item->getUUID());
            }
            return true;
        }
        case DAD_CATEGORY:
        {
            const LLInventoryObject* obj = static_cast<const LLInventoryObject*>(cargo_data);
            LLViewerInventoryCategory* cat = obj ? gInventory.getCategory(obj->getUUID()) : nullptr;
            if (!cat)
            {
                *accept = ACCEPT_NO;
                return true;
            }
            *accept = ACCEPT_YES_SINGLE;
            tooltip_msg = LLTrans::getString("SLAODropFolder");
            if (drop && !SLAOSets::instance().importFolder(cat->getUUID()))
            {
                // No notecard and no state folders: add its animations to the
                // selected state, if there is one.
                if (set_id.notNull() && state != STATE_NONE && can_edit)
                {
                    LLInventoryModel::cat_array_t cats;
                    LLInventoryModel::item_array_t items;
                    LLIsType is_anim(LLAssetType::AT_ANIMATION);
                    gInventory.collectDescendentsIf(cat->getUUID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, is_anim);
                    uuid_vec_t ids;
                    for (const LLPointer<LLViewerInventoryItem>& anim : items)
                    {
                        if (ids.size() >= 256)
                        {
                            break;
                        }
                        ids.push_back(anim->getUUID());
                    }
                    if (!ids.empty())
                    {
                        SLAOSets::instance().addAnimations(set_id, state, ids);
                        return true;
                    }
                }
                LLNotificationsUtil::add("SLAOImportHint");
            }
            return true;
        }
        default:
            *accept = ACCEPT_NO;
            return false;
        }
    }
}

//=============================================================================
// SLFloaterAO: compact control
//=============================================================================

SLFloaterAO::SLFloaterAO(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterAO::~SLFloaterAO()
{
}

bool SLFloaterAO::postBuild()
{
    mEnabledCheck = getChild<LLCheckBoxCtrl>("ao_enabled");
    mSetCombo = getChild<LLComboBox>("set_combo");
    mSitCheck = getChild<LLCheckBoxCtrl>("sit_override");
    mPauseBtn = getChild<LLButton>("pause_btn");
    mStatusText = getChild<LLTextBox>("status_text");

    mEnabledCheck->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            SLAOEngine::instance().setEnabled(ctrl->getValue().asBoolean());
        });
    mSetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            SLAOEngine::instance().selectSet(mSetCombo->getValue().asUUID());
        });
    mSitCheck->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            SLAOEngine::instance().setSitOverride(ctrl->getValue().asBoolean());
        });
    mPauseBtn->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            SLAOEngine& engine = SLAOEngine::instance();
            engine.setUserPaused(!engine.isUserPaused());
        });
    getChild<LLButton>("prev_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLAOEngine::instance().nextAnim(-1); });
    getChild<LLButton>("next_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLAOEngine::instance().nextAnim(1); });
    getChild<LLButton>("edit_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("sl_ao_editor"); });
    getChild<LLButton>("ignore_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLAOEngine::instance().ignoreTakeoverAnim(); });
    getChild<LLButton>("resume_scripted_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLAOEngine::instance().resumeFromScriptedAO(); });

    mSetsConnection = SLAOSets::instance().setChangedCallback([this]() { mSetsDirty = true; mStatusDirty = true; });
    mEngineConnection = SLAOEngine::instance().setChangedCallback([this]() { mStatusDirty = true; });
    return true;
}

void SLFloaterAO::onOpen(const LLSD& key)
{
    mSetsDirty = true;
    mStatusDirty = true;
}

void SLFloaterAO::draw()
{
    if (mSetsDirty)
    {
        mSetsDirty = false;
        refreshSets();
    }
    if (mStatusDirty)
    {
        mStatusDirty = false;
        refreshStatus();
    }
    LLFloater::draw();
}

void SLFloaterAO::refreshSets()
{
    mSetCombo->removeall();
    const std::vector<const SLAOSet*> sets = SLAOSets::instance().getSortedSets();
    for (const SLAOSet* set : sets)
    {
        std::string label = set->mName;
        if (set->mFromOtherViewer)
        {
            label += LLTrans::getString("SLAOOtherViewerSuffix");
        }
        mSetCombo->add(label, LLSD(set->mFolderID));
    }
    if (sets.empty())
    {
        mSetCombo->add(LLTrans::getString("SLAONoSets"), LLSD(LLUUID::null), ADD_BOTTOM, false);
    }
    mSetCombo->selectByValue(LLSD(SLAOEngine::instance().getCurrentSetID()));
}

void SLFloaterAO::refreshStatus()
{
    SLAOEngine& engine = SLAOEngine::instance();
    const SLAOSet* set = engine.getCurrentSet();
    mEnabledCheck->setValue(engine.isEnabled());
    mSitCheck->setValue(set && set->mSitOverride);
    mSitCheck->setEnabled(set != nullptr);
    mPauseBtn->setToggleState(engine.isUserPaused());
    mPauseBtn->setLabel(LLTrans::getString(engine.isUserPaused() ? "SLAOResume" : "SLAOPause"));
    mPauseBtn->setEnabled(engine.isEnabled());
    mStatusText->setText(engine.getStatusText());
    const bool by_animation = (engine.getPauseReason() == SLAOEngine::PAUSE_SCRIPT_ANIM ||
                               engine.getPauseReason() == SLAOEngine::PAUSE_SERVER_AO) &&
                              engine.getTakeoverAnim().notNull();
    getChild<LLUICtrl>("ignore_btn")->setVisible(by_animation);
    getChild<LLUICtrl>("resume_scripted_btn")->setVisible(engine.getPauseReason() == SLAOEngine::PAUSE_SCRIPTED_AO);
    if (mSetCombo->getValue().asUUID() != engine.getCurrentSetID())
    {
        mSetCombo->selectByValue(LLSD(engine.getCurrentSetID()));
    }
}

bool SLFloaterAO::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                    EDragAndDropType cargo_type, void* cargo_data,
                                    EAcceptance* accept, std::string& tooltip_msg)
{
    // Notecards and folders import; animations need the editor's state list
    if (cargo_type == DAD_NOTECARD || cargo_type == DAD_CATEGORY)
    {
        return handle_drop(drop, cargo_type, cargo_data, accept, tooltip_msg, LLUUID::null, STATE_NONE, false);
    }
    return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
}

//=============================================================================
// SLFloaterAOEditor: full editor
//=============================================================================

SLFloaterAOEditor::SLFloaterAOEditor(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterAOEditor::~SLFloaterAOEditor()
{
}

bool SLFloaterAOEditor::postBuild()
{
    mSetList = getChild<LLScrollListCtrl>("set_list");
    mStateList = getChild<LLScrollListCtrl>("state_list");
    mAnimList = getChild<LLScrollListCtrl>("anim_list");
    mCycleCheck = getChild<LLCheckBoxCtrl>("cycle_check");
    mRandomCheck = getChild<LLCheckBoxCtrl>("random_check");
    mIntervalSpin = getChild<LLSpinCtrl>("interval_spin");
    mSitOverrideCheck = getChild<LLCheckBoxCtrl>("sit_override_check");
    mSmartSitCheck = getChild<LLCheckBoxCtrl>("smart_sit_check");
    mNoMouselookCheck = getChild<LLCheckBoxCtrl>("no_mouselook_check");
    mNoTypingCheck = getChild<LLCheckBoxCtrl>("no_typing_check");
    mPreviewBtn = getChild<LLButton>("preview_btn");
    mStatusText = getChild<LLTextBox>("status_text");
    mHintText = getChild<LLTextBox>("hint_text");

    mSetList->setCommitOnSelectionChange(true);
    mSetList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetSelected(); });
    mSetList->setDoubleClickCallback([this]() { onActivateSet(); });
    mStateList->setCommitOnSelectionChange(true);
    mStateList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStateSelected(); });
    mAnimList->setCommitOnSelectionChange(true);
    mAnimList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAnimSelected(); });
    mAnimList->setDoubleClickCallback([this]() { onPreviewAnim(); });

    getChild<LLButton>("new_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNewSet(); });
    getChild<LLButton>("rename_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenameSet(); });
    getChild<LLButton>("delete_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteSet(); });
    getChild<LLButton>("activate_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onActivateSet(); });
    getChild<LLButton>("import_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    getChild<LLButton>("up_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveAnim(-1); });
    getChild<LLButton>("down_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveAnim(1); });
    getChild<LLButton>("remove_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveAnim(); });
    mPreviewBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreviewAnim(); });
    getChild<LLButton>("ignored_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("sl_ao_ignored");
        });
    getChild<LLButton>("import_object_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("sl_ao_import_object");
        });
    mAssignCombo = getChild<LLComboBox>("assign_combo");
    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        mAssignCombo->add(stateLabel((EState)i), LLSD(i));
    }
    mAssignCombo->selectByValue(LLSD((S32)STAND));
    getChild<LLButton>("assign_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAssignUnsorted(); });

    mCycleCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStateConfigChanged(); });
    mRandomCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStateConfigChanged(); });
    mPickOnEntryCheck = getChild<LLCheckBoxCtrl>("pick_on_entry_check");
    mPickOnEntryCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStateConfigChanged(); });
    mIntervalSpin->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStateConfigChanged(); });
    mSitOverrideCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetOptionsChanged(); });
    mSmartSitCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetOptionsChanged(); });
    mNoMouselookCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetOptionsChanged(); });
    mNoTypingCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetOptionsChanged(); });
    mAlwaysCheck = getChild<LLCheckBoxCtrl>("always_check");
    mIdleCheck = getChild<LLCheckBoxCtrl>("idle_check");
    mIdleMinSpin = getChild<LLSpinCtrl>("idle_min_spin");
    mIdleMaxSpin = getChild<LLSpinCtrl>("idle_max_spin");
    mIdleCapSpin = getChild<LLSpinCtrl>("idle_cap_spin");
    mIdleSitCheck = getChild<LLCheckBoxCtrl>("idle_sit_check");
    mStopSystemCheck = getChild<LLCheckBoxCtrl>("stop_system_check");
    mStopSystemCheck->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        {
            if (canEdit())
            {
                SLAOSets::instance().setStopSystemAnims(mSelectedSet, ctrl->getValue().asBoolean());
            }
        });
    mAlwaysCheck->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        {
            if (canEdit())
            {
                SLAOSets::instance().setAlwaysEnabled(mSelectedSet, ctrl->getValue().asBoolean());
            }
        });
    for (LLUICtrl* ctrl : { (LLUICtrl*)mIdleCheck, (LLUICtrl*)mIdleMinSpin, (LLUICtrl*)mIdleMaxSpin,
                            (LLUICtrl*)mIdleCapSpin, (LLUICtrl*)mIdleSitCheck })
    {
        ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIdleOptionsChanged(); });
    }

    getChild<LLCheckBoxCtrl>("ao_enabled")->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            SLAOEngine::instance().setEnabled(ctrl->getValue().asBoolean());
        });

    mSetsConnection = SLAOSets::instance().setChangedCallback([this]() { mSetsDirty = true; mStatusDirty = true; });
    mEngineConnection = SLAOEngine::instance().setChangedCallback([this]() { mStatusDirty = true; });
    mImportConnection = SLAOObjectImport::instance().setChangedCallback([this]() { mSetsDirty = true; });
    return true;
}

void SLFloaterAOEditor::onOpen(const LLSD& key)
{
    if (mSelectedSet.isNull() || !SLAOSets::instance().getSet(mSelectedSet))
    {
        mSelectedSet = SLAOEngine::instance().getCurrentSetID();
    }
    mSetsDirty = true;
    mStatusDirty = true;
}

void SLFloaterAOEditor::onClose(bool app_quitting)
{
    if (!app_quitting && SLAOEngine::instanceExists())
    {
        SLAOEngine::instance().stopPreview();
    }
}

void SLFloaterAOEditor::draw()
{
    if (mSetsDirty)
    {
        mSetsDirty = false;
        refreshSets();
        refreshStates();
        refreshAnims();
        refreshOptions();
    }
    if (mStatusDirty)
    {
        mStatusDirty = false;
        refreshStatus();
    }
    LLFloater::draw();
}

LLUUID SLFloaterAOEditor::getSelectedSetID() const
{
    return SLAOSets::instance().getSet(mSelectedSet) ? mSelectedSet : LLUUID::null;
}

EState SLFloaterAOEditor::getSelectedState() const
{
    return (mSelectedState > STATE_NONE && mSelectedState < STATE_COUNT) ? (EState)mSelectedState : STATE_NONE;
}

const std::vector<SLAOAnim>* SLFloaterAOEditor::getShownAnims() const
{
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    if (!set)
    {
        return nullptr;
    }
    if (isUnsortedSelected())
    {
        return &set->mUnsorted;
    }
    const EState state = getSelectedState();
    return (state != STATE_NONE) ? &set->mAnims[state] : nullptr;
}

LLUUID SLFloaterAOEditor::getSelectedAnimItem() const
{
    LLScrollListItem* item = mAnimList->getFirstSelected();
    return item ? item->getUUID() : LLUUID::null;
}

bool SLFloaterAOEditor::canEdit() const
{
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    return set && !set->mFromSnapshot && SLAOSets::instance().isInventoryScanned();
}

//-----------------------------------------------------------------------------
// Refresh
//-----------------------------------------------------------------------------

void SLFloaterAOEditor::refreshSets()
{
    const S32 scroll = mSetList->getScrollPos();
    mSetList->deleteAllItems();
    const LLUUID current = SLAOEngine::instance().getCurrentSetID();
    for (const SLAOSet* set : SLAOSets::instance().getSortedSets())
    {
        std::string name = set->mName;
        if (set->mFromOtherViewer)
        {
            name += LLTrans::getString("SLAOOtherViewerSuffix");
        }
        LLSD row;
        row["value"] = set->mFolderID;
        row["columns"][0]["column"] = "active";
        row["columns"][0]["value"] = (set->mFolderID == current) ? LLTrans::getString("SLAOActiveMarker") : std::string();
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = name;
        if (set->mFolderID == current)
        {
            row["columns"][1]["font"]["style"] = "BOLD";
        }
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = set->countAnims();
        mSetList->addElement(row);
    }
    if (mSelectedSet.isNull() || !SLAOSets::instance().getSet(mSelectedSet))
    {
        mSelectedSet = current;
    }
    mSetList->selectByID(mSelectedSet);
    mSetList->setScrollPos(scroll);

    const bool have_set = getSelectedSetID().notNull();
    const bool ready = SLAOSets::instance().isInventoryScanned();
    getChild<LLUICtrl>("new_btn")->setEnabled(ready);
    getChild<LLUICtrl>("rename_btn")->setEnabled(have_set && canEdit());
    getChild<LLUICtrl>("delete_btn")->setEnabled(have_set && canEdit());
    getChild<LLUICtrl>("activate_btn")->setEnabled(have_set && mSelectedSet != current);
    getChild<LLUICtrl>("import_btn")->setEnabled(ready);
}

void SLFloaterAOEditor::refreshStates()
{
    const S32 scroll = mStateList->getScrollPos();
    mStateList->deleteAllItems();
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    SLAOEngine& engine = SLAOEngine::instance();
    mMarkedState = marker_key(mSelectedSet);
    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        const EState state = (EState)i;
        const S32 count = set ? (S32)set->mAnims[i].size() : 0;
        LLSD row;
        row["value"] = i;
        row["columns"][0]["column"] = "marker";
        row["columns"][0]["value"] = marker_shows(mMarkedState, i) ? LLTrans::getString("SLAOPlayingMarker") : std::string();
        row["columns"][1]["column"] = "name";
        std::string label = stateLabel(state);
        if (count == 0 && set && set->resolveState(state) != STATE_NONE)
        {
            // Uses another state's animations
            LLStringUtil::format_map_t args;
            args["[STATE]"] = stateLabel(set->resolveState(state));
            label += LLTrans::getString("SLAOUsesState", args);
        }
        row["columns"][1]["value"] = label;
        if (count > 0)
        {
            row["columns"][1]["font"]["style"] = "BOLD";
        }
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = count ? llformat("%d", count) : std::string();
        mStateList->addElement(row);
    }
    if (set && set->mHasCopies)
    {
        // Copies taken from an object that no state uses yet
        LLSD row;
        row["value"] = (S32)STATE_COUNT;
        row["columns"][0]["column"] = "marker";
        row["columns"][0]["value"] = std::string();
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = LLTrans::getString("SLAOUnsortedLabel");
        row["columns"][1]["font"]["style"] = set->mUnsorted.empty() ? "NORMAL" : "BOLD";
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = set->mUnsorted.empty() ? std::string() : llformat("%d", (S32)set->mUnsorted.size());
        mStateList->addElement(row);
    }
    else if (mSelectedState == STATE_COUNT)
    {
        mSelectedState = STAND;
    }
    mStateList->selectByValue(LLSD(mSelectedState));
    mStateList->setScrollPos(scroll);
}

void SLFloaterAOEditor::refreshAnims()
{
    const LLUUID selected = getSelectedAnimItem();
    const S32 scroll = mAnimList->getScrollPos();
    mAnimList->deleteAllItems();
    const std::vector<SLAOAnim>* shown = getShownAnims();
    SLAOEngine& engine = SLAOEngine::instance();
    if (shown)
    {
        S32 position = 1;
        for (const SLAOAnim& anim : *shown)
        {
            std::string status;
            if (engine.isAnimationBad(anim.mAssetID))
            {
                status = LLTrans::getString("SLAOAnimFailed");
            }
            else if (engine.isLowPriority(anim.mAssetID) && anim.mAssetID != engine.getPreviewAnim() &&
                     !(mSelectedSet == engine.getCurrentSetID() && anim.mAssetID == engine.getCurrentAnim()))
            {
                status = LLTrans::getString("SLAOAnimLowPriority");
            }
            else if (anim.mAssetID == engine.getPreviewAnim())
            {
                status = LLTrans::getString("SLAOAnimPreviewing");
            }
            else if (mSelectedSet == engine.getCurrentSetID() &&
                     (anim.mAssetID == engine.getCurrentAnim() || engine.isAlwaysPlaying(anim.mAssetID)))
            {
                status = LLTrans::getString("SLAOAnimPlaying");
            }
            LLSD row;
            row["value"] = anim.mItemID.notNull() ? anim.mItemID : anim.mAssetID;
            row["columns"][0]["column"] = "pos";
            row["columns"][0]["value"] = position++;
            row["columns"][1]["column"] = "name";
            row["columns"][1]["value"] = anim.mName;
            row["columns"][2]["column"] = "status";
            row["columns"][2]["value"] = status;
            mAnimList->addElement(row);
        }
    }
    if (selected.notNull())
    {
        mAnimList->selectByID(selected);
    }
    mAnimList->setScrollPos(scroll);
    onAnimSelected();
}

void SLFloaterAOEditor::refreshOptions()
{
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    const EState state = getSelectedState();
    const bool editable = canEdit();

    const SLAOStateConfig config = (set && state != STATE_NONE) ? set->mStateConfig[state] : SLAOStateConfig();
    mCycleCheck->setValue(config.mCycle);
    mRandomCheck->setValue(config.mRandom);
    mIntervalSpin->setValue(config.mInterval);
    // Always plays its whole list at once. For Idle every idle is an entry
    // into the state, so "change only on entry" and the interval add nothing.
    const bool cycles = state != STATE_NONE && state != ALWAYS;
    const bool timed = cycles && state != IDLE;
    mCycleCheck->setEnabled(editable && cycles);
    // Random and "change only on entry" are ways of cycling
    mRandomCheck->setEnabled(editable && cycles && config.mCycle);
    mPickOnEntryCheck->setValue(timed && config.mPickOnEntry);
    mPickOnEntryCheck->setEnabled(editable && timed && config.mCycle);
    mIntervalSpin->setEnabled(editable && timed && config.mCycle && !config.mPickOnEntry);

    mSitOverrideCheck->setValue(set && set->mSitOverride);
    mSmartSitCheck->setValue(set && set->mSmartSit);
    mNoMouselookCheck->setValue(set && set->mNoStandsInMouselook);
    mNoTypingCheck->setValue(set && set->mNoStandsWhileTyping);
    mSitOverrideCheck->setEnabled(editable);
    mSmartSitCheck->setEnabled(editable && set && set->mSitOverride);
    mNoMouselookCheck->setEnabled(editable);
    mNoTypingCheck->setEnabled(editable);

    mStopSystemCheck->setValue(set && set->mStopSystemAnims);
    mStopSystemCheck->setEnabled(editable);
    mAlwaysCheck->setValue(set && set->mAlwaysEnabled);
    mIdleCheck->setValue(set && set->mIdleEnabled);
    mIdleMinSpin->setValue(set ? set->mIdleMinDelay : 30.f);
    mIdleMaxSpin->setValue(set ? set->mIdleMaxDelay : 90.f);
    mIdleCapSpin->setValue(set ? set->mIdleMaxDuration : 15.f);
    mIdleSitCheck->setValue(set && set->mIdleWhileSitting);
    const bool idle_on = editable && set && set->mIdleEnabled;
    mAlwaysCheck->setEnabled(editable);
    mIdleCheck->setEnabled(editable);
    mIdleMinSpin->setEnabled(idle_on);
    mIdleMaxSpin->setEnabled(idle_on);
    mIdleCapSpin->setEnabled(idle_on);
    mIdleSitCheck->setEnabled(idle_on);

    std::string hint;
    if (!SLAOSets::instance().isInventoryScanned())
    {
        hint = LLTrans::getString("SLAOHintLoading");
    }
    else if (SLAOObjectImport::instance().isBusy())
    {
        hint = SLAOObjectImport::instance().getStatusText();
    }
    else if (SLAOSets::instance().isImporting())
    {
        hint = LLTrans::getString("SLAOHintImporting");
    }
    else if (set && isUnsortedSelected())
    {
        hint = LLTrans::getString("SLAOHintUnsorted");
    }
    else if (!set)
    {
        hint = LLTrans::getString("SLAOHintNoSet");
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[STATE]"] = stateLabel(state);
        hint = LLTrans::getString("SLAOHintDrop", args);
    }
    mHintText->setText(hint);
}

void SLFloaterAOEditor::refreshStatus()
{
    SLAOEngine& engine = SLAOEngine::instance();
    getChild<LLUICtrl>("ao_enabled")->setValue(engine.isEnabled());
    mStatusText->setText(engine.getStatusText());

    const S32 marked = marker_key(mSelectedSet);
    if (marked != mMarkedState)
    {
        refreshStates();
    }
    // Playing / previewing / failed markers: rebuild the list only when one changed
    std::string key = engine.getCurrentAnim().asString() + engine.getPreviewAnim().asString() +
                      llformat("%d", engine.getAlwaysCount());
    if (const std::vector<SLAOAnim>* shown = getShownAnims())
    {
        for (const SLAOAnim& anim : *shown)
        {
            key += engine.isAnimationBad(anim.mAssetID) ? '1' : (engine.isLowPriority(anim.mAssetID) ? '2' : '0');
        }
    }
    if (key != mAnimMarkerKey)
    {
        mAnimMarkerKey = key;
        refreshAnims();
    }
    mPreviewBtn->setLabel(LLTrans::getString(engine.getPreviewAnim().notNull() ? "SLAOStopPreview" : "SLAOPreview"));
    if (SLAOObjectImport::instance().isBusy())
    {
        mHintText->setText(SLAOObjectImport::instance().getStatusText());
    }
    getChild<LLUICtrl>("activate_btn")->setEnabled(getSelectedSetID().notNull() && mSelectedSet != engine.getCurrentSetID());
}

//-----------------------------------------------------------------------------
// Selection
//-----------------------------------------------------------------------------

void SLFloaterAOEditor::onSetSelected()
{
    LLScrollListItem* item = mSetList->getFirstSelected();
    const LLUUID id = item ? item->getUUID() : LLUUID::null;
    if (id == mSelectedSet)
    {
        return;
    }
    mSelectedSet = id;
    SLAOEngine::instance().stopPreview();
    mSetsDirty = true;
}

void SLFloaterAOEditor::onStateSelected()
{
    LLScrollListItem* item = mStateList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const S32 state = item->getValue().asInteger();
    if (state == mSelectedState)
    {
        return;
    }
    mSelectedState = state;
    mAnimList->deselectAllItems(true);
    refreshAnims();
    refreshOptions();
}

void SLFloaterAOEditor::onAnimSelected()
{
    const bool have_anim = getSelectedAnimItem().notNull();
    const bool editable = canEdit();
    const bool unsorted = isUnsortedSelected();
    getChild<LLUICtrl>("up_btn")->setEnabled(have_anim && editable && !unsorted);
    getChild<LLUICtrl>("down_btn")->setEnabled(have_anim && editable && !unsorted);
    getChild<LLUICtrl>("remove_btn")->setEnabled(have_anim && editable && !unsorted);
    getChild<LLUICtrl>("assign_btn")->setVisible(unsorted);
    mAssignCombo->setVisible(unsorted);
    getChild<LLUICtrl>("assign_btn")->setEnabled(have_anim && editable);
    mPreviewBtn->setEnabled(have_anim || SLAOEngine::instance().getPreviewAnim().notNull());
}

//-----------------------------------------------------------------------------
// Set actions
//-----------------------------------------------------------------------------

void SLFloaterAOEditor::onNewSet()
{
    LLHandle<LLFloater> handle = getHandle();
    prompt_name(LLTrans::getString("SLAOPromptNewSet"), LLTrans::getString("SLAODefaultSetName"),
        [handle](const std::string& name)
        {
            SLAOSets::instance().createSet(name, [handle](const LLUUID& set_id)
                {
                    SLFloaterAOEditor* self = static_cast<SLFloaterAOEditor*>(handle.get());
                    if (self && set_id.notNull())
                    {
                        self->mSelectedSet = set_id;
                        self->mSetsDirty = true;
                    }
                    // First set: make it the active one
                    if (set_id.notNull() && SLAOEngine::instance().getCurrentSetID().isNull())
                    {
                        SLAOEngine::instance().selectSet(set_id);
                    }
                });
        });
}

void SLFloaterAOEditor::onRenameSet()
{
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    if (!set)
    {
        return;
    }
    const LLUUID set_id = set->mFolderID;
    LLStringUtil::format_map_t args;
    args["[NAME]"] = set->mName;
    prompt_name(LLTrans::getString("SLAOPromptRenameSet", args), set->mName, [set_id](const std::string& name)
        {
            SLAOSets::instance().renameSet(set_id, name);
        });
}

void SLFloaterAOEditor::onDeleteSet()
{
    const SLAOSet* set = SLAOSets::instance().getSet(mSelectedSet);
    if (!set)
    {
        return;
    }
    const LLUUID set_id = set->mFolderID;
    LLSD args;
    args["NAME"] = set->mName;
    const char* notification = set->mFromOtherViewer ? "SLAODeleteOtherViewerSet"
                             : (set->mHasCopies ? "SLAODeleteSetWithCopies" : "SLAODeleteSet");
    LLNotificationsUtil::add(notification, args, LLSD(),
        [set_id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            if (SLAOEngine::instance().getCurrentSetID() == set_id)
            {
                SLAOEngine::instance().selectSet(LLUUID::null);
            }
            SLAOSets::instance().deleteSet(set_id);
        });
}

void SLFloaterAOEditor::onActivateSet()
{
    const LLUUID set_id = getSelectedSetID();
    if (set_id.notNull())
    {
        SLAOEngine::instance().selectSet(set_id);
        mSetsDirty = true;
    }
}

void SLFloaterAOEditor::onImport()
{
    // Import what is selected in the inventory window; otherwise explain
    // drag and drop.
    bool imported = false;
    LLInventoryPanel* panel = LLInventoryPanel::getActiveInventoryPanel(false);
    LLFolderView* root = panel ? panel->getRootFolder() : nullptr;
    if (root)
    {
        const std::set<LLFolderViewItem*> selection = root->getSelectionList();
        for (LLFolderViewItem* folder_item : selection)
        {
            LLFolderViewModelItemInventory* model =
                folder_item ? static_cast<LLFolderViewModelItemInventory*>(folder_item->getViewModelItem()) : nullptr;
            if (!model)
            {
                continue;
            }
            const LLUUID id = model->getUUID();
            if (const LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                if (item->getType() == LLAssetType::AT_NOTECARD)
                {
                    SLAOSets::instance().importNotecard(id);
                    imported = true;
                }
            }
            else if (gInventory.getCategory(id))
            {
                imported = SLAOSets::instance().importFolder(id) || imported;
            }
        }
    }
    if (!imported)
    {
        LLNotificationsUtil::add("SLAOImportHint");
    }
    mSetsDirty = true;
}

//-----------------------------------------------------------------------------
// Animation actions
//-----------------------------------------------------------------------------

void SLFloaterAOEditor::onMoveAnim(S32 delta)
{
    const LLUUID item_id = getSelectedAnimItem();
    if (item_id.notNull() && canEdit())
    {
        SLAOSets::instance().moveAnimation(mSelectedSet, getSelectedState(), item_id, delta);
    }
}

void SLFloaterAOEditor::onRemoveAnim()
{
    const LLUUID item_id = getSelectedAnimItem();
    if (item_id.notNull() && canEdit())
    {
        SLAOSets::instance().removeAnimation(mSelectedSet, getSelectedState(), item_id);
    }
}

void SLFloaterAOEditor::onPreviewAnim()
{
    SLAOEngine& engine = SLAOEngine::instance();
    const LLUUID item_id = getSelectedAnimItem();
    LLUUID asset_id;
    if (const std::vector<SLAOAnim>* shown = getShownAnims())
    {
        for (const SLAOAnim& anim : *shown)
        {
            if (anim.mItemID == item_id || (anim.mItemID.isNull() && anim.mAssetID == item_id))
            {
                asset_id = anim.mAssetID;
                break;
            }
        }
    }
    if (engine.getPreviewAnim().notNull() && (asset_id.isNull() || asset_id == engine.getPreviewAnim()))
    {
        engine.stopPreview();
    }
    else if (asset_id.notNull())
    {
        engine.previewAnimation(asset_id);
    }
}

void SLFloaterAOEditor::onAssignUnsorted()
{
    const LLUUID item_id = getSelectedAnimItem();
    const S32 state = mAssignCombo->getValue().asInteger();
    if (item_id.isNull() || !canEdit() || state <= STATE_NONE || state >= STATE_COUNT)
    {
        return;
    }
    // Links the copy into the state; it leaves the unsorted list on rescan
    SLAOSets::instance().addAnimations(mSelectedSet, (EState)state, { item_id });
}

//-----------------------------------------------------------------------------
// Options
//-----------------------------------------------------------------------------

void SLFloaterAOEditor::onStateConfigChanged()
{
    const EState state = getSelectedState();
    if (!canEdit() || state == STATE_NONE)
    {
        return;
    }
    SLAOStateConfig config;
    config.mCycle = mCycleCheck->getValue().asBoolean();
    config.mRandom = mRandomCheck->getValue().asBoolean();
    config.mPickOnEntry = mPickOnEntryCheck->getValue().asBoolean();
    config.mInterval = llclamp((F32)mIntervalSpin->getValue().asReal(), 1.f, 3600.f);
    SLAOSets::instance().setStateConfig(mSelectedSet, state, config);
}

void SLFloaterAOEditor::onSetOptionsChanged()
{
    if (!canEdit())
    {
        return;
    }
    SLAOSets::instance().setSetOptions(mSelectedSet,
                                       mSitOverrideCheck->getValue().asBoolean(),
                                       mSmartSitCheck->getValue().asBoolean(),
                                       mNoMouselookCheck->getValue().asBoolean(),
                                       mNoTypingCheck->getValue().asBoolean());
}

void SLFloaterAOEditor::onIdleOptionsChanged()
{
    if (!canEdit())
    {
        return;
    }
    F32 min_delay = (F32)mIdleMinSpin->getValue().asReal();
    F32 max_delay = (F32)mIdleMaxSpin->getValue().asReal();
    if (max_delay < min_delay)
    {
        max_delay = min_delay;
    }
    SLAOSets::instance().setIdleOptions(mSelectedSet, mIdleCheck->getValue().asBoolean(), min_delay, max_delay,
                                        (F32)mIdleCapSpin->getValue().asReal(), mIdleSitCheck->getValue().asBoolean());
}

bool SLFloaterAOEditor::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                          EDragAndDropType cargo_type, void* cargo_data,
                                          EAcceptance* accept, std::string& tooltip_msg)
{
    if (handle_drop(drop, cargo_type, cargo_data, accept, tooltip_msg, getSelectedSetID(), getSelectedState(), canEdit()))
    {
        return true;
    }
    return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
}

//=============================================================================
// SLFloaterAOObjectImport: take animations from a worn HUD or attachment
//=============================================================================

SLFloaterAOObjectImport::SLFloaterAOObjectImport(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterAOObjectImport::~SLFloaterAOObjectImport()
{
    stopListening();
}

bool SLFloaterAOObjectImport::postBuild()
{
    mList = getChild<LLScrollListCtrl>("object_list");
    mStatusText = getChild<LLTextBox>("status_text");
    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { mStatusDirty = true; });
    mList->setDoubleClickCallback([this]() { onImport(); });
    getChild<LLButton>("import_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    getChild<LLButton>("refresh_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { scanAttachments(); });
    mImportConnection = SLAOObjectImport::instance().setChangedCallback([this]() { mStatusDirty = true; });
    return true;
}

void SLFloaterAOObjectImport::onOpen(const LLSD& key)
{
    mPreselect = key.asUUID();
    scanAttachments();
}

void SLFloaterAOObjectImport::onClose(bool app_quitting)
{
    stopListening();
}

void SLFloaterAOObjectImport::stopListening()
{
    for (const auto& [id, entry] : mEntries)
    {
        if (LLViewerObject* object = gObjectList.findObject(id))
        {
            object->removeInventoryListener(this);
        }
    }
}

void SLFloaterAOObjectImport::scanAttachments()
{
    stopListening();
    mEntries.clear();
    if (isAgentAvatarValid())
    {
        for (const auto& [point_id, attachment] : gAgentAvatarp->mAttachmentPoints)
        {
            if (!attachment)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (!object || object->isDead() || mEntries.size() >= 128)
                {
                    continue;
                }
                Entry entry;
                entry.mObjectID = object->getID();
                entry.mHUD = object->isHUDAttachment();
                entry.mPoint = attachment->getName();
                const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID());
                entry.mName = item ? item->getName() : LLTrans::getString("SLAOObjectDefaultName");
                mEntries[entry.mObjectID] = entry;
                // May answer at once if the contents are already known
                const_cast<LLViewerObject*>(object.get())->registerInventoryListener(this, nullptr);
                const_cast<LLViewerObject*>(object.get())->requestInventory();
            }
        }
    }
    mListDirty = true;
    mStatusDirty = true;
}

void SLFloaterAOObjectImport::inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory,
                                               S32 serial_num, void* user_data)
{
    if (!object || !inventory)
    {
        return;
    }
    auto it = mEntries.find(object->getID());
    if (it == mEntries.end())
    {
        return;
    }
    Entry& entry = it->second;
    entry.mAnims = 0;
    entry.mCopyable = 0;
    entry.mNotecards = 0;
    for (const LLPointer<LLInventoryObject>& obj : *inventory)
    {
        const LLInventoryItem* item = dynamic_cast<const LLInventoryItem*>(obj.get());
        if (!item)
        {
            continue;
        }
        if (item->getType() == LLAssetType::AT_ANIMATION)
        {
            ++entry.mAnims;
            entry.mCopyable += SLAOObjectImport::canCopy(item) ? 1 : 0;
        }
        else if (item->getType() == LLAssetType::AT_NOTECARD)
        {
            ++entry.mNotecards;
        }
    }
    mListDirty = true;
}

void SLFloaterAOObjectImport::refreshList()
{
    const LLUUID selected = mList->getCurrentID();
    mList->deleteAllItems();

    std::vector<const Entry*> shown;
    bool loading = false;
    for (const auto& [id, entry] : mEntries)
    {
        if (entry.mAnims < 0)
        {
            loading = true;
        }
        if (entry.mAnims != 0)
        {
            shown.push_back(&entry);
        }
    }
    // HUDs first (AO HUDs live there), then by name
    std::sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b)
        {
            if (a->mHUD != b->mHUD)
            {
                return a->mHUD;
            }
            return LLStringUtil::compareDict(a->mName, b->mName) < 0;
        });

    for (const Entry* entry : shown)
    {
        std::string anims;
        if (entry->mAnims < 0)
        {
            anims = LLTrans::getString("SLAOObjectChecking");
        }
        else if (entry->mCopyable == entry->mAnims)
        {
            anims = llformat("%d", entry->mAnims);
        }
        else
        {
            LLStringUtil::format_map_t args;
            args["[TOTAL]"] = llformat("%d", entry->mAnims);
            args["[NOCOPY]"] = llformat("%d", entry->mAnims - entry->mCopyable);
            anims = LLTrans::getString("SLAOObjectAnimCount", args);
        }
        LLSD row;
        row["value"] = entry->mObjectID;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = entry->mName;
        row["columns"][1]["column"] = "point";
        row["columns"][1]["value"] = entry->mPoint;
        row["columns"][2]["column"] = "anims";
        row["columns"][2]["value"] = anims;
        row["columns"][3]["column"] = "notecard";
        row["columns"][3]["value"] = entry->mNotecards ? LLTrans::getString("SLAOYes") : std::string();
        mList->addElement(row);
    }

    if (mPreselect.notNull() && mList->selectByID(mPreselect))
    {
        mPreselect.setNull();
    }
    else if (selected.notNull())
    {
        mList->selectByID(selected);
    }
    if (shown.empty())
    {
        mList->setCommentText(LLTrans::getString(loading ? "SLAOObjectChecking" : "SLAOObjectNone"));
    }
}

void SLFloaterAOObjectImport::draw()
{
    if (mListDirty)
    {
        mListDirty = false;
        refreshList();
        mStatusDirty = true;
    }
    if (mStatusDirty)
    {
        mStatusDirty = false;
        std::string status = SLAOObjectImport::instance().getStatusText();
        const LLUUID id = mList->getCurrentID();
        auto it = mEntries.find(id);
        const bool can_import = it != mEntries.end() && it->second.mCopyable > 0 && SLAOSets::instance().isInventoryScanned();
        if (status.empty() && it != mEntries.end() && it->second.mAnims > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COPY]"] = llformat("%d", it->second.mCopyable);
            args["[NOCOPY]"] = llformat("%d", it->second.mAnims - it->second.mCopyable);
            status = LLTrans::getString(it->second.mCopyable ? "SLAOObjectSelected" : "SLAOObjectAllNoCopyStatus", args);
        }
        mStatusText->setText(status);
        getChild<LLUICtrl>("import_btn")->setEnabled(can_import);
    }
    LLFloater::draw();
}

void SLFloaterAOObjectImport::onImport()
{
    const LLUUID id = mList->getCurrentID();
    auto it = mEntries.find(id);
    if (it == mEntries.end() || it->second.mCopyable <= 0)
    {
        return;
    }
    SLAOObjectImport::instance().importObject(id, it->second.mName);
    mStatusDirty = true;
    // Show the result where it lands
    LLFloaterReg::showInstance("sl_ao_editor");
}

//=============================================================================
// SLFloaterAOIgnored: animations that never pause the AO
//=============================================================================

SLFloaterAOIgnored::SLFloaterAOIgnored(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterAOIgnored::~SLFloaterAOIgnored()
{
}

bool SLFloaterAOIgnored::postBuild()
{
    mList = getChild<LLScrollListCtrl>("ignored_list");
    getChild<LLButton>("remove_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            const LLUUID id = mList->getCurrentID();
            if (id.notNull())
            {
                SLAOSets::instance().removeIgnoredAnim(id);
            }
        });
    mScriptedList = getChild<LLScrollListCtrl>("scripted_list");
    getChild<LLButton>("forget_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            const LLUUID id = mScriptedList->getCurrentID();
            if (id.notNull())
            {
                // Empty choice erases it, so the prompt can come back
                SLAOSets::instance().setScriptedAOChoice(id, std::string());
            }
        });
    mSetsConnection = SLAOSets::instance().setChangedCallback([this]() { mDirty = true; });
    return true;
}

void SLFloaterAOIgnored::draw()
{
    if (mDirty)
    {
        mDirty = false;
        mList->deleteAllItems();
        for (const auto& [id, label] : SLAOSets::instance().getIgnoredAnims())
        {
            LLSD row;
            row["value"] = id;
            row["columns"][0]["column"] = "label";
            row["columns"][0]["value"] = label;
            row["columns"][1]["column"] = "id";
            row["columns"][1]["value"] = id.asString();
            mList->addElement(row);
        }
        getChild<LLUICtrl>("remove_btn")->setEnabled(mList->getItemCount() > 0);

        const LLUUID selected = mScriptedList->getCurrentID();
        mScriptedList->deleteAllItems();
        for (const auto& [id, choice] : SLAOSets::instance().getScriptedAOChoices())
        {
            const LLViewerInventoryItem* item = gInventory.getItem(id);
            LLSD row;
            row["value"] = id;
            row["columns"][0]["column"] = "name";
            row["columns"][0]["value"] = item ? item->getName() : LLTrans::getString("SLAOUnknownScript") + " (" + id.asString() + ")";
            row["columns"][1]["column"] = "choice";
            row["columns"][1]["value"] = LLTrans::getString(choice == "pause" ? "SLAOChoicePaused" : "SLAOChoiceKeptBoth");
            mScriptedList->addElement(row);
        }
        if (selected.notNull())
        {
            mScriptedList->selectByID(selected);
        }
        getChild<LLUICtrl>("forget_btn")->setEnabled(mScriptedList->getItemCount() > 0);
    }
    LLFloater::draw();
}
