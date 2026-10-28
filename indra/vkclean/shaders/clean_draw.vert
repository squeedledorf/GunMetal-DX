#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_shader_draw_parameters : require   // gl_BaseVertexARB: the slot's decode header (#573)
#ifdef CLEAN_CLUSTER
// ---- CLUSTER LOD variant (#745, -DCLEAN_CLUSTER; OWNER 1b) ---------------------------------
// The cluster draws of the MDI raster tiers (llcleanclusterselectvk.cpp): one draw per
// selected cluster, firstInstance = its absolute selected index (gl_InstanceIndex). The
// per-vertex work is clean_cluster_draw_common.glsl's (shared with the mesh-shader tier).
//   tier 1 (default): uint8 indexed draw, the page pool bound as the vertex buffer (stride 20,
//     the static path's quantized attributes) and as the UINT8 index buffer; gl_BaseVertex =
//     the cluster's first vertex, its decode header at gl_BaseVertex - 2 (pool words).
//   tier 0 (-DCLEAN_CLUSTER_PULL): non-indexed draw, firstVertex = the cluster's first index
//     BYTE in the pool: gl_VertexIndex addresses the uint8 index, the vertex is pulled from
//     the pool words (no vertex input, no extension).
// The default compile (no CLEAN_CLUSTER) is the unchanged static VS below.
// DEPTH-ONLY (-DCLEAN_DEPTH_ONLY -DCLEAN_DEPTH_MASK, RenderVKDepthOnlyVS): the cluster depth
// passes (pre-pass, sun / projector casters, alpha-blend depth, translucency layer) all
// alpha-test, so the cluster tiers have the MASK flavour only: gl_Position + locations
// 0 / 1 / 3 / 4, from clusterVertex's same position chain (clean_cluster_draw_common.glsl).
#include "clean_cluster_draw_common.glsl"
#ifndef CLEAN_CLUSTER_PULL
layout(location = 0) in ivec4 a_qpos;
layout(location = 1) in ivec2 a_quv;
layout(location = 2) in vec2  a_onrm;
layout(location = 3) in vec2  a_otan;
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
layout(location = 0) flat out uint v_material_id;
layout(location = 1) invariant out vec2 v_uv;   // (invariant: as the static VS)
layout(location = 3) flat out vec4 v_tint;
layout(location = 4) flat out uint v_flags;
// <GMDX> rez effects (gmdx_rezfx.glsl): no effect data on the cluster path (w = 0: off)
layout(location = 16) invariant out vec4 v_rez;   // <GMDX> 16 / 17 (15 is #901's v_wx_rest)
layout(location = 17) invariant flat out vec4 v_rez_box;
#endif
// (declaration order as before the slim variants: the full variant's module is unchanged)
#ifdef CLEAN_VS_NORMAL_FRAME
layout(location = 2)      out vec3 v_world_normal;
#endif
#ifdef CLEAN_VS_WORLD_POS
layout(location = 5)      out vec3 v_world_pos;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
layout(location = 7)      out vec4 v_world_tangent;
layout(location = 8)      out vec2 v_uv_raw;
#endif
#ifndef CLEAN_DEPTH_ONLY
layout(location = 9) flat out uint v_terrain_rec;
layout(location = 11) flat out uint v_cel;
layout(location = 15)      out vec4 v_wx_rest;   // #901 rest-pose accumulation: the skinned vertex's bind-space position (ClVOut.rest)
#endif
#ifdef CLEAN_VS_PREV_POS
layout(location = 10)      out vec3 v_prev_world_pos;   // motion pre-pass (#749)
#endif
#ifdef CLEAN_PICK
// GPU CURSOR PICK of cluster-drawn objects (#702, -DCLEAN_PICK with the tier-0 pull variant):
// the outputs clean_pick.frag reads (= the static / rigged VS's).
layout(location = 12) flat out uint v_inst;
layout(location = 13)      out vec2 v_pick_st;
layout(location = 14)      out vec4 v_pick_tangent;
#endif
// #749: the cluster draws now run in the depth PRE-PASS too (other pipelines, same module):
// invariant keeps the G-buffer draw's depth bit-identical to the pre-pass's (as the static VS).
// (+ precise, #1088: as the static VS below.)
invariant gl_Position;
precise gl_Position;

void main()
{
    const uint sel = uint(gl_InstanceIndex);
    // F8 (alpha lists' cull-FRONT draw, push f8 = 1): a single-sided instance's back faces would
    // only be discarded by the FS -- collapse (as clean_draw_rigged.vert; the mesh tier drops the
    // cluster in its task stage).
    if (g_draw_push.f8 == 1u && (cl_instances[cl_sel[sel].instance].flags & CLI1B_DOUBLE_SIDED) == 0u)
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        return;
    }
    vec3 position;
    vec2 texcoord0;
    vec3 normal;
    vec4 tangent;
    uint local;                                           // index in its cluster (2a skinned arena)
#ifdef CLEAN_CLUSTER_PULL
    const ClSelected cs = cl_sel[sel];
    const uint slot = clRefSlot(cs.cluster_ref);
    const uint ib = uint(gl_VertexIndex);                 // absolute index byte
    local = (cl_pool[ib >> 2u] >> ((ib & 3u) * 8u)) & 0xFFu;
    const uint first = slot * CL_PAGE_VERTS + cl_pool[clRecordWord(slot, clRefIndex(cs.cluster_ref)) + 8u];   // vert_off20
    clPoolVertex(first, first + local, position, texcoord0, normal, tangent);
#else
    {
        const QvHeader qh = QV_LOAD_HEADER(cl_pool, uint(max(gl_BaseVertexARB, int(QV_HEADER_VERTS))));
        position  = qh.pos_c + vec3(a_qpos.xyz) * qh.pos_s;
        texcoord0 = qh.uv_c + vec2(a_quv) * qh.uv_s;
        normal    = qvOctDecode(a_onrm);
        tangent   = ((a_qpos.w & 1) != 0) ? vec4(qvOctDecode(a_otan), (a_qpos.w & 2) != 0 ? -1.0 : 1.0) : vec4(0.0);
        local     = uint(gl_VertexIndex - gl_BaseVertexARB);   // vertexOffset = the cluster's first vertex
    }
#endif
    const ClVOut o = clusterVertex(sel, local, position, texcoord0, normal, tangent);
    gl_Position     = o.pos;
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    v_material_id   = o.material_id;
    v_uv            = o.uv;
    v_tint          = o.tint;
    v_flags         = o.flags;
    v_rez           = vec4(0.0);   // <GMDX>
    v_rez_box       = vec4(0.0);   // <GMDX>
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    v_world_normal  = o.world_normal;
#endif
#ifdef CLEAN_VS_WORLD_POS
    v_world_pos     = o.world_pos;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    v_world_tangent = o.world_tangent;
    v_uv_raw        = o.uv_raw;
#endif
#ifndef CLEAN_DEPTH_ONLY
    v_terrain_rec   = o.terrain_rec;
    v_cel           = o.cel;
    v_wx_rest       = o.rest;
#endif
#ifdef CLEAN_VS_PREV_POS
    v_prev_world_pos = o.prev_world_pos;
#endif
#ifdef CLEAN_PICK
    v_inst = o.inst;
    v_pick_st = o.pick_st;
    v_pick_tangent = o.pick_tangent;
#endif
}
#else   // !CLEAN_CLUSTER: the static world VS
// vkclean — GPU-driven draw vertex shader (static path only).
//
// Transliterated from the STATIC branch of cleanGPUV.glsl. The compute cull
// wrote each surviving instance id into VkDrawIndexedIndirectCommand.firstInstance,
// so gl_InstanceIndex (== firstInstance for a 1-instance draw) indexes the
// Instance SSBO — the Vulkan analog of GL's gl_BaseInstanceARB. Position/UV/normal
// come from the mega-VBO vertex stream (here: the shared proto geometry, drawn
// with the slot's firstIndex/vertexOffset via the indirect command).
//
// Rigid ATTACHMENTS (flag bit9 = 512) are handled here like cleanGPUV's attach
// branch; rigged geometry uses clean_draw_rigged.vert (pre-skinned verts).
//
// DEPTH-ONLY VARIANTS (RenderVKDepthOnlyVS; -DCLEAN_DEPTH_ONLY, + -DCLEAN_DEPTH_MASK):
// the depth passes (sun cascades, projector tiles, the depth pre-pass, the alpha-blend
// depth pass, the translucency layer) ran this whole VS -- normal cofactor, tangent frame,
// previous-frame pose, encloses-camera box, cel / pick words -- for a gl_Position and, at
// most, the alpha test's inputs: 119 registers where ~30 do (Pascal: ~25% occupancy in
// vertex-bound shadow passes). CLEAN_DEPTH_ONLY keeps ONLY the position chain, compiled
// from the very same source lines below (decode, f8 collapse, attachment / spin pose,
// terrain geomorph, camera-relative projection), so with `invariant gl_Position` the
// pre-pass depth is bit-identical to the EQUAL pass's. CLEAN_DEPTH_MASK adds exactly the
// locations the alpha-test FSs read (clean_prepass_mask / _blend, clean_shadow_mask,
// clean_shadow_trans: 0 material, 1 uv, 3 tint, 4 flags) from the same expressions.
// SLIM PRE-PASS VARIANTS (on top of CLEAN_DEPTH_ONLY + CLEAN_DEPTH_MASK; RenderVKDepthOnlyVS):
// -DCLEAN_DEPTH_MOTION adds 5 world position + 10 previous world position (the motion
// pre-pass FSs, clean_prepass_motion.frag / clean_prepass_normal.frag PREPASS_MOTION: the
// previous instance row / joint table / spin time chain below, verbatim); -DCLEAN_DEPTH_NORMAL
// adds what clean_prepass_normal.frag reads: 2 world normal, 5 world position, 7 world
// tangent, 8 raw UV. Neither computes cel / pick / terrain record / encloses-camera.
// SLIM G-BUFFER VARIANT (-DCLEAN_GBUFFER_VS, RenderVKGbufSlimVS; not with CLEAN_DEPTH_ONLY):
// exactly what the deferred G-buffer FS reads (clean_draw.frag CLEAN_GBUFFER: 0 1 2 3 4 5 7 8 9
// 11, + v_flags bit 14) -- no previous-frame pose (10: second instance row, previous joint
// table, second spin pose) and no pick outputs (12 13 14). gl_Position from the same source
// lines as every other variant (invariant), so the G-buffer's EQUAL test still matches the
// depth pre-pass bit for bit.

// SLIM G-BUFFER VS REGISTER PROBES (#821, RenderVKGbufRegProbe; measurement only): the slim
// G-buffer VS with ONE of its per-vertex features compiled out per variant, built (never drawn,
// unless RenderVKGbufProbeDraw 11..17 asks) so RenderVKPipelineStats logs what each holds in
// registers ("gbufVsProbe ..." VKPipeStats lines; the slim VS measured 94-95 vs the depth-only
// VS's 54). The variants render the dropped feature WRONG:
//   CLEAN_VSPROBE_NO_TEXANIM   v_uv / v_uv_raw = the raw texcoord (no texture animation, planar
//                              texgen, uv_matrix: cleanFaceUV / cleanFaceRawUV)
//   CLEAN_VSPROBE_NO_TANGENT   no world tangent (the normal map's own UV matrix fetch +
//                              cleanFaceTangent)
//   CLEAN_VSPROBE_NO_ENCLOSE   no encloses-camera box test (v_flags bit 14)
//   CLEAN_VSPROBE_NO_REST      no #901 rest-pose accumulation output (v_wx_rest)
//   CLEAN_VSPROBE_NO_CEL       no cel / avatar-mask word (v_cel)
//   CLEAN_VSPROBE_NO_POSE      no attachment joint pose / GPU spin pose (M = the model matrix)
// What each variant computes (full VS: everything).
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MOTION) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_WORLD_POS      // 5 world position
#endif
#if (!defined(CLEAN_DEPTH_ONLY) && !defined(CLEAN_GBUFFER_VS)) || defined(CLEAN_DEPTH_MOTION)
#define CLEAN_VS_PREV_POS       // 10 previous world position (+ bindings 23 / 25)
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_NORMAL_FRAME   // 2 world normal, 7 world tangent, 8 raw UV
#endif
#if !defined(CLEAN_DEPTH_ONLY) && !defined(CLEAN_GBUFFER_VS)
#define CLEAN_VS_PICK           // 12 instance, 13 pick ST, 14 pick tangent
#endif

// QUANTIZED vertex stream (#573, clean_qvertex.glsl): lattice position + flags, lattice
// UV, octahedral normal / tangent. Decoded at the top of main() with the slot's header,
// read through binding 39 at gl_BaseVertex (= the draw's vertexOffset = the slot's first
// real vertex; the header is the 2 pseudo-vertices before it).
layout(location = 0) in ivec4 a_qpos;    // xyz lattice, w = flags (1 tangent, 2 negative handedness)
layout(location = 1) in ivec2 a_quv;
layout(location = 2) in vec2  a_onrm;    // octahedral snorm16
layout(location = 3) in vec2  a_otan;    // octahedral snorm16
layout(std430, set = 0, binding = 39) readonly buffer QvWords { uint qv_words[]; };
#include "clean_qvertex.glsl"
// The decoded attributes (object space; tangent w = handedness, zero = none).
vec3 position;
vec2 texcoord0;
vec3 normal;
vec4 tangent;

struct Instance {
    mat4 model_matrix;
    vec4 aabb_min;
    vec4 aabb_max;
    vec4 tint;
    mat4 uv_matrix;
    uint lod_group_id;
    uint material_id;
    uint flags;
    uint skin_id;
    vec4 spin_axis;     // xyz world axis*rate, w t0 (GPU target omega)
    vec4 spin_pivot;    // xyz agent-space pivot
};
layout(std430, set = 0, binding = 0) readonly buffer Instances { Instance instances[]; };
// #1033 SLIM RECORDS (clean_vs_inst.glsl; clean_vs_inst.comp derives them every frame): the
// per-instance reads below go through the 112 B record; the full row is opened only for
// the rare fields (terrain / enclose-test AABB, spin axis + pivot, GPU texture animation).
#include "clean_vs_inst.glsl"
layout(std430, set = 0, binding = 54) readonly buffer VsInstances { VsInst vs_inst[]; };
// TEMPORAL (#84): every instance as of the PREVIOUS frame (same layout, kept by GPU
// copies of the changed runs; see LLCleanRendererVK::recordPrevInstances). Only the
// motion pre-pass consumes what is derived from it.
#ifdef CLEAN_VS_PREV_POS
layout(std430, set = 0, binding = 23) readonly buffer PrevInstances { Instance prev_instances[]; };
layout(std430, set = 0, binding = 55) readonly buffer PrevVsInstances { VsInst prev_vs_inst[]; };   // (#1033)
#endif

// ATTACHMENT SKINNING (flag bit9 = 512). A statically-worn unrigged attachment
// rides rigidly on one avatar joint: model_matrix holds the CONSTANT offset
// relative to that joint (ownedWorld * inverse(jointWorld), baked at ingest), the
// joint index is packed in aabb_max.w, and skin_id is the avatar's joint-table
// slot. The live pose comes from this per-frame table of joint WORLD matrices.
#include "clean_joint_table.glsl"   // mat3x4 joint world rows (jointWorld())
layout(std430, set = 0, binding = 2) readonly buffer AvatarJointsSSBO { AvatarJoints avatar_joints[]; };
// TEMPORAL (#84, stage 2): the joint table as of the PREVIOUS frame (worn attachments'
// motion vectors; slots with no history hold this frame's rows).
#ifdef CLEAN_VS_PREV_POS
layout(std430, set = 0, binding = 25) readonly buffer PrevAvatarJointsSSBO { AvatarJoints prev_avatar_joints[]; };
#endif

// The draw's view block (pc.u_*) + the 16 B push (view index, F8 mode); #238.
#include "clean_draw_view.glsl"

#include "clean_uv_common.glsl"
#include "clean_frame_info.glsl"   // ssr.taa_params.x: previous frame's time (motion vectors)
#include "clean_terrain_lod.glsl"   // smooth terrain geomorph (#211)
#ifdef CLEAN_VS_TERRAIN_SIDE
#define CLEAN_TERRAIN_BAKE_TABLE_ONLY
#include "clean_terrain_bake.glsl"  // terrain bake table (#811): the live / bake split below
#endif
#ifndef CLEAN_DEPTH_ONLY
#define CEL_VERTEX
#include "clean_cel_common.glsl"    // avatar slot + cel scope (#549 / #550): v_cel
// #901 REST-POSE ACCUMULATION (clean_weather_accum.glsl; draw set 0 binding 50): the rest skeleton
#define WX_AC_SET 0
#define WX_AC_BINDING 50
#include "clean_weather_accum.glsl"
#endif
#define GMDX_REZFX_VS
#include "gmdx_rezfx.glsl"          // <GMDX> rez effects: de-rez vertex effects, v_rez / v_rez_box

#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
// v_uv is INVARIANT too (every variant): the mask pre-pass's alpha test and the EQUAL lit
// FS's must sample the same texel, and they now run different VS modules (depth-only VS).
layout(location = 0) flat out uint v_material_id;
layout(location = 1) invariant out vec2 v_uv;
layout(location = 3) flat out vec4 v_tint;
layout(location = 4) flat out uint v_flags;      // instance flags (bit 32 = terrain splat)
// <GMDX> rez effects (gmdx_rezfx.glsl rezfxOutputs): invariant, as v_uv -- the de-rez cut of
// the mask pre-pass and of the EQUAL lit FS must agree to the bit.
layout(location = 16) invariant out vec4 v_rez;   // <GMDX> 16 / 17 (15 is #901's v_wx_rest)
layout(location = 17) invariant flat out vec4 v_rez_box;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
layout(location = 2)      out vec3 v_world_normal;
layout(location = 7)      out vec4 v_world_tangent; // (6 is the particle glow slot)
layout(location = 8)      out vec2 v_uv_raw;        // pre-transform texcoord (per-map UV transforms)
                                                    // terrain: region-local XY (detail / PBR UVs)
#endif
#ifdef CLEAN_VS_WORLD_POS
layout(location = 5)      out vec3 v_world_pos;   // world XY drives the terrain detail UV
#endif
#ifdef CLEAN_VS_PREV_POS
// Where this vertex was LAST frame (world space), for the motion-vector pre-pass
// (clean_prepass_normal.frag PREPASS_MOTION, clean_prepass_motion.frag). No other FS reads
// it, so every other pipeline drops the output and the prev-instance reads with it.
layout(location = 10)     out vec3 v_prev_world_pos;
#endif
#ifndef CLEAN_DEPTH_ONLY
layout(location = 9) flat out uint v_terrain_rec;   // terrain: TerrainRecord index (inst.skin_id)
// AVATAR MASK / CEL (#549 / #550, clean_cel_common.glsl): avatar slot + 1 in bits 0..15,
// bit 16 = toon lit. Read by clean_draw.frag and the avatar-id FS; dropped elsewhere.
layout(location = 11) flat out uint v_cel;
// #901 REST-POSE ACCUMULATION (clean_weather_surface.glsl weatherAvatarAccum): a WORN rigid
// attachment's vertex in the wearer's REST pose (its joint's rest transform x the constant joint
// offset x the object-local position), w its rest normal's z; w -2 = none (every other draw).
layout(location = 15)      out vec4 v_wx_rest;
#endif   // !CLEAN_DEPTH_ONLY
#ifdef CLEAN_VS_PICK
// GPU CURSOR PICK (#702, clean_pick.frag): the instance this vertex belongs to. No other FS
// reads it, so every other pipeline drops the output.
layout(location = 12) flat out uint v_inst;
// ... and its surface as LL's pick reports it (#704): texcoord0 before texgen (LL's surface
// ST) and the vertex stream's tangent (world, w = handedness; 0 = the mesh has none).
layout(location = 13)      out vec2 v_pick_st;
layout(location = 14)      out vec4 v_pick_tangent;
#endif   // CLEAN_VS_PICK
// DEPTH PRE-PASS invariance: the depth pre-pass pipelines run this source (the
// CLEAN_DEPTH_ONLY module, or this one with no FS / alpha-test FS) and pass 1 then
// depth-tests EQUAL. Without `invariant` the driver may compile gl_Position differently
// per pipeline / module (varying DCE, FMA contraction) and an ULP of difference punches
// z-fighting holes. Every variant computes it through the same source lines.
// #1088: + `precise` (NoContraction on every operation feeding the position): `invariant`
// alone did not hold on an AMD Windows driver (Vega iGPU) across the pre-pass and shading
// pipelines -- thin unlit lines along triangle edges. With no FMA contraction
// left to the backend, every module evaluates the same exact IEEE operations in the same order.
invariant gl_Position;
precise gl_Position;

// One axis of the ENCLOSES-CAMERA oriented-box test: |d . axis| within the half extent
// (axis = an un-normalized model column, so both sides carry len^2). Called per axis, NOT
// from a loop over M[i]: a dynamically indexed M kept the pose matrix in function memory,
// so the position chain compiled differently here than in the depth-only VS.
bool boxAxisInside(vec3 d, vec3 axis, float h)
{
    const float len = length(axis);
    return !(len > 0.0 && abs(dot(d, axis)) > h * len * len);
}

// GPU TARGET OMEGA: model_matrix is the un-spun pose; rotate it about the pivot by
// rate*(t - t0) around the world axis (Rodrigues, right-handed -- matches LL's
// rot * dQ with dQ = quat(angle, axis)).
mat4 spinPose(mat4 M, vec4 spin_axis, vec4 spin_pivot, float time)
{
    // precise (#1088): the pose feeds gl_Position, and `precise gl_Position` does not reach
    // into called functions (sqrt(dot) rather than length() so the decoration covers the dot).
    precise float rate  = sqrt(dot(spin_axis.xyz, spin_axis.xyz));
    vec3  a     = spin_axis.xyz / rate;
    precise float ang   = mod(rate * (time - spin_axis.w), 6.28318531);
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    precise mat3 R = mat3(t*a.x*a.x + c,     t*a.x*a.y + s*a.z, t*a.x*a.z - s*a.y,
                  t*a.x*a.y - s*a.z, t*a.y*a.y + c,     t*a.y*a.z + s*a.x,
                  t*a.x*a.z + s*a.y, t*a.y*a.z - s*a.x, t*a.z*a.z + c);
    vec3 pv = spin_pivot.xyz;
    precise mat4 P = mat4(vec4(R * M[0].xyz, 0.0), vec4(R * M[1].xyz, 0.0), vec4(R * M[2].xyz, 0.0),
                          vec4(pv + R * (M[3].xyz - pv), 1.0));
    return P;
}

void main()
{
    {
        // Exact lattice decode (power-of-two steps): every pipeline running this module
        // (pre-pass / EQUAL twins) reconstructs bit-identical positions.
        const QvHeader qh = QV_LOAD_HEADER(qv_words, uint(max(gl_BaseVertexARB, int(QV_HEADER_VERTS))));
        position  = qh.pos_c + vec3(a_qpos.xyz) * qh.pos_s;
        texcoord0 = qh.uv_c + vec2(a_quv) * qh.uv_s;   // (all variants: the terrain geomorph reads .y)
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
        normal    = qvOctDecode(a_onrm);                  // (planar texgen)
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
        tangent   = ((a_qpos.w & 1) != 0) ? vec4(qvOctDecode(a_otan), (a_qpos.w & 2) != 0 ? -1.0 : 1.0) : vec4(0.0);
#endif
    }
    const VsInst inst = vs_inst[gl_InstanceIndex];
    const mat4 inst_model = vsiModel(inst);

#ifdef CLEAN_VS_SKIP_TERRAIN
    // G-BUFFER TERRAIN SPLIT (-DCLEAN_VS_SKIP_TERRAIN, RenderVKGbufTerrainSplit): the deferred
    // G-buffer's terrain-free static pipelines draw buckets 0 1 6 7 as they are; terrain
    // (bit5) is drawn from the main cull's terrain list by its own pipeline, so here its
    // draws collapse (degenerate + outside the depth range, as F8 below: nothing rasterizes).
    if ((inst.flags & 32u) != 0u)
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        return;
    }
#endif
#ifdef CLEAN_VS_TERRAIN_SIDE
    // TERRAIN BAKE SPLIT (#811, -DCLEAN_VS_TERRAIN_SIDE=1 / 2; clean_terrain_bake.glsl): the
    // G-buffer draws the main cull's terrain list twice. Side 1 (the live terrain FS, with the
    // crossfade) drops the patches whose whole box lies past the bake's full-weight distance
    // of a ready bake; side 2 (the slim bake-only FS) drops every other draw. Per instance
    // (the same box distance the cull's terrain LOD uses), so the two sides partition the
    // list exactly; collapsed as F8 below (nothing rasterizes).
    {
        const bool bakeFar = (inst.flags & 32u) != 0u
            && terrBakeFar(inst.skin_id, terrainBoxDist(inst_model, instances[gl_InstanceIndex].aabb_min.xyz,
                                                        instances[gl_InstanceIndex].aabb_max.xyz, ssr.terrain_lod.xyz));
        if (bakeFar == (CLEAN_VS_TERRAIN_SIDE == 1))
        {
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
            return;
        }
    }
#endif

    // ALPHA-BLEND DRAW PAIR (F8): the main view draws each alpha list twice, cull FRONT
    // (back faces) then cull BACK (front faces). The push's f8 word (0 otherwise)
    // marks them: 1 = the cull-FRONT draw, where a single-sided face's back side would
    // only be discarded by the FS (clean_draw.frag's back-face test) -> no fragments;
    // 1 and 2 (the cull-BACK draw): water, which the alpha FS always discards (the water
    // pipe draws it). Degenerate + outside the depth range: nothing rasterizes.
    // 3 (F2, the planar mirror's water-cap draw over its unsorted alpha bucket): the
    // inverse -- everything that is NOT water, which the water FS would only discard.
    if (g_draw_push.f8 != 0u)
    {
        const bool single = (inst.flags & (256u | 8u)) == 0u;
        const bool water  = (inst.flags & 2048u) != 0u;
        if (g_draw_push.f8 == 3u ? !water : ((single && g_draw_push.f8 == 1u) || water))
        {
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
            return;
        }
    }

    mat4 M = inst_model;
#ifdef CLEAN_VSPROBE_NO_POSE
    const bool attached = false;   // (#821 register probe)
#else
    const bool attached = (inst.flags & 512u) != 0u && inst.skin_id != 0xFFFFFFFFu
                          && inst.skin_id < uint(avatar_joints.length());
#endif
    if (attached)
    {
        uint bone = min(vsiBone(inst), 215u);
        M = jointWorld(avatar_joints[inst.skin_id].world[bone]) * inst_model;
    }
#ifndef CLEAN_VSPROBE_NO_POSE
    else if ((inst.aux & VSI_SPIN) != 0u)
    {
        M = spinPose(M, instances[gl_InstanceIndex].spin_axis, instances[gl_InstanceIndex].spin_pivot, pc.u_cam_time.w);
    }
#endif

    // SMOOTH TERRAIN GEOMORPH (#211, clean_terrain_lod.glsl): a vertex a coarser level
    // lacks slides onto that level's surface (texcoord0.y = the height step) as the MAIN
    // camera's distance approaches the level switch. Faceted terrain and every other
    // instance: lpos == position.
    vec3 lpos = position;
    if ((inst.flags & 32u) != 0u && texcoord0.y != 0.0 && ssr.terrain_lod.w > 0.5)
    {
        // (precise, #1088: the morph feeds gl_Position; a call's arguments are outside the
        // reach of `precise gl_Position`. sqrt(dot) so the decoration covers the dot.)
        precise const vec3 mdv = (M * vec4(position, 1.0)).xyz - ssr.terrain_lod.xyz;
        precise const float md = sqrt(dot(mdv, mdv));
        lpos.z += texcoord0.y * terrainMorph(position.xy, md);
    }
    lpos = rezfxDisplace(inst.flags, lpos, M);   // <GMDX> de-rez vertex effects (every variant)
#ifdef CLEAN_VS_WORLD_POS
    vec4 world_pos = M * vec4(lpos, 1.0);
#endif
    // CAMERA-RELATIVE PROJECTION (#555): the translation is differenced with the view's
    // origin first (two ~km agent-space values: exact), so the projection never cancels
    // km-sized terms per vertex -- at Z 3000 that wobbled vertices ~1.4 px at 1 m every
    // frame. u_origin = 0 (world-space u_view_proj) is the old product.
    vec3 rel_pos = mat3(M) * lpos + (M[3].xyz - pc.u_origin.xyz);
    // <GMDX> ALPHA-MASK PULL (GMDXAlphaMaskPull*, ssr.gmdx_misc): an alpha-masked world surface (flag
    // bit2, not HUD) moves a few millimetres toward the camera, more at grazing angles, so a path or
    // decal mesh lying just over the ground stays on top: the 16-bit vertex lattice (#573) rounds both
    // surfaces by up to ~1-4 mm, which closed or crossed that gap (GL keeps exact floats). Every
    // variant runs the same lines, so the pre-pass / EQUAL twins still match.
    if ((inst.flags & 12u) == 4u && ssr.gmdx_misc.x > 0.0)
    {
        const vec3 to_cam = (pc.u_cam_time.xyz - pc.u_origin.xyz) - rel_pos;
        const float cam_d = length(to_cam);
        if (cam_d > 0.01)
        {
            float pull = ssr.gmdx_misc.x + ssr.gmdx_misc.y * cam_d;
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
            const vec3 nw = normalize(mat3(M) * normal);
            pull *= min(1.0 / max(abs(dot(nw, to_cam)) / cam_d, 1e-3), ssr.gmdx_misc.z);
#endif
            rel_pos += to_cam * (min(pull, 0.25 * cam_d) / cam_d);
        }
    }
    gl_Position    = pc.u_view_proj * vec4(rel_pos, 1.0);

#ifdef CLEAN_VS_PREV_POS
    // Previous-frame position: the previous instance row (and, for GPU spinners, the
    // previous frame's time; for worn attachments, the previous joint table at the
    // previous row's slot / bone). An attachment whose previous row was not worn (just
    // attached) keeps this frame's pose: camera-only motion.
    v_prev_world_pos = world_pos.xyz;
    {
        const VsInst pinst = prev_vs_inst[gl_InstanceIndex];
        const mat4 pinst_model = vsiModel(pinst);
        if (attached)
        {
            if ((pinst.flags & 512u) != 0u && pinst.skin_id != 0xFFFFFFFFu
                && pinst.skin_id < uint(prev_avatar_joints.length()))
            {
                const uint pbone = min(vsiBone(pinst), 215u);
                // Same association as this frame's M (static pose -> exactly zero motion).
                const mat4 PM = jointWorld(prev_avatar_joints[pinst.skin_id].world[pbone]) * pinst_model;
                v_prev_world_pos = (PM * vec4(lpos, 1.0)).xyz;
            }
        }
        else
        {
            mat4 PM = pinst_model;
            if ((pinst.aux & VSI_SPIN) != 0u)
                PM = spinPose(PM, prev_instances[gl_InstanceIndex].spin_axis,
                              prev_instances[gl_InstanceIndex].spin_pivot, ssr.taa_params.x);
            v_prev_world_pos = (PM * vec4(lpos, 1.0)).xyz;   // terrain: this frame's morph
        }
    }
#endif   // CLEAN_VS_PREV_POS

#ifdef CLEAN_VS_NORMAL_FRAME
    // Normals via the normal matrix (inverse transpose, as LL's normal_matrix): the
    // cofactor of mat3(M) scaled by sign(det) -- mat3(M) alone skewed normals on
    // non-uniformly scaled objects.
    const mat3 M3   = mat3(M);
    const float det = dot(M3[0], cross(M3[1], M3[2]));
    const mat3 Ncof = mat3(cross(M3[1], M3[2]), cross(M3[2], M3[0]), cross(M3[0], M3[1]));
    v_world_normal  = (det < 0.0 ? -1.0 : 1.0) * (Ncof * normal);
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    // (depth variants: the alpha test's inputs, the same expressions)
#ifdef CLEAN_VSPROBE_NO_TEXANIM
    v_uv           = texcoord0;   // (#821 register probe)
#else
    v_uv           = cleanFaceUV(inst.flags,
                                 (inst.flags & 1024u) != 0u ? instances[gl_InstanceIndex].uv_matrix : vsiUv(inst),
                                 texcoord0, normal, position, inst_model, pc.u_cam_time.w);
#endif
    v_material_id  = inst.material_id;
    v_tint         = inst.tint;
    v_flags        = inst.flags;   // (depth variants: no bit 14 "encloses camera": no pre-pass FS reads it)
    rezfxOutputs(inst.flags, position, M, v_rez, v_rez_box);   // <GMDX>
#endif
#ifndef CLEAN_DEPTH_ONLY
#ifdef CLEAN_VSPROBE_NO_CEL
    v_cel          = 0u;   // (#821 register probe)
#else
    v_cel          = celVertexWord(inst.flags, inst.skin_id, false, attached);
#endif
#ifdef CLEAN_VS_PICK
    v_inst         = uint(gl_InstanceIndex);
#endif
    // ENCLOSES CAMERA (VS-derived v_flags bit14, not an instance flag): the camera is
    // inside the instance's box -- a weather dome / skybox the viewer sits in. The FS
    // skips the far-clip distance fade for it: that fade hides the draw-distance CULL
    // pop, which an object enclosing the camera never has (it always passes the cull),
    // and fading it by fragment depth replaced a ~750 m dome entirely with the sky. LL
    // draws such objects unfaded out to 1024 m.
    // Oriented-box test (#158), not the bounding sphere: a flat 1 km sim surround or a
    // terrain patch has a ~700 m sphere, so flying anywhere above the sim counted as
    // "inside" and those drew unfaded, sharp against the hazed scene.
#ifndef CLEAN_VSPROBE_NO_ENCLOSE
    if ((inst.flags & (512u | 8u)) == 0u)
#else
    if (false)   // (#821 register probe)
#endif
    {
        // Camera outside the box's bounding sphere (bound_r x the Frobenius norm of mat3(M)
        // bounds every box point's reach from M[3]): not inside, and the full row's AABB
        // stays unread. NaN fails the compare and takes the exact test.
        const float reach = inst.bound_r * sqrt(dot(M[0].xyz, M[0].xyz) + dot(M[1].xyz, M[1].xyz)
                                                + dot(M[2].xyz, M[2].xyz)) * 1.001 + 0.01;
        if (!(distance(pc.u_cam_time.xyz, M[3].xyz) > reach))
        {
            const vec3 hc = 0.5 * (instances[gl_InstanceIndex].aabb_min.xyz + instances[gl_InstanceIndex].aabb_max.xyz);
            const vec3 he = 0.5 * abs(instances[gl_InstanceIndex].aabb_max.xyz - instances[gl_InstanceIndex].aabb_min.xyz);
            const vec3 d  = pc.u_cam_time.xyz - (M * vec4(hc, 1.0)).xyz;
            const bool inside = boxAxisInside(d, M[0].xyz, he.x) && boxAxisInside(d, M[1].xyz, he.y)
                             && boxAxisInside(d, M[2].xyz, he.z);
            if (inside) v_flags |= 16384u;
        }
    }
#endif
#ifdef CLEAN_VS_WORLD_POS
    v_world_pos    = world_pos.xyz;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
#ifdef CLEAN_VSPROBE_NO_TEXANIM
    v_uv_raw       = texcoord0;   // (#821 register probe)
#else
    v_uv_raw       = cleanFaceRawUV(inst.flags, texcoord0, normal, position, inst_model);
#endif
    // TERRAIN (bit5): the patch mesh is region-local (model_matrix = region render
    // matrix), so position.xy is LL's terrain UV basis -- legacy detail texgen and the
    // PBR KHR transforms both start from it. skin_id carries the region's record.
    const bool terrain = (inst.flags & 32u) != 0u;
    if (terrain) v_uv_raw = position.xy;
#ifndef CLEAN_DEPTH_ONLY
    // (#783 M2: non-terrain draws carry their instance slot here -- the moving objects' own
    // snow, clean_weather_surface.glsl; the FS reads terrain records only under the terrain flag)
    v_terrain_rec  = terrain ? inst.skin_id : uint(gl_InstanceIndex);
    // #901 REST-POSE ACCUMULATION (ssr.weather[14].w bit 8: the grids live; the rest skeleton known)
    v_wx_rest = vec4(0.0, 0.0, 0.0, -2.0);
#ifdef CLEAN_VSPROBE_NO_REST
    if (false)   // (#821 register probe)
#else
    if (attached && (uint(ssr.weather[14].w + 0.5) & 8u) != 0u && (ac_head.w & 1u) != 0u)
#endif
    {
        const mat4 RJ = wxAcRestJoint(min(vsiBone(inst), 215u));
        const vec3 rp = (RJ * (inst_model * vec4(position, 1.0))).xyz;
        const mat3 O3 = mat3(inst_model);
        const vec3 rn = mat3(RJ) * (wxAcCofactor(O3) * normal) * (determinant(O3) < 0.0 ? -1.0 : 1.0);
        if (!any(isnan(rp)) && !any(isinf(rp)) && dot(rn, rn) > 1e-20) v_wx_rest = vec4(rp, normalize(rn).z);
    }
#endif
    // The normal map's own transform (if any) defines its tangent frame (LL
    // tangent_space_transform uses texture_normal_transform).
#ifdef CLEAN_VSPROBE_NO_TANGENT
    v_world_tangent = vec4(0.0);   // (#821 register probe)
#else
    const mat4 nuvm = matMapOwnUV(inst.material_id, MAP_NORMAL) ? matMapUVMatrix(inst.material_id, MAP_NORMAL)
                                                                  : vsiUv(inst);
    const vec4 tl  = cleanFaceTangent(inst.flags, nuvm, normal, tangent);
    // Unit-length world tangent (LL normalizes normal_matrix * tangent): mat3(M)
    // carries the object SCALE, and an unnormalized tangent (x20 on a stretched
    // road prim) swamped N in the normal-map basis -- opposite tilts on mirrored-UV
    // halves with a hard seam. A mirroring transform flips the handedness.
    const vec3 wt   = M3 * tl.xyz;
    v_world_tangent = (dot(wt, wt) > 1e-16) ? vec4(normalize(wt), det < 0.0 ? -tl.w : tl.w) : vec4(0.0);
#endif
#endif
#ifdef CLEAN_VS_PICK
    // Pick (#704): LL's volume tangent through the object's transform, its w kept (as the
    // CPU pick's volumeDirectionToAgent did).
    v_pick_st = texcoord0;
    const vec3 pt = M3 * tangent.xyz;
    v_pick_tangent = (dot(pt, pt) > 1e-16) ? vec4(normalize(pt), tangent.w) : vec4(0.0);
#endif   // CLEAN_VS_PICK
}
#endif   // CLEAN_CLUSTER
