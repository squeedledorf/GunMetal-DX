/**
 * @file llnetmap.cpp
 * @author James Cook
 * @brief Display of surrounding regions, objects, and agents.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2001-2010, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "llnetmap.h"
#include "llfloatermap.h" // <GMDX> Show Whole Region is the floater's
#include "gmdxminimapterrain.h" // <GMDX>
#include "gmdxminimapcompass.h" // <GMDX>
#include "gmdxminimapimage.h" // <GMDX>
#include "llfloater.h"
#include "llvkhitch.h"

// Library includes (should move below)
#include "indra_constants.h"
#include "llavatarnamecache.h"
#include "llmath.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llresmgr.h"
#include "llui.h"
#include "lltooltip.h"
#include "lltrans.h"

#include "llwin32headers.h"

// Viewer includes
#include "slavataractions.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h" // for gDisconnected
#include "llcallingcard.h" // LLAvatarTracker
#include "llgroupcolormap.h" // <GMDX>
#include "llfloaterland.h"
#include "llfloaterworldmap.h"
#include "llparcel.h"
#include "lltracker.h"
#include "llsurface.h"
#include "llurlmatch.h"
#include "llurlregistry.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "llworldmapview.h"     // shared draw code
// [RLVa:KB] - Checked: RLVa-2.0.1
#include "rlvactions.h"
#include "rlvcommon.h"
// [/RLVa:KB]
#include "lggcontactsets.h"
#include "gmdxregioncapture.h"   // <GMDX> region map capture (menu)
#include "gmdxminimapmarks.h"    // <GMDX> marks
#include "gmdxmapshare.h"        // <GMDX> shared marks, zones, pings
#include "llclipboard.h"         // <GMDX> copy coordinates
#include "llnotificationsutil.h" // <GMDX> mark names
#include "llavataractions.h"     // <GMDX> a dot's profile
#include "llcolorswatch.h"       // <GMDX> a dot's colour

static LLDefaultChildRegistry::Register<LLNetMap> r1("net_map");

constexpr F32 LLNetMap::MAP_SCALE_MIN = 32;
constexpr F32 LLNetMap::MAP_SCALE_FAR = 32;
constexpr F32 LLNetMap::MAP_SCALE_MEDIUM = 128;
constexpr F32 LLNetMap::MAP_SCALE_CLOSE = 256;
constexpr F32 LLNetMap::MAP_SCALE_VERY_CLOSE = 1024;
constexpr F32 LLNetMap::MAP_SCALE_MAX = 4096;

constexpr F32 MAP_SCALE_ZOOM_FACTOR = 1.04f; // Zoom in factor per click of scroll wheel (4%)
constexpr F32 MIN_DOT_RADIUS = 3.5f;
constexpr F32 DOT_SCALE = 0.75f;
constexpr F32 MIN_PICK_SCALE = 2.f;
constexpr F32 MIN_PICK_PX = 8.f;   // <GMDX> the zone around a dot that picks it, at least this many pixels
constexpr S32 MOUSE_DRAG_SLOP = 2;      // How far the mouse needs to move before we think it's a drag

constexpr F64 COARSEUPDATE_MAX_Z = 1020.0f;

// <GMDX> GMDXMiniMapHideAbove: an avatar more than GMDXMiniMapHideAboveMeters above me (the dot's
// position) gets no dot, no hover and no pick. One known only from the coarse locations as "1020 m
// or higher" (exactly 1020 m: a loaded avatar has its real height) is hidden whenever I am below 1020 m.
static bool gmdx_hidden_above(const LLVector3d& pos_global)
{
    static LLCachedControl<bool> hide(gSavedSettings, "GMDXMiniMapHideAbove", false);
    static LLCachedControl<F32> meters(gSavedSettings, "GMDXMiniMapHideAboveMeters", 250.f);
    if (!hide) return false;
    const F64 my_z = gAgent.getPositionGlobal().mdV[VZ];
    if (pos_global.mdV[VZ] == COARSEUPDATE_MAX_Z && my_z < COARSEUPDATE_MAX_Z) return true;
    return pos_global.mdV[VZ] - my_z > llclamp((F32)meters, 250.f, 1000.f);
}
// </GMDX>

LLNetMap::LLNetMap (const Params & p)
:   LLUICtrl (p),
    mUpdateObjectImage(false),
    mUpdateParcelImage(false),
    mBackgroundColor (p.bg_color()),
    mScale( MAP_SCALE_MEDIUM ),
    mPixelsPerMeter( MAP_SCALE_MEDIUM / REGION_WIDTH_METERS ),
    mObjectMapTPM(0.f),
    mObjectMapPixels(0.f),
    mCurPan(0.f, 0.f),
    mStartPan(0.f, 0.f),
    mPopupWorldPos(0.f, 0.f, 0.f),
    mMouseDown(0, 0),
    mPanning(false),
    mCentering(false),
    mObjectImageCenterGlobal( gAgentCamera.getCameraPositionGlobal() ),
    mObjectImageShownCenterGlobal( gAgentCamera.getCameraPositionGlobal() ),
    mObjectRawImagep(),
    mObjectImagep(),
    mParcelImageCenterGlobal( gAgentCamera.getCameraPositionGlobal() ),
    mParcelRawImagep(),
    mParcelImagep(),
    mClosestAgentToCursor(),
    mAvatarsAtLastRightClick(),
    mToolTipMsg()
{
    setScale(gSavedSettings.getF32("MiniMapScale"));
    if (gAgent.isFirstLogin())
    {
        // *HACK: On first run, set this to false for new users, otherwise the
        // default is true to maintain consistent experience for existing
        // users.
        gSavedSettings.setBOOL("MiniMapRotate", false);
    }
}

LLNetMap::~LLNetMap()
{
    // <GMDX> the Dot Colour swatch cancels its open picker when it is destroyed (with the other
    // children, after this), which would call back into this map: detach and close the picker first
    if (mDotColorSwatch)
    {
        mDotColorSwatch->closeFloaterColorPicker();
        mDotColorSwatch = nullptr;
    }
    // </GMDX>
    auto menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (menu)
    {
        menu->die();
        mPopupMenuHandle.markDead();
    }
    if (mParcelMgrConn.connected())
    {
        mParcelMgrConn.disconnect();
    }
    if (mParcelOverlayConn.connected())
    {
        mParcelOverlayConn.disconnect();
    }
}

bool LLNetMap::postBuild()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar commitRegistrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enableRegistrar;

    enableRegistrar.add("Minimap.Zoom.Check", boost::bind(&LLNetMap::isZoomChecked, this, _2));
    commitRegistrar.add("Minimap.Zoom.Set", boost::bind(&LLNetMap::setZoom, this, _2));
    commitRegistrar.add("Minimap.Tracker", boost::bind(&LLNetMap::handleStopTracking, this, _2));
    commitRegistrar.add("Minimap.Center.Activate", boost::bind(&LLNetMap::activateCenterMap, this, _2));
    enableRegistrar.add("Minimap.MapOrientation.Check", boost::bind(&LLNetMap::isMapOrientationChecked, this, _2));
    commitRegistrar.add("Minimap.MapOrientation.Set", boost::bind(&LLNetMap::setMapOrientation, this, _2));
    commitRegistrar.add("Minimap.AboutLand", boost::bind(&LLNetMap::popupShowAboutLand, this, _2));
    commitRegistrar.add("Minimap.CamToAvatar", boost::bind(&LLNetMap::handleCamToAvatar, this, _2));
    // <GMDX> a dot's profile and colour
    commitRegistrar.add("Minimap.Avatar.Profile", [this](LLUICtrl*, const LLSD&)
        {
            if (!mAvatarsAtLastRightClick.empty()) LLAvatarActions::showProfile(mAvatarsAtLastRightClick.front());
        });
    commitRegistrar.add("Minimap.Avatar.Color", [this](LLUICtrl*, const LLSD& param) { setAvatarDotColor(param.asString()); });
    enableRegistrar.add("Minimap.Avatar.HasColor", [this](LLUICtrl*, const LLSD&)
        {
            LLColor4 c;
            return !mAvatarsAtLastRightClick.empty() && GMDXMinimapMarks::avatarColor(mAvatarsAtLastRightClick.front(), c);
        });
    // </GMDX>
    commitRegistrar.add("Minimap.RegionMap.Capture", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::openWindow(); });  // <GMDX>
    enableRegistrar.add("Minimap.RegionMap.CanCapture", [](LLUICtrl*, const LLSD&) { return GMDXRegionCapture::canCapture(); });  // <GMDX>
    commitRegistrar.add("Minimap.RegionMap.Cancel", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::cancelCapture(); });  // <GMDX>
    enableRegistrar.add("Minimap.RegionMap.Running", [](LLUICtrl*, const LLSD&) { return GMDXRegionCapture::isRunning(); });  // <GMDX>
    // <GMDX> Show Whole Region compass style; Copy / Save Map Image (gmdxminimapimage.h)
    // (the rotating map keeps its own: GMDXMiniMapRotateCompass, pills or frame)
    enableRegistrar.add("Minimap.CompassStyle.Check", [this](LLUICtrl*, const LLSD& param)
        { return (rotateMap() ? GMDXMinimapCompass::getRotatingStyle() : GMDXMinimapCompass::getStyle()) == param.asInteger(); });
    commitRegistrar.add("Minimap.CompassStyle.Set", [this](LLUICtrl*, const LLSD& param)
        { gSavedSettings.setS32(rotateMap() ? "GMDXMiniMapRotateCompass" : "GMDXMiniMapCompassStyle", param.asInteger()); });
    commitRegistrar.add("Minimap.CopyImage", [](LLUICtrl*, const LLSD&) { gmdx_copy_region_map_image(); });
    commitRegistrar.add("Minimap.SaveImage", [](LLUICtrl*, const LLSD&) { gmdx_save_region_map_image(); });
    // View: Follow Me / Whole Region (GMDXMiniMapWholeRegion)
    enableRegistrar.add("Minimap.ViewMode.Check", [this](LLUICtrl*, const LLSD& param)
        {
            // (the People floater's map always follows)
            const bool whole = gSavedSettings.getBOOL("GMDXMiniMapWholeRegion") && getParentByType<LLFloaterMap>();
            return whole == (param.asString() == "region");
        });
    commitRegistrar.add("Minimap.ViewMode.Set", [](LLUICtrl*, const LLSD& param)
        { gSavedSettings.setBOOL("GMDXMiniMapWholeRegion", param.asString() == "region"); });
    enableRegistrar.add("Minimap.CanRecenter", [this](LLUICtrl*, const LLSD&) { return mCanRecenter; });
    enableRegistrar.add("Minimap.IsTracking", [](LLUICtrl*, const LLSD&) { return LLTracker::isTracking(0); });
    // Background: painted terrain / Region Terrain Map (GMDXMiniMapOCOLLook) / world map tile (GMDXMiniMapUseMapTile)
    enableRegistrar.add("Minimap.Background.Check", [](LLUICtrl*, const LLSD& param)
        {
            const std::string which = param.asString();
            const bool tile = gSavedSettings.getBOOL("GMDXMiniMapUseMapTile");
            const bool terrain = gSavedSettings.getBOOL("GMDXMiniMapOCOLLook");
            return which == "tile" ? tile : which == "terrain" ? (!tile && terrain) : (!tile && !terrain);
        });
    commitRegistrar.add("Minimap.Background.Set", [](LLUICtrl*, const LLSD& param)
        {
            const std::string which = param.asString();
            gSavedSettings.setBOOL("GMDXMiniMapUseMapTile", which == "tile");
            if (which != "tile")
            {
                gSavedSettings.setBOOL("GMDXMiniMapOCOLLook", which == "terrain");
            }
        });
    // Opacity of the map (GMDXMiniMapOpacity)
    enableRegistrar.add("Minimap.Opacity.Check", [](LLUICtrl*, const LLSD& param)
        { return fabsf(gSavedSettings.getF32("GMDXMiniMapOpacity") - (F32)param.asReal()) < 0.01f; });
    commitRegistrar.add("Minimap.Opacity.Set", [](LLUICtrl*, const LLSD& param)
        { gSavedSettings.setF32("GMDXMiniMapOpacity", (F32)param.asReal()); });
    // Region Map: delete this region's capture / open the folder
    enableRegistrar.add("Minimap.RegionMap.HasCapture", [](LLUICtrl*, const LLSD&)
        { return GMDXRegionCapture::hasCapture(gAgent.getRegion()) && !GMDXRegionCapture::isRunning(); });
    commitRegistrar.add("Minimap.RegionMap.Delete", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::deleteCapture(gAgent.getRegion()); });
    commitRegistrar.add("Minimap.RegionMap.OpenFolder", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::openMapsFolder(); });
    commitRegistrar.add("Minimap.RegionMap.Export", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::exportMap(gAgent.getRegion()); });
    commitRegistrar.add("Minimap.RegionMap.Import", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::importMap(); });
    commitRegistrar.add("Minimap.RegionMap.Upload", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::uploadMap(gAgent.getRegion()); });
    // Where the menu was opened: copy it, mark it; the mark under it: track, rename, recolour, remove
    commitRegistrar.add("Minimap.CopyPosition", [this](LLUICtrl*, const LLSD& param) { copyPopupPosition(param.asString() == "slurl"); });
    enableRegistrar.add("Minimap.CanShowLocation", [](LLUICtrl*, const LLSD&) { return RlvActions::canShowLocation(); });
    commitRegistrar.add("Minimap.Mark.Add", [this](LLUICtrl*, const LLSD& param) { addMarkAtPopup(param.asInteger()); });
    commitRegistrar.add("Minimap.Mark.Track", [this](LLUICtrl*, const LLSD&)
        {
            // ([RLVa] the tooltip is the region's name: none while the location is hidden)
            if (const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark))
                LLTracker::trackLocation(m->pos_global, m->name, RlvActions::canShowLocation() ? m->region : std::string());
        });
    commitRegistrar.add("Minimap.Mark.Rename", [this](LLUICtrl*, const LLSD&)
        {
            const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark);
            if (!m) return;
            LLSD args;
            args["NAME"] = m->name;
            const LLUUID id = m->id;
            LLNotificationsUtil::add("GMDXMinimapMarkRename", args, LLSD(), [id](const LLSD& n, const LLSD& r)
                {
                    if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                    std::string name = r["name"].asString();
                    LLStringUtil::trim(name);
                    if (!name.empty()) GMDXMinimapMarks::rename(id, name);
                });
        });
    commitRegistrar.add("Minimap.Mark.Color", [this](LLUICtrl*, const LLSD& param) { GMDXMinimapMarks::recolor(mPopupMark, param.asInteger()); });
    commitRegistrar.add("Minimap.Mark.Remove", [this](LLUICtrl*, const LLSD&) { GMDXMinimapMarks::remove(mPopupMark); });
    // Zones: draw one; the zone under the menu: line, colour, rename, remove
    commitRegistrar.add("Minimap.Zone.Draw", [this](LLUICtrl*, const LLSD& param) { startZone(param.asInteger()); });
    commitRegistrar.add("Minimap.Zone.Line", [this](LLUICtrl*, const LLSD& param)
        {
            const bool dotted = param.asString() == "dotted";
            GMDXMinimapMarks::setZoneDotted(mPopupZone, dotted);
            gSavedSettings.setBOOL("GMDXMiniMapZoneDotted", dotted);   // (the next zone's too)
        });
    commitRegistrar.add("Minimap.Zone.Color", [this](LLUICtrl*, const LLSD& param)
        {
            GMDXMinimapMarks::recolorZone(mPopupZone, param.asInteger());
            gSavedSettings.setS32("GMDXMiniMapZoneColor", param.asInteger());
        });
    commitRegistrar.add("Minimap.Zone.Fill", [this](LLUICtrl*, const LLSD& param)
        {
            GMDXMinimapMarks::setZoneFill(mPopupZone, param.asInteger());
            gSavedSettings.setS32("GMDXMiniMapZoneFill", param.asInteger());   // (the next zone's too)
        });
    commitRegistrar.add("Minimap.Zone.Rename", [this](LLUICtrl*, const LLSD&)
        {
            const GMDXMapZone* z = GMDXMinimapMarks::findZone(mPopupZone);
            if (!z) return;
            LLSD args;
            args["NAME"] = z->name;
            const LLUUID id = z->id;
            LLNotificationsUtil::add("GMDXMinimapZoneName", args, LLSD(), [id](const LLSD& n, const LLSD& r)
                {
                    if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                    std::string name = r["name"].asString();
                    LLStringUtil::trim(name);
                    GMDXMinimapMarks::renameZone(id, name);
                });
        });
    commitRegistrar.add("Minimap.Zone.Remove", [this](LLUICtrl*, const LLSD&) { GMDXMinimapMarks::removeZone(mPopupZone); });
    // Sharing (gmdxmapshare.h): my mark / zone to my group ("group") or the Send To targets
    // ("list") in this region (Send To is filled in on right-click), a ping here, a shared item kept
    // or removed
    commitRegistrar.add("Minimap.Share", [this](LLUICtrl*, const LLSD& param)
        {
            const bool group_only = param.asString() == "group";
            if (mPopupMark.notNull()) GMDXMapShare::shareMark(mPopupMark, group_only);
            else if (mPopupZone.notNull()) GMDXMapShare::shareZone(mPopupZone, group_only);
        });
    commitRegistrar.add("Minimap.Ping", [this](LLUICtrl*, const LLSD& param)
        { GMDXMapShare::ping(GMDXMinimapMarks::onGround(mPopupWorldPos), param.asInteger()); });
    commitRegistrar.add("Minimap.Shared.Keep", [this](LLUICtrl*, const LLSD&) { GMDXMapShare::keep(mPopupShared); });
    commitRegistrar.add("Minimap.Shared.Remove", [this](LLUICtrl*, const LLSD&) { GMDXMapShare::remove(mPopupShared); });
    commitRegistrar.add("Minimap.Shared.RemoveSender", [this](LLUICtrl*, const LLSD&)
        {
            if (const GMDXSharedItem* item = GMDXMapShare::find(mPopupShared)) GMDXMapShare::removeFrom(item->sender);
        });
    commitRegistrar.add("Minimap.Shared.BlockSender", [this](LLUICtrl*, const LLSD&)
        {
            if (const GMDXSharedItem* item = GMDXMapShare::find(mPopupShared)) GMDXMapShare::block(item->sender);
        });
    commitRegistrar.add("Minimap.Shared.Clear", [](LLUICtrl*, const LLSD&) { GMDXMapShare::clear(); });
    commitRegistrar.add("Minimap.Shared.UnblockAll", [](LLUICtrl*, const LLSD&) { GMDXMapShare::unblockAll(); });
    // </GMDX>

    LLMenuGL* menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>("menu_mini_map.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    mPopupMenuHandle = menu->getHandle();

    mParcelMgrConn = LLViewerParcelMgr::instance().setCollisionUpdateCallback(boost::bind(&LLNetMap::refreshParcelOverlay, this));
    mParcelOverlayConn = LLViewerParcelOverlay::setUpdateCallback(boost::bind(&LLNetMap::refreshParcelOverlay, this));
    return true;
}

void LLNetMap::setScale( F32 scale )
{
    scale = llclamp(scale, MAP_SCALE_MIN, MAP_SCALE_MAX);
    mCurPan *= scale / mScale;
    mScale = scale;

    updateScaleDerived(); // <GMDX>

    gSavedSettings.setF32("MiniMapScale", mScale);

    mUpdateObjectImage = true;
    mUpdateParcelImage = true;
}

// <GMDX> The values that follow the scale, from mScale or, in Show Whole Region, from the
// fitted scale (which never touches mScale / MiniMapScale).
void LLNetMap::updateScaleDerived()
{
    if (mObjectImagep.notNull())
    {
        F32 width = (F32)(getRect().getWidth());
        F32 height = (F32)(getRect().getHeight());
        F32 diameter = sqrt(width * width + height * height);
        F32 meters = mFitRegion ? diameter / mFitPixelsPerMeter
                                : diameter / mScale * LLWorld::getInstance()->getRegionWidthInMeters();
        F32 num_pixels = (F32)mObjectImagep->getWidth();
        mObjectMapTPM = num_pixels / meters;
        mObjectMapPixels = diameter;
    }

    mPixelsPerMeter = mFitRegion ? mFitPixelsPerMeter : mScale / REGION_WIDTH_METERS;
    mDotRadius = llmax(DOT_SCALE * mPixelsPerMeter, MIN_DOT_RADIUS);
}

LLVector3d LLNetMap::mapCenterGlobal() const
{
    return mFitRegion ? mFitCenterGlobal : gAgentCamera.getCameraPositionGlobal();
}

bool LLNetMap::rotateMap() const
{
    // Camera at top: in Show Whole Region too, where the map turns as GMDXMiniMapRotateView says
    // (round / square radar, or the whole region: mRadarRadius / mRadarSquare).
    static LLUICachedControl<bool> rotate_map("MiniMapRotate", true);
    return rotate_map;
}

LLVector2 LLNetMap::radarCenter() const
{
    return LLVector2((F32)(getRect().getWidth() / 2) + mFitPan.mV[VX], (F32)(getRect().getHeight() / 2) + mFitPan.mV[VY]);
}

LLVector2 LLNetMap::toRadarRim(const LLVector2& p) const
{
    const LLVector2 c = radarCenter();
    const LLVector2 d = p - c;
    if (mRadarRadius <= 0.f)   // the square radar, or the north-up Whole Region map
    {
        const F32 hx = llmax(mFitMapRect.getWidth() * 0.5f - 3.f, 1.f), hy = llmax(mFitMapRect.getHeight() * 0.5f - 3.f, 1.f);
        const F32 k = llmax(fabsf(d.mV[VX]) / hx, fabsf(d.mV[VY]) / hy);
        return k > 1.f ? c + d / k : p;
    }
    const F32 len = d.length();
    const F32 rim = llmax(mRadarRadius - 3.f, 0.f);
    return len > rim && len > 0.f ? c + d * (rim / len) : p;
}

namespace
{
// The round radar's disc (view coordinates) for the zone drawing; radius 0 = no radar.
LLVector2 sDiscCenter;
F32 sDiscRadius = 0.f;
void clip_disc(std::vector<LLVector2>& poly, const LLVector2& c, F32 r);
// A textured axis-aligned quad cut to the disc of radius r around the origin, inside gGL.begin(TRIANGLES).
void disc_quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 r)
{
    std::vector<LLVector2> poly = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    clip_disc(poly, LLVector2(0.f, 0.f), r);
    auto emit = [&](const LLVector2& p)
    {
        gGL.texCoord2f(u0 + (u1 - u0) * (p.mV[VX] - x0) / (x1 - x0), v0 + (v1 - v0) * (p.mV[VY] - y0) / (y1 - y0));
        gGL.vertex2f(p.mV[VX], p.mV[VY]);
    };
    for (size_t k = 1; k + 1 < poly.size(); ++k)
    {
        emit(poly[0]); emit(poly[k]); emit(poly[k + 1]);
    }
}
} // namespace
// </GMDX>


///////////////////////////////////////////////////////////////////////////////////

void LLNetMap::draw()
{
    LLRetainedLiveScope live(this);   // drawn fresh every frame inside a replayed floater
    if (!LLWorld::instanceExists())
    {
        return;
    }
    LLViewerRegion* current_region = gAgent.getRegion();
    if (!current_region)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED;
    VK_HITCH_SCOPE("ui_netmap");
    static LLFrameTimer map_timer;
    static LLUIColor map_avatar_color = LLUIColorTable::instance().getColor("MapAvatarColor", LLColor4::white);
    //static LLUIColor map_avatar_friend_color = LLUIColorTable::instance().getColor("MapAvatarFriendColor", LLColor4::white);
    static LLUIColor map_track_color = LLUIColorTable::instance().getColor("MapTrackColor", LLColor4::white);
    static LLUIColor map_whisper_ring_color = LLUIColorTable::instance().getColor("MapWhisperRingColor", LLColor4::white);
    static LLUIColor map_chat_ring_color = LLUIColorTable::instance().getColor("MapChatRingColor", LLColor4::white);
    static LLUIColor map_shout_ring_color = LLUIColorTable::instance().getColor("MapShoutRingColor", LLColor4::white);
    //static LLUIColor map_track_disabled_color = LLUIColorTable::instance().getColor("MapTrackDisabledColor", LLColor4::white);
    static LLUIColor map_frustum_color = LLUIColorTable::instance().getColor("MapFrustumColor", LLColor4::white);
    static LLUIColor map_frustum_rotating_color = LLUIColorTable::instance().getColor("MapFrustumRotatingColor", LLColor4::white);
    static LLUIColor map_line_color = LLUIColorTable::instance().getColor("MapLineColor", LLColor4::red);
    static LLUIColor map_parcel_outline_color = LLUIColorTable::instance().getColor("MapParcelOutlineColor", LLColor4::white);

    static LLCachedControl<bool> enable_object_render(gSavedSettings, "SlipstreamMinimapRenderObjects", true);
    static LLCachedControl<bool> render_guide_line(gSavedSettings, "SlipstreamMinimapGuideLine", false);
    static LLCachedControl<bool> map_chat_ring(gSavedSettings, "SlipstreamMinimapChatRings", false);
    static LLCachedControl<bool> minimap_parcel_boundries(gSavedSettings, "MiniMapShowPropertyLines", false);

    if (mObjectImagep.isNull())
    {
        createObjectImage();
    }
    if (mParcelImagep.isNull())
    {
        createParcelImage();
    }

    // <GMDX> Show Whole Region: the agent's region fitted to the view's centred square (inside
    // the compass frame, if the compass style has one), north up, no pan / zoom.
    static LLCachedControl<bool> s_whole_region(gSavedSettings, "GMDXMiniMapWholeRegion", false);
    // (only the mini-map floater's map: the one in the People floater's Nearby tab stays as it is)
    const bool whole_region = s_whole_region && getParentByType<LLFloaterMap>();
    static LLCachedControl<bool> physical_only(gSavedSettings, "GMDXMiniMapPhysicalOnly", false);
    LLRectf fit_square;
    F32 fit_ppm = 0.f;
    // the rotating map (GMDXMiniMapRotateView): 0 / 1 = the radar, round / square, turning around
    // the agent; 2 = the whole region turning about its centre, shrunk so it fits at any angle
    static LLCachedControl<S32> rotate_view(gSavedSettings, "GMDXMiniMapRotateView", 0);
    const bool rotating = whole_region && rotateMap();
    const S32 rot_view = rotating ? llclamp((S32)rotate_view, 0, 2) : -1;
    const bool radar = rot_view == 0 || rot_view == 1;   // centred on the agent
    const bool rotate_frame = rotating && GMDXMinimapCompass::getRotatingStyle() == GMDXMinimapCompass::STYLE_FRAME;
    if (whole_region)
    {
        const S32 width = getRect().getWidth();
        // the heading tape's strip sits above the map: the square is fitted below it
        const S32 height = getRect().getHeight() - (S32)GMDXMinimapCompass::getTopReserve();
        // even, so it is centred on the map's centre pixel (width / 2, height / 2) exactly
        const S32 side = llmax(llmin(width, height) & ~1, 2);
        fit_square.setOriginAndSize((F32)(width / 2 - side / 2), (F32)(height / 2 - side / 2), (F32)side, (F32)side);
        mFitPan.set(0.f, (F32)(height / 2 - getRect().getHeight() / 2));   // (below the tape strip, if any)
        const F32 region_width = current_region->getWidth();
        const F32 inset = rotating ? (rotate_frame ? GMDXMinimapCompass::getRadarRing() : 0.f) : GMDXMinimapCompass::getMapInset();
        // (the turning whole region: its diagonal fits)
        mFitBasePixelsPerMeter = llmax((F32)side - 2.f * inset, 1.f) / (region_width * (rot_view == 2 ? F_SQRT2 : 1.f));
        mFitMapRect.setOriginAndSize(fit_square.mLeft + inset, fit_square.mBottom + inset,
                                     fit_square.getWidth() - 2.f * inset, fit_square.getHeight() - 2.f * inset);
        // Zoom (the wheel; wheel out to the whole region): a new region or turning the mode on starts whole
        if (!mFitRegion || current_region->getHandle() != mFitRegionHandle)
        {
            if (!mFitRegion || !radar) mFitZoom = 1.f;   // (the radar keeps its zoom across region crossings)
            mFitRegionHandle = current_region->getHandle();
            mFitViewCenter.set(region_width * 0.5f, region_width * 0.5f);
        }
        if (radar)
        {
            // centred on the agent; past the region's edges the disc stays dark
            const LLVector3 agent = gAgent.getPositionAgent() - current_region->getOriginAgent();
            mFitViewCenter.set(agent.mV[VX], agent.mV[VY]);
        }
        else if (rot_view == 2)
        {
            mFitZoom = 1.f;   // the whole region, always (no zoom or pan)
            mFitViewCenter.set(region_width * 0.5f, region_width * 0.5f);
        }
        else
        {
            clampFitView(region_width);
        }
        fit_ppm = mFitBasePixelsPerMeter * mFitZoom;
        const LLVector3d old_center = mFitCenterGlobal;
        mFitCenterGlobal = current_region->getOriginGlobal()
            + LLVector3d(mFitViewCenter.mV[VX], mFitViewCenter.mV[VY], 0.0);
        mFitCenterGlobal.mdV[VZ] = gAgentCamera.getCameraPositionGlobal().mdV[VZ];
        // (the radar's centre moves with the agent: the layers keep their timed refresh, a forced
        // one every frame would never finish)
        if (mFitRegion && !radar && (old_center.mdV[VX] != mFitCenterGlobal.mdV[VX] || old_center.mdV[VY] != mFitCenterGlobal.mdV[VY]))
        {
            mUpdateObjectImage = true;   // the layers are centred on the view
            mUpdateParcelImage = true;
        }
    }
    if (whole_region != mFitRegion || fit_ppm != mFitPixelsPerMeter)
    {
        mFitRegion = whole_region;
        mFitPixelsPerMeter = fit_ppm;
        updateScaleDerived();
        mUpdateObjectImage = true;
        mUpdateParcelImage = true;
    }
    mRadarSquare = rot_view == 1 || rot_view == 2;
    mRadarRadius = rot_view == 0 ? mFitMapRect.getWidth() * 0.5f : 0.f;
    // </GMDX>

    static LLUICachedControl<bool> auto_center("MiniMapAutoCenter", true);
    bool auto_centering = auto_center && !mPanning;
    mCentering = mCentering && !mPanning;

    if (auto_centering || mCentering)
    {
        mCurPan = lerp(mCurPan, LLVector2(0.0f, 0.0f) , LLSmoothInterpolation::getInterpolant(0.1f));
    }
    bool centered = abs(mCurPan.mV[VX]) < 0.5f && abs(mCurPan.mV[VY]) < 0.5f;
    if (centered)
    {
        mCurPan.mV[0] = 0.0f;
        mCurPan.mV[1] = 0.0f;
        mCentering = false;
    }

    mCanRecenter = !(centered || mCentering || auto_centering || mFitRegion); // <GMDX> no pan in Show Whole Region; the menu's enable callback
    updateAboutLandPopupButton();

    // Prepare a scissor region
    F32 rotation = 0;

    gGL.pushMatrix();
    gGL.pushUIMatrix();

    LLVector3 offset = gGL.getUITranslation();
    LLVector3 scale = gGL.getUIScale();

    gGL.loadIdentity();
    gGL.loadUIIdentity();

    gGL.scalef(scale.mV[0], scale.mV[1], scale.mV[2]);
    gGL.translatef(offset.mV[0], offset.mV[1], offset.mV[2]);

    {
        // <GMDX> the map's opacity (mini-map menu, Opacity)
        static LLCachedControl<F32> s_map_opacity(gSavedSettings, "GMDXMiniMapOpacity", 1.f);
        const F32 map_opacity = llclamp((F32)s_map_opacity, 0.1f, 1.f);
        // </GMDX>
        // <GMDX> Show Whole Region: the map stays inside its square (zoomed in, the region is bigger)
        LLLocalClipRect clip(mFitRegion ? LLRect(ll_round(mFitMapRect.mLeft), ll_round(mFitMapRect.mTop),
                                                 ll_round(mFitMapRect.mRight), ll_round(mFitMapRect.mBottom))
                                        : getLocalRect());
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

            gGL.matrixMode(LLRender::MM_MODELVIEW);

            // Draw background rectangle
            LLColor4 background_color = mBackgroundColor.get();
            background_color.mV[VALPHA] *= map_opacity; // <GMDX> GMDXMiniMapOpacity
            gGL.color4fv( background_color.mV );
            if (mRadarRadius > 0.f)   // <GMDX> the radar's disc
            {
                const LLVector2 c = radarCenter();
                const F32 r = mRadarRadius;
                std::vector<LLVector2> disc = { c + LLVector2(-r, -r), c + LLVector2(r, -r), c + LLVector2(r, r), c + LLVector2(-r, r) };
                clip_disc(disc, c, r);
                gGL.begin(LLRender::TRIANGLES);
                for (size_t k = 1; k + 1 < disc.size(); ++k)
                {
                    gGL.vertex2f(disc[0].mV[VX], disc[0].mV[VY]);
                    gGL.vertex2f(disc[k].mV[VX], disc[k].mV[VY]);
                    gGL.vertex2f(disc[k + 1].mV[VX], disc[k + 1].mV[VY]);
                }
                gGL.end();
            }
            else
            gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0);
        }

        // region 0,0 is in the middle
        // <GMDX> mapPan(): no pan in Show Whole Region
        const LLVector2 pan = mapPan();
        S32 center_sw_left = getRect().getWidth() / 2 + llfloor(pan.mV[VX]);
        S32 center_sw_bottom = getRect().getHeight() / 2 + llfloor(pan.mV[VY]);
        // </GMDX>

        gGL.pushMatrix();

        gGL.translatef( (F32) center_sw_left, (F32) center_sw_bottom, 0.f);

        const bool rotate_map = rotateMap(); // <GMDX> north-up in Show Whole Region
        if( rotate_map )
        {
            // rotate subsequent draws to agent rotation
            rotation = atan2( LLViewerCamera::getInstance()->getAtAxis().mV[VX], LLViewerCamera::getInstance()->getAtAxis().mV[VY] );
            gGL.rotatef( rotation * RAD_TO_DEG, 0.f, 0.f, 1.f);
        }

        // figure out where agent is
        const S32 region_width = ll_round(LLWorld::getInstance()->getRegionWidthInMeters());
        // <GMDX> Show Whole Region: its own scale, centred on the region instead of the camera
        //const F32 scale_pixels_per_meter = mScale / region_width;
        const F32 scale_pixels_per_meter = mFitRegion ? mFitPixelsPerMeter : mScale / region_width;
        const LLVector3 map_center_agent = mFitRegion ? gAgent.getPosAgentFromGlobal(mFitCenterGlobal)
                                                      : gAgentCamera.getCameraPositionAgent();
        // the region's square, in map pixels from the map centre: overlays are cut to it
        // (the region, in map pixels from the map centre: the view's centre may be off the region's middle when zoomed)
        const F32 fit_x0 = mFitRegion ? -mFitViewCenter.mV[VX] * scale_pixels_per_meter : 0.f;
        const F32 fit_y0 = mFitRegion ? -mFitViewCenter.mV[VY] * scale_pixels_per_meter : 0.f;
        const F32 fit_x1 = mFitRegion ? fit_x0 + current_region->getWidth() * scale_pixels_per_meter : 0.f;
        const F32 fit_y1 = mFitRegion ? fit_y0 + current_region->getWidth() * scale_pixels_per_meter : 0.f;
        // </GMDX>

        for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            // <GMDX> only the agent's region in Show Whole Region
            if (mFitRegion && regionp != current_region)
            {
                continue;
            }
            // </GMDX>

            // Find x and y position relative to camera's center.
            LLVector3 origin_agent = regionp->getOriginAgent();
            LLVector3 rel_region_pos = origin_agent - map_center_agent; // <GMDX>
            F32 relative_x = rel_region_pos.mV[0] * scale_pixels_per_meter;
            F32 relative_y = rel_region_pos.mV[1] * scale_pixels_per_meter;

            // background region rectangle
            // <GMDX> the region's own width (var-regions), not mScale's 256 m
            const F32 region_pixels = regionp->getWidth() * scale_pixels_per_meter;
            F32 bottom =    relative_y;
            F32 left =      relative_x;
            F32 top =       bottom + region_pixels;
            F32 right =     left + region_pixels;
            // </GMDX>

            if (regionp == gAgent.getRegion())
            {
                gGL.color4f(1.f, 1.f, 1.f, map_opacity); // <GMDX> map_opacity
            }
            else
            {
                gGL.color4f(0.8f, 0.8f, 0.8f, map_opacity);
            }

            if (!regionp->isAlive())
            {
                gGL.color4f(1.f, 0.5f, 0.5f, map_opacity);
            }

            // Draw using texture.
            // <GMDX> LL's terrain composite with its water layer, or the map tile once loaded
            // (GMDXMiniMapUseMapTile), instead of the map tile only (white while missing).
            LLViewerTexture* water_tex = nullptr;
            LLViewerTexture* land_tex = regionp->getLand().getMinimapTerrain().getTexture(water_tex);
            for (LLViewerTexture* tex : { land_tex, water_tex })
            {
                if (!tex)
                {
                    continue;
                }
            gGL.getTexUnit(0)->bind(tex);
            // </GMDX>
            gGL.begin(LLRender::TRIANGLES);
            if (mRadarRadius > 0.f)   // <GMDX> cut to the radar's disc
            {
                disc_quad(left, bottom, right, top, 0.f, 0.f, 1.f, 1.f, mRadarRadius);
            }
            else
            {
                gGL.texCoord2f(0.f, 1.f);
                gGL.vertex2f(left, top);
                gGL.texCoord2f(0.f, 0.f);
                gGL.vertex2f(left, bottom);
                gGL.texCoord2f(1.f, 0.f);
                gGL.vertex2f(right, bottom);

                gGL.texCoord2f(0.f, 1.f);
                gGL.vertex2f(left, top);
                gGL.texCoord2f(1.f, 0.f);
                gGL.vertex2f(right, bottom);
                gGL.texCoord2f(1.f, 1.f);
                gGL.vertex2f(right, top);
            }
            gGL.end();

            gGL.flush();
            } // <GMDX>
        }

        const LLVector3& camera_position = gAgentCamera.getCameraPositionAgent();

        if (enable_object_render || minimap_parcel_boundries)
        {
            // Locate the centre of the object layer, accounting for panning
            LLVector3 new_center = globalPosToView(gAgentCamera.getCameraPositionGlobal());
            new_center.mV[VX] -= mCurPan.mV[VX];
            new_center.mV[VY] -= mCurPan.mV[VY];
            new_center.mV[VZ] = 0.f;
            LLVector3d pos_center_global = viewPosToGlobal(llfloor(new_center.mV[VX]), llfloor(new_center.mV[VY]));
            // <GMDX> Show Whole Region: the layers are centred on the region
            if (mFitRegion)
            {
                pos_center_global = mFitCenterGlobal;
            }
            // </GMDX>

            F32 image_half_width = 0.5f * mObjectMapPixels;
            F32 image_half_height = 0.5f * mObjectMapPixels;

            // <GMDX> Draws a layer image centred at `center` (map pixels from the map centre). In
            // Show Whole Region only the part over the region's square, so nothing outside the
            // region shows (the layers hold the neighbours too).
            auto draw_layer = [&](const LLVector3& center)
            {
                F32 x0 = center.mV[VX] - image_half_width;
                F32 x1 = center.mV[VX] + image_half_width;
                F32 y0 = center.mV[VY] - image_half_height;
                F32 y1 = center.mV[VY] + image_half_height;
                F32 u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
                if (mFitRegion)
                {
                    const F32 cx0 = llmax(x0, fit_x0), cx1 = llmin(x1, fit_x1);
                    const F32 cy0 = llmax(y0, fit_y0), cy1 = llmin(y1, fit_y1);
                    if (cx0 >= cx1 || cy0 >= cy1)
                    {
                        return;
                    }
                    u0 = (cx0 - x0) / (x1 - x0);
                    u1 = (cx1 - x0) / (x1 - x0);
                    v0 = (cy0 - y0) / (y1 - y0);
                    v1 = (cy1 - y0) / (y1 - y0);
                    x0 = cx0; x1 = cx1; y0 = cy0; y1 = cy1;
                }
                if (mRadarRadius > 0.f)   // and to the radar's disc
                {
                    gGL.begin(LLRender::TRIANGLES);
                    disc_quad(x0, y0, x1, y1, u0, v0, u1, v1, mRadarRadius);
                    gGL.end();
                    return;
                }
                gGL.begin(LLRender::TRIANGLES);
                {
                    gGL.texCoord2f(u0, v1);
                    gGL.vertex2f(x0, y1);
                    gGL.texCoord2f(u0, v0);
                    gGL.vertex2f(x0, y0);
                    gGL.texCoord2f(u1, v0);
                    gGL.vertex2f(x1, y0);

                    gGL.texCoord2f(u0, v1);
                    gGL.vertex2f(x0, y1);
                    gGL.texCoord2f(u1, v0);
                    gGL.vertex2f(x1, y0);
                    gGL.texCoord2f(u1, v1);
                    gGL.vertex2f(x1, y1);
                }
                gGL.end();
            };
            // </GMDX>

            // <GMDX> Only Render Physical Objects: the static layer is not built or drawn
            if (enable_object_render && physical_only)
            {
                mObjectPassActive = false;
                mUpdateObjectImage = true;   // a fresh layer when it is turned off again
            }
            else
            // </GMDX>
            if (enable_object_render)
            {
                // Redraw object layer periodically
                static LLCachedControl<F32>  object_layer_update_time_setting(gSavedSettings, "SlipstreamMinimapObjectUpdateInterval", 0.1f);
                F32 object_layer_update_time = llclamp(object_layer_update_time_setting(), 0.01f, 60.f);
                // A forced refresh (zoom / resize) restarts any pass in flight.
                if (mUpdateObjectImage || (!mObjectPassActive && map_timer.getElapsedTimeF32() > object_layer_update_time))
                {
                    mUpdateObjectImage = false;
                    mObjectPassActive = true;
                    mObjectPassCursor = 0;
                    mObjectImageCenterGlobal = pos_center_global;
                    LLImageDataLock lock(mObjectRawImagep);
                    memset(mObjectRawImagep->getData(), 0,
                           mObjectImagep->getWidth() * mObjectImagep->getHeight() * mObjectImagep->getComponents());
                    map_timer.reset();
                }
                if (mObjectPassActive)
                {
                    static LLCachedControl<U32> objects_per_frame(gSavedSettings, "SlipstreamMinimapObjectsPerFrame", 4000);
                    {
                        VK_HITCH_SCOPE("ui_netmap_objects");
                        LLImageDataLock lock(mObjectRawImagep);   // once per slice (renderPoint re-enters it)
                        mObjectPassCursor = gObjectList.renderObjectsForMap(*this, mObjectPassCursor,
                                                                            llmax((U32)objects_per_frame, 64U));
                    }
                    if (mObjectPassCursor >= gObjectList.getMapObjectCount())
                    {
                        VK_HITCH_SCOPE("ui_netmap_upload");
                        mObjectImagep->setSubImage(mObjectRawImagep, 0, 0, mObjectImagep->getWidth(), mObjectImagep->getHeight());
                        mObjectImageShownCenterGlobal = mObjectImageCenterGlobal;
                        mObjectPassActive = false;
                    }
                }

                // <GMDX> relative to the map centre (the region's in Show Whole Region)
                LLVector3 layer_center = gAgent.getPosAgentFromGlobal(mObjectImageShownCenterGlobal);
                layer_center -= map_center_agent;
                layer_center.mV[VX] *= scale_pixels_per_meter;
                layer_center.mV[VY] *= scale_pixels_per_meter;

                gGL.getTexUnit(0)->bind(mObjectImagep);
                draw_layer(layer_center);
                // </GMDX>
            }

            if (minimap_parcel_boundries)
            {
                if (mUpdateParcelImage || dist_vec_squared2D(mParcelImageCenterGlobal, pos_center_global) > 9.0f)
                {
                    mUpdateParcelImage = false;
                    mParcelImageCenterGlobal = pos_center_global;

                    // Create the base texture.
                    LLImageDataLock lock(mParcelRawImagep);
                    U8* texture_data = mParcelRawImagep->getData();
                    memset(texture_data, 0, mParcelImagep->getWidth() * mParcelImagep->getHeight() * mParcelImagep->getComponents());

                    // Process each region
                    for (LLViewerRegion* region : LLWorld::instance().getRegionList())
                    {
                        // <GMDX> only the agent's region in Show Whole Region
                        if (mFitRegion && region != current_region)
                        {
                            continue;
                        }
                        // </GMDX>
                        LLColor4 overlay_color = region->isAlive()
                            ? map_parcel_outline_color.get()
                            : LLColor4(255, 128, 128, 255);
                        renderPropertyLinesForRegion(region, overlay_color);
                    }

                    mParcelImagep->setSubImage(mParcelRawImagep, 0, 0, mParcelImagep->getWidth(), mParcelImagep->getHeight());
                }

                // <GMDX> relative to the map centre (the region's in Show Whole Region)
                LLVector3 layer_center = gAgent.getPosAgentFromGlobal(mParcelImageCenterGlobal);
                layer_center -= map_center_agent;
                layer_center.mV[VX] *= scale_pixels_per_meter;
                layer_center.mV[VY] *= scale_pixels_per_meter;

                gGL.getTexUnit(0)->bind(mParcelImagep);
                draw_layer(layer_center);
                // </GMDX>
            }
        }

        // <GMDX> Only Render Physical Objects: every frame, from the live objects
        if (enable_object_render && physical_only)
        {
            drawPhysicalObjects(map_center_agent, scale_pixels_per_meter, mFitRegion ? current_region : nullptr);
        }
        // </GMDX>

        gGL.popMatrix();

        // Mouse pointer in local coordinates
        S32 local_mouse_x;
        S32 local_mouse_y;
        //localMouse(&local_mouse_x, &local_mouse_y);
        LLUI::getInstance()->getMousePositionLocal(this, &local_mouse_x, &local_mouse_y);
        mClosestAgentToCursor.setNull();
        F32 closest_dist_squared = F32_MAX; // value will be overridden in the loop
        const F32 pick_radius = llmax(mDotRadius * MIN_PICK_SCALE, MIN_PICK_PX);   // <GMDX>
        F32 min_pick_dist_squared = pick_radius * pick_radius;
        LLVector2 closest_pos;   // <GMDX> the picked dot, highlighted

        // <GMDX> fills and outlines under the avatars (cut to the radar's disc)
        sDiscCenter = radarCenter();
        sDiscRadius = mRadarRadius;
        drawZones();
        // </GMDX>

        LLVector3 pos_map;
        uuid_vec_t avatar_ids;
        std::vector<LLVector3d> positions;
        bool unknown_relative_z;

        LLWorld::getInstance()->getAvatars(&avatar_ids, &positions, gAgentCamera.getCameraPositionGlobal());

        // Draw avatars
        for (U32 i = 0; i < avatar_ids.size(); i++)
        {
            LLUUID uuid = avatar_ids[i];
            // Skip self, we'll draw it later
            if (uuid == gAgent.getID()) continue;
            // <GMDX> Show Whole Region: only the avatars in the region
            if (mFitRegion && !current_region->pointInRegionGlobal(positions[i])) continue;
            if (gmdx_hidden_above(positions[i])) continue;
            // </GMDX>

            pos_map = globalPosToView(positions[i]);
            // <GMDX> the radar: avatars past the disc / square wait on its rim, in their direction
            if (radarOn())
            {
                const LLVector2 rim = toRadarRim(LLVector2(pos_map.mV[VX], pos_map.mV[VY]));
                pos_map.mV[VX] = rim.mV[VX];
                pos_map.mV[VY] = rim.mV[VY];
            }
            // </GMDX>

// [RLVa:KB] - Checked: RLVa-1.2.0
//            bool show_as_friend = (LLAvatarTracker::instance().getBuddyInfo(uuid) != NULL) && (RlvActions::canShowName(RlvActions::SNC_DEFAULT, uuid));
// [/RLVa:KB]
//          bool show_as_friend = (LLAvatarTracker::instance().getBuddyInfo(uuid) != NULL);

            // <GMDX> group colour (LLGroupColorMap) wins over contact sets, as in SoapStorm
            //const LLColor4 color = LGGContactSets::getInstance()->getAvatarColor(uuid, map_avatar_color, ContactSetType::MINIMAP);
            LLColor4 color = LLGroupColorMap::instance().tint(uuid, LGGContactSets::getInstance()->getAvatarColor(uuid, map_avatar_color, ContactSetType::MINIMAP));
            GMDXMinimapMarks::avatarColor(uuid, color);   // and this account's own colour for the dot over both
            // </GMDX>
            unknown_relative_z = positions[i].mdV[VZ] >= COARSEUPDATE_MAX_Z &&
                    camera_position.mV[VZ] >= COARSEUPDATE_MAX_Z;

            LLWorldMapView::drawAvatar(
                pos_map.mV[VX], pos_map.mV[VY],
                color,
                pos_map.mV[VZ], mDotRadius,
                unknown_relative_z);

            if(uuid.notNull())
            {
                bool selected = false;
                for (const LLUUID& sel_uuid : gmSelected)
                {
                    if(sel_uuid == uuid)
                    {
                        selected = true;
                        break;
                    }
                }
                if(selected)
                {
                    if( (pos_map.mV[VX] < 0) ||
                        (pos_map.mV[VY] < 0) ||
                        (pos_map.mV[VX] >= getRect().getWidth()) ||
                        (pos_map.mV[VY] >= getRect().getHeight()) )
                    {
                        S32 x = ll_round( pos_map.mV[VX] );
                        S32 y = ll_round( pos_map.mV[VY] );
                        LLWorldMapView::drawTrackingCircle( getRect(), x, y, color, 1, 10);
                    }
                    else
                    {
                        LLWorldMapView::drawTrackingDot(pos_map.mV[VX],pos_map.mV[VY],color,0.f);
                    }
                }
            }

            F32 dist_to_cursor_squared = dist_vec_squared(LLVector2(pos_map.mV[VX], pos_map.mV[VY]),
                                          LLVector2((F32)local_mouse_x, (F32)local_mouse_y));
            if(dist_to_cursor_squared < min_pick_dist_squared && dist_to_cursor_squared < closest_dist_squared)
            {
                closest_dist_squared = dist_to_cursor_squared;
                mClosestAgentToCursor = uuid;
                closest_pos.set(pos_map.mV[VX], pos_map.mV[VY]);   // <GMDX>
            }
        }

        // Draw dot for autopilot target
        if (gAgent.getAutoPilot())
        {
            drawTracking( gAgent.getAutoPilotTargetGlobal(), map_track_color );
        }
        else
        {
            LLTracker::ETrackingStatus tracking_status = LLTracker::getTrackingStatus();
            if (  LLTracker::TRACKING_AVATAR == tracking_status )
            {
                drawTracking( LLAvatarTracker::instance().getGlobalPos(), map_track_color );
            }
            else if ( LLTracker::TRACKING_LANDMARK == tracking_status
                    || LLTracker::TRACKING_LOCATION == tracking_status )
            {
                drawTracking( LLTracker::getTrackedPositionGlobal(), map_track_color );
            }
        }

        drawZonePreview();   // <GMDX>
        sDiscRadius = 0.f;   // <GMDX>
        drawMarks();   // <GMDX>
        drawShared();  // <GMDX>

        // Draw dot for self avatar position
        LLVector3d pos_global = gAgent.getPositionGlobal();
        pos_map = globalPosToView(pos_global);
        S32 dot_width = ll_round(mDotRadius * 2.f);
        LLUIImagePtr you = LLWorldMapView::sAvatarYouLargeImage;
        if (you)
        {
            you->draw(ll_round(pos_map.mV[VX] - mDotRadius),
                      ll_round(pos_map.mV[VY] - mDotRadius),
                      dot_width,
                      dot_width);

            F32 dist_to_cursor_squared = dist_vec_squared(LLVector2(pos_map.mV[VX], pos_map.mV[VY]),
                                          LLVector2((F32)local_mouse_x, (F32)local_mouse_y));
            if(dist_to_cursor_squared < min_pick_dist_squared && dist_to_cursor_squared < closest_dist_squared)
            {
                mClosestAgentToCursor = gAgent.getID();
            }
        }

        // <GMDX> the dot under the cursor (the one a right-click picks): a white ring over a dark halo
        if (mCursorInside && mClosestAgentToCursor.notNull() && mClosestAgentToCursor != gAgent.getID())
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.pushMatrix();
            gGL.translatef(closest_pos.mV[VX], closest_pos.mV[VY], 0.f);
            const F32 r = mDotRadius + 3.f;
            const LLColor4 halo(0.05f, 0.06f, 0.07f, 0.6f), ring(1.f, 1.f, 1.f, 0.95f);
            gl_washer_2d(r + 2.5f, r - 1.f, 32, halo, halo);
            gl_washer_2d(r + 1.f, r, 32, ring, ring);
            gGL.popMatrix();
        }
        // </GMDX>

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        // Draw frustum
        // <GMDX> Show Whole Region: the fitted scale
        //F32 meters_to_pixels = mScale/ LLWorld::getInstance()->getRegionWidthInMeters();
        F32 meters_to_pixels = mFitRegion ? mFitPixelsPerMeter : mScale / LLWorld::getInstance()->getRegionWidthInMeters();
        // </GMDX>

        F32 horiz_fov = LLViewerCamera::getInstance()->getView() * LLViewerCamera::getInstance()->getAspect();
        // <GMDX> the draw distance, as Firestorm's cone shows it (its camera far is set from
        // it every frame). Under Vulkan the camera's far is only set at login and on region
        // shifts (display_update_camera runs from display(), which VK skips), so it did not
        // follow the setting. mDrawDistance = RenderFarClip, or less while the VRAM ladder
        // holds it lower (#405): the distance actually drawn.
        //F32 far_clip_meters = LLViewerCamera::getInstance()->getFar();
        F32 far_clip_meters = gAgentCamera.mDrawDistance;
        // </GMDX>
        F32 far_clip_pixels = far_clip_meters * meters_to_pixels;
        // <GMDX> the radar: the cone ends at the disc's rim
        if (mRadarRadius > 0.f)
        {
            const LLVector3 camera_view = globalPosToView(gAgentCamera.getCameraPositionGlobal());
            const F32 from_center = (LLVector2(camera_view.mV[VX], camera_view.mV[VY]) - radarCenter()).length();
            far_clip_pixels = llclamp(mRadarRadius - from_center, 0.f, far_clip_pixels);
        }
        // </GMDX>

        F32 ctr_x = (F32)center_sw_left;
        F32 ctr_y = (F32)center_sw_bottom;
        // <GMDX> Show Whole Region: the map is centred on the region, the cone starts at the camera
        if (mFitRegion)
        {
            const LLVector3 camera_view = globalPosToView(gAgentCamera.getCameraPositionGlobal());
            ctr_x = camera_view.mV[VX];
            ctr_y = camera_view.mV[VY];
        }
        // </GMDX>

        if (map_chat_ring)
        {
            const F32 whisper_radius = current_region->getWhisperRange() * scale_pixels_per_meter;
            const F32 chat_radius = current_region->getChatRange() * scale_pixels_per_meter;
            const F32 shout_radius = current_region->getShoutRange() * scale_pixels_per_meter;

            gGL.pushMatrix();
                gGL.translatef(pos_map.mV[VX], pos_map.mV[VY], 0.f);
                // <GMDX> the radar: only the rings inside its disc (they are centred on it)
                const F32 ring_max = mRadarRadius > 0.f ? mRadarRadius : F32_MAX;
                if (whisper_radius <= ring_max) gl_ring(whisper_radius, 1.75f, map_whisper_ring_color, map_whisper_ring_color, 100, false);
                if (chat_radius <= ring_max) gl_ring(chat_radius, 1.75f, map_chat_ring_color, map_chat_ring_color, 100, false);
                if (shout_radius <= ring_max) gl_ring(shout_radius, 1.75f, map_shout_ring_color, map_shout_ring_color, 100, false);
            gGL.popMatrix();
        }

        const F32 steps_per_circle = 40.0f;
        const F32 steps_per_radian = steps_per_circle / F_TWO_PI;
        const F32 arc_start = -(horiz_fov / 2.0f) + F_PI_BY_TWO;
        const F32 arc_end = (horiz_fov / 2.0f) + F_PI_BY_TWO;
        const S32 steps = llmax(1, (S32)((horiz_fov * steps_per_radian) + 0.5f));

        if( rotate_map )
        {
            gGL.pushMatrix();
                gGL.translatef( ctr_x, ctr_y, 0 );
                gl_washer_segment_2d(far_clip_pixels, 0, arc_start, arc_end, steps, map_frustum_color(), map_frustum_color());
                if (render_guide_line)
                {
                    gGL.begin(LLRender::LINES);
                        gGL.color4fv(map_line_color.get().mV);
                        gGL.vertex2f(0.f, 0.f);
                        gGL.vertex2f(0.f, far_clip_pixels);
                    gGL.end();
                }
            gGL.popMatrix();
        }
        else
        {
            gGL.pushMatrix();
                gGL.translatef( ctr_x, ctr_y, 0 );
                // If we don't rotate the map, we have to rotate the frustum.
                gGL.rotatef( atan2( LLViewerCamera::getInstance()->getAtAxis().mV[VX], LLViewerCamera::getInstance()->getAtAxis().mV[VY] ) * RAD_TO_DEG, 0.f, 0.f, -1.f);
                gl_washer_segment_2d(far_clip_pixels, 0, arc_start, arc_end, steps, map_frustum_rotating_color(), map_frustum_rotating_color());
                if (render_guide_line)
                {
                    gGL.begin(LLRender::LINES);
                        gGL.color4fv(map_line_color.get().mV);
                        gGL.vertex2f(0.f, 0.f);
                        gGL.vertex2f(0.f, far_clip_pixels);
                    gGL.end();
                }
            gGL.popMatrix();
        }
    }

    gGL.popMatrix();
    gGL.popUIMatrix();

    // <GMDX> Show Whole Region: the compass, over the map (gmdxminimapcompass.h)
    if (mFitRegion)
    {
        LLLocalClipRect clip(getLocalRect());
        const LLVector3& at = LLViewerCamera::getInstance()->getAtAxis();
        F32 heading = atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG;   // 0 = north, clockwise
        if (heading < 0.f)
        {
            heading += 360.f;
        }
        const S32 rotating_style = GMDXMinimapCompass::getRotatingStyle();
        if (mRadarSquare)
        {
            GMDXMinimapCompass::drawRadarSquare(rotating_style == GMDXMinimapCompass::STYLE_FRAME ? fit_square : mFitMapRect,
                                                heading, rotating_style);
        }
        else if (mRadarRadius > 0.f)
        {
            const LLVector2 c = radarCenter();
            GMDXMinimapCompass::drawRadar(c.mV[VX], c.mV[VY], mRadarRadius, heading, rotating_style);
        }
        else
        GMDXMinimapCompass::draw(fit_square, heading);
    }
    drawCursorCoordinates();
    // </GMDX>

    LLUICtrl::draw();
}

// <GMDX> Mini-map zones: outlines, no fill, 2 px in the zone's colour over a dark halo, solid or
// dashed; the name above the shape's highest point. While drawing, the shape so far follows the cursor.
namespace
{
// One polyline as quads, dashed with a pattern carried across its corners.
void zone_lines(const std::vector<LLVector2>& pts, bool closed, F32 width, const LLColor4& color, bool dotted)
{
    constexpr F32 DASH = 7.f, GAP = 5.f;
    if (pts.size() < 2) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    F32 phase = 0.f;   // distance into the current dash + gap
    const size_t n = pts.size(), segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        const LLVector2 a = pts[i], b = pts[(i + 1) % n];
        const LLVector2 d = b - a;
        const F32 len = d.length();
        if (len < 0.01f) continue;
        const LLVector2 dir = d / len;
        const LLVector2 nrm(-dir.mV[VY] * width * 0.5f, dir.mV[VX] * width * 0.5f);
        F32 t = 0.f;
        while (t < len)
        {
            F32 run;
            bool on = true;
            if (dotted)
            {
                on = phase < DASH;
                run = llmin((on ? DASH : DASH + GAP) - phase, len - t);
            }
            else
            {
                run = len - t;
            }
            F32 t0 = t, t1 = t + run;
            if (on && sDiscRadius > 0.f)   // the radar: only the part inside the disc
            {
                const LLVector2 f = a - sDiscCenter;
                const F32 r = sDiscRadius - width * 0.5f;
                const F32 bq = f * dir, cq = f * f - r * r, disc = bq * bq - cq;
                if (disc <= 0.f) on = false;
                else
                {
                    t0 = llmax(t0, -bq - sqrtf(disc));
                    t1 = llmin(t1, -bq + sqrtf(disc));
                    on = t1 > t0;
                }
            }
            if (on)
            {
                const LLVector2 p0 = a + dir * t0, p1 = a + dir * t1;
                gGL.vertex2f(p0.mV[VX] - nrm.mV[VX], p0.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] - nrm.mV[VX], p1.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] + nrm.mV[VX], p1.mV[VY] + nrm.mV[VY]);
                gGL.vertex2f(p0.mV[VX] - nrm.mV[VX], p0.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] + nrm.mV[VX], p1.mV[VY] + nrm.mV[VY]);
                gGL.vertex2f(p0.mV[VX] + nrm.mV[VX], p0.mV[VY] + nrm.mV[VY]);
            }
            t += run;
            if (dotted) phase = fmodf(phase + run, DASH + GAP);
        }
    }
    gGL.end();
}

// Triangles (3 points each) covering a simple polygon: ear clipping, a fan when the outline crosses itself.
void zone_triangles(const std::vector<LLVector2>& pts, std::vector<LLVector2>& tris)
{
    tris.clear();
    const size_t n = pts.size();
    if (n < 3) return;
    F32 area = 0.f;
    for (size_t i = 0; i < n; ++i)
    {
        const LLVector2& a = pts[i];
        const LLVector2& b = pts[(i + 1) % n];
        area += a.mV[VX] * b.mV[VY] - b.mV[VX] * a.mV[VY];
    }
    const F32 sign = area >= 0.f ? 1.f : -1.f;
    auto cross = [](const LLVector2& o, const LLVector2& a, const LLVector2& b)
    { return (a.mV[VX] - o.mV[VX]) * (b.mV[VY] - o.mV[VY]) - (a.mV[VY] - o.mV[VY]) * (b.mV[VX] - o.mV[VX]); };
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    size_t guard = n * n + 8;
    while (idx.size() > 3 && guard--)
    {
        bool clipped = false;
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const size_t ip = idx[(k + idx.size() - 1) % idx.size()], ic = idx[k], in = idx[(k + 1) % idx.size()];
            const LLVector2 &a = pts[ip], &b = pts[ic], &c = pts[in];
            if (cross(a, b, c) * sign <= 0.f) continue;   // reflex corner
            bool inside = false;
            for (size_t j : idx)
            {
                if (j == ip || j == ic || j == in) continue;
                const LLVector2& p = pts[j];
                if (cross(a, b, p) * sign > 0.f && cross(b, c, p) * sign > 0.f && cross(c, a, p) * sign > 0.f) { inside = true; break; }
            }
            if (inside) continue;
            tris.push_back(a); tris.push_back(b); tris.push_back(c);
            idx.erase(idx.begin() + k);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3)
    {
        tris.push_back(pts[idx[0]]); tris.push_back(pts[idx[1]]); tris.push_back(pts[idx[2]]);
    }
    else if (idx.size() > 3)   // (a crossing outline) the rest as a fan
    {
        for (size_t k = 1; k + 1 < idx.size(); ++k)
        {
            tris.push_back(pts[idx[0]]); tris.push_back(pts[idx[k]]); tris.push_back(pts[idx[k + 1]]);
        }
    }
}

// A convex polygon cut to the side of the line n.p = c that keep_above says.
void clip_half(std::vector<LLVector2>& poly, const LLVector2& n, F32 c, bool keep_above)
{
    std::vector<LLVector2> out;
    const size_t m = poly.size();
    for (size_t i = 0; i < m; ++i)
    {
        const LLVector2& a = poly[i];
        const LLVector2& b = poly[(i + 1) % m];
        const F32 da = (a * n - c) * (keep_above ? 1.f : -1.f), db = (b * n - c) * (keep_above ? 1.f : -1.f);
        if (da >= 0.f) out.push_back(a);
        if ((da >= 0.f) != (db >= 0.f)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    poly.swap(out);
}

// A convex polygon cut to the disc (a RADAR_SIDES-gon) of radius r around c.
void clip_disc(std::vector<LLVector2>& poly, const LLVector2& c, F32 r)
{
    constexpr S32 RADAR_SIDES = 96;
    const F32 apothem = r * cosf(F_PI / RADAR_SIDES);
    for (S32 k = 0; k < RADAR_SIDES && poly.size() >= 3; ++k)
    {
        const F32 a = F_TWO_PI * (k + 0.5f) / RADAR_SIDES;
        const LLVector2 n(cosf(a), sinf(a));
        clip_half(poly, n, n * c + apothem, false);
    }
    if (poly.size() < 3) poly.clear();
}

// The fill: the zone's colour, or 45-degree black / yellow bands 7 px wide that move with the zone
// (anchor: a point of the zone in view coordinates).
void zone_fill(const std::vector<LLVector2>& outline, S32 fill, const LLColor4& color, const LLVector2& anchor)
{
    std::vector<LLVector2> tris;
    zone_triangles(outline, tris);
    if (tris.empty()) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    if (fill == GMDXMapZone::FILL_COLOR)
    {
        gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.22f);
        gGL.begin(LLRender::TRIANGLES);
        std::vector<LLVector2> tri;
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            tri.assign(tris.begin() + t, tris.begin() + t + 3);
            if (sDiscRadius > 0.f) clip_disc(tri, sDiscCenter, sDiscRadius);
            for (size_t k = 1; k + 1 < tri.size(); ++k)
            {
                gGL.vertex2f(tri[0].mV[VX], tri[0].mV[VY]);
                gGL.vertex2f(tri[k].mV[VX], tri[k].mV[VY]);
                gGL.vertex2f(tri[k + 1].mV[VX], tri[k + 1].mV[VY]);
            }
        }
        gGL.end();
        return;
    }
    constexpr F32 BAND = 7.f;
    const LLVector2 n(F_SQRT2 * 0.5f, F_SQRT2 * 0.5f);
    const F32 base = anchor * n;
    static const LLColor4 yellow(0.98f, 0.80f, 0.10f, 0.42f), black(0.04f, 0.04f, 0.04f, 0.40f);
    std::vector<LLVector2> poly;
    gGL.begin(LLRender::TRIANGLES);
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        F32 lo = F32_MAX, hi = -F32_MAX;
        for (size_t k = 0; k < 3; ++k) { const F32 s = tris[t + k] * n - base; lo = llmin(lo, s); hi = llmax(hi, s); }
        for (S32 band = (S32)floorf(lo / BAND); (F32)band * BAND < hi; ++band)
        {
            poly.assign(tris.begin() + t, tris.begin() + t + 3);
            clip_half(poly, n, base + band * BAND, true);
            clip_half(poly, n, base + (band + 1) * BAND, false);
            if (sDiscRadius > 0.f) clip_disc(poly, sDiscCenter, sDiscRadius);
            if (poly.size() < 3) continue;
            const LLColor4& c = (band & 1) ? black : yellow;
            gGL.color4fv(c.mV);
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                gGL.vertex2f(poly[0].mV[VX], poly[0].mV[VY]);
                gGL.vertex2f(poly[k].mV[VX], poly[k].mV[VY]);
                gGL.vertex2f(poly[k + 1].mV[VX], poly[k + 1].mV[VY]);
            }
        }
    }
    gGL.end();
}

void zone_shape(const std::vector<LLVector2>& pts, bool closed, const LLColor4& color, bool dotted)
{
    zone_lines(pts, closed, 4.f, LLColor4(0.05f, 0.06f, 0.07f, 0.55f), dotted);   // halo
    zone_lines(pts, closed, 2.f, color, dotted);
}
} // namespace

std::vector<LLVector2> LLNetMap::zoneOutline(S32 kind, const std::vector<LLVector3d>& points)
{
    std::vector<LLVector2> out;
    auto view = [this](const LLVector3d& g) { const LLVector3 v = globalPosToView(g); return LLVector2(v.mV[VX], v.mV[VY]); };
    if (kind == GMDXMapZone::RECT && points.size() >= 2)
    {
        const LLVector3d& a = points[0];
        const LLVector3d& b = points[1];
        out = { view(a), view(LLVector3d(b.mdV[VX], a.mdV[VY], 0.0)), view(b), view(LLVector3d(a.mdV[VX], b.mdV[VY], 0.0)) };
    }
    else if (kind == GMDXMapZone::CIRCLE && points.size() >= 2)
    {
        const LLVector3d& c = points[0];
        const F64 r = sqrt((points[1].mdV[VX] - c.mdV[VX]) * (points[1].mdV[VX] - c.mdV[VX])
                           + (points[1].mdV[VY] - c.mdV[VY]) * (points[1].mdV[VY] - c.mdV[VY]));
        constexpr int STEPS = 64;
        for (int i = 0; i < STEPS; ++i)
        {
            const F64 a = F_TWO_PI * i / STEPS;
            out.push_back(view(LLVector3d(c.mdV[VX] + r * cos(a), c.mdV[VY] + r * sin(a), 0.0)));
        }
    }
    else
    {
        for (const LLVector3d& p : points) out.push_back(view(p));
    }
    return out;
}

void LLNetMap::drawZones()
{
    static LLCachedControl<bool> show_zones(gSavedSettings, "GMDXMiniMapShowZones", true);
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    if (show_zones)
    {
        for (const GMDXMapZone& z : GMDXMinimapMarks::zones())
        {
            const std::vector<LLVector2> pts = zoneOutline(z.kind, z.points);
            const LLColor4 c = GMDXMinimapMarks::color(z.color);
            if (z.fill != GMDXMapZone::FILL_NONE && !pts.empty()) zone_fill(pts, z.fill, c, pts[0]);
            zone_shape(pts, true, c, z.dotted);
            if (!z.name.empty() && !pts.empty())
            {
                const LLVector2* top = &pts[0];
                for (const LLVector2& p : pts) if (p.mV[VY] > top->mV[VY]) top = &p;
                if (mRadarRadius > 0.f && (*top - radarCenter()).length() > mRadarRadius) continue;   // <GMDX> off the radar
                font->renderUTF8(z.name, 0, top->mV[VX], top->mV[VY] + 3.f, c, LLFontGL::HCENTER, LLFontGL::BOTTOM,
                                 LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 32);
            }
        }
    }
    drawSharedZones();
}

// <GMDX> Shared zones (gmdxmapshare.h): drawn as mine are, the name then the sender (in the sender's
// group colour) above the top.
void LLNetMap::drawSharedZones()
{
    static LLCachedControl<bool> show_shared(gSavedSettings, "GMDXMapShareShow", true);
    if (!show_shared) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    for (const GMDXSharedItem& s : GMDXMapShare::items())
    {
        if (s.type != GMDXShareText::Type::Zone) continue;
        const GMDXMapZone& z = s.zone;
        const std::vector<LLVector2> pts = zoneOutline(z.kind, z.points);
        if (pts.empty()) continue;
        const LLColor4 c = GMDXMinimapMarks::color(z.color);
        if (z.fill != GMDXMapZone::FILL_NONE) zone_fill(pts, z.fill, c, pts[0]);
        zone_shape(pts, true, c, z.dotted);
        const LLVector2* top = &pts[0];
        for (const LLVector2& p : pts) if (p.mV[VY] > top->mV[VY]) top = &p;
        if (mRadarRadius > 0.f && (*top - radarCenter()).length() > mRadarRadius) continue;   // off the radar
        const std::string name = z.name.empty() ? std::string() : z.name + "  ";
        const std::string sender = GMDXMapShare::senderName(s);
        const F32 left = top->mV[VX] - 0.5f * (F32)(font->getWidth(name) + font->getWidth(sender));
        if (!name.empty())
            font->renderUTF8(name, 0, left, top->mV[VY] + 3.f, c, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        font->renderUTF8(sender, 0, left + (F32)font->getWidth(name), top->mV[VY] + 3.f, GMDXMapShare::senderColor(s), LLFontGL::LEFT,
                         LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}
// </GMDX>

void LLNetMap::drawZonePreview()
{
    static LLCachedControl<bool> new_dotted(gSavedSettings, "GMDXMiniMapZoneDotted", false);
    static LLCachedControl<S32> new_color(gSavedSettings, "GMDXMiniMapZoneColor", 4);
    static LLCachedControl<S32> new_fill(gSavedSettings, "GMDXMiniMapZoneFill", 0);
    if (mDrawKind >= 0)
    {
        std::vector<LLVector3d> pts = mDrawPoints;
        if (!pts.empty() && (mDrawKind == GMDXMapZone::POLYGON || mDrawDrag)) pts.push_back(mDrawCursor);
        const LLColor4 c = GMDXMinimapMarks::color(new_color);
        if (mDrawKind == GMDXMapZone::POLYGON || pts.size() >= 2)
        {
            const std::vector<LLVector2> outline = zoneOutline(mDrawKind, pts);
            if (new_fill != GMDXMapZone::FILL_NONE && outline.size() >= 3) zone_fill(outline, new_fill, c, outline[0]);
            zone_shape(outline, mDrawKind != GMDXMapZone::POLYGON, c, new_dotted);
        }
        static const char* const hints[3] = { "Drag to draw a rectangle. Right-click to cancel.",
                                              "Drag from the centre to draw a circle. Right-click to cancel.",
                                              "Click the corners; double-click, click the first corner or right-click to finish." };
        const LLFontGL* hint_font = LLFontGL::getFontSansSerifSmall();
        const F32 cx = mFitRegion ? mFitMapRect.getCenterX() : getRect().getWidth() * 0.5f;
        const F32 ty = (mFitRegion ? mFitMapRect.mTop : (F32)getRect().getHeight()) - 6.f;
        hint_font->renderUTF8(hints[llclamp(mDrawKind, 0, 2)], 0, cx, ty, LLColor4(0.93f, 0.95f, 0.96f, 1.f),
                              LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}

LLUUID LLNetMap::zoneAt(S32 x, S32 y)
{
    static LLCachedControl<bool> show_zones(gSavedSettings, "GMDXMiniMapShowZones", true);
    if (!show_zones) return LLUUID::null;
    const LLVector2 q((F32)x, (F32)y);
    if (radarOn() && !onMap(LLVector3(q.mV[VX], q.mV[VY], 0.f))) return LLUUID::null;   // <GMDX> off the radar
    LLUUID best;
    F32 best_d = 6.f;   // px from the outline
    for (const GMDXMapZone& z : GMDXMinimapMarks::zones())
    {
        const std::vector<LLVector2> pts = zoneOutline(z.kind, z.points);
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const LLVector2 a = pts[i], b = pts[(i + 1) % pts.size()];
            const LLVector2 ab = b - a;
            const F32 len2 = ab.lengthSquared();
            const F32 t = len2 > 0.f ? llclamp(((q - a) * ab) / len2, 0.f, 1.f) : 0.f;
            const F32 d = (a + ab * t - q).length();
            if (d < best_d) { best_d = d; best = z.id; }
        }
    }
    return best;
}

void LLNetMap::startZone(S32 kind)
{
    mDrawKind = llclamp(kind, 0, 2);
    mDrawPoints.clear();
    mDrawDrag = false;
}

void LLNetMap::finishZone()
{
    GMDXMapZone z;
    z.kind = (GMDXMapZone::Kind)mDrawKind;
    z.points = mDrawPoints;
    z.color = gSavedSettings.getS32("GMDXMiniMapZoneColor");
    z.dotted = gSavedSettings.getBOOL("GMDXMiniMapZoneDotted");
    z.fill = gSavedSettings.getS32("GMDXMiniMapZoneFill");
    mDrawKind = -1;
    mDrawPoints.clear();
    if (z.points.size() < (z.kind == GMDXMapZone::POLYGON ? 3u : 2u)) return;
    LLSD args;
    args["NAME"] = llformat("Zone %d", (S32)GMDXMinimapMarks::zones().size() + 1);
    LLNotificationsUtil::add("GMDXMinimapZoneName", args, LLSD(), [z](const LLSD& n, const LLSD& r) mutable
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
            z.name = r["name"].asString();
            LLStringUtil::trim(z.name);
            GMDXMinimapMarks::addZone(z);
        });
}

// <GMDX> Mini-map marks: a diamond in the mark's colour with a dark outline, its name beside it.
bool LLNetMap::onMap(const LLVector3& p) const
{
    if (mRadarRadius > 0.f)
    {
        return (LLVector2(p.mV[VX], p.mV[VY]) - radarCenter()).length() <= mRadarRadius;
    }
    if (mFitRegion)
    {
        return p.mV[VX] >= mFitMapRect.mLeft && p.mV[VX] <= mFitMapRect.mRight
            && p.mV[VY] >= mFitMapRect.mBottom && p.mV[VY] <= mFitMapRect.mTop;
    }
    return p.mV[VX] >= 0.f && p.mV[VY] >= 0.f && p.mV[VX] <= (F32)getRect().getWidth() && p.mV[VY] <= (F32)getRect().getHeight();
}

void LLNetMap::drawMarks()
{
    static LLCachedControl<bool> show_marks(gSavedSettings, "GMDXMiniMapShowMarks", true);
    if (!show_marks || GMDXMinimapMarks::all().empty()) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    const LLColor4 outline(0.05f, 0.06f, 0.07f, 0.9f);
    for (const GMDXMapMark& m : GMDXMinimapMarks::all())
    {
        const LLVector3 p = globalPosToView(m.pos_global);
        if (!onMap(p)) continue;
        const F32 x = (F32)ll_round(p.mV[VX]), y = (F32)ll_round(p.mV[VY]);
        const LLColor4 c = GMDXMinimapMarks::color(m.color);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (int pass = 0; pass < 2; ++pass)   // outline, then the colour
        {
            const F32 r = pass ? 4.5f : 6.f;
            gGL.color4fv((pass ? c : outline).mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(x, y + r); gGL.vertex2f(x - r, y); gGL.vertex2f(x + r, y);
            gGL.vertex2f(x - r, y); gGL.vertex2f(x, y - r); gGL.vertex2f(x + r, y);
            gGL.end();
        }
        if (!m.name.empty())
        {
            font->renderUTF8(m.name, 0, x + 8.f, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::DROP_SHADOW, 24);
        }
    }
}

LLUUID LLNetMap::markAt(S32 x, S32 y)
{
    static LLCachedControl<bool> show_marks(gSavedSettings, "GMDXMiniMapShowMarks", true);
    if (!show_marks) return LLUUID::null;
    LLUUID best;
    F32 best_d2 = 8.f * 8.f;
    for (const GMDXMapMark& m : GMDXMinimapMarks::all())
    {
        const LLVector3 p = globalPosToView(m.pos_global);
        if (!onMap(p)) continue;
        const F32 dx = p.mV[VX] - (F32)x, dy = p.mV[VY] - (F32)y;
        if (dx * dx + dy * dy < best_d2) { best_d2 = dx * dx + dy * dy; best = m.id; }
    }
    return best;
}

// <GMDX> Shared marks and pings (gmdxmapshare.h): a diamond in the item's colour ringed in the sender's
// group colour, the name then the sender beside it. A ping blips as it arrives and fades out
// (GMDXMapShare::drawBlip / pingAlpha) and, off the map, waits on its edge in its direction.
void LLNetMap::drawShared()
{
    static LLCachedControl<bool> show_shared(gSavedSettings, "GMDXMapShareShow", true);
    if (!show_shared) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    for (const GMDXSharedItem& s : GMDXMapShare::items())
    {
        if (s.type == GMDXShareText::Type::Zone) continue;
        const bool ping = s.type == GMDXShareText::Type::Ping;
        const LLVector3 p = globalPosToView(s.mark.pos_global);
        LLVector2 at(p.mV[VX], p.mV[VY]);
        if (!onMap(p))
        {
            if (!ping) continue;
            if (radarOn() || mFitRegion) at = toRadarRim(at);
            else at.set(llclamp(at.mV[VX], 6.f, (F32)getRect().getWidth() - 6.f), llclamp(at.mV[VY], 6.f, (F32)getRect().getHeight() - 6.f));
        }
        const F32 x = (F32)ll_round(at.mV[VX]), y = (F32)ll_round(at.mV[VY]);
        const F32 alpha = ping ? GMDXMapShare::pingAlpha(s, true) : 1.f;
        LLColor4 c = GMDXMinimapMarks::color(s.mark.color);
        LLColor4 ring = GMDXMapShare::senderColor(s);
        c.mV[VALPHA] *= alpha;
        ring.mV[VALPHA] *= alpha;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        if (ping) GMDXMapShare::drawBlip(x, y, s, 26.f);   // a radar blip as it arrives (GMDXPingBlip)
        gGL.color4fv(ring.mV);
        gl_circle_2d(x, y, 7.5f, 20, false);
        for (int pass = 0; pass < 2; ++pass)   // outline, then the colour
        {
            const F32 r = pass ? 4.5f : 6.f;
            gGL.color4fv((pass ? c : LLColor4(0.05f, 0.06f, 0.07f, 0.9f * alpha)).mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(x, y + r); gGL.vertex2f(x - r, y); gGL.vertex2f(x + r, y);
            gGL.vertex2f(x - r, y); gGL.vertex2f(x, y - r); gGL.vertex2f(x + r, y);
            gGL.end();
        }
        const std::string name = s.mark.name.empty() ? std::string() : s.mark.name + "  ";
        if (!name.empty())
            font->renderUTF8(name, 0, x + 10.f, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        font->renderUTF8(GMDXMapShare::senderName(s), 0, x + 10.f + (F32)font->getWidth(name), y, ring, LLFontGL::LEFT,
                         LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}

LLUUID LLNetMap::sharedAt(S32 x, S32 y, bool zones)
{
    static LLCachedControl<bool> show_shared(gSavedSettings, "GMDXMapShareShow", true);
    if (!show_shared) return LLUUID::null;
    const LLVector2 q((F32)x, (F32)y);
    if (zones && radarOn() && !onMap(LLVector3(q.mV[VX], q.mV[VY], 0.f))) return LLUUID::null;   // off the radar
    LLUUID best, inside_best;
    F32 best_d = zones ? 6.f : 8.f;   // px from the outline / the diamond
    for (const GMDXSharedItem& s : GMDXMapShare::items())
    {
        if ((s.type == GMDXShareText::Type::Zone) != zones) continue;
        if (!zones)
        {
            const LLVector3 p = globalPosToView(s.mark.pos_global);
            if (!onMap(p)) continue;
            const F32 d = (LLVector2(p.mV[VX], p.mV[VY]) - q).length();
            if (d < best_d) { best_d = d; best = s.id; }
            continue;
        }
        const std::vector<LLVector2> pts = zoneOutline(s.zone.kind, s.zone.points);
        bool inside = false;   // (even-odd: a ray to the right crosses the outline an odd number of times)
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const LLVector2 a = pts[i], b = pts[(i + 1) % pts.size()];
            const LLVector2 ab = b - a;
            const F32 len2 = ab.lengthSquared();
            const F32 t = len2 > 0.f ? llclamp(((q - a) * ab) / len2, 0.f, 1.f) : 0.f;
            const F32 d = (a + ab * t - q).length();
            if (d < best_d) { best_d = d; best = s.id; }
            if ((a.mV[VY] > q.mV[VY]) != (b.mV[VY] > q.mV[VY])
                && q.mV[VX] < a.mV[VX] + (q.mV[VY] - a.mV[VY]) * ab.mV[VX] / ab.mV[VY])
                inside = !inside;
        }
        if (inside) inside_best = s.id;   // (of zones the cursor is in, the last drawn: on top)
    }
    return best.notNull() ? best : inside_best;   // an outline near the cursor first, else the zone it is in
}
// </GMDX>

// The region coordinates under the cursor (GMDXMiniMapCursorCoords), on a dark pill beside it;
// over a mark, its name first.
void LLNetMap::drawCursorCoordinates()
{
    static LLCachedControl<bool> show_coords(gSavedSettings, "GMDXMiniMapCursorCoords", true);
    if (!show_coords || !mCursorInside || mPanning || hasMouseCapture() || isMouseOnPopupMenu()) return;
    const LLVector3 at((F32)mCursorX, (F32)mCursorY, 0.f);
    if (!onMap(at)) return;
    const LLVector3d pos = GMDXMinimapMarks::onGround(viewPosToGlobal(mCursorX, mCursorY));
    std::string text = GMDXMinimapMarks::coordinatesText(pos);
    if (const GMDXMapMark* m = GMDXMinimapMarks::find(markAt(mCursorX, mCursorY)))
    {
        text = m->name + "  " + text;
    }
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 w = font->getWidth(text) + 10, h = (S32)font->getLineHeight() + 4;
    S32 left = mCursorX + 14, top = mCursorY - 10;
    left = llclamp(left, 2, llmax(getRect().getWidth() - w - 2, 2));
    if (left < mCursorX + 14) left = llmax(mCursorX - w - 8, 2);   // no room right of the cursor: left of it
    top = llclamp(top, h + 2, getRect().getHeight() - 2);
    gl_rect_2d(left, top, left + w, top - h, LLColor4(0.05f, 0.06f, 0.07f, 0.8f));
    font->renderUTF8(text, 0, (F32)(left + 5), (F32)(top - h / 2), LLColor4(0.93f, 0.95f, 0.96f, 1.f),
                     LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}

void LLNetMap::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mCursorInside = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

void LLNetMap::copyPopupPosition(bool slurl)
{
    const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark);
    const LLVector3d pos = m ? m->pos_global : GMDXMinimapMarks::onGround(mPopupWorldPos);
    const std::string text = slurl ? GMDXMinimapMarks::slurl(pos) : GMDXMinimapMarks::coordinatesText(pos);
    if (text.empty()) return;
    const LLWString w = utf8str_to_wstring(text);
    LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
}

void LLNetMap::addMarkAtPopup(S32 color)
{
    const LLVector3d pos = GMDXMinimapMarks::onGround(mPopupWorldPos);
    LLSD args;
    args["NAME"] = llformat("Mark %d", (S32)GMDXMinimapMarks::all().size() + 1);
    LLNotificationsUtil::add("GMDXMinimapMarkName", args, LLSD(), [pos, color](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
            std::string name = r["name"].asString();
            LLStringUtil::trim(name);
            GMDXMinimapMarks::add(pos, name.empty() ? std::string("Mark") : name, color);
        });
}
// </GMDX>

// <GMDX> Only Render Physical Objects (GMDXMiniMapPhysicalOnly): every physical object
// (FLAGS_USE_PHYSICS: bullets, vehicles and other dynamic objects) as a dot with a short trail
// back along its velocity, from the live objects every frame. The static object layer holds
// only owned or larger objects (renderObjectsForMap), so bullets never reach it. Physical
// objects at rest leave the active list and are not shown. Drawn in the map's frame (centred
// on the map centre, rotated with it); only_region = Show Whole Region's region, or null.
void LLNetMap::drawPhysicalObjects(const LLVector3& map_center_agent, F32 pixels_per_meter, const LLViewerRegion* only_region)
{
    static LLUIColor physical_color = LLUIColorTable::instance().getColor("GMDXMapPhysicalColor", LLColor4(1.f, 0.6f, 0.1f, 1.f));
    constexpr F32 TRAIL_SECONDS = 0.15f;    // the trail: where the object was this long ago
    constexpr F32 MIN_HALF_SIZE = 1.5f;     // a 3 px dot at least
    constexpr F32 MAX_RADIUS_M = 8.f;       // big physical objects (vehicles) stay a marker

    const LLColor4 color = physical_color.get();
    const LLColor4 tail(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.f);
    const F32 view_radius = mRadarRadius > 0.f ? mRadarRadius : 0.5f * mObjectMapPixels + 2.f;    // the radar's disc, else half the view's diagonal
    const F32 view_radius_sq = view_radius * view_radius;
    auto vertex = [](F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    };

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (const LLPointer<LLViewerObject>& objectp : gObjectList.getActiveObjects())
    {
        // Roots only (a linkset is one marker); a root with a parent is an attachment.
        if (objectp.isNull() || objectp->isDead() || !objectp->flagUsePhysics() || objectp->isAvatar()
            || objectp->isAttachment() || objectp->getParent())
        {
            continue;
        }
        if (only_region && objectp->getRegion() != only_region)
        {
            continue;
        }
        const LLVector3 rel = objectp->getPositionAgent() - map_center_agent;
        const F32 x = rel.mV[VX] * pixels_per_meter;
        const F32 y = rel.mV[VY] * pixels_per_meter;
        if (x * x + y * y > view_radius_sq)
        {
            continue;
        }

        const LLVector3& scale = objectp->getScale();
        const F32 half = llmax(MIN_HALF_SIZE, llmin(0.25f * (scale.mV[VX] + scale.mV[VY]), MAX_RADIUS_M) * pixels_per_meter);

        // The trail, 1.5 px wide, fading out towards where it came from.
        const LLVector3& vel = objectp->getVelocity();
        const F32 tx = -vel.mV[VX] * TRAIL_SECONDS * pixels_per_meter;
        const F32 ty = -vel.mV[VY] * TRAIL_SECONDS * pixels_per_meter;
        const F32 len = sqrtf(tx * tx + ty * ty);
        if (len > half)
        {
            const F32 nx = -ty / len * 0.75f;
            const F32 ny = tx / len * 0.75f;
            vertex(x + nx, y + ny, color); vertex(x - nx, y - ny, color); vertex(x + tx - nx, y + ty - ny, tail);
            vertex(x + nx, y + ny, color); vertex(x + tx - nx, y + ty - ny, tail); vertex(x + tx + nx, y + ty + ny, tail);
        }

        vertex(x - half, y - half, color); vertex(x + half, y - half, color); vertex(x + half, y + half, color);
        vertex(x - half, y - half, color); vertex(x + half, y + half, color); vertex(x - half, y + half, color);
    }
    gGL.end();
}
// </GMDX>

void LLNetMap::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    createObjectImage();
    createParcelImage();   // same size as the object layer, or the parcel lines drift (#100)
}

LLVector3 LLNetMap::globalPosToView(const LLVector3d& global_pos)
{
    LLVector3d camera_position = mapCenterGlobal(); // <GMDX> the region's centre in Show Whole Region

    LLVector3d relative_pos_global = global_pos - camera_position;
    LLVector3 pos_local;
    pos_local.setVec(relative_pos_global);  // convert to floats from doubles

    pos_local.mV[VX] *= mPixelsPerMeter;
    pos_local.mV[VY] *= mPixelsPerMeter;
    // leave Z component in meters

    if( rotateMap() ) // <GMDX> north-up in Show Whole Region
    {
        F32 radians = atan2( LLViewerCamera::getInstance()->getAtAxis().mV[VX], LLViewerCamera::getInstance()->getAtAxis().mV[VY] );
        LLQuaternion rot(radians, LLVector3(0.f, 0.f, 1.f));
        pos_local.rotVec( rot );
    }

    // <GMDX> mapPan(): no pan in Show Whole Region
    const LLVector2 pan = mapPan();
    pos_local.mV[VX] += getRect().getWidth() / 2 + pan.mV[VX];
    pos_local.mV[VY] += getRect().getHeight() / 2 + pan.mV[VY];
    // </GMDX>

    return pos_local;
}

void LLNetMap::drawTracking(const LLVector3d& pos_global, const LLColor4& color,
                            bool draw_arrow )
{
    LLVector3 pos_local = globalPosToView(pos_global);
    // <GMDX> the radar, and the Whole Region map: on its rim / edge when past it (the map is
    // clipped to its square, so an arrow at the view's edge would be cut away)
    if (radarOn() || mFitRegion)
    {
        const LLVector2 rim = toRadarRim(LLVector2(pos_local.mV[VX], pos_local.mV[VY]));
        LLWorldMapView::drawTrackingDot(rim.mV[VX], rim.mV[VY], color, pos_local.mV[VZ]);
        return;
    }
    // </GMDX>
    if( (pos_local.mV[VX] < 0) ||
        (pos_local.mV[VY] < 0) ||
        (pos_local.mV[VX] >= getRect().getWidth()) ||
        (pos_local.mV[VY] >= getRect().getHeight()) )
    {
        if (draw_arrow)
        {
            S32 x = ll_round( pos_local.mV[VX] );
            S32 y = ll_round( pos_local.mV[VY] );
            LLWorldMapView::drawTrackingCircle( getRect(), x, y, color, 1, 10 );
            LLWorldMapView::drawTrackingArrow( getRect(), x, y, color );
        }
    }
    else
    {
        LLWorldMapView::drawTrackingDot(pos_local.mV[VX],
                                        pos_local.mV[VY],
                                        color,
                                        pos_local.mV[VZ]);
    }
}

bool LLNetMap::isMouseOnPopupMenu()
{
    auto menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (!menu || !menu->isOpen())
    {
        return false;
    }

    S32 popup_x;
    S32 popup_y;
    LLUI::getInstance()->getMousePositionLocal(menu, &popup_x, &popup_y);
    // *NOTE: Tolerance is larger than it needs to be because the context menu is offset from the mouse when the menu is opened from certain
    // directions. This may be a quirk of LLMenuGL::showPopup. -Cosmic,2022-03-22
    constexpr S32 tolerance = 10;
    // Test tolerance from all four corners, as the popup menu can appear from a different direction if there's not enough space.
    // Assume the size of the popup menu is much larger than the provided tolerance.
    // In practice, this is a [tolerance]px margin around the popup menu.
    for (S32 sign_x = -1; sign_x <= 1; sign_x += 2)
    {
        for (S32 sign_y = -1; sign_y <= 1; sign_y += 2)
        {
            if (menu->pointInView(popup_x + (sign_x * tolerance), popup_y + (sign_y * tolerance)))
            {
                return true;
            }
        }
    }
    return false;
}

void LLNetMap::updateAboutLandPopupButton()
{
    auto menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (!menu || !menu->isOpen())
    {
        return;
    }

    LLViewerRegion *region = LLWorld::getInstance()->getRegionFromPosGlobal(mPopupWorldPos);
    if (!region)
    {
        menu->setItemEnabled("About Land", false);
    }
    else
    {
        // Check if the mouse is in the bounds of the popup. If so, it's safe to assume no other hover function will be called, so the hover
        // parcel can be used to check if location-sensitive tooltip options are available.
        if (isMouseOnPopupMenu())
        {
            LLViewerParcelMgr::getInstance()->setHoverParcel(mPopupWorldPos);
            LLParcel *hover_parcel = LLViewerParcelMgr::getInstance()->getHoverParcel();
            bool      valid_parcel = false;
            if (hover_parcel)
            {
                valid_parcel = hover_parcel->getOwnerID().notNull();
            }
            menu->setItemEnabled("About Land", valid_parcel);
        }
    }
}

LLVector3d LLNetMap::viewPosToGlobal( S32 x, S32 y )
{
    // <GMDX> Show Whole Region: no pan, north-up, its own scale and centre
    const LLVector2 pan = mapPan();
    x -= ll_round(getRect().getWidth() / 2 + pan.mV[VX]);
    y -= ll_round(getRect().getHeight() / 2 + pan.mV[VY]);

    LLVector3 pos_local( (F32)x, (F32)y, 0 );

    F32 radians = - atan2( LLViewerCamera::getInstance()->getAtAxis().mV[VX], LLViewerCamera::getInstance()->getAtAxis().mV[VY] );

    if( rotateMap() )
    {
        LLQuaternion rot(radians, LLVector3(0.f, 0.f, 1.f));
        pos_local.rotVec( rot );
    }

    pos_local *= mFitRegion ? 1.f / mFitPixelsPerMeter : ( LLWorld::getInstance()->getRegionWidthInMeters() / mScale );

    LLVector3d pos_global;
    pos_global.setVec( pos_local );
    pos_global += mapCenterGlobal();
    // </GMDX>

    return pos_global;
}

// <GMDX> Whole Region zoom: the view (region_width / zoom metres across) stays inside the region;
// at zoom 1 it is the region's middle.
void LLNetMap::clampFitView(F32 region_width)
{
    const F32 half = 0.5f * region_width / llmax(mFitZoom, 1.f);
    mFitViewCenter.mV[VX] = llclamp(mFitViewCenter.mV[VX], half, region_width - half);
    mFitViewCenter.mV[VY] = llclamp(mFitViewCenter.mV[VY], half, region_width - half);
}

bool LLNetMap::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    // <GMDX> Show Whole Region: its own zoom, 1 (the whole region) to 8, toward the point under the
    // cursor; MiniMapScale is left alone. Zooming back out to 1 shows the whole region again.
    if (mFitRegion)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || mFitBasePixelsPerMeter <= 0.f
            || (rotateMap() && gSavedSettings.getS32("GMDXMiniMapRotateView") == 2))   // (the turning whole region: always whole)
        {
            return true;
        }
        const F32 old_ppm = mFitBasePixelsPerMeter * mFitZoom;
        mFitZoom = llclamp(mFitZoom * (F32)pow(MAP_SCALE_ZOOM_FACTOR, -clicks), 1.f, 8.f);
        const F32 new_ppm = mFitBasePixelsPerMeter * mFitZoom;
        // keep the point under the cursor where it is
        const LLVector2 cursor((F32)x - mFitMapRect.getCenterX(), (F32)y - mFitMapRect.getCenterY());
        const LLVector2 under = mFitViewCenter + cursor / old_ppm;
        mFitViewCenter = under - cursor / new_ppm;
        clampFitView(region->getWidth());
        return true;
    }
    // </GMDX>
    // note that clicks are reversed from what you'd think: i.e. > 0  means zoom out, < 0 means zoom in
    F32 new_scale = mScale * (F32)pow(MAP_SCALE_ZOOM_FACTOR, -clicks);
    F32 old_scale = mScale;

    setScale(new_scale);

    static LLUICachedControl<bool> auto_center("MiniMapAutoCenter", true);
    if (!auto_center)
    {
        // Adjust pan to center the zoom on the mouse pointer
        LLVector2 zoom_offset;
        zoom_offset.mV[VX] = (F32)(x - getRect().getWidth() / 2);
        zoom_offset.mV[VY] = (F32)(y - getRect().getHeight() / 2);
        mCurPan -= zoom_offset * mScale / old_scale - zoom_offset;
    }

    return true;
}

bool LLNetMap::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (gDisconnected)
    {
        return false;
    }

    // If the cursor is near an avatar on the minimap, a mini-inspector will be
    // shown for the avatar, instead of the normal map tooltip.
//    if (handleToolTipAgent(mClosestAgentToCursor))
// [RLVa:KB] - Checked: RLVa-1.2.2
    bool fRlvCanShowName = (mClosestAgentToCursor.notNull()) && (RlvActions::canShowName(RlvActions::SNC_DEFAULT, mClosestAgentToCursor));
    if ( (fRlvCanShowName) && (handleToolTipAgent(mClosestAgentToCursor)) )
// [/RLVa:KB]
    {
        return true;
    }

    // The popup menu uses the hover parcel when it is open and the mouse is on
    // top of it, with some additional tolerance. Returning early here prevents
    // fighting over that hover parcel when getting tooltip info in the
    // tolerance region.
    if (isMouseOnPopupMenu())
    {
        return false;
    }

    LLRect sticky_rect;
    S32 SLOP = 4;
    localPointToScreen(x - SLOP, y - SLOP, &(sticky_rect.mLeft), &(sticky_rect.mBottom));
    sticky_rect.mRight = sticky_rect.mLeft + 2 * SLOP;
    sticky_rect.mTop   = sticky_rect.mBottom + 2 * SLOP;

    std::string parcel_name_msg;
    std::string parcel_sale_price_msg;
    std::string parcel_sale_area_msg;
    std::string parcel_owner_msg;
    std::string region_name_msg;

    LLVector3d      posGlobal = viewPosToGlobal(x, y);
    LLViewerRegion *region    = LLWorld::getInstance()->getRegionFromPosGlobal(posGlobal);
    if (region)
    {
//        std::string region_name = region->getName();
// [RLVa:KB] - Checked: RLVa-1.2.2
        std::string region_name = (RlvActions::canShowLocation()) ? region->getName() : RlvStrings::getString(RlvStringKeys::Hidden::Region);
// [/RLVa:KB]
        if (!region_name.empty())
        {
            region_name_msg = mRegionNameMsg;
            LLStringUtil::format(region_name_msg, {{"[REGION_NAME]", region_name}});
        }

        // Only show parcel information in the tooltip if property lines are visible. Otherwise, the parcel the tooltip is referring to is
        // ambiguous.
        if (gSavedSettings.getBOOL("MiniMapShowPropertyLines"))
        {
            LLViewerParcelMgr::getInstance()->setHoverParcel(posGlobal);
            LLParcel *hover_parcel = LLViewerParcelMgr::getInstance()->getHoverParcel();
            if (hover_parcel)
            {
                std::string parcel_name = hover_parcel->getName();
                if (!parcel_name.empty())
                {
                    parcel_name_msg = mParcelNameMsg;
                    LLStringUtil::format(parcel_name_msg, {{"[PARCEL_NAME]", parcel_name}});
                }

                const LLUUID      parcel_owner          = hover_parcel->getOwnerID();
                std::string       parcel_owner_name_url = LLSLURL("agent", parcel_owner, "inspect").getSLURLString();
                static LLUrlMatch parcel_owner_name_url_match;
                LLUrlRegistry::getInstance()->findUrl(parcel_owner_name_url, parcel_owner_name_url_match);
                if (!parcel_owner_name_url_match.empty())
                {
                    parcel_owner_msg              = mParcelOwnerMsg;
                    std::string parcel_owner_name = parcel_owner_name_url_match.getLabel();
                    LLStringUtil::format(parcel_owner_msg, {{"[PARCEL_OWNER]", parcel_owner_name}});
                }

                if (hover_parcel->getForSale())
                {
                    const LLUUID auth_buyer_id = hover_parcel->getAuthorizedBuyerID();
                    const LLUUID agent_id      = gAgent.getID();
                    bool         show_for_sale = auth_buyer_id.isNull() || auth_buyer_id == agent_id || parcel_owner == agent_id;
                    if (show_for_sale)
                    {
                        S32 price        = hover_parcel->getSalePrice();
                        S32 area         = hover_parcel->getArea();
                        F32 cost_per_sqm = 0.0f;
                        if (area > 0)
                        {
                            cost_per_sqm = F32(price) / area;
                        }
                        std::string formatted_price          = LLResMgr::getInstance()->getMonetaryString(price);
                        std::string formatted_cost_per_meter = llformat("%.1f", cost_per_sqm);
                        parcel_sale_price_msg                = mParcelSalePriceMsg;
                        LLStringUtil::format(parcel_sale_price_msg,
                                             {{"[PRICE]", formatted_price}, {"[PRICE_PER_SQM]", formatted_cost_per_meter}});
                        std::string formatted_area = llformat("%d", area);
                        parcel_sale_area_msg       = mParcelSaleAreaMsg;
                        LLStringUtil::format(parcel_sale_area_msg, {{"[AREA]", formatted_area}});
                    }
                }
            }
        }
    }

    std::string tool_tip_hint_msg;
    if (gSavedSettings.getBOOL("DoubleClickTeleport"))
    {
        tool_tip_hint_msg = mAltToolTipHintMsg;
    }
    else if (gSavedSettings.getBOOL("DoubleClickShowWorldMap"))
    {
        tool_tip_hint_msg = mToolTipHintMsg;
    }

    LLStringUtil::format_map_t args;
    args["[PARCEL_NAME_MSG]"]       = parcel_name_msg.empty() ? "" : parcel_name_msg + '\n';
    args["[PARCEL_SALE_PRICE_MSG]"] = parcel_sale_price_msg.empty() ? "" : parcel_sale_price_msg + '\n';
    args["[PARCEL_SALE_AREA_MSG]"]  = parcel_sale_area_msg.empty() ? "" : parcel_sale_area_msg + '\n';
    args["[PARCEL_OWNER_MSG]"]      = parcel_owner_msg.empty() ? "" : parcel_owner_msg + '\n';
    args["[REGION_NAME_MSG]"]       = region_name_msg.empty() ? "" : region_name_msg + '\n';
    args["[TOOL_TIP_HINT_MSG]"]     = tool_tip_hint_msg.empty() ? "" : tool_tip_hint_msg + '\n';
// [RLVa:KB] - Checked: RLVa-1.2.2
    LLAvatarName avName;
    args["[AGENT]"] = ( (!fRlvCanShowName) && (mClosestAgentToCursor.notNull()) && (LLAvatarNameCache::get(mClosestAgentToCursor, &avName)) ) ? RlvStrings::getAnonym(avName) + "\n" : "";
// [/RLVa:KB]

    std::string msg                 = mToolTipMsg;
    LLStringUtil::format(msg, args);
    if (msg.back() == '\n')
    {
        msg.resize(msg.size() - 1);
    }
    LLToolTipMgr::instance().show(LLToolTip::Params().message(msg).sticky_rect(sticky_rect));

    return true;
}

bool LLNetMap::handleToolTipAgent(const LLUUID& avatar_id)
{
    LLAvatarName av_name;
    if (avatar_id.isNull() || !LLAvatarNameCache::get(avatar_id, &av_name))
    {
        return false;
    }

    // only show tooltip if same inspector not already open
    LLFloater* existing_inspector = LLFloaterReg::findInstance("inspect_avatar");
    if (!existing_inspector
        || !existing_inspector->getVisible()
        || existing_inspector->getKey()["avatar_id"].asUUID() != avatar_id)
    {
        LLInspector::Params p;
        p.fillFrom(LLUICtrlFactory::instance().getDefaultParams<LLInspector>());
        p.message(av_name.getCompleteName());
        p.image.name("Inspector_I");
        p.click_callback(boost::bind(showAvatarInspector, avatar_id));
        p.visible_time_near(6.f);
        p.visible_time_far(3.f);
        p.delay_time(0.35f);
        p.wrap(false);

        LLToolTipMgr::instance().show(p);
    }
    return true;
}

// static
void LLNetMap::showAvatarInspector(const LLUUID& avatar_id)
{
    LLSD params;
    params["avatar_id"] = avatar_id;

    if (LLToolTipMgr::instance().toolTipVisible())
    {
        LLRect rect = LLToolTipMgr::instance().getToolTipRect();
        params["pos"]["x"] = rect.mLeft;
        params["pos"]["y"] = rect.mTop;
    }

    LLFloaterReg::showInstance("inspect_avatar", params);
}

void LLNetMap::renderScaledPointGlobal( const LLVector3d& pos, const LLColor4U &color, F32 radius_meters )
{
    LLVector3 local_pos;
    local_pos.setVec( pos - mObjectImageCenterGlobal );

    S32 diameter_pixels = ll_round(2 * radius_meters * mObjectMapTPM);
    renderPoint( local_pos, color, diameter_pixels );
}


void LLNetMap::renderPoint(const LLVector3 &pos_local, const LLColor4U &color,
                           S32 diameter, S32 relative_height)
{
    if (diameter <= 0)
    {
        return;
    }

    const S32 image_width = (S32)mObjectImagep->getWidth();
    const S32 image_height = (S32)mObjectImagep->getHeight();

    S32 x_offset = ll_round(pos_local.mV[VX] * mObjectMapTPM + image_width / 2);
    S32 y_offset = ll_round(pos_local.mV[VY] * mObjectMapTPM + image_height / 2);

    if ((x_offset < 0) || (x_offset >= image_width))
    {
        return;
    }
    if ((y_offset < 0) || (y_offset >= image_height))
    {
        return;
    }

    LLImageDataLock lock(mObjectRawImagep);
    U8 *datap = mObjectRawImagep->getData();

    S32 neg_radius = diameter / 2;
    S32 pos_radius = diameter - neg_radius;
    S32 x, y;

    if (relative_height > 0)
    {
        // ...point above agent
        S32 px, py;

        // vertical line
        px = x_offset;
        for (y = -neg_radius; y < pos_radius; y++)
        {
            py = y_offset + y;
            if ((py < 0) || (py >= image_height))
            {
                continue;
            }
            S32 offset = px + py * image_width;
            ((U32*)datap)[offset] = color.asRGBA();
        }

        // top line
        py = y_offset + pos_radius - 1;
        for (x = -neg_radius; x < pos_radius; x++)
        {
            px = x_offset + x;
            if ((px < 0) || (px >= image_width))
            {
                continue;
            }
            S32 offset = px + py * image_width;
            ((U32*)datap)[offset] = color.asRGBA();
        }
    }
    else
    {
        // ...point level with agent
        for (x = -neg_radius; x < pos_radius; x++)
        {
            S32 p_x = x_offset + x;
            if ((p_x < 0) || (p_x >= image_width))
            {
                continue;
            }

            for (y = -neg_radius; y < pos_radius; y++)
            {
                S32 p_y = y_offset + y;
                if ((p_y < 0) || (p_y >= image_height))
                {
                    continue;
                }
                S32 offset = p_x + p_y * image_width;
                ((U32*)datap)[offset] = color.asRGBA();
            }
        }
    }
}

void LLNetMap::renderPropertyLinesForRegion(LLViewerRegion* region, const LLColor4U& overlay_color)
{
    const S32 img_width = mParcelImagep->getWidth();
    const S32 img_height = mParcelImagep->getHeight();

    const LLVector3 origin_local(region->getOriginGlobal() - mParcelImageCenterGlobal);
    const S32 origin_x = ll_round(origin_local.mV[VX] * mObjectMapTPM + img_width / 2);
    const S32 origin_y = ll_round(origin_local.mV[VY] * mObjectMapTPM + img_height / 2);

    LLImageDataLock lock(mParcelRawImagep);
    U32* texture_data = reinterpret_cast<U32*>(mParcelRawImagep->getData());

    //
    // Draw the north and east region borders
    //
    const F32 real_width(region->getWidth());
    const S32 border_y = origin_y + ll_round(real_width * mObjectMapTPM);
    if ( (border_y >= 0) && (border_y < img_height) )
    {
        S32 cur_x = llclamp(origin_x, 0, img_width);
        S32 end_x = llclamp(origin_x + ll_round(real_width * mObjectMapTPM), 0, img_width - 1);
        for (; cur_x <= end_x; cur_x++)
            texture_data[border_y * img_width + cur_x] = overlay_color.asRGBA();
    }
    const S32 border_x = origin_x + ll_round(real_width * mObjectMapTPM);
    if ( (border_x >= 0) && (border_x < img_width) )
    {
        S32 cur_y = llclamp(origin_y, 0, img_height);
        S32 end_y = llclamp(origin_y + ll_round(real_width * mObjectMapTPM), 0, img_height - 1);
        for (; cur_y <= end_y; cur_y++)
            texture_data[cur_y * img_width + border_x] = overlay_color.asRGBA();
    }

    //
    // Render parcel lines
    //
    const F32 GRID_STEP = PARCEL_GRID_STEP_METERS;
    const S32 GRIDS_PER_EDGE = S32(real_width / GRID_STEP);

    const U8* ownership = region->getParcelOverlay()->getOwnership();
    const U8* collision = (region->getHandle() == LLViewerParcelMgr::instance().getCollisionRegionHandle()) ? LLViewerParcelMgr::instance().getCollisionBitmap() : NULL;
    for (S32 idxRow = 0; idxRow < GRIDS_PER_EDGE; idxRow++)
    {
        for (S32 idxCol = 0; idxCol < GRIDS_PER_EDGE; idxCol++)
        {
            S32 overlay = ownership[idxRow * GRIDS_PER_EDGE + idxCol];
            S32 idx_collision = idxRow * GRIDS_PER_EDGE + idxCol;
            bool for_sale = ((overlay & PARCEL_COLOR_MASK) == PARCEL_FOR_SALE);
            bool auction = ((overlay & PARCEL_COLOR_MASK) == PARCEL_AUCTION);
            bool collides = (collision) && (collision[idx_collision / 8] & (1 << (idx_collision % 8)));
            if ( (!for_sale) && (!collides) && (!auction) && (0 == (overlay & (PARCEL_SOUTH_LINE | PARCEL_WEST_LINE))) )
                continue;

            const S32 pos_x = origin_x + ll_round(idxCol * GRID_STEP * mObjectMapTPM);
            const S32 pos_y = origin_y + ll_round(idxRow * GRID_STEP * mObjectMapTPM);

            static LLCachedControl<bool> sShowForSaleParcels(gSavedSettings, "SlipstreamMiniMapForSaleParcels", false);
            static LLCachedControl<bool> sShowCollisionParcels(gSavedSettings, "SlipstreamMiniMapCollisionParcels", false);
            if ( ((sShowForSaleParcels) && (for_sale || auction)) || ((sShowCollisionParcels) && (collides)) )
            {
                S32 cur_y = llclamp(pos_y, 0, img_height);
                S32 end_y = llclamp(pos_y + ll_round(GRID_STEP * mObjectMapTPM), 0, img_height - 1);
                for (; cur_y <= end_y; cur_y++)
                {
                    S32 cur_x = llclamp(pos_x, 0, img_width);
                    S32 end_x = llclamp(pos_x + ll_round(GRID_STEP * mObjectMapTPM), 0, img_width - 1);
                    for (; cur_x <= end_x; cur_x++)
                    {
                        U32 texcolor = LLColor4U(255, 128, 128, 192).asRGBA();
                        if (for_sale)
                        {
                            texcolor = LLColor4U(255, 255, 128, 192).asRGBA();
                        }
                        else if (auction)
                        {
                            texcolor = LLColor4U(128, 0, 255, 102).asRGBA();
                        }

                        texture_data[cur_y * img_width + cur_x] = texcolor;
                    }
                }
            }
            if (overlay & PARCEL_SOUTH_LINE)
            {
                if ( (pos_y >= 0) && (pos_y < img_height) )
                {
                    S32 cur_x = llclamp(pos_x, 0, img_width);
                    S32 end_x = llclamp(pos_x + ll_round(GRID_STEP * mObjectMapTPM), 0, img_width - 1);
                    for (; cur_x <= end_x; cur_x++)
                        texture_data[pos_y * img_width + cur_x] = overlay_color.asRGBA();
                }
            }
            if (overlay & PARCEL_WEST_LINE)
            {
                if ( (pos_x >= 0) && (pos_x < img_width) )
                {
                    S32 cur_y = llclamp(pos_y, 0, img_height);
                    S32 end_y = llclamp(pos_y + ll_round(GRID_STEP * mObjectMapTPM), 0, img_height - 1);
                    for (; cur_y <= end_y; cur_y++)
                        texture_data[cur_y * img_width + pos_x] = overlay_color.asRGBA();
                }
            }
        }
    }
}

bool LLNetMap::createImage(LLPointer<LLImageRaw>& rawimagep) const
{
    // Find the size of the side of a square that surrounds the circle that surrounds getRect().
    // ... which is, the diagonal of the rect.
    F32 width = (F32)getRect().getWidth();
    F32 height = (F32)getRect().getHeight();
    S32 square_size = ll_round( sqrt(width*width + height*height) );

    // Find the least power of two >= the minimum size.
    const S32 MIN_SIZE = 64;
    const S32 MAX_SIZE = 512;
    S32 img_size = MIN_SIZE;
    while( (img_size*2 < square_size ) && (img_size < MAX_SIZE) )
    {
        img_size <<= 1;
    }

    if( rawimagep.isNull() ||
        (rawimagep->getWidth() != img_size) ||
        (rawimagep->getHeight() != img_size) )
    {
        rawimagep = new LLImageRaw(img_size, img_size, 4);
        U8* data = rawimagep->getData();
        memset(data, 0, img_size * img_size * 4);
        return true;
    }
    return false;
}

void LLNetMap::createObjectImage()
{
    if (createImage(mObjectRawImagep))
        mObjectImagep = LLViewerTextureManager::getLocalTexture( mObjectRawImagep.get(), false);
    setScale(mScale);
    mUpdateObjectImage = true;
}

void LLNetMap::createParcelImage()
{
    if (createImage(mParcelRawImagep))
        mParcelImagep = LLViewerTextureManager::getLocalTexture( mParcelRawImagep.get(), false);
    mUpdateParcelImage = true;
}

// <GMDX> Show Whole Region, zoomed in: a plain drag pans, except in a band along the map's edges,
// which still moves the window.
bool LLNetMap::fitPanAt(S32 x, S32 y) const
{
    constexpr F32 EDGE = 16.f;
    return mFitRegion && !radarOn() && mFitZoom > 1.f && x > mFitMapRect.mLeft + EDGE && x < mFitMapRect.mRight - EDGE
        && y > mFitMapRect.mBottom + EDGE && y < mFitMapRect.mTop - EDGE;
}

bool LLNetMap::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // <GMDX> drawing a zone: a rectangle / circle is dragged out, a polygon's corners clicked
    if (mDrawKind >= 0)
    {
        const LLVector3d p = viewPosToGlobal(x, y);
        mDrawCursor = p;
        if (mDrawKind == GMDXMapZone::POLYGON)
        {
            if (mDrawPoints.size() >= 3)
            {
                const LLVector3 first = globalPosToView(mDrawPoints.front());
                if (fabsf(first.mV[VX] - (F32)x) < 8.f && fabsf(first.mV[VY] - (F32)y) < 8.f) { finishZone(); return true; }
            }
            mDrawPoints.push_back(p);
            return true;
        }
        mDrawPoints.assign(1, p);
        mDrawDrag = true;
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    // </GMDX>
    if (mFitRegion ? !fitPanAt(x, y) : !(mask & MASK_SHIFT)) return false; // <GMDX>

    // Start panning
    gFocusMgr.setMouseCapture(this);

    mStartPan     = mCurPan;
    mMouseDown.mX = x;
    mMouseDown.mY = y;
    return true;
}

bool LLNetMap::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // <GMDX> a dragged rectangle / circle ends here (a click without a drag draws nothing)
    if (mDrawKind >= 0)
    {
        if (mDrawDrag)
        {
            mDrawDrag = false;
            if (hasMouseCapture()) gFocusMgr.setMouseCapture(NULL);
            const LLVector3 a = globalPosToView(mDrawPoints.front());
            if (fabsf(a.mV[VX] - (F32)x) + fabsf(a.mV[VY] - (F32)y) >= 4.f)
            {
                mDrawPoints.push_back(viewPosToGlobal(x, y));
                finishZone();
            }
            else
            {
                mDrawPoints.clear();
            }
        }
        return true;
    }
    // </GMDX>
    if (abs(mMouseDown.mX - x) < 3 && abs(mMouseDown.mY - y) < 3)
    {
        handleClick(x, y, mask);
    }

    if (hasMouseCapture())
    {
        if (mPanning)
        {
            // restore mouse cursor
            S32 local_x, local_y;
            local_x          = mMouseDown.mX + llfloor(mCurPan.mV[VX] - mStartPan.mV[VX]);
            local_y          = mMouseDown.mY + llfloor(mCurPan.mV[VY] - mStartPan.mV[VY]);
            LLRect clip_rect = getRect();
            clip_rect.stretch(-8);
            clip_rect.clipPointToRect(mMouseDown.mX, mMouseDown.mY, local_x, local_y);
            LLUI::getInstance()->setMousePositionLocal(this, local_x, local_y);

            // finish the pan
            mPanning = false;

            mMouseDown.set(0, 0);
        }
        gViewerWindow->showCursor();
        gFocusMgr.setMouseCapture(NULL);
        return true;
    }

    return false;
}

bool LLNetMap::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    // <GMDX> drawing a zone: right-click finishes a polygon (3+ corners) or cancels
    if (mDrawKind >= 0)
    {
        if (mDrawKind == GMDXMapZone::POLYGON && mDrawPoints.size() >= 3) finishZone();
        else { mDrawKind = -1; mDrawPoints.clear(); }
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(NULL);
        mDrawDrag = false;
        return true;
    }
    // </GMDX>
    auto menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (menu)
    {
        mPopupWorldPos = viewPosToGlobal(x, y);
        updateCamToAvatarMenu(menu, x, y);   // (mAvatarsAtLastRightClick)
        // <GMDX> on an avatar's dot: its profile and dot colour; on a mark: its items, and its own
        // position for the copies
        const bool on_avatar = !mAvatarsAtLastRightClick.empty();
        menu->setItemVisible("Avatar Profile", on_avatar);
        menu->setItemVisible("Avatar Dot Colour", on_avatar);
        menu->setItemVisible("Avatar Separator", on_avatar);
        // (a shared mark or ping before my zones; a shared zone only where nothing else is)
        mPopupMark = on_avatar ? LLUUID::null : markAt(x, y);
        mPopupShared = (on_avatar || mPopupMark.notNull()) ? LLUUID::null : sharedAt(x, y, false);
        mPopupZone = (on_avatar || mPopupMark.notNull() || mPopupShared.notNull()) ? LLUUID::null : zoneAt(x, y);
        if (!on_avatar && mPopupMark.isNull() && mPopupZone.isNull() && mPopupShared.isNull()) mPopupShared = sharedAt(x, y, true);
        const bool on_mark = mPopupMark.notNull(), on_zone = mPopupZone.notNull(), on_shared = mPopupShared.notNull();
        const GMDXSharedItem* shared = GMDXMapShare::find(mPopupShared);
        // (in someone's zone, which can cover the map: still a spot to mark, draw on or ping)
        const bool free_spot = !on_mark && !on_zone && (!on_shared || (shared && shared->type == GMDXShareText::Type::Zone));
        for (const char* item : { "Track Mark", "Rename Mark", "Mark Colour", "Remove Mark" }) menu->setItemVisible(item, on_mark);
        for (const char* item : { "Zone Line", "Zone Fill", "Zone Colour", "Rename Zone", "Remove Zone" }) menu->setItemVisible(item, on_zone);
        menu->setItemVisible("Add Mark Here", free_spot);
        menu->setItemVisible("Draw Zone", free_spot);
        // sharing: my mark / zone to someone; a shared item kept / removed; a ping on a free spot
        menu->setItemVisible("Share With", on_mark || on_zone);
        for (const char* item : { "Keep Shared", "Remove Shared", "Remove Shared Sender" }) menu->setItemVisible(item, on_shared);
        menu->setItemVisible("Block Shared Sender", shared && shared->sender != gAgentID);   // (not my own pings)
        if (shared)
        {
            std::string sender = GMDXMapShare::senderName(*shared);
            LLStringUtil::replaceChar(sender, '[', '(');
            LLStringUtil::replaceChar(sender, ']', ')');
            menu->setItemLabel("Remove Shared Sender", "Remove All From " + sender);
            menu->setItemLabel("Block Shared Sender", "Block Map Shares From " + sender);
        }
        menu->setItemVisible("Ping", free_spot);
        GMDXMapShare::fillShareMenus(menu);
        // </GMDX>
        menu->buildDrawLabels();
        menu->updateParent(LLMenuGL::sMenuContainer);
        // <GMDX> items that do not apply are hidden: zoom in Whole Region, the compass style in
        // Follow Me, each view's own rotation items (submenu branches have no on_visible in XUI)
        if (LLMenuGL* view = menu->findChild<LLMenuGL>("View"))
        {
            view->setItemVisible("Zoom", !mFitRegion);
            // (Show Whole Region is the mini-map floater's: not offered on the People floater's map)
            const bool in_floater = getParentByType<LLFloaterMap>() != nullptr;
            view->setItemVisible("View Follow Me", in_floater);
            view->setItemVisible("View Whole Region", in_floater);
        }
        menu->setItemVisible("Compass Style", mFitRegion);
        if (LLMenuGL* compass = menu->findChildMenuByName("Compass Style", true))
        {
            compass->setItemVisible("Compass Heading Tape", !rotateMap());   // (north-up only)
        }
        if (LLMenuGL* rotation = menu->findChildMenuByName("Rotation", true))
        {
            rotation->setItemVisible("Camera at top", !mFitRegion);
            for (const char* item : { "Turn round", "Turn square", "Turn whole" }) rotation->setItemVisible(item, mFitRegion);
        }
        // </GMDX>
        LLMenuGL::showPopup(this, menu, x, y);
    }
    return true;
}

void LLNetMap::updateCamToAvatarMenu(LLMenuGL* menu, S32 x, S32 y)
{
    // Avatars under the cursor, picked the same way as the hover in draw()
    uuid_vec_t avatar_ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&avatar_ids, &positions, gAgentCamera.getCameraPositionGlobal());

    const F32 pick_radius = llmax(mDotRadius * MIN_PICK_SCALE, MIN_PICK_PX);   // <GMDX>
    const F32 min_pick_dist_squared = pick_radius * pick_radius;
    std::vector<std::pair<F32, LLUUID>> picked;
    for (size_t i = 0; i < avatar_ids.size(); ++i)
    {
        if (avatar_ids[i] == gAgent.getID())
            continue;
        // <GMDX> Show Whole Region: only the avatars it draws
        if (mFitRegion && gAgent.getRegion() && !gAgent.getRegion()->pointInRegionGlobal(positions[i]))
            continue;
        if (gmdx_hidden_above(positions[i]))
            continue;
        // </GMDX>

        LLVector3 pos_map = globalPosToView(positions[i]);
        // <GMDX> where draw() puts the dot (the radar's rim for one past it)
        LLVector2 dot(pos_map.mV[VX], pos_map.mV[VY]);
        if (radarOn()) dot = toRadarRim(dot);
        F32 dist_to_cursor_squared = dist_vec_squared(dot, LLVector2((F32)x, (F32)y));
        // </GMDX>
        if (dist_to_cursor_squared < min_pick_dist_squared)
        {
            picked.emplace_back(dist_to_cursor_squared, avatar_ids[i]);
        }
    }
    std::sort(picked.begin(), picked.end(),
              [](const std::pair<F32, LLUUID>& a, const std::pair<F32, LLUUID>& b) { return a.first < b.first; });

    mAvatarsAtLastRightClick.clear();
    for (const auto& entry : picked)
    {
        mAvatarsAtLastRightClick.push_back(entry.second);
    }

    // One avatar: a plain item. Several (a cluster of dots): a submenu listing them.
    const size_t count = mAvatarsAtLastRightClick.size();
    menu->setItemVisible("Cam To Avatar", count == 1);
    menu->setItemVisible("Cam To Avatars", count > 1);
    menu->setItemVisible("Cam To Avatar Separator", count > 0);
    if (count == 1)
    {
        menu->setItemEnabled("Cam To Avatar", SLAvatarActions::canCamToAvatar(mAvatarsAtLastRightClick.front()));
    }

    LLMenuGL* avatars_menu = menu->findChildMenuByName("Cam To Avatars", true);
    if (!avatars_menu)
        return;

    avatars_menu->empty();
    if (count <= 1)
        return;

    S32 index = 0;
    for (const LLUUID& avatar_id : mAvatarsAtLastRightClick)
    {
        std::string label;
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(avatar_id, &av_name))
        {
// [RLVa:KB] - Checked: RLVa-1.2.0
            label = RlvActions::canShowName(RlvActions::SNC_DEFAULT, avatar_id) ? av_name.getCompleteName() : RlvStrings::getAnonym(av_name);
// [/RLVa:KB]
        }
        else
        {
            label = LLTrans::getString("AvatarNameWaiting");
        }

        LLMenuItemCallGL::Params p;
        p.name = llformat("cam_to_avatar_%d", index++);
        p.label = label;
        p.enabled = SLAvatarActions::canCamToAvatar(avatar_id);
        p.on_click.function([avatar_id](LLUICtrl*, const LLSD&) { SLAvatarActions::camToAvatar(avatar_id); });
        avatars_menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
    }
}

// <GMDX>
void LLNetMap::setAvatarDotColor(const std::string& which)
{
    if (mAvatarsAtLastRightClick.empty()) return;
    const LLUUID id = mAvatarsAtLastRightClick.front();
    if (which == "clear")
    {
        GMDXMinimapMarks::clearAvatarColor(id);
        return;
    }
    if (which != "custom")
    {
        GMDXMinimapMarks::setAvatarColor(id, GMDXMinimapMarks::color(llclamp((S32)std::stoi(which), 0, GMDXMinimapMarks::COLORS - 1)));
        return;
    }
    if (!mDotColorSwatch)
    {
        LLColorSwatchCtrl::Params p(LLUICtrlFactory::getDefaultParams<LLColorSwatchCtrl>());
        p.name = "dot_color_swatch";
        p.rect = LLRect(0, 1, 1, 0);
        p.visible = false;
        p.can_apply_immediately = true;   // the dot follows the picker live
        mDotColorSwatch = LLUICtrlFactory::create<LLColorSwatchCtrl>(p);
        addChild(mDotColorSwatch);
        mDotColorSwatch->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
            { GMDXMinimapMarks::setAvatarColor(mDotColorAvatar, LLColor4(ctrl->getValue())); });
        // Cancel: the dot goes back to what it was (no colour of its own, if it had none)
        mDotColorSwatch->setOnCancelCallback([this](LLUICtrl*, const LLSD&)
            {
                if (mDotColorHad) GMDXMinimapMarks::setAvatarColor(mDotColorAvatar, mDotColorBefore);
                else GMDXMinimapMarks::clearAvatarColor(mDotColorAvatar);
            });
    }
    mDotColorAvatar = id;
    mDotColorHad = GMDXMinimapMarks::avatarColor(id, mDotColorBefore);
    static LLUIColor map_avatar_color = LLUIColorTable::instance().getColor("MapAvatarColor", LLColor4::white);
    LLColor4 current = LLGroupColorMap::instance().tint(id, LGGContactSets::getInstance()->getAvatarColor(id, map_avatar_color, ContactSetType::MINIMAP));
    GMDXMinimapMarks::avatarColor(id, current);
    mDotColorSwatch->set(current, true);
    mDotColorSwatch->setOriginal(current);
    mDotColorSwatch->showPicker(true);
}
// </GMDX>

void LLNetMap::handleCamToAvatar(const LLSD& userdata)
{
    if (!mAvatarsAtLastRightClick.empty())
    {
        SLAvatarActions::camToAvatar(mAvatarsAtLastRightClick.front());
    }
}

bool LLNetMap::handleClick(S32 x, S32 y, MASK mask)
{
    // TODO: allow clicking an avatar on minimap to select avatar in the nearby avatar list
    // if(mClosestAgentToCursor.notNull())
    //     mNearbyList->selectUser(mClosestAgentToCursor);
    // Needs a registered observer i guess to accomplish this without using
    // globals to tell the mNearbyList in llpeoplepanel to select the user
    return true;
}

bool LLNetMap::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // <GMDX> drawing a polygon: a double-click puts the last corner and finishes
    if (mDrawKind >= 0)
    {
        if (mDrawKind == GMDXMapZone::POLYGON)
        {
            const LLVector3 last = mDrawPoints.empty() ? LLVector3(-1000.f, -1000.f, 0.f) : globalPosToView(mDrawPoints.back());
            if (fabsf(last.mV[VX] - (F32)x) + fabsf(last.mV[VY] - (F32)y) >= 4.f)   // (the click before it put a corner there)
            {
                mDrawPoints.push_back(viewPosToGlobal(x, y));
            }
            if (mDrawPoints.size() >= 3) finishZone();
        }
        return true;
    }
    // </GMDX>
    LLVector3d pos_global = viewPosToGlobal(x, y);

    bool double_click_teleport = gSavedSettings.getBOOL("DoubleClickTeleport");
    bool double_click_show_world_map = gSavedSettings.getBOOL("DoubleClickShowWorldMap");

    if (double_click_teleport || double_click_show_world_map)
    {
        // If we're not tracking a beacon already, double-click will set one
        if (!LLTracker::isTracking(NULL))
        {
            LLFloaterWorldMap* world_map = LLFloaterWorldMap::getInstance();
            if (world_map)
            {
                world_map->trackLocation(pos_global);
            }
        }
    }

    if (double_click_teleport)
    {
        // If DoubleClickTeleport is on, double clicking the minimap will teleport there
        gAgent.teleportViaLocationLookAt(pos_global);
    }
    else if (double_click_show_world_map)
    {
        LLFloaterReg::showInstance("world_map");
    }
    return true;
}

F32 LLNetMap::getScaleForName(std::string scale_name)
{
    if (scale_name == "very close")
    {
        return LLNetMap::MAP_SCALE_VERY_CLOSE;
    }
    else if (scale_name == "close")
    {
        return LLNetMap::MAP_SCALE_CLOSE;
    }
    else if (scale_name == "medium")
    {
        return LLNetMap::MAP_SCALE_MEDIUM;
    }
    else if (scale_name == "far")
    {
        return LLNetMap::MAP_SCALE_FAR;
    }
    return 0.0f;
}

// static
bool LLNetMap::outsideSlop( S32 x, S32 y, S32 start_x, S32 start_y, S32 slop )
{
    S32 dx = x - start_x;
    S32 dy = y - start_y;

    return (dx <= -slop || slop <= dx || dy <= -slop || slop <= dy);
}

bool LLNetMap::handleHover( S32 x, S32 y, MASK mask )
{
    // <GMDX> the coordinates under the cursor
    mCursorInside = true;
    mCursorX = x;
    mCursorY = y;
    if (mDrawKind >= 0)
    {
        mDrawCursor = viewPosToGlobal(x, y);
        gViewerWindow->setCursor(UI_CURSOR_CROSS);
        return true;
    }
    // </GMDX>
    if (hasMouseCapture())
    {
        if (mPanning || outsideSlop(x, y, mMouseDown.mX, mMouseDown.mY, MOUSE_DRAG_SLOP))
        {
            if (!mPanning)
            {
                // Just started panning. Hide cursor.
                mPanning = true;
                gViewerWindow->hideCursor();
            }

            LLVector2 delta(static_cast<F32>(gViewerWindow->getCurrentMouseDX()),
                            static_cast<F32>(gViewerWindow->getCurrentMouseDY()));

            // Set pan to value at start of drag + offset
            if (mFitRegion)   // <GMDX> Show Whole Region, zoomed in: the view moves over the region
            {
                const F32 ppm = mFitBasePixelsPerMeter * mFitZoom;
                if (ppm > 0.f)
                {
                    mFitViewCenter -= delta / ppm;
                }
                if (LLViewerRegion* region = gAgent.getRegion())
                {
                    clampFitView(region->getWidth());
                }
            }
            else
            mCurPan += delta;

            gViewerWindow->moveCursorToCenter();
        }
    }

    if (mFitRegion ? fitPanAt(x, y) : (mask & MASK_SHIFT)) // <GMDX>
    {
        // If shift is held, change the cursor to hint that the map can be
        // dragged. However, holding shift is not required to drag the map.
        gViewerWindow->setCursor( UI_CURSOR_TOOLPAN );
    }
    else
    {
        gViewerWindow->setCursor( UI_CURSOR_CROSS );
    }

    return true;
}

bool LLNetMap::isZoomChecked(const LLSD &userdata)
{
    std::string level = userdata.asString();
    F32         scale = getScaleForName(level);
    return scale == mScale;
}

void LLNetMap::setZoom(const LLSD &userdata)
{
    std::string level = userdata.asString();
    F32         scale = getScaleForName(level);
    if (scale != 0.0f)
    {
        setScale(scale);
    }
}

void LLNetMap::handleStopTracking (const LLSD& userdata)
{
    auto menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (menu)
    {
        menu->setItemEnabled ("Stop Tracking", false);
        LLTracker::stopTracking (LLTracker::isTracking(NULL));
    }
}

void LLNetMap::activateCenterMap(const LLSD &userdata) { mCentering = true; }

bool LLNetMap::isMapOrientationChecked(const LLSD &userdata)
{
    const std::string command_name = userdata.asString();
    const bool        rotate_map   = gSavedSettings.getBOOL("MiniMapRotate");
    if (command_name == "north_at_top")
    {
        return !rotate_map;
    }

    // <GMDX> Whole Region: how it turns (GMDXMiniMapRotateView)
    const S32 view = gSavedSettings.getS32("GMDXMiniMapRotateView");
    if (command_name == "rotate_round") return rotate_map && view == 0;
    if (command_name == "rotate_square") return rotate_map && view == 1;
    if (command_name == "rotate_whole") return rotate_map && view == 2;
    // </GMDX>

    return false;
}

void LLNetMap::setMapOrientation(const LLSD &userdata)
{
    const std::string command_name = userdata.asString();
    if (command_name == "north_at_top")
    {
        gSavedSettings.setBOOL("MiniMapRotate", false);
    }
    else if (command_name == "camera_at_top")
    {
        gSavedSettings.setBOOL("MiniMapRotate", true);
    }
    // <GMDX> Whole Region: how it turns
    else if (command_name == "rotate_round" || command_name == "rotate_square" || command_name == "rotate_whole")
    {
        gSavedSettings.setBOOL("MiniMapRotate", true);
        gSavedSettings.setS32("GMDXMiniMapRotateView", command_name == "rotate_round" ? 0 : command_name == "rotate_square" ? 1 : 2);
    }
    // </GMDX>
}

void LLNetMap::popupShowAboutLand(const LLSD &userdata)
{
    // Update parcel selection. It's important to deselect land first so the "About Land" floater doesn't refresh with the old selection.
    LLViewerParcelMgr::getInstance()->deselectLand();
    LLParcelSelectionHandle selection = LLViewerParcelMgr::getInstance()->selectParcelAt(mPopupWorldPos);
    gMenuHolder->setParcelSelection(selection);

    LLFloaterReg::showInstance("about_land", LLSD(), false);
}
