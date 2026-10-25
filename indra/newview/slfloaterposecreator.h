/**
 * @file slfloaterposecreator.h
 * @brief Pose Creator (#117) floater: keyframe list over the Pose Studio target,
 *        animation options, .anim save / upload / local preview.
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

#ifndef SL_FLOATERPOSECREATOR_H
#define SL_FLOATERPOSECREATOR_H

#include "llfloater.h"
#include "slposecreator.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;

class SLFloaterPoseCreator : public LLFloater
{
    LOG_CLASS(SLFloaterPoseCreator);
public:
    SLFloaterPoseCreator(const LLSD& key);
    ~SLFloaterPoseCreator() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    SLPoseCreator::Options readOptions() const;
    void refreshList(S32 select = -1);
    void setStatus(const std::string& key, const LLSD& args = LLSD());

    void onAdd();
    void onUpdate();
    void onDelete();
    void onGoTo();
    void onClear();
    void onTimeChanged();
    void onSelected();
    void onSave();
    void onSaveTo(const std::string& path);
    void onUpload();
    void onPreview();
    void onStopPreview();
    // Writes the animation to the scratch file; empty on error (status set).
    std::string writeScratch(const std::string& leaf);

    LLScrollListCtrl* mKeyList = nullptr;
    LLSpinCtrl*       mTimeSpin = nullptr;
    LLSpinCtrl*       mDurationSpin = nullptr;
    LLSpinCtrl*       mPrioritySpin = nullptr;
    LLSpinCtrl*       mEaseInSpin = nullptr;
    LLSpinCtrl*       mEaseOutSpin = nullptr;
    LLCheckBoxCtrl*   mLoopCheck = nullptr;
    LLCheckBoxCtrl*   mEditedOnlyCheck = nullptr;
    LLCheckBoxCtrl*   mPelvisPosCheck = nullptr;
    LLComboBox*       mHandCombo = nullptr;
    LLTextBox*        mStatusText = nullptr;
    LLUUID            mPreviewAnim;     // local anim id of the running preview
    LLUUID            mPreviewAvatar;
};

#endif // SL_FLOATERPOSECREATOR_H
