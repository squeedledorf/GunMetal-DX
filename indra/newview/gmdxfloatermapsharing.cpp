/**
 * @file gmdxfloatermapsharing.cpp
 * @brief GunMetal-DX Map Sharing window (see gmdxfloatermapsharing.h).
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

// <GMDX> whole file: see gmdxfloatermapsharing.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxfloatermapsharing.h"

#include "gmdxmapshare.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llmenugl.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llviewermenu.h"

#include <sstream>

namespace
{
using Target = GMDXMapShare::Target;

LLSD cell(const char* column, const LLSD& value, const char* type = "text")
{
    return LLSD().with("column", column).with("value", value).with("type", type);
}

// Rebuilds list from rows only when they differ from what it shows (shown), keeping its selection
// and scroll; true when it did.
bool fill(LLScrollListCtrl* list, const LLSD& rows, std::string& shown)
{
    std::ostringstream text;
    text << rows;
    if (text.str() == shown) return false;
    shown = text.str();
    const LLSD selected = list->getSelectedValue();
    const S32 scroll = list->getScrollPos();
    list->deleteAllItems();
    for (const LLSD& row : llsd::inArray(rows)) list->addElement(row);
    if (selected.isDefined()) list->selectByValue(selected);
    list->setScrollPos(scroll);
    return true;
}

bool listed(const LLUUID& id)
{
    const std::vector<Target>& targets = GMDXMapShare::targets();
    return std::any_of(targets.begin(), targets.end(), [&id](const Target& t) { return t.id == id; });
}

std::string age(F64 seconds)
{
    const S32 s = llmax(0, (S32)seconds);
    return s < 60 ? llformat("%d s", s) : llformat("%d min", s / 60);
}
} // namespace

GMDXFloaterMapSharing::GMDXFloaterMapSharing(const LLSD& key)
:   LLFloater(key)
{
}

GMDXFloaterMapSharing::~GMDXFloaterMapSharing()
{
    if (LLContextMenu* menu = mMenu.get())
    {
        gMenuHolder->removeChild(menu);
        delete menu;
    }
}

bool GMDXFloaterMapSharing::postBuild()
{
    mMyGroup = getChild<LLCheckBoxCtrl>("my_group");
    mTargets = getChild<LLScrollListCtrl>("target_list");
    mAlways = getChild<LLScrollListCtrl>("always_list");
    mBlocked = getChild<LLScrollListCtrl>("blocked_list");
    mReceived = getChild<LLScrollListCtrl>("received_list");

    auto selected = [](LLScrollListCtrl* list) { return list->getSelectedValue().asUUID(); };
    auto on_click = [this](const char* name, std::function<void()> act)
    {
        getChild<LLButton>(name)->setCommitCallback([this, act](LLUICtrl*, const LLSD&) { act(); refresh(); });
    };
    on_click("add_group_btn", [this]() { onAddGroup(); });
    on_click("add_person_btn", [this]() { onAddPerson(); });
    on_click("remove_target_btn", [this, selected]() { GMDXMapShare::removeTarget(selected(mTargets)); });
    on_click("always_remove_btn", [this, selected]() { GMDXMapShare::removeTarget(selected(mAlways)); });
    on_click("unblock_btn", [this, selected]() { GMDXMapShare::unblock(selected(mBlocked)); });
    on_click("unblock_all_btn", []() { GMDXMapShare::unblockAll(); });
    on_click("keep_btn", [this, selected]() { GMDXMapShare::keep(selected(mReceived)); });
    on_click("remove_item_btn", [this, selected]() { GMDXMapShare::remove(selected(mReceived)); });
    on_click("block_btn", [this, selected]()
        {
            if (const GMDXSharedItem* item = GMDXMapShare::find(selected(mReceived))) GMDXMapShare::block(item->sender);
        });
    on_click("clear_btn", []() { GMDXMapShare::clear(); });
    for (LLScrollListCtrl* list : { mTargets, mAlways, mBlocked, mReceived })
        list->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDirty = true; });   // (the buttons follow the selection)
    return true;
}

void GMDXFloaterMapSharing::onOpen(const LLSD& key)
{
    refresh();
}

// A few times a second while open: targets, people coming and going, names arriving, items' ages.
void GMDXFloaterMapSharing::draw()
{
    if (mDirty || mRefreshTimer.getElapsedTimeF32() >= 0.5f) refresh();
    LLFloater::draw();
}

void GMDXFloaterMapSharing::refresh()
{
    mDirty = false;
    mRefreshTimer.reset();

    // Send: my group (worn), then my list
    LLGroupData data;
    const LLUUID worn = gAgent.getGroupID();
    const std::string label = worn.notNull() && gAgent.getGroupData(worn, data)
        ? getString("my_group", LLStringUtil::format_map_t{ { "[NAME]", data.mName } }) : getString("no_group");
    if (mMyGroup->getLabel() != label) mMyGroup->setLabel(label);

    LLSD targets = LLSD::emptyArray(), always = LLSD::emptyArray();
    for (const Target& t : GMDXMapShare::targets())
    {
        std::string name = GMDXMapShare::targetName(t), here;
        if (t.kind == Target::GROUP)
        {
            const bool in = gAgent.isInGroup(t.id);
            name += " " + getString(in ? "group_suffix" : "group_left");
            if (in) here = getString("members_here", LLStringUtil::format_map_t{ { "[COUNT]", llformat("%d", GMDXMapShare::membersHere(t.id)) } });
        }
        else
        {
            here = getString(GMDXMapShare::isHere(t.id) ? "on_sim" : "gone");
            // everyone in my list is drawn without asking (Always adds one, not ticked)
            always.append(LLSD().with("value", t.id).with("columns", LLSD().with(0, cell("name", name))
                .with(1, cell("status", getString(t.on ? "gets_yours" : "receive_only") + (t.kept ? std::string() : getString("this_session"))))));
        }
        targets.append(LLSD().with("value", t.id).with("columns", LLSD().with(0, cell("on", t.on, "checkbox")).with(1, cell("keep", t.kept, "checkbox"))
            .with(2, cell("name", name)).with(3, cell("here", here))));
    }
    if (fill(mTargets, targets, mShown[0]))
    {
        // On / Keep, live (the list rebuilds on the next frame: not from inside its own click)
        for (LLScrollListItem* item : mTargets->getAllData())
        {
            const LLUUID id = item->getValue().asUUID();
            for (S32 col = 0; col < 2; ++col)
            {
                LLScrollListCheck* check = dynamic_cast<LLScrollListCheck*>(item->getColumn(col));
                if (!check || !check->getCheckBox()) continue;
                check->getCheckBox()->setCommitCallback([this, id, col](LLUICtrl* ctrl, const LLSD&)
                    {
                        const bool on = ctrl->getValue().asBoolean();
                        if (col == 0) GMDXMapShare::setTargetOn(id, on);
                        else GMDXMapShare::setTargetKept(id, on);
                        mDirty = true;
                    });
            }
        }
    }
    fill(mAlways, always, mShown[1]);

    // Receive: blocked, and what is shared now
    LLSD blocked = LLSD::emptyArray();
    const std::vector<LLUUID> blocked_ids = GMDXMapShare::blocked();
    for (const LLUUID& id : blocked_ids)
    {
        Target who;
        who.kind = Target::PERSON;
        who.id = id;
        blocked.append(LLSD().with("value", id).with("columns", LLSD().with(0, cell("name", GMDXMapShare::targetName(who)))));
    }
    fill(mBlocked, blocked, mShown[2]);

    LLSD received = LLSD::emptyArray();
    const F64 now = LLFrameTimer::getTotalSeconds();
    for (const GMDXSharedItem& s : GMDXMapShare::items())
    {
        std::string type = "Mark", name = s.mark.name;
        if (s.type == GMDXShareText::Type::Zone)
        {
            type = "Zone";
            name = s.zone.name;
        }
        else if (s.type == GMDXShareText::Type::Ping)
        {
            type = "Ping";
            const std::string on = GMDXMapShare::anchorName(s);
            if (!on.empty()) name += " on " + on;
        }
        received.append(LLSD().with("value", s.id).with("columns", LLSD().with(0, cell("type", type)).with(1, cell("name", name))
            .with(2, cell("sender", GMDXMapShare::senderName(s))).with(3, cell("age", age(now - s.arrived)))));
    }
    fill(mReceived, received, mShown[3]);

    const GMDXSharedItem* item = GMDXMapShare::find(mReceived->getSelectedValue().asUUID());
    getChildView("remove_target_btn")->setEnabled(mTargets->getFirstSelected() != nullptr);
    getChildView("always_remove_btn")->setEnabled(mAlways->getFirstSelected() != nullptr);
    getChildView("unblock_btn")->setEnabled(mBlocked->getFirstSelected() != nullptr);
    getChildView("unblock_all_btn")->setEnabled(!blocked_ids.empty());
    getChildView("keep_btn")->setEnabled(item != nullptr);
    getChildView("remove_item_btn")->setEnabled(item != nullptr);
    getChildView("block_btn")->setEnabled(item && item->sender != gAgentID);
    getChildView("clear_btn")->setEnabled(!GMDXMapShare::items().empty());
}

void GMDXFloaterMapSharing::showMenu(LLButton* under, const std::vector<std::pair<std::string, LLUUID>>& choices,
                                     const std::string& empty, const std::function<void(const LLUUID&)>& pick)
{
    if (LLContextMenu* old = mMenu.get())
    {
        gMenuHolder->removeChild(old);
        delete old;
    }
    LLContextMenu::Params mp;
    mp.name("gmdx_map_sharing_menu");
    mp.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(mp);
    LLHandle<LLFloater> self = getHandle();
    for (const auto& [label, id] : choices)
    {
        LLMenuItemCallGL::Params p;
        p.name("choice");
        p.label(label);
        p.enabled(id.notNull());   // (the "more not listed" line)
        p.on_click.function([self, pick, id](LLUICtrl*, const LLSD&)
            {
                pick(id);
                if (LLFloater* floater = self.get()) floater->refresh();
            });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
    }
    if (choices.empty())
    {
        LLMenuItemCallGL::Params p;
        p.name("none");
        p.label(empty);
        p.enabled(false);
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
    }
    mMenu = menu->getHandle();
    gMenuHolder->addChild(menu);
    S32 x, y;
    under->localPointToOtherView(0, 0, &x, &y, gMenuHolder);
    menu->show(x, y);   // (LLContextMenu::show alone: a showPopup after it hid it again)
}

namespace
{
// Menu labels: no "[...]" substitution tokens.
std::string menu_label(std::string s)
{
    LLStringUtil::replaceChar(s, '[', '(');
    LLStringUtil::replaceChar(s, ']', ')');
    return s;
}
} // namespace

// My groups not in the list yet, by name.
void GMDXFloaterMapSharing::onAddGroup()
{
    std::vector<std::pair<std::string, LLUUID>> choices;
    for (const LLGroupData& g : gAgent.mGroups)
        if (!listed(g.mID)) choices.emplace_back(menu_label(g.mName), g.mID);
    std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return LLStringUtil::compareInsensitive(a.first, b.first) < 0; });
    showMenu(getChild<LLButton>("add_group_btn"), choices, getString("no_groups"),
             [](const LLUUID& id) { GMDXMapShare::addTarget(Target::GROUP, id); });
}

// The people in my region not in the list yet, nearest first (as the mini-map menu's Add Person on Sim, at most 40).
void GMDXFloaterMapSharing::onAddPerson()
{
    constexpr size_t MAX_LISTED = 40;
    std::vector<std::pair<std::string, LLUUID>> choices;
    size_t more = 0;
    for (const LLUUID& id : GMDXMapShare::peopleHere())
    {
        if (listed(id)) continue;
        if (choices.size() >= MAX_LISTED) { ++more; continue; }
        Target who;
        who.kind = Target::PERSON;
        who.id = id;
        LLAvatarName name;
        const bool known = LLAvatarNameCache::get(id, &name);   // (asks for it when not known)
        choices.emplace_back(menu_label(known ? GMDXMapShare::targetName(who) : std::string("(loading name)")), id);
    }
    if (more) choices.emplace_back(getString("more_people", LLStringUtil::format_map_t{ { "[COUNT]", llformat("%d", (S32)more) } }), LLUUID::null);
    showMenu(getChild<LLButton>("add_person_btn"), choices, getString("nobody_here"),
             [](const LLUUID& id) { GMDXMapShare::addTarget(Target::PERSON, id); });
}
