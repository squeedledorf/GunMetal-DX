/**
* @file lllocalcliprect.cpp
*
* $LicenseInfo:firstyear=2009&license=viewerlgpl$
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
#include "linden_common.h"

#include "lllocalcliprect.h"

#include "llfontgl.h"
#include "llrender.h"
#include "llrenderbackend.h" // EXPERIMENTAL Vulkan: route UI scissor through the backend seam
#include "llui.h"

/*static*/ std::stack<LLRect> LLScreenClipRect::sClipRectStack;


LLScreenClipRect::LLScreenClipRect(const LLRect& rect, bool enabled)
:   mEnabled(enabled)
{
    if (mEnabled)
    {
        pushClipRect(rect);
        updateScissorRegion();
    }
}

LLScreenClipRect::~LLScreenClipRect()
{
    if (mEnabled)
    {
        popClipRect();
        updateScissorRegion();
    }
}

//static
void LLScreenClipRect::pushClipRect(const LLRect& rect)
{
    LLRect combined_clip_rect = rect;
    if (!sClipRectStack.empty())
    {
        LLRect top = sClipRectStack.top();
        combined_clip_rect.intersectWith(top);

        if(combined_clip_rect.isEmpty())
        {
            // avoid artifacts where zero area rects show up as lines
            combined_clip_rect = LLRect::null;
        }
    }
    sClipRectStack.push(combined_clip_rect);
}

//static
void LLScreenClipRect::popClipRect()
{
    sClipRectStack.pop();
}

//static
void LLScreenClipRect::updateScissorRegion()
{
    if (sClipRectStack.empty())
    {
        applyScissor(false, LLRect());
        return;
    }
    applyScissor(true, sClipRectStack.top());
}

//static
void LLScreenClipRect::applyScissor(bool enable, const LLRect& rect)
{
    // UI clipping goes through the render backend seam (real VK scissor dynamic
    // state). Coords are bottom-left origin; the backend flips to VK top-left.
    gGL.flush();
    if (!enable)
    {
        LLRender::sBackend->setScissor(false, 0, 0, 0, 0);
        return;
    }
    S32 x = llfloor(rect.mLeft * LLUI::getScaleFactor().mV[VX]);
    S32 y = llfloor(rect.mBottom * LLUI::getScaleFactor().mV[VY]);
    S32 w = llmax(0, llceil(rect.getWidth() * LLUI::getScaleFactor().mV[VX])) + 1;
    S32 h = llmax(0, llceil(rect.getHeight() * LLUI::getScaleFactor().mV[VY])) + 1;
    LLRender::sBackend->setScissor(true, x, y, w, h);
}

//---------------------------------------------------------------------------
// LLLocalClipRect
//---------------------------------------------------------------------------
LLLocalClipRect::LLLocalClipRect(const LLRect& rect, bool enabled /* = true */)
:   LLScreenClipRect(LLRect(rect.mLeft + LLFontGL::sCurOrigin.mX,
                    rect.mTop + LLFontGL::sCurOrigin.mY,
                    rect.mRight + LLFontGL::sCurOrigin.mX,
                    rect.mBottom + LLFontGL::sCurOrigin.mY), enabled)
{}

LLLocalClipRect::~LLLocalClipRect()
{}
