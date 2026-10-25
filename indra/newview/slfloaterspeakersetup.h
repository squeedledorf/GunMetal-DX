/**
 * @file slfloaterspeakersetup.h
 * @brief #892: "Set up as speaker..." (object right-click > Tools). Takes the speaker
 *        parameters, shows the [slspk ...] tag they make, and puts a one-shot script into the
 *        selected prim that sets the tag as hover text and deletes itself
 *        (doc/parcel_speakers_protocol.md). "Remove speaker tag" does the same with a script
 *        that clears the hover text.
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

#ifndef SL_FLOATERSPEAKERSETUP_H
#define SL_FLOATERSPEAKERSETUP_H

#include "llfloater.h"
#include "llframetimer.h"
#include "slparcelspeakers.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLViewerObject;

class SLFloaterSpeakerSetup final : public LLFloater
{
    friend class LLFloaterReg;
public:
    bool postBuild() override;
    void draw() override;

    // The prim the floater works on (the clicked prim of the selection).
    void setObject(LLViewerObject* obj);

    // Menu: one prim selected (or one prim of one linkset), modify rights, not worn.
    static LLViewerObject* getSelectedTarget();
    static bool canSetUp();
    static void showForSelection();

private:
    SLFloaterSpeakerSetup(const LLSD& key);
    ~SLFloaterSpeakerSetup() override = default;

    SLParcelSpeakers::Tag tagFromControls() const;
    void tagToControls(const SLParcelSpeakers::Tag& tag);
    static std::string makeTagString(const SLParcelSpeakers::Tag& tag);

    void onGroupKeystroke();
    void onChanged();
    void refreshState();
    std::string trustHint(LLViewerObject* obj) const;
    std::string statusFor(LLViewerObject* obj, const std::string& tag_str) const;

    void onApply();
    void onRemove();

    LLUUID mObjectID;
    S64 mKeepT0 = 0;            // the prim's t0= (no field for it: kept as it was)
    bool mHasT0 = false;
    LLFrameTimer mRefreshTimer;

    LLLineEditor* mGroupEdit = nullptr;
    LLComboBox* mChannelCombo = nullptr;
    LLSliderCtrl* mVolumeSlider = nullptr;
    LLSliderCtrl* mRangeSlider = nullptr;
    LLCheckBoxCtrl* mDirCheck = nullptr;
    LLSpinCtrl* mDirSpin = nullptr;
    LLCheckBoxCtrl* mOffCheck = nullptr;
    LLTextBox* mObjectText = nullptr;
    LLTextBox* mPreviewText = nullptr;
    LLTextBox* mTrustText = nullptr;
    LLTextBox* mStatusText = nullptr;
    LLButton* mApplyBtn = nullptr;
    LLButton* mRemoveBtn = nullptr;
};

#endif // SL_FLOATERSPEAKERSETUP_H
