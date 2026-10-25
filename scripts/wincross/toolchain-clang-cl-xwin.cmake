# Cross-compile the viewer for Windows x64 from Linux: clang-cl + lld-link against
# the MSVC CRT / Windows SDK splatted by xwin (XWIN_ROOT, default ~/.xwin).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

if (NOT XWIN_ROOT)
  if (DEFINED ENV{XWIN_ROOT})
    set(XWIN_ROOT $ENV{XWIN_ROOT})
  else ()
    set(XWIN_ROOT $ENV{HOME}/.xwin)
  endif ()
endif ()

set(_inc "/imsvc${XWIN_ROOT}/crt/include /imsvc${XWIN_ROOT}/sdk/include/ucrt /imsvc${XWIN_ROOT}/sdk/include/um /imsvc${XWIN_ROOT}/sdk/include/shared /imsvc${XWIN_ROOT}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc}")
set(CMAKE_RC_FLAGS_INIT "-I${XWIN_ROOT}/sdk/include/um -I${XWIN_ROOT}/sdk/include/shared -I${XWIN_ROOT}/crt/include -I${XWIN_ROOT}/sdk/include/ucrt")
set(_lib "/libpath:${XWIN_ROOT}/crt/lib/x86_64 /libpath:${XWIN_ROOT}/sdk/lib/um/x86_64 /libpath:${XWIN_ROOT}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

# Only search the target sysroot / packages for libraries, never the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CROSS_WINDOWS ON CACHE BOOL "Cross-compiling for Windows with clang-cl/xwin")

# xwin splats the release CRT only (no msvcrtd): compile checks and every target
# use the release DLL runtime (/MD), which the 3p packages are built against too.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
