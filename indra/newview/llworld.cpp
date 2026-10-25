/**
 * @file llworld.cpp
 * @brief Initial test structure to organize viewer regions
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llcrossingtrace.h"

#include "llworld.h"
#include "llrender.h"

#include "indra_constants.h"
#include "llstl.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercontrol.h"
#include "lldrawpool.h"
#include "llwin32headers.h"
#include "llhttpnode.h"
#include "llregionhandle.h"
#include "llsky.h"
#include "llsurface.h"
#include "lltrans.h"
#include "llviewercamera.h"
#include "llcullcamera.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llvlcomposition.h"
#include "llvoavatar.h"
#include "llvocache.h"
#include "llvowater.h"
#include "message.h"
#include "pipeline.h"
#include "llappviewer.h"        // for do_disconnect()
#include "llcleaningest.h"
#include "llstartup.h"
#include "llframetimer.h"
#include "llvkhitch.h"
#include <boost/unordered/unordered_flat_set.hpp>
#include <algorithm>
#include <deque>
#include <queue>
#include <map>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>


//
// Globals
//
U32         gAgentPauseSerialNum = 0;

//
// Constants
//
const S32 WORLD_PATCH_SIZE = 16;

extern LLColor4U MAX_WATER_COLOR;

const U32 LLWorld::mWidth = 256;

// meters/point, therefore mWidth * mScale = meters per edge
const F32 LLWorld::mScale = 1.f;

const F32 LLWorld::mWidthInMeters = mWidth * mScale;

//
// Functions
//

// allocate the stack
LLWorld::LLWorld() :
    mLandFarClip(DEFAULT_FAR_PLANE),
    mLastPacketsIn(0),
    mLastPacketsOut(0),
    mLastPacketsLost(0),
    mSpaceTimeUSec(0)
{
    for (S32 i = 0; i < EDGE_WATER_OBJECTS_COUNT; i++)
    {
        mEdgeWaterObjects[i] = NULL;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw(1,1,4);
    U8 *default_texture = raw->getData();
    *(default_texture++) = MAX_WATER_COLOR.mV[0];
    *(default_texture++) = MAX_WATER_COLOR.mV[1];
    *(default_texture++) = MAX_WATER_COLOR.mV[2];
    *(default_texture++) = MAX_WATER_COLOR.mV[3];

    mDefaultWaterTexturep = LLViewerTextureManager::getLocalTexture(raw.get(), false);
    gGL.getTexUnit(0)->bind(mDefaultWaterTexturep);
    mDefaultWaterTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);

    LLViewerRegion::sVOCacheCullingEnabled = gSavedSettings.getBOOL("RequestFullRegionCache") && gSavedSettings.getBOOL("ObjectCacheEnabled");
}


void LLWorld::resetClass()
{
    mHoleWaterObjects.clear();
    mHoleWaterKeyValid = false;
    gObjectList.destroy();
    gSky.cleanup(); // references an object
    for(region_list_t::iterator region_it = mRegionList.begin(); region_it != mRegionList.end(); )
    {
        LLViewerRegion* region_to_delete = *region_it++;
        removeRegion(region_to_delete->getHost());
    }
    flushRetiringRegions();   // #568: nothing outlives the reset

    LLViewerPartSim::getInstance()->destroyClass();

    mDefaultWaterTexturep = NULL ;
    for (S32 i = 0; i < EDGE_WATER_OBJECTS_COUNT; i++)
    {
        mEdgeWaterObjects[i] = NULL;
    }

    //make all visible drawbles invisible.
    LLDrawable::incrementVisible();
}


LLViewerRegion* LLWorld::addRegion(const U64 &region_handle, const LLHost &host)
{
    LL_INFOS() << "Add region with handle: " << region_handle << " on host " << host << LL_ENDL;
    // #568: a region still retiring on this host / at this handle finishes its teardown
    // first (its objects must leave the local-ID table before the new region's arrive).
    flushRetiringRegions(&host, region_handle);
    LLViewerRegion *regionp = getRegionFromHandle(region_handle);
    std::string seedUrl;
    if (regionp)
    {
        LLHost old_host = regionp->getHost();
        // region already exists!
        if (host == old_host && regionp->isAlive())
        {
            // This is a duplicate for the same host and it's alive, don't bother.
            LL_INFOS() << "Region already exists and is alive, using existing region" << LL_ENDL;
            return regionp;
        }

        if (host != old_host)
        {
            LL_WARNS() << "LLWorld::addRegion exists, but old host " << old_host
                    << " does not match new host " << host
                    << ", removing old region and creating new" << LL_ENDL;
        }
        if (!regionp->isAlive())
        {
            LL_WARNS() << "LLWorld::addRegion exists, but isn't alive. Removing old region and creating new" << LL_ENDL;
        }

        // Save capabilities seed URL
        seedUrl = regionp->getCapability("Seed");

        // Kill the old host, and then we can continue on and add the new host.  We have to kill even if the host
        // matches, because all the agent state for the new camera is completely different.
        removeRegion(old_host);
        // #568: the old region was just retired; finish it before its replacement exists
        // (same handle, maybe the same host: its objects must leave the local-ID table).
        flushRetiringRegions(&old_host, region_handle);
    }
    else
    {
        LL_INFOS() << "Region does not exist, creating new one" << LL_ENDL;
    }

    U32 iindex = 0;
    U32 jindex = 0;
    from_region_handle(region_handle, &iindex, &jindex);
    S32 x = (S32)(iindex/mWidth);
    S32 y = (S32)(jindex/mWidth);
    LL_INFOS() << "Adding new region (" << x << ":" << y << ")"
        << " on host: " << host << LL_ENDL;

    LLVector3d origin_global;

    origin_global = from_region_handle(region_handle);

    const double create_t0 = LLCrossingTrace::nowMs();   // #568: construction cost (land, composition, overlay)
    regionp = new LLViewerRegion(region_handle,
                                    host,
                                    mWidth,
                                    WORLD_PATCH_SIZE,
                                    getRegionWidthInMeters() );
    if (!regionp)
    {
        LL_ERRS() << "Unable to create new region!" << LL_ENDL;
    }
    {
        char b[64];
        std::snprintf(b, sizeof(b), " construct=%.2fms", LLCrossingTrace::nowMs() - create_t0);
        LLCrossingTrace::regionEvent("create " + LLCrossingTrace::regionLabel(regionp) + b);   // #568
    }

    if ( !seedUrl.empty() )
    {
        regionp->setCapability("Seed", seedUrl);
    }

    mRegionList.push_back(regionp);
    mRegionSet.insert(regionp);
    mRegionByHandle.emplace(regionp->getHandle(), regionp);
    mRegionByHost.emplace(regionp->getHost(), regionp);
    mActiveRegionList.push_back(regionp);
    mCulledRegionList.push_back(regionp);


    // Find all the adjacent regions, and attach them.
    // Generate handles for all of the adjacent regions, and attach them in the correct way.
    // connect the edges
    F32 adj_x = 0.f;
    F32 adj_y = 0.f;
    F32 region_x = 0.f;
    F32 region_y = 0.f;
    U64 adj_handle = 0;

    F32 width = getRegionWidthInMeters();

    LLViewerRegion *neighborp;
    from_region_handle(region_handle, &region_x, &region_y);

    // Iterate through all directions, and connect neighbors if there.
    S32 dir;
    for (dir = 0; dir < 8; dir++)
    {
        adj_x = region_x + width * gDirAxes[dir][0];
        adj_y = region_y + width * gDirAxes[dir][1];
        to_region_handle(adj_x, adj_y, &adj_handle);

        neighborp = getRegionFromHandle(adj_handle);
        if (neighborp)
        {
            //LL_INFOS() << "Connecting " << region_x << ":" << region_y << " -> " << adj_x << ":" << adj_y << LL_ENDL;
            regionp->connectNeighbor(neighborp, dir);
        }
    }

    updateWaterObjects();

    return regionp;
}


void LLWorld::removeRegion(const LLHost &host)
{
    F32 x, y;

    LLViewerRegion *regionp = getRegion(host);
    if (!regionp)
    {
        LL_WARNS() << "Trying to remove region that doesn't exist!" << LL_ENDL;
        return;
    }

    if (regionp == gAgent.getRegion())
    {
        for (region_list_t::iterator iter = mRegionList.begin();
             iter != mRegionList.end(); ++iter)
        {
            LLViewerRegion* reg = *iter;
            LL_WARNS() << "RegionDump: " << reg->getName()
                << " " << reg->getHost()
                << " " << reg->getOriginGlobal()
                << LL_ENDL;
        }

        LL_WARNS() << "Agent position global " << gAgent.getPositionGlobal()
            << " agent " << gAgent.getPositionAgent()
            << LL_ENDL;

        LL_WARNS() << "Regions visited " << gAgent.getRegionsVisited() << LL_ENDL;

        LL_WARNS() << "gFrameTimeSeconds " << gFrameTimeSeconds << LL_ENDL;

        LL_WARNS() << "Disabling region " << regionp->getName() << " that agent is in!" << LL_ENDL;
        LLAppViewer::instance()->forceDisconnect(LLTrans::getString("YouHaveBeenDisconnected"));

        regionp->saveObjectCache() ; //force to save objects here in case that the object cache is about to be destroyed.
        return;
    }

    from_region_handle(regionp->getHandle(), &x, &y);
    LL_INFOS() << "Removing region " << x << ":" << y << LL_ENDL;
    LLCrossingTrace::RegionTimed crossing_evt("removeRegion(delist; + destroy unless retired, see \"retired\")", regionp);   // #568

    mRegionList.remove(regionp);
    mRegionSet.erase(regionp);
    {
        auto hit = mRegionByHandle.find(regionp->getHandle());
        if (hit != mRegionByHandle.end() && hit->second == regionp)
        {
            mRegionByHandle.erase(hit);
            for (LLViewerRegion* other : mRegionList)
            {
                if (other->getHandle() == regionp->getHandle()) { mRegionByHandle.emplace(other->getHandle(), other); break; }
            }
        }
        auto oit = mRegionByHost.find(regionp->getHost());
        if (oit != mRegionByHost.end() && oit->second == regionp)
        {
            mRegionByHost.erase(oit);
            for (LLViewerRegion* other : mRegionList)
            {
                if (other->getHost() == regionp->getHost()) { mRegionByHost.emplace(other->getHost(), other); break; }
            }
        }
    }
    mActiveRegionList.remove(regionp);
    mCulledRegionList.remove(regionp);
    mVisibleRegionList.remove(regionp);

    // Delisted regions dispatch no more sim messages: stop the event poll here, not at the
    // final delete (with the incremental teardown below that is seconds later -- a mass ring
    // drop retires one region per frame -- and the poll kept posting to gMainloopWork).
    regionp->stopEventPoll();

    mRegionRemovedSignal(regionp);
    LLCleanIngest::onRegionRemoved(regionp->getHandle());

    updateWaterObjects();

    //double check all objects of this region are removed.
    gObjectList.clearAllMapObjectsInRegion(regionp) ;
    //llassert_always(!gObjectList.hasMapObjectInRegion(regionp)) ;

    // #568 INCREMENTAL TEARDOWN: the region is delisted (nothing finds it any more); its
    // objects die a time slice per frame, then its cache is saved, then it is deleted
    // (updateRetiringRegions). The LLViewerRegion stays allocated until its last object
    // is dead, so no object outlives its region pointer. RegionTeardownBudgetMs 0 = the
    // old synchronous teardown (kill every object, save, delete -- here, now).
    static LLCachedControl<F32> s_budget(gSavedSettings, "RegionTeardownBudgetMs", 2.f);
    if ((F32)s_budget > 0.f && !LLApp::isExiting())
    {
        regionp->detachFromNeighbors();
        RetiringRegion r;
        r.mRegion = regionp;
        r.mStartSecs = LLFrameTimer::getElapsedSeconds().value();
        const double t0 = LLCrossingTrace::nowMs();
        gObjectList.collectRegionObjects(regionp, r.mObjects);
        r.mWorkMs = r.mWorstMs = LLCrossingTrace::nowMs() - t0;
        crossing_evt.note("retire_collect", r.mWorkMs);
        crossing_evt.noteCount("retiring_objects", (U32)r.mObjects.size());
        mRetiringRegions.push_back(std::move(r));
        return;
    }

    delete regionp; // Delete last to prevent use after free
}

bool LLWorld::isRetiringRegion(const LLViewerRegion* regionp) const
{
    for (const RetiringRegion& r : mRetiringRegions)
    {
        if (r.mRegion == regionp) return true;
    }
    return false;
}

void LLWorld::retireStep(RetiringRegion& r, F64 budget_s)
{
    const double t0 = LLCrossingTrace::nowMs();
    ++r.mFrames;
    if (r.mStage == 0)
    {
        // Kill its objects, a slice at a time (at least one per step).
        if (gObjectList.killRegionObjectsSliced(r.mRegion, r.mObjects, r.mCursor, budget_s, r.mKilled))
        {
            r.mObjects.clear();
            r.mObjects.shrink_to_fit();
            r.mStage = 1;
        }
    }
    else if (r.mStage == 1)
    {
        // Serialize + queue the cache write (the file is written on the VOCache I/O thread).
        r.mRegion->saveObjectCacheBeforeDestroy();
        r.mStage = 2;
    }
    else if (r.mStage == 2)
    {
        // The rest of ~LLViewerRegion: land / composition / overlay, and a last walk that
        // kills anything still pointing at it (normally nothing).
        const std::string label = LLCrossingTrace::regionLabel(r.mRegion);
        delete r.mRegion;
        r.mRegion = nullptr;
        r.mStage = 3;
        const double ms = LLCrossingTrace::nowMs() - t0;
        r.mWorkMs += ms;
        r.mWorstMs = llmax(r.mWorstMs, (F64)ms);
        char b[256];
        std::snprintf(b, sizeof(b), "retired %s: %u objects killed over %u frames (%.0f ms wall), main-thread %.1f ms total, worst step %.1f ms",
                      label.c_str(), r.mKilled, r.mFrames,
                      (LLFrameTimer::getElapsedSeconds().value() - r.mStartSecs) * 1000.0, r.mWorkMs, r.mWorstMs);
        LLCrossingTrace::regionEvent(b);
        LL_INFOS("RegionTeardown") << b << LL_ENDL;
        return;
    }
    const double ms = LLCrossingTrace::nowMs() - t0;
    r.mWorkMs += ms;
    r.mWorstMs = llmax(r.mWorstMs, (F64)ms);
}

void LLWorld::updateRetiringRegions()
{
    if (mRetiringRegions.empty()) return;
    LL_PROFILE_ZONE_SCOPED;
    static LLCachedControl<F32> s_budget(gSavedSettings, "RegionTeardownBudgetMs", 2.f);
    const F64 budget_s = llmax((F32)s_budget, 0.01f) * 0.001;
    // One step of the oldest retiring region per frame.
    RetiringRegion& r = mRetiringRegions.front();
    retireStep(r, budget_s);
    if (r.mStage == 3) mRetiringRegions.pop_front();
}

void LLWorld::flushRetiringRegions(const LLHost* host, U64 handle)
{
    for (auto it = mRetiringRegions.begin(); it != mRetiringRegions.end(); )
    {
        LLViewerRegion* regionp = it->mRegion;
        const bool match = !host || (regionp && (regionp->getHost() == *host || (handle && regionp->getHandle() == handle)));
        if (!match)
        {
            ++it;
            continue;
        }
        while (it->mStage != 3)
        {
            retireStep(*it, 1.0e9);   // no budget: finish now
        }
        it = mRetiringRegions.erase(it);
    }
}


LLViewerRegion* LLWorld::getRegion(const LLHost &host)
{
    auto it = mRegionByHost.find(host);
    return it != mRegionByHost.end() ? it->second : nullptr;
}

LLViewerRegion* LLWorld::getRegionFromPosAgent(const LLVector3 &pos)
{
    return getRegionFromPosGlobal(gAgent.getPosGlobalFromAgent(pos));
}

LLViewerRegion* LLWorld::getRegionFromPosGlobal(const LLVector3d &pos)
{
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if (regionp->pointInRegionGlobal(pos))
        {
            return regionp;
        }
    }
    return NULL;
}


LLVector3d  LLWorld::clipToVisibleRegions(const LLVector3d &start_pos, const LLVector3d &end_pos)
{
    if (positionRegionValidGlobal(end_pos))
    {
        return end_pos;
    }

    LLViewerRegion* regionp = getRegionFromPosGlobal(start_pos);
    if (!regionp)
    {
        return start_pos;
    }

    LLVector3d delta_pos = end_pos - start_pos;
    LLVector3d delta_pos_abs;
    delta_pos_abs.setVec(delta_pos);
    delta_pos_abs.abs();

    LLVector3 region_coord = regionp->getPosRegionFromGlobal(end_pos);
    F64 clip_factor = 1.0;
    F32 region_width = regionp->getWidth();
    if (region_coord.mV[VX] < 0.f)
    {
        if (region_coord.mV[VY] < region_coord.mV[VX])
        {
            // clip along y -
            clip_factor = -(region_coord.mV[VY] / delta_pos_abs.mdV[VY]);
        }
        else
        {
            // clip along x -
            clip_factor = -(region_coord.mV[VX] / delta_pos_abs.mdV[VX]);
        }
    }
    else if (region_coord.mV[VX] > region_width)
    {
        if (region_coord.mV[VY] > region_coord.mV[VX])
        {
            // clip along y +
            clip_factor = (region_coord.mV[VY] - region_width) / delta_pos_abs.mdV[VY];
        }
        else
        {
            //clip along x +
            clip_factor = (region_coord.mV[VX] - region_width) / delta_pos_abs.mdV[VX];
        }
    }
    else if (region_coord.mV[VY] < 0.f)
    {
        // clip along y -
        clip_factor = -(region_coord.mV[VY] / delta_pos_abs.mdV[VY]);
    }
    else if (region_coord.mV[VY] > region_width)
    {
        // clip along y +
        clip_factor = (region_coord.mV[VY] - region_width) / delta_pos_abs.mdV[VY];
    }

    // clamp to within region dimensions
    LLVector3d final_region_pos = LLVector3d(region_coord) - (delta_pos * clip_factor);
    final_region_pos.mdV[VX] = llclamp(final_region_pos.mdV[VX], 0.0,
                                       (F64)(region_width - F_ALMOST_ZERO));
    final_region_pos.mdV[VY] = llclamp(final_region_pos.mdV[VY], 0.0,
                                       (F64)(region_width - F_ALMOST_ZERO));
    final_region_pos.mdV[VZ] = llclamp(final_region_pos.mdV[VZ], 0.0,
                                       (F64)(LLWorld::getInstance()->getRegionMaxHeight() - F_ALMOST_ZERO));
    return regionp->getPosGlobalFromRegion(LLVector3(final_region_pos));
}

LLViewerRegion* LLWorld::getRegionFromHandle(const U64 &handle)
{
    auto it = mRegionByHandle.find(handle);
    return it != mRegionByHandle.end() ? it->second : nullptr;
}

LLViewerRegion* LLWorld::getRegionFromID(const LLUUID& region_id)
{
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if (regionp->getRegionID() == region_id)
        {
            return regionp;
        }
    }
    return NULL;
}

void LLWorld::updateAgentOffset(const LLVector3d &offset_global)
{
#if 0
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        regionp->setAgentOffset(offset_global);
    }
#endif
}


bool LLWorld::positionRegionValidGlobal(const LLVector3d &pos_global)
{
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if (regionp->pointInRegionGlobal(pos_global))
        {
            return true;
        }
    }
    return false;
}


// Allow objects to go up to their radius underground.
F32 LLWorld::getMinAllowedZ(LLViewerObject* object, const LLVector3d &global_pos)
{
    F32 land_height = resolveLandHeightGlobal(global_pos);
    F32 radius = 0.5f * object->getScale().length();
    return land_height - radius;
}



LLViewerRegion* LLWorld::resolveRegionGlobal(LLVector3 &pos_region, const LLVector3d &pos_global)
{
    LLViewerRegion *regionp = getRegionFromPosGlobal(pos_global);

    if (regionp)
    {
        pos_region = regionp->getPosRegionFromGlobal(pos_global);
        return regionp;
    }

    return NULL;
}


LLViewerRegion* LLWorld::resolveRegionAgent(LLVector3 &pos_region, const LLVector3 &pos_agent)
{
    LLVector3d pos_global = gAgent.getPosGlobalFromAgent(pos_agent);
    LLViewerRegion *regionp = getRegionFromPosGlobal(pos_global);

    if (regionp)
    {
        pos_region = regionp->getPosRegionFromGlobal(pos_global);
        return regionp;
    }

    return NULL;
}


F32 LLWorld::resolveLandHeightAgent(const LLVector3 &pos_agent)
{
    LLVector3d pos_global = gAgent.getPosGlobalFromAgent(pos_agent);
    return resolveLandHeightGlobal(pos_global);
}


F32 LLWorld::resolveLandHeightGlobal(const LLVector3d &pos_global)
{
    LLViewerRegion *regionp = getRegionFromPosGlobal(pos_global);
    if (regionp)
    {
        return regionp->getLand().resolveHeightGlobal(pos_global);
    }
    return 0.0f;
}


// Takes a line defined by "point_a" and "point_b" and determines the closest (to point_a)
// point where the the line intersects an object or the land surface.  Stores the results
// in "intersection" and "intersection_normal" and returns a scalar value that represents
// the normalized distance along the line from "point_a" to "intersection".
//
// Currently assumes point_a and point_b only differ in z-direction,
// but it may eventually become more general.
F32 LLWorld::resolveStepHeightGlobal(const LLVOAvatar* avatarp, const LLVector3d &point_a, const LLVector3d &point_b,
                               LLVector3d &intersection, LLVector3 &intersection_normal,
                               LLViewerObject **viewerObjectPtr)
{
    // initialize return value to null
    if (viewerObjectPtr)
    {
        *viewerObjectPtr = NULL;
    }

    LLViewerRegion *regionp = getRegionFromPosGlobal(point_a);
    if (!regionp)
    {
        // We're outside the world
        intersection = 0.5f * (point_a + point_b);
        intersection_normal.setVec(0.0f, 0.0f, 1.0f);
        return 0.5f;
    }

    // calculate the length of the segment
    F32 segment_length = (F32)((point_a - point_b).length());
    if (0.0f == segment_length)
    {
        intersection = point_a;
        intersection_normal.setVec(0.0f, 0.0f, 1.0f);
        return segment_length;
    }

    // get land height
    // Note: we assume that the line is parallel to z-axis here
    LLVector3d land_intersection = point_a;
    F32 normalized_land_distance;

    land_intersection.mdV[VZ] = regionp->getLand().resolveHeightGlobal(point_a);
    normalized_land_distance = (F32)(point_a.mdV[VZ] - land_intersection.mdV[VZ]) / segment_length;
    intersection = land_intersection;
    intersection_normal = resolveLandNormalGlobal(land_intersection);

    if (avatarp && !avatarp->mFootPlane.isExactlyClear())
    {
        LLVector3 foot_plane_normal(avatarp->mFootPlane.mV);
        LLVector3 start_pt = avatarp->getRegion()->getPosRegionFromGlobal(point_a);
        // added 0.05 meters to compensate for error in foot plane reported by Havok
        F32 norm_dist_from_plane = ((start_pt * foot_plane_normal) - avatarp->mFootPlane.mV[VW]) + 0.05f;
        norm_dist_from_plane = llclamp(norm_dist_from_plane / segment_length, 0.f, 1.f);
        if (norm_dist_from_plane < normalized_land_distance)
        {
            // collided with object before land
            normalized_land_distance = norm_dist_from_plane;
            intersection = point_a;
            intersection.mdV[VZ] -= norm_dist_from_plane * segment_length;
            intersection_normal = foot_plane_normal;
        }
        else
        {
            intersection = land_intersection;
            intersection_normal = resolveLandNormalGlobal(land_intersection);
        }
    }

    return normalized_land_distance;
}


LLSurfacePatch * LLWorld::resolveLandPatchGlobal(const LLVector3d &pos_global)
{
    //  returns a pointer to the patch at this location
    LLViewerRegion *regionp = getRegionFromPosGlobal(pos_global);
    if (!regionp)
    {
        return NULL;
    }

    return regionp->getLand().resolvePatchGlobal(pos_global);
}


LLVector3 LLWorld::resolveLandNormalGlobal(const LLVector3d &pos_global)
{
    LLViewerRegion *regionp = getRegionFromPosGlobal(pos_global);
    if (!regionp)
    {
        return LLVector3::z_axis;
    }

    return regionp->getLand().resolveNormalGlobal(pos_global);
}


// Region visibility box. LL took it from the terrain spatial partition's root group,
// but the Vulkan renderer owns terrain and never builds LL's terrain drawables, so
// that group's bounds are stale/degenerate and whole regions dropped out of
// mVisibleRegionList depending on camera heading (property lines vanished over half
// of the view directions). Use the region's own footprint + the land's z range.
static bool region_in_frustum(LLViewerRegion* regionp)
{
    const LLSurface& land = regionp->getLand();
    const LLVector3 origin = regionp->getOriginAgent();
    const F32 width = regionp->getWidth();
    F32 min_z = land.getMinZ(), max_z = land.getMaxZ();
    if (!(max_z >= min_z)) { min_z = -100.f; max_z = 4096.f; }   // no z data yet
    LLVector4a lo, hi;
    lo.set(origin.mV[VX], origin.mV[VY], origin.mV[VZ] + min_z - 1.f);
    hi.set(origin.mV[VX] + width, origin.mV[VY] + width, origin.mV[VZ] + max_z + 1.f);
    LLVector4a center, extent;
    center.setAdd(lo, hi);   center.mul(0.5f);
    extent.setSub(hi, lo);   extent.mul(0.5f);
    return LLViewerCamera::getInstance()->AABBInFrustum(center, extent);
}

void LLWorld::updateVisibilities()
{
    F32 cur_far_clip = LLViewerCamera::getInstance()->getFar();

    // Go through the culled list and check for visible regions (region is visible if land is visible)
    for (region_list_t::iterator iter = mCulledRegionList.begin();
         iter != mCulledRegionList.end(); )
    {
        region_list_t::iterator curiter = iter++;
        LLViewerRegion* regionp = *curiter;

        if (region_in_frustum(regionp))
        {
            mCulledRegionList.erase(curiter);
            mVisibleRegionList.push_back(regionp);
        }
    }

    // Update all of the visible regions
    for (region_list_t::iterator iter = mVisibleRegionList.begin();
         iter != mVisibleRegionList.end(); )
    {
        region_list_t::iterator curiter = iter++;
        LLViewerRegion* regionp = *curiter;
        if (!regionp->getLand().hasZData())
        {
            continue;
        }

        if (region_in_frustum(regionp))
        {
            regionp->calculateCameraDistance();
            regionp->getLand().updatePatchVisibilities(gAgent);
        }
        else
        {
            mVisibleRegionList.erase(curiter);
            mCulledRegionList.push_back(regionp);
        }
    }

    // Sort visible regions
    mVisibleRegionList.sort(LLViewerRegion::CompareDistance());

    LLViewerCamera::getInstance()->setFar(cur_far_clip);
}

static LLTrace::SampleStatHandle<> sNumActiveCachedObjects("numactivecachedobjects", "Number of objects loaded from cache");

void LLWorld::updateRegions(F32 max_update_time)
{
    LL_PROFILE_ZONE_SCOPED;
    LLTimer update_timer;

    // Region ring cap (#dd4k): drop / late-connect before iterating the region list.
    {
        VK_HITCH_SCOPE("rgn_ring");   // #485
        updateRegionRing();
    }
    mNumOfActiveCachedObjects = 0;

    // The object cache's static-camera throttle follows the culling camera (#479: the
    // live world camera, or the frozen debug one).
    if(LLCullCamera::isChanged())
    {
        LLViewerRegion::sLastCameraUpdated = LLSpatialEntryData::getCurrentFrame() + 1;
    }
    LLViewerRegion::calcNewObjectCreationThrottle();
    // #485: one wall-clock budget for the whole call (SLRegionUpdateBudgetMs; LL's 1 s
    // only behind the progress screen). LL loosened it to 1 s whenever the creation
    // throttle was off (2 s after every teleport, or always with
    // NewObjectCreationThrottle -1): 250 ms for the agent's region plus 250 ms per
    // further region while time remained -- the 230-450 ms LL_updateRegions frames.
    max_update_time = LLViewerRegion::regionUpdateBudget(max_update_time);

    // The agent's region gets up to half the budget, every other region a quarter of it.
    F32 max_time = llmin((F32)(max_update_time - update_timer.getElapsedTimeF32()), max_update_time * 0.5f);
    //update the self avatar region
    LLViewerRegion* self_regionp = gAgent.getRegion();
    if(self_regionp)
    {
        self_regionp->idleUpdate(max_time);
    }

    //sort regions by its mLastUpdate
    //smaller mLastUpdate first to make sure every region has chance to get updated.
    LLViewerRegion::region_priority_list_t region_list;
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if(regionp != self_regionp)
        {
            region_list.insert(regionp);
        }
        mNumOfActiveCachedObjects += regionp->getNumOfActiveCachedObjects();
    }

    // Perform idle time updates for the regions (and associated surfaces)
    for (LLViewerRegion::region_priority_list_t::iterator iter = region_list.begin();
         iter != region_list.end(); ++iter)
    {
        if(max_time > 0.f)
        {
            max_time = llmin((F32)(max_update_time - update_timer.getElapsedTimeF32()), max_update_time * 0.25f);
        }

        if(max_time > 0.f)
        {
            (*iter)->idleUpdate(max_time);
        }
        else
        {
            //perform some necessary but very light updates.
            (*iter)->lightIdleUpdate();
        }
    }

    if(max_time > 0.f)
    {
        max_time = llmin((F32)(max_update_time - update_timer.getElapsedTimeF32()), max_update_time * 0.25f);
    }
    if(max_time > 0.f)
    {
        VK_HITCH_SCOPE("rgn_cleanup");   // #485
        LLViewerRegion::idleCleanup(max_time);
    }

    // #533 linkset hand-off check (each region runs it every 30 s; returns at once otherwise).
    for (LLViewerRegion* regionp : mRegionList)
    {
        regionp->checkLinksetHandoff();
    }

    sample(sNumActiveCachedObjects, mNumOfActiveCachedObjects);
}

void LLWorld::clearAllVisibleObjects()
{
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        //clear all cached visible objects.
        (*iter)->clearCachedVisibleObjects();
    }
    clearHoleWaterObjects();
    clearEdgeWaterObjects();
}

void LLWorld::updateParticles()
{
    LLViewerPartSim::getInstance()->updateSimulation();
}

void LLWorld::renderPropertyLines()
{
    LL_PROFILE_ZONE_SCOPED;
    for (region_list_t::iterator iter = mVisibleRegionList.begin();
         iter != mVisibleRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        regionp->renderPropertyLines();
    }
}


void LLWorld::updateNetStats()
{
    F64Bits bits;

    for (region_list_t::iterator iter = mActiveRegionList.begin();
         iter != mActiveRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        regionp->updateNetStats();
        bits += regionp->mBitsReceived;
        regionp->mBitsReceived = (F32Bits)0.f;
        regionp->mPacketsReceived = 0.f;
    }

    S32 packets_in = gMessageSystem->mPacketsIn - mLastPacketsIn;
    S32 packets_out = gMessageSystem->mPacketsOut - mLastPacketsOut;
    S32 packets_lost = gMessageSystem->mDroppedPackets - mLastPacketsLost;

    F64Bits actual_in_bits(gMessageSystem->mPacketRing.getAndResetActualInBits());
    F64Bits actual_out_bits(gMessageSystem->mPacketRing.getAndResetActualOutBits());

    add(LLStatViewer::MESSAGE_SYSTEM_DATA_IN, actual_in_bits);
    add(LLStatViewer::MESSAGE_SYSTEM_DATA_OUT, actual_out_bits);
    add(LLStatViewer::ACTIVE_MESSAGE_DATA_RECEIVED, bits);
    add(LLStatViewer::PACKETS_IN, packets_in);
    add(LLStatViewer::PACKETS_OUT, packets_out);
    add(LLStatViewer::PACKETS_LOST, packets_lost);

    // Running totals (LLViewerStats mirrors the adds): reading the viewer recording's
    // sums here re-activated it every frame.
    LLViewerStats& viewer_stats = LLViewerStats::instance();
    viewer_stats.addFrameCounts(packets_in, packets_lost);
    F32 total_packets_in = (F32)viewer_stats.getPacketsInTotal();
    if (total_packets_in > 0.f)
    {
        F32 total_packets_lost = (F32)viewer_stats.getPacketsLostTotal();
        sample(LLStatViewer::PACKETS_LOST_PERCENT, LLUnits::Ratio::fromValue((F32)total_packets_lost/(F32)total_packets_in));
    }

    mLastPacketsIn = gMessageSystem->mPacketsIn;
    mLastPacketsOut = gMessageSystem->mPacketsOut;
    mLastPacketsLost = gMessageSystem->mDroppedPackets;
}


void LLWorld::printPacketsLost()
{
    LL_INFOS() << "Simulators:" << LL_ENDL;
    LL_INFOS() << "----------" << LL_ENDL;

    LLCircuitData *cdp = NULL;
    for (region_list_t::iterator iter = mActiveRegionList.begin();
         iter != mActiveRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        cdp = gMessageSystem->mCircuitInfo.findCircuit(regionp->getHost());
        if (cdp)
        {
            LLVector3d range = regionp->getCenterGlobal() - gAgent.getPositionGlobal();

            LL_INFOS() << regionp->getHost() << ", range: " << range.length()
                    << " packets lost: " << cdp->getPacketsLost() << LL_ENDL;
        }
    }
    LL_INFOS() << "Packets dropped by Packet Ring: " << gMessageSystem->mPacketRing.getNumDroppedPackets() << LL_ENDL;
}

void LLWorld::processCoarseUpdate(LLMessageSystem* msg, void** user_data)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegion(msg->getSender());
    if( region )
    {
        region->updateCoarseLocations(msg);
    }
}

F32 LLWorld::getLandFarClip() const
{
    return mLandFarClip;
}

void LLWorld::setLandFarClip(const F32 far_clip)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_ENVIRONMENT;
    static S32 const rwidth = (S32)REGION_WIDTH_U32;
    S32 const n1 = (llceil(mLandFarClip) - 1) / rwidth;
    S32 const n2 = (llceil(far_clip) - 1) / rwidth;
    bool need_water_objects_update = n1 != n2;

    mLandFarClip = far_clip;

    if (need_water_objects_update)
    {
        updateWaterObjects();
    }
}

// Some region that we're connected to, but not the one we're in, gave us
// a (possibly) new water height. Update it in our local copy.
void LLWorld::waterHeightRegionInfo(std::string const& sim_name, F32 water_height)
{
    for (region_list_t::iterator iter = mRegionList.begin(); iter != mRegionList.end(); ++iter)
    {
        if ((*iter)->getName() == sim_name)
        {
            (*iter)->setWaterHeight(water_height);
            break;
        }
    }
}

void LLWorld::clearHoleWaterObjects()
{
    for (std::list<LLPointer<LLVOWater> >::iterator iter = mHoleWaterObjects.begin();
        iter != mHoleWaterObjects.end(); ++iter)
    {
        LLVOWater* waterp = (*iter).get();
        gObjectList.killObject(waterp);
    }
    mHoleWaterObjects.clear();
    mHoleWaterKeyValid = false;
}

void LLWorld::clearEdgeWaterObjects()
{
    for (S32 i = 0; i < EDGE_WATER_OBJECTS_COUNT; i++)
    {
        gObjectList.killObject(mEdgeWaterObjects[i]);
        mEdgeWaterObjects[i] = NULL;
    }
}

void LLWorld::updateWaterObjects()
{
    CROSSING_SCOPE(LLCrossingTrace::CH_WATER, "Crossing updateWaterObjects");
    if (!gAgent.getRegion())
    {
        return;
    }
    if (mRegionList.empty())
    {
        LL_WARNS() << "No regions!" << LL_ENDL;
        return;
    }

    // First, determine the min and max "box" of water objects
    S32 min_x = 0;
    S32 min_y = 0;
    S32 max_x = 0;
    S32 max_y = 0;
    U32 region_x, region_y;

    S32 rwidth = 256;

    // We only want to fill in water for stuff that's near us, say, within 256 or 512m
    S32 range = LLViewerCamera::getInstance()->getFar() > 256.f ? 512 : 256;

    LLViewerRegion* regionp = gAgent.getRegion();
    from_region_handle(regionp->getHandle(), &region_x, &region_y);

    min_x = (S32)region_x - range;
    min_y = (S32)region_y - range;
    max_x = (S32)region_x + range;
    max_y = (S32)region_y + range;

    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        LLVOWater* waterp = regionp->getLand().getWaterObj();
        if (waterp)
        {
            gObjectList.updateActive(waterp);
        }
    }

    // Use the water height of the region we're on for areas where there is no region
    F32 water_height = gAgent.getRegion()->getWaterHeight();

    // Now, get a list of the holes
    std::vector<std::pair<S32, S32> > holes;
    std::vector<U64> hole_handles;
    S32 x, y;
    for (x = min_x; x <= max_x; x += rwidth)
    {
        for (y = min_y; y <= max_y; y += rwidth)
        {
            U64 region_handle = to_region_handle(x, y);
            if (!getRegionFromHandle(region_handle))
            {   // No region at that area, so make water
                holes.emplace_back(x, y);
                hole_handles.push_back(region_handle);
            }
        }
    }

    // #568: rebuilding every hole-water object forces a Vulkan re-ingest for each. This is
    // called from addRegion/removeRegion/setRegion (several times per crossing), so skip
    // the rebuild when the hole set and everything that shapes the objects is unchanged.
    bool unchanged = mHoleWaterKeyValid
        && mHoleWaterKeyRegion == gAgent.getRegion()
        && mHoleWaterKeyHeight == water_height
        && mHoleWaterKeyHandles == hole_handles
        && mHoleWaterObjects.size() == holes.size();
    if (unchanged)
    {
        for (const LLPointer<LLVOWater>& w : mHoleWaterObjects)
        {
            if (w.isNull() || w->isDead())
            {
                unchanged = false;
                break;
            }
        }
    }

    if (!unchanged)
    {
        crossing_scope_.setA(1);    // rebuilt (counted per call; summed in the Crossing568 block)
        clearHoleWaterObjects();
        for (const auto& h : holes)
        {
            LLVOWater* waterp = (LLVOWater *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_WATER, gAgent.getRegion());
            waterp->setPositionGlobal(LLVector3d(h.first + rwidth/2,
                                                 h.second + rwidth/2,
                                                 256.f + water_height));
            waterp->setScale(LLVector3((F32)rwidth, (F32)rwidth, 512.f));
            gPipeline.createObject(waterp);
            mHoleWaterObjects.push_back(waterp);
        }
        mHoleWaterKeyHandles.swap(hole_handles);
        mHoleWaterKeyRegion = gAgent.getRegion();
        mHoleWaterKeyHeight = water_height;
        mHoleWaterKeyValid = true;
    }

    // Update edge water objects
    S32 wx, wy;
    S32 center_x, center_y;
    wx = (max_x - min_x) + rwidth;
    wy = (max_y - min_y) + rwidth;
    center_x = min_x + (wx >> 1);
    center_y = min_y + (wy >> 1);

    S32 add_boundary[4] = {
        (S32)(512 - (max_x - region_x)),
        (S32)(512 - (max_y - region_y)),
        (S32)(512 - (region_x - min_x)),
        (S32)(512 - (region_y - min_y)) };

    S32 dir;
    for (dir = 0; dir < EDGE_WATER_OBJECTS_COUNT; dir++)
    {
        S32 dim[2] = { 0 };
        switch (gDirAxes[dir][0])
        {
        case -1: dim[0] = add_boundary[2]; break;
        case  0: dim[0] = wx; break;
        default: dim[0] = add_boundary[0]; break;
        }
        switch (gDirAxes[dir][1])
        {
        case -1: dim[1] = add_boundary[3]; break;
        case  0: dim[1] = wy; break;
        default: dim[1] = add_boundary[1]; break;
        }

        // Resize and reshape the water objects
        const S32 water_center_x = center_x + ll_round((wx + dim[0]) * 0.5f * gDirAxes[dir][0]);
        const S32 water_center_y = center_y + ll_round((wy + dim[1]) * 0.5f * gDirAxes[dir][1]);

        LLVOWater* waterp = mEdgeWaterObjects[dir];
        if (!waterp || waterp->isDead())
        {
            // The edge water objects can be dead because they're attached to the region that the
            // agent was in when they were originally created.
            mEdgeWaterObjects[dir] = (LLVOWater *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_VOID_WATER,
                                                                                 gAgent.getRegion());
            waterp = mEdgeWaterObjects[dir];
            waterp->setIsEdgePatch(true);
            gPipeline.createObject(waterp);
        }

        waterp->setRegion(gAgent.getRegion());
        LLVector3d water_pos(water_center_x, water_center_y, 256.f + water_height) ;
        LLVector3 water_scale((F32) dim[0], (F32) dim[1], 512.f);

        //stretch out to horizon
        water_scale.mV[0] += fabsf(2048.f * gDirAxes[dir][0]);
        water_scale.mV[1] += fabsf(2048.f * gDirAxes[dir][1]);

        water_pos.mdV[0] += 1024.f * gDirAxes[dir][0];
        water_pos.mdV[1] += 1024.f * gDirAxes[dir][1];

        waterp->setPositionGlobal(water_pos);
        waterp->setScale(water_scale);

        gObjectList.updateActive(waterp);
    }
}


void LLWorld::shiftRegions(const LLVector3& offset)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    for (region_list_t::const_iterator i = getRegionList().begin(); i != getRegionList().end(); ++i)
    {
        LLViewerRegion* region = *i;
        region->updateRenderMatrix();
    }

    LLViewerPartSim::getInstance()->shift(offset);
}

LLViewerTexture* LLWorld::getDefaultWaterTexture()
{
    return mDefaultWaterTexturep;
}

void LLWorld::setSpaceTimeUSec(const U64MicrosecondsImplicit space_time_usec)
{
    mSpaceTimeUSec = space_time_usec;
}

U64MicrosecondsImplicit LLWorld::getSpaceTimeUSec() const
{
    return mSpaceTimeUSec;
}

void LLWorld::requestCacheMisses()
{
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        regionp->requestCacheMisses();
    }
}

void LLWorld::getInfo(LLSD& info)
{
    LLSD region_info;
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        regionp->getInfo(region_info);
        info["World"].append(region_info);
    }
}

void LLWorld::disconnectRegions()
{
    LLMessageSystem* msg = gMessageSystem;
    for (region_list_t::iterator iter = mRegionList.begin();
         iter != mRegionList.end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        if (regionp == gAgent.getRegion())
        {
            // Skip the main agent
            continue;
        }

        LL_INFOS() << "Sending AgentQuitCopy to: " << regionp->getHost() << LL_ENDL;
        msg->newMessageFast(_PREHASH_AgentQuitCopy);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_FuseBlock);
        msg->addU32Fast(_PREHASH_ViewerCircuitCode, gMessageSystem->mOurCircuitCode);
        msg->sendMessage(regionp->getHost());
    }
}

void process_enable_simulator(LLMessageSystem *msg, void **user_data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    // enable the appropriate circuit for this simulator and
    // add its values into the gSimulator structure
    U64     handle;
    U32     ip_u32;
    U16     port;

    msg->getU64Fast(_PREHASH_SimulatorInfo, _PREHASH_Handle, handle);
    msg->getIPAddrFast(_PREHASH_SimulatorInfo, _PREHASH_IP, ip_u32);
    msg->getIPPortFast(_PREHASH_SimulatorInfo, _PREHASH_Port, port);

    // which simulator should we modify?
    LLHost sim(ip_u32, port);

    // Region ring cap (#dd4k): neighbours outside the SlipstreamMaxRegionRing square /
    // network draw distance, over the region cap, or arriving faster than new circuits
    // are paced are not connected now (stashed; the pacer in reconnectStashedSims
    // connects them later if the rules still accept them).
    if (LLWorld::getInstance()->refuseOutOfRingSimulator(handle, sim))
    {
        return;
    }

    LLWorld::getInstance()->connectSimulator(handle, sim);
}

LLViewerRegion* LLWorld::connectSimulator(const U64& region_handle, const LLHost& sim)
{
    LLMessageSystem* msg = gMessageSystem;

    mRefusedSims.erase(region_handle);
    mDroppedSims.erase(region_handle);
    mRingStashDirty = true;

    // Viewer trusts the simulator.
    msg->enableCircuit(sim, true);
    LLViewerRegion* regionp = addRegion(region_handle, sim);

    // give the simulator a message it can use to get ip and port
    LL_INFOS() << "simulator_enable() Enabling " << sim << " with code " << msg->getOurCircuitCode() << LL_ENDL;
    msg->newMessageFast(_PREHASH_UseCircuitCode);
    msg->nextBlockFast(_PREHASH_CircuitCode);
    msg->addU32Fast(_PREHASH_Code, msg->getOurCircuitCode());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_ID, gAgent.getID());
    msg->sendReliable(sim);
    return regionp;
}

// ---------------------------------------------------------------------------
// Region ring cap (#dd4k)
// ---------------------------------------------------------------------------
namespace
{
    constexpr U32 MAX_REGION_RING        = 8;      // 17x17
    constexpr F64 RING_DROP_GRACE_SECS   = 10.0;   // ring stable this long before drops
    constexpr F64 RING_SWEEP_PERIOD_SECS = 1.0;
    constexpr F64 RING_STASH_TTL_SECS    = 180.0;  // sim-side pending child agents expire
    constexpr F64 RING_DROPPED_TTL_SECS  = 120.0;  // quiet late DisableSimulators
    constexpr F64 RING_REANCHOR_SECS     = 15.0;   // anchor != agent region (failed crossing)
    constexpr F64 RING_DD_CONNECT_MARGIN = 64.0;   // connect within network draw distance + this ...
    constexpr F64 RING_RENDER_EDGE_TOLERANCE = 0.0; // ... but never beyond the RENDER draw distance + this
    constexpr F64 RING_RECENT_ROOT_SECS  = 60.0;   // never drop a region we were root in this recently (handoff)
    constexpr F64 RING_STATE_LOG_SECS    = 60.0;
    constexpr F64 RING_SILENT_SECS       = 5.0;    // no packet this long = silent right now (stall guard)
    constexpr size_t RING_MASS_SILENT_MIN = 4;     // neighbours with circuits before "most are silent" counts
    constexpr F64 RING_STALL_LOG_SECS    = 10.0;
    constexpr size_t RING_MAX_DROPS_PER_SWEEP = 2; // stale drops per 1 s sweep
    constexpr U64 RING_NO_HANDLE         = ~(U64)0;

    U32 region_ring_setting()
    {
        static LLCachedControl<U32> s_ring(gSavedSettings, "SlipstreamMaxRegionRing", 2);
        return llmin((U32)s_ring, MAX_REGION_RING);
    }

    F64 stale_region_secs()
    {
        static LLCachedControl<F32> s_stale(gSavedSettings, "SlipstreamStaleRegionSecs", 30.f);
        return (F64)llmax((F32)s_stale, 0.f);   // 0 = off
    }

    // Hard cap on connected regions: the ring's square, (2N+1)^2. The agent / anchor
    // region and their 8 neighbours (the "core", needed for crossings) are exempt.
    size_t region_count_cap()
    {
        const size_t side = 2 * (size_t)region_ring_setting() + 1;
        return side * side;
    }

    // At most one NEW non-core circuit per this interval (no connect bursts).
    F64 region_connect_interval()
    {
        static LLCachedControl<U32> s_ms(gSavedSettings, "SlipstreamRegionConnectIntervalMs", 250);
        return (F64)(U32)s_ms / 1000.0;
    }

    // The NETWORK draw distance (what AgentUpdate Far asks the sim for), not RenderFarClip:
    // the sim only enables neighbours within Far, so connect rules must agree with it.
    // #850: Far is lowered gradually, so while it ramps down the sim still enables
    // regions out to the Far actually sent: accept those too (the larger of the two).
    F64 ring_draw_distance()
    {
        return (F64)llmax(llmax(gAgentCamera.getNetworkDrawDistance(), gAgentCamera.getNetFarSent()), 0.f);
    }

    // Region footprint in 256 m grid cells, [x0, x1) x [y0, y1). Two regions are
    // neighbours when the closed rects intersect: edge OR corner contact, matching LL's
    // 8-direction connectNeighbor in LLWorld::addRegion.
    struct GridRect
    {
        S64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    GridRect grid_rect(U64 handle, F32 width)
    {
        U32 x = 0, y = 0;
        from_region_handle(handle, &x, &y);
        const S64 cells = llmax((S64)1, (S64)std::ceil(llmax(width, 1.f) / REGION_WIDTH_METERS));
        GridRect r;
        r.x0 = (S64)(x / REGION_WIDTH_U32);
        r.y0 = (S64)(y / REGION_WIDTH_U32);
        r.x1 = r.x0 + cells;
        r.y1 = r.y0 + cells;
        return r;
    }

    bool grid_touch(const GridRect& a, const GridRect& b)
    {
        return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
    }

    bool grid_touch_any(const GridRect& a, const std::vector<GridRect>& rects)
    {
        for (const GridRect& b : rects)
        {
            if (grid_touch(a, b))
            {
                return true;
            }
        }
        return false;
    }

    // Horizontal distance from pos to the nearest point of the region rect (0 inside).
    F64 region_edge_distance(const LLVector3d& pos, U64 handle, F32 width)
    {
        U32 x = 0, y = 0;
        from_region_handle(handle, &x, &y);
        const F64 w = (F64)llmax(width, 1.f);
        const F64 dx = llmax(llmax((F64)x - pos.mdV[VX], pos.mdV[VX] - ((F64)x + w)), 0.0);
        const F64 dy = llmax(llmax((F64)y - pos.mdV[VY], pos.mdV[VY] - ((F64)y + w)), 0.0);
        return std::sqrt(dx * dx + dy * dy);
    }

    // BFS over the connected regions from the seeds.
    void flood_reachable(const std::vector<GridRect>& rects, const std::vector<bool>& seed,
                         std::vector<bool>& reach)
    {
        reach.assign(rects.size(), false);
        std::vector<size_t> queue;
        queue.reserve(rects.size());
        for (size_t i = 0; i < rects.size(); ++i)
        {
            if (seed[i])
            {
                reach[i] = true;
                queue.push_back(i);
            }
        }
        for (size_t q = 0; q < queue.size(); ++q)
        {
            const GridRect& a = rects[queue[q]];
            for (size_t j = 0; j < rects.size(); ++j)
            {
                if (!reach[j] && grid_touch(a, rects[j]))
                {
                    reach[j] = true;
                    queue.push_back(j);
                }
            }
        }
    }
}

// Acceptance context for EnableSimulator / stash candidates.
struct LLWorld::RingEval
{
    bool                    mReachOn = false;   // seeds connected: refuse unreachable
    std::vector<GridRect>   mReachable;         // connected regions reachable from the seeds
    bool                    mDistOn = false;    // reference point valid: refuse too far
    LLVector3d              mAgentPos;          // agent, or anchor centre while busy
    F64                     mDrawDistance = 0.0;  // connect limit, see buildRingEval
    std::vector<GridRect>   mCore;              // agent / anchor regions (their 3x3 always allowed)
};

struct LLWorld::RingRegionInfo
{
    LLViewerRegion* mRegion = nullptr;
    S64     mGridX = 0;
    S64     mGridY = 0;
    bool    mRoot = false;
    bool    mAnchor = false;
    bool    mInRing = true;
    bool    mReachable = true;
    bool    mNearCore = false;      // touches the agent / anchor region
    F64     mEdgeDist = 0.0;
    F64     mSincePacket = -1.0;    // -1 = no circuit
};

bool LLWorld::isInRegionRing(const U64& region_handle, F32 width) const
{
    U64 anchor = mRingAnchor;
    if (!anchor)
    {
        LLViewerRegion* agent_region = gAgent.getRegion();
        if (!agent_region)
        {
            return true;    // nothing to centre on yet (login): never refuse
        }
        anchor = agent_region->getHandle();
    }
    if (region_handle == anchor)
    {
        return true;
    }

    // Anchor rect expanded by ring * 256 m per side; the candidate rect must overlap it
    // (strictly, so ring 2 is exactly 5 columns of 256 m regions).
    F32 ax = 0.f, ay = 0.f, cx = 0.f, cy = 0.f;
    from_region_handle(anchor, &ax, &ay);
    from_region_handle(region_handle, &cx, &cy);
    F32 aw = REGION_WIDTH_METERS;
    for (const LLViewerRegion* regionp : mRegionList)
    {
        if (regionp && regionp->getHandle() == anchor)
        {
            aw = llmax(regionp->getWidth(), 1.f);
            break;
        }
    }
    const F32 cw = llmax(width, 1.f);
    const F32 e  = (F32)region_ring_setting() * REGION_WIDTH_METERS;
    const bool ox = (cx < ax + aw + e) && (cx + cw > ax - e);
    const bool oy = (cy < ay + aw + e) && (cy + cw > ay - e);
    return ox && oy;
}

void LLWorld::buildRingEval(RingEval& eval, U64 exclude_handle) const
{
    eval = RingEval();
    LLViewerRegion* agent_region = gAgent.getRegion();
    // While teleporting the agent region is still the OLD root: only the destination
    // (the anchor) seeds reachability, or the old estate's neighbours would pass.
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    std::vector<GridRect> rects;
    std::vector<bool> seed;
    rects.reserve(mRegionList.size());
    seed.reserve(mRegionList.size());
    bool any_seed = false;
    bool anchor_found = false;
    for (const LLViewerRegion* regionp : mRegionList)
    {
        if (!regionp || regionp->getHandle() == exclude_handle)
        {
            continue;
        }
        const bool is_anchor = mRingAnchor && regionp->getHandle() == mRingAnchor;
        const bool is_root = regionp == agent_region && !teleporting;
        rects.push_back(grid_rect(regionp->getHandle(), regionp->getWidth()));
        seed.push_back(is_anchor || is_root);
        any_seed |= is_anchor || is_root;
        anchor_found |= is_anchor;
        if (is_anchor || is_root)
        {
            eval.mCore.push_back(rects.back());
        }
    }
    if (mRingAnchor && !anchor_found)
    {
        eval.mCore.push_back(grid_rect(mRingAnchor, REGION_WIDTH_METERS));
    }
    if (any_seed)
    {
        std::vector<bool> reach;
        flood_reachable(rects, seed, reach);
        for (size_t i = 0; i < rects.size(); ++i)
        {
            if (reach[i])
            {
                eval.mReachable.push_back(rects[i]);
            }
        }
        eval.mReachOn = true;
    }
    // Distance reference: the agent; during teleport / login the anchor's centre (its
    // 3x3 stays exempt via mCore, the rest is re-evaluated after arrival).
    if (agent_region && !teleporting && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        eval.mDistOn = true;
        eval.mAgentPos = gAgent.getPositionGlobal();
    }
    else if (mRingAnchor)
    {
        F32 aw = REGION_WIDTH_METERS;
        for (const LLViewerRegion* regionp : mRegionList)
        {
            if (regionp && regionp->getHandle() == mRingAnchor)
            {
                aw = llmax(regionp->getWidth(), 1.f);
                break;
            }
        }
        eval.mDistOn = true;
        eval.mAgentPos = from_region_handle(mRingAnchor) + LLVector3d(aw * 0.5, aw * 0.5, 0.0);
    }
    // Connect limit (nearest region edge from the agent): network DD + margin, but never
    // beyond the render draw distance (owner invariant: never connect regions whose
    // nearest edge the renderer would not draw). The core 3x3 is exempt (crossings).
    eval.mDrawDistance = llmin(ring_draw_distance() + RING_DD_CONNECT_MARGIN,
                               (F64)gAgentCamera.mDrawDistance + RING_RENDER_EDGE_TOLERANCE);
}

const char* LLWorld::ringRefusal(const RingEval& eval, U64 region_handle, F32 width) const
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (region_handle == mRingAnchor || (agent_region && region_handle == agent_region->getHandle()))
    {
        return nullptr;
    }
    // #850: the core (agent / anchor region and their 8 neighbours, needed for crossings)
    // is exempt from the ring BEFORE the ring test: with SlipstreamMaxRegionRing=0 the
    // ring is the anchor alone, and the neighbours we cross into were never connected.
    const GridRect r = grid_rect(region_handle, width);
    const bool core = grid_touch_any(r, eval.mCore);
    if (!core && !isInRegionRing(region_handle, width))
    {
        return "outside the ring";
    }
    if (eval.mReachOn && !grid_touch_any(r, eval.mReachable))
    {
        return "unreachable";
    }
    if (eval.mDistOn && !core
        && region_edge_distance(eval.mAgentPos, region_handle, width) > eval.mDrawDistance)
    {
        return "beyond draw distance";
    }
    return nullptr;
}

bool LLWorld::isRingCore(const RingEval& eval, U64 region_handle, F32 width) const
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (region_handle == mRingAnchor || (agent_region && region_handle == agent_region->getHandle()))
    {
        return true;
    }
    return grid_touch_any(grid_rect(region_handle, width), eval.mCore);
}

bool LLWorld::refuseOutOfRingSimulator(const U64& region_handle, const LLHost& host)
{
    // Unknown width at EnableSimulator: 256 m, never looser than the real rect, so a
    // region dropped with its real width cannot bounce back in.
    F32 width = REGION_WIDTH_METERS;
    LLViewerRegion* existing = getRegionFromHandle(region_handle);
    if (existing)
    {
        width = existing->getWidth();
    }
    RingEval eval;
    buildRingEval(eval, region_handle);
    const char* why = ringRefusal(eval, region_handle, width);
    const F64 now = LLFrameTimer::getTotalSeconds();
    bool deferred = false;  // accepted by the rules, waiting for the cap / the pacer
    if (!why && !existing && !isRingCore(eval, region_handle, width))
    {
        // The ring is a backstop to the network draw distance: the sim only enables what
        // Far asks for, so the rules above accept nearly everything it sends. Still:
        // never more than the cap, and new circuits one at a time, not in bursts.
        if (mRegionList.size() >= region_count_cap())
        {
            why = "region cap";
            deferred = true;
        }
        else if (now - mRingLastConnect < region_connect_interval())
        {
            why = "paced";
            deferred = true;
        }
        else
        {
            mRingLastConnect = now;     // the caller connects it right away
        }
    }
    if (!why)
    {
        mRefusedSims.erase(region_handle);
        return false;
    }

    if (deferred)
    {
        ++mRingDeferredMsgs;
    }
    else
    {
        ++mRingRefusedMsgs;
    }
    auto it = mRefusedSims.find(region_handle);
    if (it == mRefusedSims.end())
    {
        if (!deferred)
        {
            ++mRingRefusedRegions;
        }
        RefusedSim& rs = mRefusedSims[region_handle];
        rs.mHost = host;
        rs.mLastSeen = now;
        mRingStashDirty = true;     // the pacer has a new candidate
        U32 x = 0, y = 0;
        from_region_handle(region_handle, &x, &y);
        LL_DEBUGS("RegionRing") << (deferred ? "Deferring" : "Refusing") << " EnableSimulator for region ("
                               << x / 256 << ":" << y / 256 << ") " << host << ": " << why << " (ring "
                               << (2 * region_ring_setting() + 1) << "x" << (2 * region_ring_setting() + 1)
                               << ", connected " << mRegionList.size() << "/" << region_count_cap()
                               << ", refused regions " << mRingRefusedRegions << ", msgs " << mRingRefusedMsgs
                               << ", stashed " << mRefusedSims.size() << ")" << LL_ENDL;
    }
    else
    {
        if (it->second.mHost != host)
        {
            it->second.mSeedCap.clear();    // different sim process: old seed is stale
        }
        it->second.mHost = host;
        it->second.mLastSeen = now;
        LL_DEBUGS("RegionRing") << "Re-sent EnableSimulator stashed for " << host << ": " << why << LL_ENDL;
    }
    return true;
}

bool LLWorld::stashRefusedSeedCapability(const LLHost& host, const std::string& seed)
{
    for (auto& entry : mRefusedSims)
    {
        if (entry.second.mHost == host)
        {
            entry.second.mSeedCap = seed;
            entry.second.mLastSeen = LLFrameTimer::getTotalSeconds();
            LL_DEBUGS("RegionRing") << "Stashed seed capability for refused sim " << host << LL_ENDL;
            return true;
        }
    }
    for (const auto& entry : mDroppedSims)
    {
        if (entry.second.first == host)
        {
            return true;    // late EstablishAgentCommunication for a dropped region
        }
    }
    return false;
}

bool LLWorld::forgetRefusedOrDroppedHost(const LLHost& host)
{
    bool found = false;
    for (auto it = mRefusedSims.begin(); it != mRefusedSims.end(); )
    {
        if (it->second.mHost == host) { it = mRefusedSims.erase(it); found = true; }
        else ++it;
    }
    for (auto it = mDroppedSims.begin(); it != mDroppedSims.end(); )
    {
        if (it->second.first == host) { it = mDroppedSims.erase(it); found = true; }
        else ++it;
    }
    return found;
}

void LLWorld::setRegionRingAnchor(const U64& region_handle)
{
    if (!region_handle || region_handle == mRingAnchor)
    {
        return;
    }
    mRingAnchor = region_handle;
    mRingChangedTime = LLFrameTimer::getTotalSeconds();
    // #850: forget the anchor's stash entry only once it is connected (teleport and
    // crossing both add the region first). Otherwise keep its host + seed: the anchor is
    // core, so reconnectStashedSims below connects it from the stash.
    if (getRegionFromHandle(region_handle))
    {
        mRefusedSims.erase(region_handle);
        mDroppedSims.erase(region_handle);
    }
    else if (mRefusedSims.count(region_handle))
    {
        LL_INFOS("RegionRing") << "Ring anchor moved to a stashed region " << mRefusedSims[region_handle].mHost
                               << "; connecting it from the stash" << LL_ENDL;
    }

    // Connect stashed regions the ring now covers (crossing preload): the new core at
    // once, the rest through the pacer.
    mRingStashDirty = true;
    reconnectStashedSims();
}

LLViewerRegion* LLWorld::ensureCrossingDestination(const U64& region_handle, const LLHost& host,
                                                   std::string& seed_cap, std::string& action)
{
    LLViewerRegion* regionp = getRegionFromHandle(region_handle);
    const LLCircuitData* cdp = gMessageSystem->mCircuitInfo.findCircuit(host);
    // A circuit that exists but is marked dead drops sends too ("dead circuit"), exactly
    // like a missing one; enableCircuit (in connectSimulator) revives it.
    if (regionp && regionp->getHost() == host && cdp && cdp->isAlive())
    {
        action = "pre-connected";
        return regionp;
    }
    const char* why = !regionp ? "no region"
                    : (regionp->getHost() != host ? "region on another host"
                    : (!cdp ? "no circuit" : "circuit dead"));
    std::string extra;
    auto it = mRefusedSims.find(region_handle);
    if (it != mRefusedSims.end())
    {
        extra = it->second.mSeedCap.empty() ? ", was stashed" : ", was stashed with seed";
        if (seed_cap.empty() && it->second.mHost == host && !it->second.mSeedCap.empty())
        {
            seed_cap = it->second.mSeedCap;     // CrossedRegion carried none: use the stashed one
        }
    }
    if (mDroppedSims.count(region_handle))
    {
        extra += ", was dropped";
    }
    U32 x = 0, y = 0;
    from_region_handle(region_handle, &x, &y);
    LL_WARNS("RegionRing") << "Crossing into region (" << x / 256 << ":" << y / 256 << ") " << host
                           << ": destination not pre-connected (" << why << extra
                           << "); connecting now, UseCircuitCode before CompleteAgentMovement" << LL_ENDL;
    action = std::string("connected_at_crossing(") + why + extra + ")";
    LLCrossingTrace::regionEvent("destination not pre-connected: " + action);
    // enableCircuit + addRegion + UseCircuitCode; also clears the stash / dropped entries.
    return connectSimulator(region_handle, host);
}

// Called every frame (updateRegionRing) and on anchor moves. Connects, from the stash:
// every core region (agent / anchor 3x3, needed for crossings) the rules accept, and at
// most ONE other region per SlipstreamRegionConnectIntervalMs -- the nearest -- while
// under the region cap. A region reachable only through another stashed one connects
// on a later call, once its neighbour is in. (Before: up to 8 passes connecting every
// acceptable stashed region at once -- bursts of dozens of new circuits per crossing.)
void LLWorld::reconnectStashedSims()
{
    if (mRefusedSims.empty())
    {
        mRingStashDirty = false;
        return;
    }
    if (mInStashReconnect)
    {
        return;     // nested call (connectSimulator -> ...): the next frame picks it up
    }
    const F64 now = LLFrameTimer::getTotalSeconds();
    const bool pace_ok = now - mRingLastConnect >= region_connect_interval();
    if (!mRingStashDirty && (!pace_ok || mRingStashIdle))
    {
        // Nothing changed since the last scan, and either no pacing slot or the last
        // scan found nothing to connect: cheap per-frame exit.
        return;
    }
    mRingStashDirty = false;
    mInStashReconnect = true;

    RingEval eval;
    buildRingEval(eval, RING_NO_HANDLE);
    std::vector<U64> core_ready;
    std::vector<U64> already_connected;
    U64 best = RING_NO_HANDLE;
    F64 best_dist = 0.0;
    for (const auto& entry : mRefusedSims)
    {
        if (getRegionFromHandle(entry.first))
        {
            already_connected.push_back(entry.first);
            continue;
        }
        if (ringRefusal(eval, entry.first, REGION_WIDTH_METERS))
        {
            continue;
        }
        if (isRingCore(eval, entry.first, REGION_WIDTH_METERS))
        {
            core_ready.push_back(entry.first);
            continue;
        }
        if (!pace_ok)
        {
            continue;
        }
        const F64 dist = eval.mDistOn ? region_edge_distance(eval.mAgentPos, entry.first, REGION_WIDTH_METERS) : 0.0;
        if (best == RING_NO_HANDLE || dist < best_dist)
        {
            best = entry.first;
            best_dist = dist;
        }
    }
    for (U64 handle : already_connected)
    {
        mRefusedSims.erase(handle);
    }
    // Only a full scan (pacing slot open) can tell "nothing paceable".
    mRingStashIdle = pace_ok && core_ready.empty()
                     && (best == RING_NO_HANDLE || mRegionList.size() >= region_count_cap());

    auto connect_stashed = [this](U64 handle, const char* what)
    {
        auto it = mRefusedSims.find(handle);
        if (it == mRefusedSims.end())
        {
            return;
        }
        const RefusedSim rs = it->second;
        mRefusedSims.erase(it);
        ++mRingLateConnected;
        LL_INFOS("RegionRing") << "Connecting stashed sim " << rs.mHost << " (" << what << ", connected "
                               << mRegionList.size() << "/" << region_count_cap() << ", late-connected "
                               << mRingLateConnected << ")" << LL_ENDL;
        LLViewerRegion* regionp = connectSimulator(handle, rs.mHost);
        if (regionp && !rs.mSeedCap.empty())
        {
            regionp->setSeedCapability(rs.mSeedCap);
        }
    };

    for (U64 handle : core_ready)
    {
        connect_stashed(handle, "core");
    }
    if (best != RING_NO_HANDLE && mRegionList.size() < region_count_cap())
    {
        mRingLastConnect = now;
        connect_stashed(best, "paced");
    }
    mInStashReconnect = false;
}

void LLWorld::dropRegion(LLViewerRegion* regionp, const char* reason, F64 now)
{
    const LLHost host = regionp->getHost();
    const U64 handle = regionp->getHandle();
    ++mRingDropped;
    LL_INFOS("RegionRing") << "Dropping region " << regionp->getName() << " " << host << ": " << reason
                           << " (dropped " << mRingDropped << ", connected " << mRegionList.size() - 1
                           << ")" << LL_ENDL;
    // Forget it locally, exactly like a DisableSimulator from the sim. NO AgentQuitCopy:
    // that is the LOGOUT message (disconnectRegions) -- sent to a sim that still holds our
    // presence (a region we were root in seconds ago, agent handoff in flight at speed) it
    // logged the whole agent out: every sim went silent at once (Zindra fly test,
    // 2026-10-09; likely #570). The sim's child agent times out on its own schedule.
    LLMessageSystem* msg = gMessageSystem;

    mDroppedSims[handle] = std::make_pair(host, now);
    removeRegion(host);
    msg->disableCircuit(host);
}

void LLWorld::logRegionRingState(const std::vector<RingRegionInfo>& infos, F64 now)
{
    std::ostringstream sig;
    S64 root_x = 0, root_y = 0;
    for (const RingRegionInfo& info : infos)
    {
        sig << info.mRegion->getHandle() << (info.mReachable ? 'r' : 'u') << (info.mRoot ? 'R' : '-')
            << (info.mAnchor ? 'A' : '-') << ' ';
        if (info.mRoot)
        {
            root_x = info.mGridX;
            root_y = info.mGridY;
        }
    }
    if (sig.str() == mRingStateSig && now - mRingStateLogTime < RING_STATE_LOG_SECS)
    {
        return;
    }
    mRingStateSig = sig.str();
    mRingStateLogTime = now;

    std::ostringstream os;
    os << std::fixed << std::setprecision(0) << "Connected regions " << infos.size()
       << " (network draw distance " << ring_draw_distance() << " m):";
    for (const RingRegionInfo& info : infos)
    {
        const std::string& name = info.mRegion->getName();
        os << " [" << (name.empty() ? info.mRegion->getHost().getString() : name) << " ("
           << (info.mGridX - root_x) << "," << (info.mGridY - root_y) << ")"
           << (info.mRoot ? " root" : "") << (info.mAnchor ? " anchor" : "")
           << " reach=" << (info.mReachable ? "yes" : "NO") << (info.mInRing ? "" : " out-of-ring")
           << " edge=" << info.mEdgeDist << "m pkt=";
        if (info.mSincePacket < 0.0)
        {
            os << "no-circuit";
        }
        else
        {
            os << std::setprecision(1) << info.mSincePacket << "s" << std::setprecision(0);
        }
        os << "]";
    }
    LL_INFOS("RegionRing") << os.str() << LL_ENDL;
}

void LLWorld::updateRegionRing()
{
    // Every frame: the connect pacer (cheap exit unless a slot is free or something
    // changed). Accepting is safe during arrival (only drops wait for the grace): a region
    // that became reachable / near enough since its EnableSimulator connects now.
    reconnectStashedSims();

    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now - mRingLastSweep < RING_SWEEP_PERIOD_SECS)
    {
        return;
    }
    mRingLastSweep = now;
    mRingStashDirty = true;     // re-evaluate the stash once per sweep (agent moved, DD changed)

    LLViewerRegion* agent_region = gAgent.getRegion();
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (!agent_region || teleporting || LLStartUp::getStartupState() < STATE_STARTED)
    {
        mRingWasBusy = true;
        return;
    }
    if (mRingWasBusy)
    {
        // Restart the drop grace when a teleport / login finishes, not at TeleportFinish:
        // a long arrival must not drop the previous root before its DisableSimulator.
        mRingWasBusy = false;
        mRingChangedTime = now;
    }

    // Ring size changed: re-evaluate (reconnect stash on grow, drop on shrink).
    const U32 ring = region_ring_setting();
    if (ring != mRingSize)
    {
        LL_INFOS("RegionRing") << "SlipstreamMaxRegionRing " << mRingSize << " -> " << ring << LL_ENDL;
        mRingSize = ring;
        const U64 anchor = mRingAnchor ? mRingAnchor : agent_region->getHandle();
        mRingAnchor = 0;
        setRegionRingAnchor(anchor);    // re-runs the stash reconnect, stamps the time
    }

    // A crossing that never completed leaves the anchor on a region the agent is not
    // in; fall back to the agent region.
    if (mRingAnchor != agent_region->getHandle() && now - mRingChangedTime > RING_REANCHOR_SECS)
    {
        setRegionRingAnchor(agent_region->getHandle());
    }

    // Refusal summary (per-region refusals are DEBUG: ~1000 on mainland at 4 km).
    static U32 s_logged_refused = 0;
    static U32 s_logged_deferred = 0;
    static F64 s_last_summary = 0.0;
    if ((mRingRefusedRegions != s_logged_refused || mRingDeferredMsgs != s_logged_deferred)
        && now - s_last_summary > 30.0)
    {
        s_logged_refused = mRingRefusedRegions;
        s_logged_deferred = mRingDeferredMsgs;
        s_last_summary = now;
        LL_INFOS("RegionRing") << "Region ring " << (2 * ring + 1) << "x" << (2 * ring + 1)
                               << ": connected " << mRegionList.size() << "/" << region_count_cap()
                               << " (network draw distance " << llformat("%.0f", ring_draw_distance())
                               << " m), refused regions " << mRingRefusedRegions << " (EnableSimulator msgs "
                               << mRingRefusedMsgs << ", deferred by cap/pacing " << mRingDeferredMsgs
                               << ", stashed " << mRefusedSims.size() << "), dropped " << mRingDropped
                               << ", late-connected " << mRingLateConnected << LL_ENDL;
    }

    // Prune stale stash / dropped entries.
    for (auto it = mRefusedSims.begin(); it != mRefusedSims.end(); )
    {
        if (now - it->second.mLastSeen > RING_STASH_TTL_SECS) it = mRefusedSims.erase(it);
        else ++it;
    }
    for (auto it = mDroppedSims.begin(); it != mDroppedSims.end(); )
    {
        if (now - it->second.second > RING_DROPPED_TTL_SECS) it = mDroppedSims.erase(it);
        else ++it;
    }

    // Classify every connected region: ring, reachability from the agent region /
    // anchor over the CURRENT region list, nearest-edge distance, packet age.
    const F64 msg_now = LLMessageSystem::getMessageTimeSeconds().value();
    const LLVector3d agent_pos = gAgent.getPositionGlobal();
    std::vector<RingRegionInfo> infos;
    std::vector<GridRect> rects;
    std::vector<bool> seed;
    std::vector<GridRect> core;
    infos.reserve(mRegionList.size());
    rects.reserve(mRegionList.size());
    seed.reserve(mRegionList.size());
    for (LLViewerRegion* regionp : mRegionList)
    {
        if (!regionp)
        {
            continue;
        }
        RingRegionInfo info;
        info.mRegion = regionp;
        info.mRoot = regionp == agent_region;
        if (info.mRoot) mRecentRoot[regionp->getHandle()] = now;
        info.mAnchor = regionp->getHandle() == mRingAnchor;
        const GridRect r = grid_rect(regionp->getHandle(), regionp->getWidth());
        info.mGridX = r.x0;
        info.mGridY = r.y0;
        infos.push_back(info);
        rects.push_back(r);
        seed.push_back(info.mRoot || info.mAnchor);
        if (info.mRoot || info.mAnchor)
        {
            core.push_back(r);
        }
    }
    std::vector<bool> reach;
    flood_reachable(rects, seed, reach);
    for (size_t i = 0; i < infos.size(); ++i)
    {
        RingRegionInfo& info = infos[i];
        LLViewerRegion* regionp = info.mRegion;
        info.mReachable = reach[i];
        info.mInRing = isInRegionRing(regionp->getHandle(), regionp->getWidth());
        info.mNearCore = grid_touch_any(rects[i], core);
        info.mEdgeDist = region_edge_distance(agent_pos, regionp->getHandle(), regionp->getWidth());
        if (const LLCircuitData* cdp = gMessageSystem->mCircuitInfo.findCircuit(regionp->getHost()))
        {
            info.mSincePacket = cdp->getPacketsIn() ? llmax(msg_now - cdp->getLastPacketInTime().value(), 0.0)
                                                    : (F64)cdp->getAgeInSeconds();
        }
    }
    logRegionRingState(infos, now);

    // GLOBAL STALL GUARD. A region is only "dead" if it is silent while the rest of the
    // grid keeps talking to us. When the agent's own region is silent, or most connected
    // regions went silent together, the stall is upstream of any one sim (the whole
    // session, the path to the grid, or our own receive side): every region looks stale
    // at once and dropping them would throw away live child agents in bulk (2026-10-09:
    // 54 regions dropped in one sweep while all sims were briefly silent; they were
    // alive and resumed seconds later). Hold every drop while that lasts, and for a full
    // stale window afterwards so each region gets a fresh chance to show it is alive.
    {
        F64 root_since = -1.0;
        size_t considered = 0;
        size_t silent_now = 0;
        for (const RingRegionInfo& info : infos)
        {
            if (info.mRoot)
            {
                root_since = info.mSincePacket;
                continue;
            }
            if (info.mSincePacket < 0.0)
            {
                continue;   // no circuit: says nothing about the grid
            }
            ++considered;
            if (info.mSincePacket > RING_SILENT_SECS)
            {
                ++silent_now;
            }
        }
        const bool root_silent = root_since < 0.0 || root_since > RING_SILENT_SECS;
        const bool mass_silent = considered >= RING_MASS_SILENT_MIN && silent_now * 2 > considered;
        if (root_silent || mass_silent)
        {
            if (mRingStallSince <= 0.0)
            {
                mRingStallSince = now;
            }
            mRingStallLast = now;
            if (now - mRingStallLogTime > RING_STALL_LOG_SECS)
            {
                mRingStallLogTime = now;
                LL_INFOS("RegionRing") << "Global stall, holding region drops: root region "
                                       << (root_silent ? "silent" : "receiving") << " ("
                                       << (root_since < 0.0 ? std::string("no circuit")
                                                            : llformat("%.1f s since last packet", root_since))
                                       << "), " << silent_now << " of " << considered
                                       << " neighbours silent > " << RING_SILENT_SECS << " s, stalled for "
                                       << llformat("%.1f", now - mRingStallSince) << " s" << LL_ENDL;
            }
            return;
        }
        if (mRingStallSince > 0.0)
        {
            LL_INFOS("RegionRing") << "Global stall over after " << llformat("%.1f", now - mRingStallSince)
                                   << " s; region drops held for another " << stale_region_secs() << " s"
                                   << LL_ENDL;
            mRingStallSince = 0.0;
        }
        if (mRingStallLast > 0.0 && now - mRingStallLast < llmax(stale_region_secs(), RING_SILENT_SECS))
        {
            return;
        }
    }

    // Drop regions that fail a rule once the ring has been stable for a grace period
    // (no churn on teleport arrival / login / border bouncing). Never the agent region
    // or the anchor.
    if (now - mRingChangedTime < RING_DROP_GRACE_SECS)
    {
        return;
    }
    for (auto it = mRecentRoot.begin(); it != mRecentRoot.end();)
    {
        if (now - it->second > 2.0 * RING_RECENT_ROOT_SECS) it = mRecentRoot.erase(it); else ++it;
    }
    const F64 stale_secs = stale_region_secs();
    std::vector<std::pair<const RingRegionInfo*, const char*> > to_drop;
    for (const RingRegionInfo& info : infos)
    {
        if (info.mRoot || info.mAnchor)
        {
            continue;
        }
        // A region the agent was root in moments ago may still own our presence on the
        // grid (agent handoff in flight at speed): leave it to the sim's DisableSimulator.
        {
            auto rr = mRecentRoot.find(info.mRegion->getHandle());
            if (rr != mRecentRoot.end() && now - rr->second < RING_RECENT_ROOT_SECS)
            {
                continue;
            }
        }
        // ONLY dead circuits are dropped from our side. Abandoning a LIVE neighbour (outside
        // the ring / unreachable / beyond draw distance) stops our acks while its sim still
        // holds our child agent; ~20 at once on a fast flight at max draw distance got the
        // whole agent logged out (every sim silent within a second, 2026-10-09). Live
        // neighbours leave on the sim's DisableSimulator like any viewer; the network draw
        // distance (AgentUpdate Far, LLAgentCamera::getNetworkDrawDistance) is what keeps
        // the set of child agents the sim creates for us small.
        // #850: a neighbour of the agent / anchor region that still has a circuit is never
        // dropped as stale: it is where the next crossing goes, and a slow neighbour must
        // make a slow crossing, not a broken one. It leaves on its DisableSimulator. One
        // whose circuit the message system already removed is still dropped; crossing into
        // it, or into one whose circuit is marked dead, reconnects it
        // (LLWorld::ensureCrossingDestination).
        if (info.mNearCore && info.mSincePacket >= 0.0)
        {
            continue;
        }
        const char* reason = nullptr;
        if (stale_secs > 0.0 && (info.mSincePacket < 0.0 || info.mSincePacket > stale_secs))
        {
            reason = "stale";
        }
        if (reason)
        {
            to_drop.emplace_back(&info, reason);
        }
    }
    // Rate limit: never a mass drop. The longest-silent (no circuit first) go first; the
    // rest wait for later sweeps, each re-checked against the stall guard. A region
    // reachable only through a dropped one goes on a later sweep too.
    std::sort(to_drop.begin(), to_drop.end(),
              [](const std::pair<const RingRegionInfo*, const char*>& a,
                 const std::pair<const RingRegionInfo*, const char*>& b)
              {
                  const F64 sa = a.first->mSincePacket < 0.0 ? 1.0e30 : a.first->mSincePacket;
                  const F64 sb = b.first->mSincePacket < 0.0 ? 1.0e30 : b.first->mSincePacket;
                  return sa > sb;
              });
    if (to_drop.size() > RING_MAX_DROPS_PER_SWEEP)
    {
        LL_INFOS("RegionRing") << to_drop.size() << " stale regions; dropping " << RING_MAX_DROPS_PER_SWEEP
                               << " this sweep" << LL_ENDL;
        to_drop.resize(RING_MAX_DROPS_PER_SWEEP);
    }
    for (const auto& drop : to_drop)
    {
        dropRegion(drop.first->mRegion, drop.second, now);
    }
}

class LLEstablishAgentCommunication : public LLHTTPNode
{
    LOG_CLASS(LLEstablishAgentCommunication);
public:
    virtual void describe(Description& desc) const
    {
        desc.shortInfo("seed capability info for a region");
        desc.postAPI();
        desc.input(
            "{ seed-capability: ..., sim-ip: ..., sim-port }");
        desc.source(__FILE__, __LINE__);
    }

    virtual void post(ResponsePtr response, const LLSD& context, const LLSD& input) const
    {
        if (LLApp::isExiting())
        {
            return;
        }

        if (gDisconnected)
        {
            return;
        }

        if (!LLWorld::instanceExists())
        {
            return;
        }

        if (!input["body"].has("agent-id") ||
            !input["body"].has("sim-ip-and-port") ||
            !input["body"].has("seed-capability"))
        {
            LL_WARNS() << "invalid parameters" << LL_ENDL;
            return;
        }

        LLHost sim(input["body"]["sim-ip-and-port"].asString());
        if (sim.isInvalid())
        {
            LL_WARNS() << "Got EstablishAgentCommunication with invalid host" << LL_ENDL;
            return;
        }

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegion(sim);
        if (!regionp && LLWorld::getInstance()->stashRefusedSeedCapability(sim, input["body"]["seed-capability"].asString()))
        {
            return; // region ring cap (#dd4k): refused / dropped sim, seed kept for later
        }
        if (!regionp)
        {
            LL_WARNS() << "Got EstablishAgentCommunication for unknown region "
                    << sim << LL_ENDL;
            return;
        }
        LL_DEBUGS("CrossingCaps") << "Calling setSeedCapability from LLEstablishAgentCommunication::post. Seed cap == "
                << input["body"]["seed-capability"] << " for region " << regionp->getRegionID() << LL_ENDL;
        regionp->setSeedCapability(input["body"]["seed-capability"]);
    }
};

// disable the circuit to this simulator
// Called in response to "DisableSimulator" message.
void process_disable_simulator(LLMessageSystem *mesgsys, void **user_data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    LLHost host = mesgsys->getSender();

    //LL_INFOS() << "Disabling simulator with message from " << host << LL_ENDL;
    // Region ring cap (#dd4k): a sim we refused or already dropped has no region.
    const bool known_ring_host = LLWorld::getInstance()->forgetRefusedOrDroppedHost(host);
    if (!known_ring_host || LLWorld::getInstance()->getRegion(host))
    {
        LLWorld::getInstance()->removeRegion(host);
    }

    mesgsys->disableCircuit(host);
}


void process_region_handshake(LLMessageSystem* msg, void** user_data)
{
    LLHost host = msg->getSender();
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegion(host);
    if (!regionp)
    {
        LL_WARNS() << "Got region handshake for unknown region "
            << host << LL_ENDL;
        return;
    }

    regionp->unpackRegionHandshake();
}


void send_agent_pause()
{
    // *NOTE:Mani Pausing the mainloop timeout. Otherwise a long modal event may cause
    // the thread monitor to timeout.
    LLAppViewer::instance()->pauseMainloopTimeout();

    // Note: used to check for LLWorld initialization before it became a singleton.
    // Rather than just remove this check I'm changing it to assure that the message
    // system has been initialized. -MG
    if (!gMessageSystem)
    {
        return;
    }

    gMessageSystem->newMessageFast(_PREHASH_AgentPause);
    gMessageSystem->nextBlockFast(_PREHASH_AgentData);
    gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
    gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);

    gAgentPauseSerialNum++;
    gMessageSystem->addU32Fast(_PREHASH_SerialNum, gAgentPauseSerialNum);

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
         iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        gMessageSystem->sendReliable(regionp->getHost());
    }

    gObjectList.mWasPaused = true;
    LLViewerStats::instance().getRecording().stop();
}

CapUrlMatches LLWorld::getCapURLMatches(const std::string &cap_url)
{
    std::set<std::string> url_capnames;
    std::set<LLViewerRegion*> url_capregions;

    for (auto regionp : LLWorld::getInstance()->getRegionList())
    {
        std::set<std::string> new_url_capnames = regionp->getCapURLNames(cap_url);

        if(!new_url_capnames.empty())
        {
            url_capregions.insert(regionp);
            url_capnames.insert(new_url_capnames.begin(), new_url_capnames.end());
        }
    }

    return CapUrlMatches(url_capregions, url_capnames);
}


bool LLWorld::isCapURLMapped(const std::string &cap_url)
{
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if(regionp->isCapURLMapped(cap_url))
            return true;
    }
    return false;
}

void send_agent_resume()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    // Note: used to check for LLWorld initialization before it became a singleton.
    // Rather than just remove this check I'm changing it to assure that the message
    // system has been initialized. -MG
    if (!gMessageSystem)
    {
        return;
    }

    gMessageSystem->newMessageFast(_PREHASH_AgentResume);
    gMessageSystem->nextBlockFast(_PREHASH_AgentData);
    gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
    gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);

    gAgentPauseSerialNum++;
    gMessageSystem->addU32Fast(_PREHASH_SerialNum, gAgentPauseSerialNum);


    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
         iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* regionp = *iter;
        gMessageSystem->sendReliable(regionp->getHost());
    }

    // Resume data collection to ignore invalid rates
    LLViewerStats::instance().getRecording().resume();

    LLAppViewer::instance()->resumeMainloopTimeout();
}

static LLVector3d unpackLocalToGlobalPosition(U32 compact_local, const LLVector3d& region_origin)
{
    LLVector3d pos_local;

    pos_local.mdV[VZ] = (compact_local & 0xFFU) * 4;
    pos_local.mdV[VY] = (compact_local >> 8) & 0xFFU;
    pos_local.mdV[VX] = (compact_local >> 16) & 0xFFU;

    return region_origin + pos_local;
}

void LLWorld::getAvatars(uuid_vec_t* avatar_ids, std::vector<LLVector3d>* positions, const LLVector3d& relative_to, F32 radius) const
{
    F32 radius_squared = radius * radius;

    if(avatar_ids != NULL)
    {
        avatar_ids->clear();
        avatar_ids->reserve(128);
    }
    if(positions != NULL)
    {
        positions->clear();
        positions->reserve(128);
    }

    // get the list of avatars from the character list first, so distances are correct
    // when agent is above 1020m and other avatars are nearby
    for (auto instance : LLCharacter::sInstances)
    {
        LLVOAvatar* pVOAvatar = static_cast<LLVOAvatar*>(instance);

        if (!pVOAvatar->isDead() && !pVOAvatar->mIsDummy && !pVOAvatar->isOrphaned())
        {
            LLVector3d pos_global = pVOAvatar->getPositionGlobal();
            LLUUID uuid = pVOAvatar->getID();

            if (!uuid.isNull()
                && dist_vec_squared(pos_global, relative_to) <= radius_squared)
            {
                if(positions != NULL)
                {
                    positions->emplace_back(std::move(pos_global));
                }
                if(avatar_ids !=NULL)
                {
                    avatar_ids->emplace_back(std::move(uuid));
                }
            }
        }
    }
    // ids already in avatar_ids, so the region pass dedupes in O(1) per avatar
    // instead of a linear std::find over the output list
    boost::unordered_flat_set<LLUUID> added_ids;
    if (avatar_ids != NULL)
    {
        added_ids.reserve(avatar_ids->size() + 64);
        added_ids.insert(avatar_ids->begin(), avatar_ids->end());
    }
    // region avatars added for situations where radius is greater than RenderFarClip
    for (LLViewerRegion* regionp : getRegionList())
    {
        const LLVector3d& origin_global = regionp->getOriginGlobal();
        size_t count = regionp->mMapAvatars.size();
        for (size_t i = 0; i < count; i++)
        {
            LLVector3d pos_global = unpackLocalToGlobalPosition(regionp->mMapAvatars[i], origin_global);
            if(dist_vec_squared(pos_global, relative_to) <= radius_squared)
            {
                LLUUID uuid = regionp->mMapAvatarIDs[i];
                // if this avatar doesn't already exist in the list, add it
                if(uuid.notNull() && avatar_ids != NULL && added_ids.insert(uuid).second)
                {
                    if (positions != NULL)
                    {
                        positions->emplace_back(std::move(pos_global));
                    }
                    avatar_ids->emplace_back(std::move(uuid));
                }
            }
        }
    }
}

void LLWorld::getAvatars(pos_map_t* umap, const LLVector3d& relative_to, F32 radius) const
{
    F32 radius_squared = radius * radius;

    if (!umap->empty())
    {
        umap->clear();
    }

    // get the list of avatars from the character list first, so distances are correct
    // when agent is above 1020m and other avatars are nearby
    for (auto instance : LLCharacter::sInstances)
    {
        LLVOAvatar* pVOAvatar = static_cast<LLVOAvatar*>(instance);

        if (!pVOAvatar->isDead() && !pVOAvatar->mIsDummy && !pVOAvatar->isOrphaned())
        {
            LLVector3d pos_global = pVOAvatar->getPositionGlobal();
            LLUUID uuid = pVOAvatar->getID();

            if (!uuid.isNull()
                && dist_vec_squared(pos_global, relative_to) <= radius_squared)
            {
                umap->emplace(std::move(uuid), std::move(pos_global));
            }
        }
    }
    // region avatars added for situations where radius is greater than RenderFarClip
    for (LLViewerRegion* regionp : getRegionList())
    {
        const LLVector3d& origin_global = regionp->getOriginGlobal();
        size_t count = regionp->mMapAvatars.size();
        for (size_t i = 0; i < count; i++)
        {
            LLUUID uuid = regionp->mMapAvatarIDs[i];
            if (uuid.isNull()) continue;

            LLVector3d pos_global = unpackLocalToGlobalPosition(regionp->mMapAvatars[i], origin_global);
            if(dist_vec_squared(pos_global, relative_to) <= radius_squared && umap->find(uuid) == umap->end())
            {
                umap->emplace(std::move(uuid), std::move(pos_global));
            }
        }
    }
}

void LLWorld::getAvatars(region_gpos_map_t* umap, const LLVector3d& relative_to, F32 radius) const
{
    F32 radius_squared = radius * radius;

    if (!umap->empty())
    {
        umap->clear();
    }

    // get the list of avatars from the character list first, so distances are correct
    // when agent is above 1020m and other avatars are nearby
    for (auto instance : LLCharacter::sInstances)
    {
        LLVOAvatar* pVOAvatar = static_cast<LLVOAvatar*>(instance);

        if (!pVOAvatar->isDead() && !pVOAvatar->mIsDummy && !pVOAvatar->isOrphaned())
        {
            LLUUID uuid = pVOAvatar->getID();
            auto region = pVOAvatar->getRegion();
            LLVector3d pos_global = pVOAvatar->getPositionGlobal();

            if (uuid.notNull() && region
                && dist_vec_squared(pos_global, relative_to) <= radius_squared)
            {
                umap->emplace(std::move(uuid), std::make_pair(region, std::move(pos_global)));
            }
        }
    }
    // region avatars added for situations where radius is greater than RenderFarClip
    for (LLViewerRegion* regionp : getRegionList())
    {
        const LLVector3d& origin_global = regionp->getOriginGlobal();
        size_t count = regionp->mMapAvatars.size();
        for (size_t i = 0; i < count; ++i)
        {
            LLUUID uuid = regionp->mMapAvatarIDs[i];
            if (uuid.isNull()) continue;

            LLVector3d pos_global = unpackLocalToGlobalPosition(regionp->mMapAvatars[i], origin_global);
            if (dist_vec_squared(pos_global, relative_to) <= radius_squared && umap->find(uuid) == umap->end())
            {
                umap->emplace(std::move(uuid), std::make_pair(regionp, std::move(pos_global)));
            }
        }
    }
}

F32 LLWorld::getNearbyAvatarsAndMaxGPUTime(std::vector<LLVOAvatar*> &valid_nearby_avs)
{
    F32 nearby_max_complexity = 0;
    F32 radius = LLVOAvatar::sRenderDistance * LLVOAvatar::sRenderDistance;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead() && !avatar->isControlAvatar())
        {
            if ((dist_vec_squared(avatar->getPositionGlobal(), gAgent.getPositionGlobal()) <= radius) ||
                (dist_vec_squared(avatar->getPositionGlobal(), gAgentCamera.getCameraPositionGlobal()) <= radius))
            {
                if (!avatar->isTooSlow())
                {
                }
                nearby_max_complexity = llmax(nearby_max_complexity, avatar->getGPURenderTime());
                valid_nearby_avs.push_back(avatar);
            }
        }
    }

    return nearby_max_complexity;
}

// [RLVa:KB] - Checked: RLVa-2.0.1
bool LLWorld::getAvatar(const LLUUID& idAvatar, LLVector3d& posAvatar) const
{
    for (const LLCharacter* pCharacter : LLCharacter::sInstances)
    {
        const LLVOAvatar* pAvatar = static_cast<const LLVOAvatar*>(pCharacter);
        if ( (!pAvatar->isDead()) && (!pAvatar->mIsDummy) && (!pAvatar->isOrphaned()) && (idAvatar == pAvatar->getID()) )
        {
            posAvatar = pAvatar->getPositionGlobal();
            return true;
        }
    }

    for (const LLViewerRegion* pRegion : LLWorld::getInstance()->getRegionList())
    {
        for (size_t idxAgent = 0, cntAgent = pRegion->mMapAvatarIDs.size(); idxAgent < cntAgent; ++idxAgent)
        {
            if (idAvatar == pRegion->mMapAvatarIDs[idxAgent])
            {
                posAvatar = unpackLocalToGlobalPosition(pRegion->mMapAvatars[idxAgent], pRegion->getOriginGlobal());
                return true;
            }
        }
    }

    return false;
}
// [/RLVa:KB]

bool LLWorld::isRegionListed(const LLViewerRegion* region) const
{
    return mRegionSet.find(region) != mRegionSet.end();
}

boost::signals2::connection LLWorld::setRegionRemovedCallback(const region_remove_signal_t::slot_type& cb)
{
    return mRegionRemovedSignal.connect(cb);
}

LLHTTPRegistration<LLEstablishAgentCommunication>
    gHTTPRegistrationEstablishAgentCommunication(
                            "/message/EstablishAgentCommunication");
