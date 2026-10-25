#ifndef CLEAN_NORMAL_TEX_GLSL
#define CLEAN_NORMAL_TEX_GLSL
// Tangent-space normal from a sampled normal-map texel (rgb in 0..1). Alpha-less normal
// maps may be stored BC5 (two channels, RenderVKTextureCompression #263), which sample
// blue = 0 -- impossible for a real tangent-space normal (z = -1 points into the surface)
// -- so z is rebuilt from x and y. Uncompressed / BC7 / BC1 maps pass through unchanged.
vec3 cleanNormalTex(vec3 t)
{
    vec3 n = t * 2.0 - 1.0;
    if (t.z < 0.5 / 255.0) n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// LEGACY BUMP PRESETS (#19): Material.flags bits [17,22) = the TE bumpmap code on a legacy
// face without a normal map of its own. 1 brightness / 2 darkness derive the bump from the
// face's diffuse; 3..17 (std_bump.ini: woodgrain .. weave) are heightfields with the
// height in alpha (RGB black), loaded into the normal slot. LL turned both into a normal
// map once (normgenF for 1/2, LLBumpImageList::generateNormalMapFromAlpha for 3..17);
// here the same 4-tap stencil runs per fragment on the sampled source, so no generated
// texture exists or has to be kept alive.
#ifdef CLEAN_GBPROBE_NO_BUMP
uint legacyBumpCode(uint mat_flags) { return 0u; }   // (G-buffer register probe, measurement only)
#else
uint legacyBumpCode(uint mat_flags) { return (mat_flags >> 17u) & 31u; }
#endif

// normgenF getBumpValue: luminance of the (display-encoded, as LL's GL textures were
// stored) diffuse, inverted for darkness.
float legacyBumpLum(vec3 srgb, uint code)
{
    const float l = dot(srgb, vec3(0.2995, 0.5875, 0.1145));
    return code == 2u ? 1.0 - l : l;
}

// Both LL generators sum the cross products of the four neighbour edge vectors
// (+-norm_scale, 0, dh) / (0, +-norm_scale, dh); that sum reduces exactly to
// (2s (h[-x] - h[+x]), 2s (h[-y] - h[+y]), 4 s^2). Heights scale by 512 in normgenF
// (h in 0..1) and by 255 in generateNormalMapFromAlpha (h in bytes); norm_scale is
// RenderNormalMapScale's default 64. -x / -y = one texel toward smaller u / v.
vec3 legacyBumpNormal(float hxm, float hxp, float hym, float hyp, uint code)
{
    const float k = code <= 2u ? 512.0 : 255.0;
    return normalize(vec3(k * (hxm - hxp), k * (hym - hyp), 2.0 * 64.0));
}
#endif
