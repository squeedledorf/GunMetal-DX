/**
 * @file gmdxtpfxworld.cpp
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

// <GMDX> whole file: see gmdxtpfxworld.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfx.h"
#include "gmdxtpfxworld.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcleanparticlestore.h"
#include "llimage.h"
#include "llpartdata.h"
#include "llrand.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llworld.h"

#include <algorithm>

using namespace GMDXTPFX::World;

namespace
{
constexpr U32 INVALID = LLCleanParticleStore::INVALID;
constexpr U32 ID_BASE = 0xF0000000u;      // source ids no LLViewerPartSource reaches (they count up from 1)
constexpr U32 WARM_ID = 0xEFFFFF00u;      // one row per texture while warm()
constexpr F64 WARM_SECS = 15.0;
constexpr U32 PAIR_ADD = LLCleanParticleStore::BLEND_FACTOR_SOURCE_ALPHA * LLCleanParticleStore::NUM_BLEND_FACTORS +
                         LLCleanParticleStore::BLEND_FACTOR_ONE;
constexpr F32 STREAK_OVERLAP = 1.25f;     // a bolt segment's strip is this much longer (its ends taper into the next)
constexpr F32 STREAK_SPEED = 0.01f;       // m/s: only to lay the strip along the segment (FOLLOW_VELOCITY)

F64 now() { return LLFrameTimer::getElapsedSeconds(); }
F32 frand(F32 a, F32 b) { return a + (b - a) * ll_frand(); }
F32 smooth(F32 a, F32 b, F32 x) { const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

// ---- the textures (made once, kept for the session) ----

// value noise on a `period`-cell grid that wraps
F32 lattice(S32 x, S32 y, U32 seed)
{
    U32 h = (U32)x * 374761393u + (U32)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (F32)((h ^ (h >> 16)) & 0xffff) / 65535.f;
}
F32 valueNoise(F32 x, F32 y, S32 period, U32 seed)
{
    const S32 x0 = (S32)floorf(x), y0 = (S32)floorf(y);
    const F32 fx = smooth(0.f, 1.f, x - x0), fy = smooth(0.f, 1.f, y - y0);
    auto at = [&](S32 i, S32 j) { return lattice(((i % period) + period) % period, ((j % period) + period) % period, seed); };
    return lerp(lerp(at(x0, y0), at(x0 + 1, y0), fx), lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}
// u, v in 0..1: 0.5 / 0.3 / 0.2 of 4, 8, 16 cells
F32 fbm(F32 u, F32 v, U32 seed)
{
    return 0.5f * valueNoise(u * 4.f, v * 4.f, 4, seed) + 0.3f * valueNoise(u * 8.f, v * 8.f, 8, seed + 1) +
           0.2f * valueNoise(u * 16.f, v * 16.f, 16, seed + 2);
}

struct Texel { F32 shade, alpha; };

// (fx, fy) -1..1, fy up (the billboard's top: image rows go bottom-up), (u, v) 0..1
Texel texel(Tex t, F32 fx, F32 fy, F32 u, F32 v)
{
    const F32 r = sqrtf(fx * fx + fy * fy);
    switch (t)
    {
    case Tex::SMOKE0: case Tex::SMOKE1: case Tex::SMOKE2:
    {
        // a lumpy puff: a noisy edge, uneven density, lit from above
        const U32 seed = 11u + 7u * (U32)((U8)t - (U8)Tex::SMOKE0);
        const F32 n = fbm(u, v, seed), n2 = fbm(u + 0.37f, v + 0.61f, seed + 5);
        // (denser than a wisp: the first headless recording read as thin haze)
        F32 a = (1.f - smooth(0.25f, 1.f, r + 0.38f * (n - 0.5f))) * (0.72f + 0.28f * n2);
        a = llmax(a, 0.f);
        const F32 shade = llclamp(0.6f + 0.24f * (0.5f + 0.5f * fy) + 0.3f * (n2 - 0.5f), 0.35f, 1.f);
        return { shade, a };
    }
    case Tex::BILLOW0: case Tex::BILLOW1:
    {
        // a cauliflower billow: a few round lobes, soft edged, each shaded as a ball lit from the top
        static const F32 lobes[][3] = { { 0.f, -0.12f, 0.52f }, { -0.4f, 0.1f, 0.38f }, { 0.38f, 0.06f, 0.4f },
                                        { -0.06f, 0.4f, 0.38f }, { 0.25f, -0.45f, 0.3f }, { -0.35f, -0.4f, 0.28f } };
        const bool alt = t == Tex::BILLOW1;
        const F32 n = fbm(u, v, alt ? 41u : 31u);
        F32 d = 1e9f, lit = 0.f;
        for (const auto& l : lobes)
        {
            const F32 cx = alt ? -l[0] : l[0], cy = alt ? l[1] * 0.9f + 0.03f : l[1];
            const F32 dx = fx - cx, dy = fy - cy, q = sqrtf(dx * dx + dy * dy);
            if (q - l[2] < d)
            {
                d = q - l[2];
                const F32 k = llmin(q / l[2], 1.f), nz = sqrtf(llmax(0.f, 1.f - k * k));
                lit = llmax(0.f, 0.55f * (dy / l[2]) - 0.2f * (dx / l[2]) + 0.8f * nz);
            }
        }
        const F32 a = 1.f - smooth(-0.14f, 0.03f, d + 0.08f * (n - 0.5f));
        return { llclamp(0.6f + 0.4f * lit + 0.1f * (n - 0.5f), 0.5f, 1.f), a };
    }
    case Tex::GLOW:
        return { 1.f, expf(-r * r * 5.f) * (1.f - smooth(0.85f, 1.f, r)) };
    case Tex::STREAK:
    {
        // across (fx): a hot core in a wide halo; along (v): the ends taper (overlapping segments add up to one)
        const F32 across = llmax(expf(-fx * fx / 0.012f), 0.45f * expf(-fx * fx / 0.12f));
        // each end tapers over the share of the strip its neighbour overlaps (two tapers add up to one)
        const F32 end = (STREAK_OVERLAP - 1.f) / STREAK_OVERLAP;
        return { 1.f, across * smooth(0.f, end, v) * smooth(0.f, end, 1.f - v) };
    }
    case Tex::STAR:
    {
        const F32 ax = fabsf(fx), ay = fabsf(fy);
        const F32 a = llmax(expf(-ax * 14.f) * expf(-fy * fy * 10.f), expf(-ay * 14.f) * expf(-fx * fx * 10.f)) + 0.7f * expf(-r * r * 30.f);
        return { 1.f, llmin(a, 1.f) * (1.f - smooth(0.85f, 1.f, r)) };
    }
    case Tex::LEAF:
    {
        // a pointed leaf along y with a stem and a lighter middle vein (tinted by the particle)
        const F32 s = 0.5f * (fy * 0.92f + 1.f);
        const F32 w = s > 0.f && s < 1.f ? 0.46f * powf(sinf(F_PI * s), 0.75f) : 0.f;
        F32 a = w > 0.f ? 1.f - smooth(w - 0.07f, w, fabsf(fx)) : 0.f;
        if (fy < -0.85f && fabsf(fx) < 0.035f) a = 1.f;   // the stem
        const F32 vein = 1.f - smooth(0.f, 0.05f, fabsf(fx));
        return { llclamp(0.78f + 0.22f * vein - 0.18f * (w > 0.f ? fabsf(fx) / w : 0.f), 0.f, 1.f), a };
    }
    case Tex::PAPER:
    {
        // a torn scrap: a rectangle with a ragged edge
        const F32 n = fbm(u, v, 77u);
        const F32 d = llmax(fabsf(fx) - 0.72f, fabsf(fy) - 0.88f) + 0.12f * (n - 0.5f);
        return { 0.9f + 0.1f * n, 1.f - smooth(-0.05f, 0.f, d) };
    }
    default:
        return { 1.f, 0.f };
    }
}

LLPointer<LLImageRaw> makeImage(Tex t)
{
    const S32 w = t == Tex::STREAK ? 32 : (t == Tex::GLOW || t == Tex::STAR || t == Tex::LEAF || t == Tex::PAPER ? 64 : 128);
    const S32 h = t == Tex::STREAK ? 128 : w;
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)w, (U16)h, 4);
    if (raw->isBufferInvalid()) return nullptr;
    U8* d = raw->getData();
    for (S32 y = 0; y < h; ++y)
        for (S32 x = 0; x < w; ++x)
        {
            const F32 u = ((F32)x + 0.5f) / w, v = ((F32)y + 0.5f) / h;
            const Texel tx = texel(t, u * 2.f - 1.f, v * 2.f - 1.f, u, v);
            U8* p = d + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = (U8)llclamp(tx.shade * 255.f + 0.5f, 0.f, 255.f);
            p[3] = (U8)llclamp(tx.alpha * 255.f + 0.5f, 0.f, 255.f);
        }
    return raw;
}

// The textures as LL textures with no asset behind them (as gmdxnightsky.cpp makes its own):
// setFromRaw keeps the pixels as the Vulkan upload's source, and the particle path resolves them
// by id (getFetchedTexture finds them in the texture list). Never released (ponytail: they would
// outlive LL's texture list at exit; ten small images).
const LLUUID& texId(Tex t)
{
    static LLUUID ids[(size_t)Tex::COUNT];
    static bool made = false;
    if (!made)
    {
        made = true;
        static auto* held = new std::vector<LLPointer<LLViewerFetchedTexture>>();
        for (U8 i = 0; i < (U8)Tex::COUNT; ++i)
        {
            LLPointer<LLImageRaw> raw = makeImage((Tex)i);
            LLViewerFetchedTexture* tex = raw.notNull() ? LLViewerTextureManager::getFetchedTexture(raw, FTT_LOCAL_FILE, false) : nullptr;
            if (!tex) continue;
            tex->setFromRaw(0, raw);
            held->push_back(tex);
            ids[i] = tex->getID();
        }
    }
    return ids[(size_t)t];
}

// ---- the parts ----

struct Live
{
    Part p;
    F64 t0 = 0.0;          // when it starts
    F64 born = -1.0;       // fed its spawn at
    U32 id = 0, slot = INVALID;
    S32 seg = -1;          // flip: the segment the current particle is
    F32 ph[3] = {};        // churn phases
    bool landed = false, rested = false;   // debris on the ground / no longer turning over
    LLVector3 landedOff;
    CleanEmitter e;
};
std::vector<Live> sLive;

struct Pulse
{
    LLVector3d at;
    LLColor3 rgb;
    F32 radius, attack, decay;
    F64 t0;
};
std::vector<Pulse> sPulses;

F64 sWarmUntil = -1.0;
U32 sFedFrame = 0;

void setPos(CleanEmitter& e, const LLVector3& p)
{
    for (S32 k = 0; k < 3; ++k) e.pos[k] = e.target[k] = p.mV[k];
    e.target[3] = p.mV[VZ];   // (the bounce plane: sparks bounce on the ground they start on)
}

void setLook(CleanEmitter& e, const LLColor4& c0, const LLColor4& c1, F32 w0, F32 h0, F32 w1, F32 h1, F32 life)
{
    for (S32 k = 0; k < 4; ++k) { e.start_color[k] = c0.mV[k]; e.end_color[k] = c1.mV[k]; }
    e.scale[0] = w0; e.scale[1] = h0; e.scale[2] = w1; e.scale[3] = h1;
    e.accel[3] = life;
}

CleanEmitter baseEmitter(const Part& p, U32 id)
{
    CleanEmitter e{};
    e.rot[3] = e.prev_rot[3] = e.src_rot[3] = 1.f;
    U32 flags = LLPartData::LL_PART_INTERP_COLOR_MASK | LLPartData::LL_PART_INTERP_SCALE_MASK;
    if (p.emissive) flags |= LLPartData::LL_PART_EMISSIVE_MASK;
    e.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_DROP;
    if (p.kind == Part::MOVE)
    {
        flags |= LLPartData::LL_PART_FOLLOW_SRC_MASK;   // it sits on its emitter, which the CPU moves
        setLook(e, p.c0, p.c1, p.size0, p.size0 * p.aspect, p.size1, p.size1 * p.aspect, p.life);
    }
    else if (p.kind == Part::STREAK)
    {
        // launched along the segment at 1 cm/s: FOLLOW_VELOCITY lays the strip along it
        LLVector3 d(p.to - p.at);
        const F32 len = d.normVec();
        if (d.mV[VZ] < 0.f) d = -d;   // (the strip is symmetric: keeps the rotation away from the 180 degree case)
        LLQuaternion q(-d.mV[VY], d.mV[VX], 0.f, 1.f + d.mV[VZ]);   // +Z -> d, (x, y, z, w) as the GPU's qrot reads it
        q.normalize();
        for (S32 k = 0; k < 4; ++k) e.rot[k] = q.mQ[k];
        e.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_ANGLE_CONE;
        e.speed_angle[0] = e.speed_angle[1] = STREAK_SPEED;
        flags |= LLPartData::LL_PART_FOLLOW_VELOCITY_MASK;
        setLook(e, p.c0, p.c1, p.size0, len * STREAK_OVERLAP, p.size0, len * STREAK_OVERLAP, p.life);
    }
    else
    {
        e.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_EXPLODE;
        e.pos[3] = 0.05f;   // burst radius
        e.speed_angle[0] = p.speed0;
        e.speed_angle[1] = p.speed1;
        for (S32 k = 0; k < 3; ++k) e.accel[k] = p.accel.mV[k];
        flags |= LLPartData::LL_PART_FOLLOW_VELOCITY_MASK | LLPartData::LL_PART_BOUNCE_MASK;
        setLook(e, p.c0, p.c1, p.size0 * p.aspect, p.size0, p.size1 * p.aspect, p.size1, p.life);
    }
    e.glow[0] = e.glow[1] = p.glow;   // (the glow pass scales it by the sprite's alpha: it fades with it)
    e.misc[1] = flags;
    e.misc[3] = id;   // the source id every particle carries (the GPU follows this row while it still names it)
    e.blend[0] = p.additive ? PAIR_ADD : LLCleanParticleStore::BLEND_PAIR_ALPHA;
    e.blend[1] = LLCleanParticleStore::MOTION_NORMAL;
    return e;
}

// MOVE: where it is `age` s after birth (from its start), m. Drag pulls the velocity toward the
// terminal accel / drag; the churn grows in as the puff slows.
LLVector3 offset(const Live& l, F32 age)
{
    const Part& p = l.p;
    const F32 k = llmax(p.drag, 0.05f);
    const LLVector3 vt = p.accel * (1.f / k);
    LLVector3 o = vt * age + (p.vel - vt) * ((1.f - expf(-k * age)) / k);
    if (p.wobble > 0.f)
    {
        const F32 w = F_TWO_PI * p.wobbleHz * age, g = p.wobble * (1.f - expf(-2.f * age));
        o += LLVector3(sinf(w + l.ph[0]), sinf(1.3f * w + l.ph[1]), 0.4f * sinf(0.7f * w + l.ph[2])) * g;
    }
    return o;
}

// one part, this frame; false: done with it (its particles carry on on the GPU)
bool feedOne(LLCleanParticleStore& ps, Live& l, F64 t)
{
    const Part& p = l.p;
    if (!l.id) l.id = p.id ? p.id : GMDXTPFX::World::newId();
    const U32 slot = ps.getOrCreateEmitter(l.id);   // (also: still fed, the store keeps the row)
    if (slot == INVALID) return false;
    if (slot != l.slot)
    {
        l.slot = slot;
        ps.setEmitterTexture(slot, texId(p.tex));
    }
    U32 spawn = 0;
    if (l.born < 0.0)
    {
        l.born = t;
        l.e = baseEmitter(p, l.id);
        spawn = p.kind == Part::BURST ? llmax(p.count, 1u) : 1u;
    }
    CleanEmitter& e = l.e;
    if (p.kind == Part::STREAK)
    {
        setPos(e, gAgent.getPosAgentFromGlobal((p.at + p.to) * 0.5));
        e.misc[2] = spawn;
        ps.setEmitter(slot, e);
        return false;
    }
    if (p.kind == Part::BURST)
    {
        setPos(e, gAgent.getPosAgentFromGlobal(p.at));
        e.misc[2] = spawn;
        ps.setEmitter(slot, e);
        return false;
    }

    const F32 age = (F32)(t - l.born);
    if (age >= p.life) return false;
    const F32 k = age / p.life;
    if (p.flip > 0.f && !l.rested)
    {
        // a scrap turning over: one particle per half turn, its width going +w -> -w (edge-on halfway),
        // the next one mirrored; it stays opaque until the last third of its life, then fades. Landed,
        // the half turn under way ends and one still particle lies there for the rest of its life.
        const S32 seg = (S32)(age / p.flip);
        if (seg != l.seg)
        {
            l.seg = seg;
            l.rested = l.landed;
            const F32 a0 = l.rested ? age : seg * p.flip, a1 = l.rested ? p.life : llmin((seg + 1) * p.flip, p.life);
            auto look = [&](F32 a) { LLColor4 c = lerp(p.c0, p.c1, a / p.life); c.mV[VALPHA] = p.c0.mV[VALPHA] * (1.f - smooth(0.65f, 1.f, a / p.life)); return c; };
            const F32 sign = (seg & 1) ? -1.f : 1.f, turn = l.rested ? sign : -sign;
            const F32 w0 = lerp(p.size0, p.size1, a0 / p.life), w1 = lerp(p.size0, p.size1, a1 / p.life);
            setLook(e, look(a0), look(a1), sign * w0, w0 * p.aspect, turn * w1, w1 * p.aspect, (a1 - a0) * 1.06f);
            spawn = 1;
        }
    }
    LLVector3 off = l.landed ? l.landedOff : offset(l, age);
    if (!l.landed && p.ground > -1.0e8)
    {
        const F32 size = lerp(p.size0, p.size1, k) * p.aspect;
        const F64 floor_z = p.ground + p.clear * size;
        if (p.at.mdV[VZ] + off.mV[VZ] < floor_z)
        {
            off.mV[VZ] = (F32)(floor_z - p.at.mdV[VZ]);
            if (p.clear < 0.1f)
            {
                l.landed = true;   // debris lies where it fell
                l.landedOff = off;
            }
        }
    }
    setPos(e, gAgent.getPosAgentFromGlobal(p.at + LLVector3d(off)));
    e.misc[2] = spawn;
    ps.setEmitter(slot, e);
    return true;
}

// A row per texture, fed without spawning, so the renderer resolves the textures before the first
// burst; once per warm(), one invisible additive particle, so the additive blend pipeline exists.
void feedWarm(LLCleanParticleStore& ps, F64 t)
{
    if (sWarmUntil < 0.0) return;
    if (t > sWarmUntil)
    {
        sWarmUntil = -1.0;
        return;
    }
    for (U8 i = 0; i < (U8)Tex::COUNT; ++i)
    {
        const U32 id = WARM_ID + i, slot = ps.getOrCreateEmitter(id);
        if (slot == INVALID) return;
        CleanEmitter e{};
        e.misc[3] = id;
        e.accel[3] = 1.f;
        ps.setEmitter(slot, e);
        ps.setEmitterTexture(slot, texId((Tex)i));
    }
}
}   // namespace

// ---- GMDXTPFX::World ----

U32 GMDXTPFX::World::newId()
{
    static U32 n = 0;
    n = (n + 1) & 0x0FFFFFFFu;
    if (!n) n = 1;
    return ID_BASE | n;
}

void GMDXTPFX::World::add(const Part& p)
{
    if (!LLCleanParticleStore::instance().ready() || p.life <= 0.f) return;
    Live l;
    l.p = p;
    l.t0 = now() + llmax(p.delay, 0.f);
    for (F32& a : l.ph) a = frand(0.f, F_TWO_PI);
    sLive.push_back(std::move(l));
}

void GMDXTPFX::World::light(const LLVector3d& at, const LLColor3& rgb, F32 radius, F32 delay, F32 attack, F32 decay)
{
    sPulses.push_back({ at, rgb, radius, llmax(attack, 0.005f), llmax(decay, 0.01f), now() + llmax(delay, 0.f) });
}

void GMDXTPFX::World::warm()
{
    if (!LLCleanParticleStore::instance().ready()) return;
    texId(Tex::GLOW);   // (makes them all)
    sWarmUntil = now() + WARM_SECS;
    Part p;
    p.kind = Part::BURST;
    p.count = 1;
    p.life = 0.05f;
    p.size0 = p.size1 = 0.01f;
    p.c0 = p.c1 = LLColor4(1.f, 1.f, 1.f, 0.f);
    p.additive = p.emissive = true;
    p.at = gAgent.getPositionGlobal();
    add(p);
}

void GMDXTPFX::World::clear()
{
    sLive.clear();
    sPulses.clear();
    sWarmUntil = -1.0;
}

F64 GMDXTPFX::World::Body::groundAt(const LLVector3d& p) const
{
    if (fabs(landAtFoot - foot) > 0.6) return foot;   // a floor: flat
    const F64 land = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(p.mdV[VX], p.mdV[VY], foot));
    return fabs(land - foot) < 3.0 ? land : foot;
}

GMDXTPFX::World::Body GMDXTPFX::World::body(const LLVector3d& centre)
{
    Body b;
    b.centre = centre;
    F32 to_foot = 1.f, h = 1.9f;
    if (isAgentAvatarValid())
    {
        to_foot = gAgentAvatarp->getPelvisToFoot();
        h = gAgentAvatarp->mBodySize.mV[VZ];
    }
    if (!(to_foot > 0.2f && to_foot < 3.f)) to_foot = 1.f;
    if (!(h > 0.5f && h < 6.f)) h = 1.9f;
    b.foot = centre.mdV[VZ] - to_foot;
    b.height = h;
    b.landAtFoot = LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(centre.mdV[VX], centre.mdV[VY], b.foot));
    return b;
}

LLVector3 GMDXTPFX::World::randomDir(F32 zmin)
{
    const F32 z = frand(llclamp(zmin, -1.f, 1.f), 1.f), a = frand(0.f, F_TWO_PI), s = sqrtf(llmax(0.f, 1.f - z * z));
    return LLVector3(s * cosf(a), s * sinf(a), z);
}

// ---- the hooks (gmdxtpfx.h) ----

void GMDXTPFX::feedParticles()
{
    if (sLive.empty() && sWarmUntil < 0.0) return;
    LLCleanParticleStore& ps = LLCleanParticleStore::instance();
    if (!ps.ready() || sFedFrame == (U32)gFrameCount) return;   // (once a frame, whoever calls first)
    sFedFrame = (U32)gFrameCount;
    const F64 t = now();
    feedWarm(ps, t);
    for (size_t i = 0; i < sLive.size();)
    {
        if (t < sLive[i].t0 || feedOne(ps, sLive[i], t))
        {
            ++i;
            continue;
        }
        sLive[i] = std::move(sLive.back());
        sLive.pop_back();
    }
}

bool GMDXTPFX::worldLight(F32 pos[3], F32 rgb[3], F32& radius)
{
    if (sPulses.empty()) return false;
    const F64 t = now();
    sPulses.erase(std::remove_if(sPulses.begin(), sPulses.end(),
                                 [t](const Pulse& p) { return (F32)(t - p.t0) > p.attack + 6.f * p.decay; }),
                  sPulses.end());
    // one light: the brightest pulse now
    S32 best = -1;
    F32 best_k = 0.f, best_lum = 0.01f;
    for (size_t i = 0; i < sPulses.size(); ++i)
    {
        const Pulse& p = sPulses[i];
        const F32 dt = (F32)(t - p.t0);
        const F32 k = dt < 0.f ? 0.f : (dt < p.attack ? dt / p.attack : expf(-(dt - p.attack) / p.decay));
        const F32 lum = k * llmax(p.rgb.mV[0], llmax(p.rgb.mV[1], p.rgb.mV[2]));
        if (lum > best_lum)
        {
            best = (S32)i;
            best_k = k;
            best_lum = lum;
        }
    }
    if (best < 0) return false;
    const Pulse& p = sPulses[best];
    const LLVector3 a = gAgent.getPosAgentFromGlobal(p.at);
    for (S32 k = 0; k < 3; ++k)
    {
        pos[k] = a.mV[k];
        rgb[k] = p.rgb.mV[k] * best_k;
    }
    radius = p.radius;
    return true;
}
