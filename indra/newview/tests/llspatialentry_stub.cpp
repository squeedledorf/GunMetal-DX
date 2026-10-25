/**
 * @file llspatialentry_stub.cpp
 * @brief  stub implementations to allow unit testing
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
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

LLSpatialEntryData::LLSpatialEntryData(LLSpatialEntry::eEntryDataType_t) {}
LLSpatialEntryData::~LLSpatialEntryData() = default;
bool LLSpatialEntryData::isVisible() const { return false; }
bool LLSpatialEntryData::isRecentlyVisible() const { return false; }
void LLSpatialEntryData::setVisible() const {}
void LLSpatialEntryData::resetVisible() const {}
const LLVector4a& LLSpatialEntryData::getPositionGroup() const { return LLVector4a::getZero(); }
const LLVector4a* LLSpatialEntryData::getSpatialExtents() const { return nullptr; }
void LLSpatialEntryData::setSpatialExtents(const LLVector4a& min, const LLVector4a& max) {}
void LLSpatialEntryData::setPositionGroup(const LLVector4a& pos) {}
void LLSpatialEntryData::setSpatialEntry(LLSpatialEntry* entry) {}
U32 LLSpatialEntryData::sCurVisible{};
