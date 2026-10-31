/**
 * @file gmdxfloaterminimapsettings.cpp
 * @brief GunMetal-DX mini-map settings window (see gmdxfloaterminimapsettings.h).
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

// <GMDX> whole file: see gmdxfloaterminimapsettings.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxfloaterminimapsettings.h"

#include "gmdxmapshare.h"
#include "llminimapcompass.h"
#include "llminimapimage.h"
#include "llregionmapcapture.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloatermap.h"
#include "llfloaterreg.h"
#include "llnetmap.h"
#include "llscrolllistitem.h"
#include "llviewercontrol.h"

namespace
{
// A list's value, set only when it differs (an open list keeps its highlight); none of its items:
// the label says so.
void show_value(LLComboBox* combo, const std::string& value, const std::string& none_label)
{
    if (combo->findItemByValue(value))
    {
        if (combo->getValue().asString() != value) combo->setValue(value);
        combo->updateLabel();
    }
    else
    {
        combo->setLabel(none_label);
    }
}
} // namespace

GMDXFloaterMinimapSettings::GMDXFloaterMinimapSettings(const LLSD& key)
:   LLFloater(key)
{
}

// static
void GMDXFloaterMinimapSettings::showFor(LLNetMap* map)
{
    if (GMDXFloaterMinimapSettings* floater = LLFloaterReg::showTypedInstance<GMDXFloaterMinimapSettings>("gmdx_minimap_settings"))
    {
        floater->mMap = map ? map->getHandle() : LLHandle<LLView>();
    }
}

LLNetMap* GMDXFloaterMinimapSettings::map() const
{
    if (LLView* view = mMap.get()) return static_cast<LLNetMap*>(view);
    LLFloaterMap* mini_map = LLFloaterReg::findTypedInstance<LLFloaterMap>("mini_map");
    return mini_map ? mini_map->findChild<LLNetMap>("Net Map") : nullptr;
}

bool GMDXFloaterMinimapSettings::postBuild()
{
    mView = getChild<LLComboBox>("view_mode");
    mZoom = getChild<LLComboBox>("zoom");
    mRotation = getChild<LLComboBox>("rotation");
    mCompass = getChild<LLComboBox>("compass");
    mBackground = getChild<LLComboBox>("background");
    mOpacity = getChild<LLComboBox>("opacity");
    mUnblock = getChild<LLButton>("unblock_shares");

    // View: Follow Me / Whole Region (SLMiniMapWholeRegion), the zoom, re-center
    mView->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        { gSavedSettings.setBOOL("SLMiniMapWholeRegion", ctrl->getValue().asString() == "region"); });
    mZoom->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        { if (LLNetMap* m = map()) m->setZoom(ctrl->getValue()); });
    getChild<LLButton>("recenter")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        { if (LLNetMap* m = map()) m->activateCenterMap(LLSD()); });
    // Rotation (MiniMapRotate, SLMiniMapRotateView) and the compass: Whole Region's
    // (SLMiniMapCompassStyle), or the turning map's own (SLMiniMapRotateCompass)
    mRotation->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        { if (LLNetMap* m = map()) m->setMapOrientation(ctrl->getValue()); });
    mCompass->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
        {
            LLNetMap* m = map();
            gSavedSettings.setS32(m && m->rotateMap() ? "SLMiniMapRotateCompass" : "SLMiniMapCompassStyle", ctrl->getValue().asInteger());
        });
    // Background: painted terrain / Region Terrain Map (SLMiniMapTerrainLook) / world map tile (SLMiniMapUseMapTile)
    mBackground->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            const std::string which = ctrl->getValue().asString();
            gSavedSettings.setBOOL("SLMiniMapUseMapTile", which == "tile");
            if (which != "tile")
            {
                gSavedSettings.setBOOL("SLMiniMapTerrainLook", which == "terrain");
            }
        });
    mOpacity->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        { gSavedSettings.setF32("SLMiniMapOpacity", (F32)ctrl->getValue().asReal()); });

    // Show: others' shares (gmdxmapshare.h)
    getChild<LLButton>("clear_shared")->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXMapShare::clear(); });
    mUnblock->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXMapShare::unblockAll(); });

    // Region Map (llregionmapcapture.h, llminimapimage.h): for the region I am in
    getChild<LLButton>("capture")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::openWindow(); });
    getChild<LLButton>("cancel_capture")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::cancelCapture(); });
    getChild<LLButton>("copy_image")->setCommitCallback([](LLUICtrl*, const LLSD&) { minimap_copy_region_map_image(); });
    getChild<LLButton>("save_image")->setCommitCallback([](LLUICtrl*, const LLSD&) { minimap_save_region_map_image(); });
    getChild<LLButton>("export_map")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::exportMap(gAgent.getRegion()); });
    getChild<LLButton>("upload_map")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::uploadMap(gAgent.getRegion()); });
    getChild<LLButton>("import_map")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::importMap(); });
    getChild<LLButton>("delete_map")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::deleteCapture(gAgent.getRegion()); });
    getChild<LLButton>("open_folder")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLRegionMapCapture::openMapsFolder(); });

    updateControls();
    return true;
}

void GMDXFloaterMinimapSettings::draw()
{
    updateControls();
    LLFloater::draw();
}

void GMDXFloaterMinimapSettings::fillRotation(bool whole)
{
    mRotation->removeall();
    mRotation->add("North at top", LLSD("north_at_top"));
    if (whole)
    {
        mRotation->add("Turn around me (round)", LLSD("rotate_round"));
        mRotation->add("Turn around me (square)", LLSD("rotate_square"));
        mRotation->add("Turn the whole region", LLSD("rotate_whole"));
    }
    else
    {
        mRotation->add("Camera at top", LLSD("camera_at_top"));
    }
    mRotationFor = whole ? 1 : 0;
}

void GMDXFloaterMinimapSettings::updateControls()
{
    LLNetMap* m = map();
    // (the People floater's map always follows: Whole Region is the mini-map floater's)
    const bool in_floater = m && m->getParentByType<LLFloaterMap>();
    const bool whole = in_floater && gSavedSettings.getBOOL("SLMiniMapWholeRegion");

    // View
    mView->setEnabled(in_floater);
    show_value(mView, whole ? "region" : "follow", "");
    mZoom->setEnabled(m && !m->mFitRegion);
    std::string zoom;
    if (m)
    {
        for (const char* level : { "very close", "close", "medium", "far" })
        {
            if (m->isZoomChecked(LLSD(level))) zoom = level;
        }
    }
    show_value(mZoom, zoom, "Custom");
    getChild<LLUICtrl>("auto_center")->setEnabled(!whole);
    getChild<LLUICtrl>("recenter")->setEnabled(m && !whole && m->mCanRecenter);

    // Rotation & compass: each view's own ways to turn; the compass is Whole Region's (the
    // heading tape only north-up)
    if (mRotationFor != (whole ? 1 : 0)) fillRotation(whole);
    mRotation->setEnabled(m != nullptr);
    std::string rotation;
    if (m)
    {
        for (const char* way : { "north_at_top", "camera_at_top", "rotate_round", "rotate_square", "rotate_whole" })
        {
            if (!rotation.empty() || !mRotation->findItemByValue(way)) continue;
            if (m->isMapOrientationChecked(LLSD(way))) rotation = way;
        }
    }
    show_value(mRotation, rotation, "");
    const bool rotating = m && m->rotateMap();
    mCompass->setEnabled(m && m->mFitRegion);
    mCompass->setEnabledByValue(LLSD("2"), !rotating);
    show_value(mCompass, llformat("%d", rotating ? LLMinimapCompass::getRotatingStyle() : LLMinimapCompass::getStyle()), "");

    // Background & opacity
    const bool tile = gSavedSettings.getBOOL("SLMiniMapUseMapTile");
    const bool terrain = gSavedSettings.getBOOL("SLMiniMapTerrainLook");
    show_value(mBackground, tile ? "tile" : terrain ? "terrain" : "painted", "");
    const F32 opacity = gSavedSettings.getF32("SLMiniMapOpacity");
    std::string opacity_value;
    for (const char* value : { "1.0", "0.75", "0.5", "0.25" })
    {
        if (fabsf(opacity - (F32)atof(value)) < 0.01f) opacity_value = value;
    }
    show_value(mOpacity, opacity_value, llformat("%d%%", ll_round(opacity * 100.f)));

    // Show: the blocked senders' count
    const S32 blocked = gSavedPerAccountSettings.getLLSD("SLMapShareBlocked").size();
    if (blocked != mBlocked)
    {
        mBlocked = blocked;
        mUnblock->setLabel(blocked ? llformat("Unblock All (%d)", blocked) : std::string("Unblock All"));
        mUnblock->setEnabled(blocked > 0);
    }

    // Region Map
    const bool running = LLRegionMapCapture::isRunning();
    const bool has_map = LLRegionMapCapture::hasCapture(gAgent.getRegion()) && !running;
    getChild<LLUICtrl>("capture")->setEnabled(LLRegionMapCapture::canCapture());
    getChild<LLUICtrl>("cancel_capture")->setEnabled(running);
    for (const char* name : { "export_map", "upload_map", "delete_map" }) getChild<LLUICtrl>(name)->setEnabled(has_map);
}
