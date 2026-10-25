/**
 * @file llcleanskinstore.cpp
 * @brief Implementation of LLCleanSkinStore.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanskinstore.h"
#include "lltimer.h"
#include <sstream>

#include "llgl.h"
#include "llprofiler.h"
#include "llmeshrepository.h"  // LLMeshSkinInfo
#include "llvoavatar.h"
#include "lljoint.h"
#include "llquaternion.h"
#include "llskinningutil.h"
#include "llviewerobjectlist.h"
#include "v3math.h"

#include <cstddef>
#include <cstring>

LLCleanSkinStore& LLCleanSkinStore::instance()
{
    static LLCleanSkinStore s;
    return s;
}

bool LLCleanSkinStore::growSkinMeta(U32 new_cap)
{
    if (new_cap <= mSkinMetaCapacity) return true;
    mSkinMetaCPU.resize(new_cap);   // the VK renderer reads it directly (buildVKPalettes)
    mSkinMetaCapacity = new_cap;
    return true;
}

bool LLCleanSkinStore::grow(U32 new_cap)
{
    if (new_cap <= mCapacity) return true;
    mBlocks.resize(new_cap);
    mEntries.resize(new_cap);
    mRefs.resize(new_cap, 0);
    mTouch.resize(new_cap, 0);
    // CPU state only: the skin blocks/entries above and the per-avatar joint table
    // refreshAllVK/buildVKPalettes read.
    if (mAvatarJointsCPU.size() < MAX_AVATARS)
    {
        mAvatarJointsCPU.resize(MAX_AVATARS);
        mAvatarDirty.resize(MAX_AVATARS, 0);
    }
    mCapacity = new_cap;
    return true;
}

void LLCleanSkinStore::shutdown()
{
    std::lock_guard<std::mutex> lk(mMutex);
    mCapacity = mLiveCount = mSkinMetaCapacity = mAvatarLive = 0;
    mBlocks.clear();
    mEntries.clear();
    mRefs.clear();
    mTouch.clear();
    mFreeSkinIds.clear();
    mKeyToId.clear();
    mAvatarJointsCPU.clear();
    mAvatarDirty.clear();
    mAvatarAliveVK.clear();
    mAvatarSlot.clear();
    mSkinMetaCPU.clear();
}

U32 LLCleanSkinStore::assignAvatarSlot(LLVOAvatar* avatar)
{
    if (!avatar) return 0xFFFFFFFFu;
    const LLUUID& id = avatar->getID();
    auto it = mAvatarSlot.find(id);
    if (it != mAvatarSlot.end()) return it->second;
    // Recycled slot first (past its quarantine), then a fresh one.
    for (size_t i = 0; i < mFreeAvatarSlots.size(); ++i)
    {
        if (mFreeAvatarSlots[i].second + kQuarantineFrames > mReapFrame) continue;
        const U32 slot = mFreeAvatarSlots[i].first;
        mFreeAvatarSlots.erase(mFreeAvatarSlots.begin() + i);
        mAvatarSlot[id] = slot;
        mAvatarDeadSince.erase(id);
        if (slot < mAvatarDirty.size()) mAvatarDirty[slot] = 1;
        return slot;
    }
    if (mAvatarLive >= MAX_AVATARS)
    {
        LL_WARNS_ONCE("CleanSkin") << "avatar slots exhausted (cap "
            << MAX_AVATARS << " live, " << mFreeAvatarSlots.size() << " in quarantine); new avatars will get static fallback" << LL_ENDL;
        return 0xFFFFFFFFu;
    }
    const U32 slot = mAvatarLive++;
    mAvatarSlot[id] = slot;
    mAvatarDeadSince.erase(id);
    return slot;
}

U32 LLCleanSkinStore::ensureAvatarSlot(LLVOAvatar* avatar)
{
    if (!avatar) return 0xFFFFFFFFu;
    std::lock_guard<std::mutex> lk(mMutex);
    return assignAvatarSlot(avatar);
}

U32 LLCleanSkinStore::findAvatarSlot(const LLUUID& avatar_id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mAvatarSlot.find(avatar_id);
    return it != mAvatarSlot.end() ? it->second : 0xFFFFFFFFu;
}

void LLCleanSkinStore::copyAvatarSlots(std::vector<std::pair<LLUUID, U32>>& out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    out.clear();
    out.reserve(mAvatarSlot.size());
    for (const auto& kv : mAvatarSlot) out.emplace_back(kv.first, kv.second);
}

void LLCleanSkinStore::clearGpuSlots()
{
    std::lock_guard<std::mutex> lk(mMutex);
    std::fill(mSlotGpu.begin(), mSlotGpu.end(), 0);
}

void LLCleanSkinStore::setGpuSlot(U32 slot)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (mSlotGpu.size() < MAX_AVATARS) mSlotGpu.resize(MAX_AVATARS, 0);
    if (slot < mSlotGpu.size()) mSlotGpu[slot] = 1;
}

U32 LLCleanSkinStore::getOrCreate(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo)
{
    if (!avatar || !skinInfo) return 0xFFFFFFFFu;
    // Key by (avatar*, mesh_id). The avatar* part is needed because
    // two avatars wearing the same body mesh need DIFFERENT palettes.
    // The mesh_id part (NOT mHash) is needed because two distinct
    // LLMeshSkinInfo objects can share the same mHash but have
    // different mBindShapeMatrix values -- LL's hashing apparently
    // doesn't include bind_shape, so keying on hash aliases them
    // into one entry and the second mesh inherits the first's bind
    // shape, producing dramatically-wrong vertex positions. mesh_id
    // is the canonical asset identifier; one mesh = one bind_shape.
    const Key k{avatar->getID(), skinInfo->mMeshID};

    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyToId.find(k);
    if (it != mKeyToId.end())
    {
        // Only hand back a usable skin_id once the entry is fully
        // resolved (joint_count > 0, set by populateSkinMeta on the
        // valid path). An UNresolved entry -- avatar not yet in
        // gObjectList, or LL still streaming the skin info -- has its
        // avatar_idx/joints defaulted to slot 0, so the mesh would skin
        // to the FIRST avatar and visibly stretch toward it for a frame.
        // Returning the not-ready sentinel keeps the instance on the
        // static path (bind pose at its attachment) until refreshAll
        // resolves it; registerFace re-asks every frame and picks up the
        // real id automatically once ready.
        const U32 id = it->second;
        mTouch[id] = mReapFrame;   // still wanted (pending entries live on re-asks)
        return (mSkinMetaCPU[id].joint_count > 0u) ? id : 0xFFFFFFFFu;
    }

    U32 id = takeFreeSkinId();
    if (id == 0xFFFFFFFFu)
    {
        if (mLiveCount >= mCapacity)
        {
            const U32 new_cap = mCapacity == 0 ? 256 : mCapacity * 2;
            if (!grow(new_cap))          return 0xFFFFFFFFu;
            if (!growSkinMeta(new_cap))  return 0xFFFFFFFFu;
        }
        id = mLiveCount++;
    }
    mKeyToId[k] = id;
    mTouch[id] = mReapFrame;
    Entry& e = mEntries[id];
    e.avatar    = avatar;
    e.avatar_id = avatar->getID();
    e.skin      = skinInfo;
    e.hash      = skinInfo->mHash;

    // CPU shadow only -- bind_shape AND matrices both flow through
    // the palette compute shader (matrices computed each frame from
    // joint world * inv_bind; bind_shape copied from SkinMeta).
    // No CPU-side mSSBO upload from here.
    SkinBlock& b = mBlocks[id];
    std::memset(&b, 0, sizeof(b));
    std::memcpy(b.bind_shape, skinInfo->mBindShapeMatrix.getF32ptr(), sizeof(b.bind_shape));

    // Initial meta population. Re-validated per frame in refreshAll
    // since LL fills skin info asynchronously: registration can land
    // on a partial skin (mJointNumsInitialized=false, JointNames<4,
    // bind_shape not yet affine) and the entry's meta would stay
    // wrong forever without the per-frame retry.
    populateSkinMeta(id);
    // Same readiness gate as the cache-hit path above: don't expose a
    // half-resolved entry (which would skin to avatar slot 0).
    return (mSkinMetaCPU[id].joint_count > 0u) ? id : 0xFFFFFFFFu;
}

U32 LLCleanSkinStore::flexiChain(const LLUUID& object_id, const F32 inv_bind[FLEXI_POINTS * 16])
{
    std::lock_guard<std::mutex> lk(mMutex);
    U32 id;
    auto it = mFlexiSkin.find(object_id);
    if (it != mFlexiSkin.end())
    {
        id = it->second;
    }
    else
    {
        constexpr U32 kPerSlot = MAX_AVATAR_JOINTS / FLEXI_POINTS;   // 24
        // A recycled chain (a flexi that left) past its quarantine first.
        FlexiChain reuse{0xFFFFFFFFu, 0};
        for (size_t i = 0; i < mFreeFlexiChains.size(); ++i)
            if (mFreeFlexiChains[i].second + kQuarantineFrames <= mReapFrame)
            {
                reuse = mFreeFlexiChains[i].first;
                mFreeFlexiChains.erase(mFreeFlexiChains.begin() + i);
                break;
            }
        if (reuse.slot == 0xFFFFFFFFu && (mFlexiSlot == 0xFFFFFFFFu || mFlexiNext >= kPerSlot))
        {
            if (mAvatarLive >= MAX_AVATARS)
            {
                LL_WARNS_ONCE("CleanSkin") << "joint-table slots exhausted: further flexis render rigid" << LL_ENDL;
                return 0xFFFFFFFFu;
            }
            // A pseudo avatar: a UUID no object has, so refreshAllVK never overwrites
            // it and avatar visibility leaves it on.
            LLUUID pseudo;
            pseudo.generate(llformat("slipstream-flexi-slot-%u", mAvatarLive));
            mFlexiSlot = mAvatarLive++;
            mAvatarSlot[pseudo] = mFlexiSlot;
            mFlexiPseudoSlots.insert(mFlexiSlot);
            mFlexiNext = 0;
            if (mAvatarJointsCPU.size() < MAX_AVATARS)
            {
                mAvatarJointsCPU.resize(MAX_AVATARS);
                mAvatarDirty.resize(MAX_AVATARS, 0);
            }
        }
        id = takeFreeSkinId();
        if (id == 0xFFFFFFFFu)
        {
            if (mLiveCount >= mCapacity)
            {
                const U32 new_cap = mCapacity == 0 ? 256 : mCapacity * 2;
                if (!grow(new_cap) || !growSkinMeta(new_cap)) return 0xFFFFFFFFu;
            }
            id = mLiveCount++;
        }
        mEntries[id] = Entry{};   // no avatar / skin: populateSkinMeta leaves it alone
        std::memset(&mBlocks[id], 0, sizeof(SkinBlock));
        if (reuse.slot != 0xFFFFFFFFu) mFlexiChains[id] = reuse;
        else { mFlexiChains[id] = FlexiChain{mFlexiSlot, mFlexiNext * FLEXI_POINTS}; ++mFlexiNext; }
        mFlexiSkin[object_id] = id;
    }
    const FlexiChain& fc = mFlexiChains[id];
    SkinMeta& sm = mSkinMetaCPU[id];
    std::memset(&sm, 0, sizeof(SkinMeta));
    sm.avatar_idx = fc.slot;
    sm.joint_count = FLEXI_POINTS;
    for (U32 j = 0; j < FLEXI_POINTS; ++j) sm.joint_nums[j] = (S32)(fc.base + j);
    std::memcpy(sm.inv_bind, inv_bind, FLEXI_POINTS * 16 * sizeof(F32));
    static const F32 IDENT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(sm.bind_shape, IDENT, sizeof(IDENT));
    if (id < mSkinMetaDirtyMin)     mSkinMetaDirtyMin = id;
    if (id + 1 > mSkinMetaDirtyMax) mSkinMetaDirtyMax = id + 1;
    return id;
}

// Pseudo mesh id of an avatar's system-body skin entry (no asset has it).
static const LLUUID& sysBodyKey()
{
    static const LLUUID s_key = []{ LLUUID u; u.generate(std::string("slipstream-sysavi-skin")); return u; }();
    return s_key;
}

U32 LLCleanSkinStore::sysBodySkin(LLVOAvatar* avatar, U32 joint_count, const S32* joint_nums, const F32* inv_bind)
{
    if (!avatar || joint_count == 0 || joint_count > MAX_JOINTS || !joint_nums || !inv_bind) return 0xFFFFFFFFu;
    const Key k{avatar->getID(), sysBodyKey()};
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 slot = assignAvatarSlot(avatar);
    if (slot == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    U32 id;
    auto it = mKeyToId.find(k);
    if (it != mKeyToId.end())
    {
        id = it->second;
    }
    else
    {
        id = takeFreeSkinId();
        if (id == 0xFFFFFFFFu)
        {
            if (mLiveCount >= mCapacity)
            {
                const U32 new_cap = mCapacity == 0 ? 256 : mCapacity * 2;
                if (!grow(new_cap) || !growSkinMeta(new_cap)) return 0xFFFFFFFFu;
            }
            id = mLiveCount++;
        }
        mKeyToId[k] = id;
        // No LLMeshSkinInfo: populateSkinMeta leaves the entry alone.
        mEntries[id] = Entry{};
        mEntries[id].avatar = avatar;
        mEntries[id].avatar_id = avatar->getID();
        std::memset(&mBlocks[id], 0, sizeof(SkinBlock));
        std::memset(&mSkinMetaCPU[id], 0, sizeof(SkinMeta));
    }
    SkinMeta nm;
    std::memset(&nm, 0, sizeof(SkinMeta));
    nm.avatar_idx = slot;
    nm.joint_count = joint_count;
    std::memcpy(nm.joint_nums, joint_nums, joint_count * sizeof(S32));
    std::memcpy(nm.inv_bind, inv_bind, joint_count * 16 * sizeof(F32));
    static const F32 IDENT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(nm.bind_shape, IDENT, sizeof(IDENT));
    if (std::memcmp(&nm, &mSkinMetaCPU[id], sizeof(SkinMeta)) != 0)
    {
        mSkinMetaCPU[id] = nm;
        std::memcpy(mBlocks[id].bind_shape, IDENT, sizeof(IDENT));
        if (id < mSkinMetaDirtyMin)     mSkinMetaDirtyMin = id;
        if (id + 1 > mSkinMetaDirtyMax) mSkinMetaDirtyMax = id + 1;
    }
    return id;
}

U32 LLCleanSkinStore::findSysBodySkin(const LLUUID& avatar_id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyToId.find(Key{avatar_id, sysBodyKey()});
    return it != mKeyToId.end() ? it->second : 0xFFFFFFFFu;
}

U32 LLCleanSkinStore::flexiSkin(const LLUUID& object_id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mFlexiSkin.find(object_id);
    return it != mFlexiSkin.end() ? it->second : 0xFFFFFFFFu;
}

void LLCleanSkinStore::flexiSetPose(U32 skin_id, const F32 world[FLEXI_POINTS * 16])
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mFlexiChains.find(skin_id);
    if (it == mFlexiChains.end() || it->second.slot >= mAvatarJointsCPU.size()) return;
    F32* dst = mAvatarJointsCPU[it->second.slot].world + it->second.base * JOINT_WORLD_FLOATS;
    for (U32 k = 0; k < FLEXI_POINTS; ++k)
        packJointWorld(dst + k * JOINT_WORLD_FLOATS, world + k * 16);
}

bool LLCleanSkinStore::isPending(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) const
{
    if (!avatar || !skinInfo) return false;
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyToId.find(Key{avatar->getID(), skinInfo->mMeshID});
    return it != mKeyToId.end() && mSkinMetaCPU[it->second].joint_count == 0u;
}

std::string LLCleanSkinStore::describe(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) const
{
    if (!avatar || !skinInfo) return "n/a";
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mKeyToId.find(Key{avatar->getID(), skinInfo->mMeshID});
    if (it == mKeyToId.end()) return "no-entry";
    const U32 id = it->second;
    const SkinMeta& sm = mSkinMetaCPU[id];
    std::ostringstream os;
    os << "id=" << id << " joints=" << sm.joint_count << " av_slot=" << sm.avatar_idx;
    if (sm.joint_count == 0u)
    {
        const Entry& e = mEntries[id];
        LLViewerObject* obj = gObjectList.findObject(e.avatar_id);
        const F32* bs = e.skin ? e.skin->mBindShapeMatrix.getF32ptr() : nullptr;
        os << " UNRESOLVED(av_live=" << ((obj && obj->asAvatar() && !obj->isDead()) ? 1 : 0)
           << " skin_joints=" << (e.skin ? e.skin->mJointNames.size() : 0)
           << " bs33=" << (bs ? bs[15] : -1.f) << ")";
    }
    return os.str();
}

void LLCleanSkinStore::countAvatarSkins(const LLUUID& avatar_id, U32& total, U32& unresolved) const
{
    total = unresolved = 0;
    std::lock_guard<std::mutex> lk(mMutex);
    // keys sort by avatar id first
    for (auto it = mKeyToId.lower_bound(Key{avatar_id, LLUUID::null});
         it != mKeyToId.end() && it->first.avatar_id == avatar_id; ++it)
    {
        ++total;
        if (it->second < mSkinMetaCPU.size() && mSkinMetaCPU[it->second].joint_count == 0u) ++unresolved;
    }
}

bool LLCleanSkinStore::populateSkinMeta(U32 id)
{
    Entry& e = mEntries[id];
    if (!e.skin) return false;

    // NEVER dereference e.avatar without first revalidating it via
    // gObjectList. The raw pointer can dangle (avatar destroyed)
    // OR be recycled to a DIFFERENT avatar (same heap address, new
    // identity). Calling assignAvatarSlot(stale_ptr) would then use
    // the new avatar's UUID and silently bind this skin to the wrong
    // avatar's joints -- presenting as distorted/wrong-pose meshes.
    LLViewerObject* obj = gObjectList.findObject(e.avatar_id);
    LLVOAvatar* live_av = obj ? obj->asAvatar() : nullptr;
    if (!live_av || live_av->isDead())
    {
        // Skip; will be re-tried next frame when the avatar exists.
        return false;
    }
    if (live_av != e.avatar)
    {
        // Pointer recycled / avatar respawned. Adopt the new pointer.
        e.avatar = live_av;
    }

    static const F32 SENTINEL_BIND_SHAPE[16] = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 0.f   // m33=0 -> consumer treats as static fallback
    };

    const U32   cur_joints = (U32)e.skin->mJointNames.size();
    const F32*  src_bs     = e.skin->mBindShapeMatrix.getF32ptr();
    const bool  bs_affine  = (src_bs[15] > 0.5f);
    // Resolved = at least one joint with an inverse bind for each, and an affine bind
    // shape. (Was cur_joints >= 4 as a "partial skin" heuristic, which rejected every
    // legitimately single-joint rigged mesh -- a whole avatar body stayed unresolved,
    // drawn unskinned = invisible. A partial LLMeshSkinInfo shows up as missing
    // inverse binds, not as few joints.)
    const bool  valid      = (cur_joints >= 1) && bs_affine
                          && e.skin->mInvBindMatrix.size() >= cur_joints;

    SkinBlock& b  = mBlocks[id];
    SkinMeta&  sm = mSkinMetaCPU[id];

    if (!valid)
    {
        // Sentinel path. Both joint_count=0 and bind_shape=SENTINEL
        // live in SkinMeta; the compute shader reads sm.bind_shape
        // and writes SkinBlock.bind_shape each dispatch.
        const U32 want_avatar = assignAvatarSlot(live_av);
        bool dirty = false;
        if (sm.avatar_idx != want_avatar) { sm.avatar_idx = want_avatar; dirty = true; }
        if (sm.joint_count != 0u)         { sm.joint_count = 0u;         dirty = true; }
        if (std::memcmp(sm.bind_shape, SENTINEL_BIND_SHAPE, sizeof(sm.bind_shape)) != 0)
        {
            std::memcpy(sm.bind_shape, SENTINEL_BIND_SHAPE, sizeof(sm.bind_shape));
            dirty = true;
        }
        if (dirty)
        {
            if (id < mSkinMetaDirtyMin)     mSkinMetaDirtyMin = id;
            if (id + 1 > mSkinMetaDirtyMax) mSkinMetaDirtyMax = id + 1;
        }
        return false;
    }

    if (!e.skin->mJointNumsInitialized)
    {
        LLSkinningUtil::initJointNums(
            const_cast<LLMeshSkinInfo*>(e.skin.get()), live_av);
    }

    const U32 packed = cur_joints > MAX_JOINTS ? MAX_JOINTS : cur_joints;

    // Always (re)assign the avatar slot from the LIVE pointer. Idempotent
    // on UUID. Must NOT trust sm.avatar_idx == 0xFFFFFFFFu as the "unset"
    // signal -- zero-init left it at 0 (a valid first-avatar slot, which
    // would alias every skin to avatar 0).
    const U32 want_avatar = assignAvatarSlot(live_av);
    bool meta_changed = false;
    if (sm.joint_count != packed)      meta_changed = true;
    if (sm.avatar_idx  != want_avatar) { sm.avatar_idx = want_avatar; meta_changed = true; }
    // bind_shape now lives in SkinMeta; compute shader copies it
    // into SkinBlock each dispatch. We just keep the CPU shadow
    // (b.bind_shape) updated for completeness / future reads, but
    // the GPU pipeline doesn't depend on a CPU-side bind_shape upload.
    if (std::memcmp(b.bind_shape, src_bs, sizeof(b.bind_shape)) != 0)
    {
        std::memcpy(b.bind_shape, src_bs, sizeof(b.bind_shape));
    }

    // Always re-copy joint_nums + inv_bind + bind_shape when the
    // asset content might have changed. Skin asset loads fill these
    // AFTER first registerFace and we'd otherwise keep stale data.
    bool data_changed = meta_changed;
    if (!data_changed)
    {
        if (!e.skin->mInvBindMatrix.empty() &&
            std::memcmp(sm.inv_bind,
                        e.skin->mInvBindMatrix[0].getF32ptr(),
                        16 * sizeof(F32)) != 0)
        {
            data_changed = true;
        }
        if (!data_changed &&
            std::memcmp(sm.bind_shape, src_bs, sizeof(sm.bind_shape)) != 0)
        {
            data_changed = true;
        }
    }
    if (data_changed)
    {
        sm.joint_count = packed;
        for (U32 j = 0; j < packed; ++j)
        {
            sm.joint_nums[j] = (j < e.skin->mJointNums.size())
                ? e.skin->mJointNums[j] : 0;
        }
        for (U32 j = 0; j < packed && j < e.skin->mInvBindMatrix.size(); ++j)
        {
            std::memcpy(sm.inv_bind + j * 16,
                        e.skin->mInvBindMatrix[j].getF32ptr(),
                        16 * sizeof(F32));
        }
        std::memcpy(sm.bind_shape, src_bs, sizeof(sm.bind_shape));
        if (id < mSkinMetaDirtyMin)     mSkinMetaDirtyMin = id;
        if (id + 1 > mSkinMetaDirtyMax) mSkinMetaDirtyMax = id + 1;
    }
    return true;
}

void LLCleanSkinStore::refreshAllVK()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    // CPU-only mirror of refreshAll's first half. No GL: the VK renderer has no
    // GL context and computes the palettes itself (buildVKPalettes) from the
    // per-avatar joint world matrices we refresh here. See refreshAll for the
    // rationale on each step; the GL upload + palette-compute-dispatch tail is
    // intentionally absent.
    std::lock_guard<std::mutex> lk(mMutex);
    // Size the per-avatar joint table here (the GL init() that sizes it never runs
    // under VK). Rigid attachments read it even when no rigged skin exists, so the
    // joint refresh below must not be gated on mLiveCount.
    if (mAvatarJointsCPU.size() < MAX_AVATARS)
    {
        mAvatarJointsCPU.resize(MAX_AVATARS);
        mAvatarDirty.resize(MAX_AVATARS, 0);
    }

    // Revalidate skin metadata (idempotent; retries partial async skin info). Entries
    // still UNRESOLVED (joint_count 0: avatar not in the object list yet, or LL still
    // streaming the skin) retry every frame; resolved ones only change if LL refills the
    // skin asset or the avatar is re-created, so they re-verify on a rotating 1/32
    // slice -- each populateSkinMeta is an object-list lookup + two 64 B compares, which
    // across a crowd's thousands of skins was a steady per-frame cost for no change.
    static U32 s_reval = 0;
    ++s_reval;
    for (U32 i = 0; i < mLiveCount; ++i)
    {
        if (mSkinMetaCPU[i].joint_count == 0u || ((i + s_reval) & 31u) == 0u)
            populateSkinMeta(i);
    }

    // Refresh joint world matrices per unique alive avatar (once per avatar).
    mAvatarAliveVK.assign(MAX_AVATARS, 0);
    for (const auto& kv : mAvatarSlot)
    {
        const LLUUID& avid = kv.first;
        const U32     slot = kv.second;
        LLViewerObject* obj = gObjectList.findObject(avid);
        LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
        if (!av || av->isDead()) continue;
        if (slot >= mAvatarJointsCPU.size()) continue;
        mAvatarAliveVK[slot] = 1;
        mAvatarDeadSince.erase(avid);
        // GPU-animated this frame: clean_anim.comp writes this avatar's joint table
        // row, and the CPU pose is not evaluated -- no copy.
        if (slot < mSlotGpu.size() && mSlotGpu[slot]) continue;

        AvatarJoints& aj = mAvatarJointsCPU[slot];
        const U32 nj = MAX_AVATAR_JOINTS;
        for (U32 j = 0; j < nj; ++j)
        {
            LLJoint* joint = av->getJoint((S32)j);
            if (!joint)
            {
                static const F32 IDENT_M4[16] = {
                    1.f, 0.f, 0.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 0.f, 0.f, 1.f
                };
                packJointWorld(aj.world + j * JOINT_WORLD_FLOATS, IDENT_M4);
                continue;
            }
            const F32* wm = (const F32*)joint->getWorldMatrix4a().getF32ptr();
            packJointWorld(aj.world + j * JOINT_WORLD_FLOATS, wm);
        }
    }
    ++mReapFrame;
    reapLocked();
}

U32 LLCleanSkinStore::takeFreeSkinId()
{
    // Lowest eligible id (keeps the live range compact so the top can be trimmed).
    // Eligible = past its quarantine (frames in flight, LLCleanGpuRetire is far shorter)
    // AND no instance row still references it: the avatar / flexi reaps free ids while
    // the dying linkset's rows may still name them, and a later release of such a row
    // must never decrement the NEXT owner's count.
    size_t best = mFreeSkinIds.size();
    for (size_t i = 0; i < mFreeSkinIds.size(); ++i)
    {
        if (mFreeSkinIds[i].second + kQuarantineFrames > mReapFrame) continue;
        const U32 id = mFreeSkinIds[i].first;
        if (id < mRefs.size() && mRefs[id] != 0) continue;
        if (best == mFreeSkinIds.size() || id < mFreeSkinIds[best].first) best = i;
    }
    if (best == mFreeSkinIds.size()) return 0xFFFFFFFFu;
    const U32 id = mFreeSkinIds[best].first;
    mFreeSkinIds[best] = mFreeSkinIds.back();
    mFreeSkinIds.pop_back();
    return id;
}

void LLCleanSkinStore::retargetRef(U32 old_id, U32 new_id)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (new_id != 0xFFFFFFFFu && new_id < mRefs.size()) ++mRefs[new_id];
    if (old_id != 0xFFFFFFFFu && old_id < mRefs.size())
    {
        if (mRefs[old_id] > 0) --mRefs[old_id];
        else LL_WARNS_ONCE("CleanSkin") << "skin " << old_id << " released with no references (ignored)" << LL_ENDL;
    }
}

void LLCleanSkinStore::freeSkinLocked(U32 id)
{
    if (id >= mLiveCount) return;
    mEntries[id] = Entry{};
    std::memset(&mBlocks[id], 0, sizeof(SkinBlock));
    std::memset(&mSkinMetaCPU[id], 0, sizeof(SkinMeta));
    if (id < mSkinMetaDirtyMin)     mSkinMetaDirtyMin = id;
    if (id + 1 > mSkinMetaDirtyMax) mSkinMetaDirtyMax = id + 1;
    mFreeSkinIds.emplace_back(id, mReapFrame);
    ++mReapedSkins;
}

LLCleanSkinStore::Stats LLCleanSkinStore::stats() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    Stats s;
    s.high_water = mLiveCount;
    s.capacity = mCapacity;
    s.free = (U32)mFreeSkinIds.size();
    s.live = mLiveCount >= s.free ? mLiveCount - s.free : 0;
    for (U32 i = 0; i < mLiveCount && i < mRefs.size(); ++i) if (mRefs[i]) ++s.referenced;
    s.avatar_slots = (U32)mAvatarSlot.size();
    s.avatar_free = (U32)mFreeAvatarSlots.size();
    s.flexi = (U32)mFlexiSkin.size();
    s.reaped_unused = mReapedUnused;
    s.reaped_skins = mReapedSkins;
    s.trimmed = mTrimmed;
    s.cpu_bytes = (U64)mCapacity * (sizeof(SkinBlock) + sizeof(Entry) + 2 * sizeof(U32))
                + (U64)mSkinMetaCapacity * sizeof(SkinMeta)
                + (U64)mAvatarJointsCPU.size() * sizeof(AvatarJoints)
                + (U64)mKeyToId.size() * (sizeof(Key) + sizeof(U32) + 32);
    return s;
}

// RECYCLING (#222). Called once per frame from refreshAllVK (lock held), after the
// alive pass. An avatar out of the object list (or dead) for kReapFrames gives back its
// joint-table slot and its skin entries; a flexi object gone that long gives back its
// chain + skin entry. Freed skin entries are zeroed (joint_count 0: consumers take the
// static path, populateSkinMeta leaves an avatar-less entry alone) and dirtied so the
// GPU meta copy follows.
void LLCleanSkinStore::reapLocked()
{
    auto freeSkin = [&](U32 id) { freeSkinLocked(id); };
    // Avatars: every 16 frames is plenty for a 300-frame grace period.
    if ((mReapFrame & 15u) == 0u)
    {
        std::vector<LLUUID> gone;
        for (const auto& kv : mAvatarSlot)
        {
            const U32 slot = kv.second;
            if (mFlexiPseudoSlots.count(slot)) continue;
            if (slot < mAvatarAliveVK.size() && mAvatarAliveVK[slot]) continue;
            auto it = mAvatarDeadSince.find(kv.first);
            if (it == mAvatarDeadSince.end()) { mAvatarDeadSince[kv.first] = mReapFrame; continue; }
            if (it->second + kReapFrames <= mReapFrame) gone.push_back(kv.first);
        }
        for (const LLUUID& avid : gone)
        {
            const U32 slot = mAvatarSlot[avid];
            mAvatarSlot.erase(avid);
            mAvatarDeadSince.erase(avid);
            mFreeAvatarSlots.emplace_back(slot, mReapFrame);
            ++mReapedAvatars;
            // Its skin entries (keys sort by avatar id first).
            auto it = mKeyToId.lower_bound(Key{avid, LLUUID::null});
            while (it != mKeyToId.end() && it->first.avatar_id == avid)
            {
                freeSkin(it->second);
                it = mKeyToId.erase(it);
            }
        }
    }
    // Flexi chains: objects gone from the object list.
    if ((mReapFrame & 63u) == 8u)
    {
        std::vector<LLUUID> gone;
        for (const auto& kv : mFlexiSkin)
        {
            LLViewerObject* o = gObjectList.findObject(kv.first);
            if (o && !o->isDead()) { mFlexiDeadSince.erase(kv.first); continue; }
            auto it = mFlexiDeadSince.find(kv.first);
            if (it == mFlexiDeadSince.end()) { mFlexiDeadSince[kv.first] = mReapFrame; continue; }
            if (it->second + kReapFrames <= mReapFrame) gone.push_back(kv.first);
        }
        for (const LLUUID& oid : gone)
        {
            const U32 id = mFlexiSkin[oid];
            mFlexiSkin.erase(oid);
            mFlexiDeadSince.erase(oid);
            auto fc = mFlexiChains.find(id);
            if (fc != mFlexiChains.end())
            {
                mFreeFlexiChains.emplace_back(fc->second, mReapFrame);
                mFlexiChains.erase(fc);
            }
            freeSkin(id);
            ++mReapedFlexi;
        }
    }
    // UNREFERENCED MESH SKINS of live avatars (every 64 frames): no instance row names the
    // entry and no registration asked for it for kReapFrames. Pending (unresolved)
    // entries count as wanted while registerFace keeps re-asking. System-body and flexi
    // entries (no LLMeshSkinInfo) follow their avatar / object reap above instead.
    if ((mReapFrame & 63u) == 40u)
    {
        for (U32 id = 0; id < mLiveCount; ++id)
        {
            const Entry& e = mEntries[id];
            if (!e.skin || mRefs[id] != 0 || mTouch[id] + kReapFrames > mReapFrame) continue;
            auto kit = mKeyToId.find(Key{e.avatar_id, e.skin->mMeshID});
            if (kit != mKeyToId.end() && kit->second == id) mKeyToId.erase(kit);
            freeSkinLocked(id);
            ++mReapedUnused;
        }
    }
    // TRIM the high-water mark: the top ids that are free, past quarantine and
    // unreferenced leave the mirrored / palette range (the GPU buffers keep their size).
    for (;;)
    {
        if (mLiveCount == 0) break;
        const U32 top = mLiveCount - 1;
        size_t at = mFreeSkinIds.size();
        for (size_t i = 0; i < mFreeSkinIds.size(); ++i)
            if (mFreeSkinIds[i].first == top) { at = i; break; }
        if (at == mFreeSkinIds.size()) break;
        if (mFreeSkinIds[at].second + kQuarantineFrames > mReapFrame || mRefs[top] != 0) break;
        mFreeSkinIds[at] = mFreeSkinIds.back();
        mFreeSkinIds.pop_back();
        --mLiveCount;
        ++mTrimmed;
    }
    // Only when something was recycled since the last line, at most every 10 s.
    static U32 s_logged = 0;
    static F64 s_next = 0.0;
    const U32 total = mReapedAvatars + mReapedFlexi + mReapedUnused;
    const F64 now = LLTimer::getTotalSeconds();
    if (total != s_logged && now >= s_next && (s_logged = total, s_next = now + 10.0, true))
        LL_INFOS("CleanSkin") << "recycled: avatars " << mReapedAvatars << " skins " << mReapedSkins
            << " (unreferenced " << mReapedUnused << ") flexi " << mReapedFlexi << " | slots live " << mAvatarSlot.size() << "/" << MAX_AVATARS
            << " (" << mFreeAvatarSlots.size() << " free) skins " << mLiveCount << " (" << mFreeSkinIds.size() << " free)" << LL_ENDL;
}

namespace
{
// Column-major mat4 * mat4 (a, b, out all 16 floats, column-major). out = a * b.
inline void mat4mul_cm(const F32* a, const F32* b, F32* out)
{
    for (int c = 0; c < 4; ++c)          // result column
        for (int r = 0; r < 4; ++r)      // result row
        {
            F32 s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += a[k*4 + r] * b[c*4 + k];   // a[row r, col k] * b[row k, col c]
            out[c*4 + r] = s;
        }
}
} // namespace

U32 LLCleanSkinStore::copyVKAvatarJoints(std::vector<U8>& out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 n = llmin(mAvatarLive, (U32)mAvatarJointsCPU.size());
    out.resize((size_t)n * sizeof(AvatarJoints));
    if (n > 0) std::memcpy(out.data(), mAvatarJointsCPU.data(), out.size());
    return n;
}

U32 LLCleanSkinStore::copyVKAvatarJointRows(std::vector<U8>& out, const std::vector<U8>& want,
                                             std::vector<std::pair<U32, U32>>& runs) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 n = llmin(mAvatarLive, (U32)mAvatarJointsCPU.size());
    out.resize((size_t)n * sizeof(AvatarJoints));   // (a grown tail is zero until wanted)
    const U32 nw = llmin(n, (U32)want.size());
    for (U32 slot = 0; slot < nw; ++slot)
    {
        if (!want[slot]) continue;
        U8* dst = out.data() + (size_t)slot * sizeof(AvatarJoints);
        const void* src = &mAvatarJointsCPU[slot];
        if (std::memcmp(dst, src, sizeof(AvatarJoints)) == 0) continue;
        std::memcpy(dst, src, sizeof(AvatarJoints));
        if (!runs.empty() && runs.back().second == slot) runs.back().second = slot + 1;
        else runs.emplace_back(slot, slot + 1);
    }
    return n;
}

void LLCleanSkinStore::copyVKAvatarAlive(std::vector<U8>& out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    out = mAvatarAliveVK;
}

U32 LLCleanSkinStore::copyVKSkinMetaDirty(std::vector<U8>& mirror, U32& dirty_min, U32& dirty_max)
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 n = llmin(mLiveCount, (U32)mSkinMetaCPU.size());
    const U32 had = (U32)(mirror.size() / sizeof(SkinMeta));
    mirror.resize((size_t)n * sizeof(SkinMeta));
    // Entries new to the mirror are dirty regardless of the store's range.
    U32 lo = mSkinMetaDirtyMin, hi = mSkinMetaDirtyMax;
    if (had < n) { lo = llmin(lo, had); hi = llmax(hi, n); }
    hi = llmin(hi, n);
    if (lo < hi)
        std::memcpy(mirror.data() + (size_t)lo * sizeof(SkinMeta), mSkinMetaCPU.data() + lo,
                    (size_t)(hi - lo) * sizeof(SkinMeta));
    dirty_min = lo; dirty_max = hi;
    mSkinMetaDirtyMin = 0xFFFFFFFFu;
    mSkinMetaDirtyMax = 0;
    return n;
}

U32 LLCleanSkinStore::buildVKPalettes(std::vector<F32>& out, const F32* eye) const
{
    // Mirror cleanSkinPaletteC.glsl on the CPU (VK mode has no GL context to read
    // the palette SSBO back). One SkinBlock per live skin: for each joint j,
    // m = avatars[avatar_idx].world[joint_num] * inv_bind[src_j], packed to the
    // mat3x4 layout cleanSkinPreC reads (col c = (m[c].xyz, m[3].c)), then the
    // bind_shape mat4. Caller must have run refreshAll() this frame first.
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 n = mLiveCount;
    const U32 floats_per_block = VK_SKINBLOCK_BYTES / 4;   // 110*12 + 16
    // Every float of every block is written below: resize (no zero-fill) so a reused
    // caller vector costs nothing per frame.
    out.resize((size_t)n * floats_per_block);
    if (n == 0) return 0;

    static const F32 IDENT16[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    for (U32 skin = 0; skin < n; ++skin)
    {
        if (skin >= mSkinMetaCPU.size()) break;
        const SkinMeta& sm = mSkinMetaCPU[skin];
        F32* block = out.data() + (size_t)skin * floats_per_block;
        F32* cols  = block;                       // MAX_JOINTS * 12 floats (mat3x4)
        F32* bshape= block + MAX_JOINTS * 12;      // mat4

        const U32 jc = sm.joint_count;
        const U32 av = sm.avatar_idx;
        const AvatarJoints* aj =
            (av < mAvatarJointsCPU.size()) ? &mAvatarJointsCPU[av] : nullptr;

        // Only the skin's REAL joints need the matrix math; lanes >= joint_count are a
        // copy of the last one (the tail-fill contract), so compute jc lanes and
        // replicate instead of multiplying 110 matrices per skin per frame.
        const U32 jlanes = (jc > 0) ? std::min<U32>(jc, MAX_JOINTS) : 1u;
        for (U32 j = 0; j < jlanes; ++j)
        {
            // Tail-fill: lanes >= joint_count replicate the last valid joint (keeps
            // custom-skeleton scale baked into the highest-index joint).
            U32 src_j = (j < jc) ? j : (jc > 0 ? jc - 1 : 0);
            S32 jn = sm.joint_nums[src_j];
            if (jn < 0) jn = 0;
            if (jn >= (S32)MAX_AVATAR_JOINTS) jn = MAX_AVATAR_JOINTS - 1;

            F32 wu[16];
            if (aj) unpackJointWorld(wu, aj->world + (size_t)jn * JOINT_WORLD_FLOATS);
            const F32* w  = (aj ? wu : IDENT16);
            const F32* ib = sm.inv_bind + (size_t)src_j * 16;
            F32 wr[16];
            if (eye)
            {
                // Camera-relative BEFORE the multiply: the big translation cancels
                // here once per joint (coherent for every vertex on it), not per
                // skinned vertex in the pre-skin + view-projection.
                std::memcpy(wr, w, sizeof(wr));
                wr[12] -= eye[0]; wr[13] -= eye[1]; wr[14] -= eye[2];
                w = wr;
            }
            F32 m[16];
            mat4mul_cm(w, ib, m);

            // Pack mat3x4: col c = (m[c].xyz, translation component c).
            F32* d = cols + (size_t)j * 12;
            d[0] = m[0]; d[1] = m[1]; d[2] = m[2];  d[3]  = m[12]; // col0.xyz, t.x
            d[4] = m[4]; d[5] = m[5]; d[6] = m[6];  d[7]  = m[13]; // col1.xyz, t.y
            d[8] = m[8]; d[9] = m[9]; d[10]= m[10]; d[11] = m[14]; // col2.xyz, t.z
        }
        for (U32 j = jlanes; j < MAX_JOINTS; ++j)
            std::memcpy(cols + (size_t)j * 12, cols + (size_t)(jlanes - 1) * 12, 12 * sizeof(F32));
        std::memcpy(bshape, sm.bind_shape, sizeof(F32) * 16);
    }
    return n;
}

