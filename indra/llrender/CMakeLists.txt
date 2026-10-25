# -*- cmake -*-

project(llrender)

include(00-Common)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)

set(llrender_SOURCE_FILES
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgl.cpp
    llgpuinfo.cpp
    llgltexture.cpp
    llrender.cpp
    llrenderrecorder.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    lltexture.cpp
    lltexturedata.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    )
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgl.h
    llgpuinfo.h
    llgltexture.h
    llrender.h
    llrenderrecorder.h
    llrender2dutils.h
    llrendernavprim.h
    lltexture.h
    lltexturedata.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories( llrender  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        )

if (USE_PRECOMPILED_HEADERS)
 target_precompile_headers(llrender REUSE_FROM llprecompiled)
endif ()
