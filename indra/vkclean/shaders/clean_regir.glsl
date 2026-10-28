// vkclean -- WORLD-SPACE LIGHT RESERVOIR GRID (RenderVKLightGrid; #869 part 1, ReGIR-style
// "world-space ReSTIR"). Built once per frame by clean_regir_build.comp ("c_light_regir", in the
// light pass right after the clusters / froxels), read by any shader that includes this file
// (today: the RT reflection hits, clean_deferred_light.comp passes 11 / 12 / 14 through
// clean_rt_shade.glsl's RT_SHADE_REGIR hook; later the main view's ReSTIR DI).
// (Not the deferred lighting's "LIGHT GRID" of RenderVKLightCulling 1: that one is a per-screen-
// tile light MASK; this is a per-world-cell set of light SAMPLES.)
//
// LAYOUT. A camera-centred CLIPMAP: REGIR_LEVELS_MAX nested levels of REGIR_DIM_X x _Y x _Z cells
// (x / y horizontal, z up: agent space), level l's cell edge = cell size x 2^l
// (RenderVKLightGridCellSize, default 1 m; RenderVKLightGridLevels of them, default 4: 1 / 2 / 4 /
// 8 m cells, the outermost level +-128 m across and +-64 m up / down). Each level's origin is the
// camera snapped DOWN to that level's cell edge minus half its extent (the host, every frame), so a
// cell is a fixed world box while the camera moves inside it -- no swimming. A point takes the
// FINEST level that contains it; past the outermost level it has no cell (the caller falls back
// to its exact path).
// A cell = (1 + R) uvec2 words at rg_cells[cell * (R + 1)]:
//   [0]        x = the cell's candidate count M (lights that can reach ANY point of the cell)
//                  | REGIR_CELL_EXACT, y = float bits of the cell target sum W = sum p_cell
//   [1 .. R]   x = light index, y = float bits of its inverse cell pdf W / p_cell(light)
// R = RenderVKLightGridReservoirs (2 .. REGIR_RES_MAX, default 8). Fixed buffer: the header +
// REGIR_LEVELS_MAX x REGIR_CELLS_PER_LEVEL x (REGIR_RES_MAX + 1) x 8 B = 8.5 MiB, allocated once
// on first use, never grown (live cells / bytes in the stats line).
//
// BUILD (per cell, every frame from the frame's light records: moving / colour-cycling lights are
// always current, nothing is carried over). Candidates: every light whose sphere reaches the cell
// box and, for a projector, whose frustum (x, w - x, y, w - y, z >= 0 of proj_mat) is not wholly
// outside it (all 8 corners outside one half-space) -- CONSERVATIVE: a superset of the lights
// clean_rt_shade.glsl's walk could light any point of the cell with. Target p_cell (unshadowed,
// no BRDF): colour luminance (intensity folded in) x the legacy distance falloff at the cell centre
// (floored by a quarter of the falloff at the cell's closest point, so a light reaching only a
// corner keeps p > 0) x a cone factor (1 with the cell centre inside the projector frustum, 0.25
// when the cell only straddles its edge). A projector pointing away has no candidate cell at all.
// Gobo textures are NOT in the target (the build has no bindless table): a dark gobo region costs
// variance, never bias.
//   M <= N (picks per hit): the cell stores its whole candidate list, REGIR_CELL_EXACT: the hit
//            evaluates every light exactly (no noise where there is nothing to sample).
//   M >  N:  R STRATIFIED samples of the discrete pdf q = p_cell / W: u_r = (r + xi) / R of the
//            cumulative target, one xi per cell and frame. Each slot stores its light and 1 / q.
//
// HIT (regirHitLights): RIS over the cell's R samples with the hit's own target p_hit (luminance x
// the falloff at the hit x the projector frustum test at the hit x (0.2 + 0.8 max(N.L, 0)); exactly 0
// only where the walk adds an exact 0: out of range / out of the frustum), N picks with
// replacement, duplicates merged. UNBIASED: for candidate weights w_j = p_hit(x_j) / (R q(x_j)) and
// a pick y with probability w_y / sum w, E[f(y) sum w / p_hit(y)] = (1 / R) sum_j f(x_j) / q(x_j);
// over the stratified xi, sample j has the pdf R q restricted to stratum j, so
// E[f(x_j) / q(x_j)] = R x (the integral of f over stratum j) and the sum over j is the exact
// light sum. Needs p_hit > 0 wherever f > 0 (the floor on N.L keeps back-facing projector
// ambiance) and q > 0 for every light that reaches the hit (the conservative candidate test). The
// average of N independent picks keeps it unbiased.
#ifndef CLEAN_REGIR_GLSL
#define CLEAN_REGIR_GLSL

#define REGIR_LEVELS_MAX 4u
#define REGIR_DIM_X 32u
#define REGIR_DIM_Y 32u
#define REGIR_DIM_Z 16u
#define REGIR_CELLS_PER_LEVEL (REGIR_DIM_X * REGIR_DIM_Y * REGIR_DIM_Z)
#define REGIR_RES_MAX 16u
#define REGIR_PICKS_MAX 4u
#define REGIR_CELL_EXACT 0x80000000u
#define REGIR_COUNT_MASK 0xFFFFu
#define REGIR_NO_CELL 0xFFFFFFFFu
// stat[] (host-zeroed every frame; LLCleanRendererVK regirStats): build 0 cells with a candidate,
// 1 sum of their candidates, 2 the largest candidate count; RT trace (pass 11, flushed with its
// rtt_hit counters) 3 hits sampled from the grid, 4 hits in an exact-list cell, 5 hits kept exact
// by the footprint threshold, 6 hits outside the grid, 7 lights evaluated at sampled hits, 8 sum
// of the cell candidate counts at sampled hits, 9 lights evaluated at hits that changed since last
// frame (every path).
#define REGIR_STATS 12

// ---- THE ReSTIR DI POPULATION (#869 Phase 5, RenderVKReSTIRDI; clean_deferred_light.comp pass 15) ----
// A SECOND set of cells in the same buffer, same clipmap (origins, levels, cell size), built by the
// same kernel (population 1) from a different LIGHT SET: the lights whose visibility pass 15 samples
// -- the ray-traced projector records (projStochEligible) and, with RS_F_EXT, every other projector
// and every point light (attached lights only with RS_F_ATTACHED). Its slots are pass 15's
// CANDIDATES: every light of that set that can light a point of the cell has q > 0 (the conservative
// cell test), so the candidate set at P equals the lighting set at P (the consumer multiplies
// exactly those lights by the ratio: rsLightStoch). R = rs_cfg.x slots (<= REGIR_RES_MAX), the
// cells at rg_cells[REGIR_RS_BASE + cell * (REGIR_RES_MAX + 1)] (a fixed stride: the population's
// place never moves with the settings).
#define REGIR_RS_BASE (REGIR_LEVELS_MAX * REGIR_CELLS_PER_LEVEL * (REGIR_RES_MAX + 1u))
#define RS_F_BUILT    1u    // population 1 was built this frame (pass 15 samples it; the frame info
                            // says so too: ssr.foliage_rt.x bit 64, rsFrameOn)
#define RS_F_EXT      2u    // + every projector without a ray-traced record + every point light
#define RS_F_ATTACHED 4u    // + lights on attachments (else they stay unshadowed: facelights)
#define RS_F_TEMPORAL 8u    // pass 15 reuses last frame's reservoirs (5b)
#define RS_F_SPATIAL  16u   // ... and rs_cfg2.y neighbours of them (5c)
#define RS_F_WCUT     32u   // the denoiser's light-set reference W = the cell's target sum (RenderVKReSTIRLightCut 1):
                            // any light moving anywhere in the 1 m cell (or a reprojection into the
                            // next cell) cut the DI history and the reservoirs' reuse; off (default):
                            // W = 1 on grid texels -- the reservoirs re-weigh every reused light with
                            // its CURRENT record (a light switched off drops at once) and the
                            // denoiser's neighbourhood clamp is the anti-lag
#define RS_F_STRIDE   64u   // (RenderVKReSTIRStridedWindow) pass 15's candidate window and the dense
                            // evaluation's sub-window take EVENLY SPACED slots (rsWinSlot), not a
                            // contiguous run: the slots are stratified in cumulative-target order, so a
                            // contiguous run is one stretch of the CDF -- mostly the SAME light repeated
#define RS_F_POP0     256u  // population 0 (the RT reflection hits' grid) is built too
#define RS_F_OCC      512u  // (#869) OCCLUSION CULL (RenderVKReSTIROcclusionCull): pass 15 and the lit kernels
                            // leave out the lights the occlusion cache holds blocked at P (OCCLUSION CULL below)
#define RS_F_OCC_HARD 1024u // (#1052, RenderVKReSTIROcclusionCull 2: A/B) the first cull's rules -- every sample a
                            // note (untraced ones as blocked), 8 frames, a binary verdict per axis-aligned patch
                            // (hard-edged rectangles); clear (1, default) = the SOFT cull (clean_deferred_light.comp)
#define RS_F_CONV_ANY 2048u // (#1052, RenderVKReSTIRConvMultiLight 1: A/B) the denoiser's converged shortcut for
                            // any reused reservoir; clear (default) = only where one light dominates the texel
#define RS_F_REUSE_MV 4096u // (#1038, RenderVKReSTIRReuseMotion) pass 15's temporal reservoir candidate (and the spatial
                            // taps around it) follows the pre-pass MOTION VECTORS (set 3 binding 28; only while the frame
                            // info says they were written), not the camera-only reprojection: a walking avatar's texels
                            // kept no temporal candidate while it moved (clean_deferred_light.comp reuseMotionDelta)
#define RS_F_OCC_PT_DROP 8192u // (RenderVKReSTIROcclusionPointDrop: A/B) the OCCLUSION CULL's old rule for POINT lights --
                            // a culled point light leaves the ratio's light set (and the lit kernels' sum) like a
                            // projector; clear (default) = it stays in the set with its exact zero (clean_deferred_light.comp
                            // OCCLUSION CULL, THE POINT LIGHTS' SHARE)
// (#869) THE MOVING-LIGHT SHADOW RULE (rs_cfg.w; RenderVKReSTIRMoveCut): a moving light's shadow
// moves every frame, but the projector denoiser reprojects its history along the camera / surface
// motion only -- the visibility history of a moved light is wrong wherever its shadow moved. Pass 15
// hands the denoiser the share of this texel's (unshadowed) target that comes from lights that
// MOVED (rs_moved; MOT w), the denoiser shortens that texel's history to RenderVKReSTIRMoveHistory
// frames in proportion and never takes the ReSTIR-converged shortcut there (full a-trous, the
// spatial variance), and a moved light's sample traces RS_MOVE_RAYS disc rays instead of one.
// RS_MOVE_LIGHTS (rs_cfg.w bits 8-11, RenderVKReSTIRMoveLights): such a texel evaluates that many of
// its window's candidate lights, each with a shadow ray, instead of one pick (pass 15's DENSE
// EVALUATION) -- more real samples, not more blur.
#define RS_MOVE_ON 1u
#define RS_STATS 8          // rs_stat: build 0 cells with a candidate, 1 sum of their candidates, 2 the
                            // most; pass 15 (one atomic per subgroup) 3 texels served by the grid, 4
                            // texels by the cluster walk (outside it), 5 sum of the reservoirs' M, 6
                            // temporal taps kept, 7 spatial taps kept

struct RegirHeader
{
    vec4  origin[4];      // level l: .xyz world min corner (snapped), .w cell edge (m)
    uvec4 cfg;            // x levels, y reservoirs per cell R, z picks per hit N, w frame seed
    vec4  params;         // x footprint threshold (m; 0 = sample everywhere), yzw 0
    uint  changed[8];     // host: lights whose record changed since last frame (bit = light index)
    uint  reach[8];       // build: lights that are a candidate of at least one cell
    uint  stat[REGIR_STATS];
    // ReSTIR DI (population 1; LLCleanRendererVK::regirHeaderFill)
    uvec4 rs_cfg;         // x slots per cell R1, y candidates per texel (0 = every slot), z RS_F_*,
                          // w the moving-light rule (RS_MOVE_*; 0 = off)
    uvec4 rs_cfg2;        // x M cap (x the candidate count), y spatial taps, z spatial radius (texels),
                          // w the light-set change that cuts the reuse (relative x 1000)
    vec4  rs_params;      // x point light disc radius (m), y fixture size limit (m), z the disc radius of
                          // a projector without a record (m), w its extra origin bias (m)
    uint  rs_stat[RS_STATS];
    // (#91) EMISSIVE LIGHT RECORDS (RenderVKReSTIREmissive; EMISSIVE LIGHTS below):
    uvec4 rs_em;          // x records written (GPU; may pass the cap), y dropped past the cap, z the cap
                          // (host; 0 = no build this frame), w this build's record PARITY (GPU: the
                          // record build writes it from its push)
    vec4  rs_em_params;   // x the radiance luminance a face needs to emit, y legacy glow -> radiance scale,
                          // z the PBR emissive gain (RenderVKReSTIREmissivePbrScale), w (#1006) the WEAK
                          // power threshold (RenderVKReSTIREmissiveWeakPower; with EM_F_WEAK)
    uint  rs_set[8];      // pass 15's LIGHT SET this frame (bit = light index; the host: rsLightSet)
    uint  rs_remap[64];   // last frame's light index -> this frame's (byte i of the 256; 0xFF = gone)
    uint  rs_moved[8];    // host: lights whose shadow origin MOVED since last frame (bit = light index;
                          // RS_MOVE_ON only -- LLCleanRendererVK::mRsMoved)
    uint  rs_flick[8];    // host: lights whose RADIANCE changed since last frame without moving (colour /
                          // intensity / radius / falloff: a scripted fire, a blinking club light; bit =
                          // light index; RS_MOVE_ON + RenderVKReSTIRFlickerCut -- mRsFlick)
    // (#91) EMISSIVE LIGHTS (host: regirHeaderFill)
    uvec4 rs_em_cfg;      // x emitter slots per cell R_e (<= REGIR_EM_SLOTS_MAX), y GPU: the emitters'
                          // triangles, z EM_F_*, w GPU: emitter entries (the host starts it at the LIGHT
                          // TAKEOVER table's rows T: entries [0, T) are theirs)
    vec4  rs_em_params2;  // x the range cutoff (reflected radiance of a white surface), y the longest
                          // range (m), z (#1006) the cells re-gathered per build for the budget
                          // (RenderVKReSTIREmissiveCellBudget; 0 = no cap), w the rest's refresh period
                          // (builds; RenderVKReSTIREmissiveRestRefresh, >= 1)
    uint  rs_em_stat[4];  // GPU: x cells with an emitter, y sum of their emitters, z pass-15 texels with
                          // an emitter candidate, w emitter samples traced (pass 15)
    // (#91) LIGHT TAKEOVER (RenderVKReSTIREmissiveLightTakeover; EMISSIVE LIGHTS below)
    uvec4 rs_em_take_cfg; // x the takeover table's rows T (host; the emitter entries [0, T) are theirs),
                          // y lights taken over, z lights refused (a block dropped) (GPU: mode 1), w the
                          // rest's SUBSET STRIDE this build (GPU: mode 1; stats)
    // (#986) RECORD REUSE + CELL LISTS (EMISSIVE LIGHTS below; GPU, the header upload zeroes it)
    uvec4 rs_em_cache;    // x records copied from the last build (mode 2), y records built (mode 2),
                          // z cell candidate pairs (mode 5; may pass the cap), w EM_C_* (modes 4, 5)
    uint  rs_em_take[8];  // GPU (mode 1): the point lights taken over this frame (bit = light index)
};                        // 784 B (LLCleanRendererVK::RegirHeader)

#ifndef REGIR_SET
#define REGIR_SET 2
#endif
#ifndef REGIR_BINDING
#define REGIR_BINDING 12
#endif
#ifndef REGIR_ACCESS
#define REGIR_ACCESS
#endif
layout(std430, set = REGIR_SET, binding = REGIR_BINDING) REGIR_ACCESS buffer RegirGrid
{
    RegirHeader rg;
    uvec2       rg_cells[];
};

// clean_light_eval.glsl legacyAtten, the same expression (the build has no bindless table to
// include that file with): 0 exactly where the walk's atten <= 0 test skips a light.
float regirAtten(float d, float falloff)
{
    float a = 1.0 - clamp((d + falloff) / (1.0 + falloff), 0.0, 1.0);
    a *= a;
    return a * 2.0;
}
float regirLum(vec3 c) { return dot(abs(c), vec3(0.2126, 0.7152, 0.0722)); }

uint regirLevels() { return min(rg.cfg.x, REGIR_LEVELS_MAX); }
uint regirRes()    { return clamp(rg.cfg.y, 1u, REGIR_RES_MAX); }
uint regirPicks()  { return clamp(rg.cfg.z, 1u, REGIR_PICKS_MAX); }

// The cell holding P (finest level first), or REGIR_NO_CELL outside the outermost level.
uint regirCell(vec3 P)
{
    const uint levels = regirLevels();
    const vec3 dims = vec3(float(REGIR_DIM_X), float(REGIR_DIM_Y), float(REGIR_DIM_Z));
    for (uint l = 0u; l < levels; ++l)
    {
        const vec4 o = rg.origin[l];
        const vec3 q = (P - o.xyz) / o.w;
        if (all(greaterThanEqual(q, vec3(0.0))) && all(lessThan(q, dims)))
        {
            const uvec3 c = min(uvec3(q), uvec3(REGIR_DIM_X, REGIR_DIM_Y, REGIR_DIM_Z) - 1u);
            return l * REGIR_CELLS_PER_LEVEL + (c.z * REGIR_DIM_Y + c.y) * REGIR_DIM_X + c.x;
        }
    }
    return REGIR_NO_CELL;
}
// Cell box (padded a little: P's own float rounding in regirCell).
void regirCellBox(uint cell, out vec3 lo, out vec3 hi)
{
    const uint l = cell / REGIR_CELLS_PER_LEVEL;
    const uint i = cell - l * REGIR_CELLS_PER_LEVEL;
    const uvec3 c = uvec3(i % REGIR_DIM_X, (i / REGIR_DIM_X) % REGIR_DIM_Y, i / (REGIR_DIM_X * REGIR_DIM_Y));
    const vec4 o = rg.origin[min(l, REGIR_LEVELS_MAX - 1u)];
    const float pad = 1e-3 * o.w + 1e-3;
    lo = o.xyz + vec3(c) * o.w - pad;
    hi = o.xyz + vec3(c + 1u) * o.w + pad;
}

// PCG hash (one 32-bit step) and a small per-invocation stream.
uint regirHash(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float regirU01(inout uint s)
{
    s = regirHash(s);
    return float(s >> 8) * (1.0 / 16777216.0);   // [0, 1)
}

// The cell target of light li (0: not a candidate of the box [lo, hi]). See BUILD above.
float regirCellTarget(uint li, vec3 lo, vec3 hi)
{
    const vec4 pr = lights[li].pos_radius;
    if (!(pr.w > 0.0)) return 0.0;
    const vec3 dq = clamp(pr.xyz, lo, hi) - pr.xyz;
    const float dmin2 = dot(dq, dq);
    if (!(dmin2 < pr.w * pr.w)) return 0.0;
    const vec4 cf = lights[li].color_falloff;
    const float lum = regirLum(cf.rgb);
    if (!(lum > 0.0)) return 0.0;
    const vec3 c = 0.5 * (lo + hi);
    const float dc = length(c - pr.xyz) / pr.w;
    float a = max(regirAtten(dc, cf.w), 0.25 * regirAtten(sqrt(dmin2) / pr.w, cf.w));
    if (lights[li].proj_origin.w >= 0.5)
    {   // PROJECTOR: the walk's frustum gate (x, w - x, y, w - y, z >= 0 of proj_mat) vs the box
        const mat4 M = lights[li].proj_mat;
        const vec4 rx = vec4(M[0][0], M[1][0], M[2][0], M[3][0]);
        const vec4 ry = vec4(M[0][1], M[1][1], M[2][1], M[3][1]);
        const vec4 rz = vec4(M[0][2], M[1][2], M[2][2], M[3][2]);
        const vec4 rw = vec4(M[0][3], M[1][3], M[2][3], M[3][3]);
        const vec4 pl[5] = vec4[5](rx, rw - rx, ry, rw - ry, rz);
        const float sl = 1e-3 * (hi.x - lo.x) + 1e-3;
        [[unroll]] for (int h = 0; h < 5; ++h)
        {
            float m = -1e30;
            [[unroll]] for (int k = 0; k < 8; ++k)
            {
                const vec3 p = vec3((k & 1) != 0 ? hi.x : lo.x, (k & 2) != 0 ? hi.y : lo.y, (k & 4) != 0 ? hi.z : lo.z);
                m = max(m, dot(pl[h].xyz, p) + pl[h].w);
            }
            if (m < -sl * length(pl[h].xyz)) return 0.0;   // (NaN planes: never rejected)
        }
        const vec4 h = M * vec4(c, 1.0);
        const bool inside = h.z > 0.0 && h.w > 0.0 && h.x > 0.0 && h.x < h.w && h.y > 0.0 && h.y < h.w;
        a *= inside ? 1.0 : 0.25;
    }
    const float t = lum * max(a, 1e-6);
    return (t > 0.0 && t < 3.0e38) ? t : 0.0;   // (inf / NaN records: never a candidate)
}

// The hit target of light li at P (normal N): 0 exactly where clean_rt_shade.glsl's walk skips the
// light (out of range, atten <= 0, outside the projector frustum), else > 0.
float regirHitTarget(uint li, vec3 P, vec3 N)
{
    const vec4 pr = lights[li].pos_radius;
    const vec3 lv = pr.xyz - P;
    const float d2 = dot(lv, lv);
    if (d2 >= pr.w * pr.w) return 0.0;
    const float dist = sqrt(d2);
    const vec4 cf = lights[li].color_falloff;
    const float a = regirAtten(dist / pr.w, cf.w);
    if (a <= 0.0) return 0.0;
    vec3 l = lv / max(dist, 1e-4);
    if (lights[li].proj_origin.w >= 0.5)
    {
        vec4 ptc = lights[li].proj_mat * vec4(P, 1.0);
        if (ptc.z < 0.0) return 0.0;
        ptc.xyz /= ptc.w;
        if (ptc.z <= 0.0 || ptc.x <= 0.0 || ptc.x >= 1.0 || ptc.y <= 0.0 || ptc.y >= 1.0) return 0.0;
        l = normalize(lights[li].proj_origin.xyz - P);
    }
    // (0 here only for a black light, whose walk terms are all 0 too: colour x ...)
    return regirLum(cf.rgb) * a * (0.2 + 0.8 * clamp(dot(N, l), 0.0, 1.0));
}

// ---- ReSTIR DI LIGHT SET (population 1) ----
// THE ONE SET, shared by the build (which lights are candidates) and the lit pass (which lights take
// the denoised ratio: deferRsStoch): the host's mask rs_set (LLCleanRendererVK::regirHeaderFill,
// from THIS frame's records -- the build runs before the record upload): the ray-traced projector
// records, + with RS_F_EXT every other projector and every point light (attached ones only with
// RS_F_ATTACHED). Point lights carry (llappviewer buildVulkanWorldFrame) proj_axis.x = their prim's
// bounding radius (the fixture clip), proj_axis.y = 1 on an attachment.
bool rsInSet(uint li) { return li < 256u && (rg.rs_set[li >> 5u] & (1u << (li & 31u))) != 0u; }
// (the moving-light rule: RS_MOVE_ON above)
uint rsMoveRays() { return (rg.rs_cfg.w >> 4u) & 15u; }
// (RS_MOVE_LIGHTS, bits 8-11) the candidate lights a moved texel evaluates densely (0 = off: the pick)
uint rsMoveLights() { return (rg.rs_cfg.w >> 8u) & 15u; }
bool rsMoveOn() { return (rg.rs_cfg.w & RS_MOVE_ON) != 0u; }
// (RS_F_STRIDE) Position i (< n) of a window of n among N slots (n <= N) starting at slot `start`
// (< N), cyclic: evenly spaced (start + floor(i N / n)) with the flag, else contiguous (start + i).
// Either pattern is n DISTINCT slots, and with `start` uniform over the N slots every slot is in
// the window with probability n / N -- the window's scale (N / n) and the RIS weights are the same
// for both: unbiased either way. The spaced one covers the whole stratified CDF (each light about
// in proportion to its target), the contiguous one only a stretch of it.
uint rsWinSlot(uint start, uint i, uint n, uint N)
{
    uint j = start + ((rg.rs_cfg.z & RS_F_STRIDE) != 0u ? (i * N) / max(n, 1u) : i);
    if (j >= N) j -= N;
    return j;
}
bool rsMoved(uint li) { return li < 256u && (rg.rs_moved[li >> 5u] & (1u << (li & 31u))) != 0u; }
// (#869) THE FLICKERING-LIGHT RULE: a light whose radiance changed this frame (rs_flick). Its shadow
// did not move, but the ratio pass 15 hands the denoiser is ONE visibility for the texel's whole
// light set, weighted by the lights' targets: sum w_i v_i / sum w_i. When one light's share of
// that mix changes every frame (a fire flickering beside a lamp, two fires flickering apart), the
// accumulated ratio is the OLD mix, and the lit pass multiplies the CURRENT lights by it -- a
// flaring fire leaks through its own shadow at the other lights' old share (bright flickering
// streaks on a self-shadowed limb). Such texels take the moving-light history rules (pass 15's
// MOT w share, the dense evaluation), in proportion to how much of the mix can shift.
bool rsFlick(uint li) { return li < 256u && (rg.rs_flick[li >> 5u] & (1u << (li & 31u))) != 0u; }
#ifdef RS_SHADOW_PARAMS
// A projector with a ray-traced shadow record (projStochEligible): always pass 15's, in or out of
// the grid (the cluster walk covers it outside). The includer defines RS_SHADOW_PARAMS(i).
bool rsRecordLight(uint li)
{
    const uint rec = lights[li].tex.z;
    if (rec == 0u) return false;
    const vec4 sp = RS_SHADOW_PARAMS(rec - 1u);
    return !(sp.w < 0.5) && sp.z > 1.5;
}
#endif
uint rsSlots() { return clamp(rg.rs_cfg.x, 1u, REGIR_RES_MAX); }

// ---- (#91) EMISSIVE LIGHTS (RenderVKReSTIREmissive 1): emissive TRIANGLES as ReSTIR DI area lights ----
// Built every ReSTIR frame by clean_deferred_light.comp CLEAN_RS_EMISSIVE_BUILD (ten dispatches; the
// emitter list, the blocks and the cell lists from scratch, an UNCHANGED emitter's triangle records copied from
// the last build -- RECORD REUSE below; nothing is kept for a left object; #1006 only the CHANGED cells
// re-gathered -- INCREMENTAL CELLS below) into fixed regions after the two populations:
// EMITTERS (mode 0, REGIR_EM_INST_CAP entries x REGIR_EM_ENT_WORDS uvec2 per PARITY -- the record parity
// rs_em.w bit 0; the other parity is the last build's, RECORD REUSE reads it; count rs_em_cfg.w): an instance
// in this frame's TLAS (static legacy BLAS: no rigged / worn / attached / terrain / water row; #1009 + a
// static traced on its CLUSTER cut, through its legacy row -- CLUSTER STATICS below) whose material emits --
// PBR: emissive factor x its texture's average (the smallest mip) x
// rs_em_params.z; legacy: FULLBRIGHT + GLOW only, colour x tint x texture x glow x rs_em_params.y
// (fullbright alone and glow alone never emit) -- above rs_em_params.x:
//   [0] instance, triangles      [1] first record slot, records (mode 1)   [2] stride s, offset (mode 1)
//   [3] radiance factor r, g     [4] b, texture (EM_NO_TEX: none)           [5] range (m), EM_R_* flags
//   [6] the average radiance luminance (factor x the whole texture's mean), takeover light + 1 (0: none)
//   [7] that average radiance r, g   [8] b, its power weight A c (LIGHT TAKEOVER below)
//   [9] the takeover gain G r, g     [10] b, the light's falloff (mode 1; < 0: no takeover this frame)
//   [11] the CONTENT SIGNATURE (64 bits; RECORD REUSE)   [12] the last build's first record slot of this
//        emitter's block (mode 1; REGIR_EM_NONE: build its records), EM_E_* (mode 0)
// Its RANGE (one for all its triangles: a large emitter's light is the sum of many small triangles, each
// too faint alone to reach far) ends where the reflected radiance of a white surface facing the whole
// emitter falls to rs_em_params2.x (lum x area / (pi d^2), the area from 32 strided triangles), capped at
// rs_em_params2.y; an emitter whose range misses the grid is not listed. Past REGIR_EM_INST_CAP: dropped.
// TRIANGLE RECORDS (mode 1 lays the emitters' blocks out in entry order by a prefix sum, mode 2 writes one
// record per invocation; two PARITIES of REGIR_EM_CAP x REGIR_EM_WORDS uvec2, 112 B; this build's parity
// in rs_em.w bit 0, the other one is the previous build's -- last frame's reservoir keys resolve through
// it; rs_em.x = the records written, the blocks tile [0, rs_em.x)):
//   [0] v0 x, y   [1] v0 z, e1 x   [2] e1 y, z   [3] e2 x, y   [4] e2 z, SELECTION luminance (x s)
//   [5] radiance factor r, g (x s)   [6] b, range   [7..9] the emissive map's UV at v0, v1, v2
//   [10] texture, EM_R_* flags       [11] instance, triangle   [12] average radiance r, g (x s)
//   [13] b, the takeover light's falloff (EM_R_TAKE; LIGHT TAKEOVER below)
// World-space vertices (v1 = v0 + e1, v2 = v0 + e2; the lit kernels have no RT geometry to read) and the
// UVs already in the sampled map's space (PBR: the emissive map's own transform; legacy: the base face
// UV; a texture-animated face at this frame's draw time, as the raster). An emitter's triangles are one contiguous block: triangle j of the block is mesh triangle
// offset + j s. s = 1 while all emitters' triangles fit rs_em.z (RenderVKReSTIREmissiveTriangles, <=
// REGIR_EM_CAP); else every emitter keeps every s-th (the same s for all, a FIXED offset per emitter: the
// lit kernels' unshadowed sum is not denoised, so the subset must not change from frame to frame) and
// each kept triangle carries s x its radiance (power-preserving). A block that still does not fit is
// dropped whole with every later one (counted in rs_em.y with the skipped triangles).
// SUBSET STRIDE (mode 1): a step of s re-subsets EVERY emitter at once -- every record rebuilt, every
// reservoir whose triangle left the subset rejected (emRemap), every kept cell forced (no forward entry):
// the DI shadows restart scene-wide and each emitter's light moves to its other triangles. A total near
// a stride boundary (emitters entering / leaving the TLAS or the grid) steps s back and forth (the
// periodic emissive pulse reported for 1.5.0 Beta 4). So: (1) the LIGHT TAKEOVER rows (below) keep stride
// 1 -- laid out first, their triangles leave the cap before the rest's s is chosen (while they take at
// most half of it); a subset of a lamp's few glass triangles moved its point light's power to one part of
// the glass (the lit corner swapped), and with offset >= its triangle count took none (not taken: the
// point light came back, the room brightened); (2) the rest's s has HYSTERESIS: up at once (it must fit),
// down only when the triangles fit the lower stride with a quarter to spare (CC_HDR [EM_CCH_STRIDE]: the
// last build's s and stamp). rs_em_take_cfg.w: this build's s (the host logs its steps). The AVERAGE
// radiance [12..13] is the triangle's own this frame (its texture at the mip where the triangle is about
// one texel). The SELECTION luminance [4] (cell targets, RIS targets, the light / no-light test) is that
// average's on a static face -- a triangle on a black part of the texture is no light (range 0: never a
// candidate) while its neighbours are -- but the WHOLE texture's on a TEXTURE-ANIMATED face (flag 1024):
// a scrolling texture slides over the triangles every frame, and a selection that followed it would
// reshuffle the cells' strongest triangles every frame (flicker in the undenoised sum).
// RECORD REUSE (#986; EM_F_REUSE: the host has recorded a build before, so the other parity holds the
// last build's entries, hash and records): almost every emitter is static, and its records -- vertex
// fetch, transform, the map's UVs, a texture sample per triangle -- come out the same every frame. Mode 0
// hashes everything an emitter's records are made from into its CONTENT SIGNATURE [11]: the instance's
// model matrix, UV matrix, flags, tint, material id, geometry row (first index, base vertex, count), the
// emissive map's own UV transform, the emitter's factor / texture / average radiance (the smallest mip:
// a texture that streams in changes it) / range / flags, the emission threshold, and the vertex
// positions of the 32 sampled triangles of the range estimate (a geometry slot reused in place). Mode 1
// finds the instance in the LAST build's hash: same signature, triangle count and subset (s, offset)
// there -> [12].x = the last build's first slot of the block; mode 2 then COPIES each record from that
// slot of the other parity (after checking its (instance, triangle) word) instead of building it. Always
// built: a TEXTURE-ANIMATED face (its UVs and colour follow this frame's draw time), a LIGHT TAKEOVER row
// (its gain follows the light's colour every frame) -- EM_E_DYN -- and a ROLLING 1/EM_REFRESH of the rest
// per build (a texture whose mean did not change but whose detail did, behind the same bindless index:
// no signature sees it; every record is rebuilt at least every EM_REFRESH builds). An instance freed and
// its id reused is a different signature (or the very same content: the same records). A changed subset
// stride s (the emitters' triangles crossed the cap) changes every emitter's (s, offset): all rebuilt.
// Fixed regions only: the second parity of the entries and the hash; nothing grows.
// GROUPS (mode 2; REGIR_EM_CAP / 64 x 3 uvec2): the box of each 64 consecutive records' light (the union
// of their reach spheres: centroid, bounding radius + range; empty: lo > hi) -- the cell build's first
// cull on its GATHER path (below), so a workgroup walks only the triangles near it however large the emitter.
// HASH (REGIR_EM_HASH_N uvec2 per parity: x = instance + 1 (0 empty), y = its emitter entry; this build's
// cleared by the host before the build, load <= 0.5; only emitters whose block was written): the GI split
// and the reservoirs' key remap (emRemap: last build's record -> (instance, triangle) -> this build's
// slot); the last build's: RECORD REUSE.
// CELL LISTS (#986; modes 3-6, after the records): the candidate triangles of every cell, by a counting
// sort (REGIR_EM_LCELL_BASE: per cell x = count (bits 0-15, <= REGIR_EM_CAP) | fill cursor << 16, y = its
// first pair within its 64-cell block; REGIR_EM_LBLK_BASE: per block x = its pairs, y = its first pair;
// REGIR_EM_PAIR_BASE: the pairs' record slots, two per uvec2) -- mode 3 COUNT (one workgroup per record, a
// lane per (level, z slice): at most 32 x 32 cells each) walks the cells of the record's reach box (its
// bounding sphere + range + the cell pad) and counts each cell whose emCellTarget > 0 (the candidate test
// itself; the host zeroes the counts); mode 4 SCAN (a workgroup per 64-cell block) and mode 5 (one
// workgroup: the 1024 blocks' sums; rs_em_cache.z = the pairs, past REGIR_EM_PAIR_CAP: EM_C_GATHER); mode 6
// FILL runs mode 3's walk again and writes each pair's record slot at the cell's next place. Work ~ the
// pairs (the candidates), not cells x the triangles near their workgroup. A cell's pairs are in atomic
// order: nothing below depends on it (the EXACT strongest are ranked by (target, instance, triangle)).
// LONG CELLS (#986, EM_F_LONG: RenderVKReSTIREmissiveLongCells): one lane walking its cell's list twice is
// fine for the usual handful of candidates, but the cells near a big emitter list thousands (every kept
// triangle of a road whose range reaches the cell) and that ONE lane's serial walk -- two dependent loads
// per pair, twice -- set the whole dispatch's time (1.8 ms for 163k pairs). Mode 4 (#1006: mode 11, of the
// cells re-gathered this build) lists every cell with more than EM_LONG_LIST candidates
// (REGIR_EM_LONG_BASE); mode 7 leaves them and mode 9 gives each one a
// WORKGROUP: lane l takes pairs l, l + 64, ... (walk 1: its own M, W and R strongest, merged by lane 0
// under the same EXACT TOP ranking; walk 2: the rest's aggregate summed over the lanes in lane order, the
// stratified samples assigned in LANE-MAJOR order -- lane l owns the sample positions in [its offset, +
// its rest target) -- the same estimator as the lane walk's order, which was the list's atomic order).
// The same cell words, the same meaning. (#1108: on a pairs-past-the-cap GATHER build mode 9 builds every
// cell from the record groups instead of a list -- GATHER below.)
// INCREMENTAL CELLS (#1006, RenderVKReSTIREmissiveIncremental): population 2 is NOT rebuilt from scratch
// every frame any more. The cell lists (modes 3-6) still are -- they are the cheap part, and they are how
// a cell's change is SEEN -- but a cell is re-gathered (mode 7 / 9) only when its candidate set changed,
// and the rest keep last frame's result. Per cell (CELL CACHE, REGIR_EM_CC_BASE: [0] the membership
// signature it was gathered with, [1] x = dirty since (build stamp + 1; 0 clean) | EM_CC_FORCE, y = the
// stamp of its last gather):
//   THE SIGNATURE (REGIR_EM_SIG_BASE, zeroed by the host per build; mode 3 sums it): over the cell's list
//   pairs, two independent 32-bit sums of the PAIR KEY hash(instance, mesh triangle, the emitter's GEOMETRY
//   KEY -- model matrix, geometry row, the 32 sampled vertex positions: mode 0) -- order-independent, so the
//   list's atomic order never shows. It changes exactly when a triangle arrives in / leaves the cell's list
//   (an emitter moved, appeared, was removed, changed its triangle set / LOD / subset / bounds) or a listed
//   emitter's geometry changed in place. It does NOT see colour or brightness: an exact cell stores only
//   record references (weight 1), and the records are rebuilt every frame with this frame's radiance
//   (an animated "rainbow road", a glow change, a flicker -- all current at once, no re-gather).
//   REACH RANGE (hysteresis, so a brightness change does not move the lists' edge): the lists' candidate
//   test (modes 3 / 6) uses the emitter's REACH range Rg (REGIR_EM_RECX_BASE.x per record, from the entry's
//   REGIR_EM_ENTX_BASE.x: mode 1) instead of its light range: Rg is carried from the last build while the
//   geometry key is unchanged and range <= Rg <= 2 x range, else it restarts at the range. A flicker or a
//   colour cycle settles at its brightest range after its first peak (the lists hold a superset; mode 7's
//   targets use the TRUE range -- a listed triangle out of its true range is no candidate), and only a new
//   peak or a dimming past half re-draws the edge (as a set change: the cells there go dirty).
//   DIRTY (mode 4, per cell): signature != the gathered one -> dirty since this build (kept while it stays
//   different; back to the gathered set -> clean again). A dirty cell WITH A STRATIFIED REST is FORCED (its
//   rest aggregate holds the changed triangles' light as a value, not a reference -- it is re-gathered this
//   frame); one without a rest waits for the budget: oldest first (mode 4's age histogram, mode 5's
//   threshold age and quota, mode 11's pick), at most RenderVKReSTIREmissiveCellBudget a frame.
//   SELECT (mode 11, per cell): a FULL frame (EM_F_FULL from the host: first build, a new buffer, emissive
//   toggled, settings / grid levels / cell size changed, a teleport-sized origin jump, RenderVKReSTIR-
//   EmissiveIncremental 0; or the GPU's own VALID word cleared by a gather build) selects every cell. Else:
//   the forced cells, the budget's dirty cells, a ROLLING REFRESH (every cell with content once per
//   EM_CELL_REFRESH builds, staggered: bounds any lag the signature cannot see -- the reach range's tail),
//   and the cells with a stratified rest every RenderVKReSTIREmissiveRestRefresh builds (1, the default:
//   every build -- their rest samples are a per-frame stratified draw and their aggregate colour follows
//   animated textures, exactly as the full rebuild). A selected cell goes to the BUILD LIST
//   (REGIR_EM_BLD_BASE; mode 7 runs over it through an indirect dispatch) or, past EM_LONG_LIST, to the
//   long list (mode 9). A KEPT cell's references are carried to this build's record slots: THE FORWARD MAP
//   (REGIR_EM_FWD_BASE, per last-build slot: this build's slot of the same (instance, triangle), or
//   REGIR_EM_NONE; the host fills it with NONE, mode 2 writes it -- a copied record knows its source slot, a
//   rebuilt one finds it through the last hash, both checked against the last record's (instance,
//   triangle) word). A slot whose triangle is gone (emitter removed, dropped past the cap, out of a changed
//   subset) has no forward entry: the cell is FORCED instead -- a stale reference into a reused slot never
//   survives a build. After mode 11 every cell names this build's slots only (the same invariant the full
//   rebuild had). A kept exact cell keeps its candidate list and its weights (1) and its target sum W (only
//   RS_F_WCUT reads it: the host makes those frames FULL).
//   ORIGIN SHIFT (EM_F_SHIFT, mode 12): a level's origin follows the camera in whole cells, so a cell index
//   names another world box after a step. The host copies the cell words and the cache to the SCRATCH
//   region (REGIR_EM_SCR_BASE) and mode 12 moves every cell by its level's shift (the last build's origins:
//   REGIR_EM_CC_HDR) -- the words are world-space (record slots, weights, the aggregate at the cell centre),
//   so a moved cell is valid as it is; the cells new to the clipmap start empty and FORCED.
// CLUSTER STATICS (#1009, EM_F_CLUSTER: RenderVKReSTIREmissiveClusterStatic): with the cluster LOD path on,
// a static the cluster raster draws is traced on its cluster RT cut, and its rt_geom row names that cut
// (RT_GEOM_CLUSTER: no triangle count, no legacy slot) -- mode 0 skipped it, so the emissive meshes the
// cluster RT admits (its important statics: the big ones of a venue) dropped out of the emissive light
// with the cluster path on, and its A/B against the cluster path off showed it. Statics keep their legacy
// geometry under the cluster path, so clean_rt_instances.comp records the legacy slot the static LOD rule
// picks beside the cluster row (rt_geom_vc words 1..3; clean_rt_common.glsl rtGeomEmRow) -- the very level
// the cluster-off TLAS traces -- and modes 0 / 2 list the emitter's triangles from that row exactly as a
// legacy static's: the same records, radiance, area, range, RECORD REUSE and GEOMETRY KEY (the legacy
// row's, not the RT cut's: a cut change does not move the records; a LOD step does, as with the cluster
// path off). The emitter's shadow rays meet its RT cut, within the cut's error of the legacy triangles
// (the shadow sample's near clip covers the usual error). Rigged faces (legacy or cluster) and worn
// attachments stay out in either mode. The emitters / triangles taken this way: CC_HDR [41] (stats).
// WEAK OVERLAY (#1006, RenderVKReSTIREmissiveWeakPower): an emitter whose POWER (its average radiance
// luminance x its area: what its range is made of) is below the threshold is WEAK (EM_R_WEAK on its entry
// and records; never a takeover row): it lights at most a metre or two, but in the 1 m cells of level 0 its
// reach box still lists it in tens of cells. It skips level 0 and populates the OVERLAY instead: a grid of
// REGIR_EM_OV_X x _Y x _Z cells of TWICE level 0's edge on level 1's lattice (origin 2e floor(o0 / 2e),
// covering level 0's region whatever its parity), cell ids REGIR_EM_NCELLS + i -- the same cell words, list
// words and cache as any cell (emCellWord / emLCellWord / emLBlkWord / emCellBoxAny take both). Levels 1-3
// list weak and strong emitters as before. A point of level 0 finds the strong emitters in its level-0 cell
// and the weak ones in its overlay cell: pass 15 takes both cells' slots as candidates (the candidate set at
// P is still every emitter that can light P), the lit kernels blend the overlay's 8 cells around P like the
// level's (and drop weak triangles from a coarser neighbour of the level blend: the overlay carries them --
// a coarser neighbour's REST aggregate cannot be split, the one place a weak emitter's light may count
// twice, in the band at level 0's edge). Only with the cell lists (EM_F_WEAK; a gather build lists every
// emitter at level 0 and the overlay is not read), and the consumers read the overlay only while some
// overlay cell has a candidate this build (EM_C_OVL).
// GATHER (EM_C_GATHER: the pairs passed the cap, or RenderVKReSTIREmissiveCellLists 0 -- EM_F_GATHER): the
// cell build walks the record groups near its workgroup instead (the pre-#986 build; ~ cells x triangles).
// (#1108) A pairs-past-the-cap gather (the long cells on) builds every cell in mode 9 instead: a workgroup per
// cell, step k = record group k (skipped, uniformly, unless its light box reaches the cell), lane l its record
// 64 k + l -- the same candidates and EXACT TOP as mode 7's walk, but a cell's candidates are split over 64
// lanes and the cells over every workgroup. Mode 7's lane-per-cell walk put each candidate of a crowded
// workgroup box on ONE lane serially: a camera high over a large all-emissive build (thousands of big
// triangles whose reach -- bounding radius + range -- covers the fine levels' cells) ran ~7 ms in a few
// workgroups at 4 % occupancy. The overflow itself: such a triangle lists in ~every cell of levels 0-1
// (16384 each), so a few dozen of them pass REGIR_EM_PAIR_CAP.
// POPULATION 2 (mode 7, after the lists): per grid cell (the same clipmap) REGIR_EM_CELL_WORDS uvec2:
// [0] x = slots filled | the exact strongest K_t << 16 | REGIR_CELL_EXACT, y = float bits of the cell
// target sum W; [1 .. n] x = record slot, y = float bits of its weight 1 / q; [SLOTS_MAX + 1 .. + 3] the
// REST'S AGGREGATE (V xyz, C rgb; below). Candidates: every triangle whose range (from its
// bounding sphere) reaches the cell box and, one-sided, whose front half-space does (CONSERVATIVE: a
// superset of the triangles that light any point of the cell; the cell's list, or on the GATHER path the
// workgroup culls the record groups by their box first, then their triangles by its 64-cell box). Cell
// target = average luminance x the small-source term at the cell centre (floored by a quarter of it at the
// closest point) x the range window.
// M <= R_e: the whole list (exact, weights 1). Else the K_t = R_e - R_s triangles with the largest cell
// targets exactly (weight 1, the near field; EXACT TOP: equal targets ranked by (instance, triangle), so
// which triangles are exact never depends on the list's atomic order or on the slots, which follow mode
// 0's atomic entry order -- the exact / aggregate split feeds the undenoised term and must not change
// between two frames of the same scene) + R_s = ceil(R_e / 4) stratified samples of the REST (q =
// p / W_rest x R_s; per frame) -- those samples are PASS 15's only (they spread its visibility rays over
// the far field). The lit kernels light the rest with its DETERMINISTIC AGGREGATE instead (emCellRest):
// V = the sum of the rest triangles' vector form factors at the cell centre (emTriVecFF), C = their
// average colour weighted by those, evaluated as one horizon-clipped sphere-equivalent source. The
// unshadowed light a ratio estimator multiplies by the denoised ratio is never filtered itself: a
// per-frame sample there flickers cell by cell (the owner's rainbow-road video: the avatar's tint
// jumping, a hard cell edge across the shin), an aggregate does not.
// THE LIGHT (emTriFF): a Lambertian emitting triangle. Its unshadowed diffuse reflection off (P, N) is
// diffuseColor x L(P) x FF, FF the exact polygon form factor (Lambert / Arvo contour integral) of the
// triangle CLIPPED TO P's HORIZON (dot(N, x - P) > 0; up to 4 vertices), x the range window (from the
// triangle's bounding sphere). 0 behind a one-sided triangle (the raster's front face: the transformed
// triangle's counter-clockwise normal; instance flag 256 = double-sided emits both ways) -- and 0 for a
// point on the triangle's own plane: no inside / pad rule. Far away (64 x its size) the point form. L(P)
// = factor x the emissive texture at the triangle point along P's vector irradiance direction, at the
// mip of the footprint P's height over the plane covers (clean_deferred_light.comp emTriRadiance; the
// textured area light's prefiltered lookup): a receiver over a rainbow stripe takes that stripe's
// colour, a far one the filtered mean. Diffuse only: an emitter's specular image is the reflections'
// job (RT reflections / probes see the emissive surface itself).
// DI (pass 15, the generic core): the cell's triangles join the light candidates -- key REGIR_EM_KEY +
// slot in the light-index space (rs_res stores it; next frame rsTap maps it through emRemap), target
// average luminance x FF, weight p x (1 / q) -- ONE RIS stream with the lights. An emitter pick traces
// ONE ray to a point of the horizon-clipped triangle sampled uniformly in SOLID ANGLE (Arvo 1995; the
// area form for tiny ones) -- the integrand of FF up to P's cosine; the per-frame point is the soft
// shadow the denoiser integrates. Its dt = diffuseColor x L(P) x FF, st = 0: exactly the lit kernels'
// term, so the ratio pass 15 hands the denoiser is pure visibility (a texel colour in dt alone would
// cancel in a one-pick ratio, or push it past 1 and be clamped). The lit kernels (BASE / combine:
// deferEmitterLight) add the UNSHADOWED light -- each cell's exact triangles + its rest's aggregate,
// BLENDED TRILINEARLY over the 8 cells around P (one cell alone steps at every cell face: the term is not
// denoised) -- x the denoised diffuse ratio. The candidate set is the lighting set up to that blend:
// pass 15 samples P's own cell, whose candidates are the neighbours' too near P (conservative reach);
// pass 15's den is an unbiased estimate of the same light, the ratio the visibility either way.
// GI SPLIT (EM_F_GI_SPLIT, with EM_F_LIT): a GI ray hitting an instance in the hash drops its emission
// (PBR emissive, fullbright colour): DI carries that emitter's direct light. An emitter the caps dropped
// is not in the hash and keeps its GI emission -- no light lost, none counted twice.
// LIGHT TAKEOVER (EM_F_TAKE, RenderVKReSTIREmissiveLightTakeover): an object (one prim: the SL light's own
// LLVOVolume) with an SL POINT light and emitting faces -- a lamppost's lantern glass -- lights the world
// from its glass at the light's strength, and its point light goes out where the glass's light comes in.
//   TABLE (host, every frame: LLCleanRendererVK::rsEmTakeTable from buildVulkanWorldFrame's light_takeover;
//   REGIR_EM_TAKE_BASE): [li] (li < 256) = (first row, rows) of point light li, then up to REGIR_EM_TAKE_N
//   rows (instance, row | li << 16) sorted by instance -- the prim's faces whose material may emit (not
//   attached, not a projector); the rows of one light are contiguous, in light order. Row r IS emitter entry r: the host clears entries
//   [0, T) and starts the entry counter at T, so a takeover face never competes for an entry and its
//   block is laid out FIRST by mode 1, every triangle (stride 1: SUBSET STRIDE above; it fits unless the
//   takeover blocks alone pass half the triangle cap -- then they stride with the rest).
//   Mode 0: a qualifying instance (the same threshold as any emitter) found in the table whose light is a
//   point light wholly inside the grid's outermost level (every receiver it reaches has a cell: the lit
//   kernels add emitter light only inside the grid) writes its own row, takeover light li + 1 in [6].y and
//   its power weight A c ([8].y: the face's area x 1/4 one-sided, 1/2 two-sided -- the average over all
//   directions of its projected area). Else it is an ordinary emitter (an atomic entry past T).
//   Mode 1 (after the blocks): per light, over its rows -- TAKEN when every written row's block was laid
//   out whole; then G = C K / max(Cw, floor) (rgb), Cw the rows' power-weighted average radiance, K =
//   3.25 x 0.96 x atten(1/3) x (R/3)^2 / sum A c (C the light's colour, R its radius, atten
//   regirAtten with its falloff): the irradiance sum L' A c / d^2 of the scaled triangles at d = R/3
//   equals the point light's PBR term there (3.25 atten C x the receiver's (1 - F) = 0.96; a legacy
//   receiver's point light, C atten x base, is pi / 3.12 of that: within 1 %) -- per channel, so the
//   texture's variation stays and its average takes the light's colour (a channel the glass lacks stays
//   dark: the floor only bounds the gain). The light's bit in rs_em_take. Not taken (a block dropped, no
//   written row): every row stays an ordinary emitter and the point light stays -- the light never goes
//   out (rs_em_take_cfg.z counts them).
//   Mode 2: a taken row's records carry G x their radiance (factor, average, selection luminance -- the
//   cell targets, RIS targets, dt and the lit kernels' term all read the scaled record: the ratio stays
//   pure visibility), EM_R_TAKE, range = R and the falloff in [13].y; the TAKEOVER WINDOW (emTakeWin)
//   replaces emWindow: the point form's 1 / d^2 x the window = atten(d / R) / atten(1/3) -- the SL light's
//   own falloff to its radius, 1 at R/3 -- with the near field (d < R / 9) left physical.
//   THE POINT LIGHT GOES OUT where the glass lights instead -- the main view's deferred lighting of
//   opaque receivers: pass 15 (pdnTargetGoboL's target 0: no candidate, no reused reservoir, no dense
//   evaluation) and the lit kernels' walk (localLight via CLEAN_LIGHT_TAKEN), under the SAME gate as
//   deferEmitterLight (DEFER_FLAG_PSTOCH, a ReSTIR frame, emLitOn) -- one decision, made on the GPU in this
//   frame, before every reader of it. Everything that the emitters do NOT light keeps the point light:
//   forward / alpha-blend surfaces, probe and mirror captures, the volumetric fog, and the RT reflection /
//   GI hits (their lights are the SOURCE of the reflected / bounced light, never the direct light the
//   glass now carries -- and GI's split drops the glass's own emission at its hits). Nothing is lit twice.
#define REGIR_EM_CAP 16384u         // triangle records per parity (rs_em.z: this frame's cap, <= this)
#define REGIR_EM_WORDS 14u
#define REGIR_EM_HASH_N 8192u
#define REGIR_EM_INST_CAP 4096u     // emitter entries per build
#define REGIR_EM_ENT_WORDS 13u
#define REGIR_EM_SLOTS_MAX 8u
#define REGIR_EM_BASE (2u * REGIR_RS_BASE)
#define REGIR_EM_HASH_BASE (REGIR_EM_BASE + 2u * REGIR_EM_CAP * REGIR_EM_WORDS)   // (two parities)
#define REGIR_EM_ENT_BASE (REGIR_EM_HASH_BASE + 2u * REGIR_EM_HASH_N)             // (two parities)
#define REGIR_EM_GRP_BASE (REGIR_EM_ENT_BASE + 2u * REGIR_EM_INST_CAP * REGIR_EM_ENT_WORDS)
#define REGIR_EM_CELL_BASE (REGIR_EM_GRP_BASE + (REGIR_EM_CAP / 64u) * 3u)
#define REGIR_EM_CELL_WORDS (REGIR_EM_SLOTS_MAX + 4u)   // header, slots, the rest's aggregate (3)
// (LIGHT TAKEOVER) the host's table after population 2: 256 per-light (first row, rows), then the rows
#define REGIR_EM_TAKE_BASE (REGIR_EM_CELL_BASE + REGIR_LEVELS_MAX * REGIR_CELLS_PER_LEVEL * REGIR_EM_CELL_WORDS)
#define REGIR_EM_TAKE_N 1024u       // table rows (= emitter entries reserved for takeover faces)
// (#986 CELL LISTS) after the takeover table: per cell (count | cursor << 16, first pair in its block),
// per 64-cell block (pairs, first pair), the pairs (two record slots per uvec2)
#define REGIR_EM_PAIR_CAP 1048576u   // cell candidate pairs per build (past it: EM_C_GATHER)
#define REGIR_EM_NCELLS (REGIR_LEVELS_MAX * REGIR_CELLS_PER_LEVEL)
#define REGIR_EM_LCELL_BASE (REGIR_EM_TAKE_BASE + 256u + REGIR_EM_TAKE_N)
#define REGIR_EM_LBLK_BASE (REGIR_EM_LCELL_BASE + REGIR_EM_NCELLS)
#define REGIR_EM_PAIR_BASE (REGIR_EM_LBLK_BASE + REGIR_EM_NCELLS / 64u)
// (#869 OCCLUSION CULL) after the cell lists' pairs: the occlusion cache -- OCC_STAT_U2 (8) uvec2 of stats,
// then OCC_CELLS (32768) patches x OCC_CELL_U2 (25) uvec2 (clean_deferred_light.comp OCCLUSION CULL;
// LLCleanRendererVK kRegirOccOffset, 6.25 MiB, fixed)
#define REGIR_OCC_BASE (REGIR_EM_PAIR_BASE + REGIR_EM_PAIR_CAP / 2u)
// (#986 LONG CELLS) after the occlusion cache (8 + 32768 x 25 uvec2): [0] x = the long
// cells (#1006: mode 11, the re-gathered ones; the host zeroes it), y = the longest list (stats); then the
// long cells' indices, two per uvec2
#define REGIR_EM_LONG_BASE (REGIR_OCC_BASE + 8u + 32768u * 25u)
#define EM_LONG_LIST 32u            // a cell with more candidates than this is a LONG CELL (mode 9)
// (#1006 WEAK OVERLAY) 17 x 17 x 9 cells of 2 x level 0's edge on level 1's lattice (covers level 0's 32 x 32
// x 16 region at either parity of its origin); ids REGIR_EM_NCELLS + i, padded to whole 64-cell blocks
#define REGIR_EM_OV_X 17u
#define REGIR_EM_OV_Y 17u
#define REGIR_EM_OV_Z 9u
#define REGIR_EM_OV_N (REGIR_EM_OV_X * REGIR_EM_OV_Y * REGIR_EM_OV_Z)   // (2601)
#define REGIR_EM_OV_CELLS 2624u     // (41 blocks)
#define REGIR_EM_NCELLS_ALL (REGIR_EM_NCELLS + REGIR_EM_OV_CELLS)   // (68160 = 1065 blocks)
#define REGIR_EM_NBLK_ALL (REGIR_EM_NCELLS_ALL / 64u)
// (#1006 INCREMENTAL CELLS) after the long cells' list (LLCleanRendererVK kRsEmXOffset = the old end):
//   CC_HDR (64 uvec2): [0] x = the build stamp (mode 1 adds 1), y = VALID (mode 11: every cell holds a build
//          at the stored origins; a gather build clears it); [1 + 2l], [2 + 2l] level l's origin of that build
//          (xy, zw floats); [9] EM_CCH_STRIDE (mode 1, every build, with the lists or not): the SUBSET STRIDE's
//          hysteresis; per build, zeroed by the host from EM_CCH_FRAME on: [16] x = the build list's cells,
//          y = the quota counter; [17] x = the threshold age bin (EM_NONE: every dirty cell), y = its quota;
//          [18].xy, [19].x the INDIRECT dispatch of mode 7; [20..23] STATS (EM_CCH_STAT); [24..39] the age
//          histogram (32 bins); [40] x = FULL this build (mode 5's verdict for mode 11); [41] (#1009, mode 0,
//          with the lists) x = CLUSTER STATICS' emitters, y = their triangles (stats); [42..44] (#1009, mode 0,
//          EM_F_SKIPSTAT) the SKIP DIAGNOSTIC's counters (clean_deferred_light.comp emSkipNote); [45] (mode 0,
//          with the lists) EM_CCH_OUT: the emitters left out for being outside the grid's reach (stats)
//   FWD (REGIR_EM_CAP / 2 uvec2): last build's slot -> this build's (NONE: gone)
//   RECX (2 parities x REGIR_EM_CAP uvec2): per record x = its emitter's reach range Rg, y = its PAIR KEY
//   ENTX (2 parities x REGIR_EM_INST_CAP uvec2): per entry x = Rg, y = the emitter's GEOMETRY KEY
//   SIG (REGIR_EM_NCELLS_ALL uvec2, zeroed per build) then the overlay's LCELL (REGIR_EM_OV_CELLS; zeroed with it)
//   and LBLK (41), the BUILD list (REGIR_EM_NCELLS_ALL x 4 B), the overlay's CELL words, the CELL CACHE
//   (REGIR_EM_NCELLS_ALL x 2 uvec2), then the SCRATCH copies (mode 12's source): population 2's cell words,
//   then the overlay cells + the cache
#define REGIR_EM_X_BASE (REGIR_EM_LONG_BASE + 1u + REGIR_EM_NCELLS / 2u)
#define REGIR_EM_CC_HDR REGIR_EM_X_BASE
#define REGIR_EM_FWD_BASE (REGIR_EM_CC_HDR + 64u)
#define REGIR_EM_RECX_BASE (REGIR_EM_FWD_BASE + REGIR_EM_CAP / 2u)
#define REGIR_EM_ENTX_BASE (REGIR_EM_RECX_BASE + 2u * REGIR_EM_CAP)
#define REGIR_EM_SIG_BASE (REGIR_EM_ENTX_BASE + 2u * REGIR_EM_INST_CAP)
#define REGIR_EM_OV_LCELL_BASE (REGIR_EM_SIG_BASE + REGIR_EM_NCELLS_ALL)
#define REGIR_EM_OV_LBLK_BASE (REGIR_EM_OV_LCELL_BASE + REGIR_EM_OV_CELLS)
#define REGIR_EM_BLD_BASE (REGIR_EM_OV_LBLK_BASE + REGIR_EM_OV_CELLS / 64u)
#define REGIR_EM_OV_CELL_BASE (REGIR_EM_BLD_BASE + REGIR_EM_NCELLS_ALL / 2u)
#define REGIR_EM_CC_BASE (REGIR_EM_OV_CELL_BASE + REGIR_EM_OV_CELLS * REGIR_EM_CELL_WORDS)
#define REGIR_EM_SCR_BASE (REGIR_EM_CC_BASE + 2u * REGIR_EM_NCELLS_ALL)
#define REGIR_EM_SCR2_BASE (REGIR_EM_SCR_BASE + REGIR_EM_NCELLS * REGIR_EM_CELL_WORDS)
#define REGIR_EM_X_END (REGIR_EM_SCR2_BASE + REGIR_EM_OV_CELLS * REGIR_EM_CELL_WORDS + 2u * REGIR_EM_NCELLS_ALL)
#define EM_CCH_STRIDE 9u            // CC_HDR [9] (mode 1): x = the rest's SUBSET STRIDE, y = the stamp of its build
#define EM_CCH_FRAME 16u            // CC_HDR words from here on are per build (the host zeroes them)
#define EM_CCH_STAT 20u             // [20] x dirty cells, y re-gathered; [21] x forced, y the most frames a cell
                                    // stayed dirty; [22] x weak emitters, y forward-map misses (forced);
                                    // [23] x kept cells carried to this build's slots, y FULL builds
#define EM_CC_FORCE 0x80000000u     // CELL CACHE [1].x: re-gathered this build whatever the budget
#define EM_CELL_REFRESH 64u         // the rolling refresh: every cell with content re-gathered once per this many builds
#define REGIR_EM_KEY 256u           // light-index space: 0..255 lights, REGIR_EM_KEY + slot triangles
#define REGIR_EM_NONE 0xFFFFFFFFu
#define EM_NO_TEX 0xFFFFFFFFu
#define EM_F_LIT      1u            // rs_em_cfg.z: the emitters light (pass 15 candidates + the lit kernels)
#define EM_F_GI_SPLIT 2u            // ... and the GI trace drops an emitter's emission at its hits
#define EM_F_TAKE     4u            // ... and the LIGHT TAKEOVER (the table is uploaded; EM_F_LIT only)
#define EM_F_REUSE    8u            // (#986) the other parity holds the last build: RECORD REUSE may copy
#define EM_F_GATHER   16u           // (#986) the cell build gathers (RenderVKReSTIREmissiveCellLists 0)
#define EM_F_LONG     32u           // (#986) LONG CELLS are built by a workgroup each (mode 9)
#define EM_F_FULL     128u          // (#1006) every cell is re-gathered this build (INCREMENTAL CELLS)
#define EM_F_WEAK     256u          // (#1006) the WEAK OVERLAY (rs_em_params.w: the power threshold; with the lists)
#define EM_F_SHIFT    512u          // (#1006) the host copied the cells to the scratch: mode 12 moves them (ORIGIN SHIFT)
#define EM_F_LAST     1024u         // (#1006) the other parity holds the last build (the forward map, the reach range)
#define EM_F_CLUSTER  2048u         // (#1009) CLUSTER STATICS: cluster-traced statics emit through their legacy row
#define EM_CCH_CLUSTER 41u          // (#1009) CC_HDR per-build word: x cluster-static emitters, y their triangles
#define EM_CCH_OUT 45u              // CC_HDR per-build word (mode 0, lists build): x emitters outside the grid's reach, y their triangles
#define EM_F_SKIPSTAT 4096u        // (#1009) the SKIP DIAGNOSTIC: emitting instances mode 0 left out, by why (CC_HDR [42..44])
#define EM_C_GATHER   1u            // rs_em_cache.w: no CELL LISTS this build (the pairs passed the cap)
#define EM_C_OVL      2u            // (#1006) rs_em_cache.w: an overlay cell has a candidate this build (mode 4)
#define EM_E_DYN      1u            // entry [12].y: never reused (texture-animated, a takeover row)
#define EM_REFRESH    16u           // RECORD REUSE: every record rebuilt at least every this many builds
#define EM_R_TWOSIDED 1u            // record / entry flags: emits from both faces (instance flag 256)
#define EM_R_PBR      2u            // entry: the emissive map (else the legacy base colour map)
#define EM_R_TAKE     4u            // record: a taken-over light's triangle (the takeover window, emWin)
#define EM_R_WEAK     8u            // (#1006) entry / record: a WEAK emitter's (the overlay, not level 0)
#define EM_TAKE_NEAR (1.0 / 9.0)    // the takeover window's physical near field (x the light's radius)
#define EM_PI 3.14159265
struct EmTri
{
    vec3  v0, e1, e2;     // world: v1 = v0 + e1, v2 = v0 + e2
    float lum;            // average radiance luminance (x the subset weight)
    vec3  L;              // radiance factor (x the subset weight; x the texture at the UV)
    float range;          // m (0: not a light)
    vec2  uv0, uv1, uv2;  // the sampled map's UVs
    uint  tex;            // bindless texture (EM_NO_TEX: the factor alone)
    uint  flags;          // EM_R_*
    uint  id, prim;       // instance, mesh triangle
    vec3  Lavg;           // this frame's average radiance over the triangle (x the subset weight)
    float tkF;            // EM_R_TAKE: the taken-over light's falloff (the takeover window); else -1
};
// Records ALLOCATED by this build (<= the cap): every slot a cell lists is below it.
uint emCount()  { return min(rg.rs_em.x, rg.rs_em.z); }
uint emParity() { return rg.rs_em.w & 1u; }
uint emEntCount() { return min(rg.rs_em_cfg.w, REGIR_EM_INST_CAP); }
uint emSlots()  { return clamp(rg.rs_em_cfg.x, 1u, REGIR_EM_SLOTS_MAX); }
// The emitters light this frame: the host's flag and records built THIS frame (rs_em.x is reset by the
// header upload and counted up only by the build: a frame without the build reads 0).
bool emLitOn()  { return (rg.rs_em_cfg.z & EM_F_LIT) != 0u && emCount() != 0u; }
uint emRecWord(uint par, uint slot) { return REGIR_EM_BASE + (par * REGIR_EM_CAP + slot) * REGIR_EM_WORDS; }
uint emEntWordP(uint par, uint e) { return REGIR_EM_ENT_BASE + (par * REGIR_EM_INST_CAP + e) * REGIR_EM_ENT_WORDS; }
uint emEntWord(uint e) { return emEntWordP(emParity(), e); }   // (this build's)
uint emGrpWord(uint g) { return REGIR_EM_GRP_BASE + g * 3u; }
// (#1006) population 2's cell words of cell id `cell` -- a clipmap cell (< REGIR_EM_NCELLS) or an overlay cell
uint emCellWord(uint cell)
{
    return cell < REGIR_EM_NCELLS ? REGIR_EM_CELL_BASE + cell * REGIR_EM_CELL_WORDS
                                  : REGIR_EM_OV_CELL_BASE + (cell - REGIR_EM_NCELLS) * REGIR_EM_CELL_WORDS;
}
// (#1006 WEAK OVERLAY) the overlay's min corner (xyz) and cell edge (w = 2 x level 0's): level 1's lattice
// point at or below level 0's origin (level 0's region is inside it at either parity)
vec4 emOvOrigin()
{
    const vec4 o = rg.origin[0];
    const float e2 = 2.0 * o.w;
    return vec4(floor(o.xyz / e2 + 1e-4) * e2, e2);   // (+1e-4: o.xyz is a whole multiple of o.w, float-rounded)
}
// The overlay's cell box (padded like regirCellBox) of overlay index i (< REGIR_EM_OV_N)
void emOvCellBox(uint i, out vec3 lo, out vec3 hi)
{
    const vec4 o = emOvOrigin();
    const uvec3 c = uvec3(i % REGIR_EM_OV_X, (i / REGIR_EM_OV_X) % REGIR_EM_OV_Y, i / (REGIR_EM_OV_X * REGIR_EM_OV_Y));
    const float pad = 1e-3 * o.w + 1e-3;
    lo = o.xyz + vec3(c) * o.w - pad;
    hi = o.xyz + vec3(c + 1u) * o.w + pad;
}
// Any cell id's box: a clipmap cell's (regirCellBox) or an overlay cell's
void emCellBoxAny(uint cell, out vec3 lo, out vec3 hi)
{
    if (cell < REGIR_EM_NCELLS) regirCellBox(cell, lo, hi);
    else emOvCellBox(min(cell - REGIR_EM_NCELLS, REGIR_EM_OV_N - 1u), lo, hi);
}
// The weak overlay is built this frame (the host's flag: with the cell lists)
bool emWeakOn() { return (rg.rs_em_cfg.z & EM_F_WEAK) != 0u; }
// (#1009 CLUSTER STATICS) cluster-traced statics emit through their legacy row this frame
bool emClusterOn() { return (rg.rs_em_cfg.z & EM_F_CLUSTER) != 0u; }
// ... and its cells are READ: the lists were used (no gather build) and some overlay cell has a candidate
bool emOvLive()
{
    return emWeakOn() && (rg.rs_em_cfg.z & EM_F_GATHER) == 0u && (rg.rs_em_cache.w & EM_C_GATHER) == 0u
        && (rg.rs_em_cache.w & EM_C_OVL) != 0u;
}
// The overlay cell id holding P (REGIR_NO_CELL outside the overlay)
uint emOvCell(vec3 P)
{
    const vec4 o = emOvOrigin();
    const vec3 q = (P - o.xyz) / o.w;
    const vec3 dims = vec3(float(REGIR_EM_OV_X), float(REGIR_EM_OV_Y), float(REGIR_EM_OV_Z));
    if (!all(greaterThanEqual(q, vec3(0.0))) || !all(lessThan(q, dims))) return REGIR_NO_CELL;
    const uvec3 c = min(uvec3(q), uvec3(REGIR_EM_OV_X, REGIR_EM_OV_Y, REGIR_EM_OV_Z) - 1u);
    return REGIR_EM_NCELLS + (c.z * REGIR_EM_OV_Y + c.y) * REGIR_EM_OV_X + c.x;
}
EmTri emLoad(uint par, uint slot)
{
    const uint b = emRecWord(par, slot);
    const uvec2 w0 = rg_cells[b], w1 = rg_cells[b + 1u], w2 = rg_cells[b + 2u], w3 = rg_cells[b + 3u];
    const uvec2 w4 = rg_cells[b + 4u], w5 = rg_cells[b + 5u], w6 = rg_cells[b + 6u], w7 = rg_cells[b + 7u];
    const uvec2 w8 = rg_cells[b + 8u], w9 = rg_cells[b + 9u], w10 = rg_cells[b + 10u], w11 = rg_cells[b + 11u];
    const uvec2 w12 = rg_cells[b + 12u], w13 = rg_cells[b + 13u];
    EmTri t;
    t.v0 = uintBitsToFloat(uvec3(w0, w1.x));
    t.e1 = uintBitsToFloat(uvec3(w1.y, w2));
    t.e2 = uintBitsToFloat(uvec3(w3, w4.x));
    t.lum = uintBitsToFloat(w4.y);
    t.L = uintBitsToFloat(uvec3(w5, w6.x));
    t.range = uintBitsToFloat(w6.y);
    t.uv0 = uintBitsToFloat(w7); t.uv1 = uintBitsToFloat(w8); t.uv2 = uintBitsToFloat(w9);
    t.tex = w10.x; t.flags = w10.y;
    t.id = w11.x; t.prim = w11.y;
    t.Lavg = uintBitsToFloat(uvec3(w12, w13.x));
    t.tkF = (t.flags & EM_R_TAKE) != 0u ? max(uintBitsToFloat(w13.y), 0.0) : -1.0;
    return t;
}
uint emHashSlot(uint id) { return regirHash(id * 2654435761u + 0x9E3779B9u) & (REGIR_EM_HASH_N - 1u); }
uint emHashWord(uint par, uint h) { return REGIR_EM_HASH_BASE + par * REGIR_EM_HASH_N + h; }
// Parity par's emitter entry of instance id (REGIR_EM_NONE: not in that build's hash).
uint emHashFindP(uint par, uint id)
{
    uint h = emHashSlot(id);
    [[dont_unroll]] for (uint i = 0u; i < REGIR_EM_HASH_N; ++i)
    {
        const uvec2 e = rg_cells[emHashWord(par, h)];
        if (e.x == 0u) return REGIR_EM_NONE;
        if (e.x == id + 1u) return e.y < REGIR_EM_INST_CAP ? e.y : REGIR_EM_NONE;
        h = (h + 1u) & (REGIR_EM_HASH_N - 1u);
    }
    return REGIR_EM_NONE;
}
// This build's emitter entry of instance id (REGIR_EM_NONE: not an emitter this frame, or dropped).
uint emHashFind(uint id)
{
    const uint e = emHashFindP(emParity(), id);
    return e < emEntCount() ? e : REGIR_EM_NONE;
}
// Last build's record slot os (the other parity) -> the same mesh triangle's slot in this build
// (REGIR_EM_NONE: its emitter is gone / dropped, or the triangle is not in this frame's subset).
uint emRemap(uint os)
{
    if (os >= REGIR_EM_CAP) return REGIR_EM_NONE;
    const uvec2 ip = rg_cells[emRecWord(emParity() ^ 1u, os) + 11u];
    const uint e = emHashFind(ip.x);
    if (e == REGIR_EM_NONE) return REGIR_EM_NONE;
    const uvec2 fk = rg_cells[emEntWord(e) + 1u], so = rg_cells[emEntWord(e) + 2u];
    const uint s = max(so.x, 1u);
    if (ip.y < so.y || (ip.y - so.y) % s != 0u) return REGIR_EM_NONE;
    const uint j = (ip.y - so.y) / s;
    if (j >= fk.y) return REGIR_EM_NONE;
    const uint slot = fk.x + j;
    if (slot >= emCount() || rg_cells[emRecWord(emParity(), slot) + 11u] != ip) return REGIR_EM_NONE;
    return slot;
}
// The range window: 1 near, 0 at the range (smooth).
float emWindow(float d, float range)
{
    const float t = d / max(range, 1e-4);
    const float t2 = t * t;
    const float w = clamp(1.0 - t2 * t2, 0.0, 1.0);
    return w * w;
}
// (LIGHT TAKEOVER) THE TAKEOVER WINDOW of a taken-over light's triangle (range = the light's radius R,
// falloff f): the point form's 1 / d^2 x this = atten(d / R) / atten(1/3) x 9 -- the SL light's own falloff
// (regirAtten, 0 from R on), 1 at d = R/3 (the calibration point: mode 1's gain); under EM_TAKE_NEAR x R
// the distance is held, so the near field stays the physical form factor (it saturates near the glass;
// the point form would grow without bound, the SL light does not). Bounded (<= ~1.3 past the near field).
float emTakeWin(float d, float range, float f)
{
    const float x = d / max(range, 1e-4);
    const float a3 = max(regirAtten(1.0 / 3.0, f), 1e-6);
    const float xm = max(x, EM_TAKE_NEAR);
    const float w = regirAtten(x, f) / a3 * 9.0 * xm * xm;
    return (w > 0.0 && w < 3.0e38) ? w : 0.0;
}
// The window of a triangle: tkF >= 0 (EM_R_TAKE) the takeover window, else the range window.
float emWin(float d, float range, float tkF) { return tkF >= 0.0 ? emTakeWin(d, range, tkF) : emWindow(d, range); }
// (LIGHT TAKEOVER) Point light li is carried by its emitting triangles this frame (mode 1's rs_em_take;
// the header upload clears it, so a frame without the build has none). The point light's readers that
// the emitters replace (pass 15, the lit kernels' walk) skip it -- see EMISSIVE LIGHTS, LIGHT TAKEOVER.
bool emTaken(uint li) { return li < 256u && (rg.rs_em_take[li >> 5u] & (1u << (li & 31u))) != 0u && emLitOn(); }
// The triangle's centroid (relative to its v0) and bounding radius about it.
void emTriBound(vec3 e1, vec3 e2, out vec3 cen, out float r)
{
    cen = (e1 + e2) * (1.0 / 3.0);
    r = sqrt(max(max(dot(cen, cen), dot(e1 - cen, e1 - cen)), dot(e2 - cen, e2 - cen)));
}
// The horizon clip (Sutherland-Hodgman against dot(N, x) > 0, x relative to P): the vertex count
// (0, 3 or 4) of the part of triangle (a, b, c) above P's horizon, in order.
uint emClipHorizon(vec3 N, vec3 a, vec3 b, vec3 c, out vec3 q0, out vec3 q1, out vec3 q2, out vec3 q3)
{
    vec3 q[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    const vec3 v[3] = vec3[3](a, b, c);
    const vec3 h = vec3(dot(N, a), dot(N, b), dot(N, c));
    uint n = 0u;
    [[unroll]] for (uint i = 0u; i < 3u; ++i)
    {
        const uint j = i == 2u ? 0u : i + 1u;
        if (h[i] > 0.0) { q[min(n, 3u)] = v[i]; ++n; }
        if ((h[i] > 0.0) != (h[j] > 0.0)) { q[min(n, 3u)] = mix(v[i], v[j], h[i] / (h[i] - h[j])); ++n; }
    }
    q0 = q[0]; q1 = q[1]; q2 = q[2]; q3 = q[3];
    return n >= 3u ? min(n, 4u) : 0u;
}
// One edge of the contour integral: theta x the unit normal of the edge's great circle (u, w unit).
vec3 emEdge(vec3 u, vec3 w)
{
    const vec3 c = cross(u, w);
    const float s = length(c);
    return s > 1e-12 ? c * (atan(s, dot(u, w)) / s) : vec3(0.0);
}
// THE LIGHT (above): the unshadowed form factor FF of triangle t at (P, N) -- its diffuse light is
// diffuseColor x L(P) x FF -- and Rd = the unit direction of its vector irradiance (toward the light;
// emTriRadiance's lookup direction). 0 out of range, behind a one-sided triangle, on its plane, or
// below P's horizon.
float emTriFF(EmTri t, vec3 P, vec3 N, out vec3 Rd)
{
    Rd = N;
    if (!(t.range > 0.0)) return 0.0;
    const vec3 a = t.v0 - P;
    const vec3 n = cross(t.e1, t.e2);
    const float n2 = dot(n, n);
    if (!(n2 > 1e-16)) return 0.0;
    const float side = -dot(n, a);   // > 0: P on the front side
    if (!(side > 0.0) && ((t.flags & EM_R_TWOSIDED) == 0u || !(side < 0.0))) return 0.0;
    vec3 cen;
    float rt;
    emTriBound(t.e1, t.e2, cen, rt);
    cen += a;
    const float dc = length(cen);
    const float win = emWin(max(dc - rt, 0.0), t.range, t.tkF);
    if (!(win > 0.0)) return 0.0;
    float ff;
    if (dc > 64.0 * rt)
    {   // far: the point form (A cos_e cos_r / (pi d^2))
        Rd = cen / dc;
        ff = 0.5 * abs(dot(n, Rd)) * max(dot(N, Rd), 0.0) / (EM_PI * dc * dc);
    }
    else
    {
        vec3 q0, q1, q2, q3;
        const uint k = emClipHorizon(N, a, a + t.e1, a + t.e2, q0, q1, q2, q3);
        if (k < 3u) return 0.0;
        const vec3 u0 = q0 / max(length(q0), 1e-12), u1 = q1 / max(length(q1), 1e-12);
        const vec3 u2 = q2 / max(length(q2), 1e-12);
        vec3 F = emEdge(u0, u1) + emEdge(u1, u2);
        if (k == 4u)
        {
            const vec3 u3 = q3 / max(length(q3), 1e-12);
            F += emEdge(u2, u3) + emEdge(u3, u0);
        }
        else F += emEdge(u2, u0);
        // (the clipped part is wholly above the horizon: the sign is the winding's alone)
        const float f = dot(F, N);
        ff = abs(f) * (0.5 / EM_PI);
        const float fl = length(F);
        if (fl > 1e-12) Rd = F * ((f < 0.0 ? -1.0 : 1.0) / fl);
    }
    ff *= win;
    return (ff > 0.0 && ff < 3.0e38) ? ff : 0.0;
}
// The triangle's VECTOR form factor at Q (unclipped; toward the light, length = its form factor for a
// receiver facing it), x the range window; 0 behind a one-sided triangle. The rest's aggregate (below).
vec3 emTriVecFF(EmTri t, vec3 Q)
{
    if (!(t.range > 0.0)) return vec3(0.0);
    const vec3 a = t.v0 - Q;
    const vec3 n = cross(t.e1, t.e2);
    if (!(dot(n, n) > 1e-16)) return vec3(0.0);
    const float side = -dot(n, a);
    if (!(side > 0.0) && ((t.flags & EM_R_TWOSIDED) == 0u || !(side < 0.0))) return vec3(0.0);
    vec3 cen;
    float rt;
    emTriBound(t.e1, t.e2, cen, rt);
    cen += a;
    const float dc = length(cen);
    const float win = emWin(max(dc - rt, 0.0), t.range, t.tkF);
    if (!(win > 0.0) || !(dc > 1e-6)) return vec3(0.0);
    vec3 F;
    if (dc > 64.0 * rt)
    {
        const vec3 w = cen / dc;
        F = w * (0.5 * abs(dot(n, w)) / (EM_PI * dc * dc));
    }
    else
    {
        const vec3 b = a + t.e1, c = a + t.e2;
        const vec3 u0 = a / max(length(a), 1e-12), u1 = b / max(length(b), 1e-12), u2 = c / max(length(c), 1e-12);
        F = (emEdge(u0, u1) + emEdge(u1, u2) + emEdge(u2, u0)) * (0.5 / EM_PI);
        if (dot(F, cen) < 0.0) F = -F;
    }
    return F * win;
}
// Sphere-light irradiance for radiance 1 with the horizon clip (Snyder 1996, the Frostbite form):
// cosT = N . (unit vector to the centre), sin2 = sin^2 of the subtended half-angle (< 1).
float emSphereIllum(float cosT, float sin2)
{
    if (cosT * cosT > sin2) return EM_PI * sin2 * max(cosT, 0.0);
    const float sinT = sqrt(max(1.0 - cosT * cosT, 1e-8));
    const float x = sqrt(max(1.0 / sin2 - 1.0, 0.0));
    const float y = clamp(-x * (cosT / sinT), -1.0, 1.0);
    const float sy = sinT * sqrt(max(1.0 - y * y, 0.0));
    return max((cosT * acos(y) - x * sy) * sin2 + atan(sy, x), 0.0);
}
// THE REST'S UNSHADOWED LIGHT at a receiver facing N (the lit kernels; cell word ecw of a cell that keeps
// its strongest triangles exactly): the cell's aggregate -- the summed vector form factor V of the rest
// at the cell centre as one horizon-clipped sphere-equivalent source (sin^2 = |V|, the Frostbite polygon
// approximation), x their form-factor-weighted average colour. Deterministic (no per-frame sample).
vec3 emCellRest(uint ecw, vec3 N)
{
    const uvec2 a = rg_cells[ecw + REGIR_EM_SLOTS_MAX + 1u], b = rg_cells[ecw + REGIR_EM_SLOTS_MAX + 2u];
    const uvec2 c = rg_cells[ecw + REGIR_EM_SLOTS_MAX + 3u];
    const vec3 V = uintBitsToFloat(uvec3(a, b.x));
    const vec3 C = uintBitsToFloat(uvec3(b.y, c));
    const float v = length(V);
    if (!(v > 1e-12) || !(v < 3.0e38)) return vec3(0.0);
    const vec3 E = C * (emSphereIllum(dot(N, V / v), min(v, 0.9999)) * (1.0 / EM_PI));
    return (any(isnan(E)) || any(isinf(E))) ? vec3(0.0) : max(E, vec3(0.0));
}
// Cell build data of one record (compact, the workgroup's shared copy): centroid + bounding radius, the
// front plane (unit normal, offset; a two-sided triangle: 0, 1 -- always in front), average luminance,
// area, range, the takeover falloff (-1: none; emWin).
void emTriCompact(EmTri t, out vec4 cr, out vec4 pl, out vec4 la)
{
    vec3 cen;
    float r;
    emTriBound(t.e1, t.e2, cen, r);
    cr = vec4(t.v0 + cen, r);
    const vec3 n = cross(t.e1, t.e2);
    const float nl = length(n);
    const vec3 nu = nl > 1e-12 ? n / nl : vec3(0.0);
    pl = (t.flags & EM_R_TWOSIDED) != 0u || !(nl > 1e-12) ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(nu, -dot(nu, t.v0));
    la = vec4(t.lum, 0.5 * nl, t.range, t.tkF);
}
// Can the triangle light any point of the box [lo, hi]? (its range from its bounding sphere; one-sided:
// some corner strictly in front of its plane)
bool emReachBox(vec4 cr, vec4 pl, vec4 la, vec3 lo, vec3 hi)
{
    if (!(la.z > 0.0) || !(la.x > 0.0) || !(la.y > 0.0)) return false;
    const vec3 dq = clamp(cr.xyz, lo, hi) - cr.xyz;
    const float rr = la.z + cr.w;
    if (!(dot(dq, dq) < rr * rr)) return false;
    float m = -1e30;
    [[unroll]] for (int k = 0; k < 8; ++k)
    {
        const vec3 p = vec3((k & 1) != 0 ? hi.x : lo.x, (k & 2) != 0 ? hi.y : lo.y, (k & 4) != 0 ? hi.z : lo.z);
        m = max(m, dot(pl.xyz, p) + pl.w);
    }
    return m > 0.0;
}
// The cell target of the triangle for the box [lo, hi] (0: not a candidate) -- > 0 wherever emTriFF can
// be > 0 inside the box: emReachBox is conservative and the floor keeps a corner-only reach above 0.
float emCellTarget(vec4 cr, vec4 pl, vec4 la, vec3 lo, vec3 hi)
{
    if (!emReachBox(cr, pl, la, lo, hi)) return 0.0;
    const vec3 cc = 0.5 * (lo + hi);
    const float dc = max(length(cc - cr.xyz), 1e-3);
    const float dm = max(length(clamp(cr.xyz, lo, hi) - cr.xyz), 1e-3);
    const float tc = min(la.y / (EM_PI * dc * dc), 1.0) * emWin(max(dc - cr.w, 0.0), la.z, la.w);
    const float tm = min(la.y / (EM_PI * dm * dm), 1.0) * emWin(max(dm - cr.w, 0.0), la.z, la.w);
    const float t = la.x * max(max(tc, 0.25 * tm), 1e-9);
    return (t > 0.0 && t < 3.0e38) ? t : 0.0;
}
// Record group g's light box (GROUPS above) overlaps the box [lo, hi].
bool emGrpReach(uint g, vec3 lo, vec3 hi)
{
    const uint b = emGrpWord(g);
    const uvec2 w0 = rg_cells[b], w1 = rg_cells[b + 1u], w2 = rg_cells[b + 2u];
    const vec3 glo = uintBitsToFloat(uvec3(w0, w1.x)), ghi = uintBitsToFloat(uvec3(w1.y, w2));
    return all(lessThanEqual(glo, hi)) && all(greaterThanEqual(ghi, lo));
}
// Solid angle of the spherical triangle (a, b, c) seen from the origin (Van Oosterom & Strackee 1983).
float emSolidAngle(vec3 a, vec3 b, vec3 c)
{
    const float la = length(a), lb = length(b), lc = length(c);
    const float num = abs(dot(a, cross(b, c)));
    const float den = la * lb * lc + dot(a, b) * lc + dot(a, c) * lb + dot(b, c) * la;
    return 2.0 * atan(num, den);
}
float emAngleBetween(vec3 v1, vec3 v2)
{
    return dot(v1, v2) < 0.0 ? EM_PI - 2.0 * asin(min(0.5 * length(v1 + v2), 1.0))
                             : 2.0 * asin(min(0.5 * length(v2 - v1), 1.0));
}
// A direction uniform in the solid angle of the spherical triangle (A, B, C) (unit; Arvo 1995, the
// PBRT-v4 form), u in [0, 1)^2. False: degenerate (the caller samples the area instead).
bool emSampleSphTri(vec3 A, vec3 B, vec3 C, vec2 u, out vec3 w)
{
    w = A;
    vec3 nab = cross(A, B), nbc = cross(B, C), nca = cross(C, A);
    if (!(dot(nab, nab) > 1e-14) || !(dot(nbc, nbc) > 1e-14) || !(dot(nca, nca) > 1e-14)) return false;
    nab = normalize(nab); nbc = normalize(nbc); nca = normalize(nca);
    const float alpha = emAngleBetween(nab, -nca);
    const float beta  = emAngleBetween(nbc, -nab);
    const float gamma = emAngleBetween(nca, -nbc);
    const float api = mix(EM_PI, alpha + beta + gamma, u.x);   // the sub-area's A' + pi
    if (!(alpha + beta + gamma - EM_PI > 1e-6)) return false;
    const float ca = cos(alpha), sa = sin(alpha);
    const float sp = sin(api) * ca - cos(api) * sa;
    const float cp = cos(api) * ca + sin(api) * sa;
    const float k1 = cp + ca;
    const float k2 = sp - sa * dot(A, B);
    const float dn = (k2 * sp + k1 * cp) * sa;
    if (!(abs(dn) > 1e-12)) return false;
    const float cbp = clamp((k2 + (k2 * cp - k1 * sp) * ca) / dn, -1.0, 1.0);
    const float sbp = sqrt(max(1.0 - cbp * cbp, 0.0));
    const vec3 ca_ = C - dot(C, A) * A;
    if (!(dot(ca_, ca_) > 1e-14)) return false;
    const vec3 Cp = cbp * A + sbp * normalize(ca_);
    const float ct = 1.0 - u.y * (1.0 - dot(Cp, B));
    const float st = sqrt(max(1.0 - ct * ct, 0.0));
    const vec3 cb_ = Cp - dot(Cp, B) * B;
    w = dot(cb_, cb_) > 1e-14 ? ct * B + st * normalize(cb_) : B;
    return !any(isnan(w));
}
// THE SAMPLE POINT of triangle t for a receiver at (P, N) (pass 15's emitter pick): uniform in the solid
// angle of the part above P's horizon (emTriFF's integrand up to P's cosine: the clipped polygon as a
// fan of one or two spherical triangles, picked by solid angle with xi.z); uniform over the area for a
// tiny / far / degenerate one. Returns the point (world).
vec3 emTriSample(EmTri t, vec3 P, vec3 N, vec3 xi)
{
    const vec3 a = t.v0 - P;
    const vec3 n = cross(t.e1, t.e2);
    // the area form (uniform barycentrics)
    const float su = sqrt(xi.x);
    vec3 X = t.v0 + t.e1 * (su * (1.0 - xi.y)) + t.e2 * (su * xi.y);
    vec3 cen;
    float rt;
    emTriBound(t.e1, t.e2, cen, rt);
    if (length(cen + a) > 64.0 * rt) return X;
    vec3 q0, q1, q2, q3;
    const uint k = emClipHorizon(N, a, a + t.e1, a + t.e2, q0, q1, q2, q3);
    if (k < 3u) return X;
    const float o1 = emSolidAngle(q0, q1, q2);
    const float o2 = k == 4u ? emSolidAngle(q0, q2, q3) : 0.0;
    if (!(o1 + o2 > 1e-5)) return X;
    const bool second = xi.z * (o1 + o2) >= o1;
    const vec3 B = normalize(second ? q2 : q1), C = normalize(second ? q3 : q2);
    vec3 w;
    if (!emSampleSphTri(normalize(q0), B, C, xi.xy, w)) return X;
    const float dn = dot(n, w);
    if (!(abs(dn) > 1e-12)) return X;
    const float s = dot(n, a) / dn;   // the ray P + s w meets the triangle's plane
    if (!(s > 0.0) || !(s < 1e7)) return X;
    return P + w * s;
}
uint rsCellWord(uint cell) { return REGIR_RS_BASE + cell * (REGIR_RES_MAX + 1u); }
// This frame's index of last frame's light lp (0xFFFFFFFF: gone / re-keyed).
uint rsRemap(uint lp)
{
    if (lp >= 256u) return 0xFFFFFFFFu;
    const uint b = (rg.rs_remap[lp >> 2u] >> ((lp & 3u) * 8u)) & 0xFFu;
    return b == 0xFFu ? 0xFFFFFFFFu : b;
}

// The lights to evaluate at P (normal N, rng seed): writes up to REGIR_PICKS_MAX (light, weight)
// pairs and returns their count -- the cell's whole list with weight 1 (an exact cell), or the
// RIS picks with their unbiased weights (duplicates merged) -- or REGIR_NO_CELL (P outside the
// grid: the caller's exact path). cellInfo = the cell's candidate count | REGIR_CELL_EXACT.
uint regirHitLights(vec3 P, vec3 N, uint seed, out uint pl[REGIR_PICKS_MAX], out float pw[REGIR_PICKS_MAX],
                    out uint cellInfo)
{
    [[unroll]] for (uint k = 0u; k < REGIR_PICKS_MAX; ++k) { pl[k] = 0u; pw[k] = 0.0; }
    cellInfo = 0u;
    const uint cell = regirCell(P);
    if (cell == REGIR_NO_CELL) return REGIR_NO_CELL;
    const uint R = regirRes();
    const uint base = cell * (R + 1u);
    const uvec2 h = rg_cells[base];
    cellInfo = h.x;
    const uint M = h.x & REGIR_COUNT_MASK;
    if (M == 0u) return 0u;   // no light reaches the cell: an exact 0
    if ((h.x & REGIR_CELL_EXACT) != 0u)
    {
        const uint n = min(M, REGIR_PICKS_MAX);
        for (uint i = 0u; i < n; ++i) { pl[i] = rg_cells[base + 1u + i].x; pw[i] = 1.0; }
        return n;
    }
    const uint N_ = regirPicks();
    uint  selL[REGIR_PICKS_MAX];
    float selP[REGIR_PICKS_MAX];
    [[unroll]] for (uint k = 0u; k < REGIR_PICKS_MAX; ++k) { selL[k] = 0u; selP[k] = 0.0; }
    float wsum = 0.0;
    uint rng = seed;
    for (uint j = 0u; j < R; ++j)
    {
        const uvec2 s = rg_cells[base + 1u + j];
        const uint li = s.x;
        const float p = regirHitTarget(li, P, N);
        const float w = p * uintBitsToFloat(s.y);
        if (!(w > 0.0) || !(w < 3.0e38)) continue;
        wsum += w;
        [[unroll]] for (uint k = 0u; k < REGIR_PICKS_MAX; ++k)
        {
            if (k < N_ && regirU01(rng) * wsum < w) { selL[k] = li; selP[k] = p; }
        }
    }
    if (!(wsum > 0.0) || !(wsum < 3.0e38)) return 0u;   // no sample reaches P: the estimate is 0
    const float scale = wsum / (float(R) * float(N_));
    uint n = 0u;
    for (uint k = 0u; k < N_; ++k)
    {
        const float wk = scale / selP[k];
        bool merged = false;
        for (uint i = 0u; i < n; ++i)
            if (pl[i] == selL[k]) { pw[i] += wk; merged = true; }
        if (!merged) { pl[n] = selL[k]; pw[n] = wk; ++n; }
    }
    return n;
}

#endif
