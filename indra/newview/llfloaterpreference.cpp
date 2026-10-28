/**
 * @file llfloaterpreference.cpp
 * @brief Global preferences with and without persistence.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

/*
 * App-wide preferences.  Note that these are not per-user,
 * because we need to load many preferences before we have
 * a login name.
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterpreference.h"
#include "fsdroptarget.h"
#include "fscombathitmarker.h" // <GMDX>
#include "llinventorymodel.h"
#include "llviewerinventory.h"

#include "message.h"
#include "llfloaterautoreplacesettings.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llcommandhandler.h"
#include "lldiriterator.h"
#include "lldirpicker.h"
#include "llsoundcache.h"
#include "llgputexcache.h"
#include "lleventtimer.h"
#include "llfeaturemanager.h"
#include "llfilepicker.h"
#include "llfocusmgr.h"
//#include "llfirstuse.h"
#include "llfloaterreg.h"
#include "llfloaterabout.h"
#include "llfavoritesbar.h"
#include "llfloaterpreferencesgraphicsadvanced.h"
#include "llfloaterperformance.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloaterimsession.h"
#include "llgamecontrol.h"
#include "llkeyboard.h"
#include "llmodaldialog.h"
#include "llnavigationbar.h"
#include "llfloaterimnearbychat.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llnotificationtemplate.h"
#include "llpanellogin.h"
#include "llpanelpreferencegamecontrol.h"
#include "llpanelvoicedevicesettings.h"
#include "llradiogroup.h"
#include "llsearchcombobox.h"
#include "llsky.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsliderctrl.h"
#include "lltabcontainer.h"
#include "lltrans.h"
#include "llxmlnode.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llviewereventrecorder.h"
#include "llviewermenufile.h"
#include "llviewermessage.h"
#include "llviewerwindow.h"
#include "llviewerthrottle.h"
#include "llviewertexture.h"   // tier-1 texture copy stats (#405)
#include "lltexturedata.h"     // #707 lease reasons
#include "llvoavatarself.h"
#include "llvotree.h"
#include "llfloaterpathfindingconsole.h"
// linden library includes
#include "llavatarnamecache.h"
#include "llerror.h"
#include "llfontgl.h"
#include "llrect.h"
#include "llstring.h"
#include "slunzip.h"

// project includes

#include "llbutton.h"
#include "llflexibleobject.h"
#include "lllineeditor.h"
#include "llresmgr.h"
#include "llspinctrl.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "llui.h"
#include "llversioninfo.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"
#include "llwindow.h"
#include "llworld.h"
#include "lluictrlfactory.h"
#include "llviewermedia.h"
#include "llpluginclassmedia.h"
#include "llteleporthistorystorage.h"
#include "llproxy.h"
#include "llweb.h"
// [RLVa:KB] - Checked: 2010-03-18 (RLVa-1.2.0a)
#include "rlvactions.h"
// [/RLVa:KB]

#include "lllogininstance.h"        // to check if logged in yet
#include "llsdserialize.h"
#include "llpresetsmanager.h"
#include "llviewercontrol.h"
#include "llpresetsmanager.h"
#include "llinventoryfunctions.h"

#include <boost/json.hpp>
#include <utility>

#include "llsearchableui.h"
#include "llperfstats.h"
#include "slupdatechecker.h"
#include "slfxpresets.h"
#include "slkeycommands.h"

const F32 BANDWIDTH_UPDATER_TIMEOUT = 0.5f;
char const* const VISIBILITY_DEFAULT = "default";
char const* const VISIBILITY_HIDDEN = "hidden";

/// This must equal the maximum value set for the IndirectMaxComplexity slider in panel_preferences_graphics1.xml
static const U32 INDIRECT_MAX_ARC_OFF = 101; // all the way to the right == disabled
static const U32 MIN_INDIRECT_ARC_LIMIT = 1; // must match minimum of IndirectMaxComplexity in panel_preferences_graphics1.xml
static const U32 MAX_INDIRECT_ARC_LIMIT = INDIRECT_MAX_ARC_OFF-1; // one short of all the way to the right...

/// These are the effective range of values for RenderAvatarMaxComplexity
static const F32 MIN_ARC_LIMIT =  20000.0f;
static const F32 MAX_ARC_LIMIT = 350000.0f;
static const F32 MIN_ARC_LOG = log(MIN_ARC_LIMIT);
static const F32 MAX_ARC_LOG = log(MAX_ARC_LIMIT);
static const F32 ARC_LIMIT_MAP_SCALE = (MAX_ARC_LOG - MIN_ARC_LOG) / (MAX_INDIRECT_ARC_LIMIT - MIN_INDIRECT_ARC_LIMIT);

struct LabelDef : public LLInitParam::Block<LabelDef>
{
    Mandatory<std::string> name;
    Mandatory<std::string> value;

    LabelDef()
        : name("name"),
        value("value")
    {}
};

struct LabelTable : public LLInitParam::Block<LabelTable>
{
    Multiple<LabelDef> labels;
    LabelTable()
        : labels("label")
    {}
};

const std::string DEFAULT_SKIN = "default";

typedef enum e_skin_type
{
    SYSTEM_SKIN,
    USER_SKIN
} ESkinType;

typedef struct skin_t
{
    std::string mName = "Unknown";
    std::string mAuthor = "Unknown";
    std::string mUrl = "Unknown";
    LLDate mDate = LLDate(0.0);
    std::string mCompatVer = "Unknown";
    std::string mNotes = LLStringUtil::null;
    ESkinType mType = USER_SKIN;

} skin_t;

// global functions

// helper functions for getting/freeing the web browser media
// if creating/destroying these is too slow, we'll need to create
// a static member and update all our static callbacks

void handleNameTagOptionChanged(const LLSD& newvalue);
void handleDisplayNamesOptionChanged(const LLSD& newvalue);
bool callback_clear_browser_cache(const LLSD& notification, const LLSD& response);
bool callback_clear_cache(const LLSD& notification, const LLSD& response);

//bool callback_skip_dialogs(const LLSD& notification, const LLSD& response, LLFloaterPreference* floater);
//bool callback_reset_dialogs(const LLSD& notification, const LLSD& response, LLFloaterPreference* floater);

void fractionFromDecimal(F32 decimal_val, S32& numerator, S32& denominator);

bool callback_clear_cache(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if ( option == 0 ) // YES
    {
        // flag client texture cache for clearing next time the client runs
        gSavedSettings.setBOOL("PurgeCacheOnNextStartup", true);
        LLNotificationsUtil::add("CacheWillClear");
    }

    return false;
}

bool callback_clear_browser_cache(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if ( option == 0 ) // YES
    {
        // clean web
        LLViewerMedia::getInstance()->clearAllCaches();
        LLViewerMedia::getInstance()->clearAllCookies();

        // clean nav bar history
        LLNavigationBar::getInstance()->clearHistoryCache();

        // flag client texture cache for clearing next time the client runs
        gSavedSettings.setBOOL("PurgeCacheOnNextStartup", true);
        LLNotificationsUtil::add("CacheWillClear");

        LLSearchHistory::getInstance()->clearHistory();
        LLSearchHistory::getInstance()->save();
        LLSearchComboBox* search_ctrl = LLNavigationBar::getInstance()->getChild<LLSearchComboBox>("search_combo_box");
        search_ctrl->clearHistory();

        LLTeleportHistoryStorage::getInstance()->purgeItems();
        LLTeleportHistoryStorage::getInstance()->save();
    }

    return false;
}

void handleNameTagOptionChanged(const LLSD& newvalue)
{
    LLAvatarNameCache::getInstance()->setUseUsernames(gSavedSettings.getBOOL("NameTagShowUsernames"));
    LLVOAvatar::invalidateNameTags();
}

void handleDisplayNamesOptionChanged(const LLSD& newvalue)
{
    LLAvatarNameCache::getInstance()->setUseDisplayNames(newvalue.asBoolean());
    LLVOAvatar::invalidateNameTags();
}

void handleAppearanceCameraMovementChanged(const LLSD& newvalue)
{
    if(!newvalue.asBoolean() && gAgentCamera.getCameraMode() == CAMERA_MODE_CUSTOMIZE_AVATAR)
    {
        gAgentCamera.changeCameraToDefault();
        gAgentCamera.resetView();
    }
}

void fractionFromDecimal(F32 decimal_val, S32& numerator, S32& denominator)
{
    numerator = 0;
    denominator = 0;
    for (F32 test_denominator = 1.f; test_denominator < 30.f; test_denominator += 1.f)
    {
        if (fmodf((decimal_val * test_denominator) + 0.01f, 1.f) < 0.02f)
        {
            numerator = ll_round(decimal_val * test_denominator);
            denominator = ll_round(test_denominator);
            break;
        }
    }
}

// handle secondlife:///app/worldmap/{NAME}/{COORDS} URLs
// Also see LLUrlEntryKeybinding, the value of this command type
// is ability to show up to date value in chat
class LLKeybindingHandler: public LLCommandHandler
{
public:
    // requires trusted browser to trigger
    LLKeybindingHandler(): LLCommandHandler("keybinding", UNTRUSTED_CLICK_ONLY)
    {
    }

    bool handle(const LLSD& params, const LLSD& query_map,
                const std::string& grid, LLMediaCtrl* web)
    {
        if (params.size() < 1) return false;

        LLFloaterPreference* prefsfloater = dynamic_cast<LLFloaterPreference*>
            (LLFloaterReg::showInstance("preferences"));

        if (prefsfloater)
        {
            // find 'controls' panel and bring it the front
            LLTabContainer* tabcontainer = prefsfloater->getChild<LLTabContainer>("pref core");
            LLPanel* panel = prefsfloater->getChild<LLPanel>("controls");
            if (tabcontainer && panel)
            {
                tabcontainer->selectTabPanel(panel);
            }
        }

        return true;
    }
};
LLKeybindingHandler gKeybindHandler;

// TODO: move this out of llfloaterpreference.cpp
class LLGameControlBindingHandler: public LLCommandHandler
{
public:
    // requires trusted browser to trigger
    LLGameControlBindingHandler(): LLCommandHandler("gamecontroller", UNTRUSTED_CLICK_ONLY)
    {
    }

    bool handle(const LLSD& params, const LLSD& query_map,
                const std::string& grid, LLMediaCtrl* web)
    {
        if (params.size() < 1) return false;

        LLFloaterPreference* prefsfloater = dynamic_cast<LLFloaterPreference*>
            (LLFloaterReg::showInstance("preferences"));

        if (prefsfloater)
        {
            // find 'controls' panel and bring it the front
            LLTabContainer* tabcontainer = prefsfloater->getChild<LLTabContainer>("pref core");
            LLPanel* panel = prefsfloater->getChild<LLPanel>("game_control");
            if (tabcontainer && panel)
            {
                tabcontainer->selectTabPanel(panel);
            }
        }

        return true;
    }
};
LLGameControlBindingHandler gGameControlBindingHandler;


//////////////////////////////////////////////
// LLFloaterPreference

// static
std::string LLFloaterPreference::sSkin = "";

LLFloaterPreference::LLFloaterPreference(const LLSD& key)
    : LLFloater(key),
    mGotPersonalInfo(false),
    mLanguageChanged(false),
    mAvatarDataInitialized(false),
    mSearchDataDirty(true)
{
    LLConversationLog::instance().addObserver(this);

    //Build Floater is now Called from  LLFloaterReg::add("preferences", "floater_preferences.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreference>);

    static bool registered_dialog = false;
    if (!registered_dialog)
    {
        LLFloaterReg::add("keybind_dialog", "floater_select_key.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLSetKeyBindDialog>);
        registered_dialog = true;
    }

    mCommitCallbackRegistrar.add("Pref.Cancel",             boost::bind(&LLFloaterPreference::onBtnCancel, this, _2));
    mCommitCallbackRegistrar.add("Pref.OK",                 boost::bind(&LLFloaterPreference::onBtnOK, this, _2));

    mCommitCallbackRegistrar.add("Pref.ClearCache",             boost::bind(&LLFloaterPreference::onClickClearCache, this));
    mCommitCallbackRegistrar.add("Pref.WebClearCache",          boost::bind(&LLFloaterPreference::onClickBrowserClearCache, this));
    mCommitCallbackRegistrar.add("Pref.SetCache",               boost::bind(&LLFloaterPreference::onClickSetCache, this));
    mCommitCallbackRegistrar.add("Pref.ResetCache",             boost::bind(&LLFloaterPreference::onClickResetCache, this));
    mCommitCallbackRegistrar.add("Pref.SetSoundCache",          boost::bind(&LLFloaterPreference::onClickSetSoundCache, this));
    mCommitCallbackRegistrar.add("Pref.ResetSoundCache",        boost::bind(&LLFloaterPreference::onClickResetSoundCache, this));
    mCommitCallbackRegistrar.add("Pref.SetSounds",              boost::bind(&LLFloaterPreference::onClickSetSounds, this));
    mCommitCallbackRegistrar.add("Pref.ClickEnablePopup",       boost::bind(&LLFloaterPreference::onClickEnablePopup, this));
    mCommitCallbackRegistrar.add("Pref.ClickDisablePopup",      boost::bind(&LLFloaterPreference::onClickDisablePopup, this));
    mCommitCallbackRegistrar.add("Pref.LogPath",                boost::bind(&LLFloaterPreference::onClickLogPath, this));
    mCommitCallbackRegistrar.add("Pref.RenderExceptions",       boost::bind(&LLFloaterPreference::onClickRenderExceptions, this));
    mCommitCallbackRegistrar.add("Pref.AutoAdjustments",         boost::bind(&LLFloaterPreference::onClickAutoAdjustments, this));
    mCommitCallbackRegistrar.add("Pref.HardwareDefaults",       boost::bind(&LLFloaterPreference::setHardwareDefaults, this));
    mCommitCallbackRegistrar.add("Pref.AvatarImpostorsEnable",  boost::bind(&LLFloaterPreference::onAvatarImpostorsEnable, this));
    mCommitCallbackRegistrar.add("Pref.UpdateIndirectMaxNonImpostors", boost::bind(&LLFloaterPreference::updateMaxNonImpostors, this));
    mCommitCallbackRegistrar.add("Pref.UpdateIndirectMaxComplexity",    boost::bind(&LLFloaterPreference::updateMaxComplexity, this));
    mCommitCallbackRegistrar.add("Pref.RenderOptionUpdate",     boost::bind(&LLFloaterPreference::onRenderOptionEnable, this));
    mCommitCallbackRegistrar.add("Pref.WindowedMod",            boost::bind(&LLFloaterPreference::onCommitWindowedMode, this));
    mCommitCallbackRegistrar.add("Pref.UpdateSliderText",       boost::bind(&LLFloaterPreference::refreshUI,this));
    mCommitCallbackRegistrar.add("Pref.QualityPerformance",     boost::bind(&LLFloaterPreference::onChangeQuality, this, _2));
    mCommitCallbackRegistrar.add("Pref.applyUIColor",           boost::bind(&LLFloaterPreference::applyUIColor, this ,_1, _2));
    mCommitCallbackRegistrar.add("Pref.getUIColor",             boost::bind(&LLFloaterPreference::getUIColor, this ,_1, _2));
    mCommitCallbackRegistrar.add("Pref.MaturitySettings",       boost::bind(&LLFloaterPreference::onChangeMaturity, this));
    mCommitCallbackRegistrar.add("Pref.BlockList",              boost::bind(&LLFloaterPreference::onClickBlockList, this));
    mCommitCallbackRegistrar.add("Pref.Proxy",                  boost::bind(&LLFloaterPreference::onClickProxySettings, this));
    mCommitCallbackRegistrar.add("Pref.CheckForUpdatesNow",     boost::bind(&SLUpdateChecker::checkNow));
    mCommitCallbackRegistrar.add("Pref.TranslationSettings",    boost::bind(&LLFloaterPreference::onClickTranslationSettings, this));
    mCommitCallbackRegistrar.add("Pref.AutoReplace",            boost::bind(&LLFloaterPreference::onClickAutoReplace, this));
    mCommitCallbackRegistrar.add("Pref.PermsDefault",           boost::bind(&LLFloaterPreference::onClickPermsDefault, this));
    mCommitCallbackRegistrar.add("Pref.RememberedUsernames",    boost::bind(&LLFloaterPreference::onClickRememberedUsernames, this));
    mCommitCallbackRegistrar.add("Pref.SpellChecker",           boost::bind(&LLFloaterPreference::onClickSpellChecker, this));
    mCommitCallbackRegistrar.add("Pref.Advanced",               boost::bind(&LLFloaterPreference::onClickAdvanced, this));

    sSkin = gSavedSettings.getString("SkinCurrent");

    mCommitCallbackRegistrar.add("Pref.ClickActionChange",      boost::bind(&LLFloaterPreference::onClickActionChange, this));

    gSavedSettings.getControl("NameTagShowUsernames")->getCommitSignal()->connect(boost::bind(&handleNameTagOptionChanged,  _2));
    gSavedSettings.getControl("NameTagShowFriends")->getCommitSignal()->connect(boost::bind(&handleNameTagOptionChanged,  _2));
    gSavedSettings.getControl("UseDisplayNames")->getCommitSignal()->connect(boost::bind(&handleDisplayNamesOptionChanged,  _2));

    gSavedSettings.getControl("AppearanceCameraMovement")->getCommitSignal()->connect(boost::bind(&handleAppearanceCameraMovementChanged,  _2));
    gSavedSettings.getControl("WindLightUseAtmosShaders")->getCommitSignal()->connect(boost::bind(&LLFloaterPreference::onAtmosShaderChange, this));

    LLAvatarPropertiesProcessor::getInstance()->addObserver( gAgent.getID(), this );

    mComplexityChangedSignal = gSavedSettings.getControl("RenderAvatarMaxComplexity")->getCommitSignal()->connect(boost::bind(&LLFloaterPreference::updateComplexityText, this));
    mImpostorsChangedSignal = gSavedSettings.getControl("RenderAvatarMaxNonImpostors")->getSignal()->connect(boost::bind(&LLFloaterPreference::updateIndirectMaxNonImpostors, this, _2));

    mCommitCallbackRegistrar.add("Pref.ClearLog",               boost::bind(&LLConversationLog::onClearLog, &LLConversationLog::instance()));
    mCommitCallbackRegistrar.add("Pref.DeleteTranscripts",      boost::bind(&LLFloaterPreference::onDeleteTranscripts, this));
    mCommitCallbackRegistrar.add("UpdateFilter", boost::bind(&LLFloaterPreference::onUpdateFilterTerm, this, false)); // <FS:ND/> Hook up for filtering
    mCommitCallbackRegistrar.add("Pref.AddSkin", boost::bind(&LLFloaterPreference::onAddSkin, this));
    mCommitCallbackRegistrar.add("Pref.AddFont", boost::bind(&LLFloaterPreference::onAddFont, this));
    mCommitCallbackRegistrar.add("Pref.RemoveSkin", boost::bind(&LLFloaterPreference::onRemoveSkin, this));
    mCommitCallbackRegistrar.add("Pref.ApplySkin", boost::bind(&LLFloaterPreference::onApplySkin, this));
    mCommitCallbackRegistrar.add("Pref.SelectSkin", boost::bind(&LLFloaterPreference::onSelectSkin, this, _2));

    // #1076 Safety tab: turning the VR frame-rate protection off warns about motion sickness
    // (from the checkbox itself: a Cancel that reverts the control does not warn again).
    mCommitCallbackRegistrar.add("Pref.VRSafetyGovernor", [](LLUICtrl* ctrl, const LLSD&)
        {
            if (ctrl && !ctrl->getValue().asBoolean()) LLNotificationsUtil::add("VRSafetyGovernorOff");
        });

    mCommitCallbackRegistrar.add("Pref.ResetControlDefault", [](LLUICtrl* ctrl, const LLSD& userdata)
        {
            const std::string& control_name = userdata.asString();
            LLControlVariable* controlp = gSavedSettings.getControl(control_name);
            if (controlp)
            {
                controlp->resetToDefault(true);
            }
        });

    // <GMDX> Graphics > Sky "Reset sky settings to defaults": every setting bound to a control on
    // the button's ancestor panel named by the parameter goes back to its default (Cancel undoes it).
    mCommitCallbackRegistrar.add("Pref.ResetTabControls", [](LLUICtrl* ctrl, const LLSD& userdata)
        {
            LLView* tab = ctrl;
            while (tab && tab->getName() != userdata.asString()) tab = tab->getParent();
            if (!tab) return;
            for (auto it = tab->beginTreeDFS(); it != tab->endTreeDFS(); ++it)
            {
                LLUICtrl* c = dynamic_cast<LLUICtrl*>(*it);
                if (LLControlVariable* var = c ? c->getControlVariable() : nullptr) var->resetToDefault(true);
            }
        });
    // </GMDX>

    // <GMDX> Combat > Hit Marker: preview the hit and kill sounds
    mCommitCallbackRegistrar.add("Pref.TestHitSound", [](LLUICtrl*, const LLSD&) { FSCombatHitMarker::playHitSoundPreview(); });
    mCommitCallbackRegistrar.add("Pref.TestKillSound", [](LLUICtrl*, const LLSD&) { FSCombatHitMarker::playKillSoundPreview(); });
    // </GMDX>
}

void LLFloaterPreference::processProperties( void* pData, EAvatarProcessorType type )
{
    if ( APT_PROPERTIES_LEGACY == type )
    {
        const LLAvatarLegacyData* pAvatarData = static_cast<const LLAvatarLegacyData*>( pData );
        if (pAvatarData && (gAgent.getID() == pAvatarData->avatar_id) && (pAvatarData->avatar_id != LLUUID::null))
        {
            mAllowPublish = (bool)(pAvatarData->flags & AVATAR_ALLOW_PUBLISH);
            mAvatarDataInitialized = true;
            getChild<LLUICtrl>("online_searchresults")->setValue(mAllowPublish);
        }
    }
}

void LLFloaterPreference::saveAvatarProperties( void )
{
    const bool allowPublish = getChild<LLUICtrl>("online_searchresults")->getValue();

    if ((LLStartUp::getStartupState() == STATE_STARTED)
        && mAvatarDataInitialized
        && (allowPublish != mAllowPublish))
    {
        std::string cap_url = gAgent.getRegionCapability("AgentProfile");
        if (!cap_url.empty())
        {
            mAllowPublish = allowPublish;

            LLCoros::instance().launch("saveAvatarPropertiesCoro",
                boost::bind(saveAvatarPropertiesCoro, cap_url, allowPublish));
        }
    }
}

// static
void LLFloaterPreference::saveAvatarPropertiesCoro(const std::string cap_url, bool allow_publish)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
        httpAdapter(new LLCoreHttpUtil::HttpCoroutineAdapter("put_avatar_properties_coro", httpPolicy));
    LLCore::HttpRequest::ptr_t httpRequest(new LLCore::HttpRequest);
    LLCore::HttpHeaders::ptr_t httpHeaders;

    LLCore::HttpOptions::ptr_t httpOpts(new LLCore::HttpOptions);
    httpOpts->setFollowRedirects(true);

    std::string finalUrl = cap_url + "/" + gAgentID.asString();
    LLSD data;
    data["allow_publish"] = allow_publish;

    LLSD result = httpAdapter->putAndSuspend(httpRequest, finalUrl, data, httpOpts, httpHeaders);

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    if (!status)
    {
        LL_WARNS("Preferences") << "Failed to put agent information " << data << " for id " << gAgentID << LL_ENDL;
        return;
    }

    LL_DEBUGS("Preferences") << "Agent id: " << gAgentID << " Data: " << data << " Result: " << httpResults << LL_ENDL;
}

bool LLFloaterPreference::postBuild()
{
    LLComboBox* sort_combo = getChild<LLComboBox>("conversation_sort_order");
    std::vector<std::pair<std::string, U32>> sort_options;
    for (LLScrollListItem* item : sort_combo->getAllData())
    {
        sort_options.emplace_back(item->getColumn(0)->getValue().asString(),
                                  (U32)item->getValue().asInteger() & 0xFFFF0000);
    }
    auto update_sort_combo = [sort_combo, sort_options](LLControlVariable*, const LLSD& value, const LLSD&)
    {
        U32 participants = (U32)value.asInteger() & 0xFFFF;
        const auto items = sort_combo->getAllData();
        if (items.empty() || ((U32)items.front()->getValue().asInteger() & 0xFFFF) != participants)
        {
            sort_combo->clearRows();
            for (const auto& option : sort_options)
            {
                sort_combo->add(option.first, LLSD((S32)(option.second | participants)));
            }
        }
        sort_combo->setValue(value);
    };
    update_sort_combo(nullptr, gSavedSettings.getControl("ConversationSortOrder")->getValue(), LLSD());
    mConversationSortOrderConnection = gSavedSettings.getControl("ConversationSortOrder")->getSignal()->connect(update_sort_combo);

    mDeleteTranscriptsBtn = getChild<LLButton>("delete_transcripts");

    mEnabledPopups = getChild<LLScrollListCtrl>("enabled_popups");
    mDisabledPopups = getChild<LLScrollListCtrl>("disabled_popups");
    mEnablePopupBtn = getChild<LLButton>("enable_this_popup");
    mDisablePopupBtn = getChild<LLButton>("disable_this_popup");

    gSavedSettings.getControl("ChatFontSize")->getSignal()->connect(boost::bind(&LLFloaterIMSessionTab::processChatHistoryStyleUpdate, false));

    gSavedSettings.getControl("ChatFontSize")->getSignal()->connect(boost::bind(&LLViewerChat::signalChatFontChanged));

    // <FS> Chat style and timestamp toggles redraw open conversations
    for (const char* control : { "PlainTextChatHistory", "IMShowTime", "FSShowTimestampsNearbyChat" })
    {
        gSavedSettings.getControl(control)->getSignal()->connect(boost::bind(&LLFloaterIMSessionTab::processChatHistoryStyleUpdate, false));
    }
    // </FS>

    gSavedSettings.getControl("ChatBubbleOpacity")->getSignal()->connect(boost::bind(&LLFloaterPreference::onNameTagOpacityChange, this, _2));

    gSavedSettings.getControl("PreferredMaturity")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangeMaturity, this));

    gSavedSettings.getControl("RenderAvatarComplexityMode")->getSignal()->connect(
        [this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
        {
            onChangeComplexityMode(new_val);
        });

    gSavedPerAccountSettings.getControl("ModelUploadFolder")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangeModelFolder, this));
    gSavedPerAccountSettings.getControl("PBRUploadFolder")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangePBRFolder, this));
    gSavedPerAccountSettings.getControl("TextureUploadFolder")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangeTextureFolder, this));
    gSavedPerAccountSettings.getControl("SoundUploadFolder")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangeSoundFolder, this));
    gSavedPerAccountSettings.getControl("AnimationUploadFolder")->getSignal()->connect(boost::bind(&LLFloaterPreference::onChangeAnimationFolder, this));

    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    if (!tabcontainer->selectTab(gSavedSettings.getS32("LastPrefTab")))
        tabcontainer->selectFirstTab();

    getChild<LLUICtrl>("cache_location")->setEnabled(false); // make it read-only but selectable (STORM-227)

    // Setup > "Also offer beta versions" only chooses between feeds on a Release build;
    // Beta builds always read the beta feed (SLUpdateChecker).
    if (LLUICtrl* beta = findChild<LLUICtrl>("update_willing_to_test"))
    {
        if (!SLUpdateChecker::isReleaseBuild())
        {
            beta->setEnabled(false);
            if (LLPanel* setup = beta->getParentByType<LLPanel>())
            {
                beta->setToolTip(setup->getString("update_beta_build_tip"));
            }
        }
    }
    std::string cache_location = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "");
    setCacheLocation(cache_location);

    getChild<LLUICtrl>("sound_cache_location")->setEnabled(false); // read-only but selectable, like cache_location
    refreshSoundCacheInfo();

    getChild<LLUICtrl>("log_path_string")->setEnabled(false); // make it read-only but selectable

    getChild<LLComboBox>("language_combobox")->setCommitCallback(boost::bind(&LLFloaterPreference::onLanguageChange, this));

    // <FS:Ansariel> Item sent with autoresponses
    if (FSCopyTransInventoryDropTarget* item_target = findChild<FSCopyTransInventoryDropTarget>("autoresponse_item"))
    {
        item_target->setDADCallback(boost::bind(&LLFloaterPreference::onDADAutoresponseItem, this, _1));
    }
    if (LLButton* clear_item_btn = findChild<LLButton>("clear_autoresponse_item"))
    {
        clear_item_btn->setCommitCallback(boost::bind(&LLFloaterPreference::onClearAutoresponseItem, this));
    }
    // </FS:Ansariel>


    // if floater is opened before login set default localized do not disturb message
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        gSavedPerAccountSettings.setString("DoNotDisturbModeResponse", LLTrans::getString("DoNotDisturbModeResponseDefault"));
        gSavedPerAccountSettings.setString("SlipstreamRejectTeleportOffersResponse", LLTrans::getString("RejectTeleportOffersResponseDefault"));
        gSavedPerAccountSettings.setString("SlipstreamRejectFriendshipRequestsResponse", LLTrans::getString("RejectFriendshipRequestsResponseDefault"));
    }

    // set 'enable' property for 'Clear log...' button
    changed();

    LLLogChat::getInstance()->setSaveHistorySignal(boost::bind(&LLFloaterPreference::onLogChatHistorySaved, this));

    loadUserSkins();
    populateFontSetCombo();

    LLSliderCtrl* fov_slider = getChild<LLSliderCtrl>("camera_fov");
    fov_slider->setMinValue(LLViewerCamera::getInstance()->getMinView());
    fov_slider->setMaxValue(LLViewerCamera::getInstance()->getMaxView());

    bool enable_complexity = gSavedSettings.getS32("RenderAvatarComplexityMode") != LLVOAvatar::AV_RENDER_ONLY_SHOW_FRIENDS;
    getChild<LLSliderCtrl>("IndirectMaxComplexity")->setEnabled(enable_complexity);

    // Hook up and init for filtering
    mFilterEdit = getChild<LLSearchEditor>("search_prefs_edit");
    mFilterEdit->setKeystrokeCallback(boost::bind(&LLFloaterPreference::onUpdateFilterTerm, this, false));

    // Load and assign label for 'default language'
    std::string user_filename = gDirUtilp->getExpandedFilename(LL_PATH_DEFAULT_SKIN, "default_languages.xml");
    std::map<std::string, std::string> labels;
    if (loadFromFilename(user_filename, labels))
    {
        std::string system_lang = gSavedSettings.getString("SystemLanguage");
        std::map<std::string, std::string>::iterator iter = labels.find(system_lang);
        if (iter != labels.end())
        {
            getChild<LLComboBox>("language_combobox")->add(iter->second, LLSD("default"), ADD_TOP, true);
        }
        else
        {
            LL_WARNS() << "Language \"" << system_lang << "\" is not in default_languages.xml" << LL_ENDL;
            getChild<LLComboBox>("language_combobox")->add("System default", LLSD("default"), ADD_TOP, true);
        }
    }
    else
    {
        LL_WARNS() << "Failed to load labels from " << user_filename << ". Using default." << LL_ENDL;
        getChild<LLComboBox>("language_combobox")->add("System default", LLSD("default"), ADD_TOP, true);
    }

    return true;
}

void LLFloaterPreference::updateDeleteTranscriptsButton()
{
    mDeleteTranscriptsBtn->setEnabled(LLLogChat::transcriptFilesExist());
}

void LLFloaterPreference::onDoNotDisturbResponseChanged()
{
    // set "DoNotDisturbResponseChanged" true if user edited message differs from default, false otherwise
    bool response_changed_flag =
            LLTrans::getString("DoNotDisturbModeResponseDefault")
                    != getChild<LLUICtrl>("do_not_disturb_response")->getValue().asString();

    gSavedPerAccountSettings.setBOOL("DoNotDisturbResponseChanged", response_changed_flag );
}

void LLFloaterPreference::onRejectTeleportOffersResponseChanged()
{
    bool reject_teleport_offers_response_changed_flag =
        LLTrans::getString("RejectTeleportOffersResponseDefault")
            != getChild<LLUICtrl>("autorespond_rto_response")->getValue().asString();

    gSavedPerAccountSettings.setBOOL("SlipstreamRejectTeleportOffersResponseChanged", reject_teleport_offers_response_changed_flag);
}

void LLFloaterPreference::onRejectFriendshipRequestResponseChanged()
{
    bool reject_friendship_request_response_changed_flag =
        LLTrans::getString("RejectFriendshipRequestsResponseDefault")
            != getChild<LLUICtrl>("autorespond_reject_friends_response")->getValue().asString();

    gSavedPerAccountSettings.setBOOL("SlipstreamRejectFriendshipRequestsChanged", reject_friendship_request_response_changed_flag);
}

void LLFloaterPreference::onAdHocSelectionChanged(const LLSD& newvalue)
{
    S32 value = gSavedPerAccountSettings.getS32("SlipstreamIgnoreAdHocSessions");
    getChild<LLCheckBoxCtrl>("SlipstreamReportIgnoredAdHocSession")->setEnabled(value != 0);
}

// <FS> The "...Changed" flags record whether a response was edited, so initAutoResponses()
// only replaces untouched texts with the localized default (Firestorm's FSAutoResponseChanged).
// These used to read widgets that don't exist and write settings that don't exist.
void LLFloaterPreference::onAutoRespondResponseChanged()
{
    bool auto_response_changed_flag =
            LLTrans::getString("AutoResponseModeDefault")
                    != gSavedPerAccountSettings.getString("SlipstreamAutoresponse");

    gSavedPerAccountSettings.setBOOL("SlipstreamAutoresponseChanged", auto_response_changed_flag);
}

void LLFloaterPreference::onAutoRespondNonFriendsResponseChanged()
{
    bool auto_response_non_friends_changed_flag =
            LLTrans::getString("AutoResponseModeNonFriendsDefault")
                    != gSavedPerAccountSettings.getString("SlipstreamAutoresponseNotFriend");

    gSavedPerAccountSettings.setBOOL("SlipstreamAutoresponseNotFriendChanged", auto_response_non_friends_changed_flag);
}

// <FS:PP> FIRE-10500: Autoresponse for (Away)
void LLFloaterPreference::onAwayResponseChanged()
{
    bool away_response_changed_flag =
            LLTrans::getString("AwayAvatarResponseDefault")
                    != gSavedPerAccountSettings.getString("FSAwayAvatarResponse");

    gSavedPerAccountSettings.setBOOL("FSAwayAvatarResponseChanged", away_response_changed_flag);
}
// </FS>

////////////////////////////////////////////////////
// Skins panel

skin_t manifestFromJson(const std::string& filename, const ESkinType type)
{
    skin_t skin;
    llifstream in;
    in.open(filename);
    if (in.is_open())
    {
        boost::system::error_code ec;
        auto root = boost::json::parse(in, ec);
        if (!ec.failed() && root.is_object())
        {
            auto jobj = root.as_object();
            skin.mName = jobj.contains("name") ? boost::json::value_to<std::string>(jobj.at("name")) : "Unknown";
            skin.mAuthor = jobj.contains("author") ? boost::json::value_to<std::string>(jobj.at("author")) : LLTrans::getString("Unknown");
            skin.mUrl = jobj.contains("url") ? boost::json::value_to<std::string>(jobj.at("url")) : LLTrans::getString("Unknown");
            skin.mCompatVer = jobj.contains("compatibility") ? boost::json::value_to<std::string>(jobj.at("compatibility")) : LLTrans::getString("Unknown");
            skin.mDate = jobj.contains("date") ? LLDate(boost::json::value_to<std::string>(jobj.at("date"))) : LLDate::now();
            skin.mNotes = jobj.contains("notes") ? boost::json::value_to<std::string>(jobj.at("notes")) : "";
            // If it's a system skin, the compatability version is always the current build
            if (type == SYSTEM_SKIN)
            {
                skin.mCompatVer = LLVersionInfo::instance().getShortVersion();
            }
        }
        else
        {
            LL_WARNS() << "Failed to parse " << filename << ": " << ec.message() << LL_ENDL;
        }
        in.close();
    }
    skin.mType = type;
    return skin;
}

void LLFloaterPreference::loadUserSkins()
{
    mUserSkins.clear();
    LLDirIterator sysiter(gDirUtilp->getSkinBaseDir(), "*");
    bool found = true;
    while (found)
    {
        std::string dir;
        if ((found = sysiter.next(dir)))
        {
            const std::string& fullpath = gDirUtilp->add(gDirUtilp->getSkinBaseDir(), dir);
            if (!LLFile::isdir(fullpath)) continue; // only directories!

            const std::string& manifestpath = gDirUtilp->add(fullpath, "manifest.json");
            skin_t skin = manifestFromJson(manifestpath, SYSTEM_SKIN);

            mUserSkins.emplace(dir, skin);
        }
    }

    const std::string userskindir = gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "skins");
    if (LLFile::isdir(userskindir))
    {
        LLDirIterator iter(userskindir, "*");
        found = true;
        while (found)
        {
            std::string dir;
            if ((found = iter.next(dir)))
            {
                const std::string& fullpath = gDirUtilp->add(userskindir, dir);
                if (!LLFile::isdir(fullpath)) continue; // only directories!

                const std::string& manifestpath = gDirUtilp->add(fullpath, "manifest.json");
                skin_t skin = manifestFromJson(manifestpath, USER_SKIN);

                mUserSkins.emplace(dir, skin);
            }
        }
    }
    reloadSkinList();
}

void LLFloaterPreference::populateFontSetCombo()
{
    LLComboBox* combo = findChild<LLComboBox>("font_set_combo");
    if (!combo) return;
    combo->removeall();
    combo->add(LLTrans::getString("FontSetDefault"), LLSD("fonts.xml"));
    std::set<std::string> seen;
    auto scan = [&](const std::string& dir)
    {
        if (!LLFile::isdir(dir)) return;
        std::vector<std::pair<std::string, std::string>> found;   // (display, file)
        LLDirIterator it(dir, "fonts_*.xml");
        std::string file;
        while (it.next(file))
        {
            if (!seen.insert(file).second) continue;
            // Display name: the set's <fonts name="..."> (ours carry one), else the file
            // name as Firestorm shows it (fonts_deja_vu.xml -> "Deja vu").
            std::string name;
            LLXMLNodePtr root;
            if (LLXMLNode::parseFile(gDirUtilp->add(dir, file), root, NULL) && root.notNull() && root->hasName("fonts"))
                root->getAttributeString("name", name);
            if (name.empty() && file.size() > 10)
            {
                name = file.substr(6, file.size() - 10);
                LLStringUtil::replaceChar(name, '_', ' ');
                name[0] = LLStringOps::toUpper(name[0]);
            }
            found.emplace_back(name.empty() ? file : name, file);
        }
        std::sort(found.begin(), found.end());
        for (const auto& f : found) combo->add(f.first, LLSD(f.second));
    };
    scan(gDirUtilp->add(gDirUtilp->getAppRODataDir(), "fonts"));
    // The user's own sets (and their TTFs) go here; create it so it's easy to find.
    const std::string user_dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "fonts", "");
    if (!LLFile::isdir(user_dir)) LLFile::mkdir(user_dir);
    scan(user_dir);
    combo->setValue(LLSD(gSavedSettings.getString("FSFontSettingsFile")));
}

void LLFloaterPreference::onAddFont()
{
    LLFilePickerReplyThread::startPicker(boost::bind(&LLFloaterPreference::onAddFontCallback, this, _1),
                                         LLFilePicker::FFLOAD_FONT, true);
}

// Build a font set from one family's files: the style of each file comes from its name
// (Regular / Bold / Italic / Oblique / BoldItalic, any case or separator); styles that
// weren't picked fall back to the regular face. The shipped Roboto set is the template,
// so the result keeps the same fallbacks (DejaVu glyph coverage, emoji, OS fonts),
// sizes and Monospace -- only the SansSerif faces change.
void LLFloaterPreference::onAddFontCallback(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;
    std::string reg, bold, ital, boldital;
    for (const std::string& path : filenames)
    {
        std::string stem = gDirUtilp->getBaseFileName(path, true);
        LLStringUtil::toLower(stem);
        const bool b = stem.find("bold") != std::string::npos;
        const bool i = stem.find("italic") != std::string::npos || stem.find("oblique") != std::string::npos;
        std::string& slot = (b && i) ? boldital : b ? bold : i ? ital : reg;
        if (slot.empty()) slot = path;
    }
    if (reg.empty()) reg = !bold.empty() ? bold : !ital.empty() ? ital : boldital;
    if (bold.empty()) bold = reg;
    if (ital.empty()) ital = reg;
    if (boldital.empty()) boldital = (bold != reg) ? bold : ital;

    // Family name: the regular file's name without its style suffix ("Inter-Regular" -> "Inter").
    std::string family = gDirUtilp->getBaseFileName(reg, true);
    for (const char* suffix : {"-Regular", "_Regular", " Regular", "Regular", "-Bold", "_Bold", "-Italic", "_Italic"})
    {
        const size_t pos = family.rfind(suffix);
        if (pos != std::string::npos && pos > 0 && pos + strlen(suffix) == family.size()) { family.erase(pos); break; }
    }
    LLStringUtil::trim(family);
    if (family.empty()) family = "Custom";
    std::string id;
    for (char c : family)
        id += (isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_');
    const std::string set_file = "fonts_" + id + ".xml";

    const std::string user_dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "fonts", "");
    if (!LLFile::isdir(user_dir)) LLFile::mkdir(user_dir);

    // Template: the shipped Roboto set.
    const std::string tmpl_path = gDirUtilp->add(gDirUtilp->add(gDirUtilp->getAppRODataDir(), "fonts"), "fonts_roboto.xml");
    std::string xml;
    {
        llifstream in(tmpl_path.c_str());
        if (in.is_open()) xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (xml.empty())
    {
        LL_WARNS("Fonts") << "Add Font: template " << tmpl_path << " missing" << LL_ENDL;
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Could not add the font: the font set template is missing from the installation."));
        return;
    }

    // Copy the picked files next to the set (createFont searches user_settings/fonts).
    auto install = [&](const std::string& src) -> std::string
    {
        const std::string name = gDirUtilp->getBaseFileName(src, false);
        const std::string dst = gDirUtilp->add(user_dir, name);
        if (src != dst && LLFile::copy(src, dst) != 0 && !LLFile::isfile(dst))
            LL_WARNS("Fonts") << "Add Font: could not copy " << src << " -> " << dst << LL_ENDL;
        return name;
    };
    const std::string f_reg = install(reg), f_bold = install(bold), f_ital = install(ital), f_bi = install(boldital);

    // Longest names first so "Roboto-Bold" doesn't eat "Roboto-BoldItalic".
    LLStringUtil::replaceString(xml, "Roboto-BoldItalic.ttf", f_bi);
    LLStringUtil::replaceString(xml, "Roboto-Italic.ttf", f_ital);
    LLStringUtil::replaceString(xml, "Roboto-Bold.ttf", f_bold);
    LLStringUtil::replaceString(xml, "Roboto-Regular.ttf", f_reg);
    LLStringUtil::replaceString(xml, "<fonts name=\"Roboto\">", "<fonts name=\"" + LLStringFn::xml_encode(family, true) + "\">");
    {
        llofstream out(gDirUtilp->add(user_dir, set_file).c_str());
        if (!out.is_open())
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Could not add the font: the settings fonts folder isn't writable."));
            return;
        }
        out << xml;
    }
    LL_INFOS("Fonts") << "Add Font: " << family << " -> " << set_file << " (regular " << f_reg << ", bold " << f_bold
                      << ", italic " << f_ital << ", bold italic " << f_bi << ")" << LL_ENDL;
    populateFontSetCombo();
    gSavedSettings.setString("FSFontSettingsFile", set_file);   // applies live
    if (LLComboBox* combo = findChild<LLComboBox>("font_set_combo")) combo->setValue(LLSD(set_file));
}

void LLFloaterPreference::reloadSkinList()
{
    LLScrollListCtrl* skin_list = getChild<LLScrollListCtrl>("skin_list");
    const std::string current_skin = gSavedSettings.getString("SkinCurrent");

    skin_list->clearRows();

    // User Downloaded Skins
    for (const auto& skin : mUserSkins)
    {
        LLSD row;
        row["id"] = skin.first;
        row["columns"][0]["value"] = skin.second.mName == "Unknown" ? skin.first : skin.second.mName;
        row["columns"][0]["font"]["style"] = current_skin == skin.first ? "BOLD" : "NORMAL";
        skin_list->addElement(row);
    }
    skin_list->setSelectedByValue(current_skin, true);
    onSelectSkin(skin_list->getSelectedValue());
}

void LLFloaterPreference::onAddSkin()
{
    LLFilePickerReplyThread::startPicker(boost::bind(&LLFloaterPreference::onAddSkinCallback, this, _1), LLFilePicker::FFLOAD_ZIP, false);
}

void LLFloaterPreference::onAddSkinCallback(const std::vector<std::string>& filenames)
{
    const std::string& package = filenames[0];
    auto zip = std::make_unique<SLUnZip>(package);
    if (zip->isValid())
    {
        size_t buf_size = zip->getSizeFile("manifest.json");
        if (buf_size)
        {
            buf_size++;
            buf_size *= sizeof(char);
            auto buf = std::make_unique<char[]>(buf_size);
            zip->extractFile("manifest.json", buf.get(), buf_size);
            buf[buf_size - 1] = '\0'; // force.
            std::stringstream ss;
            ss << std::string(const_cast<const char*>(buf.get()), buf_size);
            buf.reset();

            boost::system::error_code ec;
            auto root = boost::json::parse(ss, ec);
            if (!ec.failed() && root.is_object())
            {
                const auto& jobj = root.as_object();
                const std::string& name = jobj.contains("name") ? boost::json::value_to<std::string>(jobj.at("name")) : "Unknown";
                std::string pathname = gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "skins");
                if (!gDirUtilp->fileExists(pathname))
                {
                    LLFile::mkdir(pathname);
                }
                pathname = gDirUtilp->add(pathname, name);
                if (!LLFile::isdir(pathname) && (LLFile::mkdir(pathname) != 0))
                {
                    LLNotificationsUtil::add("AddSkinUnpackFailed");
                }
                else if (!zip->extract(pathname))
                {
                    LLNotificationsUtil::add("AddSkinUnpackFailed");
                }
                else
                {
                    loadUserSkins();
                    LLNotificationsUtil::add("AddSkinSuccess", LLSD().with("PACKAGE", name));
                }
            }
            else
            {
                LLNotificationsUtil::add("AddSkinCantParseManifest", LLSD().with("PACKAGE", package));
            }
        }
        else
        {
            LLNotificationsUtil::add("AddSkinNoManifest", LLSD().with("PACKAGE", package));
        }
    }
}

void LLFloaterPreference::onRemoveSkin()
{
    LLScrollListCtrl* skin_list = findChild<LLScrollListCtrl>("skin_list");
    if (skin_list)
    {
        LLSD args;
        args["SKIN"] = skin_list->getSelectedValue().asString();
        LLNotificationsUtil::add("ConfirmRemoveSkin", args, args,
                                 boost::bind(&LLFloaterPreference::callbackRemoveSkin, this, _1, _2));
    }
}

void LLFloaterPreference::callbackRemoveSkin(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option == 0) // YES
    {
        const std::string& skin = notification["payload"]["SKIN"].asString();
        std::string dir = gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "skins");
        dir = gDirUtilp->add(dir, skin);
        if (gDirUtilp->deleteDirAndContents(dir) > 0)
        {
            skinmap_t::iterator iter = mUserSkins.find(skin);
            if (iter != mUserSkins.end())
                mUserSkins.erase(iter);
            // If we just deleted the current skin, reset to default. It might not even be a good
            // idea to allow this, but we'll see!
            if (gSavedSettings.getString("SkinCurrent") == skin)
            {
                gSavedSettings.setString("SkinCurrent", DEFAULT_SKIN);
            }
            LLNotificationsUtil::add("RemoveSkinSuccess", LLSD().with("SKIN", skin));
        }
        else
        {
            LLNotificationsUtil::add("RemoveSkinFailure", LLSD().with("SKIN", skin));
        }
        reloadSkinList();
    }
}

void LLFloaterPreference::callbackApplySkin(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    switch (option)
    {
        case 0: // Yes
            gSavedSettings.setBOOL("ResetUserColorsOnLogout", true);
            break;
        case 1: // No
            gSavedSettings.setBOOL("ResetUserColorsOnLogout", false);
            break;
        case 2: // Cancel
            gSavedSettings.setString("SkinCurrent", sSkin);
            reloadSkinList();
            break;
        default:
            LL_WARNS() << "Unhandled option! How could this be?" << LL_ENDL;
            break;
    }
}

void LLFloaterPreference::onApplySkin()
{
    LLScrollListCtrl* skin_list = findChild<LLScrollListCtrl>("skin_list");
    if (skin_list)
    {
        gSavedSettings.setString("SkinCurrent", skin_list->getSelectedValue().asString());
        reloadSkinList();
    }
    if (sSkin != gSavedSettings.getString("SkinCurrent"))
    {
        LLNotificationsUtil::add("ChangeSkin", LLSD(), LLSD(),
                                 boost::bind(&LLFloaterPreference::callbackApplySkin, this, _1, _2));
    }
}

void LLFloaterPreference::onSelectSkin(const LLSD& data)
{
    bool userskin = false;
    skinmap_t::iterator iter = mUserSkins.find(data.asString());
    if (iter != mUserSkins.end())
    {
        refreshSkinInfo(iter->second);
        userskin = (iter->second.mType == USER_SKIN);
    }
    getChild<LLUICtrl>("remove_skin")->setEnabled(userskin);
}

void LLFloaterPreference::refreshSkinInfo(const skin_t& skin)
{
    getChild<LLTextBase>("skin_name")->setText(skin.mName);
    getChild<LLTextBase>("skin_author")->setText(skin.mAuthor);
    getChild<LLTextBase>("skin_homepage")->setText(skin.mUrl);
    getChild<LLTextBase>("skin_date")->setText(skin.mDate.toHTTPDateString("%A, %d %b %Y"));
    getChild<LLTextBase>("skin_compatibility")->setText(skin.mCompatVer);
    getChild<LLTextBase>("skin_notes")->setText(skin.mNotes);
}

LLFloaterPreference::~LLFloaterPreference()
{
    LLConversationLog::instance().removeObserver(this);
    if (LLAvatarPropertiesProcessor::instanceExists())
    {
        LLAvatarPropertiesProcessor::getInstance()->removeObserver(gAgent.getID(), this);
    }
    mComplexityChangedSignal.disconnect();
    mImpostorsChangedSignal.disconnect();
    mRejectTeleportConnection.disconnect();
    mRejectFriendshipRequestsConnection.disconnect();
    mAutoResponseConnection.disconnect();
    mAutoResponseNonFriendsConnection.disconnect();
    mAwayResponseConnection.disconnect();
}

void LLFloaterPreference::draw()
{
    bool has_first_selected = (mDisabledPopups->getFirstSelected()!=NULL);
    mEnablePopupBtn->setEnabled(has_first_selected);

    has_first_selected = (mEnabledPopups->getFirstSelected()!=NULL);
    mDisablePopupBtn->setEnabled(has_first_selected);

    LLFloater::draw();
}

void LLFloaterPreference::saveSettings()
{
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreference* panel = dynamic_cast<LLPanelPreference*>(view))
        {
            panel->saveSettings();
        }
    }
    saveIgnoredNotifications();
}

void LLFloaterPreference::apply()
{
    LLAvatarPropertiesProcessor::getInstance()->addObserver(gAgent.getID(), this);

    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    if (sSkin != gSavedSettings.getString("SkinCurrent"))
    {
        sSkin = gSavedSettings.getString("SkinCurrent");
    }

    // Call apply() on all panels that derive from LLPanelPreference
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreference* panel = dynamic_cast<LLPanelPreference*>(view))
        {
            panel->apply();
        }
    }

    gViewerWindow->requestResolutionUpdate(); // for UIScaleFactor

    LLSliderCtrl* fov_slider = getChild<LLSliderCtrl>("camera_fov");
    fov_slider->setMinValue(LLViewerCamera::getInstance()->getMinView());
    fov_slider->setMaxValue(LLViewerCamera::getInstance()->getMaxView());

    std::string cache_location = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "");
    setCacheLocation(cache_location);
    refreshSoundCacheInfo();

    LLViewerMedia::getInstance()->setCookiesEnabled(getChild<LLUICtrl>("cookies_enabled")->getValue());

    if (hasChild("web_proxy_enabled", true) && hasChild("web_proxy_editor", true) && hasChild("web_proxy_port", true))
    {
        bool proxy_enable = getChild<LLUICtrl>("web_proxy_enabled")->getValue();
        std::string proxy_address = getChild<LLUICtrl>("web_proxy_editor")->getValue();
        int proxy_port = getChild<LLUICtrl>("web_proxy_port")->getValue();
        LLViewerMedia::getInstance()->setProxyConfig(proxy_enable, proxy_address, proxy_port);
    }

    if (mGotPersonalInfo)
    {
        bool new_hide_online = getChild<LLUICtrl>("online_visibility")->getValue().asBoolean();

        if (new_hide_online != mOriginalHideOnlineStatus)
        {
            // This hack is because we are representing several different
            // possible strings with a single checkbox. Since most users
            // can only select between 2 values, we represent it as a
            // checkbox. This breaks down a little bit for liaisons, but
            // works out in the end.
            if (new_hide_online != mOriginalHideOnlineStatus)
            {
                if (new_hide_online) mDirectoryVisibility = VISIBILITY_HIDDEN;
                else mDirectoryVisibility = VISIBILITY_DEFAULT;
             //Update showonline value, otherwise multiple applys won't work
                mOriginalHideOnlineStatus = new_hide_online;
            }
            gAgent.sendAgentUpdateUserInfo(mDirectoryVisibility);
        }
    }

    // Setting this up so we sync the settings with menu.
    // i.e Checking the checkox form the Preferences will also check it in the menu.
    // --Fallen
    bool autoresponse_enabled = getChild<LLCheckBoxCtrl>("SlipstreamAutoresponseEnable")->get();
    bool autoresponse_notfriends_enabled = getChild<LLCheckBoxCtrl>("SlipstreamAutoresponseNotFriendEnable")->get();

    gSavedPerAccountSettings.setBOOL("SlipstreamAutoresponseEnable", autoresponse_enabled);
    gSavedPerAccountSettings.setBOOL("SlipstreamAutoresponseNotFriendEnable", autoresponse_notfriends_enabled);

    saveAvatarProperties();
}

void LLFloaterPreference::cancel(const std::vector<std::string> settings_to_skip)
{
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    // Call cancel() on all panels that derive from LLPanelPreference
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreference* panel = dynamic_cast<LLPanelPreference*>(view))
        {
            panel->cancel(settings_to_skip);
        }
    }
    // hide joystick pref floater
    LLFloaterReg::hideInstance("pref_joystick");

    // hide translation settings floater
    LLFloaterReg::hideInstance("prefs_translation");

    // hide autoreplace settings floater
    LLFloaterReg::hideInstance("prefs_autoreplace");

    // hide spellchecker settings folder
    LLFloaterReg::hideInstance("prefs_spellchecker");

    // <GMDX> (no advanced graphics floater: its settings are tabs of the Graphics panel)

    // reverts any changes to current skin
    gSavedSettings.setString("SkinCurrent", sSkin);

    updateClickActionViews();

    LLFloaterPreferenceProxy * advanced_proxy_settings = LLFloaterReg::findTypedInstance<LLFloaterPreferenceProxy>("prefs_proxy");
    if (advanced_proxy_settings)
    {
        advanced_proxy_settings->cancel();
    }
    //Need to reload the navmesh if the pathing console is up
    LLHandle<LLFloaterPathfindingConsole> pathfindingConsoleHandle = LLFloaterPathfindingConsole::getInstanceHandle();
    if (!pathfindingConsoleHandle.isDead())
    {
        LLFloaterPathfindingConsole* pPathfindingConsole = pathfindingConsoleHandle.get();
        pPathfindingConsole->onRegionBoundaryCross();
    }

    if (!mSavedGraphicsPreset.empty())
    {
        gSavedSettings.setString("PresetGraphicActive", mSavedGraphicsPreset);
        LLPresetsManager::getInstance()->triggerChangeSignal();
    }

    restoreIgnoredNotifications();
}

// <FS:Ansariel> Item sent with autoresponses: shown by name, set by drag and drop, saved at once
void LLFloaterPreference::refreshAutoresponseItem()
{
    FSCopyTransInventoryDropTarget* item_target = findChild<FSCopyTransInventoryDropTarget>("autoresponse_item");
    LLButton* clear_item_btn = findChild<LLButton>("clear_autoresponse_item");
    if (!item_target || !clear_item_btn)
    {
        return;
    }

    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        item_target->setText(LLTrans::getString("AutoresponseItemNotLoggedIn"));
        clear_item_btn->setEnabled(false);
        return;
    }

    const LLUUID item_id(gSavedPerAccountSettings.getString("FSAutoresponseItemUUID"));
    if (item_id.isNull())
    {
        item_target->setText(LLTrans::getString("AutoresponseItemNotSet"));
        clear_item_btn->setEnabled(false);
        return;
    }

    clear_item_btn->setEnabled(true);
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    item_target->setText(item ? item->getName() : LLTrans::getString("AutoresponseItemNotAvailable"));
}

void LLFloaterPreference::onDADAutoresponseItem(const LLUUID& item_id)
{
    if (gInventory.getItem(item_id))
    {
        gSavedPerAccountSettings.setString("FSAutoresponseItemUUID", item_id.asString());
        refreshAutoresponseItem();
    }
}

void LLFloaterPreference::onClearAutoresponseItem()
{
    gSavedPerAccountSettings.setString("FSAutoresponseItemUUID", "");
    refreshAutoresponseItem();
}
// </FS:Ansariel>

void LLFloaterPreference::onOpen(const LLSD& key)
{
    refreshAutoresponseItem(); // <FS:Ansariel>
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreference* panel = dynamic_cast<LLPanelPreference*>(view))
        {
            panel->onOpen(key);
        }
    }

    // this variable and if that follows it are used to properly handle do not disturb mode response message
    static bool initialized = false;
    // if user is logged in and we haven't initialized do not disturb mode response yet, do it
    if (!initialized && LLStartUp::getStartupState() == STATE_STARTED)
    {
        // Special approach is used for do not disturb response localization, because "DoNotDisturbModeResponse" is
        // in non-localizable xml, and also because it may be changed by user and in this case it shouldn't be localized.
        // To keep track of whether do not disturb response is default or changed by user additional setting DoNotDisturbResponseChanged
        // was added into per account settings.

        // initialization should happen once,so setting variable to true
        initialized = true;
        // this connection is needed to properly set "DoNotDisturbResponseChanged" setting when user makes changes in
        // do not disturb response message.
        gSavedPerAccountSettings.getControl("DoNotDisturbModeResponse")->getSignal()->connect(boost::bind(&LLFloaterPreference::onDoNotDisturbResponseChanged, this));
        mRejectTeleportConnection = gSavedPerAccountSettings.getControl("SlipstreamRejectTeleportOffersResponse")->getSignal()->connect(boost::bind(&LLFloaterPreference::onRejectTeleportOffersResponseChanged, this));
        mRejectFriendshipRequestsConnection = gSavedPerAccountSettings.getControl("SlipstreamRejectFriendshipRequestsResponse")->getSignal()->connect(boost::bind(&LLFloaterPreference::onRejectFriendshipRequestResponseChanged, this));
        mAutoResponseConnection = gSavedPerAccountSettings.getControl("SlipstreamAutoresponse")->getSignal()->connect(boost::bind(&LLFloaterPreference::onAutoRespondResponseChanged, this));
        mAutoResponseNonFriendsConnection = gSavedPerAccountSettings.getControl("SlipstreamAutoresponseNotFriend")->getSignal()->connect(boost::bind(&LLFloaterPreference::onAutoRespondNonFriendsResponseChanged, this));
        mAwayResponseConnection = gSavedPerAccountSettings.getControl("FSAwayAvatarResponse")->getSignal()->connect(boost::bind(&LLFloaterPreference::onAwayResponseChanged, this)); // <FS:PP> FIRE-10500
    }
    gAgent.sendAgentUserInfoRequest();

    refreshSoundCacheInfo();

    // Bone camera (CameraFollowJoint): list the avatar's skeleton joints.
    if (LLComboBox* joint_combo = findChild<LLComboBox>("camera_follow_joint", true))
    {
        joint_combo->clearRows();
        joint_combo->add(getString("follow_joint_none"), LLSD(-1));
        if (isAgentAvatarValid())
        {
            for (LLAvatarJoint* joint : gAgentAvatarp->getSkeleton())
            {
                if (joint)
                    joint_combo->add(joint->getName(), LLSD((S32)joint->getJointNum()));
            }
        }
        joint_combo->selectByValue(LLSD(gSavedSettings.getS32("CameraFollowJoint")));
    }

    /////////////////////////// From LLPanelGeneral //////////////////////////
    // if we have no agent, we can't let them choose anything
    // if we have an agent, then we only let them choose if they have a choice
    bool can_choose_maturity =
        gAgent.getID().notNull() &&
        (gAgent.isMature() || gAgent.isGodlike());

    LLComboBox* maturity_combo = getChild<LLComboBox>("maturity_desired_combobox");
    LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest( gAgent.getID() );
    if (can_choose_maturity)
    {
        // if they're not adult or a god, they shouldn't see the adult selection, so delete it
        if (!gAgent.isAdult() && !gAgent.isGodlikeWithoutAdminMenuFakery())
        {
            // we're going to remove the adult entry from the combo
            LLScrollListCtrl* maturity_list = maturity_combo->findChild<LLScrollListCtrl>("ComboBox");
            if (maturity_list)
            {
                maturity_list->deleteItems(LLSD(SIM_ACCESS_ADULT));
            }
        }
        getChildView("maturity_desired_combobox")->setEnabled( true);
        getChildView("maturity_desired_textbox")->setVisible( false);
    }
    else
    {
        getChild<LLUICtrl>("maturity_desired_textbox")->setValue(maturity_combo->getSelectedItemLabel());
        getChildView("maturity_desired_combobox")->setEnabled( false);
    }

    // Forget previous language changes.
    mLanguageChanged = false;
    mLastQualityLevel = gSavedSettings.getU32("RenderQualityPerformance");

    // Display selected maturity icons.
    onChangeMaturity();

    onChangeModelFolder();
    onChangePBRFolder();
    onChangeTextureFolder();
    onChangeSoundFolder();
    onChangeAnimationFolder();

    // Load (double-)click to walk/teleport settings.
    updateClickActionViews();

    // Enabled/disabled popups, might have been changed by user actions
    // while preferences floater was closed.
    buildPopupLists();


    LLPanelLogin::setAlwaysRefresh(true);
    refresh();

    // Make sure the current state of prefs are saved away when
    // the floater is opened.  That will make cancel() do its job
    saveSettings();

    // Make sure there is a default preference file
    LLPresetsManager::getInstance()->createMissingDefault(PRESETS_CAMERA);
    LLPresetsManager::getInstance()->createMissingDefault(PRESETS_GRAPHIC);

    bool started = (LLStartUp::getStartupState() == STATE_STARTED);

    LLButton* load_btn = findChild<LLButton>("PrefLoadButton");
    LLButton* save_btn = findChild<LLButton>("PrefSaveButton");
    LLButton* delete_btn = findChild<LLButton>("PrefDeleteButton");
    LLButton* exceptions_btn = findChild<LLButton>("RenderExceptionsButton");
    LLButton* auto_adjustments_btn = findChild<LLButton>("AutoAdjustmentsButton");

    if (load_btn && save_btn && delete_btn && exceptions_btn && auto_adjustments_btn)
    {
        load_btn->setEnabled(started);
        save_btn->setEnabled(started);
        delete_btn->setEnabled(started);
        exceptions_btn->setEnabled(started);
        auto_adjustments_btn->setEnabled(started);
    }

    // Start unfiltered. A filter may still be applied even if the box is empty,
    // so check the last applied term too before the search data is rebuilt.
    const bool was_filtered = !mFilterEdit->getText().empty()
        || (mSearchData && !mSearchData->mLastFilter.empty());
    collectSearchableItems();
    if (was_filtered)
    {
        mFilterEdit->setText(LLStringExplicit(""));
        onUpdateFilterTerm(true);
    }
    mFilterEdit->setFocus(true);
}

void LLFloaterPreference::onRenderOptionEnable()
{
    refreshEnabledGraphics();
}

void LLFloaterPreference::onAvatarImpostorsEnable()
{
    refreshEnabledGraphics();
}

//static
// NOTE: This was moved to the section below as we may add more autoresponses.
// - [x] Do Not Disturb
// - [x] Reject Teleport
// - [ ] Autoresponses.
// - [ ] ....
//
// -- Fallen
// void LLFloaterPreference::initDoNotDisturbResponse()
//     {
//         if (!gSavedPerAccountSettings.getBOOL("DoNotDisturbResponseChanged"))
//         {
//             //LLTrans::getString("DoNotDisturbModeResponseDefault") is used here for localization (EXT-5885)
//             gSavedPerAccountSettings.setString("DoNotDisturbModeResponse", LLTrans::getString("DoNotDisturbModeResponseDefault"));
//         }
//     }

// static
void LLFloaterPreference::initAutoResponses()
{
    // NOTE: Combined the initDoNotDisturbReponse
    // -- Fallen
    if (!gSavedPerAccountSettings.getBOOL("DoNotDisturbResponseChanged"))
    {
        //LLTrans::getString("DoNotDisturbModeResponseDefault") is used here for localization (EXT-5885)
        gSavedPerAccountSettings.setString("DoNotDisturbModeResponse", LLTrans::getString("DoNotDisturbModeResponseDefault"));
    }

    if (!gSavedPerAccountSettings.getBOOL("SlipstreamRejectTeleportOffersResponseChanged"))
    {
        gSavedPerAccountSettings.setString("SlipstreamRejectTeleportOffersResponse", LLTrans::getString("RejectTeleportOffersResponseDefault"));
    }

    if (!gSavedPerAccountSettings.getBOOL("SlipstreamRejectFriendshipRequestsChanged"))
    {
        gSavedPerAccountSettings.setString("SlipstreamRejectFriendshipRequestsResponse", LLTrans::getString("RejectFriendshipRequestsResponseDefault"));
    }

    // <FS> Keyed on the "...Changed" flags (as Firestorm), not on whether the mode is on: an edited
    // response used to be thrown away at every login while its mode was off. The flags were never
    // written before, so a text that is neither the localized nor the settings-file default counts
    // as edited (and is kept).
    auto init_response = [](const char* response_control, const char* changed_control, const char* default_string)
    {
        if (gSavedPerAccountSettings.getBOOL(changed_control))
        {
            return;
        }
        const std::string localized_default = LLTrans::getString(default_string);
        LLControlVariable* control = gSavedPerAccountSettings.getControl(response_control);
        const std::string current = control ? control->getValue().asString() : std::string();
        const std::string file_default = control ? control->getDefault().asString() : std::string();
        if (!current.empty() && current != localized_default && current != file_default)
        {
            gSavedPerAccountSettings.setBOOL(changed_control, true);
        }
        else
        {
            gSavedPerAccountSettings.setString(response_control, localized_default);
        }
    };
    init_response("SlipstreamAutoresponse", "SlipstreamAutoresponseChanged", "AutoResponseModeDefault");
    init_response("SlipstreamAutoresponseNotFriend", "SlipstreamAutoresponseNotFriendChanged", "AutoResponseModeNonFriendsDefault");
    init_response("FSAwayAvatarResponse", "FSAwayAvatarResponseChanged", "AwayAvatarResponseDefault"); // <FS:PP> FIRE-10500
    // </FS>
}

//static
void LLFloaterPreference::updateShowFavoritesCheckbox(bool val)
{
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->getChild<LLUICtrl>("favorites_on_login_check")->setValue(val);
    }
}

void LLFloaterPreference::setHardwareDefaults()
{
    std::string preset_graphic_active = gSavedSettings.getString("PresetGraphicActive");
    if (!preset_graphic_active.empty())
    {
        saveGraphicsPreset(preset_graphic_active);
        saveSettings(); // save here to be able to return to the previous preset by Cancel
    }
    setRecommendedSettings();
}

void LLFloaterPreference::setRecommendedSettings()
{
    resetAutotuneSettings();
    gSavedSettings.getControl("RenderVSyncEnable")->resetToDefault(true);

    LLFeatureManager::getInstance()->applyRecommendedSettings();

    // reset indirects before refresh because we may have changed what they control
    LLAvatarComplexityControls::setIndirectControls();

    refreshEnabledGraphics();
    gSavedSettings.setString("PresetGraphicActive", "");
    LLPresetsManager::getInstance()->triggerChangeSignal();

    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    child_list_t::const_iterator iter = tabcontainer->getChildList()->begin();
    child_list_t::const_iterator end = tabcontainer->getChildList()->end();
    for ( ; iter != end; ++iter)
    {
        LLView* view = *iter;
        LLPanelPreference* panel = dynamic_cast<LLPanelPreference*>(view);
        if (panel)
        {
            panel->setHardwareDefaults();
        }
    }
}

void LLFloaterPreference::resetAutotuneSettings()
{
    // #422: "Keep my FPS" is AutoTuneLock (reset below); AutoTuneFPS and the auto-tune
    // maximum distance are gone with LL's tuner.
    const std::string autotune_settings[] = {
        "AutoTuneLock",
        "KeepAutoTuneLock",
        "TargetFPS",
        "TuningFPSStrategy",
        "AutoTuneImpostorByDistEnabled",
        "AutoTuneImpostorFarAwayDistance" ,
        "AutoTuneRenderFarClipMin",
        "RenderAvatarMaxART"
    };

    for (auto it : autotune_settings)
    {
        gSavedSettings.getControl(it)->resetToDefault(true);
    }
}

void LLFloaterPreference::getControlNames(std::vector<std::string>& names)
{
    LLView* view = findChild<LLView>("display");
    // <GMDX> The advanced graphics settings are tabs of "display" now: one tree covers them
    if (view)
    {
        std::list<LLView*> stack;
        stack.push_back(view);
        // </GMDX>
        while(!stack.empty())
        {
            // Process view on top of the stack
            LLView* curview = stack.front();
            stack.pop_front();

            LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(curview);
            if (ctrl)
            {
                LLControlVariable* control = ctrl->getControlVariable();
                if (control)
                {
                    std::string control_name = control->getName();
                    if (std::find(names.begin(), names.end(), control_name) == names.end())
                    {
                        names.push_back(control_name);
                    }
                }
            }

            for (child_list_t::const_iterator iter = curview->getChildList()->begin();
                iter != curview->getChildList()->end(); ++iter)
            {
                stack.push_back(*iter);
            }
        }
    }
}

//virtual
void LLFloaterPreference::onClose(bool app_quitting)
{
    gSavedSettings.setS32("LastPrefTab", getChild<LLTabContainer>("pref core")->getCurrentPanelIndex());
    LLPanelLogin::setAlwaysRefresh(false);
    if (!app_quitting)
    {
        // is this a BUG? cancel() should only be called in onBtnCancel()
        // but not during closing after onBtnOK()
        cancel();
    }
}

// static
void LLFloaterPreference::onBtnOK(const LLSD& userdata)
{
    // commit any outstanding text entry
    if (hasFocus())
    {
        LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
        if (cur_focus && cur_focus->acceptsTextInput())
        {
            cur_focus->onCommit();
        }
    }

    if (canClose())
    {
        saveSettings();
        apply();

        closeFloater(false); // <GMDX> (no "closeadvanced": the advanced floater is gone)

        //Conversation transcript and log path changed so reload conversations based on new location
        if (mPriorInstantMessageLogPath.length())
        {
            if (moveTranscriptsAndLog())
            {
                //When floaters are empty but have a chat history files, reload chat history into them
                LLFloaterIMSessionTab::reloadEmptyFloaters();
            }
            //Couldn't move files so restore the old path and show a notification
            else
            {
                gSavedPerAccountSettings.setString("InstantMessageLogPath", mPriorInstantMessageLogPath);
                LLNotificationsUtil::add("PreferenceChatPathChanged");
            }
            mPriorInstantMessageLogPath.clear();
        }

        LLUIColorTable::instance().saveUserSettings();
        gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);

        // Only save once logged in and loaded per account settings
        if (mGotPersonalInfo)
        {
            gSavedPerAccountSettings.saveToFile(gSavedSettings.getString("PerAccountSettingsFile"), true);
        }
    }
    else
    {
        // Show beep, pop up dialog, etc.
        LL_INFOS("Preferences") << "Can't close preferences!" << LL_ENDL;
    }

    LLPanelLogin::updateLocationSelectorsVisibility();
    //Need to reload the navmesh if the pathing console is up
    LLHandle<LLFloaterPathfindingConsole> pathfindingConsoleHandle = LLFloaterPathfindingConsole::getInstanceHandle();
    if ( !pathfindingConsoleHandle.isDead() )
    {
        LLFloaterPathfindingConsole* pPathfindingConsole = pathfindingConsoleHandle.get();
        pPathfindingConsole->onRegionBoundaryCross();
    }
}

// static
void LLFloaterPreference::onBtnCancel(const LLSD& userdata)
{
    if (hasFocus())
    {
        LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
        if (cur_focus && cur_focus->acceptsTextInput())
        {
            cur_focus->onCommit();
        }
        refresh();
    }

    // <GMDX> (no "closeadvanced": the advanced floater is gone)
    cancel();
    closeFloater();
}

//static
void LLFloaterPreference::refreshInstance()
{
    if (LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
    {
        instance->refresh();
    }
}

// static
void LLFloaterPreference::updateUserInfo(const std::string& visibility)
{
    if (LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
    {
        instance->setPersonalInfo(visibility);
    }
}

// static
void LLFloaterPreference::refreshEnabledGraphics()
{
    // <GMDX> refresh() refreshes the advanced graphics tabs too (the Graphics panel)
    if (LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
    {
        instance->refresh();
    }
}

void LLFloaterPreference::onClickClearCache()
{
    LLNotificationsUtil::add("ConfirmClearCache", LLSD(), LLSD(), callback_clear_cache);
}

void LLFloaterPreference::onClickBrowserClearCache()
{
    LLNotificationsUtil::add("ConfirmClearBrowserCache", LLSD(), LLSD(), callback_clear_browser_cache);
}

// Called when user changes language via the combobox.
void LLFloaterPreference::onLanguageChange()
{
    // Let the user know that the change will only take effect after restart.
    // Do it only once so that we're not too irritating.
    if (!mLanguageChanged)
    {
        LLNotificationsUtil::add("ChangeLanguage");
        mLanguageChanged = true;
    }
}

void LLFloaterPreference::onNameTagOpacityChange(const LLSD& newvalue)
{
    if (LLColorSwatchCtrl* color_swatch = findChild<LLColorSwatchCtrl>("background"))
    {
        LLColor4 new_color = color_swatch->get();
        color_swatch->set(new_color.setAlpha((F32)newvalue.asReal()));
    }
}

void LLFloaterPreference::onClickSetCache()
{
    std::string cur_name(gSavedSettings.getString("CacheLocation"));
//  std::string cur_top_folder(gDirUtilp->getBaseFileName(cur_name));

    std::string proposed_name(cur_name);

    (new LLDirPickerThread(boost::bind(&LLFloaterPreference::changeCachePath, this, _1, _2), proposed_name))->getFile();
}

void LLFloaterPreference::changeCachePath(const std::vector<std::string>& filenames, std::string proposed_name)
{
    std::string dir_name = filenames[0];
    if (!dir_name.empty() && dir_name != proposed_name)
    {
        std::string new_top_folder(gDirUtilp->getBaseFileName(dir_name));
        LLNotificationsUtil::add("CacheWillBeMoved");
        gSavedSettings.setString("NewCacheLocation", dir_name);
        gSavedSettings.setString("NewCacheLocationTopFolder", new_top_folder);
    }
    else
    {
        std::string cache_location = gDirUtilp->getCacheDir();
        gSavedSettings.setString("CacheLocation", cache_location);
        std::string top_folder(gDirUtilp->getBaseFileName(cache_location));
        gSavedSettings.setString("CacheLocationTopFolder", top_folder);
    }
}

void LLFloaterPreference::onClickResetCache()
{
    if (gDirUtilp->getCacheDir(false) == gDirUtilp->getCacheDir(true))
    {
        // The cache location was already the default.
        return;
    }
    gSavedSettings.setString("NewCacheLocation", "");
    gSavedSettings.setString("NewCacheLocationTopFolder", "");
    LLNotificationsUtil::add("CacheWillBeMoved");
    std::string cache_location = gDirUtilp->getCacheDir(false);
    gSavedSettings.setString("CacheLocation", cache_location);
    std::string top_folder(gDirUtilp->getBaseFileName(cache_location));
    gSavedSettings.setString("CacheLocationTopFolder", top_folder);
}

void LLFloaterPreference::onClickSetSoundCache()
{
    const std::string configured = gSavedSettings.getString("SlipstreamSoundCacheLocation");
    std::string proposed_name(configured.empty() ? LLSoundCache::getDefaultDir() : configured);

    (new LLDirPickerThread(boost::bind(&LLFloaterPreference::changeSoundCachePath, this, _1, _2), proposed_name))->getFile();
}

void LLFloaterPreference::changeSoundCachePath(const std::vector<std::string>& filenames, std::string proposed_name)
{
    if (filenames.empty())
    {
        return;
    }
    const std::string dir_name = filenames[0];
    if (!dir_name.empty() && dir_name != proposed_name)
    {
        // Like the cache location, the sound cache folder is picked at startup.
        gSavedSettings.setString("SlipstreamSoundCacheLocation", dir_name);
        LLNotificationsUtil::add("SoundCacheWillBeMoved");
        refreshSoundCacheInfo();
    }
}

void LLFloaterPreference::onClickResetSoundCache()
{
    if (gSavedSettings.getString("SlipstreamSoundCacheLocation").empty())
    {
        // Already the default.
        return;
    }
    gSavedSettings.setString("SlipstreamSoundCacheLocation", "");
    LLNotificationsUtil::add("SoundCacheWillBeMoved");
    refreshSoundCacheInfo();
}

void LLFloaterPreference::refreshSoundCacheInfo()
{
    // Shows the configured folder (which applies on the next start), or the default.
    const std::string configured = gSavedSettings.getString("SlipstreamSoundCacheLocation");
    const std::string location = configured.empty() ? LLSoundCache::getDefaultDir() : configured;
    if (LLUICtrl* editor = findChild<LLUICtrl>("sound_cache_location"))
    {
        editor->setValue(location);
        editor->setToolTip(location);
    }
    if (LLUICtrl* usage = findChild<LLUICtrl>("sound_cache_usage"))
    {
        constexpr F64 MB = 1024.0 * 1024.0;
        usage->setValue(llformat("%.0f MB used, %u files", (F64)LLSoundCache::getBytesUsed() / MB, LLSoundCache::getEntryCount()));
        usage->setToolTip(llformat("Decoded sounds in %s: %.1f MB of %.0f MB, %u files. This session: peak %.1f MB, %u evicted.",
                                   LLSoundCache::getDir().c_str(),
                                   (F64)LLSoundCache::getBytesUsed() / MB, (F64)LLSoundCache::getBudget() / MB,
                                   LLSoundCache::getEntryCount(),
                                   (F64)LLSoundCache::getPeakBytes() / MB, LLSoundCache::getEvictionCount()));
    }
    refreshTextureTierInfo();   // (the texture cache rows sit right below)
}

void LLFloaterPreference::refreshTextureTierInfo()
{
    constexpr F64 MB = 1024.0 * 1024.0;
    if (LLUICtrl* usage = findChild<LLUICtrl>("gputex_cache_usage"))
    {
        const LLGpuTexCache::Stats st = LLGpuTexCache::getStats();
        usage->setValue(llformat("%.0f MB used, %u files", (F64)st.used / MB, st.entries));
        usage->setToolTip(llformat("GPU-ready textures in %s: %.1f MB of %.0f MB, %u files%s. This session: peak %.1f MB, "
                                   "%llu loaded from it, %llu not in it, %llu evicted, %llu bad files discarded, %llu written.",
                                   LLGpuTexCache::getDir().c_str(), (F64)st.used / MB, (F64)st.budget / MB, st.entries,
                                   st.scanned ? "" : " (still indexing)", (F64)st.peak / MB,
                                   (unsigned long long)st.hits, (unsigned long long)st.misses,
                                   (unsigned long long)st.evictions, (unsigned long long)st.corrupt,
                                   (unsigned long long)st.writes));
    }
    if (LLUICtrl* ram = findChild<LLUICtrl>("texture_ram_usage"))
    {
        U64 req = 0, opt = 0;
        U64 by_reason[LLTextureData::VKLEASE_COUNT] = {};
        LLViewerFetchedTexture::getVKTier1Bytes(req, opt, by_reason);
        const char* policy = LLViewerFetchedTexture::vkPreferDisk()
                               ? (gSavedSettings.getU32("SlipstreamTextureMemoryMode") == 1 ? "prefer disk cache"
                                                                                           : "disk cache, automatic on this RAM size")
                           : LLViewerFetchedTexture::vkKeepTier1() ? "world copies kept while memory is plentiful"
                                                                    : "dropped once on the GPU (memory is tight)";
        ram->setValue(llformat("Texture copies in RAM: %.0f MB (%.0f MB held by features, %.0f MB about to go or kept; %s)",
                               (F64)(req + opt) / MB, (F64)req / MB, (F64)opt / MB, policy));
        // #707: who holds them (per lease reason)
        std::string tip = "Decoded texture copies exist only while a feature needs them; the GPU and the disk caches hold "
                          "the rest. Held by:";
        for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i)
            tip += llformat(" %s %.0f MB%s", LLTextureData::vkLeaseName((U8)i), (F64)by_reason[i] / MB,
                            i + 1 < LLTextureData::VKLEASE_COUNT ? "," : ".");
        ram->setToolTip(tip);
    }
}

void LLFloaterPreference::buildPopupLists()
{
    mDisabledPopups->deleteAllItems();
    mEnabledPopups->deleteAllItems();

    for (LLNotifications::TemplateMap::const_iterator iter = LLNotifications::instance().templatesBegin();
         iter != LLNotifications::instance().templatesEnd();
         ++iter)
    {
        LLNotificationTemplatePtr templatep = iter->second;
        LLNotificationFormPtr formp = templatep->mForm;

        LLNotificationForm::EIgnoreType ignore = formp->getIgnoreType();
        if (ignore <= LLNotificationForm::IGNORE_NO)
            continue;

        LLSD row;
        row["columns"][0]["value"] = formp->getIgnoreMessage();
        row["columns"][0]["font"] = "SANSSERIF_SMALL";
        row["columns"][0]["width"] = 400;

        LLScrollListItem* item = NULL;

        bool show_popup = !formp->getIgnored();
        if (!show_popup)
        {
            if (ignore == LLNotificationForm::IGNORE_WITH_LAST_RESPONSE)
            {
                LLSD last_response = LLUI::getInstance()->mSettingGroups["config"]->getLLSD("Default" + templatep->mName);
                if (!last_response.isUndefined())
                {
                    for (LLSD::map_const_iterator it = last_response.beginMap();
                         it != last_response.endMap();
                         ++it)
                    {
                        if (it->second.asBoolean())
                        {
                            row["columns"][1]["value"] = formp->getElement(it->first)["ignore"].asString();
                            row["columns"][1]["font"] = "SANSSERIF_SMALL";
                            row["columns"][1]["width"] = 360;
                            break;
                        }
                    }
                }
            }
            item = mDisabledPopups->addElement(row);
        }
        else
        {
            item = mEnabledPopups->addElement(row);
        }

        if (item)
        {
            item->setUserdata((void*)&iter->first);
        }
    }
}

void LLFloaterPreference::refreshEnabledState()
{
// [RLVa:KB] - Checked: 2013-05-11 (RLVa-1.4.9)
    if (RlvActions::isRlvEnabled())
    {
        getChild<LLUICtrl>("do_not_disturb_response")->setEnabled(!RlvActions::hasBehaviour(RLV_BHVR_SENDIM));
    }
// [/RLVa:KB]

    // Cannot have floater active until caps have been received
    getChild<LLButton>("default_creation_permissions")->setEnabled(LLStartUp::getStartupState() >= STATE_STARTED);

    getChildView("block_list")->setEnabled(LLLoginInstance::getInstance()->authSuccess());

}

void LLAvatarComplexityControls::setIndirectControls()
{
    /*
     * We have controls that have an indirect relationship between the control
     * values and adjacent text and the underlying setting they influence.
     * In each case, the control and its associated setting are named Indirect<something>
     * This method interrogates the controlled setting and establishes the
     * appropriate value for the indirect control. It must be called whenever the
     * underlying setting may have changed other than through the indirect control,
     * such as when the 'Reset all to recommended settings' button is used...
     */
    setIndirectMaxNonImpostors();
    setIndirectMaxArc();
}

// static
void LLAvatarComplexityControls::setIndirectMaxNonImpostors()
{
    U32 max_non_impostors = gSavedSettings.getU32("RenderAvatarMaxNonImpostors");
    // for this one, we just need to make zero, which means off, the max value of the slider
    U32 indirect_max_non_impostors = (0 == max_non_impostors) ? LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER : max_non_impostors;
    gSavedSettings.setU32("IndirectMaxNonImpostors", indirect_max_non_impostors);
}

void LLAvatarComplexityControls::setIndirectMaxArc()
{
    U32 max_arc = gSavedSettings.getU32("RenderAvatarMaxComplexity");
    U32 indirect_max_arc;
    if (0 == max_arc)
    {
        // the off position is all the way to the right, so set to control max
        indirect_max_arc = INDIRECT_MAX_ARC_OFF;
    }
    else
    {
        // This is the inverse of the calculation in updateMaxComplexity
        indirect_max_arc = (U32)ll_round(((log(F32(max_arc)) - MIN_ARC_LOG) / ARC_LIMIT_MAP_SCALE)) + MIN_INDIRECT_ARC_LIMIT;
    }
    gSavedSettings.setU32("IndirectMaxComplexity", indirect_max_arc);
}

void LLFloaterPreference::refresh()
{
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreferenceControls* panel = dynamic_cast<LLPanelPreferenceControls*>(view))
        {
            panel->refresh();
            break;
        }
    }
    LLFloater::refresh();
    setMaxNonImpostorsText(
        gSavedSettings.getU32("RenderAvatarMaxNonImpostors"),
        getChild<LLTextBox>("IndirectMaxNonImpostorsText", true));
    LLAvatarComplexityControls::setText(
        gSavedSettings.getU32("RenderAvatarMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText", true));
    refreshEnabledState();
    // <GMDX> The Graphics panel (LLFloaterPreferenceGraphicsAdvanced) holds the advanced graphics tabs
    if (LLPanel* graphics = tabcontainer->getPanelByName("display"))
    {
        graphics->refresh();
    }
    // </GMDX>
    updateClickActionViews();
}

void LLFloaterPreference::onCommitWindowedMode()
{
    refresh();
}

void LLFloaterPreference::onChangeQuality(const LLSD& data)
{
    U32 level = (U32)data.asReal();
    constexpr U32 LVL_HIGH = 4;
    if (level >= LVL_HIGH && mLastQualityLevel < level)
    {
        constexpr U32 LOW_MEM_THRESHOLD = 4097;
        U32 total_mem = (U32Megabytes)LLMemory::getMaxMemKB();
        if (total_mem < LOW_MEM_THRESHOLD)
        {
            LLSD args;
            args["TOTAL_MEM"] = LLSD::Integer(total_mem);
            LLNotificationsUtil::add("PreferenceQualityWithLowMemory", args, LLSD(), [this](const LLSD& notification, const LLSD& response)
            {
                S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
                // If cancel pressed
                if (option == 1)
                {
                    constexpr U32 LVL_MED_PLUS = 3;
                    gSavedSettings.setU32("RenderQualityPerformance", LVL_MED_PLUS);
                    mLastQualityLevel = LVL_MED_PLUS;
                    LLFeatureManager::getInstance()->setGraphicsLevel(LVL_MED_PLUS, true);
                    refreshEnabledGraphics();
                    refresh();
                }
            }
            );
        }
    }
    mLastQualityLevel = level;
    LLFeatureManager::getInstance()->setGraphicsLevel(level, true);
    refreshEnabledGraphics();
    refresh();
}

void LLFloaterPreference::onClickSetSounds()
{
    // Disable Enable gesture sounds checkbox if the master sound is disabled
    // or if sound effects are disabled.
    getChild<LLCheckBoxCtrl>("gesture_audio_play_btn")->setEnabled(!gSavedSettings.getBOOL("MuteSounds"));
}

void LLFloaterPreference::onClickEnablePopup()
{
    std::vector<LLScrollListItem*> items = mDisabledPopups->getAllSelected();
    std::vector<LLScrollListItem*>::iterator itor;
    for (itor = items.begin(); itor != items.end(); ++itor)
    {
        LLNotificationTemplatePtr templatep = LLNotifications::instance().getTemplate(*(std::string*)((*itor)->getUserdata()));
        //gSavedSettings.setWarning(templatep->mName, true);
        std::string notification_name = templatep->mName;
        LLUI::getInstance()->mSettingGroups["ignores"]->setBOOL(notification_name, true);
    }

    buildPopupLists();
    if (!mFilterEdit->getText().empty())
    {
        filterIgnorableNotifications();
    }
}

void LLFloaterPreference::onClickDisablePopup()
{
    std::vector<LLScrollListItem*> items = mEnabledPopups->getAllSelected();
    std::vector<LLScrollListItem*>::iterator itor;
    for (itor = items.begin(); itor != items.end(); ++itor)
    {
        LLNotificationTemplatePtr templatep = LLNotifications::instance().getTemplate(*(std::string*)((*itor)->getUserdata()));
        templatep->mForm->setIgnored(true);
    }

    buildPopupLists();
    if (!mFilterEdit->getText().empty())
    {
        filterIgnorableNotifications();
    }
}

void LLFloaterPreference::resetAllIgnored()
{
    for (LLNotifications::TemplateMap::const_iterator iter = LLNotifications::instance().templatesBegin();
         iter != LLNotifications::instance().templatesEnd();
         ++iter)
    {
        if (iter->second->mForm->getIgnoreType() > LLNotificationForm::IGNORE_NO)
        {
            iter->second->mForm->setIgnored(false);
        }
    }
}

void LLFloaterPreference::setAllIgnored()
{
    for (LLNotifications::TemplateMap::const_iterator iter = LLNotifications::instance().templatesBegin();
         iter != LLNotifications::instance().templatesEnd();
         ++iter)
    {
        if (iter->second->mForm->getIgnoreType() > LLNotificationForm::IGNORE_NO)
        {
            iter->second->mForm->setIgnored(true);
        }
    }
}

void LLFloaterPreference::onClickLogPath()
{
    std::string proposed_name(gSavedPerAccountSettings.getString("InstantMessageLogPath"));
    mPriorInstantMessageLogPath.clear();

    (new LLDirPickerThread(boost::bind(&LLFloaterPreference::changeLogPath, this, _1, _2), proposed_name))->getFile();
}

void LLFloaterPreference::changeLogPath(const std::vector<std::string>& filenames, std::string proposed_name)
{
    //Path changed
    if (proposed_name != filenames[0])
    {
        gSavedPerAccountSettings.setString("InstantMessageLogPath", filenames[0]);
        mPriorInstantMessageLogPath = proposed_name;

        // enable/disable 'Delete transcripts button
        updateDeleteTranscriptsButton();
    }
}

bool LLFloaterPreference::moveTranscriptsAndLog()
{
    std::string instantMessageLogPath(gSavedPerAccountSettings.getString("InstantMessageLogPath"));
    std::string chatLogPath = gDirUtilp->add(instantMessageLogPath, gDirUtilp->getUserName());

    bool madeDirectory = false;

    //Does the directory really exist, if not then make it
    if (!LLFile::isdir(chatLogPath))
    {
        //mkdir success is defined as zero
        if (LLFile::mkdir(chatLogPath) != 0)
        {
            return false;
        }
        madeDirectory = true;
    }

    std::string originalConversationLogDir = LLConversationLog::instance().getFileName();
    std::string targetConversationLogDir = gDirUtilp->add(chatLogPath, "conversation.log");
    //Try to move the conversation log
    if (!LLConversationLog::instance().moveLog(originalConversationLogDir, targetConversationLogDir))
    {
        //Couldn't move the log and created a new directory so remove the new directory
        if (madeDirectory)
        {
            LLFile::rmdir(chatLogPath);
        }
        return false;
    }

    //Attempt to move transcripts
    std::vector<std::string> listOfTranscripts;
    std::vector<std::string> listOfFilesMoved;

    LLLogChat::getListOfTranscriptFiles(listOfTranscripts);

    if (!LLLogChat::moveTranscripts(gDirUtilp->getChatLogsDir(),
                                    instantMessageLogPath,
                                    listOfTranscripts,
                                    listOfFilesMoved))
    {
        //Couldn't move all the transcripts so restore those that moved back to their old location
        LLLogChat::moveTranscripts(instantMessageLogPath,
            gDirUtilp->getChatLogsDir(),
            listOfFilesMoved);

        //Move the conversation log back
        LLConversationLog::instance().moveLog(targetConversationLogDir, originalConversationLogDir);

        if (madeDirectory)
        {
            LLFile::rmdir(chatLogPath);
        }

        return false;
    }

    gDirUtilp->setChatLogsDir(instantMessageLogPath);
    gDirUtilp->updatePerAccountChatLogsDir();

    return true;
}

void LLFloaterPreference::setPersonalInfo(const std::string& visibility)
{
    mGotPersonalInfo = true;
    mDirectoryVisibility = visibility;

    if (visibility == VISIBILITY_DEFAULT)
    {
        mOriginalHideOnlineStatus = false;
        getChildView("online_visibility")->setEnabled(true);
    }
    else if (visibility == VISIBILITY_HIDDEN)
    {
        mOriginalHideOnlineStatus = true;
        getChildView("online_visibility")->setEnabled(true);
    }
    else
    {
        mOriginalHideOnlineStatus = true;
    }

    getChild<LLUICtrl>("online_searchresults")->setEnabled(true);
    getChildView("friends_online_notify_checkbox")->setEnabled(true);
    getChild<LLUICtrl>("online_visibility")->setValue(mOriginalHideOnlineStatus);
    getChild<LLUICtrl>("online_visibility")->setLabelArg("[DIR_VIS]", mDirectoryVisibility);

    getChildView("favorites_on_login_check")->setEnabled(true);
    getChildView("log_path_button")->setEnabled(true);
    getChildView("chat_font_size")->setEnabled(true);
    getChildView("conversation_log_combo")->setEnabled(true);
    getChild<LLUICtrl>("voice_call_friends_only_check")->setEnabled(true);
    getChild<LLUICtrl>("voice_call_friends_only_check")->setValue(gSavedPerAccountSettings.getBOOL("VoiceCallsFriendsOnly"));
}

void LLFloaterPreference::refreshUI()
{
    refresh();
}

void LLAvatarComplexityControls::updateMax(LLSliderCtrl* slider, LLTextBox* value_label, bool short_val)
{
    // Called when the IndirectMaxComplexity control changes
    // Responsible for fixing the slider label (IndirectMaxComplexityText) and setting RenderAvatarMaxComplexity
    U32 indirect_value = slider->getValue().asInteger();
    U32 max_arc;

    if (INDIRECT_MAX_ARC_OFF == indirect_value)
    {
        // The 'off' position is when the slider is all the way to the right,
        // which is a value of INDIRECT_MAX_ARC_OFF,
        // so it is necessary to set max_arc to 0 disable muted avatars.
        max_arc = 0;
    }
    else
    {
        // if this is changed, the inverse calculation in setIndirectMaxArc
        // must be changed to match
        max_arc = (U32)ll_round(exp(MIN_ARC_LOG + (ARC_LIMIT_MAP_SCALE * (indirect_value - MIN_INDIRECT_ARC_LIMIT))));
    }

    gSavedSettings.setU32("RenderAvatarMaxComplexity", (U32)max_arc);
    setText(max_arc, value_label, short_val);
}

void LLAvatarComplexityControls::setText(U32 value, LLTextBox* text_box, bool short_val)
{
    if (0 == value)
    {
        text_box->setText(LLTrans::getString("no_limit"));
    }
    else
    {
        std::string text_value = short_val ? llformat("%d", value / 1000) : llformat("%d", value);
        text_box->setText(text_value);
    }
}

void LLAvatarComplexityControls::updateMaxRenderTime(LLSliderCtrl* slider, LLTextBox* value_label, bool short_val)
{
    setRenderTimeText((F32)(LLPerfStats::renderAvatarMaxART_ns/1000), value_label, short_val);
}

void LLAvatarComplexityControls::setRenderTimeText(F32 value, LLTextBox* text_box, bool short_val)
{
    if (0 == value)
    {
        text_box->setText(LLTrans::getString("no_limit"));
    }
    else
    {
        text_box->setText(llformat("%.0f", value));
    }
}

void LLFloaterPreference::updateMaxNonImpostors()
{
    // Called when the IndirectMaxNonImpostors control changes
    // Responsible for fixing the slider label (IndirectMaxNonImpostorsText) and setting RenderAvatarMaxNonImpostors
    LLSliderCtrl* ctrl = getChild<LLSliderCtrl>("IndirectMaxNonImpostors", true);
    U32 value = ctrl->getValue().asInteger();

    if (0 == value || LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER <= value)
    {
        value = 0;
    }
    gSavedSettings.setU32("RenderAvatarMaxNonImpostors", value);
    LLVOAvatar::updateImpostorRendering(value); // make it effective immediately
    setMaxNonImpostorsText(value, getChild<LLTextBox>("IndirectMaxNonImpostorsText"));
}

void LLFloaterPreference::updateIndirectMaxNonImpostors(const LLSD& newvalue)
{
    U32 value = newvalue.asInteger();
    if ((value != 0) && (value != gSavedSettings.getU32("IndirectMaxNonImpostors")))
    {
        gSavedSettings.setU32("IndirectMaxNonImpostors", value);
    }
    setMaxNonImpostorsText(value, getChild<LLTextBox>("IndirectMaxNonImpostorsText"));
}

void LLFloaterPreference::setMaxNonImpostorsText(U32 value, LLTextBox* text_box)
{
    if (0 == value)
    {
        text_box->setText(LLTrans::getString("no_limit"));
    }
    else
    {
        text_box->setText(llformat("%d", value));
    }
}

void LLFloaterPreference::updateMaxComplexity()
{
    // Called when the IndirectMaxComplexity control changes
    LLAvatarComplexityControls::updateMax(
        getChild<LLSliderCtrl>("IndirectMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText"));
}

void LLFloaterPreference::updateComplexityText()
{
    LLAvatarComplexityControls::setText(gSavedSettings.getU32("RenderAvatarMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText", true));
}

bool LLFloaterPreference::loadFromFilename(const std::string& filename, std::map<std::string, std::string> &label_map)
{
    LLXMLNodePtr root;

    if (!LLXMLNode::parseFile(filename, root, NULL))
    {
        LL_WARNS("Preferences") << "Unable to parse file " << filename << LL_ENDL;
        return false;
    }

    if (!root->hasName("labels"))
    {
        LL_WARNS("Preferences") << filename << " is not a valid definition file" << LL_ENDL;
        return false;
    }

    LabelTable params;
    LLXUIParser parser;
    parser.readXUI(root, params, filename);

    if (params.validateBlock())
    {
        for (LLInitParam::ParamIterator<LabelDef>::const_iterator it = params.labels.begin();
            it != params.labels.end();
            ++it)
        {
            LabelDef label_entry = *it;
            label_map[label_entry.name] = label_entry.value;
        }
    }
    else
    {
        LL_WARNS("Preferences") << filename << " failed to load" << LL_ENDL;
        return false;
    }

    return true;
}

void LLFloaterPreference::onChangeMaturity()
{
    U8 sim_access = gSavedSettings.getU32("PreferredMaturity");

    getChild<LLIconCtrl>("rating_icon_general")->setVisible(sim_access == SIM_ACCESS_PG
                                                            || sim_access == SIM_ACCESS_MATURE
                                                            || sim_access == SIM_ACCESS_ADULT);

    getChild<LLIconCtrl>("rating_icon_moderate")->setVisible(sim_access == SIM_ACCESS_MATURE
                                                            || sim_access == SIM_ACCESS_ADULT);

    getChild<LLIconCtrl>("rating_icon_adult")->setVisible(sim_access == SIM_ACCESS_ADULT);
}

void LLFloaterPreference::onChangeComplexityMode(const LLSD& newvalue)
{
    bool enable_complexity = newvalue.asInteger() != LLVOAvatar::AV_RENDER_ONLY_SHOW_FRIENDS;
    getChild<LLSliderCtrl>("IndirectMaxComplexity")->setEnabled(enable_complexity);
}

std::string get_category_path(LLFolderType::EType cat_type)
{
    LLUUID cat_id = gInventory.findUserDefinedCategoryUUIDForType(cat_type);
    return get_category_path(cat_id);
}

void LLFloaterPreference::onChangeModelFolder()
{
    if (gInventory.isInventoryUsable())
    {
        getChild<LLTextBox>("upload_models")->setText(get_category_path(LLFolderType::FT_OBJECT));
    }
}

void LLFloaterPreference::onChangePBRFolder()
{
    if (gInventory.isInventoryUsable())
    {
        getChild<LLTextBox>("upload_pbr")->setText(get_category_path(LLFolderType::FT_MATERIAL));
    }
}

void LLFloaterPreference::onChangeTextureFolder()
{
    if (gInventory.isInventoryUsable())
    {
        getChild<LLTextBox>("upload_textures")->setText(get_category_path(LLFolderType::FT_TEXTURE));
    }
}

void LLFloaterPreference::onChangeSoundFolder()
{
    if (gInventory.isInventoryUsable())
    {
        getChild<LLTextBox>("upload_sounds")->setText(get_category_path(LLFolderType::FT_SOUND));
    }
}

void LLFloaterPreference::onChangeAnimationFolder()
{
    if (gInventory.isInventoryUsable())
    {
        getChild<LLTextBox>("upload_animation")->setText(get_category_path(LLFolderType::FT_ANIMATION));
    }
}

// FIXME: this will stop you from spawning the sidetray from preferences dialog on login screen
// but the UI for this will still be enabled
void LLFloaterPreference::onClickBlockList()
{
// [SL:KB] - Patch: World-Derender | Checked: Catznip-3.2
    LLFloaterReg::showInstance("blocked");
// [/SL:KB]
//  LLFloaterSidePanelContainer::showPanel("people", "panel_people",
//      LLSD().with("people_panel_tab_name", "blocked_panel"));
}

void LLFloaterPreference::onClickProxySettings()
{
    LLFloaterReg::showInstance("prefs_proxy");
}

void LLFloaterPreference::onClickTranslationSettings()
{
    LLFloaterReg::showInstance("prefs_translation");
}

void LLFloaterPreference::onClickAutoReplace()
{
    LLFloaterReg::showInstance("prefs_autoreplace");
}

void LLFloaterPreference::onClickSpellChecker()
{
    LLFloaterReg::showInstance("prefs_spellchecker");
}

void LLFloaterPreference::onClickRenderExceptions()
{
    LLFloaterReg::showInstance("blocked", LLSD("avatar_rendering_tab"));
}

void LLFloaterPreference::onClickAutoAdjustments()
{
    LLFloaterPerformance* performance_floater = LLFloaterReg::showTypedInstance<LLFloaterPerformance>("performance");
    if (performance_floater)
    {
        performance_floater->showAutoadjustmentsPanel();
    }
}

void LLFloaterPreference::onClickAdvanced()
{
    // <GMDX> The advanced graphics settings are tabs of the Graphics panel: show the first of
    // them (was: open the Advanced Graphics floater).
    selectPanel("display");
    if (LLTabContainer* graphics_tabs = findChild<LLTabContainer>("adv_tab_container"))
    {
        graphics_tabs->selectTabByName("adv_tab_general");
    }
    // </GMDX>
}

void LLFloaterPreference::onClickActionChange()
{
    updateClickActionControls();
}

void LLFloaterPreference::onAtmosShaderChange()
{
    if (LLCheckBoxCtrl* ctrl_alm = getChild<LLCheckBoxCtrl>("UseLightShaders"))
    {
        //Deferred/SSAO/Shadows
        bool bumpshiny = LLFeatureManager::getInstance()->isFeatureAvailable("RenderCubeMap") && LLFeatureManager::getInstance()->isFeatureAvailable("RenderObjectBump") && gSavedSettings.getBOOL("RenderObjectBump");
        bool shaders = gSavedSettings.getBOOL("WindLightUseAtmosShaders");
        bool enabled = LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferred") &&
                        bumpshiny &&
                        shaders;

        ctrl_alm->setEnabled(enabled);
    }
}

void LLFloaterPreference::onClickPermsDefault()
{
    LLFloaterReg::showInstance("perms_default");
}

void LLFloaterPreference::onClickRememberedUsernames()
{
    LLFloaterReg::showInstance("forget_username");
}

void LLFloaterPreference::onDeleteTranscripts()
{
    LLSD args;
    args["FOLDER"] = gDirUtilp->getUserName();

    LLNotificationsUtil::add("PreferenceChatDeleteTranscripts", args, LLSD(), boost::bind(&LLFloaterPreference::onDeleteTranscriptsResponse, this, _1, _2));
}

void LLFloaterPreference::onDeleteTranscriptsResponse(const LLSD& notification, const LLSD& response)
{
    if (0 == LLNotificationsUtil::getSelectedOption(notification, response))
    {
        LLLogChat::deleteTranscripts();
        updateDeleteTranscriptsButton();
    }
}

void LLFloaterPreference::onLogChatHistorySaved()
{
    if (!mDeleteTranscriptsBtn->getEnabled())
    {
        mDeleteTranscriptsBtn->setEnabled(true);
    }
}

void LLFloaterPreference::updateClickActionControls()
{
    const int single_clk_action = getChild<LLComboBox>("single_click_action_combo")->getValue().asInteger();
    const int double_clk_action = getChild<LLComboBox>("double_click_action_combo")->getValue().asInteger();

    // Todo: This is a very ugly way to get access to keybindings.
    // Reconsider possible options.
    // Potential option: make constructor of LLKeyConflictHandler private
    // but add a getter that will return shared pointer for specific
    // mode, pointer should only exist so long as there are external users.
    // In such case we won't need to do this 'dynamic_cast' nightmare.
    // updateTable() can also be avoided
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreferenceControls* panel = dynamic_cast<LLPanelPreferenceControls*>(view))
        {
            panel->setKeyBind("walk_to",
                              EMouseClickType::CLICK_LEFT,
                              KEY_NONE,
                              MASK_NONE,
                              single_clk_action == 1);

            panel->setKeyBind("walk_to",
                              EMouseClickType::CLICK_DOUBLELEFT,
                              KEY_NONE,
                              MASK_NONE,
                              double_clk_action == 1);

            panel->setKeyBind("teleport_to",
                              EMouseClickType::CLICK_DOUBLELEFT,
                              KEY_NONE,
                              MASK_NONE,
                              double_clk_action == 2);

            panel->updateAndApply();
        }
    }
}

void LLFloaterPreference::updateClickActionViews()
{
    bool click_to_walk = false;
    bool dbl_click_to_walk = false;
    bool dbl_click_to_teleport = false;

    // Todo: This is a very ugly way to get access to keybindings.
    // Reconsider possible options.
    LLTabContainer* tabcontainer = getChild<LLTabContainer>("pref core");
    for (LLView* view : *tabcontainer->getChildList())
    {
        if (LLPanelPreferenceControls* panel = dynamic_cast<LLPanelPreferenceControls*>(view))
        {
            click_to_walk = panel->canKeyBindHandle("walk_to",
                EMouseClickType::CLICK_LEFT,
                KEY_NONE,
                MASK_NONE);

            dbl_click_to_walk = panel->canKeyBindHandle("walk_to",
                EMouseClickType::CLICK_DOUBLELEFT,
                KEY_NONE,
                MASK_NONE);

            dbl_click_to_teleport = panel->canKeyBindHandle("teleport_to",
                EMouseClickType::CLICK_DOUBLELEFT,
                KEY_NONE,
                MASK_NONE);
        }
    }

    getChild<LLComboBox>("single_click_action_combo")->setValue((int)click_to_walk);
    getChild<LLComboBox>("double_click_action_combo")->setValue(dbl_click_to_teleport ? 2 : (int)dbl_click_to_walk);
}

void LLFloaterPreference::updateSearchableItems()
{
    mSearchDataDirty = true;
}

void LLFloaterPreference::applyUIColor(LLUICtrl* ctrl, const LLSD& param)
{
    LLUIColorTable::instance().setColor(param.asString(), LLColor4(ctrl->getValue()));
}

void LLFloaterPreference::getUIColor(LLUICtrl* ctrl, const LLSD& param)
{
    LLColorSwatchCtrl* color_swatch = (LLColorSwatchCtrl*) ctrl;
    color_swatch->setOriginal(LLUIColorTable::instance().getColor(param.asString()));
}

void LLFloaterPreference::setCacheLocation(const LLStringExplicit& location)
{
    LLUICtrl* cache_location_editor = getChild<LLUICtrl>("cache_location");
    cache_location_editor->setValue(location);
    cache_location_editor->setToolTip(location);
}

void LLFloaterPreference::selectPanel(const LLSD& name)
{
    LLTabContainer * tab_containerp = getChild<LLTabContainer>("pref core");
    LLPanel * panel = tab_containerp->getPanelByName(name.asStringRef());
    if (NULL != panel)
    {
        tab_containerp->selectTabPanel(panel);
    }
}

void LLFloaterPreference::setPanelVisibility(const LLSD& name, bool visible)
{
    LLTabContainer * tab_containerp = getChild<LLTabContainer>("pref core");
    LLPanel * panel = tab_containerp->getPanelByName(name.asStringRef());
    if (NULL != panel)
    {
        tab_containerp->setTabVisibility(panel, visible);
    }
}

void LLFloaterPreference::selectPrivacyPanel()
{
    selectPanel("im");
}

void LLFloaterPreference::selectChatPanel()
{
    selectPanel("chat");
}

void LLFloaterPreference::changed()
{
    if (LLConversationLog::instance().getIsLoggingEnabled())
    {
    getChild<LLButton>("clear_log")->setEnabled(LLConversationLog::instance().getConversations().size() > 0);
    }
    else
    {
        // onClearLog clears list, then notifies changed() and only then clears file,
        // so check presence of conversations before checking file, file will cleared later.
        llstat st;
        bool has_logs = LLConversationLog::instance().getConversations().size() > 0
                        && LLFile::stat(LLConversationLog::instance().getFileName(), &st) == 0
                        && S_ISREG(st.st_mode)
                        && st.st_size > 0;
        getChild<LLButton>("clear_log")->setEnabled(has_logs);
    }

    // set 'enable' property for 'Delete transcripts...' button
    updateDeleteTranscriptsButton();
}

void LLFloaterPreference::saveGraphicsPreset(std::string& preset)
{
    mSavedGraphicsPreset = preset;
}

//------------------------------Updater---------------------------------------

static bool handleBandwidthChanged(const LLSD& newvalue)
{
    gViewerThrottle.setMaxBandwidth((F32) newvalue.asReal());
    return true;
}

class LLPanelPreference::Updater : public LLEventTimer
{

public:

    typedef boost::function<bool(const LLSD&)> callback_t;

    Updater(callback_t cb, F32 period)
    :LLEventTimer(period),
     mCallback(cb)
    {
        mEventTimer.stop();
    }

    virtual ~Updater(){}

    void update(const LLSD& new_value)
    {
        mNewValue = new_value;
        mEventTimer.start();
    }

protected:

    bool tick()
    {
        mCallback(mNewValue);
        mEventTimer.stop();

        return false;
    }

private:

    LLSD mNewValue;
    callback_t mCallback;
};
//----------------------------------------------------------------------------
static LLPanelInjector<LLPanelPreference> t_places("panel_preference");
LLPanelPreference::LLPanelPreference()
: LLPanel(),
  mBandWidthUpdater(NULL)
{
    mCommitCallbackRegistrar.add("Pref.setControlFalse",    boost::bind(&LLPanelPreference::setControlFalse,this, _2));
    mCommitCallbackRegistrar.add("Pref.updateMediaAutoPlayCheckbox",    boost::bind(&LLPanelPreference::updateMediaAutoPlayCheckbox, this, _1));
    mCommitCallbackRegistrar.add("Pref.PrefDelete", boost::bind(&LLPanelPreference::deletePreset, this, _2));
    mCommitCallbackRegistrar.add("Pref.PrefSave",   boost::bind(&LLPanelPreference::savePreset, this, _2));
    mCommitCallbackRegistrar.add("Pref.PrefLoad",   boost::bind(&LLPanelPreference::loadPreset, this, _2));
}

//virtual
bool LLPanelPreference::postBuild()
{
    ////////////////////// PanelGeneral ///////////////////
    if (hasChild("display_names_check", true))
    {
        bool use_people_api = gSavedSettings.getBOOL("UsePeopleAPI");
        LLCheckBoxCtrl* ctrl_display_name = getChild<LLCheckBoxCtrl>("display_names_check");
        ctrl_display_name->setEnabled(use_people_api);
        if (!use_people_api)
        {
            ctrl_display_name->setValue(false);
        }
    }

    ////////////////////// PanelVoice ///////////////////
    if (hasChild("voice_unavailable", true))
    {
        bool voice_disabled = gSavedSettings.getBOOL("CmdLineDisableVoice");
        getChildView("voice_unavailable")->setVisible( voice_disabled);
        getChildView("enable_voice_check")->setVisible( !voice_disabled);
    }

    //////////////////////PanelPrivacy ///////////////////
    if (hasChild("media_enabled", true))
    {
        bool media_enabled = gSavedSettings.getBOOL("AudioStreamingMedia");
        getChild<LLCheckBoxCtrl>("media_enabled")->set(media_enabled);
        getChild<LLCheckBoxCtrl>("autoplay_enabled")->setEnabled(media_enabled);
    }
    if (hasChild("music_enabled", true))
    {
        getChild<LLCheckBoxCtrl>("music_enabled")->set(gSavedSettings.getBOOL("AudioStreamingMusic"));
    }
    if (hasChild("discord_enabled", true))
    {
        // Discord status (#205) needs a Discord application id, set per build.
        std::string discord_app_id = gSavedSettings.getString("SlipstreamDiscordAppID");
        LLStringUtil::trim(discord_app_id);
        if (discord_app_id.empty())
        {
            LLPanel* discord_panel = getChild<LLPanel>("privacy_preferences_discord");
            getChild<LLUICtrl>("discord_enabled")->setToolTip(discord_panel->getString("discord_unavailable"));
        }
    }
    if (hasChild("voice_call_friends_only_check", true))
    {
        getChild<LLCheckBoxCtrl>("voice_call_friends_only_check")->setCommitCallback(boost::bind(&showFriendsOnlyWarning, _1, _2));
    }
    if (hasChild("allow_multiple_viewer_check", true))
    {
        getChild<LLCheckBoxCtrl>("allow_multiple_viewer_check")->setCommitCallback(boost::bind(&showMultipleViewersWarning, _1, _2));
    }
    if (hasChild("favorites_on_login_check", true))
    {
        getChild<LLCheckBoxCtrl>("favorites_on_login_check")->setCommitCallback(boost::bind(&handleFavoritesOnLoginChanged, _1, _2));
        bool show_favorites_at_login = LLPanelLogin::getShowFavorites();
        getChild<LLCheckBoxCtrl>("favorites_on_login_check")->setValue(show_favorites_at_login);
    }
    if (hasChild("mute_chb_label", true))
    {
        getChild<LLTextBox>("mute_chb_label")->setShowCursorHand(false);
        getChild<LLTextBox>("mute_chb_label")->setSoundFlags(LLView::MOUSE_UP);
        getChild<LLTextBox>("mute_chb_label")->setClickedCallback(boost::bind(&toggleMuteWhenMinimized));
    }

    //////////////////////PanelSetup ///////////////////
    if (hasChild("max_bandwidth", true))
    {
        mBandWidthUpdater = new LLPanelPreference::Updater(boost::bind(&handleBandwidthChanged, _1), BANDWIDTH_UPDATER_TIMEOUT);
        gSavedSettings.getControl("ThrottleBandwidthKBPS")->getSignal()->connect(boost::bind(&LLPanelPreference::Updater::update, mBandWidthUpdater, _2));
    }

#ifdef EXTERNAL_TOS
    LLRadioGroup* ext_browser_settings = getChild<LLRadioGroup>("preferred_browser_behavior");
    if (ext_browser_settings)
    {
        // turn off ability to set external/internal browser
        ext_browser_settings->setSelectedByValue(LLWeb::BROWSER_EXTERNAL_ONLY, true);
        ext_browser_settings->setEnabled(false);
    }
#endif

    return true;
}

LLPanelPreference::~LLPanelPreference()
{
    if (mBandWidthUpdater)
    {
        delete mBandWidthUpdater;
    }
}

// virtual
void LLPanelPreference::apply()
{
    // no-op
}

// virtual
void LLPanelPreference::saveSettings()
{
    // Save the value of all controls in the hierarchy
    mSavedValues.clear();
    std::list<LLView*> view_stack;
    view_stack.push_back(this);
    // <GMDX> (no 'Advanced' graphics floater: its settings are in the Graphics panel's tree)

    while (!view_stack.empty())
    {
        // Process view on top of the stack
        LLView* curview = view_stack.front();
        view_stack.pop_front();

        if (LLColorSwatchCtrl* color_swatch = dynamic_cast<LLColorSwatchCtrl*>(curview))
        {
            mSavedColors[color_swatch->getName()] = color_swatch->get();
        }
        else
        {
            if (LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(curview))
            {
                if (LLControlVariable* control = ctrl->getControlVariable())
                {
                    mSavedValues[control] = control->getValue();
                }
            }
        }

        // Push children onto the end of the work stack
        for (LLView* view : *curview->getChildList())
        {
            view_stack.push_back(view);
        }
    }

    if (LLStartUp::getStartupState() == STATE_STARTED)
    {
        LLControlVariable* control = gSavedPerAccountSettings.getControl("VoiceCallsFriendsOnly");
        if (control)
        {
            mSavedValues[control] = control->getValue();
        }
    }
}

void LLPanelPreference::showMultipleViewersWarning(LLUICtrl* checkbox, const LLSD& value)
{
    // No warning: a second instance opens the texture and object caches read-only and
    // skips the crash / logout markers, and our own caches (pipeline cache, texture
    // compression facts) are written atomically per process -- running several viewers
    // is supported. (The inherited "texture cache collisions, corruption" alert was stale.)
    (void)checkbox;
    (void)value;
}

void LLPanelPreference::showFriendsOnlyWarning(LLUICtrl* checkbox, const LLSD& value)
{
    if (checkbox)
    {
        gSavedPerAccountSettings.setBOOL("VoiceCallsFriendsOnly", checkbox->getValue().asBoolean());
        if (checkbox->getValue())
        {
            LLNotificationsUtil::add("FriendsAndGroupsOnly");
        }
    }
}

void LLPanelPreference::handleFavoritesOnLoginChanged(LLUICtrl* checkbox, const LLSD& value)
{
    if (checkbox)
    {
        LLFavoritesOrderStorage::instance().showFavoritesOnLoginChanged(checkbox->getValue().asBoolean());
        if(checkbox->getValue())
        {
            LLNotificationsUtil::add("FavoritesOnLogin");
        }
    }
}

void LLPanelPreference::toggleMuteWhenMinimized()
{
    std::string mute("MuteWhenMinimized");
    gSavedSettings.setBOOL(mute, !gSavedSettings.getBOOL(mute));
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->getChild<LLCheckBoxCtrl>("mute_when_minimized")->setBtnFocus();
    }
}

void LLPanelPreference::cancel(const std::vector<std::string> settings_to_skip)
{
    for (control_values_map_t::iterator iter =  mSavedValues.begin();
         iter !=  mSavedValues.end(); ++iter)
    {
        LLControlVariable* control = iter->first;
        LLSD ctrl_value = iter->second;

        if((control->getName() == "InstantMessageLogPath") && (ctrl_value.asString() == ""))
        {
            continue;
        }

        auto found = std::find(settings_to_skip.begin(), settings_to_skip.end(), control->getName());
        if (found != settings_to_skip.end())
        {
            continue;
        }

        control->set(ctrl_value);
    }

    for (string_color_map_t::iterator iter = mSavedColors.begin();
         iter != mSavedColors.end(); ++iter)
    {
        LLColorSwatchCtrl* color_swatch = findChild<LLColorSwatchCtrl>(iter->first);
        if (color_swatch)
        {
            color_swatch->set(iter->second);
            color_swatch->onCommit();
        }
    }
}

void LLPanelPreference::setControlFalse(const LLSD& user_data)
{
    std::string control_name = user_data.asString();
    LLControlVariable* control = findControl(control_name);

    if (control)
        control->set(LLSD(false));
}

void LLPanelPreference::updateMediaAutoPlayCheckbox(LLUICtrl* ctrl)
{
    std::string name = ctrl->getName();

    // Disable "Allow Media to auto play" only when both
    // "Streaming Music" and "Media" are unchecked. STORM-513.
    if ((name == "enable_music") || (name == "enable_media"))
    {
        bool music_enabled = getChild<LLCheckBoxCtrl>("enable_music")->get();
        bool media_enabled = getChild<LLCheckBoxCtrl>("enable_media")->get();

        getChild<LLCheckBoxCtrl>("media_auto_play_combo")->setEnabled(music_enabled || media_enabled);
    }
}

void LLPanelPreference::deletePreset(const LLSD& user_data)
{
    LLFloaterReg::showInstance("delete_pref_preset", user_data.asString());
}

void LLPanelPreference::savePreset(const LLSD& user_data)
{
    LLFloaterReg::showInstance("save_pref_preset", user_data.asString());
}

void LLPanelPreference::loadPreset(const LLSD& user_data)
{
    LLFloaterReg::showInstance("load_pref_preset", user_data.asString());
}

void LLPanelPreference::setHardwareDefaults()
{
}

class LLPanelPreferencePrivacy : public LLPanelPreference
{
public:
    LLPanelPreferencePrivacy()
    {
        mAccountIndependentSettings.push_back("AutoDisengageMic");
        // Discord status (#205): viewer-wide settings, so Cancel must restore
        // them even when not logged in.
        mAccountIndependentSettings.push_back("SlipstreamDiscordEnabled");
        mAccountIndependentSettings.push_back("SlipstreamDiscordShowRegion");
        mAccountIndependentSettings.push_back("SlipstreamDiscordShowActivity");
        mAccountIndependentSettings.push_back("SlipstreamDiscordShowName");
        mAccountIndependentSettings.push_back("SlipstreamDiscordShowJoin");
        mAccountIndependentSettings.push_back("SlipstreamDiscordHideMatureLocation");
    }

    void saveSettings() override
    {
        LLPanelPreference::saveSettings();

        // Don't save (=erase from the saved values map) per-account privacy settings
        // if we're not logged in, otherwise they will be reset to defaults on log off.
        if (LLStartUp::getStartupState() != STATE_STARTED)
        {
            // Erase only common settings, assuming there are no color settings on Privacy page.
            for (control_values_map_t::iterator it = mSavedValues.begin(); it != mSavedValues.end(); )
            {
                const std::string setting = it->first->getName();
                if (find(mAccountIndependentSettings.begin(),
                    mAccountIndependentSettings.end(), setting) == mAccountIndependentSettings.end())
                {
                    it = mSavedValues.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

private:
    std::list<std::string> mAccountIndependentSettings;
};

static LLPanelInjector<LLPanelPreferenceGraphics> t_pref_graph("panel_preference_graphics");
static LLPanelInjector<LLPanelPreferencePrivacy> t_pref_privacy("panel_preference_privacy");

bool LLPanelPreferenceGraphics::postBuild()
{
    // <GMDX> (no Advanced Graphics floater to build: its settings are this panel's tabs)
    resetDirtyChilds();
    setPresetText();

    LLPresetsManager* presetsMgr = LLPresetsManager::getInstance();
    presetsMgr->setPresetListChangeCallback(boost::bind(&LLPanelPreferenceGraphics::onPresetsListChange, this));
    presetsMgr->createMissingDefault(PRESETS_GRAPHIC); // a no-op after the first time, but that's ok

    // Effects quick access (#545). Not bound with control_name (see the XUI):
    // the switch writes SLFxEnabled, the preset combo SLFxActivePreset, which
    // SLFxPresets loads on change; draw() keeps both in step. The list is read
    // whenever it opens, so presets added since show up. Applied at once,
    // like the World menu item and the key: Cancel does not revert them.
    if (LLUICtrl* fx_enabled = findChild<LLUICtrl>("FxEnabled"))
    {
        fx_enabled->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            if (LLControlVariable* controlp = gSavedSettings.getControl("SLFxEnabled"))
            {
                SLFxPresets::instance().init();
                controlp->setValue(LLSD(ctrl->getValue().asBoolean()));
            }
        });
    }
    if (LLComboBox* fx_presets = findChild<LLComboBox>("FxPresetCombo"))
    {
        SLFxPresets::instance().init();
        auto fill = [fx_presets](LLUICtrl*, const LLSD&)
        {
            fx_presets->removeall();
            for (const std::string& name : SLFxPresets::instance().listPresets())
            {
                fx_presets->add(name, LLSD(name));
            }
            fx_presets->selectByValue(LLSD(gSavedSettings.getString("SLFxActivePreset")));
        };
        fill(nullptr, LLSD());
        fx_presets->setPrearrangeCallback(fill);
        fx_presets->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            const std::string name = ctrl->getValue().asString();
            if (!name.empty())
            {
                gSavedSettings.setString("SLFxActivePreset", name);
            }
        });
    }

    return LLPanelPreference::postBuild();
}

void LLPanelPreferenceGraphics::draw()
{
    // The effects row is not bound to its settings (see postBuild): follow
    // them here, since the key, the World menu and Photo Studio change them too.
    if (LLUICtrl* fx_enabled = findChild<LLUICtrl>("FxEnabled"))
    {
        const LLControlVariable* controlp = gSavedSettings.getControl("SLFxEnabled");
        const bool on = controlp && controlp->getValue().asBoolean();
        fx_enabled->setEnabled(controlp != nullptr);
        if (fx_enabled->getValue().asBoolean() != on)
        {
            fx_enabled->setValue(LLSD(on));
        }
    }
    if (LLComboBox* fx_presets = findChild<LLComboBox>("FxPresetCombo"))
    {
        const std::string active = gSavedSettings.getString("SLFxActivePreset");
        if (!fx_presets->hasFocus() && fx_presets->getValue().asString() != active)
        {
            fx_presets->selectByValue(LLSD(active));
        }
    }
    LLPanelPreference::draw();
}

void LLPanelPreferenceGraphics::onPresetsListChange()
{
    resetDirtyChilds();
    setPresetText();

    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance && !gSavedSettings.getString("PresetGraphicActive").empty())
    {
        instance->saveSettings(); //make cancel work correctly after changing the preset
    }
}

void LLPanelPreferenceGraphics::setPresetText()
{
    LLTextBox* preset_text = getChild<LLTextBox>("preset_text");

    std::string preset_graphic_active = gSavedSettings.getString("PresetGraphicActive");

    if (!preset_graphic_active.empty() && preset_graphic_active != preset_text->getText())
    {
        LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
        if (instance)
        {
            instance->saveGraphicsPreset(preset_graphic_active);
        }
    }

    if (hasDirtyChilds() && !preset_graphic_active.empty())
    {
        gSavedSettings.setString("PresetGraphicActive", "");
        preset_graphic_active.clear();
        // This doesn't seem to cause an infinite recursion.  This trigger is needed to cause the pulldown
        // panel to update.
        LLPresetsManager::getInstance()->triggerChangeSignal();
    }

    if (!preset_graphic_active.empty())
    {
        if (preset_graphic_active == PRESETS_DEFAULT)
        {
            preset_graphic_active = LLTrans::getString(PRESETS_DEFAULT);
        }
        preset_text->setText(preset_graphic_active);
    }
    else
    {
        preset_text->setText(LLTrans::getString("none_paren_cap"));
    }

    preset_text->resetDirty();
}

bool LLPanelPreferenceGraphics::hasDirtyChilds()
{
    // <GMDX> (the advanced graphics settings are in this panel's tree)
    std::list<LLView*> view_stack;
    view_stack.push_back(this);
    while(!view_stack.empty())
    {
        // Process view on top of the stack
        LLView* curview = view_stack.front();
        view_stack.pop_front();

        LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(curview);
        if (ctrl)
        {
            if (ctrl->isDirty())
            {
                LLControlVariable* control = ctrl->getControlVariable();
                if (control)
                {
                    std::string control_name = control->getName();
                    if (!control_name.empty())
                    {
                        return true;
                    }
                }
            }
        }
        // Push children onto the end of the work stack
        for (child_list_t::const_iterator iter = curview->getChildList()->begin();
             iter != curview->getChildList()->end(); ++iter)
        {
            view_stack.push_back(*iter);
        }
    }

    return false;
}

void LLPanelPreferenceGraphics::resetDirtyChilds()
{
    // <GMDX> (the advanced graphics settings are in this panel's tree)
    std::list<LLView*> view_stack;
    view_stack.push_back(this);
    while(!view_stack.empty())
    {
        // Process view on top of the stack
        LLView* curview = view_stack.front();
        view_stack.pop_front();

        LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(curview);
        if (ctrl)
        {
            ctrl->resetDirty();
        }
        // Push children onto the end of the work stack
        for (child_list_t::const_iterator iter = curview->getChildList()->begin();
             iter != curview->getChildList()->end(); ++iter)
        {
            view_stack.push_back(*iter);
        }
    }
}

void LLPanelPreferenceGraphics::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);
}

void LLPanelPreferenceGraphics::saveSettings()
{
    resetDirtyChilds();

    std::string preset_graphic_active = gSavedSettings.getString("PresetGraphicActive");
    if (preset_graphic_active.empty())
    {
        LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
        if (instance)
        {
            //don't restore previous preset after closing Preferences
            instance->saveGraphicsPreset(preset_graphic_active);
        }
    }

    LLPanelPreference::saveSettings();
}

void LLPanelPreferenceGraphics::setHardwareDefaults()
{
    resetDirtyChilds();
}

//------------------------LLPanelPreferenceControls--------------------------------
static LLPanelInjector<LLPanelPreferenceControls> t_pref_contrls("panel_preference_controls");

LLPanelPreferenceControls::LLPanelPreferenceControls()
    :LLPanelPreference(),
    pControlsTable(NULL),
    pKeyModeBox(NULL),
    pCommandGroupBox(NULL),
    mEditingColumn(-1),
    mEditingMode(0)
{
    // MODE_COUNT - 1 because there are currently no settings assigned to 'saved settings'.
    for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
    {
        mConflictHandler[i].setLoadMode((LLKeyConflictHandler::ESourceMode)i);
    }
}

LLPanelPreferenceControls::~LLPanelPreferenceControls()
{
}

void LLPanelPreferenceControls::refresh()
{
    populateControlTable();
    LLPanelPreference::refresh();
}

bool LLPanelPreferenceControls::postBuild()
{
    // populate list of controls
    pControlsTable = getChild<LLScrollListCtrl>("controls_list");
    pKeyModeBox = getChild<LLComboBox>("key_mode");

    pControlsTable->setCommitCallback(boost::bind(&LLPanelPreferenceControls::onListCommit, this));
    pKeyModeBox->setCommitCallback(boost::bind(&LLPanelPreferenceControls::onModeCommit, this));
    getChild<LLButton>("restore_defaults")->setCommitCallback(boost::bind(&LLPanelPreferenceControls::onRestoreDefaultsBtn, this));

    pCommandGroupBox = getChild<LLComboBox>("command_group");
    pCommandGroupBox->setCommitCallback(boost::bind(&LLPanelPreferenceControls::onCommandGroupCommit, this));
    getChild<LLButton>("reset_group")->setCommitCallback(boost::bind(&LLPanelPreferenceControls::onResetGroupBtn, this));

    return true;
}

void LLPanelPreferenceControls::regenerateControls()
{
    mEditingMode = pKeyModeBox->getValue().asInteger();
    mConflictHandler[mEditingMode].loadFromSettings((LLKeyConflictHandler::ESourceMode)mEditingMode);
    populateControlTable();
}

bool LLPanelPreferenceControls::addControlTableColumns(const std::string &filename)
{
    LLXMLNodePtr xmlNode;
    LLScrollListCtrl::Contents contents;
    if (!LLUICtrlFactory::getLayeredXMLNode(filename, xmlNode))
    {
        LL_WARNS("Preferences") << "Failed to load " << filename << LL_ENDL;
        return false;
    }
    LLXUIParser parser;
    parser.readXUI(xmlNode, contents, filename);

    if (!contents.validateBlock())
    {
        return false;
    }

    for (LLInitParam::ParamIterator<LLScrollListColumn::Params>::const_iterator col_it = contents.columns.begin();
        col_it != contents.columns.end();
        ++col_it)
    {
        pControlsTable->addColumn(*col_it);
    }

    return true;
}

bool LLPanelPreferenceControls::addControlTableRows(const std::string &filename)
{
    LLXMLNodePtr xmlNode;
    LLScrollListCtrl::Contents contents;
    if (!LLUICtrlFactory::getLayeredXMLNode(filename, xmlNode))
    {
        LL_WARNS("Preferences") << "Failed to load " << filename << LL_ENDL;
        return false;
    }
    LLXUIParser parser;
    parser.readXUI(xmlNode, contents, filename);

    if (!contents.validateBlock())
    {
        return false;
    }

    LLScrollListCell::Params cell_params;
    // init basic cell params
    cell_params.font = LLFontGL::getFontSansSerif();
    cell_params.font_halign = LLFontGL::LEFT;
    cell_params.column = "";
    cell_params.value = "";

    for (LLScrollListItem::Params& row_params : contents.rows)
    {
        std::string control = row_params.value.getValue().asString();
        if (!control.empty() && control != "menu_separator")
        {
            bool show = true;
            bool enabled = mConflictHandler[mEditingMode].canAssignControl(control);
            if (!enabled)
            {
                // If empty: this is a placeholder to make sure user won't assign
                // value by accident, don't show it
                // If not empty: predefined control combination user should see
                // to know that combination is reserved
                show = !mConflictHandler[mEditingMode].isControlEmpty(control);
                // example: teleport_to and walk_to in first person view, and
                // sitting related functions, see generatePlaceholders()
            }

            if (show)
            {
                // At the moment viewer is hardcoded to assume that columns are named as lst_ctrl%d
                LLScrollListItem::Params item_params(row_params);
                item_params.enabled.setValue(enabled);

                S32 num_columns = pControlsTable->getNumColumns();
                for (S32 col = 1; col < num_columns; col++)
                {
                    cell_params.column = llformat("lst_ctrl%d", col);
                    cell_params.value = mConflictHandler[mEditingMode].getControlString(control, col - 1);
                    item_params.columns.add(cell_params);
                }
                pControlsTable->addRow(item_params, EAddPosition::ADD_BOTTOM);
            }
        }
        else
        {
            // Separator example:
            // <rows
            //  enabled = "false">
            //  <columns
            //   type = "icon"
            //   color = "0 0 0 0.7"
            //   halign = "center"
            //   value = "menu_separator"
            //   column = "lst_action" / >
            //</rows>
            pControlsTable->addRow(row_params, EAddPosition::ADD_BOTTOM);
        }
    }
    return true;
}

void LLPanelPreferenceControls::addControlTableSeparator()
{
    LLScrollListItem::Params separator_params;
    separator_params.enabled(false);
    LLScrollListCell::Params column_params;
    column_params.type = "icon";
    column_params.value = "menu_separator";
    column_params.column = "lst_action";
    column_params.color = LLColor4(0.f, 0.f, 0.f, 0.7f);
    column_params.font_halign = LLFontGL::HCENTER;
    separator_params.columns.add(column_params);
    pControlsTable->addRow(separator_params, EAddPosition::ADD_BOTTOM);
}

void LLPanelPreferenceControls::populateControlTable()
{
    updateCommandControls();
    // The mode combo is the source of truth for which bindings to display.
    // Keep mEditingMode in sync with it and make sure that mode's bindings are
    // loaded before we render; otherwise the table shows empty cells on first
    // open (refresh() populates before any load has happened, and mEditingMode
    // may not match the mode currently selected in the combo).
    if (pKeyModeBox)
    {
        mEditingMode = pKeyModeBox->getValue().asInteger();
    }
    if (mEditingMode >= 0
        && mEditingMode < (S32)LLKeyConflictHandler::MODE_COUNT
        && mConflictHandler[mEditingMode].empty())
    {
        mConflictHandler[mEditingMode].loadFromSettings((LLKeyConflictHandler::ESourceMode)mEditingMode);
    }

    pControlsTable->clearRows();
    pControlsTable->clearColumns();

    // Add columns
    std::string filename;
    switch ((LLKeyConflictHandler::ESourceMode)mEditingMode)
    {
    case LLKeyConflictHandler::MODE_THIRD_PERSON:
    case LLKeyConflictHandler::MODE_FIRST_PERSON:
    case LLKeyConflictHandler::MODE_EDIT_AVATAR:
    case LLKeyConflictHandler::MODE_SITTING:
    case LLKeyConflictHandler::MODE_FLYCAM:
        filename = "control_table_contents_columns_basic.xml";
        break;
    case LLKeyConflictHandler::MODE_COMMANDS:
        // one shortcut per command, menu items have a single accelerator
        filename = "control_table_contents_columns_commands.xml";
        break;
    default:
        {
            // Either unknown mode or MODE_SAVED_SETTINGS
            // It doesn't have UI or actual settings yet
            LL_WARNS("Preferences") << "Unimplemented mode" << LL_ENDL;

            // Searchable columns were removed, mark searchables for an update
            LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
            if (instance)
            {
                instance->updateSearchableItems();
            }
            return;
        }
    }
    addControlTableColumns(filename);

    // Add rows.
    // Each file represents individual visual group (movement/camera/media...)
    if (mEditingMode == LLKeyConflictHandler::MODE_FIRST_PERSON)
    {
        // Don't display whole camera and editing groups
        addControlTableRows("control_table_contents_movement.xml");
        addControlTableSeparator();
        addControlTableRows("control_table_contents_media.xml");
        addControlTableSeparator();
        addControlTableRows("control_table_contents_game_control.xml");
    }
    else if (mEditingMode == LLKeyConflictHandler::MODE_FLYCAM)
    {
        // Flycam has its own dedicated set of actions; camera/editing/media
        // groups don't apply to it.
        addControlTableRows("control_table_contents_flycam.xml");
    }
    else if (mEditingMode == LLKeyConflictHandler::MODE_COMMANDS)
    {
        addCommandTableRows();
    }
    // MODE_THIRD_PERSON; MODE_EDIT_AVATAR; MODE_SITTING
    else if (mEditingMode < LLKeyConflictHandler::MODE_COMMANDS)
    {
        // In case of 'sitting' mode, movements still apply due to vehicles
        // but walk_to is not supported and will be hidden by addControlTableRows
        addControlTableRows("control_table_contents_movement.xml");
        addControlTableSeparator();

        addControlTableRows("control_table_contents_camera.xml");
        addControlTableSeparator();

        addControlTableRows("control_table_contents_editing.xml");
        addControlTableSeparator();

        addControlTableRows("control_table_contents_media.xml");
        addControlTableSeparator();

        addControlTableRows("control_table_contents_game_control.xml");
    }
    else
    {
        LL_WARNS("Preferences") << "Unimplemented mode" << LL_ENDL;
    }

    // explicit update to make sure table is ready for llsearchableui
    pControlsTable->updateColumns();

    // Searchable columns were removed and readded, mark searchables for an update
    // Note: at the moment tables/lists lack proper llsearchableui support
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->updateSearchableItems();
    }
}

// Menu and other shortcuts (#863)

void LLPanelPreferenceControls::updateCommandControls()
{
    const bool commands = mEditingMode == LLKeyConflictHandler::MODE_COMMANDS;
    pCommandGroupBox->setVisible(commands);
    getChildView("reset_group")->setVisible(commands);

    if (commands && pCommandGroupBox->getItemCount() == 0)
    {
        pCommandGroupBox->add(getString("all_groups"), LLSD(std::string()));
        for (const std::string& group : SLKeyCommands::instance().getGroups())
        {
            pCommandGroupBox->add(group, LLSD(group));
        }
        pCommandGroupBox->selectFirstItem();
    }
    getChildView("reset_group")->setEnabled(commands && !pCommandGroupBox->getValue().asString().empty());

    // the group row sits between the mode box and the list
    LLRect rect = pControlsTable->getRect();
    const S32 top = getRect().getHeight() - (commands ? 57 : 31);
    if (rect.mTop != top)
    {
        rect.mTop = top;
        pControlsTable->setShape(rect);
    }
}

void LLPanelPreferenceControls::addCommandTableRows()
{
    LLKeyConflictHandler& handler = mConflictHandler[mEditingMode];
    const std::string shown_group = pCommandGroupBox->getValue().asString();

    LLScrollListCell::Params cell_params;
    cell_params.font = LLFontGL::getFontSansSerif();
    cell_params.font_halign = LLFontGL::LEFT;

    std::string current_group;
    bool any_row = false;
    for (const SLKeyCommand& command : SLKeyCommands::instance().getCommands())
    {
        if (command.mGroup == SLKeyCommands::instance().getFixedGroup())
        {
            continue;
        }
        if (!shown_group.empty() && command.mGroup != shown_group)
        {
            continue;
        }
        if (command.mGroup != current_group)
        {
            if (any_row)
            {
                addControlTableSeparator();
            }
            LLScrollListItem::Params header_params;
            header_params.enabled(false);
            LLScrollListCell::Params header_cell;
            header_cell.column = "lst_action";
            header_cell.value = command.mGroup;
            header_cell.font = LLFontGL::getFontSansSerifBold();
            header_cell.font_halign = LLFontGL::LEFT;
            header_params.columns.add(header_cell);
            pControlsTable->addRow(header_params, EAddPosition::ADD_BOTTOM);
            current_group = command.mGroup;
        }

        LLScrollListItem::Params item_params;
        item_params.value = LLSD(command.mId);
        cell_params.column = "lst_action";
        cell_params.value = command.mLabel;
        item_params.columns.add(cell_params);
        cell_params.column = "lst_ctrl1";
        cell_params.value = handler.getControlString(command.mId, 0);
        item_params.columns.add(cell_params);
        pControlsTable->addRow(item_params, EAddPosition::ADD_BOTTOM);
        any_row = true;
    }

    // Keys that cannot be rebound, listed so that every shortcut is documented here
    SLKeyCommands& registry = SLKeyCommands::instance();
    if (!registry.getFixedKeys().empty() && (shown_group.empty() || shown_group == registry.getFixedGroup()))
    {
        if (any_row)
        {
            addControlTableSeparator();
        }
        LLScrollListItem::Params header_params;
        header_params.enabled(false);
        LLScrollListCell::Params header_cell;
        header_cell.column = "lst_action";
        header_cell.value = registry.getFixedGroup();
        header_cell.font = LLFontGL::getFontSansSerifBold();
        header_cell.font_halign = LLFontGL::LEFT;
        header_params.columns.add(header_cell);
        pControlsTable->addRow(header_params, EAddPosition::ADD_BOTTOM);

        for (const SLFixedKey& fixed : registry.getFixedKeys())
        {
            LLScrollListItem::Params item_params;
            item_params.enabled(false);
            cell_params.column = "lst_action";
            cell_params.value = fixed.mLabel;
            item_params.columns.add(cell_params);
            cell_params.column = "lst_ctrl1";
            cell_params.value = fixed.mKeys;
            item_params.columns.add(cell_params);
            pControlsTable->addRow(item_params, EAddPosition::ADD_BOTTOM);
        }
    }
}

void LLPanelPreferenceControls::onCommandGroupCommit()
{
    populateControlTable();
}

void LLPanelPreferenceControls::onResetGroupBtn()
{
    const std::string group = pCommandGroupBox->getValue().asString();
    if (mEditingMode != LLKeyConflictHandler::MODE_COMMANDS || group.empty())
    {
        return;
    }
    mConflictHandler[mEditingMode].resetCommandGroup(group);
    // Apply changes to viewer as 'temporary'
    mConflictHandler[mEditingMode].saveToSettings(true);
    updateTable();
}

void LLPanelPreferenceControls::showCommandKeyNotices(const std::string& id, const LLKeyData& data)
{
    if (SLKeyCommands::isDiscouraged(data))
    {
        LLSD args;
        args["KEY"] = LLKeyConflictHandler::getStringFromKeyData(data);
        LLNotificationsUtil::add("KeyCommandAltShift", args);
    }
}

void LLPanelPreferenceControls::applyCommandKey(const std::string& id, const LLKeyData& data, S32 conflict_action, const std::vector<std::string>& others)
{
    LLKeyConflictHandler& handler = mConflictHandler[mEditingMode];
    const LLKeyData old_data = handler.getControl(id, 0);

    for (const std::string& other : others)
    {
        if (conflict_action == 1 && !old_data.isEmpty())
        {
            // swap: the other command gets the key this one had
            handler.registerControl(other, 0, CLICK_NONE, old_data.mKey, old_data.mMask, true);
        }
        else
        {
            handler.clearControl(other, 0);
        }
    }

    if (data.mKey == KEY_NONE)
    {
        handler.clearControl(id, 0);
    }
    else
    {
        handler.registerControl(id, 0, CLICK_NONE, data.mKey, data.mMask, true);
    }
    // Apply changes to viewer as 'temporary', the menus show the new key at once
    handler.saveToSettings(true);
    updateTable();

    if (data.mKey != KEY_NONE)
    {
        showCommandKeyNotices(id, data);
    }
}

bool LLPanelPreferenceControls::onSetCommandKeyBind(KEY key, MASK mask)
{
    if (mEditingControl.empty())
    {
        return true;
    }
    if (key == KEY_NONE && mask != MASK_NONE)
    {
        // modifiers alone are not a shortcut
        return true;
    }

    LLKeyConflictHandler& handler = mConflictHandler[mEditingMode];
    const LLKeyData data(CLICK_NONE, key, mask, true);
    const SLKeyCommand* command = SLKeyCommands::instance().findCommand(mEditingControl);
    if (!command)
    {
        return true;
    }

    if (key == KEY_NONE)
    {
        // cleared
        applyCommandKey(mEditingControl, data, 0, std::vector<std::string>());
        return true;
    }

    mPendingCommand = mEditingControl;
    mPendingCommandKey = data;
    mPendingCommandOthers = handler.findCommandConflicts(mEditingControl, data);

    LLSD args;
    args["KEY"] = LLKeyConflictHandler::getStringFromKeyData(data);
    args["COMMAND"] = command->mLabel;

    if (!mPendingCommandOthers.empty())
    {
        std::string others;
        for (const std::string& other : mPendingCommandOthers)
        {
            const SLKeyCommand* other_command = SLKeyCommands::instance().findCommand(other);
            if (!others.empty())
            {
                others += ", ";
            }
            others += "\"" + (other_command ? other_command->mLabel : other) + "\"";
        }
        const LLKeyData old_data = handler.getControl(mEditingControl, 0);
        args["OTHERS"] = others;
        args["OLDKEY"] = old_data.isEmpty() ? getString("no_shortcut") : LLKeyConflictHandler::getStringFromKeyData(old_data);
        LLNotificationsUtil::add("KeyCommandConflict", args, LLSD(), boost::bind(&LLPanelPreferenceControls::onCommandConflictResponse, this, _1, _2));
        return true;
    }

    const std::string movement = SLKeyCommands::instance().findMovementConflict(data);
    if (!movement.empty())
    {
        // the name in the Controls panel is the action's own row label
        args["ACTION"] = movement;
        LLNotificationsUtil::add("KeyCommandMovementConflict", args, LLSD(), boost::bind(&LLPanelPreferenceControls::onCommandMovementResponse, this, _1, _2));
        return true;
    }

    applyCommandKey(mEditingControl, data, 0, std::vector<std::string>());
    return true;
}

void LLPanelPreferenceControls::onCommandConflictResponse(const LLSD& notification, const LLSD& response)
{
    const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option == 0 || option == 1)
    {
        // 0: swap, 1: clear the other commands
        applyCommandKey(mPendingCommand, mPendingCommandKey, option == 0 ? 1 : 2, mPendingCommandOthers);
    }
    else
    {
        updateTable();
    }
}

void LLPanelPreferenceControls::onCommandMovementResponse(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        applyCommandKey(mPendingCommand, mPendingCommandKey, 0, std::vector<std::string>());
    }
    else
    {
        updateTable();
    }
}

void LLPanelPreferenceControls::updateTable()
{
    mEditingControl.clear();
    std::vector<LLScrollListItem*> list = pControlsTable->getAllData();
    for (S32 i = 0; i < list.size(); ++i)
    {
        std::string control = list[i]->getValue();
        if (!control.empty())
        {
            LLScrollListCell* cell = NULL;

            S32 num_columns = pControlsTable->getNumColumns();
            for (S32 col = 1; col < num_columns; col++)
            {
                cell = list[i]->getColumn(col);
                cell->setValue(mConflictHandler[mEditingMode].getControlString(control, col - 1));
            }
        }
    }
    pControlsTable->deselectAllItems();
}

void LLPanelPreferenceControls::apply()
{
    for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
    {
        if (mConflictHandler[i].hasUnsavedChanges())
        {
            mConflictHandler[i].saveToSettings();
        }
    }
}

void LLPanelPreferenceControls::cancel(const std::vector<std::string> settings_to_skip)
{
    for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
    {
        if (mConflictHandler[i].hasUnsavedChanges())
        {
            mConflictHandler[i].clear();
            if (mEditingMode == i)
            {
                // cancel() can be called either when preferences floater closes
                // or when child floater closes (like advanced graphical settings)
                // in which case we need to clear and repopulate table
                regenerateControls();
            }
        }
    }
}

void LLPanelPreferenceControls::saveSettings()
{
    for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
    {
        if (mConflictHandler[i].hasUnsavedChanges())
        {
            mConflictHandler[i].saveToSettings();
            mConflictHandler[i].clear();
        }
    }

    S32 mode = pKeyModeBox->getValue().asInteger();
    if (mConflictHandler[mode].empty() || pControlsTable->isEmpty())
    {
        regenerateControls();
    }
}

void LLPanelPreferenceControls::resetDirtyChilds()
{
    regenerateControls();
}

void LLPanelPreferenceControls::onListCommit()
{
    LLScrollListItem* item = pControlsTable->getFirstSelected();
    if (item == NULL)
    {
        return;
    }

    std::string control = item->getValue();

    if (control.empty())
    {
        pControlsTable->deselectAllItems();
        return;
    }

    if (!mConflictHandler[mEditingMode].canAssignControl(control))
    {
        pControlsTable->deselectAllItems();
        return;
    }

    S32 cell_ind = item->getSelectedCell();
    if (cell_ind <= 0)
    {
        pControlsTable->deselectAllItems();
        return;
    }

    // List does not tell us what cell was clicked, so we have to figure it out manually, but
    // fresh mouse coordinates are not yet accessible during onCommit() and there are other issues,
    // so we cheat: remember item user clicked at, trigger 'key dialog' on hover that comes next,
    // use coordinates from hover to calculate cell

    LLScrollListCell* cell = item->getColumn(cell_ind);
    if (cell)
    {
        LLSetKeyBindDialog* dialog = LLFloaterReg::getTypedInstance<LLSetKeyBindDialog>("keybind_dialog", LLSD());
        if (dialog)
        {
            mEditingControl = control;
            mEditingColumn = cell_ind;
            dialog->setParent(this, pControlsTable,
                mEditingMode == LLKeyConflictHandler::MODE_COMMANDS ? (ALLOW_KEYS | ALLOW_MASK_KEYS) : DEFAULT_KEY_FILTER);

            LLFloater* root_floater = gFloaterView->getParentFloater(this);
            if (root_floater)
                root_floater->addDependentFloater(dialog);
            dialog->openFloater();
            dialog->setFocus(true);
        }
    }
    else
    {
        pControlsTable->deselectAllItems();
    }
}

void LLPanelPreferenceControls::onModeCommit()
{
    mEditingMode = pKeyModeBox->getValue().asInteger();
    if (mConflictHandler[mEditingMode].empty())
    {
        // opening for first time
        mConflictHandler[mEditingMode].loadFromSettings((LLKeyConflictHandler::ESourceMode)mEditingMode);
    }
    populateControlTable();
}

void LLPanelPreferenceControls::onRestoreDefaultsBtn()
{
    LLNotificationsUtil::add("PreferenceControlsDefaults", LLSD(), LLSD(), boost::bind(&LLPanelPreferenceControls::onRestoreDefaultsResponse, this, _1, _2));
}

void LLPanelPreferenceControls::onRestoreDefaultsResponse(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    switch(option)
    {
    case 0: // All
        for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
        {
            mConflictHandler[i].resetToDefaults();
            // Apply changes to viewer as 'temporary'
            mConflictHandler[i].saveToSettings(true);

            // notify comboboxes in move&view about potential change
            LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
            if (instance)
            {
                instance->updateClickActionViews();
            }
        }

        updateTable();
        break;
    case 1: // Current
        mConflictHandler[mEditingMode].resetToDefaults();
        // Apply changes to viewer as 'temporary'
        mConflictHandler[mEditingMode].saveToSettings(true);

        if (mEditingMode == LLKeyConflictHandler::MODE_THIRD_PERSON)
        {
            // notify comboboxes in move&view about potential change
            LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
            if (instance)
            {
                instance->updateClickActionViews();
            }
        }

        updateTable();
        break;
    case 2: // Cancel
    default:
        //exit;
        break;
    }
}

// Bypass to let Move & view read values without need to create own key binding handler
// Assumes third person view
// Might be better idea to just move whole mConflictHandler into LLFloaterPreference
bool LLPanelPreferenceControls::canKeyBindHandle(const std::string &control, EMouseClickType click, KEY key, MASK mask)
{
    S32 mode = LLKeyConflictHandler::MODE_THIRD_PERSON;
    if (mConflictHandler[mode].empty())
    {
        // opening for first time
        mConflictHandler[mode].loadFromSettings(LLKeyConflictHandler::MODE_THIRD_PERSON);
    }

    return mConflictHandler[mode].canHandleControl(control, click, key, mask);
}

// Bypass to let Move & view modify values without need to create own key binding handler
// Assumes third person view
// Might be better idea to just move whole mConflictHandler into LLFloaterPreference
void LLPanelPreferenceControls::setKeyBind(const std::string &control, EMouseClickType click, KEY key, MASK mask, bool set)
{
    S32 mode = LLKeyConflictHandler::MODE_THIRD_PERSON;
    if (mConflictHandler[mode].empty())
    {
        // opening for first time
        mConflictHandler[mode].loadFromSettings(LLKeyConflictHandler::MODE_THIRD_PERSON);
    }

    if (!mConflictHandler[mode].canAssignControl(mEditingControl))
    {
        return;
    }

    bool already_recorded = mConflictHandler[mode].canHandleControl(control, click, key, mask);
    if (set)
    {
        if (already_recorded)
        {
            // nothing to do
            return;
        }

        // find free spot to add data, if no free spot, assign to first
        S32 index = 0;
        for (S32 i = 0; i < 3; i++)
        {
            if (mConflictHandler[mode].getControl(control, i).isEmpty())
            {
                index = i;
                break;
            }
        }
        // At the moment 'ignore_mask' mask is mostly ignored, a placeholder
        // Todo: implement it since it's preferable for things like teleport to match
        // mask exactly but for things like running to ignore additional masks
        // Ideally this needs representation in keybindings UI
        bool ignore_mask = true;
        mConflictHandler[mode].registerControl(control, index, click, key, mask, ignore_mask);
    }
    else if (!set)
    {
        if (!already_recorded)
        {
            // nothing to do
            return;
        }

        // find specific control and reset it
        for (S32 i = 0; i < 3; i++)
        {
            LLKeyData data = mConflictHandler[mode].getControl(control, i);
            if (data.mMouse == click && data.mKey == key && data.mMask == mask)
            {
                mConflictHandler[mode].clearControl(control, i);
            }
        }
    }
}

void LLPanelPreferenceControls::updateAndApply()
{
    S32 mode = LLKeyConflictHandler::MODE_THIRD_PERSON;
    mConflictHandler[mode].saveToSettings(true);
    updateTable();
}

// from LLSetKeybindDialog's interface
bool LLPanelPreferenceControls::onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes)
{
    if (mEditingMode == LLKeyConflictHandler::MODE_COMMANDS)
    {
        return onSetCommandKeyBind(key, mask);
    }

    if (!mConflictHandler[mEditingMode].canAssignControl(mEditingControl))
    {
        return true;
    }

    if ( mEditingColumn > 0)
    {
        if (all_modes)
        {
            for (U32 i = 0; i < LLKeyConflictHandler::MODE_COMMANDS; ++i)
            {
                if (mConflictHandler[i].empty())
                {
                    mConflictHandler[i].loadFromSettings((LLKeyConflictHandler::ESourceMode)i);
                }
                mConflictHandler[i].registerControl(mEditingControl, mEditingColumn - 1, click, key, mask, true);
                // Apply changes to viewer as 'temporary'
                mConflictHandler[i].saveToSettings(true);
            }
        }
        else
        {
            // Each mode (first person, third person, sitting, edit avatar) has its own copy
            // of every binding. Emptying a binding in one tab used to leave the very same
            // binding live in the other three (e.g. middle mouse push-to-talk kept firing
            // while seated), so an emptied binding is emptied wherever an identical one
            // exists. Assigning something new stays per mode ("Apply to all" for the rest).
            const bool clearing = (click == CLICK_NONE && key == KEY_NONE);
            LLKeyData cleared = mConflictHandler[mEditingMode].getControl(mEditingControl, mEditingColumn - 1);

            mConflictHandler[mEditingMode].registerControl(mEditingControl, mEditingColumn - 1, click, key, mask, true);
            // Apply changes to viewer as 'temporary'
            mConflictHandler[mEditingMode].saveToSettings(true);

            if (clearing && !cleared.isEmpty())
            {
                for (U32 i = 0; i < LLKeyConflictHandler::MODE_COMMANDS; ++i)
                {
                    if (i == (U32)mEditingMode)
                    {
                        continue;
                    }
                    if (mConflictHandler[i].empty())
                    {
                        mConflictHandler[i].loadFromSettings((LLKeyConflictHandler::ESourceMode)i);
                    }
                    bool changed = false;
                    for (U32 slot = 0; slot < 3; ++slot)
                    {
                        LLKeyData other = mConflictHandler[i].getControl(mEditingControl, slot);
                        if (!other.isEmpty() && other == cleared)
                        {
                            changed |= mConflictHandler[i].clearControl(mEditingControl, slot);
                        }
                    }
                    if (changed)
                    {
                        mConflictHandler[i].saveToSettings(true);
                    }
                }
            }
        }
    }

    updateTable();

    if ((mEditingMode == LLKeyConflictHandler::MODE_THIRD_PERSON || all_modes)
        && (mEditingControl == "walk_to"
            || mEditingControl == "teleport_to"
            || click == CLICK_LEFT
            || click == CLICK_DOUBLELEFT))
    {
        // notify comboboxes in move&view about potential change
        LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
        if (instance)
        {
            instance->updateClickActionViews();
        }
    }

    return true;
}

// OCOL: bind a game controller input to the selected action
bool LLPanelPreferenceControls::onSetControllerBind(U8 action_type, U8 action, bool all_modes)
{
    if (!mConflictHandler[mEditingMode].canAssignControl(mEditingControl))
    {
        return true;
    }

    if (mEditingColumn > 0)
    {
        if (all_modes)
        {
            for (U32 i = 0; i < LLKeyConflictHandler::MODE_COUNT - 1; ++i)
            {
                if (mConflictHandler[i].empty())
                {
                    mConflictHandler[i].loadFromSettings((LLKeyConflictHandler::ESourceMode)i);
                }
                mConflictHandler[i].registerControllerControl(mEditingControl, mEditingColumn - 1, action_type, action);
                // Apply changes to viewer as 'temporary'
                mConflictHandler[i].saveToSettings(true);
            }
        }
        else
        {
            mConflictHandler[mEditingMode].registerControllerControl(mEditingControl, mEditingColumn - 1, action_type, action);
            // Apply changes to viewer as 'temporary'
            mConflictHandler[mEditingMode].saveToSettings(true);
        }
    }

    updateTable();
    return true;
}

void LLPanelPreferenceControls::onDefaultKeyBind(bool all_modes)
{
    if (mEditingMode == LLKeyConflictHandler::MODE_COMMANDS)
    {
        if (!mEditingControl.empty())
        {
            mConflictHandler[mEditingMode].resetToDefault(mEditingControl, 0);
            // Apply changes to viewer as 'temporary'
            mConflictHandler[mEditingMode].saveToSettings(true);
        }
        updateTable();
        return;
    }

    if (!mConflictHandler[mEditingMode].canAssignControl(mEditingControl))
    {
        return;
    }

    if (mEditingColumn > 0)
    {
        if (all_modes)
        {
            for (U32 i = 0; i < LLKeyConflictHandler::MODE_COMMANDS; ++i)
            {
                if (mConflictHandler[i].empty())
                {
                    mConflictHandler[i].loadFromSettings((LLKeyConflictHandler::ESourceMode)i);
                }
                mConflictHandler[i].resetToDefault(mEditingControl, mEditingColumn - 1);
                // Apply changes to viewer as 'temporary'
                mConflictHandler[i].saveToSettings(true);
            }
        }
        else
        {
            mConflictHandler[mEditingMode].resetToDefault(mEditingControl, mEditingColumn - 1);
            // Apply changes to viewer as 'temporary'
            mConflictHandler[mEditingMode].saveToSettings(true);
        }
    }

    updateTable();

    if (mEditingMode == LLKeyConflictHandler::MODE_THIRD_PERSON || all_modes)
    {
        // notify comboboxes in move&view about potential change
        LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
        if (instance)
        {
            instance->updateClickActionViews();
        }
    }
}

void LLPanelPreferenceControls::onCancelKeyBind()
{
    pControlsTable->deselectAllItems();
}

//------------------------LLFloaterPreferenceProxy--------------------------------

LLFloaterPreferenceProxy::LLFloaterPreferenceProxy(const LLSD& key)
    : LLFloater(key),
      mSocksSettingsDirty(false)
{
    mCommitCallbackRegistrar.add("Proxy.OK",                boost::bind(&LLFloaterPreferenceProxy::onBtnOk, this));
    mCommitCallbackRegistrar.add("Proxy.Cancel",            boost::bind(&LLFloaterPreferenceProxy::onBtnCancel, this));
    mCommitCallbackRegistrar.add("Proxy.Change",            boost::bind(&LLFloaterPreferenceProxy::onChangeSocksSettings, this));
}

LLFloaterPreferenceProxy::~LLFloaterPreferenceProxy()
{
}

bool LLFloaterPreferenceProxy::postBuild()
{
    LLRadioGroup* socksAuth = getChild<LLRadioGroup>("socks5_auth_type");
    if (!socksAuth)
    {
        return false;
    }
    if (socksAuth->getSelectedValue().asString() == "None")
    {
        getChild<LLLineEditor>("socks5_username")->setEnabled(false);
        getChild<LLLineEditor>("socks5_password")->setEnabled(false);
    }
    else
    {
        // Populate the SOCKS 5 credential fields with protected values.
        LLPointer<LLCredential> socks_cred = gSecAPIHandler->loadCredential("SOCKS5");
        getChild<LLLineEditor>("socks5_username")->setValue(socks_cred->getIdentifier()["username"].asString());
        getChild<LLLineEditor>("socks5_password")->setValue(socks_cred->getAuthenticator()["creds"].asString());
    }

    return true;
}

void LLFloaterPreferenceProxy::onOpen(const LLSD& key)
{
    saveSettings();
}

void LLFloaterPreferenceProxy::onClose(bool app_quitting)
{
    if (app_quitting)
    {
        cancel();
    }

    if (mSocksSettingsDirty)
    {

        // If the user plays with the Socks proxy settings after login, it's only fair we let them know
        // it will not be updated until next restart.
        if (LLStartUp::getStartupState()>STATE_LOGIN_WAIT)
        {
            LLNotifications::instance().add("ChangeProxySettings", LLSD(), LLSD());
            mSocksSettingsDirty = false; // we have notified the user now be quiet again
        }
    }
}

void LLFloaterPreferenceProxy::saveSettings()
{
    // Save the value of all controls in the hierarchy
    mSavedValues.clear();
    std::list<LLView*> view_stack;
    view_stack.push_back(this);
    while (!view_stack.empty())
    {
        // Process view on top of the stack
        LLView* curview = view_stack.front();
        view_stack.pop_front();

        LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(curview);
        if (ctrl)
        {
            LLControlVariable* control = ctrl->getControlVariable();
            if (control)
            {
                mSavedValues[control] = control->getValue();
            }
        }

        // Push children onto the end of the work stack
        for (child_list_t::const_iterator iter = curview->getChildList()->begin();
                iter != curview->getChildList()->end(); ++iter)
        {
            view_stack.push_back(*iter);
        }
    }
}

void LLFloaterPreferenceProxy::onBtnOk()
{
    // commit any outstanding text entry
    if (hasFocus())
    {
        LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
        if (cur_focus && cur_focus->acceptsTextInput())
        {
            cur_focus->onCommit();
        }
    }

    // Save SOCKS proxy credentials securely if password auth is enabled
    LLRadioGroup* socksAuth = getChild<LLRadioGroup>("socks5_auth_type");
    if (socksAuth->getSelectedValue().asString() == "UserPass")
    {
        LLSD socks_id = LLSD::emptyMap();
        socks_id["type"] = "SOCKS5";
        socks_id["username"] = getChild<LLLineEditor>("socks5_username")->getValue().asString();

        LLSD socks_authenticator = LLSD::emptyMap();
        socks_authenticator["type"] = "SOCKS5";
        socks_authenticator["creds"] = getChild<LLLineEditor>("socks5_password")->getValue().asString();

        // Using "SOCKS5" as the "grid" argument since the same proxy
        // settings will be used for all grids and because there is no
        // way to specify the type of credential.
        LLPointer<LLCredential> socks_cred = gSecAPIHandler->createCredential("SOCKS5", socks_id, socks_authenticator);
        gSecAPIHandler->saveCredential(socks_cred, true);
    }
    else
    {
        // Clear SOCKS5 credentials since they are no longer needed.
        LLPointer<LLCredential> socks_cred = new LLCredential("SOCKS5");
        gSecAPIHandler->deleteCredential(socks_cred);
    }

    closeFloater(false);
}

void LLFloaterPreferenceProxy::onBtnCancel()
{
    if (hasFocus())
    {
        LLUICtrl* cur_focus = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
        if (cur_focus && cur_focus->acceptsTextInput())
        {
            cur_focus->onCommit();
        }
        refresh();
    }

    cancel();
}

void LLFloaterPreferenceProxy::onClickCloseBtn(bool app_quitting)
{
    cancel();
}

void LLFloaterPreferenceProxy::cancel()
{
    for (const auto& iter : mSavedValues)
    {
        iter.first->set(iter.second);
    }
    mSocksSettingsDirty = false;
    closeFloater();
}

void LLFloaterPreferenceProxy::onChangeSocksSettings()
{
    mSocksSettingsDirty = true;

    LLRadioGroup* socksAuth = getChild<LLRadioGroup>("socks5_auth_type");
    if (socksAuth->getSelectedValue().asString() == "None")
    {
        getChild<LLLineEditor>("socks5_username")->setEnabled(false);
        getChild<LLLineEditor>("socks5_password")->setEnabled(false);
    }
    else
    {
        getChild<LLLineEditor>("socks5_username")->setEnabled(true);
        getChild<LLLineEditor>("socks5_password")->setEnabled(true);
    }

    // Check for invalid states for the other HTTP proxy radio
    LLRadioGroup* otherHttpProxy = getChild<LLRadioGroup>("other_http_proxy_type");
    if ((otherHttpProxy->getSelectedValue().asString() == "Socks" &&
            getChild<LLCheckBoxCtrl>("socks_proxy_enabled")->get() == false )||(
                    otherHttpProxy->getSelectedValue().asString() == "Web" &&
                    getChild<LLCheckBoxCtrl>("web_proxy_enabled")->get() == false ) )
    {
        otherHttpProxy->selectFirstItem();
    }
}

void LLFloaterPreference::onUpdateFilterTerm(bool force)
{
    LLWString seachValue = utf8str_to_wstring( mFilterEdit->getValue() );
    LLWStringUtil::toLower( seachValue );

    if( !mSearchData || (mSearchData->mLastFilter == seachValue && !force))
        return;

    if (mSearchDataDirty)
    {
        // Data exists, but is obsolete, regenerate
        collectSearchableItems();
    }

    mSearchData->mLastFilter = seachValue;

    if( !mSearchData->mRootTab )
        return;

    mSearchData->mRootTab->hightlightAndHide( seachValue );
    filterIgnorableNotifications();

    if (LLTabContainer* pRoot = getChild<LLTabContainer>("pref core"))
        pRoot->selectFirstTab();
}

void LLFloaterPreference::filterIgnorableNotifications()
{
    bool visible = mEnabledPopups->highlightMatchingItems(mFilterEdit->getValue());
    visible |= mDisabledPopups->highlightMatchingItems(mFilterEdit->getValue());

    if (visible)
    {
        getChildRef<LLTabContainer>("pref core").setTabVisibility( getChild<LLPanel>("msgs"), true );
    }
}

void collectChildren( LLView const *aView, ll::prefs::PanelDataPtr aParentPanel, ll::prefs::TabContainerDataPtr aParentTabContainer )
{
    if( !aView )
        return;

    llassert_always( aParentPanel || aParentTabContainer );

    for (LLView* pView : *aView->getChildList())
    {
        if (!pView)
            continue;

        ll::prefs::PanelDataPtr pCurPanelData = aParentPanel;
        ll::prefs::TabContainerDataPtr pCurTabContainer = aParentTabContainer;

        LLPanel const *pPanel = dynamic_cast< LLPanel const *>( pView );
        LLTabContainer const *pTabContainer = dynamic_cast< LLTabContainer const *>( pView );
        ll::ui::SearchableControl const *pSCtrl = dynamic_cast< ll::ui::SearchableControl const *>( pView );

        if( pTabContainer )
        {
            pCurPanelData.reset();

            pCurTabContainer = ll::prefs::TabContainerDataPtr( new ll::prefs::TabContainerData );
            pCurTabContainer->mTabContainer = const_cast< LLTabContainer *>( pTabContainer );
            pCurTabContainer->mLabel = pTabContainer->getLabel();
            pCurTabContainer->mPanel = 0;

            if( aParentPanel )
                aParentPanel->mChildPanel.push_back( pCurTabContainer );
            if( aParentTabContainer )
                aParentTabContainer->mChildPanel.push_back( pCurTabContainer );
        }
        else if( pPanel )
        {
            pCurTabContainer.reset();

            pCurPanelData = ll::prefs::PanelDataPtr( new ll::prefs::PanelData );
            pCurPanelData->mPanel = pPanel;
            pCurPanelData->mLabel = pPanel->getLabel();

            llassert_always( aParentPanel || aParentTabContainer );

            if( aParentTabContainer )
                aParentTabContainer->mChildPanel.push_back( pCurPanelData );
            else if( aParentPanel )
                aParentPanel->mChildPanel.push_back( pCurPanelData );
        }
        else if( pSCtrl && pSCtrl->getSearchText().size() )
        {
            ll::prefs::SearchableItemPtr item = ll::prefs::SearchableItemPtr( new ll::prefs::SearchableItem() );
            item->mView = pView;
            item->mCtrl = pSCtrl;

            item->mLabel = utf8str_to_wstring( pSCtrl->getSearchText() );
            LLWStringUtil::toLower( item->mLabel );

            llassert_always( aParentPanel || aParentTabContainer );

            if( aParentPanel )
                aParentPanel->mChildren.push_back( item );
            if( aParentTabContainer )
                aParentTabContainer->mChildren.push_back( item );
        }
        collectChildren( pView, pCurPanelData, pCurTabContainer );
    }
}

void LLFloaterPreference::collectSearchableItems()
{
    mSearchData.reset( nullptr );
    LLTabContainer *pRoot = getChild< LLTabContainer >( "pref core" );
    if( mFilterEdit && pRoot )
    {
        mSearchData.reset(new ll::prefs::SearchData() );

        ll::prefs::TabContainerDataPtr pRootTabcontainer = ll::prefs::TabContainerDataPtr( new ll::prefs::TabContainerData );
        pRootTabcontainer->mTabContainer = pRoot;
        pRootTabcontainer->mLabel = pRoot->getLabel();
        mSearchData->mRootTab = pRootTabcontainer;

        collectChildren( this, ll::prefs::PanelDataPtr(), pRootTabcontainer );
    }
    mSearchDataDirty = false;
}

void LLFloaterPreference::saveIgnoredNotifications()
{
    for (LLNotifications::TemplateMap::const_iterator iter = LLNotifications::instance().templatesBegin();
            iter != LLNotifications::instance().templatesEnd();
            ++iter)
    {
        LLNotificationTemplatePtr templatep = iter->second;
        LLNotificationFormPtr formp = templatep->mForm;

        LLNotificationForm::EIgnoreType ignore = formp->getIgnoreType();
        if (ignore <= LLNotificationForm::IGNORE_NO)
            continue;

        mIgnorableNotifs[templatep->mName] = !formp->getIgnored();
    }
}

void LLFloaterPreference::restoreIgnoredNotifications()
{
    for (std::map<std::string, bool>::iterator it = mIgnorableNotifs.begin(); it != mIgnorableNotifs.end(); ++it)
    {
        LLUI::getInstance()->mSettingGroups["ignores"]->setBOOL(it->first, it->second);
    }
}
