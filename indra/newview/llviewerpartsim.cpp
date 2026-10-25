/**
 * @file llviewerpartsim.cpp
 * @brief LLViewerPart class implementation
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

#include "llviewerpartsim.h"
#include "llworldclock.h"

#include "llviewercontrol.h"

#include "llagent.h"
#include "llcleanparticlestore.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llvopartgroup.h"
#include "llworld.h"
#include "llmutelist.h"
#include "pipeline.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

const F32 PART_SIM_BOX_SIDE = 16.f;

//static
S32 LLViewerPartSim::sMaxParticleCount = 0;
S32 LLViewerPartSim::sParticleCount = 0;
S32 LLViewerPartSim::sParticleCount2 = 0;
// This controls how greedy individual particle burst sources are allowed to be, and adapts according to how near the particle-count limit we are.
F32 LLViewerPartSim::sParticleAdaptiveRate = 0.0625f;
F32 LLViewerPartSim::sParticleBurstRate = 0.5f;
S32 LLViewerPartSim::sGPUParticleCount = 0;

//static
const S32 LLViewerPartSim::MAX_PART_COUNT = 8192;
const F32 LLViewerPartSim::PART_THROTTLE_THRESHOLD = 0.9f;
const F32 LLViewerPartSim::PART_ADAPT_RATE_MULT = 2.0f;

//static
const F32 LLViewerPartSim::PART_THROTTLE_RESCALE = PART_THROTTLE_THRESHOLD / (1.0f-PART_THROTTLE_THRESHOLD);
const F32 LLViewerPartSim::PART_ADAPT_RATE_MULT_RECIP = 1.0f/PART_ADAPT_RATE_MULT;


U32 LLViewerPart::sNextPartID = 1;

F32 calc_desired_size(LLViewerCamera* camera, LLVector3 pos, LLVector2 scale)
{
    F32 desired_size = (pos - camera->getOrigin()).magVec();
    desired_size /= 4;
    return llclamp(desired_size, scale.magVec()*0.5f, PART_SIM_BOX_SIDE*2);
}

LLViewerPart::LLViewerPart() :
    mPartID(0),
    mLastUpdateTime(0.f),
    mSkipOffset(0.f),
    mVPCallback(NULL),
    mImagep(NULL)
{
    mPartSourcep = NULL;
    mParent = NULL;
    mChild = NULL;
    ++LLViewerPartSim::sParticleCount2 ;
}

LLViewerPart::~LLViewerPart()
{
    if (mPartSourcep.notNull() && mPartSourcep->mLastPart == this)
    {
        mPartSourcep->mLastPart = NULL;
    }

    //patch up holes in the ribbon
    if (mParent)
    {
        llassert(mParent->mChild == this);
        mParent->mChild = mChild;
    }

    if (mChild)
    {
        llassert (mChild->mParent == this);
        mChild->mParent = mParent;
    }

    mPartSourcep = NULL;

    --LLViewerPartSim::sParticleCount2 ;
}

void LLViewerPart::init(LLPointer<LLViewerPartSource> sourcep, LLViewerTexture *imagep, LLVPCallback cb)
{
    mPartID = LLViewerPart::sNextPartID;
    LLViewerPart::sNextPartID++;
    mFlags = 0x00f;
    mLastUpdateTime = 0.f;
    mMaxAge = 10.f;
    mSkipOffset = 0.0f;

    mVPCallback = cb;
    mPartSourcep = sourcep;

    mImagep = imagep;
}


/////////////////////////////
//
// LLViewerPartGroup implementation
//
//


LLViewerPartGroup::LLViewerPartGroup(const LLVector3 &center_agent, const F32 box_side, bool hud)
 : mHud(hud)
{
    mVOPartGroupp = NULL;
    mUniformParticles = true;

    mRegionp = LLWorld::getInstance()->getRegionFromPosAgent(center_agent);
    llassert_always(center_agent.isFinite());

    if (!mRegionp)
    {
        //LL_WARNS() << "No region at position, using agent region!" << LL_ENDL;
        mRegionp = gAgent.getRegion();
    }
    mCenterAgent = center_agent;
    mBoxRadius = F_SQRT3*box_side*0.5f;

    if (mHud)
    {
        mVOPartGroupp = (LLVOPartGroup *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_HUD_PART_GROUP, getRegion());
    }
    else
    {
    mVOPartGroupp = (LLVOPartGroup *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_PART_GROUP, getRegion());
    }
    mVOPartGroupp->setViewerPartGroup(this);
    mVOPartGroupp->setPositionAgent(getCenterAgent());

    mBoxSide = box_side;

    F32 scale = box_side * 0.5f;

    mVOPartGroupp->setScale(LLVector3(scale,scale,scale));

    //gPipeline.addObject(mVOPartGroupp);
    gPipeline.createObject(mVOPartGroupp);

    // The group's box (LL took the particle-octree node the drawable landed in when there
    // was one; there are no particle octrees any more, #480).
    LLVector3 extents(mBoxRadius, mBoxRadius, mBoxRadius);
    mMinObjPos = center_agent - extents;
    mMaxObjPos = center_agent + extents;

    mSkippedTime = 0.f;

    static U32 id_seed = 0;
    mID = ++id_seed;
}

LLViewerPartGroup::~LLViewerPartGroup()
{
    cleanup();

    S32 count = (S32) mParticles.size();
    for(S32 i = 0 ; i < count ; i++)
    {
        delete mParticles[i] ;
    }
    mParticles.clear();

    LLViewerPartSim::decPartCount(count);
}

void LLViewerPartGroup::cleanup()
{
    if (mVOPartGroupp)
    {
        if (!mVOPartGroupp->isDead())
        {
            gObjectList.killObject(mVOPartGroupp);
        }
        mVOPartGroupp = NULL;
    }
}

bool LLViewerPartGroup::posInGroup(const LLVector3 &pos, const F32 desired_size)
{
    if ((pos.mV[VX] < mMinObjPos.mV[VX])
        || (pos.mV[VY] < mMinObjPos.mV[VY])
        || (pos.mV[VZ] < mMinObjPos.mV[VZ]))
    {
        return false;
    }

    if ((pos.mV[VX] > mMaxObjPos.mV[VX])
        || (pos.mV[VY] > mMaxObjPos.mV[VY])
        || (pos.mV[VZ] > mMaxObjPos.mV[VZ]))
    {
        return false;
    }

    if (desired_size > 0 &&
        (desired_size < mBoxRadius*0.5f ||
        desired_size > mBoxRadius*2.f))
    {
        return false;
    }

    return true;
}


bool LLViewerPartGroup::addPart(LLViewerPart* part, F32 desired_size)
{
    if (part->mFlags & LLPartData::LL_PART_HUD && !mHud)
    {
        return false;
    }

    bool uniform_part = part->mScale.mV[0] == part->mScale.mV[1] &&
                    !(part->mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK);

    if (!posInGroup(part->mPosAgent, desired_size) ||
        (mUniformParticles && !uniform_part) ||
        (!mUniformParticles && uniform_part))
    {
        return false;
    }

    gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);

    mParticles.push_back(part);
    part->mSkipOffset=mSkippedTime;
    LLViewerPartSim::incPartCount(1);
    return true;
}


void LLViewerPartGroup::updateParticles(const F32 lastdt)
{
    F32 dt;

    LLVector3 gravity(0.f, 0.f, GRAVITY);

    LLViewerPartSim::checkParticleCount(static_cast<U32>(mParticles.size()));

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLViewerRegion *regionp = getRegion();
    S32 end = (S32) mParticles.size();
    for (S32 i = 0 ; i < (S32)mParticles.size();)
    {
        LLVector3 a(0.f, 0.f, 0.f);
        LLViewerPart* part = mParticles[i] ;

        dt = lastdt + mSkippedTime - part->mSkipOffset;
        part->mSkipOffset = 0.f;

        // Update current time
        const F32 cur_time = part->mLastUpdateTime + dt;
        const F32 frac = cur_time / part->mMaxAge;

        // "Drift" the object based on the source object
        if (part->mFlags & LLPartData::LL_PART_FOLLOW_SRC_MASK)
        {
            part->mPosAgent = part->mPartSourcep->mPosAgent;
            part->mPosAgent += part->mPosOffset;
        }

        // Do a custom callback if we have one...
        if (part->mVPCallback)
        {
            (*part->mVPCallback)(*part, dt);
        }

        if (part->mFlags & LLPartData::LL_PART_WIND_MASK)
        {
            part->mVelocity *= 1.f - 0.1f*dt;
            part->mVelocity += 0.1f*dt*regionp->mWind.getVelocity(regionp->getPosRegionFromAgent(part->mPosAgent));
        }

        // Now do interpolation towards a target
        if (part->mFlags & LLPartData::LL_PART_TARGET_POS_MASK)
        {
            F32 remaining = part->mMaxAge - part->mLastUpdateTime;
            F32 step = dt / remaining;

            step = llclamp(step, 0.f, 0.1f);
            step *= 5.f;
            // we want a velocity that will result in reaching the target in the
            // Interpolate towards the target.
            LLVector3 delta_pos = part->mPartSourcep->mTargetPosAgent - part->mPosAgent;

            delta_pos /= remaining;

            part->mVelocity *= (1.f - step);
            part->mVelocity += step*delta_pos;
        }


        if (part->mFlags & LLPartData::LL_PART_TARGET_LINEAR_MASK)
        {
            LLVector3 delta_pos = part->mPartSourcep->mTargetPosAgent - part->mPartSourcep->mPosAgent;
            part->mPosAgent = part->mPartSourcep->mPosAgent;
            part->mPosAgent += frac*delta_pos;
            part->mVelocity = delta_pos;
        }
        else
        {
            // Do velocity interpolation
            part->mPosAgent += dt*part->mVelocity;
            part->mPosAgent += 0.5f*dt*dt*part->mAccel;
            part->mVelocity += part->mAccel*dt;
        }

        // Do a bounce test
        if (part->mFlags & LLPartData::LL_PART_BOUNCE_MASK)
        {
            // Need to do point vs. plane check...
            // For now, just check relative to object height...
            F32 dz = part->mPosAgent.mV[VZ] - part->mPartSourcep->mPosAgent.mV[VZ];
            if (dz < 0)
            {
                part->mPosAgent.mV[VZ] += -2.f*dz;
                part->mVelocity.mV[VZ] *= -0.75f;
            }
        }


        // Reset the offset from the source position
        if (part->mFlags & LLPartData::LL_PART_FOLLOW_SRC_MASK)
        {
            part->mPosOffset = part->mPosAgent;
            part->mPosOffset -= part->mPartSourcep->mPosAgent;
        }

        // Do color interpolation
        if (part->mFlags & LLPartData::LL_PART_INTERP_COLOR_MASK)
        {
            part->mColor.setVec(part->mStartColor);
            // note: LLColor4's v%k means multiply-alpha-only,
            //       LLColor4's v*k means multiply-rgb-only
            part->mColor *= 1.f - frac; // rgb*k
            part->mColor %= 1.f - frac; // alpha*k
            part->mColor += frac%(frac*part->mEndColor); // rgb,alpha
        }

        // Do scale interpolation
        if (part->mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK)
        {
            part->mScale.setVec(part->mStartScale);
            part->mScale *= 1.f - frac;
            part->mScale += frac*part->mEndScale;
        }

        // Do glow interpolation
        part->mGlow.mV[3] = (U8) ll_round(lerp(part->mStartGlow, part->mEndGlow, frac)*255.f);

        // Set the last update time to now.
        part->mLastUpdateTime = cur_time;


        // Kill dead particles (either flagged dead, or too old)
        if ((part->mLastUpdateTime > part->mMaxAge) || (LLViewerPart::LL_PART_DEAD_MASK == part->mFlags))
        {
            mParticles[i] = mParticles.back() ;
            mParticles.pop_back() ;
            delete part ;
        }
        else
        {
            F32 desired_size = calc_desired_size(camera, part->mPosAgent, part->mScale);
            if (!posInGroup(part->mPosAgent, desired_size))
            {
                // Transfer particles between groups
                LLViewerPartSim::getInstance()->put(part) ;
                mParticles[i] = mParticles.back() ;
                mParticles.pop_back() ;
            }
            else
            {
                i++ ;
            }
        }
    }

    S32 removed = end - (S32)mParticles.size();
    if (removed > 0)
    {
        // we removed one or more particles, so flag this group for update
        if (mVOPartGroupp.notNull())
        {
            gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);
        }
        LLViewerPartSim::decPartCount(removed);
    }

    // Kill the viewer object if this particle group is empty
    if (mParticles.empty())
    {
        gObjectList.killObject(mVOPartGroupp);
        mVOPartGroupp = NULL;
    }

    LLViewerPartSim::checkParticleCount() ;
}


void LLViewerPartGroup::shift(const LLVector3 &offset)
{
    mCenterAgent += offset;
    mMinObjPos += offset;
    mMaxObjPos += offset;

    for (S32 i = 0 ; i < (S32)mParticles.size(); i++)
    {
        mParticles[i]->mPosAgent += offset;
    }
}

void LLViewerPartGroup::removeParticlesByID(const U32 source_id)
{
    for (S32 i = 0; i < (S32)mParticles.size(); i++)
    {
        if(mParticles[i]->mPartSourcep->getID() == source_id)
        {
            mParticles[i]->mFlags = LLViewerPart::LL_PART_DEAD_MASK;
        }
    }
}

//////////////////////////////////
//
// LLViewerPartSim implementation
//
//

//static
void LLViewerPartSim::checkParticleCount(U32 size)
{
    if(LLViewerPartSim::sParticleCount2 != LLViewerPartSim::sParticleCount)
    {
        LL_ERRS() << "sParticleCount: " << LLViewerPartSim::sParticleCount << " ; sParticleCount2: " << LLViewerPartSim::sParticleCount2 << LL_ENDL ;
    }

    if(size > (U32)LLViewerPartSim::sParticleCount2)
    {
        LL_ERRS() << "curren particle size: " << LLViewerPartSim::sParticleCount2 << " array size: " << size << LL_ENDL ;
    }
}

LLViewerPartSim::LLViewerPartSim()
{
    sMaxParticleCount = llmin(gSavedSettings.getS32("RenderMaxPartCount"), LL_MAX_PARTICLE_COUNT);
    static U32 id_seed = 0;
    mID = ++id_seed;
}

//enable/disable particle system
void LLViewerPartSim::enable(bool enabled)
{
    if(!enabled && sMaxParticleCount > 0)
    {
        sMaxParticleCount = 0; //disable
    }
    else if(enabled && sMaxParticleCount < 1)
    {
        sMaxParticleCount = llmin(gSavedSettings.getS32("RenderMaxPartCount"), LL_MAX_PARTICLE_COUNT);
    }

    return;
}

void LLViewerPartSim::destroyClass()
{
    S32 i;
    S32 count;

    // Kill all of the groups (and particles)
    count = (S32) mViewerPartGroups.size();
    for (i = 0; i < count; i++)
    {
        delete mViewerPartGroups[i];
    }
    mViewerPartGroups.clear();

    // Kill all of the sources
    mViewerPartSources.clear();
}

//static
bool LLViewerPartSim::shouldAddPart()
{
    if (sParticleCount >= MAX_PART_COUNT)
    {
        return false;
    }

    if (sParticleCount > PART_THROTTLE_THRESHOLD*sMaxParticleCount)
    {
        F32 frac = (F32)sParticleCount/(F32)sMaxParticleCount;
        frac -= PART_THROTTLE_THRESHOLD;
        frac *= PART_THROTTLE_RESCALE;
        if (ll_frand() < frac)
        {
            // Skip...
            return false;
        }
    }

    // Check frame rate, and don't add more if the viewer is really slow
    const F32 MIN_FRAME_RATE_FOR_NEW_PARTICLES = 4.f;
    if (gFPSClamped < MIN_FRAME_RATE_FOR_NEW_PARTICLES)
    {
        return false;
    }

    return true;
}

//static
// GPU path: LL's shouldAddPart() odds as a keep fraction (applied to a whole spawn
// count at once instead of one coin flip per particle). The hard cap itself is
// exact on the GPU (clean_particle_emit.comp); this is LL's soft throttle above
// PART_THROTTLE_THRESHOLD of the cap, which spreads the remaining headroom across
// sources instead of letting the first emitters to run take it all.
F32 LLViewerPartSim::gpuShouldAddFraction()
{
    if (sMaxParticleCount <= 0 || sGPUParticleCount >= MAX_PART_COUNT || sGPUParticleCount >= sMaxParticleCount)
    {
        return 0.f;
    }
    // Check frame rate, and don't add more if the viewer is really slow
    const F32 MIN_FRAME_RATE_FOR_NEW_PARTICLES = 4.f;
    if (gFPSClamped < MIN_FRAME_RATE_FOR_NEW_PARTICLES)
    {
        return 0.f;
    }
    if (sGPUParticleCount > PART_THROTTLE_THRESHOLD*sMaxParticleCount)
    {
        F32 frac = (F32)sGPUParticleCount/(F32)sMaxParticleCount;
        frac -= PART_THROTTLE_THRESHOLD;
        frac *= PART_THROTTLE_RESCALE;
        return llclamp(1.f - frac, 0.f, 1.f);
    }
    return 1.f;
}

//static
U32 LLViewerPartSim::thinSpawn(U32 n, F32 keep)
{
    if (n == 0 || keep <= 0.f) return 0;
    if (keep >= 1.f) return n;
    // Stochastic rounding: E[result] = n * keep, as n independent LL coin flips.
    return (U32)llfloor((F32)n * keep + ll_frand());
}

void LLViewerPartSim::addPart(LLViewerPart* part)
{
    if (sParticleCount < MAX_PART_COUNT)
    {
        put(part);
    }
    else
    {
        //delete the particle if can not add it in
        delete part ;
        part = NULL ;
    }
}


LLViewerPartGroup *LLViewerPartSim::put(LLViewerPart* part)
{
    const F32 MAX_MAG = 1000000.f*1000000.f; // 1 million
    LLViewerPartGroup *return_group = NULL ;
    if (part->mPosAgent.magVecSquared() > MAX_MAG || !part->mPosAgent.isFinite())
    {
#if 0 && !LL_RELEASE_FOR_DOWNLOAD
        LL_WARNS() << "LLViewerPartSim::put Part out of range!" << LL_ENDL;
        LL_WARNS() << part->mPosAgent << LL_ENDL;
#endif
    }
    else
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        F32 desired_size = calc_desired_size(camera, part->mPosAgent, part->mScale);

        S32 count = (S32) mViewerPartGroups.size();
        for (S32 i = 0; i < count; i++)
        {
            if (mViewerPartGroups[i]->addPart(part, desired_size))
            {
                // We found a spatial group that we fit into, add us and exit
                return_group = mViewerPartGroups[i];
                break ;
            }
        }

        // Hmm, we didn't fit in any of the existing spatial groups
        // Create a new one...
        if(!return_group)
        {
            llassert_always(part->mPosAgent.isFinite());
            LLViewerPartGroup *groupp = createViewerPartGroup(part->mPosAgent, desired_size, part->mFlags & LLPartData::LL_PART_HUD);
            groupp->mUniformParticles = (part->mScale.mV[0] == part->mScale.mV[1] &&
                                    !(part->mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK));
            if (!groupp->addPart(part))
            {
                LL_WARNS() << "LLViewerPartSim::put - Particle didn't go into its box!" << LL_ENDL;
                LL_INFOS() << groupp->getCenterAgent() << LL_ENDL;
                LL_INFOS() << part->mPosAgent << LL_ENDL;
                mViewerPartGroups.pop_back() ;
                delete groupp;
                groupp = NULL ;
            }
            return_group = groupp;
        }
    }

    if(!return_group) //failed to insert the particle
    {
        delete part ;
        part = NULL ;
    }

    return return_group ;
}

LLViewerPartGroup *LLViewerPartSim::createViewerPartGroup(const LLVector3 &pos_agent, const F32 desired_size, bool hud)
{
    //find a box that has a center position divisible by PART_SIM_BOX_SIDE that encompasses
    //pos_agent
    LLViewerPartGroup *groupp = new LLViewerPartGroup(pos_agent, desired_size, hud);
    mViewerPartGroups.push_back(groupp);
    return groupp;
}


void LLViewerPartSim::shift(const LLVector3 &offset)
{
    S32 i;
    S32 count;

    count = (S32) mViewerPartSources.size();
    for (i = 0; i < count; i++)
    {
        mViewerPartSources[i]->mPosAgent += offset;
        mViewerPartSources[i]->mTargetPosAgent += offset;
        mViewerPartSources[i]->mLastUpdatePosAgent += offset;
    }

    count = (S32) mViewerPartGroups.size();
    for (i = 0; i < count; i++)
    {
        mViewerPartGroups[i]->shift(offset);
    }
}

static LLTrace::BlockTimerStatHandle FTM_SIMULATE_PARTICLES("Simulate Particles");

void LLViewerPartSim::updateSimulation()
{
    static LLFrameTimer update_timer;

    const F32 dt = llmin(update_timer.getElapsedTimeAndResetF32(), 0.1f);

    if (!(gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES)))
    {
        // GPU particles: zero the emitters' spawn counts, or the GPU keeps spawning
        // at the last frame's rates while the render type is off (#43).
        if (LLCleanParticleStore::instance().ready())
        {
            LLCleanParticleStore::instance().beginFrame();
        }
        return;
    }

    LL_RECORD_BLOCK_TIME(FTM_SIMULATE_PARTICLES);

    // GPU-driven particle path: feed one emitter descriptor per live
    // script source to LLCleanParticleStore and let the GPU own emission
    // + simulation + rendering. Replaces the CPU sim (the LLViewerPart
    // creation in source update() and the per-group updateParticles()
    // below). Still honours the same per-source muting/complexity gates.
    static LLCachedControl<bool> clean_particles(gSavedSettings, "RenderCleanParticles", true);
    const bool gpu_particles = clean_particles && LLCleanParticleStore::instance().ready();
    if (gpu_particles)
    {
        LLCleanParticleStore& ps = LLCleanParticleStore::instance();
        // World frozen: no spawning (spawn counts zeroed, emitters kept alive) and no
        // source timers advance; the GPU sim runs with dt 0, so live particles hold in
        // place. dt above was still taken, so the thaw frame is an ordinary frame.
        if (LLWorldClock::isFrozen())
        {
            ps.holdFrame();
            return;
        }
        ps.beginFrame();
        // RenderMaxPartCount (Graphics prefs slider / presets / Lightbox): the GPU emit
        // enforces it exactly as a live-particle cap (0 = particles off); the live count
        // read back from the GPU drives LL's soft throttles (adaptive + burst rate, the
        // shouldAddPart odds, the script sources' distance / size throttle).
        ps.setLiveCap((U32)llclamp(sMaxParticleCount, 0, LL_MAX_PARTICLE_COUNT));
        sGPUParticleCount = (S32)ps.readLiveParticleCount();
        updateAdaptiveRate(sGPUParticleCount);
        updatePartBurstRate(sGPUParticleCount);
    }

    // Start at a random particle system so the same
    // particle system doesn't always get first pick at the
    // particles.  Theoretically we'd want to do this in distance
    // order or something, but sorting particle sources will be a big
    // pain.
    S32 i;
    S32 count = (S32) mViewerPartSources.size();
    S32 start = (S32)ll_frand((F32)count);
    S32 dir = 1;
    S32 deldir = 0;
    if (ll_frand() > 0.5f)
    {
        dir = -1;
        deldir = -1;
    }

    S32 num_updates = 0;
    for (i = start; num_updates < count;)
    {
        if (i >= count)
        {
            i = 0;
        }
        if (i < 0)
        {
            i = count - 1;
        }

        if (!mViewerPartSources[i]->isDead())
        {
            bool upd = true;
            LLViewerObject* vobj = mViewerPartSources[i]->mSourceObjectp;

            if (vobj && vobj->isAvatar() && ((LLVOAvatar*)vobj)->isInMuteList())
            {
                upd = false;
            }

            if(vobj && vobj->isOwnerInMuteList(mViewerPartSources[i]->getOwnerUUID()))
            {
                upd = false;
            }

            if (upd && vobj && (vobj->getPCode() == LL_PCODE_VOLUME))
            {
                if(vobj->getAvatar() && vobj->getAvatar()->isTooComplex() && vobj->getAvatar()->isTooSlow())
                {
                    upd = false;
                }

                LLVOVolume* vvo = (LLVOVolume *)vobj;
                if (!LLPipeline::sRenderAttachedParticles && vvo && vvo->isAttachment())
                {
                    upd = false;
                }
            }

            if (upd)
            {
                if (gpu_particles)
                {
                    // Every source type feeds the GPU emitter table via its
                    // virtual buildGPUEmitter (script + the procedural
                    // spiral / beam / chat sources). Types with no GPU
                    // emitter return false and just don't fill their slot.
                    const U32 slot = LLCleanParticleStore::instance().getOrCreateEmitter(
                        mViewerPartSources[i]->getID());
                    if (slot != LLCleanParticleStore::INVALID)
                    {
                        CleanEmitter e;
                        LLUUID img;
                        if (mViewerPartSources[i]->buildGPUEmitter(dt, e, img))
                        {
                            // LL's per-particle shouldAddPart() (count / frame-rate
                            // throttle): the spiral / beam / chat sources only. LL's
                            // script source never asks it (its own throttles -- the
                            // particle limit, the adaptive burst rate, the size /
                            // distance test -- ran in buildGPUEmitter, #693).
                            if (mViewerPartSources[i]->getType() != LLViewerPartSource::LL_PART_SOURCE_SCRIPT)
                            {
                                e.misc[2] = thinSpawn(e.misc[2], gpuShouldAddFraction());
                            }
                            if (LLCleanParticleStore::auditEnabled())
                            {
                                LLCleanParticleStore::instance().auditFed(mViewerPartSources[i]->getID(), e.misc[2]);
                            }
                            LLCleanParticleStore::instance().setEmitter(slot, e);
                            LLCleanParticleStore::instance().setEmitterTexture(slot, img);
                            // Who emits from this slot (particle pick: block particle owner).
                            const LLViewerObject* src_obj = mViewerPartSources[i]->mSourceObjectp;
                            LLCleanParticleStore::instance().setEmitterOwner(slot, mViewerPartSources[i]->getID(),
                                mViewerPartSources[i]->getOwnerUUID(), src_obj ? src_obj->getID() : LLUUID::null);
                            // PARTICLE FOG (#1003): the source's classification inputs (script
                            // sources; the others clear the slot's).
                            CleanPartFogDesc fog;
                            if (mViewerPartSources[i]->buildGPUFogDesc(fog))
                                LLCleanParticleStore::instance().setEmitterFog(slot, fog);
                            else
                                LLCleanParticleStore::instance().clearEmitterFog(slot);
                        }
                    }
                }
                else
                {
                    mViewerPartSources[i]->update(dt);
                }
            }
        }

        if (mViewerPartSources[i]->isDead())
        {
            mViewerPartSources.erase(mViewerPartSources.begin() + i);
            count--;
            i+=deldir;
        }
        else
        {
             i += dir;
        }
        num_updates++;
    }

    if (gpu_particles)
    {
        // The VK renderer resolves emitter textures and streams the frame's emitter
        // descriptors itself; skip the entire CPU group simulation + adaptive-rate
        // bookkeeping below.
        static LLFrameTimer s_log;
        if (s_log.getElapsedTimeF32() > 2.f)
        {
            s_log.reset();
            const LLCleanParticleStore::Stats st = LLCleanParticleStore::instance().stats();
            LL_INFOS("CleanParticle") << "fed " << LLCleanParticleStore::instance().emitterCount()
                << " emitters (sources=" << mViewerPartSources.size() << ")"
                << " live_particles=" << LLCleanParticleStore::instance().readLiveParticleCount()
                << "/" << LLCleanParticleStore::instance().maxParticles()
                << " slots reclaimed/reused=" << st.reclaimed << "/" << st.reused << LL_ENDL;
        }
        return;
    }

    count = (S32) mViewerPartGroups.size();
    for (i = 0; i < count; i++)
    {
        LLViewerObject* vobj = mViewerPartGroups[i]->mVOPartGroupp;

        S32 visirate = 1;
        if (vobj && !vobj->isDead() && vobj->mDrawable && !vobj->mDrawable->isDead())
        {
            // Off-screen groups update at 1/8 rate. Asks the drawable (its visibility
            // stamp, else its octree group's): HUD particles have no octree (#476).
            if (!vobj->mDrawable->isVisible())
            {
                visirate = 8;
            }
        }

        if ((LLDrawable::getCurrentFrame()+mViewerPartGroups[i]->mID)%visirate == 0)
        {
            if (vobj && !vobj->isDead())
            {
                gPipeline.markRebuild(vobj->mDrawable, LLDrawable::REBUILD_ALL);
            }
            mViewerPartGroups[i]->updateParticles(dt * visirate);
            mViewerPartGroups[i]->mSkippedTime=0.0f;
            if (!mViewerPartGroups[i]->getCount())
            {
                delete mViewerPartGroups[i];
                mViewerPartGroups.erase(mViewerPartGroups.begin() + i);
                i--;
                count--;
            }
        }
        else
        {
            mViewerPartGroups[i]->mSkippedTime+=dt;
        }

    }
    updateAdaptiveRate(sParticleCount);
    updatePartBurstRate(sParticleCount) ;

    //LL_INFOS() << "Particles: " << sParticleCount << " Adaptive Rate: " << sParticleAdaptiveRate << LL_ENDL;
}

void LLViewerPartSim::updateAdaptiveRate(S32 count)
{
    if (LLDrawable::getCurrentFrame()%16==0)
    {
        if (count > sMaxParticleCount * 0.875f
            && sParticleAdaptiveRate < 2.0f)
        {
            sParticleAdaptiveRate *= PART_ADAPT_RATE_MULT;
        }
        else
        {
            if (count < sMaxParticleCount * 0.5f
                && sParticleAdaptiveRate > 0.03125f)
            {
                sParticleAdaptiveRate *= PART_ADAPT_RATE_MULT_RECIP;
            }
        }
    }
}

void LLViewerPartSim::updatePartBurstRate(S32 count)
{
    if (!(LLDrawable::getCurrentFrame() & 0xf))
    {
        if (count >= MAX_PART_COUNT) //set rate to zero
        {
            sParticleBurstRate = 0.0f ;
        }
        else if(count > 0)
        {
            if(sParticleBurstRate > 0.0000001f)
            {
                F32 total_particles = count / sParticleBurstRate ; //estimated
                F32 new_rate = llclamp(0.9f * sMaxParticleCount / total_particles, 0.0f, 1.0f) ;
                F32 delta_rate_threshold = llmin(0.1f * llmax(new_rate, sParticleBurstRate), 0.1f) ;
                F32 delta_rate = llclamp(new_rate - sParticleBurstRate, -1.0f * delta_rate_threshold, delta_rate_threshold) ;

                sParticleBurstRate = llclamp(sParticleBurstRate + 0.5f * delta_rate, 0.0f, 1.0f) ;
            }
            else
            {
                sParticleBurstRate += 0.0000001f ;
            }
        }
        else
        {
            sParticleBurstRate += 0.00125f ;
        }
    }
}

void LLViewerPartSim::addPartSource(LLPointer<LLViewerPartSource> sourcep)
{
    if (!sourcep)
    {
        LL_WARNS() << "Null part source!" << LL_ENDL;
        return;
    }
    sourcep->setStart() ;
    mViewerPartSources.push_back(sourcep);
}

void LLViewerPartSim::removeLastCreatedSource()
{
    mViewerPartSources.pop_back();
}

void LLViewerPartSim::cleanupRegion(LLViewerRegion *regionp)
{
    for (group_list_t::iterator i = mViewerPartGroups.begin(); i != mViewerPartGroups.end(); )
    {
        group_list_t::iterator iter = i++;

        if ((*iter)->getRegion() == regionp)
        {
            delete *iter;
            i = mViewerPartGroups.erase(iter);
        }
    }
}

void LLViewerPartSim::clearParticlesByID(const U32 system_id)
{
    for (group_list_t::iterator g = mViewerPartGroups.begin(); g != mViewerPartGroups.end(); ++g)
    {
        (*g)->removeParticlesByID(system_id);
    }
    for (source_list_t::iterator i = mViewerPartSources.begin(); i != mViewerPartSources.end(); ++i)
    {
        if ((*i)->getID() == system_id)
        {
            (*i)->setDead();
            break;
        }
    }

}

void LLViewerPartSim::clearParticlesByOwnerID(const LLUUID& task_id)
{
    for (source_list_t::iterator iter = mViewerPartSources.begin(); iter != mViewerPartSources.end(); ++iter)
    {
        if ((*iter)->getOwnerUUID() == task_id)
        {
            clearParticlesByID((*iter)->getID());
        }
    }
}
