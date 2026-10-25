/**
 * @file llwuffs.cpp
 * @brief The one translation unit that compiles the vendored Wuffs
 *        implementation (third_party/wuffs, see its README). Third-party code:
 *        no viewer precompiled header, warnings off (CMakeLists.txt).
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

#define WUFFS_IMPLEMENTATION
#include "llwuffs.h"
