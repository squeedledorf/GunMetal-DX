# Native floater windows (#1013)

Implementation status: source implementation; runtime acceptance remains pending.
No full viewer build, viewer link, package or runtime test was performed. Internal
cargo transfer is routed where SDL delivers cross-window events; Wayland implicit
grabs can keep those events on the source surface (see platform limits below).

## Boundaries

`llwindow/llnativewindow.*` owns native lifecycle, bounded registration, stable
64-bit IDs, placement policy and surface event dispatch. It contains no LLUI or
UI renderer types. SDL coordinates are logical window units; framebuffer extent,
pixel density and content scale remain distinct. `LLNativeSurface` is the toolkit
adapter contract.

`vkgfx/vk_ui_surface.h` supplies a renderer-neutral frame target: identity, command
buffer, color image/view, extent, format, layout, DPI, and exact common graphics
submission/completion serials. Main, native and VR UI targets use that contract.
Recorders own no WSI and submit nothing. Main's optional backdrop source grants
transfer-source usage only. The native implementation contains no UI middleware
headers. Existing GPU-driven world work is unchanged.

`LLFloaterPopout` is the LLUI adapter. It owns per-window view roots and the scoped
legacy UI globals, translates input, reparents the same floater, and persists its
presentation. Popup menus, combo popups and tooltips follow the active root.

## Implemented paths

- The title-bar button and title-bar context menu request native presentation of
  inventory, people, conversations, detached IMs, script/notecard editors, maps,
  script/region consoles, statistics and frame timing floaters. Hosted tabs must
  first be torn out of their container; dependent dialogs are not eligible.
- The same `LLFloater` moves into a native root, with its original parent,
  rectangle, follows flags, drag/resize/minimize behavior retained for docking.
  OS close docks; the internal close button retains the floater's normal close
  and unsaved-content behavior. Close All and quit include native floaters.
- Each window owns a Vulkan surface, swapchain, command pool, two command buffers
  and synchronization objects. The existing LLRender UI path replays its view
  tree without a world pass. Texture/pipeline storage is shared; vertex slots are
  separate and released after the closed window retires. A graphics timeline gates shared
  texture/upload retirement across independent swapchains.
- SDL events route by window ID. UI keyboard dispatch gives the focused control
  and editing commands first refusal and never enters the world held-key scanner.
  Text input, composition, caret placement, clipboard, pointer capture and wheel
  input use the native window's UI context and display scale.
- Geometry, monitor name, maximize and pop-out state persist in the bounded
  `NativeFloaterWindows` map. Monitor-relative placement is clamped on Windows and
  X11. Wayland restores size and lets the compositor place the window. A missing
  monitor leaves/docks the floater in the main viewer.
- Resources are allocated on demand, capped at 16 native windows and 128 saved
  identities (32 coalesced pending requests). The native swapchain estimate is
  capped at 128 MiB/window and 256 MiB aggregate, including retiring windows;
  auxiliary vertex rings are capped at 16 MiB each. These limits bound application
  objects/estimates, not presentation-engine residency or retirement time.
  Unfocused windows update at 10 Hz, focused windows at up to 60 Hz.
  No auxiliary surfaces, vertex buffers, timeline markers or render loops exist
  on the unused path.
- With optional swapchain maintenance support, resize/close drains GPU submissions,
  outstanding acquisitions and presentation fences by polling; there are no
  auxiliary device-idle or queue-idle calls. A fixed present-proof pool is reserved
  before acquisition. At capacity, skip that auxiliary update.
- Without maintenance support, pop-outs still work. Rare retire events are batched
  into one presenting-queue idle call, with path and duration logs. This is the
  approved conventional WSI-release assumption, not an exact presentation-engine
  proof. It may delay main rendering when queues are shared. Steady frames do not
  wait this queue. Failed waits and unsignaled acquire fences do not authorize free.
  A refused fallback wait backs off and stops after three attempts, retaining the
  bounded objects until exit; ambiguous maintenance present results are also
  retained. On either path, native window/surface/slot ownership survives until
  safe retirement.
- Main swapchain teardown also tracks present fences when maintenance is enabled
  device-wide. A shutdown with pending auxiliary presentation retains the bounded
  context until process exit instead of freeing objects on a timeout.

## Lightweight checks

From the repository root, with a temporary output directory:

```sh
python3 -I scripts/check/run.py --stage source
g++ -std=c++20 -Wall -Wextra -Werror indra/newview/tests/nativewindowgeometry_test.cpp -o /tmp/nativewindowgeometry_test
/tmp/nativewindowgeometry_test
g++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers indra/newview/vkgfx/tests/vk_swapchain_ui_test.cpp indra/newview/vkgfx/vk_swapchain.cpp -o /tmp/vk_swapchain_ui_test
/tmp/vk_swapchain_ui_test
```

The geometry test covers Windows/X11 versus Wayland coordinate density,
fractional/asymmetric scaling, off-window capture coordinates, negative monitor
origins and malformed/oversized saved rectangles. The mocked WSI test covers
independent present policy, nonblocking acquire, per-image semaphores,
out-of-date/suboptimal handling, isolated surface loss, allocation cleanup,
GPU/acquire/present proof separation, abandoned acquisitions and wait-free fast
retirement. A refused proof or pending acquire cannot free native resources.
Neither test starts SDL or uses a GPU.

## Runtime acceptance still required

Run on Windows, X11 and native Wayland, including a pair of monitors with different
scale factors. Use the normal viewer validation and separate synchronization
validation runs after the integrating agent builds this branch.

1. Open each candidate, type/filter/edit before detaching, and verify that content,
   selection, undo, callbacks and tabs survive detaching and docking. Verify an
   actual populated view tree renders and remains interactive in each OS window.
2. Move, resize, maximize, minimize and restore several windows independently.
   Check taskbar/title/icon, glyph placement, clipping, pointer hit targets,
   scrolling, menus, clipboard, dead keys, AltGr, CJK composition and IME candidates.
   Check Enter/Tab/editing commands and that typing never moves the avatar.
3. Close the OS window (dock), close internally (normal close), cancel unsaved
   script/notecard close, use Close All, then quit/cancel quit with dirty editors.
   Application-owned notification prompts can still appear in the main window.
4. Restart/reopen; verify each instance's saved state. Reconfigure the display
   arrangement, unplug the saved monitor, and repeat. Wayland must make no request
   to restore absolute coordinates. Identically named displays need special
   attention because SDL monitor names are not guaranteed unique persistent IDs.
5. Minimize the main viewer and continue editing native windows. Exercise different
   idle/active rates across several windows under sync validation; verify no texture
   or staging-buffer release occurs before the queue completion marker. Stress
   repeated create/destroy, resize/out-of-date, surface loss and allocation refusal.
6. Verify both retirement capabilities: on a device that exposes maintenance,
   resize/close repeatedly without explicit idle waits; without it, verify one
   INFO duration line per batch and no fallback waits in steady frames. Force
   swapchain allocation refusal, close while minimized, move between mixed-DPI
   displays and stress the 16-window admission limit including hidden retirees.
7. Drag inventory cargo between the main window, IM and inventory pop-outs in both
   directions. Existing cargo semantics and protections must remain in force; use
   an alt for any action that sends an item. Windows/X11 event routing is implemented
   but unverified. Wayland's implicit pointer grab may retain the source surface:
   a Wayland drag-source protocol bridge would be needed to support that compositor case.

Fonts currently share the main window's raster atlas, scaled as a whole for the
target monitor. Geometry/input scaling is independent; atlas sharpness at a higher
native DPI is not equivalent to a separate per-window font registry. Tooltips,
menus and combo popups are scoped to each root; application-owned notifications
and dialogs outside the floater subtree retain their existing ownership. Display
names are not guaranteed unique persistent monitor IDs. These limitations and the
compositor-dependent cargo path need runtime/product review before acceptance.

## Browser and Studio follow-up

### Native hover regression (#1013)

Each native root now uses the same front-to-back hover collection as main,
including transparent siblings, capture ancestors, popups and the top control.
Weak-handle enter/leave snapshots preserve Inventory's enter-before-leave order;
OS leave, focus changes, hide/minimize and docking drain the owning sets. Hover
is also recomputed at the host's UI cadence, so scrolling under a stationary
pointer can retire the old row. Native selection/slider capture ends on focus
transfer even when the destination is another viewer window.

Runtime checks (not executed by the source-only recovery worker):

- Sweep Inventory rows/folders in two native windows, then main. Only the current
  row may remain hovered; leave to desktop, alt-tab, minimize and dock while over
  a row. Selection must remain intact while hover/title flags clear.
- Repeat with buttons, tab buttons/arrows, scroll-list rows/cells, combo popups
  and tooltips. Scroll/filter/delete the hovered row without moving the pointer.
  Open an opaque popup above it; transparent ancestors must not mask its leave.
- Drag a slider or text selection beyond the client edge and release; move focus
  to another native window/main mid-drag. No stale capture, pressed button or
  tooltip may persist. Inventory cargo still uses its separate cross-root path.
- Close/destroy a hovered control or popup from an enter/leave callback, then
  dock/close its host. Weak handles must skip destroyed controls without UAF.

Browser candidates now include Web Content, Media Browser, Help, Search,
Destinations, Marketplace, Web Profile and My Web Profile. They use the same
media textures and bounded Vulkan UI upload/cache path as main, drawn into the
auxiliary swapchain by the existing LLUI adapter. Photo Studio History can also
detach: it edits the existing undo stack and has no world viewport.

Photo Studio, its section panes, Pose Studio and Pose Creator now also detach.
They retain their existing models, controls and undo stacks. The main window
remains the world viewport; native windows do not acquire a camera or dispatch
world-tool input. Explicit white-balance picking, reference-frame capture and
Creator preview restore/raise main, using its existing renderer and tool path.
Other settings and pose edits can be made while main is minimized, with their
world effect visible on restore. OS focus-stealing policies may require the user
to activate main after a raise request.

Main-specific operations use `LLWindowSDL::getMainSDLWindow()`, whose identity
is set on SDL creation and cleared on teardown. `getSDLWindow()` and ordinary
window services intentionally follow the active `ScopedUIWindow` target. The
Studio actions and paused-frame query must never use that scoped target or
temporarily restore global UI context to reach main.

The earlier six exclusions were audited individually:

| Floater | Source dependency and disposition |
| --- | --- |
| Photo Studio | `SLFloaterPhotoStudio::pickWhiteBalance` arms `SLToolScenePicker`; `SLPanelPhotoStudioSection` queues `requestReferenceStill`. Both explicit world actions now restore/raise main. Settings, curves, effects, haze and history remain ordinary UI/model operations. Enabled. |
| Photo Studio pane | `SLPhotoStudioSlot` hosts the same sections, including `SLPanelPhotoStudioScopes::updateReadouts`, which previously interpreted scoped native mouse coordinates as a world sample. Native scopes now show aggregate main-frame data without that invalid pixel lookup, and identify the retained frame while main is minimized. Existing section/put-back/close callbacks remain authoritative. Enabled. |
| Pose Studio | `SLPoseFigureCtrl` is a 2D bone control; editing callbacks use `SLPoseStudio` joint data. `SLPoseIK::pick`, `pickRing` and `pickMove` require main's world projection, but are dispatched only by main's viewer input. Native input never enters that path, and main's early native-hover return now clears IK hover and pointer-over-world state. Enabled. |
| Pose Creator | `onPreview` plays a local animation on the target avatar through `LLLocalAnimMgr`; it does not draw an embedded world view. Successful preview now restores/raises main. Keyframe edits/export and weak-handle save callbacks remain in the existing model. Enabled. |
| Snapshot | `postBuild` places `LLSnapshotLivePreview` below `gSnapshotFloaterView` in main's root; `draw_snapshot_crop_frame` and countdown placement consume main world/window dimensions. `ImplBase::updateResolution`, thumbnail generation and `LLSnapshotLivePreview::onIdle`/`rawSnapshot` also depend on the main capture contract. Native `Context` replaces window sizes/root/scale during control callbacks. Moving just the controls can therefore change capture dimensions and misplace crop/countdown/freeze input. **Must stay docked** until capture geometry and preview ownership are explicitly separated from UI-host geometry, with timer expiry serviced against main rendering while minimized. |
| Simple Snapshot | `postBuild` creates a full-screen `LLSnapshotLivePreview`, `Impl::updateResolution` clamps HUD dimensions through `gViewerWindow`, and `onOpen` still uses `gSnapshotFloaterView`. It shares thumbnail/raw capture with Snapshot and may also be a dependent outfit dialog, which native admission intentionally rejects. **Must stay docked** until main capture geometry/continuation and dependent-dialog ownership are explicit; the simpler controls do not eliminate those dependencies. |

The two Snapshot exceptions need capture/preview changes beyond this UI-host
layer; source-only capability flags would hide broken behavior. Their timer,
filename and capture implementation is unchanged. No independent world rendering
or camera ownership was added. Application-owned file/upload dialogs can still
open in main, as with the existing native editors.

Browser IME supports native candidates and committed Unicode, with candidates
anchored near the last click inside the browser's own window. Inline CEF preedit
and exact DOM caret tracking are unsupported by the current media protocol.
Page-script geometry requests are ignored while detached; native resize controls
the content size. Application prompts/file dialogs outside the floater tree keep
their existing ownership. These are limitations, not runtime acceptance claims.

After integration builds, run these additional checks on Windows, X11 and Wayland:

1. Open each of the eight browsers with real media content. Detach, navigate,
   resize and dock without losing page state. Confirm actual page pixels, dynamic
   page/video updates and scrollbars in each native window. Minimize main while
   leaving two browsers visible; focus either one, then another application.
   Visible native media must not inherit main's hidden priority. Docked UI and
   in-world media must keep their existing minimize policy.
2. Drag between monitors with different DPI, change UI scale, maximize and resize
   rapidly while the page changes size asynchronously. Click links and select
   text near all four corners; verify vertical/horizontal wheel direction,
   fractional trackpad scrolling, capture outside the window and letterboxing.
   Native size/DPI must determine texture size, with no resize feedback to main.
3. Test ordinary typing, Shift, AltGr, dead keys, CJK candidate selection and
   non-BMP commits in a form. Check Enter/keypad Enter submits once, Tab traversal,
   arrows, cut/copy/paste/select all and browser address-bar editing. Candidate
   navigation must not invoke page/app commands. Begin composition in one window,
   switch to another browser or main's editor, then commit/cancel: text must not
   leak to the other root. Verify the documented candidate anchor limitation.
4. Minimize/restore the native browser itself; close/dock during composition and
   while main is minimized. Restore main, switch LL tools and TPS, then type in
   the browser: no held movement, camera rotation or extra camera owner may result.
5. Detach History with existing edits. Undo/redo and jump to a step; resize/filter
   the list, minimize main, and repeat. Close Photo Studio and confirm History
   closes safely. Reopen it and verify the same model remains authoritative.
6. Detach Photo Studio and several section panes; change layouts/sections, curves,
   effects, haze, Looks and undo/redo. Close/reopen Studio with native panes and
   History alive; verify put-back and persistence. Resize/move across DPI and
   exercise cached scope menus. Minimize main: scopes must identify the retained
   frame; native mouse positions must never appear as world RGB readings.
   White-balance pick/reference grab must restore main and use its world view.
   Specifically keep the native Studio visible while main is minimized, then
   invoke each action from its native callback. Main must restore/raise while
   the native control tree retains its context. A visible native scope must
   show the paused-frame label when main alone is minimized; minimizing only
   the scope must not label the running main view paused after scope restore.
   Detach Pose Studio and Creator: use the 2D figure, sliders, presets, IK undo,
   keyframes, export and preview. Preview must restore main. IK ring picking and
   camera/LL tool/TPS interaction stay in main, with no lingering hover/capture
   after focus changes. Verify edited models survive docking and close semantics.
   Confirm Snapshot and Simple Snapshot have no native pop-out action.
7. Check Snapshot's existing save countdown, cancel on hide/minimize/close, filename
   field and save naming. The follow-up changes only the capability attribute in
   its XML; the integrated countdown and filename implementation must remain intact.
8. Stress the native admission/memory limits and refuse a media texture allocation.
   The failed initial upload must remain retryable rather than caching texture 0;
   a later frame can recover without reopening the browser. Repeat resize/close
   under synchronization validation using both WSI retirement capabilities above.

Static validation includes all shipped skins/localizations of these fifteen
floater filenames and the two profile files' `filename` inheritance. Runtime,
Windows compilation and GPU validation remain required before acceptance.
