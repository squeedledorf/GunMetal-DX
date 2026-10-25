# vkclean — standalone native Vulkan clean-renderer prototype.
#
# Independent of the Alchemy viewer build (like vktest). It reuses vktest's thin
# Vulkan-native helper layer (vk_device / vk_offscreen / vk_pipeline — Vulkan 1.3,
# dynamic rendering, sync2, BDA, descriptor-indexing/bindless, timeline sems,
# drawIndirectCount) and adds the clean-pipeline prototype on top.
#
# Build:
#   cmake -S indra/vkclean -B build-vkclean -G Ninja
#   cmake --build build-vkclean
#   ./build-vkclean/vkclean --out golden/vkclean.png
cmake_minimum_required(VERSION 3.20)
project(vkclean LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(glm QUIET)

# Reuse the vktest thin helper layer (the acceptable Vulkan-native abstraction).
set(VKTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vktest)

add_executable(vkclean
    main.cpp
    clean_scene.cpp
    ${VKTEST_DIR}/vk_device.cpp
    ${VKTEST_DIR}/vk_offscreen.cpp
    ${VKTEST_DIR}/vk_pipeline.cpp
)

target_include_directories(vkclean PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VKTEST_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

# vk_offscreen.cpp pulls stb_image_write via "third_party/stb_image_write.h";
# it resolves relative to that .cpp's own directory, so vktest/third_party is
# on the include path implicitly. We also keep our own copy for standalone use.
target_include_directories(vkclean PRIVATE ${VKTEST_DIR})

target_link_libraries(vkclean PRIVATE Vulkan::Vulkan)

if (TARGET glm::glm)
    target_link_libraries(vkclean PRIVATE glm::glm)
else()
    find_path(GLM_INCLUDE_DIR glm/glm.hpp)
    if (GLM_INCLUDE_DIR)
        target_include_directories(vkclean PRIVATE ${GLM_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "glm not found")
    endif()
endif()

# ---- Shader compilation: GLSL -> SPIR-V via glslc ----------------------------
find_program(GLSLC glslc REQUIRED)

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADERS
    clean_cull.comp
    clean_draw.vert
    clean_draw.frag
)

set(SPV_OUTPUTS)
foreach(SH ${SHADERS})
    set(SRC ${SHADER_SRC_DIR}/${SH})
    set(OUT ${SHADER_OUT_DIR}/${SH}.spv)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLC} --target-env=vulkan1.3 ${SRC} -o ${OUT}
        DEPENDS ${SRC}
        COMMENT "glslc ${SH}"
        VERBATIM
    )
    list(APPEND SPV_OUTPUTS ${OUT})
endforeach()

add_custom_target(vkclean_shaders DEPENDS ${SPV_OUTPUTS})
add_dependencies(vkclean vkclean_shaders)
