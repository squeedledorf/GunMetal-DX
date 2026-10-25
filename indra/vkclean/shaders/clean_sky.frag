#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// vkclean — sky full-screen pass (fragment).
//
// The WindLight/EEP sky (cleanSkyFullF port: haze integral per pixel, rainbow/halo,
// sun + moon billboards, stars) by WORLD view direction, un-projected through the camera's inverse
// view-projection. Linear HDR into the scene target; the tonemap pass displays it.
#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];   // bindless (cloud noise, sky textures)
#include "clean_clouds.glsl"
#include "clean_vol_cloud_sky.glsl"
#include "clean_sky_bodies.glsl"

layout(location = 0) in vec2 v_ndc;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    vec4 u_horizon;        // 0  : (unused; legacy gradient colours) -- .w (#719): 1 = this view
                           //      composites the volumetric clouds itself (main view), 0 = the
                           //      panorama (mirror); recordWorld / recordMirror always write it
    vec4 u_zenith;         // 16
    mat4 u_inv_view_proj;  // 32 : VK clip -> world (u_cam.w 1) / camera-relative world (u_cam.w 2)
    vec4 u_cam;            // 96 : .xyz camera world pos, .w 1 = valid, 2 = valid + relative inverse
} pc;

// ---- Aurora (upstream Alchemy auroraF.glsl, per sky direction) -------------------
// Upstream draws it on the WL dome mesh; here it is evaluated per pixel of the
// full-screen sky pass by view direction (Y-up, as the dome's local space).
#define aurora_intensity ssr.sky_fx.x
#define aurora_time      ssr.sky_fx.y
// 3D value noise
float auroraHash31(vec3 p)
{
    p  = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float valueNoise3D(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);

    float n000 = auroraHash31(i + vec3(0.0, 0.0, 0.0));
    float n100 = auroraHash31(i + vec3(1.0, 0.0, 0.0));
    float n010 = auroraHash31(i + vec3(0.0, 1.0, 0.0));
    float n110 = auroraHash31(i + vec3(1.0, 1.0, 0.0));
    float n001 = auroraHash31(i + vec3(0.0, 0.0, 1.0));
    float n101 = auroraHash31(i + vec3(1.0, 0.0, 1.0));
    float n011 = auroraHash31(i + vec3(0.0, 1.0, 1.0));
    float n111 = auroraHash31(i + vec3(1.0, 1.0, 1.0));

    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y),
               u.z);
}

vec3 auroraColor(vec3 dir)
{
    float elev = dir.y;

    vec2  horiz = dir.xz;
    float hlen  = length(horiz);
    if (hlen < 1e-3) return vec3(0.0);
    vec2  az = horiz / hlen;

    // Keep a generous performance cap but make sure the visible fade itself is
    // driven by the smooth envelopes below — a tight discard near the visible
    // edge produces a hard line where bloom amplifies sub-threshold values.
    if (elev < -0.05) return vec3(0.0);
    if (elev > 0.90) return vec3(0.0);

    float t = aurora_time;

    // ----- Multi-cluster coverage -----------------------------------------
    // Two-scale noise gives several active sectors distributed around the
    // sky with natural gaps between them, so the aurora reads as a
    // multi-direction display rather than a single sheet stuck on one
    // side. Slow temporal drift reshapes the clusters over minutes.
    float cov_big = valueNoise3D(vec3(az * 1.0, t * 0.03));
    float cov_med = valueNoise3D(vec3(az * 2.8, t * 0.07));
    // Wider smoothstep so the sub-threshold tail fades smoothly instead of
    // cutting off at a cov=0.01 edge. Let the final strength discard handle
    // the "no contribution" case.
    float cov     = smoothstep(0.22, 0.90, cov_big * 0.62 + cov_med * 0.38);

    // ----- Base arc with traveling wave folds -----------------------------
    // The arc's elevation undulates as a sum of waves moving in opposite
    // directions; interference between them produces the slow, organic
    // curtain-folding motion seen in real displays rather than a clean
    // periodic wobble. Amplitudes kept modest so the arc sits low on the
    // horizon rather than bowing up into the middle of the sky.
    float fold = 0.06 * sin(az.x * 2.0 + t * 0.18)
               + 0.04 * sin(az.y * 3.1 + t * 0.27 + 1.4)
               + 0.03 * sin(az.x * 4.3 - t * 0.33)
               + 0.03 * valueNoise3D(vec3(az * 2.5, t * 0.11));
    float base = 0.10 + fold;

    float d_above = elev - base;
    if (d_above > 0.55) return vec3(0.0);   // no below-base discard — let it fade naturally

    // ----- Soft vertical streaks ------------------------------------------
    // Strongly anisotropic fBm: azimuth sampled at ~8x the frequency of
    // elevation, so noise cells stretch vertically into soft streaks with
    // gently varying widths — neither sharp filaments nor isotropic
    // blobs. Each octave carries its own lateral drift and temporal
    // morph rate so the pattern never settles into a visible period.
    vec2 drift = vec2(t * 0.045, t * 0.028);
    vec3 p     = vec3(az * 16.0 + drift, elev * 2.0);

    float n  = 0.50 * valueNoise3D(p             + vec3(0.0, 0.0, t * 0.09));
    n       += 0.28 * valueNoise3D(p * 2.1       + vec3(drift,       t * 0.14));
    n       += 0.14 * valueNoise3D(p * 4.5       + vec3(drift * 1.4, t * 0.22));
    n       += 0.08 * valueNoise3D(p * 9.5       + vec3(drift * 1.9, t * 0.33));

    // Soft smoothstep instead of clamp+pow so the brightest curtain peaks
    // don't plateau at 1.0 — that plateau becomes a visible "ceiling" where
    // the elevation fade then scales it down, producing the hard top edge
    // the user was seeing.
    float curtain_mask = pow(smoothstep(0.05, 0.92, n), 1.6);

    // ----- Elevation envelope --------------------------------------------
    // A single smooth factor for the horizon and zenith transitions,
    // applied uniformly across body/halo/base_line. The ranges extend
    // slightly past the visible regions so both ends ease out below the
    // perceptual threshold rather than ending on a smoothstep knee.
    float horizon_fade = smoothstep(-0.03, 0.28, elev);
    float zenith_fade  = smoothstep(0.88, 0.38, elev);
    float elev_env     = horizon_fade * zenith_fade;

    // ----- Body envelope --------------------------------------------------
    // Below-base fade rate softened further (was 7) so the green trails
    // gradually toward the horizon with no visible edge.
    float body_top = 0.30;
    float body_env = exp(min(d_above, 0.0) * 3.5)
                   * smoothstep(body_top, body_top * 0.15, d_above);
    body_env *= elev_env;

    // ----- Top halo -------------------------------------------------------
    // Diffuse pink/purple cap above the body. Overlaps the body top so
    // the colour transition reads as a smooth gradient along each fold
    // rather than a hard seam.
    float halo_lo  = 0.18;
    float halo_hi  = 0.45;
    float halo_env = smoothstep(halo_lo * 0.40, halo_lo,        d_above)
                   * smoothstep(halo_hi * 1.15, halo_hi * 0.55, d_above);
    halo_env *= elev_env;

    // Halo modulation is a softened version of the curtain mask — some
    // structure is preserved so bright folds extend their colour up into
    // the halo, but contrast is reduced so the halo feels diffuse.
    float halo_mask = 0.5 + 0.5 * sqrt(curtain_mask);

    // ----- Base line glow -------------------------------------------------
    // Concentrated bright green at the 100km O layer. Narrow in elevation
    // but modulated along azimuth so it isn't a painted stripe. Lower
    // smoothstep extended so the line has a gentle skirt rather than a
    // hard lower cut-off.
    float base_line = exp(-pow((d_above - 0.02) * 20.0, 2.0));
    base_line *= smoothstep(-0.22, -0.02, d_above);
    base_line *= 0.45 + 0.55 * valueNoise3D(vec3(az * 6.0, t * 0.10));
    base_line *= elev_env;

    // ----- Animation layers -----------------------------------------------
    // Three independent time scales: a slow global breath with an
    // azimuth-dependent phase offset (different sky sectors brighten at
    // different moments), a medium pulse, and a fast shimmer sampled
    // with time-evolving noise. Together they give a rich, non-periodic
    // motion without any single animation reading as a repeating loop.
    float breath_phase = valueNoise3D(vec3(az * 1.2, 0.0)) * 6.28318;
    float breath  = 0.80 + 0.20 * sin(t * 0.18 + breath_phase);
    float pulse   = 0.88 + 0.12 * sin(t * 0.65);
    float shimmer = 0.88 + 0.12 * valueNoise3D(vec3(az * 22.0, t * 1.4));

    // ----- Palette --------------------------------------------------------
    vec3 col_green   = vec3(0.30, 1.50, 0.60);
    vec3 col_magenta = vec3(1.15, 0.50, 0.95);
    vec3 col_purple  = vec3(0.60, 0.35, 1.10);
    vec3 col_baseln  = vec3(0.50, 1.80, 0.80);

    float halo_t   = clamp((d_above - halo_lo) / (halo_hi - halo_lo), 0.0, 1.0);
    vec3  halo_col = mix(col_magenta, col_purple, smoothstep(0.35, 1.00, halo_t));

    // ----- Composition ----------------------------------------------------
    vec3 body_rgb = col_green   * body_env * curtain_mask;
    vec3 halo_rgb = halo_col    * halo_env * halo_mask * 0.60;
    vec3 base_rgb = col_baseln  * base_line;

    float strength = body_env * curtain_mask
                   + halo_env * halo_mask * 0.40
                   + base_line;
    strength *= cov * breath * pulse * shimmer;
    if (strength <= 0.003) return vec3(0.0);

    vec3 aurora_rgb = (body_rgb + halo_rgb + base_rgb)
                    * cov * breath * pulse * shimmer * aurora_intensity;

    // Per-fold chromatic bias — subtle warmer/cooler variation between
    // neighbouring sectors, matching per-column variability in O vs N2
    // emission ratios.
    float chroma_n = valueNoise3D(vec3(az * 11.0, 0.0));
    aurora_rgb *= mix(vec3(0.95, 1.05, 0.98),
                      vec3(1.05, 0.98, 1.02), chroma_n);

    // Upstream writes aurora_rgb * 1.5 to the emissive target with an additive
    // blend (BT_ADD), so it adds straight onto the sky.
    return aurora_rgb * 1.5;
}
#undef aurora_intensity
#undef aurora_time

// ---- Meteors (upstream Alchemy meteorsV/F.glsl, analytic per sky direction) -------
// Upstream expands each meteor into a screen-aligned quad (half-width 1.5 px x the
// meteor's width scale) along the head-tail great circle; here the same profile is
// evaluated in angle space: along = tail..head position, across = angular distance
// from the great circle converted to pixels.
vec3 meteorColor(vec3 d)
{
    vec3 sum = vec3(0.0);
    const int count = int(ssr.sky_fx.z + 0.5);
    const float rad_px = max(ssr.sky_fx.w, 1e-6);
    for (int i = 0; i < count; ++i)
    {
        vec4 hw = ssr.meteors[3 * i];
        vec3 H = hw.xyz, T = ssr.meteors[3 * i + 1].xyz;
        vec3 M = normalize(H + T);
        float half_seg = 0.5 * acos(clamp(dot(H, T), -1.0, 1.0));
        float width_scale = max(hw.w, 1e-3);
        float half_w = 1.5 * width_scale * rad_px;             // quad half-width (rad)
        if (dot(d, M) < cos(half_seg + half_w + 1e-4)) continue; // outside the quad's cone
        vec3 n = cross(T, H);
        float nl = length(n);
        if (nl < 1e-7) continue;
        n /= nl;
        float across_ang = asin(clamp(dot(d, n), -1.0, 1.0));
        float y_norm = across_ang / half_w;                       // upstream v / width_scale
        if (abs(y_norm) > 1.0) continue;
        vec3 e2 = normalize(cross(n, M));                         // toward the head
        if (dot(e2, H) < 0.0) e2 = -e2;
        vec3 p = d - n * dot(d, n);
        float u = atan(dot(p, e2), dot(p, M)) / max(half_seg, 1e-6);
        if (abs(u) > 1.0) continue;
        float along_t = clamp((u + 1.0) * 0.5, 0.0, 1.0);
        float along = along_t + pow(along_t, 6.0) * 1.2;
        float across = exp(-y_norm * y_norm * 6.0);
        float shape = along * across;
        if (shape < 0.003) continue;
        sum += ssr.meteors[3 * i + 2].rgb * shape * 4.0;          // upstream emissive: color * 4
    }
    return sum;
}

void main()
{
    vec3 dir = vec3(1.0, 0.0, 0.0);
    if (pc.u_cam.w > 0.5)
    {
        // #555: the main / mirror views push a CAMERA-RELATIVE inverse (w = 2), so no eye
        // is subtracted. Un-projecting through the world inverse at depth 0.5 (~2 near
        // planes from the eye) and subtracting a km-sized eye cost 2-7 px of direction at
        // Z 3000-8000 -- stars jumping from frame to frame.
        vec4 p = pc.u_inv_view_proj * vec4(v_ndc, 0.5, 1.0);
        dir = normalize(p.xyz / p.w - (pc.u_cam.w > 1.5 ? vec3(0.0) : pc.u_cam.xyz));
    }
    // LL's sky gbuffer order: haze + rainbow/halo, then the sun, moon and stars, then
    // the cloud dome over them; then resolve to linear HDR.
    vec3 c = skyResolve(cloudComposite(skyBodiesComposite(skySceneryPre(dir), dir), dir));
    // WEATHER (#783 M2, LLCleanWeatherVK; zeros without weather): cloud and rain darken the sky a
    // little (ssr.weather[9].w); a lightning flash brightens it, most toward the strike.
    if (ssr.weather[9].w > 0.0) c *= 1.0 - ssr.weather[9].w;
    if (ssr.weather[8].w > 0.0)
    {
        const float toward = max(dot(dir, ssr.weather[9].xyz), 0.0);
        c += ssr.weather[8].rgb * (0.25 + 0.75 * toward * toward * toward) * 0.6;
    }
    // Volumetric clouds (#719): the main view marches + composites them after pass 1; the
    // mirror's sky takes the panorama (no-op unless the layer is live).
    if (pc.u_horizon.w < 0.5) c = volCloudSkyApply(c, dir);
    // Night-sky effects (Lightbox Sky tab). The CPU zeroes them by day (star brightness
    // gate) and in snapshots of reflection probes, as upstream skips them in cube maps.
    if (pc.u_cam.w > 0.5)
    {
        // Same HDR sky scale the sky itself carries (skyColor), so the night
        // effects keep their brightness relative to the sky.
        if (ssr.sky_fx.x > 0.0) c += auroraColor(atmosToYUp(dir)) * atmosHdrScale();
        if (ssr.sky_fx.z > 0.5) c += meteorColor(dir) * atmosHdrScale();
    }
    // <GMDX> Above the water, below the horizon: open sea to the horizon (horizonWater).
    if (!camUnderwater() && pc.u_cam.w > 0.5 && pc.u_cam.z > waterHeight())
    {
        float w = horizonWaterAmount(dir);
        if (w > 0.0) c = mix(c, horizonWater(dir, pc.u_cam.z - waterHeight()), w);
    }
    // Underwater: open-water directions with no geometry are water, not sky -- LL's
    // water-haze pass fogs these far pixels fully. (Upward directions are covered by
    // the water surface's underside draw.)
    if (camUnderwater() && pc.u_cam.w > 0.5)
        c = applyWaterFog(c, dir * 4096.0, pc.u_cam.z - waterHeight());
    outColor = vec4(c, 1.0);
}
