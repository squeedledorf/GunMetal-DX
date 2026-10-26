#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_shader_draw_parameters : require   // gl_BaseVertexARB: the source vertex (#616)
#include "clean_av_slots.glsl"
// vkclean — rigged draw vertex shader (Vulkan port of cleanPreSkinnedV.glsl).
//
// Source vertices come from the SKIN-OUTPUT buffer that the pre-skin compute
// (clean_preskin.comp) already filled with WORLD-space skinned positions/normals.
// So the VS does NO LBS and NO model_matrix apply -- it just projects the world
// position and passes the (already world-space) normal to the fragment shader,
// which lights with the world-space sun direction exactly like the static path.
//
// Per-instance constant data (material id, flags, tint, UV xform) still flows via
// gl_InstanceIndex (== the cull-written firstInstance) into the Instance SSBO,
// identical to the static path. The fragment shader (clean_draw.frag) is reused.
//
// DEPTH-ONLY VARIANTS (RenderVKDepthOnlyVS; -DCLEAN_DEPTH_ONLY, + -DCLEAN_DEPTH_MASK), as
// clean_draw.vert: only the position chain below (the collapse tests, the reduced-tier
// pelvis placement, the projection), + for MASK the alpha test's inputs (locations 0 / 1 /
// 3 / 4), from the same source lines. No previous-frame reads, normal frame or cel / pick.
// SLIM PRE-PASS VARIANTS (as clean_draw.vert): + -DCLEAN_DEPTH_MOTION 5 world position + 10
// previous world position (the re-skinned previous positions / held-pose pelvis chain below,
// verbatim); + -DCLEAN_DEPTH_NORMAL 2 world normal, 5 world position, 7 world tangent, 8 raw UV.
// SLIM G-BUFFER VARIANT (-DCLEAN_GBUFFER_VS, RenderVKGbufSlimVS; as clean_draw.vert): what the
// G-buffer FS reads -- no previous skinned position (10) and no pick outputs (12 13 14).
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MOTION) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_WORLD_POS      // 5 world position
#endif
#if (!defined(CLEAN_DEPTH_ONLY) && !defined(CLEAN_GBUFFER_VS)) || defined(CLEAN_DEPTH_MOTION)
#define CLEAN_VS_PREV_POS       // 10 previous world position (+ bindings 24 / 25)
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_NORMAL)
#define CLEAN_VS_NORMAL_FRAME   // 2 world normal, 7 world tangent, 8 raw UV
#endif
#if !defined(CLEAN_DEPTH_ONLY) && !defined(CLEAN_GBUFFER_VS)
#define CLEAN_VS_PICK           // 12 instance, 13 pick ST, 14 pick tangent
#endif

// COMPACT SKINNED VERTEX (#616, clean_skinvert.glsl, 20 B): fp32 position + octahedral
// normal / tangent words (decoded below: the tangent's "none" sentinel and sign bit need
// the raw word). The texcoord is the SOURCE quantized vertex's, read through binding 39:
// this draw's vertices were skinned from [rig_cache RC_SRC, + vert count) of the static
// mega-VBO in order, so the source index is that base + (gl_VertexIndex - vertexOffset).
// That holds for the per-frame ranges, the skin cache (#31), decimated steps (#561: the
// cull stores the step's own slot) and the shadow-caster LOD (#564: its index buffer
// renumbers onto the same skinned vertices, same vertexOffset).
layout(location = 0) in vec3 position;   // WORLD-space (pre-skinned)
layout(location = 2) in uint a_snrm;     // octahedral normal (WORLD-space)
layout(location = 3) in uint a_stan;     // octahedral tangent + bitangent sign (WORLD-space)
layout(std430, set = 0, binding = 39) readonly buffer QvWords { uint qv_words[]; };
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
    vec4 spin_axis;     // xyz world axis*rate, w t0 (GPU target omega)
    vec4 spin_pivot;    // xyz agent-space pivot
};
layout(std430, set = 0, binding = 0) readonly buffer Instances { Instance instances[]; };
// #1033 SLIM RECORD (clean_vs_inst.glsl, derived every frame by clean_vs_inst.comp): the 112 B
// record carries every field read below; the full row is opened only for GPU texture animation.
#include "clean_vs_inst.glsl"
layout(std430, set = 0, binding = 54) readonly buffer VsInstances { VsInst vs_inst[]; };
// TEMPORAL (#84, stage 2): each skinned vertex's position LAST frame, eye-relative to
// THIS frame's eye like `position`, at the same index (clean_preskin.comp re-skins it
// with last frame's palettes). Only the motion pre-pass consumes what is derived from it.
// Tightly packed xyz, 3 floats per vertex (clean_preskin.comp binding 6).
#ifdef CLEAN_VS_PREV_POS
layout(std430, set = 0, binding = 24) readonly buffer PrevSkinned { float prev_skinned[]; };
#endif
// REDUCED-TIER SKIN CACHE (#31, clean_cull.comp / clean_preskin.comp). The main cull's
// per-instance state word (rig_cache[id * RC_WORDS + RC_STATE]): bit 8 = this draw reads the avatar's
// cached PELVIS-LOCAL vertices (joint-table slot in bits 0..7), bit 9 = they were
// re-skinned this frame. Local vertices are placed with the pelvis's CURRENT transform
// from the joint table (it follows the root every frame, so a held pose still moves
// smoothly); held frames' previous position is the same local vertex under LAST
// frame's pelvis (root motion only, as recordPrevJoints makes the joint history).
layout(std430, set = 0, binding = 35) readonly buffer RigCache { uint rig_cache[]; };
#include "clean_joint_table.glsl"   // mat3x4 joint world rows (jointWorld())
layout(std430, set = 0, binding = 2) readonly buffer AvatarJointsSSBO { AvatarJoints avatar_joints[]; };
#ifdef CLEAN_VS_PREV_POS
layout(std430, set = 0, binding = 25) readonly buffer PrevAvatarJointsSSBO { AvatarJoints prev_avatar_joints[]; };
#endif

// Rotation of a joint world matrix (LLMatrix4::initAll: columns = world axes x local
// scale). Must match clean_preskin.comp's rigidRot exactly.
mat3 rigidRot(mat4 m)
{
    vec3 c0 = m[0].xyz, c1 = m[1].xyz, c2 = m[2].xyz;
    float l0 = length(c0), l1 = length(c1), l2 = length(c2);
    if (!(l0 > 1e-8 && l1 > 1e-8 && l2 > 1e-8)) return mat3(1.0);
    return mat3(c0 / l0, c1 / l1, c2 / l2);
}

// The draw's view block (pc.u_*; u_water_fog.xyz = the main eye in MIRROR / PROBE
// captures, sun_color.w >= 1) + the 16 B push (view index, F8 mode); #238.
#include "clean_draw_view.glsl"

#include "clean_uv_common.glsl"
#ifndef CLEAN_DEPTH_ONLY
#define CEL_VERTEX
#include "clean_cel_common.glsl"    // avatar slot + cel scope (#549 / #550): v_cel
#include "clean_frame_info.glsl"    // #783 M3: ssr.weather[6].z -- avatars carry snow (else no slot lookup)
// #901 REST-POSE ACCUMULATION (clean_weather_accum.glsl; draw set 0 binding 50): the record's head
#define WX_AC_SET 0
#define WX_AC_BINDING 50
#include "clean_weather_accum.glsl"
#endif
#define GMDX_REZFX_VS
#include "gmdx_rezfx.glsl"          // <GMDX> rez effects: de-rez vertex effects, v_rez / v_rez_box

#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
// v_uv invariant in every variant (clean_draw.vert): mask pre-pass and EQUAL lit FS sample alike.
layout(location = 0) flat out uint v_material_id;
layout(location = 1) invariant out vec2 v_uv;
layout(location = 3) flat out vec4 v_tint;
layout(location = 4) flat out uint v_flags;      // match the shared fragment shader interface
// <GMDX> rez effects (gmdx_rezfx.glsl): invariant, as v_uv (the mask pre-pass's de-rez cut)
layout(location = 16) invariant out vec4 v_rez;   // <GMDX> 16 / 17 (15 is #901's v_wx_rest)
layout(location = 17) invariant flat out vec4 v_rez_box;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
layout(location = 2)      out vec3 v_world_normal;
layout(location = 7)      out vec4 v_world_tangent;
layout(location = 8)      out vec2 v_uv_raw;        // pre-transform texcoord (per-map UV transforms)
#endif
#ifdef CLEAN_VS_WORLD_POS
layout(location = 5)      out vec3 v_world_pos;   // (rigged is never terrain, but the FS reads both)
#endif
#ifdef CLEAN_VS_PREV_POS
// Motion pre-pass interface (clean_draw.vert): where this vertex was last frame (world
// space). No other FS reads it, so every other pipeline drops it and the prev read.
layout(location = 10)     out vec3 v_prev_world_pos;
#endif
#ifndef CLEAN_DEPTH_ONLY
layout(location = 9) flat out uint v_terrain_rec;   // shared FS interface (never terrain; #783 M3: 0x80000000 | the avatar's slot -- its own snow)
// AVATAR MASK / CEL (#549 / #550): avatar slot + 1 (from the skin meta) | toon-lit bit 16.
layout(location = 11) flat out uint v_cel;
// #901 REST-POSE ACCUMULATION (clean_weather_surface.glsl weatherAvatarAccum): xyz the vertex's
// REST-POSE position (its bind pose: bind_shape x the source position), w its rest normal's z;
// w -2 = none (the accumulation off, no skin meta: the lit FS keeps the per-avatar values).
layout(location = 15) out vec4 v_wx_rest;
#endif   // !CLEAN_DEPTH_ONLY
#ifdef CLEAN_VS_PICK
// GPU CURSOR PICK (#702, clean_pick.frag): the instance this vertex belongs to (dropped by
// every other pipeline).
layout(location = 12) flat out uint v_inst;
// ... and its surface as LL's pick reports it (#704, clean_draw.vert): texcoord0 and the
// skinned tangent (world, w = handedness; 0 = none).
layout(location = 13)      out vec2 v_pick_st;
layout(location = 14)      out vec4 v_pick_tangent;
#endif   // CLEAN_VS_PICK
// DEPTH PRE-PASS invariance: the depth pre-pass pipelines run this source (the
// CLEAN_DEPTH_ONLY module, or this one with no FS / alpha-test FS) and pass 1 then
// depth-tests EQUAL. Without `invariant` the driver may compile gl_Position differently
// per pipeline / module (varying DCE, FMA contraction) and an ULP of difference punches
// z-fighting holes.
invariant gl_Position;

void main()
{
    const VsInst inst = vs_inst[gl_InstanceIndex];
    const mat4 inst_model = vsiModel(inst);

    // Position is EYE-RELATIVE (camera-relative palettes); the renderer pushes the
    // translation-free view-projection for rigged draws. World position for
    // lighting = eye + relative.
    // Mouselook with "Show me in Mouselook" off: main-view rigged draws carry
    // u_ambient_color.w = 1 and drop the agent's own avatar (bit13). Mirror /
    // probe / shadow draws never set it, so reflections and shadows keep it.
    // F8 (see clean_draw.vert): push f8 1 = the alpha cull-FRONT draw, where a
    // single-sided face's back side would only be discarded by the FS.
    // u_ambient_color.w = 0.25: only the agent's own system-avatar HEAD parts (head, eyes,
    // eyelashes, hair: instance bit 18) leave the main view -- LL's !needsRenderHead()
    // (mouselook with "Show me in Mouselook" on, RLVa showselfhead).
    const bool f8_collapse = g_draw_push.f8 == 1u
                             && (inst.flags & (256u | 8u)) == 0u;
    // CLUSTER LOD (#747): a pass that draws the cluster path's avatars itself (u_sky_zenith.w
    // = 1: the main view's opaque / mask passes; the sun cascades, projector tiles, mirror,
    // dynamic probe faces and the GPU pick when their cluster rig list / pick draw runs this
    // frame) drops an instance the cluster path owns (rig_cache state bit 1024, main cull).
    // The selection outline and the alpha-blend passes keep it.
    const uint rcb0 = uint(gl_InstanceIndex) * RC_WORDS;
    const bool cluster_owned = pc.u_sky_zenith.w > 0.5 && rcb0 + RC_WORDS <= uint(rig_cache.length())
                            && (rig_cache[rcb0 + RC_STATE] & RC_ST_CLUSTER) != 0u;
    if (f8_collapse || cluster_owned
        || (pc.u_ambient_color.w > 0.5 && (inst.flags & 8192u) != 0u && (inst.flags & 8u) == 0u)
        || (pc.u_ambient_color.w > 0.2 && (inst.flags & 262144u) != 0u))
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);   // outside the [0,1] depth range: clipped
        return;
    }
    // Pre-skinned verts are relative to the MAIN eye; the mirror and probe captures'
    // cam_time holds their own camera, so the main eye rides in u_water_fog.
    const vec3 eye = (pc.u_sun_color.w > 0.5) ? pc.u_water_fog.xyz : pc.u_cam_time.xyz;

#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    const vec3 normal  = svUnpackNormal(a_snrm);
#else
    const vec3 normal  = vec3(0.0);   // (no output reads it)
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    const vec4 tangent = svUnpackTangent(a_stan);
#else
    const vec4 tangent = vec4(0.0);
#endif
    vec3 pos = position, nrm = normal;
    vec4 tng = tangent;
#ifdef CLEAN_VS_PREV_POS
    // gl_VertexIndex includes the draw's vertexOffset (= this item's private range).
    const uint pvb = uint(gl_VertexIndex) * 3u;
    vec3 prev_world = vec3(prev_skinned[pvb+0u], prev_skinned[pvb+1u], prev_skinned[pvb+2u]) + eye;
#endif
    const uint rcb = uint(gl_InstanceIndex) * RC_WORDS;
    const bool rc_ok = rcb + RC_WORDS <= uint(rig_cache.length());
    const uint rs = rc_ok ? rig_cache[rcb + RC_STATE] : 0u;
    // The source vertex's texcoord (out of range: a frame's transient before the state
    // buffer grows -- the draw shows UV 0 rather than reading past the VBO).
    vec2 texcoord0 = vec2(0.0);
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    {
        const uint sbase = rc_ok ? rig_cache[rcb + RC_SRC] : 0xFFFFFFFFu;
        const uint local = uint(gl_VertexIndex - gl_BaseVertexARB);
        if (sbase != 0xFFFFFFFFu && (sbase + local + 1u) * QV_WORDS <= uint(qv_words.length()))
            texcoord0 = SV_SRC_UV(qv_words, sbase, local);
    }
#endif
    if ((rs & RC_ST_LOCAL) != 0u && (inst.flags & 8u) == 0u)
    {
        const uint slot = rs & RC_ST_SLOT;
        const mat4 P = jointWorld(avatar_joints[slot].world[0]);
        const mat3 R = rigidRot(P);
        pos = R * position + (P[3].xyz - eye);
        nrm = R * normal;
        tng.xyz = R * tangent.xyz;
#ifdef CLEAN_VS_PREV_POS
        if ((rs & RC_ST_SKINNED) == 0u)
        {
            const mat4 PP = jointWorld(prev_avatar_joints[slot].world[0]);
            prev_world = rigidRot(PP) * position + PP[3].xyz;
        }
#endif
    }

    // <GMDX> REZ EFFECTS: the source (bind pose) vertex is the effects' object space; the de-rez
    // vertex effects move the skinned vertex (every variant: the same source lines).
    vec3 rz_src = vec3(0.0);
    if (rezfxSlotOf(inst.flags) != 0u)
    {
        const uint sb = rc_ok ? rig_cache[rcb + RC_SRC] : 0xFFFFFFFFu;
        const uint lc = uint(gl_VertexIndex - gl_BaseVertexARB);
        if (sb != 0xFFFFFFFFu && sb >= QV_HEADER_VERTS && (sb + lc + 1u) * QV_WORDS <= uint(qv_words.length()))
            rz_src = qvPos(QV_LOAD_HEADER(qv_words, sb), qv_words[(sb + lc) * QV_WORDS], qv_words[(sb + lc) * QV_WORDS + 1u]);
        if ((inst.flags & 0x00400000u) != 0u)
        {
            const bool ctrOk = rc_ok && rig_cache[rcb + RC_CTR_KEY] == uint(gl_BaseVertexARB) + 1u;
            vec3 ctr = ctrOk ? uintBitsToFloat(uvec3(rig_cache[rcb + RC_CTR], rig_cache[rcb + RC_CTR + 1u], rig_cache[rcb + RC_CTR + 2u]))
                             : vec3(0.0);
            if (ctrOk && (rs & RC_ST_LOCAL) != 0u && (inst.flags & 8u) == 0u)   // pelvis-local, as `position`
            {
                const mat4 P = jointWorld(avatar_joints[rs & RC_ST_SLOT].world[0]);
                ctr = rigidRot(P) * ctr + (P[3].xyz - eye);
            }
            pos = rezfxRigDisplace(inst.flags, pos, rz_src, ctr, ctrOk);
        }
    }
    // </GMDX>
    gl_Position    = pc.u_view_proj * vec4(pos, 1.0);
#ifdef CLEAN_VS_NORMAL_FRAME
    v_world_normal = nrm;   // already world-space from the pre-skin pass
#endif
#if !defined(CLEAN_DEPTH_ONLY) || defined(CLEAN_DEPTH_MASK)
    // Planar texgen needs object-local positions (the pre-skinned stream is
    // eye-relative), so rigged faces take the texcoord path; the anim still applies.
    // (depth variants: the alpha test's inputs, the same expressions)
    v_uv           = cleanFaceUV(inst.flags & ~64u,
                                 (inst.flags & 1024u) != 0u ? instances[gl_InstanceIndex].uv_matrix : vsiUv(inst),
                                 texcoord0, nrm, pos, inst_model, pc.u_cam_time.w);
    v_material_id  = inst.material_id;
    v_tint         = inst.tint;
    v_flags        = inst.flags;
    rezfxRigOutputs(inst.flags, rz_src, v_rez, v_rez_box);   // <GMDX>
#endif
#ifndef CLEAN_DEPTH_ONLY
    v_cel          = celVertexWord(inst.flags, inst.skin_id, true, false);
#ifdef CLEAN_VS_PICK
    v_inst         = uint(gl_InstanceIndex);
#endif
#endif
#ifdef CLEAN_VS_WORLD_POS
    v_world_pos    = pos + eye;
#endif
#ifdef CLEAN_VS_PREV_POS
    v_prev_world_pos = prev_world;
#endif
#ifdef CLEAN_VS_NORMAL_FRAME
    v_uv_raw        = texcoord0;
#ifndef CLEAN_DEPTH_ONLY
    // #783 M3 SNOW ON AVATARS (clean_weather_surface.glsl weatherSnow): the wearer's joint-table
    // slot -- the skin cache's state word when it holds one (pelvis-local rows), else the skin
    // meta's avatar_idx (binding 38: valid whenever the skin meta exists, CEL_META). Flexi
    // chains (bit19) sit in pseudo slots: none.
    {
        uint av = 0xFFFFFFFFu;
        // (#901: also while the avatar shelter maps run -- in rain without snow the M3 rows do
        // not exist, and without its slot a rigged body was never sheltered by its umbrella)
        if ((ssr.weather[6].z > 0.5 || ssr.weather[6].w > 0.5) && (inst.flags & 524288u) == 0u)   // (weather off: no load)
        {
            if ((rs & RC_ST_LOCAL) != 0u) av = rs & RC_ST_SLOT;
            else if ((cel_flags & CEL_META) != 0u && inst.skin_id < 0x100000u
                     && inst.skin_id * CEL_SKIN_META_WORDS < uint(cel_skin_meta.length()))
                av = cel_skin_meta[inst.skin_id * CEL_SKIN_META_WORDS];
        }
        v_terrain_rec = av < AV_SLOT_CAP ? (0x80000000u | av) : 0xFFFFFFFFu;
        // #901 REST-POSE ACCUMULATION (ssr.weather[14].w bit 8: the grids live): the bind pose of
        // the SOURCE vertex this one was skinned from (the texcoord's, above) and its normal.
        v_wx_rest = vec4(0.0, 0.0, 0.0, -2.0);
        if (av < AV_SLOT_CAP && (uint(ssr.weather[14].w + 0.5) & 8u) != 0u && (cel_flags & CEL_META) != 0u
            && inst.skin_id < 0x100000u && (inst.skin_id + 1u) * CEL_SKIN_META_WORDS <= uint(cel_skin_meta.length()))
        {
            const uint sbase = rc_ok ? rig_cache[rcb + RC_SRC] : 0xFFFFFFFFu;
            const uint src = sbase + uint(gl_VertexIndex - gl_BaseVertexARB);
            if (sbase != 0xFFFFFFFFu && (src + 1u) * QV_WORDS <= uint(qv_words.length()))
            {
                const uint w = inst.skin_id * CEL_SKIN_META_WORDS + 1876u;   // (SkinMeta::bind_shape)
                mat4 BS;
                for (int c = 0; c < 4; ++c)
                    BS[c] = vec4(uintBitsToFloat(cel_skin_meta[w + 4u * uint(c)]), uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 1u]),
                                 uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 2u]), uintBitsToFloat(cel_skin_meta[w + 4u * uint(c) + 3u]));
                const QvHeader qh = QV_LOAD_HEADER(qv_words, max(sbase, QV_HEADER_VERTS));
                const vec3 rp = (BS * vec4(qvPos(qh, qv_words[src * QV_WORDS], qv_words[src * QV_WORDS + 1u]), 1.0)).xyz;
                const vec3 rn = wxAcCofactor(mat3(BS)) * qvNormal(qv_words[src * QV_WORDS + 3u]);
                if (!any(isnan(rp)) && !any(isinf(rp)) && dot(rn, rn) > 1e-20)
                    v_wx_rest = vec4(rp, normalize(rn).z);
            }
        }
    }
#endif
    const mat4 nuvm = matMapOwnUV(inst.material_id, MAP_NORMAL) ? matMapUVMatrix(inst.material_id, MAP_NORMAL)
                                                                  : vsiUv(inst);
    v_world_tangent = cleanFaceTangent(inst.flags & ~64u, nuvm, nrm, tng);
#endif
#ifdef CLEAN_VS_PICK
    v_pick_st       = texcoord0;
    v_pick_tangent  = (dot(tng.xyz, tng.xyz) > 1e-16) ? vec4(normalize(tng.xyz), tng.w) : vec4(0.0);
#endif   // CLEAN_VS_PICK
}
