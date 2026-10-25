/**
 * @file slfloaterweather.h
 * @brief Weather floater (#783): your own local weather -- on / off, rain or snow,
 *        intensity, wind (gusts), thunderstorm, transition speed -- and the region / parcel
 *        weather opt-ins with their status. Every control is a setting (Weather*,
 *        floater_sl_weather.xml); the floater only adds the live status line.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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

#ifndef SL_FLOATERWEATHER_H
#define SL_FLOATERWEATHER_H

#include "llfloater.h"
#include "llframetimer.h"
#include "slweather.h"

class LLButton;
class LLComboBox;
class LLParcel;
class LLSpinCtrl;
class LLTextBox;

class SLFloaterWeather : public LLFloater
{
    friend class LLFloaterReg;
public:
    bool postBuild() override;
    void draw() override;

private:
    SLFloaterWeather(const LLSD& key);
    ~SLFloaterWeather() override = default;

    void refreshStatus();
    // "Set as parcel weather" / "Clear parcel weather" (#783): edit the [slwx ...] tag in the
    // description of the parcel the agent stands on, after a confirmation.
    void refreshExport();
    void onExport(bool clear);
    static void onExportConfirmed(const LLSD& notification, const LLSD& response, bool clear, const std::string& new_desc, S32 parcel_id,
                                  const std::string& stamp);
    // The agent's parcel, if the agent may edit its description (About Land's check), else null.
    static LLParcel* editableParcel();
    // #783 SHARED SNOW DEPTH: the depth / rate / since fields (WeatherParcelSnow*) added to the
    // exported target. false: a field is out of range (`why` says which; nothing is written).
    // `stamp`: an unstamped since= taken as now (kept in WeatherParcelSnowSince once confirmed).
    static bool addSnowDepth(SLWeather::Target& t, std::string& why, std::string& stamp);
    void refreshSnowSince();
    // #954 PRECIPITATION RATE: the rate spinner / preset ladder of the selected type (labels, unit,
    // range and the class shown follow WeatherType; a field being edited is left alone).
    void refreshFall();
    void onFallCommit();
    void onPresetCommit();
    // The local rate of a type (its WeatherRainFall / WeatherSnowFall, or -1 there: the intensity's).
    static F32 localFall(SLWeather::EPrecip type);

    LLTextBox*   mStatus = nullptr;
    LLTextBox*   mRegionStatus = nullptr;
    LLButton*    mExportBtn = nullptr;
    LLButton*    mClearBtn = nullptr;
    LLTextBox*   mSnowSince = nullptr;
    LLSpinCtrl*  mFallSpin = nullptr;
    LLComboBox*  mPreset = nullptr;
    LLTextBox*   mFallUnit = nullptr;
    S32          mPresetType = -1;          // the type the preset ladder's labels are for
    LLFrameTimer mStatusTimer;
};

#endif // SL_FLOATERWEATHER_H
