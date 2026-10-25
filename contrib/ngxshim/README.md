# slipstream_ngx: optional NVIDIA DLSS for Slipstream

This folder is a small, **separate** project, licensed under MIT (see `LICENSE`). It is not
part of the viewer build, and the viewer does not need it. When you build it yourself and
put it next to the viewer, the viewer can use two NVIDIA DLSS features:

- **DLSS Super Resolution** (SR): upscaling and anti-aliasing (DLAA at native size). The
  cheaper network, for more frames per second, with or without ray tracing.
- **DLSS Ray Reconstruction** (RR): a combined denoiser and upscaler that replaces the
  viewer's own ray-tracing denoisers. Worth it with ray-traced reflections, GI or ReSTIR
  lighting on; with ray tracing off it costs more than SR and gives nothing back.

In Preferences > Graphics > Advanced > Upscaling, pick one of the "DLSS" modes, then set
**Ray Reconstruction** to Auto (RR while a noisy ray-traced effect is on, SR otherwise), On
or Off (always SR).

Why it is separate: the DLSS SDK's licence does not allow the SDK to become part of an
open-source-licensed work. The viewer is LGPL 2.1, so it contains no NVIDIA code, headers or
binaries. It only loads `libslipstream_ngx.so` / `slipstream_ngx.dll` at run time through the
plain C interface in `include/slipstream_ngx.h` (the "slip_nr" ABI). If the library is
missing, or it fails, the option shows as unavailable and says why.

**You accept NVIDIA's licence by downloading the DLSS SDK.** Read `LICENSE.txt` in the SDK
before you build. Nothing from the SDK is stored in this repository.

## Quick start (one command)

1. Clone NVIDIA's SDK (this is where you accept its licence): `git clone https://github.com/NVIDIA/DLSS`
2. Run the script from a Slipstream checkout, giving it the SDK folder:

   - Linux: `contrib/ngxshim/build.sh ~/DLSS`
   - Windows (PowerShell): `powershell -ExecutionPolicy Bypass -File contrib\ngxshim\build.ps1 C:\src\DLSS`

3. Restart the viewer and pick a "DLSS" mode in Preferences > Graphics > Advanced > Upscaling.

The script checks that you have the tools (CMake, a C++ compiler or Visual Studio 2022 / 2026, the
Vulkan headers) and says what is missing. It builds the shim in a temporary folder and
installs it, plus the DLSS runtime libraries from your SDK's `rel` folder, into the `dlss`
folder of the viewer's data folder (`~/.slipstreamviewer/dlss` on Linux,
`%APPDATA%\SlipstreamViewer\dlss` on Windows). The viewer looks there by default, so you
set nothing. A second argument installs somewhere else; the script then prints the
`RenderVKDlssShimPath` value to set. Running it again (for example with a newer SDK) is safe:
files are written to a temporary name and renamed into place, so it never changes a library a
running viewer has loaded. These scripts are for your machine only: they are not run by CI and
are not part of any viewer build.

**Flatpak:** the sandbox cannot see `~/.slipstreamviewer`. Its data folder is
`~/.var/app/org.slipstreamviewer.Slipstream/.slipstreamviewer`, so run
`contrib/ngxshim/build.sh --flatpak ~/DLSS` to install into its `dlss` folder. (Whether DLSS
then works depends on the sandbox's NVIDIA driver; the `DLSS:` line in `Slipstream.log` says
what happened.)

The sections below are the manual steps the script automates.

## What you need

- An NVIDIA GeForce RTX GPU (RTX 20 series and newer, for both features). RR costs more per
  frame than SR, and is aimed at ray-tracing setups.
- A recent NVIDIA driver. DLSS 4.5 and newer RR needs driver 580 or later.
- The DLSS SDK: `git clone https://github.com/NVIDIA/DLSS` (or download a release, e.g.
  v310.9.16). You need its `include/` and `lib/` folders.
- CMake 3.16 or newer, a C++17 compiler and the Vulkan headers (the Vulkan SDK on Windows,
  `vulkan-headers` / `libvulkan-dev` on Linux).

## Build on Linux

```sh
cmake -S contrib/ngxshim -B build-ngxshim -DDLSS_SDK_DIR=$HOME/src/DLSS -DCMAKE_BUILD_TYPE=Release
cmake --build build-ngxshim
```

This produces `build-ngxshim/libslipstream_ngx.so`. It statically links the SDK's
`lib/Linux_x86_64/libnvsdk_ngx.a` and exports only the `slip_nr_*` functions.

## Build on Windows (Visual Studio 2022 or 2026, x64)

```bat
rem Visual Studio 2022:
cmake -S contrib\ngxshim -B build-ngxshim -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_DIR=C:\src\DLSS
rem Visual Studio 2026 (CMake 4.2 or newer), with the VS 2022 toolset NVIDIA's library is built with:
cmake -S contrib\ngxshim -B build-ngxshim -G "Visual Studio 18 2026" -A x64 -T v143 -DDLSS_SDK_DIR=C:\src\DLSS
cmake --build build-ngxshim --config Release
```

With Visual Studio 2026, add **"MSVC v143 - VS 2022 C++ x64/x86 build tools"** in the Visual
Studio Installer (Individual components). `build.ps1` picks the newest Visual Studio it finds
and uses the v143 toolset when it is installed.

This produces `build-ngxshim\Release\slipstream_ngx.dll`. It links `nvsdk_ngx_d.lib`, which
needs the dynamic runtime (`/MD`). Pass `-DSLIP_NR_STATIC_CRT=ON` to use `/MT` and
`nvsdk_ngx_s.lib` instead. Both come from the SDK's `lib/Windows_x86_64/x64`. Build
Release: those libraries are built for the release runtime, and the SDK's `_dbg` variants
are only for Debug builds.

## Install

You need the shim you built, and NVIDIA's feature library for each feature you want, from
the SDK's **`rel`** folder. Never use the `dev` folder: its libraries draw a debug overlay.
With only one of the two NVIDIA libraries, that feature works and the other shows as
unavailable.

| | Shim | Super Resolution (from the SDK) | Ray Reconstruction (from the SDK) |
|---|---|---|---|
| Linux | `libslipstream_ngx.so` | `lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.<version>` | `lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.<version>` |
| Windows | `slipstream_ngx.dll` | `lib/Windows_x86_64/rel/nvngx_dlss.dll` | `lib/Windows_x86_64/rel/nvngx_dlssd.dll` |

Put the files in **one** folder. The viewer looks in these places, in order:

1. The folder in the debug setting `RenderVKDlssShimPath`. This can be a folder, or the
   shim's full file path. When that setting is empty, the `dlss` folder in the viewer's
   data folder instead (`~/.slipstreamviewer/dlss`, `%APPDATA%\SlipstreamViewer\dlss`);
   a viewer update never removes it. If the shim isn't found, `Slipstream.log` lists every
   place it looked.
2. The viewer's own library folder: `<install>/lib` on Linux, the folder with
   `SlipstreamViewer.exe` on Windows.

The viewer tells NGX to look for the feature libraries in the shim's folder. Copy the NVIDIA
libraries as-is: do not strip, re-sign or patch them. NGX checks NVIDIA's signature and
refuses a modified file.

Restart the viewer. The shim is loaded before the Vulkan device is created, because DLSS
needs extra Vulkan extensions. Look in `Slipstream.log` for the `DLSS:` line. It says whether
the shim was found, its version, the extensions it enabled, and which features (SR, RR) are
available. If one isn't, it says why. While DLSS runs, a `DLSS: running SR|RR <quality>
<render size> -> <window size>` line is logged each time the mode changes.

A shim built from an older checkout (before version 1.2) only offers Ray Reconstruction:
rebuild it from this folder to get Super Resolution.

- To stop the viewer loading the shim at all, set `RenderVKDlssShim` to 0.
- NGX's own messages go to `Slipstream.log` under the `NGX` debug tag. NGX's data folder is
  `<viewer cache>/ngx`.
- The shim never asks NGX for model updates: it does not call NGX's update function. Note
  that NVIDIA's driver may still start its own `nvidia-ngx-updater` when NGX initialises.
- On Linux, NGX tries to create `/usr/share/nvidia/ngx` and logs "Permission denied" when it
  can't. That is harmless: DLSS loads the feature libraries from the shim's folder.
  `~/.config/nvidia-ngx-conf.json` is NGX's optional config file.

## The slip_nr ABI

`include/slipstream_ngx.h` is plain C. The viewer and the shim share that header file. The
ABI:

- has a version number (`SLIP_NR_ABI_VERSION`, still 1: the ABI only grows);
- starts every struct with `struct_size`, so either side can grow a struct (fields are only
  ever appended; `*_SIZE_V1` are the first release's sizes);
- adds entry points only as optional ones (`slip_nr_optimal_render_size2`, 1.2+), so an old
  viewer runs a new shim and a new viewer runs an old one;
- uses UTF-8 strings and `uint32_t` flags;
- uses no vendor types.

The calls, in order:

1. `slip_nr_instance_extensions`: before the Vulkan instance is created.
2. `slip_nr_device_extensions`: after the GPU is picked, before the device is created.
3. `slip_nr_init`.
4. `slip_nr_get_caps` (per feature: `ray_reconstruction`, and from 1.2 `super_resolution`)
   and `slip_nr_optimal_render_size2` (per feature; `slip_nr_optimal_render_size` = RR).
5. `slip_nr_create` (`slip_nr_create_info.feature` picks SR or RR, from 1.2), then
   `slip_nr_evaluate` each frame, then `slip_nr_destroy`.
6. `slip_nr_shutdown`.

`slip_nr_last_error` returns the text of the most recent failure.

Another backend could implement the same ABI and the viewer would use it unchanged.

## Licence

The code in this folder is MIT. The DLSS SDK and NVIDIA's feature libraries are under
NVIDIA's own licence, which you accepted when you downloaded them. If you redistribute a
build of this shim, you are bound by that licence: it includes NVIDIA's attribution and
notification requirements.
