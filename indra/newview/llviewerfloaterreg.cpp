/**
 * @file llviewerfloaterreg.cpp
 * @brief LLViewerFloaterReg class registers floaters used in the viewer
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h"

#include "llfloaterreg.h"
#include "llviewerfloaterreg.h"
#include "fsfloaterteleporthistory.h"
#include "fsfloaterradar.h"
#include "fsfloaterhitmarker.h" // <GMDX>
#include "fsfloaterkillfeed.h" // <GMDX>
#include "fsfloatergrouptitles.h"
#include "fsfloatergroupviewer.h" // <GMDX>
#include "gmdxregioncapture.h" // <GMDX>
#include "gmdxfloaterloadouts.h" // <GMDX>
#include "gmdxweaponsorter.h" // <GMDX>
#include "fsfloateraddtocontactset.h"
#include "fsfloatercontacts.h"
#include "fsfloatercontactsetconfiguration.h"

#include "slchatbar.h"
#include "slfloatergenerictext.h"
#include "slfloaterparticleeditor.h"
#include "slfloaterregiontracker.h"
#include "bdfloatercamera.h"
#include "slfloaterphotostudio.h"
#include "slfloatershapeeditor.h"
#include "fsfloaterposer.h" // <FS:AR> [FIRE-30873]: Poser
#include "slfloaterao.h"
#include "slfloatergesturesets.h"
#include "slfloaterfirstrun.h"
#include "slfloaterposecreator.h"
#include "slfloaterposestudio.h"
#include "slfloaterviewerimport.h"
#include "llcommandhandler.h"
#include "llcompilequeue.h"
#include "llfasttimerview.h"
#include "llfloater360capture.h"
#include "llfloaterabout.h"
#include "llfloateraddpaymentmethod.h"
#include "llfloaterauction.h"
#include "llfloaterautoreplacesettings.h"
#include "llfloateravatarpicker.h"
#include "llfloateravatarwelcomepack.h"
#include "llfloateravatarrendersettings.h"
#include "llfloateravatartextures.h"
#include "llfloaterbanduration.h"
#include "llfloaterbigpreview.h"
#include "llfloaterbeacons.h"
// [SL:KB] - Patch: World-Derender | Checked: Catznip-3.2
#include "llfloaterblocked.h"
// [/SL:KB]
#include "llfloaterbuildoptions.h"
#include "llfloaterbulkpermission.h"
#include "llfloaterbulkupload.h"
#include "llfloaterbump.h"
#include "llfloaterbuy.h"
#include "llfloaterbuycontents.h"
#include "llfloaterbuycurrency.h"
#include "llfloaterbuycurrencyhtml.h"
#include "llfloaterbuyland.h"
#include "llfloaterbvhpreview.h"
#include "llfloatercamera.h"
#include "llfloatercamerapresets.h"
#include "llfloaterchangeitemthumbnail.h"
#include "llfloaterchatmentionpicker.h"
#include "llfloaterchatimage.h"
#include "llfloaterchatvoicevolume.h"
#include "llfloaterclassified.h"
#include "llfloaterconversationlog.h"
#include "llfloaterconversationpreview.h"
#include "llfloatercreatelandmark.h"
#include "llfloaterdeleteprefpreset.h"
#include "llfloaterdestinations.h"
#include "llfloaterdirectory.h"
#include "llfloaterdisplayname.h"
#include "llfloatereditextdaycycle.h"
#include "llfloateremojipicker.h"
#include "llfloaterenvironmentadjust.h"
#include "llfloaterexperienceprofile.h"
#include "llfloaterexperiences.h"
#include "llfloaterexperiencepicker.h"
#include "llfloaterevent.h"
#include "llfloaterfixedenvironment.h"
#include "llfloaterfonttest.h"
#include "llfloaterforgetuser.h"
#include "llfloatergesture.h"
#include "ssfloatergamecontroldebug.h" // <SS:GameControl>
#include "ssfloatergamecontroloverlay.h" // <SS:GameControl>
#include "ssfloatergamecontrolgyro.h" // <SS:GameControl>
#include "llfloatergltfasseteditor.h"
#include "llfloatergltfexport.h"
#include "llfloatergodtools.h"
#include "llfloatergridstatus.h"
#include "llfloatergroupprofile.h"
#include "llfloatergroups.h"
#include "llfloaterhelpbrowser.h"
#include "llfloaterhoverheight.h"
#include "llfloaterhowto.h"
#include "llfloaterhud.h"
#include "llfloaterimagepreview.h"
#include "llfloaterimsession.h"
#include "llfloaterinspect.h"
#include "llfloaterinventorysettings.h"
#include "llfloaterinventorythumbnailshelper.h"
#include "llfloaterjoystick.h"
#include "llfloaterlagmeter.h"
#include "llfloaterland.h"
#include "llfloaterlandholdings.h"
#include "llfloaterlinkreplace.h"
#include "llfloaterloadprefpreset.h"
#include "llfloaterlocalassets.h"
#include "llfloatermap.h"
#include "llfloatermarketplace.h"
#include "llfloatermarketplacelistings.h"
#include "llfloatermediasettings.h"
#include "llfloatermemleak.h"
#include "llfloatermeshbulkupload.h"
#include "llfloatermodelpreview.h"
#include "llfloatermyscripts.h"
#include "llfloatermyenvironment.h"
#include "llfloaternamedesc.h"
#include "llfloaternewfeaturenotification.h"
#include "llfloaternotificationsconsole.h"
#include "llfloaternotificationstabbed.h"
#include "llfloaterobjectweights.h"
#include "llfloateropenobject.h"
#include "llfloatersimplesnapshot.h"
#include "fsareasearch.h"
#include "slfloatersoundexplorer.h"
#include "slfloaterspeakersetup.h"
#include "slfloaterweather.h"
#include "llfloaterpathfindingcharacters.h"
#include "llfloaterpathfindingconsole.h"
#include "llfloaterpathfindinglinksets.h"
#include "llfloaterpay.h"
#include "llfloaterperformance.h"
#include "llfloaterperms.h"
#include "llfloaterpreference.h"
#include "llfloaterpreferencesgraphicsadvanced.h"
#include "llfloaterpreferenceviewadvanced.h"
#include "llfloaterpreviewtrash.h"
#include "llfloaterprofile.h"
#include "llfloaterprofilelegacy.h"
#include "llfloaterprogressview.h"
#include "llfloaterpublishclassified.h"
#include "llfloaterregiondebugconsole.h"
#include "llfloaterregioninfo.h"
#include "llfloaterregionrestarting.h"
#include "llfloaterreporter.h"
#include "llfloaterreportbug.h"
#include "llfloatersavecamerapreset.h"
#include "llfloatersaveoutfitas.h"
#include "llfloatersaveprefpreset.h"
#include "llfloatersceneloadstats.h"
#include "llfloaterscriptdebug.h"
#include "llfloaterscriptedprefs.h"
#include "llfloaterscriptlimits.h"
#include "llfloatersearch.h"
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-26 (Catznip-2.3)
#include "llfloatersearchreplace.h"
// [/SL:KB]
#include "llfloatersellland.h"
#include "llfloatersettingscolor.h"
#include "llfloatersettingsdebug.h"
#include "llfloatersidepanelcontainer.h"
#include "llfloaterslapptest.h"
#include "llfloatersnapshot.h"
#include "llfloatersounddevices.h"
#include "llfloaterspellchecksettings.h"
#include "llfloatertelehub.h"
#include "llfloatertestinspectors.h"
#include "llfloatertestlistview.h"
#include "llfloatertools.h"
#include "llfloatertoolstexturedock.h"
#include "llfloatertopobjects.h"
#include "llfloatertos.h"
#include "llfloatertoybox.h"
#include "llfloatertransactionlog.h"
#include "llfloatertranslationsettings.h"
#include "llfloateruipreview.h"
#include "llfloatervoiceeffect.h"
#include "llfloaterwebcontent.h"
#include "llfloaterwebprofile.h"
#include "llfloatervoicevolume.h"
#include "llfloaterwhitelistentry.h"
#include "llfloaterwindowsize.h"
#include "llfloaterworldmap.h"
#include "llfloaterimcontainer.h"
#include "llinspectavatar.h"
#include "llinspectgroup.h"
#include "llinspectobject.h"
#include "llinspectremoteobject.h"
#include "llinspecttoast.h"
#include "lllegacynotificationwellwindow.h"
#include "llmaterialeditor.h"
#include "llmoveview.h"
#include "llfloaterimnearbychat.h"
#include "llpanelblockedlist.h"
#include "llpanelemojicomplete.h"
#include "llpreviewanim.h"
#include "llpreviewgesture.h"
#include "llpreviewnotecard.h"
#include "llpreviewscript.h"
#include "llpreviewsound.h"
#include "llpreviewtexture.h"
#include "llscriptfloater.h"
#include "llsyswellwindow.h"

// *NOTE: Please add files in alphabetical order to keep merges easy.
// [RLVa:KB] - Checked: 2010-03-11
#include "rlvfloaters.h"
// [/RLVa:KB]
// handle secondlife:///app/openfloater/{NAME} URLs
const std::string FLOATER_PROFILE("profile");
class LLFloaterOpenHandler : public LLCommandHandler
{
public:
    // requires trusted browser to trigger or an explicit click
    LLFloaterOpenHandler() : LLCommandHandler("openfloater", UNTRUSTED_THROTTLE) { }

    bool canHandleUntrusted(
        const LLSD& params,
        const LLSD& query_map,
        LLMediaCtrl* web,
        const std::string& nav_type) override
    {
        if (params.size() != 1)
        {
            return true; // will fail silently
        }

        // handle() unescapes the name, so check the name it will actually open
        const std::string fl_name = LLURI::unescape(params[0].asString());

        // Links from untrusted content open only floaters on an explicit list; anything not
        // listed (new floaters included) stays closed until someone decides it is safe.
        // Untrusted content navigating on its own may only open floaters that just show
        // information and take no action when opened.
        static const std::set<std::string> allowlist_untrusted = {
            "about_land",
            "avatar_welcome_pack",
            "blocked",
            "conversation",
            "destinations",
            "event",
            "experiences",
            "fs_radar",
            "fs_teleporthistory",
            "gestures",
            "grid_status",
            "guidebook",
            "help_browser",
            "im_container",
            "inventory",
            "lagmeter",
            "land_holdings",
            "marketplace",
            "marketplace_listings",
            "mini_map",
            "my_environments",
            "nearby_chat",
            "people",
            "places",
            "preferences",
            "profile",
            "quick_prefs",
            "search",
            "stats",
            "toybox",
            "world_map"
        };
        // An explicit click (or the external browser, which asks the user first) may also
        // open these: windows that only show things or wait for the user to act.
        static const std::set<std::string> allowlist_clicked = {
            "about",
            "area_search",
            "beacons",
            "camera",
            "chatbar",
            "region_info",
            "region_tracker",
            "sl_ao",
            "sl_ao_editor",
            "sl_weather",
            "sound_explorer"
        };
        if (allowlist_untrusted.count(fl_name))
        {
            return true;
        }
        return (nav_type == NAV_TYPE_CLICKED || nav_type == NAV_TYPE_EXTERNAL) && allowlist_clicked.count(fl_name) != 0;
    }

    bool handle(
        const LLSD& params,
        const LLSD& query_map,
        const std::string& grid,
        LLMediaCtrl* web) override
    {
        if (params.size() != 1)
        {
            return false;
        }

        const std::string floater_name = LLURI::unescape(params[0].asString());
        LLSD key;
        if (floater_name == FLOATER_PROFILE)
        {
            key["id"] = gAgentID;
        }
        LLFloaterReg::showInstance(floater_name, key);

        return true;
    }
};

LLFloaterOpenHandler gFloaterOpenHandler;

void LLViewerFloaterReg::registerFloaters()
{
    if (gNonInteractive)
    {
        return;
    }
    // *NOTE: Please keep these alphabetized for easier merges

    LLFloaterAboutUtil::registerFloater();
    LLFloaterReg::add("360capture", "floater_360capture.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloater360Capture>);
    LLFloaterReg::add("block_timers", "floater_fast_timers.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFastTimerView>);
    LLFloaterReg::add("about_land", "floater_about_land.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLand>);
    LLFloaterReg::add("add_payment_method", "floater_add_payment_method.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAddPaymentMethod>);
    LLFloaterReg::add("appearance", "floater_my_appearance.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSidePanelContainer>);
    LLFloaterReg::add("associate_listing", "floater_associate_listing.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAssociateListing>);
    LLFloaterReg::add("auction", "floater_auction.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAuction>);
    LLFloaterReg::add("avatar_picker", "floater_avatar_picker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAvatarPicker>);
    LLFloaterReg::add("avatar_welcome_pack", "floater_avatar_welcome_pack.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAvatarWelcomePack>);
    LLFloaterReg::add("avatar_render_settings", "floater_avatar_render_settings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAvatarRenderSettings>);
    LLFloaterReg::add("avatar_textures", "floater_avatar_textures.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAvatarTextures>);

    LLFloaterReg::add("ban_duration", "floater_ban_duration.xml", &LLFloaterReg::build<LLFloaterBanDuration>);
    LLFloaterReg::add("beacons", "floater_beacons.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBeacons>);
// [SL:KB] - Patch: World-Derender | Checked: Catznip-3.2
    LLFloaterReg::add("blocked", "floater_blocked.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBlocked>);
// [/SL:KB]
    LLFloaterReg::add("bulk_perms", "floater_bulk_perms.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBulkPermission>);
    LLFloaterReg::add("buy_currency", "floater_buy_currency.xml", &LLFloaterBuyCurrency::buildFloater);
    LLFloaterReg::add("buy_currency_html", "floater_buy_currency_html.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBuyCurrencyHTML>);
    LLFloaterReg::add("buy_land", "floater_buy_land.xml", &LLFloaterBuyLand::buildFloater);
    LLFloaterReg::add("buy_object", "floater_buy_object.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBuy>);
    LLFloaterReg::add("buy_object_contents", "floater_buy_contents.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBuyContents>);
    LLFloaterReg::add("build", "floater_tools.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTools>);
    LLFloaterReg::add("tools_texture_dock", "floater_tools_texture_dock.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterToolsTextureDock>);
    LLFloaterReg::add("build_options", "floater_build_options.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBuildOptions>);
    LLFloaterReg::add("bulk_upload", "floater_bulk_upload.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBulkUpload>);
    LLFloaterReg::add("bumps", "floater_bumps.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBump>);

    LLFloaterReg::add("camera", "floater_camera.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterCamera>);
    LLFloaterReg::add("camera_presets", "floater_camera_presets.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterCameraPresets>);
    LLFloaterReg::add("recorder", "floater_recorder.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<BDFloaterCamera>);
    LLFloaterReg::add("photo_studio", "floater_photo_studio.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterPhotoStudio>);
    LLFloaterReg::add("photo_studio_pane", "floater_photo_studio_pane.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterPhotoStudioPane>);
    LLFloaterReg::add("shape_editor", "floater_shape_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterShapeEditor>);
    LLFloaterReg::add("shape_editor_pane", "floater_shape_pane.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterShapePane>);
    LLFloaterReg::add("photo_studio_history", "floater_photo_studio_history.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterPhotoStudioHistory>);
    // The standalone Scopes window: a plain floater around the Scopes panel.
    LLFloaterReg::add("scopes", "floater_scopes.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("chat_image_preview", "floater_chat_image_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterChatImage>);
    LLFloaterReg::add("chat_voice", "floater_voice_chat_volume.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterChatVoiceVolume>);
    LLFloaterReg::add("change_item_thumbnail", "floater_change_item_thumbnail.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterChangeItemThumbnail>);
    LLFloaterReg::add("nearby_chat", "floater_im_session.xml", (LLFloaterBuildFunc)&LLFloaterIMNearbyChat::buildFloater);
    LLFloaterReg::add("chat_mention_picker", "floater_chat_mention_picker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterChatMentionPicker>);
    LLFloaterReg::add("classified", "floater_classified.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterClassified>);
    LLFloaterReg::add("compile_queue", "floater_script_queue.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterCompileQueue>);
    LLFloaterReg::add("conversation", "floater_conversation_log.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterConversationLog>);
    LLFloaterReg::add("add_landmark", "floater_create_landmark.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterCreateLandmark>);

    LLFloaterReg::add("delete_pref_preset", "floater_delete_pref_preset.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterDeletePrefPreset>);
    LLFloaterReg::add("destinations", "floater_destinations.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterDestinations>);

    LLFloaterReg::add("emoji_picker", "floater_emoji_picker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterEmojiPicker>);
    LLFloaterReg::add("emoji_complete", "floater_emoji_complete.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterEmojiComplete>);

    LLFloaterReg::add("env_fixed_environmentent_water", "floater_fixedenvironment.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterFixedEnvironmentWater>);
    LLFloaterReg::add("env_fixed_environmentent_sky", "floater_fixedenvironment.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterFixedEnvironmentSky>);

    LLFloaterReg::add("env_adjust_snapshot", "floater_adjust_environment.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterEnvironmentAdjust>);

    LLFloaterReg::add("env_edit_extdaycycle", "floater_edit_ext_day_cycle.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterEditExtDayCycle>);
    LLFloaterReg::add("my_environments", "floater_my_environments.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMyEnvironment>);

    LLFloaterReg::add("event", "floater_event.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterEvent>);
    LLFloaterReg::add("experiences", "floater_experiences.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterExperiences>);
    LLFloaterReg::add("fs_teleporthistory", "floater_fs_teleporthistory.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterTeleportHistory>);
    LLFloaterReg::add("fs_radar", "floater_fs_radar.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterRadar>);
    LLFloaterReg::add("fs_kill_feed", "floater_fs_kill_feed.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterKillFeed>); // <GMDX>
    LLFloaterReg::add("fs_hit_marker", "floater_fs_hit_marker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterHitMarker>); // <GMDX>
    LLFloaterReg::add("fs_hit_marker_emojis", "floater_fs_hit_marker_emojis.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterHitMarkerEmojis>); // <GMDX>
    LLFloaterReg::add("fs_group_titles", "floater_fs_group_titles.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterGroupTitles>);
    LLFloaterReg::add("fs_group_viewer", "floater_fs_group_viewer.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterGroupViewer>); // <GMDX>
    LLFloaterReg::add("gmdx_loadouts", "floater_gmdx_loadouts.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<GMDXFloaterLoadouts>); // <GMDX>
    LLFloaterReg::add("gmdx_weapon_import", "floater_gmdx_weapon_import.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<GMDXFloaterWeaponImport>); // <GMDX>
    LLFloaterReg::add("gmdx_region_map", "floater_gmdx_region_map.xml", &GMDXRegionCapture::buildWindow); // <GMDX>
    LLFloaterReg::add("gmdx_weapon_sort", "floater_gmdx_weapon_sort.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<GMDXFloaterWeaponSort>); // <GMDX>
    LLFloaterReg::add("gmdx_loadout_toast", "floater_gmdx_loadout_toast.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<GMDXFloaterLoadoutToast>); // <GMDX>
    LLFloaterReg::add("fs_add_contact", "floater_fs_contact_add.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterAddToContactSet>);
    LLFloaterReg::add("fs_contact_set_config", "floater_fs_contact_set_configuration.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterContactSetConfiguration>);
    LLFloaterReg::add("imcontacts", "floater_fs_contacts.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterContacts>);
    LLFloaterReg::add("local_assets", "floater_local_assets.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLocalAssets>);
    LLFloaterReg::add("experience_profile", "floater_experienceprofile.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterExperienceProfile>);
    LLFloaterReg::add("experience_search", "floater_experience_search.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterExperiencePicker>);

    LLFloaterReg::add("font_test", "floater_font_test.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterFontTest>);
    LLFloaterReg::add("forget_username", "floater_forget_user.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterForgetUser>);

    LLFloaterReg::add("gestures", "floater_gesture.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGesture>);
    LLFloaterReg::add("game_control_gyro", "floater_game_control_gyro.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SSFloaterGameControlGyro>); // <SS:GameControl>
    LLFloaterReg::add("game_control_overlay", "floater_game_control_overlay.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SSFloaterGameControlOverlay>); // <SS:GameControl>
    LLFloaterReg::add("game_control_debug", "floater_game_control_debug.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SSFloaterGameControlDebug>); // <SS:GameControl>
    LLFloaterReg::add("gesture_sets", "floater_gesture_sets.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterGestureSets>);
    LLFloaterReg::add("sl_ao", "floater_sl_ao.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterAO>);
    LLFloaterReg::add("sl_ao_editor", "floater_sl_ao_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterAOEditor>);
    LLFloaterReg::add("sl_ao_ignored", "floater_sl_ao_ignored.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterAOIgnored>);
    LLFloaterReg::add("sl_ao_import_object", "floater_sl_ao_import_object.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterAOObjectImport>);
    LLFloaterReg::add("gltf_asset_editor", "floater_gltf_asset_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGLTFAssetEditor>);
    LLFloaterReg::add("gltf_export", "floater_gltf_export.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGLTFExport>);
    LLFloaterReg::add("god_tools", "floater_god_tools.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGodTools>);
    LLFloaterReg::add("grid_status", "floater_grid_status.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGridStatus>);
    LLFloaterReg::add("group_picker", "floater_choose_group.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGroupPicker>);

    LLFloaterReg::add("help_browser", "floater_help_browser.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterHelpBrowser>);
    LLFloaterReg::add("edit_hover_height", "floater_edit_hover_height.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterHoverHeight>);
    LLFloaterReg::add("quick_prefs", "floater_quick_prefs.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterHoverHeight>);
    LLFloaterReg::add("hud", "floater_hud.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterHUD>);

    LLFloaterReg::add("impanel", "floater_im_session.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterIMSession>);
    LLFloaterReg::add("im_container", "floater_im_container.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterIMContainer>);
    LLFloaterReg::add("im_well_window", "floater_sys_well.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLIMWellWindow>);
    LLFloaterReg::add("incoming_call", "floater_incoming_call.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLIncomingCallDialog>);
    LLFloaterReg::add("inventory", "floater_my_inventory.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSidePanelContainer>);
    LLFloaterReg::add("inspect", "floater_inspect.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterInspect>);
    LLFloaterReg::add("inventory_thumbnails_helper", "floater_inventory_thumbnails_helper.xml", (LLFloaterBuildFunc) &LLFloaterReg::build<LLFloaterInventoryThumbnailsHelper>);
    LLFloaterReg::add("item_properties", "floater_item_properties.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterItemProperties>);
    LLFloaterReg::add("task_properties", "floater_task_properties.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterItemProperties>);
    LLFloaterReg::add("inventory_settings", "floater_inventory_settings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterInventorySettings>);
    LLInspectAvatarUtil::registerFloater();
    LLInspectGroupUtil::registerFloater();
    LLInspectObjectUtil::registerFloater();
    LLInspectRemoteObjectUtil::registerFloater();
    LLFloaterVoiceVolumeUtil::registerFloater();
    LLNotificationsUI::registerFloater();
    LLFloaterDisplayNameUtil::registerFloater();

    LLFloaterReg::add("lagmeter", "floater_lagmeter.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLagMeter>);
    LLFloaterReg::add("land_holdings", "floater_land_holdings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLandHoldings>);
    LLFloaterReg::add("linkreplace", "floater_linkreplace.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLinkReplace>);
    LLFloaterReg::add("load_pref_preset", "floater_load_pref_preset.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterLoadPrefPreset>);

    LLFloaterReg::add("mem_leaking", "floater_mem_leaking.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMemLeak>);

    LLFloaterReg::add("media_settings", "floater_media_settings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMediaSettings>);
    LLFloaterReg::add("marketplace", "floater_marketplace.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMarketplace>);
    LLFloaterReg::add("marketplace_listings", "floater_marketplace_listings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMarketplaceListings>);
    LLFloaterReg::add("marketplace_validation", "floater_marketplace_validation.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMarketplaceValidation>);
    LLFloaterReg::add("message_critical", "floater_critical.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTOS>);
    LLFloaterReg::add("message_tos", "floater_tos.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTOS>);
    LLFloaterReg::add("moveview", "floater_moveview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMove>);
    LLFloaterReg::add("mute_object_by_name", "floater_mute_object.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGetBlockedObjectName>);
    LLFloaterReg::add("mini_map", "floater_map.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMap>);
    LLFloaterReg::add("new_feature_notification", "floater_new_feature_notification.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterNewFeatureNotification>);

    LLFloaterReg::add("notifications_console", "floater_notifications_console.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterNotificationConsole>);

    if(!gSkinSettings.getBOOL("LegacyNotificationWell"))
    {
        LLFloaterReg::add("notification_well_window", "floater_notifications_tabbed.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterNotificationsTabbed>);
    }
    else
    {
        LLFloaterReg::add("legacy_notification_well_window", "floater_legacy_sys_well.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLLegacyNotificationWellWindow>);
    }

    LLFloaterReg::add("object_weights", "floater_object_weights.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterObjectWeights>);
    LLFloaterReg::add("openobject", "floater_openobject.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterOpenObject>);
    LLFloaterReg::add("outgoing_call", "floater_outgoing_call.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLOutgoingCallDialog>);
    LLFloaterPayUtil::registerFloater();

    LLFloaterReg::add("sound_explorer", "floater_sound_explorer.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterSoundExplorer>);
    LLFloaterReg::add("sl_speaker_setup", "floater_sl_speaker_setup.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterSpeakerSetup>);
    LLFloaterReg::add("sl_weather", "floater_sl_weather.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterWeather>);
    LLFloaterReg::add("area_search", "floater_fs_area_search.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSAreaSearch>);
    LLFloaterReg::add("pathfinding_characters", "floater_pathfinding_characters.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPathfindingCharacters>);
    LLFloaterReg::add("pathfinding_linksets", "floater_pathfinding_linksets.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPathfindingLinksets>);
    LLFloaterReg::add("pathfinding_console", "floater_pathfinding_console.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPathfindingConsole>);
    LLFloaterReg::add("people", "floater_people.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSidePanelContainer>);
    LLFloaterReg::add("performance", "floater_performance.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPerformance>);
    LLFloaterReg::add("perms_default", "floater_perms_default.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPermsDefault>);
    LLFloaterReg::add("places", "floater_places.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSidePanelContainer>);
    LLFloaterReg::add("preferences", "floater_preferences.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreference>);
    LLFloaterReg::add("prefs_graphics_advanced", "floater_preferences_graphics_advanced.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreferenceGraphicsAdvanced>);
    LLFloaterReg::add("prefs_view_advanced", "floater_preferences_view_advanced.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreferenceViewAdvanced>);
    LLFloaterReg::add("prefs_proxy", "floater_preferences_proxy.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreferenceProxy>);
    LLFloaterReg::add("prefs_spellchecker_import", "floater_spellcheck_import.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSpellCheckerImport>);
    LLFloaterReg::add("prefs_translation", "floater_translation_settings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTranslationSettings>);
    LLFloaterReg::add("prefs_spellchecker", "floater_spellcheck.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSpellCheckerSettings>);
    LLFloaterReg::add("prefs_autoreplace", "floater_autoreplace.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAutoReplaceSettings>);
    LLFloaterReg::add("pref_joystick", "floater_joystick.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterJoystick>);
    LLFloaterReg::add("ocol_mist", "floater_ocol_mist.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloater>);   // <OCOL> OCOL mist sliders (settings-bound)
    LLFloaterReg::add("preview_anim", "floater_preview_animation.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewAnim>, "preview");
    LLFloaterReg::add("preview_conversation", "floater_conversation_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterConversationPreview>);
    LLFloaterReg::add("preview_gesture", "floater_preview_gesture.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewGesture>, "preview");
    LLFloaterReg::add("preview_notecard", "floater_preview_notecard.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewNotecard>, "preview");
    LLFloaterReg::add("preview_script", "floater_script_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewLSL>, "preview");
    LLFloaterReg::add("preview_scriptedit", "floater_live_lsleditor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLLiveLSLEditor>, "preview");
    LLFloaterReg::add("preview_sound", "floater_preview_sound.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewSound>, "preview");
    LLFloaterReg::add("preview_texture", "floater_preview_texture.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLPreviewTexture>, "preview");
    LLFloaterReg::add("preview_trash", "floater_preview_trash.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPreviewTrash>);
    LLFloaterReg::add("publish_classified", "floater_publish_classified.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterPublishClassified>);
    LLFloaterReg::add("save_pref_preset", "floater_save_pref_preset.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSavePrefPreset>);
    LLFloaterReg::add("save_camera_preset", "floater_save_camera_preset.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSaveCameraPreset>);
    LLFloaterReg::add("save_outfit_as", "floater_outfit_save_as.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSaveOutfitAs>);
    LLFloaterReg::add("script_colors", "floater_script_ed_prefs.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterScriptEdPrefs>);

    LLFloaterReg::add("material_editor", "floater_material_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLMaterialEditor>);
    LLFloaterReg::add("live_material_editor", "floater_live_material_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLMaterialEditor>);

    LLFloaterReg::add("telehubs", "floater_telehub.xml",&LLFloaterReg::build<LLFloaterTelehub>);
    LLFloaterReg::add("test_inspectors", "floater_test_inspectors.xml", &LLFloaterReg::build<LLFloaterTestInspectors>);
    //LLFloaterReg::add("test_list_view", "floater_test_list_view.xml",&LLFloaterReg::build<LLFloaterTestListView>);
    LLFloaterReg::add("test_textbox", "floater_test_textbox.xml", &LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("test_text_editor", "floater_test_text_editor.xml", &LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("test_widgets", "floater_test_widgets.xml", &LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("top_objects", "floater_top_objects.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTopObjects>);
    LLFloaterReg::add("toybox", "floater_toybox.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterToybox>);

    LLFloaterReg::add("reporter", "floater_report_abuse.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterReporter>);
    LLFloaterReg::add("report_bug", "floater_report_bug.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterReportBug>);
    LLFloaterReg::add("reset_queue", "floater_script_queue.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterResetQueue>);
    LLFloaterReg::add("region_debug_console", "floater_region_debug_console.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterRegionDebugConsole>);
    LLFloaterReg::add("region_info", "floater_region_info.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterRegionInfo>);
    LLFloaterReg::add("region_restarting", "floater_region_restarting.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterRegionRestarting>);
// [RLVa:KB] - Checked: 2010-03-11 (RLVa-1.2.0e) | Added: RLVa-1.2.0a
    LLFloaterReg::add("rlv_behaviours", "floater_rlv_behaviours.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<RlvFloaterBehaviours>);
    LLFloaterReg::add("rlv_console", "floater_rlv_console.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<RlvFloaterConsole>);
    LLFloaterReg::add("rlv_locks", "floater_rlv_locks.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<RlvFloaterLocks>);
    LLFloaterReg::add("rlv_strings", "floater_rlv_strings.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<RlvFloaterStrings>);
// [/RLVa:KB]

    LLFloaterReg::add("script_debug", "floater_script_debug.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterScriptDebug>);
    LLFloaterReg::add("script_debug_output", "floater_script_debug_panel.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterScriptDebugOutput>);
    LLFloaterReg::add("script_floater", "floater_script.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLScriptFloater>);
    LLFloaterReg::add("script_limits", "floater_script_limits.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterScriptLimits>);
    LLFloaterReg::add("my_scripts", "floater_my_scripts.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMyScripts>);
    LLFloaterReg::add("sell_land", "floater_sell_land.xml", &LLFloaterSellLand::buildFloater);
    LLFloaterReg::add("settings_color", "floater_settings_color.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSettingsColor>);
    LLFloaterReg::add("settings_debug", "floater_settings_debug.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSettingsDebug>);
    LLFloaterReg::add("sound_devices", "floater_sound_devices.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSoundDevices>);
    LLFloaterReg::add("stats", "floater_stats.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("start_queue", "floater_script_queue.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterRunQueue>);
    LLFloaterReg::add("scene_load_stats", "floater_scene_load_stats.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSceneLoadStats>);
    LLFloaterReg::add("stop_queue", "floater_script_queue.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterNotRunQueue>);
    LLFloaterReg::add("snapshot", "floater_snapshot.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSnapshot>);
    LLFloaterReg::add("simple_snapshot", "floater_simple_snapshot.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSimpleSnapshot>);
    //LLFloaterReg::add("search", "floater_search.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSearch>);
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-26 (Catznip-2.3)
    LLFloaterReg::add("search_replace", "floater_search_replace.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSearchReplace>);
// [/SL:KB]
    LLFloaterReg::add("profile", "floater_profile.xml",(LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterProfile>);
    LLFloaterReg::add("guidebook", "floater_how_to.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterHowTo>);
    LLFloaterReg::add("slapp_test", "floater_test_slapp.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSLappTest>);

    LLFloaterReg::add("big_preview", "floater_big_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBigPreview>);

    LLFloaterUIPreviewUtil::registerFloater();
    LLFloaterReg::add("upload_anim_bvh", "floater_animation_bvh_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterBvhPreview>, "upload");
    LLFloaterReg::add("upload_anim_anim", "floater_animation_anim_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterAnimPreview>, "upload");
    LLFloaterReg::add("upload_image", "floater_image_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterImagePreview>, "upload");
    LLFloaterReg::add("upload_model", "floater_model_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterModelPreview>, "upload");
    LLFloaterReg::add("mesh_bulk_upload", "floater_mesh_bulk_upload.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterMeshBulkUpload>);
    LLFloaterReg::add("upload_script", "floater_script_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterScriptPreview>, "upload");
    LLFloaterReg::add("upload_sound", "floater_sound_preview.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterSoundPreview>, "upload");

    LLFloaterReg::add("voice_effect", "floater_voice_effect.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterVoiceEffect>);

    LLFloaterReg::add("web_content", "floater_web_content.xml", (LLFloaterBuildFunc)&LLFloaterWebContent::create);
    LLFloaterReg::add("whitelist_entry", "floater_whitelist_entry.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterWhiteListEntry>);
    LLFloaterReg::add("window_size", "floater_window_size.xml", &LLFloaterReg::build<LLFloaterWindowSize>);
    LLFloaterReg::add("world_map", "floater_world_map.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterWorldMap>);

    LLFloaterReg::add("fs_poser", "floater_fs_poser.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<FSFloaterPoser>); // <FS:AR> [FIRE-30873]: Poser
    LLFloaterReg::add("sl_pose_studio", "floater_sl_pose_studio.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterPoseStudio>);
    LLFloaterReg::add("sl_pose_creator", "floater_sl_pose_creator.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterPoseCreator>);
    LLFloaterReg::add("sl_first_run", "floater_sl_first_run.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterFirstRun>);
    LLFloaterReg::add("viewer_import", "floater_sl_viewer_import.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterViewerImport>);
    // *NOTE: Please keep these alphabetized for easier merges

    LLFloaterReg::add("delete_queue", "floater_script_queue.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterDeleteQueue>);
    LLFloaterReg::add("generic_text", "floater_generic_text.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGenericText>);
    LLFloaterReg::add("group_profile", "floater_group_profile.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterGroupProfile>);
    LLFloaterReg::add("legacy_profile", "floater_profile_legacy.xml", (LLFloaterBuildFunc) &LLFloaterReg::build<LLFloaterProfileLegacy>);
    LLFloaterReg::add("music_ticker", "floater_music_ticker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloater>);
    LLFloaterReg::add("chatbar", "floater_sl_chatbar.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLChatBar>);
    LLFloaterReg::add("particle_editor", "floater_particle_editor.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterParticleEditor>);
    LLFloaterReg::add("progress_view", "floater_progress_view.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterProgressView>);
    LLFloaterReg::add("region_tracker", "floater_region_tracker.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<SLFloaterRegionTracker>);
    LLFloaterReg::add("search", "floater_directory.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterDirectory>);
    LLFloaterReg::add("transaction_log", "floater_transaction_log.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLFloaterTransactionLog>);
    LLFloaterReg::add("webprofile", "floater_web_profile.xml", (LLFloaterBuildFunc)&LLFloaterWebProfile::create);

    LLFloaterReg::registerControlVariables(); // Make sure visibility and rect controls get preserved when saving
}
