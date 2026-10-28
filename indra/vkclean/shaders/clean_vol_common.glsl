// vkclean -- VOLUMETRIC FOG (#718): shared definitions of the froxel passes
// (clean_vol_inject / _light / _temporal / _integrate .comp) and their consumers
// (clean_vol_apply.glsl). Requires clean_frame_info.glsl (ssr) and clean_atmos_common.glsl.
//
// THE GRID. A camera-aligned frustum grid ("froxels", Wronski 2014) of vol_grid.x x .y
// screen tiles (the main view's UNJITTERED projection, uniform in screen uv) x vol_grid.z
// depth slices over view depth [0, far] (far = vol_grid.w). Slice boundaries are
// exponential: depth(u) = n (exp(u ln(1 + far / n)) - 1), u in [0, 1] -- slices thin near
// the eye (n = vol_slice.x controls how thin: ~13 cm for the first of 64 at n = 2 m,
// ~8.5 m for the last at far = 128 m), so near fog / volume edges get the resolution and the
// far ones (seen small) do not waste it. View depth = clip.w of vol_vp, the main view's
// depth along its forward axis -- the one definition every pass and consumer uses.
//
// WHAT THE GRID HOLDS (the composition contract, see clean_vol_apply.glsl):
//   * the EXTRA medium: a height fog (EEP-derived default, vol_fog) + the local volume
//     primitives (clean_vol_inject.comp) + later sources (cloud layer hook, particles):
//     scattering + extinction, lit by sun / moon (cascade-shadowed, x cloudShadow), sky
//     ambient and the local lights;
//   * the HAZE's sun single scattering ("god rays", vol_slice.z): the WindLight haze stays
//     LL's analytic haze on every surface (atmosApplyHaze: untouched), the grid adds only
//     its shadowed sun shafts, exactly the god rays pass's term (clean_godrays.comp), which
//     the renderer does not dispatch while the fog runs -- one sun single-scatter, never two.
//   The integrated volume stores, per cell, the in-scatter L reaching the eye from the
//   segment [eye, the cell's far boundary] and the EXTRA medium's transmittance T over it
//   (the haze's own transmittance is LL's atten.r, already on the surface colour).
//   Every consumer: C' = C_LLhazed * T + L.
#ifndef CLEAN_VOL_COMMON_GLSL
#define CLEAN_VOL_COMMON_GLSL

bool  volOn()       { return ssr.vol_eye.w > 0.5; }
float volFar()      { return ssr.vol_grid.w; }
uvec3 volDims()     { return uvec3(ssr.vol_grid.xyz + 0.5); }

// Slice coordinate u in [0, 1] <-> view depth (m).
float volDepthOfSlice(float u) { return ssr.vol_slice.x * (exp(u * ssr.vol_slice.y) - 1.0); }
float volSliceOfDepth(float vz) { return log(1.0 + max(vz, 0.0) / ssr.vol_slice.x) / ssr.vol_slice.y; }

// Henyey-Greenstein phase; cos_t = dot(view direction (eye -> point), direction TO the light).
float volPhaseHG(float cos_t, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOS_PI * pow(max(1.0 + g2 - 2.0 * g * cos_t, 1e-4), 1.5));
}

// The sun / moon beam (linear): the haze light colour through the atmosphere above,
// as clean_godrays.comp beamColor() (atmos_k[0] is the same extinction, #816).
vec3 volBeam() { return atmosSrgbToLinear(ssr.atmos_k[0].rgb) * ssr.atmos[12].z; }

// Sky ambient radiance (linear), as atmosLightTerms' amblit with the normal factor
// averaged out (a medium has no normal), x the fog's ambient scale.
vec3 volAmbient()
{
    vec3 a = ssr.atmos_k[4].rgb * 0.9;   // (atmosAmbientLighting's mean over the sphere ~0.9)
    if (!atmosClassic())
    {
        a = atmosSrgbToLinear(a);
        a = vec3(dot(a, vec3(0.2126, 0.7152, 0.0722)));
    }
    return a * ssr.atmos[12].w * ssr.vol_fog.w
         * (atmosClassic() ? gmdxNightAmbientSrgb() : gmdxNightAmbient());   // <GMDX> the night's ambient
}

// The WindLight haze's extinction (/m), LL's atten.r rate (atmos_k[1].r); 0 with the
// atmosphere off.
float volHazeExt() { return atmosEnabled() ? ssr.atmos_k[1].r : 0.0; }

// The extra height fog's extinction at world position P (/m): sigma0 at and below the
// base height, decaying exp(-k (z - base)) above it; none below the water cut (the water
// fog owns everything under the surface).
float volFogDensity(vec3 P)
{
    if (P.z < ssr.vol_misc.z) return 0.0;
    return ssr.vol_fog.x * exp(-ssr.vol_fog.y * max(P.z - ssr.vol_fog.z, 0.0));
}

// Optical depth of the height fog along the segment A -> B (analytic: the exponential
// integral; the segment is clipped to the water cut first, and the base-height clamp is
// applied at the two ends -- exact for segments wholly above the base, a close bound
// for the rest).
float volFogOpticalDepth(vec3 A, vec3 B)
{
    const float s0 = ssr.vol_fog.x;
    if (s0 <= 0.0) return 0.0;
    const float k = ssr.vol_fog.y, base = ssr.vol_fog.z, cut = ssr.vol_misc.z;
    float za = A.z, zb = B.z;
    float len = length(B - A);
    if (za < cut && zb < cut) return 0.0;
    if (za < cut || zb < cut)
    {
        const float f = (cut - za) / (zb - za);   // (za != zb here)
        if (za < cut) { len *= 1.0 - f; za = cut; } else { len *= f; zb = cut; }
    }
    const float ea = max(za - base, 0.0), eb = max(zb - base, 0.0);
    const float x = k * (eb - ea);
    const float avg = abs(x) < 1e-4 ? exp(-k * ea) * (1.0 - 0.5 * x)
                                    : (exp(-k * ea) - exp(-k * eb)) / x;
    return s0 * len * avg;
}

// The extra fog's UNSHADOWED source radiance per unit extinction along view direction V:
// albedo x (pi x beam x phase (sun above the horizon) + ambient). pi: LL's light colours are
// irradiance / pi (a Lambertian surface shows albedo x colour x N.L), so a medium scatters
// sigma_s x pi x colour x phase -- the same light the surfaces get, in the same units.
vec3 volFogSource(vec3 V)
{
    const vec3 L = atmosLightDir();
    vec3 sun = vec3(0.0);
    if (L.z > 0.0) sun = ATMOS_PI * volBeam() * volPhaseHG(dot(V, L), ssr.vol_slice.w);
    return ssr.vol_albedo.rgb * (sun + volAmbient());
}

#endif
