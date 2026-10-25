# -*- cmake -*-
#
# EXPERIMENTAL Vulkan integration for the viewer (vkgfx module).
#
# Provides the ll::vulkan interface target: Vulkan headers + the Vulkan loader.
# GL remains the default renderer; this only supplies the toolchain bits the
# vkgfx sources compile/link against. If Vulkan is not found the build still
# succeeds — the module is compiled but will fail at runtime only if the user
# explicitly enables RenderUseVulkan without a Vulkan driver.
cmake_minimum_required( VERSION 3.13 FATAL_ERROR )

include_guard()

add_library( ll::vulkan INTERFACE IMPORTED )

# Prefer CMake's FindVulkan (system SDK / loader). This is a de-risking spike;
# on the target build hosts (and CI Linux) the Vulkan loader + headers are
# present. We do not vendor a 3p-vulkan package for this milestone.
find_package( Vulkan QUIET )

if (Vulkan_FOUND)
    target_link_libraries( ll::vulkan INTERFACE Vulkan::Vulkan )
    message(STATUS "vkgfx: Vulkan found (${Vulkan_LIBRARY})")
else ()
    # Fall back to the well-known loader name so the link still resolves on
    # systems where FindVulkan's config is incomplete but libvulkan exists.
    find_library( VULKAN_LOADER NAMES vulkan vulkan-1 )
    if (VULKAN_LOADER)
        target_link_libraries( ll::vulkan INTERFACE ${VULKAN_LOADER} )
        message(STATUS "vkgfx: using Vulkan loader ${VULKAN_LOADER} (headers assumed on default include path)")
    else ()
        message(WARNING "vkgfx: Vulkan loader NOT found; RenderUseVulkan will not work at runtime")
    endif ()
endif ()
