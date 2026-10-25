// vkclean -- the avatar (joint-table) slot cap, ONE definition for every shader that indexes a
// per-avatar table (av_vis, the joint table, the cluster rig lists / per-avatar rows, the weld's
// per-avatar words, the cel selection bitset). C++ twin: LLCleanSkinStore::MAX_AVATARS (=
// LLCleanRendererVK::kAvVisSlots, LLCleanClusterSkinVK::kMaxAvatars). 2026-10-14: 256 -> 1024
// (63 animesh + avatars filled 256 slots at Cloudsdale; right-sizing is #762).
#ifndef CLEAN_AV_SLOTS_GLSL
#define CLEAN_AV_SLOTS_GLSL
#define AV_SLOT_CAP   1024u
#define AV_SLOT_WORDS 32u      // AV_SLOT_CAP / 32: one bit per slot
#define AV_SLOT_MASK  1023u    // a slot field of AV_SLOT_BITS bits
#define AV_SLOT_BITS  10u
// The legacy rigged skin-cache STATE word (clean_cull.comp rig_cache word RC_STATE): the slot in
// bits 0..11, then the flags (were bits 8..10 with an 8-bit slot).
#define RC_ST_SLOT    0xFFFu
#define RC_ST_LOCAL   0x1000u  // drawn from the cache (pelvis-local vertices of the slot's avatar)
#define RC_ST_SKINNED 0x2000u  // re-skinned this frame (prev positions written)
#define RC_ST_CLUSTER 0x4000u  // the cluster LOD path draws it in the MAIN view (#747)
#endif
