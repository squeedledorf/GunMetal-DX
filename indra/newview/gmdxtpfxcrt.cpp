/**
 * @file gmdxtpfxcrt.cpp
 * @brief GunMetal-DX teleport effect: CRT power-off (the old picture tube switching off and on).
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

// <GMDX> whole file: see gmdxtpfxeffect.h. Departure: the frozen frame squashes vertically into a
// bright line (brightening as it goes), the line shrinks to a dot, the dot fades: black. Hold: black.
// Arrival: TV-on in reverse, a dot, a line, the picture opening out of it (the live new scene)
// through a white wash, with scanlines and a little flicker. Quick (same region): the same, fast.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfxeffect.h"

#include "llrender.h"
#include "llrender2dutils.h"

using namespace GMDXTPFX;

namespace
{
// ---- tunables (depart progress; the arrival runs them backwards) ----
constexpr F32 SQUASHED = 0.45f;   // the picture is a line
constexpr F32 DOTTED = 0.75f;     // the line is a dot
constexpr F32 LINE_PX = 3.f;      // the line's thickness
constexpr S32 SCANLINE_PX = 3;    // scanline spacing
const LLColor4 TUBE(0.012f, 0.014f, 0.016f, 1.f);   // the switched-off screen
const LLColor4 PHOSPHOR(0.88f, 0.94f, 1.f, 1.f);    // the line and the dot

void scanlines(const Frame& f, F32 y0, F32 y1, F32 a)
{
    if (a <= 0.004f) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4f(0.f, 0.f, 0.f, a);
    gGL.begin(LLRender::LINES);
    for (F32 y = y0; y < y1; y += SCANLINE_PX)
    {
        gGL.vertex2f(0.f, y);
        gGL.vertex2f(f.w, y);
    }
    gGL.end();
}

// The bright horizontal line (width 0..1 of the screen) and its glow, at the centre.
void line(const Frame& f, F32 width, F32 a)
{
    const F32 cx = f.w * 0.5f, cy = f.h * 0.5f, hw = llmax(2.f, 0.5f * f.w * width);
    LLColor4 c = PHOSPHOR;
    c.mV[VALPHA] = a;
    gl_rect_2d((S32)(cx - hw), (S32)(cy + LINE_PX * 0.5f), (S32)(cx + hw), (S32)(cy - LINE_PX * 0.5f), c, true);
    gGL.pushUIMatrix();
    gGL.translateUI(cx, cy, 0.f);
    LLColor4 glow = PHOSPHOR;
    glow.mV[VALPHA] = 0.35f * a;
    LLColor4 none = glow;
    none.mV[VALPHA] = 0.f;
    gl_washer_2d(llmax(8.f, hw * 0.35f), 0.f, 32, glow, none);
    gGL.popUIMatrix();
}

class CRT : public Effect
{
public:
    const char* name() const override { return "crt"; }
    bool quick() const override { return true; }
    F32 departSecs() const override { return 0.7f; }
    const char* departSound() const override { return "tpfx_crt_off.wav"; }
    const char* arriveSound() const override { return "tpfx_crt_on.wav"; }

    void depart(const Frame& f) override
    {
        LLColor4 tube = TUBE;
        tube.mV[VALPHA] = f.alpha;
        fill(f, tube);
        if (f.u < SQUASHED)
        {
            // the picture squashes toward the centre line, brightening
            const F32 k = smooth(0.f, SQUASHED, f.u);
            const F32 half = llmax(LINE_PX * 0.5f, 0.5f * f.h * (1.f - k));
            const F32 cy = f.h * 0.5f;
            drawFrozen(f, LLColor4(1.f, 1.f, 1.f, f.alpha), 0.f, cy - half, f.w, cy + half);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            LLColor4 wash = PHOSPHOR;
            wash.mV[VALPHA] = f.alpha * 0.9f * smooth(0.1f, 1.f, k);
            gl_rect_2d(0, (S32)(cy + half), (S32)f.w + 1, (S32)(cy - half), wash, true);
            scanlines(f, cy - half, cy + half, 0.18f * f.alpha * (1.f - k));
        }
        else if (f.u < DOTTED)
            line(f, 1.f - smooth(SQUASHED, DOTTED, f.u), f.alpha);   // the line shrinks to a dot
        else
            line(f, 0.f, f.alpha * (1.f - smooth(DOTTED, 1.f, f.u)));   // the dot fades
    }

    void hold(const Frame& f) override
    {
        LLColor4 tube = TUBE;
        tube.mV[VALPHA] = f.alpha;
        fill(f, tube);
    }

    void arrive(const Frame& f) override
    {
        // TV on: a dot widens to a line (to 0.3), the picture opens out of it (to 0.75), settles
        const F32 cy = f.h * 0.5f;
        LLColor4 tube = TUBE;
        tube.mV[VALPHA] = f.alpha;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        if (f.a < 0.3f)
        {
            fill(f, tube);
            line(f, smooth(0.05f, 0.3f, f.a), f.alpha * smooth(0.f, 0.05f, f.a));
            return;
        }
        const F32 k = smooth(0.3f, 0.75f, f.a);
        const F32 half = llmax(LINE_PX * 0.5f, 0.5f * f.h * k);
        // the tube outside the opening picture
        gl_rect_2d(0, (S32)f.h + 1, (S32)f.w + 1, (S32)(cy + half), tube, true);
        gl_rect_2d(0, (S32)(cy - half), (S32)f.w + 1, 0, tube, true);
        // the picture: a white wash fading, scanlines, a little flicker
        LLColor4 wash = PHOSPHOR;
        wash.mV[VALPHA] = f.alpha * 0.85f * (1.f - smooth(0.3f, 0.9f, f.a));
        gl_rect_2d(0, (S32)(cy + half), (S32)f.w + 1, (S32)(cy - half), wash, true);
        scanlines(f, cy - half, cy + half, 0.22f * f.alpha * (1.f - smooth(0.6f, 1.f, f.a)));
        if (sinf(f.t * 47.f) > 0.55f) fill(f, LLColor4(0.f, 0.f, 0.f, 0.12f * f.alpha * (1.f - f.a)));
    }
};
} // namespace

GMDXTPFX::Effect& tpfxCRT()
{
    static CRT e;
    return e;
}
