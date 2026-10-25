# Bundled starter effects -- third-party notices (#957)

Slipstream ships a small, curated set of ReShade FX effects so a fresh install has
something useful in Photo Studio > Effects. The files are copied **unmodified** from
the upstream releases below. They are read-only here: put a file of the same name in
your own effects folder and yours is used instead (first folder wins by file name).

| Pack | Source | Pinned commit | Licence |
|---|---|---|---|
| SweetFX | https://github.com/CeeJayDK/SweetFX | `93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361` (master, 2026-09-26; the repo has no tags) | MIT, Copyright (c) 2014 CeeJayDK -- `Shaders/SweetFX/LICENSE.txt` |
| prod80 | https://github.com/prod80/prod80-ReShade-Repository | `1c2ed5b093b03c558bfa6aea45c2087052e99554` (master, 2023-01-05; the repo has no tags) | MIT, Copyright (c) 2020 prod80 -- `Shaders/prod80/LICENSE.txt` |

The same notices are in the About box (licenses.txt).

## Audit rule

The repo LICENSE applies to a file unless its header names a different licence, or a
different author without stating terms, or credits code under another licence. Nothing
under `Textures/` is shipped (no texture terms to clear), so anything that needs an
external texture, or a shared header that does, is out. `ReShade.fxh` (CC0) and
`ReShadeUI.fxh` (Slipstream's own) are in `../Shaders`, not in this pack.

## Shipped (10 files)

| File | Evidence |
|---|---|
| SweetFX `Curves.fx`, `Levels.fx`, `Vibrance.fx`, `Tonemap.fx`, `LumaSharpen.fx`, `Vignette.fx`, `FilmGrain.fx` | header: "by Christian Cann Schuldt Jensen ~ CeeJay.dk"; no other author or licence named; repo MIT |
| SweetFX `Monochrome.fx` | header: "Author: CeeJay.dk, License: MIT" |
| prod80 `PD80_04_Color_Balance.fx`, `PD80_05_Sharpening.fx` | header: "Author: prod80 (Bas Veth), License: MIT, Copyright (c) 2020 prod80" with the MIT text; no `PD80_*.fxh` include, no textures, no other credits |

## Excluded

SweetFX:
- `CRT.fx`: GPL-2.0-or-later (cgwg, Themaister, DOLLS)
- `FXAA.fx`, `FXAA.fxh`: NVIDIA all-rights-reserved notice
- `SMAA.fx`, `SMAA.fxh`, `AreaTex.png`, `SearchTex.png`: other authors (Jimenez et al.), textures; redundant with the built-in AA
- `CAS.fx`: Copyright AMD (MIT, but a separate notice; not needed for the starter set)
- `Layer.fx`, `Layer.png`: texture
- `DPX.fx` (Loadus), `Technicolor.fx` (DKT70), `Technicolor2.fx` (Prod80 original), `LiftGammaGain.fx` (3an), `Border.fx` (Oomek): other authors, no terms in the file
- `Template.fx`: blank licence field
- `ASCII`, `Cartoon`, `ChromaticAberration`, `ColorMatrix`, `Compare`, `FakeHDR`, `Nostalgia`, `Sepia`, `Splitscreen`: left out to keep the set modest (novelty or redundant); not audited for this pack

prod80:
- `PD80_04_Color_Temperature.fx`: credits CC-BY-3.0 code (shadertoy, Renaud Bedard)
- `PD80_04_Technicolor.fx`: credits a StackExchange snippet (CC-BY-SA)
- `PD80_04_BlacknWhite.fx`: credits shadertoy 'iq' without terms
- every effect that includes `PD80_00_Noise_Samplers.fxh` (needs `pd80_bluenoise*.png`, `pd80_gaussnoise.png`), `PD80_06_Film_Grain.fx` (`pd80_permtexture.png`), and the LUT effects / textures: textures without stated terms
- every effect that includes `PD80_00_Color_Spaces.fxh` (collected from lolengine / Bruce Lindbloom with mixed provenance), `PD80_00_Blend_Modes.fxh` or `PD80_00_Base_Effects.fxh`
- the rest: left out to keep the set modest

Not bundled by policy (user-installed only): qUINT, iMMERSE, Lilium HDR, crosire `reshade-shaders` samples.
