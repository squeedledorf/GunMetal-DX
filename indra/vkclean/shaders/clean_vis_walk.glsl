// vkclean -- THE SHADOW / VISIBILITY WALK LIBRARY (#1039 M0): the ONE any-hit walk every ray-traced
// visibility consumer calls -- the lit FS's sun / projector cores and the RT shadow mask
// (clean_shadow_rt.glsl), pass 15's generic core (clean_shadow_core.glsl), the RT hit shading
// (clean_rt_shade.glsl), RTAO, god rays, the volumetric fog, the GI reuse and the visibility queue's
// walk kernel (clean_deferred_light.comp VIS WALK). Before it there were two walk bodies that had
// drifted apart (traceShadowWalk here, traceShadowWalkG in the generic core: only the latter had the
// own-fixture rule and the opaque-first pre-ray) plus an inline fixture copy in the fog.
// Included by clean_rt_common.glsl (needs its candidate rules: rtCandidateAlphaCol, rtSelfCandidate,
// rtFol*, rtRaySane, rt_block_t). Kernel specialisations (SH_KIND_*, RT_WALK_OFIRST) are defined by
// the includer BEFORE clean_rt_common.glsl.
//
// One body serves every shadow-ray flavour; `kind` picks the candidate rules at run time (uniform per
// ray). It exists for COMPILE COST: glslc -O inlines every call, and the lit FS reaches these walks
// from many places (sun + projector cores, hard ray / disc / split tails, every lightSurface branch).
// Four separate walk functions, each inlining the whole candidate alpha test (rtCandidateAlphaCol: UV
// fetch, ray cone, texture sample -- ~7k SPIR-V instructions), made the ray-query lit FS ~870k words
// and a ~90 s / 1.7 GB driver compile per pipeline. A call site with a compile-time kind folds the
// other kinds' rules away; one with a run-time kind inlines one body for all of them.
//   RT_WALK_GREY / RT_WALK_RGB: the MIXED walk (traceShadowRay / traceShadowRayRGB).
//   RT_WALK_VIS: the opaque set, binary (traceShadowRayVis).
//   RT_WALK_TRANS / RT_WALK_TRANS_RGB: the translucent set (traceTransmittance).
//   RT_WALK_OPAQUE: every instance force-opaque (traceShadowRayOpaque).
//   RT_WALK_FOL / RT_WALK_FOL_RGB: the #735 foliage transmission ray (traceFoliageTransmit).
//
// Shadow / visibility ray (RT_WALK_GREY): 1.0 = unoccluded over [tmin, tmax], 0.0 = blocked.
// Double-sided (no facing cull). Alpha-mask / blend instances run the candidate
// alpha test; FORCE_OPAQUE instances (the vast majority) never produce candidates.
// Alpha-mask casters are binary (the raster's cutoff). Alpha-blend casters are
// TRANSLUCENT: each passes (1 - alpha) of the light, so a blended prim (light-beam
// cones, glow cards, animated-alpha effects) dims the light in proportion instead of
// switching it fully on/off as its alpha crosses a threshold (that flickered whole
// projectors on animated textures). Matches the shadow-map path's dithered blend.
// Coloured shadow ray (RT_WALK_RGB, #212): each alpha-blend caster passes
// translucentTransmit(colour, alpha) -- stained glass throws coloured light. Order
// doesn't matter (the product commutes); an opaque hit, or transmittance under 1%,
// ends the ray dark. (Grey = the same walk with (1 - alpha) in every channel.)
//
// TRANSLUCENT SPLIT (RenderVKRTTranslucentSplit; clean_shadow_rt.glsl): shadow
// visibility is two independent traversals instead of one mixed walk.
//   * RT_WALK_VIS: the OPAQUE set (callers pass a mask without RT_MASK_BLEND):
//     binary 0 / 1, alpha-mask cut-outs still alpha-tested (OMM where baked), blocker
//     distance recorded. No alpha-blend candidate ever reaches the shader, so a soft
//     shadow's disc rays stay 0 / 1 and the adaptive early-out settles.
//   * RT_WALK_TRANS(_RGB): the TRANSLUCENT set only (RT_MASK_BLEND), ONE ray per
//     receiver: the product of every blend caster's transmittance along it (order
//     doesn't matter), the same per-caster rules as the mixed walk. It never
//     touches rt_block_t: translucent casters do not size the denoiser's penumbra.
//     Honours rt_blend_parity / rt_blend_coarse like the mixed walks. A blend-class
//     instance whose material carries an alpha cutoff (rtCandidateAlphaCol: blend =
//     false) is a binary caster here, as in the mixed walk. Its foliage rule (#735):
//     an away-facing solid leaf passes in mode 1; mode 2 colours the light by the first
//     rt_fol_layers of them (the rest pass: the walk can't stop early -- light-facing
//     blend casters later in traversal order must still block). Their distance sizes
//     the denoiser's penumbra.
// Final visibility = opaque visibility x translucent transmittance.
#ifndef CLEAN_VIS_WALK_GLSL
#define CLEAN_VIS_WALK_GLSL

#extension GL_EXT_control_flow_attributes : require   // (rtVisTrace's [[dont_unroll]])

#define RT_WALK_GREY      0u
#define RT_WALK_RGB       1u
#define RT_WALK_VIS       2u
#define RT_WALK_TRANS     3u
#define RT_WALK_TRANS_RGB 4u
#define RT_WALK_OPAQUE    5u
#define RT_WALK_FOL       6u
#define RT_WALK_FOL_RGB   7u
// KIND SETS: what a call site knows at compile time about its kind -- the bit (1 << kind) of every kind it
// may pass. A caller with a run-time choice (the disc ray's split / mixed walk, traceTransmittance /
// traceFoliageTransmit's grey or coloured flavour) names its set, so the walk still folds every other
// kind's rules away -- a run-time kind alone would keep them all. RT_WALK_KS_ANY: the kind decides at run
// time (a constant kind still folds).
#define RT_WALK_KS_ANY    0xFFu
#define RT_WALK_KS_MIXED  ((1u << RT_WALK_GREY) | (1u << RT_WALK_RGB))
#define RT_WALK_KS_DISC   (RT_WALK_KS_MIXED | (1u << RT_WALK_VIS))   // a disc ray: the split's or the mixed walk
#define RT_WALK_KS_TRANS  ((1u << RT_WALK_TRANS) | (1u << RT_WALK_TRANS_RGB))
#define RT_WALK_KS_FOL    ((1u << RT_WALK_FOL) | (1u << RT_WALK_FOL_RGB))
// kind in `want` given that it is in `ks` (folded when ks decides it)
bool rtWalkIn(uint kind, uint ks, uint want)
{
    return (ks & want) == 0u ? false : ((ks & ~want) == 0u ? true : ((1u << kind) & want) != 0u);
}
bool rtWalkFol(uint kind, uint ks)
{
#ifdef SH_KIND_POSITIONAL
    return false;   // (local lights: no foliage transmission ray)
#else
    return rtWalkIn(kind, ks, RT_WALK_KS_FOL);
#endif
}
bool rtWalkTrans(uint kind, uint ks)  { return rtWalkIn(kind, ks, RT_WALK_KS_TRANS); }
bool rtWalkOpaque(uint kind, uint ks) { return rtWalkIn(kind, ks, 1u << RT_WALK_OPAQUE); }
bool rtWalkVisK(uint kind, uint ks)   { return rtWalkIn(kind, ks, 1u << RT_WALK_VIS); }
bool rtWalkRgb(uint kind) { return kind == RT_WALK_RGB || kind == RT_WALK_TRANS_RGB || kind == RT_WALK_FOL_RGB; }

// ---- (#1031) OPAQUE FIRST (RT_WALK_OFIRST: the includer maps it onto a dispatch-uniform input; pass 15
//      PDN_OPT_OFIRST, RenderVKShadowOpaqueFirst): a blocking walk first traces the opaque geometry alone
//      (hardware only, no candidate loop) and runs the candidate walk -- over the non-opaque geometry
//      only -- just when that ray comes back clear. The visibility is the same. What differs where an
//      opaque caster blocks: rt_block_t / the moving-caster note are that caster's (any-hit, as the
//      walk's own commit) -- translucent layers the walk might have met in front of it first
//      (traversal order) no longer shorten the penumbra estimate or flag a moving blocker. ----
#ifndef RT_WALK_OFIRST
#define RT_WALK_OFIRST false   // (#1031: read where used -- the includer maps it onto its push / UBO)
#endif
// ---- (#1031) KERNEL SPECIALISATION: an includer whose rays are all one light kind defines
//      SH_KIND_DIRECTIONAL (the sun / moon: no own-fixture code) or SH_KIND_POSITIONAL (local lights:
//      no foliage transmission walk) before clean_rt_common.glsl; the walk then compiles only those
//      paths -- the same results, fewer values live across it (clean_shadow_core.glsl compiles the
//      rest of its sample code the same way). ----
#if defined(SH_KIND_DIRECTIONAL) && defined(SH_KIND_POSITIONAL)
#error "one light kind per kernel"
#endif
// The own-fixture rule (below) compiles where the includer asks for it (RT_WALK_FIXTURE: the kernels that
// trace point lights -- pass 15's generic / positional core, the GI trace's budgeted point rays, the fog,
// the visibility queue's walk); everywhere else rt_fix_on could never be set and the rule folds away.
#if defined(SH_KIND_DIRECTIONAL) || !defined(RT_WALK_FIXTURE)
#define SH_FIX_ON false   // (the own-fixture rule is a positional light's)
#else
#define SH_FIX_ON rt_fix_on
#endif

// ---- OWN-FIXTURE RULE (#439 / #869: a POINT light's own lamp; clean_shadow_core.glsl SH_FIXTURE,
//      the RT hits' budgeted point-light rays, the fog's point rays) ----
// SL point lights sit inside their fixtures (a bulb in a shade, a lantern, a sconce): traced
// plainly, the fixture shadows its own light and the lamp goes dark. A caster instance whose own
// box (object space) holds the light's centre and whose world size is at most rt_fix_max is the
// light's FIXTURE: its translucent faces do not tint the ray, and when an opaque / alpha-tested
// face of it ends the ray, the ray is traced again up to where it ENTERS that box (rt_fix_t) --
// everything between the receiver and the lamp still shadows, nothing inside the lamp does. Not
// an infinite near plane: the clip is the fixture's own bounds, and only a small enclosing box
// qualifies (a room-sized mesh around the light never does). Set per ray by the caller, which
// traces through rtVisTrace (the re-trace loop) and clears rt_fix_on after.
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
// A walk ended by a committed (opaque / confirmed) caster: blocked. OWN FIXTURE: the committed caster's
// box entry (the slab test in its object space: the ray parameter is the same through the affine map) --
// the caller re-traces up to it. Else the blocker's distance + identity (srad, the moving-caster bit).
// q: the walk's query, standing on the committed intersection (its transforms read only for the fixture).
vec3 rtWalkBlocked(vec3 origin, vec3 dir, bool trans_walk, rayQueryEXT q)
{
#if defined(RT_WALK_FIXTURE) || defined(RT_TRACK_BLOCKER)
    const uint fid = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)),
                                   uint(rayQueryGetIntersectionGeometryIndexEXT(q, true)));
#else
    const uint fid = 0u;   // (neither rule reads the committed caster's identity here: no id lookup)
#endif
    vec3 fa, fb;
    if (SH_FIX_ON && !trans_walk
        && rtFixtureBox(fid, rayQueryGetIntersectionWorldToObjectEXT(q, true), rayQueryGetIntersectionObjectToWorldEXT(q, true),
                        fa, fb))
    {
        const mat4x3 w2o = rayQueryGetIntersectionWorldToObjectEXT(q, true);
        const vec3 oo = w2o * vec4(origin, 1.0);
        const vec3 od = w2o * vec4(dir, 0.0);
        const vec3 inv = vec3(1.0) / mix(od, vec3(1e-20), lessThan(abs(od), vec3(1e-20)));
        const vec3 t0 = (fa - oo) * inv, t1 = (fb - oo) * inv;
        const vec3 tn = min(t0, t1);
        rt_fix_t = max(max(max(tn.x, tn.y), tn.z), 0.0);
        return vec3(0.0);
    }
    if (!trans_walk)
    {
        rt_block_t = min(rt_block_t, rayQueryGetIntersectionTEXT(q, true));
        RT_NOTE_BLOCKER(fid);
    }
    return vec3(0.0);
}
// One alpha-tested candidate of the walk (it passed the self / facing rules: fol = rtFolAway's verdict):
// THE alpha test, then the kind's rule on the small accumulator (trans, n_fol). True: the foliage walk
// ends here (enough layers / no light left), a shadow walk confirms the candidate (a solid caster).
bool rtWalkCand(uint id, uint prim, vec2 bc, float t, vec3 dir, rayQueryEXT q, uint kind, uint ks, uint fol,
                inout vec3 trans, inout uint n_fol)
{
    const bool rgb = rtWalkRgb(kind);
    const bool trans_walk = rtWalkTrans(kind, ks);
    float cutoff; bool blend; vec3 col;
    const float a = rtCandidateAlphaCol(id, prim, bc, t, dir, q, cutoff, blend, col);   // (the ONE alpha test)
#ifndef SH_KIND_POSITIONAL
    if (rtWalkFol(kind, ks))
    {
        if (a < cutoff) return false;   // a hole in the leaf
        trans *= rgb ? rtFolLayer(col) : vec3(rtFolStrength());
        rt_block_t = min(rt_block_t, t);
        RT_NOTE_BLOCKER(id);
        return ++n_fol >= rtFolLayers() || max(max(trans.r, trans.g), trans.b) < 0.01;
    }
#endif
    vec3 fa, fb;   // (rtFixtureBox's box)
    if (rtWalkVisK(kind, ks))
        return a >= cutoff && fol == RT_FOL_OCCLUDE;   // #735
    if ((blend ? a >= RT_BLEND_SOLID : a >= cutoff) && fol == RT_FOL_LEAF)
    {
        // #735: a transmissive (away-facing solid) leaf. The mixed walks pass it; the translucent
        // walk colours the light by the first layers in mode 2.
        if (trans_walk && rtFolMode() == 2u && n_fol < rtFolLayers())
        {
            trans *= rgb ? rtFolLayer(col) : vec3(rtFolStrength());
            ++n_fol;
            rt_block_t = min(rt_block_t, t);
            return max(max(trans.r, trans.g), trans.b) < 0.01;
        }
        return false;
    }
    if (blend && SH_FIX_ON && rtFixtureBox(id, rayQueryGetIntersectionWorldToObjectEXT(q, false),
                                             rayQueryGetIntersectionObjectToWorldEXT(q, false), fa, fb))
        return false;   // (the light's own translucent shade: no tint)
    if (blend && rt_blend_parity && a >= ALPHA_BLEND_SOLID) return true;   // LL: solid caster
    if (blend && !(rt_blend_parity && a < ALPHA_BLEND_NONE))
    {
        trans *= rgb ? translucentTransmit(col, a) : vec3(1.0 - clamp(a, 0.0, 1.0));
        if (!trans_walk && a > ALPHA_TRANS_MIN) { rt_block_t = min(rt_block_t, t); RT_NOTE_BLOCKER(id); }
        return max(max(trans.r, trans.g), trans.b) < 0.01;
    }
    if (blend) return false;   // LL: casts nothing
    return a >= cutoff;
}
// THE WALK. Visibility (or the foliage ray's colour) over [tmin, tmax] along dir for walk `kind` over
// TLAS classes `mask`. Per-ray state the caller sets around it (globals; cleared after): rt_blend_parity
// / rt_blend_coarse (blend casters), rtFoliageSet .. rtFoliageEnd (sun / moon facing culls), rt_self_owner
// (no-self-shadow characters), rt_cone_* (the alpha test's LOD), rt_fix_* (own fixture: rtVisTrace).
// Writes rt_block_t (the nearest blocker distance, lowered) and RT_NOTE_BLOCKER. ks: the kinds this call
// site may pass (RT_WALK_KS_*; rtVisWalk: any).
// <GMDX> PROJECTOR FACING (set by projShadowRayCoreImpl around its rays, clean_shadow_rt.glsl): LL
// renders spot shadow maps back-face culled, so a single-sided face turned AWAY from the light never
// casts -- e.g. the inside of a lantern's housing around its light prim. The ray runs receiver ->
// light and meets such a face on its front side. Opaque walls are CULL_DISABLE in the TLAS (the sun's
// rays keep them two-sided), so these rays go NoOpaque (no opaque-first pre-ray, the NoOpaque budget)
// and the walk skips those hits itself.
bool rt_proj_facing = false;
// </GMDX>
vec3 rtVisWalkF(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind, uint ks)
{
    const bool fol_walk = rtWalkFol(kind, ks);
    const bool trans_walk = rtWalkTrans(kind, ks);
    const bool opaque_walk = rtWalkOpaque(kind, ks);
    uint flags;
    if (fol_walk)
        // FOLIAGE TRANSMISSION (#735, mode 2): the LIGHT-facing faces culled (the shadow rays' side),
        // NoOpaque so an opacity micromap's "known opaque" micro-triangles still arrive as candidates
        // (their leaf colour) while its "known transparent" ones stay dropped (traceFoliageTransmit)
        flags = gl_RayFlagsNoOpaqueEXT | (rtFolFlip() ? gl_RayFlagsCullFrontFacingTrianglesEXT
                                                      : gl_RayFlagsCullBackFacingTrianglesEXT);
    else if (opaque_walk)
        // FORCE-OPAQUE (traceShadowRayOpaque): a receiver that may sit on a no-self-shadow character
        // (rt_self_owner != 0) drops the opaque flag -- it would commit that character's own faces
        // without a candidate -- and confirms every candidate but the receiver's own
        flags = gl_RayFlagsTerminateOnFirstHitEXT | (rt_self_owner != 0u ? gl_RayFlagsNoneEXT : gl_RayFlagsOpaqueEXT)
              | rtFolShadowFlags();
    else
        flags = gl_RayFlagsTerminateOnFirstHitEXT | (trans_walk ? gl_RayFlagsNoneEXT : rtFolShadowFlags());
    if (rt_proj_facing && !fol_walk)   // <GMDX> every candidate reaches the walk (see rt_proj_facing)
        flags = (flags & ~gl_RayFlagsOpaqueEXT) | gl_RayFlagsNoOpaqueEXT;
    // (Xid 109, rtRaySane: an invalid ray is blocked as G-7; the foliage walk keeps no colour)
    if (!rtRaySane(origin, tmin, dir, tmax)) return fol_walk ? vec3(1.0) : vec3(0.0);
    // OPAQUE FIRST (RT_WALK_OFIRST): a blocking walk over the full shadow classes (the disc rays: not the
    // translucent split's, the foliage transmission's or a force-opaque walk's -- those meet only one
    // kind of geometry anyway) first asks the hardware alone: the same ray and flags with the non-opaque
    // geometry culled -- no candidate, no alpha test. An opaque caster ends it, blocked, whatever lies
    // in front (every kind adds 0 for a committed caster: the walk below returns 0 then, tinted layers
    // and budget alike). Clear: nothing opaque lies on the segment, so the walk below meets only the
    // non-opaque layers -- it culls the opaque geometry and comes to the same result.
    if (RT_WALK_OFIRST && !fol_walk && !trans_walk && !rt_proj_facing && (flags & gl_RayFlagsOpaqueEXT) == 0u)   // <GMDX> (not NoOpaque)
    {
        rayQueryEXT rq0;
        rayQueryInitializeEXT(rq0, uTLAS, flags | gl_RayFlagsCullNoOpaqueEXT, mask & 0xFFu, origin, tmin, dir, tmax);
        while (rayQueryProceedEXT(rq0)) {}   // (no candidate reaches the shader: the loop never runs)
        if (rayQueryGetIntersectionTypeEXT(rq0, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            return rtWalkBlocked(origin, dir, false, rq0);
        flags |= gl_RayFlagsCullOpaqueEXT;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, flags, mask & 0xFFu, origin, tmin, dir, tmax);
    vec3 trans = vec3(1.0);
    uint n_cand = 0u;
    uint n_fol = 0u;   // #735 transmissive layers coloured so far
    const uint budget = (fol_walk || rt_proj_facing) ? RT_CANDIDATE_BUDGET_NOOPAQUE : RT_CANDIDATE_BUDGET;   // <GMDX> rt_proj_facing
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        if (++n_cand > budget)
        {
            // over budget (G-7): the shadow walks treat it as opaque -- the ray is blocked here; the
            // foliage ray keeps the colour gathered so far (visibility is the shadow rays')
            if (!fol_walk) rayQueryConfirmIntersectionEXT(rq);
            rayQueryTerminateEXT(rq);
            break;
        }
        const uint id = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                      uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
        // <GMDX> projector rays: a single-sided face met on its front faces away from the light
        if (rt_proj_facing && !fol_walk && rtIdIn(id) && (rt_instances[id].flags & 256u) == 0u
            && rayQueryGetIntersectionFrontFaceEXT(rq, false)) continue;
        uint fol = RT_FOL_OCCLUDE;
        if (fol_walk)
        {
            if (!rtIdIn(id) || (rt_instances[id].flags & 256u) != 0u) continue;   // double-sided: an occluder (/ #874 guard)
        }
        else
        {
            // No-self-shadow characters (rtSelfCandidate): the receiver's own stale geometry never
            // occludes it; another throttled character's opaque face is solid at once.
            const uint self_c = rtSelfCandidate(RT_CAND_OWNER(rq), id);
            if (self_c == 1u) continue;
            if (self_c == 2u || opaque_walk) { rayQueryConfirmIntersectionEXT(rq); continue; }
            // An away-facing face of a single-sided worn / rigged caster (rt_fol_rig; only alpha-blend
            // ones arrive -- the rest are culled in traversal): the shadow pass's alpha FS drops it at
            // ANY alpha, so it neither occludes nor tints -- no alpha fetch. (Static leaves keep #735's
            // solid-only rule in rtWalkCand.)
            fol = rtFolAway(id, rayQueryGetIntersectionFrontFaceEXT(rq, false));
            if (fol == RT_FOL_PASS) continue;
        }
        // (the candidate's rule: its own function of the intersection + the small accumulator -- nothing
        //  of the caller is needed past the alpha test; the transforms are read inside, where used)
        if (rtWalkCand(id, RT_HIT_PRIM(rq, false, id), rayQueryGetIntersectionBarycentricsEXT(rq, false),
                       rayQueryGetIntersectionTEXT(rq, false), dir, rq, kind, ks, fol, trans, n_fol))
        {
            if (fol_walk) { rayQueryTerminateEXT(rq); break; }
            rayQueryConfirmIntersectionEXT(rq);
        }
    }
    if (fol_walk) return trans;
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return trans;
    return rtWalkBlocked(origin, dir, trans_walk, rq);
}
vec3 rtVisWalk(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind)
{
    return rtVisWalkF(origin, dir, tmin, tmax, mask, kind, RT_WALK_KS_ANY);
}
// The walk with the OWN-FIXTURE re-trace (rt_fix_*): a ray ended by the light's own fixture is traced
// again up to the fixture's box -- at most twice more (nested lamps) -- through the same call site.
// Without rt_fix_on: rtVisWalk once.
vec3 rtVisTraceK(vec3 origin, vec3 dir, float tmax, uint mask, uint kind, uint ks)
{
    vec3 w = vec3(1.0);
    float tmc = tmax;
    [[dont_unroll]] for (uint fr = 0u; fr < 3u; ++fr)
    {
        rt_fix_t = -1.0;
        w = rtVisWalkF(origin, dir, 0.0, tmc, mask, kind, ks);   // THE walk site
        if (!(rt_fix_t >= 0.0)) break;
        tmc = min(rt_fix_t, tmc) - 0.002;
        w = vec3(1.0);   // (nothing before the fixture so far: lit unless the next pass meets one)
        if (tmc <= 0.0) break;
    }
    return w;
}
vec3 rtVisTrace(vec3 origin, vec3 dir, float tmax, uint mask, uint kind)
{
    return rtVisTraceK(origin, dir, tmax, mask, kind, RT_WALK_KS_ANY);
}

// ---- THE SINGLE SUN / MOON RAY (#1048): one ray along d with its tails -- the force-opaque walk (opaque_only),
//      else the split's opaque set (binary; RenderVKRTTranslucentSplit) or the mixed walk (parity: LL's
//      blend thresholds, RenderVKRTBlendShadowParity), then the translucent ray where the opaque one came
//      back lit, then the #735 foliage transmission ray where light is left (mode 2). The RT hit shading's
//      sun ray (clean_rt_shade.glsl rtHitSunShadowCore) and the RT god rays' march rays
//      (clean_godrays.comp) -- the god rays had a copy that gave the mixed walk no parity. Every ray
//      through ONE walk site. The caller holds the sun / moon trace state (rtFoliageSet .. rtFoliageEnd)
//      and rt_cone_* (the casters' alpha-test LOD). rgb: coloured translucency (#212). ----
// (#1039 M2) ... and the visibility queue's walk kernel, whose requests are not all sun rays: sun false = ONE
// ray of walk `kind` over TLAS classes `mask` (wkind / wmask; the sun's switches unused) with the own-fixture
// re-trace of rtVisTraceK (rt_fix_*: the caller sets them, at most two more passes) -- the hit occlusion ray
// (traceShadowRay) and the budgeted light rays (rtrProjRay) through the same walk site as the sun's.
vec3 rtVisQueued(vec3 o, vec3 d, float tmax, bool sun, uint wkind, uint wmask,
                 bool opaque_only, bool split, bool parity, bool rgb)
{
    uint kind = !sun ? wkind : (opaque_only ? RT_WALK_OPAQUE : (split ? RT_WALK_VIS : (rgb ? RT_WALK_RGB : RT_WALK_GREY)));
    uint mask = !sun ? wmask : ((opaque_only || split) ? (RT_MASK_SHADOW & ~RT_MASK_BLEND) : RT_MASK_SHADOW);
    bool par = sun && !opaque_only && !split && parity;
    vec3 vis = vec3(1.0);
    float tmc = tmax;
    [[dont_unroll]] for (uint ph = 0u; ph < 3u; ++ph)
    {
        if (!sun) rt_fix_t = -1.0;
        else if (ph == 1u)
        {
            // the translucent split's ray, where the opaque set left the light
            if (opaque_only || !split || !(vis.x > 0.0)) continue;
            kind = rgb ? RT_WALK_TRANS_RGB : RT_WALK_TRANS;
            mask = RT_MASK_BLEND;
            par = parity;
        }
        else if (ph == 2u)
        {
            // foliage transmission (mode 2), where any light is left
            if (opaque_only || rtFolMode() != 2u || !(max(max(vis.r, vis.g), vis.b) > 0.0)) break;
            kind = rgb ? RT_WALK_FOL_RGB : RT_WALK_FOL;
            mask = RT_MASK_ALPHAMASK;
            par = false;
        }
        rt_blend_parity = par;
        const vec3 w = rtVisWalk(o, d, 0.0, tmc, mask, kind);   // THE walk site
        rt_blend_parity = false;
        if (sun) { vis = ph == 0u ? w : vis * w; continue; }
        // one ray, re-traced up to its light's fixture box (rtVisTraceK's steps)
        vis = w;
        if (!(rt_fix_t >= 0.0)) break;
        tmc = min(rt_fix_t, tmc) - 0.002;
        vis = vec3(1.0);   // (nothing before the fixture so far: lit unless the next pass meets one)
        if (tmc <= 0.0) break;
    }
    return vis;
}
vec3 rtSunRayVis(vec3 o, vec3 d, float tmax, bool opaque_only, bool split, bool parity, bool rgb)
{
    return rtVisQueued(o, d, tmax, true, 0u, 0u, opaque_only, split, parity, rgb);
}

// ---- The walks by their old names (one inlined walk per call site: prefer one call with a run-time kind
//      where a caller picks between them) ----
vec3 traceShadowWalk(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, kind);
}
float traceShadowRay(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_GREY).x;
}
vec3 traceShadowRayRGB(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_RGB);
}
float traceShadowRayVis(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_VIS).x;
}
// rgb = coloured (#212, translucentTransmit) else the grey (1 - alpha) of traceShadowRay.
vec3 traceTransmittance(vec3 origin, vec3 dir, float tmin, float tmax, bool rgb)
{
    return rtVisWalkF(origin, dir, tmin, tmax, RT_MASK_BLEND, rgb ? RT_WALK_TRANS_RGB : RT_WALK_TRANS, RT_WALK_KS_TRANS);
}
// FOLIAGE TRANSMISSION RAY (#735, mode 2): ONE ray per lit receiver over the alpha-mask
// class only, after the shadow rays found the light visible. Those rays already culled the
// away-facing leaf faces and stopped on any occluder -- opaque, worn, rigged, terrain
// AND light-facing leaves -- so nothing this walk could miss by stopping early is solid:
// it is free to end after rt_fol_layers transmissive leaves ("collects colour from N
// leaves and stops"), whatever order traversal hands them over in.
//   * Culls the LIGHT-facing faces (the shadow rays' side) in traversal: only away-facing
//     leaf faces become candidates. Double-sided instances (CULL_DISABLE) still arrive and
//     are skipped -- the shadow rays treat them as occluders from both sides.
//   * gl_RayFlagsNoOpaqueEXT: opacity micromaps commit "known opaque" micro-triangles in
//     hardware, with no candidate -- the ray would end there without the leaf's colour.
//     NoOpaque overrides the micromap's opaque result to non-opaque (a candidate), while
//     "known transparent" micro-triangles stay ignored (the micromap lookup happens first),
//     so the micromap still drops the holes for free. The mask holds only non-opaque
//     instances, so it adds no other candidate. (Modes 0 / 1 and the shadow rays keep the
//     micromaps' full benefit: facing culling runs before the micromap lookup.)
// rgb = coloured (RenderVKTranslucentShadows) else grey. Lowers rt_block_t at each
// coloured layer: the tinted spot is a shadow edge the mask denoiser must size and blur.
// (Over budget / an invalid ray: the colour gathered so far / none -- visibility is the shadow rays'.)
vec3 traceFoliageTransmit(vec3 origin, vec3 dir, float tmin, float tmax, bool rgb)
{
    return rtVisWalkF(origin, dir, tmin, tmax, RT_MASK_ALPHAMASK, rgb ? RT_WALK_FOL_RGB : RT_WALK_FOL, RT_WALK_KS_FOL);
}
// Opaque fast path: every instance treated as opaque (no alpha test, no candidates).
// A receiver that may sit on a no-self-shadow character (rt_self_owner != 0) drops the
// OPAQUE ray flag -- it would commit that character's own faces without a candidate -- and
// confirms every candidate but the receiver's own (rtSelfCandidate), so it stays the same
// all-opaque rule otherwise (one candidate before the first-hit termination, plus skips).
float traceShadowRayOpaque(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_OPAQUE).x;
}

#endif
