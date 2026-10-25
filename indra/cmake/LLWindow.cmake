# -*- cmake -*-

include(Variables)
include(Prebuilt)
include(SDL3)

include_guard()

# SDL is the only window backend (Linux and Windows).
set(USE_SDL_WINDOW ON CACHE BOOL "Build with SDL window backend" FORCE)
