/**
 * @file llufbx.cpp
 * @brief The one translation unit that compiles the vendored ufbx
 *        implementation (third_party/ufbx, see its README.md). Third-party
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

// ufbx is portable C that also compiles as C++ (upstream README); building it
// as C++ keeps the viewer free of C-language target setup on both toolchains.
#include "third_party/ufbx/ufbx.c"
