// <GMDX> REZ EFFECTS (indra/newview/gmdxrezfx.cpp; Preferences > Rez Effects). What objects do
// when they appear (rez-in) and when they are removed (de-rez).
//
// An object in an effect carries instance flag bits 23..31 = its slot in the effect table + 1
// (binding 45, rewritten by the CPU every frame while effects run), and bit 22 while it is
// DYING: a killed object drawn only for its de-rez -- the cull keeps it out of shadow views and
// probe captures, ray tracing and the GPU pick skip it. Objects that start an effect on the
// same frame share a slot.
//
// FAIRNESS (a combat viewer): rez-in effects never move a vertex, and never discard -- the
// object is whole, solid and in place from its first frame; they only paint its surface. The one
// exception is the HIDDEN MODE of the two build effects (3D printer, transporter): they may hide
// what is not built yet, which is their real look. Their objects take a slot from the upper half
// of the table (slot >= 256: instance bit 31, REZFX_HIDE_POOL), which the main view's cull routes
// into the mask buckets, so the depth pre-pass runs the same cut (rezfxHidden). On damage-enabled
// land with GMDXRezFxCombatSafe on they start in the lower half instead and stay solid throughout,
// paint only; the table's wind.x says per frame whether a hidden-mode slot may hide right now.
// De-rez effects cut a dead object away (fragment discards, vertex collapse); its rows run in
// the mask buckets, so the depth pre-pass (clean_prepass_*.frag) runs the same cut on the same
// invariant inputs as the lit FS.
//
// Includers:
//   VS (clean_draw.vert, clean_draw_rigged.vert): define GMDX_REZFX_VS -- rezfxDisplace (every
//     variant: the position chain) and rezfxOutputs (the full and MASK variants: v_rez /
//     v_rez_box at locations 15 / 16).
//   FS (clean_draw.frag, the mask pre-pass FSs): define GMDX_REZFX_FS -- the inputs,
//     rezfxDiscard (the de-rez cut) and, in clean_draw.frag, rezfxSurface (the paint).
#ifndef GMDX_REZFX_GLSL
#define GMDX_REZFX_GLSL

#define REZFX_DYING       0x00400000u
#define REZFX_SLOT_SHIFT  23u
#define REZFX_HIDE_POOL   0x80000000u   // slot >= 256: a build effect that may hide (gmdxrezfx.cpp)
#define REZFX_PRINT_POOL  0xC0000000u   // slot >= 384: of those, the 3D printer (double-sided: its cut is capped)
// Effect ids (gmdxrezfx.h GMDXRezFx::Effect).
#define REZFX_TRON         1u
#define REZFX_FIRE         2u
#define REZFX_HOLOGRAM     3u
#define REZFX_VOXEL        4u
#define REZFX_PRINTER      5u
#define REZFX_TRANSPORTER  6u
#define REZFX_EMBER        8u
#define REZFX_DISINTEGRATE 9u
#define REZFX_ASH         10u
#define REZFX_GLITCH      11u
#define REZFX_IMPLODE     12u
#define REZFX_TRON_OUT    13u
#define REZFX_SHATTER     14u
#define REZFX_DEREZ_FIRST  8u

// One table row (gmdxrezfx.h GMDXRezFx::Slot, 80 B).
struct RezFxSlot {
    uint  effect;     // REZFX_* (0: unused)
    uint  stage;      // Tron: 0 wireframe scan (holds at progress 1 while textures load), 1 fill
    float progress;   // 0..1 through the stage
    float time;       // seconds since the stage began
    vec4  colour;     // linear rgb glow colour (Tron, Tron de-rez: a = glow strength)
    vec4  wind;       // ash: unit drift direction (agent space), w strength 0..1; Tron, Tron de-rez:
                      // x line width; printer / transporter: x 1 = may hide now, y the printer's
                      // layer count; every effect but ash: z the effect's length (s); rez-ins:
                      // w the hold for textures (GMDXRezFxWaitForTextures) -- Tron, voxel: its idle
                      // loop 0..1; the printer: keep-warm (1 until its final layers, then cooling)
    vec4  aux0;       // printer: its linkset's (avatar's) box centre in the draw frame, w 1 = print
                      // in that frame (0: each prim in its own); shatter: xyz the unit direction of
                      // the impact (toward the camera when it died)
    vec4  aux1;       // printer: that box's half extents (m), w the infill rows per layer
};
layout(std430, set = 0, binding = 56) readonly buffer RezFxTable { RezFxSlot rezfx_slots[]; };

// The slot of an instance's flags (0 = none) and whether the table holds it.
uint rezfxSlotOf(uint flags) { return flags >> REZFX_SLOT_SHIFT; }
bool rezfxSlotOk(uint s) { return s != 0u && s <= uint(rezfx_slots.length()); }
// A natively opaque row in the mask buckets only for a build effect's cut (clean_cull.comp): no
// alpha test (the lit EQUAL FS tests flag-4 rows only, as in its own opaque bucket).
bool rezfxRerouted(uint flags) { return (flags & (REZFX_HIDE_POOL | REZFX_DYING | 6u)) == REZFX_HIDE_POOL; }

// ---- noise (Vulkanstorm's rez.glsl, after Crystal Frost's de-rez) ----
// The de-rez cuts must come out bit-identical in the pre-pass and the lit FS modules: every helper
// a cut calls returns a `precise` result (glslang pins only the computations inside the function
// that declares the precise variable, so each helper pins its own).
float rzHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    precise float r = fract((p.x + p.y) * p.z);
    return r;
}
vec3 rzHash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    precise vec3 r = fract((p.xxy + p.yxx) * p.zyx);
    return r;
}
float rzNoise(vec3 p)
{
    const vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    precise float r = mix(mix(mix(rzHash13(i), rzHash13(i + vec3(1, 0, 0)), f.x),
                              mix(rzHash13(i + vec3(0, 1, 0)), rzHash13(i + vec3(1, 1, 0)), f.x), f.y),
                          mix(mix(rzHash13(i + vec3(0, 0, 1)), rzHash13(i + vec3(1, 0, 1)), f.x),
                              mix(rzHash13(i + vec3(0, 1, 1)), rzHash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
    return r;
}
// Vulkanstorm's two-octave surface noise, stretched to 0..1.
float rzBurnNoise(vec3 p)
{
    precise float r = clamp((0.65 * rzNoise(p * 2.5) + 0.35 * rzNoise(p * 7.0) - 0.2) / 0.6, 0.0, 1.0);
    return r;
}

// ---- de-rez shapes shared by the vertex and fragment sides ----
// Glitch: 14 jumps a second; the object-space slab height its blocks jump by (m).
float rzGlitchStep(float t) { precise float r = floor(t * 14.0); return r; }
float rzGlitchSlab(float radius) { precise float r = clamp(radius * 0.25, 0.06, 1.0); return r; }
// DISINTEGRATE: object space in cubes (about a twelfth of the object, 3 cm to 1.5 m). A cube
// breaks loose at its moment (rzCubeTime: a wave down from the top, shuffled; the last at 0.78),
// then flies off spinning (rezfxDisplace) until the end.
float rzCubeSize(float radius) { precise float r = clamp(radius * 0.09, 0.03, 1.5); return r; }
// The cube grid is offset by RZ_CUBE_OFS of a cube, so a face at a round size never lies on a
// cube boundary (where the cell would flip pixel by pixel).
#define RZ_CUBE_OFS 0.371
// The cube of object point q (m), and when it breaks loose; sz = the object's z size (m).
float rzCubeTimeAt(vec3 q, float radius, float sz, out vec3 cell)
{
    precise float vs = rzCubeSize(radius);
    cell = floor(q / vs + RZ_CUBE_OFS);
    precise float hz = clamp(((cell.z + 0.5 - RZ_CUBE_OFS) * vs) / max(sz, 0.01) + 0.5, 0.0, 1.0);
    precise float r = 0.1 + 0.5 * (1.0 - hz) + 0.18 * rzHash13(cell + 5.0);
    return r;
}
// SHATTER: Voronoi shards (about a third of the object across, 5 cm to 3 m) in the WORLD-oriented
// frame round the object's centre (gravity is -z). The impact point is where the line from the
// centre toward the camera (when it died) leaves the object's box -- on the surface that faced the
// camera (the top of a rigged mesh). It flashes, a shockwave ring runs over the surface, cracks
// spider out from it by RZ_CRACK_END, and the shards break loose just behind the cracks and are
// blasted away from it (the nearer, the faster), spinning, then fall; they shrink away at the end.
// TUNABLES (progress is 0..1 over the effect, 1.2 s at the default duration). The ones marked *
// have a CPU copy for the debris (gmdxrezfx.cpp kSh*): change both.
#define RZ_CRACK_END     0.1    // * the cracks reach the far side (spread speed)
#define RZ_LOOSE_LAG     0.03   // * a shard breaks loose this long after the cracks reach it...
#define RZ_LOOSE_JITTER  0.05   // * ...plus up to this much, at random
#define RZ_SHARD_FRAC    0.3    // * shard size, of the bounding radius (5 cm .. 3 m)
#define RZ_BURST_BASE    2.0    // * burst speed at the impact (m/s): BASE + PER_M x radius, ..
#define RZ_BURST_PER_M   2.2    // *
#define RZ_BURST_MAX     14.0   // * .. at most MAX (a 0.5 m pane 2.8, a 2 m crate 5.8, a 10 m wall 14)
#define RZ_BURST_FAR     0.25   // * the far side's share of that speed (falloff with distance)
#define RZ_BURST_DRAG    2.0    // * 1/s: the burst slows, a shard travels at most speed / DRAG
#define RZ_SPALL         0.25   // * share of shards thrown back toward the camera (the rest away)
#define RZ_SPIN_MIN      6.0    // * spin (rad/s): MIN + RANGE at random, slower far from the impact
#define RZ_SPIN_RANGE    18.0   // *
#define RZ_SHRINK_START  0.7    // * every shard shrinks to nothing from here to the end
#define RZ_FLASH_DECAY   0.03   // the impact flash dies off (progress for 1/e)
#define RZ_FLASH_SIZE    0.35   // its radius, of the bounding radius (4 cm .. 1.5 m)
#define RZ_RING_END      0.07   // the shockwave ring has crossed the object
#define RZ_CRACK_FLARE   0.06   // a crack flares as it opens, settling over this (progress)
#define RZ_RADIAL_CRACKS 7.0    // the straight cracks of the star round the impact
#define RZ_GLINT         14.0   // the glint of a spinning shard (HDR)
float rzShardSize(float radius) { precise float r = clamp(radius * RZ_SHARD_FRAC, 0.05, 3.0); return r; }
// How far the impact point is from the centre along dir (unit, toward the camera): where that line
// leaves the object's box (B: its scaled axes, the model matrix's).
float rzImpactDist(vec3 dir, mat3 B)
{
    precise vec3 l2 = vec3(dot(B[0], B[0]), dot(B[1], B[1]), dot(B[2], B[2]));
    precise vec3 a = max(abs(vec3(dot(dir, B[0]), dot(dir, B[1]), dot(dir, B[2]))), vec3(1e-6));
    precise vec3 s = 0.5 * l2 / a;
    precise float r = min(s.x, min(s.y, s.z));
    return r;
}
// The burst: a shard at a (from the impact) is launched, its velocity (m/s, world-oriented), and
// spins at spin (rad/s). n: the impact direction; norm: the farthest a point can be from the
// impact (radius + its distance from the centre). Out along the surface from the impact, and away
// through the object (RZ_SPALL of them back toward the camera), a little up.
vec3 rzShardLaunch(vec3 a, vec3 n, vec3 r3, float norm, float radius, out float spin)
{
    const float dn = clamp(length(a) / max(norm, 1e-3), 0.0, 1.0);
    const float f = mix(RZ_BURST_FAR, 1.0, (1.0 - dn) * (1.0 - dn));
    const vec3 lat = a - n * dot(a, n);
    const vec3 d = normalize(lat / max(length(lat), 1e-4) + n * (r3.y < RZ_SPALL ? 0.5 : -0.6) + vec3(0.0, 0.0, 0.15));
    spin = (RZ_SPIN_MIN + RZ_SPIN_RANGE * r3.z) * (0.4 + 0.6 * f);
    return d * (clamp(RZ_BURST_BASE + RZ_BURST_PER_M * radius, RZ_BURST_BASE, RZ_BURST_MAX) * f * (0.6 + 0.8 * r3.x));
}
// Where a launched shard is dt s on (velocity v): the burst, slowed by drag, and gravity.
vec3 rzShardFlight(vec3 v, float dt)
{
    return v * ((1.0 - exp(-RZ_BURST_DRAG * dt)) / RZ_BURST_DRAG) + vec3(0.0, 0.0, -4.9 * dt * dt);
}
// The shard of point q: its seed (q's frame), its cell id; edge = the distance to the nearest other
// shard's border (m, the crack lines). 27 cells.
vec3 rzShardOf(vec3 q, float cs, out vec3 id, out float edge)
{
    const vec3 g = floor(q / cs);
    float d1 = 1e30, d2 = 1e30;
    vec3 s1 = vec3(0.0);
    id = g;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                precise vec3 c = g + vec3(float(x), float(y), float(z));
                precise vec3 sd = (c + 0.15 + 0.7 * rzHash33(c + 3.0)) * cs;
                precise vec3 dv = q - sd;
                precise float d = dot(dv, dv);
                if (d < d1) { d2 = d1; d1 = d; s1 = sd; id = c; }
                else if (d < d2) d2 = d;
            }
    edge = 0.5 * (sqrt(d2) - sqrt(d1));
    return s1;
}
// When a shard (seed sd, cell id) breaks loose: just after the cracks reached it.
float rzShardTime(vec3 sd, vec3 id, vec3 impact, float radius)
{
    precise float r = RZ_CRACK_END * clamp(distance(sd, impact) / max(radius + length(impact), 1e-3), 0.0, 1.0)
                    + RZ_LOOSE_LAG + RZ_LOOSE_JITTER * rzHash13(id + 29.0);
    return r;
}
// TRON DE-REZ: the second front (lines winking out, top down) -- the height above which nothing
// is left, a little ragged per object-space cell; RZ_TOUT_SCAN the first front's end.
#define RZ_TOUT_SCAN 0.42
float rzToutGone(float p, vec3 cell)
{
    precise float r = mix(1.12, -0.04, clamp((p - 0.58) / 0.38, 0.0, 1.0)) - 0.1 * rzHash13(cell + 61.0);
    return r;
}
float rzToutCell(float radius) { precise float r = clamp(radius * 0.06, 0.02, 0.6); return r; }
// Rotates v about the unit axis a by angle w (Rodrigues).
vec3 rzRotate(vec3 v, vec3 a, float w)
{
    const float c = cos(w), s = sin(w);
    return v * c + cross(a, v) * s + a * (dot(a, v) * (1.0 - c));
}
// Ash: where a point is along the burn (0 the upwind side .. 1 downwind, with noise) and the cut.
float rzAshValue(vec3 lp, float sweep) { precise float r = 0.55 * sweep + 0.45 * rzBurnNoise(lp * 1.3); return r; }
float rzAshCut(float p) { precise float r = mix(-0.05, 1.05, p); return r; }

#ifdef GMDX_REZFX_VS
// ---- vertex side ----
// The de-rez vertex effects, in object space (lpos: before the model matrix M; volumes live in
// the unit cube). Every VS variant runs this on the same source lines, so the depth pre-pass
// and the lit pass place every vertex alike. Rez-in effects never move a vertex.
vec3 rezfxDisplace(uint flags, vec3 lpos, mat4 M)
{
    if ((flags & REZFX_DYING) == 0u) return lpos;
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return lpos;
    const RezFxSlot z = rezfx_slots[s - 1u];
    const float p = z.progress;
    if (z.effect == REZFX_IMPLODE)
    {
        // IMPLODE: everything falls in on the object's centre, twisting as it goes, holds as a
        // small hot core through the flash (rezfxPaint) and is gone.
        const float q = min(p / 0.8, 1.0);
        const float shrink = p < 0.8 ? mix(1.0, 0.12, q * q) : 0.12 * (1.0 - smoothstep(0.88, 1.0, p));
        const float a = 2.5 * q * q;
        const float ca = cos(a), sa = sin(a);
        return vec3(ca * lpos.x - sa * lpos.y, sa * lpos.x + ca * lpos.y, lpos.z) * shrink;
    }
    const vec3 sc = max(vec3(length(M[0].xyz), length(M[1].xyz), length(M[2].xyz)), vec3(1e-4));
    if (z.effect == REZFX_GLITCH)
    {
        // GLITCH OUT: horizontal slabs of the object jump sideways, a new pattern 14 times a
        // second, more of them as it goes (triangles across two slabs tear).
        const float slab = rzGlitchSlab(0.5 * length(sc));
        const float band = floor(lpos.z * sc.z / slab);
        const float st = rzGlitchStep(z.time);
        const float k = smoothstep(0.0, 0.4, p);
        if (rzHash13(vec3(band, st, 3.0)) > 1.0 - 0.55 * k)
        {
            const vec2 off = (rzHash33(vec3(band, st, 9.0)).xy - 0.5) * (0.12 + 0.25 * slab) * k;
            lpos.xy += off / sc.xy;
        }
        return lpos;
    }
    if (z.effect == REZFX_ASH)
    {
        // ASH ON THE WIND: what is burning off drifts away downwind (z.wind: the region's wind
        // where the camera is), lifting a little; the rest follows slowly. Just ahead of the cut
        // the edge curls: it lifts outward and up, as burning paper does.
        const vec3 axis = z.wind.xyz;
        const float e = 0.5 * (abs(dot(M[0].xyz, axis)) + abs(dot(M[1].xyz, axis)) + abs(dot(M[2].xyz, axis)));
        const vec3 w = mat3(M) * lpos;
        const float sweep = (dot(w, axis) + e) / max(2.0 * e, 1e-3);
        const float v = rzAshValue(lpos * sc, sweep);
        const float near = 1.0 - smoothstep(0.0, 0.25, v - rzAshCut(p));   // 1 at the cut
        const float radius = 0.5 * length(sc);
        const float amt = z.wind.w * (0.15 * p * p + 0.6 * near * p) * (0.4 + 0.3 * length(sc));
        const float curl = near * near * near * clamp(0.1 * radius, 0.01, 0.6);
        const vec3 outw = normalize(w + vec3(0.0, 0.0, 1e-4));
        return lpos + inverse(mat3(M)) * (axis * amt + (outw * 0.7 + vec3(0.0, 0.0, 1.0)) * curl);
    }
    if (z.effect == REZFX_DISINTEGRATE)
    {
        // DISINTEGRATE: a vertex moves with its cube once that has broken loose -- out from the
        // object's centre and a little up, spinning, under some gravity, shrinking to nothing by
        // the end (z.wind.z: the effect's length, s). A triangle wholly in one cube flies as a
        // piece; one across cubes would stretch, and the FS hides it (rezfxDiscard).
        const float radius = 0.5 * length(sc);
        const vec3 q = lpos * sc;
        vec3 cell;
        const float tc = rzCubeTimeAt(q, radius, sc.z, cell);
        if (p <= tc) return lpos;
        const float len = max(z.wind.z, 0.05);
        const float dt = (p - tc) * len;
        const float life = max((1.0 - tc) * len, 1e-3);
        const float vs = rzCubeSize(radius);
        const vec3 cc = (cell + 0.5 - RZ_CUBE_OFS) * vs;
        const vec3 r3 = rzHash33(cell + 19.0);
        const vec3 ax = normalize(r3 - 0.5 + vec3(1e-3, 0.0, 0.0));
        const float shrink = 1.0 - smoothstep(0.3, 1.0, dt / life);
        const vec3 piece = rzRotate(q - cc, ax, dt * (4.0 + 9.0 * r3.x)) * shrink;
        const vec3 outw = normalize(mat3(M) * (cc / sc) + vec3(0.0, 0.0, 0.3 * radius) + vec3(1e-4));
        const float speed = (0.7 + 1.5 * r3.y) * clamp(0.5 + 0.45 * radius, 0.6, 6.0);
        const vec3 fly = outw * (speed * dt) + vec3(0.0, 0.0, -3.0 * dt * dt);
        return (cc + piece) / sc + inverse(mat3(M)) * fly;
    }
    if (z.effect == REZFX_SHATTER)
    {
        // SHATTER: a vertex moves with its shard once that has broken loose: blasted away from
        // the impact (rzShardLaunch), spinning, slowing and falling; every shard shrinks away
        // from RZ_SHRINK_START.
        const float radius = 0.5 * length(sc);
        const mat3 B = mat3(M);
        const vec3 q = B * lpos;   // world-oriented, round the centre (m)
        const vec3 n = z.aux0.xyz;
        const vec3 impact = n * rzImpactDist(n, B);
        vec3 id;
        float edge;
        const vec3 sd = rzShardOf(q, rzShardSize(radius), id, edge);
        const float tc = rzShardTime(sd, id, impact, radius);
        if (p <= tc) return lpos;
        const float dt = (p - tc) * max(z.wind.z, 0.05);
        const vec3 r3 = rzHash33(id + 37.0);
        const vec3 ax = normalize(r3 - 0.5 + vec3(0.0, 1e-3, 0.0));
        const float shrink = 1.0 - smoothstep(RZ_SHRINK_START, 1.0, p);
        float spin;
        const vec3 v = rzShardLaunch(sd - impact, n, r3, radius + length(impact), radius, spin);
        const vec3 piece = rzRotate(q - sd, ax, dt * spin) * shrink;
        return inverse(B) * (sd + piece + rzShardFlight(v, dt));
    }
    return lpos;
}

// v_rez:     xyz = object space in metres (the effects' patterns live there, so they stay put on
//                  a moving object and run on across its faces), w = the sweep coordinate 0..1
//                  (world up from the object's bottom; ash: along the wind).
// v_rez_box: x = the sweep extent (m), y = the bounding radius (m), z = the object's own z size
//            (m: v_rez.z runs over +-z/2), w = 1 (0: no effect).
// The BUILD EFFECTS (3D printer, transporter) work in the world frame instead: v_rez.xyz = the
// offset from the centre along world x / y / z (m), v_rez_box.yz = the bounds' half widths along
// world x / y (the footprint the nozzle crosses, the beam's column).
// Object bounds: the volume's unit cube (every prim, sculpt and mesh volume lives in it), not the
// face's own box -- one scan line for the whole object.
void rezfxOutputs(uint flags, vec3 lpos, mat4 M, out vec4 rez, out vec4 box)
{
    rez = vec4(0.0);
    box = vec4(0.0);
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return;
    const RezFxSlot z = rezfx_slots[s - 1u];
    const vec3 sc = vec3(length(M[0].xyz), length(M[1].xyz), length(M[2].xyz));
    const vec3 axis = z.effect == REZFX_ASH ? z.wind.xyz : vec3(0.0, 0.0, 1.0);
    const float e = 0.5 * (abs(dot(M[0].xyz, axis)) + abs(dot(M[1].xyz, axis)) + abs(dot(M[2].xyz, axis)));
    const float d = dot(mat3(M) * lpos, axis);   // along the axis from the centre (M[3] = the centre)
    rez = vec4(lpos * sc, (d + e) / max(2.0 * e, 1e-3));
    box = vec4(2.0 * e, 0.5 * length(sc), sc.z, 1.0);
    if (z.effect == REZFX_PRINTER && z.aux0.w > 0.5 && (flags & 512u) == 0u)
    {
        // The printer in its linkset's frame (aux0 / aux1, the CPU's box this frame): one print
        // line, one nozzle for the whole build.
        rez.xyz = mat3(M) * lpos + (M[3].xyz - z.aux0.xyz);
        rez.w = (rez.z + z.aux1.z) / max(2.0 * z.aux1.z, 1e-3);
        box.xyz = vec3(2.0 * z.aux1.z, z.aux1.xy);
    }
    else if (z.effect == REZFX_PRINTER || z.effect == REZFX_TRANSPORTER)
    {
        rez.xyz = mat3(M) * lpos;
        box.yz = 0.5 * vec2(abs(M[0].x) + abs(M[1].x) + abs(M[2].x), abs(M[0].y) + abs(M[1].y) + abs(M[2].y));
    }
    else if (z.effect == REZFX_DISINTEGRATE)
    {
        // w and box.x: the vertex's cube time -- interpolated against the provoking vertex's
        // (flat), the FS tells a triangle within one cube from one across cubes.
        vec3 cell;
        rez.w = rzCubeTimeAt(rez.xyz, box.y, box.z, cell);
        box.x = rez.w;
    }
    else if (z.effect == REZFX_SHATTER)
    {
        // World-oriented round the centre; w / box.x the shard time, as disintegrate; box.z the
        // impact point's distance from the centre (rzImpactDist; the object's z size is unused).
        rez.xyz = mat3(M) * lpos;
        box.z = rzImpactDist(z.aux0.xyz, mat3(M));
        vec3 id;
        float edge;
        const vec3 sd = rzShardOf(rez.xyz, rzShardSize(box.y), id, edge);
        rez.w = rzShardTime(sd, id, z.aux0.xyz * box.z, box.y);
        box.x = rez.w;
    }
}
// ---- RIGGED (clean_draw_rigged.vert) ----
// No model matrix here: the source (bind pose) vertex stands in for object space -- a rigged
// mesh's volume lives in the unit cube (its bind shape scales it to the avatar), the system
// avatar body (instance bit 17) in metres around the pelvis. The patterns stay on the skin as it
// animates; the sweep runs up the bind pose (one avatar about 2 m tall).
void rzRigLocal(uint flags, vec3 src, out vec3 lp, out float sweep)
{
    const bool body = (flags & 131072u) != 0u;
    lp = body ? src : src * 2.0;
    sweep = clamp(body ? (src.z + 1.1) * 0.5 : src.z + 0.5, 0.0, 1.0);
}
void rezfxRigOutputs(uint flags, vec3 src, out vec4 rez, out vec4 box)
{
    rez = vec4(0.0);
    box = vec4(0.0);
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return;
    vec3 lp;
    float sweep;
    rzRigLocal(flags, src, lp, sweep);
    rez = vec4(lp, sweep);
    box = vec4(2.0, 1.0, 2.0, 1.0);
    const uint fx = rezfx_slots[s - 1u].effect;
    if (fx == REZFX_PRINTER || fx == REZFX_TRANSPORTER) box.yz = vec2(0.25, 0.3);   // a body's footprint (bind pose)
    vec3 cell;
    if (fx == REZFX_DISINTEGRATE) rez.w = rzCubeTimeAt(lp, 1.0, 2.0, cell);
    if (fx == REZFX_SHATTER)
    {
        float edge;
        const vec3 sd = rzShardOf(lp, rzShardSize(1.0), cell, edge);
        rez.w = rzShardTime(sd, cell, vec3(0.0, 0.0, 0.9), 1.0);
    }
    if (fx == REZFX_DISINTEGRATE || fx == REZFX_SHATTER) box.x = rez.w;
}
// The de-rez vertex effects on a rigged draw, in the skinned (eye-relative world) space: pos the
// vertex, src its source vertex, ctr the draw's posed box centre (valid: ctrOk).
vec3 rezfxRigDisplace(uint flags, vec3 pos, vec3 src, vec3 ctr, bool ctrOk)
{
    if ((flags & REZFX_DYING) == 0u) return pos;
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return pos;
    const RezFxSlot z = rezfx_slots[s - 1u];
    const float p = z.progress;
    vec3 lp;
    float sweep;
    rzRigLocal(flags, src, lp, sweep);
    if (z.effect == REZFX_IMPLODE)
    {
        if (!ctrOk) return pos;
        const float q = min(p / 0.8, 1.0);
        const float shrink = p < 0.8 ? mix(1.0, 0.12, q * q) : 0.12 * (1.0 - smoothstep(0.88, 1.0, p));
        const float a = 2.5 * q * q;
        const float ca = cos(a), sa = sin(a);
        const vec3 d = pos - ctr;
        return ctr + vec3(ca * d.x - sa * d.y, sa * d.x + ca * d.y, d.z) * shrink;
    }
    if (z.effect == REZFX_GLITCH)
    {
        const float slab = rzGlitchSlab(1.0);
        const float band = floor(lp.z / slab);
        const float st = rzGlitchStep(z.time);
        const float k = smoothstep(0.0, 0.4, p);
        if (rzHash13(vec3(band, st, 3.0)) > 1.0 - 0.55 * k)
            pos.xy += (rzHash33(vec3(band, st, 9.0)).xy - 0.5) * (0.12 + 0.25 * slab) * k;
        return pos;
    }
    if (z.effect == REZFX_ASH)
    {
        const float near = 1.0 - smoothstep(0.0, 0.25, rzAshValue(lp, sweep) - rzAshCut(p));
        return pos + z.wind.xyz * (z.wind.w * (0.15 * p * p + 0.6 * near * p) * 1.0)
                   + vec3(0.0, 0.0, 0.06 * near * near * near);
    }
    // Disintegrate / shatter on a skin: the pieces (bind-pose cells) fly and fall without the
    // spin or the shrink (no rigid piece frame on a posed vertex); the FS ends them.
    if (z.effect == REZFX_DISINTEGRATE)
    {
        vec3 cell;
        const float tc = rzCubeTimeAt(lp, 1.0, 2.0, cell);
        if (p <= tc) return pos;
        const vec3 away = normalize((cell + 0.5 - RZ_CUBE_OFS) * rzCubeSize(1.0) + vec3(0.0, 0.0, 0.3));
        const float dt = (p - tc) * max(z.wind.z, 0.05);
        const vec3 r3 = rzHash33(cell + 19.0);
        return pos + away * ((0.6 + 1.2 * r3.y) * dt) + vec3(0.0, 0.0, -4.0 * dt * dt);
    }
    if (z.effect == REZFX_SHATTER)
    {
        // (the impact at the top: rzPieceTime)
        vec3 cell;
        float edge;
        const vec3 impact = vec3(0.0, 0.0, 0.9);
        const vec3 sd = rzShardOf(lp, rzShardSize(1.0), cell, edge);
        const float tc = rzShardTime(sd, cell, impact, 1.0);
        if (p <= tc) return pos;
        const float dt = (p - tc) * max(z.wind.z, 0.05);
        float spin;
        const vec3 v = rzShardLaunch(sd - impact, vec3(0.0, 0.0, 1.0), rzHash33(cell + 37.0), 1.9, 1.0, spin);
        return pos + rzShardFlight(v, dt);
    }
    return pos;
}
#endif   // GMDX_REZFX_VS

#ifdef GMDX_REZFX_FS
// ---- fragment side ----
layout(location = 16) in vec4 v_rez;   // <GMDX> 16 / 17 (15 is #901's v_wx_rest)
layout(location = 17) flat in vec4 v_rez_box;

// Reflection-probe captures see every object whole and unpainted: a cached cube would keep a
// half-built object long after it is done (the capture FS: a constant; the full FS and the mask
// pre-pass: the draw view says so).
#if defined(CLEAN_CAPTURE)
bool rzCapture() { return true; }
#elif defined(CLEAN_DRAW_VIEW_GLSL)
bool rzCapture() { return pc.u_sun_color.w > 0.5 && pc.u_sun_color.w < 1.5; }
#else
bool rzCapture() { return false; }   // (main-view-only modules: the motion / blend pre-passes)
#endif

// ---- the build effects (3D printer, transporter): what is built so far ----
// Shared by the hidden-mode cut (rezfxHidden: the mask pre-pass FSs, the lit FS, the pick) and the
// paint. Every helper returns a precise result, as the de-rez helpers above.

// 4x4 ordered dither, 0..15, fixed per screen pixel (the printer's ghost: a stable screen door).
// The mask buckets always draw at full shading rate, so the pre-pass and the lit FS see the same
// pixel centres.
uint rzBayer(uvec2 c)
{
    c &= 3u;
    const uint x = c.x ^ c.y;
    return ((x & 1u) << 3) | ((c.y & 1u) << 2) | (x & 2u) | ((c.y & 2u) >> 1);
}

// 3D PRINTER. The object prints in discrete layers, bottom up: the slot's layer count (wind.y,
// from the build's height on the CPU: 5 to 16) and infill rows per layer (aux1.w: 3 to 6, or 4).
// In each layer the nozzle zig-zags across the footprint row by row, the rows turning 90 degrees
// every layer as a slicer's rectilinear infill does: even layers run along world x (rows stepping
// along y), odd layers along y.
float rzPrintLayers(float target) { precise float r = max(1.0, floor(target + 0.5)); return r; }
float rzPrintRows(vec4 aux1) { precise float r = aux1.w >= 1.0 ? floor(aux1.w + 0.5) : 4.0; return r; }
// A point's footprint position, 0..1 along world x / y.
vec2 rzPrintFoot(vec3 q, vec4 box)
{
    precise vec2 r = clamp(q.xy / max(2.0 * box.yz, vec2(1e-3)) + 0.5, 0.0, 1.0);
    return r;
}
// Where in layer k's fill (0..1) the nozzle passes footprint point f (R rows).
float rzPrintRaster(float k, vec2 f, float R)
{
    const bool odd = mod(k, 2.0) > 0.5;
    precise float a = odd ? f.y : f.x;                 // along a row
    precise float row = min(floor((odd ? f.x : f.y) * R), R - 1.0);
    precise float along = mod(row, 2.0) > 0.5 ? 1.0 - a : a;
    precise float r = (row + along) / R;
    return r;
}
// The progress (0..1) at which the point at height y (m above the bottom) and footprint f is printed.
float rzPrintAt(float y, vec2 f, float H, float nl, float R)
{
    precise float lh = H / nl;
    precise float k = clamp(floor(y / lh), 0.0, nl - 1.0);
    precise float r = (k + rzPrintRaster(k, f, R)) / nl;
    return r;
}
// The nozzle at progress p: its footprint position (0..1) and its layer L (gmdxrezfx.cpp
// printHead draws it there).
vec2 rzPrintHead(float p, float nl, float R, out float L)
{
    L = min(floor(p * nl), nl - 1.0);
    const float lf = clamp(p * nl - L, 0.0, 1.0) * R;
    const float row = min(floor(lf), R - 1.0);
    const float along = mod(row, 2.0) > 0.5 ? 1.0 - (lf - row) : lf - row;
    const vec2 nf = vec2(along, (row + 0.5) / R);
    return mod(L, 2.0) > 0.5 ? nf.yx : nf;
}
// Hidden mode: the unprinted part shows as a faint ghost of the finished object -- a screen door,
// denser just above the print line (the next layers) and sparse higher up. No blending: the
// G-buffer and the depth pre-pass keep exactly these pixels.
bool rzGhostKeep(float y, float H, float p, float lh)
{
    precise float dl = (y - p * H) / lh;               // layers above the print line
    precise float g = 0.13 + 0.25 * clamp(1.0 - dl * 0.2, 0.0, 1.0);
    precise float b = (float(rzBayer(uvec2(gl_FragCoord.xy))) + 0.5) / 16.0;
    return b < g;
}

// TRANSPORTER. Solid at RZ_TRANS_SOLID (the flash); before it the surface is a cloud of small
// object cells (1 to 10 cm by size) that fills in, thickest in the bright interference bands,
// with spiral strands of sparkles and random glints that thin out as it solidifies.
#define RZ_TRANS_SOLID 0.7
float rzTransCell(vec4 box)
{
    precise float r = clamp(0.012 * max(box.x, 2.0 * max(box.y, box.z)), 0.008, 0.1);
    return r;
}
// Horizontal interference bands (two waves beating), rippled around the object.
float rzTransBands(vec3 q, float h, float H, float t)
{
    precise float nb = clamp(H / 0.35, 3.0, 9.0);     // bands over the height
    precise float ph = h * nb * 6.2831853;
    precise float w = 0.5 + 0.5 * sin(ph - t * 7.0 + 0.8 * sin(atan(q.y + 1e-7, q.x) * 3.0 + t * 5.0))
                                * cos(ph * 0.43 + t * 3.1);
    return w;
}
// Sparkles in three strands spiralling up around the object (1 on one), thinning out.
float rzTransSpark(vec3 q, float h, float H, float p, float t, float cs)
{
    precise float u = atan(q.y + 1e-7, q.x) * (3.0 / 6.2831853) + h * 1.6 - t * 1.25;
    precise float j = floor(h * H / (cs * 4.0) + t * 2.0);
    precise float r = rzHash13(vec3(floor(u), j, 23.0));
    precise float on = step(fract(u), 0.12) * step(r, 0.85 - 0.8 * clamp(p / RZ_TRANS_SOLID, 0.0, 1.0))
                     * step(0.25, 0.5 + 0.5 * sin(t * 31.0 + r * 60.0));
    return on;
}
// Random glints over the whole surface (1 on one), most at the start.
float rzTransGlint(vec3 cell, float p, float t)
{
    precise float r = rzHash13(cell + 31.0);
    precise float on = step(1.0 - 0.06 * (1.0 - clamp(p / RZ_TRANS_SOLID, 0.0, 1.0)), r)
                     * step(0.4, 0.5 + 0.5 * sin(t * 23.0 + r * 90.0));
    return on;
}
// Whether the surface point is there yet (hidden mode).
bool rzTransKeep(vec3 q, float h, vec4 box, float p, float t)
{
    if (p >= RZ_TRANS_SOLID) return true;
    precise float cs = rzTransCell(box);
    precise vec3 cell = floor(q / cs);
    precise float d = smoothstep(0.05, RZ_TRANS_SOLID, p);
    precise float r = rzHash13(cell + 7.0);
    precise float th = d * 1.04 + (1.0 - d) * (0.3 * (rzTransBands(q, h, box.x, t) - 0.5) + 0.06 * sin(t * 25.0 + r * 40.0));
    return r < th || rzTransSpark(q, h, box.x, p, t, cs) > 0.5 || rzTransGlint(cell, p, t) > 0.5;
}

// HIDDEN MODE: whether this fragment is a part a build effect has not built yet (and hides).
// Reads only the table, v_rez (invariant), v_rez_box (flat) and the pixel, so the mask pre-pass
// FSs, the lit FS and the pick agree. pick: the 1x1 pick target has no screen door -- the whole
// ghost counts as not there (it is not clickable).
bool rezfxHidden(uint flags, bool pick)
{
    if ((flags & (REZFX_HIDE_POOL | REZFX_DYING)) != REZFX_HIDE_POOL || v_rez_box.w < 0.5 || rzCapture()) return false;
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return false;
    const RezFxSlot z = rezfx_slots[s - 1u];
    if (z.wind.x < 0.5) return false;   // combat-safe right now: the always-visible version
    const float p = z.progress;
    if (z.effect == REZFX_PRINTER)
    {
        precise float H = max(v_rez_box.x, 0.01);
        precise float y = v_rez.w * H;
        precise float nl = rzPrintLayers(z.wind.y);
        if (p >= rzPrintAt(y, rzPrintFoot(v_rez.xyz, v_rez_box), H, nl, rzPrintRows(z.aux1))) return false;   // printed
        if (!gl_FrontFacing) return true;   // the inside of the unprinted part: no ghost
        return pick || !rzGhostKeep(y, H, p, H / nl);
    }
    if (z.effect == REZFX_TRANSPORTER)
        return !rzTransKeep(v_rez.xyz, v_rez.w, v_rez_box, p, z.time);
    return false;
}

// BACK FACES THE EFFECTS KEEP (rezfxBack: the FSs' single-sided cull asks; unrigged, natively
// single-sided rows only).
// - THE PRINTER'S CUT SURFACE (backface capping): its hidden-mode rows draw both sides in the main
//   view (clean_cull.comp: the print pool goes to the double-sided mask bucket) and their back
//   faces survive the single-sided cull. rezfxHidden then keeps only the printed ones: seen
//   through the open top, they are where the cut surface is, and rezfxPaint shades them as it.
//   Depth stays the back face's own, in the pre-pass and the lit pass alike: the same rows, the
//   same fragments, the same cut -- no specks.
// - The flying pieces of disintegrate and shatter (dying rows draw both sides in the main view
//   too): a piece that has broken loose tumbles, and its back must not vanish.
// Shatter's impact point (v_rez's frame): rezfxOutputs put its distance in v_rez_box.z; a rigged
// mesh's is at its top (rezfxRigDisplace).
vec3 rzShatterImpact(uint flags, vec4 aux0)
{
    return (flags & 1u) != 0u ? vec3(0.0, 0.0, 0.9 * v_rez_box.y) : aux0.xyz * v_rez_box.z;
}
// The piece time of this fragment (disintegrate's cube, shatter's shard).
float rzPieceTime(uint fx, uint flags, vec4 aux0)
{
    vec3 id;
    if (fx == REZFX_DISINTEGRATE) return rzCubeTimeAt(v_rez.xyz, v_rez_box.y, v_rez_box.z, id);
    float edge;
    const vec3 impact = rzShatterImpact(flags, aux0);
    precise vec3 sd = rzShardOf(v_rez.xyz, rzShardSize(v_rez_box.y), id, edge);
    precise float r = rzShardTime(sd, id, impact, v_rez_box.y);
    return r;
}
bool rezfxBack(uint flags)
{
    if ((flags & (256u | 8u | 1u)) != 0u || v_rez_box.w < 0.5 || rzCapture()) return false;
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return false;
    const RezFxSlot z = rezfx_slots[s - 1u];
    if ((flags & REZFX_DYING) != 0u)
        return (z.effect == REZFX_DISINTEGRATE || z.effect == REZFX_SHATTER) && z.progress >= rzPieceTime(z.effect, flags, z.aux0);
    // (not alpha-blend rows: their pipelines draw both sides anyway, and no blend row is capped)
    return (flags & (REZFX_PRINT_POOL | 2u)) == REZFX_PRINT_POOL && z.effect == REZFX_PRINTER && z.wind.x > 0.5;
}

// The cut: a build effect's hidden part (rezfxHidden), and what a de-rez has taken away from a
// dying object. Reads only the table and v_rez (invariant), so the mask pre-pass FSs and the lit
// FS cut the same fragments; `precise` keeps the driver from contracting the noise differently
// in the two modules.
void rezfxDiscard(uint flags)
{
    if (rezfxSlotOf(flags) == 0u || v_rez_box.w < 0.5) return;
    if ((flags & REZFX_DYING) == 0u)
    {
        if (rezfxHidden(flags, false)) discard;
        return;
    }
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return;
    const RezFxSlot z = rezfx_slots[s - 1u];
    const float p = z.progress;
    if (z.effect == REZFX_EMBER)
    {
        // EMBER BURN (Vulkanstorm): the surface burns away where its noise is under the cut.
        precise float n = rzBurnNoise(v_rez.xyz);
        precise float cut = mix(-0.08, 1.02, p);   // whole at 0, nothing left at 1
        if (n < cut) discard;
    }
    else if (z.effect == REZFX_DISINTEGRATE || z.effect == REZFX_SHATTER)
    {
        // DISINTEGRATE / SHATTER: a triangle within one piece (v_rez.w, the interpolated piece
        // time, equals the provoking vertex's, v_rez_box.x) flies whole (rezfxDisplace) and
        // shrinks away by the end. One across pieces would stretch between them once any of its
        // vertices flies, so it goes the moment its own piece, its provoking vertex's or (as
        // far as the interpolated time can tell) any of its vertices' breaks loose.
        precise float tc = rzPieceTime(z.effect, flags, z.aux0);
        // (also where this fragment's piece is not the provoking vertex's: the interpolated time
        // alone equals the flat one along a line through a spanning triangle)
        const bool span = abs(v_rez.w - v_rez_box.x) > 1e-4 || abs(tc - v_rez_box.x) > 1e-4;
        if (span ? p >= min(min(tc, v_rez_box.x), v_rez.w) : p >= 0.995) discard;
    }
    else if (z.effect == REZFX_TRON_OUT)
    {
        // TRON DE-REZ: the lines wink out from the top down (rzToutGone).
        precise float cs = rzToutCell(v_rez_box.y);
        precise vec3 cell = floor(v_rez.xyz / cs);
        if (v_rez.w > rzToutGone(p, cell)) discard;
    }
    else if (z.effect == REZFX_ASH)
    {
        // ASH: burns off from the upwind side, through the surface noise.
        precise float v = rzAshValue(v_rez.xyz, v_rez.w);
        if (v < rzAshCut(p)) discard;
    }
    else if (z.effect == REZFX_GLITCH)
    {
        // GLITCH: from 60 % on it blinks -- the whole object out on some jumps, more blocks
        // missing on the others -- until nothing is left.
        if (p > 0.6)
        {
            precise float k = (p - 0.6) / 0.4;
            const float st = rzGlitchStep(z.time);
            if (rzHash13(vec3(st, 11.0, 13.0)) < k) discard;
            precise vec3 blk = floor(v_rez.xyz / rzGlitchSlab(v_rez_box.y));
            if (rzHash13(vec3(blk.xy + st, blk.z)) < k * k + 0.1 * k) discard;
        }
    }
    else if (z.effect == REZFX_IMPLODE)
    {
        if (p >= 0.99) discard;   // (collapsed to nothing: rezfxDisplace)
    }
}

#ifdef GMDX_REZFX_SURFACE
// TRON WIREFRAME (REZFX_WIRE builds: the lit FS twins with VK_KHR_fragment_shader_barycentric,
// which LLCleanRendererVK binds when the device has it). The edge distance in barycentric units
// and its screen rate are taken at the top of the FS, before any discard, so the derivative is
// defined across the quad (rezfxDerivs, called first thing by decodeSurface).
float g_rzEdge = 1.0, g_rzEdgeW = 1.0;
float g_rzDz = 0.0;   // the world height's screen rate (the 3D printer's layer ridges)
void rezfxDerivs(uint flags)
{
    if (rezfxSlotOf(flags) == 0u) return;   // (flat: uniform over the quad)
    g_rzDz = fwidth(v_world_pos.z);
#ifdef REZFX_WIRE
    const vec3 b = gl_BaryCoordEXT;
    g_rzEdge = min(b.x, min(b.y, b.z));
    g_rzEdgeW = fwidth(g_rzEdge);
#endif
}
// 0..1 coverage of a wireframe line width_px wide (GMDXRezFxTronLineWidth); 0 without
// barycentrics.
float rzWireLine(float width_px)
{
#ifdef REZFX_WIRE
    return 1.0 - smoothstep(0.0, width_px * g_rzEdgeW, g_rzEdge);
#else
    return 0.0;
#endif
}

// The paint: replaces mixk of the surface with the unlit colour col, adds the emission add
// (both linear HDR) and returns the glow factor.
float rezfxPaint(uint flags, vec3 P, vec3 N, out vec3 col, out float mixk, out vec3 add)
{
    col = vec3(0.0);
    mixk = 0.0;
    add = vec3(0.0);
    if (v_rez_box.w < 0.5 || rzCapture()) return 0.0;
    const uint s = rezfxSlotOf(flags);
    if (!rezfxSlotOk(s)) return 0.0;
    const RezFxSlot z = rezfx_slots[s - 1u];
    const vec3 c = z.colour.rgb;
    const float h = v_rez.w;                    // 0 bottom .. 1 top
    const float H = max(v_rez_box.x, 0.01);     // the object's height (m)
    if (z.effect == REZFX_TRON)
    {
        // TRON (Vulkanstorm, made solid): the object is there in full from its first frame, an
        // unlit dark shell of its own shape. A scan climbs it drawing its wireframe (stage 0;
        // it holds there while the textures load), then a second scan climbs it leaving the
        // textured surface behind a bright edge (stage 1).
        const float line = rzWireLine(z.wind.x > 0.0 ? z.wind.x : 1.5);
        const float gk = z.colour.a;            // GMDXRezFxTronGlow (1 = the original look)
        if (z.stage == 0u)
        {
            const float p = z.progress;
            const float ahead = (p - h) * H;    // metres below the scan front (< 0: not reached)
            mixk = 1.0;
            col = c * 0.012;
            float glow = 0.0;
            if (ahead >= 0.0)
            {
                // Lines just drawn by the climbing scan flare, then settle (Vulkanstorm).
                const float flare = p < 1.0 ? 1.0 - smoothstep(0.0, 0.25, ahead) : 0.0;
                // (glow strength 0 still draws the lines, unlit, at their colour)
                add = c * line * max(3.2 * gk * (1.0 + 2.0 * flare), 1.0);
                glow = gk * line * (0.5 + 0.5 * flare);
#ifndef REZFX_WIRE
                add += c * 0.08 * gk;          // no barycentrics: the scanned part glows faintly
#endif
            }
            // The scan front itself: a thin bright band.
            if (p < 1.0)
            {
                const float band = 1.0 - smoothstep(0.0, 0.04, abs(ahead));
                add += c * 2.5 * gk * band;
                glow = max(glow, gk * band);
            }
            // <GMDX> Holding for its textures (wind.w, GMDXRezFxWaitForTextures): the finished
            // wireframe idles -- a soft scan keeps climbing it on a loop and the lines breathe.
            if (z.wind.w > 0.0)
            {
                const float hi = fract(z.time * 0.45);
                const float ib = (1.0 - smoothstep(0.0, 0.1, abs(hi - h) * H)) * z.wind.w;
                const float breathe = 1.0 + 0.2 * z.wind.w * sin(z.time * 3.0);
                add = add * breathe + c * 1.6 * gk * ib;
                glow = max(glow * breathe, 0.7 * gk * ib);
            }
            return glow;
        }
        // stage 1: the fill scan
        const float below = (z.progress - h) * H;   // metres under the fill front
        if (below >= 0.0)
        {
            const float edge = 1.0 - smoothstep(0.0, 0.12, below);
            add = c * 4.0 * gk * edge;               // solid and textured, its edge lit
            return gk * edge;
        }
        mixk = 1.0;
        col = c * 0.012;
        add = c * line * max(3.2 * gk, 1.0);
        const float band = 1.0 - smoothstep(0.0, 0.04, -below);
        add += c * 2.5 * gk * band;
        return gk * max(line * 0.5, band);
    }
    if (z.effect == REZFX_TRON_OUT)
    {
        // TRON DE-REZ (Tron in reverse): a scan comes down the object turning its textured surface
        // back into the glowing wireframe over a dark shell; the whole wire flickers; then a second
        // front comes down and the lines collapse and wink out (rezfxDiscard), flaring as they go.
        // Without barycentrics an object-space lattice stands in for the triangle edges.
        const float p = z.progress;
        const float gk = z.colour.a;
        const float scan = 1.0 - p / RZ_TOUT_SCAN;                    // the first front (sweep units)
        const float above = (h - scan) * H;                           // metres above it (> 0: wire)
        const float cs = rzToutCell(v_rez_box.y);
        const vec3 cell = floor(v_rez.xyz / cs);
        const float gone = rzToutGone(p, cell);
        const float doom = 1.0 - smoothstep(0.0, 0.15, gone - h);    // 1 just below the collapse
#ifdef REZFX_WIRE
        float line = rzWireLine(z.wind.x > 0.0 ? z.wind.x : 1.5);
#else
        const vec3 lg = fract(v_rez.xyz / cs);
        const vec3 lm = min(lg, 1.0 - lg);
        const float le = min(min(lm.x + lm.y, lm.y + lm.z), lm.x + lm.z);   // to the lattice's edges
        float line = 1.0 - smoothstep(0.0, 0.05, le);
#endif
        // The wire flickers: per cell, a few frames off now and then, more as it nears the end.
        const float fr = rzHash13(cell + floor(z.time * 24.0) + 71.0);
        line *= step(0.12 + 0.45 * smoothstep(0.45, 1.0, p) * (1.0 - doom), fr) * (1.0 + 2.0 * doom);
        float glow = 0.0;
        if (above >= 0.0)
        {
            mixk = 1.0;
            col = c * 0.012;
            add = c * line * max(3.2 * gk, 1.0);
            glow = gk * line * 0.6;
        }
        const float band = p < RZ_TOUT_SCAN ? 1.0 - smoothstep(0.0, 0.04, abs(above)) : 0.0;
        add += c * 2.5 * gk * band;
        // the collapsing front itself: a thin bright line where the lines go
        const float edgeB = 1.0 - smoothstep(0.0, 0.03, abs(gone - h));
        add += c * 3.0 * gk * edgeB * step(0.58, p);
        return max(glow, gk * max(band, edgeB * step(0.58, p)));
    }
    const float p = z.progress;
    const float t = z.time;
    const vec3 lp = v_rez.xyz;
    if (z.effect == REZFX_FIRE)
    {
        // FIRE: the object burns INTO view -- the ember burn run backwards, made solid. A glowing
        // front spreads over it through its surface noise; ahead of the front it is a charred,
        // smouldering skin of its own shape, behind it the real surface.
        const float n = rzBurnNoise(lp);
        const float front = mix(-0.08, 1.02, p);
        const float flick = 0.85 + 0.15 * sin(t * 23.0 + n * 40.0);
        if (n < front)
        {
            const float rim = 1.0 - smoothstep(0.0, 0.08, front - n);
            add = c * 4.0 * rim * flick;
            return rim;
        }
        const float heat = 1.0 - smoothstep(0.0, 0.18, n - front);
        const float speck = smoothstep(0.78, 0.95, rzNoise(lp * 14.0 + vec3(0.0, 0.0, t * 1.5)));
        mixk = 1.0;
        col = vec3(0.018, 0.010, 0.008) + c * (0.6 * speck * (0.3 + 0.7 * heat));
        add = c * 1.5 * heat * heat * flick;
        return heat * 0.8;
    }
    if (z.effect == REZFX_HOLOGRAM)
    {
        // HOLOGRAM: the object shows as a projection of itself -- scanlines climbing it, a
        // flicker, a slight colour split between the channels and a bright rim -- that settles
        // into the real surface over the last part of the effect.
        const float k = 1.0 - smoothstep(0.55, 1.0, p);
        const float ph = P.z * 55.0 - t * 9.0;
        const vec3 lines = 0.5 + 0.5 * cos(vec3(ph + 0.9, ph, ph - 0.9));   // r / g / b a little apart
        const float fl = 0.8 + 0.2 * step(0.35, rzHash13(vec3(floor(t * 18.0), 3.0, 7.0)));
        const float sweep = 1.0 - smoothstep(0.0, 0.05, abs(fract(t * 0.8) - h));
        const vec3 V = normalize(pc.u_cam_time.xyz - P);
        const float rim = pow(1.0 - clamp(abs(dot(normalize(N), V)), 0.0, 1.0), 2.0);
        mixk = 0.75 * k;
        col = c * (0.25 + 0.75 * lines) * fl * (1.0 + 1.5 * sweep);
        add = c * (0.8 * rim + 0.6 * sweep) * k * fl;
        return k * (0.3 + 0.4 * rim + 0.5 * sweep);
    }
    if (z.effect == REZFX_VOXEL)
    {
        // VOXEL ASSEMBLE: object space quantised into cubes (about an eighth of its size, 5 to
        // 50 cm). Each cube is a flat glowing block until its turn -- bottom to top, a little
        // shuffled -- then it flashes and shows the real surface.
        const float vs = clamp(v_rez_box.y * 0.12, 0.05, 0.5);
        const vec3 cell = floor(lp / vs);
        const float rnd = rzHash13(cell + 17.0);
        const float hz = clamp(((cell.z + 0.5) * vs) / max(v_rez_box.z, 0.01) + 0.5, 0.0, 1.0);
        const float act = 0.7 * hz + 0.3 * rnd;
        if (p >= act)
        {
            const float flash = 1.0 - smoothstep(0.0, 0.08, p - act);
            add = c * 3.0 * flash;
            return flash;
        }
        mixk = 1.0;
        col = c * (0.06 + 0.3 * rzHash13(cell + 41.0));
        // <GMDX> Holding for its textures (wind.w): the blocks shimmer, a slow wave of light
        // rising through them, each a little out of step.
        const float wave = 0.5 + 0.5 * sin(t * 2.6 - hz * 9.0 + rnd * 2.5);
        col *= 1.0 + z.wind.w * (0.7 * wave - 0.25);
        add = c * (0.35 * z.wind.w * wave * wave);
        return 0.15 + 0.25 * z.wind.w * wave;
    }
    // The build effects' hidden mode (rezfxHidden): what is drawn of the unbuilt part is the kept
    // pixels / cells; in the always-visible version (combat-safe) it is the whole solid surface.
    const bool hide = (flags & REZFX_HIDE_POOL) != 0u && z.wind.x > 0.5;
    if (z.effect == REZFX_PRINTER)
    {
        // 3D PRINTER: the object prints in discrete layers (rzPrintAt) under a nozzle that zig-zags
        // across each layer (gmdxrezfx.cpp draws it, with its gantry, in the world overlay).
        // - What it has just laid down glows white-hot and cools through the glow colour as raised
        //   filament ridges, then settles into the real surface.
        // - Seen from above, the cut is a solid top (the back faces, rezfxBack): the layer's
        //   infill lines, the perimeter along the walls and the fresh bead glowing where the
        //   nozzle just passed.
        // - Above the print line the unprinted shape is a ghost: a faint screen door with a
        //   glowing outline (hidden mode), or a dark glassy shell with faint slice lines (always
        //   visible, combat-safe).
        const float nl = rzPrintLayers(z.wind.y);
        const float R = rzPrintRows(z.aux1);
        const float lh = H / nl;
        const float y = h * H;
        const float len = max(z.wind.z, 0.05);
        const float on = 1.0 - step(0.999, p);
        float L;
        const vec2 hf = rzPrintHead(p, nl, R, L);
        const vec3 nz = vec3((hf - 0.5) * 2.0 * v_rez_box.yz, (L + 1.0) * lh);   // the nozzle tip (bottom-up y)
        const float rn = max(clamp(0.03 * (v_rez_box.y + v_rez_box.z), 0.01, 0.4), 0.5 * lh);
        const vec3 white = vec3(1.0, 0.86, 0.62);
        const vec3 fil = mix(vec3(0.2), c, 0.45) * 0.5;   // cooled filament (the cut surface)
        if (!gl_FrontFacing && hide && (flags & (256u | 8u | 1u)) == 0u)
        {
            // THE CUT SURFACE (rezfxBack): a printed back face seen through the open top. The view
            // ray crosses the top of the layer being printed where it is laid at that point, else
            // the top of the layer below; that point is shaded as a flat top.
            const vec3 B = lp;
            const vec3 dir = pc.u_cam_time.xyz - P;
            float k = L;
            vec3 C = B;
            const bool above = dir.z > 1e-5;
            if (above)
            {
                C = B + dir * (((L + 1.0) * lh - y) / dir.z);
                if (rzPrintRaster(L, rzPrintFoot(C, v_rez_box), R) > clamp(p * nl - L, 0.0, 1.0))
                {
                    k = L - 1.0;
                    C = B + dir * ((L * lh - y) / dir.z);
                }
                if (C.z < B.z) { C = B; k = clamp(floor(y / lh), 0.0, nl - 1.0); }   // the side of the fresh bead
            }
            const vec2 f = rzPrintFoot(C, v_rez_box);
            const bool oddk = mod(k, 2.0) > 0.5;
            const float lw = 2.0 * (oddk ? v_rez_box.y : v_rez_box.z) / R;   // an infill line's width (m)
            const float r = fract((oddk ? f.x : f.y) * R) * 2.0 - 1.0;
            float bead = sqrt(max(1.0 - r * r, 0.0));
            // The perimeter: two shells along the wall the ray leaves through (this back face's).
            const vec3 Nw = -normalize(N);   // the wall's outward normal (N faces the viewer)
            const float nh = length(Nw.xy);
            if (above && nh > 0.3)
            {
                const float pw = 0.5 * lw;
                const float dw = dot(B.xy - C.xy, Nw.xy / nh);
                if (dw >= 0.0 && dw < 2.0 * pw)
                {
                    const float rr = fract(dw / pw) * 2.0 - 1.0;
                    bead = sqrt(max(1.0 - rr * rr, 0.0));
                }
            }
            const float age = k < 0.0 ? 1e3 : (p - (k + rzPrintRaster(k, f, R)) / nl) * len;
            const float heat = above ? exp(-max(age, 0.0) / 0.45) * (1.0 - smoothstep(0.92, 1.0, p)) : 0.0;
            const vec3 hot = mix(c * 3.0, white * 7.0, heat * heat * heat);
            const vec2 dn = (C.xy - nz.xy) / rn;
            const float tip = (k == L && above) ? exp(-dot(dn, dn)) * on : 0.0;
            mixk = 1.0;
            col = fil * ((above ? 0.3 : 0.15) + 0.7 * bead);
            add = hot * (heat * (0.3 + 0.7 * bead)) + white * (5.0 * tip);
            return max(heat * bead, tip);
        }
        const float age = (p - rzPrintAt(y, rzPrintFoot(lp, v_rez_box), H, nl, R)) * len;   // s since printed
        // Layers on screen (the ridges / slice lines fade out below 4 px a layer: no aliasing).
        const float aa = clamp(lh / max(g_rzDz, 1e-6) * 0.5 - 1.0, 0.0, 1.0);
        // The nozzle's glow on the walls round the layer being printed.
        const vec3 dv = vec3(lp.xy, y) - (nz - vec3(0.0, 0.0, 0.5 * lh));
        const float dn2 = dot(dv, dv) / (rn * rn);
        const float core = exp(-dn2) * on;
        const float halo = exp(-dn2 * 0.0625) * on;
        float glow;
        if (age >= 0.0)
        {
            // Printed: a rounded bead per layer, seams between, while it is warm. It cools in about
            // a second; the last layers cool off with the end of the effect (no pop).
            const float heat = exp(-age / 0.35) * (1.0 - smoothstep(0.9, 1.0, p));
            const float r = fract(y / lh) * 2.0 - 1.0;
            const float bead = mix(0.8, sqrt(max(1.0 - r * r, 0.0)), aa);
            // <GMDX> wind.w: kept warm (filament, not the real surface) until its final layers.
            const float warm = max(smoothstep(0.03, 0.5, heat), z.wind.w);
            mixk = 0.9 * warm;
            col = (vec3(0.05) + c * 0.06) * (0.45 + 0.9 * bead);
            add = c * (2.6 * heat * (0.55 + 0.45 * bead)) + white * (5.0 * pow(heat, 8.0));
            glow = heat;
        }
        else
        {
            // Not printed yet: the ghost, a cool tint of the glow colour, warm just above the line.
            const vec3 gc = mix(c, vec3(0.5, 0.72, 1.0), 0.65);
            const vec3 V = normalize(pc.u_cam_time.xyz - P);
            const float rim = pow(1.0 - clamp(abs(dot(normalize(N), V)), 0.0, 1.0), 2.5);
            const float next = exp(-max(y - p * H, 0.0) / lh * 0.6);
            const float slice = (1.0 - smoothstep(0.0, 0.12, abs(fract(y / lh + 0.5) - 0.5))) * aa;
            mixk = 1.0;
            if (hide)
            {
                col = gc * 0.12;
                add = gc * (0.35 + 1.6 * rim) + c * (0.6 * next);
            }
            else
            {
                col = vec3(0.012) + gc * 0.03;
                add = gc * (0.06 + 0.9 * rim + 0.15 * slice) + c * (0.5 * next);
            }
            glow = (hide ? 0.25 : 0.1) + 0.4 * rim;
        }
        add += white * (6.0 * core) + c * (1.5 * halo);
        return max(glow, max(core, 0.5 * halo));
    }
    if (z.effect == REZFX_TRANSPORTER)
    {
        // TRANSPORTER: the object beams in. Before it is solid (RZ_TRANS_SOLID) its surface is
        // light: interference bands ripple up through it, sparkles run in spiral strands and glint
        // all over, thickest at the start; in hidden mode only those and a growing cloud of cells
        // are there (rzTransKeep), in the always-visible version they play over a dark shell. As it
        // fills in the real surface comes through, then a bright flash as it becomes solid and a
        // glow that fades off the surface. (The beam column and the spiralling sparkles around it:
        // GMDXRezFx::renderOverlay.)
        const float cs = rzTransCell(v_rez_box);
        const float d = smoothstep(0.05, RZ_TRANS_SOLID, p);
        const float u = 1.0 - d;                            // how much is not there yet
        const float pre = 1.0 - step(RZ_TRANS_SOLID, p);
        const float band = rzTransBands(lp, h, H, t);
        const float spark = rzTransSpark(lp, h, H, p, t, cs) * pre;
        const float glint = rzTransGlint(floor(lp / cs), p, t) * pre;
        const vec3 V = normalize(pc.u_cam_time.xyz - P);
        const float rim = pow(1.0 - clamp(abs(dot(normalize(N), V)), 0.0, 1.0), 2.0);
        const float fl = exp(-pow((p - RZ_TRANS_SOLID) / 0.035, 2.0));       // the flash
        const float ag = p > RZ_TRANS_SOLID ? 1.0 - smoothstep(RZ_TRANS_SOLID, 1.0, p) : 0.0;   // afterglow
        const vec3 bright = c + vec3(0.6);
        mixk = max(pre * u * (hide ? 0.85 : 0.92), 0.6 * fl);
        col = mix(c * ((0.08 + 0.55 * band) * u), bright, fl);
        add = c * (pre * u * (1.4 * band + 1.2 * rim)) + bright * (6.0 * spark + 4.0 * glint)
            + (c * 3.0 + vec3(3.0)) * fl + c * (1.6 * ag * ag * (0.65 + 0.35 * band));
        return max(max(spark, glint), max(fl, max(0.6 * band * u * pre, 0.5 * ag)));
    }
    if (z.effect == REZFX_DISINTEGRATE)
    {
        // DISINTEGRATE: glowing seams open along the cube boundaries as each cube's moment nears;
        // it flashes as it breaks loose, then flies off (rezfxDisplace) glowing ever hotter as it
        // shrinks away, its edges still lit. (gmdxrezfx.cpp adds short glow trails behind them.)
        vec3 cell;
        const float tc = rzCubeTimeAt(lp, v_rez_box.y, v_rez_box.z, cell);
        const float vs = rzCubeSize(v_rez_box.y);
        const vec3 g = fract(lp / vs + RZ_CUBE_OFS);
        const vec3 gm = min(g, 1.0 - g);
        const float e = min(gm.x, min(gm.y, gm.z)) * vs;                 // to the nearest cube face (m)
        const float seam = 1.0 - smoothstep(0.0, 0.05 * vs + 0.004, e);
        const float near = smoothstep(tc - 0.3, tc - 0.02, p);           // the seams light up ahead
        const float loose = step(tc, p);
        const float flash = exp(-pow((p - tc) / 0.035, 2.0));
        const float hot = loose * smoothstep(tc, 1.0, p);
        mixk = max(0.25 * near + 0.6 * hot, max(seam * near, flash));
        col = c * (0.4 + 2.6 * max(flash, seam * near));
        add = c * (3.0 * seam * max(near, 0.5 * loose) + 1.5 * hot);
        return max(max(flash, seam * near), 0.7 * hot);
    }
    if (z.effect == REZFX_ASH)
    {
        // ASH: the burn line is a ragged, flickering ember rim (noise along it, white-hot at the
        // very edge) that curls up off the surface (rezfxDisplace); behind it a charred band with
        // glowing cracks, then grey ash. (gmdxrezfx.cpp: flakes peel off it and drift downwind,
        // glowing and cooling, with a little smoke.)
        const float d = rzAshValue(lp, h) - rzAshCut(p);   // > 0: still there
        const float flick = 0.6 + 0.4 * rzNoise(lp * 5.0 + vec3(t * 7.0, -t * 5.0, t * 9.0));
        const float rw = 0.02 + 0.035 * rzNoise(lp * 11.0 + vec3(0.0, 0.0, t * 2.0));
        const float ember = 1.0 - smoothstep(0.0, rw, d);
        const float charK = 1.0 - smoothstep(rw, rw + 0.14, d);
        const float cracks = smoothstep(0.6, 0.85, rzNoise(lp * 26.0 + t * 0.7)) * charK * (1.0 - ember);
        const vec3 hotc = mix(c * 2.4, vec3(1.0, 0.8, 0.5) * 6.0, ember * ember * ember);
        mixk = max(charK * 0.94, ember);
        col = mix(mix(vec3(0.11, 0.105, 0.1), vec3(0.02, 0.016, 0.014), charK), hotc, ember);
        add = hotc * (ember * flick) + c * (2.5 * cracks * flick);
        return max(ember * flick, 0.6 * cracks);
    }
    if (z.effect == REZFX_SHATTER)
    {
        // SHATTER: a white-hot flash at the impact point and a shockwave ring racing out over the
        // surface; glowing fracture lines (the shard borders, and a star of straight cracks round
        // the impact) spider out from it, flaring as they open, while the surface goes glassy;
        // then the shards break loose and are blasted off (rezfxDisplace), glassy and pale with a
        // bright rim, glinting hard as they spin, their broken edges still glowing.
        const bool rig = (flags & 1u) != 0u;
        const float radius = v_rez_box.y;
        const vec3 n = rig ? vec3(0.0, 0.0, 1.0) : z.aux0.xyz;
        const vec3 impact = rzShatterImpact(flags, z.aux0);
        const float norm = max(radius + length(impact), 1e-3);
        vec3 id;
        float edge;
        const float cs = rzShardSize(radius);
        const vec3 sd = rzShardOf(lp, cs, id, edge);
        const float tc = rzShardTime(sd, id, impact, radius);
        const vec3 dv = lp - impact;
        const float dimp = length(dv);
        const float dn = clamp(dimp / norm, 0.0, 1.0);
        const float reach = dn * RZ_CRACK_END;
        const float cracked = smoothstep(reach, reach + 0.015, p);
        const float tipk = exp(-pow((p - reach) / 0.015, 2.0));          // the racing crack tip
        const float flare = cracked * exp(-max(p - reach, 0.0) / RZ_CRACK_FLARE);
        const float crush = (1.0 - dn) * (1.0 - dn);                     // wider cracks near the impact
        const float width = (0.025 * cs + 0.003) * (1.0 + 1.5 * crush);
        // the star: RZ_RADIAL_CRACKS straight cracks out from the impact across the surface,
        // a little crooked, each its own length (about half way across)
        const vec3 u = normalize(cross(n, abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
        const vec2 pl = vec2(dot(dv, u), dot(dv, cross(n, u)));
        // (one crack per sector, kept off the sector's sides by its jitter and wobble)
        const float sa = atan(pl.y, pl.x + 1e-7) * (RZ_RADIAL_CRACKS / 6.2831853);
        const float ci = floor(sa);
        const float wob = 0.25 * (rzNoise(vec3(dimp / cs * 2.0, ci * 3.1, 7.0)) - 0.5);
        const float off = abs(sa - ci - 0.5 - 0.3 * (rzHash13(vec3(ci, 3.0, 11.0)) - 0.5) - wob) * (6.2831853 / RZ_RADIAL_CRACKS) * length(pl);
        const float rlen = norm * (0.35 + 0.35 * rzHash13(vec3(ci, 5.0, 13.0)));
        const float radial = (1.0 - smoothstep(0.0, width, off)) * (1.0 - smoothstep(0.6 * rlen, rlen, dimp));
        const float line = max(1.0 - smoothstep(0.0, width, edge), radial) * cracked;
        const float loose = step(tc, p);
        const float dt = (p - tc) * max(z.wind.z, 0.05);
        const float r = rzHash13(id + 43.0);
        float spin;
        rzShardLaunch(sd - impact, n, rzHash33(id + 37.0), norm, radius, spin);
        const float glint = loose * pow(max(sin(dt * spin + r * 6.2831853), 0.0), 12.0);
        // the impact: a flash (white-hot core, coloured fringe) and the shockwave ring
        const float fr = clamp(RZ_FLASH_SIZE * radius, 0.04, 1.5);
        const float flash = exp(-p / RZ_FLASH_DECAY) * exp(-(dimp * dimp) / (fr * fr));
        const float ringR = norm * p / RZ_RING_END;
        const float ringW = 0.03 * norm + 0.01;
        const float ring = p < RZ_RING_END ? exp(-pow((dimp - ringR) / ringW, 2.0)) * (1.0 - p / RZ_RING_END) : 0.0;
        const vec3 V = normalize(pc.u_cam_time.xyz - P);
        const float rim = pow(1.0 - clamp(abs(dot(normalize(N), V)), 0.0, 1.0), 2.0);
        const float glass = max(0.45 * smoothstep(0.0, 2.0 * RZ_CRACK_END, p), 0.6 * loose);
        const float fade = 1.0 - 0.6 * loose * smoothstep(tc, 1.0, p);
        mixk = max(max(glass, line), max(flash, ring));
        col = mix(c * 0.16, c * 2.8, line * fade);
        add = c * (glass * 1.2 * rim + line * fade * (1.5 + 5.0 * flare + 3.0 * tipk))
            + vec3(1.0) * (RZ_GLINT * glint + 10.0 * flash * flash) + c * (6.0 * flash + 4.0 * ring) + vec3(2.0 * ring);
        return max(max(line * fade, glint), max(max(flash, ring), 0.4 * rim * glass));
    }
    if (z.effect == REZFX_GLITCH)
    {
        // GLITCH: slabs of the surface take a hard colour split (red one jump, cyan the next,
        // in the glow colour), flickering.
        const float st = rzGlitchStep(t);
        const float slab = rzGlitchSlab(v_rez_box.y);
        const float band = floor(lp.z / (slab * 0.35));
        const float k = smoothstep(0.0, 0.3, p);
        const float r = rzHash13(vec3(band, st, 5.0));
        if (r > 1.0 - 0.5 * k)
        {
            const bool red = rzHash13(vec3(band, st, 6.0)) > 0.5;
            mixk = 0.6;
            col = (red ? vec3(1.0, 0.1, 0.25) : vec3(0.1, 0.9, 1.0)) * c * 2.5 + (red ? vec3(0.4, 0.0, 0.0) : vec3(0.0, 0.3, 0.4));
            return 0.6 * k;
        }
        return 0.0;
    }
    if (z.effect == REZFX_IMPLODE)
    {
        // IMPLODE: heats up as it falls in, then a short flash from the collapsed core.
        const float q = min(p / 0.8, 1.0);
        const float flash = smoothstep(0.72, 0.84, p) * (1.0 - smoothstep(0.9, 0.99, p));
        mixk = flash;
        col = c * 8.0;
        add = c * 1.2 * q * q;
        return max(flash, 0.3 * q);
    }
    if (z.effect == REZFX_EMBER)
    {
        // EMBER BURN (Vulkanstorm): an orange rim glows just above the cut (rezfxDiscard); the
        // surface keeps its own texture throughout.
        const float n = rzBurnNoise(v_rez.xyz);
        const float cut = mix(-0.08, 1.02, z.progress);
        const float rim = 1.0 - smoothstep(0.0, 0.08, n - cut);
        add = c * 4.0 * rim;
        return rim;
    }
    return 0.0;
}

// Applies the paint to a decoded surface (clean_surface_light.glsl SurfaceLit): the replaced
// part's albedo goes dark and the effect colour comes in as emission (lit models: emissive,
// which the legacy model and the G-buffer carry for this; fullbright: its unlit albedo).
// Returns the glow factor for the glow target.
float rezfxSurface(uint flags, vec3 P, inout SurfaceLit s)
{
    vec3 col, add;
    float mixk;
    const float glow = rezfxPaint(flags, P, s.Ng, col, mixk, add);
    if (mixk <= 0.0 && glow <= 0.0 && max(add.r, max(add.g, add.b)) <= 0.0) return glow;
    if (s.model == SURF_FULLBRIGHT)
        s.albedo = mix(s.albedo, min(col, vec3(1.0)), mixk) + min(add, vec3(1.0));
    else
    {
        s.albedo *= 1.0 - mixk;
        s.emissive = s.emissive * (1.0 - mixk) + col * mixk + add;
    }
    return glow;
}
#else
void rezfxDerivs(uint flags) {}
#endif   // GMDX_REZFX_SURFACE
#endif   // GMDX_REZFX_FS
#endif   // GMDX_REZFX_GLSL
