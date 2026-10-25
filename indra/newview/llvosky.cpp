/**
 * @file llvosky.cpp
 * @brief LLVOSky class implementation
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

#include "llvosky.h"


#include "lldrawable.h"
#include "llviewertexturelist.h"
#include "pipeline.h"

#include "llsettingssky.h"
#include "llenvironment.h"


#undef min
#undef max

namespace
{
// Heavenly body constants
    const F32 SUN_DISK_RADIUS   = 0.5f;
    const F32 MOON_DISK_RADIUS  = SUN_DISK_RADIUS * 0.9f;
    const F32 SUN_INTENSITY = 1e5;
}
LLHeavenBody::LLHeavenBody(const F32 rad)
: mDirectionCached(LLVector3(0,0,0)),
  mDirection(LLVector3(0,0,0)),
  mIntensity(0.f),
  mDiskRadius(rad),
  mDraw(false),
  mHorizonVisibility(1.f),
  mVisibility(1.f),
  mVisible(false)
{
    mColor.setToBlack();
    mColorCached.setToBlack();
}

const LLQuaternion& LLHeavenBody::getRotation() const
{
    return mRotation;
}

void LLHeavenBody::setRotation(const LLQuaternion& rot)
{
    mRotation = rot;
}

const LLVector3& LLHeavenBody::getDirection() const
{
    return mDirection;
}

void LLHeavenBody::setDirection(const LLVector3 &direction)
{
    mDirection = direction;
}

void LLHeavenBody::setAngularVelocity(const LLVector3 &ang_vel)
{
    mAngularVelocity = ang_vel;
}

const LLVector3& LLHeavenBody::getAngularVelocity() const
{
    return mAngularVelocity;
}

const LLVector3& LLHeavenBody::getDirectionCached() const
{
    return mDirectionCached;
}

void LLHeavenBody::renewDirection()
{
    mDirectionCached = mDirection;
}

const LLColor3& LLHeavenBody::getColorCached() const
{
    return mColorCached;
}

void LLHeavenBody::setColorCached(const LLColor3& c)
{
    mColorCached = c;
}

const LLColor3& LLHeavenBody::getColor() const
{
    return mColor;
}

void LLHeavenBody::setColor(const LLColor3& c)
{
    mColor = c;
}

void LLHeavenBody::renewColor()
{
    mColorCached = mColor;
}

F32 LLHeavenBody::sInterpVal = 0;

F32 LLHeavenBody::interpVal()
{
    return sInterpVal;
}

void LLHeavenBody::setInterpVal(const F32 v)
{
    sInterpVal = v;
}

LLColor3 LLHeavenBody::getInterpColor() const
{
    return sInterpVal * mColor + (1 - sInterpVal) * mColorCached;
}

const F32& LLHeavenBody::getVisibility() const
{
    return mVisibility;
}

void LLHeavenBody::setVisibility(const F32 c)
{
    mVisibility = c;
}

bool LLHeavenBody::isVisible() const
{
    return mVisible;
}

void LLHeavenBody::setVisible(const bool v)
{
    mVisible = v;
}

const F32& LLHeavenBody::getIntensity() const
{
    return mIntensity;
}

void LLHeavenBody::setIntensity(const F32 c)
{
    mIntensity = c;
}

void LLHeavenBody::setDiskRadius(const F32 radius)
{
    mDiskRadius = radius;
}

F32 LLHeavenBody::getDiskRadius() const
{
    return mDiskRadius;
}

void LLHeavenBody::setDraw(const bool draw)
{
    mDraw = draw;
}

bool LLHeavenBody::getDraw() const
{
    return mDraw;
}

const LLVector3& LLHeavenBody::corner(const S32 n) const
{
    return mQuadCorner[n];
}

LLVector3& LLHeavenBody::corner(const S32 n)
{
    return mQuadCorner[n];
}

const LLVector3* LLHeavenBody::corners() const
{
    return mQuadCorner;
}

/***************************************
        Sky
***************************************/

LLVOSky::LLVOSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
:   LLStaticViewerObject(id, pcode, regionp, true),
    mBumpSunDir(0.f, 0.f, 1.f),
    mSun(SUN_DISK_RADIUS), mMoon(MOON_DISK_RADIUS),
    mCloudDensity(0.2f),
    mWind(0.f)
{
    mInitialized = false;
    mbCanSelect = false;

    mSun.setIntensity(SUN_INTENSITY);
    mMoon.setIntensity(0.1f * SUN_INTENSITY);
}


LLVOSky::~LLVOSky()
{
    // Don't delete images - it'll get deleted by gTextureList on shutdown
}

void LLVOSky::init()
{
    llassert(!mInitialized);

    // Update sky at least once to get correct initial sun/moon directions
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    psky->update();

    updateDirections(psky);

    mInitialized = true;
}

void LLVOSky::updateDirections(LLSettingsSky::ptr_t psky)
{
    mSun.setDirection(psky->getSunDirection());
    mMoon.setDirection(psky->getMoonDirection());
    mSun.setRotation(psky->getSunRotation());
    mMoon.setRotation(psky->getMoonRotation());
    mSun.renewDirection();
    mMoon.renewDirection();
}

void LLVOSky::idleUpdate(LLAgent &agent, const F64 &time)
{
}

LLDrawable *LLVOSky::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_SKY);
    return mDrawable;
}

void LLVOSky::setSunAndMoonDirectionsCFR(const LLVector3 &sun_dir_cfr, const LLVector3 &moon_dir_cfr)
{
    mSun.setDirection(sun_dir_cfr);
    mMoon.setDirection(moon_dir_cfr);

    // Push the sun "South" as it approaches directly overhead so that we can always see bump mapping
    // on the upward facing faces of cubes.
    // Same as dot product with the up direction + clamp.
    F32 sunDot = llmax(0.f, sun_dir_cfr.mV[2]);
    sunDot *= sunDot;

    // Create normalized vector that has the sunDir pushed south about an hour and change.
    LLVector3 adjustedDir = (sun_dir_cfr + LLVector3(0.f, -0.70711f, 0.70711f)) * 0.5f;

    // Blend between normal sun dir and adjusted sun dir based on how close we are
    // to having the sun overhead.
    mBumpSunDir = adjustedDir * sunDot + sun_dir_cfr * (1.0f - sunDot);
    mBumpSunDir.normalize();

    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    updateDirections(psky);
}

void LLVOSky::setSunDirectionCFR(const LLVector3 &sun_dir_cfr)
{
    mSun.setDirection(sun_dir_cfr);

    // Push the sun "South" as it approaches directly overhead so that we can always see bump mapping
    // on the upward facing faces of cubes.
    // Same as dot product with the up direction + clamp.
    F32 sunDot = llmax(0.f, sun_dir_cfr.mV[2]);
    sunDot *= sunDot;

    // Create normalized vector that has the sunDir pushed south about an hour and change.
    LLVector3 adjustedDir = (sun_dir_cfr + LLVector3(0.f, -0.70711f, 0.70711f)) * 0.5f;

    // Blend between normal sun dir and adjusted sun dir based on how close we are
    // to having the sun overhead.
    mBumpSunDir = adjustedDir * sunDot + sun_dir_cfr * (1.0f - sunDot);
    mBumpSunDir.normalize();

    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    updateDirections(psky);
}

void LLVOSky::setMoonDirectionCFR(const LLVector3 &moon_dir_cfr)
{
    mMoon.setDirection(moon_dir_cfr);
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    updateDirections(psky);
}
