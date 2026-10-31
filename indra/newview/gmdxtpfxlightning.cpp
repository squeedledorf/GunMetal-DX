/**
 * @file gmdxtpfxlightning.cpp
 * @brief GunMetal-DX teleport effect: Lightning strike (a bolt in the world onto the avatar, a flash, sparks).
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

// <GMDX> whole file: see gmdxtpfxeffect.h and gmdxtpfxworld.h. In the world: a jagged, forked bolt
// from high above comes down onto the avatar's feet and flickers (three strokes down the same
// channel in 0.3 s, each dimmer); a light flashes over the scene with the first stroke (the later
// ones barely), a gentle screen flash, a glare at the impact, sparks bouncing off the ground and a
// little scorch smoke. He is gone in the first stroke. Cross-region: the strike plays in the old
// world, the screen sinks into the dark hold, and the arrival is a second strike onto the new
// spot, the hold lifting under its flash with him standing there. Same region: a strike where he
// was, one where he lands.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"
#include "gmdxtpfxworld.h"

#include "llagent.h"

using namespace GMDXTPFX;
using namespace GMDXTPFX::World;

namespace
{
// ---- tunables (s, m) ----
constexpr S32 STROKES = 3;
constexpr F32 STROKE_AT[STROKES] = { 0.f, 0.17f, 0.38f };   // the return strokes (no faster than a flicker)
constexpr F32 STROKE_K[STROKES] = { 1.f, 0.75f, 0.55f };
constexpr F32 STROKE_LIFE = 0.16f;
constexpr F32 LIGHT_K[STROKES] = { 1.f, 0.35f, 0.2f };     // the scene light: one real flash, then barely
constexpr S32 LEVELS = 6, FORK_LEVELS = 4, FORKS = 3;      // midpoint displacement: 2^6 segments, forks 2^4
constexpr F32 ROUGH = 0.2f, FORK_ROUGH = 0.28f;            // sideways jag, of a segment's length
constexpr F32 TOP_MIN = 32.f, TOP_MAX = 42.f;              // the top of the bolt above his feet
constexpr F32 LEAN_MIN = 3.f, LEAN_MAX = 7.f;              // ... and off to the side
constexpr F32 CORE_W = 0.2f, HALO_W = 1.2f;              // the strips' widths
constexpr F32 HIDE_AT = 0.03f;                             // after the first stroke: he is gone
constexpr F32 SHOW_AT = 0.03f;                             // into the arrival: he is there in its first stroke
constexpr F32 FLASH = 0.16f;                               // the screen flash's peak opacity (gentle)
constexpr F32 VEIL_FROM = 0.5f, VEIL_TO = 1.f;             // departure progress: the screen sinks into the hold
constexpr F32 UNVEIL_FROM = 0.05f, UNVEIL_TO = 0.2f;       // arrival progress: and comes out of it under the flash
const LLColor3 SKY_LIGHT(2.4f, 2.6f, 3.2f);                // the flash's light (linear, intensity folded in)

typedef std::vector<LLVector3d> Path;

// A jagged path from a to b: midpoint displacement, each midpoint pushed sideways (any direction
// across the segment) by up to `rough` of its segment's length.
Path jag(const LLVector3d& a, const LLVector3d& b, S32 levels, F32 rough)
{
    Path p{ a, b };
    for (S32 k = 0; k < levels; ++k, rough *= 0.55f)
    {
        Path n;
        n.reserve(p.size() * 2);
        for (size_t i = 0; i + 1 < p.size(); ++i)
        {
            LLVector3 d(p[i + 1] - p[i]);
            const F32 len = d.normVec();
            LLVector3 side = randomDir();
            side -= d * (side * d);
            side.normVec();
            n.push_back(p[i]);
            n.push_back((p[i] + p[i + 1]) * 0.5 + LLVector3d(side * (rnd(-rough, rough) * len)));
        }
        n.push_back(p.back());
        p.swap(n);
    }
    return p;
}

// A strike from the sky onto the feet of the avatar standing at `at`.
void strike(const LLVector3d& at, const Frame& f, bool arrival)
{
    const Body b = body(at);
    const F32 lit = llclamp(f.intensity, 0.f, 2.f);
    const LLVector3d foot(b.centre.mdV[VX], b.centre.mdV[VY], b.foot + 0.03);
    const F32 ang = rnd(0.f, F_TWO_PI), lean = rnd(LEAN_MIN, LEAN_MAX);
    const LLVector3d top = foot + LLVector3d(cosf(ang) * lean, sinf(ang) * lean, rnd(TOP_MIN, TOP_MAX));
    std::vector<Path> paths{ jag(top, foot, LEVELS, ROUGH) };
    for (S32 i = 0; i < FORKS; ++i)
    {
        const Path& m = paths.front();
        const LLVector3d from = m[(size_t)(m.size() * rnd(0.2f, 0.7f))];
        LLVector3 d(rnd(-1.f, 1.f), rnd(-1.f, 1.f), -rnd(1.2f, 2.f));
        d.normVec();
        paths.push_back(jag(from, from + LLVector3d(d * rnd(5.f, 11.f)), FORK_LEVELS, FORK_ROUGH));
    }

    // the bolt: every segment a hot core in a wide blue halo, struck again (the same emitter rows)
    // by each stroke; the forks only flicker in the first two
    std::vector<U32> ids;
    for (S32 k = 0; k < STROKES; ++k)
    {
        size_t id = 0;
        for (size_t pi = 0; pi < paths.size(); ++pi)
        {
            const Path& p = paths[pi];
            const bool fork = pi > 0;
            const F32 w = fork ? 0.6f : 1.f, a = STROKE_K[k] * (fork ? 0.7f : 1.f);
            for (size_t j = 0; j + 1 < p.size(); ++j)
                for (S32 layer = 0; layer < 2; ++layer, ++id)
                {
                    if (id >= ids.size()) ids.push_back(newId());
                    if (fork && k == STROKES - 1) continue;
                    Part s;
                    s.kind = Part::STREAK;
                    s.tex = Tex::STREAK;
                    s.additive = s.emissive = true;
                    s.at = p[j];
                    s.to = p[j + 1];
                    s.life = STROKE_LIFE;
                    s.delay = STROKE_AT[k];
                    s.id = ids[id];
                    if (layer == 0)
                    {
                        s.size0 = CORE_W * w;
                        s.c0 = LLColor4(1.f, 1.f, 1.f, a);
                        s.c1 = LLColor4(0.85f, 0.9f, 1.f, 0.35f * a);
                        s.glow = 0.8f;
                    }
                    else
                    {
                        s.size0 = HALO_W * w;
                        s.c0 = LLColor4(0.55f, 0.65f, 1.f, 0.32f * a);
                        s.c1 = LLColor4(0.5f, 0.6f, 1.f, 0.08f * a);
                        s.glow = 0.35f;
                    }
                    s.size1 = s.size0;
                    add(s);
                }
        }
        if (lit > 0.f)
            light(foot + LLVector3d(0.0, 0.0, 5.0), SKY_LIGHT * (lit * LIGHT_K[k] * (arrival ? 0.8f : 1.f)), 30.f, STROKE_AT[k], 0.012f, 0.09f);
    }

    // the glare at the impact (again, smaller, with the second stroke)
    for (S32 k = 0; k < 2; ++k)
    {
        Part g;
        g.tex = Tex::GLOW;
        g.additive = g.emissive = true;
        g.at = foot + LLVector3d(0.0, 0.0, 0.3);
        g.delay = STROKE_AT[k];
        g.life = k ? 0.18f : 0.3f;
        g.size0 = k ? 0.8f : 1.f;
        g.size1 = k ? 2.5f : 4.5f;
        g.c0 = LLColor4(0.8f, 0.86f, 1.f, k ? 0.5f : 0.9f);
        g.c1 = LLColor4(0.6f, 0.7f, 1.f, 0.f);
        g.glow = 0.5f;
        add(g);
    }
    // sparks, flung off the ground and bouncing on it
    static const F32 spark[3][4] = { { 14.f, 2.f, 5.f, 0.45f }, { 14.f, 3.f, 7.f, 0.65f }, { 12.f, 4.f, 9.f, 0.85f } };   // count, speeds, life
    const F32 many = llclamp(f.intensity, 0.3f, 2.f) * (f.quick ? 0.7f : 1.f);
    for (const auto& sp : spark)
    {
        Part p;
        p.kind = Part::BURST;
        p.tex = Tex::STREAK;
        p.additive = p.emissive = true;
        p.at = foot + LLVector3d(0.0, 0.0, 0.02);
        p.count = (U32)llmax(1.f, sp[0] * many + 0.5f);
        p.speed0 = sp[1];
        p.speed1 = sp[2];
        p.life = sp[3];
        p.accel.set(0.f, 0.f, -9.8f);
        p.size0 = 0.22f;   // length
        p.size1 = 0.08f;
        p.aspect = 0.15f;  // width
        p.c0 = LLColor4(1.f, 0.95f, 0.75f, 1.f);
        p.c1 = LLColor4(1.f, 0.45f, 0.1f, 0.f);
        p.glow = 0.6f;
        p.delay = 0.01f;
        add(p);
    }
    // a little scorch smoke curling up off the spot
    const F32 s = b.scale();
    for (S32 i = 0; i < 9; ++i)
    {
        const F32 a = rnd(0.f, F_TWO_PI), r = rnd(0.2f, 0.5f) * s;
        const LLVector3 d(cosf(a), sinf(a), 0.f);
        Part p;
        p.tex = (Tex)((U8)Tex::SMOKE0 + (i % 3));
        p.at = LLVector3d(foot.mdV[VX] + d.mV[VX] * r, foot.mdV[VY] + d.mV[VY] * r, b.foot + 0.25 * s);
        p.vel = d * rnd(0.6f, 1.4f) + LLVector3(0.f, 0.f, rnd(0.6f, 1.2f));
        p.drag = 1.5f;
        p.accel.set(0.f, 0.f, 0.5f);
        p.wobble = 0.15f * s;
        p.wobbleHz = 0.4f;
        p.ground = b.groundAt(p.at);
        p.life = rnd(1.8f, 2.6f);
        p.size0 = 0.4f * s;
        p.size1 = 1.8f * s;
        p.c0 = LLColor4(0.24f, 0.23f, 0.23f, 0.55f);
        p.c1 = LLColor4(0.3f, 0.3f, 0.31f, 0.f);
        p.delay = rnd(0.04f, 0.2f);
        add(p);
    }
}

class Lightning : public Effect
{
public:
    const char* name() const override { return "lightning"; }
    bool quick() const override { return true; }
    bool crossRegion() const override { return false; }   // same-region only: they read best in the live world around a short jump
    bool inWorld() const override { return true; }
    F32 departSecs() const override { return 0.9f; }
    const char* departSound() const override { return "tpfx_thunder.wav"; }
    const char* arriveSound() const override { return "tpfx_thunder.wav"; }

    void begin(const Frame&) override
    {
        mStrikeT = mArriveT = -1.f;
    }

    void tick(const Frame& f) override
    {
        if (mStrikeT < 0.f)
        {
            mStrikeT = f.t;
            strike(f.quick ? f.from : gAgent.getPositionGlobal(), f, false);   // (same region: where he was)
        }
        if (f.arriving && mArriveT < 0.f)
        {
            mArriveT = f.t;
            strike(gAgent.getPositionGlobal(), f, true);
        }
    }

    bool hidesAvatar(const Frame& f) const override
    {
        if (f.alpha < 1.f || mStrikeT < 0.f) return false;   // a failed / cancelled teleport fading out: he is back
        if (!f.arriving) return f.t - mStrikeT >= HIDE_AT;
        return mArriveT < 0.f || f.t - mArriveT < SHOW_AT;
    }

    void depart(const Frame& f) override
    {
        if (!f.quick) veil(f, f.left ? 1.f : smooth(VEIL_FROM, VEIL_TO, f.u));
        flash(f, mStrikeT);
    }

    void hold(const Frame& f) override
    {
        if (!f.quick) Effect::hold(f);   // (same region: waiting for the jump, the world stays in view)
    }

    void arrive(const Frame& f) override
    {
        if (!f.quick) veil(f, 1.f - smooth(UNVEIL_FROM, UNVEIL_TO, f.a));
        flash(f, mArriveT);
    }

private:
    F32 mStrikeT = -1.f, mArriveT = -1.f;   // f.t of the departure strike / the arrival's

    // one soft screen flash with a strike's first stroke (at f.t == t0)
    static void flash(const Frame& f, F32 t0)
    {
        if (t0 < 0.f) return;
        const F32 dt = f.t - t0;
        const F32 k = FLASH * llclamp(f.intensity, 0.f, 1.5f) * smooth(0.f, 0.02f, dt) * (1.f - smooth(0.03f, 0.14f, dt));
        if (k > 0.004f) fill(f, LLColor4(0.85f, 0.9f, 1.f, k * f.alpha));
    }
};
} // namespace

GMDXTPFX::Effect& tpfxLightning()
{
    static Lightning e;
    return e;
}
