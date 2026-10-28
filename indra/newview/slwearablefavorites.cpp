/**
 * @file slwearablefavorites.cpp
 * @brief Favorite Wearables (#1132): the folder of links behind the Favorite Wearables window.
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

#include "llviewerprecompiledheaders.h"

#include "slwearablefavorites.h"

#include <algorithm>
#include <set>
#include <vector>

#include "llcallbacklist.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lltimer.h"
#include "llviewerinventory.h"

const std::string SLWearableFavorites::PARENT_FOLDER_NAME = "#Firestorm";
const std::string SLWearableFavorites::FOLDER_NAME = "#Wearable Favorites";

namespace
{
    constexpr F32 FETCH_POLL_SECS = 0.5f;
    constexpr F64 FETCH_TIMEOUT_SECS = 30.0;

    LLUUID find_child_folder(const LLUUID& parent_id, const std::string& name)
    {
        if (parent_id.isNull())
        {
            return LLUUID::null;
        }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent_id, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (cat->getName() == name)
                {
                    return cat->getUUID();
                }
            }
        }
        return LLUUID::null;
    }

    bool folder_fetched(const LLUUID& cat_id)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
        return cat
            && cat->getVersion() != LLViewerInventoryCategory::VERSION_UNKNOWN
            && cat->getDescendentCount() != LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN
            && cat->getDescendentCount() == cat->getViewerDescendentCount();
    }

    // The original an item stands for (the item itself when it is not a link); null if unknown.
    const LLViewerInventoryItem* original_of(const LLUUID& item_id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (!item)
        {
            return nullptr;
        }
        return gInventory.getItem(item->getLinkedUUID());
    }

    // Every favorite link (an item link inside the favorites folder or one of its subfolders).
    void collect_favorite_links(const LLUUID& folder_id, LLInventoryModel::item_array_t& links)
    {
        if (folder_id.isNull())
        {
            return;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(folder_id, cats, items, false);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item && item->getActualType() == LLAssetType::AT_LINK)
            {
                links.push_back(item);
            }
        }
    }

    // ensureFolder() bookkeeping: one creation in flight, every caller answered together.
    std::vector<SLWearableFavorites::folder_callback_t> sWaiters;
    bool sBusy = false;

    void finish(const LLUUID& folder_id)
    {
        std::vector<SLWearableFavorites::folder_callback_t> waiters;
        waiters.swap(sWaiters);
        sBusy = false;
        for (const SLWearableFavorites::folder_callback_t& cb : waiters)
        {
            if (cb)
            {
                cb(folder_id);
            }
        }
    }

    void create_favorites_folder(const LLUUID& parent_id)
    {
        LLInvCreateLog::Site inv_site("Favorite Wearables folder"); // #461
        gInventory.createNewCategory(parent_id, LLFolderType::FT_NONE, SLWearableFavorites::FOLDER_NAME,
            [](const LLUUID& new_id)
            {
                finish(new_id);
            });
    }
}

LLUUID SLWearableFavorites::getFolderID()
{
    const LLUUID parent_id = find_child_folder(gInventory.getRootFolderID(), PARENT_FOLDER_NAME);
    return find_child_folder(parent_id, FOLDER_NAME);
}

void SLWearableFavorites::ensureFolder(const folder_callback_t& cb)
{
    const LLUUID root_id = gInventory.getRootFolderID();
    if (!gInventory.isInventoryUsable() || root_id.isNull())
    {
        if (cb)
        {
            cb(LLUUID::null);
        }
        return;
    }
    const LLUUID existing = getFolderID();
    if (existing.notNull())
    {
        if (cb)
        {
            cb(existing);
        }
        return;
    }

    sWaiters.push_back(cb);
    if (sBusy)
    {
        return;
    }
    sBusy = true;

    const LLUUID parent_id = find_child_folder(root_id, PARENT_FOLDER_NAME);
    if (parent_id.isNull())
    {
        LLInvCreateLog::Site inv_site("Favorite Wearables parent folder"); // #461
        gInventory.createNewCategory(root_id, LLFolderType::FT_NONE, PARENT_FOLDER_NAME,
            [](const LLUUID& new_parent_id)
            {
                if (new_parent_id.isNull())
                {
                    finish(LLUUID::null);
                    return;
                }
                create_favorites_folder(new_parent_id);
            });
        return;
    }

    if (folder_fetched(parent_id))
    {
        create_favorites_folder(parent_id);
        return;
    }

    // The parent exists but its children are not known yet: the favorites folder may well be
    // among them. Creating before they arrive could make a second one, so wait (bounded).
    LLInventoryModelBackgroundFetch::instance().start(parent_id, true);
    const F64 started = LLTimer::getElapsedSeconds();
    doPeriodically([parent_id, started]() -> bool
        {
            const LLUUID found = getFolderID();
            if (found.notNull())
            {
                finish(found);
                return true;
            }
            if (folder_fetched(parent_id))
            {
                create_favorites_folder(parent_id);
                return true;
            }
            if (LLTimer::getElapsedSeconds() - started > FETCH_TIMEOUT_SECS)
            {
                LL_WARNS("Inventory") << "Favorite Wearables: gave up waiting for " << PARENT_FOLDER_NAME
                                      << " to load; not creating the folder" << LL_ENDL;
                finish(LLUUID::null);
                return true;
            }
            return false;
        }, FETCH_POLL_SECS);
}

bool SLWearableFavorites::canFavorite(const LLUUID& item_id)
{
    const LLViewerInventoryItem* original = original_of(item_id);
    if (!original)
    {
        return false;
    }
    switch (original->getActualType())
    {
        case LLAssetType::AT_CLOTHING:
        case LLAssetType::AT_BODYPART:
        case LLAssetType::AT_OBJECT:
            break;
        default:
            return false;
    }
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (gInventory.isObjectDescendentOf(original->getUUID(), trash_id)
        || gInventory.isObjectDescendentOf(original->getUUID(), gInventory.getLibraryRootFolderID()))
    {
        return false;
    }
    // Only items of My Inventory (not the library, not another avatar's or an object's contents).
    return gInventory.isObjectDescendentOf(original->getUUID(), gInventory.getRootFolderID());
}

bool SLWearableFavorites::isFavorite(const LLUUID& item_id)
{
    const LLViewerInventoryItem* original = original_of(item_id);
    if (!original)
    {
        return false;
    }
    LLInventoryModel::item_array_t links;
    collect_favorite_links(getFolderID(), links);
    for (const LLPointer<LLViewerInventoryItem>& link : links)
    {
        if (link->getLinkedUUID() == original->getUUID())
        {
            return true;
        }
    }
    return false;
}

void SLWearableFavorites::add(const uuid_vec_t& item_ids)
{
    // Resolve to originals, drop duplicates and ineligible items now, while the selection is current.
    std::vector<LLConstPointer<LLInventoryObject>> originals;
    std::set<LLUUID> seen;
    for (const LLUUID& id : item_ids)
    {
        const LLViewerInventoryItem* original = original_of(id);
        if (!original || !canFavorite(id) || !seen.insert(original->getUUID()).second)
        {
            continue;
        }
        originals.push_back(LLConstPointer<LLInventoryObject>(original));
    }
    if (originals.empty())
    {
        return;
    }

    ensureFolder([originals](const LLUUID& folder_id)
        {
            if (folder_id.isNull())
            {
                LL_WARNS("Inventory") << "Favorite Wearables: no folder available, nothing added" << LL_ENDL;
                return;
            }
            // Skip what became a favorite meanwhile (the list is checked again at send time).
            LLInventoryModel::item_array_t links;
            collect_favorite_links(folder_id, links);
            std::set<LLUUID> already;
            for (const LLPointer<LLViewerInventoryItem>& link : links)
            {
                already.insert(link->getLinkedUUID());
            }
            LLInventoryObject::const_object_list_t to_link;
            for (const LLConstPointer<LLInventoryObject>& original : originals)
            {
                if (original.notNull() && gInventory.getItem(original->getUUID()) && !already.count(original->getUUID()))
                {
                    to_link.push_back(original);
                }
            }
            if (to_link.empty())
            {
                return;
            }
            LLInvCreateLog::Site inv_site("Favorite Wearables add"); // #461
            link_inventory_array_verified(folder_id, to_link, LLPointer<LLInventoryCallback>());
        });
}

void SLWearableFavorites::remove(const uuid_vec_t& item_ids)
{
    const LLUUID folder_id = getFolderID();
    if (folder_id.isNull())
    {
        return;
    }
    LLInventoryModel::item_array_t links;
    collect_favorite_links(folder_id, links);

    std::set<LLUUID> link_ids;   // favorite links to delete (never anything else)
    for (const LLUUID& id : item_ids)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item)
        {
            continue;
        }
        for (const LLPointer<LLViewerInventoryItem>& link : links)
        {
            if (link->getUUID() == id || link->getLinkedUUID() == item->getLinkedUUID())
            {
                link_ids.insert(link->getUUID());
            }
        }
    }
    for (const LLUUID& link_id : link_ids)
    {
        remove_inventory_item(link_id, LLPointer<LLInventoryCallback>());
    }
}
