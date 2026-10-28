/**
 * @file llcleananimstore.cpp
 * @brief VK GPU avatar animation: CPU half (see llcleananimstore.h).
 *
 * SEMANTICS PORTED (LL -> clean_anim.comp), all unchanged from LL:
 *  - Motion bookkeeping stays in LLMotionController (activation, ease in/out weights,
 *    loop time, stop/deactivate, joint signatures, LOD fade). Keyframe motions compute
 *    their looped time exactly as LLKeyframeMotion::onUpdate and, for a GPU-animated
 *    character, tag each joint state with (JointMotionList, joint motion index, time)
 *    instead of sampling (LLKeyframeMotion::applyKeyframes).
 *  - LLPoseBlender::addMotion still builds each joint's LLJointStateBlender slots (at most
 *    JSB_NUM_JOINT_STATES, ordered by priority descending, newer motions first within a
 *    priority, additive and regular motions in the same list). The slot lists are
 *    exported verbatim; blendJointStates runs on the GPU with LL's exact arithmetic:
 *    blend starts from the joint's CURRENT local transform (so un-animated components
 *    and joints keep their last value -- LL's "sticky" pose), additive states accumulate
 *    without advancing the weight sums, regular states nlerp/lerp by running weight
 *    ratio, final = (blended + added) with added_rot * blended_rot, non-finite guards as
 *    LLJoint::setPosition/setRotation/setScale.
 *  - Keyframe sampling: std::map lower_bound on key time, exact-key / before-first /
 *    past-last cases, IT_STEP -> previous key, else linear (nlerp for rotations).
 *  - Hierarchy: LLXformMatrix::update (child offset scaled by the parent's local scale --
 *    every LLJoint scales child offsets -- rotated by the parent's world rotation, world
 *    rotation = local * parent world), world matrix = LLMatrix4::initAll(local scale,
 *    world rotation, world position).
 * CPU-VALUED STATES (exported by value, blended on the GPU with the rest):
 *  - every non-keyframe motion (head/eye look-at, breathe, hand pose, editing/targeting
 *    arm, walk adjust, physics-free procedural motions);
 *  - keyframe motions with IK constraints (applyConstraints reads and rewrites the sampled
 *    states against world positions): sampled + constrained on the CPU, counted in
 *    LLKeyframeMotion::sGpuConstraintFallbacks;
 *  - the states LLKeyframeStandMotion (foot IK) and LLKeyframeFallMotion (pelvis) read or
 *    overwrite after sampling.
 * LOCAL WRITES FROM NON-ANIMATION CODE (visual params, avatar physics, joint position /
 * scale overrides, skeleton resets) are tracked per joint by LLJoint::mGpuExtWrite and
 * uploaded as base writes, applied before the blend exactly where LL would see them.
 * CPU CONSUMERS of joint transforms (procedural motions, head offset, footsteps, attachment
 * placement, nametags, picking, sounds) get a SPARSE readback, RING frames late: only
 * joints something reads come back -- a base set (pelvis, head, eyes, neck, hands, feet,
 * occupied attachment points) plus any joint whose getters ran recently (want bits set by
 * LLJoint's getters, aged out after kWantFrames unread), plus their parent chains. Each
 * record carries the local pose AND the world transform relative to the root the GPU
 * used; LLJoint composes the latter with the LIVE root (no root-motion lag, no parent
 * chain walk). A joint read for the first time returns its previous (stale) value until
 * its first record arrives (<= RING+1 frames). Rigid attachments are drawn from the GPU
 * joint table (no lag), and their instances skip the frustum test (flag 512), so the CPU
 * lag cannot cull them.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleananimstore.h"

#include "llcleanskinstore.h"
#include "llcontrolavatar.h"
#include "llavatarjoint.h"
#include "llviewerjointattachment.h"
#include "llvovolume.h"
#include "lldrawable.h"
#include "llkeyframemotion.h"
#include "fsposeranimator.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvkhitch.h"
#include "llvoavatar.h"

namespace
{
constexpr U32 kWantFrames = 600;     // a read joint stays in the readback set this long
constexpr U32 kBaseRebuild = 30;     // base set (occupied attachment points) refresh period
constexpr U32 kCmpArm = LLCleanAnimStore::RING + 3;   // full readback frames before a compare
const char* const kBaseJointNames[] = {
    "mPelvis", "mTorso", "mChest", "mNeck", "mHead", "mSkull", "mEyeLeft", "mEyeRight",
    "mWristLeft", "mWristRight", "mAnkleLeft", "mAnkleRight", "mFootLeft", "mFootRight" };
}

LLCleanAnimStore& LLCleanAnimStore::instance()
{
    static LLCleanAnimStore s;
    return s;
}

LLCleanAnimStore::LLCleanAnimStore()
{
    LLKeyframeMotion::JointMotionList::sOnDestroy = &LLCleanAnimStore::onListDestroyed;
    sAlive = true;
}

LLCleanAnimStore::~LLCleanAnimStore()
{
    sAlive = false;
    LLKeyframeMotion::JointMotionList::sOnDestroy = nullptr;
}

bool LLCleanAnimStore::enabled()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKGpuAnim", true);
    return s_on;
}

void LLCleanAnimStore::beginAvatar(LLVOAvatar* av)
{
    LLJoint::sFrameCounter = mFrame;
    if (!av) return;
    U8 mode = 0;
    U32 slot = 0xFFFFFFFFu;
    // A locally posed avatar (FS poser) stays on the CPU path: its pose is written by a
    // procedural motion every frame and the poser/gizmos read joints back immediately.
    if (enabled() && !av->isDead() && !av->isSelf() && !av->isUIAvatar() && av->mSpecialRenderMode == 0
        && !FSPoserAnimator::isAvatarPosedLocally(av))
    {
        bool own_animesh = false;
        if (av->isControlAvatar())
        {
            const LLVOAvatar* wearer = static_cast<LLControlAvatar*>(av)->getAttachedAvatar();
            own_animesh = wearer && wearer->isSelf();
        }
        if (!own_animesh)
        {
            // Only avatars with a joint-table slot have a GPU consumer for their pose; an
            // avatar with nothing rigged or worn stays on the CPU path.
            slot = LLCleanSkinStore::instance().findAvatarSlot(av->getID());
            if (slot != 0xFFFFFFFFu && slot < LLCleanSkinStore::MAX_AVATARS) mode = 2;
        }
    }

    auto it = mAvatars.find(av);
    if (mode == 0)
    {
        av->mGpuAnimMode = 0;
        av->mGpuAnimSink = nullptr;
        av->mGpuAnimIncremental = false;
        if (it != mAvatars.end())
        {
            it->second->mode = 0;
            it->second->seeded = false;   // re-seed from the CPU pose if it comes back
            if (it->second->bound && it->second->av == av) unbindJoints(*it->second);
        }
        return;
    }
    if (it == mAvatars.end() || it->second->id != av->getID())
    {
        auto st = std::make_unique<AvState>();
        st->store = this;
        st->av = av;
        st->id = av->getID();
        st->last_want.assign(MAX_JOINTS, 0u);
        it = mAvatars.insert_or_assign(av, std::move(st)).first;
    }
    AvState& st = *it->second;
    if (st.joints.empty())
    {
        st.joints.assign(MAX_JOINTS, nullptr);
        st.index.clear();
        for (U32 j = 0; j < MAX_JOINTS; ++j)
        {
            LLJoint* jp = av->getJoint((S32)j);
            st.joints[j] = jp;
            if (jp) st.index.emplace(jp, (U16)j);
        }
    }
    if (st.slot != slot)
    {
        st.slot = slot;
        st.seeded = false;
        st.topo_slot = 0xFFFFFFFFu;
    }

    // RenderVKAnimCompare: every 300 frames, up to 4 GPU avatars run in shadow mode (the
    // full LL CPU path AND the GPU path from the same pre-blend pose) for a diff.
    // The compared avatars are ARMED kCmpArm frames ahead: their readback covers every
    // joint until then, so the CPU local pose the compare frame blends from (and seeds the
    // GPU with) is the GPU's own, as with a full readback.
    static LLCachedControl<bool> s_cmp(gSavedSettings, "RenderVKAnimCompare", false);
    st.compare = false;
    const U32 phase = mFrame % 300u;
    if (phase == 300u - kCmpArm && mCmpArmFrame != mFrame) { mCmpArmFrame = mFrame; mCmpArmedCount = 0; }
    if (s_cmp && st.seeded && phase == 300u - kCmpArm && !st.cmp_armed && mCmpArmedCount < 4)
    {
        st.cmp_armed = true;
        ++mCmpArmedCount;
    }
    if (phase == 0u && st.cmp_armed)
    {
        st.cmp_armed = false;
        if (s_cmp && !mCmpPending && mCmpSlotsThisFrame.size() < 4)
        {
            mode = 1;
            st.compare = true;
            mCmpSlotsThisFrame.push_back(slot);   // reserved; captured in buildFrame
        }
    }
    else if (phase != 0u && phase < 300u - kCmpArm)
    {
        st.cmp_armed = false;   // missed its compare frame
    }

    // Mode 2: joints bound to the sparse readback. Mode 1 (the CPU is authoritative for
    // this frame): unbound, LL's own chain.
    if (mode == 2 && !st.bound) bindJoints(st);
    else if (mode != 2 && st.bound) unbindJoints(st);

    st.mode = mode;
    st.last_seen = mFrame;
    st.seed.clear();
    av->mGpuAnimMode = mode;
    av->mGpuAnimSink = &st;
    // #257: the motion controller defers addMotion and lets this state reuse its cached
    // export structure (mode 2 only; compare frames run LL's full path).
    av->mGpuAnimIncremental = mode == 2 && incrementalEnabled();
}

bool LLCleanAnimStore::gpuDriven(const LLVOAvatar* av) const
{
    return av && av->mGpuAnimMode == 2;
}

void LLCleanAnimStore::bindJoints(AvState& st)
{
    LLVOAvatar* av = st.av;
    LLJoint* root = av->getRootJoint();
    for (U32 w = 0; w < 7; ++w) { av->mGpuWantBits[w] = 0u; av->mGpuExtBits[w] = 0xFFFFFFFFu; }   // first frame: scan all
    for (U32 j = 0; j < MAX_JOINTS; ++j)
        if (LLJoint* jp = st.joints[j]) jp->gpuBind(root, av->mGpuWantBits, av->mGpuExtBits, (U16)j);
    st.bound = true;
    st.base_frame = 0;
}

void LLCleanAnimStore::unbindJoints(AvState& st)
{
    for (LLJoint* jp : st.joints)
        if (jp) jp->gpuUnbind();
    st.bound = false;
    st.refresh.clear();
    std::fill(st.last_want.begin(), st.last_want.end(), 0u);
}

void LLCleanAnimStore::rebuildBase(AvState& st)
{
    st.base.clear();
    st.refresh.clear();
    auto add = [&](LLJoint* jp) {
        if (!jp) return;
        auto it = st.index.find(jp);
        if (it == st.index.end()) return;
        st.base.push_back(it->second);
        st.refresh.push_back(jp);
    };
    for (const char* name : kBaseJointNames) add(st.av->getJoint(std::string_view(name)));
    // occupied attachment points: their LLXform children (the attached objects'
    // drawables) read the cached world transform directly, with no getter
    for (const auto& ap : st.av->mAttachmentPoints)
    {
        LLViewerJointAttachment* att = ap.second;
        if (att && !att->mAttachedObjects.empty()) add(att);
    }
    st.base_frame = mFrame;
}

void LLCleanAnimStore::forgetAvatar(LLVOAvatar* av)
{
    auto it = mAvatars.find(av);
    if (it == mAvatars.end()) return;
    const LLUUID id = it->second->id;
    // No unbind: the joints are being destroyed with the avatar.
    mAvatars.erase(it);
    // Pending compare references hold raw joint pointers of this avatar.
    for (auto c = mCmpRefs.begin(); c != mCmpRefs.end(); )
        c = (c->id == id) ? mCmpRefs.erase(c) : c + 1;
    // Readback records are validated by (id, pointer) + mAvatars membership, so an
    // erased state makes consumeReadback skip them.
}

void LLCleanAnimStore::refreshAvatar(LLVOAvatar* av)
{
    auto it = mAvatars.find(av);
    if (it == mAvatars.end() || it->second->av != av) return;
    it->second->inc_valid = false;
}

std::string LLCleanAnimStore::describeAvatar(const LLVOAvatar* av, bool* stale) const
{
    if (stale) *stale = false;
    if (!av) return "gpu{n/a}";
    const U32 skin_slot = LLCleanSkinStore::instance().findAvatarSlot(av->getID());
    std::string out = "gpu{enabled=" + std::to_string(enabled() ? 1 : 0)
        + " mode=" + std::to_string((U32)av->mGpuAnimMode)
        + " skin_slot=" + (skin_slot == 0xFFFFFFFFu ? std::string("none") : std::to_string(skin_slot));
    auto it = mAvatars.find(av);
    if (it == mAvatars.end() || it->second->av != av || it->second->id != av->getID())
        return out + " state=none}";
    const AvState& st = *it->second;
    if (stale) *stale = st.mode != 0 && (mFrame - st.last_seen) >= 600u;
    out += " slot=" + (st.slot == 0xFFFFFFFFu ? std::string("none") : std::to_string(st.slot))
        + " seeded=" + std::to_string(st.seeded ? 1 : 0)
        + " bound=" + std::to_string(st.bound ? 1 : 0)
        + " seen_ago=" + std::to_string(mFrame - st.last_seen)
        + " export_ago=" + (st.pend_frame == 0xFFFFFFFFu ? std::string("never") : std::to_string(mFrame - st.pend_frame))
        + " export_joints=" + std::to_string(st.pend.hdr.size() / 3)
        + " inc_valid=" + std::to_string(st.inc_valid ? 1 : 0) + "}";
    return out;
}

void LLCleanAnimStore::refreshWorld(LLVOAvatar* av)
{
    auto it = mAvatars.find(av);
    if (it == mAvatars.end()) return;
    AvState& st = *it->second;
    if (!st.bound || st.av != av) return;
    for (LLJoint* jp : st.refresh) jp->updateWorldMatrixParent();
}

void LLCleanAnimStore::captureLocals(AvState& st, std::vector<F32>& out) const
{
    out.assign((size_t)MAX_JOINTS * 12, 0.f);
    for (U32 j = 0; j < MAX_JOINTS; ++j)
    {
        LLJoint* jp = st.joints[j];
        if (!jp) continue;
        F32* o = &out[(size_t)j * 12];
        const LLXformMatrix* x = jp->getXform();   // (not the getters: no readback requests)
        const LLVector3& p = x->getPosition();
        const LLQuaternion& r = x->getRotation();
        const LLVector3& s = x->getScale();
        o[0] = p.mV[0]; o[1] = p.mV[1]; o[2] = p.mV[2];
        o[4] = r.mQ[0]; o[5] = r.mQ[1]; o[6] = r.mQ[2]; o[7] = r.mQ[3];
        o[8] = s.mV[0]; o[9] = s.mV[1]; o[10] = s.mV[2];
    }
}

bool LLCleanAnimStore::incrementalEnabled()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKAnimIncremental", true);
    return s_on;
}

bool LLCleanAnimStore::verifyEnabled()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKAnimIncrementalVerify", false);
    return s_on;
}

void LLCleanAnimStore::AvState::beginExport()
{
    t_export = std::chrono::steady_clock::now();
    export_incr = false;
    verifying = false;
    cap_mode = CAP_NONE;
    out = &pend;
    if (pend_frame != store->mFrame)
    {
        pend.clear();
        pend_frame = store->mFrame;
    }
    if (mode == 1)
    {
        // Shadow/compare: the GPU starts from exactly the local pose the CPU blend is
        // about to start from (post physics / visual params, pre blend).
        store->captureLocals(*this, seed);
        for (LLJoint* jp : joints)
            if (jp) jp->mGpuExtWrite = 0;
    }
}

void LLCleanAnimStore::AvState::endExport()
{
    cap_mode = CAP_NONE;
    out = &pend;
    verifying = false;
    if (mode != 2) return;
    const U32 us = (U32)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t_export).count();
    LLVKHitch::count("animExportUs", us);
    LLVKHitch::count(export_incr ? "av_animExportIncr" : "av_animExportFull");
    store->mSumExportUs += us;
    ++(export_incr ? store->mSumExportIncr : store->mSumExportFull);
}

U32 LLCleanAnimStore::AvState::jointIndex(const LLJoint* joint) const
{
    const S32 jn = joint->getJointNum();
    if (jn >= 0 && jn < (S32)MAX_JOINTS && jn < (S32)joints.size() && joints[jn] == joint)
        return (U32)jn;
    auto it = index.find(joint);
    return it != index.end() ? (U32)it->second : 0xFFFFFFFFu;
}

void LLCleanAnimStore::AvState::emit(Pending& dst, U32 ji, LLJointState* const* states,
                                     const bool* additive, S32 count)
{
    dst.hdr.push_back(ji);
    dst.hdr.push_back((U32)dst.blends.size());
    dst.hdr.push_back((U32)count);
    for (S32 s = 0; s < count; ++s)
    {
        const LLJointState* js = states[s];
        Pending::Blend b{};
        b.flags = ((js->getUsage() & 7u) << 1) | (additive[s] ? 16u : 0u);
        b.weight = js->getWeight();
        if (js->mGpuList)
        {
            b.flags |= 1u;
            b.list = js->mGpuList;
            b.index = (U32)js->mGpuIndex;
            b.time = js->mGpuTime;
        }
        else
        {
            b.list = nullptr;
            b.index = (U32)(dst.values.size() / 12);
            const LLVector3& p = js->getPosition();
            const LLQuaternion& r = js->getRotation();
            const LLVector3& sc = js->getScale();
            const F32 v[12] = { p.mV[0], p.mV[1], p.mV[2], 0.f,
                                r.mQ[0], r.mQ[1], r.mQ[2], r.mQ[3],
                                sc.mV[0], sc.mV[1], sc.mV[2], 0.f };
            dst.values.insert(dst.values.end(), v, v + 12);
        }
        dst.blends.push_back(b);
    }
}

void LLCleanAnimStore::AvState::exportJoint(LLJoint* joint, LLJointState* const* states,
                                            const bool* additive, S32 count)
{
    if (!joint || count <= 0) return;
    if (pend_frame != store->mFrame)
    {
        pend.clear();
        pend_frame = store->mFrame;
    }
    const U32 ji = jointIndex(joint);
    if (cap_mode != CAP_NONE)
    {
        // record every blender LL exported -- out-of-table joints too: their
        // notePosAnimated side effect must be replayed on cached frames
        IncCall c{};
        c.joint = joint;
        c.ji = ji;
        c.count = llmin(count, JSB_NUM_JOINT_STATES);
        for (S32 s = 0; s < c.count; ++s) { c.states[s] = states[s]; c.additive[s] = additive[s]; }
        (cap_mode == CAP_VERIFY ? v_calls : inc_calls).push_back(c);
    }
    if (ji == 0xFFFFFFFFu) return;   // not in the joint table (no GPU consumer)
    emit(out ? *out : pend, ji, states, additive, count);
}

// Validate the cached structure against the live motions / pose states; on success,
// replay the export from it (weights, times and procedural values read live). Nothing
// cached is dereferenced before it is proven equal to a live pointer.
bool LLCleanAnimStore::AvState::exportCached(LLMotion* const* motions, size_t count)
{
    if (mode != 2 || !incrementalEnabled()) return false;
    // (one literal per counter: LLVKHitch keys slots by name pointer)
    static const char* const kMissMotions = "animIncrMissMotions";
    const char* miss = nullptr;
    if (!inc_valid) miss = "animIncrMissCold";
    else if (count != inc_motions.size()) miss = kMissMotions;
    else
    {
        for (size_t i = 0; i < count && !miss; ++i)
        {
            LLMotion* m = motions[i];
            const IncMotion& cm = inc_motions[i];
            if (m != cm.m || (S32)m->getPriority() != cm.prio || (S32)m->getBlendType() != cm.blend)
            {
                miss = kMissMotions;
                break;
            }
            const std::vector<LLJointState*>& v = m->getPose()->getJointStates();
            if (v.size() != cm.n) { miss = "animIncrMissPose"; break; }
            const IncState* cs = inc_states.data() + cm.first;
            for (size_t k = 0; k < v.size(); ++k)
            {
                const LLJointState* js = v[k];
                const IncState& c = cs[k];
                if (js != c.js || js->getJoint() != c.joint || js->mGpuList != c.gpu_list
                    || js->mGpuIndex != c.gpu_index || js->getUsage() != c.usage
                    || (S32)js->getPriority() != c.prio)
                {
                    miss = "animIncrMissState";
                    break;
                }
            }
        }
    }
    if (miss)
    {
        LLVKHitch::count(miss);
        inc_valid = false;
        return false;
    }

    // Verify (RenderVKAnimIncrementalVerify): ~1/8 of the cached exports each frame
    // (rotating by slot) also run the full LL path; both go to scratch, are compared,
    // and the full one is kept.
    verifying = verifyEnabled() && ((store->mFrame + slot) % 8u) == 0u;
    Pending& dst = verifying ? v_fast : pend;
    if (verifying) v_fast.clear();
    for (const IncCall& c : inc_calls)
    {
        // LLJointStateBlender::notePosAnimated (#207): a non-additive state with weight
        // drives the joint's position this evaluation -- weight-dependent, per frame.
        for (S32 s = 0; s < c.count; ++s)
        {
            const LLJointState* js = c.states[s];
            if (!c.additive[s] && js->getWeight() > 0.f && (js->getUsage() & LLJointState::POS))
            {
                c.joint->notePosAnimated();
                break;
            }
        }
        if (c.ji != 0xFFFFFFFFu) emit(dst, c.ji, c.states, c.additive, c.count);
    }
    export_incr = true;
    return true;
}

void LLCleanAnimStore::AvState::beginCapture(LLMotion* const* motions, size_t count, bool verify)
{
    if (verify)
    {
        cap_mode = CAP_VERIFY;
        v_calls.clear();
        v_full.clear();
        out = &v_full;
        return;
    }
    // New structure: the validation key, snapshotted after the deferred addMotion calls
    // (nothing changes the motions or their poses before exportAndClear).
    cap_mode = CAP_STRUCTURE;
    out = &pend;
    inc_calls.clear();
    inc_motions.clear();
    inc_states.clear();
    for (size_t i = 0; i < count; ++i)
    {
        LLMotion* m = motions[i];
        const std::vector<LLJointState*>& v = m->getPose()->getJointStates();
        inc_motions.push_back({ m, (S32)m->getPriority(), (S32)m->getBlendType(),
                                (U32)inc_states.size(), (U32)v.size() });
        for (const LLJointState* js : v)
            inc_states.push_back({ js, js->getJoint(), js->mGpuList, js->mGpuIndex,
                                   js->getUsage(), (S32)js->getPriority() });
    }
}

void LLCleanAnimStore::AvState::endCapture()
{
    if (cap_mode == CAP_STRUCTURE)
    {
        inc_valid = mode == 2;
    }
    else if (cap_mode == CAP_VERIFY)
    {
        verifyCompare();
    }
    cap_mode = CAP_NONE;
    out = &pend;
}

// Append src to dst (src's blend / value indices are relative to src).
void LLCleanAnimStore::appendPending(Pending& dst, const Pending& src)
{
    const U32 b0 = (U32)dst.blends.size();
    const U32 v0 = (U32)(dst.values.size() / 12);
    for (size_t h = 0; h + 2 < src.hdr.size(); h += 3)
    {
        dst.hdr.push_back(src.hdr[h]);
        dst.hdr.push_back(src.hdr[h + 1] + b0);
        dst.hdr.push_back(src.hdr[h + 2]);
    }
    for (Pending::Blend b : src.blends)
    {
        if (!(b.flags & 1u)) b.index += v0;
        dst.blends.push_back(b);
    }
    dst.values.insert(dst.values.end(), src.values.begin(), src.values.end());
}

void LLCleanAnimStore::AvState::verifyCompare()
{
    LLCleanAnimStore& s = *store;
    ++s.mSumVerify;
    LLVKHitch::count("animVerify");
    std::string what;
    // 1. the blender calls (joint, states, additive flags, order)
    if (v_calls.size() != inc_calls.size())
    {
        what = "call count " + std::to_string(inc_calls.size()) + " cached vs " + std::to_string(v_calls.size()) + " full";
    }
    else
    {
        for (size_t i = 0; i < v_calls.size() && what.empty(); ++i)
        {
            const IncCall& a = inc_calls[i];
            const IncCall& b = v_calls[i];
            bool same = a.joint == b.joint && a.ji == b.ji && a.count == b.count;
            for (S32 k = 0; same && k < a.count; ++k)
                same = a.states[k] == b.states[k] && a.additive[k] == b.additive[k];
            if (!same)
                what = "call " + std::to_string(i) + " joint " + (b.joint ? b.joint->getName() : std::string("?"))
                     + " (states/order/additive differ)";
        }
    }
    // 2. the emitted blend inputs, bitwise
    if (what.empty())
    {
        if (v_fast.hdr != v_full.hdr) what = "joint headers differ";
        else if (v_fast.blends.size() != v_full.blends.size()) what = "blend count differs";
        else if (v_fast.values.size() != v_full.values.size()) what = "value count differs";
        else
        {
            for (size_t i = 0; i < v_fast.blends.size() && what.empty(); ++i)
            {
                const Pending::Blend& a = v_fast.blends[i];
                const Pending::Blend& b = v_full.blends[i];
                if (a.flags != b.flags || a.list != b.list || a.index != b.index
                    || std::memcmp(&a.weight, &b.weight, sizeof(F32)) || std::memcmp(&a.time, &b.time, sizeof(F32)))
                    what = "blend " + std::to_string(i) + " differs (flags/weight/source/time)";
            }
            if (what.empty() && !v_fast.values.empty()
                && std::memcmp(v_fast.values.data(), v_full.values.data(), v_fast.values.size() * sizeof(F32)))
                what = "procedural values differ";
        }
    }
    if (!what.empty())
    {
        ++s.mSumVerifyBad;
        LLVKHitch::count("animVerifyMismatch");
        inc_valid = false;   // re-capture next update
        const F64 now = LLTimer::getTotalSeconds();
        if (now - s.mVerifyLogTime >= 1.0)
        {
            s.mVerifyLogTime = now;
            LL_WARNS("VKAnimIncr") << "INCREMENTAL EXPORT MISMATCH avatar " << id << " slot " << slot << ": " << what
                << " (verified " << s.mSumVerify << ", mismatched " << s.mSumVerifyBad << " in this window)" << LL_ENDL;
        }
    }
    // the full export is authoritative
    appendPending(pend, v_full);
}

void LLCleanAnimStore::buildTopology(AvState& st, U32 slot)
{
    if (mTopo.size() < (size_t)(slot + 1) * MAX_JOINTS)
        mTopo.resize((size_t)(slot + 1) * MAX_JOINTS, 0xFFFEu);
    LLJoint* root = st.av->getRootJoint();
    U32 parent[MAX_JOINTS];
    S32 depth[MAX_JOINTS];
    for (U32 j = 0; j < MAX_JOINTS; ++j)
    {
        depth[j] = -1;
        LLJoint* jp = st.joints[j];
        if (!jp) { parent[j] = 0xFFFEu; continue; }
        LLJoint* pj = jp->getParent();
        if (!pj || pj == root) { parent[j] = 0xFFFFu; continue; }
        auto it = st.index.find(pj);
        parent[j] = (it != st.index.end()) ? it->second : 0xFFFFu;   // outside the table: treat as root child
        if (it == st.index.end())
            LL_WARNS_ONCE("VKAnim") << "joint " << jp->getName() << " has a parent outside the joint table" << LL_ENDL;
    }
    U32 maxd = 0;
    for (U32 j = 0; j < MAX_JOINTS; ++j)
    {
        if (parent[j] == 0xFFFEu) continue;
        // walk up to a joint with a known depth (skeleton depth is small)
        U32 chain[MAX_JOINTS]; U32 n = 0; U32 k = j;
        while (depth[k] < 0 && parent[k] != 0xFFFFu && parent[k] != 0xFFFEu && n < MAX_JOINTS)
        {
            chain[n++] = k;
            k = parent[k];
        }
        S32 d = (depth[k] >= 0) ? depth[k] : 0;
        if (depth[k] < 0) depth[k] = 0;      // k is a root child
        while (n > 0) { k = chain[--n]; depth[k] = ++d; }
        maxd = llmax(maxd, (U32)depth[j]);
    }
    for (U32 j = 0; j < MAX_JOINTS; ++j)
    {
        mTopo[(size_t)slot * MAX_JOINTS + j] = (parent[j] == 0xFFFEu)
            ? 0xFFFEu : (parent[j] | ((U32)llmax(depth[j], 0) << 16));
    }
    st.max_depth = maxd;
    st.topo_slot = slot;
    mTopoDirty = true;
}

S32 LLCleanAnimStore::ensureUploaded(LLKeyframeMotion::JointMotionList* list)
{
    if (!list) return -1;
    if (list->mGpuBase >= 0)
    {
        // Liveness stamp, sampled (a hash lookup per blend per frame adds up in a crowd).
        if ((mFrame & 63u) == 0u)
        {
            auto it = mLists.find(list);
            if (it != mLists.end()) it->second.last_used = mFrame;
        }
        return list->mGpuBase;
    }
    ListRange r{};
    r.last_used = mFrame;
    r.desc_first = (U32)(mDesc.size() / 8);
    r.key_first = (U32)mKeyTimes.size();
    const U32 nm = list->getNumJointMotions();
    for (U32 i = 0; i < nm; ++i)
    {
        const LLKeyframeMotion::JointMotion* jm = list->getJointMotion(i);
        const U32 rot_off = (U32)mKeyTimes.size();
        for (const auto& kv : jm->mRotationCurve.mKeys)
        {
            mKeyTimes.push_back(kv.first);
            const LLQuaternion& q = kv.second.mRotation;
            mKeyVals.insert(mKeyVals.end(), { q.mQ[0], q.mQ[1], q.mQ[2], q.mQ[3] });
        }
        const U32 rot_n = (U32)mKeyTimes.size() - rot_off;
        const U32 pos_off = (U32)mKeyTimes.size();
        for (const auto& kv : jm->mPositionCurve.mKeys)
        {
            mKeyTimes.push_back(kv.first);
            const LLVector3& p = kv.second.mPosition;
            mKeyVals.insert(mKeyVals.end(), { p.mV[0], p.mV[1], p.mV[2], 0.f });
        }
        const U32 pos_n = (U32)mKeyTimes.size() - pos_off;
        const U32 d[8] = { rot_off, rot_n, (U32)jm->mRotationCurve.mInterpolationType, 0u,
                           pos_off, pos_n, (U32)jm->mPositionCurve.mInterpolationType, 0u };
        mDesc.insert(mDesc.end(), d, d + 8);
    }
    r.desc_count = nm;
    r.key_count = (U32)mKeyTimes.size() - r.key_first;
    mLists[list] = r;
    list->mGpuBase = (S32)r.desc_first;
    // dirty runs (arena is append-only between compactions)
    const U32 dlo = r.desc_first * 2, dhi = (r.desc_first + nm) * 2;   // uvec4 units
    if (mArenaDirty.desc_lo >= mArenaDirty.desc_hi) { mArenaDirty.desc_lo = dlo; mArenaDirty.desc_hi = dhi; }
    else { mArenaDirty.desc_lo = llmin(mArenaDirty.desc_lo, dlo); mArenaDirty.desc_hi = llmax(mArenaDirty.desc_hi, dhi); }
    const U32 klo = r.key_first, khi = r.key_first + r.key_count;
    if (mArenaDirty.key_lo >= mArenaDirty.key_hi) { mArenaDirty.key_lo = klo; mArenaDirty.key_hi = khi; }
    else { mArenaDirty.key_lo = llmin(mArenaDirty.key_lo, klo); mArenaDirty.key_hi = llmax(mArenaDirty.key_hi, khi); }
    return list->mGpuBase;
}

void LLCleanAnimStore::onListDestroyed(LLKeyframeMotion::JointMotionList* list)
{
    LLCleanAnimStore& s = instance();
    auto it = s.mLists.find(list);
    if (it != s.mLists.end())
    {
        s.mDescGarbage += it->second.desc_count;
        s.mKeyGarbage += it->second.key_count;
        s.mLists.erase(it);
    }
    s.mDeadLists.insert(list);
}

void LLCleanAnimStore::compactArena()
{
    std::vector<LLKeyframeMotion::JointMotionList*> live;
    live.reserve(mLists.size());
    for (auto& kv : mLists)
    {
        auto* l = const_cast<LLKeyframeMotion::JointMotionList*>(kv.first);
        l->mGpuBase = -1;
        live.push_back(l);
    }
    mLists.clear();
    mDesc.clear(); mKeyTimes.clear(); mKeyVals.clear();
    mDescGarbage = mKeyGarbage = 0;
    for (auto* l : live) ensureUploaded(l);
    mArenaDirty = ArenaDirty{};
    mArenaDirty.full = true;
    LL_INFOS("VKAnim") << "keyframe arena compacted: " << live.size() << " animations, "
        << mKeyTimes.size() << " keys" << LL_ENDL;
}

LLCleanAnimStore::ArenaDirty LLCleanAnimStore::takeArenaDirty()
{
    ArenaDirty d = mArenaDirty;
    mArenaDirty = ArenaDirty{};
    return d;
}

void LLCleanAnimStore::consumeReadback(U32 ring, const F32* records, size_t floats)
{
    if (ring >= RING) return;
    std::vector<RbRecord>& recs = mRbRecords[ring];
    const std::vector<U16>& req = mRbReq[ring];
    if (!records) { recs.clear(); return; }
    for (const RbRecord& rec : recs)
    {
        LLViewerObject* o = gObjectList.findObject(rec.id);
        if (!o || o != (LLViewerObject*)rec.av || o->isDead()) continue;
        auto it = mAvatars.find(rec.av);
        if (it == mAvatars.end()) continue;
        AvState& st = *it->second;
        // CPU-evaluated now (its locals are authoritative again), or a compare frame
        // whose CPU pose must stay exactly LL's until captured.
        if (st.mode != 2 || !st.bound) continue;
        if ((size_t)(rec.first + rec.count) * RB_RECORD_FLOATS > floats) continue;
        if ((size_t)rec.req_off + rec.count > req.size()) continue;
        const F32* base = records + (size_t)rec.first * RB_RECORD_FLOATS;
        for (U32 k = 0; k < rec.count; ++k)
        {
            LLJoint* jp = st.joints[req[rec.req_off + k]];
            if (!jp) continue;
            // A local written by non-animation code after that frame is newer than the
            // readback (the GPU has it; a later readback carries it): keep it.
            const bool write_local = !(jp->mGpuExtWrite || jp->mGpuExtFrame > rec.frame);
            const F32* r = base + (size_t)k * RB_RECORD_FLOATS;
            jp->setFromGpu(r, r + 12, write_local, rec.frame);
        }
    }
    recs.clear();
}

U32 LLCleanAnimStore::buildFrame(const std::vector<std::pair<LLUUID, U32>>& slots)
{
    LLCleanSkinStore& sk = LLCleanSkinStore::instance();
    sk.clearGpuSlots();
    mRing = mFrame % RING;
    mRbRecords[mRing].clear();
    mRbReq[mRing].clear();
    mStatWantAdds = mStatWantExpires = 0;
    U32 rb_avatars = 0;
    mEntryCount = 0;
    mReadbackCount = 0;
    mStatGpu = 0;
    // (mCmpRefs survive until checkCompare consumes them, 4+ frames later.)
    if (!mCmpPending) mCmpRefs.clear();
    std::vector<U32> cmp_reserved;
    cmp_reserved.swap(mCmpSlotsThisFrame);

    // keyframe arena compaction when half of it is dead
    if (mKeyGarbage > 65536 && mKeyGarbage * 2 > (U32)mKeyTimes.size())
        compactArena();

    // Skin cache (#31): every slot dirty unless proven clean below.
    mSlotClean.assign(LLCleanSkinStore::MAX_AVATARS, 0u);
    mSlotWorldClean.assign(LLCleanSkinStore::MAX_AVATARS, 0u);

    // C11: per-frame scratch kept across frames (capacity reused; buildFrame runs on
    // the main thread only) -- six fresh vectors per frame were reallocated as they grew.
    static std::vector<U32> s_ent, s_hdr, s_blend, s_basew, s_reqw;
    static std::vector<F32> s_values;
    std::vector<U32>& ent = s_ent;     ent.clear();
    std::vector<U32>& hdr = s_hdr;     hdr.clear();
    std::vector<U32>& blend = s_blend; blend.clear();
    std::vector<U32>& basew = s_basew; basew.clear();
    std::vector<U32>& reqw = s_reqw;   reqw.clear();
    std::vector<F32>& values = s_values; values.clear();
    ent.reserve(slots.size() * ENTRY_WORDS);
    U32 n_blend_states = 0;

    // ATTACHED-ANIMESH FOLLOW. An animesh worn as an attachment is rooted at its
    // wearer's attachment point (LLControlAvatar::matchVolumeTransform). For a
    // GPU-animated wearer the CPU's copy of that joint is the sparse readback -- its
    // pose relative to the root is RING frames old -- so a root built on the CPU trails
    // the body by ~3 frames in fast animations, while the wearer's rigid attachments
    // (drawn from the GPU joint table) do not. Instead the root is composed ON THE GPU
    // from the wearer's joint evaluated this frame: follow entries are packed last and
    // run in a second dispatch (LLCleanRendererVK::recordAnim).
    static LLCachedControl<bool> s_follow(gSavedSettings, "RenderVKAnimeshAttachFollow", true);
    static LLCachedControl<bool> s_follow_dbg(gSavedSettings, "RenderVKAnimeshAttachDebug", false);
    static std::vector<U32> s_fent;
    static std::vector<std::pair<U32, U32>> s_follows;   // (slot, wearer slot)
    std::vector<U32>& fent = s_fent; fent.clear();
    s_follows.clear();
    static std::vector<U8> s_listed;
    s_listed.assign(LLCleanSkinStore::MAX_AVATARS, 0u);
    for (const auto& sl : slots)
        if (sl.second < LLCleanSkinStore::MAX_AVATARS) s_listed[sl.second] = 1u;
    // the eval predicate below, for an avatar other than the entry being built
    auto evalAvatar = [&](LLVOAvatar* w, U32 wslot) -> bool {
        auto it = mAvatars.find(w);
        if (it == mAvatars.end() || it->second->id != w->getID()) return false;
        const AvState& ws = *it->second;
        return enabled() && ws.mode != 0 && ws.slot == wslot && (mFrame - ws.last_seen) < 600u;
    };
    const bool dbg_tick = s_follow_dbg && (mFrame % 120u) == 0u;

    for (const auto& sl : slots)
    {
        const U32 slot = sl.second;
        if (slot >= LLCleanSkinStore::MAX_AVATARS) continue;
        U32 e[ENTRY_WORDS] = {};
        e[0] = slot; e[1] = MAX_JOINTS; e[2] = 0; e[3] = 0xFFFFFFFFu;
        e[20] = 0; e[21] = 0;
        e[22] = 0xFFFFFFFFu;

        LLViewerObject* o = gObjectList.findObject(sl.first);
        LLVOAvatar* av = o ? o->asAvatar() : nullptr;
        AvState* st = nullptr;
        if (av && !av->isDead())
        {
            auto it = mAvatars.find(av);
            if (it != mAvatars.end() && it->second->id == sl.first) st = it->second.get();
        }
        // GPU-evaluated: in GPU/shadow mode and seen by the motion update recently (an
        // avatar the viewer stops updating keeps its last GPU pose, re-rooted).
        const bool eval = enabled() && st && st->mode != 0 && st->slot == slot
                          && (mFrame - st->last_seen) < 600u;

        // Attached animesh: follow its wearer's GPU-evaluated attachment joint.
        U32 follow_wslot = 0xFFFFFFFFu;
        if (av && !av->isDead() && av->isControlAvatar())
        {
            LLControlAvatar* cav = static_cast<LLControlAvatar*>(av);
            cav->mVKAttachGpuFollow = false;
            cav->mVKAttachRbAge = -1;
            LLVOAvatar* wearer = cav->getAttachedAvatar();
            LLVOVolume* rootv = cav->mRootVolp;
            LLViewerJointAttachment* att = nullptr;
            if (wearer && !wearer->isDead() && rootv && rootv->mDrawable.notNull())
                att = wearer->getTargetAttachmentPoint(rootv);
            bool wearer_gpu = false;
            if (att)
            {
                if (att->gpuRelUsable()) cav->mVKAttachRbAge = (S32)(mFrame - att->mGpuRelFrame);
                const U32 wslot = LLCleanSkinStore::instance().findAvatarSlot(wearer->getID());
                const S32 jn = att->getJointNum();
                wearer_gpu = wslot < LLCleanSkinStore::MAX_AVATARS && s_listed[wslot] && evalAvatar(wearer, wslot);
                if (s_follow && eval && st->mode == 2 && wearer_gpu && wslot != slot && jn >= 0 && (U32)jn < MAX_JOINTS
                    && wearer->getJoint(jn) == static_cast<LLJoint*>(att))
                {
                    // matchVolumeTransform: root = obj_pos * joint_rot + joint_pos,
                    // obj_rot * joint_rot (the root volume's drawable, local to the point)
                    const LLVector3 lp = rootv->mDrawable->getPosition();
                    const LLQuaternion lr = rootv->mDrawable->getRotation();
                    e[22] = wslot;
                    e[23] = (U32)jn;
                    std::memcpy(&e[24], lp.mV, 3 * sizeof(F32));
                    std::memcpy(&e[28], lr.mQ, 4 * sizeof(F32));
                    follow_wslot = wslot;
                    cav->mVKAttachGpuFollow = true;
                }
            }
            if (dbg_tick && wearer)
            {
                LL_INFOS("VKAnim") << "animesh attach align: " << cav->getID() << " wearer " << wearer->getFullname()
                    << " attach " << (att ? att->getName() : std::string("none"))
                    << " wearer_gpu " << (wearer_gpu ? 1 : 0) << " animesh_gpu " << (eval ? 1 : 0)
                    << " gpu_follow " << (cav->mVKAttachGpuFollow ? 1 : 0)
                    << " order_lag " << cav->mVKAttachOrderLag
                    << " cpu_attach_rb_age " << cav->mVKAttachRbAge
                    << (cav->mVKAttachGpuFollow ? " (root this frame, GPU)"
                        : (cav->mVKAttachRbAge > 0 ? " (root from CPU readback: LAGS by rb_age frames)"
                                                   : " (root from CPU pose)"))
                    << LL_ENDL;
            }
        }

        if (!eval)
        {
            ent.insert(ent.end(), e, e + ENTRY_WORDS);
            ++mEntryCount;
            continue;
        }
        const bool topo_new = st->topo_slot != slot;
        if (topo_new) buildTopology(*st, slot);
        const bool seeding = !st->seeded || st->mode == 1;
        e[2] = 1;

        // BASE WRITES
        const U32 bw_first = (U32)(basew.size() / 12);
        auto push_base = [&](U32 j, U32 mask, const F32* v) {
            U32 w[12];
            w[0] = j | (mask << 16);
            std::memcpy(&w[1], &v[0], 3 * sizeof(F32));    // pos
            std::memcpy(&w[4], &v[4], 4 * sizeof(F32));    // rot
            std::memcpy(&w[8], &v[8], 3 * sizeof(F32));    // scale
            w[11] = 0;
            basew.insert(basew.end(), w, w + 12);
        };
        if (!st->seeded || st->mode == 1)
        {
            // full seed: the shadow-mode pre-blend snapshot, else the CPU's current pose
            std::vector<F32> snap;
            const std::vector<F32>* src = &st->seed;
            if (st->seed.size() != (size_t)MAX_JOINTS * 12) { captureLocals(*st, snap); src = &snap; }
            for (U32 j = 0; j < MAX_JOINTS; ++j)
            {
                LLJoint* jp = st->joints[j];
                if (!jp) continue;
                push_base(j, 7u, &(*src)[(size_t)j * 12]);
                if (st->mode != 1) jp->mGpuExtWrite = 0;   // shadow mode cleared them at the snapshot
            }
            st->seeded = true;
            if (st->bound) for (U32 w = 0; w < 7; ++w) av->mGpuExtBits[w] = 0u;
        }
        else
        {
            F32 v[12];
            auto base_one = [&](U32 j) {
                LLJoint* jp = st->joints[j];
                if (!jp || !jp->mGpuExtWrite) return;
                const LLXformMatrix* x = jp->getXform();   // (not the getters: no readback requests)
                const LLVector3& p = x->getPosition();
                const LLQuaternion& r = x->getRotation();
                const LLVector3& s = x->getScale();
                v[0] = p.mV[0]; v[1] = p.mV[1]; v[2] = p.mV[2]; v[3] = 0.f;
                v[4] = r.mQ[0]; v[5] = r.mQ[1]; v[6] = r.mQ[2]; v[7] = r.mQ[3];
                v[8] = s.mV[0]; v[9] = s.mV[1]; v[10] = s.mV[2]; v[11] = 0.f;
                push_base(j, jp->mGpuExtWrite & 7u, v);
                jp->mGpuExtWrite = 0;
            };
            if (st->bound)
            {
                // only the joints a non-animation write flagged (LLJoint ext bits) -- no
                // per-frame skeleton scan
                for (U32 w = 0; w < 7; ++w)
                {
                    U32 bits = av->mGpuExtBits[w];
                    av->mGpuExtBits[w] = 0u;
                    while (bits)
                    {
                        const U32 j = w * 32u + (U32)__builtin_ctz(bits);
                        bits &= bits - 1u;
                        if (j < MAX_JOINTS) base_one(j);
                    }
                }
            }
            else
            {
                for (U32 j = 0; j < MAX_JOINTS; ++j) base_one(j);
            }
        }
        e[6] = bw_first;
        e[7] = (U32)(basew.size() / 12) - bw_first;

        // BLENDS exported by this frame's motion update
        const U32 h_first = (U32)(hdr.size() / 4);
        U32 n_missing = 0;   // keyframe blends whose motion data is not on the GPU (weight 0 below)
        if (st->pend_frame == mFrame)
        {
            const U32 value_first = (U32)(values.size() / 12);
            values.insert(values.end(), st->pend.values.begin(), st->pend.values.end());
            const U32 blend_first = (U32)(blend.size() / 4);
            for (const Pending::Blend& b : st->pend.blends)
            {
                U32 w[4];
                w[0] = b.flags;
                F32 weight = b.weight;
                U32 src = 0;
                if (b.flags & 1u)
                {
                    S32 base = -1;
                    if (mDeadLists.empty() || !mDeadLists.count(b.list))
                        base = ensureUploaded(static_cast<LLKeyframeMotion::JointMotionList*>(const_cast<void*>(b.list)));
                    if (base < 0) { weight = 0.f; src = 0; ++n_missing; }
                    else src = (U32)base + b.index;
                }
                else
                {
                    src = value_first + b.index;
                }
                std::memcpy(&w[1], &weight, sizeof(F32));
                w[2] = src;
                std::memcpy(&w[3], &b.time, sizeof(F32));
                blend.insert(blend.end(), w, w + 4);
            }
            // ONE header per joint (#238): clean_anim.comp blends each header on its own
            // thread, read-modify-writing the joint's shared local pose, so two headers
            // for one joint race. A second motion update in the same frame (a forced
            // update) exports the joint again; keep the LAST export -- what the CPU's
            // sequential re-blend ends on -- and drop the earlier ones entirely.
            U32 last[MAX_JOINTS];
            std::fill(std::begin(last), std::end(last), 0xFFFFFFFFu);
            for (size_t h = 0; h + 2 < st->pend.hdr.size(); h += 3)
                if (st->pend.hdr[h] < MAX_JOINTS) last[st->pend.hdr[h]] = (U32)h;
            for (size_t h = 0; h + 2 < st->pend.hdr.size(); h += 3)
            {
                const U32 ji = st->pend.hdr[h];
                if (ji < MAX_JOINTS && last[ji] != (U32)h) continue;
                const U32 w[4] = { ji, blend_first + st->pend.hdr[h + 1], st->pend.hdr[h + 2], 0u };
                hdr.insert(hdr.end(), w, w + 4);
            }
            n_blend_states += (U32)st->pend.blends.size();
            // (2026-10-22) per-avatar GPU animation data loss / restore, timestamped by the log:
            // a played motion whose keyframes the GPU arena does not hold (destroyed list, or not
            // uploadable) blends at weight 0 -- the avatar loses that motion's pose on the GPU.
            if (n_missing && !st->gpu_missing)
                LL_INFOS("VKAnim") << "GPU animation data MISSING for avatar " << st->id << " (slot " << slot << "): "
                                   << n_missing << " of " << st->pend.blends.size()
                                   << " exported blends have no keyframes on the GPU (held at weight 0), frame " << mFrame
                                   << LL_ENDL;
            else if (!n_missing && st->gpu_missing)
                LL_INFOS("VKAnim") << "GPU animation data restored for avatar " << st->id << " (slot " << slot << "), frame "
                                   << mFrame << LL_ENDL;
            st->gpu_missing = n_missing != 0;
        }
        e[4] = h_first;
        e[5] = (U32)(hdr.size() / 4) - h_first;

        // ROOT (mRoot has no parent: its world transform is its local one)
        LLJoint* root = av->getRootJoint();
        const LLVector3 rp = root->getWorldPosition();
        const LLQuaternion rr = root->getWorldRotation();
        const LLVector3 rs = root->getScale();
        std::memcpy(&e[8], rp.mV, 3 * sizeof(F32));
        std::memcpy(&e[12], rr.mQ, 4 * sizeof(F32));
        std::memcpy(&e[16], rs.mV, 3 * sizeof(F32));
        e[19] = st->max_depth;
        // Skin cache (#31): the GPU only re-roots this slot's held local pose.
        {
            const bool root_scale_same = rs == st->last_root_scale;
            st->last_root_scale = rs;
            mSlotClean[slot] = (!topo_new && !seeding && e[5] == 0u && e[7] == 0u && root_scale_same) ? 1u : 0u;
            // RT refit gate: + the root did not move or turn since the previous frame.
            const bool root_same = st->last_root_frame + 1u == mFrame && rp == st->last_root_pos
                                   && rr == st->last_root_rot;
            st->last_root_pos = rp; st->last_root_rot = rr; st->last_root_frame = mFrame;
            mSlotWorldClean[slot] = (mSlotClean[slot] && root_same) ? 1u : 0u;
            // A follow entry's root is the wearer's joint (this frame, on the GPU) x the
            // local offset: world-clean needs the same link + offset as the previous
            // frame here, and the wearer world-clean (ANDed after the loop).
            const U32 fkey = follow_wslot == 0xFFFFFFFFu ? 0xFFFFFFFFu : (follow_wslot | (e[23] << 16));
            LLVector3 fp; LLQuaternion fr;
            if (fkey != 0xFFFFFFFFu) { std::memcpy(fp.mV, &e[24], 3 * sizeof(F32)); std::memcpy(fr.mQ, &e[28], 4 * sizeof(F32)); }
            const bool follow_same = st->follow_key == fkey && st->follow_frame + 1u == mFrame
                                     && (fkey == 0xFFFFFFFFu || (fp == st->follow_pos && fr == st->follow_rot));
            st->follow_key = fkey; st->follow_frame = mFrame; st->follow_pos = fp; st->follow_rot = fr;
            if (fkey != 0xFFFFFFFFu && !follow_same) mSlotWorldClean[slot] = 0u;
        }

        // READBACK (CPU consumers): sparse -- the base set, every joint read within
        // kWantFrames (LLJoint want bits), their parent chains; everything while armed
        // for a compare. Mode 1 (shadow) frames: none (the CPU pose is LL's own).
        if (st->mode == 2 && st->bound)
        {
            if (st->base_frame == 0 || mFrame - st->base_frame >= kBaseRebuild) rebuildBase(*st);
            for (U32 w = 0; w < 7; ++w)
            {
                U32 bits = av->mGpuWantBits[w];
                av->mGpuWantBits[w] = 0u;
                while (bits)
                {
                    const U32 j = w * 32u + (U32)__builtin_ctz(bits);
                    bits &= bits - 1u;
                    if (j >= MAX_JOINTS) continue;
                    if (st->last_want[j] == 0u) ++mStatWantAdds;
                    st->last_want[j] = mFrame;
                }
            }
            U8 in_set[MAX_JOINTS] = {};
            const U32* tp = &mTopo[(size_t)slot * MAX_JOINTS];
            const U32 req_off = (U32)mRbReq[mRing].size();
            const U32 req_first = (U32)reqw.size();
            auto add = [&](U32 j) {
                // the joint, then its parent chain (the chain fallback -- a joint under a
                // non-animation write -- recomputes from these locals)
                while (j < MAX_JOINTS && !in_set[j] && st->joints[j] && tp[j] != 0xFFFEu)
                {
                    in_set[j] = 1;
                    reqw.push_back(j);
                    mRbReq[mRing].push_back((U16)j);
                    const U32 p = tp[j] & 0xFFFFu;
                    if (p == 0xFFFFu) break;
                    j = p;
                }
            };
            if (st->cmp_armed)
            {
                for (U32 j = 0; j < MAX_JOINTS; ++j) add(j);
            }
            else
            {
                for (U16 j : st->base) add(j);
                for (U32 j = 0; j < MAX_JOINTS; ++j)
                {
                    const U32 lw = st->last_want[j];
                    if (lw == 0u) continue;
                    if (mFrame - lw < kWantFrames) { add(j); continue; }
                    st->last_want[j] = 0u;   // expired: fall back to LL's chain until read again
                    ++mStatWantExpires;
                    if (!in_set[j] && st->joints[j]) st->joints[j]->mGpuRelValid = false;
                }
            }
            const U32 cnt = (U32)reqw.size() - req_first;
            e[3] = mReadbackCount;
            e[20] = req_first;
            e[21] = cnt;
            mRbRecords[mRing].push_back({ av, sl.first, mReadbackCount, cnt, req_off, mFrame });
            mReadbackCount += cnt;
            ++rb_avatars;
        }

        // COMPARE reference: the full LL CPU pose for this frame (shadow mode)
        if (st->mode == 1 && st->compare && !mCmpPending)
        {
            CmpRef ref;
            ref.id = sl.first;
            ref.slot = slot;
            ref.cpu.assign((size_t)MAX_JOINTS * 16, 0.f);
            ref.joints.assign(MAX_JOINTS, nullptr);
            for (U32 j = 0; j < MAX_JOINTS; ++j)
            {
                LLJoint* jp = st->joints[j];
                if (!jp)
                {
                    static const F32 I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                    std::memcpy(&ref.cpu[(size_t)j * 16], I, sizeof(I));
                    continue;
                }
                std::memcpy(&ref.cpu[(size_t)j * 16], jp->getWorldMatrix4a().getF32ptr(), 16 * sizeof(F32));
                ref.joints[j] = jp;
            }
            std::string anims;
            U32 na = 0;
            for (LLMotion* m : av->getMotionController().getActiveMotions())
            {
                if (!m || na >= 8) continue;
                anims += (na ? "," : "") + m->getID().asString().substr(0, 8);
                ++na;
            }
            ref.anims = anims;
            mCmpRefs.push_back(std::move(ref));
            mCmpSlotsThisFrame.push_back(slot);
        }

        sk.setGpuSlot(slot);
        if (follow_wslot != 0xFFFFFFFFu)
        {
            fent.insert(fent.end(), e, e + ENTRY_WORDS);
            s_follows.emplace_back(slot, follow_wslot);
        }
        else
        {
            ent.insert(ent.end(), e, e + ENTRY_WORDS);
        }
        ++mEntryCount;
        ++mStatGpu;
    }
    mDeadLists.clear();

    // follow entries last (second dispatch); their world-clean also needs the wearer's
    mFollowFirst = (U32)(ent.size() / ENTRY_WORDS);
    ent.insert(ent.end(), fent.begin(), fent.end());
    for (const auto& f : s_follows)
        if (!mSlotWorldClean[f.second]) mSlotWorldClean[f.first] = 0u;
    LLVKHitch::count("animAttachFollow", (unsigned)s_follows.size());

    // pack
    const U32 hdr_base = HEADER_WORDS + (U32)ent.size();
    const U32 blend_base = hdr_base + (U32)hdr.size();
    const U32 value_base = blend_base + (U32)blend.size();
    const U32 basew_base = value_base + (U32)values.size();
    const U32 req_base = basew_base + (U32)basew.size();
    mWords.clear();
    mWords.reserve(req_base + reqw.size());
    mWords.resize(HEADER_WORDS, 0u);
    mWords[0] = mEntryCount;
    mWords[1] = hdr_base;
    mWords[2] = blend_base;
    mWords[3] = value_base;
    mWords[4] = basew_base;
    mWords[5] = req_base;
    mWords[6] = mFollowFirst;   // (informational: the renderer pushes it)
    mWords.insert(mWords.end(), ent.begin(), ent.end());
    mWords.insert(mWords.end(), hdr.begin(), hdr.end());
    mWords.insert(mWords.end(), blend.begin(), blend.end());
    const size_t vw = mWords.size();
    mWords.resize(vw + values.size());
    if (!values.empty()) std::memcpy(&mWords[vw], values.data(), values.size() * sizeof(F32));
    mWords.insert(mWords.end(), basew.begin(), basew.end());
    mWords.insert(mWords.end(), reqw.begin(), reqw.end());

    if (!mCmpSlotsThisFrame.empty() && !mCmpRefs.empty() && !mCmpPending)
    {
        mCmpPending = true;
        mCmpFrame = mFrame;
    }
    else if (mCmpPending)
    {
        mCmpSlotsThisFrame.clear();   // one compare in flight at a time
    }

    // Keyframe arena LRU: lists no avatar has evaluated for kListEvictFrames become
    // garbage (the compaction above reclaims it; ensureUploaded re-uploads on demand).
    // Stamps are sampled every 64 frames, so the window is >> the sampling period.
    if ((mFrame % 600u) == 300u)
    {
        for (auto it = mLists.begin(); it != mLists.end(); )
        {
            if (mFrame - it->second.last_used > kListEvictFrames)
            {
                mDescGarbage += it->second.desc_count;
                mKeyGarbage += it->second.key_count;
                const_cast<LLKeyframeMotion::JointMotionList*>(it->first)->mGpuBase = -1;
                it = mLists.erase(it);
            }
            else ++it;
        }
    }

    // stale avatar states (not updated for a long time or dead)
    if ((mFrame % 600u) == 0u)
    {
        for (auto it = mAvatars.begin(); it != mAvatars.end(); )
        {
            if (mFrame - it->second->last_seen > 1800u)
            {
                AvState& st = *it->second;
                if (st.bound && gObjectList.findObject(st.id) == (LLViewerObject*)st.av) unbindJoints(st);
                it = mAvatars.erase(it);
            }
            else ++it;
        }
    }

    LLVKHitch::count("animGpuAvatars", mStatGpu);
    LLVKHitch::count("animBlendStates", n_blend_states);
    LLVKHitch::count("animBaseWrites", (unsigned)(basew.size() / 12));
    LLVKHitch::count("animConstraintCpu", LLKeyframeMotion::sGpuConstraintFallbacks);
    LLKeyframeMotion::sGpuConstraintFallbacks = 0;
    LLVKHitch::count("animCpuKeySamples", LLKeyframeMotion::sCpuJointSamples);   // #332
    LLKeyframeMotion::sCpuJointSamples = 0;
    LLVKHitch::count("animPoseAdds", LLPoseBlender::sJointStatesAdded);   // #330
    LLPoseBlender::sJointStatesAdded = 0;
    LLVKHitch::count("animMotionVisits", LLMotionController::sMotionVisits);   // #334
    LLMotionController::sMotionVisits = 0;
    LLVKHitch::count("animMotionCreate", LLMotionController::sMotionsCreated);         // #331
    LLVKHitch::count("animMotionDeprecate", LLMotionController::sMotionsDeprecated);   // #331
    LLMotionController::sMotionsCreated = 0;
    LLMotionController::sMotionsDeprecated = 0;
    LLVKHitch::count("animKeyArenaKB", (unsigned)((mKeyTimes.size() * 20 + mDesc.size() * 4) >> 10));
    LLVKHitch::count("animRbJoints", mReadbackCount);
    LLVKHitch::count("animRbAvatars", rb_avatars);
    LLVKHitch::count("animWantAdd", mStatWantAdds);
    LLVKHitch::count("animWantExpire", mStatWantExpires);
    mSumRbJoints += mReadbackCount; mSumRbAvatars += rb_avatars;
    mSumWantAdds += mStatWantAdds; mSumWantExpires += mStatWantExpires;
    if ((mFrame % 600u) == 0u)
    {
        if (mSumRbAvatars)
            LL_INFOS("VKAnim") << "sparse readback (600 frames): avg " << (mSumRbJoints / 600) << " joints/frame for "
                << (mSumRbAvatars / 600) << " GPU avatars (" << (mSumRbJoints / mSumRbAvatars) << " per avatar of "
                << MAX_JOINTS << "), want adds " << mSumWantAdds << ", expiries " << mSumWantExpires << LL_ENDL;
        mSumRbJoints = mSumRbAvatars = mSumWantAdds = mSumWantExpires = 0;
        const U64 exports = mSumExportFull + mSumExportIncr;
        if (exports)
            LL_INFOS("VKAnim") << "pose export (600 frames, RenderVKAnimIncremental="
                << (incrementalEnabled() ? "on" : "off") << "): " << mSumExportFull << " full, "
                << mSumExportIncr << " incremental (" << (mSumExportIncr * 100 / exports) << "%), avg "
                << (mSumExportUs / 600) << " us/frame, " << (mSumExportUs * 1000 / exports) << " ns/export"
                << (verifyEnabled() ? ", verified " + std::to_string(mSumVerify) + ", mismatched "
                                      + std::to_string(mSumVerifyBad) : std::string()) << LL_ENDL;
        mSumExportFull = mSumExportIncr = mSumExportUs = mSumVerify = mSumVerifyBad = 0;
    }

    ++mFrame;
    LLJoint::sFrameCounter = mFrame;
    return mEntryCount;
}

void LLCleanAnimStore::checkCompare(const F32* gpu_rows, size_t floats)
{
    mCmpPending = false;
    if (!gpu_rows) { mCmpRefs.clear(); return; }
    for (size_t k = 0; k < mCmpRefs.size(); ++k)
    {
        const CmpRef& ref = mCmpRefs[k];
        // The GPU table rows are mat3x4 (12 floats per joint, clean_joint_table.glsl): the
        // upper 3x4 of the joint's mat4, row r at [r * 4 ..]. The 4th row is constant.
        const size_t base = k * MAX_JOINTS * LLCleanSkinStore::JOINT_WORLD_FLOATS;
        if (base + (size_t)MAX_JOINTS * LLCleanSkinStore::JOINT_WORLD_FLOATS > floats) break;
        F32 max_abs[3] = { 0.f, 0.f, 0.f };   // bone / collision volume / attachment point
        F32 worst = 0.f; U32 worst_j = 0; U32 bad = 0;
        for (U32 j = 0; j < MAX_JOINTS; ++j)
        {
            if (!ref.joints[j]) continue;
            const LLJoint* jp = ref.joints[j];
            const U32 cls = dynamic_cast<const LLViewerJointAttachment*>(jp) ? 2u
                          : dynamic_cast<const LLAvatarJointCollisionVolume*>(jp) ? 1u : 0u;
            F32 jmax = 0.f;
            for (U32 f = 0; f < 16; ++f)
            {
                const U32 c = f >> 2, r = f & 3u;   // mat4 element (column c, row r)
                const F32 g = r == 3u ? (c == 3u ? 1.f : 0.f)
                                      : gpu_rows[base + (size_t)j * LLCleanSkinStore::JOINT_WORLD_FLOATS + r * 4u + c];
                const F32 d = std::fabs(ref.cpu[(size_t)j * 16 + f] - g);
                jmax = llmax(jmax, d);
            }
            max_abs[cls] = llmax(max_abs[cls], jmax);
            if (jmax > 1e-3f) ++bad;
            if (jmax > worst) { worst = jmax; worst_j = j; }
        }
        LL_INFOS("VKAnimCmp") << "GPU anim compare (frame " << mCmpFrame << ", avatar " << ref.id
            << " slot " << ref.slot << "): max_abs bones=" << max_abs[0] << " cv=" << max_abs[1]
            << " attach=" << max_abs[2] << " worst joint " << worst_j << " ("
            << (ref.joints[worst_j] ? ref.joints[worst_j]->getName() : std::string("?")) << ") " << worst
            << " mismatched_joints=" << bad << " anims[" << ref.anims << "]"
            << (bad ? "  ** GPU ANIMATION DIVERGES FROM CPU **" : "  (match)") << LL_ENDL;
    }
    mCmpRefs.clear();
}
