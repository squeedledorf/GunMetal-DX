/**
 * @file llheroprobemanager.h
 * @brief LLHeroProbeManager class declaration
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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


#pragma once

#include "llpointer.h"

#include <vector>

class LLVOVolume;

// Mirror (hero probe) registry. LLVOVolumes flagged as mirror probes register here;
// the Vulkan renderer picks its planar mirror from getHeroList() (buildVKMirror).
class LLHeroProbeManager
{
public:
    LLHeroProbeManager();
    ~LLHeroProbeManager();

    bool registerViewerObject(LLVOVolume* vobj);
    void unregisterViewerObject(LLVOVolume* vobj);

    // Registered mirror probe objects (Vulkan selects its planar mirror from these).
    const std::vector<LLPointer<LLVOVolume>>& getHeroList() const { return mHeroVOList; }

private:
    std::vector<LLPointer<LLVOVolume>> mHeroVOList;
};
