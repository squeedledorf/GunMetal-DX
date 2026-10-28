/**
 * @file gmdxtoolbarpins.cpp
 * @brief Inventory items and folders pinned onto the toolbars as buttons.
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

#include "llviewerprecompiledheaders.h"

#include "gmdxtoolbarpins.h"

#include "gmdxloadouts.h"

#include "llappearancemgr.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcommandmanager.h"
#include "lldir.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfolderview.h"
#include "llgesturemgr.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventorypanel.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llscrollcontainer.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsearcheditor.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lltooldraganddrop.h"
#include "lltrans.h"
#include "llviewerfoldertype.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"
#include "llwindow.h"
#include "rlvactions.h"

#include <algorithm>
#include <functional>

// llinventorygallerymenu.cpp: the inventory's "Replace current outfit" / "Add folder items"
// (folder limit, library copy).
void modify_outfit(bool append, const LLUUID& cat_id, LLInventoryModel* model);

namespace
{
    const std::string PIN_PREFIX = "gmdx_pin_";

    // Folder menus: submenus this deep, then a subfolder opens its own menu when clicked;
    // at most this many entries per menu and in all.
    constexpr S32 MENU_MAX_DEPTH = 3;
    constexpr S32 MENU_MAX_ENTRIES = 60;
    constexpr S32 MENU_BUDGET = 600;

    LLHandle<LLContextMenu> sMenu;

    //-------------------------------------------------------------------------
    // Per-pin look: a library icon and a name of the user's own, by inventory id, per account
    // (<account dir>/gmdx_toolbar_pins.xml, an LLSD map id -> {icon, label}). A pin without an
    // entry shows its item's inventory icon and name.
    //-------------------------------------------------------------------------

    // "Change Icon..." library: Phosphor fill icons (skins/default/textures/phosphor, MIT), each a
    // textures.xml entry GMDX_PinIcon_<name> drawn in IconColor at the toolbar icon size.
    const char* const kPinIcons[] =
    {
        "sword", "pistol", "rifle", "flintlock", "ammo", "knife", "axe", "hammer", "gavel", "crosshair",
        "crosshair-simple", "target", "shield", "shield-check", "shield-plus", "shield-star",
        "shield-warning", "bomb", "fire", "fire-simple", "skull", "ghost",
        "biohazard", "radioactive", "lightning", "magic-wand", "sparkle", "star-four",
        "hand-fist", "boxing-glove", "detective", "binoculars", "crown", "crown-simple",
        "trophy", "medal", "medal-military", "flag", "flag-banner", "flag-pennant",
        "flag-checkered", "castle-turret", "heart", "heart-break", "heartbeat", "first-aid",
        "first-aid-kit", "bandaids", "pill", "syringe", "drop", "drop-half",
        "flask", "atom", "person-simple-run", "person-simple-walk", "person-simple-bike", "person-simple-swim",
        "person-simple-throw", "person-arms-spread", "footprints", "airplane", "parachute", "rocket",
        "car", "motorcycle", "bicycle", "boat", "sailboat", "anchor",
        "horse", "flying-saucer", "compass", "compass-rose", "map-trifold", "map-pin",
        "globe-hemisphere-west", "signpost", "path", "mountains", "tent", "campfire",
        "house", "door", "backpack", "t-shirt", "coat-hanger", "dress",
        "sneaker", "boot", "high-heel", "pants", "hoodie", "shirt-folded",
        "baseball-cap", "goggles", "mask-happy", "gift", "diamond", "watch",
        "bag", "music-note", "music-notes", "headphones", "speaker-high", "megaphone",
        "microphone", "microphone-stage", "guitar", "piano-keys", "vinyl-record", "video-camera",
        "camera", "image", "chat", "chat-circle", "chats", "envelope",
        "phone", "star", "bookmark", "bookmark-simple", "folder", "folder-open",
        "package", "archive", "cube", "wrench", "gear", "toolbox",
        "lock", "lock-open", "key", "eye", "eye-slash", "sun",
        "moon", "moon-stars", "cloud", "snowflake", "wind", "leaf",
        "tree", "tree-evergreen", "plant", "flower", "flower-lotus", "paw-print",
        "cat", "dog", "bird", "butterfly", "fish", "rabbit",
        "bug", "smiley", "smiley-wink", "smiley-sad", "smiley-angry", "smiley-x-eyes",
        "hand", "hand-waving", "hand-heart", "hand-peace", "hands-clapping", "handshake",
        "users", "user", "users-three", "coffee", "beer-bottle", "beer-stein",
        "wine", "martini", "pizza", "hamburger", "fork-knife", "cake",
        "champagne", "confetti", "balloon", "coins", "coin", "money",
        "shopping-bag", "tag", "ticket", "scroll", "book", "book-open",
        "books", "note-pencil", "paint-brush", "palette", "hourglass", "timer",
        "alarm", "clock", "bell", "lightbulb", "flashlight", "game-controller",
        "joystick", "dice-five", "puzzle-piece", "alien", "robot", "planet",
        "infinity",
        // (A-Z, 0-9: scripts/pinicons/make_glyph_icons.py)
        "letter-a", "letter-b", "letter-c", "letter-d", "letter-e", "letter-f",
        "letter-g", "letter-h", "letter-i", "letter-j", "letter-k", "letter-l",
        "letter-m", "letter-n", "letter-o", "letter-p", "letter-q", "letter-r",
        "letter-s", "letter-t", "letter-u", "letter-v", "letter-w", "letter-x",
        "letter-y", "letter-z", "number-0", "number-1", "number-2", "number-3",
        "number-4", "number-5", "number-6", "number-7", "number-8", "number-9",
    };

    bool isLibraryIcon(const std::string& name)
    {
        return std::find(std::begin(kPinIcons), std::end(kPinIcons), name) != std::end(kPinIcons);
    }

    std::string iconTexture(const std::string& name)
    {
        return "GMDX_PinIcon_" + name;
    }

    struct PinStyle
    {
        std::string icon;   // a kPinIcons name, empty: the inventory icon
        std::string label;  // empty: the item's name
        std::string click;  // a left click: "" as the inventory's double-click (a folder: its menu),
                            // "replace" wear in place of what is worn, "add" wear on top, "ask" a menu
    };
    std::map<LLUUID, PinStyle> sStyles;

    std::string stylesFile()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "gmdx_toolbar_pins.xml");
    }

    void loadStyles()
    {
        llifstream in(stylesFile().c_str());
        LLSD sd;
        if (!in.is_open() || LLSDSerialize::fromXML(sd, in) <= 0 || !sd.isMap())
        {
            return;
        }
        for (const auto& [key, value] : llsd::inMap(sd))
        {
            LLUUID id;
            if (id.set(key, false) && id.notNull())
            {
                sStyles[id] = PinStyle{ value["icon"].asString(), value["label"].asString(), value["click"].asString() };
            }
        }
    }

    void saveStyles()
    {
        LLSD sd = LLSD::emptyMap();
        for (const auto& [id, style] : sStyles)
        {
            if (!style.icon.empty() || !style.label.empty() || !style.click.empty())
            {
                sd[id.asString()]["icon"] = style.icon;
                sd[id.asString()]["label"] = style.label;
                sd[id.asString()]["click"] = style.click;
            }
        }
        llofstream out(stylesFile().c_str());
        if (out.is_open())
        {
            LLSDSerialize::toPrettyXML(sd, out);
        }
    }

    const PinStyle& styleOf(const LLUUID& id)
    {
        static const PinStyle none;
        auto it = sStyles.find(id);
        return it != sStyles.end() ? it->second : none;
    }

    void setIcon(const LLUUID& id, const std::string& icon)
    {
        sStyles[id].icon = icon;
        saveStyles();  // the button picks it up on its next refresh (LLToolBar::COMMAND_STATE_PERIOD)
    }

    void setLabel(const LLUUID& id, const std::string& label)
    {
        sStyles[id].label = label;
        saveStyles();
    }

    void setClick(const LLUUID& id, const std::string& click)
    {
        sStyles[id].click = click;
        saveStyles();
    }

    // RLVa @showinv hides the inventory (and My Outfits): a folder's menu would show it.
    bool folderBlocked()
    {
        return RlvActions::hasBehaviour(RLV_BHVR_SHOWINV);
    }

    std::string commandName(const LLUUID& id)
    {
        return PIN_PREFIX + id.asString();
    }

    LLUUID trashID()
    {
        return gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    }

    // The object a pin of inv_id points at (links resolve to their target), or null when it
    // isn't in the agent's inventory or is in the Trash.
    LLUUID pinTarget(const LLUUID& inv_id)
    {
        const LLUUID id = gInventory.getLinkedItemID(inv_id);
        if (id.isNull() || !gInventory.getObject(id)
            || !gInventory.isObjectDescendentOf(id, gInventory.getRootFolderID())
            || gInventory.isObjectDescendentOf(id, trashID()))
        {
            return LLUUID::null;
        }
        return id;
    }

    // The inventory's own icon for the object (folder type, item type and subtype).
    std::string typeIcon(const LLUUID& id)
    {
        if (const LLViewerInventoryCategory* cat = gInventory.getCategory(id))
        {
            // the viewer's own folders: their own icons, as LLFolderBridge::getFolderIcon
            static const char* const own[] = { "GMDX_Inv_EquipmentClosed", "GMDX_Inv_LoadoutsClosed", "GMDX_Inv_OutdatedClosed" };
            if (int kind = GMDXLoadouts::folderKind(id))
            {
                return own[kind - 1];
            }
            return LLViewerFolderType::lookupIconName(cat->getPreferredType(), false);
        }
        if (const LLViewerInventoryItem* item = gInventory.getItem(id))
        {
            return LLInventoryIcon::getIconName(item->getType(), item->getInventoryType(), item->getFlags(), false);
        }
        return LLInventoryIcon::getIconName(LLAssetType::AT_UNKNOWN);
    }

    bool isSystemFolder(const LLUUID& id)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        return cat && LLFolderType::lookupIsProtectedType(cat->getPreferredType());
    }

    // The name the inventory shows: a system folder's is translated (LLFolderBridge::buildDisplayName,
    // e.g. Favorites is "Favorites Bar").
    std::string objectName(const LLInventoryObject* obj)
    {
        if (!obj)
        {
            return std::string();
        }
        std::string name = obj->getName();
        if (isSystemFolder(obj->getUUID()))
        {
            LLTrans::findString(name, "InvFolder " + obj->getName(), LLSD());
        }
        return name;
    }

    LLCommandId ensureCommand(const LLUUID& id)
    {
        const std::string name = commandName(id);
        const LLCommandId command_id(name);
        LLCommandManager& mgr = LLCommandManager::instance();
        if (!mgr.getCommand(command_id))
        {
            LLCommand::Params p;
            p.available_in_toybox = false;
            p.name = name;
            p.icon = typeIcon(id);  // kept up to date by the enabled poll (onEnabled)
            p.label_ref = "GMDXPinnedItem";
            p.tooltip_ref = "GMDXPinnedItem";
            p.execute_function = "GMDX.ToolbarPin.Open";
            p.execute_parameters = LLSD(id);
            p.is_enabled_function = "GMDX.ToolbarPin.Enabled";
            p.is_enabled_parameters = LLSD(id);
            p.is_running_function = "GMDX.ToolbarPin.Worn";
            p.is_running_parameters = LLSD(id);
            mgr.addCommand(new LLCommand(p));
        }
        return command_id;
    }

    void saveSoon()
    {
        doOnIdleOneTime([] { if (gToolBarView) gToolBarView->saveToolbars(); });
    }

    //-------------------------------------------------------------------------
    // Actions
    //-------------------------------------------------------------------------

    // What double-click does in the inventory; an active gesture plays instead of opening.
    void activateItem(const LLUUID& id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item)
        {
            return;
        }
        if (item->getType() == LLAssetType::AT_GESTURE && LLGestureMgr::instance().isGestureActive(id))
        {
            LLGestureMgr::instance().playGesture(id);
            return;
        }
        LLInvFVBridgeAction::doAction(item->getType(), id, &gInventory);
    }

    void openInInventory(const LLUUID& id)
    {
        LLInventoryPanel::openInventoryPanelAndSetSelection(true, id, true);
    }

    //-------------------------------------------------------------------------
    // Folder menu
    //-------------------------------------------------------------------------

    void showFolderMenu(const LLUUID& cat_id, LLView* anchor);
    bool wearAction(const LLUUID& id, const std::string& action);
    void doWear(const LLUUID& id, const std::string& action);
    void showWearMenu(const LLUUID& id, LLView* anchor);

    void addCall(LLContextMenu* menu, const std::string& label, std::function<void()> on_click, bool enabled = true)
    {
        LLMenuItemCallGL::Params p;
        p.name("gmdx_pin_entry");
        p.label(label);
        p.enabled(enabled);
        if (on_click)
        {
            p.on_click.function([on_click](LLUICtrl*, const LLSD&) { on_click(); });
        }
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
    }

    void addItem(LLContextMenu* menu, const LLViewerInventoryItem* item)
    {
        const LLUUID id = item->getUUID();
        LLMenuItemCheckGL::Params p;
        p.name("gmdx_pin_item");
        p.label(item->getName());
        p.on_click.function([id](LLUICtrl*, const LLSD&) { activateItem(id); });
        p.on_check.function([id](LLUICtrl*, const LLSD&) { return get_is_item_worn(id); });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCheckGL>(p));
    }

    struct MenuBuilder
    {
        LLHandle<LLView> anchor;
        S32 budget = MENU_BUDGET;

        void fill(LLContextMenu* menu, const LLUUID& cat_id, S32 depth)
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
            if (!cat)
            {
                return;
            }
            addCall(menu, std::string("Open in Inventory"), [cat_id] { openInInventory(cat_id); });

            const LLFolderType::EType type = cat->getPreferredType();
            if (depth == 0)
            {
                // A system folder (Clothing, Current Outfit, My Outfits...): only taking its items off,
                // as the inventory offers (LLFolderBridge::buildContextMenuFolderOptions); never from
                // the whole inventory (a scan of everything, and it would take off all).
                const bool system = LLFolderType::lookupIsProtectedType(type);
                LLAppearanceMgr& app = LLAppearanceMgr::instance();
                const bool can_add = !system && type != LLFolderType::FT_OUTFIT && app.getCanAddToCOF(cat_id);
                const bool can_remove = type != LLFolderType::FT_ROOT_INVENTORY && LLAppearanceMgr::getCanRemoveFromCOF(cat_id);
                const bool can_replace = !system && app.getCanReplaceCOF(cat_id);
                if (can_add || can_remove || can_replace)
                {
                    menu->addSeparator();
                    if (!system)
                    {
                        addCall(menu, std::string("Replace current outfit"),
                                [cat_id] { modify_outfit(false, cat_id, &gInventory); }, can_replace);
                    }
                    if (!system && type != LLFolderType::FT_OUTFIT)
                    {
                        addCall(menu, std::string("Add folder items"),
                                [cat_id] { modify_outfit(true, cat_id, &gInventory); }, can_add);
                    }
                    addCall(menu, std::string("Take off folder items"),
                            [cat_id] { LLAppearanceMgr::instance().takeOffOutfit(cat_id); }, can_remove);
                }
            }
            menu->addSeparator();

            if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                cat->fetch();
                addCall(menu, std::string("Loading..."), nullptr, false);
                return;
            }

            // Subfolders first, then items, each by name. Links show as what they point at.
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(cat_id, cats, items);
            std::vector<LLViewerInventoryCategory*> folders;
            std::vector<LLViewerInventoryItem*> things;
            if (cats)
            {
                for (const auto& c : *cats) folders.push_back(c.get());
            }
            if (items)
            {
                for (const auto& i : *items)
                {
                    if (LLViewerInventoryCategory* linked_cat = i->getLinkedCategory())
                    {
                        folders.push_back(linked_cat);
                    }
                    else if (LLViewerInventoryItem* linked = i->getLinkedItem())
                    {
                        things.push_back(linked);
                    }
                    else if (!i->getIsLinkType())
                    {
                        things.push_back(i.get());
                    }
                }
            }
            auto by_name = [](const LLInventoryObject* a, const LLInventoryObject* b)
            {
                return LLStringUtil::compareDict(a->getName(), b->getName()) < 0;
            };
            std::sort(folders.begin(), folders.end(), by_name);
            std::sort(things.begin(), things.end(), by_name);

            if (folders.empty() && things.empty())
            {
                addCall(menu, std::string("(empty)"), nullptr, false);
                return;
            }

            S32 shown = 0;
            const S32 total = S32(folders.size() + things.size());
            for (LLViewerInventoryCategory* sub : folders)
            {
                if (shown >= MENU_MAX_ENTRIES || budget <= 0) break;
                ++shown;
                --budget;
                const LLUUID sub_id = sub->getUUID();
                if (depth + 1 < MENU_MAX_DEPTH && budget > 0)
                {
                    LLContextMenu::Params mp;
                    mp.name("gmdx_pin_folder");
                    mp.label(objectName(sub));
                    mp.visible(false);
                    LLContextMenu* submenu = LLUICtrlFactory::create<LLContextMenu>(mp);
                    fill(submenu, sub_id, depth + 1);
                    menu->appendContextSubMenu(submenu);
                }
                else
                {
                    // Too deep for submenus: this folder's own menu, opened after this one closes.
                    LLHandle<LLView> at = anchor;
                    addCall(menu, objectName(sub) + " ...", [sub_id, at]
                    {
                        doOnIdleOneTime([sub_id, at] { if (LLView* v = at.get()) showFolderMenu(sub_id, v); });
                    });
                }
            }
            for (const LLViewerInventoryItem* item : things)
            {
                if (shown >= MENU_MAX_ENTRIES || budget <= 0) break;
                ++shown;
                --budget;
                addItem(menu, item);
            }
            if (shown < total)
            {
                addCall(menu, llformat("%d more: Open in Inventory", total - shown), [cat_id] { openInInventory(cat_id); });
            }
        }
    };

    void showFolderMenu(const LLUUID& cat_id, LLView* anchor)
    {
        if (LLContextMenu* old = sMenu.get())
        {
            gMenuHolder->removeChild(old);
            delete old;
        }
        LLContextMenu::Params mp;
        mp.name("gmdx_pin_menu");
        mp.visible(false);
        LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(mp);
        MenuBuilder builder;
        builder.anchor = anchor->getHandle();
        builder.fill(menu, cat_id, 0);
        sMenu = menu->getHandle();
        gMenuHolder->addChild(menu);

        // Beside the button, away from the screen edge its toolbar sits on (LLContextMenu::show
        // flips it left / up where it would leave the screen).
        const LLToolBar* toolbar = anchor->getParentByType<LLToolBar>();
        const LLToolBarEnums::SideType side = toolbar ? toolbar->getSideType() : LLToolBarEnums::SIDE_LEFT;
        const LLRect rect = anchor->getLocalRect();
        S32 x = 0, y = 0;
        anchor->localPointToOtherView(side == LLToolBarEnums::SIDE_LEFT ? rect.mRight : rect.mLeft,
                                      side == LLToolBarEnums::SIDE_TOP ? rect.mBottom : rect.mTop,
                                      &x, &y, gMenuHolder);
        menu->show(x, y);
    }

    //-------------------------------------------------------------------------
    // Command callbacks (commands.xml style: the parameter is the inventory id)
    //-------------------------------------------------------------------------

    void onOpen(LLUICtrl* ctrl, const LLSD& param)
    {
        const LLUUID id = param.asUUID();
        if (pinTarget(id).isNull())
        {
            return;
        }
        // the pin's own left click (right-click menu: Left Click), when it can do that now
        const std::string& click = styleOf(id).click;
        if ((click == "replace" || click == "add") && wearAction(id, click == "replace" ? "wear" : "add"))
        {
            doWear(id, click == "replace" ? "wear" : "add");
            return;
        }
        if (click == "ask" && ctrl && !gInventory.getCategory(id))
        {
            showWearMenu(id, ctrl);
            return;
        }
        if (gInventory.getCategory(id))
        {
            if (ctrl && !folderBlocked())
            {
                showFolderMenu(id, ctrl);
            }
        }
        else
        {
            activateItem(id);
        }
    }

    // Polled by LLToolBar::refreshCommandState (ctrl is the toolbar, see below): enabled while the item is
    // there, and keeps the button's icon, name and tooltip in step with the inventory.
    bool onEnabled(LLUICtrl* ctrl, const LLSD& param)
    {
        const LLUUID id = param.asUUID();
        bool found = pinTarget(id).notNull();

        // A library icon / own name chosen for the pin win over the item's (styleOf).
        const PinStyle& style = styleOf(id);
        std::string icon = isLibraryIcon(style.icon) ? iconTexture(style.icon) : typeIcon(id);
        std::string item_name, name, tooltip;
        item_name = objectName(gInventory.getObject(id));
        name = style.label.empty() ? item_name : style.label;

        if (found)
        {
            std::string path;
            const LLInventoryObject* obj = gInventory.getObject(id);
            for (LLUUID parent = obj->getParentUUID(); parent.notNull();)
            {
                const LLViewerInventoryCategory* folder = gInventory.getCategory(parent);
                if (!folder) break;
                path = path.empty() ? objectName(folder) : objectName(folder) + " > " + path;
                parent = folder->getParentUUID();
            }
            // first line the button's name; then the item (when renamed) and where it is
            std::string where = path.empty() ? std::string() : "in " + path;
            if (name != item_name)
            {
                where = where.empty() ? item_name : item_name + " " + where;
            }
            tooltip = where.empty() ? name : name + "\n" + where;
            if (gInventory.getCategory(id) && folderBlocked())
            {
                tooltip += "\nUnavailable while your inventory is hidden (RLV)";
                found = false;
            }
        }
        else if (!item_name.empty())
        {
            tooltip = name + "\n" + "In the Trash: restore it to use this button.";
        }
        else if (!LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
        {
            tooltip = name.empty() ? std::string("Inventory still loading...") : name + "\nInventory still loading...";
        }
        else
        {
            if (name.empty())
            {
                name = "Missing item";
            }
            tooltip = name + "\n" + "No longer in your inventory. Right-click > Remove this button to unpin.";
        }

        // LLToolBar::createButton binds the enable callback with initEnableCallback(), which fixes
        // the control argument to the toolbar itself (the button refreshCommandState passes is
        // dropped). Take either, so this keeps working if that binding ever changes.
        LLToolBarButton* button = dynamic_cast<LLToolBarButton*>(ctrl);
        LLToolBar* toolbar = button ? button->getParentByType<LLToolBar>() : dynamic_cast<LLToolBar*>(ctrl);
        if (toolbar && !button)
        {
            button = toolbar->findChild<LLToolBarButton>(commandName(id));
        }
        if (toolbar && button)
        {
            if (button->LLView::getToolTip() != tooltip)
            {
                button->setToolTip(tooltip);
            }
            if (!icon.empty() && button->getImageOverlay() != LLUI::getUIImage(icon))
            {
                button->setImageOverlay(icon, button->getImageOverlayHAlign());  // colours: setEnabled() right after
            }
            const LLToolBarEnums::ButtonType mode = toolbar->getButtonType();
            const bool shows_text = mode == LLToolBarEnums::BTNTYPE_ICONS_WITH_TEXT || mode == LLToolBarEnums::BTNTYPE_TEXT_ONLY;
            if (shows_text && !name.empty() && button->getCurrentLabel().getString() != name)
            {
                button->setLabel(name);
                toolbar->reshape(toolbar->getRect().getWidth(), toolbar->getRect().getHeight());  // lay out again for the new width
            }
        }
        return found;
    }

    bool onWorn(LLUICtrl*, const LLSD& param)
    {
        return get_is_item_worn(param.asUUID());
    }

    //-------------------------------------------------------------------------
    // The toolbar's right-click menu on a pinned button (menu_toolbars.xml gmdx_pin_*)
    //-------------------------------------------------------------------------

    // The pin of the button the toolbar menu was opened on, null when it isn't a pin.
    LLUUID menuPin()
    {
        LLUUID id;
        const LLView* button = LLToolBar::sGMDXMenuButton.get();
        if (button && button->getName().compare(0, PIN_PREFIX.size(), PIN_PREFIX) == 0)
        {
            id.set(button->getName().substr(PIN_PREFIX.size()), false);
        }
        return id;
    }

    void onMenu(LLUICtrl*, const LLSD& param)
    {
        const LLUUID id = menuPin();
        if (id.isNull())
        {
            return;
        }
        const std::string action = param.asString();
        if (action == "icon")
        {
            LLFloaterReg::showInstance("gmdx_pin_icons", LLSD(id), true);
        }
        else if (action == "reset_icon")
        {
            setIcon(id, std::string());
        }
        else if (action == "rename")
        {
            std::string current = styleOf(id).label;
            if (current.empty())
            {
                const LLInventoryObject* obj = gInventory.getObject(id);
                current = objectName(obj);
            }
            LLSD args;
            args["NAME"] = current;
            LLNotificationsUtil::add("GMDXPinRename", args, LLSD(), [id](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                {
                    return;
                }
                std::string label = response["message"].asString();
                LLStringUtil::trim(label);
                const LLInventoryObject* obj = gInventory.getObject(id);
                // the item's own name (or nothing) means no name of its own
                setLabel(id, (obj && label == objectName(obj)) ? std::string() : label);
            });
        }
        else if (action == "reset_name")
        {
            setLabel(id, std::string());
        }
        else if (action == "wear" || action == "add" || action == "take_off")
        {
            doWear(id, action);
        }
        else if (action.compare(0, 6, "click_") == 0)
        {
            const std::string click = action.substr(6);
            setClick(id, click == "default" ? std::string() : click);
        }
    }

    // As the inventory's own Wear / Add / Take Off (RLVa checks happen in there).
    void doWear(const LLUUID& id, const std::string& action)
    {
        LLAppearanceMgr& app = LLAppearanceMgr::instance();
        if (gInventory.getCategory(id))
        {
            if (action == "take_off") app.takeOffOutfit(id);
            else modify_outfit(action == "add", id, &gInventory);
        }
        else if (action == "take_off")
        {
            app.removeItemFromAvatar(id);
        }
        else
        {
            app.wearItemOnAvatar(id, true, action == "wear");
        }
    }

    // A left click set to Ask: Wear (Replace) / Wear (Add) / Take Off, beside the button.
    void showWearMenu(const LLUUID& id, LLView* anchor)
    {
        if (LLContextMenu* old = sMenu.get())
        {
            gMenuHolder->removeChild(old);
            delete old;
        }
        LLContextMenu::Params mp;
        mp.name("gmdx_pin_menu");
        mp.visible(false);
        LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(mp);
        bool any = false;
        for (const auto& [action, label] : { std::pair<const char*, const char*>{ "wear", "Wear (Replace)" },
                                             { "add", "Wear (Add)" }, { "take_off", "Take Off" } })
        {
            if (!wearAction(id, action)) continue;
            const std::string a = action;
            addCall(menu, label, [id, a] { doWear(id, a); });
            any = true;
        }
        if (!any)
        {
            delete menu;
            activateItem(id);   // (not wearable: as an ordinary click)
            return;
        }
        sMenu = menu->getHandle();
        gMenuHolder->addChild(menu);
        S32 x = 0, y = 0;
        const LLRect rect = anchor->getLocalRect();
        anchor->localPointToOtherView(rect.mRight, rect.mTop, &x, &y, gMenuHolder);
        menu->show(x, y);
    }

    // What a pin may do with the avatar: wear in place of / add to what is worn, or take off.
    bool wearAction(const LLUUID& id, const std::string& action)
    {
        LLAppearanceMgr& app = LLAppearanceMgr::instance();
        if (const LLViewerInventoryCategory* cat = gInventory.getCategory(id))
        {
            const LLFolderType::EType type = cat->getPreferredType();
            const bool system = LLFolderType::lookupIsProtectedType(type);
            if (action == "wear") return !system && app.getCanReplaceCOF(id);
            if (action == "add") return !system && type != LLFolderType::FT_OUTFIT && LLAppearanceMgr::getCanAddToCOF(id);
            return type != LLFolderType::FT_ROOT_INVENTORY && LLAppearanceMgr::getCanRemoveFromCOF(id);
        }
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item) return false;
        const LLAssetType::EType type = item->getType();
        const bool wearable = type == LLAssetType::AT_CLOTHING || type == LLAssetType::AT_BODYPART || type == LLAssetType::AT_OBJECT;
        if (!wearable) return false;
        const bool worn = get_is_item_worn(id);
        if (action == "wear") return !worn;
        if (action == "add") return !worn && type != LLAssetType::AT_BODYPART;   // (a body part only replaces)
        return worn && type != LLAssetType::AT_BODYPART;
    }

    // Left Click submenu: the pin's current choice
    bool onMenuCheck(LLUICtrl*, const LLSD& param)
    {
        const LLUUID id = menuPin();
        if (id.isNull()) return false;
        const std::string want = param.asString().substr(6);
        const std::string& click = styleOf(id).click;
        return click == (want == "default" ? std::string() : want);
    }

    // Pin entries show for a pinned button only; "Reset ..." only once there is something to reset.
    bool onMenuVisible(LLUICtrl*, const LLSD& param)
    {
        const LLUUID id = menuPin();
        if (id.isNull())
        {
            return false;
        }
        const std::string action = param.asString();
        if (action == "reset_icon")
        {
            return !styleOf(id).icon.empty();
        }
        if (action == "reset_name")
        {
            return !styleOf(id).label.empty();
        }
        if (action == "wear" || action == "add" || action == "take_off")
        {
            return wearAction(id, action);
        }
        if (action.compare(0, 6, "click_") == 0)
        {
            // only for what can be worn (a folder has no "ask": its own menu already offers both)
            if (action == "click_ask" && gInventory.getCategory(id)) return false;
            return wearAction(id, "wear") || wearAction(id, "add") || wearAction(id, "take_off");
        }
        return true;
    }

    //-------------------------------------------------------------------------
    // "Change Icon..." picker: the library as a searchable grid of buttons
    //-------------------------------------------------------------------------

    class GMDXFloaterPinIcons : public LLFloater
    {
    public:
        GMDXFloaterPinIcons(const LLSD& key) : LLFloater(key) {}

        bool postBuild() override
        {
            mFilter = getChild<LLSearchEditor>("filter");
            mFilter->setKeystrokeCallback([this](LLUICtrl*, const LLSD&) { layoutGrid(); mScroll->goToTop(); });
            mScroll = getChild<LLScrollContainer>("scroll");
            mGrid = getChild<LLPanel>("grid");
            for (const char* icon : kPinIcons)
            {
                const std::string name = icon;
                std::string words = name;
                LLStringUtil::replaceChar(words, '-', ' ');
                if (name == "pistol" || name == "rifle" || name == "flintlock") words += " gun";   // (found by "gun" too)
                if (name == "ammo") words += " bullet cartridge";
                LLButton::Params bp(LLUICtrlFactory::getDefaultParams<LLButton>());
                bp.name(name);
                bp.label(LLStringUtil::null);
                bp.tool_tip(words);
                bp.image_overlay(LLUI::getUIImage(iconTexture(name)));
                bp.rect(LLRect(0, BUTTON_SIZE, BUTTON_SIZE, 0));
                bp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
                bp.click_callback.function([this, name](LLUICtrl*, const LLSD&) { pick(name); });
                LLButton* button = LLUICtrlFactory::create<LLButton>(bp);
                mGrid->addChild(button);
                mButtons.push_back(button);
            }
            return true;
        }

        void onOpen(const LLSD& key) override
        {
            mPin = key.asUUID();
            layoutGrid();
            mFilter->setFocus(true);
        }

        void reshape(S32 width, S32 height, bool called_from_parent = true) override
        {
            LLFloater::reshape(width, height, called_from_parent);
            if (mGrid)
            {
                layoutGrid();
            }
        }

    private:
        static constexpr S32 BUTTON_SIZE = 36;
        static constexpr S32 CELL = BUTTON_SIZE + 4;

        // The buttons matching the search (spaces match the names' dashes), row by row; the
        // pin's current icon pressed.
        void layoutGrid()
        {
            std::string filter = mFilter->getText();
            LLStringUtil::toLower(filter);
            LLStringUtil::trim(filter);
            LLStringUtil::replaceChar(filter, ' ', '-');
            const std::string& current = styleOf(mPin).icon;

            const S32 columns = llmax(1, mScroll->getContentWindowRect().getWidth() / CELL);
            S32 shown = 0;
            for (LLButton* button : mButtons)
            {
                shown += (filter.empty() || button->getName().find(filter) != std::string::npos) ? 1 : 0;
            }
            const S32 height = llmax(CELL, ((shown + columns - 1) / columns) * CELL);
            mGrid->reshape(columns * CELL, height);

            S32 index = 0;
            for (LLButton* button : mButtons)
            {
                const bool show = filter.empty() || button->getName().find(filter) != std::string::npos;
                button->setVisible(show);
                if (!show)
                {
                    continue;
                }
                button->setToggleState(button->getName() == current);
                button->setOrigin((index % columns) * CELL + 2, height - (index / columns + 1) * CELL + 2);
                ++index;
            }
        }

        void pick(const std::string& name)
        {
            if (mPin.notNull())
            {
                setIcon(mPin, name);
            }
            closeFloater();
        }

        LLUUID mPin;
        LLSearchEditor* mFilter = nullptr;
        LLScrollContainer* mScroll = nullptr;
        LLPanel* mGrid = nullptr;
        std::vector<LLButton*> mButtons;
    };

    // LLToolBar::sGMDXPinCargo: an inventory item or folder dragged from the agent's inventory.
    LLCommandId pinCargo(EDragAndDropType type, void* data, bool drop)
    {
        if (!data || !gToolBarView
            || LLToolDragAndDrop::getInstance()->getSource() != LLToolDragAndDrop::SOURCE_AGENT)
        {
            return LLCommandId::null;
        }
        switch (type)
        {
            case DAD_TEXTURE:  case DAD_SOUND:    case DAD_CALLINGCARD: case DAD_LANDMARK:
            case DAD_SCRIPT:   case DAD_CLOTHING: case DAD_OBJECT:      case DAD_NOTECARD:
            case DAD_CATEGORY: case DAD_BODYPART: case DAD_ANIMATION:   case DAD_GESTURE:
            case DAD_LINK:     case DAD_MESH:     case DAD_SETTINGS:    case DAD_MATERIAL:
                break;
            default:
                return LLCommandId::null;  // not an inventory object (people, toolbar buttons...)
        }
        const LLUUID id = pinTarget(static_cast<const LLInventoryObject*>(data)->getUUID());
        if (id.isNull())
        {
            return LLCommandId::null;
        }
        if (!drop)
        {
            return LLCommandId(commandName(id));
        }
        // The toolbar adds it at the drop point: off the toolbar it is on first (a move), saved after.
        const LLCommandId command_id = ensureCommand(id);
        int old_rank = LLToolBar::RANK_NONE;
        gToolBarView->removeCommand(command_id, old_rank);
        saveSoon();
        return command_id;
    }

    // A pinnable system folder's inventory view. The inventory doesn't let a system folder be dragged
    // (LLFolderViewItem::handleHover: isMovable); this one starts the drag of it, alone, as the item
    // view would, and LLToolDragAndDrop sends it to the toolbars only (pinOnlyDragOrDrop).
    class PinOnlyFolderView : public LLFolderViewFolder
    {
    protected:
        PinOnlyFolderView(const LLFolderViewItem::Params& p) : LLFolderViewFolder(p) {}
        friend class ::LLUICtrlFactory;   // (unqualified, it would name one in this namespace)

    public:
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            LLFolderView* root = getRoot();
            const auto* model = static_cast<const LLFolderViewModelItemInventory*>(getViewModelItem());
            if (hasMouseCapture() && root && model && !isMovable()
                && (x - mDragStartX) * (x - mDragStartX) + (y - mDragStartY) * (y - mDragStartY)
                       > DRAG_N_DROP_DISTANCE_THRESHOLD * DRAG_N_DROP_DISTANCE_THRESHOLD
                && root->getAllowDrag() && root->getCurSelectedItem() == this
                && root->getSelectionList().size() == 1 && GMDXToolbarPins::isPinOnlyFolder(model->getUUID())
                && root->startDrag())
            {
                // (as LLFolderViewItem::handleHover)
                root->autoOpenTest(nullptr);
                root->setShowSelectionContext(true);
                gFocusMgr.setKeyboardFocus(nullptr);
                getWindow()->setCursor(UI_CURSOR_ARROW);
                root->clearHoveredItem();
                return true;
            }
            return LLFolderViewFolder::handleHover(x, y, mask);
        }
    };
}

namespace GMDXToolbarPins
{
    void init()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.Open", onOpen);
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.Enabled", onEnabled);
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.Worn", onWorn);
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.Menu", onMenu);
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.MenuVisible", onMenuVisible);
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("GMDX.ToolbarPin.MenuCheck", onMenuCheck);
        loadStyles();
        LLToolBar::sGMDXPinCargo = pinCargo;
    }

    void registerSaved(const std::string& command_name)
    {
        if (command_name.compare(0, PIN_PREFIX.size(), PIN_PREFIX) != 0)
        {
            return;
        }
        LLUUID id;
        if (id.set(command_name.substr(PIN_PREFIX.size()), false) && id.notNull())
        {
            ensureCommand(id);  // kept even when the item is gone: the button shows it as missing
        }
    }

    LLFloater* buildIconPicker(const LLSD& key)
    {
        return new GMDXFloaterPinIcons(key);
    }

    bool isPinDrag()
    {
        LLToolDragAndDrop* dnd = LLToolDragAndDrop::getInstance();
        if (!dnd->hasMouseCapture() || dnd->getSource() != LLToolDragAndDrop::SOURCE_AGENT || dnd->getCargoCount() != 1)
        {
            return false;
        }
        const uuid_vec_t& ids = dnd->getCargoIDs();
        return !ids.empty() && canPin(ids.front());   // (not the Trash's, nor anything else pinCargo turns down)
    }

    bool canPin(const LLUUID& inv_id)
    {
        return pinTarget(inv_id).notNull();
    }

    bool isPinOnlyFolder(const LLUUID& cat_id)
    {
        return isSystemFolder(cat_id) && canPin(cat_id);
    }

    LLFolderViewFolder* createPinOnlyFolderView(LLFolderViewItem::Params& p)
    {
        return LLUICtrlFactory::create<PinOnlyFolderView>(p);
    }

    bool pinOnlyDragOrDrop(S32 x, S32 y, MASK mask, bool drop, EAcceptance* acceptance, std::string& tooltip)
    {
        // (LL never drags a system folder: LLFolderViewItem::isMovable, LLInventoryGallery::startDrag)
        LLToolDragAndDrop* dnd = LLToolDragAndDrop::getInstance();
        const uuid_vec_t& ids = dnd->getCargoIDs();
        if (dnd->getSource() != LLToolDragAndDrop::SOURCE_AGENT || std::none_of(ids.begin(), ids.end(), isSystemFolder))
        {
            return false;
        }
        *acceptance = ACCEPT_NO;
        tooltip = "Drop it on a toolbar to pin it there.";
        LLViewerInventoryCategory* cat = ids.size() == 1 ? gInventory.getCategory(ids.front()) : nullptr;
        if (!cat || !gToolBarView || !canPin(cat->getUUID()))
        {
            return true;
        }
        for (S32 loc = LLToolBarEnums::TOOLBAR_FIRST; loc <= LLToolBarEnums::TOOLBAR_LAST; ++loc)
        {
            LLToolBar* toolbar = gToolBarView->getToolbar((LLToolBarEnums::EToolBarLocation)loc);
            if (!toolbar || !toolbar->isInVisibleChain())
            {
                continue;
            }
            S32 local_x = 0, local_y = 0;
            toolbar->screenPointToLocal(x, y, &local_x, &local_y);
            if (toolbar->getGMDXPinZone().pointInRect(local_x, local_y))
            {
                // (LLToolBar::handleDragAndDrop: sGMDXPinCargo; the caret, or the pin on a drop)
                EAcceptance accept = ACCEPT_NO;
                if (toolbar->handleDragAndDrop(local_x, local_y, mask, drop, DAD_CATEGORY, cat, &accept, tooltip))
                {
                    *acceptance = accept;
                    tooltip.clear();
                }
                break;
            }
        }
        return true;
    }

    void pin(const uuid_vec_t& inv_ids)
    {
        if (!gToolBarView)
        {
            return;
        }
        init();
        bool changed = false;
        for (const LLUUID& inv_id : inv_ids)
        {
            const LLUUID id = pinTarget(inv_id);
            if (id.isNull())
            {
                continue;
            }
            const LLCommandId command_id = ensureCommand(id);
            if (gToolBarView->hasCommand(command_id) == LLToolBarEnums::TOOLBAR_NONE)
            {
                gToolBarView->addCommand(command_id, LLToolBarEnums::TOOLBAR_LEFT);
                changed = true;
            }
        }
        if (changed)
        {
            gToolBarView->saveToolbars();
        }
    }
}
