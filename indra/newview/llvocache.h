/**
 * @file llvocache.h
 * @brief Cache of objects on the viewer.
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
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

#ifndef LL_LLVOCACHE_H
#define LL_LLVOCACHE_H

#include "lluuid.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llspatialentry.h"
#include "llviewercamera.h"
#include "llapr.h"
#include "llgltfmaterial.h"
#include "llvocachegrid.h"
#include "threadpool_fwd.h"

#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <unordered_map>

//---------------------------------------------------------------------------
// Cache entries
class LLCamera;
class LLViewerRegion;
struct LLVOCacheReadResult;   // (#312) llvocache.cpp

class LLGLTFOverrideCacheEntry
{
public:
    static const std::string VERSION_LABEL;
    static const int VERSION;
    bool fromLLSD(const LLSD& data);
    LLSD toLLSD() const;

    LLUUID mObjectId;
    U32    mLocalId = 0;
    boost::unordered_map<S32, LLSD> mSides; //override LLSD per side
    boost::unordered_map<S32, LLPointer<LLGLTFMaterial> > mGLTFMaterial; //GLTF material per side
    U64 mRegionHandle = 0;
};

class LLVOCacheEntry
:   public LLSpatialEntryData
{
    LL_ALIGN_NEW
public:
    enum
    {
        //low 16-bit state
        INACTIVE = 0x00000000,     //not visible
        IN_QUEUE = 0x00000001,     //in visible queue, object to be created
        WAITING  = 0x00000002,     //object creation request sent
        ACTIVE   = 0x00000004,      //object created, and in rendering pipeline.

        //high 16-bit state
        IN_VO_TREE = 0x00010000,    //the entry is in the object cache tree.

        LOW_BITS  = 0x0000ffff,
        HIGH_BITS = 0xffff0000
    };

    struct CompareVOCacheEntry
    {
        bool operator()(const LLVOCacheEntry* const& lhs, const LLVOCacheEntry* const& rhs) const
        {
            F32 lpa = lhs->getSceneContribution();
            F32 rpa = rhs->getSceneContribution();
            // A NaN breaks strict weak ordering, which std::sort (#321) does not survive.
            if (lpa != lpa) lpa = -1.f;
            if (rpa != rpa) rpa = -1.f;

            //larger pixel area first
            if(lpa > rpa)
            {
                return true;
            }
            else if(lpa < rpa)
            {
                return false;
            }
            else
            {
                return lhs < rhs;
            }
        }
    };

protected:
    ~LLVOCacheEntry();
public:
    LLVOCacheEntry(U32 local_id, U32 crc, LLDataPackerBinaryBuffer &dp);
    LLVOCacheEntry(LLAPRFile* apr_file);
    LLVOCacheEntry();

    void updateEntry(U32 crc, LLDataPackerBinaryBuffer &dp);

    void clearState(U32 state) {mState &= ~state;}
    bool hasState(U32 state)   {return mState & state;}
    void setState(U32 state);
    bool isState(U32 state)    {return (mState & LOW_BITS) == state;}
    U32  getState() const      {return mState & LOW_BITS;}

    U32 getLocalID() const          { return mLocalID; }
    U32 getCRC() const              { return mCRC; }
    S32 getHitCount() const         { return mHitCount; }
    S32 getCRCChangeCount() const   { return mCRCChangeCount; }

    void calcSceneContribution(const LLVector4a& camera_origin, bool needs_update, U32 last_update, F32 dist_threshold);
    // calcSceneContribution's value from origin (region space), without storing it or
    // touching the visibility stamp (look-ahead priority, metrics).
    F32  sceneContributionFrom(const LLVector4a& origin, F32 max_dist) const;
    void setSceneContribution(F32 scene_contrib) {mSceneContrib = scene_contrib;}
    F32 getSceneContribution() const             { return mSceneContrib;}

    void dump() const;
    S32 writeToBuffer(U8 *data_buffer) const;
    LLDataPackerBinaryBuffer *getDP() const;
    void recordHit();
    void recordDupe() { mDupeCount++; }

    /*virtual*/ void setSpatialEntry(LLSpatialEntry* entry);

    void setParentID(U32 id);
    U32  getParentID() const {return mParentID;}
    bool isChild() const {return mParentID > 0;}

    void addChild(LLVOCacheEntry* entry);
    void removeChild(LLVOCacheEntry* entry);
    void removeAllChildren();
    LLVOCacheEntry* getChild(); //remove the first child, and return it.
    // #533: membership test / removal that, unlike removeChild, keeps the child's parent id.
    bool hasChild(LLVOCacheEntry* entry) const { return mChildrenList.find(entry) != mChildrenList.end(); }
    void eraseChild(LLVOCacheEntry* entry) { mChildrenList.erase(entry); }
    S32  getNumOfChildren() const { return static_cast<S32>(mChildrenList.size()); }

    void setBoundingInfo(const LLVector3& pos, const LLVector3& scale); //called from processing object update message
    void updateParentBoundingInfo();
    void saveBoundingSphere();

    void setValid(bool valid = true) {mValid = valid;}
    bool isValid() const {return mValid;}

    void setUpdateFlags(U32 flags) {mUpdateFlags = flags;}
    U32  getUpdateFlags() const    {return mUpdateFlags;}

    static void updateDebugSettings();
    static F32  getSquaredPixelThreshold(bool is_front);

private:
    void updateParentBoundingInfo(const LLVOCacheEntry* child);

public:
    typedef std::unordered_map<U32, LLPointer<LLVOCacheEntry> > vocache_entry_map_t;
    typedef std::set<LLVOCacheEntry*>                      vocache_entry_set_t;

    typedef boost::unordered_map<U32, LLGLTFOverrideCacheEntry>  vocache_gltf_overrides_map_t;

// [SL:KB] - Patch: World-Derender | Checked: 2014-08-10 (Catznip-3.7)
    vocache_entry_set_t::const_iterator getChildrenBegin() const { return mChildrenList.begin(); }
    vocache_entry_set_t::const_iterator getChildrenEnd() const { return mChildrenList.end(); }
// [/SL:KB]

    S32                         mLastCameraUpdated;

    // VRAM soft ladder, far geometry (#405 step 6): LLViewerRegion::killCachedObjectsBeyond
    // returned this root to the cache. The cache's own reload test (calcSceneContribution:
    // the linkset bbox centre within draw distance + the CACHE bin radius, 4x the linkset
    // half-size, up to 256 m) is far looser than the kill test (the root drawable's position
    // + its own bin radius + margin), so a killed linkset was usually re-created on the next
    // visible-group pass and killed again (kill / reload churn). While held, the entry
    // reloads only by the kill test's own metric minus the margin (hysteresis): the camera
    // came within draw distance + mVramHoldRadius of mVramHoldPos (region space), or the
    // effective draw distance is back at the setting.
    void setVramHold(const LLVector3& pos_region, F32 radius, F64 now)
    {
        mVramHoldPos = pos_region; mVramHoldRadius = radius; mVramKillTime = now;
    }
    bool isVramHeld() const { return mVramHoldRadius >= 0.f; }
    // True (and the hold cleared) once the camera / distance would no longer kill it.
    bool releaseVramHold(const LLVector3& camera_region, F32 draw_distance, bool distance_restored)
    {
        if (!isVramHeld()) return true;
        const F32 reach = draw_distance + mVramHoldRadius;
        if (!distance_restored && (mVramHoldPos - camera_region).lengthSquared() > reach * reach) return false;
        mVramHoldRadius = -1.f;
        return true;
    }
    // Wall time of the last far-geometry kill (0 = never), for the re-ingest count.
    F64  takeVramKillTime() { const F64 t = mVramKillTime; mVramKillTime = 0.0; return t; }

    // #479 object-cache grid (LLVOCacheGrid) bookkeeping. mGrid is set while the entry
    // sits in a region's grid (IN_VO_TREE); the grid owns these fields.
    LLVOCacheGrid*              mGrid = nullptr;
    S32                         mGridCell = -1;
    U32                         mGridIndex = 0;
    U32                         mGridListGen = 0;       // de-dup in the region's selected-entry list
    U32                         mSelectedFrame = 0;     // last frame a cache cull selected it (LLViewerRegion::addGridVisibleEntry)
    F64                         mEligibleSince = 0.0;   // metrics: first sampled on screen + above threshold (0 = not)

protected:
    LLVector3                   mVramHoldPos;
    F32                         mVramHoldRadius = -1.f;   // < 0: not held
    F64                         mVramKillTime = 0.0;
    U32                         mLocalID;
    U32                         mParentID;
    U32                         mCRC;
    U32                         mUpdateFlags; //receive from sim
    S32                         mHitCount;
    S32                         mDupeCount;
    S32                         mCRCChangeCount;
    mutable LLDataPackerBinaryBuffer    mDP;
    U8                          *mBuffer;

    F32                         mSceneContrib; //projected scene contributuion of this object.
    U32                         mState; //high 16 bits reserved for special use.
    vocache_entry_set_t         mChildrenList; //children entries in a linked set.

    bool                        mValid; //if set, this entry is valid, otherwise it is invalid and will be removed.

    LLVector4a                  mBSphereCenter; //bounding sphere center
    F32                         mBSphereRadius; //bounding sphere radius

public:
    static U32                  sMinFrameRange;
    static F32                  sNearRadius;
    static F32                  sRearFarRadius;
    static F32                  sFrontPixelThreshold;
    static F32                  sRearPixelThreshold;
};

// The region's object-cache selection (#479, #480): owns the flat grid of the region's
// cached ROOT entries and decides, once per frame, which of them the next idle pass
// considers for instantiation (front: the culling camera's frustum; rear: a sphere
// around it). The VOCache octree it used to walk is gone (#480); the grid is the only path.
class LLVOCachePartition
{
public:
    LLVOCachePartition(LLViewerRegion* regionp);
    ~LLVOCachePartition();

    LLVOCachePartition(const LLVOCachePartition&) = delete;
    LLVOCachePartition& operator=(const LLVOCachePartition&) = delete;

    bool addEntry(LLVOCacheEntry* entry);
    void removeEntry(LLVOCacheEntry* entry);
    S32  cull(LLCamera &camera);

    void setCullHistory(bool has_new_object);

    bool isFrontCull() const {return mFrontCull;}

    LLVOCacheGrid& getGrid() { return mGrid; }
    // The front cull ran on this frame (not skipped by the static-camera throttle).
    bool frontCulledOnFrame(U32 frame) const { return mFrontCullFrame == frame; }

private:
    void selectBackObjects(LLCamera &camera, F32 projection_area_cutoff); //select objects behind camera.

private:
    // Frames between front culls while the camera is static (the cull throttle's seed is
    // half of it, at least 4). Was the VOCache partition's octree LOD period.
    static constexpr U32 STATIC_CULL_PERIOD = 16;

    LLViewerRegion* mRegionp;
    LLVOCacheGrid mGrid;
    U32   mFrontCullFrame = 0;
    bool  mFrontCull = false; //the view frustum cull if set, otherwise is back sphere cull.
    U32   mCullHistory;
    U32   mCulledTime[LLViewerCamera::NUM_CAMERAS];

    S32   mBackSlectionEnabled; //enable to select back objects if > 0.
    U32   mIdleHash;
};

//
//Note: LLVOCache is not thread-safe: main thread only. (#312: it runs its file I/O on its
//own serial I/O thread, which sees only copies; see startIO in llvocache.cpp.)
//
class LLVOCache : public LLParamSingleton<LLVOCache>
{
    LLSINGLETON(LLVOCache, bool read_only);
    ~LLVOCache() ;

private:
#if LL_WINDOWS
#pragma pack(push,1)
#endif
    struct HeaderEntryInfo
    {
        HeaderEntryInfo() = default;
        U64 mHandle = 0;
        S32 mIndex = 0;
        U32 mTime = 0;
    };

    struct HeaderMetaInfo
    {
        HeaderMetaInfo() = default;

        U32 mVersion = 0;
        U32 mAddressSize = 0;
    };

#if LL_WINDOWS
#pragma pack(pop)
#endif

    struct header_entry_less
    {
        bool operator()(const HeaderEntryInfo* lhs, const HeaderEntryInfo* rhs) const
        {
            if(lhs->mTime == rhs->mTime)
            {
                return lhs < rhs ;
            }

            return lhs->mTime < rhs->mTime ; // older entry in front of queue (set)
        }
    };
    typedef std::set<HeaderEntryInfo*, header_entry_less> header_entry_queue_t;
    typedef std::map<U64, HeaderEntryInfo*> handle_entry_map_t;

public:
    // We need this init to be separate from constructor, since we might construct cache, purge it, then init.
    void initCache(ELLPath location, U32 size, U32 cache_version);
    void removeCache(ELLPath location, bool started = false) ;

    // (#312) A region's cache files as read on the I/O thread.
    typedef LLVOCacheReadResult ReadResult;
    typedef std::function<void(ReadResult&)> read_callback_t;

    // Reads the region's cache files on the I/O thread. done runs on the main thread --
    // before this returns when there is nothing on disk to read -- and hands the result
    // to applyRead.
    void readFromCacheAsync(U64 handle, const LLUUID& id, read_callback_t done);
    // Adopts a read into the region's maps; false when the cache must be rewritten
    // (missing, short or corrupt).
    bool applyRead(U64 handle, ReadResult& result, LLVOCacheEntry::vocache_entry_map_t& cache_entry_map,
                   LLVOCacheEntry::vocache_gltf_overrides_map_t& cache_extras_entry_map, LLViewerRegion* regionp);

    // Queue the files' writes: the entries are serialized here, the files written on the
    // I/O thread.
    void writeToCache(U64 handle, const LLUUID& id, const LLVOCacheEntry::vocache_entry_map_t& cache_entry_map, bool dirty_cache, bool removal_enabled);
    void writeGenericExtrasToCache(U64 handle, const LLUUID& id, const LLVOCacheEntry::vocache_gltf_overrides_map_t& cache_extras_entry_map, bool dirty_cache, bool removal_enabled);
    void removeEntry(U64 handle) ;
    void removeGenericExtrasForHandle(U64 handle);

    // Blocks until every queued cache file read and write has run.
    void flushIO();
    // (#568) Main loop, once per frame (LLAppViewer::idle, also during login): run the I/O
    // completions -- cache reads landing, which send the region handshake reply the sim
    // waits on before it streams objects -- for up to budget_ms, at least one. They used
    // to ride the shared mainloop work queue and waited there behind other work (a
    // 108 ms read applied 425 ms after it was posted at login). Returns completions run.
    U32 drainCompletions(F32 budget_ms);

    U32 getCacheEntries() { return mNumEntries; }
    U32 getCacheEntriesMax() { return mCacheSize; }

private:
    void setDirNames(ELLPath location);
    // determine the cache filename for the region from the region handle
    void getObjectCacheFilename(U64 handle, std::string& filename);
    std::string getObjectCacheExtrasFilename(U64 handle);
    void removeFromCache(HeaderEntryInfo* entry);
    void readCacheHeader();
    void writeCacheHeader();
    void clearCacheInMemory();
    void removeCache() ;
    void removeEntry(HeaderEntryInfo* entry) ;
    void purgeEntries(U32 size);
    void updateEntry(const HeaderEntryInfo* entry);   // queues the header record's write
    S32 firstFreeSlot() const;

    void startIO();
    void stopIO();
    // Runs work on the I/O thread (inline when there is none), then done, if any, on the
    // main thread.
    void postIO(std::function<void()> work, std::function<void()> done);
    static bool writeHeaderEntry(const std::string& header_filename, const HeaderEntryInfo& entry, LLVolatileAPRPool* pool);

private:
    bool                 mEnabled;
    bool                 mInitialized ;
    bool                 mReadOnly ;
    HeaderMetaInfo       mMetaInfo;
    U32                  mCacheSize;
    U32                  mNumEntries;
    std::string          mHeaderFileName ;
    std::string          mObjectCacheDirName;
    LLVolatileAPRPool*   mLocalAPRFilePoolp ;
    LLVolatileAPRPool*   mIOAPRFilePoolp;   // the I/O thread's (inline jobs' when it does not run)
    LL::ThreadPool*      mIOPool;           // one thread: jobs run in posting order
    // (#568) Completions posted by the I/O thread, drained by drainCompletions. Bounded by
    // the jobs in flight (one per queued read / write).
    std::mutex                         mDoneMutex;
    std::deque<std::function<void()>>  mDoneQueue;
    header_entry_queue_t mHeaderEntryQueue;
    handle_entry_map_t   mHandleEntryMap;
};

#endif
