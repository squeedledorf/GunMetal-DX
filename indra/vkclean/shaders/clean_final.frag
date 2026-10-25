#version 450
// vkclean — final display-space effects (Lightbox; port of upstream Alchemy
// class1/alchemy/blitWithEffectsF.glsl + postEffectUtilsF.glsl): lens distortion,
// vignette, colour-vision compensation, film grain, dither, preview modes. Runs last,
// after FXAA/SMAA, into the swapchain. The effect functions below are upstream's,
// verbatim; their uniforms are mapped onto the FinalBlock UBO by the macros here.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D diffuseRect;   // finished frame (display space)
layout(set = 0, binding = 2) uniform sampler2D uReferenceStill; // grabbed frame (same point as diffuseRect)

layout(std140, set = 0, binding = 1) uniform FinalBlock {
    vec4 res_frame;       // resolution.xy, frame id (mod 2^20), 0
    vec4 vig0;            // amount, radius, soft, shape
    vec4 vig_color;       // rgb, mid point
    vec4 vig_mid_color;   // rgb, feather
    vec4 vig_center_aspect; // center.xy, aspect (w, h)
    vec4 grain0;          // amount, style, size, range
    vec4 grain_tint;      // rgb, animate
    vec4 cvd;             // compensate mode, compensate amount, preview mode, dither amount
    vec4 dither;          // bits, animate, 0, 0
    vec4 lens0;           // amount, fit scale, k1, k2
    vec4 lens1;           // squeeze.xy, center.xy
    vec4 lens2;           // tangential.xy, CAS sharpness (0 = off), 0
    vec4 ref;             // wipe mode (0 off, 1 wipe, 2 side by side), seam position, 0, 0
    vec4 ads;             // <GMDX> ADS vignette strength (0 = off), 0, 0, 0
} fx;
#define uRefWipeMode           int(fx.ref.x + 0.5)
#define uRefWipePos            fx.ref.y

#define uResolution            fx.res_frame.xy
#define uFrameId               uint(fx.res_frame.z)
#define uVignetteAmount        fx.vig0.x
#define uVignetteRadius        fx.vig0.y
#define uVignetteSoft          fx.vig0.z
#define uVignetteShape         fx.vig0.w
#define uVignetteColor         fx.vig_color.rgb
#define uVignetteMidPoint      fx.vig_color.w
#define uVignetteMidColor      fx.vig_mid_color.rgb
#define uVignetteFeather       fx.vig_mid_color.w
#define uVignetteCenter        fx.vig_center_aspect.xy
#define uVignetteAspect        fx.vig_center_aspect.zw
#define uGrainAmount           fx.grain0.x
#define uGrainStyle            int(fx.grain0.y + 0.5)
#define uGrainSize             fx.grain0.z
#define uGrainRange            fx.grain0.w
#define uGrainTint             fx.grain_tint.rgb
#define uGrainAnimate          (fx.grain_tint.w > 0.5)
#define uCompensateMode        int(fx.cvd.x + 0.5)
#define uCompensateAmount      fx.cvd.y
#define uPreviewMode           int(fx.cvd.z + 0.5)
#define uDitherAmount          fx.cvd.w
#define uDitherBits            int(fx.dither.x + 0.5)
#define uDitherAnimate         (fx.dither.y > 0.5)
#define uLensDistortAmount     fx.lens0.x
#define uLensDistortScale      fx.lens0.y
#define uLensDistortK          fx.lens0.zw
#define uLensDistortSqueeze    fx.lens1.xy
#define uLensDistortCenter     fx.lens1.zw
#define uLensDistortTangential fx.lens2.xy

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// Cheap 2D → [0,1) hash used by noise-based effects (film grain, dither).
// Derived from Dave Hoskins' hash collection — quick and good enough for
// per-pixel decorrelated noise without needing a texture lookup.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Per-frame pixel-space offset for temporally decorrelated noise. The frame
// id is hashed into two independent axes and scaled by 1024 so the sample
// location jumps to an effectively uncorrelated region each frame — giving
// a full pattern reshuffle instead of the scrolling you'd get from just
// adding the frame id to the input. Pass `animate=false` to freeze the
// pattern (useful for stills and debugging).
vec2 frameNoiseOffset(bool animate)
{
    uint  frame = animate ? uFrameId : 0u;
    float ff    = float(frame);
    return vec2(hash12(vec2(ff, 13.7)),
                hash12(vec2(ff, 91.3))) * 1024.0;
}




vec3 applyVignette(vec3 color, vec2 uv)
{
    if (uVignetteAmount <= 0.0)
        return color;

    // Center offset and branchless aspect correction: one component of
    // `scale` is always 1.0, the other is the larger-axis ratio, so the
    // "short-axis half-height" normalization below holds for any viewport.
    vec2  d      = uv - 0.5 - uVignetteCenter;
    float aspect = max(uVignetteAspect.x, 1e-4) / max(uVignetteAspect.y, 1e-4);
    d *= max(vec2(aspect, 1.0 / aspect), 1.0);

    // Two distance metrics, blended by `uVignetteShape`:
    // `sq_base` is non-negative by construction, so `x * sqrt(x)` is a valid
    // faster form of pow(x, 1.5) without a domain guard.
    float circular = length(d) * 2.0;
    float sq_base  = max(abs(d.x), abs(d.y)) * 2.0;
    float square   = sq_base * sqrt(sq_base);
    float dist     = mix(circular, square, uVignetteShape);

    // `start` is where fading begins; `end` is where the edge color is reached.
    float start = uVignetteRadius - uVignetteSoft;
    float end   = uVignetteRadius;

    // Three-color ramp only if midpoint is strictly inside (0, 1).
    if (uVignetteMidPoint > 1e-4 && uVignetteMidPoint < 0.9999)
    {
        float mid      = mix(start, end, uVignetteMidPoint);
        float tMidRaw  = smoothstep(start, mid, dist);
        float tEdgeRaw = smoothstep(mid,   end, dist);
        // pow reshapes the mask before scaling by overall amount.
        float tMid  = pow(tMidRaw,  uVignetteFeather) * uVignetteAmount;
        float tEdge = pow(tEdgeRaw, uVignetteFeather) * uVignetteAmount;
        color = mix(color, uVignetteMidColor, tMid);
        color = mix(color, uVignetteColor,    tEdge);
    }
    else
    {
        float tRaw = smoothstep(start, end, dist);
        float t    = pow(tRaw, uVignetteFeather) * uVignetteAmount;
        color = mix(color, uVignetteColor, t);
    }
    return color;
}


// =============================================================================
// Film grain — additive hash-based noise with luma weighting
// =============================================================================
//
// Four styles:
//
// Size is multiplied by the display's resolution-relative pixel scale so
// grain particles stay perceptually constant across 1080p / 4K / HiDPI
// outputs. Amount is squared in-shader for a gentler slider toe; keeping
// it in-shader (vs. pre-baking on the CPU like the CA uniforms) lets a
// future UI slider animate without rebinding on every change.


// Noise sample in ~[-0.5, 0.5]. Style controls color vs luma and whether
// cells are quantized for a coarser look. The caller scales by amplitude.
vec3 grainSample(vec2 fragCoord, int style, float size)
{
    float cell = max(size, 1.0);
    vec2  gp   = floor(fragCoord / cell) * cell;
    if (style == 1)
    {
        // Independent per-channel noise for a "digital" look.
        return vec3(hash12(gp),
                    hash12(gp + 17.0),
                    hash12(gp + 31.0)) - 0.5;
    }
    return vec3(hash12(gp) - 0.5);
}

vec3 applyFilmGrain(vec3 color, vec2 fragCoord)
{
    if (uGrainAmount <= 0.0)
        return color;

    // 0.1 is the empirical ceiling where grain stops reading as "film
    // texture" and starts reading as "broken image." Squaring amount
    // biases the slider toward subtle values.
    float amp   = uGrainAmount * uGrainAmount * 0.1;
    float gluma = dot(color, LUMA);

    // Scale grain cells by resolution so a "size 1" grain looks the same
    // perceptual size on 1080p and 4K displays.
    float pixelScale    = max(uResolution.x, uResolution.y) / 1080.0;
    float effectiveSize = uGrainSize * pixelScale;
    vec3  n = grainSample(fragCoord + frameNoiseOffset(uGrainAnimate),
                          uGrainStyle, effectiveSize);

    if (uGrainStyle == 3)
    {
        // Photon shot noise: amplitude proportional to sqrt(luminance),
        // matching how CCD sensor noise scales with photon count.
        color += n * uGrainTint * sqrt(max(gluma, 0.0)) * amp;
    }
    else
    {
        // Midtone-biased bell: weight peaks at uGrainRange and falls off
        // toward shadows/highlights. Matches how real film grain is most
        // visible in midtones.
        float d      = gluma - uGrainRange;
        float weight = max(1.0 - (d * d) * 4.0, 0.0);
        color += n * uGrainTint * weight * amp;
    }
    return color;
}


// =============================================================================
// Color Vision Deficiency — simulation, daltonization, accessibility preview
// =============================================================================
//
// Two separate use cases share these helpers:
//
// CVD matrices are Machado et al. 2009 (severity = 1.0) — physiologically
// grounded and slightly more accurate than Brettel/Viénot, especially for
// tritanopia. Monochromacy/achromatomaly modes are implemented as
// desaturation toward Rec.709 luma.

// ---- Compensation (daltonization, output path) -----------------------------

// ---- Preview / debug --------------------------------------------------------

// Forward-simulate how a CVD viewer perceives `c`. Used both internally
// (daltonize computes the difference between original and simulated) and
// as the preview overlay.
vec3 simulateCVD(vec3 c, int mode)
{
    if (mode == 4)
    {
        // Achromatopsia (rod monochromacy). Real rod response peaks at
        // ~507nm and biases neutral gray slightly cyan-green; standard
        // practice uses Rec.709 luma as a reasonable proxy.
        return vec3(dot(c, LUMA));
    }
    if (mode == 5)
    {
        // Blue cone monochromacy — only S-cones work, so the image
        // collapses to a blue-yellow axis. Approximated by extracting the
        // blue-yellow opponent signal and recombining as a desaturated
        // blue tint. Not physiologically exact but matches common
        // simulator output and reads correctly to normal viewers.
        float l  = dot(c, LUMA);
        float by = c.b - 0.5 * (c.r + c.g);
        return vec3(l - by * 0.15, l - by * 0.05, l + by * 0.25);
    }
    if (mode == 6)
    {
        // Achromatomaly — partial color loss. 80% blend toward gray
        // retains a hint of residual color.
        return mix(c, vec3(dot(c, LUMA)), 0.8);
    }

    mat3 m;
    if (mode == 1)
    {
        // Protanopia (Machado 2009, severity 1.0)
        m = mat3(0.152286, 1.052583, -0.204868,
                 0.114503, 0.786281,  0.099216,
                -0.003882,-0.048116,  1.051998);
    }
    else if (mode == 2)
    {
        // Deuteranopia (Machado 2009, severity 1.0)
        m = mat3(0.367322, 0.860646, -0.227968,
                 0.280085, 0.672501,  0.047413,
                -0.011820, 0.042940,  0.968881);
    }
    else
    {
        // Tritanopia (Machado 2009, severity 1.0)
        m = mat3(1.255528,-0.078441, -0.004733,
                -0.076749, 0.930809,  0.691367,
                 0.178779,-0.147632,  0.303900);
    }
    return m * c;
}

// Daltonization: compute the error between original and CVD-simulated
// image, then redistribute that error into channels the viewer can still
// see. Monochromacies (modes ≥ 4) have no perceivable axis to shift into,
// so the function is a no-op for them.
vec3 daltonize(vec3 c, int mode, float amount)
{
    if (mode >= 4) return c;

    vec3 sim = simulateCVD(c, mode);
    vec3 err = c - sim;
    vec3 shift;
    if (mode == 3)
    {
        // Tritan — shift blue error into the red/green axis.
        float be = err.b;
        shift = vec3( be*0.7 + err.g*0.3, -be*0.7 - err.g*0.3, 0.0);
    }
    else
    {
        // Protan/deutan — shift red/green error into green and blue.
        shift = vec3(0.0, err.r*0.7 + err.g, err.r*0.7);
    }
    return c + shift * amount;
}

// False-color exposure map (ARRI-style palette). 8 bands from under-exposed
// (deep blue) to clipped (white), keyed on Rec.709 luminance.
vec3 falseColor(vec3 c)
{
    float l = dot(c, LUMA);
    if (l < 0.02) return vec3(0.0, 0.0, 0.4);
    if (l < 0.10) return vec3(0.0, 0.3, 0.8);
    if (l < 0.30) return vec3(0.0, 0.7, 0.5);
    if (l < 0.50) return vec3(0.3, 0.7, 0.0);
    if (l < 0.70) return vec3(0.8, 0.8, 0.0);
    if (l < 0.90) return vec3(1.0, 0.5, 0.0);
    if (l < 0.98) return vec3(1.0, 0.1, 0.0);
    return vec3(1.0);
}

// Accessibility pipeline entry. Zero-cost when disabled.
vec3 applyCVDCompensation(vec3 color)
{
    if (uCompensateMode <= 0 || uCompensateAmount <= 0.0)
        return color;
    return clamp(daltonize(color, uCompensateMode, uCompensateAmount), 0.0, 1.0);
}

// Preview overlay entry. Mode 7 replaces the image entirely with a
// false-color map; modes 1-6 re-simulate the output as a CVD viewer
// would perceive it. Intended for debugging only — must be 0 for final
// shipping output.
vec3 applyPreview(vec3 color)
{
    // Fast path — zero preview mode is the shipping configuration and should
    // cost a single compare.
    if (uPreviewMode == 0)
        return color;
    if (uPreviewMode == 7)
        return falseColor(color);
    return simulateCVD(clamp(color, 0.0, 1.0), uPreviewMode);
}


// =============================================================================
// Dither — triangular PDF noise for banding suppression at the output stage
// =============================================================================
//
// Interleaved Gradient Noise (Jimenez, COD:AW 2014) sampled twice with a
// small offset to produce a TPDF-shaped noise in [-1, 1]. TPDF is
// perceptually superior to rectangular PDF — it fully masks quantization
// banding with no residual noise-floor shift.
//
// Temporal decorrelation is handled by frameNoiseOffset() (see top of file):
// the noise pattern fully reshuffles between frames instead of visibly
// scrolling as uFrameId increments.
//
// Amplitude scales to output bit depth: ±1/255 for 8-bit, ±1/1023 for 10-bit.


// Interleaved Gradient Noise core — hashes a 2D input to a well-distributed
// [0, 1) sample. Frame decorrelation is applied at the call site via
// frameNoiseOffset(); the TPDF pair must share the same offset so keeping it
// outside IGN is essential, not just an optimization.
float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 applyDither(vec3 color, vec2 fragCoord)
{
    if (uDitherAmount <= 0.0)
        return color;

    // Compute the frame offset *once* and reuse for both TPDF taps — the
    // IGN calls are cheap but factoring the per-frame hash out keeps the
    // pattern coherent across the pair (otherwise the two taps would
    // decorrelate from each other, not just from previous frames).
    vec2 p = fragCoord + frameNoiseOffset(uDitherAnimate);

    float tpdf = ign(p) + ign(p + vec2(1.618, 2.414)) - 1.0;

    float levels = (uDitherBits >= 10) ? 1023.0 : 255.0;
    return color + tpdf * (uDitherAmount / levels);
}


// =============================================================================
// Geometric lens distortion — Brown-Conrady radial + tangential
// =============================================================================
//
// The only entry point here that transforms a coordinate instead of a colour.
// Runs in the final blit, warping the coordinate the scene is sampled at, so
// the vignette/grain/dither/CVD chain that follows stays in unwarped sensor
// space. That split is deliberate: distortion happens in the lens, the print
// effects happen at the sensor and on the print.
//
// This is the *inverse* map — for each output pixel it answers "where in the
// source does this come from", which is the direction a gather-based post
// process needs. Negative k1 therefore reads as barrel and positive as
// pincushion, matching how the coefficients are named on a real lens profile.
//
// Radial distance is measured in aspect-corrected units using the same
// branchless form the rest of this file uses: one component of `scale` stays
// 1.0 and the other carries the ratio, so the wide axis is stretched to its
// true physical extent and r lands near 1.0 at the frame corners. That is not
// cosmetic -- on a 16:9 frame the corners really are further from the optical
// axis than the edge midpoints, and a lens distorts by physical radius.
//
// Note: uLensDistortAmount, uLensDistortK, uLensDistortScale, uLensDistortSqueeze
// and uLensDistortTangential arrive pre-baked from the CPU (pipeline.cpp).
// The master amount is folded into the coefficients there, so the shader does
// one gate and then pure polynomial evaluation. The slider ranges quoted below
// are the *user-facing* values before baking.


vec2 applyLensDistortion(vec2 uv)
{
    // Fast path when the effect is disabled — the uniform branch is coherent
    // across the whole draw, and zero-initialized uniforms land here (see the
    // GL 4.1 no-default-initializers note in llshadermgr).
    if (uLensDistortAmount <= 0.0)
        return uv;

    // Offset from the optical axis, then aspect-corrected so the radial term
    // is isotropic in physical units rather than in UV units.
    vec2  p      = uv - 0.5 - uLensDistortCenter;
    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec2  scale  = max(vec2(aspect, 1.0 / max(aspect, 1e-4)), 1.0);
    vec2  q      = p * scale;

    float r2 = dot(q, q);

    // Radial: 1 + k1*r^2 + k2*r^4. Horner keeps it to two FMAs.
    float radial = 1.0 + r2 * (uLensDistortK.x + r2 * uLensDistortK.y);

    // Tangential: the classic Brown-Conrady decentering pair. Zero by default,
    // and the two terms vanish independently, so leaving them at 0 costs only
    // the multiplies.
    float p1 = uLensDistortTangential.x;
    float p2 = uLensDistortTangential.y;
    vec2  tangential = vec2(2.0 * p1 * q.x * q.y + p2 * (r2 + 2.0 * q.x * q.x),
                            p1 * (r2 + 2.0 * q.y * q.y) + 2.0 * p2 * q.x * q.y);

    // Warp, rescale to keep the frame filled, undo the aspect correction, then
    // apply the anamorphic squeeze in UV space.
    vec2 warped = (q * radial + tangential) * uLensDistortScale;
    warped /= scale;
    warped *= uLensDistortSqueeze;

    return warped + 0.5 + uLensDistortCenter;
}

// =============================================================================
// Lens dirt — grime on the front element, lit by whatever is already glowing
// =============================================================================
//
// Contributes nothing on its own. Dirt is only visible where light is already
// falling on it, so this takes the bloom and flare terms as its input rather
// than the scene: point the camera at a flat wall and the lens looks clean no
// matter how high the strength goes, exactly as a real one does. It is also
// what makes the effect cheap -- the cost rides on effects that are already
// running.
//
// Tier 3 (uniform branch) rather than a compile-time permutation, because this
// file is a shared object attached to all nine post programs and no
// per-program define can reach it. The strength is forced to 0 by the CPU
// whenever no plate exists, so the sampler is never read unbound.
//
// The plate is a single channel: dirt is a scalar mask, and the generator
// (lensDirtGenF.glsl) writes one into an R8 target.

float lensDistortMask(vec2 duv)
{
    if (uLensDistortAmount <= 0.0)
        return 1.0;

    vec2 inside = step(vec2(0.0), duv) * step(duv, vec2(1.0));
    return inside.x * inside.y;
}


// AMD FidelityFX CAS (contrast adaptive sharpening), no-scaling path, as LL's
// RenderCASSharpness: 3x3 cross/box min-max, amplitude from the headroom, a
// negative-lobe cross filter weighted by it.
vec3 casSample(sampler2D src, vec2 uv)
{
    const float sharp = fx.lens2.z;
    vec3 e = texture(src, uv).rgb;
    if (sharp <= 0.0) return e;
    vec2 px = 1.0 / uResolution;
    vec3 a = texture(src, uv + vec2(-px.x, -px.y)).rgb;
    vec3 b = texture(src, uv + vec2( 0.0,  -px.y)).rgb;
    vec3 c = texture(src, uv + vec2( px.x, -px.y)).rgb;
    vec3 d = texture(src, uv + vec2(-px.x,  0.0)).rgb;
    vec3 f = texture(src, uv + vec2( px.x,  0.0)).rgb;
    vec3 g = texture(src, uv + vec2(-px.x,  px.y)).rgb;
    vec3 h = texture(src, uv + vec2( 0.0,   px.y)).rgb;
    vec3 i = texture(src, uv + vec2( px.x,  px.y)).rgb;
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(mn, min(min(a, c), min(g, i)));
    mn += mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(mx, max(max(a, c), max(g, i)));
    mx += mx2;
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, clamp(sharp, 0.0, 1.0));
    vec3 w = amp * peak;
    vec3 rcpW = 1.0 / (1.0 + 4.0 * w);
    return clamp((b * w + d * w + f * w + h * w + e) * rcpW, 0.0, 1.0);
}

// Reference still (upstream blitWithEffectsF sampleWithReference): pick the pane and
// the side of the seam from the raw uv, then distort, so still and live carry the
// same lens warp. uRefWipeMode is 0 whenever no still exists.
vec4 sampleWithReference(vec2 uv)
{
    vec2 pane_uv    = uv;
    bool from_still = false;
    if (uRefWipeMode == 1)
    {
        from_still = (uv.x < uRefWipePos);
    }
    else if (uRefWipeMode == 2)
    {
        from_still = (uv.x < 0.5);
        pane_uv.x  = from_still ? uv.x * 2.0 : (uv.x - 0.5) * 2.0;
    }
    vec2 duv = applyLensDistortion(pane_uv);
    vec4 s = vec4(from_still ? casSample(uReferenceStill, duv) : casSample(diffuseRect, duv), 1.0);
    s.rgb *= lensDistortMask(duv);
    return s;
}

// <GMDX> Aim-down-sights vignette (fscombatads.cpp): SoapStorm's exoVignetteF.glsl as its
// ADS path drives it (vignette = (1, 2 * strength, 1)): a radial darkening over the whole
// frame, from the centre out, that deepens toward the corners.
vec3 applyADSVignette(vec3 color, vec2 uv)
{
    if (fx.ads.x <= 0.0)
        return color;
    vec2 tc = uv - 0.5;
    float v = 1.0 - dot(tc, tc);
    return color * clamp(pow(v * v * v * v, 2.0 * fx.ads.x), 0.0, 1.0);
}
// </GMDX>

// Hairline at the seam, one screen pixel wide at any resolution.
vec3 applyWipeSeam(vec3 color, vec2 uv)
{
    if (uRefWipeMode == 0)
        return color;
    float seam  = (uRefWipeMode == 1) ? uRefWipePos : 0.5;
    float width = fwidth(uv.x);
    float line  = 1.0 - smoothstep(0.0, width, abs(uv.x - seam));
    return mix(color, vec3(1.0), line * 0.75);
}

void main()
{
    vec4 diff = sampleWithReference(v_uv);
    diff.rgb = applyVignette(diff.rgb, v_uv);
    diff.rgb = applyADSVignette(diff.rgb, v_uv);   // <GMDX>
    diff.rgb = applyCVDCompensation(diff.rgb);
    diff.rgb = applyFilmGrain(diff.rgb, gl_FragCoord.xy);
    diff.rgb = applyDither(diff.rgb, gl_FragCoord.xy);
    diff.rgb = applyPreview(diff.rgb);
    diff.rgb = applyWipeSeam(diff.rgb, v_uv);
    outColor = vec4(clamp(diff.rgb, 0.0, 1.0), 1.0);
}
