# -*- cmake -*-
#
# Per-configuration compiler switches, formerly LL's "build-variables" repo
# (github.com/AlchemyViewer/build-variables, file `variables`), which autobuild
# exported as LL_BUILD_RELEASE / LL_BUILD_RELWITHDEBINFO for the host platform and
# 00-Common.cmake appended to CMAKE_CXX_FLAGS_<CONFIG>. They are folded in here
# verbatim so the build needs neither autobuild nor that repo; 00-Common.cmake
# still appends them at the same point (to the C++ flags only, as before -- C
# sources never got them), and its later string(REPLACE) tweaks (-march v2->v3 for
# the avx2 SIMD option, /arch:SSE4.2 -> -msse4.2 for clang-cl, /Ob2 -> /Ob3) still
# apply to them.
#
# What each switch is for:
#   all POSIX     -std=c++20 -fPIC -DPIC   language level; position-independent code
#   Linux base    -march=x86-64-v2 -g -DLL_LINUX=1
#                   ISA floor (SSE4.2/POPCNT; see fix_isa_note.py, glibc-check), debug
#                   info (stripped/split at packaging), platform macro
#   Release       -O3 -fstack-protector -DLL_RELEASE=1 -DLL_RELEASE_FOR_DOWNLOAD=1 -DNDEBUG
#   RelWithDebInfo -Og (Linux) / -O0 (macOS) / /Od /Ob0 (Windows)
#                   -DLL_RELEASE=1 -DLL_RELEASE_WITH_DEBUG_INFO=1 -DNDEBUG
#   macOS base    -g --debug -mmacosx-version-min=12.0 -DLL_DARWIN=1
#   Windows base  /arch:SSE4.2 /std:c++20 /permissive- /Zc:wchar_t /Zc:__cplusplus
#                 /Zc:preprocessor /Zi /GR /EHsc and WIN32/_WINDOWS/LL_WINDOWS/UNICODE,
#                 WINVER=_WIN32_WINNT=0x0A00 (Windows 10)
#   Windows Release/RelWithDebInfo /MD (release DLL CRT, what the 3p packages use),
#                 _ITERATOR_DEBUG_LEVEL=0
# Debug flavours existed in build-variables too but this tree only supports
# Release and RelWithDebInfo (00-Common.cmake CMAKE_CONFIGURATION_TYPES).
include_guard()

if (WINDOWS)
  set(_ll_base_macros "/DWIN32 /D_WINDOWS /DLL_WINDOWS=1 /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00")
  set(_ll_base_switches "/arch:SSE4.2 /std:c++20 /permissive- /Zc:wchar_t /Zc:__cplusplus /Zc:preprocessor /Zi /GR /EHsc")
  set(LL_BUILD_RELEASE
      "/MD /O2 /Ob2 ${_ll_base_switches} /DLL_RELEASE=1 /DLL_RELEASE_FOR_DOWNLOAD=1 /DNDEBUG /D_ITERATOR_DEBUG_LEVEL=0 ${_ll_base_macros}")
  set(LL_BUILD_RELWITHDEBINFO
      "/MD /Od /Ob0 ${_ll_base_switches} /DLL_RELEASE=1 /DLL_RELEASE_WITH_DEBUG_INFO=1 /DNDEBUG /D_ITERATOR_DEBUG_LEVEL=0 ${_ll_base_macros}")
elseif (DARWIN)
  set(_ll_base_macros "-DLL_DARWIN=1 -DPIC")
  set(_ll_base_switches "-g --debug -mmacosx-version-min=12.0 -std=c++20 -fPIC")
  set(LL_BUILD_RELEASE
      "-O3 -fstack-protector ${_ll_base_switches} -DLL_RELEASE=1 -DLL_RELEASE_FOR_DOWNLOAD=1 -DNDEBUG ${_ll_base_macros}")
  set(LL_BUILD_RELWITHDEBINFO
      "-O0 ${_ll_base_switches} -DLL_RELEASE=1 -DLL_RELEASE_WITH_DEBUG_INFO=1 -DNDEBUG ${_ll_base_macros}")
else ()
  set(_ll_base_macros "-DLL_LINUX=1 -DPIC")
  set(_ll_base_switches "-march=x86-64-v2 -g -std=c++20 -fPIC")
  set(LL_BUILD_RELEASE
      "-O3 -fstack-protector ${_ll_base_switches} -DLL_RELEASE=1 -DLL_RELEASE_FOR_DOWNLOAD=1 -DNDEBUG ${_ll_base_macros}")
  set(LL_BUILD_RELWITHDEBINFO
      "-Og ${_ll_base_switches} -DLL_RELEASE=1 -DLL_RELEASE_WITH_DEBUG_INFO=1 -DNDEBUG ${_ll_base_macros}")
endif ()
unset(_ll_base_macros)
unset(_ll_base_switches)
