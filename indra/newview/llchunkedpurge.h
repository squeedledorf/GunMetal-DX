/**
 * @file llchunkedpurge.h
 * @brief Deletes a folder tree in parts when the server refuses to delete it
 *        in one request ("Too many subcategories in request").
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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

#ifndef LL_LLCHUNKEDPURGE_H
#define LL_LLCHUNKEDPURGE_H

#include "lluuid.h"
#include "llpointer.h"

class LLInventoryCallback;

// Empty Trash / Purge on a folder with tens of thousands of subfolders is
// refused by AIS with 403 "Too many subcategories in request" and nothing is
// deleted. LLChunkedPurge then deletes the same tree in smaller pieces,
// automatically (the user already confirmed the delete).
//
// Inventory-safety invariants (do not relax):
//  - only descendants of the confirmed root are ever targeted, re-checked right
//    before each request (isObjectDescendentOf + root not moved); never a
//    protected (system) folder, never anything with a pending #538 copy from it;
//  - the root itself is removed only in REMOVE_FOLDER mode, i.e. when the
//    refused request the user confirmed was the removal of that very folder;
//  - one request at a time, the next one only after the previous reply;
//  - a refused request is never resent: the refused subtree is split into
//    smaller (different) requests; any other failure stops everything;
//  - the local model changes only through the server's replies (AISUpdate).
class LLChunkedPurge
{
public:
    enum EMode
    {
        PURGE_CHILDREN, // the refused request was PurgeDescendents(root): root stays
        REMOVE_FOLDER   // the refused request was RemoveCategory(root): root goes too
    };

    // Called when the user-confirmed request on root_id was refused for having
    // too many subcategories. done_cb (may be null) fires once with a null id
    // when the job ends, successfully or not (as the AIS callback would have).
    static void start(const LLUUID& root_id, EMode mode, LLPointer<LLInventoryCallback> done_cb);

    // True if id is the root of a queued/running job, inside one, or contains one.
    // Deleting it now would race the job, so callers refuse with a notice.
    static bool overlapsActiveJob(const LLUUID& id);

    // Disconnect/teardown: logs what was left undeleted and drops all jobs.
    static void shutdown();
};

#endif // LL_LLCHUNKEDPURGE_H
