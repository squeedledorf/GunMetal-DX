/**
 * @file lldrawable.cpp
 * @brief LLDrawable class implementation
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

#include "llviewerprecompiledheaders.h"

#include "lldrawable.h"

// library includes
#include "material_codes.h"

// viewer includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "llface.h"
#include "lllightconstants.h"
#include "llmatrix4a.h"
#include "llsky.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "llvosurfacepatch.h" // for debugging
#include "llworld.h"
#include "pipeline.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvocache.h"
#include "llcontrolavatar.h"
#include "llcleaningest.h"

const F32 MIN_INTERPOLATE_DISTANCE_SQUARED = 0.001f * 0.001f;
const F32 MAX_INTERPOLATE_DISTANCE_SQUARED = 10.f * 10.f;
const F32 OBJECT_DAMPING_TIME_CONSTANT = 0.06f;

extern bool gShiftFrame;

// Moving linksets (#477 worn / HUD attachments and animesh, #480 in-world vehicles and
// scripted linksets) are posed by the clean pipeline (joint table, HUD attach point,
// control avatar, the root's live matrix); no spatial bridge carries them any more. A
// linkset's bridge used to send the clean pipeline its root-moved notice (markMoved of
// the bridge from updateXform, and LLSpatialBridge::move when a member re-binned); that
// notice is sent directly: the root of an ACTIVE volume linkset.
static void note_linkset_moved(LLDrawable* drawable)
{
    LLDrawable* root = drawable->getRoot();
    if (root && root->getVObj())
    {
        LLCleanIngest::instance().noteObjectMoved(root->getVObj());
    }
}


////////////////////////
//
// Inline implementations.
//
//



//////////////////////////////
//
// Drawable code
//
//

// static
U32 LLDrawable::sNumZombieDrawables = 0;
F32 LLDrawable::sCurPixelAngle = 0;
std::vector<LLPointer<LLDrawable> > LLDrawable::sDeadList;

// static
void LLDrawable::incrementVisible()
{
    LLSpatialEntryData::incrementVisible();
    sCurPixelAngle = (F32) gViewerWindow->getWindowHeightRaw()/LLViewerCamera::getInstance()->getView();
}

LLDrawable::LLDrawable(LLViewerObject *vobj)
:   LLSpatialEntryData(LLSpatialEntry::LLDRAWABLE),
    mVObjp(vobj)
{
    init();
}

void LLDrawable::init()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    // mXform
    mParent = NULL;
    mRenderType = 0;
    mCurrentScale = LLVector3(1,1,1);
    mDistanceWRTCamera = 0.0f;
    mState     = 0;

    // mFaces
    mRadius = 0.f;
    mGeneration = -1;

    LLSpatialEntry* entry = NULL;
    LLVOCacheEntry* vo_entry = NULL;
    if(mVObjp && getRegion() != NULL)
    {
        vo_entry = getRegion()->getCacheEntryForDrawable(mVObjp->getLocalID());
        if(vo_entry)
        {
            entry = vo_entry->getEntry();
        }
    }
    setSpatialEntry(entry);
    if(vo_entry)
    {
        if(!entry)
        {
            vo_entry->setSpatialEntry(mEntry);
        }

        getRegion()->addActiveCacheEntry(vo_entry);

        if(vo_entry->getNumOfChildren() > 0)
        {
            getRegion()->addVisibleChildCacheEntry(vo_entry, NULL); //to load all children.
        }

        llassert(!vo_entry->hasState(LLVOCacheEntry::IN_VO_TREE)); //not in the object cache grid.
    }

    llassert(!vo_entry || vo_entry->getEntry() == mEntry);

    initVisible(sCurVisible - 2);//invisible for the current frame and the last frame.
}

void LLDrawable::unload()
{
    LLVOVolume *pVVol = getVOVolume();
    pVVol->setNoLOD();
    pVVol->markForUpdate();
}

// static
void LLDrawable::initClass()
{
}


void LLDrawable::destroy()
{
    if (isDead())
    {
        sNumZombieDrawables--;
    }

    std::for_each(mFaces.begin(), mFaces.end(), DeletePointer());
    mFaces.clear();


    /*if (!(sNumZombieDrawables % 10))
    {
        LL_INFOS() << "- Zombie drawables: " << sNumZombieDrawables << LL_ENDL;
    }*/

}

void LLDrawable::markDead()
{
    if (isDead())
    {
        LL_WARNS() << "Warning!  Marking dead multiple times!" << LL_ENDL;
        return;
    }
    setState(DEAD);

    sNumZombieDrawables++;

    // We're dead.  Free up all of our references to other objects
    cleanupReferences();
//  sDeadList.push_back(this);
}

LLVOVolume* LLDrawable::getVOVolume() const
{
    LLViewerObject* objectp = mVObjp;
    if ( !isDead() && objectp && (objectp->getPCode() == LL_PCODE_VOLUME))
    {
        return ((LLVOVolume*)objectp);
    }
    else
    {
        return NULL;
    }
}

const LLMatrix4& LLDrawable::getRenderMatrix() const
{
    return isRoot() ? getWorldMatrix() : getParent()->getWorldMatrix();
}

bool LLDrawable::isLight() const
{
    LLViewerObject* objectp = mVObjp;
    if ( objectp && (objectp->getPCode() == LL_PCODE_VOLUME) && !isDead())
    {
        return ((LLVOVolume*)objectp)->getIsLight();
    }
    else
    {
        return false;
    }
}

void LLDrawable::cleanupReferences()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    std::for_each(mFaces.begin(), mFaces.end(), DeletePointer());
    mFaces.clear();

    detachSpatialEntry();

    // Cleanup references to other objects
    mVObjp = NULL;
    mParent = NULL;
}

void LLDrawable::detachSpatialEntry()
{
    if(!mEntry)
    {
        return;
    }

    mEntry->removeData(this);
    if(mEntry->hasVOCacheEntry())
    {
        getRegion()->removeActiveCacheEntry((LLVOCacheEntry*)mEntry->getVOCacheEntry(), this);
    }
    mEntry = NULL;
}

void LLDrawable::cleanupDeadDrawables()
{
    sDeadList.clear();
}

S32 LLDrawable::findReferences(LLDrawable *drawablep)
{
    S32 count = 0;
    if (mParent == drawablep)
    {
        LL_INFOS() << this << ": parent reference" << LL_ENDL;
        count++;
    }
    return count;
}

LLFace* LLDrawable::addFace(LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    {
        face = new LLFace(this, mVObjp);
    }

    if (!face) LL_ERRS() << "Allocating new Face: " << mFaces.size() << LL_ENDL;

    if (face)
    {
        mFaces.push_back(face);

        if (poolp)
        {
            face->setPool(poolp, texturep);
        }

        if (isState(UNLIT))
        {
            face->setState(LLFace::FULLBRIGHT);
        }
    }
    return face;
}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;

    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setNormalMap(normalp);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp, LLViewerTexture *specularp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setNormalMap(normalp);
    face->setSpecularMap(specularp);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

void LLDrawable::setNumFaces(const S32 newFaces, LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (newFaces == (S32)mFaces.size())
    {
        return;
    }
    else if (newFaces < (S32)mFaces.size())
    {
        std::for_each(mFaces.begin() + newFaces, mFaces.end(), DeletePointer());
        mFaces.erase(mFaces.begin() + newFaces, mFaces.end());
    }
    else // (newFaces > mFaces.size())
    {
        mFaces.reserve(newFaces);
        for (auto i = mFaces.size(); i<newFaces; i++)
        {
            addFace(poolp, texturep);
        }
    }

    llassert_always(mFaces.size() == newFaces);
}

void LLDrawable::setNumFacesFast(const S32 newFaces, LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (newFaces <= (S32)mFaces.size() && newFaces >= (S32)mFaces.size()/2)
    {
        return;
    }
    else if (newFaces < (S32)mFaces.size())
    {
        std::for_each(mFaces.begin() + newFaces, mFaces.end(), DeletePointer());
        mFaces.erase(mFaces.begin() + newFaces, mFaces.end());
    }
    else // (newFaces > mFaces.size())
    {
        mFaces.reserve(newFaces);
        for (auto i = mFaces.size(); i<newFaces; i++)
        {
            addFace(poolp, texturep);
        }
    }

    llassert_always(mFaces.size() == newFaces) ;
}

void LLDrawable::mergeFaces(LLDrawable* src)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    auto face_count = mFaces.size() + src->mFaces.size();

    mFaces.reserve(face_count);
    for (size_t i = 0; i < src->mFaces.size(); i++)
    {
        LLFace* facep = src->mFaces[i];
        facep->setDrawable(this);
        mFaces.push_back(facep);
    }
    src->mFaces.clear();
}

void LLDrawable::deleteFaces(S32 offset, S32 count)
{
    face_list_t::iterator face_begin = mFaces.begin() + offset;
    face_list_t::iterator face_end = face_begin + count;

    std::for_each(face_begin, face_end, DeletePointer());
    mFaces.erase(face_begin, face_end);
}

void LLDrawable::update()
{
    LL_ERRS() << "Shouldn't be called!" << LL_ENDL;
}


void LLDrawable::updateMaterial()
{
}

void LLDrawable::makeActive()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

#if !LL_RELEASE_FOR_DOWNLOAD
    if (mVObjp.notNull())
    {
        U32 pcode = mVObjp->getPCode();
        if (pcode == LLViewerObject::LL_VO_WATER ||
            pcode == LLViewerObject::LL_VO_VOID_WATER ||
            pcode == LLViewerObject::LL_VO_SURFACE_PATCH ||
            pcode == LLViewerObject::LL_VO_PART_GROUP ||
            pcode == LLViewerObject::LL_VO_HUD_PART_GROUP ||
            pcode == LLViewerObject::LL_VO_SKY)
        {
            LL_ERRS() << "Static viewer object has active drawable!" << LL_ENDL;
        }
    }
#endif

    if (!isState(ACTIVE)) // && mGeneration > 0)
    {
        setState(ACTIVE);
        clearState(SPATIAL_BOUND);   // its extents become root-relative on the next bound

        //parent must be made active first
        if (!isRoot() && !mParent->isActive())
        {
            mParent->makeActive();
            //NOTE: linked set will now NEVER become static
            mParent->setState(LLDrawable::ACTIVE_CHILD);
        }

        //all child objects must also be active
        llassert_always(mVObjp);

        LLViewerObject::const_child_list_t& child_list = mVObjp->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); iter++)
        {
            LLViewerObject* child = *iter;
            LLDrawable* drawable = child->mDrawable;
            if (drawable)
            {
                drawable->makeActive();
            }
        }

        if (mVObjp->getPCode() == LL_PCODE_VOLUME)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_VOLUME);
        }
        updatePartition();
    }
    else if (!isRoot() && !mParent->isActive()) //this should not happen, but occasionally it does...
    {
        mParent->makeActive();
        //NOTE: linked set will now NEVER become static
        mParent->setState(LLDrawable::ACTIVE_CHILD);
    }

    llassert(isAvatar() || isRoot() || mParent->isActive());
}


void LLDrawable::makeStatic(bool warning_enabled)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (isState(ACTIVE) &&
        !isState(ACTIVE_CHILD) &&
        !mVObjp->isAttachment() &&
        !mVObjp->isFlexible() &&
        !mVObjp->isAnimatedObject())
    {
        clearState(ACTIVE | ANIMATED_CHILD | SPATIAL_BOUND);

        //drawable became static with active parent, not acceptable
        llassert(mParent.isNull() || !mParent->isActive() || !warning_enabled);

        LLViewerObject::const_child_list_t& child_list = mVObjp->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); iter++)
        {
            LLViewerObject* child = *iter;
            LLDrawable* child_drawable = child->mDrawable;
            if (child_drawable)
            {
                if (child_drawable->getParent() != this)
                {
                    LL_WARNS() << "Child drawable has unknown parent." << LL_ENDL;
                }
                child_drawable->makeStatic(warning_enabled);
            }
        }

        if (mVObjp->getPCode() == LL_PCODE_VOLUME)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_VOLUME);
        }

        updatePartition();
    }

    llassert(isAvatar() || isRoot() || mParent->isStatic());
}

// Returns "distance" between target destination and resulting xfrom
F32 LLDrawable::updateXform(bool undamped)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    bool damped = !undamped;

    // Position
    const LLVector3 old_pos(mXform.getPosition());
    LLVector3 target_pos;
    if (mXform.isRoot())
    {
        // get root position in your agent's region
        target_pos = mVObjp->getPositionAgent();
    }
    else
    {
        // parent-relative position
        target_pos = mVObjp->getPosition();
    }

    // Rotation
    const LLQuaternion old_rot(mXform.getRotation());
    LLQuaternion target_rot = mVObjp->getRotation();
    //scaling
    LLVector3 target_scale = mVObjp->getScale();
    LLVector3 old_scale = mCurrentScale;

    // Damping
    F32 dist_squared = 0.f;
    F32 camdist2 = (mDistanceWRTCamera * mDistanceWRTCamera);

    if (damped && isVisible())
    {
        F32 lerp_amt = llclamp(LLSmoothInterpolation::getInterpolant(OBJECT_DAMPING_TIME_CONSTANT), 0.f, 1.f);
        LLVector3 new_pos = lerp(old_pos, target_pos, lerp_amt);
        dist_squared = dist_vec_squared(new_pos, target_pos);

        LLQuaternion new_rot = nlerp(lerp_amt, old_rot, target_rot);
        // FIXME: This can be negative! It is be possible for some rots to 'cancel out' pos or size changes.
        dist_squared += (1.f - dot(new_rot, target_rot)) * 10.f;

        LLVector3 new_scale = lerp(old_scale, target_scale, lerp_amt);
        dist_squared += dist_vec_squared(new_scale, target_scale);

        if ((dist_squared >= MIN_INTERPOLATE_DISTANCE_SQUARED * camdist2) &&
            (dist_squared <= MAX_INTERPOLATE_DISTANCE_SQUARED))
        {
            // interpolate
            target_pos = new_pos;
            target_rot = new_rot;
            target_scale = new_scale;
        }
        else if (mVObjp->getAngularVelocity().isExactlyZero())
        {
            // snap to final position (only if no target omega is applied)
            dist_squared = 0.0f;
            //set target scale here, because of dist_squared = 0.0f remove object from move list
            mCurrentScale = target_scale;

            if (getVOVolume() && !isRoot())
            { //child prim snapping to some position, needs a rebuild
                gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
            }
        }
    }
    else
    {
        // The following fixes MAINT-1742 but breaks vehicles similar to MAINT-2275
        // dist_squared = dist_vec_squared(old_pos, target_pos);

        // The following fixes MAINT-2247 but causes MAINT-2275
        //dist_squared += (1.f - dot(old_rot, target_rot)) * 10.f;
        //dist_squared += dist_vec_squared(old_scale, target_scale);
    }

    const LLVector3 vec = mCurrentScale-target_scale;

    //It's a very important on each cycle on Drawable::update form(), when object remained in move
    //, list update the CurrentScale member, because if do not do that, it remained in this list forever
    //or when the delta time between two frames a become a sufficiently large (due to interpolation)
    //for overcome the MIN_INTERPOLATE_DISTANCE_SQUARED.
    mCurrentScale = target_scale;

    if (vec*vec > MIN_INTERPOLATE_DISTANCE_SQUARED)
    {
        // LL: "scale change requires immediate rebuild" (GL baked scale into the
        // vertices). VK keeps scale in the instance matrix only (planar texgen derives
        // it there too), and markRebuild is purely a re-ingest request -- issuing it
        // every frame of a damped scale ease re-registered every face per frame. The
        // per-frame mover refresh (updateMovedList -> noteObjectMoved) re-poses it with
        // the eased scale instead; the setScale that started the ease already requested
        // one re-ingest at the target scale (#180). Branch kept so the else-chain below
        // behaves as before.
    }
    else if (!isRoot() &&
         (!mVObjp->getAngularVelocity().isExactlyZero() ||
            dist_squared > 0.f))
    { //child prim moving relative to parent, tag as needing to be rendered atomically and rebuild
        dist_squared = 1.f; //keep this object on the move list
        if (!isState(LLDrawable::ANIMATED_CHILD))
        {
            setState(LLDrawable::ANIMATED_CHILD);
            gPipeline.markRebuild(this, LLDrawable::REBUILD_ALL);
        }
    }
    else if (!isRoot() &&
            ((dist_vec_squared(old_pos, target_pos) > 0.f)
            || (1.f - dot(old_rot, target_rot)) > 0.f))
    { //fix for BUG-840, MAINT-2275, MAINT-1742, MAINT-2247
        mVObjp->shrinkWrap();
        gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
    }
    else if (!getVOVolume() && !isAvatar())
    {
        movePartition();
    }

    // Update
    mXform.setPosition(target_pos);
    mXform.setRotation(target_rot);
    mXform.setScale(LLVector3(1,1,1)); //no scale in drawable transforms (IT'S A RULE!)
    mXform.updateMatrix();
    if (isRoot() && mVObjp->isAnimatedObject() && mVObjp->getControlAvatar())
    {
        mVObjp->getControlAvatar()->matchVolumeTransform();
    }

    if (isRoot() && isActive() && getVOVolume())
    {
        // A moving linkset's root (see note_linkset_moved): the notice its bridge's
        // markMoved gave.
        LLCleanIngest::instance().noteObjectMoved(mVObjp);
    }
    return dist_squared;
}

void LLDrawable::setRadius(F32 radius)
{
    if (mRadius != radius)
    {
        mRadius = radius;
    }
}

void LLDrawable::moveUpdatePipeline(bool moved)
{
    if (moved)
    {
        makeActive();
    }

    // Update the face centers.
    for (S32 i = 0; i < getNumFaces(); i++)
    {
        LLFace* face = getFace(i);
        if (face)
        {
            face->updateCenterAgent();
        }
    }
}

void LLDrawable::movePartition()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    // No spatial partitions (#476, #477, #480). What re-binning a drawable did besides the
    // octree itself: refresh its entry data (extents, centre, bin radius -- what
    // LLSpatialPartition::put / updateInGroup refreshed on the way in), which the object
    // cache, LOD distance, texture streaming and picking read. Objects LL never binned
    // (PARTITION_NONE: sky, ...) are left alone, as before.
    if (mVObjp.isNull() || mVObjp->getPartitionType() == LLViewerRegion::PARTITION_NONE)
    {
        return;
    }

    updateSpatialExtents();
    setState(SPATIAL_BOUND);

    if (isActive() && getVOVolume())
    {
        // A moving linkset member: the root-moved notice its linkset's bridge sent from
        // LLSpatialBridge::move.
        note_linkset_moved(this);
    }
}

bool LLDrawable::updateMove()
{
    if (isDead())
    {
        LL_WARNS() << "Update move on dead drawable!" << LL_ENDL;
        return true;
    }

    if (mVObjp.isNull())
    {
        return false;
    }

    makeActive();

    // #3256 force undampened movement for attached objects in mouselook
    // to prevent animation bork for linkset with animated parts
    if (!isRoot() && gAgentCamera.cameraMouselook() &&
        !mVObjp->isRiggedMesh() && mVObjp->getAvatar() && mVObjp->getAvatar()->isSelf())
    {
        return updateMoveUndamped();
    }

    return isState(MOVE_UNDAMPED) ? updateMoveUndamped() : updateMoveDamped();
}

bool LLDrawable::updateMoveUndamped()
{
    F32 dist_squared = updateXform(true);

    mGeneration++;

    if (!isState(LLDrawable::INVISIBLE))
    {
        bool moved = (dist_squared > 0.001f && dist_squared < 255.99f);
        moveUpdatePipeline(moved);
        mVObjp->updateText();
    }

    mVObjp->clearChanged(LLXform::MOVED);
    return true;
}

void LLDrawable::updatePartition()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (!getVOVolume())
    {
        movePartition();
    }
    else
    {
        //a child prim moved and needs its verts regenerated
        gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
    }
}

bool LLDrawable::updateMoveDamped()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    F32 dist_squared = updateXform(false);

    mGeneration++;

    if (!isState(LLDrawable::INVISIBLE))
    {
        bool moved = (dist_squared > 0.001f && dist_squared < 128.0f);
        moveUpdatePipeline(moved);
        mVObjp->updateText();
    }

    bool done_moving = dist_squared == 0.0f;

    if (done_moving)
    {
        mVObjp->clearChanged(LLXform::MOVED);
    }

    return done_moving;
}

void LLDrawable::updateDistance(LLCamera& camera, bool force_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        LL_WARNS() << "Attempted to update distance for non-world camera." << LL_ENDL;
        return;
    }

    if (gShiftFrame)
    {
        return;
    }

    LLVector3 pos;

    {
        LLVOVolume* volume = getVOVolume();
        if (volume)
        {
            // LL measured from the bbox centre while the drawable sat in an octree group. A
            // static volume's centre is agent space; an active one's is root-relative (its
            // bridge transformed the camera into that frame -- a transform nothing applies
            // any more), so a moving linkset measures from its agent position, as worn
            // attachments always have (#480).
            if (isStatic() && isState(SPATIAL_BOUND))
            {
                pos.set(getPositionGroup().getF32ptr());
            }
            else
            {
                pos = getPositionAgent();
            }

            if (isState(LLDrawable::HAS_ALPHA))
            {
                for (S32 i = 0; i < getNumFaces(); i++)
                {
                    LLFace* facep = getFace(i);
                    if (facep &&
                        (force_update || facep->isInAlphaPool()))
                    {
                        LLVector4a box;
                        box.setSub(facep->mExtents[1], facep->mExtents[0]);
                        box.mul(0.25f);
                        LLVector3 v = (facep->mCenterLocal-camera.getOrigin());
                        const LLVector3& at = camera.getAtAxis();
                        for (U32 j = 0; j < 3; j++)
                        {
                            v.mV[j] -= box[j] * at.mV[j];
                        }
                        facep->mDistance = v * camera.getAtAxis();
                    }
                }
            }


            // MAINT-7926 Handle volumes in an animated object as a special case
            // SL-937: add dynamic box handling for rigged mesh on regular avatars.
            //if (volume->getAvatar() && volume->getAvatar()->isControlAvatar())
            if (volume->getAvatar())
            {
                const LLVector3* av_box = volume->getAvatar()->getLastAnimExtents();
                LLVector3 cam_pos_from_agent = LLViewerCamera::getInstance()->getOrigin();
                LLVector3 cam_to_box_offset = point_to_box_offset(cam_pos_from_agent, av_box);
                mDistanceWRTCamera = llmax(0.01f, ll_round(cam_to_box_offset.magVec(), 0.01f));
                mVObjp->updateLOD();
                return;
            }
        }
        else
        {
            pos = LLVector3(getPositionGroup().getF32ptr());
        }

        pos -= camera.getOrigin();
        mDistanceWRTCamera = ll_round(pos.magVec(), 0.01f);
        mVObjp->updateLOD();
    }
}

void LLDrawable::updateTexture()
{
    if (isDead())
    {
        LL_WARNS() << "Dead drawable updating texture!" << LL_ENDL;
        return;
    }

    if (getNumFaces() != mVObjp->getNumTEs())
    { //drawable is transitioning its face count
        return;
    }

    if (getVOVolume())
    {
        gPipeline.markRebuild(this, LLDrawable::REBUILD_MATERIAL);
    }
}

void LLDrawable::shiftPos(const LLVector4a &shift_vector)
{
    if (isDead())
    {
        LL_WARNS() << "Shifting dead drawable" << LL_ENDL;
        return;
    }

    if (mParent)
    {
        mXform.setPosition(mVObjp->getPosition());
    }
    else
    {
        mXform.setPosition(mVObjp->getPositionAgent());
    }

    mXform.updateMatrix();

    if (isStatic())
    {
        LLVOVolume* volume = getVOVolume();

        bool rebuild = (!volume &&
                        getRenderType() != LLPipeline::RENDER_TYPE_TREE &&
                        getRenderType() != LLPipeline::RENDER_TYPE_TERRAIN &&
                        getRenderType() != LLPipeline::RENDER_TYPE_SKY);

        if (rebuild)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_ALL);
        }

        for (S32 i = 0; i < getNumFaces(); i++)
        {
            LLFace *facep = getFace(i);
            if (facep)
            {
                facep->mCenterAgent += LLVector3(shift_vector.getF32ptr());
                facep->mExtents[0].add(shift_vector);
                facep->mExtents[1].add(shift_vector);

            }
        }

        shift(shift_vector);
    }
    else if (isAvatar())
    {
        shift(shift_vector);
    }

    mVObjp->onShift(shift_vector);
}

const LLVector3& LLDrawable::getBounds(LLVector3& min, LLVector3& max) const
{
    mXform.getMinMax(min,max);
    return mXform.getPositionW();
}

void LLDrawable::updateSpatialExtents()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (mVObjp)
    {
        const LLVector4a* exts = getSpatialExtents();
        LLVector4a extents[2] = { exts[0], exts[1] };

        mVObjp->updateSpatialExtents(extents[0], extents[1]);
        setSpatialExtents(extents[0], extents[1]);
    }

    updateBinRadius();
}


void LLDrawable::updateBinRadius()
{
    if (mVObjp.notNull())
    {
        setBinRadius(llmin(mVObjp->getBinRadius(), 256.f));
    }
    else
    {
        setBinRadius(llmin(getRadius()*4.f, 256.f));
    }
}

void LLDrawable::updateSpecialHoverCursor(bool enabled)
{
    // TODO: maintain a list of objects that have special
    // hover cursors, then use that list for per-frame
    // hover cursor selection. JC
}

F32 LLDrawable::getVisibilityRadius() const
{
    if (isDead())
    {
        return 0.f;
    }

    if (isLight())
    {
        if (const LLVOVolume* vov = getVOVolume())
        {
            return llmax(getRadius(), vov->getLightRadius());
        }
        // LL_WARNS() ?
    }

    return getRadius();
}

void LLDrawable::updateUVMinMax()
{
}

bool LLDrawable::isRecentlyVisible() const
{
    // Currently visible or visible in the previous frame.
    if (LLSpatialEntryData::isRecentlyVisible())
    {
        return true;
    }

    const U32 MIN_VIS_FRAME_RANGE = 2 ; // Two frames: the current one and the last one.
    if (sCurVisible - getVisible() < MIN_VIS_FRAME_RANGE)
    {
        return true;
    }

    return false;
}

const LLVector3 LLDrawable::getPositionAgent() const
{
    if (getVOVolume())
    {
        if (isActive())
        {
            LLVector3 pos(0,0,0);
            if (!isRoot())
            {
                pos = mVObjp->getPosition();
            }
            return pos * getRenderMatrix();
        }
        else
        {
            return mVObjp->getPositionAgent();
        }
    }
    else
    {
        return getWorldPosition();
    }
}

bool LLDrawable::isAnimating() const
{
    if (!getVObj())
    {
        return true;
    }

    if (getScale() != mVObjp->getScale())
    {
        return true;
    }

    if (mVObjp->getPCode() == LLViewerObject::LL_VO_PART_GROUP)
    {
        return true;
    }
    if (mVObjp->getPCode() == LLViewerObject::LL_VO_HUD_PART_GROUP)
    {
        return true;
    }

    /*if (!isRoot() && !mVObjp->getAngularVelocity().isExactlyZero())
    { //target omega
        return true;
    }*/

    return false;
}

void LLDrawable::updateFaceSize(S32 idx)
{
    if (mVObjp.notNull())
    {
        mVObjp->updateFaceSize(idx);
    }
}

LLDrawable* LLDrawable::getRoot()
{
    LLDrawable* ret = this;
    while (!ret->isRoot())
    {
        ret = ret->getParent();
    }

    return ret;
}
