Slipstream Viewer for Linux
===========================

Slipstream is a third-party viewer for Second Life with a GPU-driven Vulkan
renderer. This software is not provided or supported by Linden Lab, the makers
of Second Life.

  Website and downloads:  https://slipstreamviewer.org
  Help and FAQ:           https://slipstreamviewer.org/wiki/
  Bug reports:            https://git.slipstreamviewer.org/sky_line/Slipstream/issues
  Privacy policy:         https://slipstreamviewer.org/privacy.html (we collect no data)


1. System requirements
----------------------

* x86_64 Linux with glibc 2.38 or newer (Fedora 39+, Ubuntu 24.04+,
  Debian 13+, Linux Mint 22+, Arch, CachyOS and similar) and a CPU
  supporting x86-64-v2.
* A GPU with a Vulkan 1.3 driver: NVIDIA proprietary driver 510 or newer, or
  Mesa 22.0 or newer (RADV for AMD, ANV for Intel).
* Optional ray-traced shadows need an NVIDIA RTX 20-series / AMD RX 6000 or
  newer GPU.
* PipeWire or PulseAudio for sound and voice.
* GStreamer (with the plugins for the formats you want) for in-world media.


2. Running
----------

Slipstream runs from the folder you unpacked it into; no installation is
needed. Start it with:

    ./slipstream

Optionally, ./install.sh installs it for your user (or system-wide as root)
and adds a desktop menu entry.

Ubuntu 24.04+, Debian 13+, Linux Mint 22+, Pop!_OS and other derivatives:
install the .deb package from the release page instead of using this
tarball. apt then installs every library the viewer and its web browser
need:

    sudo apt install ./Slipstream_<version>_amd64.deb

(Keep the "./": it tells apt to install the downloaded file. A graphical
package installer such as GDebi works too.) Install a newer .deb the same way to
upgrade; "sudo apt remove slipstream-viewer" uninstalls it. Ubuntu 22.04 and
Debian 12 are too old for this release; apt refuses the package there.

Arch Linux, CachyOS, EndeavourOS, Manjaro and SteamOS: install the
slipstream-bin package instead of using this tarball. pacman then installs
every library the viewer and its web browser need. Get it from the AUR:

    yay -S slipstream-bin        (or: paru -S slipstream-bin)

Or download it from the release page, which carries the PKGBUILD and, when
available, a prebuilt package:

    sudo pacman -U slipstream-bin-<version>-x86_64.pkg.tar.zst
    # or, in an empty folder with the PKGBUILD:  makepkg -si

Both packages install to /opt/slipstream and add a 'slipstream' command, a
menu entry and the secondlife:// link handler. Your settings stay in
~/.slipstreamviewer. If you used this tarball before, delete
~/.local/share/applications/slipstream-viewer.desktop to avoid a second menu
entry.

Slipstream runs natively on Wayland when its window gets a title bar there and
uses X11 (XWayland) otherwise. To choose, use Preferences > Graphics >
Advanced > Display backend (Auto / Wayland / X11, needs a restart), or start it
with:

    ./slipstream --x11        (or SDL_VIDEO_DRIVER=x11 ./slipstream)


3. Where your data lives
------------------------

Settings, chat logs and caches:  ~/.slipstreamviewer
Log file:                        ~/.slipstreamviewer/logs/Slipstream.log

Include Slipstream.log (and Slipstream.old from the previous run, if you've
restarted) with any bug report.

A second (third, ...) viewer running at the same time logs to Slipstream-2.log
(Slipstream-3.log, ...) instead, with its own .old file.


4. Troubleshooting
------------------

* "No Vulkan device" or the viewer won't start: install the Vulkan driver
  package for your GPU (for example vulkan-radeon, vulkan-intel or the NVIDIA
  driver's Vulkan component). "vulkaninfo --summary" should list your GPU.
* "CPU ISA level is lower than required" or a glibc error: your system is
  older than the requirements above.
* No title bar or window border (GNOME on Wayland): GNOME leaves drawing the
  frame to the application, which uses libdecor. Slipstream carries its own
  copy in lib/libdecor; the system's (Ubuntu: libdecor-0-0 and
  libdecor-0-plugin-1-gtk) is used when installed. The "Display backend:" lines
  in Slipstream.log say which one loaded. Choosing X11 in the Display backend
  preference (or ./slipstream --x11) always gives a frame.
* Web and media: if web pages, the search / profile / help floaters or
  media on a prim stay blank, or "The following Media Plugin has failed:
  media_plugin_cef" appears, a system library the built-in browser (CEF)
  needs is missing. From the Slipstream folder, run

      LD_LIBRARY_PATH=./lib ldd lib/libcef.so | grep "not found"

  and install the packages that provide what it lists:

    Ubuntu 24.04+ / Mint 22+ / Debian 13+ (not needed with the .deb):
      sudo apt install libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 \
        libatspi2.0-0t64 libcups2t64 libdrm2 libgbm1 libxkbcommon0 \
        libxcomposite1 libxdamage1 libxrandr2 libasound2t64 libpango-1.0-0 \
        libcairo2
    Fedora:
      sudo dnf install nss nspr atk at-spi2-atk at-spi2-core cups-libs \
        libdrm mesa-libgbm libxkbcommon libXcomposite libXdamage libXrandr \
        alsa-lib pango cairo
    Arch / CachyOS / Manjaro / SteamOS (not needed with slipstream-bin):
      sudo pacman -S --needed nss nspr at-spi2-core libcups libdrm mesa \
        libxkbcommon libxcomposite libxdamage libxrandr alsa-lib pango cairo

  The 'slipstream' script checks this at startup and prints the missing
  libraries. The plugin's own error is also in Slipstream.log (search for
  "could not load").
* Low frame rate: see https://slipstreamviewer.org/wiki/graphics/
* Audio problems: the 'slipstream' launch script has an LL_BAD_OPENAL_DRIVER
  option you can uncomment to start without audio.
* Advanced: the 'slipstream' script also has options for running under
  gamemode, MangoHud or a debugger.

More help: https://slipstreamviewer.org/wiki/troubleshooting/


5. Source code
--------------

Slipstream is free software under the GNU LGPL 2.1. The source code is at
https://git.slipstreamviewer.org/sky_line/Slipstream
