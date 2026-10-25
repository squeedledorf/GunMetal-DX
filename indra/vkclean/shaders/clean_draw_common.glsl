// vkclean — shared world fragment-shader core (clean_draw.frag + clean_water.frag):
// vertex inputs, material/light/probe/SSR bindings, the draw push block, and the
// lighting helpers (local lights, SSR tap, reflection-probe sampling).
// [[unroll]]: small fixed-trip loops over private arrays are unrolled so every index is a
// constant -- a private array indexed at run time lives in local memory, not registers.
#extension GL_EXT_control_flow_attributes : require
#include "clean_shader_clock.glsl"   // CLK_MARK (RenderVKShaderClockLighting kernels; else empty)
#include "clean_lights_common.glsl"
#include "clean_probes_common.glsl"
#include "clean_cel_common.glsl"   // cel shading parameters (#550): binding 37

// Screen position of the shaded sample (render pixels, pixel centre). Fragment shaders:
// gl_FragCoord; the deferred lighting compute (clean_deferred_light.comp) defines it as
// its own pixel centre before including this file.
#ifndef CLEAN_FRAG_XY
#define CLEAN_FRAG_XY gl_FragCoord.xy
#endif

#ifdef CLEAN_COMPUTE_LIGHT
// DEFERRED LIGHTING compute (RenderVKDeferredLighting): no vertex inputs. Every G-buffer
// pixel is an opaque / mask world surface: no alpha-blend (2), HUD (8) or other bits the
// shared lighting tests.
const uint v_flags = 0u;
#else
layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 2)      in vec3 v_world_normal;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;      // bit 32 = terrain splat
layout(location = 5)      in vec3 v_world_pos;   // world XY -> terrain detail UV
#ifndef CLEAN_NO_TANGENT_IN
layout(location = 7)      in vec4 v_world_tangent; // w = handedness; zero = no tangent
layout(location = 8)      in vec2 v_uv_raw;        // pre-transform texcoord (per-map UVs);
                                                   // terrain: region-local XY (meters)
layout(location = 9) flat in uint v_terrain_rec;   // terrain: TerrainRecord index (skin_id)
#endif
#include "clean_mat_uv.glsl"

layout(location = 0) out vec4 outColor;
#endif   // CLEAN_COMPUTE_LIGHT

// 96 bytes — matches LLCleanMaterialStore::Material. The two 64-bit bindless
// handles per map are stored as uvec2 pairs here; base_color_handle.x carries the
// descriptor-array index (Vulkan-native substitute for the GL GLuint64 handle).
struct Material {
    uvec2 base_color_handle;   // .x = bindless texture INDEX in Vulkan
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;               // bit0 has_base ...
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };

layout(set = 1, binding = 0) uniform sampler2D uTextures[];

#ifndef CLEAN_NO_TANGENT_IN   // world draws only (particles never shade terrain)
// Per-region terrain record (LLCleanMaterialStore::TerrainRecord, 176 B), indexed by
// the terrain instance's skin_id. mode 0 = legacy 4-texture splat (detail UV =
// region-local XY * legacy.z + legacy.xy, RenderTerrainScale), 1 = PBR: mat[] are four
// glTF materials, each sampled at xf[2i] / xf[2i+1] (u' = dot(row0.xyz, (x, y, 1))) of
// the region-local XY. misc = (paint_idx reserved, mode, RenderTerrainPBRDetail as
// int, paint type).
struct TerrainRecord {
    uvec4 mat;
    uvec4 misc;
    vec4  legacy;
    vec4  xf[8];
};
layout(std430, set = 0, binding = 22) readonly buffer TerrainRecords { TerrainRecord terrain_records[]; };
#endif

// Ray-query variant (RenderVKRayTracing on): the scene TLAS + RT geometry tables at
// RT_SET (LLCleanRTVK's consumer set; clean_draw.frag uses set 2, clean_particle.frag
// set 3). The set layout is PARTIALLY_BOUND and the TLAS is only touched when a
// projector record says "ray traced" (params.z = 2, set on the CPU only when this
// frame's TLAS was built).
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#include "clean_rt_common.glsl"
#endif

// Local lights, clustered by clean_lights.comp.
layout(std430, set = 0, binding = 3) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 4) readonly buffer CCount { uint cluster_count[]; };
layout(std430, set = 0, binding = 5) readonly buffer CIndex { uint cluster_index[]; };
// Per-frame light info: a UNIFORM BUFFER (#814; std140 == std430 here -- LightInfo is vec4 /
// mat4 / uvec4 / vec4[6] only, every offset a multiple of 16 under both rules). Draw set 0
// binding 6 is UNIFORM_BUFFER; the light / froxel / probe-cluster / GPU-probe compute sets
// still read the same buffer as an SSBO (the buffer carries both usages).
layout(std140, set = 0, binding = 6) uniform LInfo { LightInfo linfo; };

// ---- LIT GUARDS (white / pink / cyan tile hunt, 2026-10-13) ----
// Every shading term that sums untrusted data (a light record, a probe cube texel, last
// frame's colour through SSR) goes through lgFinite: a non-finite value becomes 0, a
// finite one beyond the HDR targets' range (RGBA16F: 65504 -> inf on store, and inf x 0 =
// NaN downstream) is clamped. Each trip sets its term's bit in g_lgBits (RenderVKDebugView
// 27 paints them) and, in the deferred lighting compute (CLEAN_LIT_GUARD_COUNTERS), counts
// into draw set 0 binding 20 words 4.. (LLCleanRendererVK::readLitGuardStats logs them:
// "VK lit guard: ..."). Words 0-3 there are the overdraw counter's.
#define LG_LOCAL      0u   // a local light's contribution
#define LG_PROBE      1u   // the probe radiance / irradiance blend
#define LG_SSR        2u   // an SSR hit (last frame's colour)
#define LG_OUT        3u   // the final lit colour
#define LG_PROBE_IDX  4u   // a probe list entry past the probe count (skipped)
#define LG_LIGHT_IDX  5u   // a light list entry past the light count (skipped)
#define LG_MAX_VALUE  65000.0
uint g_lgBits = 0u;
#ifdef CLEAN_LIT_GUARD_COUNTERS
// [0..3] overdraw (clean_draw.frag OverdrawCounter); [4 + LG_*] trips; [4 + 6 ..] the
// deferred pass's cluster scan (clean_deferred_light.comp lgClusterScan).
layout(std430, set = 0, binding = 20) buffer LitGuardCounters { uint lg_words[]; };
#define LG_WORD0 4u
#endif
void lgFlag(uint k) { g_lgBits |= 1u << k; }
void lgNote(uint k)
{
    lgFlag(k);
#ifdef CLEAN_LIT_GUARD_COUNTERS
    atomicAdd(lg_words[LG_WORD0 + k], 1u);
#endif
}
vec3 lgFinite(vec3 c, uint k)
{
    if (any(isnan(c)) || any(isinf(c))) { lgNote(k); return vec3(0.0); }
    if (any(greaterThan(c, vec3(LG_MAX_VALUE)))) { lgNote(k); return min(c, vec3(LG_MAX_VALUE)); }
    return c;
}

// Screen-space reflections: the PREVIOUS frame's lit scene + depth, traced by
// reprojecting into its view (LL screenSpaceReflUtil: sceneMap is last frame too).
layout(set = 0, binding = 7) uniform sampler2D uPrevColor;
layout(set = 0, binding = 8) uniform sampler2D uPrevDepth;
// Planar mirror capture (this frame's reflected view, main-view screen UVs).
layout(set = 0, binding = 17) uniform sampler2D uMirror;
#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"

// Reflection probes (LL reflectionProbeF): probe list, per-cluster candidates, the
// prefiltered radiance cube-map array and the irradiance cube-map array.
layout(std430, set = 0, binding = 10) readonly buffer Probes { ProbeHeader phdr; Probe probes[]; };
layout(std430, set = 0, binding = 11) readonly buffer PCount { uint probe_count[]; };
layout(std430, set = 0, binding = 12) readonly buffer PIndex { uint probe_index[]; };
// [0] the radiance array (RenderVKProbeFormat: RGBA16F / RGB9E5 / R11G11B10 / BC6H);
// [1] the LIVE array (#565, format 3 only -- else the same view as [0]): uncompressed
// cubes for the default probe and the realtime probe (Probe.idx.y bits 1-2, written by
// clean_probe_gpu.comp: 1 = live cube 0, 2 = live cube 1, 0 = the slot in [0]).
layout(set = 0, binding = 13) uniform samplerCubeArray uProbeRadiance[2];
layout(set = 0, binding = 14) uniform samplerCubeArray uProbeIrradiance;

// The draw's view block (pc.u_*, same as the VS) + the 16 B push; #238.
#include "clean_draw_view.glsl"

// Probe CAPTURE draws set push sun_color.w = 1: no SSR / probe sampling there (all
// main-camera data; sampling probes would also feed back). Local lights come from the
// capture's own culled list (cluster CLUSTER_COUNT + water_fog.w, see localLights).
// MIRROR capture (planar hero probe) draws set sun_color.w = 2: full lighting, but
// the light/probe clusters and SSR are main-camera screen data -- lights are walked
// directly and probes looked up by projecting into the main view (mainCluster).
// CAPTURE FS (#829, clean_draw.frag -DCLEAN_CAPTURE): only ever bound by probe faces, so
// both are constants there and every gate below folds at compile time (the push still
// carries sun_color.w = 1 for the other modules of the pass).
#ifdef CLEAN_CAPTURE
bool probeCapture() { return true; }
bool mirrorPass() { return false; }
#else
bool probeCapture() { return pc.u_sun_color.w > 0.5 && pc.u_sun_color.w < 1.5; }
bool mirrorPass() { return pc.u_sun_color.w > 1.5; }
#endif

// ---- LIT-PASS COST SWITCHES (RenderVKLitDebugMask -> ssr.lit_dbg.x; live A/B with the
// GPU timers). Each bit drops or reduces one fragment-shading term of the world lit FS so
// its share of g_lit_static (and the alpha / water passes, which share this code) can be
// measured. Main view only: probe and mirror captures always shade in full (their content
// would otherwise be baked from a crippled scene). 0 = normal rendering.
const uint LDBG_NO_LOCAL       = 1u;      // local lights (clustered point + projector loop)
const uint LDBG_NO_PROJ_SHADOW = 2u;      // projector visibility = 1 (no atlas / RT-mask / ray reads)
const uint LDBG_NO_PROBES      = 4u;      // reflection probes off (sky colour + sky ambient)
const uint LDBG_PROBE0_ONLY    = 8u;      // probes: default probe 0 only (no cluster walk)
const uint LDBG_NO_SSR         = 16u;     // screen-space reflections off
const uint LDBG_NO_SUN_SHADOW  = 32u;     // sun / moon visibility = 1 (no RT-mask / cascade reads)
const uint LDBG_MASK_NEAREST   = 64u;     // RT shadow mask: 1 nearest texel (no 4-tap depth-aware upsample)
const uint LDBG_NO_HAZE        = 128u;    // atmospheric haze / water fog application off
const uint LDBG_NO_MAPS        = 256u;    // material normal / ORM-specular / emissive maps not fetched
const uint LDBG_NO_LEGACY_ENV  = 512u;    // legacy shiny env (sharp lod-0 probe walk + SSR) off
const uint LDBG_NO_AO          = 1024u;   // SSAO read off
const uint LDBG_NO_MIRROR      = 2048u;   // planar mirror tap off
const uint LDBG_VRS_VIEW       = 4096u;   // tint by the fragment's shading rate (2x1 red, 1x2 green, 2x2 blue)
// DEBUG SPECIALIZATION (kDebugViews): clean_draw.frag and clean_deferred_light.comp declare
// it as specialization constant 1 (CLEAN_DEBUG_CONST) -- the shipping lit pipelines are built
// with it FALSE, so every debug path below (these cost switches, the RenderVKDebugView terms,
// Highlight Transparent / blend-tolerance tints, the RT terminator view) folds away in the
// driver; LLCleanRendererVK builds the true twins lazily while a debug feature is on. Every
// other includer (water, particles, ...) keeps the debug paths as a plain constant.
#ifndef CLEAN_DEBUG_CONST
const bool kDebugViews = true;
#endif
bool litDbg(uint bit)
{
#ifdef CLEAN_GBPROBE_NO_DEBUG
    return false;   // (G-buffer register probe, measurement only)
#else
    return kDebugViews && (uint(ssr.lit_dbg.x + 0.5) & bit) != 0u && !probeCapture() && !mirrorPass();
#endif
}
// RenderVKDebugView as the lit FS sees it (frame info params1.z; 0 = normal shading).
int litDebugView() { return kDebugViews ? int(ssr.params1.z + 0.5) : 0; }


// ---- PBR (glTF metallic-roughness), forward ----
// srgbEncode, PI and the BRDF terms (EnvBRDFApprox, llPunctual, ...) are shared with
// the ray-traced hit shading (clean_rt_shade.glsl).
#include "clean_brdf.glsl"

#ifndef CLEAN_COMPUTE_LIGHT   // (screen derivatives: fragment stage only)
// Normal mapping without a tangent attribute: cotangent frame from screen-space
// derivatives of world position and UV (Schueler). Handles mirrored UVs. B follows
// increasing v of the sampled UVs, as LL's shaders use B = sign*cross(N,T).
vec3 perturbNormal(vec3 N, vec3 p, vec2 uv, vec3 tn)
{
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (!(m > 1e-20)) return N;                 // degenerate UVs: keep geometric normal
    float inv = inversesqrt(m);
    return normalize(mat3(T * inv, B * inv, N) * tn);
}

// Normal map -> world normal with LL's per-vertex frame (pbropaqueF / materialF):
// vB = w * cross(vN, vT), n = normalize(tn.x*vT + tn.y*vB + tn.z*vN), then flipped
// for back faces like LL. No tangent (terrain/water/planar/animated) -> the
// derivative frame built on the already-oriented normal.
vec3 applyNormalMap(vec3 Ngeo, vec4 T, vec3 p, vec2 uv, vec3 tn, bool front)
{
    if (dot(T.xyz, T.xyz) > 1e-12)
    {
        vec3 vB = T.w * cross(Ngeo, T.xyz);
        vec3 n  = normalize(tn.x * T.xyz + tn.y * vB + tn.z * Ngeo);
        return front ? n : -n;
    }
    return perturbNormal(front ? Ngeo : -Ngeo, p, uv, tn);
}

// LEGACY BUMP PRESET (#19) tangent normal at the base-colour UV (LL's bump pass samples
// the bump map with the diffuse texcoords). False when the source map isn't resident
// (or no preset). Steps one texel of the resident image (LL generated the normal map at
// the source's current size); the taps keep the mip bias, so distant bumps flatten as
// LL's mipmapped normal maps did.
#include "clean_normal_tex.glsl"
bool legacyBumpTN(Material m, vec2 uv, float bias, out vec3 tn)
{
    tn = vec3(0.0, 0.0, 1.0);
    const uint code = legacyBumpCode(m.flags);
    if (code == 0u) return false;
    const bool lum = code <= 2u;
    if ((m.flags & (lum ? 1u : 2u)) == 0u) return false;
    const uint th = lum ? m.base_color_handle.x : m.normal_handle.x;
    const vec2 st = 1.0 / vec2(max(textureSize(uTextures[nonuniformEXT(th)], 0), ivec2(1)));
    const vec4 xm = texture(uTextures[nonuniformEXT(th)], uv - vec2(st.x, 0.0), bias);
    const vec4 xp = texture(uTextures[nonuniformEXT(th)], uv + vec2(st.x, 0.0), bias);
    const vec4 ym = texture(uTextures[nonuniformEXT(th)], uv - vec2(0.0, st.y), bias);
    const vec4 yp = texture(uTextures[nonuniformEXT(th)], uv + vec2(0.0, st.y), bias);
    if (lum)
        tn = legacyBumpNormal(legacyBumpLum(srgbEncode(xm.rgb), code), legacyBumpLum(srgbEncode(xp.rgb), code),
                              legacyBumpLum(srgbEncode(ym.rgb), code), legacyBumpLum(srgbEncode(yp.rgb), code), code);
    else
        tn = legacyBumpNormal(xm.a, xp.a, ym.a, yp.a, code);
    return true;
}
#endif   // !CLEAN_COMPUTE_LIGHT



// ---- Local lights: ports of LL's deferred point (multiPointLightF) and projector
//      (spotLightF) lighting, evaluated forward per fragment over its cluster. ----
#include "clean_light_eval.glsl"   // legacyAtten, projSample / projDiffuse / projAmbiance



#ifdef CLEAN_FWD_FROXEL
// The froxel light masks (FROXEL_WORDS uints per cell) at LightInfo.fwd.xy (localLights).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FwdFroxelRef { uint w[]; };
#endif

// Main-view light/probe cluster of this fragment (screen tile + log depth slice).
uint mainClusterFrag(vec3 P)
{
    uint cx = min(uint(CLEAN_FRAG_XY.x / linfo.grid.x), GX - 1u);
    uint cy = min(uint(CLEAN_FRAG_XY.y / linfo.grid.y), GY - 1u);
    float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    uint cz = (vz <= linfo.zinfo.x) ? 0u
            : min(uint(log(vz / linfo.zinfo.x) * linfo.zinfo.z), GZ - 1u);
    return (cz * GY + cy) * GX + cx;
}

// The main-view cluster a world point projects into, clamped to the grid edges (the
// mirror capture sees points the main view doesn't: they take the nearest edge
// tile's candidates -- approximate, but the probes there are still local ones).
uint mainClusterProject(vec3 P)
{
    vec4 v = linfo.view * vec4(P, 1.0);        // -Z forward
    float z = max(-v.z, 1e-3);
    float sx = (v.x / z) / max(linfo.grid.z, 1e-4);   // [-1,1] across the view
    float sy = (v.y / z) / max(linfo.grid.w, 1e-4);
    uint cx = uint(clamp((sx * 0.5 + 0.5) * float(GX), 0.0, float(GX) - 1.0));
    uint cy = uint(clamp((0.5 - sy * 0.5) * float(GY), 0.0, float(GY) - 1.0));   // top-down tiles
    float vz = -v.z;
    uint cz = (vz <= linfo.zinfo.x) ? 0u
            : min(uint(log(vz / linfo.zinfo.x) * linfo.zinfo.z), GZ - 1u);
    return (cz * GY + cy) * GX + cx;
}

// RT FORWARD REFLECTIONS (#859; clean_deferred_light.comp CLEAN_DEFER_PASS 14): the front-most
// alpha-blend / water layer's ray-traced reflection, traced in compute before the forward pass at
// half resolution (rgb, a = that layer's view depth; 0 = none; a 1 x 1 zero placeholder while
// RenderVKRTReflectionsForward is off). The main view's forward lit FS (transparent fragments) and
// the water FS take it as their environment radiance where the depth matches -- a texture read,
// no ray query in either (#849). Particles / captures: not compiled / never applied.
#if !defined(CLEAN_COMPUTE_LIGHT) && !defined(CLEAN_CAPTURE) && !defined(CLEAN_NO_TANGENT_IN)
#define CLEAN_RT_FWD_AVAILABLE
layout(set = 0, binding = 48) uniform sampler2D uRtFwd;
vec4 rtFwdIn(vec3 P)
{
    if (probeCapture() || mirrorPass()) return vec4(0.0);
    const ivec2 sz = textureSize(uRtFwd, 0);
    const vec4 v = texelFetch(uRtFwd, clamp(ivec2(CLEAN_FRAG_XY * 0.5), ivec2(0), sz - 1), 0);
    if (!(v.a > 0.0) || any(isnan(v.rgb)) || any(isinf(v.rgb))) return vec4(0.0);
    const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    if (abs(vz - v.a) > 0.02 * vz + 0.05) return vec4(0.0);   // (another layer / surface here)
    return vec4(max(v.rgb, vec3(0.0)), 1.0);
}
// surfaceReflectionsW's hook: transparent (alpha-blend, v_flags bit 1) fragments only.
#define CLEAN_REFL_RT_FWD(P) ((v_flags & 2u) != 0u ? rtFwdIn(P) : vec4(0.0))
#endif

// Planar mirror (LL tapHeroProbe, planar path): inside the mirror probe's box, blend
// the mirror capture (same screen pixel -- it was rendered with the reflected view
// and this view's projection) over the environment reflection, faded at the box
// edge and by smoothness. Main view only. tapMirrorWeight: the blend weight alone (0 =
// no mirror content at P; the split lighting's half-resolution reflections keep the
// pixels a strong weight reaches at full resolution).
float tapMirrorWeight(vec3 P, float glossiness)
{
    if (!specMirrorOn() || mirrorPass() || probeCapture() || litDbg(LDBG_NO_MIRROR)) return 0.0;
    vec3 ls = (ssr.mirror_box * vec4(P, 1.0)).xyz;
    float contain = 1.0 - max(max(abs(ls.x), abs(ls.y)), abs(ls.z));
    if (contain <= 0.0) return 0.0;
    // only the reflective (viewer) side of the mirror plane
    if (dot(ssr.mirror_plane.xyz, P) + ssr.mirror_plane.w < -0.05) return 0.0;
    return smoothstep(0.0, 0.15, contain) * smoothstep(0.0, 0.5, glossiness);
}
vec3 tapMirror(vec3 env, vec3 P, float glossiness)
{
    const float w = tapMirrorWeight(P, glossiness);
    if (w <= 0.0) return env;
    // The capture is unjittered; under TAA this fragment shows the scene at
    // gl_FragCoord - jitter (render pixels, y down; zero when not jittered) -- sampling
    // the capture at the jittered position shook the reflection by a pixel (#204).
    // (#860) mirror_params.w = 1: the RT mirror image (RenderVKRTMirrors) is traced at the render
    // extent at this frame's jittered pixel centres -- read where this fragment is, no shift.
    vec2 uv = (CLEAN_FRAG_XY - ssr.taa_jitter.xy * (1.0 - ssr.mirror_params.w)) * ssr.mirror_params.yz;
    return mix(env, textureLod(uMirror, uv, 0.0).rgb, w);
}

// ---- Shadow-map filtering (SlipstreamRenderShadowFilterQuality -> ssr.render_opts.x) ----
// After upstream Alchemy's shadowUtil (e8b487a816):
//   0 Low     one hardware 2x2 compare tap (bilinear PCF)
//   1 Medium  4x4 Gaussian from 4 gather-compares
//   2 High    6x6 Gaussian from 9 gather-compares
//   3 Ultra   PCSS contact hardening (sun / moon; projectors take High): a blocker
//             search on the raw depth sizes the penumbra per pixel
// The Gaussians are apodized (the window-edge value subtracted off) so a texel entering
// or leaving the fixed window carries ~zero weight instead of popping. Probe captures
// take Low: the capture resolves to a small prefiltered cube, so a wide kernel buys
// nothing there.
int shadowFilterQuality()
{
    return probeCapture() ? 0 : specFilterQuality();   // (clean_frame_info.glsl: spec constant 8)
}
float shadowIGN(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
vec2 shadowNoisePx()
{
    vec2 px = CLEAN_FRAG_XY;
    if (ssr.taa_params.w > 0.5) px += 97.0 * (ssr.taa_jitter.xy + 0.5);
    return px;
}
// Separable apodized Gaussian over a K-texel window starting at texel index base, for a
// receiver at texel-space position c. K even, <= 6.
void shadowGaussW(int K, vec2 c, vec2 base, out float wx[6], out float wy[6])
{
    const float sigma  = float(K) * 0.25;
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);
    const float edge   = float(K) * 0.5;
    const float ped    = exp(-edge * edge * inv2s2);
    [[unroll]] for (int i = 0; i < 6; ++i)
    {
        const float dx = (base.x + float(i) + 0.5) - c.x;
        const float dy = (base.y + float(i) + 0.5) - c.y;
        wx[i] = i < K ? max(0.0, exp(-dx * dx * inv2s2) - ped) : 0.0;
        wy[i] = i < K ? max(0.0, exp(-dy * dy * inv2s2) - ped) : 0.0;
    }
}
// Vogel discs (r = sqrt((i+0.5)/n), golden angle), rotated per pixel.
const vec2 kPCSSBlockerDisc[8] = vec2[8](
    vec2(+0.250000, +0.000000), vec2(-0.319290, +0.292496),
    vec2(+0.048872, -0.556877), vec2(+0.402444, +0.524918),
    vec2(-0.738535, -0.130636), vec2(+0.699605, -0.445031),
    vec2(-0.234004, +0.870484), vec2(-0.446271, -0.859268));
const vec2 kPCSSFilterDisc[12] = vec2[12](
    vec2(+0.204124, +0.000000), vec2(-0.260699, +0.238822),
    vec2(+0.039904, -0.454688), vec2(+0.328595, +0.428593),
    vec2(-0.603011, -0.106664), vec2(+0.571225, -0.363367),
    vec2(-0.191064, +0.710747), vec2(-0.364379, -0.701590),
    vec2(+0.790557, +0.288710), vec2(-0.822442, +0.339492),
    vec2(+0.396472, -0.847237), vec2(+0.292982, +0.934074));
vec2 pcssRotate(vec2 v, vec2 cs) { return vec2(v.x * cs.x - v.y * cs.y, v.x * cs.y + v.y * cs.x); }

// ---- Projector shadows (spot lights stop at the first surface) ----
// One D32 atlas of per-projector depth tiles (comparison sampler, border = lit) + a
// record per shadowed projector; a light's tex.z holds its record index + 1.
layout(set = 0, binding = 18) uniform sampler2DShadow uProjShadowAtlas;
// Translucency layer of the atlas (#212): nearest translucent caster depth + transmittance.
layout(set = 0, binding = 29) uniform sampler2DShadow uProjTransDepth;
layout(set = 0, binding = 30) uniform sampler2D uProjTransColor;
#include "clean_projshadow.glsl"
#include "clean_shadow_mask_read.glsl"
#include "clean_cloud_shadow_map.glsl"   // #719: cloudShadowMap (draw set 0 binding 46)
// Pass 1 of the main view with the RT shadow mask (#195): opaque receivers read it.
bool rtMaskUsable()
{
#ifdef CLEAN_NO_TANGENT_IN
    return false;   // particles: never in the pre-pass depth
#else
    return ssr.rt_mask.x > 0.5 && !probeCapture() && !mirrorPass() && (v_flags & 2u) == 0u;
#endif
}

// GEOMETRIC (triangle-plane) normal of the receiving fragment, set by clean_draw.frag
// from cross(dFdx(P), dFdy(P)); zero = not set (deferred / compute includers: the
// shading normal is used). The projector normal offset and receiver-plane bias use it so
// a normal MAP cannot jitter the lookup per pixel (#441).
vec3 g_shadow_ng = vec3(0.0);

// Receiver-plane depth bias for the projector PCF (#441; the sun path's shadowRPDB).
// Returns d(ndc.z) per ATLAS TEXEL along +x / +y of the tile for the receiver's own
// plane (point Pr + normal Ng, camera-relative): the light-ray through each neighbouring
// texel is intersected with the plane analytically. A ray of the projector satisfies
// x = y = w = 0 at the apex, so its direction for tile-ndc (nx, ny) is
// cross(row0 - nx*row3, row1 - ny*row3) of the world->clip matrix.
float projPlaneZ(mat4 vp, vec3 apex, vec3 Pr, vec3 Ng, vec2 n)
{
    const vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    const vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    const vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    const vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    vec3 D = normalize(cross(r0 - n.x * r3, r1 - n.y * r3));
    if (dot(r3, D) < 0.0) D = -D;
    float den = dot(Ng, D);
    // Grazing: cap the plane slope (tan <= ~4) so the bias stays bounded.
    den = (den < 0.0 ? -1.0 : 1.0) * max(abs(den), 0.24);
    const float t = dot(Ng, Pr - apex) / den;
    const vec3  X = apex + t * D;
    const float w = max(t * dot(r3, D), 1e-6);
    return (dot(r2, X) + vp[3][2]) / w;
}
vec2 projReceiverDz(ProjShadow s, vec3 apex, vec3 Pr, vec3 Ng, vec2 ndc_xy)
{
    const float step_ndc = 2.0 * s.texel.x / max(s.rect.z, 1e-6);   // one atlas texel in tile ndc
    const float z0 = projPlaneZ(s.vp, apex, Pr, Ng, ndc_xy);
    return vec2(projPlaneZ(s.vp, apex, Pr, Ng, ndc_xy + vec2(step_ndc, 0.0)) - z0,
                projPlaneZ(s.vp, apex, Pr, Ng, ndc_xy + vec2(0.0, step_ndc)) - z0);
}

// Shadow-map path: normal + light offset by the receiver's texel footprint (grows with
// distance under the perspective), 5-tap weighted pattern on the hardware 2x2 compare
// filter (like the sun cascades), taps clamped inside the tile.
vec3 projShadowMap(Light L, uint idx, vec3 P, vec3 N)
{
    ProjShadow s = pshadow[idx];
    if (s.params.w < 0.5) return vec3(1.0);
    vec3  tl    = L.proj_origin.xyz - P;
    float d     = max(dot(-tl, L.proj_axis.xyz), 0.0);
    vec3  l     = normalize(tl);
    float texel = s.params.x * d;
    float k     = 1.0 - clamp(dot(N, l), 0.0, 1.0);
    // Receiver offsets in texel footprints (RenderVKProjShadowNormalOffset /
    // LightOffset): kept small -- a large push toward the light moves the lookup past
    // a THIN caster (a wall right in front of the receiver) and the light leaks.
    // Offset along the GEOMETRIC normal (facing the same side as the shading normal): a
    // normal map must not change the lookup point per pixel (#441).
    vec3  Ng    = N;
    {
        const float l2 = dot(g_shadow_ng, g_shadow_ng);
        if (l2 > 1e-30) { Ng = g_shadow_ng * inversesqrt(l2); if (dot(Ng, N) < 0.0) Ng = -Ng; }
    }
    const float kg = 1.0 - clamp(dot(Ng, l), 0.0, 1.0);
    vec3  Po    = P + Ng * (texel * s.texel.z * (0.5 + kg)) + l * (texel * s.texel.w);
    vec4  c     = s.vp * vec4(Po - ssr.cam_origin.xyz, 1.0);   // camera-relative (#555)
    if (c.w <= 0.0) return vec3(1.0);
    vec3  ndc   = c.xyz / c.w;
    if (ndc.z >= 1.0 || ndc.z <= 0.0) return vec3(1.0);
    vec2  uv    = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(1.0);
    vec2  t     = s.texel.xy;
    vec2  lo    = s.rect.xy + t * 0.5, hi = s.rect.xy + s.rect.zw - t * 0.5;
    vec2  a     = s.rect.xy + uv * s.rect.zw;
    const int q = shadowFilterQuality();
    // Receiver-plane depth gradient per atlas texel + a half-texel slope safety (#441).
    const vec3 Pr   = Po - ssr.cam_origin.xyz;
    const vec2 dzt  = q == 0 ? vec2(0.0) : projReceiverDz(s, L.proj_origin.xyz - ssr.cam_origin.xyz, Pr, Ng, ndc.xy);
    const float zref = ndc.z - 0.5 * max(abs(dzt.x), abs(dzt.y));
    float vis;
    if (q == 0) vis = texture(uProjShadowAtlas, vec3(clamp(a, lo, hi), ndc.z));
    else
    {
    // Gaussian gather PCF in atlas texels; each 2x2 block's gather point is held a
    // texel inside the tile so no tap reads a neighbouring projector's depth.
    const int K = q == 1 ? 4 : 6;
    const vec2 ct = a / t;
    const vec2 base = floor(ct + 0.5) - float(K / 2);
    float wx[6], wy[6];
    shadowGaussW(K, ct, base, wx, wy);
    const vec2 glo = s.rect.xy + t, ghi = s.rect.xy + s.rect.zw - t;
    float sh = 0.0, ws = 0.0;
    // Unrolled to the largest K (constant wx / wy indices: registers, not local memory);
    // the K guard is uniform, so the same taps run in the same order.
    [[unroll]] for (int by = 0; by < 6; by += 2)
        [[unroll]] for (int bx = 0; bx < 6; bx += 2)
            if (by < K && bx < K)
            {
                const vec2 P = clamp((base + vec2(float(bx), float(by)) + 1.0) * t, glo, ghi);
                // the block centre's depth on the receiver's plane (texels from the receiver)
                const vec2 blk = base + vec2(float(bx), float(by)) + 1.0;
                const vec4 g = textureGather(uProjShadowAtlas, P, zref + dot(blk - ct, dzt));
                sh += g.w * (wx[bx] * wy[by])     + g.z * (wx[bx + 1] * wy[by])
                    + g.x * (wx[bx] * wy[by + 1]) + g.y * (wx[bx + 1] * wy[by + 1]);
                ws += (wx[bx] + wx[bx + 1]) * (wy[by] + wy[by + 1]);
            }
    vis = sh / max(ws, 1e-6);
    }
    // Translucency atlas (#212, render_opts.w): half resolution, same tile uv. Behind
    // the nearest translucent caster the light takes the casters' transmittance.
    if (vis > 0.001 && specProjTransOn())
    {
        const vec2 t2 = t * 2.0;
        const vec2 tc = clamp(a, s.rect.xy + t2, s.rect.xy + s.rect.zw - t2);
        const float front = texture(uProjTransDepth, vec3(tc, ndc.z));
        if (front < 0.999) return vis * mix(texture(uProjTransColor, tc).rgb, vec3(1.0), front);
    }
    return vec3(vis);
}

#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
// RT ray budget per receiver: alpha-blend fragments (and particles) trace ONE ray per
// light. Stacked translucent layers (foliage cards ~7 deep) each traced the full Shadow
// quality count (up to 4); the soft edge on a see-through layer isn't worth 4x, and TAA
// smooths the single stratified ray over frames. Opaque receivers use the RT mask.
// #666: PER-PARTICLE lighting (clean_particle_light.comp: one invocation per particle,
// not per covered fragment) takes the full count -- a handful of rays per particle is
// nothing, and its sun term is accumulated per particle there (no screen history).
uint rtReceiverRays()
{
#if defined(CLEAN_NO_TANGENT_IN) && defined(CLEAN_COMPUTE_LIGHT)
    return 16u;
#elif defined(CLEAN_NO_TANGENT_IN)
    return 1u;
#elif defined(CLEAN_CAPTURE)
    return 1u;   // (#829: the capture FS's one hard sun ray, CLEAN_RT_SUN_HARD)
#else
    return (v_flags & 2u) != 0u ? 1u : 16u;
#endif
}
#include "clean_shadow_rt.glsl"
// GEOMETRIC normal for the ray origin (clean_rt_common.glsl offsetRayOrigin): the
// fragment's TRIANGLE plane, cross(dFdx(P), dFdy(P)) -- exact within a primitive
// (helper lanes extrapolate the same plane). clean_draw.frag stores it at the top of
// decodeSurface (uniform control flow, before any discard); the traces run later
// inside divergent lighting branches where derivatives are undefined. Zero = not set
// (other includers): the wrappers fall back to the interpolated vertex normal, which
// on low-poly smooth-shaded meshes can lean far enough from the facet for the origin
// to sit under the receiver's own triangle.
vec3 g_rt_face_ng = vec3(0.0);
vec3 rtFaceNormal(vec3 fallback)
{
    const float l2 = dot(g_rt_face_ng, g_rt_face_ng);
    return l2 > 1e-30 ? g_rt_face_ng * inversesqrt(l2) : fallback;
}
// NO-SELF-SHADOW CHARACTERS (clean_rt_common.glsl rtSelfCandidate): rigged receivers (and
// the deferred-lighting compute's G-buffer pixels, which carry no instance) may sit on a
// throttled character -- left UNKNOWN, resolved by the probe ray only if a shadow ray meets
// one; everything else (static faces, particles) is never one. Once per invocation: the sun
// and every projector share the resolution.
bool g_rt_self_set = false;
void rtSelfBegin(vec3 P)
{
    if (g_rt_self_set) return;
    g_rt_self_set = true;
#if defined(CLEAN_NO_TANGENT_IN)
    rt_self_owner = 0u;
#elif defined(CLEAN_COMPUTE_LIGHT)
    rt_self_owner = RT_SELF_UNKNOWN;
#else
    rt_self_owner = (v_flags & 1u) != 0u ? RT_SELF_UNKNOWN : 0u;   // instance flags bit 0: rigged
#endif
    rt_self_P = P;
    rt_self_cam = pc.u_cam_time.xyz;
}
// TRANSPARENT RECEIVERS (alpha-blend, v_flags bit 1) within this distance (m;
// RenderVKRTTransReceiverAlphaDist, frame info lit_dbg.w bits [16,24); 0 = never) keep
// the alpha-mask casters' alpha test in their opaque-only sun trace
// (clean_shadow_rt.glsl rt_opaque_cutouts).
float rtTransCutoutDist() { return float((rtBlendOpts() >> 16u) & 0xFFu); }
#ifndef CLEAN_NO_TANGENT_IN
// #664: this fragment's render pixel in the RT terminator triangle target, or -1 where
// the target's triangle is not this fragment's: alpha-blend receivers (not in the depth
// pre-pass -- the pixel holds the opaque surface behind them) and the mirror / probe
// views (other cameras, other sizes). Opaque / mask receivers are drawn EQUAL against
// the pre-pass depth, so the pixel holds exactly their triangle.
ivec2 rtTermTriPx()
{
    if ((v_flags & 2u) != 0u || probeCapture() || mirrorPass()) return ivec2(-1);
    return ivec2(CLEAN_FRAG_XY);
}
#endif
vec2 projNoisePx()
{
    vec2 px = CLEAN_FRAG_XY;
    if (ssr.taa_params.w > 0.5) px += 97.0 * (ssr.taa_jitter.xy + 0.5);
    return px;
}

vec3 projShadowRay(Light L, ProjShadow s, vec3 P, vec3 N)
{
    rtSelfBegin(P);
#ifdef CLEAN_NO_TANGENT_IN
    vec3 Ng = N;                               // particles: the billboard normal
#else
#ifdef CLEAN_COMPUTE_LIGHT
    vec3 Ng = N;   // (compute: no vertex normal -- #859's RT reflect pass compiles this, never calls it)
#else
    vec3 Ng = normalize(v_world_normal);
#endif
    if (any(isnan(Ng))) Ng = N;
    Ng = rtFaceNormal(Ng);                     // the triangle's plane (origin side: toward the light)
    // SHADOW TERMINATOR (#664; clean_shadow_rt.glsl rtTerminatorProbe, as sunShadowRayRGB):
    // in this projector's band (Ng on the visible side) the Hanika origin lift from the
    // triangle's vertex normals. One probe per fragment, shared with the sun and the
    // other projectors (rtTerminatorCached).
    const vec3 cam = pc.u_cam_time.xyz;
    const float ngl = dot(Ng, normalize(L.proj_origin.xyz - P)) * (dot(Ng, cam - P) < 0.0 ? -1.0 : 1.0);
    if (rtTerminatorOn() && rtTerminatorBand(ngl))
    {
        vec3 tng;
        rt_term_on = rtTerminatorCached(P, cam, Ng, RT_TERM_FACE_COS, rtTermTriPx(), rt_term_lift, rt_term_ns, tng);
    }
#endif
    const vec3 v = projShadowRayCore(L, s, P, Ng, pc.u_cam_time.xyz, pc.u_cam_time.w, projNoisePx(), rtReceiverRays());
    rt_term_on = false;
    return v;
}
#endif

#ifdef CLEAN_VIS_REUSE
// ---- SPLIT-LIGHTING VISIBILITY REUSE (RenderVKDeferVisReuse; the main view's forward
// alpha-blend FS, clean_draw.frag -DCLEAN_BLEND_TOL, render pass 2) ----
// The split deferred lighting's SHADOW pass (clean_deferred_light.comp) resolved the sun
// visibility and up to 4 shadowed projectors' visibility of the OPAQUE surface at every
// pixel, and recorded where (uVisDepth: that surface's view depth, signed by "the sun was
// evaluated", + its oct geometric normal; 0 = no lit surface). A blended fragment lying ON
// that surface -- decals, onion-skin layers, coplanar signs / grime -- takes those values
// instead of filtering the cascades / projector atlas (or tracing) again: the same lookups
// at the same point give the same result. Match: view depth within 4 mm + 0.1 % (below the
// lookups' own 1.5-texel normal offset), geometric normal within ~25 deg. Anything else --
// foliage, glass in front of walls, hair, smoke -- runs the full path, so the code (and its
// registers) stays; the saving is the filter ALU / fetches of the matched fragments. The
// draw push word (pad0, VISR_*) says what is valid this frame: 0 in every other draw.
// (Pass 1's depth -- binding 40 -- can't be the reference: the alpha-depth pre-pass wrote
// near-opaque blend texels into it after the lighting; their fragments would match it and
// take the visibility of the surface behind them.)
layout(set = 0, binding = 41, rgba8) uniform readonly image2D uVisReuseSun;
layout(set = 0, binding = 42, r32ui) uniform readonly uimage2DArray uVisReuseProj;
layout(set = 0, binding = 43, r32ui) uniform readonly uimage2D uVisReuseDepth;
const uint VISR_ON = 1u, VISR_PROJ = 2u, VISR_PROJ_RGB = 4u;
bool g_visr_sun = false;    // this fragment lies on a surface whose sun visibility was evaluated
bool g_visr_proj = false;   // ... whose shadowed projectors were resolved into slots
void visReuseMatch(vec3 P, vec3 Ng)
{
    g_visr_sun = false;
    g_visr_proj = false;
    const uint flags = g_draw_push.pad0;
    if ((flags & VISR_ON) == 0u) return;
    const uint d = imageLoad(uVisReuseDepth, ivec2(CLEAN_FRAG_XY)).x;
    const float vzS = unpackHalf2x16(d & 0xFFFFu).x;
    if (vzS == 0.0) return;   // sky / cleared / fullbright there
    const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    if (abs(vz - abs(vzS)) > 0.004 + 0.001 * vz) return;
    const vec2 e = unpackUnorm4x8(d >> 16u).xy * 2.0 - 1.0;   // oct Ng, 8 + 8 bits
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    if (dot(normalize(n), Ng) < 0.9) return;
    g_visr_sun = vzS > 0.0;
    g_visr_proj = (flags & VISR_PROJ) != 0u;
}
// The sun visibility of the matched surface (sqrt-encoded, as the combine reads it).
bool visReuseSun(out vec3 tint, out float vis)
{
    tint = vec3(1.0);
    vis = 1.0;
    if (!g_visr_sun) return false;
    const vec4 v = imageLoad(uVisReuseSun, ivec2(CLEAN_FRAG_XY));
    tint = v.rgb * v.rgb;
    vis = v.a * v.a;
    return true;
}
// A projector the matched surface resolved (false: not in its slots -- gated off there by
// the opaque surface's own tests, or a 5th+ overlapping one: the full path decides).
bool visReuseProj(uint light, out vec3 vis)
{
    vis = vec3(1.0);
    if (!g_visr_proj) return false;
    const ivec2 px = ivec2(CLEAN_FRAG_XY);
    const uint ids = imageLoad(uVisReuseProj, ivec3(px, 0)).x;
    // its own slot, else the REST slot (clean_lights_common.glsl THE REST SLOT: the deferred
    // shadow pass folded the 5th+ shadowed projectors there); neither: resolve it here
    uint want = light + 1u;
    if (want >= PROJ_DEFER_REST_ID) want = PROJ_DEFER_REST_ID;
    bool own = false;
    [[unroll]] for (uint k = 0u; k < 4u; ++k) own = own || ((ids >> (k * 8u)) & 0xFFu) == want;
    if (!own) want = PROJ_DEFER_REST_ID;
    [[unroll]] for (uint k = 0u; k < 4u; ++k)
        if (((ids >> (k * 8u)) & 0xFFu) == want)
        {
            const uint sh = k * 8u;
            vec3 e = vec3(float((imageLoad(uVisReuseProj, ivec3(px, 1)).x >> sh) & 0xFFu) / 255.0);
            if ((g_draw_push.pad0 & VISR_PROJ_RGB) != 0u)
            {
                e.g = float((imageLoad(uVisReuseProj, ivec3(px, 2)).x >> sh) & 0xFFu) / 255.0;
                e.b = float((imageLoad(uVisReuseProj, ivec3(px, 3)).x >> sh) & 0xFFu) / 255.0;
            }
            vis = e * e;
            return true;
        }
    return false;
}
#endif

// (#85 phase 2) A projector whose shadowed direct light the PROJECTOR STOCHASTIC sampler
// (clean_deferred_light.comp pass 15) covers: a ray-traced shadow record. The sampler walks
// the cluster's shadowed run, which holds every such record reaching the cluster
// (kMaxProjShadows 32 <= CLUSTER_CAP: never cut).
bool projStochEligible(Light L)
{
    if (L.tex.z == 0u) return false;
    const ProjShadow ps = pshadow[L.tex.z - 1u];
    return !(ps.params.w < 0.5) && ps.params.z > 1.5;
}

// Unified projector visibility (1 = lit; RGB: ray-traced shadows pass coloured light
// through translucent casters, #212). The technique is chosen per record on the
// CPU (RenderVKProjectorShadows): params.z = 2 -> ray traced (only ever set when the
// device has ray queries AND this frame's TLAS is valid), else the shadow-map atlas.
vec3 projectorVisibility(Light L, uint light, vec3 P, vec3 N, inout RtMaskTaps taps, inout bool tapsOk)
{
    // (Spec constant 10, clean_frame_info.glsl: projector shadows off folds the whole path.)
    if (!specProjOn() || L.tex.z == 0u || litDbg(LDBG_NO_PROJ_SHADOW)) return vec3(1.0);
#ifdef CLEAN_VIS_REUSE
    {   // (the split lighting resolved it on the surface this fragment lies on)
        vec3 rv;
        if (visReuseProj(light, rv)) return rv;
    }
#endif
#ifdef CLEAN_PROJ_VIS_EXTERNAL
    // Split deferred lighting (clean_deferred_light.comp combine): the shadow pass resolved
    // this pixel's shadowed projectors (atlas or RT mask, this function's #else) into 4
    // slots keyed by light index, the RT mask's scheme; a projector in none of them (5th+
    // overlapping, or not in its cluster's shadowed run) is unshadowed, as with the mask.
    return CLEAN_PROJ_VIS_EXTERNAL(light);
#else
    const uint idx = L.tex.z - 1u;
    if (pshadow[idx].params.w < 0.5) return vec3(1.0);
    if (specProjTraced() && pshadow[idx].params.z > 1.5)   // (shadow maps only: folded)
    {
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
        // Mirror / probe captures walk their own light lists (no clusters): one ray
        // per light per pixel there cost ~20 ms; reflections take the unshadowed term.
        if (mirrorPass() || probeCapture()) return vec3(1.0);
#if !defined(CLEAN_NO_TANGENT_IN) && !defined(CLEAN_COMPUTE_LIGHT)
        // ALPHA-BLEND RECEIVERS (RenderVKRTBlendProjShadows 0, the default): no ray query per
        // projector per fragment -- the RT shadow mask / stochastic sampler only cover the
        // depth pre-pass surfaces, so a blended surface traced one shadow ray (+ a terminator
        // probe) per ray-traced projector at EVERY shaded fragment: a screen-filling lit pool
        // under scripted projectors paid ~3 ms for it. Lit unshadowed, as water and the
        // captures already are. 1 = the inline trace (A/B, small glass under stage lights).
        if ((v_flags & 2u) != 0u && (linfo.fwd.z & 1u) == 0u) return vec3(1.0);
#endif
        return projShadowRay(L, pshadow[idx], P, N);
#else
        // Plain variant: pass 1 with the RT shadow mask reads it; otherwise no ray
        // queries here and the atlas holds nothing for a traced record. The depth-aware
        // upsample taps (4 depth fetches + plane math) depend only on the fragment, so
        // they are built once for the whole light loop, not once per shadowed projector.
        if (!rtMaskUsable()) return vec3(1.0);
        if (!tapsOk) { taps = rtMaskTaps(P); tapsOk = true; }
        return rtMaskProjTaps(light, taps);
#endif
    }
    return projShadowMap(L, idx, P, N);
#endif
}

// LIT-PASS TRIM (RenderVKLightCutoff -> ssr.lit_dbg.z, linear radiance; 0 = off): a light
// whose attenuated colour at this point is below the cutoff is faded out over [c, 2c] and
// skipped below c -- before its shadow / gobo fetches. Smooth, so no visible edge where
// the tail of a dim / distant light's falloff used to end.
float lightCutoffFade(float atten, vec3 color)
{
    const float c = ssr.lit_dbg.z;
    if (c <= 0.0) return 1.0;
    return smoothstep(c, 2.0 * c, atten * max(max(color.r, color.g), color.b));
}

// CEL SHADING (#550): lightSurface() sets this for a toon-lit surface; the local lights
// then band their N.L x attenuation term (celRamp) like the sun. False everywhere else.
bool g_cel = false;
float celLocal(float nlAtten) { return g_cel ? celRamp(nlAtten) : nlAtten; }

// LIGHT STAGING (#815, RenderVKLightStage; the deferred lighting's uber / combine kernels
// only -- clean_deferred_light.comp defines CLEAN_LIGHT_STAGE + lightStaged() and the shared
// arrays): with a workgroup-uniform light mask (RenderVKLightCulling 0 / 1) the workgroup
// copies, once per tile, the 32 B point part (pos_radius, color_falloff) and the projector
// bit of every light its mask holds into shared memory, one light per lane; every pixel then
// reads those from shared memory instead of two dependent global loads per candidate light
// (position -> range test -> colour), the walk's serial latency chain. Bit copies: the same
// lights, in the same order, through the same math. Everything else -- the forward lit FS,
// water, particles, the froxel walk -- loads from the light buffer (lightStaged() false).
#ifdef CLEAN_LIGHT_STAGE
vec4 lightPosRadius(uint li, bool st)   { if (st) return s_lightPos[li]; return lights[li].pos_radius; }
vec4 lightColorFalloff(uint li, bool st) { if (st) return s_lightCol[li]; return lights[li].color_falloff; }
bool lightIsProjector(uint li, bool st)
{
    if (st) return (s_lightProj[li >> 5u] & (1u << (li & 31u))) != 0u;
    return !(lights[li].proj_origin.w < 0.5);
}
#else
vec4 lightPosRadius(uint li, bool st)    { return lights[li].pos_radius; }
vec4 lightColorFalloff(uint li, bool st) { return lights[li].color_falloff; }
bool lightIsProjector(uint li, bool st)  { return !(lights[li].proj_origin.w < 0.5); }
#endif

// One local light's contribution at P (linear). base = linear albedo. ls = the legacy
// face's specular inputs (legacySpecNone() for PBR / matte / particles); glare (LL
// materialF's forward-alpha glare) accumulates this light's legacy specular for the
// alpha-blend coverage boost. (taps / tapsOk: the fragment's RT-mask upsample taps,
// built on the first shadowed projector.) st: read the point part from the tile's staged
// copy (lightPosRadius above; uniform).
// REGISTER PEAK (#815): the 160-byte record is never copied for a POINT light -- position /
// radius, then colour / falloff and the projector bit, are read where they are used, and the
// full record (proj_mat, axis, origin, params, gobo) is loaded only inside the projector
// branch. (A `Light L = lights[li]` before the branch let the compiler hoist all 40 words of
// it for every candidate light.) Same values, same order of operations: lossless.
// The back-face self-shadow rule (RenderVKRTBackfaceSelfShadow) for the SHADOWED AMBIANCE's
// turned-away receivers (localLight): rtBackfaceSelfShadow where it is compiled (ray-query
// variants), else the same frame-info bit; particles (no surface of their own) never.
#if defined(CLEAN_PROJ_SHADOW_RAYQUERY)
bool projAmbBackSelf() { return rtBackfaceSelfShadow(); }
#elif defined(CLEAN_NO_TANGENT_IN)
bool projAmbBackSelf() { return false; }
#else
bool projAmbBackSelf() { return (uint(ssr.foliage_rt.x + 0.5) & 16u) != 0u; }
#endif
vec3 localLight(uint li, vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, vec3 diffuseColor, vec3 specularColor,
                float rough, LegacySpec ls, inout float glare, inout RtMaskTaps taps, inout bool tapsOk, bool st)
{
    // Range test on the position alone (squared, no sqrt) before the rest of the record
    // is loaded: most cluster candidates miss the fragment. Bit-identical outside the
    // last ulp of the radius, where the attenuation is 0 anyway.
    const vec4  pr = lightPosRadius(li, st);
    vec3  lv   = pr.xyz - P;
    const float d2 = dot(lv, lv);
    if (d2 >= pr.w * pr.w) return vec3(0.0);
#ifdef CLEAN_LIGHT_TAKEN
    // (#91 LIGHT TAKEOVER, the deferred lit kernels only: clean_deferred_light.comp deferLightTaken) a
    // point light whose prim's emitting triangles carry it this frame -- their light replaces it here
    // (after the range test: only the lights that reach P pay the header read)
    if (CLEAN_LIGHT_TAKEN(li)) return vec3(0.0);
#endif
    const vec4 cf = lightColorFalloff(li, st);   // linear rgb, falloff
    float dist = sqrt(d2);
    if (dist >= pr.w) return vec3(0.0);
    float atten = legacyAtten(dist / pr.w, cf.w);
    if (atten <= 0.0) return vec3(0.0);
    const float fade = lightCutoffFade(atten, cf.rgb);
    if (fade <= 0.0) return vec3(0.0);
    atten *= fade;
    // LL's forward alpha path (materialF calcPointLightOrSpotLight) clamps each light's
    // legacy specular to [0, 1]; the deferred point light (multiPointLightF) does not.
    const bool blend = (v_flags & 2u) != 0u;

    if (!lightIsProjector(li, st))
    {   // POINT (multiPointLightF)
        vec3 l = lv / max(dist, 1e-4);
#ifdef CLEAN_PROJ_STOCH_LIT
        // (#869 / #439 ReSTIR DI, RenderVKReSTIRLights 1) a point light in pass 15's light set
        // (CLEAN_RS_STOCH: the ReSTIR frame, in its grid, clean_regir.glsl rsEligible): shadowed by
        // the denoised visibility ratio, diffuse / specular shares as the projectors' (projDirectTerms)
        // (#869 SPLIT: the hook takes the light -- a split light gets its own visibility, the others
        // the rest's ratio; clean_pdn_common.glsl SPLIT CHANNEL)
        const bool pst = CLEAN_PROJ_STOCH_ON() && CLEAN_RS_STOCH(li, P);
        vec3 pvd = vec3(1.0), pvs = vec3(1.0);
        if (pst) CLEAN_PROJ_STOCH_VIS(li, P, N, pvd, pvs);
#else
        const bool pst = false;
        const vec3 pvd = vec3(1.0), pvs = vec3(1.0);
#endif
        if (pbr)
        {
            float nl; vec3 df, sp;
            llPunctual(diffuseColor, specularColor, rough, N, V, l, nl, df, sp);
            if (g_cel)   // banded light amount, specular as a step (#550)
                return cf.rgb * 3.25 * clamp(celRamp(nl * atten) * df + celSpecStep(nl * atten * sp, specularColor),
                                             vec3(0.0), vec3(10.0)) * pvd;
            if (pst)
            {
                const vec3 d = nl * df, sv = nl * sp, t = d + sv;
                const vec3 k = clamp(t, vec3(0.0), vec3(10.0)) / max(t, vec3(1e-12));
                return atten * cf.rgb * 3.25 * k * (d * pvd + sv * pvs);
            }
            return atten * cf.rgb * 3.25 * clamp(nl * (df + sp), vec3(0.0), vec3(10.0));
        }
        float nl = dot(N, l);
        if (nl <= 0.0) return vec3(0.0);
        vec3 col = cf.rgb * celLocal(nl * atten) * base * pvd;
        // Legacy Blinn-Phong specular (#470): lit = min(nl * 6, 1) * dist_atten,
        // col += lit * scol * light colour * specular colour.
        if (ls.gloss > 0.0)
        {
            float slit;
            const float scol = legacySpecular(N, V, l, ls.gloss, slit);
            vec3 speccol = (slit * atten * scol) * cf.rgb * ls.spec;
            if (blend) speccol = clamp(speccol, vec3(0.0), vec3(1.0));
            speccol *= pvs;
            glare = max(glare, speccol.r) + max(max(speccol.r, speccol.g), speccol.b);   // LL materialF, verbatim
            col += speccol;
        }
        return col;
    }

    // PROJECTOR (spotLightF): lit only inside the projected frustum. The full record now
    // (the helpers below take it whole; only its used members survive inlining).
    const Light L = lights[li];
    vec4 ptc = L.proj_mat * vec4(P, 1.0);
    if (ptc.z < 0.0) return vec3(0.0);
    ptc.xyz /= ptc.w;
    if (ptc.z <= 0.0 || ptc.x <= 0.0 || ptc.x >= 1.0 || ptc.y <= 0.0 || ptc.y >= 1.0) return vec3(0.0);
    float l_dist = -dot(lv, L.proj_axis.xyz);
    vec3  l  = normalize(L.proj_origin.xyz - P);
    float nl_raw = dot(N, l);
    float nl = clamp(nl_raw, 0.000001, 1.0);   // calcHalfVectors
#ifdef CLEAN_PROJ_STOCH_LIT
    // (#85 phase 2, DEFER_FLAG_PSTOCH; the deferred lighting kernels only) RATIO ESTIMATOR
    // (Heitz et al. 2018): a ray-traced projector's light is evaluated here exactly and fresh
    // every frame (colour, gobo, position, BRDF -- nothing of it is accumulated), and only its
    // VISIBILITY comes from the denoiser: the pixel's denoised shadowed / unshadowed ratio of
    // the stochastic sampler (pass 15), one for the diffuse and one for the specular share.
    // (#869 SPLIT, RenderVKProjStochasticSplitK) the K strongest lights at the pixel have their OWN
    // visibility; the ratio above is THE REST's, over the other lights only -- so a projector behind
    // a wall no longer takes the room lamps' ratio (its gobo through the wall), nor darkens them.
    // (#869 ReSTIR DI) + a projector without a ray-traced record in pass 15's extended light set
    const bool stochExt = CLEAN_PROJ_STOCH_ON() && !projStochEligible(L) && CLEAN_RS_STOCH(li, P);
    const bool stoch = (CLEAN_PROJ_STOCH_ON() && projStochEligible(L)) || stochExt;
#else
    const bool stoch = false;
    const bool stochExt = false;
#endif
    // One gobo fetch: the ambiance term reuses the diffuse texel where both sample the
    // same LOD, and is skipped outright without ambiance (it is exactly 0 then: PBR
    // passes lit 0; legacy's min(0, 1 - lit) only goes negative facing the light, where
    // max(dot(-l, N), 0) zeroes it).
    vec4 gtex; float glod;
    vec3  dlit = projDiffuseT(L, l_dist, ptc.xy, gtex, glod);
    const bool legacySpec = !pbr && ls.gloss > 0.0;
    const bool legacyEnv  = !pbr && ls.env > 0.0;
    // Visibility only where it can matter: the direct term is ~0 when the
    // surface faces away from the light or the projected texel (gobo) is
    // black, so a shadow ray there is pure cost. Mirror captures walk every
    // light (not the main view's clusters) -- they take the unshadowed term
    // rather than tracing each one (see projectorVisibility). A legacy glint peaks
    // ~15x the diffuse term (normalized Blinn-Phong), and the image reflection reads
    // another texel, so those widen the test.
    vec3 vis = vec3(1.0);
    vec3 visS = vec3(1.0);   // (#85 phase 2) the specular share's visibility (stochastic: its own ratio)
    // (shader clock: the projector's visibility is its own stage; marks outside the branch, so
    // the lanes that skip it charge their wait to it too)
    CLK_MARK(LOCAL);
    // SHADOWED AMBIANCE (RenderVKProjAmbianceShadowedMode, frame info foliage_rt.x bit 5; default
    // on): the ambiance term takes the projector's DIFFUSE visibility like the direct light --
    // before, a shadowed projector's soft max-LOD gobo copy reached the far side of a wall.
    // Shadowed projectors only (a shadow record, projector shadows on); unshadowed ones keep it.
    const bool amb = L.proj_params.w > 0.0;
    const bool ambSh = amb && projAmbShadowed()
                    && ((specProjOn() && L.tex.z != 0u && !(pshadow[L.tex.z - 1u].params.w < 0.5)) || stochExt);
    // BACK FACES (the legacy ambiance only ever lights them: max(dot(-l, N), 0)): any occlusion
    // test from a surface turned away from the apex crosses that surface itself -- a ray from
    // the visible side meets its own facet; the only way it does not is the light-side origin
    // flip that put a thin wall's origin behind the wall (the leak this fixes). So "blocked by
    // other geometry" and "blocked by its own surface" coincide: under the back-face self-shadow
    // rule (RenderVKRTBackfaceSelfShadow) the value is 0, with no lookup and no ray. Without
    // that rule (0, or particles: no surface of their own) the visibility query runs as for a
    // front face (the backend's own origin rule).
    const bool ambBack = ambSh && !(nl_raw > 0.0);
    if (ambBack && projAmbBackSelf()) { vis = vec3(0.0); visS = vec3(0.0); } else
#ifdef CLEAN_PROJ_STOCH_LIT
    if (stoch) CLEAN_PROJ_STOCH_VIS(li, P, N, vis, visS); else   // (#869: per light -- its own if split here, 0 if culled)
#endif
    // (ambSh: the ambiance's max-LOD texel is non-black past the direct texel's gobo edge --
    // the soft copy -- so it widens the test)
    if (ambBack || (nl_raw > 0.0 && (ambSh || legacyEnv || max(max(dlit.r, dlit.g), dlit.b) * atten * (legacySpec ? 16.0 : 1.0) > 0.002)))
        vis = projectorVisibility(L, li, P, N, taps, tapsOk);
    if (!stoch) visS = vis;
    CLK_MARK(PROJ);
    const vec3 dlit0 = dlit;   // (unshadowed: the stochastic path's specular share)
    dlit *= vis;
    const vec3 ambVis = ambSh ? vis : vec3(1.0);   // (stochastic: the diffuse ratio)
    float amb_da = (nl * 0.5 + 0.5) * L.proj_params.w;
    vec3 sum = vec3(0.0);
    if (pbr)
    {
        float nlp; vec3 df, sp;
        llPunctual(diffuseColor, specularColor, rough, N, V, l, nlp, df, sp);
        vec3 brdf = clamp(nlp * (df + sp), vec3(0.0), vec3(10.0));
        if (g_cel) sum += dlit * 3.25 * clamp(celRamp(nlp * atten) * df, vec3(0.0), vec3(10.0));   // banded (#550)
        else if (stoch)
        {
            // (#85) the clamped BRDF split into its diffuse / specular shares (projDirectTerms),
            // each with its own visibility ratio
            const vec3 d = nlp * df, sv = nlp * sp;
            const vec3 k = brdf / max(d + sv, vec3(1e-12));
            sum += atten * dlit0 * 3.25 * k * (d * vis + sv * visS);
        }
        else       sum += atten * dlit * 3.25 * brdf;
        if (amb) sum += projAmbianceT(L, amb_da, atten, 0.0, nl, ptc.xy, gtex, glod) * 3.25 * brdf * ambVis;
    }
    else
    {
        float lit = nl * atten;
        sum += dlit * celLocal(lit) * base;
        if (amb) sum += base * projAmbianceT(L, amb_da, atten, lit, nl, ptc.xy, gtex, glod) * max(dot(-l, N), 0.0) * ambVis;
        // Legacy Blinn-Phong specular (#470, spotLightF): dlit (projected colour x
        // visibility) * min(nl * 6, 1) * dist_atten * scol * specular colour, clamped.
        if (legacySpec)
        {
            float slit;
            const float scol = legacySpecular(N, V, l, ls.gloss, slit);
            const vec3 speccol = clamp((stoch ? dlit0 * visS : dlit) * (slit * atten * scol) * ls.spec, vec3(0.0), vec3(1.0));
            glare = max(glare, speccol.r) + max(max(speccol.r, speccol.g), speccol.b);
            sum += speccol;
        }
        // Legacy shiny: the projector's image mirrored in the surface (spotLightF
        // envIntensity branch). LL evaluates it anywhere in front of the projector within
        // its radius; here only inside the projected frustum, the region the light is
        // binned into (clusters / deferred tiles), so it can't show tile edges. Surfaces
        // facing away (no visibility evaluated; LL's shadow map darkens them) skip it.
        if (legacyEnv && nl_raw > 0.0)
            sum += projEnvReflect(L, P, N, V, ls.gloss) * visS * ls.env;
    }
    return sum;
}

#ifdef CLEAN_PROJ_STOCH_TERMS
// (#85 phase 2) PROJECTOR STOCHASTIC SAMPLER's terms (clean_deferred_light.comp pass 15): the
// VISIBILITY-DEPENDENT part of localLight's projector branch for one projector at P, split
// into its diffuse and specular share and UNSHADOWED (vis = 1) -- exactly what localLight
// multiplies by vis: PBR atten x dlit x 3.25 x clamp(nl (df + sp), 0, 10) (the clamp's scale
// shared by both shares, so they sum to localLight's term), legacy dlit x nl x atten x base
// and the clamped Blinn-Phong glint. Not here: the legacy image reflection (localLight keeps
// it, unshadowed, under DEFER_FLAG_PSTOCH); the PBR ambiance joins the diffuse share only with
// RenderVKProjAmbianceShadowedMode (localLight then gives it the diffuse ratio; 0: unshadowed, left
// out); and cel banding (cel surfaces stay on localLight). Same gates as localLight up to its visibility call, plus N.L > 0 (it
// only evaluates visibility there; the PBR term facing away is ~0 x its 0.001 N.L floor).
// MUST MATCH localLight's projector branch term for term.
// cheap: the gobo texel taken as 1 (no texture fetch: the light sampler's target function --
// it stays > 0 wherever the real term can be, so the estimator stays unbiased).
// pdist: the distance to the projector's apex (the specular hit distance). False: no light.
bool projDirectTerms(uint li, vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, vec3 diffuseColor, vec3 specularColor,
                     float rough, LegacySpec ls, bool cheap, out vec3 dterm, out vec3 sterm, out float pdist)
{
    dterm = vec3(0.0); sterm = vec3(0.0); pdist = 0.0;
    const vec4 pr = lights[li].pos_radius;
    const vec3 lv = pr.xyz - P;
    const float d2 = dot(lv, lv);
    if (d2 >= pr.w * pr.w) return false;
    if (lights[li].proj_origin.w < 0.5)
    {
        // (#869 / #439 ReSTIR DI) A POINT LIGHT: localLight's point branch term for term (the
        // shadowed shares it multiplies by the ratios), N.L > 0 only as for the projectors.
        const vec4 cf = lights[li].color_falloff;
        const float dist = sqrt(d2);
        if (dist >= pr.w) return false;
        float atten = legacyAtten(dist / pr.w, cf.w);
        if (atten <= 0.0) return false;
        const float fade = lightCutoffFade(atten, cf.rgb);
        if (fade <= 0.0) return false;
        atten *= fade;
        const vec3 l = lv / max(dist, 1e-4);
        const float nl_raw = dot(N, l);
        if (nl_raw <= 0.0) return false;
        pdist = dist;
        if (pbr)
        {
            float nl; vec3 df, sp;
            llPunctual(diffuseColor, specularColor, rough, N, V, l, nl, df, sp);
            const vec3 d = nl * df, sv = nl * sp, t = d + sv;
            const vec3 k = clamp(t, vec3(0.0), vec3(10.0)) / max(t, vec3(1e-12));
            dterm = atten * cf.rgb * 3.25 * d * k;
            sterm = atten * cf.rgb * 3.25 * sv * k;
        }
        else
        {
            dterm = cf.rgb * (nl_raw * atten) * base;
            if (ls.gloss > 0.0)
            {
                float slit;
                const float scol = legacySpecular(N, V, l, ls.gloss, slit);
                sterm = (slit * atten * scol) * cf.rgb * ls.spec;
            }
        }
        return true;
    }
    const Light L = lights[li];
    const float dist = sqrt(d2);
    if (dist >= L.pos_radius.w) return false;
    float atten = legacyAtten(dist / L.pos_radius.w, L.color_falloff.w);
    if (atten <= 0.0) return false;
    const float fade = lightCutoffFade(atten, L.color_falloff.rgb);
    if (fade <= 0.0) return false;
    atten *= fade;
    vec4 ptc = L.proj_mat * vec4(P, 1.0);
    if (ptc.z < 0.0) return false;
    ptc.xyz /= ptc.w;
    if (ptc.z <= 0.0 || ptc.x <= 0.0 || ptc.x >= 1.0 || ptc.y <= 0.0 || ptc.y >= 1.0) return false;
    const float l_dist = -dot(lv, L.proj_axis.xyz);
    const vec3 l = normalize(L.proj_origin.xyz - P);
    const float nl_raw = dot(N, l);
    if (nl_raw <= 0.0) return false;
    pdist = length(L.proj_origin.xyz - P);
    vec3 dlit;
    vec4 gtex = vec4(1.0); float glod = 0.0;
    if (cheap)
    {
        // projDiffuseT with the texel = 1: the light colour x the frustum edge fade.
        const float proj_lod = L.proj_params.z;
        const float lod = clamp((l_dist - L.proj_params.x) / L.proj_params.y, 0.0, 1.0) * proj_lod;
        const vec2 ed = vec2(0.5) - abs(ptc.xy - vec2(0.5));
        const float det = min(lod / max(proj_lod * 0.5, 1e-4), 1.0);
        const float edge = 0.25 * det;
        dlit = L.color_falloff.rgb * (edge > 0.0 ? clamp(min(ed.x, ed.y) / edge, 0.0, 1.0) : 1.0);
    }
    else
        dlit = projDiffuseT(L, l_dist, ptc.xy, gtex, glod);
    if (pbr)
    {
        float nlp; vec3 df, sp;
        llPunctual(diffuseColor, specularColor, rough, N, V, l, nlp, df, sp);
        const vec3 d = nlp * df, sv = nlp * sp;
        const vec3 t = d + sv;
        const vec3 k = clamp(t, vec3(0.0), vec3(10.0)) / max(t, vec3(1e-12));   // localLight's clamp, shared
        const vec3 c = atten * dlit * 3.25;
        dterm = c * d * k;
        sterm = c * sv * k;
        // SHADOWED AMBIANCE (projAmbShadowed): localLight gives the ambiance this sampler's
        // DIFFUSE ratio, so it is part of the diffuse share's weight here -- an ambiance-only
        // pixel (black direct texel) then has a non-zero den and its own ray. (Legacy: its
        // ambiance lights only back faces, never evaluated here. cheap: the texel taken as 1.)
        if (L.proj_params.w > 0.0 && projAmbShadowed())
        {
            const float nl = clamp(nl_raw, 0.000001, 1.0);
            const float amb_da = (nl * 0.5 + 0.5) * L.proj_params.w;
            const float alod = cheap ? L.proj_params.z : glod;
            dterm += projAmbianceT(L, amb_da, atten, 0.0, nl, ptc.xy, gtex, alod) * 3.25 * clamp(t, vec3(0.0), vec3(10.0));
        }
    }
    else
    {
        const float nl = clamp(nl_raw, 0.000001, 1.0);
        dterm = dlit * (nl * atten) * base;
        if (ls.gloss > 0.0)
        {
            float slit;
            const float scol = legacySpecular(N, V, l, ls.gloss, slit);
            sterm = clamp(dlit * (slit * atten * scol) * ls.spec, vec3(0.0), vec3(1.0));
        }
    }
    return true;
}
#endif

// Sum of this fragment's local lights (linear). base = linear albedo; ls = the legacy
// specular inputs (legacySpecNone() for none). glare: LL materialF's alpha-blend glare
// from the local lights' legacy specular (0 when there is none).
// CLEAN_TILE_LIGHTS (the deferred lighting compute): a 256-bit light mask (32 lights per word,
// walked in light-index order) instead of the cluster list -- the workgroup's shared tile mask
// or this pixel's froxel cell, OR-ed over the subgroup (clean_deferred_light.comp
// tileLightBase / tileLightWord). Either is a superset of the lights that reach P; every other
// light returns an exact 0 from localLight's range / frustum tests before any side effect, so
// the sum (ascending index order) is the same whichever superset is walked.
vec3 localLights(vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, float metal, float rough,
                 LegacySpec ls, out float glare)
{
    glare = 0.0;
#ifdef CLEAN_LPROBE_NO_LOCAL
    return vec3(0.0);   // (lighting register probe, measurement only: RenderVKLightRegProbe)
#endif
    uint count = uint(linfo.cam_pos.w);
    if (count == 0u || litDbg(LDBG_NO_LOCAL)) return vec3(0.0);
    vec3 diffuseColor  = base * (1.0 - 0.04) * (1.0 - metal);
    vec3 specularColor = mix(vec3(0.04), base, metal);
    vec3 sum = vec3(0.0);
    RtMaskTaps taps;              // RT-mask upsample taps, built on the first shadowed projector
    bool tapsOk = false;
#ifdef CLEAN_TILE_LIGHTS
    const uint tlBase = tileLightBase(P);
#ifdef CLEAN_LIGHT_STAGE
    const bool st = lightStaged();   // (push constant: uniform)
#else
    const bool st = false;
#endif
    CLK_MARK(LOCAL);
    uint bits = tileLightWord(tlBase, 0u);
    CLK_MARK(FROXEL);   // (the cell's first word: its load latency is the stage)
    for (uint w = 0u; w < TILE_LIGHT_WORDS; ++w)
    {
        // The next word's load is issued before this word's lights are walked.
        const uint nextBits = w + 1u < TILE_LIGHT_WORDS ? tileLightWord(tlBase, w + 1u) : 0u;
        while (bits != 0u)
        {
            const uint b = uint(findLSB(bits));
            bits &= bits - 1u;
            sum += lgFinite(localLight(w * 32u + b, P, N, V, pbr, base, diffuseColor, specularColor, rough, ls, glare,
                                       taps, tapsOk, st), LG_LOCAL);
        }
        bits = nextBits;
    }
#else
#ifdef CLEAN_FWD_FROXEL
    // FORWARD FROXEL WALK (RenderVKForwardFroxels, with RenderVKLightCulling 2): the main view's
    // forward surfaces (alpha-blend, forward opaque) walk the SAME per-pixel light sets as the
    // deferred lighting -- the froxel masks (clean_light_froxel.comp: 2^fx_dims.w px tiles x 48
    // log slices, lossless 256-bit masks, built from the lights alone -- no depth read -- so
    // they hold for any P in the view, transparent surfaces included). The cluster grid
    // (16 x 9 x 24: ~240 px tiles at 4K) listed every light touching a far larger volume (up to
    // CLUSTER_CAP, the rest dropped). Cell = (this pixel's tile, the slice of P's view depth):
    // froxelCellBase's definition; P lies on the jittered view ray through the fragment's
    // centre, the case the build's slack is proven for. The words are OR-ed over the subgroup
    // (as tileLightWord): one light set per subgroup, uniform light indices -- a superset of
    // each lane's own; localLight adds an exact 0 for a light that does not reach P, so the sum
    // (ascending index order) equals the per-lane walk's. Mirror / probe captures: below.
    if ((linfo.fwd.x | linfo.fwd.y) != 0u && !mirrorPass() && !probeCapture())
    {
        const uvec4 fdims = linfo.fx_dims;
        const uvec2 ft = min(uvec2(CLEAN_FRAG_XY) >> fdims.w, fdims.xy - 1u);
        const uint fs = froxelSlice(dot(P - linfo.fx_eye.xyz, linfo.cam_fwd.xyz), linfo.fx_slices, fdims.z);
        const uint fbase = ((fs * fdims.y + ft.y) * fdims.x + ft.x) * FROXEL_WORDS;
        FwdFroxelRef fr = FwdFroxelRef(linfo.fwd.xy);
        uint fbits = subgroupOr(fr.w[fbase]);
        for (uint w = 0u; w < FROXEL_WORDS; ++w)
        {
            // The next word's load is issued before this word's lights are walked.
            const uint nextBits = w + 1u < FROXEL_WORDS ? subgroupOr(fr.w[fbase + w + 1u]) : 0u;
            while (fbits != 0u)
            {
                const uint li = w * 32u + uint(findLSB(fbits));
                fbits &= fbits - 1u;
                if (li >= count) break;   // (never: the build marks only this frame's lights)
                sum += lgFinite(localLight(li, P, N, V, pbr, base, diffuseColor, specularColor, rough, ls, glare,
                                           taps, tapsOk, false), LG_LOCAL);
            }
            fbits = nextBits;
        }
        return sum;
    }
#endif
    // Mirror capture: the clusters are the MAIN view's screen tiles; walk every light.
    // Probe capture: the probe's own list (CPU-culled to its capture sphere, nearest
    // first), stored past the main clusters: CLUSTER_COUNT + list (push water_fog.w).
    const bool all = mirrorPass();
    uint c = all ? 0u : (probeCapture() ? CLUSTER_COUNT + uint(pc.u_water_fog.w + 0.5)
                                        : mainClusterFrag(P));
    uint n = all ? count : clusterLen(cluster_count[c]);
#ifdef CLEAN_CAPTURE
    // (#829) The capture FS walks the NEAREST CAPTURE_LIGHT_CAP of the probe's list
    // (clean_probe_gpu.comp phaseLights sorts it by distance to the probe origin whenever it
    // holds more): the rest lie farther out, where their pools cover a few metre-wide
    // texels of the face at most.
    n = min(n, CAPTURE_LIGHT_CAP);
#endif
    for (uint i = 0u; i < n; ++i)
    {
        const uint li = all ? i : cluster_index[c * CLUSTER_CAP + i];
        if (li >= count) { lgFlag(LG_LIGHT_IDX); continue; }   // (never: an entry past this frame's lights)
        sum += lgFinite(localLight(li, P, N, V, pbr, base, diffuseColor, specularColor, rough, ls, glare, taps, tapsOk,
                                   false), LG_LOCAL);
    }
#endif
    return sum;
}
vec3 localLights(vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, float metal, float rough)
{
    float glare;
    return localLights(P, N, V, pbr, base, metal, rough, legacySpecNone(), glare);
}

// ---- SSR: a view-space march over the PREVIOUS frame's depth + colour (the inputs of
//      LL screenSpaceReflUtil traceScreenRay / tapScreenSpaceReflection, LL:
//      inv_modelview_delta), with a standard hit test. The LL march accepted any sample
//      behind the depth buffer as a surface crossing (no thickness), bisected onto it
//      and returned the bisection's colour unchecked -- a ray passing BEHIND a thin
//      object (an avatar seen from above) "hit" its silhouette and painted a stretched,
//      smeared copy of it along the ray. Here: a front-to-back crossing is bisected,
//      then VERIFIED against a thickness; a failed verify (a depth discontinuity: the
//      ray went behind an occluder) keeps marching; a back-to-front crossing and a hit
//      on a surface facing away from the ray are rejected; the hit fades on low
//      confidence. Settings keep their roles: Iterations = the march budget, RayStep =
//      the first step (m), AdaptiveStepMultiplier = the step growth, DistanceBias = the
//      thickness floor (m), DepthRejectBias = the minimum origin / ray depth (m). ----
vec2 ssrProject(vec3 pos)
{
    vec4 c = ssr.prev_proj * vec4(pos, 1.0);
    return (c.xy / c.w) * 0.5 + 0.5;
}

// The previous frame's depth texel under tc (texelFetch: no filtering across a
// silhouette) -> its view-space position (prev view). Reverse-Z: the far plane (sky,
// depth 0) unprojects to w = 0 -> a huge depth (never a hit).
vec3 ssrPrevViewPos(ivec2 px, ivec2 sz)
{
    px = clamp(px, ivec2(0), sz - 1);
    const float d = texelFetch(uPrevDepth, px, 0).r;
    const vec2 ndc = (vec2(px) + 0.5) / vec2(sz) * 2.0 - 1.0;
    const vec4 p = ssr.prev_inv_proj * vec4(ndc, d, 1.0);
    if (abs(p.w) < 1e-20) return vec3(0.0, 0.0, -1e20);
    return p.xyz / p.w;
}

float ssrLinearDepth(vec2 tc)
{
    const ivec2 sz = textureSize(uPrevDepth, 0);
    const float z = -ssrPrevViewPos(ivec2(tc * vec2(sz)), sz).z;
    return (isnan(z) || isinf(z)) ? 1e20 : z;
}

// Scene normal (prev view space, facing the camera) at a hit texel, from the depth of its
// neighbours: per axis the side with the smaller depth step (so a silhouette's other
// side is not differenced in). Only evaluated on an accepted hit (4 fetches).
vec3 ssrPrevNormal(vec2 tc)
{
    const ivec2 sz = textureSize(uPrevDepth, 0);
    const ivec2 px = clamp(ivec2(tc * vec2(sz)), ivec2(0), sz - 1);
    const vec3 c  = ssrPrevViewPos(px, sz);
    const vec3 r  = ssrPrevViewPos(px + ivec2(1, 0), sz) - c;
    const vec3 l  = c - ssrPrevViewPos(px - ivec2(1, 0), sz);
    const vec3 u  = ssrPrevViewPos(px + ivec2(0, 1), sz) - c;
    const vec3 dn = c - ssrPrevViewPos(px - ivec2(0, 1), sz);
    const vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    const vec3 dy = abs(u.z) < abs(dn.z) ? u : dn;
    vec3 n = cross(dx, dy);
    const float len = length(n);
    if (!(len > 1e-12)) return -normalize(c);   // degenerate: treat as facing the camera
    n /= len;
    return dot(n, c) > 0.0 ? -n : n;            // toward the camera
}

// Returns the hit's confidence (0 = miss): thickness margin * hit-normal facing * the
// hit's distance from the screen border.
float ssrTrace(vec3 position, vec3 reflection, out vec3 hitColor)
{
    const float iterationCount = ssr.params0.x;
    const float rayStep = max(ssr.params0.y, 1e-3);
    const float distanceBias = max(ssr.params0.z, 1e-3);
    const float depthRejectBias = ssr.params0.w;
    const float adaptiveStepMultiplier = max(ssr.params1.x, 1.0);
    // Thickness the depth buffer is assumed to have: the DistanceBias floor, growing with
    // depth (a texel's depth span on an oblique surface ~ depth * pixel angle / slope).
    const float kThickPerZ = 0.03;
    const int   kBisect = 8;          // per candidate crossing (the LL refinement budget)
    const int   kMaxCandidates = 3;   // failed verifies before giving up (cost bound)

    hitColor = vec3(0.0);
    const float originZ = -position.z;
    if (originZ <= depthRejectBias) return 0.0;

    const vec2 sz = vec2(textureSize(uPrevDepth, 0));
    const vec2 originTc = ssrProject(position);

    vec3  prevPos = position;
    float prevDelta = -1.0;           // the origin: on its surface, the ray leaves it (in front)
    float stepLen = rayStep;
    int   candidates = 0;

    for (int i = 0; float(i) < iterationCount; ++i)
    {
        const vec3 pos = prevPos + reflection * stepLen;
        const float rayZ = -pos.z;
        if (rayZ <= depthRejectBias) return 0.0;   // ray reached the camera plane
        const vec2 tc = ssrProject(pos);
        if (any(greaterThan(tc, vec2(1.0))) || any(lessThan(tc, vec2(0.0)))) return 0.0;
        const float sceneZ = ssrLinearDepth(tc);
        const float delta = rayZ - sceneZ;          // > 0: the ray is behind the depth buffer

        // Only a FRONT-to-BACK crossing is a candidate; a ray already behind something
        // (passing under an occluder) stays a non-hit until it comes out in front again
        // (a back-to-front crossing is the far side of a surface: never a hit).
        // (Samples on the origin's own texel(s) say nothing: depth precision there can
        // read either side, so they are neither a candidate nor a "behind" state.)
        const bool inOrigin = all(lessThanEqual(abs(tc - originTc) * sz, vec2(1.5)));
        if (delta > 0.0 && prevDelta <= 0.0 && !inOrigin)
        {
            // Bisect the crossing: lo in front, hi behind.
            vec3 lo = prevPos, hi = pos;
            vec2 hiTc = tc;
            float hiDelta = delta, hiSceneZ = sceneZ;
            for (int r = 0; r < kBisect; ++r)
            {
                const vec3 mid = 0.5 * (lo + hi);
                const vec2 mtc = ssrProject(mid);
                const float mz = ssrLinearDepth(mtc);
                const float md = -mid.z - mz;
                if (md > 0.0) { hi = mid; hiTc = mtc; hiDelta = md; hiSceneZ = mz; }
                else          { lo = mid; }
            }
            // Verify: within the thickness of the surface it crossed. A thin occluder the
            // ray passed behind converges onto its silhouette with a large delta -> reject
            // and keep marching (from behind it).
            const float thickness = max(distanceBias, kThickPerZ * hiSceneZ);
            if (hiDelta < thickness)
            {
                // Back-face: the hit surface must face the incoming ray.
                const vec3 n = ssrPrevNormal(hiTc);
                const float facing = -dot(reflection, n);
                if (facing > 0.0)
                {
                    const vec2 e = 1.0 - abs(hiTc * 2.0 - 1.0);   // border fade (the hit)
                    float conf = (1.0 - smoothstep(0.5, 1.0, hiDelta / thickness))
                               * clamp(facing * 8.0, 0.0, 1.0)
                               * clamp(min(e.x, e.y) * 16.0, 0.0, 1.0);
                    if (conf > 0.0)
                    {
                        // The depth texel's own colour (the colour sampler is linear: at
                        // a silhouette it would blend in the background behind it).
                        hitColor = textureLod(uPrevColor, (floor(hiTc * sz) + 0.5) / sz, 0.0).rgb;
                        return conf;
                    }
                }
            }
            if (++candidates >= kMaxCandidates) return 0.0;
        }
        prevPos = pos;
        prevDelta = inOrigin ? -1.0 : delta;
        // Adaptive growth, capped proportionally to depth (a far reflection still reaches
        // far, a near one keeps steps short enough not to tunnel through what it meets).
        stepLen = min(stepLen * adaptiveStepMultiplier, max(0.5, 0.25 * rayZ));
    }
    return 0.0;
}

// One tap, as LL's probe sampler calls it. Returns rgb + weight (confidence * vignette).
vec4 ssrTap(vec3 P, vec3 N, float glossiness)
{
    vec3 viewPos = (ssr.prev_view * vec4(P, 1.0)).xyz;
    vec3 n = normalize(mat3(ssr.prev_view) * N);
    vec3 rayDirection = normalize(reflect(viewPos, n));

    vec2 tc = ssrProject(viewPos);
    vec2 screenpos = 1.0 - abs(tc * 2.0 - 1.0);
    float vignette = clamp(abs(screenpos.x) * abs(screenpos.y) * 16.0, 0.0, 1.0);
    vignette *= clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 4.0 - 0.05, 0.0, 1.0);
    vignette *= clamp(1.0 + (viewPos.z / 128.0), 0.0, 1.0);
    vignette *= clamp(glossiness * 3.0 - 1.7, 0.0, 1.0);
    if (vignette <= 0.0) return vec4(0.0);

    vec3 hit;
    const float conf = ssrTrace(viewPos, rayDirection, hit);
    if (conf <= 0.0) return vec4(0.0);
    // (last frame's colour: a bad pixel there must not re-enter every frame after it)
    return vec4(lgFinite(hit, LG_SSR), vignette * conf);
}

// ---- Reflection probes: port of LL reflectionProbeF (sphere/box weights, Lagarde
//      box parallax, manual-over-automatic blend, per-probe ambiance). ----
vec3 probeBoxHit(Probe p, vec3 P, vec3 dir, float scale)
{
    vec3 PL = (p.box * vec4(P, 1.0)).xyz;
    vec3 RL = mat3(p.box) * dir;
    vec3 first  = (vec3( scale) - PL) / RL;
    vec3 second = (vec3(-scale) - PL) / RL;
    vec3 far3 = max(first, second);
    float t = min(min(far3.x, far3.y), far3.z);
    return P + dir * t;
}

vec3 probeSphereHit(vec3 o, vec3 dir, vec3 c, float r)
{
    vec3 oc = c - o;
    float tca = dot(oc, dir);
    float d2 = dot(oc, oc) - tca * tca;
    float t1 = tca + sqrt(max(r * r - d2, 0.0));
    return o + dir * t1;
}

// A probe's radiance along dir at lod: its slot in the radiance array, or (#565 format 3)
// its uncompressed live cube.
vec3 probeRadiance(Probe p, vec3 dir, float lod)
{
    const int live = p.idx.y >> 1;
    if (live != 0) return textureLod(uProbeRadiance[1], vec4(dir, float(live - 1)), lod).rgb;
    return textureLod(uProbeRadiance[0], vec4(dir, float(p.idx.x)), lod).rgb;
}

// ---- PROBE TEXEL GUARD (the deferred lighting compute only: CLEAN_LIT_GUARD_COUNTERS) ----
// LG_PROBE says the BLEND went non-finite / past LG_MAX_VALUE, not where it came from. Here
// every raw cube texel a probe contributes is checked before it is weighted (the blend
// weights are bounded: a bad sum needs a bad texel, a bad lookup direction or a bad
// ambiance) and a bad one is attributed: which store (LGPT_*), which cube slot (persistent
// stores only: the slot bitmask LLCleanRendererVK::readLitGuardStats heals from), the value
// range (a legit bright texel quantized just over the guard vs. garbage) and one sample of
// slot / store / class / LOD. Counting only -- the texel still goes into the blend, which
// lgFinite clamps as before, so deferred and forward shade alike.
// Words (binding 20, LG_WORD0 + ...; = LLCleanRendererVK kLgPt*):
//   13..16 bad texels per store, 17 of them non-finite, 18 max finite bad value (float
//   bits), 19 ~min finite bad value (~bits: atomicMax = min), 20 non-finite lookup inputs
//   (direction / LOD: a NaN position or normal reaching the probe walk; or an ambiance
//   past the guard), 21 the last sample
//   (bit layout below), 22 live cube 1's slot + 1 (the realtime capture: rewritten every
//   frame, never healed), 23..30 bitmask of slots with a bad PERSISTENT texel (store, live
//   cube 0, irradiance).
#define LGPT_STORE   0u   // the slot's own layers of the radiance array (format 3: BC6H)
#define LGPT_LIVE0   1u   // format 3 live cube 0 (the default probe, uncompressed)
#define LGPT_LIVE1   2u   // format 3 live cube 1 (the realtime probe's latest capture)
#define LGPT_IRR     3u   // the irradiance array
#ifdef CLEAN_LIT_GUARD_COUNTERS
#define LGPT_WORD    (LG_WORD0 + 13u)
void lgProbeInput(vec3 dir, float s)   // s: the LOD, or the ambiance
{
    if (any(isnan(dir)) || any(isinf(dir)) || isnan(s) || isinf(s) || abs(s) > LG_MAX_VALUE)
        atomicAdd(lg_words[LGPT_WORD + 7u], 1u);
}
void lgProbeTexel(Probe p, vec3 c, uint kind, float lod)
{
    const bool nf = any(isnan(c)) || any(isinf(c));
    if (!nf && !any(greaterThan(c, vec3(LG_MAX_VALUE)))) return;
    const uint slot = uint(p.idx.x) & 255u;
    atomicAdd(lg_words[LGPT_WORD + kind], 1u);
    if (nf) atomicAdd(lg_words[LGPT_WORD + 4u], 1u);
    else
    {
        const uint mb = floatBitsToUint(max(max(c.r, c.g), c.b));   // (> 65000: positive, bits order as values)
        atomicMax(lg_words[LGPT_WORD + 5u], mb);
        atomicMax(lg_words[LGPT_WORD + 6u], ~mb);
    }
    if (kind == LGPT_LIVE1) atomicMax(lg_words[LGPT_WORD + 9u], slot + 1u);
    else atomicOr(lg_words[LGPT_WORD + 10u + (slot >> 5u)], 1u << (slot & 31u));
    // sample: bit 0 set, 1-8 slot, 9-10 store, 11-12 class (idx.w), 13-16 LOD (rounded),
    // 17 non-finite, 18 still fading in, 19 manual, 20 box
    const float lq = (lod >= 0.0 && lod <= 15.0) ? lod + 0.5 : 0.0;   // (NaN: 0)
    atomicExchange(lg_words[LGPT_WORD + 8u],
        1u | (slot << 1u) | (kind << 9u) | ((uint(p.idx.w) & 3u) << 11u) | (min(uint(lq), 15u) << 13u)
           | (nf ? 1u << 17u : 0u) | (p.params.y < 1.0 ? 1u << 18u : 0u) | (p.params.w > 0.5 ? 1u << 19u : 0u)
           | (p.params.z > 0.5 ? 1u << 20u : 0u));
}
#endif
// probeRadiance + the texel guard (its store: the live cube the shading list names).
vec3 probeRadianceG(Probe p, vec3 dir, float lod)
{
    const vec3 c = probeRadiance(p, dir, lod);
#ifdef CLEAN_LIT_GUARD_COUNTERS
    const int live = p.idx.y >> 1;
    lgProbeInput(dir, lod);
    lgProbeTexel(p, c, live == 1 ? LGPT_LIVE0 : (live == 2 ? LGPT_LIVE1 : LGPT_STORE), lod);
#endif
    return c;
}

// A probe's blend weight at P (LL reflectionProbeF sphereWeight / boxWeight): w (its share
// within its group) and dw (its COVERAGE, which sets how much its group holds over the one
// below it). False when P is outside its influence (or it isn't complete).
//
// BLEND GROUPS (not LL): 0 = the default sky probe (probe 0), 1 = the local automatic
// probes (terrain grid + automatic object probes), 2 = manual probes. The manual group goes
// over the automatic class by the manual coverage (LL), and the local automatic group over
// the default probe by ITS coverage. LL normalizes the default probe as one more 1 / d
// weight of the automatic class; its origin is 64 m above the camera, so its weight (~1/64)
// is tiny against a local probe's (1/d, d a few metres) and a lone local probe's share fell
// from ~100 % to 0 within the last few cm .. tens of cm of its radius: a HARD SPHERE-SHAPED
// SEAM (the default probe's sky-blue outside, the local capture inside) on whatever crossed
// an automatic object probe's edge (r 8 m: ~0.35 m wide), and its 1 s fade-in -- which
// scales w -- swept that seam outward from the origin. A local automatic probe's coverage
// is its fade x smoothstep(LL's attenuation: 1 inside r/2, 0 at r), so the default probe
// is blended in across the whole outer half of the radius and the fade-in is a cross-fade.
uint probeGroup(uint pi, Probe p) { return pi == 0u ? 0u : (p.params.w > 0.5 ? 2u : 1u); }

// A cluster list entry that names a probe past this frame's probe count (stale / garbage
// list): skipped (LG_PROBE_IDX). Never expected -- clean_probe_cluster.comp writes only
// indices below hdr.info.x of the same frame.
bool probeIdxBad(uint pi)
{
    if (pi < uint(phdr.info.x)) return false;
    lgFlag(LG_PROBE_IDX);
    return true;
}

bool probeWeight(Probe p, vec3 P, out float w, out float dw)
{
    w = 0.0; dw = 0.0;
    if (p.idx.y == 0) return false;
    if (p.params.z > 0.5)
    {
        vec3 PL = (p.box * vec4(P, 1.0)).xyz;
        if (any(greaterThan(abs(PL), vec3(1.0)))) return false;
        float d = 1.0 - max(max(abs(PL.x), abs(PL.y)), abs(PL.z));
        w  = max(d, 0.001);
        dw = min(4.0 * d, 1.0);
    }
    else
    {
        vec3 ctr = p.sphere.xyz;
        float r = p.sphere.w;
        float d = max(length(P - ctr), 0.001);
        if (d > r) return false;
        float atten = 1.0 - max(d - r * 0.5, 0.0) / max(r * 0.5, 0.001);
        w  = (1.0 / d) * p.params.y * atten;
        dw = p.params.w > 0.5 ? w * max(r, 1.0) * 4.0                      // manual (LL)
                              : p.params.y * smoothstep(0.0, 1.0, atten);   // automatic
    }
    return w > 0.0;
}

// <SS:Skoomastream> Water samples as LL's sampleProbesWater: automatic probes skipped (only
// manual probes plus the default probe 0). The automatic terrain/water grid probes are captured
// 2 m above the water with the whole scene in them and swamp the sky probe, which drew dark,
// wave-edged regions on open water that Firestorm keeps clean. Set around one call only.
bool gProbeSkipAuto = false;
// </SS:Skoomastream>
// Per-group multipliers of the blend: result = k[0] * sum(w c, default) + k[1] * sum(w c,
// automatic) + k[2] * sum(w c, manual); the final weights sum to 1 and are continuous in
// every probe's w and dw (a probe entering or leaving at w = dw = 0 moves nothing).
// ws: the groups' weight sums; dwA / dwM: the automatic / manual coverage sums.
// (Group-indexed values are vec3 components / named variables picked by compare, never
// private arrays: a run-time group index into an array forces it into local memory.)
void probeGroupFactors(float ws0, float ws1, float ws2, float dwA, float dwM, out vec3 k)
{
    const bool has0 = ws0 > 0.0, hasA = ws1 > 0.0, hasM = ws2 > 0.0;
    const float cA = hasA ? (has0 ? min(dwA, 1.0) : 1.0) : 0.0;   // automatic over the default
    const float m  = hasM ? ((has0 || hasA) ? min(dwM, 1.0) : 1.0) : 0.0;   // manual over both
    k.x = has0 ? (1.0 - m) * (1.0 - cA) / ws0 : 0.0;
    k.y = hasA ? (1.0 - m) * cA / ws1 : 0.0;
    k.z = hasM ? m / ws2 : 0.0;
}
// k[g] for a probe group g (probeGroup: 0, 1 or 2).
float probeGroupK(vec3 k, uint g) { return g == 0u ? k.x : (g == 1u ? k.y : k.z); }

// The group factors of the pixel's cluster list (+ the default probe). False: no probe.
bool probeClusterFactors(uint c, uint n, vec3 P, out vec3 k)
{
    float ws0 = 0.0, ws1 = 0.0, ws2 = 0.0;
    float dwA = 0.0, dwM = 0.0;
    for (uint k2 = 0u; k2 <= n; ++k2)
    {
        const uint pi = (k2 == n) ? 0u : probe_index[c * PROBE_CLUSTER_CAP + k2];   // + default probe 0
        if (probeIdxBad(pi)) continue;
        if (gProbeSkipAuto && pi != 0u && probes[pi].params.w < 0.5) continue;   // <SS:Skoomastream/>
        float w, dw;
        if (!probeWeight(probes[pi], P, w, dw)) continue;
        const uint g = probeGroup(pi, probes[pi]);
        if (g == 0u) ws0 += w;
        else if (g == 1u) { ws1 += w; dwA += dw; }
        else { ws2 += w; dwM += dw; }
    }
    probeGroupFactors(ws0, ws1, ws2, dwA, dwM, k);
    return ws0 > 0.0 || ws1 > 0.0 || ws2 > 0.0;
}

// One probe's radiance / irradiance x w (box / sphere parallax as LL), for the caller to
// add to its sums (zero for what isn't needed).
void probeAccum(Probe p, vec3 P, vec3 N, vec3 R, float lod, float lod2, vec3 amblit,
                bool needRad, bool needRad2, bool needIrr, float w,
                out vec3 rs, out vec3 r2s, out vec3 is)
{
    rs = vec3(0.0); r2s = vec3(0.0); is = vec3(0.0);
    const vec3 ctr = p.sphere.xyz;
    vec3 rdir = R, ndir = N;
    if (p.params.z > 0.5)
    {
        if (needRad || needRad2) rdir = probeBoxHit(p, P, R, 1.0) - ctr;
        if (needIrr)             ndir = probeBoxHit(p, P, N, 3.0) - ctr;
    }
    else if (needRad || needRad2)
        rdir = (p.idx.z != 0) ? probeSphereHit(P, R, ctr, p.sphere.w) - ctr : R;
    if (needRad)  rs  = probeRadianceG(p, rdir, lod) * w;
    if (needRad2) r2s = probeRadianceG(p, rdir, lod2) * w;
    if (needIrr)
    {
        const float amb = max(phdr.info.z, p.params.x);   // max(sky probe ambiance, its own)
        const vec3 itx = textureLod(uProbeIrradiance, vec4(ndir, float(p.idx.x)), 0.0).rgb;
#ifdef CLEAN_LIT_GUARD_COUNTERS
        lgProbeInput(ndir, amb);   // (the ambiance scales the texel: a bad one counts as a bad input)
        lgProbeTexel(p, itx, LGPT_IRR, 0.0);
#endif
        const vec3 it = itx * amb;
        is = mix(amblit, it, min(amb, 1.0)) * w;
    }
}

// PROBE TOP-N (RenderVKProbeTopN -> ssr.lit_dbg.y; 0 = every candidate): only the N
// probes with the largest FINAL blend weight (group share x the group factor) are fetched.
// The kept weights are reduced by the (N+1)-th largest before renormalizing, so a probe
// leaves the kept set at weight 0: where the N-th and (N+1)-th swap rank the blend is
// continuous (plain truncation stepped there by the swapped probe's share -- curved seams
// along the equal-weight surfaces). With N >= the candidates it is the plain walk.
#define PROBE_TOPN_MAX 4
// The kept probes and their normalized weights, IN PLACE: entries [0, topN) in rank order,
// a dropped one at weight 0 (callers skip bw <= 0). Returns how many are kept (0: no probe;
// bw is then not normalized). Every bi / bw index is a constant (the loops are unrolled to
// PROBE_TOPN_MAX and predicated on topN; probeTopNPick for a run-time slot), so the lists
// stay in registers.
uint probeTopNWeights(uint c, uint n, uint topN, vec3 P, out uint bi[PROBE_TOPN_MAX + 1], out float bw[PROBE_TOPN_MAX + 1])
{
    [[unroll]] for (int j = 0; j <= PROBE_TOPN_MAX; ++j) { bw[j] = 0.0; bi[j] = 0u; }
    vec3 gk;
    if (!probeClusterFactors(c, n, P, gk)) return 0u;
    for (uint k = 0u; k <= n; ++k)
    {
        const uint pi = (k == n) ? 0u : probe_index[c * PROBE_CLUSTER_CAP + k];
        if (probeIdxBad(pi)) continue;
        if (gProbeSkipAuto && pi != 0u && probes[pi].params.w < 0.5) continue;   // <SS:Skoomastream/>
        float w, dw;
        if (!probeWeight(probes[pi], P, w, dw)) continue;
        float fw = w * probeGroupK(gk, probeGroup(pi, probes[pi]));
        uint  id = pi;
        // insertion into the descending list (topN kept + the first one dropped)
        [[unroll]] for (int j = 0; j <= PROBE_TOPN_MAX; ++j)
        {
            if (uint(j) <= topN && fw > bw[j])
            {
                const float tw = bw[j]; const uint ti = bi[j];
                bw[j] = fw; bi[j] = id;
                fw = tw; id = ti;
            }
        }
    }
    float cut = 0.0;        // the largest dropped weight, bw[topN] (0: nothing dropped)
    [[unroll]] for (int j = 1; j <= PROBE_TOPN_MAX; ++j)
        if (uint(j) == topN) cut = bw[j];
    float ws = 0.0;
    [[unroll]] for (int j = 0; j < PROBE_TOPN_MAX; ++j)
        if (uint(j) < topN) ws += max(bw[j] - cut, 0.0);
    if (ws <= 0.0)          // every kept weight ties the cut: plain truncation
    {
        cut = 0.0;
        [[unroll]] for (int j = 0; j < PROBE_TOPN_MAX; ++j)
            if (uint(j) < topN) ws += bw[j];
    }
    if (ws <= 0.0) return 0u;
    uint m = 0u;
    [[unroll]] for (int j = 0; j < PROBE_TOPN_MAX; ++j)
    {
        if (uint(j) < topN)
        {
            bw[j] = max(bw[j] - cut, 0.0) / ws;
            if (!(bw[j] <= 0.0)) ++m;   // (the consumers' skip test, exactly)
        }
    }
    return m;
}
// bi[j] / bw[j] for a run-time slot j (< PROBE_TOPN_MAX) through constant indices.
void probeTopNPick(uint bi[PROBE_TOPN_MAX + 1], float bw[PROBE_TOPN_MAX + 1], uint j, out uint id, out float w)
{
    id = bi[0]; w = bw[0];
    [[unroll]] for (int t = 1; t < PROBE_TOPN_MAX; ++t)
        if (uint(t) == j) { id = bi[t]; w = bw[t]; }
}

bool sampleProbesTopN(uint c, uint n, uint topN, vec3 P, vec3 N, vec3 R, float lod, float lod2, vec3 amblit,
                      bool needRad, bool needRad2, bool needIrr,
                      out vec3 rad, out vec3 rad2, out vec3 irr)
{
    rad = vec3(0.0); rad2 = vec3(0.0); irr = amblit;
    uint  bi[PROBE_TOPN_MAX + 1];
    float bw[PROBE_TOPN_MAX + 1];
    const uint m = probeTopNWeights(c, n, topN, P, bi, bw);
    if (m == 0u) return false;
    vec3 rs = vec3(0.0), r2s = vec3(0.0), is = vec3(0.0);
    for (uint j = 0u; j < topN; ++j)
    {
        uint id; float w;
        probeTopNPick(bi, bw, j, id, w);
        if (w <= 0.0) continue;
        vec3 dr, dr2, di;
        probeAccum(probes[id], P, N, R, lod, lod2, amblit, needRad, needRad2, needIrr, w, dr, dr2, di);
        rs += dr; r2s += dr2; is += di;
    }
    rad = lgFinite(rs, LG_PROBE); rad2 = lgFinite(r2s, LG_PROBE);
    irr = needIrr ? lgFinite(is, LG_PROBE) : amblit;
    return true;
}

// Radiance (at lod) along R, optionally a second radiance at lod2 (the legacy sharp env),
// and irradiance along N from the probes around P. ONE cluster walk serves every lookup;
// the need* flags skip the cube fetches (and box-hit math) a caller discards -- matte
// legacy faces need irradiance only, classic-sky legacy faces no irradiance, fullbright
// shiny radiance only. The weights don't depend on what is fetched, so every result a
// caller uses is bit-identical to a separate full walk.
bool sampleProbesEx(vec3 P, vec3 N, vec3 R, float lod, float lod2, vec3 amblit,
                    bool needRad, bool needRad2, bool needIrr,
                    out vec3 rad, out vec3 rad2, out vec3 irr)
{
    rad = vec3(0.0); rad2 = vec3(0.0); irr = amblit;
    if (phdr.info.w < 0.5 || probeCapture() || litDbg(LDBG_NO_PROBES)) return false;
    uint count = uint(phdr.info.x);
    if (count == 0u) return false;

#ifdef CLEAN_PROBE_CLUSTER_HOOK
    // (#859: the RT reflection pass looks up probes at a ray HIT, not at this pixel's surface:
    // the cluster its position projects into, as the mirror capture does)
    uint c = CLEAN_PROBE_CLUSTER_HOOK(P);
#else
    uint c = mirrorPass() ? mainClusterProject(P) : mainClusterFrag(P);
#endif
    // low 16 bits: list length (the high half counts the candidates; clean_probe_cluster.comp)
    uint n = litDbg(LDBG_PROBE0_ONLY) ? 0u : min(probe_count[c] & 0xFFFFu, PROBE_CLUSTER_CAP);
    const uint topN = specProbeTopN();   // (spec constant 15; clamp PROBE_TOPN_MAX)
    if (topN > 0u && n + 1u > topN)
        return sampleProbesTopN(c, n, topN, P, N, R, lod, lod2, amblit, needRad, needRad2, needIrr, rad, rad2, irr);

    // Per-group sums (default / automatic / manual) as named variables: the group is a
    // run-time value, and a group-indexed private array would sit in local memory.
    vec3  rsum0 = vec3(0.0), rsum1 = vec3(0.0), rsum2 = vec3(0.0);
    vec3  r2sum0 = vec3(0.0), r2sum1 = vec3(0.0), r2sum2 = vec3(0.0);
    vec3  isum0 = vec3(0.0), isum1 = vec3(0.0), isum2 = vec3(0.0);
    float wsum0 = 0.0, wsum1 = 0.0, wsum2 = 0.0;
    float dwA = 0.0, dwM = 0.0;

    for (uint k = 0u; k <= n; ++k)
    {
        uint pi = (k == n) ? 0u : probe_index[c * PROBE_CLUSTER_CAP + k];   // + default probe 0
        if (probeIdxBad(pi)) continue;
        if (gProbeSkipAuto && pi != 0u && probes[pi].params.w < 0.5) continue;   // <SS:Skoomastream/>
        Probe p = probes[pi];
        float w, dw;
        if (!probeWeight(p, P, w, dw)) continue;
        const uint g = probeGroup(pi, p);
        vec3 dr, dr2, di;
        probeAccum(p, P, N, R, lod, lod2, amblit, needRad, needRad2, needIrr, w, dr, dr2, di);
        if (g == 0u)      { rsum0 += dr; r2sum0 += dr2; isum0 += di; wsum0 += w; }
        else if (g == 1u) { rsum1 += dr; r2sum1 += dr2; isum1 += di; wsum1 += w; dwA += dw; }
        else              { rsum2 += dr; r2sum2 += dr2; isum2 += di; wsum2 += w; dwM += dw; }
    }
    if (wsum0 <= 0.0 && wsum1 <= 0.0 && wsum2 <= 0.0) return false;
    vec3 gk;
    probeGroupFactors(wsum0, wsum1, wsum2, dwA, dwM, gk);
    rad  = lgFinite(rsum0  * gk.x + rsum1  * gk.y + rsum2  * gk.z, LG_PROBE);
    rad2 = lgFinite(r2sum0 * gk.x + r2sum1 * gk.y + r2sum2 * gk.z, LG_PROBE);
    irr  = needIrr ? lgFinite(isum0 * gk.x + isum1 * gk.y + isum2 * gk.z, LG_PROBE) : amblit;
    return true;
}

// DEBUG VIEW 25 (RenderVKDebugView): the probe blend at P exactly as sampleProbesEx weights
// it (cluster list, groups, top-N): sum over the contributing probes of final weight x the
// probe's identity colour (hue = cube slot; vivid automatic object probe, pastel terrain
// grid, near-white manual, dark slate = the default sky probe). A smooth blend shows as a
// gradient; a seam in the probe lighting shows as a HARD colour edge here. a = how many
// probes contribute (final weight >= 1 %).
vec4 probeBlendDebug(vec3 P)
{
    vec3 col = vec3(0.0);
    uint used = 0u;
    if (phdr.info.w < 0.5 || uint(phdr.info.x) == 0u) return vec4(1.0, 0.0, 1.0, 0.0);
    const uint c = mirrorPass() ? mainClusterProject(P) : mainClusterFrag(P);
    const uint n = litDbg(LDBG_PROBE0_ONLY) ? 0u : min(probe_count[c] & 0xFFFFu, PROBE_CLUSTER_CAP);
    const uint topN = specProbeTopN();   // (spec constant 15; clamp PROBE_TOPN_MAX)
    uint  bi[PROBE_TOPN_MAX + 1];
    float bw[PROBE_TOPN_MAX + 1];
    vec3 gk;
    const bool trim = topN > 0u && n + 1u > topN;
    uint m = 0u;
    if (trim) m = probeTopNWeights(c, n, topN, P, bi, bw);
    else if (!probeClusterFactors(c, n, P, gk)) return vec4(1.0, 0.0, 1.0, 0.0);
    const uint walk = trim ? (m > 0u ? topN : 0u) : n + 1u;
    for (uint k = 0u; k < walk; ++k)
    {
        uint pi; float fw;
        if (trim)
        {
            probeTopNPick(bi, bw, k, pi, fw);
            if (fw <= 0.0) continue;   // dropped (probeTopNWeights keeps the slots in place)
        }
        else
        {
            pi = (k == n) ? 0u : probe_index[c * PROBE_CLUSTER_CAP + k];
            if (probeIdxBad(pi)) continue;
            float w, dw;
            if (!probeWeight(probes[pi], P, w, dw)) continue;
            fw = w * probeGroupK(gk, probeGroup(pi, probes[pi]));
        }
        const Probe p = probes[pi];
        vec3 id = vec3(0.12, 0.14, 0.2);   // the default sky probe
        if (pi != 0u)
        {
            const float hue = fract(float(p.idx.x) * 0.61803399);
            const vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
            id = mix(vec3(1.0), rgb, p.idx.w == 3 ? 1.0 : (p.idx.w == 2 ? 0.45 : 0.15));
        }
        col += id * fw;
        if (fw >= 0.01) ++used;
    }
    return vec4(col, float(used));
}

// Radiance (at lod) along R and irradiance along N (one full walk).
bool sampleProbes(vec3 P, vec3 N, vec3 R, float lod, vec3 amblit, out vec3 rad, out vec3 irr)
{
    vec3 rad2;
    return sampleProbesEx(P, N, R, lod, 0.0, amblit, true, false, true, rad, rad2, irr);
}

// ---- Sun/moon shadows (LL shadowUtil sampleDirectionalShadow, cascaded) ----
layout(set = 0, binding = 15) uniform sampler2DArrayShadow uShadowMap;

// Cascade index covering view depth vz (3 past the last split).
int shadowCascade(float vz)
{
    vec4 c = ssr.shadow_clip;
    return vz < c.x ? 0 : (vz < c.y ? 1 : (vz < c.z ? 2 : 3));
}

// Raw depth of the same cascades (non-comparison sampler) for the PCSS blocker search.
layout(set = 0, binding = 26) uniform sampler2DArray uShadowMapRaw;

// SHADOW-MAP RECEIVER FIT (RenderVKShadowReceiverFit, frame info shadow_fit; 0 = the old
// receiver). The Gaussian / PCSS filters compare their taps against the receiver's PLANE
// extrapolated to each tap (shadowRPDB). That plane comes from the interpolated VERTEX
// normal (the G-buffer's Ng), which on smoothed meshes / terrain is tilted from the facet the
// map actually holds; the error grows with the tap's distance, and on sun-facing ground the
// other margins are ~0 (the normal offset goes with 1 - N.L, the raster slope bias with the
// facet's slope to the light). In the far cascades (camera high or zoomed out: 0.3-0.6 m
// texels, 16-texel PCSS radii) that is metres of extrapolation -> flickering triangle-shaped
// acne wedges on flat ground. The fix is the standard one: a slope tolerance proportional to
// the tap's offset (zero at the pixel itself, so contact shadows stay), in texels of the
// cascade (it scales with the cascade the way the error does).
bool shadowFitOn() { return ssr.shadow_fit.x > 0.5; }
// Light depth (NDC) per texel of tap offset the receiver plane may be off by. Not in cascade 0
// (~1.4 cm texels at 2048: a corner tap's tolerance would be ~2 cm of depth -- brow / eyelid /
// snout shadows are ~1 cm features), as the sin(theta) normal offset in sunShadowTCore.
float shadowPlaneTol(int c)
{
    if (!shadowFitOn() || c == 0) return 0.0;
    const mat4 m = ssr.shadow_mat[c];
    return ssr.shadow_fit.y * ssr.shadow_texel[c] * length(vec3(m[0][2], m[1][2], m[2][2]));
}
// The receiver plane's depth at each of the four texels textureGather returns for texel-space
// point p (off = p - the receiver, in texels; dz = shadowRPDB), each at its OWN centre.
// Gather order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0), i0 = floor(p - 0.5).
vec4 shadowGatherPlane(float ref, vec2 off, vec2 p, vec2 dz)
{
    const vec2 f = fract(p - 0.5);   // p minus the (i0, j0) texel's centre
    return ref + dot(off - f, dz) + vec4(dz.y, dz.x + dz.y, dz.x, 0.0);
}

// Receiver-plane depth bias, analytic: the depth step per shadow texel of the plane
// through the receiver with geometric normal N, in cascade c's light space. The cascade
// matrix is affine (orthographic), so the plane's shadow-space normal is M^-T N; with
// M's rows r0 r1 r2 that is (N.(r1 x r2), N.(r2 x r0), N.(r0 x r1)) up to a common
// factor. No screen derivatives, so it is valid in the divergent branches sunShadow is
// called from. Clamped: at grazing light the slope runs away (silhouette guard).
vec2 shadowRPDB(int c, vec3 N, float texel_uv)
{
    const mat4 m = ssr.shadow_mat[c];
    const vec3 r0 = vec3(m[0][0], m[1][0], m[2][0]);
    const vec3 r1 = vec3(m[0][1], m[1][1], m[2][1]);
    const vec3 r2 = vec3(m[0][2], m[1][2], m[2][2]);
    const vec3 ns = vec3(dot(N, cross(r1, r2)), dot(N, cross(r2, r0)), dot(N, cross(r0, r1)));
    if (abs(ns.z) < 1e-6) return vec2(0.0);
    // NDC xy spans 2 per uv unit
    return clamp(-ns.xy / ns.z * (2.0 * texel_uv), vec2(-0.004), vec2(0.004));
}

// PCSS: blocker search on the raw depth, penumbra from the receiver-blocker gap, then a
// variable-radius disc PCF. The penumbra is physical: gap (world m, from the cascade's
// depth scale) x tan(light angular radius) / world texel size. The light's angular
// radius follows Shadow softness (the RT sun's disc, sun_rt.x), never smaller than the
// real sun's (0.27 deg) so contact hardening shows at the default softness.
float shadowPCSS(int c, vec2 uv, float ref, float res, vec2 dz)
{
    const float layer = float(c);
    const float texel = 1.0 / res;
    const float phi = 6.2831853 * shadowIGN(shadowNoisePx());
    const vec2 cs = vec2(cos(phi), sin(phi));
    const mat4 m = ssr.shadow_mat[c];
    const float dz_per_m = max(length(vec3(m[0][2], m[1][2], m[2][2])), 1e-6);
    const float tex_per_gap = tan(max(ssr.sun_rt.x, 0.00465)) / (dz_per_m * max(ssr.shadow_texel[c], 1e-5));
    // RECEIVER FIT (shadowFitOn): every gathered texel is compared against the receiver
    // plane at ITS OWN centre (the disc point's plane depth was up to a texel of slope off
    // for the gather's corner texels -- the raster's whole slope bias), minus the plane
    // tolerance for the tap's distance; the radius (and the search) is capped in metres.
    const bool fit = shadowFitOn();
    const float tol = shadowPlaneTol(c);
    const vec2 cc = uv * res;
    const float rmax = (fit && ssr.shadow_fit.z > 0.0)
                     ? clamp(ssr.shadow_fit.z / max(ssr.shadow_texel[c], 1e-5), 1.0, 16.0) : 16.0;
    const float search = min(8.0, rmax);
    float sum = 0.0, cnt = 0.0;
    [[unroll]] for (int i = 0; i < 8; ++i)   // constant disc indices (no local copy of the table)
    {
        const vec2 off = pcssRotate(kPCSSBlockerDisc[i], cs) * search;
        const vec4 d = textureGather(uShadowMapRaw, vec3(uv + off * texel, layer), 0);
        const vec4 r = fit ? shadowGatherPlane(ref, off, cc + off, dz) - tol * (length(off) + 0.5)
                           : vec4(ref + dot(off, dz));
        const vec4 b = step(d, r);   // nearer the light than the receiver
        sum += dot(d, b);
        cnt += dot(b, vec4(1.0));
    }
    if (cnt <= 0.0) return 1.0;   // nothing occludes: lit (the cheap open-ground exit)
    const float radius = clamp((ref - sum / cnt) * tex_per_gap, 1.0, rmax);
    float sh = 0.0, ws = 0.0;
    [[unroll]] for (int i = 0; i < 12; ++i)
    {
        const vec2 unit = kPCSSFilterDisc[i];
        const vec2 off  = pcssRotate(unit, cs) * radius;
        const vec4 d    = textureGather(uShadowMapRaw, vec3(uv + off * texel, layer), 0);
        const float w   = 1.0 - dot(unit, unit);
        const vec4 r    = fit ? shadowGatherPlane(ref, off, cc + off, dz) - tol * (length(off) + 0.5)
                              : vec4(ref + dot(off, dz));
        sh += dot(step(r, d), vec4(0.25)) * w;
        ws += w;
    }
    return sh / ws;
}

// Translucency layer of the cascades (#212, render_opts.z): half resolution, the
// nearest translucent caster's depth (comparison sampler) + the product of every
// translucent caster's transmittance (RGBA8, white = clear). A receiver behind the
// nearest translucent caster takes its colour; in front of it (or the caster
// itself), none. The depth compare is bilinear-filtered, so tinted edges are soft.
layout(set = 0, binding = 27) uniform sampler2DArrayShadow uShadowTransDepth;
layout(set = 0, binding = 28) uniform sampler2DArray uShadowTransColor;
vec3 shadowTint(int c, vec3 P)
{
    if (!specTransOn()) return vec3(1.0);
    const vec4 s = shadowSpace(c, P);
    const vec2 uv = s.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || s.z > 1.0) return vec3(1.0);
    const float front = texture(uShadowTransDepth, vec4(uv, float(c), s.z - ssr.shadow_params.x));
    if (front > 0.999) return vec3(1.0);
    return mix(texture(uShadowTransColor, vec3(uv, float(c))).rgb, vec3(1.0), front);
}

// One cascade lookup, filtered per SlipstreamRenderShadowFilterQuality (see shadowFilterQuality).
float shadowTap(int c, vec3 P, vec3 N)
{
    vec4 s = shadowSpace(c, P);
    vec2 uv = s.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || s.z > 1.0) return 1.0;
    const float ref = s.z - ssr.shadow_params.x;
    const float layer = float(c);
    const float res = max(ssr.shadow_params.z, 1.0);
    const int q = shadowFilterQuality();
    if (q == 0) return texture(uShadowMap, vec4(uv, layer, ref));
    const vec2 dz = shadowRPDB(c, N, 1.0 / res);
    if (q == 3) return shadowPCSS(c, uv, ref, res, dz);
    const int K = q == 1 ? 4 : 6;
    const vec2 cc = uv * res;
    const vec2 base = floor(cc + 0.5) - float(K / 2);
    float wx[6], wy[6];
    shadowGaussW(K, cc, base, wx, wy);
    const float tol = shadowPlaneTol(c);   // (0 with the receiver fit off)
    float sh = 0.0, ws = 0.0;
    // Unrolled to the largest K, as projShadowMap's gather (constant wx / wy indices).
    [[unroll]] for (int by = 0; by < 6; by += 2)
        [[unroll]] for (int bx = 0; bx < 6; bx += 2)
            if (by < K && bx < K)
            {
                const vec2 blk = base + vec2(float(bx), float(by)) + 1.0;   // block centre, texels
                // (the block's texels sit +-0.5 texel from its centre: + 0.5 in the tolerance)
                const float rb = ref + dot(blk - cc, dz) - tol * (length(blk - cc) + 0.5);
                const vec4 g = textureGather(uShadowMap, vec3(blk / res, layer), rb);
                sh += g.w * (wx[bx] * wy[by])     + g.z * (wx[bx + 1] * wy[by])
                    + g.x * (wx[bx] * wy[by + 1]) + g.y * (wx[bx + 1] * wy[by + 1]);
                ws += (wx[bx] + wx[bx + 1]) * (wy[by] + wy[by + 1]);
            }
    return sh / max(ws, 1e-6);
}

#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
vec3 sunShadowRayRGB(vec3 P, vec3 N, vec3 L)
{
    rtSelfBegin(P);
    // Transparent receivers trace opaque-only (see sunShadowRayCore); within
    // RenderVKRTTransReceiverAlphaDist they keep the cut-outs' alpha test.
#ifdef CLEAN_NO_TANGENT_IN
    const bool opaque_only = true;
    const vec3 Ng = N;
#else
    const float camd = length(P - pc.u_cam_time.xyz);
    const bool blend = (v_flags & 2u) != 0u;
    const bool far_cut = ssr.taa_scale.z > 0.0 && camd > ssr.taa_scale.z;
    const bool opaque_only = blend || far_cut;
    rt_opaque_cutouts = blend && !far_cut && camd < rtTransCutoutDist();
    const vec3 Ng = rtFaceNormal(N);
#ifndef CLEAN_RT_SUN_HARD   // (#829 capture FS: no terminator probe -- millimetre facet detail
                            //  under metre-wide probe texels)
    // SHADOW TERMINATOR (clean_shadow_rt.glsl rtTerminatorProbe): in the band where a
    // smooth-shaded facet can self-shadow, the Hanika origin lift from the triangle's
    // vertex normals (the camera probe finds the RT copy of this very triangle).
    // Band as the mask's (-0.5 < Ng.L < 0.2, Ng on the visible side).
    const float ngl = dot(Ng, L) * (dot(Ng, pc.u_cam_time.xyz - P) < 0.0 ? -1.0 : 1.0);
    if (rtTerminatorOn() && rtTerminatorBand(ngl))
    {
        vec3 tng;
        rt_term_on = rtTerminatorCached(P, pc.u_cam_time.xyz, Ng, RT_TERM_FACE_COS, rtTermTriPx(), rt_term_lift, rt_term_ns, tng);
    }
#endif
#endif
    const vec3 v = sunShadowRayCore(P, Ng, L, pc.u_cam_time.xyz, pc.u_cam_time.w, projNoisePx(), opaque_only, rtReceiverRays());
    rt_opaque_cutouts = false;
    rt_term_on = false;
    return v;
}
#endif

// Sun/moon visibility at P (1 = lit). Receivers offset along the normal by the
// cascade's texel size and along the light like LL (1-N.L)*offset*2 (capped at half a
// texel); the last 20% of
// each cascade blends into the next, and the last cascade fades out to lit. Ray-query
// variants with RenderVKSunShadows 2 trace instead (same range, same fade-out).
float sunShadowTCore(vec3 P, vec3 N, out vec3 tint)
{
    tint = vec3(1.0);
    // (Spec constant 9, clean_frame_info.glsl: off folds everything below, cascades-only
    // folds the traced branches.)
    if (!specSunOn() || litDbg(LDBG_NO_SUN_SHADOW)) return 1.0;
    const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    if (vz >= ssr.shadow_clip.w) return 1.0;
    const vec3 L = atmosLightDir();
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
    // (Probe / mirror captures bind this variant too in ray-query frames: the cascades are not
    // rendered then. The #829 capture FS traces one hard ray here, CLEAN_RT_SUN_HARD.)
    if (specSunTraced())
    {
        // Keep the cascades' far fade so the shadowed range ends the same way.
        const float fade = clamp((vz - ssr.shadow_clip.w * 0.8) / max(ssr.shadow_clip.w * 0.2, 1e-3), 0.0, 1.0);
        const vec3 vis = mix(sunShadowRayRGB(P, N, L), vec3(1.0), fade);
        const float sv = max(max(vis.r, vis.g), vis.b);
        if (sv > 1e-4) tint = vis / sv;
        return sv;
    }
#else
    // Ray-traced sun this frame, but this pass has no ray-query variant: the probe /
    // mirror capture pipelines. The cascades aren't rendered in that mode (the map holds
    // stale / never-written depth -> everything "shadowed": probes captured dark and
    // darkened every nearby surface's ambient + reflections), so these captures take
    // the sun unshadowed. (A main-view plain frame -- TLAS not valid -- did render them.)
    if (specSunTraced() && rtMaskUsable())
    {
        const vec3 vis = rtMaskSun(P);   // traced by the mask pass (fade applied there)
        const float sv = max(max(vis.r, vis.g), vis.b);
        if (sv > 1e-4) tint = vis / sv;
        return sv;
    }
    if (specSunTraced() && (probeCapture() || mirrorPass())) return 1.0;
#endif
    const float nl = clamp(dot(N, L), 0.0, 1.0);
    const float k = 1.0 - nl;
    const int c = shadowCascade(vz);
    // Receiver offsets, both proportional to the cascade's world texel (the standard
    // normal-offset rule: the acne they hide is one texel of depth quantization):
    // 1.5 texels along the normal, and LL's light-direction offset (RenderShadowOffset,
    // (1 - N.L) * offset * 2 toward the light) capped at half a texel. LL's constant
    // 2 cm toward the light was 1.4 texels in cascade 0 on top of the normal offset --
    // close-up faces lost brow / nose / snout shadows to it -- and nothing at all in
    // cascade 3.
    // RECEIVER FIT (shadowFitOn): past cascade 0 the normal offset scales with sin(theta)
    // (the standard normal-offset rule: the depth error a texel of lateral slop causes goes
    // with tan(theta), and 1 - cos(theta) is ~theta^2 / 2 -- ~0 on sun-facing ground at
    // 10-30 degrees, where the far cascades' metre-scale texels flickered). Cascade 0 keeps
    // 1 - N.L: close-up faces / bodies keep their contact shadows exactly as before.
    const float sn = sqrt(max(1.0 - nl * nl, 0.0));
    const bool fit = shadowFitOn();
    const float nw0 = (fit && c > 0) ? sn : k;
    const float nw1 = fit ? sn : k;   // (cascade c + 1 >= 1)
    const float lo2 = ssr.shadow_params.y * 2.0;
    const vec3 P0 = P + N * (ssr.shadow_texel[c] * 1.5 * nw0) + L * (k * min(lo2, 0.5 * ssr.shadow_texel[c]));
    float s = shadowTap(c, P0, N);
    const float cend = ssr.shadow_clip[c];
    const float t = clamp((vz - cend * 0.8) / max(cend * 0.2, 1e-3), 0.0, 1.0);
    const vec3 P1 = (c < 3) ? P + N * (ssr.shadow_texel[c + 1] * 1.5 * nw1)
                              + L * (k * min(lo2, 0.5 * ssr.shadow_texel[c + 1])) : P0;
    if (t > 0.0)
    {
        if (c < 3) s = mix(s, shadowTap(c + 1, P1, N), t);
        else       s = mix(s, 1.0, t);
    }
    // Translucent casters (#212): only where some light still arrives.
    if (s > 0.001 && specTransOn())
    {
        tint = shadowTint(c, P0);
        if (t > 0.0) tint = mix(tint, c < 3 ? shadowTint(c + 1, P1) : vec3(1.0), t);
    }
    return s;
}

// VOLUMETRIC CLOUD SHADOW (#719, clean_cloud_shadow_map.glsl): the cloud layer's sun / moon
// transmittance at P, MULTIPLIED into every visibility the core returns -- cascades, the
// ray-traced sun, the RT shadow mask's consumer (rtMaskSun) and the early-outs (sun
// shadows off / past the far split: the clouds still shade). Applied at the consumer, not in
// the mask / denoiser histories, so moving cloud shadows never smear. 1 when the layer is off.
float sunShadowT(vec3 P, vec3 N, out vec3 tint)
{
    return sunShadowTCore(P, N, tint) * cloudShadowMap(P);
}

// Sun/moon visibility without the translucent tint (water, particles).
float sunShadow(vec3 P, vec3 N)
{
    vec3 tint;
    return sunShadowT(P, N, tint);
}
