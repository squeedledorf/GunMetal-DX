// vkclean -- RAY-TRACED GLOBAL ILLUMINATION (#87, ReSTIR GI; RenderVKGI): the shared state of the
// GI trace (clean_deferred_light.comp pass 11 -DCLEAN_GI_TRACE), the GI denoiser (a second
// LLCleanDenoiserVK, clean_dn_*.comp unchanged) and the consumers (the split lighting's combine /
// BASE: deferReflIn replaces the probes' DIFFUSE irradiance with the denoised GI where it is valid).
//
// THE SIGNAL: per GI texel (the render extent x RenderVKGIScale, the denoiser's jittered sample
// convention) the receiver's IRRADIANCE / pi -- the cosine-weighted mean incoming radiance, the
// same quantity as the reflection probes' irradiance maps (clean_probe_filter.comp mode 1: E / pi),
// so the lit pass multiplies it by its own diffuse colour exactly as the probe term. Demodulated by
// construction (no albedo of the receiver in it).
//
// ONE BOUNCE (GI-1): a cosine-weighted ray from the G-buffer receiver; its closest hit is shaded by
// the RT reflections' hit shading (clean_rt_shade.glsl rtShadeHit: the sun / moon with one shadow
// ray, the local lights through the light grid's picks or the cluster, emissive / fullbright
// surfaces, the hit's ambient per GI_HIT_AMB_*), a miss takes the sky along the ray.
// RESERVOIRS (GI-2 temporal, GI-3 spatial; Ouyang et al. 2021 ReSTIR GI): the sample is the
// path's second vertex x_s with its normal n_s and outgoing radiance L; a reused sample is
// RECONNECTED from this receiver with the solid-angle Jacobian of the move
//     J = (|cos phi_now| / |cos phi_then|) x (|x_s - x_then|^2 / |x_s - x_now|^2)
// (phi = the angle at x_s between n_s and the direction to the receiver; 1 for a sky sample) and
// its visibility from this receiver is traced once (no light through walls from a neighbour's
// sample). Target p(y) = lum(L) x max(N.w, 0); the 1/M combine; M capped (prm2.x frames).
// RADIANCE CACHE (GI-4): a world-space hash grid of irradiance (SHaRC-style, our own): cells of
// prm3.x m x 2^level (the level grows with the distance to the camera), split by the dominant axis
// of the normal (the two faces of a thin wall never share a cell), filled by every GI receiver's
// estimate (atomic fixed-point accumulation), resolved once per frame into an exponential mean
// (prm3.z frames), evicted after prm3.w frames untouched, and INVALIDATED where the host's change
// spheres say geometry moved / appeared / vanished -- never by camera motion (the cells are world
// boxes). A GI hit takes its ambient from the cache (multi-bounce: last frame's irradiance there)
// before the probes. Bounded: a fixed table (cfg.z slots), live / cap / peak in the stats.
#ifndef CLEAN_GI_COMMON_GLSL
#define CLEAN_GI_COMMON_GLSL

#define GI_F_ON         1u     // GI runs this frame (the frame info's foliage_rt.x bit 128 too)
#define GI_F_FINAL_IN   2u     // the GI denoiser's final image is IN (an odd a-trous count), else OUT
#define GI_F_TEMPORAL   4u     // GI-2: reuse last frame's reservoir
#define GI_F_SPATIAL    8u     // GI-3: + its neighbours'
#define GI_F_CACHE      16u    // GI-4: the radiance cache is built / read
#define GI_F_RES        32u    // the reservoir buffer is bound at this extent (write this parity)
#define GI_F_RES_PREV   64u    // ... and last frame's parity holds this extent's reservoirs
#define GI_F_CACHE_FILL 128u   // the receivers write their estimate into the cache
#define GI_SPECOCC_SHIFT 8u    // bits 8-9: the specular occlusion (RenderVKGISpecOcclusion: 0 off,
                               // 1 the sky visibility, 2 max(it, GI / probe irradiance))
#define GI_EVERY_SHIFT 10u     // bits 10-11: trace 1 texel in 2^n per frame (RenderVKGITraceEvery 1 / 2 / 4;
                               // the others reuse their reservoirs only, or hold the history)
#define GI_F_MISS_PROBES 4096u // a ray that leaves without a hit takes the probes' far field along it
                               // (RenderVKGIMissProbes), not the raw sky
#define GI_F_CULL_BACK   8192u // GI rays cull single-sided back faces in hardware (RenderVKGICullBackFaces)
#define GI_F_CEL_OCC     16384u // cel shading's flatten reference (the sky ambient: the rim light's and the
                                // flattened ambient's level) x GI's occlusion (RenderVKGICelOcclusion)
#define GI_F_SPECOCC_RT_RES 32768u // RT-marked pixels: the GI specular occlusion scales the probe env their
                                   // RT splice LEFT (misses, fades) instead of skipping them
                                   // (RenderVKGISpecOcclusionRTResidual)
#define GI_F_BACK_STOP  131072u // (RenderVKGIBackFaceStop) a GI ray whose first surface is a SINGLE-SIDED BACK
                                // FACE stops there and returns no light (the surface's inside). Before, the
                                // walk re-traced past back faces (and GI_F_CULL_BACK culled them in hardware),
                                // so a ray that started inside geometry -- the receiver's depth-rebuilt point a
                                // hair behind / inside its own or an overlapping prim at a contact edge, a
                                // moving avatar's raster vs. TLAS pose -- or slipped into a seam walked out
                                // through the far side and brought back the lit world / the sky: sparkles
                                // along edges. Hardware back-face culling is off for GI rays with it.
#define GI_F_POINT_SHADOW 262144u // (RenderVKGIHitPointShadows) GI hits' point lights join the hit shadow-ray
                                  // budget (RenderVKGIHitShadowRays: the K brightest shares get a ray); before,
                                  // point lights lit every GI hit unshadowed -- crevices, undersides, the far
                                  // side of a wall: bright samples at contact edges.
#define GI_F_REPROJ_SEARCH 524288u // (RenderVKGIReprojSearch) a temporal reservoir candidate whose nearest texel is
                                  // not this surface (sub-texel motion across a thin strand / a silhouette at the
                                  // GI extent) takes the best-matching texel of the 3 x 3 around the reprojected
                                  // point instead (the GI denoiser's history the same: DN_F_REPROJ_SEARCH)
#define GI_F_UPS_NORMAL 1048576u  // (RenderVKGIUpsampleNormal) the half / quarter-extent GI upsample weights its taps by
                                  // their normal too (prm4.w: the GEOM parity) and, with no tap on this surface,
                                  // takes the best-matching texel of the 4 x 4 (deferGiFetch)
#define GI_F_HAZE_OCC  4194304u // (RenderVKGIHazeOcclusion) the main view's haze IN-SCATTER (sky light) at a lit pixel x
                               // min(1, 2 x its GI sky visibility): a sealed room's walls take no sky-lit haze
                               // (the owner's teal glow in the gold cube); outdoor facades (half the sky) keep it
#define GI_F_STATS     2097152u // the trace / resolve counters (stat .. stat4) are wanted: the host reads them
                               // back only while the GPU timers run (RenderVKGpuTimers) -- else no stat atomics
#define GI_F_LAG       8388608u // (#978 phase 2, RenderVKGILatencyFrame) ONE FRAME OF GI LATENCY: this frame's trace runs
                               // AFTER the combine (on the async queue beside the rest of the frame), so the GI images
                               // the combine / BASE read hold LAST frame's result -- in last frame's pixel grid, from
                               // last frame's camera. deferGiFetch then reprojects (ssr.taa_prev_vp: the receiver's
                               // point into last frame's view; last frame's view depth = clip w, the images' alpha
                               // convention) and takes only taps on this surface; a disocclusion takes the radiance
                               // cache's cell, else no estimate (the probes). The header's GI_F_FINAL_IN and prm4.w
                               // (the GEOM parity) then describe LAST frame's trace (the host's mGiLag*).
#define GI_F_LAG_STALE 16777216u // (GI_F_LAG) last frame's GI images are unusable this frame (the first frame of the
                               // mode, a resize, the GI images / extent re-created, a frame without a trace, an
                               // offscreen / dropped recording in between): the cache, else the probes, everywhere
#define GI_F_LIGHT_CHG  65536u // CHANGED LIGHTS (RenderVKGILightChangeCut): a reservoir sample stores the
                               // radiance its hit point had WHEN IT WAS SHADED; a reused sample whose
                               // point a light reaches that changed since last frame (moved, or its colour /
                               // intensity / radius / falloff changed by more than RenderVKReSTIRFlickerEps:
                               // a colour-cycling club light, a fire, a moving lamp -- the host's chg mask,
                               // measured against each light's GI REFERENCE (its record when GI last flagged
                               // it; a move counts past max(RenderVKReSTIRMoveEps, RenderVKGILightMoveRel x
                               // its radius) -- #980: a worn light's mm-a-frame sway flagged every frame)
                               // carries a stale colour: when it is the PICK and those lights are SIGNIFICANT
                               // at its point (their unshadowed light there, old record or new, at least
                               // RenderVKGIReshadeThreshold of its stored radiance: prm5.x, chg_prev; #980),
                               // its point is RE-SHADED with this frame's lights (one closest-hit ray that is
                               // also its visibility ray), so the reuse stays and the colour is current; the
                               // share of the candidates' weight such SIGNIFICANT lights hold caps the GI
                               // denoiser's history (the moving-light rule's MOT w). Without it: crawling
                               // pink / green blotches under club lights (each blotch one stale sample spread
                               // by the spatial reuse).

// hit ambient (prm.w): what a GI hit's own indirect light is
#define GI_HIT_AMB_NONE   0u   // nothing: one bounce, a sealed room goes dark
#define GI_HIT_AMB_PROBES 1u   // the reflection probes there (the RT reflections' rule)
#define GI_HIT_AMB_CACHE  2u   // the radiance cache, the probes where it has no cell
#define GI_HIT_AMB_CACHE0 3u   // the radiance cache, nothing where it has no cell

// debug views (cfg.w; RenderVKGIDebug)
#define GI_DBG_FINAL   1u      // the denoised GI irradiance, full screen
#define GI_DBG_RAW     2u      // this frame's raw estimate (no history, no filter)
#define GI_DBG_LEN     3u      // the GI denoiser's history length
#define GI_DBG_VAR     4u      // its variance
#define GI_DBG_RES     5u      // the reservoir: hue = where the sample came from (red fresh, green
                               // temporal, blue spatial), brightness = M / the cap
#define GI_DBG_CACHE   6u      // the cache at the receiver: green = a resolved cell (its value),
                               // red = no cell, yellow = a cell without samples yet
#define GI_DBG_SKYVIS  7u      // the denoised sky visibility (the specular occlusion's input)
#define GI_DBG_OUTLIER 9u      // SPARKLE CAUSES: texels whose GI estimate is > 4x the mean of their subgroup's
                               // (an 8 x 4 texel block), coloured by where the sample came from -- red = a
                               // fresh ray that left without a hit (sky / probe far field), orange = a fresh
                               // GRAZING ray (receiver cosine < 0.2: the hairline-gap escapes), yellow = a fresh
                               // hit, green = reused from last frame, blue = from a neighbour, cyan = a
                               // re-shaded reused sample, WHITE = a fresh hit with no reuse candidate at all (a
                               // disocclusion: RenderVKGIDisoccTaps / RenderVKGIReprojSearch target these);
                               // every other texel dim grey (its GI), and DIM PURPLE = a
                               // fresh ray stopped at a back face this frame (GI_F_BACK_STOP: a leak caught --
                               // dark, never a sparkle itself)
#define GI_DBG_CHG     8u      // GI_F_LIGHT_CHG: red = the significant changed lights' share of the candidates'
                               // weight (the GI denoiser's history cap), green = the picked reused sample was
                               // re-shaded (changed lights significant at its point), DIM green = a changed light
                               // reaches the pick but too weakly there (RenderVKGIReshadeThreshold: kept as it
                               // was), blue = the reservoir's W was scaled by the firefly clamp

#define GI_SPHERES_MAX 64u
#define GI_CHG_PREV_MAX 64u   // (#980) chg_prev's records
struct GiHeader
{
    uvec4 cfg;     // x GI_F_*, y frame, z cache slots (power of two; 0 = none), w debug view
    vec4  prm;     // x intensity, y minimum ambient (the probe irradiance's fraction kept as a floor),
                   // z ray length (m), w hit ambient (GI_HIT_AMB_*)
    vec4  prm2;    // x M cap (frames), y spatial taps, z spatial radius (texels), w firefly clamp
                   // (luminance of one sample's irradiance estimate; 0 = off)
    vec4  prm3;    // x cache cell edge at level 0 (m), y distance per level step (m), z the cache
                   // mean's longest window (frames), w the eviction age (frames)
    uvec4 stat;    // x rays, y hits, z misses (sky), w sum of the hit distances (dm)
    uvec4 stat2;   // x temporal taps kept, y spatial taps kept, z reused samples rejected by their
                   // visibility ray, w sum of the reservoirs' M (x 4)
    uvec4 stat3;   // x cache lookups at hits, y of them found, z cells inserted, w inserts that
                   // found the table full
    uvec4 stat4;   // x live cells (the resolve), y evicted, z invalidated by change spheres, w picked
                   // reused samples re-shaded because a changed light reaches them (GI_F_LIGHT_CHG)
    uvec4 sph_n;   // x change spheres this frame, y overflow, z (#980) picked reused samples a changed
                   // light reaches but too weakly to re-shade (RenderVKGIReshadeThreshold; a stat, GI_F_STATS)
    vec4  prm4;    // x the smallest receiver cosine a reservoir candidate may have (RenderVKGIReuseCosMin;
                   // 0 = the old 1e-4 floor), y the re-shade gain limit (RenderVKGIReshadeGain; 0 =
                   // none), z the disocclusion taps (RenderVKGIDisoccTaps; 0 = none), w the GI
                   // denoiser's GEOM parity the CONSUMERS read (GI_F_UPS_NORMAL): this frame's trace's,
                   // or under GI_F_LAG last frame's (the trace takes its own from the push, rtr2.w)
    uvec4 chg[2];  // the lights that moved or whose radiance changed (bit = light index; the host's mGiChg:
                   // against each light's GI reference), GI_F_LIGHT_CHG only
    vec4  prm5;    // (#980) x the re-shade significance threshold (RenderVKGIReshadeThreshold; 0 = re-shade
                   // wherever a changed light reaches), y the old records in chg_prev, z (#1041) the own-fixture
                   // box limit of the hits' budgeted point-light rays (RenderVKReSTIRFixtureSize, m; 0 = off)
    vec4  sph[GI_SPHERES_MAX];   // xyz centre (world), w radius
    vec4  chg_prev[128];   // (#980; 2 x GI_CHG_PREV_MAX) the changed lights' OLD records (their GI reference), in
                   // ascending light index (the k-th set bit of chg): [2k] pos_radius (w < 0: it MOVED --
                   // radius |w|, its old projector frustum unknown), [2k + 1] color_falloff. A changed light
                   // past prm5.y has none: significant wherever it reaches.
};                 // 3280 B (LLCleanRendererVK::GiHeader)

#ifndef GI_STATE_ACCESS
#define GI_STATE_ACCESS
#endif
// The state buffer: the header, then the cache -- RESOLVED cells (2 uvec4 each: [0] key (0 empty,
// GI_KEY_TOMB freed), n | age << 16, centre x, y bits; [1] centre z bits, half(r, g), half(b, 0),
// cell edge bits) at gi_data[2 slot ..], then the ACCUMULATORS (1 uvec4 each: r, g, b fixed point
// x GI_ACC_SCALE, count) at gi_data[2 cap + slot].
#define GI_KEY_TOMB 0xFFFFFFFFu
// (the deferred kernels: set 3 binding 23; the forward lit FS: draw set 0 binding 53, CLEAN_GI_FORWARD)
#ifndef GI_STATE_SET
#define GI_STATE_SET 3
#define GI_STATE_BINDING 23
#endif
layout(std430, set = GI_STATE_SET, binding = GI_STATE_BINDING) GI_STATE_ACCESS buffer GiState
{
    GiHeader gi;
    uvec4    gi_data[];
};
#define GI_ACC_SCALE 64.0
#define GI_ACC_MAXLUM 512.0

// GI ran this frame (the frame info's foliage_rt.x bit 128): only then may the GI state be read.
#define giFrameOn() ((uint(ssr.foliage_rt.x + 0.5) & 128u) != 0u)   // (a macro: some includers see ssr later)
uint giPcg(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
float giLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---- the cache's cell of (P, N): its key (never 0), level and edge ----
uint giCellKey(vec3 P, vec3 N, vec3 cam, out float edge)
{
    const float d = length(P - cam);
    const float step = max(gi.prm3.y, 1.0);
    const int level = clamp(int(floor(log2(max(d / step, 1.0)))), 0, 7);
    edge = max(gi.prm3.x, 0.05) * float(1u << uint(level));
    const ivec3 q = ivec3(floor(P / edge));
    const vec3 a = abs(N);
    const uint axis = (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
    const uint bucket = axis * 2u + ((axis == 0u ? N.x : (axis == 1u ? N.y : N.z)) < 0.0 ? 1u : 0u);
    uint h = giPcg(uint(q.x) ^ giPcg(uint(q.y) ^ giPcg(uint(q.z) ^ giPcg(uint(level) * 8u + bucket))));
    return (h == 0u || h == GI_KEY_TOMB) ? 1u : h;
}
// The cell centre of P at edge (the resolve's change-sphere test).
vec3 giCellCentre(vec3 P, float edge) { return (floor(P / edge) + 0.5) * edge; }
uint giCacheSlots() { return gi.cfg.z; }
// The slot holding key (read only); 0xFFFFFFFF: none.
uint giCacheFind(uint key)
{
    const uint cap = giCacheSlots();
    if (cap == 0u) return 0xFFFFFFFFu;
    uint s = giPcg(key) & (cap - 1u);
    for (uint i = 0u; i < 8u; ++i)
    {
        const uint k = gi_data[2u * s].x;
        if (k == key) return s;
        if (k == 0u) return 0xFFFFFFFFu;   // (a freed slot, GI_KEY_TOMB, keeps the chain going)
        s = (s + 1u) & (cap - 1u);
    }
    return 0xFFFFFFFFu;
}
#ifndef GI_STATE_READONLY
// Find or insert the cell of key (centre / edge written by its inserter); 0xFFFFFFFF: the probe
// window is full (counted). inserted: this call took an empty / freed slot.
uint giCacheFindOrInsert(uint key, vec3 centre, float edge, out bool inserted)
{
    inserted = false;
    const uint cap = giCacheSlots();
    if (cap == 0u) return 0xFFFFFFFFu;
    uint s = giPcg(key) & (cap - 1u);
    for (uint i = 0u; i < 8u; ++i)
    {
        uint k = gi_data[2u * s].x;
        if (k == key) return s;
        if (k == 0u || k == GI_KEY_TOMB)
        {
            const uint prev = atomicCompSwap(gi_data[2u * s].x, k, key);
            if (prev == k)
            {
                inserted = true;
                gi_data[2u * s].y = 0u;
                gi_data[2u * s].z = floatBitsToUint(centre.x);
                gi_data[2u * s].w = floatBitsToUint(centre.y);
                gi_data[2u * s + 1u] = uvec4(floatBitsToUint(centre.z), 0u, 0u, floatBitsToUint(edge));
                return s;
            }
            if (prev == key) return s;
        }
        s = (s + 1u) & (cap - 1u);
    }
    return 0xFFFFFFFFu;
}
// A receiver's estimate (irradiance / pi) in the accumulator's fixed point.
uvec3 giCacheQ(vec3 E)
{
    const float l = giLum(E);
    if (l > GI_ACC_MAXLUM) E *= GI_ACC_MAXLUM / l;
    return uvec3(clamp(E, vec3(0.0), vec3(GI_ACC_MAXLUM)) * GI_ACC_SCALE + 0.5);
}
// A receiver's estimate (irradiance / pi) into its cell's accumulator.
void giCacheAdd(uint s, vec3 E)
{
    const uvec3 q = giCacheQ(E);
    const uint a = 2u * giCacheSlots() + s;
    atomicAdd(gi_data[a].x, q.x);
    atomicAdd(gi_data[a].y, q.y);
    atomicAdd(gi_data[a].z, q.z);
    atomicAdd(gi_data[a].w, 1u);
}
#endif
// The cell's resolved irradiance; false: no cell or no samples yet.
// Light li changed since last frame (GI_F_LIGHT_CHG; the host's mask).
bool giLightChanged(uint li) { return li < 256u && (gi.chg[li >> 7u][(li >> 5u) & 3u] & (1u << (li & 31u))) != 0u; }
bool giCacheRead(uint s, out vec3 E)
{
    E = vec3(0.0);
    if (s == 0xFFFFFFFFu) return false;
    const uvec4 c0 = gi_data[2u * s];
    const uvec4 c1 = gi_data[2u * s + 1u];
    if ((c0.y & 0xFFFFu) == 0u) return false;
    const vec2 rg = unpackHalf2x16(c1.y);
    E = vec3(rg, unpackHalf2x16(c1.z).x);
    return !(any(isnan(E)) || any(isinf(E)));
}

// ---- reservoirs (GI-2 / GI-3): 2 uvec4 per texel and parity, set 3 binding 22 ----
//   [0] x_s (3 x float bits), W (float bits)
//   [1] x: n_s (snorm oct 2 x 16), y: half(L.r, L.g), z: half(L.b, M), w: half(the distance from
//       the receiver that took it to x_s (0 = a sky sample), |cos phi| there)
struct GiSample { vec3 xs; vec3 ns; vec3 L; float dist; float cosv; };
vec2 giOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
uint giPackN(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    const vec2 e = n.z >= 0.0 ? n.xy : giOctWrap(n.xy);
    return packSnorm2x16(e);
}
vec3 giUnpackN(uint v)
{
    const vec2 e = unpackSnorm2x16(v);
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
#endif
