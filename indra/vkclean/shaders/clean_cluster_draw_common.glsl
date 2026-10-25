// vkclean -- CLUSTER LOD draw: the per-vertex work shared by the cluster vertex shader
// (clean_draw.vert -DCLEAN_CLUSTER, raster tiers 0 / 1) and the cluster mesh shader
// (clean_cluster_draw.mesh, tier 2) (#745, OWNER: 1b).
//
// Bindings: draw sets 0 / 1 exactly as clean_draw.vert (instances, draw view, frame info,
// material UVs, cel block, skin meta) and the cluster set at CLUSTER_SET = 2 (pool,
// ClusterInstance rows, selected entries, the 1b views tail and aux words). A cluster vertex
// belongs to selected entry `sel` (gl_InstanceIndex = the draw's firstInstance in the MDI
// tiers, the task payload in the mesh tier): ClusterSelected -> ClusterInstance -> the
// renderer Instance row (render_instance) that carries the material, tint, UV transform and
// flags -- the same material path as the legacy static draw, so at tolerance 0 the G-buffer
// it writes matches the legacy draw's.
//
// Pose: static instances use the renderer row's model_matrix (+ the GPU target-omega spin),
// as clean_draw.vert. RIGGED instances (CLI_RIGGED) read their SKINNED vertex (2a, #747): the
// cluster pre-skin (clean_cluster_preskin.comp) skinned the selected cluster into the skinned
// arena (CL_B_SKIN_OUT, slot = the scratch map word of the selected entry), and the vertex is
// placed exactly as clean_draw_rigged.vert places the legacy pre-skin output (eye-relative
// position + the palettes' eye, world normal / tangent, texcoord from the source vertex, no
// planar texgen). An entry the pre-skin did not skin (not drawn for real: the debug overlay of
// an avatar still on the legacy path) is drawn in REST POSE through ClusterInstance::xform.
// Attachments (bit 9), terrain (bit 5), HUD and water never reach the cluster path (the CPU
// gatherer, llcleanclusterselectvk.cpp, keeps them on the legacy one). Rigged alpha-blend does
// (CLI1B_BLEND): drawn from the ordered alpha lists with the blend / translucency pipelines.
//
// DEPTH-ONLY (-DCLEAN_DEPTH_ONLY [+ -DCLEAN_DEPTH_MASK], RenderVKDepthOnlyVS): the ClVOut
// fields the depth passes never read (normal / tangent frame, previous position, cel, raw
// UV, encloses-camera bit, pick) are not computed; pos (+ for MASK material / uv / tint /
// flags) come from the unchanged lines, so the depth matches the G-buffer draw's bit for bit.
// SLIM PRE-PASS VARIANTS (+ -DCLEAN_DEPTH_MOTION / -DCLEAN_DEPTH_NORMAL on top of the MASK
// depth flavour, as clean_draw.vert): world_pos + prev_world_pos (the previous instance row /
// the skinned arena's previous region, verbatim) / world normal + tangent + raw UV.
#ifndef CLEAN_CLUSTER_DRAW_COMMON_GLSL
#define CLEAN_CLUSTER_DRAW_COMMON_GLSL
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MOTION) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_WORLD_POS      // 5 world position
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MOTION)
#define CLEAN_VS_PREV_POS       // 10 previous world position (+ binding 23)
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_NORMAL_FRAME   // 2 world normal, 7 world tangent, 8 raw UV
#endif

#ifndef CLUSTER_SET
#define CLUSTER_SET 2   // (-DCLUSTER_SET=3: the ray-query lit capture variant, RT set at 2)
#endif
#define CL_DECLARE_POOL
#define CL_DECLARE_INSTANCES
#include "clean_cluster_common.glsl"
#include "clean_cluster_select_common.glsl"
#define CL_DECLARE_SKIN_OUT
#define CL_DECLARE_SKIN_SCRATCH
#include "clean_cluster_skin_common.glsl"
#include "clean_qvertex.glsl"
#include "clean_skinvert.glsl"

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
    vec4 spin_axis;
    vec4 spin_pivot;
};
layout(std430, set = 0, binding = 0) readonly buffer Instances { Instance instances[]; };
// TEMPORAL (#84): the instance rows as of the PREVIOUS frame (= clean_draw.vert binding 23).
// Read only for prev_world_pos, which only the motion pre-pass FS consumes (#749: cluster
// draws in the depth / normal / motion pre-pass, so AO, the RT shadow mask and TAA see them).
#ifdef CLEAN_VS_PREV_POS
layout(std430, set = 0, binding = 23) readonly buffer PrevInstances { Instance prev_instances[]; };
#endif

#include "clean_draw_view.glsl"
#include "clean_uv_common.glsl"
#include "clean_frame_info.glsl"
#define CEL_VERTEX
#include "clean_cel_common.glsl"

// The outputs the G-buffer fragment shaders read (clean_draw_common.glsl locations).
struct ClVOut
{
    vec4 pos;            // gl_Position
    uint material_id;    // 0
    vec2 uv;             // 1
    vec3 world_normal;   // 2
    vec4 tint;           // 3 (debug views: the debug colour)
    uint flags;          // 4
    vec3 world_pos;      // 5
    vec4 world_tangent;  // 7
    vec2 uv_raw;         // 8
    uint terrain_rec;    // 9
    uint cel;            // 11
    vec3 prev_world_pos; // 10 (motion pre-pass, #749)
    uint inst;           // 12 GPU pick (#702): the renderer Instance row (-DCLEAN_PICK only)
    vec2 pick_st;        // 13 ... texcoord0 before texgen (LL's surface ST)
    vec4 pick_tangent;   // 14 ... the vertex stream's tangent, world, w = handedness (0 = none)
    vec4 rest;           // 15 #901 REST-POSE ACCUMULATION: the bind-space position, w the bind-space
                         //    normal's z (w -2: none -- static, depth-only, the grids off)
};

// One quantized vertex (20 B = 5 words) of the pool at absolute QVertex index `v`, decoded
// with the cluster block's header (2 pseudo-vertices before its first real vertex `first`).
void clPoolVertex(uint first, uint v, out vec3 position, out vec2 texcoord0, out vec3 normal, out vec4 tangent)
{
    const QvHeader qh = QV_LOAD_HEADER(cl_pool, first);
    const uint b = v * QV_WORDS;
    position  = qvPos(qh, cl_pool[b], cl_pool[b + 1u]);
    texcoord0 = qvUV(qh, cl_pool[b + 2u]);
    normal    = qvNormal(cl_pool[b + 3u]);
    tangent   = qvTangent(cl_pool[b + 1u], cl_pool[b + 4u]);
}

// One axis of the ENCLOSES-CAMERA oriented-box test: |d . axis| within the half extent
// (axis = an un-normalized model column, so both sides carry len^2). Called per axis, NOT
// from a loop over M[i]: a dynamically indexed M kept the pose matrix in function memory,
// so the position chain compiled differently here than in the depth-only variant.
bool boxAxisInside(vec3 d, vec3 axis, float h)
{
    const float len = length(axis);
    return !(len > 0.0 && abs(dot(d, axis)) > h * len * len);
}

// GPU TARGET OMEGA (= clean_draw.vert spinPose).
mat4 clSpinPose(mat4 M, vec4 spin_axis, vec4 spin_pivot, float time)
{
    float rate  = length(spin_axis.xyz);
    vec3  a     = spin_axis.xyz / rate;
    float ang   = mod(rate * (time - spin_axis.w), 6.28318531);
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    mat3 R = mat3(t*a.x*a.x + c,     t*a.x*a.y + s*a.z, t*a.x*a.z - s*a.y,
                  t*a.x*a.y - s*a.z, t*a.y*a.y + c,     t*a.y*a.z + s*a.x,
                  t*a.x*a.z + s*a.y, t*a.y*a.z - s*a.x, t*a.z*a.z + c);
    vec3 pv = spin_pivot.xyz;
    return mat4(vec4(R * M[0].xyz, 0.0), vec4(R * M[1].xyz, 0.0), vec4(R * M[2].xyz, 0.0),
                vec4(pv + R * (M[3].xyz - pv), 1.0));
}

// A SKINNED rigged cluster vertex (2a, #747): skinned cluster `s`, vertex `local` of it.
// = clean_draw_rigged.vert for a per-frame (eye-relative) range, with the world draw view.
ClVOut clusterSkinnedVertex(uint sel, ClSelected cs, Instance inst, uint s, uint local, vec3 position, vec3 normal,
                            vec2 texcoord0)
{
    ClVOut o;
    const uint v = s * CL_MAX_CLUSTER_VERTS + min(local, CL_MAX_CLUSTER_VERTS - 1u);
    // CHARACTER SPACE (2026-10-14 step 2): the arena holds the vertex in its character's frame;
    // eye-relative = M local + t (clean_cluster_skin_common.glsl CHARACTER SPACE), normal and
    // tangent rotated by M.
    const ClChar ch = clSkinChar(clSkinRow(cl_instances[cs.instance].flags), false);
    const vec3 rel = ch.m * SV_POS(cl_skin_out, v) + ch.t;   // eye-relative (the palettes' eye)
    const vec3 eye = clSkinEye();
    // CAMERA-RELATIVE PROJECTION (#555): (eye - origin) first, both ~km agent-space values.
    o.pos = pc.u_view_proj * vec4(rel + (eye - pc.u_origin.xyz), 1.0);
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    const vec3 nrm = ch.m * svUnpackNormal(cl_skin_out[v * SV_WORDS + 3u]);
    o.uv = cleanFaceUV(inst.flags & ~64u, inst.uv_matrix, texcoord0, nrm, rel, inst.model_matrix, pc.u_cam_time.w);
    o.material_id = inst.material_id;
    o.tint = inst.tint;
    o.flags = inst.flags;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    vec4 tng = svUnpackTangent(cl_skin_out[v * SV_WORDS + 4u]);
    tng.xyz = ch.m * tng.xyz;
    o.world_normal = nrm;
#endif
#ifndef CLEAN_DEPTH_ONLY
    o.cel = celVertexWord(inst.flags, inst.skin_id, true, false);
#endif
#ifdef CLEAN_VS_WORLD_POS
    o.world_pos = rel + eye;
#endif
#ifdef CLEAN_VS_PREV_POS
    // Previous position (integration 2a + 2c; = clean_draw_rigged.vert's prev_skinned + eye):
    // the cluster pre-skin's previous-position region, eye-relative to THIS frame's eye. Read
    // only by the motion pre-pass FS, on the frames the pre-skin writes it.
    o.prev_world_pos = o.world_pos;
    {
        // (2026-10-14 step 3) a slot skinned THIS frame has its previous position in the region; a
        // REUSED slot (held pose: the same character-space vertex last frame) takes it from its
        // row's previous frame: Mp local + tp (relative to this frame's eye, as the region).
        const uint cap = cl_rig[CLK_CAP];
        const uint mw = clSkinMetaBaseC(cap) + s * CL_SKIN_META_WORDS + 7u;
        const uint pb = clSkinPrevBase() + v * 3u;
        if (mw < uint(cl_skin_out.length()) && cl_skin_out[mw] != cl_rig[CLK_FRAME])
        {
            const ClChar pc_ = clSkinChar(clSkinRow(cl_instances[cs.instance].flags), true);
            o.prev_world_pos = pc_.m * SV_POS(cl_skin_out, v) + pc_.t + eye;
        }
        else if (pb + 3u <= uint(cl_skin_out.length()))
            o.prev_world_pos = vec3(uintBitsToFloat(cl_skin_out[pb]), uintBitsToFloat(cl_skin_out[pb + 1u]),
                                    uintBitsToFloat(cl_skin_out[pb + 2u])) + eye;
    }
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    o.uv_raw = texcoord0;
    o.terrain_rec = 0xFFFFFFFFu;
    const mat4 nuvm = matMapOwnUV(inst.material_id, MAP_NORMAL) ? matMapUVMatrix(inst.material_id, MAP_NORMAL)
                                                                  : inst.uv_matrix;
    o.world_tangent = cleanFaceTangent(inst.flags & ~64u, nuvm, nrm, tng);
#endif
#ifndef CLEAN_DEPTH_ONLY
    // #783 M3 / #901 SNOW AND SHELTER ON AVATARS (= clean_draw_rigged.vert): the wearer's joint-table
    // slot -- the ClusterInstance's avatar row (CLI1B_AVATAR, bits 16..25) -- as 0x80000000 | slot
    // in v_terrain_rec (clean_weather_surface.glsl weatherAvatarSlot). Without it every cluster-drawn
    // avatar part had no slot: no shelter map, no per-avatar wetness, no M3 coat, no grid -- no snow
    // at all (#901 / #909 follow-up). Only while the avatar snow / shelter maps run (no load otherwise).
    o.terrain_rec = 0xFFFFFFFFu;
    o.rest = vec4(0.0, 0.0, 0.0, -2.0);
    {
        const uint cf = cl_instances[cs.instance].flags;
        const uint av = (cf & CLI1B_AVATAR) != 0u ? ((cf >> CLI1B_AV_SHIFT) & AV_SLOT_MASK) : 0xFFFFFFFFu;
        if (av < AV_SLOT_CAP && (ssr.weather[6].z > 0.5 || ssr.weather[6].w > 0.5) && (inst.flags & 524288u) == 0u)
        {
            o.terrain_rec = 0x80000000u | av;
            // #901 REST-POSE ACCUMULATION (ssr.weather[14].w bit 8: the grids live): the BIND-SPACE
            // position of this vertex -- bind_shape x the cluster's own source vertex (the pool vertex
            // the pre-skin skinned it from: clean_cluster_preskin.comp bind_pos), the rigged VS's
            // key -- and its normal. Derived here per vertex from the pool and the skin meta (one
            // mat4 per instance, binding 38): no per-vertex rest copy, no VRAM.
            if ((uint(ssr.weather[14].w + 0.5) & 8u) != 0u && (cel_flags & CEL_META) != 0u
                && inst.skin_id < 0x100000u && (inst.skin_id + 1u) * CEL_SKIN_META_WORDS <= uint(cel_skin_meta.length()))
            {
                const uint w = inst.skin_id * CEL_SKIN_META_WORDS + 1876u;   // (SkinMeta::bind_shape)
                mat4 BS;
                for (int c = 0; c < 4; ++c)
                    BS[c] = vec4(uintBitsToFloat(cel_skin_meta[w + 4u * uint(c)]), uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 1u]),
                                 uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 2u]), uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 3u]));
                const mat3 B3 = mat3(BS);
                const vec3 rp = (BS * vec4(position, 1.0)).xyz;
                const vec3 rn = mat3(cross(B3[1], B3[2]), cross(B3[2], B3[0]), cross(B3[0], B3[1])) * normal;
                if (!any(isnan(rp)) && !any(isinf(rp)) && dot(rn, rn) > 1e-20)
                    o.rest = vec4(rp, normalize(rn).z);
            }
        }
    }
    // Pick (= clean_draw_rigged.vert): texcoord0, the skinned stream tangent.
    o.inst = cl_instances[cs.instance].render_instance;
    o.pick_st = texcoord0;
    o.pick_tangent = (dot(tng.xyz, tng.xyz) > 1e-16) ? vec4(normalize(tng.xyz), tng.w) : vec4(0.0);
#endif   // !CLEAN_DEPTH_ONLY
    // (Debug views: the tint's alpha 1 also reaches the alpha-test FSs, as before.)
    const uint dm = cl_aux.debug_mode;
    if (clDebugTinted(dm))
        o.tint = vec4(clDebugColour(dm, cs.cluster_ref, cs.instance, cl_draws[cl_aux.aux_base + sel]), 1.0);
    // 6 WELD (#798): this skinned cluster's weld outcome (clean_cluster_skin_common.glsl WELD STATUS)
    // -- grey = no weld candidate, green = candidates welded (moved), cyan = every candidate already
    // seamless with its partners (NO-OP: left alone), yellow = some welded or seamless / some with no
    // partner, red = candidates but none paired (no other mesh's vertex within eps in the pairing
    // space), blue = rejected by the skin weights / the max gap only.
    if (dm == 6u)
    {
        const uint sw = clSkinWeldStatBaseC(cl_rig[CLK_CAP]) + s;
        const uint st = sw < uint(cl_skin_out.length()) ? cl_skin_out[sw] : 0u;
        const uint cand = st & 0xFFu, welded = (st >> 8u) & 0xFFu, nopair = (st >> 16u) & 0xFFu;
        const uint rej = (st >> 24u) & 0xFu, noop = st >> 28u;
        vec3 c = vec3(0.55);
        if (cand != 0u)
        {
            if (nopair != 0u) c = (welded | noop) != 0u ? vec3(1.0, 0.85, 0.05) : vec3(1.0, 0.05, 0.05);
            else if (rej != 0u) c = vec3(0.1, 0.3, 1.0);
            else if (welded != 0u) c = vec3(0.1, 0.9, 0.15);
            else c = vec3(0.0, 0.9, 0.95);   // (#798: every candidate already seamless: left alone)
        }
        o.tint = vec4(c, 1.0);
    }
    return o;
}

// The vertex of selected entry `sel` (absolute index into CL_B_SELECTED) with decoded
// attributes; `local` = its index inside its cluster (the skinned arena's addressing).
// Same arithmetic as clean_draw.vert's static branch.
ClVOut clusterVertex(uint sel, uint local, vec3 position, vec2 texcoord0, vec3 normal, vec4 tangent)
{
    ClVOut o;
    const ClSelected cs = cl_sel[sel];
    const ClInstance ci = cl_instances[cs.instance];
    const Instance inst = instances[ci.render_instance];
    const bool rest = (ci.flags & CLI_RIGGED) != 0u;
    // A rigged instance drawn for real (1b's CLI1B_DRAW = 1024) whose cluster got no skinned slot
    // (the arena overflowed this frame; it grows from the read-back demand) is NOT drawn: every
    // vertex collapses to one point (degenerate, no fragment). Never the rest pose (2026-10-14:
    // the cluster path replaces the legacy one, there is no legacy copy to fall back to). The
    // RenderVKClusterDebug overlay of undrawn instances keeps the rest pose.
    bool collapse = false;
    if (rest)
    {
        const uint s = clSkinnedCluster(sel);
        if (s != CL_NONE) return clusterSkinnedVertex(sel, cs, inst, s, local, position, normal, texcoord0);
        collapse = (ci.flags & 1024u) != 0u;
    }
    mat4 M = inst.model_matrix;
    if (rest)
        M = transpose(mat4(ci.xform_r0, ci.xform_r1, ci.xform_r2, vec4(0.0, 0.0, 0.0, 1.0)));
    if (collapse) { M = mat4(0.0); M[3] = vec4(pc.u_origin.xyz, 1.0); position = vec3(0.0); }
    else if (dot(inst.spin_axis.xyz, inst.spin_axis.xyz) > 0.0)
        M = clSpinPose(M, inst.spin_axis, inst.spin_pivot, pc.u_cam_time.w);

#ifdef CLEAN_VS_WORLD_POS
    const vec4 world_pos = M * vec4(position, 1.0);
#endif
#ifdef CLEAN_VS_PREV_POS
    // Previous-frame position (#749; = clean_draw.vert's static branch): the previous
    // instance row (+ the previous frame's time for GPU spinners). Rigged rest pose (wave 1,
    // until 2a skins the cut): no history -- camera-only motion, as a rigged draw without one.
    o.prev_world_pos = world_pos.xyz;
    if (!rest && ci.render_instance < uint(prev_instances.length()))
    {
        const Instance pinst = prev_instances[ci.render_instance];
        mat4 PM = pinst.model_matrix;
        if (dot(pinst.spin_axis.xyz, pinst.spin_axis.xyz) > 0.0)
            PM = clSpinPose(PM, pinst.spin_axis, pinst.spin_pivot, ssr.taa_params.x);
        o.prev_world_pos = (PM * vec4(position, 1.0)).xyz;
    }
#endif
    // CAMERA-RELATIVE PROJECTION (#555), as clean_draw.vert.
    o.pos = pc.u_view_proj * vec4(mat3(M) * position + (M[3].xyz - pc.u_origin.xyz), 1.0);
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    o.uv = cleanFaceUV(inst.flags, inst.uv_matrix, texcoord0, normal, position, inst.model_matrix, pc.u_cam_time.w);
    o.material_id = inst.material_id;
    o.tint = inst.tint;
    o.flags = inst.flags;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    const mat3 M3   = mat3(M);
    const float det = dot(M3[0], cross(M3[1], M3[2]));
    const mat3 Ncof = mat3(cross(M3[1], M3[2]), cross(M3[2], M3[0]), cross(M3[0], M3[1]));
    o.world_normal = (det < 0.0 ? -1.0 : 1.0) * (Ncof * normal);
#endif
#ifndef CLEAN_DEPTH_ONLY
    o.rest = vec4(0.0, 0.0, 0.0, -2.0);   // (#901: static / a rigged rest-pose overlay -- no grid)
    o.cel = celVertexWord(inst.flags, inst.skin_id, rest, false);
    // ENCLOSES CAMERA (v_flags bit14), static only (clean_draw.vert's oriented-box test).
    if (!rest && (inst.flags & (512u | 8u)) == 0u)
    {
        const vec3 hc = 0.5 * (inst.aabb_min.xyz + inst.aabb_max.xyz);
        const vec3 he = 0.5 * abs(inst.aabb_max.xyz - inst.aabb_min.xyz);
        const vec3 d  = pc.u_cam_time.xyz - (M * vec4(hc, 1.0)).xyz;
        const bool inside = boxAxisInside(d, M[0].xyz, he.x) && boxAxisInside(d, M[1].xyz, he.y)
                         && boxAxisInside(d, M[2].xyz, he.z);
        if (inside) o.flags |= 16384u;
    }
#endif
#ifdef CLEAN_VS_WORLD_POS
    o.world_pos = world_pos.xyz;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    o.uv_raw = cleanFaceRawUV(inst.flags, texcoord0, normal, position, inst.model_matrix);
    o.terrain_rec = ci.render_instance;   // (#783 M2: the instance slot, as clean_draw.vert)
    const mat4 nuvm = matMapOwnUV(inst.material_id, MAP_NORMAL) ? matMapUVMatrix(inst.material_id, MAP_NORMAL)
                                                                  : inst.uv_matrix;
    const vec4 tl = cleanFaceTangent(inst.flags, nuvm, normal, tangent);
    const vec3 wt = M3 * tl.xyz;
    o.world_tangent = (dot(wt, wt) > 1e-16) ? vec4(normalize(wt), det < 0.0 ? -tl.w : tl.w) : vec4(0.0);
#endif
#ifndef CLEAN_DEPTH_ONLY
    // Pick (= clean_draw.vert): LL's volume tangent through the transform, its w kept.
    o.inst = ci.render_instance;
    o.pick_st = texcoord0;
    const vec3 pt = M3 * tangent.xyz;
    o.pick_tangent = (dot(pt, pt) > 1e-16) ? vec4(normalize(pt), tangent.w) : vec4(0.0);
#endif   // !CLEAN_DEPTH_ONLY
    // DEBUG VIEWS (RenderVKClusterDebug 1..3): the colour rides the tint (clean_cluster_draw_debug.frag).
    const uint dm = cl_aux.debug_mode;
    if (clDebugTinted(dm))
        o.tint = vec4(clDebugColour(dm, cs.cluster_ref, cs.instance, cl_draws[cl_aux.aux_base + sel]), 1.0);
    return o;
}

#endif
