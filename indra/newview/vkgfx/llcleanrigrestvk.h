/**
 * @file llcleanrigrestvk.h
 * @brief The avatars' REST JOINT ROWS (#641 / #798): the pairing space of BOTH rigged welds.
 *
 * Rigged welding pairs open-boundary vertices of different meshes of one avatar where they meet
 * at the avatar's REST pose: the bind pose skinned with this avatar's shape (skeletal
 * distortions) and joint overrides, every joint rotation at identity. A rest row holds, per
 * joint-table index j (LLVOAvatar::getJoint(j), 216 joints), the joint's rest world position and
 * its local scale: 8 words {pos xyz, -, scale xyz, -} (float bits); the GLSL side is
 * clean_rig_weld_common.glsl rwRestPos.
 *
 * Users: the legacy weld's rest-pose pairs (llcleanrigweldvk.cpp, RenderVKRigWeldMode 1: rows in
 * its discovery upload ring) and the cluster LOD weld on the cut (llcleanclusterskinvk.cpp: a
 * persistent per-avatar REST TABLE). Defined in llcleanrigweldvk.cpp (render thread only).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#pragma once

#include "lluuid.h"

#include <cstdint>
#include <utility>
#include <vector>

class LLVOAvatar;

namespace LLRigRest
{
constexpr uint32_t kJoints   = 216;
constexpr uint32_t kRowWords = kJoints * 8;   // = clean_rig_weld_common.glsl RW_REST_ROW_WORDS

// The joint-table slots' owners (LLCleanSkinStore::copyAvatarSlots: {avatar id, slot}).
void slotOwners(std::vector<std::pair<LLUUID, uint32_t>>& out);
// The live avatar behind a slot owner (nullptr: gone, dead, not in the object list yet, or no
// avatar at all -- a flexi pseudo slot).
LLVOAvatar* findAvatar(const LLUUID& id);
// The avatar's rest row (kRowWords floats). Reads the joints' xforms only.
void buildRow(LLVOAvatar* av, float* out);
// Change detectors: the skeleton serial (shape sliders; avatar physics bumps it too -- rate-limit)
// and a hash of the BONES' local positions / scales (joint overrides, skeletal distortions; 0.1 mm).
uint32_t skeletonSerial(LLVOAvatar* av);
uint64_t boneHash(LLVOAvatar* av);
}
