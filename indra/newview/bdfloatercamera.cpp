/**
 *
 * Copyright (C) 2018, NiranV Dean
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
 */

#include "llviewerprecompiledheaders.h"

#include "bdfloatercamera.h"

#include "llagentpilot.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llscrolllistcell.h"
#include "llviewercamera.h"
#include "llwindow.h"
#include "m3math.h"
#include "llquaternion.h"

namespace
{
    // Scroll list columns (floater_recorder.xml).
    enum
    {
        COL_NUMBER = 0,
        COL_POS_X, COL_POS_Y, COL_POS_Z,
        COL_ROT_X, COL_ROT_Y, COL_ROT_Z,
        COL_TARGET_X, COL_TARGET_Y, COL_TARGET_Z,
        COL_VIEW, COL_TYPE, COL_TIME
    };

    F32 cellF32(LLScrollListItem* item, S32 col)
    {
        return (F32)item->getColumn(col)->getValue().asReal();
    }
}

BDFloaterCamera::BDFloaterCamera(const LLSD& key)
    :   LLFloater(key)
{
    mCommitCallbackRegistrar.add("Recorder.Export", boost::bind(&BDFloaterCamera::onExport, this));
    mCommitCallbackRegistrar.add("Recorder.Import", boost::bind(&BDFloaterCamera::onImport, this));
    mCommitCallbackRegistrar.add("Recorder.Override", boost::bind(&BDFloaterCamera::toggleOverride, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.Loop", boost::bind(&BDFloaterCamera::toggleLoop, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.StartStop", boost::bind(&BDFloaterCamera::onStartStop, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.Record", boost::bind(&BDFloaterCamera::onRecord, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.Refresh", boost::bind(&BDFloaterCamera::onRefresh, this));
    mCommitCallbackRegistrar.add("Recorder.Delete", boost::bind(&BDFloaterCamera::onDelete, this));
    mCommitCallbackRegistrar.add("Recorder.Waypoint", boost::bind(&BDFloaterCamera::addWaypoint, this));
    mCommitCallbackRegistrar.add("Recorder.Position", boost::bind(&BDFloaterCamera::savePosition, this));
    mCommitCallbackRegistrar.add("Recorder.Rotation", boost::bind(&BDFloaterCamera::saveRotation, this));
    mCommitCallbackRegistrar.add("Recorder.CamPos", boost::bind(&BDFloaterCamera::onCameraPos, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.CamRot", boost::bind(&BDFloaterCamera::onCameraRot, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.Time", boost::bind(&BDFloaterCamera::onTime, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.CamAngle", boost::bind(&BDFloaterCamera::onCameraAngle, this, _1, _2));
    mCommitCallbackRegistrar.add("Recorder.Clipboard", boost::bind(&BDFloaterCamera::copyToClipboard, this));
}

BDFloaterCamera::~BDFloaterCamera()
{
}

bool BDFloaterCamera::postBuild()
{
    mRecorderScroll = getChild<LLScrollListCtrl>("recorder_scroll");
    mRecorderScroll->setCommitOnSelectionChange(true);
    mRecorderScroll->setCommitCallback(boost::bind(&BDFloaterCamera::onRecorderSelection, this));
    return true;
}

void BDFloaterCamera::draw()
{
    //BD - Keep the buttons and list in sync when playback/recording ends on its own.
    const bool playing = gAgentPilot.isPlaying();
    const bool recording = gAgentPilot.isRecording();
    if (playing != mWasPlaying || recording != mWasRecording)
    {
        getChild<LLUICtrl>("playback_btn")->setValue(playing);
        getChild<LLUICtrl>("record_btn")->setValue(recording);
        if (mWasRecording && !recording)
            onRefresh();
        mWasPlaying = playing;
        mWasRecording = recording;
    }
    LLFloater::draw();
}

void BDFloaterCamera::onOpen(const LLSD& key)
{
    LLViewerCamera* viewer_cam = LLViewerCamera::getInstance();
    mOrigin = viewer_cam->getOrigin();
    mXAxis = viewer_cam->getXAxis();
    mYAxis = viewer_cam->getYAxis();
    mZAxis = viewer_cam->getZAxis();
    mView = viewer_cam->getView();
    getChild<LLUICtrl>("override_btn")->setValue(false);
    gAgentPilot.setPreviewCamera(false);

    onRefresh();
}

void BDFloaterCamera::onClose(bool app_quitting)
{
    gAgentPilot.setPreviewCamera(false);
    if (mRecorderScroll)
        mRecorderScroll->clearRows();
}

void BDFloaterCamera::updatePreview()
{
    gAgentPilot.setPreviewPose(mOrigin, mXAxis, mYAxis, mZAxis, mView);
}

void BDFloaterCamera::poseFromItem(LLScrollListItem* item)
{
    mOrigin.set(cellF32(item, COL_POS_X), cellF32(item, COL_POS_Y), cellF32(item, COL_POS_Z));
    LLQuaternion quat;
    quat.setEulerAngles(cellF32(item, COL_ROT_X), cellF32(item, COL_ROT_Y), cellF32(item, COL_ROT_Z));
    LLMatrix3 rot_mat(quat);
    mXAxis = LLVector3(rot_mat.mMatrix[0]);
    mYAxis = LLVector3(rot_mat.mMatrix[1]);
    mZAxis = LLVector3(rot_mat.mMatrix[2]);
    mView = cellF32(item, COL_VIEW);
    updatePreview();
}

////////////////////////////////
//BD - Recorder
////////////////////////////////
void BDFloaterCamera::onExport()
{
    std::vector<LLAgentPilot::Action> actions;
    for (LLScrollListItem* item : mRecorderScroll->getAllData())
    {
        if (!item)
            continue;
        LLAgentPilot::Action action;
        action.mCameraOrigin = LLVector3(cellF32(item, COL_POS_X), cellF32(item, COL_POS_Y), cellF32(item, COL_POS_Z));

        LLQuaternion quat;
        quat.setEulerAngles(cellF32(item, COL_ROT_X), cellF32(item, COL_ROT_Y), cellF32(item, COL_ROT_Z));
        LLMatrix3 mat(quat);
        action.mCameraXAxis = LLVector3(mat.mMatrix[0]);
        action.mCameraYAxis = LLVector3(mat.mMatrix[1]);
        action.mCameraZAxis = LLVector3(mat.mMatrix[2]);

        action.mTarget = LLVector3d(item->getColumn(COL_TARGET_X)->getValue().asReal(),
                                    item->getColumn(COL_TARGET_Y)->getValue().asReal(),
                                    item->getColumn(COL_TARGET_Z)->getValue().asReal());
        action.mCameraView = cellF32(item, COL_VIEW);
        action.mType = (LLAgentPilot::EActionType)item->getColumn(COL_TYPE)->getValue().asInteger();
        action.mTime = item->getColumn(COL_TIME)->getValue().asReal();
        actions.push_back(action);
    }

    gAgentPilot.setActions(actions);
    gAgentPilot.save();
}

void BDFloaterCamera::onImport()
{
    gAgentPilot.load();
    onRefresh();
}

void BDFloaterCamera::toggleOverride(LLUICtrl* ctrl, const LLSD& param)
{
    const bool preview = ctrl->getValue().asBoolean();
    if (preview)
        updatePreview();
    gAgentPilot.setPreviewCamera(preview);
}

void BDFloaterCamera::toggleLoop(LLUICtrl* ctrl, const LLSD& param)
{
    gAgentPilot.setLoop(ctrl->getValue().asBoolean());
}

void BDFloaterCamera::onStartStop(LLUICtrl* ctrl, const LLSD& param)
{
    const bool playing = gAgentPilot.isPlaying();
    if (ctrl->getValue().asBoolean())
    {
        if (!playing)
        {
            // Playback drives the camera along the recorded poses unless disabled.
            gAgentPilot.setOverrideCamera(getChild<LLUICtrl>("camera_playback_check")->getValue().asBoolean());
            gAgentPilot.setNumRuns(-1);
            gAgentPilot.startPlayback();
        }
    }
    else if (playing)
    {
        gAgentPilot.stopPlayback();
    }
    mWasPlaying = gAgentPilot.isPlaying();
    ctrl->setValue(mWasPlaying);
}

void BDFloaterCamera::onRecord(LLUICtrl* ctrl, const LLSD& param)
{
    const bool recording = gAgentPilot.isRecording();
    if (ctrl->getValue().asBoolean())
    {
        if (!recording)
            gAgentPilot.startRecord();
    }
    else if (recording)
    {
        gAgentPilot.stopRecord();
        onRefresh();
    }
    mWasRecording = gAgentPilot.isRecording();
}

void BDFloaterCamera::onRefresh()
{
    mRecorderScroll->clearRows();
    S32 count = 0;
    for (const LLAgentPilot::Action& action : gAgentPilot.getActions())
    {
        LLQuaternion quat(action.mCameraXAxis, action.mCameraYAxis, action.mCameraZAxis);
        LLVector3 euler;
        quat.getEulerAngles(&euler.mV[VX], &euler.mV[VY], &euler.mV[VZ]);
        LLSD row;
        row["columns"][COL_NUMBER]["column"] = "number";
        row["columns"][COL_NUMBER]["value"] = count;
        row["columns"][COL_POS_X]["column"] = "pos_x";
        row["columns"][COL_POS_X]["value"] = action.mCameraOrigin.mV[VX];
        row["columns"][COL_POS_Y]["column"] = "pos_y";
        row["columns"][COL_POS_Y]["value"] = action.mCameraOrigin.mV[VY];
        row["columns"][COL_POS_Z]["column"] = "pos_z";
        row["columns"][COL_POS_Z]["value"] = action.mCameraOrigin.mV[VZ];
        row["columns"][COL_ROT_X]["column"] = "rot_x";
        row["columns"][COL_ROT_X]["value"] = euler.mV[VX];
        row["columns"][COL_ROT_Y]["column"] = "rot_y";
        row["columns"][COL_ROT_Y]["value"] = euler.mV[VY];
        row["columns"][COL_ROT_Z]["column"] = "rot_z";
        row["columns"][COL_ROT_Z]["value"] = euler.mV[VZ];
        row["columns"][COL_TARGET_X]["column"] = "target_x";
        row["columns"][COL_TARGET_X]["value"] = action.mTarget.mdV[VX];
        row["columns"][COL_TARGET_Y]["column"] = "target_y";
        row["columns"][COL_TARGET_Y]["value"] = action.mTarget.mdV[VY];
        row["columns"][COL_TARGET_Z]["column"] = "target_z";
        row["columns"][COL_TARGET_Z]["value"] = action.mTarget.mdV[VZ];
        row["columns"][COL_VIEW]["column"] = "view";
        row["columns"][COL_VIEW]["value"] = action.mCameraView;
        row["columns"][COL_TYPE]["column"] = "type";
        row["columns"][COL_TYPE]["value"] = (S32)action.mType;
        row["columns"][COL_TIME]["column"] = "time";
        row["columns"][COL_TIME]["value"] = action.mTime;
        mRecorderScroll->addElement(row);
        count++;
    }
    onRecorderControlsRefresh();
}

void BDFloaterCamera::onDelete()
{
    mRecorderScroll->deleteSelectedItems();
    onExport();
    onRecorderControlsRefresh();
}

void BDFloaterCamera::addWaypoint()
{
    gAgentPilot.addAction(LLAgentPilot::STRAIGHT);
    gAgentPilot.save();
    onRefresh();
}

void BDFloaterCamera::savePosition()
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    item->getColumn(COL_POS_X)->setValue(origin.mV[VX]);
    item->getColumn(COL_POS_Y)->setValue(origin.mV[VY]);
    item->getColumn(COL_POS_Z)->setValue(origin.mV[VZ]);
    onExport();
    onRecorderSelection();
}

void BDFloaterCamera::saveRotation()
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;
    LLQuaternion quat = LLViewerCamera::getInstance()->getQuaternion();
    LLVector3 rotation;
    quat.getEulerAngles(&rotation.mV[VX], &rotation.mV[VY], &rotation.mV[VZ]);
    item->getColumn(COL_ROT_X)->setValue(rotation.mV[VX]);
    item->getColumn(COL_ROT_Y)->setValue(rotation.mV[VY]);
    item->getColumn(COL_ROT_Z)->setValue(rotation.mV[VZ]);
    onExport();
    onRecorderSelection();
}

void BDFloaterCamera::onCameraPos(LLUICtrl* ctrl, const LLSD& param)
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;

    const F32 val = (F32)ctrl->getValue().asReal();
    const std::string axis = param.asString();
    const S32 i = axis == "X" ? VX : axis == "Y" ? VY : VZ;
    mOrigin.mV[i] = val;
    item->getColumn(COL_POS_X + i)->setValue(val);
    updatePreview();
}

void BDFloaterCamera::onCameraRot(LLUICtrl* ctrl, const LLSD& param)
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;

    const S32 axis = llclamp(param.asInteger(), 0, 2);
    item->getColumn(COL_ROT_X + axis)->setValue((F32)ctrl->getValue().asReal());
    poseFromItem(item);
}

void BDFloaterCamera::onTime(LLUICtrl* ctrl, const LLSD& param)
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;
    item->getColumn(COL_TIME)->setValue(ctrl->getValue().asReal());
}

void BDFloaterCamera::onCameraAngle(LLUICtrl* ctrl, const LLSD& param)
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (!item)
        return;
    mView = (F32)ctrl->getValue().asReal();
    item->getColumn(COL_VIEW)->setValue(mView);
    updatePreview();
}

void BDFloaterCamera::onRecorderControlsRefresh()
{
    const bool have_items = mRecorderScroll->getItemCount() != 0;
    const bool selected = mRecorderScroll->getFirstSelected() != nullptr;
    getChild<LLUICtrl>("export_btn")->setEnabled(have_items);
    getChild<LLUICtrl>("playback_btn")->setEnabled(have_items);
    getChild<LLUICtrl>("clipboard_btn")->setEnabled(selected);
    getChild<LLUICtrl>("save_rotation_btn")->setEnabled(selected);
    getChild<LLUICtrl>("save_position_btn")->setEnabled(selected);
    getChild<LLUICtrl>("remove_btn")->setEnabled(selected);
    for (const char* name : {"Position_X", "Position_Y", "Position_Z", "Rotation_X", "Rotation_Y", "Rotation_Z",
                             "Camera_View", "Transition_Time"})
    {
        getChild<LLUICtrl>(name)->setEnabled(selected);
    }
    getChild<LLUICtrl>("loop_btn")->setValue(gAgentPilot.getLoop());
}

void BDFloaterCamera::copyToClipboard()
{
    std::string string;
    for (LLScrollListItem* item : mRecorderScroll->getAllSelected())
    {
        if (item)
        {
            string.append(item->getContentsCSV());
            string.append("\n");
        }
    }
    LLView::getWindow()->copyTextToClipboard(utf8str_to_wstring(string));
}

void BDFloaterCamera::onRecorderSelection()
{
    LLScrollListItem* item = mRecorderScroll->getFirstSelected();
    if (item)
    {
        getChild<LLUICtrl>("Position_X")->setValue(item->getColumn(COL_POS_X)->getValue());
        getChild<LLUICtrl>("Position_Y")->setValue(item->getColumn(COL_POS_Y)->getValue());
        getChild<LLUICtrl>("Position_Z")->setValue(item->getColumn(COL_POS_Z)->getValue());
        getChild<LLUICtrl>("Rotation_X")->setValue(item->getColumn(COL_ROT_X)->getValue());
        getChild<LLUICtrl>("Rotation_Y")->setValue(item->getColumn(COL_ROT_Y)->getValue());
        getChild<LLUICtrl>("Rotation_Z")->setValue(item->getColumn(COL_ROT_Z)->getValue());
        getChild<LLUICtrl>("Camera_View")->setValue(item->getColumn(COL_VIEW)->getValue());
        getChild<LLUICtrl>("Transition_Time")->setValue(item->getColumn(COL_TIME)->getValue());
        poseFromItem(item);
    }
    onRecorderControlsRefresh();
}
