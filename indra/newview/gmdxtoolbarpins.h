/**
 * @file gmdxtoolbarpins.h
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

// <GMDX> TOOLBAR PINS. An item or folder of the agent's inventory can sit on the left / right /
// bottom / top toolbar as a button of its own, next to the normal toolbar buttons:
//  - pin: drag it from the inventory onto a toolbar (LLToolBar::sGMDXPinCargo), or inventory
//    right-click "Pin to Toolbar" (end of the left toolbar); system folders too (isPinOnlyFolder);
//  - a pin is an LLCommand made at run time, named "gmdx_pin_<inventory uuid>". toolbars.xml
//    stores it like any other button (<command name="gmdx_pin_..."/>), so its toolbar and place
//    are kept per account; LLToolBarView::loadToolbars registers the saved ones before adding them;
//  - unpin: right-click the button -> "Remove this button", or drag it back to the toolbox,
//    as with every toolbar button;
//  - click: an item does what double-click does in the inventory (LLInvFVBridgeAction::doAction:
//    wear / take off, attach / detach, open, teleport...), an active gesture plays; a folder opens
//    a menu of its contents with the same actions, subfolders as submenus;
//  - the button shows the item's inventory icon and its name (text modes), the tooltip its name and
//    folder path. A pin whose item is gone (or in the Trash) is disabled with a "missing" tooltip;
//  - right-click a pinned button -> "Change Icon..." picks a library icon (Phosphor, drawn like
//    the other toolbar icons), "Reset Icon" goes back to the inventory icon; "Rename..." gives the
//    button a name of its own (its label in the text modes, the tooltip's first line), "Reset
//    Name" goes back to the item's. Both are kept per
//    account by inventory id in <account dir>/gmdx_toolbar_pins.xml, not in toolbars.xml;
//  - inventory drags can be dropped anywhere along a toolbar's button row and a button past its
//    ends, empty toolbars included, which show up while the drag is near their edge of the
//    screen (LLToolBar::getGMDXPinZone).
// Links are pinned as their target. Everything goes through the inventory's own actions, so RLVa
// restrictions apply as they do in the inventory.

#pragma once

#include "llfolderviewitem.h"   // (LLFolderViewItem::Params, EAcceptance)
#include "lluuid.h"

#include <string>

class LLFloater;
class LLSD;

namespace GMDXToolbarPins
{
    // Registers the callbacks and the toolbar drag hook (once). Called by LLToolBarView::loadToolbars.
    void init();

    // A saved toolbars.xml command: makes the pin's command when it is one of ours.
    void registerSaved(const std::string& command_name);

    // An inventory drag is on (one object from the agent's inventory that canPin takes): the toolbars
    // show where it can be pinned; an empty one shows while the cursor is near its edge of the
    // screen (LLToolBarView::draw).
    bool isPinDrag();

    // The "Change Icon..." picker (floater gmdx_pin_icons, key: the pin's inventory id).
    LLFloater* buildIconPicker(const LLSD& key);

    // True when this inventory object can be pinned (agent inventory, not in the Trash).
    bool canPin(const LLUUID& inv_id);

    // Inventory right-click "Pin to Toolbar": at the end of the left toolbar (pins already on a
    // toolbar stay where they are).
    void pin(const uuid_vec_t& inv_ids);

    // System folders (Clothing, Current Outfit, My Outfits, Received Items...) can be pinned too, but
    // the inventory never lets them be dragged. One that canPin takes gets an inventory view of its own
    // (LLInventoryPanel::createFolderViewFolder) that starts a drag of it alone, and every drag that
    // carries a system folder goes to the toolbars' pin zones and nowhere else (LLToolDragAndDrop::
    // dragOrDrop): no folder, avatar, object, the world or the toolbox can take it.
    bool isPinOnlyFolder(const LLUUID& cat_id);
    LLFolderViewFolder* createPinOnlyFolderView(LLFolderViewItem::Params& p);
    // True when the drag carries a system folder: offered only to the toolbar at (x, y) (root view
    // coordinates), *acceptance ACCEPT_NO anywhere else.
    bool pinOnlyDragOrDrop(S32 x, S32 y, MASK mask, bool drop, EAcceptance* acceptance, std::string& tooltip);
}
