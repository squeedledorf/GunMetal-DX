# GunMetal-DX

**A Second Life viewer built for combat.**

GunMetal-DX is a fork of the [Slipstream Viewer](https://git.slipstreamviewer.org/sky_line/Slipstream) and keeps
its GPU-driven Vulkan renderer. On top of it, GunMetal-DX adds the tools that SL combat sims are played with:
an over-the-shoulder aim camera, aim-down-sights, hit markers and a kill feed, a minimap that shows who is
who, an MMO-style chat log, and controller support.

> This software is not provided or supported by Linden Lab, the makers of Second Life, and is not affiliated
> with or endorsed by the Slipstream project. Please report GunMetal-DX problems
> [here](https://github.com/squeedledorf/GunMetal-DX/issues), not to Slipstream.

**Status:** beta. Linux x86_64 and Windows x64.

## Combat

<!-- GIF: docs/readme/ots-ads.gif -->
- **Over-the-shoulder aim (OTS).** A third-person shooter camera with a true-aim dot that shows where a shot
  will actually land, aim convergence, and a custom crosshair.
- **Aim down sights.** Zoom with a vignette and its own mouse sensitivity.
- **Fair by design.** Anything that changes what the region is told about your aim is fixed in code, with no
  setting to bend it. The only options are purely local visuals, like the crosshair and the ADS look.
  Nothing that would show avatars through walls ships.

<!-- GIF: docs/readme/hitmarkers-killfeed.gif -->
- **Hit markers, hit and kill sounds, and a kill feed**, read from Second Life's Combat 2.0 events through the
  GunMetal-DX helper.
- **Group colours.** Pick a colour for a group, and its members show in it on the minimap, in the kill feed,
  on name tags and in the crosshair identification.

## The map

<!-- GIF: docs/readme/minimap-region.gif -->
- **Whole Region minimap.** The whole region fills the map, with a choice of compass styles, zoom and pan.
  **Only Render Physical Objects** shows bullets and vehicles without the clutter of every prim.

<!-- GIF: docs/readme/region-capture.gif -->
- **Region map capture.** A perfect orthographic picture of the region, used as the minimap's background.
  Water is drawn in a clean map style. Maps can be shared as a file or as an in-world texture.

<!-- GIF: docs/readme/marks-zones.gif -->
- **Marks and zones.** Drop named pins, and draw rectangles, circles and polygons with solid or dotted outlines,
  plain or caution-stripe fills. Use them for objectives, spawns and no-go areas. Hover the map for
  coordinates, or copy any spot as text or as a SLURL.

## Chat and interface

<!-- GIF: docs/readme/chat-log.gif -->
- **Chat log.** One log in the corner instead of floating toasts:
  - fades at rest;
  - opens a tabbed panel while you type (All, Local, Group, IM, Combat);
  - drops to three faint lines in mouselook and while aiming.
  Names take group colours, and repeated object spam collapses into one line.
- **Rez effects.** Optional rez-in and de-rez effects. They never hide or delay an object, and objects that
  only exist briefly never play one.
- **Selection styles.** Classic outlines, a screen-space outline, or a wireframe.

<!-- GIF: docs/readme/controller.gif -->
- **Game controllers**, including the 2026 Steam Controller with its trackpads.

Everything Slipstream brings comes too: hardware ray-traced shadows, Photo Studio, cascaded shadow maps, and
the rest of the [Slipstream feature list](https://slipstreamviewer.org/wiki/features/).

## Requirements

- **Linux:** x86_64, glibc 2.38 or newer (for example Ubuntu 24.04, Pop!_OS 24.04, Fedora 39+, Arch).
- **Windows:** Windows 10 or 11, x64.
- **GPU:** a Vulkan 1.3 driver. That means NVIDIA GTX 750 / 900 series or newer, AMD RX 400 series or newer, or
  Intel HD/UHD 500 / 600 series or newer. Ray-traced features need `VK_KHR_ray_query` (RTX 20-series+,
  RX 6000+).
- **Linux parcel media:** the GStreamer plugin sets (`gst-plugins-good`, `gst-libav`, `gst-plugins-bad`,
  `gst-plugins-ugly` on Arch; `gstreamer1.0-plugins-good`, `gstreamer1.0-libav` and friends on Ubuntu).

## Download and updates

Download from [Releases](https://github.com/squeedledorf/GunMetal-DX/releases):

- **Windows:** run `GunMetal-DX_<version>_Setup.exe`. It installs for your user only, so there is no admin
  prompt. `GunMetal-DX_<version>_win64.zip` is the same viewer without an installer.
- **Linux:** extract `GunMetal-DX_<version>_x86_64.tar.xz` anywhere you can write to and run `./gunmetal-dx`.

After the first login, GunMetal-DX asks whether it may check GitHub for updates: check and install, check and
only tell you, or never check. You can change this later in Preferences > Setup. Updates are signed, and the viewer
installs only a signed update downloaded from this repository's releases.

## Crash reports

If GunMetal-DX crashes, it asks the next time it starts whether to send the crash report to the developers
through [BugSplat](https://www.bugsplat.com/). The choices are Send, Always send or Don't send. A report holds
the crash dump and the end of the viewer's log, which includes your avatar name and recent regions. Nothing is
sent without your answer.

## Building

GunMetal-DX builds on Linux; the Windows version is cross-compiled from Linux with clang-cl and xwin, as
Slipstream's is.

```sh
make            # venv + dependencies + configure + build + package
make run        # launch the packaged viewer
make windows    # cross-build the Windows x64 package
make help       # every target and override
```

See Slipstream's [building page](https://slipstreamviewer.org/wiki/building/) for the package lists and the
Windows toolchain setup.

## Credits and license

GunMetal-DX is a fork of the [**Slipstream Viewer**](https://git.slipstreamviewer.org/sky_line/Slipstream) by
backspace119 and the Slipstream contributors ([website](https://slipstreamviewer.org)). Its renderer, and most
of what is here, is their work. Each GunMetal-DX source release says which Slipstream commit it is based on.

Slipstream is built on [Linden Lab's Second Life viewer](https://github.com/secondlife/viewer) and the
[Alchemy Viewer](https://github.com/AlchemyViewer/Alchemy), and includes RLVa. It also ports features from:
- [Firestorm](https://www.firestormviewer.org/);
- [Black Dragon](https://github.com/NiranV/Black-Dragon-Viewer).

GunMetal-DX's own combat features build on work from SoapStorm and Firestorm, including the kill feed, hit
markers and group viewer, and on the Exodus viewer's combat identification.

Original copyright headers are kept in the ported files. The source is licensed under the LGPL 2.1; see
`doc/LICENSE-source.txt` and `doc/LGPL-license.txt`. The viewer artwork notice is in `doc/LICENSE-logos.txt`.
Second Life and related marks belong to Linden Research, Inc.
