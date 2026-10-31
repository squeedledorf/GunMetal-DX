/**
 * @file gmdxfloaterskydebug.cpp
 * @brief GunMetal-DX Sky Debugger window (see gmdxfloaterskydebug.h).
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

// <GMDX> whole file: see gmdxfloaterskydebug.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxfloaterskydebug.h"

#include "gmdxskytune.h"
#include "slnightlight.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llslider.h"
#include "llsliderctrl.h"
#include "lltextbase.h"
#include "llviewercontrol.h"

#include <cmath>

namespace
{
// Every knob: its slider is "knob_" + the member's name (floater_gmdx_sky_debug.xml).
struct Knob { const char* name; GMDXKnob GMDXSkyTune::* knob; };
#define GMDX_KNOB(m) { #m, &GMDXSkyTune::m }
const Knob KNOBS[] = {
    GMDX_KNOB(night_cloud_light), GMDX_KNOB(moon_disc), GMDX_KNOB(night_ambient),
    GMDX_KNOB(night_max_lift), GMDX_KNOB(dusk_high), GMDX_KNOB(dusk_low),
    GMDX_KNOB(night_adapt), GMDX_KNOB(night_knee), GMDX_KNOB(night_deep_adapt), GMDX_KNOB(night_photo_adapt),
    GMDX_KNOB(night_photo_grade),
    GMDX_KNOB(corona), GMDX_KNOB(corona_droplet), GMDX_KNOB(halo22),
    GMDX_KNOB(star_twinkle), GMDX_KNOB(star_size), GMDX_KNOB(star_saturation), GMDX_KNOB(airglow), GMDX_KNOB(zodiacal),
    GMDX_KNOB(deep_sky), GMDX_KNOB(planets), GMDX_KNOB(satellites), GMDX_KNOB(meteors),
    GMDX_KNOB(sky_eep_air), GMDX_KNOB(sky_aerial),
    GMDX_KNOB(cloud_albedo), GMDX_KNOB(cloud_powder), GMDX_KNOB(cloud_sky_ambient), GMDX_KNOB(cloud_ground),
    GMDX_KNOB(cloud_erosion), GMDX_KNOB(cloud_curl), GMDX_KNOB(cloud_light_len), GMDX_KNOB(cloud_moon_g),
    GMDX_KNOB(cloud_moon_lobe), GMDX_KNOB(cloud_min_width), GMDX_KNOB(cloud_world), GMDX_KNOB(cloud_body_density),
    GMDX_KNOB(cloud_body_share), GMDX_KNOB(cloud_steps), GMDX_KNOB(cloud_light_steps), GMDX_KNOB(cloud_layer_margin),
};
#undef GMDX_KNOB

F32 elevationDeg(const LLVector3& d) { return RAD_TO_DEG * asinf(llclamp(d.mV[VZ] / llmax(d.length(), 1e-6f), -1.f, 1.f)); }
} // namespace

GMDXFloaterSkyDebug::GMDXFloaterSkyDebug(const LLSD& key)
:   LLFloater(key)
{
}

bool GMDXFloaterSkyDebug::postBuild()
{
    for (const Knob& k : KNOBS)
    {
        LLSliderCtrl* slider = getChild<LLSliderCtrl>(std::string("knob_") + k.name);
        mInitial.push_back(slider->getValueF32());   // (the constant, as the XUI has it)
        GMDXKnob GMDXSkyTune::* member = k.knob;
        slider->setCommitCallback([member](LLUICtrl* ctrl, const LLSD&)
        {
            GMDXKnob& knob = gmdxSkyTune().*member;
            knob.v = (F32)ctrl->getValue().asReal();
            knob.set = true;
        });
    }
    mView = getChild<LLComboBox>("cloud_view");
    mView->setValue(gmdxSkyTune().cloud_view);
    mView->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { gmdxSkyTune().cloud_view = ctrl->getValue().asInteger(); });
    mReadout = getChild<LLTextBase>("readout");
    getChild<LLButton>("reset_all")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        GMDXSkyTune& tune = gmdxSkyTune();
        for (size_t i = 0; i < LL_ARRAY_SIZE(KNOBS); ++i)
        {
            (tune.*KNOBS[i].knob).set = false;
            // (a knob whose code has not run yet: back to the XUI's constant)
            getChild<LLSliderCtrl>(std::string("knob_") + KNOBS[i].name)->setValue(mInitial[i]);
        }
        tune.cloud_view = 0;
        mView->setValue(0);
        followConstants();
    });
    followConstants();
    return true;
}

void GMDXFloaterSkyDebug::followConstants()
{
    const GMDXSkyTune& tune = gmdxSkyTune();
    for (const Knob& k : KNOBS)
    {
        const GMDXKnob& knob = tune.*k.knob;
        if (knob.set || !knob.seen) continue;   // (moved, or its code has not run yet)
        LLSliderCtrl* slider = getChild<LLSliderCtrl>(std::string("knob_") + k.name);
        if (!slider->hasFocus() && std::fabs(slider->getValueF32() - knob.def) > 1e-6f) slider->setValue(knob.def);
    }
}

void GMDXFloaterSkyDebug::draw()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now >= mNextReadout)
    {
        mNextReadout = now + 0.25;
        followConstants();
        updateReadout();
    }
    LLFloater::draw();
}

void GMDXFloaterSkyDebug::updateReadout()
{
    const GMDXSkyReadout& ro = gmdxSkyReadout();
    const SLNightLight& nl = slNightLight();
    std::string s;

    if (LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
    {
        const LLVector3 sun = sky->getSunDirection(), moon = sky->getMoonDirection();
        const LLColor3 sc = sky->getSunlightColor();
        s += llformat("Sun   (%+.2f %+.2f %+.2f) %+5.1f deg, light %.2f %.2f %.2f\n",
             sun.mV[VX], sun.mV[VY], sun.mV[VZ], elevationDeg(sun), sc.mV[0], sc.mV[1], sc.mV[2]);
        s += llformat("Moon  (%+.2f %+.2f %+.2f) %+5.1f deg, brightness %.2f\n",
             moon.mV[VX], moon.mV[VY], moon.mV[VZ], elevationDeg(moon), sky->getMoonBrightness());
        s += llformat("EEP clouds: coverage %.2f, scale %.2f\n", sky->getCloudShadow(), sky->getCloudScale());
    }
    s += llformat("Physical sky %s, physical night %s, volumetric clouds %s\n",
         gSavedSettings.getBOOL("RenderVKPhysicalSky") ? "on" : "off",
         gSavedSettings.getBOOL("RenderVKPhysicalNight") ? "on" : "off",
         gSavedSettings.getU32("RenderVKVolumetricClouds") ? "on" : "off");
    s += "\n";
    if (nl.on)
    {
        s += llformat("Night light: on%s\n", nl.creators_sky ? " (a creator's sky: Respect region skies is off)" : "");
        s += llformat("  moon %.4f of the sun (full %.4f), moon share %.2f, night band %.2f\n",
             nl.moon, nl.moon_full, nl.moon_share, nl.night);
        s += llformat("  scene light (%+.2f %+.2f %+.2f)%s, sky light q %+.3f, ambient x%.3f\n",
             nl.dir[0], nl.dir[1], nl.dir[2], nl.blended ? " blended" : "", nl.sky_q, nl.amb);
        s += llformat("  clouds' moonlight gain %.2f\n", 1.f + (gmdxSkyTune().night_cloud_light.value() - 1.f) * llclamp(nl.moon_share, 0.f, 1.f));
    }
    else
        s += llformat("Night light: off%s\n", nl.creators_sky ? " (a creator's sky: Second Life's night)" : "");
    s += llformat("Night exposure: lift at most %.2f stops this frame\n", ro.night_lift_cap);
    s += "\n";
    if (ro.clouds_live)
    {
        static const char* const patterns[] = {"all texels", "checkerboard", "one per 2x2"};
        s += llformat("Clouds (GunMetal-DX model): live, quality %d, %s, %d steps, %d light\n",
             ro.clouds_quality, patterns[llclamp(ro.clouds_pattern, 0, 2)], ro.clouds_steps, ro.clouds_light_steps);
        s += llformat("  layer %.0f - %.0f m, camera %s\n", ro.clouds_base, ro.clouds_top, ro.clouds_in_layer ? "IN the layer" : "outside");
        s += llformat("  tile %.0f m, base noise period %.0f m\n", ro.clouds_tile, ro.clouds_base_period);
        s += llformat("  temporal %s, camera medium %s, panorama %s\n", ro.clouds_temporal ? "on" : "off",
             ro.clouds_physical ? "on (raw radiance)" : "off", ro.clouds_pano_valid ? "valid" : "filling");
        s += llformat("  wind %.0f, %.0f m (past the region's own %.0f, %.0f m)\n",
             ro.clouds_wind[0], ro.clouds_wind[1], ro.clouds_wind_extra[0], ro.clouds_wind_extra[1]);
        if (ro.clouds_shadow_age >= 0.f)
            s += llformat("  shadow map light (%+.2f %+.2f %+.2f), refreshed %.0f frames ago\n",
                 ro.clouds_light[0], ro.clouds_light[1], ro.clouds_light[2], ro.clouds_shadow_age);
        else
            s += llformat("  cloud shadows off\n");
    }
    else
        s += llformat("Clouds: not live (the classic 2D layer)\n");
    mReadout->setText(s);
}
