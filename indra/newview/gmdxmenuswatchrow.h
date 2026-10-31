/**
 * @file gmdxmenuswatchrow.h
 * @brief GunMetal-DX menu row of colour swatches.
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

// <GMDX> One menu row: its label, then the six mark colours (LLMinimapMarks::color) as swatches.
// Clicking a swatch runs the row's on_click with that swatch's index (0..5) as the value, so the
// on_click takes no parameter. The swatch under the mouse is outlined and its colour's name is the
// tooltip; a click beside the swatches does nothing and leaves the menu open.
//   <gmdx_swatch_row label="Colour" name="Mark Colour">
//       <gmdx_swatch_row.on_click function="Minimap.Mark.Color" />
//   </gmdx_swatch_row>

#pragma once

#include "llmenugl.h"

class GMDXMenuSwatchRow : public LLMenuItemCallGL
{
public:
    struct Params : public LLInitParam::Block<Params, LLMenuItemCallGL::Params>
    {
    };

protected:
    GMDXMenuSwatchRow(const Params& p);
    friend class LLUICtrlFactory;

public:
    void draw() override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    void onCommit() override;
    LLSD getValue() const override;
    U32 getNominalWidth() const override;
    U32 getNominalHeight() const override;

private:
    S32 swatchLeft(S32 index) const;
    S32 swatchAt(S32 x) const;   // -1: none

    S32 mHover = -1;
};
