// vkclean — lit-FS side of the ray-traced shadow mask (#195). The compute pass
// clean_rt_shadow_mask.comp traces the sun / moon and the ray-traced projectors for
// every mask texel of the depth pre-pass; pass 1 of the main view (the plain lit FS
// variant) reads the results here instead of tracing. Transparent receivers, particles,
// water and the probe / mirror captures keep tracing inline (they aren't in the
// pre-pass depth).
//
// DEPTH-AWARE UPSAMPLE: the mask may be below render resolution (rt_mask.y < 1). A
// fragment blends its 4 nearest mask texels bilinearly, each weighted by how close the
// texel's receiver depth is to its own, so a shadow doesn't bleed across a silhouette;
// if none match it takes the closest-depth texel. With the shadow denoiser
// (rt_mask.x == 2) each texel carries its receiver's depth PLANE (view depth + its
// per-render-pixel gradient): the fragment compares against the depth that plane
// predicts at the fragment, so sloped receivers -- ground at a grazing view, where the
// depth changes by more than the old flat 3 % per texel -- keep all four taps instead
// of collapsing to the closest one (stair-stepped, swimming shadow edges).
//
// #1086 THIN-RECEIVER FALLBACK (RenderVKRTShadowMaskThinFallback, frame info ao_up.w): below
// full resolution each mask texel traces ONE render pixel of its footprint (moving with the
// TAA jitter), so a sub-sample receiver -- a hair strand on the unsampled column, the
// one-pixel rim of a horn or an ear -- often has none of its 4 taps on its own surface. The
// closest-depth tap the upsample then takes whole is the surface right BEHIND the thin
// receiver (scalp, the head behind the ear), i.e. exactly where that receiver casts its own
// contact shadow: a black pixel, a different set every frame under jitter (DLSS RR / FSR
// lock them in as dark ~2x2 blocks), changing with the avatar's motion without it. Scale 1
// never takes this path (the texel is the pixel). Where no tap is within half the tolerance
// and some tap lies behind the receiver, the fallback is the BRIGHTEST of those behind taps
// (per channel): a thin receiver in front cannot be in the shadow it casts on what is behind
// it, so the most lit surface behind it bounds it better than the closest one. Blended in
// from half the tolerance (e = 0.5) to none (e = 1), so a fragment with a matching tap keeps
// the old weights exactly; no tap close behind (the receiver seen through a gap, or only the
// backdrop behind a silhouette): the old rule.
#include "clean_upsample_common.glsl"   // upsampleWeights4 (shared with the half-res AO)
// #1075 LAYERED STEREO: a layer per view (2D_ARRAY at every view count; the reader's view is
// CLEAN_VIEW -- gl_ViewIndex on a multiview lit pipeline, the dispatch's z in a layered lighting
// kernel, 0 elsewhere); the projector image has 4 slot layers per view (rtMaskProjLayer).
layout(set = 0, binding = 31) uniform sampler2DArray uRtMaskSun;    // rgb = sun visibility x tint
layout(set = 0, binding = 32) uniform sampler2DArray uRtMaskProj;   // 4 slots per view: rgb per projector
int rtMaskProjLayer(int s) { return int(CLEAN_VIEW) * 4 + s; }
// 33 / 34: one per history slot (the mask's ping-pong); rt_mask.x's fraction picks this frame's.
layout(set = 0, binding = 33) uniform usampler2DArray uRtMaskIds[2];   // slot light index + 1 (0 = empty)
// 34: [0] / [1] receiver view depth (r32f; < 0 sky) per history slot, [2] / [3] its d/dx, d/dy
// (render px; rgba16f, xy) per history slot.
layout(set = 0, binding = 34) uniform sampler2DArray uRtMaskDepth[4];
// (Constant indices: a dynamic one needs shaderSampledImageArrayDynamicIndexing.)
bool  rtMaskSlot1() { return fract(ssr.rt_mask.x) > 0.125; }
uvec4 rtMaskIdsAt(ivec2 p) { return rtMaskSlot1() ? texelFetch(uRtMaskIds[1], ivec3(p, int(CLEAN_VIEW)), 0) : texelFetch(uRtMaskIds[0], ivec3(p, int(CLEAN_VIEW)), 0); }
vec4  rtMaskDepthAt(ivec2 p)
{
    return rtMaskSlot1() ? vec4(texelFetch(uRtMaskDepth[1], ivec3(p, int(CLEAN_VIEW)), 0).x, texelFetch(uRtMaskDepth[3], ivec3(p, int(CLEAN_VIEW)), 0).xy, 0.0)
                         : vec4(texelFetch(uRtMaskDepth[0], ivec3(p, int(CLEAN_VIEW)), 0).x, texelFetch(uRtMaskDepth[2], ivec3(p, int(CLEAN_VIEW)), 0).xy, 0.0);
}

// (Every px / w index is a constant: the k loops are unrolled, so the taps stay in
// registers across the lit FS's light loop instead of in local memory.)
// conf: 1 = the weights alone; below 1 the result blends toward the brightest of the taps
// in fb (bit k = tap k lies behind the receiver) -- the #1086 thin-receiver fallback (header).
struct RtMaskTaps { ivec2 px[4]; float w[4]; float conf; uint fb; };
// Frame info ao_up.w (an exact integer): bit 0 RenderVKRTShadowMaskThinFallback (0 = the
// closest-depth tap, as before), bits 1+ the plane upsample's absolute depth tolerance floor
// in mm (RenderVKRTShadowMaskUpsampleTolMM, default 30 = the old 3 cm; 0 = 3 cm).
uint  rtMaskUpBits()       { return uint(max(ssr.ao_up.w, 0.0) + 0.5); }
bool  rtMaskThinFallback() { return (rtMaskUpBits() & 1u) != 0u; }
float rtMaskPlaneTolFloor() { const uint mm = rtMaskUpBits() >> 1u; return mm == 0u ? 0.03 : float(mm) * 0.001; }
// How far behind the receiver a fallback tap may lie, in depth tolerances (header; 8 x the
// 3 cm close-range floor = 24 cm: hair over the scalp, an ear or a horn over the head).
#define RT_MASK_THIN_BEHIND_E 8.0

RtMaskTaps rtMaskTaps(vec3 P)
{
    RtMaskTaps t;
    t.conf = 1.0;
    t.fb = 0u;
    const ivec2 mx = ivec2(ssr.rt_mask.zw) - 1;
    // Full resolution: the texel itself. (LDBG_MASK_NEAREST, cost A/B: the nearest
    // texel without the depth-aware 4-tap upsample.)
    if (ssr.rt_mask.y >= 0.999 || litDbg(LDBG_MASK_NEAREST))
    {
        t.px[0] = clamp(ivec2(CLEAN_FRAG_XY * min(ssr.rt_mask.y, 1.0)), ivec2(0), mx); t.w[0] = 1.0;
        [[unroll]] for (int k = 1; k < 4; ++k) { t.px[k] = t.px[0]; t.w[k] = 0.0; }
        return t;
    }
    // The compute sampled texel m at render px (m + 0.5 + j) / scale (j = the TAA
    // jitter in mask texels, clean_rt_shadow_mask.comp): invert that for the taps.
    const vec2 j = ssr.taa_params.w > 0.5 ? ssr.taa_jitter.xy : vec2(0.0);
    const vec2 f = CLEAN_FRAG_XY * ssr.rt_mask.y - 0.5 - j;
    const ivec2 b = ivec2(floor(f));
    const vec2 fr = f - vec2(b);
    const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    const bool plane = ssr.rt_mask.x > 1.5;
    // Depth tolerance (m): 3 % of the distance flat, 1.5 % against the predicted plane (at
    // least RenderVKRTShadowMaskUpsampleTolMM, default 3 cm: #1086 A/B -- close up, hair
    // cards / scalp within it count as one surface and blend each other's shadow).
    const float tol = plane ? max(vz * 0.015, rtMaskPlaneTolFloor()) : max(vz * 0.03, 0.05);
    vec4 bw, e;
    uint behind = 0u;   // #1086: bit k = tap k's surface lies behind the receiver (not sky)
    [[unroll]] for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        t.px[k] = clamp(b + o, ivec2(0), mx);
        bw[k] = (o.x == 1 ? fr.x : 1.0 - fr.x) * (o.y == 1 ? fr.y : 1.0 - fr.y);
        const vec3 dp = rtMaskDepthAt(t.px[k]).xyz;
        float d = dp.x;
        // The texel's sample was the render pixel at (m + 0.5 + j) / scale: extrapolate
        // its plane from there to this fragment (bounded: a broken gradient can't pass).
        if (plane && d > 0.0)
        {
            const vec2 spx = floor((vec2(t.px[k]) + 0.5 + j) / ssr.rt_mask.y) + 0.5;
            d += clamp(dot(dp.yz, CLEAN_FRAG_XY - spx), -0.25 * d, 0.25 * d);
        }
        const float dz = dp.x < 0.0 ? 1e30 : abs(d - vz);
        e[k] = dz / tol;
        // (within RT_MASK_THIN_BEHIND_E tolerances: the surface the receiver lies on / in
        // front of -- scalp, head, a wall it touches --, never the backdrop metres behind a
        // silhouette, which may be sunlit while the receiver stands in shade)
        if (dp.x > 0.0 && d > vz && e[k] < RT_MASK_THIN_BEHIND_E) behind |= 1u << uint(k);
    }
    // Bilinear x depth similarity; every tap on another surface -> the closest one
    // (clean_upsample_common.glsl, shared with the half-res AO upsample).
    const vec4 w = upsampleWeights4(bw, e, vec4(1.0));
    [[unroll]] for (int k = 0; k < 4; ++k) t.w[k] = w[k];
    // #1086 thin receiver (header): no tap within half the tolerance, some tap behind it.
    if (rtMaskThinFallback() && behind != 0u)
    {
        const float emin = min(min(e.x, e.y), min(e.z, e.w));
        t.conf = 1.0 - smoothstep(0.5, 1.0, emin);
        if (t.conf < 1.0) t.fb = behind;
    }
    return t;
}

vec3 rtMaskSun(vec3 P)
{
    const RtMaskTaps t = rtMaskTaps(P);
    vec3 v = vec3(0.0), vb = vec3(0.0);
    [[unroll]] for (int k = 0; k < 4; ++k)
    {
        const bool inb = (t.fb & (1u << uint(k))) != 0u;
        if (t.w[k] > 0.0 || inb)
        {
            const vec3 s = texelFetch(uRtMaskSun, ivec3(t.px[k], int(CLEAN_VIEW)), 0).rgb;
            v += t.w[k] * s;
            if (inb) vb = max(vb, s);
        }
    }
    // #1086: the brightest tap behind a thin receiver (fb is 0 unless conf < 1)
    return t.fb != 0u ? mix(vb, v, t.conf) : v;
}

// Per light: only taps that traced it count -- in its own slot, else the tap's REST slot
// (clean_lights_common.glsl PROJ_MASK_REST_ID: the 5th+ projectors at the texel, traced and
// folded into one light-weighted ratio). A projector no nearby texel holds in either (gated
// off there: out of range / frustum, a black gobo texel) is unshadowed -- it lights nothing
// there that its visibility could change.
// (t: rtMaskTaps of this fragment -- the lit FS builds it once per light loop.)
int rtMaskSlotOf(uvec4 ids, uint lid)
{
    const int s = ids.x == lid ? 0 : (ids.y == lid ? 1 : (ids.z == lid ? 2 : (ids.w == lid ? 3 : -1)));
    if (s >= 0) return s;
    const uint r = PROJ_MASK_REST_ID;
    return ids.x == r ? 0 : (ids.y == r ? 1 : (ids.z == r ? 2 : (ids.w == r ? 3 : -1)));
}
vec3 rtMaskProjTaps(uint light, RtMaskTaps t)
{
    vec3 v = vec3(0.0), vb = vec3(0.0);
    float ws = 0.0;
    bool hb = false;   // #1086: a tap behind the thin receiver holds this light
    [[unroll]] for (int k = 0; k < 4; ++k)
    {
        const bool inb = (t.fb & (1u << uint(k))) != 0u;
        if (!(t.w[k] <= 0.0) || inb)   // (the old skip test, exactly, + the #1086 fallback taps)
        {
            const uvec4 ids = rtMaskIdsAt(t.px[k]);
            // the slot holding this light, else the REST slot (no inner loop: keeps k unrollable)
            const int s = rtMaskSlotOf(ids, light + 1u);
            if (s >= 0)
            {
                const vec3 pv = texelFetch(uRtMaskProj, ivec3(t.px[k], rtMaskProjLayer(s)), 0).rgb;
                if (!(t.w[k] <= 0.0)) { v += t.w[k] * pv; ws += t.w[k]; }
                if (inb) { vb = max(vb, pv); hb = true; }
            }
        }
    }
    const vec3 r = ws > 0.0 ? v / ws : vec3(1.0);
    // #1086: as the sun -- the brightest behind tap that traced this light, else the old value
    return hb ? mix(vb, r, t.conf) : r;
}

vec3 rtMaskProj(uint light, vec3 P) { return rtMaskProjTaps(light, rtMaskTaps(P)); }
