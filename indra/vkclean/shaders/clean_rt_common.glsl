// vkclean — ray-query helpers over the clean renderer's TLAS (LLCleanRTVK).
//
// The TLAS holds ONE instance per clean Instance id (instanceCustomIndex = id):
// static / attachment instances reference their LOD slot's shared BLAS (the SAME
// LOD the raster cull picks). Rigged faces are GROUPED per owner avatar + mask class
// into one multi-geometry BLAS built from the pre-skinned positions in the owner's
// PELVIS-LOCAL space (#632: the record's transform = the avatar's current pelvis, so a
// held animation frame needs no refit; world space + identity for an avatar-less
// group); the group has one record (custom index RT_RIG_GROUP | first rig entry) and
// rtHitInstance maps a hit back to the face's id. Rigged positions / normals read from
// rt_rigpos are in the BLAS's space: apply the hit's object-to-world.
//
// Consumers declare the bindless table `uTextures` (set 1, binding 0 -- the draw
// set layout) BEFORE including this file, and bind LLCleanRTVK's RT set at RT_SET
// (default 2). Set `rt_anim_time` (seconds, the draw push's cam_time.w) before
// tracing when GPU texture animation must match the raster.
#ifndef CLEAN_RT_COMMON_GLSL
#define CLEAN_RT_COMMON_GLSL

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_control_flow_attributes : require   // (rtCandidateAlphaCol's corner loop: [[dont_unroll]])

#ifndef RT_SET
#define RT_SET 2
#endif

#include "clean_uv_common.glsl"
#include "clean_translucent.glsl"
#include "clean_qvertex.glsl"   // the static VBO's quantized vertex + 16-bit indices (#573)

// TLAS instance mask CLASSES. Exactly one bit per instance (priority water >
// terrain > blend > rigged / own avatar > attachment > mask > opaque) so a ray mask can
// EXCLUDE a class (hit test is (instance_mask & ray_mask) != 0).
// SIZE CLASS (#859 perf, RenderVKRTReflectionsSizeClasses): the agent's own avatar (rigged +
// worn attachments) used to have a bit of its own (0x80), but every ray treated it exactly as
// the rigged class (the probe RT's static rays drop both, the self / lift probes take both,
// everything else takes both), so it SHARES the rigged bit now (RT_MASK_SELF = RT_MASK_RIGGED:
// every ray mask keeps its meaning) and 0x80 is the SMALL STATIC OPAQUE class: static opaque
// instances whose world bounds are below RenderVKRTReflectionsSmallSize
// (clean_rt_instances.comp maskClassSized). RT_MASK_OPAQUE names BOTH bits, so every ray that
// took static opaque geometry still takes all of it; only the reflection rays' far segment
// (clean_deferred_light.comp rtrTraceRefl) leaves RT_MASK_SMALL out.
#define RT_MASK_SMALL     0x80u   // static, opaque, small (the instance's class bit)
#define RT_MASK_OPAQUE    (0x01u | RT_MASK_SMALL)   // static, opaque (any size)
#define RT_MASK_ALPHAMASK 0x02u   // static, alpha-mask material
#define RT_MASK_BLEND     0x04u   // alpha-blend (any kind)
#define RT_MASK_RIGGED    0x08u   // rigged mesh (avatars / animesh), and the agent's own avatar
#define RT_MASK_TERRAIN   0x10u
#define RT_MASK_WATER     0x20u
#define RT_MASK_ATTACH    0x40u   // worn rigid attachment (other avatars)
#define RT_MASK_SELF      RT_MASK_RIGGED   // the agent's own avatar (rigged + attachments): the rigged bit
#define RT_MASK_ALL       0xFFu
// Sun-shadow caster rules: everything but the water surface (HUD is never in the TLAS).
#define RT_MASK_SHADOW    (RT_MASK_ALL & ~RT_MASK_WATER)
// What writes raster depth: no water, no alpha-blend (both depth-write off).
#define RT_MASK_PRIMARY   (RT_MASK_ALL & ~(RT_MASK_WATER | RT_MASK_BLEND))

// PER-RAY CANDIDATE BUDGET (G-7): every rayQueryProceedEXT loop counts the candidates
// it handles and ends the ray past the budget -- a stack of thousands of faint-alpha
// faces along one ray would otherwise run the alpha test for each and can hang the GPU
// (TDR). Shadow / visibility rays treat the ray as blocked; closest-hit rays keep the
// nearest hit committed so far. Real content stays far below it: opaque instances
// never produce candidates, so 64 means 64 alpha-tested surfaces on one ray. Rays
// traced with gl_RayFlagsNoOpaqueEXT see EVERY triangle as a candidate and get the
// larger budget.
#define RT_CANDIDATE_BUDGET          64u
#define RT_CANDIDATE_BUDGET_NOOPAQUE 256u

// RAY INPUT GUARD (RT reflections GPU hang, Xid 109 CTX SWITCH TIMEOUT on the async trace):
// OpRayQueryInitializeKHR requires a finite origin / direction (no NaN / Inf), a non-zero
// direction, 0 <= tmin <= tmax with no NaN -- anything else is UNDEFINED, and on NVIDIA a
// traversal fed such a ray can fail to terminate (the candidate budget above never runs: the
// hardware walk itself does not return). The candidate loops are all bounded; the inputs were
// not. Sources seen in the reflection path: a hit triangle with exploded (huge, finite) skinned
// vertices -- clean_preskin.comp keeps large finite positions on purpose -- whose cross product
// overflows to Inf and normalises to NaN (rtSurfaceEval's Ng, then every secondary ray's lift /
// origin), and rtrClosest's back-face re-trace stepping tmin past tmax. Every rayQueryInitializeEXT
// site passes its ray through this first. A finite ray whose tmax lies below tmin is clamped to
// the legal empty interval tmax = tmin (a miss, as the hardware treated it: every finite ray keeps
// its old result) and returns true. A NON-FINITE ray (NaN / Inf origin or direction, zero
// direction, NaN / negative / Inf tmin, NaN tmax) becomes an EMPTY one far below the world and
// returns false: a miss for closest-hit queries; shadow walks treat it as blocked (the G-7
// over-budget rule) themselves. Valid rays are untouched, so normal images are bit-identical.
// (#1049, the hang recurred) Also refused as non-finite: an origin / tmin past RT_RAY_WORLD_MAX and a
// direction of absurd length (finite, but no world ray -- origins and spans that large are garbage
// whose traversal math overflows inside the walk); tmax is clamped to a finite 2 x RT_RAY_WORLD_MAX.
// PER-INVOCATION RAY-QUERY BUDGET (#1049): rt_query_left = the ray queries this invocation may still
// start (unlimited by default; the RT reflection kernels set it per reflection ray, clean_deferred_
// light.comp RTR_QUERY_BUDGET): past it a query is refused the same way (empty ray, false), so one
// ray's total traversal work is bounded whatever its walks / re-traces / hit shading add up to. The
// counters (rt_ray_rejected, rt_query_refused) are read by the kernels that set the budget.
#define RT_RAY_WORLD_MAX 1.0e7
// (#1128) THE VERDICT ON THE BIT PATTERN. The float form below (isnan / isinf, ordered compares) is what
// #1049 shipped; the viewer never declares SignedZeroInfNanPreserve, so a Vulkan driver may assume no NaN /
// Inf in it -- fold isnan / isinf to false, and turn a NEGATED ordered compare (the !ok branch) into the
// opposite one, which is false for NaN where the original was true. The max() of the origin bound returns
// the non-NaN operand on NVIDIA. Under those assumptions a NaN origin passes, and whether they apply
// depends on the surrounding code generation (a value-identical edit of rtHitLocalLights, 690930d47c,
// hung the RT reflection LIGHT kernel on 615.78 and its revert stopped it). The verdict now comes from
// integer tests on the bits, which no float assumption can change: |x| bits below those of the bound
// (finite non-negative floats order as their bits, and Inf / NaN lie above every finite one), the
// exponent of tmax not all ones unless it is +-Inf (NaN refused), -0 tmin accepted as it was. For every
// input on which the float form is evaluated as written, both verdicts agree, so a valid ray is exactly
// as before (images unchanged).
// rt_ray_folded counts the rays the float form ACCEPTED and the bits refused: non-zero proves the compiled
// float guard let a non-finite / out-of-world ray through (the folding above), i.e. that such rays reached
// rayQueryInitializeEXT before this change.
#define RT_RAY_WORLD_MAX_BITS 0x4B189680u   // floatBitsToUint(1.0e7)
#define RT_DD_MIN_BITS        0x179ABE15u   // floatBitsToUint(1.0e-24)
#define RT_DD_MAX_BITS        0x6753C21Cu   // floatBitsToUint(1.0e24)
uint rt_query_left = 0xFFFFFFFFu;
uint rt_query_refused = 0u;
uint rt_ray_rejected = 0u;
uint rt_ray_folded = 0u;
#ifdef CLEAN_RT_HANG_BEACON
// (#1128) THE HANG BEACON (RenderVKRTHangBeacon; the beacon variants of the RT reflection list trace and
// the RT forward pass only): every ray query's inputs, as the caller passed them, into the lane's beacon
// slot in host-visible memory, before the query starts -- defined by the kernel (clean_deferred_light.comp).
// rt_bc_site: the walk about to start (RT_BC_SITE_*: set by the walk functions, cleared by each record).
void rtBeaconQuery(vec3 o, float tmin, vec3 d, float tmax, bool ok, bool folded);
uint rt_bc_site = 0u;
#define RT_BC_SITE(s) rt_bc_site = (s)
#else
#define RT_BC_SITE(s)
#endif
#define RT_BC_SITE_CLOSEST 0x1000u   // | the ray mask: rtClosestWalk
#define RT_BC_SITE_VIS     0x2000u   // | walk kind << 8 | mask: rtVisWalkFP
#define RT_BC_SITE_LAYERS  0x3000u   // | mask: rtrBlendLayersT
bool rtRaySane(inout vec3 o, inout float tmin, inout vec3 d, inout float tmax)
{
    const vec3 ao = abs(o);
    const float dd = dot(d, d);
    const bool okFloat = !any(isnan(o)) && !any(isinf(o)) && !any(isnan(d)) && !any(isinf(d))
                      && max(max(ao.x, ao.y), ao.z) < RT_RAY_WORLD_MAX
                      && dd > 1e-24 && dd < 1e24 && tmin >= 0.0 && tmin < RT_RAY_WORLD_MAX && !isnan(tmax);
    const uvec3 ob = floatBitsToUint(o) & 0x7FFFFFFFu;
    const uvec3 db = floatBitsToUint(d) & 0x7FFFFFFFu;
    const uint ddb = floatBitsToUint(dd);       // (d finite: dd >= +0, finite or +Inf)
    const uint tnb = floatBitsToUint(tmin);
    const bool ok = all(lessThan(ob, uvec3(RT_RAY_WORLD_MAX_BITS))) && all(lessThan(db, uvec3(0x7F800000u)))
                 && ddb > RT_DD_MIN_BITS && ddb < RT_DD_MAX_BITS
                 && (tnb < RT_RAY_WORLD_MAX_BITS || (tnb >= 0x80000000u && tnb < 0x80800000u))   // (-0 / a negative
                    // denormal: >= 0 as the float form saw it -- denormals flush to zero)
                 && (floatBitsToUint(tmax) & 0x7FFFFFFFu) <= 0x7F800000u;
#ifdef CLEAN_RT_HANG_BEACON
    rtBeaconQuery(o, tmin, d, tmax, ok, okFloat && !ok);
#endif
    if (okFloat && !ok) ++rt_ray_folded;
    if (!ok) ++rt_ray_rejected;
    else if (rt_query_left == 0u) ++rt_query_refused;
    if (!ok || rt_query_left == 0u) { o = vec3(0.0, 0.0, -1.0e6); d = vec3(0.0, 0.0, -1.0); tmin = 0.0; tmax = 0.0; return false; }
    if (rt_query_left != 0xFFFFFFFFu) --rt_query_left;
    tmin = max(tmin, 0.0);   // (a negative denormal -> 0; every other accepted tmin as it is)
    tmax = min(tmax, 2.0 * RT_RAY_WORLD_MAX);   // (not NaN here: exact)
    tmax = max(tmax, tmin);   // (both finite, not NaN: the legal empty query where tmax < tmin, as before)
    return true;
}
// (#1039 occupancy) rtRaySane for a caller that does not trace a refused ray (the shadow walk: blocked at once):
// the same verdict, counters and interval, with the ray itself untouched -- a sane ray leaves rtRaySane
// unchanged, so the walk keeps using its caller's origin / direction instead of a sanitised copy of them (six
// floats live across the traversal next to the originals).
bool rtRaySaneT(vec3 o, inout float tmin, vec3 d, inout float tmax)
{
    return rtRaySane(o, tmin, d, tmax);
}

// THE ALPHA-TEST RULE (#1044, clean_alpha_rule.glsl): the cutoffs / thresholds every candidate test uses.
#include "clean_alpha_rule.glsl"
// Alpha-blend materials have no cutoff (alpha_cutoff < 0): their any-hit treats
// base-colour alpha below this as a hole (LL's shadow pass does the same).
#ifndef RT_BLEND_CUTOFF
#define RT_BLEND_CUTOFF ALPHA_BLEND_CUTOFF
#endif

// 224 B -- LLCleanInstanceStore::Instance.
struct RtInstance {
    mat4 model_matrix;
    vec4 aabb_min;
    vec4 aabb_max;
    vec4 tint;
    mat4 uv_matrix;
    uint lod_group_id;
    uint material_id;
    uint flags;
    uint skin_id;
    vec4 spin_axis;
    vec4 spin_pivot;
};
// 96 B -- LLCleanMaterialStore::Material (base_color_handle.x = bindless index).
struct RtMaterial {
    uvec2 base_color_handle;
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;
};

layout(set = RT_SET, binding = 0) uniform accelerationStructureEXT uTLAS;
layout(std430, set = RT_SET, binding = 1) readonly buffer RtInstances { RtInstance rt_instances[]; };
layout(std430, set = RT_SET, binding = 2) readonly buffer RtMaterials { RtMaterial rt_materials[]; };
// Per TLAS instance (= instance id), written by clean_rt_instances.comp this frame:
//   x = first_index, y = static base_vertex (UV / normal / object-space position),
//   z = rigged: 0x80000000 | base of its skinned positions in rt_rigpos; else 0
//       (cluster hits: RT_GEOM_CLUSTER* below),
//   w = index_count (bits 0..30) | RT_GEOM_W_CLUSTER_RIG.
layout(std430, set = RT_SET, binding = 3) readonly buffer RtGeom { uvec4 rt_geom[]; };
// rt_geom.w bit 31: a rigged face the CLUSTER LOD path draws in the main view (#747; the
// main cull's rig-cache RC_CLUSTER) whose geometry in the TLAS is still the LEGACY one this
// frame, not the surface on screen (clean_rt_shadow_mask.comp receiver lift). Never set on a
// face traced on its cluster RT cut (RT_GEOM_CLUSTER_RIG below: pass B writes w = 0).
#define RT_GEOM_W_CLUSTER_RIG 0x80000000u
bool rtIsClusterRig(uint id) { return id < uint(rt_geom.length()) && (rt_geom[id].w & RT_GEOM_W_CLUSTER_RIG) != 0u; }
layout(std430, set = RT_SET, binding = 4) readonly buffer RtVbo  { uint  rt_vbo[]; };    // QVertex: 5 words (+ slot headers)
layout(std430, set = RT_SET, binding = 5) readonly buffer RtIbo  { uint  rt_ibo[]; };    // 16-bit indices, 2 per word
// Rigged skinned vertices (clean_rt_instances.comp rigCopy): RT_RIGPOS_WORDS words each
// = world xyz (fp32 bits; the rig BLAS's build input, stride 16) + the skinned, welded
// vertex normal (octahedral word, clean_skinvert.glsl / clean_qvertex.glsl qvNormal).
#define RT_RIGPOS_WORDS 4u
layout(std430, set = RT_SET, binding = 6) readonly buffer RtRig  { uint rt_rigpos[]; };
// Rig entries (LLCleanRTVK::RigEntryCPU, 8 words; word 0 = instance id). Rigged faces
// share one BLAS per owner + mask class; the group's TLAS record has custom index
// RT_RIG_GROUP | its first entry and one geometry per member entry, in order.
layout(std430, set = RT_SET, binding = 9) readonly buffer RtRigMap { uint rt_rigmap[]; };
#define RT_RIG_GROUP 0x800000u
// CLUSTER LOD HITS (#748, vkgfx/llcleanclusterrtvk.h). An instance whose TLAS record references a
// cluster BLAS (the RT cut of the cluster path) has RT_GEOM_CLUSTER in rt_geom.z; its geometry
// lives in the cluster page pool (binding 11, the same QVertex layout as rt_vbo, uint8 local
// indices) instead of the static VBO / IBO:
//   geometry index = the cluster ref (RT_GEOM_CLUSTER_DIRECT: CLAS) or the entry of the
//                    instance's hit-cut range at rt_geom.x (binding 12, {ref, vpage, 0, 0}),
//   primitive index = the triangle within that cluster.
// RT_HIT_PRIM folds both into the `prim` every consumer passes around (geometry << 7 | triangle,
// cluster hits only), rtTriIndices returns vertex numbers relative to rt_geom.y with
// RT_POOL_VERT set (rt_geom.y = RT_POOL_VERT | first vertex of one cluster of the cut: its
// decode header is the mesh's, shared by every cluster), and the vertex reads below take a
// RT_POOL_VERT vertex from the pool. Legacy hits are untouched (vertex numbers < 2^31).
#define RT_GEOM_CLUSTER        0x40000000u
#define RT_GEOM_CLUSTER_DIRECT 0x20000000u
#define RT_POOL_VERT           0x80000000u
layout(std430, set = RT_SET, binding = 11) readonly buffer RtClPool { uint rt_cl_pool[]; };
layout(std430, set = RT_SET, binding = 12) readonly buffer RtClCut  { uvec4 rt_cl_cut[]; };
// ---- #874 / #889 READ GUARD ----
// No pipeline robustness on the world pipelines: an out-of-range index into any SSBO below
// is a raw global load at base + index x stride, which AMD turns into a page fault
// (DEVICE_LOST) and NVIDIA usually survives. Every index a hit or a table supplies is
// checked against the bound buffer's length() and, where the RT tables carry it, the
// geometry's own bound (triangle count rt_geom.w, vertex count rt_geom_vc). A failing read
// is skipped: ids -> RT_BAD_ID (callers treat it as an opaque blocker / no triangle),
// triangles -> degenerate (zero positions / indices), textures -> opaque.
// RECORDING (RT_GUARD_RECORD, clean_rt_shadow_mask.comp only): binding 15 = the frame slot's
// host-visible guard buffer (LLCleanRTVK::guard*, llcleanrtaudit.cpp). Words:
//   [0] host: bit 0 guard on (RenderVKRTGuard, informational: the pipeline's kRtGuardOn decides)
//   [1] host: RT frame   [2] host: written bindless texture slots   [3] host: 0
//   [4] GPU claim (first offender)   [5] GPU total violations
//   [6..] first offender: site, custom index, geometry index, prim, resolved id, value,
//         bound, flags (1 rigged, 2 cluster, 4 group hit, 8 id valid), rt_geom row x y z w,
//         vertex count
//   [RTG_COUNTS + site] per-site counts (invocations whose FIRST violation was at that site;
//   [5] counts every violation). Recorded once per invocation by rtGuardFlush.
// Every other includer checks without recording (no binding 15 / 16 needed there... except
// rt_geom_vc, which is part of the RT set and always bound).
#define RTG_ID        1u    // resolved instance id outside rt_instances / rt_geom
#define RTG_RIGMAP    2u    // rig group entry (custom + geometry index) outside rt_rigmap
#define RTG_PRIM      3u    // primitive >= the geometry's triangle count (rt_geom.w / 3)
#define RTG_IBO       4u    // index word outside rt_ibo
#define RTG_VTX       5u    // triangle index >= the geometry's vertex count (rt_geom_vc)
#define RTG_VBO       6u    // static VBO word (vertex / decode header) outside rt_vbo
#define RTG_RIGPOS    7u    // skinned position / normal word outside rt_rigpos
#define RTG_MAT       8u    // material id outside rt_materials
#define RTG_TEX       9u    // bindless texture index >= the written table
#define RTG_CL_CUT   10u    // cluster hit-cut entry outside rt_cl_cut
#define RTG_CL_POOL  11u    // cluster page pool word outside rt_cl_pool
#define RTG_CL_RIG   12u    // rigged cluster arena word outside rt_cl_rig / rt_cl_skin
#define RTG_PSHADOW  13u    // projector shadow record outside pshadow (mask)
#define RTG_GEOMROW  14u    // a hit on an instance whose rt_geom row is empty (not this frame's geometry)
#define RTG_LIGHT    15u    // light list / projector texture out of range (mask)
#define RTG_SITES    16u
#define RTG_REC      6u
#define RTG_COUNTS   20u
#define RT_BAD_ID    0xFFFFFFFFu
// Per geometry (= instance id, as rt_geom), RT_GEOM_VC_WORDS words: [0] its vertex count (0 = unknown:
// cluster rows); (#1009) [1..3] a cluster-traced static's legacy row (rtGeomEmRow; clean_rt_instances.comp).
#define RT_GEOM_VC_WORDS 4u
layout(std430, set = RT_SET, binding = 16) readonly buffer RtGeomVc { uint rt_geom_vc[]; };
#ifdef RT_GUARD_RECORD
layout(std430, set = RT_SET, binding = 15) coherent buffer RtGuard { uint rt_guard[]; };
// RenderVKRTGuard (A/B): a specialization constant of the mask pipeline (set at creation; the
// driver folds the checks away when off) -- a run-time test here tripled the mask's code.
layout(constant_id = 40) const bool kRtGuardOn = true;
bool rtGuardOn() { return kRtGuardOn; }
uint rtGuardTexCount() { return uint(rt_guard.length()) > 2u ? rt_guard[2] : 0xFFFFFFFFu; }
#else
bool rtGuardOn() { return true; }
uint rtGuardTexCount() { return 0xFFFFFFFFu; }   // (unknown here: not checked)
#endif
// Hit context of the read being checked (set by rtHitInstance / the triangle readers).
uint rt_g_custom = 0u, rt_g_geom = 0u, rt_g_id = RT_BAD_ID, rt_g_prim = 0u;
// The invocation's FIRST violation is kept in registers (cheap at every one of the many inlined
// call sites: no atomics there) and written once by rtGuardFlush at the end of the mask's main:
// total = violations of this invocation, per-site count = invocations whose first one it was.
uint rt_g_n = 0u, rt_g_site = 0u, rt_g_val = 0u, rt_g_bound = 0u;
uint rt_g_fc = 0u, rt_g_fg = 0u, rt_g_fp = 0u, rt_g_fid = RT_BAD_ID;
void rtGuardNote(uint site, uint value, uint bound)
{
#ifdef RT_GUARD_RECORD
    ++rt_g_n;
    if (rt_g_site == 0u)
    {
        rt_g_site = site; rt_g_val = value; rt_g_bound = bound;
        rt_g_fc = rt_g_custom; rt_g_fg = rt_g_geom; rt_g_fp = rt_g_prim; rt_g_fid = rt_g_id;
    }
#endif
}
#ifdef RT_GUARD_RECORD
void rtGuardFlush()
{
    if (rt_g_n == 0u) return;
    atomicAdd(rt_guard[5], rt_g_n);
    atomicAdd(rt_guard[RTG_COUNTS + min(rt_g_site, RTG_SITES - 1u)], 1u);
    if (atomicCompSwap(rt_guard[4], 0u, 1u) == 0u)
    {
        const uint id = rt_g_fid;
        const bool idok = id < uint(rt_geom.length());
        const uvec4 g = idok ? rt_geom[id] : uvec4(0xFFFFFFFFu);
        rt_guard[RTG_REC + 0u] = rt_g_site;
        rt_guard[RTG_REC + 1u] = rt_g_fc;
        rt_guard[RTG_REC + 2u] = rt_g_fg;
        rt_guard[RTG_REC + 3u] = rt_g_fp;
        rt_guard[RTG_REC + 4u] = id;
        rt_guard[RTG_REC + 5u] = rt_g_val;
        rt_guard[RTG_REC + 6u] = rt_g_bound;
        rt_guard[RTG_REC + 7u] = (idok && (g.z & 0x80000000u) != 0u ? 1u : 0u)
                               | (idok && (g.z & 0x40000000u) != 0u ? 2u : 0u)
                               | ((rt_g_fc & 0x800000u) != 0u ? 4u : 0u) | (idok ? 8u : 0u);
        rt_guard[RTG_REC + 8u] = g.x;
        rt_guard[RTG_REC + 9u] = g.y;
        rt_guard[RTG_REC + 10u] = g.z;
        rt_guard[RTG_REC + 11u] = g.w;
        rt_guard[RTG_REC + 12u] = (idok && id < uint(rt_geom_vc.length()) / RT_GEOM_VC_WORDS)
                                ? rt_geom_vc[id * RT_GEOM_VC_WORDS] : 0xFFFFFFFFu;
    }
}
#endif
// RTG_OK(cond, site, value, bound): cond, or note the violation; false = skip the read.
// With the guard off (RenderVKRTGuard 0, mask only) every read goes ahead unchecked.
bool rtGuardFail(uint site, uint value, uint bound)
{
    if (!rtGuardOn()) return true;
    rtGuardNote(site, value, bound);
    return false;
}
#define RTG_OK(COND, SITE, VALUE, BOUND) ((COND) || rtGuardFail((SITE), (VALUE), (BOUND)))
// Silent id test for the per-instance reads downstream of rtHitInstance (which noted it).
bool rtIdIn(uint id) { return !rtGuardOn() || (id < uint(rt_instances.length()) && id < uint(rt_geom.length())); }
void rtGuardCtx(uint id, uint prim) { rt_g_id = id; rt_g_prim = prim; }

bool rtIsCluster(uint id) { return rtIdIn(id) && (rt_geom[id].z & RT_GEOM_CLUSTER) != 0u; }
// RIGGED CLUSTER HITS (#747 / #748 integration, llcleanclusterrtvk.h RIGGED): a face of an avatar
// the cluster raster draws, traced on its RT cut skinned this frame. rt_geom.z = 0x80000000
// (rigged: every rigged rule applies) | RT_GEOM_CLUSTER | RT_GEOM_CLUSTER_RIG, rt_geom.x = its
// first slot of the RT SKINNED ARENA (binding 13), rt_geom.y = RT_POOL_VERT | a pool vertex of
// its mesh (the decode header: UVs read from the pool as any cluster hit). Geometry index g =
// the entry of its cut: slot s = x + g, header {ref, vpage, ...} at word 16 + s*4; vertex l of
// the slot at word rt_cl_rig[0] + (s*64 + l)*4: skinned position relative to the RT ORIGIN (the
// BLAS's space: apply the hit's object-to-world, a translation by that origin) + the skinned
// normal (a slot taking the SHARED skinned cut holds a copy of 2a's SKIN_OUT vertices, #748 perf
// pass 2; binding 14 is no longer read).
#define RT_GEOM_CLUSTER_RIG    0x10000000u
layout(std430, set = RT_SET, binding = 13) readonly buffer RtClRig  { uint rt_cl_rig[]; };
layout(std430, set = RT_SET, binding = 14) readonly buffer RtClSkin { uint rt_cl_skin[]; };
// Checked single-word loads (a failed one reads 0).
uint rtLdPool(uint i)  { return RTG_OK(i < uint(rt_cl_pool.length()), RTG_CL_POOL, i, uint(rt_cl_pool.length())) ? rt_cl_pool[i] : 0u; }
uint rtLdClRig(uint i) { return RTG_OK(i < uint(rt_cl_rig.length()), RTG_CL_RIG, i, uint(rt_cl_rig.length())) ? rt_cl_rig[i] : 0u; }
uint rtLdClSkin(uint i){ return RTG_OK(i < uint(rt_cl_skin.length()), RTG_CL_RIG, i | 0x80000000u, uint(rt_cl_skin.length())) ? rt_cl_skin[i] : 0u; }
uint rtLdClCut(uint i) { return RTG_OK(i < uint(rt_cl_cut.length()), RTG_CL_CUT, i, uint(rt_cl_cut.length())) ? rt_cl_cut[i].x : 0u; }
uint rtGeomVc(uint id) { return id < uint(rt_geom_vc.length()) / RT_GEOM_VC_WORDS ? rt_geom_vc[id * RT_GEOM_VC_WORDS] : 0u; }
// (#1009) Instance id's LEGACY geometry row as rt_geom lays one out (first index, base vertex, 0, index
// count; w = 0: none): a legacy static's own row, or -- for a static traced on its CLUSTER cut (its rt_geom
// row names the cut) -- the legacy slot clean_rt_instances.comp recorded beside it (the static LOD rule's
// level: what the cluster-off TLAS would trace). Rigged rows (legacy or cluster) have none. Consumers that
// enumerate an instance's triangles (the emissive light build) read the vertices through this row.
uvec4 rtGeomEmRow(uint id, bool clusterStatic)
{
    if (!rtIdIn(id)) return uvec4(0u);
    const uvec4 g = rt_geom[id];
    if ((g.z & 0x80000000u) != 0u) return uvec4(0u);                              // rigged
    if ((g.z & RT_GEOM_CLUSTER) == 0u) return uvec4(g.x, g.y, 0u, g.w & 0x7FFFFFFFu);
    const uint b = id * RT_GEOM_VC_WORDS;
    if (!clusterStatic || id >= uint(rt_geom_vc.length()) / RT_GEOM_VC_WORDS) return uvec4(0u);
    return uvec4(rt_geom_vc[b + 1u], rt_geom_vc[b + 2u], 0u, rt_geom_vc[b + 3u] & 0x7FFFFFFFu);
}
// Word index of vertex l of rigged slot s (bit 31: in rt_cl_skin, else rt_cl_rig): xyz at +0..2,
// the octahedral normal at +3. (#748 perf pass 2: every slot's vertices are in the RT skinned arena
// -- a shared slot's are COPIED there from SKIN_OUT, relative to the RT origin like the CLAS built
// from them; SKIN_OUT itself is eye-relative and per frame, while a kept avatar's CLAS are not.)
uint rtClRigVtxWord(uint s, uint l)
{
    return rtLdClRig(0u) + (s * 64u + l) * 4u;
}
uint rtClRigW(uint w, uint k) { return (w & 0x80000000u) != 0u ? rtLdClSkin((w & 0x7FFFFFFFu) + k) : rtLdClRig(w + k); }
vec3 rtClRigPos(uint w) { return uintBitsToFloat(uvec3(rtClRigW(w, 0u), rtClRigW(w, 1u), rtClRigW(w, 2u))); }
bool rtIsClusterRigged(uint id) { return rtIdIn(id) && (rt_geom[id].z & RT_GEOM_CLUSTER_RIG) != 0u; }
// A rigged cluster hit's arena slot and the triangle's local vertex indices (0..63).
void rtClRigTri(uint id, uint prim, out uint s, out uvec3 l)
{
    rtGuardCtx(id, prim);
    s = 0u; l = uvec3(0u);
    if (!rtIdIn(id)) return;
    s = rt_geom[id].x + (prim >> 7u);
    const uint ref = rtLdClRig(16u + s * 4u);
    const uint slot = ref >> 6u, ci = ref & 63u;
    const uint rw = slot * 20480u + (32u + ci * 64u) / 4u;               // ClusterRecord words
    const uint ib = slot * 81920u + rtLdPool(rw + 10u) * 4u + (prim & 127u) * 3u;   // index_off4 -> bytes
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint b = ib + k;
        l[k] = (rtLdPool(b >> 2u) >> ((b & 3u) * 8u)) & 0xFFu;
    }
}
uint rtHitPrim(uint id, uint geom, uint prim)
{
    return rtIsCluster(id) ? ((geom << 7u) | (prim & 127u)) : prim;
}
#define RT_HIT_PRIM(RQ, C, ID) rtHitPrim((ID), uint(rayQueryGetIntersectionGeometryIndexEXT(RQ, C)), \
                                         uint(rayQueryGetIntersectionPrimitiveIndexEXT(RQ, C)))
// Word k of vertex v (static VBO, or the pool for RT_POOL_VERT).
uint rtVW(uint v, uint k)
{
    if ((v & RT_POOL_VERT) != 0u)
    {
        const uint pv = v & ~RT_POOL_VERT;
        return RTG_OK(pv < uint(rt_cl_pool.length()) / QV_WORDS, RTG_CL_POOL, pv, uint(rt_cl_pool.length()) / QV_WORDS)
             ? rt_cl_pool[pv * QV_WORDS + k] : 0u;
    }
    return RTG_OK(v < uint(rt_vbo.length()) / QV_WORDS, RTG_VBO, v, uint(rt_vbo.length()) / QV_WORDS)
         ? rt_vbo[v * QV_WORDS + k] : 0u;
}
// The clean Instance id a hit belongs to (instanceCustomIndex, resolved through the rig
// group map for grouped rigged hits). The primitive index is per geometry, so it is
// already the triangle index within that instance's slot.
// GUARDED (#874): the rig map entry and the resolved id are checked here, once per hit;
// RT_BAD_ID when either is out of range (every consumer then treats the hit as an opaque
// blocker / no triangle through rtIdIn).
uint rtHitInstance(uint custom, uint geom)
{
    rt_g_custom = custom; rt_g_geom = geom; rt_g_id = RT_BAD_ID; rt_g_prim = 0u;
    uint id = custom;
    if ((custom & RT_RIG_GROUP) != 0u)
    {
        const uint e = (custom & 0x7FFFFFu) + geom;
        const uint n = uint(rt_rigmap.length()) / 8u;
        if (!RTG_OK(e < n, RTG_RIGMAP, e, n)) return RT_BAD_ID;
        id = rt_rigmap[e * 8u];
    }
    rt_g_id = id;
    const uint n = min(uint(rt_instances.length()), uint(rt_geom.length()));
    if (!RTG_OK(id < n, RTG_ID, id, n)) return RT_BAD_ID;
    return id;
}

// ---- NO-SELF-SHADOW CHARACTERS (rigged RT update throttle, llcleanrtvk.cpp classifyNoSelf) ----
// The refit gate (RenderVKRigRefitGate 2) updates the RT geometry of far / small on-screen
// avatars and animesh only every 2nd..4th frame, so on its off frames their rigged BLAS /
// CLAS hold a pose up to 3 frames old while the raster draws the current one. A shadow ray
// leaving such a character's CURRENT surface then met its own STALE copy -- self-shadow
// patches jumping every few frames (a swimming fish school). Rule: a THROTTLED character
// (classified with hysteresis on the CPU, never the own avatar / the protected set) never
// self-shadows in RT, on any frame -- so nothing blinks on its update frames -- while it
// still casts onto everything else and receives every other caster.
//   TLAS: its rigged group records (legacy group BLAS and cluster CLAS records, pass B of
//   clean_rt_instances.comp) carry the owner avatar slot + 1 in the instance SBT record
//   offset (unused by ray queries otherwise; 0 = every other record) and FORCE_NO_OPAQUE,
//   so every ray sees their triangles as candidates (opaque faces are confirmed at once:
//   rtSelfCandidate).
//   Receiver: rt_self_owner = that slot + 1 for a receiver ON such a character, 0 = none.
//   Callers that cannot tell set RT_SELF_UNKNOWN (+ rt_self_P / rt_self_cam): it is resolved
//   LAZILY, by rtSelfProbe, the first time one of the receiver's rays meets a throttled
//   character's candidate -- receivers whose rays never come near one pay nothing.
#define RT_SELF_UNKNOWN 0xFFFFFFFFu
uint rt_self_owner = 0u;
vec3 rt_self_P   = vec3(0.0);   // the receiver (absolute world, as the TLAS)
vec3 rt_self_cam = vec3(0.0);   // the camera that sees it
// (#1039 occupancy) WALK-STATE HOOKS: the walk reads a few per-ray values set by its caller -- the self
// probe's receiver (RT_SELF_P), the alpha test's cone width (RT_CONE_WIDTH0) and receiver footprint
// (RT_FOOT_AB), the own fixture's light centre and size cap (clean_vis_walk.glsl RT_FIX_P / RT_FIX_MAX).
// Kept in globals they are live across the whole traversal (a register each, every candidate). An includer
// whose walk state lives in memory -- the visibility queue's walk kernels: the REQUEST -- defines these to
// read it where the walk uses it, before including this file. Defaults: the globals (no change).
#ifndef RT_SELF_P
#define RT_SELF_P rt_self_P
#endif
#ifndef RT_SELF_CAM
#define RT_SELF_CAM rt_self_cam
#endif
// RECEIVER IDENTITY PROBE: a camera-ray segment through the receiver, +-RT_SELF_PROBE_W,
// over the rigged + blend + own-avatar classes, every triangle a candidate (no commit). The candidate
// nearest the receiver along the view ray names its character: the SBT owner of a
// throttled one, 0 for any other surface (another avatar the probe met first, a static
// blend prim). The window covers how far a stale pose can sit from the drawn surface (a
// receiver inside its own stale hull meets that hull leaving it toward the camera; one
// outside it, behind) -- on an update frame the drawn surface itself is at ~0. A surface
// within the window along the view line that is NOT the receiver can only lie at a
// silhouette (anything in front of the receiver would hide it), so a misidentified ground
// pixel loses that one character's shadow only right at its contact silhouette.
#define RT_SELF_PROBE_W 0.2
uint rtSelfProbe()
{
    const vec3 sP = RT_SELF_P;
    vec3 V = sP - RT_SELF_CAM;
    const float d = length(V);
    if (!(d > 1e-4)) return 0u;
    V /= d;
    vec3 po = sP - V * RT_SELF_PROBE_W;
    float pt0 = 0.0, pt1 = 2.0 * RT_SELF_PROBE_W;
    if (!rtRaySane(po, pt0, V, pt1)) return 0u;
    rayQueryEXT pq;
    rayQueryInitializeEXT(pq, uTLAS, gl_RayFlagsNoOpaqueEXT, RT_MASK_RIGGED | RT_MASK_BLEND | RT_MASK_SELF,
                          po, pt0, V, pt1);
    float best = 1e30;
    uint owner = 0u, n = 0u;
    while (rayQueryProceedEXT(pq))
    {
        if (rayQueryGetIntersectionTypeEXT(pq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        if (++n > RT_CANDIDATE_BUDGET) { rayQueryTerminateEXT(pq); break; }
        const float e = abs(rayQueryGetIntersectionTEXT(pq, false) - RT_SELF_PROBE_W);
        if (e < best)
        {
            best = e;
            owner = uint(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(pq, false));
        }
    }
    return owner;
}
// A candidate's throttled-character owner (slot + 1, 0 = none).
#define RT_CAND_OWNER(RQ) uint(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(RQ, false))
// Candidate of a throttled character (owner != 0): 1 = the receiver's own (skip it),
// 2 = another one's opaque face (confirm: the record is non-opaque only for this rule),
// 0 = the ordinary candidate rules apply (its alpha-tested / blend faces).
// (#1039 occupancy) The receiver's owner is KNOWN here: the walk (clean_vis_walk.glsl rtVisWalkF) resolves an
// UNKNOWN one with rtSelfProbe outside its traversal -- it ends the walk at the first owner != 0 candidate,
// probes, and walks again -- so no probe query is nested in a walk's.
uint rtSelfCandidate(uint owner, uint id)
{
    if (owner == 0u) return 0u;
    if (owner == rt_self_owner) return 1u;
    if (!rtIdIn(id)) return 2u;   // (#874 guard: a bad id blocks)
    return (rt_instances[id].flags & 6u) == 0u ? 2u : 0u;
}

float rt_anim_time = 0.0;
#ifndef RT_ANIM_TIME
#define RT_ANIM_TIME rt_anim_time   // (WALK-STATE HOOKS above)
#endif

// BLOCKER IDENTITY HOOK (shadow denoiser, clean_rt_shadow_mask.comp only): with
// RT_TRACK_BLOCKER defined, every place below that records a blocker distance also
// hands the blocker's clean Instance id to rtNoteBlocker (defined by the includer: it
// flags MOVING casters -- avatars, attachments, moved / spinning prims -- so the
// temporal pass can tell a moving shadow from static sub-pixel aliasing). Compiled out
// everywhere else (zero cost in the lit FS).
#ifdef RT_TRACK_BLOCKER
void rtNoteBlocker(uint id);
#define RT_NOTE_BLOCKER(id) rtNoteBlocker(id)
#define RT_NOTE_COMMITTED(rq) rtNoteBlocker(rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)), uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true))))
#else
#define RT_NOTE_BLOCKER(id)
#define RT_NOTE_COMMITTED(rq)
#endif

// BLOCKER DISTANCE (shadow denoiser, clean_rt_shadow_mask.comp): every shadow trace
// below lowers this to the distance of the first caster it met -- an opaque / alpha-
// mask hit, or a translucent caster that dimmed the light. The traces end on the FIRST
// hit found, not the nearest, so it is an estimate; the mask takes the minimum over a
// pixel's rays. Only the mask resets and reads it (inert for every other caller).
// With the translucent split only the opaque-set rays (traceShadowRayVis) lower it.
float rt_block_t = 1e30;

// ---- SECONDARY-RAY ORIGIN (self-intersection avoidance) ----
// Wachter & Binder, "A Fast and Robust Method for Avoiding Self-Intersection" (Ray
// Tracing Gems, ch. 6): move the origin off the receiver along its geometric normal n
// (on the side the ray leaves from) by a few float ULPs of the coordinates themselves,
// so the offset follows the real float error instead of a fixed distance.
//
// OUR SPACE: receiver positions and the TLAS are ABSOLUTE agent (region-local) world
// space -- the RT shadow mask rebuilds P = camera + camera-relative offset
// (clean_rt_mask_common.glsl worldAt), the lit FS interpolates absolute v_world_pos, the
// rigged BLAS holds pre-skinned positions + the eye (clean_rt_instances.comp rigCopy) and
// static BLASes are placed by absolute instance transforms. |x|, |y| are up to ~256 m
// in the agent's region (a few hundred beyond it), z a few tens of metres on the
// ground and up to 4096 m for skyboxes. One ULP: 1.5e-5 m at 128-256 m, 3e-5 m at
// 256-512 m, 4.9e-4 m at 4096 m.
// The error sources between the rasterized receiver and the traced triangle:
//   * rounding of each coordinate (P = cam + offset, the rigged BLAS = eye-relative
//     skin + eye, the hit interpolation): ~1-2 ULPs of THAT coordinate -> the W&B
//     integer step, RT_OFS_INT_SCALE ULPs per component;
//   * static geometry rasterizes through the ABSOLUTE view-projection (clean_draw.vert:
//     world terms of ~|coordinate| cancel against the camera translation down to a
//     view depth of ~camd), so its depth carries ~2-4 ULPs of the LARGEST coordinate
//     in every direction, whatever n's dominant axis (ground at (200, 200, 25): the z
//     step alone would be 8x too small) -> a frame floor of RT_OFS_FRAME_ULPS ULPs of
//     max |p_i| along n. It also is W&B's near-zero fallback: components below
//     RT_OFS_ORIGIN take no integer step (their bits would cross zero);
//   * RT_OFS_FLOAT_SCALE: an absolute floor (m) for a frame whose every coordinate is
//     tiny.
// W&B's own constants (int_scale 256, origin 1/32, float_scale 1/65536) are sized for
// unit-scale scenes with deep transform chains: 256 ULPs is ~8 mm at 256 m and jumps
// the stacked layers of SL mesh avatars. These give ~0.1-0.25 mm in a region and
// ~4-6 mm at skybox altitude.
#define RT_OFS_ORIGIN      (1.0 / 32.0)
#define RT_OFS_INT_SCALE   12.0
#define RT_OFS_FRAME_ULPS  6.0
#define RT_OFS_FLOAT_SCALE (1.0 / 131072.0)
// DEPTH RECONSTRUCTION (receivers rebuilt from the reversed-Z D32F depth buffer): the
// stored depth ~ near / view depth keeps ~2^-24 relative precision; rasterizer plane
// interpolation and the un-projection add a few ULPs. 2^-21 (8 relative ULPs) of the
// camera distance: 0.5 um at 1 m, 0.05 mm at 100 m. (The absolute-space raster error
// of static geometry is the frame floor above, not this.)
#define RT_DEPTH_REL_ERR   (1.0 / 2097152.0)

// Spacing of floats at |x| (normal range; 0 below it).
float rtUlp(float x)
{
    const uint e = (floatBitsToUint(x) >> 23) & 0xFFu;
    return e > 23u ? uintBitsToFloat((e - 23u) << 23) : 0.0;
}
float rtOffsetComp(float p, float n, float fl)
{
    const float pf = p + n * fl;                    // frame floor
    if (abs(p) < RT_OFS_ORIGIN) return pf;          // near zero: no integer step
    const int oi = int(RT_OFS_INT_SCALE * n);
    const float pi = intBitsToFloat(floatBitsToInt(p) + (p < 0.0 ? -oi : oi));
    return abs(pi - p) > abs(pf - p) ? pi : pf;     // both move along sign(n): the farther
}
// p: the receiver / hit; n: unit GEOMETRIC normal on the side the ray leaves from.
vec3 offsetRayOrigin(vec3 p, vec3 n)
{
    const float m  = max(max(abs(p.x), abs(p.y)), abs(p.z));
    const float fl = max(RT_OFS_FRAME_ULPS * rtUlp(m), RT_OFS_FLOAT_SCALE);
    return vec3(rtOffsetComp(p.x, n.x, fl), rtOffsetComp(p.y, n.y, fl), rtOffsetComp(p.z, n.z, fl));
}
// A receiver rebuilt from the depth buffer / rasterized at camera distance camd, plus
// the user's extra offset (RenderVKRTShadowBias, m; default 0).
vec3 rtReceiverOrigin(vec3 p, vec3 n, float camd, float extra)
{
    return offsetRayOrigin(p + n * (camd * RT_DEPTH_REL_ERR + extra), n);
}

// RAY CONE texture LOD for the alpha test (Akenine-Moller et al., "Improved Shader
// and Texture Level of Detail Using Ray Cones", JCGT 2021). Cone width at hit
// distance t = rt_cone_width0 + rt_cone_spread * t (metres). Primary rays: width0 0,
// spread = the camera's per-pixel angle. Secondary (shadow) rays: width0 = the
// originating surface's pixel footprint (pixel angle * camera distance), spread ~0
// (or the light's angular size). Both 0 => mip 0.
// rt_cone_aniso mirrors the raster sampler: 1 = isotropic (LOD from the footprint's
// major axis w/|n.d|), N > 1 = anisotropic filtering up to N:1 (LOD from
// max(minor, major/N), what the hardware picks).
float rt_cone_spread = 0.0;
float rt_cone_width0 = 0.0;
float rt_cone_aniso  = 1.0;
#ifndef RT_CONE_WIDTH0
#define RT_CONE_WIDTH0 rt_cone_width0   // (WALK-STATE HOOKS above)
#endif
#ifndef RT_CONE_SPREAD
#define RT_CONE_SPREAD rt_cone_spread
#endif
#ifndef RT_CONE_ANISO
#define RT_CONE_ANISO rt_cone_aniso
#endif

// ALPHA-MASK TEST of a candidate (cutoff >= 0; not alpha-blend), as the RASTER decides it
// (clean_draw.frag: maskTestAlpha(base.a, lod) < cutoff discards). The cone above is the
// footprint the hit's COLOUR is filtered over: reflection rays widen it by their ray density
// (one ray standing for up to 8 x 8 pixels) and by the receiver's roughness, which is right
// for colour (the resolve filters over that footprint anyway) but wrong for a BINARY coverage
// test: a thin cut-out (eyebrow / lash strands, hair cards, grass blades) sampled 1 - 4 mips
// coarser than the raster's pixel averages its alpha below the cutoff on every texel and the
// whole feature vanishes from the reflection -- worst on PLAIN alpha mips (server bakes /
// Bakes on Mesh are never given coverage-preserving chains, #786; nor a chain not rebuilt
// yet), where the raster survives only through maskTestAlpha's per-mip coverage scaling.
// Callers that set these test the mask the raster's way (clean_deferred_light.comp
// rtrMaskCone); their defaults keep every other consumer's rule unchanged:
//   rt_mask_width0 >= 0: the test's cone (width0 + spread x t, the plain PIXEL footprint)
//     replaces rt_cone_* for the mask test (anisotropy as rt_cone_aniso); -1 = rt_cone_*.
//   rt_mask_lod_bias: added to the test's LOD (the raster's material mip bias: taaLodBias).
//   rt_mask_lod_max: the test's LOD cap (0 = mip 0 always; debug A/B).
//   rt_mask_cov_k: maskTestAlpha's coverage scale (RenderVKAlphaMipCoverage): the TESTED
//     alpha is scaled by 1 + lod x k (the returned alpha of a mask material is only ever
//     compared with its cutoff). 0 = off.
// rt_mask_boost: OUT, the scale the last mask test applied (1 = none; diagnostics: a hit that
// passes only because of it was "rescued" by the coverage rule).
float rt_mask_width0   = -1.0;
float rt_mask_spread   = 0.0;
float rt_mask_lod_bias = 0.0;
float rt_mask_lod_max  = 64.0;
float rt_mask_cov_k    = 0.0;
float rt_mask_boost    = 1.0;

// RECEIVER-PIXEL FOOTPRINT for SHADOW rays (clean_shadow_rt.glsl rtSetShadowFootprint;
// replaces the cone above while rt_foot_on). The alpha test of a shadow ray must decide
// coverage the way the RASTER decided it at the receiver's pixel -- otherwise a stack of
// cut-out layers (shell grass: parallel planes of sparse dots) is see-through on screen
// at one mip and solid to the shadow ray at another, and the visible lower layers fall
// into their own upper layers' shadow. rt_foot_a / _b: the receiver pixel's footprint
// ellipse axes in WORLD space on the receiver's plane (minor = across the view, major =
// along it, stretched by 1 / N.V), already scaled by the raster's material mip bias.
// The candidate projects them along the ray onto its own triangle, maps them to UV
// derivatives and samples with textureGrad: the SAME sampler (its anisotropy) and the
// same footprint the raster's derivatives give -- for a caster parallel to the receiver
// (stacked layers) exactly the raster's LOD and anisotropic filter.
bool rt_foot_on = false;
vec3 rt_foot_a = vec3(0.0);
vec3 rt_foot_b = vec3(0.0);
#ifndef RT_FOOT_AB
#define RT_FOOT_AB(A, B) { A = rt_foot_a; B = rt_foot_b; }   // (WALK-STATE HOOKS above)
#endif
// (#1031, RenderVKShadowIsoLod) RT_NO_FOOT_LOD: the includer's shadow walks take an ISOTROPIC LOD --
// the footprint's minor axis as the ray cone's constant width (clean_shadow_core.glsl shadowSampleVis)
// -- and the alpha test compiles no footprint projection / textureGrad path (rt_foot_on never set
// when it runs). Only kernels that are shadow walks alone define it.
#ifdef RT_NO_FOOT_LOD
#define RT_FOOT_ON false
#else
#define RT_FOOT_ON rt_foot_on
#endif

// ALPHA-BLEND CASTERS in shadow rays (clean_shadow_rt.glsl shadowRayVis /
// clean_rt_shade.glsl rtHitSunShadow set these around their traces; false for every
// other caller -- visibility / closest-hit rays never read them).
// rt_blend_parity (RenderVKRTBlendShadowParity): LL's shadow-pass rule
//   (clean_translucent.glsl llBlendShadowPass): a blend texel at 0.88 or more is solid
//   -- an opaque caster, confirmed by the opaque-set rays of the translucent split too
//   (traceShadowRayVis), so leaf / fence shapes get the same soft penumbra as other
//   opaque casters <SS:Skoomastream>. Below that: coloured (#212) the caster passes
//   translucentTransmit (nothing below 0.05); grey, LL's own result
//   (rt_cand_ll_pass: nothing under its 0.598 texture cut / 0.05, else 50%).
// rt_blend_coarse (RenderVKProbeRTBlendCoarse, probe captures): a blend caster's alpha /
//   colour comes from the texture's COARSEST mip (its average) -- no triangle fetch, face
//   UVs or ray-cone LOD. Captures are GGX-filtered; texel-accurate translucent shadow
//   detail is invisible there.
bool rt_blend_parity = false;
bool rt_blend_coarse = false;
// <SS:Skoomastream> Set by rtCandidateAlphaCol for blend candidates: the light LL's shadow
// pass lets through that texel (llBlendShadowPass: 1, 0.5 or 0).
float rt_cand_ll_pass = 1.0;

// FOLIAGE LIGHT TRANSMISSION (#735; RenderVKRTFoliageTransmission, frame info foliage_rt,
// loaded by rtFoliageSet around the SUN / MOON traces only -- projectors, probes' closest
// hits and the debug views stay outside it: mode 0, no facing cull). The shadow pass culls the back faces of
// single-sided casters, so a leaf card whose FRONT faces away from the sun casts nothing
// in the shadow map, while a shadow ray met it as a solid wall: a dense canopy went solid
// black under RT (no dappled light, no god rays).
// FACING: a shadow ray runs receiver -> light. It meets a leaf whose front faces AWAY
// from the light on that front side -- exactly the face the shadow map culls. Those hits
// are TRANSMISSIVE; light-facing hits that pass the alpha test occlude as before.
// The hardware facing (the culling ray flags / rayQueryGetIntersectionFrontFaceEXT) is
// the spec's ray-space area sign: front <=> dot(cross(p1 - p0, p2 - p0), dir) < 0 in the
// instance's OBJECT space -- the raster's rule (rtCandidateFront) for every instance
// whose transform keeps orientation; clean_rt_instances.comp sets FLIP_FACING on the
// mirrored (negative-determinant) ones, so it holds for those too. rt_fol_flip
// (RenderVKRTFoliageFacingFlip, debug) inverts the convention, A/B only.
// SCOPE: the sun / moon shadow rays always ask for facing culling (rt_fol_cull); WHO is
// cullable is the TLAS instance's CULL_DISABLE bit (clean_rt_instances.comp geomFlags /
// pass B): single-sided static alpha-mask instances (TLAS class RT_MASK_ALPHAMASK) while
// the mode is not 0, and -- RIGGED / WORN FACING PARITY (RenderVKRTRiggedFacingParity),
// whatever the mode -- single-sided avatar casters: rigged meshes (bodies, worn rigged
// clothing, animesh, the own avatar) and worn rigid attachments, opaque or alpha-mask.
// The shadow pass draws those back-face culled (mShadowRigPipe / mShadowRigMaskPipe,
// mShadowPipe / mShadowMaskPipe), so an "inverted hull" outline (an inside-out shell a few
// mm off the body) never cast there; as a two-sided RT occluder it shadowed the whole
// face it wraps. Opaque walls / floors, terrain, water and alpha-blend stay CULL_DISABLE.
// Double-sided materials (flag 256) cast from both sides in the shadow map: they stay
// occluders from either side here too (CULL_DISABLE; the transmission walk skips them).
// Solid alpha-blend casters (alpha >= RT_BLEND_SOLID) get the same facing rule in the
// shader (rtFolAway): static leaves with the mode, worn / rigged ones with the parity.
// Tinted transmission is FOLIAGE only (the alpha-mask class): avatars never colour light.
// MODES: 0 = off (every leaf hit occludes, the old behaviour); 1 = CULL (shadow-map
// parity): the shadow rays cull the away-facing leaf faces in traversal -- a ray flag, no
// candidate, no shader work, opacity micromaps untouched (facing culling runs before
// the micromap lookup); 2 = TINTED: mode 1's rays, then ONE transmission ray per lit
// receiver over the alpha-mask class (traceFoliageTransmit) that colours the light by the
// first rt_fol_layers away-facing leaves it passes (rtFolLayer).
// (#1031) The parameters stay the frame info's raw vec4 (every caller loads ssr.foliage_rt -- a
// dispatch-uniform value the compiler can keep in the constant bank) and are decoded where read
// (rtFolMode() ...): no six derived private values live across the walks. rt_fol_cull is the
// per-thread "inside a sun / moon trace" state; mode and rig read 0 / false outside one.
vec4  rt_fol_p    = vec4(0.0, 0.35, 0.5, 2.0);   // x mode (bits 0-1) | flip (2) | rig (3), y strength, z tint, w layers
// (#1039 occupancy: WALK-STATE HOOKS) read only inside a sun / moon trace (rt_fol_cull: every reader below is
// behind it); the visibility queue's shadow walks, whose every trace sets the frame info's values, read those
#ifndef RT_FOL_P
#define RT_FOL_P rt_fol_p
#endif
bool  rt_fol_cull = false;   // inside a sun / moon trace (rtFoliageSet .. rtFoliageEnd)
uint  rtFolBits()     { return uint(RT_FOL_P.x + 0.5); }
uint  rtFolMode()     { return rt_fol_cull ? min(rtFolBits() & 3u, 2u) : 0u; }
bool  rtFolRig()      { return rt_fol_cull && (rtFolBits() & 8u) != 0u; }   // rigged / worn facing parity (RenderVKRTRiggedFacingParity)
bool  rtFolFlip()     { return (rtFolBits() & 4u) != 0u; }
float rtFolStrength() { return clamp(RT_FOL_P.y, 0.0, 1.0); }
float rtFolTint()     { return clamp(RT_FOL_P.z, 0.0, 1.0); }
uint  rtFolLayers()   { return clamp(uint(RT_FOL_P.w + 0.5), 1u, 8u); }
// A blend caster at or above this alpha is SOLID (LL's shadow-pass threshold; see
// rt_blend_parity): the only blend hits the facing rule changes.
#define RT_BLEND_SOLID ALPHA_BLEND_SOLID
// p = frame info foliage_rt: x mode (bits 0-1) | facing flip (bit 2) | rigged / worn
// facing parity (bit 3), y strength, z tint amount, w layer cap.
void rtFoliageSet(vec4 p)
{
    rt_fol_p    = p;
    rt_fol_cull = true;
}
// The end of a sun / moon trace: every other ray keeps every face an occluder (the parameters keep
// their values, as before: only mode / rig / the culling switch off).
void rtFoliageEnd()
{
    rt_fol_cull = false;
}
// Sun / moon shadow rays (every mode): cull the away-facing faces of the cullable
// instances -- those the TLAS left without CULL_DISABLE (see SCOPE above).
uint rtFolShadowFlags()
{
    if (!rt_fol_cull) return gl_RayFlagsNoneEXT;
    return rtFolFlip() ? gl_RayFlagsCullBackFacingTrianglesEXT : gl_RayFlagsCullFrontFacingTrianglesEXT;
}
// One transmissive layer: strength x the leaf's HUE (unit-max chroma, as
// translucentTransmit) blended in by the tint amount. Luminance comes from the strength
// alone -- a dark albedo texture would otherwise swallow the light it should colour.
vec3 rtFolLayer(vec3 col)
{
    const float m = max(max(col.r, col.g), col.b);
    const vec3 hue = m > 1e-4 ? col / m : vec3(1.0);
    return rtFolStrength() * mix(vec3(1.0), hue, rtFolTint());
}
// Candidate-side facing of a hit on an instance the culling flags cannot reach (alpha-blend
// is CULL_DISABLE). The shadow pass's alpha FS drops the back faces of single-sided blend
// casters too, static and rigged (clean_shadow_mask.frag / clean_shadow_trans.frag).
//   RT_FOL_OCCLUDE: the hit occludes (light-facing, double-sided, or its rule is off).
//   RT_FOL_LEAF: an away-facing face of a single-sided, static, unworn caster (mode != 0):
//     a SOLID one (alpha >= RT_BLEND_SOLID / past its cutoff) is transmissive, tinted in
//     mode 2 (#735); a fainter one stays translucent.
//   RT_FOL_PASS: an away-facing face of a single-sided worn / rigged / own-avatar caster
//     (rt_fol_rig): passes at any alpha, never tinted (avatars do not colour the light).
// Cullable candidates that still arrive are light-facing (or double-sided), so this is
// RT_FOL_OCCLUDE for them -- the rule is the same one the cull flags apply.
#define RT_FOL_OCCLUDE 0u
#define RT_FOL_LEAF    1u
#define RT_FOL_PASS    2u
uint rtFolAway(uint id, bool front)
{
    if (!rt_fol_cull || front == rtFolFlip() || !rtIdIn(id)) return RT_FOL_OCCLUDE;
    const uint f = rt_instances[id].flags;
    if ((f & 256u) != 0u) return RT_FOL_OCCLUDE;                                      // double-sided
    if ((f & (512u | 8192u)) != 0u || (rt_geom[id].z & 0x80000000u) != 0u)          // worn / own / rigged
        return rtFolRig() ? RT_FOL_PASS : RT_FOL_OCCLUDE;
    return rtFolMode() != 0u ? RT_FOL_LEAF : RT_FOL_OCCLUDE;
}

struct RtHit {
    bool  hit;
    float dist;       // ray t (dir normalized => metres)
    uint  instance;   // clean Instance id (instanceCustomIndex)
    uint  prim;       // triangle index within the slot
    vec2  bary;       // (b1, b2): v = v0*(1-b1-b2) + v1*b1 + v2*b2
    bool  front;
    mat4x3 o2w;       // object -> world of the hit instance (rigged: the owner's pelvis, #632)
};

// GUARDED (#874): a legacy triangle is checked against its geometry's triangle count
// (rt_geom.w; an EMPTY row = a hit on an instance pass A / B did not give this frame's
// geometry), the IBO's length and -- per index -- the geometry's vertex count (rt_geom_vc,
// 0 = unknown). A failing triangle returns its first vertex three times (degenerate: zero area,
// every vertex read in range) -- (0, 0, 0) when the base itself is bad.
uvec3 rtTriIndices(uint id, uint prim)
{
    rtGuardCtx(id, prim);
    if (!rtIdIn(id)) return uvec3(0u);
    if (rtIsCluster(id))
    {
        // the cluster (clean_cluster_common.glsl page layout, no cluster-set includes here)
        const uint g = prim >> 7u, t = prim & 127u;
        const uint ref = rtIsClusterRigged(id) ? rtLdClRig(16u + (rt_geom[id].x + g) * 4u)
                       : (rt_geom[id].z & RT_GEOM_CLUSTER_DIRECT) != 0u ? g : rtLdClCut(rt_geom[id].x + g);
        const uint slot = ref >> 6u, ci = ref & 63u;
        const uint rw = slot * 20480u + (32u + ci * 64u) / 4u;          // ClusterRecord words
        const uint first = slot * 4096u + rtLdPool(rw + 8u);           // vert_off20: first real vertex
        const uint ib = slot * 81920u + rtLdPool(rw + 10u) * 4u + t * 3u;   // index_off4 -> bytes
        uvec3 l;
        for (uint k = 0u; k < 3u; ++k)
        {
            const uint b = ib + k;
            l[k] = (rtLdPool(b >> 2u) >> ((b & 3u) * 8u)) & 0xFFu;
        }
        return ((uvec3(first) + l) | uvec3(RT_POOL_VERT)) - uvec3(rt_geom[id].y);   // (vb + this = pool vertex)
    }
    const uvec4 gr = rt_geom[id];
    const uint cnt = gr.w & 0x7FFFFFFFu;
    if (!RTG_OK(cnt != 0u, RTG_GEOMROW, prim, 0u)) return uvec3(0u);
    if (!RTG_OK(prim < cnt / 3u, RTG_PRIM, prim, cnt / 3u)) return uvec3(0u);
    const uint b = gr.x + prim * 3u;
    const uint ilen = uint(rt_ibo.length()) * 2u;   // 16-bit indices
    if (!RTG_OK(b < ilen && ilen - b >= 3u, RTG_IBO, b, ilen)) return uvec3(0u);
    const uvec3 t = uvec3(QV_INDEX16(rt_ibo, b), QV_INDEX16(rt_ibo, b + 1u), QV_INDEX16(rt_ibo, b + 2u));
    const uint vc = rtGeomVc(id);
    const uint tm = max(max(t.x, t.y), t.z);
    if (vc != 0u && !RTG_OK(tm < vc, RTG_VTX, tm, vc)) return uvec3(0u);
    return t;
}

// Static vertex v (absolute) of the slot whose first real vertex is vb (rt_geom.y): the
// slot's decode header first (#573), then the vertex attributes. (A RT_POOL_VERT vb / v: a
// cluster hit's, from the pool.)
QvHeader rtHdr(uint vb)
{
    // GUARDED (#874): the header's 2 pseudo-vertices must lie in the buffer (zero header else:
    // every position / UV decodes to 0, a degenerate triangle).
    const QvHeader zh = qvHeaderFromWords(0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    if ((vb & RT_POOL_VERT) != 0u)
    {
        const uint pb = max(vb & ~RT_POOL_VERT, QV_HEADER_VERTS);
        if (!RTG_OK(pb <= uint(rt_cl_pool.length()) / QV_WORDS, RTG_CL_POOL, pb, uint(rt_cl_pool.length()) / QV_WORDS)) return zh;
        return QV_LOAD_HEADER(rt_cl_pool, pb);
    }
    const uint b = max(vb, QV_HEADER_VERTS);
    if (!RTG_OK(b <= uint(rt_vbo.length()) / QV_WORDS, RTG_VBO, b, uint(rt_vbo.length()) / QV_WORDS)) return zh;
    return QV_LOAD_HEADER(rt_vbo, b);
}
vec2 rtVertUV(QvHeader h, uint v)      { return qvUV(h, rtVW(v, 2u)); }
vec3 rtVertPos(QvHeader h, uint v)     { return qvPos(h, rtVW(v, 0u), rtVW(v, 1u)); }
vec3 rtVertNormal(uint v)              { return qvNormal(rtVW(v, 3u)); }
vec4 rtVertTangent(uint v)             { return qvTangent(rtVW(v, 1u), rtVW(v, 4u)); }

// Positions of the hit triangle in the BLAS's space (object space for static /
// attachments, world space for rigged).
void rtTriPositions(uint id, uint prim, out vec3 p0, out vec3 p1, out vec3 p2)
{
    p0 = vec3(0.0); p1 = vec3(0.0); p2 = vec3(0.0);
    if (!rtIdIn(id)) return;   // (#874 guard: degenerate)
    uint z = rt_geom[id].z;
    if ((z & RT_GEOM_CLUSTER_RIG) != 0u)
    {
        // rigged cluster: the skinned arena (eye-relative = the BLAS's space)
        uint s;
        uvec3 l;
        rtClRigTri(id, prim, s, l);
        p0 = rtClRigPos(rtClRigVtxWord(s, l.x));
        p1 = rtClRigPos(rtClRigVtxWord(s, l.y));
        p2 = rtClRigPos(rtClRigVtxWord(s, l.z));
        return;
    }
    uvec3 t = rtTriIndices(id, prim);
    if ((z & 0x80000000u) != 0u)
    {
        uint rb = z & 0x7FFFFFFFu;
        // GUARDED (#874): the entry's skinned range must lie in rt_rigpos
        const uint rv = uint(rt_rigpos.length()) / RT_RIGPOS_WORDS, tm = rb + max(max(t.x, t.y), t.z);
        if (!RTG_OK(tm < rv, RTG_RIGPOS, tm, rv)) return;
        uvec3 r = (uvec3(rb) + t) * RT_RIGPOS_WORDS;
        p0 = uintBitsToFloat(uvec3(rt_rigpos[r.x], rt_rigpos[r.x + 1u], rt_rigpos[r.x + 2u]));
        p1 = uintBitsToFloat(uvec3(rt_rigpos[r.y], rt_rigpos[r.y + 1u], rt_rigpos[r.y + 2u]));
        p2 = uintBitsToFloat(uvec3(rt_rigpos[r.z], rt_rigpos[r.z + 1u], rt_rigpos[r.z + 2u]));
    }
    else
    {
        uint vb = rt_geom[id].y;
        const QvHeader qh = rtHdr(vb);
        p0 = rtVertPos(qh, vb + t.x); p1 = rtVertPos(qh, vb + t.y); p2 = rtVertPos(qh, vb + t.z);
    }
}

// RIGGED instance id's vertex v (slot-local index, as rtTriIndices returns): its skinned,
// welded normal in the group BLAS's space (#632: pelvis-local -- rotate by the hit's
// object-to-world) -- the very normal the rigged raster interpolates (unit; a collapsed /
// invalid entry stores 0 = +Z, which the caller's facing guard rejects).
vec3 rtRigNormal(uint id, uint v)
{
    if (!rtIdIn(id)) return vec3(0.0);
    const uint r = (rt_geom[id].z & 0x7FFFFFFFu) + v, rv = uint(rt_rigpos.length()) / RT_RIGPOS_WORDS;
    if (!RTG_OK(r < rv, RTG_RIGPOS, r, rv)) return vec3(0.0);
    return qvNormal(rt_rigpos[r * RT_RIGPOS_WORDS + 3u]);
}
// The same for corner c (0..2) of hit triangle prim, v = rtTriIndices(id, prim)[c]: also covers
// a rigged CLUSTER hit (its skinned normal from the RT skinned arena, eye-relative frame).
vec3 rtRigCornerNormal(uint id, uint prim, uint c, uint v)
{
    if (rtIsClusterRigged(id))
    {
        uint s;
        uvec3 l;
        rtClRigTri(id, prim, s, l);
        return qvNormal(rtClRigW(rtClRigVtxWord(s, l[c]), 3u));
    }
    return rtRigNormal(id, v);
}

// Instance opacity as the TLAS flags it (clean_rt_instances.comp geomFlags):
// no alpha test for opaque buckets, terrain and water.
bool rtInstanceOpaque(uint iflags)
{
    return (iflags & 6u) == 0u || (iflags & (32u | 2048u)) != 0u;
}

// Face UV of vertex v exactly as clean_draw(_rigged).vert computes it (texgen, TE /
// glTF transform, texture animation); the raster interpolates these per-vertex UVs.
// (#1039 occupancy) cleanFaceUV's steps with the instance's matrices read where each step uses them -- passed as
// arguments, both mat4 (32 floats) were loaded before the call and live at the alpha test, the walks' register
// peak; the same arithmetic (the same precise TE transform, planarProjectionUV, texAnimUV). The guard-recording
// build (the RT shadow mask) keeps the eager call: its vertex reads note violations in their old order.
vec2 rtVertexFaceUV(uint id, uint f, QvHeader h, uint v)
{
#ifdef RT_GUARD_RECORD
    return cleanFaceUV(f, rt_instances[id].uv_matrix, rtVertUV(h, v), rtVertNormal(v), rtVertPos(h, v),
                       rt_instances[id].model_matrix, RT_ANIM_TIME);
#else
    vec2 base_uv = rtVertUV(h, v);
    if ((f & 64u) != 0u)
    {
        const vec3 obj_scale = vec3(length(rt_instances[id].model_matrix[0].xyz), length(rt_instances[id].model_matrix[1].xyz),
                                    length(rt_instances[id].model_matrix[2].xyz));
        base_uv = planarProjectionUV(normalize(rtVertNormal(v)), rtVertPos(h, v) * obj_scale);
    }
    if ((f & 1024u) != 0u)
        return texAnimUV(base_uv, rt_instances[id].uv_matrix, RT_ANIM_TIME);
    precise vec2 r = (rt_instances[id].uv_matrix * vec4(base_uv, 0.0, 1.0)).xy;
    return r;
#endif
}
// (#1039 occupancy) The same for the alpha test's corners, the slot's decode header (vb: its first real vertex)
// read by the step that uses it -- its UV part for every corner, its position part only for planar texgen --
// instead of all ten words before the call (rtVertexFaceUV's steps: keep in step).
vec2 rtVertexFaceUVB(uint id, uint f, uint vb, uint v)
{
#ifdef RT_GUARD_RECORD
    return rtVertexFaceUV(id, f, rtHdr(vb), v);
#else
    vec2 base_uv = rtVertUV(rtHdr(vb), v);
    if ((f & 64u) != 0u)
    {
        const vec3 obj_scale = vec3(length(rt_instances[id].model_matrix[0].xyz), length(rt_instances[id].model_matrix[1].xyz),
                                    length(rt_instances[id].model_matrix[2].xyz));
        base_uv = planarProjectionUV(normalize(rtVertNormal(v)), rtVertPos(rtHdr(vb), v) * obj_scale);
    }
    if ((f & 1024u) != 0u)
        return texAnimUV(base_uv, rt_instances[id].uv_matrix, RT_ANIM_TIME);
    precise vec2 r = (rt_instances[id].uv_matrix * vec4(base_uv, 0.0, 1.0)).xy;
    return r;
#endif
}

// The raster's alpha-mask rule at a candidate hit (clean_draw.frag): base =
// base_color_factor * tint * texel(base colour, face UV); discard if alpha_cutoff >= 0
// && base.a < alpha_cutoff. Terrain / untextured faces use the constant alpha.
// Alpha-blend materials (cutoff < 0) cut at RT_BLEND_CUTOFF. The texel is sampled
// through the SAME bindless sampler at the ray-cone LOD (see rt_cone_*).
//   t   = hit distance, dir = normalized ray direction, q = the ray query standing on the
//         CANDIDATE: its object -> world transform (the positions are in BLAS space) is read
//         only where a LOD path needs it (#1031: no 12-float argument per candidate).
// Candidate coverage: alpha at the hit and the cutoff the raster would apply.
// blend = true for alpha-blend materials (cutoff = RT_BLEND_CUTOFF for callers that
// want a binary answer; shadow rays treat these as translucent instead).
// col = the linear base colour at the hit (factor x tint x texel): the hue a
// translucent caster gives the light it passes (#212).
float rtCandidateAlphaCol(uint id, uint prim, vec2 bary, float t, vec3 dir, rayQueryEXT q,
                          out float cutoff, out bool blend, out vec3 col)
{
    blend = false;
    cutoff = 0.0;
    col = vec3(1.0);
    rt_mask_boost = 1.0;
    if (!rtIdIn(id)) return 1.0;   // (#874 guard: a bad id is an opaque blocker)
    rtGuardCtx(id, prim);
    uint mid = rt_instances[id].material_id;
    if (mid >= uint(rt_materials.length()))
    {
        if (mid != 0xFFFFFFFFu) rtGuardFail(RTG_MAT, mid, uint(rt_materials.length()));
        return 1.0;
    }
    uint iflags = rt_instances[id].flags;
    // An OPAQUE-bucket face that arrives as a candidate (a NoOpaque ray, or a no-self-shadow
    // character's FORCE_NO_OPAQUE record): solid, as its opaque TLAS / geometry flag makes it
    // for every other ray -- never the alpha test the raster does not run on it.
    if (rtInstanceOpaque(iflags)) return 1.0;
    cutoff = rt_materials[mid].alpha_cutoff;
    if (cutoff < 0.0)
    {
        if ((iflags & 2u) == 0u) { cutoff = 0.0; return 1.0; }   // opaque material on a no-opaque instance
        cutoff = RT_BLEND_CUTOFF;
        blend = true;
    }
    float a = rt_materials[mid].base_color_factor.a * rt_instances[id].tint.a;
    col = rt_materials[mid].base_color_factor.rgb * rt_instances[id].tint.rgb;
    float tex_a = 1.0;   // the base-colour texel's own alpha (LL's 0.598 cut)
    // GUARDED (#874): the bindless index against the written table (mask only: the guard
    // buffer carries the count); a bad one is an opaque blocker (no fetch).
    if ((rt_materials[mid].flags & 1u) != 0u && (iflags & 32u) == 0u)
    {
        const uint tc = rtGuardTexCount();
        if (!RTG_OK(rt_materials[mid].base_color_handle.x < tc, RTG_TEX, rt_materials[mid].base_color_handle.x, tc))
        { blend = false; cutoff = 0.0; return 1.0; }
    }
    if (blend && rt_blend_coarse && (rt_materials[mid].flags & 1u) != 0u && (iflags & 32u) == 0u)
    {
        // Coarsest mip (textureLod clamps the LOD to the last level): the texture's mean.
        const vec4 texel = textureLod(uTextures[nonuniformEXT(rt_materials[mid].base_color_handle.x)],
                                      vec2(0.5), 32.0);
        a *= texel.a;
        col *= texel.rgb;
        if (blend) rt_cand_ll_pass = llBlendShadowPass(texel.a, a, (rt_materials[mid].flags & 64u) != 0u);
        return a;
    }
    if ((rt_materials[mid].flags & 1u) != 0u && (iflags & 32u) == 0u)
    {
        uvec3 tri = rtTriIndices(id, prim);
        uint  vb  = rt_geom[id].y;
        const bool rigged = (rt_geom[id].z & 0x80000000u) != 0u;
        uint f = rigged ? (iflags & ~64u) : iflags;   // rigged: no planar texgen (clean_draw_rigged.vert)
        // (#1039 occupancy) the three corners' face UVs ONE AT A TIME (a loop, not unrolled): the decode header,
        // the face's uv matrix and model matrix are loaded for the corner being mapped, not held for all three --
        // the same arithmetic per corner (cleanFaceUV), ~16 fewer registers at the alpha test, the walks' peak
#ifdef RT_GUARD_RECORD
        // (the guard-recording build, the RT shadow mask: the decode header read once, the corners in their old
        //  order -- a violation is noted as often and in the order it always was)
        const QvHeader qh = rtHdr(vb);
        vec2 t0 = rtVertexFaceUV(id, f, qh, vb + tri.x);
        vec2 t1 = rtVertexFaceUV(id, f, qh, vb + tri.y);
        vec2 t2 = rtVertexFaceUV(id, f, qh, vb + tri.z);
#else
        vec2 t0 = vec2(0.0), t1 = vec2(0.0), t2 = vec2(0.0);
        [[dont_unroll]] for (uint k = 0u; k < 3u; ++k)
        {
            const vec2 tk = rtVertexFaceUVB(id, f, vb, vb + (k == 0u ? tri.x : (k == 1u ? tri.y : tri.z)));
            if (k == 0u) t0 = tk;
            else if (k == 1u) t1 = tk;
            else t2 = tk;
        }
#endif
        vec2 uv = t0 * (1.0 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y;
        uint tex = rt_materials[mid].base_color_handle.x;
        // The triangle's positions, fetched once for whichever LOD path needs them (one
        // inlined fetch instead of two: compile cost, see RT_WALK_*).
        // (alpha-MASK test: the caller's pixel cone when it set one -- rt_mask_* above)
        const bool mtest = !blend;
        const float width = (mtest && rt_mask_width0 >= 0.0) ? rt_mask_width0 + rt_mask_spread * t
                                                              : RT_CONE_WIDTH0 + RT_CONE_SPREAD * t;
        vec3 p0 = vec3(0.0), p1 = vec3(0.0), p2 = vec3(0.0);
        if (RT_FOOT_ON || width > 0.0) rtTriPositions(id, prim, p0, p1, p2);
#ifndef RT_NO_FOOT_LOD   // (#1031: the shadow walks' isotropic variants carry no anisotropic footprint code)
        if (rt_foot_on)
        {
            // Receiver footprint (rt_foot_a / _b) carried along the ray onto this
            // triangle's plane, then world -> UV through the triangle's own mapping.
            const mat3 M = mat3(rayQueryGetIntersectionObjectToWorldEXT(q, false));   // (#1031: read here, not passed)
            const vec3 e1 = M * (p1 - p0), e2 = M * (p2 - p0);
            const vec3 n = cross(e1, e2);
            const float nl = length(n);
            const float g11 = dot(e1, e1), g12 = dot(e1, e2), g22 = dot(e2, e2);
            const float det = g11 * g22 - g12 * g12;
            vec2 ga = vec2(0.0), gb = vec2(0.0);
            if (nl > 1e-20 && det > 1e-30)
            {
                // Grazing caster: the footprint stretch is capped at 64:1 (the sampler
                // caps anisotropy far below that; the LOD is then the coarse one).
                float dn = dot(dir, n);
                const float dmin = nl / 64.0;
                if (abs(dn) < dmin) dn = dn < 0.0 ? -dmin : dmin;
                vec3 footA, footB;
                RT_FOOT_AB(footA, footB);
                const vec3 fa = footA - dir * (dot(footA, n) / dn);
                const vec3 fb = footB - dir * (dot(footB, n) / dn);
                const vec2 d1 = t1 - t0, d2 = t2 - t0;
                const float ia = 1.0 / det;
                ga = ((g22 * dot(fa, e1) - g12 * dot(fa, e2)) * d1 + (g11 * dot(fa, e2) - g12 * dot(fa, e1)) * d2) * ia;
                gb = ((g22 * dot(fb, e1) - g12 * dot(fb, e2)) * d1 + (g11 * dot(fb, e2) - g12 * dot(fb, e1)) * d2) * ia;
            }
            const vec4 texel = textureGrad(uTextures[nonuniformEXT(tex)], uv, ga, gb);
            a *= texel.a;
            col *= texel.rgb;
            return a;
        }
#endif
        float lod = 0.0;
        float a_lod = 0.0;   // (the mask test's unclamped LOD: maskTestAlpha's input, as textureQueryLod)
        if (width > 0.0)
        {
            const mat3 M = mat3(rayQueryGetIntersectionObjectToWorldEXT(q, false));   // (#1031: read here, not passed)
            vec3 n = cross(M * (p1 - p0), M * (p2 - p0));
            float pa = length(n);                                          // 2 x world area
            vec2 e1 = t1 - t0, e2 = t2 - t0;
            vec2 tsz = vec2(textureSize(uTextures[nonuniformEXT(tex)], 0));
            float ta = abs(e1.x * e2.y - e1.y * e2.x) * tsz.x * tsz.y;     // 2 x texel area
            if (pa > 1e-20 && ta > 1e-20)
            {
                float ndotd = max(abs(dot(n / pa, dir)), 1e-4);
                float major = width / ndotd;
                float foot = (RT_CONE_ANISO > 1.0) ? max(width, major / RT_CONE_ANISO) : major;
                lod = 0.5 * log2(ta / pa) + log2(foot);
                if (mtest) lod += rt_mask_lod_bias;
                a_lod = lod;
                float levels = float(textureQueryLevels(uTextures[nonuniformEXT(tex)]));
                lod = clamp(lod, 0.0, max(levels - 1.0, 0.0));
            }
        }
        if (mtest) lod = min(lod, max(rt_mask_lod_max, 0.0));
        const vec4 texel = textureLod(uTextures[nonuniformEXT(tex)], uv, lod);
        a *= texel.a;
        col *= texel.rgb;
        tex_a = texel.a;
        // maskTestAlpha (the raster's per-mip coverage rule): only the tested alpha is scaled.
        if (mtest && rt_mask_cov_k > 0.0 && cutoff <= 0.9706)   // <GMDX> not above the ceiling (a dissolve: maskTestAlpha)
        {
            rt_mask_boost = alphaCoverage(1.0, a_lod, rt_mask_cov_k);   // (#1044: THE rule's coverage scale)
            a *= rt_mask_boost;
        }
    }
    if (blend) rt_cand_ll_pass = llBlendShadowPass(tex_a, a, (rt_materials[mid].flags & 64u) != 0u);
    return a;
}

float rtCandidateAlpha(uint id, uint prim, vec2 bary, float t, vec3 dir, rayQueryEXT q,
                       out float cutoff, out bool blend)
{
    vec3 col;
    return rtCandidateAlphaCol(id, prim, bary, t, dir, q, cutoff, blend, col);
}

bool rtAlphaOpaque(uint id, uint prim, vec2 bary, float t, vec3 dir, rayQueryEXT q)
{
    float cutoff; bool blend;
    float a = rtCandidateAlpha(id, prim, bary, t, dir, q, cutoff, blend);
    return a >= cutoff;
}

// ---- WORLD BLOCKERS: "does this surface stop sound (or rain)?" ----
// Shared by the acoustics probe (clean_audio_rt.comp, AudioAcousticsAlphaBlocks) and meant
// for the weather plan's shelter rays: trace RT_MASK_WORLD_BLOCK with gl_RayFlagsNoneEXT and
// run every candidate through rtWorldBlockCandidate. Opaque / terrain instances are
// FORCE_OPAQUE (never candidates). The rule (owner, 2026-10-18) is the alpha you would SEE
// at the hit point: fully transparent passes, anything even slightly visible blocks.
//   * ALPHA-BLEND (RT_MASK_BLEND, flag 2): face alpha (material factor x face tint) <=
//     RT_BLOCK_ALPHA_EPS passes WITHOUT a texture fetch (invisiprims-style 100%-transparent
//     faces); otherwise factor x tint x texel(face UV) > RT_BLOCK_ALPHA_EPS blocks -- a
//     95%-transparent window blocks, a fully transparent texel of a blend texture does not.
//     Avatar-owned blend faces (worn attachments, the agent's own, rigged / animesh) always
//     pass: avatars are not walls (the class mask cannot exclude them -- blend outranks the
//     avatar classes in maskClass).
//   * ALPHA-MASK (RT_MASK_ALPHAMASK, static only): the raster's cut-out rule (rtAlphaOpaque:
//     same cutoff, same texel); what renders blocks, what is discarded passes.
// Returns 0 = passes, 1 = alpha-mask blocker, 2 = alpha-blend blocker. ONE inlined
// rtCandidateAlpha per call site (compile cost: see RT_WALK_*).
// (Faces the TLAS leaves out never reach this: RenderVKRTBlendCull drops non-fullbright
// blend faces with face alpha < 0.05 at instance build, clean_rt_instances.comp passA.)
#define RT_MASK_WORLD_BLOCK (RT_MASK_OPAQUE | RT_MASK_TERRAIN | RT_MASK_ALPHAMASK | RT_MASK_BLEND)
#define RT_BLOCK_ALPHA_EPS  ALPHA_VISIBLE_EPS

// Worn attachment (512), the agent's own avatar (8192), rigged / animesh skin (bit 0 with a
// skin; a flexi chain carries bit 0 without one and is world geometry).
bool rtAvatarOwned(uint id)
{
    if (!rtIdIn(id)) return false;
    const uint f = rt_instances[id].flags;
    return (f & (512u | 8192u)) != 0u || ((f & 1u) != 0u && rt_instances[id].skin_id != 0xFFFFFFFFu);
}

uint rtWorldBlockCandidate(uint id, uint prim, vec2 bary, float t, vec3 dir, rayQueryEXT q)
{
    if (!rtIdIn(id)) return 1u;   // (#874 guard)
    const bool isBlend = (rt_instances[id].flags & 2u) != 0u;
    if (isBlend)
    {
        if (rtAvatarOwned(id)) return 0u;
        const uint mid = rt_instances[id].material_id;
        float fa = rt_instances[id].tint.a;
        if (mid < uint(rt_materials.length())) fa *= rt_materials[mid].base_color_factor.a;
        if (fa <= RT_BLOCK_ALPHA_EPS) return 0u;   // fully transparent face: no texture fetch
    }
    float cutoff; bool blend;
    const float a = rtCandidateAlpha(id, prim, bary, t, dir, q, cutoff, blend);
    if (isBlend) return a > RT_BLOCK_ALPHA_EPS ? 2u : 0u;
    return a >= cutoff ? 1u : 0u;
}

// Raster back-face rule for a candidate: single-sided (no flag 256) triangles are
// front-facing when counter-clockwise seen from the viewer in the right-handed world
// (the raster's frontFace CCW / gl_FrontFacing), i.e. the world-space geometric
// normal of the TRANSFORMED triangle faces the ray. Computed from the transformed
// edges, so mirrored (negative-determinant) instances flip exactly as they do on
// screen -- no dependence on the ray-tracing facing convention / FLIP_FACING bits.
bool rtCandidateFront(uint id, uint prim, vec3 dir, mat4x3 o2w)
{
    if (!rtIdIn(id) || (rt_instances[id].flags & 256u) != 0u) return true;   // double-sided (/ #874 guard)
    vec3 p0, p1, p2;
    rtTriPositions(id, prim, p0, p1, p2);
    mat3 M = mat3(o2w);
    return dot(cross(M * (p1 - p0), M * (p2 - p0)), dir) < 0.0;
}
// ... for the CANDIDATE q stands on (#1031: its transform read here, not passed)
bool rtCandidateFrontQ(uint id, uint prim, vec3 dir, rayQueryEXT q)
{
    if (!rtIdIn(id) || (rt_instances[id].flags & 256u) != 0u) return true;   // double-sided (/ #874 guard)
    vec3 p0, p1, p2;
    rtTriPositions(id, prim, p0, p1, p2);
    const mat3 M = mat3(rayQueryGetIntersectionObjectToWorldEXT(q, false));
    return dot(cross(M * (p1 - p0), M * (p2 - p0)), dir) < 0.0;
}

// ---- SHADOW-RAY WALKS (#1039 M0: the one walk library -- every visibility walk, its old names, the
//      own-fixture rule and the opaque-first pre-ray; the kinds RT_WALK_*) ----
#include "clean_vis_walk.glsl"

// Nearest hit with alpha-mask holes honoured. cull_back = the raster's back-face rule
// (primary / camera-like rays); shadow-style queries pass false (double-sided). With
// cull_back every triangle becomes a candidate (NoOpaque) so the facing test can run
// on opaque instances too -- costlier, meant for camera-ray consumers.
RtHit traceClosestHit(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, bool cull_back)
{
    rtRaySane(origin, tmin, dir, tmax);   // (invalid ray: the empty ray -- a miss)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, cull_back ? gl_RayFlagsNoOpaqueEXT : gl_RayFlagsNoneEXT,
                          mask & 0xFFu, origin, tmin, dir, tmax);
    const uint budget = cull_back ? RT_CANDIDATE_BUDGET_NOOPAQUE : RT_CANDIDATE_BUDGET;
    uint n_cand = 0u;
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            if (++n_cand > budget)
            {   // over budget (G-7): stop -- the nearest hit committed so far stands
                rayQueryTerminateEXT(rq);
                break;
            }
            uint id   = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                      uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
            uint prim = RT_HIT_PRIM(rq, false, id);
            vec2 bc   = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            float t   = rayQueryGetIntersectionTEXT(rq, false);
            if (cull_back && !rtCandidateFrontQ(id, prim, dir, rq)) continue;
            if (!rtIdIn(id) || (cull_back && rtInstanceOpaque(rt_instances[id].flags))
                || rtAlphaOpaque(id, prim, bc, t, dir, rq))
                rayQueryConfirmIntersectionEXT(rq);
        }
    }
    RtHit h;
    h.hit = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    h.dist = h.hit ? rayQueryGetIntersectionTEXT(rq, true) : tmax;
    h.instance = h.hit ? rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                        uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true))) : 0xFFFFFFFFu;
    // (#874 guard: a committed hit whose id failed the check is reported as a miss -- every
    // RtHit consumer indexes the instance tables with h.instance)
    if (h.hit && !rtIdIn(h.instance)) { h.hit = false; h.dist = tmax; h.instance = 0xFFFFFFFFu; }
    h.prim = h.hit ? RT_HIT_PRIM(rq, true, h.instance) : 0u;
    h.bary = h.hit ? rayQueryGetIntersectionBarycentricsEXT(rq, true) : vec2(0.0);
    h.front = h.hit && rayQueryGetIntersectionFrontFaceEXT(rq, true);
    h.o2w = h.hit ? rayQueryGetIntersectionObjectToWorldEXT(rq, true)
                  : mat4x3(vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0));
    return h;
}

// World-space geometric normal of a committed hit (the TLAS transform applied to
// the triangle's edges, so attachments / spinners match what was traced).
vec3 rtHitWorldNormal(RtHit h)
{
    vec3 p0, p1, p2;
    rtTriPositions(h.instance, h.prim, p0, p1, p2);
    mat3 M = mat3(h.o2w);
    vec3 n = cross(M * (p1 - p0), M * (p2 - p0));
    float l = length(n);
    return l > 1e-20 ? n / l : vec3(0.0, 0.0, 1.0);
}

#endif
