# -*- cmake -*-

# Segment store: one budgeted, crash-safe, append-only disk cache (#715, #708, #709).
# Depends on llcommon only (LLUUID, logging, HBXXH64, LLCacheCodec).

project(llcachestore)

include(00-Common)
include(LLCommon)

set(llcachestore_SOURCE_FILES
    llcachestore.cpp
    llcachestore_format.cpp
    llcachestore_index.cpp
    llcachestore_platform.cpp
    )

set(llcachestore_HEADER_FILES
    CMakeLists.txt
    llcachestore.h
    llcachestore_format.h
    llcachestore_index.h
    llcachestore_platform.h
    llcachestore_workqueue.h
    )

list(APPEND llcachestore_SOURCE_FILES ${llcachestore_HEADER_FILES})

add_library (llcachestore ${llcachestore_SOURCE_FILES})

target_link_libraries(llcachestore
        llcommon
    )
target_include_directories( llcachestore  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

if (USE_PRECOMPILED_HEADERS)
    target_precompile_headers(llcachestore REUSE_FROM llprecompiled)
endif ()

# Add tests
if (LL_TESTS)
    include(LLAddBuildTest)
    # INTEGRATION TESTS (filesystem heavy: crash/truncation, two instances, disk full, eviction)
    set(test_libs llcachestore llcommon)
    LL_ADD_INTEGRATION_TEST(llcachestore "" "${test_libs}")
endif (LL_TESTS)
