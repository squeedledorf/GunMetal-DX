# -*- cmake -*-
include(Prebuilt)

# llphysicsextensions: the open stub package (source only, built here). Linden Lab's
# Havok-backed variants (llphysicsextensions_source / _tpv) are licensed and not used.

include_guard()
add_library( llphysicsextensions_impl INTERFACE IMPORTED )

use_prebuilt_binary(llphysicsextensions_stub)
set(LLPHYSICSEXTENSIONS_SRC_DIR ${LIBS_PREBUILT_DIR}/llphysicsextensions/stub)
target_link_libraries( llphysicsextensions_impl INTERFACE llphysicsextensionsstub)

target_include_directories( llphysicsextensions_impl INTERFACE   ${LIBS_PREBUILT_DIR}/include/llphysicsextensions)
