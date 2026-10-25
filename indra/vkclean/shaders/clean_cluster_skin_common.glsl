// vkclean -- CLUSTER LOD skinning (#747, OWNER: 2a): the layouts 2a owns inside the cluster
// set (clean_cluster_common.glsl) and their readers' helpers. C++ twin:
// vkgfx/llcleanclusterskinvk.h (the same constants). Include AFTER clean_cluster_common.glsl.
//
//   16 CL_B_SKIN_OUT      uint[]  the SKINNED CLUSTER ARENA: per skinned cluster `s` 64 compact
//                                 skinned vertices (clean_skinvert.glsl SkinVertex, 5 words) at
//                                 [(s * 64 + local) * 5, + 5): local = the vertex's index inside
//                                 its cluster (0 .. vert_count - 1). Positions are EYE-RELATIVE
//                                 to the palettes' eye (header CLK_EYE), exactly the legacy
//                                 pre-skin's output space. Then the PREVIOUS-POSITION region
//                                 (integration 2a + 2c, the motion pre-pass): 3 floats per vertex
//                                 at [cap * 64 * 5 + (s * 64 + local) * 3, + 3), cap = header
//                                 CLK_CAP, eye-relative to THIS frame's eye (clSkinPrevPos);
//                                 written on motion frames only (its only reader).
//   17 CL_B_SKIN_SCRATCH  uint[]  header (CLK_* words below) | per-ClusterInstance word at
//                                 CLK_INST_BASE + instance: the posed-bound DISTANCE MARGIN in
//                                 metres (float bits; rigged instances, written every frame
//                                 before the selection) | per-selected-entry map at
//                                 header[CLK_MAP_BASE] + absolute selected index: the skinned
//                                 cluster `s` of that entry, CL_NONE = not skinned (static, not
//                                 drawn for real, or the arena was full) | the owned bitset |
//                                 the weld grid.
// Readers: 1b selection (margins: posed culling / distance), the cluster draw (map + arena),
// 2b RT (the margins of its rigged RT cut; the map + arena: the rigged CLAS input / hits of the
// instances the shared rig cut covers whole, #748).
#ifndef CLEAN_CLUSTER_SKIN_COMMON_GLSL
#define CLEAN_CLUSTER_SKIN_COMMON_GLSL
#include "clean_av_slots.glsl"

// (CL_B_SKIN_OUT = 16 / CL_B_SKIN_SCRATCH = 17: clean_cluster_common.glsl)

// ---- CL_B_SKIN_SCRATCH header (words) -----------------------------------------------------
#define CLK_HDR_WORDS     48u
#define CLK_USED           0u   // selected entries given an arena slot this frame (kept + new; step 1 ALLOC)
#define CLK_CAP            1u   // arena capacity in clusters
#define CLK_OVERFLOW       2u   // entries refused a slot (free list empty: the arena grows from LIVE + ALLOC_N)
#define CLK_SKIN_VERTS     3u   // vertices skinned this frame (stats)
#define CLK_LEGACY_VERTS   4u   // legacy pre-skin vertices of the instances the cluster path owns
#define CLK_LEGACY_ITEMS   5u   // ... and their legacy work items
#define CLK_WELD_N         6u   // weld entries appended (may exceed the weld cap)
#define CLK_WELD_DROP      7u   // ... dropped (over the cap)
#define CLK_WELD_APPLIED   8u   // vertices the weld moved
#define CLK_MAP_BASE       9u   // first map word
#define CLK_SEL_BASE      10u   // the skinned view's sel_base
#define CLK_SEL_COUNT     11u   // its valid selected entries
#define CLK_EYE           12u   // 3 words: the palettes' eye (float bits)
#define CLK_FRAME         15u   // frame of the last pre-skin
#define CLK_DISPATCH      16u   // 3 words: indirect args of the pre-skin (one group per entry)
#define CLK_SKIN_CLUSTERS 19u   // (unused since 2026-10-14: the host derives the skinned clusters from CLK_USED)
#define CLK_ALLOC_TAKEN   19u   // (#1010, the word above reused) free-list slots the TAKE pass handed out
#define CLK_MAP_CAP       20u   // map words (entries at or past it are never skinned / read)
#define CLK_WELD_SKIP     21u   // weld entries skipped: their avatar had a dropped insert this frame
#define CLK_WELD_DISPATCH 22u   // 3 words: indirect args of the weld's two passes (one group per 64
                                // entries written; the pre-skin raises x, the ARGS pass zeroes it)
// STABLE SLOTS (2026-10-14 step 1, clean_cluster_preskin_prep.comp modes 4 / 5):
#define CLK_FREE_N        25u   // free arena slots this frame (the sweep's free list length)
#define CLK_ALLOC_N       26u   // slots the ALLOC pass asked for (cut entries new to the arena; may exceed CLK_FREE_N)
#define CLK_LIVE          27u   // slots the sweep kept (used within the grace period)
#define CLK_ALLOC_DISPATCH 28u  // 3 words: indirect args of the ALLOC pass (one lane per selected entry)
#define CLK_REUSED        31u   // (step 3) slots whose skinned vertices were reused this frame
// WELD AVATAR-DROP BITS: CLW_AV_WORDS words right before the weld grid's cell heads (push
// weld_base - CLW_AV_WORDS; host-zeroed each frame), bit (avatar key & AV_SLOT_MASK) set by an insert that
// found the entry list full. The weld then skips EVERY entry of that avatar: an avatar is welded
// whole or not at all that frame -- never one side of a boundary without the other (a partial,
// atomic-order weld left lighting lines along mesh joins, 2026-10-14).
#define CLW_AV_WORDS       AV_SLOT_WORDS
// SEAM DIAGNOSTICS (#798; sampled with the rest of the header every frame, logged on the CLOD skin
// line). Weld resolve outcomes per candidate: no vertex of another mesh within eps in the pairing
// space (the avatar's rest pose, or bind space in RenderVKRigWeldMode 0) / some within eps but none
// with matching skin weights (#626) / matching but none within max gap. PAIRED = candidates with a
// matching partner in the pairing space (gap or not); NOREST = boundary vertices not inserted
// because their avatar has no rest row yet (rest-pose pairing only; counted at the insert).
// Candidates of clusters whose GENERATING group is CLG_FACE_BORDER_FREE (the face's open border
// was simplified there: some of its LOD0 weld vertices are gone, their partners have nothing to
// weld to -- a sawtooth seam) and the number of avatars with any (bits CLW_BF_AV, CLW_AV_WORDS
// words right before the drop bits, cleared by ARGS).
#define CLK_WELD_NOPAIR   32u
#define CLK_WELD_WREJ     33u
#define CLK_WELD_GREJ     34u
#define CLK_WELD_BFREE    35u
#define CLK_WELD_BFREE_AV 36u
#define CLK_WELD_PAIRED   37u
#define CLK_WELD_NOREST   38u
// (#798 resolve) candidates left alone because the blend equals the vertex (NO-OP: seamless), and
// candidates with more than one partner (other meshes, or distinct vertices of one mesh within eps)
#define CLK_WELD_NOOP     39u
#define CLK_WELD_MULTI    40u
// #826 WELD PAIR CACHE: entries applied from their row's stored pairs (no search) / their stored
// partner failed validation this frame (left its slot or the cut: not welded to it) / search entries
// (rows re-paired this frame). Counted on the sampled stats frames, like the #798 diagnostics.
#define CLK_WELD_CACHED   41u
#define CLK_WELD_INVALID  42u
#define CLK_WELD_SEARCHED 43u
// #1010 WHOLE-AVATAR ARENA ADMISSION (clean_cluster_preskin_prep.comp modes 5 / 7 / 8): the arena
// never hands out a random subset of a frame's new clusters. ALLOC (mode 5) only COUNTS the entries
// new to the arena, per avatar row; GRANT (mode 7) -- when they exceed the free list (SHORT) --
// walks the rows in the host's PRIORITY ORDER (own avatar first, then by on-screen size) and refuses
// every row past the point where the free list runs out; TAKE (mode 8) hands slots to the new
// entries of the rows not refused and hides EVERY entry of a refused row (its kept slots too: map
// CL_NONE). An avatar is drawn whole or not at all; the host's arena controller coarsens the least
// important avatars' cuts so the cut fits again (llcleanclusterskinvk.h ARENA PRESSURE).
#define CLK_PRIO_BASE     44u   // scratch word of the PRIORITY region below (CL_NONE = none: no row refused)
#define CLK_SHORT         45u   // 1 = this frame's new entries exceeded the free list (GRANT decided rows)
#define CLK_REFUSED_AV    46u   // avatar rows refused whole this frame
#define CLK_HIDDEN        47u   // entries not drawn because their avatar row was refused
// PRIORITY region (scratch, at header[CLK_PRIO_BASE]): [0] n, [1, 1 + CL_SKIN_AV_ROWS) the host's row
// order (most important first; n valid), then per row the entries new to the arena this frame (ALLOC
// counts; ARGS zeroes), then per row 1 = refused (GRANT writes; ARGS zeroes). = C++ kPrioWords.
#define CL_SKIN_PRIO_WORDS (1u + 3u * (AV_SLOT_CAP + 1u))
#define CLK_INST_BASE     48u   // per-ClusterInstance margin words start here

#ifdef CL_DECLARE_SKIN_OUT
#ifndef CL_SKIN_OUT_QUAL
#define CL_SKIN_OUT_QUAL readonly
#endif
layout(std430, set = CLUSTER_SET, binding = CL_B_SKIN_OUT) CL_SKIN_OUT_QUAL buffer ClSkinOut { uint cl_skin_out[]; };
#endif
#ifdef CL_DECLARE_SKIN_SCRATCH
#ifndef CL_SKIN_SCRATCH_QUAL
#define CL_SKIN_SCRATCH_QUAL readonly
#endif
layout(std430, set = CLUSTER_SET, binding = CL_B_SKIN_SCRATCH) CL_SKIN_SCRATCH_QUAL buffer ClSkinScratch { uint cl_rig[]; };

// The posed-bound margin of a rigged instance (metres, >= 0; huge / non-finite clamped).
float clRigMargin(uint instance)
{
    const uint w = CLK_INST_BASE + instance;
    if (w >= uint(cl_rig.length())) return 0.0;
    const float m = uintBitsToFloat(cl_rig[w]);
    return (m >= 0.0 && m < 1.0e5) ? m : 1.0e5;
}
// The skinned cluster of an absolute selected index (CL_NONE = not skinned).
uint clSkinnedCluster(uint sel)
{
    if (sel >= cl_rig[CLK_MAP_CAP]) return CL_NONE;
    const uint w = cl_rig[CLK_MAP_BASE] + sel;
    return w < uint(cl_rig.length()) ? cl_rig[w] : CL_NONE;
}
// First word of the previous-position region (CL_B_SKIN_OUT; = cap * 64 * SkinVertex words).
uint clSkinPrevBase() { return cl_rig[CLK_CAP] * CL_MAX_CLUSTER_VERTS * 5u; }
vec3 clSkinEye() { return vec3(uintBitsToFloat(cl_rig[CLK_EYE]), uintBitsToFloat(cl_rig[CLK_EYE + 1u]), uintBitsToFloat(cl_rig[CLK_EYE + 2u])); }

// ---- SKINNED BOUNDS (#748 perf pass: the rig-list cull) ----------------------------------------
// After the previous-position region of CL_B_SKIN_OUT, written by the cluster pre-skin every
// frame it runs (eye-relative, like the vertices):
//   per skinned cluster s, CL_SKIN_BOX_WORDS words at clSkinBoxBase() + s * 8: lo.xyz, hi.xyz
//     (float bits; padded by the weld's max gap: the weld moves vertices after the box is taken),
//     the per-avatar row of its instance (= the joint-table slot, CL_SKIN_AV_ROWS - 1 = none), 1;
//   per avatar row, 8 words at clSkinAvBoxBase() + row * 8: the union of its clusters' boxes as
//     {~clOrderEnc(lo).xyz, clOrderEnc(hi).xyz, 0, 0} (atomicMax both; the host zeroes the
//     region before the pre-skin: a 0 word = no cluster this frame, unknown).
// A rig-list record is tested against its AVATAR's box first (one test for every record of an
// avatar outside the view; the words stay cached across its records), then its cluster's own.
#define CL_SKIN_BOX_WORDS 8u
#define CL_SKIN_AV_ROWS (AV_SLOT_CAP + 1u)
// ---- STABLE SLOTS (2026-10-14 step 1) ------------------------------------------------------------
// A skinned cluster keeps its arena slot `s` for as long as it stays in the cut (+ a grace period):
// after the per-avatar boxes, per slot CL_SKIN_META_WORDS words of persistent META --
//   [0] render instance, [1] cluster mesh (CL_NONE = free), [2] mesh generation, [3] skin id,
//   [4] mesh-local cluster id (page header page_index x 64 + cluster index): THE KEY;
//   [5] frame last in the cut (CL_NONE = free), [6] (step 3) skinned pose generation,
//   [7] frame last skinned --
// then the per-frame FREE LIST (cap words) and the per-frame KEY HASH (clSkinHashWords(cap) words:
// open addressing, linear probing, slot ids; rebuilt by the sweep every frame from the live slots).
// All three sized by the arena capacity cap (header CLK_CAP); the host fills the META with
// CL_NONE whenever it (re)creates the arena (every slot free, everything re-skinned once).
#define CL_SKIN_META_WORDS 8u
uint clSkinBoxBaseC(uint cap)   { return cap * CL_MAX_CLUSTER_VERTS * 8u; }   // (5 + 3 words per vertex before it)
uint clSkinAvBoxBaseC(uint cap) { return clSkinBoxBaseC(cap) + cap * CL_SKIN_BOX_WORDS; }
uint clSkinMetaBaseC(uint cap)  { return clSkinAvBoxBaseC(cap) + CL_SKIN_AV_ROWS * CL_SKIN_BOX_WORDS; }
uint clSkinFreeBaseC(uint cap)  { return clSkinMetaBaseC(cap) + cap * CL_SKIN_META_WORDS; }
uint clSkinHashBaseC(uint cap)  { return clSkinFreeBaseC(cap) + cap; }
// 2 x the next power of two of cap (load factor <= 1/2); = LLCleanClusterSkinVK::hashWords.
uint clSkinHashWords(uint cap)  { return cap <= 1u ? 2u : (2u << uint(findMSB(cap - 1u) + 1)); }
uint clSkinKeyHash(uint ri, uint mesh, uint gen, uint skin, uint local)
{
    uint h = ri * 0x9E3779B1u ^ mesh * 0x85EBCA77u ^ gen * 0xC2B2AE3Du ^ skin * 0x27D4EB2Fu ^ local * 0x165667B1u;
    h ^= h >> 15u;
    h *= 0x2c1b3c6du;
    h ^= h >> 12u;
    return h;
}
// ---- CHARACTER SPACE (2026-10-14 step 2) ----------------------------------------------------------
// The arena holds positions, normals, tangents and boxes in each CHARACTER's space, not eye-relative:
// the frame of its avatar's PELVIS (joint 0 of its joint-table row, columns normalised -- the #632
// held-frame cache's frame, clean_preskin.comp rigidRot, and the RT's avatar-owned BLAS frame), so a
// HELD pose's skinned vertices do not change while the character walks / turns or the camera moves.
// After the key hash, per character row (joint-table slot; row CL_SKIN_AV_ROWS - 1 = none: identity,
// i.e. eye-relative, for an instance without an avatar) CL_SKIN_CHAR_WORDS words, written every frame
// by clean_cluster_preskin_prep.comp mode 6 before the pre-skin:
//   [0, 9)   M  (mat3, column-major): character -> world rotation (this frame)
//   [9, 12)  t  : the pelvis position - THIS frame's eye (palettes' eye; formed in float in agent
//               space exactly as clean_skin_palette.comp forms its eye-relative translations)
//   [12, 21) Mp, [21, 24) tp : the same of the PREVIOUS frame's joints, relative to THIS frame's eye
//               (= the previous-frame palettes' convention; = M, t without joint history)
//   [24, 33) M^-1 (the pre-skin's world -> character), [33] 1 = an avatar frame (0 = identity)
// eye-relative position = M * local + t (+ eye = world). Every mesh / cluster level of a character
// uses its one row: seams stay watertight (the same float ops on the same welded values).
#define CL_SKIN_CHAR_WORDS 36u
uint clSkinCharBaseC(uint cap) { return clSkinHashBaseC(cap) + clSkinHashWords(cap); }
// ---- HELD POSES (2026-10-14 step 3) --------------------------------------------------------------
// After the character table: per row its POSE GENERATION (host-written every frame: a new, globally
// unique value whenever the row's pose relative to its pelvis may have changed -- anything but a
// frame LLCleanAnimStore PROVED held, skin meta unchanged, the 64-frame refresh; row none: every
// frame), then CL_SKIN_NEW_WORDS words of NEW bits (ARGS zeroes them; ALLOC sets a row's bit when a
// cluster of it enters the cut). A slot whose meta [6] (generation it was skinned at) equals its
// row's generation holds this frame's pose already: the pre-skin REUSES it (no skinning, no
// previous positions: the draw takes the previous position from the row's previous frame, Mp / tp)
// -- unless the weld is on and its row has a new cluster (then the row is re-skinned whole, so a
// seam never welds an old welded vertex against a fresh unwelded one).
#define CL_SKIN_NEW_WORDS ((CL_SKIN_AV_ROWS + 31u) / 32u)
uint clSkinGenBaseC(uint cap) { return clSkinCharBaseC(cap) + CL_SKIN_AV_ROWS * CL_SKIN_CHAR_WORDS; }
uint clSkinNewBaseC(uint cap) { return clSkinGenBaseC(cap) + CL_SKIN_AV_ROWS; }
// ---- WELD STATUS per slot (#798 diagnostics, RenderVKClusterDebug 6) ----------------------------
// After the new bits: one word per arena slot, written when the slot is SKINNED (a reused slot keeps
// its last one, as it keeps its welds): bits 0..7 the weld candidates it inserted, 8..15 those the
// weld moved, 16..23 those with no other mesh's vertex within eps in the pairing space, 24..27 those
// rejected by the skin weights / the max gap, 28..31 the NO-OPs (every partner already seamless; both
// 4-bit, saturating). (The weld adds its outcomes only with push slot_stats.)
uint clSkinWeldStatBaseC(uint cap) { return clSkinNewBaseC(cap) + CL_SKIN_NEW_WORDS; }
// ---- WELD PAIR CACHE (#826: pair once, apply per frame) -------------------------------------------
// The weld's PAIRS are a function of the rest pose (the pairing space), the skin weights and which
// candidates are in the cut -- not of the animated pose. A cluster keeps its arena slot while it stays
// in the cut (STABLE SLOTS), so an arena vertex index (slot x 64 + local) IS a stable id for a weld
// candidate for as long as both it and its partner stay in the cut; the boundary vertices are locked
// at their LOD0 positions in every level, so a cut change re-pairs onto the same physical vertices.
// After the weld status, all persistent (never cleared per frame):
//   PAIR      one word per arena vertex (cap x 64): the candidate's partner -- the arena vertex of
//             its one other-mesh partner (the search's choice), CL_NONE = none; written by every
//             SEARCH entry (clean_cluster_preskin_weld.comp mode 0), read by CACHED entries
//   ALLOC     one word per slot: the frame the slot's key was last assigned (ALLOC on a miss); a
//             partner is valid only while its slot was assigned no later than the row's pair frame
//             and it is in the cut this frame (meta [5] = this frame)
//   ROW PAIR  2 words per row: [0] the frame of its last complete pairing (CL_NONE = none / invalid),
//             [1] the last frame a pairing found a candidate with more than one partner mesh (the
//             one-word pair cannot hold it: the row SEARCHES every frame, as before #826)
//   REPAIR    CL_SKIN_NEW_WORDS words, host-written when non-zero (the rows whose rest row changed
//             this frame; every row on a pairing-parameter change or a cache re-enable)
// A row is CACHED (no rest-pose evaluation, no grid insert, no search: its entries only re-apply
// their stored partner) when: the cache is on, no cluster of the row is new to the cut (new bits,
// which include a kept slot skinned at another pose generation), no host repair bit, its pair frame
// is valid and no multi-partner candidate was found at that pairing. Otherwise it is a SEARCH row:
// the pre-skin inserts its candidates into the grid as before, and the weld writes their pairs.
uint clSkinPairBaseC(uint cap)      { return clSkinWeldStatBaseC(cap) + cap; }
uint clSkinAllocFrameBaseC(uint cap) { return clSkinPairBaseC(cap) + cap * CL_MAX_CLUSTER_VERTS; }
uint clSkinRowPairBaseC(uint cap)   { return clSkinAllocFrameBaseC(cap) + cap; }
uint clSkinRepairBaseC(uint cap)    { return clSkinRowPairBaseC(cap) + CL_SKIN_AV_ROWS * 2u; }
// The character row of a ClusterInstance (flags = CLI1B_AVATAR 2048 | slot << CLI1B_AV_SHIFT 16,
// clean_cluster_select_common.glsl; the same row the skinned boxes carry in word 6).
uint clSkinRow(uint inst_flags)
{
    return (inst_flags & 2048u) != 0u ? ((inst_flags >> 16u) & AV_SLOT_MASK) : CL_SKIN_AV_ROWS - 1u;
}
struct ClChar { mat3 m; vec3 t; };
// eye-relative AABB of a character-space box (conservative: + a few ulps of the extent / offset)
void clCharBox(ClChar c, inout vec3 lo, inout vec3 hi)
{
    const vec3 ctr = 0.5 * (lo + hi), ext = 0.5 * (hi - lo);
    const vec3 nc = c.m * ctr + c.t;
    const vec3 ne = abs(c.m[0]) * ext.x + abs(c.m[1]) * ext.y + abs(c.m[2]) * ext.z;
    const vec3 pad = ne * 1.0e-5 + abs(nc) * 1.0e-6 + 1.0e-5;
    lo = nc - ne - pad;
    hi = nc + ne + pad;
}
#ifdef CL_DECLARE_SKIN_OUT
// Row `row`'s frame (prev: the previous frame's, relative to this frame's eye). Identity when the
// table is not there (never once the pre-skin ran).
ClChar clSkinChar(uint row, bool prev)
{
    ClChar c;
    c.m = mat3(1.0);
    c.t = vec3(0.0);
    const uint b = clSkinCharBaseC(cl_rig[CLK_CAP]) + min(row, CL_SKIN_AV_ROWS - 1u) * CL_SKIN_CHAR_WORDS + (prev ? 12u : 0u);
    if (b + 12u > uint(cl_skin_out.length())) return c;
    c.m = mat3(uintBitsToFloat(cl_skin_out[b]),      uintBitsToFloat(cl_skin_out[b + 1u]), uintBitsToFloat(cl_skin_out[b + 2u]),
               uintBitsToFloat(cl_skin_out[b + 3u]), uintBitsToFloat(cl_skin_out[b + 4u]), uintBitsToFloat(cl_skin_out[b + 5u]),
               uintBitsToFloat(cl_skin_out[b + 6u]), uintBitsToFloat(cl_skin_out[b + 7u]), uintBitsToFloat(cl_skin_out[b + 8u]));
    c.t = uintBitsToFloat(uvec3(cl_skin_out[b + 9u], cl_skin_out[b + 10u], cl_skin_out[b + 11u]));
    return c;
}
uint clSkinBoxBase() { return clSkinBoxBaseC(cl_rig[CLK_CAP]); }
uint clSkinAvBoxBase() { return clSkinAvBoxBaseC(cl_rig[CLK_CAP]); }
// Order-preserving float <-> uint (for atomicMin / atomicMax on float bounds).
uint clOrderEnc(float f) { const uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u); }
float clOrderDec(uint e) { return uintBitsToFloat((e & 0x80000000u) != 0u ? (e & 0x7FFFFFFFu) : ~e); }
// False = the box (already in the view's (world - origin) space) lies entirely outside one SIDE
// plane of view-projection M (VK clip: -w <= x, y <= w), and with `depth` also outside the near /
// far planes (0 <= z <= w). Shadow casting passes take no depth test (casters behind a light's
// near plane still cast: the cascades clamp depth); the CAPTURE list's passes (mirror, probe
// faces: no depth clamp) do -- a box past a probe face's far plane or behind the mirror's
// oblique near plane rasterizes nothing, so culling it changes no pixel (fixed-cost audit
// 2026-10-14: probe faces drew every on-screen avatar's whole cut, whatever its distance).
bool clBoxInClip(vec3 lo, vec3 hi, mat4 M, bool depth)
{
    const vec4 r0 = vec4(M[0][0], M[1][0], M[2][0], M[3][0]);
    const vec4 r1 = vec4(M[0][1], M[1][1], M[2][1], M[3][1]);
    const vec4 r2 = vec4(M[0][2], M[1][2], M[2][2], M[3][2]);
    const vec4 r3 = vec4(M[0][3], M[1][3], M[2][3], M[3][3]);
    const vec4 pl[6] = vec4[6](r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2);
    for (uint k = 0u; k < (depth ? 6u : 4u); ++k)
    {
        const vec3 pv = mix(lo, hi, greaterThanEqual(pl[k].xyz, vec3(0.0)));   // the corner furthest inside
        if (dot(pl[k].xyz, pv) + pl[k].w < 0.0) return false;
    }
    return true;
}
// RIG-LIST CULL: false = selected entry `sel` (skinned this frame) lies outside view-projection M,
// whose input is (world - origin); off = the palettes' eye - origin; depth: also the near / far
// planes (the capture list only, clBoxInClip). Its avatar's box first, then
// its cluster's box. Not skinned / no box / not finite: true (drawn as before).
bool clRigEntryInView(uint sel, mat4 M, vec3 off, bool depth)
{
    const uint s = clSkinnedCluster(sel);
    if (s == CL_NONE) return true;
    const uint b = clSkinBoxBase() + s * CL_SKIN_BOX_WORDS;
    if (b + CL_SKIN_BOX_WORDS > uint(cl_skin_out.length()) || cl_skin_out[b + 7u] != 1u) return true;
    const uint av = cl_skin_out[b + 6u];
    if (av < CL_SKIN_AV_ROWS)
    {
        const uint ab = clSkinAvBoxBase() + av * CL_SKIN_BOX_WORDS;
        if (ab + CL_SKIN_BOX_WORDS <= uint(cl_skin_out.length()))
        {
            const uvec3 el = uvec3(cl_skin_out[ab], cl_skin_out[ab + 1u], cl_skin_out[ab + 2u]);
            const uvec3 eh = uvec3(cl_skin_out[ab + 3u], cl_skin_out[ab + 4u], cl_skin_out[ab + 5u]);
            if (all(notEqual(el, uvec3(0u))) && all(notEqual(eh, uvec3(0u))))
            {
                vec3 alo = vec3(clOrderDec(~el.x), clOrderDec(~el.y), clOrderDec(~el.z));
                vec3 ahi = vec3(clOrderDec(eh.x), clOrderDec(eh.y), clOrderDec(eh.z));
                if (!any(isnan(alo)) && !any(isnan(ahi)) && all(lessThanEqual(alo, ahi)))
                {
                    clCharBox(clSkinChar(av, false), alo, ahi);   // (character space -> eye-relative, step 2)
                    if (!clBoxInClip(alo + off, ahi + off, M, depth)) return false;
                }
            }
        }
    }
    vec3 lo = uintBitsToFloat(uvec3(cl_skin_out[b], cl_skin_out[b + 1u], cl_skin_out[b + 2u]));
    vec3 hi = uintBitsToFloat(uvec3(cl_skin_out[b + 3u], cl_skin_out[b + 4u], cl_skin_out[b + 5u]));
    if (any(isnan(lo)) || any(isnan(hi)) || any(greaterThan(lo, hi))) return true;
    clCharBox(clSkinChar(av, false), lo, hi);
    return clBoxInClip(lo + off, hi + off, M, depth);
}
#endif
#endif

#endif
