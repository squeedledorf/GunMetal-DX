# vktest — standalone Vulkan de-risking spike.
#
# This is INTENTIONALLY independent of the Alchemy viewer build. It builds a
# single tiny executable `vktest` and compiles GLSL -> SPIR-V with glslc at
# build time. It does not touch indra/'s CMake or the viewer targets.
#
# Build:
#   cmake -S indra/vktest -B build-vktest -G Ninja
#   cmake --build build-vktest
#   ./build-vktest/vktest --scene clear --out /tmp/clear.png
cmake_minimum_required(VERSION 3.20)
project(vktest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# glm: prefer a CMake package; fall back to a plain include dir (header-only).
find_package(glm QUIET)

add_executable(vktest
    main.cpp
    vk_device.cpp
    vk_offscreen.cpp
    vk_pipeline.cpp
    scenes.cpp
    scenes_gpu.cpp
)

target_include_directories(vktest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vktest PRIVATE Vulkan::Vulkan)

if (TARGET glm::glm)
    target_link_libraries(vktest PRIVATE glm::glm)
else()
    # header-only fallback: glm.hpp is under a system include dir already.
    find_path(GLM_INCLUDE_DIR glm/glm.hpp)
    if (GLM_INCLUDE_DIR)
        target_include_directories(vktest PRIVATE ${GLM_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "glm not found")
    endif()
endif()

# ---- Shader compilation: GLSL -> SPIR-V via glslc --------------------------
find_program(GLSLC glslc REQUIRED)

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADERS
    triangle.vert triangle.frag
    texquad.vert  texquad.frag
    litcube.vert  litcube.frag
    gpucubes.vert gpucubes.frag
    cull.comp
)

set(SPV_OUTPUTS)
foreach(SH ${SHADERS})
    set(SRC ${SHADER_SRC_DIR}/${SH})
    set(OUT ${SHADER_OUT_DIR}/${SH}.spv)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O ${SRC} -o ${OUT}
        DEPENDS ${SRC}
        COMMENT "glslc ${SH}"
        VERBATIM
    )
    list(APPEND SPV_OUTPUTS ${OUT})
endforeach()

add_custom_target(vktest_shaders DEPENDS ${SPV_OUTPUTS})
add_dependencies(vktest vktest_shaders)
