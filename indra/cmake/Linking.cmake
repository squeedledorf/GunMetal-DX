# -*- cmake -*-

include_guard()
include(Variables)

set(ARCH_PREBUILT_DIRS ${PREBUILT_INSTALL_DIR}/lib)
set(ARCH_PREBUILT_DIRS_PLUGINS ${PREBUILT_INSTALL_DIR}/plugins)
set(ARCH_PREBUILT_DIRS_RELEASE ${PREBUILT_INSTALL_DIR}/lib/release)
set(ARCH_PREBUILT_DIRS_DEBUG ${PREBUILT_INSTALL_DIR}/lib/debug)
if (WINDOWS OR DARWIN )
  # Kludge for older cmake versions, 3.20+ is needed to use a genex in add_custom_command( OUTPUT <var> ... )
  # Using this will work okay-ish, as Debug is not supported anyway. But for property multi config and also
  # ninja support the genex version is preferred.
  if(${CMAKE_VERSION} VERSION_LESS "3.20.0")
    if(CMAKE_BUILD_TYPE MATCHES Release)
      set(SHARED_LIB_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/Release)
    elseif (CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
      set(SHARED_LIB_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/RelWithDebInfo)
    endif()
  else()
    if (WINDOWS)
      # viewer_manifest.py looks in sharedlibs/<config> on Windows; $<CONFIG> is
      # the build type under single-config generators too (Ninja cross build).
      set(SHARED_LIB_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/$<CONFIG>)
    else ()
      set(SHARED_LIB_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>)
    endif ()
    set(SYMBOLS_STAGING_DIR ${CMAKE_BINARY_DIR}/symbols/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>/${VIEWER_CHANNEL})
  endif()

  if( DARWIN )
    set( SHARED_LIB_STAGING_DIR ${SHARED_LIB_STAGING_DIR}/Resources)
  endif()
  set(EXE_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>)
elseif (LINUX)
  set(SHARED_LIB_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/lib)
  set(EXE_STAGING_DIR ${CMAKE_BINARY_DIR}/sharedlibs/bin)
endif ()

if (WINDOWS OR LINUX)
  set(SIMD_ARCH "SSE" CACHE STRING "Architecture for SIMD instructions(SSE, AVX2, ARM64)")
  string(TOLOWER ${SIMD_ARCH} simd_lower)
  if("${CMAKE_GENERATOR_PLATFORM}" STREQUAL "ARM64")
    set(simd_lower arm64)
  endif()
  set(SIMD_ARCH_LOWERED "${simd_lower}" CACHE STRING "Architecture for SIMD instructions(SSE, AVX2, ARM64)" FORCE)

  if ("${SIMD_ARCH_LOWERED}" STREQUAL "sse" OR "${SIMD_ARCH_LOWERED}" STREQUAL "avx2" OR "${SIMD_ARCH_LOWERED}" STREQUAL "arm64" )
    set(ARCH_PREBUILT_DIRS_ARCH ${ARCH_PREBUILT_DIRS}/${SIMD_ARCH_LOWERED})
    set(ARCH_PREBUILT_DIRS_ARCH_RELEASE ${ARCH_PREBUILT_DIRS_ARCH}/release)
    set(ARCH_PREBUILT_DIRS_ARCH_DEBUG ${ARCH_PREBUILT_DIRS_ARCH}/debug)
  else()
    message(FATAL_ERROR "Invalid SIMD_ARCH value")
  endif()
endif()

# Prebuilt packages must provide 'release' versions of libraries, but may provide versions for
# specific build types.  PREBUILT_LIBS_INSTALL_DIRS lists first the build type directory and then
# the 'release' directory (as a default fallback).
# *NOTE - we have to take special care to use CMAKE_CFG_INTDIR on IDE generators (like mac and
# windows) and CMAKE_BUILD_TYPE on Makefile based generators (like linux).  The reason for this is
# that CMAKE_BUILD_TYPE is essentially meaningless at configuration time for IDE generators and
# CMAKE_CFG_INTDIR is meaningless at build time for Makefile generators
if(NOT DARWIN)
  link_directories(${PREBUILT_INSTALL_DIR}/lib/$<LOWER_CASE:$<CONFIG>>)
  link_directories(${PREBUILT_INSTALL_DIR}/lib/release)
endif(NOT DARWIN)

add_library( ll::oslibraries INTERFACE IMPORTED )

if (LINUX)
  target_link_libraries( ll::oslibraries INTERFACE
          dl
          pthread
          rt)
elseif (WINDOWS)
  target_link_libraries( ll::oslibraries INTERFACE
          advapi32
          shell32
          ws2_32
          mswsock
          psapi
          winmm
          netapi32
          wldap32
          gdi32
          user32
          ole32
          dbghelp
          rpcrt4.lib
          legacy_stdio_definitions
          )
else()
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  find_library(CARBON_LIBRARY Carbon)
  find_library(COCOA_LIBRARY Cocoa)
  find_library(IOKIT_LIBRARY IOKit)

  find_library(APPKIT_LIBRARY AppKit)
  find_library(COREAUDIO_LIBRARY CoreAudio)
  find_library(COREGRAPHICS_LIBRARY CoreGraphics)
  find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)

  target_link_libraries( ll::oslibraries INTERFACE
          ${COCOA_LIBRARY}
          ${IOKIT_LIBRARY}
          ${COREFOUNDATION_LIBRARY}
          ${CARBON_LIBRARY}
          ${APPKIT_LIBRARY}
          ${COREAUDIO_LIBRARY}
          ${AUDIOTOOLBOX_LIBRARY}
          ${COREGRAPHICS_LIBRARY}
          )
endif()



