# -*- cmake -*-
include(Linking)
include(Prebuilt)

include_guard()
add_library( ll::vorbis INTERFACE IMPORTED )

use_system_binary(vorbis)
use_prebuilt_binary(ogg_vorbis)
target_include_directories( ll::vorbis SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include )

find_library(OGG_LIBRARY
    NAMES
    libogg.lib
    libogg.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBIS_LIBRARY
    NAMES
    libvorbis.lib
    libvorbis.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBISENC_LIBRARY
    NAMES
    libvorbisenc.lib
    libvorbisenc.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBISFILE_LIBRARY
    NAMES
    libvorbisfile.lib
    libvorbisfile.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::vorbis INTERFACE ${VORBISENC_LIBRARY} ${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY} )


# ---- Optional Opus decoding (sound uploader) ---------------------------------------------
# Enabled automatically once an "opusfile" package (libopus + libopusfile, built without
# HTTP/URL support, headers in include/opus/) is added to cmake/3p_manifest.json. Never
# falls back to system libraries, so the build stays portable.
file(READ "${SL_3P_MANIFEST}" _sl_manifest_text)
string(FIND "${_sl_manifest_text}" "\"opusfile\":" _sl_opusfile_pos)
set(_sl_have_opusfile FALSE)
if (NOT _sl_opusfile_pos EQUAL -1)
  use_prebuilt_binary(opusfile)
  find_library(OPUSFILE_LIBRARY NAMES libopusfile.lib libopusfile.a opusfile.lib
      PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" NO_DEFAULT_PATH)
  find_library(OPUS_LIBRARY NAMES libopus.lib libopus.a opus.lib
      PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" NO_DEFAULT_PATH)
  find_path(OPUSFILE_INCLUDE_DIR opusfile.h
      PATHS "${LIBS_PREBUILT_DIR}/include/opus" NO_DEFAULT_PATH)
  if (OPUSFILE_LIBRARY AND OPUS_LIBRARY AND OPUSFILE_INCLUDE_DIR)
    set(_sl_have_opusfile TRUE)
  else ()
    message(WARNING "opusfile package installed but libraries/headers not found; Opus upload disabled")
  endif ()
endif ()
if (_sl_have_opusfile)
  message(STATUS "Opus decoding for sound upload: ENABLED")
  add_library( ll::opusfile INTERFACE IMPORTED )
  target_include_directories(ll::opusfile SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include ${OPUSFILE_INCLUDE_DIR})
  # opusfile needs libogg (provided by ll::vorbis) after it on the link line.
  target_link_libraries(ll::opusfile INTERFACE ${OPUSFILE_LIBRARY} ${OPUS_LIBRARY} ${OGG_LIBRARY})
else ()
  message(STATUS "Opus decoding for sound upload: disabled (no opusfile 3p package)")
endif ()
