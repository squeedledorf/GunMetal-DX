/**
 * @file llsky.cpp
 * @brief IndraWorld sky class
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

//  Ideas:
//      -haze should be controlled by global query from sims
//      -need secondary optical effects on sun (flare)
//      -stars should be brought down from sims
//      -star intensity should be driven by global ambient level from sims,
//       so that eclipses, etc can be easily done.
//

#include "llviewerprecompiledheaders.h"

#include "llsky.h"

// linden library includes
#include "llerror.h"
#include "llmath.h"
#include "math.h"
#include "v4color.h"

#include "llviewerobjectlist.h"
#include "llviewerobject.h"
#include "llviewercamera.h"
#include "pipeline.h"

#include "llvosky.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llvoavatarself.h"

F32 azimuth_from_vector(const LLVector3 &v);
F32 elevation_from_vector(const LLVector3 &v);

LLSky gSky;

// ---------------- LLSky ----------------
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

LLSky::LLSky()
{
}


LLSky::~LLSky()
{
}

void LLSky::cleanup()
{
    mVOSkyp = NULL;
}

void LLSky::setSunAndMoonDirectionsCFR(const LLVector3 &sun_direction, const LLVector3 &moon_direction)
{
    if(mVOSkyp.notNull()) {
        mVOSkyp->setSunAndMoonDirectionsCFR(sun_direction, moon_direction);
    }
}

void LLSky::setSunDirectionCFR(const LLVector3 &sun_direction)
{
    if(mVOSkyp.notNull()) {
        mVOSkyp->setSunDirectionCFR(sun_direction);
    }
}

void LLSky::setMoonDirectionCFR(const LLVector3 &moon_direction)
{
    if(mVOSkyp.notNull()) {
        mVOSkyp->setMoonDirectionCFR(moon_direction);
    }
}

//////////////////////////////////////////////////////////////////////
// Public Methods
//////////////////////////////////////////////////////////////////////

void LLSky::init()
{
    mVOSkyp = (LLVOSky *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_SKY, NULL);
    mVOSkyp->init();
    gPipeline.createObject(mVOSkyp.get());

    gSky.setFogRatio(gSavedSettings.getF32("RenderFogRatio"));
}


void LLSky::setCloudDensityAtAgent(F32 cloud_density)
{
    if (mVOSkyp)
    {
        mVOSkyp->setCloudDensity(cloud_density);
    }
}


void LLSky::setWind(const LLVector3& average_wind)
{
    if (mVOSkyp)
    {
        mVOSkyp->setWind(average_wind);
    }
}

//////////////////////////////////////////////////////////////////////
// Private Methods
//////////////////////////////////////////////////////////////////////


void LLSky::addSunMoonBeacons()
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    static LLCachedControl<bool> show_sun_beacon(gSavedSettings, "sunbeacon", false);
    static LLCachedControl<bool> show_moon_beacon(gSavedSettings, "moonbeacon", false);
    if (!show_sun_beacon && !show_moon_beacon)
    {
        return;
    }

    // Agent-frame directions from the active sky (the VK sky's source; LLVOSky is
    // not maintained under VK).
    const LLVector3 pos_agent = gAgentAvatarp->getRenderPosition();
    if (show_sun_beacon)
    {
        renderSunMoonBeacons(pos_agent, LLEnvironment::instance().getSunDirection(), LLColor4(1.f, 0.5f, 0.f, 0.5f));
    }
    if (show_moon_beacon)
    {
        renderSunMoonBeacons(pos_agent, LLEnvironment::instance().getMoonDirection(), LLColor4(1.f, 0.f, 0.8f, 0.5f));
    }
}

LLColor4 LLSky::getSkyFogColor() const
{
    if (mVOSkyp)
    {
        return mVOSkyp->getSkyFogColor();
    }

    return LLColor4(1.f, 1.f, 1.f, 1.f);
}


void LLSky::setFogRatio(const F32 fog_ratio)
{
    if (mVOSkyp)
    {
        mVOSkyp->setFogRatio(fog_ratio);
    }
}


F32 LLSky::getFogRatio() const
{
    if (mVOSkyp)
    {
        return mVOSkyp->getFogRatio();
    }
    else
    {
        return 0.f;
    }
}


// Returns angle (DEGREES) between the horizontal plane and "v",
// where the angle is negative when v.mV[VZ] < 0.0f
F32 elevation_from_vector(const LLVector3 &v)
{
    F32 elevation = 0.0f;
    F32 xy_component = (F32) sqrt(v.mV[VX] * v.mV[VX] + v.mV[VY] * v.mV[VY]);
    if (xy_component != 0.0f)
    {
        elevation = RAD_TO_DEG * (F32) atan(v.mV[VZ]/xy_component);
    }
    else
    {
        if (v.mV[VZ] > 0.f)
        {
            elevation = 90.f;
        }
        else
        {
            elevation = -90.f;
        }
    }
    return elevation;
}
