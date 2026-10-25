/**
 * @file llgltfexporter.h
 * @brief Export the selected linkset(s) to glTF 2.0 (#908). Full-permission
 *        content only; see llgltfexporter.cpp for exactly what is verified.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#pragma once

#include <string>

class LLGLTFExporter
{
public:
    // Menu enable: a non-empty selection of prims whose roots you own, no avatars,
    // nothing attached to another avatar. Cheap; the full check runs at export time.
    static bool canExportSelection();

    // Menu action: snapshot the current selection and start an export job
    // (properties -> materials -> inventory -> permission check -> file picker ->
    // asset fetch -> write). Only one job runs at a time.
    static void exportSelection();

    // Progress floater interface.
    static bool isActive();
    static void cancel();
    static std::string getStatusText();
    static float getProgress(); // 0..1
};
