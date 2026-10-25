/**
 * @file slmmbnav.h
 * @brief Blender-style middle-mouse camera navigation (#652)
 *
 *   MMB drag         orbit around the surface point under the cursor at the start of the
 *                    drag (nothing there: the current focus), like an Alt-click orbit
 *   Shift + MMB drag pan parallel to the view plane, scaled by the distance to the focus
 *                    so the grabbed point stays under the cursor
 *   Ctrl + MMB drag  dolly toward / away from the focus
 *
 * MMB is the push-to-talk button in the stock key bindings, so while this is on a press is
 * decided by what the mouse does: a drag past a small threshold navigates, a press that
 * stays put (click or hold) is push-to-talk, started after a short hold delay so a drag
 * never keys the microphone. Once talking has started the press stays push-to-talk until
 * release (moving the mouse never cuts the mic); to navigate, start moving right away.
 *
 * It is a mouse captor for the length of the press (like the other camera tools), claimed
 * by LLViewerWindow::handleAnyMouseClick only when nothing else took the press: UI,
 * Pose Studio handles / rings and any other captor win. Mouselook, appearance editing and
 * the TPS camera (which captures the mouse itself) are left alone.
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

#ifndef SL_MMBNAV_H
#define SL_MMBNAV_H

#include "llsingleton.h"
#include "lltool.h"
#include "llframetimer.h"
#include "llviewerwindow.h"   // LLPickInfo

class SLMMBNav final : public LLTool, public LLSingleton<SLMMBNav>
{
    LLSINGLETON(SLMMBNav);
    virtual ~SLMMBNav() = default;

public:
    // SlipstreamMMBNav setting.
    static bool isEnabled();
    // A press is being decided or navigated (safe before the singleton exists). While true
    // the middle-button release is ours, including the push-to-talk release.
    static bool isActive();

    // A middle-button press that reached the world. True: claimed (the push-to-talk binding
    // stays deferred until the mouse decides); false: not eligible, run the bindings now.
    bool tryBegin(S32 x, S32 y, MASK mask);

    bool handleMiddleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    LLTool* getOverrideTool(MASK mask) override { return nullptr; }
    bool    clipMouseWhenDown() override { return false; }

private:
    enum EState { IDLE, DECIDING, TALKING, NAVIGATING };

    void beginNavigation();
    void endPress();
    void firePushToTalk(bool down);

    EState       mState = IDLE;
    S32          mDownX = 0;
    S32          mDownY = 0;
    S32          mAccum = 0;
    MASK         mDownMask = 0;
    bool         mTalkDown = false;   // the middle-button bindings saw a press
    bool         mHideCursor = false;
    LLFrameTimer mHoldTimer;
    LLPickInfo   mPress;              // what was under the cursor at the press (orbit centre)
};

#endif // SL_MMBNAV_H
