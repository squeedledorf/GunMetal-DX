// vkclean — local-light data shared by clean_lights.comp and clean_draw.frag.
// Byte layout matches LLCleanRendererVK's VKLightGPU / VKLightInfo (std430).

#define GX 16u
#define GY 9u
#define GZ 24u
#define CLUSTER_COUNT (GX * GY * GZ)
#define CLUSTER_CAP 64u   // <= 127 (clusterShadowedLen packs it in 7 bits)
// CAPTURE LIGHT CAP (#829): the reflection-probe capture FS (clean_draw.frag -DCLEAN_CAPTURE)
// walks only the nearest this many lights of its probe's list; clean_probe_gpu.comp phaseLights
// sorts a list by distance to the probe origin whenever it holds more.
#define CAPTURE_LIGHT_CAP 16u

// cluster_count[c] packing (clean_lights.comp; clean_probe_gpu.comp's capture lists set the
// list length only):
//   bits  0-7   list length (<= CLUSTER_CAP)
//   bits  8-14  how many SHADOWED projectors (tex.z != 0) lead the list -- they are listed
//               first so the cap never drops one while it has room (the RT shadow mask and
//               the stochastic sampler trace only the projectors its cluster lists: one the
//               cap dropped is lit UNSHADOWED by the lossless froxel / tile lists, white tile
//               x depth-slice blocks where the cap overflowed -- bit 15, view 26 magenta)
//   bit   15    the cluster's shadowed projectors alone exceeded the cap
//   bits 16-31  every light that touched the cluster (> CLUSTER_CAP: overflow)
// THE REST SLOT (exact projector shadows, RenderVKProjStochastic 0): the RT shadow mask
// (clean_rt_shadow_mask.comp) and the deferred SHADOW resolve (clean_deferred_light.comp
// deferResolveProjectors) keep 4 visibility slots per pixel. When more shadowed projectors
// light the pixel, slots 0-2 keep their own projectors and slot 3 becomes the REST slot: every
// other one of them traced and folded into one light-weighted visibility ratio (sum of
// light x vis / sum of light, the light being the projector's unshadowed gobo x attenuation at
// the pixel). A reader that does not find its projector in a slot takes the REST slot's value
// when there is one -- never visibility 1 (the 5th+ projector used to be lit UNSHADOWED: a club's
// projector in the next room painted its gobo through the wall). Over the rest set the ratio
// reproduces the shadowed sum exactly where the readers' light equals the producer's weight.
#define PROJ_MASK_REST_ID 0xFFFFu   // the RT mask's ids (rgba16ui; light index + 1 elsewhere)
#ifndef PROJ_DEFER_REST_ID          // (clean_deferred_light.comp defines it before this include)
#define PROJ_DEFER_REST_ID 0xFFu    // the deferred SHADOW slots' 8-bit ids (lights 0..253 listed)
#endif

uint clusterLen(uint raw)            { return min(raw & 0xFFu, CLUSTER_CAP); }
uint clusterShadowedLen(uint raw)    { return min((raw >> 8) & 0x7Fu, clusterLen(raw)); }
bool clusterShadowedOverflow(uint raw) { return (raw & 0x8000u) != 0u; }
uint clusterCandidates(uint raw)     { return raw >> 16; }

struct Light {
    vec4 pos_radius;     // world pos, radius (LL deferred size)
    vec4 color_falloff;  // linear rgb, falloff
    vec4 proj_params;    // focus, range, lod range, ambiance
    vec4 proj_origin;    // apex .xyz, .w = 1 projector / 0 point
    vec4 proj_axis;      // forward axis .xyz, .w near clip (legacy projector reflection plane)
    mat4 proj_mat;       // world -> projector texcoord
    uvec4 tex;           // .x bindless index, .y = 1 if textured
};                       // 160 B

struct LightInfo {
    vec4 cam_pos;        // .xyz camera world pos, .w light count
    vec4 cam_fwd;        // .xyz camera forward (world)
    vec4 zinfo;          // near, far (draw distance), GZ / log(far / near), projection far
    vec4 grid;           // tile px w, tile px h, tan(half fov x), tan(half fov y)
    mat4 view;           // world -> view (-Z forward)
    vec4 cull_planes[6]; // the main CULL frustum (ax+by+cz+d >= 0 inside; the frozen one under
                         // "Freeze culling camera"): clean_probe_gpu.comp skips probes off it (#517)
    // ---- FROXEL GRID (RenderVKLightCulling 2; clean_light_froxel.comp builds, the deferred
    //      lighting reads: froxelSlice below) ----
    mat4  fx_vp;         // the main view's JITTERED P * rotation-only V: (world - fx_eye) -> VK clip,
                         // the matrix the deferred lighting inverts (its inv_vp_rel)
    vec4  fx_eye;        // .xyz the main view's draw origin (its pc.u_cam_time.xyz), .w 0
    vec4  fx_slices;     // .x end of slice 0 (m), .y (slices - 1) / ln(far / near), .z far, .w 0
    uvec4 fx_dims;       // tiles x, tiles y, depth slices, log2(tile px)
    vec4  fx_extent;     // render extent w, h (px), 1 / w, 1 / h
    // ---- FORWARD LIT FS (the main view's forward surfaces: alpha-blend, forward opaque) ----
    uvec4 fwd;           // .xy the FROXEL MASK buffer's device address (lo, hi) while the forward
                         // walk reads it this frame (RenderVKForwardFroxels + RenderVKLightCulling
                         // 2 + the masks built for this extent), 0 = walk the cluster lists;
                         // .z options: bit 0 = alpha-blend receivers trace ray-traced projector
                         // shadows inline (RenderVKRTBlendProjShadows 1); .w 0
};

// ---- FROXEL LIGHT MASKS (RenderVKLightCulling 2) ----
// Cell (tile tx, ty of 2^fx_dims.w render px; depth slice s) = FROXEL_WORDS uints at
// ((s * fx_dims.y + ty) * fx_dims.x + tx) * FROXEL_WORDS: bit li set = light li may reach a
// surface in the cell (256 lights = MAX_VK_LIGHTS: one bit each, it can never overflow).
// View depth vz = dot(P - fx_eye, cam_fwd), the build's and the lookup's one definition.
// Slice 0: vz <= fx_slices.x (and NaN); 1 .. n-1: log-spaced over [fx_slices.x, far], the last
// one open-ended (everything beyond far). Monotone non-decreasing in vz (the build bounds a
// light's slice run by the slices of its depth interval's two ends).
#define FROXEL_WORDS 8u
uint froxelSlice(float vz, vec4 fs, uint nz)
{
    if (!(vz > fs.x)) return 0u;
    return 1u + uint(min(log(vz / fs.x) * fs.y, float(nz - 2u)));   // (clamped before the uint: inf-safe)
}
// The depth interval of slice s (slice 0 from -1e30, the last to 1e30): what froxelSlice maps
// into s, up to the rounding of log / exp (the build widens it).
vec2 froxelSliceRange(uint s, vec4 fs, uint nz)
{
    const float z0 = s == 0u ? -1e30 : fs.x * exp(float(s - 1u) / fs.y);
    const float z1 = s == 0u ? fs.x : (s + 1u >= nz ? 1e30 : fs.x * exp(float(s) / fs.y));
    return vec2(z0, z1);
}
