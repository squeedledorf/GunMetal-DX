/**
 * @file slposemath.h
 * @brief Pose Studio (#500): the OP Pose HUD dial model (exact port of its generator) and the compact
 *        quaternion quantisation used by pose sync.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 * $/LicenseInfo$
 */

#ifndef SL_POSEMATH_H
#define SL_POSEMATH_H

#include "llquaternion.h"
#include "llsd.h"
#include "v3math.h"

#include <string>
#include <vector>

// The three dial values of a bone, in degrees, exactly as the OP Pose HUD numbers them
// (its animation names x<X>y<Y>z<Z>): dial = generator angle + offset.
struct SLPoseAngles
{
    F32 mX = 0.f;
    F32 mY = 0.f;
    F32 mZ = 0.f;

    F32 get(S32 axis) const { return axis == 0 ? mX : (axis == 1 ? mY : mZ); }
    void set(S32 axis, F32 v) { (axis == 0 ? mX : (axis == 1 ? mY : mZ)) = v; }
};

// One bone's dial model: an exact port of rotate_quats_spherical() from the owner's
// OP Pose HUD animation generator (src/Anim.c), with that bone's generator settings
// (app_settings/posestudio/bone_axes.xml, extracted from bin/<bone>/gen_settings.txt and
// checked against every bone's generated .anim files).
//
// In the generator's own terms, for applied angles a = invert * (dial - offset):
//   the bone's reference axis r (twist_axis: X, Y or Z) is swung onto the point pos of the
//   unit sphere given by two of the angles (sphericalToCartesian(theta, phi)), then twisted
//   about pos by the third:
//     axis X: pos = S(a.y, a.z), twist a.x      axis Y: pos = S(a.x, a.z), twist a.y
//     axis Z: pos = S(a.y, a.x), twist a.z (+ the "leg twist mitigation" term)
//   result = base * pre * (twist * swing)   (Hamilton products, left applied last)
// with pre the pre_rotate Euler (or, with leg mitigation, a y-dependent replacement), and
// finally w is rebuilt as +sqrt(1 - x^2 - y^2 - z^2) the way his .anim writer (and LL's
// reader) does -- which is why a few large elbow angles are not the "mathematical" rotation
// but are what the HUD actually plays.
//
// Quaternion conventions: his Quaternion_t (w,x,y,z) and multiplyQuaternions(l, r) is the
// Hamilton product l*r (r applied first). LLQuaternion stores the same components
// (mQ = x,y,z,w) and its operator*(a, b) is the Hamilton product b*a (a applied first).
// The port computes in his conventions on plain doubles and only converts the final
// quaternion component-for-component, so no product order has to be translated.
struct SLBoneModel
{
    struct Follower
    {
        std::string mJoint;
        bool mInvert = false;   // conjugated rotation (the generator's -if)
    };

    S32 mAxis = 0;              // twist axis: 0 X, 1 Y, 2 Z
    bool mLeg = false;          // leg twist mitigation
    LLVector3 mPre;             // pre_rotate, degrees
    LLQuaternion mBase;         // rest rotation (input .anim), LL component order
    LLVector3 mOffset{ 0.f, -90.f, 0.f };
    LLVector3 mInvert{ 1.f, 1.f, 1.f };
    LLVector3 mMin{ -180.f, -180.f, -180.f };
    LLVector3 mMax{ 180.f, 180.f, 180.f };
    LLVector3 mSteps{ 5.f, 5.f, 5.f };
    std::vector<Follower> mFollowers;

    static SLBoneModel fromLLSD(const LLSD& sd, const SLBoneModel& defaults);

    // The joint rotation for these dial values (follower_invert: the conjugated variant
    // the generator writes for inverted followers).
    LLQuaternion toQuat(const SLPoseAngles& dial, bool follower_invert = false) const;

    // Dial values for a joint rotation. Every rotation has several dial triples. If reference
    // itself still produces the rotation it is returned unchanged (exact round trip of what
    // the user dialled); otherwise the triple inside the dial ranges nearest to reference is
    // returned (so dials never jump). At the sphere's poles the swing direction is undefined
    // and reference's value is kept there -- only a remembered reference can recover it.
    SLPoseAngles fromQuat(const LLQuaternion& rotation, const SLPoseAngles& reference) const;

    // Left/right mirror of a joint rotation across the avatar's sagittal (X-Z) plane. For
    // every HUD bone except the hips this equals the HUD's own mirror rule (X -> -X, Y kept,
    // Z -> -Z on the other side's dials); the hips' generated sets are not mirror images of
    // each other, so the geometric mirror is used for all bones.
    static LLQuaternion mirror(const LLQuaternion& rotation);
};

namespace SLPoseQuant
{
    // Smallest-three quaternion packing: 2 bits largest index + 3 x 15-bit components
    // (well under 0.01 degrees of error), written as 6 bytes, big-endian.
    void packQuat(const LLQuaternion& q, U8 out[6]);
    LLQuaternion unpackQuat(const U8 in[6]);

    // Unpadded base64url (A-Z a-z 0-9 - _): no characters that LSL, chat or our
    // comma-separated envelope treat specially.
    std::string encode64(const U8* data, size_t len);
    bool decode64(const std::string& text, std::string& out);
}

#endif // SL_POSEMATH_H
