/**
 * @file llviewerdisplay.cpp
 * @brief LLViewerDisplay class implementation
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "llviewerdisplay.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcoord.h"
#include "llcriticaldamp.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llfeaturemanager.h"
#include "llfloaterprogressview.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "llgltfmateriallist.h"
#include "llhudmanager.h"
#include "llmemory.h"
#include "llparcel.h"
#include "llperfstats.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "llsky.h"
#include "llstartup.h"
#include "llstartup.h"
#include "lltooldraganddrop.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "lltracker.h"
#include "lltrans.h"
#include "llui.h"
#include "lluuid.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvograss.h"
#include "llworld.h"
#include "pipeline.h"

#include <filesystem>
#include <iomanip>
#include <sstream>
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "llvisualeffect.h"
#include "rlvactions.h"
#include "rlvlocks.h"
// [/RLVa:KB]

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

extern LLPointer<LLViewerTexture> gStartTexture;
extern bool gShiftFrame;

// display()/display_startup() present through the Vulkan UI path (llappviewer.cpp).
void displayVulkanUI();

LLPointer<LLViewerTexture> gDisconnectedImagep = nullptr;

// used to toggle renderer back on after teleport
bool         gTeleportDisplay = false;
LLFrameTimer gTeleportDisplayTimer;
LLFrameTimer gTeleportArrivalTimer;
constexpr F32 RESTORE_GL_TIME = 5.f;  // Wait this long while reloading textures before we raise the curtain

bool gForceRenderLandFence = false;
bool gDisplaySwapBuffers = false;
bool gDepthDirty = false;
bool gResizeScreenTexture = false;
bool gResizeShadowTexture = false;
bool gWindowResized = false;
bool gSnapshot = false;
bool gSnapshotNoPost = false;
bool gShaderProfileFrame = false;

// This is how long the sim will try to teleport you before giving up.
constexpr F32 TELEPORT_EXPIRY = 15.0f;
// Additional time (in seconds) to wait per attachment
constexpr F32 TELEPORT_EXPIRY_PER_ATTACHMENT = 3.f;

U32 gRecentFrameCount = 0; // number of 'recent' frames
LLFrameTimer gRecentFPSTime;
LLFrameTimer gRecentMemoryTime;
LLFrameTimer gAssetStorageLogTime;

// Rendering stuff


void display_startup()
{
    if (   !gViewerWindow
        || !gViewerWindow->getActive()
        || !gViewerWindow->getWindow()->getVisible()
        || gViewerWindow->getWindow()->getMinimized()
        || gNonInteractive)
    {
        return;
    }
    // Startup progress / login frames go through the VK UI present path.
    displayVulkanUI();
}

void display_update_camera()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Update Camera");
    // TODO: cut draw distance down if customizing avatar?
    // TODO: cut draw distance on per-parcel basis?

    // Cut draw distance in half when customizing avatar,
    // but on the viewer only.
    F32 final_far = gAgentCamera.mDrawDistance;
    if (CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode())
    {
        final_far *= 0.5f;
    }
    else if (LLViewerTexture::sDesiredDiscardBias > 2.f)
    {
        final_far = llmax(32.f, final_far / (LLViewerTexture::sDesiredDiscardBias - 1.f));
    }
    LLViewerCamera::getInstance()->setFar(final_far);
    LLVOAvatar::sRenderDistance = llclamp(final_far, 16.f, 256.f);
    gViewerWindow->setup3DRender();

    // Update land visibility too
    LLWorld::getInstance()->setLandFarClip(final_far);
}

// Write some stats to LL_INFOS()
void display_stats()
{
    LL_PROFILE_ZONE_SCOPED;
    constexpr F32 FPS_LOG_FREQUENCY = 10.f;
    if (gRecentFPSTime.getElapsedTimeF32() >= FPS_LOG_FREQUENCY)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - FPS");
        F32 fps = gRecentFrameCount / FPS_LOG_FREQUENCY;
        LL_INFOS() << llformat("FPS: %.02f", fps) << LL_ENDL;
        gRecentFrameCount = 0;
        gRecentFPSTime.reset();
    }
    static LLCachedControl<F32> mem_log_freq(gSavedSettings, "MemoryLogFrequency", 600.f);
    if (mem_log_freq > 0.f && gRecentMemoryTime.getElapsedTimeF32() >= mem_log_freq)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - Memory");
        gMemoryAllocated = U64Bytes(LLMemory::getCurrentRSS());
        U32Megabytes memory = gMemoryAllocated;
        LL_INFOS() << "MEMORY: " << memory << LL_ENDL;
        LLMemory::logMemoryInfo(true) ;
        gRecentMemoryTime.reset();
    }
    constexpr F32 ASSET_STORAGE_LOG_FREQUENCY = 60.f;
    if (gAssetStorageLogTime.getElapsedTimeF32() >= ASSET_STORAGE_LOG_FREQUENCY)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - Asset Storage");
        gAssetStorageLogTime.reset();
        gAssetStorage->logAssetStorageInfo();
    }
}

static void update_tp_display(bool minimized)
{
    static LLCachedControl<F32> teleport_arrival_delay(gSavedSettings, "TeleportArrivalDelay");
    static LLCachedControl<F32> teleport_local_delay(gSavedSettings, "TeleportLocalDelay");

    S32 attach_count = 0;
    if (isAgentAvatarValid())
    {
        attach_count = gAgentAvatarp->getAttachmentCount();
    }
    F32 teleport_save_time = TELEPORT_EXPIRY + TELEPORT_EXPIRY_PER_ATTACHMENT * attach_count;
    F32 teleport_elapsed = gTeleportDisplayTimer.getElapsedTimeF32();
    F32 teleport_percent = teleport_elapsed * (100.f / teleport_save_time);
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_START && teleport_percent > 100.f)
    {
        // Give up.  Don't keep the UI locked forever.
        LL_WARNS("Teleport") << "Giving up on teleport. elapsed time " << teleport_elapsed << " exceeds max time " << teleport_save_time << LL_ENDL;
        gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
        gAgent.setTeleportMessage(std::string());
    }

    LLFloaterProgressView* pProgFloater = LLFloaterReg::getTypedInstance<LLFloaterProgressView>("progress_view");

    // Make sure the TP progress panel gets hidden in case the viewer window
    // is minimized *during* a TP. HB
    if (minimized)
    {
        pProgFloater->setShowProgress(false);
    }

    const std::string& message = gAgent.getTeleportMessage();
    switch (gAgent.getTeleportState())
    {
        case LLAgent::TELEPORT_PENDING:
        {
            gTeleportDisplayTimer.reset();
            const std::string& msg = LLAgent::sTeleportProgressMessages["pending"];
            if (!minimized)
            {
                pProgFloater->setShowProgress(true);
                pProgFloater->setProgressPercent(llmin(teleport_percent, 0.0f));
                pProgFloater->setProgressString(msg);
            }
            gAgent.setTeleportMessage(msg);
            break;
        }

        case LLAgent::TELEPORT_START:
        {
            // Transition to REQUESTED.  Viewer has sent some kind
            // of TeleportRequest to the source simulator
            gTeleportDisplayTimer.reset();
            const std::string& msg = LLAgent::sTeleportProgressMessages["requesting"];
            LL_INFOS("Teleport") << "A teleport request has been sent, setting state to TELEPORT_REQUESTED" << LL_ENDL;
            gAgent.setTeleportState(LLAgent::TELEPORT_REQUESTED);
            gAgent.setTeleportMessage(msg);
            if (!minimized)
            {
                pProgFloater->setShowProgress(true);
                pProgFloater->setProgressPercent(llmin(teleport_percent, 0.0f));
                pProgFloater->setProgressString(msg);
                //pProgFloater->setProgressMessage(gAgent.mMOTD);
            }
            break;
        }

        case LLAgent::TELEPORT_REQUESTED:
            // Waiting for source simulator to respond
            if (!minimized)
            {
                pProgFloater->setProgressPercent(llmin(teleport_percent, 37.5f));
                pProgFloater->setProgressString(message);
            }
            break;

        case LLAgent::TELEPORT_MOVING:
            // Viewer has received destination location from source simulator
            if (!minimized)
            {
                pProgFloater->setProgressPercent(llmin(teleport_percent, 75.f));
                pProgFloater->setProgressString(message);
            }
            break;

        case LLAgent::TELEPORT_START_ARRIVAL:
            // Transition to ARRIVING.  Viewer has received avatar update, etc.,
            // from destination simulator
            gTeleportArrivalTimer.reset();
            LL_INFOS("Teleport") << "Changing state to TELEPORT_ARRIVING" << LL_ENDL;
            gAgent.setTeleportState(LLAgent::TELEPORT_ARRIVING);
            gAgent.setTeleportMessage(LLAgent::sTeleportProgressMessages["arriving"]);
            gAgent.sheduleTeleportIM();
            gTextureList.mForceResetTextureStats = true;
            gAgentCamera.resetView(true, true);
            if (!minimized)
            {
                pProgFloater->setProgressCancelButtonVisible(false, LLTrans::getString("Cancel"));
                pProgFloater->setProgressPercent(75.f);
            }
            break;

        case LLAgent::TELEPORT_ARRIVING:
        // Make the user wait while content "pre-caches"
        {
            F32 arrival_fraction = (gTeleportArrivalTimer.getElapsedTimeF32() / teleport_arrival_delay());
            if (arrival_fraction > 1.f)
            {
                arrival_fraction = 1.f;
                //LLFirstUse::useTeleport();
                LL_INFOS("Teleport") << "arrival_fraction is " << arrival_fraction << " changing state to TELEPORT_NONE" << LL_ENDL;
                gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
            }
            if (!minimized)
            {
                pProgFloater->setProgressCancelButtonVisible(false, LLTrans::getString("Cancel"));
                pProgFloater->setProgressPercent(arrival_fraction * 25.f + 75.f);
                pProgFloater->setProgressString(message);
            }
            break;
        }

        case LLAgent::TELEPORT_LOCAL:
        // Short delay when teleporting in the same sim (progress screen active but not shown - did not
        // fall-through from TELEPORT_START)
        {
            if (gTeleportDisplayTimer.getElapsedTimeF32() > teleport_local_delay())
            {
                //LLFirstUse::useTeleport();
                LL_INFOS("Teleport") << "State is local and gTeleportDisplayTimer " << gTeleportDisplayTimer.getElapsedTimeF32()
                                     << " exceeds teleport_local_delete " << teleport_local_delay
                                     << "; setting state to TELEPORT_NONE"
                                     << LL_ENDL;
                gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
            }
            break;
        }

        case LLAgent::TELEPORT_NONE:
            // No teleport in progress
            pProgFloater->setShowProgress(false);
            gTeleportDisplay = false;
    }
}


// Paint the display!
void display(bool rebuild, F32 zoom_factor, int subfield, bool for_snapshot)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Render");
    LL_PROFILE_GPU_ZONE("Render");
    // Vulkan is the only renderer (GL-B7 removed the GL body). Snapshots take the VK
    // capture path in LLViewerWindow::rawSnapshot and land here too.
    gRecentFrameCount++;   // display_stats' 10 s FPS log line
    display_stats();
    displayVulkanUI();
}

// The whole HUD area (#919: the HUD rect -- LL's world view rect -- not the VK world view,
// which is the whole window). Only the HUD matrices use it.
LLRect get_whole_screen_region()
{
    LLRect whole_screen = gViewerWindow->getHudRectScaled();

    // apply camera zoom transform (for high res screenshots)
    F32 zoom_factor = LLViewerCamera::getInstance()->getZoomFactor();
    S16 sub_region = LLViewerCamera::getInstance()->getZoomSubRegion();
    if (zoom_factor > 1.f)
    {
        S32 num_horizontal_tiles = llceil(zoom_factor);
        S32 tile_width = ll_round((F32)gViewerWindow->getHudRectScaled().getWidth() / zoom_factor);
        S32 tile_height = ll_round((F32)gViewerWindow->getHudRectScaled().getHeight() / zoom_factor);
        int tile_y = sub_region / num_horizontal_tiles;
        int tile_x = sub_region - (tile_y * num_horizontal_tiles);

        whole_screen.setLeftTopAndSize(tile_x * tile_width, gViewerWindow->getHudRectScaled().getHeight() - (tile_y * tile_height), tile_width, tile_height);
    }
    return whole_screen;
}

bool get_hud_matrices(const LLRect& screen_region, glm::mat4 &proj, glm::mat4&model)
{
    if (isAgentAvatarValid() && gAgentAvatarp->hasHUDAttachment())
    {
        F32 zoom_level = gAgentCamera.mHUDCurZoom;
        LLBBox hud_bbox = gAgentAvatarp->getHUDBBox();

        F32 hud_depth = llmax(1.f, hud_bbox.getExtentLocal().mV[VX] * 1.1f);
        // #919: HUD space is 1 unit tall and the HUD rect's aspect wide (LL: the camera
        // aspect, which was the world view's; the VK world camera spans the whole window).
        const F32 aspect_ratio = gViewerWindow->getHudProjectionAspect();
        const LLRect hud_rect = gViewerWindow->getHudRectScaled();
        const F32 hud_w = (F32)llmax(hud_rect.getWidth(), 1);
        const F32 hud_h = (F32)llmax(hud_rect.getHeight(), 1);
        proj = glm::ortho(-0.5f * aspect_ratio, 0.5f * aspect_ratio, -0.5f, 0.5f, 0.f, hud_depth);
        proj[2][2] = -0.01f;
        // Centre the depth range on the HUD camera plane (#413): the zoom scales HUD
        // X (depth) too, so at zoom > 1 prims behind the bbox centre land at negative
        // eye depth. LL's near plane at 0 clipped them (LL capped the zoom at 1x);
        // with no offset the -0.01 scale keeps depths -100..100 in GL's [-1,1].
        proj[3][2] = 0.f;

        F32 scale_x = hud_w / (F32)llmax(screen_region.getWidth(), 1);
        F32 scale_y = hud_h / (F32)llmax(screen_region.getHeight(), 1);

        glm::mat4 mat = glm::identity<glm::mat4>();
        mat = glm::translate(mat,
            glm::vec3(clamp_rescale((F32)(screen_region.getCenterX() - screen_region.mLeft), 0.f, hud_w, 0.5f * scale_x * aspect_ratio, -0.5f * scale_x * aspect_ratio),
                clamp_rescale((F32)(screen_region.getCenterY() - screen_region.mBottom), 0.f, hud_h, 0.5f * scale_y, -0.5f * scale_y),
                0.f));
        mat = glm::scale(mat, glm::vec3(scale_x, scale_y, 1.f));
        proj *= mat;

        glm::mat4 tmp_model = glm::make_mat4(OGL_TO_CFR_ROTATION);
        mat = glm::identity<glm::mat4>();
        mat = glm::translate(mat, glm::vec3(-hud_bbox.getCenterLocal().mV[VX] + (hud_depth * 0.5f), 0.f, 0.f));
        mat = glm::scale(mat, glm::vec3(zoom_level));
        // HUD edit pan (#413): centre the zoomed view on mHUDCurPan (HUD-local).
        const LLVector3& pan = gAgentCamera.mHUDCurPan;
        mat = glm::translate(mat, glm::vec3(-pan.mV[VX], -pan.mV[VY], -pan.mV[VZ]));
        tmp_model *= mat;
        model = tmp_model;

        return true;
    }
    else
    {
        return false;
    }
}

bool get_hud_matrices(glm::mat4 &proj, glm::mat4&model)
{
    LLRect whole_screen = get_whole_screen_region();
    return get_hud_matrices(whole_screen, proj, model);
}

bool setup_hud_matrices()
{
    LLRect whole_screen = get_whole_screen_region();
    return setup_hud_matrices(whole_screen);
}

bool setup_hud_matrices(const LLRect& screen_region)
{
    glm::mat4 proj, model;
    bool result = get_hud_matrices(screen_region, proj, model);
    if (!result) return result;

    // set up transform to keep HUD objects in front of camera
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.loadMatrix(glm::value_ptr(proj));
    set_current_projection(proj);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(glm::value_ptr(model));
    set_current_modelview(model);
    return true;
}

void display_cleanup()
{
    gDisconnectedImagep = nullptr;
}

// Vulkan: display() is diverted, but the teleport state machine (START_ARRIVAL ->
// ARRIVING -> NONE, the progress screen, the give-up timeout) lives in it; without
// this a teleport stalls at TELEPORT_START_ARRIVAL with the old region unloaded.
// Same per-frame step display() runs while gTeleportDisplay is set.
void display_update_teleport()
{
    if (!gTeleportDisplay) return;
    const bool minimized = gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getWindow()->getMinimized();
    update_tp_display(minimized);
// [SL:KB] - Patch: Appearance-TeleportAttachKill | Checked: Catznip-4.0
    if (!minimized) LLViewerParcelMgr::getInstance()->onTeleportDone();
// [/SL:KB]
}
