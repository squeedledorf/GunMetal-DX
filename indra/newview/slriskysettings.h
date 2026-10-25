/**
 * @file slriskysettings.h
 * @brief Crash-loop guard for risky render settings (#736).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_RISKYSETTINGS_H
#define SL_RISKYSETTINGS_H

// CRASH-LOOP GUARD. A few render settings switch on GPU paths that a driver can fail on
// (ray-traced reflections, ray-traced sun / projector shadows). When
// one is RAISED into its risky range, the change goes on probation: it is recorded in
// logs/<instance>.risky_settings (name, previous value, new value) until it has survived
// kProbationSecs of in-world time, across sessions if need be. If a session ends without
// a clean exit while a change is on probation and that session reached the world, the
// next launch puts the previous value back before the renderer reads it, saves the
// settings, logs it and shows a one-time notice. Lowering a setting out of the risky
// range (by the user, a preset, the VRAM ladder) ends its probation. Second instances
// do not take part (they share the settings file with the first).
namespace SLRiskySettings
{
    // Once, after the settings are loaded and before the renderer is created (Vulkan
    // bring-up). Reverts a crashed probation, then arms this session's listeners.
    void startup(bool second_instance);
    // Every frame, main thread: the probation clock (in-world time only).
    void tick(float dt_secs, bool in_world);
    // Clean exit: this session did not crash (probation entries are kept).
    void cleanExit();
}

#endif // SL_RISKYSETTINGS_H
