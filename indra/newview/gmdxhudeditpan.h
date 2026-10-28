/**
 * @file gmdxhudeditpan.h
 * @brief HUD edit: keeps the manipulator on screen, shows the real screen bounds.
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

// <GMDX> While a HUD attachment is edited (GMDXHUDEditKeepGizmoOnScreen), the HUD edit pan
// (LLAgentCamera::mHUDTargetPan, #413) is clamped so the manipulator's centre stays on screen
// with room for its arms, at any HUD zoom. Every HUD consumer (get_hud_matrices, mousePointHUD,
// the manipulators) already reads the eased pan, so drawing and picking move together.
//
// With GMDXHUDEditShowBounds, the same edit also draws where the real screen is in the zoomed /
// panned view: the normal HUD view's outline, centre lines, the HUD attachment points with
// their names, and a red (off screen) or orange (over the edge) box on each selected prim
// that the normal view does not fully show.

#pragma once

class LLVector3;

// Clamp 'pan' (HUD-local, as mHUDTargetPan) so the selection's manipulator centre shows on
// screen at HUD zoom 'zoom' (as mHUDCurZoom, HUDScaleFactor included). Call only while editing
// a HUD selection. Leaves 'pan' alone while a drag holds the mouse.
void gmdx_keep_hud_gizmo_on_screen(LLVector3& pan, float zoom);

// The screen bounds overlay, in the UI pass (LLViewerWindow::draw, scaled UI pixels).
void gmdx_draw_hud_edit_bounds();
