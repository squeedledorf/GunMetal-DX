# ReShade FX effect compiler (#543, FX P0): the vendored effect-compiler part of
# crosire/reshade v6.8.0 (BSD-3-Clause, see LICENSE.md / NOTICE / README.md) + the
# Khronos SPIR-V headers it includes (MIT-style, include/spirv/LICENSE.SPIRV-Headers).
#
# A static library outside the viewer's precompiled header. Third-party code: warnings
# off for these files only (the viewer builds with -Werror / /WX), and C++17 -- upstream's
# language level; under C++20 path::u8string() returns std::u8string, which its Windows
# branch does not expect. The flag is a compile option so it lands after the global
# -std=c++20 / /std:c++20 in CMAKE_CXX_FLAGS (the last one wins).

add_library(slreshadefx STATIC
    src/effect_codegen_spirv.cpp
    src/effect_expression.cpp
    src/effect_lexer.cpp
    src/effect_parser_exp.cpp
    src/effect_parser_stmt.cpp
    src/effect_preprocessor.cpp
    src/effect_symbol_table.cpp
    src/effect_codegen.hpp
    src/effect_expression.hpp
    src/effect_lexer.hpp
    src/effect_module.hpp
    src/effect_parser.hpp
    src/effect_preprocessor.hpp
    src/effect_symbol_table.hpp
    src/effect_symbol_table_intrinsics.inl
    src/effect_token.hpp
    include/spirv/spirv.hpp
    include/spirv/GLSL.std.450.h
    )
target_include_directories(slreshadefx
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/spirv)
set_target_properties(slreshadefx PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    DISABLE_PRECOMPILE_HEADERS ON)
if (MSVC)
  target_compile_options(slreshadefx PRIVATE /std:c++17 /w)
  target_compile_definitions(slreshadefx PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else ()
  target_compile_options(slreshadefx PRIVATE -std=c++17 -w)
endif ()

# Developer smoke test (not built by default): `ninja slfxc`, then
#   slfxc --vk -I indra/newview/app_settings/fx/Shaders -I <pack>/Shaders <pack>/Shaders/*.fx
# compiles each effect through the viewer's front end (vkgfx/llfxcompile.cpp), runs
# spirv-val (when on PATH) and creates the shader modules on a real device.
add_executable(slfxc EXCLUDE_FROM_ALL
    tools/slfxc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../llfxcompile.cpp)
target_include_directories(slfxc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(slfxc slreshadefx)
set_target_properties(slfxc PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
if (TARGET ll::vulkan)
  target_link_libraries(slfxc ll::vulkan)
  target_compile_definitions(slfxc PRIVATE SLFXC_WITH_VULKAN=1)
endif ()
