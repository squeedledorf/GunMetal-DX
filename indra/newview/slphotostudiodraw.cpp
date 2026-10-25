/**
 * @file slphotostudiodraw.cpp
 * @brief Anti-aliased 2D drawing for Photo Studio's graphs, wheels and scopes:
 *        a mitred polyline, the area under a polyline, and a washer whose
 *        colour varies around its sweep.
 *
 * Slipstream Photo Studio: these were upstream Alchemy additions to
 * llrender2dutils (ported with the Lightbox, credited); they moved here so
 * llrender is back to Linden's own helpers (#443).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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
#include "slphotostudiodraw.h"

#include "llmath.h"
#include "llrender.h"

#include <cmath>

void sl_polyline_2d(const std::vector<LLVector2>& points, const LLColor4& color, F32 width, bool closed)
{
    const size_t count = points.size();
    if (count < 2)
    {
        return;
    }

    const F32 half = llmax(width, 0.1f) * 0.5f;
    // One pixel of falloff either side. Wider reads as a blurred line rather
    // than a smooth one; narrower leaves the stair steps visible.
    constexpr F32 FEATHER = 1.0f;
    // A mitre grows as 1/cos(theta/2), so a hairpin would send the corner off
    // the widget. Past this the join is simply blunted; on a curve sampled
    // densely enough to be smooth the clamp never engages.
    constexpr F32 MITRE_LIMIT = 4.0f;

    const size_t segments = closed ? count : count - 1;

    // Per-vertex mitre normals, so consecutive segments share their corner
    // vertices exactly and the ribbon has no notches at the joins.
    std::vector<LLVector2> normals(count);
    for (size_t i = 0; i < count; ++i)
    {
        const bool has_prev = closed || i > 0;
        const bool has_next = closed || i + 1 < count;

        LLVector2 n_prev(0.f, 0.f);
        LLVector2 n_next(0.f, 0.f);

        if (has_prev)
        {
            const LLVector2& p = points[(i + count - 1) % count];
            LLVector2 d = points[i] - p;
            if (d.lengthSquared() > 0.f)
            {
                d.normalize();
                n_prev.set(-d.mV[VY], d.mV[VX]);
            }
        }
        if (has_next)
        {
            LLVector2 d = points[(i + 1) % count] - points[i];
            if (d.lengthSquared() > 0.f)
            {
                d.normalize();
                n_next.set(-d.mV[VY], d.mV[VX]);
            }
        }

        LLVector2 n = n_prev + n_next;
        if (n.lengthSquared() <= 0.f)
        {
            // An end point (one neighbour) or a doubled-back segment; fall
            // back to whichever side actually has a direction.
            n = has_next ? n_next : n_prev;
        }
        if (n.lengthSquared() <= 0.f)
        {
            normals[i].set(0.f, 0.f);
            continue;
        }
        n.normalize();

        // Lengthen the mitre so the ribbon keeps a constant apparent width
        // through the turn rather than pinching.
        const LLVector2& reference = (n_next.lengthSquared() > 0.f) ? n_next : n_prev;
        const F32 cos_half = n * reference;
        const F32 scale = (cos_half > 1.f / MITRE_LIMIT) ? (1.f / cos_half) : MITRE_LIMIT;
        normals[i] = n * scale;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const LLColor4 edge(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.f);

    // TRIANGLES rather than a strip: LLRender auto-flushes this mode on a
    // multiple of three, so a long polyline cannot overrun the immediate-mode
    // vertex buffer and lose its tail the way an unsplittable strip would.
    gGL.begin(LLRender::TRIANGLES);
    for (size_t s = 0; s < segments; ++s)
    {
        const size_t i0 = s;
        const size_t i1 = (s + 1) % count;
        const LLVector2& p0 = points[i0];
        const LLVector2& p1 = points[i1];
        if ((p1 - p0).lengthSquared() <= 0.f)
        {
            continue;
        }
        const LLVector2& n0 = normals[i0];
        const LLVector2& n1 = normals[i1];

        // Three bands per segment: the opaque core, and a fading skirt on
        // each side. The skirts are what actually anti-alias the edge.
        const F32 offsets[4] = { -(half + FEATHER), -half, half, half + FEATHER };
        const LLColor4* colors[4] = { &edge, &color, &color, &edge };

        for (S32 band = 0; band < 3; ++band)
        {
            const LLVector2 a0 = p0 + n0 * offsets[band];
            const LLVector2 a1 = p1 + n1 * offsets[band];
            const LLVector2 b0 = p0 + n0 * offsets[band + 1];
            const LLVector2 b1 = p1 + n1 * offsets[band + 1];
            const LLColor4& ca = *colors[band];
            const LLColor4& cb = *colors[band + 1];

            gGL.color4fv(ca.mV); gGL.vertex2f(a0.mV[VX], a0.mV[VY]);
            gGL.color4fv(ca.mV); gGL.vertex2f(a1.mV[VX], a1.mV[VY]);
            gGL.color4fv(cb.mV); gGL.vertex2f(b1.mV[VX], b1.mV[VY]);

            gGL.color4fv(ca.mV); gGL.vertex2f(a0.mV[VX], a0.mV[VY]);
            gGL.color4fv(cb.mV); gGL.vertex2f(b1.mV[VX], b1.mV[VY]);
            gGL.color4fv(cb.mV); gGL.vertex2f(b0.mV[VX], b0.mV[VY]);
        }
    }
    gGL.end();
    gGL.flush();
}

void sl_polyfill_2d(const std::vector<LLVector2>& points, F32 baseline_y, const LLColor4& color)
{
    const size_t count = points.size();
    if (count < 2)
    {
        return;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // TRIANGLES, not a strip, for the reason sl_polyline_2d gives: LLRender
    // auto-flushes this mode on a multiple of three, so a densely sampled curve
    // cannot overrun the immediate-mode buffer and lose its tail.
    gGL.begin(LLRender::TRIANGLES);
    gGL.color4fv(color.mV);
    for (size_t i = 0; i + 1 < count; ++i)
    {
        const LLVector2& p0 = points[i];
        const LLVector2& p1 = points[i + 1];

        // Degenerate where the curve touches the baseline, which is the common
        // case for a weight band outside its range. Harmless, and cheaper to
        // emit than to test for.
        gGL.vertex2f(p0.mV[VX], baseline_y);
        gGL.vertex2f(p1.mV[VX], baseline_y);
        gGL.vertex2f(p1.mV[VX], p1.mV[VY]);

        gGL.vertex2f(p0.mV[VX], baseline_y);
        gGL.vertex2f(p1.mV[VX], p1.mV[VY]);
        gGL.vertex2f(p0.mV[VX], p0.mV[VY]);
    }
    gGL.end();
    gGL.flush();
}

void sl_washer_angular_2d(F32 outer_radius, F32 inner_radius,
                          const std::vector<LLColor4>& colors, F32 inner_fade)
{
    const size_t steps = colors.size();
    if (steps < 3)
    {
        return;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // TRIANGLES rather than a strip, for the same reason sl_polyline_2d does:
    // LLRender auto-flushes this mode on a multiple of three, so a finely
    // stepped ring cannot overrun the immediate-mode buffer and lose its tail.
    gGL.begin(LLRender::TRIANGLES);
    // Step i's leading edge is step i-1's trailing edge, so each edge is
    // computed once and carried across.
    F32 c0 = 1.f, s0 = 0.f;
    LLColor4 in0(colors[0]);  in0.mV[VALPHA] *= inner_fade;
    for (size_t i = 0; i < steps; ++i)
    {
        const size_t j = (i + 1) % steps;
        const F32 a1 = F_TWO_PI * (F32)j / (F32)steps;
        const F32 c1 = cosf(a1), s1 = sinf(a1);
        LLColor4 in1(colors[j]);  in1.mV[VALPHA] *= inner_fade;

        // Outer i -> outer j -> inner j, then outer i -> inner j -> inner i.
        gGL.color4fv(colors[i].mV); gGL.vertex2f(outer_radius * c0, outer_radius * s0);
        gGL.color4fv(colors[j].mV); gGL.vertex2f(outer_radius * c1, outer_radius * s1);
        gGL.color4fv(in1.mV);       gGL.vertex2f(inner_radius * c1, inner_radius * s1);

        gGL.color4fv(colors[i].mV); gGL.vertex2f(outer_radius * c0, outer_radius * s0);
        gGL.color4fv(in1.mV);       gGL.vertex2f(inner_radius * c1, inner_radius * s1);
        gGL.color4fv(in0.mV);       gGL.vertex2f(inner_radius * c0, inner_radius * s0);

        c0 = c1;
        s0 = s1;
        in0 = in1;
    }
    gGL.end();
    gGL.flush();
}
