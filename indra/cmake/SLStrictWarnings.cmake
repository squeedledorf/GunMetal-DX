# -*- cmake -*-
#
# Strict warning tier for Slipstream's OWN code (vkgfx, clean pipeline, sl* modules, ...).
# LL-derived code stays on the baseline -Wall -Werror from 00-Common.cmake; this adds
# a second, stricter set of warnings to an explicit list of our source files only.
#
#   SL_STRICT_WARNINGS        (default ON)   apply the tier at all
#   SL_STRICT_WARNINGS_FATAL  (default OFF)  make the tier's warnings errors
#
# The tier starts non-fatal: its warnings are reported but each is demoted with
# -Wno-error=<name>, so the baseline -Werror keeps guarding everything else. Measure the
# count, fix it, then flip SL_STRICT_WARNINGS_FATAL on. GCC_DISABLE_FATAL_WARNINGS (the
# newer-compiler escape hatch) also keeps the tier non-fatal.
#
# Not in the tier on purpose:
#   -Wconversion / -Wsign-conversion  would be thousands of hits; follow-up, per module.
#   -Wunused-parameter, -Wmissing-field-initializers  (both in -Wextra) are switched off:
#       Vulkan create-info structs are brace-initialised with only sType, and interface
#       overrides legitimately ignore parameters.
#   -Wsign-compare stays off like the baseline.
#
# Windows (clang-cl / MSVC) is untouched: the tier is a no-op there.

option(SL_STRICT_WARNINGS "Apply the stricter warning tier to Slipstream's own source files" ON)
option(SL_STRICT_WARNINGS_FATAL "Make the strict warning tier fatal (-Werror)" OFF)

# The tier's warnings, per compiler. Both lists name every warning group individually so
# that the non-fatal mode can demote exactly them (-Wextra's members included).
set(_sl_strict_gcc_enable
  -Wextra -Wshadow -Wnon-virtual-dtor -Wnull-dereference -Wduplicated-cond
  -Wduplicated-branches -Wlogical-op -Wuseless-cast -Wimplicit-fallthrough
  -Wno-unused-parameter -Wno-missing-field-initializers)
# -Wextra's members that are not also in the baseline -Wall; demoting these leaves the
# baseline errors (unused variables, format, ...) fatal.
set(_sl_strict_gcc_nonfatal
  -Wno-error=extra -Wno-error=cast-function-type -Wno-error=clobbered -Wno-error=deprecated-copy
  -Wno-error=empty-body -Wno-error=ignored-qualifiers -Wno-error=implicit-fallthrough
  -Wno-error=redundant-move -Wno-error=shift-negative-value -Wno-error=type-limits
  -Wno-error=unused-but-set-parameter -Wno-error=shadow -Wno-error=non-virtual-dtor
  -Wno-error=null-dereference -Wno-error=duplicated-cond -Wno-error=duplicated-branches
  -Wno-error=logical-op -Wno-error=useless-cast)

# Clang has no -Wlogical-op, -Wduplicated-* or -Wuseless-cast. Its groups propagate
# -Wno-error=<group> to their members.
set(_sl_strict_clang_enable
  -Wextra -Wshadow -Wnon-virtual-dtor -Wnull-dereference -Wimplicit-fallthrough
  -Wno-unused-parameter -Wno-missing-field-initializers)
set(_sl_strict_clang_nonfatal
  -Wno-error=extra -Wno-error=shadow -Wno-error=non-virtual-dtor
  -Wno-error=null-dereference -Wno-error=implicit-fallthrough)

# sl_apply_strict_warnings(<source files...>)
# Appends the strict tier to the COMPILE_OPTIONS of each source file (paths relative to
# the calling CMakeLists.txt, as for set_source_files_properties).
function(sl_apply_strict_warnings)
  if (NOT SL_STRICT_WARNINGS OR NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" OR MSVC OR NOT ARGN)
    return()
  endif ()

  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_enable ${_sl_strict_gcc_enable})
    set(_nonfatal ${_sl_strict_gcc_nonfatal})
  elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_enable ${_sl_strict_clang_enable})
    set(_nonfatal ${_sl_strict_clang_nonfatal})
  else ()
    return()
  endif ()

  if (SL_STRICT_WARNINGS_FATAL AND NOT GCC_DISABLE_FATAL_WARNINGS AND NOT CLANG_DISABLE_FATAL_WARNINGS)
    set(_opts ${_enable} -Werror)
  else ()
    set(_opts ${_enable} ${_nonfatal})
  endif ()

  # Only C++ sources are listed, but guard on the language anyway: several of these
  # options are C++-only.
  list(TRANSFORM _opts PREPEND "$<$<COMPILE_LANGUAGE:CXX>:" )
  list(TRANSFORM _opts APPEND ">")
  set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${_opts})
endfunction()

# sl_collect_strict_sources(<out var> <relative glob>...)
# Expands (non-recursive) globs, relative to the calling CMakeLists.txt, into a list of
# existing files, dropping anything under a third_party directory. Name each directory
# explicitly (e.g. vkgfx/*.cpp and vkgfx/llrendervk/*.cpp); re-globs on every configure.
function(sl_collect_strict_sources out_var)
  set(_found)
  foreach (_pattern IN LISTS ARGN)
    file(GLOB _hits CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
    list(APPEND _found ${_hits})
  endforeach ()
  list(FILTER _found EXCLUDE REGEX "(^|/)third_party/")
  list(REMOVE_DUPLICATES _found)
  set(${out_var} ${_found} PARENT_SCOPE)
endfunction()
