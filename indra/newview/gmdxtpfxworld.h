/**
 * @file gmdxtpfxworld.h
 * @brief GunMetal-DX: teleport effects that happen in the world (smoke, debris, lightning, a light).
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

// <GMDX> whole file. Only the teleport effect files include this.
//
// Local particles in the live world, drawn by the viewer's own GPU particle system: each part
// here is one emitter row of LLCleanParticleStore under a source id no LLViewerPartSource uses
// (nothing is sent; nobody else sees them). The renderer simulates, sorts, lights and draws
// them like any script particle; smoke is lit by the scene and casts shadows, the glows and the
// bolt are emissive and additive. The textures are made here once, from code.
//  * MOVE parts follow their emitter (FOLLOW_SRC): the CPU moves the emitter every frame along
//    a drag / buoyancy / churn path, so a puff can rush out and settle, which the GPU's plain
//    ballistic integration cannot do. Size and colour go linearly from birth to death (GPU).
//  * STREAK parts are fixed glowing strips from a to b (a bolt's segments): a billboard laid
//    along the segment (FOLLOW_VELOCITY on a 1 cm/s launch), so it faces the camera from anywhere.
//  * BURST parts are n GPU-simulated sparks flung from one point: gravity, bounce, streaking.
// Everything is in global coordinates and lives a few seconds on its own, independent of the
// effect that started it (smoke left behind keeps rolling after the effect is over). Feeding:
// LLViewerPartSim::updateSimulation (feedParticles, gmdxtpfx.h) right after it clears the
// frame's spawn counts, and gmdxtpfx.cpp's idle tick on the frames idle() skips the particle
// update (gTeleportDisplay: a teleport under way).

#pragma once

#include "v3color.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4color.h"

namespace GMDXTPFX
{
namespace World
{
enum class Tex : U8 { SMOKE0, SMOKE1, SMOKE2, BILLOW0, BILLOW1, GLOW, STREAK, STAR, LEAF, PAPER, COUNT };

struct Part
{
    enum Kind : U8 { MOVE, STREAK, BURST };
    Kind kind = MOVE;
    Tex tex = Tex::GLOW;
    LLVector3d at;                  // global: where it starts (STREAK: one end; BURST: the source)
    LLVector3d to;                  // STREAK: the other end
    LLVector3 vel;                  // MOVE: m/s at birth
    LLVector3 accel;                // MOVE: m/s^2 (buoyancy +z, gravity -z); terminal velocity accel / drag
    F32 drag = 3.f;                 // MOVE: 1/s (a puff rushes out and settles)
    F32 wobble = 0.f, wobbleHz = 0.f;   // MOVE: churn, m and turns per second
    F64 ground = -1.0e9;            // MOVE: global z of the ground under it
    F32 clear = 0.3f;               // MOVE: share of its size kept above the ground; below 0.1 it lands and stays
    F32 flip = 0.f;                 // MOVE: > 0, a flat scrap turning over every `flip` s (the particle is reborn mirrored)
    F32 delay = 0.f;                // s from now
    F32 life = 1.f;                 // s
    F32 size0 = 1.f, size1 = 1.f;   // m at birth / death (STREAK: its width; BURST: the spark's length)
    F32 aspect = 1.f;               // height / width (BURST: width / length)
    LLColor4 c0, c1;                // rgba at birth / death (sRGB, as script particles)
    F32 glow = 0.f;                 // SL glow (fades with c1's alpha share)
    bool additive = false;          // additive blend (glows, sparks, the bolt)
    bool emissive = false;          // fullbright instead of lit by the scene
    U32 count = 0;                  // BURST: sparks
    F32 speed0 = 0.f, speed1 = 0.f; // BURST: m/s
    U32 id = 0;                     // 0: a new emitter row; else newId()'s (a bolt's segment struck again)
};

void add(const Part& p);
U32 newId();
// A light pulse (the poof, the strike): rgb linear (intensity folded in), LL light radius (m).
void light(const LLVector3d& at, const LLColor3& rgb, F32 radius, F32 delay, F32 attack, F32 decay);
// Makes the textures now and has the renderer take them (and the additive blend pipeline) in, so
// the first burst draws in its first frame. arm() / preview() call it.
void warm();
void clear();   // everything goes (logout, disconnect)

// The avatar standing at `centre` (global; gAgent's position): the ground under it and its height.
struct Body
{
    LLVector3d centre;
    F64 foot = 0.0;       // global z of its feet
    F32 height = 1.9f;    // m
    F64 landAtFoot = 0.0; // the terrain under it (> 0.6 m off the feet: it stands on a floor)
    F64 groundAt(const LLVector3d& p) const;   // the ground under p (the terrain near its feet, else its floor)
    F32 scale() const { return height / 1.9f; }
};
Body body(const LLVector3d& centre);

// A random unit vector, uniform on the sphere; z > zmin only.
LLVector3 randomDir(F32 zmin = -1.f);
}
}
