# -*- cmake -*-
#
# libwebp (#651, WebP chat image previews): built from the pinned upstream source
# release as a small static library -- decoder + demux (WebPAnimDecoder) only; no
# encoder, no sharpyuv, no tools, no install rules, no threads. Never the system
# libwebp, and no 3p package: the same source builds Linux, the Windows cross build
# (clang-cl) and macOS.
#
# The release tarball is fetched (and verified) by Prebuilt.cmake's fetcher, so it
# lives in the 3p download cache (SL_3P_CACHE, default ~/.cache/slipstream-3p) and
# offline rebuilds work from there. To bump: change WEBP_VERSION and WEBP_SHA256
# (take the hash from the release tarball whose .asc verifies against the WebP
# release signing key 6B0E6B70976DE303EDF2F601F9C3D6BDB8232B5D), and check the
# source lists below against src/{dec,demux,dsp,utils}/Makefile.am.
#
# We do not use upstream's CMakeLists.txt: its webpdemux links the full libwebp
# (encoder + sharpyuv), and it adds directory-wide definitions, try_compile probes
# and install() rules to our tree.
#
# Provides ll::libwebp (include <webp/decode.h>, <webp/demux.h>).
include_guard()

include(Prebuilt)   # _sl_3p_fetch, SL_3P_CACHE_DIR

set(WEBP_VERSION 1.6.0)
set(WEBP_URL "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WEBP_VERSION}.tar.gz")
set(WEBP_SHA256 "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564")

set(WEBP_SRC_PARENT "${CMAKE_BINARY_DIR}/3p-src")
set(WEBP_ROOT "${WEBP_SRC_PARENT}/libwebp-${WEBP_VERSION}")
set(_webp_stamp "${WEBP_SRC_PARENT}/libwebp.extracted")

set(_webp_have "")
if (EXISTS "${_webp_stamp}")
  file(READ "${_webp_stamp}" _webp_have)
  string(STRIP "${_webp_have}" _webp_have)
endif ()
if (NOT _webp_have STREQUAL "${WEBP_SHA256}" OR NOT EXISTS "${WEBP_ROOT}/src/webp/decode.h")
  _sl_3p_fetch(libwebp-src "${WEBP_URL}" SHA256 "${WEBP_SHA256}" "${WEBP_SHA256}" _webp_archive)
  message(STATUS "libwebp: unpacking ${WEBP_VERSION}")
  file(REMOVE_RECURSE "${WEBP_ROOT}")
  file(REMOVE "${_webp_stamp}")
  file(ARCHIVE_EXTRACT INPUT "${_webp_archive}" DESTINATION "${WEBP_SRC_PARENT}")
  if (NOT EXISTS "${WEBP_ROOT}/src/webp/decode.h")
    message(FATAL_ERROR "libwebp: ${_webp_archive} did not unpack to ${WEBP_ROOT}")
  endif ()
  file(WRITE "${_webp_stamp}" "${WEBP_SHA256}\n")
endif ()

set(_webp_dec_srcs
    alpha_dec.c buffer_dec.c frame_dec.c idec_dec.c io_dec.c quant_dec.c tree_dec.c
    vp8_dec.c vp8l_dec.c webp_dec.c)
set(_webp_demux_srcs
    anim_decode.c demux.c)
# dsp: COMMON_SOURCES + every decoder SIMD flavour. A flavour the target cannot
# use compiles to a stub (each file is guarded by WEBP_USE_<flavour>).
set(_webp_dsp_srcs
    alpha_processing.c cpu.c dec.c dec_clip_tables.c filters.c lossless.c rescaler.c
    upsampling.c yuv.c
    alpha_processing_sse2.c dec_sse2.c filters_sse2.c lossless_sse2.c rescaler_sse2.c
    upsampling_sse2.c yuv_sse2.c
    alpha_processing_sse41.c dec_sse41.c lossless_sse41.c upsampling_sse41.c yuv_sse41.c
    lossless_avx2.c
    alpha_processing_neon.c dec_neon.c filters_neon.c lossless_neon.c rescaler_neon.c
    upsampling_neon.c yuv_neon.c
    dec_msa.c filters_msa.c lossless_msa.c rescaler_msa.c upsampling_msa.c
    dec_mips32.c rescaler_mips32.c yuv_mips32.c
    alpha_processing_mips_dsp_r2.c dec_mips_dsp_r2.c filters_mips_dsp_r2.c
    lossless_mips_dsp_r2.c rescaler_mips_dsp_r2.c upsampling_mips_dsp_r2.c yuv_mips_dsp_r2.c)
set(_webp_utils_srcs
    bit_reader_utils.c color_cache_utils.c filters_utils.c huffman_utils.c palette.c
    quant_levels_dec_utils.c rescaler_utils.c random_utils.c thread_utils.c utils.c)

set(_webp_srcs "")
foreach (_dir dec demux dsp utils)
  foreach (_f IN LISTS _webp_${_dir}_srcs)
    set(_p "${WEBP_ROOT}/src/${_dir}/${_f}")
    if (NOT EXISTS "${_p}")
      message(FATAL_ERROR "libwebp: ${_p} is missing (update the source lists in WebP.cmake for ${WEBP_VERSION})")
    endif ()
    list(APPEND _webp_srcs "${_p}")
  endforeach ()
endforeach ()

add_library(slwebp STATIC ${_webp_srcs})
# Sources include "src/..." relative to the tarball root; users include <webp/...>.
target_include_directories(slwebp PRIVATE "${WEBP_ROOT}")
target_include_directories(slwebp SYSTEM INTERFACE "${WEBP_ROOT}/src")
set_target_properties(slwebp PROPERTIES
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    DISABLE_PRECOMPILE_HEADERS ON)

# The viewer's directory-wide options are for its C++ code: /TP (compile everything
# as C++) must not reach these C files, and third-party warnings are not ours to fix
# under -Werror / /WX.
get_target_property(_webp_opts slwebp COMPILE_OPTIONS)
if (_webp_opts)
  list(REMOVE_ITEM _webp_opts /TP -TP /WX -WX -Werror)
  set_target_properties(slwebp PROPERTIES COMPILE_OPTIONS "${_webp_opts}")
endif ()
if (MSVC)
  target_compile_options(slwebp PRIVATE /w)
  target_compile_definitions(slwebp PRIVATE _CRT_SECURE_NO_WARNINGS)
else ()
  target_compile_options(slwebp PRIVATE -w)
endif ()

# SIMD. No HAVE_CONFIG_H: src/dsp/cpu.h derives everything from compiler macros.
# A file's SIMD body is compiled when the compiler targets that ISA for it
# (WEBP_USE_<x>, from __SSE4_1__ / __AVX2__ / __aarch64__ / MSVC), and the generic
# code dispatches to it behind a runtime CPU check when WEBP_HAVE_<x> is set.
# The viewer's C files are built for the x86-64 baseline (its -march is C++ only),
# so on x86-64 Linux / the Windows cross build the SSE4.1 / AVX2 files get their own
# ISA flag and the dispatch is enabled target-wide. (MSVC proper compiles the
# intrinsics without flags; cpu.h enables them there by itself.) macOS builds
# universal (arm64;x86_64): no per-file x86 flags there (they would break the arm64
# slice); it gets SSE2 / NEON, which cpu.h detects per slice.
if (NOT DARWIN AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$"
    AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(
      "${WEBP_ROOT}/src/dsp/alpha_processing_sse41.c"
      "${WEBP_ROOT}/src/dsp/dec_sse41.c"
      "${WEBP_ROOT}/src/dsp/lossless_sse41.c"
      "${WEBP_ROOT}/src/dsp/upsampling_sse41.c"
      "${WEBP_ROOT}/src/dsp/yuv_sse41.c"
      PROPERTIES COMPILE_OPTIONS "-msse4.1")
  set_source_files_properties(
      "${WEBP_ROOT}/src/dsp/lossless_avx2.c"
      PROPERTIES COMPILE_OPTIONS "-mavx2")
  target_compile_definitions(slwebp PRIVATE WEBP_HAVE_SSE2 WEBP_HAVE_SSE41 WEBP_HAVE_AVX2)
endif ()

if (NOT TARGET ll::libwebp)
  add_library(ll::libwebp ALIAS slwebp)
endif ()
