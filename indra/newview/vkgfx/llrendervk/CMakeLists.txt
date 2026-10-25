# llrendervk — standalone offscreen de-risk of LLRender's immediate-mode API on
# Vulkan. Independent of the viewer build (like indra/vktest). Builds one exe
# `llrendervk_test` and compiles the UI shaders to SPIR-V with glslc.
#
#   cmake -S indra/newview/vkgfx/llrendervk -B build-llrendervk -G Ninja
#   cmake --build build-llrendervk
#   ./build-llrendervk/llrendervk_test --out /tmp/uiframe.png \
#       --shaders build-llrendervk/shaders
cmake_minimum_required(VERSION 3.20)
project(llrendervk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(glm QUIET)

# Shared facade + RHI sources (both test exes link these).
set(LLRENDERVK_SRC
    llrendervk.cpp
    vk_pipeline_ui.cpp
    vk_device.cpp
    vk_offscreen.cpp
)

add_executable(llrendervk_test llrendervk_test.cpp ${LLRENDERVK_SRC})

# ui_text_test: full state parity + font/text glyph-quad path.
add_executable(ui_text_test ui_text_test.cpp ${LLRENDERVK_SRC})

foreach(EXE llrendervk_test ui_text_test)
    target_include_directories(${EXE} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Vulkan_INCLUDE_DIRS}
    )
    target_link_libraries(${EXE} PRIVATE Vulkan::Vulkan)

    if (TARGET glm::glm)
        target_link_libraries(${EXE} PRIVATE glm::glm)
    else()
        find_path(GLM_INCLUDE_DIR glm/glm.hpp)
        if (GLM_INCLUDE_DIR)
            target_include_directories(${EXE} PRIVATE ${GLM_INCLUDE_DIR})
        else()
            message(FATAL_ERROR "glm not found")
        endif()
    endif()
endforeach()

# ---- shaders: GLSL -> SPIR-V ------------------------------------------------
find_program(GLSLC glslc REQUIRED)
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADERS ui.vert ui.frag)
set(SPV_OUTPUTS)
foreach(SH ${SHADERS})
    add_custom_command(
        OUTPUT ${SHADER_OUT_DIR}/${SH}.spv
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O
                ${SHADER_SRC_DIR}/${SH} -o ${SHADER_OUT_DIR}/${SH}.spv
        DEPENDS ${SHADER_SRC_DIR}/${SH}
        COMMENT "glslc ${SH}"
        VERBATIM)
    list(APPEND SPV_OUTPUTS ${SHADER_OUT_DIR}/${SH}.spv)
endforeach()
add_custom_target(llrendervk_shaders DEPENDS ${SPV_OUTPUTS})
add_dependencies(llrendervk_test llrendervk_shaders)
add_dependencies(ui_text_test llrendervk_shaders)
