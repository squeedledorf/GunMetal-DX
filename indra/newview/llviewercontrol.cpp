/**
 * @file llviewercontrol.cpp
 * @brief Viewer configuration
 * @author Richard Nelson
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

#include "vkgfx/llvulkancontext.h"   // VSync -> Vulkan present mode
#include "llviewercontrol.h"
#include "llviewerchat.h"

// Library includes
#include "llwindow.h"   // getGamma()

// For Listeners
#include "llaudioengine.h"
#include "llsoundcache.h"
#include "llgputexcache.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llconsole.h"
#include "llflexibleobject.h"
#include "llfeaturemanager.h"

#include "llsky.h"
#include "llvieweraudio.h"
#include "llviewermenu.h"
#include "llviewertexturelist.h"
#include "llviewerthrottle.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvoiceclient.h"
#include "llvotree.h"
#include "llvovolume.h"
#include "llworld.h"
#include "llvlcomposition.h"
#include "pipeline.h"
#include "llviewerjoystick.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llparcel.h"
#include "llkeyboard.h"
#include "llerrorcontrol.h"
#include "llappviewer.h"
#include "llvosurfacepatch.h"
#include "llrender.h"
#include "llnavigationbar.h"
#include "llnotificationsutil.h"
#include "llfloatertools.h"
#include "llpaneloutfitsinventory.h"
#include "llpanellogin.h"
#include "llpaneltopinfobar.h"
#include "llspellcheck.h"
#include "llslurl.h"
#include "llstartup.h"
#include "llperfstats.h"
#include "llvkautotune.h"   // #422
// [RLVa:KB] - Checked: 2015-12-27 (RLVa-1.5.0)
#include "llvisualeffect.h"
#include "rlvactions.h"
#include "rlvcommon.h"
// [/RLVa:KB]


// Third party library includes
#include <boost/algorithm/string.hpp>

#ifdef TOGGLE_HACKED_GODLIKE_VIEWER
bool                gHackGodmode = false;
#endif

// Should you contemplate changing the name "Global", please first grep for
// that string literal. There are at least a couple other places in the C++
// code that assume the LLControlGroup named "Global" is gSavedSettings.
LLControlGroup gSavedSettings("Global");    // saved at end of session
LLControlGroup gSavedPerAccountSettings("PerAccount"); // saved at end of session
LLControlGroup gCrashSettings("CrashSettings"); // saved at end of session
LLControlGroup gWarningSettings("Warnings"); // persists ignored dialogs/warnings
LLControlGroup gSkinSettings("SkinUI"); // [AL:CR] read only settings tied to skin selection

std::string gLastRunVersion;

extern bool gResizeScreenTexture;
extern bool gResizeShadowTexture;
extern bool gDebugGL;

namespace
{
    std::string renderSettingText(const LLSD& v)
    {
        std::string text = v.isBoolean() ? (v.asBoolean() ? "1" : "0")
                         : v.isReal()    ? llformat("%g", v.asReal())
                                         : v.asString();
        if (text.size() > 40) text = text.substr(0, 40) + "...";
        return text;
    }

    struct NonDefaultRenderCollector : public LLControlGroup::ApplyFunctor
    {
        std::vector<std::string> items;
        void apply(const std::string& name, LLControlVariable* control) override
        {
            if (name.compare(0, 8, "RenderVK") != 0) return;
            const std::string now = renderSettingText(control->getValue());
            if (now == renderSettingText(control->getDefault())) return;
            items.push_back(name + "=" + now);
        }
    };
}

std::string nonDefaultRenderSettings(size_t max_items)
{
    NonDefaultRenderCollector collector;
    gSavedSettings.applyToAll(&collector);
    std::sort(collector.items.begin(), collector.items.end());
    std::string out;
    const size_t shown = std::min(max_items, collector.items.size());
    for (size_t i = 0; i < shown; ++i)
    {
        if (i) out += " ";
        out += collector.items[i];
    }
    if (collector.items.size() > shown)
    {
        out += " +" + std::to_string(collector.items.size() - shown) + " more";
    }
    return out;
}
////////////////////////////////////////////////////////////////////////////
// Listeners

static bool handleRenderAvatarMouselookChanged(const LLSD& newvalue)
{
    LLVOAvatar::sVisibleInFirstPerson = newvalue.asBoolean();
    return true;
}

static bool handleRenderFarClipChanged(const LLSD& newvalue)
{
    if (LLStartUp::getStartupState() >= STATE_STARTED)
    {
        // The VRAM ladder's far-geometry rung (#405) may hold the EFFECTIVE distance
        // below the setting; the setting keeps the user's value.
        F32 draw_distance = LLVulkanContext::instance().vramDrawDistance((F32)newvalue.asReal());
    gAgentCamera.mDrawDistance = draw_distance;
    LLWorld::getInstance()->setLandFarClip(draw_distance);
    return true;
    }
    return false;
}

static bool handleDebugAvatarJointsChanged(const LLSD& newvalue)
{
    std::string new_string = newvalue.asString();
    LLJoint::setDebugJointNames(new_string);
    return true;
}

static bool handleAvatarHoverOffsetChanged(const LLSD& newvalue)
{
    if (isAgentAvatarValid())
    {
        gAgentAvatarp->setHoverIfRegionEnabled();
    }
    return true;
}


static bool handleRenderPerfTestChanged(const LLSD& newvalue)
{
       bool status = !newvalue.asBoolean();
       if (!status)
       {
               gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_WL_SKY,
                                                                         LLPipeline::RENDER_TYPE_TERRAIN,
                                                                         LLPipeline::RENDER_TYPE_GRASS,
                                                                         LLPipeline::RENDER_TYPE_TREE,
                                                                         LLPipeline::RENDER_TYPE_WATER,
                                                                         LLPipeline::RENDER_TYPE_PASS_GRASS,
                                                                         LLPipeline::RENDER_TYPE_HUD,
                                                                         LLPipeline::RENDER_TYPE_CLOUDS,
                                                                         LLPipeline::RENDER_TYPE_HUD_PARTICLES,
                                                                         LLPipeline::END_RENDER_TYPES);
               gPipeline.setRenderDebugFeatureControl(LLPipeline::RENDER_DEBUG_FEATURE_UI, false);
       }
       else
       {
               gPipeline.setRenderTypeMask(LLPipeline::RENDER_TYPE_WL_SKY,
                                                                         LLPipeline::RENDER_TYPE_TERRAIN,
                                                                         LLPipeline::RENDER_TYPE_GRASS,
                                                                         LLPipeline::RENDER_TYPE_TREE,
                                                                         LLPipeline::RENDER_TYPE_WATER,
                                                                         LLPipeline::RENDER_TYPE_PASS_GRASS,
                                                                         LLPipeline::RENDER_TYPE_HUD,
                                                                         LLPipeline::RENDER_TYPE_CLOUDS,
                                                                         LLPipeline::RENDER_TYPE_HUD_PARTICLES,
                                                                         LLPipeline::END_RENDER_TYPES);
               gPipeline.setRenderDebugFeatureControl(LLPipeline::RENDER_DEBUG_FEATURE_UI, true);
       }

       return true;
}

bool handleRenderTransparentWaterChanged(const LLSD& newvalue)
{
    // #521: the cached flag (read by the VK renderer) was only set at startup, so a live
    // change (Preferences, a graphics preset) did nothing until a restart.
    LLPipeline::updateRenderTransparentWater();
    LLWorld::getInstance()->updateWaterObjects();
    return true;
}


static bool handleDisableVintageMode(const LLSD& newvalue)
{
    gSavedSettings.setBOOL("RenderEnableEmissiveBuffer", newvalue.asBoolean());
    gSavedSettings.setBOOL("RenderHDREnabled", newvalue.asBoolean());
    return true;
}

// #410: the Advanced Graphics floater also exposes these two (like Advanced > Rendering); LLPipeline
// caches them in statics that only the menu handler refreshed, so keep them in sync on any change
// (floater, graphics preset load, debug settings).
static bool handleAttachedLightsChanged(const LLSD& newvalue)
{
    LLPipeline::sRenderAttachedLights = newvalue.asBoolean();
    return true;
}

static bool handleAttachedParticlesChanged(const LLSD& newvalue)
{
    LLPipeline::sRenderAttachedParticles = newvalue.asBoolean();
    return true;
}

static bool handleVSyncChanged(const LLSD& newvalue)
{
    LLVulkanContext::instance().setVSync(newvalue.asBoolean());
    // (#422: TargetFPS is no longer clamped to the refresh rate here -- with the refresh
    // rate unknown (0) on SDL that wrote TargetFPS = 0. The auto-tuner caps its EFFECTIVE
    // target at the refresh rate while vsync is on; the setting keeps the user's value.)
    return true;
}

// RenderVKNoVsyncPresentMode: the vsync-off present mode order (diagnostic); recreates
// the swapchain next frame like the vsync toggle.
static bool handleNoVsyncPresentModeChanged(const LLSD& newvalue)
{
    LLVulkanContext::instance().setNoVsyncPresentMode((uint32_t)newvalue.asInteger());
    return true;
}

// #919 FullScreen (the debug settings editor, a preferences restore): enter / leave live.
// LLViewerWindow::setFullscreen writes the setting itself only when it differs from the
// window, so the toggle's own write lands here with the window already matching (no-op).
static bool handleFullScreenChanged(const LLSD& newvalue)
{
    if (gViewerWindow && gViewerWindow->getWindow() && gViewerWindow->getFullscreen() != newvalue.asBoolean())
    {
        gViewerWindow->setFullscreen(newvalue.asBoolean());
    }
    return true;
}

// #919 FullScreenMode: re-applied live while fullscreen (A/B borderless vs exclusive).
static bool handleFullScreenModeChanged(const LLSD& newvalue)
{
    if (gViewerWindow && gViewerWindow->getFullscreen())
    {
        gViewerWindow->setFullscreen(true);
    }
    return true;
}

static bool validateAnisotropicFiltering(const LLSD& val)
{
    S32 filter_level = val.asInteger();
    return filter_level == 0 || filter_level == 2 || filter_level == 4 || filter_level == 8 || filter_level == 16;
}

// EFFECTIVE detail values under the VRAM soft ladder (#405 steps 4 and 6) and the auto-tuner's
// "Keep my FPS" caps (#422, merged in the renderer): the runtime statics (LOD factors) and
// the agent camera's draw distance are the user's settings times the renderer's current
// multipliers. Settings are never written (Preferences keep showing
// the user's values); the multipliers return to 1 as the ladder recovers.
static void applyVolumeLOD(F32 setting)
{
    const F32 eff = llclamp(setting, 0.01f, MAX_LOD_FACTOR) * LLVulkanContext::instance().vramMeshLodScale();
    LLVOVolume::sLODFactor = llclamp(eff, 0.01f, MAX_LOD_FACTOR);
    LLVOVolume::sDistanceFactor = 1.f-LLVOVolume::sLODFactor * 0.1f;
}

static void applyAvatarLOD(F32 setting)
{
    LLVOAvatar::sLODFactor = llclamp(setting, 0.f, MAX_AVATAR_LOD_FACTOR) * LLVulkanContext::instance().vramAvatarLodScale();
}

static void applyTerrainLOD(F32 setting)
{
    LLVOSurfacePatch::sLODFactor = setting * LLVulkanContext::instance().vramTerrainLodScale();
    //sqaure lod factor to get exponential range of [0,4] and keep
    //a value of 1 in the middle of the detail slider for consistency
    //with other detail sliders (see panel_preferences_graphics1.xml)
    LLVOSurfacePatch::sLODFactor *= LLVOSurfacePatch::sLODFactor;
}

void applyVramDetailCaps()
{
    static F32 s_last[4] = {1.f, 1.f, 1.f, 1.f};
    const LLVulkanContext& vk = LLVulkanContext::instance();
    const F32 far_setting = gSavedSettings.getF32("RenderFarClip");
    const F32 cur[4] = {vk.vramMeshLodScale(), vk.vramAvatarLodScale(), vk.vramTerrainLodScale(),
                        far_setting > 0.f ? vk.vramDrawDistance(far_setting) / far_setting : 1.f};
    if (cur[0] == s_last[0] && cur[1] == s_last[1] && cur[2] == s_last[2] && cur[3] == s_last[3]) return;
    if (cur[0] != s_last[0]) applyVolumeLOD(gSavedSettings.getF32("RenderVolumeLODFactor"));
    if (cur[1] != s_last[1]) applyAvatarLOD(gSavedSettings.getF32("RenderAvatarLODFactor"));
    if (cur[2] != s_last[2]) applyTerrainLOD(gSavedSettings.getF32("RenderTerrainLODFactor"));
    if (cur[3] != s_last[3] && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        const F32 draw_distance = vk.vramDrawDistance(far_setting);
        gAgentCamera.mDrawDistance = draw_distance;
        LLWorld::getInstance()->setLandFarClip(draw_distance);
    }
    else if (cur[3] != s_last[3])
    {
        return;   // before STATE_STARTED: retried next idle (the camera picks it up at start)
    }
    LL_INFOS("VKMem") << "VRAM ladder / auto-tune (#422) effective detail: mesh LOD factor " << LLVOVolume::sLODFactor << " (set "
                      << gSavedSettings.getF32("RenderVolumeLODFactor") << "), avatar LOD " << LLVOAvatar::sLODFactor
                      << ", terrain LOD x" << cur[2] << ", draw distance " << gAgentCamera.mDrawDistance << " m (set "
                      << far_setting << " m)" << LL_ENDL;
    for (int i = 0; i < 4; ++i) s_last[i] = cur[i];
}

static bool handleVolumeLODChanged(const LLSD& newvalue)
{
    applyVolumeLOD((F32)newvalue.asReal());
    return true;
}

static bool handleAvatarLODChanged(const LLSD& newvalue)
{
    applyAvatarLOD((F32)newvalue.asReal());
    return true;
}

static bool handleAvatarPhysicsLODChanged(const LLSD& newvalue)
{
    LLVOAvatar::sPhysicsLODFactor = llclamp((F32) newvalue.asReal(), 0.f, MAX_AVATAR_LOD_FACTOR);
    return true;
}

static bool handleTerrainLODChanged(const LLSD& newvalue)
{
    applyTerrainLOD((F32)newvalue.asReal());
    return true;
}

static bool handleTreeLODChanged(const LLSD& newvalue)
{
    LLVOTree::sTreeFactor = (F32) newvalue.asReal();
    return true;
}

static bool handleFlexLODChanged(const LLSD& newvalue)
{
    LLVolumeImplFlexible::sUpdateFactor = (F32) newvalue.asReal();
    return true;
}

static bool handleGammaChanged(const LLSD& newvalue)
{
    F32 gamma = (F32) newvalue.asReal();
    if (gamma == 0.0f)
    {
        gamma = 1.0f; // restore normal gamma
    }
    if (gViewerWindow && gViewerWindow->getWindow() && gamma != gViewerWindow->getWindow()->getGamma())
    {
        // Only save it if it's changed
        if (!gViewerWindow->getWindow()->setGamma(gamma))
        {
            LL_WARNS() << "setGamma failed!" << LL_ENDL;
        }
    }

    return true;
}

const F32 MAX_USER_FOG_RATIO = 10.f;
const F32 MIN_USER_FOG_RATIO = 0.5f;

static bool handleFogRatioChanged(const LLSD& newvalue)
{
    F32 fog_ratio = llmax(MIN_USER_FOG_RATIO, llmin((F32) newvalue.asReal(), MAX_USER_FOG_RATIO));
    gSky.setFogRatio(fog_ratio);
    return true;
}

static bool handleMaxPartCountChanged(const LLSD& newvalue)
{
    LLViewerPartSim::setMaxPartCount(newvalue.asInteger());
    return true;
}

static bool handleChatFontSizeChanged(const LLSD& newvalue)
{
    if(gConsole)
    {
        gConsole->setFontSize(newvalue.asInteger());
    }
    if (gViewerWindow && gViewerWindow->getNearbyChatConsole())
    {
        gViewerWindow->getNearbyChatConsole()->setFont(LLViewerChat::getChatFont());
    }
    return true;
}

static bool handleConsoleMaxLinesChanged(const LLSD& newvalue)
{
    if(gConsole)
    {
        gConsole->setMaxLines(newvalue.asInteger());
    }
    if (gViewerWindow && gViewerWindow->getNearbyChatConsole())
    {
        gViewerWindow->getNearbyChatConsole()->setMaxLines(newvalue.asInteger());
    }
    return true;
}

static void handleAudioVolumeChanged(const LLSD& newvalue)
{
    audio_update_volume(true);
}

static bool handleJoystickChanged(const LLSD& newvalue)
{
    LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
    return true;
}

static bool handleUseOcclusionChanged(const LLSD& newvalue)
{
    LLPipeline::sUseOcclusion = (newvalue.asBoolean()
        && LLFeatureManager::getInstance()->isFeatureAvailable("UseOcclusion") && !gUseWireframe) ? 2 : 0;
    return true;
}

static bool handleUploadBakedTexOldChanged(const LLSD& newvalue)
{
    LLPipeline::sForceOldBakedUpload = newvalue.asBoolean();
    return true;
}


static bool handleRenderDynamicLODChanged(const LLSD& newvalue)
{
    LLPipeline::sDynamicLOD = newvalue.asBoolean();
    return true;
}

static bool handleRenderResolutionDivisorChanged(const LLSD&)
{
    gResizeScreenTexture = true;
    return true;
}

static bool handleDebugViewsChanged(const LLSD& newvalue)
{
    LLView::sDebugRects = newvalue.asBoolean();
    return true;
}

static bool handleLogFileChanged(const LLSD& newvalue)
{
    std::string log_filename = newvalue.asString();
    LLFile::remove(log_filename);
    LLError::logToFile(log_filename);
    LL_INFOS() << "Logging switched to " << log_filename << LL_ENDL;
    return true;
}

bool handleHideGroupTitleChanged(const LLSD& newvalue)
{
    gAgent.setHideGroupTitle(newvalue);
    return true;
}

bool handleEffectColorChanged(const LLSD& newvalue)
{
    gAgent.setEffectColor(LLColor4(newvalue));
    return true;
}

bool handleHighResSnapshotChanged(const LLSD& newvalue)
{
    // High Res Snapshot active, must uncheck RenderUIInSnapshot
    if (newvalue.asBoolean())
    {
        gSavedSettings.setBOOL( "RenderUIInSnapshot", false);
    }
    return true;
}

bool handleVoiceClientPrefsChanged(const LLSD& newvalue)
{
    if (LLVoiceClient::instanceExists())
    {
        LLVoiceClient::getInstance()->updateSettings();
    }
    return true;
}

bool handleVelocityInterpolate(const LLSD& newvalue)
{
    LLMessageSystem* msg = gMessageSystem;
    if ( newvalue.asBoolean() )
    {
        msg->newMessageFast(_PREHASH_VelocityInterpolateOn);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gAgent.sendReliableMessage();
        LL_INFOS() << "Velocity Interpolation On" << LL_ENDL;
    }
    else
    {
        msg->newMessageFast(_PREHASH_VelocityInterpolateOff);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gAgent.sendReliableMessage();
        LL_INFOS() << "Velocity Interpolation Off" << LL_ENDL;
    }
    return true;
}

bool handleForceShowGrid(const LLSD& newvalue)
{
    LLPanelLogin::updateLocationSelectorsVisibility();
    return true;
}

bool handleLoginLocationChanged()
{
    /*
     * This connects the default preference setting to the state of the login
     * panel if it is displayed; if you open the preferences panel before
     * logging in, and change the default login location there, the login
     * panel immediately changes to match your new preference.
     */
    std::string new_login_location = gSavedSettings.getString("LoginLocation");
    LL_DEBUGS("AppInit")<<new_login_location<<LL_ENDL;
    LLStartUp::setStartSLURL(LLSLURL(new_login_location));
    return true;
}

bool handleSpellCheckChanged()
{
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
            return true;
        }
    }
    LLSpellChecker::setUseSpellCheck(LLStringUtil::null);
    return true;
}

bool toggle_agent_pause(const LLSD& newvalue)
{
    if ( newvalue.asBoolean() )
    {
        send_agent_pause();
    }
    else
    {
        send_agent_resume();
    }
    return true;
}

bool toggle_show_navigation_panel(const LLSD& newvalue)
{
    bool value = newvalue.asBoolean();

    LLNavigationBar::getInstance()->setVisible(value);
    gSavedSettings.setBOOL("ShowMiniLocationPanel", !value);
    gViewerWindow->reshapeStatusBarContainer();
    return true;
}

bool toggle_show_mini_location_panel(const LLSD& newvalue)
{
    bool value = newvalue.asBoolean();

    LLPanelTopInfoBar::getInstance()->setVisible(value);
    gSavedSettings.setBOOL("ShowNavbarNavigationPanel", !value);

    return true;
}

bool toggle_show_object_render_cost(const LLSD& newvalue)
{
    LLFloaterTools::sShowObjectCost = newvalue.asBoolean();
    return true;
}

// #422 auto-tuner (LLVKAutoTune reads its other settings live). TargetFPS: clamped to
// 10..500 (0 -- what the old refresh-rate clamp wrote -- becomes 60); never clamped to the
// refresh rate (that is the runtime effective target's job).
void handleTargetFPSChanged(const LLSD& newValue)
{
    const U32 v = (U32)llmax(0, newValue.asInteger());
    const U32 c = LLVKAutoTune::clampTargetFPS(v);
    if (c != v)
    {
        gSavedSettings.setU32("TargetFPS", c);
    }
}

// "Keep my FPS" (continuous mode: temporary caps only, never a setting write).
void handleAutoTuneLockChanged(const LLSD& newValue)
{
    LLVKAutoTune::instance().setContinuous(newValue.asBoolean());
}

void handleRenderAvatarMaxARTChanged(const LLSD& newValue)
{
    LLPerfStats::updateRenderCostLimitFromSettings();
}

void handlePerformanceStatsEnabledChanged(const LLSD& newValue)
{
    const auto newval = gSavedSettings.getBOOL("PerfStatsCaptureEnabled");
    LLPerfStats::StatsRecorder::setEnabled(newval);
}

void handleLocalTerrainChanged(const LLSD& newValue)
{
    for (U32 i = 0; i < LLTerrainMaterials::ASSET_COUNT; ++i)
    {
        const auto setting = gSavedSettings.getString(std::string("LocalTerrainAsset") + std::to_string(i + 1));
        const LLUUID materialID(setting);
        gLocalTerrainMaterials.setDetailAssetID(i, materialID);

        // *NOTE: The GLTF spec allows for different texture infos to have their texture transforms set independently, but as a simplification, this debug setting only updates all the transforms in-sync (i.e. only one texture transform per terrain material).
        LLGLTFMaterial::TextureTransform transform;
        const std::string prefix = std::string("LocalTerrainTransform") + std::to_string(i + 1);
        transform.mScale.mV[VX] = gSavedSettings.getF32(prefix + "ScaleU");
        transform.mScale.mV[VY] = gSavedSettings.getF32(prefix + "ScaleV");
        transform.mRotation = gSavedSettings.getF32(prefix + "Rotation") * DEG_TO_RAD;
        transform.mOffset.mV[VX] = gSavedSettings.getF32(prefix + "OffsetU");
        transform.mOffset.mV[VY] = gSavedSettings.getF32(prefix + "OffsetV");
        LLPointer<LLGLTFMaterial> mat_override = new LLGLTFMaterial();
        for (U32 info = 0; info < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++info)
        {
            mat_override->mTextureTransform[info] = transform;
        }
        if (*mat_override == LLGLTFMaterial::sDefault)
        {
            gLocalTerrainMaterials.setMaterialOverride(i, nullptr);
        }
        else
        {
            gLocalTerrainMaterials.setMaterialOverride(i, mat_override);
        }
        const bool paint_enabled = gSavedSettings.getBOOL("LocalTerrainPaintEnabled");
        gLocalTerrainMaterials.setPaintType(paint_enabled ? TERRAIN_PAINT_TYPE_PBR_PAINTMAP : TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE);
    }
}
////////////////////////////////////////////////////////////////////////////

LLPointer<LLControlVariable> setting_get_control(LLControlGroup& group, const std::string& setting)
{
    LLPointer<LLControlVariable> cntrl_ptr = group.getControl(setting);
    if (cntrl_ptr.isNull())
    {
        LLError::LLUserWarningMsg::showMissingFiles();
        LL_ERRS() << "Unable to set up setting listener for " << setting
            << "." << LL_ENDL;
    }
    return cntrl_ptr;
}

void setting_setup_signal_listener(LLControlGroup& group, const std::string& setting, std::function<void(const LLSD& newvalue)> callback)
{
    setting_get_control(group, setting)->getSignal()->connect([callback](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
    {
        callback(new_val);
    });
}

void setting_setup_signal_listener(LLControlGroup& group, const std::string& setting, std::function<void()> callback)
{
    setting_get_control(group, setting)->getSignal()->connect([callback](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
    {
        callback();
    });
}

void settings_setup_listeners()
{
    setting_setup_signal_listener(gSavedSettings, "FirstPersonAvatarVisible", handleRenderAvatarMouselookChanged);
    // Motion blur preset (Preferences > Advanced): writes the underlying RenderVKMotionBlur* settings.
    setting_setup_signal_listener(gSavedSettings, "SLMotionBlurPreset", [](const LLSD& newvalue)
    {
        const S32 preset = newvalue.asInteger();
        gSavedSettings.setBOOL("RenderVKMotionBlur", preset > 0);
        if (preset <= 0)
            return;
        const bool cinematic = preset >= 2;
        gSavedSettings.setF32("RenderVKMotionBlurShutter", cinematic ? 1.f / 30.f : 1.f / 60.f);
        gSavedSettings.setF32("RenderVKMotionBlurCameraTranslation", cinematic ? 1.f : 0.5f);
        gSavedSettings.setF32("RenderVKMotionBlurCameraRotation", 0.f);
        gSavedSettings.setF32("RenderVKMotionBlurObject", 1.f);
        gSavedSettings.setF32("RenderVKMotionBlurMaxRadius", cinematic ? 40.f : 24.f);
    });
    setting_setup_signal_listener(gSavedSettings, "RenderFarClip", handleRenderFarClipChanged);
    setting_setup_signal_listener(gSavedSettings, "FSFontSettingsFile", [] { if (gViewerWindow) gViewerWindow->requestFontSetUpdate(); });
    setting_setup_signal_listener(gSavedSettings, "FSFontSizeAdjustment", [] { if (gViewerWindow) gViewerWindow->requestFontSetUpdate(); });
    // Sound cache budget (#176) applies live; evicts least recently played sounds if now over it.
    setting_setup_signal_listener(gSavedSettings, "SlipstreamSoundCacheSizeMB", [](const LLSD& newvalue)
    {
        const U32 budget_mb = llclamp((U32)newvalue.asInteger(), LLSoundCache::MIN_BUDGET_MB, LLSoundCache::MAX_BUDGET_MB);
        LLSoundCache::setBudget(static_cast<U64>(budget_mb) * 1024 * 1024);
    });
    // GPU-ready texture cache (#274): budget and on/off apply live.
    setting_setup_signal_listener(gSavedSettings, "SlipstreamGpuTexCacheSizeMB", [](const LLSD& newvalue)
    {
        const U32 budget_mb = llclamp((U32)newvalue.asInteger(), LLGpuTexCache::MIN_BUDGET_MB, LLGpuTexCache::MAX_BUDGET_MB);
        LLGpuTexCache::setBudget(static_cast<U64>(budget_mb) * 1024 * 1024);
    });
    setting_setup_signal_listener(gSavedSettings, "SlipstreamGpuTexCache", [](const LLSD& newvalue)
    {
        LLGpuTexCache::setEnabled(newvalue.asBoolean());
    });
    gSavedSettings.getControl("RenderAnisotropicLevel")->getValidateSignal()->connect(boost::bind(&validateAnisotropicFiltering, _2));
    setting_setup_signal_listener(gSavedSettings, "RenderDisableVintageMode", handleDisableVintageMode);
    setting_setup_signal_listener(gSavedSettings, "RenderVolumeLODFactor", handleVolumeLODChanged);
    // (RenderAvatarComplexityMode: "simplify avatars beyond" is re-evaluated by LLVKAutoTune live, #422.)
    setting_setup_signal_listener(gSavedSettings, "RenderAvatarLODFactor", handleAvatarLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderAvatarPhysicsLODFactor", handleAvatarPhysicsLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderTerrainLODFactor", handleTerrainLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderTreeLODFactor", handleTreeLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderFlexTimeFactor", handleFlexLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderGamma", handleGammaChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderFogRatio", handleFogRatioChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderMaxPartCount", handleMaxPartCountChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderDynamicLOD", handleRenderDynamicLODChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderAttachedLights", handleAttachedLightsChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderAttachedParticles", handleAttachedParticlesChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderVSyncEnable", handleVSyncChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderVKNoVsyncPresentMode", handleNoVsyncPresentModeChanged);
    setting_setup_signal_listener(gSavedSettings, "FullScreen", handleFullScreenChanged);           // #919
    setting_setup_signal_listener(gSavedSettings, "FullScreenMode", handleFullScreenModeChanged);   // #919
    setting_setup_signal_listener(gSavedSettings, "RenderResolutionDivisor", handleRenderResolutionDivisorChanged);
// [SL:KB] - Patch: Settings-RenderResolutionMultiplier | Checked: Catznip-5.4
    setting_setup_signal_listener(gSavedSettings, "RenderResolutionMultiplier", handleRenderResolutionDivisorChanged);
// [/SL:KB]
    setting_setup_signal_listener(gSavedSettings, "RenderPerformanceTest", handleRenderPerfTestChanged);
    setting_setup_signal_listener(gSavedSettings, "ChatFontSize", handleChatFontSizeChanged);
    setting_setup_signal_listener(gSavedSettings, "ConsoleMaxLines", handleConsoleMaxLinesChanged);
    setting_setup_signal_listener(gSavedSettings, "UploadBakedTexOld", handleUploadBakedTexOldChanged);
    setting_setup_signal_listener(gSavedSettings, "UseOcclusion", handleUseOcclusionChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelMaster", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelSFX", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelUI", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelAmbient", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelMusic", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelMedia", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelVoice", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteAudio", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteMusic", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteMedia", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteVoice", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteAmbient", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "MuteUI", handleAudioVolumeChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "JoystickAxis6", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisScale6", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "FlycamAxisDeadZone6", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisScale5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "AvatarAxisDeadZone5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisScale5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone0", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone1", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone2", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone3", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone4", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "BuildAxisDeadZone5", handleJoystickChanged);
    setting_setup_signal_listener(gSavedSettings, "DebugViews", handleDebugViewsChanged);
    setting_setup_signal_listener(gSavedSettings, "UserLogFile", handleLogFileChanged);
    setting_setup_signal_listener(gSavedSettings, "RenderHideGroupTitle", handleHideGroupTitleChanged);
    setting_setup_signal_listener(gSavedSettings, "HighResSnapshot", handleHighResSnapshotChanged);
    setting_setup_signal_listener(gSavedSettings, "EnableVoiceChat", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "PTTCurrentlyEnabled", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "PushToTalkButton", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "PushToTalkToggle", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceEarLocation", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceEchoCancellation", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceAutomaticGainControl", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceNoiseSuppressionLevel", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceInputAudioDevice", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VoiceOutputAudioDevice", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "AudioLevelMic", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "LipSyncEnabled", handleVoiceClientPrefsChanged);
    setting_setup_signal_listener(gSavedSettings, "VelocityInterpolate", handleVelocityInterpolate);
    setting_setup_signal_listener(gSavedSettings, "QAMode", show_debug_menus);
    setting_setup_signal_listener(gSavedSettings, "UseDebugMenus", show_debug_menus);
    setting_setup_signal_listener(gSavedSettings, "AgentPause", toggle_agent_pause);
    setting_setup_signal_listener(gSavedSettings, "ShowNavbarNavigationPanel", toggle_show_navigation_panel);
    setting_setup_signal_listener(gSavedSettings, "ShowMiniLocationPanel", toggle_show_mini_location_panel);
    setting_setup_signal_listener(gSavedSettings, "ShowObjectRenderingCost", toggle_show_object_render_cost);
    setting_setup_signal_listener(gSavedSettings, "ForceShowGrid", handleForceShowGrid);
    setting_setup_signal_listener(gSavedSettings, "RenderTransparentWater", handleRenderTransparentWaterChanged);
    setting_setup_signal_listener(gSavedSettings, "SpellCheck", handleSpellCheckChanged);
    setting_setup_signal_listener(gSavedSettings, "SpellCheckDictionary", handleSpellCheckChanged);
    setting_setup_signal_listener(gSavedSettings, "LoginLocation", handleLoginLocationChanged);
    setting_setup_signal_listener(gSavedSettings, "DebugAvatarJoints", handleDebugAvatarJointsChanged);

    LLVKAutoTune::validateSettings();   // #422: repair TargetFPS (0 from the old refresh-rate bug), session-only AutoTuneLock
    setting_setup_signal_listener(gSavedSettings, "TargetFPS", handleTargetFPSChanged);
    setting_setup_signal_listener(gSavedSettings, "AutoTuneLock", handleAutoTuneLockChanged);
    // #422: a Tune once / Undo write (or any non-Preferences write) takes effect at once; only
    // the Preferences slider applied it before. "Keep my FPS" / "simplify avatars beyond"
    // re-apply their effective cap on top within 0.25 s (LLVKAutoTune::applyLLSideCaps).
    setting_setup_signal_listener(gSavedSettings, "RenderAvatarMaxNonImpostors",
                                  [](const LLSD& v) { LLVOAvatar::updateImpostorRendering((U32)llmax(0, v.asInteger())); });
    setting_setup_signal_listener(gSavedSettings, "RenderAvatarMaxART", handleRenderAvatarMaxARTChanged);
    setting_setup_signal_listener(gSavedSettings, "PerfStatsCaptureEnabled", handlePerformanceStatsEnabledChanged);
    {
        setting_setup_signal_listener(gSavedSettings, "LocalTerrainPaintEnabled", handleLocalTerrainChanged);
        const char* transform_suffixes[] = {
            "ScaleU",
            "ScaleV",
            "Rotation",
            "OffsetU",
            "OffsetV"
        };
        for (U32 i = 0; i < LLTerrainMaterials::ASSET_COUNT; ++i)
        {
            const auto asset_setting_name = std::string("LocalTerrainAsset") + std::to_string(i + 1);
            setting_setup_signal_listener(gSavedSettings, asset_setting_name, handleLocalTerrainChanged);
            for (const char* ts : transform_suffixes)
            {
                const auto transform_setting_name = std::string("LocalTerrainTransform") + std::to_string(i + 1) + ts;
                setting_setup_signal_listener(gSavedSettings, transform_setting_name, handleLocalTerrainChanged);
            }
        }
    }

    setting_setup_signal_listener(gSavedPerAccountSettings, "AvatarHoverOffsetZ", handleAvatarHoverOffsetChanged);
// [RLVa:KB] - Checked: 2015-12-27 (RLVa-1.5.0)
    gSavedSettings.getControl(RlvSettingNames::Main)->getSignal()->connect(boost::bind(&RlvSettings::onChangedSettingMain, _2));
// [/RLVa:KB]
}

#if TEST_CACHED_CONTROL

#define DECL_LLCC(T, V) static LLCachedControl<T> mySetting_##T("TestCachedControl"#T, V)
DECL_LLCC(U32, (U32)666);
DECL_LLCC(S32, (S32)-666);
DECL_LLCC(F32, (F32)-666.666);
DECL_LLCC(bool, true);
DECL_LLCC(bool, false);
static LLCachedControl<std::string> mySetting_string("TestCachedControlstring", "Default String Value");
DECL_LLCC(LLVector3, LLVector3(1.0f, 2.0f, 3.0f));
DECL_LLCC(LLVector3d, LLVector3d(6.0f, 5.0f, 4.0f));
DECL_LLCC(LLRect, LLRect(0, 0, 100, 500));
DECL_LLCC(LLColor4, LLColor4(0.0f, 0.5f, 1.0f));
DECL_LLCC(LLColor3, LLColor3(1.0f, 0.f, 0.5f));
DECL_LLCC(LLColor4U, LLColor4U(255, 200, 100, 255));

LLSD test_llsd = LLSD()["testing1"] = LLSD()["testing2"];
DECL_LLCC(LLSD, test_llsd);

void test_cached_control()
{
#define do { TEST_LLCC(T, V) if((T)mySetting_##T != V) LL_ERRS() << "Fail "#T << LL_ENDL; } while(0)
    TEST_LLCC(U32, 666);
    TEST_LLCC(S32, (S32)-666);
    TEST_LLCC(F32, (F32)-666.666);
    TEST_LLCC(bool, true);
    TEST_LLCC(bool, false);
    if((std::string)mySetting_string != "Default String Value") LL_ERRS() << "Fail string" << LL_ENDL;
    TEST_LLCC(LLVector3, LLVector3(1.0f, 2.0f, 3.0f));
    TEST_LLCC(LLVector3d, LLVector3d(6.0f, 5.0f, 4.0f));
    TEST_LLCC(LLRect, LLRect(0, 0, 100, 500));
    TEST_LLCC(LLColor4, LLColor4(0.0f, 0.5f, 1.0f));
    TEST_LLCC(LLColor3, LLColor3(1.0f, 0.f, 0.5f));
    TEST_LLCC(LLColor4U, LLColor4U(255, 200, 100, 255));
//There's no LLSD comparsion for LLCC yet. TEST_LLCC(LLSD, test_llsd);
}
#endif // TEST_CACHED_CONTROL

