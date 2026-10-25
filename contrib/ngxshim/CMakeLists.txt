# slipstream_ngx: the optional DLSS shim (Super Resolution + Ray Reconstruction) for the Slipstream viewer.
# SPDX-License-Identifier: MIT -- see LICENSE. Standalone project: NOT part of the viewer build.
#
#   cmake -S contrib/ngxshim -B build-ngxshim -DDLSS_SDK_DIR=/path/to/your/DLSS -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-ngxshim --config Release
#
# DLSS_SDK_DIR is your own download of https://github.com/NVIDIA/DLSS (you accept NVIDIA's
# licence by downloading it). Nothing from it is stored in this repository.
cmake_minimum_required(VERSION 3.16)
project(slipstream_ngx VERSION 1.2.0 LANGUAGES CXX)

set(DLSS_SDK_DIR "" CACHE PATH "Path to your checkout / download of github.com/NVIDIA/DLSS")
set(DLSS_SDK_VERSION "" CACHE STRING "Label for the SDK version (e.g. 310.9.16); detected from the feature library name when empty")

if(NOT DLSS_SDK_DIR OR NOT EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h")
  message(FATAL_ERROR "Set -DDLSS_SDK_DIR=<your download of github.com/NVIDIA/DLSS> (expected include/nvsdk_ngx_vk.h in it)")
endif()

find_package(Vulkan REQUIRED)   # headers only; the shim never links the Vulkan loader

if(WIN32)
  set(_plat "Windows_x86_64")
  # /MT -> nvsdk_ngx_s.lib, /MD -> nvsdk_ngx_d.lib (NVIDIA's Programming Guide 5.1.1).
  option(SLIP_NR_STATIC_CRT "Link the static MSVC runtime (/MT)" OFF)
  if(SLIP_NR_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(_ngx_lib_name nvsdk_ngx_s.lib)
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(_ngx_lib_name nvsdk_ngx_d.lib)
  endif()
  set(_ngx_lib "${DLSS_SDK_DIR}/lib/${_plat}/x64/${_ngx_lib_name}")
  if(NOT EXISTS "${_ngx_lib}")
    set(_ngx_lib "${DLSS_SDK_DIR}/lib/${_plat}/${_ngx_lib_name}")
  endif()
  file(GLOB _feature_libs "${DLSS_SDK_DIR}/lib/${_plat}/rel/nvngx_dlss.dll" "${DLSS_SDK_DIR}/lib/${_plat}/rel/nvngx_dlssd.dll")
  set(_out_name slipstream_ngx)
else()
  set(_plat "Linux_x86_64")
  set(_ngx_lib "${DLSS_SDK_DIR}/lib/${_plat}/libnvsdk_ngx.a")
  file(GLOB _feature_libs "${DLSS_SDK_DIR}/lib/${_plat}/rel/libnvidia-ngx-dlss.so.*"
                          "${DLSS_SDK_DIR}/lib/${_plat}/rel/libnvidia-ngx-dlssd.so.*")
  set(_out_name libslipstream_ngx)
endif()
if(NOT EXISTS "${_ngx_lib}")
  message(FATAL_ERROR "NGX loader library not found: ${_ngx_lib}")
endif()

if(NOT DLSS_SDK_VERSION)
  foreach(_f ${_feature_libs})
    if(_f MATCHES "libnvidia-ngx-dlssd?\\.so\\.([0-9.]+)$")
      set(DLSS_SDK_VERSION "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()
if(NOT DLSS_SDK_VERSION)
  set(DLSS_SDK_VERSION "unknown")
endif()

add_library(slipstream_ngx SHARED src/slipstream_ngx.cpp include/slipstream_ngx.h)
set_target_properties(slipstream_ngx PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  PREFIX ""                       # exactly libslipstream_ngx.so / slipstream_ngx.dll
  OUTPUT_NAME "${_out_name}")
target_include_directories(slipstream_ngx PRIVATE include "${DLSS_SDK_DIR}/include" ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(slipstream_ngx PRIVATE SLIP_NR_SDK_VERSION="${DLSS_SDK_VERSION}")
target_link_libraries(slipstream_ngx PRIVATE "${_ngx_lib}")
if(NOT WIN32)
  target_link_libraries(slipstream_ngx PRIVATE ${CMAKE_DL_LIBS} stdc++)
  target_link_options(slipstream_ngx PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map"
    "-Wl,--exclude-libs,ALL" "-Wl,--no-undefined")
endif()

message(STATUS "slipstream_ngx: DLSS SDK ${DLSS_SDK_VERSION} at ${DLSS_SDK_DIR}")
message(STATUS "slipstream_ngx: install the built library next to the viewer's libraries together with "
               "the NVIDIA feature libraries you want (dlss = Super Resolution, dlssd = Ray Reconstruction): ${_feature_libs}")
