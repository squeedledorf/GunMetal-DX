/**
 * @file llviewerwindow.cpp
 * @brief Implementation of the LLViewerWindow class.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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
#include "llviewerwindow.h"
#include "slkeycommands.h"
#include "llworldclock.h"
#include "llmemorygovernor.h"         // #873 live snapshot system-memory check
#include "llvkhitch.h"

#include "vkgfx/llvulkancontext.h"  // EXPERIMENTAL Vulkan: swapchain recreate on resize

// system library includes
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/lambda/core.hpp>
#include <boost/regex.hpp>

#include "llagent.h"
#include "llagentcamera.h"
#include "llcommandhandler.h"
#include "llcommunicationchannel.h"
#include "llfloaterreg.h"
#include "llhudicon.h"
#include "llmeshrepository.h"
#include "llnotificationhandler.h"
#include "llpanellogin.h"
#include "llsetkeybinddialog.h"
#include "llviewerinput.h"
#include "llviewermenu.h"

#include "llviewquery.h"
#include "llxmltree.h"
#include "llslurl.h"
#include "llrender.h"

#include "stringize.h"

//
// TODO: Many of these includes are unnecessary.  Remove them.
//

// linden library includes
#include "llaudioengine.h"      // mute on minimize
#include "llchatentry.h"
#include "indra_constants.h"
#include "llassetstorage.h"
#include "lldate.h"
#include "llerrorcontrol.h"
#include "llfontgl.h"
#include "llmousehandler.h"
#include "llrect.h"
#include "llsky.h"
#include "llstring.h"
#include "llui.h"
#include "lluuid.h"
#include "llview.h"
#include "llscrollbar.h"
#include "llscrollcontainer.h"
#include "llscrolllistctrl.h"
#include "llxfermanager.h"
#include "message.h"
#include "object_flags.h"
#include "lltimer.h"
#include "lltooltip.h"
#include "llmediaentry.h"
#include "llurldispatcher.h"
#include "raytrace.h"

// newview includes
#include "slposeik.h"
#include "slmmbnav.h"
#include "slchatbar.h"
#include "sltpscamera.h"
#include "fscombathitmarker.h" // <GMDX>
#include "fsfloaterkillfeed.h" // <GMDX>
#include "gmdxselectionoutline.h" // <GMDX>
#include "gmdxchatlog.h" // <GMDX>
#include "llagent.h"
// [SL:KB] - Patch: Build-DragNDrop | Checked: 2013-07-22 (Catznip-3.6)
#include "llagentbenefits.h"
// [/SL:KB]
#include "llbox.h"
#include "llchicletbar.h"
#include "llconsole.h"
#include "llviewercontrol.h"
#include "llcylinder.h"
#include "lldebugview.h"
#include "lldir.h"
#include "lldrawable.h"
#include "llmaniptranslate.h"
#include "llface.h"
#include "llfeaturemanager.h"
#include "llfilepicker.h"
#include "llfirstuse.h"
#include "llfloater.h"
#include "llfloaterbuyland.h"
#include "llfloatercamera.h"
#include "llfloaterland.h"
#include "llfloaterinspect.h"
#include "llfloatermap.h"
#include "llfloaternamedesc.h"
#include "llfloaterpreference.h"
#include "llfloaterprogressview.h"
#include "llfloatersnapshot.h"
#include "llfloatertools.h"
#include "llfloaterworldmap.h"
#include "llfocusmgr.h"
#include "llfontfreetype.h"
#include "llgamecontrol.h"
#include "llgesturemgr.h"
#include "llhudmanager.h"
#include "llhudobject.h"
#include "llhudview.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "llimageworker.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "lllocalbitmaps.h"
#include "llfloaterlocalassets.h"
#include "lllocalgltfmaterials.h"
#include "llmenugl.h"
#include "llmenuoptionpathfindingrebakenavmesh.h"
#include "llmodaldialog.h"
#include "llchannelmanager.h"
#include "llmorphview.h"
#include "llmoveview.h"
#include "llnavigationbar.h"
#include "llpaneltopinfobar.h"
#include "llpopupview.h"
#include "llpreviewtexture.h"
#include "llprogressview.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llrootview.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "llstatview.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltexlayer.h"
#include "lltextbox.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "lltextureview.h"
#include "lltoast.h"
#include "lltool.h"
#include "lltoolbarview.h"
#include "lltoolcomp.h"
#include "lltooldraganddrop.h"
#include "lltoolface.h"
#include "lltoolfocus.h"
#include "lltoolgrab.h"
#include "lltoolmgr.h"
#include "lltoolmorph.h"
#include "lltoolpie.h"
#include "lltoolselectland.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llversioninfo.h"
#include "llvieweraudio.h"
#include "llviewergesture.h"
#include "llviewertexturelist.h"
#include "llviewerinventory.h"
#include "llviewermedia.h"
#include "llviewermediafocus.h"
#include "llviewermenu.h"
// [SL:KB] - Patch: Build-DragNDrop | Checked: 2013-07-22 (Catznip-3.6)
#include "llviewermenufile.h"
// [/SL:KB]
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llrender.h"
#include "llviewerstats.h"
#include "llvoavatarself.h"
#include "llvopartgroup.h"
#include "llvovolume.h"
#include "llvocachegrid.h"
#include "llworld.h"
#include "llworldmapview.h"
#include "llcleaningest.h"   // single-pass layered probe render (task #52)
#include "llcleaninstancestore.h"   // clean-pipeline picking under VK (dead LL octree)
#include "llcleanmeshasset.h"       // DebugShowTextureInfo: UV density decode
#include "pipeline.h"
#include "llappviewer.h"
#include "llviewerdisplay.h"
#include "llviewerjoystick.h"
#include "llviewermenufile.h" // LLFilePickerReplyThread
#include "llviewernetwork.h"
#include "llfloaterimnearbychat.h"
#include "llviewerchat.h"
#include "llagentui.h"
#include "llwearablelist.h"

#include "llviewereventrecorder.h"

#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llnotificationmanager.h"

#include "llfloaternotificationsconsole.h"

#include "llwindowlistener.h"
#include "llsdl.h"
#include "llviewerwindowlistener.h"
#include "llcleanup.h"
#include "llcleancursorpick.h"   // GPU raster cursor pick (#702)

// [RLVa:KB] - Checked: 2010-03-31 (RLVa-1.2.0c)
#include "rlvactions.h"
#include "rlveffects.h"
#include "rlvhandler.h"
// [/RLVa:KB]

#if LL_WINDOWS
#include <tchar.h> // For Unicode conversion methods
#endif

void upload_single_file(const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter type, const LLUUID& dest);
void upload_bulk(const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter type, bool allow_2k, const LLUUID& dest);
//
// Globals
//

extern bool gDebugClicks;
extern bool gDisplaySwapBuffers;
extern bool gDepthDirty;
extern bool gResizeScreenTexture;
extern bool gSnapshotNoPost;

LLViewerWindow  *gViewerWindow = NULL;

LLFrameTimer    gAwayTimer;
LLFrameTimer    gAwayTriggerTimer;

bool            gShowOverlayTitle = false;

LLViewerObject*  gDebugRaycastObject = NULL;
bool            gDebugRaycastParticle = false;
LLVector4a       gDebugRaycastIntersection;
LLVector4a      gDebugRaycastParticleIntersection;
LLVector2        gDebugRaycastTexCoord;
LLVector4a       gDebugRaycastNormal;
LLVector4a       gDebugRaycastTangent;
S32             gDebugRaycastFaceHit;
S32             gDebugRaycastGLTFNodeHit;
S32             gDebugRaycastGLTFPrimitiveHit;
LLVector4a       gDebugRaycastStart;
LLVector4a       gDebugRaycastEnd;

// HUD display lines in lower right
bool                gDisplayWindInfo = false;
bool                gDisplayCameraPos = false;
bool                gDisplayFOV = false;
bool                gDisplayBadge = false;

static const U8 NO_FACE = 255;
bool gQuietSnapshot = false;

// Minimum value for UIScaleFactor, also defined in preferences, ui_scale_slider
static const F32 MIN_UI_SCALE = 0.75f;
// 4.0 in preferences, but win10 supports larger scaling and value is used more as
// sanity check, so leaving space for larger values from DPI updates.
static const F32 MAX_UI_SCALE = 7.0f;
static const F32 MIN_DISPLAY_SCALE = 0.75f;
LLTrace::SampleStatHandle<> LLViewerWindow::sMouseVelocityStat("Mouse Velocity");


class RecordToChatConsoleRecorder : public LLError::Recorder
{
public:
    virtual void recordMessage(LLError::ELevel level,
                                const std::string& message)
    {
        //FIXME: this is NOT thread safe, and will do bad things when a warning is issued from a non-UI thread

        // only log warnings to chat console
        //if (level == LLError::LEVEL_WARN)
        //{
            //LLFloaterChat* chat_floater = LLFloaterReg::findTypedInstance<LLFloaterChat>("chat");
            //if (chat_floater && gSavedSettings.getBOOL("WarningsAsChat"))
            //{
            //  LLChat chat;
            //  chat.mText = message;
            //  chat.mSourceType = CHAT_SOURCE_SYSTEM;

            //  chat_floater->addChat(chat, false, false);
            //}
        //}
    }
};

class RecordToChatConsole : public LLSingleton<RecordToChatConsole>
{
    LLSINGLETON(RecordToChatConsole);
public:
    void startRecorder() { LLError::addRecorder(mRecorder); }
    void stopRecorder() { LLError::removeRecorder(mRecorder); }

private:
    LLError::RecorderPtr mRecorder;
};

RecordToChatConsole::RecordToChatConsole():
    mRecorder(new RecordToChatConsoleRecorder())
{
    mRecorder->showTags(false);
    mRecorder->showLocation(false);
    mRecorder->showMultiline(true);
}

////////////////////////////////////////////////////////////////////////////
//
// Print Utility
//

// Convert a normalized float (-1.0 <= x <= +1.0) to a fixed 1.4 format string:
//
//    s#.####
//
// Where:
//    s  sign character; space if x is positiv, minus if negative
//    #  decimal digits
//
// This is similar to printf("%+.4f") except positive numbers are NOT cluttered with a leading '+' sign.
// NOTE: This does NOT null terminate the output
void normalized_float_to_string(const float x, char *out_str)
{
    static const unsigned char DECIMAL_BCD2[] =
    {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99
    };

    int neg = (x < 0);
    int rem = neg
            ? (int)(x * -10000.)
            : (int)(x *  10000.);

    int d10 = rem % 100; rem /= 100;
    int d32 = rem % 100; rem /= 100;

    out_str[6] = '0' + ((DECIMAL_BCD2[ d10 ] >> 0) & 0xF);
    out_str[5] = '0' + ((DECIMAL_BCD2[ d10 ] >> 4) & 0xF);
    out_str[4] = '0' + ((DECIMAL_BCD2[ d32 ] >> 0) & 0xF);
    out_str[3] = '0' + ((DECIMAL_BCD2[ d32 ] >> 4) & 0xF);
    out_str[2] = '.';
    out_str[1] = '0' + (rem & 1);
    out_str[0] = " -"[neg]; // Could always show '+' for positive but this clutters up the common case
}

// normalized float
//    printf("%-.4f    %-.4f    %-.4f")
// Params:
//   float  &matrix_row[4]
//   int    matrix_cell_index
//   string out_buffer (size 32)
// Note: The buffer is assumed to be pre-filled with spaces
#define MATRIX_ROW_N32_TO_STR(matrix_row, i, out_buffer)          \
    normalized_float_to_string(matrix_row[i+0], out_buffer +  0); \
    normalized_float_to_string(matrix_row[i+1], out_buffer + 11); \
    normalized_float_to_string(matrix_row[i+2], out_buffer + 22); \
    out_buffer[31] = 0;


// regular float
//    sprintf(buffer, "%-8.2f  %-8.2f  %-8.2f", matrix_row[i+0], matrix_row[i+1], matrix_row[i+2]);
// Params:
//   float  &matrix_row[4]
//   int    matrix_cell_index
//   char   out_buffer[32]
// Note: The buffer is assumed to be pre-filled with spaces
#define MATRIX_ROW_F32_TO_STR(matrix_row, i, out_buffer) {                       \
    static const char *format[3] = {                                             \
        "%-8.2f"  ,  /* 0 */                                                     \
        ">  99K  ",  /* 1 */                                                     \
        "< -99K  "   /* 2 */                                                     \
    };                                                                           \
                                                                                 \
    F32 temp_0 = matrix_row[i+0];                                                \
    F32 temp_1 = matrix_row[i+1];                                                \
    F32 temp_2 = matrix_row[i+2];                                                \
                                                                                 \
    U8 flag_0 = (((U8)(temp_0 < -99999.99)) << 1) | ((U8)(temp_0 > 99999.99));   \
    U8 flag_1 = (((U8)(temp_1 < -99999.99)) << 1) | ((U8)(temp_1 > 99999.99));   \
    U8 flag_2 = (((U8)(temp_2 < -99999.99)) << 1) | ((U8)(temp_2 > 99999.99));   \
                                                                                 \
    if (temp_0 < 0.f) out_buffer[ 0] = '-';                                      \
    if (temp_1 < 0.f) out_buffer[11] = '-';                                      \
    if (temp_2 < 0.f) out_buffer[22] = '-';                                      \
                                                                                 \
    sprintf(out_buffer+ 1,format[flag_0],fabsf(temp_0)); out_buffer[ 1+8] = ' '; \
    sprintf(out_buffer+12,format[flag_1],fabsf(temp_1)); out_buffer[12+8] = ' '; \
    sprintf(out_buffer+23,format[flag_2],fabsf(temp_2)); out_buffer[23+8] =  0 ; \
}

////////////////////////////////////////////////////////////////////////////
//
// LLDebugText
//

static LLTrace::BlockTimerStatHandle FTM_DISPLAY_DEBUG_TEXT("Display Debug Text");

// #685 TEXTURE INSPECTOR (RenderVKTexInspector, Develop > Rendering): the Vulkan residency
// state of the texture on the face under the cursor -- full / resident / requested level,
// why it is below full, storage format, texel density (LLCleanRendererVK::texInspectText).
// Refreshed at 4 Hz by LLDebugText: one cursor pick; the GPU pick's per-pixel derivatives
// of the sampled UV at that surface give the texel density at the cursor (#704).
static std::string vk_tex_inspector_text()
{
    if (!LLVulkanContext::instance().isInitialized()) return "Texture inspector: the Vulkan renderer is not running";
    const S32 x = gViewerWindow->getCurrentMouseX(), y = gViewerWindow->getCurrentMouseY();
    LLPickInfo pick = gViewerWindow->pickImmediate(x, y, true /*transparent*/, true /*rigged*/);
    LLViewerObject* obj = pick.getObject();
    if (!obj || pick.mPickType != LLPickInfo::PICK_OBJECT || pick.mObjectFace < 0)
        return "Texture inspector: no object face under the cursor";
    // UV per raw window pixel, right / up (the pick target's y runs down the screen).
    F32 uv_px[4] = {0.f, 0.f, 0.f, 0.f};
    LLVector2 uv_at;
    bool have_uv = LLCleanCursorPick::surfaceTexture(obj, pick.mObjectFace, uv_at, uv_px);
    uv_px[2] = -uv_px[2];
    uv_px[3] = -uv_px[3];
    static LLCachedControl<U32> s_map(gSavedSettings, "RenderVKDebugMipMap", 0);
    std::string t = LLVulkanContext::instance().texInspectText(obj->getID(), pick.mObjectFace, (U32)s_map,
                                                                have_uv ? uv_px : nullptr);
    if (t.empty()) t = "Texture inspector: the Vulkan world renderer is not up yet";
    return t;
}

class LLDebugText
{
private:
    struct Line
    {
        Line(const std::string& in_text, S32 in_x, S32 in_y) : text(in_text), x(in_x), y(in_y) {}
        std::string text;
        S32 x,y;
    };

    LLViewerWindow *mWindow;

    typedef std::vector<Line> line_list_t;
    line_list_t mLineList;
    LLColor4 mTextColor;

    LLColor4 mBackColor;
    LLRect mBackRectCamera1;
    LLRect mBackRectCamera2;

    void addText(S32 x, S32 y, const std::string &text)
    {
        mLineList.push_back(Line(text, x, y));
    }

    void clearText() { mLineList.clear(); }

public:
    LLDebugText(LLViewerWindow* window) : mWindow(window) {}

    void update()
    {
        if (!gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        {
            clearText();
            return;
        }

        static LLCachedControl<bool> log_texture_traffic(gSavedSettings,"LogTextureNetworkTraffic", false) ;

        std::string wind_vel_text;
        std::string wind_vector_text;
        std::string rwind_vel_text;
        std::string rwind_vector_text;
        std::string audio_text;

        static const std::string beacon_particle = LLTrans::getString("BeaconParticle");
        static const std::string beacon_physical = LLTrans::getString("BeaconPhysical");
        static const std::string beacon_scripted = LLTrans::getString("BeaconScripted");
        static const std::string beacon_scripted_touch = LLTrans::getString("BeaconScriptedTouch");
        static const std::string beacon_sound = LLTrans::getString("BeaconSound");
        static const std::string beacon_media = LLTrans::getString("BeaconMedia");
        static const std::string beacon_sun = LLTrans::getString("BeaconSun");
        static const std::string beacon_moon = LLTrans::getString("BeaconMoon");
        static const std::string particle_hiding = LLTrans::getString("ParticleHiding");

        // Draw the statistics in a light gray
        // and in a thin font
        mTextColor = LLColor4( 0.86f, 0.86f, 0.86f, 1.f );

        // Draw stuff growing up from right lower corner of screen
        S32 x_right = mWindow->getWorldViewWidthScaled();
        S32 xpos = x_right - 400;
        xpos = llmax(xpos, 0);
        S32 ypos = 64;
        const S32 y_inc = 20;

        // Camera matrix text is hard to see again a white background
        // Add a dark background underneath the matrices for readability (contrast)
        mBackRectCamera1.mLeft   = xpos;
        mBackRectCamera1.mRight  = x_right;
        mBackRectCamera1.mTop    = -1;
        mBackRectCamera1.mBottom = -1;
        mBackRectCamera2 = mBackRectCamera1;

        mBackColor = LLUIColorTable::instance().getColor( "MenuDefaultBgColor" );

        clearText();

        static LLCachedControl<bool> debug_show_time(gSavedSettings, "DebugShowTime", false);
        if (debug_show_time())
        {
            F32 time = gFrameTimeSeconds;
            S32 hours = (S32)(time / (60*60));
            S32 mins = (S32)((time - hours*(60*60)) / 60);
            S32 secs = (S32)((time - hours*(60*60) - mins*60));
            addText(xpos, ypos, llformat("Time: %d:%02d:%02d", hours,mins,secs)); ypos += y_inc;
        }

        static LLCachedControl<bool> debug_show_memory(gSavedSettings, "DebugShowMemory", false);
        if (debug_show_memory())
        {
            addText(xpos, ypos,
                    STRINGIZE("Memory: " << (LLMemory::getCurrentRSS() / 1024) << " (KB)"));
            ypos += y_inc;
        }

        if (gDisplayCameraPos)
        {
            std::string camera_view_text;
            std::string camera_center_text;
            std::string agent_view_text;
            std::string agent_left_text;
            std::string agent_center_text;
            std::string agent_root_center_text;

            LLVector3d tvector; // Temporary vector to hold data for printing.

            // Update camera center, camera view, wind info every other frame
            tvector = gAgent.getPositionGlobal();
            agent_center_text = llformat("AgentCenter  %f %f %f",
                                         (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));

            if (isAgentAvatarValid())
            {
                tvector = gAgent.getPosGlobalFromAgent(gAgentAvatarp->mRoot->getWorldPosition());
                agent_root_center_text = llformat("AgentRootCenter %f %f %f",
                                                  (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));
            }
            else
            {
                agent_root_center_text = "---";
            }


            tvector = LLVector4(gAgent.getFrameAgent().getAtAxis());
            agent_view_text = llformat("AgentAtAxis  %f %f %f",
                                       (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));

            tvector = LLVector4(gAgent.getFrameAgent().getLeftAxis());
            agent_left_text = llformat("AgentLeftAxis  %f %f %f",
                                       (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));

            tvector = gAgentCamera.getCameraPositionGlobal();
            camera_center_text = llformat("CameraCenter %f %f %f",
                                          (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));

            tvector = LLVector4(LLViewerCamera::getInstance()->getAtAxis());
            camera_view_text = llformat("CameraAtAxis    %f %f %f",
                                        (F32)(tvector.mdV[VX]), (F32)(tvector.mdV[VY]), (F32)(tvector.mdV[VZ]));

// [RLVa:KB] - @showloc
            if (!RlvActions::canShowLocation())
            {
                agent_center_text = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
                agent_root_center_text = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
                camera_center_text = RlvStrings::getString(RlvStringKeys::Hidden::Generic);
            }
// [/RLVa:KB]

            addText(xpos, ypos, agent_center_text);  ypos += y_inc;
            addText(xpos, ypos, agent_root_center_text);  ypos += y_inc;
            addText(xpos, ypos, agent_view_text);  ypos += y_inc;
            addText(xpos, ypos, agent_left_text);  ypos += y_inc;
            addText(xpos, ypos, camera_center_text);  ypos += y_inc;
            addText(xpos, ypos, camera_view_text);  ypos += y_inc;
        }

        if (gDisplayWindInfo)
        {
            wind_vel_text = llformat("Wind velocity %.2f m/s", gWindVec.magVec());
            wind_vector_text = llformat("Wind vector   %.2f %.2f %.2f", gWindVec.mV[0], gWindVec.mV[1], gWindVec.mV[2]);
            rwind_vel_text = llformat("RWind vel %.2f m/s", gRelativeWindVec.magVec());
            rwind_vector_text = llformat("RWind vec   %.2f %.2f %.2f", gRelativeWindVec.mV[0], gRelativeWindVec.mV[1], gRelativeWindVec.mV[2]);

            addText(xpos, ypos, wind_vel_text);  ypos += y_inc;
            addText(xpos, ypos, wind_vector_text);  ypos += y_inc;
            addText(xpos, ypos, rwind_vel_text);  ypos += y_inc;
            addText(xpos, ypos, rwind_vector_text);  ypos += y_inc;
        }
        if (gDisplayWindInfo)
        {
            audio_text = llformat("Audio for wind: %d", gAudiop ? gAudiop->isWindEnabled() : -1);
            addText(xpos, ypos, audio_text);  ypos += y_inc;
        }
        if (gDisplayFOV)
        {
            addText(xpos, ypos, llformat("FOV: %2.1f deg", RAD_TO_DEG * LLViewerCamera::getInstance()->getView()));
            ypos += y_inc;
        }
        // #479 frozen culling camera: the overlay's legend (empty unless frozen).
        for (const std::string& line : LLVOCacheDebug::legend())
        {
            addText(xpos, ypos, line);
            ypos += y_inc;
        }
        if (gDisplayBadge)
        {
            addText(xpos, ypos+(y_inc/2), llformat("Hippos!", RAD_TO_DEG * LLViewerCamera::getInstance()->getView()));
            ypos += y_inc * 2;
        }

        /*if (LLViewerJoystick::getInstance()->getOverrideCamera())
        {
            addText(xpos + 200, ypos, llformat("Flycam"));
            ypos += y_inc;
        }*/

        static LLCachedControl<bool> debug_show_render_info(gSavedSettings, "DebugShowRenderInfo", false);
        if (debug_show_render_info())
        {
            LLTrace::Recording& last_frame_recording = LLTrace::get_frame_recording().getLastRecording();

            //show streaming cost/triangle count of known prims in current region OR selection
            {
                F32 cost = 0.f;
                S32 count = 0;
                S32 vcount = 0;
                S32 object_count = 0;
                S32 total_bytes = 0;
                S32 visible_bytes = 0;

                const char* label = "Region";
                if (LLSelectMgr::getInstance()->getSelection()->getObjectCount() == 0)
                { //region
                    LLViewerRegion* region = gAgent.getRegion();
                    if (region)
                    {
                        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
                        {
                            LLViewerObject* object = gObjectList.getObject(i);
                            if (object &&
                                object->getRegion() == region &&
                                object->getVolume())
                            {
                                object_count++;
                                S32 bytes = 0;
                                S32 visible = 0;
                                cost += object->getStreamingCost();
                                LLMeshCostData costs;
                                if (object->getCostData(costs))
                                {
                                    bytes = costs.getSizeTotal();
                                    visible = costs.getSizeByLOD(object->getLOD());
                                }

                                S32 vt = 0;
                                count += object->getTriangleCount(&vt);
                                vcount += vt;
                                total_bytes += bytes;
                                visible_bytes += visible;
                            }
                        }
                    }
                }
                else
                {
                    label = "Selection";
                    cost = LLSelectMgr::getInstance()->getSelection()->getSelectedObjectStreamingCost(&total_bytes, &visible_bytes);
                    count = LLSelectMgr::getInstance()->getSelection()->getSelectedObjectTriangleCount(&vcount);
                    object_count = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
                }

                addText(xpos,ypos, llformat("%s streaming cost: %.1f", label, cost));
                ypos += y_inc;

                addText(xpos, ypos, llformat("    %.3f KTris, %.3f KVerts, %.1f/%.1f KB, %d objects",
                                        count/1000.f, vcount/1000.f, visible_bytes/1024.f, total_bytes/1024.f, object_count));
                ypos += y_inc;

            }

            addText(xpos, ypos, llformat("%d Render Calls", (U32)last_frame_recording.getSampleCount(LLPipeline::sStatBatchSize)));
            ypos += y_inc;

            addText(xpos, ypos, llformat("%d/%d Objects Active", gObjectList.getNumActiveObjects(), gObjectList.getNumObjects()));
            ypos += y_inc;

            addText(xpos, ypos, llformat("%d Matrix Ops", gPipeline.mMatrixOpCount));
            ypos += y_inc;

            addText(xpos, ypos, llformat("%d Texture Matrix Ops", gPipeline.mTextureMatrixOps));
            ypos += y_inc;

            gPipeline.mTextureMatrixOps = 0;
            gPipeline.mMatrixOpCount = 0;

            if (last_frame_recording.getSampleCount(LLPipeline::sStatBatchSize) > 0)
            {
                addText(xpos, ypos, llformat("Batch min/max/mean: %d/%d/%d", (U32)last_frame_recording.getMin(LLPipeline::sStatBatchSize), (U32)last_frame_recording.getMax(LLPipeline::sStatBatchSize), (U32)last_frame_recording.getMean(LLPipeline::sStatBatchSize)));
            }
            ypos += y_inc;


            addText(xpos,ypos, llformat("%d Avatars visible", LLVOAvatar::sNumVisibleAvatars));

            ypos += y_inc;

            addText(xpos,ypos, llformat("%d Lights visible", LLPipeline::sVisibleLightCount));

            ypos += y_inc;

            if (gMeshRepo.meshRezEnabled())
            {
                addText(xpos, ypos, llformat("%.3f MB Mesh Data Received", LLMeshRepository::sBytesReceived/(1024.f*1024.f)));

                ypos += y_inc;

                addText(xpos, ypos, llformat("%d/%d Mesh HTTP Requests/Retries", LLMeshRepository::sHTTPRequestCount,
                    LLMeshRepository::sHTTPRetryCount));
                ypos += y_inc;

                addText(xpos, ypos, llformat("%d/%d Mesh LOD Pending/Processing", LLMeshRepository::sLODPending, LLMeshRepository::sLODProcessing));
                ypos += y_inc;

                addText(xpos, ypos, llformat("%.3f/%.3f MB Mesh Cache Read/Write ", LLMeshRepository::sCacheBytesRead/(1024.f*1024.f), LLMeshRepository::sCacheBytesWritten/(1024.f*1024.f)));
                ypos += y_inc;

                addText(xpos, ypos, llformat("%.3f/%.3f MB Mesh Skins/Decompositions Memory", LLMeshRepository::sCacheBytesSkins / (1024.f*1024.f), LLMeshRepository::sCacheBytesDecomps / (1024.f*1024.f)));
                ypos += y_inc;

                addText(xpos, ypos, llformat("%.3f MB Mesh Headers Memory", LLMeshRepository::sCacheBytesHeaders / (1024.f*1024.f)));

                ypos += y_inc;
            }

            LLPipeline::sVisibleLightCount = 0;
        }
        static LLCachedControl<bool> debug_show_avatar_render_info(gSavedSettings, "DebugShowAvatarRenderInfo", false);
        if (debug_show_avatar_render_info())
        {
            std::map<std::string, LLVOAvatar*> sorted_avs;
            {
                for (LLCharacter* character : LLCharacter::sInstances)
                {
                    LLVOAvatar* avatar = (LLVOAvatar*)character;
                    if (!avatar->isDead()) // Not dead yet
                    {
                        // Stuff into a sorted map so the display is ordered
                        sorted_avs[avatar->getFullname()] = avatar;
                    }
                }
            }

            std::string trunc_name;
            std::map<std::string, LLVOAvatar*>::reverse_iterator av_iter = sorted_avs.rbegin();     // Put "A" at the top
            while (av_iter != sorted_avs.rend())
            {
                LLVOAvatar* avatar = av_iter->second;

                avatar->calculateUpdateRenderComplexity(); // Make sure the numbers are up-to-date

                trunc_name = utf8str_truncate(avatar->getFullname(), 16);
                addText(xpos, ypos, llformat("%s : %s, complexity %d, area %.2f",
                    trunc_name.c_str(),
                    LLVOAvatar::rezStatusToString(avatar->getRezzedStatus()).c_str(),
                    avatar->getVisualComplexity(),
                    avatar->getAttachmentSurfaceArea()));
                ypos += y_inc;
                av_iter++;
            }
        }
        static LLCachedControl<bool> debug_show_game_controller_state(gSavedSettings, "DebugShowControllerState", false);
        if (debug_show_game_controller_state())
        {
            static const char* axis_names[LLGameControl::NUM_MOVE_DIRS] = {
                "LEFT","RIGHT","FORWARD","BACKWARD", "TURN LEFT", "TURN RIGHT",
                "LOOK UP","LOOK DOWN", "UP", "DOWN", "ROLL LEFT", "ROLL RIGHT"
            };
            static const char* button_names[LLGameControl::NUM_BUTTONS] = {
                "A", "B", "X", "Y", "BACK", "SELECT", "START", "LSTICK", "RSTICK",
                "LSHOULDER", "RSHOULDER", "UP", "DOWN", "LEFT", "RIGHT",
                "MISC1", "PADDLE1", "PADDLE2", "PADDLE3", "PADDLE4", "TOUCHPAD",
                "B21", "B22", "B23", "B24", "B25", "B26", "B27", "B28", "B29", "B30", "B31",
            };
            const LLGameControl::State& gc_state = LLGameControl::getState();
            for (size_t i = 0; i < gc_state.mAxes.size(); i+=2)
            {
                addText(xpos, ypos, llformat(
                    "  %-10s %6d   %-10s %6d",
                    (i < LLGameControl::NUM_MOVE_DIRS) ? axis_names[i] : "AXIS",
                    (S32)gc_state.mAxes[i],
                    ((i+1) < LLGameControl::NUM_MOVE_DIRS) ? axis_names[i+1] : "AXIS",
                    i < gc_state.mAxes.size() ? (S32)gc_state.mAxes[i+1] : 0
                ));
                ypos += y_inc;
            }
            addText(xpos, ypos, "Game Controller Final State Axes");
            ypos += y_inc;
            static const char* pressed = "░░░░░░░░░░";
            static const char* released = "          ";
            for (U8 i = 0; i < LLGameControl::NUM_BUTTONS; i += 4)
            {
                addText(xpos, ypos, llformat(
                    "  %-10s %-10s %-10s %-10s",
                    gc_state.mButtons & (1 << (i+0)) ? pressed : released,
                    gc_state.mButtons & (1 << (i+1)) ? pressed : released,
                    gc_state.mButtons & (1 << (i+2)) ? pressed : released,
                    gc_state.mButtons & (1 << (i+3)) ? pressed : released
                ));
                addText(xpos, ypos, llformat("  %-10s %-10s %-10s %-10s", button_names[i],button_names[i+1],button_names[i+2],button_names[i+3]));
                ypos += y_inc;
            }
            addText(xpos, ypos, "Game Controller Final State Buttons");
            ypos += y_inc;
        }
        static LLCachedControl<bool> debug_show_render_matrices(gSavedSettings, "DebugShowRenderMatrices", false);
        if (debug_show_render_matrices())
        {
            char camera_lines[8][32];
            memset(camera_lines, ' ', sizeof(camera_lines));

            // Projection last column is always <0,0,-1.0001,0>
            // Projection last row is always <0,0,-0.2>
            mBackRectCamera1.mBottom = ypos - y_inc + 2;
            MATRIX_ROW_N32_TO_STR(gGLProjection, 12,camera_lines[7]); addText(xpos, ypos, std::string(camera_lines[7])); ypos += y_inc;
            MATRIX_ROW_N32_TO_STR(gGLProjection,  8,camera_lines[6]); addText(xpos, ypos, std::string(camera_lines[6])); ypos += y_inc;
            MATRIX_ROW_N32_TO_STR(gGLProjection,  4,camera_lines[5]); addText(xpos, ypos, std::string(camera_lines[5])); ypos += y_inc; mBackRectCamera1.mTop    = ypos + 2;
            MATRIX_ROW_N32_TO_STR(gGLProjection,  0,camera_lines[4]); addText(xpos, ypos, std::string(camera_lines[4])); ypos += y_inc; mBackRectCamera2.mBottom = ypos + 2;

            addText(xpos, ypos, "Projection Matrix");
            ypos += y_inc;

#if LL_DARWIN
// For sprintf deprecation
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            // View last column is always <0,0,0,1>
            MATRIX_ROW_F32_TO_STR(gGLModelView, 12,camera_lines[3]); addText(xpos, ypos, std::string(camera_lines[3])); ypos += y_inc;
#if LL_DARWIN
#pragma clang diagnostic pop
#endif
            MATRIX_ROW_N32_TO_STR(gGLModelView,  8,camera_lines[2]); addText(xpos, ypos, std::string(camera_lines[2])); ypos += y_inc;
            MATRIX_ROW_N32_TO_STR(gGLModelView,  4,camera_lines[1]); addText(xpos, ypos, std::string(camera_lines[1])); ypos += y_inc; mBackRectCamera2.mTop = ypos + 2;
            MATRIX_ROW_N32_TO_STR(gGLModelView,  0,camera_lines[0]); addText(xpos, ypos, std::string(camera_lines[0])); ypos += y_inc;

            addText(xpos, ypos, "View Matrix");
            ypos += y_inc;
        }
        // disable pixel readback which messes up nVidia nSight graphics debugging
        static LLCachedControl<bool> debug_show_color(gSavedSettings, "DebugShowColor", false);
        if (debug_show_color())
        {
            U8 color[4];
            LLCoordGL coord = gViewerWindow->getCurrentMouse();

            // Convert x,y to raw pixel coords
            S32 x_raw = (S32)llround(coord.mX * gViewerWindow->getWindowWidthRaw() / (F32) gViewerWindow->getWindowWidthScaled());
            S32 y_raw = (S32)llround(coord.mY * gViewerWindow->getWindowHeightRaw() / (F32) gViewerWindow->getWindowHeightScaled());

            // Sample the world-frame swap copy (world only, no HUD/UI, refreshed
            // every ~2 s) -- no GPU wait.
            if (LLVulkanContext::instance().readWorldPixel(x_raw, y_raw, color))
            {
                addText(xpos, ypos, llformat("Pixel <%1d, %1d> R:%1d G:%1d B:%1d A:%1d (world, <=2s old)", x_raw, y_raw, color[0], color[1], color[2], color[3]));
            }
            else
            {
                addText(xpos, ypos, llformat("Pixel <%1d, %1d> (no world capture yet)", x_raw, y_raw));
            }
            ypos += y_inc;
        }

        // #685 texture inspector (see vk_tex_inspector_text): 4 Hz, lines bottom-up; a
        // wider column than the rest (the lines are long).
        static LLCachedControl<bool> s_tex_insp(gSavedSettings, "RenderVKTexInspector", false);
        if (s_tex_insp())
        {
            static std::vector<std::string> s_lines;
            static F32 s_next = 0.f;
            const F32 now = gFrameTimeSeconds;
            if (now >= s_next || s_lines.empty())
            {
                s_next = now + 0.25f;
                s_lines.clear();
                const std::string t = vk_tex_inspector_text();
                size_t a = 0;
                while (a <= t.size())
                {
                    const size_t b = t.find('\n', a);
                    std::string ln = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
                    // wrap at ~110 monospace columns (continuations indented)
                    const size_t kCols = 110;
                    while (ln.size() > kCols)
                    {
                        size_t cut = ln.rfind(' ', kCols);
                        if (cut == std::string::npos || cut < 20) cut = kCols;
                        s_lines.push_back(ln.substr(0, cut));
                        ln = "      " + ln.substr(cut + (ln[cut] == ' ' ? 1 : 0));
                    }
                    s_lines.push_back(ln);
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
            }
            const S32 xi = llmax(x_right - 900, 0);
            for (auto it = s_lines.rbegin(); it != s_lines.rend(); ++it)
            {
                addText(xi, ypos, *it);
                ypos += y_inc;
            }
        }

        // only display these messages if we are actually rendering beacons at this moment
        if (LLPipeline::getRenderBeacons() && LLFloaterReg::instanceVisible("beacons"))
        {
            if (LLPipeline::getRenderMOAPBeacons())
            {
                addText(xpos, ypos, "Viewing media beacons (white)");
                ypos += y_inc;
            }

            if (LLPipeline::toggleRenderTypeControlNegated(LLPipeline::RENDER_TYPE_PARTICLES))
            {
                addText(xpos, ypos, particle_hiding);
                ypos += y_inc;
            }

            if (LLPipeline::getRenderParticleBeacons())
            {
                addText(xpos, ypos, "Viewing particle beacons (blue)");
                ypos += y_inc;
            }

            if (LLPipeline::getRenderSoundBeacons())
            {
                addText(xpos, ypos, "Viewing sound beacons (yellow)");
                ypos += y_inc;
            }

            if (LLPipeline::getRenderScriptedBeacons())
            {
                addText(xpos, ypos, beacon_scripted);
                ypos += y_inc;
            }
            else
                if (LLPipeline::getRenderScriptedTouchBeacons())
                {
                    addText(xpos, ypos, beacon_scripted_touch);
                    ypos += y_inc;
                }

            if (LLPipeline::getRenderPhysicalBeacons())
            {
                addText(xpos, ypos, "Viewing physical object beacons (green)");
                ypos += y_inc;
            }
        }

        static LLUICachedControl<bool> show_sun_beacon("sunbeacon", false);
        static LLUICachedControl<bool> show_moon_beacon("moonbeacon", false);

        if (show_sun_beacon)
        {
            addText(xpos, ypos, beacon_sun);
            ypos += y_inc;
        }
        if (show_moon_beacon)
        {
            addText(xpos, ypos, beacon_moon);
            ypos += y_inc;
        }

        if(log_texture_traffic)
        {
            U32 old_y = ypos ;
            for(S32 i = LLViewerTexture::BOOST_NONE; i < LLViewerTexture::MAX_GL_IMAGE_CATEGORY; i++)
            {
                if(gTotalTextureBytesPerBoostLevel[i] > (S32Bytes)0)
                {
                    addText(xpos, ypos, llformat("Boost_Level %d:  %.3f MB", i, F32Megabytes(gTotalTextureBytesPerBoostLevel[i]).value()));
                    ypos += y_inc;
                }
            }
            if(ypos != old_y)
            {
                addText(xpos, ypos, "Network traffic for textures:");
                ypos += y_inc;
            }
        }

        static LLCachedControl<bool> debug_show_texture_info(gSavedSettings, "DebugShowTextureInfo", false);
        if (debug_show_texture_info())
        {
            LLViewerObject* objectp = NULL ;

            LLSelectNode* nodep = LLSelectMgr::instance().getHoverNode();
            if (nodep)
            {
                objectp = nodep->getObject();
            }

            if (objectp && !objectp->isDead())
            {
                S32 num_faces = objectp->mDrawable->getNumFaces() ;
                std::set<LLViewerFetchedTexture*> tex_list;

                for(S32 i = 0 ; i < num_faces; i++)
                {
                    LLFace* facep = objectp->mDrawable->getFace(i) ;
                    if(facep)
                    {
                        LLViewerFetchedTexture* tex = dynamic_cast<LLViewerFetchedTexture*>(facep->getTexture()) ;
                        if(tex)
                        {
                            if(tex_list.find(tex) != tex_list.end())
                            {
                                continue ; //already displayed.
                            }
                            tex_list.insert(tex);

                            std::string uuid_str;
                            tex->getID().toString(uuid_str);
                            uuid_str = uuid_str.substr(0,7);

                            addText(xpos, ypos, llformat("ID: %s v_size: %.3f", uuid_str.c_str(), tex->getMaxVirtualSize()));
                            ypos += y_inc;

                            addText(xpos, ypos, llformat("discard level: %d desired level: %d Missing: %s", tex->getDiscardLevel(),
                                tex->getDesiredDiscardLevel(), tex->isMissingAsset() ? "Y" : "N"));
                            ypos += y_inc;

                            // Vulkan residency: on-screen demand (top-mip texels, 16777215 = full)
                            // vs decoded / GPU-resident size; age > 5 s = handed back to LL.
                            addText(xpos, ypos, llformat("vk demand: %u age: %.1fs active: %s decoded: %dx%d full: %dx%d gpu: %u",
                                tex->getVKDemand(), tex->getVKDemandAge(), tex->vkDemandActive() ? "Y" : "N",
                                tex->getWidth(), tex->getHeight(), tex->getFullWidth(), tex->getFullHeight(), tex->getVKGpuTexels()));
                            ypos += y_inc;

                            // The cull's texel-demand input for this face (clean_cull.comp
                            // uvDensityWorld): captured UV density, object and world metres per
                            // raw UV unit (before the UV transform / anisotropy allowance).
                            LLCleanInstanceStore::Instance ci;
                            if (LLCleanInstanceStore::instance().debugGet(objectp->getID(), facep->getTEOffset(), ci))
                            {
                                U32 bits = 0;
                                std::memcpy(&bits, &ci.spin_pivot[3], sizeof(bits));
                                F32 d = 0.f, w[3] = {0.f, 0.f, 0.f}, st = 1.f;
                                if (LLCleanMeshAsset::decodeUVDensity(bits, d, w, st))
                                {
                                    const F32* m = ci.model_matrix;
                                    const F32 sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                                    const F32 sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                                    const F32 sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                                    const F32 cx = sy * sz, cy = sx * sz, cz = sx * sy;
                                    const F32 lin = sqrtf(sqrtf(w[0] * cx * cx + w[1] * cy * cy + w[2] * cz * cz));
                                    addText(xpos, ypos, llformat("uv density: %.4g m/UV (world %.4g) n2: %.2f %.2f %.2f uv stretch: %.2f",
                                        d, d * lin, w[0], w[1], w[2], st));
                                }
                                else
                                {
                                    addText(xpos, ypos, "uv density: none (demand from bounds / UV span)");
                                }
                                ypos += y_inc;
                            }
                        }
                    }
                }
            }
        }
    }

    void draw()
    {
        LL_RECORD_BLOCK_TIME(FTM_DISPLAY_DEBUG_TEXT);

        // Camera matrix text is hard to see again a white background
        // Add a dark background underneath the matrices for readability (contrast)
        if (mBackRectCamera1.mTop >= 0)
        {
            mBackColor.setAlpha( 0.75f );
            gl_rect_2d(mBackRectCamera1, mBackColor, true);

            mBackColor.setAlpha( 0.66f );
            gl_rect_2d(mBackRectCamera2, mBackColor, true);
        }

        for (line_list_t::iterator iter = mLineList.begin();
             iter != mLineList.end(); ++iter)
        {
            const Line& line = *iter;
            LLFontGL::getFontMonospace()->renderUTF8(line.text, 0, (F32)line.x, (F32)line.y, mTextColor,
                    LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }

};

void LLViewerWindow::updateDebugText()
{
    mDebugText->update();
}

////////////////////////////////////////////////////////////////////////////
//
// LLViewerWindow
//

LLViewerWindow::Params::Params()
:   title("title"),
    name("name"),
    x("x"),
    y("y"),
    width("width"),
    height("height"),
    min_width("min_width"),
    min_height("min_height"),
    fullscreen("fullscreen", false),
    ignore_pixel_depth("ignore_pixel_depth", false)
{}


void LLViewerWindow::handlePieMenu(S32 x, S32 y, MASK mask)
{
    if (CAMERA_MODE_CUSTOMIZE_AVATAR != gAgentCamera.getCameraMode() && LLToolMgr::getInstance()->getCurrentTool() != LLToolPie::getInstance() && gAgent.isInitialized())
    {
        // If the current tool didn't process the click, we should show
        // the pie menu.  This can be done by passing the event to the pie
        // menu tool.
        LLToolPie::getInstance()->handleRightMouseDown(x, y, mask);
    }
}

bool LLViewerWindow::handleAnyMouseClick(LLWindow *window, LLCoordGL pos, MASK mask, EMouseClickType clicktype, bool down, bool& is_toolmgr_action)
{
    const char* buttonname = "";
    const char* buttonstatestr = "";
    S32 x = pos.mX;
    S32 y = pos.mY;
    x = ll_round((F32)x / mDisplayScale.mV[VX]);
    y = ll_round((F32)y / mDisplayScale.mV[VY]);

    // A button press: picks made while handling it take only a GPU answer to their exact
    // ray, waiting for the frames in flight once if needed, else the CPU pick (#702,
    // llcleancursorpick.h).
    LLCleanCursorPick::ClickScope pick_click(down);

    // Right-click health (always on, lltoolpie.cpp): where this press went and whether a menu opened.
    struct RightPressGuard
    {
        bool on;
        ~RightPressGuard() { if (on) LLRightClickHealth::pressEnd(); }
    } rc_guard{ down && clicktype == CLICK_RIGHT };
    if (rc_guard.on) LLRightClickHealth::pressBegin();

    // Handle non-consuming global keybindings, like voice.
    // Middle-mouse navigation (#652): a middle PRESS is held back (it is push-to-talk in the stock
    // bindings) until we know it is not the start of a camera drag; the guard runs the bindings on
    // the way out unless SLMMBNav claims the press below. Its release belongs to SLMMBNav while a
    // press is being decided (it replays the bindings itself).
    struct MMBDeferGuard
    {
        bool pending;
        MASK mask;
        ~MMBDeferGuard()
        {
            if (pending) gViewerInput.handleGlobalBindsMouse(CLICK_MIDDLE, mask, true);
        }
    } mmb_guard{ clicktype == CLICK_MIDDLE && down && SLMMBNav::isEnabled(), mask };
    if (!mmb_guard.pending && !(clicktype == CLICK_MIDDLE && !down && SLMMBNav::isActive()))
    {
        gViewerInput.handleGlobalBindsMouse(clicktype, mask, down);
    }

    // only send mouse clicks to UI if UI is visible
    if(gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {

        if (down)
        {
            buttonstatestr = "down" ;
        }
        else
        {
            buttonstatestr = "up" ;
        }

        switch (clicktype)
        {
        case CLICK_LEFT:
            mLeftMouseDown = down;
            buttonname = "Left";
            break;
        case CLICK_RIGHT:
            mRightMouseDown = down;
            buttonname = "Right";
            break;
        case CLICK_MIDDLE:
            mMiddleMouseDown = down;
            buttonname = "Middle";
            break;
        case CLICK_DOUBLELEFT:
            mLeftMouseDown = down;
            buttonname = "Left Double Click";
            break;
        case CLICK_BUTTON4:
            buttonname = "Button 4";
            break;
        case CLICK_BUTTON5:
            buttonname = "Button 5";
            break;
        default:
            break; // COUNT and NONE
        }

        LLView::sMouseHandlerMessage.clear();

        if (gMenuBarView)
        {
            // stop ALT-key access to menu
            gMenuBarView->resetMenuTrigger();
        }

        if (gDebugClicks)
        {
            LL_INFOS() << "ViewerWindow " << buttonname << " mouse " << buttonstatestr << " at " << x << "," << y << LL_ENDL;
        }

        // Make sure we get a corresponding mouseup event, even if the mouse leaves the window
        if (down)
            mWindow->captureMouse();
        else
            mWindow->releaseMouse();

        // Indicate mouse was active
        LLUI::getInstance()->resetMouseIdleTimer();

        // Don't let the user move the mouse out of the window until mouse up.
        if( LLToolMgr::getInstance()->getCurrentTool()->clipMouseWhenDown() )
        {
            mWindow->setMouseClipping(down);
        }

        LLMouseHandler* mouse_captor = gFocusMgr.getMouseCapture();
        if (mouse_captor && down && (clicktype == CLICK_LEFT || clicktype == CLICK_DOUBLELEFT) && (mask & MASK_ALT)
            && mouse_captor == LLToolPie::getInstance() && LLToolPie::getInstance()->rightPressPending())
        {
            // A right press that has not moved yet is only holding the mouse: it must not
            // swallow the Alt / Ctrl+Alt / Ctrl+Alt+Shift camera tools. Drop the press (no
            // drag, no menu) and let the click go to the tool the modifiers select.
            LLToolPie::getInstance()->cancelRightPress();
            mouse_captor = gFocusMgr.getMouseCapture();
        }
        if( mouse_captor )
        {
            S32 local_x;
            S32 local_y;
            mouse_captor->screenPointToLocal( x, y, &local_x, &local_y );
            if (LLView::sDebugMouseHandling)
            {
                LL_INFOS() << buttonname << " Mouse " << buttonstatestr << " handled by captor " << mouse_captor->getName() << LL_ENDL;
            }

            bool r = mouse_captor->handleAnyMouseClick(local_x, local_y, mask, clicktype, down);
            if (rc_guard.on) LLRightClickHealth::route(r ? "mouse captor took it" : "mouse captor declined", mouse_captor->getName());
            if (r) {

                LL_DEBUGS() << "LLViewerWindow::handleAnyMouseClick viewer with mousecaptor calling updatemouseeventinfo - local_x|global x  "<< local_x << " " << x  << "local/global y " << local_y << " " << y << LL_ENDL;

                LLViewerEventRecorder::instance().setMouseGlobalCoords(x,y);
                LLViewerEventRecorder::instance().logMouseEvent(std::string(buttonstatestr),std::string(buttonname));

            }
            else if (down && clicktype == CLICK_RIGHT)
            {
                handlePieMenu(x, y, mask);
                r = true;
            }
            return r;
        }

        // Mark the click as handled and return if we aren't within the root view to avoid spurious bugs
        if( !mRootView->pointInView(x, y) )
        {
            return true;
        }
        // Give the UI views a chance to process the click

        bool r= mRootView->handleAnyMouseClick(x, y, mask, clicktype, down) ;
        if (r && rc_guard.on) LLRightClickHealth::route("UI view took it", std::string());
        if (r)
        {

            LL_DEBUGS() << "LLViewerWindow::handleAnyMouseClick calling updatemouseeventinfo - global x  "<< " " << x   << "global y " << y  << "buttonstate: " << buttonstatestr << " buttonname " << buttonname << LL_ENDL;

            LLViewerEventRecorder::instance().setMouseGlobalCoords(x,y);

            // Clear local coords - this was a click on root window so these are not needed
            // By not including them, this allows the test skeleton generation tool to be smarter when generating code
            // the code generator can be smarter because when local coords are present it can try the xui path with local coords
            // and fallback to global coordinates only if needed.
            // The drawback to this approach is sometimes a valid xui path will appear to work fine, but NOT interact with the UI element
            // (VITA support not implemented yet or not visible to VITA due to widget further up xui path not being visible to VITA)
            // For this reason it's best to provide hints where possible here by leaving out local coordinates
            LLViewerEventRecorder::instance().setMouseLocalCoords(-1,-1);
            LLViewerEventRecorder::instance().logMouseEvent(buttonstatestr,buttonname);

            if (LLView::sDebugMouseHandling)
            {
                LL_INFOS() << buttonname << " Mouse " << buttonstatestr << " " << LLViewerEventRecorder::instance().get_xui()   << LL_ENDL;
            }
            return true;
        } else if (LLView::sDebugMouseHandling)
            {
                LL_INFOS() << buttonname << " Mouse " << buttonstatestr << " not handled by view" << LL_ENDL;
            }
    }

    // Middle-mouse navigation (#652) with the UI hidden: no captor routing above, so its release comes here.
    if (clicktype == CLICK_MIDDLE && !down && SLMMBNav::isActive())
    {
        SLMMBNav::getInstance()->handleMiddleMouseUp(x, y, mask);
        return true;
    }

    // Pose Studio IK handles (#530): a click on a handle wins over the world (any tool).
    if (!gDisconnected && SLPoseIK::instanceExists() && SLPoseIK::instance().handleMouseClick(x, y, mask, clicktype, down))
    {
        LLViewerEventRecorder::instance().clear_xui();
        return true;
    }

    // Middle-mouse navigation (#652): the press reached the world (no UI, Pose Studio handle or captor took it).
    if (mmb_guard.pending && !gDisconnected && SLMMBNav::getInstance()->tryBegin(x, y, mask))
    {
        mmb_guard.pending = false;
        LLViewerEventRecorder::instance().clear_xui();
        return true;
    }

    // Do not allow tool manager to handle mouseclicks if we have disconnected
    if(!gDisconnected && LLToolMgr::getInstance()->getCurrentTool()->handleAnyMouseClick( x, y, mask, clicktype, down ) )
    {
        if (rc_guard.on) LLRightClickHealth::route("current tool took it", LLToolMgr::getInstance()->getCurrentTool()->getName());
        LLViewerEventRecorder::instance().clear_xui();
        is_toolmgr_action = true;
        return true;
    }

    if (down && clicktype == CLICK_RIGHT)
    {
        handlePieMenu(x, y, mask);
        return true;
    }

    // If we got this far on a down-click, it wasn't handled.
    // Up-clicks, though, are always handled as far as the OS is concerned.
    bool default_rtn = !down;
    return default_rtn;
}

bool LLViewerWindow::handleMouseDown(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    mAllowMouseDragging = false;
    if (!mMouseDownTimer.getStarted())
    {
        mMouseDownTimer.start();
    }
    else
    {
        mMouseDownTimer.reset();
    }
    bool down = true;
    //handleMouse() loops back to LLViewerWindow::handleAnyMouseClick
    return gViewerInput.handleMouse(window, pos, mask, CLICK_LEFT, down);
}

bool LLViewerWindow::handleDoubleClick(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    // try handling as a double-click first, then a single-click if that
    // wasn't handled.
    bool down = true;
    if (gViewerInput.handleMouse(window, pos, mask, CLICK_DOUBLELEFT, down))
    {
        return true;
    }
    return handleMouseDown(window, pos, mask);
}

bool LLViewerWindow::handleMouseUp(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    if (mMouseDownTimer.getStarted())
    {
        mMouseDownTimer.stop();
    }
    bool down = false;
    return gViewerInput.handleMouse(window, pos, mask, CLICK_LEFT, down);
}
bool LLViewerWindow::handleRightMouseDown(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    bool down = true;
    return gViewerInput.handleMouse(window, pos, mask, CLICK_RIGHT, down);
}

bool LLViewerWindow::handleRightMouseUp(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    bool down = false;
    return gViewerInput.handleMouse(window, pos, mask, CLICK_RIGHT, down);
}

bool LLViewerWindow::handleMiddleMouseDown(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    bool down = true;
    gViewerInput.handleMouse(window, pos, mask, CLICK_MIDDLE, down);

    // Always handled as far as the OS is concerned.
    return true;
}

// [SL:KB] - Patch: Build-DragNDrop | Checked: 2013-07-22 (Catznip-3.6)
LLWindowCallbacks::DragNDropResult LLViewerWindow::handleDragNDrop(LLWindow *window, LLCoordGL pos, MASK mask, LLWindowCallbacks::DragNDropAction action,
                                                                   LLWindowCallbacks::DragNDropType type, const std::vector<std::string>& data)
{
    LLWindowCallbacks::DragNDropResult result = LLWindowCallbacks::DND_NONE;
    switch (type)
    {
        case LLWindowCallbacks::DNDT_FILE:
            result = handleDragNDropFile(window, pos, mask, action, type, data);
            break;
        case LLWindowCallbacks::DNDT_DEFAULT:
        default:
            result = handleDragNDropDefault(window, pos, mask, action, (!data.empty()) ? data.front() : LLStringUtil::null);
            break;
    }
    return result;
}
// [/SL:KB]

//LLWindowCallbacks::DragNDropResult LLViewerWindow::handleDragNDrop( LLWindow *window, LLCoordGL pos, MASK mask, LLWindowCallbacks::DragNDropAction action, std::string data)
// [SL:KB] - Patch: Build-DragNDrop | Checked: 2013-07-22 (Catznip-3.6)
LLWindowCallbacks::DragNDropResult LLViewerWindow::handleDragNDropDefault(LLWindow* window, LLCoordGL pos, MASK mask, LLWindowCallbacks::DragNDropAction action, std::string data)
// [/SL:KB]
{
    LLWindowCallbacks::DragNDropResult result = LLWindowCallbacks::DND_NONE;

    const bool prim_media_dnd_enabled = gSavedSettings.getBOOL("PrimMediaDragNDrop");
    const bool slurl_dnd_enabled = gSavedSettings.getBOOL("SLURLDragNDrop");

    if ( prim_media_dnd_enabled || slurl_dnd_enabled )
    {
        switch(action)
        {
            // Much of the handling for these two cases is the same.
            case LLWindowCallbacks::DNDA_TRACK:
            case LLWindowCallbacks::DNDA_DROPPED:
            case LLWindowCallbacks::DNDA_START_TRACKING:
            {
                bool drop = (LLWindowCallbacks::DNDA_DROPPED == action);

                if (slurl_dnd_enabled)
                {
                    LLSLURL dropped_slurl(data);
                    if(dropped_slurl.isSpatial())
                    {
                        if (drop)
                        {
                            LLURLDispatcher::dispatch( dropped_slurl.getSLURLString(), LLCommandHandler::NAV_TYPE_CLICKED, NULL, true );
                            return LLWindowCallbacks::DND_MOVE;
                        }
                        return LLWindowCallbacks::DND_COPY;
                    }
                }

                if (prim_media_dnd_enabled)
                {
                    LLPickInfo pick_info = pickImmediate( pos.mX, pos.mY,
                                                          true /* pick_transparent */,
                                                          false /* pick_rigged */);

                    S32 object_face = pick_info.mObjectFace;
                    std::string url = data;

                    LL_DEBUGS() << "Object: picked at " << pos.mX << ", " << pos.mY << " - face = " << object_face << " - URL = " << url << LL_ENDL;

                    LLVOVolume *obj = dynamic_cast<LLVOVolume*>(static_cast<LLViewerObject*>(pick_info.getObject()));

                    if (obj && !obj->getRegion()->getCapability("ObjectMedia").empty())
                    {
                        LLTextureEntry *te = obj->getTE(object_face);

                        // can modify URL if we can modify the object or we have navigate permissions
                        bool allow_modify_url = obj->permModify() || (te && obj->hasMediaPermission( te->getMediaData(), LLVOVolume::MEDIA_PERM_INTERACT ));

                        if (te && allow_modify_url )
                        {
                            if (drop)
                            {
                                // object does NOT have media already
                                if ( ! te->hasMedia() )
                                {
                                    // we are allowed to modify the object
                                    if ( obj->permModify() )
                                    {
                                        // Create new media entry
                                        LLSD media_data;
                                        // XXX Should we really do Home URL too?
                                        media_data[LLMediaEntry::HOME_URL_KEY] = url;
                                        media_data[LLMediaEntry::CURRENT_URL_KEY] = url;
                                        media_data[LLMediaEntry::AUTO_PLAY_KEY] = true;
                                        obj->syncMediaData(object_face, media_data, true, true);
                                        // XXX This shouldn't be necessary, should it ?!?
                                        if (obj->getMediaImpl(object_face))
                                            obj->getMediaImpl(object_face)->navigateReload();
                                        obj->sendMediaDataUpdate();

                                        result = LLWindowCallbacks::DND_COPY;
                                    }
                                }
                                else
                                // object HAS media already
                                {
                                    // URL passes the whitelist
                                    if (te->getMediaData()->checkCandidateUrl( url ) )
                                    {
                                        // just navigate to the URL
                                        if (obj->getMediaImpl(object_face))
                                        {
                                            obj->getMediaImpl(object_face)->navigateTo(url);
                                        }
                                        else
                                        {
                                            // This is very strange.  Navigation should
                                            // happen via the Impl, but we don't have one.
                                            // This sends it to the server, which /should/
                                            // trigger us getting it.  Hopefully.
                                            LLSD media_data;
                                            media_data[LLMediaEntry::CURRENT_URL_KEY] = url;
                                            obj->syncMediaData(object_face, media_data, true, true);
                                            obj->sendMediaDataUpdate();
                                        }
                                        result = LLWindowCallbacks::DND_LINK;

                                    }
                                }
                                LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
                                mDragHoveredObject = NULL;

                            }
                            else
                            {
                                // Check the whitelist, if there's media (otherwise just show it)
                                if (te->getMediaData() == NULL || te->getMediaData()->checkCandidateUrl(url))
                                {
                                    if ( obj != mDragHoveredObject.get())
                                    {
                                        // Highlight the dragged object
                                        LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
                                        mDragHoveredObject = obj;
                                        LLSelectMgr::getInstance()->highlightObjectOnly(mDragHoveredObject);
                                    }
                                    result = (! te->hasMedia()) ? LLWindowCallbacks::DND_COPY : LLWindowCallbacks::DND_LINK;

                                }
                            }
                        }
                    }
                }
            }
            break;

            case LLWindowCallbacks::DNDA_STOP_TRACKING:
                // The cleanup case below will make sure things are unhilighted if necessary.
            break;
        }

        if (prim_media_dnd_enabled &&
            result == LLWindowCallbacks::DND_NONE && !mDragHoveredObject.isNull())
        {
            LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
            mDragHoveredObject = NULL;
        }
    }

    return result;
}

// [SL:KB] - Patch: Build-DragNDrop | Checked: 2013-07-22 (Catznip-3.6)
// This really should be standardized somewhere instead of having extensions duplicated all over the code
typedef std::tuple<const char*, LLAssetType::EType, LLFilePicker::ELoadFilter> dragdrop_type_lookup_t;
static dragdrop_type_lookup_t s_DragDropTypesLookup[] = {
    std::make_tuple("bmp", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("jpg", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("jpeg", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("png", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("tga", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("webp", LLAssetType::AT_TEXTURE, LLFilePicker::FFLOAD_IMAGE),
    std::make_tuple("bvh", LLAssetType::AT_ANIMATION, LLFilePicker::FFLOAD_ANIM),
    std::make_tuple("anim", LLAssetType::AT_ANIMATION, LLFilePicker::FFLOAD_ANIM),
    std::make_tuple("wav", LLAssetType::AT_SOUND_WAV, LLFilePicker::FFLOAD_WAV),
    std::make_tuple("ogg", LLAssetType::AT_SOUND_WAV, LLFilePicker::FFLOAD_WAV),
    std::make_tuple("mp3", LLAssetType::AT_SOUND_WAV, LLFilePicker::FFLOAD_WAV),
    std::make_tuple("opus", LLAssetType::AT_SOUND_WAV, LLFilePicker::FFLOAD_WAV),
    std::make_tuple("flac", LLAssetType::AT_SOUND_WAV, LLFilePicker::FFLOAD_WAV),
    std::make_tuple("dae", LLAssetType::AT_MESH, LLFilePicker::FFLOAD_MODEL),
    std::make_tuple("fbx", LLAssetType::AT_MESH, LLFilePicker::FFLOAD_MODEL),
    std::make_tuple("gltf", LLAssetType::AT_MATERIAL, LLFilePicker::FFLOAD_GLTF),
    std::make_tuple("glb", LLAssetType::AT_MATERIAL, LLFilePicker::FFLOAD_GLTF),
};

static LLAssetType::EType getAssetTypeFromFilename(const std::string strFilename)
{
    const std::string& strExt = gDirUtilp->getExtension(strFilename);
    for (int idxLookup = 0, cntLookup = sizeof(s_DragDropTypesLookup) / sizeof(dragdrop_type_lookup_t); idxLookup < cntLookup; idxLookup++)
    {
        if (strExt == std::get<0>(s_DragDropTypesLookup[idxLookup]))
            return std::get<1>(s_DragDropTypesLookup[idxLookup]);
    }
    return LLAssetType::AT_NONE;
}

static LLFilePicker::ELoadFilter getLoadFilterFromFilename(const std::string strFilename)
{
    const std::string& strExt = gDirUtilp->getExtension(strFilename);
    for (int idxLookup = 0, cntLookup = sizeof(s_DragDropTypesLookup) / sizeof(dragdrop_type_lookup_t); idxLookup < cntLookup; idxLookup++)
    {
        if (strExt == std::get<0>(s_DragDropTypesLookup[idxLookup]))
            return std::get<2>(s_DragDropTypesLookup[idxLookup]);
    }
    return LLFilePicker::FFLOAD_ALL;
}

LLWindowCallbacks::DragNDropResult LLViewerWindow::handleDragNDropFile(LLWindow *window, LLCoordGL pos, MASK mask, LLWindowCallbacks::DragNDropAction action,
                                                                       LLWindowCallbacks::DragNDropType type, const std::vector<std::string>& data)
{
    bool fDrop = (LLWindowCallbacks::DNDA_DROPPED == action);

    LLWindowCallbacks::DragNDropResult result = LLWindowCallbacks::DND_NONE;
    switch (action)
    {
        case LLWindowCallbacks::DNDA_START_TRACKING:
            {
                mDragItems.clear();
                if (gLoggedInTime.getStarted())
                {
                    for (std::vector<std::string>::const_iterator itFile = data.begin(); itFile != data.end(); ++itFile)
                    {
                        const std::string& strFile = *itFile;

                        LLAssetType::EType eAssetType = getAssetTypeFromFilename(strFile);
                        if (LLAssetType::AT_NONE != eAssetType)
                        {
                            LLPointer<LLViewerInventoryItem> pItem = new LLViewerInventoryItem(LLUUID::generateNewID(), LLUUID::null, gDirUtilp->getBaseFileName(strFile), LLInventoryType::defaultForAssetType(eAssetType));
                            pItem->setType(eAssetType);
                            mDragItems.push_back(drag_item_t(pItem, strFile));
                        }
                    }
                }

                if (!mDragItems.empty())
                {
                    result = LLWindowCallbacks::DND_LINK;
                }
            }
            // Fall through to tracking and dropping for now
        case LLWindowCallbacks::DNDA_TRACK:
        case LLWindowCallbacks::DNDA_DROPPED:
            {
                // Route a file drop landing on the Local Assets floater into its tabs,
                // ahead of the world/upload paths (works in or out of build mode). `pos`
                // is in raw GL device pixels but UI rects are in scaled UI coords, so
                // convert before the hit-test (mirrors the mouse path, see ~line 1032).
                if (!mDragItems.empty())
                {
                    LLFloater* la = LLFloaterReg::findInstance("local_assets");
                    const S32 ui_x = ll_round((F32)pos.mX / mDisplayScale.mV[VX]);
                    const S32 ui_y = ll_round((F32)pos.mY / mDisplayScale.mV[VY]);
                    if (la && la->isInVisibleChain() && la->calcScreenRect().pointInRect(ui_x, ui_y))
                    {
                        result = LLWindowCallbacks::DND_COPY;
                        if (fDrop)
                        {
                            std::vector<std::string> files;
                            for (const drag_item_t& dragItem : mDragItems)
                            {
                                files.push_back(dragItem.second);
                            }
                            if (LLFloaterLocalAssets* laf = dynamic_cast<LLFloaterLocalAssets*>(la))
                            {
                                laf->dropFiles(files);
                            }
                        }
                        break; // handled by the floater
                    }
                }

                if (!LLToolMgr::getInstance()->inBuildMode())
                {
                    if (!mDragItems.empty())
                    {
                        if (fDrop)
                        {
                            bool fCanBulkUpload = (mDragItems.size() > 1) && (std::all_of(mDragItems.begin(), mDragItems.end(), [](const drag_item_t& dragItem)
                                                                                                                                {
                                                                                                                                    auto asset_type = getAssetTypeFromFilename(dragItem.second);
                                                                                                                                    return (asset_type == LLAssetType::AT_TEXTURE)
                                                                                                                                        || (asset_type == LLAssetType::AT_ANIMATION)
                                                                                                                                        || (asset_type == LLAssetType::AT_SOUND_WAV)
                                                                                                                                        || (asset_type == LLAssetType::AT_MATERIAL);
                                                                                                                                }));
                            if (!fCanBulkUpload)
                            {
                                if (gAgentCamera.cameraMouselook())
                                {
                                    gAgentCamera.changeCameraToDefault();
                                }

                                for (const drag_item_t& dragItem : mDragItems)
                                {
                                    std::vector<std::string> tmpvec;
                                    tmpvec.push_back(dragItem.second);
                                    upload_single_file(tmpvec, getLoadFilterFromFilename(dragItem.second), LLUUID::null);
                                }
                            }
                            else
                            {
                                std::vector<std::string> files;
                                for (const drag_item_t& dragItem : mDragItems)
                                    files.push_back(dragItem.second);

                                upload_bulk(files, LLFilePicker::FFLOAD_ALL, true, LLUUID::null);
                            }
                        }
                        result = DND_COPY;
                    }
                }
                else
                {
                    if ( (1 == mDragItems.size()) && (LLAssetType::AT_TEXTURE == mDragItems.front().first->getType() || LLAssetType::AT_MATERIAL == mDragItems.front().first->getType()) )
                    {
                        LLPickInfo pick = pickImmediate(pos.mX, pos.mY, true);

                        LLViewerObject* pObj = pick.getObject();
                        if ( (pObj) && (pObj->permModify()) )
                        {
                            result = LLWindowCallbacks::DND_LINK;
                            if (!fDrop)
                            {
                                if (pObj != mDragHoveredObject.get())
                                {
                                    LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
                                    mDragHoveredObject = pObj;
                                    LLSelectMgr::getInstance()->highlightObjectOnly(mDragHoveredObject);
                                }
                            }
                            else
                            {
                                LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
                                mDragHoveredObject = NULL;

                                const std::string& strFilename = mDragItems.front().second;

                                if (LLAssetType::AT_TEXTURE == mDragItems.front().first->getType())
                                {
                                    LLUUID idLocalBitmap = LLLocalBitmapMgr::instance().getUnitID(strFilename, false);
                                    if (idLocalBitmap.isNull())
                                    {
                                        idLocalBitmap = LLLocalBitmapMgr::instance().addUnit(strFilename);
                                    }

                                    if (idLocalBitmap.notNull())
                                    {
                                        pObj->setTETexture(pick.mObjectFace, LLLocalBitmapMgr::instance().getWorldID(idLocalBitmap));
                                        pObj->sendTEUpdate();
                                    }
                                }
                                else if (LLAssetType::AT_MATERIAL == mDragItems.front().first->getType())
                                {
                                    LLUUID isLocalMat = LLLocalGLTFMaterialMgr::instance().getUnitID(strFilename, 0, false);
                                    if (isLocalMat.isNull())
                                    {
                                        LLLocalGLTFMaterialMgr::instance().addUnit(strFilename, isLocalMat);
                                    }

                                    if (isLocalMat.notNull())
                                    {
                                        pObj->setRenderMaterialID(pick.mObjectFace, LLLocalGLTFMaterialMgr::instance().getWorldID(isLocalMat));
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;
        case LLWindowCallbacks::DNDA_STOP_TRACKING:
            {
                mDragItems.clear();
            }
            break;
    }

    if ( (LLWindowCallbacks::DND_NONE == result) && (mDragHoveredObject.notNull()) )
    {
        LLSelectMgr::getInstance()->unhighlightObjectOnly(mDragHoveredObject);
        mDragHoveredObject = NULL;
    }
    return result;
}
// [/SL:KB]

bool LLViewerWindow::handleMiddleMouseUp(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    bool down = false;
    gViewerInput.handleMouse(window, pos, mask, CLICK_MIDDLE, down);

    // Always handled as far as the OS is concerned.
    return true;
}

bool LLViewerWindow::handleOtherMouse(LLWindow *window, LLCoordGL pos, MASK mask, S32 button, bool down)
{
    switch (button)
    {
    case 4:
        gViewerInput.handleMouse(window, pos, mask, CLICK_BUTTON4, down);
        break;
    case 5:
        gViewerInput.handleMouse(window, pos, mask, CLICK_BUTTON5, down);
        break;
    default:
        break;
    }

    // Always handled as far as the OS is concerned.
    return true;
}

bool LLViewerWindow::handleOtherMouseDown(LLWindow *window, LLCoordGL pos, MASK mask, S32 button)
{
    return handleOtherMouse(window, pos, mask, button, true);
}

bool LLViewerWindow::handleOtherMouseUp(LLWindow *window, LLCoordGL pos, MASK mask, S32 button)
{
    return handleOtherMouse(window, pos, mask, button, false);
}

// WARNING: this is potentially called multiple times per frame
void LLViewerWindow::handleMouseMove(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    S32 x = pos.mX;
    S32 y = pos.mY;

    x = ll_round((F32)x / mDisplayScale.mV[VX]);
    y = ll_round((F32)y / mDisplayScale.mV[VY]);

    mMouseInWindow = true;

    // Save mouse point for access during idle() and display()

    LLCoordGL mouse_point(x, y);

    if (mouse_point != mCurrentMousePoint)
    {
        LLUI::getInstance()->resetMouseIdleTimer();
    }

    saveLastMouse(mouse_point);

    mWindow->showCursorFromMouseMove();

    if (!gDisconnected)
    {
        if (gAwayTimer.getElapsedTimeF32() > LLAgent::MIN_AFK_TIME)
    {
        gAgent.clearAFK();
    }
        else
        {
            gAwayTriggerTimer.reset();
        }
    }
}

void LLViewerWindow::handleMouseDragged(LLWindow *window,  LLCoordGL pos, MASK mask)
{
    if (mMouseDownTimer.getStarted())
    {
        if (mMouseDownTimer.getElapsedTimeF32() > 0.1)
        {
            mAllowMouseDragging = true;
            mMouseDownTimer.stop();
        }
    }
    if(mAllowMouseDragging || !LLToolCamera::getInstance()->hasMouseCapture())
    {
        handleMouseMove(window, pos, mask);
    }
}

void LLViewerWindow::handleMouseLeave(LLWindow *window)
{
    // Note: we won't get this if we have captured the mouse.
    llassert( gFocusMgr.getMouseCapture() == NULL );
    mMouseInWindow = false;
    LLToolTipMgr::instance().blockToolTips();
}

bool LLViewerWindow::handleCloseRequest(LLWindow *window)
{
    if (!LLApp::isExiting() && !LLApp::isStopped())
    {
        // User has indicated they want to close, but we may need to ask
        // about modified documents.
        LLAppViewer::instance()->userQuit();
        // Don't quit immediately
    }
    return false;
}

void LLViewerWindow::handleQuit(LLWindow *window)
{
    if (gNonInteractive)
    {
        LLAppViewer::instance()->requestQuit();
    }
    else
    {
        LL_INFOS() << "Window forced quit" << LL_ENDL;
        LLAppViewer::instance()->forceQuit();
    }
}

void LLViewerWindow::handleResize(LLWindow *window,  S32 width,  S32 height)
{
    reshape(width, height);
    mResDirty = true;
}

// The top-level window has gained focus (e.g. via ALT-TAB)
void LLViewerWindow::handleFocus(LLWindow *window)
{
    gFocusMgr.setAppHasFocus(true);
    LLModalDialog::onAppFocusGained();

    gAgent.onAppFocusGained();
    LLToolMgr::getInstance()->onAppFocusGained();

    // See if we're coming in with modifier keys held down
    if (gKeyboard)
    {
        gKeyboard->resetMaskKeys();
    }

    // resume foreground running timer
    // since we artifically limit framerate when not frontmost
    gForegroundTime.unpause();

    // #919: exclusive fullscreen is re-acquired on focus gain (released on loss)
    LLVulkanContext::instance().onWindowFocusChanged(true);
}

// The top-level window has lost focus (e.g. via ALT-TAB)
void LLViewerWindow::handleFocusLost(LLWindow *window)
{
    gFocusMgr.setAppHasFocus(false);
    //LLModalDialog::onAppFocusLost();
    LLToolMgr::getInstance()->onAppFocusLost();
    gFocusMgr.setMouseCapture( NULL );

    if (gMenuBarView)
    {
        // stop ALT-key access to menu
        gMenuBarView->resetMenuTrigger();
    }

    // restore mouse cursor
    showCursor();
    getWindow()->setMouseClipping(false);

    // If losing focus while keys are down, handle them as
    // an 'up' to correctly release states, then reset states
    if (gKeyboard)
    {
        gKeyboard->resetKeyDownAndHandle();
        gKeyboard->resetKeys();
    }

    // pause timer that tracks total foreground running time
    gForegroundTime.pause();

    // #919: alt-tab out of exclusive fullscreen gives the display back
    LLVulkanContext::instance().onWindowFocusChanged(false);
}


bool LLViewerWindow::handleTranslatedKeyDown(KEY key,  MASK mask, bool repeated)
{
    // Handle non-consuming global keybindings, like voice
    // Never affects event processing.
    gViewerInput.handleGlobalBindsKeyDown(key, mask);

    if (gAwayTimer.getElapsedTimeF32() > LLAgent::MIN_AFK_TIME)
    {
        gAgent.clearAFK();
    }
    else
    {
        gAwayTriggerTimer.reset();
    }

    // *NOTE: We want to interpret KEY_RETURN later when it arrives as
    // a Unicode char, not as a keydown.  Otherwise when client frame
    // rate is really low, hitting return sends your chat text before
    // it's all entered/processed.
    if (key == KEY_RETURN && mask == MASK_NONE)
    {
        // RIDER: although, at times some of the controlls (in particular the CEF viewer
        // would like to know about the KEYDOWN for an enter key... so ask and pass it along.
        LLFocusableElement* keyboard_focus = gFocusMgr.getKeyboardFocus();
        if (keyboard_focus && !keyboard_focus->wantsReturnKey())
            return false;
    }

    // #919 diagnosis: function keys as the window delivered them (F11 not reaching fullscreen).
    if (key >= KEY_F1 && key <= KEY_F12 && !repeated)
    {
        LL_INFOS("Window") << "Key down: F" << (S32)(key - KEY_F1 + 1) << " mask 0x" << std::hex << mask << std::dec
                           << " (before the input remap)" << LL_ENDL;
    }

    // remaps, handles ignored cases and returns back to viewer window.
    return gViewerInput.handleKey(key, mask, repeated);
}

bool LLViewerWindow::handleTranslatedKeyUp(KEY key,  MASK mask)
{
    // Handle non-consuming global keybindings, like voice
    // Never affects event processing.
    gViewerInput.handleGlobalBindsKeyUp(key, mask);

    // Let the inspect tool code check for ALT key to set LLToolSelectRect active instead LLToolCamera
    LLToolCompInspect * tool_inspectp = LLToolCompInspect::getInstance();
    if (LLToolMgr::getInstance()->getCurrentTool() == tool_inspectp)
    {
        tool_inspectp->keyUp(key, mask);
    }

    return gViewerInput.handleKeyUp(key, mask);
}

void LLViewerWindow::handleScanKey(KEY key, bool key_down, bool key_up, bool key_level)
{
    LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
    gViewerInput.scanKey(key, key_down, key_up, key_level);
    return; // Be clear this function returns nothing
}




bool LLViewerWindow::handleActivate(LLWindow *window, bool activated)
{
    // #919: minimised (activated false) / restored: exclusive fullscreen released / re-acquired
    LLVulkanContext::instance().onWindowMinimized(!activated);
    if (activated)
    {
        mActive = true;
        send_agent_resume();
        gAgent.clearAFK();

        // Unmute audio
        audio_update_volume();
    }
    else
    {
        mActive = false;

        // if the user has chosen to go Away automatically after some time, then go Away when minimizing
        static LLCachedControl<S32> afk_time(gSavedSettings, "AFKTimeout", 300);
        if (afk_time())
        {
            gAgent.setAFK();
        }

        // SL-53351: Make sure we're not in mouselook when minimised, to prevent control issues
        // <SS:Skoomastream>
        //if (gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK)
        if (gAgentCamera.cameraMouselook()) // also true in OTS
        // </SS:Skoomastream>
        {
            gAgentCamera.changeCameraToDefault();
        }

        send_agent_pause();

        // Mute audio
        audio_update_volume();
    }
    return true;
}

bool LLViewerWindow::handleActivateApp(LLWindow *window, bool activating)
{
    //if (!activating) gAgentCamera.changeCameraToDefault();

    LLViewerJoystick::getInstance()->setNeedsReset(true);
    return false;
}


void LLViewerWindow::handleMenuSelect(LLWindow *window,  S32 menu_item)
{
}


bool LLViewerWindow::handlePaint(LLWindow *window,  S32 x,  S32 y, S32 width,  S32 height)
{
    return false;
}


void LLViewerWindow::handleScrollWheel(LLWindow *window,  S32 clicks)
{
    handleScrollWheel( clicks );
}

void LLViewerWindow::handleScrollHWheel(LLWindow *window,  S32 clicks)
{
    handleScrollHWheel(clicks);
}

// Smooth scrolling / camera zoom (#727): the window hands over each wheel / touchpad event with
// its fractional deltas. Scrollbars move by the fraction (in pixels, eased; LLScrollbar::
// sWheelDelta) while every other widget still gets the whole steps; sub-notch events reach only
// scrolling widgets. The camera zooms by the fraction and eases it (LLAgentCamera).
bool LLViewerWindow::handleScrollWheelSmooth(LLWindow *window, F32 dy, F32 dx, S32 clicks_y, S32 clicks_x)
{
    static LLCachedControl<bool> s_scroll(gSavedSettings, "UISmoothScroll", true);
    static LLCachedControl<bool> s_zoom(gSavedSettings, "CameraSmoothZoom", true);
    if (!s_scroll && !s_zoom) return false;
    if (LLAppViewer::instance()->quitRequested()) return true;

    if (dy != 0.f)
    {
        mCameraWheelFractional = s_zoom;
        mCameraWheelDelta = dy;
        if (s_scroll) LLScrollbar::sWheelDelta = dy;
        if (clicks_y != 0) handleScrollWheel(clicks_y);
        else if (s_scroll || s_zoom) handleScrollWheelFraction(dy, false);
        LLScrollbar::sWheelDelta = 0.f;
        mCameraWheelFractional = false;
    }
    if (dx != 0.f)
    {
        if (s_scroll) LLScrollbar::sHWheelDelta = dx;
        if (clicks_x != 0) handleScrollHWheel(clicks_x);
        else if (s_scroll) handleScrollWheelFraction(dx, true);
        LLScrollbar::sHWheelDelta = 0.f;
    }
    return true;
}

// A wheel event too small for a whole step (touchpad, high-resolution wheel): the nearest
// scrolling widget under the cursor takes it by pixels; failing that, the camera zooms.
bool LLViewerWindow::handleScrollWheelFraction(F32 notches, bool horizontal)
{
    LLUI::getInstance()->resetMouseIdleTimer();
    // Menus, popups and drags keep to whole steps.
    if (gFocusMgr.getMouseCapture() || gFocusMgr.getTopCtrl()) return false;

    static LLCachedControl<bool> s_scroll(gSavedSettings, "UISmoothScroll", true);
    if (s_scroll)
    {
        const S32 x = mCurrentMousePoint.mX, y = mCurrentMousePoint.mY;
        LLView* v = mRootView->childFromPoint(x, y, true);
        for (; v; v = v->getParent())
        {
            if (!v->getVisible()) continue;
            if (LLScrollContainer* sc = dynamic_cast<LLScrollContainer*>(v))
            {
                if (sc->handleScrollWheelFraction(notches, horizontal)) return true;
            }
            else if (LLScrollListCtrl* sl = dynamic_cast<LLScrollListCtrl*>(v))
            {
                if (sl->handleScrollWheelFraction(notches, horizontal)) return true;
            }
        }
    }

    if (!horizontal && mCameraWheelFractional
        && getWorldViewRectScaled().pointInRect(mCurrentMousePoint.mX, mCurrentMousePoint.mY)
        && gAgentCamera.isInitialized())
    {
        gAgentCamera.handleScrollWheel(mCameraWheelDelta);
        return true;
    }
    return false;
}

void LLViewerWindow::handleWindowBlock(LLWindow *window)
{
    send_agent_pause();
}

void LLViewerWindow::handleWindowUnblock(LLWindow *window)
{
    send_agent_resume();
}

void LLViewerWindow::handleDataCopy(LLWindow *window, S32 data_type, void *data)
{
    const S32 SLURL_MESSAGE_TYPE = 0;
    switch (data_type)
    {
    case SLURL_MESSAGE_TYPE:
        // received URL
        std::string url = (const char*)data;
        LLMediaCtrl* web = NULL;
        const bool trusted_browser = false;
        // don't treat slapps coming from external browsers as "clicks" as this would bypass throttling
        if (LLURLDispatcher::dispatch(url, LLCommandHandler::NAV_TYPE_EXTERNAL, web, trusted_browser))
        {
            // bring window to foreground, as it has just been "launched" from a URL
            mWindow->bringToFront();
        }
        break;
    }
}

// static
void LLViewerWindow::flashTaskbarIfUnfocused(const char* setting_name)
{
    if (!gViewerWindow || !setting_name || !gSavedSettings.getBOOL(setting_name))
    {
        return;
    }
    if (gAgent.isDoNotDisturb())
    {
        return;
    }
    LLWindow* window = gViewerWindow->getWindow();
    if (window && !window->getFocused())
    {
        window->flashIcon(0.f); // until the window regains focus
    }
}

bool LLViewerWindow::handleTimerEvent(LLWindow *window)
{
    //TODO: just call this every frame from gatherInput instead of using a convoluted 30fps timer callback
    if (LLViewerJoystick::getInstance()->getOverrideCamera())
    {
        LLViewerJoystick::getInstance()->updateStatus();
        return true;
    }
    return false;
}

bool LLViewerWindow::handleDeviceChange(LLWindow *window)
{
    // give a chance to use a joystick after startup (hot-plugging)
    if (!LLViewerJoystick::getInstance()->isJoystickInitialized() )
    {
        LLViewerJoystick::getInstance()->init(true);
        return true;
    }
    return false;
}

bool LLViewerWindow::handleDPIChanged(LLWindow *window, F32 ui_scale_factor, S32 window_width, S32 window_height)
{
    LLFontGL::sResolutionGeneration++;
    if (ui_scale_factor >= MIN_UI_SCALE && ui_scale_factor <= MAX_UI_SCALE)
    {
        LLViewerWindow::reshape(window_width, window_height);
        mResDirty = true;
        return true;
    }
    else
    {
        LL_WARNS() << "DPI change caused UI scale to go out of bounds: " << ui_scale_factor << LL_ENDL;
        return false;
    }
}

bool LLViewerWindow::handleDisplayChanged()
{
    LLFontGL::sResolutionGeneration++;
    return false;
}

bool LLViewerWindow::handleWindowDidChangeScreen(LLWindow *window)
{
    LLCoordScreen window_rect;
    mWindow->getSize(&window_rect);
    reshape(window_rect.mX, window_rect.mY);
    return true;
}

// #919 ---------------------------------------------------------------------------------
// FullScreenMode: 0 = automatic, 1 = borderless, 2 = exclusive.
//
// AUTOMATIC picks whichever is best for performance on the platform:
//  - Windows: EXCLUSIVE (application-controlled VK_EXT_full_screen_exclusive) where the
//    device and the monitor support it. The swapchain then owns the display: it flips
//    straight to scan-out with no DWM composition (no compositor frame of latency, no
//    copy), and IMMEDIATE / MAILBOX present as on a dedicated display. A borderless window
//    only gets that when the driver decides to promote it to independent flip, which
//    overlays, other windows on top or HDR / multi-monitor setups prevent. Without the
//    extension (or on a monitor that refuses it): BORDERLESS.
//  - Linux: BORDERLESS (SDL desktop fullscreen). Compositors (KWin, Mutter, wlroots-based,
//    picom...) unredirect / directly scan out a fullscreen window that covers the output,
//    so a borderless window already skips composition, without a display mode switch
//    (no flicker, instant alt-tab, no lost display settings on a crash). Wayland has no
//    exclusive mode at all, and an X11 mode set at the desktop mode buys nothing more.
// The setting is live (change it while fullscreen to A/B the two), and every switch logs
// the decision ("Fullscreen: mode auto -> exclusive (VK_EXT_full_screen_exclusive)").
// static
bool LLViewerWindow::resolveFullscreenExclusive(std::string* why)
{
    const U32 mode = gSavedSettings.getU32("FullScreenMode");
    bool exclusive = false;
    std::string text;
    if (mode == 1)
    {
        text = "borderless (FullScreenMode 1)";
    }
    else if (mode == 2)
    {
        exclusive = true;
#if LL_WINDOWS
        text = LLVulkanContext::instance().fullScreenExclusiveAvailable()
            ? "exclusive (FullScreenMode 2, VK_EXT_full_screen_exclusive)"
            : "exclusive (FullScreenMode 2; VK_EXT_full_screen_exclusive not available: SDL display-mode fullscreen)";
#else
        text = "exclusive (FullScreenMode 2, SDL display-mode fullscreen)";
#endif
    }
    else
    {
#if LL_WINDOWS
        exclusive = LLVulkanContext::instance().fullScreenExclusiveAvailable();
        text = exclusive ? "auto -> exclusive (VK_EXT_full_screen_exclusive)"
                         : "auto -> borderless (VK_EXT_full_screen_exclusive not available on this device / monitor)";
#else
        text = "auto -> borderless (SDL desktop fullscreen; the compositor scans out a fullscreen window directly)";
#endif
    }
    if (why)
    {
        *why = text;
    }
    return exclusive;
}

bool LLViewerWindow::getFullscreen() const
{
    return mWindow && mWindow->getFullscreen();
}

void LLViewerWindow::setFullscreen(bool fullscreen)
{
    if (!mWindow)
    {
        return;
    }
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (fullscreen)
    {
        std::string why;
        const bool exclusive = resolveFullscreenExclusive(&why);
        LL_INFOS("Window") << "Fullscreen: mode " << why << LL_ENDL;
#if LL_WINDOWS
        // Windows exclusive = a borderless window covering the monitor + exclusive Vulkan
        // presentation (what the extension expects); SDL's own display-mode fullscreen only
        // when the extension is not there.
        const bool vk_exclusive = exclusive && vk.fullScreenExclusiveAvailable();
        const bool sdl_exclusive = exclusive && !vk_exclusive;
#else
        const bool vk_exclusive = false;
        const bool sdl_exclusive = exclusive;
#endif
        // Armed BEFORE the window changes, so the swapchain recreate the resize triggers is
        // the exclusive one (one rebuild, not two).
        vk.setFullScreenExclusive(vk_exclusive);
        if (!mWindow->setFullscreen(true, sdl_exclusive))
        {
            LL_WARNS("Window") << "Fullscreen: the window could not enter fullscreen" << LL_ENDL;
            vk.setFullScreenExclusive(false);
        }
    }
    else
    {
        vk.setFullScreenExclusive(false);
        if (!mWindow->setFullscreen(false, false))
        {
            LL_WARNS("Window") << "Fullscreen: the window could not leave fullscreen" << LL_ENDL;
        }
    }
    // Persist the state the window is in (the FullScreen listener comes back here only when
    // the setting differs from the window, so this does not recurse).
    const bool now = mWindow->getFullscreen();
    if (gSavedSettings.getBOOL("FullScreen") != now)
    {
        gSavedSettings.setBOOL("FullScreen", now);
    }
}

void LLViewerWindow::handleFullscreenChanged(LLWindow *window, bool fullscreen)
{
    // The window manager changed it (not setFullscreen): the renderer follows, the setting
    // records it. Exclusive presentation is never kept on a window we did not place.
    if (!fullscreen)
    {
        LLVulkanContext::instance().setFullScreenExclusive(false);
    }
    if (gSavedSettings.getBOOL("FullScreen") != fullscreen)
    {
        gSavedSettings.setBOOL("FullScreen", fullscreen);
    }
}

void LLViewerWindow::handlePingWatchdog(LLWindow *window, const char * msg)
{
    LLAppViewer::instance()->pingMainloopTimeout(msg);
}


void LLViewerWindow::handleResumeWatchdog(LLWindow *window)
{
    LLAppViewer::instance()->resumeMainloopTimeout();
}

void LLViewerWindow::handlePauseWatchdog(LLWindow *window)
{
    LLAppViewer::instance()->pauseMainloopTimeout();
}

//virtual
std::string LLViewerWindow::translateString(const char* tag)
{
    return LLTrans::getString( std::string(tag) );
}

//virtual
std::string LLViewerWindow::translateString(const char* tag,
        const std::map<std::string, std::string>& args)
{
    // LLTrans uses a special subclass of std::string for format maps,
    // but we must use std::map<> in these callbacks, otherwise we create
    // a dependency between LLWindow and LLFormatMapString.  So copy the data.
    LLStringUtil::format_map_t args_copy;
    std::map<std::string,std::string>::const_iterator it = args.begin();
    for ( ; it != args.end(); ++it)
    {
        args_copy[it->first] = it->second;
    }
    return LLTrans::getString( std::string(tag), args_copy);
}

//
// Classes
//
LLViewerWindow::LLViewerWindow(const Params& p)
:   mWindow(NULL),
    mActive(true),
    mUIVisible(true),
    mWindowRectRaw(0, p.height, p.width, 0),
    mWindowRectScaled(0, p.height, p.width, 0),
    mWorldViewRectRaw(0, p.height, p.width, 0),
    mLeftMouseDown(false),
    mMiddleMouseDown(false),
    mRightMouseDown(false),
    mMouseInWindow( false ),
    mAllowMouseDragging(true),
    mMouseDownTimer(),
    mLastMask( MASK_NONE ),
    mToolStored( NULL ),
    mHideCursorPermanent( false ),
    mCursorHidden(false),
    mResDirty(false),
    mStatesDirty(false),
    mProgressView(NULL)
{
    // gKeyboard is still NULL, so it doesn't do LLWindowListener any good to
    // pass its value right now. Instead, pass it a nullary function that
    // will, when we later need it, return the value of gKeyboard.
    // boost::lambda::var() constructs such a functor on the fly.
    mWindowListener.reset(new LLWindowListener(this, boost::lambda::var(gKeyboard)));
    mViewerWindowListener.reset(new LLViewerWindowListener(this));

    mSystemChannel.reset(new LLNotificationChannel("System", "Visible", LLNotificationFilters::includeEverything));
    mCommunicationChannel.reset(new LLCommunicationChannel("Communication", "Visible"));
    mAlertsChannel.reset(new LLNotificationsUI::LLViewerAlertHandler("VW_alerts", "alert"));
    mModalAlertsChannel.reset(new LLNotificationsUI::LLViewerAlertHandler("VW_alertmodal", "alertmodal"));

    bool ignore = gSavedSettings.getBOOL("IgnoreAllNotifications");
    LLNotifications::instance().setIgnoreAllNotifications(ignore);
    if (ignore)
    {
    LL_INFOS() << "NOTE: ALL NOTIFICATIONS THAT OCCUR WILL GET ADDED TO IGNORE LIST FOR LATER RUNS." << LL_ENDL;
    }


    /*
    LLWindowCallbacks* callbacks,
    const std::string& title, const std::string& name, S32 x, S32 y, S32 width, S32 height, U32 flags,
    bool fullscreen,
    bool clearBg,
    bool disable_vsync,
    bool ignore_pixel_depth,
    U32 fsaa_samples)
    */
    // create window

    U32 max_core_count = gSavedSettings.getU32("EmulateCoreCount");
    F32 max_gl_version = gSavedSettings.getF32("RenderMaxOpenGLVersion");

#if LL_LINUX
    // Wayland / X11 choice, read once before SDL initializes (#196/#197).
    set_sdl_video_backend(gSavedSettings.getString("LinuxDisplayBackend"));
#endif

    mWindow = LLWindowManager::createWindow(this,
        p.title, p.name, p.x, p.y, p.width, p.height, 0,
        p.fullscreen,
        false,
        gSavedSettings.getBOOL("RenderVSyncEnable"),
        p.ignore_pixel_depth,
        0,
        max_core_count,
        max_gl_version); //don't use window level anti-aliasing

    if (NULL == mWindow)
    {
        LLSplashScreen::update(LLTrans::getString("StartupRequireDriverUpdate"));

        LL_WARNS("Window") << "Failed to create window, to be shutting Down, be sure your graphics driver is updated." << LL_ENDL ;

        ms_sleep(5000) ; //wait for 5 seconds.

        LLSplashScreen::update(LLTrans::getString("ShuttingDown"));
#if LL_LINUX
        LL_WARNS() << "Unable to create window, be sure screen is set at 32-bit color and your graphics driver is configured correctly.  See README-linux.txt for further information."
                << LL_ENDL;
#else
        LL_WARNS("Window") << "Unable to create window, be sure screen is set at 32-bit color in Control Panels->Display->Settings"
                << LL_ENDL;
#endif
        LLAppViewer::instance()->fastQuit(1);
    }

    if (!LLAppViewer::instance()->restoreErrorTrap())
    {
        // this always happens, so downgrading it to INFO
        LL_INFOS("Window") << " Someone took over my signal/exception handler (post createWindow; normal)" << LL_ENDL;
    }

    const bool do_not_enforce = false;
    mWindow->setMinSize(p.min_width, p.min_height, do_not_enforce);  // root view not set
    LLCoordScreen scr;
    mWindow->getSize(&scr);

    // Reset UI scale factor on first run if OS's display scaling is not 100%
    if (gSavedSettings.getBOOL("ResetUIScaleOnFirstRun"))
    {
        if (mWindow->getSystemUISize() != 1.f)
        {
            gSavedSettings.setF32("UIScaleFactor", 1.f);
        }
        gSavedSettings.setBOOL("ResetUIScaleOnFirstRun", false);
    }

    // Get the real window rect the window was created with (since there are various OS-dependent reasons why
    // the size of a window or fullscreen context may have been adjusted slightly...)
    F32 ui_scale_factor = llclamp(gSavedSettings.getF32("UIScaleFactor") * mWindow->getSystemUISize(), MIN_UI_SCALE, MAX_UI_SCALE);

    mDisplayScale.setVec(llmax(1.f / mWindow->getPixelAspectRatio(), 1.f), llmax(mWindow->getPixelAspectRatio(), 1.f));
    mDisplayScale *= ui_scale_factor;
    LLUI::setScaleFactor(mDisplayScale);
    LLFontGL::sResolutionGeneration++;

    {
        LLCoordWindow size;
        mWindow->getSize(&size);
        mWindowRectRaw.set(0, size.mY, size.mX, 0);
        mWindowRectScaled.set(0, ll_round((F32)size.mY / mDisplayScale.mV[VY]), ll_round((F32)size.mX / mDisplayScale.mV[VX]), 0);
    }

    LLFontManager::initClass();

    // Init font system, load default fonts and generate basic glyphs
    // currently it takes aprox. 0.5 sec and we would load these fonts anyway
    // before login screen.
    LLFontGL::initClass( gSavedSettings.getF32("FontScreenDPI"),
        mDisplayScale.mV[VX],
        mDisplayScale.mV[VY],
        gDirUtilp->getAppRODataDir(), true,
        gSavedSettings.getString("FSFontSettingsFile"),
        llclamp(gSavedSettings.getF32("FSFontSizeAdjustment"), -2.f, 4.f));

    //
    // We want to set this stuff up BEFORE we initialize the pipeline, so we can turn off
    // stuff like AGP if we think that it'll crash the viewer.
    //
    LL_DEBUGS("Window") << "Loading feature tables." << LL_ENDL;

    // If we crashed while initializng GL stuff last time, disable certain features
    if (gSavedSettings.getBOOL("RenderInitError"))
    {
        mInitAlert = "DisplaySettingsNoShaders";
        LLFeatureManager::getInstance()->setGraphicsLevel(0, false);
        gSavedSettings.setU32("RenderQualityPerformance", 0);
    }

    // Init the image list.  Must happen after GL is initialized and before the images that
    // LLViewerWindow needs are requested, as well as before LLViewerMedia starts updating images.
    LLTextureData::initClass(false);
    gTextureList.init();
    LLViewerTextureManager::init() ;

    // Create container for all sub-views
    LLView::Params rvp;
    rvp.name("root");
    rvp.rect(mWindowRectScaled);
    rvp.mouse_opaque(false);
    rvp.follows.flags(FOLLOWS_NONE);
    mRootView = LLUICtrlFactory::create<LLRootView>(rvp);
    LLUI::getInstance()->setRootView(mRootView);

    // Make avatar head look forward at start
    mCurrentMousePoint.mX = getWindowWidthScaled() / 2;
    mCurrentMousePoint.mY = getWindowHeightScaled() / 2;

    gShowOverlayTitle = gSavedSettings.getBOOL("ShowOverlayTitle");
    mOverlayTitle = gSavedSettings.getString("OverlayTitle");
    // Can't have spaces in settings.ini strings, so use underscores instead and convert them.
    LLStringUtil::replaceChar(mOverlayTitle, '_', ' ');

    mDebugText = new LLDebugText(this);

    mWorldViewRectScaled = calcScaledRect(mWorldViewRectRaw, mDisplayScale);
}

std::string LLViewerWindow::getLastSnapshotDir()
{
    static LLCachedControl<std::string> sSnapshotDir(LLCachedControl<std::string>(gSavedPerAccountSettings, "SnapshotBaseDir", ""));
    return sSnapshotDir;
}

void LLViewerWindow::initGLDefaults()
{
    // RN: Need this for translation and stretch manip.
    gBox.prerender();
}

struct MainPanel : public LLPanel
{
};

void LLViewerWindow::initBase()
{
    S32 height = getWindowHeightScaled();
    S32 width = getWindowWidthScaled();

    LLRect full_window(0, height, width, 0);

    ////////////////////
    //
    // Set the gamma
    //

    F32 gamma = gSavedSettings.getF32("RenderGamma");
    if (gamma != 0.0f)
    {
        getWindow()->setGamma(gamma);
    }

    // Create global views

    // Login screen and main_view.xml need edit menus for preferences and browser
    LL_DEBUGS("AppInit") << "initializing edit menu" << LL_ENDL;
    initialize_edit_menu();

    LLFontGL::loadCommonFonts();

    // Create the floater view at the start so that other views can add children to it.
    // (But wait to add it as a child of the root view so that it will be in front of the
    // other views.)
    MainPanel* main_view = new MainPanel();
    if (!main_view->buildFromFile("main_view.xml"))
    {
        LL_ERRS() << "Failed to initialize viewer: Viewer couldn't process file main_view.xml, "
                << "if this problem happens again, please validate your installation." << LL_ENDL;
    }
    main_view->setShape(full_window);
    getRootView()->addChild(main_view);

    // placeholder widget that controls where "world" is rendered
    mWorldViewPlaceholder = main_view->getChildView("world_view_rect")->getHandle();
    mPopupView = main_view->getChild<LLPopupView>("popup_holder");
    mHintHolder = main_view->getChild<LLView>("hint_holder")->getHandle();
    mLoginPanelHolder = main_view->getChild<LLView>("login_panel_holder")->getHandle();
    mStatusBarContainer = main_view->getChild<LLPanel>("status_bar_container");
    mNavBarContainer = mStatusBarContainer->getChild<LLView>("nav_bar_container");
    mTopInfoContainer = main_view->getChild<LLPanel>("topinfo_bar_container");

    // Create the toolbar view
    // Get a pointer to the toolbar view holder
    LLPanel* panel_holder = main_view->getChild<LLPanel>("toolbar_view_holder");
    // Load the toolbar view from file
    gToolBarView = LLUICtrlFactory::getInstance()->createFromFile<LLToolBarView>("panel_toolbar_view.xml", panel_holder, LLDefaultChildRegistry::instance());
    if (!gToolBarView)
    {
        LL_ERRS() << "Failed to initialize viewer: Viewer couldn't process file panel_toolbar_view.xml, "
                << "if this problem happens again, please validate your installation." << LL_ENDL;
    }
    gToolBarView->setShape(panel_holder->getLocalRect());
    // Hide the toolbars for the moment: we'll make them visible after logging in world (see LLViewerWindow::initWorldUI())
    gToolBarView->setVisible(false);

    mFloaterSnapRegion = gToolBarView->getChild<LLView>("floater_snap_region");
    mChicletContainer = gToolBarView->getChild<LLPanel>("chiclet_container");
    // Constrain floaters to inside the menu and status bar regions.
    gFloaterView = main_view->getChild<LLFloaterView>("Floater View");
    for (S32 i = 0; i < LLToolBarEnums::TOOLBAR_COUNT; ++i)
    {
        LLToolBar * toolbarp = gToolBarView->getToolbar((LLToolBarEnums::EToolBarLocation)i);
        if (toolbarp)
        {
            toolbarp->getCenterLayoutPanel()->setReshapeCallback(boost::bind(&LLFloaterView::setToolbarRect, gFloaterView, _1, _2));
        }
    }
    gFloaterView->setFloaterSnapView(mFloaterSnapRegion->getHandle());
    gSnapshotFloaterView = main_view->getChild<LLSnapshotFloaterView>("Snapshot Floater View");

    const F32 CHAT_PERSIST_TIME = 20.f;

    // Console
    llassert( !gConsole );
    LLConsole::Params cp;
    cp.name("console");
    cp.max_lines(gSavedSettings.getS32("ConsoleBufferSize"));
    cp.rect(getChatConsoleRect());
    cp.persist_time(CHAT_PERSIST_TIME);
    cp.font_size_index(gSavedSettings.getS32("ChatFontSize"));
    cp.follows.flags(FOLLOWS_LEFT | FOLLOWS_RIGHT | FOLLOWS_BOTTOM);
    gConsole = LLUICtrlFactory::create<LLConsole>(cp);
    getRootView()->addChild(gConsole);

    cp.name("nearby_chat_console");
    cp.tab_stop(false);
    cp.visible(false);
    mNearbyChatConsole = LLUICtrlFactory::create<LLConsole>(cp);
    getRootView()->addChild(mNearbyChatConsole);
    // <GMDX> the chat log (NearbyChatOverlayStyle "gmdx"), over the whole window; it draws only then
    if (GMDXChatLog* chat_log = GMDXChatLog::create())
    {
        chat_log->setRect(getRootView()->getLocalRect());
        getRootView()->addChild(chat_log);
    }
    // </GMDX>
    // #489: the console style of the nearby chat overlay has its own persist / fade / line count /
    // text size / background settings (Preferences > Chat > Alerts), applied live.
    applyNearbyChatConsoleSettings();
    for (const char* setting : {"NearbyChatConsolePersistTime", "NearbyChatConsoleFadeTime",
                               "NearbyChatConsoleMaxLines", "NearbyChatConsoleFontSize",
                               "NearbyChatConsoleBackground", "ChatFontSize"})
    {
        gSavedSettings.getControl(setting)->getSignal()->connect(
            [](LLControlVariable*, const LLSD&, const LLSD&)
            {
                if (gViewerWindow)
                {
                    gViewerWindow->applyNearbyChatConsoleSettings();
                }
            });
    }
    for (const char* setting : {"NearbyChatOverlayStyle", "NearbyChatOverlayHideWhenVisible",
                               "NotificationNearbyChatOptions", "NotificationObjectIMOptions"})
    {
        gSavedSettings.getControl(setting)->getSignal()->connect(
            [handle = mNearbyChatConsole->getHandle()](LLControlVariable*, const LLSD&, const LLSD&)
            {
                if (handle.isDead())
                {
                    return;
                }
                static_cast<LLConsole*>(handle.get())->clear();
                auto* channel = LLNotificationsUI::LLChannelManager::getInstance()->findChannelByID(
                    LLNotificationsUI::NEARBY_CHAT_CHANNEL_UUID);
                if (channel)
                {
                    channel->removeToastsFromChannel();
                }
            });
    }

    // optionally forward warnings to chat console/chat floater
    // for qa runs and dev builds
#if  !LL_RELEASE_FOR_DOWNLOAD
    RecordToChatConsole::getInstance()->startRecorder();
#else
    if(gSavedSettings.getBOOL("QAMode"))
    {
        RecordToChatConsole::getInstance()->startRecorder();
    }
#endif

    gDebugView = getRootView()->getChild<LLDebugView>("DebugView");
    gDebugView->init();
    gToolTipView = getRootView()->getChild<LLToolTipView>("tooltip view");

    // Initialize do not disturb response message when logged in
    // LLAppViewer::instance()->setOnLoginCompletedCallback(boost::bind(&LLFloaterPreference::initDoNotDisturbResponse));
    //
    // This method was created, as we'll be adding more auto-responses in the future
    // rather than putting multiple callbacks, single callback and address them there.
    // -- Fallen
    LLAppViewer::instance()->setOnLoginCompletedCallback(boost::bind(&LLFloaterPreference::initAutoResponses));

    // Add the progress bar view (startup view), which overrides everything
    mProgressView = getRootView()->findChild<LLProgressView>("progress_view");
    setShowProgress(false);
    setProgressCancelButtonVisible(false);

    gMenuHolder = getRootView()->getChild<LLViewerMenuHolderGL>("Menu Holder");
    LLMenuGL::sMenuContainer = gMenuHolder;
}

void LLViewerWindow::initWorldUI()
{
    if (gNonInteractive)
    {
        gIMMgr = LLIMMgr::getInstance();
        LLNavigationBar::getInstance();
        gFloaterView->pushVisibleAll(false);
        return;
    }

    S32 height = mRootView->getRect().getHeight();
    S32 width = mRootView->getRect().getWidth();
    LLRect full_window(0, height, width, 0);


    gIMMgr = LLIMMgr::getInstance();

    //getRootView()->sendChildToFront(gFloaterView);
    //getRootView()->sendChildToFront(gSnapshotFloaterView);

    if (!gNonInteractive)
    {
        LLChicletBar* chiclet_bar = LLChicletBar::getInstance();
        chiclet_bar->setShape(mChicletContainer->getLocalRect());
        chiclet_bar->setFollowsAll();
        mChicletContainer->addChild(chiclet_bar);
        mChicletContainer->setVisible(true);
    }

    LLRect morph_view_rect = full_window;
    morph_view_rect.stretch( -STATUS_BAR_HEIGHT );
    morph_view_rect.mTop = full_window.mTop - 32;
    LLMorphView::Params mvp;
    mvp.name("MorphView");
    mvp.rect(morph_view_rect);
    mvp.visible(false);
    gMorphView = LLUICtrlFactory::create<LLMorphView>(mvp);
    getRootView()->addChild(gMorphView);

    LLWorldMapView::initClass();

    // Force gFloaterWorldMap to initialize
    LLFloaterReg::getInstance("world_map");

    // Force gFloaterTools to initialize
    LLFloaterReg::getInstance("build");

    LLNavigationBar* navbar = LLNavigationBar::getInstance();
    if (!gStatusBar)
    {
        // Status bar
        gStatusBar = new LLStatusBar(mStatusBarContainer->getLocalRect());
        gStatusBar->setFollows(FOLLOWS_LEFT | FOLLOWS_TOP | FOLLOWS_RIGHT);
        gStatusBar->setShape(mStatusBarContainer->getLocalRect());
        // sync bg color with menu bar
        gStatusBar->setBackgroundColor(gMenuBarView->getBackgroundColor());
        // add InBack so that gStatusBar won't be drawn over menu
        mStatusBarContainer->addChildInBack(gStatusBar, 2/*tab order, after menu*/);
        mStatusBarContainer->setVisible(true);

        // Navigation bar
        navbar->setShape(mNavBarContainer->getLocalRect());
        navbar->setBackgroundColor(gMenuBarView->getBackgroundColor());
        mNavBarContainer->addChild(navbar);
        mNavBarContainer->setVisible(true);
    }
    else
    {
        mStatusBarContainer->setVisible(true);
        mNavBarContainer->setVisible(true);
    }

    if (!gSavedSettings.getBOOL("ShowNavbarNavigationPanel"))
    {
        navbar->setVisible(false);
    }
    else
    {
        reshapeStatusBarContainer();
    }


    // Top Info bar
    LLPanelTopInfoBar* topinfo_bar = LLPanelTopInfoBar::getInstance();
    topinfo_bar->setShape(mTopInfoContainer->getLocalRect());

    mTopInfoContainer->addChild(topinfo_bar);
    mTopInfoContainer->setVisible(true);

    if (!gSavedSettings.getBOOL("ShowMiniLocationPanel"))
    {
        topinfo_bar->setVisible(false);
    }

    if ( gHUDView == NULL )
    {
        LLRect hud_rect = full_window;
        hud_rect.mBottom += 50;
        if (gMenuBarView && gMenuBarView->isInVisibleChain())
        {
            hud_rect.mTop -= gMenuBarView->getRect().getHeight();
        }
        gHUDView = new LLHUDView(hud_rect);
        getRootView()->addChild(gHUDView);
        getRootView()->sendChildToBack(gHUDView);
    }

    LLPanel* panel_ssf_container = gToolBarView->getChild<LLPanel>("state_management_buttons_container");

    LLPanelStandStopFlying* panel_stand_stop_flying = LLPanelStandStopFlying::getInstance();
    panel_ssf_container->addChild(panel_stand_stop_flying);

    LLPanelHideBeacon* panel_hide_beacon = LLPanelHideBeacon::getInstance();
    panel_ssf_container->addChild(panel_hide_beacon);

    panel_ssf_container->setVisible(true);

    LLMenuOptionPathfindingRebakeNavmesh::getInstance()->initialize();

    // Load and make the toolbars visible
    // Note: we need to load the toolbars only *after* the user is logged in and IW
    if (gToolBarView)
    {
        gToolBarView->loadToolbars();
        gToolBarView->setVisible(true);
    }

    if (!gNonInteractive)
    {
        LLMediaCtrl* destinations = LLFloaterReg::getInstance("destinations")->getChild<LLMediaCtrl>("destination_guide_contents");
        if (destinations)
        {
            destinations->setErrorPageURL(gSavedSettings.getString("GenericErrorPageURL"));
            std::string url = gSavedSettings.getString("DestinationGuideURL");
            url = LLWeb::expandURLSubstitutions(url, LLSD());
            destinations->navigateTo(url, HTTP_CONTENT_TEXT_HTML);
        }
        LLMediaCtrl* avatar_welcome_pack = LLFloaterReg::getInstance("avatar_welcome_pack")->findChild<LLMediaCtrl>("avatar_picker_contents");
        if (avatar_welcome_pack)
        {
            avatar_welcome_pack->setErrorPageURL(gSavedSettings.getString("GenericErrorPageURL"));
            std::string url = gSavedSettings.getString("AvatarWelcomePack");
            url = LLWeb::expandURLSubstitutions(url, LLSD());
            avatar_welcome_pack->navigateTo(url, HTTP_CONTENT_TEXT_HTML);
        }
        LLMediaCtrl* search = LLFloaterReg::getInstance("search")->findChild<LLMediaCtrl>("search_contents");
        if (search)
        {
            search->setErrorPageURL(gSavedSettings.getString("GenericErrorPageURL"));
        }
        LLMediaCtrl* marketplace = LLFloaterReg::getInstance("marketplace")->getChild<LLMediaCtrl>("marketplace_contents");
        if (marketplace)
        {
            marketplace->setErrorPageURL(gSavedSettings.getString("GenericErrorPageURL"));
            std::string url = gSavedSettings.getString("MarketplaceURL");
            marketplace->navigateTo(url, HTTP_CONTENT_TEXT_HTML);
        }
    }
}

// Destroy the UI
void LLViewerWindow::shutdownViews()
{
    // clean up warning logger
    RecordToChatConsole::getInstance()->stopRecorder();
    LL_INFOS() << "Warning logger is cleaned." << LL_ENDL ;

    gFocusMgr.unlockFocus();
    gFocusMgr.setMouseCapture(NULL);
    gFocusMgr.setKeyboardFocus(NULL);
    gFocusMgr.setTopCtrl(NULL);
    if (mWindow)
    {
        mWindow->allowLanguageTextInput(NULL, false);
    }

    delete mDebugText;
    mDebugText = NULL;

    LL_INFOS() << "DebugText deleted." << LL_ENDL ;

    // Cleanup global views
    if (gMorphView)
    {
        gMorphView->setVisible(false);
    }
    LL_INFOS() << "Global views cleaned." << LL_ENDL ;

    LLNotificationsUI::LLToast::cleanupToasts();
    LL_INFOS() << "Leftover toast cleaned up." << LL_ENDL;

    // DEV-40930: Clear sModalStack. Otherwise, any LLModalDialog left open
    // will crump with LL_ERRS.
    LLModalDialog::shutdownModals();
    LL_INFOS() << "LLModalDialog shut down." << LL_ENDL;

    // destroy the nav bar, not currently part of gViewerWindow
    // *TODO: Make LLNavigationBar part of gViewerWindow
    LLNavigationBar::deleteSingleton();
    LL_INFOS() << "LLNavigationBar destroyed." << LL_ENDL ;

    // destroy menus after instantiating navbar above, as it needs
    // access to gMenuHolder
    cleanup_menus();
    LL_INFOS() << "menus destroyed." << LL_ENDL ;

    view_listener_t::cleanup();
    LL_INFOS() << "view listeners destroyed." << LL_ENDL ;

    // Clean up pointers that are going to be invalid. (todo: check sMenuContainer)
    mProgressView = NULL;
    mPopupView = NULL;

    // Delete all child views.
    delete mRootView;
    mRootView = NULL;
    mNearbyChatConsole = nullptr;
    LL_INFOS() << "RootView deleted." << LL_ENDL ;

    LLMenuOptionPathfindingRebakeNavmesh::getInstance()->quit();

    // Automatically deleted as children of mRootView.  Fix the globals.
    gStatusBar = NULL;
    gIMMgr = NULL;
    gToolTipView = NULL;

    gToolBarView = NULL;
    gFloaterView = NULL;
    gMorphView = NULL;

    gHUDView = NULL;
}

// shutdownViews() needs to be called first
LLViewerWindow::~LLViewerWindow()
{
    LL_INFOS() << "Destroying Window" << LL_ENDL;
    destroyWindow();

    delete mDebugText;
    mDebugText = NULL;
}


void LLViewerWindow::setCursor( ECursorType c )
{
    mWindow->setCursor( c );
}

void LLViewerWindow::showCursor()
{
    mWindow->showCursor();

    mCursorHidden = false;
}

void LLViewerWindow::hideCursor()
{
    // And hide the cursor
    mWindow->hideCursor();

    mCursorHidden = true;
}

void LLViewerWindow::sendShapeToSim()
{
    LLMessageSystem* msg = gMessageSystem;
    if(!msg) return;
    msg->newMessageFast(_PREHASH_AgentHeightWidth);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addU32Fast(_PREHASH_CircuitCode, gMessageSystem->mOurCircuitCode);
    msg->nextBlockFast(_PREHASH_HeightWidthBlock);
    msg->addU32Fast(_PREHASH_GenCounter, 0);
    U16 height16 = (U16) mWorldViewRectRaw.getHeight();
    U16 width16 = (U16) mWorldViewRectRaw.getWidth();
    msg->addU16Fast(_PREHASH_Height, height16);
    msg->addU16Fast(_PREHASH_Width, width16);
    gAgent.sendReliableMessage();
}

// Must be called after window is created to set up agent
// camera variables and UI variables.
void LLViewerWindow::reshape(S32 width, S32 height)
{
    // Destroying the window at quit time generates spurious
    // reshape messages.  We don't care about these, and we
    // don't want to send messages because the message system
    // may have been destructed.
    if (!LLApp::isExiting())
    {
        gWindowResized = true;

        // update our window rectangle
        mWindowRectRaw.mRight = mWindowRectRaw.mLeft + width;
        mWindowRectRaw.mTop = mWindowRectRaw.mBottom + height;

        // EXPERIMENTAL Vulkan: reshape() is the only place LL learns the window
        // changed size (SDL_EVENT_WINDOW_RESIZED -> handleResize -> here). The GL
        // renderFrame that would normally do this is diverted, so notify the VK
        // context here or the swapchain stays pinned at the launch extent while LL
        // draws the UI at the new size -> blank UI on maximize. onResize() only
        // latches mWidth/mHeight + mNeedsRecreate (idempotent); the frame loop does
        // the actual mSwapchain.recreate() with proper GPU sync, and the recreate
        // clamps to the surface's driver-reported currentExtent. This runs only on
        // genuine window events (not per-frame), past the isExiting guard above.
        if (width > 0 && height > 0)
        {
            LLVulkanContext::instance().onResize((uint32_t)width, (uint32_t)height);
        }

        LLViewerCamera * camera = LLViewerCamera::getInstance(); // simpleton, might not exist
        if (height > 0 && camera)
        {
            camera->setViewHeightInPixels( mWorldViewRectRaw.getHeight() );
            camera->setAspect( getWorldViewAspectRatio() );
        }

        calcDisplayScale();

        bool display_scale_changed = mDisplayScale != LLUI::getScaleFactor();
        LLUI::setScaleFactor(mDisplayScale);
        LLFontGL::sResolutionGeneration++;

        // update our window rectangle
        mWindowRectScaled.mRight = mWindowRectScaled.mLeft + ll_round((F32)width / mDisplayScale.mV[VX]);
        mWindowRectScaled.mTop = mWindowRectScaled.mBottom + ll_round((F32)height / mDisplayScale.mV[VY]);

        setup2DViewport();

        // Inform lower views of the change
        // round up when converting coordinates to make sure there are no gaps at edge of window
        LLView::sForceReshape = display_scale_changed;
        mRootView->reshape(llceil((F32)width / mDisplayScale.mV[VX]), llceil((F32)height / mDisplayScale.mV[VY]));
        if (display_scale_changed)
        {
            // Needs only a 'scale change' update, everything else gets handled by LLLayoutStack::updateClass()
            LLPanelLogin::reshapePanel();
        }
        LLView::sForceReshape = false;

        // clear font width caches
        if (display_scale_changed)
        {
            LLHUDObject::reshapeAll();
        }

        sendShapeToSim();

        // store new settings for the mode we are in, regardless
        // (#919: not while fullscreen -- those are the WINDOWED placement, restored on leaving
        // fullscreen and used by the next windowed start; the fullscreen size would replace it)
        const bool fullscreen = mWindow->getFullscreen();
        bool maximized = mWindow->getMaximized();
        if (!fullscreen)
        {
            gSavedSettings.setBOOL("WindowMaximized", maximized);
        }

        if (!maximized && !fullscreen)
        {
            U32 min_window_width=gSavedSettings.getU32("MinWindowWidth");
            U32 min_window_height=gSavedSettings.getU32("MinWindowHeight");
            // tell the OS specific window code about min window size
            mWindow->setMinSize(min_window_width, min_window_height);

            LLCoordScreen window_rect;
            if (!gNonInteractive && mWindow->getSize(&window_rect))
            {
            // Only save size if not maximized
                gSavedSettings.setU32("WindowWidth", window_rect.mX);
                gSavedSettings.setU32("WindowHeight", window_rect.mY);
            }
        }

        sample(LLStatViewer::WINDOW_WIDTH, width);
        sample(LLStatViewer::WINDOW_HEIGHT, height);

        LLLayoutStack::updateClass();
        updateHudRect();   // #919: the new window / scale's HUD rect before the next frame

        // Moving between monitors fires a burst of size / scale changes, and the
        // notification channels are laid out during it from regions that may not have
        // been reshaped yet. Re-lay them out once the window has settled (#691).
        mOverlayRelayoutFrames = 3;
    }
}


// Hide normal UI when a logon fails
void LLViewerWindow::setNormalControlsVisible( bool visible )
{
    if(LLChicletBar::instanceExists())
    {
        LLChicletBar::getInstance()->setVisible(visible);
        LLChicletBar::getInstance()->setEnabled(visible);
    }

    if ( gMenuBarView )
    {
        gMenuBarView->setVisible( visible );
        gMenuBarView->setEnabled( visible );

        // ...and set the menu color appropriately.
        setMenuBackgroundColor(gAgent.getGodLevel() > GOD_NOT,
            LLGridManager::getInstance()->isInProductionGrid());
    }

    if ( gStatusBar )
    {
        gStatusBar->setVisible( visible );
        gStatusBar->setEnabled( visible );
    }

    if (mNavBarContainer)
    {
        // when it's time to show navigation bar we need to ensure that the user wants to see it
        // i.e. ShowNavbarNavigationPanel option is true
        mNavBarContainer->setVisible( visible && gSavedSettings.getBOOL("ShowNavbarNavigationPanel") );
    }
}

void LLViewerWindow::setMenuBackgroundColor(bool god_mode, bool dev_grid)
{
    LLSD args;
    LLUIColor new_bg_color;

    // god more important than project, proj more important than grid
    if ( god_mode )
    {
        if ( LLGridManager::getInstance()->isInProductionGrid() )
        {
            new_bg_color = LLUIColorTable::instance().getColor( "MenuBarGodBgColor" );
        }
        else
        {
            new_bg_color = LLUIColorTable::instance().getColor( "MenuNonProductionGodBgColor" );
        }
    }
    else
    {
        switch (LLVersionInfo::instance().getViewerMaturity())
        {
        case LLVersionInfo::TEST_VIEWER:
            new_bg_color = LLUIColorTable::instance().getColor( "MenuBarTestBgColor" );
            break;

        case LLVersionInfo::PROJECT_VIEWER:
            new_bg_color = LLUIColorTable::instance().getColor( "MenuBarProjectBgColor" );
            break;

        case LLVersionInfo::BETA_VIEWER:
            new_bg_color = LLUIColorTable::instance().getColor( "MenuBarBetaBgColor" );
            break;

        case LLVersionInfo::RELEASE_VIEWER:
            if(!LLGridManager::getInstance()->isInProductionGrid())
            {
                new_bg_color = LLUIColorTable::instance().getColor( "MenuNonProductionBgColor" );
            }
            else
            {
                new_bg_color = LLUIColorTable::instance().getColor( "MenuBarBgColor" );
            }
            break;
        }
    }

    if(gMenuBarView)
    {
        gMenuBarView->setBackgroundColor( new_bg_color );
    }

    if(gStatusBar)
    {
        gStatusBar->setBackgroundColor( new_bg_color );
    }
}

void LLViewerWindow::drawDebugText()
{
    gGL.color4f(1,1,1,1);
    gGL.pushMatrix();
    gGL.pushUIMatrix();
    {
        // scale view by UI global scale factor and aspect ratio correction factor
        gGL.scaleUI(mDisplayScale.mV[VX], mDisplayScale.mV[VY], 1.f);
        mDebugText->draw();
    }
    gGL.popUIMatrix();
    gGL.popMatrix();

    gGL.flush();
}

void LLViewerWindow::draw()
{

//#if LL_DEBUG
    LLView::sIsDrawing = true;
//#endif

    LLUI::setLineWidth(1.f);

    LLUI::setLineWidth(1.f);
    // Reset any left-over transforms
    gGL.matrixMode(LLRender::MM_MODELVIEW);

    gGL.loadIdentity();

    //S32 screen_x, screen_y;

    if (!LLPipeline::RenderUIBuffer)
    {
        LLView::sDirtyRect = getWindowRectScaled();
    }

    // HACK for timecode debugging
    static LLCachedControl<bool> displayTimecode(gSavedSettings, "DisplayTimecode", false);
    if (displayTimecode)
    {
        // draw timecode block
        std::string text;

        gGL.loadIdentity();

        microsecondsToTimecodeString(gFrameTime,text);
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        font->renderUTF8(text, 0,
                        ll_round((getWindowWidthScaled()/2)-100.f),
                        ll_round((getWindowHeightScaled()-60.f)),
            LLColor4( 1.f, 1.f, 1.f, 1.f ),
            LLFontGL::LEFT, LLFontGL::TOP);
    }

    // Draw all nested UI views.
    // No translation needed, this view is glued to 0,0

    gGL.color4f(1, 1, 1, 1);

    gGL.pushMatrix();
    LLUI::pushMatrix();
    {

        // scale view by UI global scale factor and aspect ratio correction factor
        gGL.scaleUI(mDisplayScale.mV[VX], mDisplayScale.mV[VY], 1.f);

        LLVector2 old_scale_factor = LLUI::getScaleFactor();
        // apply camera zoom transform (for high res screenshots)
        F32 zoom_factor = LLViewerCamera::getInstance()->getZoomFactor();
        S16 sub_region = LLViewerCamera::getInstance()->getZoomSubRegion();
        if (zoom_factor > 1.f)
        {
            //decompose subregion number to x and y values
            int pos_y = sub_region / llceil(zoom_factor);
            int pos_x = sub_region - (pos_y*llceil(zoom_factor));
            // offset for this tile
            gGL.translatef((F32)getWindowWidthScaled() * -(F32)pos_x,
                        (F32)getWindowHeightScaled() * -(F32)pos_y,
                        0.f);
            gGL.scalef(zoom_factor, zoom_factor, 1.f);
            LLUI::getScaleFactor() *= zoom_factor;
        }

        // Draw tool specific overlay on world
        LLToolMgr::getInstance()->getCurrentTool()->draw();

        if (gAgentCamera.isUsingFlycam() && LLGameControl::isFlycamCrosshairEnabled())
        {
            // Same crosshair image/centering LLToolGun::draw() uses for
            // mouselook's "ShowCrosshairs" -- flycam has no LLTool of its own
            // to hang this off of, so it's drawn here instead.
            LLUIImagePtr crosshair = LLUI::getUIImage("crosshairs.tga");
            crosshair->draw(
                ( getWorldViewRectScaled().getWidth() - crosshair->getWidth() ) / 2,
                ( getWorldViewRectScaled().getHeight() - crosshair->getHeight() ) / 2);
        }

        if( gAgentCamera.cameraMouselook())
        {
            drawMouselookInstructions();
        }
        else
        {
            // TPS camera crosshair + release hint
            SLTPSCamera::drawOverlay();
        }

        // <GMDX> Kill feed: text-only overlay, in and out of mouselook
        FSFloaterKillFeed::drawOverlay();
        FSCombatHitMarker::draw(); // <GMDX> hit marker flash and fading hit report

        // Draw all nested UI views.
        // No translation needed, this view is glued to 0,0
        mRootView->draw();

        if (LLView::sDebugRects)
        {
            gToolTipView->drawStickyRect();
        }

        // Draw optional on-top-of-everyone view
        LLUICtrl* top_ctrl = gFocusMgr.getTopCtrl();
        if (top_ctrl && top_ctrl->getVisible())
        {
            S32 screen_x, screen_y;
            top_ctrl->localPointToScreen(0, 0, &screen_x, &screen_y);

            gGL.matrixMode(LLRender::MM_MODELVIEW);
            LLUI::pushMatrix();
            LLUI::translate( (F32) screen_x, (F32) screen_y);
            top_ctrl->draw();
            LLUI::popMatrix();
        }


        if( gShowOverlayTitle && !mOverlayTitle.empty() )
        {
            // Used for special titles such as "Second Life - Special E3 2003 Beta"
            const S32 DIST_FROM_TOP = 20;
            LLFontGL::getFontSansSerifBig()->renderUTF8(
                mOverlayTitle, 0,
                ll_round( getWindowWidthScaled() * 0.5f),
                getWindowHeightScaled() - DIST_FROM_TOP,
                LLColor4(1, 1, 1, 0.4f),
                LLFontGL::HCENTER, LLFontGL::TOP);
        }

        // Freeze World indicator (its menu shortcut). Part of the UI, so UI-less snapshots
        // and hidden-UI photography never carry it.
        if (LLWorldClock::userFrozen())
        {
            const S32 DIST_FROM_TOP = 64;
            LLStringUtil::format_map_t args;
            args["[KEY]"] = SLKeyCommands::instance().getMenuBindingString("viewer", "Freeze World");
            LLFontGL::getFontSansSerifBig()->renderUTF8(
                LLTrans::getString(args["[KEY]"].length() == 0 ? "WorldFrozenNoKey" : "WorldFrozen", args), 0,
                (F32)ll_round(getWindowWidthScaled() * 0.5f),
                (F32)(getWindowHeightScaled() - DIST_FROM_TOP),
                LLColor4(0.55f, 0.85f, 1.f, 0.95f),
                LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
        }

        LLUI::setScaleFactor(old_scale_factor);
    }
    LLUI::popMatrix();
    gGL.popMatrix();

    gGL.flush();

    LLView::sIsDrawing = false;
}

// Takes a single keyup event, usually when UI is visible
bool LLViewerWindow::handleKeyUp(KEY key, MASK mask)
{
    if (LLSetKeyBindDialog::recordKey(key, mask, false))
    {
        LL_DEBUGS() << "KeyUp handled by LLSetKeyBindDialog" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key, mask);
        return true;
    }

    LLFocusableElement* keyboard_focus = gFocusMgr.getKeyboardFocus();

    if (keyboard_focus
        && !(mask & (MASK_CONTROL | MASK_ALT))
        && !gFocusMgr.getKeystrokesOnly())
    {
        // We have keyboard focus, and it's not an accelerator
        if (keyboard_focus && keyboard_focus->wantsKeyUpKeyDown())
        {
            return keyboard_focus->handleKeyUp(key, mask, false);
        }
        else if (key < 0x80)
        {
            // Not a special key, so likely (we hope) to generate a character.  Let it fall through to character handler first.
            return (gFocusMgr.getKeyboardFocus() != NULL);
        }
    }

    if (keyboard_focus)
    {
        if (keyboard_focus->handleKeyUp(key, mask, false))
        {
            LL_DEBUGS() << "LLviewerWindow::handleKeyUp - in 'traverse up' - no loops seen... just called keyboard_focus->handleKeyUp an it returned true" << LL_ENDL;
            LLViewerEventRecorder::instance().logKeyEvent(key, mask);
            return true;
        }
        else {
            LL_DEBUGS() << "LLviewerWindow::handleKeyUp - in 'traverse up' - no loops seen... just called keyboard_focus->handleKeyUp an it returned false" << LL_ENDL;
        }
    }

    // Try for a new-format gesture
    if (LLGestureMgr::instance().triggerGestureRelease(key, mask))
    {
        LL_DEBUGS() << "LLviewerWindow::handleKey new gesture release feature" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }
    //Old format gestures do not support this, so no need to implement it.

    // don't pass keys on to world when something in ui has focus
    return gFocusMgr.childHasKeyboardFocus(mRootView)
        || LLMenuGL::getKeyboardMode()
        || (gMenuBarView && gMenuBarView->getHighlightedItem() && gMenuBarView->getHighlightedItem()->isActive());
}

// Takes a single keydown event, usually when UI is visible
// <GMDX> A shortcut pressed in mouselook opens its window as well (Ctrl+P, L). Mouselook blocks floaters
// from opening so nothing pops up mid-fight; a window the user asked for is not a pop-up.
namespace
{
    struct ShortcutShowsFloaters
    {
        ShortcutShowsFloaters() { LLFloaterReg::blockShowFloaters(false); }
        // Afterwards blocked exactly while the floaters are hidden for mouselook (push/popVisibleAll
        // are the only other callers). Putting back the block from before the key went wrong both
        // ways: M out of mouselook left every menu unable to open a window, M into it left the
        // floaters free to pop up.
        ~ShortcutShowsFloaters() { LLFloaterReg::blockShowFloaters(gFloaterView && gFloaterView->isVisiblePushed()); }
    };
}
// </GMDX>

bool LLViewerWindow::handleKey(KEY key, MASK mask)
{
    // hide tooltips on keypress
    LLToolTipMgr::instance().blockToolTips();

    // UI hidden (Advanced > Rendering Features > UI, Ctrl+Alt+F1): Escape brings it
    // back. On Linux Ctrl+Alt+F1 switches the virtual console and never reaches us,
    // so without this there was no way back short of a restart.
    if (SLKeyCommands::instance().matches("restore_ui", key, mask)
        && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        return true;
    }

    // Menus get handled on key down instead of key up
    // so keybindings have to be recorded before that
    if (LLSetKeyBindDialog::recordKey(key, mask, true))
    {
        LL_DEBUGS() << "Key handled by LLSetKeyBindDialog" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }

    // #919: the fullscreen key (F11 by default, rebindable) is taken here, before the
    // focus chain and the gesture/menu accelerator tail that the key used to depend on:
    // a focused chat bar or login field and the panels above it saw the key first, and the
    // menu accelerator is the last stop. A gesture the user bound to the same key keeps
    // the key (so user gestures are never broken). A key that types text (no Ctrl/Alt, not
    // a function or other special key) is left to the menu accelerator while a text field
    // has focus, so a letter the user rebinds fullscreen to does not eat typing.
    {
        LLFocusableElement* focus_now = gFocusMgr.getKeyboardFocus();
        const bool types_text = key < 0x80 && !(mask & (MASK_CONTROL | MASK_ALT));
        // #919 diagnosis: whether a function key reaches here and what the fullscreen lookup says.
        if (key >= KEY_F1 && key <= KEY_F12 && !gKeyboard->getKeyRepeated(key))
        {
            LL_INFOS("Window") << "Key at handleKey: F" << (S32)(key - KEY_F1 + 1) << " mask 0x" << std::hex << mask << std::dec
                               << " fullscreen viewer=" << SLKeyCommands::instance().matchesMenuItem("viewer", "Fullscreen", key, mask)
                               << " login=" << SLKeyCommands::instance().matchesMenuItem("login", "Fullscreen", key, mask)
                               << " gesture=" << LLGestureMgr::instance().hasKeyTrigger(key, mask)
                               << " focus=" << (focus_now ? 1 : 0) << LL_ENDL;
        }
        const bool fullscreen_key = (!focus_now || !types_text)
            && (SLKeyCommands::instance().matchesMenuItem("viewer", "Fullscreen", key, mask)
                || SLKeyCommands::instance().matchesMenuItem("login", "Fullscreen", key, mask));
        const bool gesture_owns = fullscreen_key && LLGestureMgr::instance().hasKeyTrigger(key, mask);
        if (gesture_owns && !gKeyboard->getKeyRepeated(key))
        {
            // Say once per key (log) and once per session (hint) why the shortcut did nothing
            static std::set<U32> s_logged_keys;
            static bool s_hinted = false;
            if (s_logged_keys.insert(((U32)mask << 16) | (U32)key).second)
            {
                const std::string key_name = LLKeyboard::stringFromAccelerator(mask, key);
                const std::string gesture_name = LLGestureMgr::instance().getKeyTriggerName(key, mask);
                LL_INFOS("Window") << "Fullscreen key " << key_name << " skipped: owned by gesture '"
                                   << gesture_name << "'" << LL_ENDL;
                if (!s_hinted)
                {
                    s_hinted = true;
                    LLSD args;
                    args["KEY"] = key_name;
                    args["NAME"] = gesture_name;
                    LLNotificationsUtil::add("GestureOwnsKey", args);
                }
            }
        }
        if (fullscreen_key && !gesture_owns)
        {
            if (!gKeyboard->getKeyRepeated(key))
            {
                LL_INFOS("Window") << "Fullscreen: toggled by key "
                                   << LLKeyboard::stringFromAccelerator(mask, key) << LL_ENDL;
                toggleFullscreen();
            }
            LLViewerEventRecorder::instance().logKeyEvent(key, mask);
            return true;
        }
    }

    LLFocusableElement* keyboard_focus = gFocusMgr.getKeyboardFocus();

    if (keyboard_focus
        && !gFocusMgr.getKeystrokesOnly())
    {
#if 0 // THIS BREAKS SO MUCH - RYE
        //Most things should fall through, but mouselook is an exception,
        //don't switch to mouselook if any floater has focus
        if ((key == KEY_MOUSELOOK) && !(mask & (MASK_CONTROL | MASK_ALT)))
        {
            return true;
        }

        LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(keyboard_focus);
        if (cur_focus && cur_focus->acceptsTextInput())
#endif
        {
#if defined(LL_WINDOWS) && !LL_SDL_WINDOW
            // On windows Alt Gr key generates additional Ctrl event, as result handling situations
            // like 'AltGr + D' will result in 'Alt+Ctrl+D'. If it results in WM_CHAR, don't let it
            // pass into menu or it will trigger 'develop' menu assigned to this combination on top
            // of character handling.
            // Alt Gr can be additionally modified by Shift
            const MASK alt_gr = MASK_CONTROL | MASK_ALT;
            LLWindowWin32 *window = static_cast<LLWindowWin32*>(mWindow);
            U32 raw_key = window->getRawWParam();
            if ((mask & alt_gr) != 0
                && ((raw_key >= 0x30 && raw_key <= 0x5A) //0-9, plus normal chartacters
                    || (raw_key >= 0xBA && raw_key <= 0xE4)) // Misc/OEM characters that can be covered by AltGr, ex: -, =, ~
                && (GetKeyState(VK_RMENU) & 0x8000) != 0
                && (GetKeyState(VK_RCONTROL) & 0x8000) == 0) // ensure right control is not pressed, only left one
            {
                // Alt Gr key is represented as right alt and left control.
                // Any alt+ctrl combination is treated as Alt Gr by TranslateMessage() and
                // will generate a WM_CHAR message, but here we only treat virtual Alt Graph
                // key by checking if this specific combination has unicode char.
                //
                // I decided to handle only virtual RAlt+LCtrl==AltGr combination to minimize
                // impact on menu, but the right way might be to handle all Alt+Ctrl calls.

                BYTE keyboard_state[256];
                if (GetKeyboardState(keyboard_state))
                {
                    const int char_count = 6;
                    wchar_t chars[char_count];
                    HKL layout = GetKeyboardLayout(0);
                    // ToUnicodeEx changes buffer state on OS below Win10, which is undesirable,
                    // but since we already did a TranslateMessage() in gatherInput(), this
                    // should have no negative effect
                    // ToUnicodeEx works with virtual key codes
                    int res = ToUnicodeEx(raw_key, 0, keyboard_state, chars, char_count, 1 << 2 /*do not modify buffer flag*/, layout);
                    if (res == 1 && chars[0] >= 0x20)
                    {
                        // Let it fall through to character handler and get a WM_CHAR.
                        return true;
                    }
                }
            }
#endif

            if (!(mask & (MASK_CONTROL | MASK_ALT)))
            {
                // We have keyboard focus, and it's not an accelerator
                if (keyboard_focus && keyboard_focus->wantsKeyUpKeyDown())
                {
                    return keyboard_focus->handleKey(key, mask, false);
                }
                else if (key < 0x80)
                {
                    // Not a special key, so likely (we hope) to generate a character.  Let it fall through to character handler first.
                    return true;
                }
            }
        }
    }

    // let menus handle navigation keys for navigation
    if ((gMenuBarView && gMenuBarView->handleKey(key, mask, true))
        ||(gLoginMenuBarView && gLoginMenuBarView->handleKey(key, mask, true))
        ||(gMenuHolder && gMenuHolder->handleKey(key, mask, true)))
    {
        LL_DEBUGS() << "LLviewerWindow::handleKey handle nav keys for nav" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }


    // give menus a chance to handle modified (Ctrl, Alt) shortcut keys before current focus
    // as long as focus isn't locked
    if (mask & (MASK_CONTROL | MASK_ALT) && !gFocusMgr.focusLocked())
    {
        ShortcutShowsFloaters shortcut_shows_floaters; // <GMDX>
        // Check the current floater's menu first, if it has one.
        if (gFocusMgr.keyboardFocusHasAccelerators()
            && keyboard_focus
            && keyboard_focus->handleKey(key,mask,false))
        {
            LLViewerEventRecorder::instance().logKeyEvent(key,mask);
            return true;
        }

        if (gAgent.isInitialized()
            && (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE || gAgent.getTeleportState() == LLAgent::TELEPORT_LOCAL)
            && gMenuBarView
            && gMenuBarView->handleAcceleratorKey(key, mask))
        {
            LLViewerEventRecorder::instance().logKeyEvent(key, mask);
            return true;
        }

        if (gLoginMenuBarView && gLoginMenuBarView->handleAcceleratorKey(key, mask))
        {
            LLViewerEventRecorder::instance().logKeyEvent(key,mask);
            return true;
        }
    }

    // give floaters first chance to handle TAB key
    // so frontmost floater gets focus
    // if nothing has focus, go to first or last UI element as appropriate
    // (Ctrl+Tab and Ctrl+Shift+Tab are rebindable commands, #863; plain Tab with nothing
    // focused moves the focus into the UI and stays Tab)
    const bool cycle_next = SLKeyCommands::instance().matches("cycle_floaters_next", key, mask);
    const bool cycle_prev = SLKeyCommands::instance().matches("cycle_floaters_prev", key, mask);
    if (cycle_next || cycle_prev || (key == KEY_TAB && keyboard_focus == NULL))
    {
        LL_WARNS() << "LLviewerWindow::handleKey give floaters first chance at tab key " << LL_ENDL;
        if (gMenuHolder) gMenuHolder->hideMenus();

        // if cycling windows (and not just TAB with no focus), go into window cycle mode
        gFloaterView->setCycleMode(cycle_next || cycle_prev);

        // do next/previous window (CTRL-TAB and CTRL-SHIFT-TAB by default) logic
        if (cycle_prev || (!cycle_next && (mask & MASK_SHIFT)))
        {
            mRootView->focusPrevRoot();
        }
        else
        {
            mRootView->focusNextRoot();
        }
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }
    // hidden edit menu for cut/copy/paste
    if (gEditMenu && gEditMenu->handleAcceleratorKey(key, mask))
    {
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }

    LLFloater* focused_floaterp = gFloaterView->getFocusedFloater();
    std::string focusedFloaterName = (focused_floaterp ? focused_floaterp->getInstanceName() : "");

    if( keyboard_focus )
    {
        if ((focusedFloaterName == "chatbar") || (focusedFloaterName == "nearby_chat") || (focusedFloaterName == "im_container") || (focusedFloaterName == "impanel"))
        {
            static LLCachedControl<bool> key_move(gSavedSettings, "ArrowKeysAlwaysMove");
            if (key_move())
            {
                // let Control-Up and Control-Down through for chat line history,
                if (!(key == KEY_UP && mask == MASK_CONTROL)
                    && !(key == KEY_DOWN && mask == MASK_CONTROL)
                    && !(key == KEY_UP && mask == MASK_ALT)
                    && !(key == KEY_DOWN && mask == MASK_ALT))
                {
                    switch(key)
                    {
                    case KEY_LEFT:
                    case KEY_RIGHT:
                    case KEY_UP:
                    case KEY_DOWN:
                    case KEY_PAGE_UP: //jump
                    case KEY_PAGE_DOWN: // down
                    case KEY_HOME: // toggle fly
                        // when chatbar is empty or ArrowKeysAlwaysMove set,
                        // pass arrow keys on to avatar...
                        return false;
                    default:
                        break;
                    }
                }
        }
        }

        if (keyboard_focus->handleKey(key, mask, false))
        {

            LL_DEBUGS() << "LLviewerWindow::handleKey - in 'traverse up' - no loops seen... just called keyboard_focus->handleKey an it returned true" << LL_ENDL;
            LLViewerEventRecorder::instance().logKeyEvent(key,mask);
            return true;
        } else {
            LL_DEBUGS() << "LLviewerWindow::handleKey - in 'traverse up' - no loops seen... just called keyboard_focus->handleKey an it returned false" << LL_ENDL;
        }
    }

    if( LLToolMgr::getInstance()->getCurrentTool()->handleKey(key, mask) )
    {
        LL_DEBUGS() << "LLviewerWindow::handleKey toolbar handling?" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }

    // Try for a new-format gesture
    if (LLGestureMgr::instance().triggerGesture(key, mask))
    {
        LL_DEBUGS() << "LLviewerWindow::handleKey new gesture feature" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }

    // See if this is a gesture trigger.  If so, eat the key and
    // don't pass it down to the menus.
    if (gGestureList.trigger(key, mask))
    {
        LL_DEBUGS() << "LLviewerWindow::handleKey check gesture trigger" << LL_ENDL;
        LLViewerEventRecorder::instance().logKeyEvent(key,mask);
        return true;
    }

    // If "Pressing letter keys starts local chat" option is selected, we are not in mouselook,
    // no view has keyboard focus, this is a printable character key (and no modifier key is
    // pressed except shift), then give focus to nearby chat (STORM-560)
    if ( LLStartUp::getStartupState() >= STATE_STARTED &&
        gSavedSettings.getS32("LetterKeysFocusChatBar") && !gAgentCamera.cameraMouselook() &&
        !keyboard_focus && key < 0x80 && (mask == MASK_NONE || mask == MASK_SHIFT) )
    {
        static LLCachedControl<bool> sChatInWindow(gSavedSettings, "SlipstreamNearbyChatInput", true);
        if (!sChatInWindow)
        {
            // Initialize nearby chat if it's missing
            LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
            if (!nearby_chat)
            {
                LLSD name("im_container");
                LLFloaterReg::toggleInstanceOrBringToFront(name);
                // Now try to find it again since the container should have spawned it
                nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
            }

            LLChatEntry* chat_editor = nearby_chat ? nearby_chat->getChatBox() : nullptr;
            if (chat_editor)
            {
                // passing nullptr here, character will be added later when it is handled by character handler.
                nearby_chat->startChat(nullptr);
                return true;
            }
        }
        else
        {
            // Nearby chat bar (SLChatBar): character will be added later by the character handler.
            SLChatBar::startChat(nullptr);
            return true;
        }
    }

    // give menus a chance to handle unmodified accelerator keys
    ShortcutShowsFloaters shortcut_shows_floaters; // <GMDX>
    if (gAgent.isInitialized()
        && (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE || gAgent.getTeleportState() == LLAgent::TELEPORT_LOCAL)
        && gMenuBarView
        && gMenuBarView->handleAcceleratorKey(key, mask))
    {
        LLViewerEventRecorder::instance().logKeyEvent(key, mask);
        return true;
    }

    if (gLoginMenuBarView && gLoginMenuBarView->handleAcceleratorKey(key, mask))
    {
        return true;
    }

    // don't pass keys on to world when something in ui has focus
    return gFocusMgr.childHasKeyboardFocus(mRootView)
        || LLMenuGL::getKeyboardMode()
        || (gMenuBarView && gMenuBarView->getHighlightedItem() && gMenuBarView->getHighlightedItem()->isActive());
}


bool LLViewerWindow::handleUnicodeChar(llwchar uni_char, MASK mask)
{
    // <SS:GameControl> the character of a key the key binding dialog just recorded
    if (LLSetKeyBindDialog::consumesTypedChar())
    {
        return true;
    }

    // HACK:  We delay processing of return keys until they arrive as a Unicode char,
    // so that if you're typing chat text at low frame rate, we don't send the chat
    // until all keystrokes have been entered. JC
    // HACK: Numeric keypad <enter> on Mac is Unicode 3
    // HACK: Control-M on Windows is Unicode 13
    if ((uni_char == 13 && mask != MASK_CONTROL)
        || (uni_char == 3 && mask == MASK_NONE) )
    {
        if (mask != MASK_ALT)
        {
            // remaps, handles ignored cases and returns back to viewer window.
            return gViewerInput.handleKey(KEY_RETURN, mask, gKeyboard->getKeyRepeated(KEY_RETURN));
        }
    }

    // let menus handle navigation (jump) keys
    if (gMenuBarView && gMenuBarView->handleUnicodeChar(uni_char, true))
    {
        return true;
    }

    // Traverses up the hierarchy
    LLFocusableElement* keyboard_focus = gFocusMgr.getKeyboardFocus();
    if( keyboard_focus )
    {
        if (keyboard_focus->handleUnicodeChar(uni_char, false))
        {
            return true;
        }

        return true;
    }

    return false;
}


void LLViewerWindow::handleScrollWheel(S32 clicks)
{
    LLUI::getInstance()->resetMouseIdleTimer();

    LLMouseHandler* mouse_captor = gFocusMgr.getMouseCapture();
    if( mouse_captor )
    {
        S32 local_x;
        S32 local_y;
        mouse_captor->screenPointToLocal( mCurrentMousePoint.mX, mCurrentMousePoint.mY, &local_x, &local_y );
        mouse_captor->handleScrollWheel(local_x, local_y, clicks);
        if (LLView::sDebugMouseHandling)
        {
            LL_INFOS() << "Scroll Wheel handled by captor " << mouse_captor->getName() << LL_ENDL;
        }
        return;
    }

    LLUICtrl* top_ctrl = gFocusMgr.getTopCtrl();
    if (top_ctrl)
    {
        S32 local_x;
        S32 local_y;
        top_ctrl->screenPointToLocal( mCurrentMousePoint.mX, mCurrentMousePoint.mY, &local_x, &local_y );
        if (top_ctrl->handleScrollWheel(local_x, local_y, clicks)) return;
    }

    if (mRootView->handleScrollWheel(mCurrentMousePoint.mX, mCurrentMousePoint.mY, clicks) )
    {
        if (LLView::sDebugMouseHandling)
        {
            LL_INFOS() << "Scroll Wheel" << LLView::sMouseHandlerMessage << LL_ENDL;
        }
        return;
    }
    else if (LLView::sDebugMouseHandling)
    {
        LL_INFOS() << "Scroll Wheel not handled by view" << LL_ENDL;
    }

    // Zoom the camera in and out behavior

    if(top_ctrl == 0
        && getWorldViewRectScaled().pointInRect(mCurrentMousePoint.mX, mCurrentMousePoint.mY)
        && gAgentCamera.isInitialized())
        gAgentCamera.handleScrollWheel(mCameraWheelFractional ? mCameraWheelDelta : (F32)clicks);

    return;
}

void LLViewerWindow::handleScrollHWheel(S32 clicks)
{
    if (LLAppViewer::instance()->quitRequested())
    {
        return;
    }

    LLUI::getInstance()->resetMouseIdleTimer();

    LLMouseHandler* mouse_captor = gFocusMgr.getMouseCapture();
    if (mouse_captor)
    {
        S32 local_x;
        S32 local_y;
        mouse_captor->screenPointToLocal(mCurrentMousePoint.mX, mCurrentMousePoint.mY, &local_x, &local_y);
        mouse_captor->handleScrollHWheel(local_x, local_y, clicks);
        if (LLView::sDebugMouseHandling)
        {
            LL_INFOS() << "Scroll Horizontal Wheel handled by captor " << mouse_captor->getName() << LL_ENDL;
        }
        return;
    }

    LLUICtrl* top_ctrl = gFocusMgr.getTopCtrl();
    if (top_ctrl)
    {
        S32 local_x;
        S32 local_y;
        top_ctrl->screenPointToLocal(mCurrentMousePoint.mX, mCurrentMousePoint.mY, &local_x, &local_y);
        if (top_ctrl->handleScrollHWheel(local_x, local_y, clicks)) return;
    }

    if (mRootView->handleScrollHWheel(mCurrentMousePoint.mX, mCurrentMousePoint.mY, clicks))
    {
        if (LLView::sDebugMouseHandling)
        {
            LL_INFOS() << "Scroll Horizontal Wheel" << LLView::sMouseHandlerMessage << LL_ENDL;
        }
        return;
    }
    else if (LLView::sDebugMouseHandling)
    {
        LL_INFOS() << "Scroll Horizontal Wheel not handled by view" << LL_ENDL;
    }

    return;
}

void LLViewerWindow::addPopup(LLView* popup)
{
    if (mPopupView)
    {
        mPopupView->addPopup(popup);
    }
}

void LLViewerWindow::removePopup(LLView* popup)
{
    if (mPopupView)
    {
        mPopupView->removePopup(popup);
    }
}

void LLViewerWindow::clearPopups()
{
    if (mPopupView)
    {
        mPopupView->clearPopups();
    }
}

void LLViewerWindow::moveCursorToCenter()
{
    if (! gSavedSettings.getBOOL("DisableMouseWarp"))
    {
        S32 x = getWorldViewWidthScaled() / 2;
        S32 y = getWorldViewHeightScaled() / 2;

#if !(LL_WINDOWS && !LL_SDL_WINDOW)
        // Hidden cursor + warp-to-center == a drag/look tool (camera orbit/pan/
        // zoom, grab, mouselook) is using the cursor as a delta source. Drive it
        // from SDL3 relative mode (xrel/yrel from the raw relative pointer), NOT
        // from absolute-position diffs against the warped center.
        //
        // Why absolute diffs are wrong here (the ctrl+alt+click camera "fling"):
        // SDL3's warp is not atomic w.r.t. the event stream. On Wayland,
        // Wayland_WarpMouse does lock->position-hint->unlock and immediately
        // synthesizes an absolute motion event at the target, so SDL's mouse
        // state says "center" at once -- but wl_pointer.motion events the
        // compositor sent BEFORE it processed the warp are still in the socket
        // and carry the OLD absolute position (near the click point). They are
        // dispatched on the next pump and diff against mLastMousePoint=center,
        // yielding a one-frame delta of (click point - window center), i.e. up
        // to half the window width -> the camera orbits by up to ~180 degrees
        // on the first drag frame. It only happens when the mouse moved while
        // the warp was in flight, hence "often but not every time". (Same
        // class of race as XWarpPointer under X11.) The event's xrel is
        // equally polluted, because SDL computes it as x - last_x after the
        // warp reset last_x to the center.
        //
        // In window relative mode SDL drops absolute motion events outright
        // (SDL_PrivateSendMouseMotion: !relative && window_is_relative ->
        // not posted), SDL_SetRelativeMouseMode flushes queued motion, and a
        // warp only updates SDL's internal position (no physical warp, no
        // synthetic event), so stale pre-warp positions can never become a
        // delta. This is the SDL equivalent of LLWindowWin32's raw-input
        // getCursorDelta. The keep-alive/release logic lives in
        // updateMouseDelta(); on release SDL restores the cursor to the last
        // warped position.
        //
        // The previous trigger (engage relative mode only when the warp looked
        // like a no-op) never fired on KWin: Wayland_WarpMouse's synthetic
        // motion updates SDL_GetMouseState immediately, so before != after.
        bool want_relative = mCursorHidden;
        if (!want_relative)
        {
            // Visible cursor: still detect a genuinely ignored warp (compositor
            // without pointer-constraints) so drags keep working at edges.
            LLCoordWindow before;
            if (mWindow->getCursorPosition(&before))
            {
                LLUI::getInstance()->setMousePositionScreen(x, y);
                LLCoordWindow after;
                want_relative = mWindow->getCursorPosition(&after) &&
                                before.mX == after.mX && before.mY == after.mY;
                if (!want_relative)
                {
                    mCurrentMousePoint.set(x,y);
                    mLastMousePoint.set(x,y);
                    mCurrentMouseDelta.set(0,0);
                    return;
                }
            }
        }
        if (want_relative)
        {
            // Engage BEFORE warping, so the warp is internal-only (no lock/hint
            // dance, no synthetic absolute event) and any in-flight absolute
            // motion is discarded by SDL.
            mRelativeMouseModeFrames = llmax(mRelativeMouseModeFrames, 3);
            mWindow->setRelativeMouseMode(true);
            LLUI::getInstance()->setMousePositionScreen(x, y);
            mSuppressMouseDeltaFrames = 0;
            // Deltas come from getCursorDelta (xrel) while relative; keep the
            // hover point at the (virtual) center like the absolute path does.
            mCurrentMousePoint.set(x,y);
            mLastMousePoint.set(x,y);
            mCurrentMouseDelta.set(0,0);
            return;
        }
#endif

        LLUI::getInstance()->setMousePositionScreen(x, y);

        //on a forced move, all deltas get zeroed out to prevent jumping
        mCurrentMousePoint.set(x,y);
        mLastMousePoint.set(x,y);
        mCurrentMouseDelta.set(0,0);
    }
}

void LLViewerWindow::moveCursorTo(S32 x, S32 y)
{
    // Clamp to the whole window (not just the world view) so a gamepad-driven
    // cursor (see LLAgent::applyExternalActions()'s CONTROL_MODE_CURSOR
    // handling) can reach the menu bar and toolbars, but not escape the window.
    x = llclamp(x, 0, getWindowWidthScaled());
    y = llclamp(y, 0, getWindowHeightScaled());

    LLUI::getInstance()->setMousePositionScreen(x, y);

    // on a forced move, all deltas get zeroed out to prevent jumping
    mCurrentMousePoint.set(x, y);
    mLastMousePoint.set(x, y);
    mCurrentMouseDelta.set(0, 0);
}


//////////////////////////////////////////////////////////////////////
//
// Hover handlers
//

void append_xui_tooltip(LLView* viewp, LLToolTip::Params& params)
{
    if (viewp)
    {
        if (!params.styled_message.empty())
        {
            params.styled_message.add().text("\n---------\n");
        }
        LLView::root_to_view_iterator_t end_tooltip_it = viewp->endRootToView();
        // NOTE: we skip "root" since it is assumed
        for (LLView::root_to_view_iterator_t tooltip_it = ++viewp->beginRootToView();
            tooltip_it != end_tooltip_it;
            ++tooltip_it)
        {
            LLView* viewp = *tooltip_it;

            params.styled_message.add().text(viewp->getName());

            LLPanel* panelp = dynamic_cast<LLPanel*>(viewp);
            if (panelp && !panelp->getXMLFilename().empty())
            {
                params.styled_message.add()
                    .text("(" + panelp->getXMLFilename() + ")")
                    .style.color(LLColor4(0.7f, 0.7f, 1.f, 1.f));
            }
            params.styled_message.add().text("/");
        }
    }
}

static LLTrace::BlockTimerStatHandle ftm("Update UI");

// Update UI based on stored mouse position from mouse-move
// event processing.
// Collects visible views whose screen bounding rect contains (x, y), in display order, carrying the screen
// origin down the tree instead of re-walking ancestors per view. With honor_opaque, a mouse-opaque hit ends
// the traversal once its own subtree has been visited (nothing behind it can be hovered).
static void collectHoverViews(LLView* viewp, S32 origin_x, S32 origin_y, S32 x, S32 y, bool honor_opaque,
                              std::set<LLHandle<LLView> >& hover_set, bool& stop)
{
    if (!viewp->getVisible() || !viewp->calcScreenBoundingRect(origin_x, origin_y).pointInRect(x, y))
    {
        return;
    }

    hover_set.insert(viewp->getHandle());

    const bool opaque = honor_opaque && viewp->getMouseOpaque();
    for (LLView* childp : *viewp->getChildList())
    {
        const LLRect& child_rect = childp->getRect();
        collectHoverViews(childp, origin_x + child_rect.mLeft, origin_y + child_rect.mBottom, x, y, honor_opaque, hover_set, stop);
        if (stop)
        {
            return;
        }
    }
    if (opaque)
    {
        stop = true;
    }
}

static void collectHoverViews(LLView* rootp, S32 x, S32 y, bool honor_opaque, std::set<LLHandle<LLView> >& hover_set)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    S32 origin_x, origin_y;
    rootp->localPointToScreen(0, 0, &origin_x, &origin_y);
    bool stop = false;
    collectHoverViews(rootp, origin_x, origin_y, x, y, honor_opaque, hover_set, stop);
}

void LLViewerWindow::updateUI()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI; //LL_RECORD_BLOCK_TIME(ftm);

    // Ease any in-flight smooth scrolls (UISmoothScroll).
    LLScrollbar::updateSmoothScrolling(LLFrameTimer::getFrameDeltaTimeF32());

    static std::string last_handle_msg;

    if (gLoggedInTime.getStarted())
    {
        const F32 DESTINATION_GUIDE_HINT_TIMEOUT = 1200.f;
        const F32 SIDE_PANEL_HINT_TIMEOUT = 300.f;
        if (gLoggedInTime.getElapsedTimeF32() > DESTINATION_GUIDE_HINT_TIMEOUT)
        {
            LLFirstUse::notUsingDestinationGuide();
        }
        if (gLoggedInTime.getElapsedTimeF32() > SIDE_PANEL_HINT_TIMEOUT)
        {
            LLFirstUse::notUsingSidePanel();
        }
    }

    { VK_HITCH_SCOPE("ui_upd_console"); LLConsole::updateClass(); }

    // animate layout stacks so we have up to date rect for world view
    { VK_HITCH_SCOPE("ui_upd_layout"); LLLayoutStack::updateClass(); }
    VK_HITCH_SCOPE("ui_upd_rest");   // world view rect, debug raycast, hover / tooltip pass
    // Vulkan renders the world into the FULL window (every pass is window-sized), so
    // the world view -- which drives the camera aspect, the pick ray
    // (mouseDirectionGlobal / mousePointHUD) and the HUD viewport -- must be the full
    // window too. With the docked-toolbar placeholder rect, the pick ray used the
    // shorter rect's centre/height while the image spanned the window: picks were
    // offset/scaled vs what's drawn (the far edge of objects was unclickable) and the
    // world was drawn with the wrong aspect (horizontally squeezed).
    updateWorldViewRect(true);
    // #919: the HUD lays out over LL's world view rect (the placeholder), not the window.
    updateHudRect();

    LLView::sMouseHandlerMessage.clear();

    S32 x = mCurrentMousePoint.mX;
    S32 y = mCurrentMousePoint.mY;

    MASK    mask = gKeyboard->currentMask(true);

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST))
    {
        gDebugRaycastFaceHit = gDebugRaycastGLTFNodeHit = gDebugRaycastGLTFPrimitiveHit = -1;
        gDebugRaycastObject = cursorIntersect(-1, -1, 512.f, NULL, -1, false, false, true, false,
                                              &gDebugRaycastFaceHit,
                                              &gDebugRaycastGLTFNodeHit,
                                              &gDebugRaycastGLTFPrimitiveHit,
                                              &gDebugRaycastIntersection,
                                              &gDebugRaycastTexCoord,
                                              &gDebugRaycastNormal,
                                              &gDebugRaycastTangent,
                                              &gDebugRaycastStart,
                                              &gDebugRaycastEnd);

        // Every frame: the per-frame GPU answer, never a synchronous wait (#481).
        gDebugRaycastParticle = gPipeline.lineSegmentIntersectParticle(gDebugRaycastStart, gDebugRaycastEnd, /*sync*/ false,
                                                                       &gDebugRaycastParticleIntersection, NULL, NULL);
    }

    updateMouseDelta();
    updateKeyboardFocus();

    bool handled = false;

    LLUICtrl* top_ctrl = gFocusMgr.getTopCtrl();
    LLMouseHandler* mouse_captor = gFocusMgr.getMouseCapture();
    LLView* captor_view = dynamic_cast<LLView*>(mouse_captor);

    //FIXME: only include captor and captor's ancestors if mouse is truly over them --RN

    //build set of views containing mouse cursor by traversing UI hierarchy and testing
    //screen rect against mouse cursor
    view_handle_set_t mouse_hover_set;

    // constraint mouse enter events to children of mouse captor
    LLView* root_view = captor_view;

    // if mouse captor doesn't exist or isn't a LLView
    // then allow mouse enter events on entire UI hierarchy
    if (!root_view)
    {
        root_view = mRootView;
    }

    static LLCachedControl<bool> dump_menu_holder(gSavedSettings, "DumpMenuHolderSize", false);
    if (dump_menu_holder)
    {
        static bool init = false;
        static LLFrameTimer child_count_timer;
        static std::vector <std::string> child_vec;
        if (!init)
        {
            child_count_timer.resetWithExpiry(5.f);
            init = true;
        }
        if (child_count_timer.hasExpired())
        {
            LL_INFOS() << "gMenuHolder child count: " << gMenuHolder->getChildCount() << LL_ENDL;
            std::vector<std::string> local_child_vec;
            LLView::child_list_t child_list = *gMenuHolder->getChildList();
            for (auto child : child_list)
            {
                local_child_vec.emplace_back(child->getName());
            }
            if (!local_child_vec.empty() && local_child_vec != child_vec)
            {
                std::vector<std::string> out_vec;
                std::sort(local_child_vec.begin(), local_child_vec.end());
                std::sort(child_vec.begin(), child_vec.end());
                std::set_difference(child_vec.begin(), child_vec.end(), local_child_vec.begin(), local_child_vec.end(), std::inserter(out_vec, out_vec.begin()));
                if (!out_vec.empty())
                {
                    LL_INFOS() << "gMenuHolder removal diff size: '"<<out_vec.size() <<"' begin_child_diff";
                    for (auto str : out_vec)
                    {
                        LL_CONT << " : " << str;
                    }
                    LL_CONT << " : end_child_diff" << LL_ENDL;
                }

                out_vec.clear();
                std::set_difference(local_child_vec.begin(), local_child_vec.end(), child_vec.begin(), child_vec.end(), std::inserter(out_vec, out_vec.begin()));
                if (!out_vec.empty())
                {
                    LL_INFOS() << "gMenuHolder addition diff size: '" << out_vec.size() << "' begin_child_diff";
                    for (auto str : out_vec)
                    {
                        LL_CONT << " : " << str;
                    }
                    LL_CONT << " : end_child_diff" << LL_ENDL;
                }
                child_vec.swap(local_child_vec);
            }
            child_count_timer.resetWithExpiry(5.f);
        }
    }

    // only update mouse hover set when UI is visible (since we shouldn't send hover events to invisible UI
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        // include all ancestors of captor_view as automatically having mouse
        if (captor_view)
        {
            LLView* captor_parent_view = captor_view->getParent();
            while(captor_parent_view)
            {
                mouse_hover_set.insert(captor_parent_view->getHandle());
                captor_parent_view = captor_parent_view->getParent();
            }
        }

        // aggregate visible views that contain mouse cursor in display order
        LLPopupView::popup_list_t popups = mPopupView->getCurrentPopups();

        for(LLPopupView::popup_list_t::iterator popup_it = popups.begin(); popup_it != popups.end(); ++popup_it)
        {
            LLView* popup = popup_it->get();
            if (popup && popup->calcScreenBoundingRect().pointInRect(x, y))
            {
                collectHoverViews(popup, x, y, false, mouse_hover_set);
            }
        }

        // while the top_ctrl contains the mouse cursor, only it and its descendants will receive onMouseEnter events
        if (top_ctrl && top_ctrl->calcScreenBoundingRect().pointInRect(x, y))
        {
            collectHoverViews(top_ctrl, x, y, false, mouse_hover_set);
        }
        else
        {
            collectHoverViews(root_view, x, y, true, mouse_hover_set);
        }
    }

    typedef std::vector<LLHandle<LLView> > view_handle_list_t;

    // call onMouseEnter() on all views which contain the mouse cursor but did not before
    view_handle_list_t mouse_enter_views;
    std::set_difference(mouse_hover_set.begin(), mouse_hover_set.end(),
                        mMouseHoverViews.begin(), mMouseHoverViews.end(),
                        std::back_inserter(mouse_enter_views));
    for (view_handle_list_t::iterator it = mouse_enter_views.begin();
        it != mouse_enter_views.end();
        ++it)
    {
        LLView* viewp = it->get();
        if (viewp)
        {
            LLRect view_screen_rect = viewp->calcScreenRect();
            viewp->onMouseEnter(x - view_screen_rect.mLeft, y - view_screen_rect.mBottom, mask);
        }
    }

    // call onMouseLeave() on all views which no longer contain the mouse cursor
    view_handle_list_t mouse_leave_views;
    std::set_difference(mMouseHoverViews.begin(), mMouseHoverViews.end(),
                        mouse_hover_set.begin(), mouse_hover_set.end(),
                        std::back_inserter(mouse_leave_views));
    for (view_handle_list_t::iterator it = mouse_leave_views.begin();
        it != mouse_leave_views.end();
        ++it)
    {
        LLView* viewp = it->get();
        if (viewp)
        {
            LLRect view_screen_rect = viewp->calcScreenRect();
            viewp->onMouseLeave(x - view_screen_rect.mLeft, y - view_screen_rect.mBottom, mask);
        }
    }

    // store resulting hover set for next frame
    swap(mMouseHoverViews, mouse_hover_set);

    // The pointer is over the world (the hover reaches a tool, or a tool holds the mouse);
    // UI hidden: wherever it is in the window. Read by DoF focus-follows-pointer (#724).
    mPointerOverWorld = mMouseInWindow && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);

    // only handle hover events when UI is enabled
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {

        if( mouse_captor )
        {
            mPointerOverWorld = mMouseInWindow && dynamic_cast<LLTool*>(mouse_captor) != nullptr;
            // Pass hover events to object capturing mouse events.
            S32 local_x;
            S32 local_y;
            mouse_captor->screenPointToLocal( x, y, &local_x, &local_y );
            handled = mouse_captor->handleHover(local_x, local_y, mask);
            if (LLView::sDebugMouseHandling)
            {
                LL_INFOS() << "Hover handled by captor " << mouse_captor->getName() << LL_ENDL;
            }

            if( !handled )
            {
                LL_DEBUGS("UserInput") << "hover not handled by mouse captor" << LL_ENDL;
            }
        }
        else
        {
            if (top_ctrl)
            {
                S32 local_x, local_y;
                top_ctrl->screenPointToLocal( x, y, &local_x, &local_y );
                handled = top_ctrl->pointInView(local_x, local_y) && top_ctrl->handleHover(local_x, local_y, mask);
            }

            if ( !handled )
            {
                // x and y are from last time mouse was in window
                // mMouseInWindow tracks *actual* mouse location
                if (mMouseInWindow && mRootView->handleHover(x, y, mask) )
                {
                    if (LLView::sDebugMouseHandling && LLView::sMouseHandlerMessage != last_handle_msg)
                    {
                        last_handle_msg = LLView::sMouseHandlerMessage;
                        LL_INFOS() << "Hover" << LLView::sMouseHandlerMessage << LL_ENDL;
                    }
                    handled = true;
                }
                else if (LLView::sDebugMouseHandling)
                {
                    if (last_handle_msg != LLStringUtil::null)
                    {
                        last_handle_msg.clear();
                        LL_INFOS() << "Hover not handled by view" << LL_ENDL;
                    }
                }
            }

            // Pose Studio IK handles (#530): hover highlight before the world tool.
            if (SLPoseIK::instanceExists())
            {
                if (!handled && mMouseInWindow)
                    handled = SLPoseIK::instance().handleHover(x, y, mask);
                else
                    SLPoseIK::instance().clearHover();  // over the UI: no lingering highlight
            }

            if (!handled)
            {
                LLTool *tool = LLToolMgr::getInstance()->getCurrentTool();

                if(mMouseInWindow && tool)
                {
                    mPointerOverWorld = true;
                    handled = tool->handleHover(x, y, mask);
                }
            }
        }

        // Show a new tool tip (or update one that is already shown)
        bool tool_tip_handled = false;
        std::string tool_tip_msg;
        if( handled
            && !mWindow->isCursorHidden())
        {
            LLRect screen_sticky_rect = mRootView->getLocalRect();
            S32 local_x, local_y;

            static LLCachedControl<bool> debug_show_xui_names(gSavedSettings, "DebugShowXUINames", 0);
            if (debug_show_xui_names)
            {
                LLToolTip::Params params;

                LLView* tooltip_view = mRootView;
                LLView::tree_iterator_t end_it = mRootView->endTreeDFS();
                for (LLView::tree_iterator_t it = mRootView->beginTreeDFS(); it != end_it; ++it)
                {
                    LLView* viewp = *it;
                    LLRect screen_rect;
                    viewp->localRectToScreen(viewp->getLocalRect(), &screen_rect);
                    if (!(viewp->getVisible()
                         && screen_rect.pointInRect(x, y)))
                    {
                        it.skipDescendants();
                    }
                    // only report xui names for LLUICtrls,
                    // and blacklist the various containers we don't care about
                    else if (dynamic_cast<LLUICtrl*>(viewp)
                            && viewp != gMenuHolder
                            && viewp != gFloaterView
                            && viewp != gConsole)
                    {
                        if (dynamic_cast<LLFloater*>(viewp))
                        {
                            // constrain search to descendants of this (frontmost) floater
                            // by resetting iterator
                            it = viewp->beginTreeDFS();
                        }

                        // if we are in a new part of the tree (not a descendent of current tooltip_view)
                        // then push the results for tooltip_view and start with a new potential view
                        // NOTE: this emulates visiting only the leaf nodes that meet our criteria
                        if (!viewp->hasAncestor(tooltip_view))
                        {
                            append_xui_tooltip(tooltip_view, params);
                            screen_sticky_rect.intersectWith(tooltip_view->calcScreenRect());
                        }
                        tooltip_view = viewp;
                    }
                }

                append_xui_tooltip(tooltip_view, params);
                params.styled_message.add().text("\n");

                screen_sticky_rect.intersectWith(tooltip_view->calcScreenRect());

                params.sticky_rect = screen_sticky_rect;
                params.max_width = 400;

                LLToolTipMgr::instance().show(params);
            }
            // if there is a mouse captor, nothing else gets a tooltip
            else if (mouse_captor)
            {
                mouse_captor->screenPointToLocal(x, y, &local_x, &local_y);
                tool_tip_handled = mouse_captor->handleToolTip(local_x, local_y, mask);
            }
            else
            {
                // next is top_ctrl
                if (!tool_tip_handled && top_ctrl)
                {
                    top_ctrl->screenPointToLocal(x, y, &local_x, &local_y);
                    tool_tip_handled = top_ctrl->handleToolTip(local_x, local_y, mask );
                }

                if (!tool_tip_handled)
                {
                    local_x = x; local_y = y;
                    tool_tip_handled = mRootView->handleToolTip(local_x, local_y, mask );
                }

                LLTool* current_tool = LLToolMgr::getInstance()->getCurrentTool();
                if (!tool_tip_handled && current_tool)
                {
                    current_tool->screenPointToLocal(x, y, &local_x, &local_y);
                    tool_tip_handled = current_tool->handleToolTip(local_x, local_y, mask );
                }
            }
        }
    }
    else
    {   // just have tools handle hover when UI is turned off
        LLTool *tool = LLToolMgr::getInstance()->getCurrentTool();

        if (SLMMBNav::isActive())
        {
            // Middle-mouse navigation (#652) works with the UI hidden (photography): the mouse
            // captor is only consulted above while the UI is on.
            handled = SLMMBNav::getInstance()->handleHover(x, y, mask);
        }
        else if(mMouseInWindow && tool)
        {
            handled = tool->handleHover(x, y, mask);
        }
    }

    updateLayout();

    mLastMousePoint = mCurrentMousePoint;

    // cleanup unused selections when no modal dialogs are open
    if (LLModalDialog::activeCount() == 0)
    {
        LLViewerParcelMgr::getInstance()->deselectUnused();
    }

    if (LLModalDialog::activeCount() == 0)
    {
        LLSelectMgr::getInstance()->deselectUnused();
    }
}


void LLViewerWindow::updateLayout()
{
    static LLCachedControl<bool> freeze_time(gSavedSettings, "FreezeTime", false);
    LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
    if (gFloaterTools != NULL
        && tool != NULL
        && tool != gToolNull
        && tool != LLToolCompInspect::getInstance()
        && tool != LLToolDragAndDrop::getInstance()
        && !freeze_time)
    {
        // Suppress the toolbox view if our source tool was the pie tool,
        // and we've overridden to something else.
        bool suppress_toolbox =
            (LLToolMgr::getInstance()->getBaseTool() == LLToolPie::getInstance()) &&
            (LLToolMgr::getInstance()->getCurrentTool() != LLToolPie::getInstance());

        LLMouseHandler *captor = gFocusMgr.getMouseCapture();
        // With the null, inspect, or drag and drop tool, don't muck
        // with visibility.

        if (gFloaterTools->isMinimized()
            ||  (tool != LLToolPie::getInstance()                       // not default tool
                && tool != LLToolCompGun::getInstance()                 // not coming out of mouselook
                && !suppress_toolbox                                    // not override in third person
                && LLToolMgr::getInstance()->getCurrentToolset()->isShowFloaterTools()
                && (!captor || dynamic_cast<LLView*>(captor) != NULL)))                     // not dragging
        {
            // Force floater tools to be visible (unless minimized)
            if (!gFloaterTools->getVisible())
            {
                gFloaterTools->openFloater();
            }
            // Update the location of the blue box tool popup
            LLCoordGL select_center_screen;
            MASK    mask = gKeyboard->currentMask(true);
            gFloaterTools->updatePopup( select_center_screen, mask );
        }
        else
        {
            gFloaterTools->setVisible(false);
        }
        //gMenuBarView->setItemVisible("BuildTools", gFloaterTools->getVisible());
    }

    // Always update console
    if(gConsole)
    {
        LLRect console_rect = getChatConsoleRect();
        gConsole->reshape(console_rect.getWidth(), console_rect.getHeight());
        gConsole->setRect(console_rect);
    }
    if (mNearbyChatConsole)
    {
        const LLRect rect = getChatConsoleRect();
        mNearbyChatConsole->setRect(rect);
        mNearbyChatConsole->reshape(rect.getWidth(), rect.getHeight());
        const bool overlay_visible = LLViewerChat::isNearbyChatOverlayVisible();
        if (!overlay_visible && mNearbyChatOverlayVisible)
        {
            if (auto* nearby = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat"))
            {
                nearby->removeScreenChat();
            }
        }
        mNearbyChatOverlayVisible = overlay_visible;
        const bool visible = overlay_visible && gSavedSettings.getString("NearbyChatOverlayStyle") == "text";
        if (!visible && mNearbyChatConsole->getVisible())
        {
            mNearbyChatConsole->clear();
        }
        mNearbyChatConsole->setVisible(visible);
    }
}

void LLViewerWindow::updateMouseDelta()
{
#if LL_WINDOWS && !LL_SDL_WINDOW
    LLCoordCommon delta;
    mWindow->getCursorDelta(&delta);
    S32 dx = delta.mX;
    S32 dy = delta.mY;
#else
    S32 dx, dy;
    // When relative mouse mode is engaged (camera capture on Wayland and
    // similar), pull deltas from SDL3 xrel/yrel which keep coming even when
    // the cursor clamps at a window edge. Decrement the keep-alive counter
    // each frame; if no warp has been requested recently, drop back to the
    // position-diff path and release relative mode.
    if (mRelativeMouseModeFrames > 0)
    {
        LLCoordCommon rel_delta;
        mWindow->getCursorDelta(&rel_delta);
        // SDL motion deltas (event.motion.xrel/yrel) are in window
        // coordinates (Y points down). The position-diff path above uses
        // GL coordinates (Y points up) because mCurrentMousePoint is fed
        // through convertCoords(). Flip Y so dx/dy keep the same sign
        // convention regardless of which source produced them; otherwise
        // pan/orbit invert vertically when relative mode is engaged.
        // Units: getCursorDelta returns window PIXELS, which is already what the
        // position-diff path produces (scaled UI diff * UI scale). Do not apply the
        // UI scale again, or orbit/pan speed grows with UI size (#460).
        dx = rel_delta.mX;
        dy = -rel_delta.mY;
        // Hold relative mode for the WHOLE drag: while any tool has mouse capture
        // (button held) the keep-alive never lapses. The old pure 3-FRAME countdown
        // (refreshed only by hover-driven moveCursorToCenter calls) lapsed on any
        // ~20 ms pause at 144 FPS; the delta path then fell back to absolute
        // position diffs against a stale last point -> huge phantom deltas (camera
        // spinning / jumping to a random orbit).
        if (gFocusMgr.getMouseCapture() != nullptr)
        {
            mRelativeMouseModeFrames = llmax(mRelativeMouseModeFrames, 3);
        }
        else if (--mRelativeMouseModeFrames == 0)
        {
            mWindow->setRelativeMouseMode(false);
            // Resync: the first absolute events after leaving relative mode must
            // not diff against the pre/in-relative position.
            mSuppressMouseDeltaFrames = 2;
        }
    }
    else if (mSuppressMouseDeltaFrames > 0)
    {
        --mSuppressMouseDeltaFrames;
        mLastMousePoint = mCurrentMousePoint;
        dx = 0;
        dy = 0;
    }
    else
    {
        dx = lltrunc((F32) (mCurrentMousePoint.mX - mLastMousePoint.mX) * LLUI::getScaleFactor().mV[VX]);
        dy = lltrunc((F32) (mCurrentMousePoint.mY - mLastMousePoint.mY) * LLUI::getScaleFactor().mV[VY]);
    }
#endif

    //RN: fix for asynchronous notification of mouse leaving window not working
    LLCoordWindow mouse_pos;
    mWindow->getCursorPosition(&mouse_pos);
    if (mouse_pos.mX < 0 ||
        mouse_pos.mY < 0 ||
        mouse_pos.mX > mWindowRectRaw.getWidth() ||
        mouse_pos.mY > mWindowRectRaw.getHeight())
    {
        mMouseInWindow = false;
    }
    else
    {
        mMouseInWindow = true;
    }

    LLVector2 mouse_vel;

    static LLCachedControl<bool> mouseSmooth(gSavedSettings, "MouseSmooth");
    if (mouseSmooth)
    {
        static F32 fdx = 0.f;
        static F32 fdy = 0.f;

        F32 amount = 16.f;
        fdx = fdx + ((F32) dx - fdx) * llmin(gFrameIntervalSeconds.value()*amount,1.f);
        fdy = fdy + ((F32) dy - fdy) * llmin(gFrameIntervalSeconds.value()*amount,1.f);

        mCurrentMouseDelta.set(ll_round(fdx), ll_round(fdy));
        mouse_vel.setVec(fdx,fdy);
    }
    else
    {
        mCurrentMouseDelta.set(dx, dy);
        mouse_vel.setVec((F32) dx, (F32) dy);
    }

    sample(sMouseVelocityStat, mouse_vel.magVec());
}

void LLViewerWindow::updateKeyboardFocus()
{
    if (!gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        gFocusMgr.setKeyboardFocus(NULL);
    }

    // clean up current focus
    LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    if (cur_focus)
    {
        bool is_in_visible_chain = cur_focus->isInVisibleChain();
        bool is_in_enabled_chain = cur_focus->isInEnabledChain();
        if (!is_in_visible_chain || !is_in_enabled_chain)
        {
            // don't release focus, just reassign so that if being given
            // to a sibling won't call onFocusLost on all the ancestors
            // gFocusMgr.releaseFocusIfNeeded(cur_focus);

            LLUICtrl* parent = cur_focus->getParentUICtrl();
            const LLUICtrl* focus_root = cur_focus->findRootMostFocusRoot();
            bool new_focus_found = false;
            while(parent)
            {
                if (!is_in_visible_chain)
                {
                    is_in_visible_chain = parent->isInVisibleChain();
                }
                if (!is_in_enabled_chain)
                {
                    is_in_enabled_chain = parent->isInEnabledChain();
                }
                if (parent->isCtrl()
                    && (parent->hasTabStop() || parent == focus_root)
                    && !parent->getIsChrome()
                    && is_in_visible_chain
                    && is_in_enabled_chain)
                {
                    if (!parent->focusFirstItem())
                    {
                        parent->setFocus(true);
                    }
                    new_focus_found = true;
                    break;
                }
                parent = parent->getParentUICtrl();
            }

            // if we didn't find a better place to put focus, just release it
            // hasFocus() will return true if and only if we didn't touch focus since we
            // are only moving focus higher in the hierarchy
            if (!new_focus_found)
            {
                cur_focus->setFocus(false);
            }
        }
        else if (cur_focus->isFocusRoot())
        {
            // focus roots keep trying to delegate focus to their first valid descendant
            // this assumes that focus roots are not valid focus holders on their own
            cur_focus->focusFirstItem();
        }
    }

    // last ditch force of edit menu to selection manager
    if (LLEditMenuHandler::gEditMenuHandler == NULL && LLSelectMgr::getInstance()->getSelection()->getObjectCount())
    {
        LLEditMenuHandler::gEditMenuHandler = LLSelectMgr::getInstance();
    }

    if (gFloaterView->getCycleMode())
    {
        // sync all floaters with their focus state
        gFloaterView->highlightFocusedFloater();
        gSnapshotFloaterView->highlightFocusedFloater();
        MASK    mask = gKeyboard->currentMask(true);
        if ((mask & MASK_CONTROL) == 0)
        {
            // control key no longer held down, finish cycle mode
            gFloaterView->setCycleMode(false);

            gFloaterView->syncFloaterTabOrder();
        }
        else
        {
            // user holding down CTRL, don't update tab order of floaters
        }
    }
    else
    {
        // update focused floater
        gFloaterView->highlightFocusedFloater();
        gSnapshotFloaterView->highlightFocusedFloater();
        // make sure floater visible order is in sync with tab order
        gFloaterView->syncFloaterTabOrder();
    }
}

static LLTrace::BlockTimerStatHandle FTM_UPDATE_WORLD_VIEW("Update World View");
void LLViewerWindow::updateWorldViewRect(bool use_full_window)
{
    LL_RECORD_BLOCK_TIME(FTM_UPDATE_WORLD_VIEW);

    // start off using whole window to render world
    LLRect new_world_rect = mWindowRectRaw;

    if (!use_full_window && mWorldViewPlaceholder.get())
    {
        new_world_rect = mWorldViewPlaceholder.get()->calcScreenRect();
        // clamp to at least a 1x1 rect so we don't try to allocate zero width gl buffers
        new_world_rect.mTop = llmax(new_world_rect.mTop, new_world_rect.mBottom + 1);
        new_world_rect.mRight = llmax(new_world_rect.mRight, new_world_rect.mLeft + 1);

        new_world_rect.mLeft = ll_round((F32)new_world_rect.mLeft * mDisplayScale.mV[VX]);
        new_world_rect.mRight = ll_round((F32)new_world_rect.mRight * mDisplayScale.mV[VX]);
        new_world_rect.mBottom = ll_round((F32)new_world_rect.mBottom * mDisplayScale.mV[VY]);
        new_world_rect.mTop = ll_round((F32)new_world_rect.mTop * mDisplayScale.mV[VY]);
    }

    if (mWorldViewRectRaw != new_world_rect)
    {
        mWorldViewRectRaw = new_world_rect;
        gResizeScreenTexture = true;
        LLViewerCamera::getInstance()->setViewHeightInPixels( mWorldViewRectRaw.getHeight() );
        LLViewerCamera::getInstance()->setAspect( getWorldViewAspectRatio() );
    }

    // The scaled rect depends on the display (UI) scale as well as the raw rect. The
    // world view is always the full window now, so a UI size change leaves the raw rect
    // untouched; gating this on a raw change kept the scaled rect at the OLD scale. The
    // pick ray (mouseDirectionGlobal / mousePointHUD) then took UI-scaled mouse coords
    // against a centre/height in the old units: picks and the alt-zoom focus landed
    // toward the bottom-left of the click, in proportion to the scale change (#460).
    const LLRect new_world_rect_scaled = calcScaledRect(mWorldViewRectRaw, mDisplayScale);
    if (mWorldViewRectScaled != new_world_rect_scaled)
    {
        LLRect old_world_rect_scaled = mWorldViewRectScaled;
        mWorldViewRectScaled = new_world_rect_scaled;

        // sending a signal with a new WorldView rect
        mOnWorldViewRectUpdated(old_world_rect_scaled, mWorldViewRectScaled);
    }
}

void LLViewerWindow::saveLastMouse(const LLCoordGL &point)
{
    // Store last mouse location.
    // If mouse leaves window, pretend last point was on edge of window

    if (point.mX < 0)
    {
        mCurrentMousePoint.mX = 0;
    }
    else if (point.mX > getWindowWidthScaled())
    {
        mCurrentMousePoint.mX = getWindowWidthScaled();
    }
    else
    {
        mCurrentMousePoint.mX = point.mX;
    }

    if (point.mY < 0)
    {
        mCurrentMousePoint.mY = 0;
    }
    else if (point.mY > getWindowHeightScaled() )
    {
        mCurrentMousePoint.mY = getWindowHeightScaled();
    }
    else
    {
        mCurrentMousePoint.mY = point.mY;
    }
}


// Draws the selection outlines for the currently selected objects
// Must be called after displayObjects is called, which sets the mGLName parameter
// NOTE: This function gets called 3 times:
//  render_ui_3d:           false, false, true
//  render_hud_elements:    false, false, false
void LLViewerWindow::renderSelections( bool for_gl_pick, bool pick_parcel_walls, bool for_hud )
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();

    if (!for_hud && !for_gl_pick)
    {
        // Call this once and only once
        LLSelectMgr::getInstance()->updateSilhouettes();
    }

    // <FS:Beq> render the poser manipulator guides
    // if we have something selected those toosl should override
    if (!for_hud && selection->isEmpty())
    {
        if (LLToolMgr::getInstance()->getCurrentTool() == FSToolCompPose::getInstance())
            FSToolCompPose::getInstance()->render();
        if (LLToolMgr::getInstance()->getCurrentTool() == FSToolCompPoseTranslate::getInstance())
            FSToolCompPoseTranslate::getInstance()->render();
    }
    // </FS:Beq>

    // Draw fence around land selections
    if (for_gl_pick)
    {
        if (pick_parcel_walls)
        {
            LLViewerParcelMgr::getInstance()->renderParcelCollision();
        }
    }
    else if (( for_hud && selection->getSelectType() == SELECT_TYPE_HUD) ||
             (!for_hud && selection->getSelectType() != SELECT_TYPE_HUD))
    {
        // The selection outline is drawn by the VK renderer (LLCleanRendererVK).
        // <GMDX> ...except LL's silhouettes for prims and sculpties (below).
        // setup HUD render
        if (selection->getSelectType() == SELECT_TYPE_HUD && LLSelectMgr::getInstance()->getSelection()->getObjectCount())
        {
            LLBBox hud_bbox = gAgentAvatarp->getHUDBBox();

            // set up transform to encompass bounding box of HUD
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.pushMatrix();
            // #919: the HUD rect within the full-window UI frame (the remap), then the
            // HUD ortho at the HUD aspect -- the same mapping as the HUD pass draws with.
            F32 hud_remap[16];
            getHudClipRemap(hud_remap);
            gGL.loadMatrix(hud_remap);
            F32 depth = llmax(1.f, hud_bbox.getExtentLocal().mV[VX] * 1.1f);
            // Depth range -100..100 like get_hud_matrices (#413): the manipulators
            // scale by the HUD zoom, which pushes handles past [0, depth] at zoom > 1.
            const F32 hud_aspect = getHudProjectionAspect();
            gGL.ortho(-0.5f * hud_aspect, 0.5f * hud_aspect, -0.5f, 0.5f, -100.f, 100.f);

            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            gGL.loadIdentity();
            gGL.loadMatrix(OGL_TO_CFR_ROTATION);        // Load Cory's favorite reference frame
            gGL.translatef(-hud_bbox.getCenterLocal().mV[VX] + (depth *0.5f), 0.f, 0.f);
        }

        GMDXSelectionOutline::render(for_hud); // <GMDX> LL's renderSilhouettes(for_hud)

        // Render light for editing
        if (LLSelectMgr::sRenderLightRadius && LLToolMgr::getInstance()->inEdit())
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            LLRender::DepthScope gls_depth(true, false);
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            if (selection->getSelectType() == SELECT_TYPE_HUD)
            {
                F32 zoom = gAgentCamera.mHUDCurZoom;
                gGL.scalef(zoom, zoom, zoom);
                const LLVector3& pan = gAgentCamera.mHUDCurPan;   // #413
                gGL.translatef(-pan.mV[VX], -pan.mV[VY], -pan.mV[VZ]);
            }

            struct f : public LLSelectedObjectFunctor
            {
                virtual bool apply(LLViewerObject* object)
                {
                    LLDrawable* drawable = object->mDrawable;
                    if (drawable && drawable->isLight())
                    {
                        LLVOVolume* vovolume = drawable->getVOVolume();
                        gGL.pushMatrix();

                        LLVector3 center = drawable->getPositionAgent();
                        gGL.translatef(center[0], center[1], center[2]);
                        F32 scale = vovolume->getLightRadius();
                        gGL.scalef(scale, scale, scale);

                        // Unculled: one pass covers both the outside and the inside
                        // (0.75 matches the old twice-drawn 0.5 coverage).
                        LLColor4 color(vovolume->getLightSRGBColor(), .75f);
                        gGL.color4fv(color.mV);
                        render_unit_sphere();

                        gGL.popMatrix();
                    }
                    return true;
                }
            } func;
            LLSelectMgr::getInstance()->getSelection()->applyToObjects(&func);

            gGL.popMatrix();
        }

        // NOTE: The average position for the axis arrows of the selected objects should
        // not be recalculated at this time.  If they are, then group rotations will break.

        // Draw arrows at average center of all selected objects
        LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
        if (tool)
        {
            if(tool->isAlwaysRendered())
            {
                tool->render();
            }
            else
            {
                if( !LLSelectMgr::getInstance()->getSelection()->isEmpty() )
                {
                    bool all_selected_objects_move;
                    bool all_selected_objects_modify;
                    // Note: This might be costly to do on each frame and when a lot of objects are selected
                    // we might be better off with some kind of memory for selection and/or states, consider
                    // optimizing, perhaps even some kind of selection generation at level of LLSelectMgr to
                    // make whole viewer benefit.
                    LLSelectMgr::getInstance()->selectGetEditMoveLinksetPermissions(all_selected_objects_move, all_selected_objects_modify);

                    bool draw_handles = true;

                    if (tool == LLToolCompTranslate::getInstance() && !all_selected_objects_move && !LLSelectMgr::getInstance()->isMovableAvatarSelected())
                    {
                        draw_handles = false;
                    }

                    if (tool == LLToolCompRotate::getInstance() && !all_selected_objects_move && !LLSelectMgr::getInstance()->isMovableAvatarSelected())
                    {
                        draw_handles = false;
                    }

                    if ( !all_selected_objects_modify && tool == LLToolCompScale::getInstance() )
                    {
                        draw_handles = false;
                    }

                    if( draw_handles )
                    {
                        tool->render();
                    }
                }
            }
        }

        // un-setup HUD render
        if (selection->getSelectType() == SELECT_TYPE_HUD && selection->getObjectCount())
        {
            gGL.matrixMode(LLRender::MM_PROJECTION);
            gGL.popMatrix();

            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.popMatrix();
        }
    }
}

// Return a point near the clicked object representative of the place the object was clicked.
LLVector3d LLViewerWindow::clickPointInWorldGlobal(S32 x, S32 y_from_bot, LLViewerObject* clicked_object) const
{
    // create a normalized vector pointing from the camera center into the
    // world at the location of the mouse click
    LLVector3 mouse_direction_global = mouseDirectionGlobal( x, y_from_bot );

    LLVector3d relative_object = clicked_object->getPositionGlobal() - gAgentCamera.getCameraPositionGlobal();

    // make mouse vector as long as object vector, so it touchs a point near
    // where the user clicked on the object
    mouse_direction_global *= (F32) relative_object.magVec();

    LLVector3d new_pos;
    new_pos.setVec(mouse_direction_global);
    // transform mouse vector back to world coords
    new_pos += gAgentCamera.getCameraPositionGlobal();

    return new_pos;
}


bool LLViewerWindow::clickPointOnSurfaceGlobal(const S32 x, const S32 y, LLViewerObject *objectp, LLVector3d &point_global) const
{
    bool intersect = false;

//  U8 shape = objectp->mPrimitiveCode & LL_PCODE_BASE_MASK;
    if (!intersect)
    {
        point_global = clickPointInWorldGlobal(x, y, objectp);
        LL_INFOS() << "approx intersection at " <<  (objectp->getPositionGlobal() - point_global) << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "good intersection at " <<  (objectp->getPositionGlobal() - point_global) << LL_ENDL;
    }

    return intersect;
}

void LLViewerWindow::pickAsync( S32 x,
                                S32 y_from_bot,
                                MASK mask,
                                void (*callback)(const LLPickInfo& info),
                                bool pick_transparent,
                                bool pick_rigged,
                                bool pick_unselectable,
                                bool pick_reflection_probes)
{
    // "Show Debug Alpha" means no object actually transparent
    bool in_build_mode = LLFloaterReg::instanceVisible("build");
    if (LLPipeline::sShowDebugAlpha
        || (in_build_mode && gSavedSettings.getBOOL("SelectInvisibleObjects")))
    {
        pick_transparent = true;
    }

    LLPickInfo pick_info(LLCoordGL(x, y_from_bot), mask, pick_transparent, pick_rigged, false, pick_reflection_probes, pick_unselectable, true, callback);
    schedulePick(pick_info);
}

void LLViewerWindow::schedulePick(LLPickInfo& pick_info)
{
    if (mPicks.size() >= 1024 || mWindow->getMinimized())
    { //something went wrong, picks are being scheduled but not processed

        if (pick_info.mPickCallback)
        {
            pick_info.mPickCallback(pick_info);
        }

        return;
    }
    mPicks.push_back(pick_info);

    // delay further event processing until we receive results of pick
    // only do this for async picks so that handleMouseUp won't be called
    // until the pick triggered in handleMouseDown has been processed, for example
    mWindow->delayInputProcessing();
}


void LLViewerWindow::performPick()
{
    if (!mPicks.empty())
    {
        // A click's pick whose exact GPU answer is not in yet waits for it here (the per-frame
        // request asks for its pixel) instead of a blocking answer-now behind the frames in
        // flight; the input after it waits too, as schedulePick asks (llcleancursorpick.h
        // holdClick, RenderVKPickAsyncClick).
        for (const LLPickInfo& p : mPicks)
        {
            if (!p.mPickClick) continue;
            if (LLCleanCursorPick::holdClick(p.mMousePt.mX, p.mMousePt.mY))
            {
                mWindow->holdInputFrame();
                return;
            }
            break;
        }
        std::vector<LLPickInfo>::iterator pick_it;
        for (pick_it = mPicks.begin(); pick_it != mPicks.end(); ++pick_it)
        {
            pick_it->fetchResults();
        }
        LLCleanCursorPick::clickFetched();

        mLastPick = mPicks.back();
        mPicks.clear();
    }
}

void LLViewerWindow::returnEmptyPicks()
{
    LLCleanCursorPick::clickFetched();   // (a held click goes with them)
    std::vector<LLPickInfo>::iterator pick_it;
    for (pick_it = mPicks.begin(); pick_it != mPicks.end(); ++pick_it)
    {
        mLastPick = *pick_it;
        // just trigger callback with empty results
        if (pick_it->mPickCallback)
        {
            pick_it->mPickCallback(*pick_it);
        }
    }
    mPicks.clear();
}

// Performs the GL object/land pick.
LLPickInfo LLViewerWindow::pickImmediate(S32 x, S32 y_from_bot, bool pick_transparent, bool pick_rigged, bool pick_particle, bool pick_unselectable, bool pick_reflection_probe)
{
    bool in_build_mode = LLFloaterReg::instanceVisible("build");
    static LLCachedControl<bool> select_invisible_objects(gSavedSettings, "SelectInvisibleObjects");
    if ((in_build_mode && select_invisible_objects) || LLPipeline::sShowDebugAlpha)
    {
        // build mode allows interaction with all transparent objects
        // "Show Debug Alpha" means no object actually transparent
        pick_transparent = true;
    }

    // shortcut queueing in mPicks and just update mLastPick in place
    MASK    key_mask = gKeyboard->currentMask(true);
    mLastPick = LLPickInfo(LLCoordGL(x, y_from_bot), key_mask, pick_transparent, pick_rigged, pick_particle, pick_reflection_probe, true, false, NULL);
    mLastPick.fetchResults();
    ++mPickSerial;

    return mLastPick;
}

LLHUDIcon* LLViewerWindow::cursorIntersectIcon(S32 mouse_x, S32 mouse_y, F32 depth,
                                           LLVector4a* intersection)
{
    S32 x = mouse_x;
    S32 y = mouse_y;

    if ((mouse_x == -1) && (mouse_y == -1)) // use current mouse position
    {
        x = getCurrentMouseX();
        y = getCurrentMouseY();
    }

    // world coordinates of mouse
    // VECTORIZE THIS
    LLVector3 mouse_direction_global = mouseDirectionGlobal(x,y);
    LLVector3 mouse_point_global = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 mouse_world_start = mouse_point_global;
    LLVector3 mouse_world_end   = mouse_point_global + mouse_direction_global * depth;

    LLVector4a start, end;
    start.load3(mouse_world_start.mV);
    end.load3(mouse_world_end.mV);

    return LLHUDIcon::lineSegmentIntersectAll(start, end, intersection);
}

LLViewerObject* LLViewerWindow::cursorIntersect(S32 mouse_x, S32 mouse_y, F32 depth,
                                                LLViewerObject *this_object,
                                                S32 this_face,
                                                bool pick_transparent,
                                                bool pick_rigged,
                                                bool pick_unselectable,
                                                bool pick_reflection_probe,
                                                S32* face_hit,
                                                S32* gltf_node_hit,
                                                S32* gltf_primitive_hit,
                                                LLVector4a *intersection,
                                                LLVector2 *uv,
                                                LLVector4a *normal,
                                                LLVector4a *tangent,
                                                LLVector4a* start,
                                                LLVector4a* end)
{
    S32 x = mouse_x;
    S32 y = mouse_y;

    if ((mouse_x == -1) && (mouse_y == -1)) // use current mouse position
    {
        x = getCurrentMouseX();
        y = getCurrentMouseY();
    }

    // HUD coordinates of mouse
    LLVector3 mouse_point_hud = mousePointHUD(x, y);
    LLVector3 mouse_hud_start = mouse_point_hud - LLVector3(depth, 0, 0);
    LLVector3 mouse_hud_end   = mouse_point_hud + LLVector3(depth, 0, 0);

    // world coordinates of mouse
    LLVector3 mouse_direction_global = mouseDirectionGlobal(x,y);
    LLVector3 mouse_point_global = LLViewerCamera::getInstance()->getOrigin();

    //get near clip plane
    LLVector3 n = LLViewerCamera::getInstance()->getAtAxis();
    LLVector3 p = mouse_point_global + n * LLViewerCamera::getInstance()->getNear();

    //project mouse point onto plane
    LLVector3 pos;
    line_plane(mouse_point_global, mouse_direction_global, p, n, pos);
    mouse_point_global = pos;

    LLVector3 mouse_world_start = mouse_point_global;
    LLVector3 mouse_world_end   = mouse_point_global + mouse_direction_global * depth;

    // (LL kept the last hit here for the flycam / pointer DoF focus; DoF reads the GPU
    // cursor pick's own answer now, LLCleanCursorPick::focusPoint, #724.)
    gDebugRaycastIntersection.load3(mouse_world_end.mV);

    LLVector4a mw_start;
    mw_start.load3(mouse_world_start.mV);
    LLVector4a mw_end;
    mw_end.load3(mouse_world_end.mV);

    LLVector4a mh_start;
    mh_start.load3(mouse_hud_start.mV);
    LLVector4a mh_end;
    mh_end.load3(mouse_hud_end.mV);

    if (start)
    {
        *start = mw_start;
    }

    if (end)
    {
        *end = mw_end;
    }

    LLViewerObject* found = NULL;

    if (this_object)  // check only this object
    {
        // The GPU raster pick (#702 / #704, llcleancursorpick.h): this object's nearest
        // surface on the ray as drawn (posed rigged mesh, worn attachments on this frame's
        // joints, the drawn LOD), with the face, LL's surface ST, normal and tangent that
        // touch / media / face select read. Hidden HUDs (Ctrl+Alt+H) can't be picked (#187).
        const bool hud = this_object->isHUDAttachment();
        if (hud ? LLPipeline::sShowHUDAttachments : (pick_reflection_probe || !this_object->isReflectionProbe()))
        {
            if (LLCleanCursorPick::intersectObject(this_object, x, y, hud ? mh_start : mw_start, hud ? mh_end : mw_end,
                                                   this_face, pick_transparent, pick_unselectable, face_hit,
                                                   gltf_node_hit, gltf_primitive_hit, intersection, uv, normal, tangent))
            {
                found = this_object;
            }
        }
    }
    else // check ALL objects
    {
// [RLVa:KB] - Checked: 2010-03-31 (RLVa-1.2.0c) | Modified: RLVa-1.2.0c
        // The camera tool's Alt pick ignores HUDs.
        const bool hud_blocked = (rlv_handler_t::isEnabled()) && (LLToolCamera::getInstance()->hasMouseCapture())
                                 && (gKeyboard->currentMask(true) & MASK_ALT);
// [/RLVa:KB]
        // The GPU raster pick (#702 / #704, llcleancursorpick.h): the renderer's answer for
        // this ray -- what is drawn there, under LL's pick rules. A click no answer covers
        // is answered now (one GPU wait); hover takes the newest answer.
        S32 g_face = -1, g_node = -1, g_prim = -1;
        LLVector4a g_ip, g_nrm, g_tng;
        LLVector2 g_uv;
        g_ip.clear(); g_nrm.clear(); g_tng.clear();
        LLViewerObject* g_found = NULL;
        LLCleanCursorPick::intersectAll(
            x, y, mw_start, mw_end, mh_start, mh_end, hud_blocked, pick_transparent, pick_rigged,
            pick_unselectable, pick_reflection_probe, g_found, &g_face, &g_node, &g_prim, &g_ip, &g_uv, &g_nrm, &g_tng);
        found = g_found;
        if (found)
        {
            if (face_hit)           *face_hit = g_face;
            if (gltf_node_hit)      *gltf_node_hit = g_node;
            if (gltf_primitive_hit) *gltf_primitive_hit = g_prim;
            if (intersection)       *intersection = g_ip;
            if (uv)                 *uv = g_uv;
            if (normal)             *normal = g_nrm;
            if (tangent)            *tangent = g_tng;
        }
        if (found && !found->isHUDAttachment() && !pick_transparent && intersection)
        {
            gDebugRaycastIntersection = *intersection;
        }

// [RLVa:KB] - Checked: RLVa-1.2.0
        if ( (found) && ((gTeleportDisplay) || ((rlv_handler_t::isEnabled()) && (gRlvHandler.hasBehaviour(RLV_BHVR_INTERACT)))) )
        {
            // Allow picking if:
            //   - the drag-and-drop tool is active (allows inventory offers)
            //   - the camera tool is active
            //   - the pie tool is active *and* we picked our own avie (allows "mouse steering" and the self pie menu)
            LLTool* pCurTool = LLToolMgr::getInstance()->getCurrentTool();
            if ( (LLToolDragAndDrop::getInstance() != pCurTool) &&
                 (!LLToolCamera::getInstance()->hasMouseCapture()) &&
                 ((LLToolPie::getInstance() != pCurTool) || (gAgent.getID() != found->getID())) )
            {
                found = NULL;
            }
        }
// [/RLVa:KB]
    }

    return found;
}

// Returns unit vector relative to camera
// indicating direction of point on screen x,y
LLVector3 LLViewerWindow::mouseDirectionGlobal(const S32 x, const S32 y) const
{
    // find vertical field of view
    F32         fov = LLViewerCamera::getInstance()->getView();

    // find world view center in scaled ui coordinates
    F32         center_x = (F32)getWorldViewRectScaled().getCenterX();
    F32         center_y = (F32)getWorldViewRectScaled().getCenterY();

    // calculate pixel distance to screen
    F32         distance = ((F32)getWorldViewHeightScaled() * 0.5f) / (tan(fov / 2.f));

    // calculate click point relative to middle of screen
    F32         click_x = x - center_x;
    F32         click_y = y - center_y;

    // compute mouse vector
    LLVector3   mouse_vector =  distance * LLViewerCamera::getInstance()->getAtAxis()
                                - click_x * LLViewerCamera::getInstance()->getLeftAxis()
                                + click_y * LLViewerCamera::getInstance()->getUpAxis();

    mouse_vector.normVec();

    return mouse_vector;
}

LLVector3 LLViewerWindow::mousePointHUD(const S32 x, const S32 y) const
{
    // #919: HUD space spans the HUD rect (LL's world view rect), as drawn.
    const LLRect hud_rect = getHudRectScaled();
    const F32   height = (F32)llmax(hud_rect.getHeight(), 1);

    // find the HUD rect's center (exact, not getCenterX's integer halving)
    const F32   center_x = 0.5f * (F32)(hud_rect.mLeft + hud_rect.mRight);
    const F32   center_y = 0.5f * (F32)(hud_rect.mBottom + hud_rect.mTop);

    // remap with uniform scale (1/height) so that top is -0.5, bottom is +0.5
    F32 hud_x = -((F32)x - center_x)  / height;
    F32 hud_y = ((F32)y - center_y) / height;

    // Inverse of get_hud_matrices: screen offset / zoom + pan (#413).
    const LLVector3& pan = gAgentCamera.mHUDCurPan;
    return LLVector3(0.f, hud_x/gAgentCamera.mHUDCurZoom + pan.mV[VY], hud_y/gAgentCamera.mHUDCurZoom + pan.mV[VZ]);
}

// Returns unit vector relative to camera in camera space
// indicating direction of point on screen x,y
LLVector3 LLViewerWindow::mouseDirectionCamera(const S32 x, const S32 y) const
{
    // find vertical field of view
    F32         fov_height = LLViewerCamera::getInstance()->getView();
    F32         fov_width = fov_height * LLViewerCamera::getInstance()->getAspect();

    // find screen resolution
    S32         height = getWorldViewHeightScaled();
    S32         width = getWorldViewWidthScaled();

    // find world view center
    F32         center_x = (F32)getWorldViewRectScaled().getCenterX();
    F32         center_y = (F32)getWorldViewRectScaled().getCenterY();

    // calculate click point relative to middle of screen
    F32         click_x = (((F32)x - center_x) / (F32)width) * fov_width * -1.f;
    F32         click_y = (((F32)y - center_y) / (F32)height) * fov_height;

    // compute mouse vector
    LLVector3   mouse_vector =  LLVector3(0.f, 0.f, -1.f);
    LLQuaternion mouse_rotate;
    mouse_rotate.setQuat(click_y, click_x, 0.f);

    mouse_vector = mouse_vector * mouse_rotate;
    // project to z = -1 plane;
    mouse_vector = mouse_vector * (-1.f / mouse_vector.mV[VZ]);

    return mouse_vector;
}



bool LLViewerWindow::mousePointOnPlaneGlobal(LLVector3d& point, const S32 x, const S32 y,
                                        const LLVector3d &plane_point_global,
                                        const LLVector3 &plane_normal_global)
{
    LLVector3d  mouse_direction_global_d;

    mouse_direction_global_d.setVec(mouseDirectionGlobal(x,y));
    LLVector3d  plane_normal_global_d;
    plane_normal_global_d.setVec(plane_normal_global);
    F64 plane_mouse_dot = (plane_normal_global_d * mouse_direction_global_d);
    LLVector3d plane_origin_camera_rel = plane_point_global - gAgentCamera.getCameraPositionGlobal();
    F64 mouse_look_at_scale = (plane_normal_global_d * plane_origin_camera_rel)
                                / plane_mouse_dot;
    if (llabs(plane_mouse_dot) < 0.00001)
    {
        // if mouse is parallel to plane, return closest point on line through plane origin
        // that is parallel to camera plane by scaling mouse direction vector
        // by distance to plane origin, modulated by deviation of mouse direction from plane origin
        LLVector3d plane_origin_dir = plane_origin_camera_rel;
        plane_origin_dir.normVec();

        mouse_look_at_scale = plane_origin_camera_rel.magVec() / (plane_origin_dir * mouse_direction_global_d);
    }

    point = gAgentCamera.getCameraPositionGlobal() + mouse_look_at_scale * mouse_direction_global_d;

    return mouse_look_at_scale > 0.0;
}


// Returns global position
bool LLViewerWindow::mousePointOnLandGlobal(const S32 x, const S32 y, LLVector3d *land_position_global, bool ignore_distance)
{
    LLVector3       mouse_direction_global = mouseDirectionGlobal(x,y);
    F32             mouse_dir_scale;
    bool            hit_land = false;
    LLViewerRegion  *regionp;
    F32         land_z;
    const F32   FIRST_PASS_STEP = 1.0f;     // meters
    const F32   SECOND_PASS_STEP = 0.1f;    // meters
    const F32   draw_distance = ignore_distance ? MAX_FAR_CLIP : gAgentCamera.mDrawDistance;
    LLVector3d  camera_pos_global;

    camera_pos_global = gAgentCamera.getCameraPositionGlobal();
    LLVector3d      probe_point_global;
    LLVector3       probe_point_region;

    // walk forwards to find the point
    for (mouse_dir_scale = FIRST_PASS_STEP; mouse_dir_scale < draw_distance; mouse_dir_scale += FIRST_PASS_STEP)
    {
        LLVector3d mouse_direction_global_d;
        mouse_direction_global_d.setVec(mouse_direction_global * mouse_dir_scale);
        probe_point_global = camera_pos_global + mouse_direction_global_d;

        regionp = LLWorld::getInstance()->resolveRegionGlobal(probe_point_region, probe_point_global);

        if (!regionp)
        {
            // ...we're outside the world somehow
            continue;
        }

        S32 i = (S32) (probe_point_region.mV[VX]/regionp->getLand().getMetersPerGrid());
        S32 j = (S32) (probe_point_region.mV[VY]/regionp->getLand().getMetersPerGrid());
        S32 grids_per_edge = (S32) regionp->getLand().mGridsPerEdge;
        if ((i >= grids_per_edge) || (j >= grids_per_edge))
        {
            //LL_INFOS() << "LLViewerWindow::mousePointOnLand probe_point is out of region" << LL_ENDL;
            continue;
        }

        land_z = regionp->getLand().resolveHeightRegion(probe_point_region);

        //LL_INFOS() << "mousePointOnLand initial z " << land_z << LL_ENDL;

        if (probe_point_region.mV[VZ] < land_z)
        {
            // ...just went under land

            // cout << "under land at " << probe_point << " scale " << mouse_vec_scale << endl;

            hit_land = true;
            break;
        }
    }


    if (hit_land)
    {
        // Don't go more than one step beyond where we stopped above.
        // This can't just be "mouse_vec_scale" because floating point error
        // will stop the loop before the last increment.... X - 1.0 + 0.1 + 0.1 + ... + 0.1 != X
        F32 stop_mouse_dir_scale = mouse_dir_scale + FIRST_PASS_STEP;

        // take a step backwards, then walk forwards again to refine position
        for ( mouse_dir_scale -= FIRST_PASS_STEP; mouse_dir_scale <= stop_mouse_dir_scale; mouse_dir_scale += SECOND_PASS_STEP)
        {
            LLVector3d mouse_direction_global_d;
            mouse_direction_global_d.setVec(mouse_direction_global * mouse_dir_scale);
            probe_point_global = camera_pos_global + mouse_direction_global_d;

            regionp = LLWorld::getInstance()->resolveRegionGlobal(probe_point_region, probe_point_global);

            if (!regionp)
            {
                // ...we're outside the world somehow
                continue;
            }

            /*
            i = (S32) (local_probe_point.mV[VX]/regionp->getLand().getMetersPerGrid());
            j = (S32) (local_probe_point.mV[VY]/regionp->getLand().getMetersPerGrid());
            if ((i >= regionp->getLand().mGridsPerEdge) || (j >= regionp->getLand().mGridsPerEdge))
            {
                // LL_INFOS() << "LLViewerWindow::mousePointOnLand probe_point is out of region" << LL_ENDL;
                continue;
            }
            land_z = regionp->getLand().mSurfaceZ[ i + j * (regionp->getLand().mGridsPerEdge) ];
            */

            land_z = regionp->getLand().resolveHeightRegion(probe_point_region);

            //LL_INFOS() << "mousePointOnLand refine z " << land_z << LL_ENDL;

            if (probe_point_region.mV[VZ] < land_z)
            {
                // ...just went under land again

                *land_position_global = probe_point_global;
                return true;
            }
        }
    }

    return false;
}

// Saves an image to the harddrive as "SnapshotX" where X >= 1.
void LLViewerWindow::saveImageNumbered(LLImageFormatted *image, bool force_picker, const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    if (!image)
    {
        LL_WARNS() << "No image to save" << LL_ENDL;
        return;
    }
    std::string extension("." + image->getExtension());
    LLImageFormatted* formatted_image = image;
    // Get a base file location if needed.
    if (force_picker || !isSnapshotLocSet())
    {
        static LLCachedControl<std::string> sSnapshotBaseName(LLCachedControl<std::string>(gSavedPerAccountSettings, "SnapshotBaseName", "Snapshot"));
        std::string proposed_name(sSnapshotBaseName);

        // getSaveFile will append an appropriate extension to the proposed name, based on the ESaveFilter constant passed in.
        LLFilePicker::ESaveFilter pick_type;

        if (extension == ".j2c")
            pick_type = LLFilePicker::FFSAVE_J2C;
        else if (extension == ".bmp")
            pick_type = LLFilePicker::FFSAVE_BMP;
        else if (extension == ".jpg")
            pick_type = LLFilePicker::FFSAVE_JPEG;
        else if (extension == ".png")
            pick_type = LLFilePicker::FFSAVE_PNG;
        else if (extension == ".tga")
            pick_type = LLFilePicker::FFSAVE_TGA;
        else
            pick_type = LLFilePicker::FFSAVE_ALL;

        LLFilePickerReplyThread::startPicker(boost::bind(&LLViewerWindow::onDirectorySelected, this, _1, formatted_image, success_cb, failure_cb), pick_type, proposed_name,
                                        boost::bind(&LLViewerWindow::onSelectionFailure, this, failure_cb));
    }
    else
    {
        saveImageLocal(formatted_image, success_cb, failure_cb);
    }
}

void LLViewerWindow::onDirectorySelected(const std::vector<std::string>& filenames, LLImageFormatted *image, const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    // Copy the directory + file name
    std::string filepath = filenames[0];

    gSavedPerAccountSettings.setString("SnapshotBaseName", gDirUtilp->getBaseFileName(filepath, true));
    gSavedPerAccountSettings.setString("SnapshotBaseDir", gDirUtilp->getDirName(filepath));
    saveImageLocal(image, success_cb, failure_cb);
}

void LLViewerWindow::onSelectionFailure(const snapshot_saved_signal_t::slot_type& failure_cb)
{
    failure_cb();
}


void LLViewerWindow::saveImageLocal(LLImageFormatted *image, const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    std::string lastSnapshotDir = LLViewerWindow::getLastSnapshotDir();
    if (lastSnapshotDir.empty())
    {
        failure_cb();
        return;
    }

// Check if there is enough free space to save snapshot
#ifdef LL_WINDOWS
    boost::filesystem::path b_path(ll_convert<std::wstring>(lastSnapshotDir));
#else
    boost::filesystem::path b_path(lastSnapshotDir);
#endif
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(b_path, ec) || ec.failed())
    {
        LLSD args;
        args["PATH"] = lastSnapshotDir;
        LLNotificationsUtil::add("SnapshotToLocalDirNotExist", args);
        resetSnapshotLoc();
        failure_cb();
        return;
    }
    boost::filesystem::space_info b_space = boost::filesystem::space(b_path, ec);
    if (ec.failed())
    {
        LLSD args;
        args["PATH"] = lastSnapshotDir;
        LLNotificationsUtil::add("SnapshotToLocalDirNotExist", args);
        resetSnapshotLoc();
        failure_cb();
        return;
    }
    if (b_space.free < image->getDataSize())
    {
        LLSD args;
        args["PATH"] = lastSnapshotDir;

        std::string needM_bytes_string;
        LLResMgr::getInstance()->getIntegerString(needM_bytes_string, (image->getDataSize()) >> 10);
        args["NEED_MEMORY"] = needM_bytes_string;

        std::string freeM_bytes_string;
        LLResMgr::getInstance()->getIntegerString(freeM_bytes_string, (S32)(b_space.free >> 10));
        args["FREE_MEMORY"] = freeM_bytes_string;

        LLNotificationsUtil::add("SnapshotToComputerFailed", args);

        failure_cb();

        // Shouldn't there be a return here?
    }

    static LLCachedControl<std::string> sSnapshotBaseName(LLCachedControl<std::string>(gSavedPerAccountSettings, "SnapshotBaseName", "Snapshot"));
    static LLCachedControl<std::string> sSnapshotDir(LLCachedControl<std::string>(gSavedPerAccountSettings, "SnapshotBaseDir", ""));

    // Look for an unused file name
    auto is_snapshot_name_loc_set = isSnapshotLocSet();
    std::string filepath;
    auto i = 1;
    auto err = 0;
    auto extension("." + image->getExtension());
    auto now = LLDate::now();
    do
    {
        filepath = sSnapshotDir;
        filepath += gDirUtilp->getDirDelimiter();
        filepath += sSnapshotBaseName;
        filepath += now.toLocalDateString("_%Y-%m-%d_%H%M%S");
        filepath += llformat("%.2d", i);
        filepath += extension;

        llstat stat_info;
        err = LLFile::stat(filepath, &stat_info);
        i++;
    }
    while( -1 != err  // Search until the file is not found (i.e., stat() gives an error).
            && is_snapshot_name_loc_set); // Or stop if we are rewriting.

    LL_INFOS() << "Saving snapshot to " << filepath << LL_ENDL;
    if (image->save(filepath))
    {
        playSnapshotAnimAndSound();
        success_cb();
    }
    else
    {
        failure_cb();
    }
}

void LLViewerWindow::resetSnapshotLoc()
{
    gSavedPerAccountSettings.setString("SnapshotBaseDir", std::string());
}

// static
void LLViewerWindow::movieSize(S32 new_width, S32 new_height)
{
    LLCoordWindow size;
    LLCoordWindow new_size(new_width, new_height);
    gViewerWindow->getWindow()->getSize(&size);
    if ( size != new_size )
    {
        gViewerWindow->getWindow()->setSize(new_size);
    }
}

bool LLViewerWindow::saveSnapshot(const std::string& filepath, S32 image_width, S32 image_height, bool show_ui, bool show_hud, bool do_rebuild, bool show_balance, LLSnapshotModel::ESnapshotLayerType type, LLSnapshotModel::ESnapshotFormat format)
{
    LL_INFOS() << "Saving snapshot to: " << filepath << LL_ENDL;

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    bool success = rawSnapshot(raw, image_width, image_height, true, false, show_ui, show_hud, do_rebuild, show_balance);

    if (success)
    {
        U8 image_codec = IMG_CODEC_BMP;
        switch (format)
        {
        case LLSnapshotModel::SNAPSHOT_FORMAT_PNG:
            image_codec = IMG_CODEC_PNG;
            break;
        case LLSnapshotModel::SNAPSHOT_FORMAT_JPEG:
            image_codec = IMG_CODEC_JPEG;
            break;
        default:
            image_codec = IMG_CODEC_BMP;
            break;
        }

        LLPointer<LLImageFormatted> formated_image = LLImageFormatted::createFromType(image_codec);
        success = formated_image->encode(raw, 0.0f);
        if (success)
        {
            success = formated_image->save(filepath);
        }
        else
        {
            LL_WARNS() << "Unable to encode snapshot of format " << format << LL_ENDL;
        }
    }
    else
    {
        LL_WARNS() << "Unable to capture raw snapshot" << LL_ENDL;
    }

    return success;
}


void LLViewerWindow::playSnapshotAnimAndSound()
{
    if (gSavedSettings.getBOOL("QuietSnapshotsToDisk"))
    {
        return;
    }
    gAgent.sendAnimationRequest(ANIM_AGENT_SNAPSHOT, ANIM_REQUEST_START);
    send_sound_trigger(LLUUID(gSavedSettings.getString("UISndSnapshot")), 1.0f);
}

bool LLViewerWindow::isSnapshotLocSet() const
{
    static LLCachedControl<std::string> sSnapshotDir(LLCachedControl<std::string>(gSavedPerAccountSettings, "SnapshotBaseDir", ""));
    return !sSnapshotDir().empty();
}

void LLViewerWindow::resetSnapshotLoc() const
{
    gSavedPerAccountSettings.setString("SnapshotBaseDir", std::string());
}

bool LLViewerWindow::thumbnailSnapshot(LLImageRaw *raw, S32 preview_width, S32 preview_height, bool show_ui, bool show_hud, bool do_rebuild, bool no_post, LLSnapshotModel::ESnapshotLayerType type)
{
    return rawSnapshot(raw, preview_width, preview_height, false, false, show_ui, show_hud, do_rebuild, no_post, gSavedSettings.getBOOL("RenderBalanceInSnapshot"), type);
}

// LIVE SNAPSHOTS (#873; vkgfx/llcleansnapshotvk.h). The shot is taken from the MAIN RENDER
// PATH -- exactly what the live view shows (RT reflections, weather / snow, volumetrics,
// TAA's converged image, post), with or without HUD / UI -- instead of re-rendering the
// world offscreen at the shot size:
//  - at or below the window size: the next live frame at its capture point (1 world only,
//    2 world + HUD, 3 the whole frame with UI), cropped to the shot's aspect (never another
//    FOV; the Snapshot floater shows the crop frame) and resampled on the GPU (Lanczos, linear
//    light). One extra live frame: no hitch, no TAA / FSR history reset;
//  - above it (SnapshotAntialias on): RenderSnapshotTemporalFrames..64 live frames, each with
//    its own sub-pixel jitter, accumulated on the GPU into the shot-size image (2x / 4x real
//    detail of a still scene). The world is frozen for the capture (RenderSnapshotFreeze, by
//    default from 2x) -- otherwise moving pixels are rejected by motion vectors. Beyond
//    RenderSnapshotMaxUpscale x the window the rest is scaled on the CPU (the hook a learned
//    upscaler would replace; none is used);
//  - depth: the live depth buffer, linearised and resampled.
// Only the final image is read back, after its frame's fence. Returns false when the live
// path cannot take the shot (setting, no-post, renderer / format, memory): the caller falls
// back to the offscreen renderer, which also keeps the 360 capture (simpleSnapshot).
static bool vk_live_snapshot(LLImageRaw* raw, S32 image_width, S32 image_height, bool show_ui, bool show_hud,
                             bool do_rebuild, bool want_depth, S32 max_size)
{
    LLVulkanContext& vk = LLVulkanContext::instance();
    uint32_t cw = 0, ch = 0;
    if (!vk.liveFrameInfo(cw, ch)) return false;
    if (image_width <= 0 || image_height <= 0) return false;
    // UI is not rescaled: the output never exceeds the window, as before.
    if (show_ui)
    {
        image_width  = llmin(image_width, (S32)cw);
        image_height = llmin(image_height, (S32)ch);
    }
    // Pad the line length to a multiple of 4 bytes (BMP) and honour max_size, as before --
    // here BEFORE the capture, so the GPU writes the final size directly.
    image_width += (image_width * 3) % 4;
    if (image_width > max_size || image_height > max_size)
    {
        const F32 f = llmin((F32)max_size / image_width, (F32)max_size / image_height);
        image_width = llmax(1, (S32)(image_width * f));
        image_height = llmax(1, (S32)(image_height * f));
    }
    // The crop: the largest centred rectangle of the shot's aspect inside the live frame.
    const F64 ratio = llmin((F64)cw / image_width, (F64)ch / image_height);
    const F64 crop_w = llclamp(ratio * image_width, 1.0, (F64)cw);
    const F64 crop_h = llclamp(ratio * image_height, 1.0, (F64)ch);
    LLVulkanContext::LiveSnapshot req;
    req.crop[0] = (F32)((cw - crop_w) * 0.5 / cw);
    req.crop[1] = (F32)((ch - crop_h) * 0.5 / ch);
    req.crop[2] = (F32)(crop_w / cw);
    req.crop[3] = (F32)(crop_h / ch);
    req.point = show_ui ? 3u : (show_hud ? 2u : 1u);
    {
        // #932: the shot's 8-bit output is dithered once, at the shot size, like the live view.
        static LLCachedControl<bool> s_dither(gSavedSettings, "RenderDitherEnabled", true);
        req.dither = s_dither;
    }
    const F64 up = (F64)image_width / crop_w;   // output px per live px
    S32 out_w = image_width, out_h = image_height;
    bool freeze = false;
    std::string why;
    if (want_depth)
    {
        req.mode = 1;
        req.point = 1;
        static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 128.f);
        req.depthNear = LLViewerCamera::getInstance()->getNear();
        req.depthFar = llmax(LLViewerCamera::getInstance()->getFar(), (F32)far_clip);
    }
    else if (up > 1.0001 && !show_ui)
    {
        static LLCachedControl<bool> s_aa(gSavedSettings, "SnapshotAntialias", true);
        static LLCachedControl<U32> s_frames(gSavedSettings, "RenderSnapshotTemporalFrames", 16);
        static LLCachedControl<U32> s_max_up(gSavedSettings, "RenderSnapshotMaxUpscale", 4);
        static LLCachedControl<U32> s_freeze(gSavedSettings, "RenderSnapshotFreeze", 1);
        if (s_aa && (U32)s_frames > 0)
        {
            // Accumulated at up to RenderSnapshotMaxUpscale x the live pixels; the remainder is
            // a CPU scale (ML UPSCALER HOOK: a learned single-image upscaler would go here).
            const F64 acc_up = llmin(up, (F64)llclamp((U32)s_max_up, 1u, 8u));
            if (acc_up < up)
            {
                out_w = llmax(1, (S32)llround(crop_w * acc_up));
                out_h = llmax(1, (S32)llround(crop_h * acc_up));
                LL_INFOS("Snapshot") << "live snapshot " << image_width << "x" << image_height << ": accumulated at "
                                     << out_w << "x" << out_h << " (RenderSnapshotMaxUpscale " << (U32)s_max_up
                                     << "x), scaled the rest of the way" << LL_ENDL;
            }
            // Samples per output pixel ~ frames / up^2: at least 4 (Halton fills the grid).
            req.mode = 2;
            req.frames = llmin(64u, llmax((U32)s_frames, (U32)ceil(4.0 * acc_up * acc_up)));
            freeze = (U32)s_freeze >= 2u || ((U32)s_freeze == 1u && acc_up >= 2.0);
            req.motionReject = !freeze;
        }
        else
        {
            req.mode = 0;   // one live frame, Lanczos-upsampled
        }
    }
    else
    {
        req.mode = 0;
    }
    // System memory: the host-visible output, the RGB copy and the LLImageRaw (3 B / px each).
    if (LLMemoryGovernor::getTotalRAM() > 0)
    {
        const U64 avail = LLMemoryGovernor::getAvailBytes(), reserve = LLMemoryGovernor::getReserveBytes();
        const U64 ram = avail > reserve ? avail - reserve : 0;
        const U64 need = (U64)out_w * out_h * 9u + (U64)image_width * image_height * 3u;
        if (need > ram)
        {
            LL_WARNS("Snapshot") << "live snapshot " << image_width << "x" << image_height << ": not enough free system memory ("
                                 << (need >> 20) << " MB needed, " << (ram >> 20) << " MB above the reserve); using the offscreen path"
                                 << LL_ENDL;
            return false;
        }
    }
    // Arm (only the shot's buffers are allocated, soft). Out of video memory: a smaller
    // accumulation, then a single-frame upsample, then the offscreen path.
    for (;;)
    {
        req.outW = (uint32_t)out_w; req.outH = (uint32_t)out_h;
        if (vk.beginLiveSnapshot(req, why)) break;
        LL_WARNS("Snapshot") << "live snapshot " << out_w << "x" << out_h << " (mode " << req.mode << "): " << why << LL_ENDL;
        if (req.mode == 2 && (F64)out_w / crop_w > 1.25)
        {
            out_w = llmax(1, (S32)llround(out_w * 0.75));
            out_h = llmax(1, (S32)llround(out_h * 0.75));
            req.frames = llmin(64u, llmax(req.frames / 2u, 8u));
            continue;
        }
        if (req.mode == 2)
        {
            req.mode = 0;
            out_w = llmin(image_width, (S32)llround(crop_w));
            out_h = llmin(image_height, (S32)llround(crop_h));
            freeze = false;
            continue;
        }
        return false;
    }
    // Freeze the world for an accumulated shot: the jittered frames must see one scene.
    if (freeze && !LLWorldClock::isFrozen())
    {
        LLWorldClock::setCaptureFreeze(true);
        LLWorldClock::tick();   // (frozen now: world time holds, its delta is 0, avatars pause)
    }
    else
    {
        freeze = false;
    }
    // Render live frames until the shot has all of its samples (frames can skip: swapchain
    // recreate, minimised). Each one is presented: the live view keeps running.
    const U32 max_frames = (req.mode == 2 ? req.frames : 1u) + 8u;
    for (U32 i = 0; i < max_frames && !vk.liveSnapshotFinished(); ++i)
        display(do_rebuild, 1.f, 0, true);   // VK: displayVulkanUI() -- renders + presents one frame
    if (freeze) LLWorldClock::setCaptureFreeze(false);   // (the next idle tick thaws without a jump)
    std::vector<U8> rgb;
    uint32_t w = 0, h = 0;
    if (!vk.liveSnapshotFinished())
    {
        vk.cancelLiveSnapshot();
        LL_WARNS("Snapshot") << "live snapshot: the frames were not rendered (window hidden?)" << LL_ENDL;
        return false;
    }
    if (!vk.readLiveSnapshot(rgb, w, h, why) || !w || !h)
    {
        LL_WARNS("Snapshot") << "live snapshot failed: " << why << LL_ENDL;
        return false;
    }
    // The VRAM comparison (#873 acceptance): what the offscreen path would have re-created.
    {
        const LLVulkanContext::SnapshotVram sv = vk.snapshotVram();
        const F64 bpp = sv.livePx ? (F64)sv.liveTargetBytes / (F64)sv.livePx : 0.0;
        LL_INFOS("Snapshot") << "live snapshot " << w << "x" << h << " (requested " << image_width << "x" << image_height
                             << ", point " << req.point << ", mode " << req.mode
                             << (req.mode == 2 ? llformat(", %u frames%s", req.frames, freeze ? ", frozen" : ", motion rejection")
                                               : std::string())
                             << "); the offscreen path would have re-created ~" << (U64)(bpp * w * h / 1048576.0)
                             << " MB of render targets at this size" << LL_ENDL;
    }
    LLImageDataLock lock(raw);
    raw->resize((S32)w, (S32)h, 3);
    if (raw->isBufferInvalid()) return false;
    memcpy(raw->getData(), rgb.data(), (size_t)w * h * 3);
    if ((S32)w != image_width || (S32)h != image_height)
        return raw->scale(image_width, image_height, true);
    return true;
}

// Saves the image from the screen to a raw image
// Since the required size might be bigger than the available screen, this method rerenders the scene in parts (called subimages) and copy
// the results over to the final raw image.
bool LLViewerWindow::rawSnapshot(LLImageRaw *raw, S32 image_width, S32 image_height,
    bool keep_window_aspect, bool is_texture, bool show_ui, bool show_hud, bool do_rebuild, bool no_post, bool show_balance, LLSnapshotModel::ESnapshotLayerType type, S32 max_size)
{
    if (!raw)
    {
        return false;
    }

    //  * #873: every snapshot is taken from the MAIN RENDER PATH (vk_live_snapshot, above) --
    //    unless RenderSnapshotOffscreen (A/B, fallback) or "No post-processing" (the live
    //    frame has its post) asks for the offscreen renderer, or the live path cannot take it.
    //  * Fallback: world / world+HUD and depth snapshots render OFFSCREEN at exactly the
    //    requested size (vkRenderOffscreenSnapshot), clamped to the device's image limit, the
    //    video memory and the system memory (vkSnapshotFit, #730; then upscaled).
    //  * Fallback: snapshots WITH UI capture the presented frame (the UI is laid out for the
    //    window), cropped to the requested aspect.
    {
        static LLCachedControl<bool> s_offscreen(gSavedSettings, "RenderSnapshotOffscreen", false);
        const bool want_depth = (type == LLSnapshotModel::SNAPSHOT_TYPE_DEPTH);
        if (!s_offscreen && !(no_post && !show_ui && !want_depth))
        {
            setCursor(UI_CURSOR_WAIT);
            if (show_ui) setBalanceVisible(show_balance);
            const bool ok = vk_live_snapshot(raw, image_width, image_height, show_ui && !want_depth, show_hud,
                                             do_rebuild, want_depth, max_size);
            setBalanceVisible(true);
            setCursor(UI_CURSOR_ARROW);
            if (ok) return true;
            LL_INFOS("Snapshot") << "live snapshot not taken; falling back to the "
                                 << ((show_ui && !want_depth) ? "presented-frame capture" : "offscreen renderer") << LL_ENDL;
        }
    }
    if (!show_ui || type == LLSnapshotModel::SNAPSHOT_TYPE_DEPTH)
    {
        extern bool vkRenderOffscreenSnapshot(S32 w, S32 h, bool show_hud, std::vector<U8>& rgb,
                                              std::vector<F32>* depth, F32 proj[2], U32 aa_frames);
        extern U32 vkSnapshotAAFrames();
        extern U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames);
        extern const std::string& vkSnapshotFitLimit();
        const bool want_depth = (type == LLSnapshotModel::SNAPSHOT_TYPE_DEPTH);
        if (image_width <= 0 || image_height <= 0) return false;
        // Render size (#730): the requested size within the device limit, the video memory
        // the renderer's targets measure per pixel and the system memory for the readback
        // (a supersampled snapshot's accumulators are dropped first when they don't fit,
        // #417); the rest is upscaled. Nothing that fits: refused up front. (The renderer's
        // resolution-sized targets are still hard allocations: this sizing is what keeps
        // them inside the budget.)
        S32 rw = image_width, rh = image_height;
        const U32 aa = vkSnapshotFit(rw, rh, want_depth ? 0u : vkSnapshotAAFrames());
        if (rw <= 0 || rh <= 0 || rw != image_width || rh != image_height)
        {
            // Tell the user once per requested size (the floater re-renders previews).
            static S32 s_told_w = 0, s_told_h = 0;
            if (s_told_w != image_width || s_told_h != image_height)
            {
                s_told_w = image_width;
                s_told_h = image_height;
                LLSD args;
                args["REQUESTED"] = llformat("%d x %d", image_width, image_height);
                args["RENDERED"] = rw > 0 ? llformat("%d x %d", rw, rh) : std::string();
                args["LIMIT"] = vkSnapshotFitLimit();
                LLNotificationsUtil::add(rw > 0 ? "VKSnapshotReduced" : "VKSnapshotRefused", args);
            }
            if (rw <= 0 || rh <= 0) return false;
        }
        setCursor(UI_CURSOR_WAIT);
        std::vector<U8> rgb;
        std::vector<F32> depth;
        F32 proj[2] = {0.f, 0.f};
        // Snapshot floater "No post-processing" (RenderSnapshotNoPost; LL gSnapshotNoPost):
        // the offscreen render's tonemap takes the no-post path (no exposure, no tonemapper,
        // a clamp -- LL's NoPost tonemap programs). UI snapshots capture the presented frame.
        gSnapshotNoPost = no_post;
        const bool got = vkRenderOffscreenSnapshot(rw, rh, show_hud, rgb, want_depth ? &depth : nullptr, proj, aa);
        gSnapshotNoPost = false;
        setCursor(UI_CURSOR_ARROW);
        if (!got)
        {
            LL_WARNS("Snapshot") << "Vulkan: offscreen snapshot " << rw << "x" << rh << " failed" << LL_ENDL;
            return false;
        }
        LL_INFOS("Snapshot") << "Vulkan offscreen snapshot " << rw << "x" << rh << " (requested "
            << image_width << "x" << image_height << ", hud=" << show_hud << ", depth=" << want_depth
            << ", antialias frames=" << aa << ")" << LL_ENDL;
        LLImageDataLock lock(raw);
        raw->resize(rw, rh, 3);
        if (raw->isBufferInvalid()) return false;
        if (want_depth)
        {
            // Linear view distance D = P14 / (d + P10), 8-bit over [near, far] as LL.
            static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 128.f);
            const F32 znear = LLViewerCamera::getInstance()->getNear();
            const F32 zfar = llmax(LLViewerCamera::getInstance()->getFar(), (F32)far_clip);
            U8* out = raw->getData();
            for (S32 y = 0; y < rh; ++y)
            {
                const F32* row = depth.data() + (size_t)(rh - 1 - y) * rw;   // bottom-up
                for (S32 x = 0; x < rw; ++x)
                {
                    const F32 den = row[x] + proj[0];
                    const F32 lin = (fabsf(den) > 1e-7f) ? proj[1] / den : zfar;
                    const U8 b = F32_to_U8(lin, znear, zfar);
                    U8* px = out + ((size_t)y * rw + x) * 3;
                    px[0] = px[1] = px[2] = b;
                }
            }
        }
        else
        {
            memcpy(raw->getData(), rgb.data(), (size_t)rw * rh * 3);
        }
        // Pad the line length to a multiple of 4 bytes (BMP) and honour max_size, as GL.
        image_width += (image_width * 3) % 4;
        if (image_width > max_size || image_height > max_size)
        {
            const F32 f = llmin((F32)max_size / image_width, (F32)max_size / image_height);
            image_width = llmax(1, (S32)(image_width * f));
            image_height = llmax(1, (S32)(image_height * f));
        }
        if (image_width != rw || image_height != rh)
            return raw->scale(image_width, image_height, true);
        return true;
    }
    setCursor(UI_CURSOR_WAIT);
    if (show_ui) setBalanceVisible(show_balance);
    const U32 point = show_ui ? 3u : (show_hud ? 2u : 1u);
    std::vector<U8> rgb; U32 cw = 0, ch = 0;
    bool got = false;
    for (int attempt = 0; attempt < 3 && !got; ++attempt)   // a frame can skip (swapchain recreate)
    {
        if (!LLVulkanContext::instance().requestCapture(point)) break;
        display(do_rebuild, 1.f, 0, true);   // VK: displayVulkanUI() -- renders + presents one frame
        got = LLVulkanContext::instance().readCapture(rgb, cw, ch);
    }
    setBalanceVisible(true);
    setCursor(UI_CURSOR_ARROW);
    if (!got || !cw || !ch)
    {
        LL_WARNS("Snapshot") << "Vulkan: frame capture failed (point " << point << ")" << LL_ENDL;
        return false;
    }
    LL_INFOS("Snapshot") << "Vulkan snapshot: captured " << cw << "x" << ch << " (point " << point
        << ", ui=" << show_ui << " hud=" << show_hud << ") -> requested " << image_width << "x"
        << image_height << " keep_aspect=" << keep_window_aspect << LL_ENDL;
    if (show_ui)
    {
        // UI is not rescaled: limit the output to the window, as the GL path does
        image_width  = llmin(image_width, (S32)cw);
        image_height = llmin(image_height, (S32)ch);
    }
    if (image_width <= 0 || image_height <= 0) return false;
    // Centre-crop the capture to the requested aspect ratio (keep_window_aspect
    // callers pass window-aspect sizes, so this is then the whole frame).
    const F32 ratio = llmin((F32)cw / image_width, (F32)ch / image_height);
    const S32 crop_w = llclamp((S32)llround(ratio * image_width), 1, (S32)cw);
    const S32 crop_h = llclamp((S32)llround(ratio * image_height), 1, (S32)ch);
    const S32 x0 = ((S32)cw - crop_w) / 2, y0 = ((S32)ch - crop_h) / 2;
    LLImageDataLock lock(raw);
    raw->resize(crop_w, crop_h, 3);
    if (raw->isBufferInvalid()) return false;
    for (S32 y = 0; y < crop_h; ++y)
        memcpy(raw->getData() + (size_t)y * crop_w * 3,
               rgb.data() + ((size_t)(y + y0) * cw + x0) * 3, (size_t)crop_w * 3);
    if (image_width > crop_w || image_height > crop_h)
    {
        LL_WARNS_ONCE("Snapshot") << "Vulkan: snapshot " << image_width << "x" << image_height
            << " exceeds the window capture " << crop_w << "x" << crop_h
            << "; upscaling (high-res offscreen rendering not implemented yet)" << LL_ENDL;
    }
    // Pad the line length to a multiple of 4 bytes (BMP), as the GL path does.
    image_width += (image_width * 3) % 4;
    if (image_width > max_size || image_height > max_size)
    {
        const F32 f = llmin((F32)max_size / image_width, (F32)max_size / image_height);
        image_width = llmax(1, (S32)(image_width * f));
        image_height = llmax(1, (S32)(image_height * f));
    }
    bool ok = true;
    if (image_width != crop_w || image_height != crop_h)
        ok = raw->scale(image_width, image_height, llabs(image_width - crop_w) > 4 || llabs(image_height - crop_h) > 4);
    return ok;
}

bool LLViewerWindow::simpleSnapshot(LLImageRaw* raw, S32 image_width, S32 image_height, const int num_render_passes)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_APP;
    // The world at exactly image_width x image_height with the current camera
    // (360 capture sets aspect/FOV/direction per face), no UI or HUD.
    extern bool vkRenderOffscreenSnapshot(S32 w, S32 h, bool show_hud, std::vector<U8>& rgb,
                                          std::vector<F32>* depth, F32 proj[2], U32 aa_frames);
    extern U32 vkSnapshotAAFrames();
    extern U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames);
    if (!raw || image_width <= 0 || image_height <= 0) return false;
    // Supersampled like the Snapshot floater's (#417) when the accumulators fit; the size
    // itself is the caller's (each 360 face is exact), so only the antialiasing can drop.
    U32 aa = 0;
    {
        S32 fw = image_width, fh = image_height;
        aa = vkSnapshotFit(fw, fh, vkSnapshotAAFrames());
        if (fw != image_width || fh != image_height) aa = 0;
    }
    std::vector<U8> rgb;
    if (!vkRenderOffscreenSnapshot(image_width, image_height, false, rgb, nullptr, nullptr, aa))
        return false;
    LLImageDataLock lock(raw);
    raw->resize(image_width, image_height, 3);
    if (raw->isBufferInvalid()) return false;
    memcpy(raw->getData(), rgb.data(), (size_t)image_width * image_height * 3);
    return true;
}


void LLViewerWindow::destroyWindow()
{
    if (mWindow)
    {
        LLWindowManager::destroyWindow(mWindow);
    }
    mWindow = NULL;
}


void LLViewerWindow::drawMouselookInstructions()
{
    static LLCachedControl<bool> draw_mouselook_instructions(gSavedSettings, "SlipstreamMouselookInstructions", true);
    static LLCachedControl<bool> draw_mouselook_position(gSavedSettings, "SlipstreamMouselookPosition", true);
    // <GMDX> health shows in a damage parcel whatever SlipstreamMouselookPosition says (off by default:
    // the coordinates are gone, the health a combat viewer needs stays)
    LLViewerParcelMgr* vpm = LLViewerParcelMgr::getInstance();
    const bool allow_damage = vpm->allowAgentDamage(gAgent.getRegion(), vpm->getAgentParcel());
    if (draw_mouselook_position || draw_mouselook_instructions || allow_damage)
    {
        // Draw information for mouselook (pos, health)
        const LLFontGL* font = LLFontGL::getFontSansSerifBig();
        //to be at top of viewer
        const S32 INSTRUCTIONS_TOP_PAD = getWorldViewRectScaled().mTop - 15;
        const S32 text_pos_start = getWorldViewRectScaled().getCenterX() - 150;

        if (draw_mouselook_position)
        {
            const LLVector3& vec = gAgent.getPositionAgent();

            font->renderUTF8(
                llformat("X: %.2f", vec.mV[VX]), 0,
                text_pos_start,
                INSTRUCTIONS_TOP_PAD,
                LLColor4(1.0f, 0.5f, 0.5f, 0.5f),
                LLFontGL::HCENTER, LLFontGL::TOP,
                LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
            font->renderUTF8(
                llformat("Y: %.2f", vec.mV[VY]), 0,
                text_pos_start + 100,
                INSTRUCTIONS_TOP_PAD,
                LLColor4(0.5f, 1.0f, 0.5f, 0.5f),
                LLFontGL::HCENTER, LLFontGL::TOP,
                LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
            font->renderUTF8(
                llformat("Z: %.2f", vec.mV[VZ]), 0,
                text_pos_start + 200,
                INSTRUCTIONS_TOP_PAD,
                LLColor4(0.5f, 0.5f, 1.0f, 0.5f),
                LLFontGL::HCENTER, LLFontGL::TOP,
                LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
        }
        if (allow_damage)
        {
            S32 health = -1;
            if (gStatusBar)
            {
                health = gStatusBar->getHealth();
            }
            font->renderUTF8(
                llformat("HP: %d%%", health), 0,
                draw_mouselook_position ? text_pos_start + 300 : getWorldViewRectScaled().getCenterX(),
                INSTRUCTIONS_TOP_PAD,
                LLColor4(1.0f, 1.0f, 1.0f, 0.5f),
                LLFontGL::HCENTER, LLFontGL::TOP,
                LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
        }

        if (draw_mouselook_instructions)
        {
            // Draw instructions for mouselook ("Press ESC to return to World View" partially transparent at the bottom of the screen.)
            static const std::string instructions = LLTrans::getString("LeaveMouselook");

            //to be on top of Bottom bar when it is opened
            const S32 INSTRUCTIONS_BOTTOM_PAD = 50;

            font->renderUTF8(
                instructions, 0,
                getWorldViewRectScaled().getCenterX(),
                getWorldViewRectScaled().mBottom + INSTRUCTIONS_BOTTOM_PAD,
                LLColor4(1.0f, 1.0f, 1.0f, 0.5f),
                LLFontGL::HCENTER, LLFontGL::TOP,
                LLFontGL::BOLD, LLFontGL::DROP_SHADOW);
        }
    }
}

void* LLViewerWindow::getPlatformWindow() const
{
    return mWindow->getPlatformWindow();
}

void LLViewerWindow::focusClient()      const
{
    return mWindow->focusClient();
}

LLRootView* LLViewerWindow::getRootView() const
{
    return mRootView;
}

LLRect LLViewerWindow::getWorldViewRectScaled() const
{
    return mWorldViewRectScaled;
}

S32 LLViewerWindow::getWorldViewHeightScaled() const
{
    return mWorldViewRectScaled.getHeight();
}

S32 LLViewerWindow::getWorldViewWidthScaled() const
{
    return mWorldViewRectScaled.getWidth();
}


S32 LLViewerWindow::getWorldViewHeightRaw() const
{
    return mWorldViewRectRaw.getHeight();
}

S32 LLViewerWindow::getWorldViewWidthRaw() const
{
    return mWorldViewRectRaw.getWidth();
}

S32 LLViewerWindow::getWindowHeightScaled() const
{
    return mWindowRectScaled.getHeight();
}

S32 LLViewerWindow::getWindowWidthScaled() const
{
    return mWindowRectScaled.getWidth();
}

S32 LLViewerWindow::getWindowHeightRaw()    const
{
    return mWindowRectRaw.getHeight();
}

S32 LLViewerWindow::getWindowWidthRaw() const
{
    return mWindowRectRaw.getWidth();
}

void LLViewerWindow::setup2DRender()
{
    // setup ortho camera
    gl_state_for_2d(mWindowRectRaw.getWidth(), mWindowRectRaw.getHeight());
    setup2DViewport();
}

void LLViewerWindow::setup2DViewport(S32 x_offset, S32 y_offset)
{
    gGLViewport[0] = mWindowRectRaw.mLeft + x_offset;
    gGLViewport[1] = mWindowRectRaw.mBottom + y_offset;
    gGLViewport[2] = mWindowRectRaw.getWidth();
    gGLViewport[3] = mWindowRectRaw.getHeight();
    // The VK present path sets the viewport as pipeline dynamic state from the
    // swapchain extent; gGLViewport[] is bookkeeping for CPU consumers.
}


// Vulkan: the 3D camera the edit-overlay pass drew with (VK clip convention),
// restored by setup3DRender after LL helpers switch to 2D mid-render (LLManip's XYZ
// readout). setPerspective builds GL matrices and calls glViewport -- neither valid.
F32  LLViewerWindow::sVKOverlayProj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
F32  LLViewerWindow::sVKOverlayView[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

void LLViewerWindow::setVKOverlayCamera(const F32* proj, const F32* view)
{
    std::memcpy(sVKOverlayProj, proj, sizeof(sVKOverlayProj));
    std::memcpy(sVKOverlayView, view, sizeof(sVKOverlayView));
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.loadMatrix(sVKOverlayProj);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(sVKOverlayView);
}

void LLViewerWindow::setup3DRender()
{
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.loadMatrix(sVKOverlayProj);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(sVKOverlayView);
}

void LLViewerWindow::setup3DViewport(S32 x_offset, S32 y_offset)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    gGLViewport[0] = mWorldViewRectRaw.mLeft + x_offset;
    gGLViewport[1] = mWorldViewRectRaw.mBottom + y_offset;
    gGLViewport[2] = mWorldViewRectRaw.getWidth();
    gGLViewport[3] = mWorldViewRectRaw.getHeight();
    // Viewport is VK dynamic state (see setup2DViewport).
}

void LLViewerWindow::revealIntroPanel()
{
    if (mProgressView)
    {
        mProgressView->revealIntroPanel();
    }
}

void LLViewerWindow::initTextures(S32 location_id)
{
    if (mProgressView)
    {
        mProgressView->initTextures(location_id, LLGridManager::getInstance()->isInProductionGrid());
    }
}

void LLViewerWindow::setShowProgress(const bool show)
{
    if (mProgressView)
    {
        mProgressView->setVisible(show);
    }
}

void LLViewerWindow::setStartupComplete()
{
    if (mProgressView)
    {
        mProgressView->setStartupComplete();
    }
}

bool LLViewerWindow::getShowProgress() const
{
    return (mProgressView && mProgressView->getVisible());
}

void LLViewerWindow::setProgressString(const std::string& string)
{
    if (mProgressView)
    {
        mProgressView->setText(string);
    }
}

void LLViewerWindow::setProgressMessage(const std::string& msg)
{
    if(mProgressView)
    {
        mProgressView->setMessage(msg);
    }
}

void LLViewerWindow::setProgressPercent(const F32 percent)
{
    if (mProgressView)
    {
        mProgressView->setPercent(percent);
    }
}

void LLViewerWindow::setProgressCancelButtonVisible( bool b, const std::string& label )
{
    LLFloaterProgressView* pProgFloater = LLFloaterReg::findTypedInstance<LLFloaterProgressView>("progress_view");
    if(pProgFloater)
    {
        pProgFloater->setProgressCancelButtonVisible(b, label);
    }
    if (mProgressView)
    {
        mProgressView->setCancelButtonVisible( b, label );
    }
}

void LLViewerWindow::setBalanceVisible(bool visible)
{
    if (gStatusBar)
    {
        gStatusBar->setBalanceVisible(visible);
    }
}

LLProgressView *LLViewerWindow::getProgressView() const
{
    return mProgressView;
}

void LLViewerWindow::dumpState()
{
    LL_INFOS() << "LLViewerWindow Active " << S32(mActive) << LL_ENDL;
    LL_INFOS() << "mWindow visible " << S32(mWindow->getVisible())
        << " minimized " << S32(mWindow->getMinimized())
        << LL_ENDL;
}

void LLViewerWindow::initFonts(F32 zoom_factor)
{
    // Initialize with possibly different zoom factor

    LLFontManager::initClass();

    LLFontGL::initClass( gSavedSettings.getF32("FontScreenDPI"),
                                mDisplayScale.mV[VX] * zoom_factor,
                                mDisplayScale.mV[VY] * zoom_factor,
                                gDirUtilp->getAppRODataDir(), true,
                                gSavedSettings.getString("FSFontSettingsFile"),
                                llclamp(gSavedSettings.getF32("FSFontSizeAdjustment"), -2.f, 4.f));
}

void LLViewerWindow::requestResolutionUpdate()
{
    mResDirty = true;
}

static LLTrace::BlockTimerStatHandle FTM_WINDOW_CHECK_SETTINGS("Window Settings");

void LLViewerWindow::checkSettings()
{
    LL_RECORD_BLOCK_TIME(FTM_WINDOW_CHECK_SETTINGS);
    if (mStatesDirty)
    {
        mStatesDirty = false;
    }

    // We want to update the resolution AFTER the states getting refreshed not before.
    if (mResDirty)
    {
        reshape(getWindowWidthRaw(), getWindowHeightRaw());
        mResDirty = false;
    }

    if (mOverlayRelayoutFrames > 0 && --mOverlayRelayoutFrames == 0)
    {
        // Window geometry has been stable for a few frames: centre alerts and modals
        // against the final size so none is stranded off-screen (#691).
        for (auto& elem : LLNotificationsUI::LLChannelManager::getInstance()->getChannelList())
        {
            if (LLNotificationsUI::LLScreenChannelBase* chan = elem.channel.get())
            {
                chan->relayout();
            }
        }
        LLModalDialog::recenterAll();
    }

    if (mFontSetDirty)
    {
        mFontSetDirty = false;
        applyFontSet();
    }
}

void LLViewerWindow::applyFontSet()
{
    LLFontGL::switchFontSet(gSavedSettings.getString("FSFontSettingsFile"),
                            llclamp(gSavedSettings.getF32("FSFontSizeAdjustment"), -2.f, 4.f));

    // Same relayout a UI-scale change does: drop cached text geometry, force every
    // view (text reflows, layout stacks, favorites bar) to reshape at its current size.
    LLFontGL::sResolutionGeneration++;
    LLView::sForceReshape = true;
    mRootView->reshape(mRootView->getRect().getWidth(), mRootView->getRect().getHeight());
    LLPanelLogin::reshapePanel();
    LLView::sForceReshape = false;
    LLHUDObject::reshapeAll();
}

F32 LLViewerWindow::getWorldViewAspectRatio() const
{
    F32 world_aspect = (F32)mWorldViewRectRaw.getWidth() / (F32)mWorldViewRectRaw.getHeight();
    return world_aspect;
}

F32 LLViewerWindow::getHudAspectRatio() const
{
    const S32 h = mHudRectRaw.getHeight();
    if (h <= 0 || mHudRectRaw.getWidth() <= 0) return getWorldViewAspectRatio();   // before the first update
    return (F32)mHudRectRaw.getWidth() / (F32)h;
}

F32 LLViewerWindow::getHudProjectionAspect() const
{
    return mHudAspectOverride > 0.f ? mHudAspectOverride : getHudAspectRatio();
}

// #919: the HUD layout rect = LL's world view rect, the "world_view_rect" placeholder
// (main_view.xml: the world panel under the status bar container, which holds the menu
// bar and -- ShowNavbarNavigationPanel -- the navigation / favourites bars; hidden UI
// collapses it to the whole window). Same raw conversion as updateWorldViewRect's
// placeholder branch. The world view itself stays the whole window under Vulkan (see
// updateUI) until #918 sizes the 3D viewport to this rect.
void LLViewerWindow::updateHudRect()
{
    LLRect raw = mWindowRectRaw;
    if (LLView* ph = mWorldViewPlaceholder.get())
    {
        LLRect r = ph->calcScreenRect();
        r.mTop = llmax(r.mTop, r.mBottom + 1);
        r.mRight = llmax(r.mRight, r.mLeft + 1);
        r.mLeft = ll_round((F32)r.mLeft * mDisplayScale.mV[VX]);
        r.mRight = ll_round((F32)r.mRight * mDisplayScale.mV[VX]);
        r.mBottom = ll_round((F32)r.mBottom * mDisplayScale.mV[VY]);
        r.mTop = ll_round((F32)r.mTop * mDisplayScale.mV[VY]);
        // Inside the window (the placeholder rounds up to cover the edge: llceil in
        // reshape), and never degenerate (a collapsed layout mid-animation, a minimised
        // window): fall back to the whole window.
        r.intersectWith(mWindowRectRaw);
        if (r.getWidth() >= 16 && r.getHeight() >= 16) raw = r;
    }
    const LLRect scaled = calcScaledRect(raw, mDisplayScale);
    if (raw == mHudRectRaw && scaled == mHudRectScaled) return;
    mHudRectRaw = raw;
    mHudRectScaled = scaled;
    LL_INFOS("HUD") << "HUD rect " << raw.mLeft << "," << raw.mBottom << " " << raw.getWidth() << "x" << raw.getHeight()
                    << " raw px (window " << mWindowRectRaw.getWidth() << "x" << mWindowRectRaw.getHeight()
                    << ", top inset " << (mWindowRectRaw.mTop - raw.mTop) << " px), scaled " << scaled.mLeft << ","
                    << scaled.mBottom << " " << scaled.getWidth() << "x" << scaled.getHeight() << ", aspect "
                    << getHudAspectRatio() << ", display scale " << mDisplayScale << ", fullscreen "
                    << (mWindow && mWindow->getFullscreen() ? 1 : 0) << LL_ENDL;
}

void LLViewerWindow::getHudClipRemap(F32 out[16]) const
{
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.f : 0.f;
    const F32 W = (F32)llmax(mWindowRectRaw.getWidth(), 1);
    const F32 H = (F32)llmax(mWindowRectRaw.getHeight(), 1);
    const F32 x = (F32)(mHudRectRaw.mLeft - mWindowRectRaw.mLeft);
    const F32 y = (F32)(mWindowRectRaw.mTop - mHudRectRaw.mTop);   // top-left origin
    const F32 w = (F32)mHudRectRaw.getWidth(), h = (F32)mHudRectRaw.getHeight();
    if (w <= 0.f || h <= 0.f) return;
    out[0]  = w / W;
    out[5]  = h / H;
    out[12] = (2.f * x + w) / W - 1.f;
    out[13] = (2.f * y + h) / H - 1.f;
}

void LLViewerWindow::calcDisplayScale()
{
    F32 ui_scale_factor = llclamp(gSavedSettings.getF32("UIScaleFactor") * mWindow->getSystemUISize(), MIN_UI_SCALE, MAX_UI_SCALE);
    LLVector2 display_scale;
    display_scale.setVec(llmax(1.f / mWindow->getPixelAspectRatio(), 1.f), llmax(mWindow->getPixelAspectRatio(), 1.f));
    display_scale *= ui_scale_factor;

    // limit minimum display scale
    if (display_scale.mV[VX] < MIN_DISPLAY_SCALE || display_scale.mV[VY] < MIN_DISPLAY_SCALE)
    {
        display_scale *= MIN_DISPLAY_SCALE / llmin(display_scale.mV[VX], display_scale.mV[VY]);
    }

    if (display_scale != mDisplayScale)
    {
        LL_INFOS() << "Setting display scale to " << display_scale << " for ui scale: " << ui_scale_factor << LL_ENDL;

        mDisplayScale = display_scale;
        // Init default fonts
        initFonts();
    }
}

//static
LLRect  LLViewerWindow::calcScaledRect(const LLRect & rect, const LLVector2& display_scale)
{
    LLRect res = rect;
    res.mLeft = ll_round((F32)res.mLeft / display_scale.mV[VX]);
    res.mRight = ll_round((F32)res.mRight / display_scale.mV[VX]);
    res.mBottom = ll_round((F32)res.mBottom / display_scale.mV[VY]);
    res.mTop = ll_round((F32)res.mTop / display_scale.mV[VY]);

    return res;
}

S32 LLViewerWindow::getChatConsoleBottomPad()
{
    S32 offset = 0;

    if(gToolBarView)
        offset += gToolBarView->getBottomToolbar()->getRect().getHeight();

    return offset;
}

void LLViewerWindow::applyNearbyChatConsoleSettings()
{
    if (!mNearbyChatConsole)
    {
        return;
    }
    const F32 persist = llclamp(gSavedSettings.getF32("NearbyChatConsolePersistTime"), 1.f, 3600.f);
    const F32 fade = llclamp(gSavedSettings.getF32("NearbyChatConsoleFadeTime"), 0.f, persist);
    mNearbyChatConsole->setLinePersistTime(persist);
    mNearbyChatConsole->setFadeDuration(fade);
    mNearbyChatConsole->setMaxLines(llclamp(gSavedSettings.getS32("NearbyChatConsoleMaxLines"), 1, 200));
    mNearbyChatConsole->setShowBackground(gSavedSettings.getBOOL("NearbyChatConsoleBackground"));

    const LLFontGL* font = nullptr;
    switch (gSavedSettings.getS32("NearbyChatConsoleFontSize"))
    {
        case 0:  font = LLFontGL::getFontSansSerifSmall(); break;
        case 1:  font = LLFontGL::getFontSansSerif();      break;
        case 2:  font = LLFontGL::getFontSansSerifBig();   break;
        case 3:  font = LLFontGL::getFontSansSerifHuge();  break;
        default: font = LLViewerChat::getChatFont();       break;  // -1: same as chat text
    }
    mNearbyChatConsole->setFont(font);
}

LLRect LLViewerWindow::getChatConsoleRect()
{
    LLRect full_window(0, getWindowHeightScaled(), getWindowWidthScaled(), 0);
    LLRect console_rect = full_window;

    const S32 CONSOLE_PADDING_TOP = 24;
    const S32 CONSOLE_PADDING_LEFT = 24;
    const S32 CONSOLE_PADDING_RIGHT = 10;

    console_rect.mTop    -= CONSOLE_PADDING_TOP;
    console_rect.mBottom += getChatConsoleBottomPad();

    console_rect.mLeft   += CONSOLE_PADDING_LEFT;

    static const bool CHAT_FULL_WIDTH = gSavedSettings.getBOOL("ChatFullWidth");

    if (CHAT_FULL_WIDTH)
    {
        console_rect.mRight -= CONSOLE_PADDING_RIGHT;
    }
    else
    {
        // Make console rect somewhat narrow so having inventory open is
        // less of a problem.
        console_rect.mRight  = console_rect.mLeft + 2 * getWindowWidthScaled() / 3;
    }

    return console_rect;
}

void LLViewerWindow::reshapeStatusBarContainer()
{
    S32 new_height = mStatusBarContainer->getRect().getHeight();
    S32 new_width = mStatusBarContainer->getRect().getWidth();

    if (gSavedSettings.getBOOL("ShowNavbarNavigationPanel"))
    {
        // Navigation bar is outside visible area, expand status_bar_container to show it
        new_height += mNavBarContainer->getRect().getHeight();
    }
    else
    {
        // collapse status_bar_container
        new_height -= mNavBarContainer->getRect().getHeight();
    }
    mStatusBarContainer->reshape(new_width, new_height, true);
}

void LLViewerWindow::resetStatusBarContainer()
{
    LLNavigationBar* navbar = LLNavigationBar::getInstance();
    if (gSavedSettings.getBOOL("ShowNavbarNavigationPanel") || navbar->getVisible())
    {
        // was previously showing navigation bar
        S32 new_height = mStatusBarContainer->getRect().getHeight();
        S32 new_width = mStatusBarContainer->getRect().getWidth();
        new_height -= mNavBarContainer->getRect().getHeight();
        mStatusBarContainer->reshape(new_width, new_height, true);
    }
}
//----------------------------------------------------------------------------


void LLViewerWindow::setUIVisibility(bool visible)
{
    mUIVisible = visible;

    if (!visible)
    {
        gAgentCamera.changeCameraToThirdPerson(false);
        gFloaterView->hideAllFloaters();
    }
    else
    {
        gFloaterView->showHiddenFloaters();
    }

    if (gToolBarView)
    {
        gToolBarView->setToolBarsVisible(visible);
    }

    LLNavigationBar::getInstance()->setVisible(visible ? gSavedSettings.getBOOL("ShowNavbarNavigationPanel") : false);
    LLPanelTopInfoBar::getInstance()->setVisible(visible? gSavedSettings.getBOOL("ShowMiniLocationPanel") : false);
    mStatusBarContainer->setVisible(visible);
}

bool LLViewerWindow::getUIVisibility()
{
    return mUIVisible;
}

////////////////////////////////////////////////////////////////////////////
//
// LLPickInfo
//
LLPickInfo::LLPickInfo()
    : mKeyMask(MASK_NONE),
      mPickCallback(NULL),
      mPickType(PICK_INVALID),
      mWantSurfaceInfo(false),
      mObjectFace(-1),
      mUVCoords(-1.f, -1.f),
      mSTCoords(-1.f, -1.f),
      mXYCoords(-1, -1),
      mIntersection(),
      mNormal(),
      mTangent(),
      mBinormal(),
      mHUDIcon(NULL),
      mPickTransparent(false),
      mPickRigged(false),
      mPickParticle(false)
{
}

LLPickInfo::LLPickInfo(const LLCoordGL& mouse_pos,
    MASK keyboard_mask,
    bool pick_transparent,
    bool pick_rigged,
    bool pick_particle,
    bool pick_reflection_probe,
    bool pick_uv_coords,
    bool pick_unselectable,
    void (*pick_callback)(const LLPickInfo& pick_info))
    : mMousePt(mouse_pos),
    mKeyMask(keyboard_mask),
    mPickCallback(pick_callback),
    mPickType(PICK_INVALID),
    mWantSurfaceInfo(pick_uv_coords),
    mObjectFace(-1),
    mUVCoords(-1.f, -1.f),
    mSTCoords(-1.f, -1.f),
    mXYCoords(-1, -1),
    mNormal(),
    mTangent(),
    mBinormal(),
    mHUDIcon(NULL),
    mPickTransparent(pick_transparent),
    mPickRigged(pick_rigged),
    mPickParticle(pick_particle),
    mPickReflectionProbe(pick_reflection_probe),
      mPickUnselectable(pick_unselectable)
{
    mPickClick = LLCleanCursorPick::inClick();   // (#702) a click's pick, even when fetched later
}

F32 LLPickInfo::getPickDepth() const
{
    static LLCachedControl<bool> disable_camera_constraints(gSavedSettings, "DisableCameraConstraints", false);
    if (mPickCallback != LLToolCamera::pickCallback || !disable_camera_constraints)
    {
        return 512.f;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    F32 forward = gViewerWindow->mouseDirectionGlobal(mMousePt.mX, mMousePt.mY) * camera->getAtAxis();
    if (forward <= 0.f)
    {
        return 512.f;
    }

    static LLCachedControl<F32> render_far(gSavedSettings, "RenderFarClip", 128.f);
    F32 far_distance = llclamp(LLVulkanContext::instance().vramDrawDistance(render_far), 8.f, 8192.f);
    if (far_distance <= camera->getNear() + 1.f)
    {
        far_distance = camera->getNear() + 256.f;
    }
    far_distance = llmax(far_distance * 2.f, MAX_FAR_CLIP * 2.f);

    return llmax(512.f, (far_distance - camera->getNear()) / forward);
}

void LLPickInfo::fetchResults()
{
    VK_HITCH_SCOPE("pick_fetchResults");   // (#757 crash breadcrumb: the pick consumers below)
    // A click's async pick (performPick, a frame later) keeps the click's answer policy
    // (#702): the exact ray or the CPU pick, never a trailing hover answer.
    LLCleanCursorPick::ClickScope click_scope(mPickClick && !LLCleanCursorPick::inClick());
    S32 face_hit = -1;

    LLVector4a intersection, normal;
    LLVector4a tangent;

    LLVector2 uv;

    const F32 pick_depth = getPickDepth();
    LLHUDIcon* hit_icon = gViewerWindow->cursorIntersectIcon(mMousePt.mX, mMousePt.mY, pick_depth, &intersection);

    LLVector4a origin;
    origin.load3(LLViewerCamera::getInstance()->getOrigin().mV);
    F32 icon_dist = 0.f;
    LLVector4a start;
    LLVector4a end;
    LLVector4a particle_end;

    if (hit_icon)
    {
        LLVector4a delta;
        delta.setSub(intersection, origin);
        icon_dist = delta.getLength3().getF32();
    }

    LLViewerObject* hit_object = gViewerWindow->cursorIntersect(mMousePt.mX, mMousePt.mY, pick_depth,
                                    nullptr, -1, mPickTransparent, mPickRigged, mPickUnselectable, mPickReflectionProbe, &face_hit, &mGLTFNodeIndex, &mGLTFPrimitiveIndex,
                                &intersection, &uv, &normal, &tangent, &start, &end);

    mPickPt = mMousePt;

// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
    if ( (RlvActions::hasBehaviour(RLV_BHVR_SETOVERLAY)) && (hit_object) && (!hit_object->isHUDAttachment()) )
    {
        std::list<RlvOverlayEffect*> effects;
        LLVfxManager::instance().getEffects<RlvOverlayEffect>(effects);
        for (const RlvOverlayEffect* pEffect : effects)
        {
            if (pEffect->getEnabled() && pEffect->hitTest(mMousePt))
            {
                hit_object = nullptr;
                break;
            }
        }
    }
// [/RLVa:KB]

    U32 te_offset = face_hit > -1 ? face_hit : 0;

    if (mPickParticle)
    { //get the end point of line segement to use for particle raycast
        if (hit_object)
        {
            particle_end = intersection;
        }
        else
        {
            particle_end = end;
        }
    }

    LLViewerObject* objectp = hit_object;


    LLVector4a delta;
    delta.setSub(origin, intersection);

    if (hit_icon &&
        (!objectp ||
        icon_dist < delta.getLength3().getF32()))
    {
        // was this name referring to a hud icon?
        mHUDIcon = hit_icon;
        mPickType = PICK_ICON;
        mPosGlobal = mHUDIcon->getPositionGlobal();

    }
    else if (objectp)
    {
        if( objectp->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH )
        {
            // Hit land
            mPickType = PICK_LAND;
            mObjectID.setNull(); // land has no id

            // put global position into land_pos
            LLVector3d land_pos;
            if (!gViewerWindow->mousePointOnLandGlobal(mPickPt.mX, mPickPt.mY, &land_pos, mPickUnselectable))
            {
                // The selected point is beyond the draw distance or is otherwise
                // not selectable. Return before calling mPickCallback().
                return;
            }

            // Fudge the land focus a little bit above ground.
            mPosGlobal = land_pos + LLVector3d::z_axis * 0.1f;
        }
        else
        {
            if(isFlora(objectp))
            {
                mPickType = PICK_FLORA;
            }
            else
            {
                mPickType = PICK_OBJECT;
            }

            LLVector3 v_intersection(intersection.getF32ptr());

            mObjectOffset = gAgentCamera.calcFocusOffset(objectp, v_intersection, mPickPt.mX, mPickPt.mY);
            mObjectID = objectp->mID;
            mObjectFace = (te_offset == NO_FACE) ? -1 : (S32)te_offset;
            mPickHUD = objectp->isHUDAttachment();


            mPosGlobal = gAgent.getPosGlobalFromAgent(v_intersection);

            if (mWantSurfaceInfo)
            {
                getSurfaceInfo();
            }
        }
    }

    if (mPickParticle)
    { //search for closest particle to click origin out to intersection point
        // A pick that asks for particles is the right click's pickImmediate (its menu reads
        // the owner at once): a synchronous GPU answer (#481).
        gPipeline.lineSegmentIntersectParticle(start, particle_end, /*sync*/ true, NULL,
                                               &mParticleOwnerID, &mParticleSourceID);
    }

    if (mPickCallback)
    {
        mPickCallback(*this);
    }
}

LLPointer<LLViewerObject> LLPickInfo::getObject() const
{
    return gObjectList.findObject( mObjectID );
}

void LLPickInfo::updateXYCoords()
{
    if (mObjectFace > -1)
    {
        // The object can be gone (killed since the pick) and the face can have no TE.
        LLPointer<LLViewerObject> objectp = getObject();
        const LLTextureEntry* tep = objectp.notNull() && mObjectFace < objectp->getNumTEs() ? objectp->getTE(mObjectFace) : nullptr;
        if (!tep) return;
        LLPointer<LLViewerTexture> imagep = LLViewerTextureManager::getFetchedTexture(tep->getID());
        if(mUVCoords.mV[VX] >= 0.f && mUVCoords.mV[VY] >= 0.f && imagep.notNull())
        {
            mXYCoords.mX = ll_round(mUVCoords.mV[VX] * (F32)imagep->getWidth());
            mXYCoords.mY = ll_round((1.f - mUVCoords.mV[VY]) * (F32)imagep->getHeight());
        }
    }
}

void LLPickInfo::getSurfaceInfo()
{
    // The face the object pick reported (#702): intersect it first -- one face instead of
    // all of them -- then the whole object if the ray misses it there.
    const S32 face_hint = mObjectFace;
    // set values to uninitialized - this is what we return if no intersection is found
    mObjectFace   = -1;
    mUVCoords     = LLVector2(-1, -1);
    mSTCoords     = LLVector2(-1, -1);
    mXYCoords     = LLCoordScreen(-1, -1);
    mIntersection = LLVector3(0,0,0);
    mNormal       = LLVector3(0,0,0);
    mBinormal     = LLVector3(0,0,0);
    mTangent      = LLVector4(0,0,0,0);

    LLVector4a tangent;
    LLVector4a intersection;
    LLVector4a normal;

    tangent.clear();
    normal.clear();
    intersection.clear();

    LLViewerObject* objectp = getObject();

    if (objectp)
    {
        auto pick = [&](S32 face, LLVector4a* seg_start, LLVector4a* seg_end)
        {
            return gViewerWindow->cursorIntersect(ll_round((F32)mMousePt.mX), ll_round((F32)mMousePt.mY), llmax(1024.f, getPickDepth()),
                                                  objectp, face, mPickTransparent, mPickRigged, mPickUnselectable, mPickReflectionProbe,
                                                  &mObjectFace,
                                                  &mGLTFNodeIndex,
                                                  &mGLTFPrimitiveIndex,
                                                  &intersection,
                                                  &mSTCoords,
                                                  &normal,
                                                  &tangent,
                                                  seg_start, seg_end);
        };
        LLVector4a seg_start, seg_end;
        seg_start.clear(); seg_end.clear();
        // (The GPU pick answers the object's surface the all-objects pick just reported --
        // a trailing hover answer within a few pixels of this ray included, #702 / #704.)
        bool hit = face_hint >= 0 && !objectp->mGLTFAsset && pick(face_hint, &seg_start, &seg_end) != nullptr;
        if (!hit) hit = pick(-1, &seg_start, &seg_end) != nullptr;
        if (hit)
        {
            // The texture coordinates: the UV the renderer samples there (planar texgen,
            // TE / glTF transform, texture animation -- LL's surfaceToTexture of what is
            // drawn), else LL's transform of the surface ST.
            if (!LLCleanCursorPick::surfaceTexture(objectp, mObjectFace, mUVCoords, nullptr)
                && objectp->mDrawable.notNull() && mObjectFace > -1)
            {
                LLFace* facep = objectp->mDrawable->getFace(mObjectFace);
                if (facep)
                {
                    mUVCoords = facep->surfaceToTexture(mSTCoords, intersection, normal);
                }
            }

            mIntersection.set(intersection.getF32ptr());
            mNormal.set(normal.getF32ptr());
            mTangent.set(tangent.getF32ptr());

            //extrapoloate binormal from normal and tangent

            LLVector4a binormal;
            binormal.setCross3(normal, tangent);
            binormal.mul(tangent.getF32ptr()[3]);

            mBinormal.set(binormal.getF32ptr());

            mBinormal.normalize();
            mNormal.normalize();
            mTangent.normalize();

            // and XY coords:
            updateXYCoords();

        }
    }
}

//static
bool LLPickInfo::isFlora(LLViewerObject* object)
{
    if (!object) return false;

    LLPCode pcode = object->getPCode();

    if( (LL_PCODE_LEGACY_GRASS == pcode)
        || (LL_PCODE_LEGACY_TREE == pcode)
        || (LL_PCODE_TREE_NEW == pcode))
    {
        return true;
    }
    return false;
}
