/**
 * @file llviewerobjectlist.h
 * @brief Description of LLViewerObjectList class.
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

#ifndef LL_LLVIEWEROBJECTLIST_H
#define LL_LLVIEWEROBJECTLIST_H

#include <map>
#include <deque>
#include <set>

// common includes
#include "llhost.h"
#include "llstring.h"
#include "lltrace.h"

// project includes
#include "llviewerobject.h"
#include "lleventcoro.h"
#include "llcoros.h"

class LLCamera;
class LLNetMap;
class LLDebugBeacon;
class LLVOCacheEntry;

const U32 CLOSE_BIN_SIZE = 10;
const U32 NUM_BINS = 128;

class LLViewerObjectList
{
public:
    LLViewerObjectList();
    ~LLViewerObjectList();

    void destroy();

    // For internal use only.  Does NOT take a local id, takes an index into
    // an internal dynamic array.
    inline LLViewerObject *getObject(const S32 index);

    inline LLViewerObject *findObject(const LLUUID &id);
    LLViewerObject *createObjectViewer(const LLPCode pcode, LLViewerRegion *regionp, S32 flags = 0); // Create a viewer-side object
    LLViewerObject *createObjectFromCache(const LLPCode pcode, LLViewerRegion *regionp, const LLUUID &uuid, const U32 local_id);
    LLViewerObject *createObject(const LLPCode pcode, LLViewerRegion *regionp,
                                 const LLUUID &uuid, const U32 local_id, const LLHost &sender);

    LLViewerObject *replaceObject(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp); // TomY: hack to switch VO instances on the fly

    bool killObject(LLViewerObject *objectp);
    void killObjects(LLViewerRegion *regionp); // Kill all objects owned by a particular region.
    // #568 INCREMENTAL REGION TEARDOWN (LLWorld::updateRetiringRegions). collect: the
    // region's objects (one walk, at retirement; also despawns its local mesh previews).
    // killSliced: kill those still owned by the region from `cursor` on, within budget_s
    // (at least one); returns true when the list is done. Killed objects are compacted by
    // the per-frame cleanDeadObjects, as any other kill.
    void collectRegionObjects(LLViewerRegion* regionp, std::vector<LLUUID>& out);
    bool killRegionObjectsSliced(LLViewerRegion* regionp, const std::vector<LLUUID>& objects,
                                 size_t& cursor, F64 budget_s, U32& killed);
    void killAllObjects();

    // Clean up the dead object list. Compaction out of mObjects is immediate; the
    // destructors (the last LLPointer release) run from a backlog under a per-frame budget
    // (SLObjectDestroyBudgetMs) when use_timer, else all of them now. See drainPendingDestroy.
    void cleanDeadObjects(const bool use_timer = true);
    // Run the deferred destructors: within budget_ms (at least one object), or all of
    // them when budget_ms <= 0. regionp: only the objects of that region (a region being
    // deleted), all of them now.
    void drainPendingDestroy(F32 budget_ms, LLViewerRegion* regionp = nullptr);
    U32 getPendingDestroyCount() const { return (U32)mPendingDestroy.size(); }

    // Simulator and viewer side object updates...
    void processUpdateCore(LLViewerObject* objectp, void** data, U32 block, const EObjectUpdateType update_type,
                           LLDataPacker* dpp, bool justCreated, bool from_cache = false);
    LLViewerObject* processObjectUpdateFromCache(LLVOCacheEntry* entry, LLViewerRegion* regionp);
    void processObjectUpdate(LLMessageSystem *mesgsys, void **user_data, EObjectUpdateType update_type, bool compressed=false);
    void processCompressedObjectUpdate(LLMessageSystem *mesgsys, void **user_data, EObjectUpdateType update_type);
    void processCachedObjectUpdate(LLMessageSystem *mesgsys, void **user_data, EObjectUpdateType update_type);
    void updateApparentAngles(LLAgent &agent);
    void update(LLAgent &agent);

    void fetchObjectCosts();
    void fetchPhysicsFlags();

    void updateObjectCost(LLViewerObject* object);
    void updateObjectCost(const LLUUID& object_id, F32 object_cost, F32 link_cost, F32 physics_cost, F32 link_physics_cost);
    void onObjectCostFetchFailure(const LLUUID& object_id);

    void updatePhysicsFlags(const LLViewerObject* object);
    void onPhysicsFlagsFetchFailure(const LLUUID& object_id);
    void updatePhysicsShapeType(const LLUUID& object_id, S32 type);
    void updatePhysicsProperties(const LLUUID& object_id,
                                    F32 density,
                                    F32 friction,
                                    F32 restitution,
                                    F32 gravity_multiplier);

    void shiftObjects(const LLVector3 &offset);
    void repartitionObjects();

    bool hasMapObjectInRegion(LLViewerRegion* regionp) ;
    void clearAllMapObjectsInRegion(LLViewerRegion* regionp) ;
    // Plots map objects [start, start + max_count) into the net map's object layer;
    // returns the next index (== getMapObjectCount() when the pass is complete).
    size_t renderObjectsForMap(LLNetMap &netmap, size_t start = 0, size_t max_count = ~size_t(0));
    size_t getMapObjectCount() const { return mMapObjects.size(); }
    void renderObjectBounds(const LLVector3 &center);

    void addDebugBeacon(const LLVector3 &pos_agent, const std::string &string,
                        const LLColor4 &color=LLColor4(1.f, 0.f, 0.f, 0.5f),
                        const LLColor4 &text_color=LLColor4(1.f, 1.f, 1.f, 1.f),
                        S32 line_width = 1);
    // Object-tracking beacon: the position is resolved from the object's CURRENT
    // render pose when the beacon is drawn (follows moving objects); the beacon is
    // dropped if the object is gone (dead / out of range) by then.
    void addDebugBeacon(const LLViewerObject* objectp, const std::string &string,
                        const LLColor4 &color=LLColor4(1.f, 0.f, 0.f, 0.5f),
                        const LLColor4 &text_color=LLColor4(1.f, 1.f, 1.f, 1.f),
                        S32 line_width = 1);
    // Agent-space position the VK world renderer draws objectp at (eased pose for
    // movers, attach point for worn attachments). False for HUD attachments and
    // dead objects.
    static bool getBeaconPositionAgent(LLViewerObject* objectp, LLVector3& pos_agent);
    // VK world-overlay pass: beams for every queued beacon (lines batched per width).
    void renderObjectBeacons();
    // VK HUD-text pass: beacon labels (nearest on-screen ones, capped).
    void renderObjectBeaconLabels();
    // Once per frame, after both passes above: beacons live for exactly one draw.
    void resetObjectBeacons();

    void dirtyAllObjectInventory();
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    void setAllObjectDefaultTextures(U32 nChannel, bool fShowDefault);
// [/SL:KB]

    void removeFromActiveList(LLViewerObject* objectp);
    void updateActive(LLViewerObject *objectp);

    void updateAvatarVisibility();

    inline S32 getNumObjects() { return (S32) mObjects.size(); }
    inline S32 getNumActiveObjects() { return (S32) mActiveObjects.size(); }
    // <GMDX> the objects that move / update each frame (the mini-map's physical-objects pass)
    const std::vector<LLPointer<LLViewerObject> >& getActiveObjects() const { return mActiveObjects; }

    void addToMap(LLViewerObject *objectp);
    void removeFromMap(LLViewerObject *objectp);

    void clearDebugText();

    ////////////////////////////////////////////
    //
    // Only accessed by markDead in LLViewerObject
    void cleanupReferences(LLViewerObject *objectp);

    S32 findReferences(LLDrawable *drawablep) const; // Find references to drawable in all objects, and return value.

    S32 getOrphanParentCount() const { return (S32) mOrphanChildren.size(); }
    S32 getOrphanCount() const { return mNumOrphans; }
    S32 getAvatarCount() const { return mNumAvatars; }
    void orphanize(LLViewerObject *childp, U32 parent_id, U32 ip, U32 port);
    void findOrphans(LLViewerObject* objectp, U32 ip, U32 port);

public:
    // Class for keeping track of orphaned objects
    class OrphanInfo
    {
    public:
        OrphanInfo();
        OrphanInfo(const U64 parent_info, const LLUUID child_info);
        bool operator==(const OrphanInfo &rhs) const;
        bool operator!=(const OrphanInfo &rhs) const;
        U64 mParentInfo;
        LLUUID mChildInfo;
    };


    U32 mCurBin; // Current bin we're working on...

    // Statistics data (see also LLViewerStats)
    S32 mNumNewObjects;

    // if we paused in the last frame
    // used to discount stats from this frame
    bool mWasPaused;

    void getUUIDFromLocal(LLUUID &id,
                                const U32 local_id,
                                const U32 ip,
                                const U32 port);
    void setUUIDAndLocal(const LLUUID &id,
                                const U32 local_id,
                                const U32 ip,
                                const U32 port,
                                LLViewerObject* objectp); // Requires knowledge of message system info!

    bool removeFromLocalIDTable(LLViewerObject* objectp);
    // Used ONLY by the orphaned object code.
    U64 getIndex(const U32 local_id, const U32 ip, const U32 port);

    S32 mNumUnknownUpdates;
    S32 mNumDeadObjectUpdates;
    S32 mNumDeadObjects;
protected:
    // Orphaned children keyed by parent (LocalID/ip,port index); purged when the parent arrives or its region dies
    boost::unordered_map<U64, std::vector<LLUUID> > mOrphanChildren;
    void purgeOrphansForRegion(const LLHost& host);
    S32 mNumOrphans;
    S32 mNumAvatars;

    typedef std::vector<LLPointer<LLViewerObject> > vobj_list_t;

    vobj_list_t mObjects;
    std::vector<LLPointer<LLViewerObject> > mActiveObjects;

    vobj_list_t mMapObjects;

    uuid_set_t   mDeadObjects;

    // DEFERRED DESTRUCTION: dead objects already compacted out of mObjects (markDead has
    // unhooked them from every lookup, the instance store and the pipeline) waiting for
    // their destructor. One avatar's departure used to destroy the avatar and all its
    // attachments inside one cleanDeadObjects call (35+ ms each, stacking to 100-300 ms).
    struct PendingDestroy
    {
        LLPointer<LLViewerObject> mObject;
        F64 mQueuedAt = 0.0;
    };
    std::deque<PendingDestroy> mPendingDestroy;
    struct DestroyStats
    {
        U32 mPeak = 0;              // backlog high-water since the last stat line
        U32 mDestroyed = 0;         // destructors run since the last stat line
        U32 mDeferred = 0;          // objects queued since the last stat line
        F32 mMaxDrainMs = 0.f;      // slowest per-frame drain
        F32 mWorstMs = 0.f;         // slowest single destructor
        std::string mWorstWhat;     // and what it was
        F64 mLastLog = 0.0;
    } mDestroyStats;
    void destroyPendingAt(size_t idx);
    void logDestroyStats(F64 now);

    boost::unordered_map<LLUUID, LLPointer<LLViewerObject> > mUUIDObjectMap;

    //set of objects that need to update their cost
    uuid_set_t   mStaleObjectCost;
    uuid_set_t   mPendingObjectCost;
    // When each object's cost was last asked for: an object a script updates several times a second marks its
    // cost stale on every full update, which re-asked GetObjectCost back to back while it was selected.
    // Ids asked within OBJECT_COST_MIN_INTERVAL stay queued in mStaleObjectCost until it passes (none are dropped).
    std::unordered_map<LLUUID, F64> mObjectCostAskedAt;
    F64          mObjectCostAskedPrune = 0.0;
    // Objects whose cost didn't arrive (request failed, id missing from the reply, region/capability not
    // ready): re-asked with exponential backoff while still selected, since the build floater only
    // re-reads a cost when something dirties it.
    struct ObjectCostRetry { F64 mNext = 0.0; S32 mAttempts = 0; };
    std::unordered_map<LLUUID, ObjectCostRetry> mObjectCostRetry;
    void scheduleObjectCostRetry(const LLUUID& object_id);

    //set of objects that need to update their physics flags
    uuid_set_t   mStalePhysicsFlags;
    uuid_set_t   mPendingPhysicsFlags;

    std::vector<LLDebugBeacon> mDebugBeacons;

    S32 mCurLazyUpdateIndex;

    static U32 sSimulatorMachineIndex;
    boost::unordered_map<U64, U32> mIPAndPortToIndex;

    boost::unordered_map<U64, LLUUID> mIndexAndLocalIDToUUID;

    friend class LLViewerObject;

private:
    static void reportObjectCostFailure(LLSD &objectList);
    void fetchObjectCostsCoro(std::string url, LLSD idList, std::string region_name);

    static void reportPhysicsFlagFailure(LLSD &obejectList);
    void fetchPhisicsFlagsCoro(std::string url, LLSD idList);

};


class LLDebugBeacon
{
public:
    ~LLDebugBeacon();

    LLVector3 mPositionAgent;
    LLUUID mObjectID;           // non-null: position tracks this object each draw
    std::string mString;
    LLColor4 mColor;
    LLColor4 mTextColor;
    S32 mLineWidth;
};



// Global object list
extern LLViewerObjectList gObjectList;

// Inlines
/**
 * Note:
 * it will return NULL for offline avatar_id
 */
inline LLViewerObject *LLViewerObjectList::findObject(const LLUUID &id)
{
    if (id.isNull())
        return NULL;

    auto iter = mUUIDObjectMap.find(id);
    if (iter != mUUIDObjectMap.end())
    {
        return iter->second;
    }

    return NULL;
}

inline LLViewerObject *LLViewerObjectList::getObject(const S32 index)
{
    LLViewerObject *objectp;
    objectp = mObjects[index];
    if (objectp->isDead())
    {
        //LL_WARNS() << "Dead object " << objectp->mID << " in getObject" << LL_ENDL;
        return NULL;
    }
    return objectp;
}

inline void LLViewerObjectList::addToMap(LLViewerObject *objectp)
{
    objectp->setMapListIndex((S32)mMapObjects.size());
    mMapObjects.push_back(objectp);
}

inline void LLViewerObjectList::removeFromMap(LLViewerObject *objectp)
{
    // Stored index + swap-and-pop (as removeFromActiveList does) instead of a
    // linear find and a vector erase per removed object. mMapObjects is unordered
    // (arrival order); the minimap's rolling pass tolerates removal either way.
    S32 idx = objectp->getMapListIndex();
    if (idx < 0)
    {
        return; // not in the list (every insert stores its index)
    }
    objectp->setMapListIndex(-1);
    S32 size = (S32)mMapObjects.size();
    if (idx >= size || mMapObjects[idx] != objectp)
    {
        // Stale index (should not happen): fall back to a search.
        std::vector<LLPointer<LLViewerObject> >::iterator iter = std::find(mMapObjects.begin(), mMapObjects.end(), objectp);
        if (iter == mMapObjects.end())
        {
            return;
        }
        idx = (S32)(iter - mMapObjects.begin());
    }

    S32 last_index = size - 1;
    if (idx < last_index)
    {
        mMapObjects[idx] = std::move(mMapObjects[last_index]);
        mMapObjects[idx]->setMapListIndex(idx);
    }
    mMapObjects.pop_back();
}


#endif // LL_VIEWER_OBJECT_LIST_H
