/**
 * @file llworld.h
 * @brief Collection of viewer regions in the vacinity of the user.
 *
 * Represents the whole world, so far as 3D functionality is conserned.
 * Always contains the region that the user's avatar is in along with
 * neighboring regions. As the user crosses region boundaries, new
 * regions are added to the world and distant ones are rolled up.
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

#ifndef LL_LLWORLD_H
#define LL_LLWORLD_H

#include "llpatchvertexarray.h"

#include "llmath.h"
#include "v3math.h"
#include "llsingleton.h"
#include "llstring.h"
#include "llviewerpartsim.h"
#include "llviewertexture.h"
#include "llvowater.h"
#include "llhost.h"
#include "lluuid.h"
#include "indra_constants.h"

#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

class LLViewerRegion;
class LLVector3d;
class LLMessageSystem;
class LLNetMap;
class LLHost;

class LLViewerObject;
class LLSurfacePatch;

class LLCharacter;
class LLCloudPuff;
class LLCloudGroup;
class LLVOAvatar;

class CapUrlMatches
{
public:
    CapUrlMatches(std::set<LLViewerRegion*>& regions, std::set<std::string>& cap_names)
        : mRegions(regions)
        , mCapNames(cap_names)
    {
    }

    std::set<LLViewerRegion*> mRegions;
    std::set<std::string> mCapNames;
};

// LLWorld maintains a stack of unused viewer_regions and an array of pointers to viewer regions
// as simulators are connected to, viewer_regions are popped off the stack and connected as required
// as simulators are removed, they are pushed back onto the stack

class LLWorld final : public LLSimpleton<LLWorld>
{
public:
    LLWorld();

    // Clear any objects, regions
    // Prepares class to be reused or destroyed
    void resetClass();

    LLViewerRegion* addRegion(const U64 &region_handle, const LLHost &host);
        // safe to call if already present, does the "right thing" if
        // hosts are same, or if hosts are different, etc...
    void            removeRegion(const LLHost &host);
    // #568 INCREMENTAL REGION TEARDOWN (RegionTeardownBudgetMs > 0). removeRegion delists a
    // region at once (lookups stop returning it: getRegion*, isRegionListed, the region
    // lists, neighbour links) and RETIRES it: the LLViewerRegion stays allocated -- so its
    // objects' region pointers stay valid -- while its objects are killed a time slice per
    // frame, then its object cache is saved, then it is deleted (one step per frame, one
    // region at a time). Main loop, every frame, before the dead-object cleanup.
    void            updateRetiringRegions();
    // Finish retiring regions now (all, or the ones on `host` / with `handle`): a region
    // re-added on the same host must not find the old one's objects in the local-ID table.
    void            flushRetiringRegions(const LLHost* host = nullptr, U64 handle = 0);
    bool            isRetiringRegion(const LLViewerRegion* regionp) const;

    void    disconnectRegions(); // Send quit messages to all child regions

    // ---- Region ring cap (#dd4k) -------------------------------------------------
    // The simulator enables child agents in every neighbour within the agent's draw
    // distance (AgentUpdate Far): at 4096 m on mainland that is ~33x33 regions. The
    // viewer caps what it connects to a square ring of SlipstreamMaxRegionRing regions
    // (default 2 -> 5x5) around the ANCHOR region: the region the agent is in, or the
    // one it is crossing / teleporting into (set before that sim's EnableSimulators
    // arrive). Geometry is in meters from region bounds so var-regions work.
    //
    // The ring is now a BACKSTOP: what the sim enables is driven by the network draw
    // distance (AgentUpdate Far, LLAgentCamera::getNetworkDrawDistance), which is small.
    // Out-of-ring EnableSimulators are refused (no circuit, no UseCircuitCode) and
    // stashed (handle -> host / seed cap) so the region can still be connected later
    // if the ring moves over it, whether or not the sim re-sends EnableSimulator.
    // Accepted regions must be reachable from the agent region through a chain of
    // adjacent connected regions (8-neighbour, as LL's connectNeighbor) and lie within
    // min(network DD + 64 m, render DD) (the agent / anchor 3x3, the "core", always
    // allowed). Connected regions never exceed (2N+1)^2 except for the core, and new
    // non-core circuits are paced (SlipstreamRegionConnectIntervalMs); accepted ones
    // that must wait sit in the same stash. Only DEAD circuits are dropped from our side
    // (no packets for SlipstreamStaleRegionSecs, held during a global stall, at most 2 per
    // sweep; local remove + circuit off, never AgentQuitCopy); live ones leave on the
    // sim's DisableSimulator.

    // Connect to a neighbour simulator (the body of EnableSimulator).
    LLViewerRegion* connectSimulator(const U64& region_handle, const LLHost& host);
    // true when a region rect [origin, origin + width) intersects the current ring.
    bool    isInRegionRing(const U64& region_handle, F32 width = REGION_WIDTH_METERS) const;
    // Refuses (and stashes) an out-of-ring / unreachable / too-far EnableSimulator;
    // false = go ahead.
    bool    refuseOutOfRingSimulator(const U64& region_handle, const LLHost& host);
    // Connects stashed regions the rules now accept (ring move, new neighbour, DD up).
    void    reconnectStashedSims();
    // EstablishAgentCommunication for a refused sim: keep its seed cap. true = consumed.
    bool    stashRefusedSeedCapability(const LLHost& host, const std::string& seed);
    // DisableSimulator for a host we already refused / dropped (quiet; true = handled).
    bool    forgetRefusedOrDroppedHost(const LLHost& host);
    // The region the ring is centred on (crossing / teleport target, then agent region).
    void    setRegionRingAnchor(const U64& region_handle);
    // #850 CrossedRegion: makes sure the destination has a region and a circuit before
    // CompleteAgentMovement goes out (connectSimulator, i.e. UseCircuitCode, as the
    // teleport path does). Pre-connected is the normal case; anything else is logged.
    // seed_cap: the CrossedRegion seed; filled from the stash if empty. action: a short
    // description for the Crossing568 block.
    LLViewerRegion* ensureCrossingDestination(const U64& region_handle, const LLHost& host,
                                              std::string& seed_cap, std::string& action);
    // Per-frame: anchor sanity, stash reconnect / prune, deferred out-of-ring drop.
    void    updateRegionRing();

    LLViewerRegion*         getRegion(const LLHost &host);
    LLViewerRegion*         getRegionFromPosGlobal(const LLVector3d &pos);
    LLViewerRegion*         getRegionFromPosAgent(const LLVector3 &pos);
    LLViewerRegion*         getRegionFromHandle(const U64 &handle);
    LLViewerRegion*         getRegionFromID(const LLUUID& region_id);
    bool                    positionRegionValidGlobal(const LLVector3d& pos);           // true if position is in valid region
    LLVector3d              clipToVisibleRegions(const LLVector3d &start_pos, const LLVector3d &end_pos);

    void                    updateAgentOffset(const LLVector3d &offset);

    // All of these should be in the agent coordinate frame
    LLViewerRegion*         resolveRegionGlobal(LLVector3 &localpos, const LLVector3d &position);
    LLViewerRegion*         resolveRegionAgent(LLVector3 &localpos, const LLVector3 &position);
    F32                     resolveLandHeightGlobal(const LLVector3d &position);
    F32                     resolveLandHeightAgent(const LLVector3 &position);

    // Return the lowest allowed Z point to prevent objects from being moved
    // underground.
    F32 getMinAllowedZ(LLViewerObject* object, const LLVector3d &global_pos);

    // takes a line segment defined by point_a and point_b, then
    // determines the closest (to point_a) point of intersection that is
    // on the land surface or on an object of the world.
    // Stores results in "intersection" and "intersection_normal" and
    // returns a scalar value that is the normalized (by length of line segment)
    // distance along the line from "point_a" to "intersection".
    //
    // Currently assumes point_a and point_b only differ in z-direction,
    // but it may eventually become more general.
    F32 resolveStepHeightGlobal(const LLVOAvatar* avatarp, const LLVector3d &point_a, const LLVector3d &point_b,
                            LLVector3d &intersection, LLVector3 &intersection_normal,
                            LLViewerObject** viewerObjectPtr=NULL);

    LLSurfacePatch *        resolveLandPatchGlobal(const LLVector3d &position);
    LLVector3               resolveLandNormalGlobal(const LLVector3d &position);        // absolute frame

    U32                     getRegionWidthInPoints() const  { return mWidth; }
    F32                     getRegionScale() const          { return mScale; }

    // region X and Y size in meters
    F32                     getRegionWidthInMeters() const  { return mWidthInMeters; }
    F32                     getRegionMinHeight() const      { return -mWidthInMeters; }
    F32                     getRegionMaxHeight() const      { return MAX_OBJECT_Z; }

    void                    updateRegions(F32 max_update_time);
    void                    updateVisibilities();
    void                    updateParticles();

    void                    renderPropertyLines();

    void updateNetStats(); // Update network statistics for all the regions...

    void printPacketsLost();
    void requestCacheMisses();

    // deal with map object updates in the world.
    static void processCoarseUpdate(LLMessageSystem* msg, void** user_data);

    F32 getLandFarClip() const;
    void setLandFarClip(const F32 far_clip);

    LLViewerTexture *getDefaultWaterTexture();
    void updateWaterObjects();

    void waterHeightRegionInfo(std::string const& sim_name, F32 water_height);
    void shiftRegions(const LLVector3& offset);

    void setSpaceTimeUSec(const U64MicrosecondsImplicit space_time_usec);
    U64MicrosecondsImplicit getSpaceTimeUSec() const;

    void getInfo(LLSD& info);
    U32  getNumOfActiveCachedObjects() const {return mNumOfActiveCachedObjects;}

    void clearAllVisibleObjects();

    virtual CapUrlMatches getCapURLMatches(const std::string& cap_url);
    virtual bool isCapURLMapped(const std::string& cap_url);

public:
    typedef std::list<LLViewerRegion*> region_list_t;
    const region_list_t& getRegionList() const { return mActiveRegionList; }

    typedef boost::signals2::signal<void(LLViewerRegion*)> region_remove_signal_t;
    boost::signals2::connection setRegionRemovedCallback(const region_remove_signal_t::slot_type& cb);

    // Returns lists of avatar IDs and their world-space positions within a given distance of a point.
    // All arguments are optional. Given containers will be emptied and then filled.
    // Not supplying origin or radius input returns data on all avatars in the known regions.
    void getAvatars(
        uuid_vec_t* avatar_ids = nullptr,
        std::vector<LLVector3d>* positions = nullptr,
        const LLVector3d& relative_to = LLVector3d(), F32 radius = FLT_MAX) const;

    typedef std::map<LLUUID, LLVector3d> pos_map_t;
    void getAvatars(pos_map_t* map = nullptr,
                    const LLVector3d& relative_to = LLVector3d(),
                    F32 radius = FLT_MAX) const;

    // Returns list of avatar ids with region pointer and global position
    typedef std::pair<LLViewerRegion*, LLVector3d > regionp_gpos_pair_t;
    typedef std::map<LLUUID, regionp_gpos_pair_t > region_gpos_map_t;
    void getAvatars(region_gpos_map_t* map = nullptr,
        const LLVector3d& relative_to = LLVector3d(),
        F32 radius = FLT_MAX) const;

// [RLVa:KB] - Checked: RLVa-2.0.1
    bool getAvatar(const LLUUID& idAvatar, LLVector3d& posAvatar) const;
// [/RLVa:KB]

    // Returns 'true' if the region is in mRegionList,
    // 'false' if the region has been removed due to region change
    // or if the circuit to this simulator had been lost.
    bool isRegionListed(const LLViewerRegion* region) const;

    // profile nearby avatars using gPipeline.profileAvatar and update their render times
    // return max GPU time
    F32 getNearbyAvatarsAndMaxGPUTime(std::vector<LLVOAvatar*> &valid_nearby_avs);

public:
    // #568: an origin shift moves hole-water positions, so the next updateWaterObjects() must rebuild
    void invalidateHoleWaterCache() { mHoleWaterKeyValid = false; }
private:
    void clearHoleWaterObjects();
    void clearEdgeWaterObjects();

    region_list_t   mActiveRegionList;
    region_list_t   mRegionList;
    // O(1) indexes over mRegionList, maintained in addRegion/removeRegion
    std::unordered_set<const LLViewerRegion*> mRegionSet;
    std::unordered_map<U64, LLViewerRegion*> mRegionByHandle;
    std::unordered_map<LLHost, LLViewerRegion*, LLHostHash> mRegionByHost;
    region_list_t   mVisibleRegionList;
    region_list_t   mCulledRegionList;

    region_remove_signal_t mRegionRemovedSignal;

    // #568 retiring regions (removeRegion -> updateRetiringRegions), oldest first. Bounded
    // by the regions removed in the last few seconds (each retires in a few frames).
    struct RetiringRegion
    {
        LLViewerRegion* mRegion = nullptr;
        std::vector<LLUUID> mObjects;   // its objects at retirement (looked up again when killed)
        size_t mCursor = 0;
        U32    mStage = 0;          // 0 kill objects, 1 save cache, 2 delete
        U32    mKilled = 0;
        U32    mFrames = 0;
        F64    mStartSecs = 0.0;
        F64    mWorkMs = 0.0;       // main-thread ms spent on it so far
        F64    mWorstMs = 0.0;      // largest single step
    };
    std::deque<RetiringRegion> mRetiringRegions;
    void retireStep(RetiringRegion& r, F64 budget_s);   // one step; mStage == 3 when deleted

    // Number of points on edge
    static const U32 mWidth;

    // meters/point, therefore mWidth * mScale = meters per edge
    static const F32 mScale;

    static const F32 mWidthInMeters;

    F32 mLandFarClip;                   // Far clip distance for land.
    LLPatchVertexArray      mLandPatch;
    S32 mLastPacketsIn;
    S32 mLastPacketsOut;
    S32 mLastPacketsLost;
    U32 mNumOfActiveCachedObjects;
    U64MicrosecondsImplicit mSpaceTimeUSec;

    ////////////////////////////
    //
    // Data for "Fake" objects
    //

    std::list<LLPointer<LLVOWater> > mHoleWaterObjects;
    // #568: inputs the hole-water set was last built from; rebuild skipped when unchanged
    std::vector<U64> mHoleWaterKeyHandles;
    const LLViewerRegion* mHoleWaterKeyRegion = nullptr;
    F32 mHoleWaterKeyHeight = 0.f;
    bool mHoleWaterKeyValid = false;
    static const S32 EDGE_WATER_OBJECTS_COUNT = 8;
    LLPointer<LLVOWater> mEdgeWaterObjects[EDGE_WATER_OBJECTS_COUNT];

    LLPointer<LLViewerTexture> mDefaultWaterTexturep;

    // Region ring cap (#dd4k), see refuseOutOfRingSimulator().
    struct RefusedSim
    {
        LLHost      mHost;
        std::string mSeedCap;
        F64         mLastSeen = 0.0;    // LLFrameTimer::getTotalSeconds()
    };
    struct RingEval;
    struct RingRegionInfo;
    void        buildRingEval(RingEval& eval, U64 exclude_handle) const;
    const char* ringRefusal(const RingEval& eval, U64 region_handle, F32 width) const;  // nullptr = accept
    // Agent / anchor region or one touching them: exempt from the region cap and pacing.
    bool        isRingCore(const RingEval& eval, U64 region_handle, F32 width) const;
    void        dropRegion(LLViewerRegion* regionp, const char* reason, F64 now);
    void        logRegionRingState(const std::vector<RingRegionInfo>& infos, F64 now);

    std::map<U64, RefusedSim>   mRefusedSims;           // handle -> stashed enable
    std::map<U64, std::pair<LLHost, F64> > mDroppedSims;  // handle -> host, drop time
    std::map<U64, F64> mRecentRoot;     // handle -> last sweep the agent was root there (no drop for RING_RECENT_ROOT_SECS)
    U64     mRingAnchor = 0;
    F64     mRingChangedTime = 0.0;     // last anchor / ring-size change
    F64     mRingLastSweep = 0.0;
    U32     mRingSize = 2;              // last applied SlipstreamMaxRegionRing
    bool    mRingWasBusy = true;        // last sweep saw a teleport / login in progress
    U32     mRingRefusedMsgs = 0;       // EnableSimulators refused (incl. re-sends)
    U32     mRingRefusedRegions = 0;    // distinct regions refused (first refusal)
    U32     mRingDropped = 0;           // connected regions dropped on ring moves
    U32     mRingLateConnected = 0;     // stashed regions connected when the ring moved
    bool    mRingStashDirty = false;    // a region connected since the last stash pass
    bool    mInStashReconnect = false;
    bool    mRingStashIdle = false;     // last full stash scan found nothing to connect
    std::string mRingStateSig;          // last logged region set
    F64     mRingStateLogTime = 0.0;
    U32     mRingDeferredMsgs = 0;      // accepted EnableSimulators stashed for the cap / pacer
    F64     mRingLastConnect = 0.0;     // last new non-core circuit (SlipstreamRegionConnectIntervalMs)
    F64     mRingStallSince = 0.0;     // global stall (root / most regions silent) start, 0 = none
    F64     mRingStallLast = 0.0;       // last sweep that saw a global stall (drops held after it)
    F64     mRingStallLogTime = 0.0;
};


void process_enable_simulator(LLMessageSystem *mesgsys, void **user_data);
void process_disable_simulator(LLMessageSystem *mesgsys, void **user_data);

void process_region_handshake(LLMessageSystem* msg, void** user_data);

void send_agent_pause();
void send_agent_resume();

#endif
