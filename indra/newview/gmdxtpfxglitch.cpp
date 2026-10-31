/**
 * @file gmdxtpfxglitch.cpp
 * @brief GunMetal-DX teleport effect: Glitch (the frame corrupts like bad video data).
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

// <GMDX> whole file: see gmdxtpfxeffect.h. Departure: the frozen frame corrupts like bad video data:
// horizontal slices jump sideways, the red and blue channels split off, 16-pixel blocks drop out or
// show the wrong part of the picture, worse and worse, then a cut to black. Hold: black, a faint
// rolling bar. Arrival: the new scene tears in band by band through leftover slices of the old one
// and a few dead blocks, then settles. Quick (same region): the same, a short snap.
//
// Easy on the eyes: the corruption changes in steps (STEP_HZ), the picture only ever darkens on the
// way out and only ever clears on the way in (no strobing), and the colours are muted.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llrand.h"

using namespace GMDXTPFX;

namespace
{
// ---- tunables ----
constexpr F32 STEP_HZ = 12.f, QUICK_STEP_HZ = 24.f;   // the corruption changes this often
constexpr F32 CUT = 0.85f;          // depart progress of the cut to black
constexpr F32 SETTLE = 0.7f;        // arrive progress by which the new scene is clean
constexpr F32 MAX_SHIFT = 0.12f;    // a slice's sideways jump, screen widths (times GMDXTPFXIntensity)
constexpr F32 SPLIT = 0.006f;       // the colour channels' split, screen widths (grows to 1.2x)
constexpr S32 BLOCKS = 40;
constexpr F32 BLOCK_PX = 16.f;      // blocks snap to this grid (macroblocks)
const LLColor4 BLACK(0.f, 0.f, 0.f, 1.f);
const LLColor4 HOLD_BG(0.02f, 0.02f, 0.025f, 1.f);
const LLColor4 TINTS[3] = { LLColor4(0.45f, 0.15f, 0.42f, 1.f), LLColor4(0.12f, 0.4f, 0.42f, 1.f), LLColor4(0.2f, 0.38f, 0.16f, 1.f) };

struct Band { F32 y0, y1, gone; bool ghost; };   // gone: when it corrupts (0..1); ghost: shows the old frame on the way in
struct Block { F32 x, y, w, h, gone; S32 kind; };   // kind 0 black, 1 the wrong part of the picture, 2 a muted colour

// A steady random number in 0..1 for (run, step, item).
F32 hash01(U32 a, U32 b, U32 c)
{
    U32 h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (F32)(h & 0xffffff) / 16777215.f;
}

void rect(F32 x0, F32 y0, F32 x1, F32 y1, const LLColor4& c)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(c.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex2f(x0, y0); gGL.vertex2f(x1, y0); gGL.vertex2f(x1, y1);
    gGL.vertex2f(x0, y0); gGL.vertex2f(x1, y1); gGL.vertex2f(x0, y1);
    gGL.end();
}

class Glitch : public Effect
{
public:
    const char* name() const override { return "glitch"; }
    bool quick() const override { return true; }
    F32 departSecs() const override { return 0.8f; }
    const char* departSound() const override { return "tpfx_glitch.wav"; }
    const char* arriveSound() const override { return "tpfx_glitch.wav"; }

    void begin(const Frame& f) override
    {
        mSeed = (U32)rnd(1.f, 1.6e7f);
        mBands.clear();
        for (F32 y = 0.f; y < f.h;)
        {
            const F32 y1 = llmin(f.h + 1.f, y + f.h * rnd(0.01f, 0.08f));
            mBands.push_back({ y, y1, rnd(0.f, 1.f), ll_frand() < 0.3f });
            y = y1;
        }
        mBlocks.clear();
        for (S32 i = 0; i < BLOCKS; ++i)
        {
            const F32 bw = BLOCK_PX * (F32)(1 + (S32)rnd(0.f, 8.f)), bh = BLOCK_PX * (F32)(1 + (S32)rnd(0.f, 4.f));
            const F32 x = BLOCK_PX * floorf(rnd(0.f, f.w) / BLOCK_PX), y = BLOCK_PX * floorf(rnd(0.f, f.h) / BLOCK_PX);
            mBlocks.push_back({ x, y, bw, bh, rnd(0.15f, 1.f), i % 3 });
        }
    }

    void depart(const Frame& f) override
    {
        LLColor4 black = BLACK;
        black.mV[VALPHA] = f.alpha;
        fill(f, black);
        if (f.u >= CUT) return;   // the cut
        const F32 g = smooth(0.f, CUT, f.u);   // how corrupt it is
        const U32 step = this->step(f);
        // the slices, each colour channel on its own, the red and blue ones split off sideways
        F32 split = SPLIT * f.w * (0.2f + g) * f.intensity;
        if (hash01(mSeed, step, 7777) < 0.4f) split *= -0.6f;
        const F32 off[3] = { split, 0.f, -split };
        for (S32 ch = 0; ch < 3; ++ch)
        {
            gGL.setColorMask(ch == 0, ch == 1, ch == 2, false);
            for (size_t b = 0; b < mBands.size(); ++b)
            {
                const Band& s = mBands[b];
                const F32 dx = off[ch] + shift(f, step, (U32)b, g);
                drawFrozen(f, LLColor4(1.f, 1.f, 1.f, f.alpha), dx, s.y0, f.w + dx, s.y1, 0.f, s.y0 / f.h, 1.f, s.y1 / f.h);
            }
        }
        gGL.setColorMask(true, true);
        drawBlocks(f, step, [g](const Block& k) { return g > k.gone; });
    }

    void hold(const Frame& f) override
    {
        LLColor4 bg = HOLD_BG;
        bg.mV[VALPHA] = f.alpha;
        fill(f, bg);
        // a faint bar rolling up the dead screen
        const F32 y = f.h * fmodf(f.t * 0.15f, 1.2f) - 0.1f * f.h;
        rect(0.f, y, f.w + 1.f, y + 0.06f * f.h, LLColor4(0.12f, 0.13f, 0.15f, 0.08f * f.alpha));
    }

    void arrive(const Frame& f) override
    {
        const F32 k = smooth(0.f, SETTLE, f.a);   // how much of the new scene is in
        const U32 step = this->step(f);
        for (size_t b = 0; b < mBands.size(); ++b)
        {
            const Band& s = mBands[b];
            if (s.gone > k)   // still dead: black, or a slice of the old frame, shifted
            {
                rect(0.f, s.y0, f.w + 1.f, s.y1, LLColor4(0.f, 0.f, 0.f, f.alpha));
                if (s.ghost)
                {
                    const F32 dx = shift(f, step, (U32)b, 1.f - k);
                    drawFrozen(f, LLColor4(1.f, 1.f, 1.f, 0.6f * f.alpha), dx, s.y0, f.w + dx, s.y1, 0.f, s.y0 / f.h, 1.f, s.y1 / f.h);
                }
            }
            else if (s.gone > k - 0.15f)   // just in: a fading colour cast
            {
                LLColor4 c = TINTS[b % 3];
                c.mV[VALPHA] = 0.25f * f.alpha * (1.f - (k - s.gone) / 0.15f);
                rect(0.f, s.y0, f.w + 1.f, s.y1, c);
            }
        }
        drawBlocks(f, step, [k](const Block& b) { return b.gone > 1.2f * k; });
    }

    void end() override
    {
        mBands.clear();
        mBlocks.clear();
    }

private:
    U32 mSeed = 1;
    std::vector<Band> mBands;
    std::vector<Block> mBlocks;

    U32 step(const Frame& f) const { return (U32)(f.t * (f.quick ? QUICK_STEP_HZ : STEP_HZ)); }

    // A slice's sideways jump this step (more of them, and further, as g grows).
    F32 shift(const Frame& f, U32 step, U32 band, F32 g) const
    {
        if (hash01(mSeed, step, band) > 0.05f + 0.55f * g) return 0.f;
        return (hash01(mSeed, step, band + 1000) - 0.5f) * 2.f * MAX_SHIFT * f.w * g * f.intensity;
    }

    template <typename Shown> void drawBlocks(const Frame& f, U32 step, Shown shown) const
    {
        for (size_t i = 0; i < mBlocks.size(); ++i)
        {
            const Block& b = mBlocks[i];
            if (!shown(b)) continue;
            if (b.kind == 1)   // the wrong part of the picture, a new one each step
            {
                const F32 du = 0.3f * (hash01(mSeed, step, 5000 + (U32)i) - 0.5f), dv = 0.2f * (hash01(mSeed, step, 6000 + (U32)i) - 0.5f);
                drawFrozen(f, LLColor4(1.f, 1.f, 1.f, f.alpha), b.x, b.y, b.x + b.w, b.y + b.h,
                           b.x / f.w + du, b.y / f.h + dv, (b.x + b.w) / f.w + du, (b.y + b.h) / f.h + dv);
            }
            else
            {
                LLColor4 c = b.kind == 0 ? BLACK : TINTS[i % 3];
                c.mV[VALPHA] = f.alpha * (b.kind == 0 ? 1.f : 0.7f);
                rect(b.x, b.y, b.x + b.w, b.y + b.h, c);
            }
        }
    }
};
} // namespace

GMDXTPFX::Effect& tpfxGlitch()
{
    static Glitch e;
    return e;
}
