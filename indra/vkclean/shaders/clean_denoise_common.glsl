// vkclean — TEMPORAL DENOISER HELPERS shared by the ray-traced shadow mask
// (clean_rt_mask_temporal.comp / clean_rt_mask_spatial.comp) and the god rays' denoiser
// (#657: clean_godrays_dn_temporal.comp / clean_godrays_atrous.comp). Push-block and
// binding agnostic: pure functions on a signal in [0, 1] (shadow visibility; the god rays'
// normalized beam visibility -- clean_godrays.comp's denoiser output).
// Changing anything here changes the RT shadow mask: keep its output bit-identical.

// Clamp to [lo, hi] + adaptive blend. len_prev: frames already accumulated (0 = no
// history); cap: this texel's longest history (historyCap).
vec3 accumulate(vec3 raw, vec3 lo, vec3 hi, vec3 hist, float len_prev, float cap, out float len_new)
{
    len_new = 1.0;
    if (len_prev < 0.5) return raw;
    const vec3 hc = clamp(hist, lo, hi);
    const vec3 dv = abs(hc - hist);
    const float diff = max(max(dv.r, dv.g), dv.b);
    len_new = min(len_prev + 1.0, cap);
    // ANTI-LAG: the more the clamp had to move the history, the shorter it gets, so the
    // following frames converge on the new state instead of crawling there.
    if (diff > 0.02) len_new = min(len_new, mix(8.0, 1.0, clamp((diff - 0.02) * 4.0, 0.0, 1.0)));
    return mix(hc, raw, 1.0 / len_new);
}

float lum3(vec3 v) { return dot(v, vec3(1.0 / 3.0)); }

// SVGF mode: as accumulate, against a LUMINANCE box [lo, hi]; the clamp shifts every
// channel by the same amount (a translucent tint keeps its colour). cut: how far the
// history had to move (the debug view).
vec3 accumulateLum(vec3 raw, float lo, float hi, vec3 hist, float len_prev, float cap,
                   out float len_new, out float cut)
{
    len_new = 1.0;
    cut = 0.0;
    if (len_prev < 0.5) return raw;
    const float hl = lum3(hist);
    const float d = clamp(hl, lo, hi) - hl;
    const vec3 hc = clamp(hist + d, vec3(0.0), vec3(1.0));
    cut = abs(d);
    len_new = min(len_prev + 1.0, cap);
    if (cut > 0.02) len_new = min(len_new, mix(8.0, 1.0, clamp((cut - 0.02) * 4.0, 0.0, 1.0)));
    return mix(hc, raw, 1.0 / len_new);
}

// MOVING-CASTER CHANGE DETECTOR: 0 = nothing beyond the noise, 1 = the signal moved here.
// x: this frame's centre sample, xm: its 3x3 mean, ref: the reference (fast mean / the
// history), tau: the noise band. Both must leave the band: the centre alone fires on a
// noisy penumbra sample, the mean alone on a lit texel next to a still shadow edge.
float changeDetect(float x, float xm, float ref, float tau)
{
    const float d = min(abs(x - ref), abs(xm - ref));
    return clamp((d - tau) / 0.15, 0.0, 1.0);
}

// A-trous (Dammertz 2010) B3-spline taps (the spatial passes; the temporal ones define
// DENOISE_NO_ATROUS, which keeps their SPIR-V free of the unused constant array).
#ifndef DENOISE_NO_ATROUS
const float kB3[5] = float[5](1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
#endif
