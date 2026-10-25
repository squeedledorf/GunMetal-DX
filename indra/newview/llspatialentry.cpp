/**
 * @file llspatialentry.cpp
 * @brief LLSpatialEntry / LLSpatialEntryData implementation (#480: what is left of LL's
 *        viewer octree entry).
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
#include "llspatialentry.h"

//-----------------------------------------------------------------------------------
//static variables definitions
//-----------------------------------------------------------------------------------
U32 LLSpatialEntryData::sCurVisible = 10; //reserve the low numbers for special use.

//-----------------------------------------------------------------------------------
//class LLSpatialEntry definitions
//-----------------------------------------------------------------------------------
LLSpatialEntry::LLSpatialEntry()
:   mBinRadius(0.f),
    mVisible(0)
{
    mPositionGroup.clear();
    mExtents[0].clear();
    mExtents[1].clear();

    for(S32 i = 0; i < NUM_DATA_TYPE; i++)
    {
        mData[i] = NULL;
    }
}

LLSpatialEntry::~LLSpatialEntry()
{
}

void LLSpatialEntry::addData(LLSpatialEntryData* data)
{
    llassert(data != NULL);

    mData[data->getDataType()] = data;
}

void LLSpatialEntry::removeData(LLSpatialEntryData* data)
{
    if(mData[data->getDataType()] != data)
    {
        return;
    }

    mData[data->getDataType()] = NULL;
}

//-----------------------------------------------------------------------------------
//class LLSpatialEntryData definitions
//-----------------------------------------------------------------------------------
LLSpatialEntryData::~LLSpatialEntryData()
{
    if(mEntry)
    {
        mEntry->removeData(this);
    }
}

LLSpatialEntryData::LLSpatialEntryData(LLSpatialEntry::eEntryDataType_t data_type)
    : mDataType(data_type),
      mEntry(NULL)
{
}

//virtual
void LLSpatialEntryData::setSpatialEntry(LLSpatialEntry* entry)
{
    llassert_always(mEntry.isNull());

    if(mEntry.notNull())
    {
        return;
    }

    if(!entry)
    {
        mEntry = new LLSpatialEntry();
    }
    else
    {
        mEntry = entry;
    }
    mEntry->addData(this);
}

void LLSpatialEntryData::removeSpatialEntry()
{
    if(mEntry)
    {
        mEntry->removeData(this);
        mEntry = NULL;
    }
}

void LLSpatialEntryData::setSpatialExtents(const LLVector3& min, const LLVector3& max)
{
    mEntry->mExtents[0].load3(min.mV);
    mEntry->mExtents[1].load3(max.mV);
}

void LLSpatialEntryData::setSpatialExtents(const LLVector4a& min, const LLVector4a& max)
{
    mEntry->mExtents[0] = min;
    mEntry->mExtents[1] = max;
}

void LLSpatialEntryData::setPositionGroup(const LLVector4a& pos)
{
    mEntry->mPositionGroup = pos;
}

const LLVector4a* LLSpatialEntryData::getSpatialExtents() const
{
    return mEntry->getSpatialExtents();
}

void LLSpatialEntryData::shift(const LLVector4a &shift_vector)
{
    mEntry->mExtents[0].add(shift_vector);
    mEntry->mExtents[1].add(shift_vector);
    mEntry->mPositionGroup.add(shift_vector);
}

const LLVector4a& LLSpatialEntryData::getPositionGroup() const
{
    return mEntry->getPositionGroup();
}

bool LLSpatialEntryData::isVisible() const
{
    if(mEntry)
    {
        return mEntry->mVisible == sCurVisible;
    }
    return false;
}

// (LL also counted an entry visible while its octree group was recently visible; no
// group has been stamped visible since the Vulkan frame replaced LL's cull.)
bool LLSpatialEntryData::isRecentlyVisible() const
{
    return isVisible();
}

void LLSpatialEntryData::setVisible() const
{
    if(mEntry)
    {
        mEntry->mVisible = sCurVisible;
    }
}

void LLSpatialEntryData::resetVisible() const
{
    if(mEntry)
    {
        mEntry->mVisible = 0;
    }
}
