// vkclean — shared weights of the DEPTH-AWARE 4-TAP UPSAMPLES that read a reduced-resolution
// buffer at full resolution: the RT shadow mask (clean_shadow_mask_read.glsl rtMaskTaps) and
// the half-resolution ambient occlusion (clean_surface_light.glsl aoSample). Each caller picks
// its 4 nearest low-res texels and measures, per tap, how far that texel's surface is from
// this pixel's (its own geometry test, in units of its own tolerance); the weighting and the
// fallback are this one function.
// (The split lighting's half-res reflections, clean_deferred_light.comp dsHalfUpsample, keep
// their own loop: separate irradiance / specular sums with a roughness term and a score-based
// fallback per sum.)

#ifndef CLEAN_UPSAMPLE_COMMON_GLSL
#define CLEAN_UPSAMPLE_COMMON_GLSL

// bw: the taps' bilinear weights; e: each tap's surface mismatch in tolerance units (0 = the
// same surface, >= 1 = another one; 1e30 = no data); extra: further per-tap weights (1 = none).
// Weight = bilinear x (1 - e)^2 x extra, normalized. When every weight vanishes (all four taps
// belong to other surfaces) the tap with the smallest mismatch takes it all -- the closest
// depth, never a blend across the silhouette. Ties: the lowest index.
vec4 upsampleWeights4(vec4 bw, vec4 e, vec4 extra)
{
    const vec4 wz = max(vec4(0.0), 1.0 - e);
    const vec4 w = bw * wz * wz * extra;
    const float sum = w.x + w.y + w.z + w.w;
    if (sum < 1e-4)
    {
        int bi = 0;
        float best = e.x;
        if (e.y < best) { best = e.y; bi = 1; }
        if (e.z < best) { best = e.z; bi = 2; }
        if (e.w < best) { best = e.w; bi = 3; }
        return vec4(bi == 0 ? 1.0 : 0.0, bi == 1 ? 1.0 : 0.0, bi == 2 ? 1.0 : 0.0, bi == 3 ? 1.0 : 0.0);
    }
    return w / sum;
}

#endif
