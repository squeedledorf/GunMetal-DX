# Slipstream effects (ReShade FX) -- notes for effect authors

Slipstream runs unmodified ReShade FX effects (`.fx` / `.fxh`) inside its own Vulkan
renderer. Put them in `<user data folder>/effects/Shaders` (textures in
`effects/Textures`), or add an existing `reshade-shaders` folder as a search path in
Photo Studio > Effects. The compiler is ReShade's own (v6.8.0); `__RESHADE__` is 60800.

## Where effects run

- **Display point** (default): tonemapped, display resolution, after FXAA / SMAA,
  before RLVa vision restrictions, selection outlines, name tags, HUD and UI.
  `BUFFER_COLOR_SPACE 1`, `BUFFER_COLOR_BIT_DEPTH 8`.
- **HDR point**: the linear scene after bloom, before the tonemapper, exposure-scaled
  (values above 1 are real). `BUFFER_COLOR_SPACE 2`, `BUFFER_COLOR_BIT_DEPTH 16`, back
  buffer RGBA16F. Choose it per effect in the Effects page, or per technique with the
  annotation `technique MyTech < slipstream_point = "hdr"; > { ... }`.

## Depth

`DEPTH` (ReShade.fxh's `DepthBuffer`) is display resolution and **unjittered** (no TAA
shimmer), reversed, and encoded so `ReShade::GetLinearizedDepth()` returns view depth /
`RESHADE_DEPTH_LINEARIZATION_FAR_PLANE`, which Slipstream sets to the camera's far clip
(draw distance, rounded up to 64 m). The `RESHADE_DEPTH_INPUT_*` switches are fixed by
the viewer; preset values for them are ignored. Multiply linearized depth by
`RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` to get metres. The sky is linear depth 1.

## Slipstream extensions (`#if __SLIPSTREAM__`)

`__SLIPSTREAM__` is the extension level (1). Include `Slipstream.fxh` for ready-made
declarations, or declare textures with these semantics yourself:

| Semantic | Format | Contents |
|---|---|---|
| `SLIPSTREAM_NORMAL` | RGBA16F | view-space unit normal (xyz; -z forward, +y up); w = 1 pre-pass normal, 0.5 reconstructed from depth |
| `SLIPSTREAM_MOTION` | RG16F | motion vector in UV units, current - previous, unjittered (0 without temporal AA) |
| `SLIPSTREAM_AVATAR_MASK` | R32F | avatar id per pixel (slot + 1; 0 = no avatar) |
| `SLIPSTREAM_SCENE_HDR` | RGBA16F | linear HDR scene x exposure |

All are display resolution and unjittered. Unknown semantics bind black.

## Scope

Any effect can be limited to **avatars only** or **everything except avatars** in the
Effects page (with a soft edge). The viewer keeps the image from before the technique
and blends the result back by the avatar mask -- the effect itself needs no changes.

## Snapshots

Effects run on snapshots at the snapshot's size (recompiled for it: `BUFFER_WIDTH` /
`BUFFER_HEIGHT` are compile-time) and once per supersampled snapshot. The `screenshot`
uniform source is true on that frame; techniques with `enabled_in_screenshot = false`
are skipped. 360 captures never run effects (seams).

## Uniform sources

`timer`, `frametime`, `framecount`, `date`, `random`, `pingpong`, `key`, `mousepoint`,
`mousebutton`, `mousewheel`, `bufready_depth`, `screenshot`. `overlay_open`,
`overlay_active`, `overlay_hovered` and `mousedelta` are always 0.

## Limits

2D textures only (no 1D / 3D); `source` files: png, jpg, bmp, tga (decoded to RGBA8,
resized to the declared size); texture edge 16384; 16 mip levels; 64 passes per
technique. A broken effect is disabled with an error; an effect that takes the GPU down
is quarantined on the next launch until you enable it again.
