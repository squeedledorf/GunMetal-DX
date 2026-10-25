/**
 * @file llfloaterperformance.cpp
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2021, Linden Research, Inc.
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
#include "llfloaterperformance.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappearancemgr.h"
#include "llavataractions.h"
#include "llavatarrendernotifier.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfeaturemanager.h"
#include "llfloaterpreference.h" // LLAvatarComplexityControls
#include "llfloaterreg.h"
#include "llnamelistctrl.h"
#include "llnotificationsutil.h"
#include "llperfstats.h"
#include "llpresetsmanager.h"
#include "llradiogroup.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llscrollcontainer.h"
#include "lltexteditor.h"
#include "lltrans.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvkautotune.h"
#include "llvkperfcapture.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"

const F32 REFRESH_INTERVAL = 1.0f;
const S32 BAR_LEFT_PAD = 2;
const S32 BAR_RIGHT_PAD = 5;
const S32 BAR_BOTTOM_PAD = 9;

class LLExceptionsContextMenu : public LLListContextMenu
{
public:
    LLExceptionsContextMenu(LLFloaterPerformance* floater_settings)
        :   mFloaterPerformance(floater_settings)
    {}
protected:
    LLContextMenu* createMenu()
    {
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
        registrar.add("Settings.SetRendering", boost::bind(&LLFloaterPerformance::onCustomAction, mFloaterPerformance, _2, mUUIDs.front()));
        enable_registrar.add("Settings.IsSelected", boost::bind(&LLFloaterPerformance::isActionChecked, mFloaterPerformance, _2, mUUIDs.front()));
        LLContextMenu* menu = createFromFile("menu_avatar_rendering_settings.xml");

        return menu;
    }

    LLFloaterPerformance* mFloaterPerformance;
};

LLFloaterPerformance::LLFloaterPerformance(const LLSD& key)
:   LLFloater(key),
    mUpdateTimer(new LLTimer())
{
    mContextMenu = new LLExceptionsContextMenu(this);
}

LLFloaterPerformance::~LLFloaterPerformance()
{
    mMaxARTChangedSignal.disconnect();
    delete mContextMenu;
    delete mUpdateTimer;
}

bool LLFloaterPerformance::postBuild()
{
    mMainPanel = getChild<LLPanel>("panel_performance_main");
    mNearbyPanel = getChild<LLPanel>("panel_performance_nearby");
    mComplexityPanel = getChild<LLPanel>("panel_performance_complexity");
    mSettingsPanel = getChild<LLPanel>("panel_performance_preferences");
    mHUDsPanel = getChild<LLPanel>("panel_performance_huds");
    mAutoadjustmentsPanel = getChild<LLPanel>("panel_performance_autoadjustments");
    mTimingsPanel = getChild<LLPanel>("panel_performance_timings");

    getChild<LLPanel>("nearby_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mNearbyPanel));
    getChild<LLPanel>("complexity_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mComplexityPanel));
    getChild<LLPanel>("settings_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mSettingsPanel));
    getChild<LLPanel>("huds_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mHUDsPanel));
    getChild<LLPanel>("autoadjustments_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mAutoadjustmentsPanel));
    // #966: the Frame timings page, from its row and from the frame breakdown under the rows.
    getChild<LLPanel>("timings_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mTimingsPanel));
    getChild<LLPanel>("breakdown_subpanel")->setMouseDownCallback(boost::bind(&LLFloaterPerformance::showSelectedPanel, this, mTimingsPanel));

    initBackBtn(mNearbyPanel);
    initBackBtn(mComplexityPanel);
    initBackBtn(mSettingsPanel);
    initBackBtn(mHUDsPanel);
    initBackBtn(mAutoadjustmentsPanel);
    initBackBtn(mTimingsPanel);

    mTimingsText = mTimingsPanel->getChild<LLTextEditor>("timings_text");
    mCaptureBtn = mTimingsPanel->getChild<LLButton>("capture_btn");
    mCopyCaptureBtn = mTimingsPanel->getChild<LLButton>("copy_capture_btn");
    mCaptureStatus = mTimingsPanel->getChild<LLTextBox>("capture_status");
    mCaptureBtn->setCommitCallback(boost::bind(&LLFloaterPerformance::onCaptureTimings, this));
    mCopyCaptureBtn->setCommitCallback(boost::bind(&LLFloaterPerformance::onCopyCapture, this));
    mTimingsPanel->getChild<LLButton>("report_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLFloaterReg::showInstance("report_bug");
    });

    mHUDList = mHUDsPanel->getChild<LLNameListCtrl>("hud_list");
    mHUDList->setNameListType(LLNameListCtrl::SPECIAL);
    mHUDList->setHoverIconName("StopReload_Off");
    mHUDList->setIconClickedCallback(boost::bind(&LLFloaterPerformance::detachObject, this, _1));

    mObjectList = mComplexityPanel->getChild<LLNameListCtrl>("obj_list");
    mObjectList->setNameListType(LLNameListCtrl::SPECIAL);
    mObjectList->setHoverIconName("StopReload_Off");
    mObjectList->setIconClickedCallback(boost::bind(&LLFloaterPerformance::detachObject, this, _1));

    mSettingsPanel->getChild<LLButton>("advanced_btn")->setCommitCallback(boost::bind(&LLFloaterPerformance::onClickAdvanced, this));
    mSettingsPanel->getChild<LLButton>("defaults_btn")->setCommitCallback(boost::bind(&LLFloaterPerformance::onClickDefaults, this));
    mSettingsPanel->getChild<LLRadioGroup>("graphics_quality")->setCommitCallback(boost::bind(&LLFloaterPerformance::onChangeQuality, this, _2));
    // (#422: the GL "advanced lighting model" checkbox hook -- a control this panel never
    // had -- and the shadows-need-quality-4 confirmation are gone: Vulkan always renders
    // the full lighting model, shadows need no quality level.)

    mNearbyPanel->getChild<LLButton>("exceptions_btn")->setCommitCallback(boost::bind(&LLFloaterPerformance::onClickExceptions, this));
    mNearbyPanel->getChild<LLCheckBoxCtrl>("hide_avatars")->setCommitCallback(boost::bind(&LLFloaterPerformance::onClickHideAvatars, this));
    mNearbyPanel->getChild<LLCheckBoxCtrl>("hide_avatars")->set(!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_AVATAR));
    mNearbyList = mNearbyPanel->getChild<LLNameListCtrl>("nearby_list");
    mNearbyList->setRightMouseDownCallback(boost::bind(&LLFloaterPerformance::onAvatarListRightClick, this, _1, _2, _3));

    mMaxARTChangedSignal = gSavedSettings.getControl("RenderAvatarMaxART")->getCommitSignal()->connect(boost::bind(&LLFloaterPerformance::updateMaxRenderTime, this));
    mNearbyPanel->getChild<LLSliderCtrl>("RenderAvatarMaxART")->setCommitCallback(boost::bind(&LLFloaterPerformance::updateMaxRenderTime, this));

    // [FPS_LIMIT] (the display's refresh rate) is filled on the refresh timer: it can be
    // unknown at first and changes when the window moves to another monitor (#422).
    mTextVsyncLimit = mAutoadjustmentsPanel->getChild<LLTextBox>("vsync_desc_limit");
    mTextDisplayDesc = mAutoadjustmentsPanel->getChild<LLTextBox>("display_desc");
    mAutoadjustmentsPanel->getChild<LLButton>("defaults_btn")->setCommitCallback(boost::bind(&LLFloaterPerformance::onClickDefaults, this));

    mStartAutotuneBtn = mAutoadjustmentsPanel->getChild<LLButton>("start_autotune");
    mStopAutotuneBtn = mAutoadjustmentsPanel->getChild<LLButton>("stop_autotune");
    mUndoAutotuneBtn = mAutoadjustmentsPanel->getChild<LLButton>("undo_autotune");
    mStartAutotuneBtn->setCommitCallback(boost::bind(&LLFloaterPerformance::startAutotune, this));
    mStopAutotuneBtn->setCommitCallback(boost::bind(&LLFloaterPerformance::stopAutotune, this));
    mUndoAutotuneBtn->setCommitCallback(boost::bind(&LLFloaterPerformance::undoAutotune, this));

    mTextWIPDesc = mAutoadjustmentsPanel->getChild<LLTextBox>("wip_desc");
    mTextTunerStatus = mAutoadjustmentsPanel->getChild<LLTextBox>("tuner_status");
    mTextTunerSummary = mAutoadjustmentsPanel->getChild<LLTextBox>("tuner_summary");
    mTextBreakdownAuto = mAutoadjustmentsPanel->getChild<LLTextBox>("frame_breakdown");
    mTextBreakdownMain = getChild<LLTextBox>("frame_breakdown_main");

    mTextFPSLabel = getChild<LLTextBox>("fps_lbl");
    mTextFPSValue = getChild<LLTextBox>("fps_value");

    gSavedPerAccountSettings.declareBOOL("HadEnabledAutoFPS", false, "User had enabled AutoFPS at least once", LLControlVariable::PERSIST_ALWAYS);

    return true;
}

void LLFloaterPerformance::showSelectedPanel(LLPanel* selected_panel)
{
    hidePanels();
    mMainPanel->setVisible(false);
    selected_panel->setVisible(true);

    if (mHUDsPanel == selected_panel)
    {
        populateHUDList();
    }
    else if (mNearbyPanel == selected_panel)
    {
        populateNearbyList();
    }
    else if (mComplexityPanel == selected_panel)
    {
        populateObjectList();
    }
    else if (mTimingsPanel == selected_panel)
    {
        mTimingsNext = 0.0;   // fill at once
    }
    updateTunerTexts();
}

void LLFloaterPerformance::showAutoadjustmentsPanel()
{
    showSelectedPanel(mAutoadjustmentsPanel);
}

void LLFloaterPerformance::showTimingsPanel()
{
    showSelectedPanel(mTimingsPanel);
}

// static
void LLFloaterPerformance::showTimings()
{
    LLFloaterPerformance* floater = LLFloaterReg::showTypedInstance<LLFloaterPerformance>("performance");
    if (floater)
    {
        floater->showTimingsPanel();
    }
}

void LLFloaterPerformance::draw()
{
    // #422: keep the per-pass GPU timers on while the breakdown is on screen.
    LLVKAutoTune::instance().requestPassTimers();
    if (mTimingsPanel->getVisible() && !isMinimized())
    {
        // #966: sampling runs only while this page is on screen (or a capture runs).
        LLVKPerfCapture::instance().requestLive();
        updateTimingsPanel();
    }

    if (mUpdateTimer->hasExpired() &&
        !LLFloaterReg::instanceVisible("save_pref_preset", PRESETS_GRAPHIC)) // give user a chance to save the graphics settings before updating them
    {
        setFPSText();
        updateTunerTexts();
        if (mHUDsPanel->getVisible())
        {
            populateHUDList();
        }
        else if (mNearbyPanel->getVisible())
        {
            populateNearbyList();
            mNearbyPanel->getChild<LLCheckBoxCtrl>("hide_avatars")->set(!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_AVATAR));
        }
        else if (mComplexityPanel->getVisible())
        {
            populateObjectList();
        }

        mUpdateTimer->setTimerExpirySec(REFRESH_INTERVAL);
    }
    updateAutotuneCtrls();

    LLFloater::draw();
}

void LLFloaterPerformance::showMainPanel()
{
    hidePanels();
    mMainPanel->setVisible(true);
    updateTunerTexts();
}

void LLFloaterPerformance::hidePanels()
{
    mNearbyPanel->setVisible(false);
    mComplexityPanel->setVisible(false);
    mHUDsPanel->setVisible(false);
    mSettingsPanel->setVisible(false);
    mAutoadjustmentsPanel->setVisible(false);
    mTimingsPanel->setVisible(false);
}

void LLFloaterPerformance::initBackBtn(LLPanel* panel)
{
    panel->getChild<LLButton>("back_btn")->setCommitCallback(boost::bind(&LLFloaterPerformance::showMainPanel, this));

    panel->getChild<LLTextBox>("back_lbl")->setShowCursorHand(false);
    panel->getChild<LLTextBox>("back_lbl")->setSoundFlags(LLView::MOUSE_UP);
    panel->getChild<LLTextBox>("back_lbl")->setClickedCallback(boost::bind(&LLFloaterPerformance::showMainPanel, this));
}

// #422: per-attachment cost for the "your avatar" and HUD lists. These showed
// LLViewerObject::mGPURenderTime, written only by the GL renderer's per-object timer
// queries (-1 under Vulkan: every bar full, "-1" in every row). The GPU-driven renderer
// has no per-object timer, so the lists show complexity -- the same per-item numbers as
// the Appearance window (#627, LLVOAvatar::getSelfItemComplexity) and, for HUDs (not
// part of avatar complexity), the same object + texture cost sum.
static U32 attachment_complexity(LLViewerObject* obj)
{
    if (!obj) return 0;
    if (gAgentAvatarp && !obj->isHUDAttachment())
    {
        const std::map<LLUUID, U32>& items = obj->isTempAttachment() ? gAgentAvatarp->getSelfTempItemComplexity()
                                                                      : gAgentAvatarp->getSelfItemComplexity();
        auto it = items.find(obj->isTempAttachment() ? obj->getID() : obj->getAttachmentItemID());
        if (it != items.end()) return it->second;
    }
    if (obj->mDrawable.isNull()) return 0;
    const LLVOVolume* volume = obj->mDrawable->getVOVolume();
    if (!volume) return 0;
    LLVOVolume::texture_cost_t textures;
    F32 cost = (F32)volume->getRenderCost(textures);
    for (LLViewerObject* child : obj->getChildren())
    {
        if (const LLVOVolume* cv = dynamic_cast<const LLVOVolume*>(child))
        {
            cost += (F32)cv->getRenderCost(textures);
        }
    }
    for (const LLViewerTexture* tex : textures)
    {
        cost += (F32)LLVOVolume::getTextureCost(tex);
    }
    return (U32)cost;
}

void LLFloaterPerformance::populateAttachmentList(LLNameListCtrl* list, bool huds)
{
    S32 prev_pos = list->getScrollPos();
    LLUUID prev_selected_id = list->getSelectedSpecialId();
    list->clearRows();
    list->updateColumns(true);

    LLVOAvatar* avatar = gAgentAvatarp;
    if (!avatar)
    {
        return;
    }

    std::vector<std::pair<LLViewerObject*, U32>> rows;
    U32 max_cost = 0;
    for (const auto& ap : avatar->mAttachmentPoints)
    {
        LLViewerJointAttachment* attachment = ap.second;
        if (!attachment)
        {
            continue;
        }
        for (const auto& attached : attachment->mAttachedObjects)
        {
            LLViewerObject* attached_object = attached.get();
            if (attached_object && attached_object->isHUDAttachment() == huds)
            {
                const U32 cost = attachment_complexity(attached_object);
                rows.emplace_back(attached_object, cost);
                max_cost = llmax(max_cost, cost);
            }
        }
    }

    for (const auto& r : rows)
    {
        LLViewerObject* attached_object = r.first;
        LLSD item;
        item["special_id"] = attached_object->getID();
        item["target"] = LLNameListCtrl::SPECIAL;
        LLSD& row = item["columns"];
        row[0]["column"] = "complex_visual";
        row[0]["type"] = "bar";
        LLSD& value = row[0]["value"];
        value["ratio"] = max_cost > 0 ? (F32)r.second / (F32)max_cost : 0.f;
        value["bottom"] = BAR_BOTTOM_PAD;
        value["left_pad"] = BAR_LEFT_PAD;
        value["right_pad"] = BAR_RIGHT_PAD;

        row[1]["column"] = "complex_value";
        row[1]["type"] = "text";
        row[1]["value"] = llformat("%u", r.second);   // complexity
        row[1]["font"]["name"] = "SANSSERIF";

        row[2]["column"] = "name";
        row[2]["type"] = "text";
        row[2]["value"] = attached_object->getAttachmentItemName();
        row[2]["font"]["name"] = "SANSSERIF";

        LLScrollListItem* obj = list->addElement(item);
        if (obj)
        {
            LLScrollListText* value_text = dynamic_cast<LLScrollListText*>(obj->getColumn(1));
            if (value_text)
            {
                value_text->setAlignment(LLFontGL::HCENTER);
            }
        }
    }
    list->sortByColumnIndex(1, false);
    list->setScrollPos(prev_pos);
    list->selectItemBySpecialId(prev_selected_id);
}

void LLFloaterPerformance::populateHUDList()
{
    populateAttachmentList(mHUDList, true);
}

void LLFloaterPerformance::populateObjectList()
{
    populateAttachmentList(mObjectList, false);
}

void LLFloaterPerformance::populateNearbyList()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_APP;
    S32 prev_pos = mNearbyList->getScrollPos();
    LLUUID prev_selected_id = mNearbyList->getStringUUIDSelectedItem();
    mNearbyList->clearRows();
    mNearbyList->updateColumns(true);

    static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0);
    std::vector<LLVOAvatar*> valid_nearby_avs;
    mNearbyMaxGPUTime = LLWorld::getInstance()->getNearbyAvatarsAndMaxGPUTime(valid_nearby_avs);

    for (LLVOAvatar* avatar : valid_nearby_avs)
    {
        if (LLVOAvatar::AOA_INVISIBLE != avatar->getOverallAppearance())
        {
            F32 render_av_gpu_ms = avatar->getGPURenderTime();

            auto is_slow = avatar->isTooSlow();
            LLSD item;
            item["id"] = avatar->getID();
            LLSD& row = item["columns"];
            row[0]["column"] = "complex_visual";
            row[0]["type"] = "bar";
            LLSD& value = row[0]["value"];
            // The ratio used in the bar is the current cost, as soon as we take action this changes so we keep the
            // pre-tune value for the numerical column and sorting.
            // (#422: the Vulkan estimate, LLVKAutoTune::updateAvatarCosts; 0 before any rigged
            // GPU sample -- no division by zero.)
            value["ratio"] = mNearbyMaxGPUTime > 0.f ? render_av_gpu_ms / mNearbyMaxGPUTime : 0.f;
            value["bottom"] = BAR_BOTTOM_PAD;
            value["left_pad"] = BAR_LEFT_PAD;
            value["right_pad"] = BAR_RIGHT_PAD;

            row[1]["column"] = "complex_value";
            row[1]["type"] = "text";
            // use GPU time in us
            row[1]["value"] = llformat( "%.f", render_av_gpu_ms * 1000.f);
            row[1]["font"]["name"] = "SANSSERIF";

            row[3]["column"] = "name";
            row[3]["type"] = "text";
            row[3]["value"] = avatar->getFullname();
            row[3]["font"]["name"] = "SANSSERIF";

            LLScrollListItem* av_item = mNearbyList->addElement(item);
            if(av_item)
            {
                LLScrollListText* value_text = dynamic_cast<LLScrollListText*>(av_item->getColumn(1));
                if (value_text)
                {
                    value_text->setAlignment(LLFontGL::HCENTER);
                }
                LLScrollListText* name_text = dynamic_cast<LLScrollListText*>(av_item->getColumn(2));
                if (name_text)
                {
                    if (avatar->isSelf())
                    {
                        name_text->setColor(LLUIColorTable::instance().getColor("DrYellow"));
                    }
                    else
                    {
                        std::string color = "white";
                        if (is_slow || LLVOAvatar::AOA_JELLYDOLL == avatar->getOverallAppearance())
                        {
                            color = "LabelDisabledColor";
                            LLScrollListBar* bar = dynamic_cast<LLScrollListBar*>(av_item->getColumn(0));
                            if (bar)
                            {
                                bar->setColor(LLUIColorTable::instance().getColor(color));
                            }
                        }
                        else if (LLVOAvatar::AOA_NORMAL == avatar->getOverallAppearance())
                        {
                            color = LLAvatarActions::isFriend(avatar->getID()) ? "ConversationFriendColor" : "white";
                        }
                        name_text->setColor(LLUIColorTable::instance().getColor(color));
                    }
                }
            }
        }
    }
    mNearbyList->sortByColumnIndex(1, false);
    mNearbyList->setScrollPos(prev_pos);
    mNearbyList->selectByID(prev_selected_id);
}

void LLFloaterPerformance::setFPSText()
{
    const S32 NUM_PERIODS = 50;
    S32 current_fps = (S32)llround(LLTrace::get_frame_recording().getPeriodMedianPerSec(LLStatViewer::FPS, NUM_PERIODS));
    mTextFPSValue->setValue(current_fps);

    std::string fps_text = getString("fps_text");
    static LLCachedControl<bool> vsync_enabled(gSavedSettings, "RenderVSyncEnable", true);
    S32 refresh_rate = gViewerWindow->getWindow()->getRefreshRate();
    if (vsync_enabled && (refresh_rate > 0) && (current_fps >= refresh_rate))
    {
        fps_text += getString("max_text");
    }
    mTextFPSLabel->setValue(fps_text);
}

void LLFloaterPerformance::detachObject(const LLUUID& obj_id)
{
    LLViewerObject* obj = gObjectList.findObject(obj_id);
    if (obj)
    {
        LLAppearanceMgr::instance().removeItemFromAvatar(obj->getAttachmentItemID());
    }
}

void LLFloaterPerformance::onClickAdvanced()
{
    LLFloaterPreference* instance = LLFloaterReg::getTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->saveSettings();
    }
    LLFloaterReg::showInstance("prefs_graphics_advanced");
}

void LLFloaterPerformance::onClickDefaults()
{
    LLFloaterPreference* instance = LLFloaterReg::getTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->setRecommendedSettings();
    }
}

void LLFloaterPerformance::onChangeQuality(const LLSD& data)
{
    LLFloaterPreference* instance = LLFloaterReg::getTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->onChangeQuality(data);
    }
}

void LLFloaterPerformance::onClickHideAvatars()
{
    LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_AVATAR);
}

void LLFloaterPerformance::onClickExceptions()
{
    LLFloaterReg::showInstance("blocked", LLSD("avatar_rendering_tab"));
}

void LLFloaterPerformance::updateMaxRenderTime()
{
    LLAvatarComplexityControls::updateMaxRenderTime(
        mNearbyPanel->getChild<LLSliderCtrl>("RenderAvatarMaxART"),
        mNearbyPanel->getChild<LLTextBox>("RenderAvatarMaxARTText"),
        true);
}

static LLVOAvatar* find_avatar(const LLUUID& id)
{
    LLViewerObject *obj = gObjectList.findObject(id);
    while (obj && obj->isAttachment())
    {
        obj = (LLViewerObject *)obj->getParent();
    }

    if (obj && obj->isAvatar())
    {
        return (LLVOAvatar*)obj;
    }
    else
    {
        return NULL;
    }
}

void LLFloaterPerformance::onCustomAction(const LLSD& userdata, const LLUUID& av_id)
{
    const std::string command_name = userdata.asString();

    S32 new_setting = 0;
    if ("default" == command_name)
    {
        new_setting = S32(LLVOAvatar::AV_RENDER_NORMALLY);
    }
    else if ("never" == command_name)
    {
        new_setting = S32(LLVOAvatar::AV_DO_NOT_RENDER);
    }
    else if ("always" == command_name)
    {
        new_setting = S32(LLVOAvatar::AV_ALWAYS_RENDER);
    }

    LLVOAvatar *avatarp = find_avatar(av_id);
    if (avatarp)
    {
        avatarp->setVisualMuteSettings(LLVOAvatar::VisualMuteSettings(new_setting));
    }
    else
    {
        LLRenderMuteList::getInstance()->saveVisualMuteSetting(av_id, new_setting);
    }
}


bool LLFloaterPerformance::isActionChecked(const LLSD& userdata, const LLUUID& av_id)
{
    const std::string command_name = userdata.asString();

    S32 visual_setting = LLRenderMuteList::getInstance()->getSavedVisualMuteSetting(av_id);
    if ("default" == command_name)
    {
        return (visual_setting == S32(LLVOAvatar::AV_RENDER_NORMALLY));
    }
    else if ("non_default" == command_name)
    {
        return (visual_setting != S32(LLVOAvatar::AV_RENDER_NORMALLY));
    }
    else if ("never" == command_name)
    {
        return (visual_setting == S32(LLVOAvatar::AV_DO_NOT_RENDER));
    }
    else if ("always" == command_name)
    {
        return (visual_setting == S32(LLVOAvatar::AV_ALWAYS_RENDER));
    }
    return false;
}

void LLFloaterPerformance::onAvatarListRightClick(LLUICtrl* ctrl, S32 x, S32 y)
{
    LLNameListCtrl* list = dynamic_cast<LLNameListCtrl*>(ctrl);
    if (!list) return;
    list->selectItemAt(x, y, MASK_NONE);
    uuid_vec_t selected_uuids;

    if((list->getCurrentID().notNull()) && (list->getCurrentID() != gAgentID))
    {
        selected_uuids.push_back(list->getCurrentID());
        mContextMenu->show(ctrl, selected_uuids, x, y);
    }
}

void LLFloaterPerformance::startAutotune()
{
    // First Tune once on this account: offer to save the current graphics settings as a
    // preset first (Undo restores the last run's changes; a preset keeps them for good).
    if (!gSavedPerAccountSettings.getBOOL("HadEnabledAutoFPS"))
    {
        gSavedPerAccountSettings.setBOOL("HadEnabledAutoFPS", true);
        LLNotificationsUtil::add("EnableAutoFPSWarning", LLSD(), LLSD(),
            [](const LLSD& notif, const LLSD& resp)
            {
                S32 opt = LLNotificationsUtil::getSelectedOption(notif, resp);
                if (opt == 0)
                { // save a preset first; Tune once is started again by the user
                    LLFloaterReg::showInstance("save_pref_preset", PRESETS_GRAPHIC);
                }
                else
                {
                    LLVKAutoTune::instance().startTuneOnce();
                }
            });
        return;
    }
    LLVKAutoTune::instance().startTuneOnce();
}

void LLFloaterPerformance::stopAutotune()
{
    LLVKAutoTune::instance().cancelTuneOnce();
}

void LLFloaterPerformance::undoAutotune()
{
    LLVKAutoTune::instance().undo();
    updateTunerTexts();
}

void LLFloaterPerformance::updateAutotuneCtrls()
{
    const LLVKAutoTune& tuner = LLVKAutoTune::instance();
    const bool tuning = tuner.tuningOnce();
    mStartAutotuneBtn->setEnabled(!tuning);
    mStopAutotuneBtn->setEnabled(tuning);
    mUndoAutotuneBtn->setEnabled(tuner.canUndo());
    mTextWIPDesc->setVisible(tuning);
}

// #422: our frame breakdown (main page and auto-adjust page), the tuner's status and the
// last Tune once's summary; the display's refresh rate for the vsync texts.
void LLFloaterPerformance::updateTunerTexts()
{
    const LLVKAutoTune& tuner = LLVKAutoTune::instance();
    if (mMainPanel->getVisible())
    {
        mTextBreakdownMain->setText(tuner.breakdownText(false));
    }
    if (mAutoadjustmentsPanel->getVisible())
    {
        mTextBreakdownAuto->setText(tuner.breakdownText(true));
        mTextTunerStatus->setText(tuner.statusText());
        mTextTunerSummary->setText(tuner.summaryText());
        const S32 refresh = LLVKAutoTune::refreshRate();
        static LLCachedControl<bool> vsync(gSavedSettings, "RenderVSyncEnable", true);
        const LLStringExplicit fps_limit(llformat("%d", refresh));
        mTextDisplayDesc->setTextArg("[FPS_LIMIT]", fps_limit);
        mTextVsyncLimit->setTextArg("[FPS_LIMIT]", fps_limit);
        mTextDisplayDesc->setVisible(refresh > 0 && vsync);
        mTextVsyncLimit->setVisible(refresh > 0);
    }
}

// #966 Frame timings page: the live readout twice a second, the capture state every draw
// (its countdown).
void LLFloaterPerformance::updateTimingsPanel()
{
    LLVKPerfCapture& pc = LLVKPerfCapture::instance();
    const F64 now = LLTimer::getTotalSeconds();
    if (now >= mTimingsNext)
    {
        mTimingsNext = now + 0.5;
        // setText moves the cursor to the start (scrolling to the top): keep the reader's place.
        LLScrollContainer* scroller = mTimingsText->getScrollContainer();
        const LLRect visible = scroller ? scroller->getVisibleContentRect() : LLRect();
        mTimingsText->setText(pc.liveText());
        if (scroller)
        {
            mTimingsText->updateLayoutNow();
            scroller->scrollToShowRect(visible);
        }
    }
    std::string status;
    LLStringUtil::format_map_t args;
    if (pc.capturing())
    {
        args["[SECONDS]"] = llformat("%.0f", (F64)pc.captureRemaining());
        status = mTimingsPanel->getString("capturing", args);
    }
    else if (!pc.lastText().empty())
    {
        args["[SUMMARY]"] = pc.lastSummary();
        args["[FILE]"] = pc.lastFile();
        args["[MINUTES]"] = llformat("%.0f", (now - pc.lastAt()) / 60.0);
        status = mTimingsPanel->getString("captured", args);
    }
    else
    {
        status = mTimingsPanel->getString("no_capture");
    }
    if (status != mCaptureStatusText)
    {
        mCaptureStatusText = status;
        mCaptureStatus->setText(status);
    }
    mCaptureBtn->setEnabled(!pc.capturing());
    mCopyCaptureBtn->setEnabled(!pc.capturing() && !pc.lastText().empty());
}

void LLFloaterPerformance::onCaptureTimings()
{
    static LLCachedControl<U32> s_seconds(gSavedSettings, "PerfCaptureSeconds", LLVKPerfCapture::kDefaultSeconds);
    LLVKPerfCapture::instance().startCapture(s_seconds, true);   // the result goes to the clipboard too
}

void LLFloaterPerformance::onCopyCapture()
{
    LLVKPerfCapture::instance().copyLastToClipboard();
}
// EOF
