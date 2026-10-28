# -*- cmake -*-
#
# whisper.cpp + ggml (#1118, VR dictation): local speech-to-text, CPU only, built from
# the pinned upstream source release as one small run-time-loaded module (slwhisper:
# lib/libslwhisper.so, slwhisper.dll). No 3p package and no system library: the same
# source builds Linux and the Windows cross build (clang-cl). Released under the MIT
# licence (licenses-*.txt "whisper.cpp / ggml").
#
# Why a module (dlopen'ed by LLSpeechToText on the first dictation, never linked): the
# viewer ships for x86-64-v2 / SSE4.2 (SIMD_ARCH defaults to SSE, Linking.cmake), but
# whisper needs AVX2 to be fast enough. whisper.cpp and gguf.cpp have static
# initializers; compiled for AVX2 and linked into the viewer they would run (VEX-encoded)
# at every startup and fault on a CPU without AVX, and AVX2 template instances could be
# picked by the linker for the viewer's own code. In the module, AVX2 code only ever runs
# after LLSpeechToText checked the CPU, and nothing is loaded with VR / dictation unused.
#
# The release tarball is fetched and verified (SHA256) into the 3p download cache
# (SL_3P_CACHE, default ~/.cache/slipstream-3p) under its own name, so offline rebuilds
# work from there. To bump: change WHISPER_VERSION / WHISPER_SHA256, and check the
# source lists below against ggml/src/CMakeLists.txt (ggml-base, ggml),
# ggml/src/ggml-cpu/CMakeLists.txt (GGML_CPU_SOURCES + arch/x86) and src/CMakeLists.txt.
# WHISPER_SOURCE_URL may point at a mirror of the same tarball (same SHA256).
#
# We do not use upstream's CMakeLists.txt: it adds directory-wide definitions, install
# rules and backend probing to our tree. What is different on purpose:
#  - ggml/src/ggml.cpp is NOT built: its only content is a static initializer that
#    installs a std::terminate handler (ggml_print_backtrace, which may fork a debugger)
#    when the module loads -- it would take over the viewer's crash handling. Nothing
#    references its symbols.
#  - CPU backend only, statically registered inside the module (GGML_USE_CPU; no
#    GGML_BACKEND_DL: ggml itself never dlopens anything), no OpenMP (ggml's own thread
#    pool), no GPU backends: the GPU renders the headset.
#  - ISA: upstream's "haswell" CPU variant, SSE4.2 AVX F16C FMA AVX2 BMI2 (every machine
#    with a ray-tracing GPU has it; LLSpeechToText checks before loading).
#  - Exports: only the whisper_* / ggml_* C API (WHISPER_SHARED / GGML_SHARED), the rest
#    is hidden; the ISA note of the .so is set to v3 (CachyOS' crt objects would stamp v4
#    and glibc would refuse to load it on other machines).
#
# Provides the module target slwhisper and ll::whisper (headers only, for the types of
# <whisper.h>, and LL_WHISPER=1); the viewer depends on slwhisper so it is built with it.
# SL_WHISPER=OFF builds without it (dictation then reports it is not available).
include_guard()

include(Prebuilt)   # SL_3P_CACHE_DIR

option(SL_WHISPER "Local speech-to-text for VR dictation (whisper.cpp, #1118)" ON)

if (NOT SL_WHISPER OR DARWIN OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  return()
endif ()

set(WHISPER_VERSION 1.9.5)
set(WHISPER_SHA256 "ff1a9053feb509ff9d7729703355541ae9690073a6b1c40eb692c962e0dc1720")
set(WHISPER_SOURCE_URL "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v${WHISPER_VERSION}.tar.gz"
    CACHE STRING "whisper.cpp source release tarball (a mirror must serve the same bytes)")

set(WHISPER_SRC_PARENT "${CMAKE_BINARY_DIR}/3p-src")
set(WHISPER_ROOT "${WHISPER_SRC_PARENT}/whisper.cpp-${WHISPER_VERSION}")
set(_whisper_stamp "${WHISPER_SRC_PARENT}/whisper.cpp.extracted")
set(_whisper_archive "${SL_3P_CACHE_DIR}/whisper.cpp-${WHISPER_VERSION}.tar.gz")

set(_whisper_have "")
if (EXISTS "${_whisper_stamp}")
  file(READ "${_whisper_stamp}" _whisper_have)
  string(STRIP "${_whisper_have}" _whisper_have)
endif ()
if (NOT _whisper_have STREQUAL "${WHISPER_SHA256}" OR NOT EXISTS "${WHISPER_ROOT}/include/whisper.h")
  # The archive is cached under a name of its own (GitHub's tag archives are all called
  # v<version>.tar.gz, which would collide in the shared cache).
  set(_whisper_ok FALSE)
  if (EXISTS "${_whisper_archive}")
    file(SHA256 "${_whisper_archive}" _whisper_got)
    if (_whisper_got STREQUAL "${WHISPER_SHA256}")
      set(_whisper_ok TRUE)
    else ()
      message(WARNING "whisper.cpp: ${_whisper_archive} does not match its SHA256; downloading it again")
      file(REMOVE "${_whisper_archive}")
    endif ()
  endif ()
  if (NOT _whisper_ok)
    file(MAKE_DIRECTORY "${SL_3P_CACHE_DIR}")
    string(RANDOM LENGTH 8 _rnd)
    set(_part "${_whisper_archive}.part-${_rnd}")
    message(STATUS "whisper.cpp: downloading ${WHISPER_VERSION} <- ${WHISPER_SOURCE_URL}")
    file(DOWNLOAD "${WHISPER_SOURCE_URL}" "${_part}"
         STATUS _status
         TLS_VERIFY ON
         INACTIVITY_TIMEOUT 120
         EXPECTED_HASH SHA256=${WHISPER_SHA256})
    list(GET _status 0 _code)
    if (NOT _code EQUAL 0)
      list(GET _status 1 _msg)
      file(REMOVE "${_part}")
      message(FATAL_ERROR "whisper.cpp: failed to download ${WHISPER_SOURCE_URL}: ${_msg} "
                          "(or configure with -DSL_WHISPER=OFF to build without VR dictation)")
    endif ()
    file(RENAME "${_part}" "${_whisper_archive}")
  endif ()
  message(STATUS "whisper.cpp: unpacking ${WHISPER_VERSION}")
  file(REMOVE_RECURSE "${WHISPER_ROOT}")
  file(REMOVE "${_whisper_stamp}")
  file(ARCHIVE_EXTRACT INPUT "${_whisper_archive}" DESTINATION "${WHISPER_SRC_PARENT}")
  if (NOT EXISTS "${WHISPER_ROOT}/include/whisper.h")
    message(FATAL_ERROR "whisper.cpp: ${_whisper_archive} did not unpack to ${WHISPER_ROOT}")
  endif ()
  file(WRITE "${_whisper_stamp}" "${WHISPER_SHA256}\n")
endif ()

# ggml-base + ggml (minus ggml.cpp, see above).
set(_whisper_ggml_srcs
    ggml.c ggml-alloc.c ggml-backend.cpp ggml-backend-meta.cpp ggml-opt.cpp
    ggml-threading.cpp ggml-quants.c gguf.cpp
    ggml-backend-dl.cpp ggml-backend-reg.cpp)
# The CPU backend (GGML_CPU_SOURCES, x86, llamafile sgemm).
set(_whisper_cpu_srcs
    ggml-cpu.c ggml-cpu.cpp repack.cpp hbm.cpp quants.c traits.cpp
    amx/amx.cpp amx/mmq.cpp tiled/tiled.cpp tiled/tiled-kernel.cpp
    binary-ops.cpp unary-ops.cpp vec.cpp ops.cpp
    arch/x86/quants.c arch/x86/repack.cpp
    llamafile/sgemm.cpp)
set(_whisper_lib_srcs
    whisper.cpp aneforge/whisper-aneforge.cpp)

set(_whisper_srcs "")
foreach (_f IN LISTS _whisper_ggml_srcs)
  list(APPEND _whisper_srcs "${WHISPER_ROOT}/ggml/src/${_f}")
endforeach ()
foreach (_f IN LISTS _whisper_cpu_srcs)
  list(APPEND _whisper_srcs "${WHISPER_ROOT}/ggml/src/ggml-cpu/${_f}")
endforeach ()
foreach (_f IN LISTS _whisper_lib_srcs)
  list(APPEND _whisper_srcs "${WHISPER_ROOT}/src/${_f}")
endforeach ()
foreach (_p IN LISTS _whisper_srcs)
  if (NOT EXISTS "${_p}")
    message(FATAL_ERROR "whisper.cpp: ${_p} is missing (update the source lists in Whisper.cmake for ${WHISPER_VERSION})")
  endif ()
endforeach ()

# ggml.c includes ggml-version.h (upstream: configure_file of ggml-version.h.in).
set(_whisper_gen "${WHISPER_SRC_PARENT}/whisper.cpp-gen")
file(CONFIGURE OUTPUT "${_whisper_gen}/ggml-version.h" CONTENT
     "#pragma once\n\n#define GGML_VERSION \"whisper.cpp-${WHISPER_VERSION}\"\n#define GGML_COMMIT  \"v${WHISPER_VERSION}\"\n")

add_library(slwhisper MODULE ${_whisper_srcs})
target_include_directories(slwhisper PRIVATE
    "${WHISPER_ROOT}/ggml"
    "${WHISPER_ROOT}/ggml/src"
    "${WHISPER_ROOT}/ggml/src/ggml-cpu"
    "${WHISPER_ROOT}/src"
    "${_whisper_gen}")
target_include_directories(slwhisper SYSTEM PRIVATE
    "${WHISPER_ROOT}/include"
    "${WHISPER_ROOT}/ggml/include")
set_target_properties(slwhisper PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DISABLE_PRECOMPILE_HEADERS ON
    C_STANDARD 11
    CXX_STANDARD 17)
if (WINDOWS)
  # Beside the viewer's other DLLs (viewer_manifest.py packages it from there).
  set_target_properties(slwhisper PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${SHARED_LIB_STAGING_DIR}"
      RUNTIME_OUTPUT_DIRECTORY "${SHARED_LIB_STAGING_DIR}")
endif ()

# The viewer's directory-wide options are for its own code: /TP (compile everything as
# C++) must not reach the C files, /fp:fast breaks ggml's -INFINITY masks, and
# third-party warnings are not ours to fix under -Werror / /WX.
get_target_property(_whisper_opts slwhisper COMPILE_OPTIONS)
if (_whisper_opts)
  list(REMOVE_ITEM _whisper_opts /TP -TP /WX -WX -Werror /fp:fast)
  set_target_properties(slwhisper PROPERTIES COMPILE_OPTIONS "${_whisper_opts}")
endif ()

target_compile_definitions(slwhisper PRIVATE
    WHISPER_SHARED WHISPER_BUILD   # export the whisper_* API, nothing else
    GGML_SHARED GGML_BUILD         # ... and the ggml_* C API (log / abort hooks)
    GGML_USE_CPU                   # the CPU backend is registered statically (ggml-backend-reg.cpp)
    GGML_USE_CPU_REPACK
    GGML_USE_LLAMAFILE
    GGML_SCHED_MAX_COPIES=4
    WHISPER_VERSION="${WHISPER_VERSION}")
if (WINDOWS)
  target_compile_definitions(slwhisper PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else ()
  target_compile_definitions(slwhisper PRIVATE _XOPEN_SOURCE=600 _GNU_SOURCE)
endif ()

# ISA: upstream's "haswell" CPU variant (see the header).
target_compile_definitions(slwhisper PRIVATE
    GGML_SSE42 GGML_AVX GGML_AVX2 GGML_F16C GGML_FMA GGML_BMI2)
if (MSVC)
  # clang-cl: /arch:AVX2; FMA / F16C / BMI2 are not implied by it in clang-cl's macros,
  # so they are passed as clang flags (upstream defines __BMI2__ by hand for cl.exe).
  target_compile_options(slwhisper PRIVATE /arch:AVX2 /fp:precise /w)
  if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(slwhisper PRIVATE /clang:-mfma /clang:-mf16c /clang:-mbmi2)
  else ()
    target_compile_definitions(slwhisper PRIVATE __BMI2__)
  endif ()
else ()
  target_compile_options(slwhisper PRIVATE -msse4.2 -mavx -mavx2 -mf16c -mfma -mbmi2 -w)
endif ()

find_package(Threads REQUIRED)
target_link_libraries(slwhisper PRIVATE Threads::Threads)
if (LINUX)
  target_link_libraries(slwhisper PRIVATE m dl)
  # Exports: the C API only. The libstdc++ template instances (default visibility despite
  # -fvisibility=hidden) and GNU "unique" symbols stay local, so nothing in the module can
  # bind into the viewer's namespace and dlclose really unloads it.
  file(CONFIGURE OUTPUT "${_whisper_gen}/slwhisper.map" CONTENT
       "{\n  global: whisper_*; ggml_*;\n  local: *;\n};\n")
  target_link_options(slwhisper PRIVATE "LINKER:--version-script=${_whisper_gen}/slwhisper.map")
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(slwhisper PRIVATE -fno-gnu-unique)
  endif ()
  # The ISA note: v3 (= what it is compiled for), not the build host's crt objects' v4.
  include(Python)
  add_custom_command(TARGET slwhisper POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/fix_isa_note.py $<TARGET_FILE:slwhisper> 7
    VERBATIM)
endif ()

# For the viewer: <whisper.h> for its types only (every call goes through the module,
# looked up at run time), and LL_WHISPER=1.
if (NOT TARGET ll::whisper)
  add_library(ll::whisper INTERFACE IMPORTED)
  target_include_directories(ll::whisper SYSTEM INTERFACE
      "${WHISPER_ROOT}/include"
      "${WHISPER_ROOT}/ggml/include")
  target_compile_definitions(ll::whisper INTERFACE LL_WHISPER=1)
endif ()
