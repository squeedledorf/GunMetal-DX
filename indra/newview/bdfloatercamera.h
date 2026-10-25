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

// Camera and agent recorder, ported from Black Dragon (NiranV/Black-Dragon-Viewer,
// bdfloatercamera.*). A UI over LLAgentPilot: record, add/edit waypoints (camera
// position, rotation, field of view, time), preview a waypoint's camera, play back
// with looping, save/load the pilot file (StatsPilotXMLFile).

#ifndef BD_FLOATER_CAMERA_H
#define BD_FLOATER_CAMERA_H

#include "llfloater.h"
#include "llagentpilot.h"

class LLScrollListCtrl;

class BDFloaterCamera :
    public LLFloater
{
    friend class LLFloaterReg;
private:
    BDFloaterCamera(const LLSD& key);
    /*virtual*/ ~BDFloaterCamera();
    /*virtual*/ bool postBuild();
    /*virtual*/ void draw();
    /*virtual*/ void onOpen(const LLSD& key);
    /*virtual*/ void onClose(bool app_quitting);

    //BD - Camera and Agent Recorder Commands
    //     Export       - Apply the list to the pilot and save it to the pilot file.
    //     Import       - Load the pilot file into the list.
    //     Override     - Preview the selected record's camera (editing).
    //     Loop         - Toggle whether playback loops.
    //     StartStop    - Start/stop the playback of all records.
    //     Record       - Start/stop recording.
    //     Refresh      - Refresh the current list.
    //     Delete       - Delete all currently selected records.
    //     Waypoint     - Add a waypoint with the current position and camera.
    //     Position     - Save the current camera position into the selected record.
    //     Rotation     - Save the current camera rotation into the selected record.
    //     CamPos/CamRot/CamAngle/Time - Edit the selected record.
    void onExport();
    void onImport();

    void toggleOverride(LLUICtrl* ctrl, const LLSD& param);
    void toggleLoop(LLUICtrl* ctrl, const LLSD& param);

    void onStartStop(LLUICtrl* ctrl, const LLSD& param);
    void onRecord(LLUICtrl* ctrl, const LLSD& param);
    void onRefresh();

    void onDelete();
    void addWaypoint();
    void savePosition();
    void saveRotation();

    void onCameraPos(LLUICtrl* ctrl, const LLSD& param);
    void onCameraRot(LLUICtrl* ctrl, const LLSD& param);
    void onTime(LLUICtrl* ctrl, const LLSD& param);
    void onCameraAngle(LLUICtrl* ctrl, const LLSD& param);

    void onRecorderSelection();
    void onRecorderControlsRefresh();
    void copyToClipboard();

    // Push the edited pose to the pilot's preview camera.
    void updatePreview();
    // Rebuild the preview pose from the selected row.
    void poseFromItem(class LLScrollListItem* item);

    LLScrollListCtrl*   mRecorderScroll = nullptr;

    LLVector3   mXAxis = LLVector3::x_axis;
    LLVector3   mYAxis = LLVector3::y_axis;
    LLVector3   mZAxis = LLVector3::z_axis;
    LLVector3   mOrigin;
    F32         mView = 1.f;
    bool        mWasPlaying = false;
    bool        mWasRecording = false;
};

#endif
