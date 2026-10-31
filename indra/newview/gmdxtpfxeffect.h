/**
 * @file gmdxtpfxeffect.h
 * @brief GunMetal-DX: the interface of one teleport effect, and the drawing helpers they share.
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

// <GMDX> whole file. Only the effect files (gmdxtpfx*.cpp) include this.
//
// An effect draws; gmdxtpfx.cpp does everything else (arming, the capture, timing, the teleport
// state, the hold's line and Cancel, sounds, every way an effect ends). Each frame it calls exactly
// one of depart() (u < 1), hold() (departed, waiting) or arrive() (uncovering the live new scene),
// inside the 2D overlay pass (LLHUDView::draw, gGL immediate mode, root-view pixels over the whole
// window, origin bottom-left, alpha blending, no texture bound).
//
// An effect in the world (inWorld(): gmdxtpfxworld.h) plays its departure in the live world while
// the server works (the frozen frame goes unused): tick() starts its smoke / bolt (every idle
// frame while it runs, before the draw), hidesAvatar() hides the own avatar in the main view, and
// the 2D calls only veil the screen into the hold and out of it. Its same-region variant waits
// for the real jump (up to QUICK_WORLD_BY) and leaves its smoke at both ends.
//
// Adding an effect: a gmdxtpfx<name>.cpp defining `GMDXTPFX::Effect& tpfx<Name>()` (a function-local
// static), one X(tpfx<Name>) line in GMDXTPFX_EFFECTS (gmdxtpfx.cpp), the file in CMakeLists.txt,
// and its name as a combo item in panel_preferences_tpfx.xml (the same-region combo too if quick()).

#pragma once

#include "v2math.h"
#include "v3dmath.h"
#include "v4color.h"

#include <functional>

class LLViewerTexture;

namespace GMDXTPFX
{
struct Frame
{
    F32 t = 0.f;            // seconds since the effect began
    F32 u = 0.f;            // departure progress, 0..1
    F32 a = 0.f;            // arrival progress, 0..1 (arrive() only; linear, ease it yourself)
    F32 alpha = 1.f;        // the whole effect's opacity (< 1 while a failed or cancelled teleport fades it out)
    F32 w = 0.f, h = 0.f;   // the window, root-view pixels
    F32 intensity = 1.f;    // GMDXTPFXIntensity, 0..3
    bool quick = false;     // the short same-region variant (about 0.5 s all told, a world one up to ~2 s: no real hold)
    LLVector2 avatar;       // the agent on screen at departure, re-taken when the arrival starts (centre if not visible)
    LLViewerTexture* frozen = nullptr;   // the last frame before departure: world + HUD attachments, no UI
    bool arriving = false;  // the arrival has started (arrive(); `a` runs)
    bool left = false;      // cross-region: the agent is in the new region already (the old world is gone)
    LLVector3d from;        // the agent (global) when the teleport was asked for / the preview started
};

class Effect
{
public:
    virtual ~Effect() = default;
    virtual const char* name() const = 0;           // its GMDXTPFX*Effect setting value, lower case
    virtual bool quick() const { return false; }    // has a short same-region variant
    virtual bool crossRegion() const { return true; }   // offered for teleports to another region
    virtual F32 departSecs() const { return 1.f; }  // the departure at GMDXTPFXDuration 1 (cross-region)
    virtual F32 arriveFrom() const { return 1.f; }  // departure progress before an arrival may start
    virtual const char* departSound() const { return nullptr; }   // app_settings/sounds/teleport/<file>
    virtual const char* arriveSound() const { return "tpfx_whoosh.wav"; }
    virtual void begin(const Frame&) {}             // a new run, its capture in: build the run's state
    virtual void depart(const Frame& f) = 0;
    virtual void hold(const Frame& f);              // default: the dark, faintly breathing hold
    virtual void arrive(const Frame& f) = 0;
    virtual void end() {}                           // the run is over (however it ended): free its state
    virtual bool inWorld() const { return false; }  // a 3D effect (above)
    virtual void tick(const Frame&) {}              // every idle frame while it runs, before the draw
    virtual bool hidesAvatar(const Frame&) const { return false; }   // the own avatar is hidden (main view) now
};

// ---- shared helpers (gmdxtpfx.cpp) ----
F32 smooth(F32 a, F32 b, F32 x);   // smoothstep
F32 rnd(F32 a, F32 b);
// The dark hold (reveal 0) or an iris opening on the live scene from the screen centre (reveal 0..1).
void drawCover(const Frame& f, F32 reveal);
void veil(const Frame& f, F32 k);   // the dark hold at opacity k (a world effect fading into / out of it)
void fill(const Frame& f, const LLColor4& c);   // the whole window
// The frozen frame: its part (u0,v0)-(u1,v1) (0..1, origin bottom-left) into (x0,y0)-(x1,y1).
void drawFrozen(const Frame& f, const LLColor4& c, F32 x0, F32 y0, F32 x1, F32 y1,
                F32 u0 = 0.f, F32 v0 = 0.f, F32 u1 = 1.f, F32 v1 = 1.f);
// The frozen frame as a polar mesh about c out to rmax (rings crowded toward c): warp(r, angle, pos, col)
// moves the frame's point at (r, angle) from c to pos and tints it (col comes in white, alpha f.alpha).
// Mesh points that sample outside the frame fade out instead of smearing its edge.
typedef std::function<void(F32 r, F32 angle, LLVector2& pos, LLColor4& col)> Warp;
void drawFrozenPolar(const Frame& f, const LLVector2& c, F32 rmax, S32 rings, S32 sectors, const Warp& warp);
F32 farCorner(const Frame& f, const LLVector2& p);   // p to the window's farthest corner
}
