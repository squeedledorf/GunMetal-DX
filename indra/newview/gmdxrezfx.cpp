/**
 * @file gmdxrezfx.cpp
 * @brief GunMetal-DX rez effects: what objects do on screen when they appear and disappear.
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

// <GMDX> See gmdxrezfx.h.

#include "llviewerprecompiledheaders.h"
#include "gmdxrezfx.h"

#include "llagent.h"
#include "llapp.h"
#include "llavatarappearancedefines.h"   // isBakedImageId (GMDXRezFxWaitForTextures)
#include "llappviewer.h"            // gWindVec
#include "llcleaninstancestore.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"               // gGL (the transporter's beam: renderOverlay)
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llviewerparcelmgr.h"
#include "llvoavatar.h"
#include "llvolume.h"               // the de-rez debris: points on the object's surface
#include "llworld.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using namespace GMDXRezFx;

// Tron: the wireframe scan takes this fraction of the effect's duration (Vulkanstorm's
// REZ_WIRE_BUILD), and the wireframe waits at most this long for the textures before the fill
// scan starts anyway (Vulkanstorm waited 10 s; capped low here: the wireframe is only there to
// cover the white blobs while textures load).
constexpr F32 kWireBuild = 0.6f;
constexpr F32 kTextureWait = 2.0f;
// WAITING FOR TEXTURES (GMDXRezFxWaitForTextures, off by default). A two-part rez-in plays its first
// part, then holds there -- idling, not frozen -- until the object's textures are loaded for this
// viewer (stillLoading), and only then plays its final part. Where it holds, as a fraction of the
// effect (holdPoint): Tron at the end of its wireframe scan (its own stage 0, as before, just
// longer and with an idle loop); hologram at 0.55, a full projection, before it settles; voxel at
// 0, all blocks, before any turns into surface; 3D printer at 0.8, before the final layers and
// the cooling into the real surface (kept warm until then); transporter at 0.5, sparkles and a
// half-filled cloud of cells, before it turns solid; fire at 0.06, a smouldering charred shell,
// before the burn-in front reaches the surface. Every rez-in still ends within kWaitMax of its
// start, a texture that never arrives included, so a hidden-mode effect never hides anything
// longer. Not in combat-safe (there it never waits, and a hold already running lets go).
constexpr F32 kWaitMax = 30.f;
constexpr F64 kWaitCheck = 0.1;   // a held object's textures are looked at 10 times a second
F32 holdPoint(U32 fx)
{
    switch (fx)
    {
    case FX_FIRE: return 0.06f;
    case FX_HOLOGRAM: return 0.55f;
    case FX_VOXEL: return 0.f;
    case FX_PRINTER: return 0.8f;
    case FX_TRANSPORTER: return 0.5f;
    default: return -1.f;   // (Tron holds between its two stages; the de-rezzes never)
    }
}
F32 smooth01(F32 x) { x = llclamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }
// An object whose rows come back within this long (an avatar dropping out of the ingest and
// back, a re-registered glTF scene) does not play its rez-in again.
constexpr F64 kReplayGuard = 20.0;
// Frames a freed slot rests before reuse: a row's flag change and the table reach the GPU in
// the same frame, but a stale row must never name a slot that already means something else.
constexpr U32 kSlotRest = 4;
// Objects in an effect at once; past this, objects just appear / disappear.
constexpr size_t kMaxObjects = 8192;
// Longest de-rez (s): a dead flexi's skin entry is reaped 300 frames on (llcleanskinstore.cpp),
// 1.25 s at 240 fps; a dying object never outlives what poses it.
constexpr F32 kDeRezMax = 1.2f;
// HIDDEN MODE (the build effects, gmdx_rezfx.glsl rezfxHidden): their slots come from the upper
// half of the table, so instance bit 31 says "may hide" to the cull (mask buckets in the main
// view) and to every shader without a table read. Every other effect uses the lower half.
constexpr U32 kHideSlotFirst = 256;
// Of those, the 3D printer's (bits 31 + 30): the main view draws them double-sided, which caps the
// cut (clean_cull.comp, gmdx_rezfx.glsl rezfxBack). One slot per build (linkset / avatar), so its
// prims print in one frame under one nozzle, whenever their meshes arrive.
constexpr U32 kPrintSlotFirst = 384;
// The build effects take longer than the duration setting, or there is nothing to see of the
// nozzle and the layers (Tron's two scans run 1.6x it too). The printer: about 3.5 s at the
// default 0.6 s, 8 s at most; the transporter 1.5x, 6 s at most. Never in combat-safe.
constexpr F32 kPrinterLength = 3.5f / 0.6f;
constexpr F32 kPrinterMax = 8.f;
constexpr F32 kTransporterLength = 1.5f;
// De-rezzes with flying pieces or a second phase take longer than the duration setting, so they
// can be followed by eye -- still never past kDeRezMax, and 0.25 s in combat-safe.
F32 deRezScale(U32 fx)
{
    switch (fx)
    {
    case FX_DISINTEGRATE: return 1.8f;
    case FX_ASH: case FX_SHATTER: case FX_TRON_OUT: return 2.f;
    default: return 1.f;
    }
}
// De-rez debris (renderOverlay): points sampled on a dying object's surface at its death, at most
// kDebrisPerObj per object and kMaxDebris over all.
constexpr size_t kDebrisPerObj = 48;
constexpr size_t kMaxDebris = 2400;

struct SlotState
{
    U32 effect = FX_NONE;
    U32 stage = 0;
    F64 start = 0.0;
    F32 len = 1.f;
    U32 refs = 0;
    bool hide = false;   // a hidden-mode slot (>= kHideSlotFirst)
    LLUUID group;        // a printer slot: its build (linkset root / avatar)
    bool boxOk = false;  // printer: lo / hi hold the build's box (agent space)
    LLVector3 lo, hi;
    LLVector3 dir;       // shatter: the impact direction (unit, world)
    // <GMDX> Waiting for textures (kWaitMax). The table row's progress is p0 + (p1 - p0) x its
    // time through len, and its time runs from tbase (one clock across both parts). p1 < 1: the
    // first part, which holds at p1 while its objects wait; rest is then the final part's length.
    F64 tbase = 0.0;
    F32 p0 = 0.f, p1 = 1.f, rest = 0.f;
    bool wait = false;   // started waiting for textures (the table's wind.w)
    F32 fade = 0.f;      // a final part: the idle loop its first part had reached (eases out)
    U32 waitFrame = 0;   // a printer build: the last frame one of its prims still waited
};

// A point of the de-rez debris (renderOverlay), fixed at the object's death.
struct Bit
{
    LLVector3 w;       // agent space
    LLVector3 q;       // object space (m): ash, disintegrate; shatter: world-oriented from the centre
    LLVector3 c;       // disintegrate / shatter: its piece's centre, in that frame
    LLVector3 v;       // shatter: its shard's launch velocity (m/s, rzShardLaunch)
    F32 t0 = 0.f;      // disintegrate / shatter: when its piece breaks loose (progress)
    F32 r = 0.f;       // random 0..1 (shatter: its glint phase, rzHash13(id + 43))
    F32 sp = 0.f;      // disintegrate: its cube's speed factor (rezfxDisplace r3.y); shatter: its spin (rad/s)
};

struct Obj
{
    U32 slot = 0;      // 1-based
    U32 effect = FX_NONE;
    U32 stage = 0;
    F64 start = 0.0;   // when the current stage began
    F32 len = 1.f;     // its length (s)
    F32 wait = 0.f;    // Tron: longest the wireframe waits for textures
    bool waitTex = false;   // <GMDX> Tron: waits for its textures in full (stillLoading)
    F64 nextCheck = 0.0;    // <GMDX> a held object: when its textures are looked at next (kWaitCheck)
    bool dying = false;
    bool ended = false; // the stage reached its end (the object leaves the effect next frame)
    LLUUID wearer;     // a dying attachment: its wearer (whose skeleton poses it)
    // De-rez debris: the object's pose at its death and the points.
    LLVector3 pos, scale;
    LLQuaternion rot;
    std::vector<Bit> bits;
    // Shatter: the impact point (world-oriented, from pos), its burst's particle count (in the
    // debris cap with the bits) and their random seed.
    LLVector3 impact;
    U32 burst = 0;
    F32 seed = 0.f;
};
size_t debrisOf(const Obj& o) { return o.bits.size() + o.burst; }

SlotState sSlots[MAX_SLOTS + 1];               // [1..MAX_SLOTS]
std::vector<U32> sFree;                        // free slots [1, kHideSlotFirst)
std::vector<U32> sFreeHide;                    // free slots [kHideSlotFirst, kPrintSlotFirst)
std::vector<U32> sFreePrint;                   // free slots [kPrintSlotFirst, MAX_SLOTS]
std::unordered_map<LLUUID, U32> sGroupSlots;   // a build's live printer slot
size_t sDebris = 0;                            // debris points held
std::vector<std::pair<U32, U32>> sResting;     // (slot, frame freed)
std::unordered_map<LLUUID, Obj> sObjs;
std::unordered_map<LLUUID, F64> sRecent;       // rez-in played at (kReplayGuard)
// Objects whose first rows were already considered: an object rezzes in once per appearance.
// Selecting an object (and other render changes) re-creates its rows, which looked like a
// first appearance and replayed the rez-in on selection. Cleared when the object is killed.
std::unordered_set<LLUUID> sSeen;
std::unordered_map<U64, U32> sFrameSlots;      // (effect, stage) -> slot started at sFrameSlotsAt
F64 sFrameSlotsAt = -1.0;
Slot sTable[MAX_SLOTS];
U32 sHigh = 0, sGen = 0, sFrame = 0;
bool sHiding = false;   // a hidden-mode slot is live (hiding())
F64 sNow = 0.0, sLastUpdate = -1.0, sRecentPurge = 0.0;
bool sWire = false, sCollect = false;
std::vector<LLUUID> sNewIds;
// Killed linksets: the root's de-rez decision for each child (objectKilled), and the child's pose
// while it was still linked (its debris), until the root's markDead has killed them (the same
// call); cleared every frame.
struct LinkKill
{
    bool allowed = false;
    LLUUID wearer;
    LLVector3 pos;
    LLQuaternion rot;
};
std::unordered_map<LLUUID, LinkKill> sLinkset;

std::vector<U32>& poolOf(U32 s) { return s >= kPrintSlotFirst ? sFreePrint : (s >= kHideSlotFirst ? sFreeHide : sFree); }

// The printer's slot for a build: the live one (a prim arriving later joins the print where it
// is), or a new one from the print pool.
U32 allocGroupSlot(const LLUUID& group, F32 len, F32 hold = -1.f)
{
    auto it = sGroupSlots.find(group);
    if (it != sGroupSlots.end() && sSlots[it->second].refs != 0)
    {
        ++sSlots[it->second].refs;
        return it->second;
    }
    if (sFreePrint.empty()) return 0;
    const U32 s = sFreePrint.back();
    sFreePrint.pop_back();
    SlotState& st = sSlots[s];
    st = SlotState();
    st.effect = FX_PRINTER; st.start = sNow; st.len = llmax(len, 0.01f); st.refs = 1; st.hide = true; st.group = group;
    st.tbase = sNow;
    if (hold >= 0.f)
    {
        // <GMDX> waiting for textures: the whole build prints to the hold point, holds as one
        st.wait = true; st.p1 = hold; st.rest = len * (1.f - hold); st.len = llmax(len * hold, 0.01f);
    }
    sGroupSlots[group] = s;
    return s;
}

// <GMDX> hold >= 0: the first part of a rez-in that waits for textures (holds at that progress); from:
// the final part after that slot's hold (its clock goes on). wait: Tron's waiting wireframe.
U32 allocSlot(U32 effect, U32 stage, F32 len, bool hide = false, F32 hold = -1.f, U32 from = 0, bool wait = false)
{
    // Objects starting on the same frame clock share a slot (same effect, stage and mode).
    if (sFrameSlotsAt != sNow) { sFrameSlots.clear(); sFrameSlotsAt = sNow; }
    const U64 key = ((U64)effect << 32) | ((U64)from << 8) | ((U64)(hold >= 0.f || wait) << 4) | ((U64)stage << 1) | (hide ? 1u : 0u);
    auto it = sFrameSlots.find(key);
    if (it != sFrameSlots.end())
    {
        ++sSlots[it->second].refs;
        return it->second;
    }
    std::vector<U32>& pool = hide ? sFreeHide : sFree;
    if (pool.empty()) return 0;
    const U32 s = pool.back();
    pool.pop_back();
    SlotState& st = sSlots[s];
    st = SlotState();
    st.effect = effect; st.stage = stage; st.start = sNow; st.len = llmax(len, 0.01f); st.refs = 1; st.hide = hide;
    // <GMDX> waiting for textures
    st.tbase = sNow;
    st.wait = wait;
    if (hold >= 0.f) { st.wait = true; st.p1 = hold; st.rest = len * (1.f - hold); st.len = llmax(len * hold, 0.01f); }
    if (from)
    {
        const SlotState& f = sSlots[from];
        st.wait = true; st.p0 = f.p1; st.tbase = f.tbase;
        st.fade = smooth01(((F32)(sNow - f.start) - f.len) / 0.5f);
    }
    sFrameSlots.emplace(key, s);
    return s;
}

void derefSlot(U32 s)
{
    if (s == 0 || s > MAX_SLOTS || sSlots[s].refs == 0) return;
    if (--sSlots[s].refs == 0)
    {
        sSlots[s].effect = FX_NONE;
        if (sSlots[s].group.notNull())
        {
            auto g = sGroupSlots.find(sSlots[s].group);
            if (g != sGroupSlots.end() && g->second == s) sGroupSlots.erase(g);
        }
        sResting.emplace_back(s, sFrame);
        // (a slot started this frame and already empty must not be handed out again this frame)
        for (auto it = sFrameSlots.begin(); it != sFrameSlots.end(); ++it)
            if (it->second == s) { sFrameSlots.erase(it); break; }
    }
}

// Whether any texture an object's faces sample is still on its way: nothing decoded yet and
// not a known missing asset (LLViewerTexture discard level -1). Base colour of the legacy face
// and of a glTF material.
bool texturesLoading(LLViewerObject* obj)
{
    const U8 n = obj->getNumTEs();
    for (U8 i = 0; i < n; ++i)
    {
        const LLViewerTexture* t = obj->getTEImage(i);
        if (t && t->getDiscardLevel() < 0 && !t->isMissingAsset()) return true;
        const LLTextureEntry* te = obj->getTE(i);
        const LLFetchedGLTFMaterial* m = te ? static_cast<const LLFetchedGLTFMaterial*>(te->getGLTFRenderMaterial()) : nullptr;
        if (m && m->mBaseColorTexture.notNull() && m->mBaseColorTexture->getDiscardLevel() < 0
            && !m->mBaseColorTexture->isMissingAsset())
            return true;
    }
    return false;
}

// Temp-on-rez objects (bullets, projectiles, rezzed effects) never play an effect: a fixed
// rule, not a setting -- a firefight must not turn into fireworks.
bool isTemporary(const LLViewerObject* obj)
{
    const LLViewerObject* root = obj->getRootEdit();
    return obj->flagTemporaryOnRez() || (root && root->flagTemporaryOnRez());
}

bool isHud(const LLViewerObject* obj)
{
    const LLViewerObject* root = obj->getRootEdit();
    return obj->isHUDAttachment() || (root && root->isHUDAttachment());
}

// Worn by an avatar (any prim of an attachment linkset) or an avatar itself.
bool isAvatarOrWorn(const LLViewerObject* obj)
{
    if (obj->isAvatar()) return true;
    const LLViewerObject* root = obj->getRootEdit();
    return obj->isAttachment() || (root && root->isAttachment());
}

// <GMDX> WAITING FOR TEXTURES (kWaitMax): whether an object is still loading what it shows.
// - A face's texture is loaded once it has pixels AND either LL's fetcher is done with it (the
//   texture is "there and completely downloaded", LLViewerFetchedTexture::isFullyLoaded: fetched
//   down to the level the viewer asked for) or it is already as sharp as the level the viewer wants
//   for it now (getDesiredDiscardLevel -- from its size on screen, so a far object does not wait for
//   detail nobody would see). A missing asset counts as loaded (it will never come).
// - Faces looked at: the base colour of every face that shows (alpha above 0), legacy and glTF.
//   Bake faces of worn objects (IMG_USE_BAKED_*) are the wearer's appearance, below.
// - An avatar: until LL calls it fully loaded (its bakes, attachments and meshes). Something worn:
//   its own faces, and until its wearer is fully loaded, so an avatar finishes as a whole.
bool texLoaded(LLViewerTexture* t)
{
    LLViewerFetchedTexture* ft = dynamic_cast<LLViewerFetchedTexture*>(t);
    if (!ft || ft->isMissingAsset()) return true;
    const S32 d = ft->getDiscardLevel();
    return d >= 0 && (d <= ft->getDesiredDiscardLevel() || ft->isFullyLoaded());
}

bool stillLoading(LLViewerObject* obj)
{
    if (obj->isAvatar()) return !static_cast<LLVOAvatar*>(obj)->isFullyLoaded();
    if (isAvatarOrWorn(obj))
    {
        LLVOAvatar* av = obj->getAvatar();
        if (av && !av->isDead() && !av->isFullyLoaded()) return true;
    }
    const U8 n = obj->getNumTEs();
    for (U8 i = 0; i < n; ++i)
    {
        const LLTextureEntry* te = obj->getTE(i);
        if (!te || te->getAlpha() <= 0.f) continue;
        if (!LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(te->getID()) && !texLoaded(obj->getTEImage(i)))
            return true;
        const LLFetchedGLTFMaterial* m = static_cast<const LLFetchedGLTFMaterial*>(te->getGLTFRenderMaterial());
        if (m && m->mBaseColorTexture.notNull() && !texLoaded(m->mBaseColorTexture.get())) return true;
    }
    return false;
}

// Combat-safe (GMDXRezFxCombatSafe, off by default): on damage-enabled land -- the parcel or
// region the agent stands in -- no effect lasts longer than this, and the Tron wireframe does
// not wait for textures. Read once per frame (update).
constexpr F32 kCombatSafeMax = 0.25f;
bool sCombatSafe = false;

void refreshCombatSafe()
{
    static LLCachedControl<bool> s_safe(gSavedSettings, "GMDXRezFxCombatSafe", false);
    sCombatSafe = false;
    if (!s_safe) return;
    LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
    sCombatSafe = pm->allowAgentDamage(gAgent.getRegion(), pm->getAgentParcel());
}

// The effect's duration in seconds.
F32 duration()
{
    static LLCachedControl<F32> s_dur(gSavedSettings, "GMDXRezFxDuration", 0.6f);
    const F32 d = llclamp((F32)s_dur, 0.05f, 5.f);
    return sCombatSafe ? llmin(d, kCombatSafeMax) : d;
}

// A random one of the ticked effects (the random mix), or none if nothing is ticked.
U32 pickTicked(const U32* fx, LLCachedControl<bool>* const* ticks, U32 n)
{
    U32 pool[8];
    U32 k = 0;
    for (U32 i = 0; i < n; ++i)
        if (*ticks[i]) pool[k++] = fx[i];
    return k ? pool[llmin((U32)(ll_frand() * (F32)k), k - 1)] : (U32)FX_NONE;
}

// The rez-in for one object: the chosen effect, or for "random mix" (7) one of the ticked ones.
U32 pickRezIn(bool forObject = true)
{
    static LLCachedControl<S32> s_in(gSavedSettings, "GMDXRezFxInEffect", 1);
    const S32 v = s_in;
    if (v >= 1 && v <= 6) return (U32)v;
    if (v != 7) return FX_NONE;
    if (!forObject) return FX_TRON;   // (only "is any rez-in on")
    static LLCachedControl<bool> t0(gSavedSettings, "GMDXRezFxMixTron", true), t1(gSavedSettings, "GMDXRezFxMixFire", true),
        t2(gSavedSettings, "GMDXRezFxMixHologram", true), t3(gSavedSettings, "GMDXRezFxMixVoxel", true),
        t4(gSavedSettings, "GMDXRezFxMixPrinter", true), t5(gSavedSettings, "GMDXRezFxMixTransporter", true);
    static LLCachedControl<bool>* const ticks[6] = {&t0, &t1, &t2, &t3, &t4, &t5};
    static const U32 fx[6] = {FX_TRON, FX_FIRE, FX_HOLOGRAM, FX_VOXEL, FX_PRINTER, FX_TRANSPORTER};
    return pickTicked(fx, ticks, 6);
}

// The de-rez for one object: the chosen effect, or for "random mix" (6) one of the ticked ones.
// (7 and 8, Tron de-rez and shatter, came after the random mix.)
U32 pickDeRez()
{
    static LLCachedControl<S32> s_out(gSavedSettings, "GMDXRezFxOutEffect", 1);
    const S32 v = s_out;
    if (v >= 1 && v <= 5) return (U32)(FX_EMBER - 1 + v);
    if (v == 7) return FX_TRON_OUT;
    if (v == 8) return FX_SHATTER;
    if (v != 6) return FX_NONE;
    static LLCachedControl<bool> t0(gSavedSettings, "GMDXRezFxMixEmber", true), t1(gSavedSettings, "GMDXRezFxMixDisintegrate", true),
        t2(gSavedSettings, "GMDXRezFxMixAsh", true), t3(gSavedSettings, "GMDXRezFxMixGlitch", true),
        t4(gSavedSettings, "GMDXRezFxMixImplode", true), t5(gSavedSettings, "GMDXRezFxMixTronOut", true),
        t6(gSavedSettings, "GMDXRezFxMixShatter", true);
    static LLCachedControl<bool>* const ticks[7] = {&t0, &t1, &t2, &t3, &t4, &t5, &t6};
    static const U32 fx[7] = {FX_EMBER, FX_DISINTEGRATE, FX_ASH, FX_GLITCH, FX_IMPLODE, FX_TRON_OUT, FX_SHATTER};
    return pickTicked(fx, ticks, 7);
}

bool avatarsToo()
{
    static LLCachedControl<bool> s_av(gSavedSettings, "GMDXRezFxAvatars", true);
    return s_av;
}

// The build an object prints with (the printer's slot and frame): its avatar for anything worn,
// else its linkset root.
LLUUID groupOf(LLViewerObject* obj)
{
    if (obj->isAvatar()) return obj->getID();
    if (isAvatarOrWorn(obj))
    {
        LLVOAvatar* av = obj->getAvatar();
        if (av) return av->getID();
    }
    LLViewerObject* root = obj->getRootEdit();
    return root ? root->getID() : obj->getID();
}

// The agent-space box of a build (groupOf's key): an avatar (its body, standing) or a linkset.
bool groupBounds(const LLUUID& key, LLVector3& lo, LLVector3& hi)
{
    LLViewerObject* obj = gObjectList.findObject(key);
    if (!obj || obj->isDead()) return false;
    if (obj->isAvatar())
    {
        LLVOAvatar* av = static_cast<LLVOAvatar*>(obj);
        const LLVector3 pos = av->getPositionAgent();
        const F32 foot = pos.mV[VZ] - av->getPelvisToFoot();
        const F32 tall = av->mBodySize.mV[VZ] > 0.5f ? av->mBodySize.mV[VZ] : 1.9f;
        lo.set(pos.mV[VX] - 0.3f, pos.mV[VY] - 0.3f, foot);
        hi.set(pos.mV[VX] + 0.3f, pos.mV[VY] + 0.3f, foot + tall);
        return true;
    }
    const LLBBox rb = obj->getBoundingBoxAgent().getAxisAligned();
    lo = rb.getMinAgent();
    hi = rb.getMaxAgent();
    for (const LLViewerObject* child : obj->getChildren())
    {
        if (!child || child->isAvatar()) continue;
        const LLBBox cb = child->getBoundingBoxAgent().getAxisAligned();
        lo.set(llmin(lo.mV[VX], cb.getMinAgent().mV[VX]), llmin(lo.mV[VY], cb.getMinAgent().mV[VY]),
               llmin(lo.mV[VZ], cb.getMinAgent().mV[VZ]));
        hi.set(llmax(hi.mV[VX], cb.getMaxAgent().mV[VX]), llmax(hi.mV[VY], cb.getMaxAgent().mV[VY]),
               llmax(hi.mV[VZ], cb.getMaxAgent().mV[VZ]));
    }
    return true;
}

// The printer's layer count and infill rows for a build of height H and footprint W (m): fewer,
// clearer layers -- 5 for a small prop up to 16 for a building -- and 3 to 6 bold infill lines.
void printGrid(F32 H, F32 W, F32& layers, F32& rows)
{
    layers = llclamp(floorf(4.f + 3.f * log2f(1.f + llmax(H, 0.01f) / 0.25f) + 0.5f), 5.f, 16.f);
    const F32 lh = llmax(H, 0.01f) / layers;
    rows = llclamp(floorf(W / (2.5f * lh) + 0.5f), 3.f, 6.f);
}

// ---- GLSL replicas (gmdx_rezfx.glsl), for the debris to follow the shader's pieces ----
F32 fr(F32 x) { return x - floorf(x); }
F32 hash13(LLVector3 p)
{
    p.set(fr(p.mV[0] * 0.1031f), fr(p.mV[1] * 0.1031f), fr(p.mV[2] * 0.1031f));
    const F32 d = p.mV[0] * (p.mV[2] + 31.32f) + p.mV[1] * (p.mV[1] + 31.32f) + p.mV[2] * (p.mV[0] + 31.32f);
    p += LLVector3(d, d, d);
    return fr((p.mV[0] + p.mV[1]) * p.mV[2]);
}
LLVector3 hash33(LLVector3 p)
{
    p.set(fr(p.mV[0] * 0.1031f), fr(p.mV[1] * 0.1030f), fr(p.mV[2] * 0.0973f));
    const F32 d = p.mV[0] * (p.mV[1] + 33.33f) + p.mV[1] * (p.mV[0] + 33.33f) + p.mV[2] * (p.mV[2] + 33.33f);
    p += LLVector3(d, d, d);
    return LLVector3(fr((p.mV[0] + p.mV[1]) * p.mV[2]), fr((p.mV[0] + p.mV[0]) * p.mV[1]), fr((p.mV[1] + p.mV[0]) * p.mV[0]));
}
LLVector3 floor3(const LLVector3& v) { return LLVector3(floorf(v.mV[0]), floorf(v.mV[1]), floorf(v.mV[2])); }
F32 noise3(const LLVector3& p)
{
    const LLVector3 i = floor3(p);
    LLVector3 f = p - i;
    for (S32 k = 0; k < 3; ++k) f.mV[k] = f.mV[k] * f.mV[k] * (3.f - 2.f * f.mV[k]);
    auto h = [&](F32 x, F32 y, F32 z) { return hash13(i + LLVector3(x, y, z)); };
    auto mix = [](F32 a, F32 b, F32 t) { return a + (b - a) * t; };
    return mix(mix(mix(h(0, 0, 0), h(1, 0, 0), f.mV[0]), mix(h(0, 1, 0), h(1, 1, 0), f.mV[0]), f.mV[1]),
               mix(mix(h(0, 0, 1), h(1, 0, 1), f.mV[0]), mix(h(0, 1, 1), h(1, 1, 1), f.mV[0]), f.mV[1]), f.mV[2]);
}
F32 burnNoise(const LLVector3& p)
{
    return llclamp((0.65f * noise3(p * 2.5f) + 0.35f * noise3(p * 7.f) - 0.2f) / 0.6f, 0.f, 1.f);
}
// rzCubeTimeAt
constexpr F32 kCubeOfs = 0.371f;   // RZ_CUBE_OFS
F32 cubeTimeAt(const LLVector3& q, F32 radius, F32 sz, LLVector3& cell, F32& vs)
{
    vs = llclamp(radius * 0.09f, 0.03f, 1.5f);
    cell = floor3(q * (1.f / vs) + LLVector3(kCubeOfs, kCubeOfs, kCubeOfs));
    const F32 hz = llclamp(((cell.mV[2] + 0.5f - kCubeOfs) * vs) / llmax(sz, 0.01f) + 0.5f, 0.f, 1.f);
    return 0.1f + 0.5f * (1.f - hz) + 0.18f * hash13(cell + LLVector3(5.f, 5.f, 5.f));
}
// rzShardOf / rzShardTime
LLVector3 shardOf(const LLVector3& q, F32 cs, LLVector3& id)
{
    const LLVector3 g = floor3(q * (1.f / cs));
    F32 d1 = 1e30f;
    LLVector3 s1;
    id = g;
    for (S32 z = -1; z <= 1; ++z)
        for (S32 y = -1; y <= 1; ++y)
            for (S32 x = -1; x <= 1; ++x)
            {
                const LLVector3 c = g + LLVector3((F32)x, (F32)y, (F32)z);
                const LLVector3 h = hash33(c + LLVector3(3.f, 3.f, 3.f));
                const LLVector3 sd = (c + LLVector3(0.15f, 0.15f, 0.15f) + h * 0.7f) * cs;
                const F32 d = dist_vec_squared(q, sd);
                if (d < d1) { d1 = d; s1 = sd; id = c; }
            }
    return s1;
}
// SHATTER TUNABLES. The shader's (gmdx_rezfx.glsl RZ_*, the same names): change both.
constexpr F32 kShCrackEnd = 0.1f;      // RZ_CRACK_END: the cracks reach the far side (progress)
constexpr F32 kShLooseLag = 0.03f;     // RZ_LOOSE_LAG: a shard breaks loose this long after...
constexpr F32 kShLooseJitter = 0.05f;  // RZ_LOOSE_JITTER: ...plus up to this, at random
constexpr F32 kShShardFrac = 0.3f;     // RZ_SHARD_FRAC: shard size, of the bounding radius
constexpr F32 kShBurstBase = 2.f;      // RZ_BURST_BASE / PER_M / MAX: burst speed at the impact (m/s)
constexpr F32 kShBurstPerM = 2.2f;
constexpr F32 kShBurstMax = 14.f;
constexpr F32 kShBurstFar = 0.25f;     // RZ_BURST_FAR: the far side's share of it
constexpr F32 kShBurstDrag = 2.f;      // RZ_BURST_DRAG (1/s)
constexpr F32 kShSpall = 0.25f;        // RZ_SPALL: share of shards thrown back toward the camera
constexpr F32 kShSpinMin = 6.f;        // RZ_SPIN_MIN / RANGE (rad/s)
constexpr F32 kShSpinRange = 18.f;
constexpr F32 kShShrinkStart = 0.7f;   // RZ_SHRINK_START
constexpr F32 kShFlashDecay = 0.03f;   // RZ_FLASH_DECAY: the impact flash (progress for 1/e)
constexpr F32 kShFlashSize = 0.35f;    // RZ_FLASH_SIZE: its radius, of the bounding radius
// The debris's own (renderDebris): the impact burst's particles, from the overall debris cap;
// every fifth a glass chip, every fifth a puff of glass dust, the rest sparks.
constexpr F32 kShParticles = 10.f;     // particles at least...
constexpr F32 kShParticlesPerM = 8.f;  // ...plus this many per metre of radius...
constexpr F32 kShParticlesMax = 40.f;  // ...at most
constexpr F32 kShSparkSpeed = 7.f;     // m/s (x 0.6..3 by size), fast and short-lived
constexpr F32 kShSparkLife = 0.45f;    // s, at most
constexpr F32 kShChipBoost = 1.8f;     // a chip flies this much faster than the shards at the impact...
constexpr F32 kShChipDrag = 0.8f;      // ...and slows less (1/s): faster and farther
constexpr F32 kShChipSize = 0.3f;      // of a shard
constexpr F32 kShDustSize = 0.12f;     // a dust puff's radius, of the bounding radius (it grows 2x)
constexpr F32 kShDustAlpha = 0.35f;    // its opacity
constexpr F32 kShGlint = 1.5f;         // the overlay glint on a spinning shard / chip (display colour)
F32 shardSize(F32 radius) { return llclamp(radius * kShShardFrac, 0.05f, 3.f); }
F32 shardTime(const LLVector3& sd, const LLVector3& id, const LLVector3& impact, F32 radius)
{
    return kShCrackEnd * llclamp(dist_vec(sd, impact) / llmax(radius + impact.length(), 1e-3f), 0.f, 1.f)
         + kShLooseLag + kShLooseJitter * hash13(id + LLVector3(29.f, 29.f, 29.f));
}
// rzImpactDist: from the centre along dir to the object's box (scale, rot: its pose).
F32 impactDist(const LLVector3& dir, const LLVector3& scale, const LLQuaternion& rot)
{
    F32 t = 1e30f;
    for (S32 i = 0; i < 3; ++i)
    {
        LLVector3 ax;
        ax.mV[i] = 1.f;
        t = llmin(t, 0.5f * scale.mV[i] / llmax(fabsf((ax * rot) * dir), 1e-6f));
    }
    return t;
}
F32 burstSpeed(F32 radius) { return llclamp(kShBurstBase + kShBurstPerM * radius, kShBurstBase, kShBurstMax); }
// rzShardLaunch / rzShardFlight
LLVector3 shardLaunch(const LLVector3& a, const LLVector3& n, const LLVector3& r3, F32 norm, F32 radius, F32& spin)
{
    const F32 dn = llclamp(a.length() / llmax(norm, 1e-3f), 0.f, 1.f);
    const F32 f = kShBurstFar + (1.f - kShBurstFar) * (1.f - dn) * (1.f - dn);
    LLVector3 lat = a - n * (a * n);
    lat *= 1.f / llmax(lat.length(), 1e-4f);
    LLVector3 d = lat + n * (r3.mV[1] < kShSpall ? 0.5f : -0.6f) + LLVector3(0.f, 0.f, 0.15f);
    d.normVec();
    spin = (kShSpinMin + kShSpinRange * r3.mV[2]) * (0.4f + 0.6f * f);
    return d * (burstSpeed(radius) * f * (0.6f + 0.8f * r3.mV[0]));
}
LLVector3 flight(const LLVector3& v, F32 dt, F32 drag)
{
    return v * ((1.f - expf(-drag * dt)) / drag) + LLVector3(0.f, 0.f, -4.9f * dt * dt);
}

// Points on a dying object's surface (its volume's triangles), for its debris: the de-rezzes with
// pieces or flakes, prims and meshes not worn, while the overall cap allows.
void sampleDebris(LLViewerObject* obj, Obj& o)
{
    if (o.effect != FX_ASH && o.effect != FX_DISINTEGRATE && o.effect != FX_SHATTER) return;
    if (sCombatSafe || isAvatarOrWorn(obj) || sDebris >= kMaxDebris) return;
    const LLVolume* vol = obj->getVolume();
    const S32 nf = vol ? vol->getNumVolumeFaces() : 0;
    if (nf <= 0) return;
    const F32 radius = 0.5f * o.scale.length();
    size_t want = (size_t)llclamp(12.f + radius * 8.f, 12.f, (F32)kDebrisPerObj);
    want = llmin(want, kMaxDebris - sDebris);
    // Disintegrate / shatter: one point per piece (a trail / a glint each).
    std::unordered_set<U64> pieces;
    auto key = [](const LLVector3& c) { return ((U64)(U32)(S32)c.mV[0] << 42) ^ ((U64)(U32)(S32)c.mV[1] << 21) ^ (U64)(U32)(S32)c.mV[2]; };
    const LLVector3 n = sSlots[o.slot].dir;
    const LLVector3 impact = o.impact;
    for (size_t tries = 0; tries < want * 3 && o.bits.size() < want; ++tries)
    {
        const LLVolumeFace& f = vol->getVolumeFace(ll_rand(nf));
        if (f.mNumIndices < 3 || f.mNumVertices < 3 || !f.mPositions || !f.mIndices) continue;
        const S32 tri = ll_rand(f.mNumIndices / 3);
        const U16 i0 = f.mIndices[tri * 3], i1 = f.mIndices[tri * 3 + 1], i2 = f.mIndices[tri * 3 + 2];
        if (i0 >= f.mNumVertices || i1 >= f.mNumVertices || i2 >= f.mNumVertices) continue;
        F32 a = ll_frand(), b = ll_frand();
        if (a + b > 1.f) { a = 1.f - a; b = 1.f - b; }
        const LLVector3 v0(f.mPositions[i0].getF32ptr()), v1(f.mPositions[i1].getF32ptr()), v2(f.mPositions[i2].getF32ptr());
        const LLVector3 q = (v0 + (v1 - v0) * a + (v2 - v0) * b).scaledVec(o.scale);   // object space (m)
        Bit bt;
        bt.q = q;
        bt.w = o.pos + q * o.rot;
        bt.r = ll_frand();
        if (o.effect == FX_DISINTEGRATE)
        {
            LLVector3 cell;
            F32 vs;
            bt.t0 = cubeTimeAt(q, radius, o.scale.mV[VZ], cell, vs);
            if (!pieces.insert(key(cell)).second) continue;
            bt.c = (cell + LLVector3(0.5f - kCubeOfs, 0.5f - kCubeOfs, 0.5f - kCubeOfs)) * vs;
            bt.sp = hash33(cell + LLVector3(19.f, 19.f, 19.f)).mV[1];   // rezfxDisplace r3.y
        }
        else if (o.effect == FX_SHATTER)
        {
            bt.q = q * o.rot;   // world-oriented, as the shader's shards
            LLVector3 id;
            bt.c = shardOf(bt.q, shardSize(radius), id);
            if (!pieces.insert(key(id)).second) continue;
            bt.t0 = shardTime(bt.c, id, impact, radius);
            bt.r = hash13(id + LLVector3(43.f, 43.f, 43.f));
            bt.v = shardLaunch(bt.c - impact, n, hash33(id + LLVector3(37.f, 37.f, 37.f)), radius + impact.length(), radius, bt.sp);
        }
        o.bits.push_back(bt);
    }
    sDebris += o.bits.size();
    if (o.effect == FX_SHATTER)
    {
        // the impact burst: sparks, chips and dust (renderDebris)
        o.burst = (U32)llmin((size_t)llclamp(kShParticles + kShParticlesPerM * radius, kShParticles, kShParticlesMax), kMaxDebris - sDebris);
        o.seed = floorf(ll_frand() * 997.f);
        sDebris += o.burst;
    }
}

// Starts an effect on an object that has rows. False: no slot / no rows (nothing changed).
// pose: a killed linkset child's pose while it was linked (its debris), else its own.
bool start(LLViewerObject* obj, U32 effect, bool dying, const LinkKill* pose = nullptr)
{
    const LLUUID& id = obj->getID();
    const F32 d = duration();
    // The build effects may hide what is not built yet, except in combat-safe (they then stay
    // solid throughout, paint only, at the combat-safe length).
    const bool build = !dying && (effect == FX_PRINTER || effect == FX_TRANSPORTER);
    const bool hide = build && !sCombatSafe;
    Obj o;
    o.effect = effect;
    o.stage = 0;
    o.start = sNow;
    o.len = effect == FX_TRON ? d * kWireBuild : d;
    if (hide) o.len = effect == FX_PRINTER ? llmin(d * kPrinterLength, llmax(d, kPrinterMax))
                                           : llmin(d * kTransporterLength, llmax(d, 6.f));
    if (dying) o.len = llmin(d * deRezScale(effect), kDeRezMax);
    if (sCombatSafe) o.len = llmin(o.len, kCombatSafeMax);
    o.wait = sCombatSafe ? 0.f : kTextureWait;
    o.dying = dying;
    // <GMDX> GMDXRezFxWaitForTextures (kWaitMax): the first part holds for the textures. Tron's
    // wireframe waits in full instead of 2 s, its fill still done within kWaitMax.
    static LLCachedControl<bool> s_wait(gSavedSettings, "GMDXRezFxWaitForTextures", false);
    const F32 hold = (s_wait && !dying && !sCombatSafe) ? holdPoint(effect) : -1.f;
    o.waitTex = s_wait && !dying && !sCombatSafe && effect == FX_TRON;
    if (o.waitTex) o.wait = llmax(kWaitMax - o.len - d, 0.f);
    // </GMDX>
    if (hide && effect == FX_PRINTER)
    {
        // One slot per build: a prim arriving mid-print joins it where it is.
        o.slot = allocGroupSlot(groupOf(obj), o.len, hold);
        if (o.slot) { o.start = sSlots[o.slot].start; o.len = sSlots[o.slot].len; }
    }
    else
        o.slot = allocSlot(effect, 0, o.len, hide, hold, 0, o.waitTex);
    if (!o.slot) return false;
    if (effect == FX_SHATTER && sSlots[o.slot].refs == 1)
    {
        // The impact: the side facing the camera now.
        LLVector3 dir = LLViewerCamera::getInstance()->getOrigin() - (pose ? pose->pos : obj->getPositionAgent());
        if (dir.normVec() < 1e-3f) dir.set(0.f, 0.f, 1.f);
        sSlots[o.slot].dir = dir;
    }
    const U32 bits = (o.slot << SLOT_SHIFT) | (dying ? DYING_FLAG : 0u);
    if (!LLCleanInstanceStore::instance().setObjectRezFx(id, bits, ALL_FLAGS, dying))
    {
        derefSlot(o.slot);
        return false;
    }
    if (dying)
    {
        o.pos = pose ? pose->pos : obj->getPositionAgent();
        o.rot = pose ? pose->rot : obj->getRotationRegion();
        o.scale = obj->getScale();
        if (effect == FX_SHATTER)
        {
            const LLVector3& n = sSlots[o.slot].dir;
            o.impact = n * impactDist(n, o.scale, o.rot);
        }
        // (a worn linkset's child is already unparented here and no longer looks worn)
        if (!pose || pose->wearer.isNull()) sampleDebris(obj, o);
    }
    auto it = sObjs.find(id);
    if (it != sObjs.end())
    {
        derefSlot(it->second.slot);   // a rez-in cut short by the kill
        sDebris -= llmin(sDebris, debrisOf(it->second));
    }
    sObjs[id] = std::move(o);
    return true;
}

// The object leaves the table for good (its debris with it).
std::unordered_map<LLUUID, Obj>::iterator forgetObj(std::unordered_map<LLUUID, Obj>::iterator it)
{
    derefSlot(it->second.slot);
    sDebris -= llmin(sDebris, debrisOf(it->second));
    return sObjs.erase(it);
}

// The object's rows leave the effect (a rez-in that ended) and it forgets the object.
void finish(std::unordered_map<LLUUID, Obj>::iterator it)
{
    LLCleanInstanceStore::instance().setObjectRezFx(it->first, 0u, ALL_FLAGS);
    derefSlot(it->second.slot);
}

// A newly registered object: does it rez in, and how.
void considerRezIn(const LLUUID& id)
{
    auto it = sObjs.find(id);
    if (it != sObjs.end())
    {
        // Already in an effect (its rows were re-created): the new rows take its bits again.
        if (!it->second.dying)
            LLCleanInstanceStore::instance().setObjectRezFx(id, it->second.slot << SLOT_SHIFT, ALL_FLAGS);
        return;
    }
    if (!sSeen.insert(id).second) return;   // rows re-created (selection, LOD...): not a rez
    if (sObjs.size() >= kMaxObjects) return;
    LLViewerObject* obj = gObjectList.findObject(id);
    if (!obj || obj->isDead() || isTemporary(obj) || isHud(obj)) return;
    if (obj->getPCode() != LL_PCODE_VOLUME && !obj->isAvatar()) return;   // prims and avatars only
    const bool worn = isAvatarOrWorn(obj);
    // An avatar (its system body: rows keyed by its id) or something worn, rigged mesh included:
    // always, when avatars are in (they arrive as clouds, not white blobs).
    if (worn && !avatarsToo()) return;
    auto r = sRecent.find(id);
    if (r != sRecent.end() && sNow - r->second < kReplayGuard) return;
    // A prim of a build that is printing joins the print where it is, whatever it would pick and
    // loaded or not: one build, one print.
    if (!sGroupSlots.empty() && !sCombatSafe)
    {
        auto g = sGroupSlots.find(groupOf(obj));
        if (g != sGroupSlots.end() && sSlots[g->second].refs != 0)
        {
            if (start(obj, FX_PRINTER, false)) sRecent[id] = sNow;
            return;
        }
    }
    if (!worn)
    {
        static LLCachedControl<bool> s_loading(gSavedSettings, "GMDXRezFxOnlyLoading", true);
        if (s_loading && !texturesLoading(obj)) return;   // already textured: it just appears
    }
    const U32 effect = pickRezIn();
    if (effect != FX_NONE && start(obj, effect, false)) sRecent[id] = sNow;
}
}   // namespace

namespace GMDXRezFx
{
void setWireframe(bool on) { sWire = on; }
bool hiding() { return sHiding; }
bool wireframe() { return sWire; }

const Slot* table(U32& count, U32& gen)
{
    count = sHigh;
    gen = sGen;
    return sTable;
}

void update()
{
    static const bool s_init = []()
    {
        sFree.reserve(kHideSlotFirst);
        sFreeHide.reserve(kPrintSlotFirst - kHideSlotFirst);
        sFreePrint.reserve(MAX_SLOTS + 1 - kPrintSlotFirst);
        for (U32 i = MAX_SLOTS; i >= 1; --i)   // pop_back hands out the lowest first
            poolOf(i).push_back(i);
        return true;
    }();
    (void)s_init;
    ++sFrame;
    sNow = LLFrameTimer::getTotalSeconds();
    sLastUpdate = sNow;
    if (!sLinkset.empty()) sLinkset.clear();
    for (size_t i = 0; i < sResting.size();)
    {
        if (sFrame - sResting[i].second >= kSlotRest)
        {
            poolOf(sResting[i].first).push_back(sResting[i].first);
            sResting[i] = sResting.back();
            sResting.pop_back();
        }
        else ++i;
    }

    refreshCombatSafe();
    // New objects (their first mesh registered since the last frame).
    const bool collect = pickRezIn(false) != FX_NONE;
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    if (collect != sCollect)
    {
        store.setRezFxCollect(collect);
        sCollect = collect;
    }
    if (store.rezFxNewPending())
    {
        store.takeRezFxNew(sNewIds);
        for (const LLUUID& id : sNewIds) considerRezIn(id);
        sNewIds.clear();
    }
    if (!sRecent.empty() && sNow - sRecentPurge > 2.0)
    {
        sRecentPurge = sNow;
        for (auto it = sRecent.begin(); it != sRecent.end();)
            it = (sNow - it->second >= kReplayGuard) ? sRecent.erase(it) : std::next(it);
    }
    if (sObjs.empty()) { sHiding = false; return; }   // (the table keeps its last rows: no row names them)

    // Stages and ends.
    for (auto it = sObjs.begin(); it != sObjs.end();)
    {
        Obj& o = it->second;
        const F64 t = sNow - o.start;
        if (o.dying)
        {
            if (o.ended)
            {
                store.releaseDyingRows(it->first);
                it = forgetObj(it);
                continue;
            }
            if (t >= o.len) o.ended = true;   // one more frame, drawn at progress 1
            ++it;
            continue;
        }
        if (o.effect == FX_TRON && o.stage == 0)
        {
            if (t >= o.len)
            {
                // The wireframe is complete: the fill starts once the textures are in (or it
                // waited long enough, or the object is gone).
                LLViewerObject* obj = gObjectList.findObject(it->first);
                // <GMDX> waiting for textures in full (stillLoading, looked at kWaitCheck apart);
                // combat-safe land lets go at once.
                bool loading = false;
                if (obj && !obj->isDead() && o.waitTex)
                {
                    loading = !sCombatSafe && (sNow < o.nextCheck || stillLoading(obj));
                    if (loading && sNow >= o.nextCheck) o.nextCheck = sNow + kWaitCheck;
                }
                else if (obj && !obj->isDead()) loading = texturesLoading(obj);
                // </GMDX>
                if (!obj || obj->isDead() || t >= o.len + o.wait || !loading)
                {
                    const U32 s = allocSlot(FX_TRON, 1, duration());
                    if (!s)
                    {
                        finish(it);
                        it = sObjs.erase(it);
                        continue;
                    }
                    store.setObjectRezFx(it->first, s << SLOT_SHIFT, ALL_FLAGS);
                    derefSlot(o.slot);
                    o.slot = s;
                    o.stage = 1;
                    o.start = sNow;
                    o.len = duration();
                }
            }
            ++it;
            continue;
        }
        // (the rest go by their slot's clock: a printer build's slot moves on in place)
        const SlotState& ss = sSlots[o.slot];
        const F64 ts = sNow - ss.start;
        if (ss.p1 < 1.f)
        {
            // <GMDX> GMDXRezFxWaitForTextures: the first part, held at its end while the object
            // still loads -- never past kWaitMax from its start (minus the final part), never on
            // combat-safe land, not for an object that is gone.
            if (ts >= ss.len)
            {
                LLViewerObject* obj = gObjectList.findObject(it->first);
                const bool wait = obj && !obj->isDead() && !sCombatSafe && sNow < ss.tbase + kWaitMax - ss.rest
                                  && (sNow < o.nextCheck || stillLoading(obj));
                if (wait && sNow >= o.nextCheck) o.nextCheck = sNow + kWaitCheck;
                if (ss.group.notNull())
                {
                    if (wait) sSlots[o.slot].waitFrame = sFrame;   // a build holds as one (below)
                }
                else if (!wait)
                {
                    const U32 s = allocSlot(o.effect, 0, ss.rest, ss.hide, -1.f, o.slot);
                    if (!s)
                    {
                        finish(it);
                        it = sObjs.erase(it);
                        continue;
                    }
                    store.setObjectRezFx(it->first, s << SLOT_SHIFT, ALL_FLAGS);
                    derefSlot(o.slot);
                    o.slot = s;
                    o.start = sNow;
                    o.len = sSlots[s].len;
                }
            }
            ++it;
            continue;
        }
        if (o.ended)
        {
            finish(it);
            it = sObjs.erase(it);
            continue;
        }
        if (ts >= ss.len) o.ended = true;   // one more frame, drawn at progress 1
        ++it;
    }
    // <GMDX> A printer build that held for its textures prints its final layers once none of its
    // prims waits any more: its slot moves on in place (one print, one nozzle).
    for (U32 s = kPrintSlotFirst; s <= MAX_SLOTS; ++s)
    {
        SlotState& st = sSlots[s];
        if (st.refs == 0 || st.p1 >= 1.f || sNow - st.start < st.len || st.waitFrame == sFrame) continue;
        st.start = sNow;
        st.len = llmax(st.rest, 0.01f);
        st.p0 = st.p1;
        st.p1 = 1.f;
        st.rest = 0.f;
    }

    // The table: every live slot's progress this frame.
    static LLCachedControl<LLColor4> s_col[15] = {
        {gSavedSettings, "GMDXRezFxColorTron", LLColor4::white},  // (0 unused)
        {gSavedSettings, "GMDXRezFxColorTron", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorFire", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorHologram", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorVoxel", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorPrinter", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorTransporter", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorTron", LLColor4::white},  // (7 unused)
        {gSavedSettings, "GMDXRezFxColorEmber", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorDisintegrate", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorAsh", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorGlitch", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorImplode", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorTronOut", LLColor4::white},
        {gSavedSettings, "GMDXRezFxColorShatter", LLColor4::white}};
    // Ash drifts with the region's wind where the camera is (LLWind, gWindVec); in still air a
    // fixed light breeze.
    LLVector3 wind = gWindVec;
    F32 speed = wind.normVec();
    if (speed < 0.5f) { wind.set(0.8f, 0.6f, 0.f); speed = 2.f; }
    wind.mV[VZ] += 0.25f;   // ash rises a little as it goes
    wind.normVec();
    const F32 strength = llclamp(speed / 8.f, 0.35f, 1.f);
    sHigh = 0;
    sHiding = false;
    F32 storeD[3] = {0.f, 0.f, 0.f};   // the draw frame = agent + d (the printer's box)
    if (!sGroupSlots.empty()) store.renderOffset(storeD);
    for (U32 s = 1; s <= MAX_SLOTS; ++s)
    {
        Slot& row = sTable[s - 1];
        const SlotState& st = sSlots[s];
        if (st.refs == 0)
        {
            row.effect = FX_NONE;
            continue;
        }
        row.effect = st.effect;
        row.stage = st.stage;
        sHiding |= st.hide;
        // <GMDX> (tbase / p0 / p1: the same as before unless the rez-in waits for textures)
        row.time = (F32)(sNow - st.tbase);
        const F32 x = llclamp((F32)(sNow - st.start) / st.len, 0.f, 1.f);
        row.progress = st.p0 + (st.p1 - st.p0) * x;
        const LLColor4 c = linearColor4(s_col[llmin(st.effect, 14u)]);
        row.colour[0] = c.mV[VRED]; row.colour[1] = c.mV[VGREEN]; row.colour[2] = c.mV[VBLUE]; row.colour[3] = 1.f;
        // Only ash has wind; every other effect: wind.z = its length (s).
        if (st.effect == FX_ASH) { row.wind[0] = wind.mV[VX]; row.wind[1] = wind.mV[VY]; row.wind[2] = wind.mV[VZ]; row.wind[3] = strength; }
        else { row.wind[0] = 0.f; row.wind[1] = 0.f; row.wind[2] = st.len; row.wind[3] = 0.f; }
        for (S32 k = 0; k < 4; ++k) { row.aux0[k] = 0.f; row.aux1[k] = 0.f; }
        if (st.effect == FX_TRON || st.effect == FX_TRON_OUT)
        {
            // Tron, rez-in and de-rez: colour.a = its glow strength, wind.x = its line width (px).
            static LLCachedControl<F32> s_glow(gSavedSettings, "GMDXRezFxTronGlow", 1.f);
            static LLCachedControl<F32> s_width(gSavedSettings, "GMDXRezFxTronLineWidth", 1.5f);
            row.colour[3] = llclamp((F32)s_glow, 0.f, 4.f);
            row.wind[0] = llclamp((F32)s_width, 0.5f, 5.f);
        }
        else if (st.effect == FX_SHATTER)
        {
            row.aux0[0] = st.dir.mV[VX]; row.aux0[1] = st.dir.mV[VY]; row.aux0[2] = st.dir.mV[VZ];
        }
        else if (st.effect == FX_PRINTER || st.effect == FX_TRANSPORTER)
        {
            // The build effects: x = may hide now (a hidden-mode slot, and not on combat-safe land
            // this frame: walking onto it shows the object whole at once), y = the printer's layer
            // count; the printer's build box (its own slot: one per linkset / avatar) in aux.
            row.wind[0] = (st.hide && !sCombatSafe) ? 1.f : 0.f;
            row.wind[1] = 10.f;
            row.aux1[3] = 4.f;
            SlotState& ss = sSlots[s];
            if (ss.group.notNull())
            {
                LLVector3 lo, hi;
                if (groupBounds(ss.group, lo, hi)) { ss.lo = lo; ss.hi = hi; ss.boxOk = true; }
                if (ss.boxOk)
                {
                    const LLVector3 ctr = (ss.lo + ss.hi) * 0.5f, half = (ss.hi - ss.lo) * 0.5f;
                    printGrid(2.f * half.mV[VZ], 2.f * llmax(half.mV[VX], half.mV[VY]), row.wind[1], row.aux1[3]);
                    for (S32 k = 0; k < 3; ++k) { row.aux0[k] = ctr.mV[k] + storeD[k]; row.aux1[k] = llmax(half.mV[k], 0.005f); }
                    row.aux0[3] = 1.f;
                }
            }
        }
        // <GMDX> Waiting for textures: wind.w = the hold's idle loop, easing in once the first
        // part has played (Tron's wireframe, voxel's blocks); the printer: keep-warm, 1 until its
        // final layers and cooling off across them.
        if (st.wait && st.effect < DEREZ_FIRST)
        {
            if (st.effect == FX_PRINTER) row.wind[3] = st.p1 < 1.f ? 1.f : 1.f - smooth01(x);
            else if (st.p1 < 1.f || (st.effect == FX_TRON && st.stage == 0))
                row.wind[3] = smooth01(((F32)(sNow - st.start) - st.len) / 0.5f);
            else row.wind[3] = st.fade * (1.f - smooth01((F32)(sNow - st.start) / 0.3f));
        }
        sHigh = s;
    }
    ++sGen;
}

// THE TRANSPORTER'S BEAM. The effect shader paints only an object's own surface; the column of
// light around it is extra geometry, drawn here in the world-overlay segment (llappviewer.cpp:
// gGL against the scene depth, after the world is lit) -- additive, depth-tested, no depth write,
// so the object hides the back of the column and the front of it glows over the object. Per
// object (a linkset once, at its root; an avatar and what it wears once, around the avatar):
//  - a soft cylindrical shell a little wider than the bounds, bright at its silhouette and its
//    base, fading out above the top;
//  - shimmering vertical streaks of light turning around it;
//  - glints spiralling up around and through the object, thickest at the start, gone by the
//    moment it is solid;
//  - all of it flares with the surface flash as the object becomes solid, then fades out.
// Hidden mode only (the full look): in combat-safe the transporter is paint alone. Nothing runs
// while no transporter does.
static void renderBeams()
{
    if (sCombatSafe) return;
    struct Beam { LLUUID key; LLVector3 lo, hi; F32 p, t; };
    static std::vector<Beam> beams;
    static std::unordered_set<LLUUID> keys;
    beams.clear();
    keys.clear();
    for (const auto& kv : sObjs)
    {
        const Obj& o = kv.second;
        if (o.dying || o.effect != FX_TRANSPORTER || o.slot < kHideSlotFirst || o.slot > MAX_SLOTS) continue;
        LLViewerObject* obj = gObjectList.findObject(kv.first);
        if (!obj || obj->isDead()) continue;
        Beam b;
        LLVOAvatar* av = obj->isAvatar() ? static_cast<LLVOAvatar*>(obj) : (isAvatarOrWorn(obj) ? obj->getAvatar() : nullptr);
        LLViewerObject* root = av ? nullptr : obj->getRootEdit();
        b.key = av ? av->getID() : (root ? root->getID() : obj->getID());
        if (!keys.insert(b.key).second) continue;
        if (av)
        {
            if (av->isDead()) continue;
            const LLVector3 pos = av->getPositionAgent();
            const F32 foot = pos.mV[VZ] - av->getPelvisToFoot();
            const F32 tall = av->mBodySize.mV[VZ] > 0.5f ? av->mBodySize.mV[VZ] : 1.9f;
            b.lo.set(pos.mV[VX] - 0.3f, pos.mV[VY] - 0.3f, foot);
            b.hi.set(pos.mV[VX] + 0.3f, pos.mV[VY] + 0.3f, foot + tall);
        }
        else
        {
            if (!root) root = obj;
            const LLBBox rb = root->getBoundingBoxAgent().getAxisAligned();
            b.lo = rb.getMinAgent();
            b.hi = rb.getMaxAgent();
            for (const LLViewerObject* child : root->getChildren())
            {
                if (!child || child->isAvatar()) continue;
                const LLBBox cb = child->getBoundingBoxAgent().getAxisAligned();
                b.lo.set(llmin(b.lo.mV[VX], cb.getMinAgent().mV[VX]), llmin(b.lo.mV[VY], cb.getMinAgent().mV[VY]),
                         llmin(b.lo.mV[VZ], cb.getMinAgent().mV[VZ]));
                b.hi.set(llmax(b.hi.mV[VX], cb.getMaxAgent().mV[VX]), llmax(b.hi.mV[VY], cb.getMaxAgent().mV[VY]),
                         llmax(b.hi.mV[VZ], cb.getMaxAgent().mV[VZ]));
            }
        }
        const Slot& row = sTable[o.slot - 1];
        b.p = row.progress;
        b.t = row.time;
        beams.push_back(b);
    }
    if (beams.empty()) return;
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin(), left = cam->getLeftAxis(), up = cam->getUpAxis();
    // A whole region beaming in at login: only the nearest kMaxBeams get a column (the rest still
    // shimmer in on their surface). Each is a few thousand immediate-mode vertices at most.
    constexpr size_t kMaxBeams = 24;
    if (beams.size() > kMaxBeams)
    {
        auto dist = [&](const Beam& x) { return dist_vec_squared((x.lo + x.hi) * 0.5f, eye); };
        std::nth_element(beams.begin(), beams.begin() + kMaxBeams, beams.end(),
                         [&](const Beam& a, const Beam& b) { return dist(a) < dist(b); });
        beams.resize(kMaxBeams);
    }

    static LLCachedControl<LLColor4> s_col(gSavedSettings, "GMDXRezFxColorTransporter", LLColor4::white);
    const LLColor4 col = s_col;   // (display colour: the overlay draws after the world is tonemapped)
    constexpr F32 kSolid = 0.7f;   // gmdx_rezfx.glsl RZ_TRANS_SOLID: the flash
    constexpr F32 kTau = 6.2831853f;
    auto smooth = [](F32 e0, F32 e1, F32 x) { const F32 k = llclamp((x - e0) / (e1 - e0), 0.f, 1.f); return k * k * (3.f - 2.f * k); };
    auto hash = [](U32 i, U32 k) { const F32 v = sinf((F32)i * 12.9898f + (F32)k * 78.233f) * 43758.5453f; return v - floorf(v); };
    auto vtx = [&](const LLVector3& v, F32 k) { gGL.color4f(col.mV[VRED] * k, col.mV[VGREEN] * k, col.mV[VBLUE] * k, 1.f); gGL.vertex3fv(v.mV); };
    auto vtxw = [&](const LLVector3& v, F32 k, F32 w)   // whiter (glints)
    {
        gGL.color4f((col.mV[VRED] * (1.f - w) + w) * k, (col.mV[VGREEN] * (1.f - w) + w) * k, (col.mV[VBLUE] * (1.f - w) + w) * k, 1.f);
        gGL.vertex3fv(v.mV);
    };

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    for (const Beam& b : beams)
    {
        const LLVector3 ctr = (b.lo + b.hi) * 0.5f;
        const F32 ex = 0.5f * (b.hi.mV[VX] - b.lo.mV[VX]), ey = 0.5f * (b.hi.mV[VY] - b.lo.mV[VY]);
        const F32 tall = llmax(b.hi.mV[VZ] - b.lo.mV[VZ], 0.05f);
        const F32 size = llmax(tall, 2.f * llmax(ex, ey));
        const F32 rb = 1.15f * llmax(ex, ey) + 0.05f;               // the column's radius
        const F32 z0 = b.lo.mV[VZ] - 0.02f, z1 = b.hi.mV[VZ] + 0.2f * tall + 0.1f;
        const F32 p = b.p, t = b.t;
        // Strength: up fast, holds, flares with the surface flash, gone at the end.
        const F32 flash = expf(-powf((p - kSolid) / 0.04f, 2.f));
        const F32 in = smooth(0.f, 0.1f, p) * (1.f - smooth(kSolid, 1.f, p)) + 2.f * flash;
        if (in <= 0.001f) continue;
        // To the camera, flattened (the shell's silhouette).
        LLVector3 tc = eye - ctr;
        tc.mV[VZ] = 0.f;
        if (tc.normVec() < 1e-4f) tc.set(1.f, 0.f, 0.f);

        // 1. The shell: 24 segments x 3 bands; bright at the silhouette and the base.
        static const F32 kRingZ[4] = {0.f, 0.12f, 0.6f, 1.f};
        static const F32 kRingK[4] = {1.f, 0.7f, 0.45f, 0.f};
        gGL.begin(LLRender::TRIANGLES);
        constexpr S32 kSeg = 24;
        for (S32 i = 0; i < kSeg; ++i)
        {
            const F32 a0 = kTau * (F32)i / (F32)kSeg, a1 = kTau * (F32)(i + 1) / (F32)kSeg;
            const LLVector3 n0(cosf(a0), sinf(a0), 0.f), n1(cosf(a1), sinf(a1), 0.f);
            auto rim = [&](const LLVector3& n, F32 a)
            {
                const F32 f = 1.f - fabsf(n * tc);
                return (0.12f + 0.88f * f * f * f) * (0.85f + 0.15f * sinf(t * 14.f + a * 3.f));
            };
            const F32 k0 = rim(n0, a0) * in * 0.4f, k1 = rim(n1, a1) * in * 0.4f;
            for (S32 r = 0; r < 3; ++r)
            {
                const F32 za = z0 + (z1 - z0) * kRingZ[r], zb = z0 + (z1 - z0) * kRingZ[r + 1];
                const LLVector3 p00 = ctr + n0 * rb, p10 = ctr + n1 * rb;
                const LLVector3 a(p00.mV[VX], p00.mV[VY], za), bb(p10.mV[VX], p10.mV[VY], za);
                const LLVector3 c(p10.mV[VX], p10.mV[VY], zb), d(p00.mV[VX], p00.mV[VY], zb);
                vtx(a, k0 * kRingK[r]); vtx(bb, k1 * kRingK[r]); vtx(c, k1 * kRingK[r + 1]);
                vtx(a, k0 * kRingK[r]); vtx(c, k1 * kRingK[r + 1]); vtx(d, k0 * kRingK[r + 1]);
            }
        }
        // 2. Streaks: 7 thin vertical ribbons of light turning around the column, shimmering.
        for (S32 s = 0; s < 7; ++s)
        {
            const F32 a = kTau * (F32)s / 7.f + t * ((s & 1) ? 1.3f : -0.9f);
            const LLVector3 at = ctr + LLVector3(cosf(a), sinf(a), 0.f) * (rb * 0.97f);
            LLVector3 side = LLVector3(0.f, 0.f, 1.f) % (eye - at);
            if (side.normVec() < 1e-4f) continue;
            side *= 0.5f * (0.06f * rb + 0.01f);
            const F32 k = in * 0.3f * (0.5f + 0.5f * sinf(t * 7.f + (F32)s * 1.7f));
            const F32 zm = z0 + 0.4f * (z1 - z0);
            const LLVector3 b0(at.mV[VX], at.mV[VY], z0), bm(at.mV[VX], at.mV[VY], zm), bt(at.mV[VX], at.mV[VY], z1);
            // centre line bright, edges 0: two halves per section
            const F32 ks[3] = {0.6f * k, k, 0.f};
            const LLVector3 c3[3] = {b0, bm, bt};
            for (S32 q = 0; q < 2; ++q)
                for (F32 sgn : {-1.f, 1.f})
                {
                    vtx(c3[q], ks[q]); vtx(c3[q] + side * sgn, 0.f); vtx(c3[q + 1] + side * sgn, 0.f);
                    vtx(c3[q], ks[q]); vtx(c3[q + 1] + side * sgn, 0.f); vtx(c3[q + 1], ks[q + 1]);
                }
        }
        // 3. Glints spiralling up around and through the object, thinning out by the flash.
        const S32 total = llclamp((S32)(30.f + 10.f * size), 30, 160);
        const S32 count = (S32)((F32)total * powf(llmax(1.f - p / (kSolid + 0.05f), 0.f), 1.2f));
        const F32 gs = llclamp(0.02f * size, 0.015f, 0.25f);
        for (S32 i = 0; i < count; ++i)
        {
            const F32 r0 = hash(i, 1), r1 = hash(i, 2), r2 = hash(i, 3), r3 = hash(i, 4);
            const F32 hf = fmodf(r2 + t * (0.25f + 0.35f * r3), 1.f);
            const F32 a = kTau * r0 + t * (2.2f + 1.8f * r1);
            const F32 rad = rb * (0.25f + 0.8f * r3) * (1.f - 0.3f * hf);   // tightening as they climb
            const LLVector3 at = ctr + LLVector3(cosf(a) * rad, sinf(a) * rad, 0.f);
            const LLVector3 c(at.mV[VX], at.mV[VY], b.lo.mV[VZ] + hf * tall);
            const F32 k = in * sinf(hf * 3.14159f) * (0.55f + 0.45f * sinf(t * (18.f + 10.f * r0) + kTau * r2));
            if (k <= 0.01f) continue;
            const F32 sz = gs * (0.6f + 0.8f * r1);
            // a soft core and a thin cross (centre bright, tips 0)
            const LLVector3 ax[3][2] = {{left * (0.6f * sz), up * (0.6f * sz)},
                                         {left * (2.2f * sz), up * (0.18f * sz)},
                                         {up * (2.2f * sz), left * (0.18f * sz)}};
            for (S32 g = 0; g < 3; ++g)
            {
                const LLVector3 tips[4] = {c + ax[g][0], c + ax[g][1], c - ax[g][0], c - ax[g][1]};
                const F32 kc = k * (g == 0 ? 0.7f : 1.f);
                for (S32 e = 0; e < 4; ++e)
                {
                    vtxw(c, kc, 0.6f); vtxw(tips[e], 0.f, 0.f); vtxw(tips[(e + 1) & 3], 0.f, 0.f);
                }
            }
        }
        gGL.end();
    }
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

namespace
{
F32 smooth(F32 e0, F32 e1, F32 x) { const F32 k = llclamp((x - e0) / (e1 - e0), 0.f, 1.f); return k * k * (3.f - 2.f * k); }
// Flat-shaded solid triangles (the printer's head and gantry): lit by a fixed key light.
void solidTri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor3& base, F32 alpha)
{
    static const LLVector3 key = []() { LLVector3 k(0.4f, 0.3f, 1.f); k.normVec(); return k; }();
    LLVector3 n = (b - a) % (c - a);
    n.normVec();
    const F32 k = 0.45f + 0.55f * llmax(fabsf(n * key), 0.f) * (n.mV[VZ] >= -0.2f ? 1.f : 0.6f);
    gGL.color4f(base.mV[0] * k, base.mV[1] * k, base.mV[2] * k, alpha);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
}
void solidBox(const LLVector3& c, const LLVector3& h, const LLColor3& base, F32 alpha)
{
    LLVector3 v[8];
    for (S32 i = 0; i < 8; ++i)
        v[i].set(c.mV[0] + ((i & 1) ? h.mV[0] : -h.mV[0]), c.mV[1] + ((i & 2) ? h.mV[1] : -h.mV[1]), c.mV[2] + ((i & 4) ? h.mV[2] : -h.mV[2]));
    static const S32 f[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : f)
    {
        solidTri(v[q[0]], v[q[1]], v[q[2]], base, alpha);
        solidTri(v[q[0]], v[q[2]], v[q[3]], base, alpha);
    }
}
// A camera-facing soft disc: additive (add: centre col, rim black) or alpha-blended (centre alpha
// col.a, rim alpha 0).
void softDisc(const LLVector3& c, F32 r, const LLVector3& left, const LLVector3& up, const LLColor4& col, bool add = true)
{
    const LLColor4 rim = add ? LLColor4(0.f, 0.f, 0.f, 0.f) : LLColor4(col.mV[0], col.mV[1], col.mV[2], 0.f);
    constexpr S32 kN = 10;
    for (S32 i = 0; i < kN; ++i)
    {
        const F32 a0 = 6.2831853f * (F32)i / kN, a1 = 6.2831853f * (F32)(i + 1) / kN;
        const LLVector3 p0 = c + (left * cosf(a0) + up * sinf(a0)) * r, p1 = c + (left * cosf(a1) + up * sinf(a1)) * r;
        gGL.color4fv(col.mV); gGL.vertex3fv(c.mV);
        gGL.color4fv(rim.mV); gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV);
    }
}
// Additive: a thin cross of light (a glint), centre bright, tips 0.
void glint(const LLVector3& c, F32 r, const LLVector3& left, const LLVector3& up, const LLColor4& col)
{
    const LLColor4 z(0.f, 0.f, 0.f, 0.f);
    const LLVector3 arms[2][2] = {{left * r, up * (0.12f * r)}, {up * r, left * (0.12f * r)}};
    for (const auto& a : arms)
    {
        const LLVector3 t[4] = {c + a[0], c + a[1], c - a[0], c - a[1]};
        for (S32 e = 0; e < 4; ++e)
        {
            gGL.color4fv(col.mV); gGL.vertex3fv(c.mV);
            gGL.color4fv(z.mV); gGL.vertex3fv(t[e].mV); gGL.vertex3fv(t[(e + 1) & 3].mV);
        }
    }
}
// Additive: a ribbon from a (bright, k) to b (0) facing the camera, width w (a trail).
void trail(const LLVector3& a, const LLVector3& b, F32 w, const LLVector3& eye, const LLColor4& col)
{
    LLVector3 side = (b - a) % (eye - a);
    if (side.normVec() < 1e-6f) return;
    side *= 0.5f * w;
    const LLColor4 z(0.f, 0.f, 0.f, 0.f);
    gGL.color4fv(col.mV); gGL.vertex3fv((a + side).mV);
    gGL.color4fv(col.mV); gGL.vertex3fv((a - side).mV);
    gGL.color4fv(z.mV); gGL.vertex3fv(b.mV);
}
}   // namespace

// THE 3D PRINTER'S HEAD: per printing build (its own slot and box), a small print head at the
// nozzle position the shader prints from (rzPrintHead: the same raster): a brass nozzle with a
// white-hot tip over the layer being laid, a heater block and a fan housing above it, an X rail
// through it across the build and two Y rails along its sides -- a Cartesian gantry, rising with
// the print. Solid parts alpha-blended (opaque), the tip's glow additive. The nearest 24 builds.
static void renderPrintHeads(bool solid)
{
    if (sCombatSafe) return;
    struct Head { U32 s; F32 d; };
    static std::vector<Head> heads;
    heads.clear();
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin(), left = cam->getLeftAxis(), up = cam->getUpAxis();
    for (U32 s = kPrintSlotFirst; s <= MAX_SLOTS; ++s)
    {
        const SlotState& st = sSlots[s];
        if (st.refs == 0 || st.effect != FX_PRINTER || !st.boxOk || s > sHigh) continue;
        heads.push_back({s, dist_vec_squared((st.lo + st.hi) * 0.5f, eye)});
    }
    if (heads.empty()) return;
    constexpr size_t kMaxHeads = 24;
    if (heads.size() > kMaxHeads)
    {
        std::nth_element(heads.begin(), heads.begin() + kMaxHeads, heads.end(), [](const Head& a, const Head& b) { return a.d < b.d; });
        heads.resize(kMaxHeads);
    }
    static LLCachedControl<LLColor4> s_col(gSavedSettings, "GMDXRezFxColorPrinter", LLColor4::white);
    const LLColor4 col = s_col;
    gGL.begin(LLRender::TRIANGLES);
    for (const Head& hd : heads)
    {
        const SlotState& st = sSlots[hd.s];
        const Slot& row = sTable[hd.s - 1];
        const F32 p = row.progress;
        const F32 vis = smooth(0.f, 0.04f, p) * (1.f - smooth(0.96f, 1.f, p));
        if (vis <= 0.01f) continue;
        // rzPrintHead
        const F32 nl = llmax(1.f, floorf(row.wind[1] + 0.5f)), R = row.aux1[3] >= 1.f ? floorf(row.aux1[3] + 0.5f) : 4.f;
        const F32 L = llmin(floorf(p * nl), nl - 1.f);
        const F32 lf = llclamp(p * nl - L, 0.f, 1.f) * R;
        const F32 rw = llmin(floorf(lf), R - 1.f);
        const F32 along = fmodf(rw, 2.f) > 0.5f ? 1.f - (lf - rw) : lf - rw;
        F32 fx = along, fy = (rw + 0.5f) / R;
        if (fmodf(L, 2.f) > 0.5f) std::swap(fx, fy);
        const LLVector3 lo = st.lo, hi = st.hi;
        const F32 H = llmax(hi.mV[VZ] - lo.mV[VZ], 0.02f);
        const F32 W = llmax(hi.mV[VX] - lo.mV[VX], hi.mV[VY] - lo.mV[VY]);
        LLVector3 tip(lo.mV[VX] + fx * (hi.mV[VX] - lo.mV[VX]), lo.mV[VY] + fy * (hi.mV[VY] - lo.mV[VY]),
                      lo.mV[VZ] + (L + 1.f) * H / nl);
        const F32 sz = llclamp(0.11f * llmax(W, 0.5f * H), 0.04f, 1.f);   // the head's size
        // <GMDX> Holding for textures (GMDXRezFxWaitForTextures): the nozzle lifts a little off the
        // print and hovers there, its tip breathing.
        const F32 idle = st.p1 < 1.f ? smooth01(((F32)(sNow - st.start) - st.len) / 0.5f) : 0.f;
        const F32 bob = 0.5f + 0.5f * sinf(row.time * 2.4f);
        tip.mV[VZ] += idle * sz * (0.15f + 0.1f * bob);
        const F32 breathe = 1.f - idle * 0.4f * (1.f - bob);
        if (solid)
        {
            const F32 a = vis;
            // the nozzle: a hexagonal brass cone down to the tip
            const LLColor3 brass(0.72f, 0.52f, 0.22f), alu(0.62f, 0.63f, 0.66f), dark(0.1f, 0.1f, 0.11f), rail(0.28f, 0.29f, 0.31f);
            const F32 hn = 0.55f * sz, rn = 0.22f * sz;
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 a0 = 1.0471976f * i, a1 = 1.0471976f * (i + 1);
                const LLVector3 p0 = tip + LLVector3(cosf(a0) * rn, sinf(a0) * rn, hn), p1 = tip + LLVector3(cosf(a1) * rn, sinf(a1) * rn, hn);
                solidTri(tip, p1, p0, brass, a);
            }
            const LLVector3 blk = tip + LLVector3(0.f, 0.f, hn + 0.28f * sz);
            solidBox(blk, LLVector3(0.42f * sz, 0.32f * sz, 0.28f * sz), alu, a);                         // heater block
            solidBox(blk + LLVector3(0.f, 0.f, 0.62f * sz), LLVector3(0.5f * sz, 0.5f * sz, 0.34f * sz), dark, a);   // fan housing
            // the gantry, at the carriage's height: X rail through the head, Y rails at the sides
            const F32 rz = blk.mV[VZ] + 0.62f * sz, rt = 0.12f * sz, m = 0.08f * W + sz;
            const F32 x0 = lo.mV[VX] - m, x1 = hi.mV[VX] + m, y0 = lo.mV[VY] - m, y1 = hi.mV[VY] + m;
            solidBox(LLVector3(0.5f * (x0 + x1), tip.mV[VY] + 0.6f * sz, rz), LLVector3(0.5f * (x1 - x0), rt, rt), rail, a);
            solidBox(LLVector3(x0, 0.5f * (y0 + y1), rz), LLVector3(rt * 1.4f, 0.5f * (y1 - y0), rt * 1.4f), rail, a);
            solidBox(LLVector3(x1, 0.5f * (y0 + y1), rz), LLVector3(rt * 1.4f, 0.5f * (y1 - y0), rt * 1.4f), rail, a);
        }
        else
        {
            // the white-hot tip and its glow on the fresh bead
            const LLColor4 hot(0.5f + 0.5f * col.mV[0], 0.42f + 0.4f * col.mV[1], 0.3f + 0.3f * col.mV[2], 1.f);
            softDisc(tip, 0.9f * sz, left, up, hot * (vis * breathe));
            softDisc(tip, 0.25f * sz, left, up, LLColor4(1.f, 0.95f, 0.85f, 1.f) * (vis * breathe));
        }
    }
    gGL.end();
}

// DE-REZ DEBRIS: what flies off a dying object, from the points sampled on its surface at its death
// (sampleDebris). Every position is a closed form of the effect's progress, as the shader's pieces
// are: nothing is simulated or kept per frame.
//  - Ash: as the burn line passes a point (the shader's rzAshValue, same noise), a flake peels off:
//    it drifts downwind with the region wind, rising a little and fluttering, tumbles, glows hot,
//    cools through red to grey and fades; every third point also lets off a little grey smoke.
//  - Disintegrate: a short glow trail behind each cube that breaks loose (one per sampled cube,
//    flying as rezfxDisplace flies it).
//  - Shatter: at the impact point a flash (a white-hot disc and star, gone in about a tenth of a
//    second), a spray of sparks, a few glass chips that fly faster and farther than the shards
//    and a puff of glass dust; and a glint on each loose shard as it spins, in step with the
//    shader's (the shards' flight is rezfxDisplace's).
// solid: the alpha-blended pass (smoke, flakes), else the additive one (glows, trails, glints).
static void renderDebris(bool solid)
{
    if (sCombatSafe || sDebris == 0) return;
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin(), left = cam->getLeftAxis(), up = cam->getUpAxis();
    static LLCachedControl<LLColor4> s_ash(gSavedSettings, "GMDXRezFxColorAsh", LLColor4::white);
    static LLCachedControl<LLColor4> s_dis(gSavedSettings, "GMDXRezFxColorDisintegrate", LLColor4::white);
    static LLCachedControl<LLColor4> s_sha(gSavedSettings, "GMDXRezFxColorShatter", LLColor4::white);
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& kv : sObjs)
    {
        const Obj& o = kv.second;
        if (!o.dying || debrisOf(o) == 0 || o.slot == 0 || o.slot > MAX_SLOTS) continue;
        const Slot& row = sTable[o.slot - 1];
        const F32 p = row.progress, len = llmax(sSlots[o.slot].len, 0.05f);
        const F32 radius = 0.5f * o.scale.length();
        const F32 endk = 1.f - smooth(0.85f, 1.f, p);
        if (o.effect == FX_ASH)
        {
            const LLVector3 axis(row.wind[0], row.wind[1], row.wind[2]);
            const F32 strength = row.wind[3];
            F32 e = 0.f;
            for (S32 k = 0; k < 3; ++k)
            {
                LLVector3 ax; ax.mV[k] = o.scale.mV[k];
                e += fabsf((ax * o.rot) * axis);
            }
            e = llmax(0.5f * e, 1e-3f);
            const LLColor4 c = s_ash;
            const F32 fs0 = llclamp(radius * 0.05f, 0.015f, 0.35f);
            LLVector3 side = axis % LLVector3(0.f, 0.f, 1.f);
            if (side.normVec() < 1e-3f) side.set(1.f, 0.f, 0.f);
            for (size_t i = 0; i < o.bits.size(); ++i)
            {
                const Bit& b = o.bits[i];
                const F32 sweep = ((b.w - o.pos) * axis + e) / (2.f * e);
                const F32 v = 0.55f * sweep + 0.45f * burnNoise(b.q * 1.3f);
                const F32 tau = (p - (v + 0.05f) / 1.1f) * len;   // s since the burn line passed
                if (tau < 0.f) continue;
                const F32 spd = (1.f + 2.f * strength) * (0.7f + 0.6f * b.r);
                const LLVector3 drift = axis * (spd * tau) + LLVector3(0.f, 0.f, 0.35f * tau)
                                      + side * (0.12f * sinf(tau * (5.f + 4.f * b.r) + b.r * 9.f) * llmin(tau * 3.f, 1.f));
                if ((i % 3) == 0 && solid)
                {
                    // smoke: a soft grey puff, rising and spreading
                    const F32 life = 1.1f;
                    if (tau < life)
                    {
                        const LLVector3 at = b.w + axis * (0.6f * spd * tau) + LLVector3(0.f, 0.f, 0.7f * tau);
                        const F32 a = 0.2f * sinf(3.14159f * tau / life) * endk;
                        softDisc(at, fs0 * 3.f * (1.f + 2.5f * tau), left, up, LLColor4(0.2f, 0.19f, 0.18f, a), false);
                    }
                }
                const F32 life = 0.9f;
                if (tau >= life) continue;
                const LLVector3 at = b.w + drift;
                const F32 heat = expf(-tau / 0.18f);
                const F32 fade = (1.f - smooth(0.55f, 1.f, tau / life)) * endk;
                const F32 fs = fs0 * (0.6f + 0.8f * b.r) * (1.f - 0.4f * tau / life);
                if (solid)
                {
                    // the flake: a small quad tumbling about its own axis, hot to grey
                    LLVector3 ax(b.r - 0.5f, fr(b.r * 7.3f) - 0.5f, fr(b.r * 3.1f) - 0.5f);
                    if (ax.normVec() < 1e-3f) ax.set(0.f, 0.f, 1.f);
                    const LLQuaternion tq(tau * (4.f + 6.f * b.r), ax);
                    const LLVector3 u = LLVector3(fs, 0.f, 0.f) * tq, w = LLVector3(0.f, 0.7f * fs, 0.f) * tq;
                    const LLColor3 grey(0.33f, 0.31f, 0.3f), ember(0.9f * c.mV[0] + 0.1f, 0.35f * c.mV[1] + 0.05f, 0.1f * c.mV[2]);
                    const LLColor3 hot(1.f, 0.75f + 0.25f * c.mV[1], 0.35f + 0.4f * c.mV[2]);
                    const F32 h2 = smooth(0.f, 1.f, heat);
                    const LLColor3 fc = grey * (1.f - smooth(0.f, 0.6f, heat)) + ember * (smooth(0.f, 0.6f, heat) * (1.f - h2)) + hot * h2;
                    gGL.color4f(fc.mV[0], fc.mV[1], fc.mV[2], fade);
                    const LLVector3 q0 = at - u - w, q1 = at + u - w, q2 = at + u + w, q3 = at - u + w;
                    gGL.vertex3fv(q0.mV); gGL.vertex3fv(q1.mV); gGL.vertex3fv(q2.mV);
                    gGL.vertex3fv(q0.mV); gGL.vertex3fv(q2.mV); gGL.vertex3fv(q3.mV);
                }
                else if (heat > 0.05f)
                {
                    const F32 k = heat * fade;
                    softDisc(at, fs * 2.2f, left, up, LLColor4(c.mV[0] * k, c.mV[1] * 0.7f * k, c.mV[2] * 0.4f * k, 1.f));
                }
            }
        }
        else if (o.effect == FX_DISINTEGRATE && !solid)
        {
            const LLColor4 c = s_dis;
            const F32 vs = llclamp(radius * 0.09f, 0.03f, 1.5f);
            const F32 spd0 = llclamp(0.5f + 0.45f * radius, 0.6f, 6.f);
            for (const Bit& b : o.bits)
            {
                const F32 dt = (p - b.t0) * len;
                if (dt <= 0.f) continue;
                const F32 life = llmax((1.f - b.t0) * len, 1e-3f);
                const F32 shrink = 1.f - smooth(0.3f, 1.f, dt / life);
                if (shrink <= 0.01f) continue;
                LLVector3 outw = b.c * o.rot + LLVector3(0.f, 0.f, 0.3f * radius);
                if (outw.normVec() < 1e-4f) outw.set(0.f, 0.f, 1.f);
                const F32 speed = (0.7f + 1.5f * b.sp) * spd0;
                auto at = [&](F32 t) { return o.pos + b.c * o.rot + outw * (speed * t) + LLVector3(0.f, 0.f, -3.f * t * t); };
                const LLVector3 head = at(dt), tail = at(llmax(dt - 0.16f, 0.f));
                const F32 k = shrink * (0.5f + 0.5f * expf(-dt / 0.15f));
                trail(head, tail, 0.5f * vs * shrink, eye, LLColor4(c.mV[0] * k, c.mV[1] * k, c.mV[2] * k, 1.f));
                softDisc(head, 0.7f * vs * shrink, left, up, LLColor4(c.mV[0] * k * 0.6f, c.mV[1] * k * 0.6f, c.mV[2] * k * 0.6f, 1.f));
            }
        }
        else if (o.effect == FX_SHATTER)
        {
            const LLColor4 c = s_sha;
            const LLColor4 white(1.f, 1.f, 1.f, 1.f), gc(0.6f + 0.4f * c.mV[0], 0.6f + 0.4f * c.mV[1], 0.6f + 0.4f * c.mV[2], 1.f);
            const F32 t = p * len;                                // s since the impact
            const LLVector3 n = sSlots[o.slot].dir;
            const LLVector3 hit = o.pos + o.impact;
            const F32 cs = shardSize(radius);
            const F32 shrink = 1.f - smooth(kShShrinkStart, 1.f, p);
            const F32 sizeK = llclamp(0.6f + 0.5f * radius, 0.6f, 3.f);
            auto glintK = [](F32 dt, F32 spin, F32 r) { return powf(llmax(sinf(dt * spin + r * 6.2831853f), 0.f), 12.f); };
            if (!solid)
            {
                // The flash at the impact: a soft white-hot disc in a wider coloured one and a star,
                // a little toward the eye (off the surface it sits on).
                const F32 fk = expf(-p / kShFlashDecay);
                if (fk > 0.02f)
                {
                    const F32 fr = llclamp(kShFlashSize * radius, 0.04f, 1.5f);
                    LLVector3 toEye = eye - hit;
                    toEye.normVec();
                    const LLVector3 at = hit + toEye * (0.5f * fr);
                    softDisc(at, fr * (2.f + 2.f * (1.f - fk)), left, up, c * fk);
                    softDisc(at, fr * 0.7f, left, up, white * fk);
                    glint(at, fr * 4.f * fk, left, up, white * fk);
                }
                // A glint on each loose shard as it spins (in step with its surface glint).
                for (const Bit& b : o.bits)
                {
                    const F32 dt = (p - b.t0) * len;
                    if (dt <= 0.f) continue;
                    const F32 g = glintK(dt, b.sp, b.r) * shrink;
                    if (g < 0.03f) continue;
                    const LLVector3 at = o.pos + b.c + flight(b.v, dt, kShBurstDrag);
                    glint(at, 0.6f * cs * (0.4f + 0.6f * g), left, up, gc * (kShGlint * g));
                    softDisc(at, 0.12f * cs, left, up, gc * (0.6f * g));
                }
            }
            // The impact burst: per particle, a closed form of the time since the impact.
            const F32 v0 = burstSpeed(radius);
            for (U32 i = 0; i < o.burst; ++i)
            {
                F32 r[5];
                for (S32 k = 0; k < 5; ++k) r[k] = hash13(LLVector3((F32)i, (F32)k * 7.f + 1.f, o.seed));
                // out from the surface (toward the camera) and across it
                const F32 ph = 6.2831853f * r[0], ct = 2.f * r[1] - 1.f, st = sqrtf(llmax(1.f - ct * ct, 0.f));
                LLVector3 d = n * (0.25f + 0.6f * r[2]) + LLVector3(st * cosf(ph), st * sinf(ph), ct) * 0.9f;
                if (d.normVec() < 1e-3f) d = n;
                const U32 kind = i % 5;
                if (kind == 3)
                {
                    // A glass chip: a small tumbling sliver, faster and farther than the shards.
                    if (shrink <= 0.01f) continue;
                    const LLVector3 at = hit + flight(d * (kShChipBoost * v0 * (0.7f + 0.6f * r[3])), t, kShChipDrag);
                    const F32 spin = 20.f + 20.f * r[4];
                    const F32 sz = kShChipSize * cs * (0.5f + 0.5f * r[3]) * shrink;
                    if (solid)
                    {
                        LLVector3 ax(r[4] - 0.5f, r[0] - 0.5f, r[2] - 0.5f);
                        if (ax.normVec() < 1e-3f) ax.set(0.f, 0.f, 1.f);
                        const LLQuaternion tq(t * spin, ax);
                        const LLVector3 a = LLVector3(sz, 0.f, 0.f) * tq, b = LLVector3(-0.4f * sz, 0.6f * sz, 0.f) * tq,
                                        e = LLVector3(-0.5f * sz, -0.5f * sz, 0.1f * sz) * tq;
                        solidTri(at + a, at + b, at + e, LLColor3(gc.mV[0], gc.mV[1], gc.mV[2]), 0.7f);
                    }
                    else
                    {
                        const F32 g = glintK(t, spin, r[1]) * shrink;
                        if (g >= 0.03f) glint(at, 2.5f * sz * (0.5f + 0.5f * g), left, up, gc * (kShGlint * g));
                    }
                }
                else if (kind == 4)
                {
                    // Glass dust: a pale puff thrown off the impact, spreading and thinning out.
                    if (!solid) continue;
                    const F32 life = 0.7f;
                    if (t >= life) continue;
                    const LLVector3 at = hit + d * (0.35f * sizeK * (1.f - expf(-4.f * t))) + LLVector3(0.f, 0.f, 0.12f * t);
                    const F32 pr = llclamp(kShDustSize * radius, 0.04f, 1.2f) * (0.6f + 0.8f * r[3]) * (1.f + (1.f - expf(-3.f * t)));
                    const F32 a = kShDustAlpha * powf(1.f - t / life, 1.5f) * llmin(t / 0.03f, 1.f) * endk;
                    softDisc(at, pr, left, up, LLColor4(0.55f + 0.3f * c.mV[0], 0.55f + 0.3f * c.mV[1], 0.55f + 0.3f * c.mV[2], a), false);
                }
                else
                {
                    // A spark: a white-hot streak, fast and short, cooling to the glow colour.
                    if (solid) continue;
                    const F32 life = kShSparkLife * (0.35f + 0.65f * r[4]);
                    if (t >= life) continue;
                    const LLVector3 v = d * (kShSparkSpeed * sizeK * (0.5f + r[3]));
                    const LLVector3 head = hit + flight(v, t, 3.f), tail = hit + flight(v, llmax(t - 0.035f, 0.f), 3.f);
                    const F32 age = t / life, k = (1.f - age) * (1.f - age) * endk;
                    const LLColor4 sc = white * (1.f - age) + c * age;
                    trail(head, tail, 0.012f * sizeK, eye, sc * k);
                }
            }
        }
    }
    gGL.end();
}

void renderOverlay()
{
    if (sObjs.empty()) return;
    renderBeams();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    renderDebris(true);
    renderPrintHeads(true);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    renderDebris(false);
    renderPrintHeads(false);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

bool objectKilled(LLViewerObject* obj)
{
    if (!obj) return false;
    const LLUUID& id = obj->getID();
    sSeen.erase(id);   // gone: if it comes back, that is a new appearance
    auto forget = [&]()
    {
        auto it = sObjs.find(id);
        if (it != sObjs.end()) forgetObj(it);
        return false;
    };
    if (obj->isAvatar())
    {
        // A leaving avatar just goes -- and so do the attachments still de-rezzing on it (the
        // server may take them off a moment before the avatar): nothing will pose them now.
        for (auto it = sObjs.begin(); it != sObjs.end();)
        {
            if (it->second.dying && it->second.wearer == id)
            {
                LLCleanInstanceStore::instance().releaseDyingRows(it->first);
                it = forgetObj(it);
            }
            else ++it;
        }
        return forget();
    }
    // Whether this object de-rezzes, and on whom. A linkset decides once, at its root: the root
    // unparents its children before it kills them, so a child cannot tell any more that it is
    // part of a HUD, an attachment or animesh.
    bool allowed = true;
    LLUUID wearer;
    LinkKill pose;
    bool havePose = false;
    auto lk = sLinkset.find(id);
    if (lk != sLinkset.end())
    {
        allowed = lk->second.allowed;
        wearer = lk->second.wearer;
        pose = lk->second;
        havePose = true;
        sLinkset.erase(lk);
    }
    else
    {
        const LLViewerObject* root = obj->getRootEdit();
        // The renderer is not running (no update lately): nothing would ever release the rows.
        if (sLastUpdate < 0.0 || LLFrameTimer::getTotalSeconds() - sLastUpdate > 1.0) allowed = false;
        // Exiting, teleporting, or a region going away (a neighbour dropped): objects just go.
        else if (LLApp::isExiting() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) allowed = false;
        else if (!obj->getRegion() || !LLWorld::getInstance()->isRegionListed(obj->getRegion())) allowed = false;
        else if (isTemporary(obj) || isHud(obj)) allowed = false;
        // Animesh: its skeleton (a control avatar) goes with it -- nothing would pose the rows.
        else if (obj->isAnimatedObject() || (root && root->isAnimatedObject())) allowed = false;
        else if (isAvatarOrWorn(obj))
        {
            // An attachment taken off, its wearer staying: it de-rezzes on the wearer (rigged or
            // not, the wearer's skeleton still poses it). With the wearer it just goes.
            const LLVOAvatar* av = root ? root->getAvatar() : obj->getAvatar();
            if (!avatarsToo() || !av || av->isDead()) allowed = false;
            else wearer = av->getID();
        }
        for (const LLViewerObject* child : obj->getChildren())
        {
            if (!child || child->isAvatar()) continue;
            LinkKill& k = sLinkset[child->getID()];
            k.allowed = allowed;
            k.wearer = wearer;
            k.pos = child->getPositionAgent();   // (while still linked: the root unparents it before its kill)
            k.rot = child->getRotationRegion();
        }
    }
    // Prims only (no water, trees, grass or terrain).
    if (!allowed || obj->getPCode() != LL_PCODE_VOLUME) return forget();
    const U32 fx = pickDeRez();
    if (fx == FX_NONE || sObjs.size() >= kMaxObjects) return forget();
    sNow = LLFrameTimer::getTotalSeconds();
    if (!start(obj, fx, true, havePose ? &pose : nullptr)) return forget();
    sObjs[id].wearer = wearer;
    return true;
}
}   // namespace GMDXRezFx
