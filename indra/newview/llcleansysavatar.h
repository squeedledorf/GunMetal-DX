/**
 * @file llcleansysavatar.h
 * @brief Vulkan ingest of the classic SYSTEM AVATAR BODY (LLPolyMesh head / upper /
 *        lower / eyes / eyelashes / hair / skirt) into the GPU-driven rigged pipeline.
 *
 * GEOMETRY. Each (body part, polymesh LOD) is ONE mesh-store slot shared by every
 * avatar: the reference mesh's BASE (unmorphed) vertices + the LOD's face list + weight4
 * encoded against a global "system body palette" (LL's polymesh skinning blends a
 * vertex between joint render entry j and j + 1 -- linear blend skinning with two
 * influences, each entry's matrix = joint world * T(skin pivot); eyeballs are rigid to
 * their eye joint). One LOD group per (part, LOD) holding that single slot: the LOD is
 * picked per avatar on the CPU with LL's pixel-area rule (LLAvatarJoint::updateLOD) and
 * written into the part's instance only when it changes.
 *
 * MORPHS (shape sliders, blinks, hand poses, avatar physics). LL keeps applying visual
 * params into each avatar's LLPolyMesh on the CPU (mCoords / mNormals / mTexCoords).
 * The avatar's morphed REFERENCE vertex array is uploaded as its own slot -- only when
 * LLPolyMesh::mMorphGen moved (event-driven, budgeted per frame) -- and the cull points
 * the pre-skin at it (instance flag bit 17, slot id in uv_matrix[3][2]): indices and
 * weights stay shared, the per-avatar data is the vertex stream only. A GPU morph pass
 * (delta tables x per-avatar weights) is the follow-up; see the .cpp.
 *
 * SKINNING. One LLCleanSkinStore entry per avatar (sysBodySkin): joint_nums into the
 * avatar's joint table (GPU anim / CPU pose), inv_bind = the per-entry pivot (or a rigid
 * eyeball's mesh-local transform), identity bind shape -- so the standard palette
 * compute + pre-skin + rigged buckets draw it, and av_vis (frustum, policy hide, reduced
 * tier, no-shadow, skin cache) applies through the skin meta's avatar slot.
 *
 * Main thread, at the ingest sync point (LLCleanIngest::vkIngestTick).
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"

#include <string>
#include <unordered_map>
#include <vector>

class LLVOAvatar;
class LLAvatarJointMesh;

class LLCleanSysAvatar
{
public:
    static LLCleanSysAvatar& instance();
    void tick();

    static constexpr S32 kParts   = 8;     // MESH_ID_NUM_INDICES
    static constexpr S32 kMaxLod  = 6;     // hair has 6 polymesh LODs
    static constexpr S32 kFaceBase = 0x5A500;   // instance face index = kFaceBase + part
    static constexpr U32 kInstFlag = 131072u;   // bit 17: morph-stream source (clean_cull.comp)
    static constexpr U32 kHeadFlag = 262144u;   // bit 18: own head part (clean_draw_rigged.vert)

private:
    LLCleanSysAvatar() = default;

    // Shared (part, LOD) geometry.
    struct Shared
    {
        U32 slot = 0xFFFFFFFFu, group = 0xFFFFFFFFu, nv = 0;
        F32 lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};
        bool failed = false;
    };
    Shared mShared[kParts][kMaxLod];

    // Global system-body palette: entry -> how each avatar resolves its joint + inv bind.
    struct PalEntry
    {
        std::string joint;   // key (with pivot): skinned entries of one joint + pivot merge
        F32  pivot[3] = {0.f, 0.f, 0.f};
        bool rigid = false;
        S32  part = 0;
        S32  lod = 0;        // rigid: the LOD mesh; skinned: the reference mesh's LOD index
        S32  jrd = 0;        // skinned: joint render data index in the reference mesh
    };
    std::vector<PalEntry> mPal;
    U32 mPalGen = 1;
    std::vector<U8> mJrdPal[kParts][kMaxLod];   // per reference mesh: jrd index -> palette
    S16 mRigidPal[kParts][kMaxLod];
    bool mPalInit = false;

    struct Morph { U32 slot = 0xFFFFFFFFu; U32 gen = 0; LLUUID key; };
    struct Part
    {
        bool live = false;
        U64  sig = 0;
        LLUUID tex;
        U32  material = 0xFFFFFFFFu;
    };
    struct AvState
    {
        U32 seen = 0;
        U32 skin_id = 0xFFFFFFFFu;
        U32 skin_pal_gen = 0;
        U32 skin_frame = 0;
        Part parts[kParts];
        U8 why[kParts] = {255,255,255,255,255,255,255,255};   // #461 last logged state per part
        Morph morph[kParts][kMaxLod];   // per REFERENCE mesh (part, its LOD index)
    };
    std::unordered_map<LLUUID, AvState> mAv;
    U32 mFrame = 0;
    U32 mLastDrawnAvatars = 0;
    bool mWasEnabled = false;

    void noteWhy(LLVOAvatar* av, AvState& st, S32 part, U8 why);
    bool ensurePalette(LLVOAvatar* av, S32 part, S32 ref_lod);
    bool ensureRigidPal(LLVOAvatar* av, S32 part, S32 lod);
    bool ensureShared(LLVOAvatar* av, S32 part, S32 lod);
    // Returns the morph-stream slot (uploading when the mesh's morph gen moved); INVALID
    // while not uploaded (budget) or on failure.
    U32  ensureMorph(LLVOAvatar* av, AvState& st, S32 part, S32 ref_lod, U32& budget, U32& budget_first,
                     U32& uploads);
    bool buildSkin(LLVOAvatar* av, AvState& st);
    void hidePart(const LLUUID& id, AvState& st, S32 part);
    void releaseAvatar(const LLUUID& id, AvState& st);
};
