// vkclean — DIFFUSE / SPECULAR DENOISER passes (#85 phase 2; LLCleanDenoiserVK): the pipeline
// layout both passes share -- set 0 = the draw set 0 (only the frame info is read: the motion /
// reset / jitter state, clean_frame_info.glsl), set 1 = the denoiser's own images
// (clean_pdn_common.glsl IMAGES), one push block.
//   clean_dn_temporal.comp  reprojection + history validation + clamp / anti-lag + accumulation
//   clean_dn_atrous.comp    one variance-guided a-trous iteration per dispatch
#include "clean_frame_info.glsl"
#include "clean_pdn_common.glsl"

layout(set = 1, binding = 0, rgba16f) uniform image2DArray uDnIn;     // producer's raw; a-trous scratch
layout(set = 1, binding = 1, rgba32ui) uniform uimage2D uDnMot;       // camera-only reprojection + expected depth
layout(set = 1, binding = 2, rgba32ui) uniform uimage2DArray uDnGeom; // guides, per parity
layout(set = 1, binding = 3, rgba32ui) uniform uimage2DArray uDnHist; // accumulated colour + variance, per parity
layout(set = 1, binding = 4, rgba16f) uniform image2DArray uDnOut;    // temporal output / a-trous ping-pong
layout(set = 1, binding = 5) uniform sampler2D uDnMotion;             // pre-pass motion vectors (render extent)

// 64 B (LLCleanDenoiserVK::Push).
layout(push_constant) uniform DnPush {
    vec4  dims;   // denoiser w, h; render w, h
    vec4  prm0;   // x diffuse max history (frames), y specular max history, z clamp width (sigmas),
                  // w luminance edge-stop (sigmas, a-trous)
    vec4  prm1;   // x depth edge-stop, y diffuse normal power, z W change that cuts the history
                  // (relative; 0 = off), w the shortest specular history (a mirror's, frames)
    uvec4 ctl;    // x flags (DN_F_*), y this a-trous iteration k, z iterations N, w frame index
    // SUN / MOON CHANNEL (DN_F_SUN; clean_pdn_common.glsl SUN CHANNEL):
    vec4  sun0;   // x longest history (frames), y moving-caster hard history, z moving-caster
                  // longest history (0 = no cap), w the light's step since last frame (disc radii)
    vec4  sun1;   // x a-trous penumbra radius cap (texels), y moving-caster box width (sigmas),
                  // z moving-caster countdown (frames), w the disc rays per texel (0: sun_rt.y)
} dp;
#define DN_F_HIST   1u    // the history is valid (the denoiser ran last frame at this size)
#define DN_F_PARITY 2u    // this frame's GEOM / HIST layer (the producer wrote GEOM layer parity)
                          // (bit 2: DN_F_YOUNG_BOOST, bit 31: DN_F_MOVE_ZTOL -- below, #1038)
#define DN_DBG_SHIFT 4u   // bits 4-7: the debug view (PDN_DBG_*; 8 / 9 the #869 split views)
#define DN_F_SUN       256u    // the producer wrote the sun / moon channel (IN layer 2)
#define DN_F_SUN_HIST  512u    // ... and last frame's sun history is valid (no sun <-> moon cut)
#define DN_F_SUN_HARD  1024u   // hard sun (no disc: one ray per pixel)
#define DN_F_SUN_EXACT 2048u   // exact history reprojection (RenderVKRTMaskExactReproj)
// NO-INFORMATION SAMPLES (the projector WALL-BLEED fix, RenderVKProjStochBleedFix; the producer's
// PDN_OPT_BLEED, clean_deferred_light.comp): a texel without a ray-traced projector candidate (W 0)
// or whose picks carried no light this frame (IN layer 0 a < 0 diffuse, layer 1 a < 0 specular)
// says nothing about visibility -- its ratio 1 is a default, not "lit". With this bit such a
// frame is a WEIGHT-0 SAMPLE of the ratio estimator, never a reason to drop the texel:
//   * temporal: the history holds at its length (no accumulation); with no usable history the
//     informative same-surface 3x3 mean seeds it at length 1 (a real estimate); only a texel
//     with neither stays UNKNOWN (length 0, its value the default);
//   * the 3x3 clamp box is built from informative raw samples only, widened by the bounded
//     [0, 1] ratio's Bernoulli spread when few are informative (no collapsed box resetting the
//     history every frame);
//   * history / a-trous / upsample taps are NOT rejected: a texel's value is the history or the
//     seeded estimate. Only UNKNOWN texels are down-weighted (DN_UNKNOWN_W), and an unknown
//     a-trous centre takes its neighbours' geometry-stopped mean (no luminance stop anchored on
//     the default). Leakage across walls is stopped by the geometric stops (depth plane +
//     normal) every tap already has, and by the producer's facing gate (PDN_OPT_FACING).
// Without this bit the 1s are real samples and spread through the filters to receivers behind walls.
#define DN_F_INFO 4096u
// (#869 / #85) ReSTIR-CONVERGED DIFFUSE: bits 13-15 = the a-trous iterations a converged texel's
// diffuse takes (GEOM meta bit 15, set by the temporal pass: a reused reservoir with M >= DN_RS_M
// and a diffuse history of >= DN_RS_M frames); 0 = every texel takes all of them.
#define DN_CONV_SHIFT 13u
#define DN_RS_M 8.0
uint dnConvIters() { return (dp.ctl.x >> DN_CONV_SHIFT) & 7u; }
// (#869) THE MOVING-LIGHT RULE (RenderVKReSTIRMoveCut; clean_regir.glsl RS_MOVE_*): bits 16-19 = the
// history (frames) of a texel whose light MOVED this frame -- the producer's MOT w = the moved
// lights' share of its target x 255; 0 = the rule is off (every other producer writes w = 0).
#define DN_MOVE_SHIFT 16u
// #870 RAW SIGNAL ROUTING (RenderVKDlssRawSignals, while DLSS Ray Reconstruction reconstructs):
// the consumer wants this frame's RAW samples -- no history, no a-trous filter, exactly the
// PDN_DBG_RAW pass-through but without its debug false colour or the consumer's debug view.
// DN_F_RAW: the diffuse / specular channels (projectors; GI) -- and (#869) the split lights' own
// visibility with them (one ray each this frame; a slot without information passes as 1, its light
// has no term there): with RenderVKProjStochasticSplitK 0 exactly the old raw routing;
// DN_F_RAW_SUN: the sun / moon channel.
// The last a-trous iteration still writes the view depth into alpha (the consumer's upsample).
#define DN_F_RAW     (1u << 20)
#define DN_F_RAW_SUN (1u << 21)
uint dnMoveHist() { return (dp.ctl.x >> DN_MOVE_SHIFT) & 15u; }
// (#869) MOVING SURFACES (RenderVKReSTIRSurfaceMoveVeto; bits 22-25 = the threshold in quarter
// denoiser texels, 0 = off): a texel whose surface moved on its own since last frame -- the motion
// vector's reprojection lands more than this far from the camera-only one (an animated avatar /
// animesh, a moving prim) -- never takes the ReSTIR-converged shortcut. Its reservoir's M is the
// light SELECTION's confidence (the pick is re-weighed and re-traced every frame), not a sign that
// the visibility converged: a static lamp's shadow slides over a breathing / walking body in
// surface space, which the moving-LIGHT rule (a light that moved) never sees.
#define DN_SURF_MOVE_SHIFT 22u
float dnSurfMoveTexels() { return float((dp.ctl.x >> DN_SURF_MOVE_SHIFT) & 15u) * 0.25; }
// The moving-light rule caps the DIFFUSE history only (the GI denoiser: its specular channel is the
// sky visibility, which no light change touches -- RenderVKGILightChangeHistory).
#define DN_F_MOVE_DIFF_ONLY (1u << 26)
bool dnMoveDiffOnly() { return (dp.ctl.x & DN_F_MOVE_DIFF_ONLY) != 0u; }
// (#87) DISOCCLUSION HISTORY FIX (the GI denoiser, RenderVKGIDisoccFix; bits 27-29 = N frames, 0 = off):
// a texel whose history is shorter than N frames (a disocclusion: camera zoom / pan, silhouettes of
// hair strands and limbs) holds a 1..N-sample estimate. SVGF / ReBLUR-style spatial boost, fading as
// the history grows: the temporal pass floors its variance at a few-sample estimate's (~mean^2 / n:
// the a-trous luminance stop no longer anchors on its own outlier), and the a-trous weights its taps
// by their history length relative to the centre's (up to DN_HFIX_MAXW: converged neighbours fill
// it) with the luminance stop widened. Texels at N frames or more: exactly as before.
#define DN_HFIX_SHIFT 27u
#define DN_HFIX_MAXW 8.0
float dnHistFix() { return float((dp.ctl.x >> DN_HFIX_SHIFT) & 7u); }
// youth in [0, 1]: 1 - len / N below N frames, 0 at N or more (and with the fix off)
float dnYouth(float len) { const float n = dnHistFix(); return n > 0.0 ? clamp(1.0 - len / n, 0.0, 1.0) : 0.0; }
// ... and the other half: a SETTLED centre's tap younger than N frames counts len / N (at least
// 1 / DN_HFIX_MAXW) -- the young outlier's floored variance widens its settled neighbours' stop too
// (the 3x3 variance prefilter), which must not pull its sparkle into them (a halo). 1 with the fix
// off, at N frames or more, and for an unknown tap (dnConf has it).
float dnMaturity(float len)
{
    const float n = dnHistFix();
    return (n > 0.0 && len > 0.5) ? max(min(len / n, 1.0), 1.0 / DN_HFIX_MAXW) : 1.0;
}
// (#87) REPROJECTION SEARCH (the GI denoiser, RenderVKGIReprojSearch): when none of the bilinear 2x2
// history taps is the same surface (sub-texel motion across a thin strand / a silhouette at the
// reduced extent: every tap lands beside it), the best-matching texel of the 3x3 around the
// reprojected point (the depth plane, the normal) gives the history instead of a restart.
#define DN_F_REPROJ_SEARCH (1u << 30)
bool dnReprojSearch() { return (dp.ctl.x & DN_F_REPROJ_SEARCH) != 0u; }
// (#1038) YOUNG-TEXEL BOOST (RenderVKDenoiseYoungBoost; acts only with the history fix, DN_HFIX_SHIFT
// > 0): a disocclusion that uncovers a WHOLE surface at once -- an avatar walking out from behind a
// wall, its body revealed in one frame -- has no settled neighbour for the history fix to fill from:
// every tap is as young as the centre, and the a-trous degenerated to plain SVGF on 1-sample texels,
// its normal stop (power 64) cutting nearly every tap on a curved, normal-mapped body. With this bit:
//   * temporal: a texel with NO usable history stores the same-surface 3x3 mean of this frame's raw
//     samples (the centre included; informative ones only under DN_F_INFO) as its first estimate
//     (length 1) instead of its own single sample -- the history starts ~9x less noisy and the
//     clamp box of the next frames is not anchored on an outlier;
//   * a-trous: a young centre's normal stop relaxes toward power 8 with its youth (indirect light is
//     smooth over a body's curvature at this resolution; the depth plane still stops silhouettes) and,
//     from the second iteration on, its step doubles (a wider footprint without another dispatch).
// Texels at or past the history-fix length: exactly as before (no steady-state cost or change).
#define DN_F_YOUNG_BOOST 4u
bool dnYoungBoost() { return (dp.ctl.x & DN_F_YOUNG_BOOST) != 0u && dnHistFix() > 0.0; }
// (#1038) MOVING-SURFACE DEPTH TOLERANCE (RenderVKDenoiseMovingDepthTol): the producer's expected
// previous depth (MOT z) is the CAMERA-ONLY one -- this frame's point seen from last frame's camera.
// A surface that moved on its own (a walking / running avatar, a moving prim) was elsewhere, at
// another depth: toward / away from the camera by about as much as it moved sideways. The history
// taps' depth test then rejected a fast mover's history every frame (a constant restart). With this
// bit the tolerance grows by the surface's own sideways motion in metres (the motion vectors'
// reprojection against the camera-only one, through last frame's projection), at most 25 % of the
// depth; the normal test still applies. Static surfaces (no own motion): unchanged.
#define DN_F_MOVE_ZTOL (1u << 31)
bool dnMoveZTol() { return (dp.ctl.x & DN_F_MOVE_ZTOL) != 0u; }
// The extra depth tolerance (m) for a surface whose motion-vector reprojection hc lands dTex denoiser
// texels from its camera-only one, at expected depth pw (0: no own motion / no projection).
float dnMoveDepthTol(vec2 dTex, float pw)
{
    const vec2 pk = vec2(abs(ssr.views[CLEAN_VIEW].prev_proj[0][0]), abs(ssr.views[CLEAN_VIEW].prev_proj[1][1]));
    if (!(pk.x > 0.05 && pk.y > 0.05) || !(pw > 0.0)) return 0.0;
    const float lat = length(dTex / dp.dims.xy * 2.0 / pk) * pw;
    return (lat >= 0.0) ? min(lat, 0.25 * pw) : 0.0;
}
bool dnInfoOn() { return (dp.ctl.x & DN_F_INFO) != 0u; }
// Informative diffuse / specular at texel q of THIS frame (W: its GEOM w's reference, raw: IN).
bool dnInfoD(float W, vec4 raw) { return W > 0.0 && raw.a > 0.0; }
bool dnInfoS(float W, vec4 raw) { return W > 0.0 && raw.a >= 0.0; }
// Tap weight of an UNKNOWN texel (DN_F_INFO, history length 0: never sampled, no estimate) --
// soft, never a hard zero (a band of them must not starve the filters).
#define DN_UNKNOWN_W 0.05
float dnConf(float len) { return (!dnInfoOn() || len > 0.5) ? 1.0 : DN_UNKNOWN_W; }
// The sun / moon disc rays per texel this frame (sun1.w; 0 = the frame info's RenderVKRTSunSamples):
// the one-ray option (RenderVKShadowSunOneRay) traces 1.
float dnSunRays() { return dp.sun1.w > 0.5 ? dp.sun1.w : max(ssr.sun_rt.y, 1.0); }
uint dnParity()  { return (dp.ctl.x & DN_F_PARITY) != 0u ? 1u : 0u; }
uint dnDebug()   { return (dp.ctl.x >> DN_DBG_SHIFT) & 15u; }
// a pass-through view (RAW, or the ReSTIR view's false colour): no history, no filter
bool dnRawView() { const uint d = dnDebug(); return d == PDN_DBG_RAW || d == PDN_DBG_RESTIR || (dp.ctl.x & DN_F_RAW) != 0u; }
// the sun / moon channel's pass-through (its debug RAW view, or #870 raw routing)
bool dnSunRaw()  { return dnDebug() == PDN_DBG_RAW || (dp.ctl.x & DN_F_RAW_SUN) != 0u; }
bool dnHistOk()  { return (dp.ctl.x & DN_F_HIST) != 0u && ssr.taa_params.z < 0.5; }
bool dnSunOn()   { return (dp.ctl.x & DN_F_SUN) != 0u; }
bool dnSunHistOk() { return dnHistOk() && (dp.ctl.x & DN_F_SUN_HIST) != 0u; }
float dnLum3(vec3 v) { return dot(v, vec3(1.0 / 3.0)); }   // (the sun channel: the RT mask's luminance)

// (#869) THE SPLIT CHANNEL (clean_pdn_common.glsl SPLIT CHANNEL): the images carry it (GEOM's
// layer count -- no push bit; the producer writes empty slots when it does not split), and its
// IN / OUT layer (the last one: after the sun channel's, which is allocated with it).
bool dnSplitOn()     { return imageSize(uDnGeom).z >= PDN_SPLIT_GEOM_LAYERS; }
int  dnSplitLayer()  { return imageSize(uDnIn).z - 1; }
// The render pixel this denoiser texel samples (the RT mask's convention, clean_rt_mask_common
// .glsl rtMaskSamplePx: below the render extent the sample point follows the TAA jitter).
vec2 dnJitter() { return (ssr.taa_params.w > 0.5 && dp.dims.x < dp.dims.z) ? ssr.taa_jitter.xy : vec2(0.0); }
ivec2 dnRenderPx(ivec2 m)
{
    const vec2 r = dp.dims.zw / dp.dims.xy;
    return clamp(ivec2((vec2(m) + 0.5 + dnJitter()) * r), ivec2(0), ivec2(dp.dims.zw) - 1);
}

// The guide of texel q in layer l: view depth (<= 0: none), normal, roughness, depth gradient.
struct DnGeom { float z; vec3 n; float rough; vec2 grad; uint meta; };
DnGeom dnGeom(ivec2 q, uint l)
{
    const uvec4 g = imageLoad(uDnGeom, ivec3(q, int(l)));
    DnGeom r;
    r.z = uintBitsToFloat(g.x);
    r.n = pdnUnpackN(g.y);
    r.rough = pdnUnpackR(g.y);
    r.grad = unpackHalf2x16(g.z);
    r.meta = g.w;
    if (!(r.z > 0.0) || isinf(r.z)) r.z = -1.0;
    return r;
}
