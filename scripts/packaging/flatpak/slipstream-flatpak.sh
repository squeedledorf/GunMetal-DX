#!/bin/sh
# /app/bin/slipstream: Flatpak launcher (#286). Runs the release tarball's own
# wrapper (/app/slipstream/slipstream), which sets LD_LIBRARY_PATH to its lib/,
# picks libdecor and handles --x11 / --wayland, exactly as for the tarball.
# The .packaged-by marker makes the wrapper skip the per-user desktop entry,
# the secondlife:// registration and the missing-library check (the runtime
# provides the libraries; flatpak exports the desktop entry).
# User data: ~/.slipstreamviewer inside the sandbox, stored in
# ~/.var/app/org.slipstreamviewer.Slipstream/.slipstreamviewer on the host.
export SLIPSTREAM_SKIP_MEDIA_CHECK=1
# Window class / Wayland app_id = the desktop file ID, so the desktop shows
# this app's icon and groups its windows.
export SDL_VIDEO_WAYLAND_WMCLASS=org.slipstreamviewer.Slipstream
export SDL_VIDEO_X11_WMCLASS=org.slipstreamviewer.Slipstream
exec /app/slipstream/slipstream "$@"
