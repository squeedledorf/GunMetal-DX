/**
 * @file gmdxtpfxfold.cpp
 * @brief GunMetal-DX teleport effect: Fold-away (the frame flips away like a card).
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

// <GMDX> whole file: see gmdxtpfxeffect.h. Departure: the frozen frame flips away like a card about
// the screen's vertical centre line, in perspective, falling back and darkening as it turns, until
// it is edge-on and gone. Hold: the stock dark hold. Arrival: the card turns on in from edge-on,
// the same way round, with the live new scene in it, coming forward to fill the screen. Quick (same
// region): the same flip, fast.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llviewertexture.h"

using namespace GMDXTPFX;

namespace
{
// ---- tunables ----
constexpr F32 PERSP = 1.4f;     // the eye's distance from the screen, screen widths (smaller: more perspective)
constexpr F32 RECEDE = 0.8f;    // how far back the card falls when edge-on, screen widths (>= pi/4 keeps it inside the screen)
constexpr F32 SHADE = 0.55f;    // how dark it gets as it turns
constexpr S32 STRIPS = 16;      // vertical strips (keeps the picture from bending along the diagonals)
const LLColor4 BG(0.035f, 0.04f, 0.05f, 1.f);   // the stock hold's colour (drawCover)

class Fold : public Effect
{
public:
    const char* name() const override { return "fold"; }
    bool quick() const override { return true; }
    F32 departSecs() const override { return 0.7f; }
    const char* departSound() const override { return "tpfx_fold.wav"; }
    const char* arriveSound() const override { return "tpfx_fold.wav"; }

    void depart(const Frame& f) override
    {
        LLColor4 bg = BG;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        const F32 e = smooth(0.f, 1.f, f.u);
        const F32 th = F_PI_BY_TWO * e, z0 = RECEDE * f.w * e;
        if (!f.frozen) return;
        gGL.getTexUnit(0)->bind(f.frozen);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < STRIPS; ++i)
        {
            const F32 s0 = (F32)i / STRIPS, s1 = (F32)(i + 1) / STRIPS;
            const LLVector2 q[4] = { at(f, s0, 0.f, th, z0), at(f, s1, 0.f, th, z0), at(f, s1, 1.f, th, z0), at(f, s0, 1.f, th, z0) };
            const F32 uv[4][2] = { { s0, 0.f }, { s1, 0.f }, { s1, 1.f }, { s0, 1.f } };
            for (S32 k : { 0, 1, 2, 0, 2, 3 })
            {
                const F32 sh = shade(uv[k][0], th);   // darker as it turns, the far side most
                gGL.color4f(sh, sh, sh, f.alpha);
                gGL.texCoord2f(uv[k][0], uv[k][1]);
                gGL.vertex2f(q[k].mV[VX], q[k].mV[VY]);
            }
        }
        gGL.end();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    void arrive(const Frame& f) override
    {
        // the card's corners (BL, BR, TR, TL), turning in from edge-on the same way round
        const F32 e = smooth(0.f, 1.f, f.a);
        const F32 th = -F_PI_BY_TWO * (1.f - e), z0 = RECEDE * f.w * (1.f - e);
        const LLVector2 q[4] = { at(f, 0.f, 0.f, th, z0), at(f, 1.f, 0.f, th, z0), at(f, 1.f, 1.f, th, z0), at(f, 0.f, 1.f, th, z0) };
        const LLVector2 s[4] = { LLVector2(-1.f, -1.f), LLVector2(f.w + 1.f, -1.f), LLVector2(f.w + 1.f, f.h + 1.f), LLVector2(-1.f, f.h + 1.f) };
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        // the dark around the card: four pieces between the screen's edges and the card's
        LLColor4 bg = BG;
        bg.mV[VALPHA] = f.alpha;
        gGL.color4fv(bg.mV);
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 j = (i + 1) % 4;
            for (const LLVector2* p : { &s[i], &s[j], &q[j], &s[i], &q[j], &q[i] }) gGL.vertex2f(p->mV[VX], p->mV[VY]);
        }
        // the card's shading over the live scene (left / right edges)
        const F32 dl = (1.f - shade(0.f, th)) * f.alpha, dr = (1.f - shade(1.f, th)) * f.alpha;
        const F32 da[4] = { dl, dr, dr, dl };
        for (S32 k : { 0, 1, 2, 0, 2, 3 })
        {
            gGL.color4f(0.f, 0.f, 0.f, da[k]);
            gGL.vertex2f(q[k].mV[VX], q[k].mV[VY]);
        }
        gGL.end();
        // a faint edge while it turns
        gGL.color4f(0.8f, 0.85f, 0.9f, 0.3f * f.alpha * (1.f - e));
        gGL.begin(LLRender::LINES);
        for (S32 i = 0; i < 4; ++i)
        {
            gGL.vertex2f(q[i].mV[VX], q[i].mV[VY]);
            gGL.vertex2f(q[(i + 1) % 4].mV[VX], q[(i + 1) % 4].mV[VY]);
        }
        gGL.end();
    }

private:
    // The card's point (sx, sy) (0..1 across it) turned th about the vertical centre line (the right
    // side going away for th > 0), pushed back z0 pixels, in perspective.
    static LLVector2 at(const Frame& f, F32 sx, F32 sy, F32 th, F32 z0)
    {
        const F32 x = (sx - 0.5f) * f.w, y = (sy - 0.5f) * f.h, d = PERSP * f.w;
        const F32 k = d / llmax(0.1f * d, d + z0 + x * sinf(th));
        return LLVector2(0.5f * f.w + x * cosf(th) * k, 0.5f * f.h + y * k);
    }
    static F32 shade(F32 sx, F32 th)
    {
        const F32 far_side = th >= 0.f ? sx : 1.f - sx;
        return 1.f - SHADE * fabsf(sinf(th)) * (0.6f + 0.4f * far_side);
    }
};
} // namespace

GMDXTPFX::Effect& tpfxFold()
{
    static Fold e;
    return e;
}
