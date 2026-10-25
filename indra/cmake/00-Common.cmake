# -*- cmake -*-
#
# Compilation options shared by all Second Life components.

#*****************************************************************************
#   It's important to realize that CMake implicitly concatenates
#   CMAKE_CXX_FLAGS with (e.g.) CMAKE_CXX_FLAGS_RELEASE for Release builds. So
#   set switches in CMAKE_CXX_FLAGS that should affect all builds, but in
#   CMAKE_CXX_FLAGS_RELEASE or CMAKE_CXX_FLAGS_RELWITHDEBINFO for switches
#   that should affect only that build variant.
#
#   Also realize that CMAKE_CXX_FLAGS may already be partially populated on
#   entry to this file.
#
#   Additionally CMAKE_C_FLAGS is prepended to CMAKE_CXX_FLAGS_RELEASE and
#   CMAKE_CXX_FLAGS_RELWITHDEBINFO which risks having flags overriden by cmake
#   inserting additional options that are part of the build config type.
#*****************************************************************************
include_guard()

include(Variables)
include(Linking)

# Per-configuration switches (formerly LL build-variables via autobuild): LLBuildFlags.cmake.
include(LLBuildFlags)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${LL_BUILD_RELEASE}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${LL_BUILD_RELWITHDEBINFO}")

# Given that, all the flags you see added below are flags NOT present in
# LLBuildFlags.cmake (formerly LL's build-variables repo).
# Before adding new ones here, it's important to ask: can this flag really be
# applied to the viewer only, or should/must it be applied to all 3p libraries
# as well?

# Portable compilation flags.
add_compile_definitions( ADDRESS_SIZE=${ADDRESS_SIZE})
# Because older versions of Boost.Bind dumped placeholders _1, _2 et al. into
# the global namespace, Boost now requires either BOOST_BIND_NO_PLACEHOLDERS
# to avoid that or BOOST_BIND_GLOBAL_PLACEHOLDERS to state that we require it
# -- which we do. Without one or the other, we get a ton of Boost warnings.
add_compile_definitions(BOOST_BIND_GLOBAL_PLACEHOLDERS)

# Boost.Wave (LSL preprocessor) turns on BOOST_SPIRIT_THREADSAFE in its own TUs; the
# other Spirit Classic users (LLCalc number fields, the XUI parser) did not, so the
# same object_with_id / id-supply templates were instantiated with two layouts (ODR
# violation). Windows crashed on the first number-field evaluation, Linux at exit
# destroying the id supply's mutex (#7). Every TU must agree: threadsafe everywhere.
add_compile_definitions(BOOST_SPIRIT_THREADSAFE PHOENIX_THREADSAFE=1)

# Force enable SSE2 instructions in GLM per the manual
# https://github.com/g-truc/glm/blob/master/manual.md#section2_10
add_compile_definitions(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES=1 GLM_ENABLE_EXPERIMENTAL=1)

# SSE2NEON throws a pointless warning when compiler optimizations are enabled
add_compile_definitions(SSE2NEON_SUPPRESS_WARNINGS=1)

# Configure crash reporting
set(RELEASE_CRASH_REPORTING OFF CACHE BOOL "Enable use of crash reporting in release builds")
set(NON_RELEASE_CRASH_REPORTING OFF CACHE BOOL "Enable use of crash reporting in developer builds")

if(RELEASE_CRASH_REPORTING)
  add_compile_definitions( LL_SEND_CRASH_REPORTS=1)
endif()

if(NON_RELEASE_CRASH_REPORTING)
  add_compile_definitions( LL_SEND_CRASH_REPORTS=1)
endif()

set(USE_LTO OFF CACHE BOOL "Enable Link Time Optimization")
if(USE_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# Don't bother with a MinSizeRel or Debug builds.
set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Release" CACHE STRING "Supported build types." FORCE)

# Platform-specific compilation flags.

if (WINDOWS)
  # Don't build DLLs.
  set(BUILD_SHARED_LIBS OFF)

  # for "backwards compatibility", cmake sneaks in the Zm1000 option which royally
  # screws incredibuild. this hack disables it.
  # for details see: http://connect.microsoft.com/VisualStudio/feedback/details/368107/clxx-fatal-error-c1027-inconsistent-values-for-ym-between-creation-and-use-of-precompiled-headers
  # http://www.ogre3d.org/forums/viewtopic.php?f=2&t=60015
  # http://www.cmake.org/pipermail/cmake/2009-September/032143.html
  string(REPLACE "/Zm1000" " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

  add_link_options(
       /LARGEADDRESSAWARE
       /NODEFAULTLIB:LIBCMT
       /IGNORE:4099
  )

  add_compile_definitions(
      WIN32_LEAN_AND_MEAN
      NOMINMAX
#     DOM_DYNAMIC                     # For shared library colladadom
      _CRT_SECURE_NO_WARNINGS         # Allow use of sprintf etc
      _CRT_NONSTDC_NO_DEPRECATE       # Allow use of sprintf etc
      _CRT_OBSOLETE_NO_WARNINGS
      _WINSOCK_DEPRECATED_NO_WARNINGS # Disable deprecated WinSock API warnings
      )
  add_compile_options(
          /Zo
          /GS
          /TP
          /W3
          /c
          /Zc:forScope
          /nologo
          /Oy-
          /fp:fast
          /MP
          /permissive-
      )

  if("${simd_lower}" STREQUAL "avx2")
    string(REPLACE "/arch:SSE4.2" "/arch:AVX2" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/arch:SSE4.2" "/arch:AVX2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/arch:SSE4.2" "/arch:AVX2" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/arch:SSE4.2" "/arch:AVX2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/arch:SSE4.2" "/arch:AVX2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    # Also add previous SSE definitions
    add_compile_definitions(
        __SSE3__=1
        __SSSE3__=1
        __SSE4__=1
        __SSE4_1__=1
        __SSE4_2__=1
        )
  elseif("${simd_lower}" STREQUAL "arm64")
    string(REPLACE "/arch:SSE4.2" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/arch:SSE4.2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/arch:SSE4.2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/arch:SSE4.2" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/arch:SSE4.2" "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  else()
    add_compile_definitions(
        __SSE3__=1
        __SSSE3__=1
        __SSE4__=1
        __SSE4_1__=1
        __SSE4_2__=1
        )
  endif()

  # Cross build (clang-cl + xwin from Linux, scripts/wincross): clang-cl has no
  # /arch:SSE4.2 for x64 -- the equivalent is -msse4.2.
  if (CROSS_WINDOWS)
    foreach(_v CMAKE_CXX_FLAGS CMAKE_C_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELEASE
               CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/arch:SSE4.2" "-msse4.2" ${_v} "${${_v}}")
    endforeach()
    # SDL3's SDL_endian.h defines _m_prefetch for old clang-cl; current clang has
    # it as a builtin. Its own include guard skips that shim.
    add_compile_definitions(__PRFCHWINTRIN_H)
  endif ()

  # We want aggressive inlining on MSVC to better match clang/gcc at O3
  string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REPLACE "/Ob2" "/Ob3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

  if (NOT VS_DISABLE_FATAL_WARNINGS)
    add_compile_options(/WX)
  endif (NOT VS_DISABLE_FATAL_WARNINGS)

  #ND: When using something like buildcache (https://github.com/mbitsnbites/buildcache)
  # to make those wrappers work /Zi must be changed to /Z7, as /Zi due to it's nature is not compatible with caching
  if(${CMAKE_CXX_COMPILER_LAUNCHER} MATCHES ".*cache.*")
    add_compile_options( /Z7 )
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  endif ()
endif (WINDOWS)


if (LINUX)
  set(CMAKE_SKIP_RPATH TRUE)

   # LL_IGNORE_SIGCHLD
   # don't catch SIGCHLD in our base application class for the viewer - some of
   # our 3rd party libs may need their *own* SIGCHLD handler to work. Sigh! The
   # viewer doesn't need to catch SIGCHLD anyway.

  add_compile_definitions(
          _REENTRANT
          APPID=secondlife
          LL_IGNORE_SIGCHLD
          $<$<CONFIG:Release>:_FORTIFY_SOURCE=2>
  )
  add_compile_options(
          $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-fstack-protector>
          -fexceptions
          -fno-math-errno
          -fno-strict-aliasing
          -fsigned-char
          -g
          -pthread
          -fvisibility=hidden
  )
  # Portability: pin libm symbols that newer glibc re-versioned (see the header).
  add_compile_options("SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/glibc_compat.h")
endif (LINUX)

# Portability: executables inherit the ISA-level note of every linked object; a
# distro glibc built for x86-64-v4 (CachyOS Scrt1.o) makes them claim to need
# AVX-512. Rewrite the note to what we compile for (baseline + v2) after linking.
function(ll_fix_isa_note target)
  if (LINUX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if (NOT PYTHON_EXECUTABLE)
      include(Python)
    endif ()
    # baseline+v2 (0x3); the avx2 SIMD option compiles for v3 (0x7)
    set(_isa_mask 3)
    if ("${simd_lower}" STREQUAL "avx2")
      set(_isa_mask 7)
    endif ()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/fix_isa_note.py $<TARGET_FILE:${target}> ${_isa_mask}
      VERBATIM)
  endif ()
endfunction()

if (LINUX)

  if("${simd_lower}" STREQUAL "avx2")
    string(REPLACE "-march=x86-64-v2" "-march=x86-64-v3" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-march=x86-64-v2" "-march=x86-64-v3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "-march=x86-64-v2" "-march=x86-64-v3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "-march=x86-64-v2" "-march=x86-64-v3" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "-march=x86-64-v2" "-march=x86-64-v3" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  endif()

  if (USE_ASAN OR USE_LEAKSAN OR USE_UBSAN OR USE_THDSAN)
    add_compile_options(-Og)
  endif ()

  # Enable these flags so we have a read only GOT and some linking opts
  add_link_options("LINKER:-z,relro" "LINKER:-z,now" "LINKER:--as-needed" "LINKER:--build-id=uuid")
endif (LINUX)

if (DARWIN)
  # Use rpath loading on macos
  set(CMAKE_MACOSX_RPATH TRUE)

  # Warnings should be fatal -- thanks, Nicky Perian, for spotting reversed default
  set(CLANG_DISABLE_FATAL_WARNINGS OFF)
  set(CMAKE_CXX_LINK_FLAGS "-Wl,-headerpad_max_install_names,-search_paths_first")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_CXX_LINK_FLAGS}")

  # Ensure debug symbols are always generated
  add_compile_options(-g --debug) # --debug is a clang synonym for -g that bypasses cmake behaviors

  # Silence GL deprecation warnings
  add_compile_definitions(GL_SILENCE_DEPRECATION=1)
endif(DARWIN)

if (LINUX OR DARWIN)
  add_compile_options(-Wall -Wno-sign-compare -Wno-trigraphs -Wno-reorder)

  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # libstdc++ headers contain deprecated declarations that fail on clang
    # macOS currently has many deprecated calls
    add_compile_options(-Wno-unused-local-typedef)
  endif()

  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-stringop-truncation -Wno-stringop-overflow -Wno-maybe-uninitialized)
  endif()

  if (NOT GCC_DISABLE_FATAL_WARNINGS AND NOT CLANG_DISABLE_FATAL_WARNINGS)
    add_compile_options(-Werror)
  endif ()

  add_compile_options(${GCC_WARNINGS})
  add_compile_options(-m${ADDRESS_SIZE})
endif (LINUX OR DARWIN)


# Anonymous, reproducible paths: rewrite the checkout's absolute path to "." in
# __FILE__ (asserts, logs), debug info and profile data, so binaries never carry
# the build machine's directory layout (home directory, user name). LLError's
# abbreviateFile strips through "indra/", so log locations read the same.
get_filename_component(LL_REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if (MSVC)
  # clang-cl (the Windows cross build): pass the GCC-style flag through.
  add_compile_options("/clang:-ffile-prefix-map=${LL_REPO_ROOT}=.")
  # Only the PDB's file name goes into the executable, not its build path.
  add_link_options("/pdbaltpath:%_PDB%")
else ()
  add_compile_options("-ffile-prefix-map=${LL_REPO_ROOT}=.")
endif ()
