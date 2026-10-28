/**
 * @file gmdxtoolbarcluster.h
 * @brief Where each toolbar's buttons sit along its strip: around the chat log, and moved by grips.
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

// <GMDX> TOOLBAR BUTTON CLUSTERS. Each toolbar (left / right / bottom / top) lays its buttons out in
// a part of its strip, by giving the toolbar view that part as its rect along the strip (the toolbar's
// own centring and wrapping then work inside it):
//  - the chat log unit (GMDXChatLog::unitPlace: the panel and the chat bar under it) may sit anywhere,
//    into the corners; where it lies over a strip, that toolbar's buttons keep to the free part of the
//    strip on one side of it, never under it. They wrap into another row / column, as on a small
//    window, when no side is long enough; the strip goes back to normal once the unit is away;
//  - centred toolbars have a grip at each end of their buttons, shown while the mouse is over the
//    toolbar: dragging it slides the buttons along the strip (they stop at the chat unit's edge),
//    double-clicking it puts them back in the middle. The offset from the middle of the strip is
//    kept per account (GMDXToolbarClusterOffsets: {left, right, bottom, top} in pixels, toward the
//    top / right). Left- / right-aligned and filled toolbars keep their layout inside the free part.
// The rect only changes when the unit, the strip, the buttons or the offset change.

#pragma once

namespace GMDXToolbarCluster
{
    // LLToolBarView::draw, before the toolbars lay themselves out.
    void update();
}
