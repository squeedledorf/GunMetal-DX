// Temporal lighting histories use elapsed milliseconds. Setting values and local
// working lengths are durations in units of the 60 Hz reference interval, not samples.
// Reservoir M, ray counts, parity and sampling phases remain actual sample/frame counts.
#ifndef CLEAN_HISTORY_TIME_GLSL
#define CLEAN_HISTORY_TIME_GLSL
const float HT_REF_MS = 1000.0 / 60.0;
float htDeltaMs() { return clamp(ssr.history_time.x, 0.1, 100.0); }
float htStep() { return htDeltaMs() / HT_REF_MS; }
// tau = -dt_ref / log(1 - alpha_ref). This exactly preserves alpha_ref at 60 Hz.
// alpha_ref == 1 is an instantaneous reset (tau == 0), at every cadence.
float htAlpha(float alphaRef)
{
    if (alphaRef >= 1.0) return 1.0;
    return 1.0 - exp(log(max(1.0 - alphaRef, 1e-8)) * htStep());
}
float htAdvance(float age, float cap) { return min(age + htStep(), cap); }
// A motion cap is specified using this frame's displacement. Convert it to a
// reference-interval displacement before deriving a time horizon.
float htMotion(float stepPerFrame) { return stepPerFrame / htStep(); }
#endif
