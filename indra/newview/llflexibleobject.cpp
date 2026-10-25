/**
 * @file llflexibleobject.cpp
 * @brief Flexible object implementation
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
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

#include "pipeline.h"
#include "llface.h"
#include "llflexibleobject.h"
#include "llwin32headers.h"
#include "llviewerobject.h"
#include "llagent.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "llvoavatar.h"

static const F32 SEC_PER_FLEXI_FRAME = 1.f / 60.f; // 60 flexi updates per second
/*static*/ F32 LLVolumeImplFlexible::sUpdateFactor = 1.0f;

// LLFlexibleObjectData::pack/unpack now in llprimitive.cpp

//-----------------------------------------------
// constructor
//-----------------------------------------------
LLVolumeImplFlexible::LLVolumeImplFlexible(LLViewerObject* vo, LLFlexibleObjectData* attributes) :
        mVO(vo),
        mAttributes(attributes),
        mLastFrameNum(0),
        mLastUpdatePeriod(0)
{
    static U32 seed = 0;
    mID = seed++;
    mInitialized = false;
    mUpdated = false;
    mInitializedRes = -1;
    mSimulateRes = 0;
    mCollisionSphereRadius = 0.f;
    mRenderRes = -1;

    if(mVO->mDrawable.notNull())
    {
        mVO->mDrawable->makeActive() ;
    }
}//-----------------------------------------------

LLVolumeImplFlexible::~LLVolumeImplFlexible()
{
}

LLVector3 LLVolumeImplFlexible::getFramePosition() const
{
    return mVO->getRenderPosition();
}

LLQuaternion LLVolumeImplFlexible::getFrameRotation() const
{
    return mVO->getRenderRotation();
}

void LLVolumeImplFlexible::onParameterChanged(U16 param_type, LLNetworkData *data, bool in_use, bool local_origin)
{
    if (param_type == LLNetworkData::PARAMS_FLEXIBLE)
    {
        mAttributes = (LLFlexibleObjectData*)data;
        setAttributesOfAllSections();
    }
}

void LLVolumeImplFlexible::onShift(const LLVector4a &shift_vector)
{
    //VECTORIZE THIS
    LLVector3 shift(shift_vector.getF32ptr());
    for (int section = 0; section < (1<<FLEXIBLE_OBJECT_MAX_SECTIONS)+1; ++section)
    {
        mSection[section].mPosition += shift;
    }
}

//-----------------------------------------------------------------------------------------------
void LLVolumeImplFlexible::setParentPositionAndRotationDirectly( LLVector3 p, LLQuaternion r )
{
    mParentPosition = p;
    mParentRotation = r;

}//-----------------------------------------------------------------------------------------------------

void LLVolumeImplFlexible::remapSections(LLFlexibleObjectSection *source, S32 source_sections,
                                         LLFlexibleObjectSection *dest, S32 dest_sections)
{
    S32 num_output_sections = 1<<dest_sections;
    LLVector3 scale = mVO->mDrawable->getScale();
    F32 section_length = scale.mV[VZ] / (F32)num_output_sections;
    if (source_sections == -1)
    {
        // Generate all from section 0
        dest[0] = source[0];
        for (S32 section=0; section<num_output_sections; ++section)
        {
            dest[section+1] = dest[section];
            dest[section+1].mPosition += dest[section].mDirection * section_length;
            dest[section+1].mVelocity.setVec( LLVector3::zero );
        }
    }
    else if (source_sections > dest_sections)
    {
        // Copy, skipping sections

        S32 num_steps = 1<<(source_sections-dest_sections);

        // Copy from left to right since it may be an in-place computation
        for (S32 section=0; section<num_output_sections; ++section)
        {
            dest[section+1] = source[(section+1)*num_steps];
        }
        dest[0] = source[0];
    }
    else if (source_sections < dest_sections)
    {
        // Interpolate section info
        // Iterate from right to left since it may be an in-place computation
        S32 step_shift = dest_sections-source_sections;
        S32 num_steps = 1<<step_shift;
        F32 source_section_length = scale.mV[VZ] / (F32)(1<<source_sections);
        for (S32 section=num_output_sections-num_steps; section>=0; section -= num_steps)
        {
            LLFlexibleObjectSection *last_source_section = &source[section>>step_shift];
            LLFlexibleObjectSection *source_section = &source[(section>>step_shift)+1];

            // Cubic interpolation of position
            // At^3 + Bt^2 + Ct + D = f(t)
            LLVector3 D = last_source_section->mPosition;
            LLVector3 C = last_source_section->mdPosition * source_section_length;
            LLVector3 Y = source_section->mdPosition * source_section_length - C; // Helper var
            LLVector3 X = (source_section->mPosition - D - C); // Helper var
            LLVector3 A = Y - 2*X;
            LLVector3 B = X - A;

            F32 t_inc = 1.f/F32(num_steps);
            F32 t = t_inc;
            for (S32 step=1; step<num_steps; ++step)
            {
                dest[section+step].mScale =
                    lerp(last_source_section->mScale, source_section->mScale, t);
                dest[section+step].mAxisRotation =
                    slerp(t, last_source_section->mAxisRotation, source_section->mAxisRotation);

                // Evaluate output interpolated values
                F32 t_sq = t*t;
                dest[section+step].mPosition = t_sq*(t*A + B) + t*C + D;
                dest[section+step].mRotation =
                    slerp(t, last_source_section->mRotation, source_section->mRotation);
                dest[section+step].mVelocity = lerp(last_source_section->mVelocity, source_section->mVelocity, t);
                dest[section+step].mDirection = lerp(last_source_section->mDirection, source_section->mDirection, t);
                dest[section+step].mdPosition = lerp(last_source_section->mdPosition, source_section->mdPosition, t);
                dest[section+num_steps] = *source_section;
                t += t_inc;
            }
        }
        dest[0] = source[0];
    }
    else
    {
        // numbers are equal. copy info
        for (S32 section=0; section <= num_output_sections; ++section)
        {
            dest[section] = source[section];
        }
    }
}

//-----------------------------------------------------------------------------
void LLVolumeImplFlexible::setAttributesOfAllSections(LLVector3* inScale)
{
    LLVector2 bottom_scale, top_scale;
    F32 begin_rot = 0, end_rot = 0;
    if (mVO->getVolume())
    {
        const LLPathParams &params = mVO->getVolume()->getParams().getPathParams();
        bottom_scale = params.getBeginScale();
        top_scale = params.getEndScale();
        begin_rot = F_PI * params.getTwistBegin();
        end_rot = F_PI * params.getTwist();
    }

    if (!mVO->mDrawable)
    {
        return;
    }

    S32 num_sections = 1 << mSimulateRes;

    LLVector3 scale;
    if (inScale == (LLVector3*)NULL)
    {
        scale = mVO->mDrawable->getScale();
    }
    else
    {
        scale = *inScale;
    }

    mSection[0].mPosition = getAnchorPosition();
    mSection[0].mDirection = LLVector3::z_axis * getFrameRotation();
    mSection[0].mdPosition = mSection[0].mDirection;
    mSection[0].mScale.setVec(scale.mV[VX]*bottom_scale.mV[0], scale.mV[VY]*bottom_scale.mV[1]);
    mSection[0].mVelocity.setVec(0,0,0);
    mSection[0].mAxisRotation.setQuat(begin_rot,0,0,1);

    remapSections(mSection, mInitializedRes, mSection, mSimulateRes);
    mInitializedRes = mSimulateRes;

    F32 t_inc = 1.f/F32(num_sections);
    F32 t = t_inc;

    for ( int i=1; i<= num_sections; i++)
    {
        mSection[i].mAxisRotation.setQuat(lerp(begin_rot,end_rot,t),0,0,1);
        mSection[i].mScale = LLVector2(
            scale.mV[VX] * lerp(bottom_scale.mV[0], top_scale.mV[0], t),
            scale.mV[VY] * lerp(bottom_scale.mV[1], top_scale.mV[1], t));
        t += t_inc;
    }
}//-----------------------------------------------------------------------------------


void LLVolumeImplFlexible::onSetVolume(const LLVolumeParams &volume_params, const S32 detail)
{
}


void LLVolumeImplFlexible::updateRenderRes()
{
    if (!mAttributes)
        return;

    LLDrawable* drawablep = mVO->mDrawable;

    S32 new_res = mAttributes->getSimulateLOD();

#if 1 //optimal approximation of previous behavior that doesn't rely on atan2
    F32 app_angle = mVO->getScale().mV[2]/drawablep->mDistanceWRTCamera;

    // Rendering sections increases with visible angle on the screen
    mRenderRes = (S32) (12.f*app_angle);
#else //legacy behavior
    //number of segments only cares about z axis
    F32 app_angle = ll_round((F32) atan2( mVO->getScale().mV[2]*2.f, drawablep->mDistanceWRTCamera) * RAD_TO_DEG, 0.01f);

    // Rendering sections increases with visible angle on the screen
    mRenderRes = (S32)(FLEXIBLE_OBJECT_MAX_SECTIONS*4*app_angle*DEG_TO_RAD/LLViewerCamera::getInstance()->getView());
#endif

    mRenderRes = llclamp(mRenderRes, new_res-1, (S32) FLEXIBLE_OBJECT_MAX_SECTIONS);

    // Throttle back simulation of segments we're not rendering
    if (mRenderRes < new_res)
    {
        new_res = mRenderRes;
    }

    if (!mInitialized || (mSimulateRes != new_res))
    {
        mSimulateRes = new_res;
        setAttributesOfAllSections();
        mInitialized = true;
    }
}
//---------------------------------------------------------------------------------
// This calculates the physics of the flexible object. Note that it has to be 0
// updated every time step. In the future, perhaps there could be an
// optimization similar to what Havok does for objects that are stationary.
//---------------------------------------------------------------------------------
void LLVolumeImplFlexible::doIdleUpdate()
{
    LLDrawable* drawablep = mVO->mDrawable;

    if (drawablep)
    {
        //ensure drawable is active
        drawablep->makeActive();

        if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_FLEXIBLE))
        {
            bool visible = drawablep->isVisible();

            if (mRenderRes == -1)
            {
                updateRenderRes();
                gPipeline.markRebuild(drawablep, LLDrawable::REBUILD_POSITION);
            }
            else
            {
                F32 pixel_area = mVO->getPixelArea();

                // Note: Flexies afar will be rarely updated, closer ones will be updated more frequently.
                // But frequency differences are extremely noticeable, so consider modifying update factor,
                // or at least clamping value a bit more from both sides.
                U32 update_period = (U32) (llmax((S32) (LLViewerCamera::getInstance()->getScreenPixelArea()*0.01f/(pixel_area*(sUpdateFactor+1.f))),0)+1);
                // MAINT-1890 Clamp the update period to ensure that the update_period is no greater than 32 frames
                update_period = llclamp(update_period, 1U, 32U);

                // We control how fast flexies update, buy splitting updates among frames
                U64 virtual_frame_num = (U64)(LLTimer::getElapsedSeconds() / SEC_PER_FLEXI_FRAME);

                if  (visible)
                {
                    if (!drawablep->isState(LLDrawable::IN_REBUILD_Q) &&
                        pixel_area > 256.f)
                    {
                        U32 id;
                        if (mVO->isRootEdit())
                        {
                            id = mID;
                        }
                        else
                        {
                            LLVOVolume* parent = (LLVOVolume*)mVO->getParent();
                            id = parent->getVolumeInterfaceID();
                        }


                        // Throttle flexies and spread load by preventing flexies from updating in same frame
                        // Shows how many frames we need to wait before next update
                        U64 throttling_delay = (virtual_frame_num + id) % update_period;

                        if ((throttling_delay == 0 && mLastFrameNum < virtual_frame_num) //one or more virtual frames per frame
                            || (mLastFrameNum + update_period < virtual_frame_num) // missed virtual frame
                            || mLastFrameNum > virtual_frame_num) // overflow
                        {
                            // We need mLastFrameNum to compensate for 'unreliable time' and to filter 'duplicate' frames
                            // If happened too late, subtract throttling_delay (it is zero otherwise)
                            mLastFrameNum = virtual_frame_num - throttling_delay;

                            // Store update period for updateClass()
                            // Note: Consider substituting update_period with mLastUpdatePeriod everywhere.
                            mLastUpdatePeriod = update_period;

                            updateRenderRes();

                            mVO->shrinkWrap();
                            gPipeline.markRebuild(drawablep, LLDrawable::REBUILD_POSITION);
                        }
                    }
                }
                else
                {
                    mLastFrameNum = virtual_frame_num;
                    mLastUpdatePeriod = update_period;
                }
            }

        }
    }
}

inline S32 log2(S32 x)
{
    S32 ret = 0;
    while (x > 1)
    {
        ++ret;
        x >>= 1;
    }
    return ret;
}

void LLVolumeImplFlexible::doFlexibleUpdate()
{
    LL_PROFILE_ZONE_SCOPED;
    LLVolume* volume = mVO->getVolume();
    LLPath *path = &volume->getPath();
    if ((mSimulateRes == 0 || !mInitialized) && mVO->mDrawable->isVisible())
    {
        bool force_update = mSimulateRes == 0;
        doIdleUpdate();

        if (!force_update || !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_FLEXIBLE))
        {
            return; // we did not get updated or initialized, proceeding without can be dangerous
        }
    }

    if (!mInitialized || !mAttributes)
    {
        //the object is not visible
        return;
    }

    // Fix for MAINT-1894
    // Skipping the flexible update if render res is negative.  If we were to continue with a negative value,
    // the subsequent S32 num_render_sections = 1<<mRenderRes; code will specify a really large number of
    // render sections which will then create a length exception in the std::vector::resize() method.
    if (mRenderRes < 0)
    {
        return;
    }

    S32 num_sections = 1 << mSimulateRes;

    F32 secondsThisFrame = mTimer.getElapsedTimeAndResetF32();
    if (secondsThisFrame > 0.2f)
    {
        secondsThisFrame = 0.2f;
    }

    LLVector3 BasePosition = getFramePosition();
    LLQuaternion BaseRotation = getFrameRotation();
    LLQuaternion parentSegmentRotation = BaseRotation;
    LLVector3 anchorDirectionRotated = LLVector3::z_axis * parentSegmentRotation;
    LLVector3 anchorScale = mVO->mDrawable->getScale();

    F32 section_length = anchorScale.mV[VZ] / (F32)num_sections;
    F32 inv_section_length = 1.f / section_length;

    S32 i;

    // ANCHOR position is offset from BASE position (centroid) by half the length
    LLVector3 AnchorPosition = BasePosition - (anchorScale.mV[VZ]/2 * anchorDirectionRotated);

    mSection[0].mPosition = AnchorPosition;
    mSection[0].mDirection = anchorDirectionRotated;
    mSection[0].mRotation = BaseRotation;

    LLQuaternion deltaRotation;

    LLVector3 lastPosition;

    // Coefficients which are constant across sections
    F32 t_factor = mAttributes->getTension() * 0.1f;
    t_factor = t_factor*(1 - pow(0.85f, secondsThisFrame*30));
    if ( t_factor > FLEXIBLE_OBJECT_MAX_INTERNAL_TENSION_FORCE )
    {
        t_factor = FLEXIBLE_OBJECT_MAX_INTERNAL_TENSION_FORCE;
    }

    F32 friction_coeff = (mAttributes->getAirFriction()*2+1);
    friction_coeff = pow(10.f, friction_coeff*secondsThisFrame);
    friction_coeff = (friction_coeff > 1) ? friction_coeff : 1;
    F32 momentum = 1.0f / friction_coeff;

    F32 wind_factor = (mAttributes->getWindSensitivity()*0.1f) * section_length * secondsThisFrame;
    F32 max_angle = atan(section_length*2.f);

    F32 force_factor = section_length * secondsThisFrame;

    // Update simulated sections
    for (i=1; i<=num_sections; ++i)
    {
        LLVector3 parentSectionVector;
        LLVector3 parentSectionPosition;
        LLVector3 parentDirection;

        //---------------------------------------------------
        // save value of position as lastPosition
        //---------------------------------------------------
        lastPosition = mSection[i].mPosition;

        //------------------------------------------------------------------------------------------
        // gravity
        //------------------------------------------------------------------------------------------
        mSection[i].mPosition.mV[2] -= mAttributes->getGravity() * force_factor;

        //------------------------------------------------------------------------------------------
        // wind force
        //------------------------------------------------------------------------------------------
        if (mAttributes->getWindSensitivity() > 0.001f)
        {
            mSection[i].mPosition += gAgent.getRegion()->mWind.getVelocity( mSection[i].mPosition ) * wind_factor;
        }

        //------------------------------------------------------------------------------------------
        // user-defined force
        //------------------------------------------------------------------------------------------
        mSection[i].mPosition += mAttributes->getUserForce() * force_factor;

        //---------------------------------------------------
        // tension (rigidity, stiffness)
        //---------------------------------------------------
        parentSectionPosition = mSection[i-1].mPosition;
        parentDirection = mSection[i-1].mDirection;

        if ( i == 1 )
        {
            parentSectionVector = mSection[0].mDirection;
        }
        else
        {
            parentSectionVector = mSection[i-2].mDirection;
        }

        LLVector3 currentVector = mSection[i].mPosition - parentSectionPosition;

        LLVector3 difference = (parentSectionVector*section_length) - currentVector;
        LLVector3 tensionForce = difference * t_factor;

        mSection[i].mPosition += tensionForce;

        //------------------------------------------------------------------------------------------
        // sphere collision, currently not used
        //------------------------------------------------------------------------------------------
        /*if ( mAttributes->mUsingCollisionSphere )
        {
            LLVector3 vectorToCenterOfCollisionSphere = mCollisionSpherePosition - mSection[i].mPosition;
            if ( vectorToCenterOfCollisionSphere.magVecSquared() < mCollisionSphereRadius * mCollisionSphereRadius )
            {
                F32 distanceToCenterOfCollisionSphere = vectorToCenterOfCollisionSphere.magVec();
                F32 penetration = mCollisionSphereRadius - distanceToCenterOfCollisionSphere;

                LLVector3 normalToCenterOfCollisionSphere;

                if ( distanceToCenterOfCollisionSphere > 0.0f )
                {
                    normalToCenterOfCollisionSphere = vectorToCenterOfCollisionSphere / distanceToCenterOfCollisionSphere;
                }
                else // rare
                {
                    normalToCenterOfCollisionSphere = LLVector3::x_axis; // arbitrary
                }

                // push the position out to the surface of the collision sphere
                mSection[i].mPosition -= normalToCenterOfCollisionSphere * penetration;
            }
        }*/

        //------------------------------------------------------------------------------------------
        // inertia
        //------------------------------------------------------------------------------------------
        mSection[i].mPosition += mSection[i].mVelocity * momentum;

        //------------------------------------------------------------------------------------------
        // clamp length & rotation
        //------------------------------------------------------------------------------------------
        mSection[i].mDirection = mSection[i].mPosition - parentSectionPosition;
        mSection[i].mDirection.normVec();
        deltaRotation.shortestArc( parentDirection, mSection[i].mDirection );

        F32 angle;
        LLVector3 axis;
        deltaRotation.getAngleAxis(&angle, axis);
        if (angle > F_PI) angle -= 2.f*F_PI;
        if (angle < -F_PI) angle += 2.f*F_PI;
        if (angle > max_angle)
        {
            //angle = 0.5f*(angle+max_angle);
            deltaRotation.setQuat(max_angle, axis);
        } else if (angle < -max_angle)
        {
            //angle = 0.5f*(angle-max_angle);
            deltaRotation.setQuat(-max_angle, axis);
        }
        LLQuaternion segment_rotation = parentSegmentRotation * deltaRotation;
        parentSegmentRotation = segment_rotation;

        mSection[i].mDirection = (parentDirection * deltaRotation);
        mSection[i].mPosition = parentSectionPosition + mSection[i].mDirection * section_length;
        mSection[i].mRotation = segment_rotation;

        if (i > 1)
        {
            // Propogate half the rotation up to the parent
            LLQuaternion halfDeltaRotation(angle/2, axis);
            mSection[i-1].mRotation = mSection[i-1].mRotation * halfDeltaRotation;
        }

        //------------------------------------------------------------------------------------------
        // calculate velocity
        //------------------------------------------------------------------------------------------
        mSection[i].mVelocity = mSection[i].mPosition - lastPosition;
        if (mSection[i].mVelocity.magVecSquared() > 1.f)
        {
            mSection[i].mVelocity.normVec();
        }
    }

    // Calculate derivatives (not necessary until normals are automagically generated)
    mSection[0].mdPosition = (mSection[1].mPosition - mSection[0].mPosition) * inv_section_length;
    // i = 1..NumSections-1
    for (i=1; i<num_sections; ++i)
    {
        // Quadratic numerical derivative of position

        // f(-L1) = aL1^2 - bL1 + c = f1
        // f(0)   =               c = f2
        // f(L2)  = aL2^2 + bL2 + c = f3
        // f = ax^2 + bx + c
        // d/dx f = 2ax + b
        // d/dx f(0) = b

        // c = f2
        // a = [(f1-c)/L1 + (f3-c)/L2] / (L1+L2)
        // b = (f3-c-aL2^2)/L2

        LLVector3 a = (mSection[i-1].mPosition-mSection[i].mPosition +
                    mSection[i+1].mPosition-mSection[i].mPosition) * 0.5f * inv_section_length * inv_section_length;
        LLVector3 b = (mSection[i+1].mPosition-mSection[i].mPosition - a*(section_length*section_length));
        b *= inv_section_length;

        mSection[i].mdPosition = b;
    }

    // i = NumSections
    mSection[i].mdPosition = (mSection[i].mPosition - mSection[i-1].mPosition) * inv_section_length;

    // Create points
    llassert(mRenderRes > -1);
    S32 num_render_sections = 1<<mRenderRes;
    if (path->getPathLength() != num_render_sections+1)
    {
        ((LLVOVolume*) mVO)->mVolumeChanged = true;
        volume->resizePath(num_render_sections+1);
    }

    LLPath::PathPt *new_point;

    LLFlexibleObjectSection newSection[ (1<<FLEXIBLE_OBJECT_MAX_SECTIONS)+1 ];
    remapSections(mSection, mSimulateRes, newSection, mRenderRes);

    //generate transform from global to prim space
    LLVector3 delta_scale = LLVector3(1,1,1);
    LLVector3 delta_pos;
    LLQuaternion delta_rot;

    delta_rot = ~getFrameRotation();
    delta_pos = -getFramePosition()*delta_rot;

    // Vertex transform (4x4)
    LLVector3 x_axis = LLVector3(delta_scale.mV[VX], 0.f, 0.f) * delta_rot;
    LLVector3 y_axis = LLVector3(0.f, delta_scale.mV[VY], 0.f) * delta_rot;
    LLVector3 z_axis = LLVector3(0.f, 0.f, delta_scale.mV[VZ]) * delta_rot;

    LLMatrix4 rel_xform;
    rel_xform.initRows(LLVector4(x_axis, 0.f),
                                LLVector4(y_axis, 0.f),
                                LLVector4(z_axis, 0.f),
                                LLVector4(delta_pos, 1.f));

    LL_CHECK_MEMORY
    for (i=0; i<=num_render_sections; ++i)
    {
        new_point = &path->mPath[i];
        LLVector3 pos = newSection[i].mPosition * rel_xform;
        LLQuaternion rot = mSection[i].mAxisRotation * newSection[i].mRotation * delta_rot;

        LLVector3 np(new_point->mPos.getF32ptr());

        if (!mUpdated || (np-pos).magVec()/mVO->mDrawable->mDistanceWRTCamera > 0.001f)
        {
            new_point->mPos.load3((newSection[i].mPosition * rel_xform).mV);
            mUpdated = false;
        }

        new_point->mRot.loadu(LLMatrix3(rot));
        new_point->mScale.set(newSection[i].mScale.mV[0], newSection[i].mScale.mV[1], 0,1);
        new_point->mTexT = ((F32)i)/(num_render_sections);
    }
    LL_CHECK_MEMORY
    mLastSegmentRotation = parentSegmentRotation;
}


void LLVolumeImplFlexible::preRebuild()
{
    if (!mUpdated)
    {
        LL_PROFILE_ZONE_SCOPED;
        doFlexibleRebuild(false);
    }
}

void LLVolumeImplFlexible::doFlexibleRebuild(bool rebuild_volume)
{
    LLVolume* volume = mVO->getVolume();
    if (volume)
    {
        if (rebuild_volume)
        {
            volume->setDirty();
        }
        volume->regen();
    }

    mUpdated = true;
}

//------------------------------------------------------------------

void LLVolumeImplFlexible::onSetScale(const LLVector3& scale, bool damped)
{
    setAttributesOfAllSections((LLVector3*) &scale);
}

//----------------------------------------------------------------------------------
void LLVolumeImplFlexible::setCollisionSphere( LLVector3 p, F32 r )
{
    mCollisionSpherePosition = p;
    mCollisionSphereRadius   = r;

}//------------------------------------------------------------------


//----------------------------------------------------------------------------------
void LLVolumeImplFlexible::setUsingCollisionSphere( bool u )
{
}//------------------------------------------------------------------


//----------------------------------------------------------------------------------
void LLVolumeImplFlexible::setRenderingCollisionSphere( bool r )
{
}//------------------------------------------------------------------

//------------------------------------------------------------------
LLVector3 LLVolumeImplFlexible::getEndPosition()
{
    S32 num_sections = 1 << mAttributes->getSimulateLOD();
    return mSection[ num_sections ].mPosition;

}//------------------------------------------------------------------


//------------------------------------------------------------------
LLVector3 LLVolumeImplFlexible::getNodePosition( int nodeIndex )
{
    S32 num_sections = 1 << mAttributes->getSimulateLOD();
    if ( nodeIndex > num_sections - 1 )
    {
        nodeIndex = num_sections - 1;
    }
    else if ( nodeIndex < 0 )
    {
        nodeIndex = 0;
    }

    return mSection[ nodeIndex ].mPosition;

}//------------------------------------------------------------------

LLVector3 LLVolumeImplFlexible::getPivotPosition() const
{
    return getAnchorPosition();
}

//------------------------------------------------------------------
LLVector3 LLVolumeImplFlexible::getAnchorPosition() const
{
    LLVector3 BasePosition = getFramePosition();
    LLQuaternion parentSegmentRotation = getFrameRotation();
    LLVector3 anchorDirectionRotated = LLVector3::z_axis * parentSegmentRotation;
    LLVector3 anchorScale = mVO->mDrawable->getScale();
    return BasePosition - (anchorScale.mV[VZ]/2 * anchorDirectionRotated);

}//------------------------------------------------------------------


//------------------------------------------------------------------
LLQuaternion LLVolumeImplFlexible::getEndRotation()
{
    return mLastSegmentRotation;

}//------------------------------------------------------------------


void LLVolumeImplFlexible::updateRelativeXform(bool force_identity)
{
    LLQuaternion delta_rot;
    LLVector3 delta_pos, delta_scale;
    LLVOVolume* vo = (LLVOVolume*) mVO;

    bool use_identity = vo->mDrawable->isSpatialRoot() || force_identity;

    //matrix from local space to parent relative/global space
    delta_rot = use_identity ? LLQuaternion() : vo->mDrawable->getRotation();
    delta_pos = use_identity ? LLVector3(0,0,0) : vo->mDrawable->getPosition();
    delta_scale = LLVector3(1,1,1);

    // Vertex transform (4x4)
    LLVector3 x_axis = LLVector3(delta_scale.mV[VX], 0.f, 0.f) * delta_rot;
    LLVector3 y_axis = LLVector3(0.f, delta_scale.mV[VY], 0.f) * delta_rot;
    LLVector3 z_axis = LLVector3(0.f, 0.f, delta_scale.mV[VZ]) * delta_rot;

    vo->mRelativeXform.initRows(LLVector4(x_axis, 0.f),
                            LLVector4(y_axis, 0.f),
                            LLVector4(z_axis, 0.f),
                            LLVector4(delta_pos, 1.f));

    x_axis.normVec();
    y_axis.normVec();
    z_axis.normVec();

    vo->mRelativeXformInvTrans.setRows(x_axis, y_axis, z_axis);
}

const LLMatrix4& LLVolumeImplFlexible::getWorldMatrix(LLXformMatrix* xform) const
{
    return xform->getWorldMatrix();
}

//-----------------------------------------------------------------------------
// Vulkan flexi chain (#25)
//-----------------------------------------------------------------------------
#include "llcleaninstancestore.h"
#include "llcleanskinstore.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"

namespace
{
    std::set<LLUUID> sVKFlexiTracked;
}

// static
void LLVolumeImplFlexible::vkRestPoints(const LLVolumeParams& params, const LLVector3& obj_scale,
                                        VKRestPoint out[VK_FLEXI_POINTS])
{
    const LLPathParams& pp = params.getPathParams();
    const LLVector2 bottom = pp.getBeginScale(), top = pp.getEndScale();
    const F32 begin_rot = F_PI * pp.getTwistBegin(), end_rot = F_PI * pp.getTwist();
    const S32 n = VK_FLEXI_POINTS - 1;
    for (S32 i = 0; i <= n; ++i)
    {
        const F32 t = (F32)i / (F32)n;
        out[i].pos.setVec(0.f, 0.f, -0.5f * obj_scale.mV[VZ] + t * obj_scale.mV[VZ]);
        out[i].rot.setQuat(lerp(begin_rot, end_rot, t), 0.f, 0.f, 1.f);
        out[i].scale.setVec(obj_scale.mV[VX] * lerp(bottom.mV[0], top.mV[0], t),
                            obj_scale.mV[VY] * lerp(bottom.mV[1], top.mV[1], t));
    }
}

bool LLVolumeImplFlexible::vkSimulate(F32 out_world[VK_FLEXI_POINTS * 16])
{
    if (!mAttributes || !mVO || mVO->mDrawable.isNull() || !mVO->getVolume())
        return false;
    // Full resolution always: the GPU chain has 9 joints, and 8 CPU sections are
    // trivial next to what GL spent re-tessellating.
    const S32 res = FLEXIBLE_OBJECT_MAX_SECTIONS;
    mRenderRes = res;
    if (!mInitialized || mSimulateRes != res)
    {
        mSimulateRes = res;
        setAttributesOfAllSections();
        mInitialized = true;
    }
    doFlexibleUpdate();   // rewrites the live path points (prim frame)

    const LLPath& path = mVO->getVolume()->getPath();
    if (path.getPathLength() != VK_FLEXI_POINTS)
        return false;
    const LLQuaternion frot = getFrameRotation();
    const LLVector3 fpos = getFramePosition();
    for (S32 i = 0; i < VK_FLEXI_POINTS; ++i)
    {
        const LLPath::PathPt& pt = path.mPath[i];
        F32* m = out_world + i * 16;
        // mRot rows are the point's basis in the prim frame (LLVolume::generate applies
        // them as columns); rotate each into the world by the frame (row-vector * quat).
        for (S32 k = 0; k < 3; ++k)
        {
            const F32* r = pt.mRot.mMatrix[k].getF32ptr();
            const LLVector3 w = LLVector3(r[0], r[1], r[2]) * frot;
            m[k * 4 + 0] = w.mV[0]; m[k * 4 + 1] = w.mV[1]; m[k * 4 + 2] = w.mV[2]; m[k * 4 + 3] = 0.f;
        }
        const F32* p = pt.mPos.getF32ptr();
        const LLVector3 wp = LLVector3(p[0], p[1], p[2]) * frot + fpos;
        m[12] = wp.mV[0]; m[13] = wp.mV[1]; m[14] = wp.mV[2]; m[15] = 1.f;
    }
    return true;
}

// static
void LLVolumeImplFlexible::vkTrack(const LLUUID& object_id)
{
    sVKFlexiTracked.insert(object_id);
}

// static
void LLVolumeImplFlexible::vkUpdateAll()
{
    LL_PROFILE_ZONE_SCOPED;
    if (sVKFlexiTracked.empty()) return;
    LLCleanSkinStore& store = LLCleanSkinStore::instance();
    static LLCachedControl<F32> s_far(gSavedSettings, "RenderFarClip", 256.f);
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 far2 = (F32)s_far * (F32)s_far;
    F32 world[VK_FLEXI_POINTS * 16];
    static F64 s_next_log = 0.0;
    const bool log_now = LLTimer::getElapsedSeconds() > s_next_log;
    if (log_now) s_next_log = LLTimer::getElapsedSeconds() + 5.0;
    U32 n_sim = 0, n_fail = 0, n_noskin = 0, n_skip = 0, n_worn = 0, n_worn_moved = 0;
    bool logged_worn = false;
    // UPDATE RATE by distance and visibility (a dense sim has 1000+ flexis -- grass,
    // plants, hair -- and simulating all of them every frame cost ~4 ms of main thread):
    // on screen every frame within 20 m, every 2nd / 4th / 8th frame within 40 / 80 m /
    // beyond; off screen half as often again (up to every 16th: their shadows still
    // show). Staggered per object so the work spreads evenly. The simulation steps by
    // its own timer (doFlexibleUpdate), so a skipped flexi moves at the same speed.
    static U32 s_flexi_frame = 0;
    ++s_flexi_frame;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (auto it = sVKFlexiTracked.begin(); it != sVKFlexiTracked.end(); )
    {
        LLViewerObject* obj = gObjectList.findObject(*it);
        LLVOVolume* vo = (obj && !obj->isDead() && obj->getPCode() == LL_PCODE_VOLUME) ? (LLVOVolume*)obj : nullptr;
        LLVolumeImplFlexible* impl = vo ? vo->vkFlexiImpl() : nullptr;
        if (!impl)
        {
            it = sVKFlexiTracked.erase(it);
            continue;
        }
        const U32 skin = store.flexiSkin(*it);
        // Beyond the draw distance the chain keeps its last pose (it isn't drawn).
        // One render-position query per flexi (a worn flexi's goes through the joint /
        // parent chain; it was evaluated three times per object per frame).
        LLVector3 rp;
        F32 d2 = 0.f;
        if (skin != 0xFFFFFFFFu)
        {
            rp = vo->getRenderPosition();
            d2 = (rp - cam).magVecSquared();
        }
        if (skin == 0xFFFFFFFFu) ++n_noskin;
        else if (d2 <= far2)
        {
            U32 interval = d2 < 20.f * 20.f ? 1u : d2 < 40.f * 40.f ? 2u : d2 < 80.f * 80.f ? 4u : 8u;
            const F32 radius = llmax(vo->getScale().magVec() * 0.5f, 0.1f);
            if (!camera->sphereInFrustum(rp, radius)) interval = llmin(interval * 2u, 16u);
            if (interval > 1u && ((s_flexi_frame + (U32)it->mData[0] + ((U32)it->mData[1] << 8)) % interval) != 0u)
            {
                ++n_skip;
                ++it;
                continue;
            }
            if (impl->vkSimulate(world))
            {
                store.flexiSetPose(skin, world);
                // WORN FLEXI: registerCleanFace composes a worn prim's model_matrix
                // attach-point-relative, which for a flexi chain (bit19, no bit9) the cull's
                // worldAABB and the pick box read as a WORLD pose -- the bound sat near the
                // region origin and the chain was frustum-culled everywhere else. Its
                // vertices are skinned from the chain frame (getRenderPosition), so the
                // bound follows that frame's origin.
                LLVOAvatar* wearer = vo->isHUDAttachment() ? nullptr : vo->getAvatarAncestor();
                if (wearer)
                {
                    ++n_worn;
                    const LLVector3 fpos = impl->getFramePosition();
                    if (LLCleanInstanceStore::instance().setFlexiOrigin(*it, fpos.mV)) ++n_worn_moved;
                    if (log_now && !logged_worn)
                    {
                        logged_worn = true;
                        const LLVector3 wp = wearer->getPositionAgent();
                        LL_INFOS("VKFlexi") << "worn " << *it << " skin " << skin << " frame (" << fpos.mV[0] << ","
                                            << fpos.mV[1] << "," << fpos.mV[2] << ") wearer (" << wp.mV[0] << ","
                                            << wp.mV[1] << "," << wp.mV[2] << ")" << LL_ENDL;
                    }
                }
                // Keep the live volume (picking) and the drawable bounds near the
                // rendered shape: 4 Hz within 64 m (hover picks run every frame).
                const F64 now = LLTimer::getElapsedSeconds();
                if (now - impl->mVKLastRegen > 0.25 && d2 < 64.f * 64.f)
                {
                    impl->mVKLastRegen = now;
                    impl->doFlexibleRebuild(false);
                    if (LLDrawable* dr = vo->mDrawable)
                    {
                        // genBBoxes only runs with a REBUILD flag, which VK never sets
                        // (same trick as the ingest sweep's bounds regen).
                        vo->updateRelativeXform();
                        const bool had = dr->isState(LLDrawable::REBUILD_POSITION);
                        if (!had) dr->setState(LLDrawable::REBUILD_POSITION);
                        vo->genBBoxes(false);
                        if (!had) dr->clearState(LLDrawable::REBUILD_POSITION);
                    }
                }
                if (log_now && n_sim == 0)
                    LL_INFOS("VKFlexi") << "pose " << *it << " skin " << skin << " p0 (" << world[12] << ","
                                        << world[13] << "," << world[14] << ") p8 (" << world[8 * 16 + 12] << ","
                                        << world[8 * 16 + 13] << "," << world[8 * 16 + 14] << ") path "
                                        << vo->getVolume()->getPath().getPathLength() << LL_ENDL;
                ++n_sim;
            }
            else ++n_fail;
        }
        ++it;
    }
    if (log_now)
        LL_INFOS("VKFlexi") << "tracked " << sVKFlexiTracked.size() << " simulated " << n_sim
                            << " skipped (rate) " << n_skip << " failed " << n_fail << " no-skin " << n_noskin
                            << " worn " << n_worn << " (bound moved " << n_worn_moved << ")" << LL_ENDL;
}
