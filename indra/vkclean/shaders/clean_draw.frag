#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
#extension GL_KHR_shader_subgroup_ballot : require   // overdraw diagnostic (kOverdrawStats)
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#extension GL_EXT_ray_query : require   // ray-traced projector shadows (clean_draw_common.glsl)
#endif
#ifdef REZFX_WIRE
#extension GL_EXT_fragment_shader_barycentric : require   // <GMDX> the Tron rez-in wireframe (gmdx_rezfx.glsl)
#endif
#if !defined(CLEAN_CAPTURE) && !defined(CLEAN_GBUFFER)
// FORWARD FROXEL WALK (clean_draw_common.glsl localLights, RenderVKForwardFroxels): the main
// view's forward lit FS reads the deferred lighting's froxel light masks through their device
// address (LightInfo.fwd.xy) and walks them OR-ed over the subgroup.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#define CLEAN_FWD_FROXEL
#endif
// vkclean — GPU-driven draw fragment shader (opaque / alpha-mask / alpha-blend).
//
// Reads the REAL Material SSBO layout (LLCleanMaterialStore::Material, 96 B) via
// the per-instance material_id, then samples the base-color texture. In GL the
// Material carries a GLuint64 bindless handle per map; the Vulkan-native analog
// is a descriptor-indexed sampler array — the base-color handle becomes an index
// into a runtime-sized combined-image-sampler array (VK_EXT_descriptor_indexing).
//
// ONE shader drives all three static buckets; the pipeline state differs:
//   * opaque  : blend off, depth write on  -> alpha output ignored.
//   * mask    : blend off, depth write on  -> discard where alpha < cutoff.
//   * alpha   : src-alpha blend, depth write off -> alpha output composites.
// The material's alpha_cutoff selects mask behaviour: cutoff >= 0 means the
// material is an alpha-mask (foliage/cutout) so we alpha-test; cutoff < 0 (the
// store's sentinel for "no mask") skips the test. Sun/ambient come from the draw
// push constant (real LLEnvironment day cycle).
//
// DEFERRED (RenderVKDeferredLighting): decodeSurface() is the material half, lightSurface()
// (clean_surface_light.glsl) the lighting half. -DCLEAN_GBUFFER builds the G-buffer
// variant (decode + pack, clean_gbuffer.glsl); clean_deferred_light.comp lights it with
// the same lightSurface(). Forward (this file without CLEAN_GBUFFER) runs both halves.

// DEBUG SPECIALIZATION (clean_draw_common.glsl kDebugViews): constant 1. The renderer's lit
// pipelines (LIT_*, both variants) are built with it FALSE -- no RenderVKDebugView terms,
// RenderVKLitDebugMask switches, Highlight Transparent / blend-tolerance tints or RT
// terminator view in the shipping code -- and their true twins are built on first use
// (LLCleanRendererVK::ensureLitDebug). The default (true) keeps every other pipeline built
// from this module (HUD, mirror, cluster, overdraw stats) exactly as before. The G-buffer
// variant decodes only (its NO_MAPS / NO_LEGACY_ENV switches stay a plain constant).
// CAPTURE FS (#829, -DCLEAN_CAPTURE; RenderVKProbeCaptureSlim): the reflection-probe faces'
// own forward lit FS (LIT_CAP_* + the probe-only rigged / alpha pipes; the planar mirror keeps
// the full FS). A face is 128 px over 90 degrees -- pixels metres wide, filtered and mipped
// afterwards -- so everything the probe draws could only spend registers on is compiled out:
// probeCapture() / mirrorPass() are CONSTANTS here (clean_draw_common.glsl), which folds the
// probe walk, SSR, the planar mirror tap, AO, the RT shadow mask taps, the distance fade and
// every RenderVKDebugView / RenderVKLitDebugMask term; kDebugViews is a plain false (no debug
// twin: Highlight Transparent tints never show in captures), no glow / reactive outputs (the
// capture attachments have neither), and the ray-query twin traces the sun with ONE hard ray
// (no terminator probe, no disc; clean_shadow_rt.glsl CLEAN_RT_SUN_HARD) and never a projector.
// Local lights: the probe's own list, nearest CAPTURE_LIGHT_CAP only (localLights).
#ifdef CLEAN_CAPTURE
#define CLEAN_DEBUG_CONST
const bool kDebugViews = false;
#define CLEAN_SPEC_SETTINGS
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#define CLEAN_RT_SUN_HARD
#endif
#elif !defined(CLEAN_GBUFFER)
#define CLEAN_DEBUG_CONST
layout(constant_id = 1) const bool kDebugViews = true;
// SETTING SPECIALIZATION (clean_frame_info.glsl, constants 8-17): the lit pipelines of a
// spec set (LLCleanRendererVK, llcleanspecvk.cpp) carry the graphics settings; every other
// pipeline from this module keeps the defaults (-1: the run-time tests, as before).
#define CLEAN_SPEC_SETTINGS
#endif
// The main view's alpha-blend FS (render pass 2) reuses the split lighting's visibility on
// the opaque surfaces its fragments lie on (clean_draw_common.glsl CLEAN_VIS_REUSE).
#ifdef CLEAN_BLEND_TOL
#define CLEAN_VIS_REUSE
#endif
#include "clean_draw_common.glsl"
// G-BUFFER REGISTER PROBES (RenderVKGbufRegProbe, measurement only: pipelines built and
// logged by RenderVKPipelineStats, never drawn): each CLEAN_GBPROBE_* compiles one feature
// out so its register cost can be read. Production builds define none of them.
// G-BUFFER TERRAIN SPLIT (RenderVKGbufTerrainSplit): the terrain branch (instance flag bit5)
// held the G-buffer FS at 112 registers on Pascal for every surface; without it 48. The split
// draws terrain separately (clean_cull.comp TERR_COL list), so each variant is compiled for
// one side: -DCLEAN_GBUFFER_NO_TERRAIN (every non-terrain draw: its VS collapses terrain draws,
// clean_draw.vert CLEAN_VS_SKIP_TERRAIN) and -DCLEAN_GBUFFER_TERRAIN (the terrain list only).
// Each is the full FS with V_TERRAIN / V_NOT_TERRAIN constants, so a surface of its side
// decodes exactly as before (the other builds keep the flag tests, token for token). The
// cluster G-buffer FS is the NO_TERRAIN one too: terrain never takes the cluster path
// (LLCleanClusterSelectVK::kRowNever).
// TERRAIN BAKE (#811, RenderVKTerrainBake): the terrain list is drawn twice -- the live
// terrain FS above (its VS, CLEAN_VS_TERRAIN_SIDE 1, collapses the patches the bake covers;
// it crossfades to the bake in the band) and -DCLEAN_GBUFFER_TERRAIN_BAKE, the slim
// bake-only FS (VS side 2: only the patches wholly past the band with a ready bake).
#if defined(CLEAN_GBPROBE_NO_TERRAIN) || defined(CLEAN_GBUFFER_NO_TERRAIN)
#define V_TERRAIN     false
#define V_NOT_TERRAIN true
#elif defined(CLEAN_GBUFFER_TERRAIN) || defined(CLEAN_GBUFFER_TERRAIN_BAKE)
#define V_TERRAIN     true
#define V_NOT_TERRAIN false
#else
#define V_TERRAIN     ((v_flags & 32u) != 0u)
#define V_NOT_TERRAIN ((v_flags & 32u) == 0u)
#endif
#include "clean_normal_tex.glsl"   // BC5 normal maps: z rebuilt (#263)
#if !defined(CLEAN_CAPTURE) && !defined(CLEAN_GBUFFER)
// (#87) GI for the FORWARD lit surfaces (alpha-blend / -mask faces, forward frames): the GI
// radiance cache's cell at the fragment replaces the probes' diffuse irradiance and occludes their
// specular (clean_surface_light.glsl giForwardRefl). Draw set 0 binding 53 = the GI state.
#define CLEAN_GI_FORWARD
#define GI_STATE_SET 0
#define GI_STATE_BINDING 53
#define GI_STATE_ACCESS readonly
#define GI_STATE_READONLY
#include "clean_gi_common.glsl"
#endif
#ifndef CLEAN_CAPTURE   // (#829: every debug term is gated off in captures anyway)
#define CLEAN_SURFACE_DEBUG   // RenderVKDebugView terms (vertex inputs: forward only)
#endif
#include "clean_surface_light.glsl"
#include "clean_vol_apply.glsl"     // volumetric fog consumer (#718): alpha-blend faces
// (#783 M2: the fragment's instance flags / slot for the moving objects' own snow -- the VS
// writes the instance slot into v_terrain_rec for every non-terrain draw.)
#define WEATHER_INST_FLAGS v_flags
#define WEATHER_INST_ID v_terrain_rec
#ifndef CLEAN_WEATHER_SNOWMESH
// #901 REST-POSE ACCUMULATION: the avatar vertex's rest-pose position + rest normal z (w -2: none;
// every VS paired with this FS writes it -- clean_draw.vert, clean_draw_rigged.vert, the cluster tiers)
layout(location = 15) in vec4 v_wx_rest;
#define WEATHER_REST v_wx_rest
#endif
#include "clean_weather_surface.glsl"   // wet surfaces (#783): weatherSurface()
#include "clean_weather_veil.glsl"      // precipitation veil consumer (#954, fog-off fallback): alpha-blend faces
#ifdef CLEAN_GBUFFER
#include "clean_gbuffer.glsl"
#endif
// <GMDX> rez effects: the de-rez cut (decodeSurface) and the surface paint (shade / G-buffer
// main). Probe captures never paint (a cached cube would keep a half-rezzed object for good).
#define GMDX_REZFX_FS
#ifndef CLEAN_CAPTURE
#define GMDX_REZFX_SURFACE
#endif
#include "gmdx_rezfx.glsl"

// ---- DEPTH PRE-PASS variant (-DCLEAN_EARLY_Z; RenderVKDepthPrepass) ----
// Pass 1 runs AFTER a depth-only pre-pass over the same indirect draws, with depth
// test EQUAL and depth write OFF, so the depth buffer is already final: force the
// depth test ahead of the shader so every hidden fragment is rejected before it pays
// for lighting. This FS never writes gl_FragDepth. Its discards (water / back face /
// alpha cutoff) only drop colour now -- they no longer need to keep depth unwritten.
#ifdef CLEAN_EARLY_Z
layout(early_fragment_tests) in;
#endif

// UV for a non-base map slot: its own transform of the raw texcoord (LL per-map
// transforms), else the base-colour UV. Texture-animated faces: matMapAnimUV (<GMDX>).
vec2 mapUV(int slot)
{
    if ((v_flags & 1024u) != 0u)   // <GMDX> glTF slots: own transform after the animation
        return matMapAnimUV(v_material_id, slot, v_uv, (materials[v_material_id].flags & 64u) != 0u);
    return matMapUV(v_material_id, slot, v_uv_raw, v_uv);
}

#include "clean_terrain_mat.glsl"    // terrain material code (shared with the #811 bake)
#include "clean_terrain_bake.glsl"   // per-region terrain bakes (#811): table + sampling

// <SS:Skoomastream> HUD PBR LIGHTING (SSHudPBRLighting). LL draws glTF HUDs unlit, so
// metal and roughness read flat. HUD faces get their own tiny lighting environment that
// nothing in the world sees: a soft studio "probe" (a vertical gradient, lighter above)
// and one very diffuse lamp from the viewer, a little above. Both are analytic, in HUD
// space; settings arrive in the HUD draw view's u_sun_color (x = on, y = lamp strength,
// z = ambient; the HUD pass never lights with the sun).
bool hudPbrLit() { return pc.u_sun_color.x > 0.5; }

// Studio radiance along a direction whose component along "up" is z: 0.5 + 0.3 z (x the
// ambient). Its cosine-convolved irradiance is 0.5 + 0.2 z (a linear gradient keeps its
// form, the slope scaled by 2/3), and a rough lobe blurs it towards that.
vec3 hudPbrLight(SurfaceLit s)
{
    // Toward the viewer in HUD space: the axis the ortho HUD projection's clip x and y
    // ignore, signed against depth (which grows away from the viewer).
    const mat4 vp = pc.u_view_proj;
    const vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    const vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    const vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 V = normalize(cross(r0, r1));
    if (dot(V, r2) > 0.0) V = -V;
    const vec3 up = normalize(vec3(0.0, 0.0, 1.0) - V * V.z);   // HUD space: +Z is up
    const vec3 N = s.N;
    const float strength = pc.u_sun_color.y, amb = pc.u_sun_color.z;
    const float rough = s.rough, metal = s.metal;

    const vec3 diffuseColor  = s.albedo * (1.0 - 0.04) * (1.0 - metal);
    const vec3 specularColor = mix(vec3(0.04), s.albedo, metal);
    const float NdotV = clamp(abs(dot(N, V)), 0.001, 1.0);

    // probe: split-sum IBL on the analytic gradient
    const float irr = amb * (0.5 + 0.2 * dot(N, up));
    const vec3  Rb  = normalize(mix(reflect(-V, N), N, rough * rough));
    const float rad = amb * (0.5 + 0.3 * mix(1.0, 2.0 / 3.0, rough) * dot(Rb, up));
    vec3 color = (diffuseColor * irr + rad * EnvBRDFApprox(specularColor, rough, NdotV)) * s.ao;

    // lamp: squared half-Lambert (wraps round to the back, never black); its highlight
    // never sharper than roughness 0.6. 0.55 makes a white rough dielectric facing the
    // viewer ~1.0, as bright as the unlit HUD at the default settings.
    const vec3 L = normalize(V + 0.35 * up);
    float w = dot(N, L) * 0.5 + 0.5;
    w *= w;
    float nl; vec3 dP, sP;
    llPunctual(diffuseColor, specularColor, max(rough, 0.6), N, V, L, nl, dP, sP);
    color += strength * 0.55 * (diffuseColor * w + sP * nl * PI);
    return max(color, vec3(0.0));
}
// </SS:Skoomastream>

// Decode this fragment's material into a SurfaceLit (clean_surface_light.glsl): every
// discard of the lit FS, the terrain / PBR / legacy / fullbright material fetches and
// normal mapping -- everything up to (not including) the lighting. Returns false for
// HUD fragments, which are finished: `hud` holds their display-space colour. `alpha` is
// the fragment's coverage (base alpha).
bool decodeSurface(Material m, out SurfaceLit s, out float alpha, out vec4 hud)
{
    s = surfaceInit();
    hud = vec4(0.0);
    // Screen gradients for the terrain branch, taken before any discard / divergent
    // branch (the PBR terrain samples with textureGrad).
    const vec2 t_dxy1 = dFdx(v_uv_raw), t_dxy2 = dFdy(v_uv_raw);
    const vec3 t_dp1  = dFdx(v_world_pos), t_dp2 = dFdy(v_world_pos);
    g_shadow_ng = cross(t_dp1, t_dp2);   // projector shadow map: geometric normal (#441)
    rezfxDerivs(v_flags);                // <GMDX> the Tron wireframe's edge rate, before any discard
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
    g_rt_face_ng = cross(t_dp1, t_dp2);   // the triangle's plane: RT shadow-ray origins
#endif

    // WATER (instance flag bit 11) is drawn by the dedicated water pipeline
    // (clean_water.frag) in render pass 2, where the solid-scene depth is available
    // for depth-fog. This shared FS runs the alpha bucket in the same pass, so it
    // must NOT also draw water — discard those fragments here to avoid double-draw.
    if ((v_flags & 2048u) != 0u) discard;

    // WATER EXCLUSION surfaces (invisiprims, bit 16) only cut the water out, through
    // the exclusion mask (clean_water_excl.frag); they never draw visibly -- except
    // under Highlight Transparent, which shows them green like LL (PASS_INVISIBLE).
    if ((v_flags & 65536u) != 0u && ssr.ao_params2.z <= 0.5) discard;

    // BACK-FACE CULLING (LL GL_CULL_FACE). Opaque/mask pipelines cull in the
    // rasterizer; the alpha-blend pipelines draw both sides (their bucket mixes
    // single- and double-sided faces, and its order matters), so single-sided alpha
    // faces drop their back side here. Double-sided glTF (bit8) keeps both; HUD
    // pipelines cull in the rasterizer.
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u && !rezfxBack(v_flags)) discard;   // <GMDX> but the printer's cut surface (gmdx_rezfx.glsl)

    // Pre-pass variant: `precise` pins the alpha product's evaluation order so the
    // cutoff compare matches clean_prepass_mask.frag bit-for-bit (no speckle of sky
    // along cutout edges from a reassociated multiply).
#ifdef CLEAN_EARLY_Z
    precise
#endif
    vec4 base = m.base_color_factor * v_tint;    // rgb color, a alpha
    vec4 texel = vec4(1.0);                      // sampled base-color texel (LINEAR)
    float a_lod = 0.0;                           // its sampled mip level (#648 maskTestAlpha)
    // PBR terrain: blended surface, consumed by the glTF branch below.
    bool tpbr = false;
    vec3 t_orm = vec3(1.0), t_emis = vec3(0.0), t_N = vec3(0.0, 0.0, 1.0);
    const bool t_rec_ok = v_terrain_rec < uint(terrain_records.length());
    // TERRAIN BAKE (#811, clean_terrain_bake.glsl): the region's baked surface, crossfaded in
    // over [d0, d0 + band] of the main camera's distance; past it the live blend is skipped.
    // The slim G-buffer variant (CLEAN_GBUFFER_TERRAIN_BAKE) only draws patches wholly past
    // d0 + band with a ready bake (its VS collapses every other draw): bake only, no live
    // code. Probe / mirror captures (forward FS, u_sun_color.w 1 / 2) take a ready bake at
    // every distance.
    // REGISTERS: only the weight crosses the live blend -- the bake is fetched AFTER it (its
    // explicit gradients make the late, conditional fetch legal), so its 12 floats are never
    // live across terrainPBR / terrainLegacy. Fetched first, they held the live nohex FS at 87
    // registers on Pascal; after it, 64 -- the same as a twin with the bake compiled out.
    float t_bake = 0.0;
    vec3 tb_col = vec3(0.0), tb_orm = vec3(1.0, 1.0, 0.0), tb_emis = vec3(0.0), tb_N = vec3(0.0, 0.0, 1.0);
    const vec3 t_Ng = gl_FrontFacing ? normalize(v_world_normal) : -normalize(v_world_normal);
    if (V_TERRAIN && t_rec_ok)
    {
#if defined(CLEAN_GBUFFER_TERRAIN_BAKE)
        t_bake = 1.0;
#else
        t_bake = terrBakeWeight(v_terrain_rec, v_world_pos);
#ifndef CLEAN_GBUFFER
        // Probe / mirror captures (u_sun_color.w 1 / 2): a ready bake at every distance --
        // their pixels are metres wide (a 128-256 px probe face over 90 deg: ~0.6-1.2 deg),
        // past any bake's parity distance, and every capture face re-shaded the full blend.
#ifdef CLEAN_CAPTURE
        if (terrBakeReady(v_terrain_rec)) t_bake = 1.0;   // (#829: always a capture)
#else
        if (pc.u_sun_color.w > 0.5 && terrBakeReady(v_terrain_rec)) t_bake = 1.0;
#endif
#elif defined(CLEAN_CAPTURE_GBUF)
        // (#833) The deferred probe capture's G-buffer FS: the capture FS's rule above.
        if (terrBakeReady(v_terrain_rec)) t_bake = 1.0;
#endif
#endif
    }
#if defined(CLEAN_GBUFFER_TERRAIN_BAKE)
    const bool t_live = false;
#else
    const bool t_live = t_bake < 1.0;
#endif
    if (V_TERRAIN && t_rec_ok && terrain_records[v_terrain_rec].misc.y == 1u)
    {
        // PBR TERRAIN (record mode 1). Paint-map regions (misc.w == 1) have no paint
        // map source yet: LL then binds black, which weights slot 1 only.
        vec3 tcol = vec3(0.0);
#if !defined(CLEAN_GBUFFER_TERRAIN_BAKE)
        if (t_live)
        {
            const vec4 w = (terrain_records[v_terrain_rec].misc.w == 1u) ? vec4(1.0, 0.0, 0.0, 0.0) : terrainWeights(v_uv.x);
            terrainPBR(v_terrain_rec, w, v_uv_raw, t_dxy1, t_dxy2, t_dp1, t_dp2, t_Ng, exp2(taaLodBias(v_flags)),
                       tcol, t_orm, t_emis, t_N);
        }
#endif
        if (t_bake > 0.0)
        {
            terrBakeFetch(v_terrain_rec, v_uv_raw, t_dxy1, t_dxy2, t_Ng, exp2(taaLodBias(v_flags)),
                          tb_col, tb_orm, tb_emis, tb_N);
            tcol   = mix(tcol, tb_col, t_bake);
            t_orm  = mix(t_orm, tb_orm, t_bake);
            t_emis = mix(t_emis, tb_emis, t_bake);
            t_N    = t_bake >= 1.0 ? tb_N : normalize(mix(t_N, tb_N, t_bake));
        }
        base = vec4(tcol, 1.0);   // alpha 1: the depth pre-pass treats terrain as opaque
        tpbr = true;
    }
    else if (V_TERRAIN)   // TERRAIN SPLAT: 4 detail textures blended by elevation
    {
        // The 4 detail textures are packed into the material's base/normal/mr/emissive
        // handle slots (detail_0..3, low->high band). v_uv.x carries the per-vertex
        // composition coord (~0..3, region->getCompositionXY). Detail UV = LL's
        // object-plane texgen: region-local XY / RenderTerrainScale + the region's
        // origin offset (record legacy.xyz), continuous across regions. Blend matches
        // LL's alpha_ramp (inverted clamp form). Ported from the GL cleanGPUF flag-32
        // branch.
        vec3 col = vec3(0.0);
#if !defined(CLEAN_GBUFFER_TERRAIN_BAKE)
        if (t_live)
        {
            const vec4 lg = t_rec_ok ? terrain_records[v_terrain_rec].legacy : vec4(0.0, 0.0, 1.0 / 12.0, 0.0);
            vec2 d_uv = v_uv_raw * lg.z + lg.xy;
            // Explicit gradients (the TAA mip bias folded in): hex tiling fetches per cell.
            const float gs = exp2(taaLodBias(v_flags));
            const vec2 dg1 = t_dxy1 * lg.z * gs, dg2 = t_dxy2 * lg.z * gs;
            col = terrainLegacy(m, v_uv.x, d_uv, dg1, dg2);
        }
#endif
        if (t_bake > 0.0)
        {
            terrBakeFetch(v_terrain_rec, v_uv_raw, t_dxy1, t_dxy2, t_Ng, exp2(taaLodBias(v_flags)),
                          tb_col, tb_orm, tb_emis, tb_N);
            col = mix(col, tb_col, t_bake);
        }
        base = vec4(col, 1.0);
    }
    else if ((m.flags & 1u) != 0u)   // has base-color texture
    {
        uint tex = m.base_color_handle.x;
        texel = texture(uTextures[nonuniformEXT(tex)], v_uv, taaLodBias(v_flags));   // same bias as the pre-pass alpha tests
        base *= texel;
        a_lod = textureQueryLod(uTextures[nonuniformEXT(tex)], v_uv).y + taaLodBias(v_flags);   // #648 maskTestAlpha
    }

    // Alpha-mask bucket: discard cutout fragments. cutoff < 0 => not a mask.
    // Pre-pass variant: only MASK instances (flag bit 2 = 4u, the cull's mask buckets)
    // alpha-test. The pre-pass wrote depth for opaque-bucket instances without an
    // alpha test, so an opaque instance that carries a material cutoff must not
    // discard here, or the pixel would show neither it nor what it hid.
    // Highlight Transparent keeps cutouts (the pre-passes skip the same discard).
    const bool hl_alpha = ssr.ao_params2.z > 0.5;
    rezfxDiscard(v_flags);   // <GMDX> the de-rez cut (= the mask pre-pass FSs'), after the implicit-LOD fetch
#ifdef CLEAN_EARLY_Z
    if (!hl_alpha && (v_flags & 4u) != 0u && m.alpha_cutoff >= 0.0 && maskTestAlpha(base.a, a_lod, m.alpha_cutoff) < m.alpha_cutoff)
        discard;
#else
    if (!hl_alpha && m.alpha_cutoff >= 0.0 && maskTestAlpha(base.a, a_lod, m.alpha_cutoff) < m.alpha_cutoff)
        discard;
#endif

    // Alpha-blend: a fully transparent texel adds nothing to the blend -- skip all of
    // the lighting below (shadow rays, clustered lights, probes). Stacked alpha-blend
    // foliage ran the full lit FS ~6 layers deep per pixel, mostly on empty card area.
    // Highlight Transparent still shows them; glowing faces keep running (a 100%
    // transparent prim with glow is a visible halo, #26 -- its glow coverage ignores the
    // face alpha).
    // Shiny legacy faces (specular / env) keep running while the FACE is visible: LL
    // materialF's glare lifts a fully transparent texel's coverage under a highlight
    // (clear glass that shows only its glints, #470; see lightSurface).
    const bool glareCapable = (m.flags & 64u) == 0u && V_NOT_TERRAIN
        && (m.spec_color_gloss.a > 0.0 || ((m.flags & 16u) != 0u && m.emissive_factor.a > 0.0))
        && m.base_color_factor.a * v_tint.a >= (1.0 / 255.0);
    if (!hl_alpha && (v_flags & 2u) != 0u && base.a < (1.0 / 255.0) && ((m.flags >> 8u) & 0xFFu) == 0u
        && !glareCapable)
        discard;

    alpha = base.a;

    // HUD (bit 3): drawn AFTER the tonemap straight onto the swapchain, so it keeps
    // LL's display-space fullbright: stored (sRGB) texel * face colour, no lighting.
    // A HUD material (flags bit16, #532) binds the texture's UNORM view: the texel is
    // already the stored value, FILTERED IN GAMMA SPACE as LL's GL HUD samples it.
    // Through the sRGB view the bilinear / trilinear blend ran in linear light and was
    // re-encoded after: a half-covered edge texel of white-on-black text came out 188
    // instead of 128 -- bolder, foggy text and haloed icons. (A HUD face on another
    // material -- glTF asset -- still re-encodes its sRGB-view sample.)
    // <SS:Skoomastream> glTF HUD faces with SSHudPBRLighting go on to the glTF branch
    // below and are lit there by the HUD studio (hudPbrLight); everything else stays here.
    const bool hudLit = (v_flags & 8u) != 0u && (m.flags & 64u) != 0u && hudPbrLit();
    if ((v_flags & 8u) != 0u && !hudLit)
    {
        const vec3 stored = (m.flags & 65536u) != 0u ? texel.rgb : srgbEncode(texel.rgb);
        hud = vec4(stored * m.base_color_factor.rgb * v_tint.rgb, base.a);
        // <SS:Skoomastream> glTF HUD (Firestorm pbropaqueF forward branch): linear base
        // colour (factor * tint * texel, as the world PBR path) + emissive, then encoded.
        if ((m.flags & 64u) != 0u)
        {
            vec3 e = m.emissive_factor.rgb;
            if ((m.flags & 8u) != 0u)
                e *= texture(uTextures[nonuniformEXT(m.emissive_handle.x)], mapUV(MAP_EMISSIVE), taaLodBias(v_flags)).rgb;
            hud.rgb = srgbEncode(max(base.rgb + e, vec3(0.0)));
        }
        // </SS:Skoomastream>
        return false;
    }

    // Back faces (world pipelines don't cull) light their visible side -- decided by
    // the TRIANGLE's winding (gl_FrontFacing), exactly as LL's shaders do. The old
    // per-fragment dot(N, V) flip inverted smooth-shaded normals that legitimately
    // lean away from the camera near silhouettes (wrinkles, curved surfaces at
    // grazing angles), turning them away from the light: dark patches that swim with
    // the camera. (The GL clean pipeline had and fixed the same bug.)
    const vec3 Ng = gl_FrontFacing ? normalize(v_world_normal) : -normalize(v_world_normal);
    s.Ng = Ng;
    s.N  = Ng;

    // World fullbright (bit 12): LL fullbrightF -- sRGB texel * colour, linearized
    // (lightSurface applies sky_hdr_scale). Haze still applies (surfacePost).
    if ((v_flags & 4096u) != 0u && V_NOT_TERRAIN && !hudLit)   // <SS:Skoomastream> !hudLit
    {
        vec3 c = srgbEncode(texel.rgb) * m.base_color_factor.rgb * v_tint.rgb;
        s.model  = SURF_FULLBRIGHT;
        s.albedo = atmosSrgbToLinear(c);
        // FULLBRIGHT + SHINY: opaque legacy faces only (blend 2u / mask 4u buckets
        // never take this pass in LL).
        s.env = ((m.flags & 16u) != 0u && (v_flags & 6u) == 0u) ? m.emissive_factor.a : 0.0;
        return true;
    }

    const bool terrain = V_TERRAIN;
    // PBR BRDF only for glTF materials (flag bit6). Plain faces (no material) and
    // Blinn-Phong faces shade with LL's legacy diffuse -- routing plain faces through
    // the PBR branch as a rough dielectric added GGX + Fresnel env sheen at grazing
    // angles that LL never draws.
    const bool gltf = (m.flags & 64u) != 0u;
    if (tpbr || (!terrain && gltf))
    {
        // ---- glTF metallic-roughness (PBR terrain arrives pre-blended: t_N / t_orm /
        // t_emis, no per-map samples) ----
        vec3 N = Ng;
        if (tpbr) N = t_N;
        else if ((m.flags & 2u) != 0u && !litDbg(LDBG_NO_MAPS))   // normal map
        {
            vec3 tn = cleanNormalTex(texture(uTextures[nonuniformEXT(m.normal_handle.x)], mapUV(MAP_NORMAL), taaLodBias(v_flags)).xyz);
            N = applyNormalMap(normalize(v_world_normal), v_world_tangent, v_world_pos,
                               ((v_flags & 1024u) != 0u) ? mapUV(MAP_NORMAL) : v_uv, tn, gl_FrontFacing);   // <GMDX> animated: no vertex tangent, frame from the normal map's UV
        }
        float rough = tpbr ? t_orm.g : m.roughness_factor;
        float metal = tpbr ? t_orm.b : m.metallic_factor;
        float ao    = tpbr ? t_orm.r : 1.0;
        if (!tpbr && (m.flags & 4u) != 0u && !litDbg(LDBG_NO_MAPS))   // ORM: R = occlusion, G = roughness, B = metallic
        {
            vec3 mr = texture(uTextures[nonuniformEXT(m.mr_handle.x)], mapUV(MAP_MR), taaLodBias(v_flags)).rgb;
            ao     = mr.r;
            rough *= mr.g;
            metal *= mr.b;
        }
        vec3 emissive = tpbr ? t_emis : m.emissive_factor.rgb;
        if (atmosDropEmissive(v_flags)) emissive = vec3(0.0);   // vintage mode
        else if (!tpbr && (m.flags & 8u) != 0u && !litDbg(LDBG_NO_MAPS))
            emissive *= texture(uTextures[nonuniformEXT(m.emissive_handle.x)], mapUV(MAP_EMISSIVE), taaLodBias(v_flags)).rgb;
        s.model    = SURF_PBR;
        s.albedo   = base.rgb;
        s.N        = N;
        s.rough    = clamp(rough, 0.0, 1.0);
        s.metal    = clamp(metal, 0.0, 1.0);
        s.ao       = ao;
        s.emissive = emissive;
        // <SS:Skoomastream> lit glTF HUD: finished here, in the HUD's display encoding.
        if (hudLit)
        {
            hud = vec4(srgbEncode(hudPbrLight(s) + s.emissive), base.a);
            return false;
        }
        // </SS:Skoomastream>
        return true;
    }

    // ---- legacy / Blinn-Phong: cleanSoftenF legacy branch ----
    // LL's legacy diffuse is the sRGB texel times the (sRGB) face colour, linearized.
    s.model  = SURF_LEGACY;
    s.albedo = terrain ? base.rgb
             : atmosSrgbToLinear(srgbEncode(texel.rgb) * m.base_color_factor.rgb * v_tint.rgb);
    // Legacy material normal map (LL materialF: same TBN as PBR). Terrain reuses the
    // normal slot for a detail texture, so it is excluded.
    // Normal-map alpha scales glossiness (materialF getNormal: glossiness *= vNt.a).
    float nmap_a = 1.0;
    vec3 bump_tn;
    if (!terrain && legacyBumpCode(m.flags) != 0u)
    {
        // Legacy bump preset (#19; LL deferred bumpF): the preset's normal through the
        // same TBN. Glossiness stays the shininess (bumpF writes vertex_color.a).
        if (!litDbg(LDBG_NO_MAPS) && legacyBumpTN(m, v_uv, taaLodBias(v_flags), bump_tn))
            s.N = applyNormalMap(normalize(v_world_normal), v_world_tangent, v_world_pos, v_uv, bump_tn, gl_FrontFacing);
    }
    else if (!terrain && (m.flags & 2u) != 0u && !litDbg(LDBG_NO_MAPS))
    {
        vec4 nt = texture(uTextures[nonuniformEXT(m.normal_handle.x)], mapUV(MAP_NORMAL), taaLodBias(v_flags));
        nmap_a = nt.a;
        vec3 tn = normalize(cleanNormalTex(nt.xyz));
        s.N = applyNormalMap(normalize(v_world_normal), v_world_tangent, v_world_pos, v_uv, tn, gl_FrontFacing);
    }
    // materialF getSpecular: spec = specular map texel (raw, as stored) * specular
    // colour; no map -> (specular colour, 1). softenLightF linearizes spec.rgb.
    // env = env_intensity * spec.a; glossiness = specular exponent * normal-map alpha.
    vec4 spec_tex = vec4(1.0);
    if (!terrain && (m.flags & 4u) != 0u && !litDbg(LDBG_NO_MAPS))
        spec_tex = texture(uTextures[nonuniformEXT(m.mr_handle.x)], mapUV(MAP_MR), taaLodBias(v_flags));
    s.gloss = m.spec_color_gloss.a * nmap_a;
    s.spec  = atmosSrgbToLinear(spec_tex.rgb * m.spec_color_gloss.rgb);
    // Legacy materials carry env intensity in emissive_factor.a (LLCleanMaterialStore).
    s.env = ((m.flags & 16u) != 0u) ? m.emissive_factor.a * spec_tex.a : 0.0;
    if (litDbg(LDBG_NO_LEGACY_ENV)) s.env = 0.0;
    // Alpha-blend legacy faces: LL materialF's glare lifts the coverage under specular
    // highlights / shiny reflections (lightSurface): al = max(texel alpha, glare) x the
    // face (vertex) alpha. Terrain is never alpha-blended.
    if ((v_flags & 2u) != 0u && !terrain) s.glareA = m.base_color_factor.a * v_tint.a;
    // <GMDX> LEGACY EMISSIVE MASK (flag bit7, LL DIFFUSE_ALPHA_MODE_EMISSIVE): materialF
    // getEmissive = the diffuse texel's alpha. Such faces are opaque (getPoolTypeFromTE, no
    // cutoff); one pushed to alpha-blend by its face alpha takes LL's BLEND variant, where
    // getEmissive is emissive_brightness (0) and the alpha is transparency again.
    if ((m.flags & 128u) != 0u && (v_flags & 2u) == 0u && !terrain) s.emask = texel.a;
    return true;
}

// CEL SHADING (#550): the VS resolved scope + per-avatar selection into v_cel bit 16
// (clean_cel_common.glsl); lit models only.
layout(location = 11) flat in uint v_cel;
bool celSurface(SurfaceLit s)
{
    return (v_cel & CEL_V_LIT) != 0u && (s.model == SURF_PBR || s.model == SURF_LEGACY);
}

// Glow factor of this fragment (material flags bits [8,16): LLCleanMaterialStore
// te->getGlow()). PBR glow (LL pbrglowF): only EMISSIVE texels glow -- factor = max
// emissive x te glow. Legacy faces glow over the whole face (LL's legacy glow pass).
// DARK GLOWS LESS: a legacy glowing face's glow strength is multiplied by its base colour's
// brightness (max channel, gamma-encoded via sqrt), so dark texels of a glowing texture (an eye's dark
// pupil on an iris set to glow) barely bloom and white texels keep their full glow. The glow
// target is the SHADED colour x glow (it takes haze, specular and ambient), so a black texel
// still bloomed; LL's own glow pass used the texture colour. PBR faces glow by their emissive
// texture already.
float g_glow_black_k = 1.0;
float glowBlackMask(Material m, SurfaceLit s)
{
    if ((m.flags & 64u) != 0u) return 1.0;
    return sqrt(clamp(max(max(s.albedo.r, s.albedo.g), s.albedo.b), 0.0, 1.0));
}
float materialGlow(Material m)
{
    float glow = float((m.flags >> 8u) & 0xFFu) * (1.0 / 255.0);
    if ((m.flags & 64u) != 0u && V_NOT_TERRAIN && glow > 0.0)
    {
        vec3 e = m.emissive_factor.rgb;
        if ((m.flags & 8u) != 0u)
            e *= texture(uTextures[nonuniformEXT(m.emissive_handle.x)], mapUV(MAP_EMISSIVE), taaLodBias(v_flags)).rgb;
        glow *= max(max(e.r, e.g), e.b);
    }
    // LEGACY EMISSIVE-MASK (LL DIFFUSE_ALPHA_MODE_EMISSIVE, flag bit7): the diffuse alpha is
    // not transparency, it is the emissive mask -- glow = te glow * diffuse.a (#515).
    else if ((m.flags & 128u) != 0u && (m.flags & 1u) != 0u && V_NOT_TERRAIN && glow > 0.0)
        glow *= m.base_color_factor.a
              * texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags)).a;
    return glow;
}

#ifdef CLEAN_GBUFFER
// ---- G-BUFFER variant (RenderVKDeferredLighting): pass 1b's opaque / mask / rigged
// draws, EQUAL after the depth pre-pass (CLEAN_EARLY_Z: the same discards and `precise`
// alpha). Writes the decoded surface (clean_gbuffer.glsl); clean_deferred_light.comp
// lights it. Never HUD / alpha-blend / capture / debug view (CPU-side fallbacks).
// -DCLEAN_CAPTURE_GBUF (#833, RenderVKProbeDeferred): the reflection-probe faces' G-buffer
// (their opaque / mask / rigged draws in the capture's own states, the mask pre-pass then
// EQUAL); clean_probe_defer.comp lights it at probe resolution. Same decode -- the terrain
// takes a ready bake at every distance, as the capture FS does.
layout(location = 1) out vec4 outG1;
layout(location = 2) out vec4 outG2;
layout(location = 3) out vec4 outG3;

void main()
{
    Material m = materials[v_material_id];
    SurfaceLit s;
    float alpha;
    vec4 hud;
    decodeSurface(m, s, alpha, hud);
    weatherSurface(s, v_world_pos);   // (#783: the G-buffer holds the wet surface)
    s.cel = celSurface(s);
    float glow = materialGlow(m);
    const float rezGlow = rezfxSurface(v_flags, v_world_pos, s);   // <GMDX> rez effect paint
    // Glow coverage as the forward path's outGlow alpha (#751): a legacy face's glow is
    // masked by its texture alpha (the alpha-mode-None glow mask -- only the painted
    // lights of a collar glow) and the material alpha. Emissive-mask faces already fold
    // the alpha into materialGlow; PBR faces glow by their emissive texture.
    if ((m.flags & 128u) == 0u && (m.flags & 64u) == 0u && glow > 0.0)
    {
        glow *= m.base_color_factor.a;
        if ((m.flags & 1u) != 0u && V_NOT_TERRAIN)
            glow *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags)).a;
    }
    if (glow > 0.0) glow *= glowBlackMask(m, s);
    glow = max(glow, rezGlow);   // <GMDX>
    gbufferPack(s, glow, (v_flags & 16384u) != 0u, outColor, outG1, outG2, outG3);
}

#else   // forward

// ---- OVERDRAW DIAGNOSTIC (RenderVKOverdrawStats) ----
// Specialization constant: false in every normal pipeline, so the store is dead code
// and the shader has no side effects (a storage write would otherwise force late-Z
// on the non-pre-pass variant). The stats pipelines set it true and count lit-FS
// invocations (non-helper lanes) with one subgroup-aggregated atomic.
layout(constant_id = 0) const bool kOverdrawStats = false;
layout(std430, set = 0, binding = 20) buffer OverdrawCounter { uint od_shaded; };

// Glow target (LL per-face glow): shaded color * glow, blended like color so nearer
// geometry occludes glow behind it. Blurred + composited by clean_glow.comp /
// clean_glow_combine.frag. HUD passes have no glow attachment (write discarded).
layout(location = 1) out vec4 outGlow;
// REACTIVE MASK (F5): render pass 2's third attachment (RGBA8, MAX blend, the alpha
// pipelines write R, and B on no-history faces): this fragment's coverage, which the TAA resolve reads as
// "no motion vector here, trust the history less" (clean_taa.comp). Pass 2b clears
// it before the water, so only the post-water alpha (and the particles) count -- as
// the old reactive reference (the scene re-copied after the water, #204) did. Every
// other pipeline has no attachment 2 (write discarded) or masks it.
// B (#605): the same coverage on a no-history face (live media / animated texture,
// noTemporalFace): the resolve takes this frame only there, in proportion -- the
// blended counterpart of the motion pre-pass's no-history mask for opaque faces.
layout(location = 2) out vec4 outReact;

#ifdef CLEAN_BLEND_TOL
// ---- ALPHA-BLEND DEPTH TOLERANCE (#731; -DCLEAN_BLEND_TOL: the main view's LIT_ALPHA,
// render pass 2 only; rigged alpha keeps the exact hardware test, #742) ----
// "Onion skin" blended layers (a baked-AO decal < 1 mm over a road) cross the surface
// under them by the vertex quantization (#573: int16 lattice per slot, ~1-2 mm on 64 m
// faces, plus the asset's own U16 positions); the jittered main view (TAA / FSR) flipped
// that near-tie pixel by pixel every frame. These pipelines therefore run WITHOUT the
// hardware depth test, and this test is the authority: a blended fragment draws unless
// it lies more than (base + scale x distance) / |N.V| (capped) BEHIND the opaque depth
// along its view ray, measured in metres. The opaque depth is pass 1's final depth (incl.
// the alpha-depth pre-pass's near-opaque blend texels), read-only in pass 2
// (DEPTH_READ_ONLY_OPTIMAL, also the water's depth source), at render size.
//
// COARSE SHADING (RenderVKAlphaShadingRate): one invocation covers fw x fh pixels, and
// the hardware depth test used to keep the occlusion per pixel. The test runs per covered
// pixel and writes gl_SampleMask: with one sample per pixel the coverage index of pixel
// (px, py) of the fragment is fw * fh - 1 - (px + py * fw) (Vulkan spec, "Multisampling":
// coverage index under a fragment shading rate). A device without
// fragmentShadingRateWithShaderSampleMask clamps these draws to 1x1 (spec), which is
// equally correct.
// (#1075 LAYERED STEREO: the main depth has a layer per view -- this view's is CLEAN_VIEW; a
//  2D_ARRAY view at every view count, draw set 0 binding 40 in every shader that names it)
layout(set = 0, binding = 40) uniform sampler2DArray uBlendSceneDepth;

// Discards the fragment when every covered pixel is hidden beyond the tolerance, else
// masks the hidden pixels. Returns true when the tolerance kept a pixel the strict test
// (in front or level) would have hidden (the debug tint). Uniform control flow (derivatives).
bool blendDepthTolerance()
{
    // Fragment size: neighbouring coarse fragments sit a fragment width apart (see the
    // LDBG_VRS_VIEW overlay in main).
    const vec2 fsz = clamp(round(vec2(abs(dFdx(gl_FragCoord.x)), abs(dFdy(gl_FragCoord.y)))), vec2(1.0), vec2(4.0));
    // 1 / view depth: gl_FragCoord.w is 1 / clip w (= view depth in the perspective main
    // view); it and the depth gl_FragCoord.z (no depth bias on these pipelines) are affine
    // in screen space -> per pixel from their derivatives (per fragment width).
    const float iw  = gl_FragCoord.w;
    const vec2  diw = vec2(dFdx(iw), dFdy(iw)) / fsz;
    const vec2  dz  = vec2(dFdx(gl_FragCoord.z), dFdy(gl_FragCoord.z)) / fsz;
    const ivec2 fw  = ivec2(fsz);
    const ivec2 org = ivec2(floor(gl_FragCoord.xy / fsz)) * fw;   // fragments align to their size
    const ivec2 res = textureSize(uBlendSceneDepth, 0).xy;
    // Opaque 1 / view depth: along one pixel's ray 1 / w is affine in the depth, with slope
    // I[2][3] of the inverse view-projection (its w row is translation-free), and the
    // fragment's own (z, 1 / w) lies on that line -- so iwO = iwF + I[2][3] (zO - zF). Its
    // error scales with the GAP, not with the absolute depth (no x / y / constant terms of
    // a float inverse built around a ~km translation). Reversed-Z: 0 = sky, nearer = larger.
    const float kz = pc.u_inv_view_proj[2][3];
    // Metres along this ray per metre of view depth (>= 1), and the tolerance.
    const vec3  toEye = pc.u_cam_time.xyz - v_world_pos;
    const float distF = length(toEye);
    const float k = distF * iw;
    const float nl = length(v_world_normal);
    const float ndv = (nl > 1e-6 && distF > 1e-6) ? abs(dot(v_world_normal / nl, toEye / distF)) : 1.0;
    const float graze = min(1.0 / max(ndv, 1e-4), max(ssr.blend_tol.z, 1.0));
    // Floor: float noise (a tie -- a blend texel the alpha-depth pre-pass wrote, rebuilt
    // per pixel under coarse shading -- must still pass with the tolerance off).
    const float noise = 2e-5 * distF;
    // AVATAR GEOMETRY IS EXACT (#742 / #750): worn rigid attachments (instance bit9) and
    // rigged faces (bit0; their pipelines no longer run this FS) take the noise floor only,
    // i.e. the pre-#731 strict test. Their layers sit millimetres apart on purpose (prim
    // eyes under lids, layered eye / makeup / clothing shells) -- inside a 4 mm x grazing
    // window, so hidden layers drew over the near-opaque ones the alpha-depth pre-pass had
    // made win -- and they are small, so the #573 lattice step the tolerance absorbs (~1-2 mm
    // on 64 m world faces) is microns there. World faces keep it (the #731 road decal).
    const bool exact = (v_flags & (1u | 512u)) != 0u;
    // <GMDX> ORTHOGRAPHIC views (the region map capture): w is 1 everywhere, so the 1 / w line
    // above is flat (kz = 0) and could never find a fragment behind the opaque surface -- every
    // blended face drew over everything (blended floors through roofs). There depth is linear:
    // metres along the (parallel) ray per unit of depth = |column 2 of the inverse view-projection|.
    // The eye is ~1 km away, so the distance term is left out (the base tolerance only).
    const bool ortho = abs(kz) < 1e-12;
    const float ortho_mpz = ortho ? length(vec3(pc.u_inv_view_proj[2].xyz)) : 0.0;
    const float tol = exact ? noise : max((ssr.blend_tol.x + (ortho ? 0.0 : ssr.blend_tol.y * distF)) * graze, noise);
    const int n = fw.x * fw.y;
    uint mask = 0u;
    bool saved = false;
    for (int py = 0; py < fw.y; ++py)
    {
        for (int px = 0; px < fw.x; ++px)
        {
            const ivec2 pix = org + ivec2(px, py);
            const vec2  c   = vec2(pix) + 0.5;
            const vec2  off = c - gl_FragCoord.xy;
            const float iwF = iw + dot(diw, off);
            const float zF  = gl_FragCoord.z + dot(dz, off);
            const float zO  = texelFetch(uBlendSceneDepth, ivec3(clamp(pix, ivec2(0), res - 1), int(CLEAN_VIEW)), 0).r;
            bool keep = true;
            if (ortho)   // <GMDX> (see above): reversed-Z, nearer = larger
            {
                if (zO > 0.0 && zO > zF)
                {
                    const float gap = (zO - zF) * ortho_mpz;
                    keep = gap <= tol;
                    saved = saved || (keep && gap > noise);
                }
            }
            else if (zO > 0.0 && iwF > 0.0)
            {
                const float iwO = iwF + kz * (zO - zF);
                if (iwO > iwF)   // the fragment is farther than the opaque surface
                {
                    const float gap = k * (1.0 / iwF - 1.0 / iwO);   // metres along the ray
                    keep = gap <= tol;
                    saved = saved || (keep && gap > noise);
                }
            }
            if (keep) mask |= 1u << uint(n - 1 - (px + py * fw.x));
        }
    }
    if (mask == 0u) discard;
    gl_SampleMask[0] = int(mask);
    return saved;
}
#endif

float g_rezGlow = 0.0;   // <GMDX> the rez effect's glow factor (shade -> main)
#ifdef CLEAN_WEATHER_SNOWMESH
// #783 M3 SNOW SURFACE MESH, FORWARD (-DCLEAN_WEATHER_SNOWMESH; clean_weather_snowmesh.vert
// -DWX_MESH_FORWARD): frames that do not go deferred light the mesh here, with the same snow as
// its G-buffer FS (clean_weather_snowmesh.frag: the hand-over cut, the relief, fresh snow
// PBR) through the forward lighting -- shadows, probes, ray-traced shadows in the ray-query
// variant -- like every other forward surface. v_uv carries the hand-over band.
// (#906: the relief / sparkle are clean_weather_snow_relief.glsl's, via clean_weather_surface.glsl)
vec4 shade(Material m)
{
    const float wxPx = max(length(dFdx(v_world_pos)), length(dFdy(v_world_pos)));   // (before the discard)
    if (v_uv.y > 0.5 && v_uv.x < 0.98) discard;   // (#783 the hand-over: the ring only past the band; snowmesh.comp kRingCut)
    SurfaceLit s = surfaceInit();
    s.model = SURF_PBR;
    vec3 Ng = normalize(v_world_normal);   // (#783: no back-face flip -- see clean_weather_snowmesh.frag)
    float glint;
    s.Ng = Ng;
    s.N = wxSnowRelief(Ng, v_world_pos.xy, wxPx, glint);
    s.albedo = vec3(0.86, 0.89, 0.93);   // fresh snow (weatherSnowLayer's / the G-buffer FS's)
    s.rough = mix(0.85, 0.3, glint);
    s.metal = 0.0;
    s.ao = 1.0;
    return lightSurface(s, v_world_pos, 1.0);
}
#else
vec4 shade(Material m)
{
    SurfaceLit s;
    float alpha;
    vec4 hud;
    if (!decodeSurface(m, s, alpha, hud)) return hud;
    weatherSurface(s, v_world_pos);   // (#783)
    s.cel = celSurface(s);
    g_glow_black_k = glowBlackMask(m, s);
#ifdef GMDX_REZFX_SURFACE
    g_rezGlow = rezfxSurface(v_flags, v_world_pos, s);   // <GMDX> rez effect paint
#endif
    return lightSurface(s, v_world_pos, alpha);
}
#endif

void main()
{
#ifdef CLEAN_BLEND_TOL
    // First: hidden blended fragments leave before the overdraw count and any shading
    // (they used to fail the early hardware depth test).
    const bool blendSaved = blendDepthTolerance();
#endif
    if (kOverdrawStats && !gl_HelperInvocation)
    {
        const uvec4 lanes = subgroupBallot(true);
        if (subgroupElect()) atomicAdd(od_shaded, subgroupBallotBitCount(lanes));
    }
    // Shading-rate overlay (LDBG_VRS_VIEW): neighbouring coarse fragments sit a fragment
    // width apart, so the gl_FragCoord derivative IS the fragment size (no shading-rate
    // capability needed in this module). Uniform control flow (the switch is uniform).
    vec2 fsz = vec2(1.0);
    if (litDbg(LDBG_VRS_VIEW)) fsz = vec2(abs(dFdx(gl_FragCoord.x)), abs(dFdy(gl_FragCoord.y)));
    Material m = materials[v_material_id];
    outColor = shade(m);
    const bool hud = (v_flags & 8u) != 0u;
    // World fragments are LINEAR HDR (the tonemap pass displays them). Fullbright
    // world faces still take the haze (they skipped the lit branches above).
    if (!hud && litDebugView() == 0)
    {
        const uint model = ((v_flags & 4096u) != 0u && V_NOT_TERRAIN) ? SURF_FULLBRIGHT : SURF_LEGACY;
        outColor.rgb = surfacePost(model, (v_flags & 16384u) != 0u, outColor.rgb, v_world_pos);
    }
#ifndef CLEAN_CAPTURE
    // VOLUMETRIC FOG (#718, clean_vol_apply.glsl): ALPHA-BLENDED world faces (render pass 2)
    // take the froxel fog over their LL-hazed colour, as clean_vol_apply.frag gave the opaque
    // scene behind them. Opaque / mask faces never do here (the fullscreen apply covers them
    // -- forward pass 1 and the deferred lighting alike); probe / mirror captures neither
    // (the grid is the main view's).
    if (!hud && (v_flags & 2u) != 0u && volOn() && !probeCapture() && !mirrorPass() && litDebugView() == 0)
        outColor.rgb = volFogApply(outColor.rgb, v_world_pos);
    // PRECIPITATION VEIL (#954, clean_weather_veil.glsl): likewise over the alpha-blended faces (the
    // fullscreen veil gave the opaque scene behind them its own distance's).
    if (!hud && (v_flags & 2u) != 0u && !probeCapture() && !mirrorPass() && litDebugView() == 0)
        outColor.rgb = weatherVeilApply(outColor.rgb, v_world_pos);
#endif
    // LIT GUARD (clean_draw_common.glsl lgFinite): nothing non-finite or beyond the HDR
    // target's range leaves the lit FS (forward passes, probe / mirror captures).
    outColor.rgb = lgFinite(outColor.rgb, LG_OUT);
    if (!(outColor.a >= 0.0)) outColor.a = 0.0;
#ifdef CLEAN_CAPTURE
    // (#829) The capture attachments are colour + depth only (no glow, no reactive mask):
    // the glow factor's texture fetches and the debug tints below are skipped outright.
    outGlow = vec4(0.0);
    outReact = vec4(0.0);
    return;
#endif
    // DEBUG VIEW 27 (lit guards): the scene's luminance, dimmed, with every guard that
    // tripped at this fragment painted over it: red = a local light's contribution was
    // non-finite / out of range, green = the probe blend, blue = an SSR hit, white = the
    // final colour, magenta = a light / probe list entry past this frame's count. Clean
    // frames show grey only. (Forward: debug views take the forward pass.)
    if (!hud && litDebugView() == 27)
    {
        const float lum = clamp(dot(outColor.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        vec3 c = vec3(0.15 + 0.35 * sqrt(lum));
        if ((g_lgBits & (1u << LG_LOCAL)) != 0u) c = vec3(1.0, 0.1, 0.1);
        if ((g_lgBits & (1u << LG_PROBE)) != 0u) c = mix(c, vec3(0.1, 1.0, 0.1), 0.85);
        if ((g_lgBits & (1u << LG_SSR)) != 0u)   c = mix(c, vec3(0.2, 0.3, 1.0), 0.85);
        if ((g_lgBits & (1u << LG_OUT)) != 0u)   c = vec3(1.0);
        if ((g_lgBits & ((1u << LG_PROBE_IDX) | (1u << LG_LIGHT_IDX))) != 0u) c = vec3(1.0, 0.0, 1.0);
        outColor.rgb = c;
    }
    // DEBUG VIEW 19 (RenderVKTextureCompression, #263): how the base-colour map is stored
    // (Material.base_color_handle.y, patched per frame while the view is on), over the
    // surface's brightness: grey RGBA8, red BC1, orange BC3, yellow BC4, blue BC5, green
    // BC7, magenta one texel (constant colour); Adaptive's BC1 / BC3: cyan = chosen by the
    // content rule (measured good), pink = at a reduced residency level; brown = RGBA8
    // below RenderVKTexCompressMinDim (uncompressed by design); dark = no base texture.
    if (!hud && litDebugView() == 19)
    {
        const vec3 kFmt[10] = vec3[10](vec3(0.6), vec3(1.0, 0.15, 0.1), vec3(1.0, 0.55, 0.0), vec3(1.0, 0.95, 0.1),
                                       vec3(0.15, 0.4, 1.0), vec3(0.1, 0.9, 0.2), vec3(1.0, 0.1, 0.9),
                                       vec3(0.1, 0.9, 0.95), vec3(1.0, 0.55, 0.7), vec3(0.55, 0.33, 0.15));
        const float lum = sqrt(clamp(dot(outColor.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.02, 1.0));
        outColor.rgb = (m.flags & 1u) != 0u ? kFmt[min(m.base_color_handle.y, 9u)] * (0.35 + 0.65 * lum)
                                            : vec3(0.08) * lum;
    }
    // DEBUG VIEWS 23 / 24 (#685, texture mip level): the selected map's (RenderVKDebugMipMap)
    // residency code, written into Material.base_color_handle.y per frame while the view is on
    // (LLCleanRendererVK::fillDbgMipCodes): bits 0-3 resident levels below full, 4-7 the
    // requested (demand + budget bias) level, 8-11 the demand-only level, 12 small asset,
    // 13 own-slot image (media), 14 constant 1x1, 15 pinned, 31 valid. Over the surface's
    // brightness like view 19; dark = no texture.
    //   23 resident vs full: green full, yellow -1, orange -2, red -3 or more; blue = a small
    //      asset (<= RenderVKDebugMipSmallDim) at its full size; magenta constant colour (1x1);
    //      grey media / own-slot image.
    //   24 requested vs resident: green = the request is met (demand alone); purple = met but
    //      the budget / pressure bias lowered the request; yellow / orange / red = resident 1 /
    //      2 / 3+ levels below the request (streaming backlog); pale blue = resident above the
    //      request (downgrade pending); magenta / grey as 23.
    {
        const int dv = litDebugView();
        if (!hud && (dv == 23 || dv == 24))
        {
            const uint c = m.base_color_handle.y;
            const float lum = sqrt(clamp(dot(outColor.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.02, 1.0));
            const vec3 kLvl[4] = vec3[4](vec3(0.1, 0.9, 0.2), vec3(1.0, 0.95, 0.1), vec3(1.0, 0.5, 0.0), vec3(1.0, 0.1, 0.08));
            vec3 col = vec3(0.6);
            if ((c & 0x80000000u) == 0u) col = vec3(-1.0);
            else if ((c & 0x2000u) != 0u) col = vec3(0.6);
            else if ((c & 0x4000u) != 0u) col = vec3(1.0, 0.1, 0.9);
            else
            {
                const uint rd = c & 15u, rq = (c >> 4) & 15u, dm = (c >> 8) & 15u;
                if (dv == 23)
                    col = (rd == 0u && (c & 0x1000u) != 0u) ? vec3(0.1, 0.55, 1.0) : kLvl[min(rd, 3u)];
                else if (rd > rq) col = kLvl[min(rd - rq, 3u)];
                else if (rd < rq) col = vec3(0.6, 0.8, 1.0);
                else col = rq > dm ? vec3(0.65, 0.25, 1.0) : kLvl[0];
            }
            outColor.rgb = col.r < 0.0 ? vec3(0.08) * lum : col * (0.35 + 0.65 * lum);
        }
    }
    // Glow factor: the glow target gets the amount; the colour comes from the final
    // (post-tonemap) image at extract time, as LL's glowExtract.
    float glow = materialGlow(m);
    // Highlight Transparent (Build > Options, Ctrl+Alt+T; LL sShowDebugAlpha): every
    // alpha-blend or alpha-mask face shows red, fully transparent ones included --
    // the way builders find invisible prims. Water exclusion surfaces (invisiprims,
    // bit 16) show green instead (LL's PASS_INVISIBLE highlight colour).
    // (The tint is debug-twin only: kDebugViews. The cutout / invisiprim discards above keep
    // reading the flag in every pipeline, so a frame bound to a normal pipeline -- the twin
    // not built yet -- still covers exactly what the pre-passes kept.)
    const bool hl_tint = kDebugViews && ssr.ao_params2.z > 0.5 && (v_flags & 8u) == 0u;
    if (hl_tint && (v_flags & 65536u) != 0u)
        outColor = vec4(mix(outColor.rgb, vec3(0.0, 1.0, 0.0), 0.65), max(outColor.a, 0.45));
    else if (hl_tint && (v_flags & 6u) != 0u)
        outColor = vec4(mix(outColor.rgb, vec3(1.0, 0.0, 0.0), 0.65), max(outColor.a, 0.45));
    if (!hud && (fsz.x > 1.5 || fsz.y > 1.5))
        outColor.rgb = mix(outColor.rgb, (fsz.x > 1.5 && fsz.y > 1.5) ? vec3(0.1, 0.3, 1.0)
                                       : (fsz.x > 1.5 ? vec3(1.0, 0.15, 0.1) : vec3(0.1, 1.0, 0.15)), 0.45);
#ifdef CLEAN_BLEND_TOL
    // RenderVKBlendDepthTolDebug: magenta = a pixel of this fragment lay behind the opaque
    // surface and drew only because of the tolerance.
    if (kDebugViews && blendSaved && ssr.blend_tol.w > 0.5)
        outColor = vec4(mix(outColor.rgb, vec3(1.0, 0.0, 1.0), 0.7), max(outColor.a, 0.6));
#endif
    // Glow coverage: the fragment's alpha, EXCEPT legacy faces take it from the texture
    // (and material) alone, not the face transparency -- LL's legacy glow pass ignores
    // the TE alpha, so a 100%-transparent prim with glow is a visible halo (#26); with
    // the face alpha here the blended glow write was multiplied by zero.
    float glow_a = outColor.a;
    float glow_out = glow;
    if ((m.flags & 128u) != 0u && (m.flags & 64u) == 0u)
        glow_a = 1.0;   // emissive mask: alpha already folded into the glow factor (#515)
    else if ((m.flags & 64u) == 0u && glow > 0.0)   // <SS:Skoomastream> HUDs too (SSHudGlow)
    {
        glow_a = m.base_color_factor.a;
        if ((m.flags & 1u) != 0u && V_NOT_TERRAIN)
            glow_a *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags)).a;
    }
    // <SS:Skoomastream> Alpha-BLENDED glTF (Firestorm lldrawpoolalpha renderPbrEmissives):
    // the face's blend scales the glow under it by (1 - a), then the PBR glow pass ADDS
    // its amount unscaled (blendFunc ZERO, ONE, ONE, ONE). Our attachment blends
    // src * a + dst * (1 - a), so writing amount / a at coverage a gives exactly
    // dst * (1 - a) + amount. Coverage floored at 1/255 (a ~0 face: dst * 254/255).
    else if ((m.flags & 64u) != 0u && (v_flags & 2u) != 0u && glow > 0.0)   // (HUDs too)
    {
        glow_a = max(glow_a, 1.0 / 255.0);
        glow_out = glow / glow_a;
    }
    // </SS:Skoomastream>
    if (!hud) glow_out *= g_glow_black_k;   // (upstream: legacy glow scales with base-colour brightness)
    if (g_rezGlow > glow_out) { glow_out = g_rezGlow; glow_a = 1.0; }   // <GMDX> rez effect glow
    // <SS:Skoomastream> LL parity: the glow target carries the glow AMOUNT only (LL's
    // emissive pass writes texel.a * glow into screen alpha); the bloom extract takes the
    // colour from the composited scene, as LL's glowExtract takes it from the final pixel.
    // Using the face's own colour made faint glowing alpha (light-shaft beams, glass) glow
    // several times brighter than the blended pixel actually is.
    outGlow = vec4(vec3(glow_out), glow_a);
    // </SS:Skoomastream>
    // <SS:Skoomastream> HUD glow (SSHudGlow): the HUD glow pipelines' attachment takes the
    // halo colour itself -- the face's display colour x amount, what LL's glowExtract
    // would read off the screen -- since nothing tone-maps the HUD layer to read it from.
    // No glow attachment (the plain HUD pipes): the write is discarded.
    if (hud) outGlow = vec4(outColor.rgb * glow_out, glow_a);
    // </SS:Skoomastream>
    const float cov = clamp(outColor.a, 0.0, 1.0);
    outReact = vec4(cov, 0.0, noTemporalFace(v_flags) ? cov : 0.0, 0.0);
}
#endif   // CLEAN_GBUFFER
