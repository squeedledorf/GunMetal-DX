/**
 * @file sltoolscenepicker.cpp
 * @brief Transient tool that reports the linear scene colour under a click
 *
 * Slipstream Photo Studio: originally altoolscenepicker.cpp from Alchemy Viewer's Lightbox
 * (upstream Alchemy, credited); renamed for Slipstream.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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

#include "sltoolscenepicker.h"

#include "lltoolmgr.h"
#include "llviewerwindow.h"
#include "pipeline.h"

SLToolScenePicker::SLToolScenePicker()
:   LLTool(std::string("ScenePicker"))
{
}

SLToolScenePicker::~SLToolScenePicker()
{
}

void SLToolScenePicker::handleSelect()
{
    gViewerWindow->setCursor(UI_CURSOR_PIPETTE);
}

void SLToolScenePicker::handleDeselect()
{
    // Dropped whether the pick completed or was cancelled. A callback that
    // outlived its tool would fire into whatever floater came next.
    mCallback = nullptr;
    setMouseCapture(false);
}

bool SLToolScenePicker::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setMouseCapture(true);
    return true;
}

bool SLToolScenePicker::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // Taken here rather than on the press so the aim can be corrected, and
    // abandoned by releasing outside the world view.
    pick_cb_t callback = mCallback;

    setMouseCapture(false);

    // Pops the tool, which runs handleDeselect and clears the callback -- so
    // the copy above is the one that gets used, and the tool is already gone
    // by the time the sample arrives a frame later.
    LLToolMgr::getInstance()->clearTransientTool();

    // No gPipeline.isInit() gate: the Vulkan viewer never runs LLPipeline::init(); the
    // VK renderer services the request (LLCleanRendererVK::recordScenePixel).
    if (callback)
    {
        gPipeline.requestScenePixel(x, y, std::move(callback));
    }
    return true;
}

bool SLToolScenePicker::handleHover(S32 x, S32 y, MASK mask)
{
    gViewerWindow->setCursor(UI_CURSOR_PIPETTE);
    return true;
}
