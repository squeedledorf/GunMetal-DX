/**
 * @file fspanelradar.cpp
 * @brief Firestorm radar panel implementation
 *
 * $LicenseInfo:firstyear=2013&license=viewerlgpl$
 * Copyright (c) 2013 Ansariel Hiller @ Second Life
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h"

#include "fspanelradar.h"
#include "llavatariconloader.h"

// libs
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llmenubutton.h"
#include "llscrolllistcell.h"
#include "llscrolllistitem.h"
#include "lltoggleablemenu.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"

// newview
#include "slavataractions.h"
#include "fsradarmenu.h"
#include "llagent.h"
#include "llviewerregion.h"
#include "llavataractions.h"
#include "llavatariconctrl.h"
#include "llmutelist.h"
#include "lltimer.h"
#include "lluiimage.h"
#include "llcallingcard.h"
#include "llnetmap.h"
#include "llviewercontrol.h"        // for gSavedSettings
#include "llviewermenu.h"           // for gMenuHolder
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

/**
 * Update buttons on changes in our friend relations (STORM-557).
 */
class FSButtonsUpdater : public FSRadar::Updater, public LLFriendObserver
{
public:
    FSButtonsUpdater(callback_t cb)
    :   FSRadar::Updater(cb)
    {
        LLAvatarTracker::instance().addObserver(this);
    }

    ~FSButtonsUpdater() override
    {
        LLAvatarTracker::instance().removeObserver(this);
    }

    void changed(U32) override
    {
        update();
    }
};

// Sort order of the old Nearby list / the earlier radar options menu (NearbyPeopleSortOrder,
// values from LLPanelPeople::ESortOrder; age is radar-only). Only read to seed FSRadarSortColumn.
enum ERadarSortOrder : U32
{
    RADAR_SORT_BY_NAME = 0,
    RADAR_SORT_BY_DISTANCE = 3,
    RADAR_SORT_BY_RECENT_ARRIVAL = 5,
    RADAR_SORT_BY_AGE = 6
};

// Double-click behaviour, shared with the old Nearby list (SlipstreamNearbyPeopleClickAction)
enum ERadarClickAction : U32
{
    RADAR_CLICK_TO_IM = 0,
    RADAR_CLICK_TO_PROFILE,
    RADAR_CLICK_TO_ZOOM,
    RADAR_CLICK_TO_TELEPORT
};

//=============================================================================

static LLPanelInjector<FSPanelRadar> t_fs_panel_radar("fs_panel_radar");

FSPanelRadar::FSPanelRadar()
    :   LLPanel(),
        mRadarGearButton(nullptr),
        mOptionsButton(nullptr),
        mMiniMap(nullptr),
        mAddFriendButton(nullptr),
        mFilterEditor(nullptr),
        mFilterSubString(LLStringUtil::null),
        mFilterSubStringOrig(LLStringUtil::null),
        mRadarList(nullptr),
        mVisibleCheckFunction(),
        mUpdateSignalConnection(),
        mFSRadarColumnConfigConnection(),
        mLastResizeDelta(0)
{
    mButtonsUpdater = std::make_unique<FSButtonsUpdater>(boost::bind(&FSPanelRadar::updateButtons, this));
    mCommitCallbackRegistrar.add("Radar.AddFriend", boost::bind(&FSPanelRadar::onAddFriendButtonClicked,    this));
    mCommitCallbackRegistrar.add("Radar.Gear",      boost::bind(&FSPanelRadar::onGearButtonClicked,         this, _1));

    mColumnBits["name"] = 1;
    mColumnBits["voice_level"] = 2;
    mColumnBits["in_region"] = 4;
    mColumnBits["typing_status"] = 8;
    mColumnBits["sitting_status"] = 16;
    mColumnBits["flags"] = 32;
    mColumnBits["age"] = 64;
    mColumnBits["seen"] = 128;
    mColumnBits["range"] = 256;
    mColumnBits["has_notes"] = 512;
    mColumnBits["avatar_icon"] = 1024;
}

FSPanelRadar::~FSPanelRadar()
{
    if (mUpdateSignalConnection.connected())
    {
        mUpdateSignalConnection.disconnect();
    }

    if (mFSRadarColumnConfigConnection.connected())
    {
        mFSRadarColumnConfigConnection.disconnect();
    }

    if (mSortColumnConnection.connected())
    {
        mSortColumnConnection.disconnect();
    }

    if (mSortAscendingConnection.connected())
    {
        mSortAscendingConnection.disconnect();
    }

    if (mOptionsMenuHandle.get())
        mOptionsMenuHandle.get()->die();
}

bool FSPanelRadar::postBuild()
{
    mRadarList = getChild<FSRadarListCtrl>("radar_list");
    mRadarList->setContextMenu(&FSFloaterRadarMenu::gFSRadarMenu);
    mRadarList->setDoubleClickCallback(boost::bind(&FSPanelRadar::onRadarListDoubleClicked, this));
    mRadarList->setCommitCallback(boost::bind(&FSPanelRadar::onRadarListCommitted, this));
    mRadarList->setSortChangedCallback(boost::bind(&FSPanelRadar::onRadarListSortChanged, this));

    mAddFriendButton = getChild<LLButton>("add_friend_btn");

    mFilterEditor = getChild<LLFilterEditor>("nearby_filter_input");
    mFilterEditor->setCommitCallback(boost::bind(&FSPanelRadar::onFilterEdit, this, _2));

    // Create menus.
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("Radar.Option.Action",    boost::bind(&FSPanelRadar::onOptionsMenuItemClicked, this, _2));
    registrar.add("Radar.NameFmt",          boost::bind(&FSRadar::onRadarNameFmtClicked, _2));
    registrar.add("Radar.ReportTo",         boost::bind(&FSRadar::onRadarReportToClicked, _2));
    registrar.add("Radar.ToggleColumn",     boost::bind(&FSPanelRadar::onColumnVisibilityChecked, this, _2));
    registrar.add("Radar.ClickAction",      boost::bind(&FSPanelRadar::onClickActionMenuItemClicked, this, _2));

    enable_registrar.add("Radar.NameFmtCheck",      boost::bind(&FSRadar::radarNameFmtCheck, _2));
    enable_registrar.add("Radar.ReportToCheck",     boost::bind(&FSRadar::radarReportToCheck, _2));
    enable_registrar.add("Radar.EnableColumn",      boost::bind(&FSPanelRadar::onEnableColumnVisibilityChecked, this, _2));
    enable_registrar.add("Radar.ClickActionCheck",  boost::bind(&FSPanelRadar::onClickActionMenuItemCheck, this, _2));

    mRadarGearButton = getChild<LLButton>("gear_btn");
    mOptionsButton = getChild<LLMenuButton>("options_btn");

    if (LLToggleableMenu* options_menu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>("menu_fs_radar_options.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()); options_menu)
    {
        mOptionsMenuHandle = options_menu->getHandle();
        mOptionsButton->setMenu(options_menu, LLMenuButton::MP_BOTTOM_LEFT);
    }

    mFSRadarColumnConfigConnection = gSavedSettings.getControl("FSRadarColumnConfig")->getSignal()->connect(boost::bind(&FSPanelRadar::onColumnDisplayModeChanged, this));
    // Both radars (People tab and the standalone floater) follow the shared sort settings
    mSortColumnConnection = gSavedSettings.getControl("FSRadarSortColumn")->getSignal()->connect(boost::bind(&FSPanelRadar::applySortOrder, this));
    mSortAscendingConnection = gSavedSettings.getControl("FSRadarSortAscending")->getSignal()->connect(boost::bind(&FSPanelRadar::applySortOrder, this));
    onColumnDisplayModeChanged();

    // Register for radar updates
    mUpdateSignalConnection = FSRadar::getInstance()->setUpdateCallback(boost::bind(&FSPanelRadar::updateList, this, _1, _2));

    // call this method in case some list is empty and buttons can be in inconsistent state
    updateButtons();

    return true;
}

void FSPanelRadar::updateButtons()
{
    LLUUID selected_id;

    uuid_vec_t selected_uuids;
    getCurrentItemIDs(selected_uuids);

    bool item_selected = (selected_uuids.size() == 1);

    // Check whether selected avatar is our friend.
    bool is_friend = true;
    if (item_selected)
    {
        selected_id = selected_uuids.front();
        is_friend = LLAvatarTracker::instance().getBuddyInfo(selected_id) != NULL;
    }
    mAddFriendButton->setEnabled(!is_friend && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES));
    mRadarGearButton->setEnabled(selected_uuids.size() > 0 && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES));
}

LLUUID FSPanelRadar::getCurrentItemID() const
{
    if (LLScrollListItem* item = mRadarList->getFirstSelected(); item)
    {
        return item->getUUID();
    }

    return LLUUID::null;
}

void FSPanelRadar::getCurrentItemIDs(uuid_vec_t& selected_uuids) const
{
    for (auto selected_item : mRadarList->getAllSelected())
    {
        selected_uuids.emplace_back(selected_item->getUUID());
    }
}

// FS filters inside its patched LLScrollListCtrl; the radar rebuilds every row on each
// update anyway, so the filter is applied while the rows are built instead.
bool FSPanelRadar::isFilteredOut(const std::string& name) const
{
    if (mFilterSubString.empty())
    {
        return false;
    }
    std::string name_upper = name;
    LLStringUtil::toUpper(name_upper);
    return name_upper.find(mFilterSubString) == std::string::npos;
}

void FSPanelRadar::onFilterEdit(const std::string& search_string)
{
    mFilterSubStringOrig = search_string;
    LLStringUtil::trimHead(mFilterSubStringOrig);
    // Searches are case-insensitive
    std::string search_upper = mFilterSubStringOrig;
    LLStringUtil::toUpper(search_upper);

    if (mFilterSubString == search_upper)
    {
        return;
    }

    mFilterSubString = search_upper;

    // Apply new filter.
    requestUpdate();
}

void FSPanelRadar::onRadarListDoubleClicked()
{
    LLScrollListItem* item = mRadarList->getFirstSelected();
    if (!item)
    {
        return;
    }

    const LLUUID clicked_id = item->getUUID();
    if (clicked_id.isNull() || clicked_id == gAgentID)
    {
        return;
    }

    // [RLVa] Don't give away identities through the double-click actions
    if (RlvActions::isRlvEnabled() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, clicked_id))
    {
        return;
    }

    static LLCachedControl<U32> click_action(gSavedSettings, "SlipstreamNearbyPeopleClickAction", RADAR_CLICK_TO_IM);
    switch (click_action())
    {
        default:
        case RADAR_CLICK_TO_IM:
            LLAvatarActions::startIM(clicked_id);
            break;
        case RADAR_CLICK_TO_PROFILE:
            LLAvatarActions::showProfile(clicked_id);
            break;
        case RADAR_CLICK_TO_ZOOM:
        {
            const std::string name = item->getColumn(mRadarList->getColumn("name")->mIndex)->getValue().asString();
            FSRadar::getInstance()->zoomAvatar(clicked_id, name);
            break;
        }
        case RADAR_CLICK_TO_TELEPORT:
            SLAvatarActions::teleportTo(clicked_id);
            break;
    }
}

void FSPanelRadar::onClickActionMenuItemClicked(const LLSD& userdata)
{
    const std::string chosen_item = userdata.asString();
    if (chosen_item == "click_im")
        gSavedSettings.setU32("SlipstreamNearbyPeopleClickAction", RADAR_CLICK_TO_IM);
    else if (chosen_item == "click_profile")
        gSavedSettings.setU32("SlipstreamNearbyPeopleClickAction", RADAR_CLICK_TO_PROFILE);
    else if (chosen_item == "click_zoom")
        gSavedSettings.setU32("SlipstreamNearbyPeopleClickAction", RADAR_CLICK_TO_ZOOM);
    else if (chosen_item == "click_teleport")
        gSavedSettings.setU32("SlipstreamNearbyPeopleClickAction", RADAR_CLICK_TO_TELEPORT);
}

bool FSPanelRadar::onClickActionMenuItemCheck(const LLSD& userdata)
{
    const std::string item = userdata.asString();
    const U32 click_action = gSavedSettings.getU32("SlipstreamNearbyPeopleClickAction");
    if (item == "click_im")
        return click_action == RADAR_CLICK_TO_IM;
    else if (item == "click_profile")
        return click_action == RADAR_CLICK_TO_PROFILE;
    else if (item == "click_zoom")
        return click_action == RADAR_CLICK_TO_ZOOM;
    else if (item == "click_teleport")
        return click_action == RADAR_CLICK_TO_TELEPORT;
    return false;
}

void FSPanelRadar::onRadarListCommitted()
{
    if (mMiniMap)
    {
        uuid_vec_t selected_uuids;
        getCurrentItemIDs(selected_uuids);
        mMiniMap->setSelected(selected_uuids);
    }

    updateButtons();
    mChangeSignal();
}

void FSPanelRadar::onAddFriendButtonClicked()
{
    if (LLUUID id = getCurrentItemID(); id.notNull())
    {
        LLAvatarActions::requestFriendshipDialog(id);
    }
}

void FSPanelRadar::onOptionsMenuItemClicked(const LLSD& userdata)
{
    std::string chosen_item = userdata.asString();

    if (chosen_item == "panel_block_list_sidetray")
    {
        // Our block list floater (covers more than the FS one)
        LLFloaterReg::showInstance("blocked");
    }
}

void FSPanelRadar::onGearButtonClicked(LLUICtrl* btn)
{
    uuid_vec_t selected_uuids;
    getCurrentItemIDs(selected_uuids);
    // Spawn at bottom left corner of the button.
    FSFloaterRadarMenu::gFSRadarMenu.show(btn, selected_uuids, 0, 0);
}

void FSPanelRadar::requestUpdate()
{
    std::vector<LLSD> entries;
    LLSD stats;
    FSRadar::getInstance()->getCurrentData(entries, stats);
    updateList(entries, stats);
}

void FSPanelRadar::updateList(const std::vector<LLSD>& entries, const LLSD& stats)
{
    if (mVisibleCheckFunction && !mVisibleCheckFunction())
    {
        return;
    }

    static const std::string flagsColumnValues [3] = { getString("FlagsColumnValue_0"), getString("FlagsColumnValue_1"), getString("FlagsColumnValue_2") };
    static const std::string flagsToolTips [3] = { LLStringUtil::null, getString("PaymentOnFileToolTip"), getString("PaymentUsedToolTip") };
    static const std::string notesColumnIcon = getString("NotesColumnIcon");
    static const std::string inRegionGlyph = getString("InRegionGlyph");
    static const std::string sittingGlyph = getString("SittingGlyph");
    static const std::string typingGlyph = getString("TypingGlyph");
    static const std::string inRegionToolTip = getString("InRegionToolTip");
    static const std::string onParcelToolTip = getString("OnParcelToolTip");
    static const std::string sittingToolTip = getString("SittingToolTip");
    static const std::string typingToolTip = getString("TypingToolTip");

    LLUIColorTable& colortable = LLUIColorTable::instance();
    static const LLUIColor sameParcelColor = colortable.getColor("AvatarListItemSameParcel", LLColor4::white);
    static const LLUIColor sameRegionColor = colortable.getColor("AvatarListItemSameRegion", LLColor4::grey);
    static const LLUIColor typingColor = colortable.getColor("AvatarListItemTyping", LLColor4::white);
    static const LLUIColor glyphColor = colortable.getColor("AvatarListItemStatusGlyph", LLColor4::grey);

    // Store current selection and scroll position
    LLUUID last_selected_id;
    if (mRadarList->getLastSelectedItem())
    {
        last_selected_id = mRadarList->getLastSelectedItem()->getUUID();
    }
    uuid_vec_t selected_ids;
    for (auto selected_item : mRadarList->getAllSelected())
    {
        selected_ids.emplace_back(selected_item->getUUID());
    }
    S32 lastScroll = mRadarList->getScrollPos();

    // Update list
    mRadarList->setCommentText(RlvActions::canShowNearbyAgents() ? LLStringUtil::null : RlvStrings::getString("blocked_nearby"));

    bool needs_sort = mRadarList->isSorted();
    mRadarList->setNeedsSort(false);

    const S32 rangeColumnIndex = mRadarList->getColumn("range")->mIndex;
    const S32 nameColumnIndex = mRadarList->getColumn("name")->mIndex;
    const S32 voiceLevelColumnIndex = mRadarList->getColumn("voice_level")->mIndex;
    const S32 inRegionColumnIndex = mRadarList->getColumn("in_region")->mIndex;
    const S32 typingColumnIndex = mRadarList->getColumn("typing_status")->mIndex;
    const S32 sittingColumnIndex = mRadarList->getColumn("sitting_status")->mIndex;
    const S32 flagsColumnIndex = mRadarList->getColumn("flags")->mIndex;
    const S32 ageColumnIndex = mRadarList->getColumn("age")->mIndex;

    // #267 optional profile picture column.  With it off nothing below touches the icon cells
    // (they stay empty: zero height, no requests), so rows / sorting are unchanged.
    LLScrollListColumn* iconColumn = mRadarList->getColumn("avatar_icon");
    const S32 iconColumnIndex = iconColumn ? iconColumn->mIndex : -1;
    constexpr S32 RADAR_ICON_SIZE = 16;
    constexpr S32 RADAR_ICON_COL_BIT = 1024;
    const bool show_icons = iconColumn && (gSavedSettings.getU32("FSRadarColumnConfig") & RADAR_ICON_COL_BIT)
                            && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES) && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNEARBY);
    // Only rows in (or just around) the visible window are given pictures / requests
    const S32 first_visible_row = llmax(0, lastScroll - 1);
    const S32 last_visible_row = lastScroll + mRadarList->getLinesPerPage() + 1;
    if (show_icons && !mIconConnection.connected())
    {
        mIconConnection = LLAvatarIconLoader::instance().setUpdateCallback([this]() { requestUpdate(); });
    }
    else if (!show_icons)
    {
        mIconConnection.disconnect();
    }

    mRadarList->clearRows();
    for (const auto& avdata : entries)
    {
        // Same face as the Friends / Groups / Recent avatar list items (SansSerifSmall)
        constexpr char default_font[] = "SANSSERIF_SMALL";

        LLSD entry = avdata["entry"];
        LLSD options = avdata["options"];

        if (isFilteredOut(entry["name"].asString()))
        {
            continue;
        }

        const bool on_parcel = entry["on_parcel"].asBoolean();
        const bool in_region = entry["in_region"].asBoolean();
        const bool typing = entry["typing"].asBoolean();
        const bool sitting = entry["sitting"].asBoolean();
        const S32 flags = entry.has("flags") ? llclamp(entry["flags"].asInteger(), 0, 2) : 0;

        LLSD row_data;
        row_data["value"] = entry["id"];
        row_data["columns"][0]["column"] = "name";
        row_data["columns"][0]["value"] = entry["name"];
        row_data["columns"][0]["font"] = default_font;

        row_data["columns"][1]["column"] = "voice_level";
        row_data["columns"][1]["type"] = "icon";
        row_data["columns"][1]["value"] = ""; // set after the row is created so the 18px icon doesn't grow the line height

        row_data["columns"][2]["column"] = "in_region";
        row_data["columns"][2]["value"] = (on_parcel || in_region) ? inRegionGlyph : "";
        row_data["columns"][2]["font"] = default_font;
        row_data["columns"][2]["halign"] = "center";
        row_data["columns"][2]["tool_tip"] = on_parcel ? onParcelToolTip : (in_region ? inRegionToolTip : "");

        row_data["columns"][3]["column"] = "typing_status";
        row_data["columns"][3]["value"] = typing ? typingGlyph : "";
        row_data["columns"][3]["font"] = default_font;
        row_data["columns"][3]["halign"] = "center";
        row_data["columns"][3]["tool_tip"] = typing ? typingToolTip : "";

        row_data["columns"][4]["column"] = "sitting_status";
        row_data["columns"][4]["value"] = sitting ? sittingGlyph : "";
        row_data["columns"][4]["font"] = default_font;
        row_data["columns"][4]["halign"] = "center";
        row_data["columns"][4]["tool_tip"] = sitting ? sittingToolTip : "";

        row_data["columns"][5]["column"] = "flags";
        row_data["columns"][5]["value"] = flagsColumnValues[flags];
        row_data["columns"][5]["font"] = default_font;
        row_data["columns"][5]["tool_tip"] = flagsToolTips[flags];

        row_data["columns"][6]["column"] = "has_notes";
        row_data["columns"][6]["type"] = "icon";
        row_data["columns"][6]["value"] = (entry["notes"].asString().empty() ? "" : notesColumnIcon);
        row_data["columns"][6]["tool_tip"] = entry["notes"].asString();

        // Compact, unit-suffixed numbers (the header columns are narrow)
        std::string age_text = entry["age"].asString();
        const S32 age_days = entry.has("age_days") ? entry["age_days"].asInteger() : -1;
        if (age_days > -1)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", age_days);
            age_text = getString("AgeFormat", args);
        }
        row_data["columns"][7]["column"] = "age";
        row_data["columns"][7]["value"] = age_text;
        row_data["columns"][7]["halign"] = "right";
        row_data["columns"][7]["font"] = default_font;

        row_data["columns"][8]["column"] = "seen";
        row_data["columns"][8]["value"] = formatSeen(entry["seen_secs"].asInteger());
        row_data["columns"][8]["halign"] = "right";
        row_data["columns"][8]["font"] = default_font;
        row_data["columns"][8]["tool_tip"] = entry["seen"].asString();

        const F32 range_m = (F32)entry["range_m"].asReal();
        LLStringUtil::format_map_t range_args;
        range_args["[DIST]"] = (range_m < 100.f) ? llformat("%.1f", range_m) : llformat("%.0f", range_m);
        row_data["columns"][9]["column"] = "range";
        row_data["columns"][9]["value"] = getString(entry["range_known"].asBoolean() ? "RangeFormat" : "RangeBeyondFormat", range_args);
        row_data["columns"][9]["halign"] = "right";
        row_data["columns"][9]["font"] = default_font;

        // Hidden numeric sort keys (zero padded so the dictionary compare orders them numerically)
        row_data["columns"][10]["column"] = "seen_sort";
        row_data["columns"][10]["value"] = llformat("%010d_", entry["seen_secs"].asInteger()) + entry["name"].asString();

        row_data["columns"][11]["column"] = "range_sort";
        row_data["columns"][11]["value"] = entry["range_known"].asBoolean() ? llformat("%012.2f", range_m) : std::string("999999999.99");

        row_data["columns"][12]["column"] = "age_sort";
        row_data["columns"][12]["value"] = (age_days > -1) ? llformat("%08d", age_days) : std::string("99999999");

        // Parcel ranks above region (same glyph, only the colour differs)
        row_data["columns"][13]["column"] = "in_region_sort";
        row_data["columns"][13]["value"] = on_parcel ? "2" : (in_region ? "1" : "0");

        // Loudest first when sorted descending (the icon names don't sort by level)
        row_data["columns"][14]["column"] = "voice_sort";
        row_data["columns"][14]["value"] = voiceSortKey(entry["voice_level_icon"].asString());

        if (iconColumn)
        {
            row_data["columns"][15]["column"] = "avatar_icon";
            row_data["columns"][15]["type"] = "icon";
            row_data["columns"][15]["value"] = ""; // empty icon has no height; the picture is set below
        }

        LLScrollListItem* row = mRadarList->addElement(row_data);
        if (!row)
        {
            continue;
        }

        LLScrollListText* radarRangeCell = (LLScrollListText*)row->getColumn(rangeColumnIndex);
        radarRangeCell->setColor(LLColor4(options["range_color"]));
        radarRangeCell->setFontStyle((U8)options["range_style"].asInteger());

        LLScrollListText* radarNameCell = (LLScrollListText*)row->getColumn(nameColumnIndex);
        radarNameCell->setFontStyle((U8)options["name_style"].asInteger());
        if (options.has("name_color"))
        {
            radarNameCell->setColor(LLColor4(options["name_color"]));
        }

        if (entry.has("voice_level_icon"))
        {
            row->getColumn(voiceLevelColumnIndex)->setValue(entry["voice_level_icon"].asString());
        }

        if (on_parcel || in_region)
        {
            ((LLScrollListText*)row->getColumn(inRegionColumnIndex))->setColor(on_parcel ? sameParcelColor.get() : sameRegionColor.get());
        }
        if (typing)
        {
            ((LLScrollListText*)row->getColumn(typingColumnIndex))->setColor(typingColor.get());
        }
        if (sitting)
        {
            ((LLScrollListText*)row->getColumn(sittingColumnIndex))->setColor(glyphColor.get());
        }
        if (flags)
        {
            ((LLScrollListText*)row->getColumn(flagsColumnIndex))->setColor(glyphColor.get());
        }

        if (options.has("age_color"))
        {
            LLScrollListText* ageCell = (LLScrollListText*)row->getColumn(ageColumnIndex);
            ageCell->setColor(LLColor4(options["age_color"]));
        }
    }

    mRadarList->setNeedsSort(needs_sort);
    mRadarList->updateSort();

    // #267 pictures are assigned AFTER sorting: the visible window is in sorted order, not in
    // insertion order (assigning during insertion left the rows actually on screen without a
    // picture and without a request whenever the list was sorted).
    if (show_icons && iconColumnIndex >= 0)
    {
        static LLUIImagePtr default_icon = LLUI::getUIImage("Generic_Person");
        S32 sorted_row = -1;
        for (LLScrollListItem* item : mRadarList->getAllData())
        {
            ++sorted_row;
            LLScrollListIcon* icon_cell = dynamic_cast<LLScrollListIcon*>(item->getColumn(iconColumnIndex));
            if (!icon_cell)
            {
                continue;
            }
            const LLUUID av_id = item->getUUID();
            const bool visible = sorted_row >= first_visible_row && sorted_row <= last_visible_row;
            if (av_id.isNull() || LLMuteList::getInstance()->isMuted(av_id)
                || !RlvActions::canShowName(RlvActions::SNC_DEFAULT, av_id))
            {
                continue; // blank: no fetch, no identity leak
            }
            LLUIImagePtr picture = visible
                ? LLAvatarIconLoader::instance().getIcon(av_id, RADAR_ICON_SIZE, true, false) : LLUIImagePtr();
            icon_cell->setImage(picture ? picture : default_icon, RADAR_ICON_SIZE);
        }
    }

    // The counts live on the filter field (label + tooltip), keeping the Name header short
    LLStringUtil::format_map_t name_count_args;
    name_count_args["[TOTAL]"] = stats["total"].asString();
    name_count_args["[IN_REGION]"] = stats["region"].asString();
    name_count_args["[IN_CHAT_RANGE]"] = stats["chatrange"].asString();
    if (const std::string filter_label = getString("filter_label", name_count_args); filter_label != mFilterLabel)
    {
        mFilterLabel = filter_label;
        mFilterEditor->setLabel(filter_label);
    }
    mFilterEditor->setToolTip(getString("avatar_name_count", name_count_args));

    mRadarList->refreshLineHeight();

    // Restore scroll position
    mRadarList->setScrollPos(lastScroll);

    // Restore selection list
    if (!selected_ids.empty())
    {
        mRadarList->selectMultiple(selected_ids);
        if (last_selected_id.notNull())
        {
            mRadarList->setLastSelectedItem(last_selected_id);
        }
    }

    updateButtons();
    mChangeSignal();

    // Retained UI: the rows were rebuilt and cells recoloured directly; report once so the
    // recorded floater draw is replayed with the fresh distances / voice / typing state.
    onRetainedDirty();
}

void FSPanelRadar::onColumnDisplayModeChanged()
{
    U32 column_config = gSavedSettings.getU32("FSRadarColumnConfig");
    const std::vector<LLScrollListColumn::Params> column_params = mRadarList->getColumnInitParams();
    S32 column_padding = mRadarList->getColumnPadding();

    LLFloater* parent_floater{ nullptr };
    LLView* parent = getParent();
    while (parent)
    {
        parent_floater = dynamic_cast<LLFloater*>(parent);
        if (parent_floater)
        {
            break;
        }
        parent = parent->getParent();
    }

    S32 default_width{ 0 };
    S32 new_width{ 0 };

    mRadarList->clearRows();
    mRadarList->clearColumns();
    mRadarList->updateLayout();

    for (const auto& p : column_params)
    {
        default_width += (p.width.pixel_width.getValue() + column_padding);

        LLScrollListColumn::Params params;
        params.header = p.header;
        params.name = p.name;
        params.halign = p.halign;
        params.sort_direction = p.sort_direction;
        params.sort_column = p.sort_column;
        params.tool_tip = p.tool_tip;

        auto bit = mColumnBits.find(p.name.getValue());
        if (bit != mColumnBits.end() && (column_config & bit->second))
        {
            params.width = p.width;
            new_width += (params.width.pixel_width.getValue() + column_padding);
        }
        else
        {
            params.width.pixel_width.set(-1, true);
        }

        mRadarList->addColumn(params);
    }

    // FS shrinks/grows the host floater's minimum width with the visible columns. Only do this
    // when we are already attached to a floater (People builds its tabs before parenting).
    if (parent_floater)
    {
        S32 min_width, min_height;
        parent_floater->getResizeLimits(&min_width, &min_height);
        min_width -= (default_width - new_width - mLastResizeDelta);
        mLastResizeDelta = default_width - new_width;
        parent_floater->setResizeLimits(min_width, min_height);

        if (parent_floater->getRect().getWidth() < min_width)
        {
            parent_floater->reshape(min_width, parent_floater->getRect().getHeight());
        }
    }

    // Columns were recreated (sort order cleared): restore the saved header sort. Sorting reads
    // the cell data, so it still works when that column is hidden.
    applySortOrder();

    mRadarList->dirtyColumns();
    onRetainedDirty();
}

void FSPanelRadar::onColumnVisibilityChecked(const LLSD& userdata)
{
    std::string column = userdata.asString();
    U32 column_config = gSavedSettings.getU32("FSRadarColumnConfig");

    U32 new_value;
    if (U32 enabled = (mColumnBits[column] & column_config); enabled)
    {
        new_value = (column_config & ~mColumnBits[column]);
    }
    else
    {
        new_value = (column_config | mColumnBits[column]);
    }

    gSavedSettings.setU32("FSRadarColumnConfig", new_value);
}

bool FSPanelRadar::onEnableColumnVisibilityChecked(const LLSD& userdata)
{
    std::string column = userdata.asString();
    U32 column_config = gSavedSettings.getU32("FSRadarColumnConfig");

    return (mColumnBits[column] & column_config);
}

std::string FSPanelRadar::formatSeen(S32 seconds) const
{
    // Same compact units as the last-interaction time in the Friends / Recent lists
    seconds = llmax(0, seconds);
    LLStringUtil::format_map_t args;
    const char* format;
    if (seconds < 60)
    {
        args["[COUNT]"] = llformat("%d", seconds);
        format = "SeenSeconds";
    }
    else if (seconds < 3600)
    {
        args["[COUNT]"] = llformat("%d", seconds / 60);
        format = "SeenMinutes";
    }
    else if (seconds < 86400)
    {
        args["[COUNT]"] = llformat("%d", seconds / 3600);
        format = "SeenHours";
    }
    else
    {
        args["[COUNT]"] = llformat("%d", seconds / 86400);
        format = "SeenDays";
    }
    return getString(format, args);
}

// static
std::string FSPanelRadar::voiceSortKey(const std::string& icon)
{
    if (icon == "VoicePTT_Lvl3") return "5";
    if (icon == "VoicePTT_Lvl2") return "4";
    if (icon == "VoicePTT_Lvl1") return "3";
    if (icon == "VoicePTT_On")   return "2";
    if (icon == "VoicePTT_Off")  return "1";
    return "0"; // no voice / muted
}

void FSPanelRadar::applySortOrder()
{
    std::string sort_column = gSavedSettings.getString("FSRadarSortColumn");
    bool ascending = gSavedSettings.getBOOL("FSRadarSortAscending");
    if (sort_column.empty())
    {
        // Not sorted from a header yet: carry over the old menu / Nearby list choice
        ascending = true;
        switch (gSavedSettings.getU32("NearbyPeopleSortOrder"))
        {
            case RADAR_SORT_BY_NAME:
                sort_column = "name";
                break;
            case RADAR_SORT_BY_RECENT_ARRIVAL:
                // shortest time seen first = most recent arrival first
                sort_column = "seen_sort";
                break;
            case RADAR_SORT_BY_AGE:
                // youngest accounts first
                sort_column = "age_sort";
                break;
            case RADAR_SORT_BY_DISTANCE:
            default:
                sort_column = "range_sort";
                break;
        }
    }

    if (!mRadarList->getColumn(sort_column))
    {
        sort_column = "range_sort";
        ascending = true;
    }

    // Skip when already in this order: saving a header click re-enters here through the
    // setting signals, and the list was sorted by the click itself.
    if (mRadarList->getSortColumnName() != sort_column || mRadarList->getSortAscending() != ascending)
    {
        mRadarList->sortByColumn(sort_column, ascending);
    }
    onRetainedDirty();
}

void FSPanelRadar::onRadarListSortChanged()
{
    // A header click sorted this list; persist it so the other radar and the next session follow.
    // getSortColumnName() is the sort key column (e.g. range_sort for the Range header).
    const std::string sort_column = mRadarList->getSortColumnName();
    if (sort_column.empty())
    {
        return;
    }
    const bool ascending = mRadarList->getSortAscending();
    // Column first: the setting signals re-apply after each write, and an intermediate apply of
    // (new column, old direction) only re-sorts the same key, leaving the secondary order alone.
    gSavedSettings.setString("FSRadarSortColumn", sort_column);
    gSavedSettings.setBOOL("FSRadarSortAscending", ascending);
    onRetainedDirty();
}
