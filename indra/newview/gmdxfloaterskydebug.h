/**
 * @file gmdxfloaterskydebug.h
 * @brief GunMetal-DX Sky Debugger window (Develop > Rendering > Sky Debugger...).
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

// <GMDX> Sky Debugger (floater_gmdx_sky_debug.xml): where the sky's deep tuning lives instead of
// user settings. Live sliders on the constants (gmdxskytune.h: the volumetric clouds' light, shape
// and march, the night's moonlight, exposure curve and corona, the stars and space at altitude),
// in memory and never saved, with Reset All; the clouds' views (unlit density, self shadow, the
// weather map, world coverage, cloud shadow map, panorama, the physical sky's LUTs); and a readout
// of last frame's sky (sun / moon, the night light, the cloud module's state). Separate from Sky
// Test (World > Environment), which is for players.

#pragma once

#include "llfloater.h"

#include <vector>

class LLComboBox;
class LLTextBase;

class GMDXFloaterSkyDebug : public LLFloater
{
public:
    GMDXFloaterSkyDebug(const LLSD& key);

    bool postBuild() override;
    void draw() override;

private:
    void followConstants();   // every untouched knob's slider shows its constant
    void updateReadout();

    LLComboBox* mView = nullptr;
    LLTextBase* mReadout = nullptr;
    F64 mNextReadout = 0.0;
    std::vector<F32> mInitial;   // each knob slider's XUI value (its constant)
};
