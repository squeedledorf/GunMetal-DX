// vkclean — RT SUN SHADOW CACHE (#771, RenderVKRTShadowCache): re-trace only where something
// changed. Shared by the tile classifier (clean_rt_mask_cache.comp) and the trace
// (clean_rt_shadow_mask.comp). The includer declares set 3 bindings 0 (pre-pass depth),
// 5 (history acc sun), 8 (history view depth), 9 (history meta), 22 (history depth gradient +
// flags) and includes clean_rt_mask_common.glsl first.
//
// Per mask frame (LLCleanRendererVK::rtMaskCacheRecord):
//   1. CLASSIFY (one workgroup per TxT mask tile, T = RenderVKRTShadowCacheTile 8 / 16): a tile
//      is RE-TRACED when any receiver in it has no reprojected history (disocclusion, cut,
//      first frame -- the temporal pass's nearest-tap plane test), a young history (length
//      below RenderVKRTShadowCacheMinHistory), a moving caster in its history (the trace's
//      rt_block_dyn bit or the temporal pass's persistence countdown), a NON-UNIFORM history
//      3x3 (a shadow edge / penumbra / translucent tint: anything not fully lit or fully
//      blocked within RenderVKRTShadowCacheEps), sits near the shadow range's fade band;
//      when its receivers meet a SWEPT CASTER VOLUME (round 3): every caster that MOVED
//      (avatars / animesh whose pose or root changed -- held / still characters do not
//      count --, the ingest's moving and spinning objects; this and last frame's box) and
//      every static change sphere (rez / delete / edit / re-registration, kept for
//      RenderVKRTShadowCacheChangeFrames frames) as its box swept along the light's travel
//      down to the ground under it (bounded by RenderVKRTShadowCacheMaxShadow). The tile's
//      receivers' world AABB is tested against the swept box EXACTLY (a segment-vs-box slab
//      test in the Minkowski difference, padded by the penumbra spread); the host bins each
//      volume's projected convex hull (not its rectangle) into coarse screen cells, so a tile
//      only tests the volumes over its cell; or in the progressive refresh
//      slice (hashed round robin: every tile at least every RenderVKRTShadowCacheRefreshFrames,
//      RenderVKRTShadowCacheSunRefreshFrames while the sun drifts). Otherwise it is REUSED.
//   2. FINALIZE: the two block counts -> two indirect dispatch argument sets.
//   3. TRACE (indirect, list 0): the trace as before. REUSE (indirect, list 1): the same
//      shader without sun rays -- the receiver's depth plane / ids / projectors are rebuilt
//      as always (every pixel, every frame), the sun's raw value is the EXACT class of the
//      reprojected accumulated history (1 fully lit / 0 fully blocked -- what the rays
//      return there; never the history value itself, which drifted under the temporal
//      pass's bilinear reprojection), so the temporal / spatial passes run unchanged and
//      converge as without the cache.
// Edges (non-uniform 3x3) are always traced, so a slowly moving sun keeps every shadow edge
// current (a 1-texel ring per frame); the refresh slice bounds the staleness of anything
// no signal reports (a caster's texture / LOD streaming in) to the refresh period.

// Set 4 binding 0 (host, per frame slot): header + swept casters + their screen bins.
// Capacities: llcleanrenderervk.h kRtCacheMaxCasters / kRtCacheMaxCells / kRtCacheMaxBinned.
struct RcCaster
{
    vec4 a;   // xyz box min (camera-relative), w sweep length along rc_D (m)
    vec4 b;   // xyz box max (camera-relative), w kind (0 moving caster, 1 static change)
};
layout(std430, set = 4, binding = 0) readonly buffer RtCacheParams
{
    uvec4 rc_h0;          // x tile size (mask texels: 8 / 16), y casters, z -, w flags
                          // (bit 0 force-trace every tile, bit 1 count stats, bit 2 STATIC CARRY:
                          // the camera is bit-identical to the last mask frame's and no ray-
                          // traced projector exists -- reused blocks then skip ALL mask work)
    uvec4 rc_h1;          // x refresh period (frames), y frame index, z blocks per row (8x8), w block capacity
    vec4  rc_f0;          // x uniform eps, y min history length, z depth pad (m), w full-trace fraction
                          // (the finalize falls back to one plain full dispatch above it)
    vec4  rc_vzrow;       // view depth of a camera-relative point: dot(P - cam, xyz) + w (the host's clip w)
    uvec4 rc_h2;          // x screen cell size (mask texels, a multiple of 16), y cells per row,
                          // z cell rows, w 1 = binned (else every tile tests every caster)
    vec4  rc_D;           // xyz the shadow direction (-L, unit), w tan(sun disc radius)
    RcCaster rc_cast[4096];
    uvec2 rc_cells[4096]; // per cell: first index, count (into rc_bin)
    uint  rc_bin[];       // caster indices, cell after cell
};
float rcViewDepth(vec3 rel) { return dot(rel, rc_vzrow.xyz) + rc_vzrow.w; }
// Set 4 binding 1 (device): counts, indirect args, stats, block lists.
layout(std430, set = 4, binding = 1) RT_CACHE_WORK_QUAL buffer RtCacheWork
{
    uint rc_count[4];     // 0 traced blocks, 1 reused blocks (classifier atomics), 2 the finalize's
                          // FULL flag (traced share above rc_f0.w: list 0 = every block in raster
                          // order, list 1 empty)
    uint rc_args[8];      // [0..2] trace dispatch, [4..6] reuse dispatch (byte offsets 16 / 32)
    uint rc_stats[32];    // accumulated since the host's last readback (RC_ST_*)
    uint rc_list[];       // [0, cap) trace blocks, [cap, 2 cap) reuse blocks: x | y << 16 (8x8 blocks)
};

// rc_stats slots (llcleanrenderervk.cpp rtMaskCacheHarvest reads the same order).
#define RC_ST_TRACED   0u   // tiles traced
#define RC_ST_REUSED   1u   // tiles reused (sky-only tiles included)
#define RC_ST_SKY      2u   // tiles without a receiver
#define RC_ST_FORCED   3u   // reason: the host forced every tile (cut, sun jump, overflow, ...)
#define RC_ST_NOHIST   4u   // reason: a receiver without reprojected history
#define RC_ST_YOUNG    5u   // reason: a history shorter than the minimum
#define RC_ST_MOVER    6u   // reason: a moving caster in the history
#define RC_ST_EDGE     7u   // reason: a non-uniform history 3x3 (edge / penumbra / tint)
#define RC_ST_FADE     8u   // reason: the shadow range's fade band
#define RC_ST_CHANGE   9u   // reason: a static change's swept volume meets the receivers
#define RC_ST_DYN     10u   // reason: a moving caster's swept volume meets the receivers
#define RC_ST_REFRESH 11u   // reason: the progressive refresh slice
#define RC_ST_BTRACE  12u   // 8x8 blocks traced
#define RC_ST_BREUSE  13u   // 8x8 blocks reused
#define RC_ST_FRAMES  14u   // classifier frames
#define RC_ST_FULL    15u   // frames the finalize fell back to the plain full dispatch
#define RC_ST_NH_PREV 16u   // nohist tiles by the first failing texel's cause: off last frame's view
#define RC_ST_NH_SKY  17u   //   ... every tap sky / unwritten
#define RC_ST_NH_DEPTH 18u  //   ... no tap's depth plane matches
#define RC_ST_CARRY   19u   // tiles carried whole (static carry: no mask work at all)
#define RC_ST_TESTS   20u   // swept-volume tests run (the bins' efficiency)

// Does the receiver box [tmn, tmx] meet the caster box swept along D by [0, t]? Exact: a
// point p of the receivers lies in the volume iff p - q = s D for a caster point q, i.e.
// the segment s D (s in [0, t]) meets the Minkowski difference [tmn - cmx, tmx - cmn],
// grown by g (the penumbra spread at the far end + the pad). Slab test.
bool rcSweptHit(vec3 tmn, vec3 tmx, RcCaster c, vec3 D, float tn, float pad)
{
    const float t = c.a.w;
    const float g = pad + t * tn;
    const vec3 bmn = tmn - c.b.xyz - g, bmx = tmx - c.a.xyz + g;
    float s0 = 0.0, s1 = t;
    for (int k = 0; k < 3; ++k)
    {
        if (abs(D[k]) < 1e-7)
        {
            if (bmn[k] > 0.0 || bmx[k] < 0.0) return false;
            continue;
        }
        float u = bmn[k] / D[k], v = bmx[k] / D[k];
        if (u > v) { const float w = u; u = v; v = w; }
        s0 = max(s0, u); s1 = min(s1, v);
        if (s0 > s1) return false;
    }
    return true;
}

// Push p0.y bits 18-20 (an exact integer; clean_rt_mask_common.glsl rtModeBits).
bool rcListMode()  { return (rtModeBits() & (1u << 18u)) != 0u; }   // trace: blocks from rc_list
bool rcReuseMode() { return (rtModeBits() & (1u << 19u)) != 0u; }   // ... the reuse list (no sun rays)
bool rcFinalize()  { return (rtModeBits() & (1u << 20u)) != 0u; }   // classifier: the args pass

// STATIC CARRY (round 2, rc_h0.w bit 2): a reused block of a frame whose camera did not move
// (and with no ray-traced projector anywhere) is CARRIED -- the reuse dispatch copies last
// frame's per-texel outputs (ids, depth plane, accumulation, meta) into this frame's
// history slot and writes the class value as raw, and the temporal / spatial passes skip
// it (its final mask texels keep last frame's filtered values). Reuse-list entries carry
// bit 31; the per-block bitmap after the two lists ([2 cap, 2 cap + cap / 32]) is what the
// temporal / spatial passes test (only with push bit 18: the cache ran this frame).
bool rcStaticCarry() { return (rc_h0.w & 4u) != 0u; }
bool rcCarried(uvec2 blk)
{
    if (!rcListMode() || rc_count[2] != 0u || !rcStaticCarry()) return false;
    const uint bi = blk.y * rc_h1.z + blk.x;
    return (rc_list[2u * rc_h1.w + (bi >> 5u)] & (1u << (bi & 31u))) != 0u;
}

#ifndef RC_NO_HIST
// The receiver at mask texel m reprojected into last frame's mask: of the 2x2 bilinear
// taps around the temporal pass's continuous coordinate (exact inversion included), the one
// whose stored depth plane best predicts the reprojected view depth (the temporal pass's
// tap test, without the slope match: this frame's gradient is the trace's output).
// (Round 2: the NEAREST tap alone failed wherever that one texel had sampled another
// surface -- below full resolution / with jitter a mask texel's sample moves inside its
// footprint, so at foliage, alpha-tested cut-outs, silhouettes and sky edges the nearest
// tap often holds the neighbour surface while another tap matches: ~14 % of tiles read
// "no history" with a still camera. The temporal pass accepts any matching tap.)
// Returns 0 = a tap (hn), else why not: RC_NH_PREV (behind last frame's eye / off its
// mask), RC_NH_SKY (every tap sky / unwritten), RC_NH_DEPTH (no tap's plane matches).
#define RC_NH_PREV  1u
#define RC_NH_SKY   2u
#define RC_NH_DEPTH 3u
uint rcHistTapWhy(vec3 P, ivec2 rp, out ivec2 hn)
{
    hn = ivec2(0);
    if (!rtMaskTemporal() || !rtDenoise()) return RC_NH_PREV;
    vec2 prpx; float pw;
    if (!rtMaskPrevPx(P, rp, prpx, pw)) return RC_NH_PREV;
    const vec2 s = rtMaskScale();
    const vec2 jp = mp.p1.xy;
    const vec2 Jp = (mp.dims.z < mp.dims.x) ? jp : vec2(0.0);
    vec2 hc = (prpx + jp) * s - Jp - 0.5;
    const bool ex = rtExactReproj();
    if (ex) hc = rtExactHc(prpx, jp, Jp, s, hc);
    const ivec2 hb = ivec2(floor(hc));
    const ivec2 mx = ivec2(mp.dims.zw) - 1;
    const float tol = max(pw * 0.02, 0.03);
    float best = 1.0;   // |error| / tol of the best tap (must stay below 1)
    bool any_in = false, any_surf = false;
    for (int i = 0; i < 4; ++i)
    {
        const ivec2 t = hb + ivec2(i & 1, i >> 1);
        if (any(lessThan(t, ivec2(0))) || any(greaterThan(t, mx))) continue;
        any_in = true;
        const float hz = texelFetch(uHistDepth, ivec3(t, RT_MASK_L), 0).x;
        if (!(hz > 0.0)) continue;
        any_surf = true;
        const vec2 hg = texelFetch(uHistDepthG, ivec3(t, RT_MASK_L), 0).xy;
        const vec2 off = ex ? prpx - rtPrevCentre(vec2(t), jp, Jp, s) : (hc - vec2(t)) / s;
        const float zp = hz + clamp(dot(hg, off), -0.25 * hz, 0.25 * hz);
        const float e = abs(zp - pw) / tol;
        if (e < best) { best = e; hn = t; }
    }
    if (!any_in) return RC_NH_PREV;
    if (!any_surf) return RC_NH_SKY;
    return best < 1.0 ? 0u : RC_NH_DEPTH;
}
bool rcHistTap(vec3 P, ivec2 rp, out ivec2 hn) { return rcHistTapWhy(P, rp, hn) == 0u; }

#endif   // RC_NO_HIST

// History visibility class: 1 fully lit, 2 fully blocked, 0 anything else (edge, penumbra,
// translucent tint, NaN).
uint rcClass(vec3 v, float eps)
{
    if (any(isnan(v)) || any(isinf(v))) return 0u;
    if (all(greaterThanEqual(v, vec3(1.0 - eps)))) return 1u;
    if (all(lessThanEqual(v, vec3(eps)))) return 2u;
    return 0u;
}
