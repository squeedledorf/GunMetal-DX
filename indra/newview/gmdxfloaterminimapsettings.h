/**
 * @file gmdxfloaterminimapsettings.h
 * @brief GunMetal-DX mini-map settings window.
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

// <GMDX> Map Settings (floater_gmdx_minimap_settings.xml): the mini-map's display settings and the
// Region Map tools, which used to fill its right-click menu. Opened from that menu (Map Settings...)
// or the mini-map's gear button. Three tabs: View (view, zoom, rotation, compass, background,
// opacity), Show (what the map draws) and Region Map (capture and the map files). The checkboxes
// bind their settings in XUI; the lists and buttons call what the menu items did (LLNetMap's zoom,
// orientation and re-center, LLRegionMapCapture, GMDXMapShare). Zoom, view and re-center are the
// map's that opened it (the People floater's map has no Whole Region), else the mini-map's.

#pragma once

#include "llfloater.h"

class LLButton;
class LLComboBox;
class LLNetMap;

class GMDXFloaterMinimapSettings : public LLFloater
{
public:
    GMDXFloaterMinimapSettings(const LLSD& key);

    bool postBuild() override;
    void draw() override;

    static void showFor(LLNetMap* map);

private:
    LLNetMap* map() const;
    void updateControls();          // every frame: the lists follow the settings, the buttons what applies
    void fillRotation(bool whole);  // Follow Me: north / camera at top; Whole Region: north / three ways to turn

    LLHandle<LLView> mMap;
    LLComboBox* mView = nullptr;
    LLComboBox* mZoom = nullptr;
    LLComboBox* mRotation = nullptr;
    LLComboBox* mCompass = nullptr;
    LLComboBox* mBackground = nullptr;
    LLComboBox* mOpacity = nullptr;
    LLButton* mUnblock = nullptr;
    S32 mRotationFor = -1;          // the view the Rotation list was filled for (0 Follow Me, 1 Whole Region)
    S32 mBlocked = -1;              // the blocked count on the Unblock button
};
