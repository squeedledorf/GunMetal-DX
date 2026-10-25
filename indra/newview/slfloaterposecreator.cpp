/**
 * @file slfloaterposecreator.cpp
 * @brief Pose Creator (#117) floater. See slfloaterposecreator.h / slposecreator.h.
 *
 * Workflow: pose the target in Pose Studio, set the time, Add keyframe; repeat;
 * Preview plays the result on the target (posing stopped: the poser's overrides
 * outrank any animation); Save writes the .anim; Upload hands it to the standard
 * animation upload floater (which accepts .anim files as they are).
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

#include "slfloaterposecreator.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfilepicker.h"
#include "lllocalanim.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluistring.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread, upload_single_file
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "slposestudio.h"

SLFloaterPoseCreator::SLFloaterPoseCreator(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterPoseCreator::~SLFloaterPoseCreator() = default;

bool SLFloaterPoseCreator::postBuild()
{
    mKeyList = getChild<LLScrollListCtrl>("key_list");
    mTimeSpin = getChild<LLSpinCtrl>("key_time");
    mDurationSpin = getChild<LLSpinCtrl>("duration_spin");
    mPrioritySpin = getChild<LLSpinCtrl>("priority_spin");
    mEaseInSpin = getChild<LLSpinCtrl>("ease_in_spin");
    mEaseOutSpin = getChild<LLSpinCtrl>("ease_out_spin");
    mLoopCheck = getChild<LLCheckBoxCtrl>("loop_check");
    mEditedOnlyCheck = getChild<LLCheckBoxCtrl>("edited_only_check");
    mPelvisPosCheck = getChild<LLCheckBoxCtrl>("pelvis_pos_check");
    mHandCombo = getChild<LLComboBox>("hand_combo");
    mStatusText = getChild<LLTextBox>("status_text");

    getChild<LLButton>("add_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdd(); });
    getChild<LLButton>("update_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpdate(); });
    getChild<LLButton>("delete_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDelete(); });
    getChild<LLButton>("goto_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGoTo(); });
    getChild<LLButton>("clear_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClear(); });
    getChild<LLButton>("save_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    getChild<LLButton>("upload_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpload(); });
    getChild<LLButton>("preview_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreview(); });
    getChild<LLButton>("stop_preview_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStopPreview(); });
    mTimeSpin->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTimeChanged(); });
    mKeyList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelected(); });
    mKeyList->setDoubleClickCallback([this]() { onGoTo(); });
    refreshList();
    return true;
}

void SLFloaterPoseCreator::onOpen(const LLSD& key)
{
    refreshList(mKeyList->getFirstSelectedIndex());
    setStatus("status_idle");
}

void SLFloaterPoseCreator::onClose(bool app_quitting)
{
    if (!app_quitting)
        onStopPreview();
}

SLPoseCreator::Options SLFloaterPoseCreator::readOptions() const
{
    SLPoseCreator::Options opt;
    opt.mDuration = (F32)mDurationSpin->getValue().asReal();
    opt.mLoop = mLoopCheck->getValue().asBoolean();
    opt.mPriority = mPrioritySpin->getValue().asInteger();
    opt.mEaseIn = (F32)mEaseInSpin->getValue().asReal();
    opt.mEaseOut = (F32)mEaseOutSpin->getValue().asReal();
    opt.mHandPose = (U32)llmax(0, mHandCombo->getValue().asInteger());
    opt.mEditedOnly = mEditedOnlyCheck->getValue().asBoolean();
    opt.mPelvisPosition = mPelvisPosCheck->getValue().asBoolean();
    return opt;
}

void SLFloaterPoseCreator::refreshList(S32 select)
{
    const auto& keys = SLPoseCreator::instance().getKeyframes();
    mKeyList->deleteAllItems();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "index";
        row["columns"][0]["value"] = llformat("%d", (S32)i + 1);
        row["columns"][1]["column"] = "time";
        row["columns"][1]["value"] = llformat("%.2f", keys[i].mTime);
        row["columns"][2]["column"] = "bones";
        row["columns"][2]["value"] = llformat("%d", (S32)keys[i].mEdited.size());
        mKeyList->addElement(row);
    }
    if (select >= 0 && select < (S32)keys.size())
        mKeyList->selectNthItem(select);
    const bool sel = mKeyList->getFirstSelectedIndex() >= 0;
    getChildView("update_btn")->setEnabled(sel);
    getChildView("delete_btn")->setEnabled(sel);
    getChildView("goto_btn")->setEnabled(sel);
    const bool any = !keys.empty();
    getChildView("save_btn")->setEnabled(any);
    getChildView("upload_btn")->setEnabled(any);
    getChildView("preview_btn")->setEnabled(any);
    getChildView("clear_btn")->setEnabled(any);
}

void SLFloaterPoseCreator::setStatus(const std::string& key, const LLSD& args)
{
    if (!hasString(key))
    {
        mStatusText->setText(key);
        return;
    }
    LLUIString text = getString(key);
    for (LLSD::map_const_iterator it = args.beginMap(); it != args.endMap(); ++it)
        text.setArg("[" + it->first + "]", it->second.asString());
    mStatusText->setText(text.getString());
}

void SLFloaterPoseCreator::onAdd()
{
    LLVOAvatar* avatar = SLPoseStudio::instance().getTarget();
    if (!avatar)
    {
        setStatus("status_no_target");
        return;
    }
    const S32 idx = SLPoseCreator::instance().addKeyframe(avatar, (F32)mTimeSpin->getValue().asReal());
    if (idx < 0)
    {
        setStatus("status_capture_failed");
        return;
    }
    // Next keyframe one second later by default.
    mTimeSpin->setValue(llmin((F32)mTimeSpin->getValue().asReal() + 1.f, SLPoseCreator::MAX_DURATION));
    refreshList(idx);
    LLSD args;
    args["N"] = idx + 1;
    setStatus("status_added", args);
}

void SLFloaterPoseCreator::onUpdate()
{
    const S32 idx = mKeyList->getFirstSelectedIndex();
    LLVOAvatar* avatar = SLPoseStudio::instance().getTarget();
    if (idx < 0 || !avatar || !SLPoseCreator::instance().recapture(idx, avatar))
    {
        setStatus("status_capture_failed");
        return;
    }
    refreshList(idx);
    LLSD args;
    args["N"] = idx + 1;
    setStatus("status_updated", args);
}

void SLFloaterPoseCreator::onDelete()
{
    const S32 idx = mKeyList->getFirstSelectedIndex();
    SLPoseCreator::instance().removeKeyframe(idx);
    refreshList(llmax(0, idx - 1));
}

void SLFloaterPoseCreator::onGoTo()
{
    const S32 idx = mKeyList->getFirstSelectedIndex();
    LLVOAvatar* avatar = SLPoseStudio::instance().getTarget();
    if (idx < 0 || !avatar)
        return;
    if (!SLPoseStudio::instance().isPosing(avatar))
    {
        setStatus("status_need_posing");
        return;
    }
    if (!SLPoseCreator::instance().applyKeyframe(idx, avatar))
        setStatus("status_capture_failed");
}

void SLFloaterPoseCreator::onClear()
{
    SLPoseCreator::instance().clear();
    mTimeSpin->setValue(0.f);
    refreshList();
}

void SLFloaterPoseCreator::onTimeChanged()
{
    const S32 idx = mKeyList->getFirstSelectedIndex();
    if (idx < 0)
        return;   // the time of the next Add
    const S32 moved = SLPoseCreator::instance().setKeyframeTime(idx, (F32)mTimeSpin->getValue().asReal());
    refreshList(moved);
}

void SLFloaterPoseCreator::onSelected()
{
    const S32 idx = mKeyList->getFirstSelectedIndex();
    const auto& keys = SLPoseCreator::instance().getKeyframes();
    if (idx >= 0 && idx < (S32)keys.size())
        mTimeSpin->setValue(keys[idx].mTime);
    refreshList(idx);
}

void SLFloaterPoseCreator::onSave()
{
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            SLFloaterPoseCreator* self = dynamic_cast<SLFloaterPoseCreator*>(handle.get());
            if (self && !files.empty())
                self->onSaveTo(files.front());
        },
        LLFilePicker::FFSAVE_ANIM, "pose.anim");
}

void SLFloaterPoseCreator::onSaveTo(const std::string& path)
{
    std::string error;
    LLSD args;
    if (!SLPoseCreator::instance().exportAnim(path, readOptions(), error))
    {
        args["REASON"] = hasString("reason_" + error) ? getString("reason_" + error) : error;
        setStatus("status_error", args);
        return;
    }
    args["FILE"] = gDirUtilp->getBaseFileName(path);
    setStatus("status_saved", args);
}

std::string SLFloaterPoseCreator::writeScratch(const std::string& leaf)
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, leaf);
    std::string error;
    if (!SLPoseCreator::instance().exportAnim(path, readOptions(), error))
    {
        LLSD args;
        args["REASON"] = hasString("reason_" + error) ? getString("reason_" + error) : error;
        setStatus("status_error", args);
        return std::string();
    }
    return path;
}

void SLFloaterPoseCreator::onUpload()
{
    const std::string path = writeScratch("slipstream_pose_creator.anim");
    if (path.empty())
        return;
    // The standard upload path: an .anim opens the animation upload floater as it is.
    upload_single_file({path}, LLFilePicker::FFLOAD_ANIM, LLUUID::null);
}

void SLFloaterPoseCreator::onPreview()
{
    LLVOAvatar* avatar = SLPoseStudio::instance().getTarget();
    if (!avatar)
    {
        setStatus("status_no_target");
        return;
    }
    // The poser's joint overrides outrank every animation: the preview needs posing off.
    if (SLPoseStudio::instance().isPosing(avatar))
    {
        setStatus("status_stop_posing");
        return;
    }
    onStopPreview();
    const std::string path = writeScratch("slipstream_pose_creator_preview.anim");
    if (path.empty())
        return;
    LLLocalAnimMgr& mgr = LLLocalAnimMgr::instance();
    // A fresh unit per preview: the keyframe cache is keyed by the motion id, so
    // re-loading under a new id is what picks the edited animation up.
    const LLUUID old = mgr.getUnitID(path);
    if (old.notNull())
        mgr.delUnit(old);
    const LLUUID id = mgr.addUnit(path);
    if (id.isNull() || !mgr.playOnAvatar(avatar, id))
    {
        setStatus("status_preview_failed");
        return;
    }
    mPreviewAnim = id;
    mPreviewAvatar = avatar->getID();
    setStatus("status_previewing");
}

void SLFloaterPoseCreator::onStopPreview()
{
    if (mPreviewAnim.isNull())
        return;
    if (LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(mPreviewAvatar)))
        LLLocalAnimMgr::instance().stopOnAvatar(avatar);
    LLLocalAnimMgr::instance().delUnit(mPreviewAnim);
    mPreviewAnim.setNull();
    mPreviewAvatar.setNull();
    setStatus("status_idle");
}
