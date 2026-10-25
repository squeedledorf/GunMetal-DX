/**
 * @file llvosky.h
 * @brief LLVOSky class header file
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

#ifndef LL_LLVOSKY_H
#define LL_LLVOSKY_H

#include "stdtypes.h"
#include "v3color.h"
#include "v4coloru.h"
#include "llquaternion.h"
#include "llviewertexture.h"
#include "llviewerobject.h"
#include "llframetimer.h"
#include "v3colorutil.h"
#include "llsettingssky.h"
#include "lllegacyatmospherics.h"

const F32 HEAVENLY_BODY_DIST    = HORIZON_DIST - 20.f;
const F32 HEAVENLY_BODY_FACTOR  = 0.1f;
const F32 HEAVENLY_BODY_SCALE   = HEAVENLY_BODY_DIST * HEAVENLY_BODY_FACTOR;

/// TODO Move into the stars draw pool (and rename them appropriately).
class LLHeavenBody
{
protected:
    LLVector3       mDirectionCached;       // hack for events that shouldn't happen every frame

    LLColor3        mColor;
    LLColor3        mColorCached;
    F32             mIntensity;
    LLVector3       mDirection;             // direction of the local heavenly body
    LLQuaternion    mRotation;
    LLVector3       mAngularVelocity;       // velocity of the local heavenly body

    F32             mDiskRadius;
    bool            mDraw;                  // false - do not draw.
    F32             mHorizonVisibility;     // number [0, 1] due to how horizon
    F32             mVisibility;            // same but due to other objects being in throng.
    bool            mVisible;
    static F32      sInterpVal;
    LLVector3       mQuadCorner[4];
    LLVector3       mO;

public:
    LLHeavenBody(const F32 rad);
    ~LLHeavenBody() {}

    const LLQuaternion& getRotation() const;
    void                setRotation(const LLQuaternion& rot);

    const LLVector3& getDirection() const;
    void setDirection(const LLVector3 &direction);
    void setAngularVelocity(const LLVector3 &ang_vel);
    const LLVector3& getAngularVelocity() const;

    const LLVector3& getDirectionCached() const;
    void renewDirection();

    const LLColor3& getColorCached() const;
    void setColorCached(const LLColor3& c);
    const LLColor3& getColor() const;
    void setColor(const LLColor3& c);

    void renewColor();

    static F32 interpVal();
    static void setInterpVal(const F32 v);

    LLColor3 getInterpColor() const;

    const F32& getVisibility() const;
    void setVisibility(const F32 c = 1);

    bool isVisible() const;
    void setVisible(const bool v);

    const F32& getIntensity() const;
    void setIntensity(const F32 c);

    void setDiskRadius(const F32 radius);
    F32 getDiskRadius() const;

    void setDraw(const bool draw);
    bool getDraw() const;

    const LLVector3& corner(const S32 n) const;
    LLVector3& corner(const S32 n);
    const LLVector3* corners() const;
};

// The sun/moon state the environment drives. The Vulkan renderer draws the sky
// (and fetches the sky textures itself); this object only carries CPU-side state.
class LLVOSky : public LLStaticViewerObject
{
public:
    LLVOSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    // Initialize/delete data that's only inited once per class.
    void init();

    /*virtual*/ void idleUpdate(LLAgent &agent, const F64 &time);

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);

    const LLHeavenBody& getSun() const                      { return mSun;  }
    const LLHeavenBody& getMoon() const                     { return mMoon; }

    // directions provided should already be in CFR coord sys (+x at, +z up, +y right)
    void setSunAndMoonDirectionsCFR(const LLVector3 &sun_dir, const LLVector3 &moon_dir);
    void setSunDirectionCFR(const LLVector3 &sun_direction);
    void setMoonDirectionCFR(const LLVector3 &moon_direction);

    void setFogRatio(const F32 fog_ratio)               { m_legacyAtmospherics.setFogRatio(fog_ratio); }
    F32  getFogRatio() const                            { return m_legacyAtmospherics.getFogRatio(); }

    LLColor4 getSkyFogColor() const                     { return m_legacyAtmospherics.getFogColor(); }

    void setCloudDensity(F32 cloud_density)             { mCloudDensity = cloud_density; }
    void setWind ( const LLVector3& wind )              { mWind = wind.length(); }

public:
    LLVector3   mBumpSunDir;

protected:
    ~LLVOSky();

    void updateDirections(LLSettingsSky::ptr_t psky);

    LLHeavenBody        mSun;
    LLHeavenBody        mMoon;
    F32                 mCloudDensity;
    F32                 mWind;

    bool                mInitialized;

    LLAtmospherics      m_legacyAtmospherics;
};

#endif
