/**
 * @file gmdxminimapimage.h
 * @brief GunMetal-DX: the region map image behind the mini-map's Copy / Save Map Image.
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

// <GMDX> The mini-map menu's "Copy Map Image" and "Save Map Image As...". The image is the
// region's map: its captured region map (GMDXRegionCapture) while GMDXMinimapUseCapturedMaps is
// on and there is one, else the server's world map tile.

#pragma once

#include "llpointer.h"
#include <functional>

class LLImageRaw;
class LLViewerRegion;

// Gets the map image of `regionp` and calls `done` with a copy of it, or with null when it
// could not be loaded. `done` may run before this returns or on a later frame. Returns false,
// without calling `done`, when the region has no map image to load.
bool gmdx_get_region_map_image(LLViewerRegion* regionp, std::function<void(LLPointer<LLImageRaw>)> done);

// The menu actions, for the agent's region: put the map image on the clipboard, or save it as
// a PNG chosen in a file picker. Failures are reported in an alert.
void gmdx_copy_region_map_image();
void gmdx_save_region_map_image();
