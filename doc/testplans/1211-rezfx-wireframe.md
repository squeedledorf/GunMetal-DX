# Live RezFX wireframe pipelines

The setting is `SLRezFXInEffect`: Tron (1) and Random (7) request wire twins;
all other values request stock world modules. Devices without fragment-shader
barycentrics always use stock modules.

## Integration and lifetime

- `LLCleanRendererVK::rezFxFrameTick`, called from `specFrameTick`, uses the existing
  frame-boundary hook in both LLVulkanContext recording paths, before frame state
  and draw recording. It queues the existing async `PipelineBatchScope` batch at
  user priority, outside the login gate. Workers write only stable staging slots.
- Plain/RT lit and existing debug recipes come from `mLitDbgDesc`. Extra recipes
  retain exact HUD, mirror, full/slim G-buffer and no-terrain G-buffer state.
  Existing overdraw-stat pipelines are rebuilt with their counter specialization;
  first-time overdraw/debug creation waits for a pending mode switch.
  Dedicated capture shaders and terrain-only shaders have no wire twins and stay
  unchanged. No buffers or compute dispatches are added or modified.
- Startup world/RT jobs and any old debug job must land before a switch starts.
  A replacement publishes only after every requested slot succeeds and its mode
  still matches the current request. Failure retains the complete old generation;
  another attempt requires changing the requested mode. Rapid flips discard stale
  results. Published old pipelines use the existing frame retirement lists.
- Publishing detaches the old specialized set. Matching generic pipelines draw
  while the new specialized set compiles. The spec key's field 14 (bits 56–59)
  records module identity, separate from specialization constants and stereo's
  field 15. Loading workers capture this mode by value; they never read settings
  or a mutable global mode. Shutdown joins the batch before freeing staging.

## In-world checks

1. Start with CombatFX off, Tron selected. Confirm loading objects show wireframe
   on a supported GPU. Confirm an unsupported GPU uses the filled fallback.
2. Switch Tron → None → Tron and Fire → Random → Fire without restarting.
   Watch for `RezFX: installed wire twins` / `stock modules` in the log. The old
   world must keep drawing while `RezFX: compiling` is pending.
3. Repeat fast switches while compiling, including returning to the original
   value. Confirm stale completions never publish the wrong mode. Repeat while
   the startup RT group and a debug-view batch are compiling.
4. Check static/rigged opaque and alpha surfaces, both G-buffer VS modes, terrain
   split on/off, mirrors, HUDs, probe capture, debug views and stereo where available.
   Repeat with `RenderVKSpecPipelines` on/off, overdraw stats enabled before/during
   a switch, and an environment/sky change pending.
5. Quit during a switch. If testing injected pipeline creation failure, confirm
   the log says the installed world pipelines are retained, without blank frames.

## A/B GPU cost

Use the existing `RenderVKGpuTimers` output; no viewer was launched for this patch.
`VKGpu`'s five-second GPU windows now include `rezfx_wire=0/1` (installed generic
module generation). Specialized build/bind logs also include their wire key.

Keep camera, scene, resolution, render settings and lighting fixed; let textures,
effects and all pipeline jobs settle. Compare None (0, stock) with Tron (1, twins)
while no objects are arriving or leaving, so this measures the dormant shader
cost rather than the visual work of different effects. Wait for the install log
and specialization to settle; discard the first two timer windows after each
switch. Collect at least six complete windows per state in A/B/A order. Compare
GPU total averages/worst times; with queue overlap enabled, individual pass/sub
deltas are not independent costs. Repeat with specialization off and on, and with
the normal deferred configuration. A fixed batch of newly appearing objects can
separately measure active Tron overhead, but changing effects is not an isolated
measurement of wire-module overhead.
