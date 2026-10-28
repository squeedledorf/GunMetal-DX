// vkclean — SUN EXPOSURE CERTIFICATES (SXC, #771 Phase 2; RenderVKSunCert): a world-space cache
// of "this surface cannot see the sun for ANY sun direction along the next stretch of the sun's
// path", consumed by the merged RT shadow pass (clean_deferred_light.comp pass 15, generic core:
// CLEAN_SHADOW_MERGE + CLEAN_SHADOW_CORE) on the SUN / MOON SAMPLE ONLY (sample 0). Host side and
// the table's life cycle: newview/vkgfx/llcleansuncertvk.cpp (the header of that file has the
// full design); the per-frame maintenance kernel: clean_sun_cert.comp.
//
// WHY IT STAYS VALID WHILE THE SUN MOVES (the earlier caches did not: they certified the PRESENT
// -- "history uniform, nothing changed since last frame" -- and a sun that moves a little every
// frame broke that everywhere, slowly). A certificate covers an ARC WEDGE of sun directions: the
// sun's path (its great circle through the anchor La, tangent B, axis A) is cut into BINS of beta
// (RenderVKSunCertBinDeg, 2 deg = ~80 s at the default 4 h day), each widened by the disc radius
// along the arc and by +-(mu + disc) across it (RenderVKSunCertLateralDeg). A cell's ROUND traces
// certification rays through every bin of [rs, rs + nb) (the current one + RenderVKSunCertLookahead
// ahead); a bin is CERTIFIED BLOCKED when every one of its rays hit static opaque geometry. The
// sun's motion only advances the bin index -- a certificate expires when the sun's disc leaves its
// bins (minutes to hours), never because time passed a little. A sun that does not move (fixed
// skies, clubs) uses one CONE bin around the anchor (cone mode) and never expires.
//
// SKIP: a sun texel whose cell (world-space key: level x cell x normal bucket) is certified for
// every bin the sun's disc touches now, whose 4x4 SUB-CELL contributed rays to that round
// (coverage: a surface part the round never saw is traced), within RenderVKSunCertMaxAge, not a
// spot check (1 / RenderVKSunCertSpotCheck of would-skip texels trace anyway), writes EXACTLY what
// a fully blocked trace writes -- mix(1, mix(0, 1, far fade), weak-sun weight), the penumbra radius
// from the round's nearest blocker, no moving caster -- and traces no sun ray, no terrain re-anchor,
// no terminator / rig probe. The denoiser cannot tell a skipped texel from a traced umbra texel:
// no history cut, no pop when a certificate is made or expires.
//
// SAFETY: certification rays see only STATIC opaque geometry (RT_MASK_OPAQUE | RT_MASK_TERRAIN,
// a hit that moved since last frame / spins / is rigged or attached is inconclusive = open);
// alpha-mask and alpha-blend never certify (light passes them: traced as today). A traced or
// spot-checked sun ray that comes back LIT in a cell certified for the current bin REVOKES the
// cell and its 6 neighbours; static change spheres (moved / re-registered / rezzed / deleted
// objects, BLAS swaps) revoke every cell whose wedge they cross (maintenance kernel, kept
// RenderVKSunCertChangeFrames frames); sun jumps, sun <-> moon, origin shifts, settings changes
// start a new EPOCH (every certificate dropped: traced as today until re-certified).
//
// BUFFER (one device-local SSBO, device address in the push's rtr3.xy; words):
//   [0, 64)        header, written by the host each frame (SXC_H_*)
//   [64, 128)      stats (atomics; the host copies + clears them ~5 s)
//   [128, 1152)    change spheres (xyz r, agent space), SXC_H_NSPH of them
//   [SXC_TABLE, ..) the table: 8-slot buckets of 16-word entries (SXC_E_*)
#ifndef CLEAN_SUN_CERT_GLSL
#define CLEAN_SUN_CERT_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer SxcBuf { uint w[]; };

// ---- header words (llcleansuncertvk.cpp SxcHeader: keep in step) ----
#define SXC_H_LA      0u    // f32 x3 anchor direction La, [3] beta (rad)
#define SXC_H_B       4u    // f32 x3 the arc's tangent at La, [7] lateral half-width mu + disc (rad)
#define SXC_H_A       8u    // f32 x3 the arc's axis La x B, [11] disc radius (rad)
#define SXC_H_EPOCH   12u   // u32 epoch (>= 1)
#define SXC_H_FRAME   13u   // u32 SXC frame counter
#define SXC_H_FLAGS   14u   // u32 SXC_HF_*
#define SXC_H_CAPMASK 15u   // u32 buckets - 1 (power of two; 8 slots each)
#define SXC_H_BINLO   16u   // u32 the lowest / highest bin the sun's disc touches now (absolute)
#define SXC_H_BINHI   17u
#define SXC_H_SUNBIN  18u   // u32 the bin of the sun's centre
#define SXC_H_NB      19u   // u32 bins per round (1 + look-ahead; 1 in cone mode), <= 32
#define SXC_H_SPOT    20u   // u32 spot-check mask (rate - 1, rate a power of two; 0 = every would-skip traced)
#define SXC_H_TICKETS 21u   // u32 certifying texels per cell per frame
#define SXC_H_RPB     22u   // u32 certification rays per bin per round
#define SXC_H_BUDGET  23u   // u32 round starts per frame
#define SXC_H_CELL    24u   // f32 level-0 cell edge (m)
#define SXC_H_LEVELS  25u   // u32 level count (cell edge doubles per level)
#define SXC_H_PXCELL  26u   // f32 render-pixel footprints per cell edge (level choice)
#define SXC_H_TMAX    27u   // f32 certification ray length (m)
#define SXC_H_NSPH    28u   // u32 change spheres
#define SXC_H_IDLE    29u   // u32 frames untouched before an entry is evicted
#define SXC_H_MAXAGE  30u   // u32 frames a certificate is used before it is re-certified
#define SXC_H_GAP     31u   // u32 minimum frames between two rounds of a cell (x 4^revocations)
#define SXC_H_DEBUG   32u   // u32 RenderVKSunCertDebug
#define SXC_H_CONE    33u   // f32 cone mode: the cone's radius (rad); 0 = arc mode
#define SXC_H_SPREAD  34u   // f32 the change-sphere sweep's half angle around L (rad)
#define SXC_H_ROUNDS  40u   // u32 (per frame, zeroed by the host) round starts so far
#define SXC_H_LIVE    41u   // u32 (per frame, zeroed by the host) live entries
#define SXC_HF_ON     1u
// ---- stats words (SXC_STATS + i; the host's log line) ----
#define SXC_STATS     64u
#define SXC_S_ELIG     0u   // sun texels SXC looked at (eligible)
#define SXC_S_SKIP     1u   // skipped (certified)
#define SXC_S_SPOT     2u   // would skip, traced as a spot check
#define SXC_S_UNCOV    3u   // certified cell, sub-cell not covered: traced
#define SXC_S_NOCELL   4u   // no entry (new / pending / bucket full): traced
#define SXC_S_NOCERT   5u   // entry, not certified for the sun's bins now: traced
#define SXC_S_CRAYS    6u   // certification rays
#define SXC_S_CBLOCK   7u   // ... that hit static opaque geometry
#define SXC_S_REVLIT   8u   // revocations: a traced / spot ray came back lit in a certified cell
#define SXC_S_INSERT   9u   // new entries
#define SXC_S_FULL     10u  // insert refused: the bucket's 8 slots are taken
#define SXC_S_INELIG   11u  // sun texels SXC did not consider (terminator band / facing / range)
#define SXC_S_SPOTLIT  12u  // spot checks that came back lit
#define SXC_S_CERTTX   13u  // would skip, traced to certify a renewal round
#define SXC_S_LIVE     16u  // (maintenance) live entries, summed over frames
#define SXC_S_COMMIT   17u  //   entries holding a committed round
#define SXC_S_CERTNOW  18u  //   entries certified for the sun's bins now
#define SXC_S_RACTIVE  19u  //   rounds in progress
#define SXC_S_RSTART   20u  //   rounds started
#define SXC_S_RDONE    21u  //   rounds committed
#define SXC_S_RBLOCK   22u  //   ... with at least one bin certified blocked
#define SXC_S_REVCHG   23u  //   revoked by a change sphere
#define SXC_S_REVFLAG  24u  //   revoked by the pass's flag (lit ray)
#define SXC_S_EVICT    25u  //   evicted (idle)
#define SXC_S_EPOCH    26u  //   entries reset by an epoch change
#define SXC_S_FRAMES   27u  //   maintenance frames
#define SXC_S_DEFER    28u  //   round starts deferred by the per-frame budget
#define SXC_S_RRESTART 29u  //   rounds restarted (the sun left the round's bins)
#define SXC_SPHERES   128u
#define SXC_MAX_SPHERES 256u
#define SXC_TABLE     1152u
// ---- entry words (16 per slot) ----
#define SXC_E_KEY0    0u    // 0x80000000 | x15 << 15 | y15
#define SXC_E_KEY1    1u    // 0x80000000 | z15 << 6 | level3 << 3 | bucket3
#define SXC_E_BLOCK   2u    // committed: bit i = bin cb + i certified blocked
#define SXC_E_CB      3u    // committed: the first bin (absolute)
#define SXC_E_COV     4u    // committed: the sub-cells (4 x 4) that traced the round
#define SXC_E_BT      5u    // committed: nearest static blocker (f32 bits, m)
#define SXC_E_CFRAME  6u    // committed: the frame it was committed
#define SXC_E_FLAGS   7u    // SXC_F_* | revocation count << 8 | epoch (16 bit) << 16
#define SXC_E_TOUCH   8u    // last frame a texel looked it up
#define SXC_E_TICKET  9u    // certifying texels this frame (atomic; maintenance zeroes)
#define SXC_E_RS      10u   // round: first bin (absolute)
#define SXC_E_RRAYS   11u   // round: rays issued (atomic)
#define SXC_E_ROPEN   12u   // round: bins a ray found open (atomic)
#define SXC_E_RCOV    13u   // round: sub-cells that traced (atomic)
#define SXC_E_RBT     14u   // round: nearest static blocker (atomicMin, f32 bits)
#define SXC_E_RFRAME  15u   // round: the frame it started
#define SXC_WORDS     16u
#define SXC_F_NEED    1u    // (pass) a round is wanted: new entry / uncovered sub-cell
#define SXC_F_REVOKE  2u    // (pass) a lit ray in a certified cell
#define SXC_F_COMMIT  4u    // (maintenance) a committed round is held
#define SXC_F_ROUND   8u    // (maintenance) a round is in progress
#define SXC_BIN0      1024u // the anchor's bin (absolute bins start here: the sun may sit a half bin before it)

// ---- the shared math (pass 15 and the maintenance kernel) ----
uint sxcHash(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
uint sxcBucketOf(uint key0, uint key1, uint capMask) { return sxcHash(key0 ^ sxcHash(key1 * 0x9E3779B9u)) & capMask; }
float sxcCellSize(SxcBuf b, uint level) { return uintBitsToFloat(b.w[SXC_H_CELL]) * float(1u << level); }
// The cell's centre (agent space) from its keys.
vec3 sxcCellCentre(SxcBuf b, uint key0, uint key1)
{
    const uint level = (key1 >> 3u) & 7u;
    const float cs = sxcCellSize(b, level);
    const ivec3 c = ivec3(int((key0 >> 15u) & 0x7FFFu) - 16384, int(key0 & 0x7FFFu) - 16384, int((key1 >> 6u) & 0x7FFFu) - 4096);
    return (vec3(c) + 0.5) * cs;
}
// Sun direction of wedge sample (bin k relative to the round's first bin rs; u in [0, 1] along the
// bin incl. the disc margin, v in [-1, 1] across) in arc mode; a disc sample of the cone in cone mode.
vec3 sxcWedgeDir(SxcBuf b, uint absBin, vec2 uv)
{
    const vec3 La = vec3(uintBitsToFloat(b.w[SXC_H_LA]), uintBitsToFloat(b.w[SXC_H_LA + 1u]), uintBitsToFloat(b.w[SXC_H_LA + 2u]));
    const vec3 B  = vec3(uintBitsToFloat(b.w[SXC_H_B]),  uintBitsToFloat(b.w[SXC_H_B + 1u]),  uintBitsToFloat(b.w[SXC_H_B + 2u]));
    const vec3 A  = vec3(uintBitsToFloat(b.w[SXC_H_A]),  uintBitsToFloat(b.w[SXC_H_A + 1u]),  uintBitsToFloat(b.w[SXC_H_A + 2u]));
    const float cone = uintBitsToFloat(b.w[SXC_H_CONE]);
    if (cone > 0.0)
    {
        // uv: (radius fraction, angle fraction) -> a point of the cone's disc (tangent plane)
        const float r = cone * uv.x;
        const float a = 6.2831853 * uv.y;
        return normalize(La + (B * cos(a) + A * sin(a)) * tan(r));
    }
    const float beta = uintBitsToFloat(b.w[SXC_H_LA + 3u]);
    const float lat = uintBitsToFloat(b.w[SXC_H_B + 3u]);
    const float disc = uintBitsToFloat(b.w[SXC_H_A + 3u]);
    const float k = float(int(absBin) - int(SXC_BIN0));
    const float th = (k - 0.5) * beta - disc + uv.x * (beta + 2.0 * disc);
    const float ph = uv.y * lat;
    return normalize(cos(ph) * (cos(th) * La + sin(th) * B) + sin(ph) * A);
}
// Certification pattern p of a bin: the 4 wedge corners first (the plan's corners), then the R2
// sequence over the interior (re-randomised per round by the round's start frame).
vec2 sxcPattern(uint p, uint seed, bool cone)
{
    if (p < 4u)
    {
        if (cone) return vec2(1.0, (float(p) + 0.125 * float(seed & 7u)) * 0.25);   // 4 points on the rim
        return vec2(float(p & 1u), (p & 2u) != 0u ? 1.0 : -1.0);
    }
    const vec2 r2 = fract(vec2(0.7548776662, 0.5698402910) * float(p - 4u + (seed & 1023u)) + 0.5);
    return cone ? vec2(sqrt(r2.x), r2.y) : vec2(r2.x, r2.y * 2.0 - 1.0);
}

#ifndef SXC_MAINTENANCE
// ============================ THE PASS SIDE (pass 15, sun sample) ============================
SxcBuf sxcBuf() { return SxcBuf(g_draw_push.rtr3.xy); }
bool sxcOn() { return (g_draw_push.rtr3.x | g_draw_push.rtr3.y) != 0u; }   // (push: uniform)

// Per sun texel: what the lookup decided, carried from before the trace to after it.
#define SXC_C_OFF     0u   // not considered (SXC off / ineligible)
#define SXC_C_SKIP    1u
#define SXC_C_SPOT    2u
#define SXC_C_UNCOV   3u
#define SXC_C_NOCELL  4u
#define SXC_C_NOCERT  5u
#define SXC_C_CERT    6u
struct SxcTexel {
    uint  e;        // the entry's first word (0xFFFFFFFF: none)
    uint  cls;      // SXC_C_*
    uint  sub;      // the receiver's sub-cell (0..15)
    bool  certNow;  // the cell claims the sun's current bins (a lit ray revokes it)
    bool  ticket;   // this texel traces certification rays after its sun sample
    uint  key0, key1;
    vec3  out_;     // SXC_C_SKIP: the sun value to write
    float srad;     // SXC_C_SKIP: the penumbra radius (denoiser texels)
};
SxcTexel sxcNone()
{
    SxcTexel t;
    t.e = 0xFFFFFFFFu; t.cls = SXC_C_OFF; t.sub = 0u; t.certNow = false; t.ticket = false;
    t.key0 = 0u; t.key1 = 0u; t.out_ = vec3(1.0); t.srad = -1.0;
    return t;
}
// Subgroup-aggregated stats counter (one atomic per subgroup and counter).
void sxcStat(SxcBuf b, uint i, bool on)
{
    const uint n = subgroupAdd(on ? 1u : 0u);
    if (n != 0u && subgroupElect()) atomicAdd(b.w[SXC_STATS + i], n);
}
// The cell keys of receiver P with visible-side normal N at pixel footprint pxw (m); false: out of
// the key range.
bool sxcKeys(SxcBuf b, vec3 P, vec3 N, float pxw, out uint key0, out uint key1, out uint sub)
{
    key0 = 0u; key1 = 0u; sub = 0u;
    const float base = uintBitsToFloat(b.w[SXC_H_CELL]);
    const uint levels = clamp(b.w[SXC_H_LEVELS], 1u, 8u);
    const float want = uintBitsToFloat(b.w[SXC_H_PXCELL]) * pxw;   // the smallest edge that holds enough pixels
    const uint level = uint(clamp(ceil(log2(max(want / base, 1.0))), 0.0, float(levels - 1u)));
    const float cs = base * float(1u << level);
    const vec3 q = P / cs;
    const ivec3 c = ivec3(floor(q));
    const ivec3 o = c + ivec3(16384, 16384, 4096);
    if (any(lessThan(o, ivec3(0))) || any(greaterThan(o, ivec3(32767)))) return false;
    // the normal bucket: the dominant axis and its sign (the two faces of a thin wall / roof differ)
    const vec3 an = abs(N);
    uint ax = an.x >= an.y && an.x >= an.z ? 0u : (an.y >= an.z ? 1u : 2u);
    const uint bkt = ax * 2u + (N[ax] < 0.0 ? 1u : 0u);
    key0 = 0x80000000u | (uint(o.x) << 15u) | uint(o.y);
    key1 = 0x80000000u | (uint(o.z) << 6u) | (level << 3u) | bkt;
    // the 4x4 sub-cell on the two axes across the bucket's axis
    const vec3 f = fract(q);
    const vec2 uv = ax == 0u ? f.yz : (ax == 1u ? f.xz : f.xy);
    const uvec2 s = min(uvec2(uv * 4.0), uvec2(3u));
    sub = s.y * 4u + s.x;
    return true;
}
// Find (or, insert = true, claim) the entry of keys (key0, key1). Returns its first word, else
// 0xFFFFFFFF (pending: another texel is claiming it this frame; full; or not found).
uint sxcFind(SxcBuf b, uint key0, uint key1, bool insert, uint frame, out bool inserted, out bool full)
{
    inserted = false; full = false;
    const uint bucket = sxcBucketOf(key0, key1, b.w[SXC_H_CAPMASK]);
    const uint e0 = SXC_TABLE + bucket * 8u * SXC_WORDS;
    uint empty = 8u;
    for (uint s = 0u; s < 8u; ++s)
    {
        const uint e = e0 + s * SXC_WORDS;
        const uint k1 = b.w[e + SXC_E_KEY1];
        if (k1 == key1)
        {
            const uint k0 = b.w[e + SXC_E_KEY0];
            if (k0 == key0) return e;
            if (k0 == 0u) return 0xFFFFFFFFu;   // (being claimed this frame: no key0 yet)
        }
        else if (k1 == 0u && empty == 8u) empty = s;
    }
    if (!insert) return 0xFFFFFFFFu;
    for (uint s = empty; s < 8u; ++s)
    {
        const uint e = e0 + s * SXC_WORDS;
        if (b.w[e + SXC_E_KEY1] != 0u) continue;
        const uint old = atomicCompSwap(b.w[e + SXC_E_KEY1], 0u, key1);
        if (old == 0u)
        {
            // an empty slot is all zero (the maintenance kernel clears evicted entries whole):
            // the key, the touch frame and a round request; the epoch mismatch makes the next
            // maintenance pass initialise it
            atomicExchange(b.w[e + SXC_E_KEY0], key0);
            b.w[e + SXC_E_TOUCH] = frame;
            atomicOr(b.w[e + SXC_E_FLAGS], SXC_F_NEED);
            inserted = true;
            return 0xFFFFFFFFu;   // (no certificate yet: trace)
        }
        if (old == key1) return 0xFFFFFFFFu;   // (claimed by another texel of this key or its twin)
    }
    full = true;
    return 0xFFFFFFFFu;
}
bool sxcBinsCertified(SxcBuf b, uint blocked, uint cb)
{
    const uint lo = b.w[SXC_H_BINLO], hi = b.w[SXC_H_BINHI];
    const uint nb = b.w[SXC_H_NB];
    if (lo < cb || hi >= cb + nb || hi < lo) return false;
    const uint n = hi - lo + 1u;
    const uint m = (n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u)) << (lo - cb);
    return (blocked & m) == m;
}

// BEFORE the sun sample (after the receiver is set up; pdnSunGate passed): the lookup and the skip
// decision. ngl: the receiver facet's N.L; fade / sw: pdnSunSample's far fade and the weak-sun
// weight; m: the denoiser texel; texPerPx: denoiser texels per render px.
SxcTexel sxcSunPre(vec3 P, vec3 N, vec3 L, float ngl, float pxw, float fade, float sw, ivec2 m, uint frame,
                   float texPerPx)
{
    SxcTexel t = sxcNone();
    if (!sxcOn()) return t;
    SxcBuf b = sxcBuf();
    if ((b.w[SXC_H_FLAGS] & SXC_HF_ON) == 0u) return t;
    // ELIGIBLE: clearly sun-facing (outside the #664 terminator band -- its lifted origins are not
    // the certification rays'), and a key in range
    const bool band = rtTerminatorOn() && rtTerminatorBand(ngl);
    bool elig = !band && ngl > 0.05;
    uint key0, key1, sub;
    if (elig) elig = sxcKeys(b, P, N, pxw, key0, key1, sub);
    sxcStat(b, SXC_S_INELIG, !elig);
    if (!elig) return t;
    t.key0 = key0; t.key1 = key1; t.sub = sub;
    const uint sframe = b.w[SXC_H_FRAME];
    bool inserted, full;
    const uint e = sxcFind(b, key0, key1, true, sframe, inserted, full);
    sxcStat(b, SXC_S_ELIG, true);
    sxcStat(b, SXC_S_INSERT, inserted);
    sxcStat(b, SXC_S_FULL, full);
    if (e == 0xFFFFFFFFu)
    {
        t.cls = SXC_C_NOCELL;
        sxcStat(b, SXC_S_NOCELL, true);
        return t;
    }
    t.e = e;
    b.w[e + SXC_E_TOUCH] = sframe;   // (LRU: every looking texel writes the same value)
    const uint fl = b.w[e + SXC_E_FLAGS];
    const bool epochOk = (fl >> 16u) == (b.w[SXC_H_EPOCH] & 0xFFFFu);
    const bool committed = epochOk && (fl & SXC_F_COMMIT) != 0u && (fl & SXC_F_REVOKE) == 0u;
    t.certNow = committed && sxcBinsCertified(b, b.w[e + SXC_E_BLOCK], b.w[e + SXC_E_CB]);
    const bool covered = ((b.w[e + SXC_E_COV] >> sub) & 1u) != 0u;
    const bool fresh = sframe - b.w[e + SXC_E_CFRAME] < b.w[SXC_H_MAXAGE];
    // a round in progress: the FIRST texel of every sub-cell the round has not seen traces its
    // certification rays (coverage: every visible part of the cell certifies in one frame), plus up
    // to SXC_H_TICKETS more texels a frame (a quarter of the texels ask: fewer atomics on a big cell)
    if (epochOk && (fl & SXC_F_ROUND) != 0u)
    {
        const uint bit = 1u << sub;
        if ((b.w[e + SXC_E_RCOV] & bit) == 0u) t.ticket = (atomicOr(b.w[e + SXC_E_RCOV], bit) & bit) == 0u;
        if (!t.ticket && (sxcHash(uint(m.x) * 7919u + uint(m.y) * 104729u + frame) & 3u) == 0u)
            t.ticket = atomicAdd(b.w[e + SXC_E_TICKET], 1u) < b.w[SXC_H_TICKETS];
    }
    if (t.certNow && !covered)
    {
        // a part of the cell its round never saw: trace, and ask for a round that covers it
        if ((fl & SXC_F_NEED) == 0u) atomicOr(b.w[e + SXC_E_FLAGS], SXC_F_NEED);
        t.cls = SXC_C_UNCOV;
        sxcStat(b, SXC_S_UNCOV, true);
        return t;
    }
    if (!(t.certNow && fresh))
    {
        t.cls = SXC_C_NOCERT;
        sxcStat(b, SXC_S_NOCERT, true);
        return t;
    }
    if (t.ticket)
    {
        // a certifying texel of a renewal round: traced (its rays need the sun sample's origin)
        t.cls = SXC_C_CERT;
        sxcStat(b, SXC_S_CERTTX, true);
        return t;
    }
    const uint spot = b.w[SXC_H_SPOT];
    if ((sxcHash(uint(m.x) * 1973u + uint(m.y) * 9277u + frame * 26699u + 0x5bd1e995u) & spot) == 0u)
    {
        t.cls = SXC_C_SPOT;
        sxcStat(b, SXC_S_SPOT, true);
        return t;
    }
    // SKIP: exactly a fully blocked sun sample (pass 15's isSun tail with vis = 0)
    t.cls = SXC_C_SKIP;
    t.out_ = mix(vec3(1.0), clamp(mix(vec3(0.0), vec3(1.0), fade), vec3(0.0), vec3(1.0)), clamp(sw, 0.0, 1.0));
    const float bt = uintBitsToFloat(b.w[e + SXC_E_BT]);
    t.srad = (bt > 0.0 && bt < 1e29) ? bt * tan(ssr.sun_rt.x) / max(ngl, 0.15) / pxw * texPerPx : 0.0;
    sxcStat(b, SXC_S_SKIP, true);
    return t;
}

// Revoke the entry of keys (key0, key1) if it exists (no insert).
void sxcRevokeKey(SxcBuf b, uint key0, uint key1)
{
    bool ins, full;
    const uint e = sxcFind(b, key0, key1, false, 0u, ins, full);
    if (e != 0xFFFFFFFFu) atomicOr(b.w[e + SXC_E_FLAGS], SXC_F_REVOKE);
}
// The 6 face neighbours of a cell (same level and bucket).
void sxcRevokeNeighbours(SxcBuf b, uint key0, uint key1)
{
    const uint x = (key0 >> 15u) & 0x7FFFu, y = key0 & 0x7FFFu, z = (key1 >> 6u) & 0x7FFFu;
    const uint lo0 = key0 & 0x80000000u, lo1 = key1 & ~(0x7FFFu << 6u);
    for (uint i = 0u; i < 6u; ++i)
    {
        const int d = (i & 1u) != 0u ? 1 : -1;
        const uint ax = i >> 1u;
        const int nx = int(x) + (ax == 0u ? d : 0), ny = int(y) + (ax == 1u ? d : 0), nz = int(z) + (ax == 2u ? d : 0);
        if (nx < 0 || ny < 0 || nz < 0 || nx > 32767 || ny > 32767 || nz > 32767) continue;
        sxcRevokeKey(b, lo0 | (uint(nx) << 15u) | uint(ny), lo1 | (uint(nz) << 6u));
    }
}
// A certification ray's hit is a STATIC caster: the moving-caster rules (clean_rt_moving_caster.glsl
// rtNoteBlocker) inverted -- not rigged / attached, not spinning, its row unchanged since last frame.
bool sxcStaticCaster(uint id)
{
    if (id >= uint(rt_instances.length())) return false;
    if ((rt_instances[id].flags & (1u | 512u)) != 0u) return false;
    if (dot(rt_instances[id].spin_axis.xyz, rt_instances[id].spin_axis.xyz) > 0.0) return false;
    if (id >= uint(prev_instances.length())) return true;
    const mat4 a = rt_instances[id].model_matrix, p = prev_instances[id].model_matrix;
    return all(equal(a[0].xyz, p[0].xyz)) && all(equal(a[1].xyz, p[1].xyz))
        && all(equal(a[2].xyz, p[2].xyz)) && all(equal(a[3].xyz, p[3].xyz));
}
// One certification ray from origin o along d (static opaque + terrain, first hit): the hit
// distance when a static caster blocks it, else -1 (open or inconclusive).
float sxcCertRay(vec3 o, vec3 d, float tmax)
{
    float tmin = 0.0;
    if (!rtRaySane(o, tmin, d, tmax)) return -1.0;   // (Xid 109: an invalid ray is inconclusive)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          (RT_MASK_OPAQUE | RT_MASK_TERRAIN) & 0xFFu, o, tmin, d, tmax);
    while (rayQueryProceedEXT(rq)) {}
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return -1.0;
    const uint id = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                  uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)));
    return sxcStaticCaster(id) ? rayQueryGetIntersectionTEXT(rq, true) : -1.0;
}

// AFTER the sun sample was traced (vis: shadowSampleVis' raw visibility, before the fades), while
// shadowRecvLight's globals still hold (rt_cl_lift): revocation by a lit ray, then this texel's
// certification rays (from the sun rays' own origin rule: rtReceiverOrigin of the sample's origin
// point Pin on the facet Nin, plus the sample's bias and the rig / slope lift).
void sxcSunPost(SxcTexel t, vec3 vis, vec3 Pin, vec3 Nin, vec3 cam, float bias, uint frame)
{
    if (t.cls == SXC_C_OFF || t.cls == SXC_C_SKIP || t.e == 0xFFFFFFFFu) return;
    SxcBuf b = sxcBuf();
    const bool lit = max(max(vis.r, vis.g), vis.b) > 0.02;
    // a LIT ray where the cell claims the sun's bins are blocked: the certificate was wrong (a gap
    // narrower than the certification rays' spacing, a change no sphere reported) -- revoke the
    // cell and its neighbours (the maintenance kernel resets them and backs their next round off)
    const bool revoke = lit && t.certNow;
    sxcStat(b, SXC_S_SPOTLIT, lit && t.cls == SXC_C_SPOT);
    sxcStat(b, SXC_S_REVLIT, revoke);
    if (revoke)
    {
        atomicOr(b.w[t.e + SXC_E_FLAGS], SXC_F_REVOKE);
        sxcRevokeNeighbours(b, t.key0, t.key1);
    }
    if (!t.ticket) return;
    // CERTIFICATION: one ray per bin of the round (at most 8; a longer look-ahead strides the bins
    // over the round's tickets), wedge corners first, then the R2 interior
    const uint nb = max(b.w[SXC_H_NB], 1u);
    const uint n = min(nb, 8u);
    const uint rs = b.w[t.e + SXC_E_RS];
    const uint seed = b.w[t.e + SXC_E_RFRAME];
    const uint i0 = atomicAdd(b.w[t.e + SXC_E_RRAYS], n);
    const bool cone = uintBitsToFloat(b.w[SXC_H_CONE]) > 0.0;
    const float tmax = uintBitsToFloat(b.w[SXC_H_TMAX]);
    const vec3 Nr = rtVisibleSide(Nin, Pin, cam);
    const vec3 o = rtReceiverOrigin(Pin, Nr, length(Pin - cam), bias + rt_cl_lift);
    uint open = 0u, nblock = 0u;
    float tmin = 1e30;
    for (uint j = 0u; j < n; ++j)
    {
        const uint idx = i0 + j;
        const uint k = idx % nb;
        const vec3 d = sxcWedgeDir(b, rs + k, sxcPattern(idx / nb, seed, cone));
        // a wedge direction grazing / behind the receiver's facet: its own surface would "block" it
        // -- inconclusive (open): the real trace there is the terminator band's, never skipped
        float th = -1.0;
        if (dot(Nr, d) > 0.035) th = sxcCertRay(o, d, tmax);
        if (th < 0.0) open |= 1u << k;
        else { tmin = min(tmin, th); ++nblock; }
    }
    if (open != 0u) atomicOr(b.w[t.e + SXC_E_ROPEN], open);
    if (tmin < 1e29) atomicMin(b.w[t.e + SXC_E_RBT], floatBitsToUint(tmin));
    const uint nr = subgroupAdd(n), nbk = subgroupAdd(nblock);
    if (subgroupElect())
    {
        atomicAdd(b.w[SXC_STATS + SXC_S_CRAYS], nr);
        atomicAdd(b.w[SXC_STATS + SXC_S_CBLOCK], nbk);
    }
}
// (#1039, RenderVKVisQueue bit 8) sxcSunPost in its two halves for the visibility queue's shadow core: the
// CERTIFICATION rays need no visibility, so the producer runs them (with Pin / Nin / the bias it holds; same
// atomics, order-free); the REVOCATION needs the walk's visibility, so the walk kernel runs it (the keys from
// the entry: a committed entry -- certNow -- holds exactly the keys the lookup matched). The same lines as above:
// keep the three in step.
void sxcSunRevoke(SxcTexel t, vec3 vis)
{
    if (t.cls == SXC_C_OFF || t.cls == SXC_C_SKIP || t.e == 0xFFFFFFFFu) return;
    SxcBuf b = sxcBuf();
    const bool lit = max(max(vis.r, vis.g), vis.b) > 0.02;
    const bool revoke = lit && t.certNow;
    sxcStat(b, SXC_S_SPOTLIT, lit && t.cls == SXC_C_SPOT);
    sxcStat(b, SXC_S_REVLIT, revoke);
    if (revoke)
    {
        atomicOr(b.w[t.e + SXC_E_FLAGS], SXC_F_REVOKE);
        sxcRevokeNeighbours(b, b.w[t.e + SXC_E_KEY0], b.w[t.e + SXC_E_KEY1]);
    }
}
void sxcSunCertify(SxcTexel t, vec3 Pin, vec3 Nin, vec3 cam, float bias, uint frame)
{
    if (t.cls == SXC_C_OFF || t.cls == SXC_C_SKIP || t.e == 0xFFFFFFFFu || !t.ticket) return;
    SxcBuf b = sxcBuf();
    const uint nb = max(b.w[SXC_H_NB], 1u);
    const uint n = min(nb, 8u);
    const uint rs = b.w[t.e + SXC_E_RS];
    const uint seed = b.w[t.e + SXC_E_RFRAME];
    const uint i0 = atomicAdd(b.w[t.e + SXC_E_RRAYS], n);
    const bool cone = uintBitsToFloat(b.w[SXC_H_CONE]) > 0.0;
    const float tmax = uintBitsToFloat(b.w[SXC_H_TMAX]);
    const vec3 Nr = rtVisibleSide(Nin, Pin, cam);
    const vec3 o = rtReceiverOrigin(Pin, Nr, length(Pin - cam), bias + rt_cl_lift);
    uint open = 0u, nblock = 0u;
    float tmin = 1e30;
    for (uint j = 0u; j < n; ++j)
    {
        const uint idx = i0 + j;
        const uint k = idx % nb;
        const vec3 d = sxcWedgeDir(b, rs + k, sxcPattern(idx / nb, seed, cone));
        float th = -1.0;
        if (dot(Nr, d) > 0.035) th = sxcCertRay(o, d, tmax);
        if (th < 0.0) open |= 1u << k;
        else { tmin = min(tmin, th); ++nblock; }
    }
    if (open != 0u) atomicOr(b.w[t.e + SXC_E_ROPEN], open);
    if (tmin < 1e29) atomicMin(b.w[t.e + SXC_E_RBT], floatBitsToUint(tmin));
    const uint nr = subgroupAdd(n), nbk = subgroupAdd(nblock);
    if (subgroupElect())
    {
        atomicAdd(b.w[SXC_STATS + SXC_S_CRAYS], nr);
        atomicAdd(b.w[SXC_STATS + SXC_S_CBLOCK], nbk);
    }
}
// RenderVKSunCertDebug: 1 = what SXC decided (green skipped, cyan spot check, orange traced in a
// certified cell's uncovered part, blue traced uncertified, yellow + certifying rays this frame,
// magenta no entry, grey not considered, red a lit ray revoked the cell); 2 = certificate age
// (green new -> red at RenderVKSunCertMaxAge; blue none). Written as the sun visibility (the
// colour shows where the sun's light reaches the surface; the denoiser smooths it a little).
vec3 sxcDebug(SxcTexel t, vec3 sun, vec3 vis)
{
    if (!sxcOn()) return sun;
    SxcBuf b = sxcBuf();
    const uint mode = b.w[SXC_H_DEBUG];
    if (mode == 0u) return sun;
    if (mode == 2u)
    {
        if (t.e == 0xFFFFFFFFu || !t.certNow) return vec3(0.0, 0.0, 1.0);
        const float age = float(b.w[SXC_H_FRAME] - b.w[t.e + SXC_E_CFRAME]) / float(max(b.w[SXC_H_MAXAGE], 1u));
        return mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), clamp(age, 0.0, 1.0));
    }
    if (t.certNow && t.cls != SXC_C_SKIP && max(max(vis.r, vis.g), vis.b) > 0.02) return vec3(1.0, 0.0, 0.0);
    if (t.ticket) return vec3(1.0, 1.0, 0.0);
    switch (t.cls)
    {
    case SXC_C_SKIP:   return vec3(0.0, 1.0, 0.0);
    case SXC_C_SPOT:   return vec3(0.0, 1.0, 1.0);
    case SXC_C_UNCOV:  return vec3(1.0, 0.5, 0.0);
    case SXC_C_NOCERT: return vec3(0.0, 0.0, 1.0);
    case SXC_C_NOCELL: return vec3(1.0, 0.0, 1.0);
    default:           return vec3(0.25);
    }
}
#endif   // !SXC_MAINTENANCE

#endif
