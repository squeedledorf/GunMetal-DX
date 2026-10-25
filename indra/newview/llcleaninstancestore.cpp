/**
 * @file llcleaninstancestore.cpp
 * @brief Implementation of LLCleanInstanceStore.
 */

#include "llviewerprecompiledheaders.h"
#include "llcrossingtrace.h"

#include "llcleaninstancestore.h"
#include "llworldclock.h"
#include "llcleandebug.h"

#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleanskinstore.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llgl.h"
#include "llprofiler.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "llvoavatar.h"
#include "llviewerjointattachment.h"
#include "pipeline.h"                 // LLPipeline::sPickAvatar
#include <unordered_set>
#include <sstream>
#include "llviewercontrol.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <functional>

// CPU-only store: the VK world renderer reads mInstances directly (cpuInstanceBytes).

LLCleanInstanceStore& LLCleanInstanceStore::instance()
{
    static LLCleanInstanceStore s;
    return s;
}

// OWNED model matrix: volume-local -> AGENT space from the object's CURRENT world
// pose (getRenderPosition/getRenderRotation compose parent linkset + attachment +
// region origin; getScale is the object scale). Identical formula to
// LLCleanIngest::registerCleanFace so every transform writer agrees --
// no fighting between the ingest sweep, the per-frame mover refresh, and the static
// rolling sweep. Replaces the LL relativeXform*renderMatrix path (LL-cached, goes
// stale for static drawables -> the #29 off-screen-draw suspect). Returns false if
// the object can't supply a pose. out is 16 floats (column-major, LLMatrix4 layout).
// Target-omega spinner drawn by the GPU spin path? (vkUsesGpuSpin: stationary,
// non-physical, unsat linksets only -- a moving vehicle takes LL's eased drawable pose.)
static bool isOmegaSpinner(const LLViewerObject* o)
{
    return vkUsesGpuSpin(o);
}

void LLCleanInstanceStore::composeOwnedPose(LLViewerObject* vobj, F32 out[16],
                                            F32 spin_axis[4], F32 spin_pivot[4])
{
    // OBJECT-STATE pose, NOT drawable state. getRenderPosition/getRenderRotation
    // read the DRAWABLE xform, which LL's own render loop used to maintain lazily
    // -- with the clean pipeline bypassing LL's renderer, a mass drawable-state
    // reset (the hitch event) left getRenderPosition returning garbage (-1,0,0)
    // for thousands of objects FOREVER (audit: stored==render==(-1,0,0) while
    // agent==(242,20,22)). The sweep then faithfully re-snapshotted the garbage
    // every pass (CHANGED=0) = the month-old #29 "mass vanish on hitch, never
    // heals". getPositionAgent/getRotationRegion are maintained by OBJECT updates
    // (network/interp), independent of any drawable/render machinery we bypass.
    LLVector3    wpos = vobj->getPositionAgent();
    LLQuaternion wrot = vobj->getRotationRegion();
    // Scale is not inherited down a linkset (each prim carries its own), so the eased
    // scale is just this prim's (#180: LL eases llSetScale like llSetPos/llSetRot).
    LLVector3    wscale = vobj->getScale();
    for (int k = 0; k < 4; ++k) { spin_axis[k] = 0.f; spin_pivot[k] = 0.f; }
    // Render poses used below (and by the spinner branch, which must not mix in
    // object state: a wheel drawn at the eased pose but spun about the object-state
    // pivot orbits ~vel*dt off-centre -- the "wrong axis" wheels, #96).
    LLVector3 lp, pp; LLQuaternion lr, pr;
    bool have_parent_pose = false;
    {
        // SMOOTH SCRIPTED MOTION: LL renders damped llSetPos/llSetRot moves with the
        // drawable's EASED xform (LLDrawable::updateXform lerp/nlerp), not the snapped
        // object target. Compose from the eased poses (root in agent space, child
        // relative to it) -- identical to object state once settled.
        LLViewerObject* par = (LLViewerObject*)vobj->getParent();
        if (!par)
        {
            vkRenderLocalPose(vobj, wpos, wrot, &wscale);
        }
        else if (par->getPCode() == LL_PCODE_VOLUME && !par->getParent())
        {
            vkRenderLocalPose(vobj, lp, lr, &wscale);
            vkRenderLocalPose(par, pp, pr);
            have_parent_pose = true;
            wpos = pp + lp * pr;
            wrot = lr * pr;
        }
    }

    // GPU TARGET OMEGA (VK). LL spins on the CPU every frame (applyAngularVelocity:
    // rot = rot * dQ, accumulated in mAngularVelocityRot), which would force a
    // re-upload per frame. Instead factor the accumulated spin OUT (stable base
    // pose) and hand the GPU the axis/rate/phase; the VS applies it. rot*dQ spins
    // about the axis expressed in the PARENT frame, around the prim's origin.
    // A spinning linkset ROOT spins its children about the root origin.
    LLViewerObject* parent = (LLViewerObject*)vobj->getParent();
    const bool parent_ok = parent && parent->getPCode() == LL_PCODE_VOLUME && !parent->isAttachment();
    LLViewerObject* spinner = nullptr;
    LLQuaternion frame;   // rotation taking the spinner's omega axis to region space
    LLVector3 pivot;
    if (isOmegaSpinner(vobj) && (!parent || parent_ok))
    {
        // Spinning prim: base = its own rotation with the accumulated spin factored
        // out (object state -- both terms come from the same spin integration),
        // placed in its parent's RENDER frame; pivot = where it is drawn.
        spinner = vobj;
        const LLQuaternion base = vobj->getRotation() * ~vobj->getAngularVelocityRot();
        frame = have_parent_pose ? pr : LLQuaternion::DEFAULT;
        wrot  = base * frame;
        pivot = wpos;
    }
    else if (parent_ok && !parent->getParent() && isOmegaSpinner(parent) && have_parent_pose)
    {
        // Child of a spinning root: children spin with it about the root origin.
        spinner = parent;
        const LLQuaternion pbase = parent->getRotation() * ~parent->getAngularVelocityRot();
        frame = LLQuaternion::DEFAULT;
        wrot  = vobj->getRotation() * pbase;
        wpos  = pp + lp * pbase;
        pivot = pp;
    }
    if (spinner)
    {
        const LLVector3 w = spinner->getAngularVelocity() * frame;
        spin_axis[0] = w.mV[VX]; spin_axis[1] = w.mV[VY]; spin_axis[2] = w.mV[VZ];
        // t0 on the world clock: the VS phase is cam_time.w (= LLWorldClock::getTime()) - t0.
        spin_axis[3] = (F32)LLWorldClock::getTime() - spinner->getRotTime();
        spin_pivot[0] = pivot.mV[VX]; spin_pivot[1] = pivot.mV[VY]; spin_pivot[2] = pivot.mV[VZ];
    }

    const LLVector3 ax = LLVector3(wscale.mV[VX], 0.f, 0.f) * wrot;
    const LLVector3 ay = LLVector3(0.f, wscale.mV[VY], 0.f) * wrot;
    const LLVector3 az = LLVector3(0.f, 0.f, wscale.mV[VZ]) * wrot;
    LLMatrix4 m;
    m.initRows(LLVector4(ax, 0.f), LLVector4(ay, 0.f),
               LLVector4(az, 0.f), LLVector4(wpos, 1.f));
    std::memcpy(out, m.mMatrix, sizeof(F32) * 16);
}

void LLCleanInstanceStore::mulMat4(const F32 a[16], const F32 b[16], F32 out[16])
{
    F32 r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1]
                           + a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    std::memcpy(out, r, sizeof(r));
}

// Write the owned pose (+ spin record) into an instance; true if anything changed.
// The spin phase t0 is only replaced when the axis changes: recomputing it every
// refresh would jitter by the time-dilation drift and dirty spinners every frame.
// d (#568): the store's render-frame offset (nullptr = 0): the composed agent-space pose is
// stored as agent + d in the row's agent-space lanes (rowAgentSpace).
// applyComposedPose: the same with the object's composed pose (composeOwnedPose output)
// supplied -- it depends only on the object, so a multi-face object composes it once.
static bool applyComposedPose(LLCleanInstanceStore::Instance& inst, const F32 pose[16], const F32 pose_ax[4],
                              const F32 pose_pv[4], const F32* local, const F32* d)
{
    F32 m[16], ax[4], pv[4];
    std::memcpy(m, pose, sizeof(m));
    std::memcpy(ax, pose_ax, sizeof(ax));
    std::memcpy(pv, pose_pv, sizeof(pv));
    if (local) LLCleanInstanceStore::mulMat4(m, local, m);   // node-local pre-transform (#39)
    if (d && LLCleanInstanceStore::rowAgentSpace(inst.flags, inst.lod_group_id))
    {
        if ((inst.flags & 1u) == 0u) { m[12] += d[0]; m[13] += d[1]; m[14] += d[2]; }
        pv[0] += d[0]; pv[1] += d[1]; pv[2] += d[2];
    }
    bool changed = false;
    if (std::memcmp(inst.model_matrix, m, sizeof(m)) != 0)
    {
        std::memcpy(inst.model_matrix, m, sizeof(m));
        changed = true;
    }
    if (std::memcmp(inst.spin_axis, ax, sizeof(F32) * 3) != 0)
    {
        std::memcpy(inst.spin_axis, ax, sizeof(ax));
        changed = true;
    }
    if (std::memcmp(inst.spin_pivot, pv, sizeof(F32) * 3) != 0)   // w: texel demand input, not pose
    {
        std::memcpy(inst.spin_pivot, pv, sizeof(F32) * 3);
        changed = true;
    }
    return changed;
}

static bool applyOwnedPose(LLViewerObject* vobj, LLCleanInstanceStore::Instance& inst,
                           const F32* local = nullptr, const F32* d = nullptr)
{
    F32 m[16], ax[4], pv[4];
    LLCleanInstanceStore::composeOwnedPose(vobj, m, ax, pv);
    return applyComposedPose(inst, m, ax, pv, local, d);
}

F32 LLCleanInstanceStore::packUVExtent(F32 du, F32 dv)
{
    // Degenerate / unknown (caps keep a zero mTexCoordExtents): unset, the cull uses 1.
    if (!(du > 1e-4f) || !(dv > 1e-4f) || !llfinite(du) || !llfinite(dv)) return 0.f;
    auto q = [](F32 v) -> U32
    {
        const F32 l = llclamp(log2f(v), -8.f, 8.f);
        return (U32)llclamp((S32)lroundf((l + 8.f) * (16383.f / 16.f)), 0, 16383);
    };
    // bit 28 = valid; it also keeps the float normal (no NaN / denormal bit patterns).
    const U32 bits = q(du) | (q(dv) << 14) | 0x10000000u;
    F32 f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool LLCleanInstanceStore::init(U32 initial_capacity)
{
    // CPU-only store (the VK renderer mirrors it via copyDirtyVK). Idempotent: a
    // repeat init hits grow()'s no-grow early-out.
    return (mCapacity >= initial_capacity) ? true : grow(initial_capacity);
}

bool LLCleanInstanceStore::grow(U32 new_cap)
{
    if (new_cap <= mCapacity) return true;

    const U32 old_cap = mCapacity;
    mInstances.resize(new_cap);
    mDirty.resize(new_cap, 0);
    mSig.resize(new_cap, 0);
    mRowGen.resize(new_cap, 0);
    mActiveIndex.resize(new_cap, INVALID_ACTIVE_INDEX);
    mIdToKey.resize(new_cap);

    // Sentinel new entries so the compute cull skips them. A
    // default-constructed (zeroed) Instance has lod_group_id == 0
    // which is a VALID slot (the first registered mesh) -- the
    // compute shader would dispatch a draw for it. That's how the
    // "world wipe" happened: ~45k ghost instances drawing slot 0
    // at the world origin, overflowing the 65536 bucket cap.
    //
    // The sentinels also have to be uploaded to GPU; widen the
    // coalesced dirty range to cover the new entries. (The first-
    // grow special case below only handles mLiveCount > 0, and
    // initial init has mLiveCount == 0, so the sentinels would
    // otherwise stay CPU-only and the GPU SSBO would keep its
    // uninitialized contents.)
    for (U32 i = old_cap; i < new_cap; ++i)
    {
        mInstances[i].lod_group_id = 0xFFFFFFFFu;
        mDirty[i] = 1;
    }
    if (old_cap < mDirtyMin) mDirtyMin = old_cap;
    if (new_cap > mDirtyMax) mDirtyMax = new_cap;

    // New slots are free. The free list is a min-heap (freePop hands out the lowest id):
    // live ids stay packed at the bottom, so the tail can drain and trimTail can give
    // the id range (and every id-indexed GPU table) back under VRAM pressure.
    for (U32 i = new_cap; i > old_cap; --i)
    {
        mFreeList.push_back(i - 1);
    }
    std::make_heap(mFreeList.begin(), mFreeList.end(), std::greater<U32>());

    mCapacity = new_cap;
    // After grow, everything currently live must be re-uploaded.
    if (mLiveCount > 0)
    {
        mDirtyMin = 0;
        mDirtyMax = new_cap;
        std::fill(mDirty.begin(), mDirty.end(), (U8)1);
    }
    return true;
}

void LLCleanInstanceStore::freePush(U32 id)
{
    mFreeList.push_back(id);
    std::push_heap(mFreeList.begin(), mFreeList.end(), std::greater<U32>());
}

U32 LLCleanInstanceStore::freePop()
{
    std::pop_heap(mFreeList.begin(), mFreeList.end(), std::greater<U32>());
    const U32 id = mFreeList.back();
    mFreeList.pop_back();
    return id;
}

U32 LLCleanInstanceStore::trimTail()
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 cap = mCapacity;
    if (cap <= 65536u || mFreeList.size() < (size_t)cap / 2u) return 0;   // cannot halve
    // The highest id in use = the highest id NOT on the free list (a live key may hold a
    // sentinel row, so the rows themselves don't tell).
    std::vector<U8> is_free(cap, 0);
    for (U32 id : mFreeList) if (id < cap) is_free[id] = 1;
    U32 top = cap;
    while (top > 0 && is_free[top - 1]) --top;
    // Keep the grow() pattern (65536 x 2^k) with >= 25% room over the top id in use; only
    // when that at least halves the range. Rows still waiting for the mirror (dirty: a
    // release this frame) block it -- the renderer must have seen them as sentinels.
    U32 new_cap = 65536u;
    while (new_cap < top + top / 4u && new_cap < cap) new_cap *= 2u;
    if (new_cap > cap / 2u) return 0;
    for (U32 id = new_cap; id < cap; ++id)
        if (mDirty[id]) return 0;
    mFreeList.erase(std::remove_if(mFreeList.begin(), mFreeList.end(), [new_cap](U32 id) { return id >= new_cap; }),
                    mFreeList.end());
    std::make_heap(mFreeList.begin(), mFreeList.end(), std::greater<U32>());
    // Every id >= new_cap is free: sentinel row, no key, not active (removeActive ran at
    // release), no local transform. The renderer's mirror follows on its next copyDirtyVK
    // (a size change = one full copy) and the pick boxes / grid rebuild on the size change.
    mInstances.resize(new_cap);
    mInstances.shrink_to_fit();
    mDirty.resize(new_cap);
    mDirty.shrink_to_fit();
    mSig.resize(new_cap);
    mSig.shrink_to_fit();
    mRowGen.resize(new_cap);
    mRowGen.shrink_to_fit();
    mActiveIndex.resize(new_cap);
    mActiveIndex.shrink_to_fit();
    mIdToKey.resize(new_cap);
    mIdToKey.shrink_to_fit();
    if (mDirtyMax > new_cap) mDirtyMax = new_cap;
    if (mDirtyMin >= mDirtyMax) { mDirtyMin = 0xFFFFFFFFu; mDirtyMax = 0; }
    mCapacity = new_cap;
    return new_cap;
}

// static
LLCleanInstanceStore::RowRefs LLCleanInstanceStore::rowRefs(const Instance& in)
{
    RowRefs r{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    if (in.lod_group_id == 0xFFFFFFFFu) return r;   // free / sentinel row: holds nothing
    r.mat = in.material_id;
    if ((in.flags & 32u) != 0u)     r.trec = in.skin_id;   // terrain: record index
    else if ((in.flags & 1u) != 0u) r.skin = in.skin_id;   // rigged / flexi / system body
    return r;
}

// static
void LLCleanInstanceStore::retargetRefs(const RowRefs& o, const RowRefs& n)
{
    if (o.mat != n.mat || o.trec != n.trec)
        LLCleanMaterialStore::instance().retarget(o.mat, n.mat, o.trec, n.trec);
    if (o.skin != n.skin)
        LLCleanSkinStore::instance().retargetRef(o.skin, n.skin);
}

void LLCleanInstanceStore::shutdown()
{
    std::lock_guard<std::mutex> lk(mMutex);
    // Give back every live row's holder references (the stores may outlive us).
    {
        const RowRefs none{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        for (const Instance& in : mInstances) retargetRefs(rowRefs(in), none);
    }
    LLCleanMeshAsset::instance().resetGroupRefs();   // every instance goes
    mOrphanSuspect.clear();
    mOrphanCursorInit = false;
    mCapacity = 0;
    mLiveCount = 0;
    mWaterExclCount = 0;
    mInstances.clear();
    mFreeList.clear();
    mDirty.clear();
    mSig.clear();
    mRowGen.clear();
    mLocalXform.clear();
    keyClear();
    mDirtyMin = 0xFFFFFFFFu;
    mDirtyMax = 0;
    mChangeLog.clear();
    mChangeLogFull = true;
    // #568: an empty store starts over at the agent frame. (A queued GPU shift still
    // reaches the renderer: its rows are re-streamed from the emptied mirror anyway.)
    mRenderD[0] = mRenderD[1] = mRenderD[2] = 0.f;
    mMirrorShiftPending[0] = mMirrorShiftPending[1] = mMirrorShiftPending[2] = 0.f;
    mMirrorShiftPendingSet = false;
}

void LLCleanInstanceStore::keyAdd(const std::pair<LLUUID, S32>& k, U32 id)
{
    mKeyToId[k] = id;
    mKeyHash[k] = id;
    mObjIds[k.first].push_back(id);
}

void LLCleanInstanceStore::keyErase(std::pair<LLUUID, S32> k, U32 id)
{
    mKeyHash.erase(k);
    auto oit = mObjIds.find(k.first);
    if (oit != mObjIds.end())
    {
        std::vector<U32>& v = oit->second;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == id) { v[i] = v.back(); v.pop_back(); break; }
        if (v.empty()) mObjIds.erase(oit);
    }
    mKeyToId.erase(k);
}

void LLCleanInstanceStore::keyClear()
{
    mKeyToId.clear();
    mKeyHash.clear();
    mObjIds.clear();
}

U32 LLCleanInstanceStore::upsert(const LLUUID& object_uuid, S32 face_idx,
                                 const Instance& data_agent, bool is_active, U64 sig,
                                 const F32* local_xform)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // #568: callers pass agent space; the store keeps agent + d (rowAgentSpace lanes), so
    // the content compare below and the stored row are both in the store frame.
    Instance data = data_agent;
    toStoreFrame(data);
    const auto key = std::make_pair(object_uuid, face_idx);
    const U32 found = keyFind(object_uuid, face_idx);
    U32 id;
    if (found != 0xFFFFFFFFu)
    {
        id = found;
        // #567: a re-registration keeps the dynamic-caster bit (owned by the ingest's mover
        // tracker, setObjectDynamicCaster) -- dropping it for even one frame would let a
        // static shadow tile bake a moving object it never invalidates.
        data.flags |= (mInstances[id].flags & DYNAMIC_CASTER_FLAG);
        // <GMDX> rez effects: the effect bits stay; a dying row re-registered by a live object
        // of the same id (it came back within its de-rez) drops them.
        if ((mInstances[id].flags & REZFX_DYING_FLAG) == 0u) data.flags |= (mInstances[id].flags & REZFX_FLAGS);
        // #825: a representative-LOD swap re-registers the same LOD group with that level's
        // capture bounds -- keep the box steady (see stabilizeBounds).
        const bool absorbed = stabilizeBounds(mInstances[id], data);
        if (absorbed && mRegDiagOn && mShadowDirtyOn) ++mRegDiag.aabb_absorbed;
        // Real-change counter for the sweep's adaptive pace: rigged/active instances
        // are re-upserted every visit (no dedup signature), and movers' transforms
        // change continuously -- neither means the world still needs full-rate
        // ingest. Count content changes only (transform + spin excluded for movers).
        const Instance& old = mInstances[id];
        const size_t off = is_active ? offsetof(Instance, aabb_min) : 0;
        const size_t len = is_active ? (offsetof(Instance, spin_axis) - off) : sizeof(Instance);
        if (std::memcmp(reinterpret_cast<const U8*>(&old) + off,
                        reinterpret_cast<const U8*>(&data) + off, len) != 0)
        {
            ++mUpsertCount;
            // #567: cached shadow tiles holding the old or the new shape re-render.
            if (mShadowDirtyOn)
            {
                U32 flag_xor = 0;
                F32 aabb_delta = 0.f;
                const U32 raw = shadowChangeWhy(old, data, is_active, flag_xor, aabb_delta);
                const U32 why = raw ? shadowRelevantWhy(old, data, raw, flag_xor) : 0u;
                if (why != 0u)
                {
                    const bool emit_old = noteShadowDirtyLocked(old);
                    const bool emit_new = noteShadowDirtyLocked(data);
                    if ((emit_old || emit_new) && mRegDiagOn)
                        regDiagLocked(object_uuid, why, data.flags, flag_xor, aabb_delta);
                }
                else if (raw != 0u && mRegDiagOn && shadowCasterRow(old) && shadowCasterRow(data))
                {
                    // #825: what the relevance rules dropped (would have emitted before)
                    ++mRegDiag.dropped_events;
                    for (U32 k = 0; k < SRD_COUNT; ++k)
                        if ((raw & (1u << k)) != 0u) ++mRegDiag.dropped[k];
                    if ((raw & (1u << SRD_FLAGS)) != 0u)
                        for (U32 b = 0; b < 32; ++b)
                            if ((flag_xor & (1u << b)) != 0u) ++mRegDiag.dropped_flag_bits[b];
                }
            }
        }
    }
    else
    {
        if (mFreeList.empty())
        {
            grow(mCapacity == 0 ? 65536 : mCapacity * 2);
        }
        id = freePop();
        // <GMDX> rez effects: an object's first row queues it for its rez-in; a new face of an
        // object in an effect takes the effect bits of its other faces.
        {
            auto rit = mObjIds.find(object_uuid);
            if (rit == mObjIds.end())
            {
                if (mRezFxCollect && mRezFxNew.size() < 16384)
                {
                    mRezFxNew.push_back(object_uuid);
                    mRezFxNewAny.store(true, std::memory_order_relaxed);
                }
            }
            else
                for (const U32 iid : rit->second)
                    if (iid < mCapacity && (mInstances[iid].flags & REZFX_FLAGS) != 0u
                        && (mInstances[iid].flags & REZFX_DYING_FLAG) == 0u)
                    {
                        data.flags |= mInstances[iid].flags & REZFX_FLAGS;
                        break;
                    }
        }
        // </GMDX>
        keyAdd(key, id);
        mIdToKey[id] = key;   // flat reverse map (pick broadphase); stale on release,
                              // harmless: released ids are sentinel'd and skipped
        ++mLiveCount;
        ++mUpsertCount;   // new instance
        ++mNewCount;
        // #825: a NEW face of an object the mover tracker holds dynamic (a face / LOD rebuilt
        // while it moves) takes the bit at once -- before, it waited up to 8 frames for
        // tickDynCasters' re-assert: a moving object's face could bake into a static layer
        // meanwhile, and it emitted a registration sphere for a caster the overlay draws.
        if ((data.flags & (1u | 8u | DYNAMIC_CASTER_FLAG)) == 0u)
        {
            auto oit = mObjIds.find(object_uuid);
            if (oit != mObjIds.end())
                for (const U32 iid : oit->second)
                    if (iid != id && iid < mCapacity && (mInstances[iid].flags & DYNAMIC_CASTER_FLAG) != 0u)
                    {
                        data.flags |= DYNAMIC_CASTER_FLAG;
                        break;
                    }
        }
        // #567: a new caster in cached shadow tiles
        if (noteShadowDirtyLocked(data) && mRegDiagOn) regDiagLocked(object_uuid, 1u << SRD_NEW, data.flags, 0u, 0.f);
    }

    if (found != 0xFFFFFFFFu && (mInstances[id].flags & WATER_EXCLUSION_FLAG) != 0u) --mWaterExclCount;
    if ((data.flags & WATER_EXCLUSION_FLAG) != 0u) ++mWaterExclCount;
    // A fresh id is a sentinel row (holds nothing); a reused key releases what it held.
    retargetRefs(rowRefs(mInstances[id]), rowRefs(data));
    // LOD-group lifetime: instances are the groups' users (free rows hold the sentinel).
    LLCleanMeshAsset::instance().groupRefSwap(mInstances[id].lod_group_id, data.lod_group_id);
    mInstances[id] = data;
    mSig[id] = sig;
    if (local_xform)
    {
        std::array<F32, 16>& lx = mLocalXform[id];
        std::memcpy(lx.data(), local_xform, sizeof(F32) * 16);
    }
    else if (!mLocalXform.empty())
    {
        mLocalXform.erase(id);
    }
    mDirty[id] = 1;
    if (id < mDirtyMin) mDirtyMin = id;
    if (id + 1 > mDirtyMax) mDirtyMax = id + 1;

    if (is_active) addActive(id, object_uuid, face_idx);
    else           removeActive(id);
    return id;
}

bool LLCleanInstanceStore::setLocalXform(const LLUUID& object_uuid, S32 face_idx, const F32 local_xform[16])
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 id = keyFind(object_uuid, face_idx);
    if (id == 0xFFFFFFFFu) return false;
    std::array<F32, 16>& lx = mLocalXform[id];
    if (std::memcmp(lx.data(), local_xform, sizeof(F32) * 16) == 0) return false;
    std::memcpy(lx.data(), local_xform, sizeof(F32) * 16);
    return true;
}

bool LLCleanInstanceStore::isUpToDate(const LLUUID& object_uuid, S32 face_idx, U64 sig) const
{
    if (sig == 0) return false; // reserved "no signature"
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 fid = keyFind(object_uuid, face_idx);
    return fid != 0xFFFFFFFFu && mSig[fid] == sig;
}

void LLCleanInstanceStore::markDirtyLocked(U32 id)
{
    mDirty[id] = 1;
    if (id < mDirtyMin) mDirtyMin = id;
    if (id + 1 > mDirtyMax) mDirtyMax = id + 1;
}

void LLCleanInstanceStore::setShadowDirtyCollect(bool on)
{
    std::lock_guard<std::mutex> lk(mMutex);
    mShadowDirtyOn = on;
    if (!on) { mShadowDirty.clear(); mShadowDirtyOverflow = false; }
}

void LLCleanInstanceStore::takeShadowDirty(std::vector<F32>& out, bool& overflow)
{
    std::lock_guard<std::mutex> lk(mMutex);
    out.swap(mShadowDirty);
    mShadowDirty.clear();
    overflow = mShadowDirtyOverflow;
    mShadowDirtyOverflow = false;
}

// Shadow casters' depth depends on: where (pose / spin, unless the row is an active
// mover whose pose the mover tracker owns), what (LOD group = geometry, local bounds),
// how it is routed (flags: rigged / alpha / mask / double-sided / water ...), and for
// mask / blend faces the material (its alpha). Tint, UVs and texel-demand lanes are not.
// static
U32 LLCleanInstanceStore::shadowChangeWhy(const Instance& a, const Instance& b, bool is_active, U32& flag_xor,
                                          F32& aabb_delta)
{
    U32 why = 0;
    if (!is_active)
    {
        if (std::memcmp(a.model_matrix, b.model_matrix, sizeof(a.model_matrix)) != 0) why |= 1u << SRD_POSE;
        if (std::memcmp(a.spin_axis, b.spin_axis, sizeof(F32) * 3) != 0) why |= 1u << SRD_SPIN;
    }
    if (a.lod_group_id != b.lod_group_id) why |= 1u << SRD_GROUP;
    flag_xor = a.flags ^ b.flags;
    if (flag_xor != 0u) why |= 1u << SRD_FLAGS;
    aabb_delta = 0.f;
    for (int k = 0; k < 3; ++k)
        aabb_delta = std::max({ aabb_delta, std::fabs(a.aabb_min[k] - b.aabb_min[k]),
                                std::fabs(a.aabb_max[k] - b.aabb_max[k]) });
    if (std::memcmp(a.aabb_min, b.aabb_min, sizeof(F32) * 3) != 0
        || std::memcmp(a.aabb_max, b.aabb_max, sizeof(F32) * 3) != 0) why |= 1u << SRD_AABB;
    if ((b.flags & 6u) != 0u && a.material_id != b.material_id) why |= 1u << SRD_MATERIAL;
    return why;
}

// static
bool LLCleanInstanceStore::shadowCasterRow(const Instance& row)
{
    return row.lod_group_id != 0xFFFFFFFFu && (row.flags & (1u | 8u | 512u | DYNAMIC_CASTER_FLAG)) == 0u;
}

// #825 RELEVANCE RULES (what a cached static shadow layer actually holds of a re-registered
// row; the raw differences come from shadowChangeWhy):
//  - FLAGS: only the routing bits the shadow passes read (SHADOW_IGNORED_FLAGS lists the rest:
//    fullbright / unlit shading, self texture priority, live media -- a scripted lamp or a
//    media face toggling them changes no depth). Planar texgen (bit6) and GPU texture
//    animation (bit10) move the UVs, which a mask / blend face's cutout reads: relevant there.
//  - MATERIAL (mask / blend faces): only when the two materials cast differently
//    (LLCleanMaterialStore::shadowEquivalent: base texture / factor, cutoff, alpha mode) --
//    not a glow / emissive / specular / normal-map change.
// Pose, spin, LOD group and bounds (after stabilizeBounds) stay exact: a real move, a new
// shape (a new geometry key = a new group) or a real bounds change re-renders.
// static
U32 LLCleanInstanceStore::shadowRelevantWhy(const Instance& a, const Instance& b, U32 raw, U32 flag_xor)
{
    U32 why = raw;
    if ((why & (1u << SRD_FLAGS)) != 0u)
    {
        U32 ignored = SHADOW_IGNORED_FLAGS;
        if (((a.flags | b.flags) & 6u) == 0u) ignored |= 64u | 1024u;   // opaque: the UVs read nothing
        if ((flag_xor & ~ignored) == 0u) why &= ~(1u << SRD_FLAGS);
    }
    if ((why & (1u << SRD_MATERIAL)) != 0u
        && LLCleanMaterialStore::instance().shadowEquivalent(a.material_id, b.material_id))
        why &= ~(1u << SRD_MATERIAL);
    return why;
}

// #825 LOD-REPRESENTATIVE BOUNDS. registerCleanFace takes a row's local box from the
// representative level's capture (LL's live level when resident, else another resident
// one), so a re-registration after LL's LOD moved -- or a level being freed / captured --
// rewrote the box with another level's bounds although the row draws the same LOD GROUP
// (the GPU picks the level by distance; the box only feeds the culls). Each swap was a
// shadow-relevant change: a registration sphere and a re-rendered far cascade for nothing.
// Same group, a static-layer row (not rigged / HUD / worn; not terrain, whose box picks
// its level), and every face of the new box within 1/8 of the axis extent of the old one:
// keep the UNION (the culls stay conservative for every level; it only ever grows within
// that tolerance, so swaps back and forth settle on one box). Anything larger is taken as
// is (a real bounds change: the relevance test sees it). True if a difference was absorbed.
// static
bool LLCleanInstanceStore::stabilizeBounds(const Instance& old, Instance& data)
{
    if (old.lod_group_id != data.lod_group_id || old.lod_group_id == 0xFFFFFFFFu) return false;
    if (((old.flags | data.flags) & (1u | 8u | 32u | 512u)) != 0u) return false;
    if (std::memcmp(old.aabb_min, data.aabb_min, sizeof(F32) * 3) == 0
        && std::memcmp(old.aabb_max, data.aabb_max, sizeof(F32) * 3) == 0) return false;
    for (int k = 0; k < 3; ++k)
    {
        const F32 olo = old.aabb_min[k], ohi = old.aabb_max[k], nlo = data.aabb_min[k], nhi = data.aabb_max[k];
        if (!std::isfinite(olo) || !std::isfinite(ohi) || !std::isfinite(nlo) || !std::isfinite(nhi)
            || ohi < olo || nhi < nlo) return false;
        const F32 tol = 0.125f * std::max(ohi - olo, nhi - nlo) + 1e-4f;
        if (std::fabs(olo - nlo) > tol || std::fabs(ohi - nhi) > tol) return false;
    }
    for (int k = 0; k < 3; ++k)
    {
        data.aabb_min[k] = std::min(old.aabb_min[k], data.aabb_min[k]);
        data.aabb_max[k] = std::max(old.aabb_max[k], data.aabb_max[k]);
    }
    return true;
}

void LLCleanInstanceStore::setShadowRegDiag(bool on)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (on == mRegDiagOn) return;
    mRegDiagOn = on;
    mRegDiag = ShadowRegDiag{};
    mRegDiagObjs.clear();
}

void LLCleanInstanceStore::takeShadowRegDiag(ShadowRegDiag& out)
{
    std::lock_guard<std::mutex> lk(mMutex);
    out = mRegDiag;
    out.top.clear();
    for (const auto& kv : mRegDiagObjs) out.top.push_back(kv.second);
    std::sort(out.top.begin(), out.top.end(),
              [](const ShadowRegDiag::Obj& x, const ShadowRegDiag::Obj& y) { return x.n > y.n; });
    if (out.top.size() > kRegDiagTop) out.top.resize(kRegDiagTop);
    mRegDiag = ShadowRegDiag{};
    mRegDiagObjs.clear();
}

void LLCleanInstanceStore::regDiagLocked(const LLUUID& id, U32 why, U32 row_flags, U32 flag_xor, F32 aabb_delta)
{
    ++mRegDiag.events;
    for (U32 k = 0; k < SRD_COUNT; ++k)
        if ((why & (1u << k)) != 0u) ++mRegDiag.why[k];
    if ((why & (1u << SRD_FLAGS)) != 0u)
        for (U32 b = 0; b < 32; ++b)
            if ((flag_xor & (1u << b)) != 0u) ++mRegDiag.flag_bits[b];
    if ((why & (1u << SRD_AABB)) != 0u)
    {
        mRegDiag.aabb_max_delta = std::max(mRegDiag.aabb_max_delta, aabb_delta);
        if ((why & (1u << SRD_GROUP)) == 0u) ++mRegDiag.aabb_same_group;
    }
    auto it = mRegDiagObjs.find(id);
    if (it == mRegDiagObjs.end())
    {
        if (mRegDiagObjs.size() >= kRegDiagObjCap) return;   // (bounded: the counters above still count)
        it = mRegDiagObjs.emplace(id, ShadowRegDiag::Obj{}).first;
        it->second.id = id;
    }
    ShadowRegDiag::Obj& o = it->second;
    ++o.n;
    o.why |= why;
    o.row_flags = row_flags;
    o.flag_xor |= flag_xor;
    o.aabb_delta = std::max(o.aabb_delta, aabb_delta);
}

bool LLCleanInstanceStore::noteShadowDirtyLocked(const Instance& row)
{
    if (!mShadowDirtyOn || mShadowDirtyOverflow) return false;
    // Rigged / HUD / worn attachments / dynamic casters never sit in a cached static layer.
    if (!shadowCasterRow(row)) return false;
    Instance in = row;
    toAgentFrame(in);   // #568: the store keeps agent + d
    const F32* M = in.model_matrix;
    F32 h[3], c[3];
    for (int k = 0; k < 3; ++k)
    {
        const F32 lo = in.aabb_min[k], hi = in.aabb_max[k];
        const bool ok = std::isfinite(lo) && std::isfinite(hi) && hi >= lo;
        h[k] = ok ? 0.5f * (hi - lo) : 0.5f;   // unknown: the unit prim
        c[k] = ok ? 0.5f * (hi + lo) : 0.f;
    }
    F32 r2 = 0.f;
    for (int k = 0; k < 3; ++k)
    {
        const F32 col = std::sqrt(M[k * 4] * M[k * 4] + M[k * 4 + 1] * M[k * 4 + 1] + M[k * 4 + 2] * M[k * 4 + 2]);
        r2 += (col * h[k]) * (col * h[k]);
    }
    // A spinner sweeps the sphere about its pivot through its farthest point.
    F32 x = M[0] * c[0] + M[4] * c[1] + M[8] * c[2] + M[12];
    F32 y = M[1] * c[0] + M[5] * c[1] + M[9] * c[2] + M[13];
    F32 z = M[2] * c[0] + M[6] * c[1] + M[10] * c[2] + M[14];
    F32 r = std::sqrt(r2);
    if (in.spin_axis[0] != 0.f || in.spin_axis[1] != 0.f || in.spin_axis[2] != 0.f)
    {
        const F32 dx = x - in.spin_pivot[0], dy = y - in.spin_pivot[1], dz = z - in.spin_pivot[2];
        r += std::sqrt(dx * dx + dy * dy + dz * dz);
        x = in.spin_pivot[0]; y = in.spin_pivot[1]; z = in.spin_pivot[2];
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(r)) return false;
    r = std::max(r, 0.25f) + 0.25f;
    // COALESCE: an object's faces register / release back to back with (nearly) the same
    // bounds -- grow the last sphere to enclose this one when that costs little, so a
    // linkset rez or a teleport-in stays far below the cap (overflow re-renders every tile).
    if (mShadowDirty.size() >= 4)
    {
        F32* L = &mShadowDirty[mShadowDirty.size() - 4];
        const F32 dx = x - L[0], dy = y - L[1], dz = z - L[2];
        const F32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d + r <= L[3]) return true;                   // already inside
        const F32 R = 0.5f * (d + r + L[3]);              // enclosing sphere
        if (R <= std::max(L[3], r) * 1.25f + 0.25f)
        {
            if (L[3] + d <= r) { L[0] = x; L[1] = y; L[2] = z; L[3] = r; return true; }   // contains the last
            const F32 t = (R - L[3]) / std::max(d, 1e-6f);
            L[0] += dx * t; L[1] += dy * t; L[2] += dz * t; L[3] = R;
            return true;
        }
    }
    if (mShadowDirty.size() >= 4 * 8192) { mShadowDirtyOverflow = true; mShadowDirty.clear(); return true; }
    mShadowDirty.insert(mShadowDirty.end(), { x, y, z, r });
    return true;
}

bool LLCleanInstanceStore::setObjectDynamicCaster(const LLUUID& id, bool on)
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto oit = mObjIds.find(id);
    if (oit == mObjIds.end()) return false;
    bool any = false;
    for (const U32 iid : oit->second)
    {
        if (iid >= mCapacity) continue;
        Instance& inst = mInstances[iid];
        if ((inst.flags & (1u | 8u)) != 0u) continue;   // rigged / HUD: never in the static shadow cull
        const U32 f = on ? (inst.flags | DYNAMIC_CASTER_FLAG) : (inst.flags & ~DYNAMIC_CASTER_FLAG);
        if (f == inst.flags) continue;
        inst.flags = f;
        markDirtyLocked(iid);
        any = true;
    }
    return any;
}

// <GMDX> rez effects (gmdxrezfx.cpp)
bool LLCleanInstanceStore::setObjectRezFx(const LLUUID& id, U32 set, U32 clear, bool dying)
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto oit = mObjIds.find(id);
    if (oit == mObjIds.end()) return false;
    for (const U32 iid : oit->second)
    {
        if (iid >= mCapacity) continue;
        Instance& inst = mInstances[iid];
        U32 f = (inst.flags & ~clear) | set;
        if (dying)
        {
            noteShadowDirtyLocked(inst);              // its shadow goes now (the cull drops dying rows from shadows)
            if ((f & 6u) == 0u) f |= 4u;              // opaque -> mask buckets: the pre-pass cuts what the de-rez cut
            mSig[iid] = 0;                            // a live object of this id back meanwhile re-registers (upsert drops the bits)
        }
        if (f == inst.flags) continue;
        inst.flags = f;
        markDirtyLocked(iid);
    }
    return true;
}

void LLCleanInstanceStore::releaseDyingRows(const LLUUID& id)
{
    std::vector<S32> faces;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        auto oit = mObjIds.find(id);
        if (oit == mObjIds.end()) return;
        for (const U32 iid : oit->second)
            if (iid < mCapacity && (mInstances[iid].flags & REZFX_DYING_FLAG) != 0u && iid < mIdToKey.size())
                faces.push_back(mIdToKey[iid].second);
    }
    for (const S32 f : faces) release(id, f);
}

void LLCleanInstanceStore::setRezFxCollect(bool on)
{
    std::lock_guard<std::mutex> lk(mMutex);
    mRezFxCollect = on;
    if (!on) { mRezFxNew.clear(); mRezFxNewAny.store(false, std::memory_order_relaxed); }
}

void LLCleanInstanceStore::takeRezFxNew(std::vector<LLUUID>& out)
{
    std::lock_guard<std::mutex> lk(mMutex);
    out.swap(mRezFxNew);
    mRezFxNew.clear();
    mRezFxNewAny.store(false, std::memory_order_relaxed);
}
// </GMDX>

bool LLCleanInstanceStore::setFlexiOrigin(const LLUUID& id, const F32 origin[3])
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto oit = mObjIds.find(id);
    if (oit == mObjIds.end()) return false;
    bool any = false;
    for (const U32 iid : oit->second)
    {
        if (iid >= mCapacity) continue;
        Instance& inst = mInstances[iid];
        if ((inst.flags & 524288u) == 0u) continue;   // flexi chains only (bit0 row: agent space, no d)
        F32* t = &inst.model_matrix[12];
        if (t[0] == origin[0] && t[1] == origin[1] && t[2] == origin[2]) continue;
        t[0] = origin[0]; t[1] = origin[1]; t[2] = origin[2];
        markDirtyLocked(iid);
        any = true;
    }
    return any;
}

bool LLCleanInstanceStore::refreshObjectPose(LLViewerObject* obj)
{
    if (!obj || obj->isDead()) return false;
    std::lock_guard<std::mutex> lk(mMutex);
    const LLUUID& id = obj->getID();
    auto oit = mObjIds.find(id);
    if (oit == mObjIds.end()) return false;
    bool any = false;
    // The composed poses depend only on the object (each instance adds at most its own
    // glTF node pre-transform): compose each kind at most ONCE per refresh, not once per
    // face. A moved multi-face prim re-ran composeOwnedPose (parent chain, region ->
    // agent, spin factoring) per instance -- the bulk of the moved-queue drain.
    bool have_hud = false, hud_ok = false, have_att = false, have_world = false;
    F32 hud_m[16], att_m[16], world_m[16], world_ax[4], world_pv[4];
    for (const U32 iid : oit->second)
    {
        if (iid >= mCapacity) continue;
        Instance& inst = mInstances[iid];
        if ((inst.flags & 1u) != 0u) continue;   // rigged: pose comes from the skin palette
        const F32* local = localXformLocked(iid);   // glTF node pre-transform (#39)
        bool changed = false;
        F32 m[16];
        if ((inst.flags & 8u) != 0u)
        {
            // HUD: HUD-local pose (same composer as registerCleanFace / the HUD pick).
            if (!have_hud)
            {
                have_hud = true;
                LLMatrix4 c;
                hud_ok = vkComposeAttachmentMatrix(obj, c);
                if (hud_ok) std::memcpy(hud_m, c.mMatrix, sizeof(hud_m));
            }
            if (!hud_ok) continue;
            std::memcpy(m, hud_m, sizeof(m));
            if (local) mulMat4(m, local, m);
            changed = std::memcmp(inst.model_matrix, m, sizeof(m)) != 0;
        }
        else if ((inst.flags & 512u) != 0u)
        {
            // Worn attachment: pose RELATIVE TO THE ATTACH POINT (registerCleanFace's
            // formula); the VS applies the live joint matrix.
            if (!have_att)
            {
                have_att = true;
                LLVector3 wpos; LLQuaternion wrot; LLVector3 sc;
                vkRenderLocalPose(obj, wpos, wrot, &sc);   // eased while LL interpolates (scale too, #180)
                LLViewerObject* root = obj->getRootEdit();
                if (root && root != obj)
                {
                    LLVector3 rpos; LLQuaternion rrot;
                    vkRenderLocalPose(root, rpos, rrot);
                    wpos = wpos * rrot + rpos;
                    wrot = wrot * rrot;
                }
                LLMatrix4 c;
                c.initRows(LLVector4(LLVector3(sc.mV[VX], 0.f, 0.f) * wrot, 0.f),
                           LLVector4(LLVector3(0.f, sc.mV[VY], 0.f) * wrot, 0.f),
                           LLVector4(LLVector3(0.f, 0.f, sc.mV[VZ]) * wrot, 0.f),
                           LLVector4(wpos, 1.f));
                std::memcpy(att_m, c.mMatrix, sizeof(att_m));
            }
            std::memcpy(m, att_m, sizeof(m));
            if (local) mulMat4(m, local, m);
            changed = std::memcmp(inst.model_matrix, m, sizeof(m)) != 0;
        }
        else
        {
            if (!have_world)
            {
                have_world = true;
                composeOwnedPose(obj, world_m, world_ax, world_pv);
            }
            changed = applyComposedPose(inst, world_m, world_ax, world_pv, local,
                                        renderOffsetZero() ? nullptr : mRenderD);
            if (changed) { markDirtyLocked(iid); any = true; }
            continue;
        }
        if (changed)
        {
            std::memcpy(inst.model_matrix, m, sizeof(m));
            markDirtyLocked(iid);
            any = true;
        }
    }
    return any;
}

void LLCleanInstanceStore::addActive(U32 id, const LLUUID& uuid, S32 face_idx)
{
    if (id >= mActiveIndex.size()) return; // grow() sizes mActiveIndex
    if (mActiveIndex[id] != INVALID_ACTIVE_INDEX) return;
    mActiveIndex[id] = (U32)mActiveIds.size();
    mActiveIds.push_back(id);
    mActiveKey.emplace_back(uuid, face_idx);
}

void LLCleanInstanceStore::removeActive(U32 id)
{
    if (id >= mActiveIndex.size()) return;
    const U32 pos = mActiveIndex[id];
    if (pos == INVALID_ACTIVE_INDEX) return;
    const U32 last = mActiveIds.back();
    mActiveIds[pos] = last;
    mActiveKey[pos] = mActiveKey.back();
    mActiveIndex[last] = pos;
    mActiveIds.pop_back();
    mActiveKey.pop_back();
    mActiveIndex[id] = INVALID_ACTIVE_INDEX;
}

bool LLCleanInstanceStore::debugGet(const LLUUID& object_uuid, S32 face_idx, Instance& out,
                                    U32* out_id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyHash.find(std::make_pair(object_uuid, face_idx));
    if (it == mKeyHash.end()) return false;
    out = mInstances[it->second];
    toAgentFrame(out);   // #568: callers compare against LL / agent-space state
    if (out_id) *out_id = it->second;
    return true;
}

void LLCleanInstanceStore::release(const LLUUID& object_uuid, S32 face_idx)
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyHash.find(std::make_pair(object_uuid, face_idx));
    if (it == mKeyHash.end()) return;
    const U32 id = it->second;
    keyErase(it->first, id);   // erases mKeyToId too
    // #567: its shadow leaves cached tiles
    if (noteShadowDirtyLocked(mInstances[id]) && mRegDiagOn)
        regDiagLocked(object_uuid, 1u << SRD_RELEASE, mInstances[id].flags, 0u, 0.f);
    if ((mInstances[id].flags & WATER_EXCLUSION_FLAG) != 0u) --mWaterExclCount;
    retargetRefs(rowRefs(mInstances[id]), RowRefs{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu});
    LLCleanMeshAsset::instance().groupRefSwap(mInstances[id].lod_group_id, 0xFFFFFFFFu);
    // Sentinel the slot so the compute cull skips it (slot 0 is a
    // VALID mesh slot -- zeroing lod_group_id would alias to the
    // first registered asset and draw it everywhere).
    std::memset(&mInstances[id], 0, sizeof(Instance));
    mInstances[id].lod_group_id = 0xFFFFFFFFu;
    mSig[id] = 0;
    ++mRowGen[id];   // (rowGenerations: the id may next hold another key)
    mLocalXform.erase(id);
    mDirty[id] = 1;
    if (id < mDirtyMin) mDirtyMin = id;
    if (id + 1 > mDirtyMax) mDirtyMax = id + 1;
    removeActive(id);
    freePush(id);
    --mLiveCount;
}

void LLCleanInstanceStore::debugDumpInstanceFor(const LLUUID& obj, S32 face) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyHash.find(std::make_pair(obj, face));
    if (it == mKeyHash.end())
    {
        LL_INFOS("CleanSel") << "  NO clean instance for obj=" << obj
            << " face=" << face << LL_ENDL;
        return;
    }
    Instance in = mInstances[it->second];
    toAgentFrame(in);   // #568: log agent space, like every other position in the log
    auto col = [&](int c){ return sqrtf(in.model_matrix[c*4+0]*in.model_matrix[c*4+0]
        + in.model_matrix[c*4+1]*in.model_matrix[c*4+1]
        + in.model_matrix[c*4+2]*in.model_matrix[c*4+2]); };
    const F32* m3 = in.model_matrix;
    const F32 det3 =
          m3[0]*(m3[5]*m3[10]-m3[6]*m3[9])
        - m3[4]*(m3[1]*m3[10]-m3[2]*m3[9])
        + m3[8]*(m3[1]*m3[6]-m3[2]*m3[5]);
    LL_INFOS("CleanSel") << "  CLEAN inst id=" << it->second
        << " slot=" << in.lod_group_id << (in.lod_group_id==0xFFFFFFFFu?"(SENTINEL)":"")
        << " flags=" << in.flags
        << llformat(" det=%.3g%s", det3, det3 < 0.f ? " **REFLECTED**" : "")
        << llformat(" pos=(%.2f,%.2f,%.2f) scale=(%.2f,%.2f,%.2f)",
            in.model_matrix[12], in.model_matrix[13], in.model_matrix[14],
            col(0), col(1), col(2))
        << llformat(" aabb=[(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)]",
            in.aabb_min[0], in.aabb_min[1], in.aabb_min[2],
            in.aabb_max[0], in.aabb_max[1], in.aabb_max[2])
        << LL_ENDL;
}

void LLCleanInstanceStore::collectInUseMeshSlots(std::unordered_set<U32>& out) const
{
    // The GC frees by SLOT id, but an instance references a LOD GROUP. Gather the
    // live groups under our lock, then expand each to its resident slot_ids via the
    // mesh store (its own lock, taken OUTSIDE ours to avoid nested locking). Every
    // resident LOD of a referenced group is in use -- the cull may pick any level.
    // (Inserting the raw lod_group_id here -- a different number space than slot_id
    // -- made the GC free live slots -> #29 mass vanish.)
    std::vector<U32> groups;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        groups.reserve(mLiveCount);
        for (const Instance& inst : mInstances)
        {
            if (inst.lod_group_id != 0xFFFFFFFFu)   // 0xFFFFFFFF = empty/released
                groups.push_back(inst.lod_group_id);
        }
    }
    out.reserve(out.size() + groups.size() * 2);
    for (U32 gid : groups)
        LLCleanMeshAsset::instance().collectGroupSlots(gid, out);
}

void LLCleanInstanceStore::collectInUseMeshSlotsLod(std::unordered_set<U32>& out, const F32 cam_origin[3],
                                                    F32 lod_factor) const
{
    // gid -> the highest level any of its instances may select (NUM_LOD - 1 = all).
    std::unordered_map<U32, S32> want;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        // #568: rows are in the store frame (agent + d); move the agent-space camera there.
        const F32 cam_store[3] = {cam_origin[0] + mRenderD[0], cam_origin[1] + mRenderD[1], cam_origin[2] + mRenderD[2]};
        cam_origin = cam_store;
        want.reserve(mLiveCount);
        for (const Instance& inst : mInstances)
        {
            if (inst.lod_group_id == 0xFFFFFFFFu) continue;   // empty/released
            S32 lvl = LLCleanMeshAsset::NUM_LOD - 1;
            // rigged (1), HUD (8), terrain (32), worn attachment (512), vegetation (bit 15)
            const bool own_lod = (inst.flags & (1u | 8u | 32u | 512u | (1u << 15))) != 0u;
            const F32 radius = inst.aabb_min[3];
            if (!own_lod && radius > 0.f && lod_factor > 0.f)
            {
                // clean_cull.comp / LLCleanMeshAsset::drawnSlot: detailFromTan(ll_round(f r / d, 0.01)).
                const F32 dx = inst.model_matrix[12] - cam_origin[0];
                const F32 dy = inst.model_matrix[13] - cam_origin[1];
                const F32 dz = inst.model_matrix[14] - cam_origin[2];
                const F32 dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.001f);
                const F32 tan_angle = std::floor((lod_factor * radius) / dist * 100.f + 0.5f) * 0.01f;
                const S32 sel = tan_angle <= 0.03f ? 0 : tan_angle <= 0.06f ? 1 : tan_angle <= 0.24f ? 2 : 3;
                // One level of margin: LL's own calcLOD (the level the ingest captures)
                // and small camera moves land a level above the cull's choice.
                lvl = std::min(sel + 1, LLCleanMeshAsset::NUM_LOD - 1);
            }
            S32& w = want.try_emplace(inst.lod_group_id, lvl).first->second;
            w = std::max(w, lvl);
        }
    }
    out.reserve(out.size() + want.size() * 2);
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    for (const auto& kv : want)
    {
        if (kv.second >= LLCleanMeshAsset::NUM_LOD - 1) ms.collectGroupSlots(kv.first, out);
        else ms.collectGroupSlotsUpTo(kv.first, kv.second, out);
    }
}

U32 LLCleanInstanceStore::shiftOrigin(const F32 off[3])
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 n = (U32)mInstances.size();
    U32 shifted = 0;
    for (U32 id = 0; id < n; ++id)
    {
        Instance& in = mInstances[id];
        if (in.lod_group_id == 0xFFFFFFFFu) continue;    // free / sentinel slot
        if ((in.flags & (8u | 512u)) != 0u) continue;    // HUD / worn attachment: local
        if ((in.flags & 1u) == 0u)
        {
            in.model_matrix[12] += off[0];
            in.model_matrix[13] += off[1];
            in.model_matrix[14] += off[2];
        }
        // agent-space spin pivot (target omega)
        in.spin_pivot[0] += off[0]; in.spin_pivot[1] += off[1]; in.spin_pivot[2] += off[2];
        // the content changed: a later identical re-registration must still upsert
        mSig[id] = 0;
        mDirty[id] = 1;
        ++shifted;
    }
    if (n)
    {
        if (0u < mDirtyMin) mDirtyMin = 0u;
        if (n > mDirtyMax) mDirtyMax = n;
    }
    return shifted;
}

// ---- #568 RENDER-WORLD FRAME (see the header) ----------------------------------------------
void LLCleanInstanceStore::translateRow(Instance& in, const F32 t[3])
{
    if (!rowAgentSpace(in.flags, in.lod_group_id)) return;
    if ((in.flags & 1u) == 0u)
    {
        in.model_matrix[12] += t[0];
        in.model_matrix[13] += t[1];
        in.model_matrix[14] += t[2];
    }
    in.spin_pivot[0] += t[0]; in.spin_pivot[1] += t[1]; in.spin_pivot[2] += t[2];
}

void LLCleanInstanceStore::translateRowBytes(U8* row, const F32 t[3])
{
    U32 flags, lod;
    std::memcpy(&flags, row + offsetof(Instance, flags), sizeof(U32));
    std::memcpy(&lod, row + offsetof(Instance, lod_group_id), sizeof(U32));
    if (!rowAgentSpace(flags, lod)) return;
    auto add3 = [&](size_t off)
    {
        F32 v[3];
        std::memcpy(v, row + off, sizeof(v));
        v[0] += t[0]; v[1] += t[1]; v[2] += t[2];
        std::memcpy(row + off, v, sizeof(v));
    };
    if ((flags & 1u) == 0u) add3(offsetof(Instance, model_matrix) + 12 * sizeof(F32));
    add3(offsetof(Instance, spin_pivot));
}

void LLCleanInstanceStore::renderOffset(F32 out[3]) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    out[0] = mRenderD[0]; out[1] = mRenderD[1]; out[2] = mRenderD[2];
}

F32 LLCleanInstanceStore::renderOffsetSpan() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return std::max(std::fabs(mRenderD[0]), std::fabs(mRenderD[1]));
}

void LLCleanInstanceStore::noteAgentShift(const F32 off[3])
{
    std::lock_guard<std::mutex> lk(mMutex);
    // store = agent + d and agent' = agent + off  =>  d' = d - off keeps every stored row
    // (and every pick box) where it is. Exact: off and d are multiples of 256 m.
    for (int k = 0; k < 3; ++k)
    {
        mRenderD[k] -= off[k];
        mGpuShiftPending[k] += off[k];
    }
}

U32 LLCleanInstanceStore::reanchor(F32 delta_out[3])
{
    std::lock_guard<std::mutex> lk(mMutex);
    const F32 delta[3] = {-mRenderD[0], -mRenderD[1], -mRenderD[2]};
    if (delta_out) { delta_out[0] = delta[0]; delta_out[1] = delta[1]; delta_out[2] = delta[2]; }
    if (renderOffsetZero()) return 0;
    // store' = store + delta and d' = d + delta = 0: store - d is unchanged, so the GPU rows,
    // every renderer cache and the upload are untouched. The renderer's byte mirror follows
    // in its next copyDirtyVK (mMirrorShiftPending); the content signatures (mSig) are left:
    // a static whose recomputed signature now differs is re-upserted with identical content
    // on its next sweep visit (spread over the sweep, not a hitch).
    const U32 n = (U32)mInstances.size();
    U32 moved = 0;
    for (U32 id = 0; id < n; ++id)
    {
        Instance& in = mInstances[id];
        if (!rowAgentSpace(in.flags, in.lod_group_id)) continue;
        translateRow(in, delta);
        ++moved;
    }
    for (int k = 0; k < 3; ++k) mMirrorShiftPending[k] += delta[k];
    mMirrorShiftPendingSet = true;
    mRenderD[0] = mRenderD[1] = mRenderD[2] = 0.f;
    return moved;
}

void LLCleanInstanceStore::releaseObject(const LLUUID& object_uuid)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // mKeyToId is std::map<(uuid,face), id>; entries with same uuid
    // are contiguous (lexicographic on the pair). face_idx is S32
    // so INT32_MIN/MAX bound the range for this uuid.
    auto lo = mKeyToId.lower_bound(std::make_pair(object_uuid, (S32)INT32_MIN));
    auto hi = mKeyToId.upper_bound(std::make_pair(object_uuid, (S32)INT32_MAX));
    for (auto it = lo; it != hi; )
    {
        const U32 id = it->second;
        // #567: its shadow leaves cached tiles
        if (noteShadowDirtyLocked(mInstances[id]) && mRegDiagOn)
            regDiagLocked(object_uuid, 1u << SRD_RELEASE, mInstances[id].flags, 0u, 0.f);
        if ((mInstances[id].flags & WATER_EXCLUSION_FLAG) != 0u) --mWaterExclCount;
        retargetRefs(rowRefs(mInstances[id]), RowRefs{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu});
        LLCleanMeshAsset::instance().groupRefSwap(mInstances[id].lod_group_id, 0xFFFFFFFFu);
        std::memset(&mInstances[id], 0, sizeof(Instance));
        mInstances[id].lod_group_id = 0xFFFFFFFFu;
        mSig[id] = 0;
        ++mRowGen[id];   // (rowGenerations: the id may next hold another key)
        mLocalXform.erase(id);
        mDirty[id] = 1;
        if (id < mDirtyMin) mDirtyMin = id;
        if (id + 1 > mDirtyMax) mDirtyMax = id + 1;
        removeActive(id);
        freePush(id);
        --mLiveCount;
        mKeyHash.erase(it->first);
        it = mKeyToId.erase(it);
    }
    mObjIds.erase(object_uuid);
}

void LLCleanInstanceStore::auditOrphansSlice(U32 max_keys, U32 now_frame)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (mKeyToId.empty()) { mOrphanSuspect.clear(); return; }
    auto it = mOrphanCursorInit ? mKeyToId.upper_bound(mOrphanCursor) : mKeyToId.begin();
    mOrphanCursorInit = true;
    const U32 n = std::min<U32>(std::max<U32>(max_keys, 1u), (U32)mKeyToId.size());
    LLUUID last;
    for (U32 i = 0; i < n; ++i)
    {
        if (it == mKeyToId.end()) it = mKeyToId.begin();
        mOrphanCursor = it->first;
        const LLUUID id = it->first.first;
        ++it;
        if (id == last) continue;   // (one lookup per object: its faces are contiguous)
        last = id;
        LLViewerObject* obj = gObjectList.findObject(id);
        if (!obj || obj->isDead())
        {
            if (mOrphanSuspect.size() < 65536u) mOrphanSuspect.emplace(id, now_frame);
        }
        else if (!mOrphanSuspect.empty())
        {
            mOrphanSuspect.erase(id);
        }
    }
}

LLCleanInstanceStore::OrphanStats LLCleanInstanceStore::orphanStats(U32 now_frame, U32 min_age_frames)
{
    std::lock_guard<std::mutex> lk(mMutex);
    OrphanStats st;
    for (auto it = mOrphanSuspect.begin(); it != mOrphanSuspect.end(); )
    {
        auto oit = mObjIds.find(it->first);
        if (oit == mObjIds.end()) { it = mOrphanSuspect.erase(it); continue; }   // released since
        if (now_frame - it->second >= min_age_frames)
        {
            ++st.objects;
            st.rows += (U32)oit->second.size();
        }
        ++it;
    }
    st.suspects = (U32)mOrphanSuspect.size();
    return st;
}

void LLCleanInstanceStore::pushTint(const LLUUID& object_uuid, S32 face_idx,
                                    F32 r, F32 g, F32 b, F32 a)
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyHash.find(std::make_pair(object_uuid, face_idx));
    if (it == mKeyHash.end()) return; // not yet registered, no-op
    const U32 id = it->second;
    Instance& inst = mInstances[id];
    if (inst.tint[0] == r && inst.tint[1] == g &&
        inst.tint[2] == b && inst.tint[3] == a) return;
    inst.tint[0] = r; inst.tint[1] = g; inst.tint[2] = b; inst.tint[3] = a;
    // Tint (part of the dedup signature) changed outside upsert; invalidate the
    // stored signature so a later registerFace can't wrongly dedup a change-back.
    mSig[id] = 0;
    mDirty[id] = 1;
    if (id < mDirtyMin)     mDirtyMin = id;
    if (id + 1 > mDirtyMax) mDirtyMax = id + 1;
}

void LLCleanInstanceStore::rollingTransformSweep()
{
    // Lock is held by refreshAndReap. Recompose a bounded SLICE of ALL live
    // instances each frame from their CURRENT authoritative transform, advancing
    // a rolling cursor through mKeyToId so the whole store is covered every few
    // frames. This makes static-instance transforms self-heal: registration bakes
    // model_matrix = relativeXform * (region/render matrix) ONCE, and nothing
    // recomposes it when the region render matrix shifts (agent region-cross) or a
    // drawable flips active<->static -- the static then renders off-screen and is
    // frustum-rejected forever (#29). Recomposing on our own schedule fixes that
    // regardless of whether LL ever re-dirties the group.
    if (mKeyToId.empty()) return;

    constexpr U32 SLICE = 4096;
    const U32 budget = (mKeyToId.size() < (size_t)SLICE) ? (U32)mKeyToId.size() : SLICE;

    auto it = mSweepCursorInit ? mKeyToId.upper_bound(mSweepCursorKey)
                               : mKeyToId.begin();
    mSweepCursorInit = true;

    U32 scanned = 0, recomposed = 0, changed = 0;
    while (scanned < budget)
    {
        if (it == mKeyToId.end()) it = mKeyToId.begin(); // wrap to start
        const std::pair<LLUUID, S32> key = it->first;
        const U32 id = it->second;
        mSweepCursorKey = key;
        ++scanned;
        ++it;

        // Actives are refreshed every frame by the walk above -- don't redo them.
        if (id < mActiveIndex.size() && mActiveIndex[id] != INVALID_ACTIVE_INDEX) continue;
        if (id >= mCapacity) continue;

        Instance& inst = mInstances[id];
        // Rigged (bit0): transform comes from the skin palette in compute, model_
        // matrix ignored. HUD (bit3): rendered under a separate HUD projection with
        // its own matrix handling. Attachment (bit9): model_matrix is a CONSTANT
        // joint-local offset -- the live world pose is applied GPU-side from the
        // avatar joints table, so recomposing a world pose here would DOUBLE-
        // transform it (joint * world) and fling the geometry. Skip all three.
        if ((inst.flags & 1u) != 0u || (inst.flags & 8u) != 0u ||
            (inst.flags & 512u) != 0u) continue;

        LLViewerObject* vobj = gObjectList.findObject(key.first);
        // Pointer-safe: a transient findObject miss must NOT remove a static.
        // markDead already releases instances of genuinely-dead objects.
        if (!vobj || vobj->isDead() || !vobj->mDrawable || vobj->mDrawable->isDead())
            continue;

        LLDrawable* dr = vobj->mDrawable;
        LLVOVolume* vvol = dr->getVOVolume();
        bool pose_changed = false;
        if (vvol)
        {
            // OWNED transform from the live world pose (see composeOwnedPose) --
            // same formula as registerCleanFace and the mover refresh.
            pose_changed = applyOwnedPose(vobj, inst, localXformLocked(id), renderOffsetZero() ? nullptr : mRenderD);
        }
        else
        {
            // Terrain / non-volume: geometry is region-local, region matrix only.
            LLViewerRegion* rg = vobj->getRegion();
            if (!rg) continue;
            F32 rm[16];   // #568: the region matrix (agent space) in the store frame
            std::memcpy(rm, rg->mRenderMatrix.mMatrix, sizeof(rm));
            if (!renderOffsetZero() && rowAgentSpace(inst.flags, inst.lod_group_id))
            { rm[12] += mRenderD[0]; rm[13] += mRenderD[1]; rm[14] += mRenderD[2]; }
            if (std::memcmp(inst.model_matrix, rm, sizeof(inst.model_matrix)) != 0)
            {
                std::memcpy(inst.model_matrix, rm, sizeof(inst.model_matrix));
                pose_changed = true;
            }
        }

        ++recomposed;
        if (pose_changed)
        {
            mDirty[id] = 1;
            if (id < mDirtyMin)     mDirtyMin = id;
            if (id + 1 > mDirtyMax) mDirtyMax = id + 1;
            ++changed;
        }
    }

    // Audit: a persistently large CHANGED means stale static transforms ARE a real
    // source of vanished geometry (the #29 root). ~0 means transforms are stable
    // and this sweep has ruled them out -- on our own terms, continuously.
    static U32 s_tick = 0;
    if (CLEAN_DEBUG && (s_tick++ % 64) == 0)
    {
        LL_INFOS("Clean29Xform") << "rollingTransformSweep scanned=" << scanned
            << " recomposed=" << recomposed << " CHANGED=" << changed
            << " live=" << (U32)mKeyToId.size() << LL_ENDL;
    }
}

// AVATARS. Avatars are not render instances (their pickable shape is the joint collision
// volumes), so the GPU pick's fragment list never holds one (#704: kept on the CPU -- ~20
// analytic spheres per avatar plus the name tag, not mesh data): test each avatar's
// collision volumes with LL's precedence rule (LLPipeline::lineSegmentIntersectInWorld): the avatar wins
// unless the current hit is an attachment within ATTACHMENT_OVERRIDE_DIST of it.
// static
bool LLCleanInstanceStore::intersectAvatars(const LLVector4a& start, LLVector4a& local_end, LLViewerObject*& hit_obj,
                                            bool pick_transparent, bool pick_rigged, bool pick_unselectable,
                                            S32* face_hit, LLVector4a* intersection, LLVector2* tex_coord,
                                            LLVector4a* normal, LLVector4a* tangent)
{
    const F32 ATTACHMENT_OVERRIDE_DIST = 0.1f;
    bool taken = false;
    const bool saved_pick_avatar = LLPipeline::sPickAvatar;
    LLPipeline::sPickAvatar = true;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* av = (LLVOAvatar*)character;
        if (!av || av->isDead() || av->mDrawable.isNull()) continue;
        S32 fh = -1; LLVector4a ip, nrm, tng; LLVector2 tc;
        if (av->lineSegmentIntersect(start, local_end, -1, pick_transparent, pick_rigged,
                                     pick_unselectable, &fh, &ip, &tc, &nrm, &tng))
        {
            LLVector4a delta;
            delta.setSub(ip, local_end);
            if (!hit_obj || !hit_obj->isAttachment()
                || delta.getLength3().getF32() > ATTACHMENT_OVERRIDE_DIST)
            {
                hit_obj = av;
                local_end = ip;
                taken = true;
                if (face_hit)     *face_hit = fh;
                if (intersection) *intersection = ip;
                if (tex_coord)    *tex_coord = tc;
                if (normal)       *normal = nrm;
                if (tangent)      *tangent = tng;
                // #610 (RenderVKPickDiag): an avatar hit far from the avatar is a ghost
                // (stale collision volumes); collision volumes span ~1.5 m from the
                // pelvis (the name tag up to ~3 m). gpu_mode 2 = GPU-animated.
                static LLCachedControl<bool> s_av_diag(gSavedSettings, "RenderVKPickDiag", false);
                static F64 s_av_diag_next = 0.0;
                if (s_av_diag && LLFrameTimer::getTotalSeconds() >= s_av_diag_next)
                {
                    s_av_diag_next = LLFrameTimer::getTotalSeconds() + 1.0;
                    const F32 off = (LLVector3(ip.getF32ptr()) - av->getRenderPosition()).length();
                    LL_INFOS("VKPickDiag") << "AVPICK av=" << av->getID() << " gpu_mode=" << (S32)av->mGpuAnimMode
                                           << " hit_from_avatar=" << off << "m"
                                           << (off > 3.f ? " GHOST" : "") << LL_ENDL;
                }
            }
        }
    }
    LLPipeline::sPickAvatar = saved_pick_avatar;
    return taken;
}

void LLCleanInstanceStore::copyDirtyVK(std::vector<U8>& mirror, std::vector<std::pair<U32, U32>>& runs, U32 heal_mode,
                                       U32* live_out, VKFrameSync* sync_out)
{
    LLCrossingTrace::CopyProbe crossing_probe(runs, sizeof(Instance));   // #568: first call after an origin shift
    std::lock_guard<std::mutex> lk(mMutex);
    if (live_out) *live_out = mLiveCount;
    // #568 RENDER-WORLD FRAME. The mirror rows are store-frame copies; hand the renderer the
    // frame (upload writes row - d) and the agent shift its GPU rows still need. A pending
    // re-anchor moves the mirror like the store (O(N) adds; dirty rows are re-copied below
    // anyway). Without a sync_out (no consumer) the shift stays queued.
    if (mMirrorShiftPendingSet)
    {
        if (mirror.size() == mInstances.size() * sizeof(Instance))
        {
            const size_t rows = mirror.size() / sizeof(Instance);
            for (size_t i = 0; i < rows; ++i) translateRowBytes(mirror.data() + i * sizeof(Instance), mMirrorShiftPending);
        }
        mMirrorShiftPending[0] = mMirrorShiftPending[1] = mMirrorShiftPending[2] = 0.f;
        mMirrorShiftPendingSet = false;
    }
    if (sync_out)
    {
        for (int k = 0; k < 3; ++k)
        {
            sync_out->d[k] = mRenderD[k];
            sync_out->gpu_shift[k] += mGpuShiftPending[k];
            mGpuShiftPending[k] = 0.f;
        }
        sync_out->shifted = sync_out->gpu_shift[0] != 0.f || sync_out->gpu_shift[1] != 0.f || sync_out->gpu_shift[2] != 0.f;
    }
    // runs is APPENDED to (the caller clears it once uploaded), so dirt consumed on a
    // frame that doesn't record is still streamed next time.
    const U32 n = (U32)mInstances.size();
    const size_t bytes = (size_t)n * sizeof(Instance);
    if (mirror.size() != bytes)
    {
        mirror.resize(bytes);
        if (bytes) std::memcpy(mirror.data(), mInstances.data(), bytes);
        std::fill(mDirty.begin(), mDirty.end(), (U8)0);
        mDirtyMin = 0xFFFFFFFFu; mDirtyMax = 0;
        if (n) runs.emplace_back(0u, n);
        if (mChangeLogOn) { mChangeLog.clear(); mChangeLogFull = true; }   // (every row: a rescan)
        return;
    }
    auto copyRun = [&](U32 a, U32 b)
    {
        std::memcpy(mirror.data() + (size_t)a * sizeof(Instance), &mInstances[a],
                    (size_t)(b - a) * sizeof(Instance));
        runs.emplace_back(a, b);
    };
    // (the row change log: dirtied runs only, see setChangeLog)
    auto logRun = [&](U32 a, U32 b)
    {
        if (!mChangeLogOn || mChangeLogFull) return;
        if (mChangeLog.size() >= kChangeLogCap) { mChangeLog.clear(); mChangeLogFull = true; return; }
        mChangeLog.emplace_back(a, b);
    };
    if (mDirtyMax > mDirtyMin)
    {
        const U32 end = std::min(mDirtyMax, std::min(n, (U32)mDirty.size()));
        U32 i = mDirtyMin;
        while (i < end)
        {
            if (!mDirty[i]) { ++i; continue; }
            U32 j = i;
            // Coalesce: bridge gaps of up to 8 clean ids (one bigger copy beats
            // many tiny regions).
            U32 last_dirty = i;
            while (j < end && (mDirty[j] || j - last_dirty <= 8))
            {
                if (mDirty[j]) { mDirty[j] = 0; last_dirty = j; }
                ++j;
            }
            copyRun(i, last_dirty + 1);
            logRun(i, last_dirty + 1);
            i = last_dirty + 1;
        }
    }
    mDirtyMin = 0xFFFFFFFFu; mDirtyMax = 0;

    // Rolling self-heal slice (C8: every row it covers is re-streamed, re-copied to the
    // previous-instance store and re-visited by demand / material refs / RT downstream).
    static U32 s_cursor = 0;
    if (n && heal_mode != 0)
    {
        const U32 slice = heal_mode >= 2 ? std::max<U32>(64u, n / 64u) : std::max<U32>(16u, n / 256u);
        if (s_cursor >= n) s_cursor = 0;
        const U32 b = std::min(n, s_cursor + slice);
        copyRun(s_cursor, b);
        s_cursor = b;
    }
}

void LLCleanInstanceStore::setChangeLog(bool on)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (on == mChangeLogOn) return;
    mChangeLogOn = on;
    mChangeLog.clear();
    mChangeLog.shrink_to_fit();
    mChangeLogFull = true;   // (a consumer that just started knows nothing: rescan)
}

void LLCleanInstanceStore::takeChangeLog(std::vector<std::pair<U32, U32>>& out, bool& full)
{
    std::lock_guard<std::mutex> lk(mMutex);
    full = mChangeLogFull;
    mChangeLogFull = false;
    if (full) mChangeLog.clear();
    else out.insert(out.end(), mChangeLog.begin(), mChangeLog.end());
    mChangeLog.clear();   // (capacity kept: <= kChangeLogCap runs, 512 KiB)
}

bool LLCleanInstanceStore::findInstanceId(const LLUUID& object_uuid, S32 face_idx, U32& id_out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyHash.find(std::make_pair(object_uuid, face_idx));
    if (it == mKeyHash.end()) return false;
    id_out = it->second;
    return true;
}

bool LLCleanInstanceStore::keyForId(U32 id, LLUUID& object_uuid, S32& face_idx) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (id >= mIdToKey.size() || id >= mInstances.size()) return false;
    if (mInstances[id].lod_group_id == 0xFFFFFFFFu) return false;   // released slot
    object_uuid = mIdToKey[id].first;
    face_idx = mIdToKey[id].second;
    return object_uuid.notNull();
}
