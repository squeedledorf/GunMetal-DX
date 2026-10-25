# AMD FidelityFX Super Resolution 3.1 (upscaler only), vendored

Used by the Vulkan clean renderer when `RenderVKUpscaler` is 1-6
(`indra/newview/vkgfx/llcleanfsr3vk.{h,cpp}`). Only the FSR 3.1 *upscaler* is here;
frame generation / frame interpolation, the SDK host libraries, the FFX Vulkan backend
and the FidelityFX shader compiler are not used.

## Source

- Upstream: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>
- Tag: `v1.1.4` (FidelityFX SDK 1.1.4), commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- FSR3 upscaler version: 3.1.4 (`FFX_FSR3UPSCALER_VERSION_*` in the SDK's
  `sdk/include/FidelityFX/host/ffx_fsr3upscaler.h`)
- License: MIT, Copyright (C) 2024 Advanced Micro Devices, Inc. -- `LICENSE.txt` (the
  SDK's own file, unmodified). Every vendored file keeps its license header. The notice
  is also in `indra/newview/licenses-*.txt`, which ship with the viewer.

## Layout

| Here | Upstream |
|---|---|
| `gpu/ffx_common_types.h`, `gpu/ffx_core*.h` | `sdk/include/FidelityFX/gpu/` |
| `gpu/fsr1/ffx_fsr1.h` (RCAS) | `sdk/include/FidelityFX/gpu/fsr1/` |
| `gpu/spd/ffx_spd.h` (single-pass downsampler) | `sdk/include/FidelityFX/gpu/spd/` |
| `gpu/fsr3upscaler/*.h` (no HLSL callbacks) | `sdk/include/FidelityFX/gpu/fsr3upscaler/` |
| `shaders/fsr3upscaler/*_pass.glsl` (the 9 Vulkan entry points used; not autogen-reactive) | `sdk/src/backends/vk/shaders/fsr3upscaler/` |

These are exactly the files the passes include (`glslc -M`), plus the SDK's VK entry
points; nothing is fetched at build time.

## How it is built

`indra/vkclean/regen_world_spv.py` compiles the pass entry points with `glslc
--target-env=vulkan1.3 -O` into `vk_fsr3_shaders_spv.h` in the build tree at build time (embedded
SPIR-V, like every other renderer shader), one permutation fixed to the renderer's
inputs (see `FFX_DEFS` there): HDR colour, render-size (low-resolution) unjittered
motion vectors, inverted (reversed-Z) depth, reference Lanczos reprojection (no LUT --
the SDK only uses the LUT on wave32/64 AMD parts), fp32. Two permutations of
`accumulate` (with / without sharpening) and of the two SPD passes (subgroup-quad and
`FFX_SPD_NO_WAVE_OPERATIONS`). The same header also carries our own
`clean_fsr_inputs.comp` and `clean_fsr_probe.comp` (the `RenderVKFSRDebugProbe` numeric
readback, `LLCleanFSRProbeVK`). The host side of `ffx_fsr3upscaler.cpp`
(resources, clears, constants, pass order, dispatch sizes) is re-implemented in
`llcleanfsr3vk.cpp`, which says where it follows the SDK.

## Local patches

In `gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`, marked `SLIPSTREAM PATCH`:

1. `rw_luma_history` declared `rgba16f` instead of `rgba8`: the SDK allocates
   `FSR3UPSCALER_LumaHistory1/2` as `R16G16B16A16_FLOAT`, and a storage image's format
   qualifier must match its view.
2. `rw_upscaled_output` takes an optional explicit format
   (`FFX_FSR3UPSCALER_UPSCALED_OUTPUT_FORMAT`, set to `rgba16f` by the regen script) so
   the accumulate / RCAS / debug passes do not need `shaderStorageImageWriteWithoutFormat`.
   Unset, the declaration is the SDK's format-less one.
4. Finite, bounded colour in and out (`SlipstreamSanitizeRgb`, the rule of
   `clean_taa.comp`'s `sanitize()`: NaN / Inf -> 0, clamp to [0, 65000]) in
   `LoadInputColor`, `SampleInputColor` and `StoreUpscaledOutput`. The SDK only clamps the
   input at 0, so an fp16 overflow (Inf) in the scene became `Tonemap(Inf) = NaN` in the
   upsample and persisted in the history; and a NaN / Inf output reached the bloom and the
   dynamic exposure.

And one in `gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h` (also marked `SLIPSTREAM PATCH`):

3. RCAS runs on `Tonemap(colour * Exposure())` (the header's own reversible
   `max(rgb) + 1` compression, as our `clean_rcas.comp` does) and its result is clamped to
   the per-channel [min, max] of the 5-tap cross it read (same space) before
   `InverseTonemap(...) / Exposure()`, instead of RCAS on linear HDR x exposure.
   - On linear HDR, RCAS's limiter (which bounds its output to [0, peak 1], i.e. SDR)
     degenerates for sunlit values > 1: full-strength sharpening, unbounded overshoot,
     negative undershoot that is clamped to 0 downstream.
   - The first version of this patch (tonemapped space, inverse with a `1 - 1/1024` clamp,
     no cross clamp) still brightened the image: the limiter bounds the *ring's* clipping,
     not the centre's, so a bright texel over a dimmer ring is pushed past 1.0 (sharpness
     0.4: lobe = -0.1875 x 2^-1.2 x 0.5 denoise = -0.041; centre 0.95 over a 0.5 ring ->
     (0.95 - 0.041 x 2) / (1 - 0.163) = 1.04). The clamp + inverse turned that into
     1023 / exposure (exposed HDR 19 -> 1023) -- every centre above
     `1 - 0.163 x (1 - ring)` (ring 0.5: exposed HDR > 11). Those fireflies fed the
     bloom (halos, haze) and the dynamic exposure's 256-tap mean (the rest of the frame
     darkened), and the convex inverse also raised the mean of sharpened bright texture.
     With the cross clamp the output never leaves the input's own local range.
   Only the non-paired (`FSR_RCAS_F`) path is patched; the paired FP16 path is not
   compiled here.

## Updating

Copy the same file list from a newer SDK tag (FSR 3.1.x; the SDK 2.x line ships FSR 4 /
DX12 only), re-apply the four patches, diff `ffx_fsr3upscaler.cpp` against
`llcleanfsr3vk.cpp` (constants struct, resource formats, clears, pass order), update the
tag / commit above, and rebuild (the build regenerates the header).
