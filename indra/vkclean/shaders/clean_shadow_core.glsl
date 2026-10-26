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
//     foliage transmission ray -- every ray through ONE walk call (clean_vis_walk.glsl rtVisTrace), so the
//     candidate alpha test, the self-shadow probe and the facing rules are inlined once.
// Per-sample globals (clean_rt_common / clean_shadow_rt): the caller resets rt_block_t (and
// rt_block_dyn) before a sample and reads them after; shadowRecvLight sets rt_term_* and
// rt_cl_lift, the caller clears them after the sample (shadowRecvLightEnd).
#ifndef CLEAN_SHADOW_CORE_GLSL
#define CLEAN_SHADOW_CORE_GLSL

// ---- THE WALK: clean_vis_walk.glsl (#1039 M0; included by clean_rt_common.glsl) -- rtVisWalk over every
//      kind, the own-fixture rule (rt_fix_*, re-traced by rtVisTrace), the opaque-first pre-ray
//      (RT_WALK_OFIRST) and the kernel specialisation (SH_KIND_DIRECTIONAL / SH_KIND_POSITIONAL: the
//      includer defines them before clean_rt_common.glsl; this file compiles its sample code by them). ----

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
// #664 TERMINATOR TRIANGLE of the receiver: clean_shadow_rt.glsl rtTerminatorCached (the pre-pass
// triangle or the camera probe, their lift escape-checked once after either; #1042: this file's copy of
// it is gone -- every consumer shares that one). Same cache (rt_tc_*: keyed by P) and the same globals
// for rtTerminatorPost.

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
                   && rtTerminatorCached(R.P, R.cam, R.Ng, RT_TERM_MASK_COS, R.rp, tlift, tns, tng);
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

// ---- (#1031) THE SAMPLE'S RAY-GENERATION INPUTS (ShRayGen): everything shadowSampleVis derives once
//      per sample and every ray of it re-reads (the disc frame, the origin rules, the near plane, the
//      floor, the flags). With SH_PARK (the includer: 64-lane workgroups) they wait in shared memory,
//      one row set per lane, and each ray reloads them -- so none of them is live across a walk: the
//      walk's alpha test, the register peak, sees only the accumulators. Without it: a local copy. ----
#define SHG_SPHERE   1u
#define SHG_HARD     2u
#define SHG_LIFTED   4u
#define SHG_BFS      8u
#define SHG_SPLIT   16u
#define SHG_FOL     32u
#define SHG_OO      64u
#define SHG_DPAR   128u   // the disc rays' blend parity
#define SHG_TPAR   256u   // the translucent ray's
#define SHG_KIND_SHIFT 9u   // the disc rays' walk kind (3 bits)
#define SHG_VISSET 4096u   // the disc rays walk the opaque set (binary): mask without the blend class
struct ShRayGen {
    vec3 pos;  float radius;     // S.pos (directional: the unit direction L), the disc radius
    vec3 axis; float nearClip;
    vec3 Pb;   float biasLift;   // (positional origins) the lifted receiver, S.bias + rt_cl_lift
    vec3 Nin;  float camd;
    vec3 Nv;   float efloor;
    vec3 Nf;   float tmax;
    vec3 u;    float noiseX;
    vec3 v;    float noiseY;
    vec3 oL;   uint  bits;       // (directional) every ray's origin; SHG_*
    vec3 tlift; uint n;          // rt_term_lift; the disc rays
    vec3 post;                   // rtTerminatorPost's light direction
};
#ifdef SH_PARK
#define SH_PARK_ROWS 11
shared uvec4 s_shPark[64][SH_PARK_ROWS];
void shRayGenPark(ShRayGen g)
{
    const uint l = gl_LocalInvocationIndex;
    s_shPark[l][0]  = uvec4(floatBitsToUint(g.pos), floatBitsToUint(g.radius));
    s_shPark[l][1]  = uvec4(floatBitsToUint(g.axis), floatBitsToUint(g.nearClip));
    s_shPark[l][2]  = uvec4(floatBitsToUint(g.Pb), floatBitsToUint(g.biasLift));
    s_shPark[l][3]  = uvec4(floatBitsToUint(g.Nin), floatBitsToUint(g.camd));
    s_shPark[l][4]  = uvec4(floatBitsToUint(g.Nv), floatBitsToUint(g.efloor));
    s_shPark[l][5]  = uvec4(floatBitsToUint(g.Nf), floatBitsToUint(g.tmax));
    s_shPark[l][6]  = uvec4(floatBitsToUint(g.u), floatBitsToUint(g.noiseX));
    s_shPark[l][7]  = uvec4(floatBitsToUint(g.v), floatBitsToUint(g.noiseY));
    s_shPark[l][8]  = uvec4(floatBitsToUint(g.oL), g.bits);
    s_shPark[l][9]  = uvec4(floatBitsToUint(g.tlift), g.n);
    s_shPark[l][10] = uvec4(floatBitsToUint(g.post), 0u);
}
ShRayGen shRayGenUnpark()
{
    const uint l = gl_LocalInvocationIndex;
    ShRayGen g;
    uvec4 q;
    q = s_shPark[l][0];  g.pos  = uintBitsToFloat(q.xyz); g.radius   = uintBitsToFloat(q.w);
    q = s_shPark[l][1];  g.axis = uintBitsToFloat(q.xyz); g.nearClip = uintBitsToFloat(q.w);
    q = s_shPark[l][2];  g.Pb   = uintBitsToFloat(q.xyz); g.biasLift = uintBitsToFloat(q.w);
    q = s_shPark[l][3];  g.Nin  = uintBitsToFloat(q.xyz); g.camd     = uintBitsToFloat(q.w);
    q = s_shPark[l][4];  g.Nv   = uintBitsToFloat(q.xyz); g.efloor   = uintBitsToFloat(q.w);
    q = s_shPark[l][5];  g.Nf   = uintBitsToFloat(q.xyz); g.tmax     = uintBitsToFloat(q.w);
    q = s_shPark[l][6];  g.u    = uintBitsToFloat(q.xyz); g.noiseX   = uintBitsToFloat(q.w);
    q = s_shPark[l][7];  g.v    = uintBitsToFloat(q.xyz); g.noiseY   = uintBitsToFloat(q.w);
    q = s_shPark[l][8];  g.oL   = uintBitsToFloat(q.xyz); g.bits     = q.w;
    q = s_shPark[l][9];  g.tlift = uintBitsToFloat(q.xyz); g.n       = q.w;
    q = s_shPark[l][10]; g.post = uintBitsToFloat(q.xyz);
    return g;
}
#endif
// A ray's origin for a positional light toward point T (the receiver rules: lifted -> the lift's side,
// back-face self-shadow -> the visible side, else the side facing T).
vec3 shPosOrigin(ShRayGen g, vec3 toT)
{
    vec3 Nr = g.Nin;
    if ((g.bits & SHG_LIFTED) != 0u) { if (dot(Nr, g.tlift) < 0.0) Nr = -Nr; }
    else if ((g.bits & SHG_BFS) != 0u) Nr = g.Nv;
    else if (dot(Nr, toT) < 0.0) Nr = -Nr;
    return rtReceiverOrigin(g.Pb, Nr, g.camd, g.biasLift);
}
// A positional ray's length to the near plane (dot(X - O, A) = near: the fixture never shadows its own
// light) or, for a point light, to its own prim's bounding sphere (SH_SPHERE)
float shPosTmax(ShRayGen g, vec3 o, vec3 d, float len)
{
    if ((g.bits & SHG_SPHERE) != 0u) return len - g.nearClip - 0.002;
    const float da = dot(d, g.axis);
    const float dp = dot(o - g.pos, g.axis);
    return min((da < -1e-6) ? (g.nearClip - dp) / da : len, len) - 0.002;
}

// The visibility (x translucent / foliage tint) of receiver P (geometric normal Nin: the facet
// shadowRecvLight chose) toward light sample S. The caller set rt_term_* / rt_cl_lift
// (shadowRecvLight), rt_block_t, rt_noise_shift; the sample's rays share ONE walk call site.
// Exactly the former cores' arithmetic: sunShadowRayCore (directional) / projShadowRayCore
// (positional) of clean_shadow_rt.glsl.
vec3 shadowSampleVis(ShadowSample S, vec3 P, vec3 Nin, vec3 cam, float time)
{
    // (#1031 KERNEL SPECIALISATION: a one-kind kernel folds the other kind's paths away)
#if defined(SH_KIND_DIRECTIONAL)
    const bool pos = false, sphere = false;
    const bool oo = (S.flags & SH_OPAQUE_ONLY) != 0u;
    const bool fol = (S.flags & SH_FOLIAGE) != 0u;
    rt_fix_on = false;
#elif defined(SH_KIND_POSITIONAL)
    const bool pos = true, oo = false, fol = false;
    const bool sphere = (S.flags & SH_SPHERE) != 0u;
    rt_fix_on = (S.flags & SH_FIXTURE) != 0u;
#else
    const bool pos = (S.flags & SH_POSITIONAL) != 0u;
    const bool oo = (S.flags & SH_OPAQUE_ONLY) != 0u;
    const bool fol = (S.flags & SH_FOLIAGE) != 0u;
    const bool sphere = (S.flags & SH_SPHERE) != 0u;
    rt_fix_on = (S.flags & SH_FIXTURE) != 0u;
#endif
    const bool split = (S.flags & SH_SPLIT) != 0u;
    rt_fix_P = S.pos;
    rtSetShadowFootprint(P, Nin, cam);
    if (fol) rtFolBegin();
    const float camd = length(P - cam);
    rt_anim_time   = time;
    rt_cone_spread = 0.0;
    rt_cone_width0 = camd * 0.001;   // (fallback only: rtSetShadowFootprint's rt_foot_* override it)
    rt_cone_aniso  = 1.0;
#ifdef RT_NO_FOOT_LOD
    // (#1031) ISOTROPIC SHADOW LOD (RenderVKShadowIsoLod): the receiver pixel's footprint (minor axis:
    // the pixel across the view, already x the raster's material mip bias) as the ray cone's CONSTANT
    // width -- the alpha test's cone path: LOD = log2(width / |n.d| x the caster triangle's texel
    // density). The anisotropic projection (textureGrad along both footprint axes) is not compiled.
    if (rt_foot_on)
    {
        rt_cone_width0 = length(rt_foot_a);
        rt_foot_on = false;
    }
#endif
    // ---- the sample's ray-generation inputs (ShRayGen above) ----
    ShRayGen G;
    G.pos = S.pos; G.radius = S.radius; G.axis = S.axis; G.nearClip = S.nearClip;
    G.camd = camd; G.Nin = Nin; G.tmax = S.tmax; G.noiseX = S.noise.x; G.noiseY = S.noise.y;
    G.n = max(S.nRays, 1u);
    G.tlift = rt_term_lift;
    G.biasLift = S.bias + rt_cl_lift;
    // TERMINATOR (#664): a lifted origin stays on the VISIBLE side (above the facet, on the
    // surface the vertex normals describe); unlifted, each ray leaves the light-facing side.
    const bool lifted = rt_term_on && dot(rt_term_lift, rt_term_lift) > 0.0;
    G.Pb = lifted ? P + rt_term_lift : P;
    const vec3 L = S.pos;               // (directional)
    const vec3 toO = S.pos - G.Pb;      // (positional: to the emitter centre)
    const float dO = length(toO);
    // BACK-FACE SELF-SHADOW (clean_shadow_rt.glsl rtBackfaceSelfShadow): unlifted, every origin
    // leaves along the VISIBLE side of the facet (Nv), and a facet turned away from the light's
    // centre is shadowed by its own surface -- no ray (the old light-side flip pushed the origin
    // through single-plane walls: the next room's projector lit this side).
    const bool bfs = !lifted && rtBackfaceSelfShadow();
    G.Nv = rtVisibleSide(Nin, P, cam);
    const bool selfShadowed = bfs && !(dot(G.Nv, pos ? toO : L) > 0.0);
    // the disc frame's normal (a point light: its disc faces the receiver)
    const vec3 A = pos ? (sphere ? (dO > 1e-4 ? toO / dO : vec3(0.0, 0.0, 1.0)) : S.axis) : L;
    const bool hard = !(S.radius > 0.0);
    // the disc floor's frame: the shading surface when known (RenderVKRTShadowTerminatorFloor)
    G.Nf = rt_term_on ? rt_term_ns : Nin;
    if (dot(G.Nf, pos ? toO : L) < 0.0) G.Nf = -G.Nf;
    G.efloor = -2.0;
    if (!hard)
    {
        if (pos) { if (dO > 1e-4) G.efloor = min(clamp(dot(G.Nf, toO) / dO, 0.0, 1.0), ssr.taa_scale.w * S.radius / dO); }
        else G.efloor = min(clamp(dot(G.Nf, L), 0.0, 1.0), ssr.taa_scale.w * S.radius);
    }
    G.u = vec3(0.0); G.v = vec3(0.0);
    if (!hard || !pos)   // (the sun's frame is always built: unused when hard)
    {
        G.u = normalize(abs(A.z) < 0.9 ? cross(A, vec3(0.0, 0.0, 1.0)) : cross(A, vec3(1.0, 0.0, 0.0)));
        G.v = cross(A, G.u);
    }
    const uint optsB = rtBlendOpts();
    const bool discVisSet = split || oo;   // binary opaque-set disc rays
    const uint discKind = (oo && !rt_opaque_cutouts) ? RT_WALK_OPAQUE
                        : (discVisSet ? RT_WALK_VIS : (specTransOn() ? RT_WALK_RGB : RT_WALK_GREY));
    G.bits = (sphere ? SHG_SPHERE : 0u) | (hard ? SHG_HARD : 0u) | (lifted ? SHG_LIFTED : 0u) | (bfs ? SHG_BFS : 0u)
           | (split ? SHG_SPLIT : 0u) | (fol ? SHG_FOL : 0u) | (oo ? SHG_OO : 0u)
           | ((!discVisSet && (optsB & 1u) != 0u) ? SHG_DPAR : 0u) | ((optsB & 1u) != 0u ? SHG_TPAR : 0u)
           | (discKind << SHG_KIND_SHIFT) | (discVisSet ? SHG_VISSET : 0u);
    // (#1031) a directional light's rays all leave ONE origin (every ray's Nr rule tests against L)
    G.oL = vec3(0.0);
    if (!pos)
    {
        vec3 Nr = Nin;
        if (lifted) { if (dot(Nr, rt_term_lift) < 0.0) Nr = -Nr; }
        else if (bfs) Nr = G.Nv;
        else if (dot(Nr, L) < 0.0) Nr = -Nr;
        G.oL = rtReceiverOrigin(G.Pb, Nr, camd, G.biasLift);
    }
    G.post = pos ? normalize(S.pos - P) : L;
#ifdef SH_PARK
    shRayGenPark(G);
#endif

    vec3 vis = vec3(0.0), first = vec3(0.0);
    float rays = float(G.n);   // the normaliser: rays actually traced (early-outs below)
    uint k = 0u;
    uint phase = selfShadowed ? 3u : 0u;   // 0 the disc, 1 the translucent ray, 2 the foliage ray, 3 done
                                           // (self-shadowed: vis 0, no ray)
    const uint nRays = G.n;   // (the loop bound: one value across the walks)
    [[dont_unroll]] for (uint it = 0u; it < nRays + 2u && phase < 3u; ++it)
    {
#ifdef SH_PARK
        const ShRayGen g = shRayGenUnpark();   // (this ray's inputs: reloaded, not carried across the last walk)
#else
        const ShRayGen g = G;
#endif
        const uint n = g.n;
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
                if ((g.bits & SHG_HARD) == 0u)
                {
                    const float r   = g.radius * sqrt((float(i) + g.noiseX) / float(n));
                    const float phi = g.noiseY + 2.3999632 * float(i);
                    off = (g.u * cos(phi) + g.v * sin(phi)) * r;
                }
                if (pos)
                {
                    const vec3 T = g.pos + off;
                    o = shPosOrigin(g, T - g.Pb);
                    const vec3 toL = T - o;
                    const float len = length(toL);
                    if (len <= 1e-4) { vis += vec3(1.0); continue; }
                    d = toL / len;
                    const float e = dot(g.Nf, d);
                    if (e < g.efloor) d = normalize(d + g.Nf * (g.efloor - e));
                    tm = shPosTmax(g, o, d, len);
                    if (tm <= 0.0) { vis += vec3(1.0); continue; }
                }
                else
                {
                    o = g.oL;
                    d = g.pos;
                    if ((g.bits & SHG_HARD) == 0u)
                    {
                        d = normalize(g.pos + off);
                        const float e = dot(g.Nf, d);
                        if (e < g.efloor) d = normalize(d + g.Nf * (g.efloor - e));
                    }
                    tm = g.tmax;
                }
                kind = (g.bits >> SHG_KIND_SHIFT) & 7u;
                mask = (g.bits & SHG_VISSET) != 0u ? (RT_MASK_SHADOW & ~RT_MASK_BLEND) : RT_MASK_SHADOW;
                par = (g.bits & SHG_DPAR) != 0u;
                want = true;
            }
        }
        if (phase == 1u && !want)
        {
            // TRANSLUCENT SPLIT: one ray toward the light's centre over the blend class (same
            // origin rules and near-plane clip as a disc ray; no disc floor), where any light is left
            phase = 2u;
            if ((g.bits & SHG_SPLIT) != 0u && any(greaterThan(vis, vec3(0.0))))
            {
                if (pos)
                {
                    o = shPosOrigin(g, g.pos - g.Pb);
                    const vec3 toL = g.pos - o;
                    const float len = length(toL);
                    if (len > 1e-4)
                    {
                        d = toL / len;
                        tm = shPosTmax(g, o, d, len);
                        want = tm > 0.0;
                    }
                }
                else { o = g.oL; d = g.pos; tm = g.tmax; want = true; }
                kind = specTransOn() ? RT_WALK_TRANS_RGB : RT_WALK_TRANS;
                mask = RT_MASK_BLEND;
                par = (g.bits & SHG_TPAR) != 0u;
            }
            if (!want) continue;
        }
        else if (phase == 2u && !want)
        {
            // FOLIAGE TRANSMISSION (#735 mode 2; the sun / moon's full trace only): one ray along
            // the centre over the alpha-mask class, where the shadow rays left some light
            phase = 3u;
            if ((g.bits & SHG_FOL) == 0u || pos || (g.bits & SHG_OO) != 0u || rtFolMode() != 2u
                || !(max(max(vis.r, vis.g), vis.b) > 0.0))
                continue;
            o = g.oL;   // (SH_FOLIAGE: the sun / moon's only -- directional)
            d = g.pos; tm = g.tmax;
            kind = specTransOn() ? RT_WALK_FOL_RGB : RT_WALK_FOL;
            mask = RT_MASK_ALPHAMASK;
            want = true;
        }
        if (!want) continue;
        rt_blend_parity = par;
        // (SH_FIXTURE: a ray ended by the light's own fixture is traced again up to the fixture's box
        //  -- at most twice, nested lamps; the same call site: rtVisTrace)
        const vec3 w = rtVisTrace(o, d, tm, mask, kind);   // THE walk site
        rt_blend_parity = false;
        if (kind == RT_WALK_OPAQUE || kind == RT_WALK_VIS || kind == RT_WALK_GREY || kind == RT_WALK_RGB) vis += w;
        else vis *= w;
    }
    if (phase == 0u) vis /= rays;   // (never: the loop bound covers the disc + both tails)
    rt_fix_on = false;
    if (fol) rtFolEnd();
    rt_foot_on = false;
#ifdef SH_PARK
    const vec3 r = rtTerminatorPost(vis, shRayGenUnpark().post);
#else
    const vec3 r = rtTerminatorPost(vis, G.post);
#endif
    rt_term_rej = false;
    return r;
}

#endif
