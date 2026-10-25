// vkclean — shared by the three ray-traced shadow mask passes (#195 + shadow denoiser):
//   clean_rt_shadow_mask.comp   (A) trace: raw per-frame estimate + receiver depth plane
//   clean_rt_mask_temporal.comp (B) temporal accumulation (clamped, adaptive length)
//   clean_rt_mask_spatial.comp  (C) edge-aware penumbra blur -> the mask the lit FS reads
// One pipeline layout (sets 0, 1, RT consumer, mask set 3) and one push block for all.
// Needs clean_frame_info.glsl (ssr) before inclusion.
//
// Mask set 3 (all GENERAL; the history images are last frame's B output + ids/depth):
//   0 pre-pass depth (sampled)        1 final sun rgba8 (C)        2 final proj rgba8[4] (C)
//   3 ids rgba16ui (A)                4 view depth r32f (A), 21 its gradient rgba16f (A): d(vz)/dx, d(vz)/dy, flags
//     (the old rgba32f (vz, dx, dy, flags) split: depth keeps 32 bits, the rest fp16; 8 / 22 the history's)
//     (1: a = this frame's shadow SHADING CHANGE for the TAA / FSR reactive mask, written by
//     B, passed through by C. 4: w = the MOVING-CASTER bits, an exact integer: bit 0 the
//     sun's rays met a moving caster, bit k + 1 projector slot k's -- clean_rt_shadow_mask
//     .comp rtNoteBlocker; the history 8 carries them as well.)
//   5 hist acc sun   6 hist acc proj[4]   7 hist ids   8 hist depth   9 hist meta (sampled)
//   10 raw sun rgba8 (A)   11 raw proj rgba8[4] (A): rgb visibility, a = penumbra radius
//      (11 is the same image as 2: raw is dead once B has read it, C then writes final)
//   12 acc sun rgba16f (B) 13 acc proj rgba16f[4] (B): rgb visibility, a = radius (-1 none)
//   14 meta rgba16f (B): x sun history length (frames), y the shortest projector history
//      length at the texel (frames; the surface age when it has no projector), zw the
//      sun raw luminance's temporal moments (mean, mean of squares)
//   15 the pre-pass motion vectors (sampled, render resolution; read when
//      ssr.taa_params.y says this frame wrote them)
//   F3a: the renderer keeps two sets; set p writes B into acc slot p (12-14) and A's ids /
//      depth into slot p (3 / 4), and samples slot 1 - p (last mask frame's) at 5-9 -- no
//      per-frame history copies. The lit FS reads slot p's ids / depth (set 0 bindings
//      33 / 34 [p], clean_shadow_mask_read.glsl).
//   16 / 17 the a-trous scratch (the read slot's sun accumulation / meta, #85)
//   18 the projector a-trous scratch: the read slot's projector accumulation (rgba16f[4],
//      dead after B, #666)
//   19 / 20 PROJECTOR SLOT HISTORY (#666, rgba16ui; 19 this slot's -- B writes, C reads --,
//      20 the read slot's -- B reads): component k = projector slot k's own history length
//      and fast (~3-frame) luminance mean, rtPackPM. The sun keeps the same in 14.

layout(push_constant) uniform MaskPush {
    mat4 inv_view_proj;   // UNJITTERED VK clip -> CAMERA-RELATIVE world (P * rotation-only view)^-1
    vec4 cam_time;        // camera world pos, time (s)
    vec4 dims;            // render w, h, mask w, h
    vec4 p0;              // x frame index, y bits (an exact integer, #666): bit 0 the denoiser
                          // (else the legacy 0.75 blend), bit 1 the history is valid (last
                          // frame's camera is real: temporal plumbing on, no reset -- with or
                          // without TAA), bit 2 projector slots on the full sun path
                          // (RenderVKRTShadowProjDenoise), z flags
                          // (an exact integer: bit 0 history hint on, bit 1 the SVGF-style
                          // sun history / variance (#85, RenderVKRTShadowSvgf),
                          // bit 2 moving-caster anti-lag, bit 3 write
                          // the shading-change reactive, bits 4-7 the moving-caster hard-shadow
                          // history (frames), bits 8-15 the reactive gain x 16, bits 16-22
                          // the sun's motion since the last mask frame in disc radii x 32,
                          // bit 23 the temporal-events debug view (RenderVKRTShadowDebug 6)),
                          // w max history length (frames)
    vec4 p1;              // xy LAST frame's TAA jitter (render px), z spatial radius cap (mask
                          // texels, 0 = no spatial pass), w history clamp width (sigmas; NEGATIVE:
                          // the ray-traced projectors are sampled by the stochastic sampler instead,
                          // #85 phase 2 -- the trace leaves their slots empty)
} mp;
// (#85 phase 2, RenderVKProjStochastic) the deferred lighting's stochastic projector sampler
// (clean_deferred_light.comp pass 15) shadows the ray-traced projectors this frame: no slots.
bool rtProjStochastic() { return mp.p1.w < 0.0; }

// #841: round(), not +0.5. Bits 22-23 (slope bias, default 2) put p0.y in [2^23, 2^24),
// where float spacing is 1: x + 0.5 rounded an odd x to even, clearing bit 0 (the
// denoiser) and carrying into bit 1 -- every default-settings frame ran the legacy path
// with no history and no noise phase (a frozen stipple). round() of an integer is exact.
uint  rtModeBits()  { return uint(round(max(mp.p0.y, 0.0))); }
uint  rtFlagBits()  { return uint(round(max(mp.p0.z, 0.0))); }
bool  rtDenoise()   { return (rtModeBits() & 1u) != 0u; }
bool  rtProjFull()  { return (rtModeBits() & 5u) == 5u; }   // #666 (denoiser on too)
bool  rtHintOn()    { return (rtFlagBits() & 1u) != 0u; }
bool  rtAntiLag()   { return (rtFlagBits() & 4u) != 0u; }   // RenderVKRTShadowAntiLag
bool  rtReactOn()   { return (rtFlagBits() & 8u) != 0u; }   // RenderVKRTShadowReactive
float rtDynHardLen(){ return max(float((rtFlagBits() >> 4u) & 15u), 1.0); }        // RenderVKRTShadowDynamicHistory
float rtReactGain() { return float((rtFlagBits() >> 8u) & 255u) * (1.0 / 16.0); }  // RenderVKRTShadowReactiveGain
bool  rtSvgf()      { return (rtFlagBits() & 2u) != 0u; }   // RenderVKRTShadowSvgf (#85)
// Sun / moon motion since the last mask frame, in disc radii (angle / tan(disc radius);
// a hard sun counts against a 0.05 degree disc). A shadow edge then moves by about its
// penumbra radius x this per frame (clean_rt_mask_temporal.comp sunMoveCap).
float rtSunMove()   { return float((rtFlagBits() >> 16u) & 127u) * (1.0 / 32.0); }
bool  rtDiagView()  { return (rtFlagBits() & 0x800000u) != 0u; }   // RenderVKRTShadowDebug 6
// p0.y bit 21: RenderVKRTShadowDebug 11, the trace's self-hit detector (clean_rt_shadow_mask.comp).
bool  rtSelfHitDiag() { return (rtModeBits() & (1u << 21u)) != 0u; }
// p0.y bits 22-23: RenderVKRTShadowSlopeBias -- the mask receiver's view-slope origin offset
// in render-pixel footprints (0 off, 1 = 1/8, 2 = 1/4, 3 = 1/2; clean_rt_shadow_mask.comp).
float rtSlopeBiasPx() { const uint k = (rtModeBits() >> 22u) & 3u; return k == 0u ? 0.0 : 0.0625 * float(1u << k); }
vec2  rtMaskScale() { return mp.dims.zw / mp.dims.xy; }   // mask px per render px (per axis)

// Mask-texel offset of this frame's sample (texels): below full resolution the sample
// point moves around the texel's footprint with the TAA jitter. Shared with
// clean_shadow_mask_read.glsl (which inverts it).
vec2 rtMaskJitter()
{
    return (ssr.taa_params.w > 0.5 && mp.dims.z < mp.dims.x) ? ssr.taa_jitter.xy : vec2(0.0);
}

// History accumulation runs whenever the previous view-projection is meaningful: the
// temporal plumbing is on and there was no history reset (cuts, teleports). #666: that
// used to be "TAA jitter on" -- with FXAA / SMAA / no AA the mask had no accumulation and
// a frozen noise phase; the host now keeps the plumbing for the mask and says so here.
bool rtMaskTemporal() { return (rtModeBits() & 2u) != 0u; }
// The view is jittered and a TAA / FSR resolve integrates per-frame variation (the disc
// blur's per-frame rotation is only for that: without a resolve it would just crawl).
bool rtMaskResolved() { return ssr.taa_params.w > 0.5 && rtMaskTemporal(); }

// #674 EXACT HISTORY REPROJECTION (RenderVKRTMaskExactReproj -> p0.y bit 3; only while
// the view is NOT jittered and the mask is below render resolution, so the TAA / FSR and
// full-resolution paths are untouched). A mask texel t below
// full resolution traces the render pixel CENTRE floor((t + 0.5 + J) / s) + 0.5
// (rtMaskRenderPx truncates), not (t + 0.5 + J) / s. The temporal pass's inversion used
// the latter: at scale 0.5 without jitter the reprojection of a STATIC receiver lands a
// quarter texel off its own texel (hc = m + 0.25) every frame. With TAA / FSR the jitter
// makes that error zero-mean; without it the history is re-sampled a quarter texel
// sideways each frame -- the shadow drifts (~0.25 x the history length), the clamp box
// catches it, cuts the length, it springs back, drifts again: a smooth back-and-forth.
// This inverts the real sample centres (piecewise linear between them) instead: a static
// receiver maps to exactly its own texel.
bool rtExactReproj() { return (rtModeBits() & 8u) != 0u && ssr.taa_params.w < 0.5 && mp.dims.z < mp.dims.x; }

// #674 MOVING-CASTER PERSISTENCE (RenderVKRTShadowMoverPersist -> p0.y bits 4-6, frames;
// 0 = off, the old one-frame flag): see clean_rt_mask_temporal.comp. The temporal pass
// keeps a per-texel countdown in the depth plane's w (bits 5-7 the sun, 8-10 any
// projector; the trace's own bits 0-4 untouched) that rides the history like the flags.
uint rtMoverPersist() { return (rtModeBits() >> 4u) & 7u; }
// #674 ZOOM-AWARE HISTORY (RenderVKRTShadowZoomAware -> p0.y bit 7).
bool rtZoomAware() { return (rtModeBits() & 128u) != 0u; }
// #674 LEADING EDGE (RenderVKRTShadowMoverDarkSnap -> p0.y bit 8): a texel whose rays met
// a moving caster THIS frame and whose raw estimate is darker than its (lit) history by
// more than the ray noise -- the centre beyond one sample's spread for the ray count, the
// current 3x3 mean beyond 0.15 -- is newly covered by the mover: that darkening is a
// geometric fact, taken at once (the change detector's verdict forced to 1: history
// length 1, no fast-history widening, moments re-seeded from the current 3x3, reactive).
// A noisy still penumbra on an idle avatar does not pass both tests.
bool rtDarkSnapOn() { return (rtModeBits() & 256u) != 0u; }
bool rtDarkSnap(float hist_l, float raw_l, float mean_l)
{
    const float tc = min(0.9, inversesqrt(max(ssr.sun_rt.y, 1.0)));
    return hist_l - raw_l > tc && hist_l - mean_l > 0.15;
}
// #674 (RenderVKRTShadowMoverSpatial -> p0.y bit 9): YOUNG texels on the moving-caster
// path get the a-trous step-1 pass even under a sub-texel penumbra radius -- their 1-2
// frame history is the raw disc pattern (the dotted half-tone in a fresh shadow), and
// the luminance weight on the young 5x5 variance keeps a real edge.
bool rtMoverSpatial() { return (rtModeBits() & 512u) != 0u; }
// #674 (RenderVKRTShadowMoverYoungFix -> p0.y bit 10): see clean_rt_mask_spatial.comp.
bool rtMoverYoungFix() { return (rtModeBits() & 1024u) != 0u; }
// #674 debug view 9 (RenderVKRTShadowDebug 9 -> p0.y bit 11): the temporal pass writes,
// per texel, red = the history length it USED (after every cap; white = RenderVKRTShadow
// History), green = the moving-caster path was taken, blue = the change detector's verdict.
bool rtDiagUsed() { return (rtModeBits() & 2048u) != 0u; }
// #674 (RenderVKRTShadowDynamicMaxHistory -> p0.y bits 12-15, frames; 0 = off): the
// longest history ANY texel on the moving-caster path keeps, wide penumbrae included.
float rtDynMaxLen() { return float((rtModeBits() >> 12u) & 15u); }
// #674 CERTAINTY BYPASS (RenderVKRTShadowCertainBypass -> p0.y bit 16): on the moving-
// caster path, a texel whose rays this frame were ALL blocked (or all clear) -- raw
// luminance at 0 / 1 -- with its same-surface 3x3 raw mean on the same side of 0.5 (an
// umbra texel beside the edge still qualifies) takes the raw
// value, no history. A moving shadow's umbra / lit side is then exactly this frame's;
// history only blends partial (penumbra) texels. Static casters are never affected.
bool rtCertainBypass() { return (rtModeBits() & 65536u) != 0u; }
bool rtCertain(float raw_l, float mean_l)
{
    return (raw_l <= 0.004 && mean_l < 0.5) || (raw_l >= 0.996 && mean_l > 0.5);
}
// #674 debug view 10 (RenderVKRTShadowDebug 10 -> p0.y bit 17): red = how far the
// temporal output was pulled from this frame's raw (luminance; full red = 0.25 or more),
// green = moving-caster path, blue = the certainty bypass took the raw.
bool rtDiagPull() { return (rtModeBits() & 131072u) != 0u; }
// The depth plane's w (trace bits 0-4 + the temporal pass's countdowns, bits 5-10):
// the sun's / projector slot s's moving-caster state at this texel.
bool rtSunMover(float w)       { const uint b = uint(clamp(w, 0.0, 2047.0) + 0.5); return (b & 1u) != 0u || ((b >> 5u) & 7u) != 0u; }
bool rtProjMover(float w, int s) { const uint b = uint(clamp(w, 0.0, 2047.0) + 0.5); return (b & (2u << uint(s))) != 0u || ((b >> 8u) & 7u) != 0u; }
// History confidence kept when the receiver's pixel footprint shrank since last frame
// (zoom in / dolly in): last frame's history was accumulated on texels q x wider, so it
// counts for 1 / q^2 of its length. Minification (q < 1) keeps it. pw / vz: the
// receiver's view depth last / this frame.
float rtZoomKeep(float vz, float pw)
{
    const float p11c = length(vec3(ssr.taa_cur_vp[0][1], ssr.taa_cur_vp[1][1], ssr.taa_cur_vp[2][1]));
    const float p11p = length(vec3(ssr.taa_prev_vp[0][1], ssr.taa_prev_vp[1][1], ssr.taa_prev_vp[2][1]));
    if (!(p11c > 1e-6) || !(p11p > 1e-6) || !(vz > 1e-6) || !(pw > 1e-6)) return 1.0;
    const float q = (pw / p11p) / (vz / p11c);
    return q > 1.02 ? clamp(1.0 / (q * q), 0.0, 1.0) : 1.0;
}
// Last frame's texel t's sample centre, unjittered render px (per axis).
vec2 rtPrevCentre(vec2 t, vec2 jp, vec2 Jp, vec2 s) { return floor((t + 0.5 + Jp) / s) + 0.5 - jp; }
// Continuous history texel coordinate of unjittered previous render px q (hc0: the linear
// estimate, the result's starting bracket).
vec2 rtExactHc(vec2 q, vec2 jp, vec2 Jp, vec2 s, vec2 hc0)
{
    vec2 hc;
    for (int a = 0; a < 2; ++a)
    {
        float b = floor(hc0[a]);
        float c0 = floor((b + 0.5 + Jp[a]) / s[a]) + 0.5 - jp[a];
        float c1 = floor((b + 1.5 + Jp[a]) / s[a]) + 0.5 - jp[a];
        if (q[a] < c0)       { b -= 1.0; c1 = c0; c0 = floor((b + 0.5 + Jp[a]) / s[a]) + 0.5 - jp[a]; }
        else if (q[a] >= c1) { b += 1.0; c0 = c1; c1 = floor((b + 1.5 + Jp[a]) / s[a]) + 0.5 - jp[a]; }
        hc[a] = b + clamp((q[a] - c0) / max(c1 - c0, 1e-3), 0.0, 1.0);
    }
    return hc;
}

// Render pixel (jittered raster) a mask texel samples this frame.
vec2 rtMaskSamplePx(ivec2 m) { return (vec2(m) + 0.5 + rtMaskJitter()) / rtMaskScale(); }
ivec2 rtMaskRenderPx(ivec2 m) { return clamp(ivec2(rtMaskSamplePx(m)), ivec2(0), ivec2(mp.dims.xy) - 1); }

vec3 worldAt(vec2 rpx, float d)
{
    vec2 ndc = rpx / mp.dims.xy * 2.0 - 1.0;
    if (ssr.taa_params.w > 0.5) ndc -= ssr.taa_jitter.zw;   // jitter is in NDC (frame info)
    vec4 w = mp.inv_view_proj * vec4(ndc, d, 1.0);
    return mp.cam_time.xyz + w.xyz / w.w;   // camera-relative: precise at any altitude
}

// Penumbra radius (mask texels) in the 8-bit raw alpha: 0 = no caster met, else
// (r + 0.25) / 64 -> 0.25-texel steps up to ~64 texels.
// Visibility in [0, 1] (NaN / Inf -> lit): history reads and accumulated outputs.
vec3 rtFiniteVis(vec3 v) { return (any(isnan(v)) || any(isinf(v))) ? vec3(1.0) : clamp(v, vec3(0.0), vec3(1.0)); }
float rtEncR(float r) { return r < 0.0 ? 0.0 : clamp((r + 0.25) / 64.0, 1.0 / 255.0, 1.0); }
float rtDecR(float a) { return a < 0.5 / 255.0 ? -1.0 : max(a * 64.0 - 0.25, 0.0); }

// MOTION-AWARE REPROJECTION: the receiver's position LAST frame, in last frame's
// unjittered render px. The pre-pass's motion vectors (clean_prepass_normal.frag: UV
// units, current minus previous, unjittered) carry OBJECT motion -- skinned avatars
// (idle animation moves an arm pixels per frame at 4K), animated / moving prims;
// camera-only reprojection fetched their history from wherever the surface was NOT,
// and the neighbourhood clamp then kept resetting it: the penumbra noise never
// integrated on exactly the receivers that move. Without motion vectors this frame
// (TAA off, no pre-pass): camera-only. pw = the receiver's view depth through last
// frame's camera (the motion's depth component isn't stored; a few mm for the motions
// that matter, well inside the history depth tolerance). false: behind last frame's eye.
layout(set = 3, binding = 15) uniform sampler2D uMotion;
bool rtMaskPrevPx(vec3 P, ivec2 rp, out vec2 prpx, out float pw)
{
    const vec4 c = ssr.taa_prev_vp * vec4(P - ssr.cam_origin.xyz, 1.0);   // camera-relative (#555)
    pw = c.w;
    prpx = vec2(0.0);
    if (c.w <= 1e-4) return false;
    prpx = (c.xy / c.w * 0.5 + 0.5) * mp.dims.xy;
    if (ssr.taa_params.y > 0.5 && rtDenoise())
    {
        const vec4 k = ssr.taa_cur_vp * vec4(P - ssr.cam_origin.xyz, 1.0);
        if (k.w > 1e-4)
        {
            const vec2 mv = texelFetch(uMotion, clamp(rp, ivec2(0), ivec2(mp.dims.xy) - 1), 0).xy;
            prpx = ((k.xy / k.w * 0.5 + 0.5) - mv) * mp.dims.xy;
        }
    }
    return true;
}

// PROJECTOR SLOT HISTORY (#666, bindings 19 / 20): history length (frames, 1..64) in
// bits 0-5, the fast luminance mean in bits 6-15 (unorm 10).
uint  rtPackPM(float len, float fm)
{
    const uint l = uint(clamp(len, 1.0, 64.0) + 0.5) - 1u;
    const uint f = uint(clamp(isnan(fm) ? 1.0 : fm, 0.0, 1.0) * 1023.0 + 0.5);
    return l | (f << 6u);
}
float rtPMLen(uint v)  { return float(v & 63u) + 1.0; }
float rtPMFast(uint v) { return float(v >> 6u) * (1.0 / 1023.0); }

float rtMaskIGN(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
