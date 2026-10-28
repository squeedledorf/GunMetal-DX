/**
 * @file slwearablefavorites.h
 * @brief Favorite Wearables (#1132): the folder of links behind the Favorite Wearables window.
 *
 * Storage
 *  - A favorite is a LINK in the folder "#Wearable Favorites" inside "#Firestorm" at the top of
 *    My Inventory (FT_NONE folders). That is the folder name and layout other viewers' favorite
 *    wearables lists already use, so an existing list shows up here and the other way round.
 *    Only the folder name and layout are shared; the code is our own.
 *  - Marking or unmarking a favorite only creates or removes links in that folder. Originals
 *    are never moved, copied or deleted by anything in this module.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 * $/LicenseInfo$
 */

#ifndef SL_WEARABLEFAVORITES_H
#define SL_WEARABLEFAVORITES_H

#include <functional>
#include <string>

#include "lluuid.h"

namespace SLWearableFavorites
{
    extern const std::string PARENT_FOLDER_NAME; // "#Firestorm"
    extern const std::string FOLDER_NAME;        // "#Wearable Favorites"

    typedef std::function<void(const LLUUID&)> folder_callback_t;

    // The favorites folder, or null when it does not exist (yet). Never creates anything.
    LLUUID getFolderID();

    // Calls cb with the favorites folder, creating it (and its parent) first when missing.
    // cb gets a null id when it cannot be had (inventory not usable, create failed, timeout).
    // Concurrent calls share one creation.
    void ensureFolder(const folder_callback_t& cb);

    // True if the item (an original or a link to it) is a wearable, body part or attachment
    // that can be marked a favorite.
    bool canFavorite(const LLUUID& item_id);

    // True if a favorite link to the item's original exists.
    bool isFavorite(const LLUUID& item_id);

    // Creates favorite links (one batched request) for the items' originals. Already marked
    // and ineligible items are skipped.
    void add(const uuid_vec_t& item_ids);

    // Removes the favorite LINKS for these items. An id may be a favorite link itself or any
    // item/link whose original is a favorite. Only link items inside the favorites folder are
    // ever removed.
    void remove(const uuid_vec_t& item_ids);
}

#endif // SL_WEARABLEFAVORITES_H
