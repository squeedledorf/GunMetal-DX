/**
 * @file llappviewer.cpp
 * @brief The LLAppViewer class definitions
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2012, Linden Research, Inc.
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
#include "llgpuinfo.h"
#include "llvkhitch.h"
#include "llvkbench.h"
#include "llvkautotune.h"   // #422 auto-tuner (frame tick)
#include "llvkperfcapture.h"   // #966 Performance window timings (frame tick)
#include "llgl.h"

#include "llappviewer.h"
#include "gmdxcrashreport.h" // <GMDX>
#include "llcrossingtrace.h"
#include "llmemdiag.h"               // crash / disconnect / memory evidence (#700)
#include "llmemtrend.h"              // periodic leak report (#883)
#include "llmemtrendprobes.h"
#include "slfxstack.h"   // built-in effects stack (#544)
#include "llpluginprocessparent.h"
#include "fsareasearch.h"
#include "fsfloaterposer.h"
#include "slposeik.h"
#include "gmdxrezfx.h"   // <GMDX> rez effects: the transporter beam (world overlay)
#include "llfloaterpathfindingobjects.h"
#include "llworldclock.h"

#include "fsradar.h"
#include "llthreadaffinity.h"
#include "llcleandebug.h"   // CLEAN_DEBUG gate for #FrameProf idle-vs-display timing

// Viewer includes
#include "llversioninfo.h"
#include "llfeaturemanager.h"
#include "lluictrlfactory.h"
#include "lltexteditor.h"
#include "llenvironment.h"
#include "llerrorcontrol.h"
#include "lleventtimer.h"
#include "llfile.h"
#include "llviewertexturelist.h"
#include "llgroupmgr.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llagentlanguage.h"
#include "llagentui.h"
#include "llagentwearables.h"
#include "lldirpicker.h"
#include "llfloaterimcontainer.h"
#include "llfloaterpreference.h"
#include "llimprocessing.h"
#include "llwindow.h"
#include "llviewerstats.h"
#include "llviewerstatsrecorder.h"
#include "llviewerinventory.h"         // LLInventoryCopyPacer (#538)
#include "llchunkedpurge.h"
#include "llkeyconflict.h" // for legacy keybinding support, remove later
#include "llmarketplacefunctions.h"
#include "llmarketplacenotifications.h"
#include "llmd5.h"
#include "llmeshrepository.h"
#include "llpanelpreferencegamecontrol.h"
#include "ssfloatergamecontroldebug.h"
#include "llsetkeybinddialog.h"
#include "llpumpio.h"
#include "llmimetypes.h"
#include "llslurl.h"
#include "llstartup.h"
#include "llfocusmgr.h"
#include "llurlfloaterdispatchhandler.h"
#include "llviewerjoystick.h"
#include "llgamecontrol.h"
#include "llcalc.h"
#include "llconversationlog.h"
#if LL_WINDOWS
#include "lldxhardware.h"
#endif
#include "lltexturestats.h"
#include "lltrace.h"
#include "lltracethreadrecorder.h"
#include "llviewerwindow.h"
#include "llfloater.h"
#include "vkgfx/llvulkancontext.h"    // EXPERIMENTAL Vulkan integration
#include "vkgfx/vk_pipeline.h"         // #462 vkSetPipelineCompileThreads
#include "vk_world_shaders_hash.h"  // embedded shader content hash (pipeline-cache build tag; generated at build time)
#include "llcleancursorpick.h"             // GPU raster cursor pick request (#702)
#include "llcleanpartpick.h"               // GPU particle pick request (#481)
#include "sscombatray.h"                   // <SS:Skoomastream> OTS / aim rays (aux picks)
#include "llcleanacoustics.h"               // acoustics probe request + EFX driver (#683)
#include "slweatheraudio.h"                // weather ambience loops (#783)
#include "slweatherregion.h"               // region / parcel weather source (#783 M2)
#include "slparcelspeakers.h"              // parcel speaker registry (#892)
#include "slparcelspeakeraudio.h"          // parcel stream through the speakers (#892)
#include "vkgfx/vk_common.h"          // gVkDeviceLostHandler, gVkBreadcrumbsWanted, gVkMemoryReportWanted, gVkTexPoolConfig, vkDeviceIsLost
#include "lldate.h"                   // device-lost report time stamp (#135)
#include "llframetimer.h"
#include "llthread.h"                 // on_main_thread
#include <mutex>
#if LL_WINDOWS
#include "llwin32headers.h"           // CreateProcessW: restart after a device loss (#135)
#else
#include <sys/types.h>
#include <unistd.h>                   // fork / execv: restart after a device loss (#135)
#endif
#include "vkgfx/llcleanvolfogvk.h"   // <OCOL> OCOL mist origin
#include "vkgfx/vk_descpool.h"        // vkdSetPushConstantCap (#238 debug)
#include "vkgfx/vk_requirements.h"    // #526 Vulkan minimum: pre-flight check + user message
#include "llcleanmeshasset.h"         // VK synth-test: clean-store cube injection
#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleaningest.h"          // VK login ingest tick: selfRegisterSweep
#include "llvolume.h"                 // LLVolume / LLVolumeParams (synth cube)
#include "llviewertexture.h"          // VK synth-test: checkerboard texture injection
#include "llimage.h"                  // LLImageRaw (synth checkerboard pixels)
#include "llmemorygovernor.h"          // RAM #262: system memory governor
#include <cmath>                      // EXPERIMENTAL Vulkan: world MVP/frustum math
#include <chrono>                     // RenderFPSLimit pacing
#include <filesystem>
#include <thread>
#include "llwindowsdl.h"             // EXPERIMENTAL Vulkan: raw SDL_Window access
#include "llfontgl.h"                // EXPERIMENTAL Vulkan: real LLFontGL text through the seam
#include "llfontfreetype.h"         // EXPERIMENTAL Vulkan: LLFontManager font system init
#include "lltexturedata.h"
#include "llrender2dutils.h"        // EXPERIMENTAL Vulkan: gl_rect_2d real UI primitives
#include "llviewerdisplay.h"
#include "lldynamictexture.h"   // #83: VK dynamic-texture pass
#include "llviewermedia.h"
#include "llviewerparcelaskplay.h"
#include "llviewerparcelmedia.h"
#include "llrender.h"
#include "llviewermediafocus.h"
#include "llcontrolavatar.h"
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llworldmap.h"
#include "llmutelist.h"
#include "llviewerhelp.h"
#include "lluicolortable.h"
#include "llurldispatcher.h"
#include "llurlhistory.h"
#include "llrender.h"
#include "llteleporthistory.h"
#include "lltoast.h"
#include "llsdutil_math.h"
#include "lllocationhistory.h"
#include "llfasttimerview.h"
#include "llvector4a.h"
#include "llviewermenufile.h"
#include "llvoicechannel.h"
#include "llvoavatarself.h"
#include "llurlmatch.h"
#include "lltextutil.h"
#include "lllogininstance.h"
#include "llprogressview.h"
#include "llvocache.h"
#include "llcullcamera.h"
#include "llviewerregion.h"   // VK VOCache-cull driver (object-cache drain)
#include "lldrawable.h"       // LLDrawable::incrementVisible() for the VK cull driver
#include "lldiskcache.h"
#include "llvopartgroup.h"
// [SL:KB] - Patch: Appearance-Misc | Checked: 2013-02-12 (Catznip-3.4)
#include "llappearancemgr.h"
// [/SL:KB]
// [RLVa:KB] - Checked: 2010-05-03 (RLVa-1.2.0g)
#include "rlvactions.h"
#include "rlvlocks.h"   // gRlvAttachmentLocks (Hide Selected, #29)
#include "rlveffects.h"
#include "rlvhandler.h"
// [/RLVa:KB]
#include "slaoengine.h"
#include "slbridge.h"
#include "slswim.h"
#include "sldatausage.h"
#include "sldiscordpresence.h"
#include "slupdatechecker.h"
#include "slrevocationupdater.h"
#include "slsettingsmigration.h"
#include "slenv.h"
#include "slriskysettings.h"   // #736 crash-loop guard

#include "llweb.h"
#include "llspellcheck.h"
#include "llavatarrenderinfoaccountant.h"
#include "lllocalbitmaps.h"
#include "llperfstats.h"
#include "llgltfmateriallist.h"
#include "llgltfmaterial.h"

// Linden library includes
#include "llavatarnamecache.h"
#include "lldiriterator.h"
#include "llexperiencecache.h"
#include "llimagej2c.h"
#include "llmemory.h"
#include "llprimitive.h"
#include "llurlaction.h"
#include "llurlentry.h"
#include "llvolumemgr.h"
#include "llxfermanager.h"
#include "llphysicsextensions.h"

#include "llnotificationmanager.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"

#include "llleap.h"
#include "stringize.h"
#include "llcoros.h"
#include "llexception.h"
#include "cef/dullahan_version.h"
#include "vlc/libvlc_version.h"

#if LL_DARWIN && !LL_SDL_WINDOW
#include "llwindowmacosx.h"
#endif

#if LL_SDL_WINDOW
#include "llwindowsdl.h"
#endif

// Third party library includes
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <boost/throw_exception.hpp>

#if LL_WINDOWS
#   include <share.h> // For _SH_DENYWR in processMarkerFiles
#else
#   include <sys/file.h> // For processMarkerFiles
#endif

#include "llapr.h"
#include <boost/lexical_cast.hpp>

#include "llviewerinput.h"
#include "lllfsthread.h"
#include "llworkerthread.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llimageworker.h"
#include "llcorehttputil.h"     // asset-perf timeline counters (#377)
#include "llevents.h"

// The files below handle dependencies from cleanup.
#include "llkeyframemotion.h"
#include "llworldmap.h"
#include "llhudmanager.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include <unordered_map>
#include <iomanip>
#include <unordered_set>
#include "vkgfx/llrendervkbackend.h"
#include "lltoolmgr.h"
#include "llassetstorage.h"
#include "llpolymesh.h"
#include "llproxy.h"
#include "llaudioengine.h"
#include "llsoundcache.h"
#include "llgputexcache.h"
#include "slcachemetrics.h"        // #715 cache metrics (periodic log, session summary)
#include "slcachestore.h"          // #715 segment store
#include "slgputexmigrate.h"       // #714 .gtx -> store migration
#include "slj2cmigrate.h"          // #714 legacy J2C cache -> store migration
#include "sltexturetiers.h"        // #705 texture tiers in the store
#include "slassetcachestore.h"      // #715 assets + cluster LOD on the store, their migrations
#include "llfilesystemstore.h"      // #715 LLFileSystem's store backend
#include "llcachemetrics.h"
#include "llcleanclusterbuild.h"    // 1a (#744)
#include "llcleanclustercache.h"    // 1a (#744)
#include "llstreamingaudio.h"
#include "llviewermenu.h"
#include "llselectmgr.h"
#include "gmdxselectionoutline.h"   // <GMDX>
#include "gmdxregioncapture.h"      // <GMDX> region map capture
#include "llpanelobjectlinks.h"
#include "lltrans.h"
#include "lltransutil.h"
#include "lltracker.h"
#include "llviewerparcelmgr.h"
#include "llworldmapview.h"

#include "lldebugview.h"
#include "llconsole.h"
#include "llcontainerview.h"
#include "lltoolfocus.h"
#include "lltooltip.h"

#include "llsdutil.h"
#include "llsdserialize.h"

#include "llworld.h"
#include "llhudeffecttrail.h"
#include "llslurl.h"
#include "llurlregistry.h"
#include "llwatchdog.h"

// Included so that constants/settings might be initialized
// in save_settings_to_globals()
#include "llbutton.h"
#include "llstatusbar.h"
#include "llsurface.h"
#include "llvosky.h"
#include "llvotree.h"
#include "llvoavatar.h"
#include "llfolderview.h"
#include "llagentpilot.h"
#include "llvovolume.h"
#include "llflexibleobject.h"
#include "llvosurfacepatch.h"
#include "llviewerfloaterreg.h"
#include "llcommandlineparser.h"
#include "llfloatermemleak.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"   // gFloaterTools (build-mode no-post, #28)
#include "llfloatersimplesnapshot.h"
#include "llfloatersnapshot.h"
#include "llsidepanelinventory.h"

// includes for idle() idleShutdown()
#include "llviewercontrol.h"
#include "lleventnotifier.h"
#include "llcallbacklist.h"
#include "lldeferredsounds.h"
#include "pipeline.h"
#include <glm/gtc/matrix_transform.hpp>   // VK projector light matrices
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>   // VK box-probe matrices
#include "llgesturemgr.h"
#include "llsky.h"
#include "llvlcomposition.h"
#include "llvlmanager.h"
#include "llviewercamera.h"
#include "llmaniptranslate.h"
#include "fsmaniptranslatejoint.h"
#include "llwearablelist.h"
#include "llvieweraudio.h"
#include "llimview.h"
#include "llviewerthrottle.h"
#include "llparcel.h"
#include "llavatariconctrl.h"
#include "llavatariconloader.h"
#include "llgroupiconctrl.h"
#include "llviewerassetstats.h"
#include "gltfscenemanager.h"

#include "workqueue.h"
#if LL_LINUX
#include <malloc.h>   // malloc_trim (RAM: hand freed texture copies back to the OS)
#endif
using namespace LL;

// Include for security api initialization
#include "llsecapi.h"
#include "llmachineid.h"
#include "llcleanup.h"

#include "llcoproceduremanager.h"
#include "llviewereventrecorder.h"


#include "slstreaminfo.h"
#include "sltpscamera.h"
#include "slphotostudiosettings.h"
#include "lltoolfocus.h"

// *FIX: These extern globals should be cleaned up.
// The globals either represent state/config/resource-storage of either
// this app, or another 'component' of the viewer. App globals should be
// moved into the app class, where as the other globals should be
// moved out of here.
// If a global symbol reference seems valid, it will be included
// via header files above.

//----------------------------------------------------------------------------
// llviewernetwork.h
#include "llviewernetwork.h"
// define a self-registering event API object
#include "llappviewerlistener.h"
#if SL_TESTCTL
#include "sltestctl.h"
#endif


static LLAppViewerListener sAppViewerListener(LLAppViewer::instance);

////// Windows-specific includes to the bottom - nasty defines in these pollute the preprocessor
//
//----------------------------------------------------------------------------
// viewer.cpp - these are only used in viewer, should be easily moved.

#if LL_DARWIN
extern void init_apple_menu(const char* product);
#endif // LL_DARWIN

extern bool gRandomizeFramerate;
extern bool gPeriodicSlowFrame;
extern bool gDebugGL;

// EXPERIMENTAL Vulkan integration (see llwindowsdl.cpp). Must be set before the
// window is created so LLWindowSDL creates an SDL_WINDOW_VULKAN window.

#if LL_DARWIN
extern bool gHiDPISupport;
#endif

////////////////////////////////////////////////////////////
// All from the last globals push...

F32 gSimLastTime; // Used in LLAppViewer::init and send_viewer_stats()
F32 gSimFrames;

bool gShowObjectUpdates = false;
bool gUseQuickTime = true;

eLastExecEvent gLastExecEvent = LAST_EXEC_NORMAL;
S32 gLastExecDuration = -1; // (<0 indicates unknown)
bool gLastExecDeviceLost = false;   // #790 (processMarkerFiles)
LLUUID gLastAgentSessionId;

#if LL_WINDOWS
#   define LL_PLATFORM_KEY "win"
#elif LL_DARWIN
#   define LL_PLATFORM_KEY "mac"
#elif LL_LINUX
#   define LL_PLATFORM_KEY "lnx"
#else
#   error "Unknown Platform"
#endif
const char* gPlatform = LL_PLATFORM_KEY;

LLSD gDebugInfo;

U32 gFrameCount = 0;
U32 gForegroundFrameCount = 0; // number of frames that app window was in foreground
LLPumpIO* gServicePump = NULL;

U64MicrosecondsImplicit gFrameTime = 0;
F32SecondsImplicit gFrameTimeSeconds = 0.f;
F32SecondsImplicit gFrameIntervalSeconds = 0.f;
F32 gFPSClamped = 10.f;                     // Pretend we start at target rate.
F32 gFrameDTClamped = 0.f;                  // Time between adjacent checks to network for packets
U64MicrosecondsImplicit gStartTime = 0; // gStartTime is "private", used only to calculate gFrameTimeSeconds

LLTimer gRenderStartTime;
LLFrameTimer gForegroundTime;
LLFrameTimer gLoggedInTime;
LLTimer gLogoutTimer;
static const F32 LOGOUT_REQUEST_TIME = 6.f;  // this will be cut short by the LogoutReply msg.
F32 gLogoutMaxTime = LOGOUT_REQUEST_TIME;


S32 gPendingMetricsUploads = 0;


bool gDisconnected = false;

// Used to restore texture state after a mode switch
LLFrameTimer gRestoreGLTimer;
bool gRestoreGL = false;
bool gUseWireframe = false;

U64Bytes gMemoryAllocated(0); // updated in display_stats() in llviewerdisplay.cpp

std::string gLastVersionChannel;

LLVector3 gWindVec(3.0, 3.0, 0.0);
LLVector3 gRelativeWindVec(0.0, 0.0, 0.0);

bool gRandomizeFramerate = false;
bool gPeriodicSlowFrame = false;

bool gCrashOnStartup = false;
bool gLLErrorActivated = false;
bool gLogoutInProgress = false;

bool gSimulateMemLeak = false;
bool gDoDisconnect = false;

// We don't want anyone, especially threads working on the graphics pipeline,
// to have to block due to this WorkQueue being full.
WorkQueue gMainloopWork("mainloop", 1024*1024);

////////////////////////////////////////////////////////////
// Internal globals... that should be removed.
static std::string gArgs;
const int MAX_MARKER_LENGTH = 1024;
const std::string MARKER_FILE_NAME("Slipstream.exec_marker");
const std::string START_MARKER_FILE_NAME("Slipstream.start_marker");
const std::string ERROR_MARKER_FILE_NAME("Slipstream.error_marker");
const std::string LOGOUT_MARKER_FILE_NAME("Slipstream.logout_marker");
static std::string gLaunchFileOnQuit;

// Used on Win32 for other apps to identify our window (eg, win_setup)
const char* const VIEWER_WINDOW_CLASSNAME = "Slipstream Viewer";

//----------------------------------------------------------------------------

// List of entries from strings.xml to always replace
static std::set<std::string> default_trans_args;
void init_default_trans_args()
{
    default_trans_args.insert("SECOND_LIFE"); // World
    default_trans_args.insert("APP_NAME");
    default_trans_args.insert("CAPITALIZED_APP_NAME");
    default_trans_args.insert("SECOND_LIFE_GRID");
    default_trans_args.insert("SUPPORT_SITE");
    // This URL shows up in a surprising number of places in various skin
    // files. We really only want to have to maintain a single copy of it.
    default_trans_args.insert("create_account_url");
}

struct SettingsFile : public LLInitParam::Block<SettingsFile>
{
    Mandatory<std::string>  name;
    Optional<std::string>   file_name;
    Optional<bool>          required,
                            persistent;
    Optional<std::string>   file_name_setting;

    SettingsFile()
    :   name("name"),
        file_name("file_name"),
        required("required", false),
        persistent("persistent", true),
        file_name_setting("file_name_setting")
    {}
};

struct SettingsGroup : public LLInitParam::Block<SettingsGroup>
{
    Mandatory<std::string>  name;
    Mandatory<S32>          path_index;
    Multiple<SettingsFile>  files;

    SettingsGroup()
    :   name("name"),
        path_index("path_index"),
        files("file")
    {}
};

struct SettingsFiles : public LLInitParam::Block<SettingsFiles>
{
    Multiple<SettingsGroup> groups;

    SettingsFiles()
    : groups("group")
    {}
};

static std::string gWindowTitle;
// #714: converts the old .gtx files into the cache store in the background (started by
// initCache, stopped at shutdown before the store closes).
static SLGpuTexMigrate::Runner sGpuTexMigration;
// #714: converts the legacy J2C texture cache (texture.entries / texture.cache / bodies)
// into the cache store in the background, serving unconverted textures by read-through.
static SLJ2CMigrate::Runner sJ2CMigration;

//----------------------------------------------------------------------------
// Metrics logging control constants
//----------------------------------------------------------------------------
static const F32 METRICS_INTERVAL_DEFAULT = 600.0;
static const F32 METRICS_INTERVAL_QA = 30.0;
static F32 app_metrics_interval = METRICS_INTERVAL_DEFAULT;
static bool app_metrics_qa_mode = false;

void idle_afk_check()
{
    // Don't check AFK status during startup states
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return;
    }

    // check idle timers
    F32 current_idle = gAwayTriggerTimer.getElapsedTimeF32();
// [RLVa:KB] - Checked: 2010-05-03 (RLVa-1.2.0g) | Modified: RLVa-1.2.0g
    // Enforce an idle time of 30 minutes if @allowidle=n restricted
    static LLCachedControl<S32> afk_timeout_cc(gSavedSettings, "AFKTimeout", 300);
    F32 afk_timeout = (!gRlvHandler.hasBehaviour(RLV_BHVR_ALLOWIDLE)) ? afk_timeout_cc() : 60.f * 30.f;
// [/RLVa:KB]
//  static LLCachedControl<S32> afk_timeout(gSavedSettings, "AFKTimeout", 300);
    if (afk_timeout && (current_idle > afk_timeout))
    {
        if (!gAgent.getAFK())
        {
            LL_INFOS("IdleAway") << "Idle more than " << afk_timeout << " seconds: automatically changing to Away status" << LL_ENDL;
            gAgent.setAFK();
        }
        else
        {
            // Refresh timer so that random one click or hover won't clear the status.
            // But expanding the window still should lift afk status
            gAwayTimer.reset();
        }
    }
}

// A callback set in LLAppViewer::init()
static void ui_audio_callback(const LLUUID& uuid)
{
    if (gAudiop)
    {
        SoundData soundData(uuid, gAgent.getID(), 1.0f, LLAudioEngine::AUDIO_TYPE_UI);
        gAudiop->triggerSound(soundData);
    }
}

// A callback set in LLAppViewer::init()
static void deferred_ui_audio_callback(const LLUUID& uuid)
{
    if (gAudiop)
    {
        SoundData soundData(uuid, gAgent.getID(), 1.0f, LLAudioEngine::AUDIO_TYPE_UI);
        LLDeferredSounds::instance().deferSound(soundData);
    }
}

bool    create_text_segment_icon_from_url_match(LLUrlMatch* match,LLTextBase* base)
{
    if (!match || match->getSkipProfileIcon() || !base || base->getPlainText())
        return false;

    LLUUID match_id = match->getID();

    LLIconCtrl* icon;

    if( match->getMenuName() == "menu_url_group.xml" // See LLUrlEntryGroup constructor
        || gAgent.isInGroup(match_id, true)) //This check seems unfiting, urls are either /agent or /group
    {
        LLGroupIconCtrl::Params icon_params;
        icon_params.group_id = match_id;
        icon_params.rect = LLRect(0, 16, 16, 0);
        icon_params.visible = true;
        icon = LLUICtrlFactory::instance().create<LLGroupIconCtrl>(icon_params);
    }
    else
    {
        LLAvatarIconCtrl::Params icon_params;
        icon_params.avatar_id = match_id;
        icon_params.rect = LLRect(0, 16, 16, 0);
        icon_params.visible = true;
        icon = LLUICtrlFactory::instance().create<LLAvatarIconCtrl>(icon_params);
    }

    LLInlineViewSegment::Params params;
    params.force_newline = false;
    params.view = icon;
    params.left_pad = 4;
    params.right_pad = 4;
    params.top_pad = -2;
    params.bottom_pad = 2;

    base->appendWidget(params," ",false);

    return true;
}

// Use these strictly for things that are constructed at startup,
// or for things that are performance critical.  JC
static void settings_to_globals()
{
    LLSurface::setTextureSize(gSavedSettings.getU32("RegionTextureSize"));

    LLVOVolume::sLODFactor              = llclamp(gSavedSettings.getF32("RenderVolumeLODFactor"), 0.01f, MAX_LOD_FACTOR);
    LLVOVolume::sDistanceFactor         = 1.f-LLVOVolume::sLODFactor * 0.1f;
    LLVolumeImplFlexible::sUpdateFactor = gSavedSettings.getF32("RenderFlexTimeFactor");
    LLVOTree::sTreeFactor               = gSavedSettings.getF32("RenderTreeLODFactor");
    LLVOAvatar::sLODFactor              = llclamp(gSavedSettings.getF32("RenderAvatarLODFactor"), 0.f, MAX_AVATAR_LOD_FACTOR);
    LLVOAvatar::sPhysicsLODFactor       = llclamp(gSavedSettings.getF32("RenderAvatarPhysicsLODFactor"), 0.f, MAX_AVATAR_LOD_FACTOR);
    LLVOAvatar::updateImpostorRendering(gSavedSettings.getU32("RenderAvatarMaxNonImpostors"));
    LLVOAvatar::sVisibleInFirstPerson   = gSavedSettings.getBOOL("FirstPersonAvatarVisible");
    // clamp auto-open time to some minimum usable value
    LLFolderView::sAutoOpenTime         = llmax(0.25f, gSavedSettings.getF32("FolderAutoOpenDelay"));
    LLSelectMgr::sRectSelectInclusive   = gSavedSettings.getBOOL("RectangleSelectInclusive");
    LLSelectMgr::sRenderHiddenSelections = gSavedSettings.getBOOL("RenderHiddenSelections");
    LLSelectMgr::sRenderLightRadius = gSavedSettings.getBOOL("RenderLightRadius");

    gAgentPilot.setNumRuns(gSavedSettings.getS32("StatsNumRuns"));
    gAgentPilot.setQuitAfterRuns(gSavedSettings.getBOOL("StatsQuitAfterRuns"));
    gAgent.setHideGroupTitle(gSavedSettings.getBOOL("RenderHideGroupTitle"));

    gDebugWindowProc = gSavedSettings.getBOOL("DebugWindowProc");
    gShowObjectUpdates = gSavedSettings.getBOOL("ShowObjectUpdates");
    LLWorldMapView::setScaleSetting(gSavedSettings.getF32("MapScale"));

#if LL_DARWIN
    gHiDPISupport = gSavedSettings.getBOOL("RenderHiDPI");
#endif
}

static void settings_modify()
{
    LLPipeline::sRenderTransparentWater = gSavedSettings.getBOOL("RenderTransparentWater");
    LLPipeline::sRenderDeferred = true; // false is deprecated
    LLVOSurfacePatch::sLODFactor        = gSavedSettings.getF32("RenderTerrainLODFactor");
    LLVOSurfacePatch::sLODFactor *= LLVOSurfacePatch::sLODFactor; //square lod factor to get exponential range of [1,16]
    gDebugGL       = gDebugGLSession || gDebugSession;
}

class LLFastTimerLogThread : public LLThread
{
public:
    std::string mFile;

    LLFastTimerLogThread(std::string& test_name) : LLThread("fast timer log")
    {
        std::string file_name = test_name + std::string(".slp");
        mFile = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, file_name);
    }

    void run()
    {
        llofstream os(mFile.c_str());

        while (!LLAppViewer::instance()->isQuitting())
        {
            LLTrace::BlockTimer::writeLog(os);
            os.flush();
            ms_sleep(32);
        }

        os.close();
    }
};

//virtual
bool LLAppViewer::initSLURLHandler()
{
    // does nothing unless subclassed
    return false;
}

//virtual
bool LLAppViewer::sendURLToOtherInstance(const std::string& url)
{
    // does nothing unless subclassed
    return false;
}

//----------------------------------------------------------------------------
// LLAppViewer definition

// Static members.
// The single viewer app.
LLAppViewer* LLAppViewer::sInstance = NULL;
LLTextureCache* LLAppViewer::sTextureCache = NULL;
LLImageDecodeThread* LLAppViewer::sImageDecodeThread = NULL;
LLTextureFetch* LLAppViewer::sTextureFetch = NULL;
LLPurgeDiskCacheThread* LLAppViewer::sPurgeDiskCacheThread = NULL;

std::string getRuntime()
{
    return llformat("%.4f", (F32)LLTimer::getElapsedSeconds().value());
}

// RenderVKUpscaleMode (0 off, 1-5 TAA upscaler presets / custom, 6-11 FSR 3.1) became
// RenderVKUpscaler (0 off, 1-6 FSR 3.1). The TAA presets move to FSR at the same scale.
// Runs after the settings files load, and again whenever something (an old graphics
// preset, the debug settings) sets the old key; it is reset to 0 so it never re-migrates.
static void migrateVKUpscaleMode()
{
    LLControlVariable* old_ctrl = gSavedSettings.getControl("RenderVKUpscaleMode");
    if (!old_ctrl || !gSavedSettings.controlExists("RenderVKUpscaler")) return;
    const U32 old_mode = gSavedSettings.getU32("RenderVKUpscaleMode");
    if (old_mode == 0) return;
    const U32 new_mode = old_mode <= 5 ? old_mode + 1 : (old_mode <= 11 ? old_mode - 5 : 0);
    gSavedSettings.setU32("RenderVKUpscaler", new_mode);
    gSavedSettings.setU32("RenderVKUpscaleMode", 0);
    LL_INFOS("Settings") << "Migrated RenderVKUpscaleMode " << old_mode << " to RenderVKUpscaler " << new_mode
                         << " (FSR 3.1)" << LL_ENDL;
}

// #962: RenderVKUpscaler 7-11 were DLSS Ray Reconstruction modes; they are now DLSS at that
// quality, the network chosen by RenderVKDlssRayReconstruction (default Auto). Settings from
// before keep Ray Reconstruction: RR On, once. RenderVKDlssSettingsVersion marks it done (the RR
// setting alone can't: a user who later picks Auto -- the default, not saved -- would be
// migrated again on every launch).
static void migrateDlssRayReconstruction()
{
    if (!gSavedSettings.controlExists("RenderVKDlssSettingsVersion")
        || !gSavedSettings.controlExists("RenderVKDlssRayReconstruction")
        || !gSavedSettings.controlExists("RenderVKUpscaler"))
        return;
    if (gSavedSettings.getU32("RenderVKDlssSettingsVersion") >= 1) return;
    const U32 mode = gSavedSettings.getU32("RenderVKUpscaler");
    if (mode >= 7 && mode <= 11)
    {
        gSavedSettings.setU32("RenderVKDlssRayReconstruction", 1);
        LL_INFOS("Settings") << "Migrated RenderVKUpscaler " << mode
                             << " (DLSS Ray Reconstruction before #962) to DLSS at the same quality with "
                                "RenderVKDlssRayReconstruction 1 (On)" << LL_ENDL;
    }
    gSavedSettings.setU32("RenderVKDlssSettingsVersion", 1);
}

// Slipstream was forked from Alchemy and used Alchemy's per-user data directory
// ("AlchemyViewer": ~/.alchemyviewer, %APPDATA%\AlchemyViewer, ~/Library/Application
// Support/AlchemyViewer). It now has its own ("SlipstreamViewer"). On the first run
// with no Slipstream directory yet, copy the old one over -- settings, accounts, chat
// logs, presets -- minus the viewer logs. It is COPIED, not moved: an upstream Alchemy
// install may still be using it. Runs before the log system exists (stderr only).
static void migrateAlchemyUserDir()
{
    namespace fs = std::filesystem;
    const std::string base = gDirUtilp->getOSUserDir();
    if (base.empty()) return;
#if LL_LINUX
    if (getenv("SLIPSTREAMVIEWER_USER_DIR")) return;   // user-chosen location: leave it alone
    const fs::path old_dir = fs::u8path(base + "/.alchemyviewer");
    const fs::path new_dir = fs::u8path(base + "/.slipstreamviewer");
#else
    const fs::path old_dir = fs::u8path(base) / "AlchemyViewer";
    const fs::path new_dir = fs::u8path(base) / "SlipstreamViewer";
#endif
    auto utf8 = [](const fs::path& p) { const auto u = p.u8string(); return std::string(u.begin(), u.end()); };
    std::error_code ec;
    if (fs::exists(new_dir, ec) || !fs::is_directory(old_dir, ec)) return;
    fs::create_directories(new_dir, ec);
    for (const fs::directory_entry& e : fs::directory_iterator(old_dir, ec))
    {
        const std::string name = utf8(e.path().filename());
        if (name == "logs" || name == "cache" || name == "wine") continue;   // logs, caches, old WINE prefix
        std::error_code cec;
        fs::copy(e.path(), new_dir / e.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::skip_existing, cec);
        if (cec)
            fprintf(stderr, "Slipstream: could not copy %s from the Alchemy data directory: %s\n",
                    name.c_str(), cec.message().c_str());
    }
    fprintf(stderr, "Slipstream: copied settings from %s to %s\n", utf8(old_dir).c_str(), utf8(new_dir).c_str());
}

LLAppViewer::LLAppViewer()
:   mMarkerFile(),
    mLogoutMarkerFile(),
    mReportedCrash(false),
    mNumSessions(0),
    mGeneralThreadPool(nullptr),
    mPurgeCache(false),
    mPurgeCacheOnExit(false),
    mPurgeUserDataOnExit(false),
    mSecondInstance(false),
    mInstanceSlot(1),
    mUpdaterNotFound(false),
    mSavedFinalSnapshot(false),
    mSavePerAccountSettings(false),     // don't save settings on logout unless login succeeded.
    mQuitRequested(false),
    mClosingFloaters(false),
    mLogoutRequestSent(false),
    mMainloopTimeout(NULL),
    mAgentRegionLastAlive(false),
    mRandomizeFramerate(LLCachedControl<bool>(gSavedSettings,"Randomize Framerate", false)),
    mPeriodicSlowFrame(LLCachedControl<bool>(gSavedSettings,"Periodic Slow Frame", false)),
    mFastTimerLogThread(NULL),
    mSettingsLocationList(NULL),
    mIsFirstRun(false)
{
    if(NULL != sInstance)
    {
        LL_ERRS() << "Oh no! An instance of LLAppViewer already exists! LLAppViewer is sort of like a singleton." << LL_ENDL;
    }

    mDumpPath ="";

    // Need to do this initialization before we do anything else, since anything
    // that touches files should really go through the lldir API
    // <GMDX> no copy of an Alchemy profile: GunMetal-DX starts clean in its own directory
    // migrateAlchemyUserDir();
    gDirUtilp->initAppDirs("GunMetalDX");   // <GMDX> own profile: ~/.gunmetaldx, GUNMETALDX_USER_DIR
    //
    // IMPORTANT! Do NOT put anything that will write
    // into the log files during normal startup until AFTER
    // we run the "program crashed last time" error handler below.
    //
    sInstance = this;

    gLoggedInTime.stop();

    processMarkerFiles();
    //
    // OK to write stuff to logs now, we've now crash reported if necessary
    //

    LLLoginInstance::instance().setPlatformInfo(gPlatform, LLOSInfo::instance().getOSVersionString(), LLOSInfo::instance().getOSStringSimple());

    // Under some circumstances we want to read the static_debug_info.log file
    // from the previous viewer run between this constructor call and the
    // init() call, which will overwrite the static_debug_info.log file for
    // THIS run. So setDebugFileNames() early.
    // write minidump files to a per-run dump directory to avoid multiple viewer issues.
    std::string logdir = gDirUtilp->getExpandedFilename(LL_PATH_DUMP, "");
    mDumpPath = logdir;

    setDebugFileNames(logdir);
}

LLAppViewer::~LLAppViewer()
{
    delete mSettingsLocationList;

    destroyMainloopTimeout();

    // If we got to this destructor somehow, the app didn't hang.
    removeMarkerFiles();
}

class LLUITranslationBridge : public LLTranslationBridge
{
public:
    virtual std::string getString(const std::string &xml_desc)
    {
        return LLTrans::getString(xml_desc);
    }
};


// #526: this computer's graphics can't run Slipstream (no Vulkan driver, no device with
// Vulkan 1.3 + the required features, or the device bring-up failed). Say why in a native
// message box -- no viewer window is visible: the pre-flight runs before the window exists
// and the SDL window stays hidden until the Vulkan bring-up succeeds -- then leave WITHOUT
// the crash path: no LL_ERRS / abort (no dump, no BugSplat, no llerror / error marker), and
// the exec marker removed so the next start does not report a crash or freeze. _exit, as
// fastQuit() does: the half-initialized viewer (worker threads, the watchdog) is never
// torn down, so nothing can fault on the way out. Exit code 0: an expected outcome the
// user has just been told about (the Linux wrapper prints "Bad shutdown" otherwise).
static void exitGraphicsUnsupported(const std::string& text)
{
    LLError::setAlwaysFlush(true);
    OSMessageBox(text, vkRequirementsUserTitle(), OSMB_OK);   // also logs the text (LL_WARNS)
    if (LLAppViewer::instance())
    {
        LLAppViewer::instance()->removeMarkerFiles();
    }
    LL_WARNS("Vulkan") << "Exiting: this computer's graphics do not meet Slipstream's requirements "
                       << "(clean exit, not a crash)" << LL_ENDL;
    _exit(0);
}

// #526 pre-flight, before the viewer window exists: a throwaway Vulkan instance checks
// every GPU against the requirements (vkgfx/vk_requirements.h) and logs one
// "Vulkan requirements:" line per device. Returns when a device qualifies (the device
// context then picks the best qualifying one); otherwise exits via the message box.
static void checkVulkanRequirementsOrExit()
{
    // #277: the graphics card chosen in Preferences ("auto" = ranking); honoured here and by
    // the device context's own pick (vk_device.cpp pickPhysicalDevice uses the same choice).
    vkSetPreferredDevice(gSavedSettings.getString("RenderVKDeviceSelect"));
    gVkForceApi12 = gSavedSettings.getBOOL("RenderVKForceApi12");   // #675: test the 1.2 + extensions fallback
    const VkRequirementsReport r = vkCheckRequirements();
    for (const std::string& l : vkRequirementsLogLines(r))
    {
        if (r.ok())
            LL_INFOS("Vulkan") << l << LL_ENDL;
        else
            LL_WARNS("Vulkan") << l << LL_ENDL;
    }
    if (!r.ok())
    {
        exitGraphicsUnsupported(vkRequirementsUserMessage(r));
    }

    // #950: a device qualified, but is it the computer's own GPU driver? Classify (and log, always),
    // and warn in plain language when it is a translation layer / software renderer, an integrated
    // GPU while a discrete one exists, a discrete GPU with no (or too old a) Vulkan driver.
    // Kill switch: SlipstreamDriverCheck = FALSE (the classification is still logged).
    const std::string pref = gSavedSettings.getString("RenderVKDeviceSelect");
    const VkDriverCheck dc = vkClassifyDriverSetup(r, vkEnumerateOsAdapters(), !pref.empty() && pref != "auto");
    if (dc.issue == VkSetupIssue::None)
    {
        LL_INFOS("Vulkan") << dc.logText << LL_ENDL;
        return;
    }
    LL_WARNS("Vulkan") << dc.logText << LL_ENDL;
    if (!gSavedSettings.getBOOL("SlipstreamDriverCheck"))
    {
        LL_WARNS("Vulkan") << "Vulkan driver check: dialog disabled (SlipstreamDriverCheck = FALSE)" << LL_ENDL;
        return;
    }
    if (gSavedSettings.getString("SlipstreamDriverCheckAck") == dc.key)
    {
        LL_WARNS("Vulkan") << "Vulkan driver check: dialog dismissed earlier for this graphics setup (SlipstreamDriverCheckAck)" << LL_ENDL;
        return;
    }
    LLError::setAlwaysFlush(true);
    const S32 choice = OSMessageBoxChoices(dc.userText, vkDriverCheckTitle(),
        {"Continue anyway", "Continue, and don't show this again for this graphics setup", "Quit"});
    if (choice == 0)
    {
        LL_WARNS("Vulkan") << "Vulkan driver check: user chose Continue anyway" << LL_ENDL;
    }
    else if (choice == 1)
    {
        gSavedSettings.setString("SlipstreamDriverCheckAck", dc.key);
        LL_WARNS("Vulkan") << "Vulkan driver check: user chose Continue and don't show again for this graphics setup" << LL_ENDL;
    }
    else
    {
        LL_WARNS("Vulkan") << "Vulkan driver check: user chose Quit (clean exit, not a crash)" << LL_ENDL;
        if (LLAppViewer::instance())
        {
            LLAppViewer::instance()->removeMarkerFiles();
        }
        _exit(0);
    }
}

bool LLAppViewer::init()
{
    setupErrorHandling(mSecondInstance);

    //
    // Start of the application
    //

    // initialize the LLSettingsType translation bridge.
    LLTranslationBridge::ptr_t trans = std::make_shared<LLUITranslationBridge>();
    LLSettingsType::initParamSingleton(trans);

    // initialize SSE options
    LLVector4a::initClass();

    //initialize particle index pool
    LLVOPartGroup::initClass();

    // set skin search path to default, will be overridden later
    // this allows simple skinned file lookups to work
    gDirUtilp->setSkinFolder("default", "en");

//  initLoggingAndGetLastDuration();

    //
    // OK to write stuff to logs now, we've now crash reported if necessary
    //
    init_default_trans_args();

    // inits from settings.xml and from strings.xml
    if (!initConfiguration())
    {
        LL_WARNS("InitInfo") << "initConfiguration() failed." << LL_ENDL;
        // quit immediately
        return false;
    }

    LL_INFOS("InitInfo") << "Configuration initialized." << LL_ENDL ;
    // <GMDX> Crash reports from here on: the profile and its log exist now, and LLApp's own signal
    // handlers (Linux) / LLWinDebug's exception filter (Windows) are installed, so Crashpad goes on
    // top: it writes the dump, then Linux hands the signal back to LLApp's handler (Windows: Crashpad
    // ends the process; it replaces LLWinDebug's Slipstream.dmp).
    GMDXCrashReport::start();

    // Log volume: telemetry tags quiet unless SlipstreamVerboseLogging (live).
    {
        extern void slipstreamApplyLogVerbosity();
        slipstreamApplyLogVerbosity();
        if (LLControlVariable* c = gSavedSettings.getControl("SlipstreamVerboseLogging"))
            c->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { slipstreamApplyLogVerbosity(); });
    }

    // Per-thread CPU placement (multi-L3 CPUs): the main thread -- and the GPU
    // driver threads it creates later -- stay on the first L3 domain; every worker
    // (LL::ThreadPool, LLThread, the HTTP service) goes to the other domains.
    // Before the first worker thread starts (HTTP service, just below).
    LLThreadAffinity::init(gSavedSettings.getBOOL("SlipstreamThreadAffinity"));
    LL_INFOS("InitInfo") << "Thread affinity: " << LLThreadAffinity::describe() << LL_ENDL;

    //set the max heap size.
    initMaxHeapSize() ;
    LLCoros::instance().setStackSize(gSavedSettings.getS32("CoroutineStackSize"));

    // Although initLoggingAndGetLastDuration() is the right place to mess with
    // setFatalFunction(), we can't query gSavedSettings until after
    // initConfiguration().
    S32 rc(gSavedSettings.getS32("QAModeTermCode"));
    if (rc >= 0)
    {
        // QAModeTermCode set, terminate with that rc on LL_ERRS. Use
        // _exit() rather than exit() because normal cleanup depends too
        // much on successful startup!
        LLError::setFatalFunction([rc](const std::string&){ _exit(rc); });
    }

    // #506: plugin hosts (SLPlugin) write their stdout/stderr to a file in the logs
    // folder; it is copied into this log if a plugin dies (e.g. CEF failing to start).
    LLPluginProcessParent::setOutputLogDir(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ""));

    // Initialize the non-LLCurl libcurl library.  Should be called
    // before consumers (LLTextureFetch).
    mAppCoreHttp.init();

    LL_INFOS("InitInfo") << "LLCore::Http initialized." << LL_ENDL ;

    LLMachineID::init();

    if (gSavedSettings.getBOOL("QAModeMetrics"))
    {
        app_metrics_qa_mode = true;
        app_metrics_interval = METRICS_INTERVAL_QA;
    }
    LLViewerAssetStatsFF::init();

    initThreads();
    LL_INFOS("InitInfo") << "Threads initialized." << LL_ENDL ;
#if SL_TESTCTL
    SLTestCtl::start();
#endif

    // Initialize settings early so that the defaults for ignorable dialogs are
    // picked up and then correctly re-saved after launching the updater (STORM-1268).
    LLUI::settings_map_t settings_map;
    settings_map["config"] = &gSavedSettings;
    settings_map["ignores"] = &gWarningSettings;
    settings_map["floater"] = &gSavedSettings; // *TODO: New settings file
    settings_map["account"] = &gSavedPerAccountSettings;

    LLUI::createInstance(settings_map,
        LLUIImageList::getInstance(),
        ui_audio_callback,
        deferred_ui_audio_callback);

    if (gSavedSettings.getBOOL("SpellCheck"))
    {
        std::list<std::string> dict_list;
        std::string dict_setting = gSavedSettings.getString("SpellCheckDictionary");
        boost::split(dict_list, dict_setting, boost::is_any_of(std::string(",")));
        if (!dict_list.empty())
        {
            LLSpellChecker::setUseSpellCheck(dict_list.front());
            dict_list.pop_front();
            LLSpellChecker::instance().setSecondaryDictionaries(dict_list);
        }
    }

    LL_INFOS("InitInfo") << "UI initialized." << LL_ENDL ;

    // NOW LLUI::getLanguage() should work. gDirUtilp must know the language
    // for this session ASAP so all the file-loading commands that follow,
    // that use findSkinnedFilenames(), will include the localized files.
    gDirUtilp->setSkinFolder(gDirUtilp->getSkinFolder(), LLUI::getLanguage());

    // Setup LLTrans after LLUI::initClass has been called.
    initStrings();

    // initialize LLWearableType translation bridge.
    // Will immediately use LLTranslationBridge to init LLWearableDictionary
    LLWearableType::initParamSingleton(trans);

    // Setup notifications after LLUI::initClass() has been called.
    LLNotifications::instance();
    LL_INFOS("InitInfo") << "Notifications initialized." << LL_ENDL ;

    //////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////
    // *FIX: The following code isn't grouped into functions yet.

    //
    // Various introspection concerning the libs we're using - particularly
    // the libs involved in getting to a full login screen.
    //
    LL_INFOS("InitInfo") << "J2C Engine is: " << LLImageJ2C::getEngineInfo() << LL_ENDL;
    LL_INFOS("InitInfo") << "libcurl version is: " << LLCore::LLHttp::getCURLVersion() << LL_ENDL;

    /////////////////////////////////////////////////
    // OS-specific login dialogs
    /////////////////////////////////////////////////

    //test_cached_control();

    // track number of times that app has run
    mNumSessions = gSavedSettings.getS32("NumSessions");
    mNumSessions++;
    gSavedSettings.setS32("NumSessions", mNumSessions);

    // LLKeyboard relies on LLUI to know what some accelerator keys are called.
    LLKeyboard::setStringTranslatorFunc( LLTrans::getKeyboardString );

    // Provide the text fields with callbacks for opening Urls
    LLUrlAction::setOpenURLCallback([](const std::string& url) { LLWeb::loadURL(url); });
    LLUrlAction::setOpenURLInternalCallback(boost::bind(&LLWeb::loadURLInternal, _1, LLStringUtil::null, LLStringUtil::null, false));
    LLUrlAction::setOpenURLExternalCallback([](const std::string& url) { LLWeb::loadURLExternal(url, true); });
    LLUrlAction::setExecuteSLURLCallback(&LLURLDispatcher::dispatchFromTextEditor);

    // Let code in llui access the viewer help floater
    LLUI::getInstance()->mHelpImpl = LLViewerHelp::getInstance();

    LL_INFOS("InitInfo") << "UI initialization is done." << LL_ENDL ;

    // Load translations for tooltips
    LLFloater::initClass();
    LLUrlFloaterDispatchHandler::registerInDispatcher();

    /////////////////////////////////////////////////

    LLToolMgr::getInstance(); // Initialize tool manager if not already instantiated

    LLViewerFloaterReg::registerFloaters();

    /////////////////////////////////////////////////
    //
    // Load settings files
    //
    //
    LLGroupMgr::parseRoleActions("role_actions.xml");

    LLAgent::parseTeleportMessages("teleport_strings.xml");

    // load MIME type -> media impl mappings
    std::string mime_types_name;
#if LL_DARWIN
    mime_types_name = "mime_types_mac.xml";
#elif LL_LINUX
    mime_types_name = "mime_types_linux.xml";
#else
    mime_types_name = "mime_types.xml";
#endif
    LLMIMETypes::parseMIMETypes( mime_types_name );

    // Copy settings to globals. *TODO: Remove or move to appropriage class initializers
    settings_to_globals();
    // Setup settings listeners
    settings_setup_listeners();
    // Modify settings based on system configuration and compile options
    settings_modify();

    // Find partition serial number (Windows) or hardware serial (Mac)
    mSerialNumber = generateSerialNumber();

    // do any necessary set-up for accepting incoming SLURLs from apps
    initSLURLHandler();

    if (!initHardwareTest())
    {
        // Early out from user choice.
        LL_WARNS("InitInfo") << "initHardwareTest() failed." << LL_ENDL;
        // quit immediately
        return false;
    }
    LL_INFOS("InitInfo") << "Hardware test initialization done." << LL_ENDL ;

    // Prepare for out-of-memory situations, during which we will crash on
    // purpose and save a dump.
#if LL_WINDOWS && LL_RELEASE_FOR_DOWNLOAD && LL_USE_SMARTHEAP
    MemSetErrorHandler(first_mem_error_handler);
#endif // LL_WINDOWS && LL_RELEASE_FOR_DOWNLOAD && LL_USE_SMARTHEAP

    // *Note: this is where gViewerStats used to be created.

    if (!initCache())
    {
        std::string msg = LLTrans::getString("MBUnableToAccessFile");
        OSMessageBox(msg.c_str(), LLStringUtil::null, OSMB_OK);
        LL_WARNS("InitInfo") << "Failed to init cache" << LL_ENDL;
        // quit immediately
        return false;
    }
    LL_INFOS("InitInfo") << "Cache initialization is done." << LL_ENDL ;

    // Initialize event recorder
    LLViewerEventRecorder::createInstance();

    //
    // Initialize the window
    //
    checkVulkanRequirementsOrExit();   // #526: before the window; exits cleanly when unsupported
    initWindow();
    LL_INFOS("InitInfo") << "Window is initialized." << LL_ENDL ;

    // writeSystemInfo can be called after window is initialized (gViewerWindow non-null)
    writeSystemInfo();

    // call all self-registered classes
    LLInitClassList::instance().fireCallbacks();

    LLFolderViewItem::initClass(); // SJB: Needs to happen after initWindow(), not sure why but related to fonts

    gGPUInfo.getInfo(gDebugInfo);
    gGPUInfo.printInfo();

#if defined(LL_X86) || defined(LL_X86_64)
    // Without SSE2 support we will crash almost immediately, warn here.
    if (!gSysCPU.hasSSE2())
    {
        // can't use an alert here since we're exiting and
        // all hell breaks lose.
        std::string msg = LLNotifications::instance().getGlobalString("UnsupportedCPUSSE2");
        OSMessageBox(msg.c_str(), LLStringUtil::null, OSMB_OK);
        LL_WARNS("InitInfo") << "SSE2 is not supported" << LL_ENDL;
        // quit immediately
        return false;
    }
#endif

    // alert the user if they are using unsupported hardware
    if (!gSavedSettings.getBOOL("AlertedUnsupportedHardware"))
    {
        bool unsupported = false;
        LLSD args;
        std::string minSpecs;

        // get cpu data from xml
        std::stringstream minCPUString(LLNotifications::instance().getGlobalString("UnsupportedCPUAmount"));
        S32 minCPU = 0;
        minCPUString >> minCPU;

        // get RAM data from XML
        std::stringstream minRAMString(LLNotifications::instance().getGlobalString("UnsupportedRAMAmount"));
        U64Bytes minRAM;
        minRAMString >> minRAM;

        if (!LLFeatureManager::getInstance()->isGPUSupported() && LLFeatureManager::getInstance()->getGPUClass() != GPU_CLASS_UNKNOWN)
        {
            minSpecs += LLNotifications::instance().getGlobalString("UnsupportedGPU");
            minSpecs += "\n";
            unsupported = true;
        }

        if (gSysCPU.getMHz() < minCPU)
        {
            minSpecs += LLNotifications::instance().getGlobalString("UnsupportedCPU");
            minSpecs += "\n";
            unsupported = true;
        }

        if (gSysMemory.getPhysicalMemoryKB() < minRAM)
        {
            minSpecs += LLNotifications::instance().getGlobalString("UnsupportedRAM");
            minSpecs += "\n";
            unsupported = true;
        }

        if (LLFeatureManager::getInstance()->getGPUClass() == GPU_CLASS_UNKNOWN)
        {
            LLNotificationsUtil::add("UnknownGPU");
        }

        if (unsupported)
        {
            if (!gSavedSettings.controlExists("WarnUnsupportedHardware")
                || gSavedSettings.getBOOL("WarnUnsupportedHardware"))
            {
                args["MINSPECS"] = minSpecs;
                LLNotificationsUtil::add("UnsupportedHardware", args );
            }
        }
    }


    // save the graphics card
    gDebugInfo["GraphicsCard"] = LLFeatureManager::getInstance()->getGPUString();

    // Save the current version to the prefs file
    gSavedSettings.setString("LastRunVersion",
                             LLVersionInfo::instance().getChannelAndVersion());

    gSimLastTime = gRenderStartTime.getElapsedTimeF32();
    gSimFrames = (F32)gFrameCount;

    if (gSavedSettings.getBOOL("JoystickEnabled"))
    {
        LLViewerJoystick::getInstance()->init(false);
    }

    // Certificate revocation data (static distrust list + newest Chrome CRLSet on disk)
    // before the certificate store validates anything; needs the cache directory.
    SLRevocationUpdater::init();
    LLGameControl::init(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "gamecontrollerdb.txt"),
        [](const std::vector<std::string>& keys) -> LLSD
        {
            LLSD result = LLSD::emptyMap();
            for (const std::string& key : keys)
            {
                if (gSavedSettings.controlExists(key))
                {
                    result[key] = gSavedSettings.getLLSD(key);
                }
            }
            return result;
        },
        [](const LLSD& key_values)
        {
            for (auto it = key_values.beginMap(); it != key_values.endMap(); ++it)
            {
                if (gSavedSettings.controlExists(it->first))
                {
                    gSavedSettings.setLLSD(it->first, it->second);
                }
            }
        },
        []() { LLPanelPreferenceGameControl::updateDeviceList(); });

    try
    {
        initializeSecHandler();
    }
    catch (LLProtectedDataException&)
    {
        LLNotificationsUtil::add("CorruptedProtectedDataStore");
    }


    // LL's updater (SLVersionChecker over LEAP) is not shipped; Slipstream checks its
    // own update feed in-viewer (SLUpdateChecker, #293).
    {
        mUpdaterNotFound = true;
        LL_WARNS("InitInfo") << "Skipping updater check." << LL_ENDL;
    }

    {
        // Iterate over --leap command-line options. But this is a bit tricky: if
        // there's only one, it won't be an array at all.
        LLSD LeapCommand(gSavedSettings.getLLSD("LeapCommand"));
        LL_DEBUGS("InitInfo") << "LeapCommand: " << LeapCommand << LL_ENDL;
        if (LeapCommand.isDefined() && !LeapCommand.isArray())
        {
            // If LeapCommand is actually a scalar value, make an array of it.
            // Have to do it in two steps because LeapCommand.append(LeapCommand)
            // trashes content! :-P
            LLSD item(LeapCommand);
            LeapCommand.append(item);
        }
        for (const auto& leap : llsd::inArray(LeapCommand))
        {
            LL_INFOS("InitInfo") << "processing --leap \"" << leap << '"' << LL_ENDL;
            // We don't have any better description of this plugin than the
            // user-specified command line. Passing "" causes LLLeap to derive a
            // description from the command line itself.
            // Suppress LLLeap::Error exception: trust LLLeap's own logging. We
            // don't consider any one --leap command mission-critical, so if one
            // fails, log it, shrug and carry on.
            LLLeap::create("", leap, false); // exception=false
        }
    }

    if (gSavedSettings.getBOOL("QAMode") && gSavedSettings.getS32("QAModeEventHostPort") > 0)
    {
        LL_WARNS("InitInfo") << "QAModeEventHostPort DEPRECATED: "
                             << "lleventhost no longer supported as a dynamic library"
                             << LL_ENDL;
    }

    LLTextUtil::TextHelpers::iconCallbackCreationFunction = create_text_segment_icon_from_url_match;

    //EXT-7013 - On windows for some locale (Japanese) standard
    //datetime formatting functions didn't support some parameters such as "weekday".
    //Names for days and months localized in xml are also useful for Polish locale(STORM-107).
    std::string language = gSavedSettings.getString("Language");
    if (language == "ja" || language == "pl")
    {
        LLStringOps::setupWeekDaysNames(LLTrans::getString("dateTimeWeekdaysNames"));
        LLStringOps::setupWeekDaysShortNames(LLTrans::getString("dateTimeWeekdaysShortNames"));
        LLStringOps::setupMonthNames(LLTrans::getString("dateTimeMonthNames"));
        LLStringOps::setupMonthShortNames(LLTrans::getString("dateTimeMonthShortNames"));
        LLStringOps::setupDayFormat(LLTrans::getString("dateTimeDayFormat"));

        LLStringOps::sAM = LLTrans::getString("dateTimeAM");
        LLStringOps::sPM = LLTrans::getString("dateTimePM");
    }

    LLAgentLanguage::init();

    /// Tell the Coprocedure manager how to discover and store the pool sizes
    // what I wanted
    LLCoprocedureManager::getInstance()->setPropertyMethods(
        boost::bind(&LLControlGroup::getU32, boost::ref(gSavedSettings), _1),
        boost::bind(&LLControlGroup::declareU32, boost::ref(gSavedSettings), _1, _2, _3, LLControlVariable::PERSIST_ALWAYS));

    // TODO: consider moving proxy initialization here or LLCopocedureManager after proxy initialization, may be implement
    // some other protection to make sure we don't use network before initializng proxy

    /*----------------------------------------------------------------------*/
    // nat 2016-06-29 moved the following here from the former mainLoop().
    mMainloopTimeout = new LLWatchdogTimeout();

    // Create IO Pump to use for HTTP Requests.
    gServicePump = new LLPumpIO(gAPRPoolp);

    // Note: this is where gLocalSpeakerMgr and gActiveSpeakerMgr used to be instantiated.

    LLVoiceChannel::initClass();
    LLVoiceClient::initParamSingleton(gServicePump);
    LLVoiceChannel::setCurrentVoiceChannelChangedCallback(boost::bind(&LLFloaterIMContainer::onCurrentChannelChanged, _1), true);

    joystick = LLViewerJoystick::getInstance();
    joystick->setNeedsReset(true);
    /*----------------------------------------------------------------------*/
    // Load User's bindings
    loadKeyBindings();

    //LLSimpleton creations
    LLEnvironment::createInstance();
    LLWorld::createInstance();
    LLViewerStatsRecorder::createInstance();
    LLSelectMgr::createInstance();
    LLViewerCamera::createInstance();
    LL::GLTFSceneManager::createInstance();


    // Older builds logged further viewers to logs/dump_logs/<uuid>.log.
    if (!mSecondInstance && LLFile::isdir(gDirUtilp->getDumpLogsDirPath()))
    {
        gDirUtilp->deleteDirAndContents(gDirUtilp->getDumpLogsDirPath());
    }
    // Settings temp files (<name>.xml.<pid>.tmp) left by a viewer that died mid-save.
    if (!mSecondInstance)
    {
        gDirUtilp->deleteFilesInDir(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, ""), "*.xml.*.tmp");
    }

    return true;
}

void LLAppViewer::initMaxHeapSize()
{
    //set the max heap size.
    //here is some info regarding to the max heap size:
    //------------------------------------------------------------------------------------------
    // OS       | setting | SL address bits | max manageable memory space | max heap size
    // Win 32   | default | 32-bit          | 2GB                         | < 1.7GB
    // Win 32   | /3G     | 32-bit          | 3GB                         | < 1.7GB or 2.7GB
    //Linux 32  | default | 32-bit          | 3GB                         | < 2.7GB
    //Linux 32  |HUGEMEM  | 32-bit          | 4GB                         | < 3.7GB
    //64-bit OS |default  | 32-bit          | 4GB                         | < 3.7GB
    //64-bit OS |default  | 64-bit          | N/A (> 4GB)                 | N/A (> 4GB)
    //------------------------------------------------------------------------------------------
    //currently SL is built under 32-bit setting, we set its max heap size no more than 1.6 GB.

 #if !defined(LL_X86_64) && !defined(LL_ARM64)
    F32Gigabytes max_heap_size_gb = (F32Gigabytes)gSavedSettings.getF32("MaxHeapSize") ;
#else
    F32Gigabytes max_heap_size_gb = (F32Gigabytes)gSavedSettings.getF32("MaxHeapSize64");
#endif

    LLMemory::initMaxHeapSizeGB(max_heap_size_gb);
}


// externally visible timers
LLTrace::BlockTimerStatHandle FTM_FRAME("Frame");

bool LLAppViewer::frame()
{
    bool ret = false;

    if (gSimulateMemLeak)
    {
        try
        {
            ret = doFrame();
        }
        catch (const LLContinueError&)
        {
            LOG_UNHANDLED_EXCEPTION("");
        }
        catch (std::bad_alloc&)
        {
            LLMemory::logMemoryInfo(true);
            LLFloaterMemLeak* mem_leak_instance = LLFloaterReg::findTypedInstance<LLFloaterMemLeak>("mem_leaking");
            if (mem_leak_instance)
            {
                mem_leak_instance->stop();
            }
            LL_WARNS() << "Bad memory allocation in LLAppViewer::frame()!" << LL_ENDL;
        }
    }
    else
    {
        // #698: out of memory on the main thread is caught at the frame boundary: the
        // reserve is released, the governor sheds (forced CRITICAL), and the next frame
        // runs. HONEST LIMITS: the frame was abandoned wherever the allocation failed --
        // a half-built UI/scene update is simply redone next frame, but a Vulkan frame
        // abandoned mid-record leaves its command buffer open (the VK frame-abort reset
        // is a renderer hook, #697/#699 territory); and a frame that cannot complete N
        // times in a row is not recoverable, so that still ends in the OOM error (with
        // the bad_alloc marker) instead of spinning forever.
        static U32 s_oom_frames = 0;
        try
        {
            ret = doFrame();
            s_oom_frames = 0;
        }
        catch (const LLContinueError&)
        {
            LOG_UNHANDLED_EXCEPTION("");
        }
        catch (const std::bad_alloc&)
        {
            LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_MAIN_FRAME, 0, "main frame");
            constexpr U32 kMaxOOMFrames = 30;
            if (++s_oom_frames >= kMaxOOMFrames)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "#698 out of memory in " << s_oom_frames << " consecutive frames | "
                          << LLMemoryGovernor::allocFailString() << LL_ENDL;
            }
            try
            {
                LL_WARNS() << "#698 out of memory in the main frame (" << s_oom_frames
                           << " in a row): frame abandoned, continuing" << LL_ENDL;
            }
            catch (...)
            {
            }
        }
    }

    return ret;
}

void sendGameControlData()
{
    LLMessageSystem* msg = gMessageSystem;
    const LLGameControl::ServerState& state = LLGameControl::getServerState();

    msg->newMessageFast(_PREHASH_GameControlData);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgentID);
    msg->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
    msg->addU8Fast(_PREHASH_Packet, LLGameControl::getNextPacketNum());
    msg->addU8Fast(_PREHASH_ActionMode, state.mActionMode);

    // Non-modal canonical axes/buttons: pack every available value every message,
    // whether or not it changed since the last one (no per-field diffing). Trailing
    // zero axes are truncated and the Buttons block is skipped entirely when zero --
    // the server assumes anything not packed is zero -- to save bytes.
    size_t num_axes = state.mAxes.size();
    while (num_axes > 0 && state.mAxes[num_axes - 1] == 0)
    {
        --num_axes;
    }
    for (size_t i = 0; i < num_axes; ++i)
    {
        msg->nextBlockFast(_PREHASH_Axes);
        msg->addS16Fast(_PREHASH_Value, state.mAxes[i]);
    }
    if (state.mButtons != 0)
    {
        msg->nextBlockFast(_PREHASH_Buttons);
        msg->addU32Fast(_PREHASH_Flags, state.mButtons);
    }

    // Modal semantic axes/buttons: same truncation policy, except the number of
    // axes considered depends on ActionMode -- see message_template.msg's
    // GameControlData doc and LLGameControl::numSemanticAxesForMode() -- since the
    // server computes its own expected axis count from the mode and zeros anything
    // not packed.
    size_t num_mode_axes = LLGameControl::numSemanticAxesForMode((LLGameControl::AgentControlMode)state.mActionMode);
    while (num_mode_axes > 0 && state.mSemanticAxes[num_mode_axes - 1] == 0)
    {
        --num_mode_axes;
    }
    for (size_t i = 0; i < num_mode_axes; ++i)
    {
        msg->nextBlockFast(_PREHASH_ModeAxes);
        msg->addS16Fast(_PREHASH_Value, state.mSemanticAxes[i]);
    }
    if (state.mSemanticButtons != 0)
    {
        msg->nextBlockFast(_PREHASH_ModeButtons);
        msg->addU32Fast(_PREHASH_Flags, state.mSemanticButtons);
    }

    LLGameControl::updateResendPeriod();
    gAgent.sendMessage();

    // Mirror the just-sent channel values into the controller debug floater.
    SSFloaterGameControlDebug::updateDataOutput();
}

#if CLEAN_PROF
// #FrameProf: idle() CPU wall time, carried from the idle block to the display
// block within a single doFrame() (main thread, non-reentrant).
static F64 sCleanIdleMs = 0.0;
#endif

// ---- A/B BENCH (RenderVKBench*; llvkbench.h) ----
// Steps RenderVKBenchSetting through RenderVKBenchValues (comma-separated),
// RenderVKBenchSeconds per value x RenderVKBenchCycles rounds, skipping the first
// RenderVKBenchSettle s after each switch; logs per value (tag VKBench) and restores the
// setting. Start by setting RenderVKBench TRUE; it clears itself when the run ends.
static std::vector<std::string> bench_split(const std::string& in)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : in)
    {
        // (any whitespace separates too: ' 0, 1' / tabs / newlines never reach a value)
        if (c == ',' || c == ';' || LLStringOps::isSpace(c)) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool bench_set(LLControlVariable* c, const std::string& v)
{
    if (!c) return false;
    switch (c->type())
    {
    case TYPE_BOOLEAN: c->setValue(LLSD(v == "1" || v == "true" || v == "TRUE")); break;
    case TYPE_U32:
    case TYPE_S32:     c->setValue(LLSD((S32)std::strtol(v.c_str(), nullptr, 0))); break;
    case TYPE_F32:     c->setValue(LLSD(std::strtod(v.c_str(), nullptr))); break;
    case TYPE_STRING:  c->setValue(LLSD(v)); break;
    default: return false;
    }
    return true;
}

static void slipstreamBenchTick(double frame_ms)
{
    static LLCachedControl<bool> s_run(gSavedSettings, "RenderVKBench", false);
    LLVKBench::State& st = LLVKBench::state();
    const double now = LLTimer::getTotalSeconds();
    if (!st.active)
    {
        if (!s_run) return;
        // ---- start ----
        st = LLVKBench::State();
        st.setting = gSavedSettings.getString("RenderVKBenchSetting");
        LLStringUtil::trim(st.setting);   // ' RenderVKOcclusion2Phase' must still resolve
        st.values = bench_split(gSavedSettings.getString("RenderVKBenchValues"));
        st.sub_names = bench_split(gSavedSettings.getString("RenderVKBenchSubs"));
        st.seconds = llmax((F32)gSavedSettings.getF32("RenderVKBenchSeconds"), 2.f);
        st.settle = llclamp((F32)gSavedSettings.getF32("RenderVKBenchSettle"), 0.f, (F32)st.seconds - 1.f);
        const U32 cycles = llclamp(gSavedSettings.getU32("RenderVKBenchCycles"), 1u, 20u);
        LLControlVariable* c = gSavedSettings.getControl(st.setting);
        if (!c || st.values.empty() || !bench_set(c, st.values[0]))
        {
            LL_WARNS("VKBench") << "bench: can't run (setting '" << st.setting << "' "
                                << (c ? "has an unsupported type" : "not found") << ", " << st.values.size()
                                << " values)" << LL_ENDL;
            gSavedSettings.setBOOL("RenderVKBench", false);
            return;
        }
        st.original = c->getValue().asString();
        if (c->type() == TYPE_BOOLEAN) st.original = c->getValue().asBoolean() ? "1" : "0";
        for (U32 k = 0; k < cycles; ++k)
            for (const std::string& v : st.values) st.schedule.push_back(v);
        st.active = true;
        st.step = 0;
        st.step_start = now;
        bench_set(c, st.schedule[0]);
        LL_INFOS("VKBench") << "bench start: " << st.setting << " in {" << gSavedSettings.getString("RenderVKBenchValues")
                            << "} x" << cycles << ", " << st.seconds << " s each (settle " << st.settle << " s)"
                            << (gSavedSettings.getBOOL("RenderVKGpuTimers") ? "" : " -- RenderVKGpuTimers is OFF: no GPU numbers")
                            << LL_ENDL;
        return;
    }
    LLControlVariable* c = gSavedSettings.getControl(st.setting);
    if (!s_run || !c)
    {
        LL_INFOS("VKBench") << "bench aborted" << LL_ENDL;
        if (c) bench_set(c, st.original);
        st = LLVKBench::State();
        return;
    }
    const double t = now - st.step_start;
    if (st.collecting && st.step < st.schedule.size())
    {
        LLVKBench::ValAcc& va = st.acc[st.schedule[st.step]];
        va.frames.push_back((float)frame_ms);
        // The just-ended frame's GPU wait (read before frameBoundary clears the slots).
        double acq = 0.0;
        for (const LLVKHitch::Slot& sl : LLVKHitch::state().slots)
            if (sl.name && (!std::strcmp(sl.name, "vk_acquireWait") || !std::strcmp(sl.name, "vk_paceWait")))
                acq += sl.ms;   // (frame pacing moved the slot-fence part of it to the frame top)
        va.acquire.add(acq);
        // CPU sections named in RenderVKBenchSubs (LLVKHitch timers of the frame that
        // just ended; GPU sub-marks arrive separately through LLVKBench::gpuFrame).
        for (const std::string& n : st.sub_names)
        {
            double ms = 0.0;
            bool found = false;
            for (const LLVKHitch::Slot& sl : LLVKHitch::state().slots)
                if (sl.name && sl.timed && n == sl.name) { ms += sl.ms; found = true; }
            if (found) va.subs[n].add(ms);
        }
    }
    st.collecting = t >= st.settle;
    if (t < st.seconds) return;
    // ---- next step ----
    st.collecting = false;
    st.pending.clear();
    ++st.step;
    if (st.step < st.schedule.size())
    {
        bench_set(c, st.schedule[st.step]);
        st.step_start = now;
        return;
    }
    // ---- done: report + restore ----
    std::ostringstream os;
    os.setf(std::ios::fixed); os.precision(2);
    os << "bench result: " << st.setting;
    for (const std::string& v : st.values)
    {
        LLVKBench::ValAcc& va = st.acc[v];
        double sum = 0.0;
        for (float f : va.frames) sum += f;
        const double avg = va.frames.empty() ? 0.0 : sum / va.frames.size();
        std::vector<float> tmp = va.frames;
        const double p50 = LLVKHitch::percentile(tmp, 0.50);
        const double p99 = LLVKHitch::percentile(tmp, 0.99);
        os << "\n  " << st.setting << "=" << v << ": frames=" << va.frames.size() << " frame avg=" << avg
           << "ms (" << (avg > 0.0 ? 1000.0 / avg : 0.0) << " fps) p50=" << p50 << " p99=" << p99
           << " acquireWait=" << va.acquire.avg();
        if (va.gpu.n) os << " gpu_total=" << va.gpu.avg() << " (n=" << va.gpu.n << ")";
        for (const std::string& n : st.sub_names)
        {
            auto it = va.subs.find(n);
            if (it != va.subs.end()) os << " " << n << "=" << it->second.avg();
        }
    }
    LL_INFOS("VKBench") << os.str() << LL_ENDL;
    bench_set(c, st.original);
    st = LLVKBench::State();
    gSavedSettings.setBOOL("RenderVKBench", false);
}

bool LLAppViewer::doFrame()
{
    LL_RECORD_BLOCK_TIME(FTM_FRAME);
    {   // A/B bench: sample the frame that just ended (before the hitch slots reset).
        static auto s_bench_prev = std::chrono::steady_clock::now();
        const auto bnow = std::chrono::steady_clock::now();
        const double bench_ms = std::chrono::duration<double, std::milli>(bnow - s_bench_prev).count();
        s_bench_prev = bnow;
        slipstreamBenchTick(bench_ms);
    }
    {   // Hitch recorder (Vulkan): attribute spiked frames to their sections.
        static LLCachedControl<bool> s_hitch(gSavedSettings, "RenderVKHitchLog", false);
        static LLCachedControl<bool> s_sections(gSavedSettings, "RenderVKSectionLog", false);
        static LLCachedControl<F32> s_event_thr(gSavedSettings, "StatsFrametimeEventThreshold", 0.1f);
        static LLCachedControl<U32> s_long_frame(gSavedSettings, "VKLongFrameLogMs", 1000);   // #700
        std::string sections;
        // Hitch lines come with the section log too: its 5 s averages can't show spikes;
        // and for a while after a graphics settings change (#521, LLVKHitch::armWatch).
        const bool watch = LLVKHitch::watchTick();
        // #422 auto-tuner: samples the frame that just ended (its GPU waits are in the
        // hitch slots until frameBoundary clears them), then measures / steps.
        LLVKAutoTune::instance().frameTick();
        // #966 Performance window timings / captures: same frame, same slots (one flag test
        // while the window is closed and no capture runs).
        LLVKPerfCapture::instance().frameTick();
        const std::string hitch = LLVKHitch::frameBoundary(s_hitch || s_sections || watch,
                                                           s_sections, &sections,
                                                           (F32)s_event_thr, (double)s_long_frame);
        // #700: ring of recent frame times, per-frame GPU-wait table, memory-state line.
        // #697/#699: the vkgfx side keeps its own per-site wait table and low-memory counters.
        static const bool s_vk_provider = (LLMemDiag::setSoftFailProvider([]
        {
            const std::string s = vkLowMemSummary();
            return s == "none" ? std::string() : s;
        }), true);
        (void)s_vk_provider;
        std::string vk_waits;
        {
            const VkWaitFrameStat w = vkWaitFrameTake();
            if (w.waits || w.elided)
            {
                vk_waits = llformat(" vk %u waits %.1fms (%u elided)", w.waits, w.ms, w.elided);
                if (w.worstFile) vk_waits += llformat(" worst %s:%d %.1fms", gDirUtilp->getBaseFileName(w.worstFile).c_str(), w.worstLine, w.worstMs);
            }
        }
        LLMemDiag::frameDone(LLVKHitch::state().prev_ms, (U32)gFrameCount, vk_waits);
        {   // Crash breadcrumb (#757 / #754 / #736): what the crash line reports besides the
            // open sections -- the frame, the teleport phase and how long it has lasted, the
            // renderer's VRAM view. Plain stores; read by llwindebug.cpp's crash report.
            LLVKHitch::Breadcrumb& bc = LLVKHitch::breadcrumb();
            const int tp = (int)gAgent.getTeleportState();
            bc.tp_frames = (tp == bc.teleport) ? bc.tp_frames + 1 : 0;
            bc.teleport = tp;
            bc.frame = (unsigned)gFrameCount;
            if ((gFrameCount % 30) == 0 && LLVulkanContext::instance().isInitialized())
            {
                const LLVKVramSummary v = LLVulkanContext::instance().vramSummary();
                if (v.valid)
                {
                    bc.vram_used_mb = (unsigned)(v.usage >> 20);
                    bc.vram_budget_mb = (unsigned)(v.budget >> 20);
                }
            }
        }
        if (!hitch.empty())
        {
            LL_INFOS("VKHitch") << hitch << LLMemDiag::lastFrameGpuWaits() << LL_ENDL;
            // The frames around it (pace / CPU / acquire / present / GPU per frame): tells a
            // real hitch from uneven frame pacing. At most one per second.
            const std::string tl = LLVKHitch::timelineLine(1.0);
            if (!tl.empty()) LL_INFOS("VKHitch") << tl << LL_ENDL;
        }
        LLMemDiag::tick();
        if (!sections.empty())
        {
            LL_INFOS("VKHitch") << sections << LL_ENDL;
            // WHERE the section averages above were taken: a before/after FPS comparison is
            // only valid at the same spot and facing (2026-10-13 "slower after PEAK" report:
            // the "before" window was a skybox 2300 m up). Region, region-local camera
            // position + view direction, agent altitude.
            if (LLViewerCamera::instanceExists())
            {
                const LLViewerCamera* cam = LLViewerCamera::getInstance();
                const LLViewerRegion* reg = gAgent.getRegion();
                const LLVector3 eye = cam->getOrigin();   // agent space = agent region local
                const LLVector3 at = cam->getAtAxis();
                const LLVector3 ap = gAgent.getPositionAgent();
                LL_INFOS("VKHitch") << "WHERE region '" << (reg ? reg->getName() : std::string("?"))
                                    << llformat("' cam (%.1f, %.1f, %.1f) at (%.2f, %.2f, %.2f) agent (%.1f, %.1f, %.1f) frame %u",
                                                eye.mV[0], eye.mV[1], eye.mV[2], at.mV[0], at.mV[1], at.mV[2],
                                                ap.mV[0], ap.mV[1], ap.mV[2], (U32)gFrameCount)
                                    << LL_ENDL;
            }
        }
    }
    LL_PROFILE_GPU_ZONE("Frame");

    // FRAME PACING (RenderVKFramePacing): wait for the frame-in-flight slot this frame will
    // render into HERE, before input, idle() and the simulation -- not mid-frame inside
    // renderFrame's acquire, after the world was already advanced and snapshotted. With the
    // wait mid-frame the CPU ran up to a frame ahead, then stalled ~2 GPU frames (a 1080 Ti
    // at 4K: ~25 ms CPU frames then a ~63 ms one, GPU a steady 32 ms), so displayed frames
    // carried uneven simulated time = micro-stutter on a steady GPU. Here the frame's time
    // step starts right after the GPU frees a slot. acquire() keeps its fence wait (free
    // now). Safe on any frame: no VK frame rendered (minimized, skipped, swapchain
    // recreate) leaves the slot's fence signaled, the first frame's fences start signaled,
    // a lost device / no context returns at once. After frameBoundary, so the wait is
    // attributed to this frame (vk_paceWait) like vk_acquireWait was.
    {
        static LLCachedControl<bool> s_pacing(gSavedSettings, "RenderVKFramePacing", true);
        if (s_pacing && !LLApp::isExiting() && LLVulkanContext::instance().isInitialized())
            LLVulkanContext::instance().waitFrameSlot();
    }

    {
    // (#422: LL's LLPerfStats auto-tune -- the deferred setting writes applied here -- is
    // replaced by LLVKAutoTune, ticked with the frame boundary above.)
    LLPerfStats::RecordSceneTime T (LLPerfStats::StatType_t::RENDER_FRAME);
    if (!LLWorld::instanceExists())
    {
        LLWorld::createInstance();
    }

    // Looked up once: the "mainloop" pump is only ever obtain()ed (owned by
    // LLEventPumps, never destroyed or replaced) and is not deleted until
    // LLSingletonBase::deleteAll() in cleanup(), after the last doFrame().
    // reset() at shutdown resets listeners but keeps the instance.
    static LLEventPump& mainloop(LLEventPumps::instance().obtain("mainloop"));
    LLSD newFrame;
    {
        LLPerfStats::RecordSceneTime T (LLPerfStats::StatType_t::RENDER_IDLE); // perf stats
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df LLTrace");
            static const LLFloaterReg::instance_list_t* s_block_timers = nullptr;
            if (!s_block_timers)
            {
                s_block_timers = LLFloaterReg::findInstanceList("block_timers");
            }
            if (s_block_timers && LLFloater::isVisible(LLFloaterReg::findInstanceIn(*s_block_timers)))
            {
                LLTrace::BlockTimer::processTimes();
            }

            LLTrace::get_frame_recording().nextPeriod();
            LLTrace::BlockTimer::logStats();
        }

        LLTrace::get_thread_recorder()->pullFromChildren();

        //clear call stack records
        LL_CLEAR_CALLSTACKS();
    }
    {
        {
            LLPerfStats::RecordSceneTime T(LLPerfStats::StatType_t::RENDER_IDLE); // ensure we have the entire top scope of frame covered (input event and coro)
            LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df processMiscNativeEvents")
            pingMainloopTimeout("Main:MiscNativeWindowEvents");

            if (gViewerWindow)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("System Messages");
                VK_HITCH_SCOPE("df_nativeEvents");
                gViewerWindow->getWindow()->processMiscNativeEvents();
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df gatherInput")
                pingMainloopTimeout("Main:GatherInput");
            }

            if (gViewerWindow)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("System Messages");
                if (!restoreErrorTrap())
                {
                    LL_WARNS() << " Someone took over my signal/exception handler (post messagehandling)!" << LL_ENDL;
                }

                VK_HITCH_SCOPE("df_gatherInput");
                gViewerWindow->getWindow()->gatherInput(gFocusMgr.getAppHasFocus());
            }

            //memory leaking simulation
            if (gSimulateMemLeak)
            {
                LLFloaterMemLeak* mem_leak_instance =
                    LLFloaterReg::findTypedInstance<LLFloaterMemLeak>("mem_leaking");
                if (mem_leak_instance)
                {
                    mem_leak_instance->idle();
                }
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df mainloop");
                // canonical per-frame event
                VK_HITCH_SCOPE("df_mainloopPost");
                mainloop.post(newFrame);
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df suspend");
                // give listeners a chance to run
                VK_HITCH_SCOPE("df_coroSuspend");
                llcoro::suspend();
                // if one of our coroutines threw an uncaught exception, rethrow it now
                LLCoros::instance().rethrow();
            }
        }

        // GRAPHICS DEVICE LOST (#135): vkgfx stopped all GPU work and logged the report;
        // tell the user and leave through the normal cleanup() before idle() / display()
        // run another frame against the dead device.
        if (vkDeviceIsLost() && !LLApp::isExiting())
        {
            onGraphicsDeviceLost();
        }

        if (!LLApp::isExiting())
        {
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df JoystickKeyboard");
                pingMainloopTimeout("Main:JoystickKeyboard");

                // Scan keyboard for movement keys.  Command keys and typing
                // are handled by windows callbacks.  Don't do this until we're
                // done initializing.  JC
                if (gViewerWindow
                    && gViewerWindow->getWindow()->getVisible()
                    && gViewerWindow->getActive()
                    && !gViewerWindow->getWindow()->getMinimized()
                    && LLStartUp::getStartupState() == STATE_STARTED
                    && !gViewerWindow->getShowProgress()
                    && !gFocusMgr.focusLocked())
                {
                    LLPerfStats::RecordSceneTime T(LLPerfStats::StatType_t::RENDER_IDLE);
                    VK_HITCH_SCOPE("df_scanInput");
                    joystick->scanJoystick();
                    gKeyboard->scanKeyboard();
                    gViewerInput.scanMouse();
                    // Hold the crouch toggle (Shift+C) for this frame.
                    gAgent.updateCrouch();
                }
            }

            // Update state based on messages, user input, object idle.
            {
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df pauseMainloopTimeout");
                    pauseMainloopTimeout(); // *TODO: Remove. Messages shouldn't be stalling for 20+ seconds!
                }

                {
                    LLPerfStats::RecordSceneTime T (LLPerfStats::StatType_t::RENDER_IDLE);
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df idle");
#if CLEAN_PROF
                    LLTimer clean_idle_timer;
#endif
                    idle();
#if CLEAN_PROF
                    sCleanIdleMs = clean_idle_timer.getElapsedTimeF64() * 1000.0;
#endif
                }

                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df resumeMainloopTimeout");
                    resumeMainloopTimeout();
                }
            }

            if (gDoDisconnect && (LLStartUp::getStartupState() == STATE_STARTED))
            {
                pauseMainloopTimeout();
                saveFinalSnapshot();

                if (LLVoiceClient::instanceExists())
                {
                    LLVoiceClient::getInstance()->terminate();
                }

                disconnectViewer();
                resumeMainloopTimeout();
            }

            // Render scene.
            if (!LLApp::isExiting() && gViewerWindow)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df Display");
                pingMainloopTimeout("Main:Display");

#if CLEAN_PROF
                LLTimer clean_display_timer;
#endif
                {
                    VK_HITCH_SCOPE("display_total");
                    display();
                }
#if CLEAN_PROF
                {
                    F64 disp_ms = clean_display_timer.getElapsedTimeF64() * 1000.0;
                    // #FrameProf: coarse CPU-wall split -- idle() is LL world/update
                    // (CPU), display() is render submit + GPU wait + swap. If idle
                    // dominates while clean GPU is tiny, we're CPU-bound in LL's frame
                    // loop (the GPU-driven endgame target); if display dominates, the
                    // cost is render-side (probes/mirror/lighting/post).
                    LL_INFOS("FrameProf") << "idle_ms=" << (F32)sCleanIdleMs
                                          << " display_ms=" << (F32)disp_ms << LL_ENDL;
                }
#endif

                {
                    LLPerfStats::RecordSceneTime T(LLPerfStats::StatType_t::RENDER_IDLE);
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df Snapshot");
                    pingMainloopTimeout("Main:Snapshot");
                    // mReflectionMapManager.update() now runs inside
                    // display() before cull cleanup (see
                    // llviewerdisplay.cpp where the same call was
                    // moved). Running it after display end gave the
                    // network/object update layer a window to destroy
                    // LLDrawInfo's that our probe-render cull was
                    // still referencing, causing intermittent crashes.
                    //
                    // Vulkan: LLViewerWindow::rawSnapshot captures from the VK frame
                    // (one-shot swap-image copy), so the snapshot floaters run here too.
                    // (This skip is why the floater never produced a preview under VK.)
                    LLFloaterSnapshot::update(); // take snapshots
                    LLFloaterSimpleSnapshot::update();
                }

                if (LLViewerStatsRecorder::instanceExists())
                {
                    VK_HITCH_SCOPE("df_statsRecorder");
                    LLViewerStatsRecorder::instance().idle();
                }
            }
        }

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df pauseMainloopTimeout");
            pingMainloopTimeout("Main:Sleep");

            pauseMainloopTimeout();
        }

        // Sleep and run background threads
        {
            //LL_RECORD_BLOCK_TIME(SLEEP2);
            LL_PROFILE_ZONE_WARN("Sleep2");

            // yield some time to the os based on command line option
            static LLCachedControl<S32> yield_time(gSavedSettings, "YieldTime", -1);
            if(yield_time >= 0)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("Yield");
                LL_PROFILE_ZONE_NUM(yield_time);
                ms_sleep(yield_time);
            }

            // "Background" = window hidden/minimized or the app does not have focus (same test
            // the legacy BackgroundYieldTime path has always used). Not applied while quitting
            // (causes trouble at mac's cleanup stage).
            const bool in_background = !LLApp::isExiting()
                && ((gViewerWindow && !gViewerWindow->getWindow()->getVisible())
                    || !gFocusMgr.getAppHasFocus());

            // Frame rate cap: pace frames to a deadline that advances by one frame budget.
            // Only the unused part of the budget is slept, so a frame that is already slower
            // than the cap is never delayed (the deadline just resets to now).
            // In the background, BackgroundFPSLimit also applies (the lower cap wins).
            static LLCachedControl<S32> fps_limit(gSavedSettings, "RenderFPSLimit", 0);
            static LLCachedControl<S32> bg_fps_limit(gSavedSettings, "BackgroundFPSLimit", 25);
            {
                using clock = std::chrono::steady_clock;
                static clock::time_point s_deadline{};
                S32 limit = fps_limit;
                const S32 bg_limit = in_background ? (S32)bg_fps_limit : 0;
                const bool bg_paced = bg_limit > 0 && (limit <= 0 || bg_limit < limit);
                if (bg_paced)
                {
                    limit = bg_limit;
                }
                if (limit > 0)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("FPSLimit");
                    const auto budget = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / llclamp(limit, 1, 1000)));
                    const clock::time_point now = clock::now();
                    if (s_deadline == clock::time_point{} || now >= s_deadline || s_deadline - now > budget)
                    {
                        // Behind (or first frame / limit changed): no wait this frame.
                        s_deadline = now + budget;
                    }
                    else if (bg_paced)
                    {
                        // Background: precision does not matter, so no yield-spin; give the
                        // whole remaining budget back to the OS.
                        LLPerfStats::RecordSceneTime T ( LLPerfStats::StatType_t::RENDER_SLEEP );
                        std::this_thread::sleep_until(s_deadline);
                        s_deadline += budget;
                    }
                    else
                    {
                        VK_HITCH_SCOPE("fpsLimitSleep");   // #422: not main-thread work (auto-tune CPU time)
                        // OS sleeps can overshoot by ~1 ms or more (Windows timer granularity);
                        // sleep to just short of the deadline, then yield-spin the rest. Linux
                        // sleeps are accurate to well under 0.5 ms, so spin less there.
#if LL_WINDOWS
                        const auto slack = std::chrono::microseconds(2000);
#else
                        const auto slack = std::chrono::microseconds(500);
#endif
                        if (s_deadline - now > slack)
                        {
                            std::this_thread::sleep_until(s_deadline - slack);
                        }
                        while (clock::now() < s_deadline)
                        {
                            std::this_thread::yield();
                        }
                        s_deadline += budget;
                    }
                }
                else
                {
                    s_deadline = clock::time_point{};
                }
            }

            if (gNonInteractive)
            {
                S32 non_interactive_ms_sleep_time = 100;
                ms_sleep(non_interactive_ms_sleep_time);
            }

            // Legacy background yield: a fixed sleep every frame when not running as the
            // foreground window. Only used when BackgroundFPSLimit is off (0); the paced
            // limiter above replaces it otherwise.
            if (in_background && bg_fps_limit <= 0)
            {
                // Sleep if we're not rendering, or the window is minimized.
                static LLCachedControl<S32> s_background_yield_time(gSavedSettings, "BackgroundYieldTime", 0);
                S32 milliseconds_to_sleep = llclamp((S32)s_background_yield_time, 0, 1000);
                // don't sleep when BackgroundYieldTime set to 0, since this will still yield to other threads
                // of equal priority on Windows
                if (milliseconds_to_sleep > 0)
                {
                    LLPerfStats::RecordSceneTime T ( LLPerfStats::StatType_t::RENDER_SLEEP );
                    ms_sleep(milliseconds_to_sleep);
                }
            }

            if (mRandomizeFramerate)
            {
                ms_sleep(rand() % 200);
            }

            if (mPeriodicSlowFrame
                && (gFrameCount % 10 == 0))
            {
                LL_INFOS() << "Periodic slow frame - sleeping 500 ms" << LL_ENDL;
                ms_sleep(500);
            }

            {
                F32 max_time = llmin(gFrameIntervalSeconds.value() *10.f, 1.f);

                {
                    VK_HITCH_SCOPE("df_textureThreads");
                    updateTextureThreads(max_time);
                }

                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_APP("LFS Thread");
                    VK_HITCH_SCOPE("df_lfs");
                    LLLFSThread::updateClass(1);
                }

                // Original code slept the main thread for up to 100 ms when
                // io_pending > 1000, with a comment about "letting the lfs
                // catch up". The LFS thread is async -- sleeping the render
                // thread does not give it CPU; it just blocks rendering. On
                // any active scene io_pending sits well over 1000, so this
                // turned into a per-frame sleep that capped framerate hard
                // (10 ms = 100 fps ceiling; 50 ms = 20 fps). Removed.
                // If the file thread genuinely needs back-pressure the right
                // place is the producer side, not the renderer.

            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df gMeshRepo");
                VK_HITCH_SCOPE("df_meshRepo");
                gMeshRepo.update() ;
                // notifyLoadedMeshes() used to be called from LLPipeline during the
                // display() path, which Vulkan does not run. gMeshRepo.update() (the fetch pump) runs here in VK, so mesh
                // bytes DO get fetched/decoded -- but nothing consumes the completed loads:
                // the objects are never told "your real triangles are ready", so mesh
                // LLVolumes stay at their low-poly placeholder and the clean-store capture
                // only ever sees a few hundred verts for a whole region (mesh renders as
                // invisible specks). Drive the completion pump here in VK so mesh geometry
                // resolves. The notifyMeshLoaded->markRebuild() it triggers is a
                // non-fatal queue push.
                gMeshRepo.notifyLoadedMeshes();
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("df resumeMainloopTimeout");
                resumeMainloopTimeout();
            }
            pingMainloopTimeout("Main:End");
        }
    }

    if (LLApp::isExiting())
    {
        // Save snapshot for next time, if we made it through initialization
        if (STATE_STARTED == LLStartUp::getStartupState())
        {
            saveFinalSnapshot();
        }

        if (LLVoiceClient::instanceExists())
        {
            LLVoiceClient::getInstance()->terminate();
        }

        delete gServicePump;
        gServicePump = NULL;

        destroyMainloopTimeout();

        LL_INFOS() << "Exiting main_loop" << LL_ENDL;
    }
    }LLPerfStats::StatsRecorder::endFrame();
    LL_PROFILER_FRAME_END;

    return ! LLApp::isRunning();
}

S32 LLAppViewer::updateTextureThreads(F32 max_time)
{
    size_t work_pending = 0;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_APP("Texture Cache");
        work_pending += LLAppViewer::getTextureCache()->update(max_time); // unpauses the texture cache thread
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_APP("Image Decode");
        work_pending += LLAppViewer::getImageDecodeThread()->update(max_time); // unpauses the image thread
    }
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_APP("Image Fetch");
        work_pending += LLAppViewer::getTextureFetch()->update(max_time); // unpauses the texture fetch thread
    }
    return static_cast<S32>(work_pending);
}

void LLAppViewer::flushLFSIO()
{
    S32 pending = LLLFSThread::updateClass(0);
    if (pending > 0)
    {
        LL_INFOS() << "Waiting for pending IO to finish: " << pending << LL_ENDL;
        while (1)
        {
            pending = LLLFSThread::updateClass(0);
            if (!pending)
            {
                break;
            }
            ms_sleep(100);
        }
    }
}

bool LLAppViewer::cleanup()
{
#if SL_TESTCTL
    SLTestCtl::stop();
#endif

    //ditch LLVOAvatarSelf instance
    gAgentAvatarp = NULL;

    LLNotifications::instance().clear();

    // workaround for DEV-35406 crash on shutdown
    LLEventPumps::instance().reset(true);

    // There used to be an 'if (LLFastTimerView::sAnalyzePerformance)' block
    // here, completely redundant with the one that occurs later in this same
    // function. Presumably the duplication was due to an automated merge gone
    // bad. Not knowing which instance to prefer, we chose to retain the later
    // one because it happens just after mFastTimerLogThread is deleted. This
    // comment is in case we guessed wrong, so we can move it here instead.

#if LL_LINUX
    // remove any old breakpad minidump files from the log directory
    if (! isError())
    {
        std::string logdir = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "");
        gDirUtilp->deleteFilesInDir(logdir, "*-*-*-*-*.dmp");
    }
#endif

    // Kill off LLLeap objects. We can find them all because LLLeap is derived
    // from LLInstanceTracker.
    LLLeap::instance_snapshot().deleteAll();

    //flag all elements as needing to be destroyed immediately
    // to ensure shutdown order
    LLMortician::setZealous(true);

    // Give any remaining SLPlugin instances a chance to exit cleanly.
    LLPluginProcessParent::shutdown();

    // #529: session data usage, while HTTP, messaging and the caches still exist
    SLDataUsage::logSummary("exit");

    disconnectViewer();
    LLViewerCamera::deleteSingleton();

    LL_INFOS() << "Viewer disconnected" << LL_ENDL;

    if (gKeyboard)
    {
        gKeyboard->resetKeys();
    }

    display_cleanup();

    release_start_screen(); // just in case

    LLError::logToFixedBuffer(NULL); // stop the fixed buffer recorder

    LL_INFOS() << "Cleaning Up" << LL_ENDL;

    // shut down mesh streamer
    LLCleanMeshAsset::instance().stopDecodes();   // before the mesh repo (adopt hook) and before static dtors
    gMeshRepo.shutdown();

    // shut down Havok
    LLPhysicsExtensions::quitSystem();

    // Must clean up texture references before viewer window is destroyed.
    if(LLHUDManager::instanceExists())
    {
        LLHUDManager::getInstance()->updateEffects();
        LLHUDObject::updateAll();
        LLHUDManager::getInstance()->cleanupEffects();
        LLHUDObject::cleanupHUDObjects();
        LL_INFOS() << "HUD Objects cleaned up" << LL_ENDL;
    }

    LLKeyframeDataCache::clear();

    // End TransferManager before deleting systems it depends on (Audio, AssetStorage)
#if 0 // this seems to get us stuck in an infinite loop...
    gTransferManager.cleanup();
#endif

    // Note: this is where gWorldMap used to be deleted.

    // Note: this is where gHUDManager used to be deleted.
    if(LLHUDManager::instanceExists())
    {
        LLHUDManager::getInstance()->shutdownClass();
    }

    delete gAssetStorage;
    gAssetStorage = NULL;

    LLPolyMesh::freeAllMeshes();

    LLStartUp::cleanupNameCache();

    // Note: this is where gLocalSpeakerMgr and gActiveSpeakerMgr used to be deleted.

    if (LLWorldMap::instanceExists())
    {
        LLWorldMap::getInstance()->reset(); // release any images
    }

    LLCalc::cleanUp();

    LL_INFOS() << "Global stuff deleted" << LL_ENDL;

    SLStreamInfo::deleteSingleton();

    if (gAudiop)
    {
        LL_INFOS() << "Shutting down audio" << LL_ENDL;

        // #892: the stream speakers / bed (engine-owned) go first: they read the stream's ring.
        SLParcelSpeakerAudio::shutdown();
        // be sure to stop the internet stream cleanly BEFORE destroying the interface to stop it.
        gAudiop->stopInternetStream();
        // shut down the streaming audio sub-subsystem first, in case it relies on not outliving the general audio subsystem.
        LLStreamingAudioInterface *sai = gAudiop->getStreamingAudioImpl();
        delete sai;
        gAudiop->setStreamingAudioImpl(NULL);

        // #783: the weather loops (engine-owned), before the engine goes.
        SLWeatherAudio::shutdown();
        // shut down the audio subsystem
        gAudiop->shutdown();

        delete gAudiop;
        gAudiop = NULL;
    }

    // Note: this is where LLFeatureManager::getInstance()-> used to be deleted.

    // Patch up settings for next time
    // Must do this before we delete the viewer window,
    // such that we can suck rectangle information out of
    // it.
    cleanupSavedSettings();
    LL_INFOS() << "Settings patched up" << LL_ENDL;

    // delete some of the files left around in the cache.
    removeCacheFiles("*.wav");
    removeCacheFiles("*.tmp");
    removeCacheFiles("*.lso");
    removeCacheFiles("*.out");
    removeCacheFiles("*.bodypart");
    removeCacheFiles("*.clothing");

    // Decoded sounds (#176) live in the sound cache folder and are kept for the next
    // session unless SlipstreamKeepDecodedSounds is off. A second instance leaves them to
    // the first one, which may still be playing from them.
    // GPU-ready texture cache (#274): stop its disk workers (queued writes are dropped;
    // a partial file is a *.part, deleted at the next startup) and log the stats.
    LLGpuTexCache::shutdown();
    // #714 / #715: the .gtx migration (a file in flight finishes its record; the rest
    // waits for the next start), then the cache store (flushes queued writes, writes the
    // index snapshot; a second instance discards its private overflow).
    sGpuTexMigration.stop();
    // #714 / #715 J2C: the legacy migration (an entry in flight finishes; the rest waits for
    // the next start), then the texture cache lets go of the store (waits for a store call
    // in flight on the cache thread; later reads miss and writes are refused while the
    // texture threads drain below), then the tiers helper.
    sJ2CMigration.stop();
    LLAppViewer::getTextureCache()->detachStore();
    SLTextureTiers::instance().stop();
    // 1a (#744): cluster LOD builder workers (a running build finishes first) and the cluster
    // LOD disk cache workers (queued writes dropped; a partial file is a *.part, deleted at the
    // next start). Both are idle (never started) when RenderVKClusterLOD was off all session.
    // Before the store closes: with CacheStoreCluster its reads / writes use the store.
    LLCleanClusterBuilder::instance().stop();
    LLCleanClusterCache::instance().shutdown();
    // #715: the asset / cluster migrations stop (a file in flight finishes its records), staged
    // asset writes are committed and LLFileSystem lets go of the store (calls in flight on the
    // mesh / General threads finish first; later ones see the old files' API with no files).
    SLAssetCacheStore::stop();
    SLCacheMetrics::beforeStoreClose();   // #715: last store sample while it is open
    SLCacheStore::instance().close();

    if (LLSoundCache::isInitialized())
    {
        LLSoundCache::logStats("shutdown");
        if (!mSecondInstance && !gSavedSettings.getBOOL("SlipstreamKeepDecodedSounds"))
        {
            LL_INFOS("SoundCache") << "Keep decoded sounds is off; clearing the sound cache" << LL_ENDL;
            LLSoundCache::clear();
        }
    }

    LL_INFOS() << "Cache files removed" << LL_ENDL;

    LL_INFOS() << "Shutting down Views" << LL_ENDL;

    // Destroy the UI
    if( gViewerWindow)
        gViewerWindow->shutdownViews();

    LL_INFOS() << "Cleaning up Inventory" << LL_ENDL;

    // Cleanup Inventory after the UI since it will delete any remaining observers
    // (Deleted observers should have already removed themselves)
    gInventory.cleanupInventory();

    LLCoros::getInstance()->printActiveCoroutines();

    LL_INFOS() << "Cleaning up Selections" << LL_ENDL;

    // Clean up selection managers after UI is destroyed, as UI may be observing them.
    // Clean up before GL is shut down because we might be holding on to objects with texture references
    LLSelectMgr::cleanupGlobals();

    LL_INFOS() << "Cleaning up Objects" << LL_ENDL;

    LLViewerObject::cleanupVOClasses();

    SUBSYSTEM_CLEANUP(LLAvatarAppearance);

    LLTracker::cleanupInstance();

    // *FIX: This is handled in LLAppViewerWin32::cleanup().
    // I'm keeping the comment to remember its order in cleanup,
    // in case of unforseen dependency.
    //#if LL_WINDOWS
    //  gDXHardware.cleanup();
    //#endif // LL_WINDOWS

    LLVolumeMgr* volume_manager = LLPrimitive::getVolumeManager();
    if (!volume_manager->cleanup())
    {
        LL_WARNS() << "Remaining references in the volume manager!" << LL_ENDL;
    }
    LLPrimitive::cleanupVolumeManager();

    LL_INFOS() << "Additional Cleanup..." << LL_ENDL;

    LLViewerParcelMgr::cleanupGlobals();

    // *Note: this is where gViewerStats used to be deleted.

    //end_messaging_system();

    LLPrimitive::cleanupVolumeManager();
    SUBSYSTEM_CLEANUP(LLWorldMapView);
    SUBSYSTEM_CLEANUP(LLFolderViewItem);

    LL_INFOS() << "Saving Data" << LL_ENDL;

    // Store the time of our current logoff
    gSavedPerAccountSettings.setU32("LastLogoff", (U32)time_corrected());

    GMDXRegionCapture::shutdown();   // <GMDX> a region map capture's settings back before they are saved

    if (LLEnvironment::instanceExists())
    {
        //Store environment settings if necessary
        LLEnvironment::getInstance()->saveToSettings();
    }

    // Must do this after all panels have been deleted because panels that have persistent rects
    // save their rects on delete.
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);

    LLUIColorTable::instance().saveUserSettings();

    // PerAccountSettingsFile should be empty if no user has been logged on.
    // *FIX:Mani This should get really saved in a "logoff" mode.
    if (gSavedSettings.getString("PerAccountSettingsFile").empty())
    {
        LL_INFOS() << "Not saving per-account settings; don't know the account name yet." << LL_ENDL;
    }
    // Only save per account settings if the previous login succeeded, otherwise
    // we might end up with a cleared out settings file in case a previous login
    // failed after loading per account settings.
    else if (!mSavePerAccountSettings)
    {
        LL_INFOS() << "Not saving per-account settings; last login was not successful." << LL_ENDL;
    }
    else
    {
        gSavedPerAccountSettings.saveToFile(gSavedSettings.getString("PerAccountSettingsFile"), true);
        LL_INFOS() << "Saved settings" << LL_ENDL;

        if (LLViewerParcelAskPlay::instanceExists())
        {
            LLViewerParcelAskPlay::getInstance()->saveSettings();
        }
    }

    std::string warnings_settings_filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, getSettingsFilename("Default", "Warnings"));
    gWarningSettings.saveToFile(warnings_settings_filename, true);

    // Save URL history file
    LLURLHistory::saveFile("url_history.xml");

    // save mute list. gMuteList used to also be deleted here too.
    if (gAgent.isInitialized() && LLMuteList::instanceExists())
    {
        LLMuteList::getInstance()->cache(gAgent.getID());
    }

    //save call log list
    if (LLConversationLog::instanceExists())
    {
        LLConversationLog::instance().cache();
    }

    clearSecHandler();

    if (mPurgeCacheOnExit)
    {
        LL_INFOS() << "Purging all cache files on exit" << LL_ENDL;
        gDirUtilp->deleteFilesInDir(gDirUtilp->getExpandedFilename(LL_PATH_CACHE,""), "*.*");
    }

    writeDebugInfo();

    LLLocationHistory::getInstance()->save();

    LLAvatarIconIDCache::getInstance()->save();

    // Stop the plugin read thread if it's running.
    LLPluginProcessParent::setUseReadThread(false);

    LL_INFOS() << "Shutting down Threads" << LL_ENDL;

    // Let threads finish
    LLTimer idleTimer;
    idleTimer.reset();
    const F64 max_idle_time = 5.f; // 5 seconds
    while(1)
    {
        S32 pending = 0;
        pending += static_cast<S32>(LLAppViewer::getTextureCache()->update(1)); // unpauses the worker thread
        pending += static_cast<S32>(LLAppViewer::getImageDecodeThread()->update(1)); // unpauses the image thread
        pending += static_cast<S32>(LLAppViewer::getTextureFetch()->update(1)); // unpauses the texture fetch thread
        pending += LLLFSThread::updateClass(0);
        F64 idle_time = idleTimer.getElapsedTimeF64();
        if(!pending)
        {
            break ; //done
        }
        else if(idle_time >= max_idle_time)
        {
            LL_WARNS() << "Quitting with pending background tasks." << LL_ENDL;
            break;
        }
    }

    if (mPurgeUserDataOnExit)
    {
        // Ideally we should not save anything from this session since it is going to be purged now,
        // but this is a very 'rare' case (user deleting himself), not worth overcomplicating 'save&cleanup' code
        std::string user_path = gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + LLStartUp::getUserId();
        gDirUtilp->deleteDirAndContents(user_path);
    }

    // Delete workers first
    // shotdown all worker threads before deleting them in case of co-dependencies
    mAppCoreHttp.requestStop();
    LLMemoryGovernor::shutdown();   // admit everything: no worker may stay parked on the budget
    sTextureFetch->shutdown();
    sTextureCache->shutdown();
    sImageDecodeThread->shutdown();
    sPurgeDiskCacheThread->shutdown();
    if (mGeneralThreadPool)
    {
        mGeneralThreadPool->close();
    }

    sTextureFetch->shutDownTextureCacheThread() ;
    LLLFSThread::sLocal->shutdown();
    // #715: every cache has stopped: final CacheMetrics block + <logs>/cache_metrics_<time>.json
    SLCacheMetrics::shutdown();

    // #275: release every texture NOW, while the renderer still exists, in LL's
    // shutdownGL() order. That teardown was skipped in Vulkan mode and then deleted with
    // shutdownGL(), so the texture statics (default/white/missing images,
    // LLViewerMediaTexture::sMediaMap, ...) were only released by static destructors --
    // after the llrender statics ~LLTextureData uses -> SIGSEGV after "Goodbye!" and a
    // multi-GB core dump that looked like a hang on exit.
    LL_INFOS() << "Cleaning up media and textures" << LL_ENDL;

    // Media first: releasing media can create/touch textures (see the note before the
    // second gTextureList.shutdown() below). The SLPlugin processes were already told to
    // exit by LLPluginProcessParent::shutdown() at the top of cleanup(); drop each
    // impl's plugin source here. Not setAllMediaEnabled(false): that writes a saved
    // setting and stops parcel media through LLViewerMediaFocus after the UI is gone.
    if (LLViewerMedia::instanceExists())
    {
        for (LLViewerMediaImpl* impl : LLViewerMedia::getInstance()->getPriorityList())
        {
            if (impl)
            {
                impl->destroyMediaSource();
            }
        }
    }

    // The sky object holds sun/moon/cloud textures. Normally already released by
    // LLWorld::resetClass() in disconnectViewer(); repeat it here (idempotent, as LL's
    // shutdownGL() did) so a plain global never carries an object into static dtors.
    gSky.cleanup();

    // Wearables hold texture references (MUST precede the texture list, as in LL).
    if (LLWearableList::instanceExists())
    {
        LLWearableList::instance().cleanup();
    }

    LLAvatarIconLoader::deleteSingleton();
    LLManipTranslate::destroyGridTexture();

    gTextureList.shutdown();
    LLWorldMapView::cleanupTextures();    // empty; cleanupClass() ran above (LL parity)
    LLViewerTextureManager::cleanup();    // static default images + LLViewerMediaTexture::sMediaMap

    LL_INFOS() << "All textures and texture data are destroyed!" << LL_ENDL;

    LL_INFOS() << "Shutting down OpenGL" << LL_ENDL;

    // Shut down OpenGL
    if (gViewerWindow)
    {
        // Destroy window, and make sure we're not fullscreen
        // This may generate window reshape and activation events.
        // Therefore must do this before destroying the message system.
        delete gViewerWindow;
        gViewerWindow = NULL;
        LL_INFOS() << "ViewerWindow deleted" << LL_ENDL;
    }

    LLSplashScreen::show();
    LLSplashScreen::update(LLTrans::getString("ShuttingDown"));

    LL_INFOS() << "Cleaning up Keyboard & Joystick" << LL_ENDL;

    // viewer UI relies on keyboard so keep it aound until viewer UI isa gone
    delete gKeyboard;
    gKeyboard = NULL;

    if (LLViewerJoystick::instanceExists())
    {
        // Turn off Space Navigator and similar devices
        LLViewerJoystick::getInstance()->terminate();
    }

    LLGameControl::terminate();

    LL_INFOS() << "Shutting down message system" << LL_ENDL;
    end_messaging_system();

    // Non-LLCurl libcurl library
    mAppCoreHttp.cleanup();

    SUBSYSTEM_CLEANUP(LLFilePickerThread);
    SUBSYSTEM_CLEANUP(LLDirPickerThread);

    //MUST happen AFTER SUBSYSTEM_CLEANUP(LLCurl)
    delete sTextureCache;
    sTextureCache = NULL;
    if (sTextureFetch)
    {
        sTextureFetch->shutdown();
        sTextureFetch->waitOnPending();
        delete sTextureFetch;
        sTextureFetch = NULL;
    }
    delete sImageDecodeThread;
    sImageDecodeThread = NULL;
    delete mFastTimerLogThread;
    mFastTimerLogThread = NULL;
    delete sPurgeDiskCacheThread;
    sPurgeDiskCacheThread = NULL;
    delete mGeneralThreadPool;
    mGeneralThreadPool = NULL;

    if (LLFastTimerView::sAnalyzePerformance)
    {
        LL_INFOS() << "Analyzing performance" << LL_ENDL;

        std::string baseline_name = LLTrace::BlockTimer::sLogName + "_baseline.slp";
        std::string current_name  = LLTrace::BlockTimer::sLogName + ".slp";
        std::string report_name   = LLTrace::BlockTimer::sLogName + "_report.csv";

        LLFastTimerView::doAnalysis(
            gDirUtilp->getExpandedFilename(LL_PATH_LOGS, baseline_name),
            gDirUtilp->getExpandedFilename(LL_PATH_LOGS, current_name),
            gDirUtilp->getExpandedFilename(LL_PATH_LOGS, report_name));
    }

    SUBSYSTEM_CLEANUP(LLMetricPerformanceTesterBasic) ;

    LL_INFOS() << "Cleaning up Media and Textures" << LL_ENDL;

    //Note:
    //SUBSYSTEM_CLEANUP(LLViewerMedia) has to be put before gTextureList.shutdown()
    //because some new image might be generated during cleaning up media. --bao
    gTextureList.shutdown(); // shutdown again in case a callback added something
    LLUIImageList::getInstance()->cleanUp();

    SUBSYSTEM_CLEANUP(LLImage);
    SUBSYSTEM_CLEANUP(LLLFSThread);

    LL_INFOS() << "Misc Cleanup" << LL_ENDL;

    gSavedSettings.cleanup();
    LLUIColorTable::instance().clear();

    LLWatchdog::getInstance()->cleanup();

    LLViewerAssetStatsFF::cleanup();

    // If we're exiting to launch an URL, do that here so the screen
    // is at the right resolution before we launch IE.
    if (!gLaunchFileOnQuit.empty())
    {
        LL_INFOS() << "Launch file on quit." << LL_ENDL;
#if LL_WINDOWS
        // Indicate an application is starting.
        SetCursor(LoadCursor(NULL, IDC_WAIT));
#endif

        // HACK: Attempt to wait until the screen res. switch is complete.
        ms_sleep(1000);

        LLWeb::loadURLExternal( gLaunchFileOnQuit, false );
        LL_INFOS() << "File launched." << LL_ENDL;
    }
    // make sure nothing uses applyProxySettings by this point.
    LL_INFOS() << "Cleaning up LLProxy." << LL_ENDL;
    SUBSYSTEM_CLEANUP(LLProxy);
    LLCore::LLHttp::cleanup();

    ll_close_fail_log();

    LLError::LLCallStacks::cleanup();
    LL::GLTFSceneManager::deleteSingleton();
    LLEnvironment::deleteSingleton();
    LLSelectMgr::deleteSingleton();
    LLViewerStatsRecorder::deleteSingleton();
    LLViewerEventRecorder::deleteSingleton();
    LLWorld::deleteSingleton();
    LLVoiceClient::deleteSingleton();
    LLUI::deleteSingleton();

    // It's not at first obvious where, in this long sequence, a generic cleanup
    // call OUGHT to go. So let's say this: as we migrate cleanup from
    // explicit hand-placed calls into the generic mechanism, eventually
    // all cleanup will get subsumed into the generic call. So the calls you
    // still see above are calls that MUST happen before the generic cleanup
    // kicks in.

    // This calls every remaining LLSingleton's cleanupSingleton() and
    // deleteSingleton() methods.
    LLSingletonBase::deleteAll();

    LLSplashScreen::hide();

    LL_INFOS() << "Goodbye!" << LL_ENDL;

    removeDumpDir();

    // The user chose "Restart Slipstream" after a graphics device loss (#135).
    relaunchIfRequested();

    // return 0;
    return true;
}

void LLAppViewer::initGeneralThread()
{
    if (mGeneralThreadPool)
    {
        return;
    }

    mGeneralThreadPool = new LL::ThreadPool("General", 3);
    mGeneralThreadPool->start();
}

bool LLAppViewer::initThreads()
{
    static const bool enable_threads = true;

    LLImage::initClass(gSavedSettings.getBOOL("TextureNewByteRange"),gSavedSettings.getS32("TextureReverseByteRange"));

    LLLFSThread::initClass(enable_threads && true); // TODO: fix crashes associated with this shutdo

    //auto configure thread count
    LLSD threadCounts = gSavedSettings.getLLSD("ThreadPoolSizes");

    // get the number of concurrent threads that can run
    S32 cores = std::thread::hardware_concurrency();
#if LL_DARWIN
    cores /= 2;
#endif
    U32 max_cores = gSavedSettings.getU32("EmulateCoreCount");
    if (max_cores != 0)
    {
        cores = llmin(cores, (S32) max_cores);
    }

    // always use at least 2 threads for image decoding to prevent
    // a single texture blocking all other textures from decoding
    S32 image_decode_count = llclamp(cores - 6, 2, 16);
    if (LLThreadAffinity::active())
    {
        // Workers own their own domains: size decode to them, leaving room for
        // the staging / mesh-decode / general / fetch threads that share them.
        // 16 decoders oversubscribed the old single-domain pin 5:1.
        image_decode_count = llclamp((S32)LLThreadAffinity::workerCpuCount() / 4, 2, 8);
    }

    threadCounts["ImageDecode"] = image_decode_count;
    gSavedSettings.setLLSD("ThreadPoolSizes", threadCounts);

    // Image decoding
    LLAppViewer::sImageDecodeThread = new LLImageDecodeThread(enable_threads && true);
    LLAppViewer::sTextureCache = new LLTextureCache(enable_threads && true);
    LLAppViewer::sTextureFetch = new LLTextureFetch(LLAppViewer::getTextureCache(),
                                                    enable_threads && true,
                                                    app_metrics_qa_mode);

    // general task background thread (LLPerfStats, etc)
    LLAppViewer::instance()->initGeneralThread();

    LLAppViewer::sPurgeDiskCacheThread = new LLPurgeDiskCacheThread();

    if (LLTrace::BlockTimer::sLog || LLTrace::BlockTimer::sMetricLog)
    {
        LLTrace::BlockTimer::setLogLock(new LLMutex());
        mFastTimerLogThread = new LLFastTimerLogThread(LLTrace::BlockTimer::sLogName);
        mFastTimerLogThread->start();
    }

    // Mesh streaming and caching
    gMeshRepo.init();

    LLFilePickerThread::initClass();
    LLDirPickerThread::initClass();

    // *FIX: no error handling here!
    return true;
}

// Callback for all LL_ERROR calls
void errorCallback(LLError::ELevel level, const std::string &error_string)
{
    if (level == LLError::LEVEL_ERROR)
    {
#ifndef LL_RELEASE_FOR_DOWNLOAD
        std::string message = error_string +
            "\n\n\nThis is a developer-only notification!\nThis notification won't be present in Release for download build";
        OSMessageBox(message, LLTrans::getString("MBFatalError"), OSMB_OK);
#endif

        LLMemDiag::lastWords("LL_ERRS");   // #700 (once; the Vulkan fatal path already ran it with its call site)
        gDebugInfo["FatalMessage"] = error_string;
        // We're not already crashing -- we simply *intend* to crash. Since we
        // haven't actually trashed anything yet, we can afford to write the whole
        // static info file.
        LLAppViewer::instance()->writeDebugInfo();

        std::string error_marker_file = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ERROR_MARKER_FILE_NAME);
        if (!LLAPRFile::isExist(error_marker_file, NULL, LL_APR_RB))
        {
            // If marker doesn't exist, create a marker with llerror code for next launch
            // otherwise don't override existing file
            LLAppViewer::instance()->createErrorMarker(LAST_EXEC_LLERROR_CRASH);
        }
    }
}

// Callback for LLError::LLUserWarningMsg
void errorHandler(const std::string& title_string, const std::string& message_string, S32 code)
{
    // message is going to hang viewer, create marker first
    switch (code)
    {
    case LLError::LLUserWarningMsg::ERROR_OTHER:
        LLAppViewer::instance()->createErrorMarker(LAST_EXEC_OTHER_CRASH);
        break;
    case LLError::LLUserWarningMsg::ERROR_BAD_ALLOC:
        LLMemDiag::lastWords("out of memory (bad_alloc)");   // #700
        LLAppViewer::instance()->createErrorMarker(LAST_EXEC_BAD_ALLOC);
        // When system run out of memory and errorHandler gets called from a thread,
        // main thread might keep going while OSMessageBox freezes the caller.
        // Todo: handle it better, but for now disconnect to avoid making things worse
        gDisconnected = true;
        break;
    case LLError::LLUserWarningMsg::ERROR_MISSING_FILES:
        LLAppViewer::instance()->createErrorMarker(LAST_EXEC_MISSING_FILES);
        break;
    default:
        break;
    }
    if (!message_string.empty())
    {
        OSMessageBox(message_string, title_string.empty() ? LLTrans::getString("MBFatalError") : title_string, OSMB_OK);
    }
}

void LLAppViewer::initLoggingAndGetLastDuration()
{
    //
    // Set up logging defaults for the viewer
    //
    LLError::initForApplication( gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "")
                                ,gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "")
                                );
    LLError::addGenericRecorder(&errorCallback);
    //LLError::setTimeFunction(getRuntime);

    LLError::LLUserWarningMsg::setHandler(errorHandler);


    if (mSecondInstance)
    {
        if (mInstanceSlot > 1)
        {
            const std::string log_file = getLogFilePath();
            const std::string old_log_file = getOldLogFilePath();
            LLFile::remove(old_log_file, ENOENT);
            LLFile::rename(log_file, old_log_file, ENOENT);
            LLError::logToFile(log_file);
            LL_INFOS() << "Started logging to " << log_file << " (viewer instance " << mInstanceSlot << ")" << LL_ENDL;
        }
        // No free slot: initConfiguration() reports it and exits.
    }
    else
    {
        // Remove the last ".old" log file.
        std::string old_log_file = getOldLogFilePath();
        LLFile::remove(old_log_file);

        // Get name of the log file
        std::string log_file = getLogFilePath();
        /*
        * Before touching any log files, compute the duration of the last run
        * by comparing the ctime of the previous start marker file with the ctime
        * of the last log file.
        */
        std::string start_marker_file_name = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, START_MARKER_FILE_NAME);
        llstat start_marker_stat;
        llstat log_file_stat;
        std::ostringstream duration_log_stream; // can't log yet, so save any message for when we can below
        int start_stat_result = LLFile::stat(start_marker_file_name, &start_marker_stat);
        int log_stat_result = LLFile::stat(log_file, &log_file_stat);
        if (0 == start_stat_result && 0 == log_stat_result)
        {
            int elapsed_seconds = (int)(log_file_stat.st_ctime - start_marker_stat.st_ctime);
            // only report a last run time if the last viewer was the same version
            // because this stat will be counted against this version
            if (markerIsSameVersion(start_marker_file_name))
            {
                gLastExecDuration = elapsed_seconds;
            }
            else
            {
                duration_log_stream << "start marker from some other version; duration is not reported";
                gLastExecDuration = -1;
            }
        }
        else
        {
            // at least one of the LLFile::stat calls failed, so we can't compute the run time
            duration_log_stream << "duration stat failure; start: " << start_stat_result << " log: " << log_stat_result;
            gLastExecDuration = -1; // unknown
        }
        std::string duration_log_msg(duration_log_stream.str());

        // Create a new start marker file for comparison with log file time for the next run
        LLAPRFile start_marker_file;
        start_marker_file.open(start_marker_file_name, LL_APR_WB);
        if (start_marker_file.getFileHandle())
        {
            recordMarkerVersion(start_marker_file);
            start_marker_file.close();
        }

        // Rename current log file to ".old"
        LLFile::rename(log_file, old_log_file);

        // Set the log file to SecondLife.log
        LLError::logToFile(log_file);
        LL_INFOS() << "Started logging to " << log_file << LL_ENDL;
        if (!duration_log_msg.empty())
        {
            LL_WARNS("MarkerFile") << duration_log_msg << LL_ENDL;
        }

        std::string user_data_path_cef_log = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "cef.log");
        if (gDirUtilp->fileExists(user_data_path_cef_log))
        {
            std::string user_data_path_cef_old = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "cef.old");
            LLFile::remove(user_data_path_cef_old, ENOENT);
            LLFile::rename(user_data_path_cef_log, user_data_path_cef_old);
        }
    }
}

bool LLAppViewer::loadSettingsFromDirectory(const std::string& location_key,
                        bool set_defaults)
{
    if (!mSettingsLocationList)
    {
        LL_ERRS() << "Invalid settings location list" << LL_ENDL;
    }

    for (const SettingsGroup& group : mSettingsLocationList->groups)
    {
        // skip settings groups that aren't the one we requested
        if (group.name() != location_key) continue;

        ELLPath path_index = (ELLPath)group.path_index();
        if(path_index <= LL_PATH_NONE || path_index >= LL_PATH_LAST)
        {
            LL_ERRS() << "Out of range path index in app_settings/settings_files.xml" << LL_ENDL;
            return false;
        }

        for (const SettingsFile& file : group.files)
        {
            LL_INFOS("Settings") << "Attempting to load settings for the group " << file.name()
                << " - from location " << location_key << LL_ENDL;

            auto settings_group = LLControlGroup::getInstance(file.name);
            if(!settings_group)
            {
                LL_WARNS("Settings") << "No matching settings group for name " << file.name() << LL_ENDL;
                continue;
            }

            std::string full_settings_path;

            if (file.file_name_setting.isProvided()
                && gSavedSettings.controlExists(file.file_name_setting()))
            {
                // try to find filename stored in file_name_setting control
                full_settings_path = gSavedSettings.getString(file.file_name_setting());
                if (full_settings_path.empty())
                {
                    continue;
                }
                else if (!gDirUtilp->fileExists(full_settings_path))
                {
                    // search in default path
                    full_settings_path = gDirUtilp->getExpandedFilename((ELLPath)path_index, full_settings_path);
                }
            }
            else
            {
                // by default, use specified file name
                full_settings_path = gDirUtilp->getExpandedFilename((ELLPath)path_index, file.file_name());
            }

            if(settings_group->loadFromFile(full_settings_path, set_defaults, file.persistent))
            {   // success!
                LL_INFOS("Settings") << "Loaded settings file " << full_settings_path << LL_ENDL;
            }
            else
            {   // failed to load
                if(file.required)
                {
                    LLError::LLUserWarningMsg::showMissingFiles();
                    LL_ERRS() << "Error: Cannot load required settings file from: " << full_settings_path << LL_ENDL;
                    return false;
                }
                else
                {
                    // only complain if we actually have a filename at this point
                    if (!full_settings_path.empty())
                    {
                        LL_INFOS("Settings") << "Cannot load " << full_settings_path << " - No settings found." << LL_ENDL;
                    }
                }
            }
        }
    }

    return true;
}

std::string LLAppViewer::getSettingsFilename(const std::string& location_key,
                                             const std::string& file)
{
    for (const SettingsGroup& group : mSettingsLocationList->groups)
    {
        if (group.name() == location_key)
        {
            for (const SettingsFile& settings_file : group.files)
            {
                if (settings_file.name() == file)
                {
                    return settings_file.file_name;
                }
            }
        }
    }

    return std::string();
}

void LLAppViewer::loadColorSettings()
{
    LLUIColorTable::instance().loadFromSettings();
}

namespace
{
    void handleCommandLineError(LLControlGroupCLP& clp)
    {
        LL_WARNS() << "Error parsing command line options. Command Line options ignored."  << LL_ENDL;

        LL_INFOS() << "Command line usage:\n" << clp << LL_ENDL;

        OSMessageBox(STRINGIZE(LLTrans::getString("MBCmdLineError") << clp.getErrorMessage()),
                     LLStringUtil::null,
                     OSMB_OK);
    }
} // anonymous namespace

// Set a named control temporarily for this session, as when set via the command line --set option.
// Name can be specified as "<control_group>.<control_name>", with default group being Global.
bool tempSetControl(const std::string& name, const std::string& value)
{
    std::string name_part;
    std::string group_part;
    LLControlVariable* control = NULL;

    // Name can be further split into ControlGroup.Name, with the default control group being Global
    size_t pos = name.find('.');
    if (pos != std::string::npos)
    {
        group_part = name.substr(0, pos);
        name_part = name.substr(pos+1);
        LL_INFOS() << "Setting " << group_part << "." << name_part << " to " << value << LL_ENDL;
        auto g = LLControlGroup::getInstance(group_part);
        if (g) control = g->getControl(name_part);
    }
    else
    {
        LL_INFOS() << "Setting Global." << name << " to " << value << LL_ENDL;
        control = gSavedSettings.getControl(name);
    }

    if (control)
    {
        control->setValue(value, false);
        return true;
    }
    return false;
}

bool LLAppViewer::initConfiguration()
{
    // Load settings files list
    std::string settings_file_list = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "settings_files.xml");
    LLXMLNodePtr root;
    bool success = LLXMLNode::parseFile(settings_file_list, root, NULL);
    if (!success)
    {
        LL_WARNS() << "Cannot load default configuration file " << settings_file_list << LL_ENDL;
        LLError::LLUserWarningMsg::showMissingFiles();
        if (gDirUtilp->fileExists(settings_file_list))
        {
            LL_ERRS() << "Cannot load default configuration file settings_files.xml. "
                << "Please reinstall viewer from https://secondlife.com/support/downloads/ "
                << "and contact https://support.secondlife.com if issue persists after reinstall."
                << LL_ENDL;
        }
        else
        {
            LL_ERRS() << "Default configuration file settings_files.xml not found. "
                << "Please reinstall viewer from https://secondlife.com/support/downloads/ "
                << "and contact https://support.secondlife.com if issue persists after reinstall."
                << LL_ENDL;
        }
    }

    mSettingsLocationList = new SettingsFiles();

    LLXUIParser parser;
    parser.readXUI(root, *mSettingsLocationList, settings_file_list);

    if (!mSettingsLocationList->validateBlock())
    {
        LLError::LLUserWarningMsg::showMissingFiles();
        LL_ERRS() << "Invalid settings file list " << settings_file_list << LL_ENDL;
    }

    // The settings and command line parsing have a fragile
    // order-of-operation:
    // - load defaults from app_settings
    // - set procedural settings values
    // - read command line settings
    // - selectively apply settings needed to load user settings.
    // - load overrides from user_settings
    // - apply command line settings (to override the overrides)
    // - load per account settings (happens in llstartup

    // - load defaults
    bool set_defaults = true;
    if (!loadSettingsFromDirectory("Default", set_defaults))
    {
        OSMessageBox(
            "Unable to load default settings file. The installation may be corrupted.",
            LLStringUtil::null,OSMB_OK);
        LLAppViewer::instance()->createErrorMarker(LAST_EXEC_MISSING_FILES);
        return false;
    }

    initStrings(); // setup paths for LLTrans based on settings files only
    // - set procedural settings
    // Note: can't use LL_PATH_PER_SL_ACCOUNT for any of these since we haven't logged in yet
    gSavedSettings.setString("ClientSettingsFile",
        gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, getSettingsFilename("Default", "Global")));

#ifndef LL_RELEASE_FOR_DOWNLOAD
    // provide developer build only overrides for these control variables that are not
    // persisted to settings.xml
    gSavedSettings.setBOOL("QAMode", true);
    gSavedSettings.setS32("WatchdogEnabled", 0);
#endif

    // These are warnings that appear on the first experience of that condition.
    // They are already set in the settings_default.xml file, but still need to be added to LLFirstUse
    // for disable/reset ability
//  LLFirstUse::addConfigVariable("FirstBalanceIncrease");
//  LLFirstUse::addConfigVariable("FirstBalanceDecrease");
//  LLFirstUse::addConfigVariable("FirstSit");
//  LLFirstUse::addConfigVariable("FirstMap");
//  LLFirstUse::addConfigVariable("FirstGoTo");
//  LLFirstUse::addConfigVariable("FirstBuild");
//  LLFirstUse::addConfigVariable("FirstLeftClickNoHit");
//  LLFirstUse::addConfigVariable("FirstTeleport");
//  LLFirstUse::addConfigVariable("FirstOverrideKeys");
//  LLFirstUse::addConfigVariable("FirstAttach");
//  LLFirstUse::addConfigVariable("FirstAppearance");
//  LLFirstUse::addConfigVariable("FirstInventory");
//  LLFirstUse::addConfigVariable("FirstSandbox");
//  LLFirstUse::addConfigVariable("FirstFlexible");
//  LLFirstUse::addConfigVariable("FirstDebugMenus");
//  LLFirstUse::addConfigVariable("FirstSculptedPrim");
//  LLFirstUse::addConfigVariable("FirstVoice");
//  LLFirstUse::addConfigVariable("FirstMedia");

    // - read command line settings.
    LLControlGroupCLP clp;
    std::string cmd_line_config = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS,
                                                          "cmd_line.xml");

    clp.configure(cmd_line_config, &gSavedSettings);

    if (!initParseCommandLine(clp))
    {
        handleCommandLineError(clp);
        return false;
    }

    // - selectively apply settings

    // If the user has specified a alternate settings file name.
    // Load it now before loading the user_settings/settings.xml
    if (clp.hasOption("settings"))
    {
        std::string user_settings_filename =
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS,
                                           clp.getOption("settings")[0]);
        gSavedSettings.setString("ClientSettingsFile", user_settings_filename);
        LL_INFOS("Settings")    << "Using command line specified settings filename: "
            << user_settings_filename << LL_ENDL;
    }

    // - load overrides from user_settings
    loadSettingsFromDirectory("User");

    // #126: values saved under the pre-1.2 Alchemy* names carry over to the Slipstream*
    // names. Right after the user file loads, before anything reads them and before the
    // command line is applied (so --set still wins).
    SLSettingsMigration::migrateSettingsFile(gSavedSettings, gSavedSettings.getString("ClientSettingsFile"));
    SLSettingsMigration::migrateNearbyChatOverlay(gSavedSettings, gSavedSettings.getString("ClientSettingsFile"));
    SLSettingsMigration::migrateGMDXChatLog(gSavedSettings);   // <GMDX>
    SLSettingsMigration::migrateGMDXMouselookHud(gSavedSettings);   // <GMDX>
    SLSettingsMigration::migrateChatNotifications(gSavedSettings, gSavedSettings.getString("ClientSettingsFile"));

    if (gSavedSettings.getBOOL("FirstRunThisInstall"))
    {
        // Set firstrun flag to indicate that some further init actiona should be taken
        // like determining screen DPI value and so on
        mIsFirstRun = true;

        gSavedSettings.setBOOL("FirstRunThisInstall", false);
    }

    if (clp.hasOption("sessionsettings"))
    {
        std::string session_settings_filename = clp.getOption("sessionsettings")[0];
        gSavedSettings.setString("SessionSettingsFile", session_settings_filename);
        LL_INFOS("Settings")    << "Using session settings filename: "
            << session_settings_filename << LL_ENDL;
    }
    loadSettingsFromDirectory("Session");

    if (clp.hasOption("usersessionsettings"))
    {
        std::string user_session_settings_filename = clp.getOption("usersessionsettings")[0];
        gSavedSettings.setString("UserSessionSettingsFile", user_session_settings_filename);
        LL_INFOS("Settings") << "Using user session settings filename: "
            << user_session_settings_filename << LL_ENDL;

    }
    loadSettingsFromDirectory("UserSession");

    // - apply command line settings
    if (!clp.notify())
    {
        handleCommandLineError(clp);
        return false;
    }

    // Photo Studio (#444): its settings renamed from the Alchemy names.
    SLPhotoStudioSettings::migrateRenamedSettings();

    migrateVKUpscaleMode();
    if (LLControlVariable* old_upscale = gSavedSettings.getControl("RenderVKUpscaleMode"))
        old_upscale->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { migrateVKUpscaleMode(); });
    migrateDlssRayReconstruction();   // (#962; after the line above: it may have set RenderVKUpscaler)

    // Register the core crash option as soon as we can
    // if we want gdb post-mortem on cores we need to be up and running
    // ASAP or we might miss init issue etc.
    if (gSavedSettings.getBOOL("DisableCrashLogger"))
    {
        LL_WARNS() << "Crashes will be handled by system, stack trace logs and crash logger are both disabled" << LL_ENDL;
        disableCrashlogger();
    }

    gNonInteractive = gSavedSettings.getBOOL("NonInteractive");
    // Handle initialization from settings.
    // Start up the debugging console before handling other options.
    if (gSavedSettings.getBOOL("ShowConsoleWindow") && !gNonInteractive)
    {
        initConsole();
    }

    if (clp.hasOption("help"))
    {
        std::ostringstream msg;
        msg << LLTrans::getString("MBCmdLineUsg") << "\n" << clp;
        LL_INFOS()  << msg.str() << LL_ENDL;

        OSMessageBox(
            msg.str(),
            LLStringUtil::null,
            OSMB_OK);

        return false;
    }

    if (clp.hasOption("set"))
    {
        const LLCommandLineParser::token_vector_t& set_values = clp.getOption("set");
        if (0x1 & set_values.size())
        {
            LL_WARNS() << "Invalid '--set' parameter count." << LL_ENDL;
        }
        else
        {
            LLCommandLineParser::token_vector_t::const_iterator itr = set_values.begin();
            for (; itr != set_values.end(); ++itr)
            {
                const std::string& name = *itr;
                const std::string& value = *(++itr);
                if (!tempSetControl(name,value))
                {
                    LL_WARNS() << "Failed --set " << name << ": setting name unknown." << LL_ENDL;
                }
            }
        }
    }

    if (clp.hasOption("logevents"))
    {
        LLViewerEventRecorder::instance().setEventLoggingOn();
    }

    std::string CmdLineChannel(gSavedSettings.getString("CmdLineChannel"));
    if (!CmdLineChannel.empty())
    {
        LLVersionInfo::instance().resetChannel(CmdLineChannel);
    }

    // If we have specified crash on startup, set the global so we'll trigger the crash at the right time
    gCrashOnStartup = gSavedSettings.getBOOL("CrashOnStartup");

    if (gSavedSettings.getBOOL("LogPerformance"))
    {
        LLTrace::BlockTimer::sLog = true;
        LLTrace::BlockTimer::sLogName = std::string("performance");
    }

    std::string test_name(gSavedSettings.getString("LogMetrics"));
    if (!test_name.empty())
    {
        LLTrace::BlockTimer::sMetricLog = true;
        // '--logmetrics' is specified with a named test metric argument so the data gathering is done only on that test
        // In the absence of argument, every metric would be gathered (makes for a rather slow run and hard to decipher report...)
        LL_INFOS() << "'--logmetrics' argument : " << test_name << LL_ENDL;
        LLTrace::BlockTimer::sLogName = test_name;
    }

    if (clp.hasOption("graphicslevel"))
    {
        // User explicitly requested --graphicslevel on the command line. We
        // expect this switch has already set RenderQualityPerformance. Check
        // that value for validity later.
        // Capture the requested value separately from the settings variable
        // because, if this is the first run, LLViewerWindow's constructor
        // will call LLFeatureManager::applyRecommendedSettings(), which
        // overwrites this settings variable!
        mForceGraphicsLevel = gSavedSettings.getU32("RenderQualityPerformance");
    }

    LLFastTimerView::sAnalyzePerformance = gSavedSettings.getBOOL("AnalyzePerformance");
    gAgentPilot.setReplaySession(gSavedSettings.getBOOL("ReplaySession"));

    if (gSavedSettings.getBOOL("DebugSession"))
    {
        gDebugSession = true;
        gDebugGL = true;

        ll_init_fail_log(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "test_failures.log"));
    }

    if (gSavedSettings.getBOOL("RenderDebugGLSession"))
    {
        gDebugGLSession = true;
        gDebugGL = true;
        // gDebugGL can cause excessive logging
        // so it's limited to a single session
        gSavedSettings.setBOOL("RenderDebugGLSession", false);
    }

    LLControlVariable* skinfolder = gSavedSettings.getControl("SkinCurrent");
    // A saved skin that this build no longer ships (the Alchemy, Heretic and Ionic skins
    // were removed: third-party artwork we may not redistribute) falls back to the
    // default skin instead of silently loading LL's bare base skin.
    if (skinfolder && LLStringUtil::null != skinfolder->getValue().asString()
        && !LLFile::isdir(gDirUtilp->add(gDirUtilp->getSkinBaseDir(), skinfolder->getValue().asString()))
        && !LLFile::isdir(gDirUtilp->add(gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "skins"),
                                         skinfolder->getValue().asString())))   // user-installed skins
    {
        LL_WARNS("AppInit") << "Skin '" << skinfolder->getValue().asString()
                            << "' is not installed; using the default skin" << LL_ENDL;
        skinfolder->resetToDefault(true);
    }
    if (skinfolder && LLStringUtil::null != skinfolder->getValue().asString())
    {
        // Examining "Language" may not suffice -- see LLUI::getLanguage()
        // logic. Unfortunately LLUI::getLanguage() doesn't yet do us much
        // good because we haven't yet called LLUI::initClass().
        gDirUtilp->setSkinFolder(skinfolder->getValue().asString(),
                                 gSavedSettings.getString("Language"));
        std::string skin_settings_file = gDirUtilp->findSkinnedFilename(LLDir::SKINBASE, "skin_settings.xml");
        (void)gSkinSettings.loadFromFile(skin_settings_file, true, false);
    }

    if (gNonInteractive)
    {
        tempSetControl("AllowMultipleViewers", "true");
        tempSetControl("SLURLPassToOtherInstance", "false");
        tempSetControl("RenderWater", "false");
        tempSetControl("FlyingAtExit", "false");
        tempSetControl("WindowWidth", "1024");
        tempSetControl("WindowHeight", "200");
        LLError::setEnabledLogTypesMask(0);
        llassert_always(!gSavedSettings.getBOOL("SLURLPassToOtherInstance"));
    }

    // Handle slurl use. NOTE: Don't let SL-55321 reappear.
    // This initial-SLURL logic, up through the call to
    // sendURLToOtherInstance(), must precede LLSplashScreen::show() --
    // because if sendURLToOtherInstance() succeeds, we take a fast exit,
    // SKIPPING the splash screen and everything else.

    // *FIX: This init code should be made more robust to prevent
    // the issue SL-55321 from returning. One thought is to allow
    // only select options to be set from command line when a slurl
    // is specified. More work on the settings system is needed to
    // achieve this. For now...

    // *NOTE:Mani The command line parser parses tokens and is
    // setup to bail after parsing the '--url' option or the
    // first option specified without a '--option' flag (or
    // any other option that uses the 'last_option' setting -
    // see LLControlGroupCLP::configure())

    // What can happen is that someone can use IE (or potentially
    // other browsers) and do the rough equivalent of command
    // injection and steal passwords. Phoenix. SL-55321

    std::string starting_location;

    std::string cmd_line_login_location(gSavedSettings.getString("CmdLineLoginLocation"));
    if (!cmd_line_login_location.empty())
    {
        starting_location = cmd_line_login_location;
    }
    else
    {
        std::string default_login_location(gSavedSettings.getString("DefaultLoginLocation"));
        if (!default_login_location.empty())
        {
            starting_location = default_login_location;
        }
    }

    LLSLURL start_slurl;
    if (!starting_location.empty())
    {
        start_slurl = starting_location;
        LLStartUp::setStartSLURL(start_slurl);
        if(start_slurl.getType() == LLSLURL::LOCATION)
        {
            LLGridManager::getInstance()->setGridChoice(start_slurl.getGrid());
        }
    }

    // NextLoginLocation is set as a side effect of LLStartUp::setStartSLURL()
    std::string nextLoginLocation = gSavedSettings.getString( "NextLoginLocation" );
    if (!nextLoginLocation.empty())
    {
        LL_DEBUGS("AppInit")<<"set start from NextLoginLocation: "<<nextLoginLocation<<LL_ENDL;
        LLStartUp::setStartSLURL(LLSLURL(nextLoginLocation));
    }
    else if ((clp.hasOption("login") || clp.hasOption("autologin"))
             && gSavedSettings.getString("CmdLineLoginLocation").empty())
    {
        // If automatic login from command line with --login switch
        // init StartSLURL location.
        std::string start_slurl_setting = gSavedSettings.getString("LoginLocation");
        LL_DEBUGS("AppInit") << "start slurl setting '" << start_slurl_setting << "'" << LL_ENDL;
        LLStartUp::setStartSLURL(LLSLURL(start_slurl_setting));
    }
    else
    {
        // the login location will be set by the login panel (see LLPanelLogin)
    }

    //RN: if we received a URL, hand it off to the existing instance.
    // don't call anotherInstanceRunning() when doing URL handoff, as
    // it relies on checking a marker file which will not work when running
    // out of different directories

    if (start_slurl.isValid() &&
        (gSavedSettings.getBOOL("SLURLPassToOtherInstance")))
    {
        if (sendURLToOtherInstance(start_slurl.getSLURLString()))
        {
            // successfully handed off URL to existing instance, exit
            return false;
        }
    }

    // Display splash screen.  Must be after above check for previous
    // crash as this dialog is always frontmost.
    std::string splash_msg;
    LLStringUtil::format_map_t args;
    args["[APP_NAME]"] = getSecondLifeTitle();
    splash_msg = LLTrans::getString("StartupLoading", args);
    LLSplashScreen::show();
    LLSplashScreen::update(splash_msg);

    //LLVolumeMgr::initClass();
    LLVolumeMgr* volume_manager = new LLVolumeMgr();
    volume_manager->useMutex(); // LLApp and LLMutex magic must be manually enabled
    LLPrimitive::setVolumeManager(volume_manager);

    // Note: this is where we used to initialize gFeatureManagerp.

    gStartTime = totalTime();

    //
    // Set the name of the window
    //
    if (LLVersionInfo::instance().getViewerMaturity() != LLVersionInfo::RELEASE_VIEWER)
    {
        gWindowTitle = LLVersionInfo::instance().getChannelAndVersion();
    }
    else
    {
        gWindowTitle = LLTrans::getString("APP_NAME");
    }
#if LL_DEBUG
    gWindowTitle += std::string(" [DEBUG]");
#elif defined(LL_RELEASE_WITH_DEBUG_INFO) || defined(SHOW_ASSERT)
    gWindowTitle += std::string(" [ASSERT]");
#endif
    if (!gArgs.empty())
    {
        gWindowTitle += std::string(" ") + gArgs;
    }
    LLStringUtil::truncate(gWindowTitle, 255);

    //
    // Check for another instance of the app running
    // This happens AFTER LLSplashScreen::show(). That may or may not be
    // important.
    //
    if (mSecondInstance && !gSavedSettings.getBOOL("AllowMultipleViewers"))
    {
        OSMessageBox(
            LLTrans::getString("MBAlreadyRunning"),
            LLStringUtil::null,
            OSMB_OK);
        return false;
    }
    if (mSecondInstance && mInstanceSlot == 0)
    {
        LLSD args;
        args["COUNT"] = MAX_INSTANCE_SLOTS;
        OSMessageBox(LLTrans::getString("MBTooManyViewers", args), LLStringUtil::null, OSMB_OK);
        return false;
    }
#if 0
    if (mSecondInstance)
    {
        // This is the second instance of SL. Mute voice,
        // but make sure the setting is *not* persisted.
        // Also see LLVoiceClient::voiceEnabled()
        LLControlVariable* enable_voice = gSavedSettings.getControl("EnableVoiceChat");
        if (enable_voice)
        {
            const bool DO_NOT_PERSIST = false;
            enable_voice->setValue(LLSD(false), DO_NOT_PERSIST);
        }
    }
#endif

    gLastRunVersion = gSavedSettings.getString("LastRunVersion");

    loadColorSettings();

    if (mSecondInstance)
    {
        // The global settings files are shared with the first viewer: from here on save only what
        // changes in this session, merged onto the files as they are then. The window is per viewer.
        gSavedSettings.enableMergeSave(gSavedSettings.getString("ClientSettingsFile"),
                                       { "WindowX", "WindowY", "WindowWidth", "WindowHeight", "WindowMaximized" });
        gWarningSettings.enableMergeSave(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS,
                                                                        getSettingsFilename("Default", "Warnings")), {});
        LLUIColorTable::instance().enableMergeSave();
    }

    // Let anyone else who cares know that we've populated our settings
    // variables.
    for (const auto& key : LLControlGroup::key_snapshot())
    {
        // For each named instance of LLControlGroup, send an event saying
        // we've initialized an LLControlGroup instance by that name.
        LLEventPumps::instance().obtain("LLControlGroup").post(LLSDMap("init", key));
    }

    LLError::LLUserWarningMsg::setOutOfMemoryStrings(LLTrans::getString("MBOutOfMemoryTitle"), LLTrans::getString("MBOutOfMemoryErr"));

// [RLVa:KB] - Patch: RLVa-2.1.0
    if (LLControlVariable* pControl = gSavedSettings.getControl(RlvSettingNames::Main))
    {
        if ( (pControl->getValue().asBoolean()) && (pControl->hasUnsavedValue()) )
        {
            pControl->resetToDefault();
            pControl->setValue(false);

            std::ostringstream msg;
            msg << LLTrans::getString("RLVaToggleMessageLogin", LLSD().with("[STATE]", LLTrans::getString("RLVaToggleDisabled")));
            OSMessageBox(msg.str(), LLStringUtil::null, OSMB_OK);
        }
    }
// [/RLVa:KB]

    return true; // Config was successful.
}

// The following logic is replicated in initConfiguration() (to be able to get
// some initial strings before we've finished initializing enough to know the
// current language) and also in init() (to initialize for real). Somehow it
// keeps growing, necessitating a method all its own.
void LLAppViewer::initStrings()
{
    std::string strings_file = "strings.xml";
    std::string strings_path_full = gDirUtilp->findSkinnedFilenameBaseLang(LLDir::XUI, strings_file);
    if (strings_path_full.empty() || !LLFile::isfile(strings_path_full))
    {
        std::string crash_reason;
        if (strings_path_full.empty())
        {
            crash_reason = "The file '" + strings_file + "' is not found";
        }
        else
        {
            llstat st;
            int rc = LLFile::stat(strings_path_full, &st);
            if (rc != 0)
            {
                crash_reason = "The file '" + strings_path_full + "' failed to get status. Error code: " + std::to_string(rc);
            }
            else if (S_ISDIR(st.st_mode))
            {
                crash_reason = "The filename '" + strings_path_full + "' is a directory name";
            }
            else
            {
                crash_reason = "The filename '" + strings_path_full + "' doesn't seem to be a regular file name";
            }
        }

        // initial check to make sure files are there failed
        gDirUtilp->dumpCurrentDirectories(LLError::LEVEL_WARN);
        LLError::LLUserWarningMsg::showMissingFiles();
        LL_ERRS() << "Viewer failed to open some of localization and UI files."
            << " " << crash_reason << "." << LL_ENDL;
    }
    LLTransUtil::parseStrings(strings_file, default_trans_args);
    LLTransUtil::parseLanguageStrings("language_settings.xml");

    // parseStrings() sets up the LLTrans substitution table. Add this one item.
    LLTrans::setDefaultArg("[sourceid]", gSavedSettings.getString("sourceid"));

    // Now that we've set "[sourceid]", have to go back through
    // default_trans_args and reinitialize all those other keys because some
    // of them, in turn, reference "[sourceid]".
    for (const std::string& key : default_trans_args)
    {
        std::string brackets(key), nobrackets(key);
        // Invalid to inspect key[0] if key is empty(). But then, the entire
        // body of this loop is pointless if key is empty().
        if (key.empty())
            continue;

        if (key[0] != '[')
        {
            // key was passed without brackets. That means that 'nobrackets'
            // is correct but 'brackets' is not.
            brackets = STRINGIZE('[' << brackets << ']');
        }
        else
        {
            // key was passed with brackets. That means that 'brackets' is
            // correct but 'nobrackets' is not. Erase the left bracket.
            nobrackets.erase(0, 1);
            std::string::size_type length(nobrackets.length());
            if (length && nobrackets[length - 1] == ']')
            {
                nobrackets.erase(length - 1);
            }
        }
        // Calling LLTrans::getString() is what embeds the other default
        // translation strings into this one.
        LLTrans::setDefaultArg(brackets, LLTrans::getString(nobrackets));
    }
}

bool LLAppViewer::meetsRequirementsForMaximizedStart()
{
    bool maximizedOk = (gSysMemory.getPhysicalMemoryKB() >= U32Gigabytes(1));

    return maximizedOk;
}

// virtual
void LLAppViewer::sendOutOfDiskSpaceNotification()
{
    LL_WARNS() << "Out of disk space notification requested" << LL_ENDL;
    LLNotificationsUtil::add("OutOfDiskSpace");
}

// UI DRAW PROBE (RenderVKSectionLog): per-panel draw time for the first two levels of the
// UI tree (LLView::sDrawProbe), aggregated here and logged as the 25 most expensive every
// ~5 s -- its own table, so hundreds of panel names never crowd the section log's slots.
namespace
{
struct VkUiProbe
{
    std::unordered_map<std::string, double> ms;
    uint32_t frames = 0;
    double next_log = 0.0;
};
VkUiProbe& vkUiProbe() { static VkUiProbe p; return p; }
void vkUiProbeHook(const LLView* v, double ms, S32 depth)
{
    // depth:parent/name -- nested entries include their children's time.
    std::string key = std::to_string(depth) + ":" + (v->getParent() ? v->getParent()->getName() + "/" : std::string()) + v->getName();
    vkUiProbe().ms[key] += ms;
}
void vkUiProbeFrame()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKSectionLog", false);
    VkUiProbe& p = vkUiProbe();
    if (!s_on)
    {
        LLView::sDrawProbe = nullptr;
        p.ms.clear(); p.frames = 0;
        return;
    }
    LLView::sDrawProbe = vkUiProbeHook;
    ++p.frames;
    const double now = LLTimer::getTotalSeconds();
    if (now < p.next_log) return;
    p.next_log = now + 5.0;
    if (p.frames > 1)
    {
        std::vector<std::pair<double, std::string>> v;
        for (const auto& kv : p.ms) v.emplace_back(kv.second / p.frames, kv.first);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::ostringstream os;
        os << "UI draw ms/frame (top views):";
        for (size_t i = 0; i < v.size() && i < 25; ++i)
            if (v[i].first >= 0.01) os << " " << v[i].second << "=" << std::fixed << std::setprecision(2) << v[i].first;
        LL_INFOS("VKUi") << os.str() << LL_ENDL;
        LLFloaterView::RetainedStats& rs = LLFloaterView::sRetainedStats;
        const double fr = (double)p.frames;
        LL_INFOS("VKUi") << "retained floaters/frame: replay " << std::fixed << std::setprecision(2) << rs.replay / fr
            << " record " << rs.capture / fr << " live " << rs.live / fr << " failed " << rs.failed / fr
            << " | why (per s): hover " << rs.why[0] / 5.0 << " focus " << rs.why[1] / 5.0 << " capture " << rs.why[2] / 5.0
            << " changed " << rs.why[3] / 5.0 << " moved " << rs.why[4] / 5.0 << " refresh " << rs.why[5] / 5.0
            << " first " << rs.why[6] / 5.0 << " fail " << rs.why[7] / 5.0
            << " | held " << std::setprecision(0) << (rs.bytes / fr) / 1024.0 << " KB"
            << " | continuous enters " << rs.cont_enter << (rs.cont_enter ? " (last: " + rs.cont_last + ")" : std::string())
            << " focus-live fallbacks " << rs.focus_fallback
            << (rs.focus_fallback ? " (last: " + rs.focus_fallback_last + ")" : std::string()) << LL_ENDL;
    }
    LLFloaterView::sRetainedStats = LLFloaterView::RetainedStats();
    p.ms.clear(); p.frames = 0;
}
}

// LOG VOLUME (SlipstreamVerboseLogging). By default the log keeps what helps fix bugs:
// startup / system info, warnings, errors, crashes, and the diagnostics a user turns on
// explicitly (section log, GPU timers, the *Diag settings). These tags are routine
// renderer / ingest telemetry -- periodic summaries and per-object traces that buried
// the useful lines (5 MB in 6 minutes): their INFO is dropped unless verbose logging is
// on; their warnings and errors always go through. LL's octree insert noise is
// errors-only. Applied at startup and live when the setting changes.
static const char* const kVerboseInfoTags[] = {
    "VKTex", "VKTexUpload", "VKRT", "VKFunnel", "VKMem", "VKFlexi", "VKIngest", "VKHuge", "VKMirror",
    "VKAnim", "VKAnimCmp", "VKAutoMask", "VKAvLOD", "VKTouch", "VKTexAnim", "VKSculpt", "VKSkinPal", "VKShift", "VKPick", "VKRTFLICKER", "VKGodRays", "VKGLTF",
    "CleanTerrain", "CleanParticle", "CleanWater", "CleanTex", "CleanSurround", "CleanSkin", "CleanSel",
    "CleanMeshAsset", "CleanMeshGC", "CleanMeshDecode", "CleanBig", "CleanBigW",
    "Clean29Xform", "Clean29Ingest", "Clean29Prov",
    "HUDdetail", "AssetStorage", "LLMediaDataClient", "CoProcMgr", "CoprocedureManager", "LLCoros",
    "FSRadar", "ContactSets",
};
static const char* const kNoiseWarnTags[] = { "OctreeErrors" };

void slipstreamApplyLogVerbosity()
{
    const bool verbose = gSavedSettings.getBOOL("SlipstreamVerboseLogging");
    for (const char* t : kVerboseInfoTags)
        LLError::setTagLevel(t, verbose ? LLError::LEVEL_INFO : LLError::LEVEL_WARN);
    for (const char* t : kNoiseWarnTags)
        LLError::setTagLevel(t, verbose ? LLError::LEVEL_WARN : LLError::LEVEL_ERROR);
    LL_INFOS("AppInit") << "Verbose logging " << (verbose ? "ON" : "off (SlipstreamVerboseLogging)") << LL_ENDL;
}

// ---- GRAPHICS DEVICE LOST (#135) --------------------------------------------------
// vkgfx detects the loss at every point it can surface (vk_common.h: submits on both
// queues, fence / timeline waits, wait-idle, query results, acquire / present, VK_CHECK)
// and calls vk_device_lost_handler ONCE, on whichever thread saw it. The handler only
// writes diagnostics (one delimited block in the log + logs/slipstream_gpu_lost.txt,
// which also marks the exit for the next launch); vkgfx is already refusing GPU work.
// The main loop then calls onGraphicsDeviceLost(): message box (Restart / Quit), logout,
// forceQuit -> the normal cleanup() (settings saved, markers removed: a clean exit, not
// a crash), with every GPU wait / teardown short-circuited by vkDeviceIsLost().
namespace
{
// Per viewer instance slot (slot 1 keeps the plain names): this session's report, and the
// copy shown at the next launch in the same slot.
std::string gpuLostFileName(bool last)
{
    const S32 slot = LLAppViewer::instance() ? LLAppViewer::instance()->getInstanceSlot() : 1;
    const char* base = last ? "slipstream_gpu_lost_last" : "slipstream_gpu_lost";
    return slot > 1 ? llformat("%s-%d.txt", base, slot) : std::string(base) + ".txt";
}
std::mutex sGpuLostMutex;
std::string sGpuLostText;            // the block (bounded: written once)
thread_local bool tGpuLostCapture = false;   // fault-report lines go into the block too
bool sGpuLostAppStateDone = false;   // main-thread state written
bool sGpuLostExitStarted = false;
bool sRelaunchAfterExit = false;
std::string sRelaunchExe;
// Focus history (#209: the loss happened while the viewer was in the background).
bool sVkFocusKnown = false;
bool sVkFocusLast = true;
F64 sVkFocusChangedAt = 0.0;

void gpuLostLine(const std::string& line)   // sGpuLostMutex held
{
    LL_WARNS("Vulkan") << "[GPU LOST] " << line << LL_ENDL;
    sGpuLostText += line;
    sGpuLostText += '\n';
}

void gpuLostLines(const std::string& text)   // '\n'-separated
{
    size_t a = 0;
    while (a < text.size())
    {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        if (b > a) gpuLostLine(text.substr(a, b - a));
        a = b + 1;
    }
}

void gpuLostWriteMarker()   // sGpuLostMutex held
{
    if (!gDirUtilp) return;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, gpuLostFileName(false));
    llofstream f(path.c_str(), std::ios::out | std::ios::trunc);
    if (f.is_open())
    {
        f << sGpuLostText;
        f.close();
    }
}

// Viewer state: main thread only (settings, window, focus).
void gpuLostAppState()   // sGpuLostMutex held
{
    if (sGpuLostAppStateDone) return;
    sGpuLostAppStateDone = true;
    gpuLostLine("Viewer: " + LLVersionInfo::instance().getChannelAndVersion() + " (" +
                LLVersionInfo::instance().getBuildConfig() + "), startup state " + LLStartUp::getStartupStateString());
    const F64 now = LLFrameTimer::getElapsedSeconds();
    gpuLostLine(llformat("Session: %.0f s since start, frame %u", now, (U32)gFrameCount));
    if (LLAppViewer* app = LLAppViewer::instance())
        gpuLostLine(llformat("Instance: viewer %d, log %s", app->getInstanceSlot(), app->getLogFilePath().c_str()));
    LLWindow* win = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    const bool focus = gFocusMgr.getAppHasFocus();
    std::string w = focus ? "focused" : "NOT focused (in the background)";
    if (!focus && sVkFocusKnown) w += llformat(" for %.0f s", now - sVkFocusChangedAt);
    if (win)
    {
        w += win->getMinimized() ? ", minimized" : ", not minimized";
        if (!win->getVisible()) w += ", hidden";
        if (win->getFullscreen())   // #919: which kind, and whether Vulkan exclusive mode was held
        {
            w += ", " + win->getFullscreenStateText();
            if (LLVulkanContext::instance().fullScreenExclusiveArmed())
                w += LLVulkanContext::instance().fullScreenExclusiveHeld() ? ", Vulkan exclusive mode held"
                                                                           : ", Vulkan exclusive mode armed, not held";
        }
    }
    gpuLostLine("Window: " + w);
    // The settings that change the GPU load most, and the background pacing (#209).
    static const char* const kSettings[] = {
        "RenderVKRayTracing", "RenderReflectionProbeDetail", "RenderShadowDetail", "RenderVKSunShadows",
        "RenderVKProjectorShadows", "RenderVKUpscaler", "RenderVKDlssRayReconstruction", "RenderVKRenderScale", "RenderVKTAA",
        "RenderVKAsyncComputeQueue", "RenderVKAsyncCompute", "RenderVKAsyncDeferredLighting", "RenderVKGIEarlyTrace", "RenderVKGILatencyFrame",
        "RenderVKFramePipelining", "RenderVKGodRays",
        "RenderVKMirrors", "RenderVKTextureCompression", "RenderVKTexBudgetMB", "RenderFarClip",
        "RenderQualityPerformance", "RenderVSyncEnable", "RenderVKFramePacing", "RenderVKNoVsyncPresentMode", "RenderFPSLimit", "BackgroundFPSLimit",
        "BackgroundYieldTime", "RenderVKGpuBreadcrumbs", "RenderVKDebugLabels", "RenderVKDebugSnapshotStress"};
    std::string line = "Settings:";
    for (const char* name : kSettings)
    {
        if (LLControlVariable* c = gSavedSettings.getControl(name))
            line += std::string(" ") + name + "=" + c->getValue().asString();
    }
    gpuLostLine(line);
}

// Restart (true) or quit. Not parented to the viewer window: #209's window was in the
// background, and a box owned by it can stay hidden behind other windows.
bool gpuLostAskRestart()
{
    const std::string title = "Slipstream: the graphics driver stopped responding";
    const std::string text =
        "Slipstream lost contact with the graphics card: the graphics driver stopped responding "
        "or was reset (on Windows: \"Display driver stopped responding and has recovered\").\n\n"
        "Slipstream cannot draw anything more in this session, so it will log out and close.\n\n"
        "Likely causes, and what usually helps:\n"
        "- An out-of-date or faulty graphics driver: install the latest driver from NVIDIA, AMD or Intel.\n"
        "- Settings too heavy for the graphics card: lower the graphics quality, turn off ray tracing "
        "and real-time reflections.\n"
        "- Overlays that hook into games (Overwolf, Discord, Steam, recording or FPS-counter tools) "
        "or another graphics-heavy program: close them.\n"
        "- An unstable graphics card: overclocks, undervolts or overheating. Try stock clocks.\n\n"
        "The details are in the log (" + LLAppViewer::instance()->getInstanceLogBaseName() + ".log) and in " +
        gpuLostFileName(false) + " in the logs folder; please include them in a bug report.";
    LLWindow* win = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    if (win)
    {
        win->flashIcon(10.f);   // the user may be elsewhere
        win->beforeDialog();
    }
    const SDL_MessageBoxButtonData buttons[2] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Restart Slipstream"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_ERROR;
    data.window = nullptr;
    data.title = title.c_str();
    data.message = text.c_str();
    data.numbuttons = 2;
    data.buttons = buttons;
    int id = 0;
    const bool shown = SDL_ShowMessageBox(&data, &id);
    if (win) win->afterDialog();
    if (!shown)
    {
        OSMessageBox(text + "\n\nSlipstream will now close.", title, OSMB_OK);
        return false;
    }
    return id == 1;
}

// Starts a new viewer process (end of cleanup(), markers removed, log closed). No LLProcess:
// it depends on the mainloop event pump, which cleanup() has torn down.
void relaunchViewer(const std::string& exe)
{
    if (exe.empty()) return;
#if LL_WINDOWS
    const std::wstring wexe = ll_convert_string_to_wide(exe);
    std::wstring cmd = L"\"" + wexe + L"\"";
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(wexe.c_str(), cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    // The environment (LD_LIBRARY_PATH from the launcher script) and the working directory
    // are inherited. Bounded fd sweep: RLIMIT_NOFILE can be enormous.
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd <= 0 || max_fd > 65536) max_fd = 65536;
    const pid_t pid = fork();
    if (pid == 0)
    {
        // Child of a multi-threaded process: async-signal-safe calls only until exec.
        for (long fd = 3; fd < max_fd; ++fd) close((int)fd);
        char* const argv[] = {const_cast<char*>(exe.c_str()), nullptr};
        execv(exe.c_str(), argv);
        _exit(127);
    }
#endif
}

// The safer settings the next launch offers after a device loss: name, value. Only
// lowered, never raised (the probe detail is capped, not set).
struct GpuSafeSetting { const char* name; S32 value; bool restart; };
const GpuSafeSetting kGpuSafeSettings[] = {
    {"RenderVKRayTracing", 0, true},
    {"RenderVKAsyncComputeQueue", 0, true},
    {"RenderReflectionProbeDetail", 1, false},
};

// "name: old -> new" for every safe setting the user's current values exceed.
std::vector<std::string> gpuSafeSettingChanges(bool apply, bool* needs_restart)
{
    std::vector<std::string> out;
    for (const GpuSafeSetting& g : kGpuSafeSettings)
    {
        LLControlVariable* c = gSavedSettings.getControl(g.name);
        if (!c) continue;
        const S32 cur = (S32)c->getValue().asInteger();
        if (cur <= g.value) continue;
        out.push_back(llformat("%s: %d -> %d%s", g.name, cur, g.value, g.restart ? " (after a restart)" : ""));
        if (needs_restart && g.restart) *needs_restart = true;
        if (apply) c->setValue(LLSD(g.value));
    }
    return out;
}

void onGpuLostSafeSettings(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        LL_INFOS("Vulkan") << "Previous session's graphics device loss: settings kept by the user" << LL_ENDL;
        return;
    }
    bool restart = false;
    const std::vector<std::string> changed = gpuSafeSettingChanges(true, &restart);
    // Written and latched now (not only at exit): a second loss must not lose the choice.
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    std::string list;
    for (const std::string& c : changed) list += "\n- " + c;
    LL_INFOS("Vulkan") << "Previous session's graphics device loss: safer settings applied:" << list << LL_ENDL;
    LLSD args;
    args["CHANGES"] = list.empty() ? std::string("\n(nothing needed changing)") : list;
    args["RESTART"] = restart ? std::string("Restart Slipstream for ray tracing / the async compute queue to change.")
                              : std::string("");
    LLNotificationsUtil::add("VKGpuLostSafeSettingsApplied", args);
}

// Next launch: the previous session ended out of memory (bad_alloc / Vulkan OOM: the exit
// marker said LAST_EXEC_BAD_ALLOC). Posted once on the first idle pass; suggests lower
// graphics settings and Help > Report Bug (#700).
void oomCheckPreviousSession()
{
    if (gLastExecEvent != LAST_EXEC_BAD_ALLOC) return;
    LL_WARNS("Vulkan") << "The previous session ended because the system ran out of memory" << LL_ENDL;
    doOnIdleOneTime([]() { LLNotificationsUtil::add("VKOomLastSession"); });
}

// Next launch: the previous session ended in a device loss (its report is still on disk).
// Consumed once (renamed to _last, which keeps it for a bug report); posted on the first
// idle pass, after the notification channels exist.
void gpuLostCheckPreviousSession()
{
    if (!gDirUtilp) return;
    const std::string marker = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, gpuLostFileName(false));
    if (!gDirUtilp->fileExists(marker)) return;
    std::string first;   // the "What:" line
    {
        llifstream f(marker.c_str());
        std::string l;
        while (f.is_open() && std::getline(f, l))
            if (l.compare(0, 6, "What: ") == 0) { first = l.substr(6); break; }
    }
    const std::string last_name = gpuLostFileName(true);
    const std::string last = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, last_name);
    LLFile::remove(last, ENOENT);
    if (LLFile::rename(marker, last) != 0) LLFile::remove(marker, ENOENT);   // never shown twice
    LL_WARNS("Vulkan") << "The previous session closed because the graphics device was lost ("
                       << (first.empty() ? "details in " + last_name : first) << ")" << LL_ENDL;
    doOnIdleOneTime([first, last_name]()
    {
        LLSD args;
        args["WHAT"] = first.empty() ? std::string("graphics device lost") : first;
        args["FILE"] = last_name;
        const std::vector<std::string> would = gpuSafeSettingChanges(false, nullptr);
        if (would.empty())
        {
            LLNotificationsUtil::add("VKGpuLostLastSessionInfo", args);
            return;
        }
        std::string list;
        for (const std::string& c : would) list += "\n- " + c;
        args["CHANGES"] = list;
        LLNotificationsUtil::add("VKGpuLostLastSession", args, LLSD(), &onGpuLostSafeSettings);
    });
}
}

static void vk_device_lost_handler(const char* call, VkResult result, const char* queue, const char* file, int line)
{
    std::lock_guard<std::mutex> lk(sGpuLostMutex);
    LLError::setAlwaysFlush(true);   // every line below reaches the disk even if the exit goes wrong
    gpuLostLine("==================== SLIPSTREAM: GRAPHICS DEVICE LOST ====================");
    gpuLostLine(llformat("What: %s returned %s (%d)%s", call ? call : "?", vkResultString(result), (int)result,
                         result == VK_TIMEOUT ? " -- GPU hang: no progress for 60 s" :
                         result == VK_ERROR_SURFACE_LOST_KHR ? " -- the window's Vulkan surface was lost" : ""));
    gpuLostLine(llformat("Where: %s:%d, queue %s, %s thread%s", file ? file : "?", line, queue ? queue : "n/a",
                         on_main_thread() ? "main" : "worker",
                         vkDeviceLostSimulated() ? " -- SIMULATED (RenderVKDebugSimulateDeviceLost), the GPU is fine" : ""));
    gpuLostLine("When: " + LLDate::now().asString());
    gpuLostLines(LLVulkanContext::instance().deviceLostDiagnostics());
    if (on_main_thread())
        gpuLostAppState();
    else
        gpuLostLine("(raised on a worker thread: the viewer state follows from the main loop)");
    {
        const char* marks[32];
        const uint32_t n = vkRecentMarks(marks, 32);
        std::string m = "Recently recorded GPU passes (oldest first; recorded, not necessarily executed):";
        for (uint32_t i = 0; i < n; ++i) m += std::string(" ") + marks[i];
        gpuLostLine(n ? m : std::string("Recently recorded GPU passes: none"));
    }
    // Built-in effects (#546): which were running; also tells the next launch to start
    // with FX off and quarantine the new / suspect ones (any thread, static state only).
    gpuLostLine(SLFxStack::deviceLostSummary());
    gpuLostLine("--- device fault report / GPU breadcrumbs (where the GPU was) ---");
    tGpuLostCapture = true;
    vkReportDeviceLost();
    tGpuLostCapture = false;
    gpuLostLine("==================== END GRAPHICS DEVICE LOST ====================");
    gpuLostWriteMarker();   // also tells the next launch
}

// Every other VK_CHECK failure: a log line and the viewer's crash path (marker + dump),
// instead of abort() -- which on Windows is a fail-fast with neither (#107).
static void vk_fatal_handler(const char* expr, VkResult result, const char* file, int line)
{
    LLError::setAlwaysFlush(true);
    // #700: last words with the failing call; an out-of-memory result also leaves the OOM
    // exit marker (the next start suggests lower settings). createErrorMarker keeps the
    // marker the crash path would write only if none exists, so this must come first.
    LLMemDiag::lastWords(llformat("Vulkan call failed: %s = %s at %s:%d", expr ? expr : "?",
                                  vkResultString(result), file ? file : "?", line).c_str());
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
    {
        if (LLAppViewer* app = LLAppViewer::instance()) app->createErrorMarker(LAST_EXEC_BAD_ALLOC);
    }
    LL_ERRS("Vulkan") << "Vulkan call failed: " << (expr ? expr : "?") << " = " << vkResultString(result)
                      << " (" << (int)result << ") at " << (file ? file : "?") << ":" << line << LL_ENDL;
}

void LLAppViewer::onGraphicsDeviceLost()
{
    if (sGpuLostExitStarted) return;
    sGpuLostExitStarted = true;
    pauseMainloopTimeout();   // the message box blocks this thread
    {
        std::lock_guard<std::mutex> lk(sGpuLostMutex);
        if (!sGpuLostAppStateDone)
        {
            gpuLostLine("--- viewer state (main loop) ---");
            gpuLostAppState();
            gpuLostWriteMarker();
        }
        // #874: the RT scene audit's CPU-table half, when the loss was raised on another
        // thread than the one recording the RT build (prints nothing otherwise).
        if (gVkRtLostAuditMainLoop)
        {
            tGpuLostCapture = true;
            gVkRtLostAuditMainLoop();
            tGpuLostCapture = false;
            gpuLostWriteMarker();
        }
    }
    sRelaunchExe = gDirUtilp ? gDirUtilp->getExecutablePathAndName() : std::string();
    const bool restart = gpuLostAskRestart();
    sRelaunchAfterExit = restart && !sRelaunchExe.empty();
    LL_WARNS("Vulkan") << "Graphics device lost: logging out and closing"
                       << (sRelaunchAfterExit ? " (the user chose to restart)" : "") << LL_ENDL;
    // Not requestQuit(): its shutdown waits for modal dialogs and floater close prompts,
    // which can no longer be drawn. Tell the sim we are leaving, then the normal cleanup().
    if (gAgent.getRegion() && !gDisconnected)
    {
        sendLogoutRequest();
    }
    forceQuit();
}

void LLAppViewer::relaunchIfRequested()
{
    if (!sRelaunchAfterExit) return;
    sRelaunchAfterExit = false;
    // The new instance must not see this one as still running (exec marker) or rotate a
    // log this process still has open.
    removeMarkerFiles();
    LL_INFOS("Vulkan") << "Restarting the viewer after the graphics device loss: " << sRelaunchExe << LL_ENDL;
    LLError::logToFile("");
    relaunchViewer(sRelaunchExe);
}

// #876 / #890: the compile-crash marker (logs/pipeline_compile_crash.marker, one line
// "<unix time> <sessions left>", written by the crash handler: llwindebug.cpp / the Unix
// fatal-signal hook). Read once at startup: a recent marker (< 30 days) with sessions left
// means this session compiles one at a time; the count is rewritten one lower; the session
// after the last one deletes the file and steps back to automatic.
struct PipelineCrashMarkerState
{
    enum Mode { NONE, SAFE, STEPPED_BACK } mode = NONE;
    S32 left_after = 0;     // clean sessions still to run one-at-a-time after this one
};

static PipelineCrashMarkerState consumePipelineCrashMarker(const std::string& path)
{
    PipelineCrashMarkerState st;
    long long when = 0;
    int left = 0;
    {
        llifstream f(path.c_str());
        if (!f.is_open()) return st;
        f >> when >> left;
        if (f.fail()) { when = 0; left = 0; }
    }
    const long long now = (long long)time(NULL);
    if (when <= 0 || now - when > 30LL * 24 * 3600)
    {
        LLFile::remove(path);   // unreadable or stale
        return st;
    }
    if (left <= 0)
    {
        LLFile::remove(path);
        st.mode = PipelineCrashMarkerState::STEPPED_BACK;
        return st;
    }
    st.mode = PipelineCrashMarkerState::SAFE;
    st.left_after = left - 1;
    llofstream o(path.c_str(), std::ios::out | std::ios::trunc);
    if (o.is_open()) o << when << " " << st.left_after << "\n";
    return st;
}

#if !LL_WINDOWS
// Runs in the Unix fatal-signal handler (async-signal-safe): arm the marker when the faulting
// thread was compiling a pipeline. (Windows does this in llwindebug.cpp, which also names the
// driver module; here only a compile-worker fault is seen, not a driver-owned thread.)
static void pipelineCrashSignalHook()
{
    if (vkPipelineCrashOnCurrentThread())
    {
        vkPipelineCrashMarkerWrite();
    }
}
#endif


// EXPERIMENTAL Vulkan integration.
//
// Bring up LLVulkanContext on the SDL window, install the LLRenderVK facade +
// backend (so gGL dispatches to Vulkan), and initialize the real font system.
// Does NOT run a render loop and does NOT early-out the boot: after this returns
// true the viewer continues its normal init (with the GL-3D-pipeline pieces
// guarded) so the REAL LLUI + startup login state machine come up and render
// through the VK present path each frame (see displayVulkanUI()). Returns true
// on success. File-local to avoid touching llappviewer.h.
static bool initVulkanForRealUI()
{
    LLWindow* win = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    LLWindowSDL* sdlwin = dynamic_cast<LLWindowSDL*>(win);
    if (!sdlwin || !sdlwin->getSDLWindow())
    {
        LL_WARNS("Vulkan") << "No SDL window available for Vulkan bring-up." << LL_ENDL;
        return false;
    }

    // #736 crash-loop guard: a risky render setting raised shortly before the previous
    // session crashed is put back here, before anything below or the renderer reads it.
    if (LLAppViewer* app = LLAppViewer::instance())
        SLRiskySettings::startup(app->isSecondInstance());

    LLCoordScreen sz;
    sdlwin->getSize(&sz);
    U32 w = (U32)(sz.mX > 0 ? sz.mX : 1024);
    U32 h = (U32)(sz.mY > 0 ? sz.mY : 768);

    // Validation defaults off (gDebugGL) but can be FORCE-enabled for diagnosis via
    // the env var, without touching settings / --debuggl. With it on, the full
    // validation feature set (GPU-assisted / best-practices / debugPrintf / sync)
    // is requested in the VK instance so the layers can report compute SSBO OOB /
    // stale-descriptor / uninitialized reads directly.
    const bool enableValidation = gDebugGL || (sl_getenv("VK_VALIDATION") != nullptr);
    if (enableValidation)
        LL_WARNS("Vulkan") << "Vulkan validation FORCE-ENABLED (gDebugGL or "
                              "SLIPSTREAM_VK_VALIDATION)" << LL_ENDL;
    // RenderVKRayTracing: 0 off, 1 auto (enable the RT extensions when the device
    // supports them). Device-creation time only -- takes effect on restart.
    LLVulkanContext::instance().setRayTracingRequested(gSavedSettings.getU32("RenderVKRayTracing") != 0);
    // RenderVKClusterLOD (#743): on at startup, every cluster extension the GPU has is enabled
    // (incl. the startup-only DGC + maintenance5 and partitioned TLAS). Off, the ones inert while
    // unused still are (RenderVKClusterDeviceFeatures: 0 none = the device exactly as before,
    // 1 the inert ones, 2 + mesh shaders), so switching it on mid-session runs those tiers
    // without a restart. Restart only.
    LLVulkanContext::instance().setClusterRequested(gSavedSettings.getBOOL("RenderVKClusterLOD"));
    // #870 DLSS Ray Reconstruction: the user-installed slip_nr shim (contrib/ngxshim) is looked
    // for in RenderVKDlssShimPath (when empty: the <user data dir>/dlss folder that
    // contrib/ngxshim/build.sh installs into), then the viewer's own library folder, and loaded before the
    // Vulkan instance (its extensions). Startup only.
    {
#if LL_WINDOWS
        const std::string lib_dir = gDirUtilp->getExecutableDir();
#else
        const std::string lib_dir = gDirUtilp->getExecutableDir() + gDirUtilp->getDirDelimiter() + ".."
                                  + gDirUtilp->getDirDelimiter() + "lib";
#endif
        const std::string ngx_dir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "ngx");
        LLFile::mkdir(ngx_dir);
        std::string shim_path = gSavedSettings.getString("RenderVKDlssShimPath");
        LLStringUtil::trim(shim_path);
        if (!shim_path.empty() && shim_path[0] == '~')
        {
            const char* home = getenv("HOME");
            if (home) shim_path = std::string(home) + shim_path.substr(1);
        }
        LLVulkanContext::instance().setNeuralReconConfig(gSavedSettings.getU32("RenderVKDlssShim") != 0,
                                                         shim_path, lib_dir,
                                                         ngx_dir, LLVersionInfo::instance().getVersion(),
                                                         gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "dlss"));
    }
    LLVulkanContext::instance().setClusterDeviceFeatures(gSavedSettings.getU32("RenderVKClusterDeviceFeatures"));
    // RenderVKAsyncComputeQueue (roadmap phase 3a): 0 none, 1 dedicated compute family
    // (else a 2nd graphics-family queue), 2 a 2nd graphics-family queue. Restart only.
    LLVulkanContext::instance().setAsyncComputeMode(gSavedSettings.getU32("RenderVKAsyncComputeQueue"));
    // RenderVKAsyncCompute: -1 (default) = auto, the per-architecture default decided at device
    // selection (VkDeviceContext::asyncAutoDecide; auto-off = no second queue), 0 off, 1 on.
    // Auto only while the user chose neither it nor RenderVKAsyncComputeQueue: an explicit 0 / 1
    // always wins. Migration from the old Boolean (default 1, Persist = save only when not
    // default): only an explicit 0 was ever written to the user's settings and it loads as
    // S32 0 (LLSD Boolean asInteger); untouched users had nothing stored and move to auto.
    {
        LLControlVariable* q = gSavedSettings.getControl("RenderVKAsyncComputeQueue");
        LLControlVariable* a = gSavedSettings.getControl("RenderVKAsyncCompute");
        if (a && !a->isDefault() && a->getSaveValue().isBoolean())
            a->setValue(LLSD(a->getSaveValue().asInteger()));   // old Boolean value -> S32 0 / 1
        const bool master_auto = a && a->getValue().asInteger() < 0;
        LLVulkanContext::instance().setAsyncComputeAuto(q && q->isDefault() && master_auto);
    }
    // RenderVKDebugRobustness (#845 / #917): device-wide robust buffer / image access + null
    // descriptors, a device-loss diagnostic. Device-creation time only -- restart.
    LLVulkanContext::instance().setDebugRobustness(gSavedSettings.getBOOL("RenderVKDebugRobustness"));
    // RenderVKPipelineStats: per-pipeline register / instruction statistics in the log
    // (VK_KHR_pipeline_executable_properties). Device-creation time only -- restart.
    LLVulkanContext::instance().setPipelineStatsRequested(gSavedSettings.getBOOL("RenderVKPipelineStats"));
    // RenderVKShaderClockLighting: per-stage shader-clock counters in the deferred lighting
    // (VK_KHR_shader_clock + the *ClockSpv kernels). Device-creation time only -- restart.
    LLVulkanContext::instance().setShaderClockRequested(gSavedSettings.getBOOL("RenderVKShaderClockLighting"));
    // Cluster LOD (#743): the forced tier for the startup capability line (live afterwards).
    LLVulkanContext::instance().setClusterForceTier(gSavedSettings.getS32("RenderVKClusterForceTier"));
    // RenderVKPipelineCacheDisk: the persistent VkPipelineCache (vkgfx/vk_pipecache.h).
    // initCache() ran before this, so a requested Clear Cache already deleted the file.
    if (gSavedSettings.getBOOL("RenderVKPipelineCacheDisk"))
    {
        // The tag carries the embedded world shaders' content hash (vk_world_shaders_hash.h):
        // iterating on shaders under one viewer version used to pile every old binary up in
        // the file (225 MB vs ~41 MB of live pipelines); now a shader change starts it fresh
        // (and the setting specialization caches next to it, llcleanspecvk.cpp).
        char shader_tag[32];
        snprintf(shader_tag, sizeof(shader_tag), " shaders %016llx", (unsigned long long)kVkWorldShadersHash);
        LLVulkanContext::instance().setPipelineCacheFile(
            gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_pipeline_cache.bin"),
            LLVersionInfo::instance().getChannelAndVersion() + shader_tag);
        // #471: what each pipeline cost to compile on this GPU + driver (the compile
        // scheduler's memory estimates), next to the cache.
        LLVulkanContext::instance().setPipelineCostFile(
            gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_pipeline_costs.txt"));
    }
    else
        LL_INFOS("Vulkan") << "pipeline cache: disk cache off (RenderVKPipelineCacheDisk=0)" << LL_ENDL;
    gVkDeviceLostHandler = &vk_device_lost_handler;
    gVkFatalHandler = &vk_fatal_handler;
    // The DEVICE_LOST report (VK_EXT_device_fault addresses mapped to buffers, GPU
    // breadcrumbs) goes to stderr AND the log, so a user's log carries it; inside the
    // device-lost handler it is also captured into the delimited block / report file.
    gVkFaultLogSink = [](const char* line)
    {
        LL_WARNS("Vulkan") << line << LL_ENDL;
        if (tGpuLostCapture)
        {
            sGpuLostText += line;
            sGpuLostText += '\n';
        }
    };
    // The previous session ended in a device loss: offer safer settings (#135).
    gpuLostCheckPreviousSession();
    oomCheckPreviousSession();   // #700
    // Built-in effects stack (#544): state, effect discovery, the runtime's plan. After the
    // device-lost check (#546 reads its own markers in initSingleton).
    SLFxStack::getInstance();
    gVkBreadcrumbsWanted = gSavedSettings.getBOOL("RenderVKGpuBreadcrumbs");
    gVkDebugLabelsWanted = gSavedSettings.getBOOL("RenderVKDebugLabels");   // #973
    gVkMemoryReportWanted = gSavedSettings.getBOOL("RenderVKDriverMemoryReport");   // #407
    gVkTexPoolConfig.enabled = gSavedSettings.getBOOL("RenderVKTextureSuballoc");   // #411
    gVkTexPoolConfig.smallBlockMB = gSavedSettings.getU32("RenderVKTexturePoolSmallBlockMB");
    gVkTexPoolConfig.mediumMaxKB = gSavedSettings.getU32("RenderVKTexturePoolMediumMaxKB");
    gVkTexPoolConfig.mediumBlockMB = gSavedSettings.getU32("RenderVKTexturePoolMediumBlockMB");
    gVkTexPoolConfig.classesKB = gSavedSettings.getString("RenderVKTexturePoolClassesKB");   // #587
    // #695 UMA (integrated GPU with a BIOS carve-out): the budget counts the carve-out + a
    // share of system RAM; the debug switches simulate it on a discrete card.
    gVkUmaConfig.forceUma = gSavedSettings.getBOOL("RenderVKDebugForceUma");
    gVkUmaConfig.fakeCarveoutMB = gSavedSettings.getU32("RenderVKDebugFakeCarveoutMB");
    gVkUmaConfig.sharedRamPercent = gSavedSettings.getU32("RenderVKUmaSharedRamPercent");
    {
        LLMemoryGovernor::SystemMemory sm;
        gVkUmaConfig.ramTotal = LLMemoryGovernor::readSystemMemory(sm) ? sm.total : 0;
    }
    // DEBUG (#238): check every pipeline layout's push constants against this cap (128 =
    // an AMD Polaris device) instead of the real limit; violations log ERROR at startup.
    vkdSetPushConstantCap(gSavedSettings.getU32("RenderVKPushConstantCap"));
    // TESTING (#672): RenderVKDebugSimulatePipelineOOM K > 0 makes the next K pipeline
    // creations report VK_ERROR_OUT_OF_HOST_MEMORY without calling the driver (the
    // recovery path: requeue + concurrency step-down + retry, or give up -> feature off).
    // Armed before init for the BATCH compiles only (the world warm-up at the login screen,
    // as in #667 / #669; the synchronous UI pipelines stay real so the screen and the notice
    // remain visible). Armed in-world (per frame, below) it covers every creation.
    // One-shot (reset to 0).
    if (const U32 sim_oom = gSavedSettings.getU32("RenderVKDebugSimulatePipelineOOM"))
    {
        gSavedSettings.setU32("RenderVKDebugSimulatePipelineOOM", 0);
        vkSimulatePipelineOom(sim_oom, /*batchOnly=*/true);
        LL_WARNS("VKPipe") << "RenderVKDebugSimulatePipelineOOM " << sim_oom << ": the next " << sim_oom
                           << " batch pipeline compiles report VK_ERROR_OUT_OF_HOST_MEMORY (testing aid)" << LL_ENDL;
    }
    LLVulkanContext::instance().setVSync(gSavedSettings.getBOOL("RenderVSyncEnable"));
    LLVulkanContext::instance().setNoVsyncPresentMode(gSavedSettings.getU32("RenderVKNoVsyncPresentMode"));
    if (!LLVulkanContext::instance().init(sdlwin->getSDLWindow(), w, h, enableValidation))
    {
        LL_WARNS("Vulkan") << "LLVulkanContext::init failed." << LL_ENDL;
        return false;
    }

    // #919: a FullScreen session enters fullscreen here -- the window was created windowed at
    // the saved windowed placement (so leaving fullscreen restores it), and the renderer is
    // up, which FullScreenMode "automatic" needs on Windows (exclusive needs
    // VK_EXT_full_screen_exclusive). Before show(), so no windowed frame flashes.
    if (gSavedSettings.getBOOL("FullScreen") && gViewerWindow)
    {
        gViewerWindow->setFullscreen(true);
    }

    // Make the window visible so the presented frames are seen.
    sdlwin->show();

    // Install the LLRenderVK facade + backend so gGL (LLRender) dispatches to
    // Vulkan. AFTER this, any LLFontGL/gl_rect_2d/LLRender/LLView::draw call
    // records into the facade (until then LLRender talks to a no-op backend).
    LLRenderBackend* backend = LLVulkanContext::instance().initRenderBackend();
    if (!backend)
    {
        LL_WARNS("Vulkan") << "Render backend init failed; cannot bring up VK UI." << LL_ENDL;
        return false;
    }
    llassert_always(LLRender::hasBackend() && LLRender::sBackend == backend);

    // GPU identity (gGPUInfo) from the real Vulkan device: name, vendor, API and driver
    // version, VRAM (largest device-local heap), max texture size and anisotropy. Read by
    // the About box, crash/bug reports, the login ViewerStats blob and the feature
    // manager's vendor / anisotropy masks (re-applied below, once the GPU class is known).
    {
        LLVulkanContext& vkc = LLVulkanContext::instance();
        std::string vkDevice, vkVendorShort, vkApiVer;
        bool vkNV = false, vkAMD = false, vkIntel = false;
        uint32_t vkVramMB = 0;
        vkc.getDeviceIdentity(vkDevice, vkVendorShort, vkApiVer, vkNV, vkAMD, vkIntel, vkVramMB);
        if (!vkDevice.empty())
        {
            gGPUInfo.mRenderer = vkDevice;
            gGPUInfo.mVendor = vkVendorShort;
            gGPUInfo.mAPIVersion = vkApiVer;
            gGPUInfo.mDriverVersion = vkc.driverVersionText();
            gGPUInfo.mIsNVIDIA = vkNV;
            gGPUInfo.mIsAMD = vkAMD;
            gGPUInfo.mIsIntel = vkIntel;
            if (vkVramMB > 0)
            {
                gGPUInfo.mVRAM = vkVramMB;
            }
            gGPUInfo.mMaxTextureSize = (S32)vkc.maxImageDimension();
            gGPUInfo.mMaxAnisotropy = vkc.getMaxSamplerAnisotropy();
            LL_INFOS("Vulkan") << "GPU identity from Vulkan device: "
                               << vkDevice << " (" << vkVendorShort << ", " << vkApiVer
                               << ", driver " << gGPUInfo.mDriverVersion
                               << ", VRAM " << vkVramMB << " MB, max texture " << gGPUInfo.mMaxTextureSize
                               << ", max aniso " << gGPUInfo.mMaxAnisotropy << ")" << LL_ENDL;
            gGPUInfo.getInfo(gDebugInfo);
        }
    }

    // GPU TIER (VK). LL picks the first-run graphics preset from a GL benchmark, which
    // can't run here, so every VK user started on high-end defaults (4 x 2048 realtime
    // cascades, 16 projector shadows) -- a GTX 1080 Ti spent ~120 ms/frame on shadows.
    // Classify from the device instead: integrated vs discrete, hardware RT (Turing /
    // RDNA2 / Arc and newer) and VRAM, then apply the recommended preset under the same
    // conditions LL uses (first run, new GPU, new feature table, safe mode, or
    // ProbeHardwareOnStartup).
    {
        LLVulkanContext& vk = LLVulkanContext::instance();
        const bool integrated = vk.isIntegratedGPU();
        const bool hw_rt = vk.supportsHardwareRayTracing();
        const U32 vram_gb = (U32)(gGPUInfo.mVRAM / 1024);
        EGPUClass cls;
        if (integrated)          cls = vram_gb >= 4 ? GPU_CLASS_1 : GPU_CLASS_0;
        else if (!hw_rt)         cls = vram_gb >= 8 ? GPU_CLASS_3
                                     : vram_gb >= 4 ? GPU_CLASS_2 : GPU_CLASS_1;
        else                     cls = vram_gb >= 16 ? GPU_CLASS_5
                                     : vram_gb >= 10 ? GPU_CLASS_4
                                     : vram_gb >= 6  ? GPU_CLASS_3 : GPU_CLASS_2;
        LLFeatureManager* fm = LLFeatureManager::getInstance();
        const std::string gpu_string = gGPUInfo.mRenderer.empty() ? fm->getGPUString() : gGPUInfo.mRenderer;
        fm->setVulkanGPU(cls, gpu_string);
        // The feature manager's base masks ran in its initSingleton (LLViewerWindow ctor),
        // before the device existed: placeholder class 5, no vendor, no anisotropy (so
        // AnisotropicMissing always applied). Re-apply them with the real device.
        fm->applyBaseMasks();
        LL_INFOS("RenderInit") << "Vulkan GPU tier: class " << (S32)cls << " (" << (integrated ? "integrated" : "discrete")
                               << ", hw RT " << (hw_rt ? "yes" : "no") << ", " << vram_gb << " GB VRAM)" << LL_ENDL;

        // Earlier VK builds recorded the placeholder "Vulkan Device" as the GPU: moving
        // to the real name is not a GPU change (don't reset those users' settings).
        std::string last_gpu = gSavedSettings.getString("LastGPUString");
        if (last_gpu == "Vulkan Device")
        {
            last_gpu = gpu_string;
            gSavedSettings.setString("LastGPUString", gpu_string);
        }
        if (fm->isSafe()
            || gSavedSettings.getS32("LastFeatureVersion") != fm->getVersion()
            || last_gpu != gpu_string
            || gSavedSettings.getBOOL("ProbeHardwareOnStartup"))
        {
            LL_INFOS("RenderInit") << "Applying recommended graphics settings for this GPU" << LL_ENDL;
            fm->applyRecommendedSettings();
            gSavedSettings.setBOOL("ProbeHardwareOnStartup", false);
        }

        // TEXTURE COMPRESSION DEFAULT (#263). Applied ONCE: on a graphics card change
        // (the card decides it), and on the first launch of a version that raised
        // kTexCompDefaultVersion -- for existing users only while they haven't chosen a
        // level themselves (the setting at its default, or at the level this wrote). It writes the
        // real setting (the Preferences / Lightbox combos show it), is never changed
        // automatically again, and the user is told once. (After the recommended
        // settings above, so nothing overwrites it.)
        const U32 tc_marker = gSavedSettings.getU32("RenderVKTexCompressionDefaultApplied");
        const bool gpu_changed = last_gpu != gpu_string;   // also the first launch (no last GPU)
        LLControlVariable* tc = gSavedSettings.getControl("RenderVKTextureCompression");
        if (tc && vk.textureCompressionBC() && (gpu_changed || tc_marker < LLVulkanContext::kTexCompDefaultVersion))
        {
            // "Chosen" by the user = different from what this logic wrote last time
            // (RenderVKTexCompressionAutoLevel; 99 = never wrote, then any non-default value
            // counts) -- so a later version can move users who got the automatic level to a
            // new default (e.g. Adaptive) while leaving the ones who picked a level alone,
            // including back to Off.
            const U32 auto_level = gSavedSettings.getU32("RenderVKTexCompressionAutoLevel");
            const U32 cur_level = (U32)tc->getValue().asInteger();
            const bool chosen = cur_level != auto_level && (auto_level != 99u || !tc->isDefault());
            // Every card with BC support gets the same level (version 2: Adaptive, which
            // picks the format per texture with no visible loss, so large cards compress too).
            const U32 level = LLVulkanContext::kTexCompDefault;
            const U32 old = (U32)tc->getValue().asInteger();
            if (gpu_changed || !chosen) gSavedSettings.setU32("RenderVKTexCompressionAutoLevel", level);
            if ((gpu_changed || !chosen) && level != old)
            {
                gSavedSettings.setU32("RenderVKTextureCompression", level);
                static const char* kLevelName[5] = {"Off", "High quality", "Balanced", "Max savings", "Adaptive"};
                const char* name = kLevelName[std::min<U32>(level, 4u)];
                LLSD args;
                args["LEVEL"] = name;
                args["REASON"] = "the recommended default";
                args["EFFECT"] = level == 0 ? "It has plenty of memory for uncompressed textures, which load a little faster."
                               : level == 4 ? "It cuts texture memory 4-8x, choosing the format per texture so nothing visibly loses quality."
                                            : "It cuts texture memory about 4x with no visible loss.";
                // Posted on the first idle pass, not here: initWindow runs before the notification
                // screen channels exist, and LLScreenChannel::init dereferenced a missing view
                // (crash at startup on every <=16 GB card, beta.4).
                doOnIdleOneTime([args]() { LLNotificationsUtil::add("VKTextureCompressionDefault", args); });
                LL_INFOS("RenderInit") << "Texture compression default applied: " << old << " -> " << level << " (" << name
                                       << "; " << (integrated ? "integrated" : "discrete") << ", " << gGPUInfo.mVRAM << " MB VRAM"
                                       << (gpu_changed ? ", new GPU" : ", first launch of this version") << ")" << LL_ENDL;
            }
            else
            {
                LL_INFOS("RenderInit") << "Texture compression default: level stays " << old
                                       << (chosen && !gpu_changed ? " (chosen by the user)" : "") << LL_ENDL;
            }
            gSavedSettings.setU32("RenderVKTexCompressionDefaultApplied", LLVulkanContext::kTexCompDefaultVersion);
        }
    }

    // The font system is normally initialized in the LLViewerWindow ctor with
    // create_gl_textures=false in some paths; ensure it is up with VK texture
    // creation. setFromRaw is VK-aware (no GL) and the glyph-atlas LLImageRaw
    // stays resident for the on-bind VK upload path.
    LLFontManager::initClass();
    LLFontGL::initClass(gSavedSettings.getF32("FontScreenDPI"),
                        LLUI::getScaleFactor().mV[VX],
                        LLUI::getScaleFactor().mV[VY],
                        gDirUtilp->getAppRODataDir(), /*create_gl_textures=*/true,
                        gSavedSettings.getString("FSFontSettingsFile"),
                        llclamp(gSavedSettings.getF32("FSFontSizeAdjustment"), -2.f, 4.f));

    // #462 / #471: user caps on the pipeline compiles. Applied before the first frame, where
    // the world pipelines start compiling. Threads: a hard cap on the compile workers (0 =
    // hardware threads - 1). Budget: a fixed memory budget in place of the live headroom
    // the scheduler normally admits against (0 = automatic; for testing low-memory machines).
    {
        const S32 forced = gSavedSettings.getS32("RenderVKPipelineCompileThreads");
        // #876 / #890: one pipeline at a time when (a) the driver is Intel's proprietary
        // Windows one (parallel vkCreate*Pipelines crashes it), or (b) the previous session
        // crashed while compiling (marker file written by the crash handler). An explicit
        // RenderVKPipelineCompileThreads wins; no setting is rewritten.
        const std::string marker_path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "pipeline_compile_crash.marker");
        const PipelineCrashMarkerState crash_state = consumePipelineCrashMarker(marker_path);
        vkPipelineCrashMarkerSetPath(marker_path.c_str());
#if !LL_WINDOWS
        LLApp::setFatalSignalHook(&pipelineCrashSignalHook);
#endif
        const bool intel_win = LLVulkanContext::driverIsIntelWindows() || gSavedSettings.getBOOL("VKDebugForceIntelCompileCap");
        std::string cap_note;
        if (forced > 0)
        {
            vkSetPipelineCompileThreads((unsigned)forced);
            LL_INFOS("Vulkan") << "Pipeline compiles: at most " << forced << " at once (RenderVKPipelineCompileThreads)" << LL_ENDL;
            cap_note = "capped to " + std::to_string(forced) + " (RenderVKPipelineCompileThreads)";
            if (intel_win || crash_state.mode == PipelineCrashMarkerState::SAFE)
            {
                LL_INFOS("Vulkan") << "Pipeline compiles: the automatic one-at-a-time cap ("
                                   << (intel_win ? "Intel Windows driver, #876" : "previous compile crash, #876/#890")
                                   << ") is overridden by RenderVKPipelineCompileThreads" << LL_ENDL;
            }
        }
        else
        {
            if (intel_win)
            {
                LL_INFOS("Vulkan") << "Pipeline compiles: 1 at a time (Intel Windows driver: parallel pipeline creation crashes it, #876; set RenderVKPipelineCompileThreads to override)" << LL_ENDL;
                cap_note = "1 at a time (Intel Windows driver, #876)";
            }
            if (crash_state.mode == PipelineCrashMarkerState::SAFE)
            {
                LL_WARNS("Vulkan") << "previous session crashed while compiling shaders: compiling one at a time for this session (#876/#890)" << LL_ENDL;
                LL_INFOS("Vulkan") << "Pipeline compiles: " << crash_state.left_after << " more clean session(s) one at a time after this one, then automatic again (marker " << marker_path << ")" << LL_ENDL;
                if (!cap_note.empty()) cap_note += "; ";
                cap_note += "1 at a time (previous session crashed compiling shaders, #876/#890; " + std::to_string(crash_state.left_after) + " more after this)";
            }
            else if (crash_state.mode == PipelineCrashMarkerState::STEPPED_BACK)
            {
                LL_INFOS("Vulkan") << "Pipeline compiles: " << kVkPipelineCrashSafeSessions << " clean sessions since the shader-compile crash: back to automatic compile concurrency (#876/#890)" << LL_ENDL;
            }
            if (intel_win || crash_state.mode == PipelineCrashMarkerState::SAFE)
            {
                vkSetPipelineCompileThreads(1);
            }
        }
        LLVulkanContext::setPipelineCompileCapNote(cap_note);
        const U32 budget_mb = gSavedSettings.getU32("RenderVKPipelineCompileBudgetMB");
        LLVulkanContext::setPipelineCompileBudgetMB(budget_mb);
        if (budget_mb > 0)
        {
            LL_INFOS("Vulkan") << "Pipeline compiles: fixed " << budget_mb << " MB memory budget (RenderVKPipelineCompileBudgetMB)" << LL_ENDL;
        }
    }
    LL_INFOS("Vulkan") << "Vulkan facade + backend + fonts up; real UI will render "
                          "through gGL -> LLRenderVK -> swapchain." << LL_ENDL;
    return true;
}

// ===========================================================================
// VK SYNTHETIC-CUBE SELF-TEST (milestone proof; NO login required).
//
// Pushes a code-built cube LLVolume through the REAL clean-pipeline ingest
// layer -- LLCleanMeshAsset::capture() (geometry -> SlotInfo + interleaved CPU
// mirror), registerLodSlot() (LOD group), and LLCleanInstanceStore::upsert()
// (one static-opaque instance per cube face) -- exactly the calls
// registerCleanFace makes, minus the live-object plumbing that only exists once
// logged in. LLCleanRendererVK::fillFromCleanStores() then mirrors these stores
// into VkBuffers and the GPU-driven cull+draw renders them. Proven by an
// offscreen PNG (LLVulkanContext::runWorldSynthTest). Triggered once when the
// env var SLIPSTREAM_VK_SYNTHTEST is set (its value is the output PNG path, or a
// default is used).
// ===========================================================================

// Fill a fixed camera (column-major MVP, Vulkan NDC + Y-flip, Gribb/Hartmann
// planes) looking at `target` from `eye`, matching buildVulkanWorldFrame's
// convention so the cull/draw math is identical. SL up is +Z.
static void vkSynthBuildCamera(LLVKWorldFrame& wf, const LLVector3& eye,
                               const LLVector3& target,
                               F32 fovY = 60.f * 0.017453292f, F32 aspect = 1.f,
                               F32 zn = 0.1f, F32 zf = 200.f)
{
    LLVector3 fwd = target - eye; fwd.normalize();
    LLVector3 world_up(0.f, 0.f, 1.f);
    LLVector3 right = fwd % world_up; right.normalize();   // cross
    LLVector3 up    = right % fwd;    up.normalize();

    const F32 rt[3]  = { right.mV[0], right.mV[1], right.mV[2] };
    const F32 upv[3] = { up.mV[0],    up.mV[1],    up.mV[2] };
    const F32 fw[3]  = { fwd.mV[0],   fwd.mV[1],   fwd.mV[2] };
    const F32 tx = -(rt[0]*eye.mV[0]  + rt[1]*eye.mV[1]  + rt[2]*eye.mV[2]);
    const F32 ty = -(upv[0]*eye.mV[0] + upv[1]*eye.mV[1] + upv[2]*eye.mV[2]);
    const F32 tz =  (fw[0]*eye.mV[0]  + fw[1]*eye.mV[1]  + fw[2]*eye.mV[2]);
    F32 V[16] = {
        rt[0],  upv[0], -fw[0], 0.f,
        rt[1],  upv[1], -fw[1], 0.f,
        rt[2],  upv[2], -fw[2], 0.f,
        tx,     ty,     tz,     1.f
    };
    const F32 f = 1.0f / std::tan(fovY * 0.5f);
    F32 P[16] = {
        f/aspect, 0.f, 0.f,             0.f,
        0.f,      f,   0.f,             0.f,
        0.f,      0.f, zf/(zn-zf),     -1.f,
        0.f,      0.f, (zn*zf)/(zn-zf), 0.f
    };
    P[1] = -P[1]; P[5] = -P[5]; P[9] = -P[9]; P[13] = -P[13]; // Vulkan Y-flip

    F32* M = wf.view_proj;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            F32 s = 0.f;
            for (int k = 0; k < 4; ++k) s += P[k*4 + r] * V[c*4 + k];
            M[c*4 + r] = s;
        }
    auto Mel = [&](int col, int row){ return M[col*4 + row]; };
    F32 r0[4] = { Mel(0,0), Mel(1,0), Mel(2,0), Mel(3,0) };
    F32 r1[4] = { Mel(0,1), Mel(1,1), Mel(2,1), Mel(3,1) };
    F32 r2[4] = { Mel(0,2), Mel(1,2), Mel(2,2), Mel(3,2) };
    F32 r3[4] = { Mel(0,3), Mel(1,3), Mel(2,3), Mel(3,3) };
    auto setPlane = [&](int idx, F32 a, F32 b, F32 c, F32 d){
        F32 len = std::sqrt(a*a + b*b + c*c);
        if (len > 0.f) { a/=len; b/=len; c/=len; d/=len; }
        wf.frustum_planes[idx][0]=a; wf.frustum_planes[idx][1]=b;
        wf.frustum_planes[idx][2]=c; wf.frustum_planes[idx][3]=d;
    };
    setPlane(0, r3[0]+r0[0], r3[1]+r0[1], r3[2]+r0[2], r3[3]+r0[3]);
    setPlane(1, r3[0]-r0[0], r3[1]-r0[1], r3[2]-r0[2], r3[3]-r0[3]);
    setPlane(2, r3[0]+r1[0], r3[1]+r1[1], r3[2]+r1[2], r3[3]+r1[3]);
    setPlane(3, r3[0]-r1[0], r3[1]-r1[1], r3[2]-r1[2], r3[3]-r1[3]);
    setPlane(4, r2[0],       r2[1],       r2[2],       r2[3]);
    setPlane(5, r3[0]-r2[0], r3[1]-r2[1], r3[2]-r2[2], r3[3]-r2[3]);

    wf.cam_pos[0]=eye.mV[0]; wf.cam_pos[1]=eye.mV[1]; wf.cam_pos[2]=eye.mV[2];
    wf.cam_pos[3]=1.125f; // LOD factor
    wf.lod_cam_pos[0]=eye.mV[0]; wf.lod_cam_pos[1]=eye.mV[1]; wf.lod_cam_pos[2]=eye.mV[2];
    wf.lod_cam_pos[3]=0.f;
    wf.enabled = true;
}

// Inject the synthetic cube into the clean stores and fill `cam`. Returns the
// number of face-instances upserted (0 => failure / no geometry captured).
static U32 vkSynthInjectCube(LLVKWorldFrame& cam)
{
    // CPU-only store init.
    // TEXTURED MILESTONE: we build a real checkerboard texture, register it
    // through the REAL texture path (LLViewerFetchedTexture -> setFromRaw ->
    // LLTextureData::mVKCpuData), and reference it from a real
    // LLCleanMaterialStore material. fillFromCleanStores() copies that material,
    // uploadRealMaterialTextures() fetches the checker via getVKCpuData(),
    // uploads it to a VkImage, writes it into the partially-bound descriptor
    // array, and rewrites the material's base_color index. The fragment shader
    // then samples uTextures[material.base_color_handle.x] at v_uv -- true GPU
    // descriptor-indexed sampling, not vertex color.
    LLCleanMeshAsset::instance().init();
    LLCleanInstanceStore::instance().init();
    LLCleanMaterialStore::instance().init();   // reserves material_id 0 = default white

    // ---- Build a distinctive checkerboard and push it through the REAL path ----
    // 128x128 (power-of-two; LLTextureData::checkSize requires it), 16x16 cells so the
    // pattern is 8x8 -- deliberately NOT the 32x32/4px checker the renderer's
    // makeTexture() proto tints use, so a misroute into a prototype texture can't
    // masquerade as success. Two bright, well-separated colors survive lighting.
    U32 checker_mat_id = 0;                    // 0 => default white (fallback)
    bool checker_ok = false;
    {
        const S32 TW = 128, TH = 128, CELL = 16;
        LLPointer<LLImageRaw> raw = new LLImageRaw(TW, TH, 4);
        U8* px = raw->getData();
        if (px)
        {
            for (S32 y = 0; y < TH; ++y)
                for (S32 x = 0; x < TW; ++x)
                {
                    const bool a = (((x / CELL) + (y / CELL)) & 1) != 0;
                    U8* p = px + ((size_t)y * TW + x) * 4;
                    if (a) { p[0]=255; p[1]=120; p[2]=10;  } // orange
                    else   { p[0]=10;  p[1]=200; p[2]=210; } // cyan
                    p[3] = 255;
                }

            // Register through the LL texture system exactly as a world texture
            // would resolve (uploadRealMaterialTextures() calls
            // getFetchedTexture(uuid) -> getTextureData() -> getVKCpuData()).
            LLViewerFetchedTexture* tex =
                LLViewerTextureManager::getFetchedTexture(raw.get(), FTT_LOCAL_FILE,
                                                          /*usemipmaps=*/false);
            if (tex)
            {
                // VK mode: setFromRaw creates no GL object; it retains the raw
                // pixels as LLTextureData::mVKCpuData for the VK backend to upload.
                tex->setFromRaw(0, raw.get());
                LLTextureData* glimg = tex->getTextureData();
                LLImageRaw* vkcpu = glimg ? glimg->getVKCpuData() : nullptr;
                if (vkcpu && vkcpu->getData())
                {
                    LLCleanMaterialStore::MaterialDesc md;
                    md.base_color_id = tex->getID();   // random UUID from the raw ctor
                    checker_mat_id = LLCleanMaterialStore::instance().getOrCreate(md);
                    checker_ok = (checker_mat_id != 0);
                }
            }
        }
        if (!checker_ok)
        {
            // HONESTY GATE: if the pixels didn't reach getVKCpuData(), do NOT
            // silently fall back to an untextured cube and pretend success.
            LL_WARNS("Vulkan") << "vkSynthInjectCube: checkerboard did NOT reach "
                                  "getVKCpuData(); cube will render UNTEXTURED "
                                  "(milestone NOT satisfied)" << LL_ENDL;
        }
        else
        {
            LL_INFOS("Vulkan") << "vkSynthInjectCube: checkerboard registered, "
                               << "material_id=" << checker_mat_id << LL_ENDL;
        }
    }

    // Build a unit box volume (default LLVolumeParams == square profile / line
    // path == box). is_unique=true forces face generation.
    LLVolumeParams params;                        // defaults to a box
    LLPointer<LLVolume> vol = new LLVolume(params, /*detail=*/1.f,
                                           /*generate_single_face=*/false,
                                           /*is_unique=*/true);
    const S32 nfaces = vol->getNumVolumeFaces();
    if (nfaces <= 0)
    {
        LL_WARNS("Vulkan") << "vkSynthInjectCube: box volume has no faces" << LL_ENDL;
        return 0;
    }
    for (S32 f = 0; f < nfaces; ++f) vol->genTangents(f);

    // A stable synthetic asset id + LOD. capture() ingests every face into its
    // own slot keyed by (mesh_id, lod, face_idx) and populates the CPU mirror.
    const LLUUID mesh_id("5ca7b0ce-0000-4000-8000-000000000029");
    const S32 lod = 3;
    LLCleanMeshAsset::instance().capture(mesh_id, lod, vol.get(), /*force=*/true);

    // World transform: box local coords are ~[-0.5,0.5]; scale it up to 2m.
    // DEFAULT: origin (the fixed close camera frames it -- the textured-cube proof).
    // REGION-COORD REPRO (SLIPSTREAM_VK_SYNTH_REGION set): place the cube at the LIVE
    // instance coords from the in-world CULL diag (104,104,22) and view it from the
    // logged live camera (241.9,20.2,27.7) ~161m away, so we can reproduce/verify the
    // far-clip cull rejection headlessly (see the vkSynthBuildCamera call below).
    const bool region_mode = (sl_getenv("VK_SYNTH_REGION") != nullptr);
    const F32 S = region_mode ? 10.0f : 2.0f;   // big enough to read clearly at 161m
    const F32 ox = region_mode ? 104.f : 0.f;
    const F32 oy = region_mode ? 104.f : 0.f;
    const F32 oz = region_mode ? 22.f  : 0.f;
    F32 model[16] = {
        S,   0.f, 0.f, 0.f,
        0.f, S,   0.f, 0.f,
        0.f, 0.f, S,   0.f,
        ox,  oy,  oz,  1.f
    };
    // TEXTURED MILESTONE: tint is WHITE so the sampled checker shows through
    // PURE (final = base_color_factor(white) * tint(white) * texture(checker)).
    // The per-face tint path was already proven by the prior vertex-color cube;
    // this milestone isolates real texture sampling, so we don't want tint
    // recoloring the checker and muddying the proof. If the checker failed to
    // register, checker_mat_id stayed 0 (default white material) and per-face
    // fallback tints below keep the faces distinguishable for diagnosis.
    const F32 tints[6][3] = {
        {0.90f,0.25f,0.25f},{0.25f,0.85f,0.35f},{0.30f,0.45f,0.95f},
        {0.95f,0.85f,0.30f},{0.85f,0.35f,0.85f},{0.30f,0.85f,0.90f}
    };

    U32 made = 0;
    for (S32 f = 0; f < nfaces; ++f)
    {
        LLCleanMeshAsset::Slot slot =
            LLCleanMeshAsset::instance().getSlot(mesh_id, lod, f);
        if (!slot.valid) continue;

        // Publish this face's slot at ALL 4 LOD levels so the cull's distance-
        // based level selection always resolves to a resident slot. The group id
        // is stable per (mesh_id, face_idx); keep the last as lod_group_id.
        //
        // LOD-RESIDENCY MISMATCH REPRO (SLIPSTREAM_VK_SYNTH_LOD3ONLY): publish the slot
        // at level 3 ONLY, mimicking the real upload-once ingest that captures just
        // the single LOD LL currently has loaded. Combined with region mode (cube
        // 161m away), the cull's distance-derived level is 0 -> slot[0]==SENTINEL.
        // BEFORE the residency fallback this dropped the prim (empty); AFTER it falls
        // back to the level-3 slot and renders. Headless before/after proof.
        static const bool s_lod3only = (sl_getenv("VK_SYNTH_LOD3ONLY") != nullptr);
        U32 group = 0xFFFFFFFFu;
        if (s_lod3only)
            group = LLCleanMeshAsset::instance().registerLodSlot(mesh_id, f, 3, slot.slot_id);
        else
            for (S32 L = 0; L < 4; ++L)
                group = LLCleanMeshAsset::instance().registerLodSlot(mesh_id, f, L, slot.slot_id);

        LLCleanInstanceStore::Instance inst;
        std::memset(&inst, 0, sizeof(inst));
        std::memcpy(inst.model_matrix, model, sizeof(model));
        // AABB in volume-local space (bounds capture() measured for this slot).
        inst.aabb_min[0]=slot.cap_lo[0]; inst.aabb_min[1]=slot.cap_lo[1]; inst.aabb_min[2]=slot.cap_lo[2];
        inst.aabb_max[0]=slot.cap_hi[0]; inst.aabb_max[1]=slot.cap_hi[1]; inst.aabb_max[2]=slot.cap_hi[2];
        inst.aabb_min[3]=2.0f;   // LOD radius (aabb_min.w); nonzero => sane LOD select
        if (checker_ok)
        {   // WHITE tint => pure checker sample (see tints[] comment above)
            inst.tint[0]=inst.tint[1]=inst.tint[2]=inst.tint[3]=1.f;
        }
        else
        {   // fallback: distinct per-face tints so an untextured cube is still readable
            inst.tint[0]=tints[f%6][0]; inst.tint[1]=tints[f%6][1]; inst.tint[2]=tints[f%6][2]; inst.tint[3]=1.f;
        }
        inst.uv_matrix[0]=1.f; inst.uv_matrix[5]=1.f; inst.uv_matrix[10]=1.f; inst.uv_matrix[15]=1.f;
        inst.lod_group_id = group;
        inst.material_id  = checker_mat_id;   // real checkerboard material (or 0 = default white)
        inst.flags        = 0;             // STATIC OPAQUE (bucket 0)
        inst.skin_id      = 0xFFFFFFFFu;

        // Distinct object key per face so each is its own instance row.
        LLUUID okey = mesh_id; okey.mData[15] = (U8)f;
        LLCleanInstanceStore::instance().upsert(okey, f, inst, /*is_active=*/false, /*sig=*/0);
        ++made;
    }

    // Camera. DEFAULT: close oblique framing of the origin cube (textured proof).
    // REGION MODE: reproduce the live scenario -- eye at the logged live camera pos,
    // looking at the region cube 161m away, with the SAME projection math as
    // buildVulkanWorldFrame. SLIPSTREAM_VK_SYNTH_FAR overrides the far plane so we can
    // A/B the far-clip cull rejection (far=64 -> cube culled; far=256 -> cube renders).
    if (region_mode)
    {
        const char* fenv = sl_getenv("VK_SYNTH_FAR");
        const F32 zf = fenv ? (F32)atof(fenv) : 64.f;   // default 64 = DEFAULT_FAR_PLANE
        const F32 fovY   = 60.f * 0.017453292f;
        const F32 aspect = 1.7778f;                     // 16:9-ish, like the live view
        const F32 zn     = 0.25f;                        // DEFAULT_NEAR_PLANE
        vkSynthBuildCamera(cam, LLVector3(241.9f, 20.2f, 27.7f),
                           LLVector3(ox, oy, oz), fovY, aspect, zn, zf);
        LL_INFOS("Vulkan") << "vkSynthInjectCube REGION: eye=(241.9,20.2,27.7) "
                              "target=(" << ox << "," << oy << "," << oz << ") far=" << zf
                           << " dist=" << (LLVector3(241.9f,20.2f,27.7f)-LLVector3(ox,oy,oz)).length()
                           << LL_ENDL;
    }
    else
    {
        vkSynthBuildCamera(cam, LLVector3(5.f, 5.f, 3.5f), LLVector3(0.f, 0.f, 0.f));
    }

    LL_INFOS("Vulkan") << "vkSynthInjectCube: faces=" << nfaces
                       << " instances=" << made << LL_ENDL;
    return made;
}

// Column-major 4x4 inverse (glu form). in/out share the same storage order, so
// inverting the MVP we push yields a clip->world matrix consistent with it. On a
// singular matrix (det==0) falls back to identity so the water FS is a harmless no-op.
static void vkInvertMat4(const F32* m, F32* invOut)
{
    F32 inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    F32 det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.f)
    {
        static const F32 I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(invOut, I, sizeof(I));
        return;
    }
    det = 1.f / det;
    for (int i = 0; i < 16; ++i) invOut[i] = inv[i] * det;
}

// Build the world-render frame inputs (view-projection + frustum planes + LOD
// camera) from the REAL LLViewerCamera, in the convention the VK GPU-driven
// static-opaque pass expects: column-major world->clip MVP with Vulkan NDC
// (depth 0..1, Y down), and Gribb/Hartmann planes extracted from it (inside if
// dot(n,p)+d >= 0). Returns false (world disabled this frame) until the agent is
// actually in-world, so the login screen stays UI-only.
// ---- PLANAR MIRROR (LL hero reflection probe) --------------------------------------
// Picks the mirror like LLHeroProbeManager::update (nearest visible box probe flagged
// mirror, +Z normal through its centre), then builds the reflected view: V * R where R
// reflects about the mirror plane (det -1, so the capture draws with clockwise front
// faces), an oblique near plane on the mirror (Lengyel, VK [0,1] depth) so nothing
// behind the mirror leaks in, cull planes whose near plane IS the mirror plane, and the
// world -> probe-box matrix the main view uses to localise the reflection. V and P are
// the main view (P already Y-flipped for Vulkan clip space).
// Offscreen snapshots render at their own size: pixel-size terms follow it.
static S32 sVKViewHeightOverride = 0;
static F32 vkViewHeightPx()
{
    if (sVKViewHeightOverride > 0) return (F32)sVKViewHeightOverride;
    return gViewerWindow ? (F32)gViewerWindow->getWindowHeightRaw() : 1080.f;
}

static void buildVKMirror(LLVKWorldFrame& wf, const F32 V[16], const F32 P[16], const LLVector3& eye)
{
    wf.mirror_valid = false;
    wf.mirror_key = 0;
    wf.mirror_cam_inside = false;
    static LLCachedControl<bool> s_mirrors(gSavedSettings, "RenderMirrors", false);
    static LLCachedControl<bool> s_vk_mirrors(gSavedSettings, "RenderVKMirrors", true);
    static LLCachedControl<F32>  s_min_px(gSavedSettings, "RenderMirrorsPlanarMinPixels", 32.f);
    static LLCachedControl<F32>  s_scale(gSavedSettings, "RenderVKMirrorResScale", 0.75f);
    static LLCachedControl<S32>  s_hero_res(gSavedSettings, "RenderHeroProbeResolution", 1024);
    static LLCachedControl<F32>  s_hero_dist(gSavedSettings, "RenderHeroProbeDistance", 8.f);
    static LLCachedControl<S32>  s_probe_detail(gSavedSettings, "RenderReflectionProbeDetail", 1);
    // LLHeroProbeManager::renderProbes reads this (no settings.xml entry: default -1).
    static LLCachedControl<S32>  s_hero_detail(gSavedSettings, "RenderHeroReflectionProbeDetail", -1);
    if (!s_mirrors || !s_vk_mirrors || gTeleportDisplay) return;
    const auto& heroes = gPipeline.mHeroProbeManager.getHeroList();
    if (heroes.empty()) return;

    auto sphereVisible = [&](const LLVector3& c, F32 r) -> bool
    {
        for (int i = 0; i < 6; ++i)
        {
            const F32* pl = wf.frustum_planes[i];
            if (pl[0]*c.mV[0] + pl[1]*c.mV[1] + pl[2]*c.mV[2] + pl[3] < -r) return false;
        }
        return true;
    };
    const LLVector3 fwd(-V[2], -V[6], -V[10]);   // camera forward (world)
    LLVOVolume* best = nullptr;
    F32 best_dist = 1e30f, best_center = 1e30f;
    for (const LLPointer<LLVOVolume>& vp : heroes)
    {
        LLVOVolume* vo = vp.get();
        if (!vo || vo->isDead() || vo->mDrawable.isNull() || !vo->isReflectionProbe()
            || !vo->getReflectionProbeIsBox())
            continue;
        const LLVector3 pos = vo->getPositionAgent();
        const F32 r = vo->getScale().magVec() * 0.5f;
        const F32 dist = (eye - pos).magVec();
        const F32 center = fwd * (pos - eye);
        if (dist > 512.f || !sphereVisible(pos, r)) continue;
        // LL: nearest AND nearest along the view direction
        if (dist < best_dist && center < best_center)
        {
            best = vo; best_dist = dist; best_center = center;
        }
    }
    if (!best) return;

    const LLVector3 pos = best->getPositionAgent();
    const LLQuaternion rot = best->getRotationRegion();
    LLVector3 n = LLVector3::z_axis * rot;
    n.normalize();
    const F32 d = -(n * pos);
    const F32 side = n * eye + d;
    if (side <= 0.01f) return;   // camera behind (or on) the one-sided mirror

    // Sub-threshold on screen: skip (the capture is only ever sampled on its pixels).
    F32 mirror_px = 0.f;
    {
        const F32 r = best->getScale().magVec() * 0.5f;
        const F32 vp_h = vkViewHeightPx();
        const F32 px = (best_dist > 0.01f) ? (2.f * r / best_dist) * (vp_h * 0.5f) * std::fabs(P[5]) : 1e6f;
        if ((F32)s_min_px > 0.f && px < (F32)s_min_px) return;
        mirror_px = px;
    }

    // Reflection about the plane: X' = X - 2 (n.X + d) n (column-major, M[c*4+r]).
    F32 R[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            R[c*4 + r] = (r == c ? 1.f : 0.f) - 2.f * n.mV[r] * n.mV[c];
    for (int r = 0; r < 3; ++r) R[12 + r] = -2.f * d * n.mV[r];
    R[15] = 1.f;
    auto mul = [](const F32* A, const F32* B, F32* out)   // out = A * B
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                F32 s = 0.f;
                for (int k = 0; k < 4; ++k) s += A[k*4 + r] * B[c*4 + k];
                out[c*4 + r] = s;
            }
    };
    F32 Vm[16], Vminv[16];
    mul(V, R, Vm);
    vkInvertMat4(Vm, Vminv);

    // RenderHeroProbeDistance ("distance in meters for hero probes to render out to"):
    // the capture's far plane, from the reflected eye (LL's hero probe origin). P is the
    // main conventional projection; only its depth row changes (zn = P[14] / P[10]).
    F32 Pm[16];
    std::memcpy(Pm, P, sizeof(Pm));
    {
        const F32 zn = (std::fabs(P[10]) > 1e-12f) ? P[14] / P[10] : 0.1f;
        const F32 zf = llmax((F32)s_hero_dist, zn + 1.f);
        Pm[10] = zf / (zn - zf);
        Pm[14] = (zn * zf) / (zn - zf);
    }
    P = Pm;

    // Clip plane (keep n.X + d >= eps: the viewer's side; the mirror surface itself,
    // coplanar, is clipped) in the reflected view's eye space: C = Vm^-T * pi.
    const F32 eps = 0.01f;
    const F32 pi[4] = { n.mV[0], n.mV[1], n.mV[2], d - eps };
    F32 C[4];
    for (int i = 0; i < 4; ++i)
    {
        F32 s = 0.f;
        for (int j = 0; j < 4; ++j) s += Vminv[i*4 + j] * pi[j];
        C[i] = s;
    }
    // Oblique near plane (Lengyel), Vulkan [0,1] depth: replace the z row with
    // c = C / (C . q), q = P^-1 * (sgn(Cc.x), sgn(Cc.y), 1, 1), Cc = P^-T * C.
    F32 Pinv[16];
    vkInvertMat4(P, Pinv);
    F32 Cc[4];
    for (int i = 0; i < 4; ++i)
    {
        F32 s = 0.f;
        for (int j = 0; j < 4; ++j) s += Pinv[i*4 + j] * C[j];
        Cc[i] = s;
    }
    const F32 Qc[4] = { Cc[0] >= 0.f ? 1.f : -1.f, Cc[1] >= 0.f ? 1.f : -1.f, 1.f, 1.f };
    F32 q[4];
    for (int r = 0; r < 4; ++r)
    {
        F32 s = 0.f;
        for (int k = 0; k < 4; ++k) s += Pinv[k*4 + r] * Qc[k];
        q[r] = s;
    }
    const F32 cq = C[0]*q[0] + C[1]*q[1] + C[2]*q[2] + C[3]*q[3];
    if (std::fabs(cq) < 1e-8f) return;
    F32 Pobl[16];
    std::memcpy(Pobl, P, sizeof(Pobl));
    for (int k = 0; k < 4; ++k) Pobl[k*4 + 2] = C[k] / cq;

    mul(Pobl, Vm, wf.mirror_view_proj);
    mul(P, Vm, wf.mirror_view_proj_sky);
    // Translation-free twin (#555): Vm's eye is the reflected eye (rcam below), so its
    // 3x3 alone maps (world - rcam) -- the mirror sky's rays without km-sized terms.
    {
        F32 Vr[16];
        std::memcpy(Vr, Vm, sizeof(Vr));
        Vr[12] = Vr[13] = Vr[14] = 0.f;
        mul(P, Vr, wf.mirror_view_proj_sky_rel);
        // The capture's draws alike: the oblique projection works in Vm's eye space,
        // which dropping the translation leaves unchanged.
        mul(Pobl, Vr, wf.mirror_view_proj_rel);
    }

    // Cull planes from the non-oblique reflected frustum; the near plane is the
    // mirror plane itself (slightly relaxed: conservative).
    {
        const F32* M = wf.mirror_view_proj_sky;
        auto el = [&](int col, int row){ return M[col*4 + row]; };
        F32 rr[4][4];
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) rr[r][c] = el(c, r);
        auto setp = [&](int idx, const F32 p4[4])
        {
            F32 a = p4[0], b = p4[1], c = p4[2], dd = p4[3];
            const F32 len = std::sqrt(a*a + b*b + c*c);
            if (len > 0.f) { a /= len; b /= len; c /= len; dd /= len; }
            wf.mirror_frustum[idx][0] = a; wf.mirror_frustum[idx][1] = b;
            wf.mirror_frustum[idx][2] = c; wf.mirror_frustum[idx][3] = dd;
        };
        F32 t[4];
        for (int c = 0; c < 4; ++c) t[c] = rr[3][c] + rr[0][c]; setp(0, t);
        for (int c = 0; c < 4; ++c) t[c] = rr[3][c] - rr[0][c]; setp(1, t);
        for (int c = 0; c < 4; ++c) t[c] = rr[3][c] + rr[1][c]; setp(2, t);
        for (int c = 0; c < 4; ++c) t[c] = rr[3][c] - rr[1][c]; setp(3, t);
        const F32 nearp[4] = { n.mV[0], n.mV[1], n.mV[2], d + 0.05f };
        setp(4, nearp);
        for (int c = 0; c < 4; ++c) t[c] = rr[3][c] - rr[2][c]; setp(5, t);
    }

    const LLVector3 rcam = eye - n * (2.f * side);
    wf.mirror_cam[0] = rcam.mV[0]; wf.mirror_cam[1] = rcam.mV[1]; wf.mirror_cam[2] = rcam.mV[2];
    wf.mirror_cam[3] = 1.f;
    wf.mirror_plane[0] = n.mV[0]; wf.mirror_plane[1] = n.mV[1]; wf.mirror_plane[2] = n.mV[2];
    wf.mirror_plane[3] = d;

    // World -> probe box [-1,1]^3: local_r = (axis_r . (X - pos)) / half_scale_r.
    {
        const LLVector3 ax[3] = { LLVector3::x_axis * rot, LLVector3::y_axis * rot, LLVector3::z_axis * rot };
        const LLVector3 sc = best->getScale();
        F32* B = wf.mirror_box;
        std::memset(B, 0, sizeof(wf.mirror_box));
        for (int r = 0; r < 3; ++r)
        {
            const F32 hs = llmax(sc.mV[r] * 0.5f, 0.001f);
            for (int c = 0; c < 3; ++c) B[c*4 + r] = ax[r].mV[c] / hs;
            B[12 + r] = -(ax[r] * pos) / hs;
        }
        B[15] = 1.f;

        // Eye inside the probe box, inflated per axis by a near-clip margin in world
        // metres: the occlusion proxy box would be near-plane clipped there, so the
        // renderer treats it as visible without consulting the query.
        const F32 near_m = llmax(LLViewerCamera::getInstance()->getNear(), 0.1f) + 0.25f;
        const LLVector3 rel = eye - pos;
        bool inside = true;
        for (int r = 0; r < 3 && inside; ++r)
            inside = std::fabs(ax[r] * rel) <= sc.mV[r] * 0.5f + near_m;
        wf.mirror_cam_inside = inside;
    }
    wf.mirror_key = (uint64_t)(uintptr_t)best;
    wf.mirror_local_id = best->getLocalID();
    std::memcpy(wf.mirror_uuid, best->getID().mData, sizeof(wf.mirror_uuid));
    wf.mirror_dist = best_dist;
    wf.mirror_size = best->getScale().magVec();
    wf.mirror_px = mirror_px;
    wf.mirror_res_scale = llclamp((F32)s_scale, 0.25f, 1.f);
    // LL renders each hero face at RenderHeroProbeResolution over ~90 degrees -- about the
    // main view's horizontal field: the planar capture's long side is capped at it.
    wf.mirror_max_res = (uint32_t)llclamp((S32)s_hero_res, 64, 8192);
    // LL: renderProbes passes force_dynamic = Dynamic flag && RenderHeroReflectionProbeDetail
    // > 0, and LLReflectionMap::update ORs getIsDynamic() (Dynamic flag &&
    // RenderReflectionProbeDetail > STATIC_ONLY).
    wf.mirror_dynamic = best->getReflectionProbeIsDynamic()
                        && ((S32)s_probe_detail > 0 || (S32)s_hero_detail > 0);

    // Unchanged view + mirror since last frame (lets the renderer honor
    // RenderHeroProbeUpdateRate without the capture swimming under a moving camera).
    static F32 s_last[16] = {};
    wf.mirror_static = std::memcmp(s_last, wf.mirror_view_proj, sizeof(s_last)) == 0;
    std::memcpy(s_last, wf.mirror_view_proj, sizeof(s_last));
    wf.mirror_valid = true;
}

// Depth of field focus + lens terms, ported from LL's LLPipeline::renderDoF (the
// focal-plane choice, the eased transition and the thin-lens CoF constants). The
// VK renderer runs the passes (LLCleanRendererVK::recordDoF).
static void buildVKDoF(LLVKWorldFrame& wf, F32 fov, const LLVector3& eye, const LLVector3& at_axis)
{
    static LLCachedControl<bool> s_dof(gSavedSettings, "RenderDepthOfField", false);
    static LLCachedControl<bool> s_dof_edit(gSavedSettings, "RenderDepthOfFieldInEditMode", false);
    static LLCachedControl<F32>  s_transition(gSavedSettings, "CameraFocusTransitionTime", 0.5f);
    static LLCachedControl<F32>  s_fnumber(gSavedSettings, "CameraFNumber", 9.f);
    static LLCachedControl<F32>  s_focal_length(gSavedSettings, "CameraFocalLength", 50.f);
    static LLCachedControl<F32>  s_fov(gSavedSettings, "CameraFieldOfView", 60.f);
    static LLCachedControl<F32>  s_max_cof(gSavedSettings, "CameraMaxCoF", 10.f);
    static LLCachedControl<F32>  s_res_scale(gSavedSettings, "CameraDoFResScale", 0.7f);

    wf.dof_enabled = s_dof && (s_dof_edit || !LLToolMgr::getInstance()->inBuildMode());
    if (!wf.dof_enabled)
        return;

    static F32 current_distance = 16.f;
    static F32 start_distance = 16.f;
    static F32 transition_time = 1.f;

    // Black Dragon focus modes: CameraDoFLocked keeps the last focus point,
    // CameraFreeDoFFocus focuses on whatever is under the mouse cursor.
    // Lightbox (upstream Alchemy) names the same two modes RenderFocusPointLocked /
    // RenderFocusPointFollowsPointer; either spelling turns the mode on.
    static LLCachedControl<bool> s_bd_free_focus(gSavedSettings, "CameraFreeDoFFocus", false);
    static LLCachedControl<bool> s_bd_locked(gSavedSettings, "CameraDoFLocked", false);
    static LLCachedControl<bool> s_al_free_focus(gSavedSettings, "RenderFocusPointFollowsPointer", false);
    static LLCachedControl<bool> s_al_locked(gSavedSettings, "RenderFocusPointLocked", false);
    const bool s_free_focus = s_bd_free_focus || s_al_free_focus;
    const bool s_locked = s_bd_locked || s_al_locked;
    static LLVector3 s_prev_focus;

    LLVector3 focus_point;
    LLViewerObject* obj = LLViewerMediaFocus::getInstance()->getFocusedObject();
    if (obj && obj->mDrawable && obj->isSelected())
    {   // focus on selected media object
        LLFace* face = obj->mDrawable->getFace(LLViewerMediaFocus::getInstance()->getFocusedFace());
        if (face)
            focus_point = face->getPositionAgent();
    }
    if (s_locked && !s_prev_focus.isExactlyZero())
        focus_point = s_prev_focus;
    if (focus_point.isExactlyZero() && (gAgent.isCameraExternallyDriven() || s_free_focus))
    {   // focus on point under cursor (#724): the GPU cursor pick's newest answer for the
        // cursor ray (the crosshair in mouselook), never a synchronous pick. LL read the
        // last hit cursorIntersect left in gDebugRaycastIntersection, which only the
        // CameraFreeDoFFocus spelling kept from being reset to the ray's far end (512 m:
        // no visible blur) -- the Lightbox "Focus follows pointer" box lost the effect.
        // Over the UI or with no answer yet, the last point holds; over sky (the whole ray
        // answered empty), the camera focus below.
        static LLVector3 s_pointer_focus;
        static bool s_pointer_valid = false;
        static const LLViewerRegion* s_pointer_region = nullptr;
        const LLViewerRegion* region = gAgent.getRegion();
        if (region != s_pointer_region || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {   // agent space moved: the held point means nothing now
            s_pointer_valid = false;
            s_pointer_region = region;
        }
        if (gViewerWindow && (gViewerWindow->getPointerOverWorld() || gAgentCamera.cameraMouselook()))
        {
            LLVector3 p;
            switch (LLCleanCursorPick::focusPoint(p))
            {
                case LLCleanCursorPick::FOCUS_HIT:  s_pointer_focus = p; s_pointer_valid = true; break;
                case LLCleanCursorPick::FOCUS_NONE: s_pointer_valid = false; break;
                default: break;
            }
        }
        if (s_pointer_valid)
            focus_point = s_pointer_focus;
    }
    if (focus_point.isExactlyZero())
    {
        if (gAgentCamera.cameraMouselook())
        {   // focus on point under mouselook crosshairs
            LLVector4a result;
            result.clear();
            gViewerWindow->cursorIntersect(-1, -1, 512.f, nullptr, -1, false, false, true, true, nullptr, nullptr, nullptr, &result);
            focus_point.set(result.getF32ptr());
        }
        else if (LLViewerRegion* region = gAgent.getRegion())
        {   // focus on alt-zoom target
            focus_point = LLVector3(gAgentCamera.getFocusGlobal() - region->getOriginGlobal());
        }
    }

    F32 target_distance = 16.f;
    if (!focus_point.isExactlyZero())
    {
        target_distance = at_axis * (focus_point - eye);
        if (!s_locked)
            s_prev_focus = focus_point;
    }

    if (transition_time >= 1.f && fabsf(current_distance - target_distance) / current_distance > 0.01f)
    {   // large shift happened, interpolate smoothly to new target distance
        transition_time = 0.f;
        start_distance = current_distance;
    }
    else if (transition_time < 1.f)
    {   // currently in a transition, continue interpolating
        transition_time += 1.f / llmax((F32)s_transition, 0.001f) * gFrameIntervalSeconds.value();
        transition_time = llmin(transition_time, 1.f);
        F32 t = cosf(transition_time * F_PI + F_PI) * 0.5f + 0.5f;
        current_distance = start_distance + (target_distance - start_distance) * t;
    }
    else
    {   // small or no change, just snap to target distance
        current_distance = target_distance;
    }

    // thin lens (LL): c = |s2-s1|/s2 * f^2/(N(S1-f)), all in mm
    const F32 subject_distance = current_distance * 1000.f;
    const F32 default_fov = s_fov * F_PI / 180.f;
    const F32 dv = 2.f * s_focal_length * tanf(default_fov / 2.f);
    const F32 focal_length = dv / (2.f * tanf(fov / 2.f));
    F32 blur_constant = focal_length * focal_length / (s_fnumber * (subject_distance - focal_length));
    blur_constant /= 1000.f;   // meters for the shader
    const F32 magnification = focal_length / (subject_distance - focal_length);
    // LLDrawable::sCurPixelAngle = window height / fov (pixels per radian)
    const F32 pixel_angle = vkViewHeightPx() / fov;

    wf.dof_lens[0] = -subject_distance / 1000.f;
    wf.dof_lens[1] = blur_constant;
    wf.dof_lens[2] = tanf(1.f / pixel_angle);
    wf.dof_lens[3] = magnification;
    wf.dof_params[0] = llmax((F32)s_max_cof, 0.01f);
    wf.dof_params[1] = llclamp((F32)s_res_scale, 0.1f, 1.f);
}

// #479 frozen culling camera: the GPU cull planes for a pose, built exactly as
// buildVulkanWorldFrame builds them for the live camera (reversed-Z P * V, Gribb/Hartmann,
// far plane at the draw distance zf_cull). Planes: ax+by+cz+d >= 0 inside.
static void vkCullPlanesForPose(const LLVector3& eye, const LLVector3& at, const LLVector3& left,
                                const LLVector3& up, F32 fovY, F32 aspect, F32 zn, F32 zf, F32 zf_cull,
                                F32 out[6][4])
{
    const LLVector3 right = -left;
    const F32 tx = -(right * eye);
    const F32 ty = -(up * eye);
    const F32 tz =  (at * eye);
    const F32 V[16] = {
        right.mV[0], up.mV[0], -at.mV[0], 0.f,
        right.mV[1], up.mV[1], -at.mV[1], 0.f,
        right.mV[2], up.mV[2], -at.mV[2], 0.f,
        tx,          ty,       tz,        1.f
    };
    const F32 f = 1.0f / std::tan(fovY * 0.5f);
    F32 P[16] = {
        f/aspect, 0.f,  0.f,                 0.f,
        0.f,      f,    0.f,                 0.f,
        0.f,      0.f,  zn/(zf-zn),         -1.f,
        0.f,      0.f,  (zn*zf)/(zf-zn),     0.f
    };
    P[1] = -P[1]; P[5] = -P[5]; P[9] = -P[9]; P[13] = -P[13];   // Vulkan Y-flip
    F32 M[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            F32 s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += P[k*4 + r] * V[c*4 + k];
            M[c*4 + r] = s;
        }
    auto Mel = [&](int col, int row){ return M[col*4 + row]; };
    const F32 r0[4] = { Mel(0,0), Mel(1,0), Mel(2,0), Mel(3,0) };
    const F32 r1[4] = { Mel(0,1), Mel(1,1), Mel(2,1), Mel(3,1) };
    const F32 r3[4] = { Mel(0,3), Mel(1,3), Mel(2,3), Mel(3,3) };
    const F32 r2[4] = { Mel(0,2), Mel(1,2), Mel(2,2), Mel(3,2) };
    auto setPlane = [&](int idx, F32 a, F32 b, F32 c, F32 d){
        const F32 len = std::sqrt(a*a + b*b + c*c);
        if (len > 0.f) { a/=len; b/=len; c/=len; d/=len; }
        out[idx][0]=a; out[idx][1]=b; out[idx][2]=c; out[idx][3]=d;
    };
    setPlane(0, r3[0]+r0[0], r3[1]+r0[1], r3[2]+r0[2], r3[3]+r0[3]); // left
    setPlane(1, r3[0]-r0[0], r3[1]-r0[1], r3[2]-r0[2], r3[3]-r0[3]); // right
    setPlane(2, r3[0]+r1[0], r3[1]+r1[1], r3[2]+r1[2], r3[3]+r1[3]); // bottom
    setPlane(3, r3[0]-r1[0], r3[1]-r1[1], r3[2]-r1[2], r3[3]-r1[3]); // top
    setPlane(4, r3[0]-r2[0], r3[1]-r2[1], r3[2]-r2[2], r3[3]-r2[3]); // near (reversed-Z)
    setPlane(5, -at.mV[0], -at.mV[1], -at.mV[2], at * eye + zf_cull); // far = draw distance
}

static bool buildVulkanWorldFrame(LLVKWorldFrame& wf)
{
    wf.enabled = false;
    if (LLStartUp::getStartupState() < STATE_STARTED)
        return false;

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    if (!cam)
        return false;

    // Camera frame. In SL the coord frame is X=at, Y=left, Z=up.
    const LLVector3 eye  = cam->getOrigin();
    const LLVector3 at   = cam->getAtAxis();     // forward
    const LLVector3 left = cam->getLeftAxis();   // +Y
    const LLVector3 up   = cam->getUpAxis();     // +Z
    const LLVector3 right = -left;               // camera-space +X points right

    // View matrix (world -> view), column-major. Rows are (right, up, -forward).
    // v = R * (p - eye).
    const F32 fwd[3]  = { at.mV[0],  at.mV[1],  at.mV[2] };
    const F32 rt[3]   = { right.mV[0], right.mV[1], right.mV[2] };
    const F32 upv[3]  = { up.mV[0],  up.mV[1],  up.mV[2] };
    const F32 tx = -(rt[0]*eye.mV[0]  + rt[1]*eye.mV[1]  + rt[2]*eye.mV[2]);
    const F32 ty = -(upv[0]*eye.mV[0] + upv[1]*eye.mV[1] + upv[2]*eye.mV[2]);
    const F32 tz =  (fwd[0]*eye.mV[0] + fwd[1]*eye.mV[1] + fwd[2]*eye.mV[2]); // -(-fwd . eye)

    // column-major V
    F32 V[16] = {
        rt[0],  upv[0], -fwd[0], 0.f,
        rt[1],  upv[1], -fwd[1], 0.f,
        rt[2],  upv[2], -fwd[2], 0.f,
        tx,     ty,     tz,      1.f
    };

    // Perspective projection, Vulkan NDC (z in [0,1]). Y is flipped below.
    //
    // ROOT CAUSE OF "only 1-2 things visible": the per-frame projection update
    // (setFar from RenderFarClip, setAspect, setPerspective) lives in the diverted
    // display() path, so LLViewerCamera::getFar() is STALE -- it sits at
    // DEFAULT_FAR_PLANE (64 m) / the login draw-distance and is never grown to the
    // real RenderFarClip. The compute cull's far plane then rejects everything
    // beyond ~64 m, leaving only the nearest one or two objects. (getView()/getNear()
    // hold valid ctor defaults; getAspect() is set at login/reshape.) Source the
    // params robustly here so the VK frame doesn't depend on display() having run.
    static LLCachedControl<F32> s_render_far(gSavedSettings, "RenderFarClip", 128.f);
    // EFFECTIVE draw distance: the VRAM ladder's far-geometry rung (#405) may hold it below
    // the setting (then the cull far, light clusters and shadow range follow it).
    const F32 render_far = LLVulkanContext::instance().vramDrawDistance((F32)s_render_far);
    F32 fovY   = cam->getView();
    if (!(fovY > 0.05f && fovY < 3.13f)) fovY = 60.f * 0.017453292f;   // sane FOV
    F32 aspect = cam->getAspect();
    if (!(aspect > 0.05f))
        aspect = (gViewerWindow ? gViewerWindow->getWorldViewAspectRatio() : 1.7778f);
    if (!(aspect > 0.05f)) aspect = 1.7778f;
    F32 zn     = cam->getNear();
    if (!(zn > 0.01f)) zn = 0.25f;
    // Far (cull) = the draw distance, RenderFarClip, read directly: the camera far is
    // stale here (see above), which is what the OLD [64,1024] clamp tripped over. It
    // used to be floored at 512 m, which silently overrode every draw distance below
    // that (default 256 m => 4x the area culled-in vs LL, and the draw-distance
    // slider did nothing under 512). Huge objects past it (sim surrounds, domes) still
    // pass: the cull tests AABBs, and the projection far is extended below.
    F32 zf     = llclamp(render_far, 8.f, 8192.f);
    if (zf <= zn + 1.f) zf = zn + 256.f;
    // CULL far vs PROJECTION far (LL parity). LL culls objects by draw distance with
    // an AABB test (the octree / camera far = mDrawDistance), but rasterizes with a
    // projection whose far plane is FIXED at MAX_FAR_CLIP*2 = 1024 m regardless of draw
    // distance (LLViewerWindow::setup3DRender). So a huge object whose bounds overlap
    // the draw distance -- a 1.5 km sim-surround / weather dome around the camera --
    // passes the cull and its faces out to 1024 m are drawn. Using the cull distance
    // as the projection far (512 m) made the rasterizer clip every face of a ~750 m
    // radius dome: it passed the cull and drew NOTHING. Keep zf as the cull distance
    // (frustum plane 5 is rebuilt from it below) and extend only the projection.
    // Draw distance now reaches 4096 m (#dd4k), past MAX_FAR_CLIP (512): keep LL's 2x
    // cull->projection ratio (512 -> 1024) instead of the fixed 1024 floor, so a 4 km
    // draw distance rasterizes out to 8 km (big objects overlapping the cull distance
    // still draw their far faces). Reversed-Z keeps the precision.
    const F32 zf_cull = zf;
    zf = llmax(zf_cull * 2.f, MAX_FAR_CLIP * 2.f);
    const F32 f      = 1.0f / std::tan(fovY * 0.5f);
    // column-major P (maps view space, -Z forward, to clip). REVERSED-Z (#188): near
    // -> depth 1, far -> 0, so float depth keeps ~constant relative precision with
    // distance (conventional depth resolved ~0.06 mm at 10 m: thin layered attachments
    // fought). Depth readers linearize from P[10] / P[14] or un-project through the
    // inverse, which hold for either convention. Pconv (conventional) feeds the planar
    // mirror, whose oblique near-plane clip and pipelines stay conventional.
    F32 P[16] = {
        f/aspect, 0.f,  0.f,                 0.f,
        0.f,      f,    0.f,                 0.f,
        0.f,      0.f,  zn/(zf-zn),         -1.f,
        0.f,      0.f,  (zn*zf)/(zf-zn),     0.f
    };
    F32 Pconv[16] = {
        f/aspect, 0.f,  0.f,                 0.f,
        0.f,      f,    0.f,                 0.f,
        0.f,      0.f,  zf/(zn-zf),         -1.f,
        0.f,      0.f,  (zn*zf)/(zn-zf),     0.f
    };
    // UI consumers of the "current GL matrices" (prim media controls placement,
    // project_world_to_screen users) read get_current_projection/modelview, which
    // LLViewerCamera::setPerspective sets in the diverted display() path. Publish
    // the VK camera in GL convention (agent space -> clip, z in [-1,1]).
    set_current_projection(glm::perspective(fovY, aspect, zn, zf));
    set_current_modelview(glm::make_mat4(V));

    // Clustered light cull camera terms (view matrix, fov tangents, slice range).
    std::memcpy(wf.view, V, sizeof(wf.view));
    wf.tan_half[1] = 1.0f / f;
    wf.tan_half[0] = aspect / f;
    wf.cluster_near = zn;
    wf.cluster_far  = llmax(zn + 1.f, render_far);
    wf.cam_fwd[0] = fwd[0]; wf.cam_fwd[1] = fwd[1]; wf.cam_fwd[2] = fwd[2];

    // Vulkan Y-flip: negate the second row of P (P[1],P[5],P[9],P[13]).
    P[1] = -P[1]; P[5] = -P[5]; P[9] = -P[9]; P[13] = -P[13];
    Pconv[1] = -Pconv[1]; Pconv[5] = -Pconv[5]; Pconv[9] = -Pconv[9]; Pconv[13] = -Pconv[13];

    std::memcpy(wf.proj, P, sizeof(wf.proj));
    vkInvertMat4(P, wf.inv_proj);

    buildVKDoF(wf, fovY, eye, LLVector3(fwd[0], fwd[1], fwd[2]));

    // Rotation-only view (camera-relative) projection for the rigged pre-skin output.
    {
        F32 Vr[16];
        std::memcpy(Vr, V, sizeof(Vr));
        Vr[12] = Vr[13] = Vr[14] = 0.f;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                F32 s = 0.f;
                for (int k = 0; k < 4; ++k) s += P[k*4 + r] * Vr[c*4 + k];
                wf.view_proj_rel[c*4 + r] = s;
            }
        // Sky view rays (#555): un-projected through the translation-free inverse, so
        // the direction never cancels a km-sized eye against a point ~2 near-planes away.
        vkInvertMat4(wf.view_proj_rel, wf.inv_view_proj_rel);
    }

    // MVP = P * V (column-major multiply).
    F32* M = wf.view_proj;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            F32 s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += P[k*4 + r] * V[c*4 + k];
            M[c*4 + r] = s;
        }

    // Inverse MVP (clip -> world) for VK water depth-fog: un-projects the sampled
    // scene depth back to a world position. Column-major cofactor inverse (glu form);
    // storage order matches M, so it round-trips the exact matrix we push.
    vkInvertMat4(M, wf.inv_view_proj);

    // Frustum planes (Gribb/Hartmann) from the row-form of MVP.
    // rowi = (M[0][i], M[1][i], M[2][i], M[3][i]) using column-major indexing:
    auto Mel = [&](int col, int row){ return M[col*4 + row]; };
    F32 r0[4] = { Mel(0,0), Mel(1,0), Mel(2,0), Mel(3,0) };
    F32 r1[4] = { Mel(0,1), Mel(1,1), Mel(2,1), Mel(3,1) };
    F32 r2[4] = { Mel(0,2), Mel(1,2), Mel(2,2), Mel(3,2) };
    F32 r3[4] = { Mel(0,3), Mel(1,3), Mel(2,3), Mel(3,3) };
    auto setPlane = [&](int idx, F32 a, F32 b, F32 c, F32 d){
        F32 len = std::sqrt(a*a + b*b + c*c);
        if (len > 0.f) { a/=len; b/=len; c/=len; d/=len; }
        wf.frustum_planes[idx][0]=a; wf.frustum_planes[idx][1]=b;
        wf.frustum_planes[idx][2]=c; wf.frustum_planes[idx][3]=d;
    };
    setPlane(0, r3[0]+r0[0], r3[1]+r0[1], r3[2]+r0[2], r3[3]+r0[3]); // left
    setPlane(1, r3[0]-r0[0], r3[1]-r0[1], r3[2]-r0[2], r3[3]-r0[3]); // right
    setPlane(2, r3[0]+r1[0], r3[1]+r1[1], r3[2]+r1[2], r3[3]+r1[3]); // bottom
    setPlane(3, r3[0]-r1[0], r3[1]-r1[1], r3[2]-r1[2], r3[3]-r1[3]); // top
    setPlane(4, r3[0]-r2[0], r3[1]-r2[1], r3[2]-r2[2], r3[3]-r2[3]); // near (reversed-Z: z <= w)
    setPlane(5, r3[0]-r2[0], r3[1]-r2[1], r3[2]-r2[2], r3[3]-r2[3]); // far (projection)
    // Cull far plane at the DRAW distance, not the projection far (see zf_cull above):
    // inside iff dot(fwd, p - eye) <= zf_cull. The compute cull tests AABBs against it,
    // so huge objects overlapping the draw distance still pass (LL behaviour).
    setPlane(5, -fwd[0], -fwd[1], -fwd[2],
             fwd[0]*eye.mV[0] + fwd[1]*eye.mV[1] + fwd[2]*eye.mV[2] + zf_cull);
    // #479 frozen culling camera (Develop > Rendering > Freeze culling camera): the GPU
    // cull frustum and the LOD / texel-demand camera (lod_cam_pos, set below) come from
    // the frozen pose; view, projection, shading eye (cam_pos), lights, shadows, mirror
    // and DoF stay on the live camera, so the view flies free over what was selected.
    LLVector3 lod_eye = eye;
    {
        LLVector3 f_eye, f_at, f_left, f_up;
        F32 f_fov = fovY, f_aspect = aspect, f_near = zn;
        if (LLCullCamera::getFrozenPose(f_eye, f_at, f_left, f_up, f_fov, f_aspect, f_near))
        {
            vkCullPlanesForPose(f_eye, f_at, f_left, f_up, f_fov, f_aspect, f_near, zf, zf_cull, wf.frustum_planes);
            lod_eye = f_eye;
        }
    }
    // Diagnostic: RenderVKCullNoFrustum makes every cull plane non-rejecting, to tell
    // a frustum-test rejection (bad bounds) from LOD/slot causes.
    {
        static LLCachedControl<bool> s_nofrustum(gSavedSettings, "RenderVKCullNoFrustum", false);
        if (s_nofrustum)
            for (int i = 0; i < 6; ++i)
            {
                wf.frustum_planes[i][0] = wf.frustum_planes[i][1] = wf.frustum_planes[i][2] = 0.f;
                wf.frustum_planes[i][3] = 1.f;
            }
    }

    buildVKMirror(wf, V, Pconv, eye);   // conventional depth (see P above)
    // "Show me in Mouselook" (FirstPersonAvatarVisible) off: hide the own avatar in
    // the main view only (renderer keeps it in mirrors, probes and shadows).
    // Picks follow the same rule (LLVOAvatar::isHiddenOwnAttachment, #554).
    wf.hide_self = LLVOAvatar::selfHiddenInMainView();
    // LL draws the own system body's head only when needsRenderHead() (main view; the
    // renderer keeps it in mirrors and shadows as LL's reflection / shadow renders did).
    wf.hide_self_head = !gAgent.needsRenderHead();
    // RLVa @setsphere: also reaches offscreen snapshots, which build their frame here.
    wf.rlv_sphere_count = 0;
    if (RlvActions::hasBehaviour(RLV_BHVR_SETSPHERE))
    {
        RlvSphereRenderParams spheres;
        LLVfxManager::instance().runEffect(EVisualEffect::RlvSphere, &spheres);
        static_assert(RlvSphereRenderParams::MAX_SPHERES == LLVKWorldFrame::kMaxRlvSpheres, "sphere caps");
        for (U32 i = 0; i < spheres.count; ++i)
        {
            const RlvSphereRenderParams::Sphere& src = spheres.spheres[i];
            LLVKWorldFrame::RlvSphere& dst = wf.rlv_spheres[i];
            std::memcpy(dst.origin, src.origin, sizeof(dst.origin));
            dst.mode = src.mode;
            std::memcpy(dst.values, src.values, sizeof(dst.values));
            std::memcpy(dst.params, src.params, sizeof(dst.params));
            dst.extend_min = src.extend_min;
            dst.extend_max = src.extend_max;
        }
        wf.rlv_sphere_count = spheres.count;
    }

    // ---- FUNNEL DIAG (behavior-neutral, throttled ~1/3s): the decisive projection
    // + plane readout. Logs the sourced fovY/aspect/zn/zf and evaluates all 6
    // frustum planes (dot(n,p)+d, "inside" iff >= 0) against TWO probe points: the
    // live in-world instance at (104,104,22) and eye+5*at (right in front of the
    // camera). A negative value at ANY plane = that point is culled by that plane;
    // the far plane (idx 5) going negative for a mid-distance point is the far-clip
    // bug. NaN anywhere = degenerate projection.
    {
        static F64 s_next = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now >= s_next)
        {
            s_next = now + 3.0;
            const LLVector3 probes[2] = {
                LLVector3(104.f, 104.f, 22.f),
                eye + at * 5.f
            };
            const char* pn[6] = {"L","R","B","T","near","far"};
            for (int pi = 0; pi < 2; ++pi)
            {
                std::ostringstream os;
                for (int i = 0; i < 6; ++i)
                {
                    const F32* pl = wf.frustum_planes[i];
                    const F32 dp = pl[0]*probes[pi].mV[0] + pl[1]*probes[pi].mV[1]
                                 + pl[2]*probes[pi].mV[2] + pl[3];
                    os << " " << pn[i] << "=" << dp;
                }
                LL_INFOS("VKFunnel") << "PROJ fovY=" << fovY << " aspect=" << aspect
                    << " zn=" << zn << " zf_proj=" << zf << " zf_cull=" << zf_cull
                    << " | probe" << pi << "(" << probes[pi].mV[0] << ","
                    << probes[pi].mV[1] << "," << probes[pi].mV[2] << ") dots:"
                    << os.str() << LL_ENDL;
            }
        }
    }

    // ---- HUGE-OBJECT DIAG (throttled 5 s, INFO "VKHuge"): the largest world object
    // near the camera (weather domes / sim surrounds) -- identity, sculpt flags, its
    // registered instances and a CPU replay of the compute cull's frustum test against
    // this frame's planes, plus the depth range its bounds span vs the projection far.
    // Decides "culled" vs "rasterizer-clipped" vs "not registered" in one line.
    // Debug only (RenderVKHugeObjectDiag): the scan walks the whole object list and was
    // a ~18 ms hitch every 5 s in busy sims while it ran unconditionally.
    static LLCachedControl<bool> s_huge_diag(gSavedSettings, "RenderVKHugeObjectDiag", false);
    if (s_huge_diag)
    {
        VK_HITCH_SCOPE("bwf_hugeDiag");
        static F64 s_next_huge = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now >= s_next_huge)
        {
            s_next_huge = now + 5.0;
            LLVOVolume* best = nullptr;
            F32 best_size = 0.f;
            const S32 n_obj = gObjectList.getNumObjects();
            for (S32 i = 0; i < n_obj; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAttachment()
                    || o->isHUDAttachment())
                    continue;
                LLVOVolume* vo = dynamic_cast<LLVOVolume*>(o);
                if (!vo) continue;
                F32 sz = o->getScale().length();
                LLCleanInstanceStore::Instance ci;
                if (LLCleanInstanceStore::instance().debugGet(o->getID(), 0, ci))
                {
                    const F32* m = ci.model_matrix;
                    F32 d2 = 0.f;
                    for (int ax = 0; ax < 3; ++ax)
                    {
                        const F32 e = ci.aabb_max[ax] - ci.aabb_min[ax];
                        const F32 cl = std::sqrt(m[ax*4]*m[ax*4] + m[ax*4+1]*m[ax*4+1] + m[ax*4+2]*m[ax*4+2]);
                        d2 += (e * cl) * (e * cl);
                    }
                    sz = llmax(sz, std::sqrt(d2));
                }
                const F32 dcam = (o->getPositionAgent() - eye).length();
                if (dcam - sz > 2048.f) continue;
                if (sz > best_size) { best_size = sz; best = vo; }
            }
            if (best && best_size > 64.f)
            {
                LLVolume* vol = best->getVolume();
                const U8 st = vol ? (U8)vol->getParams().getSculptType() : 0;
                const LLVector3 bp = best->getPositionAgent();
                LL_INFOS("VKHuge") << "HUGE obj=" << best->getID() << " local=" << best->getLocalID()
                    << " size=" << best_size << " scale=" << best->getScale()
                    << " pos=" << bp << " dist_eye=" << (bp - eye).length()
                    << " sculpt=" << (best->isSculpted() ? 1 : 0)
                    << " stype=" << (S32)(st & LL_SCULPT_TYPE_MASK)
                    << " invert=" << ((st & LL_SCULPT_FLAG_INVERT) ? 1 : 0)
                    << " mirror=" << ((st & LL_SCULPT_FLAG_MIRROR) ? 1 : 0)
                    << " mesh=" << (best->isMesh() ? 1 : 0)
                    << " animesh=" << (best->isAnimatedObject() ? 1 : 0)
                    << " rigged=" << (best->isRiggedMesh() ? 1 : 0)
                    << " texanim=" << (best->mTextureAnimp ? 1 : 0)
                    << " nfaces=" << best->getNumTEs()
                    << " zf_proj=" << zf << " zf_cull=" << zf_cull << LL_ENDL;
                const S32 nte = llmin((S32)best->getNumTEs(), 8);
                for (S32 f = 0; f < nte; ++f)
                {
                    LLCleanInstanceStore::Instance ci;
                    U32 iid = 0xFFFFFFFFu;
                    if (!LLCleanInstanceStore::instance().debugGet(best->getID(), f, ci, &iid))
                    {
                        LL_INFOS("VKHuge") << "  face " << f << ": NO INSTANCE registered" << LL_ENDL;
                        continue;
                    }
                    // CPU replay of clean_cull.comp worldAABB (instance AABB only; the
                    // GPU also unions the resident LOD slots' bounds) + frustumTest.
                    const F32* m = ci.model_matrix;
                    LLVector3 wmin(1e30f, 1e30f, 1e30f), wmax(-1e30f, -1e30f, -1e30f);
                    for (int c = 0; c < 8; ++c)
                    {
                        const F32 x = (c & 1) ? ci.aabb_max[0] : ci.aabb_min[0];
                        const F32 y = (c & 2) ? ci.aabb_max[1] : ci.aabb_min[1];
                        const F32 z = (c & 4) ? ci.aabb_max[2] : ci.aabb_min[2];
                        for (int r = 0; r < 3; ++r)
                        {
                            const F32 w = m[0*4+r]*x + m[1*4+r]*y + m[2*4+r]*z + m[3*4+r];
                            wmin.mV[r] = llmin(wmin.mV[r], w);
                            wmax.mV[r] = llmax(wmax.mV[r], w);
                        }
                    }
                    std::ostringstream pl;
                    bool pass = true;
                    for (int i = 0; i < 6; ++i)
                    {
                        const F32* p = wf.frustum_planes[i];
                        const F32 vx = p[0] >= 0.f ? wmax.mV[0] + 1.f : wmin.mV[0] - 1.f;
                        const F32 vy = p[1] >= 0.f ? wmax.mV[1] + 1.f : wmin.mV[1] - 1.f;
                        const F32 vz = p[2] >= 0.f ? wmax.mV[2] + 1.f : wmin.mV[2] - 1.f;
                        const F32 dv = p[0]*vx + p[1]*vy + p[2]*vz + p[3];
                        if (dv < 0.f) pass = false;
                        pl << (i ? "," : "") << (S32)dv;
                    }
                    // Depth range (along the view axis) of the box's corners.
                    F32 dmin = 1e30f, dmax = -1e30f;
                    for (int c = 0; c < 8; ++c)
                    {
                        const LLVector3 q((c & 1) ? wmax.mV[0] : wmin.mV[0],
                                          (c & 2) ? wmax.mV[1] : wmin.mV[1],
                                          (c & 4) ? wmax.mV[2] : wmin.mV[2]);
                        const F32 dd = (q - eye) * at;
                        dmin = llmin(dmin, dd); dmax = llmax(dmax, dd);
                    }
                    const bool inside = eye.mV[0] >= wmin.mV[0] && eye.mV[0] <= wmax.mV[0]
                                     && eye.mV[1] >= wmin.mV[1] && eye.mV[1] <= wmax.mV[1]
                                     && eye.mV[2] >= wmin.mV[2] && eye.mV[2] <= wmax.mV[2];
                    const U32 fl = ci.flags;
                    LL_INFOS("VKHuge") << "  face " << f << ": inst=" << iid
                        << " flags=0x" << std::hex << fl << std::dec
                        << " (blend=" << ((fl & 2u) ? 1 : 0) << " mask=" << ((fl & 4u) ? 1 : 0)
                        << " dsided=" << ((fl & 256u) ? 1 : 0) << " unlit=" << ((fl & 4096u) ? 1 : 0)
                        << " texanim=" << ((fl & 1024u) ? 1 : 0) << " rigged=" << (fl & 1u) << ")"
                        << " grp=" << ci.lod_group_id << " mat=" << ci.material_id
                        << " lodR=" << ci.aabb_min[3]
                        << " wmin=" << wmin << " wmax=" << wmax
                        << " cam_inside_box=" << (inside ? 1 : 0)
                        << " frustum=" << (pass ? "PASS" : "REJECT") << " planes[" << pl.str() << "]"
                        << " depth=[" << dmin << "," << dmax << "] beyond_proj_far="
                        << ((dmin > zf) ? "ALL" : (dmax > zf ? "PART" : "NONE")) << LL_ENDL;
                }
            }
        }
    }

    // LOD factor: use the SAME value the GL clean pipeline's cull pushes
    // (LLVOVolume::sLODFactor == RenderVolumeLODFactor, clamped) so VK LOD-select
    // matches GL. It was hardcoded to 1.125, which ignored the user's setting
    // (GL users run 2-4) -> VK LODed out much more aggressively than GL.
    F32 lodFactor = LLVOVolume::sLODFactor;
    if (lodFactor < 0.01f) lodFactor = 1.125f;   // guard pre-init

    wf.cam_pos[0]=eye.mV[0]; wf.cam_pos[1]=eye.mV[1]; wf.cam_pos[2]=eye.mV[2];
    {   // <OCOL> the OCOL mist's global noise frame (llcleanvolfogvk.h)
        const LLVector3d o = gAgent.getPosGlobalFromAgent(LLVector3::zero);
        LLCleanVolFogVK::setOriginGlobal(o.mdV[VX], o.mdV[VY]);
    }
    wf.cam_pos[3]=lodFactor;
    wf.lod_cam_pos[0]=lod_eye.mV[0]; wf.lod_cam_pos[1]=lod_eye.mV[1]; wf.lod_cam_pos[2]=lod_eye.mV[2];   // (#479 frozen: the cull camera)
    wf.lod_cam_pos[3]=0.f;
    // World clock (stops under Freeze World): water waves, GPU texture anim, GPU omega,
    // TAA previous time -- see llworldclock.h.
    wf.time = (F32)LLWorldClock::getTime();
    // Temporal history (#84): nothing from before or during a teleport is a valid
    // previous frame (the renderer adds origin shifts and camera cuts itself).
    wf.temporal_reset = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;

    // ---- Lighting + sky from the REAL environment (day cycle) ----
    // Sun direction and the sky/sun/ambient colors come straight from the active
    // LLSettingsSky so the VK world's Lambert term and the gradient sky track the
    // same day cycle the GL renderer uses. Falls back to the struct's daylight
    // defaults if the environment isn't resolved yet.
    {
        LLEnvironment& env = LLEnvironment::instance();
        // Advance the day cycle + apply region/parcel environment transitions. LL does
        // this in display() (llviewerdisplay.cpp), which Vulkan diverts -- without it
        // the environment never leaves the built-in "_default_" sky (fixed noon sun:
        // nights never got dark, and legacy skies never engaged classic mode).
        env.update(LLViewerCamera::getInstance());
        LLVector3 sun = env.getSunDirection();     // world-space, normalized
        if (sun.lengthSquared() > 0.0001f)
        {
            sun.normalize();
            wf.sun_dir[0] = sun.mV[0];
            wf.sun_dir[1] = sun.mV[1];
            wf.sun_dir[2] = sun.mV[2];
        }

        LLSettingsSky::ptr_t sky = env.getCurrentSky();
        if (sky)
        {
            LLColor3 sunc  = sky->getSunlightColor();
            LLColor3 ambc  = sky->getAmbientColor();
            LLColor3 horiz = sky->getBlueHorizon();

            // Sunlight/ambient come back in HDR-ish linear scale; damp so the
            // simple Lambert term doesn't blow out (full tonemap is a later chunk).
            const F32 kSun = 1.0f / 3.0f;
            const F32 kAmb = 1.0f / 3.0f;
            wf.sun_color[0] = llclamp(sunc.mV[0]*kSun, 0.f, 1.f);
            wf.sun_color[1] = llclamp(sunc.mV[1]*kSun, 0.f, 1.f);
            wf.sun_color[2] = llclamp(sunc.mV[2]*kSun, 0.f, 1.f);
            wf.ambient_color[0] = llclamp(ambc.mV[0]*kAmb + 0.15f, 0.f, 1.f);
            wf.ambient_color[1] = llclamp(ambc.mV[1]*kAmb + 0.15f, 0.f, 1.f);
            wf.ambient_color[2] = llclamp(ambc.mV[2]*kAmb + 0.18f, 0.f, 1.f);

            // Sky gradient: blue-horizon drives the horizon band; a darker,
            // slightly bluer version drives the zenith. Cheap, no scattering.
            wf.sky_horizon[0] = llclamp(horiz.mV[0], 0.f, 1.f);
            wf.sky_horizon[1] = llclamp(horiz.mV[1], 0.f, 1.f);
            wf.sky_horizon[2] = llclamp(horiz.mV[2]*1.05f + 0.05f, 0.f, 1.f);
            wf.sky_zenith[0]  = llclamp(horiz.mV[0]*0.35f, 0.f, 1.f);
            wf.sky_zenith[1]  = llclamp(horiz.mV[1]*0.50f, 0.f, 1.f);
            wf.sky_zenith[2]  = llclamp(horiz.mV[2]*0.80f + 0.10f, 0.f, 1.f);
        }

        // WINDLIGHT ATMOSPHERE + POST: the same sky parameters LLSettingsVOSky::
        // applySpecial / applyToUniforms feed LL's sky, soften (sunlit/amblit) and
        // haze, plus LLPipeline::generateExposure / tonemap's post inputs. World frame
        // (z up); the VK shaders permute into LL's Y-up frame where LL's math needs it.
        // Layout: indra/vkclean/shaders/clean_atmos_common.glsl.
        if (sky)
        {
            float* a = wf.atmos;
            static LLCachedControl<bool> s_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
            const bool auto_adj = s_auto_adjust;
            const F32  probe_amb = sky->getReflectionProbeAmbiance(auto_adj);
            const bool classic = sky->canAutoAdjust() && !auto_adj;
            const F32  g = sky->getGamma();

            // LLSettingsVOSky::applySpecial auto-adjust branch (only with RenderSkyAutoAdjustLegacy).
            static LLCachedControl<F32> s_aa_amb(gSavedSettings, "RenderSkyAutoAdjustAmbientScale", 0.75f);
            static LLCachedControl<F32> s_aa_hdr(gSavedSettings, "RenderSkyAutoAdjustHDRScale", 2.f);
            static LLCachedControl<F32> s_aa_bh(gSavedSettings, "RenderSkyAutoAdjustBlueHorizonScale", 1.f);
            static LLCachedControl<F32> s_aa_bd(gSavedSettings, "RenderSkyAutoAdjustBlueDensityScale", 1.f);
            static LLCachedControl<F32> s_aa_sun(gSavedSettings, "RenderSkyAutoAdjustSunColorScale", 1.f);
            LLColor3 bh = sky->getBlueHorizon();
            LLColor3 bd = sky->getBlueDensity();
            LLColor3 sunc = sky->getSunlightColor();
            LLColor4 amb4 = sky->getTotalAmbient();
            LLColor3 amb(amb4.mV[0], amb4.mV[1], amb4.mV[2]);
            F32 hdr = 1.f;
            if (sky->getReflectionProbeAmbiance() != 0.f)
            {
                hdr = sqrtf(g) * 2.f;
            }
            else if (sky->canAutoAdjust() && auto_adj)
            {
                amb = amb * (F32)s_aa_amb;
                hdr = s_aa_hdr;
                bh = bh * (F32)s_aa_bh;
                bd = bd * (F32)s_aa_bd;
                sunc = sunc * (F32)s_aa_sun;
            }

            const bool sun_up = sky->getIsSunUp();
            const LLColor3 moonc = sky->getMoonlightColor();
            const LLColor3 lc = sun_up ? sunc : moonc;
            const LLColor3 gl = sky->getGlow();
            LLVector3 ld = env.getLightDirection();
            LLVector3 ln = ld;
            if (ln.mV[VZ] < -0.1f) ln.mV[VZ] = -0.1f;      // getClampedLightNorm
            if (ld.lengthSquared() > 1e-6f) ld.normalize();
            const LLVector3 sdir = sky->getSunDirection();
            const LLVector3 mdir = sky->getMoonDirection();

            static LLCachedControl<F32> s_fade(gSavedSettings, "RenderVKDistanceFade", 0.6f);
            static LLCachedControl<bool> s_hdr_on(gSavedSettings, "RenderHDREnabled", true);
            static LLCachedControl<F32> s_sun_scale(gSavedSettings, "RenderSkySunlightScale", 1.5f);
            static LLCachedControl<F32> s_sun_hdr_scale(gSavedSettings, "RenderHDRSkySunlightScale", 1.5f);
            static LLCachedControl<F32> s_amb_scale(gSavedSettings, "RenderSkyAmbientScale", 1.5f);
            // VINTAGE MODE (Preferences "HDR and Emissive" off: RenderDisableVintageMode
            // sets both of these, llviewercontrol.cpp). LL renders it LDR: no exposure /
            // tonemapper (a clamp), no HDR bloom (legacy glow instead, recordWorld), the
            // non-HDR sun scale, no G-buffer emissive (opaque PBR emissive is dropped) and
            // 8-bit reflection probes (radiance clamped to 1).
            static LLCachedControl<bool> s_emissive_buf(gSavedSettings, "RenderEnableEmissiveBuffer", true);
            const bool vintage = !s_hdr_on;

            auto put = [&](int v, F32 x, F32 y, F32 z, F32 w) { a[v*4]=x; a[v*4+1]=y; a[v*4+2]=z; a[v*4+3]=w; };
            put(0,  bh.mV[0], bh.mV[1], bh.mV[2], sky->getHazeHorizon());
            put(1,  bd.mV[0], bd.mV[1], bd.mV[2], sky->getHazeDensity());
            put(2,  lc.mV[0], lc.mV[1], lc.mV[2], sky->getCloudShadow());
            put(3,  amb.mV[0], amb.mV[1], amb.mV[2], sky->getMaxY());
            put(4,  ln.mV[0], ln.mV[1], ln.mV[2], sky->getDensityMultiplier());
            put(5,  gl.mV[0], gl.mV[1], gl.mV[2], sky->getDistanceMultiplier());
            put(6,  sky->getSunMoonGlowFactor(), hdr, 1.f, s_emissive_buf ? 0.f : 1.f);
            put(7,  ld.mV[0], ld.mV[1], ld.mV[2], llclamp((F32)s_fade, 0.f, 1.f));
            put(8,  sunc.mV[0], sunc.mV[1], sunc.mV[2], classic ? 1.f : 0.f);
            put(9,  moonc.mV[0], moonc.mV[1], moonc.mV[2], sun_up ? 1.f : 0.f);
            // Disc angular radii as the GL clean sky (fillSkyBodyUniforms).
            put(10, sdir.mV[0], sdir.mV[1], sdir.mV[2], 0.05f * sky->getSunScale());
            put(11, mdir.mV[0], mdir.mV[1], mdir.mV[2], 0.045f * sky->getMoonScale());
            put(12, sky->getMoonBrightness(), (F32)sky->getStarBrightness() / 500.0f,
                    s_hdr_on ? (F32)s_sun_hdr_scale : (F32)s_sun_scale, (F32)s_amb_scale);

            // ---- post (LLPipeline::generateExposure + tonemap) ----
            static LLCachedControl<F32>  s_exposure(gSavedSettings, "RenderExposure", 1.f);
            static LLCachedControl<U32>  s_tm_type(gSavedSettings, "RenderTonemapType", 0U);
            static LLCachedControl<bool> s_dyn(gSavedSettings, "RenderDynamicExposureEnabled", true);
            static LLCachedControl<F32>  s_dyn_coef(gSavedSettings, "RenderDynamicExposureCoefficient", 0.175f);
            static LLCachedControl<F32>  s_dyn_err(gSavedSettings, "RenderDynamicExposureSpeedError", 0.1f);
            static LLCachedControl<F32>  s_dyn_tgt(gSavedSettings, "RenderDynamicExposureSpeedTarget", 2.f);
            static LLCachedControl<bool> s_use_sky_exp(gSavedSettings, "RenderUseExposureSkySettings", false);
            static LLCachedControl<F32>  s_tm_mix(gSavedSettings, "RenderTonemapMix", 1.f);
            if (!classic) sky->setTonemapMix(s_tm_mix);   // as LLSettingsVOSky::applySpecial
            F32 exp_min = 1.f, exp_max = 1.f;
            if (s_use_sky_exp)
            {
                if (s_dyn)
                {
                    exp_min = sky->getHDROffset(auto_adj) - sky->getHDRMin(auto_adj);
                    exp_max = sky->getHDROffset(auto_adj) + sky->getHDRMax(auto_adj);
                }
                else
                {
                    exp_min = exp_max = sky->getHDROffset(auto_adj);
                }
            }
            else if (s_dyn && probe_amb > 0.f)
            {
                const F32 hs = sqrtf(g) * 2.f;
                if (hs > 1.f) { exp_min = 1.f / hs; exp_max = hs; }
            }
            // LL tonemap(): NoPost programs for probe-ambiance-0 (legacy) skies, which
            // also take LEGACY_GAMMA with the sky gamma. Vintage mode takes the same
            // clamp-only path (upstream colorCorrect without TONEMAP: no exposure, no
            // tonemapper) and never meters exposure (generateExposure is HDR-only);
            // LEGACY_GAMMA stays tied to the sky.
            const bool legacy_sky = (probe_amb == 0.f);
            // Build > Options > No Post-processing (RenderDisablePostProcessing): while the
            // build floater is open, no exposure / tonemapper (LL tonemap(): "for artists
            // developing materials"). Exposure keeps metering, as in LL.
            static LLCachedControl<bool> s_build_no_post(gSavedSettings, "RenderDisablePostProcessing", false);
            const bool build_no_post = s_build_no_post && gFloaterTools && gFloaterTools->isAvailable();
            const bool no_post = legacy_sky || vintage || build_no_post;
            // <SS:Skoomastream> LL drops the tonemapper for legacy (Windlight) skies: their mix
            // reads 0 and the NoPost path skips the curve, so RenderTonemapType does
            // nothing there. RenderTonemapLegacySkies applies the chosen curve at
            // RenderTonemapMix to them too, keeping the legacy gamma; off (the default) is
            // LL's behaviour. Vintage and the build no-post mode still win.
            static LLCachedControl<bool> s_tm_legacy(gSavedSettings, "RenderTonemapLegacySkies", false);
            const bool legacy_tm = s_tm_legacy && (classic || legacy_sky) && !vintage && !build_no_post;
            const F32  tm_mix = legacy_tm ? llclamp((F32)s_tm_mix, 0.f, 1.f) : sky->getTonemapMix(auto_adj);
            // (Exposure 1 in vintage for the passes that weigh by display brightness:
            // TAA, FSR, RCAS sharpening, VRS.)
            put(13, vintage ? 1.f : llclamp((F32)s_exposure, 0.5f, 4.f), tm_mix,
                    (F32)(U32)s_tm_type, legacy_sky ? g : 0.f);
            put(14, s_dyn_coef, exp_min, exp_max, s_dyn_err);
            put(15, s_dyn_tgt, (F32)gFrameIntervalSeconds, (s_dyn && !vintage) ? 1.f : 0.f, (no_post && !legacy_tm) ? 1.f : 0.f);
            // </SS:Skoomastream>

            // ---- clouds (LLDrawPoolWLSky::renderSkyCloudsDeferred + LLSettingsVOSky::
            //      applySpecial inputs). Layout: indra/vkclean/shaders/clean_clouds.glsl.
            {
                F32* c = wf.clouds;
                const LLColor3 cc = sky->getCloudColor();
                const LLColor3 pd1 = sky->getCloudPosDensity1();
                const LLColor3 pd2 = sky->getCloudPosDensity2();
                // Scroll: x negated to match the texcoord flip (SL-13084, cloudsV).
                const LLVector2 scroll = env.getCloudScrollDelta();
                LLUUID na = sky->getCloudNoiseTextureId(), nb = sky->getNextCloudNoiseTextureId();
                F32 blend = (F32)sky->getBlendFactor();
                if (sky->getCloudScrollRate().isExactlyZero()) blend = 0.f;
                if (na.isNull()) { na = nb; blend = 0.f; }
                if (nb.isNull() || nb == na) { nb = na; blend = 0.f; }
                // The WL sky pool that fetched these (LLVOSky::setCloudNoiseTextures) is
                // GL-only: keep them fetched at full resolution ourselves.
                for (const LLUUID& nid : { na, nb })
                {
                    if (nid.isNull()) continue;
                    LLViewerFetchedTexture* nt = LLViewerTextureManager::getFetchedTexture(
                        nid, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    if (nt) nt->addTextureStats(1024.f * 1024.f);
                }
                const bool on = na.notNull() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS)
                             && sky->getCloudScale() >= 0.001f;
                c[0] = cc.mV[0]; c[1] = cc.mV[1]; c[2] = cc.mV[2]; c[3] = sky->getCloudScale();
                c[4] = pd1.mV[0] - scroll.mV[0]; c[5] = pd1.mV[1] + scroll.mV[1]; c[6] = pd1.mV[2];
                c[7] = sky->getCloudVariance();
                c[8] = pd2.mV[0]; c[9] = pd2.mV[1]; c[10] = pd2.mV[2]; c[11] = blend;
                c[12] = -1.f; c[13] = -1.f; c[14] = 0.f; c[15] = on ? 1.f : 0.f;
                std::memcpy(wf.cloud_noise[0], na.mData, 16);
                std::memcpy(wf.cloud_noise[1], nb.mData, 16);
            }

            // ---- sun/moon billboards, stars, rainbow/halo (LLVOSky::
            //      updateHeavenlyBodyGeometry, LLDrawPoolWLSky renderHeavenlyBodies /
            //      renderStarsDeferred / renderSkyHazeDeferred inputs). Layout:
            //      indra/vkclean/shaders/clean_sky_bodies.glsl.
            {
                F32* b = wf.bodies;
                // Billboard on the unit sphere: LL's quad sits at to_dir * HEAVENLY_BODY_DIST
                // with half extents scaled by the same distance, which cancels out.
                auto billboard = [&](F32* o, const LLQuaternion& rot, F32 scale, F32 disk_radius, bool draw)
                {
                    const LLVector3 to_dir = LLVector3::x_axis * rot;
                    LLVector3 right = to_dir % LLVector3::z_axis;
                    LLVector3 up    = right % to_dir;
                    if ((to_dir * LLVector3::z_axis) > 0.99f)   // at zenith the cross products fail
                    {
                        right = LLVector3::y_axis_neg * rot;
                        up    = LLVector3::z_axis * rot;
                    }
                    right.normalize();
                    up.normalize();
                    const F32 enlarge = 1.f - to_dir.mV[VZ];
                    right *= (1.f + enlarge * 0.3f) * scale * HEAVENLY_BODY_FACTOR * disk_radius;
                    up    *= (1.f + enlarge * 0.2f) * scale * HEAVENLY_BODY_FACTOR * disk_radius;
                    o[0] = to_dir.mV[0]; o[1] = to_dir.mV[1]; o[2] = to_dir.mV[2]; o[3] = draw ? 1.f : 0.f;
                    o[4] = right.mV[0];  o[5] = right.mV[1];  o[6] = right.mV[2];
                    o[8] = up.mV[0];     o[9] = up.mV[1];     o[10] = up.mV[2];
                };
                // SUN_DISK_RADIUS 0.5, MOON_DISK_RADIUS 0.45 (llvosky.cpp).
                billboard(b + 0,  sky->getSunRotation(),  sky->getSunScale(),  0.5f,  sky->getIsSunUp());
                billboard(b + 12, sky->getMoonRotation(), sky->getMoonScale(), 0.45f, sky->getIsMoonUp());

                // Sun A/B blend as renderHeavenlyBodies; the moon only ever binds A.
                LLUUID sa = sky->getSunTextureId(), sb = sky->getNextSunTextureId();
                F32 sun_blend = (F32)sky->getBlendFactor();
                if (sa.isNull()) { sa = sb; sun_blend = 0.f; }
                if (sb.isNull() || sb == sa) { sb = sa; sun_blend = 0.f; }
                b[7] = sun_blend;
                LLUUID moon = sky->getMoonTextureId();
                if (moon.isNull()) moon = sky->getNextMoonTextureId();
                // Stars use bloom A only (starsF samples diffuseMap twice); null -> IMG_BLOOM1
                // as LLVOSky::setBloomTextures.
                LLUUID bloom = sky->getBloomTextureId();
                if (bloom.isNull()) bloom = LLSettingsSky::GetDefaultBloomTextureId();
                LLUUID rainbow = sky->getRainbowTextureId();
                if (rainbow.isNull()) rainbow = LLSettingsSky::GetDefaultRainbowTextureId();
                LLUUID halo = sky->getHaloTextureId();
                if (halo.isNull()) halo = LLSettingsSky::GetDefaultHaloTextureId();

                // Rainbow/halo: hobbled when there's no light source (renderSkyHazeDeferred).
                const bool lit = sky->getIsSunUp() || sky->getIsMoonUp();
                b[24] = lit ? (F32)sky->getSkyMoistureLevel() : 0.f;
                b[25] = (F32)sky->getSkyDropletRadius();
                b[26] = lit ? (F32)sky->getSkyIceLevel() : 0.f;
                b[27] = 0.f;
                // Stars: field rotation (gFrameTimeSeconds * 0.01 DEGREES about Z) and the
                // twinkle phase mod(time, 1.25) with time = elapsed * 0.5 (starsV).
                b[28] = (F32)sky->getStarBrightness() / 500.f;
                // (world clock: both hold under Freeze World)
                b[29] = (F32)(fmod(LLWorldClock::getTime() * 0.01, 360.0) * DEG_TO_RAD);
                b[30] = (F32)fmod(LLWorldClock::getTime() * 0.5, 1.25);
                b[31] = 0.f;
                for (int i = 32; i < 40; ++i) b[i] = -1.f;

                // LLVOSky's fetches of these feed the GL-only pools: keep them fetched
                // ourselves, as the cloud noise above.
                const LLUUID ids[6] = { sa, sb, moon, bloom, rainbow, halo };
                for (int i = 0; i < 6; ++i)
                {
                    std::memcpy(wf.sky_tex[i], ids[i].mData, 16);
                    if (ids[i].isNull() || (i == 1 && ids[1] == ids[0])) continue;
                    LLViewerFetchedTexture* st = LLViewerTextureManager::getFetchedTexture(
                        ids[i], FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    if (st) st->addTextureStats(1024.f * 1024.f);
                }
            }
        }

        // Water fog colour + density from the active water settings: this is what
        // makes the VK depth-fog match the sim's real water look (the acceptance test).
        LLSettingsWater::ptr_t water = env.getCurrentWater();
        if (water)
        {
            LLColor3 fog = water->getWaterFogColor();
            wf.water_fog[0] = fog.mV[0];
            wf.water_fog[1] = fog.mV[1];
            wf.water_fog[2] = fog.mV[2];
            // The water's "Underwater Fog Modifier" applies while the camera is below the
            // surface (LL getModifiedWaterFogDensity); the raw density made underwater views
            // far denser / darker than the environment intends.
            wf.water_fog[3] = llmax(water->getModifiedWaterFogDensity(LLViewerCamera::getInstance()->cameraUnderWater()), 0.f);

            // WATER SURFACE (LLDrawPoolWater::renderPostDeferred + LLSettingsVOWater::
            // applySpecial inputs). Layout: clean_water.frag / clean_frame_info.glsl.
            F32* w = wf.water;
            const LLVector2 d1 = water->getWave1Dir(), d2 = water->getWave2Dir();
            const LLVector3 ns = water->getNormalScale();
            const bool cam_under = LLViewerCamera::getInstance()->cameraUnderWater();
            // LL's "specular" (light_diffuse): normalized sun-or-moon colour scaled by
            // how low the light sits (ground projection) -- magic numbers kept.
            LLColor3 spec(0.f, 0.f, 0.f);
            if (env.getIsSunUp())       spec += sky ? sky->getSunlightColor()  : LLColor3::white;
            else if (env.getIsMoonUp()) spec += sky ? sky->getMoonlightColor() : LLColor3::white;
            LLVector3 ldir = env.getLightDirection(); ldir.normalize();
            const F32 gp = ldir.mV[0] * ldir.mV[0] + ldir.mV[1] * ldir.mV[1];
            if (spec.normalize() > 0.f) spec *= (1.5f + 6.f * gp);
            LLVector4 cln = env.getClampedLightNorm();
            const F32 fog_ks = 1.f / llmax(cln.mV[2], 0.3f);    // LLSettingsVOWater::WATER_FOG_LIGHT_CLAMP
            w[0] = d1.mV[0]; w[1] = d1.mV[1]; w[2] = d2.mV[0]; w[3] = d2.mV[1];
            w[4] = ns.mV[0]; w[5] = ns.mV[1]; w[6] = ns.mV[2]; w[7] = (F32)water->getBlendFactor();
            w[8] = water->getFresnelScale(); w[9] = water->getFresnelOffset();
            w[10] = fmaxf(0.f, water->getBlurMultiplier()) * 2.f;
            w[11] = cam_under ? water->getScaleBelow() : water->getScaleAbove();
            w[12] = spec.mV[0]; w[13] = spec.mV[1]; w[14] = spec.mV[2]; w[15] = env.getWaterHeight();
            w[16] = fog.mV[0]; w[17] = fog.mV[1]; w[18] = fog.mV[2];
            w[19] = water->getModifiedWaterFogDensity(cam_under);
            w[20] = fog_ks; w[21] = 0.f; w[22] = 0.f; w[23] = cam_under ? 1.f : 0.f;   // [21],[22]: normal map tex indices (renderer)
            const LLUUID na = water->getNormalMapID(), nb = water->getNextNormalMapID();
            // LL's water draw pool (which requests these) doesn't run under Vulkan:
            // keep both normal maps fetched at full resolution ourselves.
            for (const LLUUID& nid : { na, nb })
            {
                if (nid.isNull()) continue;
                LLViewerFetchedTexture* nt = LLViewerTextureManager::getFetchedTexture(
                    nid, FTT_DEFAULT, true, LLGLTexture::BOOST_TERRAIN);
                if (nt) nt->addTextureStats(1024.f * 1024.f);
            }
            std::memcpy(wf.water_normal[0], na.mData, 16);
            std::memcpy(wf.water_normal[1], (nb.notNull() ? nb : na).mData, 16);
        }
    }

    // HUD camera. LL's get_hud_matrices (llviewerdisplay.cpp) builds the GL HUD
    // projection (ortho, z in GL [-1,1]) and model; remap to Vulkan clip space:
    // flip Y and map z from [-1,1] to [0,1] (z' = 0.5 z + 0.5 w).
    wf.hud_enabled = false;
    {
        extern bool get_hud_matrices(glm::mat4& proj, glm::mat4& model);
        glm::mat4 hproj, hmodel;
        if (LLPipeline::sShowHUDAttachments && !gDisconnected && get_hud_matrices(hproj, hmodel))
        {
            glm::mat4 gl_to_vk(1.f);
            gl_to_vk[1][1] = -1.f;
            gl_to_vk[2][2] = 0.5f;
            gl_to_vk[3][2] = 0.5f;
            const glm::mat4 hvp = gl_to_vk * hproj * hmodel;
            std::memcpy(wf.hud_view_proj, &hvp[0][0], sizeof(wf.hud_view_proj));
            // #919: the HUD rect (LL's world view rect: the window minus the opaque top
            // bars; raw pixels, bottom-left origin) -> top-left origin. The VK world view
            // is the whole window; HUD space is not (LLViewerWindow::getHudRectRaw).
            // Picks (LLCleanCursorPick: hud_viewport) and the HUD text / manipulators
            // (hud_proj_ui, mousePointHUD) use the same rect.
            const LLRect wv = gViewerWindow->getHudRectRaw();
            const LLRect win = gViewerWindow->getWindowRectRaw();
            wf.hud_viewport[0] = wv.mLeft - win.mLeft;
            wf.hud_viewport[1] = win.mTop - wv.mTop;
            wf.hud_viewport[2] = wv.getWidth();
            wf.hud_viewport[3] = wv.getHeight();
            wf.hud_enabled = true;
            // UI-frame HUD projection: remap clip space into the HUD rect.
            {
                F32 remap[16];
                gViewerWindow->getHudClipRemap(remap);
                const glm::mat4 S = glm::make_mat4(remap);
                const glm::mat4 hp = S * gl_to_vk * hproj;
                std::memcpy(wf.hud_proj_ui, &hp[0][0], sizeof(wf.hud_proj_ui));
                std::memcpy(wf.hud_model, &hmodel[0][0], sizeof(wf.hud_model));
            }
        }
    }

    // ---- Local lights (point + projector) ----
    // LL's deferred path gathers mNearbyLights in the (diverted) cull; gather from the
    // pipeline's light set directly with the same filters (attached-lights setting,
    // near-zero colour/radius, frustum), then keep the most important ones up to
    // RenderLocalLightCount (Preferences "Max. nearby lights"; the light SSBO holds 256).
    // Probe and mirror captures share this list: each re-filters it by distance to its
    // own origin (probes: on the GPU, clean_probe_gpu.comp PH_LIGHTS).
    wf.lights.clear();
    wf.light_takeover.clear();
    {
        static const F32 kLightFalloffFudge = 0.5f;   // pipeline.cpp DEFERRED_LIGHT_FALLOFF
        static const size_t kMaxVKLights = 256;       // LLCleanRendererVK::MAX_VK_LIGHTS
        static LLCachedControl<S32> s_light_count(gSavedSettings, "RenderLocalLightCount", 256);
        const size_t light_cap = (size_t)llclamp((S32)s_light_count, 0, (S32)kMaxVKLights);
        VK_HITCH_SCOPE("bwf_lights");
        LLViewerCamera* lcam = LLViewerCamera::getInstance();
        const LLVector3 lorigin = lcam->getOrigin();
        struct LightCache;
        struct Cand { F32 score; LLDrawable* dr; const LightCache* lc; };
        static std::vector<Cand> cands;
        cands.clear();
        // LIGHT PARAMETER CACHE: is-light / attachment / radius / colour are extra-param
        // lookups (hash map per call) on every light in the region every frame. Cached per
        // drawable (validated by object id) and refreshed on a 4-frame rotation; position
        // and the frustum test stay per frame. A scripted colour / radius change shows
        // within 4 frames.
        // Also cached: position (static lights; moving ones -- attachments, active
        // drawables -- re-read it every frame), linear colour, falloff, spotlight flag.
        // FLAT (crowd perf): the cache is a dense array in the light set's order, rebuilt
        // only when the set changes (LLPipeline::getLightsGen; entries carried over). The
        // per-frame walk used to iterate the std::set (a dependent pointer chase per node)
        // and look each drawable up in a hash map -- ~0.8 ms for ~1800 region lights;
        // now it reads the array and each drawable's state word (independent loads).
        // dr: raw, as the set it mirrors holds the reference -- the array is rebuilt on every
        // membership change, so each entry's drawable is alive while it is walked.
        struct LightCache { LLDrawable* dr = nullptr; U64 id = 0; U32 stamp = 0; bool light = false, attach = false, spot = false;
                            F32 s = 0.f; F32 c2 = 0.f; F32 falloff = 0.f; LLColor3 col; LLVector3 pos; };
        static std::vector<LightCache> lflat;
        static U32 lflat_gen = 0;
        static bool lflat_valid = false;
        static U32 lframe = 0;
        ++lframe;
        if (!lflat_valid || lflat_gen != gPipeline.getLightsGen())
        {
            // Rebuild in set order, keeping the cached entries of drawables still in it.
            std::unordered_map<const LLDrawable*, size_t> old_idx;
            old_idx.reserve(lflat.size());
            for (size_t i = 0; i < lflat.size(); ++i) old_idx.emplace(lflat[i].dr, i);
            std::vector<LightCache> next;
            next.reserve(gPipeline.getLights().size());
            for (LLDrawable* dr : gPipeline.getLights())
            {
                if (!dr) continue;
                auto it = old_idx.find(dr);
                if (it != old_idx.end()) next.push_back(std::move(lflat[it->second]));
                else { next.emplace_back(); next.back().dr = dr; }
            }
            lflat.swap(next);
            lflat_gen = gPipeline.getLightsGen();
            lflat_valid = true;
        }
        // Agent coordinates move on region crossings: cached static positions too.
        static LLVector3d s_agent_origin;
        const LLVector3d agent_origin = gAgent.getPosGlobalFromAgent(LLVector3::zero);
        const bool origin_moved = agent_origin != s_agent_origin;
        s_agent_origin = agent_origin;
        U32 n_lights = 0;
        cands.reserve(lflat.size());
        for (LightCache& lc : lflat)
        {
            LLDrawable* dr = lc.dr;
            if (!dr || dr->isDead()) continue;
            ++n_lights;
            // (the object is only touched on the refresh: a drawable's object never
            // changes, and the set holds the drawable alive while it is in the array)
            if (lc.id == 0 || lframe - lc.stamp >= 4u)
            {
                LLVOVolume* vol = dr->getVOVolume();
                if (!vol) { lc.light = false; continue; }
                const U64 oid = vol->getID().getDigest64() | 1ull;
                // Staggered: a new entry's first stamp is offset by its id, so a region of
                // lights refreshes a quarter per frame instead of all at once.
                lc.stamp = lc.id != oid ? lframe - (U32)(oid & 3u) : lframe;
                lc.id = oid;
                lc.light = vol->getIsLight();
                lc.attach = vol->isAttachment();
                lc.s = lc.light ? vol->getLightRadius() * 1.5f : 0.f;
                lc.col = lc.light ? vol->getLightLinearColor() : LLColor3();
                lc.c2 = lc.light ? lc.col.magVecSquared() : 0.f;
                lc.falloff = lc.light ? vol->getLightFalloff(kLightFalloffFudge) : 0.f;
                lc.spot = lc.light && vol->isLightSpotlight();
                lc.pos = dr->getPositionAgent();
                // Light params come off the wire (llclamp passes NaN): a light with a
                // non-finite or non-positive radius, colour or falloff never reaches the
                // GPU (NaN rays, every cluster flooded, a NaN distance in the sort below).
                if (lc.light
                    && !(std::isfinite(lc.s) && lc.s > 0.f && std::isfinite(lc.c2)
                         && std::isfinite(lc.col.mV[0]) && std::isfinite(lc.col.mV[1]) && std::isfinite(lc.col.mV[2])
                         && std::isfinite(lc.falloff)))
                    lc.light = false;
            }
            if (!lc.light) continue;
            if (lc.attach && !LLPipeline::sRenderAttachedLights) continue;
            const F32 s = lc.s;
            if (lc.c2 < 0.001f || s <= 0.001f) continue;
            if (lc.attach || dr->isActive() || origin_moved) lc.pos = dr->getPositionAgent();   // moving: current pose
            if (!lc.pos.isFinite()) continue;
            LLVector4a c; c.load3(lc.pos.mV);
            LLVector4a sa; sa.splat(s);
            if (lcam->AABBInFrustumNoFarClip(c, sa) == 0) continue;
            // Importance to the camera: brightness x radius^2 over distance^2 (how much
            // of the view the light reaches, and how strongly). Distance is floored at
            // 0.1 m so a light at the eye ranks high without dividing by zero.
            const F32 d2 = llmax((lc.pos - lorigin).magVecSquared(), 0.01f);
            cands.push_back({ sqrtf(lc.c2) * s * s / d2, dr, &lc });
        }
        LLVKHitch::count("bwf_lightsInRegion", n_lights);
        // Partial selection over all candidates (a region can hold thousands), then order
        // only the kept <= 256 most important first: a cluster that overflows its
        // CLUSTER_CAP keeps the lowest light indices (clean_lights.comp).
        const auto more_important = [](const Cand& a, const Cand& b) { return a.score > b.score; };
        if (cands.size() > light_cap)
        {
            std::nth_element(cands.begin(), cands.begin() + light_cap, cands.end(), more_important);
            cands.resize(light_cap);
        }
        std::sort(cands.begin(), cands.end(), more_important);
        // (#91 LIGHT TAKEOVER) list the point lights' emitting faces only while the GPU can use them
        static LLCachedControl<U32> s_em_take(gSavedSettings, "RenderVKReSTIREmissiveLightTakeover", 1U);
        static LLCachedControl<U32> s_em_on(gSavedSettings, "RenderVKReSTIREmissive", 1U);
        const bool em_take = s_em_take != 0u && s_em_on == 1u;

        for (const Cand& cd : cands)
        {
            LLDrawable* dr = cd.dr;
            LLVOVolume* vol = dr->getVOVolume();
            const LightCache& lc = *cd.lc;
            LLVKLight L{};
            const LLVector3 pos = lc.pos;
            const F32 s = lc.s;
            const LLColor3 col = lc.col;
            L.pos_radius[0] = pos.mV[0]; L.pos_radius[1] = pos.mV[1]; L.pos_radius[2] = pos.mV[2];
            L.pos_radius[3] = s;
            L.color_falloff[0] = col.mV[0]; L.color_falloff[1] = col.mV[1]; L.color_falloff[2] = col.mV[2];
            L.color_falloff[3] = lc.falloff;
            L.dbg_flags = (U8)((lc.attach ? 1u : 0u) | (dr->isActive() ? 2u : 0u));

            if (lc.spot)
            {
                // LLPipeline::setupSpotLight, in world (agent) space instead of eye space.
                LLVector3 params = vol->getSpotLightParams();   // fov, focus, ambiance
                // fov in [0.01, 3.0] (NaN takes the floor; llclamp would pass it).
                F32 fov = params.mV[0];
                if (!(fov >= 0.01f)) fov = 0.01f;
                else if (fov > 3.0f) fov = 3.0f;
                if (!std::isfinite(params.mV[1])) params.mV[1] = 0.f;
                if (!std::isfinite(params.mV[2])) params.mV[2] = 0.f;
                const LLQuaternion quat = vol->getRenderRotation();
                const LLVector3 scale = vol->getScale();
                LLVector3 at_axis(0.f, 0.f, -scale.mV[2] * 0.5f);
                at_axis *= quat;
                const LLVector3 np = pos + at_axis;
                at_axis.normVec();
                const F32 dist = (scale.mV[1] * 0.5f) / tanf(fov * 0.5f);
                const LLVector3 origin = np - at_axis * dist;
                LLMatrix4 light_mat(quat, LLVector4(origin, 1.f));
                const glm::mat4 light_to_agent = glm::make_mat4((F32*)light_mat.mMatrix);
                const F32 near_clip = dist;
                const F32 far_clip  = s + dist - scale.mV[2];
                const F32 aspect    = scale.mV[1] > 0.f ? scale.mV[0] / scale.mV[1] : 1.f;
                const glm::mat4 trans(0.5f, 0.f, 0.f, 0.f,
                                      0.f, 0.5f, 0.f, 0.f,
                                      0.f, 0.f, 0.5f, 0.f,
                                      0.5f, 0.5f, 0.5f, 1.f);
                const glm::mat4 world_to_tc = trans * glm::perspective(fov, aspect, near_clip, far_clip)
                                            * glm::inverse(light_to_agent);
                std::memcpy(L.proj_mat, glm::value_ptr(world_to_tc), sizeof(L.proj_mat));
                {
                    // Projector shadow map: the same frustum into VK clip space (the
                    // main view's gl_to_vk, so winding matches: front = CCW).
                    // SHADOW NEAR PLANE: LL's near_clip is EXACTLY the fixture prim's
                    // -Z face (np = pos + at_axis * scale.z / 2), and the frustum's near
                    // rectangle is exactly that face (dist is derived from scale.y and
                    // fov). Rendered with cull NONE and no depth clamp, whether that face
                    // survives the z >= 0 clip is float rounding: when it does it lands at
                    // depth ~0 over the WHOLE tile and the projector is fully shadowed
                    // (mode 1: "light missing entirely", cached, never recovers). Casters
                    // start a margin in front of the face instead; receivers inside the
                    // margin read ndc.z <= 0 = lit. The RT path shares shadow_near (its
                    // rays stop at this plane).
                    const F32 shadow_near = near_clip + llmax(0.01f, near_clip * 0.01f);
                    const F32 shadow_far  = llmax(far_clip, shadow_near + 0.01f);
                    glm::mat4 gl_to_vk(1.f);
                    gl_to_vk[1][1] = -1.f; gl_to_vk[2][2] = 0.5f; gl_to_vk[3][2] = 0.5f;
                    const glm::mat4 svp = gl_to_vk * glm::perspective(fov, aspect, shadow_near, shadow_far)
                                        * glm::inverse(light_to_agent);
                    std::memcpy(L.shadow_vp, glm::value_ptr(svp), sizeof(L.shadow_vp));
                    L.shadow_tan[1] = tanf(fov * 0.5f);
                    L.shadow_tan[0] = L.shadow_tan[1] * aspect;
                    L.shadow_near = shadow_near;
                    L.key = vol->getID().getDigest64();
                }

                LLViewerTexture* img = vol->getLightTexture();
                F32 lod_range = 0.f;
                if (img)
                {
                    img->addTextureStats(512.f * 512.f);   // keep the projected image streaming
                    lod_range = logf((F32)llmax(img->getWidth(), 1)) / logf(2.f);
                    std::memcpy(L.tex_uuid, img->getID().mData, 16);
                }
                L.proj_params[0] = params.mV[1];                         // focus
                L.proj_params[1] = llmax(far_clip - near_clip, 0.001f);  // range
                L.proj_params[2] = lod_range;
                L.proj_params[3] = params.mV[2];                         // ambiance
                L.proj_origin[0] = origin.mV[0]; L.proj_origin[1] = origin.mV[1]; L.proj_origin[2] = origin.mV[2];
                L.proj_origin[3] = 1.f;
                L.proj_axis[0] = at_axis.mV[0]; L.proj_axis[1] = at_axis.mV[1]; L.proj_axis[2] = at_axis.mV[2];
                L.proj_axis[3] = near_clip;   // legacy projector image reflection plane (spotLightF proj_p)

                // A degenerate fixture (zero / huge / non-finite scale or rotation) gives
                // non-finite projector matrices: skip the light rather than feed NaN to
                // the clusters, the shadow atlas and the ray-traced shadow rays.
                bool proj_ok = true;
                for (F32 v : L.proj_mat)     proj_ok = proj_ok && std::isfinite(v);
                for (F32 v : L.shadow_vp)    proj_ok = proj_ok && std::isfinite(v);
                for (F32 v : L.proj_params)  proj_ok = proj_ok && std::isfinite(v);
                for (F32 v : L.proj_origin)  proj_ok = proj_ok && std::isfinite(v);
                for (F32 v : L.proj_axis)    proj_ok = proj_ok && std::isfinite(v);
                for (F32 v : L.shadow_tan)   proj_ok = proj_ok && std::isfinite(v);
                proj_ok = proj_ok && std::isfinite(L.shadow_near);
                if (!proj_ok) continue;
            }
            else if (vol)
            {
                // (#869 / #439 ReSTIR DI) A POINT LIGHT's shadow data in its unused projector axis:
                // x = its prim's bounding radius (pass 15's rays stop at that sphere around the
                // light: the bulb never shadows its own light; clean_shadow_core.glsl SH_SPHERE),
                // y = 1 on an attachment (facelights: unshadowed unless RenderVKReSTIRAttachedLights),
                // and a stable key (the reservoirs follow the light across the per-frame re-sort).
                const F32 r = 0.5f * vol->getScale().magVec();
                L.proj_axis[0] = (std::isfinite(r) && r > 0.f) ? llmin(r, 64.f) : 0.f;
                L.proj_axis[1] = lc.attach ? 1.f : 0.f;
                L.key = vol->getID().getDigest64();
                // (#91 LIGHT TAKEOVER, clean_regir.glsl) this prim's faces that may emit (the GPU's mode 0
                // rule: a PBR material's emissive factor, else legacy fullbright + glow) -- their emitting
                // triangles may carry this light. Not on attachments (no emitters there); at most 16 faces.
                if (em_take && !lc.attach && !vol->isDead() && vol->mDrawable.notNull())
                {
                    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
                    const S32 nte = llmin((S32)vol->getNumTEs(), vol->mDrawable->getNumFaces(), 16);
                    const U32 li = (U32)wf.lights.size();
                    for (S32 te = 0; te < nte; ++te)
                    {
                        const LLTextureEntry* tep = vol->getTE((U8)te);
                        if (!tep) continue;
                        const LLGLTFMaterial* gm = tep->getGLTFRenderMaterial();
                        const bool emits = gm ? (gm->mEmissiveColor.mV[0] > 0.f || gm->mEmissiveColor.mV[1] > 0.f
                                                 || gm->mEmissiveColor.mV[2] > 0.f)
                                              : (tep->getFullbright() && tep->getGlow() > 0.f);
                        U32 id;
                        if (emits && store.findInstanceId(vol->getID(), te, id)) wf.light_takeover.emplace_back(id, li);
                    }
                }
            }
            wf.lights.push_back(L);
        }
    }

    // ---- Edit-mode selection outline (LL renderSilhouettes: per-face wireframe) ----
    wf.sel_ids.clear(); wf.sel_root.clear(); wf.hide_ids.clear();
    if (LLSelectMgr::instanceExists())
    {
        LLSelectMgr* sm = LLSelectMgr::getInstance();
        LLObjectSelectionHandle sel = sm->getSelection();
        // HIDE SELECTED (#29): LL markVisible skips selected drawables -- whole objects,
        // whatever the TE mode or silhouette state -- in every view (incl. its RLVa rule:
        // never a locked HUD attachment, nor one RLVa forbids editing).
        if (sm->mHideSelectedObjects && !sel->isEmpty())
        {
            LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
            for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
            {
                LLViewerObject* o = (*it) ? (*it)->getObject() : nullptr;
                if (!o || o->isDead() || o->mDrawable.isNull() || !o->isSelected()) continue;
                if (RlvActions::isRlvEnabled()
                    && ((o->isHUDAttachment() && gRlvAttachmentLocks.isLockedAttachment(o->getRootEdit()))
                        || !RlvActions::canEdit(o)))
                    continue;
                const S32 nte = llmin((S32)o->getNumTEs(), o->mDrawable->getNumFaces());
                for (S32 te = 0; te < nte; ++te)
                {
                    U32 id;
                    if (store.findInstanceId(o->getID(), te, id)) wf.hide_ids.push_back(id);
                }
            }
        }
        // LL's renderSilhouettes gates: silhouettes off (manip drags) / highlights off.
        if (!sel->isEmpty() && sm->silhouettesEnabled())
        {
            // A clicked media face is selected only to route media focus; it gets no
            // edit wireframe.
            const LLUUID media_focus_id = LLViewerMediaFocus::getInstance()->getFocusedObjectID();
            const LLColor4 pc = GMDXSelectionOutline::parentColor();   // <GMDX> Classic: Firestorm's colours
            const LLColor4 cc = GMDXSelectionOutline::childColor();    // <GMDX>
            std::memcpy(wf.sel_parent_color, pc.mV, sizeof(wf.sel_parent_color));
            std::memcpy(wf.sel_child_color,  cc.mV, sizeof(wf.sel_child_color));
            wf.sel_hidden = LLSelectMgr::sRenderHiddenSelections;
            const bool te_mode = sm->getTEMode();
            LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
            for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
            {
                LLSelectNode* node = *it;
                LLViewerObject* o = node ? node->getObject() : nullptr;
                if (!o || o->isDead() || o->mDrawable.isNull()) continue;
                if (media_focus_id.notNull() && o->getID() == media_focus_id) continue;
                if (te_mode && !node->hasSelectedTE()) continue;
                if (GMDXSelectionOutline::usesSilhouette(o)) continue;   // <GMDX> LL silhouette instead
                const S32 nte = llmin((S32)o->getNumTEs(), o->mDrawable->getNumFaces());
                for (S32 te = 0; te < nte; ++te)
                {
                    if (te_mode && !node->isTESelected(te)) continue;
                    U32 id;
                    if (store.findInstanceId(o->getID(), te, id))
                    {
                        wf.sel_ids.push_back(id);
                        wf.sel_root.push_back(o->isRootEdit() ? 1 : 0);
                    }
                }
            }
        }
        // Highlighted-but-unselected objects (About Land > Objects "Show", ForceObjectSelect,
        // box-select drag) live in mHighlightedObjects. LL drew them in renderSilhouettes; that
        // CPU path is gone, so queue their faces into the same GPU outline lists (selection
        // colours; roots as parents).
        if (sm->silhouettesEnabled())
        {
            LLObjectSelectionHandle hl = sm->getHighlightedObjects();
            if (hl.notNull() && hl->getNumNodes() > 0)
            {
                const LLColor4 pc = GMDXSelectionOutline::parentColor();   // <GMDX> Classic: Firestorm's colours
                const LLColor4 cc = GMDXSelectionOutline::childColor();    // <GMDX>
                std::memcpy(wf.sel_parent_color, pc.mV, sizeof(wf.sel_parent_color));
                std::memcpy(wf.sel_child_color,  cc.mV, sizeof(wf.sel_child_color));
                LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
                S32 hl_objs = 0, hl_faces = 0;
                for (LLObjectSelection::iterator it = hl->begin(); it != hl->end(); ++it)
                {
                    LLSelectNode* node = *it;
                    LLViewerObject* o = node ? node->getObject() : nullptr;
                    if (!o || o->isDead() || o->mDrawable.isNull()) continue;
                    // <GMDX> LL silhouette instead (one that was never generated keeps the wireframe)
                    if (node->mSilhouetteExists && GMDXSelectionOutline::usesSilhouette(o)) continue;
                    ++hl_objs;
                    const S32 nte = llmin((S32)o->getNumTEs(), o->mDrawable->getNumFaces());
                    for (S32 te = 0; te < nte; ++te)
                    {
                        U32 id;
                        if (store.findInstanceId(o->getID(), te, id))
                        {
                            wf.sel_ids.push_back(id);
                            wf.sel_root.push_back(o->isRootEdit() ? 1 : 0);
                            ++hl_faces;
                        }
                    }
                }
                static S32 s_last_hl_objs = -1;
                if (hl_objs != s_last_hl_objs)
                {
                    s_last_hl_objs = hl_objs;
                    LL_INFOS("ParcelObjects") << "Highlight outline: " << hl_objs << " objects, "
                        << hl_faces << " faces queued" << LL_ENDL;
                }
            }
        }
        // #252: hover preview from the build floater's links / faces list -- one link
        // (every face) or one face, kind 2 in the same GPU outline lists, own colour.
        LLUUID hover_id;
        S32 hover_te = -1;
        if (LLPanelObjectLinks::getHoverTarget(hover_id, hover_te))
        {
            LLViewerObject* o = gObjectList.findObject(hover_id);
            if (o && !o->isDead() && o->mDrawable.notNull())
            {
                static const LLUIColor s_hover = LLUIColorTable::instance().getColor("SilhouetteHoverColor", LLColor4(1.f, 0.35f, 1.f, 1.f));
                const LLColor4 hc = s_hover.get();
                std::memcpy(wf.sel_hover_color, hc.mV, sizeof(wf.sel_hover_color));
                LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
                const S32 nte = llmin((S32)o->getNumTEs(), o->mDrawable->getNumFaces());
                const S32 te_begin = hover_te < 0 ? 0 : hover_te;
                const S32 te_end   = hover_te < 0 ? nte : llmin(hover_te + 1, nte);
                for (S32 te = te_begin; te < te_end; ++te)
                {
                    U32 id;
                    if (store.findInstanceId(o->getID(), te, id))
                    {
                        wf.sel_ids.push_back(id);
                        wf.sel_root.push_back(2);
                    }
                }
            }
        }
    }

    // ---- Reflection probes (#478: GPU-driven; LLReflectionMapManager itself is GL-only) ----
    // The renderer's GPU passes (clean_probe_gpu.comp) make the default sky probe, the
    // terrain / water grid and the AUTOMATIC (level 3) probes -- placed from a world-anchored
    // 16 m occupancy grid over the clean instances, not the volume octree -- and pick the
    // cube slots and the captures. The CPU only hands over what lives on the CPU: the manual
    // probe objects and each region's position + terrain heights.
    wf.probes.clear();
    wf.probe_regions.clear();
    {
        static LLCachedControl<bool> s_refl(gSavedSettings, "RenderReflectionsEnabled", true);
        static LLCachedControl<S32>  s_detail(gSavedSettings, "RenderReflectionProbeDetail", 1);
        static LLCachedControl<S32>  s_level(gSavedSettings, "RenderReflectionProbeLevel", 3);
        wf.probes_enabled = s_refl && (S32)s_detail >= 0;
        wf.probe_detail = s_detail;
        wf.probe_level = s_level;
        if (LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
            wf.probe_sky_ambiance = sky->getReflectionProbeAmbiance(false);
        // The agent frame's origin (its region's SW corner: a multiple of 256 m) in 16 m
        // cells -- the GPU's world anchor (cell keys survive region crossings).
        {
            const LLVector3d og = gAgent.getPosGlobalFromAgent(LLVector3::zero);
            wf.probe_agent_cell[0] = (int32_t)std::floor(og.mdV[0] / 16.0);
            wf.probe_agent_cell[1] = (int32_t)std::floor(og.mdV[1] / 16.0);
            static bool s_warned = false;
            if (!s_warned && (std::fabs(og.mdV[0] - 16.0 * wf.probe_agent_cell[0]) > 1e-3
                              || std::fabs(og.mdV[1] - 16.0 * wf.probe_agent_cell[1]) > 1e-3))
            {
                s_warned = true;
                LL_WARNS("Vulkan") << "GPU probes: agent origin " << og << " is not on the 16 m grid" << LL_ENDL;
            }
        }
        auto ident = [](F32* m) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.f : 0.f; };
        if (wf.probes_enabled)
        {
            VK_HITCH_SCOPE("bwf_probes");
            static LLCachedControl<U32> s_probe_refresh(gSavedSettings, "RenderVKProbeRefreshFrames", 16);
            // Regions: position + terrain for the GPU's grid probes and its land lift. The
            // heights (64 grid + 289 lattice lookups) are cached per region and re-sampled
            // one region per frame (round robin, >= ~300 frames apart per region) or at once
            // for a new one; `version` tells the renderer to re-upload it.
            {
                struct RegionCache { U32 version = 0; U32 frame = 0; bool valid = false; LLVKProbeRegion r; };
                static std::unordered_map<U64, RegionCache> s_reg;
                static U32 s_reg_frame = 0, s_reg_version = 0;
                static size_t s_reg_rr = 0;
                ++s_reg_frame;
                const auto& regions = LLWorld::getInstance()->getRegionList();
                size_t ri = 0;
                bool refreshed_one = false;
                for (LLViewerRegion* region : regions)
                {
                    if (!region) { ++ri; continue; }
                    RegionCache& rc = s_reg[region->getHandle()];
                    const bool due = !rc.valid
                        || (!refreshed_one && ri == s_reg_rr % std::max<size_t>(regions.size(), 1)
                            && s_reg_frame - rc.frame > 300u);
                    ++ri;
                    if (!due) { wf.probe_regions.push_back(rc.r); continue; }
                    if (rc.valid) refreshed_one = true;
                    LLVKProbeRegion& pr = rc.r;
                    pr.handle = region->getHandle();
                    const LLVector3d o = region->getOriginGlobal();
                    pr.cell[0] = (int32_t)std::floor(o.mdV[0] / 16.0);
                    pr.cell[1] = (int32_t)std::floor(o.mdV[1] / 16.0);
                    const F32 width = llmax(region->getWidth(), 16.f);
                    pr.width_cells = llmax((S32)ll_round(width / 16.f), 1);
                    pr.water = region->getWaterHeight();
                    pr.spacing = width / 16.f;
                    // LLViewerRegion::updateReflectionProbes: 8 x 8, 32 m spacing,
                    // z = max(water, land) + 2.
                    for (U32 i = 0; i < 8; ++i)
                        for (U32 j = 0; j < 8; ++j)
                        {
                            const LLVector3 rp(16.f + 32.f * i, 16.f + 32.f * j, 0.f);
                            pr.grid_z[i * 8 + j] = llmax(pr.water, region->getLandHeightRegion(rp)) + 2.f;
                        }
                    for (U32 y = 0; y < 17; ++y)
                        for (U32 x = 0; x < 17; ++x)
                        {
                            const LLVector3 rp(llmin(pr.spacing * x, width - 0.01f), llmin(pr.spacing * y, width - 0.01f), 0.f);
                            pr.land[y * 17 + x] = region->getLandHeightRegion(rp);
                        }
                    pr.version = ++s_reg_version;
                    if (pr.version == 0) pr.version = ++s_reg_version;   // (0 = "never uploaded")
                    rc.version = pr.version; rc.frame = s_reg_frame; rc.valid = true;
                    wf.probe_regions.push_back(pr);
                }
                ++s_reg_rr;
                if (s_reg.size() > 64)   // regions come and go: bound it (keep the live ones)
                {
                    std::unordered_set<U64> live;
                    for (LLViewerRegion* region : regions) if (region) live.insert(region->getHandle());
                    for (auto it = s_reg.begin(); it != s_reg.end(); )
                        it = live.count(it->first) ? std::next(it) : s_reg.erase(it);
                }
            }
            // Manual probe objects (reflection-probe params; mirrors go to the hero path).
            if ((S32)s_level >= 1)
            {
                // Rolling discovery: a slice of the object list per frame (a full walk
                // every 30 frames cost a spike that grew with everything loaded).
                // Entries that died or stopped being probes are dropped as they're used.
                static std::vector<LLPointer<LLViewerObject>> s_manual;
                static std::unordered_set<LLViewerObject*> s_manual_set;
                static S32 s_cursor = 0;
                {
                    // C11: objects that became probes are pushed by LLVOVolume::
                    // updateReflectionProbePtr (gVKNewReflectionProbes) and picked up at
                    // once; the rolling scan stays as a slow safety net (~1 s lap, 64..512
                    // objects per frame) instead of 2048 objects every frame.
                    // RenderVKProbeRefreshFrames 0 = the old 2048-per-frame scan.
                    extern std::vector<LLUUID> gVKNewReflectionProbes;
                    for (const LLUUID& pid : gVKNewReflectionProbes)
                    {
                        LLViewerObject* o = gObjectList.findObject(pid);
                        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
                        LLVOVolume* v = (LLVOVolume*)o;
                        if (v->isReflectionProbe() && !v->getReflectionProbeIsMirror()
                            && s_manual_set.insert(o).second)
                            s_manual.push_back(o);
                    }
                    gVKNewReflectionProbes.clear();
                    const S32 n_obj = gObjectList.getNumObjects();
                    const S32 slice = (U32)s_probe_refresh == 0 ? llmin(n_obj, (S32)2048)
                                                               : llmin(n_obj, llclamp(n_obj / 60, (S32)64, (S32)512));
                    for (S32 k = 0; k < slice; ++k)
                    {
                        if (s_cursor >= n_obj) s_cursor = 0;
                        LLViewerObject* o = gObjectList.getObject(s_cursor++);
                        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
                        LLVOVolume* v = (LLVOVolume*)o;
                        if (v->isReflectionProbe() && !v->getReflectionProbeIsMirror()
                            && s_manual_set.insert(o).second)
                            s_manual.push_back(o);
                    }
                }
                for (size_t mi = 0; mi < s_manual.size(); )
                {
                    LLViewerObject* ob = s_manual[mi].get();
                    if (!ob || ob->isDead() || !((LLVOVolume*)ob)->isReflectionProbe()
                        || ((LLVOVolume*)ob)->getReflectionProbeIsMirror())
                    {
                        s_manual_set.erase(ob);
                        s_manual[mi] = s_manual.back(); s_manual.pop_back();
                        continue;
                    }
                    ++mi;
                }
                for (const LLPointer<LLViewerObject>& op : s_manual)
                {
                    if (op.isNull() || op->isDead()) continue;
                    LLVOVolume* v = (LLVOVolume*)op.get();
                    const LLVector3 pos = v->getPositionAgent();
                    const LLVector3 half = v->getScale() * 0.5f;
                    LLVKProbe p{};
                    std::memcpy(&p.key, v->getID().mData, 8);
                    p.key &= ~(1ull << 63);
                    if (p.key == 0) p.key = 1;
                    p.sphere[0] = pos.mV[0]; p.sphere[1] = pos.mV[1]; p.sphere[2] = pos.mV[2];
                    p.is_box = v->getReflectionProbeIsBox() ? 1 : 0;
                    p.sphere[3] = p.is_box ? half.magVec() : half.mV[0];
                    if (p.is_box)
                    {
                        const LLQuaternion q = v->getRenderRotation();
                        const glm::mat4 m = glm::translate(glm::mat4(1.f), glm::vec3(pos.mV[0], pos.mV[1], pos.mV[2]))
                            * glm::mat4_cast(glm::quat(q.mQ[3], q.mQ[0], q.mQ[1], q.mQ[2]))
                            * glm::scale(glm::mat4(1.f), glm::vec3(half.mV[0], half.mV[1], half.mV[2]));
                        const glm::mat4 inv = glm::inverse(m);
                        std::memcpy(p.box, glm::value_ptr(inv), sizeof(p.box));
                    }
                    else
                    {
                        ident(p.box);
                        p.parallax_sphere = 1;
                    }
                    p.ambiance = v->getReflectionProbeAmbiance();
                    p.near_clip = llmax(v->getReflectionProbeNearClip(), 0.1f);
                    p.manual = 1;
                    p.dynamic = v->getReflectionProbeIsDynamic() ? 1 : 0;
                    wf.probes.push_back(p);
                }
            }
            LLVKHitch::count("probesManual", (U32)wf.probes.size());
        }
    }

    // ---- Sun/moon shadow cascades (LL generateSunShadow) ----
    // Splits: LL's exponent formula over [near, min(far, 512)] (clip[0] padded 1.25x),
    // light = sun or moon, whichever is up (atmos lane 7, the shader's light dir).
    // Each cascade is fitted as the BOUNDING SPHERE of its overlapping frustum slice
    // (LL's 0.75/1.25 bands), so its ortho extent is constant and the centre can be
    // snapped to whole shadow texels in light space -- stable under camera motion,
    // where LL's per-frame AABB/LiSPSM refit swims. The ortho near is pulled far
    // toward the light (cull planes included) so casters behind the camera still land
    // in the map; D32 keeps the long depth range precise.
    wf.shadow_enabled = false;
    {
        static LLCachedControl<S32> s_detail(gSavedSettings, "RenderShadowDetail", 2);
        static LLCachedControl<LLVector3> s_split_exp(gSavedSettings, "RenderShadowSplitExponent", LLVector3(3.f, 3.f, 2.f));
        static LLCachedControl<U32> s_res(gSavedSettings, "RenderVKShadowResolution", 2048);
        static LLCachedControl<F32> s_offset(gSavedSettings, "RenderShadowOffset", 0.01f);
        static LLCachedControl<F32> s_sbias(gSavedSettings, "RenderVKShadowSampleBias", 0.f);
        static LLCachedControl<U32> s_update(gSavedSettings, "RenderVKShadowUpdate", 0);
        LLVector3 L(wf.atmos[28], wf.atmos[29], wf.atmos[30]);           // toward the light
        const F32 light_lum = wf.atmos[8] + wf.atmos[9] + wf.atmos[10];   // sun-or-moon colour
        if ((S32)s_detail > 0 && L.normalize() > 0.5f && light_lum > 1e-4f)
        {
            const F32 n = llclamp(zn, 0.1f, 4.f);
            // (VRAM ladder "far shadows off", #405: the range shrinks; settings untouched.)
            const F32 f = llmax(n + 16.f, llmin(render_far, 512.f) * LLVulkanContext::instance().vramShadowRangeScale());
            const LLVector3 sx = s_split_exp;
            const F32 da = powf(1.f - llmax(fabsf(L * up), fabsf(L * left)), sx.mV[2]);
            const F32 sxp = sx.mV[1] + (sx.mV[0] - sx.mV[1]) * da;
            F32 clip[4];
            for (int i = 0; i < 4; ++i) clip[i] = n + (f - n) * powf((F32)(i + 1) / 4.f, sxp);
            clip[0] *= 1.25f;

            // Light basis: fw = direction light travels (ortho forward), rt/uu span the map.
            wf.shadow_update = llmin((U32)s_update, 2u);
            const LLVector3 fw = -L;
            LLVector3 rt = (fabsf(fw.mV[VZ]) < 0.99f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f)) % fw;
            rt.normalize();
            const LLVector3 uu = fw % rt;
            const F32 k = tanf(fovY * 0.5f) * sqrtf(1.f + aspect * aspect);   // half-diagonal slope
            // The map size actually allocated (a #226 fit cap / the VRAM ladder can hold it
            // below the setting, #405); the setting until the renderer has one.
            const U32 res_alloc = LLVulkanContext::instance().shadowMapResolution();
            const U32 res = res_alloc ? res_alloc : llclamp((U32)s_res, 256u, 8192u);
            // Capped at 2048 m (unchanged for draw distances <= 1024): at a 4 km draw
            // distance a 8 km pull-back would drag skyboxes kilometres overhead into the
            // ground cascades (and the caster cull) while the shadow range stays 512 m.
            const F32 back = llclamp(2.f * render_far, 512.f, 2048.f);
            for (int c = 0; c < 4; ++c)
            {
                const F32 a = (c == 0) ? n : clip[c - 1] * 0.75f;
                const F32 b = (c == 3) ? clip[3] : clip[c] * 1.25f;
                // Minimal sphere around the slice's 8 corners (centre on the view axis).
                const F32 t = llmin(b, (a + b) * (1.f + k * k) * 0.5f);
                F32 R = sqrtf(llmax((b - t) * (b - t) + b * b * k * k, (t - a) * (t - a) + a * a * k * k));
                const LLVector3 ctr = eye + at * t;
                wf.shadow_slice[c][0] = ctr.mV[0]; wf.shadow_slice[c][1] = ctr.mV[1];
                wf.shadow_slice[c][2] = ctr.mV[2]; wf.shadow_slice[c][3] = R;
                // Cached cascades (all but the nearest, when shadow updates aren't
                // realtime) are fitted 15% larger, so the map stays valid while the
                // camera moves until the renderer's coverage test says re-render.
                if (wf.shadow_update > 0 && c > 0) R *= 1.15f;
                R = ceilf(R * 4.f) * 0.25f;
                const F32 texel = 2.f * R / (F32)res;
                // TEXEL SNAP in ABSOLUTE light space (the grid is fixed to the world, so
                // the map is stable under camera motion), in double: every input is a
                // float, so the products are exact and floor() sees the true quotient --
                // at Z 3000 the float quotient (~1e6 texels) was itself off by ~0.03 texel.
                auto dotd = [](const LLVector3& a, const LLVector3& b)
                { return (F64)a.mV[0] * b.mV[0] + (F64)a.mV[1] * b.mV[1] + (F64)a.mV[2] * b.mV[2]; };
                const F64 texd = (F64)texel;
                const F64 cxd = std::floor(dotd(ctr, rt) / texd) * texd;
                const F64 cyd = std::floor(dotd(ctr, uu) / texd) * texd;
                const F32 cx = (F32)cxd;
                const F32 cy = (F32)cyd;
                const F32 cz = ctr * fw;
                const F32 znear = cz - R - back, zfar = cz + R;
                const F32 iz = 1.f / (zfar - znear);
                // World -> shadow NDC (x,y in [-1,1] map to uv like the VK viewport;
                // z in [0,1]), column-major. Rendered and sampled with the same matrix,
                // so no Y flip is involved. Mw (world space) only feeds the cull planes.
                F32 Mw[16];
                Mw[0] = rt.mV[0] / R; Mw[4] = rt.mV[1] / R; Mw[8]  = rt.mV[2] / R; Mw[12] = -cx / R;
                Mw[1] = uu.mV[0] / R; Mw[5] = uu.mV[1] / R; Mw[9]  = uu.mV[2] / R; Mw[13] = -cy / R;
                Mw[2] = fw.mV[0] * iz; Mw[6] = fw.mV[1] * iz; Mw[10] = fw.mV[2] * iz; Mw[14] = -znear * iz;
                Mw[3] = 0.f; Mw[7] = 0.f; Mw[11] = 0.f; Mw[15] = 1.f;
                // CAMERA-RELATIVE (#555): what the GPU gets maps (world - eye), the
                // translation built in double (the snapped centre minus the eye, both
                // ~km along the light axes, cancel exactly here instead of per vertex /
                // per pixel in float). World-space, at Z 3000 the receiver depth and the
                // caster depth each carried ~2e-4 m of float noise -- 3x the D32
                // constant bias -- which re-rounded every frame: flickering acne on
                // sun-facing floors (normal offset ~0 there). Camera-relative: ~2e-5 m,
                // the same as at ground level.
                {
                    const F64 zneard = dotd(ctr - eye, fw) - (F64)R - (F64)back;   // znear - eye.fw
                    F32* M = wf.shadow_mat[c];
                    std::memcpy(M, Mw, sizeof(Mw));
                    M[12] = (F32)(-(cxd - dotd(eye, rt)) / (F64)R);
                    M[13] = (F32)(-(cyd - dotd(eye, uu)) / (F64)R);
                    M[14] = (F32)(-zneard * (F64)iz);
                }
                const F32* M = Mw;
                // Cull planes (Gribb-Hartmann, VK depth 0..1): L R B T near far.
                F32 rw[4][4];
                for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) rw[r][col] = M[col * 4 + r];
                const int sgn[6][2] = {{0,1},{0,-1},{1,1},{1,-1},{2,0},{2,-1}};
                for (int i = 0; i < 6; ++i)
                {
                    F32 pl[4];
                    for (int col = 0; col < 4; ++col)
                        pl[col] = (i == 4) ? rw[2][col] : rw[3][col] + (F32)sgn[i][1] * rw[sgn[i][0]][col];
                    const F32 len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                    for (int col = 0; col < 4; ++col) wf.shadow_planes[c][i][col] = len > 0.f ? pl[col] / len : pl[col];
                }
                wf.shadow_clip[c]  = clip[c];
                wf.shadow_texel[c] = texel;

                // SHADOW-CASTER PLANES for this cascade's view slice [a, b]: a shadow
                // travels along fw, so for every slice plane whose inward normal does
                // not point along fw (n.fw <= 0) the caster itself must already be
                // inside it. Necessary, not sufficient -- conservative by construction.
                {
                    const F32 hy = fovY * 0.5f;
                    const F32 hx = atanf(tanf(hy) * aspect);
                    LLVector3 nrm[6];
                    F32 dd[6];
                    auto side = [&](const LLVector3& edge_dir, const LLVector3& axis, int i) {
                        LLVector3 nn = axis % edge_dir;
                        if (nn * at < 0.f) nn = -nn;
                        nn.normalize();
                        nrm[i] = nn; dd[i] = -(nn * eye);
                    };
                    side(at * cosf(hx) + left * sinf(hx), up,   0);   // left
                    side(at * cosf(hx) - left * sinf(hx), up,   1);   // right
                    side(at * cosf(hy) + up * sinf(hy),   left, 2);   // top
                    side(at * cosf(hy) - up * sinf(hy),   left, 3);   // bottom
                    nrm[4] = at;  dd[4] = -(at * eye) - a;            // slice near
                    nrm[5] = -at; dd[5] =  (at * eye) + b;            // slice far
                    for (int i = 0; i < 6; ++i)
                    {
                        F32* P = wf.shadow_caster_planes[c][i];
                        if (nrm[i] * fw <= 0.f)
                        { P[0] = nrm[i].mV[0]; P[1] = nrm[i].mV[1]; P[2] = nrm[i].mV[2]; P[3] = dd[i]; }
                        else
                        { P[0] = 0.f; P[1] = 0.f; P[2] = 0.f; P[3] = 1.f; }
                    }
                }
            }
            wf.shadow_bias   = s_sbias;
            wf.shadow_offset = s_offset;
            wf.shadow_enabled = true;
        }
    }

    wf.enabled = true;
    return true;
}

// Render one frame of the REAL viewer through the Vulkan present path. Called
// from display() (llviewerdisplay.cpp) in place of the GL render while
// the Vulkan renderer is active. When the agent is in-world, the GPU-driven
// static-opaque WORLD renders first (compute cull -> indirect-count draw into the
// swapchain, with depth), then the real LLUI draws on top through LLRenderVK, and
// the frame is presented via the swapchain instead of swapBuffers. Rigged/alpha/
// water/sky/deferred-lighting are follow-on chunks.
// VK OBJECT-CACHE DRAIN DRIVER (render-completeness fix).
//
// The region object cache becomes live world objects only via
//   LLVOCachePartition::cull() -> region's grid-selected entries
//   -> (next idle) LLViewerRegion::updateVisibleEntries() -> createVisibleObjects()
//   -> gObjectList.
// That cull runs ONLY inside gPipeline.updateCull() (pipeline.cpp:2712), which lives
// in the diverted display() path. Under VK display() is diverted, so the cull never
// runs and the cache never drains -- the viewer sees only the objects that arrive via
// live UDP ObjectUpdates (measured ~1/3 of a region: 2701 vs GL's 8752). Drive the
// same per-region cull here each frame so the cache instantiates.
//
// display() supplies two things GL gets "for free" that we must reproduce (no GL):
//  (1) LLSpatialEntryData::sCurVisible is bumped by LLDrawable::incrementVisible()
//      in display(); the caller bumps it before us. Without it the "current
//      frame" is frozen and cull()'s "already culled this frame" guard (mCulledTime ==
//      getCurrentFrame()) makes it a one-shot no-op after the first frame.
//  (2) The camera's agent frustum planes are refreshed by updateFrustumPlanes(), which
//      reads the (dead-in-VK) GL matrices. Rebuild the 8 frustum corners from the live
//      LLViewerCamera params and call calcAgentFrustumPlanes() directly so cull()'s
//      frustum test is against the CURRENT camera, not a stale login frustum.
static void driveVulkanWorldVOCacheCull()
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
        return;
    if (!LLViewerRegion::sVOCacheCullingEnabled)
        return; // RequestFullRegionCache/ObjectCacheEnabled off -> nothing to drain

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    if (!cam)
        return;

    // (2) Refresh the world camera's agent frustum planes WITHOUT GL. Mirror the
    // non-ortho branch of LLViewerCamera::updateFrustumPlanes(): near quad in
    // BL/BR/TR/TL order (as calcAgentFrustumPlanes expects), far corners along the ray
    // origin->near-corner. The far distance does not change the side planes (same rays),
    // and cull() overrides the far plane anyway via calcRegionFrustumPlanes(mDrawDistance),
    // so any sane far works here.
    static LLCachedControl<F32> s_render_far(gSavedSettings, "RenderFarClip", 128.f);
    F32 farp = llmax(cam->getFar(), (F32)s_render_far);
    farp = llclamp(farp, 64.f, 8192.f);   // draw distance reaches 4096 m
    LLCullCamera::calcAgentFrustum(*cam, farp);

    // #479: the culling camera -- the live world camera just refreshed, or the frozen
    // debug pose (Develop > Rendering > Freeze culling camera), rebuilt in agent space.
    // Then the per-frame inputs of the cache cull: look-ahead (off by default) and the
    // metrics switch.
    LLCullCamera::update(farp);
    LLVOCacheLookAhead::update();
    LLVOCacheMetrics::beginFrame();
    LLCamera& cull_cam = LLCullCamera::get();

    // Drive the cache cull for every active region against the culling camera. Occlusion
    // OFF: VK has no GL occlusion queries, so frustum-only (errs toward creating more).
    // Fills each region's selection (the grid's entry list); the next idle's
    // updateRegions -> createVisibleObjects drains it into the world. Save/restore the
    // camera id the cull requires.
    const LLViewerCamera::eCameraID saved = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        if (!region)
            continue;
        LLVOCachePartition* vo_part = region->getVOCachePartition();
        if (vo_part)
        {
            LLTimer cull_timer;
            vo_part->cull(cull_cam);
            LLVOCacheMetrics::addCullTime(cull_timer.getElapsedTimeF64());
        }
    }
    LLVOCacheMetrics::tick();   // sampled outcome metrics + invariant check (RenderVKVOCacheMetrics)
    LLViewerCamera::sCurCameraID = saved;
}

bool vkRenderOffscreenSnapshot(S32 w, S32 h, bool show_hud, std::vector<U8>& rgb,
                               std::vector<F32>* depth, F32 proj[2], U32 aa_frames);   // (below)
U32 vkSnapshotAAFrames();                                        // (below)
U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames);                // (below)

void displayVulkanUI()
{
    if (!gViewerWindow || !gViewerWindow->getWindow())
    {
        return;
    }
    if (!gViewerWindow->getActive()
        || !gViewerWindow->getWindow()->getVisible()
        || gViewerWindow->getWindow()->getMinimized())
    {
        return;
    }

    // MILESTONE SELF-TEST (no login): when SLIPSTREAM_VK_SYNTHTEST is set, inject a
    // synthetic cube through the real clean-store ingest + render it offscreen to
    // a PNG once, then continue the normal UI present. The env value is the PNG
    // path (default under the app temp dir).
    {
        static bool s_synth_done = false;
        static const char* const synth = sl_getenv("VK_SYNTHTEST"); // read once, not per frame
        if (synth && !s_synth_done && LLVulkanContext::instance().isInitialized())
        {
            s_synth_done = true;
            std::string png = (synth[0] ? std::string(synth)
                                        : std::string("vk_synth_cube.png"));
            LLVKWorldFrame cam;
            const U32 n = vkSynthInjectCube(cam);
            if (n > 0)
            {
                const bool ok = LLVulkanContext::instance().runWorldSynthTest(png, cam);
                LL_INFOS("Vulkan") << "VK SYNTHTEST: instances=" << n
                                   << " png=" << png << " ok=" << (ok?1:0) << LL_ENDL;
            }
            else
            {
                LL_WARNS("Vulkan") << "VK SYNTHTEST: cube injection produced no "
                                      "instances; nothing rendered" << LL_ENDL;
            }
        }
    }

    LLVKWorldFrame world;
    auto uiRender = [&world]()
    {
        // DYNAMIC TEXTURES (#83): appearance composites, visual-param hints and the
        // upload / material previews render into their GPU targets as offscreen
        // segments (recorded ahead of the world and the UI that sample them). First,
        // before any world-overlay segment or retained-UI recording is open.
        {
            VK_HITCH_SCOPE("vk_dynamicTextures");
            LLViewerDynamicTexture::updateAllInstancesVK();
        }
        // WORLD 3D OVERLAYS (LL: the pipeline's post pass -- renderSelections,
        // LLTracker::render3D, property lines, parcel highlight, ban lines -- drawn
        // against the scene depth; diverted under VK). Issued as the facade's
        // world-overlay segment: recorded by the world renderer against the world
        // depth before the HUD pass, so LLRender::DepthScope works as in LL (lines hide
        // behind terrain/buildings; manipulators LL draws with depth off stay on
        // top). gGL is loaded with the world camera in VK clip convention.
        // (The selection outline itself is GPU-side in the world renderer. No
        // RENDER_DEBUG_FEATURE_UI gate: that mask reads 0 under VK.)
        // Tools that draw even with nothing selected (land brush, select land: #235) go
        // through renderSelections too, as in LL.
        const bool tool_always = LLToolMgr::instanceExists() && LLToolMgr::getInstance()->getCurrentTool()
            && LLToolMgr::getInstance()->getCurrentTool()->isAlwaysRendered();
        // Rect (box) select queues objects in mRectSelectedObjects; updateSilhouettes()
        // moves them into the highlighted set that mouse-up selects. LL ran it from
        // renderSelections every frame; here renderSelections only runs with a selection,
        // so sync while nothing is selected yet (otherwise box select selects nothing).
        bool box_drag = false;
        if (LLSelectMgr::instanceExists() && LLSelectMgr::getInstance()->getSelection()->isEmpty())
        {
            LLSelectMgr* sm = LLSelectMgr::getInstance();
            box_drag = sm->hasRectSelection();
            if (box_drag)
                sm->updateSilhouettes();
            else if (sm->getHighlightedObjects()->getNumNodes() > 0)
                sm->unhighlightAll();   // no drag: drop stale highlights (they'd keep drawing)
        }
        const bool has_sel = (LLSelectMgr::instanceExists()
            && (!LLSelectMgr::getInstance()->getSelection()->isEmpty() || box_drag)) || tool_always;
        // Ctrl+Alt+F1 (RENDER_DEBUG_FEATURE_UI off) hides the UI, as LL's display():
        // no 3D UI overlays, hover text / name tags, 2D UI or debug text. Selection
        // outlines stay (LL draws them in render_hud_elements regardless). Before
        // login the UI always draws.
        const bool ui_on = !world.enabled
            || gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (world.enabled && LLVulkanContext::instance().beginWorldOverlay())
        {
            VK_HITCH_SCOPE("vk_worldOverlays");
            LLViewerWindow::setVKOverlayCamera(world.proj, world.view);
            {
                gGL.color4f(1, 1, 1, 1);
                LLRender::DepthScope depth(true, false);
                GMDXRezFx::renderOverlay();   // <GMDX> rez effects: the transporter's beam (world content, UI on or off)
                gGL.color4f(1, 1, 1, 1);      // <GMDX>
                if (has_sel)
                    gViewerWindow->renderSelections(false, false, false);
                if (ui_on)
                {
                    LLTracker::render3D();
                    // Debug beacons (LL: render_ui_3d's renderObjectBeacons, deleted
                    // with the GL display path): queue the Develop/View > Beacons and
                    // telehub beacons, then draw everything queued since last frame
                    // (area search, pathfinding, poser, ...) at the current poses.
                    LLPipeline::addDebugBeacons();
                    // Floater-owned beacons are queued here every frame, not in the
                    // floaters' draw(): retained UI replays idle floaters without draw().
                    if (FSAreaSearch* as = LLFloaterReg::findTypedInstance<FSAreaSearch>("area_search"); as && as->getVisible())
                        as->addBeacons();
                    for (const char* pf : { "pathfinding_linksets", "pathfinding_characters" })
                        if (LLFloaterPathfindingObjects* f = LLFloaterReg::findTypedInstance<LLFloaterPathfindingObjects>(pf); f && f->getVisible())
                            f->addBeacons();
                    if (FSFloaterPoser* fp = LLFloaterReg::findTypedInstance<FSFloaterPoser>("fs_poser"); fp && fp->getVisible())
                        fp->drawOnHoverJointHint();
                    // Pose Studio IK handles (#530): alpha cones on the posed avatar.
                    if (SLPoseIK::instanceExists())
                        SLPoseIK::instance().render();
                    gObjectList.renderObjectBeacons();
                    gSky.addSunMoonBeacons();
                    // Develop > Render Metadata > Physics Shapes / Bounding Boxes (#29).
                    LLPipeline::renderDebugOverlaysVK();
                    if (LLWorld::instanceExists())
                        LLWorld::getInstance()->renderPropertyLines();   // ShowPropertyLines
                    LLViewerParcelMgr::getInstance()->render();            // selected-parcel highlight
                    LLViewerParcelMgr::getInstance()->renderParcelCollision(); // ban lines

                    // <GMDX> (The OTS aim-convergence debug lines are gone: they drew every
                    // avatar's hitbox in front of the camera through walls, a wallhack.)
                }
                // #479 frozen culling camera: its frustum, spheres and cache-entry boxes.
                LLVOCacheDebug::renderOverlay();
                gGL.flush();
            }
            LLVulkanContext::instance().endWorldOverlay();
        }
        else if (world.enabled && has_sel)
        {
            LLViewerWindow::setVKOverlayCamera(world.proj, world.view);
            gViewerWindow->renderSelections(false, false, false);
            gGL.flush();
        }
        // HUD edit overlays (HUD camera; 2D layer, no depth -- the HUD pass owns
        // the depth buffer by now).
        if (world.enabled && world.hud_enabled && has_sel)
        {
            LLViewerWindow::setVKOverlayCamera(world.hud_proj_ui, world.hud_model);
            gViewerWindow->renderSelections(false, false, true);
            gGL.flush();
        }
        // WORLD-SPACE HUD TEXT (LL: display()'s "UI 2D" LLHUDObject::renderAll and the
        // pipeline's renderHUD LLHUDText::renderAllHUD, both diverted under VK):
        // avatar name tags + chat bubbles and prim hover text. Bubble backgrounds are
        // world-space quads (world camera loaded); the text itself projects through
        // get_current_* and draws in a 2D ortho. Drawn over the world, under the UI.
        // (No RENDER_DEBUG_FEATURE_UI gate: that mask is only filled by the skipped
        // gPipeline.init() and reads 0 under VK.)
        if (!ui_on && LLHUDManager::instanceExists())
        {
            LLHUDObject::renderAllForTimer();   // LL: make sure particle effects disappear
        }
        else if (world.enabled && LLHUDManager::instanceExists())
        {
            VK_HITCH_SCOPE("vk_hudText");
            // IN-WORLD TEXT UNDER THE HUDS (#468): LL draws name tags / hover text /
            // bubbles before render_hud_attachments, so HUD attachments cover them.
            // They go to the pre-HUD layer, recorded by the world renderer between its
            // overlay segment and its HUD pass. LLHUDText::render() skips text on HUD
            // attachments (mOnHUDAttachment -> sVisibleHUDTextObjects); that text is
            // drawn by renderAllHUD below, after the layer, so it stays over its HUD.
            // IN-WORLD TEXT OCCLUSION (#487): with the world camera the layer tests each
            // anchored label (name tags, hover text with LL's z-compare on) once, at its
            // anchor, against the scene depth -- hidden / faded as a whole, as LL's
            // depth-tested tags. Beacon labels and debug text stay on top (no anchor).
            static LLCachedControl<bool> s_text_occl(gSavedSettings, "RenderVKTextOcclusion", true);
            const bool pre_hud = s_text_occl
                ? LLVulkanContext::instance().beginPreHudLayer(world.proj, world.view)
                : LLVulkanContext::instance().beginPreHudLayer();
            LLViewerWindow::setVKOverlayCamera(world.proj, world.view);
            LLHUDObject::renderAll();
            gObjectList.renderObjectBeaconLabels();   // world camera, before the HUD swap
            if (pre_hud)
            {
                gGL.flush();
                LLVulkanContext::instance().endPreHudLayer();
            }
            if (world.hud_enabled && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD))
            {
                LLViewerWindow::setVKOverlayCamera(world.hud_proj_ui, world.hud_model);
                // hud_render_text projects each label with the "current GL matrices"
                // (get_current_projection/modelview). Under VK those hold the WORLD
                // camera for the whole frame (buildVulkanWorldFrame), so HUD text was
                // projected through the world camera and drawn in-world near the
                // region origin (#92). LL's GL HUD pass loads the HUD matrices first;
                // do the same (GL convention, from get_hud_matrices) and restore.
                extern bool get_hud_matrices(glm::mat4& proj, glm::mat4& model);
                glm::mat4 hproj, hmodel;
                const bool hud_mats = get_hud_matrices(hproj, hmodel);
                {
                    static LLCachedControl<bool> s_diag(gSavedSettings, "RenderVKHudTextDiag", false);
                    static F64 s_next = 0.0;
                    if (s_diag && LLTimer::getTotalSeconds() >= s_next)
                    {
                        s_next = LLTimer::getTotalSeconds() + 2.0;
                        LL_INFOS("VKHudText") << "HUD text pass: hud matrices " << (hud_mats ? "ok" : "MISSING") << LL_ENDL;
                    }
                }
                if (hud_mats)
                {
                    const glm::mat4 saved_proj = get_current_projection();
                    const glm::mat4 saved_view = get_current_modelview();
                    set_current_projection(hproj);
                    set_current_modelview(hmodel);
                    LLHUDText::renderAllHUD();
                    set_current_projection(saved_proj);
                    set_current_modelview(saved_view);
                }
            }
            gGL.flush();
        }
        // Beacons are queued per frame (floater draws below, the overlay pass above)
        // and drawn exactly once: drop them unconditionally, whether or not the
        // overlay ran, so the list can never grow without bound.
        gObjectList.resetObjectBeacons();
        // RLVa @setoverlay (LL order: after the HUD text, before the 2D UI). A restriction:
        // drawn even with the UI hidden. Under VK the HUD attachments are already in the
        // frame (recordWorld's HUD pass), so the overlay covers them too.
        if (world.enabled && RlvActions::hasBehaviour(RLV_BHVR_SETOVERLAY))
        {
            LLVfxManager::instance().runEffect(EVisualEffect::RlvOverlay);
            gGL.flush();
        }
        if (!ui_on)
            return;
        // 2D ortho for the UI (gGL -> facade builds a Vulkan-correct ortho).
        gViewerWindow->setup2DRender();
        // The real UI tree: gUIProgram.bind() inside is a no-op under VK; the
        // login panel, fields, labels, text, logo all render here via gGL.
        VK_HITCH_SCOPE("ui_draw");
        vkUiProbeFrame();
        gViewerWindow->draw();
        gGL.flush();
        // Develop > Show Info overlay (camera, time, render info, color under
        // cursor, ...): LL updates+draws it at the end of the GL display(), which
        // is diverted under VK, so none of those lines ever showed. Same order as
        // LL: after the 2D UI, on top of it.
        gViewerWindow->setup2DRender();
        gViewerWindow->updateDebugText();
        gViewerWindow->drawDebugText();
        gGL.flush();
    };

    // VK-SIDE INGEST TICK (step 3). gPipeline.init() is skipped in VK, so the GL
    // renderFrame() that normally drives the clean-store sweep never runs. Once
    // in-world, drive it here instead: ensure the stores are CPU-init'd (idempotent)
    // and run ONE selfRegisterSweep slice per frame -- the
    // exact CPU walk (gObjectList -> registerCleanFace -> capture/upsert/
    // registerLodSlot) the GL path used, now GL-guarded so it stages into the CPU
    // mirrors that syncFromStores() mirrors into VkBuffers. This makes the real sim
    // ingest in VK. STATIC-UNTEXTURED SCOPE for this chunk: materials are forced
    // empty in fillFromCleanStores (white-dummy tint), and rigged draw is a later
    // chunk; the sweep's attachment/skin calls are CPU-only (no crash) but rigged
    // geometry won't render until the rigged VK chunk. NOT image-verified here (no
    // login in the milestone); the cube proof exercises the same capture->stores->
    // fillFromCleanStores->VK-draw path this feeds.
    if (LLStartUp::getStartupState() >= STATE_STARTED)
    {
        // VK FIRST-CLASS Phase 2: re-home the per-frame side-effects that live ONLY in
        // the diverted display() GL body (llviewerdisplay.cpp ~635-648). Without them VK
        // ran with a FROZEN frame clock -- gFrameCount and LLTextureData::sLastFrameTime
        // never advanced -- so texture aging, "recently bound", fetch throttling and LOD
        // hysteresis were all stuck. THIS is why assets never reached full res under VK
        // (the discard-stamp fix was necessary but not sufficient while the clock stood
        // still). Advance the clocks BEFORE the texture subsystem below reads them.
        gFrameCount++;
        if (gFocusMgr.getAppHasFocus()) gForegroundFrameCount++;
        LLTextureData::updateStats(gFrameTimeSeconds);   // sets sLastFrameTime (texture age clock)

        static LLCachedControl<S32>  avatar_name_tag_mode(gSavedSettings, "AvatarNameTagMode", 1);
        static LLCachedControl<bool> name_tag_show_group_titles(gSavedSettings, "NameTagShowGroupTitles", true);
        LLVOAvatar::sRenderName        = avatar_name_tag_mode;
        LLVOAvatar::sRenderGroupTitles = name_tag_show_group_titles && avatar_name_tag_mode > 0;

        { VK_HITCH_SCOPE("vk_checkSettings"); gViewerWindow->checkSettings(); }

        // GATE 1 for the VOCache-cull driver below: advance the "current frame" counter
        // that display() normally bumps (LLDrawable::incrementVisible()). Without this,
        // sCurVisible is frozen and every visibility/aging check -- most importantly
        // cull()'s "already culled this frame" guard -- misbehaves.
        LLDrawable::incrementVisible();

        // Seed gPipeline's render-type mask ONCE. gPipeline.init() is skipped in VK, so
        // mRenderTypeEnabled[] stays all-false -- which makes the pick filters
        // (vkPickFiltersPass) and every other hasRenderType gate reject everything,
        // breaking picking. setAllRenderTypes() only sets the CPU mask array (no GL).
        // Harmless where VK owns rendering; required for the pick rules.
        static bool s_vk_render_types_seeded = false;
        if (!s_vk_render_types_seeded)
        {
            s_vk_render_types_seeded = true;
            gPipeline.setAllRenderTypes();
        }

        LLCleanMeshAsset::instance().init();
        LLCleanInstanceStore::instance().init();
        { VK_HITCH_SCOPE("vk_ingestTick"); LLCleanIngest::instance().vkIngestTick(); }

        // GL-A2 (#47): drain LLGLUpdate::sGLQ, as GL's display() does. Every updateGL
        // left is CPU work -- terrain patch composition stats (clears mSTexUpdate, so
        // dirty patches finally leave LLSurface's dirty list instead of regenerating
        // every frame), the parcel overlay texture, avatar updateMeshTextures (bakes
        // that arrive after the appearance message). Under VK nothing drained it.
        { VK_HITCH_SCOPE("vk_glUpdateQ"); gPipeline.updateGL(); }

        // VK WORLD-TEXTURE FETCH DRIVER (finding #1).
        // In VK mode display() is diverted to this function, so the GL renderer's
        // gTextureList.updateImages() (which normally dispatches decodes AND, via
        // updateImagesCreateTextures -> LLTextureData::setFromRaw, retains decoded
        // pixels as mVKCpuData) never runs, and nothing
        // computes per-face texture priority. Result: world materials' base-color
        // pixels never become CPU-resident, and LLCleanRendererVK::
        // uploadRealMaterialTextures() falls back to white. Drive it minimally
        // here: boost a bounded slice of live material base-color textures, then
        // pump one small updateImages budget so their pixels decode into
        // mVKCpuData. The create step it triggers is the same retain path the
        // UI/font preload already uses.
        //
        // STATUS: COMPILE-VERIFIED ONLY -- NOT image-verified. The synthetic-cube
        // milestone does not exercise this (it injects its checkerboard directly).
        // A real logged-in world textured render is a follow-on chunk; this driver
        // is the minimal seam so world textures can begin resolving in VK.
        // Run the STOCK texture subsystem exactly as display() does (llviewerdisplay.cpp
        // ~544-563). display() is diverted under VK, so without this LLViewerTexture::
        // updateClass() (VRAM accounting + discard-bias management) never runs and the
        // texture list is starved -- assets never reach full-res decode (sculpt maps
        // stall coarse, LODs stuck, objects don't finish loading). updateImages() drives
        // updateImageDecodePriority (which computes per-face calcPixelArea itself, so it
        // works without the render path) + updateFetch over a rolling slice of ALL
        // textures, sculpt maps included. Replaces an earlier "minimal seam" that only
        // force-boosted material base-color textures -- which stripped their own face-
        // path priority (BOOST_HIGH skips it) and starved decode.
        if (!gNonInteractive)
        {
            // Sustain sculpt-map fetch priority every frame (not just when the amortized
            // sweep visits each object), so pending sculpt maps decode to full res --
            // must run BEFORE updateImages so the priority is set for this frame's fetch.
            { VK_HITCH_SCOPE("vk_sculptFetch"); LLVOVolume::vkDriveSculptFetch(); }
            { VK_HITCH_SCOPE("LL_texUpdateClass"); LLViewerTexture::updateClass(); }
            // RAM (#159, Linux): texture pixel copies are decoded on worker threads, so glibc
            // spreads them across per-thread arenas, and freed arena memory is not handed
            // back to the OS -- thumbnailing GBs of CPU copies left RSS where it was.
            // After a large drop, trim once on a worker thread (malloc_trim walks and locks
            // the arenas; never on the main thread). Windows returns large blocks itself.
#if LL_LINUX
            {
                static S64 s_trim_high = 0;
                static F64 s_next_trim = 0.0;
                const S64 cpu_now = LLTextureData::getVKCpuBytes();
                s_trim_high = llmax(s_trim_high, cpu_now);
                const F64 now_t = LLTimer::getTotalSeconds();
                // RAM #262: under memory pressure / over the retained allowance (the
                // arrival burst), hand freed copies back sooner and in smaller steps.
                const bool urgent = LLMemoryGovernor::shouldTrimAggressively();
                const S64 trim_step = urgent ? (128ll << 20) : (256ll << 20);
                if (now_t >= s_next_trim && s_trim_high - cpu_now >= trim_step)
                {
                    s_next_trim = now_t + (urgent ? 2.0 : 10.0);
                    s_trim_high = cpu_now;
                    if (LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General"))
                        q->post([]() { malloc_trim(0); });
                }
            }
#endif
            F32 max_image_decode_time = 0.050f * gFrameIntervalSeconds.value();          // 50 ms/second
            max_image_decode_time     = llclamp(max_image_decode_time, 0.002f, 0.005f);  // 2-5 ms/frame
            VK_HITCH_SCOPE("LL_updateImages");
            gTextureList.updateImages(max_image_decode_time);
        }

        // Drain the region object cache into the world (VK skips gPipeline.updateCull,
        // which is the only place this cull runs in GL). Fills each region's visible
        // group set; idle's LLWorld::updateRegions() then instantiates the objects.
        { VK_HITCH_SCOPE("vk_voCacheCull"); driveVulkanWorldVOCacheCull(); }

        // AVATAR VISIBILITY (VK). LLVOAvatar::isVisible() is mDrawable->isVisible(), which
        // only LL's cull sets (LLPipeline::markVisible) -- diverted under VK, so EVERY
        // avatar read as invisible: other avatars and animesh ran HIDDEN_UPDATE (motions
        // never evaluated a pose), updateTextures() early-outs (bakes never prioritized ->
        // BOM stays IMG_DEFAULT gray), and their texture loaded-callbacks were paused.
        // Supply the cull's result: an avatar whose bounds meet the camera frustum is
        // visible this frame. Runs right after incrementVisible + the frustum refresh
        // above, so idle's updateCharacter next frame sees the same sCurVisible, exactly
        // as GL's display()-cull -> idle ordering.
        {
            VK_HITCH_SCOPE("vk_avatarVis");
            LLViewerCamera* vcam = LLViewerCamera::getInstance();
            for (LLCharacter* ch : LLCharacter::sInstances)
            {
                LLVOAvatar* av = (LLVOAvatar*)ch;
                if (!av || av->isDead() || av->mDrawable.isNull() || av->isUIAvatar())
                    continue;
                // Animated extents + camera distance: GL refreshes both from the
                // pipeline's move/cull (updateInGroup -> updateSpatialExtents,
                // updateDistance), diverted under VK. calcLOD of every attachment/
                // rigged mesh reads them (avatar box distance + radius), so stale
                // login-time values pinned worn mesh at a low LOD. (Not updateDistance
                // itself: for an avatar it also runs LLVOAvatar::updateLOD -> system
                // mesh GL buffer work.)
                av->mDrawable->updateSpatialExtents();
                {
                    // Same as LLDrawable::updateDistance's non-volume branch.
                    LLVector3 pos(av->mDrawable->getPositionGroup().getF32ptr());
                    pos -= vcam->getOrigin();
                    av->mDrawable->mDistanceWRTCamera = ll_round(pos.magVec(), 0.01f);
                }
                // Avatar bounds: root position + a radius that covers the body and
                // typical attachments (drawable radius is only maintained by the
                // diverted pipeline update, so floor it).
                const F32 rad = llmax(av->mDrawable->getRadius(), 3.f);
                bool in_view = vcam->sphereInFrustum(av->mDrawable->getPositionAgent(), rad);
                // ANIMESH: a control avatar's own position is only synced to its object
                // (LLControlAvatar::matchVolumeTransform) by its VISIBLE updateCharacter,
                // so a stale position could keep it hidden and never re-synced. Also test
                // the animesh object itself, whose position is always current.
                if (!in_view && av->isControlAvatar())
                {
                    LLVOVolume* root = ((LLControlAvatar*)av)->mRootVolp;
                    if (root && !root->isDead() && root->mDrawable.notNull())
                        in_view = vcam->sphereInFrustum(root->getRenderPosition(),
                                                        llmax(root->mDrawable->getRadius(), rad));
                }
                // #631: the renderer draws an avatar (LLCleanRendererVK::computeAvatarVis)
                // from its RENDER position -- for a seated avatar the seat's render matrix,
                // not the drawable's own world position tested above -- and from its
                // animated extents (LL's cull also went by the extents). An avatar the
                // renderer draws but this stamp misses runs HIDDEN_UPDATE every frame,
                // which never poses it: it stays in its rest pose for as long as that
                // lasts, and neither Refresh Animations nor Reset Skeleton (which pose
                // nothing themselves) can help. Test the same inputs before calling it
                // hidden.
                if (!in_view)
                {
                    in_view = vcam->sphereInFrustum(av->getRenderPosition(), rad);
                }
                if (!in_view)
                {
                    const LLVector4a* ext = av->mDrawable->getSpatialExtents();
                    LLVector4a center, half;
                    center.setAdd(ext[0], ext[1]);
                    center.mul(0.5f);
                    half.setSub(ext[1], ext[0]);
                    half.mul(0.5f);
                    const F32* h = half.getF32ptr();
                    const F32* c = center.getF32ptr();
                    bool sane = true;
                    for (int k = 0; k < 3; ++k)
                    {
                        if (!std::isfinite(c[k]) || !std::isfinite(h[k]) || h[k] < 0.f || h[k] > 256.f) sane = false;
                    }
                    if (sane)
                    {
                        in_view = vcam->AABBInFrustum(center, half) != 0;
                    }
                }
                if (in_view)
                {
                    av->mDrawable->setVisible();
                }
            }
        }
        LLVOAvatar::logBakeDiag();

        // HUD EFFECTS (VK). Effect updates and lifetimes ride LL's display() path
        // (updateEffects + renderAllForTimer, llviewerdisplay.cpp), diverted under VK:
        // selection / point-at / beam effects never expired, so their particle sources
        // were never marked dead and the selection sparkles emitted forever. Tick them
        // here (the spiral's and beam's renderForTimer are GL-free under VK).
        // TELEPORT (VK): drive display()'s teleport state machine + progress screen.
        { VK_HITCH_SCOPE("vk_teleport"); display_update_teleport(); }

        {
            static LLCachedControl<U32> s_ui_tex_mb(gSavedSettings, "RenderVKUITexCacheMB", 128);
            static LLCachedControl<U32> s_ui_tex_idle(gSavedSettings, "RenderVKUITexIdleSec", 30);
            LLRenderVKBackend::sUICacheBudgetMB = (U32)s_ui_tex_mb;
            LLRenderVKBackend::sUIIdleEvictSec = (U32)s_ui_tex_idle;
        }
        if (LLHUDManager::instanceExists())
        {
            VK_HITCH_SCOPE("vk_hudEffects");
            LLHUDManager::getInstance()->updateEffects();
            // Text/name-tag visibility, positions, overlap layout and dead-object
            // purge (LL: display()'s "HUD Update", diverted under VK) -- without it no
            // nametag / hover text was ever marked visible.
            LLHUDObject::updateAll();
            LLHUDObject::renderAllForTimer();
        }

        // ENTRY-DATA UPKEEP (VK). Re-homed from display()'s "Update Geom" block
        // (llviewerdisplay.cpp: processPartitionQ). Drawables queued by markPartitionMove
        // (shrink-wrap, a LOD bin-radius change) refresh their bin radius and entry data
        // (LLDrawable::movePartition; the spatial partitions it re-binned them in are
        // gone, #480). Without it mPartitionQ never drained and PARTITION_MOVE stuck set
        // (so later changes were never re-queued). updateGeom itself is NOT called: it
        // also builds mBuildQ1 drawables into GL vertex buffers.
        { VK_HITCH_SCOPE("vk_partitionQ"); gPipeline.processPartitionQ(); }

        // PROCESS SCHEDULED PICKS (click/select). Clicks schedule async picks via
        // pickAsync->schedulePick (input path, runs in VK), but performPick() -- which
        // drains them via LLPickInfo::fetchResults() -> cursorIntersect (a CPU analytic
        // raycast, zero GL) -> selection -- is only called from the diverted display()
        // path (llviewerdisplay.cpp). Drive it here so in-world click/select works in VK.
        // Cheap: early-outs when no picks are queued.
        { VK_HITCH_SCOPE("vk_performPick"); gViewerWindow->performPick(); }

        // Re-homed from display() (llviewerdisplay.cpp:569): reap dead GLTF materials
        // so the material store / bindless table don't leak entries across the session.
        { VK_HITCH_SCOPE("vk_flushGLTF"); gGLTFMaterialList.flushMaterials(); }
    }

    // Re-home from display() (llviewerdisplay.cpp:1218): clear the per-frame region-
    // shift flag. It is set true on a region-origin shift (llviewermessage.cpp:3258)
    // and, in GL, cleared at the END of display() so object distances recompute the
    // next frame. display() is diverted under VK, so after the first shift (which
    // happens at login) gShiftFrame stayed true FOREVER -- and LLDrawable::
    // updateDistance early-returns while it is set, so mDistanceWRTCamera stayed 0,
    // calcLOD never raised mLOD, and every mesh was frozen at the lowest LOD (prims
    // generate detail locally so they were unaffected -- the deterministic mesh-only
    // "stuck low LOD" split). Clear it each frame exactly as display() did.
    {
        extern bool gShiftFrame;
        gShiftFrame = false;
    }

    { VK_HITCH_SCOPE("buildWorldFrame"); buildVulkanWorldFrame(world); }   // world.enabled == false until in-world
    // Cursor ray for the GPU raster pick (#702): answered by this frame's GPU work.
    LLCleanCursorPick::fillRequest(world);
    SSCombatRay::fillRequests(world);   // <SS:Skoomastream> OTS camera / aim rays (aux picks)
    // ... and for the GPU particle pick (#481), only while a per-frame answer is wanted.
    LLCleanPartPick::fillRequest(world);
    // ... and the acoustics probe (#683, AudioAcoustics): listener + in-world voices.
    LLCleanAcoustics::fillRequest(world);

    // DEVICE LOST (#135): focus history for the report (#209 was lost in the background),
    // and the testing aid -- RenderVKDebugSimulateDeviceLost 1 / 2 / 3 makes the next
    // queue submit / fence-or-semaphore wait / present report VK_ERROR_DEVICE_LOST without
    // touching the driver (one-shot: the setting resets to 0).
    {
        const bool focus = gFocusMgr.getAppHasFocus();
        if (!sVkFocusKnown || focus != sVkFocusLast)
        {
            sVkFocusKnown = true;
            sVkFocusLast = focus;
            sVkFocusChangedAt = LLFrameTimer::getElapsedSeconds();
        }
        static LLCachedControl<U32> s_sim_lost(gSavedSettings, "RenderVKDebugSimulateDeviceLost", 0);
        if (s_sim_lost != 0)
        {
            const U32 site = s_sim_lost;
            gSavedSettings.setU32("RenderVKDebugSimulateDeviceLost", 0);
            LL_WARNS("Vulkan") << "RenderVKDebugSimulateDeviceLost " << site
                               << ": simulating a graphics device loss (testing aid; the GPU is fine)" << LL_ENDL;
            vkSimulateDeviceLost((int)site);
        }
        // #672: also armable mid-session (e.g. before enabling a feature that compiles).
        static LLCachedControl<U32> s_sim_oom(gSavedSettings, "RenderVKDebugSimulatePipelineOOM", 0);
        if (s_sim_oom != 0)
        {
            const U32 k = s_sim_oom;
            gSavedSettings.setU32("RenderVKDebugSimulatePipelineOOM", 0);
            LL_WARNS("VKPipe") << "RenderVKDebugSimulatePipelineOOM " << k << ": the next " << k
                               << " pipeline creations report VK_ERROR_OUT_OF_HOST_MEMORY (testing aid)" << LL_ENDL;
            vkSimulatePipelineOom(k);
        }
        // #672: pipelines the driver could not compile (out of memory, after the retries)
        // leave their features off for the session: tell the user once.
        static unsigned s_pipe_failed_seen = 0;
        const unsigned pipe_failed = vkPipelineFailedCount();
        if (pipe_failed != s_pipe_failed_seen)
        {
            if (!s_pipe_failed_seen)
            {
                LLSD args;
                args["MESSAGE"] = llformat("The graphics driver ran out of memory while preparing shaders (%u could not "
                                           "be built). Slipstream kept running, but some visual effects or surfaces may "
                                           "be missing this session.\n\nRestarting the viewer usually fixes this: the "
                                           "shaders built so far are kept. Closing other memory-hungry programs helps.",
                                           pipe_failed);
                LLNotificationsUtil::add("GenericAlert", args);
            }
            LL_WARNS("VKPipe") << "#672: " << pipe_failed << " pipeline(s) failed to compile this session (out of memory);"
                               << " their features are off" << LL_ENDL;
            s_pipe_failed_seen = pipe_failed;
        }
    }

    { VK_HITCH_SCOPE("renderFrame"); LLVulkanContext::instance().renderFrame(uiRender, &world); }
    // <GMDX> region map capture: its step for this frame (a preparation render or one map tile)
    { VK_HITCH_SCOPE("gmdx_regionMap"); GMDXRegionCapture::frameTick(); }

    // REPRO AID (RenderVKDebugSnapshotStress N > 0): every N frames an offscreen snapshot
    // render, cycling sizes / kinds (thumbnail, window size, square with HUD, half-size
    // depth, larger than the window), exactly as the Snapshot floater / 360 snapshots do
    // -- the size round trips that re-create the shared screen targets.
    {
        static LLCachedControl<U32> s_stress(gSavedSettings, "RenderVKDebugSnapshotStress", 0);
        static U32 s_count = 0, s_phase = 0, s_done = 0;
        if (s_stress > 0 && world.enabled && gViewerWindow)
        {
            if (++s_count >= (U32)s_stress)
            {
                s_count = 0;
                const S32 ww = llmax(64, gViewerWindow->getWindowWidthRaw());
                const S32 wh = llmax(64, gViewerWindow->getWindowHeightRaw());
                struct Shot { S32 w, h; bool hud, depth; };
                const Shot shots[5] = {{700, 381, false, false},
                                       {ww, wh, false, false},
                                       {512, 512, true, false},
                                       {llmax(64, ww / 2), llmax(64, wh / 2), false, true},
                                       {ww + ww / 4, wh + wh / 4, false, false}};
                Shot sh = shots[s_phase++ % 5];
                // rawSnapshot's limits (vkSnapshotFit): the device's image size, the video
                // and system memory (#730), supersampled as the Snapshot floater would (#417).
                const U32 aa = vkSnapshotFit(sh.w, sh.h, sh.depth ? 0u : vkSnapshotAAFrames());
                std::vector<U8> rgb;
                std::vector<F32> depth;
                F32 proj[2] = {0.f, 0.f};
                const bool ok = vkRenderOffscreenSnapshot(sh.w, sh.h, sh.hud, rgb, sh.depth ? &depth : nullptr, proj, aa);
                if ((++s_done % 50u) == 1u || !ok)
                    LL_INFOS("Vulkan") << "snapshot stress #" << s_done << ": " << sh.w << "x" << sh.h
                                       << (sh.hud ? " +HUD" : "") << (sh.depth ? " depth" : "")
                                       << (aa ? " AA x" + std::to_string(aa) : std::string())
                                       << (ok ? "" : " FAILED") << LL_ENDL;
            }
        }
        else
            s_count = 0;
    }
}

// SNAPSHOT SUPERSAMPLING (#417): how many jittered frames a world snapshot averages --
// SnapshotAntialias (the Snapshot floater's "Antialias (slower)") x
// RenderSnapshotTemporalFrames (default 16, at most 64); 0 = the old single frame.
U32 vkSnapshotAAFrames()
{
    static LLCachedControl<bool> s_aa(gSavedSettings, "SnapshotAntialias", true);
    static LLCachedControl<U32> s_frames(gSavedSettings, "RenderSnapshotTemporalFrames", 16);
    return s_aa ? llmin((U32)s_frames, 64u) : 0u;
}

// What limited the last vkSnapshotFit below the requested size ("" = nothing): the
// Snapshot floater tells the user (LLViewerWindow::rawSnapshot, #730).
static std::string sVKSnapshotFitLimit;
const std::string& vkSnapshotFitLimit() { return sVKSnapshotFitLimit; }

// Offscreen snapshot render size (#730): the requested w x h within the device's image
// limit, the video memory and the system memory; the result is upscaled to the requested
// size. Returns the supersampling frames that fit: the requested size keeps priority --
// the accumulation is dropped (old single-frame render) before the image is shrunk.
//
// VIDEO MEMORY. The render resizes the renderer's resolution-sized images (G-buffer, HDR,
// depth, AO, SSR / TAA history, bloom chain, VRS, RT shadow mask ...) to w x h. Their cost
// per pixel is MEASURED from what the renderer holds at the live view's size (the old
// fixed ~100 B/px guess missed the RT shadow mask alone, ~44 B/px, and #730's 8192x4320
// request on an 8 GB card ran a hard target allocation out of memory -> fatal). Most of
// the live images are resized in place, so ~3/4 of their bytes come back (the SSR history
// pair and the scaled-render targets stay). A supersampled snapshot adds its accumulators
// (scene + glow, RGBA16F: 16 B/px). Usable: 85% of the smaller of the OS budget's and the
// renderer's own budget's free memory.
// SYSTEM MEMORY. The readback (~4 B/px), the RGB copy and the raw image (3 + 3 B/px) at the
// render size, plus the upscaled output at the requested size (3 B/px), within what the
// RAM governor reports available above its reserve.
U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames)
{
    sVKSnapshotFitLimit.clear();
    if (w <= 0 || h <= 0) return 0;
    const S32 req_w = w, req_h = h;
    const F64 req_px = (F64)w * h;
    const F32 lim = (F32)llmax((U32)1024, LLVulkanContext::instance().maxImageDimension());
    F32 f = llmin(1.f, llmin(lim / w, lim / h));
    if (f < 1.f) sVKSnapshotFitLimit = llformat("graphics card's largest image size (%.0f px)", lim);
    // Pixels allowed so far, and a cap on them with what set it.
    F64 px = req_px * f * f;
    auto cap = [&](F64 px_max, const std::string& why)
    {
        if (px <= px_max) return;
        px = llmax(px_max, 0.0);
        sVKSnapshotFitLimit = why;
    };

    const LLVulkanContext::SnapshotVram sv = LLVulkanContext::instance().snapshotVram();
    if (sv.usable > 0 || sv.liveTargetBytes > 0)   // (both 0: no budget known -- the old unlimited path)
    {
        constexpr F64 kMinBytesPerPx = 100.0;   // the live targets not made yet (first frames)
        const F64 bpp = llmax(kMinBytesPerPx, sv.livePx ? (F64)sv.liveTargetBytes / (F64)sv.livePx : 0.0);
        const F64 usable = (F64)sv.usable * 0.85 + (F64)sv.liveTargetBytes * 0.75;
        if (aa_frames > 0 && px * (bpp + 16.0) > usable)
        {
            LL_INFOS("Snapshot") << "snapshot " << req_w << "x" << req_h << ": not enough free video memory to antialias ("
                                 << (U64)(px * (bpp + 16.0) / 1048576.0) << " MB wanted, " << (U64)(usable / 1048576.0)
                                 << " MB usable); rendering it without" << LL_ENDL;
            aa_frames = 0;
        }
        cap(usable / (bpp + (aa_frames ? 16.0 : 0.0)),
            llformat("free video memory (%llu MB usable, about %.0f bytes per pixel with the current graphics settings)",
                     (unsigned long long)(usable / 1048576.0), bpp));
    }
    if (LLMemoryGovernor::getTotalRAM() > 0)
    {
        const U64 avail = LLMemoryGovernor::getAvailBytes();
        const U64 reserve = LLMemoryGovernor::getReserveBytes();
        const F64 ram = avail > reserve ? (F64)(avail - reserve) : 0.0;
        const F64 out_bytes = req_px * 3.0;   // the upscaled result, whatever the render size
        cap(ram > out_bytes ? (ram - out_bytes) / 10.0 : 0.0,
            llformat("free system memory (%llu MB above the reserve)", (unsigned long long)(ram / 1048576.0)));
    }
    // Below a usable picture (or no room for the result at all): refused, w = h = 0.
    constexpr F64 kMinPx = 256.0 * 256.0;
    if (px < llmin(kMinPx, req_px))
    {
        LL_WARNS("Snapshot") << "snapshot " << req_w << "x" << req_h << " refused: " << sVKSnapshotFitLimit
                             << " (video memory usable " << (sv.usable >> 20) << " MB, live targets "
                             << (sv.liveTargetBytes >> 20) << " MB at " << sv.livePx << " px)" << LL_ENDL;
        w = h = 0;
        return 0;
    }
    if (px < req_px * f * f) f = (F32)sqrt(px / req_px);
    w = llclamp((S32)(w * f), 1, req_w);
    h = llclamp((S32)(h * f), 1, req_h);
    if (w != req_w || h != req_h)
        LL_INFOS("Snapshot") << "snapshot " << req_w << "x" << req_h << " renders at " << w << "x" << h
                             << " and is scaled up (limited by " << sVKSnapshotFitLimit << "; video memory usable "
                             << (sv.usable >> 20) << " MB, live targets " << (sv.liveTargetBytes >> 20) << " MB at "
                             << sv.livePx << " px)" << LL_ENDL;
    return aa_frames;
}

// Vulkan offscreen snapshot (LLViewerWindow::rawSnapshot / simpleSnapshot): the world
// rendered at exactly w x h (camera aspect + pixel height set to the snapshot's for
// the frame build, so the frustum, LOD, HUD and DoF match), no UI. HUD only for
// colour snapshots. `depth` (optional) receives the raw [0,1] depth, rows top-down,
// and `proj` the projection's z terms {P[10], P[14]} to linearize it. aa_frames > 0
// (colour only, #417): supersampled over that many jittered frames (vkSnapshotFit).
bool vkRenderOffscreenSnapshot(S32 w, S32 h, bool show_hud, std::vector<U8>& rgb,
                               std::vector<F32>* depth, F32 proj[2], U32 aa_frames)
{
    if (w <= 0 || h <= 0) return false;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 old_aspect = cam->getAspect();
    const S32 old_height = cam->getViewHeightInPixels();
    cam->setAspect((F32)w / (F32)h);
    cam->setViewHeightInPixels(h);
    sVKViewHeightOverride = h;
    // #919: the HUD over the whole image at the image's aspect (the attach points keep the
    // window's HUD layout, as LL's snapshots): only the HUD projection's aspect changes.
    gViewerWindow->setHudProjectionAspectOverride((F32)w / (F32)h);
    // Speed-based FoV (#663) is a live-view effect: snapshots use the user's FoV.
    const F32 old_view = cam->getView();
    if (cam->getSpeedFovBoost() != 0.f) cam->setViewNoBroadcast(old_view - cam->getSpeedFovBoost());
    LLVKWorldFrame wf;
    buildVulkanWorldFrame(wf);
    if (cam->getView() != old_view) cam->setViewNoBroadcast(old_view);
    gViewerWindow->setHudProjectionAspectOverride(0.f);
    sVKViewHeightOverride = 0;
    cam->setAspect(old_aspect);
    cam->setViewHeightInPixels(old_height);
    if (!wf.enabled) return false;
    if (show_hud && wf.hud_enabled && !depth)
    {
        wf.hud_viewport[0] = 0; wf.hud_viewport[1] = 0;
        wf.hud_viewport[2] = w; wf.hud_viewport[3] = h;
    }
    else
    {
        wf.hud_enabled = false;
    }
    if (proj) { proj[0] = wf.proj[10]; proj[1] = wf.proj[14]; }
    return LLVulkanContext::instance().renderOffscreen(wf, (uint32_t)w, (uint32_t)h, rgb, depth, 2u,
                                                       depth ? 0u : (uint32_t)aa_frames);
}

// <GMDX> REGION MAP CAPTURE (gmdxregioncapture.h): the world straight down through an orthographic
// box, offscreen, as bottom-up RGB. The frame is this frame's (environment, probes, the instance
// store), with the camera replaced:
//  - view: down -Z, +Y (north) up, +X (east) right; projection: reversed-Z ortho (near = the
//    ceiling -> depth 1, far = the floor -> 0), Vulkan Y flip, as buildVulkanWorldFrame's
//    perspective; the cull planes are the box itself;
//  - LOD and texel demand: LLVKWorldFrame::ortho (clean_cull.comp CULL_ORTHO);
//  - sun shadows: one map fitted around the box's column in light space (cascade 0; the
//    cascade splits sit past every depth so nothing else is sampled, and cascades 1-3 cull
//    everything), realtime, casters limited to below the ceiling;
//  - no haze / distance fade / 2D clouds / god rays (atmos lane 6 z = 0: atmosEnabled() is
//    false), no local lights, mirror, DoF, HUD, selection outline, RLV spheres; the water as
//    seen from above it; one world clock for every tile.
bool vkRenderRegionOrthoView(const GMDXOrthoView& o, std::vector<U8>& rgb, std::vector<F32>* depth)
{
    if (o.width <= 0 || o.height <= 0 || !(o.right > o.left) || !(o.top > o.bottom) || !(o.ceiling > o.floor))
        return false;
    LLVKWorldFrame wf;
    buildVulkanWorldFrame(wf);
    if (!wf.enabled) return false;

    const LLVector3& eye = o.eye;
    const F32 zn = llmax(eye.mV[VZ] - o.ceiling, 0.01f);   // the ceiling, below the eye
    const F32 zf = eye.mV[VZ] - o.floor;
    // View (world -> view, column-major): right = +X, up = +Y, forward = -Z, so v = p - eye.
    const F32 V[16] = { 1.f, 0.f, 0.f, 0.f,
                        0.f, 1.f, 0.f, 0.f,
                        0.f, 0.f, 1.f, 0.f,
                        -eye.mV[VX], -eye.mV[VY], -eye.mV[VZ], 1.f };
    // Reversed-Z orthographic projection: view z = -zn -> 1, -zf -> 0; then the Vulkan Y flip.
    F32 P[16] = {};
    P[0]  = 2.f / (o.right - o.left);  P[12] = -(o.right + o.left) / (o.right - o.left);
    P[5]  = 2.f / (o.top - o.bottom);  P[13] = -(o.top + o.bottom) / (o.top - o.bottom);
    P[10] = 1.f / (zf - zn);           P[14] = zf / (zf - zn);
    P[15] = 1.f;
    P[1] = -P[1]; P[5] = -P[5]; P[9] = -P[9]; P[13] = -P[13];
    auto mul = [](const F32* A, const F32* B, F32* out)   // out = A * B (column-major)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                F32 s = 0.f;
                for (int k = 0; k < 4; ++k) s += A[k * 4 + r] * B[c * 4 + k];
                out[c * 4 + r] = s;
            }
    };
    std::memcpy(wf.view, V, sizeof(wf.view));
    std::memcpy(wf.proj, P, sizeof(wf.proj));
    vkInvertMat4(P, wf.inv_proj);
    mul(P, V, wf.view_proj);
    vkInvertMat4(wf.view_proj, wf.inv_view_proj);
    std::memcpy(wf.view_proj_rel, P, sizeof(wf.view_proj_rel));   // (the view has no rotation)
    vkInvertMat4(P, wf.inv_view_proj_rel);
    const F32 planes[6][4] = { {  1.f, 0.f, 0.f, -(eye.mV[VX] + o.left) },  { -1.f, 0.f, 0.f, eye.mV[VX] + o.right },
                               { 0.f,  1.f, 0.f, -(eye.mV[VY] + o.bottom) }, { 0.f, -1.f, 0.f, eye.mV[VY] + o.top },
                               { 0.f, 0.f, -1.f, o.ceiling },                { 0.f, 0.f,  1.f, -o.floor } };
    std::memcpy(wf.frustum_planes, planes, sizeof(wf.frustum_planes));
    wf.cam_pos[0] = wf.lod_cam_pos[0] = eye.mV[VX];
    wf.cam_pos[1] = wf.lod_cam_pos[1] = eye.mV[VY];
    wf.cam_pos[2] = wf.lod_cam_pos[2] = eye.mV[VZ];
    wf.lod_cam_pos[3] = 0.f;
    wf.cam_fwd[0] = 0.f; wf.cam_fwd[1] = 0.f; wf.cam_fwd[2] = -1.f;
    wf.tan_half[0] = 0.5f * (o.right - o.left) / zn;   // (light clusters only: there are no lights)
    wf.tan_half[1] = 0.5f * (o.top - o.bottom) / zn;
    wf.cluster_near = zn;
    wf.cluster_far = zf;
    wf.time = o.time;
    wf.temporal_reset = true;
    wf.ortho = true;
    wf.ortho_ppm = o.demand_ppm;

    // What a perspective / the live view brings that a map must not show.
    wf.dof_enabled = false;
    wf.mirror_valid = false;
    wf.hud_enabled = false;
    wf.hide_self = wf.hide_self_head = false;
    wf.lights.clear();
    wf.light_takeover.clear();
    wf.sel_ids.clear(); wf.sel_root.clear(); wf.hide_ids.clear();
    wf.rlv_sphere_count = 0;
    wf.atmos[6 * 4 + 2] = 0.f;   // atmosEnabled() false: no haze, distance fade, 2D clouds, god rays
    // One exposure for every tile: not the live view's metered one (it adapts between tiles) --
    // RenderExposure alone (clean_tonemap.frag: no dynamic scale), as with auto exposure off.
    wf.atmos[15 * 4 + 2] = 0.f;
    wf.clouds[15] = 0.f;
    // The water as seen from above it, whatever the live camera is in.
    if (LLSettingsWater::ptr_t water = LLEnvironment::instance().getCurrentWater())
    {
        wf.water_fog[3] = llmax(water->getModifiedWaterFogDensity(false), 0.f);
        wf.water[11] = water->getScaleAbove();
        wf.water[19] = water->getModifiedWaterFogDensity(false);
        wf.water[23] = 0.f;
    }

    // Sun shadows: one map over the box's column in light space (buildVulkanWorldFrame's
    // cascade fit, with the column for the view slice).
    if (!o.shadows) wf.shadow_enabled = false;
    if (wf.shadow_enabled)
    {
        LLVector3 L(wf.atmos[28], wf.atmos[29], wf.atmos[30]);
        if (L.normalize() < 0.5f) { wf.shadow_enabled = false; }
        else
        {
            const LLVector3 fw = -L;
            LLVector3 rt = (fabsf(fw.mV[VZ]) < 0.99f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f)) % fw;
            rt.normalize();
            const LLVector3 uu = fw % rt;
            auto dotd = [](const LLVector3& a, const LLVector3& b)
            { return (F64)a.mV[0] * b.mV[0] + (F64)a.mV[1] * b.mV[1] + (F64)a.mV[2] * b.mV[2]; };
            F64 mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
            for (int c = 0; c < 8; ++c)
            {
                const LLVector3 p(eye.mV[VX] + ((c & 1) ? o.right : o.left), eye.mV[VY] + ((c & 2) ? o.top : o.bottom),
                                  (c & 4) ? o.ceiling : o.floor);
                const F64 q[3] = { dotd(p, rt), dotd(p, uu), dotd(p, fw) };
                for (int k = 0; k < 3; ++k) { mn[k] = llmin(mn[k], q[k]); mx[k] = llmax(mx[k], q[k]); }
            }
            const U32 res_alloc = LLVulkanContext::instance().shadowMapResolution();
            const U32 res = res_alloc ? res_alloc : llclamp(gSavedSettings.getU32("RenderVKShadowResolution"), 256u, 8192u);
            // Half extent: the column's footprint + a few texels for the filter taps at its edge.
            F32 R = (F32)(0.5 * llmax(mx[0] - mn[0], mx[1] - mn[1]));
            R += 8.f * 2.f * R / (F32)res;
            // Centre snapped to whole texels in absolute light space: every tile has the same R, so
            // neighbouring tiles' maps share one texel grid and filter the same at the seam.
            const F64 texd = 2.0 * (F64)R / (F64)res;
            const F64 cxd = std::floor(0.5 * (mn[0] + mx[0]) / texd) * texd;
            const F64 cyd = std::floor(0.5 * (mn[1] + mx[1]) / texd) * texd;
            // Casters toward the light from the column's top (below the ceiling: the caster plane).
            const F32 back = llmin((o.ceiling - o.floor) / llmax(L.mV[VZ], 0.05f), 8192.f) + 16.f;
            const F64 znear = mn[2] - (F64)back, zfar = mx[2] + 1.0;
            const F32 iz = (F32)(1.0 / (zfar - znear));
            F32 Mw[16];
            Mw[0] = rt.mV[0] / R; Mw[4] = rt.mV[1] / R; Mw[8]  = rt.mV[2] / R; Mw[12] = (F32)(-cxd / R);
            Mw[1] = uu.mV[0] / R; Mw[5] = uu.mV[1] / R; Mw[9]  = uu.mV[2] / R; Mw[13] = (F32)(-cyd / R);
            Mw[2] = fw.mV[0] * iz; Mw[6] = fw.mV[1] * iz; Mw[10] = fw.mV[2] * iz; Mw[14] = (F32)(-znear * iz);
            Mw[3] = 0.f; Mw[7] = 0.f; Mw[11] = 0.f; Mw[15] = 1.f;
            // Camera-relative for the GPU (#555), translation in double.
            F32 M[16];
            std::memcpy(M, Mw, sizeof(M));
            M[12] = (F32)(-(cxd - dotd(eye, rt)) / (F64)R);
            M[13] = (F32)(-(cyd - dotd(eye, uu)) / (F64)R);
            M[14] = (F32)(-(znear - dotd(eye, fw)) * (F64)iz);
            // Cull planes (Gribb-Hartmann, VK depth 0..1): L R B T near far, from the world matrix.
            F32 pl0[6][4];
            {
                F32 rw[4][4];
                for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) rw[r][col] = Mw[col * 4 + r];
                const int sgn[6][2] = {{0,1},{0,-1},{1,1},{1,-1},{2,0},{2,-1}};
                for (int i = 0; i < 6; ++i)
                {
                    F32 pl[4];
                    for (int col = 0; col < 4; ++col)
                        pl[col] = (i == 4) ? rw[2][col] : rw[3][col] + (F32)sgn[i][1] * rw[sgn[i][0]][col];
                    const F32 len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                    for (int col = 0; col < 4; ++col) pl0[i][col] = len > 0.f ? pl[col] / len : pl[col];
                }
            }
            const LLVector3 ctr(eye.mV[VX] + 0.5f * (o.left + o.right), eye.mV[VY] + 0.5f * (o.bottom + o.top),
                                0.5f * (o.ceiling + o.floor));
            const F32 radius = 0.5f * sqrtf((o.right - o.left) * (o.right - o.left) + (o.top - o.bottom) * (o.top - o.bottom)
                                            + (o.ceiling - o.floor) * (o.ceiling - o.floor));
            for (int c = 0; c < 4; ++c)
            {
                std::memcpy(wf.shadow_mat[c], M, sizeof(M));
                for (int i = 0; i < 6; ++i)
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        // cascades 1-3: never sampled (the splits below), so they draw nothing
                        wf.shadow_planes[c][i][k] = c == 0 ? pl0[i][k] : (k == 3 ? -1.f : 0.f);
                        wf.shadow_caster_planes[c][i][k] = (k == 3) ? 1.f : 0.f;
                    }
                }
                // Casters above the ceiling (a skybox's roof, cut-off tree tops) cast nothing.
                wf.shadow_caster_planes[c][0][2] = -1.f;
                wf.shadow_caster_planes[c][0][3] = o.ceiling;
                wf.shadow_clip[c] = 1.0e9f;   // every view depth is in cascade 0, no blend to the next
                wf.shadow_texel[c] = 2.f * R / (F32)res;
                wf.shadow_slice[c][0] = ctr.mV[0]; wf.shadow_slice[c][1] = ctr.mV[1];
                wf.shadow_slice[c][2] = ctr.mV[2]; wf.shadow_slice[c][3] = radius;
            }
            wf.shadow_update = 0;   // realtime: nothing cached between tiles
        }
    }

    return LLVulkanContext::instance().renderOffscreen(wf, (uint32_t)o.width, (uint32_t)o.height, rgb, depth,
                                                       llmax(o.frames, 1u), o.aa_frames);
}
// </GMDX>

bool LLAppViewer::initWindow()
{
    LL_INFOS("AppInit") << "Initializing window..." << LL_ENDL;

    // always start windowed
    bool ignorePixelDepth = gSavedSettings.getBOOL("IgnorePixelDepth");

    LLViewerWindow::Params window_params;
    window_params
        .title(gWindowTitle)
        .name(VIEWER_WINDOW_CLASSNAME)
        .x(gSavedSettings.getS32("WindowX"))
        .y(gSavedSettings.getS32("WindowY"))
        .width(gSavedSettings.getU32("WindowWidth"))
        .height(gSavedSettings.getU32("WindowHeight"))
        .min_width(gSavedSettings.getU32("MinWindowWidth"))
        .min_height(gSavedSettings.getU32("MinWindowHeight"))
#ifdef LL_DARWIN
        // Setting it to true causes black screen with no UI displayed.
        // Given that it's a DEBUG settings and application goes fullscreen
        // on mac simply by expanding it, it was decided to not support/use
        // this setting on mac.
        .fullscreen(false)
#else // LL_DARWIN
        .fullscreen(gSavedSettings.getBOOL("FullScreen"))
#endif
        .ignore_pixel_depth(ignorePixelDepth)
        .first_run(mIsFirstRun);

    gViewerWindow = new LLViewerWindow(window_params);

    LL_INFOS("AppInit") << "gViewerwindow created." << LL_ENDL;

    // Need to load feature table before cheking to start watchdog.
    bool use_watchdog = false;
    int watchdog_enabled_setting = gSavedSettings.getS32("WatchdogEnabled");
    if (watchdog_enabled_setting == -1)
    {
        use_watchdog = !LLFeatureManager::getInstance()->isFeatureAvailable("WatchdogDisabled");
    }
    else
    {
        // The user has explicitly set this setting; always use that value.
        use_watchdog = bool(watchdog_enabled_setting);
    }

    LL_INFOS("AppInit") << "watchdog"
                        << (use_watchdog ? " " : " NOT ")
                        << "enabled"
                        << " (setting = " << watchdog_enabled_setting << ")"
                        << LL_ENDL;

    if (use_watchdog)
    {
        LLWatchdog::getInstance()->init();
    }

    LLNotificationsUI::LLNotificationManager::getInstance();

    if (gSavedSettings.getBOOL("WindowMaximized"))
    {
        gViewerWindow->getWindow()->maximize();
    }

    // Bring up the VK context + facade backend, then continue the real viewer boot:
    // the 2D UI init (LLViewerWindow::initBase) runs and the startup state machine
    // brings up the login screen, which renders through the VK present path.
    if (!initVulkanForRealUI())
    {
        // There is no usable render backend: idling would just present a dead (still
        // hidden) window. #526: tell the user why and exit cleanly instead of LL_ERRS.
        LL_WARNS("AppInit") << "Vulkan bring-up failed. See the log above for the device/instance error."
                            << LL_ENDL;
        std::string text = LLVulkanContext::instance().initFailureMessage();
        if (text.empty())
        {
            // Failed before / after the device bring-up proper: describe the devices anyway.
            VkRequirementsReport r = vkCheckRequirements();
            r.failure = "Vulkan bring-up on the viewer window failed";
            text = vkRequirementsUserMessage(r);
        }
        exitGraphicsUnsupported(text);
    }

    // The other per-session flags LLPipeline::init() loads (#42): without them
    // Develop > Render Attached Lights / Particles and RenderDynamicLOD reset to
    // their compiled defaults every login under VK.
    LLPipeline::sDynamicLOD = gSavedSettings.getBOOL("RenderDynamicLOD");
    LLPipeline::sRenderAttachedLights = gSavedSettings.getBOOL("RenderAttachedLights");
    LLPipeline::sRenderAttachedParticles = gSavedSettings.getBOOL("RenderAttachedParticles");

    // Seed the render-debug FEATURE mask exactly as LLPipeline::init() would.
    // We skip gPipeline.init() below (VK uses LLRenderVK's own pipelines), but
    // that init is ALSO where setAllRenderDebugFeatures() runs — and the
    // RENDER_DEBUG_FEATURE_UI bit gates UI *interactivity*, not just rendering.
    // With the mask left at its constructed 0, LLViewerWindow silently drops
    // every UI mouse click (handleAnyMouseClick early-outs on the UI-feature
    // check) and clears keyboard focus every frame (updateKeyboardFocus), so
    // the login screen draws but accepts no input. This is the same default GL
    // takes; it touches only the feature mask, never mRenderDebugMask (RAYCAST
    // stays off, so no cursorIntersect against the uninit'd GL pipeline).
    if (gSavedSettings.getBOOL("DisableAllRenderFeatures"))
    {
        gPipeline.clearAllRenderDebugFeatures();
    }
    else
    {
        gPipeline.setAllRenderDebugFeatures();
    }

    // Skip the GL 3D pipeline init (shaders, render pass pools, GL defaults)
    // — the VK UI uses LLRenderVK's own pipelines. Still finish the 2D UI
    // init so the real LLUI + login screen exist.
    LLUI::getInstance()->mWindow = gViewerWindow->getWindow();
    gViewerWindow->setCursor(UI_CURSOR_WAIT);
    gViewerWindow->initBase();   // real UI tree (login panel holder, fonts)
    // initGLDefaults() is the other half of the skipped GL init, but it makes no GL
    // calls: it fills gBox (the unit cube the stretch/scale manip and the align
    // tool draw their handles with, via gGL). Skipping it left every handle a
    // zero-size box -- no scale handles in edit mode (#153).
    gViewerWindow->initGLDefaults();

    LL_INFOS("AppInit") << "Vulkan: real 2D UI init done." << LL_ENDL;
    return true;
}

bool LLAppViewer::isUpdaterMissing()
{
    return mUpdaterNotFound;
}

bool LLAppViewer::waitForUpdater()
{
    return !gSavedSettings.getBOOL("CmdLineSkipUpdater") && !mUpdaterNotFound && !gNonInteractive;
}

void LLAppViewer::writeDebugInfo(bool isStatic)
{
    //Try to do the minimum when writing data during a crash.
    std::string* debug_filename;
    debug_filename = ( isStatic
        ? getStaticDebugFile()
        : getDynamicDebugFile() );

    LL_INFOS() << "Writing debug file " << *debug_filename << LL_ENDL;
    llofstream out_file(debug_filename->c_str());

    isStatic ?  LLSDSerialize::toPrettyXML(gDebugInfo, out_file)
             :  LLSDSerialize::toPrettyXML(gDebugInfo["Dynamic"], out_file);
}

LLSD LLAppViewer::getViewerInfo() const
{
    // The point of having one method build an LLSD info block and the other
    // construct the user-visible About string is to ensure that the same info
    // is available to a getInfo() caller as to the user opening
    // LLFloaterAbout.
    LLSD info;
    auto& versionInfo(LLVersionInfo::instance());
    // With GitHub builds, the build number is too big to fit in a 32-bit int,
    // and LLSD doesn't deal with integers wider than int. Use string.
    info["VIEWER_VERSION"] = llsd::array(versionInfo.getMajor(), versionInfo.getMinor(),
                                         versionInfo.getPatch(), stringize(versionInfo.getBuild()));
    info["VIEWER_VERSION_STR"] = versionInfo.getVersion();
    info["CHANNEL"] = versionInfo.getChannel();
    info["ADDRESS_SIZE"] = ADDRESS_SIZE;
#if LL_ARM64
    info["ARCHITECTURE"] = "NEON";
#elif defined(__AVX2__)
    info["ARCHITECTURE"] = "AVX2";
#else
    info["ARCHITECTURE"] = "SSE4_2";
#endif
    std::string build_config = versionInfo.getBuildConfig();
    if (build_config != "Release")
    {
        info["BUILD_CONFIG"] = build_config;
    }

    // return a URL to the release notes for this viewer, such as:
    // https://releasenotes.secondlife.com/viewer/2.1.0.123456.html
    std::string url = versionInfo.getReleaseNotes(); // VVM supplied
    if (url.empty())
    {
        url = LLTrans::getString("RELEASE_NOTES_BASE_URL");
        if (!LLStringUtil::endsWith(url, "/"))
            url += "/";
        url += LLURI::escape(versionInfo.getVersion()) + ".html";
    }
    info["VIEWER_RELEASE_NOTES_URL"] = url;

#if LL_MSVC && !defined(LL_CLANG)
    info["COMPILER"] = "MSVC";
    info["COMPILER_VERSION"] = _MSC_FULL_VER;
#elif LL_CLANG
    info["COMPILER"] = "Clang";
    info["COMPILER_VERSION"] = __clang_version__;
#elif LL_GNUC
    info["COMPILER"] = "GCC";
    info["COMPILER_VERSION"] = GCC_VERSION;
#endif

    // Position
    LLViewerRegion* region = gAgent.getRegion();
    if (region)
    {
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
        if (RlvActions::canShowLocation())
        {
// [/RLVa:KB]
            LLVector3d pos = gAgent.getPositionGlobal();
            info["POSITION"] = ll_sd_from_vector3d(pos);
            info["POSITION_LOCAL"] = ll_sd_from_vector3(gAgent.getPosAgentFromGlobal(pos));
            info["REGION"] = region->getName();

            boost::regex regex("\\.(secondlife|lindenlab)\\..*");
            info["HOSTNAME"] = boost::regex_replace(region->getSimHostName(), regex, "");
            info["SERVER_VERSION"] = gLastVersionChannel;
            LLSLURL slurl;
            LLAgentUI::buildSLURL(slurl);
            info["SLURL"] = slurl.getSLURLString();
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
        }
        else
        {
            info["REGION"] = RlvStrings::getString(RlvStringKeys::Hidden::Region);
        }
        info["SERVER_VERSION"] = gLastVersionChannel;
// [/RLVa:KB]
    }

    // CPU
    info["CPU"] = gSysCPU.getCPUString();
    info["MEMORY_MB"] = LLSD::Integer(gSysMemory.getPhysicalMemoryKB().valueInUnits<LLUnits::Megabytes>());
    // Moved hack adjustment to Windows memory size into llsys.cpp
    info["CONCURRENCY"] = LLSD::Integer((S32) std::thread::hardware_concurrency());
    info["OS_VERSION"] = LLOSInfo::instance().getOSString();
    // Vulkan device identity (gGPUInfo, filled by initVulkanForRealUI).
    info["GRAPHICS_CARD_VENDOR"] = gGPUInfo.vendorOrPlaceholder();
    info["GRAPHICS_CARD"]        = gGPUInfo.rendererOrPlaceholder();

#if LL_WINDOWS
    std::string drvinfo;

    if (gGPUInfo.mIsIntel)
    {
        drvinfo = gDXHardware.getDriverVersionWMI(LLDXHardware::GPU_INTEL);
    }
    else if (gGPUInfo.mIsNVIDIA)
    {
        drvinfo = gDXHardware.getDriverVersionWMI(LLDXHardware::GPU_NVIDIA);
    }
    else if (gGPUInfo.mIsAMD)
    {
        drvinfo = gDXHardware.getDriverVersionWMI(LLDXHardware::GPU_AMD);
    }

    if (drvinfo.empty())
    {
        // Generic/substitute windows driver? Unknown vendor?
        LL_WARNS("DriverVersion") << "Vendor based driver search failed, searching for any driver" << LL_ENDL;
        drvinfo = gDXHardware.getDriverVersionWMI(LLDXHardware::GPU_ANY);
    }

    if (!drvinfo.empty())
    {
        info["GRAPHICS_DRIVER_VERSION"] = drvinfo;
    }
    else
    {
        LL_WARNS("DriverVersion")<< "Cannot get driver version from getDriverVersionWMI" << LL_ENDL;
        LLSD driver_info = gDXHardware.getDisplayInfo();
        if (driver_info.has("DriverVersion"))
        {
            info["GRAPHICS_DRIVER_VERSION"] = driver_info["DriverVersion"];
        }
    }
#endif

// [RLVa:KB] - Checked: 2010-04-18 (RLVa-1.2.0)
    info["RLV_VERSION"] = (rlv_handler_t::isEnabled()) ? RlvStrings::getVersionAbout() : "(disabled)";
// [/RLVa:KB]
    info["OPENGL_VERSION"] = gGPUInfo.apiVersionOrPlaceholder();
    {
        // "Vulkan 1.4.x" -> "1.4.x"; the AboutVulkan label already names the API.
        std::string vk_version = info["OPENGL_VERSION"].asString();
        if (vk_version.rfind("Vulkan ", 0) == 0)
        {
            vk_version.erase(0, 7);
        }
        info["VULKAN_VERSION"] = vk_version;
        info["RAY_TRACING"] = LLVulkanContext::instance().rayTracingStatus();
        info["ASYNC_COMPUTE"] = LLVulkanContext::instance().asyncComputeSummary();
    }

    // Settings

    LLRect window_rect = gViewerWindow->getWindowRectRaw();
    info["WINDOW_WIDTH"] = window_rect.getWidth();
    info["WINDOW_HEIGHT"] = window_rect.getHeight();
    info["FONT_SIZE_ADJUSTMENT"] = gSavedSettings.getF32("FontScreenDPI");
    info["UI_SCALE"] = gSavedSettings.getF32("UIScaleFactor");
    info["DRAW_DISTANCE"] = gSavedSettings.getF32("RenderFarClip");
    info["NET_BANDWITH"] = gSavedSettings.getF32("ThrottleBandwidthKBPS");
    info["LOD_FACTOR"] = gSavedSettings.getF32("RenderVolumeLODFactor");
    info["RENDER_QUALITY"] = (F32)gSavedSettings.getU32("RenderQualityPerformance");
    info["TEXTURE_MEMORY"] = LLSD::Integer(gGPUInfo.mVRAM);

#if LL_DARWIN
    info["HIDPI"] = gHiDPISupport;
#endif

    // Libraries
    info["LIBCURL_VERSION"] = LLCore::LLHttp::getCURLVersion();
    info["J2C_VERSION"] = LLImageJ2C::getEngineInfo();
    bool want_fullname = true;
    info["AUDIO_DRIVER_VERSION"] = gAudiop ? LLSD(gAudiop->getDriverName(want_fullname)) : "Undefined";
    if(LLVoiceClient::getInstance()->voiceEnabled())
    {
        LLVoiceVersionInfo version = LLVoiceClient::getInstance()->getVersion();
        const std::string build_version = version.mBuildVersion;
        std::ostringstream version_string;
        if (std::equal(version.mBuildVersion.begin(), version.mBuildVersion.begin() + version.serverVersion.size(),
                       version.serverVersion.begin()))
        {  // Normal case: Show type and build version.
            version_string << version.voiceServerType << " " << version.mBuildVersion << std::endl;
        }
        else
        {  // Mismatch: Show both versions.
            version_string << version.voiceServerType << " " << version.serverVersion << "/" << version.mBuildVersion << std::endl;
        }
        info["VOICE_VERSION"] = version_string.str();
    }
    else
    {
        info["VOICE_VERSION"] = LLTrans::getString("NotConnected");
    }

    std::ostringstream cef_ver_codec;
    cef_ver_codec << "Dullahan: ";
    cef_ver_codec << DULLAHAN_VERSION_MAJOR;
    cef_ver_codec << ".";
    cef_ver_codec << DULLAHAN_VERSION_MINOR;
    cef_ver_codec << ".";
    cef_ver_codec << DULLAHAN_VERSION_POINT;
    cef_ver_codec << ".";
    cef_ver_codec << DULLAHAN_VERSION_BUILD;

    cef_ver_codec << std::endl;
    cef_ver_codec << "  CEF: ";
    cef_ver_codec << CEF_VERSION;

    cef_ver_codec << std::endl;
    cef_ver_codec << "  Chromium: ";
    cef_ver_codec << CHROME_VERSION_MAJOR;
    cef_ver_codec << ".";
    cef_ver_codec << CHROME_VERSION_MINOR;
    cef_ver_codec << ".";
    cef_ver_codec << CHROME_VERSION_BUILD;
    cef_ver_codec << ".";
    cef_ver_codec << CHROME_VERSION_PATCH;

    info["LIBCEF_VERSION"] = cef_ver_codec.str();

    std::ostringstream vlc_ver_codec;
    vlc_ver_codec << LIBVLC_VERSION_MAJOR;
    vlc_ver_codec << ".";
    vlc_ver_codec << LIBVLC_VERSION_MINOR;
    vlc_ver_codec << ".";
    vlc_ver_codec << LIBVLC_VERSION_REVISION;
    info["LIBVLC_VERSION"] = vlc_ver_codec.str();

    S32 packets_in = (S32)LLViewerStats::instance().getRecording().getSum(LLStatViewer::PACKETS_IN);
    if (packets_in > 0)
    {
        info["PACKETS_LOST"] = LLViewerStats::instance().getRecording().getSum(LLStatViewer::PACKETS_LOST);
        info["PACKETS_IN"] = packets_in;
        info["PACKETS_PCT"] = 100.f*info["PACKETS_LOST"].asReal() / info["PACKETS_IN"].asReal();
    }
    // #529: what this session downloaded
    info["DATA_USAGE"] = SLDataUsage::aboutText();

    if (mServerReleaseNotesURL.empty())
    {
        if (gAgent.getRegion())
        {
            info["SERVER_RELEASE_NOTES_URL"] = LLTrans::getString("RetrievingData");
        }
        else
        {
            info["SERVER_RELEASE_NOTES_URL"] = LLTrans::getString("NotConnected");
        }
    }
    else if (LLStringUtil::startsWith(mServerReleaseNotesURL, "http")) // it's an URL
    {
        info["SERVER_RELEASE_NOTES_URL"] = "[" + LLWeb::escapeURL(mServerReleaseNotesURL) + " " + LLTrans::getString("ReleaseNotes") + "]";
    }
    else
    {
        info["SERVER_RELEASE_NOTES_URL"] = mServerReleaseNotesURL;
    }

    // populate field for new local disk cache with some details
    info["DISK_CACHE_INFO"] = LLDiskCache::getInstance()->getCacheInfo();

    return info;
}

std::string LLAppViewer::getViewerInfoString(bool default_string) const
{
    std::ostringstream support;

    LLSD info(getViewerInfo());

    // Render the LLSD from getInfo() as a format_map_t
    LLStringUtil::format_map_t args;

    // allow the "Release Notes" URL label to be localized
    args["ReleaseNotes"] = LLTrans::getString("ReleaseNotes", default_string);

    for (LLSD::map_const_iterator ii(info.beginMap()), iend(info.endMap());
        ii != iend; ++ii)
    {
        if (! ii->second.isArray())
        {
            // Scalar value
            if (ii->second.isUndefined())
            {
                args[ii->first] = LLTrans::getString("none_text", default_string);
            }
            else
            {
                // don't forget to render value asString()
                args[ii->first] = ii->second.asString();
            }
        }
        else
        {
            // array value: build KEY_0, KEY_1 etc. entries
            for (LLSD::Integer n(0), size(static_cast<LLSD::Integer>(ii->second.size())); n < size; ++n)
            {
                args[STRINGIZE(ii->first << '_' << n)] = ii->second[n].asString();
            }
        }
    }

    // Now build the various pieces
    support << LLTrans::getString("AboutHeader", args, default_string);
    if (info.has("BUILD_CONFIG"))
    {
        support << "\n" << LLTrans::getString("BuildConfig", args, default_string);
    }
    if (info.has("REGION"))
    {
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
        support << "\n\n" << LLTrans::getString( (RlvActions::canShowLocation()) ? "AboutPosition" : "AboutPositionRLVShowLoc", args, default_string);
// [/RLVa:KB]
//      support << "\n\n" << LLTrans::getString("AboutPosition", args);
    }
    support << "\n\n" << LLTrans::getString("AboutSystem", args, default_string);
    support << "\n";
    if (info.has("GRAPHICS_DRIVER_VERSION"))
    {
        support << "\n" << LLTrans::getString("AboutDriver", args, default_string);
    }
    support << "\n" << LLTrans::getString("AboutVulkan", args, default_string);
    if (info.has("RAY_TRACING"))
    {
        support << "\n" << LLTrans::getString("AboutRayTracing", args, default_string);
    }
    if (info.has("ASYNC_COMPUTE"))
    {
        support << "\n" << LLTrans::getString("AboutAsyncCompute", args, default_string);
    }
    support << "\n\n" << LLTrans::getString("AboutSettings", args, default_string);
#if LL_DARWIN
    support << "\n" << LLTrans::getString("AboutOSXHiDPI", args, default_string);
#endif
    support << "\n\n" << LLTrans::getString("AboutLibs", args, default_string);
    if (info.has("COMPILER"))
    {
        support << "\n" << LLTrans::getString("AboutCompiler", args, default_string);
    }
    if (info.has("PACKETS_IN"))
    {
        support << '\n' << LLTrans::getString("AboutTraffic", args, default_string);
    }
    if (info.has("DATA_USAGE"))
    {
        support << '\n' << LLTrans::getString("AboutDataUsage", args, default_string);
    }

    // SLT timestamp
    LLSD substitution;
    substitution["datetime"] = (S32)time(NULL);//(S32)time_corrected();
    support << "\n" << LLTrans::getString("AboutTime", substitution, default_string);

    return support.str();
}

void LLAppViewer::cleanupSavedSettings()
{
    gSavedSettings.setBOOL("MouseSun", false);

    gSavedSettings.setBOOL("UseEnergy", true);              // force toggle to turn off, since sends message to simulator

    gSavedSettings.setBOOL("DebugWindowProc", gDebugWindowProc);

    gSavedSettings.setBOOL("ShowObjectUpdates", gShowObjectUpdates);

    if (gDebugView)
    {
        gSavedSettings.setBOOL("ShowDebugConsole", gDebugView->mDebugConsolep->getVisible());
    }

    // save window position if not maximized
    // as we don't track it in callbacks
    if(NULL != gViewerWindow)
    {
        bool maximized = gViewerWindow->getWindow()->getMaximized();
        // #919: nor while fullscreen (that is the display's origin, not the windowed placement)
        if (!maximized && !gViewerWindow->getWindow()->getFullscreen())
        {
            LLCoordScreen window_pos;

            if (gViewerWindow->getWindow()->getPosition(&window_pos))
            {
                gSavedSettings.setS32("WindowX", window_pos.mX);
                gSavedSettings.setS32("WindowY", window_pos.mY);
            }
        }
    }

    gSavedSettings.setF32("MapScale", LLWorldMapView::getScaleSetting());

    // (RenderFarClip is NOT written back from gAgentCamera.mDrawDistance any more: that is
    // the EFFECTIVE distance, which the VRAM ladder's far-geometry rung may hold below the
    // user's setting (#405) -- saving it would persist a temporary reduction. Every writer
    // of mDrawDistance derives it from, or also sets, the setting.)
}

// True when the sound cache folder is the cache root itself (a configured location, or the
// last-resort fallback), where "*.dsf" in the root are live sounds, not old-layout leftovers.
static bool sound_cache_is_cache_root()
{
    auto strip = [](std::string dir)
    {
        while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
        {
            dir.pop_back();
        }
        return dir;
    };
    return strip(LLSoundCache::getDir()) == strip(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, ""));
}

void LLAppViewer::initSoundCache(bool purge, bool read_only)
{
    // Decoded sounds (#176): their own folder (SlipstreamSoundCacheLocation, empty = <cache>/sounds)
    // with an LRU byte budget. Like the cache location, a new folder applies on the next start.
    constexpr U64 SOUND_CACHE_MB = 1024 * 1024;
    const U32 budget_mb = llclamp(gSavedSettings.getU32("SlipstreamSoundCacheSizeMB"), LLSoundCache::MIN_BUDGET_MB, LLSoundCache::MAX_BUDGET_MB);
    // With "keep decoded sounds" off the folder is emptied at exit; also empty it here, in case
    // the last session crashed before it could.
    purge = purge || !gSavedSettings.getBOOL("SlipstreamKeepDecodedSounds");
    LLSoundCache::init(gSavedSettings.getString("SlipstreamSoundCacheLocation"), static_cast<U64>(budget_mb) * SOUND_CACHE_MB, purge, read_only);

    // Decoded sounds used to sit loose in the cache root; clear out those leftovers.
    if (!read_only && !sound_cache_is_cache_root())
    {
        removeCacheFiles("*.dsf");
    }
}

void LLAppViewer::removeCacheFiles(const std::string& file_mask)
{
    gDirUtilp->deleteFilesInDir(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, ""), file_mask);
}

void LLAppViewer::writeSystemInfo()
{

    if (! gDebugInfo.has("Dynamic") )
        gDebugInfo["Dynamic"] = LLSD::emptyMap();

#if LL_WINDOWS
    gDebugInfo["SLLog"] = gDirUtilp->getExpandedFilename(LL_PATH_DUMP, getInstanceLogBaseName() + ".log");
#else
    //Not ideal but sufficient for good reporting.
    gDebugInfo["SLLog"] = getOldLogFilePath();  //LLError::logFileName();
#endif

    gDebugInfo["ClientInfo"]["Name"] = LLVersionInfo::instance().getChannel();
    gDebugInfo["ClientInfo"]["MajorVersion"] = LLVersionInfo::instance().getMajor();
    gDebugInfo["ClientInfo"]["MinorVersion"] = LLVersionInfo::instance().getMinor();
    gDebugInfo["ClientInfo"]["PatchVersion"] = LLVersionInfo::instance().getPatch();
    gDebugInfo["ClientInfo"]["BuildVersion"] = std::to_string(LLVersionInfo::instance().getBuild());
    gDebugInfo["ClientInfo"]["AddressSize"] = LLVersionInfo::instance().getAddressSize();

    gDebugInfo["CAFilename"] = gDirUtilp->getCAFile();

    gDebugInfo["CPUInfo"]["CPUString"] = gSysCPU.getCPUString();
    gDebugInfo["CPUInfo"]["CPUFamily"] = gSysCPU.getFamily();
    gDebugInfo["CPUInfo"]["CPUMhz"] = (S32)gSysCPU.getMHz();
    gDebugInfo["CPUInfo"]["CPUAltivec"] = gSysCPU.hasAltivec();
    gDebugInfo["CPUInfo"]["CPUSSE"] = gSysCPU.hasSSE();
    gDebugInfo["CPUInfo"]["CPUSSE2"] = gSysCPU.hasSSE2();

    gDebugInfo["RAMInfo"]["Physical"] = LLSD::Integer(gSysMemory.getPhysicalMemoryKB().value());
    gDebugInfo["RAMInfo"]["Allocated"] = LLSD::Integer(gMemoryAllocated.valueInUnits<LLUnits::Kilobytes>());
    gDebugInfo["OSInfo"] = LLOSInfo::instance().getOSStringSimple();

    // The user is not logged on yet, but record the current grid choice login url
    // which may have been the intended grid.
    gDebugInfo["GridName"] = LLGridManager::getInstance()->getGridId();

    // *FIX:Mani - move this down in llappviewerwin32
#ifdef LL_WINDOWS
    DWORD thread_id = GetCurrentThreadId();
    gDebugInfo["MainloopThreadID"] = (S32)thread_id;
#endif

    // "CrashNotHandled" is set here, while things are running well,
    // in case of a freeze. If there is a freeze, the crash logger will be launched
    // and can read this value from the debug_info.log.
    gDebugInfo["CrashNotHandled"] = LLSD::Boolean(true);

    // Insert crash host url (url to post crash log to) if configured. This insures
    // that the crash report will go to the proper location in the case of a
    // prior freeze.
    std::string crashHostUrl = gSavedSettings.get<std::string>("CrashHostUrl");
    if(crashHostUrl != "")
    {
        gDebugInfo["CrashHostUrl"] = crashHostUrl;
    }

    // Dump some debugging info
    LL_INFOS("SystemInfo") << "Application: " << LLTrans::getString("APP_NAME") << LL_ENDL;
    LL_INFOS("SystemInfo") << "Version: " << LLVersionInfo::instance().getChannelAndVersion() << LL_ENDL;
#ifdef LL_BUILD_COMMIT
    LL_INFOS("SystemInfo") << "Build commit: " << LL_BUILD_COMMIT << LL_ENDL;
#endif

    // Dump the local time and time zone
    time_t now;
    time(&now);
    char tbuffer[256];      /* Flawfinder: ignore */
    strftime(tbuffer, 256, "%Y-%m-%dT%H:%M:%S %Z", localtime(&now));
    LL_INFOS("SystemInfo") << "Local time: " << tbuffer << LL_ENDL;

    // query some system information
    LL_INFOS("SystemInfo") << "CPU info:\n" << gSysCPU << LL_ENDL;
    LL_INFOS("SystemInfo") << "Memory info:\n" << gSysMemory << LL_ENDL;
    LL_INFOS("SystemInfo") << "OS: " << LLOSInfo::instance().getOSStringSimple() << LL_ENDL;
    LL_INFOS("SystemInfo") << "OS info: " << LLOSInfo::instance() << LL_ENDL;

    gDebugInfo["SettingsFilename"] = gSavedSettings.getString("ClientSettingsFile");
    gDebugInfo["ViewerExePath"] = gDirUtilp->getExecutablePathAndName();
    gDebugInfo["CurrentPath"] = gDirUtilp->getCurPath();
    gDebugInfo["FirstLogin"] = LLSD::Boolean(gAgent.isFirstLogin());
    gDebugInfo["FirstRunThisInstall"] = gSavedSettings.getBOOL("FirstRunThisInstall");
    gDebugInfo["StartupState"] = LLStartUp::getStartupStateString();

    if (gViewerWindow)
    {
    std::vector<std::string> resolutions = gViewerWindow->getWindow()->getDisplaysResolutionList();
    for (auto res_iter : resolutions)
    {
        gDebugInfo["DisplayInfo"].append(res_iter);
    }
    }

    writeDebugInfo(); // Save out debug_info.log early, in case of crash.
}

#ifdef LL_WINDOWS
//For whatever reason, in Windows when using OOP server for breakpad, the callback to get the
//name of the dump file is not getting triggered by the breakpad library.   Unfortunately they
//also didn't see fit to provide a simple query request across the pipe to get this name either.
//Since we are putting our output in a runtime generated directory and we know the header data in
//the dump format, we can however use the following hack to identify our file.
// TODO make this a member function.
void getFileList()
{
    std::stringstream filenames;

    typedef std::vector<std::string> vec;
    std::string pathname = gDirUtilp->getExpandedFilename(LL_PATH_DUMP,"");
    vec file_vec = gDirUtilp->getFilesInDir(pathname);
    for(vec::const_iterator iter=file_vec.begin(); iter!=file_vec.end(); ++iter)
    {
        filenames << *iter << " ";
        if ( ( iter->length() > 30 ) && (iter->rfind(".dmp") == (iter->length()-4) ) )
        {
            std::string fullname = pathname + *iter;
            llifstream fdat( fullname.c_str(), std::ifstream::binary);
            if (fdat)
            {
                char buf[5];
                fdat.read(buf,4);
                fdat.close();
                if (!strncmp(buf,"MDMP",4))
                {
                    gDebugInfo["Dynamic"]["MinidumpPath"] = fullname;
                    break;
                }
            }
        }
    }
    filenames << std::endl;
    gDebugInfo["Dynamic"]["DumpDirContents"] = filenames.str();
}
#endif

// static
void LLAppViewer::recordMarkerVersion(LLAPRFile& marker_file)
{
    std::string marker_version(LLVersionInfo::instance().getChannelAndVersion());
    if ( marker_version.length() > MAX_MARKER_LENGTH )
    {
        LL_WARNS_ONCE("MarkerFile") << "Version length ("<< marker_version.length()<< ")"
                                    << " greater than maximum (" << MAX_MARKER_LENGTH << ")"
                                    << ": marker matching may be incorrect"
                                    << LL_ENDL;
    }

    // record the viewer version in the marker file
    marker_file.write(marker_version.data(), static_cast<S32>(marker_version.length()));
}

bool LLAppViewer::markerIsSameVersion(const std::string& marker_name) const
{
    bool sameVersion = false;

    std::string my_version(LLVersionInfo::instance().getChannelAndVersion());
    char marker_data[MAX_MARKER_LENGTH];
    S32  marker_version_length;

    LLAPRFile marker_file;
    marker_file.open(marker_name, LL_APR_RB);
    if (marker_file.getFileHandle())
    {
        marker_version_length = marker_file.read(marker_data, sizeof(marker_data));
        std::string marker_string(marker_data, marker_version_length);
        size_t pos = marker_string.find('\n');
        if (pos != std::string::npos)
        {
            marker_string = marker_string.substr(0, pos);
        }
        if ( 0 == my_version.compare( 0, my_version.length(), marker_string, 0, marker_string.length()) )
        {
            sameVersion = true;
        }
        LL_DEBUGS("MarkerFile") << "Compare markers for '" << marker_name << "': "
                                << "\n   mine '" << my_version    << "'"
                                << "\n marker '" << marker_string << "'"
                                << "\n " << ( sameVersion ? "same" : "different" ) << " version"
                                << LL_ENDL;
        marker_file.close();
    }
    return sameVersion;
}

void LLAppViewer::recordSessionToMarker()
{
    std::string marker_version(LLVersionInfo::instance().getChannelAndVersion());
    std::string uuid_str = "\n" + gAgentSessionID.asString();
    if (marker_version.length() + uuid_str.length() > MAX_MARKER_LENGTH)
    {
        LL_WARNS_ONCE("MarkerFile") << "Version length (" << marker_version.length() << ")"
            << " greater than maximum (" << MAX_MARKER_LENGTH << ")"
            << ": marker matching may be incorrect"
            << LL_ENDL;
    }

    mMarkerFile.seek(APR_SET, (S32)marker_version.length());
    mMarkerFile.write(uuid_str.data(), (S32)uuid_str.length());
}

LLUUID LLAppViewer::getMarkerSessionId(const std::string& marker_name) const
{
    std::string data;
    if (getMarkerData(marker_name, data))
    {
        return LLUUID(data);
    }
    return LLUUID();
}

S32 LLAppViewer::getMarkerErrorCode(const std::string& marker_name) const
{
    std::string data;
    if (getMarkerData(marker_name, data))
    {
        if (data.empty())
        {
            return 0;
        }
        else
        {
            return std::stoi(data);
        }
    }
    return -1;
}

bool LLAppViewer::getMarkerData(const std::string& marker_name, std::string& data) const
{
    bool sameVersion = false;

    std::string my_version(LLVersionInfo::instance().getChannelAndVersion());
    char marker_data[MAX_MARKER_LENGTH];
    S32  marker_version_length;

    LLAPRFile marker_file;
    marker_file.open(marker_name, LL_APR_RB);
    if (marker_file.getFileHandle())
    {
        marker_version_length = marker_file.read(marker_data, sizeof(marker_data));
        marker_file.close();
        std::string marker_string(marker_data, marker_version_length);
        size_t pos = marker_string.find('\n');
        if (pos != std::string::npos)
        {
            data = marker_string.substr(pos + 1, marker_version_length - pos - 1);
            marker_string = marker_string.substr(0, pos);
        }
        if (0 == my_version.compare(0, my_version.length(), marker_string, 0, marker_string.length()))
        {
            sameVersion = true;
        }
        else
        {
            return false;
        }
        LL_DEBUGS("MarkerFile") << "Compare markers for '" << marker_name << "': "
            << "\n   mine '" << my_version << "'"
            << "\n marker '" << marker_string << "'"
            << "\n " << (sameVersion ? "same" : "different") << " version"
            << LL_ENDL;
        return true;
    }
    return false;
}

// #700: the previous session did not end cleanly (crash, freeze, bad_alloc, ...): its log was
// just rotated to Slipstream.old, which the next start replaces. Keep a timestamped copy
// (Slipstream-crash-YYYYMMDD-HHMMSS.log) and prune to the newest VKKeepCrashLogs (default 5).
// Never on a clean exit. The name carries no user data.
static void keepCrashLog(const std::string& old_log)
{
    const S32 keep = (S32)gSavedSettings.getU32("VKKeepCrashLogs");
    if (keep <= 0 || !gDirUtilp || !LLFile::isfile(old_log)) return;
    llstat st;
    time_t t = time(nullptr);
    if (0 == LLFile::stat(old_log, &st)) t = st.st_mtime;
    struct tm tmv;
#if LL_WINDOWS
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    const std::string name = std::string("Slipstream-crash-") + stamp + ".log";
    const std::string dest = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
    if (!LLFile::isfile(dest) && !LLFile::copy(old_log, dest))
    {
        LL_WARNS("MarkerFile") << "Could not keep the previous session's log as " << name << LL_ENDL;
        return;
    }
    LL_INFOS("MarkerFile") << "Previous session did not end cleanly (event " << (S32)gLastExecEvent
                           << (gLastExecDeviceLost ? ", graphics device lost" : "")
                           << "): its log is kept as " << name << LL_ENDL;
    std::vector<std::string> kept;   // names sort by time
    LLDirIterator iter(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ""), "Slipstream-crash-*.log");
    std::string found;
    while (iter.next(found)) kept.push_back(found);
    std::sort(kept.begin(), kept.end());
    for (S32 i = 0; i + keep < (S32)kept.size(); ++i)
    {
        LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, kept[i]), ENOENT);
    }
}

void LLAppViewer::processMarkerFiles()
{
    //We've got 4 things to test for here
    // - Other Process Running (SecondLife.exec_marker present, locked)
    // - Freeze (SecondLife.exec_marker present, not locked)
    // - LLError Crash (SecondLife.llerror_marker present)
    // - Other Crash (SecondLife.error_marker present)
    // These checks should also remove these files for the last 2 cases if they currently exist

    std::ostringstream marker_log_stream;
    bool marker_is_same_version = true;
    // first, look for the marker created at startup and deleted on a clean exit
    mMarkerFileName = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,MARKER_FILE_NAME);
    if (LLAPRFile::isExist(mMarkerFileName, NULL, LL_APR_RB))
    {
        // File exists...
        // first, read it to see if it was created by the same version (we need this later)
        marker_is_same_version = markerIsSameVersion(mMarkerFileName);
        if (marker_is_same_version)
        {
            gLastAgentSessionId = getMarkerSessionId(mMarkerFileName);
        }

        // now test to see if this file is locked by a running process (try to open for write)
        marker_log_stream << "Checking exec marker file for lock...";
        mMarkerFile.open(mMarkerFileName, LL_APR_WB);
        apr_file_t* fMarker = mMarkerFile.getFileHandle() ;
        if (!fMarker)
        {
            marker_log_stream << "Exec marker file open failed - assume it is locked.";
            mSecondInstance = true; // lock means that instance is running.
        }
        else
        {
            // We were able to open it, now try to lock it ourselves...
            if (apr_file_lock(fMarker, APR_FLOCK_NONBLOCK | APR_FLOCK_EXCLUSIVE) != APR_SUCCESS)
            {
                marker_log_stream << "Locking exec marker failed.";
                mSecondInstance = true; // lost a race? be conservative
            }
            else
            {
                // No other instances; we've locked this file now, so record our version; delete on quit.
                recordMarkerVersion(mMarkerFile);
                marker_log_stream << "Exec marker file existed but was not locked; rewritten.";
            }
        }
        if (mSecondInstance)
        {
            acquireInstanceSlot();
        }
        initLoggingAndGetLastDuration();

        std::string marker_log_msg(marker_log_stream.str());
        LL_INFOS("MarkerFile") << marker_log_msg << LL_ENDL;

        if (mSecondInstance)
        {
            LL_INFOS("MarkerFile") << "Exec marker '"<< mMarkerFileName << "' owned by another instance" << LL_ENDL;
        }
        else if (marker_is_same_version)
        {
            // the file existed, is ours, and matched our version, so we can report on what it says
            LL_INFOS("MarkerFile") << "Exec marker '"<< mMarkerFileName << "' found; last exec crashed" << LL_ENDL;
            gLastExecEvent = LAST_EXEC_OTHER_CRASH;
        }
        else
        {
            LL_INFOS("MarkerFile") << "Exec marker '"<< mMarkerFileName << "' found, but versions did not match" << LL_ENDL;
        }
    }
    else // marker did not exist... last exec (if any) did not freeze
    {
        initLoggingAndGetLastDuration();
        // Create the marker file for this execution & lock it; it will be deleted on a clean exit
        apr_status_t s;
        s = mMarkerFile.open(mMarkerFileName, LL_APR_WB, true);

        if (s == APR_SUCCESS && mMarkerFile.getFileHandle())
        {
            LL_DEBUGS("MarkerFile") << "Exec marker file '"<< mMarkerFileName << "' created." << LL_ENDL;
            if (APR_SUCCESS == apr_file_lock(mMarkerFile.getFileHandle(), APR_FLOCK_NONBLOCK | APR_FLOCK_EXCLUSIVE))
            {
                recordMarkerVersion(mMarkerFile);
                LL_DEBUGS("MarkerFile") << "Exec marker file locked." << LL_ENDL;
            }
            else
            {
                LL_WARNS("MarkerFile") << "Exec marker file cannot be locked." << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS("MarkerFile") << "Failed to create exec marker file '"<< mMarkerFileName << "'." << LL_ENDL;
        }
    }

    // now check for cases in which the exec marker may have been cleaned up by crash handlers

    // check for any last exec event report based on whether or not it happened during logout
    // (the logout marker is created when logout begins)
    std::string logout_marker_file =  gDirUtilp->getExpandedFilename(LL_PATH_LOGS, LOGOUT_MARKER_FILE_NAME);
    if(LLAPRFile::isExist(logout_marker_file, NULL, LL_APR_RB))
    {
        if (markerIsSameVersion(logout_marker_file))
        {
            gLastExecEvent = LAST_EXEC_LOGOUT_FROZE;
            LL_INFOS("MarkerFile") << "Logout crash marker '"<< logout_marker_file << "', changing LastExecEvent to LOGOUT_FROZE" << LL_ENDL;
        }
        else
        {
            LL_INFOS("MarkerFile") << "Logout crash marker '"<< logout_marker_file << "' found, but versions did not match" << LL_ENDL;
        }
        LLAPRFile::remove(logout_marker_file);
    }
    // and last refine based on whether or not a marker created during a non-llerr crash is found
    std::string error_marker_file = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ERROR_MARKER_FILE_NAME);
    if(LLAPRFile::isExist(error_marker_file, NULL, LL_APR_RB))
    {
        S32 marker_code = getMarkerErrorCode(error_marker_file);
        if (marker_code >= 0)
        {
            if (gLastExecEvent == LAST_EXEC_LOGOUT_FROZE)
            {
                gLastExecEvent = LAST_EXEC_LOGOUT_CRASH;
                LL_INFOS("MarkerFile") << "Error marker '"<< error_marker_file << "' crashed, setting LastExecEvent to LOGOUT_CRASH" << LL_ENDL;
            }
            else if (marker_code > 0 && marker_code < (S32)LAST_EXEC_COUNT)
            {
                gLastExecEvent = (eLastExecEvent)marker_code;
                LL_INFOS("MarkerFile") << "Error marker '"<< error_marker_file << "' crashed, setting LastExecEvent to " << gLastExecEvent << LL_ENDL;
            }
            else
            {
                gLastExecEvent = LAST_EXEC_OTHER_CRASH;
                LL_INFOS("MarkerFile") << "Error marker '" << error_marker_file << "' crashed, setting LastExecEvent to " << gLastExecEvent << LL_ENDL;
            }
        }
        else
        {
            LL_INFOS("MarkerFile") << "Error marker '"<< error_marker_file << "' marker found, but versions did not match" << LL_ENDL;
        }
        LLAPRFile::remove(error_marker_file);
    }
    // #790: a device loss closes the viewer cleanly (no crash marker), so its log was never
    // kept and the start after the next one deleted it. Its gpu-lost marker is still here
    // (gpuLostCheckPreviousSession consumes it later in startup).
    if (!mSecondInstance && gDirUtilp
        && gDirUtilp->fileExists(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, gpuLostFileName(false))))
    {
        gLastExecDeviceLost = true;
    }
    // #700: the final verdict on the last exit is known here.
    if (!mSecondInstance && (gLastExecEvent != LAST_EXEC_NORMAL || gLastExecDeviceLost))
    {
        keepCrashLog(getOldLogFilePath());
    }
}

std::string LLAppViewer::getInstanceLogBaseName() const
{
    return mInstanceSlot > 1 ? llformat("Slipstream-%d", mInstanceSlot) : std::string("Slipstream");
}

std::string LLAppViewer::getLogFilePath() const
{
    return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, getInstanceLogBaseName() + ".log");
}

std::string LLAppViewer::getOldLogFilePath() const
{
    return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, getInstanceLogBaseName() + ".old");
}

// A further viewer takes the first slot 2..MAX_INSTANCE_SLOTS whose lock file it can lock. The
// lock (not the file's existence) owns the slot, so a crashed viewer's slot frees with its process.
// The files are never removed: removing one could let two viewers lock different files of one name.
void LLAppViewer::acquireInstanceSlot()
{
    mInstanceSlot = 0;
    for (S32 slot = 2; slot <= MAX_INSTANCE_SLOTS; ++slot)
    {
        const std::string name = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, llformat("Slipstream-%d.instance_lock", slot));
        if (mInstanceSlotFile.open(name, LL_APR_WB) != APR_SUCCESS || !mInstanceSlotFile.getFileHandle())
        {
            mInstanceSlotFile.close();
            continue;
        }
        if (apr_file_lock(mInstanceSlotFile.getFileHandle(), APR_FLOCK_NONBLOCK | APR_FLOCK_EXCLUSIVE) == APR_SUCCESS)
        {
            mInstanceSlot = slot;
            return;
        }
        mInstanceSlotFile.close();
    }
}

void LLAppViewer::removeMarkerFiles()
{
    if (mInstanceSlotFile.getFileHandle())
    {
        mInstanceSlotFile.close();
    }
    if (!mSecondInstance)
    {
        SLRiskySettings::cleanExit();   // #736: this session did not crash
        if (mMarkerFile.getFileHandle())
        {
            mMarkerFile.close() ;
            LLAPRFile::remove( mMarkerFileName );
            LL_DEBUGS("MarkerFile") << "removed exec marker '"<<mMarkerFileName<<"'"<< LL_ENDL;
        }
        else
        {
            LL_WARNS("MarkerFile") << "marker '"<<mMarkerFileName<<"' not open"<< LL_ENDL;
        }

        if (mLogoutMarkerFile.getFileHandle())
        {
            mLogoutMarkerFile.close();
            LLAPRFile::remove( mLogoutMarkerFileName );
            LL_DEBUGS("MarkerFile") << "removed logout marker '"<<mLogoutMarkerFileName<<"'"<< LL_ENDL;
        }
        else
        {
            LL_WARNS("MarkerFile") << "logout marker '"<<mLogoutMarkerFileName<<"' not open"<< LL_ENDL;
        }
    }
    else
    {
        LL_WARNS("MarkerFile") << "leaving markers because this is a second instance" << LL_ENDL;
    }
}

void LLAppViewer::removeDumpDir()
{
    //Call this routine only on clean exit.  Crash reporter will clean up
    //its locking table for us.
    if (gDirUtilp->dumpDirExists()) // Check if dump dir was created this run
    {
        std::string dump_dir = gDirUtilp->getExpandedFilename(LL_PATH_DUMP, "");
        gDirUtilp->deleteDirAndContents(dump_dir);
    }
}

void LLAppViewer::forceQuit()
{
    LLApp::setQuitting();
}

void LLAppViewer::fastQuit(S32 error_code)
{
    // finish pending transfers
    flushLFSIO();
    // let sim know we're logging out
    sendLogoutRequest();
    // flush network buffers by shutting down messaging system
    end_messaging_system();
    // figure out the error code
    S32 final_error_code = error_code ? error_code : (S32)isError();
    // this isn't a crash
    removeMarkerFiles();
    // get outta here
    _exit(final_error_code);
}

void LLAppViewer::requestQuit()
{
    LL_INFOS() << "requestQuit" << LL_ENDL;

    LLViewerRegion* region = gAgent.getRegion();

    if ((LLStartUp::getStartupState() < STATE_STARTED) || !region)
    {
        // If we have a region, make some attempt to send a logout request first.
        // This prevents the halfway-logged-in avatar from hanging around inworld for a couple minutes.
        if (region)
        {
            sendLogoutRequest();
        }

        // Quit immediately
        forceQuit();
        return;
    }

    // Try to send metrics back to the grid
    metricsSend(!gDisconnected);

    // Try to send last batch of avatar rez metrics.
    if (!gDisconnected && isAgentAvatarValid())
    {
        gAgentAvatarp->updateAvatarRezMetrics(true); // force a last packet to be sent.
    }

    LLHUDEffectSpiral *effectp = (LLHUDEffectSpiral*)LLHUDManager::getInstance()->createViewerEffect(LLHUDObject::LL_HUD_EFFECT_POINT, true);
    effectp->setPositionGlobal(gAgent.getPositionGlobal());
    effectp->setColor(LLColor4U(gAgent.getEffectColor()));
    LLHUDManager::getInstance()->sendEffects();
    effectp->markDead() ;//remove it.

    // Attempt to close all floaters that might be
    // editing things.
    if (gFloaterView)
    {
        // application is quitting
        gFloaterView->closeAllChildren(true);
        mClosingFloaters = true;
    }

    // Send preferences once, when exiting
    bool include_preferences = true;
    send_viewer_stats(include_preferences);

    gLogoutTimer.reset();
    mQuitRequested = true;
}

static bool finish_quit(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);

    if (option == 0)
    {
        LLAppViewer::instance()->requestQuit();
    }
    return false;
}
static LLNotificationFunctorRegistration finish_quit_reg("ConfirmQuit", finish_quit);
static LLNotificationFunctorRegistration finish_quit_copies_reg("SLConfirmQuitInventoryCopies", finish_quit);

void LLAppViewer::userQuit()
{
    LL_INFOS() << "User requested quit" << LL_ENDL;
    if (gDisconnected
        || !gViewerWindow
        || !gViewerWindow->getProgressView()
        || gViewerWindow->getProgressView()->getVisible())
    {
        requestQuit();
    }
    else if (size_t copies = LLInventoryCopyPacer::pendingCount())
    {
        // Not ignorable, unlike ConfirmQuit: queued item copies (#538) would never be sent.
        LLSD args;
        args["COUNT"] = (S32)copies;
        LLNotificationsUtil::add("SLConfirmQuitInventoryCopies", args);
    }
    else
    {
        LLNotificationsUtil::add("ConfirmQuit");
    }
}

static bool finish_early_exit(const LLSD& notification, const LLSD& response)
{
    LLAppViewer::instance()->forceQuit();
    return false;
}

void LLAppViewer::earlyExit(const std::string& name, const LLSD& substitutions)
{
    LL_WARNS() << "app_early_exit: " << name << LL_ENDL;
    gDoDisconnect = true;
    LLNotificationsUtil::add(name, substitutions, LLSD(), finish_early_exit);
}

// case where we need the viewer to exit without any need for notifications
void LLAppViewer::earlyExitNoNotify()
{
    LL_WARNS() << "app_early_exit with no notification: " << LL_ENDL;
    gDoDisconnect = true;
    finish_early_exit( LLSD(), LLSD() );
}

void LLAppViewer::abortQuit()
{
    LL_INFOS() << "abortQuit()" << LL_ENDL;
    mQuitRequested = false;
    mClosingFloaters = false;
}

void LLAppViewer::migrateCacheDirectory()
{
#if LL_WINDOWS || LL_DARWIN
    // NOTE: (Nyx) as of 1.21, cache for mac is moving to /library/caches/SecondLife from
    // /library/application support/SecondLife/cache This should clear/delete the old dir.

    // As of 1.23 the Windows cache moved from
    //   C:\Documents and Settings\James\Application Support\SecondLife\cache
    // to
    //   C:\Documents and Settings\James\Local Settings\Application Support\SecondLife
    //
    // The Windows Vista equivalent is from
    //   C:\Users\James\AppData\Roaming\SecondLife\cache
    // to
    //   C:\Users\James\AppData\Local\SecondLife
    //
    // Note the absence of \cache on the second path.  James.

    // Only do this once per fresh install of this version.
    if (gSavedSettings.getBOOL("MigrateCacheDirectory"))
    {
        gSavedSettings.setBOOL("MigrateCacheDirectory", false);

        std::string old_cache_dir = gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "cache");
        std::string new_cache_dir = gDirUtilp->getCacheDir(true);

        if (gDirUtilp->fileExists(old_cache_dir))
        {
            LL_INFOS() << "Migrating cache from " << old_cache_dir << " to " << new_cache_dir << LL_ENDL;

            // Migrate inventory cache to avoid pain to inventory database after mass update
            S32 file_count = 0;
            std::string file_name;
            std::string mask = "*.*";

            LLDirIterator iter(old_cache_dir, mask);
            while (iter.next(file_name))
            {
                if (file_name == "." || file_name == "..") continue;
                std::string source_path = gDirUtilp->add(old_cache_dir, file_name);
                std::string dest_path = gDirUtilp->add(new_cache_dir, file_name);
                if (!LLFile::rename(source_path, dest_path))
                {
                    file_count++;
                }
            }
            LL_INFOS() << "Moved " << file_count << " files" << LL_ENDL;

            // Nuke the old cache
            gDirUtilp->setCacheDir(old_cache_dir);
            purgeCache();
            gDirUtilp->setCacheDir(new_cache_dir);

#if LL_DARWIN
            // Clean up Mac files not deleted by removing *.*
            std::string ds_store = old_cache_dir + "/.DS_Store";
            if (gDirUtilp->fileExists(ds_store))
            {
                LLFile::remove(ds_store);
            }
#endif
            if (LLFile::rmdir(old_cache_dir) != 0)
            {
                LL_WARNS() << "could not delete old cache directory " << old_cache_dir << LL_ENDL;
            }
        }
    }
#endif // LL_WINDOWS || LL_DARWIN
}

//static
U32 LLAppViewer::getTextureCacheVersion()
{
    // Viewer texture cache version, change if the texture cache format changes.
    // 2021-03-10 Bumping up by one to help obviate texture cache issues with
    //            Simple Cache Viewer - see SL-14985 for more information
    //const U32 TEXTURE_CACHE_VERSION = 8;
    const U32 TEXTURE_CACHE_VERSION = 9;

    return TEXTURE_CACHE_VERSION ;
}

//static
U32 LLAppViewer::getDiskCacheVersion()
{
    // Viewer disk cache version intorduced in Simple Cache Viewer, change if the cache format changes.
    const U32 DISK_CACHE_VERSION = 3;

    return DISK_CACHE_VERSION ;
}

//static
U32 LLAppViewer::getObjectCacheVersion()
{
    // Viewer object cache version, change if object update
    // format changes. JC
    const U32 INDRA_OBJECT_CACHE_VERSION = 18; // bumped: ObjectUpdate over-read (upstream 6988483ffc) could persist garbage to disk

    return INDRA_OBJECT_CACHE_VERSION;
}

bool LLAppViewer::initCache()
{
    mPurgeCache = false;
    bool read_only = mSecondInstance;
    LLAppViewer::getTextureCache()->setReadOnly(read_only) ;
    LLVOCache::initParamSingleton(read_only);

    // initialize the new disk cache using saved settings
    const std::string cache_dir_name = gSavedSettings.getString("DiskCacheDirName");

    const U32 MB = 1024 * 1024;
    const uintmax_t MIN_CACHE_SIZE = 896 * MB;
    // Up to 256 GB for people with the disk to spare (the texture cache stores full-quality
    // J2C so sharpening re-decodes from disk, never the network). The texture cache's
    // header index caps at ~1M entries (LLTextureCache::sCacheMaxEntries; fast-cache offsets
    // are S32), which at typical ~250 KB bodies is where a bigger cache stops helping.
    const uintmax_t MAX_CACHE_SIZE = 262144ll * MB;
    const uintmax_t setting_cache_total_size = uintmax_t(gSavedSettings.getU32("CacheSize")) * MB;
    const uintmax_t cache_total_size = llclamp(setting_cache_total_size, MIN_CACHE_SIZE, MAX_CACHE_SIZE);
    const F64 disk_cache_percent = gSavedSettings.getF32("DiskCachePercentOfTotal");
    const F64 texture_cache_percent = 100.0 - disk_cache_percent;

    // note that the maximum size of this cache is defined as a percentage of the
    // total cache size - the 'CacheSize' pref - for all caches.
    const uintmax_t disk_cache_size = uintmax_t(cache_total_size * disk_cache_percent / 100);
    const bool enable_cache_debug_info = gSavedSettings.getBOOL("EnableDiskCacheDebugInfo");

    bool texture_cache_mismatch = false;
    bool remove_vfs_files = false;
    if (gSavedSettings.getS32("LocalCacheVersion") != LLAppViewer::getTextureCacheVersion())
    {
        texture_cache_mismatch = true;
        if (!read_only)
        {
            gSavedSettings.setS32("LocalCacheVersion", LLAppViewer::getTextureCacheVersion());

            //texture cache version was bumped up in Simple Cache Viewer, and at this point old vfs files are not needed
            remove_vfs_files = true;
        }
    }

    if (!read_only)
    {
        // Purge cache if user requested it
        if (gSavedSettings.getBOOL("PurgeCacheOnStartup") ||
            gSavedSettings.getBOOL("PurgeCacheOnNextStartup"))
        {
            LL_INFOS("AppCache") << "Startup cache purge requested: " << (gSavedSettings.getBOOL("PurgeCacheOnStartup") ? "ALWAYS" : "ONCE") << LL_ENDL;
            gSavedSettings.setBOOL("PurgeCacheOnNextStartup", false);
            mPurgeCache = true;
            // STORM-1141 force purgeAllTextures to get called to prevent a crash here. -brad
            texture_cache_mismatch = true;
        }

        // We have moved the location of the cache directory over time.
        migrateCacheDirectory();

        // Setup and verify the cache location
        std::string cache_location = gSavedSettings.getString("CacheLocation");
        std::string new_cache_location = gSavedSettings.getString("NewCacheLocation");
        if (new_cache_location != cache_location)
        {
            LL_INFOS("AppCache") << "Cache location changed, cache needs purging" << LL_ENDL;
            gDirUtilp->setCacheDir(gSavedSettings.getString("CacheLocation"));
            purgeCache(); // purge old cache
            gDirUtilp->deleteDirAndContents(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, cache_dir_name));
            // ...and the old default sound cache folder (#176), which would otherwise be orphaned.
            gDirUtilp->deleteDirAndContents(LLSoundCache::getDefaultDir());
            // ...and the GPU-ready texture cache folder (#274).
            gDirUtilp->deleteDirAndContents(LLGpuTexCache::getDefaultDir());
            // ...and the cache store (#715; not open yet).
            gDirUtilp->deleteDirAndContents(SLCacheStore::defaultDir());
            // ...and the cluster LOD cache folder (1a, #744).
            gDirUtilp->deleteDirAndContents(LLCleanClusterCache::defaultDir());
            gSavedSettings.setString("CacheLocation", new_cache_location);
            gSavedSettings.setString("CacheLocationTopFolder", gDirUtilp->getBaseFileName(new_cache_location));
        }
    }

    if (!gDirUtilp->setCacheDir(gSavedSettings.getString("CacheLocation")))
    {
        LL_WARNS("AppCache") << "Unable to set cache location" << LL_ENDL;
        gSavedSettings.setString("CacheLocation", "");
        gSavedSettings.setString("CacheLocationTopFolder", "");
    }

    const std::string cache_dir = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, cache_dir_name);
    LLDiskCache::initParamSingleton(cache_dir, disk_cache_size, enable_cache_debug_info);

    // #715: the cache store, <cache>/store, opened when any consumer uses it:
    //   CacheStoreGpuTex  GPU-ready textures      (else .gtx files in <cache>/gputex)
    //   CacheStoreJ2C     J2C texture codestreams (else <cache>/texturecache)
    //   CacheStoreAssets  assets + mesh parts     (else sl_cache_*.asset files in <cache>/cache)
    //   CacheStoreCluster cluster LOD records     (else .clod files in <cache>/clusterlod)
    // A second instance opens it read-only with a private overflow (assets are delivered
    // through the cache, so its asset writes go there). If it cannot be opened every consumer
    // falls back to its old files. Clear Cache empties it.
    const U32 gputex_budget_mb = llclamp(gSavedSettings.getU32("SlipstreamGpuTexCacheSizeMB"),
                                         LLGpuTexCache::MIN_BUDGET_MB, LLGpuTexCache::MAX_BUDGET_MB);
    const U64 gputex_budget = static_cast<U64>(gputex_budget_mb) * 1024 * 1024;
    const U64 cluster_budget = static_cast<U64>(gSavedSettings.getU32("RenderVKClusterCacheMB")) << 20;
    const bool store_gputex = gSavedSettings.getBOOL("CacheStoreGpuTex");
    const bool store_j2c = gSavedSettings.getBOOL("CacheStoreJ2C");
    const bool store_assets = gSavedSettings.getBOOL("CacheStoreAssets");
    const bool store_cluster = gSavedSettings.getBOOL("CacheStoreCluster");
    LLCache::Store* cache_store = nullptr;
    if (store_gputex || store_j2c || store_assets || store_cluster)
    {
        SLCacheStore::Options so;
        so.read_only = read_only;
        so.purge = mPurgeCache;
        so.snapshot_interval_s = gSavedSettings.getU32("CacheStoreSnapshotIntervalSec");
        so.read_threads = gSavedSettings.getU32("CacheStoreReadThreads");
        so.decode_threads = gSavedSettings.getU32("CacheStoreDecodeThreads");
        so.textures_budget = gputex_budget;   // SLTextureTiers sets the real TEXTURES budget below
        so.assets_budget = disk_cache_size;     // CacheSize x DiskCachePercentOfTotal, as the old folder
        so.cluster_budget = cluster_budget;
        if (SLCacheStore::instance().open(so))
        {
            cache_store = SLCacheStore::instance().store();
        }
        else
        {
            LL_WARNS("AppCache") << "Cache store unavailable: GPU-ready textures, assets and cluster LOD fall back to "
                                 << "their old cache folders" << LL_ENDL;
        }
    }
    else if (mPurgeCache && !read_only)
    {
        // Clear Cache with the store unused: its records go too.
        gDirUtilp->deleteDirAndContents(SLCacheStore::defaultDir());
    }
    LLCache::Store* asset_store = store_assets ? cache_store : nullptr;

    if (!read_only)
    {
        if (gSavedSettings.getS32("DiskCacheVersion") != LLAppViewer::getDiskCacheVersion())
        {
            LLDiskCache::getInstance()->clearCache();
            if (asset_store && !asset_store->readOnly())
            {
                // The asset cache format changed: its store records go too (per kind).
                asset_store->removeAll(LLCache::Kind::ASSET);
                asset_store->removeAll(LLCache::Kind::MESH);
            }
            remove_vfs_files = true;
            gSavedSettings.setS32("DiskCacheVersion", LLAppViewer::getDiskCacheVersion());
        }

        if (remove_vfs_files)
        {
            LLDiskCache::getInstance()->removeOldVFSFiles();
        }

        if (mPurgeCache)
        {
        LLSplashScreen::update(LLTrans::getString("StartupClearingCache"));
        purgeCache();

            // clear the new C++ file system based cache
            LLDiskCache::getInstance()->clearCache();
    }
        else if (!asset_store)
        {
            // purge excessive files from the new file system based cache (its directory walk
            // is the old asset cache's startup scan: LLCacheMetrics startup)
            const auto purge_t0 = std::chrono::steady_clock::now();
            LLDiskCache::getInstance()->purge();
            LLCacheMetrics::recordStartup(LLCacheMetrics::Cache::ASSET, LLCacheMetrics::Backend::LEGACY,
                (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - purge_t0).count());
        }
    }
    // The 15-minute purge deletes files from the shared asset folder: only the instance that
    // owns the cache may run it (a second instance is read-only for every cache, #715).
    // shutdown() is safe on the never-started thread. With the store the old folder only
    // holds files the migration has not converted yet: no purge.
    if (!read_only && !asset_store)
    {
        LLAppViewer::getPurgeDiskCacheThread()->start();
    }
    if (asset_store)
    {
        // #715: LLFileSystem on the store; the old folder is converted in the background.
        SLAssetCacheStore::startAssets(asset_store, cache_dir, disk_cache_size, read_only);
    }
    if (cache_store && (asset_store || store_cluster))
    {
        SLAssetCacheStore::hookRegionChanges(cache_store);
    }

    initSoundCache(mPurgeCache, read_only);

    // The J2C texture cache's share of CacheSize (the rest of DiskCachePercentOfTotal).
    const S64 texture_cache_size = S64(cache_total_size * texture_cache_percent / 100);
    LLCache::Store* j2c_store = nullptr;   // #715: J2C textures in the cache store (CacheStoreJ2C)

    // GPU-ready texture cache (#274, tier 2 of #405). Two backends (#715):
    // - CacheStoreGpuTex on (default): records in the cache store, <cache>/store, which
    //   opens here (the first consumer of the store; J2C / assets / cluster LOD follow). A
    //   second instance opens it read-only. Clear Cache empties it. If it cannot be opened
    //   the old files serve, as below.
    // - otherwise: one .gtx file per texture in <cache>/gputex, indexed on a worker.
    // Their own folders, so the cache-root purge above doesn't reach them: Clear Cache
    // purges here.
    {
        const U64 budget = gputex_budget;
        LLCache::Store* store = nullptr;   // the store was opened above (#715)
        if (cache_store && (store_gputex || store_j2c))
        {
            // #705: J2C + GPU-ready share the TEXTURES group (budget = J2C texture share +
            // GPU-ready budget, GPU-ready capped at its budget; eviction: duplicate J2C,
            // GPU-ready, then J2C).
            SLTextureTiers::instance().start(cache_store, store_j2c, store_gputex, (U64)texture_cache_size, budget);
            if (store_gputex) store = cache_store;
            if (store_j2c) j2c_store = cache_store;
        }
        LLGpuTexCache::init(gSavedSettings.getBOOL("SlipstreamGpuTexCache"), budget, mPurgeCache, read_only, store);
        if (store && !read_only && !store->readOnly())
        {
            const std::string gtx_dir = LLGpuTexCache::getDefaultDir();
            if (mPurgeCache)
            {
                // Clear Cache: unconverted .gtx files go with the store's records.
                gDirUtilp->deleteDirAndContents(gtx_dir);
            }
            else if (SLGpuTexMigrate::hasWork(gtx_dir))
            {
                // #714: convert the old files in the background (throttled, newest first,
                // each deleted once its record is on disk; the folder goes when done).
                SLGpuTexMigrate::Options mo;
                mo.live_cap_bytes = budget;
                sGpuTexMigration.start(gtx_dir, store, mo, 10.0);
            }
        }
    }
    // 1a (#744): cluster LOD disk cache, <cache>/clusterlod. Records the configuration only:
    // the folder, its worker and the index scan start on first use (RenderVKClusterLOD on).
    // #715 (CacheStoreCluster): records in the cache store instead; the old folder is
    // converted in the background (Clear Cache: the store was emptied, the folder goes).
    LLCache::Store* cluster_store = store_cluster ? cache_store : nullptr;
    LLCleanClusterCache::instance().init(LLCleanClusterCache::defaultDir(), cluster_budget, mPurgeCache, read_only,
                                         cluster_store);
    if (cluster_store && !read_only)
    {
        if (mPurgeCache)
        {
            gDirUtilp->deleteDirAndContents(LLCleanClusterCache::defaultDir());
        }
        else
        {
            SLAssetCacheStore::startClusterMigration(cluster_store, LLCleanClusterCache::defaultDir(), cluster_budget,
                                                     read_only);
        }
    }

    LLSplashScreen::update(LLTrans::getString("StartupInitializingTextureCache"));

    // Init the texture cache
    // Allocate the remaining percent which is not allocated to the disk cache.
    // #715: CacheStoreJ2C on and the store open -> J2C records in the store (the legacy
    // files are migrated in the background); otherwise the legacy files, as before (also
    // the automatic fallback when the store cannot be opened).
    LLAppViewer::getTextureCache()->initCache(LL_PATH_CACHE, texture_cache_size, texture_cache_mismatch, j2c_store);
    if (j2c_store && !read_only && !j2c_store->readOnly())
    {
        const std::string legacy_dir = LLTextureCache::legacyDir(LL_PATH_CACHE);
        if (SLJ2CMigrate::hasWork(legacy_dir))
        {
            // #714: convert texture.entries / texture.cache / bodies in the background
            // (throttled, newest first, each entry dropped once its record is on disk; the
            // folder goes when done). Unconverted textures are served by read-through.
            SLJ2CMigrate::Options mo;
            mo.live_cap_bytes = (U64)texture_cache_size;
#if defined(ADDRESS_SIZE)
            mo.expected_address_size = ADDRESS_SIZE;
#endif
            sJ2CMigration.start(legacy_dir, j2c_store, mo, 10.0, [](const std::shared_ptr<SLJ2CMigrate::Legacy>& legacy)
            {
                LLAppViewer::getTextureCache()->setLegacySource(legacy);
            });
        }
    }
    SLCacheMetrics::init();   // #715: store sampling + periodic CacheMetrics log (idle callback)

    const U32 CACHE_NUMBER_OF_REGIONS_FOR_OBJECTS = 128;
    LLVOCache::getInstance()->initCache(LL_PATH_CACHE, CACHE_NUMBER_OF_REGIONS_FOR_OBJECTS, getObjectCacheVersion());

    // Remove old, stale CEF cache folders
    purgeCefStaleCaches();

    return true;
}

void LLAppViewer::addOnIdleCallback(const boost::function<void()>& cb)
{
    gMainloopWork.post(cb);
}

void LLAppViewer::loadKeyBindings()
{
    std::string key_bindings_file = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings.xml");
    bool user_loaded = gDirUtilp->fileExists(key_bindings_file) && gViewerInput.loadBindingsXML(key_bindings_file);
    if (user_loaded
        && !gViewerInput.hasBinding(MODE_THIRD_PERSON, "push_forward")
        && !gViewerInput.hasBinding(MODE_THIRD_PERSON, "push_backward"))
    {
        // #232: a key_bindings.xml that leaves walking unbound (an older "Import from another
        // viewer" replaced the file wholesale). Keep it aside and start from the defaults
        // rather than leave the avatar unable to move.
        const std::string kept = key_bindings_file + ".unusable";
        LLFile::remove(kept, ENOENT);
        LLFile::rename(key_bindings_file, kept);
        LL_WARNS("InitInfo") << "Key bindings in " << key_bindings_file << " leave forward/back unbound in third person;"
                             << " moved to " << kept << " and loading the defaults" << LL_ENDL;
        doOnIdleOneTime([kept]()
        {
            LLSD args;
            args["FILE"] = kept;
            LLNotificationsUtil::add("KeyBindingsRestoredAtStartup", args);
        });
        user_loaded = false;
    }
    if (!user_loaded)
    {
        // Failed to load custom bindings, try default ones
        key_bindings_file = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "key_bindings.xml");
        if (!gViewerInput.loadBindingsXML(key_bindings_file))
        {
            LLError::LLUserWarningMsg::showMissingFiles();
            LL_ERRS("InitInfo") << "Unable to open default key bindings from " << key_bindings_file << LL_ENDL;
        }
    }
    LLUrlRegistry::instance().setKeybindingHandler(&gViewerInput);
    LLUrlRegistry::instance().setGameControllerHandler(LLGameControl::getInstance());
}

// As per GHI #4498, remove old, stale CEF cache folders from previous sessions
void LLAppViewer::purgeCefStaleCaches()
{
    // TODO: we really shouldn't use a hard coded name for the cache folder here...
    const std::string browser_parent_cache = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "cef_cache");
    if (LLFile::isdir(browser_parent_cache))
    {
        // This is a sledgehammer approach - nukes the cef_cache dir entirely
        // which is then recreated the first time a CEF instance creates an
        // individual cache folder. If we ever decide to retain some folders
        // e.g. Search UI cache - then we will need a more granular approach.
        gDirUtilp->deleteDirAndContents(browser_parent_cache);
    }
}

void LLAppViewer::purgeCache()
{
    LL_INFOS("AppCache") << "Purging Cache and Texture Cache..." << LL_ENDL;
    LLAppViewer::getTextureCache()->purgeCache(LL_PATH_CACHE);
    LLVOCache::getInstance()->removeCache(LL_PATH_CACHE);
    // The Vulkan pipeline cache (RenderVKPipelineCacheDisk) lives in the cache root too;
    // named here so a narrower mask below cannot silently keep it. Purges run at startup
    // (initCache) before the Vulkan device loads it.
    LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_pipeline_cache.bin"), ENOENT);
    // ...and the per-texture compression facts (RenderVKTexFactsCache, #263), read by the
    // renderer on its first texture pass (long after this startup purge).
    LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_texcomp_decisions.bin"), ENOENT);
    gDirUtilp->deleteFilesInDir(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, ""), "*");
}

//purge cache immediately, do not wait until the next login.
void LLAppViewer::purgeCacheImmediate()
{
    LL_INFOS("AppCache") << "Purging Object Cache and Texture Cache immediately..." << LL_ENDL;
    LLAppViewer::getTextureCache()->purgeCache(LL_PATH_CACHE, false);
    LLVOCache::getInstance()->removeCache(LL_PATH_CACHE, true);
}

std::string LLAppViewer::getSecondLifeTitle() const
{
    return LLTrans::getString("APP_NAME");
}

std::string LLAppViewer::getWindowTitle() const
{
    return gWindowTitle;
}

// Callback from a dialog indicating user was logged out.
bool finish_disconnect(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);

    if (1 == option)
    {
        LLAppViewer::instance()->forceQuit();
    }
    return false;
}

// Callback from an early disconnect dialog, force an exit
bool finish_forced_disconnect(const LLSD& notification, const LLSD& response)
{
    LLAppViewer::instance()->forceQuit();
    return false;
}


void LLAppViewer::forceDisconnect(const std::string& mesg)
{
    if (gDoDisconnect)
    {
        // Already popped up one of these dialogs, don't
        // do this again.
        return;
    }

    // #700: evidence before the state is torn down (frame times, crossings, memory). The
    // message is a translated string (no names).
    LLMemDiag::logPreDisconnect(mesg.c_str());

    // *TODO: Translate the message if possible
    std::string big_reason = LLAgent::sTeleportErrorMessages[mesg];
    if ( big_reason.size() == 0 )
    {
        big_reason = mesg;
    }

    LLSD args;
    gDoDisconnect = true;

    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        // Tell users what happened
        args["ERROR_MESSAGE"] = big_reason;
        LLNotificationsUtil::add("ErrorMessage", args, LLSD(), &finish_forced_disconnect);
    }
    else
    {
        sendSimpleLogoutRequest();
        args["MESSAGE"] = big_reason;
        LLNotificationsUtil::add("YouHaveBeenLoggedOut", args, LLSD(), &finish_disconnect );
    }
}

void LLAppViewer::badNetworkHandler()
{
    // Dump the packet
    gMessageSystem->dumpPacketToLog();

    // Flush all of our caches on exit in the case of disconnect due to
    // invalid packets.

    mPurgeCacheOnExit = true;

    std::ostringstream message;
    message <<
        "The viewer has detected mangled network data indicative\n"
        "of a bad upstream network connection or an incomplete\n"
        "local installation of " << LLAppViewer::instance()->getSecondLifeTitle() << ". \n"
        " \n"
        "Try uninstalling and reinstalling to see if this resolves \n"
        "the issue. \n"
        " \n"
        "If the problem continues, see the Tech Support FAQ at: \n"
        "www.secondlife.com/support";
    forceDisconnect(message.str());

    LLApp::instance()->writeMiniDump();
}

// This routine may get called more than once during the shutdown process.
// This can happen because we need to get the screenshot before the window
// is destroyed.
void LLAppViewer::saveFinalSnapshot()
{
    if (!mSavedFinalSnapshot)
    {
        gSavedSettings.setVector3d("FocusPosOnLogout", gAgentCamera.calcFocusPositionTargetGlobal());
        gSavedSettings.setVector3d("CameraPosOnLogout", gAgentCamera.calcCameraPositionTargetGlobal());
        // Save the renderer's newest world-frame capture (post tonemap, no UI), and
        // mark the snapshot done so the logout state machine (gated on
        // hasSavedFinalSnapshot()) proceeds and the LogoutRequest reaches the wire.
        std::vector<U8> rgb; U32 w = 0, h = 0;
        if (LLVulkanContext::instance().readWorldSnapshot(rgb, w, h))
        {
            std::string snap_filename = gDirUtilp->getLindenUserDir();
            snap_filename += gDirUtilp->getDirDelimiter();
            snap_filename += LLStartUp::getScreenLastFilename();
            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)w, (U16)h, 3);
            if (raw->getData() && raw->getDataSize() >= (S32)rgb.size())
            {
                std::memcpy(raw->getData(), rgb.data(), rgb.size());
                LLPointer<LLImageFormatted> png = LLImageFormatted::createFromType(IMG_CODEC_PNG);
                if (png && png->encode(raw, 0.0f) && png->save(snap_filename))
                {
                    LL_INFOS("Vulkan") << "Saved logout snapshot " << w << "x" << h << " -> " << snap_filename << LL_ENDL;
                    LLVector3d home;
                    if (gAgent.isInHomeRegion() && gAgent.getHomePosGlobal(&home)
                        && dist_vec(home, gAgent.getPositionGlobal()) < 10)
                    {
                        std::string snap_home = gDirUtilp->getLindenUserDir();
                        snap_home += gDirUtilp->getDirDelimiter();
                        snap_home += LLStartUp::getScreenHomeFilename();
                        if (!gDirUtilp->fileExists(snap_home)) LLFile::copy(snap_filename, snap_home);
                    }
                }
            }
        }
        mSavedFinalSnapshot = true;
    }
}

static const char PRODUCTION_CACHE_FORMAT_STRING[] = "%s.%s";
static const char GRID_CACHE_FORMAT_STRING[] = "%s.%s.%s";
std::string get_name_cache_filename(const std::string &base_file, const std::string& extention)
{
    std::string filename;
    std::string path(gDirUtilp->getExpandedFilename(LL_PATH_CACHE, base_file));
    if (LLGridManager::getInstance()->isInProductionGrid())
    {
        filename = llformat(PRODUCTION_CACHE_FORMAT_STRING, path.c_str(), extention.c_str());
    }
    else
    {
        // NOTE: The inventory cache filenames now include the grid name.
        // Add controls against directory traversal or problematic pathname lengths
        // if your viewer uses grid names from an untrusted source.
        const std::string& grid_id_str   = LLGridManager::getInstance()->getGridId();
        const std::string& grid_id_lower = utf8str_tolower(grid_id_str);
        filename                         = llformat(GRID_CACHE_FORMAT_STRING, path.c_str(), grid_id_lower.c_str(), extention.c_str());
    }
    return filename;
}

void LLAppViewer::loadNameCache()
{
    // display names cache
    std::string filename = get_name_cache_filename("avatar_name_cache", "xml");
    LL_INFOS("AvNameCache") << filename << LL_ENDL;
    llifstream name_cache_stream(filename.c_str());
    if(name_cache_stream.is_open())
    {
        if ( ! LLAvatarNameCache::getInstance()->importFile(name_cache_stream))
        {
            LL_WARNS("AppInit") << "removing invalid '" << filename << "'" << LL_ENDL;
            name_cache_stream.close();
            LLFile::remove(filename);
        }
    }

    if (!gCacheName) return;

    // is there a reason for the "cache" extention?
    std::string name_cache = get_name_cache_filename("name", "cache");
    llifstream cache_file(name_cache.c_str());
    if(cache_file.is_open())
    {
        if(gCacheName->importFile(cache_file)) return;
    }
}

void LLAppViewer::saveNameCache()
{
    // display names cache
    std::string filename = get_name_cache_filename("avatar_name_cache", "xml");
    llofstream name_cache_stream(filename.c_str());
    if(name_cache_stream.is_open())
    {
        LLAvatarNameCache::getInstance()->exportFile(name_cache_stream);
    }

    // real names cache
    if (gCacheName)
    {
        std::string name_cache = get_name_cache_filename("name", "cache");
        llofstream cache_file(name_cache.c_str());
        if(cache_file.is_open())
        {
            gCacheName->exportFile(cache_file);
        }
    }
}


/*! @brief      This class is an LLFrameTimer that can be created with
                an elapsed time that starts counting up from the given value
                rather than 0.0.

                Otherwise it behaves the same way as LLFrameTimer.
*/
class LLFrameStatsTimer : public LLFrameTimer
{
public:
    LLFrameStatsTimer(F64 elapsed_already = 0.0)
        : LLFrameTimer()
        {
            mStartTime -= elapsed_already;
        }
};

static LLTrace::BlockTimerStatHandle FTM_AUDIO_UPDATE("Update Audio");
static LLTrace::BlockTimerStatHandle FTM_CLEANUP("Cleanup");
static LLTrace::BlockTimerStatHandle FTM_CLEANUP_DRAWABLES("Drawables");
static LLTrace::BlockTimerStatHandle FTM_IDLE_CB("Idle Callbacks");
static LLTrace::BlockTimerStatHandle FTM_LOD_UPDATE("Update LOD");
static LLTrace::BlockTimerStatHandle FTM_OBJECTLIST_UPDATE("Update Objectlist");
static LLTrace::BlockTimerStatHandle FTM_REGION_UPDATE("Update Region");
static LLTrace::BlockTimerStatHandle FTM_WORLD_UPDATE("Update World");
static LLTrace::BlockTimerStatHandle FTM_NETWORK("Network");
static LLTrace::BlockTimerStatHandle FTM_AGENT_NETWORK("Agent Network");
static LLTrace::BlockTimerStatHandle FTM_VLMANAGER("VL Manager");
static LLTrace::BlockTimerStatHandle FTM_AGENT_POSITION("Agent Position");
static LLTrace::BlockTimerStatHandle FTM_HUD_EFFECTS("HUD Effects");

///////////////////////////////////////////////////////
// idle()
//
// Called every time the window is not doing anything.
// Receive packets, update statistics, and schedule a redisplay.
///////////////////////////////////////////////////////
// VRAM soft ladder, far geometry (#405 step 6): while the effective draw distance is held
// below RenderFarClip, cacheable objects wholly beyond it are killed back into their
// region's object cache (LLViewerRegion::killCachedObjectsBeyond: the ordinary object
// death releases their GPU instances; the geometry GC and texture eviction then free
// their memory). They come back from the cache as the distance is restored. Paced: twice
// a second, at most 256 objects per pass (1024 inside the VRAM reserve; a kill is a full
// object teardown -- the log reports the slowest pass).
static void vramUnloadFarObjects()
{
    static LLFrameTimer s_timer;
    if (s_timer.getElapsedTimeF32() < 0.5f) return;
    s_timer.reset();
    if (LLStartUp::getStartupState() < STATE_STARTED) return;
    static LLCachedControl<F32> s_far(gSavedSettings, "RenderFarClip", 128.f);
    const F32 dist = gAgentCamera.mDrawDistance;
    static U32 s_total = 0;
    static F32 s_max_ms = 0.f;
    static LLFrameTimer s_log;
    U32 killed = 0;
    if (dist < (F32)s_far - 1.f)   // else nothing held back
    {
        // Inside the VRAM reserve (ladder fast mode) unload 4x faster: with the kill /
        // reload churn gone each kill is a real release, and the backlog is the memory.
        const bool fast = LLVulkanContext::instance().vramSummary().mode >= 2u;
        // The culling camera (#479): a frozen debug camera keeps what it selected.
        const LLVector3 cam = LLCullCamera::getOrigin();
        LLTimer pass_timer;
        U32 budget = fast ? 1024 : 256;
        for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            if (!regionp || budget == 0) continue;
            const U32 n = regionp->killCachedObjectsBeyond(cam, dist, budget);
            killed += n;
            budget -= n;
        }
        const F32 pass_s = pass_timer.getElapsedTimeF32();
        if (killed) s_max_ms = llmax(s_max_ms, pass_s * 1000.f);
    }
    s_total += killed;
    // Re-ingests: killed roots re-created since (LLViewerRegion::addActiveCacheEntry); "fast"
    // ones (within 30 s of the kill) would be kill / reload churn.
    if ((s_total || LLViewerRegion::sVramReingests) && s_log.getElapsedTimeF32() > 5.f)
    {
        LL_INFOS("VKMem") << "VRAM ladder far geometry: " << s_total << " objects beyond " << (S32)dist
                          << " m returned to the object cache (draw distance set " << (S32)(F32)s_far << " m); re-ingested "
                          << LLViewerRegion::sVramReingests << " (" << LLViewerRegion::sVramReingestsFast
                          << " within 30 s of their kill); slowest pass " << s_max_ms << " ms" << LL_ENDL;
        s_total = 0;
        s_max_ms = 0.f;
        LLViewerRegion::sVramReingests = LLViewerRegion::sVramReingestsFast = 0;
        s_log.reset();
    }
}

// #484 / #391: asset-fetch timeline after login -- how fast textures and meshes stream in,
// for comparing fetch strategies (e.g. concurrency settings) from a cold cache. One
// line every 2 s from STATE_STARTED, a "settled" line once nothing has been in flight or
// queued for 6 s, and nothing after 5 minutes. AssetFetchTimeline = false disables it.
// (#299-#309/#377: also restarts when a teleport arrives, and logs a "pipeline" line of
// the asset-path counters, so login and teleport sharpen can be compared before/after.)
static void assetFetchTimelineTick()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "AssetFetchTimeline", true);
    static F64 s_start = -1.0, s_next = 0.0, s_quiet_since = -1.0;
    static bool s_done = false, s_settled = false;
    static S32 s_tp_state = 0;
    if (!s_on || LLStartUp::getStartupState() < STATE_STARTED) return;
    {
        // re-arm on teleport arrival (in progress -> none)
        const S32 tp_state = (S32)gAgent.getTeleportState();
        if (s_tp_state != LLAgent::TELEPORT_NONE && tp_state == LLAgent::TELEPORT_NONE && s_start >= 0.0)
        {
            s_start = -1.0;
            s_done = s_settled = false;
            s_quiet_since = -1.0;
            LL_INFOS("AssetFetch") << "timeline restart: teleport arrived" << LL_ENDL;
        }
        s_tp_state = tp_state;
    }
    if (s_done) return;
    const F64 now = LLTimer::getTotalSeconds();
    if (s_start < 0.0)
    {
        s_start = now;
        s_next = now;
        LL_INFOS("AssetFetch") << "timeline start: TextureFetchConcurrency=" << gSavedSettings.getU32("TextureFetchConcurrency")
                               << " Mesh2MaxConcurrentRequests=" << gSavedSettings.getU32("Mesh2MaxConcurrentRequests") << LL_ENDL;
    }
    if (now < s_next) return;
    s_next = now + 2.0;
    const F64 t = now - s_start;
    LLTextureFetch* tf = LLAppViewer::getTextureFetch();
    const S32 tex_queued = tf ? tf->getNumRequests() : 0;
    const S32 tex_http = tf ? tf->getNumHTTPRequests() : 0;
    const U32 tex_total = tf ? tf->getTotalNumHTTPRequests() : 0;
    const S32 mesh_active = LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests
                          + LLMeshRepoThread::sActiveSkinRequests;
    const U32 mesh_gets = LLMeshRepository::sHTTPRequestCount + LLMeshRepository::sHTTPLargeRequestCount;
    LL_INFOS("AssetFetch") << llformat("+%5.1fs tex: %u GETs, %d in flight, %d queued | mesh: %u GETs, %d active, %.1f MB recv, %u retries, %u errors",
                                       t, tex_total, tex_http, tex_queued, mesh_gets, mesh_active,
                                       LLMeshRepository::sBytesReceived / 1048576.0, LLMeshRepository::sHTTPRetryCount,
                                       LLMeshRepository::sHTTPErrorCount) << LL_ENDL;
    {
        // Keep-alive health (#485): new connections vs completed requests per asset class.
        LLAppCoreHttp& ach = LLAppViewer::instance()->getAppCoreHttp();
        U64 tr, tc, mr, mc, lr, lc;
        LLCore::HttpRequest::getConnectionStats(ach.getPolicy(LLAppCoreHttp::AP_TEXTURE), tr, tc);
        LLCore::HttpRequest::getConnectionStats(ach.getPolicy(LLAppCoreHttp::AP_MESH2), mr, mc);
        LLCore::HttpRequest::getConnectionStats(ach.getPolicy(LLAppCoreHttp::AP_LARGE_MESH), lr, lc);
        LL_INFOS("AssetFetch") << llformat("        connections: tex %llu new for %llu requests | mesh %llu new for %llu | large mesh %llu new for %llu",
                                           (unsigned long long)tc, (unsigned long long)tr, (unsigned long long)mc,
                                           (unsigned long long)mr, (unsigned long long)lc, (unsigned long long)lr) << LL_ENDL;
    }
    {
        // asset-path counters: fetch parks/wakes (#299/#306; missed-wakes should stay ~0;
        // rechecks = safety-net re-runs that found the event still pending, and scale with
        // the parked backlog, e.g. the decode queue at login),
        // decode queue (#307), texture-cache entry mirror (#300), mesh thread wakes (#308),
        // pending-load index (#304), off-thread LLSD parses (#377), load callbacks (#309)
        LLTextureCache* tc = LLAppViewer::getTextureCache();
        LLImageDecodeThread* dec = LLAppViewer::getImageDecodeThread();
        LL_INFOS("AssetFetch") << llformat("        pipeline: fetch parked %d wakes %llu missed-wakes %llu rechecks %llu | decode queued %d cancelled %llu"
                                           " | cache entries %.1f MB ram-hits %llu disk-reads %llu | mesh wakes %llu polls %llu delayed %d"
                                           " vol-index %d | async LLSD %llu (%.1f MB) | tex callbacks %d of %d",
                                           tf ? tf->getParkedCount() : 0,
                                           (unsigned long long)(tf ? tf->getWakeCount() : 0),
                                           (unsigned long long)(tf ? tf->getParkTimeoutCount() : 0),
                                           (unsigned long long)(tf ? tf->getParkRecheckCount() : 0),
                                           dec ? (S32)dec->getPending() : 0,
                                           (unsigned long long)(dec ? dec->getCancelledCount() : 0),
                                           tc ? tc->getEntriesRAMBytes() / 1048576.0 : 0.0,
                                           (unsigned long long)(tc ? tc->getEntryRAMHits() : 0),
                                           (unsigned long long)(tc ? tc->getEntryDiskReads() : 0),
                                           (unsigned long long)LLMeshRepoThread::sWakeups.load(),
                                           (unsigned long long)LLMeshRepoThread::sPollWakeups.load(),
                                           (gMeshRepo.mThread ? gMeshRepo.mThread->getDelayedCount() : 0),
                                           (S32)gMeshRepo.getVolumeLoadIndexSize(),
                                           (unsigned long long)LLCoreHttpUtil::getAsyncLLSDParseCount(),
                                           LLCoreHttpUtil::getAsyncLLSDParseBytes() / 1048576.0,
                                           gTextureList.mCallbackListVisited, (S32)gTextureList.mCallbackList.size()) << LL_ENDL;
    }
    const bool quiet = tex_http == 0 && tex_queued == 0 && mesh_active == 0;
    if (!quiet) s_quiet_since = -1.0;
    else if (s_quiet_since < 0.0) s_quiet_since = t;
    if (!s_settled && quiet && t >= 10.0 && t - s_quiet_since >= 6.0)
    {
        s_settled = true;
        LL_INFOS("AssetFetch") << llformat("settled at +%.1fs (quiet since +%.1fs): %u texture GETs, %u mesh GETs, %.1f MB mesh",
                                           t, s_quiet_since, tex_total, mesh_gets, LLMeshRepository::sBytesReceived / 1048576.0) << LL_ENDL;
    }
    if (t >= 300.0) s_done = true;
}

// #485: asset fetch gate stats, every 5 s for the whole session while anything is
// queued, in flight or was sent since the last line (the timeline above only covers
// 5 minutes after login / teleport). SLFetchStatsLog = false disables it. Rates are
// per second over the interval; "conns" = sockets open now (in use + pooled idle) and
// new ones opened per second -- with keep-alive working, new/s stays ~0.
static void assetFetchGateStatsTick()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SLFetchStatsLog", true);
    if (!s_on || LLStartUp::getStartupState() < STATE_STARTED) return;
    constexpr F64 INTERVAL = 5.0;
    static F64 s_next = 0.0, s_last = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (now < s_next) return;
    s_next = now + INTERVAL;

    struct ClassSample { U64 opened = 0, closed = 0, requests = 0, bytes = 0, backoffs = 0; bool paused = false; };
    auto sample = [](LLAppCoreHttp::EAppPolicy ap) -> ClassSample
    {
        ClassSample c;
        const LLCore::HttpRequest::policy_t pc = LLAppViewer::instance()->getAppCoreHttp().getPolicy(ap);
        LLCore::HttpRequest::getSocketStats(pc, c.opened, c.closed);
        LLCore::HttpRequest::TransferStats ts;
        LLCore::HttpRequest::getTransferStats(pc, ts);
        c.requests = ts.mRequests;
        c.bytes = ts.mBytesIn;
        LLCore::HttpRequest::getBackoffStats(pc, c.backoffs, c.paused);
        return c;
    };
    const ClassSample tex = sample(LLAppCoreHttp::AP_TEXTURE);
    const ClassSample aux = sample(LLAppCoreHttp::AP_TEXTURE_AUX);
    const ClassSample mesh = sample(LLAppCoreHttp::AP_MESH2);
    const ClassSample big = sample(LLAppCoreHttp::AP_LARGE_MESH);

    LLTextureFetch::FetchGateStats fg;
    if (LLTextureFetch* tf = LLAppViewer::getTextureFetch())
    {
        tf->getFetchGateStats(fg);
    }
    const S32 mesh_in_flight = LLMeshRepoThread::sRequestWaterLevel;
    const S32 mesh_pending = (S32)gMeshRepo.mPendingRequests.size();
    const U64 mesh_dropped = LLMeshRepository::sMeshDroppedUnsent;

    static LLTextureFetch::FetchGateStats s_fg;
    static ClassSample s_tex, s_aux, s_mesh, s_big;
    static U64 s_mesh_dropped = 0;
    const F64 dt = s_last > 0.0 ? now - s_last : 0.0;
    s_last = now;
    const bool active = fg.mInFlight > 0 || fg.mInFlightAux > 0 || fg.mQueued > 0 || fg.mSent != s_fg.mSent
                        || mesh_in_flight > 0 || mesh_pending > 0 || mesh.requests != s_mesh.requests;
    // #485 network draw distance gate, textures (LLViewerFetchedTexture::vkNetGated):
    // gated now, gate evaluations that held a texture back /s, queued fetches withdrawn /s
    static U64 s_gate_skipped = 0, s_gate_dropped = 0;
    std::string tex_gate;
    if (dt > 0.0)
    {
        const U64 gs = LLViewerFetchedTexture::sNetGateSkipped, gd = LLViewerFetchedTexture::sNetGateDropped;
        tex_gate = llformat(" | net-dd gate %.0f m: tex gated %u (re-checked gated %.0f/s) dropped-queued %.1f/s",
                            gAgentCamera.getNetworkDrawDistance(), LLViewerFetchedTexture::sNetGateGated,
                            gs >= s_gate_skipped ? (F64)(gs - s_gate_skipped) / dt : 0.0,
                            gd >= s_gate_dropped ? (F64)(gd - s_gate_dropped) / dt : 0.0);
    }
    s_gate_skipped = LLViewerFetchedTexture::sNetGateSkipped;
    s_gate_dropped = LLViewerFetchedTexture::sNetGateDropped;
    // #485 mesh: network-DD gate (LLMeshRepository::netGateAllows) -- new loads not
    // requested /s, pending ones dropped /s, deferred (waiting to come back in range) and
    // re-issued /s; pending cap drops /s and the pending list's peak / approximate bytes;
    // skin requests dropped because no object shows that mesh any more.
    static U64 s_mg_skip = 0, s_mg_drop = 0, s_mg_cap = 0, s_mg_reissue = 0, s_mg_noparams = 0;
    std::string mesh_extra;
    if (dt > 0.0)
    {
        auto mrate = [dt](U64 cur, U64 prev) { return cur >= prev ? (F64)(cur - prev) / dt : 0.0; };
        mesh_extra = llformat(" | mesh gate %.0f m: skipped %.1f/s dropped-queued %.1f/s deferred %u (overflow %llu) reissued %.1f/s"
                              " | mesh pending cap-dropped %.1f/s peak %u ~%.1f MB, skin no-params dropped %.1f/s",
                              gMeshRepo.mNetGateDist,
                              mrate(LLMeshRepository::sMeshGateSkipped, s_mg_skip),
                              mrate(LLMeshRepository::sMeshGateDropped, s_mg_drop),
                              (U32)gMeshRepo.getDeferredLoadCount(),
                              (unsigned long long)LLMeshRepository::sMeshDeferredOverflow,
                              mrate(LLMeshRepository::sMeshDeferredReissued, s_mg_reissue),
                              mrate(LLMeshRepository::sMeshCapDropped, s_mg_cap),
                              LLMeshRepository::sMeshPendingPeak,
                              gMeshRepo.getPendingRequestBytes() / 1048576.0,
                              mrate(LLMeshRepository::sMeshSkinNoParamsDropped, s_mg_noparams));
    }
    s_mg_skip = LLMeshRepository::sMeshGateSkipped;
    s_mg_drop = LLMeshRepository::sMeshGateDropped;
    s_mg_cap = LLMeshRepository::sMeshCapDropped;
    s_mg_reissue = LLMeshRepository::sMeshDeferredReissued;
    s_mg_noparams = LLMeshRepository::sMeshSkinNoParamsDropped;
    if (active && dt > 0.0)
    {
        auto rate = [dt](U64 cur, U64 prev) { return cur >= prev ? (F64)(cur - prev) / dt : 0.0; };
        LL_INFOS("AssetFetch") << llformat("FetchGate: tex in-flight %d/%d (+%d aux) queued %d (age p50 %.1fs p95 %.1fs) decode-wait %d"
                                           " | sent %.0f/s dropped-unsent %.0f/s (cap %.0f/s)"
                                           " | mesh in-flight %d/%d pending %d delayed %d skin-wait %d sent %.0f/s dropped-unsent %.1f/s"
                                           " | conns open/new-per-s tex %lld/%.1f aux %lld/%.1f mesh %lld/%.1f large %lld/%.1f"
                                           " | MB/s tex %.2f mesh %.2f | busy back-offs %llu%s",
            fg.mInFlight, fg.mInFlightLimit, fg.mInFlightAux, fg.mQueued, fg.mQueueAgeP50, fg.mQueueAgeP95, fg.mGovWaiting,
            rate(fg.mSent, s_fg.mSent), rate(fg.mDroppedUnsent, s_fg.mDroppedUnsent), rate(fg.mDroppedCap, s_fg.mDroppedCap),
            mesh_in_flight, LLMeshRepoThread::sRequestHighWater, mesh_pending,
            (gMeshRepo.mThread ? gMeshRepo.mThread->getDelayedCount() : 0), (S32)LLMeshRepoThread::sSkinAwaitingHeader.load(),
            rate(mesh.requests, s_mesh.requests),
            rate(mesh_dropped, s_mesh_dropped),
            (long long)tex.opened - (long long)tex.closed, rate(tex.opened, s_tex.opened),
            (long long)aux.opened - (long long)aux.closed, rate(aux.opened, s_aux.opened),
            (long long)mesh.opened - (long long)mesh.closed, rate(mesh.opened, s_mesh.opened),
            (long long)big.opened - (long long)big.closed, rate(big.opened, s_big.opened),
            rate(tex.bytes + aux.bytes, s_tex.bytes + s_aux.bytes) / 1048576.0,
            rate(mesh.bytes + big.bytes, s_mesh.bytes + s_big.bytes) / 1048576.0,
            (unsigned long long)(tex.backoffs + aux.backoffs + mesh.backoffs + big.backoffs),
            (tex.paused || mesh.paused || big.paused || aux.paused) ? " (PAUSED now)" : "") << tex_gate << mesh_extra << LL_ENDL;
    }
    s_fg = fg;
    s_tex = tex; s_aux = aux; s_mesh = mesh; s_big = big;
    s_mesh_dropped = mesh_dropped;
}

// Advances the global frame clock once per frame, before anything that reads it
// (network, agent, UI, object update). Previously this lived inside
// LLViewerObjectList::update(), so everything earlier in idle() saw last frame's time
// and frames that skipped the object update (teleport, disconnect) froze it.
static void updateFrameClock()
{
    const F32 last_time = gFrameTimeSeconds;
    // Time _can_ go backwards, for example if the user changes the system clock.
    // It doesn't cause any fatal problems (just some oddness with stats), so we shouldn't assert here.
    U64Microseconds time = totalTime();
    gFrameTime = time;
    F64Seconds time_since_start = gFrameTime - gStartTime;
    gFrameTimeSeconds = time_since_start;

    gFrameIntervalSeconds = gFrameTimeSeconds - last_time;
    if (gFrameIntervalSeconds < 0.f)
    {
        gFrameIntervalSeconds = 0.f;
    }
    LLCrossingTrace::onFrame(gFrameIntervalSeconds);    // #568
    LLWorldClock::tick();   // world pause: before every world-time reader this frame
}

void LLAppViewer::idle()
{
    assetFetchTimelineTick();
    assetFetchGateStatsTick();  // #485
    SLDataUsage::idleTick();    // #529 session data usage
    LL_PROFILE_ZONE_SCOPED_CATEGORY_APP;
    VK_HITCH_SCOPE("LL_idle_total");
    pingMainloopTimeout("Main:Idle");

    // Update frame timers
    static LLTimer idle_timer;

    LLFrameTimer::updateFrameTime();
    LLFrameTimer::updateFrameCount();
    LLEventTimer::updateClass();
    // #736: the risky-setting probation clock (in-world time only).
    SLRiskySettings::tick(LLFrameTimer::getFrameDeltaTimeF32(), LLStartUp::getStartupState() >= STATE_STARTED);
    LLPerfStats::updateClass();

    // LLApp::stepFrame() performs the above three calls plus mRunner.run().
    // Not sure why we don't call stepFrame() here, except that LLRunner seems
    // completely redundant with LLEventTimer.
    LLNotificationsUI::LLToast::updateClass();
    LLSmoothInterpolation::updateInterpolants();
    LLMortician::updateClass();
    LLFilePickerThread::clearDead();  //calls LLFilePickerThread::notify()
    LLDirPickerThread::clearDead();
    F32 dt_raw = idle_timer.getElapsedTimeAndResetF32();

    LLGLTFMaterialList::flushUpdates();

    LLTextureData::updateClass();

    // RAM #262: system memory governor. Reads system memory availability (~4 Hz, self
    // rate-limited), paces decodes / mesh unzip / HTTP concurrency, asks the VK residency
    // to trim CPU texture copies sooner under pressure, and logs the RSS timeline after
    // login / teleport. Retained CPU texture copies are its "retained" input.
    {
        static LLCachedControl<bool> s_gov(gSavedSettings, "RenderMemoryGovernor", true);
        static LLCachedControl<U32> s_reserve(gSavedSettings, "MemoryReserveMB", 0);
        static LLCachedControl<F32> s_timeline(gSavedSettings, "MemoryTimelineSeconds", 90.f);
        static LLCachedControl<U32> s_mmap_kb(gSavedSettings, "MemoryMmapThresholdKB", 2048);
        static bool s_extra_set = false;
        if (!s_extra_set)
        {
            s_extra_set = true;
            LLMemoryGovernor::setExtraInfo([]() -> std::string
            {
                const S64 cpu = llmax(LLTextureData::getVKCpuBytes(), (S64)0);
                std::string s = llformat("cpu-tex %lld MB / %d copies, raw imgs %d",
                                         (long long)(cpu >> 20), (S32)LLTextureData::getVKCpuCount(),
                                         (S32)LLImageRaw::sRawImageCount.load());
                if (LLImageDecodeThread* dec = LLAppViewer::getImageDecodeThread())
                    s += llformat(", decode queue %d", (S32)dec->getPending());
                if (LLTextureFetch* fetch = LLAppViewer::getTextureFetch())
                    s += llformat(", fetches %d (http %d)", fetch->getNumRequests(), fetch->getNumHTTPRequests());
                // #485 RAM ATTRIBUTION: every LLImageRaw byte (cpu-tex above is the retained
                // VK copies only; the rest is held by fetch workers, textures' own raws,
                // decode scratch, callbacks), encoded image bytes, the fetch workers by state
                // and what they hold (worker sweep, SLFetchMaxWorkers / SLFetchWorkerIdleSec /
                // SLFetchWorkerUncollectedSec / SLFetchWorkerHeldMB), object / texture counts,
                // and the malloc heap (in use vs free-but-retained: fragmentation).
                s += llformat(" | raw-bytes %lld MB / %d, formatted %lld MB / %d",
                              (long long)(llmax(LLImageRaw::sRawImageBytes.load(), (S64)0) >> 20),
                              (S32)LLImageRaw::sRawImageCount.load(),
                              (long long)(llmax(LLImageFormatted::sGlobalFormattedMemory.load(), (S64)0) >> 20),
                              (S32)LLImageFormatted::sFormattedImageCount.load());
                if (LLTextureFetch* fetch = LLAppViewer::getTextureFetch())
                {
                    static LLCachedControl<U32> s_max_workers(gSavedSettings, "SLFetchMaxWorkers", 20000);
                    const LLTextureFetch::WorkerStats& w = fetch->getWorkerStats();
                    s += llformat(" | workers %d (peak %d cap %u): cache %d net-wait %d in-flight %d decode %d write %d"
                                  " uncollected %d idle %d busy %d; hold raw %lld MB (worker-only %lld, idle/done %lld)"
                                  " fmt %lld MB http %lld MB; evicted idle %llu uncollected %llu (cap %llu) sweep %.1f ms",
                                  w.mTotal, w.mPeak, (U32)s_max_workers,
                                  w.mCache, w.mNetWait, w.mInFlight, w.mDecode, w.mWrite, w.mUncollected, w.mIdle, w.mBusy,
                                  (long long)(w.mRawBytes >> 20), (long long)(w.mRawOnlyBytes >> 20),
                                  (long long)(w.mIdleRawBytes >> 20), (long long)(w.mFormattedBytes >> 20),
                                  (long long)(w.mHttpBytes >> 20),
                                  (unsigned long long)w.mEvictedIdle, (unsigned long long)w.mEvictedUncollected,
                                  (unsigned long long)w.mEvictedCap, w.mSweepMs);
                }
                s += llformat(" | objects %d textures %d (net-gated %u; tex raws %d aux %d, create-pending %d)",
                              gObjectList.getNumObjects(), (S32)gTextureList.getNumImages(),
                              LLViewerFetchedTexture::sNetGateGated, LLViewerFetchedTexture::sRawCount,
                              LLViewerFetchedTexture::sAuxCount, (S32)gTextureList.mCreateTextureList.size());
#if LL_LINUX && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
                // mallinfo2() walks every free chunk of every arena under the arena locks: hundreds
                // of ms on a multi-GB heap, stalling every allocating thread. Opt-in only.
                static LLCachedControl<bool> s_heap_stats(gSavedSettings, "SLMemLogHeapStats", false);
                if (s_heap_stats)
                {
                    const struct mallinfo2 mi = mallinfo2();
                    s += llformat(" | heap arena %llu MB in-use %llu free %llu mmap %llu MB",
                                  (unsigned long long)(mi.arena >> 20), (unsigned long long)(mi.uordblks >> 20),
                                  (unsigned long long)(mi.fordblks >> 20), (unsigned long long)(mi.hblkhd >> 20));
                }
#endif
                // #458: Vulkan host-visible memory + allocation traffic
                const std::string hm = LLVulkanContext::instance().hostMemText();
                if (!hm.empty()) s += " | vk " + hm;
                // #883: the largest tracked containers, from the last MemTrend report (no
                // probe runs here; empty until the first report).
                const std::string mt = LLMemTrend::compactLine(6);
                if (!mt.empty()) s += " | " + mt;
                return s;
            });
        }
        LLMemoryGovernor::configure(s_gov, s_reserve, s_timeline, s_mmap_kb);
        // #698: allocation failures are never a crash -- emergency reserve, retry policy for
        // the decode / mesh owners, the forced-CRITICAL shed window and the test injection.
        {
            static LLCachedControl<U32> s_oom_reserve(gSavedSettings, "SLMemEmergencyReserveMB", 96);
            static LLCachedControl<U32> s_oom_retries(gSavedSettings, "SLMemAllocRetryLimit", 4);
            static LLCachedControl<F32> s_oom_shed(gSavedSettings, "SLMemOOMShedSec", 15.f);
            static LLCachedControl<U32> s_oom_inject(gSavedSettings, "SLMemInjectAllocFail", 0);
            LLMemoryGovernor::configureAllocFailure(s_oom_reserve, s_oom_retries, s_oom_shed, s_oom_inject);
        }
        LLMemoryGovernor::setRetainedBytes((U64)llmax(LLTextureData::getVKCpuBytes(), (S64)0));
        LLMemoryGovernor::update();
        // #883: periodic leak report -- RSS trend, malloc heap, the largest / fastest-growing
        // tracked containers (probes registered in llmemtrendprobes.cpp). An overnight AFK
        // session that grows names the container in its own log.
        {
            static LLCachedControl<F32> s_trend_min(gSavedSettings, "MemTrendReportMinutes", 10.f);
            static LLCachedControl<U32> s_trend_top(gSavedSettings, "MemTrendTopN", 10);
            static LLCachedControl<U32> s_trend_trim(gSavedSettings, "MemTrendTrimFreeMB", 512);
            // From login on: the first report (one interval in) is the baseline, so it is
            // taken in the world, not at the login screen.
            if (LLStartUp::getStartupState() >= STATE_STARTED)
            {
                static const bool s_probes = (registerMemTrendProbes(), true);
                (void)s_probes;
                LLMemTrend::tick(LLTimer::getTotalSeconds(), (F64)s_trend_min * 60.0, s_trend_top,
                                 (U64)(U32)s_trend_trim << 20);
            }
        }
        // #458: Vulkan allocation-traffic sampling (logs while high or memory is tight)
        LLVulkanContext::instance().hostMemTick(LLFrameTimer::getElapsedSeconds().value(),
                                                LLMemoryGovernor::getState() != LLMemoryGovernor::COMFORTABLE);
    }

    // Service the WorkQueue we use for replies from worker threads.
    // Use function statics for the timeslice setting so we only have to fetch
    // and convert MainWorkTime once.
    static F32 MainWorkTimeRaw = gSavedSettings.getF32("MainWorkTime");
    static F32Milliseconds MainWorkTimeMs(MainWorkTimeRaw);
    // MainWorkTime is specified in fractional milliseconds, but std::chrono
    // uses integer representations. What if we want less than a microsecond?
    // Use nanoseconds. We're very sure we will never need to specify a
    // MainWorkTime that would be larger than we could express in
    // std::chrono::nanoseconds.
    static std::chrono::nanoseconds MainWorkTimeNanoSec{
        std::chrono::nanoseconds::rep(MainWorkTimeMs.value() * 1000000)};
    {
        VK_HITCH_SCOPE("LL_mainloopWork");
        gMainloopWork.runFor(MainWorkTimeNanoSec);
        // Backlogged (texture downscale / material replies during bursts): spend part of
        // the previous frame's headroom against a 60 Hz budget, more while loading
        // since nothing paces those frames.
        if (gMainloopWork.size() > 0)
        {
            constexpr F32 TARGET_FRAME_SEC = 1.f / 60.f;
            constexpr F32 MAX_EXTRA_SEC = 0.002f;
            constexpr F32 MAX_EXTRA_LOADING_SEC = 0.006f;
            const bool loading = LLStartUp::getStartupState() < STATE_STARTED;
            const F32 headroom = TARGET_FRAME_SEC - dt_raw;
            const F32 extra = loading ? MAX_EXTRA_LOADING_SEC
                                      : llclamp(headroom * 0.25f, 0.f, MAX_EXTRA_SEC);
            if (extra > 0.f)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_APP("mainloopWork extra");
                gMainloopWork.runFor(std::chrono::nanoseconds(std::chrono::nanoseconds::rep(extra * 1e9f)));
            }
        }
    }
    // #568: object cache I/O completions on their own queue, every frame (login included):
    // a region's cache read landing sends the handshake reply the sim waits on before it
    // streams that region's objects. VOCacheCompletionBudgetMs 0 = the mainloop queue above.
    {
        static LLCachedControl<F32> s_vocache_budget(gSavedSettings, "VOCacheCompletionBudgetMs", 3.f);
        if (LLVOCache::instanceExists())
        {
            VK_HITCH_SCOPE("vocache_completions");
            LLVOCache::instance().drainCompletions(s_vocache_budget);
        }
    }

    // Cap out-of-control frame times
    // Too low because in menus, swapping, debugger, etc.
    // Too high because idle called with no objects in view, etc.
    const F32 MIN_FRAME_RATE = 1.f;
    // Keyboard turning, camera orbit and ping interpolation scale by this dt; a 200 FPS cap
    // made them run proportionally fast above 200 FPS.
    const F32 MAX_FRAME_RATE = 2000.f;

    F32 frame_rate_clamped = 1.f / dt_raw;
    frame_rate_clamped = llclamp(frame_rate_clamped, MIN_FRAME_RATE, MAX_FRAME_RATE);
    gFrameDTClamped = 1.f / frame_rate_clamped;

    // Global frame timer
    // Smoothly weight toward current frame
    gFPSClamped = (frame_rate_clamped + (4.f * gFPSClamped)) / 5.f;

    static LLCachedControl<F32> quit_after_seconds(gSavedSettings, "QuitAfterSeconds");
    F32 qas = quit_after_seconds;
    if (qas > 0.f)
    {
        if (gRenderStartTime.getElapsedTimeF32() > qas)
        {
            LL_INFOS() << "Quitting after " << qas << " seconds. See setting \"QuitAfterSeconds\"." << LL_ENDL;
            LLAppViewer::instance()->forceQuit();
        }
    }

    // Must wait until both have avatar object and mute list, so poll
    // here.
    LLIMProcessing::requestOfflineMessages();

    // Discord Rich Presence (#205): self rate-limited to one tick per second and
    // never blocks; decides for itself whether we're logged in / enabled.
    SLDiscordPresence::idle();

    // Update check (#293): a few seconds after login, then once a day; cheap otherwise.
    SLUpdateChecker::idle();

    // Certificate revocation: CRLSet refresh shortly after startup, then every 6 h.
    SLRevocationUpdater::idle();

    ///////////////////////////////////
    //
    // Special case idle if still starting up
    //
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        // Skip rest if idle startup returns false (essentially, no world yet)
        if (!idle_startup())
        {
            return;
        }
    }

    updateFrameClock();

    F32 yaw = 0.f;              // radians

    if (!gDisconnected)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_NETWORK("network"); //LL_RECORD_BLOCK_TIME(FTM_NETWORK);
        // Update spaceserver timeinfo
        LLWorld::getInstance()->setSpaceTimeUSec(LLWorld::getInstance()->getSpaceTimeUSec() + LLUnits::Seconds::fromValue(dt_raw));


        //////////////////////////////////////
        //
        // Update simulator agent state
        //

        static LLCachedControl<bool> rotate_right(gSavedSettings, "RotateRight");
        if (rotate_right)
        {
            gAgent.moveYaw(-1.f);
        }

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_APP("Autopilot");
            // Handle automatic walking towards points
            gAgentPilot.updateTarget();
            gAgent.autoPilot(&yaw);
        }

        // Auto-derive the active game-control mode (Avatar/Captive/FlyCam) from
        // current avatar state so the runtime mappings and gating track it.
        gAgent.updateGameControlMode();

        // Read the agent's control flags now, before applyExternalActions() below
        // (which drives the avatar from a real game controller) has a chance to
        // fold any controller-driven movement into them this frame -- so the
        // keyboard-derived GameControl state computed here can never be sourced
        // from the controller's own output.  See LLGameControllerManager::
        // setExternalInput()/accumulateFinalState() for where the two are
        // combined (only as fully-independent final ServerStates).
        U32 control_flags = gAgent.getControlFlags();

        // apply to GameControl
        LLGameControl::setExternalInput(control_flags, gAgent.getGameControlButtonsFromKeys(), gAgent.getRunning());

        // Feed the actual on-screen cursor position to GameControl
        // Matches the same whole-window clamp LLViewerWindow::moveCursorTo()
        // uses to keep a gamepad-driven cursor on-screen.
        if (LLGameControl::getAgentControlMode() == LLGameControl::CONTROL_MODE_CURSOR)
        {
            LLCoordGL cursor_pos = gViewerWindow->getCurrentMouse();
            S32 rect_width = gViewerWindow->getWindowWidthScaled();
            S32 rect_height = gViewerWindow->getWindowHeightScaled();
            S32 pixel_x = llclamp(cursor_pos.mX, 0, rect_width);
            S32 pixel_y = llclamp(rect_height - cursor_pos.mY, 0, rect_height);
            LLGameControl::setMouseCursorPosition(pixel_x, pixel_y, rect_width, rect_height);
        }

        // OCOL: buttons bound to viewer actions or gestures don't drive their
        // game control action (nothing is reserved while a pref is capturing input)
        if (LLPanelPreferenceGameControl::isWaitingForInputChannel() || LLSetKeyBindDialog::isRecording())
        {
            LLGameControl::setReservedButtons(0);
        }
        else
        {
            LLGameControl::setReservedButtons(gViewerInput.getBoundControllerButtons()
                | LLGestureMgr::instance().getControllerBoundButtons());
        }

        if (LLPanelPreferenceGameControl::isWaitingForInputChannel())
        {
            gViewerInput.releaseControllerKeys();
            LLPanelPreferenceGameControl::applyGameControlInput();
        }
        else
        {
            // Recompute canonical controller state (also populates the internal
            // state consumed below) and send it to the server when it changed.
            // GameControlData goes only to a region that advertises the
            // "GameControlEnabled" simulator feature (LL d4270de9ea); OpenSim regions
            // don't. While unsupported the resend deadline is left untouched, so the
            // first frame after the feature appears sends immediately.
            if (LLGameControl::computeFinalStateAndCheckForChanges())
            {
                LLViewerRegion* regionp = gAgent.getRegion();
                if (regionp && regionp->gameControlEnabled())
                {
                    sendGameControlData();
                }
            }

            // Drive the local avatar from the controller.  In Avatar/Captive
            // modes this moves the avatar; in FlyCam mode applyExternalActions
            // ignores movement bits (it early-returns while flycam is active) but
            // still services the flycam on/off toggle, so we run it either way.
            // Flycam *motion* is driven separately below via gAgent.updateFlycam().
            if (LLGameControl::willControlAvatar() || LLGameControl::willControlFlycam())
            {
                LLGameControl::AgentActions game_control_actions = LLGameControl::computeAgentActions();
                gAgent.applyExternalActions(game_control_actions);
            }

            // OCOL: controller key bindings and gesture buttons (not while the
            // key binding dialog is waiting for a controller input)
            if (!LLSetKeyBindDialog::isRecording())
            {
                gViewerInput.scanControllerKeys();
                gViewerInput.scanController();
                LLGestureMgr::instance().updateControllerGestures();
            }
            else
            {
                gViewerInput.releaseControllerKeys();
            }
        }

        {
            // Mouse walk (#254): both buttons held while steering your avatar
            // walks forward. Before the agent update is sent, like the keys.
            LLToolCamera::getInstance()->updateMouseWalk();
        }

        {
            // Third-person shooter (TPS) camera: mouse orbit, camera-relative
            // movement and engage/release bookkeeping. After key scanning (sets
            // the movement input), before the agent update is sent.
            SLTPSCamera::getInstance()->update();
        }

        // Client AO (#496): see this frame's movement keys before they are
        // sent, so a locomotion replacement can go out with the same update.
        SLSwimController::instance().update(); // simulated swimming (#496): may add vertical controls
        if (SLAOEngine::instanceExists())
        {
            SLAOEngine::instance().onControls(gAgent.getControlFlags());
        }

        send_agent_update(false);

        // After calling send_agent_update() in the mainloop we always clear
        // the agent's ephemeral ControlFlags (whether an AgentUpdate was
        // actually sent or not) because these will be recomputed based on
        // real-time key/controller input and resubmitted next frame.
        gAgent.resetControlFlags();
    }

    //////////////////////////////////////
    //
    // Manage statistics
    //
    //
    {
        // Initialize the viewer_stats_timer with an already elapsed time
        // of SEND_STATS_PERIOD so that the initial stats report will
        // be sent immediately.
        static LLFrameStatsTimer viewer_stats_timer(SEND_STATS_PERIOD);

        // Update session stats every large chunk of time
        // *FIX: (?) SAMANTHA
        if (viewer_stats_timer.getElapsedTimeF32() >= SEND_STATS_PERIOD && !gDisconnected)
        {
            LL_INFOS() << "Transmitting sessions stats" << LL_ENDL;
            bool include_preferences = false;
            send_viewer_stats(include_preferences);
            viewer_stats_timer.reset();
        }

        // Print the object debugging stats
        static LLFrameTimer object_debug_timer;
        if (object_debug_timer.getElapsedTimeF32() > 5.f)
        {
            object_debug_timer.reset();
            if (gObjectList.mNumDeadObjectUpdates)
            {
                LL_INFOS() << "Dead object updates: " << gObjectList.mNumDeadObjectUpdates << LL_ENDL;
                gObjectList.mNumDeadObjectUpdates = 0;
            }
            if (gObjectList.mNumUnknownUpdates)
            {
                LL_INFOS() << "Unknown object updates: " << gObjectList.mNumUnknownUpdates << LL_ENDL;
                gObjectList.mNumUnknownUpdates = 0;
            }

        }
    }

    if (!gDisconnected)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Network");

        ////////////////////////////////////////////////
        //
        // Network processing
        //
        // NOTE: Starting at this point, we may still have pointers to "dead" objects
        // floating throughout the various object lists.
        //
        idleNameCache();
        VK_HITCH_SCOPE("LL_idleNetwork");
        idleNetwork();


        // Check for away from keyboard, kick idle agents.
        idle_afk_check();

        //  Update statistics for this frame
        update_statistics();
    }

    ////////////////////////////////////////
    //
    // Handle the regular UI idle callbacks as well as
    // hover callbacks
    //

#ifdef LL_DARWIN
    if (!mQuitRequested)  //MAINT-4243
#endif
    {
//      LL_RECORD_BLOCK_TIME(FTM_IDLE_CB);

        // Do event notifications if necessary.  Yes, we may want to move this elsewhere.
        gEventNotifier.update();

        { VK_HITCH_SCOPE("LL_idleCallbacks"); gIdleCallbacks.callFunctions(); }
        gInventory.idleNotifyObservers();
        LLAvatarTracker::instance().idleNotifyObservers();
    }

    // Metrics logging (LLViewerAssetStats, etc.)
    {
        static LLTimer report_interval;

        // *TODO:  Add configuration controls for this
        F32 seconds = report_interval.getElapsedTimeF32();
        if (seconds >= app_metrics_interval)
        {
            metricsSend(! gDisconnected);
            report_interval.reset();
        }
    }


    // Update layonts, handle mouse events, tooltips, e t c
    // updateUI() needs to be called even in case viewer disconected
    // since related notification still needs handling and allows
    // opening chat.
    { VK_HITCH_SCOPE("LL_updateUI"); gViewerWindow->updateUI(); }

    if (gDisconnected)
    {
        return;
    }

    if (gTeleportDisplay)
    {
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_ARRIVING)
        {
            // Teleported, but waiting for things to load, start processing surface data
            {
                LL_RECORD_BLOCK_TIME(FTM_NETWORK);
                gVLManager.unpackData();
            }
            {
                LL_RECORD_BLOCK_TIME(FTM_REGION_UPDATE);
                const F32 max_region_update_time = .001f; // 1ms
                LLWorld::getInstance()->updateRegions(max_region_update_time);
            }
        }

        return;
    }

    // VRAM soft ladder (#405): the EFFECTIVE LOD factors / draw distance follow the Vulkan
    // renderer's current caps (settings untouched); while the far-geometry rung holds the
    // distance below the setting, objects past it go back to the region caches.
    applyVramDetailCaps();
    vramUnloadFarObjects();

    ///////////////////////////////////////
    // Agent and camera movement
    //
    LLCoordGL current_mouse = gViewerWindow->getCurrentMouse();

    {
        // After agent and camera moved, figure out if we need to
        // deselect objects.
        LLSelectMgr::getInstance()->deselectAllIfTooFar();

    }

    {
        // Handle pending gesture processing
        LL_RECORD_BLOCK_TIME(FTM_AGENT_POSITION);
        LLGestureMgr::instance().update();

        VK_HITCH_SCOPE("LL_agentPosition");
        gAgent.updateAgentPosition(gFrameDTClamped, yaw, current_mouse.mX, current_mouse.mY);
    }

    {
        LL_RECORD_BLOCK_TIME(FTM_OBJECTLIST_UPDATE);

        if (!(logoutRequestSent() && hasSavedFinalSnapshot()))
        {
            VK_HITCH_SCOPE("LL_objectListUpdate");
            gObjectList.update(gAgent);
            { VK_HITCH_SCOPE("objUpd_gltf"); LL::GLTFSceneManager::instance().update(); }
        }
    }

    //////////////////////////////////////
    //
    // Deletes objects...
    // Has to be done after doing idleUpdates (which can kill objects)
    //

    {
        LL_RECORD_BLOCK_TIME(FTM_CLEANUP);
        {
            // #568: one step of incremental region teardown (a slice of a removed region's
            // objects, its cache save, or its delete), before the dead-object compaction.
            VK_HITCH_SCOPE("LL_retiringRegions");
            LLWorld::getInstance()->updateRetiringRegions();
        }
        {
            VK_HITCH_SCOPE("LL_cleanDead");
            gObjectList.cleanDeadObjects();
        }
        {
            LL_RECORD_BLOCK_TIME(FTM_CLEANUP_DRAWABLES);
            LLDrawable::cleanupDeadDrawables();
        }
    }

    //
    // After this point, in theory we should never see a dead object
    // in the various object/drawable lists.
    //

    //////////////////////////////////////
    //
    // Update/send HUD effects
    //
    // At this point, HUD effects may clean up some references to
    // dead objects.
    //

    {
        LL_RECORD_BLOCK_TIME(FTM_HUD_EFFECTS);
        VK_HITCH_SCOPE("LL_selectEffects");
        LLSelectMgr::getInstance()->updateEffects();
        LLHUDManager::getInstance()->cleanupEffects();
        LLHUDManager::getInstance()->sendEffects();
    }

    ////////////////////////////////////////
    //
    // Unpack layer data that we've received
    //

    {
        LL_RECORD_BLOCK_TIME(FTM_NETWORK);
        gVLManager.unpackData();
    }

    /////////////////////////
    //
    // Update surfaces, and surface textures as well.
    //

    { VK_HITCH_SCOPE("LL_updateVisibilities"); LLWorld::getInstance()->updateVisibilities(); }
    {
        const F32 max_region_update_time = .001f; // 1ms
        LL_RECORD_BLOCK_TIME(FTM_REGION_UPDATE);
        VK_HITCH_SCOPE("LL_updateRegions");
        LLWorld::getInstance()->updateRegions(max_region_update_time);
    }

    /////////////////////////
    //
    // Update weather effects
    //

    // Update wind vector
    LLVector3 wind_position_region;
    static LLVector3 average_wind;

    LLViewerRegion *regionp;
    regionp = LLWorld::getInstance()->resolveRegionGlobal(wind_position_region, gAgent.getPositionGlobal());    // puts agent's local coords into wind_position
    if (regionp)
    {
        gWindVec = regionp->mWind.getVelocity(wind_position_region);

        // Compute average wind and use to drive motion of water

        average_wind = regionp->mWind.getAverage();
        gSky.setWind(average_wind);
        //LLVOWater::setWind(average_wind);
    }
    else
    {
        gWindVec.setVec(0.0f, 0.0f, 0.0f);
    }

    //////////////////////////////////////
    //
    // Sort and cull in the new renderer are moved to pipeline.cpp
    // Here, particles are updated and drawables are moved.
    //

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_APP("world update"); //LL_RECORD_BLOCK_TIME(FTM_WORLD_UPDATE);
        VK_HITCH_SCOPE("LL_updateMove");
        gPipeline.updateMove();
    }

    { VK_HITCH_SCOPE("LL_updateParticles"); LLWorld::getInstance()->updateParticles(); }

    if (gAgentPilot.isPlaying() && gAgentPilot.getOverrideCamera())
    {
        gAgentPilot.moveCamera();
    }
    else if (gAgentPilot.isPreviewing())
    {   // camera recorder: preview the selected waypoint's pose
        gAgentPilot.applyPreviewCamera();
    }
    else if (gAgent.isUsingFlycam())
    {
        // Flycam is now a single shared state (LLAgentCamera) driven by
        // whichever input sources are actually present; each is a no-op
        // when its device/mode isn't active, so it's safe to feed both
        // every frame rather than picking one exclusively.
        LLViewerJoystick::getInstance()->moveFlycam();
        gAgent.updateFlycam();
    }
    else
    {
        if (LLToolMgr::getInstance()->inBuildMode())
        {
            LLViewerJoystick::getInstance()->moveObjects();
        }

        VK_HITCH_SCOPE("LL_updateCamera");
        gAgentCamera.updateCamera();
    }

    // HUD edit zoom (#413): ease toward the wheel target once per frame, whatever
    // drives the camera (flycam / pilot skip updateCamera).
    gAgentCamera.updateHUDZoom();

    // update media focus
    LLViewerMediaFocus::getInstance()->update();

    // Update marketplace
    LLMarketplaceInventoryImporter::update();
    LLMarketplaceInventoryNotifications::update();

    // objects and camera should be in sync, do LOD calculations now
    {
        LL_RECORD_BLOCK_TIME(FTM_LOD_UPDATE);
        VK_HITCH_SCOPE("LL_apparentAngles");
        gObjectList.updateApparentAngles(gAgent);
    }

    // #783 M2: the region / parcel weather source (only with the user's opt-in; reads, never sends).
    SLWeatherRegion::update();

    // #892: the parcel speaker registry (ParcelAudioSpeakers; off: nothing at all).
    SLParcelSpeakers::update();

    // Update AV render info
    LLAvatarRenderInfoAccountant::getInstance()->idle();

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_APP("audio update"); //LL_RECORD_BLOCK_TIME(FTM_AUDIO_UPDATE);

        if (gAudiop)
        {
            audio_update_volume(false);
            audio_update_listener();
            audio_update_wind(false);
            // #683 acoustics (AudioAcoustics): EFX on / off, GPU answers -> reverb + occlusion.
            LLCleanAcoustics::update();
            // #783 weather ambience (rain / roof / wind / snow beds by cover; uses the above).
            SLWeatherAudio::update();
            // #892 the parcel stream through in-world speakers / the stereo bed (ParcelAudioCapture).
            SLParcelSpeakerAudio::update();

            // this line actually commits the changes we've made to source positions, etc.
            VK_HITCH_SCOPE("LL_audio");
            gAudiop->idle();
        }
    }

    // Handle shutdown process, for example,
    // wait for floaters to close, send quit message,
    // forcibly quit if it has taken too long
    if (mQuitRequested)
    {
        idleShutdown();
    }
}

void LLAppViewer::idleShutdown()
{
    // Wait for all modal alerts to get resolved
    if (LLModalDialog::activeCount() > 0)
    {
        return;
    }

    // close IM interface
    if(gIMMgr)
    {
        gIMMgr->disconnectAllSessions();
    }

    // Wait for all floaters to get resolved
    if (gFloaterView)
    {
        if (!mClosingFloaters)
        {
            // application is quitting
            gFloaterView->closeAllChildren(true);
            mClosingFloaters = true;
            return;
        }
        if (!gFloaterView->allChildrenClosed())
        {
            return;
        }
    }

    // ProductEngine: Try moving this code to where we shut down sTextureCache in cleanup()
    // *TODO: ugly
    static bool saved_teleport_history = false;
    if (!saved_teleport_history)
    {
        saved_teleport_history = true;
        LLTeleportHistory::getInstance()->dump();
        LLLocationHistory::getInstance()->save(); // *TODO: find a better place for doing this
        return;
    }

    static bool saved_snapshot = false;
    if (!saved_snapshot)
    {
        saved_snapshot = true;
        saveFinalSnapshot();
        return;
    }

    const F32 SHUTDOWN_UPLOAD_SAVE_TIME = 5.f;

    S32 pending_uploads = gAssetStorage->getNumPendingUploads();
    if (pending_uploads > 0
        && gLogoutTimer.getElapsedTimeF32() < SHUTDOWN_UPLOAD_SAVE_TIME
        && !logoutRequestSent())
    {
        static S32 total_uploads = 0;
        // Sometimes total upload count can change during logout.
        total_uploads = llmax(total_uploads, pending_uploads);
        gViewerWindow->setShowProgress(true);
        S32 finished_uploads = total_uploads - pending_uploads;
        F32 percent = 100.f * finished_uploads / total_uploads;
        gViewerWindow->setProgressPercent(percent);
        gViewerWindow->setProgressString(LLTrans::getString("SavingSettings"));
        return;
    }

    if (gPendingMetricsUploads > 0
        && gLogoutTimer.getElapsedTimeF32() < SHUTDOWN_UPLOAD_SAVE_TIME
        && !logoutRequestSent())
    {
        gViewerWindow->setShowProgress(true);
        gViewerWindow->setProgressPercent(100.f);
        gViewerWindow->setProgressString(LLTrans::getString("LoggingOut"));
        return;
    }

    // All floaters are closed.  Tell server we want to quit.
    if (!logoutRequestSent())
    {
        sendLogoutRequest();

        // Wait for a LogoutReply message
        gViewerWindow->setShowProgress(true);
        gViewerWindow->setProgressPercent(100.f);
        gViewerWindow->setProgressString(LLTrans::getString("LoggingOut"));
        return;
    }

    // Make sure that we quit if we haven't received a reply from the server.
    if (logoutRequestSent()
        && gLogoutTimer.getElapsedTimeF32() > gLogoutMaxTime)
    {
        forceQuit();
        return;
    }
}

void LLAppViewer::sendLogoutRequest()
{
    if(!mLogoutRequestSent && gMessageSystem)
    {
        if (SLBridge::instanceExists())
            SLBridge::instance().detachOnLogout(); // <GMDX> take the helper off while still connected
        //Set internal status variables and marker files before actually starting the logout process
        gLogoutInProgress = true;
        if (!mSecondInstance)
        {
            mLogoutMarkerFileName = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, LOGOUT_MARKER_FILE_NAME);
            try
            {
                if (!mLogoutMarkerFile.getFileHandle())
                {
                    mLogoutMarkerFile.open(mLogoutMarkerFileName, LL_APR_WB);
                    if (mLogoutMarkerFile.getFileHandle())
                    {
                        LL_INFOS("MarkerFile") << "Created logout marker file '" << mLogoutMarkerFileName << "' " << LL_ENDL;
                        recordMarkerVersion(mLogoutMarkerFile);
                    }
                    else
                    {
                        LL_WARNS("MarkerFile") << "Cannot create logout marker file " << mLogoutMarkerFileName << LL_ENDL;
                    }
                }
                else
                {
                    LL_WARNS("MarkerFile") << "Atempted to reopen file '" << mLogoutMarkerFileName << "' " << LL_ENDL;
                }
            }
            catch (...)
            {
                LL_WARNS("MarkerFile") << "Cannot create logout marker file " << mLogoutMarkerFileName << LL_ENDL;
            }
        }
        else
        {
            LL_INFOS("MarkerFile") << "Did not logout marker file because this is a second instance" << LL_ENDL;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_LogoutRequest);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID() );
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gAgent.sendReliableMessage();

        LL_INFOS("Agent") << "Logging out as agent: " << gAgent.getID() << " Session: " << gAgent.getSessionID() << LL_ENDL;

        gLogoutTimer.reset();
        gLogoutMaxTime = LOGOUT_REQUEST_TIME;
        mLogoutRequestSent = true;

        LLVoiceClient::setVoiceEnabled(false);
    }
}

void LLAppViewer::sendSimpleLogoutRequest()
{
    if (!mLogoutRequestSent && gMessageSystem)
    {
        gLogoutInProgress = true;

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_LogoutRequest);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gAgent.sendReliableMessage();

        LL_INFOS("Agent") << "Logging out as agent: " << gAgent.getID() << " Session: " << gAgent.getSessionID() << LL_ENDL;

        gLogoutTimer.reset();
        gLogoutMaxTime = LOGOUT_REQUEST_TIME;
        mLogoutRequestSent = true;
    }
}

void LLAppViewer::updateNameLookupUrl(const LLViewerRegion * regionp)
{
    if (!regionp || !regionp->capabilitiesReceived())
    {
        return;
    }

    LLAvatarNameCache *name_cache = LLAvatarNameCache::getInstance();
    bool had_capability = LLAvatarNameCache::getInstance()->hasNameLookupURL();
    std::string name_lookup_url;
    name_lookup_url.reserve(128); // avoid a memory allocation below
    name_lookup_url = regionp->getCapability("GetDisplayNames");
    bool have_capability = !name_lookup_url.empty();
    if (have_capability)
    {
        // we have support for display names, use it
        auto url_size = name_lookup_url.size();
        // capabilities require URLs with slashes before query params:
        // https://<host>:<port>/cap/<uuid>/?ids=<blah>
        // but the caps are granted like:
        // https://<host>:<port>/cap/<uuid>
        if (url_size > 0 && name_lookup_url[url_size - 1] != '/')
        {
            name_lookup_url += '/';
        }
        name_cache->setNameLookupURL(name_lookup_url);
    }
    else
    {
        // Display names not available on this region
        name_cache->setNameLookupURL(std::string());
    }

    // Error recovery - did we change state?
    if (had_capability != have_capability)
    {
        // name tags are persistant on screen, so make sure they refresh
        LLVOAvatar::invalidateNameTags();
    }
}

void LLAppViewer::postToMainCoro(const LL::WorkQueue::Work& work)
{
    gMainloopWork.post(work);
}

void LLAppViewer::createErrorMarker(eLastExecEvent error_code) const
{
    if (!mSecondInstance)
    {
        std::string error_marker = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ERROR_MARKER_FILE_NAME);

        LLAPRFile file;
        file.open(error_marker, LL_APR_WB);
        if (file.getFileHandle())
        {
            recordMarkerVersion(file);
            std::string data = "\n" + std::to_string((S32)error_code);
            file.write(data.data(), static_cast<S32>(data.length()));
            file.close();
        }
    }
}

void LLAppViewer::outOfMemorySoftQuit()
{
    if (!mQuitRequested)
    {
        // Todo:
        // Find a way to free at least some memory to make it safer
        // Pause decoding and mesh repositorie
        getTextureCache()->pause();
        getTextureFetch()->pause();
        LLLFSThread::sLocal->pause();
        gLogoutTimer.reset();
        mQuitRequested = true;

        LLError::LLUserWarningMsg::showOutOfMemory();
    }
}

void LLAppViewer::idleNameCache()
{
    // Neither old nor new name cache can function before agent has a region
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    // deal with any queued name requests and replies.
    gCacheName->processPending();

    // Can't run the new cache until we have the list of capabilities
    // for the agent region, and can therefore decide whether to use
    // display names or fall back to the old name system.
    if (!region->capabilitiesReceived())
    {
        return;
    }

    LLAvatarNameCache::getInstance()->idle();
}

//
// Handle messages, and all message related stuff
//


constexpr F32 CHECK_MESSAGES_DEFAULT_MAX_TIME = 0.002f; // 2 ms per frame for messages
constexpr F32 CHECK_MESSAGES_CEILING_MAX_TIME = 0.005f; // backlog growth stops here
static F32 CheckMessagesMaxTime = CHECK_MESSAGES_DEFAULT_MAX_TIME;

static LLTrace::BlockTimerStatHandle FTM_IDLE_NETWORK("Idle Network");
static LLTrace::BlockTimerStatHandle FTM_MESSAGE_ACKS("Message Acks");
static LLTrace::BlockTimerStatHandle FTM_RETRANSMIT("Retransmit");
static LLTrace::BlockTimerStatHandle FTM_TIMEOUT_CHECK("Timeout Check");
static LLTrace::BlockTimerStatHandle FTM_DYNAMIC_THROTTLE("Dynamic Throttle");
static LLTrace::BlockTimerStatHandle FTM_CHECK_REGION_CIRCUIT("Check Region Circuit");

void LLAppViewer::idleNetwork()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    pingMainloopTimeout("idleNetwork");

    gObjectList.mNumNewObjects = 0;
    S32 total_decoded = 0;

    static LLCachedControl<bool> speed_test(gSavedSettings, "SpeedTest", false);
    if (!speed_test())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_NETWORK("idle network"); //LL_RECORD_BLOCK_TIME(FTM_IDLE_NETWORK); // decode

        LLTimer check_message_timer;
        //  Read all available packets from network
        const S64 frame_count = gFrameCount;  // U32->S64
        F32 total_time = 0.0f;

        {
            bool needs_drain = false;
            LockMessageChecker lmc(gMessageSystem);
            while (lmc.checkAllMessages(frame_count, gServicePump))
            {
                if (gDoDisconnect)
                {
                    // We're disconnecting, don't process any more messages from the server
                    // We're usually disconnecting due to either network corruption or a
                    // server going down, so this is OK.
                    break;
                }

                total_decoded++;

                if (total_decoded > MESSAGE_MAX_PER_FRAME)
                {
                    needs_drain = true;
                    break;
                }

                // Prevent slow packets from completely destroying the frame rate.
                // This usually happens due to clumps of avatars taking huge amount
                // of network processing time (which needs to be fixed, but this is
                // a good limit anyway).
                total_time = check_message_timer.getElapsedTimeF32();
                if (total_time >= CheckMessagesMaxTime)
                {
                    needs_drain = true;
                    break;
                }
            }
            if (needs_drain || gMessageSystem->mPacketRing.getNumBufferedPackets() > 0)
            {
                // Rather than allow packets to silently backup on the socket
                // we drain them into our own buffer so we know how many exist.
                S32 num_buffered_packets = gMessageSystem->drainUdpSocket();
                if (num_buffered_packets > 0)
                {
                    // Increase CheckMessagesMaxTime so that we will eventually catch up
                    CheckMessagesMaxTime = llmin(CheckMessagesMaxTime * 1.035f, CHECK_MESSAGES_CEILING_MAX_TIME); // 3.5% ~= 2x in 20 frames
                }
            }
            else
            {
                // Reset CheckMessagesMaxTime to default value
                CheckMessagesMaxTime = CHECK_MESSAGES_DEFAULT_MAX_TIME;
            }

            // Handle per-frame message system processing.

            static LLCachedControl<F32> ack_collection_time(gSavedSettings, "AckCollectTime", 0.1f);
            lmc.processAcks(ack_collection_time());
        }
    }
    add(LLStatViewer::NUM_NEW_OBJECTS, gObjectList.mNumNewObjects);

    // Retransmit unacknowledged packets. processAcks() above already does this every frame
    // unless the speed test skipped it.
    if (speed_test())
    {
        gXferManager->retransmitUnackedPackets();
        gAssetStorage->checkForTimeouts();
    }
    gViewerThrottle.setBufferLoadRate(gMessageSystem->getBufferLoadRate());
    gViewerThrottle.updateDynamicThrottle();

    // Check that the circuit between the viewer and the agent's current
    // region is still alive
    LLViewerRegion *agent_region = gAgent.getRegion();
    if (agent_region && (LLStartUp::getStartupState()==STATE_STARTED))
    {
        LLUUID this_region_id = agent_region->getRegionID();
        bool this_region_alive = agent_region->isAlive();
        if ((mAgentRegionLastAlive && !this_region_alive) // newly dead
            && (mAgentRegionLastID == this_region_id)) // same region
        {
            forceDisconnect(LLTrans::getString("AgentLostConnection"));
        }
        mAgentRegionLastID = this_region_id;
        mAgentRegionLastAlive = this_region_alive;
    }
}

void LLAppViewer::disconnectViewer()
{
    // Clear the Discord activity and close its connection (#205). Before the
    // gDisconnected early-out: a simulator-side disconnect sets that flag first.
    SLDiscordPresence::shutdown();

    // Item copies still queued by the copy pacer (#538) are logged and dropped
    // here, never sent. Before the gDisconnected early-out: a simulator-side
    // disconnect sets that flag first.
    LLInventoryCopyPacer::shutdown();
    // A delete in parts (Empty Trash: too many subcategories) stops here; what
    // was left undeleted is logged (ChunkedPurge).
    LLChunkedPurge::shutdown();

    if (gDisconnected)
    {
        return;
    }
    //
    // Cleanup after quitting.
    //
    // Save snapshot for next time, if we made it through initialization

    LL_INFOS() << "Disconnecting viewer!" << LL_ENDL;

    // Dump our frame statistics

    // Remember if we were flying
    gSavedSettings.setBOOL("FlyingAtExit", gAgent.getFlying() );

    // Un-minimize all windows so they don't get saved minimized
    if (gFloaterView)
    {
        gFloaterView->restoreAll();
    }

    // <FS:PP> Restore open IMs from previous session: remember them while the sessions still exist
    if (LLStartUp::getStartupState() >= STATE_STARTED)
    {
        LLIMMgr::saveOpenIMs();
    }
    // </FS:PP>

    // <FS:Ansariel> Firestorm radar: Shutdown radar
    if (FSRadar::instanceExists())
    {
        FSRadar::deleteSingleton();
    }
    // </FS:Ansariel>

    if (LLSelectMgr::instanceExists())
    {
        LLSelectMgr::getInstance()->deselectAll();
    }

    // save inventory if appropriate
    if (gInventory.isInventoryUsable()
        && gAgent.getID().notNull()) // Shouldn't be null at this stage
    {
        gInventory.cache(gInventory.getRootFolderID(), gAgent.getID());
        if (gInventory.getLibraryRootFolderID().notNull()
            && gInventory.getLibraryOwnerID().notNull()
            && !mSecondInstance) // agent is unique, library isn't
        {
            gInventory.cache(
                gInventory.getLibraryRootFolderID(),
                gInventory.getLibraryOwnerID());
        }
    }

    saveNameCache();
    if (LLExperienceCache::instanceExists())
    {
        // TODO: LLExperienceCache::cleanup() logic should be moved to
        // cleanupSingleton().
        LLExperienceCache::instance().cleanup();
    }

    // close inventory interface, close all windows
    LLSidepanelInventory::cleanup();

// [SL:KB] - Patch: Appearance-Misc | Checked: 2013-02-12 (Catznip-3.4)
    // Destroying all objects below will trigger attachment detaching code and attempt to remove the COF links for them
    LLAppearanceMgr::instance().setAttachmentInvLinkEnable(false);
// [/SL:KB]

// [RLVa:KB] - Checked: RLVa-2.3 (Housekeeping)
    SUBSYSTEM_CLEANUP(RlvHandler);
// [/RLVa:KB]

    gAgentWearables.cleanup();
    gAgentCamera.cleanup();
    // Also writes cached agent settings to gSavedSettings
    gAgent.cleanup();

    // This is where we used to call gObjectList.destroy() and then delete gWorldp.
    // Now we just ask the LLWorld singleton to cleanly shut down.
    if(LLWorld::instanceExists())
    {
        LLWorld::getInstance()->resetClass();
    }
    LLVOCache::deleteSingleton();

    // call all self-registered classes
    LLDestroyClassList::instance().fireCallbacks();

    cleanup_xfer_manager();
    gDisconnected = true;

    // Pass the connection state to LLUrlEntryParcel not to attempt
    // parcel info requests while disconnected.
    LLUrlEntryParcel::setDisconnected(gDisconnected);
}

void LLAppViewer::forceErrorLLError()
{
    LL_ERRS() << "This is a deliberate llerror" << LL_ENDL;
}

void LLAppViewer::forceErrorLLErrorMsg()
{
    LLError::LLUserWarningMsg::show("Deliberate error");
    // Note: under debug this will show a message as well,
    // but release won't show anything and will quit silently
    LL_ERRS() << "This is a deliberate llerror with a message" << LL_ENDL;
}

void LLAppViewer::forceErrorBreakpoint()
{
    LL_WARNS() << "Forcing a deliberate breakpoint" << LL_ENDL;
#ifdef LL_WINDOWS
    DebugBreak();
#else
#if defined(LL_X86) || defined(LL_X86_64)
    asm ("int $3");
#else
    __builtin_trap();
#endif
#endif
    return;
}

void LLAppViewer::forceErrorBadMemoryAccess()
{
    LL_WARNS() << "Forcing a deliberate bad memory access" << LL_ENDL;
    S32* crash = NULL;
    *crash = 0xDEADBEEF;
    return;
}

void LLAppViewer::forceErrorInfiniteLoop()
{
    LL_WARNS() << "Forcing a deliberate infinite loop" << LL_ENDL;
    // Loop is intentionally complicated to fool basic loop detection
    LLTimer timer_total;
    LLTimer timer_expiry;
    const S32 report_frequency = 10;
    timer_expiry.setTimerExpirySec(report_frequency);
    while(true)
    {
        if (timer_expiry.hasExpired())
        {
            LL_INFOS() << "Infinite loop time : " << timer_total.getElapsedSeconds() << LL_ENDL;
            timer_expiry.setTimerExpirySec(report_frequency);
        }
    }
    return;
}

void LLAppViewer::forceErrorSoftwareException()
{
    LL_WARNS() << "Forcing a deliberate exception" << LL_ENDL;
    LLTHROW(LLException("User selected Force Software Exception"));
}

void LLAppViewer::forceErrorOSSpecificException()
{
    // Virtual, MacOS only
    const std::string exception_text = "User selected Force OS Exception, Not implemented on this OS";
    throw std::runtime_error(exception_text);
}

void LLAppViewer::forceErrorCoroutineCrash()
{
    LL_WARNS() << "Forcing a crash in LLCoros" << LL_ENDL;
    LLCoros::instance().launch("LLAppViewer::crashyCoro", [] {throw LLException("A deliberate crash from LLCoros"); });
}

void LLAppViewer::forceErrorCoroprocedureCrash()
{
    LL_WARNS() << "Forcing a crash in LLCoprocedureManager" << LL_ENDL;
    LLCoprocedureManager::instance().enqueueCoprocedure("Upload", "DeliberateCrash",
        [](LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t&, const LLUUID&)
    {
        LL_WARNS() << "Forcing a deliberate bad memory access from LLCoprocedureManager" << LL_ENDL;
        S32* crash = NULL;
        *crash = 0xDEADBEEF;
    });
}

void LLAppViewer::forceErrorWorkQueueCrash()
{
    LL::WorkQueue::ptr_t workqueue = LL::WorkQueue::getInstance("General");
    if (workqueue)
    {
        workqueue->post([]() { throw LLException("This is a deliberate crash from General Queue"); });
    }
}

void LLAppViewer::forceErrorThreadCrash()
{
    class LLCrashTestThread : public LLThread
    {
    public:

        LLCrashTestThread() : LLThread("Crash logging test thread")
        {
        }

        void run()
        {
            LL_ERRS() << "This is a deliberate llerror in thread" << LL_ENDL;
        }
    };

    LL_WARNS() << "This is a deliberate crash in a thread" << LL_ENDL;
    LLCrashTestThread *thread = new LLCrashTestThread();
    thread->start();
}

void LLAppViewer::forceExceptionThreadCrash()
{
    class LLCrashTestThread : public LLThread
    {
    public:

        LLCrashTestThread() : LLThread("Crash logging test thread")
        {
        }

        void run()
        {
            const std::string exception_text = "This is a deliberate exception in a thread";
            throw std::runtime_error(exception_text);
        }
    };

    LL_WARNS() << "This is a deliberate exception in a thread" << LL_ENDL;
    LLCrashTestThread* thread = new LLCrashTestThread();
    thread->start();
}

void LLAppViewer::initMainloopTimeout(std::string_view state, F32 secs)
{
    if (!mMainloopTimeout)
    {
        mMainloopTimeout = new LLWatchdogTimeout();
        resumeMainloopTimeout(state, secs);
    }
}

void LLAppViewer::destroyMainloopTimeout()
{
    if (mMainloopTimeout)
    {
        delete mMainloopTimeout;
        mMainloopTimeout = nullptr;
    }
}

void LLAppViewer::resumeMainloopTimeout(std::string_view state, F32 secs)
{
    if (mMainloopTimeout)
    {
        if (secs < 0.0f)
        {
            static LLCachedControl<F32> mainloop_timeout(gSavedSettings, "MainloopTimeoutDefault", 60.f);
            secs = mainloop_timeout;
        }

        mMainloopTimeout->setTimeout(secs);
        mMainloopTimeout->start(state);
    }
}

void LLAppViewer::pauseMainloopTimeout()
{
    if (mMainloopTimeout)
    {
        mMainloopTimeout->stop();
    }
}

void LLAppViewer::pingMainloopTimeout(std::string_view state, F32 secs)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_APP;

    if (mMainloopTimeout)
    {
        if (secs < 0.0f)
        {
            static LLCachedControl<F32> mainloop_timeout(gSavedSettings, "MainloopTimeoutDefault", 60);
            secs = mainloop_timeout;
        }

        mMainloopTimeout->setTimeout(secs);
        mMainloopTimeout->ping(state);
    }
}

void LLAppViewer::handleLoginComplete()
{
    gLoggedInTime.start();
    initMainloopTimeout("Mainloop Init");

    // Store some data to DebugInfo in case of a freeze.
    gDebugInfo["ClientInfo"]["Name"] = LLVersionInfo::instance().getChannel();

    gDebugInfo["ClientInfo"]["MajorVersion"] = LLVersionInfo::instance().getMajor();
    gDebugInfo["ClientInfo"]["MinorVersion"] = LLVersionInfo::instance().getMinor();
    gDebugInfo["ClientInfo"]["PatchVersion"] = LLVersionInfo::instance().getPatch();
    gDebugInfo["ClientInfo"]["BuildVersion"] = std::to_string(LLVersionInfo::instance().getBuild());

    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if ( parcel && parcel->getMusicURL()[0])
    {
        gDebugInfo["ParcelMusicURL"] = parcel->getMusicURL();
    }
    if ( parcel && parcel->getMediaURL()[0])
    {
        gDebugInfo["ParcelMediaURL"] = parcel->getMediaURL();
    }

    gDebugInfo["SettingsFilename"] = gSavedSettings.getString("ClientSettingsFile");
    gDebugInfo["CAFilename"] = gDirUtilp->getCAFile();
    gDebugInfo["ViewerExePath"] = gDirUtilp->getExecutablePathAndName();
    gDebugInfo["CurrentPath"] = gDirUtilp->getCurPath();

    if(gAgent.getRegion())
    {
        gDebugInfo["CurrentSimHost"] = gAgent.getRegion()->getSimHostName();
        gDebugInfo["CurrentRegion"] = gAgent.getRegion()->getName();
    }

    if(LLAppViewer::instance()->mMainloopTimeout)
    {
        gDebugInfo["MainloopTimeoutState"] = LLAppViewer::instance()->mMainloopTimeout->getState();
    }

    if (gAgentAvatarp)
    {
        gWindowTitle.append(" - ").append(gAgentAvatarp->getFullname());
        gViewerWindow->getWindow()->setTitle(gWindowTitle);
    }

    mOnLoginCompleted();
    mOnLoginCompleted.disconnect_all_slots(); // No longer needed

    writeDebugInfo();

    // we logged in successfully, so save settings on logout
    LL_INFOS() << "Login successful, per account settings will be saved on log out." << LL_ENDL;
    mSavePerAccountSettings=true;
}

//virtual
void LLAppViewer::setMasterSystemAudioMute(bool mute)
{
    gSavedSettings.setBOOL("MuteAudio", mute);
}

//virtual
bool LLAppViewer::getMasterSystemAudioMute()
{
    return gSavedSettings.getBOOL("MuteAudio");
}

//----------------------------------------------------------------------------
// Metrics-related methods (static and otherwise)
//----------------------------------------------------------------------------

/**
 * LLViewerAssetStats collects data on a per-region (as defined by the agent's
 * location) so we need to tell it about region changes which become a kind of
 * hidden variable/global state in the collectors.  For collectors not running
 * on the main thread, we need to send a message to move the data over safely
 * and cheaply (amortized over a run).
 */
void LLAppViewer::metricsUpdateRegion(U64 region_handle)
{
    if (0 != region_handle)
    {
        LLViewerAssetStatsFF::set_region(region_handle);
    }
}

/**
 * Attempts to start a multi-threaded metrics report to be sent back to
 * the grid for consumption.
 */
void LLAppViewer::metricsSend(bool enable_reporting)
{
    if (! gViewerAssetStats)
        return;

    if (LLAppViewer::sTextureFetch)
    {
        LLViewerRegion * regionp = gAgent.getRegion();

        if (enable_reporting && regionp)
        {
            std::string caps_url = regionp->getCapability("ViewerMetrics");

            LLSD sd = gViewerAssetStats->asLLSD(true);

            // Send a report request into 'thread1' to get the rest of the data
            // and provide some additional parameters while here.
            LLAppViewer::sTextureFetch->commandSendMetrics(caps_url,
                                                           gAgentSessionID,
                                                           gAgentID,
                                                           sd);
        }
        else
        {
            LLAppViewer::sTextureFetch->commandDataBreak();
        }
    }

    // Reset even if we can't report.  Rather than gather up a huge chunk of
    // data, we'll keep to our sampling interval and retain the data
    // resolution in time.
    gViewerAssetStats->restart();
}

