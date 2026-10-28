/**
 * @file gmdxskytest.h
 * @brief GunMetal-DX Sky Test: a local test sky for tuning the night look.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

#ifndef GMDX_SKYTEST_H
#define GMDX_SKYTEST_H

// <GMDX> SKY TEST (World > Environment > Sky Test..., floater gmdx_sky_test).
//
// While GMDXSkyTest is on, the personal (ENV_LOCAL) sky is a copy of a start sky (GMDXSkyTestBase:
// by default Second Life's Midday / Sunset / Midnight blended by the test sun's height, else the
// sky in use or one of Second Life's four) with the sun, moon, clouds and haze from the
// GMDXSkyTest* settings, the way Personal Lighting does it: local only, never sent anywhere.
// The floater's controls are those settings; changing any of them starts the test. A preset
// (GMDXSkyTestPreset) fills them in. Off, or "Use Shared Environment", drops the personal sky.
// RLVa @setenv: nothing is applied (LLEnvironment refuses it too) and the floater is closed.
namespace GMDXSkyTest
{
    // After login: hooks the settings and the environment; applies --set GMDXSkyTest 1 / a preset.
    void init();
}

#endif // GMDX_SKYTEST_H
