# -*- cmake -*-

project(llaudio)

include(00-Common)
include(LLAudio)
include(OPENAL)
include(LLCommon)

set(llaudio_SOURCE_FILES
    llaudioengine.cpp
    lllistener.cpp
    llaudiodecodemgr.cpp
    llsoundcache.cpp
    llvorbisencode.cpp
    llaudiofiledecode.cpp
    )

set(llaudio_HEADER_FILES
    CMakeLists.txt

    llaudioengine.h
    lllistener.h
    llaudiodecodemgr.h
    llsoundcache.h
    llvorbisencode.h
    llaudiofiledecode.h
    llwindgen.h
    )


if (TARGET ll::openal)
  list(APPEND llaudio_SOURCE_FILES
    llaudioengine_openal.cpp
    lllistener_openal.cpp
    )
  
  list(APPEND llaudio_HEADER_FILES
    llaudioengine_openal.h
    lllistener_openal.h
    )
endif ()

list(APPEND llaudio_SOURCE_FILES ${llaudio_HEADER_FILES})

add_library (llaudio ${llaudio_SOURCE_FILES})
target_include_directories( llaudio  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries( llaudio
        llcommon
        llmath
        llmessage
        llfilesystem
        ll::vorbis
    )

# Vendored single-header FLAC / MP3 decoders (dr_libs, public domain / MIT-0). Added as a
# SYSTEM include dir so their warnings do not trip -Werror.
target_include_directories(llaudio SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs)
# dr_flac relies on wrapping signed arithmetic when fed corrupt streams. Per-file flags
# would mismatch the shared PCH, so this one TU skips it.
set_source_files_properties(llaudiofiledecode.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if (MSVC)
  # clang-cl (the Windows cross build): pass the GCC-style flag through.
  set_source_files_properties(llaudiofiledecode.cpp PROPERTIES COMPILE_OPTIONS "/clang:-fwrapv")
else ()
  set_source_files_properties(llaudiofiledecode.cpp PROPERTIES COMPILE_OPTIONS -fwrapv)
endif ()

# Opus decoding is optional: enabled only when the opusfile 3p package is installed
# (see cmake/Audio.cmake). Without it, .opus files are rejected with a clear message.
if (TARGET ll::opusfile)
  target_link_libraries(llaudio ll::opusfile)
  set_source_files_properties(llaudiofiledecode.cpp PROPERTIES COMPILE_DEFINITIONS LL_OPUSFILE=1)
endif ()

if( TARGET ll::openal )
    target_link_libraries( llaudio ll::openal )
endif()

if (USE_PRECOMPILED_HEADERS)
    target_precompile_headers(llaudio REUSE_FROM llprecompiled)
endif ()
