# Horizon water at altitude

The visual ocean is a full-screen background in `clean_sky.frag`, not a disc. Its
intersection distance is `h / -direction.z`, independent of the geometry far clip.
There is no sea radius to enlarge and no change to network draw distance.

The former background applied surface haze to the reflected sky. As transmittance
approached zero, its colour approached surface in-scatter, which differs from the
sky dome's haze integral. Classic altitude thinning affects the sky but not that
surface in-scatter. Physical aerial perspective samples elevation 0.03 rather than
the horizon. These differences can make a second band at the sea/sky join. The old
join also used reversed `smoothstep` bounds (undefined in GLSL) over 0.57 degrees.

The background now uses `T * reflectedWater + (1 - T) * horizonSky`, in linear HDR,
retaining the old extinction and Fresnel reflection. Both sky samples include the
same night/altitude treatment. The join has ordered bounds over 0.057 degrees;
its water weight tends to zero at the sky boundary. The result is continuous there
at any finite camera height, including zero extinction. Geometry fades into this
same background. Orthographic water uses parallel rays and each pixel's origin
height on the eye plane; the live orthographic sky retains its perspective artwork.
Stereo sky water uses each eye's direction and height.

The world remains flat: the sea boundary is the renderer's horizontal sky horizon.
For a spherical Earth of radius 6,371 km the tangent distance would instead be
`sqrt(h * (2R + h))` (about 61.8 km at 300 m, 225.8 km at 4,000 m), with dip
`acos(R / (R + h))`. The existing `spaceLimb` deliberately remaps the physical
atmosphere's dip to the horizontal. Introducing a spherical ocean alone would
separate it from that sky again. This change preserves that convention rather than
introducing planet curvature or changing the atmosphere's artificial height lift.

## Visual checks

1. Use a fixed camera over open sea at night. Sweep camera height through 20, 100,
   300, 512, 1,500, 2,500, 4,000 and 8,000 m. Pitch through the horizon and look
   around all azimuths. The sea should join the sky without a separate dark strip
   or apparent finite edge. A natural sea/sky colour contrast can remain.
2. Repeat with `RenderVKPhysicalSky` 0 and 1 and `RenderVKPhysicalNight` 0 and 1,
   then at midday and sunset. Include a low-haze EEP and a dense-haze EEP. Check
   through the 1,500–4,000 m space transition and after an origin/region shift.
3. Compare draw distances 64, 128 and 512 m with `RenderVKDistanceFade` at its
   default 0.6. Only the local geometry extent should change; the background sea
   should not move. Temporarily use fade 0 to distinguish a geometry cutoff from
   the background join, then restore 0.6. Keep network draw distance <= 512 m.
4. Toggle `FSCameraOrtho`, vary zoom and pitch, and compare opposite screen corners.
   Ocean rays should stay parallel, with no curved or radial ocean edge. Test a
   downward region-map capture too. Return to perspective and check the same view.
5. In VR, check each eye while rolling/tilting the head and moving through water
   level. Check distant geometry fades into the sea in both eyes. Also inspect an
   underwater view and a planar mirror with its reflected eye on either side of
   the water plane.

## Validation scope

Compile shaders with `regen_world_spv.py` into scratch output and a private cache,
limiting `os.cpu_count()` to two for its compile and reflection worker pools. This
checks all registered shader variants, push-constant budgets and shared-memory
budgets. Run the source checks and `git diff --check`. Neither compilation nor the
geometric continuity argument substitutes for the visual checks above; no viewer
build or launch is needed for this source handoff.
