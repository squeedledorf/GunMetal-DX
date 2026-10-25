/**
 * @file slfloaterweather.cpp
 * @brief Weather floater (#783): see slfloaterweather.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slfloaterweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloaterweather.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llspinctrl.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "lltextbox.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "roles_constants.h"
#include "lldbstrings.h"
#include "lldate.h"
#include "llviewercontrol.h"
#include "slweather.h"
#include "slweatherregion.h"

#include <cmath>

namespace
{
constexpr F32 STATUS_REFRESH_SECONDS = 0.25f;
// #954 PRESET LADDER (the classes: light / moderate / heavy / violent): rain mm/h, snow cm/h; the
// blizzard also raises the wind to at least kBlizzardWind m/s.
constexpr F32 kRainPresets[4] = {1.f, 5.f, 20.f, 80.f};
constexpr F32 kSnowPresets[4] = {0.3f, 1.5f, 5.f, 12.f};
constexpr F32 kBlizzardWind = 15.f;

std::string percent(F32 v)
{
    return llformat("%d", (S32)ll_round(llclamp(v, 0.f, 1.f) * 100.f));
}

// WeatherParcelSnowSince: whole unix seconds, or empty / invalid (false).
bool parseSince(const std::string& v, S64& out)
{
    std::string s(v);
    LLStringUtil::trim(s);
    if (s.empty()) return false;
    for (char ch : s)
        if (ch < '0' || ch > '9') return false;
    if (s.size() > 12) return false;
    out = std::stoll(s);
    return out >= 0 && out <= SLWeatherRegion::kMaxSince;
}

std::string agoText(S64 secs)
{
    if (secs < 120) return llformat("%lld s", (long long)secs);
    if (secs < 2 * 3600) return llformat("%lld min", (long long)(secs / 60));
    if (secs < 2 * 86400) return llformat("%.1f h", (double)secs / 3600.0);
    return llformat("%.1f days", (double)secs / 86400.0);
}
} // namespace

SLFloaterWeather::SLFloaterWeather(const LLSD& key)
:   LLFloater(key)
{
}

bool SLFloaterWeather::postBuild()
{
    mStatus = getChild<LLTextBox>("status");
    mRegionStatus = getChild<LLTextBox>("region_status");
    mExportBtn = getChild<LLButton>("export_parcel");
    mClearBtn = getChild<LLButton>("clear_parcel");
    mExportBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExport(false); });
    mClearBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExport(true); });
    mSnowSince = getChild<LLTextBox>("parcel_snow_since");
    // #783 SHARED SNOW DEPTH: "Stamp now" -- the depth field is the parcel's depth at this UTC second.
    getChild<LLButton>("parcel_snow_stamp")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        gSavedSettings.setString("WeatherParcelSnowSince", llformat("%lld", (long long)std::floor(SLWeather::syncedSeconds())));
        refreshSnowSince();
    });
    // #954 PRECIPITATION RATE: the spinner and the preset ladder write the type's rate; the
    // intensity slider hands the rate back to itself (the type's rate setting to -1).
    mFallSpin = getChild<LLSpinCtrl>("weather_fall");
    mPreset = getChild<LLComboBox>("weather_preset");
    mFallUnit = getChild<LLTextBox>("fall_unit");
    mFallSpin->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFallCommit(); });
    mPreset->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetCommit(); });
    getChild<LLUICtrl>("weather_intensity")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        const bool snow = gSavedSettings.getU32("WeatherType") == SLWeather::PRECIP_SNOW;
        gSavedSettings.setF32(snow ? "WeatherSnowFall" : "WeatherRainFall", -1.f);
    });
    refreshFall();
    refreshStatus();
    mStatusTimer.start();
    return true;
}

// static
F32 SLFloaterWeather::localFall(SLWeather::EPrecip type)
{
    const bool snow = type == SLWeather::PRECIP_SNOW;
    const F32 v = gSavedSettings.getF32(snow ? "WeatherSnowFall" : "WeatherRainFall");
    const F32 fall = v >= 0.f ? v : SLWeather::fallFromAmount(type, llclamp(gSavedSettings.getF32("WeatherIntensity"), 0.f, 1.f));
    return llclamp(fall, 0.f, snow ? SLWeather::kSnowFallMax : SLWeather::kRainFallMax);
}

void SLFloaterWeather::refreshFall()
{
    if (!mFallSpin || !mPreset) return;
    const bool snow = gSavedSettings.getU32("WeatherType") == SLWeather::PRECIP_SNOW;
    const SLWeather::EPrecip type = snow ? SLWeather::PRECIP_SNOW : SLWeather::PRECIP_RAIN;
    if (mPresetType != (S32)type)
    {
        mPresetType = (S32)type;
        mPreset->removeall();
        for (S32 k = 0; k < 4; ++k) mPreset->add(getString(llformat(snow ? "snow_class_%d" : "rain_class_%d", k)), LLSD(k));
        mFallSpin->setMaxValue(snow ? SLWeather::kSnowFallMax : SLWeather::kRainFallMax);
        mFallSpin->setIncrement(snow ? 0.1f : 0.5f);
        mFallSpin->setPrecision(snow ? 2 : 1);
        if (mFallUnit) mFallUnit->setText(getString(snow ? "unit_snow" : "unit_rain"));
    }
    const F32 fall = localFall(type);
    if (!mFallSpin->hasFocus()) mFallSpin->setValue(LLSD((F64)fall));
    // (only on a change: an open list is never re-selected under the cursor)
    const S32 cls = (S32)SLWeather::fallClass(type, fall);
    if (!mPreset->hasFocus() && mPreset->getCurrentIndex() != cls) mPreset->setCurrentByIndex(cls);
}

void SLFloaterWeather::onFallCommit()
{
    if (!mFallSpin) return;
    const bool snow = gSavedSettings.getU32("WeatherType") == SLWeather::PRECIP_SNOW;
    const F32 top = snow ? SLWeather::kSnowFallMax : SLWeather::kRainFallMax;
    gSavedSettings.setF32(snow ? "WeatherSnowFall" : "WeatherRainFall", llclamp((F32)mFallSpin->getValue().asReal(), 0.f, top));
}

void SLFloaterWeather::onPresetCommit()
{
    if (!mPreset) return;
    const S32 k = mPreset->getCurrentIndex();
    if (k < 0 || k > 3) return;
    const bool snow = gSavedSettings.getU32("WeatherType") == SLWeather::PRECIP_SNOW;
    gSavedSettings.setF32(snow ? "WeatherSnowFall" : "WeatherRainFall", snow ? kSnowPresets[k] : kRainPresets[k]);
    // (a blizzard is snow driven by a strong wind)
    if (snow && k == 3 && gSavedSettings.getF32("WeatherWindSpeed") < kBlizzardWind)
        gSavedSettings.setF32("WeatherWindSpeed", kBlizzardWind);
    refreshFall();
}

void SLFloaterWeather::draw()
{
    if (mStatusTimer.getElapsedTimeF32() >= STATUS_REFRESH_SECONDS)
    {
        refreshStatus();
        mStatusTimer.reset();
    }
    LLFloater::draw();
}

// The same check About Land uses for its Description field (llfloaterland.cpp, can_edit_identity),
// on the parcel the agent stands on.
LLParcel* SLFloaterWeather::editableParcel()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (!parcel || !gAgent.getRegion()) return nullptr;
    return LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_IDENTITY) ? parcel : nullptr;
}

void SLFloaterWeather::refreshExport()
{
    if (!mExportBtn || !mClearBtn) return;
    const bool can_edit = editableParcel() != nullptr;
    const bool local_on = SLWeather::instance().localSettings().active && SLWeather::masterEnabled();
    mExportBtn->setEnabled(can_edit && local_on);
    mClearBtn->setEnabled(can_edit && SLWeather::masterEnabled());
    mExportBtn->setToolTip(getString(!can_edit ? "export_tip_no_rights" : (!local_on ? "export_tip_local_off" : "export_tip")));
    mClearBtn->setToolTip(getString(can_edit ? "clear_tip" : "export_tip_no_rights"));
}

// #783 SHARED SNOW DEPTH: the floater's depth fields into the exported target, validated (the parser
// clamps silently; here an out-of-range value is refused, so the owner sees what is written).
bool SLFloaterWeather::addSnowDepth(SLWeather::Target& t, std::string& why, std::string& stamp)
{
    if (!gSavedSettings.getBOOL("WeatherParcelSnowShare")) return true;   // (no depth: each viewer's own build-up)
    const F32 depth = gSavedSettings.getF32("WeatherParcelSnowDepth");
    // #954: by default the rate the local snowfall builds at (cm/h x (1 - settling) / 100 m/h), written
    // explicitly so viewers older than #954 agree on the level.
    const bool fromFall = gSavedSettings.getBOOL("WeatherParcelSnowRateFromFall");
    const F32 rate = fromFall ? std::round(t.snowFall * (1.f - SLWeather::kSnowSettle) / 100.f * 1000.f) / 1000.f
                              : gSavedSettings.getF32("WeatherParcelSnowRate");
    const F32 cap = llclamp(gSavedSettings.getF32("WeatherSnowMaxDepth"), 0.f, SLWeather::kSnowDepthMax);
    if (!std::isfinite(depth) || depth < 0.f || depth > cap + 0.005f)
    {
        why = llformat("the depth %.2f m is outside 0 .. %.2f m (your Snow max depth)", depth, cap);
        return false;
    }
    if (!std::isfinite(rate) || std::fabs(rate) > SLWeatherRegion::kMaxSnowRate)
    {
        why = llformat("the rate %.3f m/h is outside -%.0f .. %.0f m/h", rate, SLWeatherRegion::kMaxSnowRate, SLWeatherRegion::kMaxSnowRate);
        return false;
    }
    S64 since = 0;
    const std::string sinceStr = gSavedSettings.getString("WeatherParcelSnowSince");
    if (!parseSince(sinceStr, since))
    {
        std::string trimmed(sinceStr);
        LLStringUtil::trim(trimmed);
        if (!trimmed.empty())
        {
            why = "the time '" + trimmed + "' is not a unix time (use Stamp now)";
            return false;
        }
        // (not stamped: the depth applies from now -- kept once confirmed, so a re-export keeps it)
        since = (S64)std::floor(SLWeather::syncedSeconds());
        stamp = llformat("%lld", (long long)since);
    }
    t.haveDepth = true;
    t.snowDepthAt = depth;
    t.haveRate = true;
    t.snowRate = rate;
    t.haveSince = true;
    t.snowSince = (F64)since;
    return true;
}

void SLFloaterWeather::refreshSnowSince()
{
    if (!mSnowSince) return;
    S64 since = 0;
    if (!parseSince(gSavedSettings.getString("WeatherParcelSnowSince"), since))
    {
        mSnowSince->setText(getString("since_none"));
        return;
    }
    const S64 now = (S64)std::floor(SLWeather::syncedSeconds());
    LLStringUtil::format_map_t args;
    args["[TIME]"] = LLDate((F64)since).toHTTPDateString(std::string("%Y-%m-%d %H:%M"));
    args["[AGO]"] = agoText(now - since);
    mSnowSince->setText(getString(since > now ? "since_ahead" : "since_at", args));
}

void SLFloaterWeather::onExport(bool clear)
{
    LLParcel* parcel = editableParcel();
    if (!parcel) return;   // (the button is disabled then; the parcel can change under the click)

    const SLWeather::Target local = SLWeather::instance().localSettings();
    std::string tag, stamp;
    if (!clear)
    {
        if (!local.active) return;
        SLWeather::Target out = local;
        std::string why;
        if (!addSnowDepth(out, why, stamp))
        {
            LLSD args;
            args["REASON"] = why;
            LLNotificationsUtil::add("WeatherParcelSnowInvalid", args);
            return;
        }
        tag = SLWeatherRegion::formatTag(out);
        if (!SLWeatherRegion::tagRoundTrips(tag, out))
        {
            LL_WARNS("Weather") << "parcel weather tag does not read back: " << tag << LL_ENDL;
            LLNotificationsUtil::add("WeatherParcelTagInvalid");
            return;
        }
    }
    std::string new_desc;
    if (!SLWeatherRegion::editDescription(parcel->getDesc(), tag, new_desc))
    {
        LLNotificationsUtil::add("WeatherParcelNoTag");
        return;
    }
    if (new_desc.size() > (size_t)DB_PARCEL_DESC_LEN)
    {
        LLSD args;
        args["TAG"] = tag;
        args["LENGTH"] = LLSD::Integer(new_desc.size());
        args["MAX"] = LLSD::Integer(DB_PARCEL_DESC_LEN);
        LLNotificationsUtil::add("WeatherParcelTooLong", args);
        return;
    }
    LLSD args;
    args["TAG"] = tag;
    args["PARCEL"] = parcel->getName();
    LLNotificationsUtil::add(clear ? "WeatherParcelClearConfirm" : "WeatherParcelExportConfirm", args, LLSD(),
                             [clear, new_desc, id = parcel->getLocalID(), stamp](const LLSD& n, const LLSD& r)
                             { onExportConfirmed(n, r, clear, new_desc, id, stamp); });
}

void SLFloaterWeather::onExportConfirmed(const LLSD& notification, const LLSD& response, bool clear, const std::string& new_desc, S32 parcel_id,
                                         const std::string& stamp)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
    // Re-check: the agent may have moved on, or lost the rights, while the dialog was up.
    LLParcel* parcel = editableParcel();
    if (!parcel || parcel->getLocalID() != parcel_id) return;
    if (parcel->getRegionID().isNull()) parcel->setRegionID(gAgent.getRegion()->getRegionID());
    parcel->setDesc(new_desc);   // (the region source sees it at its next poll; the server re-broadcasts it)
    if (!stamp.empty()) gSavedSettings.setString("WeatherParcelSnowSince", stamp);   // (#783: the time it was written with)
    LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate(parcel);
    LL_INFOS("Weather") << (clear ? "cleared" : "set") << " the weather tag of parcel " << parcel_id << LL_ENDL;
}

// The live (eased) state, not the settings: the user sees the transition happen and the
// surfaces dry after the rain stops.
void SLFloaterWeather::refreshStatus()
{
    // MASTER SWITCH: off disables every other control (their values are kept) and says why.
    const bool master = SLWeather::masterEnabled();
    for (LLView* child : *getChildList())
    {
        LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(child);
        if (!ctrl) continue;
        const std::string& n = ctrl->getName();
        if (n == "weather_master" || n == "master_status" || n == "status" || n == "region_status") continue;
        ctrl->setEnabled(master);
    }
    if (LLTextBox* ms = findChild<LLTextBox>("master_status"))
        ms->setText(master ? std::string() : getString("master_off"));
    refreshExport();
    refreshFall();
    // #783 SHARED SNOW DEPTH: the depth fields only while the depth is shared.
    {
        const bool share = master && gSavedSettings.getBOOL("WeatherParcelSnowShare");
        for (const char* n : {"parcel_snow_depth", "parcel_snow_rate", "parcel_snow_stamp", "parcel_snow_rate_fall"})
            if (LLUICtrl* c = findChild<LLUICtrl>(n)) c->setEnabled(share);
        // (#954: the rate field only when it is written as it is)
        if (LLUICtrl* c = findChild<LLUICtrl>("parcel_snow_rate"))
            c->setEnabled(share && !gSavedSettings.getBOOL("WeatherParcelSnowRateFromFall"));
        refreshSnowSince();
    }
    if (mRegionStatus)
    {
        std::string key;
        switch (SLWeatherRegion::status())
        {
        case SLWeatherRegion::STATUS_OFF:           key = "region_off"; break;
        case SLWeatherRegion::STATUS_NONE:          key = "region_none"; break;
        case SLWeatherRegion::STATUS_PARCEL:        key = "region_parcel"; break;
        case SLWeatherRegion::STATUS_PARCEL_ERRORS: key = "region_parcel_errors"; break;
        case SLWeatherRegion::STATUS_SKY_RAIN:      key = "region_sky_rain"; break;
        case SLWeatherRegion::STATUS_SKY_DRY:       key = "region_sky_dry"; break;
        }
        std::string text = getString(key);
        const SLWeatherRegion::EStatus rs = SLWeatherRegion::status();
        if ((rs == SLWeatherRegion::STATUS_PARCEL || rs == SLWeatherRegion::STATUS_PARCEL_ERRORS
             || rs == SLWeatherRegion::STATUS_SKY_RAIN)
            && SLWeather::instance().state().source == SLWeather::SOURCE_LOCAL)
        {
            LLStringUtil::format_map_t args;
            args["[REGION]"] = text;
            text = getString("region_overridden", args);
        }
        mRegionStatus->setText(text);
    }
    if (!mStatus) return;
    const SLWeather::State& st = SLWeather::instance().state();
    if (!master && !st.visible()) { mStatus->setText(getString("status_off")); return; }
    if (!st.visible())
    {
        // (visible() counts lying snow too: "off" only once it has melted and dried)
        mStatus->setText(getString("status_off"));
        return;
    }
    LLStringUtil::format_map_t args;
    // #954: the rates falling now (the bands included) and their class.
    std::string precip;
    for (U32 k = 0; k < 2; ++k)
    {
        const bool snow = k == 1;
        const F32 fall = snow ? st.snowFall : st.rainFall;
        if ((snow ? st.snow : st.rain) <= SLWeather::kEpsilon) continue;
        const SLWeather::EPrecip type = snow ? SLWeather::PRECIP_SNOW : SLWeather::PRECIP_RAIN;
        LLStringUtil::format_map_t pa;
        pa["[RATE]"] = llformat(snow ? (fall < 10.f ? "%.2f" : "%.1f") : (fall < 10.f ? "%.1f" : "%.0f"), fall);
        pa["[CLASS]"] = getString(llformat(snow ? "snow_class_%u" : "rain_class_%u", SLWeather::fallClass(type, fall)));
        precip += (precip.empty() ? "" : ", ") + getString(snow ? "status_snow" : "status_rain", pa);
    }
    if (precip.empty()) precip = getString("status_dry");
    if (st.blowing > 0.05f) precip += getString("blowing");
    args["[PRECIP]"] = precip;
    args["[WIND]"] = llformat("%.1f", std::sqrt(st.windMean[0] * st.windMean[0] + st.windMean[1] * st.windMean[1]));
    args["[GUST]"] = llformat("%.1f", std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]));
    args["[WET]"] = percent(st.wetness);
    args["[COVER]"] = percent(st.snowCover);
    // (one decimal below 10 cm: a slow build-up is visible from its first millimetres)
    const F32 depthCm = st.snowDepth * 100.f;
    args["[DEPTH]"] = depthCm < 10.f ? llformat("%.1f", depthCm) : llformat("%.0f", depthCm);
    args["[SHARED]"] = st.snowDepthShared ? getString("depth_shared") : std::string();
    mStatus->setText(getString("status_on", args));
}
