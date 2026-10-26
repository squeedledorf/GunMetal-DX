// vkclean — ray-traced shadow cores (#185 sun / moon, projectors; #212 coloured
// transmittance), shared by the lit FS (inline, via the wrappers in
// clean_draw_common.glsl) and the screen-space shadow mask (clean_rt_shadow_mask.comp,
// #195). Inputs are explicit -- receiver P, geometric normal, camera, time and the
// noise pixel -- so nothing here touches fragment-only state. Needs clean_rt_common,
// clean_frame_info (ssr), clean_lights_common + clean_projshadow before inclusion.
// Interleaved gradient noise (Jimenez 2014). With TAA jittering the view the pattern
// also moves with the frame (the Halton phase, 16 distinct offsets), so the resolve
// integrates a different disc sample every frame and the penumbra converges instead of
// staying grainy. Without TAA it stays fixed per pixel: moving noise with nothing to
// average it would crawl.
float projIGN(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

// TEMPORAL NOISE PHASE (shadow denoiser, clean_rt_shadow_mask.comp): added (mod 1) to
// both disc parameters -- radius offset (x) and rotation (y) -- of the soft-shadow
// pattern. The mask sets it from an R2 sequence over the FRAME index, so each pixel
// walks a low-discrepancy sequence that never repeats with the 16-phase TAA jitter
// (keying the noise on the Halton phase repeated every 16 frames: the short history
// never integrated it -> residual shimmer). 0 = the IGN pattern alone (every other
// caller: lit FS inline traces, RT god rays).
vec2 rt_noise_shift = vec2(0.0);

// Shadow ray through translucent casters (#212): coloured with
// RenderVKTranslucentShadows (frame info render_opts.z), else the grey (1 - alpha)
// dimming the ray-traced shadows always had.
// RT TRANSLUCENT-CASTER OPTIONS (frame info lit_dbg.w, LLCleanRendererVK): bit 0
// RenderVKRTBlendShadowParity (rt_blend_parity, clean_rt_common.glsl), bit 1
// RenderVKRTTranslucentSplit (rtSplitOn), bit 2
// RenderVKProbeRTBlendCoarse (clean_rt_shade.glsl), bits [8,16) the adaptive soft-shadow
// tolerance RenderVKRTShadowAdaptiveTol in 1/256 (rtAdaptiveDone), bits [16,24)
// RenderVKRTTransReceiverAlphaDist in metres (clean_draw_common.glsl rtTransCutoutDist).
uint rtBlendOpts() { return uint(ssr.lit_dbg.w + 0.5); }

vec3 shadowRayVis(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    rt_blend_parity = (rtBlendOpts() & 1u) != 0u;
    // (One walk with a run-time kind, not a choice of two: clean_rt_common.glsl RT_WALK_*.)
    const vec3 v = rtVisWalkF(origin, dir, tmin, tmax, mask, specTransOn() ? RT_WALK_RGB : RT_WALK_GREY, RT_WALK_KS_MIXED);
    rt_blend_parity = false;
    return v;
}

// TRANSLUCENT SPLIT (RenderVKRTTranslucentSplit, rtBlendOpts bit 1; default on): the
// soft-shadow disc rays trace the OPAQUE caster set only (every class but water and
// alpha-blend; alpha-mask cut-outs keep their alpha test; with RenderVKRTBlendShadowParity
// also the alpha-blend texels LL's shadow pass makes solid, which the transmittance ray
// then skips <SS:Skoomastream>) -- binary, so the adaptive
// 0 / 1 early-out settles under glass / water too, and no disc ray pays a translucent
// candidate. ONE transmittance ray per receiver and light, along the light's CENTRE
// direction over the alpha-blend class only (clean_rt_common.glsl traceTransmittance),
// multiplies the result, skipped where the opaque rays are all blocked. The blocker
// distance (penumbra size) comes from opaque hits alone. Off = the mixed walk: every
// disc ray tests every translucent candidate it crosses.
bool rtSplitOn() { return (rtBlendOpts() & 2u) != 0u; }

// FOLIAGE TRANSMISSION (#735, clean_rt_common.glsl rt_fol_*): the sun / moon cores load
// it around their traces (rtFolBegin / rtFolEnd); the projector cores never do -- their
// rays keep every leaf face an occluder (mode 0).
void rtFolBegin() { rtFoliageSet(ssr.foliage_rt); }
void rtFolEnd()   { rtFoliageEnd(); }
// Mode 2's transmission ray (traceFoliageTransmit), only where the shadow rays left some
// light: v = their visibility. Colour follows RenderVKTranslucentShadows like the blend
// casters' (#212).
vec3 rtFolTransmit(vec3 v, vec3 origin, vec3 dir, float tmax)
{
    if (rtFolMode() != 2u || !(max(max(v.r, v.g), v.b) > 0.0)) return v;
    return v * traceFoliageTransmit(origin, dir, 0.0, tmax, specTransOn());
}

vec3 shadowRayTrans(vec3 origin, vec3 dir, float tmin, float tmax)
{
    rt_blend_parity = (rtBlendOpts() & 1u) != 0u;
    const vec3 v = traceTransmittance(origin, dir, tmin, tmax, specTransOn());
    rt_blend_parity = false;
    return v;
}

// One shadow ray of a disc (or the hard ray): split = opaque set only, binary (the
// RT_WALK_VIS walk), else the mixed walk (shadowRayVis). ONE inlined walk either way
// (compile cost: clean_rt_common.glsl RT_WALK_*). <SS:Skoomastream> Under parity
// (RenderVKRTBlendShadowParity) the split's opaque walk takes the blend class too and
// blocks on its LL-solid texels (traceShadowRayVisSplit); the mixed walk is shadowRayVis.
vec3 shadowDiscRay(vec3 origin, vec3 dir, float tmin, float tmax, bool split)
{
    rt_blend_parity = !split && (rtBlendOpts() & 1u) != 0u;
    const vec3 v = rtVisWalkF(origin, dir, tmin, tmax,
                              split ? (RT_MASK_SHADOW & ~RT_MASK_BLEND) : RT_MASK_SHADOW,
                              split ? RT_WALK_VIS : (specTransOn() ? RT_WALK_RGB : RT_WALK_GREY), RT_WALK_KS_DISC);
    rt_blend_parity = false;
    return v;
}

// ADAPTIVE SOFT-SHADOW SAMPLING: soft shadows trace the disc's OUTERMOST samples
// first (Vogel index n-1, n-2: largest radii, ~137 deg apart). With 3+ rays, if those
// two agree that the light is fully visible or fully blocked (no translucent partial),
// the pixel can't be in a penumbra and the remaining rays are skipped -- most pixels
// are fully lit or in umbra, so the extra rays go only to shadow edges. (A caster
// thinner than the gap between the two outer rays can be missed inside a soft edge.)
// TEMPORAL HINT (shadow mask only): last frame's accumulated visibility for this
// light at this pixel when it was uniform -- 1 fully lit, 0 fully blocked, < 0 none.
// The first (outermost) ray agreeing with it ends the sampling: settled lit / umbra
// pixels cost one ray instead of two to four, and only penumbrae and changes take the
// full disc. The disc rotates per TAA phase, so a thin caster missed by one frame's
// first ray is caught by a later one and the pixel drops back to full sampling.
float rt_vis_hint = -1.0;
bool rtHintDone(uint k, vec3 vis)
{
    return k == 1u && rt_vis_hint >= 0.0 && all(equal(vis, vec3(rt_vis_hint)));
}

// AGREEING TRANSLUCENT RAYS (RenderVKRTShadowAdaptiveTol, 1/256 units in lit_dbg.w bits
// [8,16); 0 = off): the two outer rays also settle the pixel when they AGREE within the
// tolerance on a partial value -- light through the same translucent caster (a pool's
// water, a glass pane). Its transmittance doesn't change across the disc, so the
// remaining rays (each another walk through that caster's candidates) add nothing but
// cost. Opaque edges (0 vs 1) never agree. first = the first ray's visibility.
bool rtAdaptiveDone(uint k, uint n, vec3 vis, vec3 first)
{
    if (k != 2u || n < 3u) return false;
    const vec3 h = vis * 0.5;
    if (all(equal(h, vec3(0.0))) || all(equal(h, vec3(1.0)))) return true;
    const float tol = float((rtBlendOpts() >> 8u) & 0xFFu) * (1.0 / 256.0);
    const vec3 d = abs((vis - first) - first);   // second ray - first ray
    return tol > 0.0 && max(max(d.r, d.g), d.b) <= tol;
}

// RECEIVER-PIXEL FOOTPRINT of a shadow ray's alpha test (clean_rt_common.glsl
// rt_foot_*). The receiver at P (geometric normal Ng) is seen through one RENDER pixel of
// the main view: angle 2 / (P[1][1] x render height) (taa_cur_vp is unjittered, rotation
// only: its clip-y row has length P[1][1]; mirror_params.z = 1 / render height). On the
// receiver's plane that pixel is camd * angle across the view and camd * angle / N.V
// along it -- the ellipse whose screen derivatives the raster's textureGrad-equivalent
// (bias + anisotropic sampler) used to alpha-test the receiver and the layers above it.
// The material mip bias the raster adds while jittered (taaLodBias: TAA sharpening + the
// render-scale term) scales it the same way. This replaced a fixed 1 mrad per metre
// isotropic cone (camd * 0.001 / |n.d|): resolution-, FOV- and view-angle-blind, so at
// grazing views past the sampler's anisotropy cap the raster's alpha test ran at a
// COARSER mip than the shadow rays. Shell grass (stacked planes of sparse dots) then
// showed lower shells on screen (upper dots averaged away) while the rays still met the
// upper shells' dots: a dark, speckled self-shadow BAND at mid distance, following the
// camera (near: both at mip 0; far: both averaged away). Supersampled snapshots (finer
// raster mips) did not show it.
void rtSetShadowFootprint(vec3 P, vec3 Ng, vec3 cam)
{
    rt_foot_on = false;
    const vec3 toC = cam - P;
    const float camd = length(toC);
    const float p11 = length(vec3(ssr.taa_cur_vp[0][1], ssr.taa_cur_vp[1][1], ssr.taa_cur_vp[2][1]));
    if (camd < 1e-4 || p11 < 1e-6 || ssr.mirror_params.z <= 0.0) return;   // (the cone fallback)
    const vec3 V = toC / camd;
    vec3 N = Ng;
    if (dot(N, N) < 0.5) N = V;
    if (dot(N, V) < 0.0) N = -N;
    const float nv = max(dot(N, V), 1.0 / 64.0);
    const float bias = ssr.taa_params.w > 0.5 ? ssr.taa_scale.x : 0.0;
    const float w = camd * 2.0 * ssr.mirror_params.z / p11 * exp2(bias);
    vec3 t1 = cross(N, V);
    if (dot(t1, t1) < 1e-8) t1 = cross(N, abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
    t1 = normalize(t1);
    const vec3 t2 = cross(t1, N);   // in the plane, along the view's projection
    rt_foot_a = t1 * w;
    rt_foot_b = t2 * (w / nv);
    rt_foot_on = true;
}

// SHADOW TERMINATOR (Hanika 2021, "Hacking the Shadow Terminator", Ray Tracing Gems II
// ch. 4; RenderVKRTShadowTerminatorOffset, rtBlendOpts bit 3, default on). A smooth-
// shaded low-poly mesh (a garment, a body, a sculpt) is lit by its interpolated vertex
// normal, but its shadow rays leave the FLAT triangle. Near the sun terminator a facet
// whose own plane already faces away from the sun (Ng.L < 0) while its shading normal
// still faces it (Ns.L > 0) sends its rays into the mesh -- the origin goes to the
// light-facing (back) side -- and they hit the far side: the whole TRIANGLE is black
// while the lit term says lit. Facets just above grazing meet their concave neighbours
// the same way. That decides the shadow per triangle: faceted dark patches.
// Fix: the origin moves to the point the vertex normals imply -- per vertex i, the
// receiver projected onto the tangent plane of (p_i, n_i) where it lies BELOW that plane,
// blended by the barycentrics: lift = -sum b_i min(0, (P - p_i).n_i) n_i. Flat-shaded
// geometry (vertex normals = facet normal: shell grass, prims, terrain planes) and
// concave regions get no lift. The triangle comes from a short force-opaque camera ray
// through the receiver (the mask's maskProbeNormal ray): static / worn vertex normals
// from the mega-VBO through the instance cofactor; RIGGED the skinned, welded vertex
// normals the rig copy stores beside the BLAS positions (clean_rt_common.glsl
// rtRigNormal) -- the raster's own shading normals, joint bends included. A miss means
// the receiver is not in the TLAS, so it cannot self-shadow either: no lift needed.
// STACKED LAYERS / CONTACT: the lift is bounded by the triangle (sum b_i |(P - p_i).n_i|
// <= its extent x the normals' deviation) and is ESCAPE-CHECKED: one force-opaque ray
// from the receiver to the lifted origin; anything in between (a clothing layer, a
// shell, a contact blocker) shortens the lift to half the free distance below it, so a
// lift never jumps a layer or slips under a touching caster.
// EVERY RT SHADOW ORIGIN (#664): the sun / moon (mask + inline lit FS), the ray-traced
// PROJECTORS (mask + inline lit FS; the same light-independent probe, shared through
// rtTerminatorCached) and the reflection-probe capture's sun rays at ray hits (exact from
// the hit triangle itself, rtSurfaceEval -> rtHitSunShadow; no camera probe needed).
// God rays march the view ray, not a surface: nothing to lift.
#ifdef CLEAN_RT_SUN_HARD
bool rtTerminatorOn() { return false; }   // (#829 capture FS: no terminator probe)
#else
bool rtTerminatorOn() { return (rtBlendOpts() & 8u) != 0u; }
#endif
// BACK-FACE SELF-SHADOW (RenderVKRTBackfaceSelfShadow, frame info foliage_rt.x bit 4, default
// on; every RT shadow core: sunShadowRayCoreImpl, projShadowRayCoreImpl, clean_shadow_core
// shadowSampleVis). An UNLIFTED receiver whose geometric facet, on the side the camera sees,
// faces away from the light (N.l <= 0) is shadowed by its own surface -- visibility 0, no ray --
// and every unlifted origin leaves along that VISIBLE side. The old rule flipped the origin
// normal toward the light ("the light-facing side"): for a receiver turned away that is the far
// side of its own surface, and on a single-plane / thin wall the receiver offset (depth error +
// RenderVKRTShadowBias + the #747 / slope offsets) put the origin BEHIND the wall -- the ray
// never met it and a projector in the next room lit this room's wall (a crisp gobo spot where
// a normal map tilted the shading normal toward it). Terminator-LIFTED origins (#664) keep
// their rule (the lift already sits on the visible side, above the facet). 0 = the old flip.
// Particles (CLEAN_NO_TANGENT_IN: a camera-facing billboard, no surface of its own) keep the flip.
#ifdef CLEAN_NO_TANGENT_IN
bool rtBackfaceSelfShadow() { return false; }
#else
bool rtBackfaceSelfShadow() { return (uint(ssr.foliage_rt.x + 0.5) & 16u) != 0u; }
#endif
// N on the side of the surface the camera sees.
vec3 rtVisibleSide(vec3 N, vec3 P, vec3 cam) { return dot(N, cam - P) < 0.0 ? -N : N; }
// The band where a smooth-shaded facet can self-shadow toward a light (#664): ngl =
// dot(facet normal on the VISIBLE side, direction to the light). A ray leaving a convex
// facet higher than that can't meet its neighbours; well below it the lit term is 0.
bool rtTerminatorBand(float ngl) { return ngl > -0.5 && ngl < 0.2; }
// Read by sunShadowRayCoreImpl / projShadowRayCoreImpl (set by the caller around the
// core, then reset).
bool rt_term_on   = false;
vec3 rt_term_lift = vec3(0.0);   // world offset of the ray origin (0 = none / cancelled)
// CLUSTER LOD RECEIVER LIFT (#747 interim; set per light by clean_rt_shadow_mask.comp, 0
// everywhere else): extra metres the origin leaves the receiver along its light-facing
// normal when the receiver is the cluster surface of an avatar whose shadows still come
// from its LEGACY rigged geometry (llcleanclusterrtvk.h RIGGED) -- the bound of the distance
// between the two surfaces, so a ray never starts inside the avatar's own legacy hull.
float rt_cl_lift  = 0.0;
vec3 rt_term_ns   = vec3(0.0);   // interpolated vertex normal, visible side (unit)
// Written by rtTerminatorCached with its outputs (#664 round 2): the triangle's
// tessellation angle (rt_tri_dev, below) and the escape check's verdict (rt_esc_flag).
float rt_term_dev = 0.0;
float rt_term_esc = 0.0;
vec3  rt_term_ng  = vec3(0.0);   // the probed facet normal, visible side
// Set by rtTerminatorTri: the sine of the largest angle between a (used) vertex normal
// and the facet -- how far the smooth surface the shading assumes departs from the flat
// triangle the rays leave and hit, i.e. the angular resolution of the mesh there.
float rt_tri_dev = 0.0;
// Set by rtTerminatorEscape: 0 lift kept, 0.5 shortened, 1 cancelled (debug view).
float rt_esc_flag = 0.0;
// Set by rtTerminatorTri: the lift hit its size cap (#664 round 4; debug view).
bool rt_lift_capped = false;
// Set by rtTerminatorCached for THIS receiver (#664 round 4): the probe found a triangle
// but it failed validation (not the rasterized surface), so no lift. Cleared by the core
// wrappers after rtTerminatorPost (debug view only).
bool rt_term_rej = false;
bool rt_term_cap = false;
// Probe validation (rtTerminatorProbe): the least cosine between the probe's facet and
// the caller's receiver normal. Lit FS: the raster triangle's own plane (screen
// derivatives, exact up to BLAS float noise). Shadow mask: the depth-plane normal, which
// near a facet edge may come from the neighbouring facet of a coarse limb (45 deg on an
// octagon) -- looser, the hit-distance test does the rest there.
const float RT_TERM_FACE_COS = 0.85;
const float RT_TERM_MASK_COS = 0.6;

// GRAZING FADE (#664 round 2, DEFAULT OFF since round 3 -- A/B only: it also lit shadows
// cast by OTHER casters inside the (0, w) band, thin bright lines on curved limbs, and the
// #666 projector denoiser already averages the sawtooth out; RenderVKRTShadowGrazingFade,
// rtBlendOpts bits [5,8) in
// halves). Near-tangent shadow rays on a smooth-shaded low-poly mesh are
// decided by millimetre facet detail: the Hanika origin rises and falls with the
// facet's sagitta (0 at the edges, max mid-facet) and the casters further along the limb
// are polygons; at a grazing angle a with N.L = sin a, a height difference h moves the
// shadow boundary by h / tan a along the surface. A projector straight down a nearly
// vertical leg (N.L ~ 0 across the whole limb) turned the sagitta (a few mm) and the
// upper leg's facet corners into a SAWTOOTH of shadow spikes running down the leg,
// periodic with the facets. Those rays are below the mesh's own angular resolution
// (rt_tri_dev), so their answer is tessellation noise, not the smooth surface the lit
// FS shades. The visibility is blended toward lit as the SMOOTH N.L falls through
// [0, w], w = k x rt_tri_dev (capped 0.25): the light there is already scaled by that
// N.L, so the shading falls off smoothly to the terminator by N.L (what the smooth
// surface does) instead of by facet noise. Fine meshes (small deviation) are barely
// touched; Ns.L <= 0 keeps the traced value (unlit anyway). Only probed receivers
// (rt_term_on: in the band, triangle found) are faded.
float rtGrazingFadeK() { return float((rtBlendOpts() >> 5u) & 7u) * 0.5; }
float rtTerminatorFadeW(float k, float dev) { return min(k * dev, 0.25); }
float rtTerminatorFadeT(float x, float w)
{
    return (w > 0.0 && x > 0.0) ? smoothstep(0.0, w, x) : 1.0;
}
// TERMINATOR DEBUG VIEW (RenderVKRTShadowDebug 8 -> frame info lit_dbg.x bit 16): every
// RT shadow core returns, per light, in the band:
//   PRE-PASS TRIANGLE path (#664, clean_term_tri.frag): R = 1, G = lift length (5 mm =
//   full), B = escape check cancelled (1) / shortened (0.5) the lift -- red / yellow /
//   white family.
//   CAMERA-PROBE path (fallback / RenderVKRTTerminatorSource 0): R = 0, G = lift, B = 1
//   -- blue / cyan family; MAGENTA = the probe's triangle failed validation (no lift).
// Black = outside the band (or no triangle). Shown through the light itself.
#ifdef CLEAN_DEBUG_CONST   // (the lit FS: only its debug twin keeps the view, clean_draw_common.glsl)
bool rtTermDebug() { return kDebugViews && (uint(ssr.lit_dbg.x + 0.5) & 0x10000u) != 0u; }
#else
bool rtTermDebug() { return (uint(ssr.lit_dbg.x + 0.5) & 0x10000u) != 0u; }
#endif
bool rt_term_src_tri = false;   // set by rtTerminatorCached: the pre-pass triangle served
vec3 rtTerminatorPost(vec3 vis, vec3 l)
{
    if (rtTermDebug())
    {
        if (!rt_term_on) return rt_term_rej ? vec3(1.0, 0.0, 1.0) : vec3(0.0);
        const float g = clamp(length(rt_term_lift) / 0.005, 0.0, 1.0);
        if (rt_term_src_tri) return vec3(1.0, g, rt_term_esc >= 1.0 ? 1.0 : (rt_term_esc > 0.0 ? 0.5 : 0.0));
        return vec3(0.0, g, 1.0);
    }
    if (!rt_term_on) return vis;
    float t = rtTerminatorFadeT(dot(rt_term_ns, l), rtTerminatorFadeW(rtGrazingFadeK(), rt_term_dev));
    // Continuous at the band's upper edge (rtTerminatorBand: facet N.l = 0.2), where the
    // probe -- and with it the fade -- stops: neighbouring facets either side of it must
    // not switch between faded and unfaded (a very coarse mesh's smooth N.l can still be
    // under w there).
    t = max(t, smoothstep(0.1, 0.2, dot(rt_term_ng, l)));
    return mix(vec3(1.0), vis, t);
}

// HANIKA LIFT of a known triangle (#664): the raw (not escape-checked) lift at
// barycentrics b of RT instance id's triangle prim (BLAS space; object-to-world o2w),
// on the side `side` points to (toward the viewer / the arriving ray). Out: lift (world,
// 0 = none), ns (interpolated vertex normal, that side, unit), ng (the facet normal,
// that side, unit), Ph (the exact barycentric point). False = degenerate triangle.
// Shared by the camera probe (rtTerminatorProbe: lit FS + shadow mask, sun and
// projectors) and the reflection-probe capture's ray hits (clean_rt_surface.glsl
// rtSurfaceEval -> clean_rt_shade.glsl rtHitSunShadow).
bool rtTerminatorTri(uint id, uint prim, vec3 b, mat4x3 o2w, vec3 side,
                     out vec3 lift, out vec3 ns, out vec3 ng, out vec3 Ph)
{
    lift = vec3(0.0); ns = vec3(0.0); ng = vec3(0.0); Ph = vec3(0.0);
    rt_tri_dev = 0.0;
    rt_lift_capped = false;
    if (!rtIdIn(id)) return false;   // (#874 guard: no triangle)
    vec3 p0, p1, p2;
    rtTriPositions(id, prim, p0, p1, p2);
    const vec3 w0 = o2w * vec4(p0, 1.0), w1 = o2w * vec4(p1, 1.0), w2 = o2w * vec4(p2, 1.0);
    const vec3 c = cross(w1 - w0, w2 - w0);
    const float cl = length(c);
    if (!(cl > 1e-20)) return false;
    Ph = w0 * b.x + w1 * b.y + w2 * b.z;
    // The visible side: the raster flips the vertex normal with gl_FrontFacing (winding).
    const float sd = dot(c, side) < 0.0 ? -1.0 : 1.0;
    ng = c / cl * sd;
    const uvec3 t = rtTriIndices(id, prim);
    const uint vb = rt_geom[id].y;
    const bool rigged = (rt_geom[id].z & 0x80000000u) != 0u;
    // Static / worn: the cofactor of the instance transform (clean_draw.vert), sign(det).
    // Rigged: the BLAS is the owner avatar's pelvis-local space (#632; o2w = its rigid
    // pelvis transform, identity for a world-space group) and its rig_pos rows carry the
    // SKINNED, WELDED vertex normals in that space (rtRigNormal, rotated by o2w) --
    // exactly what the rigged raster interpolates. (The bind-pose normals through one per-triangle affine map, the first
    // cut, are exact only when all three vertices share one rotation: across a joint --
    // a knee, an elbow, a bent garment -- each vertex's normal turns with its own bone
    // blend, and the per-triangle map averaged that bend away, under-lifting exactly the
    // facets whose shading normal leans furthest off the facet: the stepped notches
    // left along the inner knee / thigh fold.)
    const mat3 M3 = mat3(o2w);
    const float det = dot(M3[0], cross(M3[1], M3[2]));
    const mat3 Nm = (det < 0.0 ? -1.0 : 1.0) * mat3(cross(M3[1], M3[2]), cross(M3[2], M3[0]), cross(M3[0], M3[1]));
    vec3 nsum = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        // The corner by compare, not a private vec3[3] / uint[3] indexed by i (local memory).
        const vec3 pvi = i == 0 ? w0 : (i == 1 ? w1 : w2);
        const uint ivi = i == 0 ? t.x : (i == 1 ? t.y : t.z);
        vec3 n = rigged ? M3 * rtRigCornerNormal(id, prim, uint(i), ivi) : Nm * rtVertNormal(vb + ivi);
        const float nl = dot(n, n);
        n = (nl > 1e-20 && !any(isnan(n))) ? n * (inversesqrt(nl) * sd) : ng;
        // A vertex normal on the hidden side of its facet (inside-out / broken normals):
        // no tangent plane to trust -- the facet itself (no lift from this vertex).
        if (dot(n, ng) < 0.05) n = ng;
        rt_tri_dev = max(rt_tri_dev, length(cross(n, ng)));
        nsum += n * b[i];
        lift -= min(dot(Ph - pvi, n), 0.0) * b[i] * n;
    }
    const float nsl = dot(nsum, nsum);
    ns = nsl > 1e-20 ? nsum * inversesqrt(nsl) : ng;
    // OUTWARD ONLY, ALONG THE FACET NORMAL (#664 round 4; Hanika: "only move points
    // outward"). The barycentric blend of the vertex-normal projections also has a
    // TANGENTIAL part, |normal part| x tan(normal deviation): small on a smooth convex
    // limb, but in CREASES (between the legs, under the tail, the inner thigh) the
    // skinned, welded vertex normals lean tens of degrees off the facets and the blend
    // slid the origin SIDEWAYS by millimetres to centimetres -- out from under an
    // overhang (thin squiggly streaks of light inside a crease shadow) or in under one
    // (flecks of shadow on a lit inner leg), changing with every breathing frame as the
    // skinned normals turned, and only visible up close. The escape check could not see
    // it: nothing lies between the receiver and the slid origin, the origin just starts
    // its shadow ray from a different place. Only the height above the facet is kept.
    // SIZE CAP (safety net): each vertex term is at most |P - p_i| x deviation, so on any
    // mesh whose normals describe a smooth surface the blend stays under ~0.6 x edge_max
    // x deviation (an n-gon limb's sagitta is ~ edge x deviation / 4). Capped at 0.75 x
    // edge_max x deviation: never binds on the convex terminator case, bounds degenerate
    // normal sets.
    const float lh = dot(lift, ng);
    if (any(isnan(lift)) || !(lh > 0.0)) { lift = vec3(0.0); return true; }
    const vec3 e01 = w1 - w0, e12 = w2 - w1, e20 = w0 - w2;
    const float emax = sqrt(max(max(dot(e01, e01), dot(e12, e12)), dot(e20, e20)));
    const float cap = 0.75 * emax * rt_tri_dev;
    rt_lift_capped = lh > cap;
    lift = ng * min(lh, cap);
    return true;
}

// ESCAPE CHECK: nothing between the receiver (Ps: just off its visible side) and the
// lifted origin P + lift, else the lift is shortened (stacked layers, shells, a
// touching caster). Returns the usable lift (0 = none).
vec3 rtTerminatorEscape(vec3 P, vec3 Ps, vec3 ng, vec3 lift)
{
    rt_esc_flag = 0.0;
    if (!(dot(lift, lift) > 0.0)) return vec3(0.0);
    const vec3 sv = P + lift - Ps;
    const float sl = length(sv);
    if (dot(sv, ng) > 0.0 && sl > 1e-6)
    {
        // Closest hit (no terminate-on-first): a blocker part-way up keeps the lift
        // that fits below it -- half the free distance, so the origin stays clearly
        // under that layer -- instead of dropping it altogether. A cancelled lift
        // put the origin back on the facet, whose own plane faces away from the
        // sun: the whole triangle self-shadowed again (a hard facet-shaped notch
        // wherever a second layer -- a hem, a fold of the same garment, a lining --
        // sat within the lift height).
        vec3 eo = Ps, ed = sv / sl;
        float et0 = 0.0, et1 = sl;
        rtRaySane(eo, et0, ed, et1);   // (Xid 109: an invalid ray becomes the empty one -- no blocker)
        rayQueryEXT eq;
        rayQueryInitializeEXT(eq, uTLAS, gl_RayFlagsOpaqueEXT, RT_MASK_SHADOW & 0xFFu, eo, et0, ed, et1);
        while (rayQueryProceedEXT(eq)) {}
        if (rayQueryGetIntersectionTypeEXT(eq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        {
            // Fraction of the receiver -> lifted-origin segment that is free; Ps sits
            // just off P, so scale the lift (from P) by the same fraction.
            const float f = 0.5 * rayQueryGetIntersectionTEXT(eq, true) / sl;
            lift = f > 0.05 ? lift * f : vec3(0.0);
            rt_esc_flag = f > 0.05 ? 0.5 : 1.0;
        }
    }
    return lift;
}

// P: receiver, cam: camera, ng_ref: the receiver's geometric normal as the caller knows
// it (the raster facet / the mask's depth-plane normal; 0 = unknown), min_cos: how far
// the probe's facet may turn from it. Out: lift (world), ns (visible-side interpolated
// vertex normal), ng (visible-side facet normal). 0 = no triangle, 1 = probed,
// 2 = REJECTED (#664 round 4): the probe's closest hit is not the rasterized surface. The lift is RAW:
// rtTerminatorCached runs the escape check (#1042: one site for both sources).
// The probe ray runs from w in front of P: where another surface lies within w of P
// along the view -- the far side of a CREASE close to its fold line, at the 1 mm floor
// of w several pixels wide up close -- the first hit was THAT surface, and the lift
// computed from its triangle / normals was applied to this receiver. Validated by the
// hit's distance (P must lie within w / 2 of the hit, the depth reconstruction's
// error is w / 4) and its facet orientation (both on the visible side).
int rtTerminatorProbe(vec3 P, vec3 cam, vec3 ng_ref, float min_cos, out vec3 lift, out vec3 ns, out vec3 ng)
{
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
    // VALIDATION (#664 round 4): the hit must be the receiver's own surface.
    bool ok = abs(th - w) <= 0.5 * w;
    if (ok && dot(ng_ref, ng_ref) > 0.5)
    {
        const vec3 r = normalize(ng_ref) * (dot(ng_ref, cam - P) < 0.0 ? -1.0 : 1.0);   // visible side
        ok = dot(r, ng) >= min_cos;
    }
    if (!ok) { lift = vec3(0.0); ns = vec3(0.0); ng = vec3(0.0); rt_lift_capped = false; return 2; }
    return 1;   // (the raw lift: rtTerminatorCached escape-checks either source's once)
}

// PER-RECEIVER CACHE (#664): the probe is light-independent (the triangle, its vertex
// normals, the escape check), so the sun and every ray-traced projector at one receiver
// share ONE probe, run lazily by the first light whose terminator band needs it. GLSL
// globals are private to the invocation; keyed by P, so shading another receiver
// re-probes.
int  rt_tc_state = -1;   // -1 not probed, 0 no triangle, 1 probed, 2 rejected
vec3 rt_tc_P    = vec3(0.0);
vec3 rt_tc_lift = vec3(0.0);
vec3 rt_tc_ns   = vec3(0.0);
vec3 rt_tc_ng   = vec3(0.0);
float rt_tc_dev = 0.0, rt_tc_esc = 0.0;
bool  rt_tc_cap = false;
// PRE-PASS TRIANGLE (#664, RenderVKRTTerminatorSource 1): clean_term_tri.frag wrote,
// per render pixel, the Hanika lift of the EXACT rasterized triangle (after the depth
// pre-pass, depth EQUAL): no camera-ray probe, no wrong-surface pick in a crease or at
// a weld seam, no BLAS-vs-raster mismatch, no validation thresholds. RT consumer set
// binding 10 (R32G32_UINT; layout in clean_term_tri.frag), bound and written this frame
// when frame info lit_dbg.x bit 17 is set. 0 = no data (cleared): the camera probe below
// serves instead.
#extension GL_EXT_samplerless_texture_functions : require
layout(set = RT_SET, binding = 10) uniform utexture2D uTermTri;
bool rtTermTriBound() { return (uint(ssr.lit_dbg.x + 0.5) & 0x20000u) != 0u; }
vec3 rtOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
// px: the receiver's render pixel (< 0 = none). True = data: the RAW lift (world; rtTerminatorCached
// escape-checks it), ns / ng (visible side). No ray.
bool rtTerminatorTriAt(ivec2 px, vec3 P, out vec3 lift, out vec3 ns, out vec3 ng)
{
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
bool rt_tc_tri = false;
// ng_ref / min_cos: rtTerminatorProbe's validation (per receiver, so the cache key P
// covers them). px: the receiver's render pixel for the pre-pass triangle (< 0: none --
// alpha-blend receivers, particles, mirror / probe views), else the probe.
// ONE ESCAPE CHECK (#1042): whichever source served, its lift is escape-checked here, once -- the
// generic shadow core's form (it carried its own copy of these functions with this single site, while
// this copy ran a check inside each source). Every consumer now shares this one implementation.
bool rtTerminatorCached(vec3 P, vec3 cam, vec3 ng_ref, float min_cos, ivec2 px, out vec3 lift, out vec3 ns, out vec3 ng)
{
    if (rt_tc_state < 0 || any(notEqual(rt_tc_P, P)))
    {
        rt_tc_tri = rtTerminatorTriAt(px, P, rt_tc_lift, rt_tc_ns, rt_tc_ng);
        rt_tc_state = rt_tc_tri ? 1 : rtTerminatorProbe(P, cam, ng_ref, min_cos, rt_tc_lift, rt_tc_ns, rt_tc_ng);
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

// Ray-traced path: one shadow ray from the receiver toward the projector apex, against
// the scene TLAS (every caster class but water; alpha-mask / blend casters run the
// shared candidate alpha test of clean_rt_common.glsl). Double-sided: SL content is
// full of single-sided and inside-out geometry. The ray stops at the projector's NEAR
// plane (record params.y = near distance from the apex): the fixture prim between the
// apex and its front face never occludes its own light -- the same clip the shadow map
// applies. Soft shadows: the target point is jittered on a disc of radius
// texel.x (RenderVKRTShadowSoftness, metres) around the apex, perpendicular to the
// axis, texel.z stratified samples per pixel (Shadow quality: 1-4) with IGN.
// <GMDX> No longer double-sided: single-sided faces turned away from the light pass, as
// in LL's back-face-culled spot shadow maps and the projector tiles (rt_proj_facing).
vec3 projShadowRayCoreImpl(Light L, ProjShadow s, vec3 P, vec3 Ng, vec3 cam, float time, vec2 npx, uint max_n)
{
    const vec3  O    = L.proj_origin.xyz;
    const vec3  A    = L.proj_axis.xyz;
    const float near = s.params.y;
    const float camd = length(P - cam);
    // Origin: off the receiver along its light-facing geometric normal by the float
    // error of the coordinates + depth reconstruction (clean_rt_common.glsl
    // rtReceiverOrigin), plus the user's extra offset (texel.y = RenderVKRTShadowBias, m).
    rt_anim_time   = time;
    rt_cone_spread = 0.0;
    rt_cone_width0 = camd * 0.001;             // (fallback only: rtSetShadowFootprint's rt_foot_* override it)
    rt_cone_aniso  = 1.0;
    const float soft = s.texel.x;
    // Hard shadows need one ray; soft ones take texel.z (Shadow quality: 1-4) rays
    // on a Vogel disc: sample i at radius sqrt((i + u) / n), golden-angle spaced, the
    // whole pattern rotated (theta) and its radii offset (u) by the per-pixel noise.
    // Stratified, so n rays leave far less grain than n independent ones.
    const uint n = soft > 0.0 ? clamp(uint(s.texel.z + 0.5), 1u, max(max_n, 1u)) : 1u;
    vec3 u = vec3(0.0), v = vec3(0.0);
    float theta = 0.0, ur = 0.0;
    if (soft > 0.0)
    {
        u = normalize(abs(A.z) < 0.9 ? cross(A, vec3(0.0, 0.0, 1.0)) : cross(A, vec3(1.0, 0.0, 0.0)));
        v = cross(A, u);
        ur    = fract(projIGN(npx) + rt_noise_shift.x);
        theta = 6.2831853 * fract(projIGN(npx.yx + vec2(17.0, 59.0)) + rt_noise_shift.y);
    }
    const bool split = rtSplitOn();
    rt_proj_facing = true;   // <GMDX> LL's culled spot shadow pass (clean_rt_common.glsl)
    // TERMINATOR (#664; rtTerminatorProbe, set by the caller in the projector's band):
    // a lifted origin stays on the receiver's VISIBLE side -- above the facet, on the
    // surface the vertex normals describe -- even where the facet's own plane faces away
    // from this projector, instead of being pushed to the back side (into the mesh: the
    // whole triangle black while the lit FS, by the smooth normal, says lit). Unlifted:
    // each ray's light-facing side, as always.
    const bool lifted = rt_term_on && dot(rt_term_lift, rt_term_lift) > 0.0;
    const vec3 Pb = lifted ? P + rt_term_lift : P;
    vec3 Nl = Ng;
    if (lifted && dot(Nl, rt_term_lift) < 0.0) Nl = -Nl;
    // BACK-FACE SELF-SHADOW (rtBackfaceSelfShadow): unlifted, the visible facet turned away
    // from the apex is its own occluder (no ray: its origin would leave through the surface),
    // else every ray leaves along the visible side.
    const bool bfs = !lifted && rtBackfaceSelfShadow();
    if (bfs)
    {
        Nl = rtVisibleSide(Ng, P, cam);
        if (!(dot(Nl, O - P) > 0.0)) return vec3(0.0);
    }
    // TERMINATOR-SAFE DISC (RenderVKRTShadowTerminatorFloor, frame info taa_scale.w, in
    // disc radii; 0 = off; as sunShadowRayCoreImpl): the disc samples that leave the
    // surface lower than the centre ray skim the next facet of a smooth-shaded low-poly
    // limb near the terminator -- some hit it, some miss: dotted self-shadow lines along
    // the triangle edges. Samples whose elevation above the shading surface (the
    // interpolated vertex normal when probed, else the facet) is below
    // min(N.l, f x the disc's angular radius) are lifted to it, tangential part kept.
    vec3 Nf = rt_term_on ? rt_term_ns : Ng;
    const vec3 toO = O - Pb;
    const float dO = length(toO);
    if (dot(Nf, toO) < 0.0) Nf = -Nf;
    const float efloor = (soft > 0.0 && dO > 1e-4)
                       ? min(clamp(dot(Nf, toO) / dO, 0.0, 1.0), ssr.taa_scale.w * soft / dO) : -2.0;
    vec3 vis = vec3(0.0), first = vec3(0.0);
    float rays = float(n);   // the normaliser: rays actually traced (early-outs below)
    for (uint k = 0u; k < n; ++k)
    {
        if (rtHintDone(k, vis)) { rays = 1.0; break; }
        if (k == 1u) first = vis;
        if (rtAdaptiveDone(k, n, vis, first)) { rays = 2.0; break; }
        const uint i = n - 1u - k;   // outermost samples first (rtAdaptiveDone)
        vec3 T = O;
        if (soft > 0.0)
        {
            const float r   = soft * sqrt((float(i) + ur) / float(n));
            const float phi = theta + 2.3999632 * float(i);
            T += (u * cos(phi) + v * sin(phi)) * r;
        }
        vec3 Nr  = Nl;
        vec3 toL = T - Pb;
        if (!lifted && !bfs && dot(Nr, toL) < 0.0) Nr = -Nr;
        const vec3 Po = rtReceiverOrigin(Pb, Nr, camd, s.texel.y + rt_cl_lift);
        toL = T - Po;
        const float len = length(toL);
        if (len <= 1e-4) { vis += vec3(1.0); continue; }
        vec3 dir = toL / len;
        const float e = dot(Nf, dir);
        if (e < efloor) dir = normalize(dir + Nf * (efloor - e));
        // Distance along the ray to the near plane (dot(X - O, A) = near).
        const float da = dot(dir, A);         // < 0: travelling back toward the apex
        const float dp = dot(Po - O, A);
        float tmax = (da < -1e-6) ? (near - dp) / da : len;
        tmax = min(tmax, len) - 0.002;
        if (tmax <= 0.0) { vis += vec3(1.0); continue; }
        vis += shadowDiscRay(Po, dir, 0.0, tmax, split);
    }
    vis /= rays;
    // Split: the one translucent ray, toward the apex itself (the disc's centre), with
    // the same receiver offset and near-plane clip as a disc ray (the terminator lift
    // too; no disc floor: the centre ray never skims lower than the hard ray).
    if (split && any(greaterThan(vis, vec3(0.0))))
    {
        vec3 Nr  = Nl;
        vec3 toL = O - Pb;
        if (!lifted && !bfs && dot(Nr, toL) < 0.0) Nr = -Nr;
        const vec3 Po = rtReceiverOrigin(Pb, Nr, camd, s.texel.y + rt_cl_lift);
        toL = O - Po;
        const float len = length(toL);
        if (len > 1e-4)
        {
            const vec3 dir = toL / len;
            const float da = dot(dir, A);
            const float dp = dot(Po - O, A);
            float tmax = (da < -1e-6) ? (near - dp) / da : len;
            tmax = min(tmax, len) - 0.002;
            if (tmax > 0.0) vis *= shadowRayTrans(Po, dir, 0.0, tmax);
        }
    }
    rt_proj_facing = false;   // <GMDX>
    return vis;
}

// The projector core with the receiver-pixel alpha-test footprint (rtSetShadowFootprint).
vec3 projShadowRayCore(Light L, ProjShadow s, vec3 P, vec3 Ng, vec3 cam, float time, vec2 npx, uint max_n)
{
    rtSetShadowFootprint(P, Ng, cam);
    const vec3 v = projShadowRayCoreImpl(L, s, P, Ng, cam, time, npx, max_n);
    rt_foot_on = false;
    const vec3 r = rtTerminatorPost(v, normalize(L.proj_origin.xyz - P));
    rt_term_rej = false;
    return r;
}

// RAY-TRACED sun / moon visibility (#185). Rays toward the light against the scene
// TLAS (every caster class but water; the shared candidate alpha test for mask / blend
// casters). Soft: sun_rt.y rays on a Vogel disc across the sun's angular radius
// (sun_rt.x), rotated and radius-offset by the per-pixel / per-TAA-phase noise, as the
// projector rays. The origin leaves the surface along the light-facing GEOMETRIC
// normal N (the mask: the depth-derived facet normal; the lit FS: the triangle's plane
// from screen derivatives) by the float error of the coordinates and of the depth
// reconstruction (clean_rt_common.glsl rtReceiverOrigin) -- a fraction of a millimetre
// -- plus the user's extra offset RenderVKRTShadowBias (sun_rt.z, default 0). No
// grazing-angle growth: a ray leaving a convex facet at a positive elevation can't
// meet the facets around it, and the old up-to-4x offset (4 cm at the 1 cm bias)
// jumped the stacked layers of mesh avatars and pushed origins of light-flipped
// receivers centimetres INTO heads (lit blotches, moving phantom shadows). Soft disc
// rays near the terminator are kept above the surface by the terminator floor below.
// The TLAS holds every instance within RenderVKRTRadius of the camera,
// frustum-independent, so off-screen casters shade what is in view.
// OPAQUE CUT-OUTS (opaque_only callers): rt_opaque_cutouts keeps the alpha-mask
// casters' alpha test in the opaque-only trace (traceShadowRayVis instead of the
// force-opaque traceShadowRayOpaque). Set by the lit FS for transparent receivers
// within RenderVKRTTransReceiverAlphaDist: an alpha-blend avatar overlay (blush,
// makeup) a few mm under a mostly cut-out alpha-mask layer (freckles, skin details,
// lashes, hair) was shadowed by that layer's whole triangles.
bool rt_opaque_cutouts = false;

// One sun ray: opaque_only = the opaque set (alpha-tested cut-outs when rt_opaque_cutouts,
// else the force-opaque fast path), otherwise shadowDiscRay. The cut-out case IS
// shadowDiscRay's split walk (RT_WALK_VIS over the same mask), so every candidate-testing
// walk of a sun ray is one call site (compile cost: clean_rt_common.glsl RT_WALK_*).
vec3 sunDiscRay(vec3 Po, vec3 d, float tmax, bool opaque_only, bool split)
{
    if (opaque_only && !rt_opaque_cutouts)
        return vec3(traceShadowRayOpaque(Po, d, 0.0, tmax, RT_MASK_SHADOW & ~RT_MASK_BLEND));
    return shadowDiscRay(Po, d, 0.0, tmax, split || opaque_only);
}
vec3 sunShadowRayCoreImpl(vec3 P, vec3 N, vec3 L, vec3 cam, float time, vec2 npx, bool opaque_only, uint max_n)
{
    const float camd = length(P - cam);
    vec3 Ng = N;
    // TERMINATOR (rtTerminatorProbe): a lifted origin stays on the receiver's VISIBLE side
    // even where the facet faces away from the sun -- it sits above the facet, on the
    // surface the vertex normals describe -- instead of being pushed to the back side
    // (into the mesh). Unlifted: the light-facing side, as always.
    const bool lifted = rt_term_on && dot(rt_term_lift, rt_term_lift) > 0.0;
    if (lifted) { if (dot(Ng, rt_term_lift) < 0.0) Ng = -Ng; }
    else if (rtBackfaceSelfShadow())
    {
        // BACK-FACE SELF-SHADOW (rtBackfaceSelfShadow): the visible side only; turned away from
        // the sun, its own surface is the occluder (no ray through it)
        Ng = rtVisibleSide(Ng, P, cam);
        if (!(dot(Ng, L) > 0.0)) return vec3(0.0);
    }
    else if (dot(Ng, L) < 0.0) Ng = -Ng;
    // The disc floor's frame: the shading surface when known (a terminator facet's own
    // plane faces away from the sun, so its floor would bend every soft ray sideways).
    vec3 Nf = Ng;
    if (rt_term_on) { Nf = rt_term_ns; if (dot(Nf, L) < 0.0) Nf = -Nf; }
    const float ndl = clamp(dot(Nf, L), 0.0, 1.0);
    const vec3 Po = rtReceiverOrigin(lifted ? P + rt_term_lift : P, Ng, camd, ssr.sun_rt.z + rt_cl_lift);
    rt_anim_time   = time;
    rt_cone_spread = 0.0;
    rt_cone_width0 = camd * 0.001;   // (fallback only: rtSetShadowFootprint's rt_foot_* override it)
    rt_cone_aniso  = 1.0;
#ifdef CLEAN_RT_SUN_HARD
    // (#829 capture FS) ONE ray straight at L: the sun's penumbra (0.27 deg; a few cm at
    // metres from the caster) is far below a probe texel (~0.7 deg), and a single jittered
    // disc ray would only add stipple the face's filtering can't average.
    const float ang = 0.0;
#else
    const float ang = ssr.sun_rt.x;
#endif
    const uint  n   = ang > 0.0 ? clamp(uint(ssr.sun_rt.y + 0.5), 1u, max(max_n, 1u)) : 1u;
    const float tmax = max(ssr.sun_rt.w, 1.0);
    // Transparent receivers (alpha-blend faces, particles): every caster treated as
    // opaque -- no per-candidate alpha test. Stacked blend layers each trace, and the
    // candidate test (foliage cut-outs) made them cost as much as the whole opaque
    // pass; a soft see-through surface doesn't show leaf-accurate shadow detail.
    // Also past RenderVKRTSunAlphaDist (frame info taa_scale.z; 0 = never), which
    // skips the candidate test at distance. Off by default -- treating leaf cards as
    // solid drew black rings around every canopy layer. The opaque-only trace LEAVES OUT
    // alpha-blend casters (glass, beams, invisiprims, big transparent domes): forced
    // opaque they blacked out everything behind them -- past 48 m the whole view went
    // dark. Blend casters only ever dimmed the light (translucent) in the full trace.
    const bool split = !opaque_only && rtSplitOn();
    // HARD shadow (no angular radius): the loop below with its one ray straight at L (no
    // disc offset, no terminator floor) -- the same rays and tails as a dedicated path, but
    // every walk stays a single call site (compile cost: clean_rt_common.glsl RT_WALK_*).
    const bool hard = n == 1u && ang <= 0.0;
    const vec3 u = normalize(abs(L.z) < 0.9 ? cross(L, vec3(0.0, 0.0, 1.0)) : cross(L, vec3(1.0, 0.0, 0.0)));
    const vec3 v = cross(L, u);
    const float ur    = fract(projIGN(npx + vec2(31.0, 7.0)) + rt_noise_shift.x);
    const float theta = 6.2831853 * fract(projIGN(npx.yx + vec2(3.0, 91.0)) + rt_noise_shift.y);
    const float tr = tan(ang);
    // TERMINATOR-SAFE DISC (RenderVKRTShadowTerminatorFloor, frame info taa_scale.w, in
    // disc radii; 0 = off): near the shadow terminator the receiver's plane cuts the sun
    // disc, and the disc samples that leave the surface LOWER than the centre ray skim
    // it -- along a smooth-shaded low-poly avatar or sculpt the faceted surface ahead
    // (the next facet, a wrist / elbow bend) is then hit by some of the jittered rays
    // and missed by the others: a stipple of self-shadow the hard ray never shows,
    // which the lit FS's own N.L (smooth normal) does not ask for. Samples whose
    // elevation above the receiver plane is below floor = min(Ng.L, f * tan(a)) are
    // lifted to it (tangential part kept): only receivers within ~(1 + f) disc radii
    // of grazing are touched -- every other penumbra keeps its full disc -- and at the
    // terminator no soft ray skims lower than the hard one does.
    const float efloor = min(ndl, ssr.taa_scale.w * tr);
    vec3 vis = vec3(0.0), first = vec3(0.0);
    float rays = float(n);   // the normaliser: rays actually traced (early-outs below)
    for (uint k = 0u; k < n; ++k)
    {
        if (rtHintDone(k, vis)) { rays = 1.0; break; }
        if (k == 1u) first = vis;
        if (rtAdaptiveDone(k, n, vis, first)) { rays = 2.0; break; }
        const uint i = n - 1u - k;   // outermost samples first (rtAdaptiveDone)
        vec3 d = L;
        if (!hard)
        {
            const float r   = tr * sqrt((float(i) + ur) / float(n));
            const float phi = theta + 2.3999632 * float(i);
            d = normalize(L + (u * cos(phi) + v * sin(phi)) * r);
            const float e = dot(Nf, d);
            if (e < efloor) d = normalize(d + Nf * (efloor - e));
        }
        vis += sunDiscRay(Po, d, tmax, opaque_only, split);
    }
    vis /= rays;
    // Split: the one translucent ray along the sun's centre (unlifted: the terminator
    // floor only matters for the skimming opaque self-hits).
    if (split && vis.x > 0.0) vis *= shadowRayTrans(Po, L, 0.0, tmax);
    // Foliage transmission (#735 mode 2): one ray along the centre too, as the split's.
    // Opaque-only receivers (transparent ones, past RenderVKRTSunAlphaDist) stay at mode 1:
    // their force-opaque rays already cull the away-facing leaf cards for free.
    if (!opaque_only) vis = rtFolTransmit(vis, Po, L, tmax);
    return vis;
}

// The sun / moon core with the receiver-pixel alpha-test footprint (rtSetShadowFootprint).
vec3 sunShadowRayCore(vec3 P, vec3 N, vec3 L, vec3 cam, float time, vec2 npx, bool opaque_only, uint max_n)
{
    rtSetShadowFootprint(P, N, cam);
    rtFolBegin();
    const vec3 v = sunShadowRayCoreImpl(P, N, L, cam, time, npx, opaque_only, max_n);
    rtFolEnd();
    rt_foot_on = false;
    const vec3 r = rtTerminatorPost(v, L);
    rt_term_rej = false;
    return r;
}
