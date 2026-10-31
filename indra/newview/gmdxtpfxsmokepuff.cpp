/**
 * @file gmdxtpfxsmokepuff.cpp
 * @brief GunMetal-DX teleport effect: Smoke puff (a soft, white stage-magic puff in the world).
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

// <GMDX> whole file: see gmdxtpfxeffect.h and gmdxtpfxworld.h. In the world: a soft white flash,
// then round white billows pop out of the avatar one after another into a puffy ball of stage
// smoke (a low ring of it along the floor), a few sparkles twinkle on its surface, and it melts
// away. He is gone inside it once it has closed. Cross-region: the puff plays in the old world, the
// screen sinks into the dark hold, and on arrival a second puff is already full around him when
// the hold lifts. Same region: one puff left where he was, one where he lands.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"
#include "gmdxtpfxworld.h"

#include "llagent.h"

using namespace GMDXTPFX;
using namespace GMDXTPFX::World;

namespace
{
// ---- tunables (s, m for a 1.9 m avatar: everything scales with his height) ----
constexpr F32 HIDE_AT = 0.14f;                        // after the flash: he is gone (the billows have closed)
constexpr F32 SHOW_AT = 0.3f;                         // into the arrival: he is back
constexpr F32 VEIL_FROM = 0.5f, VEIL_TO = 1.f;        // departure progress: the screen sinks into the hold
constexpr F32 UNVEIL_FROM = 0.08f, UNVEIL_TO = 0.5f;  // arrival progress: and comes out of it
constexpr S32 BILLOWS = 26, LOW = 12, SPARKLES = 9;
constexpr F32 POP = 0.08f;                            // the billows pop out over this, one after another
constexpr F32 WHITE_LO = 0.88f, WHITE_HI = 0.97f;     // the billows' white (sRGB; lit by the scene)
const LLColor3 FLASH_LIGHT(1.2f, 1.2f, 1.25f);

class SmokePuff : public Effect
{
public:
    const char* name() const override { return "smokepuff"; }
    bool quick() const override { return true; }
    bool crossRegion() const override { return false; }   // same-region only: they read best in the live world around a short jump
    bool inWorld() const override { return true; }
    F32 departSecs() const override { return 0.9f; }
    const char* departSound() const override { return "tpfx_puff.wav"; }
    const char* arriveSound() const override { return "tpfx_puff.wav"; }

    void begin(const Frame&) override
    {
        mPuffT = mArriveT = -1.f;
    }

    void tick(const Frame& f) override
    {
        if (mPuffT < 0.f)
        {
            mPuffT = f.t;
            puff(f.quick ? f.from : gAgent.getPositionGlobal(), f, false);   // (same region: where he was)
        }
        if (f.arriving && mArriveT < 0.f)
        {
            mArriveT = f.t;
            puff(gAgent.getPositionGlobal(), f, true);
        }
    }

    bool hidesAvatar(const Frame& f) const override
    {
        if (f.alpha < 1.f || mPuffT < 0.f) return false;   // a failed / cancelled teleport fading out: he is back
        if (!f.arriving) return f.t - mPuffT >= HIDE_AT;
        return mArriveT < 0.f || f.t - mArriveT < SHOW_AT;
    }

    void depart(const Frame& f) override
    {
        if (!f.quick) veil(f, f.left ? 1.f : smooth(VEIL_FROM, VEIL_TO, f.u));
    }

    void hold(const Frame& f) override
    {
        if (!f.quick) Effect::hold(f);   // (same region: waiting for the jump, the world stays in view)
    }

    void arrive(const Frame& f) override
    {
        if (!f.quick) veil(f, 1.f - smooth(UNVEIL_FROM, UNVEIL_TO, f.a));
    }

private:
    F32 mPuffT = -1.f, mArriveT = -1.f;   // f.t of the departure puff / the arrival's

    // arrival: the billows start fuller and all at once (he must not show when the hold lifts)
    static void puff(const LLVector3d& at, const Frame& f, bool arrival)
    {
        const Body b = body(at);
        const F32 s = b.scale(), h = b.height;
        const F32 n = f.quick ? 0.75f : 1.f;
        const F32 full = arrival ? 1.3f : 1.f, lag = arrival ? 0.f : 1.f;
        const F32 lit = llclamp(f.intensity, 0.f, 2.f);
        const LLVector3d mid(b.centre.mdV[VX], b.centre.mdV[VY], b.foot + 0.5 * h);

        // the flash
        {
            Part p;
            p.tex = Tex::GLOW;
            p.additive = p.emissive = true;
            p.at = mid;
            p.life = 0.22f;
            p.size0 = 0.5f * h;
            p.size1 = 1.6f * h;
            p.c0 = LLColor4(1.f, 1.f, 1.f, 0.75f);
            p.c1 = LLColor4(1.f, 1.f, 1.f, 0.f);
            p.glow = 0.25f;
            add(p);
        }
        if (lit > 0.f) light(mid, FLASH_LIGHT * (lit * (arrival ? 0.6f : 1.f)), 6.f * s, 0.f, 0.02f, 0.2f);

        auto billow = [&](const LLVector3d& from, const LLVector3& vel, F32 drag, F32 r0, F32 r1, F32 life, F32 delay)
        {
            Part p;
            // (soft smoke, not the lobed billow: the lobes read as a pile of cotton balls)
            p.tex = (Tex)((U8)Tex::SMOKE0 + ll_rand(3));
            p.at = from;
            p.vel = vel;
            p.drag = drag;
            p.accel.set(0.f, 0.f, 0.35f);
            p.wobble = 0.08f * s;
            p.wobbleHz = 0.3f;
            p.ground = b.groundAt(from);
            p.clear = 0.35f;
            p.life = life;
            p.size0 = r0;
            p.size1 = r1;
            p.delay = delay;
            const F32 w = rnd(WHITE_LO, WHITE_HI);
            p.c0 = LLColor4(w, w, w * 1.01f, 0.95f);
            p.c1 = LLColor4(w, w, w * 1.01f, 0.f);
            add(p);
        };
        // the ball: billows popping out of him in every direction (mostly sideways and up)
        for (S32 i = 0, m = llmax(1, (S32)(BILLOWS * n + 0.5f)); i < m; ++i)
        {
            LLVector3 d = randomDir(-0.25f);
            d.mV[VZ] *= 0.8f;
            d.normVec();
            billow(mid + LLVector3d(d * (0.3f * s)), d * (rnd(2.6f, 4.f) * s), 4.5f, rnd(0.5f, 0.7f) * s * full,
                   rnd(1.6f, 2.1f) * s, rnd(1.5f, 2.1f), lag * (0.02f + POP * (F32)i / (F32)m));
        }
        // a low ring along the floor
        for (S32 i = 0, m = llmax(1, (S32)(LOW * n + 0.5f)); i < m; ++i)
        {
            const F32 a = F_TWO_PI * ((F32)i + rnd(-0.3f, 0.3f)) / (F32)m;
            const LLVector3 d(cosf(a), sinf(a), 0.f);
            const LLVector3d from(b.centre.mdV[VX] + d.mV[VX] * 0.3f * s, b.centre.mdV[VY] + d.mV[VY] * 0.3f * s, b.foot + 0.3 * s);
            billow(from, d * (rnd(3.f, 4.2f) * s) + LLVector3(0.f, 0.f, 0.3f), 4.f, 0.6f * s * full, 2.2f * s, rnd(1.4f, 1.9f),
                   lag * rnd(0.03f, 0.08f));
        }
        // sparkles twinkling on its surface (their bloom shows through the white)
        for (S32 i = 0, m = llmax(1, (S32)(SPARKLES * n + 0.5f)); i < m; ++i)
        {
            Part p;
            p.tex = Tex::STAR;
            p.additive = p.emissive = true;
            p.at = mid + LLVector3d(randomDir(-0.2f) * (rnd(0.7f, 1.2f) * 0.6f * h));
            p.vel.set(0.f, 0.f, 0.25f);
            p.drag = 0.5f;
            p.life = rnd(0.28f, 0.4f);
            p.size0 = rnd(0.22f, 0.32f) * s;
            p.size1 = 0.04f * s;
            p.c0 = LLColor4(1.f, 0.97f, 0.86f, 1.f);
            p.c1 = LLColor4(1.f, 0.9f, 0.7f, 0.2f);
            p.glow = 0.6f;
            p.delay = rnd(0.06f, 0.65f);
            add(p);
        }
    }
};
} // namespace

GMDXTPFX::Effect& tpfxSmokePuff()
{
    static SmokePuff e;
    return e;
}
