# -*- cmake -*-
#
# Fuzzing build (-DLL_FUZZ=ON): instruments every library in the tree with ASan + UBSan and
# libFuzzer's coverage hooks, so the harnesses in indra/fuzz find bugs inside llcommon,
# llmessage, llimage, ... and not only in the harness files.
#
# Opt-in, Linux + clang only. Use the `linux-fuzz` preset (own build dir, clang, no -Werror,
# no PCH); never configure this into the normal build directory. The viewer itself is still a
# target of that build tree (and would be sanitised too), so build `fuzz_all`, not `all`.

include_guard()

if (NOT LINUX)
  message(FATAL_ERROR "LL_FUZZ is Linux only (the Windows cross build is never fuzzed).")
endif ()

if (CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "LL_FUZZ does not support cross compiling.")
endif ()

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "LL_FUZZ needs clang (libFuzzer). Configure with the linux-fuzz preset, or "
                      "CC=clang CXX=clang++ and a fresh build directory. Found: ${CMAKE_CXX_COMPILER_ID}")
endif ()

# `alignment` is off at compile time (not just suppressed): the tree reads wire data through
# casts such as *(U32*)&buffer[1] and (U16*)&idx[0], which is benign on x86-64 and would stop
# every run on its first valid seed. `vptr` needs RTTI-consistent builds of the 3p libs.
# Everything else in UBSan aborts (no recover), so libFuzzer sees it as a crash and saves it.
set(LL_FUZZ_SAN_COMPILE
    -fsanitize=address,undefined
    -fno-sanitize=alignment,vptr
    -fno-sanitize-recover=undefined
    -fsanitize=fuzzer-no-link
    -fno-omit-frame-pointer)
set(LL_FUZZ_SAN_LINK -fsanitize=address,undefined)

add_compile_options(${LL_FUZZ_SAN_COMPILE})
add_link_options(${LL_FUZZ_SAN_LINK})

# Lets library code that must behave differently under the fuzzer test for it.
add_compile_definitions(LL_FUZZING=1)

message(STATUS "LL_FUZZ: ASan + UBSan + libFuzzer instrumentation on for the whole tree")
