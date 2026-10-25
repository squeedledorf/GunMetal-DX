// vkclean — shared bindings + push for the particle compute chain
// (sim -> emit -> compact -> sort -> indirect). One descriptor set, 9 storage buffers.
#include "clean_particle_common.glsl"

struct DrawIndirectCommand { uint vertexCount; uint instanceCount; uint firstVertex; uint firstInstance; };

layout(std430, set = 0, binding = 0) buffer Particles { Particle parts[]; };
layout(std430, set = 0, binding = 1) readonly buffer Emitters { Emitter emit[]; };
layout(std430, set = 0, binding = 2) buffer FreeList  { uint free_idx[]; };
layout(std430, set = 0, binding = 3) buffer FreeCount { uint free_count; };
// The draw lists (#518: one per pass, culled to that pass's view), N = pool size:
//   [0, N)              MAIN: one (sort key, pool index) per pool slot, sorted ascending
//                       by key (clean_particle_sort.comp). key = segment (water side x
//                       blend pair) << 25 | inverted view distance, so each segment is
//                       contiguous and back to front; dead AND off-view slots carry
//                       0xFFFFFFFF and sort to the end.
//   [N, 2N)             MIRROR (u_cull.x & CULL_MIRROR): the same, keyed by the
//                       distance from the REFLECTED eye, culled to the mirror frustum.
//   [2N + c*N, +N)      CASCADE c (u_cull.x & CULL_CASCADE(c)): unsorted (shadow
//                       depth / transmittance products are order-free), light-blocking
//                       pairs only, appended by atomic slot; .x unused.
// Segment s of a sorted list occupies [cmds[..].firstInstance, + instanceCount).
layout(std430, set = 0, binding = 4) buffer DrawList  { uvec2 draw_list[]; };
// Counts: [0, n) main per segment, [n, 2n) mirror per segment, [2n, 2n + 4) per
// cascade, then PART_STATS words (n = u_num_buckets).
layout(std430, set = 0, binding = 5) buffer DrawCount { uint draw_count[]; };
// Indirect commands: [0, n) main, [n, 2n) mirror, [2n, 2n + 4) cascades.
layout(std430, set = 0, binding = 6) buffer Indirect  { DrawIndirectCommand cmds[]; };
// Per emitter slot: the newest ribbon particle it emitted {index, serial, source id,
// valid} -- the next one links to it (LL LLViewerPartSource::mLastPart).
layout(std430, set = 0, binding = 7) buffer RibbonTail { uvec4 rtail[]; };

// Per-pass cull volumes (#518), CPU-written each frame by vkCmdUpdateBuffer (read-only
// here; nothing on the GPU writes it). = LLCleanRendererVK::PartCullGPU (976 B; every
// member a vec4, so no std430 padding). Planes: dot(xyz, p) + w >= 0 inside, unit normals.
struct PartCull {
    vec4 main_planes[6];       // the main GPU cull's frustum (FROZEN under Freeze culling camera)
    vec4 mirror_planes[6];     // the planar mirror's cull frustum (near = the mirror plane)
    vec4 mirror_cam;           // xyz: reflected eye (mirror depth-sort keys)
    vec4 shadow_planes[24];    // cascade c: [c * 6, + 6) its ortho caster box
    vec4 caster_planes[24];    // cascade c: [c * 6, + 6) the view-slice caster planes (pass-all = 0,0,0,1)
};
layout(std430, set = 0, binding = 8) readonly buffer PartCullBuf { PartCull pcull; };

// u_cull.x bits.
const uint CULL_MIRROR = 1u;                                // build the mirror list
uint CULL_CASCADE(uint c) { return 2u << c; }               // build cascade c's list
// Stats (draw_count[2n + 4 + k]; read back for the hitch counters, never for culling).
const uint STAT_LIVE = 0u, STAT_MAIN = 1u, STAT_MIRROR_ONLY = 2u, STAT_SHADOW_ONLY = 3u;
const uint PART_STATS = 4u;

layout(push_constant) uniform PartPush {
    float u_dt;
    uint  u_frame;
    uint  u_max_particles;
    uint  u_num_buckets;    // draw segments per sorted list (water side x blend pair)
    vec4  u_shift;      // xyz: one-frame agent-origin re-base (region crossing), else 0
    vec4  u_cam;        // xyz: main camera (depth sort), w: water height
    uvec4 u_ctl;        // x: live cap (RenderMaxPartCount), y: camera under water,
                        // z / w: bitonic sort stage k / j (clean_particle_sort.comp)
    uvec4 u_cull;       // x: CULL_* list bits (#518), y: sorted entries (N, or 2N with
                        // the mirror list: the sort's length), z / w: 0
} pp;
