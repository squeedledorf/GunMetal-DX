/**
 * @file gmdxregioncapture.cpp
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

// <GMDX> whole file: see gmdxregioncapture.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxregioncapture.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcleaningest.h"
#include "lldate.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "SDL3/SDL.h"
#include "lluri.h"
#include "lltextbox.h"
#include "llfloaterreg.h"
#include "llfloaterpreference.h"
#include "llpresetsmanager.h"
#include "llimagej2c.h"
#include "llimagepng.h"
#include "llmeshrepository.h"
#include "llnotificationsutil.h"
#include "llregionhandle.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "llsurface.h"
#include "lltexturefetch.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llviewertexturelist.h"
#include "llworld.h"
#include "llworldclock.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "vkgfx/llvulkancontext.h"
#include "workqueue.h"

#include <boost/json.hpp>
#include <algorithm>
#include <map>

U32 vkSnapshotAAFrames();                            // llappviewer.cpp
U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames);    // llappviewer.cpp

bool GMDXRegionCapture::sLodOn = false;
F32  GMDXRegionCapture::sLodBox[4] = {0.f, 0.f, 0.f, 0.f};
U32  GMDXRegionCapture::sLodEpoch = 0;

namespace
{
constexpr S32 kMapSize      = 4096;   // output px (square)
constexpr S32 kGuardPx      = 16;     // per tile side, rendered and cropped away
// The eye's height above the ceiling: view rays within a few degrees of vertical, var-regions too.
F32 eyeAbove(F32 region_width) { return llmax(1000.f, 8.f * region_width); }
constexpr F32 kFloorMargin  = 2.f;    // m below the lowest land / water
constexpr F32 kFarMargin    = 32.f;   // m past the farthest corner
constexpr F32 kNetFarMax    = 512.f;  // the simulator's object range cap (LLAgentCamera::getNetworkDrawDistance)
constexpr S32 kProbeSize    = 512;    // preparation render px (without a window size)
constexpr F64 kProbeEvery   = 1.5;    // s (the texture demand holds 2 s)
constexpr F64 kMinPrepare   = 6.0;    // s before the loading signals are believed
constexpr F64 kSettleHold   = 5.0;    // s the signals must hold
constexpr S32 kMeshPendingOk = 2;     // mesh requests still out (a missing asset never answers)
constexpr S32 kTexPendingOk  = 8;     // world textures the renderer wants still fetching / decoding / sharpening
// A shared map file (exportMap / importMap): this line, one line of JSON (the sidecar's fields +
// grid, region_grid_x / region_grid_y, payload "j2c" / "png", payload_size), then the image file.
const std::string kShareMagic = "GMDXMAP1\n";
// An uploaded map texture's description: "GMDXMAP1 <grid x> <grid y> <region width> <region id>"
// (the inventory description holds 127 characters), and its name: the prefix + the region's name.
const std::string kTextureTag = "GMDXMAP1 ";
const std::string kTextureNamePrefix = "Region map: ";
constexpr S64 kShareMaxBytes = 96 * 1024 * 1024;
constexpr S32 kShareMinPx = 256, kShareMaxPx = 8192;

// Windows that snapshot settings and write the snapshot back as saved values (Preferences'
// Cancel / OK, the Advanced graphics window's close, saving a graphics preset): open during
// a capture they would save its temporary values.
const char* openGraphicsWindow()
{
    static const char* const kNames[] = { "preferences", "prefs_graphics_advanced", "performance", "save_pref_preset" };
    for (const char* n : kNames)
        if (LLFloaterReg::instanceVisible(n)) return n;
    return nullptr;
}

// Overridden settings must not clear the active graphics / Looks preset (LLPresetsManager
// treats every change of a preset's setting as the user leaving it).
struct PresetsQuiet
{
    PresetsQuiet()  { if (LLPresetsManager::instanceExists()) LLPresetsManager::getInstance()->setIgnoreChangeSignal(true); }
    ~PresetsQuiet() { if (LLPresetsManager::instanceExists()) LLPresetsManager::getInstance()->setIgnoreChangeSignal(false); }
};

// World textures the renderer wants that are not yet at the wanted sharpness (fetching,
// waiting to be created, sharpening, demand just changed). Avatars' textures are left out:
// they are not in the map and keep loading in a crowd.
S32 texturesUnsettled()
{
    S32 n = 0;
    for (auto it = gTextureList.begin(); it != gTextureList.end(); ++it)
    {
        LLViewerFetchedTexture* t = it->get();
        if (!t || !t->vkDemandActive()) continue;
        const S32 boost = t->getBoostLevel();
        if (boost == LLGLTexture::BOOST_AVATAR || boost == LLGLTexture::BOOST_AVATAR_BAKED
            || boost == LLGLTexture::BOOST_AVATAR_SELF || boost == LLGLTexture::BOOST_AVATAR_BAKED_SELF)
            continue;
        const U8 why = t->vkUnsettledReason();
        if (why == LLViewerFetchedTexture::VKU_FETCHING || why == LLViewerFetchedTexture::VKU_CREATE
            || why == LLViewerFetchedTexture::VKU_SHARPEN || why == LLViewerFetchedTexture::VKU_DEMAND_CHANGED)
            ++n;
    }
    return n;
}

enum class Stage { Idle, Confirm, Prepare, Render, Save };

struct Override
{
    std::string name;
    LLSD original, original_saved, value;
};

struct MapTex
{
    LLPointer<LLViewerTexture> tex;
    bool loading = false;
};
} // namespace

// The capture in progress (one at a time) and the mini-map's loaded maps.
struct GMDXRegionCaptureState
{
    Stage stage = Stage::Idle;
    U64 handle = 0;
    LLUUID region_id;
    std::string region_name;
    F32 width = 256.f;
    F32 ceiling = 0.f, floor = 0.f;
    F32 water = 0.f;
    bool ocol_water = false;    // GMDXRegionMapOcolWater, for the whole capture
    bool shadows = true;        // GMDXRegionMapShadows, for the whole capture
    F32 far_needed = 0.f, far_reach = 0.f;
    std::vector<Override> overrides;
    F64 start = 0.0, settled_since = -1.0, next_probe = 0.0, count_at = 0.0, next_log = 0.0;
    S32 count = -1;
    U32 probes = 0;
    bool timed_out = false;
    S32 tiles = 2, tile_px = kMapSize / 2, next = 0;
    U32 aa = 0;
    F32 time = 0.f;
    LLPointer<LLImageRaw> image;
    LLNotificationPtr progress;
    std::string stop_request;   // requestStop: stop on the next frame with this reason
    bool far_capped_told = false;
    S32 textures = 0;
    F64 next_tex_scan = 0.0;
    std::map<U64, MapTex> maps;
    F64 next_prune = 0.0;

    static GMDXRegionCaptureState& get() { static GMDXRegionCaptureState s; return s; }

    // ---- paths ----
    static std::string mapDir()
    {
        const std::string& delim = gDirUtilp->getDirDelimiter();
        std::string grid = LLDir::getScrubbedFileName(LLGridManager::getInstance()->getGridId());
        if (grid.empty()) grid = "grid";
        return gDirUtilp->getOSUserAppDir() + delim + "region_maps" + delim + grid;
    }
    static std::string mapPath(U64 region_handle, const char* ext)
    {
        U32 x = 0, y = 0;
        from_region_handle(region_handle, &x, &y);
        return mapDir() + gDirUtilp->getDirDelimiter() + llformat("%u_%u.%s", x, y, ext);
    }
    // The saved map of a region: the JPEG 2000 file, or a PNG an older build saved; "" = none.
    static std::string mapFile(U64 region_handle)
    {
        const std::string j2c = mapPath(region_handle, "j2c");
        if (LLFile::isfile(j2c)) return j2c;
        const std::string png = mapPath(region_handle, "png");
        return LLFile::isfile(png) ? png : std::string();
    }

    // ---- why a capture cannot run now ("" = it can) ----
    static std::string blocked()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || gDisconnected || !gAgent.getRegion())
            return "You need to be in a region.";
        if (!LLVulkanContext::instance().isInitialized())
            return "Region maps need the Vulkan renderer.";
        if (RlvActions::isRlvEnabled()
            && (RlvActions::hasBehaviour(RLV_BHVR_SETSPHERE) || RlvActions::hasBehaviour(RLV_BHVR_SHOWMINIMAP)
                || RlvActions::hasBehaviour(RLV_BHVR_SHOWLOC)
                || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_TEXTURES) || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_AVDISTMAX)
                || RlvActions::hasBehaviour(RLV_BHVR_SETCAM_ORIGINDISTMAX)))
            return "Region maps are not available under your current RLV restrictions.";
        if (openGraphicsWindow())
            return "Close Preferences and the graphics settings windows first: they would save the capture's "
                   "temporary settings as yours.";
        return std::string();
    }
    static void message(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("GMDXRegionMapMessage", args);
    }

    // The draw distance that reaches the region's farthest corner from the agent and from the
    // camera (the object cache selects around the camera), and the part of it the simulator
    // sends objects for.
    static F32 farNeeded(const LLViewerRegion* region)
    {
        const LLVector3 o = region->getOriginAgent();
        const LLVector3 from[2] = { gAgent.getPositionAgent(), gAgentCamera.getCameraPositionAgent() };
        const F32 w = region->getWidth();
        F32 d2 = 0.f;
        for (const LLVector3& a : from)
            for (int c = 0; c < 4; ++c)
            {
                const F32 dx = o.mV[VX] + ((c & 1) ? w : 0.f) - a.mV[VX];
                const F32 dy = o.mV[VY] + ((c & 2) ? w : 0.f) - a.mV[VY];
                d2 = llmax(d2, dx * dx + dy * dy);
            }
        return ceilf(sqrtf(d2) + kFarMargin);
    }
    static F32 netFarCap()
    {
        const F32 s = gSavedSettings.getF32("SLNetDrawDistanceMax");
        return s > 0.f ? llmin(s, kNetFarMax) : kNetFarMax;
    }

    // ---- settings, overridden for the capture only ----
    void setOverride(const char* name, const LLSD& value)
    {
        LLControlVariablePtr c = gSavedSettings.getControl(name);
        if (c.isNull() || llsd_equals(c->getValue(), value)) return;
        overrides.push_back({ name, c->getValue(), c->getSaveValue(), value });
        c->setValue(value, false);   // unsaved: never written to settings.xml
        overrides.back().value = c->getValue();   // (as the control stores it)
    }
    // Every override back, newest first. A setting the user changed meanwhile keeps the user's
    // value; one that Preferences OK saved with our value gets the user's saved value back.
    void restoreAll()
    {
        PresetsQuiet quiet;
        for (auto it = overrides.rbegin(); it != overrides.rend(); ++it)
        {
            LLControlVariablePtr c = gSavedSettings.getControl(it->name);
            if (c.isNull() || !llsd_equals(c->getValue(), it->value)) continue;
            if (llsd_equals(it->original, it->original_saved))
            {
                c->setValue(it->original, true);   // the saved value is the value: no unsaved layer
            }
            else
            {
                if (!llsd_equals(c->getSaveValue(), it->original_saved)) c->setValue(it->original_saved, true);
                c->setValue(it->original, false);
            }
        }
        if (!overrides.empty()) LL_INFOS("GMDXRegionMap") << "restored " << overrides.size() << " settings" << LL_ENDL;
        overrides.clear();
        if (GMDXRegionCapture::sLodOn) ++GMDXRegionCapture::sLodEpoch;
        GMDXRegionCapture::sLodOn = false;
    }

    void applyOverrides()
    {
        PresetsQuiet quiet;
        if (far_needed > gSavedSettings.getF32("RenderFarClip")) setOverride("RenderFarClip", LLSD((F64)far_needed));
        // Everything in range is loaded from the object cache, in view or not.
        setOverride("SceneLoadFrontPixelThreshold", LLSD(0.0));
        setOverride("SceneLoadRearPixelThreshold", LLSD(0.0));
        // Objects in the region take their highest LOD (forceMaxLOD, the cull's CULL_ORTHO); the
        // terrain and tree factors at the Graphics preferences' maximum.
        setOverride("RenderTerrainLODFactor", LLSD(4.0));
        setOverride("RenderTreeLODFactor", LLSD(2.0));
        // Effects a straight-down orthographic view gets wrong or that would differ between tiles.
        setOverride("RenderDeferredSSAO", LLSD(false));             // AO (SSAO and RTAO: RenderVKAOMethod)
        setOverride("RenderScreenSpaceReflections", LLSD(false));
        setOverride("RenderVKRTReflections", LLSD(0));
        setOverride("RenderVKGI", LLSD(0));
        setOverride("RenderVKVolumetricFog", LLSD(0));              // (OCOLHeightFog needs it)
        setOverride("RenderVKParticleFog", LLSD(0));               // (particles are left out of the map)
        setOverride("RenderVKVolumetricClouds", LLSD(0));
        setOverride("RenderVKGodRays", LLSD(0));
        setOverride("RenderVKWeatherVeil", LLSD(false));            // rain / snow haze over the view ray
        setOverride("RenderVKMotionBlur", LLSD(false));
        // the alpha-test boost for small mips (Golus) keeps distant foliage from thinning out; seen
        // from above at the map's scale it turned whole leaf cards into solid squares
        setOverride("RenderVKAlphaMipCoverage", LLSD(0.0));
        // (Bloom / glow, the lens and grain effects and auto exposure are off in the orthographic
        // renders themselves: they are Photo Studio Look settings, whose undo would record them.)
        setOverride("RenderVKTerrainBake", LLSD(false));            // live terrain at every distance
        if (gSavedSettings.getU32("RenderVKSunShadows") == 2) setOverride("RenderVKSunShadows", LLSD(1));   // (the TLAS holds avatars)
        if (gSavedSettings.getU32("RenderVKShadowResolution") < 2048) setOverride("RenderVKShadowResolution", LLSD(2048));
    }

    void setLodBox(const LLViewerRegion* region)
    {
        const LLVector3 o = region->getOriginAgent();
        GMDXRegionCapture::sLodBox[0] = o.mV[VX];
        GMDXRegionCapture::sLodBox[1] = o.mV[VY];
        GMDXRegionCapture::sLodBox[2] = o.mV[VX] + width;
        GMDXRegionCapture::sLodBox[3] = o.mV[VY] + width;
    }

    // ---- the run ----
    void stop(const std::string& why)
    {
        LL_INFOS("GMDXRegionMap") << "capture of " << region_name << " stopped: " << why << LL_ENDL;
        restoreAll();
        image = nullptr;
        stage = Stage::Idle;
        if (progress) { LLNotificationsUtil::cancel(progress); progress.reset(); }
        LLSD args;
        args["REGION"] = region_name;
        args["REASON"] = why;
        LLNotificationsUtil::add("GMDXRegionMapStopped", args);
    }

    void begin()
    {
        LLViewerRegion* region = gAgent.getRegion();
        std::string why = blocked();
        if (why.empty() && (!region || region->getHandle() != handle)) why = "You left the region.";
        stage = Stage::Idle;
        if (!why.empty()) { message(why); return; }
        if (!LLEnvironment::instance().getIsSunUp())
        {
            LLSD args;
            args["REGION"] = region->getName();
            LLNotificationsUtil::add("GMDXRegionMapNight", args);
            return;
        }
        region_id = region->getRegionID();
        region_name = region->getName();
        width = llmax(region->getWidth(), 16.f);
        ceiling = GMDXRegionCapture::ceilingFor(region);
        water = region->getWaterHeight();
        ocol_water = gSavedSettings.getBOOL("GMDXRegionMapOcolWater");
        shadows = gSavedSettings.getBOOL("GMDXRegionMapShadows");
        floor = llmin(region->getLand().getMinZ(), water) - kFloorMargin;
        ceiling = llmax(ceiling, floor + 1.f);
        far_needed = farNeeded(region);
        far_reach = llmin(far_needed, netFarCap());
        applyOverrides();
        setLodBox(region);
        GMDXRegionCapture::sLodOn = true;
        ++GMDXRegionCapture::sLodEpoch;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        start = now;
        settled_since = -1.0;
        next_probe = now;
        next_log = now + 10.0;
        count = -1;
        count_at = now;
        probes = 0;
        timed_out = false;
        stage = Stage::Prepare;
        LL_INFOS("GMDXRegionMap") << "capturing " << region_name << " (" << width << " m): ceiling " << ceiling
                                  << " m, floor " << floor << " m, draw distance " << far_needed << " m (simulator "
                                  << far_reach << " m), " << overrides.size() << " settings overridden" << LL_ENDL;
        far_capped_told = false;
        textures = 0;
        next_tex_scan = now;
        stop_request.clear();
        static LLCachedControl<F32> s_timeout(gSavedSettings, "GMDXRegionMapSettleTimeout", 180.f);
        showProgress(llformat("loading the whole region (up to %.0f s), then rendering it. Everything above %.0f m "
                              "is left out.", llmax((F32)s_timeout, (F32)(kMinPrepare + kSettleHold)), ceiling));
    }

    // The progress notification (stays until the capture ends; its button cancels), re-posted
    // with each phase.
    void showProgress(const std::string& phase)
    {
        if (progress) { LLNotificationsUtil::cancel(progress); progress.reset(); }
        LLSD args;
        args["REGION"] = region_name;
        args["PHASE"] = phase;
        progress = LLNotificationsUtil::add("GMDXRegionMapProgress", args, LLSD(),
            [](const LLSD& n, const LLSD& r)
            {
                GMDXRegionCaptureState& s = get();
                if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                s.progress.reset();   // (responded: nothing to cancel)
                if (s.stage == Stage::Prepare || s.stage == Stage::Render) s.stop("cancelled");
            });
    }

    // Avatars (rigged, worn, animesh) and particles leave the render, as the 360 capture does;
    // with OCOL water, the in-world water too (paintOcolWater paints it from the depth).
    bool renderWithoutAvatars(const GMDXOrthoView& v, std::vector<U8>& rgb, std::vector<F32>* depth = nullptr) const
    {
        const U32 types[] = { LLPipeline::RENDER_TYPE_AVATAR, LLPipeline::RENDER_TYPE_PARTICLES,
                              ocol_water ? (U32)LLPipeline::RENDER_TYPE_WATER : 0u };
        bool off[3] = {};
        for (int k = 0; k < 3; ++k)
            if (types[k] && (off[k] = gPipeline.hasRenderType(types[k]))) LLPipeline::toggleRenderTypeControl(types[k]);
        const bool ok = vkRenderRegionOrthoView(v, rgb, depth);
        for (int k = 0; k < 3; ++k)
            if (off[k]) LLPipeline::toggleRenderTypeControl(types[k]);
        return ok;
    }

    // OCOL water over a rendered tile (bottom-up RGB, w x w, the in-world water left out): every
    // pixel whose top surface (from the tile's depth, rows top-down) lies below the water level
    // takes the Region Terrain Map's water (gmdxminimapterrain.cpp: deep to shallow by depth,
    // relief, foam, shore band, desaturated), with the captured floor showing through in the
    // shallows. Docks, boats and anything else above the water keep their pixels.
    void paintOcolWater(std::vector<U8>& rgb, const std::vector<F32>& depth, S32 w, F32 ppm) const
    {
        constexpr F32 SATURATION = 0.4f, SHORE_GLOW = 0.05f;
        static const F32 tint[4] = { 0.30f, 0.46f, 0.48f, 0.35f };
        static const F32 shallow[3] = { 0.36f, 0.72f, 0.74f }, deep[3] = { 0.05f, 0.17f, 0.33f };
        const F32 span = ceiling - floor;
        auto Z = [&](S32 x, S32 y)   // top-surface height (m) at bottom-up pixel (x, y)
        {
            x = llclamp(x, 0, w - 1); y = llclamp(y, 0, w - 1);
            return floor + depth[(size_t)(w - 1 - y) * w + x] * span;
        };
        LLVector3 sun = LLEnvironment::instance().getSunDirection();
        if (sun.normalize() < 0.5f) sun.set(0.f, 0.f, 1.f);
        const S32 s = llmax(1, ll_round(0.5f * ppm));   // the slope over a metre
        const F32 run = 2.f * s / ppm;
        for (S32 y = 0; y < w; ++y)
            for (S32 x = 0; x < w; ++x)
            {
                const F32 d = water - Z(x, y);
                if (d <= 0.f) continue;
                const F32 dzdx = (Z(x + s, y) - Z(x - s, y)) / run, dzdy = (Z(x, y + s) - Z(x, y - s)) / run;
                const F32 lam = llmax(0.f, (-dzdx * sun.mV[VX] - dzdy * sun.mV[VY] + sun.mV[VZ])
                                           / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.f));
                const F32 relief = 0.85f + 0.15f * lam;
                const F32 td = powf(llclamp(d / 14.f, 0.f, 1.f), 0.6f);
                const F32 tp = powf(llclamp(llmax(d, 4.f) / 14.f, 0.f, 1.f), 0.6f);
                const F32 foam = llclamp(1.f - d / 0.6f, 0.f, 1.f) * 0.35f;
                F32 band = llclamp(1.f - d / 4.f, 0.f, 1.f);
                band = band * band * (3.f - 2.f * band) * tint[3];
                F32 c[3];
                for (S32 k = 0; k < 3; ++k)
                {
                    F32 f = (shallow[k] + (deep[k] - shallow[k]) * td) * relief;
                    f = f * (1.f - foam) + foam;
                    const F32 p = (shallow[k] + (deep[k] - shallow[k]) * tp) * relief;
                    c[k] = p + (f - p) * SHORE_GLOW;
                    c[k] += (tint[k] - c[k]) * band;
                }
                const F32 l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                // the floor through the water: half at the waterline, gone by 3 m
                const F32 see = 0.5f * llclamp(1.f - d / 3.f, 0.f, 1.f);
                U8* px = rgb.data() + ((size_t)y * w + x) * 3;
                for (S32 k = 0; k < 3; ++k)
                {
                    const F32 wc = l + (c[k] - l) * SATURATION;
                    px[k] = (U8)llclamp(ll_round((wc + (px[k] / 255.f - wc) * see) * 255.f), 0, 255);
                }
            }
    }

    // The view of region-local [x0, x1] x [y0, y1] (m), from one eye above the region's middle
    // for every tile (the specular / view-dependent terms then match across the seams).
    GMDXOrthoView view(const LLViewerRegion* region, F32 x0, F32 y0, F32 x1, F32 y1, S32 w, S32 h) const
    {
        const LLVector3 o = region->getOriginAgent();
        GMDXOrthoView v;
        v.eye.set(o.mV[VX] + 0.5f * width, o.mV[VY] + 0.5f * width, ceiling + eyeAbove(width));
        v.left = o.mV[VX] + x0 - v.eye.mV[VX];
        v.right = o.mV[VX] + x1 - v.eye.mV[VX];
        v.bottom = o.mV[VY] + y0 - v.eye.mV[VY];
        v.top = o.mV[VY] + y1 - v.eye.mV[VY];
        v.ceiling = ceiling;
        v.floor = floor;
        v.demand_ppm = (F32)kMapSize / width;
        v.shadows = shadows;
        v.width = w;
        v.height = h;
        return v;
    }

    void prepareTick(LLViewerRegion* region)
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        setLodBox(region);
        // A small render of the whole region at the map's texel demand: the texture streamer
        // and the cluster pages then load what the map needs, wherever the camera looks.
        if (now >= next_probe)
        {
            next_probe = now + kProbeEvery;
            // At the window's size (the renderer's targets then need no resize), the region
            // widened along the window's longer side.
            S32 pw = kProbeSize, ph = kProbeSize;
            if (gViewerWindow)
            {
                pw = llclamp(gViewerWindow->getWindowWidthRaw(), 64, 4096);
                ph = llclamp(gViewerWindow->getWindowHeightRaw(), 64, 4096);
            }
            const F32 bw = pw > ph ? width * pw / ph : width, bh = ph > pw ? width * ph / pw : width;
            GMDXOrthoView v = view(region, 0.5f * (width - bw), 0.5f * (width - bh), 0.5f * (width + bw),
                                   0.5f * (width + bh), pw, ph);
            v.time = (F32)LLWorldClock::getTime();
            std::vector<U8> rgb;
            if (renderWithoutAvatars(v, rgb)) ++probes;
        }
        // Loading signals: the simulator's range, the object count, the ingest sweep, meshes, textures.
        const S32 n = gObjectList.getNumObjects();
        if (count < 0 || abs(n - count) > llmax(4, count / 200)) { count = n; count_at = now; }
        // Far is never sent above the EFFECTIVE draw distance (llviewermessage.cpp), which the
        // VRAM ladder's far-geometry rung may hold below RenderFarClip (vramDrawDistance).
        const F32 far_target = llmin(far_reach, gAgentCamera.mDrawDistance);
        if (gAgentCamera.mDrawDistance + 1.f < far_needed && !far_capped_told)
        {
            far_capped_told = true;
            LL_WARNS("GMDXRegionMap") << "draw distance held at " << gAgentCamera.mDrawDistance << " m (needed "
                                      << far_needed << " m): the video memory limit" << LL_ENDL;
            message(llformat("The video memory limit holds the draw distance at %.0f m, so the parts of %s farther "
                             "than that from you may be missing from the map.", gAgentCamera.mDrawDistance,
                             region_name.c_str()));
        }
        const bool far_ok = gAgentCamera.getNetFarSent() + 1.f >= far_target;
        const bool objects_ok = now - count_at >= kSettleHold;
        const bool ingest_ok = LLCleanIngest::instance().isSweepIdle();
        const S32 meshes = LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests
                         + (S32)LLMeshRepository::sLODPending;
        if (now >= next_tex_scan) { next_tex_scan = now + 1.0; textures = texturesUnsettled(); }
        const bool ok = far_ok && objects_ok && ingest_ok && meshes <= kMeshPendingOk && textures <= kTexPendingOk
                     && probes >= 3 && now - start >= kMinPrepare;
        if (!ok) settled_since = -1.0;
        else if (settled_since < 0.0) settled_since = now;
        static LLCachedControl<F32> s_timeout(gSavedSettings, "GMDXRegionMapSettleTimeout", 180.f);
        timed_out = now - start >= llmax((F64)(F32)s_timeout, kMinPrepare + kSettleHold);
        if (now >= next_log || timed_out || (settled_since >= 0.0 && now - settled_since >= kSettleHold))
        {
            next_log = now + 10.0;
            LL_INFOS("GMDXRegionMap") << "preparing " << region_name << " " << (S32)(now - start) << " s: far sent "
                                      << gAgentCamera.getNetFarSent() << "/" << far_target << " m, objects " << n
                                      << (objects_ok ? " (steady)" : "") << ", ingest " << (ingest_ok ? "idle" : "busy")
                                      << ", mesh requests " << meshes << ", world textures loading " << textures
                                      << ", preparation renders " << probes << LL_ENDL;
        }
        if (timed_out || (settled_since >= 0.0 && now - settled_since >= kSettleHold)) startRender();
    }

    void startRender()
    {
        if (!LLEnvironment::instance().getIsSunUp()) { stop("the sun went down"); return; }
        // The fewest N x N tiles (at least 2 x 2, for the shadow map's resolution) whose tile +
        // guard band renders at full size within the device and memory limits (vkSnapshotFit).
        S32 n = 2;
        U32 frames_aa = 0;
        for (; n <= 16; n *= 2)
        {
            const S32 want = kMapSize / n + 2 * kGuardPx;
            S32 w = want, h = want;
            frames_aa = vkSnapshotFit(w, h, vkSnapshotAAFrames());
            if (w == want && h == want) break;
        }
        if (n > 16) { stop("not enough free memory to render the map"); return; }
        image = new LLImageRaw(kMapSize, kMapSize, 3);
        if (image->isBufferInvalid()) { stop("not enough free memory for the map"); return; }
        tiles = n;
        tile_px = kMapSize / n;
        aa = frames_aa;
        next = 0;
        time = (F32)LLWorldClock::getTime();
        stage = Stage::Render;
        LL_INFOS("GMDXRegionMap") << "rendering " << region_name << ": " << n << " x " << n << " tiles of "
                                  << tile_px + 2 * kGuardPx << " px" << (aa ? llformat(", antialiased x%u", aa) : std::string())
                                  << (timed_out ? " (loading timed out)" : "") << LL_ENDL;
        showProgress(llformat("rendering %d x %d tiles%s.", n, n,
                              timed_out ? " (the region had not finished loading in time)" : ""));
    }

    void renderTick(LLViewerRegion* region)
    {
        const S32 i = next % tiles, j = next / tiles;   // i east, j north
        const F32 ppm = (F32)kMapSize / width;
        const S32 px0 = i * tile_px - kGuardPx, py0 = j * tile_px - kGuardPx;
        const S32 w = tile_px + 2 * kGuardPx;
        GMDXOrthoView v = view(region, px0 / ppm, py0 / ppm, (px0 + w) / ppm, (py0 + w) / ppm, w, w);
        v.time = time;
        v.frames = 2;
        v.aa_frames = aa;
        std::vector<U8> rgb;
        if (!renderWithoutAvatars(v, rgb) || rgb.size() < (size_t)w * w * 3) { stop("a map tile could not be rendered"); return; }
        if (ocol_water)
        {
            // the depth: one more render, unjittered (the same tile and guard band, so the slopes
            // at the tile's edge see across it)
            GMDXOrthoView dv = v;
            dv.aa_frames = 0;
            std::vector<U8> unused;
            std::vector<F32> depth;
            if (!renderWithoutAvatars(dv, unused, &depth) || depth.size() < (size_t)w * w) { stop("a map tile could not be rendered"); return; }
            paintOcolWater(rgb, depth, w, ppm);
        }
        // The tile's centre into the map (both bottom-up: row 0 is the south edge).
        U8* out = image->getData();
        for (S32 r = 0; r < tile_px; ++r)
        {
            const U8* src = rgb.data() + ((size_t)(r + kGuardPx) * w + kGuardPx) * 3;
            U8* dst = out + ((size_t)(j * tile_px + r) * kMapSize + (size_t)i * tile_px) * 3;
            std::memcpy(dst, src, (size_t)tile_px * 3);
        }
        if (++next < tiles * tiles) return;
        save();
    }

    void save()
    {
        restoreAll();   // the render is done: nothing needs the overrides any more
        showProgress("saving the map.");
        stage = Stage::Save;
        const std::string dir = mapDir();
        {
            const std::string& delim = gDirUtilp->getDirDelimiter();
            LLFile::mkdir(gDirUtilp->getOSUserAppDir() + delim + "region_maps");
            LLFile::mkdir(dir);
        }
        // JPEG 2000: visually lossless at ~1:3.5 (about 14 MB for 4096 px; a PNG of these photo-like
        // maps is ~37 MB), or exactly lossless (reversible, ~27 MB) with GMDXRegionMapLossless
        const std::string png_path = mapPath(handle, "j2c"), json_path = mapPath(handle, "json");
        const std::string old_png = mapPath(handle, "png");
        const bool lossless = gSavedSettings.getBOOL("GMDXRegionMapLossless");
        LLSD meta;
        meta["region_name"] = region_name;
        meta["region_id"] = region_id;
        meta["region_width"] = width;
        meta["image_size"] = kMapSize;
        meta["captured_utc"] = LLDate::now().asString();
        meta["ceiling"] = ceiling;
        meta["floor"] = floor;
        meta["ocol_water"] = ocol_water;
        meta["shadows"] = shadows;
        meta["grid"] = LLGridManager::getInstance()->getGridId();
        meta["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
        meta["fully_loaded"] = !timed_out;
        const std::string json = boost::json::serialize(LlsdToJson(meta));
        LLPointer<LLImageRaw> raw = image;
        image = nullptr;
        const U64 h = handle;
        const F32 w = width;
        const std::string name = region_name;
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
        auto work = [raw, png_path, json_path, json, old_png, lossless]() -> std::string
        {
            // (no APR here: llofstream, not LLImageFormatted::save)
            try
            {
                LLPointer<LLImageJ2C> png = new LLImageJ2C();
                png->setReversible(lossless);
                if (!lossless) png->setRate(1.f / 3.5f);
                if (!png->encode(raw, 0.f)) return "the map image could not be encoded";
                // Written next to it, then moved over the old map in one step: a crash or a full
                // disk never leaves a half-written map; the sidecar follows the image.
                const std::string png_tmp = png_path + ".tmp";
                {
                    llofstream f(png_tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                    if (!f.is_open()) return "cannot write " + png_tmp;
                    f.write((const char*)png->getData(), png->getDataSize());
                    f.close();
                    if (f.fail()) { LLFile::remove(png_tmp, ENOENT); return "cannot write " + png_tmp; }
                }
                if (LLFile::replace(png_tmp, png_path) != 0) { LLFile::remove(png_tmp, ENOENT); return "cannot replace " + png_path; }
                const std::string json_tmp = json_path + ".tmp";
                {
                    llofstream j(json_tmp, std::ios::out | std::ios::trunc);
                    if (j.is_open()) j << json << "\n";
                }
                if (LLFile::replace(json_tmp, json_path) != 0) LLFile::remove(json_tmp, ENOENT);
                LLFile::remove(old_png, ENOENT);   // an older build's PNG of the same region
                return std::string();
            }
            catch (const std::exception& e) { return e.what(); }
            catch (...) { return "unknown error"; }
        };
        auto done = [raw, h, w, name, png_path](std::string err)
        {
            GMDXRegionCaptureState& s = get();
            s.stage = Stage::Idle;
            if (s.progress) { LLNotificationsUtil::cancel(s.progress); s.progress.reset(); }
            LLSD args;
            args["REGION"] = name;
            if (!err.empty())
            {
                LL_WARNS("GMDXRegionMap") << "map of " << name << " not saved: " << err << LL_ENDL;
                args["REASON"] = err;
                LLNotificationsUtil::add("GMDXRegionMapStopped", args);
                return;
            }
            LL_INFOS("GMDXRegionMap") << "map of " << name << " saved: " << png_path << LL_ENDL;
            // shown at once, no reload (only while the mini-map uses captured maps: 4096 px of texture)
            if (gSavedSettings.getBOOL("GMDXMinimapUseCapturedMaps")) s.maps[h] = MapTex{ minimapTexture(raw, w) };
            else s.maps.erase(h);
            args["PATH"] = png_path;
            LLNotificationsUtil::add("GMDXRegionMapSaved", args);
        };
        bool posted = false;
        if (main_queue && general)
        {
            posted = main_queue->postTo(general, work, [done](std::string err)
                {
                    if (!LLApp::isExiting()) done(err);
                });
        }
        if (!posted) done(work());   // (no worker: on this thread)
    }

    // ---- maps on disk ----
    static bool readFile(const std::string& path, std::string& bytes)
    {
        llifstream f(path, std::ios::in | std::ios::binary);
        if (!f.is_open()) return false;
        bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return !f.bad();
    }
    static LLPointer<LLImageRaw> loadPng(const std::string& path)   // (a JPEG 2000 or a PNG map)
    {
        std::string bytes;
        if (path.empty() || !readFile(path, bytes)) return nullptr;
        return decodeMap(bytes, path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0);
    }
    static LLPointer<LLImageRaw> decodeMap(const std::string& bytes, bool png_file)
    {
        if (bytes.empty()) return nullptr;
        LLPointer<LLImageFormatted> image = png_file ? (LLImageFormatted*)new LLImagePNG() : (LLImageFormatted*)new LLImageJ2C();
        U8* data = image->allocateData((S32)bytes.size());
        if (!data) return nullptr;
        std::memcpy(data, bytes.data(), bytes.size());
        if (!image->updateData()) return nullptr;
        // (a shared map comes from someone else: its header's size is checked before the decode allocates)
        if (image->getWidth() < kShareMinPx || image->getWidth() > kShareMaxPx || image->getHeight() != image->getWidth())
            return nullptr;
        image->setDiscardLevel(0);   // (the whole image)
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!image->decode(raw, 0.f) || raw->getComponents() < 3) return nullptr;
        return raw;
    }
    static LLPointer<LLViewerTexture> minimapTexture(LLPointer<LLImageRaw> raw, F32 /*region_width*/)
    {
        if (raw.isNull()) return nullptr;
        // (the mini-map draws each region at its own width, var-regions included, so the whole map is used)
        // mipmapped: the mini-map shows the 4096 px map a few hundred pixels across; without mips it shimmers
        LLPointer<LLViewerTexture> tex = LLViewerTextureManager::getLocalTexture(raw.get(), true);
        tex->dontDiscard();
        tex->setAddressMode(LLTexUnit::TAM_CLAMP);
        return tex;
    }
    void loadMinimap(const LLViewerRegion* region, MapTex& e)
    {
        const std::string path = mapFile(region->getHandle());
        if (path.empty()) return;   // (an entry without a texture: nothing to show)
        e.loading = true;
        const U64 h = region->getHandle();
        const F32 w = region->getWidth();
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
        const bool posted = main_queue && general && main_queue->postTo(general,
            [path]() -> LLPointer<LLImageRaw>
            {
                try { return loadPng(path); }
                catch (...) { return nullptr; }
            },
            [h, w](LLPointer<LLImageRaw> raw)
            {
                if (LLApp::isExiting()) return;
                auto it = get().maps.find(h);
                if (it == get().maps.end() || !it->second.loading) return;   // pruned / replaced meanwhile
                it->second.loading = false;
                it->second.tex = minimapTexture(raw, w);
            });
        if (!posted) { e.loading = false; e.tex = minimapTexture(loadPng(path), w); }
    }
    // Maps of regions no longer around free their textures.
    void pruneMaps()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < next_prune || maps.empty()) return;
        next_prune = now + 5.0;
        // Captured maps off on the mini-map: their 4096 px textures go.
        static LLCachedControl<bool> use_captured(gSavedSettings, "GMDXMinimapUseCapturedMaps", true);
        if (!use_captured) { maps.clear(); return; }
        for (auto it = maps.begin(); it != maps.end(); )
            it = LLWorld::getInstance()->getRegionFromHandle(it->first) ? std::next(it) : maps.erase(it);
    }
};

// ---- capture window (the height slider, then Capture) ----

F32 GMDXRegionCapture::ceilingFor(const LLViewerRegion* region, bool* hills)
{
    static LLCachedControl<F32> s_ceiling(gSavedSettings, "GMDXRegionMapCeilingOffset", 50.f);
    const F32 mine = gAgent.getPositionAgent().mV[VZ] + llmax((F32)s_ceiling, 0.f);
    // never below the highest terrain: hills taller than the avatar would be cut into holes (in a skybox
    // the avatar is far above the land, so its altitude still sets the ceiling)
    const F32 land = region ? region->getLand().getMaxZ() + 1.f : mine;
    if (hills) *hills = land > mine;
    return llmax(mine, land);
}

namespace
{
class GMDXFloaterRegionMap : public LLFloater
{
public:
    GMDXFloaterRegionMap(const LLSD& key) : LLFloater(key) {}
    bool postBuild() override
    {
        mReach = getChild<LLTextBox>("reach_text");
        getChild<LLUICtrl>("capture_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
            {
                closeFloater();
                GMDXRegionCapture::requestCapture();
            });
        getChild<LLUICtrl>("cancel_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
        return true;
    }
    void draw() override
    {
        // the reach follows the slider and the avatar's altitude live
        if (LLViewerRegion* region = gAgent.getRegion())
        {
            bool hills = false;
            const F32 ceiling = GMDXRegionCapture::ceilingFor(region, &hills);
            LLStringUtil::format_map_t args;
            args["[CEILING]"] = llformat("%.0f", ceiling);
            args["[ALT]"] = llformat("%.0f", gAgent.getPositionAgent().mV[VZ]);
            mReach->setText(getString(hills ? "reach_hills" : "reach", args));
        }
        getChildView("capture_btn")->setEnabled(GMDXRegionCapture::canCapture());
        LLFloater::draw();
    }
private:
    LLTextBox* mReach = nullptr;
};
}

LLFloater* GMDXRegionCapture::buildWindow(const LLSD& key)
{
    return new GMDXFloaterRegionMap(key);
}

void GMDXRegionCapture::openWindow()
{
    LLFloaterReg::showInstance("gmdx_region_map");
}

// ---- public ----

void GMDXRegionCapture::requestCapture()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (s.stage != Stage::Idle)
    {
        GMDXRegionCaptureState::message("A region map capture is already running.");
        return;
    }
    const std::string why = GMDXRegionCaptureState::blocked();
    if (!why.empty()) { GMDXRegionCaptureState::message(why); return; }
    LLViewerRegion* region = gAgent.getRegion();
    LLSD args;
    args["REGION"] = region->getName();
    if (!LLEnvironment::instance().getIsSunUp())
    {
        LLNotificationsUtil::add("GMDXRegionMapNight", args);
        return;
    }
    const F32 needed = GMDXRegionCaptureState::farNeeded(region);
    const F32 cap = GMDXRegionCaptureState::netFarCap();
    args["DISTANCE"] = llformat("%.0f", llmax(needed, gSavedSettings.getF32("RenderFarClip")));
    args["CEILING"] = llformat("%.0f", GMDXRegionCapture::ceilingFor(region));
    args["PARTIAL"] = needed > cap
        ? llformat("\n\nThe simulator sends objects only up to %.0f m away, so the parts of the region farther than that "
                   "from you may be missing. Stand nearer the middle of the region for a complete map.", cap)
        : std::string();
    s.stage = Stage::Confirm;
    s.handle = region->getHandle();
    LLNotificationsUtil::add("GMDXRegionMapConfirm", args, LLSD(),
        [](const LLSD& n, const LLSD& r)
        {
            GMDXRegionCaptureState& st = GMDXRegionCaptureState::get();
            if (st.stage != Stage::Confirm) return;
            st.stage = Stage::Idle;
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) st.begin();
        });
}

bool GMDXRegionCapture::canCapture()
{
    return GMDXRegionCaptureState::get().stage == Stage::Idle && GMDXRegionCaptureState::blocked().empty();
}

void GMDXRegionCapture::frameTick()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    s.pruneMaps();
    if (s.stage != Stage::Prepare && s.stage != Stage::Render) return;
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromHandle(s.handle);
    std::string why;
    bool prefs = false;
    if (!s.stop_request.empty()) why = s.stop_request;
    else if (LLApp::isExiting() || gDisconnected || LLStartUp::getStartupState() < STATE_STARTED
        || LLAppViewer::instance()->logoutRequestSent())
        why = "you logged out";
    else if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) why = "you teleported";
    else if (!region || gAgent.getRegion() != region) why = "you left the region";
    else if (openGraphicsWindow())
    {
        prefs = true;
        why = "Preferences or a graphics settings window was opened (it would have saved the capture's temporary "
              "settings as yours)";
    }
    else why = GMDXRegionCaptureState::blocked();
    if (!why.empty())
    {
        s.stop(why);
        // Preferences took its Cancel snapshot while the overrides were on: take it again, now
        // that the user's own values are back, or Cancel / closing Advanced would save ours.
        if (prefs)
            if (LLFloaterPreference* p = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
                p->saveSettings();
        return;
    }
    if (s.stage == Stage::Prepare) s.prepareTick(region);
    else s.renderTick(region);
}

void GMDXRegionCapture::shutdown()
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    s.restoreAll();
    s.image = nullptr;
    s.progress.reset();
    s.maps.clear();
    s.stage = Stage::Idle;
}

bool GMDXRegionCapture::hasCapture(const LLViewerRegion* region)
{
    return region && !GMDXRegionCaptureState::mapFile(region->getHandle()).empty();
}

void GMDXRegionCapture::deleteCapture(const LLViewerRegion* region)
{
    if (!region || isRunning()) return;
    const U64 handle = region->getHandle();
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "j2c"), 1);
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "png"), 1);
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, "json"), 1);
    GMDXRegionCaptureState::get().maps.erase(handle);   // the mini-map goes back to its background
    LL_INFOS("GMDXRegionMap") << "deleted the map of " << region->getName() << LL_ENDL;
}

void GMDXRegionCapture::openMapsFolder()
{
    std::string dir = GMDXRegionCaptureState::mapDir();
    LLFile::mkdir(gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "region_maps");
    LLFile::mkdir(dir);
    // (as the snapshot panel: file: URLs are not on spawnWebBrowser's whitelist, so straight
    // to SDL, which hands them to the desktop's file manager)
#if LL_WINDOWS
    std::replace(dir.begin(), dir.end(), '\\', '/');
    std::string url = "file:///";
#else
    std::string url = "file://";
#endif
    static const std::string allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/:";
    url += LLURI::escape(dir, allowed);
    if (!SDL_OpenURL(url.c_str()))
    {
        LL_WARNS("GMDXRegionMap") << "could not open " << url << ": " << SDL_GetError() << LL_ENDL;
    }
}

// ---- sharing a map: one file, exported here and imported by another user ----

namespace
{
std::string share_name(const LLViewerRegion* region)
{
    // [RLVa] no region name in the proposed file name while the location is hidden
    return LLDir::getScrubbedFileName(RlvActions::canShowLocation() ? region->getName() : std::string("region map"));
}

// Written next to it, then moved over the old file in one step.
bool write_replace(const std::string& path, const char* data, size_t size)
{
    const std::string tmp = path + ".tmp";
    {
        llofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(data, size);
        f.close();
        if (f.fail()) { LLFile::remove(tmp, ENOENT); return false; }
    }
    if (LLFile::replace(tmp, path) != 0) { LLFile::remove(tmp, ENOENT); return false; }
    return true;
}

// A shared map file, read and decoded on a worker thread.
struct SharedMap
{
    std::string error;          // "" = readable
    LLSD meta;
    std::string payload;        // the image file's bytes
    LLPointer<LLImageRaw> raw;  // decoded (the check that it is an image, and the mini-map's texture)
};

SharedMap read_shared_map(const std::string& path)
{
    SharedMap m;
    llstat st;
    const S64 size = LLFile::stat(path, &st) == 0 ? (S64)st.st_size : 0;
    if (size <= 0 || size > kShareMaxBytes) { m.error = "it is not a region map file"; return m; }
    std::string bytes;
    if (!GMDXRegionCaptureState::readFile(path, bytes)) { m.error = "it could not be read"; return m; }
    const size_t eol = bytes.size() > kShareMagic.size() ? bytes.find('\n', kShareMagic.size()) : std::string::npos;
    if (bytes.compare(0, kShareMagic.size(), kShareMagic) != 0 || eol == std::string::npos)
    {
        m.error = "it is not a region map file";
        return m;
    }
    try { m.meta = LlsdFromJson(boost::json::parse(bytes.substr(kShareMagic.size(), eol - kShareMagic.size()))); }
    catch (...) { m.error = "its description is damaged"; return m; }
    const std::string format = m.meta["payload"].asString();
    if (!m.meta.isMap() || (format != "j2c" && format != "png") || m.meta["region_grid_x"].asInteger() <= 0
        || m.meta["region_grid_y"].asInteger() <= 0 || (size_t)m.meta["payload_size"].asInteger() != bytes.size() - eol - 1)
    {
        m.error = "its description is damaged";
        return m;
    }
    m.payload = bytes.substr(eol + 1);
    m.raw = GMDXRegionCaptureState::decodeMap(m.payload, format == "png");
    if (m.raw.isNull()) m.error = "its image is damaged";
    return m;
}

void install_shared_map(const SharedMap& m, U64 handle)
{
    const std::string format = m.meta["payload"].asString();
    const std::string region = m.meta["region_name"].asString();
    const std::string dir = GMDXRegionCaptureState::mapDir();
    LLFile::mkdir(gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "region_maps");
    LLFile::mkdir(dir);
    const std::string path = GMDXRegionCaptureState::mapPath(handle, format.c_str());
    if (!write_replace(path, m.payload.data(), m.payload.size()))
    {
        GMDXRegionCaptureState::message("Could not save the imported map to " + path);
        return;
    }
    LLFile::remove(GMDXRegionCaptureState::mapPath(handle, format == "j2c" ? "png" : "j2c"), ENOENT);
    LLSD meta = m.meta;
    meta.erase("payload");
    meta.erase("payload_size");
    meta["imported_utc"] = LLDate::now().asString();
    const std::string json = boost::json::serialize(LlsdToJson(meta)) + "\n";
    write_replace(GMDXRegionCaptureState::mapPath(handle, "json"), json.data(), json.size());
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (gSavedSettings.getBOOL("GMDXMinimapUseCapturedMaps"))
        s.maps[handle] = MapTex{ GMDXRegionCaptureState::minimapTexture(m.raw, (F32)meta["region_width"].asReal()) };
    else
        s.maps.erase(handle);
    LL_INFOS("GMDXRegionMap") << "imported the map of " << region << ": " << path << LL_ENDL;
    LLSD args;
    args["REGION"] = region;
    LLNotificationsUtil::add("GMDXRegionMapImported", args);
}

void import_checked(const std::string& file, const SharedMap& m)
{
    auto fail = [&file](const std::string& why)
    {
        LL_WARNS("GMDXRegionMap") << "not imported: " << file << ": " << why << LL_ENDL;
        GMDXRegionCaptureState::message("The region map was not imported: " + why + ".");
    };
    if (!m.error.empty()) { fail(m.error); return; }
    const std::string grid = LLGridManager::getInstance()->getGridId();
    if (m.meta["grid"].asString() != grid)
    {
        fail("it is a map of a region on another grid (" + m.meta["grid"].asString() + ")");
        return;
    }
    const U64 handle = to_region_handle((U32)m.meta["region_grid_x"].asInteger() * 256u,
                                        (U32)m.meta["region_grid_y"].asInteger() * 256u);
    // A region this viewer knows at those coordinates must be the one that was mapped.
    if (const LLViewerRegion* known = LLWorld::getInstance()->getRegionFromHandle(handle))
    {
        if (m.meta["region_id"].asUUID() != known->getRegionID())
        {
            fail("it is a map of " + m.meta["region_name"].asString() + ", which is no longer at the coordinates of "
                 + known->getName());
            return;
        }
    }
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (GMDXRegionCapture::isRunning() && s.handle == handle) { fail("that region is being mapped right now"); return; }
    if (GMDXRegionCaptureState::mapFile(handle).empty()) { install_shared_map(m, handle); return; }
    LLSD args;
    args["REGION"] = m.meta["region_name"].asString();
    LLNotificationsUtil::add("GMDXRegionMapReplace", args, LLSD(),
        [m, handle](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) install_shared_map(m, handle);
        });
}
} // namespace

void GMDXRegionCapture::exportMap(const LLViewerRegion* region)
{
    const std::string path = region ? GMDXRegionCaptureState::mapFile(region->getHandle()) : std::string();
    std::string payload;
    if (path.empty() || !GMDXRegionCaptureState::readFile(path, payload) || payload.empty())
    {
        GMDXRegionCaptureState::message("This region has no captured map to export.");
        return;
    }
    LLSD meta;
    {
        std::string json;
        if (GMDXRegionCaptureState::readFile(GMDXRegionCaptureState::mapPath(region->getHandle(), "json"), json))
        {
            try { meta = LlsdFromJson(boost::json::parse(json)); }
            catch (...) { meta = LLSD(); }
        }
        if (!meta.isMap()) meta = LLSD::emptyMap();
    }
    U32 x = 0, y = 0;
    from_region_handle(region->getHandle(), &x, &y);
    meta["grid"] = LLGridManager::getInstance()->getGridId();
    meta["region_id"] = region->getRegionID();
    meta["region_name"] = region->getName();
    meta["region_width"] = region->getWidth();
    meta["region_grid_x"] = (S32)(x / 256u);
    meta["region_grid_y"] = (S32)(y / 256u);
    meta["payload"] = path.compare(path.size() - 4, 4, ".png") == 0 ? "png" : "j2c";
    meta["payload_size"] = (S32)payload.size();
    const std::string data = kShareMagic + boost::json::serialize(LlsdToJson(meta)) + "\n" + payload;
    LLFilePickerReplyThread::startPicker(
        [data](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested() || filenames.empty()) return;
            if (!write_replace(filenames[0], data.data(), data.size()))
                GMDXRegionCaptureState::message("Could not save the map file to " + filenames[0]);
        },
        LLFilePicker::FFSAVE_ALL, share_name(region) + ".gmdxmap");
}

void GMDXRegionCapture::importMap()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested() || filenames.empty()) return;
            const std::string file = filenames[0];
            LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
            LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
            const bool posted = main_queue && general && main_queue->postTo(general,
                [file]() -> SharedMap
                {
                    try { return read_shared_map(file); }
                    catch (...) { SharedMap m; m.error = "it could not be read"; return m; }
                },
                [file](SharedMap m)
                {
                    if (!LLApp::isExiting()) import_checked(file, m);
                });
            if (!posted) import_checked(file, read_shared_map(file));
        },
        LLFilePicker::FFLOAD_ALL, false);
}

// ---- sharing a map in world: an uploaded texture, used from inventory ----

void GMDXRegionCapture::uploadMap(const LLViewerRegion* region)
{
    if (!region || !hasCapture(region))
    {
        GMDXRegionCaptureState::message("This region has no captured map to upload.");
        return;
    }
    U32 x = 0, y = 0;
    from_region_handle(region->getHandle(), &x, &y);
    const std::string name = kTextureNamePrefix + region->getName();
    const std::string desc = llformat("%s%u %u %.0f %s", kTextureTag.c_str(), x / 256u, y / 256u, region->getWidth(),
                                      region->getRegionID().asString().c_str());
    const std::string region_name = region->getName();
    loadCapturedImage(region, [name, desc, region_name](LLPointer<LLImageRaw> raw)
        {
            if (raw.isNull()) { GMDXRegionCaptureState::message("The map of " + region_name + " could not be read."); return; }
            // the grid's largest upload (2048 px on Second Life)
            const S32 max_px = llclamp(llmin(gSavedSettings.getS32("max_texture_dimension_X"),
                                             gSavedSettings.getS32("max_texture_dimension_Y")), kShareMinPx, kMapSize);
            raw->biasedScaleToPowerOfTwo(max_px);
            const S32 cost = LLAgentBenefitsMgr::current().getTextureUploadCost(raw->getWidth(), raw->getHeight());
            LLSD args;
            args["REGION"] = region_name;
            args["SIZE"] = llformat("%d", raw->getWidth());
            args["COST"] = llformat("%d", cost);
            LLNotificationsUtil::add("GMDXRegionMapUpload", args, LLSD(),
                [raw, name, desc, cost](const LLSD& n, const LLSD& r)
                {
                    if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                    LLPointer<LLImageJ2C> j2c = new LLImageJ2C();
                    if (!j2c->encode(raw, 0.f)) { GMDXRegionCaptureState::message("The map could not be encoded."); return; }
                    // (as LLSnapshotLivePreview::saveTexture)
                    LLTransactionID tid;
                    tid.generate();
                    const LLAssetID asset_id = tid.makeAssetID(gAgent.getSecureSessionID());
                    LLFileSystem file(asset_id, LLAssetType::AT_TEXTURE, LLFileSystem::WRITE);
                    file.write(j2c->getData(), j2c->getDataSize());
                    LLResourceUploadInfo::ptr_t info(new LLResourceUploadInfo(
                        tid, LLAssetType::AT_TEXTURE, name, desc, 0, LLFolderType::FT_TEXTURE, LLInventoryType::IT_TEXTURE,
                        PERM_ALL, LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
                        cost, LLUUID::null, true));
                    upload_new_resource(info);
                });
        });
}

bool GMDXRegionCapture::isMapTexture(const std::string& description)
{
    return description.compare(0, kTextureTag.size(), kTextureTag) == 0;
}

namespace
{
// The map texture's loaded callback (gmdxminimapimage.cpp on_tile_loaded): the final call, or a
// failure, ends it.
struct TextureMapRequest { std::string name, desc; };

void on_map_texture_loaded(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw*,
                           S32, bool final, void* userdata)
{
    if (success && !final) return;
    std::unique_ptr<TextureMapRequest> req(static_cast<TextureMapRequest*>(userdata));
    if (!success || !src || src->isBufferInvalid() || src->getComponents() < 3)
    {
        GMDXRegionCaptureState::message("The map texture " + req->name + " could not be loaded.");
        return;
    }
    SharedMap m;
    {
        LLImageDataSharedLock lock(src);
        m.raw = new LLImageRaw(static_cast<const U8*>(src->getData()), src->getWidth(), src->getHeight(), src->getComponents());
    }
    if (m.raw->getComponents() > 3)
    {
        LLPointer<LLImageRaw> rgb = new LLImageRaw(m.raw->getWidth(), m.raw->getHeight(), 3);
        rgb->copy(m.raw);
        m.raw = rgb;
    }
    if (m.raw->getWidth() != m.raw->getHeight() || m.raw->getWidth() < kShareMinPx)
    {
        GMDXRegionCaptureState::message("The texture " + req->name + " is not a region map.");
        return;
    }
    // "GMDXMAP1 <grid x> <grid y> <region width> <region id>"
    std::istringstream in(req->desc.substr(kTextureTag.size()));
    S32 gx = 0, gy = 0;
    F32 width = 0.f;
    std::string id;
    in >> gx >> gy >> width >> id;
    if (in.fail() || gx <= 0 || gy <= 0 || width < 16.f || !LLUUID::validate(id))
    {
        GMDXRegionCaptureState::message("The texture " + req->name + " is not a region map (its description was changed).");
        return;
    }
    std::string region = req->name;
    if (region.compare(0, kTextureNamePrefix.size(), kTextureNamePrefix) == 0) region = region.substr(kTextureNamePrefix.size());
    m.meta["grid"] = LLGridManager::getInstance()->getGridId();   // (a texture is on this grid)
    m.meta["region_name"] = region;
    m.meta["region_id"] = LLUUID(id);
    m.meta["region_width"] = width;
    m.meta["region_grid_x"] = gx;
    m.meta["region_grid_y"] = gy;
    m.meta["payload"] = "j2c";
    m.meta["from_texture"] = src_vi ? src_vi->getID() : LLUUID::null;
    // saved as a map file: JPEG 2000 at the capture's quality, on a worker thread
    const std::string label = req->name;
    auto encode = [m]() -> SharedMap
    {
        SharedMap out = m;
        LLPointer<LLImageJ2C> j2c = new LLImageJ2C();
        j2c->setRate(1.f / 3.5f);
        if (!j2c->encode(out.raw, 0.f)) { out.error = "it could not be encoded"; return out; }
        out.payload.assign((const char*)j2c->getData(), (size_t)j2c->getDataSize());
        return out;
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    const bool posted = main_queue && general && main_queue->postTo(general, encode,
        [label](SharedMap done)
        {
            if (!LLApp::isExiting()) import_checked(label, done);
        });
    if (!posted) import_checked(label, encode());
}
} // namespace

void GMDXRegionCapture::useMapTexture(const LLUUID& asset_id, const std::string& name, const std::string& description)
{
    if (asset_id.isNull() || !isMapTexture(description)) return;
    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(asset_id, FTT_DEFAULT, true,
                                                                            LLGLTexture::BOOST_PREVIEW);
    if (!tex) return;
    tex->forceToSaveRawImage(0);   // the full resolution
    tex->setLoadedCallback(on_map_texture_loaded, 0, true, false, new TextureMapRequest{ name, description }, nullptr);
}

bool GMDXRegionCapture::getCapturedImage(const LLViewerRegion* region, LLPointer<LLImageRaw>& out)
{
    out = region ? GMDXRegionCaptureState::loadPng(GMDXRegionCaptureState::mapFile(region->getHandle())) : nullptr;
    return out.notNull();
}

bool GMDXRegionCapture::loadCapturedImage(const LLViewerRegion* region, std::function<void(LLPointer<LLImageRaw>)> done)
{
    if (!region || !done || !hasCapture(region)) return false;
    const std::string path = GMDXRegionCaptureState::mapFile(region->getHandle());
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    const bool posted = main_queue && general && main_queue->postTo(general,
        [path]() -> LLPointer<LLImageRaw>
        {
            try { return GMDXRegionCaptureState::loadPng(path); }
            catch (...) { return nullptr; }
        },
        [done](LLPointer<LLImageRaw> raw)
        {
            if (!LLApp::isExiting()) done(raw);
        });
    if (!posted) done(GMDXRegionCaptureState::loadPng(path));   // (no worker: here)
    return true;
}

bool GMDXRegionCapture::isRunning()
{
    const Stage st = GMDXRegionCaptureState::get().stage;
    return st == Stage::Prepare || st == Stage::Render;
}

void GMDXRegionCapture::cancelCapture()
{
    if (isRunning()) GMDXRegionCaptureState::get().stop("cancelled");
}

void GMDXRegionCapture::requestStop(const std::string& why)
{
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    if (isRunning() && s.stop_request.empty()) s.stop_request = why;
}

LLViewerTexture* GMDXRegionCapture::getMinimapTexture(const LLViewerRegion* region)
{
    static LLCachedControl<bool> use_captured(gSavedSettings, "GMDXMinimapUseCapturedMaps", true);
    if (!region || !use_captured) return nullptr;
    GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    auto it = s.maps.find(region->getHandle());
    if (it == s.maps.end())
    {
        it = s.maps.emplace(region->getHandle(), MapTex()).first;
        s.loadMinimap(region, it->second);
    }
    return it->second.tex.get();
}

bool GMDXRegionCapture::cacheSphere(const LLViewerRegion* region, LLVector3& center_agent, F32& radius)
{
    const GMDXRegionCaptureState& s = GMDXRegionCaptureState::get();
    // (while the tiles render too: objects nobody selects for a few frames go back to the cache)
    // Only the region being mapped: the sphere around its box is asked of its own cache grid,
    // so a tall band (a skybox) does not reach into the neighbours.
    if ((s.stage != Stage::Prepare && s.stage != Stage::Render) || !sLodOn || !region || region->getHandle() != s.handle)
        return false;
    const F32 w = sLodBox[2] - sLodBox[0];
    const F32 h = s.ceiling - s.floor;
    center_agent.set(sLodBox[0] + 0.5f * w, sLodBox[1] + 0.5f * w, s.floor + 0.5f * h);
    radius = 0.5f * sqrtf(2.f * w * w + h * h) + 16.f;
    return true;
}
