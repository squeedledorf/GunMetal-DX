// vkclean -- CLUSTER LOD selection / emission (#745, OWNER: 1b): the layouts 1b owns inside
// the cluster set (clean_cluster_common.glsl) -- the per-view counters, the draw records, the
// per-selected aux words, the request dedupe stamps, the 1b tail of the views buffer and the
// 1b-private ClusterInstance flag bits. C++ twin: vkgfx/llcleanclusterselectvk.h (the same
// constants, documented there for 2a / 2b / 2c). Include AFTER clean_cluster_common.glsl.
//
// Method: published material only (SIGGRAPH 2021 Nanite talk: per-cluster cut test of the
// parent / own group error against a pixel tolerance, culling, indirect emission).
#ifndef CLEAN_CLUSTER_SELECT_COMMON_GLSL
#define CLEAN_CLUSTER_SELECT_COMMON_GLSL
#include "clean_av_slots.glsl"

// ---- 1b-private ClusterInstance::flags bits (8..15; CLI_* use 0..3, CLI_DBG_LOCK_LEVEL 24..31) --
#define CLI1B_DOUBLE_SIDED 256u   // material is double-sided: bucket 1 (cull none), no cone test
#define CLI1B_NO_CONE      512u   // mirrored transform (det < 0) or other reason: no cone test
#define CLI1B_DRAW        1024u   // drawn for real (not only the RenderVKClusterDebug overlay)
#define CLI1B_AVATAR      2048u   // rigged: bits 16..25 hold the wearer's joint-table slot (rig lists)
#define CLI1B_DYN_CASTER  4096u   // static: a moving shadow caster (#567 split: the dynamic overlay's)
#define CLI1B_BLEND       8192u   // rigged ALPHA-BLEND material: its draw records go to the alpha lists only
                                  // (clean_cluster_alpha.comp orders them back to front), never buckets 0 / 1
#define CLI1B_AV_SHIFT      16u

// ---- CL_B_COUNTERS layout: CLC_VIEW_WORDS words per view, then the dedupe stamps ----------
#define CLC_VIEW_WORDS   96u
#define CLC_SELECTED      0u   // selected entries asked for (raw atomic: may exceed sel_cap)
#define CLC_DRAWS0        1u   // bucket 0 (single-sided, cull back) draw records written
#define CLC_DRAWS1        2u   // bucket 1 (double-sided, cull none) draw records written
#define CLC_DISPATCH      4u   // 3 words: { ceil(selected / 64), 1, 1 } per-selected dispatch (2a / 2b)
#define CLC_TASKS0        8u   // VkDrawMeshTasksIndirectCommandEXT, bucket 0 (32 clusters per task group)
#define CLC_TASKS1       12u   // ... bucket 1
#define CLC_SEL_CLAMPED  16u   // min(selected, sel_cap): the valid entries of the view's slice
// SHARED RIG LISTS (main view only; the secondary passes' rigged cluster draws): list L
// (CL_RIG_LIST_*) bucket b = rig index r = L * 2 + b.
#define CLC_RIG_DRAWS    20u   // + r: rig list records written (raw atomic: may exceed the rig cap)
#define CLC_RIG_TASKS    24u   // + r * 4: VkDrawMeshTasksIndirectCommandEXT of rig list r (3 words)
#define CLC_RIG_CULLED   40u   // + bucket: records of the MDI tiers' per-view rig-list cull (clean_cluster_rigcull.comp)
// ALPHA LISTS (main view only, rigged alpha-blend instances, CLI1B_BLEND): every alpha record
// (the main view's visible ones and the rig lists' ones) is STAGED unordered with a tag
// {entry, list, deterministic local index}; clean_cluster_alpha.comp then orders the instances
// back to front and scatters the records into the four alpha output lists (CLA_OUT_*).
#define CLC_AL_STAGED    42u   // staged alpha records asked for (raw atomic: may exceed the cap)
#define CLC_AL_INST      43u   // alpha instance entries asked for (raw atomic)
#define CLC_AL_OVERFLOW  44u   // alpha records dropped (staging / entry table full)
#define CLC_AL_COUNT     45u   // + k: records of alpha output list k (CLA_OUT_*), written by the order pass
#define CLC_AL_DISPATCH  49u   // 3 words: { ceil(staged / 64) groups, rows, 1 } (prep + scatter)
#define CLC_AL_TASKS     52u   // + k * 4: VkDrawMeshTasksIndirectCommandEXT of alpha output list k
#define CLC_AL_SORTED    68u   // entries sorted this frame (= min(entries, entry cap))
#define CLC_AL_MAIN_INST 69u   // main view: alpha instances with a drawn cluster (stats)
#define CLC_AL_MAIN_TRIS 70u   // main view: alpha triangles drawn (stats)
// Alpha output lists (records, same format as the buckets).
#define CLA_OUT_PRE      0u    // main view, far side of the water: pass 2a (before the refraction copy)
#define CLA_OUT_POST     1u    // main view, near side: pass 2b (after the water)
#define CLA_OUT_RIG0     2u    // rig list 0 (shadow casters: unordered use)
#define CLA_OUT_RIG1     3u    // rig list 1 (captures: back to front, pre then post side)
#define CLA_OUTS         4u
// Staged list ids (the tag's list field): 0 main view, 1 + L rig list L.
#define CLA_STAGE_LISTS  3u
#define CLA_ENTRY_WORDS 16u    // entry table: 0 instance, 1 valid, 2..4 box lo (clOrderEnc, atomicMin),
                               // 5..7 box hi (atomicMax), 8..10 records per staged list, 11 main tris,
                               // 12..14 base per staged list in its output, 15 main output (pre 0 / post 1)
#define CL_RIG_LISTS      2u
#define CL_RIG_LIST_SHADOW  0u   // sun cascades + projector tiles (casters)
#define CL_RIG_LIST_CAPTURE 1u   // planar mirror + dynamic reflection-probe faces
#define CLC_DEDUP_BASE   (CL_MAX_VIEWS * CLC_VIEW_WORDS)   // uint[CL_MAX_VIRTUAL_PAGES] request stamps
#define CL_MAX_VIRTUAL_PAGES (1u << 20)                   // = LLClusterFmt::kMaxVirtualPages
#define CL_TASK_CLUSTERS 32u    // clusters per task workgroup (mesh tier)
#define CL_TASK_GRID_X   32768u // task / dispatch grids wrap rows at this many groups

// ---- CL_B_DRAWS layout ------------------------------------------------------------------------
// View v, bucket b: draw records [draw_base + b * draw_cap, + draw count), CLD_WORDS words each:
//   tiers 1 (uint8 indexed) / 2 (mesh): VkDrawIndexedIndirectCommand {index count, 1, first index
//     BYTE in the pool, first vertex (absolute QVertex), first instance = absolute selected index};
//   tier 0 (vertex pulling): VkDrawIndirectCommand {vertex count, 1, first vertex = first index
//     BYTE in the pool, first instance = absolute selected index} + 1 spare word.
// After every slice: one aux word per selected entry at aux_base + absolute selected index:
//   bits 0..7 DAG level, bit 8 drawn as the fallback of a missing finer page, bit 9 its generating
//   group is CLG_FACE_BORDER_FREE (the face's open border -- the weld seam -- was simplified: #798),
//   (#1011, RT CUT SUMMARY below) bit 10 CLA_RANKED: bits 12..26 hold the entry's RANK within its
//   instance's main-view cut.
// Then the SHARED RIG LISTS (cl_aux.rig): rig index r's records at [rig.x + r * rig.y, + count),
// same record format, firstInstance = the MAIN view's absolute selected index (so the cluster
// pre-skin's map covers them: one skinned cut serves every pass).
#define CLD_WORDS 5u
#define CLA_LEVEL_MASK 255u
#define CLA_FALLBACK   256u
#define CLA_BORDER_FREE 512u
#define CLA_RANKED     1024u
#define CLA_RANK_SHIFT   12u
#define CLA_RANK_MASK  0x7FFFu   // (ranks past it are not ranked: such a cut is never derived, > CL_RT_SHARED_CUT)

// ---- RT CUT SUMMARY (#1011; cl_aux.flags bit 0 = CLA_FLAG_RT_SUMMARY, RenderVKClusterRTRigRank) ----
// The derived RT cut (clean_cluster_rt.comp DERIVED RT CUT) of a rigged instance IS its main-view
// shared cut. Its count / order-independent hashes / triangles used to be re-gathered by the RT
// build (mode 11: one lane per selected entry, four same-address atomics each) and its entries
// scattered into arena slots in atomic order (mode 12) -- a new slot order every frame. The main
// view's selection now reduces them per instance while it walks the cut (CL_B_INST_CUT, the
// cluster set's binding 15: CL_INST_CUT_WORDS words per ClusterInstance row {count, hash a, hash b,
// tris, the view's frame + 1, 0, 0, 0}) and RANKS every counted entry: its index in (page index,
// cluster index) order, in the aux word -- a function of the cut set alone, so an unchanged cut maps
// every cluster to the same arena slot frame after frame (the RT CLAS reuse, clean_cluster_rt.comp
// #1011). Counted = a selected entry of a shared instance (main view, in a rig list, drawn for real)
// with triangles: the rule the RT walk and mode 11 apply. Hashes: the RT walk's (clCutHashA / B).
#define CLA_FLAG_RT_SUMMARY 1u
#define CL_INST_CUT_WORDS   8u
uint clMix32(uint h)
{
    h ^= h >> 16u; h *= 0x7feb352du; h ^= h >> 15u; h *= 0x846ca68bu; h ^= h >> 16u;
    return h;
}
uint clCutHashA(uint ref) { return clMix32(ref ^ 0x9E3779B9u); }        // (summed)
uint clCutHashB(uint ref) { return clMix32(ref * 0x85EBCA77u + 1u); }   // (xor-ed)

// ---- the 1b tail of CL_B_VIEWS (after the CL_MAX_VIEWS ClusterView records, 4096 B) ----------
struct ClSelectAux
{
    mat4  occ_view_proj;   // Hi-Z reference: the main view of the frame that built the Hi-Z,
    vec4  occ_origin;      //   (world - occ_origin) -> clip, reversed-Z
    uvec4 hiz;             // x, y depth-buffer size the Hi-Z level 0 halves; z valid; w mip count
    uint  debug_mode;      // RenderVKClusterDebug (0 off, 1 cluster, 2 level, 3 fallback, 4 shaded, 5 seam)
    uint  aux_base;        // first per-selected aux word in CL_B_DRAWS
    uint  tier;            // raster tier in force (0 pull, 1 uint8, 2 mesh)
    uint  flags;           // bit 0 CLA_FLAG_RT_SUMMARY (#1011): the main view writes CL_B_INST_CUT + the ranks
    // SHARED RIG CUT (the main view's recordSelect writes it): x = first rig record (records,
    // CL_B_DRAWS), y = records per rig list and bucket, z = bit 0: the main view selects the
    // un-culled cut of every avatar in a rig list and emits the rig lists, w = bit L: rig list L's
    // RECORDS are emitted (#748 perf pass: a list no pass draws this frame -- the shadow list while
    // no shadow map draws rigged casters -- keeps its avatars' un-culled cut for the shared skinning /
    // RT, without its records).
    uvec4 rig;
    // Per rig list: AV_SLOT_CAP bits, one per joint-table slot (the avatar is drawn in that list's
    // passes this frame: the renderer's per-pass avatar visibility). rig_bits[L * 8 + w / 4][w & 3].
    uvec4 rig_bits[2u * AV_SLOT_WORDS / 4u];
    // ALPHA LISTS (CLI1B_BLEND): x = first alpha record (CL_B_DRAWS records: the staging region,
    // then the CLA_OUTS output lists, alpha_cap records each), y = alpha_cap (records), z = the
    // entry table's capacity (alpha instances, pow2), w = bit 0 on. Word bases: clAlphaTagBase()
    // (2 words per staged record: entry | list << 30, local index), clAlphaEntryBase()
    // (CLA_ENTRY_WORDS per entry), clAlphaSortBase() (4 words per entry: the sort keys).
    uvec4 alpha;
};
layout(std430, set = CLUSTER_SET, binding = CL_B_VIEWS) readonly buffer ClViews1b
{
    ClView      cl_views[CL_MAX_VIEWS];
    ClSelectAux cl_aux;
};
uint clAlphaStageRec()        { return cl_aux.alpha.x; }
uint clAlphaOutRec(uint k)    { return cl_aux.alpha.x + (1u + k) * cl_aux.alpha.y; }
uint clAlphaTagBase()         { return (cl_aux.alpha.x + (1u + CLA_OUTS) * cl_aux.alpha.y) * CLD_WORDS; }
uint clAlphaEntryBase()       { return clAlphaTagBase() + 2u * cl_aux.alpha.y; }
uint clAlphaSortBase()        { return clAlphaEntryBase() + CLA_ENTRY_WORDS * cl_aux.alpha.z; }

#ifdef CL_SELECT_WRITE
layout(std430, set = CLUSTER_SET, binding = CL_B_SELECTED) writeonly buffer ClSelectedW { ClSelected cl_sel[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_COUNTERS) coherent buffer ClCountersW { uint cl_cnt[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_DRAWS) writeonly buffer ClDrawsW { uint cl_draws[]; };
#elif defined(CL_SELECT_RW)
// (clean_cluster_alpha.comp: reads and writes the counters, records and alpha tables)
layout(std430, set = CLUSTER_SET, binding = CL_B_SELECTED) readonly buffer ClSelectedR { ClSelected cl_sel[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_COUNTERS) coherent buffer ClCountersRW { uint cl_cnt[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_DRAWS) coherent buffer ClDrawsRW { uint cl_draws[]; };
#else
layout(std430, set = CLUSTER_SET, binding = CL_B_SELECTED) readonly buffer ClSelectedR { ClSelected cl_sel[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_COUNTERS) readonly buffer ClCountersR { uint cl_cnt[]; };
layout(std430, set = CLUSTER_SET, binding = CL_B_DRAWS) readonly buffer ClDrawsR { uint cl_draws[]; };
#endif

#ifdef CL_DECLARE_INST_CUT
#ifdef CL_SELECT_WRITE
layout(std430, set = CLUSTER_SET, binding = CL_B_INST_CUT) writeonly buffer ClInstCutW { uint cl_inst_cut[]; };
#else
layout(std430, set = CLUSTER_SET, binding = CL_B_INST_CUT) readonly buffer ClInstCutR { uint cl_inst_cut[]; };
#endif
#endif

// The rig lists an instance belongs to this frame (bit L), 0 = none / sharing off.
uint clRigLists(uint inst_flags)
{
    if ((cl_aux.rig.z & 1u) == 0u || (inst_flags & (CLI1B_DRAW | CLI1B_AVATAR)) != (CLI1B_DRAW | CLI1B_AVATAR)) return 0u;
    const uint av = (inst_flags >> CLI1B_AV_SHIFT) & AV_SLOT_MASK;
    const uint w = av >> 5u;
    uint m = 0u;
    for (uint l = 0u; l < CL_RIG_LISTS; ++l)
        if (((cl_aux.rig_bits[l * (AV_SLOT_WORDS / 4u) + (w >> 2u)][w & 3u] >> (av & 31u)) & 1u) != 0u) m |= 1u << l;
    return m;
}

// Debug colours (RenderVKClusterDebug): a hash colour per cluster, a ramp per DAG level, and
// red / green for "parent drawn because the finer page is missing" / "own level".
vec3 clDebugHash(uint h)
{
    h ^= h >> 16u; h *= 0x7feb352du; h ^= h >> 15u; h *= 0x846ca68bu; h ^= h >> 16u;
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) * (0.75 / 255.0) + 0.2;
}
vec3 clDebugLevel(uint level)
{
    const vec3 k[8] = vec3[8](vec3(0.1, 0.35, 1.0), vec3(0.1, 0.8, 0.95), vec3(0.15, 0.9, 0.25), vec3(0.75, 0.95, 0.1),
                              vec3(1.0, 0.8, 0.1), vec3(1.0, 0.45, 0.05), vec3(0.95, 0.1, 0.1), vec3(0.9, 0.1, 0.9));
    return k[min(level, 7u)];
}
vec3 clDebugColour(uint mode, uint cluster_ref, uint instance, uint aux)
{
    if (mode == 1u) return clDebugHash(cluster_ref * 0x9E3779B1u + instance * 0x85EBCA77u);
    if (mode == 2u) return clDebugLevel(aux & CLA_LEVEL_MASK);
    if (mode == 3u) return (aux & CLA_FALLBACK) != 0u ? vec3(1.0, 0.1, 0.05) : vec3(0.15, 0.75, 0.2);
    // 5 SEAM (#798): red = the face's open border is simplified in this cluster (its weld vertices
    // are partly gone: a seam to the next mesh cannot close), orange = that AND drawn as the fallback
    // of a missing finer page, grey = border intact (every LOD0 weld vertex present).
    if (mode == 5u)
    {
        if ((aux & CLA_BORDER_FREE) == 0u) return vec3(0.55);
        return (aux & CLA_FALLBACK) != 0u ? vec3(1.0, 0.55, 0.0) : vec3(1.0, 0.05, 0.05);
    }
    return vec3(1.0);
}
// The debug views that colour the clusters (RenderVKClusterDebug 1..3, 5, 6; 4 draws them shaded).
bool clDebugTinted(uint mode) { return (mode >= 1u && mode <= 3u) || mode == 5u || mode == 6u; }

#endif
