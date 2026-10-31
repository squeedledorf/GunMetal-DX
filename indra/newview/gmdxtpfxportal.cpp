/**
 * @file gmdxtpfxportal.cpp
 * @brief GunMetal-DX teleport effect: Portal swirl (the frame twists into a vortex and a glowing ring).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> whole file: see gmdxtpfxeffect.h. Departure: the frozen frame twists into a spiral vortex
// about the avatar's place on screen (the centre when it is not in view), the twist tighter toward
// the middle, and is swallowed by a glowing violet ring. Hold: the ring, dim, slowly turning in the
// dark. Arrival: the ring opens out over the new scene, its spiral edge unwinding as it goes.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llrender2dutils.h"

using namespace GMDXTPFX;

namespace
{
// ---- tunables ----
constexpr F32 TWIST = 7.f;          // radians the middle turns by the end (times GMDXTPFXIntensity)
constexpr F32 SPIN = 1.2f;          // ... and the whole frame
constexpr F32 RING_R = 0.09f;       // the ring's radius in the hold, screen heights
constexpr F32 RING_W = 0.012f;      // its half-width, screen heights
constexpr F32 RING_ROT = 1.2f;      // its bright arcs turn this fast, radians per second
constexpr F32 HOLD_A = 0.6f;        // its opacity in the hold
constexpr F32 OPEN_SOFT = 0.1f;     // the opening's soft edge, screen heights
constexpr F32 OPEN_WAVE = 0.3f;     // how far the opening's spiral arms reach in and out (a fraction of its radius)
constexpr S32 ARMS = 3;             // bright arcs on the ring, spiral arms on the opening
constexpr S32 RINGS = 28, SECTORS = 64;   // the vortex mesh
const LLColor4 VOID_BG(0.03f, 0.025f, 0.05f, 1.f);
const LLColor4 GLOW(0.62f, 0.45f, 1.f, 1.f);

F32 phase(F32 t) { return -RING_ROT * ARMS * t; }   // (the arcs turn the way the twist does)

// A soft ring of radius r, `width` either side, brighter in ARMS arcs.
void drawRing(const LLVector2& c, F32 r, F32 width, F32 ph, F32 a)
{
    if (a <= 0.004f) return;
    constexpr S32 SEG = 72;
    const F32 r0 = llmax(0.f, r - width), r2 = r + width;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 k = 0; k < SEG; ++k)
    {
        const F32 th[2] = { F_TWO_PI * k / SEG, F_TWO_PI * (k + 1) / SEG };
        LLVector2 in[2], mid[2], out[2];
        LLColor4 core[2], edge[2];
        for (S32 i = 0; i < 2; ++i)
        {
            const LLVector2 d(cosf(th[i]), sinf(th[i]));
            in[i] = c + d * r0;
            mid[i] = c + d * r;
            out[i] = c + d * r2;
            const F32 b = 0.4f + 0.6f * (0.5f + 0.5f * sinf(ARMS * th[i] + ph));
            core[i] = lerp(GLOW, LLColor4::white, 0.35f * b);
            core[i].mV[VALPHA] = a * b;
            edge[i] = GLOW;
            edge[i].mV[VALPHA] = 0.f;
        }
        auto tri = [](const LLVector2& p, const LLColor4& pc, const LLVector2& q, const LLColor4& qc, const LLVector2& s, const LLColor4& sc)
        {
            gGL.color4fv(pc.mV); gGL.vertex2f(p.mV[VX], p.mV[VY]);
            gGL.color4fv(qc.mV); gGL.vertex2f(q.mV[VX], q.mV[VY]);
            gGL.color4fv(sc.mV); gGL.vertex2f(s.mV[VX], s.mV[VY]);
        };
        for (const LLVector2* band : { in, out })   // inside edge -> core, core -> outside edge
        {
            tri(band[0], edge[0], mid[0], core[0], mid[1], core[1]);
            tri(band[0], edge[0], mid[1], core[1], band[1], edge[1]);
        }
    }
    gGL.end();
}

// The faint glow inside the ring.
void drawCore(const LLVector2& c, F32 r, F32 a)
{
    LLColor4 in = GLOW, out = GLOW;
    in.mV[VALPHA] = a;
    out.mV[VALPHA] = 0.f;
    gGL.pushUIMatrix();
    gGL.translateUI(c.mV[VX], c.mV[VY], 0.f);
    gl_washer_2d(r, 0.f, 48, in, out);
    gGL.popUIMatrix();
}

class Portal : public Effect
{
public:
    const char* name() const override { return "portal"; }
    F32 departSecs() const override { return 1.2f; }
    const char* departSound() const override { return "tpfx_portal.wav"; }

    void begin(const Frame& f) override { mFrom = f.avatar; }

    void depart(const Frame& f) override
    {
        LLColor4 bg = VOID_BG;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        const LLVector2 c = f.avatar;
        const F32 rmax = farCorner(f, c);
        const F32 e = smooth(0.f, 1.f, f.u), pull = smooth(0.1f, 0.95f, f.u);
        const F32 twist = TWIST * f.intensity * e, fade = 1.f - smooth(0.8f, 1.f, f.u);
        drawFrozenPolar(f, c, rmax, RINGS, SECTORS, [&](F32 r, F32 ang, LLVector2& p, LLColor4& col)
        {
            const F32 in = 1.f - r / rmax;
            const F32 th = ang + SPIN * e + twist * in * in;
            p = c + LLVector2(cosf(th), sinf(th)) * (r * (1.f - pull));
            const F32 a = col.mV[VALPHA] * fade;
            col = lerp(LLColor4::white, GLOW, 0.6f * e * in);
            col.mV[VALPHA] = a;
        });
        const F32 ring_a = f.alpha * smooth(0.25f, 0.7f, f.u) * lerp(1.f, HOLD_A, smooth(0.75f, 1.f, f.u));
        const F32 r = lerp(0.5f * f.h, RING_R * f.h, smooth(0.2f, 0.9f, f.u));
        drawCore(c, r, 0.25f * ring_a);
        drawRing(c, r, 3.f * RING_W * f.h, phase(f.t), 0.3f * ring_a);
        drawRing(c, r, RING_W * f.h, phase(f.t), ring_a);
    }

    void hold(const Frame& f) override
    {
        LLColor4 bg = VOID_BG;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        const F32 a = f.alpha * HOLD_A * (0.9f + 0.1f * sinf(f.t * 1.5f));
        drawCore(mFrom, RING_R * f.h, 0.15f * a);
        drawRing(mFrom, RING_R * f.h, 3.f * RING_W * f.h, phase(f.t), 0.3f * a);
        drawRing(mFrom, RING_R * f.h, RING_W * f.h, phase(f.t), a);
    }

    void arrive(const Frame& f) override
    {
        const F32 e = smooth(0.f, 1.f, f.a);
        const LLVector2 c = lerp(mFrom, f.avatar, smooth(0.f, 0.35f, f.a));   // (the avatar's new place)
        const F32 rmax = farCorner(f, c) * 1.05f, soft = OPEN_SOFT * f.h;
        const F32 open = lerp(RING_R * f.h, rmax * 1.25f + soft, e), unwind = TWIST * (1.f - e);
        // the dark around the opening, its edge in spiral arms that straighten out as it opens
        constexpr S32 CR = 24;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        auto vtx = [&](S32 i, S32 j)
        {
            const F32 r = rmax * (F32)i / CR, th = F_TWO_PI * (F32)j / SECTORS;
            const F32 edge = open * (1.f + OPEN_WAVE * (1.f - e) * sinf(ARMS * th + unwind * (r / rmax) + phase(f.t)));
            LLColor4 col = VOID_BG;
            col.mV[VALPHA] = f.alpha * smooth(edge - soft, edge, r);
            gGL.color4fv(col.mV);
            gGL.vertex2f(c.mV[VX] + r * cosf(th), c.mV[VY] + r * sinf(th));
        };
        for (S32 i = 0; i < CR; ++i)
            for (S32 j = 0; j < SECTORS; ++j)
            {
                vtx(i, j); vtx(i, j + 1); vtx(i + 1, j + 1);
                vtx(i, j); vtx(i + 1, j + 1); vtx(i + 1, j);
            }
        gGL.end();
        // the ring rides the opening, flaring a little, then fades
        const F32 a = f.alpha * lerp(HOLD_A, 1.f, smooth(0.f, 0.15f, f.a)) * (1.f - smooth(0.4f, 1.f, f.a));
        drawRing(c, open, 3.f * RING_W * f.h * (1.f + 2.f * e), phase(f.t), 0.3f * a);
        drawRing(c, open, RING_W * f.h * (1.f + 2.f * e), phase(f.t), a);
    }

private:
    LLVector2 mFrom;   // where the vortex was (the avatar at departure)
};
} // namespace

GMDXTPFX::Effect& tpfxPortal()
{
    static Portal e;
    return e;
}
