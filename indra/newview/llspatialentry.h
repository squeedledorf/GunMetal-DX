/**
 * @file llspatialentry.h
 * @brief LLSpatialEntry: the spatial record an object's drawable and its object-cache
 *        entry share (extents, centre, bin radius, visibility stamp).
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLSPATIALENTRY_H
#define LL_LLSPATIALENTRY_H

#include "llpointer.h"
#include "llrefcount.h"
#include "llvector4a.h"
#include "v3math.h"

class LLSpatialEntryData;

// What is left of LL's viewer octree entry (#480: the octrees are gone). One record per
// object, SHARED by its LLDrawable and its LLVOCacheEntry: while the object is live the
// drawable writes it (LLDrawable::updateSpatialExtents, the visibility stamp) and the
// cache side reads it (LLViewerRegion::removeActiveCacheEntry hands the extents back to
// the cache grid, killCachedObjectsBeyond reads the bin radius); while it is cached the
// cache entry writes it (LLVOCacheEntry::setBoundingInfo) and a drawable created from
// the cache adopts it (LLDrawable::init). The two back-pointers let either side find the
// other.
//LL_ALIGN_PREFIX(16)
class LLSpatialEntry : public LLRefCount
{
    LL_ALIGN_NEW
    friend class LLSpatialEntryData;

public:
    typedef enum
    {
        LLDRAWABLE = 0,
        LLVOCACHEENTRY,
        NUM_DATA_TYPE
    }eEntryDataType_t;

protected:
    virtual ~LLSpatialEntry();

public:
    LLSpatialEntry();

    void removeData(LLSpatialEntryData* data);

    LLSpatialEntryData* getDrawable() const     {return mData[LLDRAWABLE];}
    bool                hasDrawable() const     {return mData[LLDRAWABLE] != NULL;}
    LLSpatialEntryData* getVOCacheEntry() const {return mData[LLVOCACHEENTRY];}
    bool                hasVOCacheEntry() const {return mData[LLVOCACHEENTRY] != NULL;}

    const LLVector4a* getSpatialExtents() const {return mExtents;}
    const LLVector4a& getPositionGroup() const  {return mPositionGroup;}
    F32               getBinRadius() const      {return mBinRadius;}

private:
    void addData(LLSpatialEntryData* data);

private:
    LLSpatialEntryData*         mData[NUM_DATA_TYPE]; //do not use LLPointer here.

    //aligned members
    LL_ALIGN_16(LLVector4a      mExtents[2]);
    LL_ALIGN_16(LLVector4a      mPositionGroup);
    F32                         mBinRadius;
    mutable U32                 mVisible;

} ;//LL_ALIGN_POSTFIX(16);

// Base of the two holders of an LLSpatialEntry (LLDrawable, LLVOCacheEntry): accessors
// for the shared record, and the per-frame visibility stamp. sCurVisible is the frame
// counter (LLDrawable::incrementVisible, once per frame); an entry stamped with it is
// visible this frame.
//LL_ALIGN_PREFIX(16)
class LLSpatialEntryData : public LLRefCount
{
protected:
    virtual ~LLSpatialEntryData();

public:
    LLSpatialEntryData(const LLSpatialEntryData& rhs)
    {
        *this = rhs;
    }
    LLSpatialEntryData(LLSpatialEntry::eEntryDataType_t data_type);

    LLSpatialEntry::eEntryDataType_t getDataType() const {return mDataType;}
    LLSpatialEntry* getEntry() {return mEntry;}

    // Adopts entry (shared with the other holder), or a new one when null.
    virtual void setSpatialEntry(LLSpatialEntry* entry);
    void         removeSpatialEntry();

    F32               getBinRadius() const   {return mEntry->getBinRadius();}
    const LLVector4a* getSpatialExtents() const;
    const LLVector4a& getPositionGroup() const;

    void setBinRadius(F32 rad)  {mEntry->mBinRadius = rad;}
    void setSpatialExtents(const LLVector3& min, const LLVector3& max);
    void setSpatialExtents(const LLVector4a& min, const LLVector4a& max);
    void setPositionGroup(const LLVector4a& pos);

    void shift(const LLVector4a &shift_vector);

    U32  getVisible() const {return mEntry ? mEntry->mVisible : 0;}
    void setVisible() const;
    void resetVisible() const;
    bool isVisible() const;
    bool isRecentlyVisible() const;

    static S32 getCurrentFrame() { return sCurVisible; }

protected:
    LLVector4a& getGroupPosition()  {return mEntry->mPositionGroup;}
    void        initVisible(U32 visible) {mEntry->mVisible = visible;}

    static void incrementVisible() {sCurVisible++;}
protected:
    LLPointer<LLSpatialEntry>        mEntry;
    LLSpatialEntry::eEntryDataType_t mDataType;
    static  U32                      sCurVisible; // Counter for what value of mVisible means currently visible
};//LL_ALIGN_POSTFIX(16);

#endif // LL_LLSPATIALENTRY_H
