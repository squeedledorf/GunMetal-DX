# Orthographic RT shadows and view directions

The branch keeps five independent changes: A+B receiver precision/history,
C receiver eyes, D alpha footprints, E cluster selection/lift, and material/view
directions. Perspective branches retain the original arithmetic. No viewer was
launched; visual equivalence and shadow quality need the following in-world checks.

## Shadow checks

Use a fixed camera, avatar pose and sky. Record original settings before testing
and restore them afterwards. Compare perspective with orthographic mode at the
same location. Keep the network draw distance at or below 512 m.

1. A: `RenderVKRTShadowDebug=11` highlights self-hits within 5 cm in red. Inspect
   the avatar's camera-facing face, chest and limbs. Orthographic mode should not
   introduce red blotches absent in perspective. A temporary
   `RenderVKRTShadowBias=0.001` provides the investigation's comparison; restore the
   original bias when assessing the fix. Check close contact shadows remain.
2. B: `RenderVKRTShadowDebug=3` shows history length; `=6` shows history rejection.
   A stationary orthographic receiver should accumulate history rather than stay
   black in mode 3 or continuously red in mode 6. Exercise zoom, camera movement,
   projection switches and temporal AA on/off. Check merged/projector shadows too.
3. C: compare `RenderVKRTBackfaceSelfShadow=1/0` on outward-facing avatar rims at
   the centre and sides of the frame. Check visible-side normals and dark bands.
   Exercise RTAO, merged sun shadows, queued light visibility, mirrors and probes.
4. D: compare alpha-masked hair, lashes, clothing and shell grass at several
   orthographic depths and zoom levels. Their shadow alpha footprint should be
   independent of depth. Check native and upscaled rendering, anisotropic and
   isotropic shadow LOD, and perspective probe/mirror captures.
5. E: first record `RenderVKClusterLOD`. With it off, E has no effect. With it on,
   compare legacy versus cluster avatars and inspect the periodic cluster RT log:
   a live ortho camera should no longer report `no main-view selection this frame`.
   Toggle cluster LOD off to distinguish cluster mismatch from A–D. Check alpha
   ordering and the region map: region-map captures keep their existing selection
   flags and remain excluded from main RT/lift bookkeeping.

## View direction checks

Check specular highlights, Fresnel and reflections across the whole orthographic
frame, with GI off/on, on PBR and legacy materials, particles and RezFX. View rays
are parallel in the main ortho view. Captures and secondary reflection-hit shading
retain their own incoming rays. GI cache-level distance policies, haze and fog
distance policies are unchanged. The blend receiver's eye direction is corrected;
the separate perspective-based blend depth-reconstruction model is outside these
RT shadow fixes.

Compare perspective captures and GPU timings against the baseline. Runtime pixel
comparison remains necessary; shader compilation alone does not prove pixel
equivalence across drivers.
