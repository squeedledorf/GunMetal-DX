// vkclean — WET AND SNOWY SURFACES (#783; vkgfx/llcleanweathervk.cpp, frame info ssr.weather): the
// decoded material (clean_draw.frag decodeSurface) is wetted BEFORE it is lit or packed,
// so the forward lit FS, the G-buffer (and so every deferred lighting kernel, the split
// reflection pass included) and the captures all see one wet surface.
//
// Our own implementation after the published wet-surface model (Lagarde, "Water drop 2b --
// Dynamic rain and its effects", 2013): water fills the pores of a dielectric (darker
// albedo) and a water film smooths it (lower roughness / higher gloss, more specular);
// metals barely change; flat upward surfaces collect water (puddles, rain ripples), walls
// shed it. Only surfaces OPEN TO THE SKY wet: the weather occlusion map (a top-down depth
// map around the camera, set 0 binding 49) says whether anything is above. Ripples: rings
// from drops landing at random places and times on a hashed cell grid (two layers), their
// slope perturbing the shading normal.
//
// SKY TEST (M1.5): two cascades of the occlusion map -- a near one (~3 cm texels over ~32 m)
// and the coarse one (~0.4-1 m texels over 4 x the precipitation radius). Near the camera the
// near map decides, blending to the coarse one over its outer tenth. Each is read with a small
// PCF: four bilinear-weighted textureGather taps on a disc (receiver-plane corrected on slopes,
// rotated per pixel and frame while the view is jittered so TAA integrates it), every texel
// compared against the receiver -- soft drip lines instead of texel stair steps.
//
// TILTED MAP (M2, RenderVKWeatherTiltedMap): each cascade is a SHEARED orthographic view along
// the precipitation's mean fall direction d ~ (s, -1) (s = horizontal drift per metre of fall,
// from the slow mean wind; capped at 60 degrees): its texels stay on the horizontal grid of the
// map's top plane, a point P lies on the column P.xy - (top - P.z) s, and the map holds the
// first surface the rain meets along it. Walls: a surface is hit only when it faces into the
// precipitation (N . -d > 0, weatherFacing) and is unoccluded along d -- a wall facing the
// wind wets, its lee side stays dry, and in still air walls barely wet at all (this replaces
// the M1.5 wall heuristic, which read the ground in front of a wall).
//
// ssr.weather[0]  x wetness 0..1, y rain 0..1 (ripple density), z time (s, bounded), w ripple
//                 normal strength (0 = no ripples). [0].x and [5].x both 0: nothing below runs.
// ssr.weather[1]  COARSE: xy map corner (world), z 1 / extent (per m), w which map: 0 none,
//                 1 / 2 = uWeatherOccl[0] / [1] (double-buffered: a map renders into the
//                 unpublished one and is published the next recording, so every reader of a
//                 recording sees the map its frame info describes, wherever it runs)
// ssr.weather[2]  COARSE: x map top height (m), y top - bottom (m), z cover bias (m), w texel (m)
// ssr.weather[3]  NEAR: as [1]; w 1 / 2 = uWeatherOccl[2] / [3] (0: no near cascade)
// ssr.weather[4]  NEAR: as [2]
// ssr.weather[5]  x snow cover 0..1, y jitter phase (frame, 0..255), z PCF radius (m, at
//                 least), w PCF radius (texels of the cascade read, at least)
// ssr.weather[6]  COARSE: xy the map's shear s (0: straight down); M3 z > 0 = avatars carry
//                 their own snow: the per-instance image's (uWeatherOccl[6]) avatar rows start at
//                 row z - 1 ([slot, z - 1] = (amount, yaw), [slot, z] = the anchor xyz); #901 w > 0 =
//                 the per-avatar SHELTER ATLAS: w - 1 = which of uWeatherOccl[8] / [9] is current
// ssr.weather[7]  NEAR: xy the map's shear s; M3 zw the precipitation's own fall shear (the maps are
//                 rendered untilted while snow falls or lies: clean_weather_snow.comp marches it)
// ssr.weather[8]  LIGHTNING (M2): rgb the flash's fill radiance (linear, x intensity), w the
//                 flash 0..1 (0: no flash, nothing reads [9].xyz)
// ssr.weather[9]  xyz unit direction from the camera to the strike (world, z up), w sky
//                 darkening 0..1 (cloud / rain; clean_sky.frag)
// ssr.weather[10] PERSISTENT SNOW COVER (M2, clean_weather_snow.comp): xy the published cover
//                 grid's corner (on its map's top plane), z 1 / extent, w which: 0 none, 1 / 2 =
//                 uWeatherOccl[4] / [5] (rgba32f: r top cover, g buried cover, b top depth (m),
//                 M3 a snow depth (m) -- the snow mesh's, clean_weather_snowmesh.comp)
// ssr.weather[11] x > 0 = moving objects carry their own snow (uWeatherOccl[6]: rgba32f, .r one
//                 texel per instance slot; x = the instance rows), y that image's width, z the weather's cloud cover (the
//                 cloud layers), w the cover grid's height tolerance (m)
// ssr.weather[12] the cover grid's frame: x its top plane (m), yz its shear, w its texel (m)
// ssr.weather[13] M4 RUN-OFF: xy the flow map's corner (on its near map's top plane), z 1 / extent,
//                 w the run-off strength 0..1 (0: no run-off -- nothing below runs)
// ssr.weather[14] M4: x the flow map's top plane (m), yz its shear, w flags: 1 the flow map
//                 (uWeatherOccl[7]) is valid, 2 rivulets, 4 the drip lines' landing boost; #901
//                 8 the avatars' REST-POSE ACCUMULATION grids are live (draw set 0 bindings 50 / 51:
//                 clean_weather_accum.glsl), 16 RenderVKWeatherAvatarAccumDebug, 32 #783
//                 RenderVKWeatherSnowDepthDebug
// ssr.weather[15] #954 the FOG-OFF precipitation veil: rgb its in-scatter colour, w its extinction
//                 (1/m; 0 none); [16] the precipitation MEDIUM in the volumetric fog -- both
//                 contracts in clean_weather_veil.glsl
//
// RUN-OFF (M4, clean_weather_runoff.comp builds the flow map): the GROUND UNDER A DRIP LINE (an
// upward surface at the landing height of a nearby outlet) wets sooner and holds puddles along
// the line, sheltered or not; walls and steep roofs carry WATER FLOW (weatherRivulets): sparse
// braided trickles in light rain that merge downslope into fewer, wider channels as the flow
// accumulates (the flow map's per-texel flow / an outlet above), turning into a continuous
// sheet with a moving sheen in heavy rain -- rendered as gloss (low roughness, slight darkening,
// highlights flowing downslope), never as dark lines. Lee walls under an outlet still stream
// (the water comes from above, not the sky).
//
// PERSISTENT SNOW (M2): with a cover grid, static surfaces near the camera take the INTEGRATED
// cover (clean_weather_snow.comp) instead of the global cover x the current openness: the top
// surface of a column its accumulated cover (soft at the drip lines by the PCF sky term), a
// surface under it the cover it had when something was put over it (a moved table keeps the
// lawn's snow underneath; its old spot is bare and refills). Moving objects (instance bit21)
// carry their own amount, drawn with object-space noise so the patches ride along. Includers
// name the fragment's instance flags / slot with WEATHER_INST_FLAGS / WEATHER_INST_ID (clean_draw.frag:
// v_flags, v_terrain_rec -- the VS writes the instance slot there for non-terrain draws; M3: the
// rigged VS writes 0x80000000 | the avatar's joint-table slot, or 0xFFFFFFFF).
//
// SNOW ON AVATARS (M3, ssr.weather[6].z): avatars -- rigged draws (bit0) and worn rigid
// attachments (bit9, whose skin_id is the wearer's slot) -- never read the grid or the global
// cover (they are not in the roof map, sitting or not): every part of an avatar takes ITS
// amount (clean_weather_snow.comp -DWX_SNOW_AVATAR), on its upward-facing skinned surfaces
// (the skinned geometric normal), with the patch pattern in the avatar's chest frame (anchor +
// yaw) so it stays on the shoulders through the animation. Cluster-path avatar parts
// (RenderVKClusterLOD) carry their slot too (clean_cluster_draw_common.glsl, the #901 / #909
// follow-up: before it they had none -- no shelter map, no wetness, no coat, no grid: no snow).
//
// PER-AVATAR SHELTER (#901, ssr.weather[6].w; vkgfx/llcleanweathervk_shelter.cpp,
// clean_weather_shelter.comp): the avatars near the camera each have a top-down height map of
// themselves and what they wear, in their own frame (uWeatherOccl[8] / [9]: x the top surface's
// height above the pelvis per tile texel; the four rows under the tiles hold each joint-table
// slot's frame and tile). A fragment of such an avatar (rigged bit0 / worn rigid bit9 with its
// slot) is sheltered when a worn surface lies above it on its column -- an umbrella keeps the
// hands and the body dry, a hat brim the face; the canopy's top is its own column top and wets.
// The maps hold avatars only (they never enter the roof map): this is their own cover.
// ssr.weather[6].w >= 10: RenderVKWeatherAvatarShelterDebug (w - 10 is the atlas): avatar fragments
// are tinted by what their map says -- green the column's top, red under a worn surface, blue
// where the map holds snow, magenta an avatar fragment with no map.

#ifndef CLEAN_WEATHER_SURFACE_GLSL
#define CLEAN_WEATHER_SURFACE_GLSL
#include "clean_weather_snow_relief.glsl"   // #906: the snow relief / sparkle (shared with the snow mesh)

// #906: the pixel footprint (m) at the fragment weatherSurface is shading (set there, read by
// weatherSnowLayer: the relief's octaves fade out under it).
float wx_footprint = 0.01;
// #970 the RT hit weather widens it to the ray cone's footprint at the hit (clean_rt_weather.glsl).
#ifndef WX_FOOTPRINT
#define WX_FOOTPRINT(est) (est)
#endif

// The occlusion maps: D32 top-down depth (0 = the map's top, 1 = its bottom), nearest (only
// ever gathered), border depth 1 (open sky). [0] / [1] the coarse cascade, [2] / [3] the near
// one. The renderer's 1 x 1 stand-in where a cascade does not exist (never read then:
// ssr.weather[1].w / [3].w are 0).
// M2: [4] / [5] the persistent snow cover grids, [6] the per-instance snow (GENERAL layout;
// the stand-in where they do not exist -- never read then, ssr.weather[10].w / [11].x are 0).
// M4: [7] the run-off flow map (rgba32f, GENERAL; read only with ssr.weather[14].w bit 1).
layout(set = 0, binding = 49) uniform sampler2D uWeatherOccl[10];
#ifndef WEATHER_INST_FLAGS
#define WEATHER_INST_FLAGS 0u
#define WEATHER_INST_ID 0xFFFFFFFFu
#endif
// The instance rows (draw set 0 binding 0; only the model matrix is read: a moving object's
// snow is drawn in its own frame). Same 224 B stride as LLCleanInstanceStore::Instance.
// #970 WX_INST_EXTERNAL: the includer names the instance rows itself (the RT hit weather,
// clean_rt_weather.glsl: the RT set's rt_instances -- draw set 0 binding 0 is VERTEX | FRAGMENT
// only, a compute pipeline must not use it) through WX_INST_COUNT() / WX_INST_MODEL(i) /
// WX_INST_SKIN(i) (the row's skin_id).
#ifndef WX_INST_EXTERNAL
struct WxSurfInstance { mat4 model_matrix; vec4 rest[10]; };   // (rest[7].w: skin_id's bits)
layout(std430, set = 0, binding = 0) readonly buffer WxSurfInstances { WxSurfInstance wx_instances[]; };
#define WX_INST_COUNT() uint(wx_instances.length())
#define WX_INST_MODEL(i) wx_instances[i].model_matrix
#define WX_INST_SKIN(i) floatBitsToUint(wx_instances[i].rest[7].w)
#endif

// The four texels bilinear filtering would blend at uv, map m (dynamically uniform: frame info).
vec4 weatherGather(uint m, vec2 uv)
{
    if (m == 0u) return textureGather(uWeatherOccl[0], uv, 0);
    if (m == 1u) return textureGather(uWeatherOccl[1], uv, 0);
    if (m == 2u) return textureGather(uWeatherOccl[2], uv, 0);
    return textureGather(uWeatherOccl[3], uv, 0);
}

// How squarely a surface faces the precipitation: N . -d for the fall direction of shear s
// (d ~ (s, -1)): 1 = head on, <= 0 = turned away (never rained / snowed on). Straight down
// (s = 0) it is N.z.
float weatherFacing(vec3 Ng, vec2 s) { return dot(Ng, vec3(-s, 1.0)) * inversesqrt(1.0 + dot(s, s)); }

// P's column in a cascade's map (uv): back along its fall direction to the map's top plane
// (fr = ssr.weather[1] / [3], top = [2].x / [4].x, s = [6].xy / [7].xy).
vec2 weatherMapUV(vec4 fr, float top, vec2 s, vec3 P) { return (P.xy - (top - P.z) * s - fr.xy) * fr.z; }

// One cascade's openness at P: 1 = open to the sky, 0 = under cover. fr = ssr.weather[1] / [3]
// (corner, 1 / extent, which), fz = [2] / [4] (top, span, bias, texel), s = [6] / [7] (shear),
// base = its first map. The disc of taps sits around P's own column (M2: no wall offset -- a wall
// facing the precipitation is the first surface its columns meet, so it reads itself). Slopes and
// walls: each tap compares against the receiver's own plane at the tap's column, not P's height,
// so a hillside / a wind-facing wall does not shadow itself: a plane through P with normal N
// meets the column c + o at z = P.z - N.xy . o / (N . (-s, 1)) -- only on surfaces facing the
// precipitation enough to have a stable one. rot: the disc's rotation (cos, sin).
float weatherOpenCascade(vec4 fr, vec4 fz, vec2 s, uint base, vec3 P, vec3 Ng, vec2 rot)
{
    const uint m = base + uint(fr.w + 0.5) - 1u;
    const float texel = fz.w;
    const float res = 1.0 / (fr.z * texel);
    const float radius = max(ssr.weather[5].z, ssr.weather[5].w * texel);
    const vec2 c = P.xy - (fz.x - P.z) * s;
    const float ls = sqrt(1.0 + dot(s, s));
    const float f = dot(Ng, vec3(-s, 1.0));
    const vec2 g = -Ng.xy / max(f, 0.25 * ls) * smoothstep(0.25, 0.45, f / ls);
    const float bias = fz.z;
    // A rotated four-tap disc (two radii): with the gathers, 16 texels around c.
    const vec2 kTaps[4] = vec2[4](vec2(-0.76, -0.33), vec2(0.33, -0.76), vec2(0.76, 0.33), vec2(-0.33, 0.76));
    float open = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const vec2 o = kTaps[i] * (i < 2 ? 1.0 : 0.55);
        const vec2 xy = c + radius * vec2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
        const vec2 uv = (xy - fr.xy) * fr.z;
        const vec2 f = fract(uv * res - 0.5);
        const vec4 top = fz.x - weatherGather(m, uv) * fz.y;   // gather order: (0,1) (1,1) (1,0) (0,0)
        const float zr = P.z + dot(g, xy - c);
        const vec4 op = clamp((vec4(zr) - (top - 1.5 * bias)) / bias, 0.0, 1.0);   // open from top - 0.5 bias up
        open += dot(op, vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)));
    }
    return open * 0.25;
}

// 1 = open to the sky at P, 0 = under cover; soft at the edges (PCF). The near cascade where it
// covers P (blending to the coarse one over its outer tenth), the coarse one elsewhere; outside
// both: open (no map says otherwise).
float weatherSkyOpen(vec3 P, vec3 Ng)
{
    const bool haveCoarse = ssr.weather[1].w > 0.5, haveNear = ssr.weather[3].w > 0.5;
    if (!haveCoarse && !haveNear) return 1.0;
    // The disc's rotation: per pixel and frame (interleaved gradient noise) while the view is
    // jittered -- TAA integrates it into a smooth edge; fixed otherwise (no static noise in
    // un-jittered views and captures).
    vec2 rot = vec2(1.0, 0.0);
#ifndef WX_NO_JITTER   // (#970 the RT hit weather: no pixel -- the fixed disc)
    if (ssr.taa_params.w > 0.5)
    {
        const vec2 px = gl_FragCoord.xy + 5.588238 * ssr.weather[5].y;
        const float a = 6.2831853 * fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
        rot = vec2(cos(a), sin(a));
    }
#endif
    float wNear = 0.0;
    if (haveNear)
    {
        const vec2 uv = weatherMapUV(ssr.weather[3], ssr.weather[4].x, ssr.weather[7].xy, P);
        const vec2 e = min(uv, 1.0 - uv);
        wNear = smoothstep(0.0, 0.1, min(e.x, e.y));
    }
    float open = 0.0;
    if (wNear > 0.0) open = wNear * weatherOpenCascade(ssr.weather[3], ssr.weather[4], ssr.weather[7].xy, 2u, P, Ng, rot);
    if (wNear < 1.0)
    {
        float coarse = 1.0;
        if (haveCoarse)
        {
            const vec2 uv = weatherMapUV(ssr.weather[1], ssr.weather[2].x, ssr.weather[6].xy, P);
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))))
                coarse = weatherOpenCascade(ssr.weather[1], ssr.weather[2], ssr.weather[6].xy, 0u, P, Ng, rot);
        }
        open += (1.0 - wNear) * coarse;
    }
    return open;
}

// The precipitation's shear the surfaces take their facing from: its own fall shear (M3: the
// maps render untilted while snow falls or lies, so theirs is not it).
vec2 weatherShear() { return ssr.weather[7].zw; }

// ---- #901 PER-AVATAR SHELTER ----
// The fragment's avatar slot (rigged: the VS's 0x80000000 | slot; worn rigid: the instance's
// skin_id), 0xFFFFFFFF none.
uint weatherAvatarSlot(uint flags, uint id)
{
    if ((flags & 1u) != 0u) return (id & 0x80000000u) != 0u ? (id & 0xFFFFu) : 0xFFFFFFFFu;
    if ((flags & 512u) != 0u && id < WX_INST_COUNT()) return WX_INST_SKIN(id);
    return 0xFFFFFFFFu;
}
const int kWxShSlotRows = 7;   // (llcleanweathervk_shelter.cpp kShSlotRows, clean_weather_shelter.glsl WX_SH_SLOT_ROWS)
// #909 NEAR SELF-COVER (clean_weather_accum.glsl wxAcNearCover, the same curve): the avatar's own
// top `above` m over a receiver within [threshold, range] covers it -- a snout over the tongue, a
// chin over the throat --, taken live per fragment (static relative to the body: nothing to
// integrate). wx_near: x range, y threshold, z softness (m); the record's (ac_head.z, the
// RenderVKWeatherAvatarNearCover* settings) while the grids are live, else these defaults.
vec3 wx_near = vec3(0.25, 0.012, 0.015);
float weatherNearCover(float above)
{
    if (!(wx_near.x > 0.0)) return 0.0;
    return smoothstep(wx_near.y, wx_near.y + max(wx_near.z, 1.0e-3), above) * (1.0 - smoothstep(wx_near.x, wx_near.x + 0.08, above));
}
float weatherShWhich() { const float w = ssr.weather[6].w; return w > 9.5 ? w - 10.0 : w; }
bool weatherShDebug() { return ssr.weather[6].w > 9.5; }
vec4 weatherShFetch(ivec2 t) { return weatherShWhich() > 1.5 ? texelFetch(uWeatherOccl[9], t, 0) : texelFetch(uWeatherOccl[8], t, 0); }
vec4 weatherShGather(vec2 uv, int c)
{
    if (weatherShWhich() > 1.5) return c == 0 ? textureGather(uWeatherOccl[9], uv, 0) : textureGather(uWeatherOccl[9], uv, 1);
    return c == 0 ? textureGather(uWeatherOccl[8], uv, 0) : textureGather(uWeatherOccl[8], uv, 1);
}
// The avatar's own cover at P (Ng: the geometric normal -- the column is taken a texel out along
// it, so a surface does not shelter itself across its own silhouette): open 1 .. 0 under a surface
// of the avatar or what it wears (#909: the body too -- a tongue under the upper jaw, a throat
// under the chin, a back under a wing), per tap of the 2 x 2 texels against the fragment's OWN
// PLANE at that tap (receiver-plane: a slope is not its own cover) with a tight tolerance (~1 cm +
// half a texel: mouth scale), bilinear; depth: the per-avatar snow depth that lies ON THIS
// FRAGMENT -- each tap's depth only as far as the fragment is that column's top (#901 umbrella:
// the column's depth belongs to its top surface, the canopy; it was taken whole and only the
// coat's gate was scaled by the openness, so once the canopy held ~1 cm the coat's threshold
// sweep turned any few percent of leak into full snow on the body under it).
// false: no shelter map for this fragment.
// local: P in the avatar's own frame (x forward, y left, z above the pelvis; the map's frame,
// current this recording) -- the coat's pattern lives there, so it stays on the body as the
// avatar walks and turns.
// nearCov (#909): the NEAR self-cover 0..1 (weatherNearCover over the same taps) -- part of open,
// in wind too (the slanted test only sees cover 15 cm and more above: a mouth read open in any
// wind); the rest-pose path applies it on its own (the grid holds the far cover).
bool weatherAvatarShelter(vec3 P, vec3 Ng, uint flags, uint id, out float open, out float depth, out vec3 local,
                          out float nearCov)
{
    open = 1.0; depth = 0.0; local = P; nearCov = 0.0;
    if (weatherShWhich() < 0.5) return false;
    const uint slot = weatherAvatarSlot(flags, id);
    if (slot >= 1024u) return false;   // (AV_SLOT_CAP)
    const ivec2 sz = textureSize(uWeatherOccl[8], 0);
    const int row = sz.y - kWxShSlotRows;
    if (int(slot) >= sz.x || row < 1) return false;
    const vec4 r2 = weatherShFetch(ivec2(int(slot), row + 2));
    const float e = r2.x, res = r2.y;
    if (!(res > 0.5) || !(e > 0.0)) return false;
    const vec2 cxy = weatherShFetch(ivec2(int(slot), row)).xy;
    const vec2 r1 = weatherShFetch(ivec2(int(slot), row + 1)).xy;   // centre z, yaw
    const vec2 o = weatherShFetch(ivec2(int(slot), row + 3)).xy;    // tile corner
    const float texel = e / res;
    const vec3 d = vec3(P.xy + Ng.xy * texel - cxy, P.z - r1.x);
    const vec2 cs = vec2(cos(r1.y), sin(r1.y));
    const vec2 q = vec2(cs.x * d.x + cs.y * d.y, cs.x * d.y - cs.y * d.x);
    {
        const vec3 dp = vec3(P.xy - cxy, d.z);   // (P itself, not the column offset along Ng)
        local = vec3(cs.x * dp.x + cs.y * dp.y, cs.x * dp.y - cs.y * dp.x, dp.z);
    }
    const vec2 tc = (q / e + 0.5) * res;   // tile texel coordinates (continuous)
    if (any(lessThan(tc, vec2(0.0))) || any(greaterThanEqual(tc, vec2(res)))) return true;   // (outside its tile: open)
    // (gather inside the tile: never a neighbour tile's texels)
    const vec2 tcl = clamp(tc, vec2(1.0), vec2(res - 1.0));
    const vec2 uv = (o + tcl) / vec2(sz);
    const vec4 H = weatherShGather(uv, 0);
    const vec2 f = fract(tcl - 0.5);
    const vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    const float tol = 0.008 + 0.5 * texel;
    // The receiver's plane at each tap (gather order (0,1) (1,1) (1,0) (0,0) from the lower-left
    // texel): z + g . (tap - sample), g the plane's slope in the map's frame (steep: clamped).
    const vec2 nl = vec2(cs.x * Ng.x + cs.y * Ng.y, cs.x * Ng.y - cs.y * Ng.x);
    const vec2 g = clamp(-nl / max(Ng.z, 0.25), vec2(-3.0), vec2(3.0));
    const vec4 zr = vec4(d.z) + vec4(dot(g, (vec2(0.0, 1.0) - f) * texel), dot(g, (vec2(1.0, 1.0) - f) * texel),
                                     dot(g, (vec2(1.0, 0.0) - f) * texel), dot(g, (vec2(0.0, 0.0) - f) * texel));
    // a texel with nothing on it is open; one whose top lies more than half a tolerance above the
    // receiver there covers it
    const vec4 op = mix(clamp((zr - (H - 1.5 * tol)) / tol, 0.0, 1.0), vec4(1.0), vec4(lessThan(H, vec4(-1.0e3))));
    open = dot(op, w);
    depth = dot(op * max(weatherShGather(uv, 1), vec4(0.0)), w);
    {
        const vec4 above = H - zr;
        const vec4 nc = vec4(weatherNearCover(above.x), weatherNearCover(above.y), weatherNearCover(above.z), weatherNearCover(above.w));
        nearCov = dot(nc * vec4(greaterThan(H, vec4(-1.0e3))), w);
    }
    // WIND (#901): the precipitation falls slanted (its fall shear): what keeps this fragment dry is
    // what lies over the line it falls along, not over its vertical column -- the canopy's rain
    // shadow moves downwind, and drops slant in under its upwind edge. The line's column at the
    // height dz of the cover: dz from the vertical taps (or a canopy's typical height when nothing
    // is over the fragment), refined twice. (The snow depth stays the vertical column's: the maps
    // are untilted while it snows.)
    const vec2 shw = weatherShear();
    if (dot(shw, shw) > 1.0e-4)
    {
        const vec2 sl = vec2(cs.x * shw.x + cs.y * shw.y, cs.x * shw.y - cs.y * shw.x);   // (the map's frame)
        float dz = 0.0;
        for (int i = 0; i < 4; ++i)
            if (H[i] > -1.0e3) dz = max(dz, H[i] - zr[i]);
        if (dz < 1.5 * tol) dz = 0.6;
        float openT = 1.0;
        for (int it = 0; it < 3; ++it)
        {
            const vec2 tq = ((q - sl * dz) / e + 0.5) * res;
            if (any(lessThan(tq, vec2(0.0))) || any(greaterThanEqual(tq, vec2(res)))) { openT = 1.0; break; }
            const float hs = weatherShFetch(ivec2(o + floor(tq))).x;
            const float above = hs > -1.0e3 ? hs - d.z : -1.0;
            // (a surface well above the fragment -- a brim, a canopy --, not its own slope or a
            // slightly taller body part upwind: the snow coat on a back stays)
            const float need = max(max(1.5 * tol, 0.15), 0.5 * dz);
            openT = clamp((need - above) / tol, 0.0, 1.0);
            if (openT > 0.5 || abs(above - dz) < 0.05) break;
            dz = clamp(above, 0.0, 3.0);
        }
        open = min(openT, 1.0 - nearCov);   // (#909: the near self-cover holds in wind)
    }
    return true;
}

// #901 PER-AVATAR WETNESS (clean_weather_shelter.comp PREP): the fragment's avatar's own wetness --
// x its top surfaces', y its body's (0..1) -- from its slot's row (row + 4). false: none (no atlas,
// no slot, never integrated: the old global path).
bool weatherAvatarWetness(uint flags, uint id, out vec2 W)
{
    W = vec2(0.0);
    if (weatherShWhich() < 0.5) return false;
    const uint slot = weatherAvatarSlot(flags, id);
    if (slot >= 1024u) return false;
    const ivec2 sz = textureSize(uWeatherOccl[8], 0);
    const int row = sz.y - kWxShSlotRows;
    if (int(slot) >= sz.x || row < 1) return false;
    const vec2 r4 = weatherShFetch(ivec2(int(slot), row + 4)).xy;
    if (!(r4.x > -1.0e3)) return false;
    W = clamp(r4, 0.0, 1.0);
    return true;
}

#ifdef WEATHER_REST
// ---- #901 REST-POSE ACCUMULATION (clean_weather_accum.glsl / .comp; ssr.weather[14].w bit 8 = the
// grids live, bit 16 = RenderVKWeatherAvatarAccumDebug) ----
// An avatar with a grid takes its snow and wetness from the cell of its REST-POSE position
// (WEATHER_REST: the VS's v_wx_rest -- xyz the rest position, w the rest normal's z), the
// buckets blended by that normal: what the surface gathered while it was actually exposed. The
// instantaneous shelter test no longer shows or hides anything on it (the compute uses it only to
// gate new accumulation); it drives only the ripples of rain landing right now.
#define WX_AC_SET 0
#define WX_AC_BINDING 50
#include "clean_weather_accum.glsl"
layout(set = 0, binding = 51) uniform sampler3D uWxAccum;   // (linear / clamp; the 1 x 1 x 1 stand-in while off)
uint weatherAccumFlags() { return uint(ssr.weather[14].w + 0.5); }
// snowM: the snow lying on this fragment (m), wet: its wetness (0..1). false: no grid / no rest
// position / nothing sampled around it yet (the per-avatar values apply).
bool weatherAvatarAccum(uint slot, vec4 rest, out float snowM, out float wet)
{
    snowM = 0.0; wet = 0.0;
    if ((weatherAccumFlags() & 8u) == 0u || rest.w < -1.5 || slot >= 1024u) return false;
    const uint gd = ac_slot[slot];
    const uint G = min(ac_head.x, WX_AC_GRIDS);
    if (gd == 0u || gd > G) return false;
    const uint g = gd - 1u;
    const vec4 b0 = ac_box[2u * g], b1 = ac_box[2u * g + 1u];
    const uvec3 res = wxAcRes();
    if (!(b0.w > 0.5) || any(equal(res, uvec3(0u))) || any(lessThanEqual(b1.xyz, b0.xyz))) return false;
    const vec3 cf = (rest.xyz - b0.xyz) / (b1.xyz - b0.xyz) * vec3(res);
    if (any(lessThan(cf, vec3(-1.0))) || any(greaterThan(cf, vec3(res) + 1.0))) return false;   // (not measured yet)
    const vec3 cr = clamp(cf, vec3(0.5), vec3(res) - 0.5);   // (never filter into a neighbour block)
    const vec3 bw = wxAcBucketW(rest.w);
    const vec3 dim = vec3(float(res.x * WX_AC_BUCKETS), float(res.y), float(res.z * G));
    vec4 acc = vec4(0.0);
    for (uint b = 0u; b < WX_AC_BUCKETS; ++b)
    {
        if (bw[b] < 1.0e-3) continue;   // (at most two buckets: one fetch, two in the blend band)
        acc += bw[b] * textureLod(uWxAccum, vec3(float(b * res.x) + cr.x, cr.y, float(g * res.z) + cr.z) / dim, 0.0);
    }
    // PREMULTIPLIED: divided by the filtered occupancy (empty cells never dilute a surface's value)
    if (acc.b < kAcOccMin) return false;
    snowM = acc.r / acc.b * kAcSnowFull;
    wet = clamp(acc.g / acc.b, 0.0, 1.0);
    return true;
}
#endif

uint weatherHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// Four uniform [0, 1] values for an integer cell.
vec4 weatherCellRand(vec2 cell, uint salt)
{
    const ivec2 c = ivec2(cell);
    const uint h = weatherHash(uint(c.x) * 0x8DA6B343u ^ uint(c.y) * 0xD8163841u ^ salt);
    return vec4(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u), float(h >> 24)) * (1.0 / 255.0);
}

// Value noise (puddle layout), 0..1.
float weatherNoise(vec2 p)
{
    const vec2 i = floor(p), f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    const float a = weatherCellRand(i, 101u).x, b = weatherCellRand(i + vec2(1.0, 0.0), 101u).x;
    const float c = weatherCellRand(i + vec2(0.0, 1.0), 101u).x, d = weatherCellRand(i + vec2(1.0, 1.0), 101u).x;
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Gradient (Perlin-style) noise, 0..1: a hashed unit gradient per lattice point, quintic fade.
// Unlike the value noise above it has no lattice-aligned plateaus, so a threshold through it
// (the snow patches, the puddles) gives organic edges instead of square blobs.
float weatherGradNoise(vec2 p)
{
    const vec2 i = floor(p), f = fract(p);
    const vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float g[4];
    for (int k = 0; k < 4; ++k)
    {
        const vec2 o = vec2(float(k & 1), float(k >> 1));
        const float a = 6.2831853 * weatherCellRand(i + o, 211u).x;
        g[k] = dot(vec2(cos(a), sin(a)), f - o);
    }
    return 0.5 + 0.7071 * mix(mix(g[0], g[1], u.x), mix(g[2], g[3], u.x), u.y);
}
// Three rotated octaves of it (the rotations keep the octaves' lattices from lining up), 0..1.
float weatherFbm(vec2 p)
{
    const mat2 r1 = mat2(0.80, 0.60, -0.60, 0.80), r2 = mat2(-0.28, 0.96, -0.96, -0.28);
    return weatherGradNoise(p) * 0.55 + weatherGradNoise(r1 * p * 2.17 + 5.3) * 0.30
         + weatherGradNoise(r2 * p * 4.61 - 11.7) * 0.15;
}

// One ripple layer: the height slope (d/dx, d/dy, per cell unit) at uv. A cell holds at most
// one drop (density: its chance), landing at a hashed point of the cell's middle at a hashed
// phase; its ring grows to ~0.45 cell and dies out. h(s) = sin(3s) exp(-s^2) across the ring.
vec2 weatherRipple(vec2 uv, float t, uint salt, float density)
{
    const vec2 cell = floor(uv);
    const vec4 h = weatherCellRand(cell, salt);
    if (h.w > density) return vec2(0.0);
    const vec2 d = fract(uv) - 0.5 - (h.xy - 0.5) * 0.4;
    const float r = length(d);
    const float age = fract(t * 1.1 + h.z);
    const float s = (r - age * 0.45) * 14.0;
    float amp = 1.0 - age;
    amp *= amp;
    const float dh = (3.0 * cos(3.0 * s) - 2.0 * s * sin(3.0 * s)) * exp(-s * s) * amp;
    return d / max(r, 1e-4) * dh;
}

// WET: darker porous albedo, a water film, puddles and ripples (wet0 > 0, sky > 0). face: the
// surface's facing to the precipitation (weatherFacing, > 0).
void weatherWet(inout SurfaceLit s, vec3 P, float sky, float wet0, float face, float pudBoost)
{
    // Only what the rain hits wets (a wall facing into the wind; not one in its lee or in still
    // air), and flat ground holds the water while walls shed most of it.
    const float up = clamp((s.Ng.z - 0.2) / 0.6, 0.0, 1.0);
    const float wet = wet0 * sky * smoothstep(0.05, 0.45, face) * mix(0.6, 1.0, up);
    // Puddles: low-frequency noise on near-flat surfaces, growing with the wetness.
    float pud = 0.0;
    if (s.Ng.z > 0.97)
    {
        const float n = weatherFbm(P.xy * 0.18);
        const float th = 1.0 - 0.55 * wet0 - 0.45 * pudBoost;   // (M4: puddles seed along a drip line)
        pud = smoothstep(th, th + 0.06, n) * sky;
    }
    // REFLECTIONS (M2): the water film's roughness decides which reflection path a wet surface
    // takes -- nothing separate is added. Puddles (0.02) and VERY wet near-flat ground (0.09, a
    // continuous film: wetness past ~0.6 on surfaces within ~18 degrees of level) sit under the
    // SSR threshold (perceptual roughness <= 0.1, clean_surface_light.glsl) and, on RT hardware,
    // under RenderVKRTReflectionsSharpRough (full ray density where SSR missed); the rest of a
    // wet surface settles near 0.12 (the RT path's glossy, half-density blocks; probes only
    // without RT). The RT pass's receiver-distance gate and GPU ray budget (#859) bound the
    // cost of a rain-soaked street like any other mirror-like floor. Legacy faces: gloss is
    // 1 - roughness on the same scale (puddles 0.98, a wet flat film 0.91, walls 0.85).
    const float film = smoothstep(0.95, 0.99, s.Ng.z) * smoothstep(0.55, 0.8, wet0) * sky;
    if (s.model == SURF_PBR)
    {
        const float porous = 1.0 - s.metal;
        s.albedo *= mix(1.0, 0.55, wet * porous) * mix(1.0, 0.85, pud * porous);
        s.rough = mix(s.rough, min(s.rough, 0.12), wet * 0.85);
        s.rough = mix(s.rough, min(s.rough, 0.09), film);
        s.rough = mix(s.rough, 0.02, pud);
    }
    else
    {
        s.albedo *= mix(1.0, 0.6, wet) * mix(1.0, 0.85, pud);
        s.gloss = mix(s.gloss, max(s.gloss, 0.85), wet * 0.85);
        s.gloss = mix(s.gloss, max(s.gloss, 0.91), film);
        s.gloss = mix(s.gloss, max(s.gloss, 0.98), pud);
        s.spec = max(s.spec, vec3(0.3 * max(wet, pud)));
        s.env = max(s.env, 0.25 * max(wet, pud));
    }
    // Standing water hides the surface's own bumps.
    if (pud > 0.0) s.N = normalize(mix(s.N, s.Ng, pud));
    // Rain ripples: flat, open, near the camera (they would only alias further out).
    const float rain = ssr.weather[0].y;
    const float strength = ssr.weather[0].w;
    if (strength > 0.0 && rain > 0.01 && s.Ng.z > 0.9)
    {
        const float dist = distance(P, ssr.cam_origin.xyz);
        const float fade = (1.0 - smoothstep(12.0, 30.0, dist)) * sky * max(wet0, pud);
        if (fade > 0.0)
        {
            const float t = ssr.weather[0].z;
            const vec2 uv = P.xy * 4.0;   // 25 cm cells
            const vec2 slope = weatherRipple(uv, t, 17u, rain) + weatherRipple(uv + vec2(0.5, 0.37), t * 0.9 + 0.37, 53u, rain);
            s.N = normalize(s.N - vec3(slope * (strength * fade), 0.0));
        }
    }
}

// #901 PER-AVATAR WETNESS on an avatar fragment: `wet` its own (the avatar's state, upward
// surfaces first -- weatherSurface), `rainHere` how much rain falls on this fragment now (the
// ripples only). No puddles, no standing film: a body sheds what a floor holds.
void weatherWetAvatar(inout SurfaceLit s, vec3 P, float wet, float rainHere)
{
    if (s.model == SURF_PBR)
    {
        const float porous = 1.0 - s.metal;
        s.albedo *= mix(1.0, 0.6, wet * porous);
        s.rough = mix(s.rough, min(s.rough, 0.15), wet * 0.8);
    }
    else
    {
        s.albedo *= mix(1.0, 0.65, wet);
        s.gloss = mix(s.gloss, max(s.gloss, 0.82), wet * 0.8);
        s.spec = max(s.spec, vec3(0.25 * wet));
        s.env = max(s.env, 0.2 * wet);
    }
    // rain ripples on a flat, open top (an umbrella's canopy, a hat's crown) while it rains on it
    const float rain = ssr.weather[0].y;
    const float strength = ssr.weather[0].w;
    if (strength > 0.0 && rain > 0.01 && rainHere > 0.0 && s.Ng.z > 0.9)
    {
        const float fade = (1.0 - smoothstep(12.0, 30.0, distance(P, ssr.cam_origin.xyz))) * rainHere * wet;
        if (fade > 0.0)
        {
            const float t = ssr.weather[0].z;
            const vec2 uv = P.xy * 4.0;
            const vec2 slope = weatherRipple(uv, t, 17u, rain) + weatherRipple(uv + vec2(0.5, 0.37), t * 0.9 + 0.37, 53u, rain);
            s.N = normalize(s.N - vec3(slope * (strength * fade), 0.0));
        }
    }
}

// #901 SHELTER DEBUG OVERLAY over the scene (RenderVKWeatherAvatarShelterDebug, non-avatar
// fragments): every shelter layer's tile footprint (white border), and inside it, where the map
// holds a surface over this point, its height above the point in 25 cm bands (yellow .. red: the
// rain is hidden below it); a point that IS the top (within 5 cm) shows green, an empty texel a
// faint blue. rgb + alpha (0: nothing here).
vec4 weatherShDebugWorld(vec3 P)
{
    const ivec2 sz = textureSize(uWeatherOccl[8], 0);
    const int row = sz.y - kWxShSlotRows;
    if (row < 1) return vec4(0.0);
    vec4 outc = vec4(0.0);
    for (int L = 0; L < 16; ++L)
    {
        const float sf = weatherShFetch(ivec2(L, row + 6)).x;
        if (!(sf > -0.5)) continue;
        const int slot = int(sf + 0.5);
        if (slot >= sz.x) continue;
        const vec2 r2 = weatherShFetch(ivec2(slot, row + 2)).xy;
        const float e = r2.x, res = r2.y;
        if (!(res > 0.5) || !(e > 0.0)) continue;
        const vec2 cxy = weatherShFetch(ivec2(slot, row)).xy;
        const vec2 r1 = weatherShFetch(ivec2(slot, row + 1)).xy;
        const vec2 o = weatherShFetch(ivec2(slot, row + 3)).xy;
        const vec2 cs = vec2(cos(r1.y), sin(r1.y));
        const vec2 d = P.xy - cxy;
        const vec2 q = vec2(cs.x * d.x + cs.y * d.y, cs.x * d.y - cs.y * d.x);
        const vec2 uv = q / e + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) continue;
        const float border = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * e;
        if (border < 0.02) { outc = vec4(1.0, 1.0, 1.0, 0.9); continue; }
        const float h = weatherShFetch(ivec2(o + floor(uv * res))).x;
        if (!(h > -1.0e3)) { if (outc.a < 0.25) outc = vec4(0.2, 0.4, 1.0, 0.25); continue; }
        const float above = (r1.x + h) - P.z;
        if (above < 0.05) { if (outc.a < 0.5) outc = vec4(0.1, 1.0, 0.2, 0.5); continue; }
        const float band = fract(above * 4.0) < 0.12 ? 0.6 : 1.0;
        outc = vec4(mix(vec3(1.0, 0.9, 0.1), vec3(1.0, 0.1, 0.05), clamp(above / 2.0, 0.0, 1.0)) * band, 0.7);
    }
    return outc;
}

// SNOW COVER (M1.5, WeatherSnowCoating; SLWeather integrates the cover 0..1): a snow layer
// over the material where the surface faces up (GEOMETRIC normal: a normal map never makes a
// wall hold snow) and the sky is open. Coverage falls off with the slope and toward sheltered
// edges (the soft sky term), and a two-octave noise field breaks the coverage edge: as the
// cover grows its threshold sweeps through the field, so snow settles in patches on the
// flattest, most open spots first instead of fading in uniformly, and melts back the same way.
// The layer: bright, slightly blue-white albedo, rough, non-metallic, with a faint noise
// relief instead of the surface's own bumps (#906: clean_weather_snow_relief.glsl, the snow
// mesh's own; faded by the pixel footprint, sparse glints). Q: where the noise is taken (world
// xy; M2: a moving object's own frame), gate: the field's scale (up x sky, or the persistent
// cover's).
void weatherSnowLayer(inout SurfaceLit s, vec2 Q, float gate, float cover)
{
    const float n = weatherFbm(Q * 0.9);
    const float field = gate * (0.6 + 0.4 * n);
    const float th = 1.0 - cover;
    const float cov = smoothstep(th, th + 0.15, field);
    if (cov <= 0.0) return;
    float glint;
    const vec3 Nsnow = wxSnowRelief(s.Ng, Q, wx_footprint, glint);
    s.N = normalize(mix(s.N, Nsnow, cov));
    const vec3 kSnow = vec3(0.86, 0.89, 0.93);   // fresh snow (linear albedo)
    s.albedo = mix(s.albedo, kSnow, cov);
    if (s.model == SURF_PBR)
    {
        s.rough = mix(s.rough, mix(0.85, 0.3, glint), cov);
        s.metal = mix(s.metal, 0.0, cov);
        s.ao = mix(s.ao, 1.0, cov);
        s.emissive *= 1.0 - cov;
    }
    else
    {
        s.spec = mix(s.spec, vec3(0.04), cov);
        s.gloss = mix(s.gloss, 0.05, cov);
        s.env = mix(s.env, 0.0, cov);
    }
}

// PERSISTENT COVER at P (M2; ssr.weather[10] / [12]): the integrated cover of the grid texels
// around P's column, each taken for the surface it describes -- its top (P within the height
// tolerance of the texel's top: the top cover) or a surface under it (the buried cover) -- and
// bilinearly blended. top: the share of P that is a column top (it keeps the soft sky edge).
// #906 DEPTH IS SNOW: with the depth on (M3) a top texel that holds snow depth is covered
// whatever its integrated cover says -- the cover integrates from 0 over WeatherSnowBuildSeconds
// while the depth (and so the snow mesh, drawn solid white past 2 cm) is already there, and the
// mesh's cut edges (tracks, kerbs, lees, thin edges) used to drop onto a half-covered coating:
// bare dark ground in noise blotches right next to a full white sheet, with the mesh's
// texel-staircase edge. Now the coating continues the sheet (a few mm of depth: full cover).
// false: no grid covers P (the global cover x the openness applies).
bool weatherCoverAt(vec3 P, out float cover, out float top)
{
    cover = 0.0; top = 0.0;
    const vec4 pr = ssr.weather[10], pz = ssr.weather[12];
    if (pr.w < 0.5) return false;
    const vec2 uv = (P.xy - (pz.x - P.z) * pz.yz - pr.xy) * pr.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    // (the gather's taps kept inside the grid: binding 49's sampler clamps to an OPAQUE WHITE
    // border -- the maps' open sky --, which read as full cover / buried cover on the outermost
    // half texel: a thin white line along the grid's edge, indoors too. Clamped: the edge texel
    // replicates.)
    const float res = 1.0 / (pr.z * pz.w);
    const vec2 uvg = clamp(uv, vec2(0.5 / res), vec2(1.0 - 0.5 / res));
    vec4 R, G, B, A;
    if (pr.w < 1.5)
    {
        R = textureGather(uWeatherOccl[4], uvg, 0); G = textureGather(uWeatherOccl[4], uvg, 1);
        B = textureGather(uWeatherOccl[4], uvg, 2); A = textureGather(uWeatherOccl[4], uvg, 3);
    }
    else
    {
        R = textureGather(uWeatherOccl[5], uvg, 0); G = textureGather(uWeatherOccl[5], uvg, 1);
        B = textureGather(uWeatherOccl[5], uvg, 2); A = textureGather(uWeatherOccl[5], uvg, 3);
    }
    const vec2 f = fract(uvg * res - 0.5);
    const vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    const float tol = ssr.weather[11].w;
    const vec4 dz = vec4(P.z) - (vec4(pz.x) - B);
    const vec4 wTop = vec4(1.0) - smoothstep(vec4(tol), vec4(2.0 * tol), abs(dz));
    const vec4 below = vec4(lessThan(dz, vec4(0.0)));
    const vec4 Rd = max(R, smoothstep(vec4(0.002), vec4(0.012), A));   // (#906: depth is snow)
    cover = dot(w, wTop * Rd + (vec4(1.0) - wTop) * below * G);
    top = dot(w, wTop);
    return true;
}

// #783 RenderVKWeatherSnowDepthDebug: the depth grid's view of P (nearest texel of the published
// grid; the depth's surface is the texel's FILTERED base, its top cover / top height the raw one):
//   magenta  a MOVING object (instance bit21: kept out of the maps; its own snow, and its box may
//            press the ground's snow under it)
//   red      under the region's water line (no snow by rule)
//   dark grey no grid here (past the inner grid: the global cover / the outer ring)
//   yellow   covered per the NEAR map (P is not the column's top there: the inner snow grid's base is
//            what lies above -- the snow there is ON that cover, not on P)
//   cyan     open per the near map but covered per the COARSE map (the inner snow grid never reads
//            the coarse map: only the coating's openness and the outer ring do)
//   orange   P is the texel's top but no depth (< 2 cm) -- the coating only: the depth was zeroed or
//            capped (support cap / slope / a mover's press / a dropped base)
//   blue -> green -> white  depth 2 cm .. 0.5 m .. 2 m
// Over all of it: the inner grid's texel borders darkened, the coarse map's texel borders red.
// #783 RenderVKWeatherSnowDepthDebug: the texel grids at world point P -- x: 1 on a border line of
// the INNER snow grid's texels (the near map's columns, ~6 cm), y: 1 on a border of the COARSE
// map's texels (~0.6 m) -- so a stair / tooth's size names the map that made it.
vec2 wxDebugGridLines(vec3 P)
{
    vec2 r = vec2(0.0);
    {
        const vec4 pr = ssr.weather[10], pz = ssr.weather[12];
        if (pr.w > 0.5)
        {
            const vec2 uv = (P.xy - (pz.x - P.z) * pz.yz - pr.xy) * pr.z;
            const vec2 f = abs(fract(uv / (pr.z * pz.w)) - 0.5);
            r.x = max(f.x, f.y) > 0.44 ? 1.0 : 0.0;
        }
    }
    {
        const vec4 cr = ssr.weather[1], cz = ssr.weather[2];
        if (cr.w > 0.5 && cz.w > 0.0)
        {
            const vec2 uv = (P.xy - (cz.x - P.z) * ssr.weather[6].xy - cr.xy) * cr.z;
            const vec2 f = abs(fract(uv / (cr.z * cz.w)) - 0.5);
            r.y = max(f.x, f.y) > 0.47 ? 1.0 : 0.0;
        }
    }
    return r;
}
vec3 weatherSnowDepthDebugBase(vec3 P, uint flags);
vec3 weatherSnowDepthDebug(vec3 P, uint flags)
{
    vec3 c = weatherSnowDepthDebugBase(P, flags);
    const vec2 gl = wxDebugGridLines(P);
    c = mix(c, c * 0.35, gl.x);
    return mix(c, vec3(1.0, 0.0, 0.0), gl.y);
}
vec3 weatherSnowDepthDebugBase(vec3 P, uint flags)
{
    if ((flags & 2097152u) != 0u) return vec3(1.0, 0.0, 1.0);
    if (P.z < ssr.water[3].w - 0.02) return vec3(1.0, 0.0, 0.0);
    const vec4 pr = ssr.weather[10], pz = ssr.weather[12];
    if (pr.w < 0.5) return vec3(0.15);
    const vec2 uv = (P.xy - (pz.x - P.z) * pz.yz - pr.xy) * pr.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.15);
    const float res = 1.0 / (pr.z * pz.w);
    const ivec2 q = clamp(ivec2(uv * res), ivec2(0), ivec2(int(res) - 1));
    const vec4 c = pr.w < 1.5 ? texelFetch(uWeatherOccl[4], q, 0) : texelFetch(uWeatherOccl[5], q, 0);
    const float top = pz.x - c.b;
    if (P.z < top - ssr.weather[11].w) return vec3(1.0, 1.0, 0.0);   // (the near map's top is above P)
    {
        // the coarse map's column top above P while the near map's is not
        const vec4 cr = ssr.weather[1], cz = ssr.weather[2];
        if (cr.w > 0.5)
        {
            const vec2 cuv = (P.xy - (cz.x - P.z) * ssr.weather[6].xy - cr.xy) * cr.z;
            if (all(greaterThanEqual(cuv, vec2(0.0))) && all(lessThan(cuv, vec2(1.0))))
            {
                const ivec2 cq = ivec2(cuv * vec2(textureSize(uWeatherOccl[0], 0)));
                const float dd = cr.w < 1.5 ? texelFetch(uWeatherOccl[0], cq, 0).r : texelFetch(uWeatherOccl[1], cq, 0).r;
                if (dd < 1.0 && P.z < cz.x - dd * cz.y - max(ssr.weather[11].w, 2.0 * cz.w)) return vec3(0.0, 1.0, 1.0);
            }
        }
    }
    if (c.a < 0.02) return vec3(1.0, 0.5, 0.0);
    const float t = clamp(c.a / 0.5, 0.0, 1.0), u = clamp((c.a - 0.5) / 1.5, 0.0, 1.0);
    return mix(mix(vec3(0.0, 0.2, 1.0), vec3(0.0, 1.0, 0.2), t), vec3(1.0), u);
}

// SNOW (M1.5 coating, M2 persistence): moving objects their own amount in their own frame;
// static surfaces the persistent cover where the grid covers them, else the global cover x
// the current openness. flags / id: the fragment's instance flags and slot.
void weatherSnow(inout SurfaceLit s, vec3 P, float sky, float snow0, uint flags, uint id, bool avMap, float avOpen, float avDepth,
                 vec3 avLocal)
{
    const float up = smoothstep(0.4, 0.82, s.Ng.z);   // none past ~66 degrees, full under ~35
    if (up <= 0.0) return;
    // #901 PER-AVATAR SNOW: an avatar with a shelter map carries the snow its map integrated
    // (clean_weather_shelter.comp: built on its top surfaces, domes on small supports, slides off
    // steep ones, shed by running, melting under cover) -- the depth at P's column, only where P is
    // that column's top (avOpen); deep snow is the avatar snow mesh on top, this the coat under its
    // thin edges. Without a map: the M3 amount below.
    if (avMap)
    {
        // (avDepth is already only the snow lying on THIS fragment -- nothing under a canopy / a
        // jaw; the gate is the facing alone. The pattern in the avatar's own frame -- tilted with
        // height, as the M3 coat's -- so it stays where it landed on the body; the depth itself is
        // integrated in that frame too.)
        if (!(avDepth > 5e-4)) return;
        weatherSnowLayer(s, vec2(avLocal.x + 0.61 * avLocal.z, avLocal.y - 0.47 * avLocal.z) * 1.7, up,
                         clamp(avDepth / 0.01, 0.0, 1.0));
        return;
    }
    // AVATARS (M3; rigged bit0 -- flexi chains bit19 carry no slot -- and worn rigid bit9): the
    // avatar's own amount, never the grid / global cover.
    if ((flags & (1u | 512u)) != 0u)
    {
        const float avRow = ssr.weather[6].z;
        if (avRow < 0.5) return;
        uint slot = 0xFFFFFFFFu;
        if ((flags & 1u) != 0u) { if ((id & 0x80000000u) != 0u) slot = id & 0xFFFFu; }
        else if (id < WX_INST_COUNT()) slot = WX_INST_SKIN(id);
        if (slot >= 1024u) return;   // (AV_SLOT_CAP)
        const int row = int(avRow + 0.5) - 1;
        const vec4 a0 = texelFetch(uWeatherOccl[6], ivec2(int(slot), row), 0);
        if (!(a0.x > 0.0)) return;
        const vec3 o = P - texelFetch(uWeatherOccl[6], ivec2(int(slot), row + 1), 0).xyz;
        const vec2 cs = vec2(cos(a0.y), sin(a0.y));
        const vec2 q = vec2(cs.x * o.x + cs.y * o.y, cs.x * o.y - cs.y * o.x);   // the chest's frame (yaw)
        weatherSnowLayer(s, vec2(q.x + 0.61 * o.z, q.y - 0.47 * o.z), up, a0.x);
        return;
    }
    // MOVING OBJECTS (instance bit21; not rigged / HUD / terrain / worn): their own snow.
    if ((flags & 2097152u) != 0u && (flags & (1u | 8u | 32u | 512u)) == 0u)
    {
        if (ssr.weather[11].x < 0.5 || id >= WX_INST_COUNT()) return;
        const uint iw = max(uint(ssr.weather[11].y + 0.5), 1u);
        if (id / iw >= uint(ssr.weather[11].x + 0.5)) return;   // (past the image: not tracked yet)
        const float amt = texelFetch(uWeatherOccl[6], ivec2(int(id % iw), int(id / iw)), 0).r;
        if (!(amt > 0.0)) return;
        // The object's own frame (metres along its axes): the patches ride with it.
        const mat4 M = WX_INST_MODEL(id);
        const vec3 d = P - M[3].xyz;
        const vec3 o = vec3(dot(d, M[0].xyz) * inversesqrt(max(dot(M[0].xyz, M[0].xyz), 1e-12)),
                            dot(d, M[1].xyz) * inversesqrt(max(dot(M[1].xyz, M[1].xyz), 1e-12)),
                            dot(d, M[2].xyz) * inversesqrt(max(dot(M[2].xyz, M[2].xyz), 1e-12)));
        weatherSnowLayer(s, vec2(o.x + 0.61 * o.z, o.y - 0.47 * o.z), up, amt);
        return;
    }
    // #783 WATER: no static snow under the region's water surface (a river / lake bed seen
    // through the water kept the coating; the persistent grid holds none there either:
    // clean_weather_snow.comp wxSubmerged).
    if (P.z < ssr.water[3].w - 0.02) return;
    // (weatherCoverAt zeroes its outputs before it knows whether a grid covers P: keep the
    // global cover x openness in its own variables so a miss leaves them intact)
    float cover = snow0, gate = sky, gcover, top;
    if (weatherCoverAt(P, gcover, top)) { cover = gcover; gate = mix(1.0, sky, top); }
    if (gate <= 0.0 || cover <= 0.0) return;
    weatherSnowLayer(s, P.xy, up * gate, cover);
}

// ---- M4 RUN-OFF ----
// The flow map at P's column: x the falloff-weighted catchment of the nearest outlet (bilinear),
// y its edge height, z its landing height, w the flow through the texel (nearest). false: no
// flow map covers P.
bool weatherFlowAt(vec3 P, out vec4 fl)
{
    fl = vec4(0.0);
    const vec4 fr = ssr.weather[13], fz = ssr.weather[14];
    const vec2 uv = (P.xy - (fz.x - P.z) * fz.yz - fr.xy) * fr.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    const vec2 res = vec2(textureSize(uWeatherOccl[7], 0));
    fl = texelFetch(uWeatherOccl[7], clamp(ivec2(uv * res), ivec2(0), ivec2(res) - 1), 0);
    const vec2 uvg = clamp(uv, 0.5 / res, 1.0 - 0.5 / res);   // (taps inside: the border is opaque white)
    const vec4 g = textureGather(uWeatherOccl[7], uvg, 0);
    const vec2 f = fract(uvg * res - 0.5);
    fl.x = dot(g, vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)));
    return true;
}

// RIVULETS AND SHEET FLOW on a steep surface (|Ng.z| < 0.85). amount 0..1: the water the
// surface carries (the rain it takes, an outlet above it, its own drainage); acc 0..1: how much
// has gathered there (the flow map's per-texel upslope flow / the discharge from above) -- the
// downstream measure. Water reads as GLOSS, not as lines: a smooth wet film, lower roughness,
// a slight darkening of porous surfaces, highlights flowing downslope.
//   TRICKLES (light rain, upslope): thin threads along the level set of a flow-aligned gradient
//            noise (squeezed across, stretched along the slope), so neighbouring threads meander,
//            braid and run into each other instead of lying parallel; a low-frequency presence
//            noise keeps only some of them in light rain.
//   CHANNELS (downslope, more water): the same at ~6x the scale -- fewer, wider streams; the
//            trickles hand over to them as the accumulated flow grows (they merge downslope).
//   SHEET    (heavy rain / a lot of flow): the threads fade into one continuous water film over
//            the whole surface with a downslope-moving ripple sheen.
// Everything that moves scrolls continuously along the slope (no dashes, nothing pops); the
// channel layout itself is fixed to the surface, as real water finds the same low lines.
float weatherRidge(float n, float w) { return 1.0 - smoothstep(0.5 * w, w, abs(n - 0.5)); }
void weatherRivulets(inout SurfaceLit s, vec3 P, float amount, float acc)
{
    const float dist = distance(P, ssr.cam_origin.xyz);
    const float fade = 1.0 - smoothstep(12.0, 24.0, dist);   // (they would only alias further out)
    amount *= fade;
    if (amount <= 0.01) return;
    const vec2 h2 = s.Ng.xy;
    const float lh = length(h2);
    if (lh < 0.05) return;
    // The surface's frame: T horizontal across it, D straight down along it.
    const vec3 T = vec3(-h2.y, h2.x, 0.0) / lh;
    const vec3 Dr = cross(s.Ng, T);
    const vec3 D = normalize(Dr.z > 0.0 ? -Dr : Dr);
    const float u = dot(P, T);
    const float v = dot(P, D);   // grows downslope
    const float t = ssr.weather[0].z;
    const float rain = ssr.weather[0].y;
    acc = clamp(acc, 0.0, 1.0);
    // The regime: a sheet in heavy rain on a well-wetted surface or under a lot of flow.
    const float sheet = smoothstep(0.5, 0.85, max(rain * smoothstep(0.25, 0.7, amount), 0.8 * acc * amount));
    // Threads: a slow, flow-aligned warp (braiding), then the level-set lines.
    const float warp = (weatherGradNoise(vec2(u * 1.3, v * 0.25) + 3.7) - 0.5) * 0.35;
    const float uf = u + warp * 0.3, uc = u + warp;
    const float nf = weatherGradNoise(vec2(uf / 0.045, v / 0.5));
    const float nc = weatherGradNoise(vec2(uc / 0.28, v / 2.2) + 11.3);
    // Light rain: only some trickles run (presence by a coarse noise against the amount).
    const float present = smoothstep(0.0, 0.25, amount - (1.0 - weatherGradNoise(vec2(u / 0.4, v / 1.8) + 5.1)) * 0.8);
    const float wf = mix(0.025, 0.07, amount);
    const float wc = mix(0.03, 0.14, acc) * (0.5 + amount);
    const float mf = weatherRidge(nf, wf) * present * (1.0 - smoothstep(0.2, 0.8, acc));
    const float mc = weatherRidge(nc, wc) * smoothstep(0.1, 0.6, acc + 0.3 * amount);
    const float threads = max(mf, mc) * (1.0 - sheet);
    // The film everywhere the water runs: the threads, or the whole surface as a sheet.
    const float film = max(threads, sheet * amount);
    if (film <= 0.003) return;
    // Flowing highlights: a soft noise scrolling downslope (0.5 - 1.2 m/s with the water).
    const float speed = mix(0.5, 1.2, max(acc, sheet));
    const float glint = weatherGradNoise(vec2(u / 0.12, (v - t * speed) / 0.35) + 17.0);
    // The normal: a rounded crown across a thread, and (sheet / glints) a downslope-moving
    // ripple whose phase wanders with the noise (no bars).
    const float crossF = clamp((nf - 0.5) / max(wf, 1e-3), -1.0, 1.0) * mf;
    const float crossC = clamp((nc - 0.5) / max(wc, 1e-3), -1.0, 1.0) * mc;
    const float ripple = sin(6.2831853 * ((v - t * speed) * 2.5 + 1.5 * glint)) * film;
    s.N = normalize(s.N + T * ((crossF + crossC) * 0.18 * (1.0 - sheet)) + D * (ripple * 0.05));
    // Water is gloss: low roughness, a slight darkening of porous materials, a brighter spec.
    const float gl = film * (0.75 + 0.25 * smoothstep(0.45, 0.8, glint));
    if (s.model == SURF_PBR)
    {
        s.albedo *= mix(1.0, 0.85, film * (1.0 - s.metal));
        s.rough = mix(s.rough, mix(0.08, 0.03, smoothstep(0.5, 0.8, glint)), gl);
    }
    else
    {
        s.albedo *= mix(1.0, 0.88, film);
        s.gloss = mix(s.gloss, max(s.gloss, 0.94 + 0.04 * glint), gl);
        s.spec = max(s.spec, vec3(0.3 * gl));
        s.env = max(s.env, 0.3 * gl);
    }
}

// The run-off below an outlet at P: 0..1 (a wall between an outlet's edge and its landing).
float weatherRunoffBelow(vec3 P, vec4 fl)
{
    if (!(fl.x > 0.0) || P.z > fl.y + 0.05 || P.z < fl.z - 0.1) return 0.0;
    return ssr.weather[13].w * smoothstep(0.05, 1.5, fl.x);
}

void weatherSurface(inout SurfaceLit s, vec3 P)
{
    const float wet0 = ssr.weather[0].x;
    const float snow0 = ssr.weather[5].x;
    // #901: an avatar fragment with its own wetness state runs even with the world dry (an avatar
    // that walked out of the rain, or teleported, dries on its own clock); the shelter debug
    // overlay too.
    const bool avFrag = (WEATHER_INST_FLAGS & (1u | 512u)) != 0u;
    const bool shOn = ssr.weather[6].w > 0.5;
    if (wet0 <= 0.0 && snow0 <= 0.0 && !(shOn && (avFrag || weatherShDebug()))) return;   // weather off / dry, no snow: one uniform branch
    if (s.model != SURF_PBR && s.model != SURF_LEGACY) return;
    if (!avFrag && shOn && weatherShDebug())
    {
        // RenderVKWeatherAvatarShelterDebug: the shelter maps' footprints / heights over the scene.
        const vec4 dc = weatherShDebugWorld(P);
        if (dc.a > 0.0) s.albedo = mix(s.albedo, dc.rgb, dc.a);
    }
    if (wet0 <= 0.0 && snow0 <= 0.0 && !avFrag) return;
#ifdef WEATHER_REST
    // #901 REST-POSE ACCUMULATION: the avatar's grid drives its coat and its wetness
    if (avFrag && (weatherAccumFlags() & 8u) != 0u) wx_near = wxAcNear();   // (#909: the settings, while the record is live)
    if (avFrag && shOn && !weatherShDebug() && (weatherAccumFlags() & 8u) != 0u)
    {
        float acSnow, acWet;
        const bool acc = weatherAvatarAccum(weatherAvatarSlot(WEATHER_INST_FLAGS, WEATHER_INST_ID), WEATHER_REST, acSnow, acWet);
        // #909 NEAR SELF-COVER, live per fragment: the grid's ~4 cm cells mix a tongue with the
        // snout's top over it (one cell, one "up" bucket, and the trilinear filter's neighbours);
        // what the avatar's own surface covers right above THIS fragment shows no snow. (The
        // wetness shown stays the grid's: a soaked avatar's throat is wet too; the grid's samples
        // inside the mouth take no new wetness -- clean_weather_accum.comp acOwnOpen.)
        float shO = 1.0, shD, nearCov = 0.0;
        vec3 shL;
        weatherAvatarShelter(P, s.Ng, WEATHER_INST_FLAGS, WEATHER_INST_ID, shO, shD, shL, nearCov);
        if ((weatherAccumFlags() & 16u) != 0u)
        {
            // RenderVKWeatherAvatarAccumDebug: grey .. blue its wetness, white its snow (from the grid);
            // magenta: no grid value (the per-avatar fallback), orange: no rest position (no skin meta /
            // bind shape for the part; cluster LOD parts carry theirs since the #901 / #909 follow-up);
            // #909 cyan: near self-covered (shows no snow whatever the grid holds)
            vec3 c = WEATHER_REST.w < -1.5 ? vec3(1.0, 0.45, 0.0) : !acc ? vec3(1.0, 0.0, 1.0)
                   : mix(mix(vec3(0.25), vec3(0.1, 0.35, 1.0), acWet), vec3(1.0), clamp(acSnow / 0.01, 0.0, 1.0));
            c = mix(c, vec3(0.0, 0.9, 0.9), smoothstep(0.25, 0.75, nearCov));
            s.albedo = mix(s.albedo, c, 0.8);
            return;
        }
        if (acc)
        {
            acSnow *= 1.0 - nearCov;
            const float face = weatherFacing(s.Ng, weatherShear());
            // the ripples only: rain landing on this fragment NOW (a canopy, a hat's crown)
            float rainHere = 0.0;
            if (wet0 > 0.0 && acWet > 0.003 && ssr.weather[0].y > 0.01 && ssr.weather[0].w > 0.0 && s.Ng.z > 0.9 && face > 0.0)
                rainHere = weatherSkyOpen(P, s.Ng) * shO * smoothstep(0.05, 0.45, face);
            if (acWet > 0.003) weatherWetAvatar(s, P, acWet, rainHere);
            if (acSnow > 5.0e-4)
            {
                const vec3 V = ssr.cam_origin.xyz - P;
                const float dv = length(V);
                wx_footprint = WX_FOOTPRINT(wxSnowFootprint(dv, dot(V, s.Ng) / max(dv, 1e-4)));
                // (the pattern in the REST pose: the patches stay where they landed on the body; the
                // gate soft -- a part that faced up when the snow fell may have turned since)
                const vec3 r = WEATHER_REST.xyz;
                weatherSnowLayer(s, vec2(r.x + 0.61 * r.z, r.y - 0.47 * r.z) * 1.7, smoothstep(-0.2, 0.5, s.Ng.z),
                                 clamp(acSnow / 0.01, 0.0, 1.0));
            }
            return;
        }
    }
#endif
    vec2 avW = vec2(0.0);
    const bool avWet = avFrag && weatherAvatarWetness(WEATHER_INST_FLAGS, WEATHER_INST_ID, avW);
    // M4 run-off (one uniform test while off): the flow map at P.
    const float run = wet0 > 0.0 ? ssr.weather[13].w : 0.0;
    const uint rflags = uint(ssr.weather[14].w + 0.5);
    vec4 fl = vec4(0.0);
    const bool flow = run > 0.0 && (rflags & 1u) != 0u && weatherFlowAt(P, fl);
    const bool rivulets = run > 0.0 && (rflags & 2u) != 0u && abs(s.Ng.z) < 0.85;
    // Turned away from the precipitation (ceilings, roof undersides, M2: a wall's lee side):
    // never rained / snowed on. (M4: a lee wall under an outlet still streams.) #901: an avatar
    // with its own wetness goes on -- soaked through, its undersides are wet too.
    const float face = weatherFacing(s.Ng, weatherShear());
    if (face <= 0.0 && !avWet)
    {
        if (rivulets && flow)
        {
            const float below = weatherRunoffBelow(P, fl);
            weatherRivulets(s, P, below * smoothstep(0.0, 0.3, wet0), below);
        }
        return;
    }
    float sky = face > 0.0 ? weatherSkyOpen(P, s.Ng) : 0.0;
    {
        // #906: the pixel footprint for the snow relief (distance / grazing estimate: this runs
        // after per-pixel branches, where derivatives are not reliable).
        const vec3 V = ssr.cam_origin.xyz - P;
        const float dv = length(V);
        wx_footprint = WX_FOOTPRINT(wxSnowFootprint(dv, dot(V, s.Ng) / max(dv, 1e-4)));
    }
    // #901: an avatar's own cover (an umbrella, a hat): its map's surfaces above this fragment.
    float avOpen = 1.0, avDepth = 0.0, avNear = 0.0;
    vec3 avLocal = P;
    const bool avMap = avFrag && weatherAvatarShelter(P, s.Ng, WEATHER_INST_FLAGS, WEATHER_INST_ID, avOpen, avDepth, avLocal, avNear);
    if (avMap) sky *= avOpen;
    if (avFrag && weatherShDebug())
    {
        // RenderVKWeatherAvatarShelterDebug: what the shelter map says about this fragment (the
        // tint's brightness: its wetness -- dark dry .. full wet; no wetness state: unscaled).
        // #909 cyan: NEAR self-covered (the avatar's own surface close above: a mouth, a throat).
        const vec3 c = !avMap ? vec3(1.0, 0.0, 1.0) : avDepth > 1e-3 ? vec3(0.1, 0.3, 1.0)
                     : avNear > 0.5 ? vec3(0.0, 0.9, 0.9) : avOpen < 0.5 ? vec3(1.0, 0.05, 0.05) : vec3(0.05, 1.0, 0.1);
        const float Wd = avWet ? mix(avW.y, avW.x, avMap ? avOpen : 1.0) : 1.0;
        s.albedo = mix(s.albedo, c * (0.35 + 0.65 * Wd), 0.7);
        return;
    }
    if (avWet)
    {
        // #901 PER-AVATAR WETNESS: the avatar's own state, not the world's -- a column's top takes
        // the top state (a canopy out in the rain, wetting on its own clock), a part under the avatar's own cover
        // the body's; upward surfaces first, the sides as it soaks, the undersides last.
        const float W = mix(avW.y, avW.x, avMap ? avOpen : 1.0);
        const float thr = 0.35 * (1.0 - clamp(s.Ng.z, -1.0, 1.0));   // (0 up, 0.35 sideways, 0.7 down)
        const float Wf = clamp((W - thr) / (1.0 - thr), 0.0, 1.0);
        if (Wf > 0.0) weatherWetAvatar(s, P, Wf, wet0 > 0.0 ? sky * smoothstep(0.05, 0.45, face) : 0.0);
        if (snow0 > 0.0 && face > 0.0)
            weatherSnow(s, P, sky, snow0, WEATHER_INST_FLAGS, WEATHER_INST_ID, avMap, avOpen, avDepth, avLocal);
        return;
    }
    // M4 LANDING: the ground under a drip line -- wetter sooner, puddles along it, sheltered or not.
    float wetL = wet0, skyL = sky, pudB = 0.0;
    if (flow && (rflags & 4u) != 0u && s.Ng.z > 0.6)
    {
        const float dz = abs(P.z - fl.z);
        const float b = run * smoothstep(0.05, 1.2, fl.x) * (1.0 - smoothstep(0.2, 0.4, dz));
        wetL = clamp(wet0 + b, 0.0, 1.0);
        skyL = max(sky, b);
        pudB = b;
    }
    if (wetL > 0.0 && skyL > 0.0) weatherWet(s, P, skyL, wetL, max(face, pudB), pudB);
    if (rivulets)
    {
        // The rain this surface takes, the water from an outlet above it, a steep roof's own flow.
        // acc: how much has gathered here -- the discharge from an outlet above, a steep roof's
        // own drainage (the flow map's upslope length: thin near the ridge, more toward the eave).
        float amt = wet0 * sky * smoothstep(0.05, 0.45, face) * 0.7;
        float acc = 0.0;
        if (flow)
        {
            const float below = weatherRunoffBelow(P, fl);
            amt += below;
            acc = below;
            if (s.Ng.z > 0.3)
            {
                const float own = smoothstep(0.2, 4.0, fl.w);
                amt += run * own * sky;
                acc = max(acc, run * own);
            }
        }
        weatherRivulets(s, P, clamp(amt, 0.0, 1.0), acc);
    }
    // (over the wet surface: snow hides it; a sheltered surface may still hold persistent snow)
    if (snow0 > 0.0) weatherSnow(s, P, sky, snow0, WEATHER_INST_FLAGS, WEATHER_INST_ID, avMap, avOpen, avDepth, avLocal);
    // #783 RenderVKWeatherSnowDepthDebug (ssr.weather[14].w bit 32): what the depth grid says about
    // this surface -- why a surface shows the coating but no snow mesh.
    if ((uint(ssr.weather[14].w + 0.5) & 32u) != 0u && (WEATHER_INST_FLAGS & (1u | 512u)) == 0u)
    {
        const vec3 dc = weatherSnowDepthDebug(P, WEATHER_INST_FLAGS);
        s.albedo = dc;
        s.emissive = max(s.emissive, dc * 0.5);
    }
}

#endif   // CLEAN_WEATHER_SURFACE_GLSL
