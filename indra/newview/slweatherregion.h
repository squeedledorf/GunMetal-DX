/**
 * @file slweatherregion.h
 * @brief Weather (#783 M2): the REGION source -- weather a parcel publishes, or rain derived
 *        from the region's shared sky -- fed to SLWeather::setRegionTarget() only with the
 *        user's opt-in.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 *
 * PARCEL TAG (WeatherAllowRegion, default off; doc/weather_region_protocol.md): the agent's
 * parcel description may carry one tag, e.g.
 *     [slwx rain=0.7 wind=225@8 var=0.5 gust=0.6 storm=0.4 seed=0x5eed]
 * #783 SHARED SNOW DEPTH: depth= (m) rate= (m/h) since= (unix s) make the lying snow's level the
 * same for everyone on the parcel (SLWeather::snowLevelAt; slweather.h SHARED SNOW DEPTH).
 * Like the rest of the tag it applies while the agent stands on the parcel -- to the whole
 * weather the viewer shows (one global level, no parcel bounds in the snow grids).
 * Only the parcel's owner (or its managers) can edit the description, every viewer on the
 * parcel already receives it (ParcelProperties, no extra request), and nothing is ever sent to
 * the server. The seed (default: from the region id and the parcel's local id) makes every
 * viewer on the parcel see the same gusts and lightning at the same moment.
 *
 * SKY (WeatherFromEnvironment, default off): with no parcel tag, heavy EEP cloud coverage
 * (the shared parcel / region sky's cloud_shadow) gives a chance of rain per 10-minute window
 * of synced UTC time, decided by a hash of the region id: every viewer in the region agrees.
 * It only ever adds rain to what the sky already shows; the region's sky is never changed.
 *
 * The local weather (Weather floater) still wins over both while it is on.
 *
 * #895: only the parcel the agent is on counts -- the agent parcel of the previous region (it
 * lingers through a teleport / crossing until the new region's ParcelProperties arrive) is
 * "not known yet", never a source; through a border CROSSING the previous region's feed is held
 * meanwhile (at most kCrossHoldSecs), so a snowfall that continues on the other side has no dip.
 * A weather reset or a new place (SLWeather::resetSerial / placeSerial: master off, teleport)
 * drops the cached tag and re-polls at once. Each evaluated place is reported to
 * SLWeather::noteSourceSettled: a crossing / parcel change / teleport is judged against it.
 */

#ifndef SL_WEATHERREGION_H
#define SL_WEATHERREGION_H

#include "slweather.h"

#include <string>

class SLWeatherRegion
{
public:
    enum EStatus : U32
    {
        STATUS_OFF = 0,        // neither opt-in is on
        STATUS_NONE,           // opted in, but this parcel publishes nothing (and no sky rain)
        STATUS_PARCEL,         // the parcel's tag is in use
        STATUS_PARCEL_ERRORS,  // the parcel's tag is in use, parts of it were not understood
        STATUS_SKY_RAIN,       // rain from the region's sky
        STATUS_SKY_DRY,        // the sky is watched; no rain in this window
    };

    // Main thread, once per frame (idle). Polls ~1 Hz.
    static void update();
    static EStatus status();

    // A description's tag -> a target (doc/weather_region_protocol.md). false: no tag.
    // `errors`: the tokens that were not understood (the rest still applies).
    static bool parseTag(const std::string& text, U32 default_seed, SLWeather::Target& out, std::string* errors = nullptr);

    // The inverse of parseTag (the Weather floater's "Set as parcel weather"): only keys that
    // differ from the parser's defaults, numbers rounded (and compact) as the parser reads them
    // back. An inactive target gives "[slwx off]". No seed (the parcel's shared default holds).
    static std::string formatTag(const SLWeather::Target& t);
    // The description with its tag replaced in place (same find logic as parseTag), or the tag
    // appended after a space. An empty `tag` removes the tag instead (false: there was none).
    // The rest of the text is untouched. The caller checks the length.
    static bool editDescription(const std::string& desc, const std::string& tag, std::string& out);
    // parseTag(tag) reads back `want` (within the rounding formatTag applies), with no errors.
    static bool tagRoundTrips(const std::string& tag, const SLWeather::Target& want);

    // #783 SHARED SNOW DEPTH: the tag's rate= range (m/h; the WeatherSnowDepthPerHour slider's top)
    // and the largest since= accepted (unix seconds, year 2100).
    static constexpr F32 kMaxSnowRate = 100.f;
    static constexpr S64 kMaxSince = 4102444800LL;
};

#endif
