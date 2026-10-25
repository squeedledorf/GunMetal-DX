// vkclean — GENERIC SHADOW-RAY CORE (#85 / #869 Phase 1; RenderVKShadowGenericCore): ONE
// light-type-agnostic visibility routine for every ray-traced light sample of a receiver -- the
// sun / moon's soft disc, a projector pick, and (Phase 5, ReSTIR DI) a reservoir's final sample
// or a point / area light -- so the merged RT shadow pass (clean_deferred_light.comp pass 15,
// CLEAN_SHADOW_CORE) inlines ONE shadow walk instead of a copy per light type and per ray kind.
// Included by pass 15 only (the RT shadow mask and the lit FS keep clean_shadow_rt.glsl's cores
// until they move over); needs clean_rt_common, clean_shadow_rt (projIGN, rtReceiverOrigin,
// rtSetShadowFootprint, the terminator globals, rtHintDone / rtAdaptiveDone, rtFol*) first.
//
// THE PIECES:
//   * ShadowRecv: the light-independent receiver (P, visible-side Ng, camera, render pixel,
//     pixel footprint, view-slope offset) and its lazily probed #747 cluster-rig lift --
//     computed once per texel, shared by every sample.
//   * shadowRecvLight: per (receiver, light) the #664 terminator band + lift (the probe is cached
//     per receiver: rtTerminatorCached), the facet the origin leaves, and the #747 rig lift +
//     slope offset along the light-facing side -- the RT shadow mask's rules, now for EVERY light
//     type (the merged pass gave projector rays none of them).
//   * ShadowSample: everything a visibility ray needs from the light -- a direction (directional)
//     or an emitter point (positional) with its disc radius, the projector's near-plane clip,
//     the extra origin bias, the disc ray count and the noise phase. No light-type code paths in
//     the trace: only data and a few flags.
//   * shadowSampleVis: the soft-disc loop (outermost Vogel samples first, the adaptive / hint
//     early-outs, the terminator-safe floor), then the translucent split ray, then the #735
//     foliage transmission ray -- every ray through ONE walk call (traceShadowWalkG), so the
//     candidate alpha test, the self-shadow probe and the facing rules are inlined once.
// Per-sample globals (clean_rt_common / clean_shadow_rt): the caller resets rt_block_t (and
// rt_block_dyn) before a sample and reads them after; shadowRecvLight sets rt_term_* and
// rt_cl_lift, the caller clears them after the sample (shadowRecvLightEnd).
#ifndef CLEAN_SHADOW_CORE_GLSL
#define CLEAN_SHADOW_CORE_GLSL

// ---- THE WALK: traceShadowWalk (clean_rt_common.glsl) + two more kinds ----
// RT_WALK_OPAQUE: traceShadowRayOpaque -- every instance force-opaque (no candidate, no alpha
// test), except a receiver that may sit on a no-self-shadow character, whose rays drop the
// opaque flag and confirm every candidate but the receiver's own (rtSelfCandidate).
// RT_WALK_FOL(_RGB): traceFoliageTransmit -- the #735 mode-2 transmission ray over the
// alpha-mask class, light-facing faces culled, coloured by the first rt_fol_layers leaves.
// Same rules and results as those functions; one inlined body for all of them.
#define RT_WALK_OPAQUE  5u
#define RT_WALK_FOL     6u
#define RT_WALK_FOL_RGB 7u
// ---- OWN-FIXTURE RULE (#439 / #869: a POINT light's own lamp; the sample's SH_FIXTURE) ----
// SL point lights sit inside their fixtures (a bulb in a shade, a lantern, a sconce): traced
// plainly, the fixture shadows its own light and the lamp goes dark. A caster instance whose own
// box (object space) holds the light's centre and whose world size is at most rt_fix_max is the
// light's FIXTURE: its translucent faces do not tint the ray, and when an opaque / alpha-tested
// face of it ends the ray, the ray is traced again up to where it ENTERS that box (rt_fix_t) --
// everything between the receiver and the lamp still shadows, nothing inside the lamp does. Not
// an infinite near plane: the clip is the fixture's own bounds, and only a small enclosing box
// qualifies (a room-sized mesh around the light never does). Set per sample by the caller.
bool  rt_fix_on  = false;
vec3  rt_fix_P   = vec3(0.0);   // the light's centre (absolute world)
float rt_fix_max = 0.0;         // the largest fixture box edge (m; RenderVKReSTIRFixtureSize)
float rt_fix_t   = -1.0;        // out: the committed caster was the fixture -- its box entry t
bool rtFixtureBox(uint id, mat4x3 w2o, mat4x3 o2w, out vec3 a, out vec3 b)
{
    a = vec3(0.0); b = vec3(0.0);
    if (!rtIdIn(id)) return false;
    a = rt_instances[id].aabb_min.xyz;
    b = rt_instances[id].aabb_max.xyz;
    const vec3 lo = w2o * vec4(rt_fix_P, 1.0);
    if (!(all(greaterThanEqual(lo, a)) && all(lessThanEqual(lo, b)))) return false;
    const vec3 e = abs(b - a) * vec3(length(o2w[0]), length(o2w[1]), length(o2w[2]));
    return max(max(e.x, e.y), e.z) <= rt_fix_max;
}
vec3 traceShadowWalkG(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind)
{
    const bool fol_walk = kind >= RT_WALK_FOL;
    const bool trans_walk = kind == RT_WALK_TRANS || kind == RT_WALK_TRANS_RGB;
    const bool opaque_walk = kind == RT_WALK_OPAQUE;
    const bool rgb = kind == RT_WALK_RGB || kind == RT_WALK_TRANS_RGB || kind == RT_WALK_FOL_RGB;
    uint flags;
    if (fol_walk)
        flags = gl_RayFlagsNoOpaqueEXT | (rt_fol_flip ? gl_RayFlagsCullFrontFacingTrianglesEXT
                                                      : gl_RayFlagsCullBackFacingTrianglesEXT);
    else if (opaque_walk)
        flags = gl_RayFlagsTerminateOnFirstHitEXT | (rt_self_owner != 0u ? gl_RayFlagsNoneEXT : gl_RayFlagsOpaqueEXT)
              | rtFolShadowFlags();
    else
        flags = gl_RayFlagsTerminateOnFirstHitEXT | (trans_walk ? gl_RayFlagsNoneEXT : rtFolShadowFlags());
    // (Xid 109, rtRaySane: an invalid ray is blocked as G-7; the foliage walk keeps no colour)
    if (!rtRaySane(origin, tmin, dir, tmax)) return fol_walk ? vec3(1.0) : vec3(0.0);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, flags, mask & 0xFFu, origin, tmin, dir, tmax);
    vec3 trans = vec3(1.0);
    vec3 fa, fb;   // (rtFixtureBox's box)
    uint n_cand = 0u;
    uint n_fol = 0u;
    const uint budget = fol_walk ? RT_CANDIDATE_BUDGET_NOOPAQUE : RT_CANDIDATE_BUDGET;
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        if (++n_cand > budget)
        {
            // over budget (G-7): the shadow walks treat it as opaque; the foliage ray keeps the
            // colour gathered so far (visibility is the shadow rays')
            if (!fol_walk) rayQueryConfirmIntersectionEXT(rq);
            rayQueryTerminateEXT(rq);
            break;
        }
        const uint id = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                      uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
        uint fol = RT_FOL_OCCLUDE;
        if (fol_walk)
        {
            if (!rtIdIn(id) || (rt_instances[id].flags & 256u) != 0u) continue;   // double-sided: an occluder (/ #874 guard)
        }
        else
        {
            const uint self_c = rtSelfCandidate(RT_CAND_OWNER(rq), id);
            if (self_c == 1u) continue;
            if (self_c == 2u || opaque_walk) { rayQueryConfirmIntersectionEXT(rq); continue; }
            fol = rtFolAway(id, rayQueryGetIntersectionFrontFaceEXT(rq, false));
            if (fol == RT_FOL_PASS) continue;
        }
        const uint prim = RT_HIT_PRIM(rq, false, id);
        const vec2 bc   = rayQueryGetIntersectionBarycentricsEXT(rq, false);
        const float t   = rayQueryGetIntersectionTEXT(rq, false);
        const mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, false);
        float cutoff; bool blend; vec3 col;
        const float a = rtCandidateAlphaCol(id, prim, bc, t, dir, o2w, cutoff, blend, col);   // (the ONE alpha test)
        if (fol_walk)
        {
            if (a < cutoff) continue;   // a hole in the leaf
            trans *= rgb ? rtFolLayer(col) : vec3(rt_fol_strength);
            rt_block_t = min(rt_block_t, t);
            RT_NOTE_BLOCKER(id);
            if (++n_fol >= rt_fol_layers || max(max(trans.r, trans.g), trans.b) < 0.01)
            {
                rayQueryTerminateEXT(rq);
                break;
            }
            continue;
        }
        if (kind == RT_WALK_VIS)
        {
            if (a >= cutoff && fol == RT_FOL_OCCLUDE)   // #735
                rayQueryConfirmIntersectionEXT(rq);
        }
        else if ((blend ? a >= RT_BLEND_SOLID : a >= cutoff) && fol == RT_FOL_LEAF)
        {
            if (trans_walk && rt_fol_mode == 2u && n_fol < rt_fol_layers)
            {
                trans *= rgb ? rtFolLayer(col) : vec3(rt_fol_strength);
                ++n_fol;
                rt_block_t = min(rt_block_t, t);
                if (max(max(trans.r, trans.g), trans.b) < 0.01) rayQueryConfirmIntersectionEXT(rq);
            }
        }
        else if (blend && rt_fix_on && rtFixtureBox(id, rayQueryGetIntersectionWorldToObjectEXT(rq, false), o2w, fa, fb)) {}
                                                   // (the light's own translucent shade: no tint)
        else if (blend && rt_blend_parity && a >= 0.88)
            rayQueryConfirmIntersectionEXT(rq);   // LL: solid caster
        else if (blend && !(rt_blend_parity && a < 0.05))
        {
            trans *= rgb ? translucentTransmit(col, a) : vec3(1.0 - clamp(a, 0.0, 1.0));
            if (!trans_walk && a > 0.004) { rt_block_t = min(rt_block_t, t); RT_NOTE_BLOCKER(id); }
            if (max(max(trans.r, trans.g), trans.b) < 0.01) rayQueryConfirmIntersectionEXT(rq);
        }
        else if (blend) {}                         // LL: casts nothing
        else if (a >= cutoff)
            rayQueryConfirmIntersectionEXT(rq);
    }
    if (fol_walk) return trans;
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return trans;
    if (rt_fix_on && !trans_walk)
    {
        // OWN FIXTURE: the committed caster's box entry (the slab test in its object space: the
        // ray parameter is the same through the affine map); the caller re-traces up to it
        const uint fid = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                       uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)));
        const mat4x3 w2o = rayQueryGetIntersectionWorldToObjectEXT(rq, true);
        if (rtFixtureBox(fid, w2o, rayQueryGetIntersectionObjectToWorldEXT(rq, true), fa, fb))
        {
            const vec3 oo = w2o * vec4(origin, 1.0);
            const vec3 od = w2o * vec4(dir, 0.0);
            const vec3 inv = vec3(1.0) / mix(od, vec3(1e-20), lessThan(abs(od), vec3(1e-20)));
            const vec3 t0 = (fa - oo) * inv, t1 = (fb - oo) * inv;
            const vec3 tn = min(t0, t1);
            rt_fix_t = max(max(max(tn.x, tn.y), tn.z), 0.0);
            return vec3(0.0);
        }
    }
    if (!trans_walk)
    {
        rt_block_t = min(rt_block_t, rayQueryGetIntersectionTEXT(rq, true));
        RT_NOTE_COMMITTED(rq);
    }
    return vec3(0.0);
}

// ---- THE RECEIVER (light-independent; once per texel) ----
struct ShadowRecv {
    vec3  P;       // the receiver (absolute world, as the TLAS)
    vec3  Ng;      // its geometric normal on the VISIBLE side (the G-buffer's, flipped to the eye)
    vec3  cam;     // the camera that sees it
    ivec2 rp;      // its render pixel (the #664 pre-pass triangle; < 0: none)
    float pxw;     // world size of one render pixel at P
    float slope;   // the view-slope origin offset (m; RenderVKRTShadowSlopeBias x pxw x sin(view, Ng))
};
// #747 cluster-rig lift (pdnClusterRigLift): one probe per receiver, run by the first light whose
// facing side needs it (< 0: not probed yet).
float g_shc_cl = -1.0;
float shadowRecvClLift(ShadowRecv R)
{
    if (g_shc_cl < 0.0) g_shc_cl = pdnClusterRigLift(R.P, R.cam);
    return g_shc_cl;
}
// #664 TERMINATOR TRIANGLE of the receiver (rtTerminatorCached, clean_shadow_rt.glsl, with ONE
// escape-check site): the pre-pass triangle (rtTerminatorTriAt) or the camera probe
// (rtTerminatorProbe), their lift escape-checked once after either. Same results, same cache
// (rt_tc_*: keyed by P) and the same globals for rtTerminatorPost.
bool shadowTermTri(ivec2 px, vec3 P, out vec3 lift, out vec3 ns, out vec3 ng)
{
    // rtTerminatorTriAt without its escape check (the raw lift ng x h)
    lift = vec3(0.0); ns = vec3(0.0); ng = vec3(0.0);
    rt_tri_dev = 0.0; rt_esc_flag = 0.0; rt_lift_capped = false;
    if (px.x < 0 || !rtTermTriBound()) return false;
    const ivec2 sz = textureSize(uTermTri, 0);
    if (any(greaterThanEqual(px, sz))) return false;
    const uvec2 t = texelFetch(uTermTri, px, 0).xy;
    if ((t.y & 0x8000u) == 0u) return false;
    ng = rtOctDecode(unpackSnorm2x16(t.x));
    ns = rtOctDecode(vec2(float((t.y >> 16u) & 255u), float(t.y >> 24u)) * (2.0 / 255.0) - 1.0);
    const float h = unpackHalf2x16(t.y & 0x7FFFu).x;
    if (h > 0.0) lift = ng * h;
    return true;
}
int shadowTermProbe(vec3 P, vec3 cam, vec3 ng_ref, float min_cos, out vec3 lift, out vec3 ns, out vec3 ng)
{
    // rtTerminatorProbe without its escape check
    lift = vec3(0.0); ns = vec3(0.0); ng = vec3(0.0);
    rt_tri_dev = 0.0; rt_esc_flag = 0.0; rt_lift_capped = false;
    vec3 V = P - cam;
    const float camd = length(V);
    if (camd < 1e-4) return 0;
    V /= camd;
    const float pm = max(max(abs(P.x), abs(P.y)), abs(P.z));
    const float w = max(4.0 * (camd * RT_DEPTH_REL_ERR + max(RT_OFS_FRAME_ULPS * rtUlp(pm), RT_OFS_FLOAT_SCALE)), 1e-3);
    vec3 qo = P - V * w;
    float qt0 = 0.0, qt1 = 2.0 * w;
    if (!rtRaySane(qo, qt0, V, qt1)) return 0;   // (Xid 109: an invalid probe ray -- no triangle)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, gl_RayFlagsOpaqueEXT, RT_MASK_SHADOW & 0xFFu, qo, qt0, V, qt1);
    while (rayQueryProceedEXT(rq)) {}
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return 0;
    const uint id   = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                    uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)));
    const uint prim = RT_HIT_PRIM(rq, true, id);
    const vec2 bb   = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
    const float th  = rayQueryGetIntersectionTEXT(rq, true);
    vec3 Ph;
    if (!rtTerminatorTri(id, prim, vec3(1.0 - bb.x - bb.y, bb.x, bb.y), o2w, cam - P, lift, ns, ng, Ph))
        return 0;
    bool ok = abs(th - w) <= 0.5 * w;
    if (ok && dot(ng_ref, ng_ref) > 0.5)
    {
        const vec3 r = normalize(ng_ref) * (dot(ng_ref, cam - P) < 0.0 ? -1.0 : 1.0);   // visible side
        ok = dot(r, ng) >= min_cos;
    }
    if (!ok) { lift = vec3(0.0); ns = vec3(0.0); ng = vec3(0.0); rt_lift_capped = false; return 2; }
    return 1;
}
bool shadowTermCached(vec3 P, vec3 cam, vec3 ng_ref, float min_cos, ivec2 px, out vec3 lift, out vec3 ns, out vec3 ng)
{
    if (rt_tc_state < 0 || any(notEqual(rt_tc_P, P)))
    {
        rt_tc_tri = shadowTermTri(px, P, rt_tc_lift, rt_tc_ns, rt_tc_ng);
        rt_tc_state = rt_tc_tri ? 1 : shadowTermProbe(P, cam, ng_ref, min_cos, rt_tc_lift, rt_tc_ns, rt_tc_ng);
        if (rt_tc_state == 1)   // (the one escape check: either source's lift)
            rt_tc_lift = rtTerminatorEscape(P, rtReceiverOrigin(P, rt_tc_ng, length(P - cam), 0.0), rt_tc_ng, rt_tc_lift);
        rt_tc_dev = rt_tri_dev; rt_tc_esc = rt_esc_flag; rt_tc_cap = rt_lift_capped;
        rt_tc_P = P;
    }
    lift = rt_tc_lift; ns = rt_tc_ns; ng = rt_tc_ng;
    rt_term_dev = rt_tc_dev; rt_term_esc = rt_tc_esc; rt_term_ng = rt_tc_ng;
    rt_term_rej = rt_tc_state == 2; rt_term_cap = rt_tc_cap; rt_term_src_tri = rt_tc_tri;
    return rt_tc_state == 1;
}

// Per (receiver, light): the RT shadow mask's receiver rules (clean_rt_shadow_mask.comp, sun and
// projectors alike). l: unit direction to the light's centre from R.P. Sets the core's
// terminator globals (rt_term_on / _lift / _ns) and rt_cl_lift; N: the facet the origin leaves
// (the probed triangle in the band, else the G-buffer's). Returns the facing measure the light's
// gate uses: the smooth (vertex) normal's or the facet's N.l, whichever is larger, when probed.
float shadowRecvLight(ShadowRecv R, vec3 l, out vec3 N)
{
    const float ngl = dot(R.Ng, l);
    vec3 tlift = vec3(0.0), tns = vec3(0.0), tng = vec3(0.0);
    const bool term = rtTerminatorOn() && rtTerminatorBand(ngl)
                   && shadowTermCached(R.P, R.cam, R.Ng, RT_TERM_MASK_COS, R.rp, tlift, tns, tng);
    N = term ? tng : R.Ng;
    rt_term_on = term; rt_term_lift = tlift; rt_term_ns = tns;
    // (#747 + the slope offset: only along the receiver's VISIBLE side -- the core lifts along the
    // light-facing side unless terminator-lifted, and the light-facing side of a receiver turned
    // away is the inside of its mesh)
    rt_cl_lift = (dot(N, l) > 0.0 || (term && dot(tlift, tlift) > 0.0)) ? shadowRecvClLift(R) + R.slope : 0.0;
    return term ? max(dot(tns, l), dot(tng, l)) : ngl;
}
void shadowRecvLightEnd()
{
    rt_term_on = false;
    rt_cl_lift = 0.0;
}

// ---- THE LIGHT SAMPLE ----
#define SH_POSITIONAL  1u   // pos is a point on the emitter (a projector apex), else a unit direction
#define SH_OPAQUE_ONLY 2u   // the opaque caster set through the force-opaque walk (the sun past
                            // RenderVKRTSunAlphaDist, transparent receivers)
#define SH_FOLIAGE     4u   // the sun / moon's foliage rules (#735: facing culls, the transmission ray)
#define SH_SPLIT       8u   // the translucent split (RenderVKRTTranslucentSplit) for this sample
#define SH_SPHERE     16u   // POSITIONAL without a near plane (a point light): the disc faces the
                            // receiver and every ray stops nearClip short of the centre (the light's
                            // own prim bounds: its bulb never shadows it)
#define SH_FIXTURE    32u   // the own-fixture rule (rt_fix_*, the caller sets rt_fix_max)
struct ShadowSample {
    vec3  pos;       // DIRECTIONAL: unit direction to the light; POSITIONAL: the emitter centre
    float radius;    // the disc: DIRECTIONAL tan(angular radius), POSITIONAL metres; <= 0 hard
    vec3  axis;      // POSITIONAL: the disc's normal and the near-plane axis (unused: directional)
    float nearClip;  // POSITIONAL: the near plane's distance from pos along axis (rays stop there)
    float bias;      // the user's extra origin offset (m): RenderVKRTShadowBias / the record's
    float tmax;      // DIRECTIONAL: the ray length (positional: to the emitter / near plane)
    uint  flags;     // SH_*
    uint  nRays;     // disc rays (the adaptive early-outs inside); 1 = one stochastic disc sample
    vec2  noise;     // the Vogel pattern's radius offset [0, 1) and rotation (radians)
};

// The visibility (x translucent / foliage tint) of receiver P (geometric normal Nin: the facet
// shadowRecvLight chose) toward light sample S. The caller set rt_term_* / rt_cl_lift
// (shadowRecvLight), rt_block_t, rt_noise_shift; the sample's rays share ONE walk call site.
// Exactly the former cores' arithmetic: sunShadowRayCore (directional) / projShadowRayCore
// (positional) of clean_shadow_rt.glsl.
vec3 shadowSampleVis(ShadowSample S, vec3 P, vec3 Nin, vec3 cam, float time)
{
    const bool pos = (S.flags & SH_POSITIONAL) != 0u;
    const bool oo = (S.flags & SH_OPAQUE_ONLY) != 0u;
    const bool fol = (S.flags & SH_FOLIAGE) != 0u;
    const bool split = (S.flags & SH_SPLIT) != 0u;
    const bool sphere = (S.flags & SH_SPHERE) != 0u;
    rt_fix_on = (S.flags & SH_FIXTURE) != 0u;
    rt_fix_P = S.pos;
    rtSetShadowFootprint(P, Nin, cam);
    if (fol) rtFolBegin();
    const float camd = length(P - cam);
    rt_anim_time   = time;
    rt_cone_spread = 0.0;
    rt_cone_width0 = camd * 0.001;   // (fallback only: rtSetShadowFootprint's rt_foot_* override it)
    rt_cone_aniso  = 1.0;
    // TERMINATOR (#664): a lifted origin stays on the VISIBLE side (above the facet, on the
    // surface the vertex normals describe); unlifted, each ray leaves the light-facing side.
    const bool lifted = rt_term_on && dot(rt_term_lift, rt_term_lift) > 0.0;
    const vec3 Pb = lifted ? P + rt_term_lift : P;
    const vec3 L = S.pos;               // (directional)
    const vec3 toO = S.pos - Pb;        // (positional: to the emitter centre)
    const float dO = length(toO);
    // BACK-FACE SELF-SHADOW (clean_shadow_rt.glsl rtBackfaceSelfShadow): unlifted, every origin
    // leaves along the VISIBLE side of the facet (Nv), and a facet turned away from the light's
    // centre is shadowed by its own surface -- no ray (the old light-side flip pushed the origin
    // through single-plane walls: the next room's projector lit this side).
    const bool bfs = !lifted && rtBackfaceSelfShadow();
    const vec3 Nv = rtVisibleSide(Nin, P, cam);
    const bool selfShadowed = bfs && !(dot(Nv, pos ? toO : L) > 0.0);
    // the disc frame's normal (a point light: its disc faces the receiver)
    const vec3 A = pos ? (sphere ? (dO > 1e-4 ? toO / dO : vec3(0.0, 0.0, 1.0)) : S.axis) : L;
    const bool hard = !(S.radius > 0.0);
    const uint n = max(S.nRays, 1u);
    // the disc floor's frame: the shading surface when known (RenderVKRTShadowTerminatorFloor)
    vec3 Nf = rt_term_on ? rt_term_ns : Nin;
    if (dot(Nf, pos ? toO : L) < 0.0) Nf = -Nf;
    float efloor = -2.0;
    if (!hard)
    {
        if (pos) { if (dO > 1e-4) efloor = min(clamp(dot(Nf, toO) / dO, 0.0, 1.0), ssr.taa_scale.w * S.radius / dO); }
        else efloor = min(clamp(dot(Nf, L), 0.0, 1.0), ssr.taa_scale.w * S.radius);
    }
    vec3 u = vec3(0.0), v = vec3(0.0);
    if (!hard || !pos)   // (the sun's frame is always built: unused when hard)
    {
        u = normalize(abs(A.z) < 0.9 ? cross(A, vec3(0.0, 0.0, 1.0)) : cross(A, vec3(1.0, 0.0, 0.0)));
        v = cross(A, u);
    }
    const uint optsB = rtBlendOpts();
    const bool discVisSet = split || oo;   // binary opaque-set disc rays
    const uint discMask = discVisSet ? (RT_MASK_SHADOW & ~RT_MASK_BLEND) : RT_MASK_SHADOW;
    const uint discKind = (oo && !rt_opaque_cutouts) ? RT_WALK_OPAQUE
                        : (discVisSet ? RT_WALK_VIS : (specTransOn() ? RT_WALK_RGB : RT_WALK_GREY));
    const bool discParity = !discVisSet && (optsB & 1u) != 0u;

    vec3 vis = vec3(0.0), first = vec3(0.0);
    float rays = float(n);   // the normaliser: rays actually traced (early-outs below)
    uint k = 0u;
    uint phase = selfShadowed ? 3u : 0u;   // 0 the disc, 1 the translucent ray, 2 the foliage ray, 3 done
                                           // (self-shadowed: vis 0, no ray)
    [[dont_unroll]] for (uint it = 0u; it < n + 2u && phase < 3u; ++it)
    {
        vec3 o = vec3(0.0), d = vec3(0.0);
        float tm = 0.0;
        uint kind = 0u, mask = 0u;
        bool par = false, want = false;
        if (phase == 0u)
        {
            bool done = k >= n;
            if (!done && rtHintDone(k, vis)) { rays = 1.0; done = true; }
            if (!done && k == 1u) first = vis;
            if (!done && rtAdaptiveDone(k, n, vis, first)) { rays = 2.0; done = true; }
            if (done)
            {
                vis /= rays;
                phase = 1u;   // (falls through to the translucent ray in this iteration)
            }
            else
            {
                const uint i = n - 1u - k;   // outermost samples first (rtAdaptiveDone)
                ++k;
                vec3 off = vec3(0.0);
                if (!hard)
                {
                    const float r   = S.radius * sqrt((float(i) + S.noise.x) / float(n));
                    const float phi = S.noise.y + 2.3999632 * float(i);
                    off = (u * cos(phi) + v * sin(phi)) * r;
                }
                if (pos)
                {
                    const vec3 T = S.pos + off;
                    vec3 Nr = Nin;
                    if (lifted) { if (dot(Nr, rt_term_lift) < 0.0) Nr = -Nr; }
                    else if (bfs) Nr = Nv;
                    else if (dot(Nr, T - Pb) < 0.0) Nr = -Nr;
                    o = rtReceiverOrigin(Pb, Nr, camd, S.bias + rt_cl_lift);
                    const vec3 toL = T - o;
                    const float len = length(toL);
                    if (len <= 1e-4) { vis += vec3(1.0); continue; }
                    d = toL / len;
                    const float e = dot(Nf, d);
                    if (e < efloor) d = normalize(d + Nf * (efloor - e));
                    // the near plane (dot(X - O, A) = near): the fixture never shadows its own light;
                    // a point light: its own prim's bounding sphere (SH_SPHERE)
                    if (sphere) tm = len - S.nearClip - 0.002;
                    else
                    {
                        const float da = dot(d, S.axis);
                        const float dp = dot(o - S.pos, S.axis);
                        tm = min((da < -1e-6) ? (S.nearClip - dp) / da : len, len) - 0.002;
                    }
                    if (tm <= 0.0) { vis += vec3(1.0); continue; }
                }
                else
                {
                    vec3 Nr = Nin;
                    if (lifted) { if (dot(Nr, rt_term_lift) < 0.0) Nr = -Nr; }
                    else if (bfs) Nr = Nv;
                    else if (dot(Nr, L) < 0.0) Nr = -Nr;
                    o = rtReceiverOrigin(Pb, Nr, camd, S.bias + rt_cl_lift);
                    d = L;
                    if (!hard)
                    {
                        d = normalize(L + off);
                        const float e = dot(Nf, d);
                        if (e < efloor) d = normalize(d + Nf * (efloor - e));
                    }
                    tm = S.tmax;
                }
                kind = discKind; mask = discMask; par = discParity;
                want = true;
            }
        }
        if (phase == 1u && !want)
        {
            // TRANSLUCENT SPLIT: one ray toward the light's centre over the blend class (same
            // origin rules and near-plane clip as a disc ray; no disc floor), where any light is left
            phase = 2u;
            if (split && any(greaterThan(vis, vec3(0.0))))
            {
                vec3 Nr = Nin;
                if (lifted) { if (dot(Nr, rt_term_lift) < 0.0) Nr = -Nr; }
                else if (bfs) Nr = Nv;
                else if (dot(Nr, pos ? toO : L) < 0.0) Nr = -Nr;
                o = rtReceiverOrigin(Pb, Nr, camd, S.bias + rt_cl_lift);
                if (pos)
                {
                    const vec3 toL = S.pos - o;
                    const float len = length(toL);
                    if (len > 1e-4)
                    {
                        d = toL / len;
                        if (sphere) tm = len - S.nearClip - 0.002;
                        else
                        {
                            const float da = dot(d, S.axis);
                            const float dp = dot(o - S.pos, S.axis);
                            tm = min((da < -1e-6) ? (S.nearClip - dp) / da : len, len) - 0.002;
                        }
                        want = tm > 0.0;
                    }
                }
                else { d = L; tm = S.tmax; want = true; }
                kind = specTransOn() ? RT_WALK_TRANS_RGB : RT_WALK_TRANS;
                mask = RT_MASK_BLEND;
                par = (optsB & 1u) != 0u;
            }
            if (!want) continue;
        }
        else if (phase == 2u && !want)
        {
            // FOLIAGE TRANSMISSION (#735 mode 2; the sun / moon's full trace only): one ray along
            // the centre over the alpha-mask class, where the shadow rays left some light
            phase = 3u;
            if (!fol || oo || rt_fol_mode != 2u || !(max(max(vis.r, vis.g), vis.b) > 0.0)) continue;
            vec3 Nr = Nin;
            if (lifted) { if (dot(Nr, rt_term_lift) < 0.0) Nr = -Nr; }
            else if (bfs) Nr = Nv;
            else if (dot(Nr, L) < 0.0) Nr = -Nr;
            o = rtReceiverOrigin(Pb, Nr, camd, S.bias + rt_cl_lift);
            d = L; tm = S.tmax;
            kind = specTransOn() ? RT_WALK_FOL_RGB : RT_WALK_FOL;
            mask = RT_MASK_ALPHAMASK;
            want = true;
        }
        if (!want) continue;
        rt_blend_parity = par;
        vec3 w = vec3(1.0);
        // (SH_FIXTURE: a ray ended by the light's own fixture is traced again up to the fixture's box
        //  -- at most twice, nested lamps; the same call site)
        float tmc = tm;
        [[dont_unroll]] for (uint fr = 0u; fr < 3u; ++fr)
        {
            rt_fix_t = -1.0;
            w = traceShadowWalkG(o, d, 0.0, tmc, mask, kind);   // THE walk site
            if (!(rt_fix_t >= 0.0)) break;
            tmc = min(rt_fix_t, tmc) - 0.002;
            w = vec3(1.0);   // (nothing before the fixture so far: lit unless the next pass meets one)
            if (tmc <= 0.0) break;
        }
        rt_blend_parity = false;
        if (kind == RT_WALK_OPAQUE || kind == RT_WALK_VIS || kind == RT_WALK_GREY || kind == RT_WALK_RGB) vis += w;
        else vis *= w;
    }
    if (phase == 0u) vis /= rays;   // (never: the loop bound covers the disc + both tails)
    rt_fix_on = false;
    if (fol) rtFolEnd();
    rt_foot_on = false;
    const vec3 r = rtTerminatorPost(vis, pos ? normalize(S.pos - P) : L);
    rt_term_rej = false;
    return r;
}

#endif
