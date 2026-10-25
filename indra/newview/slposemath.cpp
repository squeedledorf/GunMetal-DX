/**
 * @file slposemath.cpp
 * @brief Pose Studio (#500): the OP Pose HUD dial model and the compact
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

#include "llviewerprecompiledheaders.h"

#include "slposemath.h"

#include "llmath.h"
#include "llsdutil.h"
#include "llsdutil_math.h"

#include <cmath>

namespace
{
// The generator's types and helpers, on doubles, in its conventions (Hamilton products,
// left applied last). Names follow src/Anim.c.
struct HQ
{
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

struct DV
{
    double x = 0.0, y = 0.0, z = 0.0;
};

constexpr double PI_D = 3.14159265358979323846;

// vecDegToRad: deg * M_PI / 180.0, in exactly that order. The generator hits the sphere's
// singular points (e.g. elbow y = 90, swing of 180 degrees about a near-zero cross product)
// where the last bit decides the swing axis, so the arithmetic has to match bit for bit.
double degToRad(double deg)
{
    return deg * PI_D / 180.0;
}

HQ hmulRaw(const HQ& l, const HQ& r)
{
    HQ q;
    q.w = l.w * r.w - l.x * r.x - l.y * r.y - l.z * r.z;
    q.x = l.w * r.x + l.x * r.w + l.y * r.z - l.z * r.y;
    q.y = l.w * r.y - l.x * r.z + l.y * r.w + l.z * r.x;
    q.z = l.w * r.z + l.x * r.y - l.y * r.x + l.z * r.w;
    return q;
}

// multiplyQuaternions: Hamilton product, normalized.
HQ hmul(const HQ& l, const HQ& r)
{
    HQ q = hmulRaw(l, r);
    const double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n > 0.0)
    {
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
    }
    return q;
}

HQ conjQ(const HQ& q)
{
    HQ c = q;
    c.x = -c.x; c.y = -c.y; c.z = -c.z;
    return c;
}

// eulerToQuaternion
HQ eulerQ(double ex, double ey, double ez)
{
    const double cr = cos(ex * 0.5), sr = sin(ex * 0.5);
    const double cp = cos(ey * 0.5), sp = sin(ey * 0.5);
    const double cy = cos(ez * 0.5), sy = sin(ez * 0.5);
    HQ q;
    q.w = cr * cp * cy + sr * sp * sy;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    return q;
}

// sphericalToCartesian, including its theta > pi remap.
DV sph(double theta, double phi)
{
    if (theta > PI_D)
    {
        theta = 2.0 * PI_D - theta;
        phi = fmod(phi + PI_D, 2.0 * PI_D);
    }
    DV v;
    v.x = sin(theta) * cos(phi);
    v.y = sin(theta) * sin(phi);
    v.z = cos(theta);
    return v;
}

DV normV(const DV& v)
{
    if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0)
        return v;
    const double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    DV r;
    r.x = v.x / n; r.y = v.y / n; r.z = v.z / n;
    return r;
}

DV crossV(const DV& a, const DV& b)
{
    DV r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

double dotV(const DV& a, const DV& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// axisAngleToQuaternion (a zero axis gives the identity)
HQ aaQ(const DV& v, double angle)
{
    HQ q;
    const double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (n > 0.0)
    {
        const double s = sin(angle / 2.0);
        q.x = v.x / n * s; q.y = v.y / n * s; q.z = v.z / n * s;
        q.w = cos(angle / 2.0);
    }
    return q;
}

// calculateAxisAngle(v1, v2) as a quaternion (acos clamped: his unclamped acos of a dot
// a hair above 1 only ever happens with a zero cross product, which is the identity anyway)
HQ swingQ(const DV& from, const DV& to)
{
    const DV a = normV(from), b = normV(to);
    return aaQ(normV(crossV(a, b)), acos(llclamp(dotV(a, b), -1.0, 1.0)));
}

DV rotateV(const HQ& q, const DV& v)
{
    HQ p;
    p.w = 0.0; p.x = v.x; p.y = v.y; p.z = v.z;
    const HQ r = hmulRaw(hmulRaw(q, p), conjQ(q));
    DV out;
    out.x = r.x; out.y = r.y; out.z = r.z;
    return out;
}

DV refAxis(S32 axis)
{
    DV r;
    if (axis == 0) r.x = 1.0;
    else if (axis == 1) r.y = 1.0;
    else r.z = 1.0;
    return r;
}

HQ fromLL(const LLQuaternion& q)
{
    HQ h;
    h.w = q.mQ[VW]; h.x = q.mQ[VX]; h.y = q.mQ[VY]; h.z = q.mQ[VZ];
    return h;
}

// rotate_quats_spherical for applied angles (radians), then the .anim writer's w rebuild.
HQ forwardApplied(const SLBoneModel& m, double ax, double ay, double az, bool invert)
{
    DV pos;
    HQ swing, twist;
    if (m.mAxis == 0)
    {
        pos = sph(ay, az);
        swing = swingQ(refAxis(0), pos);
        twist = aaQ(pos, ax);
    }
    else if (m.mAxis == 1)
    {
        pos = sph(ax, az);
        swing = swingQ(refAxis(1), pos);
        twist = aaQ(pos, ay);
    }
    else
    {
        pos = sph(ay, ax);
        swing = swingQ(refAxis(2), pos);
        if (m.mLeg)
        {
            const double add = (ay < PI_D / 2.0) ? ax * sin(ay) : ax - ax * cos(ay);
            twist = aaQ(pos, az + add);
        }
        else
        {
            twist = aaQ(pos, az);
        }
    }

    HQ rot = fromLL(m.mBase);
    const HQ pre = eulerQ(degToRad(m.mPre.mV[VX]), degToRad(m.mPre.mV[VY]), degToRad(m.mPre.mV[VZ]));
    if (m.mLeg)
    {
        if (ay > 0.0 && ay < PI_D)
            rot = hmul(rot, pre);
        else if (ay >= PI_D || ay < -PI_D / 2.0)
            rot = hmul(rot, eulerQ(0.0, PI_D * 1.5, 0.0));
        else
            rot = hmul(rot, eulerQ(0.0, -PI_D / 2.0, 0.0));
    }
    else
    {
        rot = hmul(rot, pre);
    }

    HQ rotation = hmul(twist, swing);
    if (invert)
        rotation = conjQ(rotation);
    rot = hmul(rot, rotation);

    // The .anim keys store x, y, z only; w comes back as +sqrt(1 - |xyz|^2).
    rot.w = sqrt(llmax(0.0, 1.0 - (rot.x * rot.x + rot.y * rot.y + rot.z * rot.z)));
    return rot;
}

double angleBetweenDeg(const HQ& a, const HQ& b)
{
    const double d = fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2.0 * acos(llmin(1.0, d)) * 180.0 / PI_D;
}
}

// static
SLBoneModel SLBoneModel::fromLLSD(const LLSD& sd, const SLBoneModel& defaults)
{
    SLBoneModel m = defaults;
    if (sd.has("axis"))
        m.mAxis = llclamp((S32)sd["axis"].asInteger(), 0, 2);
    if (sd.has("leg"))
        m.mLeg = sd["leg"].asInteger() != 0;
    if (sd.has("pre"))
        m.mPre.setValue(sd["pre"]);
    if (sd.has("base"))
    {
        m.mBase = ll_quaternion_from_sd(sd["base"]);
        m.mBase.normalize();
    }
    if (sd.has("offset"))
        m.mOffset.setValue(sd["offset"]);
    if (sd.has("invert"))
    {
        m.mInvert.setValue(sd["invert"]);
        for (S32 i = 0; i < 3; ++i)
            m.mInvert.mV[i] = (m.mInvert.mV[i] < 0.f) ? -1.f : 1.f;
    }
    if (sd.has("min"))
        m.mMin.setValue(sd["min"]);
    if (sd.has("max"))
        m.mMax.setValue(sd["max"]);
    if (sd.has("steps"))
        m.mSteps.setValue(sd["steps"]);
    m.mFollowers.clear();
    if (sd.has("followers"))
    {
        for (const LLSD& f : llsd::inArray(sd["followers"]))
        {
            Follower follower;
            follower.mJoint = f["joint"].asString();
            follower.mInvert = f["invert"].asInteger() != 0;
            if (!follower.mJoint.empty())
                m.mFollowers.push_back(follower);
        }
    }
    return m;
}

LLQuaternion SLBoneModel::toQuat(const SLPoseAngles& dial, bool follower_invert) const
{
    // applied angle = invert * (dial - offset), as the generator's loop and -ix/-iy/-iz did
    const double ax = degToRad((double)mInvert.mV[VX] * ((double)dial.mX - (double)mOffset.mV[VX]));
    const double ay = degToRad((double)mInvert.mV[VY] * ((double)dial.mY - (double)mOffset.mV[VY]));
    const double az = degToRad((double)mInvert.mV[VZ] * ((double)dial.mZ - (double)mOffset.mV[VZ]));
    const HQ q = forwardApplied(*this, ax, ay, az, follower_invert);
    LLQuaternion out((F32)q.x, (F32)q.y, (F32)q.z, (F32)q.w);
    out.normalize();
    return out;
}

SLPoseAngles SLBoneModel::fromQuat(const LLQuaternion& rotation, const SLPoseAngles& reference) const
{
    HQ target = fromLL(rotation);
    if (target.w < 0.0)
    {
        target.w = -target.w; target.x = -target.x; target.y = -target.y; target.z = -target.z;
    }

    // The current dial values still produce this rotation: keep them. This also covers the
    // generator's singular points (e.g. a 180-degree swing), where many dial triples give
    // the same rotation and a fresh decomposition would jump.
    if (angleBetweenDeg(fromLL(toQuat(reference)), target) <= 0.05)
        return reference;

    // Reference in applied generator angles (radians).
    double ref[3];
    for (S32 i = 0; i < 3; ++i)
        ref[i] = degToRad((double)mInvert.mV[i] * ((double)reference.get(i) - (double)mOffset.mV[i]));
    // The swing direction's phi at the reference (kept at the poles, where it is undefined).
    const double ref_phi = (mAxis == 2) ? ref[0] : ref[2];

    // Pre-images of the w rebuild: the target itself, and the same x,y,z with -w.
    std::vector<HQ> pre_images(1, target);
    if (target.w > 1.0e-9)
    {
        HQ alt = target;
        alt.w = -alt.w;
        pre_images.push_back(alt);
    }

    // The pre-rotations the forward model can have used.
    std::vector<HQ> pres(1, eulerQ(degToRad(mPre.mV[VX]), degToRad(mPre.mV[VY]), degToRad(mPre.mV[VZ])));
    if (mLeg)
    {
        pres.push_back(eulerQ(0.0, PI_D * 1.5, 0.0));
        pres.push_back(eulerQ(0.0, -PI_D / 2.0, 0.0));
    }

    const HQ base_inv = conjQ(fromLL(mBase));
    const DV r = refAxis(mAxis);

    // Candidates are ranked: rotation reproduced and every dial inside its range, then
    // reproduced but outside a range, then not reproduced; within a rank the one nearest to
    // reference (or, unreproduced, the smallest error) wins. The range rank keeps a decode
    // from picking an equivalent triple the dials cannot show (e.g. wrist y 140 as -160).
    bool have = false;
    S32 best_rank = 0;
    double best_score = 0.0;
    SLPoseAngles best = reference;

    for (const HQ& image : pre_images)
    {
        for (const HQ& pre : pres)
        {
            // image = base * pre * R  ->  R = pre^-1 * base^-1 * image
            const HQ R = hmulRaw(conjQ(pre), hmulRaw(base_inv, image));
            const DV pos = rotateV(R, r);
            const double theta = acos(llclamp(pos.z, -1.0, 1.0));
            const bool pole = sin(theta) < 1.0e-4;
            const double phi = pole ? ref_phi : atan2(pos.y, pos.x);

            const HQ T = hmulRaw(R, conjQ(swingQ(r, pos)));
            const double twist = 2.0 * atan2(T.x * pos.x + T.y * pos.y + T.z * pos.z, T.w);

            const double thetas[3] = { theta, -theta, 2.0 * PI_D - theta };
            const double phis[3] = { phi, phi + PI_D, phi - PI_D };
            for (S32 v = 0; v < 3; ++v)
            {
                for (S32 k1 = -1; k1 <= 1; ++k1)
                {
                    for (S32 k2 = -1; k2 <= 1; ++k2)
                    {
                        const double ph = phis[v] + 2.0 * PI_D * k1;
                        const double tw = twist + 2.0 * PI_D * k2;
                        const double th = thetas[v];
                        double g[3];
                        if (mAxis == 0) { g[0] = tw; g[1] = th; g[2] = ph; }
                        else if (mAxis == 1) { g[0] = th; g[1] = tw; g[2] = ph; }
                        else
                        {
                            const double ax = ph, ay = th;
                            double add = 0.0;
                            if (mLeg)
                                add = (ay < PI_D / 2.0) ? ax * sin(ay) : ax - ax * cos(ay);
                            g[0] = ax; g[1] = ay; g[2] = tw - add;
                        }

                        SLPoseAngles dial;
                        for (S32 i = 0; i < 3; ++i)
                            dial.set(i, (F32)(g[i] * 180.0 / PI_D * mInvert.mV[i] + mOffset.mV[i]));

                        const double err = angleBetweenDeg(fromLL(toQuat(dial)), target);
                        const bool valid = err <= 0.05;
                        double dist = 0.0;
                        bool in_range = true;
                        for (S32 i = 0; i < 3; ++i)
                        {
                            dist += fabs(dial.get(i) - reference.get(i));
                            in_range = in_range && dial.get(i) >= mMin.mV[i] - 0.01f && dial.get(i) <= mMax.mV[i] + 0.01f;
                        }
                        const S32 rank = valid ? (in_range ? 2 : 1) : 0;
                        const double score = valid ? dist : err;
                        if (!have || rank > best_rank || (rank == best_rank && score < best_score))
                        {
                            have = true;
                            best_rank = rank;
                            best_score = score;
                            best = dial;
                        }
                    }
                }
            }
        }
    }
    return best;
}

// static
LLQuaternion SLBoneModel::mirror(const LLQuaternion& rotation)
{
    // Reflection through the X-Z plane (Y is avatar-left in SL): same as FSJointState::reflectRotation.
    return LLQuaternion(-rotation.mQ[VX], rotation.mQ[VY], -rotation.mQ[VZ], rotation.mQ[VW]);
}

namespace SLPoseQuant
{
namespace
{
constexpr F32 COMPONENT_RANGE = 0.70710678f; // 1/sqrt(2): bound of the three smallest components
constexpr U32 COMPONENT_MAX = 32767;         // 15 bits
constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

S32 b64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
}

void packQuat(const LLQuaternion& rotation, U8 out[6])
{
    LLQuaternion q(rotation);
    q.normalize();

    S32 largest = 0;
    for (S32 i = 1; i < 4; ++i)
    {
        if (llabs(q.mQ[i]) > llabs(q.mQ[largest]))
            largest = i;
    }
    const F32 sign = (q.mQ[largest] < 0.f) ? -1.f : 1.f;

    U64 bits = (U64)largest;
    for (S32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        const F32 c = llclamp(q.mQ[i] * sign / COMPONENT_RANGE, -1.f, 1.f);
        const U32 v = (U32)ll_round((c * 0.5f + 0.5f) * (F32)COMPONENT_MAX);
        bits = (bits << 15) | (U64)(v & COMPONENT_MAX);
    }
    // 2 + 45 = 47 bits, stored in 48.
    for (S32 i = 5; i >= 0; --i)
    {
        out[i] = (U8)(bits & 0xFF);
        bits >>= 8;
    }
}

LLQuaternion unpackQuat(const U8 in[6])
{
    U64 bits = 0;
    for (S32 i = 0; i < 6; ++i)
        bits = (bits << 8) | (U64)in[i];

    F32 comps[3];
    for (S32 i = 2; i >= 0; --i)
    {
        const U32 v = (U32)(bits & COMPONENT_MAX);
        bits >>= 15;
        comps[i] = (((F32)v / (F32)COMPONENT_MAX) * 2.f - 1.f) * COMPONENT_RANGE;
    }
    const S32 largest = (S32)(bits & 0x3);

    LLQuaternion q;
    F32 sum = 0.f;
    S32 c = 0;
    for (S32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        q.mQ[i] = comps[c++];
        sum += q.mQ[i] * q.mQ[i];
    }
    q.mQ[largest] = sqrtf(llmax(0.f, 1.f - sum));
    q.normalize();
    return q;
}

std::string encode64(const U8* data, size_t len)
{
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= len; i += 3)
    {
        const U32 v = ((U32)data[i] << 16) | ((U32)data[i + 1] << 8) | (U32)data[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    const size_t rest = len - i;
    if (rest == 1)
    {
        const U32 v = (U32)data[i] << 16;
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
    }
    else if (rest == 2)
    {
        const U32 v = ((U32)data[i] << 16) | ((U32)data[i + 1] << 8);
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
    }
    return out;
}

bool decode64(const std::string& text, std::string& out)
{
    out.clear();
    out.reserve(text.size() * 3 / 4);
    U32 acc = 0;
    S32 nbits = 0;
    for (char ch : text)
    {
        const S32 v = b64Value(ch);
        if (v < 0)
            return false;
        acc = (acc << 6) | (U32)v;
        nbits += 6;
        if (nbits >= 8)
        {
            nbits -= 8;
            out += (char)((acc >> nbits) & 0xFF);
        }
    }
    return true;
}
}
