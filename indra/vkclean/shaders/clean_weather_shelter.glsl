// vkclean — PER-AVATAR SHELTER MAPS (#901; vkgfx/llcleanweathervk_shelter.cpp,
// clean_weather_shelter.comp): the shared record of the shelter kernels and the precipitation
// simulation (clean_weather_sim.comp: rain hidden under / splashing on an umbrella).
//
// Up to WX_SH_LAYERS avatars near the camera each own one LAYER: a square TILE of the shelter
// ATLAS (rg32f, two ping-pong images: x the top surface's height in the avatar's frame, m above
// its pelvis -- kShNone where nothing of it lies on that column --, y M3+ the per-avatar snow
// depth there, m), laid out by the CPU per tier configuration (256^2 tiles for the own avatar and
// the closest, 128^2 for the rest). A layer's FRAME follows the avatar every frame: centre = its
// pelvis (world), yaw = the pelvis's heading, extent (m) = the square the tile covers, grown to its
// worn attachments' reach (an open umbrella) and capped -- so the map moves and turns with the
// avatar and nothing smears; heights are stored relative to the pelvis.
//
// The record (std430; set / binding / access chosen by the includer: WX_SH_SET, WX_SH_BINDING,
// WX_SH_ACCESS):
//   head   x work items appended (the raster's list; past y: dropped), y the list's capacity,
//          z frames since the CPU last wrote (unused), w the atlas's per-slot rows' first row
//   disp   the raster's indirect dispatch (x workgroups = the items, 1, 1)
//   mesh   M3+: x the avatar snow mesh's triangles appended, y their capacity, z / w unused
//   cfg    x the layers in use (<= WX_SH_LAYERS), y which atlas image is written this step (0 / 1),
//          z the extent cap (m, float bits), w the minimum extent (m, float bits)
//   info[L]  (CPU) x the joint-table slot (0xFFFFFFFF: free), y the tile's resolution, z the
//          tile's corner x | y << 16 (atlas texels), w flags: 1 due (re-rastered this step),
//          2 reset (newly assigned: no history), 4 its avatar's rest-pose accumulation grid is
//          due (#901 follow-up: clean_weather_accum.glsl -- the filter lists its draws too)
//   count[L] (FILTER) the raster work items appended for layer L this step
//   frame[L] (PREP) xyz centre (the pelvis, world), w yaw (rad)
//   ext[L]   (PREP) x extent (m), y the previous step's extent (m), z 1 = reset this step, w unused
//   prev[L]  (PREP) xyz the previous step's centre, w 1 = valid
//   reach[L] (RASTER) the farthest |local x / y| of what it drew (m, float bits: positive floats
//          order like their bits -- atomicMax)
//   wet[L]   (PREP) the layer's avatar's PER-AVATAR WETNESS: x its top surfaces' wetness, y its
//          body's (0..1), z the top's exposure, w the body's (0 covered .. 1 open; the log / CPU)
//   attach[L] (FILTER) the worn rigid attachments among count[L] (the log)
//   stat     (the simulation; cumulative, wrapping -- the CPU takes differences) x rain drops
//          hidden by a shelter, y drops splashed on one; z (PREP) wetness resets applied, w unused
//   rhead / rst[8]  (CPU) the wetness resets this step: rhead.x entries, rst[i] x the joint-table
//          slot, y the start wetness (packHalf2x16(top, body); 0xFFFFFFFF: the guess -- the global
//          wetness x the avatar's exposure now)
//   items[]  (FILTER) x the instance id, y the layer | 0x100 raster due | 0x200 #901 accumulation due
//
// PER-SLOT ROWS (kShSlotRows = 7, under the tiles, one column per joint-table slot, BOTH images):
//   row + 0 (centre x, y), + 1 (centre z, yaw), + 2 (extent, tile resolution -- 0: no layer),
//   + 3 (tile corner x, y), + 4 (top wetness, body wetness -- x < -1e3: never set), + 5 (top
//   exposure, body exposure); + 6 columns 0..15: the LAYER DIRECTORY (layer L's slot, -1 free).
//   The lit FS (clean_weather_surface.glsl) finds its avatar's by its slot; row = height - 7.
#ifndef CLEAN_WEATHER_SHELTER_GLSL
#define CLEAN_WEATHER_SHELTER_GLSL

#define WX_SH_LAYERS 16u
const float kShNone = -1.0e4;        // the atlas's "no surface on this column"
const float kShNoneTest = -1.0e3;    // (anything below this is "none")

#ifndef WX_SH_ACCESS
#define WX_SH_ACCESS readonly
#endif
layout(std430, set = WX_SH_SET, binding = WX_SH_BINDING) WX_SH_ACCESS buffer WxShelter
{
    uvec4 sh_head;
    uvec4 sh_disp;
    uvec4 sh_mesh;
    uvec4 sh_cfg;
    uvec4 sh_info[WX_SH_LAYERS];
    uint  sh_count[WX_SH_LAYERS];
    vec4  sh_frame[WX_SH_LAYERS];
    vec4  sh_ext[WX_SH_LAYERS];
    vec4  sh_prev[WX_SH_LAYERS];
    uint  sh_reach[WX_SH_LAYERS];
    vec4  sh_wet[WX_SH_LAYERS];
    uint  sh_attach[WX_SH_LAYERS];
    uvec4 sh_stat;
    uvec4 sh_rhead;
    uvec2 sh_rst[8];
    uvec2 sh_items[];   // (word 408: LLCleanWeatherVK::kShRecordBytes = 1632)
};
#define WX_SH_SLOT_ROWS 7
// #783 BODY CONTACT: the RASTER TARGET (r32ui, set 1 binding 0 of the shelter chain; the snow
// passes' set 1 binding 8) is 2 x the tiles' rows + WX_SH_BODY_ROWS tall: rows [0, th) the top
// map (atomicMax of wxShEnc(height)), rows [th, 2 th) the BOTTOM map (the same tiles; atomicMax of
// wxShEnc(-height): the lowest surface of the avatar per texel, m above its pelvis; 0 nothing),
// then per layer L (column L) its RASTER-TIME frame -- 2 th + 0 the slot + 1 (0: none), + 1..4
// centre x, y, z, yaw (float bits), + 5 extent (float bits), + 6 tile resolution, + 7 tile corner
// (x | y << 16) -- written by CLEAR when the layer is rasterized, so a map not re-rastered this
// step stays where its avatar was (clean_weather_snow.comp BODY CONTACT).
#define WX_SH_BODY_ROWS 8

// A world point P in layer L's frame: x forward, y left (m), z above the pelvis (m).
vec3 wxShLocal(uint L, vec3 P)
{
    const vec4 fr = sh_frame[L];
    const vec2 cs = vec2(cos(fr.w), sin(fr.w));
    const vec3 d = P - fr.xyz;
    return vec3(cs.x * d.x + cs.y * d.y, cs.x * d.y - cs.y * d.x, d.z);
}
// The world point of layer L's local point q.
vec3 wxShWorld(uint L, vec3 q)
{
    const vec4 fr = sh_frame[L];
    const vec2 cs = vec2(cos(fr.w), sin(fr.w));
    return fr.xyz + vec3(cs.x * q.x - cs.y * q.y, cs.y * q.x + cs.x * q.y, q.z);
}
// Layer L's tile texel under local xy (false: outside the tile).
bool wxShTexel(uint L, vec2 q, out ivec2 t)
{
    const float e = sh_ext[L].x;
    const int res = int(sh_info[L].y);
    t = ivec2(0);
    if (!(e > 0.0) || res <= 0) return false;
    const vec2 uv = q / e + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return false;
    t = ivec2(int(sh_info[L].z & 0xFFFFu), int(sh_info[L].z >> 16u)) + ivec2(uv * float(res));
    return true;
}
// Order-preserving float <-> uint (atomicMax on the words = on the floats; 0 never encodes a
// finite value: the raster target's clear value, "nothing drawn").
uint wxShEnc(float f) { const uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u); }
float wxShDec(uint e) { return uintBitsToFloat((e & 0x80000000u) != 0u ? (e & 0x7FFFFFFFu) : ~e); }

#endif   // CLEAN_WEATHER_SHELTER_GLSL
