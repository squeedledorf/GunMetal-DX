/**
 * @file llvrinput.h
 * @brief VR milestone M3 (#1076, epic #1072): what the controllers do.
 *
 * Once per frame, after xrWaitFrame synced the controllers and before idle() (so control flags
 * reach this frame's agent update and injected mouse moves precede the UI hover):
 *  - the UI WALL: the whole desktop UI (floaters, chat, toolbars, notifications, HUDs) on a
 *    curved wall world-locked where it was summoned (LLXrSession::setUiLayers). Always up
 *    before the world shows (login, loading); in the world the menu button (Index: left B)
 *    or View > Show UI Wall in VR shows / hides it, placing it in front of your facing.
 *  - the POINTER: the pointing hand's aim ray against the wall (LLXrSession::wallHit). The hit
 *    is the mouse: moves, the trigger = left button, A (Touch: A / X) = right button, the
 *    thumbstick scrolls -- injected through LLViewerWindow's normal mouse entry points in
 *    window pixels, like SDL's events. The beam (LLVRHands) shows while a ray hits the wall,
 *    a cursor is drawn into the UI (drawCursor).
 *
 * Desktop (no VR chosen at startup): update() returns on one static read; wallVisible() is
 * false; drawCursor() draws nothing.
 */
#pragma once

#include "stdtypes.h"

class LLVRInput
{
public:
    // LLAppViewer::doFrame, after the keyboard / joystick scan. No-op without VR.
    static void update();

    // The UI wall is shown (menu check). False on the desktop.
    static bool wallVisible();
    // View > Show UI Wall in VR (applied on the next update).
    static void toggleWall();

    // Inside the UI build, after the 2D UI: the pointer's cursor at the mouse point while the
    // pointer is on the wall (the OS cursor is not part of the UI image).
    static void drawCursor();
};
