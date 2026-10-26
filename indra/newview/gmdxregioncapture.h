/**
 * @file gmdxregioncapture.h
 * @brief Region map capture: the current region rendered straight down, orthographic, as a map.
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

// <GMDX> REGION MAP CAPTURE (stage 1). On request (mini-map menu "Capture Region Map..."), the
// region the agent is in is rendered by the Vulkan renderer through an orthographic box looking
// straight down, at 4096 x 4096, in the current environment's light:
//  - confirm: the user is told that the region loads at its highest detail and the draw distance goes to N m;
//  - prepare: settings are overridden for the capture only (LLControlVariable::setValue(v, false),
//    never saved; the active graphics preset is kept): draw distance, scene-load thresholds, the
//    terrain / tree LOD factors, and the effects that need a perspective view or would differ
//    between tiles (AO, SSR, GI, fog, clouds, god rays, the weather veil, ray-traced sun /
//    reflections, the terrain bake). Bloom, glow, auto exposure and the lens / grain effects are
//    off in the orthographic renders themselves (Photo Studio Looks are not touched). It does not
//    start, and stops, while Preferences or a graphics settings window is open (they would save
//    the temporary values). Objects in the region load their
//    highest LOD (LLVOVolume::calcLOD -> forceMaxLOD), the object cache gives up the whole region
//    (LLVOCachePartition::cull -> cacheSphere), and small orthographic renders at the map's resolution
//    ask the texture streamer for what the map needs, until the loading signals settle;
//  - render: N x N tiles with a guard band, one per frame (llappviewer.cpp vkRenderRegionOrthoView),
//    without avatars, attachments, rigged mesh, animesh and particles, cropped into one image;
//  - save: PNG + JSON sidecar under <user app dir>/region_maps/<grid id>/<x>_<y>.png, encoded and
//    written on a worker thread. Every overridden setting is restored exactly once the render is
//    done, and on cancel, failure, teleport, region change, logout or quit.
// The mini-map shows the captured map for a region while GMDXMinimapUseCapturedMaps is on
// (GMDXMinimapTerrain::getTexture).

#pragma once

#include "llpointer.h"
#include "stdtypes.h"
#include "v3math.h"

#include <functional>
#include <string>
#include <vector>

class LLImageRaw;
class LLViewerRegion;
class LLViewerTexture;

// One orthographic render (llappviewer.cpp vkRenderRegionOrthoView). Agent space, z up; the view
// looks down -Z with +Y (north) up and +X (east) to the right, so the image's first row (LLImageRaw
// is bottom-up) is the south edge.
struct GMDXOrthoView
{
    LLVector3 eye;                       // above the ceiling
    F32 left = 0.f, right = 0.f;         // the box, relative to the eye (m, x east)
    F32 bottom = 0.f, top = 0.f;         // (m, y north)
    F32 ceiling = 0.f, floor = 0.f;      // agent-space z of the box's top (the near plane) and bottom
    F32 demand_ppm = 0.f;                // texel demand / cluster LOD in px per metre (0 = this render's own)
    F32 time = 0.f;                      // world clock (water waves, animated textures): one for every tile
    S32 width = 0, height = 0;
    U32 frames = 1;                      // recordings (warm-up for the history passes)
    U32 aa_frames = 0;                   // supersampled over this many jittered recordings (vkSnapshotFit)
    bool shadows = true;                 // sun shadows (GMDXRegionMapShadows)
};

// The world through `view` as bottom-up RGB (llappviewer.cpp, next to vkRenderOffscreenSnapshot);
// with `depth`, also the raw [0,1] depth, rows top-down (1 = the ceiling, 0 = the floor; never
// antialiased).
bool vkRenderRegionOrthoView(const GMDXOrthoView& view, std::vector<U8>& rgb, std::vector<F32>* depth = nullptr);

class LLFloater;
class LLSD;
class LLUUID;

class GMDXRegionCapture
{
public:
    // Mini-map menu: "Capture Region Map..." opens the capture window (openWindow: height slider,
    // then Capture) which asks first (requestCapture); canCapture: whether it can run now.
    static void openWindow();
    static void requestCapture();
    static bool canCapture();
    // A capture is preparing or rendering (mini-map menu "Cancel Region Map Capture").
    static bool isRunning();
    static void cancelCapture();
    // Mini-map menu, Region Map: delete this region's capture (back to the chosen background),
    // and open the folder the captures are saved in.
    static void deleteCapture(const LLViewerRegion* region);
    static void openMapsFolder();
    // Sharing (mini-map menu, Region Map): export this region's map as one .gmdxmap file, and
    // import one someone else exported (checked against this grid and the region at its
    // coordinates; replacing a map of that region asks first).
    static void exportMap(const LLViewerRegion* region);
    static void importMap();
    // In world: upload this region's map as a texture (the grid's largest upload size; asks with the
    // cost first), named "Region map: <region>" with the region in its description; and, from an
    // inventory texture whose description isMapTexture, fetch it and install it as that region's map
    // (the same checks as importMap).
    static void uploadMap(const LLViewerRegion* region);
    static bool isMapTexture(const std::string& description);
    static void useMapTexture(const LLUUID& asset_id, const std::string& name, const std::string& description);
    // Stop on the next frame (restoring every setting), with the reason in the notification:
    // for code that must not change settings where it runs (the renderer's VRAM hard step).
    static void requestStop(const std::string& why);
    // The capture's ceiling for the region: the avatar's altitude + GMDXRegionMapCeilingOffset,
    // never below the region's highest terrain + 1 m. hills: set when the terrain decided it.
    static F32 ceilingFor(const LLViewerRegion* region, bool* hills = nullptr);
    // LLFloaterReg build function of the capture window ("gmdx_region_map").
    static LLFloater* buildWindow(const LLSD& key);
    // One step per frame, after the frame is rendered (llappviewer.cpp displayVulkanUI).
    static void frameTick();
    // Quit: restore every overridden setting before they are saved (LLAppViewer::cleanup).
    static void shutdown();

    // Captured maps (for the mini-map and its other tools). hasCapture: a PNG exists for this
    // region; getCapturedImage: loads it (bottom-up RGB, north up; the region's full width).
    static bool hasCapture(const LLViewerRegion* region);
    static bool getCapturedImage(const LLViewerRegion* region, LLPointer<LLImageRaw>& out);
    // The same, decoded on a worker thread; done runs on the main thread (null: unreadable).
    // False, without calling done, when the region has no captured map.
    static bool loadCapturedImage(const LLViewerRegion* region, std::function<void(LLPointer<LLImageRaw>)> done);
    // The mini-map's background for the region, null while there is none (or it is still loading,
    // or GMDXMinimapUseCapturedMaps is off).
    static LLViewerTexture* getMinimapTexture(const LLViewerRegion* region);
    // The World Map (GMDXWorldMapUseCapturedMaps): calls draw(handle, tex, width in metres) for every region of this
    // grid with a captured map that visible(handle) says is on screen and whose 512 px texture is
    // loaded (loading the others on a worker; the folder is looked at again every few seconds).
    // The map lays them over its own tiles.
    static void forEachWorldMapTexture(const std::function<bool(U64 region_handle)>& visible,
                                       const std::function<void(U64 region_handle, LLViewerTexture* tex, F32 region_width)>& draw);

    // LLVOVolume::calcLOD: an object at pos_agent (bounding radius) is in the region being mapped,
    // so it takes its highest LOD.
    static bool forceMaxLOD(const LLVector3& pos_agent, F32 radius)
    {
        return sLodOn && pos_agent.mV[VX] + radius >= sLodBox[0] && pos_agent.mV[VX] - radius <= sLodBox[2]
                      && pos_agent.mV[VY] + radius >= sLodBox[1] && pos_agent.mV[VY] - radius <= sLodBox[3];
    }
    // Changes when forceMaxLOD starts or stops (the ingest pre-signature re-runs calcLOD then).
    static U32 lodEpoch() { return sLodEpoch; }
    // LLVOCachePartition::cull: while a capture runs, the sphere (agent space) around the box of
    // the region being mapped whose cache entries come in; false for every other region.
    static bool cacheSphere(const LLViewerRegion* region, LLVector3& center_agent, F32& radius);

private:
    friend struct GMDXRegionCaptureState;
    static bool sLodOn;
    static F32  sLodBox[4];   // agent-space x0, y0, x1, y1
    static U32  sLodEpoch;
};
