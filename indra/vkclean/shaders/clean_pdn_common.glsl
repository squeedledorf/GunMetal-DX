// vkclean — DIFFUSE / SPECULAR LIGHTING DENOISER (#85 phase 2): the shared contract between a
// PRODUCER (a pass that writes a noisy lighting signal), the DENOISER (clean_dn_temporal.comp,
// clean_dn_atrous.comp; LLCleanDenoiserVK) and a CONSUMER (the lit pass that remodulates the
// result). Pure functions + constants: no bindings, no push block (each includer declares its own).
//
// THE SIGNAL (per denoiser texel; the denoiser runs at the render extent x a scale, 1 or 1/2):
//   * DIFFUSE: demodulated irradiance-like radiance, rgb >= 0 -- the producer divides its
//     diffuse term by pdnDiffFactor(albedo) (linear base / PBR diffuseColor), so texture detail
//     never enters the filter and the consumer multiplies it back at full resolution.
//   * SPECULAR: demodulated radiance -- divided by pdnSpecFactor(F0) (PBR specularColor, legacy
//     specular colour); Fresnel's angular part, D, G stay inside the signal.
//   * GUIDES: view depth (32-bit), its plane gradient (d(vz) per denoiser texel), the shading
//     normal, the specular roughness, the specular hit / light distance, the camera-only
//     previous-frame position of the surface and of its VIRTUAL (reflected) image, the expected
//     previous view depth, and the SIGNAL REFERENCE W: the producer's noise-free estimate of the
//     signal's scale (e.g. the unshadowed target sum of a light sampler) -- when it changes
//     between frames by more than the noise could, the light itself changed (a club light
//     switched / recoloured / moved) and the history is cut at once (anti-lag), whatever the
//     neighbourhood clamp would have kept.
//
// IMAGES (LLCleanDenoiserVK; all GENERAL, the denoiser extent; 112 B per texel):
//   IN    rgba16f[2]  layer 0 diffuse (rgb, a = sample confidence: the producer's sample count),
//                     layer 1 specular (rgb, a = hit / light distance in m, 0 = none).
//                     Written by the producer; the a-trous iterations reuse it as ping-pong
//                     scratch once the temporal pass has read it.
//   MOT   rgba32ui    x packHalf2x16(surface: previous texel coordinate - this texel),
//                     y packHalf2x16(virtual image: the same), z the expected previous view
//                     depth (float bits; 0 = behind last frame's eye), w spare (0).
//                     Camera-only: the temporal pass replaces x by the motion vectors
//                     (object motion: dancing avatars) when this frame wrote them.
//   GEOM  rgba32ui[2] per frame parity (the producer writes layer parity, the temporal pass
//                     reads layer parity ^ 1 as the history's guide): x view depth (float
//                     bits; <= 0 = no surface), y pdnPackNR (oct normal 12 + 12 bits,
//                     roughness 8), z packHalf2x16(d(vz)/dx, d(vz)/dy per denoiser texel),
//                     w pdnPackMeta (the producer: W; the temporal pass adds the history
//                     lengths and the specular a-trous depth).
//   HIST  rgba32ui[2] per parity: packHalf2x16 x (diff r, g), y (diff b, spec r), z (spec g, b),
//                     w (diffuse luminance variance, specular luminance variance) -- an
//                     exponentially weighted variance (never E[l^2] - E[l]^2: fp16-safe).
//   OUT   rgba16f[2]  layer 0 diffuse (rgb, a = variance; the final iteration: view depth for
//                     the consumer's upsample), layer 1 specular (rgb, a = variance).
//                     The temporal pass writes it, the a-trous iterations ping-pong OUT <-> IN:
//                     the FINAL image is OUT after an even number of iterations, IN after an
//                     odd one (LLCleanDenoiserVK::finalIsIn; the consumer's push says which).
// (#869) With the SPLIT CHANNEL (below) layers 0 / 1 are the REST's ratio -- the lights not
// split at that texel -- and GEOM / IN / OUT carry the split lights' own visibilities.
#ifndef CLEAN_PDN_COMMON_GLSL
#define CLEAN_PDN_COMMON_GLSL

// Demodulation floor: a black albedo / F0 divides by this (its signal is ~0 there anyway).
#define PDN_DEMOD_MIN 0.02
vec3 pdnDiffFactor(vec3 albedo) { return max(albedo, vec3(PDN_DEMOD_MIN)); }
vec3 pdnSpecFactor(vec3 f0)     { return max(f0, vec3(PDN_DEMOD_MIN)); }

float pdnLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Finite, non-negative, below the fp16 range (a producer's 1 / pdf weight can explode).
vec3 pdnSane(vec3 c)
{
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return clamp(c, vec3(0.0), vec3(60000.0));
}

// ---- normal + roughness word (12 + 12 bit octahedral normal, 8-bit roughness) ----
vec2 pdnOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
uint pdnPackNR(vec3 n, float rough)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    vec2 e = n.z >= 0.0 ? n.xy : pdnOctWrap(n.xy);
    e = clamp(e * 0.5 + 0.5, 0.0, 1.0);
    const uvec2 q = uvec2(e * 4095.0 + 0.5);
    const uint r = uint(clamp(rough, 0.0, 1.0) * 255.0 + 0.5);
    return q.x | (q.y << 12u) | (r << 24u);
}
vec3 pdnUnpackN(uint v)
{
    const vec2 e = vec2(float(v & 4095u), float((v >> 12u) & 4095u)) * (2.0 / 4095.0) - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float pdnUnpackR(uint v) { return float(v >> 24u) * (1.0 / 255.0); }

// ---- GEOM w: history lengths (6 bits each, 0..63 frames), the specular a-trous depth
//      (3 bits: iterations the specular takes, 0..7) and W (fp16, high half) ----
uint  pdnPackMeta(float lenD, float lenS, uint specIt, float W)
{
    const uint d = uint(clamp(lenD, 0.0, 63.0) + 0.5);
    const uint s = uint(clamp(lenS, 0.0, 63.0) + 0.5);
    return d | (s << 6u) | (min(specIt, 7u) << 12u) | (packHalf2x16(vec2(clamp(W, 0.0, 60000.0), 0.0)) << 16u);
}
// bit 15 (the temporal pass): the texel's diffuse is CONVERGED under ReSTIR DI (#869 / #85: a reused
// reservoir of high M and a long history) -- the a-trous gives it fewer iterations (DN_CONV_SHIFT)
bool  pdnMetaConv(uint w)   { return (w & 0x8000u) != 0u; }
float pdnMetaLenD(uint w)   { return float(w & 63u); }
float pdnMetaLenS(uint w)   { return float((w >> 6u) & 63u); }
uint  pdnMetaSpecIt(uint w) { return (w >> 12u) & 7u; }
float pdnMetaW(uint w)      { return unpackHalf2x16(w >> 16u).x; }

// ---- HIST word ----
uvec4 pdnPackHist(vec3 d, vec3 s, float varD, float varS)
{
    return uvec4(packHalf2x16(d.rg), packHalf2x16(vec2(d.b, s.r)), packHalf2x16(s.gb),
                 packHalf2x16(clamp(vec2(varD, varS), vec2(0.0), vec2(60000.0))));
}
void pdnUnpackHist(uvec4 h, out vec3 d, out vec3 s, out float varD, out float varS)
{
    const vec2 a = unpackHalf2x16(h.x), b = unpackHalf2x16(h.y), c = unpackHalf2x16(h.z), v = unpackHalf2x16(h.w);
    d = vec3(a, b.x);
    s = vec3(b.y, c);
    varD = v.x; varS = v.y;
}

// The specular a-trous depth from roughness and the hit distance (the lobe's footprint):
// a mirror keeps its own texel (0 iterations), a rough lobe takes all of them. f: the lobe's
// angular footprint ~ roughness^2, scaled by how far the reflected light lies relative to the
// receiver's distance (a distant reflected source keeps a tighter footprint on screen).
uint pdnSpecIters(float rough, float hitDist, float vz, uint maxIt)
{
    const float a = rough * rough;
    const float hf = hitDist > 0.0 ? clamp(hitDist / max(hitDist + vz, 1e-3) * 2.0, 0.25, 1.0) : 1.0;
    const float radius = a * hf * 48.0;   // texels the lobe spans at this surface (heuristic)
    if (radius < 0.75) return 0u;
    return min(uint(floor(log2(radius))) + 1u, maxIt);
}

// RATIO SIGNALS (the projector sampler, #85 phase 2 ratio estimator): a producer may instead
// write a VISIBILITY RATIO per signal (shadowed / unshadowed estimate of the same samples, rgb in
// [0, 1]; Heitz et al. 2018) -- no light colour ever enters the history; the consumer multiplies
// its exact, fresh unshadowed lighting by the denoised ratio.
// Debug view codes (RenderVKDenoiseDebug; the consumer's push bits): 1 raw input (layer 0, one
// frame), 2 history length (r diffuse, g specular, of the max), 3 variance (r diffuse, g
// specular, sqrt relative to the signal), 4 the denoised layer 0 (diffuse), 5 the denoised
// layer 1 (specular), 6 the denoised layer 0 (left half) beside the lit image (right half).
#define PDN_DBG_RAW 1u
#define PDN_DBG_LEN 2u
#define PDN_DBG_VAR 3u
#define PDN_DBG_DEMOD 4u
#define PDN_DBG_REMOD 5u
#define PDN_DBG_SPLIT 6u
// 7 (#869) the ReSTIR view: the producer writes a false colour into layer 0 (pass 15 PDN_OPT_DBG_RS:
// the chosen light's hue x the reservoir's M); the denoiser passes it through like RAW.
#define PDN_DBG_RESTIR 7u
// 8 / 9 (#869) the split lights by id / x their denoised visibility (SPLIT CHANNEL below; the
// denoiser's debug field is 4 bits wide for them, the consumer's 3: the host sends it 4).

// ---- SUN / MOON CHANNEL (the merged RT shadow pass, RenderVKRTShadowMerge: #195 + #85) ----
// Present only when LLCleanDenoiserVK::ensure(.., sun = true) allocated it (IN / OUT 3 layers,
// HIST 4) and used only on frames whose push says so (clean_dn_common.glsl DN_F_SUN); every
// other frame runs the projector-only code exactly.
//   IN  layer 2  rgb = this frame's raw sun / moon visibility x translucent tint (the RT shadow
//                mask's sun convention: [0, 1], the cascade-range fade and the weak-sun fade
//                applied by the producer), a = pdnSunEncA (penumbra radius in denoiser texels
//                + the moving-caster bit).
//   HIST layers 2 + parity (rgba32ui): x packHalf2x16(acc r, g), y (acc b, variance),
//                z (history length + 64 x the moving-caster countdown, fast luminance mean),
//                w (the penumbra radius the history holds, -1 none; fast luminance 2nd moment).
//   OUT layer 2  rgb = the accumulated / filtered visibility, a = variance (the final a-trous
//                iteration: the view depth, for the SHADOW kernel's depth-aware upsample).
// Its history is never cut by W (a club light switching says nothing about the sun).
float pdnSunEncA(float r, bool dyn) { return r < 0.0 ? -1.0 : (dyn ? -(2.0 + r) : r); }
void  pdnSunDecA(float a, out float r, out bool dyn)
{
    if (isnan(a) || isinf(a)) { r = -1.0; dyn = false; return; }
    dyn = a < -1.5;
    r = a > -0.5 ? a : (dyn ? -a - 2.0 : -1.0);
}
uvec4 pdnPackSunHist(vec3 acc, float var, float len, uint cnt, float fm, float fm2, float r)
{
    const float lc = clamp(floor(len + 0.5), 0.0, 63.0) + 64.0 * float(min(cnt, 7u));
    return uvec4(packHalf2x16(acc.rg), packHalf2x16(vec2(acc.b, clamp(var, 0.0, 60000.0))),
                 packHalf2x16(vec2(lc, clamp(fm, 0.0, 1.0))), packHalf2x16(vec2(clamp(r, -1.0, 60000.0), clamp(fm2, 0.0, 1.0))));
}
struct PdnSunHist { vec3 acc; float var; float len; uint cnt; float fm; float fm2; float r; };
PdnSunHist pdnUnpackSunHist(uvec4 h)
{
    PdnSunHist o;
    const vec2 a = unpackHalf2x16(h.x), b = unpackHalf2x16(h.y), c = unpackHalf2x16(h.z), d = unpackHalf2x16(h.w);
    o.acc = vec3(a, b.x);
    o.var = b.y;
    const float lc = max(c.x, 0.0);
    o.cnt = uint(floor(lc / 64.0));
    o.len = lc - 64.0 * float(o.cnt);
    o.fm = c.y;
    o.fm2 = d.y;
    o.r = d.x;
    return o;
}
// Visibility in [0, 1] (NaN / Inf -> lit).
vec3 pdnSunVis(vec3 v) { return (any(isnan(v)) || any(isinf(v))) ? vec3(1.0) : clamp(v, vec3(0.0), vec3(1.0)); }

// ---- SPLIT CHANNEL: PER-LIGHT VISIBILITY OF THE DOMINANT LIGHTS (#869, RenderVKProjStochasticSplitK) ----
// THE LEAK IT FIXES: the ratio estimator above gives every light at a texel ONE shared ratio
// (sum of shadowed / sum of unshadowed over the sampled lights). Where an unblocked light and a
// blocked one meet -- a sealed room's own lamps beside the club's projector behind its wall -- the
// ratio is the lamps' visibility x their share, well above 0, and the consumer multiplied the
// BLOCKED projector's exact unshadowed gobo by it: its gobo on the room's wall at full detail, only
// dimmer; and the lamps were darkened by the projector's 0. The standard remedy (one ratio per
// light where it matters): the K strongest lights of the lighting set at the texel (by their
// gobo-free unshadowed target, pass 15 pdnSplitSelect: a deterministic list -- the pixel cluster's,
// never the random RIS window -- with hysteresis for last frame's choice, ties by id) each get
// their OWN shadow ray every frame and their OWN visibility, denoised by key; the shared ratio
// (layers 0 / 1, now THE REST) is estimated over the other lights only (excluded from its
// numerator and its denominator: candidates, reused reservoirs, the dense evaluation). The
// consumer: direct = sum_k unshadowed_k x v_k + (the rest's unshadowed) x the rest ratio. The
// candidate set still equals the lighting set: rest = lighting set - split, split c lighting set.
// One light, one ray: its visibility serves its diffuse and specular share alike (MONO -- a split
// light's shadow through a coloured translucent caster shows its luminance, not its tint; the
// rest keeps rgb). Allocated only with K > 0 (LLCleanDenoiserVK::ensure(.., split = true));
// present <=> GEOM has PDN_SPLIT_GEOM_LAYERS layers (imageSize(..).z: no flag anywhere). K = 0
// or a producer that does not split (the per-light-type pass 15) writes id 0 in every slot: the
// consumer then finds no split light and every light takes the rest = the old shared ratio.
//   IDENTITY: Light.tex.w = the light's SPLIT ID, a host-assigned persistent 16-bit id per light
//     key (llcleanrenderervk.cpp splitIdsFrame; 0 = none: never split). The light list is
//     re-sorted every frame -- an index names another light next frame, the id does not -- so
//     the history follows the LIGHT, not the slot, with no remap (the denoiser has no ReSTIR
//     header): a light in slot 0 last frame and slot 1 now keeps its history; a new one starts.
//   GEOM layers 2 + parity (rgba32ui; the producer writes x..w, the temporal pass rewrites z, w):
//     x id0 | id1 << 16, y id2 | id3 << 16 (0 = empty slot);
//     z, w the slots' 16-bit words, slot k = word k of (z, w): the producer's RAW sample
//     (pdnSplitRawPack: value x 32766, 0x7FFF = no information -- the light has no term here --,
//     bit 15 = the light MOVED this frame: the moving-light history cap), then the temporal
//     pass's HISTORY (pdnSplitHistPack: the accumulated visibility, 11 bits, + its length, 5 bits).
//   GEOM layer 4 (the FINAL, written by the last a-trous iteration; parity-free, so the consumer
//     needs no parity): x, y the ids, z, w the denoised visibilities (unorm16; pdnSplitFinalVal).
//   IN / OUT the LAST layer (rgba16f): the slots' visibilities (temporal output, a-trous
//     ping-pong; empty slots 1). The a-trous luminance stop's sigma is the Bernoulli bound
//     sqrt(v (1 - v) / length) of the one-ray-per-frame estimate (no stored variance).
//   +64 B per denoiser texel (GEOM 3 layers, IN / OUT 1 each; +16 B more without the sun channel:
//     its layer is allocated then too, so the split layer is always the last).
// Debug (RenderVKDenoiseDebug 8 / 9, the denoiser's last a-trous iteration writes it into layer 0;
// the consumer shows layer 0): 8 the split lights by id (a stable hue per id; with several slots,
// 4-texel diagonal bands cycle through them), 9 the same hue x that light's denoised visibility.
#define PDN_SPLIT_MAX 4u
#define PDN_SPLIT_GEOM_PAR 2          // + parity
#define PDN_SPLIT_GEOM_FINAL 4
#define PDN_SPLIT_GEOM_LAYERS 5       // GEOM's layer count with the split channel
#define PDN_SPLIT_RAW_NONE 0x7FFFu
#define PDN_DBG_SPLIT_ID 8u
#define PDN_DBG_SPLIT_RATIO 9u
// slot k's 16-bit word of a (lo, hi) pair: k 0 / 1 in x, 2 / 3 in y
uint  pdnSplitWord(uvec2 p, uint k) { return ((k < 2u ? p.x : p.y) >> ((k & 1u) * 16u)) & 0xFFFFu; }
uvec2 pdnSplitPack4(uvec4 w) { return uvec2((w.x & 0xFFFFu) | (w.y << 16u), (w.z & 0xFFFFu) | (w.w << 16u)); }
// the slot holding id (-1: none)
int   pdnSplitFind(uvec2 ids, uint id)
{
    int at = -1;
    for (uint k = 0u; k < PDN_SPLIT_MAX; ++k)
        if (at < 0 && id != 0u && pdnSplitWord(ids, k) == id) at = int(k);
    return at;
}
// (pass 15) light l is one of this texel's split lights spLi (empty: 0xFFFFFFFF) -- the rest leaves it out
bool  pdnSplitHas(uvec4 spLi, uint l) { return any(equal(spLi, uvec4(l))); }
uint  pdnSplitRawPack(float v, bool info, bool moved)
{
    const uint q = info ? uint(clamp(v, 0.0, 1.0) * 32766.0 + 0.5) : PDN_SPLIT_RAW_NONE;
    return q | (moved ? 0x8000u : 0u);
}
float pdnSplitRawVal(uint w)   { const uint q = w & 0x7FFFu; return q == PDN_SPLIT_RAW_NONE ? -1.0 : float(q) * (1.0 / 32766.0); }
bool  pdnSplitRawMoved(uint w) { return (w & 0x8000u) != 0u; }
// History word: v in 11 bits rounded TOWARD this frame's sample (toward): every frame moves it at
// least one step to the sample, so a constant lit / shadowed light converges to exactly 1 / 0
// (nearest rounding stalled ~1 % short at a 31-frame history: the step fell under half a unit).
uint  pdnSplitHistPack(float v, float len, float toward)
{
    const float x = clamp(v, 0.0, 1.0) * 2047.0;
    const float q = toward * 2047.0 >= x ? ceil(x - 1e-3) : floor(x + 1e-3);
    return uint(clamp(q, 0.0, 2047.0)) | (uint(clamp(len, 0.0, 31.0) + 0.5) << 11u);
}
float pdnSplitHistVal(uint w) { return float(w & 2047u) * (1.0 / 2047.0); }
float pdnSplitHistLen(uint w) { return float(w >> 11u); }
uint  pdnSplitFinalPack(float v) { return uint(clamp(v, 0.0, 1.0) * 65535.0 + 0.5); }
float pdnSplitFinalVal(uint w)   { return float(w) * (1.0 / 65535.0); }
// The debug views' colour of split id (a stable hue per id).
vec3 pdnSplitHue(uint id)
{
    const float h = fract(float(id) * 0.61803399);
    return clamp(abs(fract(vec3(h) + vec3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

#endif
