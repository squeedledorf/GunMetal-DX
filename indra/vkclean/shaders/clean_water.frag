#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
// (demote, not discard, for the per-pixel exclusion test: the glint's specular AA takes
// screen-space derivatives, undefined in a quad with a terminated lane -- #204)
#extension GL_EXT_demote_to_helper_invocation : require
// vkclean — WATER surface (port of LL's waterF / the GL clean cleanWaterF), world space.
//
// Drawn in render PASS 2 over the solid scene. Inputs LL's water uses:
//   * two water NORMAL MAPS (environment normal map + the day cycle's next one, blended),
//     sampled as three scrolling wave layers (one big, two little) -> wave normal;
//   * REFRACTION: the solid-scene HDR colour behind the surface (copied after pass 1),
//     sampled at a wave-distorted screen position, undistorted where the distorted
//     sample would land on something ABOVE the water (LL's refPos test), and fogged by
//     LL's water fog over the in-water path to the bottom (waterFogF / cleanWaterHaze);
//   * REFLECTION: environment radiance (reflection probes, else the WindLight sky),
//     scaled by LL's Fresnel terms, + SSR for the glossy mirror;
//   * SUN GLINT: LL's GGX punctual (metallic 1, roughness = blurMultiplier*2) with the
//     water "specular" light colour;
//   * SHORELINE FADE toward the refraction where the water is shallow.
// LL's tangent frame for the water plane is world-aligned (T = +X, B = +Y, N = +Z), so
// its tangent-space waves are used directly as world normals here.
//
// OPAQUE water (LL's non-TRANSPARENT_WATER waterF / underWaterF): no refraction -- the
// water behind the surface is the fully fogged water colour (fog over a 2048 m path),
// blend off. Taken at runtime in the main view when RenderTransparentWater is off
// (ssr.ao_params2.w), and always by the WATER_OPAQUE build: the reflection-probe
// and planar-mirror captures (#36), which have no refraction source, exclusion mask or
// SSR of their own. That build references no set 2, so it runs on mDrawLayout.
#define CLEAN_SPEC_SETTINGS   // graphics-setting specialization constants 8-17 (clean_frame_info.glsl)
#include "clean_draw_common.glsl"
#include "clean_vol_apply.glsl"   // volumetric fog consumer (#718)
#include "clean_weather_veil.glsl"   // precipitation veil consumer (#954)

#ifndef WATER_OPAQUE
// TAA WATER MASK (#204, F2): G of render pass 2's third attachment (the water pipe's
// write mask is G only) = 1 where the water surface draws -- depth-tested against the
// solid scene, and not where an invisiprim excludes it (the demote below drops the
// write). It replaced a separate pass that drew the whole post-water alpha list into
// the exclusion target's G channel just to find the water.
layout(location = 2) out vec4 outWaterMask;
layout(set = 2, binding = 0) uniform sampler2D uSceneDepth;    // solid-scene depth (pass 1)
layout(set = 2, binding = 1) uniform sampler2D uSceneColor;    // solid-scene HDR colour (pass 1)
// Water exclusion mask (clean_water_excl.frag, #27): 1 = water, < 1 = an invisiprim
// covers it -- no water surface there (LL waterF: `if (mask < 1) discard`).
layout(set = 2, binding = 2) uniform sampler2D uWaterExcl;
#endif

vec3 waterNormalTex(int idx, vec2 uv)
{
    if (idx < 0) return vec3(0.0, 0.0, 1.0);
    return texture(uTextures[nonuniformEXT(uint(idx))], uv).xyz * 2.0 - 1.0;
}

#ifndef WATER_OPAQUE
// World position of the solid scene at screen uv (VK depth is [0,1]).
vec3 scenePos(vec2 uv)
{
    float d = textureLod(uSceneDepth, uv, 0.0).r;
    vec4 wp = pc.u_inv_view_proj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return wp.xyz / wp.w;
}
#endif

void main()
{
    if ((v_flags & 2048u) == 0u) discard;    // water only

    const vec3  P    = v_world_pos;
    const vec3  eye  = pc.u_cam_time.xyz;
#ifdef WATER_OPAQUE
    // Capture views (probe cube face / reflected mirror camera): their own eye decides
    // which side of the surface they see; the frame's camUnderwater() is the main one.
    const bool opaque = true;
    const bool under  = eye.z < waterHeight();
#else
    if (texelFetch(uWaterExcl, ivec2(gl_FragCoord.xy), 0).r < 1.0) demote;    // excluded (invisiprim)
    outWaterMask = vec4(0.0, 1.0, 0.0, 0.0);   // (every path below; helpers write nothing)
    const bool opaque = ssr.ao_params2.w > 0.5;   // RenderTransparentWater off
    const bool under  = camUnderwater();
#endif
    const float time = pc.u_cam_time.w * 0.5;          // LL phase_time = elapsed * 0.5
    const vec3  up   = vec3(0.0, 0.0, 1.0);

    vec3  view    = P - eye;                            // LL oEyeVec
    float dist    = length(view);
    vec3  viewVec = view / max(dist, 1e-4);

    // ---- wave coordinates (cleanWaterV) ----
    vec2 wave1d = ssr.water[0].xy, wave2d = ssr.water[0].zw;
    vec3 v = P;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    vec2 bigWave = v.xy * vec2(0.04, 0.04) + wave1d * time * 0.055;
    vec4 littleWave;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + wave2d * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2)  + wave1d * time * 0.1;

    // ---- wave normals: two normal maps, three layers, blended by the day cycle ----
    int   ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    float blend = ssr.water[1].w;
    vec3 wave1 = mix(waterNormalTex(ia, bigWave),       waterNormalTex(ib, bigWave),       blend);
    vec3 wave2 = mix(waterNormalTex(ia, littleWave.xy), waterNormalTex(ib, littleWave.xy), blend);
    vec3 wave3 = mix(waterNormalTex(ia, littleWave.zw), waterNormalTex(ib, littleWave.zw), blend);
    vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    // ---- UNDER THE SURFACE (LL underWaterF / cleanUnderWaterF): the scene above,
    // refracted through the waves (scale-below distortion), fogged over the path from
    // the eye up to the surface. ----
    if (under)
    {
        // Opaque: LL's fb = waterFogColorLinear, then the same eye-to-surface fog.
        vec3 above = atmosSrgbToLinear(ssr.water[4].rgb);
#ifndef WATER_OPAQUE
        if (!opaque)
        {
            vec2 res_u = vec2(textureSize(uSceneDepth, 0));
            vec2 uv = gl_FragCoord.xy / res_u;
            vec3 wn = normalize(wave1 + wave2 + wave3);
            uv = clamp(uv + wn.xy * ssr.water[2].w, vec2(0.0), vec2(0.999));
            above = textureLod(uSceneColor, uv, 0.0).rgb;
        }
#endif
        outColor = vec4(max(applyWaterFog(above, view, eye.z - waterHeight()), vec3(0.0)), 1.0);
        return;
    }

    // ---- Fresnel factors (calculateFresnelFactors) ----
    float fscale = ssr.water[2].x, foffset = ssr.water[2].y;
    vec3 df3 = max(vec3(0.0), vec3(dot(viewVec, wave1),
                                   dot(viewVec, (wave2 + wave3) * 0.5),
                                   dot(viewVec, wave3)) * fscale + foffset);
    df3 *= df3;
    vec2 df2 = max(vec2(0.0), vec2(df3.x + df3.y + df3.z,
                                   dot(viewVec, wavef) * fscale + foffset));

    // ---- lighting terms at the surface ----
    vec3 sunlit, amblit, additive, atten;
    atmosLightTerms(view, normalize(wavef), sunlit, amblit, additive, atten);

    float vdu = clamp(-dot(viewVec, up) * 2.0, 0.0, 1.0);
    // <SS:Skoomastream> LL waterF: normalize(vec3(wavef.xy * normScale.xy, wavef.z * 2.0)) --
    // normScale.z is not applied (it scaled z 4x and flattened the reflection normals).
    vec3 wave_ibl = normalize(vec3(wavef.xy * ssr.water[1].xy, wavef.z * 2.0));
    vec3 norm = normalize(wavef);

    // ---- behind the surface ----
    // Opaque (LL non-TRANSPARENT_WATER): white fogged over a 2048 m in-water path, i.e.
    // the saturated water fog colour; no shoreline fade.
    vec3  fb   = applyWaterFog(vec3(1.0), viewVec * 2048.0, eye.z - waterHeight());
    float fade = 1.0;
#ifndef WATER_OPAQUE
    if (!opaque)
    {
        // ---- refraction (TRANSPARENT_WATER + SHORELINE_FADE) ----
        vec2 res     = vec2(textureSize(uSceneDepth, 0));
        vec2 distort = gl_FragCoord.xy / res;
        float dmod   = sqrt(dist);
        vec3 waver   = wavef * 3.0;
        vec2 distort2 = distort + waver.xy * ssr.water[2].w / max(dmod, 1.0) * 2.0;   // refScale
        distort2 = clamp(distort2, vec2(0.0), vec2(0.999));

        vec3 refPos = scenePos(distort);
        fade = clamp((P.z - refPos.z) / 10.0, 0.0, 1.0);
        distort2 = mix(distort, distort2, min(1.0, fade * 10.0));
        vec3 refPos2 = scenePos(distort2);
        if (P.z < refPos2.z - 0.05) { distort2 = distort; refPos2 = refPos; }   // sample above water
        // <GMDX> ...or on a much shallower bottom than the one under this pixel: the waves dragged
        // bright shallow sand out over deep water in jagged patches (real refraction shifts with
        // the depth: over deep water it shows deep water).
        else if (P.z - refPos2.z < 0.5 * (P.z - refPos.z) - 0.25) { distort2 = distort; refPos2 = refPos; }
        fb = textureLod(uSceneColor, distort2, 0.0).rgb;
        // VOLUMETRIC FOG (#718): the solid scene was fogged over the air in front of the
        // water by clean_vol_apply.frag; the surface applies it again below -- take it back
        // out of the refraction first (no extra fog under the water: the same segment).
        if (volOn() && !probeCapture() && !mirrorPass()) fb = volFogRemove(fb, P);
        if (!probeCapture() && !mirrorPass()) fb = weatherVeilRemove(fb, P);   // (#954, as the fog)
        // Water fog over the in-water path (surface -> bottom); the sky/far plane behind
        // the water becomes a very long path -> fully fogged.
        fb = applyWaterFog(fb, refPos2 - eye, eye.z - waterHeight());
    }
#endif

    // ---- reflection: probes / sky, + SSR for the glossy mirror ----
    float rough = clamp(ssr.water[2].z, 0.0, 1.0);      // perceptualRoughness = blurMultiplier*2
    vec3 R = reflect(viewVec, wave_ibl);
    R.z = abs(R.z);                                     // reflections come from above
    vec3 radiance, irradiance;
    gProbeSkipAuto = true;    // <SS:Skoomastream/> LL sampleProbesWater: manual probes + probe 0 only
    if (!sampleProbes(P, wave_ibl, R, rough * phdr.info.y, amblit, radiance, irradiance))
        radiance = skyColor(R);
    gProbeSkipAuto = false;
#if !defined(WATER_OPAQUE) && defined(CLEAN_RT_FWD_AVAILABLE)
    // (#859) the ray-traced reflection of this water surface (compute, half resolution) where it
    // has one: in place of the probe / sky, under SSR as everywhere else
    {
        // <GMDX> a flat mirror image, displaced by the waves as the refraction is
        const vec2 roff = wavef.xy * 3.0 * ssr.water[2].w / max(sqrt(dist), 1.0) * 2.0
                        * vec2(textureSize(uSceneDepth, 0));
        const vec4 ri = rtFwdInWater(P, roff);
        radiance = mix(radiance, ri.rgb, ri.a);
    }
#endif
#ifndef WATER_OPAQUE   // SSR = the main view's previous frame: never in a capture
    if (specSSROn() && (1.0 - rough) >= 0.9)   // <SS:Skoomastream/> LL: SSR only on glossy water
    {
        vec4 s = ssrTap(P, wave_ibl, 1.0 - rough);
        radiance = mix(radiance, s.rgb, s.a);
    }
#endif

    // ---- sun glint (LL pbrPunctual, metallic 1) ----
    vec3 specularColor = atmosSrgbToLinear(ssr.water[3].rgb);   // metallic: spec = base
    vec3 V = -viewVec;
    vec3 glintN = normalize(wavef + up * max(dist, 32.0) / 32.0 * (1.0 - vdu));
    // SPECULAR ANTI-ALIASING (#204; Kaplanyan / Tokuyoshi kernel roughness): the wave
    // normal varies faster than a pixel toward the horizon, so a near-mirror GGX lobe
    // turned single pixels on and off with every TAA jitter phase (glint sparkle that
    // never settles). Widen the lobe by the normal's screen-space variance, capped.
    float glintRough = rough;
    {
        const vec3 dnx = dFdx(glintN), dny = dFdy(glintN);
        const float kvar = min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.18);
        const float a = max(rough, 8.0 / 255.0);
        glintRough = sqrt(sqrt(min(a * a * a * a + kvar, 1.0)));   // alpha^2 = rough^4
    }
    float nl; vec3 diffPunc, specPunc;
    llPunctual(vec3(0.0), specularColor, glintRough, glintN, V, atmosLightDir(), nl, diffPunc, specPunc);
    vec3 punctual = clamp(nl * (diffPunc + specPunc), vec3(0.0), vec3(10.0)) * sunlit * atten.r
                  // (ray-traced sun, #185: the cascades aren't rendered then -- the
                  // glint goes unshadowed rather than reading a stale map; the volumetric
                  // cloud shadow, #719, still applies)
                  * (specSunTraced() ? cloudShadowMap(P) : sunShadow(P, up));

    radiance *= df2.y;
    vec3 color = mix(fb, radiance, min(1.0, df2.x)) + punctual;

    // Shoreline: blend back to the refraction in the shallows.
    color = mix(fb, color, min(1.0, fade * 60.0));

    // Distance haze + far-clip fade, as the solid scene gets. (#816: the haze of this same
    // ray, view = P - eye, came with the light terms above: no second / third scatter.)
    color = atmosApplyRay(color, additive, atten);
    // The far-clip fade is a main-view thing (clean_draw.frag skips it in probe cubes).
    if (!probeCapture())
        color = atmosDistanceFade(color, P, eye, linfo.cam_fwd.xyz, linfo.zinfo.y, additive, atten);
    // Volumetric fog (#718, clean_vol_apply.glsl) over the hazed surface; main view only.
    if (volOn() && !probeCapture() && !mirrorPass()) color = volFogApply(color, P);
    // Precipitation veil (#954, clean_weather_veil.glsl) over it; main view only.
    if (!probeCapture() && !mirrorPass()) color = weatherVeilApply(color, P);
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
