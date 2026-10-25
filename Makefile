# Build Slipstream Viewer locally on Linux (developed for Arch/CachyOS).
#
# Plain CMake (indra/CMakePresets.json) + this thin wrapper; no autobuild. The 3p
# prebuilt packages are fetched by CMake itself at configure time
# (indra/cmake/Prebuilt.cmake, manifest indra/cmake/3p_manifest.json, maintained with
# scripts/3p/manifest.py) into a shared download cache
# (SL_3P_CACHE, default ~/.cache/slipstream-3p).
#
# System packages assumed installed:
#   base-devel cmake (>= 3.21) ninja git python pkgconf
#   libpulse libunwind libxinerama libxcursor libxfixes libxft
#   libpipewire dbus vlc gstreamer gst-plugins-base
#
# Usage:
#   make              # full build (default)
#   make run          # launch the built viewer
#   make clean        # remove build dir (keeps the 3p cache + venv)
#   make distclean    # remove everything generated in the checkout
#
# Override on the command line, e.g.:
#   make PRESET=linux-release          (or CONFIGURATION=Release)
#   make EXTRA_CMAKE_FLAGS=
#   make SL_3P_CACHE=/path/to/archives

SHELL       := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

ROOT       := $(CURDIR)
VENV       := $(ROOT)/.venv
VENV_PY    := $(VENV)/bin/python
BUILD_DIR  := $(ROOT)/build-linux-x86_64
LAUNCHER   := $(BUILD_DIR)/newview/packaged/slipstream

# CMake configure preset (indra/CMakePresets.json). CONFIGURATION is the old
# LEGACY alias for PRESET (old autobuild configuration name), still accepted: ReleaseOS -> linux-releaseos,
# Release -> linux-release.
CONFIGURATION       ?= ReleaseOS
ifeq ($(CONFIGURATION),Release)
PRESET              ?= linux-release
else
PRESET              ?= linux-releaseos
endif
EXTRA_CMAKE_FLAGS   ?= -DGCC_DISABLE_FATAL_WARNINGS=ON

# 3p download cache; empty = CMake's default ($SL_3P_CACHE or ~/.cache/slipstream-3p).
SL_3P_CACHE ?=
ifneq ($(strip $(SL_3P_CACHE)),)
EXTRA_CMAKE_FLAGS += "-DSL_3P_CACHE=$(SL_3P_CACHE)"
endif

# Clean-pipeline diagnostics master switch (see indra/newview/llcleandebug.h).
# Defaults to 0 (production, no diagnostic overhead); `make CLEAN_DEBUG=1` opts in
# to the heavy clean-pipeline instrumentation (almost all of it GL/#29-era).
# NOTE: we must pass -DCLEAN_DEBUG explicitly every build, because once the
# CMakeCache holds a value (e.g. from a prior `make CLEAN_DEBUG=1`), leaving it
# unset does NOT fall back to the header default -- CMake keeps the stale cached
# value. Flipping the value recompiles all of newview (it is a compile definition).
CLEAN_DEBUG ?= 0
ifneq ($(strip $(CLEAN_DEBUG)),)
EXTRA_CMAKE_FLAGS += -DCLEAN_DEBUG=$(CLEAN_DEBUG)
# Force a reconfigure whenever CLEAN_DEBUG is explicitly passed so the new
# -DCLEAN_DEBUG value actually reaches CMake (the CMakeCache target is otherwise
# only rebuilt when the presets/3p manifest/version change). Normal `make` (no
# CLEAN_DEBUG) is unaffected.
.PHONY: clean_debug_force
clean_debug_force:
$(BUILD_DIR)/CMakeCache.txt: clean_debug_force
endif

# The venv only carries the Python modules the build itself needs (llsd for
# viewer_manifest.py; scripts/requirements.txt). CMake's Python.cmake honours $PYTHON.
export PYTHON := $(VENV_PY)
# Versioning (scripts/ci/version.sh): MAJOR.MINOR.PATCH from
# indra/newview/VIEWER_VERSION.txt, BUILD from CI (0 locally), channel from the tag.
# <GMDX> local builds log in as "GunMetal-DX Test"
VIEWER_CHANNEL ?= GunMetal-DX Test
BUILD_ID       ?= 0
EXTRA_CMAKE_FLAGS += "-DVIEWER_CHANNEL=$(VIEWER_CHANNEL)" "-DVIEWER_BUILD_ID=$(BUILD_ID)"
# Developer test-control channel (indra/newview/sltestctl.h): `make TESTCTL=1` for our own
# test boxes only. Always passed explicitly so a stale CMakeCache value can't leak into a
# normal build; CMake refuses it when BUILD_ID is non-zero (CI releases).
TESTCTL        ?= 0
EXTRA_CMAKE_FLAGS += -DSLIPSTREAM_TESTCTL=$(if $(filter 1 ON on,$(TESTCTL)),ON,OFF)
# Auto-updater (#421): the update signing PUBLIC key (64 hex), compiled into the viewer so it
# can verify signed update manifests. Empty = the updater stays inert (scripts/updater/README.md).
# <GMDX> GunMetal-DX update signing key (public half; the private key stays outside the repo)
UPDATE_PUBKEY_HEX ?= ac6f0c3ff4f77bcb016c84b6e889a296f41034f4c2676514b608c712ea1e3ba0
ifneq ($(strip $(UPDATE_PUBKEY_HEX)),)
EXTRA_CMAKE_FLAGS += "-DUPDATE_PUBKEY_HEX=$(UPDATE_PUBKEY_HEX)"
endif

# Channel, build number and version are baked in at configure time: re-run configure
# when any of them changes (the stamp is rewritten only when its content differs).
VERSION_STAMP := $(BUILD_DIR)/.version_stamp
.PHONY: version_stamp_update
version_stamp_update:
	@mkdir -p $(BUILD_DIR)
	@new="$(PRESET)|$(VIEWER_CHANNEL)|$(BUILD_ID)|testctl=$(TESTCTL)|$$(cat indra/newview/VIEWER_VERSION.txt)"; \
	if [ "$$(cat $(VERSION_STAMP) 2>/dev/null)" != "$$new" ]; then echo "$$new" > $(VERSION_STAMP); fi
$(VERSION_STAMP): version_stamp_update

.DEFAULT_GOAL := all
.PHONY: all help venv configure build run clean distclean windows version version-bump

all: build

help:
	@printf 'Targets:\n'
	@printf '  all (default)   full build\n'
	@printf '  configure       cmake configure only (also fetches/unpacks the 3p packages)\n'
	@printf '  build           compile + package the viewer\n'
	@printf '  run             launch the built viewer\n'
	@printf '  windows         cross-build + zip the Windows x64 viewer (scripts/wincross)\n'
	@printf '  venv            create .venv with the build'"'"'s Python modules\n'
	@printf '  version         print version/channel (TAG=v0.11.0-beta.1 to preview)\n'
	@printf '  version-bump    set VIEWER_VERSION.txt (VERSION=X.Y.Z)\n'
	@printf '  glibc-check     portability checks on the packaged binaries\n'
	@printf '  clean           remove %s\n' '$(BUILD_DIR)'
	@printf '  distclean       remove build dirs, venv, build-variables leftover\n'
	@printf '\nVariables (override with make VAR=value):\n'
	@printf '  PRESET                = %s\n' '$(PRESET)'
	@printf '  EXTRA_CMAKE_FLAGS     = %s\n' '$(EXTRA_CMAKE_FLAGS)'
	@printf '  SL_3P_CACHE           = %s\n' '$(or $(SL_3P_CACHE),(CMake default: $$SL_3P_CACHE or ~/.cache/slipstream-3p))'
	@printf '  VIEWER_CHANNEL        = %s\n' '$(VIEWER_CHANNEL)'
	@printf '  BUILD_ID              = %s\n' '$(BUILD_ID)'

# Stamp file, not bin/python itself: that is a symlink to the system interpreter.
VENV_STAMP := $(VENV)/.requirements-stamp
$(VENV_STAMP): scripts/requirements.txt
	python3 -m venv $(VENV)
	$(VENV_PY) -m pip install --quiet --upgrade pip
	$(VENV_PY) -m pip install --quiet -r scripts/requirements.txt
	@touch $(VENV_STAMP)

venv: $(VENV_STAMP)

$(BUILD_DIR)/CMakeCache.txt: $(VENV_STAMP) indra/CMakePresets.json indra/cmake/3p_manifest.json $(VERSION_STAMP)
	cmake -S indra --preset $(PRESET) $(EXTRA_CMAKE_FLAGS)

configure: $(BUILD_DIR)/CMakeCache.txt

# TARBALL=1 to also produce the distributable tar.xz (xz of the ~330MB
# packaged tree costs minutes; local iteration runs from packaged/ directly).
TARBALL ?= 0
build: $(BUILD_DIR)/CMakeCache.txt
	$(if $(filter 1,$(TARBALL)),,ALCH_SKIP_TARBALL=1) cmake --build $(BUILD_DIR)
	@printf '\nLauncher: %s\n' '$(LAUNCHER)'
	@$(MAKE) --no-print-directory glibc-check

# Portability guard: fail if any packaged binary needs a glibc newer than
# GLIBC_MAX (default 2.38 -> Fedora 39+/Ubuntu 24.04+). New re-versioned libm
# symbols get pinned in indra/cmake/glibc_compat.h.
GLIBC_MAX ?= 2.38
# Same for the C++ runtime (#147): GLIBCXX_MAX 3.4.32 = GCC 13's libstdc++, what
# the CI builder (applejack, GCC 14 headers) produces. A local build with a newer
# GCC only warns; CI passes GLIBCXX_STRICT=1 so a release can never raise it.
GLIBCXX_MAX ?= 3.4.32
GLIBCXX_STRICT ?= 0
# Packaged libraries outside lib/ (libdecor sits in lib/libdecor so it doesn't
# shadow a system copy, #197).
GLIBC_CHECK_EXTRA = $(BUILD_DIR)/newview/packaged/lib/libdecor/*.so* $(BUILD_DIR)/newview/packaged/lib/libdecor/plugins-1/*.so
.PHONY: glibc-check
# Windows x64 cross build (clang-cl + xwin); see scripts/wincross/build.sh.
windows: venv
	VIEWER_CHANNEL="$(VIEWER_CHANNEL)" BUILD_ID="$(BUILD_ID)" SL_3P_CACHE="$(SL_3P_CACHE)" UPDATE_PUBKEY_HEX="$(UPDATE_PUBKEY_HEX)" bash scripts/wincross/build.sh

# Print the resolved version/channel (TAG=v0.11.0-beta.1 to preview a tag).
version:
	@bash scripts/ci/version.sh $(TAG)

# make version-bump VERSION=0.12.0 -> writes indra/newview/VIEWER_VERSION.txt
version-bump:
	@[[ "$(VERSION)" =~ ^[0-9]+\.[0-9]+\.[0-9]+$$ ]] || { echo "usage: make version-bump VERSION=X.Y.Z"; exit 1; }
	@echo "$(VERSION)" > indra/newview/VIEWER_VERSION.txt
	@echo "VIEWER_VERSION.txt -> $(VERSION) (commit it, then tag v$(VERSION) or v$(VERSION)-beta.N)"

glibc-check:
	@bad=0; for f in $(BUILD_DIR)/newview/packaged/bin/* $(BUILD_DIR)/newview/packaged/lib/*.so* $(GLIBC_CHECK_EXTRA); do \
	  [ -f "$$f" ] || continue; \
	  v=$$( (objdump -T "$$f" 2>/dev/null || true) | (grep -oE 'GLIBC_2\.[0-9]+' || true) | sort -t. -k2 -n -u | tail -1); \
	  [ -z "$$v" ] && continue; \
	  if [ "$$(printf '%s\n%s\n' "GLIBC_$(GLIBC_MAX)" "$$v" | sort -t. -k2 -n | tail -1)" != "GLIBC_$(GLIBC_MAX)" ]; then \
	    echo "glibc-check: $$f needs $$v (> $(GLIBC_MAX)):"; \
	    (objdump -T "$$f" | grep -F "$$v)" || true) | awk '{print "    " $$NF}'; bad=1; fi; \
	done; \
	if [ $$bad = 0 ]; then echo "glibc-check: all packaged binaries need <= GLIBC_$(GLIBC_MAX)"; else exit 1; fi
	@bad=0; for f in $(BUILD_DIR)/newview/packaged/bin/* $(BUILD_DIR)/newview/packaged/lib/*.so* $(GLIBC_CHECK_EXTRA); do \
	  [ -f "$$f" ] || continue; \
	  if (readelf -n "$$f" 2>/dev/null || true) | grep -q 'ISA needed:.*x86-64-v4'; then \
	    echo "isa-check: $$f declares it needs x86-64-v4 (AVX-512)"; bad=1; fi; \
	done; \
	if [ $$bad = 0 ]; then echo "isa-check: no packaged binary requires x86-64-v4"; else exit 1; fi
	@for f in $(GLIBC_CHECK_EXTRA); do \
	  [ -f "$$f" ] || continue; \
	  if (readelf -n "$$f" 2>/dev/null || true) | grep -qE 'ISA used:.*x86-64-v[34]'; then \
	    echo "isa-check: WARNING: $$f (copied from this build host) was compiled for x86-64-v3/v4;" \
	         "a tarball for other machines needs a generic build (SLIPSTREAM_LIBDECOR_LIBDIR, see viewer_manifest.py)"; fi; \
	done
	@bad=0; for f in $(BUILD_DIR)/newview/packaged/bin/* $(BUILD_DIR)/newview/packaged/lib/*.so* $(BUILD_DIR)/newview/packaged/bin/llplugin/*.so* $(GLIBC_CHECK_EXTRA); do \
	  [ -f "$$f" ] || continue; \
	  v=$$( (objdump -T "$$f" 2>/dev/null || true) | (grep -oE 'GLIBCXX_3\.4\.[0-9]+' || true) | sort -V -u | tail -1); \
	  [ -z "$$v" ] && continue; \
	  if [ "$$(printf '%s\n%s\n' "GLIBCXX_$(GLIBCXX_MAX)" "$$v" | sort -V | tail -1)" != "GLIBCXX_$(GLIBCXX_MAX)" ]; then \
	    echo "glibcxx-check: $$f needs $$v (> $(GLIBCXX_MAX))"; bad=1; fi; \
	done; \
	if [ $$bad = 0 ]; then echo "glibcxx-check: all packaged binaries need <= GLIBCXX_$(GLIBCXX_MAX)"; \
	elif [ "$(GLIBCXX_STRICT)" = 1 ]; then exit 1; \
	else echo "glibcxx-check: WARNING (local build; CI enforces this with GLIBCXX_STRICT=1)"; fi

# run does NOT depend on build: a build re-runs the packaging step (and with
# TARBALL=1 the minutes-long xz of the ~330MB tree). Build explicitly (make build)
# when sources changed.
run:
	cd $(BUILD_DIR)/newview/packaged && ./slipstream

clean:
	rm -rf $(BUILD_DIR)

# The shared 3p download cache (~/.cache/slipstream-3p) is left alone, and so are the
# legacy .autobuild-installables[-win] dirs (CI still uses them as its SL_3P_CACHE;
# Prebuilt.cmake also reads them as a fallback).
distclean:
	rm -rf $(BUILD_DIR) $(ROOT)/build-windows-x86_64 $(VENV) $(ROOT)/.build-variables
