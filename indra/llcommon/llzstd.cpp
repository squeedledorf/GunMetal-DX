/**
 * @file llzstd.cpp
 * @brief The one translation unit that compiles the vendored Zstandard
 *        implementation (third_party/zstd, see its README.md). Third-party
 *        code: no precompiled header, warnings off (CMakeLists.txt).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

// zstd is portable C that also compiles as C++ (upstream `make cxxtest`);
// building it as C++ keeps the viewer free of C-language target setup on both
// toolchains (MSVC/clang-cl builds force /TP globally anyway). zstd.h wraps its
// API in extern "C", so the symbols match what llcachecodec.cpp declares.
//
// zstd marks its API visibility("default") on GCC/Clang. Empty these so the
// project-wide -fvisibility=hidden applies: our static copy must never be
// exported from the executable's dynamic symbol table, where it would
// interpose the system libzstd.so used by some other shared library.
#define ZSTDLIB_VISIBLE
#define ZSTDERRORLIB_VISIBLE
#define ZDICTLIB_VISIBLE
// Under ASan/MSan zstd declares __asan_poison_memory_region / __msan_unpoison
// itself, without extern "C"; compiled as C++ those references get mangled
// and fail to link (USE_ASAN builds). Skip its workspace poisoning instead:
// ordinary ASan instrumentation of zstd still applies.
#define ZSTD_ASAN_DONT_POISON_WORKSPACE 1
#define ZSTD_MSAN_DONT_POISON_WORKSPACE 1
#include "third_party/zstd/zstd.c"
