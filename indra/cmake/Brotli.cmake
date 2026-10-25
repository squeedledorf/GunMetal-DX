# -*- cmake -*-

include(Prebuilt)
include(Linking)

include_guard()
add_library( ll::brotli INTERFACE IMPORTED )

if(USE_CONAN )
  target_link_libraries( ll::brotli INTERFACE CONAN_PKG::zlib )
  return()
endif()

use_prebuilt_binary(brotli)

find_library(BROTLI_DEC_LIBRARY
    NAMES
    brotlidec
    PATHS "${ARCH_PREBUILT_DIRS_ARCH_RELEASE}" "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(BROTLI_ENC_LIBRARY
    NAMES
    brotlienc
    PATHS "${ARCH_PREBUILT_DIRS_ARCH_RELEASE}" "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(BROTLI_COMMON_LIBRARY
    NAMES
    brotlicommon
    PATHS "${ARCH_PREBUILT_DIRS_ARCH_RELEASE}" "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::brotli INTERFACE ${BROTLI_DEC_LIBRARY} ${BROTLI_ENC_LIBRARY} ${BROTLI_COMMON_LIBRARY})

target_include_directories( ll::brotli SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)
