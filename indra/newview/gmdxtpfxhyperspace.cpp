/**
 * @file gmdxtpfxhyperspace.cpp
 * @brief GunMetal-DX teleport effect: Hyperspace (a lens-warp jump to light speed).
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

// <GMDX> whole file: see gmdxtpfxeffect.h. Departure: the frozen frame zooms in and stretches out
// from the screen centre like a lens warp while star streaks shoot out of the middle, longer and
// faster, then a brief white-out at the jump. Hold: dark, slow streaks drifting out. Arrival: the
// streaks shrink to points, a brief white, and the new scene comes up out of it.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llrand.h"

using namespace GMDXTPFX;

namespace
{
// ---- tunables ----
constexpr S32 STREAKS = 180;
constexpr F32 ZOOM = 0.35f;         // the frame's zoom by the jump (times GMDXTPFXIntensity)
constexpr F32 STRETCH = 1.2f;       // ... and how much more its outside is pulled out
constexpr F32 BOOST = 1.5f;         // how far the stars travel in the departure (wraps of the field)
constexpr F32 DRIFT = 0.1f;         // ... and per second the rest of the time
constexpr F32 MAX_LEN = 0.7f;       // a streak's length at speed, a fraction of its distance from the centre
constexpr F32 HOLD_LEN = 0.06f;     // ... drifting
constexpr F32 JUMP = 0.88f;         // depart progress of the white-out's peak
constexpr F32 WHITE = 0.8f;         // its peak opacity (and the arrival's)
constexpr S32 RINGS = 20, SECTORS = 48;   // the warp mesh
const LLColor4 STAR(0.8f, 0.88f, 1.f, 1.f);
const LLColor4 SPACE(0.01f, 0.012f, 0.025f, 1.f);

struct Streak { F32 ang, phase, speed, width; };

class Hyperspace : public Effect
{
public:
    const char* name() const override { return "hyperspace"; }
    F32 departSecs() const override { return 1.2f; }
    const char* departSound() const override { return "tpfx_hyperspace.wav"; }
    const char* arriveSound() const override { return "tpfx_hyperspace_out.wav"; }

    void begin(const Frame&) override
    {
        mStreaks.clear();
        for (S32 i = 0; i < STREAKS; ++i) mStreaks.push_back({ rnd(0.f, F_TWO_PI), ll_frand(), rnd(0.7f, 1.3f), rnd(1.f, 2.5f) });
    }

    void depart(const Frame& f) override
    {
        LLColor4 bg = SPACE;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        const LLVector2 c(0.5f * f.w, 0.5f * f.h);
        const F32 rmax = farCorner(f, c);
        const F32 e = smooth(0.f, 1.f, f.u), fade = 1.f - smooth(0.78f, 0.9f, f.u);
        const F32 zoom = ZOOM * f.intensity * e, stretch = STRETCH * f.intensity * e;
        drawFrozenPolar(f, c, rmax, RINGS, SECTORS, [&](F32 r, F32 ang, LLVector2& p, LLColor4& col)
        {
            const F32 k = r / rmax;
            p = c + LLVector2(cosf(ang), sinf(ang)) * (r * (1.f + zoom) * (1.f + stretch * k * k));
            const F32 a = col.mV[VALPHA] * fade;
            col = lerp(LLColor4::white, STAR, 0.6f * e);
            col.mV[VALPHA] = a;
        });
        const F32 len = lerp(lerp(HOLD_LEN, MAX_LEN, smooth(0.1f, 0.85f, f.u)), HOLD_LEN, smooth(0.9f, 1.f, f.u));
        drawStreaks(f, c, rmax, travel(f.t, f.u), len, f.alpha * smooth(0.05f, 0.35f, f.u));
        fill(f, LLColor4(1.f, 1.f, 1.f, f.alpha * WHITE * smooth(0.72f, JUMP, f.u) * (1.f - smooth(JUMP, 1.f, f.u))));
    }

    void hold(const Frame& f) override
    {
        LLColor4 bg = SPACE;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        const LLVector2 c(0.5f * f.w, 0.5f * f.h);
        drawStreaks(f, c, farCorner(f, c), travel(f.t, 1.f), HOLD_LEN, 0.7f * f.alpha);
    }

    void arrive(const Frame& f) override
    {
        LLColor4 bg = SPACE;
        bg.mV[VALPHA] = f.alpha * (1.f - smooth(0.25f, 0.35f, f.a));   // (gone under the white)
        fill(f, bg);
        const LLVector2 c(0.5f * f.w, 0.5f * f.h);
        const F32 shrink = 1.f - smooth(0.f, 0.3f, f.a);
        drawStreaks(f, c, farCorner(f, c), travel(f.t, 1.f), HOLD_LEN * shrink, 0.7f * f.alpha * shrink);
        fill(f, LLColor4(1.f, 1.f, 1.f, f.alpha * WHITE * smooth(0.12f, 0.3f, f.a) * (1.f - smooth(0.3f, 0.85f, f.a))));
    }

    void end() override { mStreaks.clear(); }

private:
    std::vector<Streak> mStreaks;

    // how far the field has moved (continuous from the departure into the hold)
    static F32 travel(F32 t, F32 u) { return DRIFT * t + BOOST * u * u * u; }

    // Streaks from the centre: each at p^2 of rmax out (p: its place in the field, 0..1, wrapping),
    // `len` of that long behind it, fading in from the tail and with distance.
    void drawStreaks(const Frame& f, const LLVector2& c, F32 rmax, F32 s, F32 len, F32 alpha) const
    {
        if (alpha <= 0.004f) return;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (const Streak& k : mStreaks)
        {
            F32 p = k.phase + s * k.speed;
            p -= floorf(p);
            const F32 head = rmax * p * p, tail = head * (1.f - len);
            if (head < 1.f) continue;
            const LLVector2 d(cosf(k.ang), sinf(k.ang)), n(-d.mV[VY] * 0.5f * k.width, d.mV[VX] * 0.5f * k.width);
            const LLVector2 h = c + d * llmax(head, tail + 1.5f), t = c + d * tail;
            LLColor4 hc = STAR, tc = STAR;
            hc.mV[VALPHA] = alpha * llmin(1.f, 0.2f + 1.2f * p);
            tc.mV[VALPHA] = 0.f;
            const LLVector2 q[4] = { t + n, t - n, h - n, h + n };
            const LLColor4* qc[4] = { &tc, &tc, &hc, &hc };
            for (S32 i : { 0, 1, 2, 0, 2, 3 })
            {
                gGL.color4fv(qc[i]->mV);
                gGL.vertex2f(q[i].mV[VX], q[i].mV[VY]);
            }
        }
        gGL.end();
    }
};
} // namespace

GMDXTPFX::Effect& tpfxHyperspace()
{
    static Hyperspace e;
    return e;
}
