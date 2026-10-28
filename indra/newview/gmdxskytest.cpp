/**
 * @file gmdxskytest.cpp
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

// <GMDX> SKY TEST: see gmdxskytest.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxskytest.h"

#include "llenvironment.h"
#include "llsettingsvo.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "rlvactions.h"

namespace
{
const S32 SKY_TEST_VERSION = -1012;   // our own ENV_LOCAL writes (Personal Lighting's are -2)

// The values the test sky is built from (each one starts the test when it changes).
const char* const VALUES[] = {
    "GMDXSkyTestSunElevation", "GMDXSkyTestSunAzimuth", "GMDXSkyTestMoonElevation", "GMDXSkyTestMoonAzimuth",
    "GMDXSkyTestMoonByPhase", "GMDXSkyTestMoonPhase", "GMDXSkyTestCloudCoverage", "GMDXSkyTestCloudDensity",
    "GMDXSkyTestCloudScale", "GMDXSkyTestCloudShiftX", "GMDXSkyTestCloudShiftY", "GMDXSkyTestFreezeClouds",
    "GMDXSkyTestHazeDensity" };

struct Preset { F32 sun, moon, phase, cover, density; };
// GMDXSkyTestPreset 1..9. Sun azimuth 0, the moon placed by its phase, cloud scale 0.42, no
// shift, frozen clouds, haze 0.7 (Second Life's defaults).
const Preset PRESETS[] = {
    { -60.f,  60.f,   0.f, 0.27f, 1.f },   // 1 full moon high (opposite the sun)
    { -12.f,  12.f,   0.f, 0.27f, 1.f },   // 2 full moon low
    { -30.f,  40.f,  90.f, 0.27f, 1.f },   // 3 quarter moon
    { -20.f,  15.f, 140.f, 0.27f, 1.f },   // 4 crescent, low near the set sun
    { -40.f, -30.f, 180.f, 0.f,   1.f },   // 5 moonless, clear
    {  -3.f,  25.f,  45.f, 0.27f, 1.f },   // 6 dusk
    {  -8.f,  25.f,  45.f, 0.27f, 1.f },   // 7 deep twilight
    {  70.f, -30.f,  90.f, 0.27f, 1.f },   // 8 noon
    { -60.f,  60.f,   0.f, 0.6f,  0.4f },  // 9 broken thin cloud over a high full moon (corona, silver edges)
};

LLSettingsSky::ptr_t sBase;      // the start sky (never changed)
LLSettingsSky::ptr_t sDay[3];    // base 5 (day cycle): Second Life's Midday, Sunset, Midnight
LLSettingsSky::ptr_t sOurs;      // the sky we put in ENV_LOCAL
bool sBaseInUse = false;         // sBase is the region's / parcel's sky (refreshed when it changes)
bool sBusy = false;              // we are writing our own settings
S32  sTicket = 0;                // the start-sky asset request still wanted

F32 getF(const char* name) { return gSavedSettings.getF32(name); }

void setQuiet(const char* name, const LLSD& value)
{
    sBusy = true;
    gSavedSettings.getControl(name)->setValue(value);
    sBusy = false;
}

// Personal Lighting's azimuth / elevation (degrees) to a sun or moon rotation.
// Base 5: Second Life's skies blended by the sun's height, as a day cycle moves through them
// (LLSettingsSky::blend, the day cycle's own blend): Midday from +20 degrees up, to Sunset by +8,
// Sunset down to +2, to Midnight by -9 and below.
const S32 BASE_DAY_CYCLE = 5;
LLSettingsSky::ptr_t dayCycleSky(F32 sun_el)
{
    LLSettingsSky::ptr_t sky = sDay[1]->buildClone();
    LLSettingsBase::ptr_t to = sun_el > 2.f ? sDay[0] : sDay[2];
    const F32 mix = sun_el > 2.f ? (sun_el - 8.f) / 12.f : (2.f - sun_el) / 11.f;
    if (sky && mix > 0.f)
    {
        sky->blend(to, llmin(mix, 1.f));
    }
    return sky;
}

LLQuaternion rotation(F32 azimuth, F32 elevation)
{
    elevation *= DEG_TO_RAD;
    if (is_approx_zero(elevation))
    {
        elevation = F_APPROXIMATELY_ZERO;
    }
    LLQuaternion quat, az_quat;
    quat.setAngleAxis(-elevation, 0, 1, 0);
    az_quat.setAngleAxis(F_TWO_PI - azimuth * DEG_TO_RAD, 0, 0, 1);
    return quat * az_quat;
}

void apply()
{
    if (!gSavedSettings.getBOOL("GMDXSkyTest") || !sBase || !RlvActions::canChangeEnvironment())
    {
        return;
    }
    const F32 sun_el = llclamp(getF("GMDXSkyTestSunElevation"), -90.f, 90.f);
    const bool day = gSavedSettings.getS32("GMDXSkyTestBase") == BASE_DAY_CYCLE && sDay[0] && sDay[1] && sDay[2];
    LLSettingsSky::ptr_t sky = day ? dayCycleSky(sun_el) : sBase->buildClone();
    if (!sky)
    {
        return;
    }

    const F32 sun_az = getF("GMDXSkyTestSunAzimuth");
    const F32 moon_el = llclamp(getF("GMDXSkyTestMoonElevation"), -90.f, 90.f);
    F32 moon_az = getF("GMDXSkyTestMoonAzimuth");
    if (gSavedSettings.getBOOL("GMDXSkyTestMoonByPhase"))
    {
        // The sun-moon angle t = 180 - phase, at the moon's own height:
        // cos t = sin(sun) sin(moon) + cos(sun) cos(moon) cos(azimuth difference).
        const F32 t = (180.f - llclamp(getF("GMDXSkyTestMoonPhase"), 0.f, 180.f)) * DEG_TO_RAD;
        const F32 s = sun_el * DEG_TO_RAD, m = moon_el * DEG_TO_RAD;
        const F32 c = (cosf(t) - sinf(s) * sinf(m)) / llmax(cosf(s) * cosf(m), 1e-4f);
        moon_az = fmodf(sun_az + acosf(llclamp(c, -1.f, 1.f)) * RAD_TO_DEG + 360.f, 360.f);
        setQuiet("GMDXSkyTestMoonAzimuth", moon_az);   // the floater shows where it went
    }
    sky->setSunRotation(rotation(sun_az, sun_el));
    sky->setMoonRotation(rotation(moon_az, moon_el));

    if (sky->getCloudNoiseTextureId().isNull())
    {
        sky->setCloudNoiseTextureId(LLSettingsSky::GetDefaultCloudNoiseTextureId());   // coverage needs a cloud map
    }
    sky->setCloudShadow(llclamp(getF("GMDXSkyTestCloudCoverage"), 0.f, 1.f));
    sky->setCloudPosDensity1(LLColor3(getF("GMDXSkyTestCloudShiftX"), getF("GMDXSkyTestCloudShiftY"),
                                      llclamp(getF("GMDXSkyTestCloudDensity"), 0.f, 3.f)));
    sky->setCloudScale(llclamp(getF("GMDXSkyTestCloudScale"), 0.01f, 3.f));
    if (gSavedSettings.getBOOL("GMDXSkyTestFreezeClouds"))
    {
        sky->setCloudScrollRate(LLVector2(0.f, 0.f));
    }
    sky->setHazeDensity(llclamp(getF("GMDXSkyTestHazeDensity"), 0.f, 5.f));
    sky->update();

    sOurs = sky;
    LLEnvironment& env = LLEnvironment::instance();
    env.setEnvironment(LLEnvironment::ENV_LOCAL, sky, SKY_TEST_VERSION);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
}

// Takes the start sky (GMDXSkyTestBase) and applies the test on it.
void captureBase()
{
    static const LLUUID* const KNOWN[] = { nullptr, &LLEnvironment::KNOWN_SKY_SUNRISE, &LLEnvironment::KNOWN_SKY_MIDDAY,
                                           &LLEnvironment::KNOWN_SKY_SUNSET, &LLEnvironment::KNOWN_SKY_MIDNIGHT };
    const S32 which = gSavedSettings.getS32("GMDXSkyTestBase");
    const S32 ticket = ++sTicket;
    if (which == BASE_DAY_CYCLE)
    {
        sBaseInUse = false;
        for (S32 i = 0; i < 3; ++i)
        {
            sDay[i].reset();
            LLSettingsVOBase::getSettingsAsset(*KNOWN[i + 2],
                [ticket, i](LLUUID, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
                {
                    if (ticket != sTicket || status || !settings || settings->getSettingsType() != "sky")
                    {
                        return;
                    }
                    sDay[i] = std::static_pointer_cast<LLSettingsSky>(settings);
                    if (sDay[0] && sDay[1] && sDay[2])
                    {
                        sBase = sDay[1];
                        apply();
                    }
                });
        }
        return;
    }
    if (which >= 1 && which <= 4)
    {
        sBaseInUse = false;
        LLSettingsVOBase::getSettingsAsset(*KNOWN[which],
            [ticket](LLUUID, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
            {
                if (ticket != sTicket || status || !settings || settings->getSettingsType() != "sky")
                {
                    return;
                }
                sBase = std::static_pointer_cast<LLSettingsSky>(settings);
                apply();
            });
        return;
    }
    // The sky in use: a personal sky someone else picked, else the parcel's / region's.
    LLEnvironment& env = LLEnvironment::instance();
    LLSettingsSky::ptr_t sky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
    sBaseInUse = !sky || sky == sOurs;
    if (sBaseInUse)
    {
        sky = env.getEnvironmentFixedSky(LLEnvironment::ENV_PARCEL, true);
    }
    sBase = sky ? sky->buildClone() : LLSettingsVOSky::buildDefaultSky();
    apply();
}

void onToggle()
{
    if (gSavedSettings.getBOOL("GMDXSkyTest"))
    {
        captureBase();
        return;
    }
    ++sTicket;
    sBase.reset();
    LLEnvironment& env = LLEnvironment::instance();
    if (sOurs && env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL) == sOurs)
    {   // (as World > Environment > Use Shared Environment)
        env.clearEnvironment(LLEnvironment::ENV_LOCAL);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }
    sOurs.reset();
}

void onValueChanged()
{
    if (sBusy)
    {
        return;
    }
    if (gSavedSettings.getBOOL("GMDXSkyTest"))
    {
        apply();
    }
    else
    {
        gSavedSettings.setBOOL("GMDXSkyTest", true);   // touching a control starts the test
    }
}

// keep_set: leave the values given on the command line (not at their defaults) alone.
void loadPreset(S32 n, bool keep_set)
{
    if (n < 1 || n > (S32)LL_ARRAY_SIZE(PRESETS))
    {
        return;
    }
    const Preset& p = PRESETS[n - 1];
    const std::pair<const char*, LLSD> values[] = {
        { "GMDXSkyTestSunElevation", p.sun }, { "GMDXSkyTestSunAzimuth", 0.f }, { "GMDXSkyTestMoonElevation", p.moon },
        { "GMDXSkyTestMoonByPhase", true }, { "GMDXSkyTestMoonPhase", p.phase }, { "GMDXSkyTestCloudCoverage", p.cover },
        { "GMDXSkyTestCloudDensity", p.density }, { "GMDXSkyTestCloudScale", 0.42f }, { "GMDXSkyTestCloudShiftX", 0.f },
        { "GMDXSkyTestCloudShiftY", 0.f }, { "GMDXSkyTestFreezeClouds", true }, { "GMDXSkyTestHazeDensity", 0.7f } };
    for (const auto& v : values)
    {
        LLControlVariable* c = gSavedSettings.getControl(v.first);
        if (!keep_set || c->isDefault())
        {
            setQuiet(v.first, v.second);
        }
    }
    if (!gSavedSettings.getBOOL("GMDXSkyTest"))
    {
        gSavedSettings.setBOOL("GMDXSkyTest", true);   // (onToggle takes the start sky)
    }
    else if (sBase)
    {
        apply();
    }
    else
    {
        captureBase();   // (--set GMDXSkyTest 1 with a preset: no start sky yet)
    }
}

void onEnvironmentChanged(LLEnvironment::EnvSelection_t which, S32 version)
{
    if (version == SKY_TEST_VERSION || !gSavedSettings.getBOOL("GMDXSkyTest"))
    {
        return;
    }
    if (which == LLEnvironment::ENV_LOCAL)
    {
        if (version == LLEnvironment::VERSION_CLEANUP)
        {   // the personal sky was dropped (Use Shared Environment, Personal Lighting's Reset): the test ends
            gSavedSettings.setBOOL("GMDXSkyTest", false);
            return;
        }
        // A personal sky someone else picked (World > Environment > Midnight, My Environments...):
        // the test goes on from it.
        setQuiet("GMDXSkyTestBase", 0);
        captureBase();
    }
    else if ((which == LLEnvironment::ENV_PARCEL || which == LLEnvironment::ENV_REGION) && sBaseInUse)
    {
        captureBase();   // the region's sky arrived or changed (login, teleport)
    }
}
} // namespace

void GMDXSkyTest::init()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    for (const char* name : VALUES)
    {
        gSavedSettings.getControl(name)->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { onValueChanged(); });
    }
    gSavedSettings.getControl("GMDXSkyTest")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { onToggle(); });
    gSavedSettings.getControl("GMDXSkyTestBase")->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
    {
        if (!sBusy && gSavedSettings.getBOOL("GMDXSkyTest"))
        {
            captureBase();
        }
    });
    gSavedSettings.getControl("GMDXSkyTestPreset")->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&)
    {
        if (!sBusy)
        {
            loadPreset(v.asInteger(), false);
        }
    });
    LLEnvironment::instance().setEnvironmentChanged(&onEnvironmentChanged);   // (lives as long as the viewer)

    // The floater's buttons.
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("GMDX.SkyTest.Preset", [](LLUICtrl*, const LLSD& param)
    {
        setQuiet("GMDXSkyTestPreset", param.asInteger());
        loadPreset(param.asInteger(), false);
    });
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("GMDX.SkyTest.Off", [](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setBOOL("GMDXSkyTest", false);
    });

    // --set GMDXSkyTest 1 / --set GMDXSkyTestPreset n on the command line.
    const S32 preset = gSavedSettings.getS32("GMDXSkyTestPreset");
    if (preset > 0)
    {
        loadPreset(preset, true);
    }
    else if (gSavedSettings.getBOOL("GMDXSkyTest"))
    {
        captureBase();
    }
}
