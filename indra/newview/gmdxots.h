/**
 * @file gmdxots.h
 * @brief GunMetal-DX's over-the-shoulder (OTS) aim camera: mouselook with the camera at the shoulder.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> GunMetal-DX 0.3.0's OTS aim camera (SkoomaStorm's CAMERA_MODE_OTS, <SS:Skoomastream>; the
// focus and seat rules developed by squeedledorf with Soap Frenzy, SoapStorm 557eddcac5), kept as REAL
// mouselook: the camera mode stays CAMERA_MODE_MOUSELOOK, so the gun tool, the mouselook UI,
// AGENT_CONTROL_MOUSELOOK and everything that tests cameraMouselook() (two-level zoom, ADS, IFF, pings,
// quick swap, chat while aiming, right button to scripts, gyro and trackpad aim) work on the shoulder
// as in first person. Only the camera moves: llagentcamera.cpp's mouselook branches ask
// getCameraPositionGlobal / getFocusGlobal here while active().
//
// M enters it (OTSEnabled; with OTSRememberLastView the view Ctrl+Space last chose,
// OTSLastViewFirstPerson) and leaves aim mode; Ctrl+Space swaps it with first person and Shift+Space
// swaps the shoulder, both staying in mouselook; the wheel in goes to first person. Esc leaves aim mode;
// with Dynamic camera (SlipstreamTPSDynamicCamera) the next Esc goes back in. Seated, only on a seat
// whose creator set a sit camera (that camera's offsets frame it); else first person. CombatFX only, never
// in VR (the headset camera sits at the head). Framing: the SlipstreamTPSCamera{Distance,Side,Height},
// SlipstreamTPSFocusDistance and SlipstreamTPSCameraCollision settings (Preferences > Combat > Camera).

#pragma once

#include "v3dmath.h"
#include "v3math.h"

namespace GMDXOTS
{
    // Aiming over the shoulder now: in mouselook with the camera at the shoulder.
    bool active();
    // The render camera is over the shoulder or still gliding away from it (into first person): the own
    // avatar stays drawn until the camera reaches the head, and send_agent_update tells the sim the eye
    // instead of the gliding camera (fair fire, as on the shoulder).
    bool shoulderCamera();
    // canUseOTS: CombatFX, not in VR, seated only with the seat's sit camera, and room under an RLVa
    // camera distance cap.
    bool canUse();
    // changeCameraToOTS: into mouselook at the shoulder, from third person or first person. False: not on
    // the shoulder (mouselook refused, or an RLVa cap kept it in first person).
    bool enter(bool animate = true);
    // Over the shoulder -> first person, staying in mouselook (a flag and a camera glide, no mode change).
    void toFirstPerson(bool animate = true);

    bool takesKeys();       // the two OTS keys work: CombatFX on, in aim mode (else the key is not taken)
    void toggleView();      // Ctrl+Space (ots_toggle_view): the shoulder <-> first person, staying in mouselook
    void swapShoulder();    // Shift+Space (ots_swap_shoulder): the other shoulder (also for the next time, in first person)
    bool onMouselookKey();  // M (LLViewMouselook); false: the stock mouselook toggle
    bool onEsc();           // Esc (LLViewResetView); true: handled (Dynamic camera went back into aim mode)
    void onWheelIn();       // the wheel in, in mouselook (LLToolCompGun): over the shoulder -> first person

    // llagentcamera.cpp
    void update();                              // per frame, at the start of updateCamera
    void onChangeToMouselook(bool animate);     // changeCameraToMouselook: every other way into it is first person
    void onCameraAnimation();                   // startCameraAnimation
    bool animatesFromFirstPerson();             // the camera animation started in first person (not the shoulder)
    LLVector3d getCameraPositionGlobal(bool& constrained);
    LLVector3d getFocusGlobal();

    // process_avatar_sit_response: the seat's sit camera eye offset (llSetCameraEyeOffset)
    void setSitCameraEye(const LLVector3& eye);
}
