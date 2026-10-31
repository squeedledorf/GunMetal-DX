/**
 * @file gmdxmenuswatchrow.cpp
 * @brief GunMetal-DX menu row of colour swatches (see gmdxmenuswatchrow.h).
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

// <GMDX> whole file: see gmdxmenuswatchrow.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxmenuswatchrow.h"

#include "llminimapmarks.h"
#include "llfontgl.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llui.h"

static MenuRegistry::Register<GMDXMenuSwatchRow> r_swatch_row("gmdx_swatch_row");

namespace
{
constexpr S32 SWATCH = 14;        // a swatch's side
constexpr S32 GAP = 5;            // between swatches
constexpr S32 LABEL_LEFT = 18;    // where LLMenuItemGL puts a plain item's label (LEFT_PLAIN_PIXELS)
constexpr S32 RIGHT_PAD = 10;     // right of the last swatch
constexpr S32 LABEL_GAP = 14;     // at least this between the label and the first swatch
constexpr S32 EXTRA_HEIGHT = 4;   // taller than a text item, for the swatches
constexpr S32 ROW_WIDTH = LLMinimapMarks::COLORS * SWATCH + (LLMinimapMarks::COLORS - 1) * GAP;
}

GMDXMenuSwatchRow::GMDXMenuSwatchRow(const Params& p)
:   LLMenuItemCallGL(p)
{
}

S32 GMDXMenuSwatchRow::swatchLeft(S32 index) const
{
    return getRect().getWidth() - RIGHT_PAD - ROW_WIDTH + index * (SWATCH + GAP);
}

S32 GMDXMenuSwatchRow::swatchAt(S32 x) const
{
    for (S32 i = 0; i < LLMinimapMarks::COLORS; ++i)
    {
        const S32 left = swatchLeft(i);
        if (x >= left - GAP / 2 && x < left + SWATCH + (GAP + 1) / 2)
        {
            return i;
        }
    }
    return -1;
}

U32 GMDXMenuSwatchRow::getNominalWidth() const
{
    return LABEL_LEFT + getFont()->getWidth(mLabel.getWString().c_str()) + LABEL_GAP + ROW_WIDTH + RIGHT_PAD;
}

U32 GMDXMenuSwatchRow::getNominalHeight() const
{
    return LLMenuItemCallGL::getNominalHeight() + EXTRA_HEIGHT;
}

LLSD GMDXMenuSwatchRow::getValue() const
{
    return LLSD(mHover);
}

void GMDXMenuSwatchRow::draw()
{
    const bool enabled = getEnabled();
    const S32 height = getRect().getHeight();
    const LLColor4 text = enabled ? mEnabledColor.get() : mDisabledColor.get();
    getFont()->renderUTF8(mLabel.getString(), 0, (F32)LABEL_LEFT, (F32)height * 0.5f, text,
                          LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);

    // (LLMenuGL tracks hover itself and does not pass it to its items: read the mouse here)
    S32 hover = -1;
    if (getHighlight())
    {
        S32 mx = 0, my = 0;
        LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
        if (my >= 0 && my < height) hover = swatchAt(mx);
    }
    const S32 bottom = (height - SWATCH) / 2;
    const LLColor4 edge(0.f, 0.f, 0.f, enabled ? 0.6f : 0.25f);
    for (S32 i = 0; i < LLMinimapMarks::COLORS; ++i)
    {
        const S32 left = swatchLeft(i);
        LLColor4 c = LLMinimapMarks::color(i);
        if (!enabled) c.mV[VALPHA] = 0.35f;
        if (enabled && i == hover)
        {
            // the one under the mouse: a white ring around it
            gl_rect_2d(left - 3, bottom + SWATCH + 3, left + SWATCH + 3, bottom - 3, LLColor4::white);
            gl_rect_2d(left - 1, bottom + SWATCH + 1, left + SWATCH + 1, bottom - 1, edge);
        }
        else
        {
            gl_rect_2d(left - 1, bottom + SWATCH + 1, left + SWATCH + 1, bottom - 1, edge);
        }
        gl_rect_2d(left, bottom + SWATCH, left + SWATCH, bottom, c);
    }
}

bool GMDXMenuSwatchRow::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = swatchAt(x);
    return LLMenuItemCallGL::handleHover(x, y, mask);
}

void GMDXMenuSwatchRow::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLMenuItemCallGL::onMouseLeave(x, y, mask);
}

bool GMDXMenuSwatchRow::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mHover = swatchAt(x);
    if (mHover < 0 || !getEnabled())
    {
        return true;   // beside the swatches: nothing, the menu stays open
    }
    return LLMenuItemCallGL::handleMouseUp(x, y, mask);
}

void GMDXMenuSwatchRow::onCommit()
{
    // (Return on the row, with no swatch under the mouse: nothing to pick)
    if (mHover >= 0)
    {
        LLMenuItemCallGL::onCommit();
    }
}

bool GMDXMenuSwatchRow::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 i = swatchAt(x);
    if (i < 0)
    {
        return LLMenuItemCallGL::handleToolTip(x, y, mask);
    }
    LLRect sticky;
    localRectToScreen(LLRect(swatchLeft(i), getRect().getHeight(), swatchLeft(i) + SWATCH, 0), &sticky);
    LLToolTipMgr::instance().show(LLToolTip::Params().message(LLMinimapMarks::colorName(i)).sticky_rect(sticky));
    return true;
}
