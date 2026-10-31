/**
 * @file gmdxtpfxshatter.cpp
 * @brief GunMetal-DX teleport effect: Shatter (the last frame cracks and falls apart as glass).
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

// <GMDX> whole file: see gmdxtpfxeffect.h. The frozen frame cracks from the centre of the screen
// (three patterns, GMDXTPFXGlassPattern) and falls apart as textured glass shards with glinting
// edges over the dark hold; the arrival is the iris (drawCover).

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llrand.h"

using namespace GMDXTPFX;

namespace
{
constexpr F32 CRACK_END = 0.12f;   // depart progress where the cracks have run out
constexpr F32 FALL_FADE = 0.72f;   // depart progress where the shards start to fade
constexpr F32 GRAVITY = 2.4f;      // shard fall, in screen heights per depart^2

struct Shard
{
    std::vector<LLVector2> p;   // at rest, root-view pixels (convex)
    LLVector2 c;                // centroid
    LLVector2 v0;               // push (pixels per depart)
    F32 rel = 0.f;              // depart progress where it lets go
    F32 spin = 0.f, tilt = 0.f, axis = 0.f, seed = 0.f;
    F32 dist = 0.f;             // centroid to the impact (crack spread)
};

class Shatter : public Effect
{
public:
    const char* name() const override { return "shatter"; }
    F32 arriveFrom() const override { return 0.5f; }   // not before the crack has been seen
    const char* departSound() const override { return "tpfx_shatter.wav"; }

    void begin(const Frame& f) override
    {
        mW = f.w;
        mH = f.h;
        mIntensity = f.intensity;
        buildShards();
    }
    void depart(const Frame& f) override
    {
        drawCover(f, 0.f);
        drawShards(f);
    }
    void arrive(const Frame& f) override
    {
        drawCover(f, smooth(0.f, 1.f, f.a));
        if (f.u < 1.f) drawShards(f);
    }
    void end() override { std::vector<Shard>().swap(mShards); }

private:
    F32 mW = 1.f, mH = 1.f, mIntensity = 1.f, mMaxDist = 1.f;
    LLVector2 mImpact;
    std::vector<Shard> mShards;

    // Keeps the part of the convex polygon with n.p <= d (Sutherland-Hodgman, one plane).
    static void clip(std::vector<LLVector2>& poly, const LLVector2& n, F32 d)
    {
        std::vector<LLVector2> out;
        const size_t k = poly.size();
        for (size_t i = 0; i < k; ++i)
        {
            const LLVector2& a = poly[i];
            const LLVector2& b = poly[(i + 1) % k];
            const F32 da = n * a - d, db = n * b - d;
            if (da <= 0.f) out.push_back(a);
            if ((da < 0.f) != (db < 0.f) && da != db) out.push_back(a + (b - a) * (da / (da - db)));
        }
        poly.swap(out);
    }

    void addShard(std::vector<LLVector2> poly)
    {
        clip(poly, LLVector2(-1.f, 0.f), 0.f);
        clip(poly, LLVector2(1.f, 0.f), mW);
        clip(poly, LLVector2(0.f, -1.f), 0.f);
        clip(poly, LLVector2(0.f, 1.f), mH);
        if (poly.size() < 3) return;
        F32 area = 0.f;
        LLVector2 c;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const LLVector2& a = poly[i];
            const LLVector2& b = poly[(i + 1) % poly.size()];
            area += a.mV[VX] * b.mV[VY] - b.mV[VX] * a.mV[VY];
            c += a;
        }
        if (fabsf(area) < 8.f) return;   // slivers
        Shard s;
        s.p.swap(poly);
        s.c = c / (F32)s.p.size();
        LLVector2 dir = s.c - mImpact;
        s.dist = dir.length();
        dir = s.dist > 1.f ? dir / s.dist : LLVector2(0.f, 1.f);
        s.v0 = dir * (rnd(0.06f, 0.18f) * mH * mIntensity) + LLVector2(rnd(-0.04f, 0.04f), rnd(0.02f, 0.10f)) * (mH * mIntensity);
        s.spin = rnd(1.f, 4.f) * (ll_frand() < 0.5f ? -1.f : 1.f) * mIntensity;
        s.tilt = rnd(1.5f, 5.f) * mIntensity;
        s.axis = rnd(0.f, F_PI);
        s.seed = rnd(0.f, F_TWO_PI);
        mShards.push_back(std::move(s));
    }

    // Rays from the impact cut by jittered rings (pattern 0 / 2); splinters halve each cell.
    void radial(S32 rays, S32 rings, bool splinters)
    {
        F32 far_r = 0.f;
        for (const LLVector2& k : { LLVector2(0.f, 0.f), LLVector2(mW, 0.f), LLVector2(0.f, mH), LLVector2(mW, mH) })
            far_r = llmax(far_r, (k - mImpact).length());
        far_r *= 1.05f;
        std::vector<F32> ang(rays);
        for (S32 k = 0; k < rays; ++k) ang[k] = F_TWO_PI * ((F32)k + rnd(-0.35f, 0.35f)) / (F32)rays;
        // v[k][j]: ray k, ring j (ring 0 is the impact itself)
        std::vector<std::vector<LLVector2>> v(rays, std::vector<LLVector2>(rings + 1, mImpact));
        for (S32 k = 0; k < rays; ++k)
            for (S32 j = 1; j <= rings; ++j)
            {
                F32 r = far_r * powf((F32)j / (F32)rings, 1.35f);
                if (j < rings) r *= rnd(0.88f, 1.12f);
                v[k][j] = mImpact + LLVector2(cosf(ang[k]), sinf(ang[k])) * r;
            }
        for (S32 k = 0; k < rays; ++k)
        {
            const S32 k2 = (k + 1) % rays;
            addShard({ mImpact, v[k][1], v[k2][1] });
            for (S32 j = 1; j < rings; ++j)
            {
                if (splinters && (k + j) % 2)
                {
                    addShard({ v[k][j], v[k2][j], v[k2][j + 1] });
                    addShard({ v[k][j], v[k2][j + 1], v[k][j + 1] });
                }
                else if (splinters)
                {
                    addShard({ v[k][j], v[k2][j], v[k][j + 1] });
                    addShard({ v[k2][j], v[k2][j + 1], v[k][j + 1] });
                }
                else addShard({ v[k][j], v[k2][j], v[k2][j + 1], v[k][j + 1] });
            }
        }
    }

    // Voronoi cells of sites crowded around the impact (pattern 1).
    void voronoi(S32 sites)
    {
        std::vector<LLVector2> s;
        for (S32 i = 0; i < sites; ++i)
        {
            if (i % 2)
                s.emplace_back(rnd(0.f, mW), rnd(0.f, mH));
            else
            {
                const F32 a = rnd(0.f, F_TWO_PI), r = mH * 0.3f * powf(ll_frand(), 1.5f);
                s.emplace_back(mImpact.mV[VX] + r * cosf(a), mImpact.mV[VY] + r * sinf(a));
            }
        }
        for (size_t i = 0; i < s.size(); ++i)
        {
            std::vector<LLVector2> poly{ LLVector2(0.f, 0.f), LLVector2(mW, 0.f), LLVector2(mW, mH), LLVector2(0.f, mH) };
            for (size_t j = 0; j < s.size() && poly.size() >= 3; ++j)
            {
                if (i == j) continue;
                const LLVector2 n = s[j] - s[i];
                if (n.lengthSquared() < 1e-4f) continue;
                clip(poly, n, n * ((s[i] + s[j]) * 0.5f));
            }
            addShard(poly);
        }
    }

    void buildShards()
    {
        static LLCachedControl<S32> pattern(gSavedSettings, "GMDXTPFXGlassPattern", 1);
        mShards.clear();
        mImpact = LLVector2(mW * 0.5f, mH * 0.5f);   // ponytail: screen centre; toward the destination is a later nicety
        switch ((S32)pattern)
        {
            case 1:  voronoi(44); break;
            case 2:  radial(18, 7, true); break;
            default: radial(11, 5, false); break;
        }
        mMaxDist = 1.f;
        for (const Shard& s : mShards) mMaxDist = llmax(mMaxDist, s.dist);
        for (Shard& s : mShards) s.rel = 0.10f + 0.32f * (s.dist / mMaxDist) + rnd(0.f, 0.06f);
    }

    void drawShards(const Frame& f)
    {
        static LLCachedControl<F32> intensity(gSavedSettings, "GMDXTPFXIntensity", 1.0f);
        const F32 u = f.u, alpha = f.alpha;
        const F32 fall = 1.f - smooth(FALL_FADE, 1.f, u);
        const F32 crack_r = mMaxDist * 1.05f * smooth(0.f, CRACK_END, u);
        const F32 glint = llclamp(0.6f + 0.4f * (F32)intensity, 0.f, 1.5f);
        std::vector<std::vector<LLVector2>> placed(mShards.size());   // where each shard is now
        std::vector<F32> edge_a(mShards.size(), 0.f);

        gGL.getTexUnit(0)->bind(f.frozen);
        gGL.begin(LLRender::TRIANGLES);
        for (size_t k = 0; k < mShards.size(); ++k)
        {
            const Shard& s = mShards[k];
            const F32 tn = llclamp((u - s.rel) / (1.f - s.rel), 0.f, 1.f);
            const F32 a = alpha * fall;
            // in-plane spin, a tilt about `axis` (a squash across it), a little closer, gravity
            const F32 th = s.spin * tn, be = s.tilt * tn;
            const F32 ct = cosf(th), st = sinf(th);
            const LLVector2 ax(cosf(s.axis), sinf(s.axis));
            const F32 squash = cosf(be) - 1.f, scale = 1.f + 0.12f * tn * mIntensity;
            const LLVector2 off = s.v0 * tn - LLVector2(0.f, 0.5f * GRAVITY * mH * tn * tn);
            const size_t n = s.p.size();
            std::vector<LLVector2>& pts = placed[k];
            pts.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                LLVector2 l = s.p[i] - s.c;
                l += ax * ((l * ax) * squash);
                pts[i] = s.c + off + LLVector2(l.mV[VX] * ct - l.mV[VY] * st, l.mV[VX] * st + l.mV[VY] * ct) * scale;
            }
            const LLVector2 c = s.c + off;
            const F32 shade = 0.62f + 0.38f * fabsf(cosf(be));   // darker edge-on
            gGL.color4f(shade, shade, shade, a);
            for (size_t i = 0; i < n; ++i)
            {
                const size_t j = (i + 1) % n;
                gGL.texCoord2f(s.c.mV[VX] / mW, s.c.mV[VY] / mH);
                gGL.vertex2f(c.mV[VX], c.mV[VY]);
                gGL.texCoord2f(s.p[i].mV[VX] / mW, s.p[i].mV[VY] / mH);
                gGL.vertex2f(pts[i].mV[VX], pts[i].mV[VY]);
                gGL.texCoord2f(s.p[j].mV[VX] / mW, s.p[j].mV[VY] / mH);
                gGL.vertex2f(pts[j].mV[VX], pts[j].mV[VY]);
            }
            // its edges: cracked lines while it holds, a glint as it turns once it falls
            F32 e = 0.f;
            if (tn > 0.f) e = glint * (0.25f + 0.75f * powf(fabsf(sinf(be + s.seed)), 6.f));
            else if (s.dist <= crack_r) e = 0.8f;
            edge_a[k] = e * a;
        }
        gGL.end();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::LINES);
        for (size_t k = 0; k < mShards.size(); ++k)
        {
            if (edge_a[k] <= 0.01f) continue;
            gGL.color4f(0.92f, 0.97f, 1.f, llmin(1.f, edge_a[k]));
            const std::vector<LLVector2>& pts = placed[k];
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const LLVector2& q = pts[(i + 1) % pts.size()];
                gGL.vertex2f(pts[i].mV[VX], pts[i].mV[VY]);
                gGL.vertex2f(q.mV[VX], q.mV[VY]);
            }
        }
        gGL.end();

        // the impact's flash
        if (u < 0.1f)
        {
            gGL.pushUIMatrix();
            gGL.translateUI(mImpact.mV[VX], mImpact.mV[VY], 0.f);
            const F32 fl = alpha * 0.55f * (1.f - u / 0.1f) * llmin(1.f, glint);
            gl_washer_2d(0.07f * mH, 0.f, 32, LLColor4(1.f, 1.f, 1.f, fl), LLColor4(0.85f, 0.92f, 1.f, 0.f));
            gGL.popUIMatrix();
        }
    }
};
} // namespace

GMDXTPFX::Effect& tpfxShatter()
{
    static Shatter e;
    return e;
}
