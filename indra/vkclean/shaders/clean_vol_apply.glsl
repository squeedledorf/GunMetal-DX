// vkclean -- VOLUMETRIC FOG (#718): the CONSUMER side. Every main-view surface after LL's
// haze takes C' = C_LLhazed * T + L from the integrated froxel volume (draw set 0 binding 45,
// clean_vol_integrate.comp; a 1 x 1 x 1 identity stub while the fog is off), plus the
// analytic height fog past the grid's far plane:
//   * opaque surfaces + sky: clean_vol_apply.frag, one fullscreen draw at the start of
//     render pass 2 (after the deferred lighting / forward pass 1, before any transparent);
//   * alpha-blended world faces: clean_draw.frag (blend flag), after surfacePost;
//   * water surface: clean_water.frag (its refraction sample is first un-fogged: the solid
//     scene behind it already took the air segment in front of the surface);
//   * "over" particles: clean_particle.frag, after their haze.
// Probe / mirror captures never apply it (the grid is the main view's).
//
// WHY ON TOP OF LL'S HAZE (not replacing it): LL's haze is srgb_to_linear(additive x
// (1 - atten)) x hdr -- not a per-metre integral, so it cannot be split into froxels without
// changing its look. Keeping it exact and adding only what LL lacks (shadowed sun shafts in
// the haze, the extra fog, local volumes) keeps every region's sky exactly as its EEP draws
// it: with the extra fog density 0 and the haze sun scatter 0 the frame is identical to
// fog-off outside the local volumes.
//
// Requires clean_frame_info.glsl + clean_atmos_common.glsl (included before this).
#ifndef CLEAN_VOL_APPLY_GLSL
#define CLEAN_VOL_APPLY_GLSL

#include "clean_vol_common.glsl"

layout(set = 0, binding = 45) uniform sampler3D uVolFog;   // rgb in-scatter, a extra-medium T

// (in-scatter, transmittance) between the grid's eye and world point P.
vec4 volFogAt(vec3 P)
{
    const vec3 rel = P - ssr.vol_eye.xyz;
    const vec4 clip = ssr.vol_vp * vec4(rel, 1.0);
    const float vz = clip.w;
    if (!(vz > 1e-4)) return vec4(0.0, 0.0, 0.0, 1.0);
    const vec2 uv = clamp(clip.xy / vz * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    const float D = ssr.vol_grid.z;
    const float far = volFar();
    // Stored index i = the integral up to slice i's FAR boundary: depth vz sits at the
    // continuous index c = u(vz) D - 1 (texel centre (c + 0.5) / D). In front of the first
    // boundary, blend from the identity (nothing integrated at the eye).
    const float c = volSliceOfDepth(min(vz, far)) * D - 1.0;
    vec4 s = textureLod(uVolFog, vec3(uv, (max(c, 0.0) + 0.5) / D), 0.0);
    if (c < 0.0) s = mix(vec4(0.0, 0.0, 0.0, 1.0), s, clamp(c + 1.0, 0.0, 1.0));
    // Past the far plane: the height fog analytically (unshadowed, no local lights / volumes),
    // seen through the grid's transmittance and the haze over [eye, far].
    if (vz > far && ssr.vol_albedo.w > 0.5)
    {
        const float f = far / vz;
        const vec3 A = ssr.vol_eye.xyz + rel * f;
        const float Tf = exp(-volFogOpticalDepth(A, P));
        const float len = length(rel);
        const vec3 V = rel / max(len, 1e-6);
        const float Th = exp(-volHazeExt() * len * f);
        s.rgb += s.a * Th * volFogSource(V) * (1.0 - Tf);
        s.a *= Tf;
    }
    // #954 PRECIPITATION past the far plane (clean_weather_veil.glsl: the froxels carry it inside
    // the grid): uniform, open, below its 1.5 km ceiling, lit as the fog's medium (unshadowed).
    const float sp = ssr.weather[16].x;
    if (vz > far && sp > 0.0)
    {
        const float len = length(rel);
        const vec3 V = rel / max(len, 1e-6);
        float seg = len * (1.0 - far / vz);
        if (V.z > 0.0) seg = min(seg, max(1500.0 / V.z - len * far / vz, 0.0));
        const float Tp = exp(-sp * seg);
        const vec3 L = atmosLightDir();
        vec3 src = volAmbient();
        if (L.z > 0.0) src += ATMOS_PI * volBeam() * volPhaseHG(dot(V, L), 0.3);
        s.rgb += s.a * src * mix(0.85, 0.95, ssr.weather[16].z) * (1.0 - Tp);
        s.a *= Tp;
    }
    return s;
}

vec3 volFogApply(vec3 C, vec3 P)
{
    if (!volOn()) return C;
    const vec4 f = volFogAt(P);
    return C * f.a + f.rgb;
}

// The inverse (water refraction: the solid scene behind the surface already took the
// air segment in front of it -- the extra fog is zero below the water, so P's (L, T) is
// what the scene sample got, up to the refraction's screen offset).
vec3 volFogRemove(vec3 C, vec3 P)
{
    if (!volOn()) return C;
    const vec4 f = volFogAt(P);
    return max(C - f.rgb, vec3(0.0)) / max(f.a, 1e-3);
}

#endif
