/**
 * @file skcensusavatarshape.cpp
 * @brief The census's avatar physics shapes.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: measured avatar hull (skcensusavatarshape, 8dd8b54a4f).

#include "llviewerprecompiledheaders.h"

#include "skcensusavatarshape.h"

#include "llvoavatar.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace
{
    struct XY
    {
        F32 x, y;
    };

    // The corners as measured, at agent height 1.189 m; each list is one level of the shape, at one z.
    // Standing: the bottom square, the lower level, the widest level and the top. The widths do not change with
    // height; the bottom two levels keep their spacing from the foot, the top its place at the box's top, and the
    // widest level moves two thirds as far as the top does.
    const XY STAND_BOTTOM[] = { { 0.051f, -0.051f }, { 0.051f, 0.051f }, { -0.051f, 0.051f }, { -0.051f, -0.051f } };
    const XY STAND_LOWER[] = { { 0.225f, -0.113f }, { 0.225f, 0.113f },   { 0.f, 0.226f },
                               { -0.195f, 0.113f }, { -0.195f, -0.113f }, { 0.f, -0.226f } };
    const XY STAND_WIDEST[] = { { 0.225f, -0.101f }, { 0.225f, 0.101f },   { 0.f, 0.301f },
                                { -0.226f, 0.101f }, { -0.226f, -0.101f }, { 0.f, -0.301f } };
    const XY STAND_TOP[] = { { 0.113f, -0.101f }, { 0.113f, 0.101f }, { -0.113f, 0.101f }, { -0.113f, -0.101f } };
    const F32 STAND_ABOVE_BOX = 0.05f;      // the shape's top and foot stand this far beyond the agent box
    const F32 STAND_LOWER_OVER_FOOT = 0.648f;
    const F32 STAND_WIDEST_Z = 0.333f;      // at the measured height
    const F32 STAND_WIDEST_RATE = 0.66f;    // of the top's own rise with height
    const F32 MEASURED_HALF = 0.6444f;      // the measured shape's half height, foot to top

    // Sat on the ground: a frustum under the avatar's position, its foot's edge bevelled. The sitting hulls are fixed,
    // so they are kept as triangles, outward-wound, rather than found from their measured corners each time.
    // 20 triangles, 156.2 L
    const LLVector3 SIT_GROUND[] = {
        { 0.3250f, 0.4508f, -0.4460f }, { 0.2263f, 0.1509f, 0.0000f }, { 0.3326f, 0.4355f, -0.4233f },
        { 0.2263f, -0.1509f, 0.0000f }, { 0.3250f, -0.4508f, -0.4460f }, { 0.3326f, -0.4355f, -0.4233f },
        { -0.3326f, -0.4355f, -0.4233f }, { -0.3250f, -0.4508f, -0.4460f }, { -0.2263f, -0.1509f, 0.0000f },
        { -0.3250f, 0.4508f, -0.4460f }, { -0.3326f, 0.4355f, -0.4233f }, { -0.2263f, 0.1509f, 0.0000f },
        { 0.3326f, -0.4355f, -0.4233f }, { 0.3250f, -0.4508f, -0.4460f }, { 0.3250f, 0.4508f, -0.4460f },
        { 0.3326f, -0.4355f, -0.4233f }, { 0.3250f, 0.4508f, -0.4460f }, { 0.3326f, 0.4355f, -0.4233f },
        { 0.2263f, -0.1509f, 0.0000f }, { -0.2263f, 0.1509f, 0.0000f }, { -0.2263f, -0.1509f, 0.0000f },
        { 0.2263f, -0.1509f, 0.0000f }, { 0.2263f, 0.1509f, 0.0000f }, { -0.2263f, 0.1509f, 0.0000f },
        { 0.2263f, -0.1509f, 0.0000f }, { 0.3326f, -0.4355f, -0.4233f }, { 0.3326f, 0.4355f, -0.4233f },
        { 0.2263f, -0.1509f, 0.0000f }, { 0.3326f, 0.4355f, -0.4233f }, { 0.2263f, 0.1509f, 0.0000f },
        { 0.2263f, -0.1509f, 0.0000f }, { -0.3250f, -0.4508f, -0.4460f }, { 0.3250f, -0.4508f, -0.4460f },
        { 0.2263f, -0.1509f, 0.0000f }, { -0.2263f, -0.1509f, 0.0000f }, { -0.3250f, -0.4508f, -0.4460f },
        { -0.3326f, -0.4355f, -0.4233f }, { -0.2263f, -0.1509f, 0.0000f }, { -0.2263f, 0.1509f, 0.0000f },
        { -0.3326f, -0.4355f, -0.4233f }, { -0.2263f, 0.1509f, 0.0000f }, { -0.3326f, 0.4355f, -0.4233f },
        { -0.3250f, 0.4508f, -0.4460f }, { -0.2263f, 0.1509f, 0.0000f }, { 0.2263f, 0.1509f, 0.0000f },
        { -0.3250f, 0.4508f, -0.4460f }, { 0.2263f, 0.1509f, 0.0000f }, { 0.3250f, 0.4508f, -0.4460f },
        { -0.3250f, 0.4508f, -0.4460f }, { -0.3326f, -0.4355f, -0.4233f }, { -0.3326f, 0.4355f, -0.4233f },
        { -0.3250f, 0.4508f, -0.4460f }, { -0.3250f, -0.4508f, -0.4460f }, { -0.3326f, -0.4355f, -0.4233f },
        { -0.3250f, 0.4508f, -0.4460f }, { 0.3250f, 0.4508f, -0.4460f }, { 0.3250f, -0.4508f, -0.4460f },
        { -0.3250f, 0.4508f, -0.4460f }, { 0.3250f, -0.4508f, -0.4460f }, { -0.3250f, -0.4508f, -0.4460f },
    };

    // Sat on an object: a wedge leaning back from the seat, behind and below the avatar's position.
    // 16 triangles, 119.6 L
    const LLVector3 SIT_OBJECT[] = {
        { -0.5380f, 0.3008f, -0.4491f }, { -0.5380f, 0.1505f, -0.0017f }, { 0.1371f, 0.3007f, -0.7462f },
        { 0.1371f, -0.3007f, -0.7462f }, { -0.5380f, -0.1505f, -0.0017f }, { -0.5380f, -0.3008f, -0.4491f },
        { 0.1102f, 0.2628f, -0.6341f }, { -0.5380f, 0.1505f, -0.0017f }, { 0.0255f, 0.1505f, -0.2999f },
        { 0.1102f, 0.2628f, -0.6341f }, { 0.1371f, 0.3007f, -0.7462f }, { -0.5380f, 0.1505f, -0.0017f },
        { 0.1102f, -0.2628f, -0.6341f }, { 0.0255f, -0.1505f, -0.2999f }, { -0.5380f, -0.1505f, -0.0017f },
        { 0.1102f, -0.2628f, -0.6341f }, { -0.5380f, -0.1505f, -0.0017f }, { 0.1371f, -0.3007f, -0.7462f },
        { -0.5380f, -0.1505f, -0.0017f }, { -0.5380f, 0.3008f, -0.4491f }, { -0.5380f, -0.3008f, -0.4491f },
        { -0.5380f, -0.1505f, -0.0017f }, { -0.5380f, 0.1505f, -0.0017f }, { -0.5380f, 0.3008f, -0.4491f },
        { -0.5380f, -0.1505f, -0.0017f }, { 0.0255f, 0.1505f, -0.2999f }, { -0.5380f, 0.1505f, -0.0017f },
        { -0.5380f, -0.1505f, -0.0017f }, { 0.0255f, -0.1505f, -0.2999f }, { 0.0255f, 0.1505f, -0.2999f },
        { 0.1371f, -0.3007f, -0.7462f }, { -0.5380f, -0.3008f, -0.4491f }, { -0.5380f, 0.3008f, -0.4491f },
        { 0.1371f, -0.3007f, -0.7462f }, { -0.5380f, 0.3008f, -0.4491f }, { 0.1371f, 0.3007f, -0.7462f },
        { 0.1102f, -0.2628f, -0.6341f }, { 0.0255f, 0.1505f, -0.2999f }, { 0.0255f, -0.1505f, -0.2999f },
        { 0.1102f, -0.2628f, -0.6341f }, { 0.1102f, 0.2628f, -0.6341f }, { 0.0255f, 0.1505f, -0.2999f },
        { 0.1102f, -0.2628f, -0.6341f }, { 0.1371f, -0.3007f, -0.7462f }, { 0.1371f, 0.3007f, -0.7462f },
        { 0.1102f, -0.2628f, -0.6341f }, { 0.1371f, 0.3007f, -0.7462f }, { 0.1102f, 0.2628f, -0.6341f },
    };

    // The convex hull of a few corners on exact levels, one fan per face: few enough corners that every three can be tried.
    std::vector<LLVector3> hullOf(const std::vector<LLVector3>& p)
    {
        const F32 EPS = 1e-4f;
        LLVector3 centre;
        for (const LLVector3& v : p)
        {
            centre += v;
        }
        centre /= (F32)p.size();
        std::vector<LLVector3> tris;
        std::set<std::vector<size_t>> faces;
        const size_t n = p.size();
        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = i + 1; j < n; ++j)
            {
                for (size_t k = j + 1; k < n; ++k)
                {
                    LLVector3 normal = (p[j] - p[i]) % (p[k] - p[i]);
                    if (normal.normVec() < 1e-6f)
                    {
                        continue;
                    }
                    if (normal * (centre - p[i]) > 0.f)
                    {
                        normal = -normal;
                    }
                    const F32 d = normal * p[i];
                    std::vector<size_t> on;
                    bool outside = false;
                    for (size_t m = 0; m < n && !outside; ++m)
                    {
                        const F32 s = normal * p[m] - d;
                        outside = s > EPS;
                        if (std::fabs(s) <= EPS)
                        {
                            on.push_back(m);
                        }
                    }
                    if (outside || !faces.insert(on).second)
                    {
                        continue;
                    }
                    // Round the face's own centre, wound outward.
                    LLVector3 mid;
                    for (size_t m : on)
                    {
                        mid += p[m];
                    }
                    mid /= (F32)on.size();
                    LLVector3 u = p[on[0]] - mid;
                    u.normVec();
                    const LLVector3 w = normal % u;
                    std::sort(on.begin(), on.end(), [&](size_t a, size_t b)
                              { return std::atan2((p[a] - mid) * w, (p[a] - mid) * u) < std::atan2((p[b] - mid) * w, (p[b] - mid) * u); });
                    for (size_t m = 1; m + 1 < on.size(); ++m)
                    {
                        tris.push_back(p[on[0]]);
                        tris.push_back(p[on[m]]);
                        tris.push_back(p[on[m + 1]]);
                    }
                }
            }
        }
        return tris;
    }

    std::vector<LLVector3> standingCorners(F32 agent_height)
    {
        const F32 half = llmax(agent_height, 0.3f) * 0.5f + STAND_ABOVE_BOX;
        const F32 lower = -half + STAND_LOWER_OVER_FOOT;
        const F32 widest = llclamp(STAND_WIDEST_Z + (half - MEASURED_HALF) * STAND_WIDEST_RATE, lower + 0.01f, half - 0.01f);
        std::vector<LLVector3> corners;
        auto level = [&](const XY* xy, size_t count, F32 z)
        {
            for (size_t i = 0; i < count; ++i)
            {
                corners.emplace_back(xy[i].x, xy[i].y, llclamp(z, -half, half));
            }
        };
        level(STAND_BOTTOM, std::size(STAND_BOTTOM), -half);
        level(STAND_LOWER, std::size(STAND_LOWER), llmin(lower, widest - 0.01f));
        level(STAND_WIDEST, std::size(STAND_WIDEST), widest);
        level(STAND_TOP, std::size(STAND_TOP), half);
        return corners;
    }
}

namespace skcensus
{
    AvatarPose avatarPose(LLVOAvatar* avatar)
    {
        if (!avatar || !avatar->isSitting())
        {
            return AvatarPose::STANDING;
        }
        return avatar->getParent() ? AvatarPose::SIT_OBJECT : AvatarPose::SIT_GROUND;
    }

    const std::vector<LLVector3>& avatarShape(AvatarPose pose, F32 agent_height)
    {
        static const std::vector<LLVector3> sit_ground(std::begin(SIT_GROUND), std::end(SIT_GROUND));
        static const std::vector<LLVector3> sit_object(std::begin(SIT_OBJECT), std::end(SIT_OBJECT));
        if (pose == AvatarPose::SIT_GROUND)
        {
            return sit_ground;
        }
        if (pose == AvatarPose::SIT_OBJECT)
        {
            return sit_object;
        }
        // Standing, one hull a millimetre of height; there are only as many as there are distinct avatar heights.
        static std::map<S32, std::vector<LLVector3>> standing;
        const S32 mm = (S32)llround(agent_height * 1000.f);
        auto it = standing.find(mm);
        if (it == standing.end())
        {
            if (standing.size() > 512)
            {
                standing.clear();
            }
            it = standing.emplace(mm, hullOf(standingCorners(mm * 0.001f))).first;
        }
        return it->second;
    }
}
