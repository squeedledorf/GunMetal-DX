/**
 * @file llagentpilot.h
 * @brief LLAgentPilot class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLAGENTPILOT_H
#define LL_LLAGENTPILOT_H

#include "stdtypes.h"
#include "lltimer.h"
#include "v3dmath.h"
#include "v3math.h"

#include <vector>

// Class that drives the agent around according to a "script".

class LLAgentPilot
{
public:
    enum EActionType
    {
        STRAIGHT,
        TURN
    };

    // One recorded waypoint: agent target + the camera pose at that time.
    class Action
    {
    public:

        EActionType     mType;
        LLVector3d      mTarget;
        F64             mTime;
        F32             mCameraView;
        LLVector3       mCameraOrigin;
        LLVector3       mCameraXAxis;
        LLVector3       mCameraYAxis;
        LLVector3       mCameraZAxis;
    };

    LLAgentPilot();
    virtual ~LLAgentPilot();

    void load();
    void loadTxt(const std::string& filename);
    void loadXML(const std::string& filename);
    void save();
    void saveTxt(const std::string& filename);
    void saveXML(const std::string& filename);

    void startRecord();
    void stopRecord();
    void addAction(enum EActionType action);

    void startPlayback();
    void stopPlayback();

    bool isRecording() { return mRecording; }
    bool isPlaying() { return mPlaying; }
    bool getOverrideCamera() { return mOverrideCamera; }

    void updateTarget();

    //BD - Camera and agent recorder (bdfloatercamera): playback/record split out of
    //     updateTarget, and direct access to the recorded actions for editing.
    void updatePlayback();
    void updateRecord();

    const std::vector<Action>& getActions() const { return mActions; }
    void setActions(const std::vector<Action>& actions) { mActions = actions; }
    void setOverrideCamera(bool override_camera) { mOverrideCamera = override_camera; }

    // Recorder editing preview: while on, the camera is held at the given pose
    // (applyPreviewCamera, called from the frame loop in place of the agent camera).
    void setPreviewCamera(bool preview) { mPreviewCamera = preview; }
    void setPreviewPose(const LLVector3& origin, const LLVector3& x_axis, const LLVector3& y_axis,
                        const LLVector3& z_axis, F32 view);
    bool isPreviewing() const { return mPreviewCamera && !mPlaying; }
    void applyPreviewCamera();

    void addWaypoint();
    void moveCamera();

    void setReplaySession(bool new_val) { mReplaySession = new_val; }
    bool getReplaySession() { return mReplaySession; }

    void setLoop(bool new_val) { mLoop = new_val; }
    bool getLoop() { return mLoop; }

    void setQuitAfterRuns(bool quit_val) { mQuitAfterRuns = quit_val; }
    void setNumRuns(S32 num_runs) { mNumRuns = num_runs; }

private:



    bool    mLoop;
    bool    mReplaySession;

    S32     mNumRuns;
    bool    mQuitAfterRuns;

    void setAutopilotTarget(const S32 id);

    bool    mRecording;
    F32     mLastRecordTime;

    bool    mStarted;
    bool    mPlaying;
    S32     mCurrentAction;

    bool    mOverrideCamera;

    bool        mPreviewCamera = false;
    LLVector3   mPreviewOrigin;
    LLVector3   mPreviewXAxis = LLVector3::x_axis;
    LLVector3   mPreviewYAxis = LLVector3::y_axis;
    LLVector3   mPreviewZAxis = LLVector3::z_axis;
    F32         mPreviewView = 1.f;

    std::vector<Action> mActions;
    LLTimer                 mTimer;

};

extern LLAgentPilot gAgentPilot;

#endif // LL_LLAGENTPILOT_H
