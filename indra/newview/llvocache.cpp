/**
 * @file llvocache.cpp
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

#include "llviewerprecompiledheaders.h"
#include "llvocache.h"
#include "llregionhandle.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "lldrawable.h"
#include "llviewerregion.h"
#include "llagentcamera.h"
#include "llsdserialize.h"
#include "llworld.h" // For LLWorld::getInstance()
#include "llcullcamera.h"
#include "gmdxregioncapture.h"   // <GMDX> region map capture: the region being mapped comes out of the cache
#include "threadpool.h"

#include <bitset>
#include <future>
//static variables
U32 LLVOCacheEntry::sMinFrameRange = 0;
F32 LLVOCacheEntry::sNearRadius = 1.0f;
F32 LLVOCacheEntry::sRearFarRadius = 1.0f;
F32 LLVOCacheEntry::sFrontPixelThreshold = 1.0f;
F32 LLVOCacheEntry::sRearPixelThreshold = 1.0f;

const S32 ENTRY_HEADER_SIZE = 6 * sizeof(S32);
const S32 MAX_ENTRY_BODY_SIZE = 10000;

bool check_read(LLAPRFile* apr_file, void* src, S32 n_bytes)
{
    return apr_file->read(src, n_bytes) == n_bytes ;
}

bool check_write(LLAPRFile* apr_file, void* src, S32 n_bytes)
{
    return apr_file->write(src, n_bytes) == n_bytes ;
}

// Material Override Cache needs a version label, so we can upgrade this later.
const std::string LLGLTFOverrideCacheEntry::VERSION_LABEL = {"GLTFCacheVer"};
const int LLGLTFOverrideCacheEntry::VERSION = 1;

bool LLGLTFOverrideCacheEntry::fromLLSD(const LLSD& data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    llassert(data.has("local_id"));
    llassert(data.has("object_id"));
    llassert(data.has("region_handle_x") && data.has("region_handle_y"));

    if (!data.has("local_id"))
    {
        return false;
    }

    if (data.has("region_handle_x") && data.has("region_handle_y"))
    {
        // TODO start requiring this once server sends this for all messages
        U32 region_handle_y = data["region_handle_y"].asInteger();
        U32 region_handle_x = data["region_handle_x"].asInteger();
        mRegionHandle = to_region_handle(region_handle_x, region_handle_y);
    }
    else
    {
        return false;
    }

    mLocalId = data["local_id"].asInteger();
    mObjectId = data["object_id"];

    // message should be interpreted thusly:
    ///  sides is a list of face indices
    //   gltf_llsd is a list of corresponding GLTF override LLSD
    //   any side not represented in "sides" has no override
    if (data.has("sides") && data.has("gltf_llsd"))
    {
        LLSD const& sides = data.get("sides");
        LLSD const& gltf_llsd = data.get("gltf_llsd");

        if (sides.isArray() && gltf_llsd.isArray() &&
            sides.size() != 0 &&
            sides.size() == gltf_llsd.size())
        {
            for (int i = 0; i < sides.size(); ++i)
            {
                S32 side_idx = sides[i].asInteger();
                // a face index outside any object's range is not an override
                if (side_idx < 0 || side_idx >= (S32)LLTEContents::MAX_TES)
                {
                    continue;
                }
                mSides[side_idx] = gltf_llsd[i];
                LLGLTFMaterial* override_mat = new LLGLTFMaterial();
                override_mat->applyOverrideLLSD(gltf_llsd[i]);
                mGLTFMaterial[side_idx] = override_mat;
            }
        }
        else
        {
            LL_WARNS_IF(sides.size() != 0, "GLTF") << "broken override cache entry" << LL_ENDL;
        }
    }

    llassert(mSides.size() == mGLTFMaterial.size());
#ifdef SHOW_ASSERT
    for (auto const & side : mSides)
    {
        // check that mSides and mGLTFMaterial have exactly the same keys present
        llassert(mGLTFMaterial.count(side.first) == 1);
    }
#endif

    return true;
}

LLSD LLGLTFOverrideCacheEntry::toLLSD() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    LLSD data;
    U32 region_handle_x, region_handle_y;
    from_region_handle(mRegionHandle, &region_handle_x, &region_handle_y);
    data["region_handle_y"] = LLSD::Integer(region_handle_y);
    data["region_handle_x"] = LLSD::Integer(region_handle_x);

    data["object_id"] = mObjectId;
    data["local_id"] = (LLSD::Integer) mLocalId;

    llassert(mSides.size() == mGLTFMaterial.size());
    for (auto const & side : mSides)
    {
        // check that mSides and mGLTFMaterial have exactly the same keys present
        llassert(mGLTFMaterial.count(side.first) == 1);
        data["sides"].append(LLSD::Integer(side.first));
        data["gltf_llsd"].append(side.second);
    }

    return data;
}

//---------------------------------------------------------------------------
// LLVOCacheEntry
//---------------------------------------------------------------------------

LLVOCacheEntry::LLVOCacheEntry(U32 local_id, U32 crc, LLDataPackerBinaryBuffer &dp)
:   LLSpatialEntryData(LLSpatialEntry::LLVOCACHEENTRY),
    mLocalID(local_id),
    mCRC(crc),
    mUpdateFlags(-1),
    mHitCount(0),
    mDupeCount(0),
    mCRCChangeCount(0),
    mState(INACTIVE),
    mSceneContrib(0.f),
    mValid(true),
    mParentID(0),
    mBSphereRadius(-1.0f)
{
    mBuffer = new U8[dp.getBufferSize()];
    mDP.assignBuffer(mBuffer, dp.getBufferSize());
    mDP = dp;
}

LLVOCacheEntry::LLVOCacheEntry()
:   LLSpatialEntryData(LLSpatialEntry::LLVOCACHEENTRY),
    mLocalID(0),
    mCRC(0),
    mUpdateFlags(-1),
    mHitCount(0),
    mDupeCount(0),
    mCRCChangeCount(0),
    mBuffer(NULL),
    mState(INACTIVE),
    mSceneContrib(0.f),
    mValid(true),
    mParentID(0),
    mBSphereRadius(-1.0f)
{
    mDP.assignBuffer(mBuffer, 0);
}

LLVOCacheEntry::LLVOCacheEntry(LLAPRFile* apr_file)
:   LLSpatialEntryData(LLSpatialEntry::LLVOCACHEENTRY),
    mBuffer(NULL),
    mUpdateFlags(-1),
    mState(INACTIVE),
    mSceneContrib(0.f),
    mValid(false),
    mParentID(0),
    mBSphereRadius(-1.0f)
{
    S32 size = -1;
    bool success;
    U8 data_buffer[ENTRY_HEADER_SIZE];   // (#312: runs on the cache I/O thread)

    mDP.assignBuffer(mBuffer, 0);

    success = check_read(apr_file, (void *)data_buffer, ENTRY_HEADER_SIZE);
    if (success)
    {
        memcpy(&mLocalID, data_buffer, sizeof(U32));
        memcpy(&mCRC, data_buffer + sizeof(U32), sizeof(U32));
        memcpy(&mHitCount, data_buffer + (2 * sizeof(U32)), sizeof(S32));
        memcpy(&mDupeCount, data_buffer + (3 * sizeof(U32)), sizeof(S32));
        memcpy(&mCRCChangeCount, data_buffer + (4 * sizeof(U32)), sizeof(S32));
        memcpy(&size, data_buffer + (5 * sizeof(U32)), sizeof(S32));

        // Corruption in the cache entries
        if ((size > MAX_ENTRY_BODY_SIZE) || (size < 1))
        {
            // We've got a bogus size, skip reading it.
            // We won't bother seeking, because the rest of this file
            // is likely bogus, and will be tossed anyway.
            LL_WARNS() << "Bogus cache entry, size " << size << ", aborting!" << LL_ENDL;
            success = false;
        }
    }
    else
    {
        LL_WARNS() << "Short read of a cache entry header (" << ENTRY_HEADER_SIZE << " bytes), aborting!" << LL_ENDL;
    }
    if(success && size > 0)
    {
        mBuffer = new U8[size];
        success = check_read(apr_file, mBuffer, size);

        if(success)
        {
            mDP.assignBuffer(mBuffer, size);
        }
        else
        {
            // Improve logging around vocache
            LL_WARNS() << "Error loading cache entry for " << mLocalID << ", size " << size << " aborting!" << LL_ENDL;
            delete[] mBuffer ;
            mBuffer = NULL ;
        }
    }

    if(!success)
    {
        mLocalID = 0;
        mCRC = 0;
        mHitCount = 0;
        mDupeCount = 0;
        mCRCChangeCount = 0;
        mBuffer = NULL;
        mEntry = NULL;
        mState = INACTIVE;
    }
}

LLVOCacheEntry::~LLVOCacheEntry()
{
    if (mGrid)
    {
        mGrid->remove(this);   // (#479) e.g. LLViewerRegion::clearVOCacheFromMemory
    }
    mDP.freeBuffer();
}

void LLVOCacheEntry::updateEntry(U32 crc, LLDataPackerBinaryBuffer &dp)
{
    if(mCRC != crc)
    {
        mCRC = crc;
        mCRCChangeCount++;
    }

    mDP.freeBuffer();

    llassert_always(dp.getBufferSize() > 0);
    mBuffer = new U8[dp.getBufferSize()];
    mDP.assignBuffer(mBuffer, dp.getBufferSize());
    mDP = dp;
}

void LLVOCacheEntry::setParentID(U32 id)
{
    if(mParentID != id)
    {
        removeAllChildren();
        mParentID = id;
    }
}

void LLVOCacheEntry::removeAllChildren()
{
    if(mChildrenList.empty())
    {
        return;
    }

    for(vocache_entry_set_t::iterator iter = mChildrenList.begin(); iter != mChildrenList.end(); ++iter)
    {
        (*iter)->setParentID(0);
    }
    mChildrenList.clear();

    return;
}

//virtual
void LLVOCacheEntry::setSpatialEntry(LLSpatialEntry* entry)
{
    if(!entry && mDP.getBufferSize() > 0)
    {
        LLUUID fullid;
        LLViewerObject::unpackUUID(&mDP, fullid, "ID");

        LLViewerObject* obj = gObjectList.findObject(fullid);
        if(obj && obj->mDrawable)
        {
            entry = obj->mDrawable->getEntry();
        }
    }

    LLSpatialEntryData::setSpatialEntry(entry);
}

void LLVOCacheEntry::setState(U32 state)
{
    if(state > LOW_BITS) //special states
    {
        mState |= (HIGH_BITS & state);
        return;
    }

    //
    //otherwise LOW_BITS states
    //
    clearState(LOW_BITS);
    mState |= (LOW_BITS & state);

    if(getState() == ACTIVE)
    {
        const U32 MIN_INTERVAL = 64U + sMinFrameRange;
        U32 last_visible = getVisible();

        setVisible();

        U32 cur_visible = getVisible();
        if(cur_visible - last_visible > MIN_INTERVAL ||
            cur_visible < MIN_INTERVAL)
        {
            mLastCameraUpdated = 0; //reset
        }
        else
        {
            mLastCameraUpdated = LLViewerRegion::sLastCameraUpdated;
        }
    }
}

void LLVOCacheEntry::addChild(LLVOCacheEntry* entry)
{
    llassert(entry != NULL);
    llassert(entry->getParentID() == mLocalID);
    llassert(entry->getEntry() != NULL);

    if(!entry || !entry->getEntry() || entry->getParentID() != mLocalID)
    {
        return;
    }

    mChildrenList.insert(entry);

    //update parent bbox
    if(getEntry() != NULL && isState(INACTIVE))
    {
        updateParentBoundingInfo(entry);
        resetVisible();
    }
}

void LLVOCacheEntry::removeChild(LLVOCacheEntry* entry)
{
    entry->setParentID(0);

    vocache_entry_set_t::iterator iter = mChildrenList.find(entry);
    if(iter != mChildrenList.end())
    {
        mChildrenList.erase(iter);
    }
}

//remove the first child, and return it.
LLVOCacheEntry* LLVOCacheEntry::getChild()
{
    LLVOCacheEntry* child = NULL;
    vocache_entry_set_t::iterator iter = mChildrenList.begin();
    if(iter != mChildrenList.end())
    {
        child = *iter;
        mChildrenList.erase(iter);
    }

    return child;
}

LLDataPackerBinaryBuffer *LLVOCacheEntry::getDP() const
{
    if (mDP.getBufferSize() == 0)
    {
        //LL_INFOS() << "Not getting cache entry, invalid!" << LL_ENDL;
        return NULL;
    }

    return &mDP;
}

void LLVOCacheEntry::recordHit()
{
    mHitCount++;
}


void LLVOCacheEntry::dump() const
{
    LL_INFOS() << "local " << mLocalID
        << " crc " << mCRC
        << " hits " << mHitCount
        << " dupes " << mDupeCount
        << " change " << mCRCChangeCount
        << LL_ENDL;
}

S32 LLVOCacheEntry::writeToBuffer(U8 *data_buffer) const
{
    S32 size = mDP.getBufferSize();

    if (size > MAX_ENTRY_BODY_SIZE)
    {
        LL_WARNS() << "Failed to write entry with size above allowed limit: " << size << LL_ENDL;
        return 0;
    }

    memcpy(data_buffer, &mLocalID, sizeof(U32));
    memcpy(data_buffer + sizeof(U32), &mCRC, sizeof(U32));
    memcpy(data_buffer + (2 * sizeof(U32)), &mHitCount, sizeof(S32));
    memcpy(data_buffer + (3 * sizeof(U32)), &mDupeCount, sizeof(S32));
    memcpy(data_buffer + (4 * sizeof(U32)), &mCRCChangeCount, sizeof(S32));
    memcpy(data_buffer + (5 * sizeof(U32)), &size, sizeof(S32));
    memcpy(data_buffer + ENTRY_HEADER_SIZE, (void*)mBuffer, size);

    return ENTRY_HEADER_SIZE + size;
}

#ifndef LL_TEST
//static
void LLVOCacheEntry::updateDebugSettings()
{
    static LLFrameTimer timer;
    if(timer.getElapsedTimeF32() < 1.0f) //update frequency once per second.
    {
        return;
    }
    timer.reset();

    //objects within the view frustum whose visible area is greater than this threshold will be loaded
    static LLCachedControl<F32> front_pixel_threshold(gSavedSettings,"SceneLoadFrontPixelThreshold");
    sFrontPixelThreshold = front_pixel_threshold;

    //objects out of the view frustum whose visible area is greater than this threshold will remain loaded
    static LLCachedControl<F32> rear_pixel_threshold(gSavedSettings,"SceneLoadRearPixelThreshold");
    sRearPixelThreshold = rear_pixel_threshold;
    sRearPixelThreshold = llmax(sRearPixelThreshold, sFrontPixelThreshold); //can not be smaller than sFrontPixelThreshold.

    //make parameters adaptive to memory usage
    //starts to put restrictions from low_mem_bound_MB, apply tightest restrictions when hits high_mem_bound_MB
    static LLCachedControl<U32> low_mem_bound_MB(gSavedSettings,"SceneLoadLowMemoryBound");
    static LLCachedControl<U32> high_mem_bound_MB(gSavedSettings,"SceneLoadHighMemoryBound");

    LLMemory::updateMemoryInfo() ;
    U32 allocated_mem = LLMemory::getAllocatedMemKB().value();
    static const F32 KB_to_MB = 1.f / 1024.f;
    U32 clamped_memory = (U32)llclamp(allocated_mem * KB_to_MB, (F32) low_mem_bound_MB, (F32) high_mem_bound_MB);
    const F32 adjust_range = (F32)(high_mem_bound_MB - low_mem_bound_MB);
    const F32 adjust_factor = (high_mem_bound_MB - clamped_memory) / adjust_range; // [0, 1]

    //min radius: all objects within this radius remain loaded in memory
    static LLCachedControl<F32> min_radius(gSavedSettings,"SceneLoadMinRadius");
    static const F32 MIN_RADIUS = 1.0f;

    F32 draw_radius = gAgentCamera.mDrawDistance;
    if (LLViewerTexture::sDesiredDiscardBias > 2.f && LLViewerTexture::isSystemMemoryLow())
    {
        // Discard's bias maximum is 4 so we need to check 2 to 4 range
        // Factor is intended to go from 1.0 to 2.0
        F32 factor = 1.f + (LLViewerTexture::sDesiredDiscardBias - 2.f) / 2.f;
        // For safety cap reduction at 50%, we don't want to go below half of draw distance
        draw_radius = llmax(draw_radius / factor, draw_radius / 2.f);
    }
    const F32 clamped_min_radius = llclamp((F32) min_radius, MIN_RADIUS, draw_radius); // [1, mDrawDistance]
    sNearRadius = MIN_RADIUS + ((clamped_min_radius - MIN_RADIUS) * adjust_factor);

    // a percentage of draw distance beyond which all objects outside of view frustum will be unloaded, regardless of pixel threshold
    static LLCachedControl<F32> rear_max_radius_frac(gSavedSettings,"SceneLoadRearMaxRadiusFraction");
    const F32 min_radius_plus_one = sNearRadius + 1.f;
    const F32 max_radius = rear_max_radius_frac * draw_radius;
    const F32 clamped_max_radius = llclamp(max_radius, min_radius_plus_one, draw_radius); // [sNearRadius, mDrawDistance]
    sRearFarRadius = min_radius_plus_one + ((clamped_max_radius - min_radius_plus_one) * adjust_factor);

    //the number of frames invisible objects stay in memory
    static LLCachedControl<U32> inv_obj_time(gSavedSettings,"NonvisibleObjectsInMemoryTime");
    static const U32 MIN_FRAMES = 10;
    static const U32 MAX_FRAMES = 64;
    const U32 clamped_frames = inv_obj_time ? llclamp((U32) inv_obj_time, MIN_FRAMES, MAX_FRAMES) : MAX_FRAMES; // [10, 64], with zero => 64
    sMinFrameRange = MIN_FRAMES + (U32)((clamped_frames - MIN_FRAMES) * adjust_factor);
}
#endif // LL_TEST

//static
F32 LLVOCacheEntry::getSquaredPixelThreshold(bool is_front)
{
    F32 threshold;
    if(is_front)
    {
        threshold = sFrontPixelThreshold;
    }
    else
    {
        threshold = sRearPixelThreshold;
    }

    //object projected area threshold (#479: the culling camera's; frozen with it)
    F32 pixel_meter_ratio = LLCullCamera::getPixelMeterRatio();
    F32 projection_threshold = pixel_meter_ratio > 0.f ? threshold / pixel_meter_ratio : 0.f;
    projection_threshold *= projection_threshold;

    return projection_threshold;
}

void LLVOCacheEntry::calcSceneContribution(const LLVector4a& camera_origin, bool needs_update, U32 last_update, F32 max_dist)
{
    if(!needs_update && getVisible() >= last_update)
    {
        return; //no need to update
    }

    mSceneContrib = sceneContributionFrom(camera_origin, max_dist);

    setVisible();
}

F32 LLVOCacheEntry::sceneContributionFrom(const LLVector4a& camera_origin, F32 max_dist) const
{
    LLVector4a lookAt;
    lookAt.setSub(getPositionGroup(), camera_origin);
    F32 distance = lookAt.getLength3().getF32();
    distance -= sNearRadius;

    if(distance <= 0.f)
    {
        //nearby objects, set a large number
        const F32 LARGE_SCENE_CONTRIBUTION = 1000.f; //a large number to force to load the object.
        return LARGE_SCENE_CONTRIBUTION;
    }

    F32 rad = getBinRadius();
    max_dist += rad;

    if(distance + sNearRadius < max_dist)
    {
        return (rad * rad) / distance;
    }
    return 0.f; //out of draw distance, not to load
}

void LLVOCacheEntry::saveBoundingSphere()
{
    mBSphereCenter = getPositionGroup();
    mBSphereRadius = getBinRadius();
}

void LLVOCacheEntry::setBoundingInfo(const LLVector3& pos, const LLVector3& scale)
{
    LLVector4a center, newMin, newMax;
    center.load3(pos.mV);
    LLVector4a size;
    size.load3(scale.mV);
    newMin.setSub(center, size);
    newMax.setAdd(center, size);

    setPositionGroup(center);
    setSpatialExtents(newMin, newMax);

    if(getNumOfChildren() > 0) //has children
    {
        updateParentBoundingInfo();
    }
    else
    {
        setBinRadius(llmin(size.getLength3().getF32() * 4.f, 256.f));
    }

    if (mGrid)
    {
        mGrid->move(this);   // (#479) bounds changed while in the cache grid
    }
}

//make the parent bounding box to include all children
void LLVOCacheEntry::updateParentBoundingInfo()
{
    if(mChildrenList.empty())
    {
        return;
    }

    for(vocache_entry_set_t::iterator iter = mChildrenList.begin(); iter != mChildrenList.end(); ++iter)
    {
        updateParentBoundingInfo(*iter);
    }
    resetVisible();
}

//make the parent bounding box to include this child
void LLVOCacheEntry::updateParentBoundingInfo(const LLVOCacheEntry* child)
{
    const LLVector4a* child_exts = child->getSpatialExtents();
    LLVector4a newMin, newMax;
    newMin = child_exts[0];
    newMax = child_exts[1];

    //move to regional space.
    {
        const LLVector4a& parent_pos = getPositionGroup();
        newMin.add(parent_pos);
        newMax.add(parent_pos);
    }

    //update parent's bbox(min, max)
    const LLVector4a* parent_exts = getSpatialExtents();
    update_min_max(newMin, newMax, parent_exts[0]);
    update_min_max(newMin, newMax, parent_exts[1]);
    for(S32 i = 0; i < 4; i++)
    {
        llclamp(newMin[i], 0.f, 256.f);
        llclamp(newMax[i], 0.f, 256.f);
    }
    setSpatialExtents(newMin, newMax);

    //update parent's bbox center
    LLVector4a center;
    center.setAdd(newMin, newMax);
    center.mul(0.5f);
    setPositionGroup(center);

    //update parent's bbox size vector
    LLVector4a size;
    size.setSub(newMax, newMin);
    size.mul(0.5f);
    setBinRadius(llmin(size.getLength3().getF32() * 4.f, 256.f));

    if (mGrid)
    {
        // (#479) A child joined this root while it sits in the cache (addChild from
        // removeActiveCacheEntry / clearCachedVisibleObjects): its bounds grew without a
        // re-insert. The grid moves the entry now (O(1)).
        mGrid->move(this);
    }
}
//-------------------------------------------------------------------
//LLVOCachePartition
//-------------------------------------------------------------------
LLVOCachePartition::LLVOCachePartition(LLViewerRegion* regionp)
:   mRegionp(regionp)
{
    mBackSlectionEnabled = -1;
    mIdleHash = 0;

    for(S32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
    {
        mCulledTime[i] = 0;
    }
    mCullHistory = -1;
}

LLVOCachePartition::~LLVOCachePartition()
{
}

bool LLVOCachePartition::addEntry(LLVOCacheEntry* entry)
{
    if(!entry || !llfinite(entry->getBinRadius()) || !entry->getPositionGroup().isFinite3())
    {
        return false; //data corrupted
    }

    mGrid.add(entry);

    return true;
}

void LLVOCachePartition::removeEntry(LLVOCacheEntry* entry)
{
    mGrid.remove(entry);
}

void LLVOCachePartition::selectBackObjects(LLCamera &camera, F32 pixel_threshold)
{
    if(LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return;
    }

    if(mBackSlectionEnabled < 0)
    {
        mBackSlectionEnabled = LLVOCacheEntry::sMinFrameRange - 1;
        mBackSlectionEnabled = llmax(mBackSlectionEnabled, (S32)1);
    }

    if(!mBackSlectionEnabled)
    {
        return;
    }

    //localize the camera
    LLVector3 region_agent = mRegionp->getOriginAgent();

    // #479: the rear sphere as a grid sphere query (the near radius is the sphere radius,
    // as LL's octree back cull passed).
    mGrid.selectSphere(camera.getOrigin() - region_agent, LLVOCacheEntry::sRearFarRadius,
                       pixel_threshold, LLVOCacheEntry::sRearFarRadius, mRegionp);

    mBackSlectionEnabled--;
    if(!mRegionp->getNumOfGridVisibleEntries())
    {
        mBackSlectionEnabled = 0;
    }

    return;
}

#ifndef LL_TEST
S32 LLVOCachePartition::cull(LLCamera &camera)
{
    // Frustum and projection-area only: object-cache occlusion culling (GL queries) is gone.

    if(!LLViewerRegion::sVOCacheCullingEnabled)
    {
        return 0;
    }
    if(mRegionp->isPaused())
    {
        return 0;
    }

    if(LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return 0; //no need for those cameras.
    }

    if(mCulledTime[LLViewerCamera::sCurCameraID] == LLSpatialEntryData::getCurrentFrame())
    {
        return 0; //already culled
    }
    mCulledTime[LLViewerCamera::sCurCameraID] = LLSpatialEntryData::getCurrentFrame();

    if(!mCullHistory && LLViewerRegion::isViewerCameraStatic())
    {
        U32 seed = llmax(STATIC_CULL_PERIOD >> 1, (U32)4);
        if(LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD)
        {
            if(!(LLSpatialEntryData::getCurrentFrame() % seed))
            {
                mIdleHash = (mIdleHash + 1) % seed;
            }
        }
        if(LLSpatialEntryData::getCurrentFrame() % seed != mIdleHash)
        {
            mFrontCull = false;

            //process back objects selection
            selectBackObjects(camera, LLVOCacheEntry::getSquaredPixelThreshold(mFrontCull));
            return 0; //nothing changed, reduce frequency of culling
        }
    }
    else
    {
        mBackSlectionEnabled = -1; //reset it.
    }

    //localize the camera
    LLVector3 region_agent = mRegionp->getOriginAgent();
    camera.calcRegionFrustumPlanes(region_agent, gAgentCamera.mDrawDistance);

    mFrontCull = true;
    mFrontCullFrame = (U32)LLSpatialEntryData::getCurrentFrame();
    const F32 pixel_threshold = LLVOCacheEntry::getSquaredPixelThreshold(mFrontCull);
    const LLVector3 local_origin = camera.getOrigin() - region_agent;
    mGrid.selectFront(camera, local_origin, pixel_threshold, LLVOCacheEntry::sNearRadius, mRegionp);
    if (LLVOCacheLookAhead::isActive())
    {
        // Look-ahead (#479, off by default): also select around where the camera is
        // heading, whatever the view direction (front threshold, near radius).
        mGrid.selectSphere(LLVOCacheLookAhead::getPointAgent() - region_agent, LLVOCacheLookAhead::getRadius(),
                           pixel_threshold, LLVOCacheEntry::sNearRadius, mRegionp);
    }
    // <GMDX> REGION MAP CAPTURE (gmdxregioncapture.h): the whole region being mapped comes out of
    // the cache, whatever the camera looks at (the front threshold is 0 meanwhile).
    {
        LLVector3 map_center;
        F32 map_radius = 0.f;
        if (GMDXRegionCapture::cacheSphere(mRegionp, map_center, map_radius))
            mGrid.selectSphere(map_center - region_agent, map_radius, pixel_threshold, LLVOCacheEntry::sNearRadius, mRegionp);
    }

    return 1;
}
#endif // LL_TEST

void LLVOCachePartition::setCullHistory(bool has_new_object)
{
    mCullHistory <<= 1;
    mCullHistory |= static_cast<U32>(has_new_object);
}

//-------------------------------------------------------------------
//LLVOCache
//-------------------------------------------------------------------
// Format strings used to construct filename for the object cache
static const char OBJECT_CACHE_FILENAME[] = "objects_%d_%d.slc";
static const char OBJECT_CACHE_EXTRAS_FILENAME[] = "objects_%d_%d_extras.slec";

const U32 MAX_NUM_OBJECT_ENTRIES = 128 ;
const U32 MIN_ENTRIES_TO_PURGE = 16 ;
// No cache file is legitimately anywhere near this. One that is has been damaged or
// planted, and is not read to find out which (its record count is untrusted: a huge
// file is a huge in-memory cache map).
const S64 MAX_CACHE_FILE_SIZE = 512 * 1024 * 1024;
static bool cache_file_too_large(const std::string& filename)
{
    llstat st;
    if (LLFile::stat(filename, &st) != 0) return false;   // missing: the readers handle that
    const S64 size = (S64)st.st_size;
    return size < 0 || size > MAX_CACHE_FILE_SIZE;        // (< 0: a 32-bit st_size wrapped)
}
const U32 INVALID_TIME = 0 ;
const char* object_cache_dirname = "objectcache";
const char* header_filename = "object.cache";


LLVOCache::LLVOCache(bool read_only) :
    mInitialized(false),
    mReadOnly(read_only),
    mNumEntries(0),
    mCacheSize(1),
    mEnabled(true),
    mIOPool(nullptr)
{
#ifndef LL_TEST
    mEnabled = gSavedSettings.getBOOL("ObjectCacheEnabled");
#endif
    mLocalAPRFilePoolp = new LLVolatileAPRPool() ;
    mIOAPRFilePoolp = new LLVolatileAPRPool();
}

LLVOCache::~LLVOCache()
{
    stopIO();   // every queued write lands before the header is written
    if(mEnabled)
    {
        writeCacheHeader();
        clearCacheInMemory();
    }
    delete mIOAPRFilePoolp;
    delete mLocalAPRFilePoolp;
}

//-------------------------------------------------------------------
// Cache file I/O thread (#312)
//
// Region cache files were read (RegionHandshake) and written (region teardown) on the
// main thread, several at a time on a region crossing. All file-system work on them now
// runs on one serial I/O thread, in the order the main thread posts it: a region's write
// lands before a later read of the same file (the region re-entered), and a removal after
// the write it follows. Everything else -- the header bookkeeping, LLWorld / gObjectList /
// region state -- stays on the main thread; jobs carry only copies (file names, a header
// record, serialized bytes) and return results to the main thread through "mainloop".
// Without the thread (none started, or ThreadPoolSizes widens it) the jobs run inline.
//-------------------------------------------------------------------
static const std::string VOCACHE_IO_POOL = "VOCacheIO";

void LLVOCache::startIO()
{
    if (mIOPool)
    {
        return;
    }
    if (LL::ThreadPoolBase::getConfiguredWidth(VOCACHE_IO_POOL, 1) != 1)
    {
        LL_WARNS("VOCache") << "ThreadPoolSizes sets " << VOCACHE_IO_POOL << " wider than 1; object cache"
                            << " file I/O must be serial, so it stays on the main thread" << LL_ENDL;
        return;
    }
    // Not auto_shutdown: regions are destroyed (and their caches written) after the app
    // leaves the "running" state; stopIO drains the queue at destruction.
    mIOPool = new LL::ThreadPool(VOCACHE_IO_POOL, 1, 1024 * 1024, false);
    mIOPool->start();
}

void LLVOCache::stopIO()
{
    if (!mIOPool)
    {
        return;
    }
    flushIO();
    mIOPool->close();   // drains, then joins
    delete mIOPool;
    mIOPool = nullptr;
    // (#568) Completions not yet drained are dropped, as the mainloop work queue's were at
    // this point (shutdown: no region is listed, and the singleton is being destroyed).
    std::lock_guard<std::mutex> lk(mDoneMutex);
    if (!mDoneQueue.empty())
    {
        LL_INFOS("VOCache") << "Dropping " << mDoneQueue.size() << " object cache I/O completions at shutdown" << LL_ENDL;
        mDoneQueue.clear();
    }
}

void LLVOCache::flushIO()
{
    if (!mIOPool)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    LL::WorkQueue::ptr_t io_queue = LL::WorkQueue::getInstance(VOCACHE_IO_POOL);
    auto drained = std::make_shared<std::promise<void>>();
    std::future<void> done = drained->get_future();
    if (io_queue && io_queue->post([drained]() { drained->set_value(); }))
    {
        done.wait();
    }
}

// (#568) VOCacheCompletionBudgetMs > 0: completions go to the cache's own queue, drained
// by LLAppViewer::idle every frame; 0 = the shared mainloop work queue (old path, A/B).
// Latched by drainCompletions (main thread).
static bool sCompletionQueue = true;

U32 LLVOCache::drainCompletions(F32 budget_ms)
{
    sCompletionQueue = budget_ms > 0.f;
    U32 n = 0;
    LLTimer timer;
    while (true)
    {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(mDoneMutex);
            if (mDoneQueue.empty()) break;
            // Budget, with a floor of one per frame so the queue always progresses.
            if (n > 0 && (budget_ms <= 0.f || timer.getElapsedTimeF32().value() * 1000.f >= budget_ms)) break;
            fn = std::move(mDoneQueue.front());
            mDoneQueue.pop_front();
        }
        ++n;
        if (fn)
        {
            fn();
        }
    }
    return n;
}

void LLVOCache::postIO(std::function<void()> work, std::function<void()> done)
{
    // An exception must not cross back to the main loop (postTo rethrows it there).
    std::function<void()> guarded = [work]()
    {
        try
        {
            work();
        }
        catch (const std::exception& e)
        {
            LL_WARNS("VOCache") << "Object cache I/O job failed: " << e.what() << LL_ENDL;
        }
        catch (...)
        {
            LL_WARNS("VOCache") << "Object cache I/O job failed: non-std exception" << LL_ENDL;
        }
    };
    if (mIOPool)
    {
        LL::WorkQueue::ptr_t io_queue = LL::WorkQueue::getInstance(VOCACHE_IO_POOL);
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        if (io_queue)
        {
            // (copies: postTo moves from its arguments, and they are still needed below
            // if it cannot post)
            if (!done)
            {
                if (io_queue->post(std::function<void()>(guarded)))
                {
                    return;
                }
            }
            else if (sCompletionQueue)
            {
                // (#568) Own completion queue (drainCompletions): not the shared mainloop
                // work queue, where a read's region handshake reply waited behind other work.
                if (io_queue->post(std::function<void()>([this, guarded, done]()
                    {
                        guarded();
                        std::lock_guard<std::mutex> lk(mDoneMutex);
                        mDoneQueue.push_back(done);
                    })))
                {
                    return;
                }
            }
            else if (main_queue
                     && main_queue->postTo(io_queue, std::function<void()>(guarded), std::function<void()>(done)))
            {
                return;
            }
        }
    }
    // Inline: only when no I/O thread runs (it is joined before mIOPool is cleared), so
    // the job never races the thread or its APR pool.
    guarded();
    if (done)
    {
        done();
    }
}

//static
bool LLVOCache::writeHeaderEntry(const std::string& header_filename, const HeaderEntryInfo& entry, LLVolatileAPRPool* pool)
{
    LLAPRFile apr_file(header_filename, APR_WRITE|APR_BINARY, pool);
    apr_file.seek(APR_SET, entry.mIndex * sizeof(HeaderEntryInfo) + sizeof(HeaderMetaInfo)) ;

    return check_write(&apr_file, (void*)&entry, sizeof(HeaderEntryInfo)) ;
}

void LLVOCache::updateEntry(const HeaderEntryInfo* entry)
{
    const HeaderEntryInfo record = *entry;
    const std::string header_filename = mHeaderFileName;
    LLVolatileAPRPool* pool = mIOAPRFilePoolp;
    postIO([record, header_filename, pool]()
           {
               if (!writeHeaderEntry(header_filename, record, pool))
               {
                   LL_WARNS() << "Failed to update cache header index " << record.mIndex << " handle = " << record.mHandle << LL_ENDL;
               }
           },
           nullptr);
}

void LLVOCache::setDirNames(ELLPath location)
{
    mHeaderFileName = gDirUtilp->getExpandedFilename(location, object_cache_dirname, header_filename);
    mObjectCacheDirName = gDirUtilp->getExpandedFilename(location, object_cache_dirname);
}

void LLVOCache::initCache(ELLPath location, U32 size, U32 cache_version)
{
    if(!mEnabled)
    {
        LL_WARNS() << "Not initializing cache: Cache is currently disabled." << LL_ENDL;
        return ;
    }

    if(mInitialized)
    {
        LL_WARNS() << "Cache already initialized." << LL_ENDL;
        return ;
    }
    mInitialized = true;

    setDirNames(location);
    if (!mReadOnly)
    {
        LLFile::mkdir(mObjectCacheDirName);
    }
    mCacheSize = llclamp(size, MIN_ENTRIES_TO_PURGE, MAX_NUM_OBJECT_ENTRIES);
    mMetaInfo.mVersion = cache_version;

#if defined(ADDRESS_SIZE)
    U32 expected_address = ADDRESS_SIZE;
#else
    U32 expected_address = 32;
#endif
    mMetaInfo.mAddressSize = expected_address;

    readCacheHeader();

    LL_INFOS() << "Viewer Object Cache Versions - expected: " << cache_version << " found: " << mMetaInfo.mVersion <<  LL_ENDL;

    if( mMetaInfo.mVersion != cache_version
        || mMetaInfo.mAddressSize != expected_address)
    {
        mMetaInfo.mVersion = cache_version ;
        mMetaInfo.mAddressSize = expected_address;
        if(mReadOnly) //disable cache
        {
            clearCacheInMemory();
        }
        else //delete the current cache if the format does not match.
        {
            LL_INFOS() << "Viewer Object Cache Versions unmatched.  clearing cache." <<  LL_ENDL;
            removeCache();
        }
    }

    startIO();
}

void LLVOCache::removeCache(ELLPath location, bool started)
{
    if(started)
    {
        removeCache();
        return;
    }

    if(mReadOnly)
    {
        LL_WARNS() << "Not removing cache at " << location << ": Cache is currently in read-only mode." << LL_ENDL;
        return ;
    }

    LL_INFOS() << "about to remove the object cache due to settings." << LL_ENDL ;

    flushIO();   // no queued write may land in the emptied directory
    std::string mask = "*";
    std::string cache_dir = gDirUtilp->getExpandedFilename(location, object_cache_dirname);
    LL_INFOS() << "Removing cache at " << cache_dir << LL_ENDL;
    gDirUtilp->deleteFilesInDir(cache_dir, mask); //delete all files
    LLFile::rmdir(cache_dir);

    clearCacheInMemory();
    mInitialized = false;
}

void LLVOCache::removeCache()
{
    if(!mInitialized)
    {
        //OK to remove cache even it is not initialized.
        LL_WARNS() << "Object cache is not initialized yet." << LL_ENDL;
    }

    if(mReadOnly)
    {
        LL_WARNS() << "Not clearing object cache: Cache is currently in read-only mode." << LL_ENDL;
        return ;
    }

    flushIO();   // no queued write may land in the emptied directory
    std::string mask = "*";
    LL_INFOS() << "Removing object cache at " << mObjectCacheDirName << LL_ENDL;
    gDirUtilp->deleteFilesInDir(mObjectCacheDirName, mask);

    clearCacheInMemory() ;
    writeCacheHeader();
}

void LLVOCache::removeEntry(HeaderEntryInfo* entry)
{
    llassert_always(mInitialized);
    if(mReadOnly)
    {
        return;
    }
    if(!entry)
    {
        return;
    }
    // Bit more tracking of cache creation/destruction.
    std::string filename;
    getObjectCacheFilename(entry->mHandle, filename);
    LL_INFOS() << "Removing entry for region with filename" << filename << LL_ENDL;

    // make sure corresponding LLViewerRegion also clears its in-memory cache
    LLViewerRegion* regionp = LLWorld::instance().getRegionFromHandle(entry->mHandle);
    if (regionp)
    {
        regionp->clearVOCacheFromMemory();
    }

    header_entry_queue_t::iterator iter = mHeaderEntryQueue.find(entry);
    if(iter != mHeaderEntryQueue.end())
    {
        mHandleEntryMap.erase(entry->mHandle);
        mHeaderEntryQueue.erase(iter);
        removeFromCache(entry);
        delete entry;

        mNumEntries = static_cast<U32>(mHandleEntryMap.size());
    }
}

void LLVOCache::removeEntry(U64 handle)
{
    handle_entry_map_t::iterator iter = mHandleEntryMap.find(handle) ;
    if(iter == mHandleEntryMap.end()) //no cache
    {
        return ;
    }
    HeaderEntryInfo* entry = iter->second ;
    removeEntry(entry) ;
}

void LLVOCache::clearCacheInMemory()
{
    if(!mHeaderEntryQueue.empty())
    {
        for(header_entry_queue_t::iterator iter = mHeaderEntryQueue.begin(); iter != mHeaderEntryQueue.end(); ++iter)
        {
            delete *iter ;
        }
        mHeaderEntryQueue.clear();
        mHandleEntryMap.clear();
        mNumEntries = 0 ;
    }

}

void LLVOCache::getObjectCacheFilename(U64 handle, std::string& filename)
{
    U32 region_x, region_y;

    grid_from_region_handle(handle, &region_x, &region_y);
    filename = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, object_cache_dirname,
               llformat(OBJECT_CACHE_FILENAME, region_x, region_y));

    return ;
}

std::string LLVOCache::getObjectCacheExtrasFilename(U64 handle)
{
    U32 region_x, region_y;

    grid_from_region_handle(handle, &region_x, &region_y);
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, object_cache_dirname,
               llformat(OBJECT_CACHE_EXTRAS_FILENAME, region_x, region_y));
}

void LLVOCache::removeFromCache(HeaderEntryInfo* entry)
{
    if(mReadOnly)
    {
        LL_WARNS() << "Not removing cache for handle " << entry->mHandle << ": Cache is currently in read-only mode." << LL_ENDL;
        return ;
    }

    std::string filename;
    getObjectCacheFilename(entry->mHandle, filename);
    LL_WARNS("GLTF", "VOCache") << "Removing object cache for handle " << entry->mHandle << "Filename: " << filename << LL_ENDL;

    // Note: `removeFromCache` should take responsibility for cleaning up all cache artefacts specfic to the handle/entry.
    // as such this now includes the generic extras
    const std::string extras_filename = getObjectCacheExtrasFilename(entry->mHandle);
    LL_WARNS("GLTF", "VOCache") << "Removing generic extras for handle " << entry->mHandle << "Filename: " << extras_filename << LL_ENDL;
    LLVolatileAPRPool* pool = mIOAPRFilePoolp;
    postIO([filename, extras_filename, pool]()
           {
               LLAPRFile::remove(filename, pool);
               LLFile::remove(extras_filename);
           },
           nullptr);

    entry->mTime = INVALID_TIME ;
    updateEntry(entry) ; //update the head file.
}

void LLVOCache::readCacheHeader()
{
    if(!mEnabled)
    {
        LL_WARNS() << "Not reading cache header: Cache is currently disabled." << LL_ENDL;
        return;
    }

    //clear stale info.
    clearCacheInMemory();

    bool success = true ;
    if (LLAPRFile::isExist(mHeaderFileName, mLocalAPRFilePoolp))
    {
        LLAPRFile apr_file(mHeaderFileName, APR_READ|APR_BINARY, mLocalAPRFilePoolp);

        //read the meta element
        success = check_read(&apr_file, &mMetaInfo, sizeof(HeaderMetaInfo)) ;

        if(success)
        {
            HeaderEntryInfo* entry = NULL ;
            mNumEntries = 0 ;
            U32 num_read = 0 ;
            while(num_read++ < MAX_NUM_OBJECT_ENTRIES)
            {
                if(!entry)
                {
                    entry = new HeaderEntryInfo() ;
                }
                success = check_read(&apr_file, entry, sizeof(HeaderEntryInfo));

                if(!success) //failed
                {
                    LL_WARNS() << "Error reading cache header entry. (entry_index=" << mNumEntries << ")" << LL_ENDL;
                    delete entry ;
                    entry = NULL ;
                    break ;
                }
                else if(entry->mTime == INVALID_TIME)
                {
                    continue ; //an empty entry
                }

                // The slot it was read from, which is where updateEntry writes it
                // back. Counting the live ones instead put every entry after a hole
                // (a region removed, the viewer not shut down cleanly since) one slot
                // too early, so the next in-place update overwrote another region's.
                entry->mIndex = (S32)num_read - 1;
                mNumEntries++;
                mHeaderEntryQueue.insert(entry) ;
                mHandleEntryMap[entry->mHandle] = entry ;
                entry = NULL ;
            }
            if(entry)
            {
                delete entry ;
            }
        }

        //---------
        //debug code
        //----------
        //std::string name ;
        //for(header_entry_queue_t::iterator iter = mHeaderEntryQueue.begin() ; success && iter != mHeaderEntryQueue.end(); ++iter)
        //{
        //  getObjectCacheFilename((*iter)->mHandle, name) ;
        //  LL_INFOS() << name << LL_ENDL ;
        //}
        //-----------
    }
    else
    {
        writeCacheHeader() ;
    }

    if(!success)
    {
        removeCache() ; //failed to read header, clear the cache
    }
    else if(mNumEntries >= mCacheSize)
    {
        purgeEntries(mCacheSize) ;
    }

    return ;
}

void LLVOCache::writeCacheHeader()
{
    if (!mEnabled)
    {
        LL_WARNS() << "Not writing cache header: Cache is currently disabled." << LL_ENDL;
        return;
    }

    if(mReadOnly)
    {
        LL_WARNS() << "Not writing cache header: Cache is currently in read-only mode." << LL_ENDL;
        return;
    }

    bool success = true ;
    {
        LLAPRFile apr_file(mHeaderFileName, APR_CREATE|APR_WRITE|APR_BINARY, mLocalAPRFilePoolp);

        //write the meta element
        success = check_write(&apr_file, &mMetaInfo, sizeof(HeaderMetaInfo)) ;


        mNumEntries = 0 ;
        for(header_entry_queue_t::iterator iter = mHeaderEntryQueue.begin() ; success && iter != mHeaderEntryQueue.end(); ++iter)
        {
            (*iter)->mIndex = mNumEntries++ ;
            success = check_write(&apr_file, (void*)*iter, sizeof(HeaderEntryInfo));
        }

        mNumEntries = static_cast<U32>(mHeaderEntryQueue.size());
        if(success && mNumEntries < MAX_NUM_OBJECT_ENTRIES)
        {
            HeaderEntryInfo* entry = new HeaderEntryInfo() ;
            entry->mTime = INVALID_TIME ;
            for(S32 i = mNumEntries ; success && i < MAX_NUM_OBJECT_ENTRIES ; i++)
            {
                //fill the cache with the default entry.
                success = check_write(&apr_file, entry, sizeof(HeaderEntryInfo)) ;

            }
            delete entry ;
        }
    }

    if(!success)
    {
        clearCacheInMemory() ;
        mReadOnly = true ; //disable the cache.
    }
    return ;
}

S32 LLVOCache::firstFreeSlot() const
{
    // The header file is a fixed array of slots and the queue knows which are taken; a
    // removal leaves a hole rather than closing up, so the count of live entries says
    // nothing about where the next one can go. The purge keeps the count under the
    // cache size, which is at most the slot count, so there is always one.
    std::bitset<MAX_NUM_OBJECT_ENTRIES> taken;
    for (const HeaderEntryInfo* entry : mHeaderEntryQueue)
    {
        if (entry->mIndex >= 0 && (U32)entry->mIndex < MAX_NUM_OBJECT_ENTRIES)
        {
            taken.set((size_t)entry->mIndex);
        }
    }
    for (U32 slot = 0; slot < MAX_NUM_OBJECT_ENTRIES; ++slot)
    {
        if (!taken.test(slot))
        {
            return (S32)slot;
        }
    }
    return (S32)MAX_NUM_OBJECT_ENTRIES - 1;
}

// (#312) What the I/O thread read of one region's cache files, for applyRead on the main
// thread. Shared (std::shared_ptr) between the job and its completion, never copied: the
// entries' LLRefCount and the LLSD are not thread-safe, so only one thread touches them at
// a time, the queue hand-off ordering the two.
struct LLVOCacheReadResult
{
    bool mReadFiles = false;    // the region had a header entry, so its files were read
    bool mSuccess = false;      // the object file read whole and clean
    LLVOCacheEntry::vocache_entry_map_t mEntries;
    std::vector<LLSD> mExtras;  // extras entries, those read before any failure
    bool mExtrasFailed = false; // the extras file is unusable (from mExtras.size() on)
    F64 mPostedAt = 0.0;
    F64 mIOSeconds = 0.0;
};

// I/O thread. The object file; false makes the region rewrite it (a partial read keeps
// the entries read before the fault).
static bool read_object_cache_file(const std::string& filename, const LLUUID& id, LLVolatileAPRPool* pool,
                                   LLVOCacheEntry::vocache_entry_map_t& cache_entry_map)
{
    bool success = true ;
    S32 num_entries = 0 ;
    {
        LLUUID cache_id;
        const bool too_large = cache_file_too_large(filename);
        if (too_large)
        {
            LL_WARNS() << "Object cache file " << filename << " is implausibly large; not read" << LL_ENDL;
        }
        LLAPRFile apr_file(filename, APR_READ|APR_BINARY, pool);

        success = !too_large && check_read(&apr_file, cache_id.mData, UUID_BYTES);
        if (!success && !too_large)
        {
            LL_WARNS() << "Object cache file " << filename << " is missing or shorter than its region id; not read" << LL_ENDL;
        }

        if(success)
        {
            if(cache_id != id)
            {
                LL_INFOS() << "Cache ID doesn't match for this region (" << filename << ": file " << cache_id
                           << ", region " << id << "), discarding"<< LL_ENDL;
                success = false ;
            }

            if(success)
            {
                success = check_read(&apr_file, &num_entries, sizeof(S32)) ;

                if(success)
                {
                    // (#528) apr_file_eof() only reports EOF after a read has already come
                    // back short, so it cannot stop this loop at the end of the file. Stop on
                    // the file size instead. Files written before #528 declare the region's
                    // whole in-memory entry count but leave out the entries killed in-session
                    // (removal_enabled), so they legitimately end before num_entries: that is
                    // a clean short file, every entry in it is good, not corruption. It
                    // returns false anyway so the region is marked dirty and the file is
                    // rewritten with the right count at the next save.
                    llstat st;
                    const S64 file_size = (LLFile::stat(filename, &st) == 0) ? (S64)st.st_size : -1;
                    if (num_entries > 0 && file_size > 0)
                    {
                        cache_entry_map.reserve((size_t)llmin((S64)num_entries, file_size / ENTRY_HEADER_SIZE));
                    }
                    for (S32 i = 0; i < num_entries; i++)
                    {
                        const S32 offset = apr_file.seek(APR_CUR, 0);
                        if (file_size >= 0 && offset >= 0 && (S64)offset >= file_size)
                        {
                            LL_INFOS("VOCache") << "Object cache file " << filename << " ends after " << i
                                                << " of " << num_entries << " declared entries (offset " << offset
                                                << ", file size " << file_size
                                                << "): short count from an older writer, all entries kept" << LL_ENDL;
                            success = false; // dirty: rewrite with the correct count
                            break;
                        }

                        LLPointer<LLVOCacheEntry> entry = new LLVOCacheEntry(&apr_file);
                        if (!entry->getLocalID())
                        {
                            LL_WARNS() << "Aborting cache file load for " << filename << ", cache file corruption! Entry "
                                       << i << " of " << num_entries << " at offset " << offset << " (file size "
                                       << file_size << ") did not decode (short read of the "
                                       << ENTRY_HEADER_SIZE << "-byte entry header or its body, or a bogus body size;"
                                       << " see the preceding warning). Keeping the " << cache_entry_map.size()
                                       << " entries read before it." << LL_ENDL;
                            success = false ;
                            break ;
                        }
                        cache_entry_map[entry->getLocalID()] = entry;
                    }
                    if (success && file_size >= 0)
                    {
                        const S32 end = apr_file.seek(APR_CUR, 0);
                        if (end >= 0 && (S64)end != file_size)
                        {
                            LL_WARNS() << "Object cache file " << filename << " has " << (file_size - end)
                                       << " trailing bytes after its " << num_entries << " declared entries (offset "
                                       << end << ", file size " << file_size << "); entries kept, file will be rewritten" << LL_ENDL;
                            success = false;
                        }
                    }
                }
                else
                {
                    LL_WARNS() << "Object cache file " << filename << ": could not read the entry count after the region id" << LL_ENDL;
                }
            }
        }
    }

    LL_DEBUGS("GLTF", "VOCache") << "Read " << cache_entry_map.size() << " entries from object cache " << filename << ", expected " << num_entries << ", success=" << (success?"True":"False") << LL_ENDL;
    return success;
}

// I/O thread. The extras (GLTF override) file, parsed to LLSD; applyRead does the rest.
// Returns false when the file is unusable from extras.size() on.
static bool read_extras_cache_file(const std::string& filename, U64 handle, const LLUUID& id, std::vector<LLSD>& extras)
{
    if (cache_file_too_large(filename))
    {
        LL_WARNS() << "Extras cache file " << filename << " is implausibly large; not read" << LL_ENDL;
        return false;
    }
    llifstream in(filename, std::ios::in | std::ios::binary);

    std::string line;
    std::getline(in, line);
    if(!in.good())
    {
        LL_WARNS() << "Failed reading extras cache for handle " << handle << LL_ENDL;
        return false;
    }
    // file formats need versions, let's add one. legacy cache files will be considered version 0
    // This will make it easier to upgrade/revise later.
    int versionNumber=0;
    if (line.compare(0, LLGLTFOverrideCacheEntry::VERSION_LABEL.length(), LLGLTFOverrideCacheEntry::VERSION_LABEL) == 0)
    {
        std::string versionStr = line.substr(LLGLTFOverrideCacheEntry::VERSION_LABEL.length()+1); // skip the version label and ':'
        try
        {
            versionNumber = std::stol(versionStr);
        }
        catch (std::logic_error&)
        {
            versionNumber = -1;
        }
    }
    // For future versions we may call a legacy handler here, but realistically we'll just consider this cache out of date.
    // The important thing is to make sure it gets removed.
    if(versionNumber != LLGLTFOverrideCacheEntry::VERSION)
    {
        LL_WARNS() << "Unexpected version number " << versionNumber << " for extras cache for handle " << handle << LL_ENDL;
        return false;
    }

    LL_DEBUGS("VOCache") << "Reading extras cache for handle " << handle << ", version " << versionNumber << LL_ENDL;
    std::getline(in, line);
    if(!LLUUID::validate(line))
    {
        LL_WARNS() << "Failed reading extras cache for handle" << handle << ". invalid uuid line: '" << line << "'" << LL_ENDL;
        return false;
    }

    LLUUID cache_id(line);
    if(cache_id != id)
    {
        // if the cache id doesn't match the expected region we should just kill the file.
        LL_WARNS() << "Cache ID doesn't match for this region, deleting it" << LL_ENDL;
        return false;
    }

    U32 num_entries;  // if removal was enabled during write num_entries might be wrong
    std::getline(in, line);
    if(!in.good())
    {
        LL_WARNS() << "Failed reading extras cache for handle " << handle << LL_ENDL;
        return false;
    }
    try
    {
        num_entries = std::stol(line);
    }
    catch(std::logic_error&)  // either invalid_argument or out_of_range
    {
        LL_WARNS() << "Failed reading extras cache for handle " << handle << ". unreadable num_entries" << LL_ENDL;
        return false;
    }

    LL_DEBUGS("GLTF") << "Beginning reading extras cache for handle " << handle << " from " << filename << LL_ENDL;

    for (U32 i = 0; i < num_entries && !in.eof(); i++)
    {
        static const U32 max_size = 4096;
        LLSD entry_llsd;
        bool success = LLSDSerialize::deserialize(entry_llsd, in, max_size);
        // check bool(in) this time since eof is not a failure condition here
        if(!success || !in)
        {
            LL_WARNS() << "Failed reading extras cache for handle " << handle << ", entry number " << i << " cache patrtial load only." << LL_ENDL;
            return false;
        }
        extras.push_back(std::move(entry_llsd));
    }
    return true;
}

void LLVOCache::readFromCacheAsync(U64 handle, const LLUUID& id, read_callback_t done)
{
    LL_PROFILE_ZONE_SCOPED;
    auto result = std::make_shared<ReadResult>();
    if(!mEnabled)
    {
        LL_WARNS() << "Not reading cache for handle " << handle << "): Cache is currently disabled." << LL_ENDL;
        result->mSuccess = true; // no problem we're just read only
        done(*result);
        return;
    }
    llassert_always(mInitialized);

    if (mHandleEntryMap.find(handle) == mHandleEntryMap.end()) //no cache
    {
        // the normal case for a region never visited from this cache: nothing to read,
        // and false (arguably not a problem) marks it dirty.
        LL_DEBUGS("VOCache") << "No handle map entry for " << handle << LL_ENDL;
        done(*result);
        return;
    }

    std::string filename;
    getObjectCacheFilename(handle, filename);
    const std::string extras_filename = getObjectCacheExtrasFilename(handle);
    LLVolatileAPRPool* pool = mIOAPRFilePoolp;
    result->mReadFiles = true;
    result->mPostedAt = LLTimer::getTotalSeconds().value();
    postIO([result, handle, id, filename, extras_filename, pool]()
           {
               LL_PROFILE_ZONE_NAMED("vocache read");
               LLTimer io_timer;
               result->mSuccess = read_object_cache_file(filename, id, pool, result->mEntries);
               result->mExtrasFailed = !read_extras_cache_file(extras_filename, handle, id, result->mExtras);
               result->mIOSeconds = io_timer.getElapsedTimeF64().value();
           },
           [result, done]()
           {
               done(*result);
           });
}

// We now return bool to trigger dirty cache; this in turn forces a rewrite after a
// partial read due to corruption.
bool LLVOCache::applyRead(U64 handle, ReadResult& result, LLVOCacheEntry::vocache_entry_map_t& cache_entry_map,
                          LLVOCacheEntry::vocache_gltf_overrides_map_t& cache_extras_entry_map, LLViewerRegion* regionp)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!result.mReadFiles)
    {
        return result.mSuccess;
    }
    LLTimer apply_timer;
    const size_t num_read = result.mEntries.size();
    if (!mEnabled || !mInitialized || mHandleEntryMap.find(handle) == mHandleEntryMap.end())
    {
        // The entry went while the read was in flight (purged, or a failed save of the
        // previous visit removed it): what was read is not this region's cache any more.
        LL_INFOS("VOCache") << "Object cache for handle " << handle << " removed while it was being read; "
                            << num_read << " entries dropped" << LL_ENDL;
        result.mEntries.clear();
        result.mExtras.clear();
        return false;
    }

    // Nothing reaches the map before the handshake reply; if anything did, it is newer.
    for (auto& [local_id, entry] : result.mEntries)
    {
        cache_entry_map.emplace(local_id, entry);
    }
    result.mEntries.clear();

    if (!result.mSuccess && cache_entry_map.empty())
    {
        removeEntry(handle);
    }

    // We pass in the cache entry map, so that we can remove entries from extras that are no longer in the primary cache.
    if (mHandleEntryMap.find(handle) != mHandleEntryMap.end())
    {
        int loaded = 0;
        int discarded = 0;
        for (const LLSD& entry_llsd : result.mExtras)
        {
            LLGLTFOverrideCacheEntry entry;
            entry.fromLLSD(entry_llsd);
            U32 local_id = entry_llsd["local_id"].asInteger();
            // only add entries that exist in the primary cache
            // this is a self-healing test that avoids us polluting the cache with entries that are no longer valid based on the main cache.
            if(cache_entry_map.find(local_id)!= cache_entry_map.end())
            {
                // attempt to backfill a null objectId, though these shouldn't be in the persisted cache really
                if(entry.mObjectId.isNull() && regionp)
                {
                    gObjectList.getUUIDFromLocal( entry.mObjectId, local_id, regionp->getHost().getAddress(), regionp->getHost().getPort() );
                }
                cache_extras_entry_map.emplace(local_id, entry);
                loaded++;
            }
            else
            {
                discarded++;
            }
        }
        if (result.mExtrasFailed)
        {
            removeGenericExtrasForHandle(handle);
        }
        LL_DEBUGS("GLTF") << "Completed reading extras cache for handle " << handle << ", " << loaded << " loaded, " << discarded << " discarded" << LL_ENDL;
    }
    result.mExtras.clear();

    LL_INFOS("VOCache") << "Read object cache for handle " << handle << ": " << num_read << " entries, success="
                        << (result.mSuccess ? "true" : "false") << ", disk "
                        << (S32)(result.mIOSeconds * 1000.0) << " ms on the I/O thread, "
                        << (S32)((LLTimer::getTotalSeconds().value() - result.mPostedAt) * 1000.0) << " ms until applied, apply "
                        << apply_timer.getElapsedTimeF64().value() * 1000.0 << " ms (main thread)" << LL_ENDL;
    return result.mSuccess;
}

void LLVOCache::purgeEntries(U32 size)
{
    LL_DEBUGS("VOCache","GLTF") << "Purging " << size << " entries from cache" << LL_ENDL;
    while(mHeaderEntryQueue.size() > size)
    {
        header_entry_queue_t::iterator iter = mHeaderEntryQueue.begin() ;
        HeaderEntryInfo* entry = *iter ;
        mHandleEntryMap.erase(entry->mHandle) ;
        mHeaderEntryQueue.erase(iter) ;
        removeFromCache(entry) ; // This now handles removing extras cache where appropriate.
        delete entry;
    }
    mNumEntries = static_cast<U32>(mHandleEntryMap.size());
}

// Main thread: the object file's bytes (region id, entry count, entries), so the I/O
// thread never touches the entries.
static bool serialize_object_cache(const LLUUID& id, const LLVOCacheEntry::vocache_entry_map_t& cache_entry_map,
                                   bool removal_enabled, const std::string& filename, std::vector<U8>& out)
{
    // (#528) The count of entries actually written, not the map size: with removal
    // enabled (RequestFullRegionCache, region up > 10 min) entries killed in-session
    // stay in the map invalid and are skipped below. Declaring the map size made every
    // such file read as "cache file corruption" at the next login.
    S32 num_entries = 0;
    for (LLVOCacheEntry::vocache_entry_map_t::const_iterator iter = cache_entry_map.begin(); iter != cache_entry_map.end(); ++iter)
    {
        if (!removal_enabled || iter->second->isValid())
        {
            ++num_entries;
        }
    }
    out.reserve(UUID_BYTES + sizeof(S32) + (size_t)num_entries * (ENTRY_HEADER_SIZE + 256));
    out.insert(out.end(), id.mData, id.mData + UUID_BYTES);
    const U8* count_bytes = reinterpret_cast<const U8*>(&num_entries);
    out.insert(out.end(), count_bytes, count_bytes + sizeof(S32));

    const S32 buffer_size = 32768; //should be large enough for couple MAX_ENTRY_BODY_SIZE
    U8 data_buffer[buffer_size]; // generaly entries are fairly small, so collect them and append in one go
    S32 size_in_buffer = 0;
    for (LLVOCacheEntry::vocache_entry_map_t::const_iterator iter = cache_entry_map.begin(); iter != cache_entry_map.end(); ++iter)
    {
        if (!removal_enabled || iter->second->isValid())
        {
            S32 size = iter->second->writeToBuffer(data_buffer + size_in_buffer);

            if (size > ENTRY_HEADER_SIZE) // body is minimum of 1
            {
                size_in_buffer += size;
            }
            else
            {
                LL_WARNS() << "Failed to write cache entry to buffer for " << filename << ", entry number " << iter->second->getLocalID() << LL_ENDL;
                return false;
            }

            // Make sure we have space in buffer for next element
            if (buffer_size - size_in_buffer < MAX_ENTRY_BODY_SIZE + ENTRY_HEADER_SIZE)
            {
                out.insert(out.end(), data_buffer, data_buffer + size_in_buffer);
                size_in_buffer = 0;
            }
        }
    }
    out.insert(out.end(), data_buffer, data_buffer + size_in_buffer);
    LL_DEBUGS("VOCache") << "Serialized " << num_entries << " entries for the primary VOCache file " << filename << LL_ENDL;
    return true;
}

void LLVOCache::writeToCache(U64 handle, const LLUUID& id, const LLVOCacheEntry::vocache_entry_map_t& cache_entry_map, bool dirty_cache, bool removal_enabled)
{
    LL_PROFILE_ZONE_SCOPED;
    std::string filename;
    getObjectCacheFilename(handle, filename);
    if(!mEnabled)
    {
        LL_WARNS() << "Not writing cache for " << filename << " (handle:" << handle << "): Cache is currently disabled." << LL_ENDL;
        return ;
    }
    llassert_always(mInitialized);

    if(mReadOnly)
    {
        LL_WARNS() << "Not writing cache for " << filename << " (handle:" << handle << "): Cache is currently in read-only mode." << LL_ENDL;
        return ;
    }

    HeaderEntryInfo* entry;
    handle_entry_map_t::iterator iter = mHandleEntryMap.find(handle) ;
    if(iter == mHandleEntryMap.end()) //new entry
    {
        if(mNumEntries >= mCacheSize - 1)
        {
            purgeEntries(mCacheSize - 1) ;
        }

        entry = new HeaderEntryInfo();
        entry->mHandle = handle ;
        entry->mTime = (U32)time(NULL) ;
        entry->mIndex = firstFreeSlot();   // (not the live count: holes exist)
        mNumEntries++;
        mHeaderEntryQueue.insert(entry) ;
        mHandleEntryMap[handle] = entry ;
    }
    else
    {
        // Update access time.
        entry = iter->second ;

        //resort
        mHeaderEntryQueue.erase(entry) ;

        entry->mTime = (U32)time(NULL) ;
        mHeaderEntryQueue.insert(entry) ;
    }

    if(!dirty_cache)
    {
        // The header record still goes out: its time is the entry's LRU age.
        updateEntry(entry);
        LL_WARNS() << "Skipping write to cache for " << filename << " (handle:" << handle << "): cache not dirty" << LL_ENDL;
        return ; //nothing changed, no need to update.
    }

    LLTimer serialize_timer;
    auto data = std::make_shared<std::vector<U8>>();
    if (!serialize_object_cache(id, cache_entry_map, removal_enabled, filename, *data))
    {
        updateEntry(entry);
        removeEntry(entry) ;
        return;
    }
    const F64 serialize_ms = serialize_timer.getElapsedTimeF64().value() * 1000.0;

    // The header record, then the file -- one job, so a failed header update skips the
    // file as before. (#528) The file is written to a temp file and swapped in only once
    // complete, so a crash, kill or power loss during the save (it runs at logout, from the
    // region destructor) leaves the previous file intact instead of a truncated one that
    // the next login reads as corrupt.
    enum { WRITE_OK, WRITE_HEADER_FAILED, WRITE_FAILED, WRITE_NOT_SWAPPED };
    auto status = std::make_shared<S32>(WRITE_OK);
    auto io_ms = std::make_shared<F64>(0.0);
    const HeaderEntryInfo record = *entry;
    const std::string header_filename = mHeaderFileName;
    LLVolatileAPRPool* pool = mIOAPRFilePoolp;
    postIO([data, status, io_ms, record, header_filename, filename, pool]()
           {
               LL_PROFILE_ZONE_NAMED("vocache write");
               LLTimer io_timer;
               if (!writeHeaderEntry(header_filename, record, pool))
               {
                   LL_WARNS() << "Failed to update cache header index " << record.mIndex << ". " << filename << " handle = " << record.mHandle << LL_ENDL;
                   *status = WRITE_HEADER_FAILED;
                   return ; //update failed.
               }

               const std::string tmp_filename = filename + ".tmp";
               bool success = true ;
               {
                   LLAPRFile apr_file(tmp_filename, APR_CREATE|APR_WRITE|APR_BINARY|APR_TRUNCATE, pool);

                   success = apr_file.getFileHandle() != NULL
                             && check_write(&apr_file, (void*)data->data(), (S32)data->size());
                   if (!success)
                   {
                       LL_WARNS() << "Failed to open or write object cache temp file " << tmp_filename << LL_ENDL;
                   }
                   if (apr_file.getFileHandle() && apr_file.close() != APR_SUCCESS)   // (unbuffered: close completes the writes)
                   {
                       LL_WARNS() << "Failed to close object cache temp file " << tmp_filename << LL_ENDL;
                       success = false;
                   }
               }

               if (success && LLFile::replace(tmp_filename, filename) != 0)
               {
                   // e.g. the file held open by a scanner on Windows. The previous file is untouched
                   // and still a whole one (stale entries are caught by their CRC), so keep it.
                   LL_WARNS() << "Failed to move object cache temp file " << tmp_filename << " over " << filename
                              << "; keeping the previous file" << LL_ENDL;
                   LLFile::remove(tmp_filename, ENOENT);
                   *status = WRITE_NOT_SWAPPED;
               }
               else if (!success)
               {
                   LLFile::remove(tmp_filename, ENOENT);
                   *status = WRITE_FAILED;
               }
               *io_ms = io_timer.getElapsedTimeF64().value() * 1000.0;
           },
           [status, io_ms, handle, serialize_ms, bytes = data->size()]()
           {
               LL_INFOS("VOCache") << "Wrote object cache for handle " << handle << ": " << bytes << " bytes, serialize "
                                   << serialize_ms << " ms (main thread), disk " << *io_ms << " ms on the I/O thread, status "
                                   << *status << LL_ENDL;
               if (*status == WRITE_FAILED && LLVOCache::instanceExists())
               {
                   LLVOCache::instance().removeEntry(handle);
               }
           });
}

void LLVOCache::removeGenericExtrasForHandle(U64 handle)
{
    if(mReadOnly)
    {
        LL_WARNS() << "Not removing cache for handle " << handle << ": Cache is currently in read-only mode." << LL_ENDL;
        return ;
    }

    // NOTE: when removing the extras, we must also remove the objects so the simulator will send us a full upddate with the valid overrides
    handle_entry_map_t::iterator iter = mHandleEntryMap.find(handle);
    if (iter != mHandleEntryMap.end())
    {
        LL_WARNS("GLTF", "VOCache") << "Removing generic extras for handle " << handle << "Filename: " << getObjectCacheExtrasFilename(handle) << LL_ENDL;
        removeEntry(iter->second);
    }
    else
    {
        //shouldn't happen, but if it does, we should remove the extras file since it's orphaned
        const std::string extras_filename = getObjectCacheExtrasFilename(handle);
        postIO([extras_filename]() { LLFile::remove(extras_filename); }, nullptr);
    }
}

void LLVOCache::writeGenericExtrasToCache(U64 handle, const LLUUID& id, const LLVOCacheEntry::vocache_gltf_overrides_map_t& cache_extras_entry_map, bool dirty_cache, bool removal_enabled)
{
    LL_PROFILE_ZONE_SCOPED;
    if(!mEnabled)
    {
        LL_WARNS() << "Not writing extras cache for handle " << handle << "): Cache is currently disabled." << LL_ENDL;
        return;
    }
    llassert_always(mInitialized);

    if(mReadOnly)
    {
        LL_WARNS() << "Not writing extras cache for handle " << handle << "): Cache is currently in read-only mode." << LL_ENDL;
        return;
    }

    // The file's text is built here (it reads gObjectList); the I/O thread writes it.
    // get ViewerRegion pointer from handle
    LLViewerRegion* pRegion = LLWorld::getInstance()->getRegionFromHandle(handle);

    std::ostringstream body;
    U32 num_entries = 0;
    U32 skipped = 0;
    size_t inmem_entries = cache_extras_entry_map.size();
    for (auto [local_id, entry] : cache_extras_entry_map)
    {
        // Only write out GLTFOverrides that we can actually apply again on import.
        // worst case we have an extra cache miss.
        // Note: A null mObjectId is valid when in memory as we might have a data race between GLTF of the object itself.
        // This remains a valid state to persist as it is consistent with the localid checks on import with the main cache.
        // the mObjectId will be updated if/when the local object is updated from the gObject list (due to full update)
        if(entry.mObjectId.isNull() && pRegion)
        {
            gObjectList.getUUIDFromLocal( entry.mObjectId, local_id, pRegion->getHost().getAddress(), pRegion->getHost().getPort() );
        }

        if( entry.mSides.size() > 0 &&
            entry.mSides.size() == entry.mGLTFMaterial.size()
          )
        {
            LLSD entry_llsd = entry.toLLSD();
            entry_llsd["local_id"] = (S32)local_id;
            LLSDSerialize::serialize(entry_llsd, body, LLSDSerialize::LLSD_XML);
            body << '\n';
            num_entries++;
        }
        else
        {
            skipped++;
        }
    }

    // It is good practice to version file formats so let's add one.
    // legacy versions will be treated as version 0.
    std::ostringstream head;
    head << LLGLTFOverrideCacheEntry::VERSION_LABEL << ":" << LLGLTFOverrideCacheEntry::VERSION << '\n';
    head << id << '\n';
    head << std::setw(10) << std::setfill('0') << num_entries << '\n';
    auto text = std::make_shared<std::string>(head.str());
    text->append(body.str());
    LL_DEBUGS("GLTF") << "Serialized extras cache for handle " << handle << ", " << num_entries << " entries. Total in RAM: " << inmem_entries << " skipped (no persist): " << skipped << LL_ENDL;

    // (#528) Written to a temp file and swapped in only once complete (see writeToCache):
    // a truncated extras file fails to read, and that removes the region's whole object
    // cache (removeGenericExtrasForHandle), i.e. a full re-download.
    const std::string filename = getObjectCacheExtrasFilename(handle);
    auto failed = std::make_shared<bool>(false);
    postIO([text, failed, filename, handle]()
           {
               LL_PROFILE_ZONE_NAMED("vocache write extras");
               const std::string tmp_filename = filename + ".tmp";
               llofstream out(tmp_filename, std::ios::out | std::ios::binary);
               if(!out.good())
               {
                   LL_WARNS() << "Failed writing extras cache for handle " << handle << LL_ENDL;
                   *failed = true;
                   return;
               }
               out.write(text->data(), (std::streamsize)text->size());
               out.flush();
               if(!out.good())
               {
                   LL_WARNS() << "Failed writing extras cache for handle " << handle << LL_ENDL;
                   out.close();
                   LLFile::remove(tmp_filename, ENOENT);
                   *failed = true;
                   return;
               }
               out.close();
               if (out.fail() || LLFile::replace(tmp_filename, filename) != 0)
               {
                   LL_WARNS() << "Failed to move extras cache temp file " << tmp_filename << " over " << filename << LL_ENDL;
                   LLFile::remove(tmp_filename, ENOENT);
                   *failed = true;
               }
           },
           [failed, handle]()
           {
               if (*failed && LLVOCache::instanceExists())
               {
                   LLVOCache::instance().removeGenericExtrasForHandle(handle);
               }
           });
}
