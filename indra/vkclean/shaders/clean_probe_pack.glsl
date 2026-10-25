// vkclean -- packed HDR texel encodings for the reflection-probe arrays (#565,
// RenderVKProbeFormat 1 / 2). The arrays are E5B9G9R9_UFLOAT / B10G11R11_UFLOAT images
// written through an R32_UINT alias view (neither format is a storage format on most
// GPUs; the hardware conversion of an r11f_g11f_b10f imageStore may also truncate) and
// sampled in their own format. Both encoders round to nearest (ties to even for the
// small floats), clamp to the format's largest finite value and map negatives / NaN to 0.
// Reference: EXT_texture_shared_exponent (RGB9E5), EXT_packed_float / the Vulkan
// "Unsigned 11-Bit / 10-Bit Floating-Point Numbers" sections (R11G11B10).
#ifndef CLEAN_PROBE_PACK_GLSL
#define CLEAN_PROBE_PACK_GLSL

// ---- RGB9E5 (VK_FORMAT_E5B9G9R9_UFLOAT_PACK32): R bits 0-8, G 9-17, B 18-26, E 27-31 ----
// value = mantissa / 512 * 2^(E - 15); largest = 511/512 * 2^16 = 65408.
#define RGB9E5_MAX 65408.0

float rgb9e5Clamp(float x) { return x > 0.0 ? min(x, RGB9E5_MAX) : 0.0; }   // (NaN -> 0)

uint packRGB9E5(vec3 c)
{
    const float r = rgb9e5Clamp(c.r), g = rgb9e5Clamp(c.g), b = rgb9e5Clamp(c.b);
    const float maxc = max(r, max(g, b));
    // floor(log2(maxc)) exactly, from the float's exponent field (maxc >= 2^-126 or 0:
    // anything below 2^-16 lands on the minimum shared exponent anyway).
    const int e2 = maxc > 0.0 ? int((floatBitsToUint(maxc) >> 23) & 0xFFu) - 127 : -127;
    int es = max(-16, e2) + 16;   // biased shared exponent candidate, 0..31 (B = 15, + 1)
    // Mantissa of the largest channel at that exponent; rounding up to 512 bumps it.
    float scale = exp2(float(es - 15 - 9));   // one mantissa step
    if (floor(maxc / scale + 0.5) >= 512.0) { es += 1; scale *= 2.0; }
    const uint rm = uint(floor(r / scale + 0.5));
    const uint gm = uint(floor(g / scale + 0.5));
    const uint bm = uint(floor(b / scale + 0.5));
    return min(rm, 511u) | (min(gm, 511u) << 9) | (min(bm, 511u) << 18) | (uint(es) << 27);
}

// ---- R11G11B10 (VK_FORMAT_B10G11R11_UFLOAT_PACK32): R bits 0-10, G 11-21, B 22-31 ----
// Unsigned small floats, exponent bias 15, no sign; 6-bit (R, G) / 5-bit (B) mantissa.
// mbits = mantissa bits. Largest finite: 2^15 * (2 - 2^-mbits) (65024 / 64512).
uint packUFloatSmall(float x, uint mbits)
{
    if (!(x > 0.0)) return 0u;   // negatives, zero, NaN
    const float maxv = 32768.0 * (2.0 - exp2(-float(mbits)));
    x = min(x, maxv);
    const uint bits = floatBitsToUint(x);
    const int e = int((bits >> 23) & 0xFFu) - 127;
    if (e < -14)
    {
        // Denormal: value = m / 2^mbits * 2^-14. Rounds up to 2^mbits = the smallest
        // normal's encoding (exponent field 1, mantissa 0), which is exactly right.
        return uint(roundEven(x * exp2(14.0 + float(mbits))));
    }
    const uint shift = 23u - mbits;
    const uint m = bits & 0x7FFFFFu;
    uint v = (uint(e + 15) << mbits) | (m >> shift);
    const uint rem = m & ((1u << shift) - 1u);
    const uint half_ = 1u << (shift - 1u);
    if (rem > half_ || (rem == half_ && (v & 1u) != 0u)) v += 1u;   // carry may bump the exponent: correct
    return v;   // (x <= maxv: never reaches the infinity exponent)
}

uint packR11G11B10(vec3 c)
{
    return packUFloatSmall(c.r, 6u) | (packUFloatSmall(c.g, 6u) << 11) | (packUFloatSmall(c.b, 5u) << 22);
}

#endif
