/**
 * @file llwuffs.h
 * @brief The vendored Wuffs library (third_party/wuffs), header part, limited to
 *        the modules the viewer uses (GIF + LZW). Include this, never the .c file
 *        directly, so every user sees the same module set.
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

#ifndef LL_LLWUFFS_H
#define LL_LLWUFFS_H

#ifndef WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW
#endif

#include "third_party/wuffs/wuffs-v0.3.c"

#endif // LL_LLWUFFS_H
