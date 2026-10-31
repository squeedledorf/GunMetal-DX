/**
 * @file gmdxtpfxninjasmoke.cpp
 * @brief GunMetal-DX teleport effect: Ninja smoke (a burst of charcoal smoke in the world, a poof, flung leaves).
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

// <GMDX> whole file: see gmdxtpfxeffect.h and gmdxtpfxworld.h. In the world: a bright poof at the
// avatar's chest (a white flash and a short warm light inside the smoke), then a thick, dark
// charcoal burst rolling out around him along the ground and at every height, churning as it
// spreads and settles, a plume rising off the top; leaves, paper scraps and dark specks are flung
// out and flutter down. He is gone inside it a moment after the poof.
// Cross-region: the burst plays in the old world while the server works and the screen sinks into
// the dark hold; on arrival a second burst is already thick around him when the hold lifts, and he
// is back as it thins. Same region: the cloud is left hanging where he was and a second one bursts
// where he lands.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"
#include "gmdxtpfxworld.h"

#include "llagent.h"
#include "llrand.h"

using namespace GMDXTPFX;
using namespace GMDXTPFX::World;

namespace
{
// ---- tunables (s, m for a 1.9 m avatar: everything scales with his height) ----
constexpr F32 HIDE_AT = 0.12f;                        // after the poof: he is gone (the smoke is thick)
constexpr F32 SHOW_AT = 0.3f;                         // into the arrival: he is back, inside the cloud
constexpr F32 VEIL_FROM = 0.45f, VEIL_TO = 1.f;       // departure progress: the screen sinks into the hold
constexpr F32 UNVEIL_FROM = 0.08f, UNVEIL_TO = 0.5f;  // arrival progress: and comes out of it
constexpr S32 RING = 30, BODY = 26, PLUME = 9, CORE = 5;   // puffs: along the ground, around him, rising, the poof's
constexpr S32 SHROUD = 14;                             // slow, dense puffs that stay wrapped round him (they hide him)
constexpr S32 LEAVES = 5, SCRAPS = 3, SPECKS = 8;
constexpr F32 SMOKE_DARK = 0.07f, SMOKE_LIGHT = 0.13f; // the puffs' grey (sRGB; lit by the scene)
const LLColor3 POOF_LIGHT(1.6f, 1.5f, 1.35f);         // the light inside the poof (linear, intensity folded in)

class NinjaSmoke : public Effect
{
public:
    const char* name() const override { return "ninjasmoke"; }
    bool quick() const override { return true; }
    bool crossRegion() const override { return false; }   // same-region only: they read best in the live world around a short jump
    bool inWorld() const override { return true; }
    const char* departSound() const override { return "tpfx_smoke.wav"; }
    const char* arriveSound() const override { return "tpfx_smoke.wav"; }

    void begin(const Frame&) override
    {
        mBurstT = mArriveT = -1.f;
    }

    void tick(const Frame& f) override
    {
        if (mBurstT < 0.f)
        {
            mBurstT = f.t;
            burst(f.quick ? f.from : gAgent.getPositionGlobal(), f, false);   // (same region: where he was)
        }
        if (f.arriving && mArriveT < 0.f)
        {
            mArriveT = f.t;
            burst(gAgent.getPositionGlobal(), f, true);
        }
    }

    bool hidesAvatar(const Frame& f) const override
    {
        if (f.alpha < 1.f || mBurstT < 0.f) return false;   // a failed / cancelled teleport fading out: he is back
        if (!f.arriving) return f.t - mBurstT >= HIDE_AT;
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
    F32 mBurstT = -1.f, mArriveT = -1.f;   // f.t of the departure burst / the arrival's

    // arrival: the cloud starts thick (he must not show when the hold lifts) and without the delay
    // that lets the poof be seen first
    static void burst(const LLVector3d& at, const Frame& f, bool arrival)
    {
        const Body b = body(at);
        const F32 s = b.scale(), h = b.height;
        const F32 n = 1.f;   // (same-region only now: the quick burst is the whole show)
        const F32 thick = arrival ? 1.35f : 1.f, lag = arrival ? 0.f : 1.f;
        const F32 lit = llclamp(f.intensity, 0.f, 2.f);
        auto count = [n](S32 c) { return llmax(1, (S32)(c * n + 0.5f)); };
        const LLVector3d chest(b.centre.mdV[VX], b.centre.mdV[VY], b.foot + 0.55 * h);
        auto around = [&](const LLVector3& d, F32 r, F64 z) { return LLVector3d(b.centre.mdV[VX] + d.mV[VX] * r, b.centre.mdV[VY] + d.mV[VY] * r, z); };
        auto flat = [](F32 a) { return LLVector3(cosf(a), sinf(a), 0.f); };

        // the poof: a white flash (seen for the few frames before the smoke closes over it) and a
        // short warm light that lights the smoke from inside
        for (S32 k = 0; k < 2; ++k)
        {
            Part p;
            p.tex = Tex::GLOW;
            p.additive = p.emissive = true;
            p.at = chest;
            p.life = 0.2f + 0.08f * k;
            p.size0 = (0.35f + 0.1f * k) * h;
            p.size1 = (1.3f + 0.3f * k) * h;
            p.c0 = LLColor4(1.f, 1.f, 1.f, 0.85f - 0.3f * k);
            p.c1 = LLColor4(1.f, 0.96f, 0.9f, 0.f);
            p.glow = 0.3f;
            add(p);
        }
        if (lit > 0.f) light(chest, POOF_LIGHT * (lit * (arrival ? 0.6f : 1.f)), 8.f * s, 0.f, 0.02f, 0.16f);

        auto puff = [&](const LLVector3d& from, const LLVector3& vel, F32 drag, F32 rise, F32 r0, F32 r1, F32 life,
                        F32 grey, F32 alpha, F32 delay)
        {
            Part p;
            p.tex = (Tex)((U8)Tex::SMOKE0 + ll_rand(3));
            p.at = from;
            p.vel = vel;
            p.drag = drag;
            p.accel.set(0.f, 0.f, rise);
            p.wobble = rnd(0.12f, 0.28f) * s;
            p.wobbleHz = rnd(0.25f, 0.5f);
            p.ground = b.groundAt(from);
            p.clear = 0.32f;
            p.life = life;
            p.size0 = r0;
            p.size1 = r1;
            p.delay = delay;
            const F32 g1 = grey + 0.06f;
            p.c0 = LLColor4(grey, grey, grey * 1.05f, alpha);
            p.c1 = LLColor4(g1, g1, g1 * 1.05f, 0.f);
            add(p);
        };
        // rolling out along the ground
        for (S32 i = 0, m = count(RING); i < m; ++i)
        {
            const LLVector3 d = flat(F_TWO_PI * ((F32)i + rnd(-0.4f, 0.4f)) / (F32)m);
            puff(around(d, 0.25f * s, b.foot + 0.3 * s), d * (rnd(3.5f, 6.f) * s) + LLVector3(0.f, 0.f, rnd(0.2f, 0.6f)), 3.5f, 0.5f,
                 rnd(0.7f, 1.f) * s * thick, rnd(2.2f, 3.f) * s, rnd(2.4f, 3.2f), rnd(SMOKE_DARK, SMOKE_LIGHT), 0.85f,
                 lag * rnd(0.03f, 0.08f));
        }
        // out of him at every height
        for (S32 i = 0, m = count(BODY); i < m; ++i)
        {
            const LLVector3 d = flat(rnd(0.f, F_TWO_PI));
            puff(around(d, rnd(0.1f, 0.3f) * s, b.foot + rnd(0.15f, 0.95f) * h), d * (rnd(1.5f, 3.2f) * s) + LLVector3(0.f, 0.f, rnd(0.2f, 0.8f)),
                 3.f, 0.6f, rnd(0.8f, 1.1f) * s * thick, rnd(2.f, 2.8f) * s, rnd(2.f, 2.8f), rnd(SMOKE_DARK, SMOKE_LIGHT), 0.9f,
                 lag * rnd(0.02f, 0.06f));
        }
        // the shroud: slow, dense puffs that stay wrapped round him at every height (what hides him
        // and his tag; the rolling ring and body puffs move out and thin)
        for (S32 i = 0; i < SHROUD; ++i)
        {
            const LLVector3 d = flat(F_TWO_PI * ((F32)i + rnd(-0.3f, 0.3f)) / (F32)SHROUD);
            puff(around(d, rnd(0.05f, 0.2f) * s, b.foot + (0.1f + 0.95f * (F32)(i % 7) / 6.f) * h),
                 d * (rnd(0.3f, 0.8f) * s) + LLVector3(0.f, 0.f, rnd(0.05f, 0.25f)), 2.5f, 0.15f,
                 rnd(1.1f, 1.4f) * s * thick, rnd(2.f, 2.5f) * s, rnd(1.9f, 2.5f), rnd(SMOKE_DARK, SMOKE_LIGHT), 0.97f,
                 lag * rnd(0.01f, 0.04f));
        }
        // a plume rising off the top
        for (S32 i = 0, m = count(PLUME); i < m; ++i)
        {
            const LLVector3 d = flat(rnd(0.f, F_TWO_PI));
            puff(around(d, 0.15f * s, b.foot + rnd(0.8f, 1.05f) * h), d * (rnd(0.5f, 1.2f) * s) + LLVector3(0.f, 0.f, rnd(1.2f, 2.f) * s),
                 1.6f, 0.3f, 0.9f * s * thick, 2.6f * s, rnd(2.6f, 3.2f), rnd(SMOKE_DARK, SMOKE_LIGHT), 0.8f, lag * rnd(0.04f, 0.12f));
        }
        // the poof's paler core, soon swallowed
        for (S32 i = 0; i < CORE; ++i)
        {
            const LLVector3 d = randomDir();
            puff(chest + LLVector3d(d * (0.2f * s)), d * 0.8f, 3.f, 0.4f, 0.6f * s, 1.6f * s, rnd(0.7f, 0.9f), 0.5f, 0.7f, 0.f);
        }

        // debris, flung up and out, fluttering down and lying where it lands (fewer on arrival)
        const F32 dn = arrival ? 0.5f : 1.f;
        auto scrap = [&](Tex tex, const LLColor4& c, const LLColor4& c_end, F32 size, F32 drag, F32 fall, F32 flip, F32 sway)
        {
            Part p;
            p.tex = tex;
            const LLVector3 d = flat(rnd(0.f, F_TWO_PI));
            p.at = around(d, 0.2f * s, b.foot + rnd(0.6f, 1.1f) * h);
            p.vel = d * (rnd(2.5f, 5.f) * s) + LLVector3(0.f, 0.f, rnd(2.5f, 4.5f) * s);
            p.drag = drag;
            p.accel.set(0.f, 0.f, -fall);
            p.wobble = sway * s;
            p.wobbleHz = rnd(0.6f, 1.f);
            p.ground = b.groundAt(p.at + LLVector3d(d * (2.f * s)));
            p.clear = 0.02f;   // (lands)
            p.flip = flip;
            p.life = rnd(2.6f, 3.4f);
            p.size0 = p.size1 = size * s;
            p.c0 = c;
            p.c1 = c_end;
            p.delay = lag * rnd(0.02f, 0.08f);
            add(p);
        };
        for (S32 i = 0, m = llmax(1, (S32)(count(LEAVES) * dn)); i < m; ++i)
        {
            const LLColor4 c = (i % 2) ? LLColor4(0.52f, 0.37f, 0.18f, 1.f) : LLColor4(0.36f, 0.44f, 0.18f, 1.f);   // dry / green
            scrap(Tex::LEAF, c, c, rnd(0.1f, 0.14f), 2.4f, 3.6f, rnd(0.18f, 0.28f), 0.3f);
        }
        for (S32 i = 0, m = llmax(1, (S32)(count(SCRAPS) * dn)); i < m; ++i)
        {
            const LLColor4 c(0.88f, 0.86f, 0.8f, 1.f);
            scrap(Tex::PAPER, c, c, rnd(0.08f, 0.11f), 2.8f, 3.8f, rnd(0.14f, 0.22f), 0.25f);
        }
        for (S32 i = 0, m = (S32)(count(SPECKS) * dn); i < m; ++i)
            scrap(Tex::GLOW, LLColor4(0.06f, 0.06f, 0.07f, 0.95f), LLColor4(0.06f, 0.06f, 0.07f, 0.35f), rnd(0.05f, 0.08f), 1.f, 7.f, 0.f, 0.05f);
    }
};
} // namespace

GMDXTPFX::Effect& tpfxNinjaSmoke()
{
    static NinjaSmoke e;
    return e;
}
