/**
 * @file slfloaterposestudio.cpp
 * @brief Pose Studio (#500): the poser floater.
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

#include "slfloaterposestudio.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcharacter.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcontrolavatar.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llsdserialize.h"
#include "llfloateravatarpicker.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "slbridge.h"
#include "slposedialctrl.h"
#include "slposefigurectrl.h"
#include "slposeik.h"
#include "slposelookat.h"
#include "slposesync.h"

#include <algorithm>

namespace
{
constexpr S32 TARGET_REFRESH_FRAMES = 120;

const char* const DIAL_NAMES[]    = { "dial_x", "dial_y", "dial_z" };
const char* const SPINNER_NAMES[] = { "spin_x", "spin_y", "spin_z" };

std::string targetName(LLVOAvatar* avatar)
{
    if (!avatar)
        return std::string();
    if (avatar->isSelf())
        return LLTrans::getString("SLPoseStudioMyself");
    if (avatar->isControlAvatar())
    {
        LLControlAvatar* control_av = dynamic_cast<LLControlAvatar*>(avatar);
        const LLVOVolume* root_volume = control_av ? control_av->mRootVolp : nullptr;
        const LLViewerObject* root_edit = root_volume ? root_volume->getRootEdit() : nullptr;
        if (root_edit && root_edit->isAttachment())
        {
            if (const LLViewerInventoryItem* item = gInventory.getItem(root_edit->getAttachmentItemID()))
                return item->getName();
        }
        return LLTrans::getString("SLPoseStudioAnimesh") + " " + avatar->getID().asString().substr(0, 8);
    }
    return avatar->getFullname();
}
}

SLFloaterPoseStudio::SLFloaterPoseStudio(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterPoseStudio::~SLFloaterPoseStudio()
{
}

bool SLFloaterPoseStudio::postBuild()
{
    SLPoseStudio& studio = SLPoseStudio::instance();

    mFigure = getChild<SLPoseFigureCtrl>("figure");
    mFigureCombo = getChild<LLComboBox>("figure_combo");
    mViewCombo = getChild<LLComboBox>("view_combo");
    mTargetCombo = getChild<LLComboBox>("target_combo");
    mPoseBtn = getChild<LLButton>("pose_btn");
    mBoneName = getChild<LLTextBox>("bone_name");
    mStatusText = getChild<LLTextBox>("status_text");
    mSnapCombo = getChild<LLComboBox>("snap_combo");
    mMirrorCheck = getChild<LLCheckBoxCtrl>("mirror_check");
    mFreezeCheck = getChild<LLCheckBoxCtrl>("freeze_check");
    mShareCheck = getChild<LLCheckBoxCtrl>("share_check");
    mShowOthersCheck = getChild<LLCheckBoxCtrl>("show_others_check");
    mSyncedCombo = getChild<LLComboBox>("synced_combo");
    mIgnoreBtn = getChild<LLButton>("ignore_btn");
    mPoseList = getChild<LLScrollListCtrl>("pose_list");
    mPoseNameEditor = getChild<LLLineEditor>("pose_name");
    mSequenceList = getChild<LLScrollListCtrl>("sequence_list");
    mHoldSpinner = getChild<LLSpinCtrl>("hold_spin");
    mPlayBtn = getChild<LLButton>("play_btn");
    mStopBtn = getChild<LLButton>("stop_btn");
    mLoopCheck = getChild<LLCheckBoxCtrl>("loop_check");

    for (S32 i = 0; i < DIAL_COUNT; ++i)
    {
        const EDial dial = (EDial)i;
        mDials[i] = getChild<SLPoseDialCtrl>(DIAL_NAMES[i]);
        mDialSpinners[i] = getChild<LLSpinCtrl>(SPINNER_NAMES[i]);
        mDials[i]->setCommitCallback([this, dial](LLUICtrl*, const LLSD&) { onDialChanged(dial); });
        mDials[i]->setReleaseCallback([this, dial](SLPoseDialCtrl*) { onDialReleased(dial); });
        mDialSpinners[i]->setCommitCallback([this, dial](LLUICtrl*, const LLSD&) { onSpinnerChanged(dial); });
    }
    const char* const pos_names[] = { "pos_x", "pos_y", "pos_z" };
    const char* const scale_names[] = { "scale_x", "scale_y", "scale_z" };
    for (S32 i = 0; i < 3; ++i)
    {
        mPosSpinners[i] = getChild<LLSpinCtrl>(pos_names[i]);
        mPosSpinners[i]->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPositionChanged(); });
        mScaleSpinners[i] = getChild<LLSpinCtrl>(scale_names[i]);
        mScaleSpinners[i]->setCommitCallback([this](LLUICtrl*, const LLSD&) { onScaleChanged(); });
    }

    mFigure->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBoneSelected(); });
    mFigure->setStateFunc([this](const std::string& joint_name) -> SLPoseFigureCtrl::EBoneState
        {
            LLVOAvatar* avatar = getTarget();
            const SLPoseStudio::joint_t* joint = SLPoseStudio::instance().findJoint(joint_name);
            if (!avatar || !joint || !SLPoseStudio::instance().avatarHasJoint(avatar, *joint))
                return SLPoseFigureCtrl::BONE_MISSING;
            return SLPoseStudio::instance().isBoneChanged(avatar, *joint) ? SLPoseFigureCtrl::BONE_CHANGED
                                                                           : SLPoseFigureCtrl::BONE_NORMAL;
        });
    mViewCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onViewSelected(); });
    mFigureCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFigureSelected(); });
    mTargetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTargetSelected(); });
    mPoseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPoseToggle(); });
    mSnapCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSnapChanged(); });
    mMirrorCheck->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { SLPoseStudio::instance().setMirror(ctrl->getValue().asBoolean()); });
    mFreezeCheck->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { SLPoseStudio::instance().setFreeze(ctrl->getValue().asBoolean()); });
    mShareCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShareToggled(); });
    mShowOthersCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShowOthersToggled(); });
    mIgnoreBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIgnoreToggled(); });
    mSyncedCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { mSyncDirty = true; });
    mLoopCheck->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { SLPoseStudio::instance().setLoop(ctrl->getValue().asBoolean()); });

    getChild<LLButton>("reset_bone_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetBone(); });
    getChild<LLButton>("reset_all_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetAll(); });
    getChild<LLButton>("undo_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    getChild<LLButton>("redo_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRedo(); });
    // IK handles (#530): the checkbox is bound to SLPoseStudioIKHandles in the XUI.
    SLPoseIK::instance();
    getChild<LLButton>("ik_unpin_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseIK::instance().clearPins(); });
    getChild<LLButton>("ik_snap_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseIK::instance().snapToBones(); });
    getChild<LLButton>("release_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRelease(); });
    getChild<LLButton>("export_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExport(); });
    getChild<LLButton>("classic_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("fs_poser"); });
    getChild<LLButton>("creator_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("sl_pose_creator"); });

    getChild<LLButton>("save_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSavePose(); });
    getChild<LLButton>("load_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLoadPose(); });
    getChild<LLButton>("delete_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeletePose(); });
    getChild<LLButton>("add_seq_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddToSequence(); });
    getChild<LLButton>("seq_remove_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSequenceRemove(); });
    getChild<LLButton>("seq_up_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSequenceMove(-1); });
    getChild<LLButton>("seq_down_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSequenceMove(1); });
    getChild<LLButton>("prev_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseStudio::instance().stepSequence(-1); });
    getChild<LLButton>("next_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseStudio::instance().stepSequence(1); });
    mPlayBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseStudio::instance().play(); });
    mStopBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { SLPoseStudio::instance().stopPlayback(); });
    mHoldSpinner->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSequenceHoldChanged(); });
    getChild<LLUICtrl>("seq_ease_spin")->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        {
            const S32 index = mSequenceList->getFirstSelectedIndex();
            if (index >= 0)
                SLPoseStudio::instance().setSequenceEase(index, (F32)ctrl->getValue().asReal());
        });
    mSequenceList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSequenceSelected(); });
    mPoseList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPoseSelected(); });
    mPoseList->setDoubleClickCallback([this]() { onLoadPose(); });

    // Posing other people: only after they accept a request (SLPoseSync verifies it).
    getChild<LLButton>("pose_other_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPoseOther(); });
    getChild<LLButton>("stop_posed_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStopBeingPosed(); });

    mChangedConnection = studio.setChangedCallback([this]()
        {
            mRefreshPending = true;
            mPoseListDirty = true;
            mSequenceDirty = true;
        });
    mEditedConnection = studio.setEditedCallback([this](const LLUUID&, bool)
        {
            // Our own dial drags already show their values; anything else (load, reset,
            // mirror partner, sync) re-reads the bone.
            if (!mApplyingDials)
                mRefreshPending = true;
        });

    mSyncConnection = SLPoseSync::instance().setChangedCallback([this]() { mSyncDirty = true; mRefreshPending = true; });

    // Look-at sync (#902): the checkboxes and spinners are bound to their settings in the XUI.
    mLookAtMapCombo = getChild<LLComboBox>("lookat_map_combo");
    mLookAtPeopleCombo = getChild<LLComboBox>("lookat_people_combo");
    mLookAtMuteBtn = getChild<LLButton>("lookat_mute_btn");
    mLookAtStatusText = getChild<LLTextBox>("lookat_status_text");
    mLookAtSendCheck = getChild<LLCheckBoxCtrl>("lookat_send_check");
    mLookAtMapCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLookAtMapSelected(); });
    mLookAtPeopleCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { mLookAtDirty = true; });
    mLookAtMuteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLookAtMuteToggled(); });
    mLookAtConnection = SLPoseLookAt::instance().setChangedCallback([this]() { mLookAtDirty = true; });

    refreshFigureList();
    const std::string figure = gSavedSettings.getString("SLPoseStudioFigure");
    if (!figure.empty() && figure != "figure_human.xml" && !mFigure->loadFigure(figure))
        mFigure->loadFigure("figure_human.xml");
    mFigureCombo->selectByValue(LLSD(figure));
    refreshFigureViews();
    mSnapCombo->selectByValue(LLSD(llformat("%d", (S32)gSavedSettings.getF32("SLPoseStudioSnapDegrees"))));
    onSnapChanged();
    return true;
}

void SLFloaterPoseStudio::onOpen(const LLSD& key)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    studio.createUserPoseDirectoryIfNeeded();
    refreshTargets();

    static LLCachedControl<bool> auto_start(gSavedSettings, "SLPoseStudioAutoStart", true);
    if (auto_start)
    {
        LLVOAvatar* avatar = getTarget();
        if (avatar && !studio.isPosing(avatar))
            studio.startPosing(avatar);
    }

    if (mFigure->getValue().asString().empty())
    {
        mFigure->setValue("mHead");
        onBoneSelected();
    }
    mRefreshPending = mPoseListDirty = mSequenceDirty = true;
}

void SLFloaterPoseStudio::onClose(bool app_quitting)
{
    if (!app_quitting && SLPoseIK::instanceExists())
        SLPoseIK::instance().reset();   // pins, poles and drag history end with the studio
    if (!app_quitting && gSavedSettings.getBOOL("SLPoseStudioStopOnClose"))
        SLPoseStudio::instance().stopAll();
    LLFloater::onClose(app_quitting);
}

void SLFloaterPoseStudio::draw()
{
    if (--mTargetRefreshCountdown <= 0)
    {
        mTargetRefreshCountdown = TARGET_REFRESH_FRAMES;
        refreshTargets();
    }
    if (mPoseListDirty)
        refreshPoseList();
    if (mSequenceDirty)
        refreshSequence();
    if (mSyncDirty)
    {
        mSyncDirty = false;
        refreshSync();
    }
    if (mLookAtDirty)
    {
        mLookAtDirty = false;
        refreshLookAt();
    }
    if (--mLookAtStatusCountdown <= 0)
    {
        mLookAtStatusCountdown = TARGET_REFRESH_FRAMES;
        refreshLookAtStatus();
    }
    if (mRefreshPending)
    {
        mRefreshPending = false;
        refreshPosingState();
        refreshBoneControls();
    }
    LLFloater::draw();
}

LLVOAvatar* SLFloaterPoseStudio::getTarget() const
{
    return SLPoseStudio::instance().getTarget();
}

const SLPoseStudio::joint_t* SLFloaterPoseStudio::getSelectedJoint() const
{
    return SLPoseStudio::instance().findJoint(mFigure->getValue().asString());
}

const SLPoseStudio::joint_t* SLFloaterPoseStudio::getControlsJoint() const
{
    return mControlsJoint.empty() ? nullptr : SLPoseStudio::instance().findJoint(mControlsJoint);
}

// ---------------------------------------------------------------------------
// Target / posing
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::refreshTargets()
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const std::vector<LLUUID> ids = studio.getPoseableNearby();
    const LLUUID selected = studio.getTargetID().isNull() ? gAgentID : studio.getTargetID();

    // Rebuild only when something changed, so an open dropdown is not yanked away.
    std::vector<std::pair<LLUUID, std::string>> entries;
    bool found = false;
    for (const LLUUID& id : ids)
    {
        LLVOAvatar* avatar = nullptr;
        if (id == gAgentID)
        {
            avatar = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
        }
        else
        {
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                if (character && character->getID() == id)
                {
                    avatar = dynamic_cast<LLVOAvatar*>(character);
                    break;
                }
            }
        }
        if (!avatar)
            continue;
        std::string name = targetName(avatar);
        if (studio.isPosing(avatar))
            name += " *";
        entries.emplace_back(id, name);
        found |= (id == selected);
    }
    if (entries != mTargetEntries)
    {
        mTargetEntries = entries;
        mTargetCombo->removeall();
        for (const auto& entry : entries)
            mTargetCombo->add(entry.second, LLSD(entry.first));
    }
    if (found)
    {
        mTargetCombo->selectByValue(LLSD(selected));
    }
    else
    {
        // The target went away (derezzed, out of range): fall back to self.
        studio.setTarget(gAgentID);
        mTargetCombo->selectByValue(LLSD(gAgentID));
    }
}

void SLFloaterPoseStudio::onTargetSelected()
{
    SLPoseStudio::instance().setTarget(mTargetCombo->getValue().asUUID());
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onPoseToggle()
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* avatar = getTarget();
    if (!avatar)
        return;
    if (studio.isPosing(avatar))
        studio.stopPosing(avatar);
    else
        studio.startPosing(avatar);
    mTargetRefreshCountdown = 0;
}

void SLFloaterPoseStudio::onRelease()
{
    SLPoseStudio::instance().stopAll();
    mTargetRefreshCountdown = 0;
}

void SLFloaterPoseStudio::refreshPosingState()
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* avatar = getTarget();
    const bool posing = studio.isPosing(avatar);

    mPoseBtn->setLabel(getString(posing ? "pose_stop_label" : "pose_start_label"));
    mPoseBtn->setToggleState(posing);
    mPoseBtn->setEnabled(avatar != nullptr && studio.canPose(avatar));

    mMirrorCheck->setValue(studio.getMirror());
    mFreezeCheck->setValue(studio.getFreeze());
    mLoopCheck->setValue(studio.getLoop());

    for (S32 i = 0; i < DIAL_COUNT; ++i)
    {
        mDials[i]->setEnabled(posing);
        mDialSpinners[i]->setEnabled(posing);
    }
    for (S32 i = 0; i < 3; ++i)
    {
        mPosSpinners[i]->setEnabled(posing);
        mScaleSpinners[i]->setEnabled(posing);
    }
    getChild<LLUICtrl>("reset_bone_btn")->setEnabled(posing);
    getChild<LLUICtrl>("reset_all_btn")->setEnabled(posing);
    getChild<LLUICtrl>("undo_btn")->setEnabled(posing);
    getChild<LLUICtrl>("redo_btn")->setEnabled(posing);
    getChild<LLUICtrl>("save_btn")->setEnabled(posing);
    getChild<LLUICtrl>("save_ease_spin")->setEnabled(posing);

    mPlayBtn->setEnabled(!studio.getSequence().empty() && !studio.isPlaying());
    mStopBtn->setEnabled(studio.isPlaying());

    std::string status;
    if (!avatar)
        status = getString("status_no_target");
    else if (!posing)
        status = getString("status_not_posing");
    else if (studio.isPlaying())
        status = getString("status_playing");
    else
        status = getString("status_posing");

    const S32 posers = (S32)SLPoseSync::instance().getPosers().size();
    getChild<LLUICtrl>("stop_posed_btn")->setVisible(posers > 0);
    if (posers > 0)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", posers);
        status += " " + getString("status_posed_by", args);
    }

    // Bridge state, while sharing or showing others' poses.
    if (SLBridge::instanceExists() && SLBridge::instance().isWanted())
    {
        const SLBridge& bridge = SLBridge::instance();
        std::string line;
        if (bridge.getState() == SLBridge::STATE_READY)
        {
            line = getString("bridge_ready");
        }
        else if (bridge.getState() == SLBridge::STATE_UNAVAILABLE)
        {
            LLStringUtil::format_map_t args;
            args["[REASON]"] = bridge.getReason();
            line = getString("bridge_unavailable", args);
        }
        else
        {
            line = getString("bridge_starting");
        }
        status += " " + line;
    }
    mStatusText->setText(status);
}

// ---------------------------------------------------------------------------
// Figure / bone selection
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::refreshFigureViews()
{
    mViewCombo->removeall();
    for (const SLPoseFigureCtrl::View& view : mFigure->getViews())
        mViewCombo->add(view.mLabel, LLSD(view.mId));
    mViewCombo->selectByValue(LLSD(mFigure->getViewId()));
}

void SLFloaterPoseStudio::refreshFigureList()
{
    // Every figure_*.xml in app_settings/posestudio, by its display name.
    mFigureCombo->removeall();
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "posestudio", "");
    std::string file;
    LLDirIterator iter(dir, "figure_*.xml");
    std::vector<std::pair<std::string, std::string>> figures;
    while (iter.next(file))
    {
        std::string name = file;
        llifstream infile(gDirUtilp->add(dir, file).c_str());
        LLSD data;
        if (infile.is_open() && LLSDSerialize::fromXML(data, infile) != LLSDParser::PARSE_FAILURE && data.has("name"))
            name = data["name"].asString();
        figures.emplace_back(name, file);
    }
    std::sort(figures.begin(), figures.end());
    for (const auto& [name, file_name] : figures)
        mFigureCombo->add(name, LLSD(file_name));
}

void SLFloaterPoseStudio::onFigureSelected()
{
    const std::string file = mFigureCombo->getValue().asString();
    if (file.empty() || !mFigure->loadFigure(file))
        return;
    gSavedSettings.setString("SLPoseStudioFigure", file);
    refreshFigureViews();
    mFigure->setValue("mHead");
    onBoneSelected();
}

void SLFloaterPoseStudio::onViewSelected()
{
    mFigure->setView(mViewCombo->getValue().asString());
}

void SLFloaterPoseStudio::onBoneSelected()
{
    // A spinner still holding the keyboard focus shows the previous bone's value (LLSpinCtrl
    // does not update a focused editor) and would commit it later, to whichever bone is
    // selected then. Finish that edit now, on the bone the controls still show.
    for (S32 i = 0; i < 3; ++i)
    {
        LLSpinCtrl* spinners[] = { mDialSpinners[i], mPosSpinners[i], mScaleSpinners[i] };
        for (LLSpinCtrl* spinner : spinners)
        {
            if (spinner && spinner->hasFocus())
                gFocusMgr.setKeyboardFocus(nullptr);
        }
    }
    mFigure->showJoint(mFigure->getValue().asString());
    mViewCombo->selectByValue(LLSD(mFigure->getViewId()));
    mRefreshPending = true;
}

void SLFloaterPoseStudio::refreshBoneControls()
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* avatar = getTarget();
    const SLPoseStudio::joint_t* joint = getSelectedJoint();

    std::string label = joint ? joint->jointName() : std::string();
    for (const SLPoseFigureCtrl::View& view : mFigure->getViews())
    {
        for (const SLPoseFigureCtrl::Bone& bone : view.mBones)
        {
            if (joint && bone.mJoint == joint->jointName())
                label = bone.mLabel;
        }
    }
    mBoneName->setText(label);

    // Per-bone dial ranges (the HUD's), in HUD numbers.
    const SLBoneModel& model = studio.getBoneModel(joint ? joint->jointName() : std::string());
    for (S32 i = 0; i < DIAL_COUNT; ++i)
    {
        mDials[i]->setRange(model.mMin.mV[i], model.mMax.mV[i]);
        mDialSpinners[i]->setMinValue(model.mMin.mV[i]);
        mDialSpinners[i]->setMaxValue(model.mMax.mV[i]);
    }
    applyDialSettings();

    // The controls now show this bone: their edits go to it (see getControlsJoint).
    mControlsJoint = joint ? joint->jointName() : std::string();

    // Edited bone: the dial values the studio remembers for it -- exactly what was dialled,
    // as long as nothing else has rotated the bone since (see SLPoseStudio::getBoneAngles).
    // An unedited bone shows dial zero (the HUD's rest), not a decode of whatever the
    // current animation does with it -- that is generally outside the bone's dial range
    // and clamped every dial to its end. The first dial touch then replaces the animated
    // rotation, exactly as playing a HUD animation does.
    // (setValue on the dials and spinners does not commit, so nothing is written back.)
    SLPoseAngles angles;
    if (!avatar || !joint || !studio.isBoneEdited(avatar->getID(), joint->jointName()) || !studio.getBoneAngles(avatar, *joint, angles))
        angles = SLPoseAngles();
    for (S32 i = 0; i < DIAL_COUNT; ++i)
    {
        if (!mDials[i]->isDragging())
            mDials[i]->setValue(angles.get(i));
        mDialSpinners[i]->setValue(angles.get(i));
    }

    const LLVector3 pos = (avatar && joint) ? studio.getBonePosition(avatar, *joint) : LLVector3::zero;
    const LLVector3 scale = (avatar && joint) ? studio.getBoneScale(avatar, *joint) : LLVector3::zero;
    for (S32 i = 0; i < 3; ++i)
    {
        mPosSpinners[i]->setValue(pos.mV[i]);
        mScaleSpinners[i]->setValue(scale.mV[i]);
    }
}

// ---------------------------------------------------------------------------
// Dials
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::applyAxis(EDial dial, F32 dial_degrees, bool committed)
{
    LLVOAvatar* avatar = getTarget();
    const SLPoseStudio::joint_t* joint = getControlsJoint();
    if (!avatar || !joint)
        return;

    // The bone's remembered dial values (an unedited bone: dial zero, the HUD's rest). Only
    // the axis being turned changes, so the other two are never clamped or re-derived under
    // the user's hand.
    SLPoseStudio& studio = SLPoseStudio::instance();
    SLPoseAngles angles;
    if (!studio.getRememberedAngles(avatar->getID(), joint->jointName(), angles)
        && (!studio.isBoneEdited(avatar->getID(), joint->jointName()) || !studio.getBoneAngles(avatar, *joint, angles)))
        angles = SLPoseAngles();
    angles.set(dial, dial_degrees);

    mApplyingDials = true;
    studio.setBoneAngles(avatar, *joint, angles, committed);
    mApplyingDials = false;
}

void SLFloaterPoseStudio::onDialChanged(EDial dial)
{
    mDialSpinners[dial]->setValue(mDials[dial]->getValueF32());
    applyAxis(dial, mDials[dial]->getValueF32(), !mDials[dial]->isDragging());
}

void SLFloaterPoseStudio::onDialReleased(EDial dial)
{
    // Preview while dragging, send on release.
    applyAxis(dial, mDials[dial]->getValueF32(), true);
}

void SLFloaterPoseStudio::onSpinnerChanged(EDial dial)
{
    mDials[dial]->setValue(mDialSpinners[dial]->getValue());
    applyAxis(dial, mDials[dial]->getValueF32(), true);
}

void SLFloaterPoseStudio::onSnapChanged()
{
    gSavedSettings.setF32("SLPoseStudioSnapDegrees", (F32)mSnapCombo->getValue().asReal());
    applyDialSettings();
}

void SLFloaterPoseStudio::applyDialSettings()
{
    // Snap: off (0, the default: smooth, full precision), a fixed step, or the HUD's own
    // per-bone steps (-1).
    const F32 snap = (F32)mSnapCombo->getValue().asReal();
    const SLPoseStudio::joint_t* joint = getSelectedJoint();
    const SLBoneModel& model = SLPoseStudio::instance().getBoneModel(joint ? joint->jointName() : std::string());
    for (S32 i = 0; i < DIAL_COUNT; ++i)
    {
        const F32 step = (snap < 0.f) ? model.mSteps.mV[i] : snap;
        mDials[i]->setSnap(step);
        mDialSpinners[i]->setIncrement(step > 0.f ? step : 1.f);
    }
}

void SLFloaterPoseStudio::onPositionChanged()
{
    LLVOAvatar* avatar = getTarget();
    const SLPoseStudio::joint_t* joint = getControlsJoint();
    if (!avatar || !joint)
        return;
    const LLVector3 pos((F32)mPosSpinners[0]->getValue().asReal(), (F32)mPosSpinners[1]->getValue().asReal(),
                        (F32)mPosSpinners[2]->getValue().asReal());
    SLPoseStudio::instance().setBonePosition(avatar, *joint, pos);
}

void SLFloaterPoseStudio::onScaleChanged()
{
    LLVOAvatar* avatar = getTarget();
    const SLPoseStudio::joint_t* joint = getControlsJoint();
    if (!avatar || !joint)
        return;
    const LLVector3 scale((F32)mScaleSpinners[0]->getValue().asReal(), (F32)mScaleSpinners[1]->getValue().asReal(),
                          (F32)mScaleSpinners[2]->getValue().asReal());
    SLPoseStudio::instance().setBoneScale(avatar, *joint, scale);
}

// ---------------------------------------------------------------------------
// Side tools
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::onResetBone()
{
    LLVOAvatar* avatar = getTarget();
    const SLPoseStudio::joint_t* joint = getSelectedJoint();
    if (avatar && joint)
        SLPoseStudio::instance().resetBone(avatar, *joint);
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onResetAll()
{
    SLPoseStudio::instance().resetAll(getTarget());
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onUndo()
{
    LLVOAvatar* avatar = getTarget();
    // The last edits were IK handle drags: undo the whole drag (every bone it moved).
    if (SLPoseIK::instance().canUndo(avatar))
    {
        SLPoseIK::instance().undo(avatar);
        mRefreshPending = true;
        return;
    }
    const SLPoseStudio::joint_t* joint = getSelectedJoint();
    if (avatar && joint)
        SLPoseStudio::instance().undoBone(avatar, *joint);
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onRedo()
{
    LLVOAvatar* avatar = getTarget();
    if (SLPoseIK::instance().canRedo(avatar))
    {
        SLPoseIK::instance().redo(avatar);
        mRefreshPending = true;
        return;
    }
    const SLPoseStudio::joint_t* joint = getSelectedJoint();
    if (avatar && joint)
        SLPoseStudio::instance().redoBone(avatar, *joint);
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onExport()
{
    // Pose files are plain XML in the poses folder; sharing a file is copying it.
    SLPoseStudio::instance().createUserPoseDirectoryIfNeeded();
    gDirUtilp->openDir(SLPoseStudio::getPoseDirectory());
}

void SLFloaterPoseStudio::onShareToggled()
{
    SLPoseSync::instance().setSharing(mShareCheck->getValue().asBoolean());
}

void SLFloaterPoseStudio::onShowOthersToggled()
{
    SLPoseSync::instance().setShowingOthers(mShowOthersCheck->getValue().asBoolean());
}

void SLFloaterPoseStudio::onPoseOther()
{
    LLHandle<LLFloater> handle = getHandle();
    LLFloater* picker = LLFloaterAvatarPicker::show([handle](const uuid_vec_t& ids, const std::vector<LLAvatarName>&)
        {
            SLFloaterPoseStudio* self = dynamic_cast<SLFloaterPoseStudio*>(handle.get());
            if (self && !ids.empty())
                self->onPoseOtherPicked(ids.front());
        }, false, true, true, getName());
    if (picker)
        addDependentFloater(picker);
}

void SLFloaterPoseStudio::onPoseOtherPicked(const LLUUID& target)
{
    std::string reason;
    if (SLPoseSync::instance().requestPose(target, reason))
    {
        mStatusText->setText(getString("status_asked"));
        return;
    }
    const std::string key = "reason_" + reason;
    LLSD args;
    args["REASON"] = hasString(key) ? getString(key) : reason;
    LLNotificationsUtil::add("SLPoseStudioRequestFailed", args);
}

void SLFloaterPoseStudio::onStopBeingPosed()
{
    SLPoseSync::instance().stopBeingPosed();
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onIgnoreToggled()
{
    const LLUUID id = mSyncedCombo->getValue().asUUID();
    if (id.isNull())
        return;
    SLPoseSync& sync = SLPoseSync::instance();
    if (sync.isRequestBlocked(id))
        sync.setRequestBlocked(id, false);      // "Allow requests" for a blocked requester
    else
        sync.setIgnored(id, !sync.isIgnored(id));
    mSyncDirty = true;
}

void SLFloaterPoseStudio::refreshSync()
{
    SLPoseSync& sync = SLPoseSync::instance();
    mShareCheck->setValue(sync.isSharing());
    mShowOthersCheck->setValue(sync.isShowingOthers());

    // Currently synced people plus the ignored ones, so an ignore can be undone.
    const LLUUID selected = mSyncedCombo->getValue().asUUID();
    std::set<LLUUID> ids;
    for (const LLUUID& id : sync.getSyncedAvatars())
        ids.insert(id);
    for (const LLUUID& id : sync.getIgnoredList())
        ids.insert(id);
    for (const LLUUID& id : sync.getBlockedRequesters())
        ids.insert(id);

    mSyncedCombo->removeall();
    for (const LLUUID& id : ids)
    {
        LLAvatarName av_name;
        std::string name = LLAvatarNameCache::get(id, &av_name) ? av_name.getCompleteName() : id.asString();
        if (sync.isRequestBlocked(id))
            name += " (" + getString("blocked_suffix") + ")";
        else if (sync.isIgnored(id))
            name += " (" + getString("ignored_suffix") + ")";
        mSyncedCombo->add(name, LLSD(id));
    }
    if (ids.empty())
        mSyncedCombo->setLabel(getString("no_synced"));
    else if (selected.notNull() && ids.count(selected))
        mSyncedCombo->selectByValue(LLSD(selected));
    else
        mSyncedCombo->selectFirstItem();

    const LLUUID current = mSyncedCombo->getValue().asUUID();
    mIgnoreBtn->setEnabled(current.notNull());
    mIgnoreBtn->setLabel(getString(sync.isRequestBlocked(current) ? "unblock_label"
                                                                  : (sync.isIgnored(current) ? "unignore_label" : "ignore_label")));
}

// ---------------------------------------------------------------------------
// Look-at sync (#902)
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::onLookAtMapSelected()
{
    const std::string id = mLookAtMapCombo->getValue().asString();
    gSavedSettings.setString("SLLookAtSyncMap", id.empty() ? std::string("auto") : id);
    mLookAtDirty = true;
}

void SLFloaterPoseStudio::onLookAtMuteToggled()
{
    const LLUUID id = mLookAtPeopleCombo->getValue().asUUID();
    if (id.isNull())
        return;
    SLPoseLookAt& lookat = SLPoseLookAt::instance();
    lookat.setMuted(id, !lookat.isMuted(id));
    mLookAtDirty = true;
}

void SLFloaterPoseStudio::refreshLookAt()
{
    SLPoseLookAt& lookat = SLPoseLookAt::instance();

    // Body map: Auto (showing what it found) plus every preset.
    const std::string setting = gSavedSettings.getString("SLLookAtSyncMap");
    mLookAtMapCombo->removeall();
    const SLLookAtMap* detected = lookat.findMap(lookat.getDetectedMapId());
    LLStringUtil::format_map_t args;
    args["[MAP]"] = detected ? detected->mLabel : std::string("Human");
    mLookAtMapCombo->add(getString("lookat_auto", args), LLSD("auto"));
    bool known = false;
    for (const SLLookAtMap& map : lookat.getMaps())
    {
        mLookAtMapCombo->add(map.mLabel, LLSD(map.mId));
        known |= (map.mId == setting);
    }
    mLookAtMapCombo->selectByValue(LLSD(known ? setting : std::string("auto")));

    // People whose look-at we show, plus the muted ones so a mute can be undone.
    const LLUUID selected = mLookAtPeopleCombo->getValue().asUUID();
    std::set<LLUUID> ids;
    for (const LLUUID& id : lookat.getSenders())
        ids.insert(id);
    for (const LLUUID& id : lookat.getMutedList())
        ids.insert(id);
    mLookAtPeopleCombo->removeall();
    for (const LLUUID& id : ids)
    {
        LLAvatarName av_name;
        std::string name = LLAvatarNameCache::get(id, &av_name) ? av_name.getCompleteName() : id.asString();
        if (lookat.isMuted(id))
            name += " (" + getString("lookat_muted_suffix") + ")";
        mLookAtPeopleCombo->add(name, LLSD(id));
    }
    if (ids.empty())
        mLookAtPeopleCombo->setLabel(getString("lookat_none"));
    else if (selected.notNull() && ids.count(selected))
        mLookAtPeopleCombo->selectByValue(LLSD(selected));
    else
        mLookAtPeopleCombo->selectFirstItem();

    const LLUUID current = mLookAtPeopleCombo->getValue().asUUID();
    mLookAtMuteBtn->setEnabled(current.notNull());
    mLookAtMuteBtn->setLabel(getString(lookat.isMuted(current) ? "lookat_unmute_label" : "lookat_mute_label"));
}

void SLFloaterPoseStudio::refreshLookAtStatus()
{
    // Never silently nothing (#902): say why "Send where I look" is not sending. The privacy
    // block has its own note under the check box (shown by the setting itself).
    // "Send where I look" is greyed out while the privacy option blocks it, but only while it
    // is off: one already on must stay switchable off from here.
    if (mLookAtSendCheck)
    {
        static LLCachedControl<bool> private_lookat(gSavedSettings, "EnableLookAtTarget", false);
        static LLCachedControl<bool> send(gSavedSettings, "SLLookAtSyncSend", false);
        mLookAtSendCheck->setEnabled(!(bool)private_lookat || (bool)send);
    }
    if (!mLookAtStatusText || !SLPoseLookAt::instanceExists())
        return;
    SLPoseLookAt& lookat = SLPoseLookAt::instance();
    std::string key;
    switch (lookat.getBlockReason())
    {
    case SLPoseLookAt::BLOCK_NONE:            key = "lookat_status_sending"; break;
    case SLPoseLookAt::BLOCK_RLV:             key = "lookat_status_rlv"; break;
    case SLPoseLookAt::BLOCK_BRIDGE:          key = "lookat_status_helper"; break;
    case SLPoseLookAt::BLOCK_LOOKAT_DISABLED: key = "lookat_status_disabled"; break;
    case SLPoseLookAt::BLOCK_AO:              key = "lookat_status_ao"; break;
    case SLPoseLookAt::BLOCK_NO_TARGET:       key = "lookat_status_no_target"; break;
    default:                                  break;     // off, privacy: nothing to add
    }
    std::string text;
    if (!key.empty())
    {
        const SLLookAtMap* map = lookat.findMap(lookat.getOwnMapId());
        LLStringUtil::format_map_t args;
        args["[MAP]"] = map ? map->mLabel : lookat.getOwnMapId();
        text = getString(key, args);
    }
    if (mLookAtStatusText->getText() != text)
        mLookAtStatusText->setText(text);
}

// ---------------------------------------------------------------------------
// Pose library
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::refreshPoseList()
{
    mPoseListDirty = false;
    const std::string selected = mPoseList->getSelectedValue().asString();
    mPoseList->deleteAllItems();
    for (const std::string& name : SLPoseStudio::instance().listPoses())
    {
        LLSD row;
        row["value"] = name;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;
        mPoseList->addElement(row);
    }
    if (!selected.empty())
        mPoseList->selectByValue(LLSD(selected));
}

void SLFloaterPoseStudio::onPoseSelected()
{
    const std::string name = mPoseList->getSelectedValue().asString();
    if (!name.empty())
    {
        mPoseNameEditor->setText(name);
        getChild<LLUICtrl>("save_ease_spin")->setValue(SLPoseStudio::instance().getPoseEaseIn(name));
    }
}

void SLFloaterPoseStudio::onSavePose()
{
    std::string name = mPoseNameEditor->getText();
    LLStringUtil::trim(name);
    LLVOAvatar* avatar = getTarget();
    if (name.empty() || !avatar)
        return;
    // Keep the name a plain file name.
    if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
    {
        LLNotificationsUtil::add("SLPoseStudioBadName");
        return;
    }

    if (SLPoseStudio::instance().poseExists(name))
    {
        LLSD args;
        args["POSE_NAME"] = name;
        LLNotificationsUtil::add("SLPoseStudioConfirmOverwrite", args, LLSD(),
            [this, name](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                    doSavePose(name);
            });
        return;
    }
    doSavePose(name);
}

void SLFloaterPoseStudio::doSavePose(const std::string& name)
{
    const F32 ease = (F32)getChild<LLUICtrl>("save_ease_spin")->getValue().asReal();
    if (!SLPoseStudio::instance().savePose(getTarget(), name, ease))
    {
        LLSD args;
        args["POSE_NAME"] = name;
        LLNotificationsUtil::add("SLPoseStudioSaveFailed", args);
    }
}

void SLFloaterPoseStudio::onLoadPose()
{
    const std::string name = mPoseList->getSelectedValue().asString();
    if (name.empty())
        return;
    SLPoseStudio::instance().stopPlayback();
    SLPoseStudio::instance().loadPoseRetrying(name);
    mRefreshPending = true;
}

void SLFloaterPoseStudio::onDeletePose()
{
    const std::string name = mPoseList->getSelectedValue().asString();
    if (name.empty())
        return;
    LLSD args;
    args["POSE_NAME"] = name;
    LLNotificationsUtil::add("SLPoseStudioConfirmDelete", args, LLSD(),
        [name](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                SLPoseStudio::instance().deletePose(name);
        });
}

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------

void SLFloaterPoseStudio::refreshSequence()
{
    mSequenceDirty = false;
    SLPoseStudio& studio = SLPoseStudio::instance();
    const S32 selected = mSequenceList->getFirstSelectedIndex();
    mSequenceList->deleteAllItems();

    const auto& sequence = studio.getSequence();
    for (S32 i = 0; i < (S32)sequence.size(); ++i)
    {
        LLSD row;
        row["value"] = i;
        row["columns"][0]["column"] = "playing";
        row["columns"][0]["value"] = (i == studio.getSequenceIndex()) ? ">" : "";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = sequence[i].mPose;
        row["columns"][2]["column"] = "hold";
        row["columns"][2]["value"] = llformat("%.1f s", sequence[i].mHold);
        row["columns"][3]["column"] = "ease";
        row["columns"][3]["value"] = (sequence[i].mEase < 0.f) ? std::string("file") : llformat("%.1f", sequence[i].mEase);
        mSequenceList->addElement(row);
    }
    if (selected >= 0 && selected < (S32)sequence.size())
        mSequenceList->selectNthItem(selected);
}

void SLFloaterPoseStudio::onAddToSequence()
{
    const std::string name = mPoseList->getSelectedValue().asString();
    if (!name.empty())
        SLPoseStudio::instance().addToSequence(name, (F32)mHoldSpinner->getValue().asReal());
}

void SLFloaterPoseStudio::onSequenceRemove()
{
    SLPoseStudio::instance().removeFromSequence(mSequenceList->getFirstSelectedIndex());
}

void SLFloaterPoseStudio::onSequenceMove(S32 delta)
{
    const S32 index = mSequenceList->getFirstSelectedIndex();
    if (index < 0)
        return;
    SLPoseStudio::instance().moveSequenceEntry(index, delta);
    refreshSequence();
    mSequenceList->selectNthItem(index + delta);
}

void SLFloaterPoseStudio::onSequenceSelected()
{
    const S32 index = mSequenceList->getFirstSelectedIndex();
    const auto& sequence = SLPoseStudio::instance().getSequence();
    if (index >= 0 && index < (S32)sequence.size())
    {
        mHoldSpinner->setValue(sequence[index].mHold);
        getChild<LLUICtrl>("seq_ease_spin")->setValue(sequence[index].mEase);
    }
}

void SLFloaterPoseStudio::onSequenceHoldChanged()
{
    const S32 index = mSequenceList->getFirstSelectedIndex();
    if (index >= 0)
        SLPoseStudio::instance().setSequenceHold(index, (F32)mHoldSpinner->getValue().asReal());
}
