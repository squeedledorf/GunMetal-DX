/**
 * @file slprimdefaults.h
 * @brief Defaults for prims rezzed with the Build tool and for new
 *        scripts (#466), set in Preferences > Building.
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

#ifndef SL_PRIMDEFAULTS_H
#define SL_PRIMDEFAULTS_H

#include <deque>

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

class LLViewerInventoryItem;
class LLViewerObject;
class LLViewerRegion;

// Everything is off by default: with the stock settings a rez sends the
// same ObjectAdd as before and nothing is queued or changed afterwards.
//
// Size and Physical go into the ObjectAdd message itself. Phantom,
// Temporary, the texture and the embedded item need the new object, so
// LLToolPlacer queues one pending entry per create-selected ObjectAdd and
// LLViewerObjectList::processUpdateCore hands every just-created
// create-selected object we own to onCreateSelectedObject(), which
// consumes the oldest matching entry. Entries are matched by region and
// requested size and expire, so shift-drag copies and rezzes the sim
// refused do not pick up the defaults; several quick rezzes simply queue
// several entries.
class SLPrimDefaults : public LLSingleton<SLPrimDefaults>
{
    LLSINGLETON_EMPTY_CTOR(SLPrimDefaults);
    LOG_CLASS(SLPrimDefaults);

public:
    // Size to put in ObjectAdd for a new prim: stock_scale unless the
    // default size is enabled.
    static LLVector3 getNewPrimScale(const LLVector3& stock_scale);
    // True if new prims should be created physical.
    static bool getNewPrimPhysical();

    // LLToolPlacer: a create-selected ObjectAdd was sent to regionp asking
    // for a prim of this size.
    void noteRezRequest(const LLViewerRegion* regionp, const LLVector3& scale);

    // LLViewerObjectList: objectp was just created with FLAGS_CREATE_SELECTED.
    void onCreateSelectedObject(LLViewerObject* objectp);

    // The custom default new script, or nullptr to create the stock one.
    // A configured script that can't be used (gone, not copy + modify)
    // gives a notification and nullptr, so the caller falls back.
    static LLViewerInventoryItem* getDefaultScriptItem();

private:
    void applyDefaults(LLViewerObject* objectp);
    void embedItem(LLViewerObject* objectp);

    struct PendingRez
    {
        U64         mRegionHandle;
        LLVector3   mScale;
        F64         mExpires;
    };
    std::deque<PendingRez> mPending;
    LLUUID mWarnedEmbedItem; // last embed item we warned about
};

#endif // SL_PRIMDEFAULTS_H
