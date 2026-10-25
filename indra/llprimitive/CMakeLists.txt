# -*- cmake -*-

project(llprimitive)

include(00-Common)
include(LLCommon)
include(LLCoreHttp)
include(LLPhysicsExtensions)
include(LLPrimitive)
include(GLM)
include(TinyGLTF)

set(llprimitive_SOURCE_FILES
    lldaeloader.cpp
    llfbxloader.cpp
    llgltfmaterial.cpp
    llmaterialid.cpp
    llmaterial.cpp
    llmaterialtable.cpp
    llmediaentry.cpp
    llmodel.cpp
    llmodelloader.cpp
    llprimitive.cpp
    llprimtexturelist.cpp
    lltextureanim.cpp
    lltextureentry.cpp
    lltreeparams.cpp
    llufbx.cpp
    llvolumemessage.cpp
    material_codes.cpp
    )

set(llprimitive_HEADER_FILES
    CMakeLists.txt
    lldaeloader.h
    llfbxloader.h
    llgltfmaterial.h
    llgltfmaterial_templates.h
    legacy_object_types.h
    llmaterial.h
    llmaterialid.h
    llmaterialtable.h
    llmediaentry.h
    llmodel.h
    llmodelloader.h
    llprimitive.h
    llprimtexturelist.h
    lllslconstants.h
    lltextureanim.h
    lltextureentry.h
    lltreeparams.h
    lltree_common.h
    llvolumemessage.h
    material_codes.h
    object_flags.h
    )

list(APPEND llprimitive_SOURCE_FILES ${llprimitive_HEADER_FILES})

# The ufbx implementation TU (FBX mesh upload, llfbxloader.cpp): vendored
# third-party C compiled as C++ (third_party/ufbx/README.md). No precompiled
# header, and its warnings are not ours to fix (-Werror / /WX would trip on new
# compilers): warnings off for this one file only. MSVC builds use /fp:fast
# globally; the FBX parser gets precise floating point.
if (MSVC)
  set(LL_UFBX_COMPILE_FLAGS "/w /fp:precise")
else ()
  set(LL_UFBX_COMPILE_FLAGS "-w")
endif ()
set_source_files_properties(
    llufbx.cpp
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS TRUE
        COMPILE_FLAGS "${LL_UFBX_COMPILE_FLAGS}")

add_library (llprimitive ${llprimitive_SOURCE_FILES})
target_include_directories( llprimitive  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llprimitive
        llcommon
        llmath
        llmessage
        llcorehttp
        llxml
        llcharacter
        llrender
        llphysicsextensions_impl
        ll::colladadom
        ll::glm
        )

if (TARGET llconvexdecomposition)
    target_link_libraries(llprimitive
            llconvexdecomposition
            )
endif ()

if (USE_PRECOMPILED_HEADERS)
    target_precompile_headers(llprimitive REUSE_FROM llprecompiled)
endif ()

#add unit tests
if (LL_TESTS)
    INCLUDE(LLAddBuildTest)
    SET(llprimitive_TEST_SOURCE_FILES
      llmediaentry.cpp
      llprimitive.cpp
      llgltfmaterial.cpp
      )

    set_property(SOURCE llprimitive.cpp PROPERTY LL_TEST_ADDITIONAL_LIBRARIES llmessage)
    LL_ADD_PROJECT_UNIT_TESTS(llprimitive "${llprimitive_TEST_SOURCE_FILES}")
endif (LL_TESTS)
